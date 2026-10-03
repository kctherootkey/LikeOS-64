// LikeOS -- DisplayPort dual-mode (DP++) adaptors and LSPCON.
//
// A DP++ port can drive a passive adaptor to HDMI or DVI: the port sends
// TMDS on its lanes and the sink's DDC bus is wired through.  Behind the
// same bus, at I2C address 0x40, a type 2 adaptor has a small register
// file: an identification string, an adaptor ID, the highest TMDS clock
// it passes and a switch for its TMDS output buffers.  Type 1 adaptors
// need not answer at all, and are limited to 165 MHz.  An LSPCON (a level
// shifter and protocol converter, as some Intel platforms solder down for
// HDMI 2.0) answers like a type 2 adaptor with a DPCD flag and can be
// switched between passing TMDS through (LS) and converting DisplayPort
// to HDMI (PCON).
//
// The I2C adapter is the port's DDC bus (GMBUS) or, for an LSPCON, the
// I2C-over-AUX tunnel of its DisplayPort side (<kernel/dev/i2c.h>).  The
// device argument only names the device in the log and may be NULL.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright © 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_DP_DUAL_MODE_HELPER_H
#define KERNEL_DEV_GPU_DRM_DP_DUAL_MODE_HELPER_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/gpu_util.h>

/* Optional for type 1 DVI adaptors, mandatory for type 1 HDMI and type 2
 * adaptors. */
#define DP_DUAL_MODE_HDMI_ID 0x00 /* 00-0f */
#define  DP_DUAL_MODE_HDMI_ID_LEN 16
/* Optional for type 1 adaptors, mandatory for type 2 adaptors. */
#define DP_DUAL_MODE_ADAPTOR_ID 0x10
#define  DP_DUAL_MODE_REV_MASK 0x07
#define  DP_DUAL_MODE_REV_TYPE2 0x00
#define  DP_DUAL_MODE_TYPE_MASK 0xf0
#define  DP_DUAL_MODE_TYPE_TYPE2 0xa0
/* This field is marked reserved in the dual mode spec, used in LSPCON */
#define  DP_DUAL_MODE_TYPE_HAS_DPCD 0x08
#define DP_DUAL_MODE_IEEE_OUI 0x11 /* 11-13*/
#define  DP_DUAL_IEEE_OUI_LEN 3
#define DP_DUAL_DEVICE_ID 0x14 /* 14-19 */
#define  DP_DUAL_DEVICE_ID_LEN 6
#define DP_DUAL_MODE_HARDWARE_REV 0x1a
#define DP_DUAL_MODE_FIRMWARE_MAJOR_REV 0x1b
#define DP_DUAL_MODE_FIRMWARE_MINOR_REV 0x1c
#define DP_DUAL_MODE_MAX_TMDS_CLOCK 0x1d
#define DP_DUAL_MODE_I2C_SPEED_CAP 0x1e
#define DP_DUAL_MODE_TMDS_OEN 0x20
#define  DP_DUAL_MODE_TMDS_DISABLE 0x01
#define DP_DUAL_MODE_HDMI_PIN_CTRL 0x21
#define  DP_DUAL_MODE_CEC_ENABLE 0x01
#define DP_DUAL_MODE_I2C_SPEED_CTRL 0x22

/* LSPCON specific registers, defined by MCA */
#define DP_DUAL_MODE_LSPCON_MODE_CHANGE 0x40
#define DP_DUAL_MODE_LSPCON_CURRENT_MODE 0x41
#define  DP_DUAL_MODE_LSPCON_MODE_PCON 0x1

struct drm_device;
struct i2c_adapter;

/* The read and write calls return 0 or a negative errno. */
typedef long ssize_t;

/* `size' bytes from / to the adaptor registers starting at `offset': 0,
 * or a negative errno (-EPROTO for a transfer cut short, -ENOMEM). */
ssize_t drm_dp_dual_mode_read(struct i2c_adapter *adapter,
			      u8 offset, void *buffer, size_t size);
ssize_t drm_dp_dual_mode_write(struct i2c_adapter *adapter,
			       u8 offset, const void *buffer, size_t size);

enum drm_lspcon_mode {
	DRM_LSPCON_MODE_INVALID,
	DRM_LSPCON_MODE_LS, /* level shifter: TMDS passed through */
	DRM_LSPCON_MODE_PCON, /* protocol converter: DisplayPort in */
};

/* The order matters: the code compares against TYPE2_DVI. */
enum drm_dp_dual_mode_type {
	DRM_DP_DUAL_MODE_NONE,
	DRM_DP_DUAL_MODE_UNKNOWN,
	DRM_DP_DUAL_MODE_TYPE1_DVI,
	DRM_DP_DUAL_MODE_TYPE1_HDMI,
	DRM_DP_DUAL_MODE_TYPE2_DVI,
	DRM_DP_DUAL_MODE_TYPE2_HDMI,
	DRM_DP_DUAL_MODE_LSPCON,
};

/* What sits behind the port.  UNKNOWN when the adaptor did not answer:
 * a native HDMI port and a type 1 DVI adaptor look alike, and only the
 * driver (the VBT, a configuration pin) can tell them apart. */
enum drm_dp_dual_mode_type
drm_dp_dual_mode_detect(const struct drm_device *dev, struct i2c_adapter *adapter);
/* The highest TMDS clock the adaptor passes, kHz; 0 for NONE (no limit). */
int drm_dp_dual_mode_max_tmds_clock(const struct drm_device *dev, enum drm_dp_dual_mode_type type,
				    struct i2c_adapter *adapter);
/* The state of the adaptor's TMDS output buffers (always on for type 1). */
int drm_dp_dual_mode_get_tmds_output(const struct drm_device *dev, enum drm_dp_dual_mode_type type,
				     struct i2c_adapter *adapter, bool *enabled);
/* Switch them (nothing to do for type 1); written and read back up to
 * three times, -EIO when the adaptor does not take the value. */
int drm_dp_dual_mode_set_tmds_output(const struct drm_device *dev, enum drm_dp_dual_mode_type type,
				     struct i2c_adapter *adapter, bool enable);
const char *drm_dp_get_dual_mode_type_name(enum drm_dp_dual_mode_type type);

/* The LSPCON's current mode: 0, -EINVAL (NULL mode), -EFAULT when it does
 * not answer within six reads. */
int drm_lspcon_get_mode(const struct drm_device *dev, struct i2c_adapter *adapter,
			enum drm_lspcon_mode *current_mode);
/* Switch the LSPCON's mode and wait for it to report the new one, up to
 * time_out milliseconds: 0, a negative errno, -ETIMEDOUT. */
int drm_lspcon_set_mode(const struct drm_device *dev, struct i2c_adapter *adapter,
			enum drm_lspcon_mode reqd_mode, int time_out);

#endif
