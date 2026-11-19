// LikeOS -- LikeOS Image Viewer: the editor.
//
// The shell round the tools: the top bar, the tool rail, the canvas with
// the rendered preview, the options panel, undo and redo, the preview
// renders in a worker thread, saving at full size, the guard against
// leaving with unsaved edits, and the building blocks the tools make their
// panels of.  See editor.h for the interface the tools use.
//
// Renders never pile up: while one runs, a newer request only marks that
// another is wanted, and when the running one finishes the newest state is
// rendered next.  Each render works on a copy of the state, so the tools
// can go on changing theirs meanwhile.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>
#include <unistd.h>

#include "editor.h"

static const IvTool *const tools[] = {
	&iv_tool_crop,	  &iv_tool_adjust,  &iv_tool_styles,
	&iv_tool_draw,	  &iv_tool_retouch, &iv_tool_background,
};
#define N_TOOLS ((int)G_N_ELEMENTS(tools))

/* An image shared between the editor and its worker threads. */
typedef struct {
	gint ref;
	IvImage *img;
} Shared;

static Shared *shared_new(IvImage *img)
{
	Shared *s = g_new0(Shared, 1);

	s->ref = 1;
	s->img = img;
	return s;
}

static Shared *shared_ref(Shared *s)
{
	if (s)
		g_atomic_int_inc(&s->ref);
	return s;
}

static void shared_unref(Shared *s)
{
	if (!s || !g_atomic_int_dec_and_test(&s->ref))
		return;
	iv_image_free(s->img);
	g_free(s);
}

/* The render cache, shared the same way. */
typedef struct {
	gint ref;
	IvRenderCache *cache;
} SharedCache;

static SharedCache *cache_new(void)
{
	SharedCache *c = g_new0(SharedCache, 1);

	c->ref = 1;
	c->cache = iv_render_cache_new();
	return c;
}

static SharedCache *cache_ref(SharedCache *c)
{
	if (c)
		g_atomic_int_inc(&c->ref);
	return c;
}

static void cache_unref(SharedCache *c)
{
	if (!c || !g_atomic_int_dec_and_test(&c->ref))
		return;
	iv_render_cache_free(c->cache);
	g_free(c);
}

typedef enum { SAVE_OVERWRITE, SAVE_COPY, SAVE_CLIPBOARD } SaveMode;

struct IvEditor {
	IvWindow *win;
	GtkWidget *root;

	/* The top bar. */
	GtkWidget *b_zoom_in, *b_zoom_out, *b_fit, *b_zoom_pct;
	GtkWidget *b_undo, *b_redo, *b_revert, *b_compare;
	GtkWidget *b_done, *b_done_menu, *b_discard;

	/* The rail, the canvas, the panel. */
	GtkWidget *rail_buttons[G_N_ELEMENTS(tools)];
	GtkWidget *overlay;
	IvCanvas *canvas;
	IvToast *toast;
	GtkWidget *panel_card, *panel_stack;
	GtkWidget *panels[G_N_ELEMENTS(tools)];

	/* What is edited. */
	IvPicture *pic;
	char *path, *name, *title;
	Shared *source, *proxy;
	double proxy_scale;
	int sw, sh;
	gboolean ready;
	guint open_gen;

	/* The state. */
	IvEditState *committed, *live, *saved;
	IvHistory *history;
	int tool, last_tool;
	IvRenderFlags flags;
	gboolean compare;

	/* Rendering. */
	SharedCache *cache;
	gboolean render_running, render_pending;
	gboolean first_render;
	GCancellable *render_cancel;
	cairo_surface_t *shown;		/* the last render */
	int shown_w, shown_h;
	cairo_surface_t *original;	/* the proxy, for Compare */
	GArray *rendered_hooks;		/* RenderedHook, called per render */
	const char *render_failed;	/* the step the last preview lacked */

	/* Slow work and saving. */
	gboolean busy;
	GCancellable *job_cancel;
	gboolean saving;
	IvEditorLeave after_save;
	gpointer after_save_data;
};

static void update_commands(IvEditor *ed);
static void request_render(IvEditor *ed);
static void notify_tools(IvEditor *ed);
static void select_tool(IvEditor *ed, int index);
static void save(IvEditor *ed, SaveMode mode);

/* ---- small things ----------------------------------------------------------- */

const char *iv_editor_title(IvEditor *ed)
{
	return ed->title ? ed->title : "Editing";
}

GtkWindow *iv_editor_window(IvEditor *ed)
{
	return iv_window_gtk(ed->win);
}

IvCanvas *iv_editor_canvas(IvEditor *ed)
{
	return ed->canvas;
}

const char *iv_editor_path(IvEditor *ed)
{
	return ed->path;
}

const IvImage *iv_editor_source(IvEditor *ed)
{
	return ed->source ? ed->source->img : NULL;
}

const IvImage *iv_editor_proxy(IvEditor *ed)
{
	return ed->proxy ? ed->proxy->img : NULL;
}

double iv_editor_proxy_scale(IvEditor *ed)
{
	return ed->proxy_scale > 0 ? ed->proxy_scale : 1.0;
}

void iv_editor_source_size(IvEditor *ed, int *w, int *h)
{
	if (w)
		*w = ed->sw;
	if (h)
		*h = ed->sh;
}

const IvTool *iv_editor_current_tool(IvEditor *ed)
{
	return ed->tool >= 0 && ed->tool < N_TOOLS ? tools[ed->tool] : NULL;
}

gboolean iv_editor_busy(IvEditor *ed)
{
	return ed->busy || ed->saving;
}

gboolean iv_editor_dirty(IvEditor *ed)
{
	if (!ed->ready || !ed->committed)
		return FALSE;
	if (ed->live && !iv_edit_state_equal(ed->live, ed->committed))
		return TRUE;
	return !iv_edit_state_equal(ed->committed, ed->saved);
}

/* ---- the state ------------------------------------------------------------- */

const IvEditState *iv_editor_state(IvEditor *ed)
{
	return ed->live ? ed->live : ed->committed;
}

const IvEditState *iv_editor_committed(IvEditor *ed)
{
	return ed->committed;
}

gboolean iv_editor_live(IvEditor *ed)
{
	return ed->live != NULL;
}

IvEditState *iv_editor_begin(IvEditor *ed)
{
	if (!ed->live)
		ed->live = iv_edit_state_copy(ed->committed);
	return ed->live;
}

void iv_editor_changed(IvEditor *ed)
{
	request_render(ed);
	iv_editor_redraw_overlay(ed);
	update_commands(ed);
}

static void state_replaced(IvEditor *ed)
{
	request_render(ed);
	notify_tools(ed);
	iv_editor_redraw_overlay(ed);
	update_commands(ed);
}

void iv_editor_commit(IvEditor *ed, const char *label)
{
	IvEditState *before;

	if (!ed->live)
		return;
	if (iv_edit_state_equal(ed->live, ed->committed)) {
		iv_edit_state_free(ed->live);
		ed->live = NULL;
		update_commands(ed);
		return;
	}
	before = ed->committed;
	ed->committed = ed->live;
	ed->live = NULL;
	iv_history_push(ed->history, before, label);
	update_commands(ed);
}

void iv_editor_cancel(IvEditor *ed)
{
	if (!ed->live)
		return;
	iv_edit_state_free(ed->live);
	ed->live = NULL;
	state_replaced(ed);
}

void iv_editor_apply(IvEditor *ed, IvEditState *s, const char *label)
{
	if (!s)
		return;
	if (ed->live) {
		iv_edit_state_free(ed->live);
		ed->live = NULL;
	}
	if (iv_edit_state_equal(s, ed->committed)) {
		iv_edit_state_free(s);
		state_replaced(ed);
		return;
	}
	iv_history_push(ed->history, ed->committed, label);
	ed->committed = s;
	state_replaced(ed);
}

void iv_editor_undo(IvEditor *ed)
{
	IvEditState *s;

	if (!ed->ready || iv_editor_busy(ed))
		return;
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	s = iv_history_undo(ed->history, ed->committed);
	if (!s)
		return;
	ed->committed = s;
	state_replaced(ed);
}

void iv_editor_redo(IvEditor *ed)
{
	IvEditState *s;

	if (!ed->ready || iv_editor_busy(ed))
		return;
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	s = iv_history_redo(ed->history, ed->committed);
	if (!s)
		return;
	ed->committed = s;
	state_replaced(ed);
}

static void revert_answered(gboolean yes, gpointer data)
{
	IvEditor *ed = data;

	if (!yes)
		return;
	/* A step of its own, so Revert can be undone too. */
	iv_editor_apply(ed, iv_edit_state_new(), "Revert");
	iv_editor_toast(ed, "All edits reverted");
}

void iv_editor_revert(IvEditor *ed)
{
	if (!ed->ready || iv_editor_busy(ed))
		return;
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	if (iv_edit_state_is_identity(ed->committed))
		return;
	iv_confirm(GTK_OVERLAY(ed->overlay), "Revert all edits?",
		   "Every change made to this picture since it was opened is "
		   "removed.  Revert can itself be undone.",
		   "_Revert", "_Keep editing", TRUE, revert_answered, ed);
}

static void notify_tools(IvEditor *ed)
{
	for (int i = 0; i < N_TOOLS; i++)
		if (tools[i]->state_changed && (ed->panels[i] || i == ed->tool))
			tools[i]->state_changed(ed);
}

/* ---- the display ------------------------------------------------------------- */

void iv_editor_set_render_flags(IvEditor *ed, IvRenderFlags flags)
{
	if (ed->flags == flags)
		return;
	ed->flags = flags;
	request_render(ed);
}

IvRenderFlags iv_editor_render_flags(IvEditor *ed)
{
	return ed->flags;
}

void iv_editor_display_size(IvEditor *ed, int *w, int *h)
{
	iv_render_display_size(iv_editor_state(ed), ed->sw, ed->sh, ed->flags, w,
			       h);
}

void iv_editor_display_to_source(IvEditor *ed, double dx, double dy,
				 double *sx, double *sy)
{
	iv_render_display_to_source(iv_editor_state(ed), ed->sw, ed->sh,
				    ed->flags, dx, dy, sx, sy);
}

void iv_editor_source_to_display(IvEditor *ed, double sx, double sy,
				 double *dx, double *dy)
{
	iv_render_source_to_display(iv_editor_state(ed), ed->sw, ed->sh,
				    ed->flags, sx, sy, dx, dy);
}

void iv_editor_display_to_output(IvEditor *ed, double dx, double dy,
				 double *ox, double *oy)
{
	iv_render_display_to_output(iv_editor_state(ed), ed->sw, ed->sh,
				    ed->flags, dx, dy, ox, oy);
}

