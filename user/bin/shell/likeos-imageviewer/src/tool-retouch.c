// LikeOS -- LikeOS Image Viewer: the Retouch tool.
//
// Painting over spots to remove them.  The brush paints a mask -- SOURCE
// coordinates, at MASK resolution (tool-brush.h) -- shown over the picture
// as a translucent accent wash while it is painted.  The painted area is
// then filled from its surroundings by ivproc's exemplar-based inpainting:
//
//   * with "Apply as you paint" on (the default), as soon as the button
//     goes up after each stroke;
//   * with it off, strokes add up until Apply is pressed (Clear mask
//     drops them); the right button, or the brush with Alt held, takes
//     away from the mask.
//
// Applying adds the mask to the edit state's retouch list as one undo
// step.  The inpainting itself is done by the render pipeline, which
// keeps each step's result (render.c): applying first renders the new
// state over the proxy in a worker (iv_editor_run_render) under a progress
// toast that can be cancelled, so the preview that follows finds the step
// done, and saving inpaints every mask again over the full-size picture,
// the mask resized to it.  Because the mask is stored at (or near) the
// source's own resolution, the full-size result fills exactly the area
// painted, however small the proxy is.
//
// The brush size is in picture pixels (5 to 200); the outline that stands
// in for the pointer is a dashed circle of that size at the current zoom.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tool-brush.h"

#define BRUSH_MIN 5.0
#define BRUSH_MAX 200.0
/* A brush that is solid to 80 % of its radius: what is painted is filled
 * (the inpainting takes bytes >= 128), with a soft rim so the overlay does
 * not look cut out. */
#define HARDNESS 0.8
/* The wash over the painted area: the shell accent. */
#define WASH 0x3f74f294u

static IvEditor *editor;
static double brush_size = 40;		/* diameter, SOURCE pixels */
static gboolean auto_apply = TRUE;

/* The painted mask (MASK resolution, writable: in no state) and its
 * picture on the canvas. */
static IvMask *pending;
static IvMaskView *view;
static gboolean has_paint;		/* something >= 128 in pending */

/* A stroke in progress. */
static gboolean painting, removing;
static double last_mx, last_my;		/* MASK pixels */

static gboolean pointer_in;
static double pointer_wx, pointer_wy;

/* The panel. */
static GtkWidget *size_row, *auto_switch, *manual_box, *status_label;
static GtkWidget *apply_button, *clear_button, *count_label, *remove_all_button;
static gboolean syncing;

static void sync_panel(void);
static void apply_mask(IvEditor *ed);

/* ---- the mask -------------------------------------------------------------- */

static void drop_mask(void)
{
	g_clear_pointer(&pending, iv_mask_unref);
	g_clear_pointer(&view, iv_mask_view_free);
	has_paint = FALSE;
	painting = FALSE;
}

/* The mask and its view, made for the editor's picture on first use. */
static gboolean ensure_mask(IvEditor *ed)
{
	int mw, mh;

	iv_brush_mask_size(ed, &mw, &mh);
	if (mw <= 0)
		return FALSE;
	if (pending && (pending->w != mw || pending->h != mh))
		drop_mask();
	if (!pending)
		pending = iv_mask_new(mw, mh);
	if (!view)
		view = iv_mask_view_new(mw, mh);
	return pending && view;
}

static void clear_mask(void)
{
	if (pending)
		memset(pending->data, 0, (gsize)pending->w * (gsize)pending->h);
	iv_mask_view_clear(view);
	has_paint = FALSE;
}

/* Brush radius in MASK pixels. */
static double mask_radius(IvEditor *ed)
{
	return brush_size / 2 * iv_brush_mask_scale(ed);
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
	double r = mask_radius(ed);
	int value = removing ? 0 : 255;

	iv_brush_line(pending->data, pending->w, pending->w, pending->h, last_mx,
		      last_my, mx, my, r, value, HARDNESS, NULL);
	iv_mask_view_line(view, last_mx, last_my, mx, my, r, value, HARDNESS);
	last_mx = mx;
	last_my = my;
}

/* Whether anything the inpainting would fill is painted. */
static gboolean mask_has_paint(void)
{
	int x, y, w, h;

	return pending && iv_mask_bbox(pending->data, pending->w, pending->h,
				       128, &x, &y, &w, &h);
}

/* The share of the picture the inpainting would fill.  The fill is made
 * from what is left, so most of the picture must be left. */
static double mask_share(void)
{
	gsize n, filled = 0;

	if (!pending)
		return 0;
	n = (gsize)pending->w * (gsize)pending->h;
	for (gsize i = 0; i < n; i++)
		filled += pending->data[i] >= 128;
	return n ? (double)filled / n : 0;
}

