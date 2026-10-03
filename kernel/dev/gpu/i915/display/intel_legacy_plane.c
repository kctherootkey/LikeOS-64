// LikeOS -- the primary plane, cursor and palette of the Intel parts
// before DDI.
//
// Each pipe is fed by one primary plane (DSPCNTR and friends).  Up to
// gen3 the plane names its pipe in its control register and is addressed
// by one linear address (DSPADDR); the mobile gen2/3 parts with a
// framebuffer compressor cross them over, plane A feeding pipe B where the
// LVDS and the panel fitter are.  From gen4 the plane takes a surface base
// (DSPSURF) and an offset into it -- linear (DSPLINOFF) or in tiles
// (DSPTILEOFF) -- and detiles X-tiled surfaces itself.  The control
// register arms on its own when the plane is enabled; otherwise the
// surface write latches everything.  On i830/i845 every register latches
// as written.
//
// Gen2/3 planes do not detile: an X-tiled surface is bound fence-sized
// and fence-aligned behind the aperture and read through a fence
// register, pinned while the plane scans it out.
//
// The cursor is a small ARGB plane: the 845G/865G kind has a free width
// in multiples of 64 and a stride field, the later kind fixed squares of
// 64, 128 or 256.  Several gen2/3 parts fetch the cursor by physical
// address, so its image is copied into a contiguous buffer of our own.
//
// From gen4 the primary plane and the cursor scan out rotated by 180
// degrees (the plane then starts at the last pixel of its window: the
// offsets point there, and a GMCH cursor's base does too), and the primary
// plane of Cherryview's pipe B also reflects along x.  The primary planes
// sit at the immutable zpos 0, the cursors above them; the cursors, and
// Valleyview's and Cherryview's primary planes, blend pre-multiplied.  The
// pipe gamma and CSC enables each control register carries are the
// pipe's colour state's (intel_legacy_color.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020-2025 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2024 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/dev/gpu/i915/intel_legacy_color.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/ke/interrupt.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- formats and layouts ------------------------------------------------------------ */

const uint32_t intel_legacy_fb_formats[] = {
	DRM_FORMAT_XRGB8888,	DRM_FORMAT_ARGB8888,	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB1555,	DRM_FORMAT_XBGR8888,	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010,
};
const uint32_t intel_legacy_nfb_formats =
	sizeof(intel_legacy_fb_formats) / sizeof(intel_legacy_fb_formats[0]);
const uint64_t intel_legacy_fb_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	I915_FORMAT_MOD_X_TILED,
};
const uint32_t intel_legacy_nfb_modifiers =
	sizeof(intel_legacy_fb_modifiers) / sizeof(intel_legacy_fb_modifiers[0]);

static int format_cpp(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB1555:
		return 2;
	default:
		return 4;
	}
}

/* What each generation's primary plane takes.  The alpha formats are
 * shown as their opaque twins: a primary plane has nothing beneath it. */
static int format_ok(struct lg_display *d, uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_RGB565:
		return 1;
	case DRM_FORMAT_XRGB1555:
		return d->ver < 4;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XBGR2101010:
		return d->ver >= 4;
	default:
		return 0;
	}
}

static uint32_t format_bits(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB1555:
		return DISP_FORMAT_BGRX555;
	case DRM_FORMAT_RGB565:
		return DISP_FORMAT_BGRX565;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return DISP_FORMAT_RGBX888;
	case DRM_FORMAT_XRGB2101010:
		return DISP_FORMAT_BGRX101010;
	case DRM_FORMAT_XBGR2101010:
		return DISP_FORMAT_RGBX101010;
	default:
		return DISP_FORMAT_BGRX888;
	}
}

/* The largest stride the plane fetches. */
static uint32_t max_stride(struct lg_display *d, uint64_t modifier, int cpp)
{
	int tiled = modifier == I915_FORMAT_MOD_X_TILED;

	if (!d->gmch) {
		/* Ironlake to Ivy Bridge: TILEOFF.x limits a tiled one */
		if (tiled)
			return (uint32_t)(4096 * cpp) < 32768u ? (uint32_t)(4096 * cpp) : 32768u;
		return 32768;
	}
	if (d->ver >= 4) {
		if (tiled)
			return (uint32_t)(4096 * cpp) < 16384u ? (uint32_t)(4096 * cpp) : 16384u;
		return 32768;
	}
	if (d->ver == 3)
		return tiled ? 8192 : 16384;
	return 8192;
}

static struct lg_display *dev_lg(void)
{
	return lg_get();
}