void iv_editor_output_to_display(IvEditor *ed, double ox, double oy,
				 double *dx, double *dy)
{
	iv_render_output_to_display(iv_editor_state(ed), ed->sw, ed->sh,
				    ed->flags, ox, oy, dx, dy);
}

/* The canvas's logical size is that of the last render shown; while a
 * newer render is on its way the display frame may already differ, so
 * the widget mapping scales between the two. */
void iv_editor_display_to_widget(IvEditor *ed, double dx, double dy,
				 double *wx, double *wy)
{
	int dw, dh, lw, lh;

	iv_editor_display_size(ed, &dw, &dh);
	iv_canvas_get_image_size(ed->canvas, &lw, &lh);
	if (lw > 0 && dw > 0 && (lw != dw || lh != dh)) {
		dx *= (double)lw / dw;
		dy *= (double)lh / dh;
	}
	iv_canvas_image_to_widget(ed->canvas, dx, dy, wx, wy);
}

void iv_editor_widget_to_display(IvEditor *ed, double wx, double wy,
				 double *dx, double *dy)
{
	int dw, dh, lw, lh;

	iv_canvas_widget_to_image(ed->canvas, wx, wy, dx, dy);
	iv_editor_display_size(ed, &dw, &dh);
	iv_canvas_get_image_size(ed->canvas, &lw, &lh);
	if (lw > 0 && dw > 0 && (lw != dw || lh != dh)) {
		*dx *= (double)dw / lw;
		*dy *= (double)dh / lh;
	}
}

double iv_editor_view_scale(IvEditor *ed)
{
	return iv_canvas_get_zoom(ed->canvas);
}

void iv_editor_redraw_overlay(IvEditor *ed)
{
	gtk_widget_queue_draw(IV_CANVAS_GTK(ed->canvas));
}

void iv_editor_set_cursor(IvEditor *ed, const char *name)
{
	iv_canvas_set_cursor_name(ed->canvas, name);
}

void iv_editor_set_drag_pan(IvEditor *ed, gboolean on)
{
	iv_canvas_set_drag_pan(ed->canvas, on);
}

static void show_current(IvEditor *ed, gboolean keep_view)
{
	if (ed->compare && ed->original)
		iv_canvas_set_surface(ed->canvas, ed->original, ed->sw, ed->sh,
				      TRUE);
	else if (ed->shown)
		iv_canvas_set_surface(ed->canvas, ed->shown, ed->shown_w,
				      ed->shown_h, keep_view);
}

void iv_editor_set_compare(IvEditor *ed, gboolean original)
{
	if (ed->compare == original || !ed->ready)
		return;
	ed->compare = original;
	iv_set_class(ed->b_compare, "iv-pressed", original);
	show_current(ed, TRUE);
}

/* ---- rendering ------------------------------------------------------------------ */

typedef struct {
	IvEditor *ed;
	guint gen;
	Shared *src;
	SharedCache *cache;
	IvEditState *state;
	IvRenderFlags flags;
	int sw, sh, dw, dh;
	GCancellable *cancel;
	cairo_surface_t *result;
	const char *failed;	/* a step that ran out of memory */
} RenderJob;

static void render_job_free(gpointer p)
{
	RenderJob *j = p;

	shared_unref(j->src);
	cache_unref(j->cache);
	iv_edit_state_free(j->state);
	g_clear_object(&j->cancel);
	if (j->result)
		cairo_surface_destroy(j->result);
	g_free(j);
}

static void render_thread(GTask *task, gpointer src, gpointer data,
			  GCancellable *c)
{
	RenderJob *j = data;
	IvRenderOutcome out;
	IvImage *img = iv_render_ex(j->src->img, j->sw, j->sh, j->state, j->flags,
				    j->cache->cache, j->cancel, NULL, NULL, &out);

	if (img) {
		j->result = iv_surface_from_image(img);
		iv_image_free(img);
		if (!j->result)
			j->failed = "Your edits";
	} else {
		j->failed = out.failed;
	}
	g_task_return_boolean(task, j->result != NULL);
}

static void start_render(IvEditor *ed);

/* The tools that follow the preview hear of every new render. */
typedef struct {
	IvEditorRendered cb;
	gpointer data;
} RenderedHook;

void iv_editor_add_rendered_hook(IvEditor *ed, IvEditorRendered cb,
				 gpointer data)
{
	RenderedHook h = { cb, data };

	if (!ed->rendered_hooks)
		ed->rendered_hooks = g_array_new(FALSE, FALSE, sizeof h);
	g_array_append_val(ed->rendered_hooks, h);
}

cairo_surface_t *iv_editor_shown_surface(IvEditor *ed, int *logical_w,
					 int *logical_h)
{
	cairo_surface_t *s = ed->ready ? ed->shown : NULL;

	if (logical_w)
		*logical_w = s ? ed->shown_w : 0;
	if (logical_h)
		*logical_h = s ? ed->shown_h : 0;
	return s;
}

static void notify_rendered(IvEditor *ed)
{
	for (guint i = 0; ed->rendered_hooks && i < ed->rendered_hooks->len; i++) {
		RenderedHook *h = &g_array_index(ed->rendered_hooks, RenderedHook, i);

		h->cb(ed, h->data);
	}
}

static void render_done(GObject *o, GAsyncResult *res, gpointer data)
{
	RenderJob *j = g_task_get_task_data(G_TASK(res));
	IvEditor *ed = j->ed;

	if (j->gen != ed->open_gen) {
		/* The editor moved on to another picture meanwhile. */
		return;
	}
	ed->render_running = FALSE;
	if (j->result && !g_cancellable_is_cancelled(j->cancel)) {
		if (ed->shown)
			cairo_surface_destroy(ed->shown);
		ed->shown = cairo_surface_reference(j->result);
		ed->shown_w = j->dw;
		ed->shown_h = j->dh;
		if (!ed->compare)
			show_current(ed, !ed->first_render);
		ed->first_render = FALSE;
		iv_canvas_set_loading(ed->canvas, FALSE);
		ed->render_failed = NULL;
		notify_rendered(ed);
	} else if (j->failed && !g_cancellable_is_cancelled(j->cancel) &&
		   j->failed != ed->render_failed) {
		/* The canvas keeps the last picture it could show; say why
		 * it is not the newest (once, not on every slider step). */
		char *what = g_strdup(j->failed);
		char *msg;

		/* "Retouching" as the object of the sentence. */
		what[0] = g_ascii_tolower(what[0]);
		msg = g_strdup_printf("The preview could not be updated: not "
				      "enough memory for %s",
				      what);
		g_free(what);

		ed->render_failed = j->failed;
		iv_canvas_set_loading(ed->canvas, FALSE);
		iv_editor_toast(ed, msg);
		g_free(msg);
	}
	if (ed->render_pending) {
		ed->render_pending = FALSE;
		start_render(ed);
	}
}

static void start_render(IvEditor *ed)
{
	RenderJob *j = g_new0(RenderJob, 1);
	GTask *t;

	j->ed = ed;
	j->gen = ed->open_gen;
	j->src = shared_ref(ed->proxy);
	j->cache = cache_ref(ed->cache);
	j->state = iv_edit_state_copy(iv_editor_state(ed));
	j->flags = ed->flags;
	j->sw = ed->sw;
	j->sh = ed->sh;
	iv_render_display_size(j->state, j->sw, j->sh, j->flags, &j->dw, &j->dh);
	j->cancel = g_object_ref(ed->render_cancel);
	ed->render_running = TRUE;
	t = g_task_new(NULL, NULL, render_done, NULL);
	g_task_set_task_data(t, j, render_job_free);
	g_task_run_in_thread(t, render_thread);
	g_object_unref(t);
}

static void request_render(IvEditor *ed)
{
	if (!ed->ready)
		return;
	if (ed->render_running) {
		ed->render_pending = TRUE;
		return;
	}
	start_render(ed);
}

/* ---- the canvas hooks ------------------------------------------------------------- */

static void canvas_overlay(IvCanvas *c, cairo_t *cr, gpointer data)
{
	IvEditor *ed = data;
	const IvTool *t = iv_editor_current_tool(ed);

	if (ed->ready && !ed->compare && t && t->draw_overlay)
		t->draw_overlay(ed, cr);
}

static gboolean canvas_event(IvCanvas *c, GdkEvent *ev, double ix, double iy,
			     gpointer data)
{
	IvEditor *ed = data;
	const IvTool *t = iv_editor_current_tool(ed);
	double dx = ix, dy = iy;
	int dw, dh, lw, lh;

	if (!ed->ready || ed->compare || iv_editor_busy(ed) || !t ||
	    !t->canvas_event)
		return FALSE;
	/* Canvas image coordinates are those of the render shown; the
	 * tool gets the current DISPLAY frame. */
	iv_editor_display_size(ed, &dw, &dh);
	iv_canvas_get_image_size(c, &lw, &lh);
	if (lw > 0 && dw > 0 && (lw != dw || lh != dh)) {
		dx *= (double)dw / lw;
		dy *= (double)dh / lh;
	}
	return t->canvas_event(ed, ev, dx, dy);
}

static void view_changed(IvCanvas *c, IvEditor *ed)
{
	char *s = g_strdup_printf("%d %%", (int)lround(iv_canvas_get_zoom(c) * 100));
	GtkWidget *l = iv_text_button_label(ed->b_zoom_pct);

	gtk_label_set_text(GTK_LABEL(l), s);
	g_free(s);
}

/* ---- the tools ------------------------------------------------------------------- */

static void reset_tool_defaults(IvEditor *ed)
{
	ed->flags = IV_RENDER_ALL;
	iv_canvas_set_cursor_name(ed->canvas, NULL);
	iv_canvas_set_drag_pan(ed->canvas, TRUE);
}

static void select_tool(IvEditor *ed, int index)
{
	IvRenderFlags before = ed->flags;
	const IvTool *t;

	if (index < 0 || index >= N_TOOLS)
		return;
	if (index == ed->tool && ed->panels[index])
		return;
	if (ed->tool >= 0 && ed->tool < N_TOOLS) {
		const IvTool *old = tools[ed->tool];

		if (old->deactivate)
			old->deactivate(ed);
		/* No edit is left open across tools. */
		if (ed->live)
			iv_editor_commit(ed, old->label);
		iv_remove_class(ed->rail_buttons[ed->tool], "active");
	}
	ed->tool = index;
	t = tools[index];
	iv_add_class(ed->rail_buttons[index], "active");
	if (!ed->panels[index]) {
		ed->panels[index] = t->build_panel(ed);
		gtk_widget_show_all(ed->panels[index]);
		gtk_stack_add_named(GTK_STACK(ed->panel_stack), ed->panels[index],
				    t->id);
	}
	gtk_stack_set_visible_child(GTK_STACK(ed->panel_stack), ed->panels[index]);
	reset_tool_defaults(ed);
	if (t->activate)
		t->activate(ed);
	if (t->state_changed)
		t->state_changed(ed);
	if (ed->flags != before)
		request_render(ed);
	else
		iv_editor_redraw_overlay(ed);
}

