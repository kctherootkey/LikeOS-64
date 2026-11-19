// LikeOS -- LikeOS Image Viewer: what the brush tools share.
//
// Draw, Retouch and Background all put the pointer to work on the picture:
// they need to know where a SOURCE or OUTPUT pixel lies on the canvas
// whatever the zoom, the pan and the geometry are, they paint soft round
// dabs along the pointer's path into masks, they show those masks over the
// picture while painting (an accent tint, or a diagonal hatching) and they
// draw a brush outline in place of the pointer.  Their panels also have
// value sliders that belong to the tool rather than to the edit state (the
// brush size), which iv_panel_slider() cannot be used for.
//
// Masks are kept at MASK resolution: the SOURCE size, or for very large
// pictures the SOURCE scaled down to IV_BRUSH_MASK_EDGE pixels on the long
// edge -- the render pipeline resizes a mask to whatever size it renders
// at, so a mask of any size with the source's aspect ratio is a mask in
// SOURCE coordinates.  What is shown on the canvas while painting is a
// second, smaller copy (an IvMaskView, long edge at most
// IV_BRUSH_VIEW_EDGE) that is painted with the very same dabs, so the
// overlay never has to be rebuilt from the full mask on a pointer motion.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_TOOL_BRUSH_H
#define IV_TOOL_BRUSH_H

#include "editor.h"

#define IV_BRUSH_MASK_EDGE 4096
#define IV_BRUSH_VIEW_EDGE 2048

/* ---- where things are on the canvas -------------------------------------- */

/* The affine map from SOURCE pixels to canvas widget pixels for the view
 * and the geometry shown right now (every geometry step is affine, so three
 * points settle it).  FALSE while there is no picture. */
gboolean iv_brush_source_matrix(IvEditor *ed, cairo_matrix_t *m);
/* The same for OUTPUT pixels (the Draw tool's frame). */
gboolean iv_brush_output_matrix(IvEditor *ed, cairo_matrix_t *m);
/* How many widget pixels one unit of the matrix's input is (the square
 * root of its determinant: the same in every direction for the maps above,
 * which only turn, mirror and scale). */
double iv_brush_matrix_scale(const cairo_matrix_t *m);
/* The OUTPUT size at full resolution. */
void iv_brush_output_size(IvEditor *ed, int *ow, int *oh);

/* ---- masks ---------------------------------------------------------------- */

/* The MASK resolution for the editor's picture (see above). */
void iv_brush_mask_size(IvEditor *ed, int *mw, int *mh);
/* MASK pixels per SOURCE pixel (1 for all but very large pictures). */
double iv_brush_mask_scale(IvEditor *ed);
/* A new, empty mask at MASK resolution (NULL without a picture). */
IvMask *iv_brush_mask_new(IvEditor *ed);
/* `m' at MASK resolution: `m' itself (one more ref) when it has that size
 * already, else a resized copy.  NULL for NULL. */
IvMask *iv_brush_mask_conform(IvEditor *ed, IvMask *m);

/* Soft round dabs from (x0, y0) to (x1, y1), radius r, spaced a fraction
 * of the radius apart so the line has no beads, moving the mask towards
 * `value' (iv_mask_dab).  `stride' is the length of a row in bytes (the
 * width for plain masks).  The rectangle that changed is merged into
 * `dirty' when it is not NULL (its width 0 = nothing yet). */
void iv_brush_line(uint8_t *mask, int stride, int w, int h, double x0,
		   double y0, double x1, double y1, double r, int value,
		   double hardness, GdkRectangle *dirty);

/* ---- the mask shown over the picture ---------------------------------------- */

typedef enum {
	IV_MASK_TINT,		/* a translucent accent wash */
	IV_MASK_HATCH,		/* diagonal accent lines over a faint wash */
} IvMaskLook;

typedef struct {
	int mw, mh;		/* the MASK size it stands for */
	int w, h, stride;	/* its own size */
	double k;		/* its pixels per MASK pixel */
	uint8_t *data;		/* stride * h, also the surface's pixels */
	cairo_surface_t *surface;	/* CAIRO_FORMAT_A8 over data */
	gboolean empty;		/* nothing painted (a hint, not exact) */
} IvMaskView;

