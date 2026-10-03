// LikeOS -- DisplayPort dual-mode (DP++) adaptors and LSPCON: identify the
// adaptor behind a port, read its TMDS clock limit, switch its TMDS
// output buffers, and switch an LSPCON between LS and PCON mode.
//
// Type 1 adaptors: adaptor registers (if any) and the sink's DDC bus are
// reached over I2C.  Type 2 adaptors: over I2C or I2C-over-AUX, whichever
// the source implements.  Both are at I2C address 0x40 (the sink's EDID
// stays at 0x50 on the same bus).
//
// The register reads always start at offset 0 and discard the bytes
// before the one wanted: not every adaptor implements sub-addressing, and
// reading from the start gives the right bytes whether it does or not.
//
// Allocates (a buffer per transfer at a non-zero offset) and busy-waits
// between LSPCON polls: for the probe and mode set paths, not for
// interrupt context.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright © 2016 Intel Corporation

#include <kernel/dev/gpu/drm_dp_dual_mode_helper.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/i2c.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* A port without an adaptor answers nothing, which is normal: the
 * failures and the identification are logged only with this set. */
#ifndef DRM_DP_DUAL_MODE_DEBUG
#define DRM_DP_DUAL_MODE_DEBUG 0
#endif

#define dual_mode_dbg(fmt, ...)                                    \
	do {                                                       \
		if (DRM_DP_DUAL_MODE_DEBUG)                        \
			kprintf("drm: dp dual mode: " fmt, ##__VA_ARGS__); \
	} while (0)

#define dual_mode_err(fmt, ...) kprintf("drm: dp dual mode: " fmt, ##__VA_ARGS__)

#define DP_DUAL_MODE_SLAVE_ADDRESS 0x40

static void dual_mode_memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
}

ssize_t drm_dp_dual_mode_read(struct i2c_adapter *adapter,
			      u8 offset, void *buffer, size_t size)
{
	u8 zero = 0;
	u8 *tmpbuf = NULL;
	/* Read from the start and discard the bytes before `offset' (see
	 * the top of the file). */
	struct i2c_msg msgs[] = {
		{
			.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
			.flags = 0,
			.len = 1,
			.buf = &zero,
		},
		{
			.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
			.flags = I2C_M_RD,
			.len = (uint16_t)(size + offset),
			.buf = buffer,
		},
	};
	int ret;

	if (size + offset > 0xffff)
		return -EINVAL;

	if (offset) {
		tmpbuf = kalloc(size + offset);
		if (!tmpbuf)
			return -ENOMEM;

		msgs[1].buf = tmpbuf;
	}

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (tmpbuf)
		dual_mode_memcpy(buffer, tmpbuf + offset, size);

	kfree(tmpbuf);

	if (ret < 0)
		return ret;
	if (ret != (int)ARRAY_SIZE(msgs))
		return -EPROTO;

	return 0;
}

ssize_t drm_dp_dual_mode_write(struct i2c_adapter *adapter,
			       u8 offset, const void *buffer, size_t size)
{
	struct i2c_msg msg = {
		.addr = DP_DUAL_MODE_SLAVE_ADDRESS,
		.flags = 0,
		.len = (uint16_t)(1 + size),
		.buf = NULL,
	};
	u8 *data;
	int ret;

	if (size >= 0xffff)
		return -EINVAL;

	data = kalloc(msg.len);
	if (!data)
		return -ENOMEM;

	msg.buf = data;

	data[0] = offset;
	dual_mode_memcpy(data + 1, buffer, size);

	ret = i2c_transfer(adapter, &msg, 1);

	kfree(data);

	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EPROTO;

	return 0;
}

static bool is_hdmi_adaptor(const char hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN])
{
	static const char dp_dual_mode_hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN + 1] =
		"DP-HDMI ADAPTOR\x04";
	int i;

	for (i = 0; i < DP_DUAL_MODE_HDMI_ID_LEN; i++)
		if (hdmi_id[i] != dp_dual_mode_hdmi_id[i])
			return false;
	return true;
}

static bool is_type1_adaptor(uint8_t adaptor_id)
{
	return adaptor_id == 0 || adaptor_id == 0xff;
}

static bool is_type2_adaptor(uint8_t adaptor_id)
{
	return adaptor_id == (DP_DUAL_MODE_TYPE_TYPE2 |
			      DP_DUAL_MODE_REV_TYPE2);
}

static bool is_lspcon_adaptor(const char hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN],
			      const uint8_t adaptor_id)
{
	return is_hdmi_adaptor(hdmi_id) &&
		(adaptor_id == (DP_DUAL_MODE_TYPE_TYPE2 |
		 DP_DUAL_MODE_TYPE_HAS_DPCD));
}