static void rail_clicked(GtkButton *b, IvEditor *ed)
{
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "iv-tool"));

	if (!iv_editor_busy(ed))
		select_tool(ed, i);
}

/* ---- slow work ---------------------------------------------------------------------- */

typedef struct {
	IvEditor *ed;
	guint gen;
	IvEditorWork work;
	IvEditorDone done;
	gpointer data;
	GDestroyNotify free_data;
	GCancellable *cancel;
	char *label;
	GMutex lock;
	double fraction;
	guint poll;
} Job;

static int job_progress(double f, void *data)
{
	Job *j = data;

	g_mutex_lock(&j->lock);
	j->fraction = f;
	g_mutex_unlock(&j->lock);
	return g_cancellable_is_cancelled(j->cancel) ? 1 : 0;
}

static gboolean job_poll(gpointer data)
{
	Job *j = data;
	double f;

	g_mutex_lock(&j->lock);
	f = j->fraction;
	g_mutex_unlock(&j->lock);
	if (j->gen == j->ed->open_gen)
		iv_toast_progress(j->ed->toast,
				  g_cancellable_is_cancelled(j->cancel) ?
					  "Cancelling…" :
					  j->label,
				  f);
	return G_SOURCE_CONTINUE;
}

/* The progress toast's Cancel (added for the slow tools: Esc does the
 * same, the button says so). */
static void job_cancel_clicked(gpointer data)
{
	IvEditor *ed = data;

	if (ed->busy && ed->job_cancel)
		g_cancellable_cancel(ed->job_cancel);
}

static void job_thread(GTask *task, gpointer src, gpointer data,
		       GCancellable *c)
{
	Job *j = data;

	j->work(j->data, j->cancel, job_progress, j);
	g_task_return_boolean(task, !g_cancellable_is_cancelled(j->cancel));
}

static void job_free(gpointer p)
{
	Job *j = p;

	if (j->poll)
		g_source_remove(j->poll);
	if (j->free_data)
		j->free_data(j->data);
	g_clear_object(&j->cancel);
	g_mutex_clear(&j->lock);
	g_free(j->label);
	g_free(j);
}

static void job_done(GObject *o, GAsyncResult *res, gpointer data)
{
	Job *j = g_task_get_task_data(G_TASK(res));
	IvEditor *ed = j->ed;
	gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);

	if (j->poll) {
		g_source_remove(j->poll);
		j->poll = 0;
	}
	if (j->gen != ed->open_gen) {
		if (j->done)
			j->done(j->data, FALSE);
		return;
	}
	ed->busy = FALSE;
	g_clear_object(&ed->job_cancel);
	iv_toast_hide(ed->toast);
	if (j->done)
		j->done(j->data, ok);
	update_commands(ed);
}

void iv_editor_run(IvEditor *ed, const char *label, IvEditorWork work,
		   IvEditorDone done, gpointer data, GDestroyNotify free_data)
{
	Job *j;
	GTask *t;

	if (ed->busy) {
		if (done)
			done(data, FALSE);
		if (free_data)
			free_data(data);
		return;
	}
	j = g_new0(Job, 1);
	j->ed = ed;
	j->gen = ed->open_gen;
	j->work = work;
	j->done = done;
	j->data = data;
	j->free_data = free_data;
	j->cancel = g_cancellable_new();
	j->label = g_strdup(label ? label : "Working");
	g_mutex_init(&j->lock);
	ed->busy = TRUE;
	ed->job_cancel = g_object_ref(j->cancel);
	iv_toast_progress_action(ed->toast, j->label, 0, "Cancel",
				 job_cancel_clicked, ed);
	j->poll = g_timeout_add(120, job_poll, j);
	update_commands(ed);
	t = g_task_new(NULL, NULL, job_done, NULL);
	g_task_set_task_data(t, j, job_free);
	g_task_run_in_thread(t, job_thread);
	g_object_unref(t);
}

/* iv_editor_run_render (added for the Retouch tool): a job whose work is a
 * render of a state over the proxy into the editor's own render cache.
 * The job holds the proxy and the cache, so neither goes while it runs,
 * whatever happens to the editor meanwhile. */
typedef struct {
	Shared *src;
	SharedCache *cache;
	IvEditState *state;
	int sw, sh;
	gboolean rendered;
	IvEditorDone done;
	gpointer data;
	GDestroyNotify free_data;
} RenderAhead;

static void render_ahead_work(gpointer data, GCancellable *cancel,
			      IvProgress progress, void *progress_data)
{
	RenderAhead *r = data;
	/* Only the steps on the source pixels are wanted here (the slow
	 * ones, which the cache keeps); colour, background, geometry and
	 * drawing are left to the preview render. */
	IvImage *img = iv_render(r->src->img, r->sw, r->sh, r->state,
				 IV_RENDER_NO_GEOMETRY | IV_RENDER_NO_COLOR |
					 IV_RENDER_NO_BACKGROUND,
				 r->cache->cache, cancel, progress, progress_data);

	if (img) {
		r->rendered = !g_cancellable_is_cancelled(cancel);
		iv_image_free(img);
	}
}

static void render_ahead_done(gpointer data, gboolean ok)
{
	RenderAhead *r = data;

	if (r->done)
		r->done(r->data, ok && r->rendered);
}

static void render_ahead_free(gpointer data)
{
	RenderAhead *r = data;

	shared_unref(r->src);
	cache_unref(r->cache);
	iv_edit_state_free(r->state);
	if (r->free_data)
		r->free_data(r->data);
	g_free(r);
}

void iv_editor_run_render(IvEditor *ed, const char *label,
			  const IvEditState *s, IvEditorDone done,
			  gpointer data, GDestroyNotify free_data)
{
	RenderAhead *r;

	if (!ed->ready || !ed->proxy || !ed->cache || !s) {
		if (done)
			done(data, FALSE);
		if (free_data)
			free_data(data);
		return;
	}
	r = g_new0(RenderAhead, 1);
	r->src = shared_ref(ed->proxy);
	r->cache = cache_ref(ed->cache);
	r->state = iv_edit_state_copy(s);
	r->sw = ed->sw;
	r->sh = ed->sh;
	r->done = done;
	r->data = data;
	r->free_data = free_data;
	iv_editor_run(ed, label, render_ahead_work, render_ahead_done, r,
		      render_ahead_free);
}

/* ---- messages ----------------------------------------------------------------------- */

void iv_editor_toast(IvEditor *ed, const char *text)
{
	iv_toast_show(ed->toast, text, NULL, NULL, NULL, NULL, 0);
}

void iv_editor_toast_action(IvEditor *ed, const char *text, const char *action,
			    IvToastAction cb, gpointer data)
{
	iv_toast_show(ed->toast, text, action, cb, data, NULL, 0);
}

void iv_editor_confirm(IvEditor *ed, const char *title, const char *text,
		       const char *accept, const char *cancel,
		       gboolean destructive, IvConfirmDone done, gpointer data)
{
	iv_confirm(GTK_OVERLAY(ed->overlay), title, text, accept, cancel,
		   destructive, done, data);
}

/* ---- saving ------------------------------------------------------------------------- */

typedef struct {
	IvEditor *ed;
	guint gen;
	SaveMode mode;
	char *path;
	IvSaveOptions opt;
	Shared *src;
	IvEditState *state;
	int sw, sh;
	GCancellable *cancel;
	GMutex lock;
	double fraction;
	guint poll;
	GdkPixbuf *result;
	gboolean preview;	/* rendered over the proxy: preview size */
	const char *failed;	/* a step that ran out of memory */
} SaveJob;

static void save_job_free(SaveJob *j)
{
	if (j->poll)
		g_source_remove(j->poll);
	g_free(j->path);
	iv_save_options_clear(&j->opt);
	shared_unref(j->src);
	iv_edit_state_free(j->state);
	g_clear_object(&j->cancel);
	g_clear_object(&j->result);
	g_mutex_clear(&j->lock);
	g_free(j);
}

static int save_progress(double f, void *data)
{
	SaveJob *j = data;

	g_mutex_lock(&j->lock);
	j->fraction = f;
	g_mutex_unlock(&j->lock);
	return g_cancellable_is_cancelled(j->cancel) ? 1 : 0;
}

static gboolean save_poll(gpointer data)
{
	SaveJob *j = data;
	double f;

	g_mutex_lock(&j->lock);
	f = j->fraction;
	g_mutex_unlock(&j->lock);
	if (j->gen == j->ed->open_gen)
		iv_toast_progress(j->ed->toast,
				  j->mode == SAVE_CLIPBOARD ? "Preparing the copy" :
							      "Saving",
				  f * 0.9);
	return G_SOURCE_CONTINUE;
}

static void full_render_thread(GTask *task, gpointer src, gpointer data,
			       GCancellable *c)
{
	SaveJob *j = data;
	/* A cache of its own: the full-size steps are not worth keeping
	 * once the picture is saved. */
	IvRenderCache *cache = iv_render_cache_new();
	IvRenderOutcome out;
	IvImage *img = iv_render_ex(j->src->img, j->sw, j->sh, j->state,
				    IV_RENDER_ALL, cache, j->cancel, save_progress,
				    j, &out);

	iv_render_cache_free(cache);
	if (img) {
		j->result = iv_pixbuf_from_image(img);
		iv_image_free(img);
		if (!j->result)
			j->failed = "Your edits";
	} else {
		j->failed = out.failed;
	}
	g_task_return_boolean(task, j->result != NULL);
}

static void saving_finished(IvEditor *ed, gboolean ok)
{
	IvEditorLeave then = ed->after_save;
	gpointer data = ed->after_save_data;

	ed->saving = FALSE;
	ed->after_save = NULL;
	ed->after_save_data = NULL;
	update_commands(ed);
	if (then)
		then(ok, data);
}