IvMaskView *iv_mask_view_new(int mask_w, int mask_h);
void iv_mask_view_free(IvMaskView *v);
void iv_mask_view_clear(IvMaskView *v);
/* Shows `mask' (mw x mh, MASK resolution; NULL clears). */
void iv_mask_view_load(IvMaskView *v, const uint8_t *mask);
/* The same dabs as iv_brush_line on the MASK, in MASK coordinates. */
void iv_mask_view_line(IvMaskView *v, double x0, double y0, double x1,
		       double y1, double r, int value, double hardness);
/* Paints the view over the canvas.  `src_to_widget' is
 * iv_brush_source_matrix(); `mask_scale' iv_brush_mask_scale();
 * `accent' the colour (0xRRGGBBAA, its alpha the strength of the wash). */
void iv_mask_view_paint(IvMaskView *v, cairo_t *cr,
			const cairo_matrix_t *src_to_widget, double mask_scale,
			IvMaskLook look, guint32 accent);

/* ---- the brush outline ---------------------------------------------------- */

typedef enum {
	IV_CURSOR_RING,		/* a solid ring: a pen of that size */
	IV_CURSOR_DASHED,	/* a dashed ring: a mask brush */
	IV_CURSOR_ERASER,	/* a ring with a soft fill: the eraser */
} IvCursorLook;

/* A ring of widget radius `r' centred on (x, y), drawn light over dark so
 * it shows on any picture; small rings get a cross in the middle so the
 * hot spot is never lost.  `fill' (0xRRGGBBAA, 0 = none) tints the inside
 * (the pen's colour). */
void iv_brush_draw_cursor(cairo_t *cr, double x, double y, double r,
			  IvCursorLook look, guint32 fill);
/* The nib of the calligraphy pen: a bar of widget length `len' at the
 * nib's angle (radians, clockwise from the x axis). */
void iv_brush_draw_nib(cairo_t *cr, double x, double y, double len,
		       double angle, guint32 fill);

/* ---- tool-owned sliders ----------------------------------------------------- */

/* A slider row looking like iv_panel_slider()'s, for a value that belongs
 * to the tool: label, value text (`unit' after the number, e.g. " px";
 * "%" ...), the scale.  `logarithmic' spreads the low values out (brush
 * sizes).  `changed(value, final, data)': `final' is TRUE when the button
 * was released (or the value set by keys), FALSE while dragging. */
typedef void (*IvBrushSliderChanged)(double value, gboolean final,
				     gpointer data);
GtkWidget *iv_brush_slider(const char *label, double min, double max,
			   double value, gboolean logarithmic, const char *unit,
			   IvBrushSliderChanged changed, gpointer data);
/* Shows `value' (no `changed' call). */
void iv_brush_slider_set(GtkWidget *row, double value);
double iv_brush_slider_get(GtkWidget *row);
/* The text after the value ("12 px"), for sliders whose unit is not the
 * slider's own value (the Draw tool shows the line width it stands for). */
void iv_brush_slider_set_text(GtkWidget *row, const char *text);

/* ---- small things ---------------------------------------------------------------- */

/* A panel section header with a glyph before its title (the look of the
 * other tools' sections); `reset' as for iv_panel_section(). */
GtkWidget *iv_brush_section(GtkWidget *panel, IvGlyph g, const char *title,
			    GtkWidget **reset);
/* A caption of secondary text above a group of controls ("Colour"). */
GtkWidget *iv_brush_caption(const char *text);
/* A bulleted tip ("• Paint a little beyond the spot"). */
GtkWidget *iv_brush_tip(const char *text);
/* Shows the picture's checkerboard-and-image thumbnail `s' scaled to cover
 * w x h with rounded corners (radius r). */
void iv_brush_paint_thumb(cairo_t *cr, cairo_surface_t *s, double x, double y,
			  double w, double h, double r);
/* 0xRRGGBBAA to cairo's source. */
void iv_brush_set_rgba(cairo_t *cr, guint32 c, double alpha_scale);

#endif
