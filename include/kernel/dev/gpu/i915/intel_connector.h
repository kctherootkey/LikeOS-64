// LikeOS -- the connectors of the Intel display: what is plugged in, and
// the modes it offers.
//
// A connector's index is its crtc's and names one output (struct
// intel_output, o->conn).  The DRM core asks through the driver table:
// detect (intel_detect()) and get_modes (intel_get_modes()), both
// declared in intel_display.h, and -- with I915_FEAT_PROBE_HELPER --
// mode_valid (intel_mode_valid() below) for every mode the list is built
// from; the core's probe helper then drops the refused ones and the
// duplicates and sorts the rest, the preferred mode first.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's and Dave Airlie's code: MIT
// Portions Copyright (C) 2019 Intel Corporation
// Portions Copyright (C) 2008 Intel Corporation
// Portions Copyright (C) 2006-2010 Intel Corporation
// Portions Copyright (c) 2006 Dave Airlie <airlied@linux.ie>

#ifndef KERNEL_DEV_GPU_I915_INTEL_CONNECTOR_H
#define KERNEL_DEV_GPU_I915_INTEL_CONNECTOR_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_output;

/* The probing hooks and feature bits in the device's driver table (from
 * intel_display_driver_setup(), before registration): detect and
 * get_modes, mode_valid and the probe helper bit (I915_FEAT_PROBE_HELPER,
 * not on the displays before the DDI kind), the atomic fences bit
 * (I915_FEAT_ATOMIC_FENCES). */
void intel_connector_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* drm_driver.mode_valid: can the output behind `c' show `mode' (the
 * transcoder's timing limits, the dot clock CDCLK allows, the DisplayPort
 * link and the branch device in front of the sink, the HDMI port's TMDS
 * limits, the panel scaler's bounds)?  MODE_OK or the reason it cannot.
 * Called by the core with no driver lock held, and from inside
 * get_modes with it held; takes the (nesting) display lock itself. */
enum drm_mode_status intel_mode_valid(struct drm_device *dev, struct drm_connector *c,
				      const struct drm_display_mode *mode);

/* The probe of connector `conn' at display init, before any client asks
 * (the console needs its modes): the core's probe helper where the
 * driver table has it, else detect and get_modes as before.  Process
 * context, display lock not held.  dev->conn[conn].connected and the
 * mode list are set on return. */
void intel_connector_probe(struct i915_device *i915, int conn);

#endif
