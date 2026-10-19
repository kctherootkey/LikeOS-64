// LikeOS -- display-manager core: rectangles.
//
// struct drm_rect is a rectangle by its corners: x1/y1 inclusive, x2/y2
// exclusive, so width = x2 - x1.  Plane source rectangles are in 16.16
// fixed point, destination rectangles in whole pixels.  The small
// operations are inline here; clipping a scaled source against its
// destination, scale factors and rotation are drm_rect.c's.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2011-2013 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_RECT_H
#define KERNEL_DEV_GPU_DRM_RECT_H

#include <kernel/uapi/types.h>

/* The errors the scale computations return (negated). */
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

struct drm_rect {
	int x1, y1, x2, y2;
};

/* A rectangle from position and size, as an initialiser. */
#define DRM_RECT_INIT(x, y, w, h) ((struct drm_rect){ \
		.x1 = (x), \
		.y1 = (y), \
		.x2 = (x) + (w), \
		.y2 = (y) + (h) })

/* For printing: DRM_RECT_FMT with DRM_RECT_ARG(&r) gives "WxH+X+Y".  These
 * are for a printf with the '+' flag (snprintf in the host tests, user
 * space); kprintf has none, so kernel code prints a rectangle with
 * drm_rect_debug_print(), which spells the signs itself. */
#define DRM_RECT_FMT "%dx%d%+d%+d"
#define DRM_RECT_ARG(r) drm_rect_width(r), drm_rect_height(r), (r)->x1, (r)->y1

/* The same for a 16.16 rectangle, with six decimals. */
#define DRM_RECT_FP_FMT "%d.%06ux%d.%06u%+d.%06u%+d.%06u"
#define DRM_RECT_FP_ARG(r) \
		drm_rect_width(r) >> 16, ((drm_rect_width(r) & 0xffff) * 15625) >> 10, \
		drm_rect_height(r) >> 16, ((drm_rect_height(r) & 0xffff) * 15625) >> 10, \
		(r)->x1 >> 16, (((r)->x1 & 0xffff) * 15625) >> 10, \
		(r)->y1 >> 16, (((r)->y1 & 0xffff) * 15625) >> 10

static inline void drm_rect_init(struct drm_rect *r, int x, int y,
				 int width, int height)
{
	r->x1 = x;
	r->y1 = y;
	r->x2 = x + width;
	r->y2 = y + height;
}

/* Grow (or, with negative values, shrink) by dw/dh, centred. */
static inline void drm_rect_adjust_size(struct drm_rect *r, int dw, int dh)
{
	r->x1 -= dw >> 1;
	r->y1 -= dh >> 1;
	r->x2 += (dw + 1) >> 1;
	r->y2 += (dh + 1) >> 1;
}

static inline void drm_rect_translate(struct drm_rect *r, int dx, int dy)
{
	r->x1 += dx;
	r->y1 += dy;
	r->x2 += dx;
	r->y2 += dy;
}

/* Move so that the top-left corner is at x, y. */
static inline void drm_rect_translate_to(struct drm_rect *r, int x, int y)
{
	drm_rect_translate(r, x - r->x1, y - r->y1);
}

/* Divide every coordinate (horz for x, vert for y). */
static inline void drm_rect_downscale(struct drm_rect *r, int horz, int vert)
{
	r->x1 /= horz;
	r->y1 /= vert;
	r->x2 /= horz;
	r->y2 /= vert;
}

static inline int drm_rect_width(const struct drm_rect *r)
{
	return r->x2 - r->x1;
}

static inline int drm_rect_height(const struct drm_rect *r)
{
	return r->y2 - r->y1;
}

/* Has the rectangle any area? */
static inline bool drm_rect_visible(const struct drm_rect *r)
{
	return drm_rect_width(r) > 0 && drm_rect_height(r) > 0;
}

static inline bool drm_rect_equals(const struct drm_rect *r1,
				   const struct drm_rect *r2)
{
	return r1->x1 == r2->x1 && r1->x2 == r2->x2 &&
		r1->y1 == r2->y1 && r1->y2 == r2->y2;
}

/* A 16.16 rectangle in whole numbers (truncated). */
static inline void drm_rect_fp_to_int(struct drm_rect *dst,
				      const struct drm_rect *src)
{
	drm_rect_init(dst, src->x1 >> 16, src->y1 >> 16,
		      drm_rect_width(src) >> 16,
		      drm_rect_height(src) >> 16);
}

/* Do the two rectangles share any area? */
static inline bool drm_rect_overlap(const struct drm_rect *a,
				    const struct drm_rect *b)
{
	return (a->x2 > b->x1 && b->x2 > a->x1 &&
		a->y2 > b->y1 && b->y2 > a->y1);
}

/* ---- drm_rect.c ---------------------------------------------------------- */

/* r becomes its intersection with clip; whether anything is left. */
bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *clip);
/* Clip dst against clip and src (16.16) by the same proportion, keeping
 * the scale factor (rounded toward 1.0, so clipping never turns
 * upscaling into downscaling); whether anything of dst is left. */
bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst,
			  const struct drm_rect *clip);
/* The 16.16 scale factor src/dst (rounded down below 1.0, up above it:
 * the most pessimistic value for the limit check), 0 for an empty dst,
 * -ERANGE outside [min, max], -EINVAL (with a warning) for negative
 * sizes. */
int drm_rect_calc_hscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_hscale, int max_hscale);
int drm_rect_calc_vscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_vscale, int max_vscale);
/* Log r ("[drm] <prefix>WxH+X+Y", 16.16 with six decimals when
 * fixed_point).  Unconditional: the caller decides when it is worth a
 * line. */
void drm_rect_debug_print(const char *prefix,
			  const struct drm_rect *r, bool fixed_point);
/* Rotate / reflect r inside a width x height area by `rotation'
 * (DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_*), and back. */
void drm_rect_rotate(struct drm_rect *r,
		     int width, int height,
		     unsigned int rotation);
void drm_rect_rotate_inv(struct drm_rect *r,
			 int width, int height,
			 unsigned int rotation);

#endif
