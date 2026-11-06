// LikeOS -- LikeOS Image Viewer: the editor.
//
// The editor replaces the viewer's chrome in the same window: a title row
// with the way back, a dark top bar (zoom, undo, redo, Revert at the left;
// the Done split button and Discard at the right), the tool rail at the
// left, the canvas in the middle and the current tool's options panel at
// the right.  It holds the picture being edited as a SOURCE image (full
// size) and a PROXY of it (long edge <= IV_PROXY_EDGE), the committed edit
// state with its undo history, and at most one LIVE edit -- a copy of the
// state a tool is changing right now (a slider being dragged, a stroke
// being drawn) that is shown but not yet an undo step.
//
// ===== For tool authors: the whole interface ===============================
//
// Reading the state
//   iv_editor_state(ed)       the state shown: the live edit while one is
//                             open, the committed state otherwise.  Read
//                             only; valid until the next call that changes
//                             the state.
//   iv_editor_committed(ed)   the committed state, even during a live edit.
//
// Changing the state -- live edits (previews, then one undo step)
//   s = iv_editor_begin(ed)   opens a live edit (or returns the open one):
//                             a private copy of the committed state the
//                             tool may change freely.
//   iv_editor_changed(ed)     the live copy was changed: render the
//                             preview again (coalesced: renders never pile
//                             up, the newest state is always rendered last).
//   iv_editor_commit(ed, "Brightness")
//                             the live copy becomes the committed state as
//                             one undo step with that label.  Does nothing
//                             (and makes no step) when it equals the
//                             committed state.
//   iv_editor_cancel(ed)      drops the live copy; the committed state is
//                             shown again.
//   iv_editor_apply(ed, s, "Rotate")
//                             one step at once: `s' (taken over) becomes
//                             the committed state.  An open live edit is
//                             dropped first.
// Sliders: begin on the first change, changed() on every movement, commit()
// when the button is released (the panel helpers below do exactly this).
// Masks and strokes in a state are immutable (editstate.h): to change a
// mask, iv_mask_copy() it, paint the copy, and put the copy into the live
// state in place of the old one.
//
// The pictures
//   iv_editor_source(ed)      the full-size SOURCE (IvImage, read only).
//   iv_editor_proxy(ed)       the proxy of it; iv_editor_proxy_scale() is
//                             proxy->w / source->w.  Analyses that need to
//                             be quick (background estimation, auto
//                             enhance, style thumbnails) run on the proxy.
//   iv_editor_source_size(ed, &w, &h)
//
// What the canvas shows (DISPLAY coordinates)
//   The canvas shows a render of the state with the current render flags
//   (render.h): by default the OUTPUT; the crop tool asks for
//   IV_RENDER_NO_CROP | IV_RENDER_STEADY and sees the whole straightened
//   picture with the FRAME in it (iv_render_frame_origin).  DISPLAY
//   coordinates are the full-resolution pixels of that render, whatever the
//   proxy's size; they are what canvas_event receives.  Conversions:
//     iv_editor_display_to_source / _source_to_display     masks, strokes
//     iv_editor_display_to_output / _output_to_display     the result (px)
//     iv_editor_display_to_widget / _widget_to_display     the overlay
//     iv_editor_view_scale(ed)  widget pixels per DISPLAY pixel
//   iv_editor_display_size(ed, &w, &h) is the size of the DISPLAY frame for
//   the state shown.
//   iv_editor_set_render_flags(ed, flags)   what the render shows
//   iv_editor_redraw_overlay(ed)            draw_overlay again (no render)
//   iv_editor_set_cursor(ed, "crosshair")   NULL: the default
//   iv_editor_set_drag_pan(ed, FALSE)       the first button is the tool's
//   iv_editor_set_compare(ed, TRUE)         shows the unedited picture
//
// Slow work (inpainting, background estimation)
//   iv_editor_run(ed, "Retouching", work, done, data, free)
//     runs work(data, cancel, progress, progress_data) in a worker thread
//     with a progress toast; done(data, ok) runs on the main thread after
//     (ok FALSE when it was cancelled -- by the toast's Cancel, by Esc, or
//     by leaving the editor).
//     work must call progress(fraction, progress_data) now and then and
//     stop when it returns nonzero.  One job at a time: while one runs,
//     iv_editor_busy() is TRUE and the editor's own buttons are off.
//   iv_editor_run_render(ed, "Retouching", state, done, data, free)
//     the same for rendering a state ahead into the preview's cache.
//
// Messages
//   iv_editor_toast(ed, "Background detected")
//   iv_editor_toast_action(ed, text, "Undo", cb, data)
//   iv_editor_confirm(ed, title, text, accept, cancel, destructive, done,
//                     data)            a dark confirm card over the editor
//
// Building panels (the editor's look, so all tools match)
//   iv_panel_new(title, subtitle)        the panel: a vertical box
//   iv_panel_section(panel, title, &reset)  a section header; `reset' (may
//                                        be NULL) receives its reset button
//   iv_panel_slider(...)                 label, value, slider -100..100
//                                        style; live edit wiring built in
//   iv_panel_chips(...)                  a wrapping group of toggle chips
//   iv_panel_button(glyph, label)        a full-width command button
//   iv_panel_switch(label, active)       a label and a switch
//   iv_panel_swatches(...)               colour swatches + a custom colour
//   iv_panel_note(text)                  secondary text
//
// ===========================================================================
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_EDITOR_H
#define IV_EDITOR_H

