// LikeOS -- LikeOS Image Viewer: the Styles tool.
//
// Ready-made looks for the whole picture, and Auto enhance.
//
//   Auto enhance  asks the processing library for a histogram-based
//                 suggestion -- levels, a gentle contrast, white balance,
//                 a lift for dark pictures -- and puts it on the Adjust
//                 sliders (light and colour; the detail sliders and the
//                 vignette stay as they are), as one undoable step.  The
//                 result is ordinary slider values, so Adjust can refine
//                 it.  While the sliders hold exactly the suggestion the
//                 button says so, and pressing it again takes it off.
//   The grid      every style as a small preview of THIS picture as it is
//                 edited now (crop, adjustments, retouching and all), the
//                 unstyled picture first.  The previews are rendered in a
//                 worker thread from a small copy of the proxy and kept;
//                 they are made again, shortly after, when the edit under
//                 the style changes.  The current style is outlined in the
//                 accent with a check mark.  Arrow keys walk the grid and
//                 preview each style (one undo step once they rest),
//                 Return or a click takes it at once.
//   Intensity     how much of the chosen style is mixed in, 0..100 %.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "editor.h"

/* The cells of the grid: a square preview over the style's name. */
#define CELL_PX 88
/* The small copy of the picture the previews are made from: its long
 * edge.  Large enough for a crop of a third of the picture to still fill
 * a cell. */
#define SMALL_EDGE 400
/* The edit under the style changed: previews again after this (ms). */
#define REFRESH_MS 250
/* Arrow keys through the grid: the choice becomes a step after this. */
#define KEY_REST_MS 450

typedef struct {
	guint gen;
	IvImage *proxy;		/* a copy, only when `small' must be made */
	IvImage *small;		/* the small source (made here or given) */
	int sw, sh;		/* full source size */
	IvEditState *base;	/* the edit under the style */
	int px;			/* preview size, device pixels */
	GCancellable *cancel;
	cairo_surface_t *out[IV_STYLE_COUNT];
} ThumbJob;

static struct {
	IvEditor *ed;
	GtkWidget *panel, *grid, *auto_button, *auto_note;
	GtkWidget *cells[IV_STYLE_COUNT];	/* the drawing areas */
	GtkWidget *children[IV_STYLE_COUNT];	/* their flow box children */
	GtkWidget *intensity, *intensity_note;
	gboolean syncing;
	guint key_rest;

	/* the previews */
	cairo_surface_t *thumbs[IV_STYLE_COUNT];
	IvEditState *thumb_base;	/* what they show */
	IvImage *small;			/* the small source */
	guint32 small_print;		/* the proxy it was made of */
	int small_w, small_h;		/* that proxy's size */
	char *small_path;		/* and its file */
	guint gen;
	gboolean running, again;
	GCancellable *cancel;
	guint refresh;

	/* Auto enhance */
	IvAdjust suggestion;
	gboolean have_suggestion;
	const IvEditState *auto_state;	/* the state Auto enhance made */
} S;

static void sync_panel(IvEditor *ed);
static void schedule_refresh(IvEditor *ed);

/* ---- what the previews show ------------------------------------------------------ */

/* The edit under the style: the committed state without its style. */
static IvEditState *base_of(const IvEditState *st)
{
	IvEditState *b = iv_edit_state_copy(st);

	b->style = IV_STYLE_NONE;
	b->style_intensity = 0;
	return b;
}

/* A fingerprint of the proxy's pixels: a hash of a 16 x 16 grid of
 * them.  The same file edited again after saving has the same name and
 * often the same size (and its proxy may even land at the same address),
 * but not the same pixels. */
static guint32 fingerprint(const IvImage *img)
{
	guint32 h = 2166136261u;

	if (!img)
		return 0;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 16; gx++) {
			int x = (int)((gx + 0.5) * img->w / 16);
			int y = (int)((gy + 0.5) * img->h / 16);
			const uint8_t *px = img->px + (gsize)y * img->stride + 4 * x;

			for (int c = 0; c < 4; c++) {
				h ^= px[c];
				h *= 16777619u;
			}
		}
	return h ? h : 1;
}