int intel_legacy_fb_check(struct drm_device *dev, struct drm_gem_object *o,
			  const struct drm_mode_fb_cmd2 *r, uint64_t *modifier)
{
	struct lg_display *d = dev_lg();
	struct i915_bo *bo = o->priv;
	uint64_t mod = *modifier;
	uint64_t obj_mod;

	(void)dev;
	if (!d || !bo)
		return -EINVAL;
	if (!format_ok(d, r->pixel_format))
		return -EINVAL;
	obj_mod = bo->tiling == I915_TILING_X ? I915_FORMAT_MOD_X_TILED : DRM_FORMAT_MOD_LINEAR;
	/* the display never fetched Y tiles before gen9 */
	if (bo->tiling != I915_TILING_NONE && bo->tiling != I915_TILING_X)
		return -EINVAL;
	if (mod == DRM_FORMAT_MOD_INVALID)
		mod = obj_mod;
	else if (mod != obj_mod && bo->tiling != I915_TILING_NONE)
		return -EINVAL;
	if (mod != DRM_FORMAT_MOD_LINEAR && mod != I915_FORMAT_MOD_X_TILED)
		return -EINVAL;
	/* Gen2/3 planes see an X-tiled surface only through a fence
	 * register, which takes its layout from the object: the object
	 * must be X tiled itself */
	if (mod == I915_FORMAT_MOD_X_TILED && d->ver < 4 && bo->tiling != I915_TILING_X)
		return -EINVAL;
	int cpp = format_cpp(r->pixel_format);
	uint32_t pitch = r->pitches[0];
	if (mod == I915_FORMAT_MOD_X_TILED) {
		if (pitch % 512)
			return -EINVAL;
		if (bo->tiling != I915_TILING_NONE && bo->stride != pitch)
			return -EINVAL;
		/* the surface starts at a tile row */
		if (r->offsets[0])
			return -EINVAL;
	} else if (pitch % 64) {
		return -EINVAL;
	}
	if (pitch > max_stride(d, mod, cpp) || pitch < r->width * (uint32_t)cpp)
		return -EINVAL;
	if (r->offsets[0] & 63)
		return -EINVAL;
	uint32_t th = mod == I915_FORMAT_MOD_X_TILED ? 8 : 1;
	uint64_t rows = ((uint64_t)r->height + th - 1) / th * th;
	if ((uint64_t)r->offsets[0] + (uint64_t)pitch * rows > o->size)
		return -EINVAL;
	*modifier = mod;
	return 0;
}

/* ---- binding ------------------------------------------------------------------- */

/* Gen2/3: a tiled surface is scanned out through a fence. */
static int needs_fence(struct lg_display *d, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;

	return d->ver < 4 && bo && bo->tiling != I915_TILING_NONE;
}

/* A fence covers a power-of-two region at its own alignment, behind the
 * aperture: whether the object's global binding is such a region. */
static int fenced_binding_ok(struct lg_display *d, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	uint32_t fs = i915_legacy_fence_size(d->i915, o);
	uint64_t reserved = bo->ggtt_size ? bo->ggtt_size : (uint64_t)o->npages * 4096;

	return !(bo->ggtt & (fs - 1)) && reserved >= fs &&
	       (uint64_t)bo->ggtt + fs <= d->i915->bar_aperture.size;
}

/* Whether a plane scans the object's binding out right now. */
static int on_screen(struct lg_display *d, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;

	for (int p = 0; p < d->num_pipes; p++) {
		struct lg_pipe *lp = &d->pipes[p];
		if (lp->plane_enabled && lp->surf >= bo->ggtt &&
		    (uint64_t)lp->surf < (uint64_t)bo->ggtt + (uint64_t)o->npages * 4096)
			return 1;
	}
	return 0;
}

static int bo_bind(struct lg_display *d, struct drm_gem_object *o, uint32_t *ggtt)
{
	struct i915_bo *bo = o ? o->priv : NULL;

	if (!bo)
		return -EINVAL;
	/* the display reads memory, not the processor's cache */
	i915_gem_object_set_display(d->i915, o);
	if (needs_fence(d, o) && bo->bound && !fenced_binding_ok(d, o)) {
		/* bound before it was tiled: bound again where a fence
		 * can cover it, unless something reads the old binding */
		if (on_screen(d, o) || (bo->gtt_map_bound && !bo->gtt_map_own))
			return -EBUSY;
		i915_ggtt_unbind(d->i915, bo->ggtt,
				 bo->ggtt_size ? bo->ggtt_size : (uint32_t)(o->npages * 4096));
		bo->bound = 0;
		bo->ggtt_size = 0;
	}
	if (!bo->bound && needs_fence(d, o)) {
		int rc = i915_legacy_ggtt_bind_fenced(d->i915, o, &bo->ggtt);
		if (rc)
			return rc;
		bo->ggtt_size = i915_legacy_fence_size(d->i915, o);
		bo->ggtt_uncached = 1;
		bo->bound = 1;
	} else if (!bo->bound) {
		int rc = i915_ggtt_bind_scanout(d->i915, o, &bo->ggtt);
		if (rc)
			return rc;
		bo->bound = 1;
	}
	*ggtt = bo->ggtt;
	return 0;
}