/* ---- applying ------------------------------------------------------------------ */

typedef struct {
	IvEditor *ed;
	IvEditState *state;	/* the committed state plus the mask */
} Apply;

static void apply_free(gpointer p)
{
	Apply *a = p;

	if (a->state)
		iv_edit_state_free(a->state);
	g_free(a);
}

static void apply_done(gpointer data, gboolean ok)
{
	Apply *a = data;
	IvEditState *s = a->state;

	/* The brush outline again in place of the busy pointer. */
	if (editor == a->ed && iv_editor_committed(a->ed) &&
	    iv_editor_current_tool(a->ed) == &iv_tool_retouch)
		iv_editor_set_cursor(a->ed, "none");

	if (!ok) {
		/* Cancelled (or the editor moved on): the painted mask stays
		 * for another try -- it is in no state any more once a's
		 * copy goes, so it may be painted again. */
		iv_edit_state_free(s);
		a->state = NULL;
		/* Said only when the editor is still there (leaving it
		 * cancels too, and then there is nobody to tell). */
		if (editor == a->ed && iv_editor_committed(a->ed)) {
			has_paint = mask_has_paint();
			iv_editor_toast(a->ed, "Retouching cancelled — Enter tries "
					       "again, Esc clears the mask");
			iv_editor_redraw_overlay(a->ed);
		}
		sync_panel();
		return;
	}
	a->state = NULL;
	iv_editor_apply(a->ed, s, "Retouch");
	/* The mask belongs to the state now; painting starts afresh. */
	g_clear_pointer(&pending, iv_mask_unref);
	iv_mask_view_clear(view);
	has_paint = FALSE;
	sync_panel();
	iv_editor_redraw_overlay(a->ed);
}

static void apply_mask(IvEditor *ed)
{
	Apply *a;

	if (iv_editor_busy(ed) || !pending)
		return;
	has_paint = mask_has_paint();
	if (!has_paint) {
		clear_mask();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return;
	}
	if (mask_share() > 0.6) {
		iv_editor_toast(ed, "Too much is painted: leave most of the picture "
				    "to rebuild the rest from");
		sync_panel();
		return;
	}
	a = g_new0(Apply, 1);
	a->ed = ed;
	a->state = iv_edit_state_copy(iv_editor_committed(ed));
	iv_edit_state_add_retouch(a->state, pending);
	iv_editor_run_render(ed, "Retouching", a->state, apply_done, a,
			     apply_free);
	/* The canvas takes no input while the job runs: the brush outline
	 * would stand still, so the pointer says the editor is busy. */
	if (iv_editor_busy(ed))
		iv_editor_set_cursor(ed, "progress");
	sync_panel();
}

/* ---- the canvas ------------------------------------------------------------------ */

static gboolean canvas_event(IvEditor *ed, GdkEvent *ev, double x, double y)
{
	double mx, my;

	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		if (ev->button.button != 1 && ev->button.button != 3)
			return FALSE;
		if (!ensure_mask(ed))
			return FALSE;
		/* Taking away only makes sense from a mask still being
		 * gathered; with Apply as you paint each stroke is applied
		 * at once, so the right button does nothing there. */
		removing = ev->button.button == 3 ||
			   (ev->button.state & GDK_MOD1_MASK) != 0;
		if (removing && auto_apply)
			return TRUE;
		painting = TRUE;
		to_mask(ed, x, y, &mx, &my);
		last_mx = mx;
		last_my = my;
		paint_to(ed, mx, my);
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
		return ev->button.button == 1 || ev->button.button == 3;
	case GDK_MOTION_NOTIFY:
		iv_editor_display_to_widget(ed, x, y, &pointer_wx, &pointer_wy);
		pointer_in = TRUE;
		if (painting && pending) {
			to_mask(ed, x, y, &mx, &my);
			paint_to(ed, mx, my);
		}
		iv_editor_redraw_overlay(ed);
		return painting;
	case GDK_BUTTON_RELEASE:
		if (!painting)
			return ev->button.button == 3;
		painting = FALSE;
		has_paint = mask_has_paint();
		if (auto_apply && has_paint)
			apply_mask(ed);
		else
			sync_panel();
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
	iv_mask_view_paint(view, cr, &m, iv_brush_mask_scale(ed), IV_MASK_TINT,
			   WASH);
	if (pointer_in && !iv_editor_busy(ed))
		iv_brush_draw_cursor(cr, pointer_wx, pointer_wy,
				     brush_size / 2 * iv_brush_matrix_scale(&m),
				     IV_CURSOR_DASHED, 0);
}

