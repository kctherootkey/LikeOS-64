// LikeOS -- the primary plane and the cursor of each pipe: binding the
// framebuffers for scanout, programming the planes from their atomic
// states, flips, and the plane part of the atomic check.
//
// Skylake and later program the universal plane registers (PLANE_CTL,
// PLANE_COLOR_CTL from display version 10, and the rest); Broadwell and
// Haswell the older DSPCNTR primary plane.  The cursor is CUR_CTL on all
// of them.  The bits a plane carries for the pipe's colour stages come
// from intel_color_plane_bits(); the underrun interrupt the new
// watermarks re-arm is intel_vblank.c's.
//
// The check clips each plane against the client's mode: what the
// hardware is given is the clipped source and destination (a primary
// plane of Skylake and later may sit anywhere, partly off the pipe;
// Broadwell's must cover it), while the cursor keeps its own unclipped
// position and is only switched off when nothing of it is left on the
// pipe.  Rotation, reflection, plane alpha and the blend mode go into the
// control registers (intel_features.h has the switches); the properties
// themselves are attached in intel_plane_props.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2021 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_plane.h>
#include <kernel/dev/gpu/i915/intel_color.h>
#include <kernel/dev/gpu/i915/intel_vblank.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/i915_ggtt_view.h>
#include <kernel/dev/gpu/i915/skl_universal_plane_regs.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/drm_damage_helper.h>
#include <kernel/dev/gpu/drm_fourcc.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* 90/270-degree scanout reads a column-ordered view of the surface that
 * the GGTT makes (intel_plane_rotated_view_bind(), i915_ggtt_view.c).
 * 0: no such views are made and those rotations are refused at check
 * time even where I915_FEAT_ROTATED_VIEWS allows them -- the user's
 * switch is that one; this one says whether the views exist at all. */
#define INTEL_PLANE_ROTATED_GGTT_VIEWS 1

/* ---- objects in the GGTT ------------------------------------------------- */

int intel_plane_bo_bind(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (!bo)
		return -EINVAL;
	/* the display reads memory, not the processor's cache */
	i915_gem_object_set_display(i915, o);
	if (bo->bound)
		return 0;
	if (i915_lmem_present(i915)) {
		/* The display of a discrete part reads only its own memory:
		 * a system-memory surface is shown through a copy there,
		 * refreshed on every commit (bo->bound stays clear). */
		int s = i915_lmem_scanout_shadow(i915, o, &bo->ggtt);
		if (s <= 0)
			return s;
	}
	int rc = i915_ggtt_bind_scanout(i915, o, &bo->ggtt);
	if (rc)
		return rc;
	bo->bound = 1;
	return 0;
}

/* The GGTT's description of the view the plane code laid out: one
 * colour plane (every format these planes take has one). */
static void rotated_view_to_gtt(const struct intel_rotated_view_info *info,
				struct i915_rotation_info *r)
{
	mm_memset(r, 0, sizeof(*r));
	r->plane[0].offset = info->offset;
	r->plane[0].width = info->width;
	r->plane[0].height = info->height;
	r->plane[0].src_stride = info->src_stride;
	r->plane[0].dst_stride = info->dst_stride;
}

/* Where a rotated view must start: where the plane's surface offsets are
 * rounded to (plane_view_of()) -- 1 MB for a Y-tiled surface before
 * display version 12 -- and never below the 256 KB every scanout
 * binding gets. */
static uint32_t rotated_view_alignment(struct i915_device *i915)
{
	return i915->info->display_ver >= 12 ? 256u * 1024u : 1024u * 1024u;
}

int intel_plane_rotated_view_bind(struct i915_device *i915, struct drm_gem_object *o,
				  const struct intel_rotated_view_info *info, uint32_t *ggtt)
{
	struct i915_rotation_info r;
	uint32_t shown[INTEL_MAX_PIPES];

	*ggtt = 0;
	if (!INTEL_PLANE_ROTATED_GGTT_VIEWS || !I915_FEAT_ROTATED_VIEWS)
		return -EOPNOTSUPP;
	/* A discrete part scans a system-memory surface out of a copy in
	 * its local memory, which has no rotated views: the check refuses
	 * those (fb_rotates_90_270()). */
	if (i915_lmem_present(i915) && !i915_lmem_object(o))
		return -EOPNOTSUPP;
	rotated_view_to_gtt(info, &r);
	/* what the pipes show now stays bound, whichever view it is */
	for (int k = 0; k < INTEL_MAX_PIPES; k++)
		shown[k] = i915->display.pipes[k].surf_ggtt;
	return i915_ggtt_rotated_view_get(i915, o, &r, rotated_view_alignment(i915), shown,
					  INTEL_MAX_PIPES, ggtt);
}

/* Said a few times, early: what a client puts on the screen and what the
 * plane was given for it.  A picture that is structured but wrong is
 * almost always one of these two disagreeing. */
static int g_plane_logged;

/* Broadwell keeps the older plane registers (DSPCNTR and friends) and
 * the older watermarks: one level per pipe, a FIFO fill in 64-byte
 * units that the plane must have buffered to cover the memory latency. */
static int legacy_plane(struct i915_device *i915)
{
	return i915->info->display_ver < 9;
}

int intel_plane_dspcntr(struct i915_device *i915)
{
	return legacy_plane(i915);
}

/* The plane's colour and alpha controls live in PLANE_COLOR_CTL from
 * display version 10 (Gemini Lake); the plane code used to put them
 * there only from version 11, which is what it keeps doing without the
 * plane properties. */
static int has_color_ctl(struct i915_device *i915)
{
	return i915->info->display_ver >= (I915_FEAT_PLANE_PROPS ? 10 : 11);
}

/* ---- what the framebuffer is ---------------------------------------------------- */

static const struct drm_format_info *fb_info(const struct drm_framebuffer *fb)
{
	return fb->format_info ? fb->format_info : __drm_format_info(fb->format);
}

static int fb_has_alpha(const struct drm_framebuffer *fb)
{
	const struct drm_format_info *info = fb_info(fb);
	return info && info->has_alpha;
}

static int fb_is_yuv(const struct drm_framebuffer *fb)
{
	const struct drm_format_info *info = fb_info(fb);
	return info && info->is_yuv;
}

static uint32_t fb_cpp(const struct drm_framebuffer *fb)
{
	const struct drm_format_info *info = fb_info(fb);
	if (info && info->cpp[0])
		return info->cpp[0];
	return fb->format == DRM_FORMAT_RGB565 ? 2 : 4;
}

/* ---- rotation, alpha, colour encoding: the control register bits ---------------- */

