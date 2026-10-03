// LikeOS -- the universal planes of the Intel display: the primary plane
// and the cursor of each pipe, programmed from their atomic states.
//
// intel_display.c drives a crtc's planes only through the calls below:
// the check of a crtc's plane states, the full programming of the primary
// plane (or just its surface address for a flip), the cursor, and the
// disable.  The plane properties a client sees are attached once per crtc
// at init (intel_plane_props_init(), intel_plane_props.c), and the core
// feature bits that belong to planes (damage clips) are set in
// intel_plane_driver_setup().
//
// What the planes offer (with the switches of intel_features.h on):
//   Skylake .. Tiger Lake primary:  rotation 0/90/180/270, from display
//       version 11 also reflect-x; 90/270 only for Y-tiled surfaces
//       through a rotated GGTT view (i915_ggtt_view.h)
//   display version 13 on primary:  rotation 0/180 (+ reflect-x)
//   universal primary:  alpha, pixel blend mode None / Pre-multiplied /
//       Coverage, COLOR_ENCODING BT601/BT709 (+BT2020 from version 10),
//       COLOR_RANGE limited/full (defaults BT709, limited), zpos 0
//   Haswell / Broadwell primary:  rotation 0/180, zpos 0
//   cursor:  rotation 0/180, pixel blend mode Pre-multiplied, zpos 1
//   FB_DAMAGE_CLIPS from display version 12 (accepted; a discrete card's
//       scanout copy in local memory is refreshed over the damage only)
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2021 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_PLANE_H
#define KERNEL_DEV_GPU_I915_INTEL_PLANE_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_pipe;

/* What the planes of a pipe were last programmed with beyond the
 * surface itself (embedded in struct intel_pipe as `planes').  All zero
 * means "nothing beyond the defaults": unrotated, opaque, no plane
 * colour conversion -- what the planes have always been given. */
struct intel_plane_pipe_state {
	uint32_t primary_rotation; /* DRM_MODE_ROTATE_* | REFLECT_*, 0 = default */
	uint32_t cursor_rotation;
	uint16_t primary_alpha; /* 0 = default (opaque) */
	uint16_t primary_blend; /* DRM_MODE_BLEND_*, as programmed */
	/* enum drm_color_encoding / drm_color_range, as programmed */
	uint16_t primary_color_encoding;
	uint16_t primary_color_range;
	/* The cursor has a framebuffer but none of it is on the pipe: it is
	 * off in the hardware (its watermarks stay allocated). */
	int cursor_hidden;
};

/* Feature bits, hooks and limits of the planes in the device's driver
 * table (intel_display_driver_setup() calls it while the table is being
 * made, before registration).  Leaves the parts whose display is not the
 * DDI kind (intel_display_legacy_part()) alone. */
void intel_plane_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* Attach the plane properties to the primary and cursor planes of crtc
 * `crtc' (drm_crtc_primary() / drm_crtc_cursor()), right after
 * drm_connector_add() created them at display init.  0, or a negative
 * errno -- which the caller logs and otherwise ignores: the planes still
 * scan out without the properties. */
int intel_plane_props_init(struct i915_device *i915, int crtc);

/* The plane part of the atomic check of crtc `crtc' (active in `cs'):
 * the primary and cursor plane states of `st' against the client's mode
 * (cs->mode) and what this generation's planes can do.  Called with the
 * display lock held, after the crtc's mode was accepted and
 * cs->adjusted_mode settled.  Leaves each plane state's src / dst /
 * visible set (the source and destination clipped to the mode).  0, or
 * -EINVAL / -ERANGE through intel_display_check_reject() with the
 * reason. */
int intel_plane_atomic_check(struct i915_device *i915, struct drm_atomic_state *st,
			     int crtc, struct drm_crtc_state *cs);

/* What the watermarks and the display buffer have to carry for the
 * primary plane state `ps' after the check: whether the plane is on, the
 * size it fetches and the first source column (panning).  The clipped
 * rectangle where the planes are clipped, the requested one otherwise. */
void intel_plane_wm_geometry(const struct drm_plane_state *ps, int *on,
			     uint32_t *w, uint32_t *h, uint32_t *src_x);

/* Bind a framebuffer's object into the GGTT for scanout (once; the
 * offset stays in the object's i915_bo).  0 or a negative errno. */
int intel_plane_bo_bind(struct i915_device *i915, struct drm_gem_object *o);

/* Bind the framebuffer of plane state `ps' for scanout before a commit
 * touches the hardware, the way intel_plane_bo_bind() does; on a
 * discrete card, a system-memory surface whose local copy exists and
 * whose update says what it damaged gets only those rectangles copied.
 * 0 (also for a state without a framebuffer) or a negative errno. */
int intel_plane_fb_prepare(struct i915_device *i915, const struct drm_plane_state *ps);

/* The primary plane from its state: the whole thing when the layout
 * changed (or `force'), just the surface address for a flip; off when the
 * state has no framebuffer or nothing of it is visible.  0 or the bind's
 * errno. */
int intel_plane_primary_program(struct i915_device *i915, struct intel_pipe *p,
				const struct drm_plane_state *ps, int force);
/* The cursor of pipe `cp' from its state (off without a framebuffer). */
void intel_plane_cursor_program(struct i915_device *i915, struct intel_pipe *cp,
				const struct drm_plane_state *ps);
/* The primary plane off. */
void intel_plane_disable(struct i915_device *i915, struct intel_pipe *p);
/* Is the primary plane of `pipe' enabled in the hardware? */
int intel_plane_enabled(struct i915_device *i915, int pipe);
/* Broadwell and Haswell keep the older primary plane registers (DSPCNTR
 * and friends): display version below 9. */
int intel_plane_dspcntr(struct i915_device *i915);

/* ---- shared by the plane files ------------------------------------------------ */

/* Every DRM_MODE_ROTATE_* / REFLECT_* the plane of type `type'
 * (DRM_PLANE_TYPE_*) can do on this device. */
unsigned int intel_plane_supported_rotations(struct i915_device *i915, uint32_t type);

/* ---- 90/270-degree rotation ---------------------------------------------------- *
 *
 * A Y-tiled surface is scanned out rotated by 90 or 270 degrees through a
 * view of it in the GGTT whose tiles are laid out column by column: the
 * plane then reads the view as an ordinary tiled surface of the rotated
 * size.  intel_plane.c computes where everything lies (the view's shape
 * below, and the plane's offset and stride within it); binding such a
 * view is the GGTT's part. */
struct intel_rotated_view_info {
	uint32_t offset; /* first tile of the framebuffer in the object */
	uint32_t src_stride; /* tiles per tile row of the framebuffer */
	uint32_t width; /* tile columns the view covers */
	uint32_t height; /* tile rows the view covers */
	uint32_t dst_stride; /* tiles per tile row of the rotated view */
};

/* Bind the rotated view `info' of object `o' into the GGTT (made once,
 * kept with the object, found again for later flips); *ggtt gets its
 * address.  0 or a negative errno: -EOPNOTSUPP with the views switched
 * off (the check then refuses 90/270) or for a system-memory object on a
 * discrete part, -EINVAL for a view the object cannot hold, -ENOSPC,
 * -ENOMEM. */
int intel_plane_rotated_view_bind(struct i915_device *i915, struct drm_gem_object *o,
				  const struct intel_rotated_view_info *info, uint32_t *ggtt);

#endif
