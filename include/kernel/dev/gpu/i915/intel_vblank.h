// LikeOS -- vblank of the Intel display pipes: the pipe interrupts
// (vblank and FIFO underrun), and the vblank bookkeeping of the crtcs
// they feed.
//
// A crtc's index is its connector's, not a pipe: the pipe is chosen at
// the mode set (pipe_for_output()), so everything here maps crtc ->
// output -> pipe.  intel_display.c switches a crtc's vblank on once its
// pipe runs and off before the pipe stops, and resets each crtc's state
// once at init; the interrupt handler hands the pipe interrupt bits to
// intel_display_irq() (declared in intel_display.h).  The core's vblank
// hooks (counter, scanout position, enable/disable) are set in
// intel_vblank_driver_setup() (I915_FEAT_VBLANK_HW); every update of the
// interrupt mask and enable registers goes through one lock.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2023 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_VBLANK_H
#define KERNEL_DEV_GPU_I915_INTEL_VBLANK_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/i915/intel_features.h>

struct i915_device;
struct intel_pipe;

/* Per-pipe vblank state beyond the counters in struct intel_pipe
 * (embedded there as `vbl').  All zero: the pipe serves no crtc's
 * vblank. */
struct intel_vblank_pipe_state {
	uint8_t on; /* the crtc's vblank is on (between crtc_on and crtc_off) */
	int8_t crtc; /* the crtc it serves while on, else unused */
	/* Lines the scanline counter (PIPEDSL) runs behind the line being
	 * scanned out: 1, or 2 on an HDMI transcoder of Haswell up to
	 * display version 20.  Set at crtc_on. */
	uint8_t scanline_offset;
};

/* When the vblank interrupt of a running crtc goes off: the switch
 * I915_FEAT_VBLANK_IRQ_OFF in intel_features.h. */

/* Vblank hooks and counter limits in the device's driver table (from
 * intel_display_driver_setup(), before registration). */
void intel_vblank_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* Once per crtc at display init, after drm_connector_add() made it: the
 * crtc starts with its vblank off (the core's reset state). */
void intel_vblank_crtc_reset(struct i915_device *i915, int crtc);

/* The pipe `p' runs for crtc `crtc' (transcoder enabled, planes
 * programmed): its vblank interrupt and the crtc's vblank bookkeeping
 * on.  From the mode set, display lock held, process context. */
void intel_vblank_crtc_on(struct i915_device *i915, struct intel_pipe *p, int crtc);

/* The pipe `p' that served crtc `crtc' (-1 when the caller no longer
 * knows: the crtc the pipe was switched on for is used) is about to
 * stop: its vblank interrupt and the crtc's vblank off, pending events
 * sent.  Before the planes and the transcoder go off; process context
 * (it waits for a vblank work in progress). */
void intel_vblank_crtc_off(struct i915_device *i915, struct intel_pipe *p, int crtc);

/* New watermarks were programmed on pipe `p': the FIFO underrun
 * interrupt, masked after the first underrun it reported, is let
 * through again so the engine can say whether they hold. */
void intel_vblank_underrun_rearm(struct i915_device *i915, struct intel_pipe *p);

#endif