/* ---- the scanout fence (gen2/3) ------------------------------------------------- */

/* The fence for the surface the plane is about to read: pinned over its
 * binding (the one it has stays).  Returns the object whose pin the
 * caller lets go of once the plane has moved off it, or NULL. */
static int scanout_fence_get(struct lg_display *d, struct lg_pipe *p, struct drm_gem_object *o,
			     uint32_t ggtt, struct drm_gem_object **old)
{
	*old = NULL;
	if (p->fence_obj == o)
		return 0;
	if (needs_fence(d, o)) {
		int rc = i915_legacy_fence_pin(d->i915, o, ggtt);
		if (rc) {
			static int said;
			if (said < 4) {
				said++;
				kprintf("[drm] i915: no fence register for a tiled scanout surface (%d)\n",
					rc);
			}
			return rc;
		}
		drm_gem_get(o);
	} else {
		o = NULL;
	}
	*old = p->fence_obj;
	p->fence_obj = o;
	return 0;
}

/* A pin the plane no longer needs: released once the plane has latched
 * what replaced it (the next vblank; at once on a pipe that is off). */
static void scanout_fence_put(struct lg_display *d, int pipe, struct drm_gem_object *o)
{
	if (!o)
		return;
	lg_wait_for_vblank(d, pipe);
	i915_legacy_fence_unpin(d->i915, o);
	drm_gem_put(o);
}

/* ---- the primary plane ------------------------------------------------------------ */

void lg_plane_init(struct lg_display *d)
{
	/* Mobile gen2/3 with a compressor: plane A (the compressor's) feeds
	 * pipe B, where the LVDS and the fitter are. */
	int cross = d->ver < 4 && d->num_pipes == 2 &&
		    (d->is_i85x || d->is_i915gm || d->is_i945gm);

	for (int p = 0; p < LG_MAX_PIPES; p++)
		d->pipes[p].plane = cross ? !p : p;
}

/* ---- plane properties ------------------------------------------------------------- */

/* The rotation each primary plane is asked for (lg_plane_set_rotation),
 * and the one its control register and offsets were last written with. */
static uint32_t g_rotation[LG_MAX_PIPES];
static uint32_t g_rotation_hw[LG_MAX_PIPES];

void lg_plane_set_rotation(struct lg_display *d, int pipe, uint32_t rotation)
{
	(void)d;
	if (pipe >= 0 && pipe < LG_MAX_PIPES)
		g_rotation[pipe] = rotation;
}

/* What a primary plane on `pipe' can do: from gen4 rotation by 180
 * degrees, on Cherryview's pipe B also the reflection along x. */
static uint32_t primary_rotations(struct lg_display *d, int pipe)
{
	uint32_t rot = DRM_MODE_ROTATE_0;

#if I915_FEAT_LEGACY_PLANE_PROPS
	if (d->ver >= 4)
		rot |= DRM_MODE_ROTATE_180;
	if (d->is_chv && pipe == 1)
		rot |= DRM_MODE_REFLECT_X;
#else
	(void)d;
	(void)pipe;
#endif
	return rot;
}

/* The primary plane's rotation as programmed: none where the property is
 * not offered. */
static uint32_t primary_rotation(struct lg_display *d, int pipe)
{
	uint32_t rot = pipe >= 0 && pipe < LG_MAX_PIPES ? g_rotation[pipe] : 0;

	if (rot & ~primary_rotations(d, pipe))
		return DRM_MODE_ROTATE_0;
	return rot;
}

/* The pipe's colour enables in the primary plane's or the cursor's
 * control register: the pipe gamma always, before colour management. */
static uint32_t pipe_color_bits(struct lg_display *d, int pipe, int cursor)
{
#if I915_FEAT_LEGACY_COLOR_MGMT
	return lg_color_plane_bits(d, pipe, cursor);
#else
	(void)pipe;
	if (cursor)
		return (d->is_i845 || d->is_i865) ? CURSOR_PIPE_GAMMA_ENABLE :
						    MCURSOR_PIPE_GAMMA_ENABLE;
	return DISP_PIPE_GAMMA_ENABLE;
#endif
}

