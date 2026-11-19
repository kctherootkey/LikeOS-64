// LikeOS -- LikeOS Image Viewer: the Background tool.
//
// Finding what is behind the subject and blurring, removing or replacing
// it.  The background is a mask in the edit state (IvBackgroundEdit,
// SOURCE coordinates at MASK resolution, 255 = background) and an
// operation applied through it by the render pipeline (iv_bg_apply), so it
// is rendered again at full size when saving.
//
//   Detect background   ivproc's estimation (iv_bg_estimate) on a copy of
//                       the proxy in a worker, under a progress toast that
//                       can be cancelled; the result is carried up to MASK
//                       resolution.  It reports whether the background
//                       was clear; when it was not, the weak guess (if
//                       any) is kept and the refine brush is offered.
//   The three actions   large cards with live thumbnails of the current
//                       picture, each rendered with its action through the
//                       pipeline in a worker: Blur (with its strength),
//                       Remove (transparent, over the checkerboard) and
//                       Replace with a colour (swatches and a chooser).
//                       Pressing the chosen card again takes the action
//                       off and leaves the selection.
//   Refine brush        paints the selection by hand: Add takes area into
//                       the background, Erase gives it back to the
//                       subject; size and edge softness are the brush's,
//                       "Soften edges" feathers the whole selection.
//   Reset background    no selection, no action.
//
// The selection is shown over the picture as diagonal accent hatching
// whenever it is being refined, and when it exists without an action; the
// canvas shows the picture without the background action while refining
// (IV_RENDER_NO_BACKGROUND), so painting needs no render at all.  Every
// change is one undo step.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tool-brush.h"

#define REFINE_MIN 5.0
#define REFINE_MAX 400.0
#define HATCH_ACCENT 0x5b8ff9a0u
#define THUMB_EDGE 200
#define THUMB_W 104
#define THUMB_H 70

typedef struct {
	IvBgOp op;
	const char *title;
	const char *text;
	const char *step;	/* the undo label */
} Action;

static const Action actions[] = {
	{ IV_BG_BLUR, "Blur", "Soften what is behind the subject.",
	  "Blur background" },
	{ IV_BG_REMOVE, "Remove", "Make it transparent.", "Remove background" },
	{ IV_BG_REPLACE, "Replace", "Fill it with a plain colour.",
	  "Replace background" },
};
#define N_ACTIONS ((int)G_N_ELEMENTS(actions))

/* Backdrops to put behind the subject: paper and ink, the LikeOS blues,
 * and soft studio tones. */
static const guint32 backdrops[] = {
	0xffffffff, /* white */
	0xeef2f9ff, /* mist */
	0x141a24ff, /* ink */
	0x2a5bd7ff, /* LikeOS blue */
	0x9cc3ffff, /* sky */
	0x1f9d55ff, /* green */
	0xf2d7b6ff, /* sand */
	0xf6c1d6ff, /* rose */
};

static IvEditor *editor;

/* The selection on the canvas. */
static IvMaskView *view;
static IvMask *shown;			/* the state's mask the view shows (held) */
static double coverage = -1;		/* share of the picture selected */

/* The refine brush. */
static gboolean refining;
static gboolean refine_add = TRUE;
static double refine_size = 60;		/* diameter, SOURCE px */
static double softness = 40;		/* 0 hard .. 100 soft */
static IvMask *working;			/* a stroke being painted (writable) */
static gboolean painting, painting_add;
static double last_mx, last_my;
static gboolean pointer_in;
static double pointer_wx, pointer_wy;

/* The thumbnails of the actions. */
static IvImage *thumb_src;		/* the proxy, small */
static const IvImage *thumb_for;	/* which proxy it was made from */
static cairo_surface_t *thumbs[G_N_ELEMENTS(actions)];
static cairo_surface_t *thumb_plain;
static guint thumb_gen, thumb_timer;
static gboolean thumb_running, thumb_again;

/* The panel. */
static GtkWidget *detect_button, *status_label;
static GtkWidget *cards[G_N_ELEMENTS(actions)], *card_areas[G_N_ELEMENTS(actions)];
static GtkWidget *card_checks[G_N_ELEMENTS(actions)];
static GtkWidget *blur_box, *blur_row, *replace_box, *replace_swatches;
static GtkWidget *remove_note;
static GtkWidget *refine_switch, *refine_box, *mode_chips, *refine_size_row;
static GtkWidget *softness_row, *soften_button, *reset_button;
static gboolean syncing;

static void sync_panel(void);
static void schedule_thumbs(void);
static void update_flags(IvEditor *ed);

/* ---- small things -------------------------------------------------------------- */

static const IvBackgroundEdit *bg(IvEditor *ed)
{
	return &iv_editor_state(ed)->bg;
}

static gboolean has_mask(IvEditor *ed)
{
	return bg(ed)->mask != NULL;
}