/* The small source belongs to one picture: forget it (and the previews)
 * when the editor has another one. */
static void check_picture(IvEditor *ed)
{
	const IvImage *proxy = iv_editor_proxy(ed);
	const char *path = iv_editor_path(ed);
	guint32 print;

	if (proxy && proxy->w == S.small_w && proxy->h == S.small_h &&
	    g_strcmp0(path, S.small_path) == 0) {
		print = fingerprint(proxy);
		if (print == S.small_print)
			return;
	} else {
		print = fingerprint(proxy);
	}
	if (S.cancel)
		g_cancellable_cancel(S.cancel);
	S.gen++;
	S.running = FALSE;
	S.again = FALSE;
	g_clear_pointer(&S.small, iv_image_free);
	g_clear_pointer(&S.thumb_base, iv_edit_state_free);
	for (int i = 0; i < IV_STYLE_COUNT; i++)
		g_clear_pointer(&S.thumbs[i], cairo_surface_destroy);
	S.small_print = print;
	S.small_w = proxy ? proxy->w : 0;
	S.small_h = proxy ? proxy->h : 0;
	g_free(S.small_path);
	S.small_path = g_strdup(path);
	S.have_suggestion = FALSE;
	S.auto_state = NULL;
	if (S.grid)
		gtk_widget_queue_draw(S.grid);
}

static gboolean thumbs_current(IvEditor *ed)
{
	IvEditState *b;
	gboolean same;

	if (!S.thumb_base || !iv_editor_committed(ed))
		return FALSE;
	b = base_of(iv_editor_committed(ed));
	same = iv_edit_state_equal(b, S.thumb_base);
	iv_edit_state_free(b);
	return same;
}

/* ---- rendering the previews (worker thread) ---------------------------------------- */

static void thumb_job_free(gpointer p)
{
	ThumbJob *j = p;

	if (j->proxy)
		iv_image_free(j->proxy);
	if (j->small)
		iv_image_free(j->small);
	if (j->base)
		iv_edit_state_free(j->base);
	g_clear_object(&j->cancel);
	for (int i = 0; i < IV_STYLE_COUNT; i++)
		if (j->out[i])
			cairo_surface_destroy(j->out[i]);
	g_free(j);
}

/* The middle square of `img', `px' wide: the cells are square, and a
 * filled cell reads better than a letterboxed one. */
static IvImage *square_of(const IvImage *img, int px)
{
	int side = MIN(img->w, img->h);
	IvImage *sq, *out;

	if (side <= 0)
		return NULL;
	sq = iv_crop(img, (img->w - side) / 2, (img->h - side) / 2, side, side);
	if (!sq)
		return NULL;
	if (side == px)
		return sq;
	out = iv_image_scale(sq, px, px, 1);
	iv_image_free(sq);
	return out;
}

