// LikeOS -- the variable refresh timing generator of the Intel display.
//
// From display version 30 even a fixed refresh rate runs through the
// variable-refresh timing generator (intel_vrr_transcoder_enable() /
// _disable(), around the transcoder).  Variable refresh proper -- a
// DisplayPort or embedded-panel sink with a refresh range, VRR_ENABLED
// on the crtc, display version 11 and later -- goes through the other
// calls: the check of a crtc state, the sink told to ignore the timing
// parameters before the link trains, the enable once the pipe runs, the
// disable before it stops (or when the client turns it off), a push
// after each flip, and the connector's vrr_capable state after its EDID
// was read.  The feature bit and properties are set in
// intel_vrr_driver_setup() and intel_vrr_connector_init().
// I915_FEAT_VRR (intel_features.h) 0 leaves only the fixed-rate use.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_VRR_H
#define KERNEL_DEV_GPU_I915_INTEL_VRR_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_pipe;
struct intel_output;

/* What a pipe's timing generator runs (embedded in struct intel_pipe as
 * `vrr').  All zero: no variable refresh; the fixed-rate use of the
 * generator on display 30+ needs no state.  `enabled' is read without
 * the display lock by the vblank code (scanout position): the range is
 * stored before it is set and it is cleared before the range changes. */
struct intel_vrr_pipe_state {
	uint8_t enabled; /* variable refresh is running on the pipe */
	/* The sink behind the pipe has a refresh range holding the mode's
	 * refresh: the generator was set up for it with the transcoder
	 * (display versions 11 to 29). */
	uint8_t in_range;
	/* The range in lines, as the frame lengths it stands for (the
	 * registers hold one less, and on display 11/12 the minimum one line
	 * less again); with variable refresh off all three are the mode's
	 * vertical total. */
	uint32_t vmin, vmax, flipline;
	/* Lines before the end of the frame at which the vblank starts (and
	 * the double-buffered registers latch); display 11/12 program it as
	 * the "pipeline full" line count instead. */
	uint32_t guardband, pipeline_full;
};

/* What the sink behind an output allows (embedded in struct
 * intel_output as `vrr').  All zero: not capable. */
struct intel_vrr_output_state {
	uint8_t capable;
	uint16_t min_vrefresh, max_vrefresh; /* Hz, from the EDID's range */
};

/* The VRR feature bit in the device's driver table (from
 * intel_display_driver_setup(), before registration). */
void intel_vrr_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* Display 30+: the timing generator on at the fixed refresh of the
 * pipe's mode, after the DDI function and before the transcoder; and off
 * again after the transcoder has stopped, before its DDI function goes.
 * Display 11 to 29: when the sink behind the pipe (p->output, p->mode
 * set) has a refresh range holding the mode's refresh, the generator's
 * fixed range and guard band are set up (not switched on) at the same
 * point, and cleared again after the transcoder has stopped; nothing is
 * touched for any other sink.  With I915_FEAT_VRR 0 only the display 30
 * part runs. */
void intel_vrr_transcoder_enable(struct i915_device *i915, struct intel_pipe *p);
void intel_vrr_transcoder_disable(struct i915_device *i915, struct intel_pipe *p);

/* The VRR part of the atomic check of an active crtc whose output is
 * `o' (cs->adjusted_mode settled): variable refresh asked for
 * (cs->vrr_enabled) on a sink and pipe that can do it.  Display lock
 * held.  0, or -EINVAL through intel_display_check_reject(); a request
 * the sink cannot honour may also simply be ignored (0), the way a
 * fixed-rate sink ignores it. */
int intel_vrr_check(struct i915_device *i915, struct intel_output *o,
		    struct drm_crtc_state *cs);

/* Called at the end of every update of a running crtc (a full mode set
 * once the pipe, planes and vblank are on, or an update of a pipe that
 * stays up once its planes are programmed): switch variable refresh on
 * if `cs' asks for it and it is not running (p->vrr records it). */
void intel_vrr_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		      const struct drm_crtc_state *cs);

/* Called before the planes of a running pipe change and before a pipe
 * stops: switch variable refresh off if it runs and `cs' does not ask
 * for it -- `cs' NULL means the pipe is going off. */
void intel_vrr_disable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		       const struct drm_crtc_state *cs);

/* The planes of a running pipe `p' were reprogrammed (a flip, a cursor
 * change; the surface address written last): with variable refresh
 * running, push the frame out now rather than at the range's end. */
void intel_vrr_send_push(struct i915_device *i915, struct intel_pipe *p);

/* Output `o' got its DRM connector `conn' at display init: attach the
 * connector's vrr_capable property (DisplayPort and embedded panels,
 * display version 11 and later), false until a probe finds a range. */
void intel_vrr_connector_init(struct i915_device *i915, struct intel_output *o, int conn);

/* Before the link of DisplayPort output `o' trains for the crtc state
 * `cs' (o not active yet; cs->adjusted_mode settled): record whether the
 * sink is to ignore the MSA timing parameters -- set when its refresh
 * range holds the mode's refresh, so variable refresh can come on later
 * without retraining.  intel_vrr_enable() corrects it on the live link
 * when this was not called. */
void intel_vrr_link_prepare(struct i915_device *i915, struct intel_output *o,
			    const struct drm_crtc_state *cs);

/* The sink on output `o' (DRM connector index `conn') was probed and
 * its EDID parsed into the connector's display info: record whether it
 * has a refresh range the pipe can run (o->vrr) and update the
 * connector's vrr_capable property.  Display lock held. */
void intel_vrr_connector_update(struct i915_device *i915, struct intel_output *o, int conn);

#endif