int lg_plane_props_init(struct lg_display *d, int crtc)
{
#if I915_FEAT_LEGACY_PLANE_PROPS
	struct drm_device *dev = d->drm;
	struct drm_plane *prim = drm_crtc_primary(dev, crtc);
	struct drm_plane *cur = drm_crtc_cursor(dev, crtc);
	uint32_t rot = DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180;
	int rc = 0, r;

	if (!prim || !cur)
		return -ENODEV;
	if (d->ver >= 4) {
		/* A crtc is not a pipe here: crtc 1 is the one that gets
		 * pipe B whenever it is free, and the check refuses the
		 * reflection on any other pipe. */
		if (d->is_chv && crtc == 1)
			rot |= DRM_MODE_REFLECT_X;
		r = drm_plane_create_rotation_property(dev, prim, DRM_MODE_ROTATE_0, rot);
		if (r && !rc)
			rc = r;
	}
	if (d->is_vlv || d->is_chv) {
		r = drm_plane_create_blend_mode_property(dev, prim, 1u << DRM_MODE_BLEND_PREMULTI);
		if (r && !rc)
			rc = r;
	}
	r = drm_plane_create_zpos_immutable_property(dev, prim, 0);
	if (r && !rc)
		rc = r;
	if (d->ver >= 4) {
		r = drm_plane_create_rotation_property(dev, cur, DRM_MODE_ROTATE_0,
						       DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180);
		if (r && !rc)
			rc = r;
	}
	r = drm_plane_create_blend_mode_property(dev, cur, 1u << DRM_MODE_BLEND_PREMULTI);
	if (r && !rc)
		rc = r;
	/* above the primary plane (the sprites are not offered) */
	r = drm_plane_create_zpos_immutable_property(dev, cur, 1);
	if (r && !rc)
		rc = r;
	return rc;
#else
	(void)d;
	(void)crtc;
	return 0;
#endif
}

static uint32_t plane_ctl(struct lg_display *d, int pipe, const struct drm_framebuffer *fb)
{
	uint32_t ctl = DISP_ENABLE | pipe_color_bits(d, pipe, 0);
	uint32_t rot = primary_rotation(d, pipe);

	if (d->is_g4x || d->is_ilk || d->is_snb || d->is_ivb)
		ctl |= DISP_TRICKLE_FEED_DISABLE;
	ctl |= format_bits(fb->format);
	if (d->ver >= 4 && fb->modifier == I915_FORMAT_MOD_X_TILED)
		ctl |= DISP_TILED;
	if (rot & DRM_MODE_ROTATE_180)
		ctl |= DISP_ROTATE_180;
	if (rot & DRM_MODE_REFLECT_X)
		ctl |= DISP_MIRROR;
	if (d->ver < 5)
		ctl |= DISP_PIPE_SEL(pipe);
	return ctl;
}

static void plane_write(struct lg_display *d, struct lg_pipe *p, uint32_t ctl, int full)
{
	int pl = p->plane;
	uint32_t x = p->x, y = p->y;
	uint32_t rot = primary_rotation(d, p->pipe);
	uint32_t lin;

	/* Rotated, the plane starts at the last pixel of its window (the
	 * last of the first line when only reflected). */
	if (rot & DRM_MODE_ROTATE_180) {
		x += p->width - 1;
		y += p->height - 1;
	} else if (rot & DRM_MODE_REFLECT_X) {
		x += p->width - 1;
	}
	lin = y * p->stride + x * p->cpp;

	if (full) {
		lg_wr(d, DSPSTRIDE(d, pl), p->stride);
		if (d->ver < 4) {
			/* the plane's window is the whole pipe */
			lg_wr(d, DSPPOS(d, pl), 0);
			lg_wr(d, DSPSIZE(d, pl), ((p->height - 1) << 16) | (p->width - 1));
		}
		if (d->is_chv && p->pipe == 1) {
			lg_wr(d, PRIMPOS(d, pl), 0);
			lg_wr(d, PRIMSIZE(d, pl), ((p->height - 1) << 16) | (p->width - 1));
			lg_wr(d, PRIMCNSTALPHA(d, pl), 0);
		}
	}
	if (d->ver >= 4) {
		lg_wr(d, DSPLINOFF(d, pl), lin);
		lg_wr(d, DSPTILEOFF(d, pl), (y << 16) | x);
	}
	/* the control register right before the surface, so that an enable
	 * and its address arm together */
	if (full)
		lg_wr(d, DSPCNTR(d, pl), ctl);
	if (d->ver >= 4)
		lg_wr(d, DSPSURF(d, pl), p->surf);
	else
		lg_wr(d, DSPADDR(d, pl), p->surf + lin);
	lg_posting_read(d, d->ver >= 4 ? DSPSURF(d, pl) : DSPADDR(d, pl));
}

