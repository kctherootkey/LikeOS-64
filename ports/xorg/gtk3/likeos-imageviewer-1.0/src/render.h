// LikeOS -- LikeOS Image Viewer: the render pipeline.
//
// Makes pixels from a source picture and an edit state.  The order is
// fixed: first what works on the source pixels, then the geometry, then
// the drawing on top --
//
//   source -> retouch (each mask inpainted in turn, results cached)
//          -> adjust -> style -> background operation
//          -> turn / mirror -> straighten -> crop
//          -> drawn strokes (cairo) -> output
//
// The same function renders the editor's preview from a proxy of the
// source (its long edge at most IV_PROXY_EDGE pixels, the masks scaled to
// match) and the full-size picture when saving; both run in worker threads
// and stop early when their GCancellable is cancelled.  Nothing here
// touches GTK.
//
// What a render shows can be reduced with flags, for tools that need to see
// more than the result: the crop tool wants the whole straightened picture
// with its crop drawn as an overlay, the draw tool wants the picture
// without the strokes it draws live itself.  The iv_render_display_*
// functions give the size of what a render with given flags shows and
// convert points between it and the SOURCE and OUTPUT frames.
//
// The strokes are attached to the picture (SOURCE coordinates,
// editstate.h): every render that shows them maps them through the same
// geometry as the pixels, so they turn, mirror, straighten and get cut
// with what they were drawn on.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_RENDER_H
#define IV_RENDER_H

#include <cairo.h>
#include <gio/gio.h>

#include "editstate.h"

#define IV_PROXY_EDGE 2048

typedef enum {
	IV_RENDER_ALL = 0,
	/* Show the whole FRAME, not cut to the crop. */
	IV_RENDER_NO_CROP = 1 << 0,
	/* With NO_CROP: the TURNED frame, not straightened (no strokes). */
	IV_RENDER_NO_STRAIGHTEN = 1 << 1,
	/* Without the drawn strokes. */
	IV_RENDER_NO_DRAW = 1 << 2,
	/* The SOURCE frame: no geometry at all (and no strokes). */
	IV_RENDER_NO_GEOMETRY = 1 << 3,
	/* Without the background operation. */
	IV_RENDER_NO_BACKGROUND = 1 << 4,
	/* Without the style and the adjustments (the picture's own colours,
	 * for tools that pick colours from it). */
	IV_RENDER_NO_COLOR = 1 << 5,
	/* With NO_CROP: the straightened picture uncut, on a canvas the size
	 * of the TURNED frame with the FRAME centred in it.  The size of
	 * what is shown no longer depends on the angle, so the view keeps
	 * its scale while the straighten dial turns: the picture turns
	 * inside a steady canvas (its corners past the canvas cut off, the
	 * uncovered ones transparent) and the FRAME shrinks within it.
	 * Added for the crop tool. */
	IV_RENDER_STEADY = 1 << 6,
} IvRenderFlags;

/* Where the FRAME lies in what a render with `flags' shows: (0, 0) except
 * for IV_RENDER_STEADY, whose FRAME is centred in the TURNED canvas. */
void iv_render_frame_origin(const IvEditState *st, int sw, int sh,
			    IvRenderFlags flags, double *x, double *y);

/* What a render with `flags' shows, for a sw x sh source. */
void iv_render_display_size(const IvEditState *st, int sw, int sh,
			    IvRenderFlags flags, int *dw, int *dh);
void iv_render_display_to_source(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double dx, double dy,
				 double *sx, double *sy);
void iv_render_source_to_display(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double sx, double sy,
				 double *dx, double *dy);
/* OUTPUT pixels (not normalised). */
void iv_render_display_to_output(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double dx, double dy,
				 double *ox, double *oy);
void iv_render_output_to_display(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double ox, double oy,
				 double *dx, double *dy);

/* Results kept between renders: the retouch steps (inpainting is slow),
 * and masks scaled to the render's size.  Safe to share between
 * threads. */
typedef struct IvRenderCache IvRenderCache;
IvRenderCache *iv_render_cache_new(void);
void iv_render_cache_free(IvRenderCache *c);
void iv_render_cache_clear(IvRenderCache *c);

/* Renders `st' over `src', which is the SOURCE at some scale (src->w is
 * about sw * scale).  The result is what the flags show, at the same
 * scale.  NULL when cancelled (or out of memory).  `progress' (may be
 * NULL) hears how far the slow steps are, 0..1 over the whole render. */
IvImage *iv_render(const IvImage *src, int sw, int sh, const IvEditState *st,
		   IvRenderFlags flags, IvRenderCache *cache, GCancellable *cancel,
		   IvProgress progress, void *progress_user);

/* How a render went (iv_render_ex). */
typedef struct {
	gboolean cancelled;
	/* The step that could not be done for want of memory, as the
	 * subject of a sentence ("Retouching", "The adjustments"); NULL
	 * when every step was done. */
	const char *failed;
} IvRenderOutcome;

/* iv_render, telling why there is no result.  A step that runs out of
 * memory is never left out quietly: the render stops and returns NULL
 * with `out->failed' set (iv_render does the same, without saying). */
IvImage *iv_render_ex(const IvImage *src, int sw, int sh,
		      const IvEditState *st, IvRenderFlags flags,
		      IvRenderCache *cache, GCancellable *cancel,
		      IvProgress progress, void *progress_user,
		      IvRenderOutcome *out);

/* A stroke on cairo.  Its points are fractions of w x h in cairo's user
 * space and its width a fraction of the longer of the two: with w x h the
 * SOURCE size and the user space SOURCE pixels (cairo's matrix mapping
 * them to the device), that is the stroke where it belongs on the
 * picture.  The pipeline draws all strokes this way, the draw tool the one
 * being drawn and its panel's sample. */
void iv_render_draw_stroke(cairo_t *cr, const IvStroke *s, double w,
			   double h);
void iv_render_draw_strokes(cairo_t *cr, GPtrArray *strokes, double w,
			    double h);
/* The affine map from SOURCE pixels to the pixels of what a render with
 * `flags' shows, at full resolution (multiply by the render's scale for a
 * proxy render).  FALSE without a picture (sw or sh 0). */
gboolean iv_render_source_matrix(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, cairo_matrix_t *m);

/* The source at proxy size: long edge at most IV_PROXY_EDGE. */
IvImage *iv_render_make_proxy(const IvImage *src);

#endif