static void thumb_thread(GTask *task, gpointer src, gpointer data,
			 GCancellable *c)
{
	ThumbJob *j = data;
	IvRenderCache *cache;
	IvImage *base, *sq;

	if (!j->small && j->proxy) {
		double k = (double)SMALL_EDGE / MAX(j->proxy->w, j->proxy->h);

		if (k >= 1)
			j->small = iv_image_copy(j->proxy);
		else
			j->small = iv_image_scale(j->proxy,
						  MAX(1, (int)lround(j->proxy->w * k)),
						  MAX(1, (int)lround(j->proxy->h * k)), 1);
		g_clear_pointer(&j->proxy, iv_image_free);
	}
	if (!j->small || g_cancellable_is_cancelled(j->cancel)) {
		g_task_return_boolean(task, FALSE);
		return;
	}
	/* The edit under the style, small; a cache of its own (the retouch
	 * steps at this size are not worth keeping). */
	cache = iv_render_cache_new();
	base = iv_render(j->small, j->sw, j->sh, j->base, IV_RENDER_ALL, cache,
			 j->cancel, NULL, NULL);
	iv_render_cache_free(cache);
	if (!base) {
		g_task_return_boolean(task, FALSE);
		return;
	}
	sq = square_of(base, j->px);
	iv_image_free(base);
	if (!sq) {
		g_task_return_boolean(task, FALSE);
		return;
	}
	for (int s = 0; s < IV_STYLE_COUNT; s++) {
		IvImage *t;

		if (g_cancellable_is_cancelled(j->cancel))
			break;
		t = iv_image_copy(sq);
		if (!t)
			continue;
		if (s != IV_STYLE_NONE)
			iv_style_apply(t, (IvStyle)s, 1.0);
		j->out[s] = iv_surface_from_image(t);
		iv_image_free(t);
	}
	iv_image_free(sq);
	g_task_return_boolean(task, !g_cancellable_is_cancelled(j->cancel));
}

static void start_thumbs(IvEditor *ed);

static void thumbs_done(GObject *o, GAsyncResult *res, gpointer data)
{
	ThumbJob *j = g_task_get_task_data(G_TASK(res));
	gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);

	if (j->gen != S.gen)
		return;	/* another picture meanwhile */
	S.running = FALSE;
	g_clear_object(&S.cancel);
	if (ok) {
		for (int i = 0; i < IV_STYLE_COUNT; i++) {
			if (S.thumbs[i])
				cairo_surface_destroy(S.thumbs[i]);
			S.thumbs[i] = j->out[i];
			j->out[i] = NULL;
		}
		if (S.thumb_base)
			iv_edit_state_free(S.thumb_base);
		S.thumb_base = j->base;
		j->base = NULL;
	}
	if (j->small && !S.small) {
		S.small = j->small;
		j->small = NULL;
	}
	if (S.grid)
		gtk_widget_queue_draw(S.grid);
	if (S.again && S.ed) {
		S.again = FALSE;
		schedule_refresh(S.ed);
	}
}

static void start_thumbs(IvEditor *ed)
{
	ThumbJob *j;
	GTask *t;
	const IvImage *proxy = iv_editor_proxy(ed);
	int scale = S.grid ? gtk_widget_get_scale_factor(S.grid) : 1;

	if (!proxy || !iv_editor_committed(ed))
		return;
	if (S.running) {
		S.again = TRUE;
		return;
	}
	j = g_new0(ThumbJob, 1);
	j->gen = S.gen;
	if (S.small)
		j->small = iv_image_copy(S.small);
	else
		j->proxy = iv_image_copy(proxy);
	iv_editor_source_size(ed, &j->sw, &j->sh);
	j->base = base_of(iv_editor_committed(ed));
	j->px = CELL_PX * MAX(1, scale);
	S.cancel = g_cancellable_new();
	j->cancel = g_object_ref(S.cancel);
	S.running = TRUE;
	t = g_task_new(NULL, NULL, thumbs_done, NULL);
	g_task_set_task_data(t, j, thumb_job_free);
	g_task_run_in_thread(t, thumb_thread);
	g_object_unref(t);
}

static gboolean refresh_due(gpointer data)
{
	IvEditor *ed = data;

	S.refresh = 0;
	if (iv_editor_current_tool(ed) != &iv_tool_styles)
		return G_SOURCE_REMOVE;
	check_picture(ed);
	if (!thumbs_current(ed))
		start_thumbs(ed);
	return G_SOURCE_REMOVE;
}

/* Previews again soon, if the edit under the style is not what they
 * show.  Only while Styles is the tool shown: the others never see them,
 * and activate() catches up. */