/* The hatching shows while refining, and while there is a selection but
 * no action to show what it is. */
static gboolean hatch_visible(IvEditor *ed)
{
	return refining || (has_mask(ed) && !bg(ed)->active);
}

static double hardness(void)
{
	return CLAMP(1.0 - softness / 100.0, 0.0, 1.0);
}

/* The share of the picture selected, from the view (small enough to count
 * quickly, exact enough for a percentage). */
static double measure(const IvMaskView *v)
{
	guint64 sum = 0;

	if (!v || v->empty)
		return 0;
	for (int y = 0; y < v->h; y++) {
		const uint8_t *row = v->data + (gsize)y * v->stride;

		for (int x = 0; x < v->w; x++)
			sum += row[x];
	}
	return (double)sum / (255.0 * v->w * v->h);
}

/* Brings the view in line with the state's mask. */
static void sync_view(IvEditor *ed)
{
	IvMask *m = bg(ed)->mask;
	int mw, mh;

	if (painting)
		return;
	iv_brush_mask_size(ed, &mw, &mh);
	if (mw <= 0)
		return;
	if (view && (view->mw != mw || view->mh != mh))
		g_clear_pointer(&view, iv_mask_view_free);
	if (!view)
		view = iv_mask_view_new(mw, mh);
	if (m == shown && view && (m || view->empty))
		return;
	if (shown)
		iv_mask_unref(shown);
	shown = iv_mask_ref(m);
	if (!m) {
		iv_mask_view_clear(view);
	} else if (m->w == mw && m->h == mh) {
		iv_mask_view_load(view, m->data);
	} else {
		IvMask *c = iv_mask_resized(m, mw, mh);

		iv_mask_view_load(view, c ? c->data : NULL);
		iv_mask_unref(c);
	}
	coverage = measure(view);
}

/* ---- detecting ------------------------------------------------------------------- */

typedef struct {
	IvEditor *ed;
	IvImage *img;		/* a copy of the proxy */
	int mw, mh;
	IvMask *mask;		/* the result at MASK resolution */
	int result;		/* iv_bg_estimate's */
} Detect;

static void detect_free(gpointer p)
{
	Detect *d = p;

	if (d->img)
		iv_image_free(d->img);
	if (d->mask)
		iv_mask_unref(d->mask);
	g_free(d);
}

static void detect_work(gpointer data, GCancellable *cancel, IvProgress progress,
			void *progress_data)
{
	Detect *d = data;
	uint8_t *small = g_try_malloc0((gsize)d->img->w * (gsize)d->img->h);

	d->result = -1;
	if (!small)
		return;
	d->result = iv_bg_estimate(d->img, small, progress, progress_data);
	if (d->result == 0) {
		/* Not clear: the weak guess is kept as a start for the brush
		 * only when it is a guess at all -- one that takes (almost)
		 * the whole picture or (almost) nothing is no help. */
		gsize n = (gsize)d->img->w * (gsize)d->img->h;
		guint64 sum = 0;

		for (gsize i = 0; i < n; i++)
			sum += small[i];
		if (sum > 0.92 * 255.0 * n || sum < 0.02 * 255.0 * n)
			memset(small, 0, n);
	}
	if (d->result >= 0 && !g_cancellable_is_cancelled(cancel)) {
		d->mask = iv_mask_new(d->mw, d->mh);
		if (d->mask)
			iv_mask_scale(small, d->img->w, d->img->h, d->mask->data,
				      d->mw, d->mh);
		else
			d->result = -1;
	}
	g_free(small);
}

static void set_refining(IvEditor *ed, gboolean on);

static void detect_done(gpointer data, gboolean ok)
{
	Detect *d = data;
	IvEditor *ed = d->ed;
	IvEditState *s;

	if (!ok) {
		/* Cancelled, or the editor was left (both see that). */
		if (iv_editor_committed(ed))
			update_flags(ed);
		sync_panel();
		return;
	}
	update_flags(ed);
	if (d->result < 0 || !d->mask) {
		iv_editor_toast(ed, "The background could not be found (not "
				    "enough memory?)");
		sync_panel();
		return;
	}
	if (iv_mask_is_empty(d->mask)) {
		iv_editor_toast(ed, "No clear background found — paint it with "
				    "the brush");
		set_refining(ed, TRUE);
		sync_panel();
		return;
	}
	s = iv_edit_state_copy(iv_editor_committed(ed));
	iv_edit_state_set_bg_mask(s, d->mask);
	iv_editor_apply(ed, s, "Detect background");
	if (d->result == 1) {
		iv_editor_toast(ed, "Background found");
	} else {
		iv_editor_toast(ed, "No clear background found — paint it with "
				    "the brush");
		set_refining(ed, TRUE);
	}
	sync_panel();
}