unsigned int intel_plane_supported_rotations(struct i915_device *i915, uint32_t type)
{
	unsigned int rot;

	if (!I915_FEAT_PLANE_PROPS)
		return DRM_MODE_ROTATE_0;
	if (type == DRM_PLANE_TYPE_CURSOR)
		return DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180;
	if (type != DRM_PLANE_TYPE_PRIMARY)
		return DRM_MODE_ROTATE_0;
	/* Haswell / Broadwell: DSPCNTR rotates by 180 degrees only */
	if (legacy_plane(i915))
		return DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180;
	if (i915->info->display_ver >= 13)
		rot = DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180;
	else
		rot = DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_180 |
		      DRM_MODE_ROTATE_270;
	if (i915->info->display_ver >= 11)
		rot |= DRM_MODE_REFLECT_X;
	return rot;
}

static uint32_t skl_plane_ctl_rotate(unsigned int rotate)
{
	switch (rotate) {
	case DRM_MODE_ROTATE_0:
		break;
	/*
	 * DRM_MODE_ROTATE_ is counter clockwise to stay compatible with Xrandr
	 * while i915 HW rotation is clockwise, that's why this swapping.
	 */
	case DRM_MODE_ROTATE_90:
		return PLANE_CTL_ROTATE_270;
	case DRM_MODE_ROTATE_180:
		return PLANE_CTL_ROTATE_180;
	case DRM_MODE_ROTATE_270:
		return PLANE_CTL_ROTATE_90;
	default:
		break;
	}
	return 0;
}

static uint32_t icl_plane_ctl_flip(unsigned int reflect)
{
	switch (reflect) {
	case 0:
		break;
	case DRM_MODE_REFLECT_X:
		return PLANE_CTL_FLIP_HORIZONTAL;
	case DRM_MODE_REFLECT_Y:
	default:
		break;
	}
	return 0;
}

/* How the plane blends with what is below it: a format without alpha
 * (or blend mode "None") is opaque; pre-multiplied pixels are taken as
 * they are, coverage ones are multiplied by their alpha first. */
static uint32_t skl_plane_ctl_alpha(const struct drm_plane_state *ps)
{
	if (!fb_has_alpha(ps->fb))
		return PLANE_CTL_ALPHA_DISABLE;
	switch (ps->pixel_blend_mode) {
	case DRM_MODE_BLEND_PIXEL_NONE:
		return PLANE_CTL_ALPHA_DISABLE;
	case DRM_MODE_BLEND_PREMULTI:
		return PLANE_CTL_ALPHA_SW_PREMULTIPLY;
	case DRM_MODE_BLEND_COVERAGE:
		return PLANE_CTL_ALPHA_HW_PREMULTIPLY;
	default:
		return PLANE_CTL_ALPHA_DISABLE;
	}
}

static uint32_t glk_plane_color_ctl_alpha(const struct drm_plane_state *ps)
{
	if (!fb_has_alpha(ps->fb))
		return PLANE_COLOR_ALPHA_DISABLE;
	switch (ps->pixel_blend_mode) {
	case DRM_MODE_BLEND_PIXEL_NONE:
		return PLANE_COLOR_ALPHA_DISABLE;
	case DRM_MODE_BLEND_PREMULTI:
		return PLANE_COLOR_ALPHA_SW_PREMULTIPLY;
	case DRM_MODE_BLEND_COVERAGE:
		return PLANE_COLOR_ALPHA_HW_PREMULTIPLY;
	default:
		return PLANE_COLOR_ALPHA_DISABLE;
	}
}

/* The plane's constant alpha rides in the colour key registers: the top
 * byte of PLANE_KEYMAX, used when PLANE_KEYMSK says so.  The hardware
 * has 8 bits of it. */
static uint32_t skl_plane_keymax(const struct drm_plane_state *ps)
{
	uint8_t alpha = (uint8_t)(ps->alpha >> 8);
	return PLANE_KEYMAX_ALPHA(alpha);
}

static uint32_t skl_plane_keymsk(const struct drm_plane_state *ps)
{
	uint8_t alpha = (uint8_t)(ps->alpha >> 8);
	uint32_t keymsk = 0;
	if (alpha < 0xff)
		keymsk |= PLANE_KEYMSK_ALPHA_ENABLE;
	return keymsk;
}

/* What a plane state adds to the plane registers beyond format, layout
 * and the pipe's colour bits.  All zero: unrotated, opaque, BT.601
 * selectors -- what the planes were always given. */
struct plane_extra {
	uint32_t ctl; /* PLANE_CTL, or DSPCNTR on Broadwell / Haswell */
	uint32_t color_ctl; /* PLANE_COLOR_CTL */
	int keys; /* write PLANE_KEYVAL / KEYMSK / KEYMAX */
	uint32_t keymsk, keymax;
};

static void plane_extra_of(struct i915_device *i915, const struct drm_plane_state *ps,
			   struct plane_extra *x)
{
	unsigned int rotation = ps->rotation ? ps->rotation : DRM_MODE_ROTATE_0;
	int ver = i915->info->display_ver;

	x->ctl = 0;
	x->color_ctl = 0;
	x->keys = 0;
	x->keymsk = 0;
	x->keymax = 0;
	if (legacy_plane(i915)) {
		/* HSW/BDW start a 180-degree scan at the right place by
		 * themselves: the offsets stay those of the top left */
		if (I915_FEAT_PLANE_PROPS && (rotation & DRM_MODE_ROTATE_180))
			x->ctl |= DISP_ROTATE_180;
		return;
	}
	if (I915_FEAT_PLANE_PROPS) {
		x->ctl |= skl_plane_ctl_rotate(rotation & DRM_MODE_ROTATE_MASK);
		if (ver >= 11)
			x->ctl |= icl_plane_ctl_flip(rotation & DRM_MODE_REFLECT_MASK);
		if (ver < 10) {
			if (ps->color_encoding == DRM_COLOR_YCBCR_BT709)
				x->ctl |= PLANE_CTL_YUV_TO_RGB_CSC_FORMAT_BT709;
			if (ps->color_range == DRM_COLOR_YCBCR_FULL_RANGE)
				x->ctl |= PLANE_CTL_YUV_RANGE_CORRECTION_DISABLE;
		} else if (fb_is_yuv(ps->fb) && ver < 11) {
			/* Gemini Lake's planes convert YCbCr themselves; from
			 * version 11 the primary is an HDR plane whose input
			 * CSC would have to be loaded first.  None of the
			 * formats these planes take is YCbCr today. */
			switch (ps->color_encoding) {
			case DRM_COLOR_YCBCR_BT709:
				x->color_ctl |= PLANE_COLOR_CSC_MODE_YUV709_TO_RGB709;
				break;
			case DRM_COLOR_YCBCR_BT2020:
				x->color_ctl |= PLANE_COLOR_CSC_MODE_YUV2020_TO_RGB2020;
				break;
			default:
				x->color_ctl |= PLANE_COLOR_CSC_MODE_YUV601_TO_RGB601;
				break;
			}
			if (ps->color_range == DRM_COLOR_YCBCR_FULL_RANGE)
				x->color_ctl |= PLANE_COLOR_YUV_RANGE_CORRECTION_DISABLE;
		}
	}
	if (I915_FEAT_PLANE_ALPHA) {
		if (has_color_ctl(i915))
			x->color_ctl |= glk_plane_color_ctl_alpha(ps);
		else
			x->ctl |= skl_plane_ctl_alpha(ps);
		x->keys = 1;
		x->keymsk = skl_plane_keymsk(ps);
		x->keymax = skl_plane_keymax(ps);
	}
}