#include "canvas.h"
#include "editstate.h"
#include "imageviewer.h"
#include "render.h"
#include "tools.h"

/* ---- the editor and the window (window.c uses these) ------------------- */

IvEditor *iv_editor_new(IvWindow *w);
void iv_editor_free(IvEditor *ed);
GtkWidget *iv_editor_widget(IvEditor *ed);
/* Starts editing `p' (the viewer's picture).  The editor shows a spinner
 * until the source and the proxy are ready. */
void iv_editor_open(IvEditor *ed, IvPicture *p);
/* Unsaved edits? */
gboolean iv_editor_dirty(IvEditor *ed);
/* Leaving the editor (the back arrow, Esc, closing the window): with
 * unsaved edits it asks to save, discard or keep editing; `then' runs
 * with TRUE once the editor may go (and has dropped its edits), FALSE when
 * the user chose to keep editing. */
typedef void (*IvEditorLeave)(gboolean go, gpointer data);
void iv_editor_request_leave(IvEditor *ed, IvEditorLeave then, gpointer data);
/* Drops everything without asking (the picture vanished, ...). */
void iv_editor_close(IvEditor *ed);
gboolean iv_editor_key(IvEditor *ed, GdkEventKey *ev);
gboolean iv_editor_key_release(IvEditor *ed, GdkEventKey *ev);
const char *iv_editor_title(IvEditor *ed);	/* "Editing · name" */

/* ---- the state ------------------------------------------------------------- */

const IvEditState *iv_editor_state(IvEditor *ed);
const IvEditState *iv_editor_committed(IvEditor *ed);
IvEditState *iv_editor_begin(IvEditor *ed);
gboolean iv_editor_live(IvEditor *ed);
void iv_editor_changed(IvEditor *ed);
void iv_editor_commit(IvEditor *ed, const char *label);
void iv_editor_cancel(IvEditor *ed);
void iv_editor_apply(IvEditor *ed, IvEditState *s, const char *label);
void iv_editor_undo(IvEditor *ed);
void iv_editor_redo(IvEditor *ed);
/* Back to the unedited picture, after asking. */
void iv_editor_revert(IvEditor *ed);

/* ---- the pictures ---------------------------------------------------------- */

const IvImage *iv_editor_source(IvEditor *ed);
const IvImage *iv_editor_proxy(IvEditor *ed);
double iv_editor_proxy_scale(IvEditor *ed);
void iv_editor_source_size(IvEditor *ed, int *w, int *h);
const char *iv_editor_path(IvEditor *ed);
GtkWindow *iv_editor_window(IvEditor *ed);
IvCanvas *iv_editor_canvas(IvEditor *ed);

/* ---- the display ------------------------------------------------------------ */

void iv_editor_set_render_flags(IvEditor *ed, IvRenderFlags flags);
IvRenderFlags iv_editor_render_flags(IvEditor *ed);
void iv_editor_display_size(IvEditor *ed, int *w, int *h);
void iv_editor_display_to_source(IvEditor *ed, double dx, double dy,
				 double *sx, double *sy);
void iv_editor_source_to_display(IvEditor *ed, double sx, double sy,
				 double *dx, double *dy);
void iv_editor_display_to_output(IvEditor *ed, double dx, double dy,
				 double *ox, double *oy);
void iv_editor_output_to_display(IvEditor *ed, double ox, double oy,
				 double *dx, double *dy);
void iv_editor_display_to_widget(IvEditor *ed, double dx, double dy,
				 double *wx, double *wy);
void iv_editor_widget_to_display(IvEditor *ed, double wx, double wy,
				 double *dx, double *dy);