static void detect_clicked(GtkButton *b, IvEditor *ed)
{
	const IvImage *proxy = iv_editor_proxy(ed);
	Detect *d;

	if (!proxy || iv_editor_busy(ed))
		return;
	d = g_new0(Detect, 1);
	d->ed = ed;
	/* A copy of its own: the worker must not depend on the editor's
	 * pictures, which go when the editor is left. */
	d->img = iv_image_copy(proxy);
	iv_brush_mask_size(ed, &d->mw, &d->mh);
	if (!d->img) {
		detect_free(d);
		iv_editor_toast(ed, "Not enough memory to look for the background");
		return;
	}
	iv_editor_run(ed, "Finding the background", detect_work, detect_done, d,
		      detect_free);
	if (iv_editor_busy(ed))
		iv_editor_set_cursor(ed, "progress");
	sync_panel();
}

/* ---- the actions ------------------------------------------------------------------ */

static void choose_action(IvEditor *ed, int i)
{
	IvEditState *s;
	const IvBackgroundEdit *b = &iv_editor_committed(ed)->bg;

	if (iv_editor_busy(ed))
		return;
	if (!b->mask) {
		iv_editor_toast(ed, "Find the background first: Detect, or paint "
				    "it with the refine brush");
		sync_panel();
		return;
	}
	s = iv_edit_state_copy(iv_editor_committed(ed));
	if (s->bg.active && s->bg.op == actions[i].op) {
		/* The chosen card again: the action goes, the selection
		 * stays. */
		s->bg.active = FALSE;
		iv_editor_apply(ed, s, "No background action");
	} else {
		s->bg.active = TRUE;
		s->bg.op = actions[i].op;
		iv_editor_apply(ed, s, actions[i].step);
		/* Choosing an action is wanting to see it. */
		if (refining)
			set_refining(ed, FALSE);
	}
	sync_panel();
}

static void card_clicked(GtkButton *b, gpointer data)
{
	if (editor && !syncing)
		choose_action(editor, GPOINTER_TO_INT(data));
}

static gboolean card_area_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	int i = GPOINTER_TO_INT(data);
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	cairo_surface_t *s = thumbs[i] ? thumbs[i] : thumb_plain;

	iv_brush_paint_thumb(cr, s, 0, 0, W, H, 6);
	if (!editor || !has_mask(editor)) {
		/* Nothing to show the action on yet. */
		iv_rounded_rect(cr, 0, 0, W, H, 6);
		cairo_set_source_rgba(cr, 0x16 / 255.0, 0x1c / 255.0, 0x2a / 255.0,
				      0.55);
		cairo_fill(cr);
	}
	iv_rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 6);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	return TRUE;
}

static double get_strength(const IvEditState *s, gpointer data)
{
	return s->bg.strength;
}

static void set_strength(IvEditState *s, double v, gpointer data)
{
	s->bg.strength = CLAMP(v, 0, 100);
	if (s->bg.mask) {
		s->bg.active = TRUE;
		s->bg.op = IV_BG_BLUR;
	}
}

static void strength_moved(GtkRange *r, gpointer data)
{
	/* The Blur card follows the slider. */
	schedule_thumbs();
}

static void backdrop_picked(guint32 rgba, gpointer data)
{
	IvEditor *ed = editor;
	IvEditState *s;

	if (!ed || syncing || iv_editor_busy(ed))
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	s->bg.rgba = rgba;
	if (s->bg.mask) {
		s->bg.active = TRUE;
		s->bg.op = IV_BG_REPLACE;
	}
	iv_editor_apply(ed, s, "Background colour");
	sync_panel();
}

/* ---- the thumbnails ------------------------------------------------------------- */

typedef struct {
	guint gen;
	IvImage *src;
	int sw, sh;
	IvEditState *state;
	IvRenderCache *cache;
	cairo_surface_t *out[G_N_ELEMENTS(actions)];
	cairo_surface_t *plain;
} Thumbs;

static void thumbs_free(gpointer p)
{
	Thumbs *t = p;

	iv_image_free(t->src);
	iv_edit_state_free(t->state);
	iv_render_cache_free(t->cache);
	for (int i = 0; i < N_ACTIONS; i++)
		if (t->out[i])
			cairo_surface_destroy(t->out[i]);
	if (t->plain)
		cairo_surface_destroy(t->plain);
	g_free(t);
}

static cairo_surface_t *thumb_render(Thumbs *t, IvEditState *st,
				     GCancellable *c)
{
	IvImage *img = iv_render(t->src, t->sw, t->sh, st, IV_RENDER_NO_DRAW,
				 t->cache, c, NULL, NULL);
	cairo_surface_t *s;

	if (!img)
		return NULL;
	s = iv_surface_from_image(img);
	iv_image_free(img);
	return s;
}