/* ---- where the plane reads from ------------------------------------------------- */

/* The plane's source as the hardware fetches it: the surface address
 * (relative to the object's binding, or to the rotated view), the
 * PLANE_OFFSET pixel offsets, the fetched size and PLANE_STRIDE. */
struct plane_view {
	int rotated; /* 90/270 through a rotated GGTT view */
	uint32_t surf_off; /* bytes from the binding / view start */
	uint32_t x, y; /* PLANE_OFFSET */
	uint32_t w, h; /* PLANE_SIZE + 1 */
	uint32_t stride; /* PLANE_STRIDE (64-byte units or tiles) */
	struct intel_rotated_view_info rot;
};

/* Y tiles are 128 bytes by 32 rows on every part with universal planes. */
#define Y_TILE_BYTES 128u
#define Y_TILE_ROWS 32u
#define TILE_SIZE 4096u

/* Tiles moved out of the aligned base go back into x/y. */
static void adjust_tile_offset(uint32_t *x, uint32_t *y, uint32_t tile_width,
			       uint32_t tile_height, uint32_t pitch_tiles,
			       uint32_t old_offset, uint32_t new_offset)
{
	uint32_t pitch_pixels = pitch_tiles * tile_width;
	uint32_t tiles;

	WARN_ON(old_offset & (TILE_SIZE - 1));
	WARN_ON(new_offset & (TILE_SIZE - 1));
	WARN_ON(new_offset > old_offset);

	tiles = (old_offset - new_offset) / TILE_SIZE;

	*y += tiles / pitch_tiles * tile_height;
	*x += tiles % pitch_tiles * tile_width;

	/* minimize x in case it got needlessly big */
	*y += *x / pitch_pixels * tile_height;
	*x %= pitch_pixels;
}

/* The tile-aligned base of a tiled surface's pixel (x, y), rounded down
 * to `alignment', with x / y made relative to it.  For 90/270 x and y are
 * already rotated to match the rotated view, and `pitch' is that view's
 * row length in bytes of rotated tile height. */
static uint32_t compute_aligned_offset_tiled(uint32_t *x, uint32_t *y, uint32_t cpp,
					     uint32_t pitch, int rotated, uint32_t alignment)
{
	uint32_t tile_width = Y_TILE_BYTES / cpp, tile_height = Y_TILE_ROWS;
	uint32_t pitch_tiles, tile_rows, tiles, offset, offset_aligned;

	if (rotated) {
		uint32_t t = tile_width;
		pitch_tiles = pitch / tile_height;
		tile_width = tile_height;
		tile_height = t;
	} else {
		pitch_tiles = pitch / (tile_width * cpp);
	}

	tile_rows = *y / tile_height;
	*y %= tile_height;

	tiles = *x / tile_width;
	*x %= tile_width;

	offset = (tile_rows * pitch_tiles + tiles) * TILE_SIZE;

	offset_aligned = offset;
	if (alignment)
		offset_aligned = offset_aligned / alignment * alignment;

	adjust_tile_offset(x, y, tile_width, tile_height, pitch_tiles, offset, offset_aligned);
	return offset_aligned;
}

/* The rotated GGTT view of a Y-tiled framebuffer: its tiles, and where
 * the framebuffer's first pixel lands in it (*vx, *vy), and the view's
 * row length (*mapping_stride). */
static void rotated_view_layout(const struct drm_framebuffer *fb,
				struct intel_rotated_view_info *info, uint32_t *vx,
				uint32_t *vy, uint32_t *mapping_stride)
{
	uint32_t cpp = fb_cpp(fb);
	uint32_t tile_width = Y_TILE_BYTES / cpp, tile_height = Y_TILE_ROWS;
	uint32_t pitch_tiles = fb->pitch / Y_TILE_BYTES;
	uint32_t x = 0, y = 0, offset;
	struct drm_rect r;

	/* the framebuffer's offset as x/y in the normal view */
	adjust_tile_offset(&x, &y, tile_width, tile_height, pitch_tiles, fb->offset, 0);
	/* and back to a tile with x/y inside it */
	offset = compute_aligned_offset_tiled(&x, &y, cpp, fb->pitch, 0, TILE_SIZE);

	info->offset = offset / TILE_SIZE;
	info->src_stride = DIV_ROUND_UP(fb->pitch, tile_width * cpp);
	info->width = DIV_ROUND_UP(x + fb->width, tile_width);
	info->height = DIV_ROUND_UP(y + fb->height, tile_height);
	info->dst_stride = info->height;

	/* rotate the x/y offsets to match the GTT view */
	drm_rect_init(&r, (int)x, (int)y, (int)fb->width, (int)fb->height);
	drm_rect_rotate(&r, (int)(info->width * tile_width), (int)(info->height * tile_height),
			DRM_MODE_ROTATE_270);
	*vx = (uint32_t)r.x1;
	*vy = (uint32_t)r.y1;
	*mapping_stride = info->dst_stride * tile_height;
}

/* The source of a checked plane state (visible, src clipped and whole
 * pixels) as the plane fetches it. */
static void plane_view_of(struct i915_device *i915, const struct drm_plane_state *ps,
			  struct plane_view *v)
{
	const struct drm_framebuffer *fb = ps->fb;
	uint32_t tw = intel_fb_tile_width(fb->modifier);

