// LikeOS -- display-manager core: rectangle clipping, scaling, rotation.
//
// Intersecting rectangles, clipping a scaled 16.16 source together with
// its destination, computing and checking scale factors, and rotating or
// reflecting a rectangle inside an area (and back).
//
// Pure apart from drm_rect_debug_print (kprintf): fixed-width types and
// integer arithmetic only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2011-2013 Intel Corporation

#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>
#include <kernel/dev/gpu/gpu_util.h>

bool drm_rect_intersect(struct drm_rect *r1, const struct drm_rect *r2)
{
	r1->x1 = max(r1->x1, r2->x1);
	r1->y1 = max(r1->y1, r2->y1);
	r1->x2 = min(r1->x2, r2->x2);
	r1->y2 = min(r1->y2, r2->y2);

	return drm_rect_visible(r1);
}

/*
 * How much of a source extent `src' (16.16) is left when `*clip' of the
 * `dst' destination pixels it is shown on are cut away.  `*clip' is first
 * limited to `dst': what lies beyond the destination is not there to
 * clip, and an unbounded clip would make the result negative (and, as an
 * unsigned width, huge -- a source that looks visible when it should be
 * gone).
 */
static u32 clip_scaled(int src, int dst, int *clip)
{
	u64 tmp;

	if (dst == 0)
		return 0;

	*clip = min(*clip, dst);

	tmp = (u64)(u32)src * (u32)(dst - *clip);

	/*
	 * Round toward a scale of exactly 1.0: rounding the other way would
	 * turn an upscaling plane into a downscaling one (or the reverse)
	 * just because it was clipped, and the scale check that follows
	 * would then refuse a plane the hardware can show.
	 */
	if (src < (dst << 16))
		return (u32)DIV_ROUND_UP_ULL(tmp, (u32)dst);
	else
		return (u32)(tmp / (u32)dst);
}

bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst,
			  const struct drm_rect *clip)
{
	int diff;

	diff = clip->x1 - dst->x1;
	if (diff > 0) {
		u32 new_src_w = clip_scaled(drm_rect_width(src),
					    drm_rect_width(dst), &diff);

		src->x1 = src->x2 - (int)new_src_w;
		dst->x1 += diff;
	}
	diff = clip->y1 - dst->y1;
	if (diff > 0) {
		u32 new_src_h = clip_scaled(drm_rect_height(src),
					    drm_rect_height(dst), &diff);

		src->y1 = src->y2 - (int)new_src_h;
		dst->y1 += diff;
	}
	diff = dst->x2 - clip->x2;
	if (diff > 0) {
		u32 new_src_w = clip_scaled(drm_rect_width(src),
					    drm_rect_width(dst), &diff);

		src->x2 = src->x1 + (int)new_src_w;
		dst->x2 -= diff;
	}
	diff = dst->y2 - clip->y2;
	if (diff > 0) {
		u32 new_src_h = clip_scaled(drm_rect_height(src),
					    drm_rect_height(dst), &diff);

		src->y2 = src->y1 + (int)new_src_h;
		dst->y2 -= diff;
	}

	return drm_rect_visible(dst);
}

/*
 * src / dst as a 16.16 factor (src is 16.16, dst whole pixels).  Below
 * 1.0 it rounds down, above 1.0 up: the limit check then sees the most
 * extreme factor any pixel of the plane is scaled by.
 */
static int drm_calc_scale(int src, int dst)
{
	int scale = 0;

	if (WARN_ON(src < 0 || dst < 0))
		return -EINVAL;

	if (dst == 0)
		return 0;

	if (src > (dst << 16))
		return DIV_ROUND_UP(src, dst);
	else
		scale = src / dst;

	return scale;
}

int drm_rect_calc_hscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_hscale, int max_hscale)
{
	int src_w = drm_rect_width(src);
	int dst_w = drm_rect_width(dst);
	int hscale = drm_calc_scale(src_w, dst_w);

	if (hscale < 0 || dst_w == 0)
		return hscale;

	if (hscale < min_hscale || hscale > max_hscale)
		return -ERANGE;

	return hscale;
}

int drm_rect_calc_vscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_vscale, int max_vscale)
{
	int src_h = drm_rect_height(src);
	int dst_h = drm_rect_height(dst);
	int vscale = drm_calc_scale(src_h, dst_h);

	if (vscale < 0 || dst_h == 0)
		return vscale;

	if (vscale < min_vscale || vscale > max_vscale)
		return -ERANGE;

	return vscale;
}

/* The sign and magnitude of v for a "%c%u" pair: the kernel's printf has
 * no '+' flag, so DRM_RECT_FMT cannot be handed to it as it is. */
