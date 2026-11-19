// LikeOS -- LikeOS Image Viewer: the canvas.
//
// The widget a picture is shown on, in the viewer and in the editor.  It
// keeps the view -- the zoom (fit to the window, or a fixed scale) and the
// pan -- and draws the picture from the prescaled level that suits the
// zoom, over the backdrop the settings name, with the checkerboard behind
// transparent pixels.  It plays animated pictures, cross-fades from one
// picture to the next, shows a spinner while the next one loads and a
// message when there is nothing to show.
//
// Coordinates.  "Widget" coordinates are the canvas's own pixels.  "Image"
// coordinates are the pixels of what is shown, in its LOGICAL size: the
// picture's own size in the viewer; in the editor, the full-resolution size
// of what the editor displays, even when the surface it hands over is a
// smaller proxy.  Zoom is widget pixels per image pixel, 1.0 = 100 %.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_CANVAS_H
#define IV_CANVAS_H

#include "imageviewer.h"

#define IV_TYPE_CANVAS (iv_canvas_get_type())
G_DECLARE_FINAL_TYPE(IvCanvas, iv_canvas, IV, CANVAS, GtkDrawingArea)

/* The canvas as a plain widget. */
#define IV_CANVAS_GTK(c) ((GtkWidget *)(c))

IvCanvas *iv_canvas_new(void);

#define IV_ZOOM_MIN 0.02
#define IV_ZOOM_MAX 32.0

/* ---- what is shown ----------------------------------------------------- */

/* The viewer's picture.  `crossfade': fade from what was shown. The view
 * returns to fit unless `keep_view'. */
void iv_canvas_set_picture(IvCanvas *c, IvPicture *p, gboolean crossfade,
			   gboolean keep_view);
IvPicture *iv_canvas_get_picture(IvCanvas *c);
/* The editor's display: a surface (the canvas keeps a ref) standing for
 * an image of logical_w x logical_h.  When the logical size is unchanged
 * the view stays as it is; otherwise it returns to fit unless
 * `keep_view'. */
void iv_canvas_set_surface(IvCanvas *c, cairo_surface_t *s, int logical_w,
			   int logical_h, gboolean keep_view);
/* Nothing shown, with a message (glyph, title, detail; NULLs allowed). */
void iv_canvas_set_message(IvCanvas *c, IvGlyph g, const char *title,
			   const char *detail);
/* The spinner over what is shown (appears after a short delay). */
void iv_canvas_set_loading(IvCanvas *c, gboolean loading);
void iv_canvas_get_image_size(IvCanvas *c, int *w, int *h);
gboolean iv_canvas_has_image(IvCanvas *c);
void iv_canvas_set_backdrop(IvCanvas *c, IvBackdrop b);
/* Animated pictures: playing unless paused. */
void iv_canvas_set_playing(IvCanvas *c, gboolean playing);

/* ---- the view -------------------------------------------------------------- */

double iv_canvas_get_zoom(IvCanvas *c);
gboolean iv_canvas_is_fit(IvCanvas *c);
/* The zoom fit-to-window gives (never above 100 % for pictures that fit
 * already). */
double iv_canvas_fit_zoom(IvCanvas *c);
/* Zooms to `zoom', keeping the image point under widget point (ax, ay)
 * where it is; ax < 0 means the middle of the canvas.  Animated when the
 * settings ask for smooth zoom and `animate'. */
void iv_canvas_zoom_to(IvCanvas *c, double zoom, double ax, double ay,
		       gboolean animate);
/* One step in or out (the steps of the zoom menu and between them). */
void iv_canvas_zoom_step(IvCanvas *c, int direction, double ax, double ay);
void iv_canvas_zoom_fit(IvCanvas *c, gboolean animate);
void iv_canvas_zoom_actual(IvCanvas *c, double ax, double ay);
/* Fit <-> 100 %. */
void iv_canvas_toggle_fit(IvCanvas *c, double ax, double ay);
/* Moves the view by widget pixels. */
void iv_canvas_pan(IvCanvas *c, double dx, double dy);
/* Allows (or stops) panning by dragging with the first button.  The
 * editor's tools turn it off while they want the pointer. */
void iv_canvas_set_drag_pan(IvCanvas *c, gboolean on);
/* Room at the edges the view keeps clear (floating chrome), widget px. */
void iv_canvas_set_insets(IvCanvas *c, int top, int right, int bottom,
			  int left);

/* ---- coordinates --------------------------------------------------------------- */

void iv_canvas_widget_to_image(IvCanvas *c, double wx, double wy, double *ix,
			       double *iy);
void iv_canvas_image_to_widget(IvCanvas *c, double ix, double iy, double *wx,
			       double *wy);
/* Where the image lies on the widget. */
void iv_canvas_image_rect(IvCanvas *c, GdkRectangle *r);
void iv_canvas_image_rect_d(IvCanvas *c, double *x, double *y, double *w,
			    double *h);

/* ---- hooks for the editor ------------------------------------------------------ */

/* Drawn over the picture, in widget coordinates. */
typedef void (*IvCanvasOverlay)(IvCanvas *c, cairo_t *cr, gpointer data);
/* Every button, motion and scroll event, with its position in image
 * coordinates; TRUE when handled (the canvas then does not pan or zoom
 * with it). */
typedef gboolean (*IvCanvasEvent)(IvCanvas *c, GdkEvent *ev, double ix,
				  double iy, gpointer data);
void iv_canvas_set_hooks(IvCanvas *c, IvCanvasOverlay overlay,
			 IvCanvasEvent event, gpointer data);
/* The cursor over the image (NULL = the default hand/arrow). */
void iv_canvas_set_cursor_name(IvCanvas *c, const char *name);

/* ---- signals ------------------------------------------------------------------- */
/*
 *  "view-changed"     ()          zoom or pan changed
 *  "image-activated"  ()          the image was clicked once (no drag)
 *  "navigate"         (int)       the wheel asked for the previous (-1) /
 *                                 next (+1) picture (wheel set to browse)
 *  "context-menu"     (GdkEvent*) right click
 */

#endif