double iv_editor_view_scale(IvEditor *ed);
void iv_editor_redraw_overlay(IvEditor *ed);
/* The preview renders as they arrive: `cb' runs on the main thread each
 * time a new render is shown (added for panels that follow the preview,
 * such as the Adjust histogram).  iv_editor_shown_surface() is the last
 * render (NULL before the first; the editor keeps it, valid until the
 * next render) with the logical size it stands for. */
typedef void (*IvEditorRendered)(IvEditor *ed, gpointer data);
void iv_editor_add_rendered_hook(IvEditor *ed, IvEditorRendered cb,
				 gpointer data);
cairo_surface_t *iv_editor_shown_surface(IvEditor *ed, int *logical_w,
					 int *logical_h);
void iv_editor_set_cursor(IvEditor *ed, const char *name);
void iv_editor_set_drag_pan(IvEditor *ed, gboolean on);
void iv_editor_set_compare(IvEditor *ed, gboolean original);
const IvTool *iv_editor_current_tool(IvEditor *ed);

/* ---- slow work ---------------------------------------------------------------- */

typedef void (*IvEditorWork)(gpointer data, GCancellable *cancel,
			     IvProgress progress, void *progress_data);
typedef void (*IvEditorDone)(gpointer data, gboolean ok);
void iv_editor_run(IvEditor *ed, const char *label, IvEditorWork work,
		   IvEditorDone done, gpointer data, GDestroyNotify free_data);
/* A job (as iv_editor_run, with its toast and its Cancel) that renders
 * `s' (copied) over the proxy into the editor's render cache, then
 * done(data, ok).  Added for the Retouch tool: a new retouch step is
 * inpainted once, where its progress shows and it can be cancelled, and
 * the preview render after iv_editor_apply(s) finds it in the cache.  The
 * masks of `s' must be the very ones applied (the cache knows them by
 * identity). */
void iv_editor_run_render(IvEditor *ed, const char *label,
			  const IvEditState *s, IvEditorDone done,
			  gpointer data, GDestroyNotify free_data);
gboolean iv_editor_busy(IvEditor *ed);

/* ---- messages ----------------------------------------------------------------- */

void iv_editor_toast(IvEditor *ed, const char *text);
void iv_editor_toast_action(IvEditor *ed, const char *text, const char *action,
			    IvToastAction cb, gpointer data);
void iv_editor_confirm(IvEditor *ed, const char *title, const char *text,
		       const char *accept, const char *cancel,
		       gboolean destructive, IvConfirmDone done, gpointer data);

/* ---- panel building blocks ----------------------------------------------------- */

GtkWidget *iv_panel_new(const char *title, const char *subtitle);
/* Appends a section header to `panel'; returns the header row. */
GtkWidget *iv_panel_section(GtkWidget *panel, const char *title,
			    GtkWidget **reset_out);

/* A slider row.  `get' and `set' read and write the value in an edit
 * state (the slider calls them on the live copy: begin() on the first
 * movement, changed() on each, commit(label) on release); double-clicking
 * the row puts it back to `neutral'.  The returned row has the GtkScale as
 * "iv-scale" data; iv_panel_slider_sync() shows the state's value again
 * (for state_changed). */
typedef double (*IvSliderGet)(const IvEditState *s, gpointer data);
typedef void (*IvSliderSet)(IvEditState *s, double v, gpointer data);
GtkWidget *iv_panel_slider(IvEditor *ed, const char *label, double min,
			   double max, double step, double neutral,
			   IvSliderGet get, IvSliderSet set, gpointer data);
void iv_panel_slider_sync(GtkWidget *row);

/* Toggle chips in rows that wrap; `chosen(index, data)' on a click.  The
 * container has the chips as children in order. */
typedef void (*IvChipChosen)(int index, gpointer data);
GtkWidget *iv_panel_chips(const char *const *labels, int n, int active,
			  IvChipChosen chosen, gpointer data);
void iv_panel_chips_set(GtkWidget *chips, int active);

GtkWidget *iv_panel_button(IvGlyph g, const char *label);
GtkWidget *iv_panel_switch(const char *label, gboolean active,
			   GtkWidget **switch_out);
/* Swatches of `colors' (0xRRGGBBAA) and a custom-colour button at the end;
 * `picked(rgba, data)' when one is chosen. */
typedef void (*IvSwatchPicked)(guint32 rgba, gpointer data);
GtkWidget *iv_panel_swatches(const guint32 *colors, int n, guint32 current,
			     IvSwatchPicked picked, gpointer data);
void iv_panel_swatches_set(GtkWidget *swatches, guint32 rgba);
GtkWidget *iv_panel_note(const char *text);

#endif