	mm_memset(v, 0, sizeof(*v));
	if (!tw)
		tw = 64;
	if (!I915_FEAT_PLANE_PROPS) {
		/* the plane as asked, which the check kept inside the mode */
		v->surf_off = fb->offset;
		v->x = ps->src_x >> 16;
		v->y = ps->src_y >> 16;
		v->w = ps->crtc_w;
		v->h = ps->crtc_h;
		v->stride = fb->pitch / tw;
		return;
	}
	if (!drm_rotation_90_or_270(ps->rotation)) {
		/* SKL+ and HSW/BDW scan a 180-degree plane from the
		 * offsets of its top left corner by themselves */
		v->surf_off = fb->offset;
		v->x = (uint32_t)(ps->src.x1 >> 16);
		v->y = (uint32_t)(ps->src.y1 >> 16);
		v->w = (uint32_t)(drm_rect_width(&ps->src) >> 16);
		v->h = (uint32_t)(drm_rect_height(&ps->src) >> 16);
		v->stride = fb->pitch / tw;
		if (legacy_plane(i915) && fb->modifier == DRM_FORMAT_MOD_LINEAR &&
		    (v->x || v->y)) {
			/* HSW/BDW no longer read the linear offset register:
			 * a linear surface starts at the page holding its
			 * first pixel and DSPOFFSET's x/y take the rest */
			uint32_t lin = v->y * fb->pitch + v->x * fb_cpp(fb);
			uint32_t aligned = lin & ~(TILE_SIZE - 1);
			v->surf_off += aligned;
			v->y = (lin - aligned) / fb->pitch;
			v->x = (lin - aligned - v->y * fb->pitch) / fb_cpp(fb);
		}
		return;
	}
	/* 90/270: the source rotated into the view, then the view's
	 * tile-aligned base and the offsets within it */
	struct drm_rect src = ps->src;
	uint32_t vx, vy, mapping_stride;
	uint32_t alignment = i915->info->display_ver >= 12 ? 4096u : 1024u * 1024u;

	v->rotated = 1;
	rotated_view_layout(fb, &v->rot, &vx, &vy, &mapping_stride);
	drm_rect_rotate(&src, (int)(fb->width << 16), (int)(fb->height << 16),
			DRM_MODE_ROTATE_270);
	v->x = (uint32_t)(src.x1 >> 16) + vx;
	v->y = (uint32_t)(src.y1 >> 16) + vy;
	v->w = (uint32_t)(drm_rect_width(&src) >> 16);
	v->h = (uint32_t)(drm_rect_height(&src) >> 16);
	v->surf_off = compute_aligned_offset_tiled(&v->x, &v->y, fb_cpp(fb), mapping_stride, 1,
						   alignment);
	/* rotated, the stride counts tile heights */
	v->stride = mapping_stride / Y_TILE_ROWS;
}

/* ---- programming --------------------------------------------------------------- */

static void bdw_plane_program(struct i915_device *i915, struct intel_pipe *p,
			      uint32_t ggtt, uint32_t pitch, uint32_t format,
			      uint64_t modifier, const struct plane_extra *x)
{
	uint32_t ctl = DISP_ENABLE | intel_color_plane_bits(i915, p, INTEL_COLOR_REG_DSPCNTR) |
		       x->ctl;
	int cpp = (format == DRM_FORMAT_RGB565) ? 2 : 4;
	switch (format) {
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		ctl |= DISP_FORMAT_RGBX888;
		break;
	case DRM_FORMAT_XRGB2101010:
		ctl |= DISP_FORMAT_BGRX101010;
		break;
	case DRM_FORMAT_XBGR2101010:
		ctl |= DISP_FORMAT_RGBX101010;
		break;
	case DRM_FORMAT_RGB565:
		ctl |= DISP_FORMAT_BGRX565;
		break;
	default:
		ctl |= DISP_FORMAT_BGRX888;
		break;
	}
	if (modifier == I915_FORMAT_MOD_X_TILED)
		ctl |= DISP_TILED;
	i915_write32(i915, DSPSTRIDE(p->pipe), pitch);
	if (modifier == I915_FORMAT_MOD_X_TILED ||
	    (I915_FEAT_PLANE_PROPS && (p->off_x || p->off_y))) {
		/* (DSPTILEOFF is DSPOFFSET on HSW/BDW: x/y for linear
		 * surfaces too, plane_view_of() made them page-relative) */
		i915_write32(i915, DSPTILEOFF(p->pipe), (p->off_y << 16) | p->off_x);
		i915_write32(i915, DSPLINOFF(p->pipe), 0);
	} else {
		i915_write32(i915, DSPTILEOFF(p->pipe), 0);
		i915_write32(i915, DSPLINOFF(p->pipe), p->off_y * pitch + p->off_x * (uint32_t)cpp);
	}
	i915_write32(i915, DSPCNTR(p->pipe), ctl);
	/* the surface write latches the rest */
	i915_write32(i915, DSPSURF(p->pipe), ggtt);
	(void)i915_read32(i915, DSPSURF(p->pipe));
}

/* Said once, for the first surface a client puts on the screen: what it
 * asked for and what the plane was given.  A picture that is structured
 * but wrong is almost always one of these disagreeing. */