int lg_plane_update(struct lg_display *d, int pipe, struct drm_framebuffer *fb, uint32_t x,
		    uint32_t y)
{
	struct lg_pipe *p = &d->pipes[pipe];
	struct drm_gem_object *old_fence;
	uint32_t ggtt;
	int rc;

	if (!fb) {
		lg_plane_disable(d, pipe);
		return 0;
	}
	rc = bo_bind(d, fb->obj, &ggtt);
	if (rc)
		return rc;
	rc = scanout_fence_get(d, p, fb->obj, ggtt, &old_fence);
	if (rc)
		return rc;
	p->surf = ggtt + fb->offset;
	p->stride = fb->pitch;
	p->width = p->cfg.src_w;
	p->height = p->cfg.src_h;
	p->format = fb->format;
	p->cpp = (uint32_t)format_cpp(fb->format);
	p->modifier = fb->modifier;
	p->x = x;
	p->y = y;
	g_rotation_hw[pipe] = primary_rotation(d, pipe);
	plane_write(d, p, plane_ctl(d, pipe, fb), 1);
	p->plane_enabled = 1;
	scanout_fence_put(d, pipe, old_fence);
	i915_dbg("[drm] i915: plane %c on pipe %c: %ux%u pitch %u at %08x%s\n", 'A' + p->plane,
		 'A' + pipe, p->width, p->height, p->stride, p->surf,
		 p->modifier == I915_FORMAT_MOD_X_TILED ? ", X tiled" : "");
	return 0;
}

int lg_plane_flip(struct lg_display *d, int pipe, struct drm_framebuffer *fb, uint32_t x,
		  uint32_t y)
{
	struct lg_pipe *p = &d->pipes[pipe];
	struct drm_gem_object *old_fence;
	uint32_t ggtt;
	int rc;

	if (!fb || !p->plane_enabled || fb->pitch != p->stride || fb->format != p->format ||
	    fb->modifier != p->modifier || primary_rotation(d, pipe) != g_rotation_hw[pipe])
		return lg_plane_update(d, pipe, fb, x, y);
	rc = bo_bind(d, fb->obj, &ggtt);
	if (rc)
		return rc;
	rc = scanout_fence_get(d, p, fb->obj, ggtt, &old_fence);
	if (rc)
		return rc;
	p->surf = ggtt + fb->offset;
	p->x = x;
	p->y = y;
	/* i830/i845 latch every register as written: rewrite all of it */
	plane_write(d, p, plane_ctl(d, pipe, fb), d->is_i830 || d->is_i845);
	scanout_fence_put(d, pipe, old_fence);
	return 0;
}

void lg_plane_disable(struct lg_display *d, int pipe)
{
	struct lg_pipe *p = &d->pipes[pipe];
	int pl = p->plane;
	/* the pipe gamma bit (and the CSC bit) also colour the pipe's
	 * background: keep them */
	uint32_t ctl = pipe_color_bits(d, pipe, 0) | (d->ver < 5 ? DISP_PIPE_SEL(pipe) : 0);

	lg_wr(d, DSPCNTR(d, pl), ctl);
	if (d->ver >= 4) {
		lg_wr(d, DSPSURF(d, pl), 0);
		lg_posting_read(d, DSPSURF(d, pl));
	} else {
		lg_wr(d, DSPADDR(d, pl), 0);
		lg_posting_read(d, DSPADDR(d, pl));
	}
	p->plane_enabled = 0;
	p->surf = 0;
	if (p->fence_obj) {
		struct drm_gem_object *o = p->fence_obj;
		p->fence_obj = NULL;
		scanout_fence_put(d, pipe, o);
	}
}

int lg_plane_enabled_hw(struct lg_display *d, int plane, int *pipe)
{
	uint32_t v = lg_rd(d, DSPCNTR(d, plane));

	if (pipe)
		*pipe = d->ver >= 5 ? plane : (int)((v & DISP_PIPE_SEL_MASK) >> 24);
	return !!(v & DISP_ENABLE);
}

/* A primary plane covers its whole pipe (it has no window of its own on
 * most of these parts), unscaled, from inside the framebuffer. */
