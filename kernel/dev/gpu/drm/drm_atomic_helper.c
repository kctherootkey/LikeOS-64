// LikeOS -- display-manager core: shared checks for atomic states.
//
// Clipping a plane against its crtc with the scaling and positioning the
// hardware allows (filling the plane state's visible / src / dst), and
// checking that an enabled crtc shows its primary plane.  The same plane
// check serves a legacy update (drm_plane_helper_check_update), and
// drm_atomic_helper_old_plane_state() gives the committed plane as a
// state, the "old" side of the damage helpers.
//
// A library: nothing here keeps state, locks or allocates.  The callers
// hold whatever makes the device's committed objects stable (the mode-
// setting path they run on).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Red Hat's and Intel's code: MIT
// Portions Copyright (C) 2014 Red Hat
// Portions Copyright (C) 2014 Intel Corp.
// Portions Copyright (C) 2014 Intel Corporation
// Portions Copyright (c) 2016 Intel Corporation
// Portions Copyright (C) 2007-2008 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* 1: say in the log why a plane check refused a plane, with its
 * rectangles.  Off: a client that probes with TEST_ONLY makes refusals
 * routine, and the atomic core already names the first few it returns. */
#define DRM_ATOMIC_HELPER_DEBUG 0

#if DRM_ATOMIC_HELPER_DEBUG
#define helper_dbg(...) kprintf("[drm] " __VA_ARGS__)
#define helper_dbg_rect(prefix, r, fp) drm_rect_debug_print(prefix, r, fp)
#else
#define helper_dbg(...) do { } while (0)
#define helper_dbg_rect(prefix, r, fp) do { (void)(r); } while (0)
#endif

/*
 * The size of the area a crtc scans out for mode m.  A frame-packed
 * stereo mode carries both eyes one above the other, with a vertical
 * blank between them, so its active area is vdisplay + vtotal lines tall;
 * double scan and vscan are not applied (planes are placed in the mode's
 * own lines).
 */
