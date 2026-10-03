// LikeOS -- DisplayPort link training of the Intel display on the
// DisplayPort helper library: the receiver and repeater capabilities and
// the per-PHY training (intel_dp_link_training.c).
//
// The entry points the rest of the display uses -- intel_dp_link_train(),
// intel_dp_retrain_link(), intel_dp_init_lttpr_and_dprx_caps() -- are
// declared in intel_display.h; this header holds what intel_dp.c and the
// training file share besides.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2015 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_DP_LINK_TRAINING_H
#define KERNEL_DEV_GPU_I915_INTEL_DP_LINK_TRAINING_H

#include <kernel/dev/gpu/i915/intel_dp_aux.h>

struct i915_device;
struct intel_output;

/* The receiver capabilities (DPCD 0x000..0x00e) into `dpcd': with
 * I915_FEAT_DP_EXT_CAPS replaced by the extended copy at 0x2200 where the
 * sink has one (it may raise the rate to HBR3), else the base block only.
 * 0, or a negative errno (-EIO when DPCD_REV reads 0). */
int intel_dp_read_receiver_caps(struct intel_output *o, u8 dpcd[DP_RECEIVER_CAP_SIZE]);

/* The fastest link rate (10 kbit/s units, 0 = no limit) and the most
 * lanes (0 = no limit) the repeaters in front of the sink pass on. */
int intel_dp_lttpr_max_link_rate(const struct intel_output *o);
int intel_dp_lttpr_max_lane_count(const struct intel_output *o);

/* The repeaters are left in transparent mode (or there are none): the
 * sink's own wake timeout applies, not a repeater's. */
bool intel_dp_lttpr_transparent_mode(const struct intel_output *o);

#endif