static void plane_program(struct i915_device *i915, struct intel_pipe *p,
			  uint32_t ggtt, uint32_t pitch, uint32_t w, uint32_t h,
			  uint32_t format, uint64_t modifier, uint32_t stride_units,
			  const struct plane_extra *x)
{
	/* The plane's own gamma stays off and the PIPE's table applies: that
	 * table is the identity until a client sets one.  Gen11 moved those
	 * bits (and alpha) to a register of their own (Gemini Lake already
	 * has it: has_color_ctl()). */
	int color_ctl = has_color_ctl(i915);
	uint32_t ctl = PLANE_CTL_ENABLE | x->ctl;
	if (!color_ctl)
		ctl |= intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_CTL) |
		       PLANE_CTL_ALPHA_DISABLE;
	uint32_t tw = intel_fb_tile_width(modifier);
	if (!tw) {
		tw = 64;
		modifier = DRM_FORMAT_MOD_LINEAR;
	}
	switch (modifier) {
	case I915_FORMAT_MOD_X_TILED:
		ctl |= PLANE_CTL_TILED_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
		ctl |= PLANE_CTL_TILED_Y;
		break;
	case I915_FORMAT_MOD_4_TILED:
		ctl |= PLANE_CTL_TILED_4;
		break;
	default:
		ctl |= PLANE_CTL_TILED_LINEAR;
		break;
	}
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888;
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888 | PLANE_CTL_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB2101010:
		ctl |= PLANE_CTL_FORMAT_XRGB_2101010;
		break;
	case DRM_FORMAT_XBGR2101010:
		ctl |= PLANE_CTL_FORMAT_XRGB_2101010 | PLANE_CTL_ORDER_RGBX;
		break;
	case DRM_FORMAT_RGB565:
		ctl |= PLANE_CTL_FORMAT_RGB_565;
		break;
	default:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888;
		break;
	}
	if (!stride_units)
		stride_units = pitch / tw;
	/* The watermarks and buffer share belong to the surface about to be
	 * scanned out: describe it, program them, then the plane (whose
	 * surface write arms both). */
	p->surf_ggtt = ggtt;
	p->stride = pitch;
	p->width = w;
	p->height = h;
	p->format = format;
	p->modifier = modifier;
	intel_wm_update(i915);
	if (legacy_plane(i915)) {
		bdw_plane_program(i915, p, ggtt, pitch, format, modifier, x);
		goto done;
	}
	i915_gt_td_flush(i915);
	i915_write32(i915, PLANE_STRIDE(p->pipe, 0), stride_units);
	i915_write32(i915, PLANE_POS(p->pipe, 0), (p->pos_y << 16) | p->pos_x);
	i915_write32(i915, PLANE_OFFSET(p->pipe, 0), (p->off_y << 16) | p->off_x);
	i915_write32(i915, PLANE_SIZE(p->pipe, 0), ((h - 1) << 16) | (w - 1));
	if (x->keys) {
		/* the plane's constant alpha (no colour keying) */
		i915_write32(i915, PLANE_KEYVAL(p->pipe, 0), 0);
		i915_write32(i915, PLANE_KEYMSK(p->pipe, 0), x->keymsk);
		i915_write32(i915, PLANE_KEYMAX(p->pipe, 0), x->keymax);
	}
	if (color_ctl)
		i915_write32(i915, PLANE_COLOR_CTL(p->pipe, 0),
			     intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_COLOR_CTL) |
				     PLANE_COLOR_ALPHA_DISABLE | x->color_ctl);
	/* The control register self-arms if the plane was off: written just
	 * before the surface register, so the enable lands with the rest. */
	i915_write32(i915, PLANE_CTL(p->pipe, 0), ctl);
	i915_write32(i915, PLANE_SURF(p->pipe, 0), ggtt);
	(void)i915_read32(i915, PLANE_SURF(p->pipe, 0));
done:
	p->surf_ggtt = ggtt;
	p->stride = pitch;
	p->width = w;
	p->height = h;
	p->format = format;
	p->modifier = modifier;
	/* new watermarks: let the engine tell us again whether they hold */
	intel_vblank_underrun_rearm(i915, p);
	if (g_plane_logged < 8) {
		g_plane_logged++;
		i915_dbg("[drm] i915: plane %c: %ux%u %c%c%c%c, %s, pitch %u (%u units), at %08x\n",
			'A' + p->pipe, w, h, (char)(format & 0xff),
			(char)((format >> 8) & 0xff), (char)((format >> 16) & 0xff),
			(char)((format >> 24) & 0xff),
			modifier == DRM_FORMAT_MOD_LINEAR	 ? "linear" :
			modifier == I915_FORMAT_MOD_X_TILED ? "X tiled" :
			modifier == I915_FORMAT_MOD_Y_TILED ? "Y tiled" :
			modifier == I915_FORMAT_MOD_4_TILED ? "Tile4" :
								    "an unknown layout",
			pitch, stride_units, ggtt);
		if (legacy_plane(i915))
			i915_dbg("[drm] i915:   DSPCNTR %08x, DSPSTRIDE %08x, DSPSURF %08x\n",
				 i915_read32(i915, DSPCNTR(p->pipe)),
				 i915_read32(i915, DSPSTRIDE(p->pipe)),
				 i915_read32(i915, DSPSURF(p->pipe)));
		else
			i915_dbg("[drm] i915:   PLANE_CTL %08x, STRIDE %08x, SIZE %08x, SURF %08x\n",
				 i915_read32(i915, PLANE_CTL(p->pipe, 0)),
				 i915_read32(i915, PLANE_STRIDE(p->pipe, 0)),
				 i915_read32(i915, PLANE_SIZE(p->pipe, 0)),
				 i915_read32(i915, PLANE_SURF(p->pipe, 0)));
	}
}

void intel_plane_disable(struct i915_device *i915, struct intel_pipe *p)
{
	if (legacy_plane(i915)) {
		i915_write32(i915, DSPCNTR(p->pipe), 0);
		i915_write32(i915, DSPSURF(p->pipe), 0);
		(void)i915_read32(i915, DSPSURF(p->pipe));
		return;
	}
	i915_write32(i915, PLANE_CTL(p->pipe, 0), 0);
	i915_write32(i915, PLANE_SURF(p->pipe, 0), 0);
	(void)i915_read32(i915, PLANE_SURF(p->pipe, 0));
}

/* The surface register of the primary plane, for a flip. */
static void plane_flip(struct i915_device *i915, struct intel_pipe *p, uint32_t surf)
{
	/* a discrete Xe2/Xe3 card: what the engines wrote, out of their
	 * caches before the display reads it (nothing elsewhere) */
	i915_gt_td_flush(i915);
	uint32_t reg = legacy_plane(i915) ? DSPSURF(p->pipe) : PLANE_SURF(p->pipe, 0);
	i915_write32(i915, reg, surf);
	(void)i915_read32(i915, reg);
}

int intel_plane_enabled(struct i915_device *i915, int pipe)
{
	if (legacy_plane(i915))
		return !!(i915_read32(i915, DSPCNTR(pipe)) & DISP_ENABLE);
	return !!(i915_read32(i915, PLANE_CTL(pipe, 0)) & PLANE_CTL_ENABLE);
}

/* ---- the check ------------------------------------------------------------------ */

/* A refusal with its reason in the log, and the errno a client sees. */
static int plane_reject(struct i915_device *i915, const char *why, int rc)
{
	(void)intel_display_check_reject(i915, why);
	return rc;
}

/* 90/270 reads the surface through a rotated GGTT view: Y (or Yf) tiled
 * surfaces only, display version 12 and below. */
static int fb_rotates_90_270(struct i915_device *i915, const struct drm_framebuffer *fb)
{
	if (!I915_FEAT_ROTATED_VIEWS || !INTEL_PLANE_ROTATED_GGTT_VIEWS)
		return 0;
	if (i915->info->display_ver >= 13)
		return 0;
	/* a discrete part shows a system-memory surface through a copy in
	 * local memory, of which no view is made */
	if (i915_lmem_present(i915) && !i915_lmem_object(fb->obj))
		return 0;
	return fb->modifier == I915_FORMAT_MOD_Y_TILED;
}

/* The rotation and layout combinations the universal plane cannot scan
 * out. */