static void written(GObject *o, GAsyncResult *res, gpointer data)
{
	SaveJob *j = data;
	IvEditor *ed = j->ed;
	GError *e = NULL;
	gboolean ok = iv_save_finish(res, &e);

	if (j->gen != ed->open_gen) {
		g_clear_error(&e);
		save_job_free(j);
		return;
	}
	iv_toast_hide(ed->toast);
	if (!ok) {
		iv_window_error(ed->win, "The picture could not be saved", e);
		g_clear_error(&e);
		saving_finished(ed, FALSE);
		save_job_free(j);
		return;
	}
	/* Saved: the viewer shows the result, the editor is done. */
	char *path = g_strdup(j->path);
	SaveMode mode = j->mode;
	gboolean preview = j->preview;
	IvEditorLeave then = ed->after_save;
	gpointer then_data = ed->after_save_data;
	IvWindow *win = ed->win;

	ed->after_save = NULL;
	ed->after_save_data = NULL;
	ed->saving = FALSE;
	save_job_free(j);
	iv_viewer_mark_self_written(win, path);
	iv_editor_close(ed);
	iv_window_leave_editor(win);
	iv_viewer_open(win, path);
	{
		char *base = g_filename_display_basename(path);
		char *msg = mode == SAVE_OVERWRITE ?
				    g_strdup_printf(preview ? "Saved “%s” at the preview size" :
							      "Saved “%s”",
						    base) :
				    g_strdup_printf(preview ? "Saved a copy as “%s” at the "
							      "preview size" :
							      "Saved a copy as “%s”",
						    base);

		iv_window_toast(win, msg);
		g_free(msg);
		g_free(base);
	}
	g_free(path);
	/* Last: it may close the window. */
	if (then)
		then(TRUE, then_data);
}

static void write_result(SaveJob *j)
{
	iv_toast_progress(j->ed->toast, "Saving", 0.95);
	iv_save_pixbuf_async(j->result, j->path, &j->opt, NULL, written, j);
}

static void alpha_answered(int choice, gpointer data)
{
	SaveJob *j = data;
	IvEditor *ed = j->ed;

	if (choice == 1) {
		/* PNG instead: a new file next to the original. */
		char *png = iv_replace_extension(j->path, "png");

		if (g_file_test(png, G_FILE_TEST_EXISTS) &&
		    j->mode == SAVE_OVERWRITE) {
			char *u = iv_unique_sibling(j->path, "", "png");

			g_free(png);
			png = u;
		}
		g_free(j->path);
		j->path = png;
		j->opt.format = IV_FORMAT_PNG;
		j->mode = SAVE_COPY;
		write_result(j);
	} else if (choice == 2) {
		/* Over white: the encoder flattens it. */
		write_result(j);
	} else {
		iv_toast_hide(ed->toast);
		saving_finished(ed, FALSE);
		save_job_free(j);
	}
}

static gboolean pixbuf_opaque(GdkPixbuf *pb)
{
	IvImage probe;

	if (!gdk_pixbuf_get_has_alpha(pb))
		return TRUE;
	probe.w = gdk_pixbuf_get_width(pb);
	probe.h = gdk_pixbuf_get_height(pb);
	probe.stride = gdk_pixbuf_get_rowstride(pb);
	probe.px = (uint8_t *)gdk_pixbuf_read_pixels(pb);
	return iv_image_is_opaque(&probe) != 0;
}

static void render_failed(SaveJob *j);

static void rendered_full(GObject *o, GAsyncResult *res, gpointer data)
{
	SaveJob *j = g_task_get_task_data(G_TASK(res));
	IvEditor *ed = j->ed;

	/* The task does not own the job: it lives on to the write. */
	if (j->poll) {
		g_source_remove(j->poll);
		j->poll = 0;
	}
	if (j->gen != ed->open_gen) {
		save_job_free(j);
		return;
	}
	if (!j->result) {
		iv_toast_hide(ed->toast);
		if (g_cancellable_is_cancelled(j->cancel)) {
			saving_finished(ed, FALSE);
			save_job_free(j);
			return;
		}
		/* Never a quietly different picture: say what is missing
		 * and let the user decide. */
		render_failed(j);
		return;
	}
	if (j->mode == SAVE_CLIPBOARD) {
		GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

		gtk_clipboard_set_image(cb, j->result);
		iv_toast_hide(ed->toast);
		iv_editor_toast(ed, "Copied to the clipboard");
		saving_finished(ed, TRUE);
		save_job_free(j);
		return;
	}
	if (!iv_format_has_alpha(j->opt.format) && !pixbuf_opaque(j->result)) {
		iv_toast_hide(ed->toast);
		iv_choice(GTK_OVERLAY(ed->overlay), "The picture has transparent parts",
			  j->opt.format == IV_FORMAT_JPEG ?
				  "JPEG cannot keep transparency.  Save it as a PNG "
				  "copy, or fill the transparent parts with white?" :
				  "This format cannot keep transparency.  Save it as "
				  "a PNG copy, or fill the transparent parts with "
				  "white?",
			  "Save as _PNG", "_Fill with white", "_Cancel",
			  alpha_answered, j);
		return;
	}
	write_result(j);
}

/* Renders the job's state over its source (the full picture, or the
 * proxy after the user chose the preview size) in a worker thread. */
static void run_save_render(SaveJob *j)
{
	IvEditor *ed = j->ed;
	GTask *t;

	j->failed = NULL;
	ed->saving = TRUE;
	update_commands(ed);
	iv_toast_progress(ed->toast, j->mode == SAVE_CLIPBOARD ? "Preparing the copy" :
								  "Saving",
			  0);
	j->poll = g_timeout_add(120, save_poll, j);
	t = g_task_new(NULL, NULL, rendered_full, NULL);
	g_task_set_task_data(t, j, NULL);
	g_task_run_in_thread(t, full_render_thread);
	g_object_unref(t);
}

static void preview_size_answered(gboolean yes, gpointer data)
{
	SaveJob *j = data;
	IvEditor *ed = j->ed;

	if (j->gen != ed->open_gen) {
		save_job_free(j);
		return;
	}
	if (!yes) {
		saving_finished(ed, FALSE);
		save_job_free(j);
		return;
	}
	/* The same edits over the proxy the preview is made of: every step
	 * done, only fewer pixels. */
	shared_unref(j->src);
	j->src = shared_ref(ed->proxy);
	j->preview = TRUE;
	run_save_render(j);
}

/* A step of the full-size render ran out of memory. */
static void render_failed(SaveJob *j)
{
	IvEditor *ed = j->ed;
	const char *what = j->failed ? j->failed : "Your edits";
	char *title, *text;
	int pw, ph;

	if (j->preview || iv_editor_proxy_scale(ed) >= 0.999) {
		/* Nothing smaller to fall back on. */
		char *msg = g_strdup_printf("%s could not be applied: not enough "
					    "memory.  Nothing was saved.",
					    what);

		iv_editor_toast(ed, msg);
		g_free(msg);
		saving_finished(ed, FALSE);
		save_job_free(j);
		return;
	}
	pw = ed->proxy->img->w;
	ph = ed->proxy->img->h;
	title = g_strdup_printf("%s could not be applied at full size", what);
	text = g_strdup_printf(
		"There is not enough memory to finish the %d × %d picture.  "
		"It can be %s at the preview size instead, %d × %d, with every "
		"edit in it — or cancel, close other programs and try again.",
		ed->sw, ed->sh,
		j->mode == SAVE_CLIPBOARD ? "copied" : "saved", pw, ph);
	iv_confirm(GTK_OVERLAY(ed->overlay), title, text,
		   j->mode == SAVE_CLIPBOARD ? "Copy at _preview size" :
					       "Save at _preview size",
		   "_Cancel", FALSE, preview_size_answered, j);
	g_free(title);
	g_free(text);
}

static void start_full_render(IvEditor *ed, SaveMode mode, const char *path,
			      const IvSaveOptions *o)
{
	SaveJob *j = g_new0(SaveJob, 1);

	j->ed = ed;
	j->gen = ed->open_gen;
	j->mode = mode;
	j->path = g_strdup(path);
	if (o) {
		j->opt = *o;
		j->opt.exif_from = g_strdup(o->exif_from);
	}
	j->src = shared_ref(ed->source);
	j->state = iv_edit_state_copy(ed->committed);
	j->sw = ed->sw;
	j->sh = ed->sh;
	j->cancel = g_cancellable_new();
	g_mutex_init(&j->lock);
	run_save_render(j);
}

static IvFormat source_format(IvEditor *ed)
{
	return ed->pic ? iv_format_for_mime(ed->pic->mime) : IV_FORMAT_NONE;
}

static gboolean can_overwrite(IvEditor *ed)
{
	return ed->ready && source_format(ed) != IV_FORMAT_NONE &&
	       !(ed->pic && (ed->pic->anim || ed->pic->reduced)) &&
	       !iv_edit_state_equal(ed->committed, ed->saved) &&
	       access(ed->path, W_OK) == 0;
}

static void copy_chosen(const char *path, const IvSaveOptions *o,
			gpointer data)
{
	IvEditor *ed = data;

	if (!path) {
		saving_finished(ed, FALSE);
		return;
	}
	start_full_render(ed, SAVE_COPY, path, o);
}

static void save(IvEditor *ed, SaveMode mode)
{
	IvSaveOptions o;

	if (!ed->ready || iv_editor_busy(ed))
		return;
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	switch (mode) {
	case SAVE_OVERWRITE:
		if (!can_overwrite(ed)) {
			save(ed, SAVE_COPY);
			return;
		}
		iv_save_options_init(&o, source_format(ed));
		o.exif_from = g_strdup(ed->path);
		start_full_render(ed, SAVE_OVERWRITE, ed->path, &o);
		iv_save_options_clear(&o);
		break;
	case SAVE_COPY: {
		IvFormat f = source_format(ed);
		char *suggest;

		if (f == IV_FORMAT_NONE)
			f = IV_FORMAT_PNG;
		suggest = iv_unique_sibling(ed->path, " (edited)",
					    iv_format_extension(f));
		ed->saving = TRUE;
		update_commands(ed);
		iv_dialog_save_as(iv_editor_window(ed), suggest, f,
				  ed->pic && ed->pic->has_alpha, copy_chosen, ed);
		g_free(suggest);
		break;
	}
	case SAVE_CLIPBOARD:
		start_full_render(ed, SAVE_CLIPBOARD, NULL, NULL);
		break;
	}
}

/* ---- leaving --------------------------------------------------------------------- */

typedef struct {
	IvEditor *ed;
	IvEditorLeave then;
	gpointer data;
} Leave;

static void leave_answered(int choice, gpointer data)
{
	Leave *l = data;
	IvEditor *ed = l->ed;

	switch (choice) {
	case 1: /* save */
		ed->after_save = l->then;
		ed->after_save_data = l->data;
		save(ed, SAVE_OVERWRITE);
		break;
	case 2: /* discard */
		iv_editor_close(ed);
		if (l->then)
			l->then(TRUE, l->data);
		break;
	default:
		if (l->then)
			l->then(FALSE, l->data);
		break;
	}
	g_free(l);
}