enum drm_dp_dual_mode_type drm_dp_dual_mode_detect(const struct drm_device *dev,
						   struct i2c_adapter *adapter)
{
	char hdmi_id[DP_DUAL_MODE_HDMI_ID_LEN] = { 0 };
	uint8_t adaptor_id = 0x00;
	ssize_t ret;

	(void)dev;

	/* Is an adaptor there?  Read its HDMI ID registers.
	 *
	 * Type 1 DVI adaptors need not implement any registers, so a NAK
	 * here may or may not mean one is present.  Telling the two apart
	 * takes something outside the adaptor (the state of the CONFIG1
	 * pin, or the driver knowing whether the port is DP++ or native
	 * HDMI), which only the driver has. */
	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_HDMI_ID,
				    hdmi_id, sizeof(hdmi_id));
	if (DRM_DP_DUAL_MODE_DEBUG) {
		/* the ID is not terminated (and ends in 0x04) */
		char id[DP_DUAL_MODE_HDMI_ID_LEN + 1] = { 0 };

		if (!ret)
			dual_mode_memcpy(id, hdmi_id, sizeof(hdmi_id) - 1);
		dual_mode_dbg("HDMI ID: %s (err %ld)\n", id, (long)ret);
	}
	if (ret)
		return DRM_DP_DUAL_MODE_UNKNOWN;

	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_ADAPTOR_ID,
				    &adaptor_id, sizeof(adaptor_id));
	dual_mode_dbg("adaptor ID: %02x (err %ld)\n", adaptor_id, (long)ret);
	if (ret == 0) {
		if (is_lspcon_adaptor(hdmi_id, adaptor_id))
			return DRM_DP_DUAL_MODE_LSPCON;
		if (is_type2_adaptor(adaptor_id)) {
			if (is_hdmi_adaptor(hdmi_id))
				return DRM_DP_DUAL_MODE_TYPE2_HDMI;
			else
				return DRM_DP_DUAL_MODE_TYPE2_DVI;
		}
		/* Not a proper type 1 ID: still taken as type 1, but say
		 * that the type may be wrong. */
		if (!is_type1_adaptor(adaptor_id))
			dual_mode_err("unexpected adaptor ID %02x\n", adaptor_id);
	}

	if (is_hdmi_adaptor(hdmi_id))
		return DRM_DP_DUAL_MODE_TYPE1_HDMI;
	else
		return DRM_DP_DUAL_MODE_TYPE1_DVI;
}

int drm_dp_dual_mode_max_tmds_clock(const struct drm_device *dev, enum drm_dp_dual_mode_type type,
				    struct i2c_adapter *adapter)
{
	uint8_t max_tmds_clock;
	ssize_t ret;

	(void)dev;

	/* native HDMI so no limit */
	if (type == DRM_DP_DUAL_MODE_NONE)
		return 0;

	/* Type 1 adaptors are limited to 165 MHz (and their registers are
	 * not trusted, see drm_dp_dual_mode_detect); type 2 adaptors say
	 * their limit, in units of 2.5 MHz. */
	if (type < DRM_DP_DUAL_MODE_TYPE2_DVI)
		return 165000;

	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_MAX_TMDS_CLOCK,
				    &max_tmds_clock, sizeof(max_tmds_clock));
	if (ret || max_tmds_clock == 0x00 || max_tmds_clock == 0xff) {
		dual_mode_dbg("failed to query max TMDS clock\n");
		return 165000;
	}

	return max_tmds_clock * 5000 / 2;
}

int drm_dp_dual_mode_get_tmds_output(const struct drm_device *dev,
				     enum drm_dp_dual_mode_type type, struct i2c_adapter *adapter,
				     bool *enabled)
{
	uint8_t tmds_oen;
	ssize_t ret;

	(void)dev;

	/* Type 1 registers are not read (see above): the buffers are
	 * taken to be always on. */
	if (type < DRM_DP_DUAL_MODE_TYPE2_DVI) {
		*enabled = true;
		return 0;
	}

	ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_TMDS_OEN,
				    &tmds_oen, sizeof(tmds_oen));
	if (ret) {
		dual_mode_dbg("failed to query state of TMDS output buffers\n");
		return (int)ret;
	}

	*enabled = !(tmds_oen & DP_DUAL_MODE_TMDS_DISABLE);

	return 0;
}

int drm_dp_dual_mode_set_tmds_output(const struct drm_device *dev, enum drm_dp_dual_mode_type type,
				     struct i2c_adapter *adapter, bool enable)
{
	uint8_t tmds_oen = enable ? 0 : DP_DUAL_MODE_TMDS_DISABLE;
	ssize_t ret;
	int retry;

	(void)dev;

