// LikeOS -- LikeOS Image Viewer: the edit state.
//
// Everything the editor does to a picture is a description, not pixels:
// an IvEditState says how to turn, mirror, straighten and crop the picture,
// which adjustments and which style to apply, which spots were retouched,
// what happens to the background and what was drawn on top.  The pixels
// are made from the source and the description by the render pipeline
// (render.h) whenever they are needed -- small for the preview, full size
// for saving -- so every edit stays undoable and nothing is resampled
// twice.
//
// ---- coordinate frames (all in FULL-RESOLUTION units) --------------------
//
//   SOURCE   the picture as loaded and shown by the viewer (its EXIF
//            orientation already applied), sw x sh pixels.  Retouch and
//            background masks live here, and so do the drawn strokes
//            (normalised 0..1 of sw and sh): they stay on what they were
//            drawn on whatever is turned, straightened or cropped later.
//   TURNED   SOURCE turned by geo.quarter_turns (clockwise), then mirrored
//            by geo.flip_h / geo.flip_v (mirrors act on the turned frame:
//            flip_h always mirrors what is left/right on the screen).
//   FRAME    TURNED straightened by geo.straighten degrees (clockwise
//            positive) about its centre and cut to the largest rectangle
//            of the same aspect ratio that has no empty corners
//            (iv_straighten_inner_rect_aspect), fw x fh pixels (fractional).
//   OUTPUT   FRAME cut to the crop rectangle geo.crop_* (normalised 0..1 of
//            FRAME), ow x oh pixels: what is saved.
//
// The iv_geometry_* functions convert points between these frames exactly;
// tools never need to know the order of operations themselves.
//
// ---- ownership -------------------------------------------------------------
//
// IvEditState is a value: iv_edit_state_copy() makes an independent copy
// that can be changed and freed without affecting the original.  The masks
// and strokes in its lists are refcounted and IMMUTABLE once they
// are in a state: two copies share them, and a tool that wants a different
// mask makes a new one (iv_mask_copy) and puts that in its place.  Only a
// mask or stroke a tool has just made and not yet put into any state may
// be written to.  Everything here may be used from any thread as long as
// one state is not used by two threads at once.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_EDITSTATE_H
#define IV_EDITSTATE_H

#include <glib.h>
#include <stdint.h>

#include "ivproc.h"

/* ---- crop aspect ratios ------------------------------------------------- */

typedef enum {
	IV_ASPECT_FREE = 0,
	IV_ASPECT_ORIGINAL,
	IV_ASPECT_SQUARE,
	IV_ASPECT_16_9,
	IV_ASPECT_9_16,
	IV_ASPECT_4_3,
	IV_ASPECT_3_4,
	IV_ASPECT_3_2,
	IV_ASPECT_2_3,
	IV_ASPECT_5_4,
	IV_ASPECT_4_5,
	IV_ASPECT_7_5,
	IV_ASPECT_5_7,
	IV_ASPECT_2_1,
	IV_ASPECT_1_2,
	IV_ASPECT_COUNT
} IvAspect;

/* "Free", "Original", "Square", "16:9" ... */
const char *iv_aspect_label(IvAspect a);
/* Width / height of the ratio; 0 for Free.  Original is the frame's own
 * ratio, fw / fh. */
double iv_aspect_ratio(IvAspect a, double fw, double fh);
/* The same ratio the other way round (16:9 <-> 9:16); Free, Original and
 * Square are their own. */
IvAspect iv_aspect_swapped(IvAspect a);

/* ---- geometry ----------------------------------------------------------- */

typedef struct {
	int quarter_turns;	/* 0..3, clockwise */
	gboolean flip_h;	/* mirror left <-> right (after the turns) */
	gboolean flip_v;	/* mirror top <-> bottom (after the turns) */
	double straighten;	/* degrees, -45..45, clockwise positive */
	double crop_x, crop_y;	/* top-left of the crop, 0..1 of FRAME */
	double crop_w, crop_h;	/* its size, 0..1 of FRAME; 1, 1 = whole */
	IvAspect aspect;	/* the ratio the crop tool keeps */
} IvGeometry;