static void schedule_refresh(IvEditor *ed)
{
	if (!S.panel || iv_editor_current_tool(ed) != &iv_tool_styles)
		return;
	check_picture(ed);
	if (thumbs_current(ed))
		return;
	if (S.refresh)
		g_source_remove(S.refresh);
	/* Nothing to show yet: no need to wait. */
	S.refresh = g_timeout_add(S.thumb_base ? REFRESH_MS : 1, refresh_due, ed);
}

/* ---- the cells ------------------------------------------------------------------- */

static void lookup(GtkWidget *w, const char *name, const char *fallback,
		   GdkRGBA *c)
{
	if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w), name,
					    c))
		gdk_rgba_parse(c, fallback);
}

static IvStyle current_style(IvEditor *ed)
{
	const IvEditState *st = iv_editor_state(ed);

	return st ? st->style : IV_STYLE_NONE;
}

static gboolean cell_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	int s = GPOINTER_TO_INT(data);
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	double side = MIN(W, H) - 4, x = (W - side) / 2, y = 2;
	gboolean chosen = S.ed && current_style(S.ed) == (IvStyle)s;
	GdkRGBA accent, dim;

	lookup(w, "likeos_shell_accent", "#5b8ff9", &accent);
	lookup(w, "likeos_shell_text_secondary", "#a4adbf", &dim);

	cairo_save(cr);
	iv_rounded_rect(cr, x, y, side, side, 8);
	cairo_clip(cr);
	if (S.thumbs[s]) {
		double tw = cairo_image_surface_get_width(S.thumbs[s]);
		double k = side / MAX(1.0, tw);

		cairo_save(cr);
		cairo_translate(cr, x, y);
		cairo_scale(cr, k, k);
		cairo_set_source_surface(cr, S.thumbs[s], 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		/* Not rendered yet: a quiet tile with the picture glyph. */
		cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
		cairo_paint(cr);
		cairo_set_source_rgba(cr, dim.red, dim.green, dim.blue, 0.5);
		iv_glyph_paint(cr, IV_GLYPH_IMAGE, x + side / 2, y + side / 2, 20);
	}
	cairo_restore(cr);

	/* A hairline, so dark previews keep their edge on the dark panel. */
	iv_rounded_rect(cr, x + 0.5, y + 0.5, side - 1, side - 1, 8);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	if (chosen) {
		double bx = x + side - 12, by = y + 12;

		iv_rounded_rect(cr, x + 1, y + 1, side - 2, side - 2, 7.5);
		gdk_cairo_set_source_rgba(cr, &accent);
		cairo_set_line_width(cr, 2.5);
		cairo_stroke(cr);
		/* The check mark in a disc at the top right corner. */
		cairo_arc(cr, bx, by, 9, 0, 2 * G_PI);
		gdk_cairo_set_source_rgba(cr, &accent);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		iv_glyph_paint(cr, IV_GLYPH_CHECK, bx, by, 12);
	}
	return TRUE;
}

static GtkWidget *cell_new(int s)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *area = gtk_drawing_area_new();
	GtkWidget *l = gtk_label_new(iv_style_name((IvStyle)s));

	gtk_widget_set_size_request(area, CELL_PX + 4, CELL_PX + 4);
	g_signal_connect(area, "draw", G_CALLBACK(cell_draw), GINT_TO_POINTER(s));
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
	iv_add_class(l, "iv-style-name");
	gtk_box_pack_start(GTK_BOX(box), area, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	S.cells[s] = area;
	return box;
}

/* ---- choosing a style ------------------------------------------------------------- */

static char *style_step(IvStyle s)
{
	return s == IV_STYLE_NONE ? g_strdup("No style") :
				    g_strdup_printf("Style %s", iv_style_name(s));
}

static gboolean key_rest_over(gpointer data)
{
	IvEditor *ed = data;
	char *label = style_step(current_style(ed));

	S.key_rest = 0;
	iv_editor_commit(ed, label);
	g_free(label);
	return G_SOURCE_REMOVE;
}