static void thumbs_thread(GTask *task, gpointer src, gpointer data,
			  GCancellable *c)
{
	Thumbs *t = data;
	IvEditState *st = iv_edit_state_copy(t->state);

	st->bg.active = FALSE;
	t->plain = thumb_render(t, st, c);
	if (t->state->bg.mask) {
		for (int i = 0; i < N_ACTIONS; i++) {
			st->bg.active = TRUE;
			st->bg.op = actions[i].op;
			t->out[i] = thumb_render(t, st, c);
		}
	}
	iv_edit_state_free(st);
	g_task_return_boolean(task, TRUE);
}

static void start_thumbs(void);

static void thumbs_done(GObject *o, GAsyncResult *res, gpointer data)
{
	Thumbs *t = g_task_get_task_data(G_TASK(res));

	thumb_running = FALSE;
	if (t->gen == thumb_gen) {
		for (int i = 0; i < N_ACTIONS; i++) {
			if (thumbs[i])
				cairo_surface_destroy(thumbs[i]);
			thumbs[i] = t->out[i];
			t->out[i] = NULL;
		}
		if (t->plain) {
			if (thumb_plain)
				cairo_surface_destroy(thumb_plain);
			thumb_plain = t->plain;
			t->plain = NULL;
		}
		for (int i = 0; i < N_ACTIONS; i++)
			if (card_areas[i])
				gtk_widget_queue_draw(card_areas[i]);
	}
	if (thumb_again) {
		thumb_again = FALSE;
		start_thumbs();
	}
}

/* The small copy of the proxy the thumbnails are rendered from, made again
 * when the editor has another picture. */
static gboolean ensure_thumb_src(IvEditor *ed)
{
	const IvImage *proxy = iv_editor_proxy(ed);
	double k;

	if (!proxy)
		return FALSE;
	if (thumb_src && thumb_for == proxy)
		return TRUE;
	if (thumb_src)
		iv_image_free(thumb_src);
	k = MIN(1.0, (double)THUMB_EDGE / MAX(proxy->w, proxy->h));
	thumb_src = iv_image_scale(proxy, MAX(1, (int)lround(proxy->w * k)),
				   MAX(1, (int)lround(proxy->h * k)), 1);
	thumb_for = proxy;
	for (int i = 0; i < N_ACTIONS; i++)
		g_clear_pointer(&thumbs[i], cairo_surface_destroy);
	g_clear_pointer(&thumb_plain, cairo_surface_destroy);
	return thumb_src != NULL;
}

static void start_thumbs(void)
{
	Thumbs *t;
	GTask *task;
	int sw, sh;

	if (!editor || !ensure_thumb_src(editor))
		return;
	if (thumb_running) {
		thumb_again = TRUE;
		return;
	}
	iv_editor_source_size(editor, &sw, &sh);
	t = g_new0(Thumbs, 1);
	t->gen = ++thumb_gen;
	t->src = iv_image_copy(thumb_src);
	t->sw = sw;
	t->sh = sh;
	t->state = iv_edit_state_copy(iv_editor_state(editor));
	/* A cache of the task's own: the four renders share the retouch
	 * steps, and no other thread ever touches it. */
	t->cache = iv_render_cache_new();
	if (!t->src) {
		thumbs_free(t);
		return;
	}
	thumb_running = TRUE;
	task = g_task_new(NULL, NULL, thumbs_done, NULL);
	g_task_set_task_data(task, t, thumbs_free);
	g_task_run_in_thread(task, thumbs_thread);
	g_object_unref(task);
}

static gboolean thumbs_later(gpointer data)
{
	thumb_timer = 0;
	start_thumbs();
	return G_SOURCE_REMOVE;
}

static void schedule_thumbs(void)
{
	if (thumb_timer)
		g_source_remove(thumb_timer);
	thumb_timer = g_timeout_add(140, thumbs_later, NULL);
}

/* ---- refining ------------------------------------------------------------------- */

static void update_flags(IvEditor *ed)
{
	if (iv_editor_current_tool(ed) != &iv_tool_background)
		return;
	iv_editor_set_render_flags(ed, refining ? IV_RENDER_NO_BACKGROUND :
						  IV_RENDER_ALL);
	/* The brush wants the first button; without it the picture pans. */
	iv_editor_set_drag_pan(ed, !refining);
	iv_editor_set_cursor(ed, refining ? "none" : NULL);
	iv_editor_redraw_overlay(ed);
}

static void set_refining(IvEditor *ed, gboolean on)
{
	refining = on;
	if (refine_switch) {
		syncing = TRUE;
		gtk_switch_set_active(GTK_SWITCH(refine_switch), on);
		syncing = FALSE;
	}
	update_flags(ed);
	sync_panel();
}

static void to_mask(IvEditor *ed, double dx, double dy, double *mx, double *my)
{
	double sx, sy, k = iv_brush_mask_scale(ed);

	iv_editor_display_to_source(ed, dx, dy, &sx, &sy);
	*mx = sx * k;
	*my = sy * k;
}