/* ---- the panel ------------------------------------------------------------------ */

/* The brush size and the switch are kept from one run to the next. */
#define SETTINGS_GROUP "Retouch"

static void store_settings(void)
{
	iv_settings_set_tool_double(SETTINGS_GROUP, "BrushSize", brush_size);
	iv_settings_set_tool_bool(SETTINGS_GROUP, "AutoApply", auto_apply);
}

static void size_changed(double v, gboolean final, gpointer data)
{
	brush_size = CLAMP(v, BRUSH_MIN, BRUSH_MAX);
	if (final)
		store_settings();
	if (editor)
		iv_editor_redraw_overlay(editor);
}

static void auto_toggled(GObject *sw, GParamSpec *ps, gpointer data)
{
	if (syncing)
		return;
	auto_apply = gtk_switch_get_active(GTK_SWITCH(sw));
	store_settings();
	/* Turning it on with a mask waiting applies the mask, which is
	 * what the switch says. */
	if (auto_apply && editor && mask_has_paint())
		apply_mask(editor);
	sync_panel();
}

static void apply_clicked(GtkButton *b, IvEditor *ed)
{
	apply_mask(ed);
}

static void clear_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_busy(ed))
		return;
	clear_mask();
	sync_panel();
	iv_editor_redraw_overlay(ed);
}

static void remove_all_answered(gboolean yes, gpointer data)
{
	IvEditor *ed = data;
	IvEditState *s;

	if (!yes || iv_editor_committed(ed)->retouch->len == 0)
		return;
	s = iv_edit_state_copy(iv_editor_committed(ed));
	g_ptr_array_set_size(s->retouch, 0);
	iv_editor_apply(ed, s, "Remove retouching");
}

static void remove_all_clicked(GtkButton *b, IvEditor *ed)
{
	if (iv_editor_busy(ed) || iv_editor_committed(ed)->retouch->len == 0)
		return;
	iv_editor_confirm(ed, "Remove all retouching?",
			  "Every spot removed on this picture comes back.  "
			  "Undo brings the retouching back again.",
			  "_Remove", "_Keep", TRUE, remove_all_answered, ed);
}

static void sync_panel(void)
{
	const IvEditState *st;
	gboolean busy;
	guint n;
	char *text;

	if (!size_row)
		return;
	busy = editor && iv_editor_busy(editor);
	syncing = TRUE;
	gtk_switch_set_active(GTK_SWITCH(auto_switch), auto_apply);
	syncing = FALSE;
	iv_brush_slider_set(size_row, brush_size);
	/* Apply and Clear mask: always without Apply as you paint, and
	 * with it while a painted mask waits (after a cancelled run). */
	gtk_widget_set_visible(manual_box, !auto_apply || (has_paint && !busy));
	if (busy)
		gtk_label_set_text(GTK_LABEL(status_label), "Retouching…");
	else if (has_paint)
		gtk_label_set_text(GTK_LABEL(status_label),
				   "Ready: Apply removes what is painted.");
	else
		gtk_label_set_text(GTK_LABEL(status_label),
				   "Paint over what should go, then Apply.");
	gtk_widget_set_sensitive(apply_button, has_paint && !busy);
	gtk_widget_set_sensitive(clear_button,
				 pending && view && !view->empty && !busy);

	st = editor ? iv_editor_committed(editor) : NULL;
	n = st ? st->retouch->len : 0;
	if (n == 0)
		text = g_strdup("No spots removed yet");
	else
		text = g_strdup_printf(n == 1 ? "%u spot removed" :
						"%u spots removed",
				       n);
	gtk_label_set_text(GTK_LABEL(count_label), text);
	g_free(text);
	gtk_widget_set_sensitive(remove_all_button, n > 0 && !busy);
}