/* A choice made with the keys and not yet a step becomes one now. */
static void flush_key_rest(IvEditor *ed)
{
	if (!S.key_rest)
		return;
	g_source_remove(S.key_rest);
	key_rest_over(ed);
}

/* Shows `s' at once; `now' makes it a step at once, otherwise it becomes
 * one after a short rest (walking the grid with the keys). */
static void choose(IvEditor *ed, IvStyle s, gboolean now)
{
	IvEditState *st = iv_editor_begin(ed);

	if (st->style != s) {
		/* A new style starts at full strength. */
		st->style = s;
		st->style_intensity = s == IV_STYLE_NONE ? 0 : 1.0;
		iv_editor_changed(ed);
	}
	if (S.key_rest) {
		g_source_remove(S.key_rest);
		S.key_rest = 0;
	}
	if (now) {
		char *label = style_step(s);

		iv_editor_commit(ed, label);
		g_free(label);
	} else {
		S.key_rest = g_timeout_add(KEY_REST_MS, key_rest_over, ed);
	}
	sync_panel(ed);
}

static void child_activated(GtkFlowBox *box, GtkFlowBoxChild *child, IvEditor *ed)
{
	int s = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "iv-style"));

	if (S.syncing)
		return;
	choose(ed, (IvStyle)s, TRUE);
}

static void selection_changed(GtkFlowBox *box, IvEditor *ed)
{
	GList *sel;
	int s;

	if (S.syncing)
		return;
	sel = gtk_flow_box_get_selected_children(box);
	if (!sel)
		return;
	s = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel->data), "iv-style"));
	g_list_free(sel);
	if ((IvStyle)s != current_style(ed))
		choose(ed, (IvStyle)s, FALSE);
}

static void none_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_committed(ed)->style == IV_STYLE_NONE && !iv_editor_live(ed))
		return;
	choose(ed, IV_STYLE_NONE, TRUE);
}

/* The intensity slider: 0..100 % of the style. */
static double intensity_get(const IvEditState *s, gpointer data)
{
	return s->style == IV_STYLE_NONE ? 100 : s->style_intensity * 100;
}

static void intensity_set(IvEditState *s, double v, gpointer data)
{
	if (s->style != IV_STYLE_NONE)
		s->style_intensity = CLAMP(v / 100.0, 0, 1);
}

/* ---- Auto enhance ------------------------------------------------------------------- */

/* The fields Auto enhance sets; the rest of IvAdjust is left alone. */
#define AUTO_FIELDS(X)                                                          \
	X(brightness) X(exposure) X(contrast) X(highlights) X(shadows) X(whites) \
	X(blacks) X(saturation) X(vibrance) X(warmth) X(tint)

static void ensure_suggestion(IvEditor *ed)
{
	const IvImage *proxy = iv_editor_proxy(ed);

	check_picture(ed);
	if (S.have_suggestion || !proxy)
		return;
	iv_auto_enhance(proxy, &S.suggestion);
	S.have_suggestion = TRUE;
}

static gboolean suggestion_neutral(void)
{
#define ZERO(f) if (fabs(S.suggestion.f) > 0.5) return FALSE;
	AUTO_FIELDS(ZERO)
#undef ZERO
	return TRUE;
}

/* The sliders hold exactly what Auto enhance suggests. */
static gboolean auto_is_on(const IvAdjust *a)
{
	if (!S.have_suggestion || suggestion_neutral())
		return FALSE;
#define SAME(f) if (fabs(a->f - S.suggestion.f) > 1e-6) return FALSE;
	AUTO_FIELDS(SAME)
#undef SAME
	return TRUE;
}

static void auto_undo(gpointer data)
{
	IvEditor *ed = data;

	/* Only while Auto enhance is still the last step. */
	if (iv_editor_committed(ed) == S.auto_state && !iv_editor_live(ed))
		iv_editor_undo(ed);
}