static int skl_plane_check_fb(struct i915_device *i915, const struct drm_crtc_state *cs,
			      const struct drm_plane_state *ps)
{
	const struct drm_framebuffer *fb = ps->fb;
	unsigned int rotation = ps->rotation;
	int ver = i915->info->display_ver;

	if ((rotation & DRM_MODE_REFLECT_X) && fb->modifier == DRM_FORMAT_MOD_LINEAR &&
	    ver < 35)
		return plane_reject(i915, "horizontal flip is not supported with linear surfaces",
				    -EINVAL);
	/* Display20 onward tile4 hflip is not supported */
	if ((rotation & DRM_MODE_REFLECT_X) && fb->modifier == I915_FORMAT_MOD_4_TILED &&
	    ver >= 20)
		return plane_reject(i915, "horizontal flip is not supported with Tile4 surfaces",
				    -EINVAL);
	if (drm_rotation_90_or_270(rotation)) {
		if (!fb_rotates_90_270(i915, fb))
			return plane_reject(i915, "90/270-degree rotation is not available for this surface",
					    -EINVAL);
		/* 90/270 is not allowed with RGB 16-bit 5:6:5 before gen11 */
		if (fb->format == DRM_FORMAT_RGB565 && ver < 11)
			return plane_reject(i915, "pixel format not supported for 90/270-degree rotation",
					    -EINVAL);
	}
	/* Y-tiling is not supported in IF-ID Interlace mode */
	if ((cs->adjusted_mode.flags & DRM_MODE_FLAG_INTERLACE) &&
	    fb->modifier != DRM_FORMAT_MOD_LINEAR && fb->modifier != I915_FORMAT_MOD_X_TILED)
		return plane_reject(i915, "Y/Tile4 tiling not supported in interlaced modes", -EINVAL);
	return 0;
}

/* The primary plane clipped against the mode: can_position on the
 * universal planes, the whole pipe on Broadwell's.  The source comes out
 * in whole pixels, as the hardware has no sub-pixel offsets. */
static int primary_check_clipped(struct i915_device *i915, struct drm_crtc_state *cs,
				 struct drm_plane_state *ps)
{
	const struct drm_framebuffer *fb = ps->fb;
	int legacy = legacy_plane(i915);
	int rc;

	if (ps->rotation & ~intel_plane_supported_rotations(i915, DRM_PLANE_TYPE_PRIMARY))
		return plane_reject(i915, "rotation not supported by the plane", -EINVAL);
	if (legacy) {
		/* Broadwell's primary plane is X tiled or linear. */
		if (fb->modifier == I915_FORMAT_MOD_Y_TILED)
			return plane_reject(i915, "plane position/size/layout not possible on this generation",
					    -EINVAL);
	} else {
		rc = skl_plane_check_fb(i915, cs, ps);
		if (rc)
			return rc;
	}
	rc = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						 DRM_PLANE_NO_SCALING, !legacy, true);
	if (rc == -ERANGE)
		return plane_reject(i915, "the plane would have to scale", rc);
	if (rc)
		/* Broadwell's primary plane has no position or size of
		 * its own: it is the pipe. */
		return plane_reject(i915, "plane position/size/layout not possible on this generation",
				    rc);
	if (!ps->visible)
		return 0;
	/* Hardware doesn't handle subpixel coordinates. */
	{
		int sx = ps->src.x1 >> 16, sy = ps->src.y1 >> 16;
		int sw = drm_rect_width(&ps->src) >> 16, sh = drm_rect_height(&ps->src) >> 16;
		drm_rect_init(&ps->src, sx << 16, sy << 16, sw << 16, sh << 16);
	}
	if (legacy)
		return 0;
	/*
	 * Display WA #1175: glk
	 * Planes other than the cursor may cause FIFO underflow and display
	 * corruption if starting less than 4 pixels from the right edge of
	 * the screen.
	 * Besides the above WA fix the similar problem, where planes other
	 * than the cursor ending less than 4 pixels from the left edge of the
	 * screen may cause FIFO underflow and display corruption.
	 */
	if (i915->info->display_ver == 10) {
		int crtc_x = ps->dst.x1, crtc_w = drm_rect_width(&ps->dst);
		if (crtc_x + crtc_w < 4 || crtc_x > (int)cs->mode.hdisplay - 4)
			return plane_reject(i915, "the plane starts or ends within 4 pixels of the pipe's edge",
					    -ERANGE);
	}
	if (drm_rotation_90_or_270(ps->rotation)) {
		/* the rotated view must fit the plane's stride and offsets */
		struct plane_view v;
		uint32_t max_px = i915->info->display_ver >= 13 ? 65536u : 8192u;
		uint32_t max_bytes = i915->info->display_ver >= 13 ? 128u * 1024u : 32u * 1024u;
		struct i915_rotation_info r;
		plane_view_of(i915, ps, &v);
		if (v.stride * Y_TILE_ROWS > min(max_px, max_bytes / fb_cpp(fb)) ||
		    v.x >= max_px || v.y >= max_px)
			return plane_reject(i915, "the rotated surface exceeds the plane's stride", -EINVAL);
		/* every tile the view maps must be the object's */
		rotated_view_to_gtt(&v.rot, &r);
		if (!fb->obj || !i915_rotation_info_valid(&r, fb->obj->npages))
			return plane_reject(i915, "the rotated view does not fit the framebuffer's object",
					    -EINVAL);
	}
	/* HW only has 8 bits pixel precision, disable plane if invisible */
	if (I915_FEAT_PLANE_ALPHA && !(ps->alpha >> 8))
		ps->visible = false;
	return 0;
}

/* The plane part of a crtc's check: the primary plane's framebuffer and
 * position against the client's mode and this generation's planes, and
 * the cursor's size and layout. */