void iv_editor_request_leave(IvEditor *ed, IvEditorLeave then, gpointer data)
{
	Leave *l;

	if (iv_confirm_is_open(GTK_OVERLAY(ed->overlay))) {
		if (then)
			then(FALSE, data);
		return;
	}
	if (ed->saving) {
		/* Leaving is what happens after the save anyway. */
		if (then)
			then(FALSE, data);
		return;
	}
	if (ed->busy && ed->job_cancel)
		g_cancellable_cancel(ed->job_cancel);
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	if (!iv_editor_dirty(ed)) {
		iv_editor_close(ed);
		if (then)
			then(TRUE, data);
		return;
	}
	l = g_new0(Leave, 1);
	l->ed = ed;
	l->then = then;
	l->data = data;
	iv_choice(GTK_OVERLAY(ed->overlay), "Save your edits?",
		  can_overwrite(ed) ?
			  "The picture has edits that are not saved yet." :
			  "The picture has edits that are not saved yet.  This "
			  "file cannot be overwritten, so they are saved as a copy.",
		  can_overwrite(ed) ? "_Save" : "_Save a copy", "_Discard",
		  "_Keep editing", leave_answered, l);
}

static void discard_answered(gboolean yes, gpointer data)
{
	IvEditor *ed = data;

	if (!yes)
		return;
	iv_editor_close(ed);
	iv_window_leave_editor(ed->win);
}

static void discard_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_busy(ed))
		return;
	if (ed->live)
		iv_editor_commit(ed, "Edit");
	if (!iv_editor_dirty(ed)) {
		iv_editor_close(ed);
		iv_window_leave_editor(ed->win);
		return;
	}
	iv_confirm(GTK_OVERLAY(ed->overlay), "Discard your edits?",
		   "The picture goes back to how it was; the edits cannot be "
		   "brought back.",
		   "_Discard", "_Keep editing", TRUE, discard_answered, ed);
}

/* ---- the commands -------------------------------------------------------------- */

static void update_commands(IvEditor *ed)
{
	gboolean idle = ed->ready && !iv_editor_busy(ed);

	gtk_widget_set_sensitive(ed->b_undo,
				 idle && (ed->live ||
					  iv_history_can_undo(ed->history)));
	gtk_widget_set_sensitive(ed->b_redo,
				 idle && !ed->live &&
					 iv_history_can_redo(ed->history));
	gtk_widget_set_sensitive(ed->b_revert,
				 idle && !iv_edit_state_is_identity(iv_editor_state(ed)));
	gtk_widget_set_sensitive(ed->b_compare, ed->ready);
	gtk_widget_set_sensitive(ed->b_done, idle);
	gtk_widget_set_sensitive(ed->b_done_menu, idle);
	gtk_widget_set_sensitive(ed->b_discard, !ed->saving);
	for (int i = 0; i < N_TOOLS; i++)
		gtk_widget_set_sensitive(ed->rail_buttons[i], idle);
	if (ed->b_undo) {
		const char *u = iv_history_undo_label(ed->history);
		const char *r = iv_history_redo_label(ed->history);
		char *tu = u ? g_strdup_printf("Undo %s (Ctrl+Z)", u) :
			       g_strdup("Undo (Ctrl+Z)");
		char *tr = r ? g_strdup_printf("Redo %s (Ctrl+Y)", r) :
			       g_strdup("Redo (Ctrl+Y)");

		gtk_widget_set_tooltip_text(ed->b_undo, tu);
		gtk_widget_set_tooltip_text(ed->b_redo, tr);
		g_free(tu);
		g_free(tr);
	}
	/* Done saves over the file when that is possible, else a copy. */
	gtk_widget_set_tooltip_text(ed->b_done, can_overwrite(ed) ?
							"Save over the original (Ctrl+S)" :
							"Save as a copy (Ctrl+S)");
}

static void done_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_dirty(ed) || ed->live)
		save(ed, SAVE_OVERWRITE);
	else {
		/* Nothing changed: Done just goes back. */
		iv_editor_close(ed);
		iv_window_leave_editor(ed->win);
	}
}

static void menu_save(IvEditor *ed)
{
	save(ed, SAVE_OVERWRITE);
}

static void menu_save_copy(IvEditor *ed)
{
	save(ed, SAVE_COPY);
}

static void menu_clipboard(IvEditor *ed)
{
	save(ed, SAVE_CLIPBOARD);
}

static void done_menu_clicked(GtkButton *b, IvEditor *ed)
{
	GtkWidget *m = iv_menu_new();
	GtkWidget *mi;

	mi = iv_menu_item(m, IV_GLYPH_CHECK, "_Save", "Ctrl+S",
			  G_CALLBACK(menu_save), ed);
	gtk_widget_set_sensitive(mi, can_overwrite(ed));
	iv_menu_item(m, IV_GLYPH_SAVE_AS, "Save as _copy…", "Ctrl+Shift+S",
		     G_CALLBACK(menu_save_copy), ed);
	iv_menu_separator(m);
	iv_menu_item(m, IV_GLYPH_COPY, "Copy to c_lipboard", "Ctrl+C",
		     G_CALLBACK(menu_clipboard), ed);
	iv_menu_popup_below_end(m, GTK_WIDGET(b));
}

static void undo_clicked(GtkButton *b, IvEditor *ed)
{
	if (ed->live) {
		iv_editor_cancel(ed);
		return;
	}
	iv_editor_undo(ed);
}

static void redo_clicked(GtkButton *b, IvEditor *ed)
{
	iv_editor_redo(ed);
}

static void revert_clicked(GtkButton *b, IvEditor *ed)
{
	iv_editor_revert(ed);
}

static void zoom_in_clicked(GtkButton *b, IvEditor *ed)
{
	iv_canvas_zoom_step(ed->canvas, 1, -1, -1);
}

static void zoom_out_clicked(GtkButton *b, IvEditor *ed)
{
	iv_canvas_zoom_step(ed->canvas, -1, -1, -1);
}

static void fit_clicked(GtkButton *b, IvEditor *ed)
{
	iv_canvas_zoom_fit(ed->canvas, TRUE);
}

static void zoom_pct_clicked(GtkButton *b, IvEditor *ed)
{
	iv_zoom_menu_popup(ed->canvas, GTK_WIDGET(b));
}

/* Compare shows the original while the button is held. */
static gboolean compare_pressed(GtkWidget *w, GdkEventButton *ev, IvEditor *ed)
{
	if (ev->button == 1)
		iv_editor_set_compare(ed, TRUE);
	return FALSE;
}

static gboolean compare_released(GtkWidget *w, GdkEventButton *ev,
				 IvEditor *ed)
{
	if (ev->button == 1)
		iv_editor_set_compare(ed, FALSE);
	return FALSE;
}

static gboolean compare_left(GtkWidget *w, GdkEventCrossing *ev, IvEditor *ed)
{
	iv_editor_set_compare(ed, FALSE);
	return FALSE;
}

/* ---- keys ------------------------------------------------------------------------ */

gboolean iv_editor_key(IvEditor *ed, GdkEventKey *ev)
{
	const IvTool *t = iv_editor_current_tool(ed);
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
	gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
	guint key = gdk_keyval_to_lower(ev->keyval);

	if (iv_confirm_is_open(GTK_OVERLAY(ed->overlay)))
		return FALSE;
	/* Typing in a field of a panel is the field's. */
	GtkWidget *focus = gtk_window_get_focus(iv_editor_window(ed));

	if (focus && GTK_IS_ENTRY(focus) && !ctrl && key != GDK_KEY_Escape)
		return FALSE;
	if (ed->ready && !iv_editor_busy(ed) && t && t->key_press &&
	    t->key_press(ed, ev))
		return TRUE;
	if (ctrl) {
		switch (key) {
		case GDK_KEY_z:
			if (shift)
				iv_editor_redo(ed);
			else if (ed->live)
				iv_editor_cancel(ed);
			else
				iv_editor_undo(ed);
			return TRUE;
		case GDK_KEY_y:
			iv_editor_redo(ed);
			return TRUE;
		case GDK_KEY_s:
			save(ed, shift ? SAVE_COPY : SAVE_OVERWRITE);
			return TRUE;
		case GDK_KEY_c:
			save(ed, SAVE_CLIPBOARD);
			return TRUE;
		case GDK_KEY_plus:
		case GDK_KEY_equal:
		case GDK_KEY_KP_Add:
			iv_canvas_zoom_step(ed->canvas, 1, -1, -1);
			return TRUE;
		case GDK_KEY_minus:
		case GDK_KEY_KP_Subtract:
			iv_canvas_zoom_step(ed->canvas, -1, -1, -1);
			return TRUE;
		case GDK_KEY_0:
			iv_canvas_zoom_fit(ed->canvas, TRUE);
			return TRUE;
		}
		return FALSE;
	}
	switch (key) {
	case GDK_KEY_Escape:
		if (ed->busy && ed->job_cancel) {
			g_cancellable_cancel(ed->job_cancel);
			return TRUE;
		}
		return FALSE; /* the window leaves the editor */
	case GDK_KEY_plus:
	case GDK_KEY_equal:
	case GDK_KEY_KP_Add:
		iv_canvas_zoom_step(ed->canvas, 1, -1, -1);
		return TRUE;
	case GDK_KEY_minus:
	case GDK_KEY_KP_Subtract:
		iv_canvas_zoom_step(ed->canvas, -1, -1, -1);
		return TRUE;
	case GDK_KEY_0:
	case GDK_KEY_KP_0:
		iv_canvas_zoom_fit(ed->canvas, TRUE);
		return TRUE;
	case GDK_KEY_1:
	case GDK_KEY_KP_1:
		iv_canvas_zoom_actual(ed->canvas, -1, -1);
		return TRUE;
	case GDK_KEY_backslash:
		iv_editor_set_compare(ed, TRUE);
		return TRUE;
	}
	if (!iv_editor_busy(ed))
		for (int i = 0; i < N_TOOLS; i++)
			if (tools[i]->key && tools[i]->key == key) {
				select_tool(ed, i);
				return TRUE;
			}
	return FALSE;
}

gboolean iv_editor_key_release(IvEditor *ed, GdkEventKey *ev)
{
	if (ev->keyval == GDK_KEY_backslash) {
		iv_editor_set_compare(ed, FALSE);
		return TRUE;
	}
	return FALSE;
}

/* ---- opening and closing --------------------------------------------------------- */

typedef struct {
	guint gen;
	GdkPixbuf *pb;
	IvImage *source, *proxy;
} OpenJob;

static void open_job_free(gpointer p)
{
	OpenJob *j = p;

	g_clear_object(&j->pb);
	if (j->source)
		iv_image_free(j->source);
	if (j->proxy)
		iv_image_free(j->proxy);
	g_free(j);
}

