// LikeOS -- the plane properties of the Intel display and the planes'
// part of the device's driver table.
//
// Each crtc has one primary plane and one cursor (drm_connector_add()
// makes them).  Once they exist, intel_plane_props_init() attaches what
// this generation's hardware can do with them:
//   - rotation: the primary of Skylake .. Tiger Lake 0/90/180/270, of
//     display version 13 on 0/180, with reflect-x from version 11;
//     Haswell's and Broadwell's primary 0/180; every cursor 0/180;
//   - COLOR_ENCODING (BT601, BT709, and BT2020 from version 10) and
//     COLOR_RANGE (limited, full) on the universal planes, BT709 /
//     limited by default;
//   - alpha and pixel blend mode (None, Pre-multiplied, Coverage) on the
//     universal planes; the cursor's blend mode is Pre-multiplied only;
//   - an immutable zpos: the primary at the bottom (0), the cursor above
//     every plane of its pipe (the pipe has no sprite planes, so 1).
// FB_DAMAGE_CLIPS is a device-wide core property: display version 12 and
// later take it (intel_plane_driver_setup()).
//
// The core's own minimal blending set (DRM_FEATURE_BLEND) is not used:
// it would give every plane rotate-0 only and the same blend modes.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_plane.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

void intel_plane_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	if (intel_display_legacy_part(i915))
		return;
	/* Damage clips are accepted from display version 12 on (the
	 * planes still fetch everything; a discrete card's scanout copy
	 * uses them). */
	if (I915_FEAT_DAMAGE_CLIPS && i915->info->display_ver >= 12)
		drv->features |= DRM_FEATURE_DAMAGE_CLIPS;
}

/* The first error of a sequence of property creations, the rest still
 * tried: a plane without one property still has the others. */
static void keep_first(int *rc, int r)
{
	if (r && !*rc)
		*rc = r;
}

/* The universal primary plane (Skylake and later). */
static int skl_primary_props(struct i915_device *i915, struct drm_plane *plane)
{
	struct drm_device *dev = &i915->drm;
	int rc = 0;

	if (I915_FEAT_PLANE_PROPS) {
		unsigned int supported_csc;

		keep_first(&rc, drm_plane_create_rotation_property(dev, plane, DRM_MODE_ROTATE_0,
				intel_plane_supported_rotations(i915, DRM_PLANE_TYPE_PRIMARY)));

		supported_csc = BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709);
		if (i915->info->display_ver >= 10)
			supported_csc |= BIT(DRM_COLOR_YCBCR_BT2020);
		keep_first(&rc, drm_plane_create_color_properties(dev, plane, supported_csc,
				BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					BIT(DRM_COLOR_YCBCR_FULL_RANGE),
				DRM_COLOR_YCBCR_BT709, DRM_COLOR_YCBCR_LIMITED_RANGE));
	}
	if (I915_FEAT_PLANE_ALPHA) {
		keep_first(&rc, drm_plane_create_alpha_property(dev, plane));
		keep_first(&rc, drm_plane_create_blend_mode_property(dev, plane,
				BIT(DRM_MODE_BLEND_PIXEL_NONE) | BIT(DRM_MODE_BLEND_PREMULTI) |
					BIT(DRM_MODE_BLEND_COVERAGE)));
	}
	if (I915_FEAT_PLANE_PROPS)
		keep_first(&rc, drm_plane_create_zpos_immutable_property(dev, plane, 0));
	return rc;
}

/* Haswell's and Broadwell's primary plane (DSPCNTR): 180 degrees, no
 * blending of its own. */
static int hsw_primary_props(struct i915_device *i915, struct drm_plane *plane)
{
	struct drm_device *dev = &i915->drm;
	int rc = 0;

	if (!I915_FEAT_PLANE_PROPS)
		return 0;
	keep_first(&rc, drm_plane_create_rotation_property(dev, plane, DRM_MODE_ROTATE_0,
			intel_plane_supported_rotations(i915, DRM_PLANE_TYPE_PRIMARY)));
	keep_first(&rc, drm_plane_create_zpos_immutable_property(dev, plane, 0));
	return rc;
}

static int cursor_props(struct i915_device *i915, struct drm_plane *plane)
{
	struct drm_device *dev = &i915->drm;
	/* the pipe has no sprite planes: the cursor sits right above the
	 * primary */
	unsigned int zpos = 0 + 1;
	int rc = 0;

	if (!I915_FEAT_PLANE_PROPS)
		return 0;
	keep_first(&rc, drm_plane_create_rotation_property(dev, plane, DRM_MODE_ROTATE_0,
			intel_plane_supported_rotations(i915, DRM_PLANE_TYPE_CURSOR)));
	keep_first(&rc, drm_plane_create_blend_mode_property(dev, plane,
			BIT(DRM_MODE_BLEND_PREMULTI)));
	keep_first(&rc, drm_plane_create_zpos_immutable_property(dev, plane, zpos));
	return rc;
}

int intel_plane_props_init(struct i915_device *i915, int crtc)
{
	struct drm_device *dev = &i915->drm;
	struct drm_plane *prim, *cur;
	int rc = 0;

	if (!I915_FEAT_PLANE_PROPS && !I915_FEAT_PLANE_ALPHA)
		return 0;
	/* the older displays attach their own (intel_legacy_*) */
	if (intel_display_legacy_part(i915))
		return 0;
	prim = drm_crtc_primary(dev, crtc);
	cur = drm_crtc_cursor(dev, crtc);
	if (!prim || !cur)
		return -ENODEV;
	if (intel_plane_dspcntr(i915))
		keep_first(&rc, hsw_primary_props(i915, prim));
	else
		keep_first(&rc, skl_primary_props(i915, prim));
	keep_first(&rc, cursor_props(i915, cur));
	return rc;
}