int intel_plane_atomic_check(struct i915_device *i915, struct drm_atomic_state *st,
			     int crtc, struct drm_crtc_state *cs)
{
	struct drm_device *dev = &i915->drm;
	struct drm_plane *prim = drm_crtc_primary(dev, crtc);
	struct drm_plane *cur = drm_crtc_cursor(dev, crtc);
	const struct drm_mode_modeinfo *m = &cs->mode;

	if (!prim || !cur)
		return -ENODEV;
	/* the planes of this crtc */
	struct drm_plane_state *ps = drm_atomic_plane_state(st, prim);
	ps->src = drm_plane_state_src(ps);
	ps->dst = drm_plane_state_dest(ps);
	ps->visible = ps->fb != NULL;
	if (ps->fb) {
		if (!intel_fb_format_supported(ps->fb->format))
			return intel_display_check_reject(i915, "pixel format not supported");
		uint32_t tw = intel_fb_tile_width(ps->fb->modifier);
		if (!tw || ps->fb->pitch % tw)
			return intel_display_check_reject(i915, "pitch is not whole tiles of the layout");
		if (I915_FEAT_PLANE_PROPS) {
			int rc = primary_check_clipped(i915, cs, ps);
			if (rc)
				return rc;
		} else {
			if (ps->crtc_x < 0 || ps->crtc_y < 0 ||
			    (uint32_t)ps->crtc_x + ps->crtc_w > m->hdisplay ||
			    (uint32_t)ps->crtc_y + ps->crtc_h > m->vdisplay)
				return intel_display_check_reject(i915, "plane outside the mode");
			/* Broadwell's primary plane has no position or size of
			 * its own: it is the pipe, X tiled or linear. */
			if (legacy_plane(i915) &&
			    (ps->crtc_x || ps->crtc_y || ps->crtc_w != m->hdisplay ||
			     ps->crtc_h != m->vdisplay ||
			     ps->fb->modifier == I915_FORMAT_MOD_Y_TILED))
				return intel_display_check_reject(i915, "plane position/size/layout not possible on this generation");
		}
	}
	struct drm_plane_state *cps = drm_atomic_plane_state(st, cur);
	cps->src = drm_plane_state_src(cps);
	cps->dst = drm_plane_state_dest(cps);
	cps->visible = cps->fb != NULL;
	if (cps->fb) {
		if (cps->crtc_w != cps->crtc_h ||
		    (cps->crtc_w != 64 && cps->crtc_w != 128 && cps->crtc_w != 256))
			return intel_display_check_reject(i915, "cursor size not supported");
		if (cps->fb->format != DRM_FORMAT_ARGB8888 ||
		    cps->fb->modifier != DRM_FORMAT_MOD_LINEAR ||
		    cps->fb->pitch != cps->crtc_w * 4 || cps->src_x || cps->src_y)
			return intel_display_check_reject(i915, "cursor format/layout not supported");
		if (I915_FEAT_PLANE_PROPS) {
			const struct drm_rect src = cps->src, dst = cps->dst;
			if (cps->rotation &
			    ~intel_plane_supported_rotations(i915, DRM_PLANE_TYPE_CURSOR))
				return intel_display_check_reject(i915, "cursor rotation not supported");
			int rc = drm_atomic_helper_check_plane_state(cps, cs, DRM_PLANE_NO_SCALING,
								     DRM_PLANE_NO_SCALING, true, true);
			if (rc)
				return plane_reject(i915, "the cursor would have to scale", rc);
			/* Use the unclipped src/dst rectangles, which we
			 * program to hw; the clipping only says whether
			 * anything of the cursor is on the pipe. */
			cps->src = src;
			cps->dst = dst;
		}
	}
	return 0;
}

void intel_plane_wm_geometry(const struct drm_plane_state *ps, int *on,
			     uint32_t *w, uint32_t *h, uint32_t *src_x)
{
	if (I915_FEAT_PLANE_PROPS) {
		*on = ps->fb && ps->visible;
		*w = *on ? (uint32_t)(drm_rect_width(&ps->src) >> 16) : ps->crtc_w;
		*h = *on ? (uint32_t)(drm_rect_height(&ps->src) >> 16) : ps->crtc_h;
		*src_x = *on ? (uint32_t)(ps->src.x1 >> 16) : (ps->src_x >> 16);
		if (*on && drm_rotation_90_or_270(ps->rotation)) {
			uint32_t t = *w;
			*w = *h;
			*h = t;
		}
		return;
	}
	*on = ps->fb != NULL;
	*w = ps->crtc_w;
	*h = ps->crtc_h;
	*src_x = ps->src_x >> 16;
}

/* ---- binding with damage ---------------------------------------------------------- */

/* A discrete card's local copy of a system-memory surface brought up to
 * date over the damage of the update only.  Only for the framebuffer the
 * plane already shows (the copy of any other one is older than the frame
 * the damage refers to), once the copy exists (bo->ggtt holds its
 * address, bo->bound stays clear for such an object).  1 when the copy is
 * current and bo->ggtt is the address to scan out; 0: bind the usual way
 * (the whole surface copied). */
static int lmem_damage_refresh(struct i915_device *i915, const struct drm_plane_state *ps)
{
	struct drm_framebuffer *fb = ps->fb;
	struct drm_gem_object *o = fb->obj;
	struct i915_bo *bo = o ? o->priv : NULL;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_plane_state old;
	struct drm_rect clip;
	int copied = 0;

	if (!I915_FEAT_LMEM_DAMAGE || !bo || !i915_lmem_present(i915) || bo->bound ||
	    !bo->ggtt || i915_lmem_object(o))
		return 0;
	if (!ps->damage_clips || ps->ignore_damage_clips || ps->fb_changed || !ps->visible ||
	    !ps->plane || ps->plane->fb_id != ps->fb_id)
		return 0;
	drm_atomic_helper_old_plane_state(&i915->drm, ps->plane, &old);
	drm_atomic_helper_damage_iter_init(&iter, &old, ps);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		struct drm_mode_rect_k r = { clip.x1, clip.y1, clip.x2, clip.y2 };
		if (!i915_lmem_scanout_dirty(i915, fb, &r, 1))
			return copied; /* no copy after all: the full path makes it */
		copied = 1;
		/* a tiled surface interleaves any rectangle across all of
		 * it: the first one already copied the whole framebuffer */
		if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
			break;
	}
	/* An update that damaged nothing keeps the copy as it is.  Make
	 * sure it exists: an empty rectangle copies nothing. */
	if (!copied) {
		struct drm_mode_rect_k none = { 0, 0, 0, 0 };
		return i915_lmem_scanout_dirty(i915, fb, &none, 1);
	}
	i915_gem_object_set_display(i915, o);
	return 1;
}

static int plane_fb_bind(struct i915_device *i915, const struct drm_plane_state *ps)
{
	if (lmem_damage_refresh(i915, ps))
		return 0;
	return intel_plane_bo_bind(i915, ps->fb->obj);
}

int intel_plane_fb_prepare(struct i915_device *i915, const struct drm_plane_state *ps)
{
	int rc;

	if (!ps->fb || !ps->fb->obj)
		return 0;
	rc = plane_fb_bind(i915, ps);
	if (rc)
		return rc;
	/* a 90/270-degree plane reads its rotated view: made here, so that
	 * a view that cannot be made refuses the commit before anything
	 * changed (the programming finds it again) */
	if (I915_FEAT_PLANE_PROPS && ps->visible && drm_rotation_90_or_270(ps->rotation)) {
		struct plane_view v;
		uint32_t base;
		plane_view_of(i915, ps, &v);
		if (v.rotated)
			return intel_plane_rotated_view_bind(i915, ps->fb->obj, &v.rot, &base);
	}
	return 0;
}