static void paint_to(IvEditor *ed, double mx, double my)
{
	double r = refine_size / 2 * iv_brush_mask_scale(ed);
	int value = painting_add ? 255 : 0;

	iv_brush_line(working->data, working->w, working->w, working->h, last_mx,
		      last_my, mx, my, r, value, hardness(), NULL);
	iv_mask_view_line(view, last_mx, last_my, mx, my, r, value, hardness());
	last_mx = mx;
	last_my = my;
}

static gboolean refine_begin(IvEditor *ed, double x, double y, gboolean add)
{
	IvMask *base = iv_brush_mask_conform(ed, iv_editor_committed(ed)->bg.mask);
	double mx, my;

	sync_view(ed);
	if (base) {
		working = iv_mask_copy(base);
		iv_mask_unref(base);
	} else {
		working = iv_brush_mask_new(ed);
	}
	if (!working || !view)
		return FALSE;
	painting = TRUE;
	painting_add = add;
	to_mask(ed, x, y, &mx, &my);
	last_mx = mx;
	last_my = my;
	paint_to(ed, mx, my);
	return TRUE;
}

static void refine_end(IvEditor *ed)
{
	IvEditState *s;
	IvMask *m = working;

	painting = FALSE;
	working = NULL;
	if (!m)
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	if (iv_mask_is_empty(m)) {
		iv_edit_state_set_bg_mask(s, NULL);
	} else {
		iv_edit_state_set_bg_mask(s, m);
	}
	/* The view shows this mask already: no reload when the state
	 * change comes back to the tool. */
	if (shown)
		iv_mask_unref(shown);
	shown = s->bg.mask ? iv_mask_ref(s->bg.mask) : NULL;
	coverage = measure(view);
	iv_mask_unref(m);
	iv_editor_apply(ed, s, painting_add ? "Add to background" :
					      "Erase from background");
	sync_panel();
	schedule_thumbs();
}

static void refine_toggled(GObject *sw, GParamSpec *ps, gpointer data)
{
	if (syncing || !editor)
		return;
	set_refining(editor, gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void mode_chosen(int index, gpointer data)
{
	refine_add = index == 0;
}

/* The refine brush's size and softness are kept from one run to the
 * next. */
#define SETTINGS_GROUP "Background"

static void store_settings(void)
{
	iv_settings_set_tool_double(SETTINGS_GROUP, "BrushSize", refine_size);
	iv_settings_set_tool_double(SETTINGS_GROUP, "Softness", softness);
}

static void refine_size_changed(double v, gboolean final, gpointer data)
{
	refine_size = CLAMP(v, REFINE_MIN, REFINE_MAX);
	if (final)
		store_settings();
	if (editor)
		iv_editor_redraw_overlay(editor);
}

static void softness_changed(double v, gboolean final, gpointer data)
{
	softness = CLAMP(v, 0, 100);
	if (final)
		store_settings();
}

/* "Soften edges": the whole selection feathered once more, in a worker
 * (a large mask takes a moment). */
typedef struct {
	IvEditor *ed;
	IvMask *mask;		/* writable copy */
	double radius;
} Soften;

static void soften_free(gpointer p)
{
	Soften *f = p;

	iv_mask_unref(f->mask);
	g_free(f);
}

static void soften_work(gpointer data, GCancellable *cancel, IvProgress progress,
			void *progress_data)
{
	Soften *f = data;

	progress(0.2, progress_data);
	iv_mask_feather(f->mask->data, f->mask->w, f->mask->h, f->radius);
	progress(1.0, progress_data);
}

static void soften_done(gpointer data, gboolean ok)
{
	Soften *f = data;
	IvEditState *s;

	if (!ok) {
		if (iv_editor_committed(f->ed))
			update_flags(f->ed);
		sync_panel();
		return;
	}
	update_flags(f->ed);
	s = iv_edit_state_copy(iv_editor_committed(f->ed));
	iv_edit_state_set_bg_mask(s, f->mask);
	iv_editor_apply(f->ed, s, "Soften edges");
	sync_panel();
}

static void soften_clicked(GtkButton *b, IvEditor *ed)
{
	IvMask *m = iv_editor_committed(ed)->bg.mask;
	Soften *f;

	if (!m || iv_editor_busy(ed))
		return;
	f = g_new0(Soften, 1);
	f->ed = ed;
	f->mask = iv_mask_copy(m);
	/* Half a percent of the long edge: a visible softening that keeps
	 * the outline where it was. */
	f->radius = MAX(1.5, MAX(m->w, m->h) * 0.005);
	if (!f->mask) {
		g_free(f);
		return;
	}
	iv_editor_run(ed, "Softening the edges", soften_work, soften_done, f,
		      soften_free);
	if (iv_editor_busy(ed))
		iv_editor_set_cursor(ed, "progress");
}

static void reset_answered(gboolean yes, gpointer data)
{
	IvEditor *ed = data;
	IvEditState *s;

	if (!yes || iv_editor_busy(ed))
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	iv_edit_state_set_bg_mask(s, NULL);
	s->bg.active = FALSE;
	iv_editor_apply(ed, s, "Reset background");
	sync_panel();
}

static void reset_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_busy(ed) || !iv_editor_committed(ed)->bg.mask)
		return;
	iv_editor_confirm(ed, "Reset the background?",
			  "The selection and what was done to the background "
			  "are removed.  Undo brings them back.",
			  "_Reset", "_Keep", TRUE, reset_answered, ed);
}

