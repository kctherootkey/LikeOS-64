// LikeOS -- DisplayPort AUX channel of the Intel display: the per-output
// channel and its DPCD access.
//
// The DPCD register numbers are the DisplayPort specification's
// (drm_dp.h).  The channel (intel_dp_aux.c) runs one AUX transaction at a
// time on the port's AUX registers; with I915_FEAT_DP_HELPERS that is the
// transfer hook of a struct drm_dp_aux, and the DisplayPort helper library
// above it does the retries, the DPCD chunking and the I2C-over-AUX
// tunnel the EDID is read through.  Without it the driver's own request
// and I2C code runs on the same transaction routine.  intel_display.h
// includes this header, so every display file sees it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020-2021 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_DP_AUX_H
#define KERNEL_DEV_GPU_I915_INTEL_DP_AUX_H

#include <kernel/dev/i2c.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/drm_dp_helper.h>

struct i915_device;

struct intel_dp_aux {
	struct i915_device *i915;
	int port; /* DDI port, which names the AUX channel on Skylake */
	uint32_t ctl_reg, data_reg;
	/* I2C over AUX, for the EDID.  With the helpers it passes every
	 * transfer on to dp.ddc, the helper library's tunnel. */
	struct i2c_adapter i2c;
	uint32_t errors;
	/* The channel runs through the Thunderbolt controller (a Type-C
	 * port in Thunderbolt mode): set by intel_tc_connect(). */
	int tbt_io;
	/* The helper library's view of the channel (I915_FEAT_DP_HELPERS):
	 * its transfer hook is intel_dp_aux.c's, its lock serialises this
	 * output's transactions. */
	struct drm_dp_aux dp;
	/* Two outputs can sit on one hardware channel (DisplayPort and HDMI
	 * on one DDI, Skylake's DDI E on AUX A, a VBT naming another port's
	 * channel): this lock is the hardware's, shared by all of them. */
	mm_rwsem_t *hw_lock;
};

void intel_dp_aux_init(struct i915_device *i915, struct intel_dp_aux *aux,
		       int port);
/* Native AUX: read/write `len' bytes at DPCD `addr'.  Returns the byte
 * count or a negative errno. */
int intel_dp_aux_native_read(struct intel_dp_aux *aux, uint32_t addr,
			     uint8_t *buf, unsigned len);
int intel_dp_aux_native_write(struct intel_dp_aux *aux, uint32_t addr,
			      const uint8_t *buf, unsigned len);
static inline int intel_dp_dpcd_read8(struct intel_dp_aux *aux, uint32_t addr,
				      uint8_t *v)
{
	return intel_dp_aux_native_read(aux, addr, v, 1) == 1 ? 0 : -1;
}
static inline int intel_dp_dpcd_write8(struct intel_dp_aux *aux, uint32_t addr,
				       uint8_t v)
{
	return intel_dp_aux_native_write(aux, addr, &v, 1) == 1 ? 0 : -1;
}
/* The I2C adapter the sink's EDID is read through. */
static inline struct i2c_adapter *intel_dp_aux_ddc(struct intel_dp_aux *aux)
{
	return &aux->i2c;
}

#endif
