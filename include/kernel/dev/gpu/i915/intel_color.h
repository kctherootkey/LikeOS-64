// LikeOS -- colour management of the Intel display pipes: the gamma
// (and later degamma and CSC) of each pipe, and the colour bits of the
// planes that feed it.
//
// intel_display.c reaches the pipe's colour hardware only through the
// calls below: a per-crtc init after the crtcs exist, the check of a
// crtc state, the commit of a crtc state to its pipe, and the enable bits
// a plane's control registers carry for the pipe's colour stages.  The
// table sizes and the colour feature bits of the device's driver table
// are set in intel_color_driver_setup().
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019 Intel Corporation
// Portions Copyright (C) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_COLOR_H
#define KERNEL_DEV_GPU_I915_INTEL_COLOR_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_pipe;
struct intel_output;

/* What a pipe's colour stages were last programmed with (embedded in
 * struct intel_pipe as `color').  All zero: the 8-bit palette with the
 * pipe gamma on and nothing else, which is what the pipes have always
 * run and still run while no client has set a colour table or matrix. */
struct intel_color_pipe_state {
	uint32_t gamma_mode; /* GAMMA_MODE as written, 0 = 8-bit palette */
	uint32_t csc_mode; /* CSC_MODE as written, 0 = none */
	uint8_t csc_enabled; /* the pipe CSC (CTM) is in the path */
	uint8_t degamma_enabled; /* the pre-CSC LUT is in the path */
	/* The pipe runs a client's DEGAMMA_LUT / CTM / GAMMA_LUT (0: the
	 * 8-bit palette from the crtc's legacy table, as above). */
	uint8_t managed;
	/* With `managed': the pipe gamma enable the planes carry (before
	 * display version 11), and whether the hardware reads a table at
	 * all in this configuration. */
	uint8_t gamma_enabled;
	uint8_t lut_active;
};

/* The register a plane's colour bits go into, for
 * intel_color_plane_bits(). */
enum intel_color_plane_reg {
	INTEL_COLOR_REG_PLANE_CTL = 0, /* PLANE_CTL, before display version 10 */
	/* PLANE_COLOR_CTL, version 10 on: on version 10 (Gemini Lake) it
	 * carries the pipe gamma (bit 30) and pipe CSC (bit 23) enables,
	 * from 11 only the plane gamma disable (GAMMA_MODE / CSC_MODE
	 * enable the pipe's stages) */
	INTEL_COLOR_REG_PLANE_COLOR_CTL,
	INTEL_COLOR_REG_DSPCNTR, /* the DSPCNTR primary plane (Broadwell, Haswell) */
	INTEL_COLOR_REG_CUR_CTL, /* the cursor's CUR_CTL */
};

/* Gamma / degamma sizes and the colour feature bits in the device's
 * driver table (from intel_display_driver_setup(), before registration).
 * Leaves the parts whose display is not the DDI kind alone. */
void intel_color_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* Per-crtc colour set-up once the crtcs exist (display init, after
 * drm_connector_add() made crtc `crtc'): per-crtc LUT sizes and the
 * colour properties.  0, or a negative errno the caller logs and
 * otherwise ignores. */
int intel_color_crtc_init(struct i915_device *i915, int crtc);

/* The colour part of the atomic check of an active crtc whose output is
 * `o': the LUT and CTM blobs of `cs' against what the pipe can load.
 * Called with the display lock held.  0, or -EINVAL through
 * intel_display_check_reject(). */
int intel_color_check(struct i915_device *i915, struct intel_output *o,
		      struct drm_crtc_state *cs);

/* Program the colour stages of pipe `p' from `cs'.  `modeset' is 1 in a
 * full mode set, where it runs before the transcoder is enabled (the
 * table is in place before the pipe shows anything through it), and 0
 * when the pipe stays up, where it runs before the planes are
 * reprogrammed and decides itself whether anything changed
 * (cs->gamma_changed / cs->color_mgmt_changed).  Records what it wrote
 * in p->color for intel_color_plane_bits(). */
void intel_color_commit(struct i915_device *i915, struct intel_pipe *p,
			const struct drm_crtc_state *cs, int modeset);

/* The bits a plane on pipe `p' carries in register `reg' for the pipe's
 * colour stages (pipe gamma / CSC enables, and the plane's own gamma
 * disable), ORed into the rest of the register's value by the plane
 * code. */
uint32_t intel_color_plane_bits(struct i915_device *i915, const struct intel_pipe *p,
				enum intel_color_plane_reg reg);

#endif
