// LikeOS -- HDMI 2.0 link features of the Intel display: scrambling and
// the TMDS bit clock ratio (set in the transcoder and, over SCDC, in the
// sink), the TMDS limits of source, sink and DP++ adaptor, and the link
// repair after a hotplug.
//
// The mode-set path in intel_display.c calls in at three points: the
// transcoder's DDI function value (intel_hdmi_trans_ddi_bits()), just
// before the port's buffer is enabled with the transcoder already running
// (intel_hdmi_pre_port_enable(): the sink's SCDC side), and once the pipe
// runs (intel_hdmi_post_enable(): the sink's scrambler status).  The
// hotplug worker calls intel_hdmi_hpd_check() after the core re-probed an
// HDMI or DVI connector.  intel_hdmi.c calls the rest from detection, the
// mode check and its own enable and disable steps.  The rest of HDMI
// (detection, EDID, clocks, infoframes) is intel_hdmi.c's.
//
// The switches are I915_FEAT_HDMI_SCRAMBLING,
// I915_FEAT_HDMI_LOW_RATE_SCRAMBLING and I915_FEAT_DP_DUAL_MODE
// (intel_features.h).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2009 Intel Corporation
// Portions Copyright (C) 2019 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_HDMI_FEAT_H
#define KERNEL_DEV_GPU_I915_INTEL_HDMI_FEAT_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_output;
struct intel_pipe;

/* The HDMI link state of an output (embedded in struct intel_output as
 * `hdmi_link').  All zero: a plain TMDS link at most 340 MHz, nothing
 * set in the sink, no adaptor -- what HDMI has always been driven as. */
struct intel_hdmi_link_state {
	/* the link as the last mode set chose it (intel_hdmi_pre_enable()),
	 * kept until the output is disabled */
	uint8_t scrambling; /* the link runs scrambled */
	uint8_t high_tmds_clock_ratio; /* TMDS bit clock ratio 1/40 */
	/* A DP++ adaptor behind the port (enum drm_dp_dual_mode_type, 0 for
	 * none), found at detection, and the TMDS clock it passes (kHz, 0
	 * for no limit of its own). */
	uint8_t dual_mode_type;
	uint32_t dual_mode_max_tmds_khz;
	/* The sink lost the scrambling / clock ratio setting while the
	 * output was lit (seen by intel_hdmi_hpd_check()): the connector's
	 * link-status is BAD until the next full mode set of the output. */
	uint8_t link_bad;
};

/* HDMI-side settings in the device's driver table (from
 * intel_display_driver_setup(), before registration). */
void intel_hdmi_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* Extra TRANS_DDI_FUNC_CTL bits for HDMI output `o' on pipe `p' running
 * mode `m' (TRANS_DDI_HDMI_SCRAMBLING, TRANS_DDI_HIGH_TMDS_CHAR_RATE),
 * ORed into the transcoder's DDI function when the mode set programs the
 * transcoder (before intel_hdmi_pre_port_enable()).  Called for
 * INTEL_OUTPUT_HDMI outputs only (a DVI sink is never scrambled); must
 * not touch the hardware, and must return the same for the same state. */
uint32_t intel_hdmi_trans_ddi_bits(struct i915_device *i915, const struct intel_output *o,
				   const struct intel_pipe *p,
				   const struct drm_mode_modeinfo *m);

/* HDMI or DVI output `o' on pipe `p' running mode `m': the transcoder
 * runs, the port's buffer is about to be enabled (intel_hdmi_enable()).
 * Set the sink's scrambling and TMDS ratio over SCDC to match the
 * transcoder.  Mode set, display lock held. */
void intel_hdmi_pre_port_enable(struct i915_device *i915, struct intel_output *o,
				struct intel_pipe *p, const struct drm_mode_modeinfo *m);

/* HDMI or DVI output `o' on pipe `p' is fully up (buffer, planes,
 * vblank, backlight): check the sink's scrambler locked, as the HDMI 2.0
 * enable sequence asks.  Mode set, display lock held. */
void intel_hdmi_post_enable(struct i915_device *i915, struct intel_output *o,
			    struct intel_pipe *p);

/* The hotplug worker re-probed HDMI/DVI output `o' after a pulse on its
 * pin (display lock held).  An output still lit whose sink lost its
 * SCDC state (unplugged and plugged back) has its connector's
 * link-status set BAD, for the client to set the mode again.  0, or 1
 * to have the port looked at again after another debounce period. */
int intel_hdmi_hpd_check(struct i915_device *i915, struct intel_output *o);

/* ---- for intel_hdmi.c ------------------------------------------------------ */

/* The highest TMDS clock (kHz) the source drives on output `o': the
 * platform's (intel_dpll_hdmi_max_tmds_khz()), lowered by the VBT's
 * limit for the port where it has one. */
uint32_t intel_hdmi_source_max_tmds_khz(struct i915_device *i915, const struct intel_output *o);

/* The highest TMDS clock (kHz) output `o' may be given here: the source's,
 * the DP++ adaptor's, the sink's (its EDID; a DVI sink without one stops
 * at single link), and 340 MHz unless source and sink both scramble.
 * `hdmi_sink': the sink is an HDMI one. */
uint32_t intel_hdmi_port_clock_limit(struct i915_device *i915, const struct intel_output *o,
				     int hdmi_sink);

/* Work out the link of HDMI output `o' for a TMDS clock of `clock_khz'
 * into o->hdmi_link (scrambling, clock ratio) -- what
 * intel_hdmi_trans_ddi_bits() and intel_hdmi_pre_port_enable() then use.
 * Mode set, before the transcoder is programmed. */
void intel_hdmi_link_compute(struct i915_device *i915, struct intel_output *o,
			     uint32_t clock_khz);

/* Output `o' is being disabled (its transcoder still runs): the sink is
 * told to stop descrambling and to go back to the 1/10 ratio. */
void intel_hdmi_link_disable(struct i915_device *i915, struct intel_output *o);

/* The output is down: its link state is forgotten. */
void intel_hdmi_link_clear(struct intel_output *o);

/* Detection: forget the adaptor (before the EDID is read), then, the
 * EDID having said the sink is digital, look for a DP++ adaptor on the
 * port's DDC bus and note its type and TMDS limit. */
void intel_hdmi_dual_mode_reset(struct intel_output *o);
void intel_hdmi_dual_mode_detect(struct i915_device *i915, struct intel_output *o);

/* A type 2 adaptor's TMDS output buffers on (before the port is enabled)
 * or off (after it was disabled); nothing for type 1 or no adaptor. */
void intel_hdmi_dual_mode_set_tmds_output(struct i915_device *i915, struct intel_output *o,
					  int enable);

#endif
