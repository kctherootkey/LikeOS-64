// LikeOS -- reading and writing an HDMI 2.0 sink's SCDC registers.
//
// The Status and Control Data Channel lives at I2C address 0x54 of the
// sink's DDC bus.  A driver passes the I2C adapter of that bus (the GMBUS
// pin pair of the port, say, <kernel/dev/i2c.h>): a connector here does
// not carry one of its own.  The register map is drm_scdc.h.
//
// What is written to SCDC does not survive the sink going away: a cable
// pulled, or a television switched off and on again, resets it, and the
// mode set stays as it was.  A driver that scrambles has to watch the
// hotplug line and write the sink's configuration again when it returns.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from NVIDIA's code: MIT
// Portions Copyright (c) 2015 NVIDIA Corporation. All rights reserved.

#ifndef KERNEL_DEV_GPU_DRM_SCDC_HELPER_H
#define KERNEL_DEV_GPU_DRM_SCDC_HELPER_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/gpu/drm_scdc.h>

struct i2c_adapter;

/* The read and write calls return a size type for their callers' sake,
 * but only ever 0 or a negative errno. */
typedef long ssize_t;

/* `size' bytes from / to the SCDC registers starting at `offset': 0, or a
 * negative errno (-EPROTO when the transfer was cut short, -ENOMEM). */
ssize_t drm_scdc_read(struct i2c_adapter *adapter, u8 offset, void *buffer,
		      size_t size);
ssize_t drm_scdc_write(struct i2c_adapter *adapter, u8 offset,
		       const void *buffer, size_t size);

/* One register: 0 or a negative errno. */
static inline int drm_scdc_readb(struct i2c_adapter *adapter, u8 offset,
				 u8 *value)
{
	return (int)drm_scdc_read(adapter, offset, value, sizeof(*value));
}

static inline int drm_scdc_writeb(struct i2c_adapter *adapter, u8 offset,
				  u8 value)
{
	return (int)drm_scdc_write(adapter, offset, &value, sizeof(value));
}

/* Does the sink's descrambler report that it has locked on? (false also
 * when the register could not be read) */
bool drm_scdc_get_scrambling_status(struct i2c_adapter *adapter);
/* Tell the sink that the source does (true) / does not scramble; true
 * when the write went through. */
bool drm_scdc_set_scrambling(struct i2c_adapter *adapter, bool enable);
/* The TMDS bit clock ratio: 1/40 (set, for a character rate above 340
 * MHz) or 1/10.  Waits the 1 ms the sink is given to follow before
 * returning; true when the write went through. */
bool drm_scdc_set_high_tmds_clock_ratio(struct i2c_adapter *adapter, bool set);

#endif