/* ---- the canvas ------------------------------------------------------------------ */

static gboolean canvas_event(IvEditor *ed, GdkEvent *ev, double x, double y)
{
	double mx, my;

	if (!refining)
		return FALSE;
	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		if (ev->button.button != 1 && ev->button.button != 3)
			return FALSE;
		/* The right button, or Alt, paints the other way. */
		if (!refine_begin(ed, x, y,
				  refine_add ^ (ev->button.button == 3 ||
						(ev->button.state & GDK_MOD1_MASK) != 0)))
			return FALSE;
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
		return TRUE;
	case GDK_MOTION_NOTIFY:
		iv_editor_display_to_widget(ed, x, y, &pointer_wx, &pointer_wy);
		pointer_in = TRUE;
		if (painting && working) {
			to_mask(ed, x, y, &mx, &my);
			paint_to(ed, mx, my);
		}
		iv_editor_redraw_overlay(ed);
		return painting;
	case GDK_BUTTON_RELEASE:
		if (!painting)
			return FALSE;
		refine_end(ed);
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_LEAVE_NOTIFY:
		if (!painting) {
			pointer_in = FALSE;
			iv_editor_redraw_overlay(ed);
		}
		return FALSE;
	default:
		return FALSE;
	}
}

static void draw_overlay(IvEditor *ed, cairo_t *cr)
{
	cairo_matrix_t m;

	if (!iv_brush_source_matrix(ed, &m))
		return;
	if (painting || hatch_visible(ed)) {
		sync_view(ed);
		iv_mask_view_paint(view, cr, &m, iv_brush_mask_scale(ed),
				   IV_MASK_HATCH, HATCH_ACCENT);
	}
	if (refining && pointer_in && !iv_editor_busy(ed))
		iv_brush_draw_cursor(cr, pointer_wx, pointer_wy,
				     refine_size / 2 * iv_brush_matrix_scale(&m),
				     IV_CURSOR_DASHED, 0);
}

/* ---- the panel ------------------------------------------------------------------ */

static void sync_panel(void)
{
	const IvBackgroundEdit *b;
	gboolean busy, mask;
	char buf[96];

	if (!detect_button || !editor || !iv_editor_committed(editor))
		return;
	busy = iv_editor_busy(editor);
	b = &iv_editor_committed(editor)->bg;
	mask = b->mask != NULL;
	syncing = TRUE;

	gtk_widget_set_sensitive(detect_button, !busy && iv_editor_proxy(editor));
	if (busy) {
		g_strlcpy(buf, "Working…", sizeof buf);
	} else if (!mask) {
		g_strlcpy(buf, "Nothing selected yet.", sizeof buf);
	} else {
		if (coverage < 0 || b->mask != shown)
			sync_view(editor);
		g_snprintf(buf, sizeof buf, "The background is %d %% of the picture.",
			   (int)lround(CLAMP(coverage, 0, 1) * 100));
	}
	gtk_label_set_text(GTK_LABEL(status_label), buf);

	for (int i = 0; i < N_ACTIONS; i++) {
		gboolean on = mask && b->active && b->op == actions[i].op;

		iv_set_class(cards[i], "iv-selected", on);
		gtk_widget_set_visible(card_checks[i], on);
		iv_set_class(cards[i], "iv-unavailable", !mask);
		gtk_widget_set_tooltip_text(cards[i],
					    !mask ? "Detect or paint the "
						    "background first" :
					    on ? "Press again to take the "
						 "action off" :
						 NULL);
		gtk_widget_queue_draw(card_areas[i]);
	}
	gtk_widget_set_visible(blur_box, mask && b->active && b->op == IV_BG_BLUR);
	gtk_widget_set_visible(replace_box,
			       mask && b->active && b->op == IV_BG_REPLACE);
	gtk_widget_set_visible(remove_note,
			       mask && b->active && b->op == IV_BG_REMOVE);
	iv_panel_slider_sync(blur_row);
	iv_panel_swatches_set(replace_swatches, b->rgba);

	gtk_switch_set_active(GTK_SWITCH(refine_switch), refining);
	gtk_widget_set_visible(refine_box, refining);
	iv_panel_chips_set(mode_chips, refine_add ? 0 : 1);
	iv_brush_slider_set(refine_size_row, refine_size);
	iv_brush_slider_set(softness_row, softness);
	gtk_widget_set_sensitive(soften_button, mask && !busy);
	gtk_widget_set_sensitive(reset_button, mask && !busy);
	syncing = FALSE;
}