static char sign_of(int v)
{
	return v < 0 ? '-' : '+';
}

static unsigned int mag_of(int v)
{
	return v < 0 ? 0u - (unsigned int)v : (unsigned int)v;
}

/* Six decimals of a 16.16 fraction: frac / 65536 * 10^6 = frac * 15625 / 1024. */
static unsigned int frac6(int v)
{
	return (((unsigned int)v & 0xffff) * 15625) >> 10;
}

void drm_rect_debug_print(const char *prefix, const struct drm_rect *r,
			  bool fixed_point)
{
	int w = drm_rect_width(r), h = drm_rect_height(r);

	if (fixed_point)
		/* The same text as DRM_RECT_FP_FMT / DRM_RECT_FP_ARG. */
		kprintf("[drm] %s%d.%06ux%d.%06u%c%u.%06u%c%u.%06u\n", prefix,
			w >> 16, frac6(w), h >> 16, frac6(h),
			sign_of(r->x1 >> 16), mag_of(r->x1 >> 16), frac6(r->x1),
			sign_of(r->y1 >> 16), mag_of(r->y1 >> 16), frac6(r->y1));
	else
		/* The same text as DRM_RECT_FMT / DRM_RECT_ARG. */
		kprintf("[drm] %s%dx%d%c%u%c%u\n", prefix, w, h,
			sign_of(r->x1), mag_of(r->x1),
			sign_of(r->y1), mag_of(r->y1));
}

/*
 * width and height are the size of the area in its untransformed
 * orientation (along x and y before the rotation); with the rotation they
 * say where the new origin lies.  Reflection is applied first, then the
 * rotation (counter-clockwise).
 */
void drm_rect_rotate(struct drm_rect *r,
		     int width, int height,
		     unsigned int rotation)
{
	struct drm_rect tmp;

	if (rotation & (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
		tmp = *r;

		if (rotation & DRM_MODE_REFLECT_X) {
			r->x1 = width - tmp.x2;
			r->x2 = width - tmp.x1;
		}

		if (rotation & DRM_MODE_REFLECT_Y) {
			r->y1 = height - tmp.y2;
			r->y2 = height - tmp.y1;
		}
	}

	switch (rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_0:
		break;
	case DRM_MODE_ROTATE_90:
		tmp = *r;
		r->x1 = tmp.y1;
		r->x2 = tmp.y2;
		r->y1 = width - tmp.x2;
		r->y2 = width - tmp.x1;
		break;
	case DRM_MODE_ROTATE_180:
		tmp = *r;
		r->x1 = width - tmp.x2;
		r->x2 = width - tmp.x1;
		r->y1 = height - tmp.y2;
		r->y2 = height - tmp.y1;
		break;
	case DRM_MODE_ROTATE_270:
		tmp = *r;
		r->x1 = height - tmp.y2;
		r->x2 = height - tmp.y1;
		r->y1 = tmp.x1;
		r->y2 = tmp.x2;
		break;
	default:
		break;
	}
}

/*
 * The inverse of drm_rect_rotate() with the same width, height and
 * rotation (the untransformed area's size, never swapped by the caller):
 * rotate(r); rotate_inv(r) gives back r.
 */
void drm_rect_rotate_inv(struct drm_rect *r,
			 int width, int height,
			 unsigned int rotation)
{
	struct drm_rect tmp;

	switch (rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_0:
		break;
	case DRM_MODE_ROTATE_90:
		tmp = *r;
		r->x1 = width - tmp.y2;
		r->x2 = width - tmp.y1;
		r->y1 = tmp.x1;
		r->y2 = tmp.x2;
		break;
	case DRM_MODE_ROTATE_180:
		tmp = *r;
		r->x1 = width - tmp.x2;
		r->x2 = width - tmp.x1;
		r->y1 = height - tmp.y2;
		r->y2 = height - tmp.y1;
		break;
	case DRM_MODE_ROTATE_270:
		tmp = *r;
		r->x1 = tmp.y1;
		r->x2 = tmp.y2;
		r->y1 = height - tmp.x2;
		r->y2 = height - tmp.x1;
		break;
	default:
		break;
	}

	if (rotation & (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
		tmp = *r;

		if (rotation & DRM_MODE_REFLECT_X) {
			r->x1 = width - tmp.x2;
			r->x2 = width - tmp.x1;
		}

		if (rotation & DRM_MODE_REFLECT_Y) {
			r->y1 = height - tmp.y2;
			r->y2 = height - tmp.y1;
		}
	}
}