static GtkWidget *build_panel(IvEditor *ed)
{
	GtkWidget *p = iv_panel_new("Retouch",
				    "Paint over a spot, a blemish or a small "
				    "object: it is rebuilt from what lies "
				    "around it.");
	GtkWidget *row, *buttons, *tips;

	editor = ed;
	brush_size = iv_settings_tool_double(SETTINGS_GROUP, "BrushSize", brush_size,
					     BRUSH_MIN, BRUSH_MAX);
	auto_apply = iv_settings_tool_bool(SETTINGS_GROUP, "AutoApply", auto_apply);
	iv_brush_section(p, IV_GLYPH_BRUSH, "Brush", NULL);
	size_row = iv_brush_slider("Size", BRUSH_MIN, BRUSH_MAX, brush_size, TRUE,
				   " px", size_changed, NULL);
	gtk_box_pack_start(GTK_BOX(p), size_row, FALSE, FALSE, 0);
	row = iv_panel_switch("Apply as you paint", auto_apply, &auto_switch);
	gtk_widget_set_tooltip_text(row, "Remove what is painted as soon as the "
					 "mouse button goes up");
	g_signal_connect(auto_switch, "notify::active", G_CALLBACK(auto_toggled),
			 NULL);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 4);

	/* Painting first, applying when ready. */
	manual_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	status_label = iv_panel_note(NULL);
	gtk_box_pack_start(GTK_BOX(manual_box), status_label, FALSE, FALSE, 0);
	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
	apply_button = iv_panel_button(IV_GLYPH_CHECK, "Apply");
	iv_add_class(apply_button, "iv-accent-button");
	gtk_widget_set_tooltip_text(apply_button, "Remove what is painted (Enter)");
	g_signal_connect(apply_button, "clicked", G_CALLBACK(apply_clicked), ed);
	clear_button = iv_panel_button(IV_GLYPH_CLEAR, "Clear mask");
	gtk_widget_set_tooltip_text(clear_button, "Start the painting again (Esc)");
	g_signal_connect(clear_button, "clicked", G_CALLBACK(clear_clicked), ed);
	gtk_box_pack_start(GTK_BOX(buttons), apply_button, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(buttons), clear_button, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(manual_box), buttons, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(manual_box, TRUE);
	gtk_widget_show_all(buttons);
	gtk_widget_show(status_label);
	gtk_box_pack_start(GTK_BOX(p), manual_box, FALSE, FALSE, 4);

	iv_brush_section(p, IV_GLYPH_RETOUCH, "Retouching", NULL);
	count_label = iv_panel_note(NULL);
	gtk_box_pack_start(GTK_BOX(p), count_label, FALSE, FALSE, 0);
	remove_all_button = iv_panel_button(IV_GLYPH_RESET, "Remove all retouching");
	g_signal_connect(remove_all_button, "clicked",
			 G_CALLBACK(remove_all_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), remove_all_button, FALSE, FALSE, 4);

	iv_brush_section(p, IV_GLYPH_INFO, "Tips", NULL);
	tips = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	iv_add_class(tips, "iv-brush-tips");
	gtk_box_pack_start(GTK_BOX(tips),
			   iv_brush_tip("Cover the whole spot and a little of "
					"its surroundings."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   iv_brush_tip("Remove one thing at a time: small "
					"areas come out best."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   iv_brush_tip("Zoom in for fine work; [ and ] change "
					"the brush size."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   iv_brush_tip("With Apply as you paint off, the right "
					"button takes away from the mask."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tips),
			   iv_brush_tip("Every removal is one undo step."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), tips, FALSE, FALSE, 0);
	return p;
}

/* ---- the tool ------------------------------------------------------------------------ */

static void activate(IvEditor *ed)
{
	editor = ed;
	iv_editor_set_drag_pan(ed, FALSE);
	iv_editor_set_cursor(ed, "none");
	pointer_in = FALSE;
	sync_panel();
}

static void deactivate(IvEditor *ed)
{
	/* A mask not applied is no edit; it is dropped with the tool (the
	 * picture may be about to change under it). */
	drop_mask();
	pointer_in = FALSE;
	sync_panel();
}

static void state_changed(IvEditor *ed)
{
	sync_panel();
}

static gboolean key_press(IvEditor *ed, GdkEventKey *ev)
{
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();

	if (mods & GDK_CONTROL_MASK)
		return FALSE;
	switch (ev->keyval) {
	case GDK_KEY_bracketleft:
		brush_size = CLAMP(brush_size / 1.2, BRUSH_MIN, BRUSH_MAX);
		store_settings();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_KEY_bracketright:
		brush_size = CLAMP(brush_size * 1.2, BRUSH_MIN, BRUSH_MAX);
		store_settings();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (!has_paint)
			return FALSE;
		apply_mask(ed);
		return TRUE;
	case GDK_KEY_Escape:
		/* Esc first drops a mask being gathered; the next one
		 * leaves the editor as usual. */
		if (!view || view->empty || painting)
			return FALSE;
		clear_mask();
		sync_panel();
		iv_editor_redraw_overlay(ed);
		return TRUE;
	}
	return FALSE;
}

const IvTool iv_tool_retouch = {
	.id = "retouch",
	.label = "Retouch",
	.glyph = IV_GLYPH_RETOUCH,
	.tooltip = "Retouch (R)",
	.key = GDK_KEY_r,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.canvas_event = canvas_event,
	.draw_overlay = draw_overlay,
	.state_changed = state_changed,
	.key_press = key_press,
};