static void open_thread(GTask *task, gpointer src, gpointer data,
			GCancellable *c)
{
	OpenJob *j = data;

	j->source = iv_image_from_pixbuf(j->pb);
	if (j->source)
		j->proxy = iv_render_make_proxy(j->source);
	g_task_return_boolean(task, j->proxy != NULL);
}

static void opened(GObject *o, GAsyncResult *res, gpointer data)
{
	IvEditor *ed = data;
	OpenJob *j = g_task_get_task_data(G_TASK(res));

	if (j->gen != ed->open_gen)
		return;
	if (!j->proxy) {
		iv_canvas_set_loading(ed->canvas, FALSE);
		iv_canvas_set_message(ed->canvas, IV_GLYPH_WARNING,
				      "The picture cannot be edited",
				      "There is not enough memory for it.");
		return;
	}
	ed->source = shared_new(j->source);
	ed->proxy = shared_new(j->proxy);
	j->source = j->proxy = NULL;
	ed->sw = ed->source->img->w;
	ed->sh = ed->source->img->h;
	ed->proxy_scale = (double)ed->proxy->img->w / ed->sw;
	ed->original = iv_surface_from_image(ed->proxy->img);
	ed->ready = TRUE;
	ed->first_render = TRUE;
	select_tool(ed, ed->last_tool >= 0 ? ed->last_tool : 0);
	request_render(ed);
	update_commands(ed);
}

void iv_editor_open(IvEditor *ed, IvPicture *p)
{
	OpenJob *j;
	GTask *t;

	iv_editor_close(ed);
	ed->pic = iv_picture_ref(p);
	ed->path = g_strdup(p->path);
	ed->name = g_strdup(p->name);
	ed->title = g_strdup_printf("Editing · %s", p->name);
	ed->committed = iv_edit_state_new();
	ed->saved = iv_edit_state_new();
	ed->render_cancel = g_cancellable_new();
	ed->cache = cache_new();
	iv_canvas_set_message(ed->canvas, IV_GLYPH_NONE, NULL, NULL);
	iv_canvas_set_loading(ed->canvas, TRUE);
	update_commands(ed);

	j = g_new0(OpenJob, 1);
	j->gen = ed->open_gen;
	j->pb = g_object_ref(p->pixbuf);
	t = g_task_new(NULL, NULL, opened, ed);
	g_task_set_task_data(t, j, open_job_free);
	g_task_run_in_thread(t, open_thread);
	g_object_unref(t);
}

void iv_editor_close(IvEditor *ed)
{
	const IvTool *t = iv_editor_current_tool(ed);

	if (ed->ready && t && t->deactivate)
		t->deactivate(ed);
	if (ed->tool >= 0) {
		iv_remove_class(ed->rail_buttons[ed->tool], "active");
		ed->last_tool = ed->tool;
		ed->tool = -1;
	}
	/* Everything in flight belongs to the old generation from here. */
	ed->open_gen++;
	if (ed->render_cancel)
		g_cancellable_cancel(ed->render_cancel);
	g_clear_object(&ed->render_cancel);
	if (ed->job_cancel)
		g_cancellable_cancel(ed->job_cancel);
	g_clear_object(&ed->job_cancel);
	ed->busy = ed->saving = FALSE;
	ed->render_running = ed->render_pending = FALSE;
	ed->ready = FALSE;
	ed->compare = FALSE;
	if (ed->live) {
		iv_edit_state_free(ed->live);
		ed->live = NULL;
	}
	g_clear_pointer(&ed->committed, iv_edit_state_free);
	g_clear_pointer(&ed->saved, iv_edit_state_free);
	iv_history_clear(ed->history);
	g_clear_pointer(&ed->source, shared_unref);
	g_clear_pointer(&ed->proxy, shared_unref);
	g_clear_pointer(&ed->cache, cache_unref);
	g_clear_pointer(&ed->shown, cairo_surface_destroy);
	g_clear_pointer(&ed->original, cairo_surface_destroy);
	if (ed->pic) {
		iv_picture_unref(ed->pic);
		ed->pic = NULL;
	}
	g_clear_pointer(&ed->path, g_free);
	g_clear_pointer(&ed->name, g_free);
	g_clear_pointer(&ed->title, g_free);
	iv_toast_hide(ed->toast);
	iv_canvas_set_loading(ed->canvas, FALSE);
	iv_canvas_set_message(ed->canvas, IV_GLYPH_NONE, NULL, NULL);
	ed->sw = ed->sh = 0;
}

/* ---- building the editor -------------------------------------------------------- */

static GtkWidget *bar_button(IvGlyph g, const char *tip, GCallback cb,
			     IvEditor *ed)
{
	GtkWidget *b = iv_glyph_button(g, 16, tip, "iv-bar-button");

	if (cb)
		g_signal_connect(b, "clicked", cb, ed);
	return b;
}

static GtkWidget *bar_separator(void)
{
	GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);

	iv_add_class(s, "iv-bar-separator");
	return s;
}

static GtkWidget *build_topbar(IvEditor *ed)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *split = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	iv_add_class(bar, "iv-topbar");
	ed->b_zoom_in = bar_button(IV_GLYPH_ZOOM_IN, "Zoom in (+)",
				   G_CALLBACK(zoom_in_clicked), ed);
	ed->b_zoom_out = bar_button(IV_GLYPH_ZOOM_OUT, "Zoom out (−)",
				    G_CALLBACK(zoom_out_clicked), ed);
	ed->b_fit = bar_button(IV_GLYPH_FIT, "Fit to window (0)",
			       G_CALLBACK(fit_clicked), ed);
	ed->b_zoom_pct = iv_text_button(IV_GLYPH_NONE, "100 %", TRUE,
					"iv-bar-button");
	gtk_widget_set_tooltip_text(ed->b_zoom_pct, "Zoom");
	g_signal_connect(ed->b_zoom_pct, "clicked", G_CALLBACK(zoom_pct_clicked),
			 ed);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_zoom_in, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_zoom_out, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_fit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_zoom_pct, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), bar_separator(), FALSE, FALSE, 6);
	ed->b_undo = bar_button(IV_GLYPH_UNDO, "Undo (Ctrl+Z)",
				G_CALLBACK(undo_clicked), ed);
	ed->b_redo = bar_button(IV_GLYPH_REDO, "Redo (Ctrl+Y)",
				G_CALLBACK(redo_clicked), ed);
	ed->b_revert = iv_text_button(IV_GLYPH_REVERT, "Revert", FALSE,
				      "iv-bar-button");
	gtk_widget_set_tooltip_text(ed->b_revert, "Revert all edits");
	g_signal_connect(ed->b_revert, "clicked", G_CALLBACK(revert_clicked), ed);
	ed->b_compare = iv_text_button(IV_GLYPH_COMPARE, "Compare", FALSE,
				       "iv-bar-button");
	gtk_widget_set_tooltip_text(ed->b_compare,
				    "Hold to see the original (\\)");
	g_signal_connect(ed->b_compare, "button-press-event",
			 G_CALLBACK(compare_pressed), ed);
	g_signal_connect(ed->b_compare, "button-release-event",
			 G_CALLBACK(compare_released), ed);
	g_signal_connect(ed->b_compare, "leave-notify-event",
			 G_CALLBACK(compare_left), ed);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_undo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_redo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_revert, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), bar_separator(), FALSE, FALSE, 6);
	gtk_box_pack_start(GTK_BOX(bar), ed->b_compare, FALSE, FALSE, 0);

	ed->b_discard = gtk_button_new_with_mnemonic("_Discard");
	iv_add_class(ed->b_discard, "iv-outline-button");
	gtk_widget_set_can_focus(ed->b_discard, FALSE);
	gtk_widget_set_valign(ed->b_discard, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(ed->b_discard, "Leave without saving");
	g_signal_connect(ed->b_discard, "clicked", G_CALLBACK(discard_clicked), ed);
	gtk_box_pack_end(GTK_BOX(bar), ed->b_discard, FALSE, FALSE, 0);

	iv_add_class(split, "iv-split");
	gtk_widget_set_valign(split, GTK_ALIGN_CENTER);
	ed->b_done = iv_text_button(IV_GLYPH_CHECK, "Done", FALSE, "iv-done");
	g_signal_connect(ed->b_done, "clicked", G_CALLBACK(done_clicked), ed);
	ed->b_done_menu = iv_glyph_button(IV_GLYPH_CHEVRON_DOWN, 12,
					  "More ways to save", "iv-done-menu");
	g_signal_connect(ed->b_done_menu, "clicked",
			 G_CALLBACK(done_menu_clicked), ed);
	gtk_box_pack_start(GTK_BOX(split), ed->b_done, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(split), ed->b_done_menu, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar), split, FALSE, FALSE, 8);
	return bar;
}

static GtkWidget *build_rail(IvEditor *ed)
{
	GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	iv_add_class(rail, "iv-rail");
	gtk_widget_set_size_request(rail, 76, -1);
	for (int i = 0; i < N_TOOLS; i++) {
		const IvTool *t = tools[i];
		GtkWidget *b = gtk_button_new();
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GtkWidget *l = gtk_label_new(t->label);
		char *tip = t->tooltip ? g_strdup(t->tooltip) :
				t->key ? g_strdup_printf("%s (%s)", t->label,
							 gdk_keyval_name(gdk_keyval_to_upper(t->key))) :
					 g_strdup(t->label);

		gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(box), iv_glyph_new(t->glyph, 20), FALSE,
				   FALSE, 0);
		iv_add_class(l, "iv-rail-label");
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(b), box);
		gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
		gtk_widget_set_can_focus(b, FALSE);
		iv_add_class(b, "iv-rail-button");
		gtk_widget_set_tooltip_text(b, tip);
		atk_object_set_name(gtk_widget_get_accessible(b), t->label);
		g_free(tip);
		g_object_set_data(G_OBJECT(b), "iv-tool", GINT_TO_POINTER(i));
		g_signal_connect(b, "clicked", G_CALLBACK(rail_clicked), ed);
		ed->rail_buttons[i] = b;
		gtk_box_pack_start(GTK_BOX(rail), b, FALSE, FALSE, 0);
	}
	return rail;
}

