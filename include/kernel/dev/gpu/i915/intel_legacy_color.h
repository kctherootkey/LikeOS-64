// LikeOS -- the optional features of the Intel displays before DDI: their
// share of the device's driver table, the colour management of their
// pipes, and the plane properties of their primary planes and cursors.
//
// intel_legacy_driver_setup_features() completes the device's driver
// table for such a part (the vblank hooks, the colour sizes and feature
// bits) while the table is made, before registration; it is the only
// entry point the rest of the driver calls.  The lg_* calls below are the
// backend's own: the mode set of intel_legacy_display.c computes, checks
// and programs a pipe's colour stages through them, and the plane code
// asks which colour bits its control registers carry.
//
// Each feature has its switch in intel_features.h
// (I915_FEAT_LEGACY_VBLANK_HW, I915_FEAT_LEGACY_PLANE_PROPS,
// I915_FEAT_LEGACY_COLOR_MGMT); with a switch at 0 the part of the
// display it covers behaves as it did before the feature existed.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2016 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_LEGACY_COLOR_H
#define KERNEL_DEV_GPU_I915_INTEL_LEGACY_COLOR_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct lg_display;
struct lg_config;

/* The driver table entries of a part whose display is not the DDI kind
 * (intel_display_legacy_part()): the hardware vblank hooks, the colour
 * table sizes and feature bits, and the plane feature bits.  Called last
 * while the device's copy of the table is made (before registration),
 * after every other feature's setup; it decides the entries it covers
 * itself, whatever the others left in them. */
void intel_legacy_driver_setup_features(struct i915_device *i915, struct drm_driver *drv);

/* ---- colour (intel_legacy_color.c) --------------------------------------------- */

/* The colour sizes and bits of the driver table for the platform. */
void lg_color_driver_setup(const struct i915_device *i915, struct drm_driver *drv);
/* Per crtc, once it exists: the legacy table size (256) and this part's
 * GAMMA_LUT / DEGAMMA_LUT sizes and CTM.  0 or a negative errno (logged
 * by the caller, otherwise ignored). */
int lg_color_crtc_init(struct lg_display *d, int crtc);
/* Can the part's pipes load the tables and matrix of `cs'?  0 or -EINVAL. */
int lg_color_check(struct lg_display *d, const struct drm_crtc_state *cs);
/* A full mode set of `pipe' with `cs': the pipe's colour state computed
 * and recorded, and cfg->gamma_mode set, before the pipe's configuration
 * register is written. */
void lg_color_prepare(struct lg_display *d, int pipe, const struct drm_crtc_state *cs,
		      struct lg_config *cfg);
/* The PIPECONF bits besides the gamma mode the recorded state needs (the
 * Valleyview WGC enable), for the pipe configuration writes. */
uint32_t lg_color_pipeconf_bits(struct lg_display *d, int pipe);
/* Load the recorded state's tables and matrices from `cs' into a pipe
 * that runs (after the mode set enabled it). */
void lg_color_load(struct lg_display *d, int pipe, const struct drm_crtc_state *cs);
/* A pipe that stays up whose colour state changed in `cs': everything
 * reprogrammed.  1 when the bits the planes carry changed (the caller
 * programs the planes again), else 0. */
int lg_color_update(struct lg_display *d, int pipe, const struct drm_crtc_state *cs);
/* The pipe gamma / CSC enables the primary plane (cursor 0) or the
 * cursor (cursor 1) of `pipe' carries in its control register. */
uint32_t lg_color_plane_bits(struct lg_display *d, int pipe, int cursor);
/* The pipe went off: its state back to the plain 8-bit palette. */
void lg_color_pipe_reset(struct lg_display *d, int pipe);

/* ---- plane properties (intel_legacy_plane.c) ----------------------------------- */

/* Rotation, blend mode and the immutable zpos on the primary plane and
 * cursor of `crtc', right after the crtc was made.  0 or a negative errno
 * (logged by the caller; the planes work without them). */
int lg_plane_props_init(struct lg_display *d, int crtc);
/* The rotation (DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_*) the primary plane
 * of `pipe' is programmed with from its next update on. */
void lg_plane_set_rotation(struct lg_display *d, int pipe, uint32_t rotation);

#endif