static void auto_clicked(GtkButton *b, IvEditor *ed)
{
	IvEditState *s;

	if (!iv_editor_proxy(ed))
		return;
	flush_key_rest(ed);
	ensure_suggestion(ed);
	if (iv_editor_live(ed))
		iv_editor_commit(ed, "Edit");
	s = iv_edit_state_copy(iv_editor_committed(ed));
	if (auto_is_on(&s->adjust)) {
		/* Pressed again: off. */
#define OFF(f) s->adjust.f = 0;
		AUTO_FIELDS(OFF)
#undef OFF
		iv_editor_apply(ed, s, "Auto enhance off");
		iv_editor_toast(ed, "Auto enhance removed");
		return;
	}
	if (suggestion_neutral()) {
		iv_edit_state_free(s);
		iv_editor_toast(ed, "The picture is well balanced already");
		return;
	}
#define SET(f) s->adjust.f = round(S.suggestion.f);
	AUTO_FIELDS(SET)
#undef SET
	/* Rounded like the sliders show them, so Adjust shows exactly the
	 * values that are used. */
	S.suggestion = s->adjust;
	iv_editor_apply(ed, s, "Auto enhance");
	S.auto_state = iv_editor_committed(ed);
	iv_editor_toast_action(ed, "Enhanced — the values are on the Adjust "
				   "sliders",
			       "Undo", auto_undo, ed);
}

/* ---- the panel ------------------------------------------------------------------ */

static void sync_panel(IvEditor *ed)
{
	const IvEditState *st = iv_editor_state(ed);
	IvStyle s;
	GtkWidget *l;

	if (!S.panel || !st)
		return;
	s = st->style;
	S.syncing = TRUE;
	if ((int)s >= 0 && s < IV_STYLE_COUNT && S.children[s]) {
		GList *sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(S.grid));
		gboolean already = sel && sel->data == S.children[s];

		g_list_free(sel);
		if (!already)
			gtk_flow_box_select_child(GTK_FLOW_BOX(S.grid),
						  GTK_FLOW_BOX_CHILD(S.children[s]));
	}
	S.syncing = FALSE;
	iv_panel_slider_sync(S.intensity);
	gtk_widget_set_sensitive(S.intensity, s != IV_STYLE_NONE);
	if (s == IV_STYLE_NONE)
		gtk_label_set_text(GTK_LABEL(S.intensity_note),
				   "Choose a style to set how strong it is.");
	else {
		char *t = g_strdup_printf("How much of “%s” is mixed in.",
					  iv_style_name(s));

		gtk_label_set_text(GTK_LABEL(S.intensity_note), t);
		g_free(t);
	}
	for (int i = 0; i < IV_STYLE_COUNT; i++)
		if (S.cells[i])
			gtk_widget_queue_draw(S.cells[i]);

	/* Auto enhance: on or off. */
	l = iv_text_button_label(S.auto_button);
	if (S.have_suggestion && auto_is_on(&st->adjust)) {
		gtk_label_set_text(GTK_LABEL(l), "Auto enhance is on");
		iv_glyph_button_set(S.auto_button, IV_GLYPH_CHECK);
		iv_add_class(S.auto_button, "iv-on");
		gtk_widget_set_tooltip_text(S.auto_button, "Press to take the "
							   "automatic correction "
							   "off");
	} else {
		gtk_label_set_text(GTK_LABEL(l), "Auto enhance");
		iv_glyph_button_set(S.auto_button, IV_GLYPH_WAND);
		iv_remove_class(S.auto_button, "iv-on");
		gtk_widget_set_tooltip_text(S.auto_button,
					    "Levels, contrast and white balance "
					    "from the picture's own tones");
	}
}

static GtkWidget *section(GtkWidget *p, IvGlyph g, const char *title,
			  GtkWidget **reset)
{
	GtkWidget *row = iv_panel_section(p, title, reset);
	GtkWidget *glyph = iv_glyph_new(g, 14);

	iv_add_class(glyph, "iv-section-glyph");
	gtk_box_pack_start(GTK_BOX(row), glyph, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(row), glyph, 0);
	return row;
}