IvEditor *iv_editor_new(IvWindow *w)
{
	IvEditor *ed = g_new0(IvEditor, 1);
	GtkWidget *row, *scroller;

	ed->win = w;
	ed->tool = -1;
	ed->last_tool = -1;
	ed->history = iv_history_new();
	ed->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	iv_add_class(ed->root, "iv-editor");
	gtk_box_pack_start(GTK_BOX(ed->root), build_topbar(ed), FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(row), build_rail(ed), FALSE, FALSE, 0);

	ed->overlay = gtk_overlay_new();
	ed->canvas = iv_canvas_new();
	iv_add_class(IV_CANVAS_GTK(ed->canvas), "iv-edit-canvas");
	iv_canvas_set_insets(ed->canvas, 24, 24, 24, 24);
	iv_canvas_set_hooks(ed->canvas, canvas_overlay, canvas_event, ed);
	g_signal_connect(ed->canvas, "view-changed", G_CALLBACK(view_changed), ed);
	gtk_container_add(GTK_CONTAINER(ed->overlay), IV_CANVAS_GTK(ed->canvas));
	ed->toast = iv_toast_new(GTK_OVERLAY(ed->overlay));
	iv_toast_set_margin(ed->toast, 24);
	gtk_box_pack_start(GTK_BOX(row), ed->overlay, TRUE, TRUE, 0);

	ed->panel_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	iv_add_class(ed->panel_card, "iv-options");
	gtk_widget_set_size_request(ed->panel_card, 340, -1);
	scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
				       GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(ed->panel_card, FALSE);
	ed->panel_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(ed->panel_stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(ed->panel_stack), 120);
	gtk_stack_set_vhomogeneous(GTK_STACK(ed->panel_stack), FALSE);
	gtk_container_add(GTK_CONTAINER(scroller), ed->panel_stack);
	gtk_box_pack_start(GTK_BOX(ed->panel_card), scroller, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), ed->panel_card, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(ed->root), row, TRUE, TRUE, 0);
	gtk_widget_show_all(ed->root);
	update_commands(ed);
	return ed;
}

void iv_editor_free(IvEditor *ed)
{
	if (!ed)
		return;
	iv_editor_close(ed);
	iv_history_free(ed->history);
	iv_toast_free(ed->toast);
	if (ed->rendered_hooks)
		g_array_unref(ed->rendered_hooks);
	g_free(ed);
}

GtkWidget *iv_editor_widget(IvEditor *ed)
{
	return ed->root;
}

/* ---- panel building blocks ---------------------------------------------------------- */

GtkWidget *iv_panel_new(const char *title, const char *subtitle)
{
	GtkWidget *p = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

	iv_add_class(p, "iv-panel");
	if (title) {
		GtkWidget *t = gtk_label_new(title);

		gtk_label_set_xalign(GTK_LABEL(t), 0);
		iv_add_class(t, "iv-panel-title");
		gtk_box_pack_start(GTK_BOX(p), t, FALSE, FALSE, 0);
	}
	if (subtitle) {
		GtkWidget *s = gtk_label_new(subtitle);

		gtk_label_set_xalign(GTK_LABEL(s), 0);
		gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(s), 40);
		iv_add_class(s, "iv-panel-subtitle");
		gtk_box_pack_start(GTK_BOX(p), s, FALSE, FALSE, 0);
	}
	return p;
}

GtkWidget *iv_panel_section(GtkWidget *panel, const char *title,
			    GtkWidget **reset_out)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *l = gtk_label_new(title);

	iv_add_class(row, "iv-panel-section");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-panel-section-title");
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	if (reset_out) {
		GtkWidget *r = iv_glyph_button(IV_GLYPH_RESET, 14, "Reset section",
					       "iv-panel-reset");

		gtk_box_pack_end(GTK_BOX(row), r, FALSE, FALSE, 0);
		*reset_out = r;
	}
	if (panel)
		gtk_box_pack_start(GTK_BOX(panel), row, FALSE, FALSE, 0);
	return row;
}

typedef struct {
	IvEditor *ed;
	char *label;
	double neutral;
	IvSliderGet get;
	IvSliderSet set;
	gpointer data;
	GtkWidget *scale, *value;
	gboolean syncing, dragging, symmetric;
	guint commit_timer;
} Slider;

static void slider_free(gpointer p)
{
	Slider *s = p;

	if (s->commit_timer)
		g_source_remove(s->commit_timer);
	g_free(s->label);
	g_free(s);
}

static void slider_show_value(Slider *s, double v)
{
	char buf[16];

	if (s->symmetric)
		g_snprintf(buf, sizeof buf, "%+d", (int)lround(v));
	else
		g_snprintf(buf, sizeof buf, "%d", (int)lround(v));
	if (s->symmetric && lround(v) == 0)
		g_snprintf(buf, sizeof buf, "0");
	gtk_label_set_text(GTK_LABEL(s->value), buf);
}

static gboolean slider_commit_later(gpointer data)
{
	Slider *s = data;

	s->commit_timer = 0;
	if (!s->dragging)
		iv_editor_commit(s->ed, s->label);
	return G_SOURCE_REMOVE;
}

static void slider_moved(GtkRange *r, Slider *s)
{
	double v = gtk_range_get_value(r);
	IvEditState *st;

	slider_show_value(s, v);
	if (s->syncing)
		return;
	st = iv_editor_begin(s->ed);
	s->set(st, v, s->data);
	iv_editor_changed(s->ed);
	/* Keys and the wheel move it too: one undo step once it rests. */
	if (!s->dragging) {
		if (s->commit_timer)
			g_source_remove(s->commit_timer);
		s->commit_timer = g_timeout_add(600, slider_commit_later, s);
	}
}

static gboolean slider_press(GtkWidget *w, GdkEventButton *ev, Slider *s)
{
	if (ev->type == GDK_2BUTTON_PRESS) {
		IvEditState *st = iv_editor_begin(s->ed);

		s->set(st, s->neutral, s->data);
		iv_editor_changed(s->ed);
		iv_editor_commit(s->ed, s->label);
		iv_panel_slider_sync(g_object_get_data(G_OBJECT(w), "iv-row"));
		return TRUE;
	}
	if (ev->button == 1)
		s->dragging = TRUE;
	return FALSE;
}

static gboolean slider_release(GtkWidget *w, GdkEventButton *ev, Slider *s)
{
	if (ev->button != 1)
		return FALSE;
	s->dragging = FALSE;
	if (s->commit_timer) {
		g_source_remove(s->commit_timer);
		s->commit_timer = 0;
	}
	iv_editor_commit(s->ed, s->label);
	return FALSE;
}

GtkWidget *iv_panel_slider(IvEditor *ed, const char *label, double min,
			   double max, double step, double neutral,
			   IvSliderGet get, IvSliderSet set, gpointer data)
{
	Slider *s = g_new0(Slider, 1);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *eb = gtk_event_box_new();
	GtkWidget *l = gtk_label_new(label);

	s->ed = ed;
	s->label = g_strdup(label);
	s->neutral = neutral;
	s->get = get;
	s->set = set;
	s->data = data;
	s->symmetric = min < 0;
	iv_add_class(row, "iv-slider-row");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-slider-label");
	s->value = gtk_label_new("0");
	iv_add_class(s->value, "iv-slider-value");
	gtk_box_pack_start(GTK_BOX(head), l, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(head), s->value, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(eb), head);
	gtk_widget_set_tooltip_text(eb, "Double-click to reset");
	g_signal_connect(eb, "button-press-event", G_CALLBACK(slider_press), s);
	g_object_set_data(G_OBJECT(eb), "iv-row", row);
	gtk_box_pack_start(GTK_BOX(row), eb, FALSE, FALSE, 0);

	s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max,
					    step);
	gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
	gtk_range_set_value(GTK_RANGE(s->scale), neutral);
	if (s->symmetric) {
		/* A slider about a neutral middle: no fill from the left end,
		 * a mark at the middle instead. */
		gtk_scale_add_mark(GTK_SCALE(s->scale), neutral, GTK_POS_BOTTOM,
				   NULL);
		iv_add_class(s->scale, "iv-symmetric");
	}
	iv_add_class(s->scale, "iv-slider");
	gtk_widget_set_can_focus(s->scale, TRUE);
	g_object_set_data(G_OBJECT(s->scale), "iv-row", row);
	g_signal_connect(s->scale, "value-changed", G_CALLBACK(slider_moved), s);
	g_signal_connect(s->scale, "button-press-event", G_CALLBACK(slider_press),
			 s);
	g_signal_connect(s->scale, "button-release-event",
			 G_CALLBACK(slider_release), s);
	gtk_box_pack_start(GTK_BOX(row), s->scale, FALSE, FALSE, 0);
	g_object_set_data_full(G_OBJECT(row), "iv-slider", s, slider_free);
	g_object_set_data(G_OBJECT(row), "iv-scale", s->scale);
	iv_panel_slider_sync(row);
	return row;
}

void iv_panel_slider_sync(GtkWidget *row)
{
	Slider *s = g_object_get_data(G_OBJECT(row), "iv-slider");
	double v;

	if (!s)
		return;
	v = s->ed->committed || s->ed->live ? s->get(iv_editor_state(s->ed),
						     s->data) :
					      s->neutral;
	s->syncing = TRUE;
	gtk_range_set_value(GTK_RANGE(s->scale), v);
	s->syncing = FALSE;
	slider_show_value(s, v);
}

/* The flow box wraps each child in a GtkFlowBoxChild that takes the
 * keyboard focus of its own, and its focus ring would then sit round a
 * chip or swatch whose button never shows focus itself: the panel's
 * controls are pointer controls, the keys belong to the canvas. */
static void flow_child_unfocusable(GtkWidget *w)
{
	GtkWidget *child = gtk_widget_get_parent(w);

	if (child && GTK_IS_FLOW_BOX_CHILD(child))
		gtk_widget_set_can_focus(child, FALSE);
}

typedef struct {
	IvChipChosen chosen;
	gpointer data;
	gboolean syncing;
} Chips;

static void chip_toggled(GtkToggleButton *b, GtkWidget *box)
{
	Chips *c = g_object_get_data(G_OBJECT(box), "iv-chips");
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "iv-index"));

	if (c->syncing)
		return;
	iv_panel_chips_set(box, i);
	if (c->chosen)
		c->chosen(i, c->data);
}

GtkWidget *iv_panel_chips(const char *const *labels, int n, int active,
			  IvChipChosen chosen, gpointer data)
{
	GtkWidget *box = gtk_flow_box_new();
	Chips *c = g_new0(Chips, 1);

	c->chosen = chosen;
	c->data = data;
	g_object_set_data_full(G_OBJECT(box), "iv-chips", c, g_free);
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), FALSE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 6);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 6);
	iv_add_class(box, "iv-chips");
	for (int i = 0; i < n; i++) {
		GtkWidget *b = gtk_toggle_button_new_with_label(labels[i]);

		iv_add_class(b, "iv-chip");
		gtk_widget_set_can_focus(b, FALSE);
		g_object_set_data(G_OBJECT(b), "iv-index", GINT_TO_POINTER(i));
		g_signal_connect(b, "toggled", G_CALLBACK(chip_toggled), box);
		gtk_container_add(GTK_CONTAINER(box), b);
		flow_child_unfocusable(b);
	}
	iv_panel_chips_set(box, active);
	return box;
}