void iv_geometry_reset(IvGeometry *g);
gboolean iv_geometry_is_identity(const IvGeometry *g);
gboolean iv_geometry_has_crop(const IvGeometry *g);
gboolean iv_geometry_equal(const IvGeometry *a, const IvGeometry *b);

/* Sizes of the frames for a sw x sh source. */
void iv_geometry_turned_size(const IvGeometry *g, int sw, int sh, int *tw,
			     int *th);
void iv_geometry_frame_size(const IvGeometry *g, int sw, int sh, double *fw,
			    double *fh);
void iv_geometry_output_size(const IvGeometry *g, int sw, int sh, int *ow,
			     int *oh);
/* The crop rectangle in FRAME pixels. */
void iv_geometry_crop_rect(const IvGeometry *g, int sw, int sh, double *x,
			   double *y, double *w, double *h);

/* Points between the frames (any values, also outside the picture). */
void iv_geometry_source_to_turned(const IvGeometry *g, int sw, int sh,
				  double sx, double sy, double *tx, double *ty);
void iv_geometry_turned_to_source(const IvGeometry *g, int sw, int sh,
				  double tx, double ty, double *sx, double *sy);
void iv_geometry_turned_to_frame(const IvGeometry *g, int sw, int sh,
				 double tx, double ty, double *fx, double *fy);
void iv_geometry_frame_to_turned(const IvGeometry *g, int sw, int sh,
				 double fx, double fy, double *tx, double *ty);
void iv_geometry_frame_to_output(const IvGeometry *g, int sw, int sh,
				 double fx, double fy, double *ox, double *oy);
void iv_geometry_output_to_frame(const IvGeometry *g, int sw, int sh,
				 double ox, double oy, double *fx, double *fy);
void iv_geometry_source_to_output(const IvGeometry *g, int sw, int sh,
				  double sx, double sy, double *ox, double *oy);
void iv_geometry_output_to_source(const IvGeometry *g, int sw, int sh,
				  double ox, double oy, double *sx, double *sy);

/* ---- masks ----------------------------------------------------------------- */

typedef struct {
	gint ref;
	int w, h;		/* SOURCE size (or whatever it was made for) */
	uint8_t *data;		/* w * h bytes, 0..255 */
} IvMask;

IvMask *iv_mask_new(int w, int h);		/* all 0 */
IvMask *iv_mask_ref(IvMask *m);
void iv_mask_unref(IvMask *m);
IvMask *iv_mask_copy(const IvMask *m);		/* a new, writable mask */
IvMask *iv_mask_resized(const IvMask *m, int w, int h); /* new, writable */
gboolean iv_mask_is_empty(const IvMask *m);

/* ---- strokes --------------------------------------------------------------- */

typedef enum {
	IV_STROKE_PEN = 0,	/* solid, round */
	IV_STROKE_MARKER,	/* translucent, broad, flat ends */
	IV_STROKE_CALLIGRAPHY,	/* width follows the direction (a broad nib) */
	IV_STROKE_RECT,		/* a filled rectangle: pts are its four corners */
} IvStrokeKind;

typedef struct {
	gint ref;
	IvStrokeKind kind;
	guint32 rgba;		/* 0xRRGGBBAA */
	double width;		/* line width, fraction of SOURCE's long edge */
	int n;			/* points */
	double *pts;		/* x0, y0, x1, y1 ... normalised SOURCE 0..1 */
	/* IV_STROKE_RECT: n is 4, the corners in order round the rectangle
	 * (top left, top right, bottom right, bottom left as it was drawn on
	 * the screen), and the width is 0.  Being corners on the SOURCE, the
	 * rectangle turns and straightens with the picture like any stroke. */
	int cap;		/* allocated points */
	/* IV_STROKE_CALLIGRAPHY: the direction of the nib's edge in SOURCE
	 * space, radians clockwise from the x axis (y points down); new
	 * strokes start at -pi/4, rising at 45 degrees.  The draw tool sets
	 * it so that the nib looks the same on the screen however the
	 * picture is turned or mirrored, and it then turns with the
	 * picture. */
	double angle;
} IvStroke;