static void crtc_mode_hv_timing(const struct drm_mode_modeinfo *m,
				int *hdisplay, int *vdisplay)
{
	*hdisplay = m->hdisplay;
	*vdisplay = m->vdisplay;
	if ((m->flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING)
		*vdisplay += m->vtotal;
}

/*
 * The plane check proper, on just what it reads of the crtc: whether it
 * has a mode, and which.  (The legacy path has no crtc state to hand, and
 * a crtc state is too big to build on the stack for it.)
 */
static int check_plane_state(struct drm_plane_state *plane_state,
			     bool crtc_enabled,
			     const struct drm_mode_modeinfo *mode,
			     int min_scale,
			     int max_scale,
			     bool can_position,
			     bool can_update_disabled)
{
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect *src = &plane_state->src;
	struct drm_rect *dst = &plane_state->dst;
	unsigned int rotation = plane_state->rotation;
	struct drm_rect clip = { 0, 0, 0, 0 };
	int hscale, vscale;

	*src = drm_plane_state_src(plane_state);
	*dst = drm_plane_state_dest(plane_state);

	if (!fb) {
		plane_state->visible = false;
		return 0;
	}

	/* a framebuffer without a crtc is refused before this is reached */
	if (WARN_ON(plane_state->crtc < 0)) {
		plane_state->visible = false;
		return 0;
	}

	if (!crtc_enabled && !can_update_disabled) {
		helper_dbg("cannot update a plane of a disabled crtc\n");
		return -EINVAL;
	}

	/* Scale and clip in the orientation the plane is shown in. */
	drm_rect_rotate(src, (int)(fb->width << 16), (int)(fb->height << 16),
			rotation);

	hscale = drm_rect_calc_hscale(src, dst, min_scale, max_scale);
	vscale = drm_rect_calc_vscale(src, dst, min_scale, max_scale);
	if (hscale < 0 || vscale < 0) {
		helper_dbg("invalid scaling of plane\n");
		helper_dbg_rect("src: ", &plane_state->src, true);
		helper_dbg_rect("dst: ", &plane_state->dst, false);
		return -ERANGE;
	}

	/* A crtc without a mode clips everything away. */
	if (crtc_enabled)
		crtc_mode_hv_timing(mode, &clip.x2, &clip.y2);

	plane_state->visible = drm_rect_clip_scaled(src, dst, &clip);

	drm_rect_rotate_inv(src, (int)(fb->width << 16), (int)(fb->height << 16),
			    rotation);

	if (!plane_state->visible)
		/*
		 * Nothing of the plane is on the crtc.  Some hardware can
		 * leave such a plane enabled, so this is not an error here;
		 * a driver that cannot refuses it in its own check.
		 */
		return 0;

	if (!can_position && !drm_rect_equals(dst, &clip)) {
		helper_dbg("plane must cover the entire crtc\n");
		helper_dbg_rect("dst: ", dst, false);
		helper_dbg_rect("clip: ", &clip, false);
		return -EINVAL;
	}

	return 0;
}

int drm_atomic_helper_check_plane_state(struct drm_plane_state *plane_state,
					const struct drm_crtc_state *crtc_state,
					int min_scale,
					int max_scale,
					bool can_position,
					bool can_update_disabled)
{
	/* the crtc state must be that of the plane's crtc */
	WARN_ON(plane_state->crtc >= 0 && crtc_state && crtc_state->crtc &&
		plane_state->crtc != crtc_state->crtc->index);

	if (!crtc_state) {
		/* Only a plane being switched off may come without one. */
		if (WARN_ON(plane_state->fb && plane_state->crtc >= 0)) {
			plane_state->src = drm_plane_state_src(plane_state);
			plane_state->dst = drm_plane_state_dest(plane_state);
			plane_state->visible = false;
			return -EINVAL;
		}
		return check_plane_state(plane_state, false, NULL, min_scale,
					 max_scale, can_position,
					 can_update_disabled);
	}

	return check_plane_state(plane_state, crtc_state->mode_valid != 0,
				 &crtc_state->mode, min_scale, max_scale,
				 can_position, can_update_disabled);
}

uint32_t drm_atomic_helper_crtc_plane_mask(const struct drm_atomic_state *st,
					   int crtc)
{
	uint32_t mask = 0;

	if (crtc < 0)
		return 0;
	for (uint32_t i = 0; i < st->dev->nplanes && i < DRM_MAX_PLANES; i++)
		if (st->planes[i].plane && st->planes[i].crtc == crtc)
			mask |= 1u << i;
	return mask;
}

int drm_atomic_helper_check_crtc_primary_plane(const struct drm_atomic_state *st,
					       const struct drm_crtc_state *crtc_state)
{
	const struct drm_crtc *crtc = crtc_state->crtc;
	uint32_t mask = drm_atomic_helper_crtc_plane_mask(st, crtc->index);

	/* needs at least one primary plane to be enabled */
	for (uint32_t i = 0; mask; i++, mask >>= 1)
		if ((mask & 1) && st->planes[i].plane->type == DRM_PLANE_TYPE_PRIMARY)
			return 0;

	helper_dbg("[CRTC:%u] primary plane missing\n", crtc->id);
	return -EINVAL;
}

/*
 * Whether a committed crtc has a mode, the way a request built from the
 * committed state sees it (mode_valid): a mode is set, whether or not its
 * pipe runs -- DPMS off keeps the mode and the planes on it.  With the
 * older semantics only a running crtc has one.
 */
static bool crtc_has_mode(const struct drm_crtc *crtc)
{
	if (DRM_ATOMIC_ENABLE_SEMANTICS)
		return crtc->enabled || crtc->active;
	return crtc->active != 0;
}

void drm_atomic_helper_old_plane_state(struct drm_device *dev,
				       const struct drm_plane *p,
				       struct drm_plane_state *old_state)
{
	struct drm_plane_state *os = old_state;
	const struct drm_crtc *crtc = NULL;

	mm_memset(os, 0, sizeof(*os));
	os->plane = (struct drm_plane *)p;
	os->crtc = p->crtc;
	os->fb_id = p->fb_id;
	os->fb = p->fb_id ? drm_fb_lookup(dev, p->fb_id) : NULL;
	os->src_x = p->src_x;
	os->src_y = p->src_y;
	os->src_w = p->src_w;
	os->src_h = p->src_h;
	os->crtc_x = p->crtc_x;
	os->crtc_y = p->crtc_y;
	os->crtc_w = p->crtc_w;
	os->crtc_h = p->crtc_h;
	os->hot_x = p->hot_x;
	os->hot_y = p->hot_y;
	/* The committed values of the optional plane properties, as a
	 * request starts from them; a plane that never had a rotation set
	 * shows itself unrotated. */
	os->rotation = p->rotation ? p->rotation : DRM_MODE_ROTATE_0;
	os->zpos = p->zpos;
	os->normalized_zpos = p->normalized_zpos;
	os->alpha = p->alpha;
	os->pixel_blend_mode = p->pixel_blend_mode;
	os->color_encoding = p->color_encoding;
	os->color_range = p->color_range;

	if (p->crtc >= 0 && (uint32_t)p->crtc < dev->ncrtc)
		crtc = &dev->crtc[p->crtc];
	if (!os->fb || !crtc) {
		os->src = drm_plane_state_src(os);
		os->dst = drm_plane_state_dest(os);
		os->visible = false;
		return;
	}
	/*
	 * The clipping the check did when this state was committed: no
	 * scaling limit (it passed then), positioning allowed.  A crtc
	 * without a mode clips everything away; one that is merely switched
	 * off (DPMS) keeps its mode and its planes, as a request built from
	 * the committed state takes it.
	 */
	check_plane_state(os, crtc_has_mode(crtc), &crtc->mode, 0, 0x7fffffff,
			  true, true);
}

int drm_plane_helper_check_update(struct drm_plane *plane,
				  const struct drm_crtc *crtc,
				  struct drm_framebuffer *fb,
				  struct drm_rect *src,
				  struct drm_rect *dst,
				  unsigned int rotation,
				  int min_scale,
				  int max_scale,
				  bool can_position,
				  bool can_update_disabled,
				  bool *visible)
{
	struct drm_plane_state plane_state;
	int ret;

	mm_memset(&plane_state, 0, sizeof(plane_state));
	plane_state.plane = plane;
	plane_state.crtc = crtc ? crtc->index : -1;
	plane_state.fb = fb;
	plane_state.fb_id = fb ? fb->id : 0;
	plane_state.src_x = (uint32_t)src->x1;
	plane_state.src_y = (uint32_t)src->y1;
	plane_state.src_w = (uint32_t)drm_rect_width(src);
	plane_state.src_h = (uint32_t)drm_rect_height(src);
	plane_state.crtc_x = dst->x1;
	plane_state.crtc_y = dst->y1;
	plane_state.crtc_w = (uint32_t)drm_rect_width(dst);
	plane_state.crtc_h = (uint32_t)drm_rect_height(dst);
	plane_state.rotation = rotation;

	ret = check_plane_state(&plane_state, crtc && crtc_has_mode(crtc),
				crtc ? &crtc->mode : NULL, min_scale, max_scale,
				can_position, can_update_disabled);
	if (ret)
		return ret;

	*src = plane_state.src;
	*dst = plane_state.dst;
	*visible = plane_state.visible;

	return 0;
}