void iv_panel_chips_set(GtkWidget *chips, int active)
{
	Chips *c = g_object_get_data(G_OBJECT(chips), "iv-chips");
	GList *kids = gtk_container_get_children(GTK_CONTAINER(chips));

	c->syncing = TRUE;
	for (GList *l = kids; l; l = l->next) {
		GtkWidget *b = gtk_bin_get_child(GTK_BIN(l->data));
		int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "iv-index"));

		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), i == active);
	}
	c->syncing = FALSE;
	g_list_free(kids);
}

GtkWidget *iv_panel_button(IvGlyph g, const char *label)
{
	GtkWidget *b = iv_text_button(g, label, FALSE, "iv-panel-button");

	gtk_widget_set_hexpand(b, TRUE);
	return b;
}

GtkWidget *iv_panel_switch(const char *label, gboolean active,
			   GtkWidget **switch_out)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l = gtk_label_new(label);
	GtkWidget *sw = gtk_switch_new();

	iv_add_class(row, "iv-switch-row");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_switch_set_active(GTK_SWITCH(sw), active);
	gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
	gtk_widget_set_can_focus(sw, FALSE);
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), sw, FALSE, FALSE, 0);
	if (switch_out)
		*switch_out = sw;
	return row;
}

typedef struct {
	IvSwatchPicked picked;
	gpointer data;
	guint32 current;
	GtkWidget *custom;
	GArray *colors;		/* guint32, the swatches offered */
	GtkWidget *chooser;	/* the colour chooser while it is open */
} Swatches;

static void swatches_free(gpointer p)
{
	Swatches *s = p;

	if (s->chooser)
		gtk_widget_destroy(s->chooser);
	g_array_unref(s->colors);
	g_free(s);
}

/* Is `c' one of the swatches (else it came from the chooser)? */
static gboolean swatches_offer(const Swatches *s, guint32 c)
{
	for (guint i = 0; i < s->colors->len; i++)
		if (g_array_index(s->colors, guint32, i) == c)
			return TRUE;
	return FALSE;
}

static void rgba_to_gdk(guint32 c, GdkRGBA *g)
{
	g->red = ((c >> 24) & 0xff) / 255.0;
	g->green = ((c >> 16) & 0xff) / 255.0;
	g->blue = ((c >> 8) & 0xff) / 255.0;
	g->alpha = (c & 0xff) / 255.0;
}

static guint32 gdk_to_rgba(const GdkRGBA *g)
{
	return (guint32)lround(CLAMP(g->red, 0, 1) * 255) << 24 |
	       (guint32)lround(CLAMP(g->green, 0, 1) * 255) << 16 |
	       (guint32)lround(CLAMP(g->blue, 0, 1) * 255) << 8 |
	       (guint32)lround(CLAMP(g->alpha, 0, 1) * 255);
}

static gboolean swatch_draw(GtkWidget *w, cairo_t *cr, GtkWidget *box)
{
	Swatches *s = g_object_get_data(G_OBJECT(box), "iv-swatches");
	guint32 c = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "iv-rgba"));
	gboolean custom = g_object_get_data(G_OBJECT(w), "iv-custom") != NULL;
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	double r = MIN(W, H) / 2 - 3;
	GdkRGBA g, ring;
	GtkStyleContext *sc = gtk_widget_get_style_context(w);

	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), &ring);
	if (custom) {
		/* A ring of hues: any colour. */
		for (int i = 0; i < 12; i++) {
			double a0 = i * G_PI / 6, a1 = (i + 1) * G_PI / 6 + 0.02;
			double h = i / 12.0, rr, gg, bb;

			gtk_hsv_to_rgb(h, 0.8, 1.0, &rr, &gg, &bb);
			cairo_set_source_rgb(cr, rr, gg, bb);
			cairo_move_to(cr, W / 2, H / 2);
			cairo_arc(cr, W / 2, H / 2, r - 1, a0, a1);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		/* A colour picked with the chooser is the selected one: it
		 * shows in the middle of the ring, and the ring is marked. */
		if (!swatches_offer(s, s->current)) {
			double ri = (r - 1) * 0.58;

			rgba_to_gdk(s->current, &g);
			if (g.alpha < 1) {
				cairo_pattern_t *ch = iv_checker_pattern(3);

				cairo_arc(cr, W / 2, H / 2, ri, 0, 2 * G_PI);
				cairo_set_source(cr, ch);
				cairo_fill(cr);
				cairo_pattern_destroy(ch);
			}
			gdk_cairo_set_source_rgba(cr, &g);
			cairo_arc(cr, W / 2, H / 2, ri, 0, 2 * G_PI);
			cairo_fill_preserve(cr);
			/* Apart from the hues around it, whatever it is. */
			cairo_set_source_rgba(cr, 0x16 / 255.0, 0x1c / 255.0,
					      0x2a / 255.0, 1);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
			cairo_set_source_rgba(cr, ring.red, ring.green, ring.blue, 1);
			cairo_set_line_width(cr, 2);
			cairo_arc(cr, W / 2, H / 2, r + 2, 0, 2 * G_PI);
			cairo_stroke(cr);
		}
	} else {
		rgba_to_gdk(c, &g);
		if (g.alpha < 1) {
			cairo_pattern_t *ch = iv_checker_pattern(3);

			cairo_arc(cr, W / 2, H / 2, r - 1, 0, 2 * G_PI);
			cairo_set_source(cr, ch);
			cairo_fill(cr);
			cairo_pattern_destroy(ch);
		}
		gdk_cairo_set_source_rgba(cr, &g);
		cairo_arc(cr, W / 2, H / 2, r - 1, 0, 2 * G_PI);
		cairo_fill(cr);
		/* A hairline, so a swatch of the panel's own colour shows. */
		cairo_set_source_rgba(cr, ring.red, ring.green, ring.blue, 0.25);
		cairo_set_line_width(cr, 1);
		cairo_arc(cr, W / 2, H / 2, r - 0.5, 0, 2 * G_PI);
		cairo_stroke(cr);
	}
	if (!custom && c == s->current) {
		cairo_set_source_rgba(cr, ring.red, ring.green, ring.blue, 1);
		cairo_set_line_width(cr, 2);
		cairo_arc(cr, W / 2, H / 2, r + 2, 0, 2 * G_PI);
		cairo_stroke(cr);
	}
	return TRUE;
}

static void swatch_chosen(GtkWidget *box, guint32 c)
{
	Swatches *s = g_object_get_data(G_OBJECT(box), "iv-swatches");

	iv_panel_swatches_set(box, c);
	if (s->picked)
		s->picked(c, s->data);
}

/* The chooser answers (it is modal but does not stop the main loop, so
 * renders and jobs go on behind it). */
static void chooser_response(GtkDialog *d, int response, GtkWidget *box)
{
	Swatches *s = g_object_get_data(G_OBJECT(box), "iv-swatches");
	GdkRGBA g;

	if (s)
		s->chooser = NULL;
	if (s && response == GTK_RESPONSE_OK) {
		gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(d), &g);
		swatch_chosen(box, gdk_to_rgba(&g));
	}
	gtk_widget_destroy(GTK_WIDGET(d));
	g_object_unref(box);
}

static void swatch_clicked(GtkButton *b, GtkWidget *box)
{
	Swatches *s = g_object_get_data(G_OBJECT(box), "iv-swatches");
	guint32 c = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "iv-rgba"));
	GtkWidget *d;
	GdkRGBA g;

	if (!g_object_get_data(G_OBJECT(b), "iv-custom")) {
		swatch_chosen(box, c);
		return;
	}
	if (s->chooser) {
		gtk_window_present(GTK_WINDOW(s->chooser));
		return;
	}
	d = gtk_color_chooser_dialog_new(
		"Choose a colour", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))));
	rgba_to_gdk(s->current, &g);
	gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(d), TRUE);
	gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(d), &g);
	gtk_window_set_modal(GTK_WINDOW(d), TRUE);
	s->chooser = d;
	g_signal_connect(d, "response", G_CALLBACK(chooser_response),
			 g_object_ref(box));
	gtk_widget_show(d);
}

static GtkWidget *swatch(GtkWidget *box, guint32 c, gboolean custom)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *area = gtk_drawing_area_new();

	gtk_widget_set_size_request(area, 26, 26);
	g_object_set_data(G_OBJECT(area), "iv-rgba", GUINT_TO_POINTER(c));
	g_object_set_data(G_OBJECT(b), "iv-rgba", GUINT_TO_POINTER(c));
	if (custom) {
		g_object_set_data(G_OBJECT(area), "iv-custom", GINT_TO_POINTER(1));
		g_object_set_data(G_OBJECT(b), "iv-custom", GINT_TO_POINTER(1));
		gtk_widget_set_tooltip_text(b, "Another colour…");
	}
	g_signal_connect(area, "draw", G_CALLBACK(swatch_draw), box);
	gtk_container_add(GTK_CONTAINER(b), area);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(b, FALSE);
	iv_add_class(b, "iv-swatch");
	g_signal_connect(b, "clicked", G_CALLBACK(swatch_clicked), box);
	return b;
}

GtkWidget *iv_panel_swatches(const guint32 *colors, int n, guint32 current,
			     IvSwatchPicked picked, gpointer data)
{
	GtkWidget *box = gtk_flow_box_new();
	Swatches *s = g_new0(Swatches, 1);

	s->picked = picked;
	s->data = data;
	s->current = current;
	s->colors = g_array_new(FALSE, FALSE, sizeof(guint32));
	g_array_append_vals(s->colors, colors, (guint)n);
	g_object_set_data_full(G_OBJECT(box), "iv-swatches", s, swatches_free);
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 9);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(box), 5);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 2);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 2);
	iv_add_class(box, "iv-swatches");
	for (int i = 0; i < n; i++) {
		GtkWidget *b = swatch(box, colors[i], FALSE);

		gtk_container_add(GTK_CONTAINER(box), b);
		flow_child_unfocusable(b);
	}
	s->custom = swatch(box, 0, TRUE);
	gtk_container_add(GTK_CONTAINER(box), s->custom);
	flow_child_unfocusable(s->custom);
	return box;
}

void iv_panel_swatches_set(GtkWidget *swatches, guint32 rgba)
{
	Swatches *s = g_object_get_data(G_OBJECT(swatches), "iv-swatches");

	s->current = rgba;
	gtk_widget_queue_draw(swatches);
	if (s->custom)
		gtk_widget_set_tooltip_text(s->custom,
					    swatches_offer(s, rgba) ?
						    "Another colour…" :
						    "Your own colour (click to change)");
}

GtkWidget *iv_panel_note(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
	iv_add_class(l, "iv-panel-note");
	return l;
}