IvStroke *iv_stroke_new(IvStrokeKind kind, guint32 rgba, double width);
IvStroke *iv_stroke_ref(IvStroke *s);
void iv_stroke_unref(IvStroke *s);
/* Only while the stroke is being drawn (not yet in a state). */
void iv_stroke_add_point(IvStroke *s, double x, double y);
/* The stroke's bounding box, normalised, widened by its width; `aspect'
 * is the width / height of the frame the points are normalised to. */
void iv_stroke_bounds(const IvStroke *s, double aspect, double *x0,
		      double *y0, double *x1, double *y1);

/* ---- the state ------------------------------------------------------------- */

typedef struct {
	gboolean active;	/* an operation is set */
	IvMask *mask;		/* SOURCE coordinates, 255 = background */
	IvBgOp op;
	double strength;	/* IV_BG_BLUR: 0..100 */
	guint32 rgba;		/* IV_BG_REPLACE: 0xRRGGBBAA */
} IvBackgroundEdit;

typedef struct {
	IvGeometry geo;
	IvAdjust adjust;
	IvStyle style;
	double style_intensity;	/* 0..1 */
	GPtrArray *retouch;	/* IvMask*, SOURCE coordinates, in order */
	IvBackgroundEdit bg;
	GPtrArray *strokes;	/* IvStroke*, in drawing order */
} IvEditState;

IvEditState *iv_edit_state_new(void);		/* the identity */
IvEditState *iv_edit_state_copy(const IvEditState *s);
void iv_edit_state_free(IvEditState *s);
/* Same description (masks and strokes compared by identity). */
gboolean iv_edit_state_equal(const IvEditState *a, const IvEditState *b);
/* Nothing would change the picture. */
gboolean iv_edit_state_is_identity(const IvEditState *s);
/* Copies every part of `src' into `dst' (replacing what was there). */
void iv_edit_state_assign(IvEditState *dst, const IvEditState *src);

/* List changes; each holds the item itself (a ref of its own). */
void iv_edit_state_add_retouch(IvEditState *s, IvMask *m);
void iv_edit_state_add_stroke(IvEditState *s, IvStroke *st);
void iv_edit_state_remove_stroke(IvEditState *s, guint index);
/* Puts `st' (a ref of its own) in the place of the stroke at `index'. */
void iv_edit_state_replace_stroke(IvEditState *s, guint index, IvStroke *st);
/* The index of `st' in the state's strokes, or -1. */
int iv_edit_state_find_stroke(const IvEditState *s, const IvStroke *st);
void iv_edit_state_clear_strokes(IvEditState *s);
void iv_edit_state_set_bg_mask(IvEditState *s, IvMask *m); /* NULL clears */

/* ---- history ---------------------------------------------------------------- */

#define IV_HISTORY_MAX 100

typedef struct IvHistory IvHistory;

IvHistory *iv_history_new(void);
void iv_history_free(IvHistory *h);
void iv_history_clear(IvHistory *h);
/* A new state replaced `before' (taken over): it goes on the undo stack
 * under `label' ("Brightness"), the redo stack is emptied.  The oldest
 * entries go past IV_HISTORY_MAX. */
void iv_history_push(IvHistory *h, IvEditState *before, const char *label);
/* Undo: `current' (taken over) goes on the redo stack and the state to go
 * back to is returned (the caller owns it).  With nothing to undo, NULL is
 * returned and `current' is NOT taken: the caller still owns it.  Redo is
 * the same the other way. */
IvEditState *iv_history_undo(IvHistory *h, IvEditState *current);
IvEditState *iv_history_redo(IvHistory *h, IvEditState *current);
gboolean iv_history_can_undo(IvHistory *h);
gboolean iv_history_can_redo(IvHistory *h);
const char *iv_history_undo_label(IvHistory *h);
const char *iv_history_redo_label(IvHistory *h);
guint iv_history_depth(IvHistory *h);

#endif