int lg_plane_check(struct lg_display *d, const struct drm_plane_state *ps,
		   const struct lg_config *cfg)
{
	const struct drm_framebuffer *fb = ps->fb;
	uint32_t rot = ps->rotation ? ps->rotation : DRM_MODE_ROTATE_0;

	if (!fb)
		return 0;
	if (!format_ok(d, fb->format))
		return -EINVAL;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR && fb->modifier != I915_FORMAT_MOD_X_TILED)
		return -EINVAL;
	/* what the plane on the pipe chosen can do; Cherryview ignores the
	 * mirror bit while it rotates */
	if (rot & ~primary_rotations(d, cfg->pipe))
		return -EINVAL;
	if ((rot & DRM_MODE_ROTATE_180) && (rot & DRM_MODE_REFLECT_X))
		return -EINVAL;
	/* gen2/3: through a fence, which takes the object's layout */
	if (fb->modifier == I915_FORMAT_MOD_X_TILED && d->ver < 4 &&
	    (!fb->obj || !fb->obj->priv ||
	     ((struct i915_bo *)fb->obj->priv)->tiling != I915_TILING_X))
		return -EINVAL;
	if (ps->crtc_x || ps->crtc_y || ps->crtc_w != cfg->src_w || ps->crtc_h != cfg->src_h)
		return -EINVAL;
	if ((ps->src_w >> 16) != ps->crtc_w || (ps->src_h >> 16) != ps->crtc_h)
		return -EINVAL;
	if ((ps->src_x & 0xffff) || (ps->src_y & 0xffff))
		return -EINVAL;
	uint32_t x = ps->src_x >> 16, y = ps->src_y >> 16;
	if (x + ps->crtc_w > fb->width || y + ps->crtc_h > fb->height)
		return -EINVAL;
	/* the offsets the plane is given: those of the last pixel of the
	 * window when rotated (of its first line when reflected) */
	uint32_t ox = x, oy = y;
	if (rot & (DRM_MODE_ROTATE_180 | DRM_MODE_REFLECT_X))
		ox = x + ps->crtc_w - 1;
	if (rot & DRM_MODE_ROTATE_180)
		oy = y + ps->crtc_h - 1;
	int cpp = format_cpp(fb->format);
	if (fb->pitch > max_stride(d, fb->modifier, cpp))
		return -EINVAL;
	/* An X-tiled plane that reads past the stride wraps to the next
	 * tile row mid line; and the tile offsets are 12 bits. */
	if (fb->modifier == I915_FORMAT_MOD_X_TILED) {
		if ((x + ps->crtc_w) * (uint32_t)cpp > fb->pitch)
			return -EINVAL;
		if (ox > 4095 || oy > 4095)
			return -EINVAL;
	}
	/* the gen4+ linear offset register and DSPADDR are 32-bit sums */
	if ((uint64_t)oy * fb->pitch + (uint64_t)ox * cpp + fb->offset > 0xffffffffull)
		return -EINVAL;
	return 0;
}

/* ---- the palette ---------------------------------------------------------------- */

void lg_gamma_load(struct lg_display *d, int pipe, const uint16_t gamma[3][256])
{
	for (int i = 0; i < 256; i++) {
		uint32_t v = ((uint32_t)(gamma[0][i] >> 8) << 16) |
			     ((uint32_t)(gamma[1][i] >> 8) << 8) | (uint32_t)(gamma[2][i] >> 8);
		if (d->gmch)
			lg_wr(d, PALETTE(d, pipe, i), v);
		else
			lg_wr(d, LGC_PALETTE(pipe, i), v);
	}
}

/* ---- the VGA plane --------------------------------------------------------------- */

#define LGP_VGA_SR_INDEX 0x3c4
#define LGP_VGA_SR_DATA 0x3c5
#define LGP_VGA_MSR_READ 0x3cc
#define LGP_VGA_MSR_WRITE 0x3c2
#define LGP_VGA_SR01_SCREEN_OFF (1u << 5)
#define LGP_VGA_MIS_COLOR (1u << 0)
#define LGP_VGA_MIS_ENB_MEM_ACCESS (1u << 1)

void lg_vga_disable(struct lg_display *d)
{
	uint32_t reg = d->gmch ? VGACNTRL : CPU_VGACNTRL;
	uint16_t cmd = pci_cfg_read16(d->i915->pci, 0x04);

	/* The legacy screen off through the sequencer, VGA memory decode
	 * and colour mode off (only with I/O decode enabled). */
	if (cmd & 0x1) {
		outb(LGP_VGA_SR_INDEX, 0x01);
		uint8_t sr1 = inb(LGP_VGA_SR_DATA);
		outb(LGP_VGA_SR_DATA, sr1 | LGP_VGA_SR01_SCREEN_OFF);
		uint8_t msr = inb(LGP_VGA_MSR_READ);
		msr &= (uint8_t)~(LGP_VGA_MIS_ENB_MEM_ACCESS | LGP_VGA_MIS_COLOR);
		outb(LGP_VGA_MSR_WRITE, msr);
		lg_udelay(300);
	}
	/* The bit defaults to 0 on the older parts but must be set, or the
	 * pipe gets stuck. */
	lg_wr(d, reg, VGA_DISP_DISABLE);
	lg_posting_read(d, reg);
}

/* ---- the cursor ------------------------------------------------------------------ */

static struct {
	uint32_t base, size, cntl;
	uint64_t phys; /* the contiguous copy (physical-address cursors) */
	uint32_t phys_pages;
} g_cur[LG_MAX_PIPES];

/* The cursors these parts fetch by physical address. */
static int cursor_needs_physical(struct lg_display *d)
{
	return d->is_i830 || d->is_i85x || d->is_i915g || d->is_i915gm || d->is_i945g ||
	       d->is_i945gm;
}

static int is_845_cursor(struct lg_display *d)
{
	return d->is_i845 || d->is_i865;
}

uint32_t lg_cursor_max_size(struct lg_display *d)
{
	if (is_845_cursor(d) || d->is_i830 || d->is_i85x || d->is_i915g || d->is_i915gm)
		return 64;
	return 256;
}