static GtkWidget *action_card(int i)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *title = gtk_label_new(actions[i].title);
	GtkWidget *text = gtk_label_new(actions[i].text);
	GtkWidget *check = iv_glyph_new(IV_GLYPH_CHECK, 16);

	card_areas[i] = gtk_drawing_area_new();
	gtk_widget_set_size_request(card_areas[i], THUMB_W, THUMB_H);
	g_signal_connect(card_areas[i], "draw", G_CALLBACK(card_area_draw),
			 GINT_TO_POINTER(i));
	gtk_box_pack_start(GTK_BOX(row), card_areas[i], FALSE, FALSE, 0);

	gtk_label_set_xalign(GTK_LABEL(title), 0);
	iv_add_class(title, "iv-bg-card-title");
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(text), 18);
	iv_add_class(text, "iv-bg-card-text");
	gtk_box_pack_start(GTK_BOX(col), title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), text, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);

	gtk_widget_set_valign(check, GTK_ALIGN_START);
	gtk_widget_set_no_show_all(check, TRUE);
	card_checks[i] = check;
	gtk_box_pack_end(GTK_BOX(row), check, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_widget_set_can_focus(b, FALSE);
	iv_add_class(b, "iv-bg-card");
	atk_object_set_name(gtk_widget_get_accessible(b), actions[i].step);
	g_signal_connect(b, "clicked", G_CALLBACK(card_clicked), GINT_TO_POINTER(i));
	return b;
}