static GtkWidget *build_panel(IvEditor *ed)
{
	GtkWidget *p = iv_panel_new("Styles", "Looks for the whole picture.");
	GtkWidget *reset = NULL;

	S.ed = ed;
	S.panel = p;
	iv_add_class(p, "iv-styles-panel");

	S.auto_button = iv_panel_button(IV_GLYPH_WAND, "Auto enhance");
	iv_add_class(S.auto_button, "iv-auto-enhance");
	g_signal_connect(S.auto_button, "clicked", G_CALLBACK(auto_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), S.auto_button, FALSE, FALSE, 0);
	S.auto_note = iv_panel_note("Sets light and colour from the picture's "
				    "own tones; Adjust shows what it chose.");
	gtk_box_pack_start(GTK_BOX(p), S.auto_note, FALSE, FALSE, 0);

	section(p, IV_GLYPH_STYLES, "STYLES", &reset);
	gtk_widget_set_tooltip_text(reset, "No style");
	g_signal_connect(reset, "clicked", G_CALLBACK(none_clicked), ed);

	S.grid = gtk_flow_box_new();
	iv_add_class(S.grid, "iv-style-grid");
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(S.grid), GTK_SELECTION_SINGLE);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(S.grid), TRUE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(S.grid), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(S.grid), 3);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(S.grid), 3);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(S.grid), 4);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(S.grid), 2);
	for (int s = 0; s < IV_STYLE_COUNT; s++) {
		GtkWidget *child = gtk_flow_box_child_new();

		gtk_container_add(GTK_CONTAINER(child), cell_new(s));
		g_object_set_data(G_OBJECT(child), "iv-style", GINT_TO_POINTER(s));
		atk_object_set_name(gtk_widget_get_accessible(child),
				    iv_style_name((IvStyle)s));
		S.children[s] = child;
		gtk_container_add(GTK_CONTAINER(S.grid), child);
	}
	g_signal_connect(S.grid, "child-activated", G_CALLBACK(child_activated), ed);
	g_signal_connect(S.grid, "selected-children-changed",
			 G_CALLBACK(selection_changed), ed);
	gtk_box_pack_start(GTK_BOX(p), S.grid, FALSE, FALSE, 0);

	S.intensity = iv_panel_slider(ed, "Intensity", 0, 100, 1, 100, intensity_get,
				      intensity_set, NULL);
	gtk_range_set_increments(GTK_RANGE(g_object_get_data(G_OBJECT(S.intensity),
							     "iv-scale")),
				 1, 10);
	iv_add_class(S.intensity, "iv-style-intensity");
	gtk_box_pack_start(GTK_BOX(p), S.intensity, FALSE, FALSE, 0);
	S.intensity_note = iv_panel_note("");
	gtk_box_pack_start(GTK_BOX(p), S.intensity_note, FALSE, FALSE, 0);

	sync_panel(ed);
	return p;
}

/* ---- the tool ----------------------------------------------------------------------- */

static void activate(IvEditor *ed)
{
	S.ed = ed;
	check_picture(ed);
	ensure_suggestion(ed);
	sync_panel(ed);
	schedule_refresh(ed);
}

static void deactivate(IvEditor *ed)
{
	flush_key_rest(ed);
	if (S.refresh) {
		g_source_remove(S.refresh);
		S.refresh = 0;
	}
}

static void state_changed(IvEditor *ed)
{
	if (!iv_editor_live(ed) && S.key_rest) {
		/* Undone in the middle of walking the grid. */
		g_source_remove(S.key_rest);
		S.key_rest = 0;
	}
	sync_panel(ed);
	schedule_refresh(ed);
}

const IvTool iv_tool_styles = {
	.id = "styles",
	.label = "Styles",
	.glyph = IV_GLYPH_STYLES,
	.tooltip = "Styles (S)",
	.key = GDK_KEY_s,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.state_changed = state_changed,
};
