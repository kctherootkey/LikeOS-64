// LikeOS -- display-manager core: how planes are stacked and blended.
//
// A plane may carry, besides the core's own properties:
//   "rotation"          bitmask: rotate-0/90/180/270, reflect-x/y
//   "zpos"              range (or a fixed value): its place in the stack
//   "alpha"             range 0 (transparent) .. 0xffff (opaque)
//   "pixel blend mode"  enum: None / Pre-multiplied / Coverage
// and, from drm_color_mgmt.h, "COLOR_ENCODING" and "COLOR_RANGE" for YCbCr
// framebuffers.  A driver creates the ones its hardware has, plane by plane,
// with the functions below, once the device is registered
// (drm_dev_register() has run drm_kms_init()) and the plane exists.
//
// What a create function leaves behind (the contract with the atomic core):
//   - the property attached to &plane->properties with its initial value;
//   - plane->rotation_property / zpos_property / alpha_property /
//     blend_mode_property (color_encoding_property / color_range_property
//     for drm_color_mgmt.h) pointing at it -- the atomic core routes a write
//     of that property into the matching drm_plane_state field and keeps the
//     committed value in the plane field and in the property list;
//   - the plane's committed field (plane->rotation, zpos and
//     normalized_zpos, alpha, pixel_blend_mode, color_encoding,
//     color_range) set to the initial value.
// Identical properties -- same name, flags, range or entries -- are created
// once per device and the pointer is shared by every plane that asks for
// one, because the device's property table is small (DRM_MAX_PROPS) and a
// client finds a plane's properties by name in that plane's own list
// anyway.  Each kind is created at most once per plane.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Samsung's and Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Samsung Electronics Co.Ltd
// Portions Copyright (c) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_BLEND_H
#define KERNEL_DEV_GPU_DRM_BLEND_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_mode.h>

struct drm_device;
struct drm_plane;
struct drm_prop;
struct drm_atomic_state;

/* "pixel blend mode" values (drm_plane_state.pixel_blend_mode). */
#ifndef DRM_MODE_BLEND_PREMULTI
#define DRM_MODE_BLEND_PREMULTI 0
#endif
#ifndef DRM_MODE_BLEND_COVERAGE
#define DRM_MODE_BLEND_COVERAGE 1
#endif
#ifndef DRM_MODE_BLEND_PIXEL_NONE
#define DRM_MODE_BLEND_PIXEL_NONE 2
#endif

/* "alpha" of a plane that hides everything behind it. */
#ifndef DRM_BLEND_ALPHA_OPAQUE
#define DRM_BLEND_ALPHA_OPAQUE 0xffff
#endif

static inline bool drm_rotation_90_or_270(unsigned int rotation)
{
	return (rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) != 0;
}

/* ---- creating the properties (drm_blend.c) ------------------------------
 *
 * Each returns 0, -ENOMEM when the device's property table or the plane's
 * list is full, or -EINVAL for arguments that make no sense, a plane that
 * has the property already, or a device not registered yet (all warned
 * about). */

int drm_plane_create_alpha_property(struct drm_device *dev,
				    struct drm_plane *plane);
/* `rotation': the initial value (one rotate-* bit, maybe reflect-* bits);
 * `supported_rotations': every DRM_MODE_ROTATE_* / REFLECT_* bit the plane
 * can do. */
int drm_plane_create_rotation_property(struct drm_device *dev,
				       struct drm_plane *plane,
				       unsigned int rotation,
				       unsigned int supported_rotations);
/* A rotation the hardware cannot do, rewritten as an equivalent one: 180
 * degrees more with both reflections flipped.  The result may still be
 * unsupported; the caller checks. */
unsigned int drm_rotation_simplify(unsigned int rotation,
				   unsigned int supported_rotations);
/* A zpos the client may change within [min, max]. */
int drm_plane_create_zpos_property(struct drm_device *dev,
				   struct drm_plane *plane, unsigned int zpos,
				   unsigned int min, unsigned int max);
/* A zpos fixed by the hardware (an immutable [zpos, zpos] range). */
int drm_plane_create_zpos_immutable_property(struct drm_device *dev,
					     struct drm_plane *plane,
					     unsigned int zpos);
/* `supported_modes': a mask of BIT(DRM_MODE_BLEND_*); the default is
 * pre-multiplied when supported, else coverage, else none. */
int drm_plane_create_blend_mode_property(struct drm_device *dev,
					 struct drm_plane *plane,
					 unsigned int supported_modes);

/* ---- stacking ------------------------------------------------------------
 *
 * For the atomic check of a driver with mutable zpos: every crtc's planes
 * in the request get normalized_zpos 0..n-1 in the order of their zpos,
 * ties broken by plane id (lowest at the bottom).  A plane is on a crtc
 * when its state's `crtc' says so (whether it has a framebuffer or not);
 * planes on no crtc keep the value they have.  The request holds every
 * plane's state, so every crtc is normalised -- the planes of a crtc the
 * request does not touch come out as they are committed.
 *
 * *changed_crtcs (may be NULL) gets a bit per crtc index whose stacking may
 * differ from what is committed: its set of planes changed, or a plane on
 * it has a zpos other than the committed one (plane->zpos).
 * 0, or -EINVAL for a plane on a crtc that does not exist. */
int drm_atomic_normalize_zpos_crtcs(struct drm_device *dev,
				    struct drm_atomic_state *st,
				    uint32_t *changed_crtcs);
static inline int drm_atomic_normalize_zpos(struct drm_device *dev,
					    struct drm_atomic_state *st)
{
	return drm_atomic_normalize_zpos_crtcs(dev, st, NULL);
}

/* ---- for drm_color_mgmt.c --------------------------------------------- */

/* `fresh' -- the property just created -- or, when the device already has
 * one identical to it (name, flags, range, entries), that one, with
 * `fresh' given back to the table. */
struct drm_prop *drm_blend_share_property(struct drm_device *dev,
					  struct drm_prop *fresh);
/* Is the device past drm_kms_init (its core properties exist)?  Warned
 * about when not. */
bool drm_blend_device_ready(struct drm_device *dev);
/* Attach `prop' (shared when possible; NULL: it could not be made) to the
 * plane with `init_val'.  The property, or NULL when it could not be made
 * or the plane's list is full. */
struct drm_prop *drm_blend_plane_attach(struct drm_device *dev,
					struct drm_plane *plane,
					struct drm_prop *prop,
					uint64_t init_val);

#endif