int lg_cursor_check(struct lg_display *d, const struct drm_plane_state *ps)
{
	const struct drm_framebuffer *fb = ps->fb;
	uint32_t w = ps->crtc_w, h = ps->crtc_h;
	uint32_t rot = ps->rotation ? ps->rotation : DRM_MODE_ROTATE_0;

	if (!fb)
		return 0;
	if (fb->format != DRM_FORMAT_ARGB8888 || fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	/* from gen4 the cursor rotates by 180 degrees, nothing else */
	if (rot != DRM_MODE_ROTATE_0 &&
	    !(I915_FEAT_LEGACY_PLANE_PROPS && d->ver >= 4 && rot == DRM_MODE_ROTATE_180))
		return -EINVAL;
	/* no panning or scaling of a cursor */
	if (ps->src_x || ps->src_y || (ps->src_w >> 16) != w || (ps->src_h >> 16) != h)
		return -EINVAL;
	if (w == 0 || h == 0 || w > fb->width || h > fb->height)
		return -EINVAL;
	if (is_845_cursor(d)) {
		/* only the width is limited, to multiples of 64 */
		if (w % 64 || w > lg_cursor_max_size(d) || h > 1023)
			return -EINVAL;
		switch (fb->pitch) {
		case 256:
		case 512:
		case 1024:
		case 2048:
			break;
		default:
			return -EINVAL;
		}
		return 0;
	}
	if (w != 64 && w != 128 && w != 256)
		return -EINVAL;
	if (w > lg_cursor_max_size(d) || h != w)
		return -EINVAL;
	if (fb->pitch != w * 4)
		return -EINVAL;
	if (fb->offset & 4095)
		return -EINVAL;
	return 0;
}

/* Copy the cursor image into a buffer the hardware can fetch by physical
 * address; flushed, as the display reads memory. */
static int cursor_physical(struct lg_display *d, int pipe, const struct drm_framebuffer *fb,
			   uint32_t w, uint32_t h, uint32_t *base)
{
	uint32_t bytes = w * h * 4;
	uint32_t pages = (bytes + 4095) / 4096;
	struct drm_gem_object *o = fb->obj;

	if (!o || !o->pages)
		return -EINVAL;
	if (g_cur[pipe].phys_pages < pages) {
		if (g_cur[pipe].phys)
			mm_free_contiguous_pages(g_cur[pipe].phys, g_cur[pipe].phys_pages);
		g_cur[pipe].phys = 0;
		g_cur[pipe].phys_pages = 0;
		/* the i830 wants 16 KB alignment, the others a page */
		uint64_t phys = mm_allocate_contiguous_pages_aligned(pages < 4 ? 4 : pages,
								     d->is_i830 ? 4 : 1);
		if (!phys)
			return -ENOMEM;
		if (phys + (uint64_t)pages * 4096 > 0x100000000ull) {
			mm_free_contiguous_pages(phys, pages < 4 ? 4 : pages);
			return -ENOMEM;
		}
		g_cur[pipe].phys = phys;
		g_cur[pipe].phys_pages = pages < 4 ? 4 : pages;
	}
	uint8_t *dst = phys_to_virt(g_cur[pipe].phys);
	for (uint32_t row = 0; row < h; row++) {
		uint64_t src_off = (uint64_t)fb->offset + (uint64_t)row * fb->pitch;
		for (uint32_t done = 0; done < w * 4;) {
			uint64_t off = src_off + done;
			uint32_t page = (uint32_t)(off / 4096);
			uint32_t in_page = (uint32_t)(off & 4095);
			uint32_t n = 4096 - in_page;
			if (n > w * 4 - done)
				n = w * 4 - done;
			if (page >= o->npages)
				return -EINVAL;
			mm_memcpy(dst + row * w * 4 + done,
				  (uint8_t *)phys_to_virt(o->pages[page]) + in_page, n);
			done += n;
		}
	}
	for (uint32_t off = 0; off < bytes; off += 64)
		__asm__ volatile("clflush (%0)" ::"r"(dst + off) : "memory");
	__asm__ volatile("mfence" ::: "memory");
	*base = (uint32_t)g_cur[pipe].phys;
	return 0;
}

static uint32_t cursor_pos(int x, int y)
{
	uint32_t pos = 0;

	if (x < 0) {
		pos |= CURSOR_POS_X_SIGN;
		x = -x;
	}
	if (y < 0) {
		pos |= CURSOR_POS_Y_SIGN;
		y = -y;
	}
	return pos | ((uint32_t)x & 0x7fffu) | (((uint32_t)y & 0x7fffu) << 16);
}

static void cursor_write(struct lg_display *d, int pipe, uint32_t cntl, uint32_t base,
			 uint32_t pos, uint32_t size)
{
	if (is_845_cursor(d)) {
		/* base, size and stride change only with the cursor off */
		if (g_cur[pipe].base != base || g_cur[pipe].size != size ||
		    g_cur[pipe].cntl != cntl) {
			lg_wr(d, CURCNTR(d, 0), 0);
			lg_wr(d, CURBASE(d, 0), base);
			lg_wr(d, CURSIZE(d, 0), size);
			lg_wr(d, CURPOS(d, 0), pos);
			lg_wr(d, CURCNTR(d, 0), cntl);
			g_cur[pipe].base = base;
			g_cur[pipe].size = size;
			g_cur[pipe].cntl = cntl;
		} else {
			lg_wr(d, CURPOS(d, 0), pos);
		}
		return;
	}
	/* CURCNTR before CURPOS, and always CURBASE last: the base write
	 * arms the others (a write to any of them cancels an armed update) */
	if (g_cur[pipe].base != base || g_cur[pipe].cntl != cntl) {
		lg_wr(d, CURCNTR(d, pipe), cntl);
		lg_wr(d, CURPOS(d, pipe), pos);
		lg_wr(d, CURBASE(d, pipe), base);
		g_cur[pipe].base = base;
		g_cur[pipe].cntl = cntl;
	} else {
		lg_wr(d, CURPOS(d, pipe), pos);
		lg_wr(d, CURBASE(d, pipe), base);
	}
	lg_posting_read(d, CURBASE(d, pipe));
}

int lg_cursor_update(struct lg_display *d, int pipe, const struct drm_plane_state *ps)
{
	struct lg_pipe *p = &d->pipes[pipe];
	const struct drm_framebuffer *fb = ps ? ps->fb : NULL;
	uint32_t cntl, base = 0, size = 0;
	int rc;

	if (!fb) {
		lg_cursor_disable(d, pipe);
		return 0;
	}
	uint32_t w = ps->crtc_w, h = ps->crtc_h;
	int rotate = I915_FEAT_LEGACY_PLANE_PROPS && d->ver >= 4 &&
		     (ps->rotation & DRM_MODE_ROTATE_180);
	if (cursor_needs_physical(d)) {
		/* a new image (or size) is copied; a move keeps the copy */
		if (ps->fb_changed || !p->cursor_enabled || p->cursor_w != w || p->cursor_h != h) {
			rc = cursor_physical(d, pipe, fb, w, h, &base);
			if (rc)
				return rc;
		} else {
			base = (uint32_t)g_cur[pipe].phys;
		}
	} else {
		uint32_t ggtt;
		rc = bo_bind(d, fb->obj, &ggtt);
		if (rc)
			return rc;
		base = ggtt + fb->offset;
		/* A GMCH cursor rotated by 180 degrees starts at its last
		 * pixel; from Ironlake the hardware does that itself. */
		if (rotate && d->gmch)
			base += (h * w - 1) * 4;
	}
	if (is_845_cursor(d)) {
		uint32_t stride_bits = 0;
		for (uint32_t s = fb->pitch; s > 256; s >>= 1)
			stride_bits++;
		cntl = CURSOR_ENABLE | CURSOR_FORMAT_ARGB | pipe_color_bits(d, pipe, 1) |
		       (stride_bits << CURSOR_STRIDE_SHIFT);
		size = CURSOR_HEIGHT(h) | CURSOR_WIDTH(w);
	} else {
		cntl = pipe_color_bits(d, pipe, 1);
		if (rotate)
			cntl |= MCURSOR_ROTATE_180;
		if (d->is_snb || d->is_ivb)
			cntl |= MCURSOR_TRICKLE_FEED_DISABLE;
		cntl |= w == 64 ? MCURSOR_MODE_64_ARGB_AX :
			w == 128 ? MCURSOR_MODE_128_ARGB_AX : MCURSOR_MODE_256_ARGB_AX;
		if (d->ver < 5 && !d->is_g4x)
			cntl |= MCURSOR_PIPE_SEL(pipe);
	}
	cursor_write(d, pipe, cntl, base, cursor_pos(ps->crtc_x, ps->crtc_y), size);
	p->cursor_enabled = 1;
	p->cursor_w = w;
	p->cursor_h = h;
	p->cursor_base = base;
	p->cursor_x = ps->crtc_x;
	p->cursor_y = ps->crtc_y;
	return 0;
}

void lg_cursor_disable(struct lg_display *d, int pipe)
{
	struct lg_pipe *p = &d->pipes[pipe];

	if (is_845_cursor(d)) {
		if (pipe == 0)
			cursor_write(d, 0, 0, 0, 0, 0);
	} else {
		cursor_write(d, pipe, 0, 0, 0, 0);
	}
	p->cursor_enabled = 0;
	p->cursor_base = 0;
}