/* ---- the cursor and the primary plane from their states ---------------------------- */

void intel_plane_cursor_program(struct i915_device *i915, struct intel_pipe *cp,
				const struct drm_plane_state *ps)
{
	int pipe = cp->pipe;
	if (!ps->fb) {
		if (cp->cursor_w) { /* its watermarks go first */
			cp->cursor_w = 0;
			if (cp->active)
				intel_wm_update(i915);
		}
		cp->planes.cursor_hidden = 0;
		i915_write32(i915, CUR_CTL(pipe), 0);
		i915_write32(i915, CUR_BASE(pipe), 0);
		(void)i915_read32(i915, CUR_BASE(pipe));
		return;
	}
	if (intel_plane_bo_bind(i915, ps->fb->obj))
		return;
	struct i915_bo *bo = ps->fb->obj->priv;
	uint32_t w = ps->crtc_w;
	uint32_t mode = w == 64 ? CUR_MODE_64_ARGB_AX :
			w == 128 ? CUR_MODE_128_ARGB_AX : CUR_MODE_256_ARGB_AX;
	if (cp->cursor_w != w) {
		/* the cursor's share of the buffer depends on its size */
		cp->cursor_w = w;
		if (cp->active)
			intel_wm_update(i915);
	}
	if (I915_FEAT_PLANE_PROPS && !ps->visible) {
		/* Nothing of it on the pipe: off, its watermarks kept for
		 * when it comes back. */
		cp->planes.cursor_hidden = 1;
		i915_write32(i915, CUR_CTL(pipe), 0);
		i915_write32(i915, CUR_POS(pipe), 0);
		i915_write32(i915, CUR_BASE(pipe), 0);
		(void)i915_read32(i915, CUR_BASE(pipe));
		return;
	}
	cp->planes.cursor_hidden = 0;
	uint32_t pos = 0;
	int x = ps->crtc_x, y = ps->crtc_y;
	if (x < 0) {
		pos |= CUR_POS_SIGN;
		x = -x;
	}
	if (y < 0) {
		pos |= CUR_POS_SIGN << 16;
		y = -y;
	}
	pos |= (uint32_t)x | ((uint32_t)y << 16);
	/* ILK+ rotate the cursor around its base by themselves */
	uint32_t rot = 0;
	if (I915_FEAT_PLANE_PROPS && (ps->rotation & DRM_MODE_ROTATE_180))
		rot = MCURSOR_ROTATE_180;
	cp->planes.cursor_rotation = I915_FEAT_PLANE_PROPS ? ps->rotation : 0;
	i915_write32(i915, CUR_CTL(pipe),
		     mode | CUR_PIPE_SELECT((uint32_t)pipe) | rot |
			     intel_color_plane_bits(i915, cp, INTEL_COLOR_REG_CUR_CTL));
	i915_write32(i915, CUR_POS(pipe), pos);
	/* the position and the mode take effect on the base write */
	i915_write32(i915, CUR_BASE(pipe), bo->ggtt + ps->fb->offset);
	(void)i915_read32(i915, CUR_BASE(pipe));
}

/* Do the plane properties of `ps' match what the primary plane was last
 * programmed with?  Only the ones that exist under the switches. */
static int primary_props_same(const struct intel_pipe *p, const struct drm_plane_state *ps)
{
	if (I915_FEAT_PLANE_PROPS &&
	    (p->planes.primary_rotation != ps->rotation ||
	     p->planes.primary_color_encoding != ps->color_encoding ||
	     p->planes.primary_color_range != ps->color_range))
		return 0;
	if (I915_FEAT_PLANE_ALPHA &&
	    (p->planes.primary_alpha != ps->alpha ||
	     p->planes.primary_blend != ps->pixel_blend_mode))
		return 0;
	return 1;
}

static void primary_props_store(struct intel_pipe *p, const struct drm_plane_state *ps)
{
	if (I915_FEAT_PLANE_PROPS) {
		p->planes.primary_rotation = ps->rotation;
		p->planes.primary_color_encoding = (uint16_t)ps->color_encoding;
		p->planes.primary_color_range = (uint16_t)ps->color_range;
	}
	if (I915_FEAT_PLANE_ALPHA) {
		p->planes.primary_alpha = ps->alpha;
		p->planes.primary_blend = ps->pixel_blend_mode;
	}
}

/* The primary plane from its state: the whole thing when the layout
 * changed, just the surface address for a flip. */
int intel_plane_primary_program(struct i915_device *i915, struct intel_pipe *p,
				const struct drm_plane_state *ps, int force)
{
	if (!ps->fb || (I915_FEAT_PLANE_PROPS && !ps->visible)) {
		intel_plane_disable(i915, p);
		p->surf_ggtt = 0;
		return 0;
	}
	int rc = plane_fb_bind(i915, ps);
	if (rc)
		return rc;
	struct i915_bo *bo = ps->fb->obj->priv;
	struct plane_view v;
	struct plane_extra x;
	plane_view_of(i915, ps, &v);
	plane_extra_of(i915, ps, &x);
	uint32_t w = v.w, h = v.h;
	uint32_t base = bo->ggtt;
	if (v.rotated) {
		/* the plane reads the rotated view, not the binding */
		rc = intel_plane_rotated_view_bind(i915, ps->fb->obj, &v.rot, &base);
		if (rc)
			return rc;
	}
	uint32_t surf = base + v.surf_off;
	uint32_t pos_x = I915_FEAT_PLANE_PROPS ? (uint32_t)ps->dst.x1 : (uint32_t)ps->crtc_x;
	uint32_t pos_y = I915_FEAT_PLANE_PROPS ? (uint32_t)ps->dst.y1 : (uint32_t)ps->crtc_y;
	int same = !force && p->surf_ggtt && ps->fb->pitch == p->stride &&
		   ps->fb->format == p->format && ps->fb->modifier == p->modifier &&
		   w == p->width && h == p->height &&
		   pos_x == p->pos_x && pos_y == p->pos_y &&
		   v.x == p->off_x && v.y == p->off_y && primary_props_same(p, ps);
	if (same) {
		plane_flip(i915, p, surf);
		p->surf_ggtt = surf;
		return 0;
	}
	p->pos_x = pos_x;
	p->pos_y = pos_y;
	p->off_x = v.x;
	p->off_y = v.y;
	primary_props_store(p, ps);
	plane_program(i915, p, surf, ps->fb->pitch, w, h, ps->fb->format,
		      ps->fb->modifier, v.rotated ? v.stride : 0, &x);
	return 0;
}