static GtkWidget *build_panel(IvEditor *ed)
{
	static const char *const modes[] = { "Add", "Erase" };
	GtkWidget *p = iv_panel_new("Background",
				    "Blur, remove or replace what is behind "
				    "the subject.");
	GtkWidget *row, *box, *caption;

	editor = ed;
	refine_size = iv_settings_tool_double(SETTINGS_GROUP, "BrushSize",
					      refine_size, REFINE_MIN, REFINE_MAX);
	softness = iv_settings_tool_double(SETTINGS_GROUP, "Softness", softness, 0,
					   100);

	detect_button = iv_panel_button(IV_GLYPH_WAND, "Detect background");
	iv_add_class(detect_button, "iv-accent-button");
	gtk_widget_set_tooltip_text(detect_button,
				    "Find what is behind the subject");
	g_signal_connect(detect_button, "clicked", G_CALLBACK(detect_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), detect_button, FALSE, FALSE, 6);
	status_label = iv_panel_note(NULL);
	gtk_box_pack_start(GTK_BOX(p), status_label, FALSE, FALSE, 0);

	iv_brush_section(p, IV_GLYPH_BACKGROUND, "Action", NULL);
	for (int i = 0; i < N_ACTIONS; i++) {
		cards[i] = action_card(i);
		gtk_box_pack_start(GTK_BOX(p), cards[i], FALSE, FALSE, 0);
	}

	/* What the chosen action takes. */
	blur_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	blur_row = iv_panel_slider(ed, "Strength", 0, 100, 1, 50, get_strength,
				   set_strength, NULL);
	g_signal_connect(g_object_get_data(G_OBJECT(blur_row), "iv-scale"),
			 "value-changed", G_CALLBACK(strength_moved), NULL);
	gtk_box_pack_start(GTK_BOX(blur_box), blur_row, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(blur_box, TRUE);
	gtk_widget_show_all(blur_row);
	gtk_box_pack_start(GTK_BOX(p), blur_box, FALSE, FALSE, 4);

	replace_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	caption = iv_brush_caption("Colour");
	gtk_box_pack_start(GTK_BOX(replace_box), caption, FALSE, FALSE, 0);
	replace_swatches = iv_panel_swatches(backdrops, G_N_ELEMENTS(backdrops),
					     0xffffffff, backdrop_picked, NULL);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(replace_swatches), TRUE);
	iv_add_class(replace_swatches, "iv-brush-swatches");
	gtk_box_pack_start(GTK_BOX(replace_box), replace_swatches, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(replace_box, TRUE);
	gtk_widget_show_all(replace_swatches);
	gtk_widget_show(caption);
	gtk_box_pack_start(GTK_BOX(p), replace_box, FALSE, FALSE, 4);

	remove_note = iv_panel_note("Transparent parts show as a checkerboard.  "
				    "Save as PNG or WebP to keep them.");
	gtk_widget_set_no_show_all(remove_note, TRUE);
	gtk_box_pack_start(GTK_BOX(p), remove_note, FALSE, FALSE, 4);

	/* Refining by hand. */
	iv_brush_section(p, IV_GLYPH_BRUSH, "Refine", NULL);
	row = iv_panel_switch("Refine brush", refining, &refine_switch);
	gtk_widget_set_tooltip_text(row, "Paint the selection by hand");
	g_signal_connect(refine_switch, "notify::active",
			 G_CALLBACK(refine_toggled), NULL);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	refine_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	mode_chips = iv_panel_chips(modes, G_N_ELEMENTS(modes), refine_add ? 0 : 1,
				    mode_chosen, NULL);
	iv_add_class(mode_chips, "iv-brush-chips");
	gtk_box_pack_start(GTK_BOX(refine_box), mode_chips, FALSE, FALSE, 0);
	refine_size_row = iv_brush_slider("Size", REFINE_MIN, REFINE_MAX,
					  refine_size, TRUE, " px",
					  refine_size_changed, NULL);
	gtk_box_pack_start(GTK_BOX(refine_box), refine_size_row, FALSE, FALSE, 0);
	softness_row = iv_brush_slider("Edge softness", 0, 100, softness, FALSE,
				       " %", softness_changed, NULL);
	gtk_box_pack_start(GTK_BOX(refine_box), softness_row, FALSE, FALSE, 0);
	box = iv_panel_note("Add paints over the background, Erase gives "
			    "parts back to the subject.  The right button "
			    "paints the other way.");
	gtk_box_pack_start(GTK_BOX(refine_box), box, FALSE, FALSE, 2);
	gtk_widget_set_no_show_all(refine_box, TRUE);
	gtk_widget_show_all(mode_chips);
	gtk_widget_show_all(refine_size_row);
	gtk_widget_show_all(softness_row);
	gtk_widget_show(box);
	gtk_box_pack_start(GTK_BOX(p), refine_box, FALSE, FALSE, 4);

	soften_button = iv_panel_button(IV_GLYPH_BLUR, "Soften edges");
	gtk_widget_set_tooltip_text(soften_button,
				    "Feather the outline of the selection");
	g_signal_connect(soften_button, "clicked", G_CALLBACK(soften_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), soften_button, FALSE, FALSE, 0);
	reset_button = iv_panel_button(IV_GLYPH_RESET, "Reset background");
	gtk_widget_set_tooltip_text(reset_button,
				    "Remove the selection and the action");
	g_signal_connect(reset_button, "clicked", G_CALLBACK(reset_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), reset_button, FALSE, FALSE, 4);
	return p;
}

/* ---- the tool ------------------------------------------------------------------------ */

static void activate(IvEditor *ed)
{
	editor = ed;
	pointer_in = FALSE;
	if (!iv_editor_committed(ed))
		return;
	update_flags(ed);
	sync_view(ed);
	sync_panel();
	schedule_thumbs();
}

static void deactivate(IvEditor *ed)
{
	if (painting)
		refine_end(ed);
	pointer_in = FALSE;
	if (thumb_timer) {
		g_source_remove(thumb_timer);
		thumb_timer = 0;
	}
	/* Thumbnails of another picture must not show next time. */
	thumb_gen++;
	thumb_for = NULL;
	g_clear_pointer(&thumb_src, iv_image_free);
	for (int i = 0; i < N_ACTIONS; i++)
		g_clear_pointer(&thumbs[i], cairo_surface_destroy);
	g_clear_pointer(&thumb_plain, cairo_surface_destroy);
	g_clear_pointer(&shown, iv_mask_unref);
	g_clear_pointer(&view, iv_mask_view_free);
	coverage = -1;
}

static void state_changed(IvEditor *ed)
{
	sync_view(ed);
	sync_panel();
	schedule_thumbs();
	iv_editor_redraw_overlay(ed);
}

static gboolean key_press(IvEditor *ed, GdkEventKey *ev)
{
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();

	if (mods & GDK_CONTROL_MASK)
		return FALSE;
	switch (ev->keyval) {
	case GDK_KEY_bracketleft:
		refine_size = CLAMP(refine_size / 1.2, REFINE_MIN, REFINE_MAX);
		store_settings();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_KEY_bracketright:
		refine_size = CLAMP(refine_size * 1.2, REFINE_MIN, REFINE_MAX);
		store_settings();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_KEY_x:
	case GDK_KEY_X:
		/* Add <-> Erase, as in most paint programs. */
		if (!refining)
			return FALSE;
		refine_add = !refine_add;
		sync_panel();
		return TRUE;
	}
	return FALSE;
}

const IvTool iv_tool_background = {
	.id = "background",
	.label = "Background",
	.glyph = IV_GLYPH_BACKGROUND,
	.tooltip = "Background (B)",
	.key = GDK_KEY_b,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.canvas_event = canvas_event,
	.draw_overlay = draw_overlay,
	.state_changed = state_changed,
	.key_press = key_press,
};