	/* Type 1 adaptors take no register writes. */
	if (type < DRM_DP_DUAL_MODE_TYPE2_DVI)
		return 0;

	/* An LSPCON in a low-power state may ignore the first write: read
	 * back and verify the written value a few times. */
	for (retry = 0; retry < 3; retry++) {
		uint8_t tmp;

		ret = drm_dp_dual_mode_write(adapter, DP_DUAL_MODE_TMDS_OEN,
					     &tmds_oen, sizeof(tmds_oen));
		if (ret) {
			dual_mode_dbg("failed to %s TMDS output buffers (%d attempts)\n",
				      enable ? "enable" : "disable", retry + 1);
			return (int)ret;
		}

		ret = drm_dp_dual_mode_read(adapter, DP_DUAL_MODE_TMDS_OEN,
					    &tmp, sizeof(tmp));
		if (ret) {
			dual_mode_dbg("I2C read failed during TMDS output buffer %s (%d attempts)\n",
				      enable ? "enabling" : "disabling", retry + 1);
			return (int)ret;
		}

		if (tmp == tmds_oen)
			return 0;
	}

	dual_mode_dbg("I2C write value mismatch during TMDS output buffer %s\n",
		      enable ? "enabling" : "disabling");

	return -EIO;
}

const char *drm_dp_get_dual_mode_type_name(enum drm_dp_dual_mode_type type)
{
	switch (type) {
	case DRM_DP_DUAL_MODE_NONE:
		return "none";
	case DRM_DP_DUAL_MODE_TYPE1_DVI:
		return "type 1 DVI";
	case DRM_DP_DUAL_MODE_TYPE1_HDMI:
		return "type 1 HDMI";
	case DRM_DP_DUAL_MODE_TYPE2_DVI:
		return "type 2 DVI";
	case DRM_DP_DUAL_MODE_TYPE2_HDMI:
		return "type 2 HDMI";
	case DRM_DP_DUAL_MODE_LSPCON:
		return "lspcon";
	default:
		WARN_ON(type != DRM_DP_DUAL_MODE_UNKNOWN);
		return "unknown";
	}
}

int drm_lspcon_get_mode(const struct drm_device *dev, struct i2c_adapter *adapter,
			enum drm_lspcon_mode *mode)
{
	u8 data = 0;
	int ret = 0;
	int retry;

	(void)dev;

	if (!mode) {
		dual_mode_err("lspcon: NULL input\n");
		return -EINVAL;
	}

	/* Read the status (I2C over AUX); the LSPCON may need a moment. */
	for (retry = 0; retry < 6; retry++) {
		if (retry)
			lapic_delay_us(500);

		ret = (int)drm_dp_dual_mode_read(adapter,
						 DP_DUAL_MODE_LSPCON_CURRENT_MODE,
						 &data, sizeof(data));
		if (!ret)
			break;
	}

	if (ret < 0) {
		dual_mode_dbg("LSPCON read(0x80, 0x41) failed\n");
		return -EFAULT;
	}

	if (data & DP_DUAL_MODE_LSPCON_MODE_PCON)
		*mode = DRM_LSPCON_MODE_PCON;
	else
		*mode = DRM_LSPCON_MODE_LS;
	return 0;
}

int drm_lspcon_set_mode(const struct drm_device *dev, struct i2c_adapter *adapter,
			enum drm_lspcon_mode mode, int time_out)
{
	u8 data = 0;
	int ret;
	enum drm_lspcon_mode current_mode;

	if (mode == DRM_LSPCON_MODE_PCON)
		data = DP_DUAL_MODE_LSPCON_MODE_PCON;

	/* Change mode */
	ret = (int)drm_dp_dual_mode_write(adapter, DP_DUAL_MODE_LSPCON_MODE_CHANGE,
					  &data, sizeof(data));
	if (ret < 0) {
		dual_mode_err("LSPCON mode change failed\n");
		return ret;
	}

	/* Confirm the change by reading the status bit; it can take a while,
	 * so poll every 10 ms until the time is up.  (> 0 rather than != 0:
	 * a time-out that is not a multiple of 10 must still end.) */
	do {
		ret = drm_lspcon_get_mode(dev, adapter, &current_mode);
		if (ret) {
			dual_mode_err("can't confirm LSPCON mode change\n");
			return ret;
		} else {
			if (current_mode != mode) {
				lapic_delay_ms(10);
				time_out -= 10;
			} else {
				dual_mode_dbg("LSPCON mode changed to %s\n",
					      mode == DRM_LSPCON_MODE_LS ? "LS" : "PCON");
				return 0;
			}
		}
	} while (time_out > 0);

	dual_mode_err("LSPCON mode change timed out\n");
	return -ETIMEDOUT;
}
