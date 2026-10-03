// LikeOS -- display-manager core: shared checks for atomic states.
//
// What most drivers' atomic_check would otherwise each write themselves:
// clip a plane's source and destination against the crtc's mode, check the
// scaling factor and positioning the hardware allows, and set the plane
// state's visible / src / dst; check that an enabled crtc has its primary
// plane on.  Drivers call these from their atomic_check; the core calls
// the plane check itself for drivers that opted in
// (DRM_FEATURE_ATOMIC_PLANE_CHECK).
//
// What the helpers read, so a caller knows what it must have filled in:
// a plane state's plane, crtc (index, -1 off), fb, src_x/y/w/h,
// crtc_x/y/w/h and rotation (0 counts as DRM_MODE_ROTATE_0); a crtc
// state's crtc, mode_valid (the crtc has a mode: it is "enabled", even
// when switched off by DPMS) and mode.  Nothing reads the states' `state'
// back pointers or the crtc state's plane_mask: where the whole request
// is needed it is a parameter.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Red Hat's and Intel's code: MIT
// Portions Copyright (C) 2014 Red Hat
// Portions Copyright (C) 2014 Intel Corp.
// Portions Copyright (C) 2014 Intel Corporation
// Portions Copyright (c) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_ATOMIC_HELPER_H
#define KERNEL_DEV_GPU_DRM_ATOMIC_HELPER_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_rect.h>

/* min_scale = max_scale = this: the plane cannot scale.  Scale factors are
 * 16.16 source pixels per destination pixel. */
#define DRM_PLANE_NO_SCALING (1 << 16)

/* The plane state's source (16.16, framebuffer coordinates) and
 * destination (crtc pixels) as asked, before any clipping. */
static inline struct drm_rect drm_plane_state_src(const struct drm_plane_state *state)
{
	struct drm_rect src = {
		.x1 = (int)state->src_x,
		.y1 = (int)state->src_y,
		.x2 = (int)(state->src_x + state->src_w),
		.y2 = (int)(state->src_y + state->src_h),
	};
	return src;
}

static inline struct drm_rect drm_plane_state_dest(const struct drm_plane_state *state)
{
	struct drm_rect dest = {
		.x1 = state->crtc_x,
		.y1 = state->crtc_y,
		.x2 = state->crtc_x + (int)state->crtc_w,
		.y2 = state->crtc_y + (int)state->crtc_h,
	};
	return dest;
}

/* ---- drm_atomic_helper.c ------------------------------------------------- */

/* Clip the plane against the crtc's mode and check it: 0 with
 * plane_state->visible / src / dst set (src clipped and still 16.16,
 * dst clipped to the mode), -ERANGE for a scale outside
 * [min_scale, max_scale], -EINVAL for a plane that does not cover the whole
 * crtc when !can_position, or one on a crtc without a mode when
 * !can_update_disabled.  A plane without a framebuffer is simply not
 * visible (0).  A plane wholly off the crtc is not visible either, and
 * that is not an error here: a driver that cannot leave such a plane
 * enabled refuses it itself.  crtc_state is the state, in the same
 * request, of the crtc the plane is on; it may be NULL only when the plane
 * has no framebuffer. */
int drm_atomic_helper_check_plane_state(struct drm_plane_state *plane_state,
					const struct drm_crtc_state *crtc_state,
					int min_scale,
					int max_scale,
					bool can_position,
					bool can_update_disabled);
/* The planes on crtc `crtc' in request `st' (bit = plane index): what a
 * crtc state's plane_mask would hold. */
uint32_t drm_atomic_helper_crtc_plane_mask(const struct drm_atomic_state *st,
					   int crtc);
/* The crtc has its primary plane among the planes on it in request `st'
 * (attached; whether that plane is visible is the plane check's
 * business): 0, or -EINVAL.  For hardware that cannot run a crtc without
 * its primary plane; call it for a crtc that has a mode. */
int drm_atomic_helper_check_crtc_primary_plane(const struct drm_atomic_state *st,
					       const struct drm_crtc_state *crtc_state);
/* The committed state of plane `p' as a plane state -- what the plane
 * shows until the request being checked or committed is applied; the
 * "old" state the damage helpers compare against.  src / dst / visible
 * are derived as the plane check would have derived them for the
 * committed crtc's mode (without rotation: the committed state has none).
 * Fields the committed plane does not keep are zero. */
void drm_atomic_helper_old_plane_state(struct drm_device *dev,
				       const struct drm_plane *p,
				       struct drm_plane_state *old_state);

/* The check of drm_atomic_helper_check_plane_state() for a legacy update
 * (no atomic request around it): plane `plane' showing `fb' on the
 * committed crtc `crtc', src (16.16) and dst (crtc pixels) as asked.  On
 * success src / dst are clipped and *visible says whether anything is
 * shown. */
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
				  bool *visible);

#endif
