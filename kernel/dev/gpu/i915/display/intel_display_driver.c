// LikeOS -- the display's part of the device's driver table.
//
// i915_driver_for() hands every device its own copy of the driver table
// before registration; this completes it for the platform: what the
// display's features ask of the DRM core (feature bits), the hooks they
// implement, and the sizes they offer.  Each feature sets its own part
// in its *_driver_setup(), so the table says only what the driver
// actually does on this part.  The parts whose display is not the DDI
// kind keep their own planes' formats, layouts and cursor sizes, set
// last.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/intel_plane.h>
#include <kernel/dev/gpu/i915/intel_color.h>
#include <kernel/dev/gpu/i915/intel_vblank.h>
#include <kernel/dev/gpu/i915/intel_vrr.h>
#include <kernel/dev/gpu/i915/intel_connector.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/i915/intel_dsc.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_legacy_color.h>

int intel_display_legacy_part(const struct i915_device *i915)
{
	if (i915->info->platform == I915_PLATFORM_HASWELL)
		return 0;
	return i915->info->gen < 8 || i915->info->platform == I915_PLATFORM_VALLEYVIEW ||
	       i915->info->platform == I915_PLATFORM_CHERRYVIEW;
}

/* Those parts offer what their planes take: linear and X layouts only,
 * their own pixel formats, and 64-pixel cursors on the oldest. */
static void legacy_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	switch (i915->info->platform) {
	case I915_PLATFORM_I830:
	case I915_PLATFORM_I845G:
	case I915_PLATFORM_I85X:
	case I915_PLATFORM_I865G:
	case I915_PLATFORM_I915G:
	case I915_PLATFORM_I915GM:
		drv->cursor_w = drv->cursor_h = 64;
		break;
	default:
		drv->cursor_w = drv->cursor_h = 256;
		break;
	}
	drv->fb_formats = intel_legacy_fb_formats;
	drv->nfb_formats = intel_legacy_nfb_formats;
	drv->fb_modifiers = intel_legacy_fb_modifiers;
	drv->nfb_modifiers = intel_legacy_nfb_modifiers;
	/* The vblank hooks, colour sizes and plane features of these parts;
	 * last, so they win over what the modern setups above put in. */
	intel_legacy_driver_setup_features(i915, drv);
}

void intel_display_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	/* The colour sizes start as the base table has them: a GAMMA_LUT_SIZE
	 * of 256 (the legacy table's), no degamma and no CTM -- which, with
	 * no colour feature bit, is also all a client sees.  The colour
	 * setups say what a part really has: intel_color_driver_setup() for
	 * the DDI displays (CTM, degamma and the larger GAMMA_LUT sizes with
	 * I915_FEAT_COLOR_MGMT), and for the older displays the legacy setup
	 * below, which runs last and sets its own. */

	/* Each feature's own part of the table.  A feature the parts with
	 * the older display do not have yet leaves them alone (each setup
	 * asks intel_display_legacy_part() itself); the hooks themselves
	 * hand a legacy part's calls to the intel_legacy_* backend, the way
	 * intel_detect() does. */
	intel_plane_driver_setup(i915, drv);
	intel_color_driver_setup(i915, drv);
	intel_vblank_driver_setup(i915, drv);
	intel_vrr_driver_setup(i915, drv);
	intel_connector_driver_setup(i915, drv);
	intel_hdmi_driver_setup(i915, drv);
	intel_dsc_driver_setup(i915, drv);

	if (intel_display_legacy_part(i915))
		legacy_driver_setup(i915, drv);
}
