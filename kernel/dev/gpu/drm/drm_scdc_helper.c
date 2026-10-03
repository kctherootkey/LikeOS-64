// LikeOS -- the HDMI 2.0 Status and Control Data Channel: register access
// and the scrambling / TMDS clock ratio switches.
//
// SCDC is a point-to-point protocol introduced by HDMI 2.0 for the source
// and the sink to exchange status and configuration.  It runs over the
// same I2C bus as the EDID, at slave address 0x54, as plain register
// reads and writes: a write of the register offset followed by a read (a
// repeated START between them), or a single write of the offset and the
// data.  Above a TMDS character rate of 340 MHz the source must scramble
// and switch to the 1/40 bit clock ratio, and the sink must be told both
// through the TMDS configuration register.
//
// The SCDC state is lost whenever the sink goes away (see
// drm_scdc_helper.h); the caller restores it on hotplug.
//
// A write allocates its message buffer, and the clock ratio switch busy-
// waits a millisecond: for the mode set path, not for interrupt context.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from NVIDIA's code: MIT
// Portions Copyright (c) 2015 NVIDIA Corporation. All rights reserved.

#include <kernel/dev/gpu/drm_scdc_helper.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/i2c.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* Failures to talk to the sink are expected (a DVI sink, a sink that is
 * still waking up) and are the caller's to judge: logged only with this
 * set. */
#ifndef DRM_SCDC_DEBUG
#define DRM_SCDC_DEBUG 0
#endif

#define scdc_dbg(fmt, ...)                                       \
	do {                                                     \
		if (DRM_SCDC_DEBUG)                              \
			kprintf("drm: scdc: " fmt, ##__VA_ARGS__); \
	} while (0)

#define SCDC_I2C_SLAVE_ADDRESS 0x54

ssize_t drm_scdc_read(struct i2c_adapter *adapter, u8 offset, void *buffer,
		      size_t size)
{
	int ret;
	struct i2c_msg msgs[2] = {
		{
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = 0,
			.len = 1,
			.buf = &offset,
		}, {
			.addr = SCDC_I2C_SLAVE_ADDRESS,
			.flags = I2C_M_RD,
			.len = (uint16_t)size,
			.buf = buffer,
		}
	};

	/* An i2c_msg length is 16 bits wide; the register file is 256. */
	if (size > 0xffff)
		return -EINVAL;

	ret = i2c_transfer(adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != (int)ARRAY_SIZE(msgs))
		return -EPROTO;

	return 0;
}

ssize_t drm_scdc_write(struct i2c_adapter *adapter, u8 offset,
		       const void *buffer, size_t size)
{
	struct i2c_msg msg = {
		.addr = SCDC_I2C_SLAVE_ADDRESS,
		.flags = 0,
		.len = (uint16_t)(1 + size),
		.buf = NULL,
	};
	const u8 *src = buffer;
	u8 *data;
	size_t i;
	int err;

	if (size >= 0xffff)
		return -EINVAL;

	/* The offset and the data go out in one message. */
	data = kalloc(1 + size);
	if (!data)
		return -ENOMEM;

	msg.buf = data;

	data[0] = offset;
	for (i = 0; i < size; i++)
		data[1 + i] = src[i];

	err = i2c_transfer(adapter, &msg, 1);

	kfree(data);

	if (err < 0)
		return err;
	if (err != 1)
		return -EPROTO;

	return 0;
}

bool drm_scdc_get_scrambling_status(struct i2c_adapter *adapter)
{
	u8 status;
	int ret;

	ret = drm_scdc_readb(adapter, SCDC_SCRAMBLER_STATUS, &status);
	if (ret < 0) {
		scdc_dbg("failed to read scrambling status: %d\n", ret);
		return false;
	}

	return (status & SCDC_SCRAMBLING_STATUS) != 0;
}

bool drm_scdc_set_scrambling(struct i2c_adapter *adapter, bool enable)
{
	u8 config;
	int ret;

	/* Read-modify-write: the clock ratio bit shares the register. */
	ret = drm_scdc_readb(adapter, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		scdc_dbg("failed to read TMDS config: %d\n", ret);
		return false;
	}

	if (enable)
		config |= SCDC_SCRAMBLING_ENABLE;
	else
		config &= (u8)~SCDC_SCRAMBLING_ENABLE;

	ret = drm_scdc_writeb(adapter, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		scdc_dbg("failed to %s scrambling: %d\n",
			 enable ? "enable" : "disable", ret);
		return false;
	}

	return true;
}

/* TMDS clock ratio:
 *	TMDS character = a 10-bit TMDS encoded value
 *	TMDS character rate = the rate characters are sent at (Mcsc)
 *	TMDS bit rate = 10x the TMDS character rate
 *
 *	TMDS clock rate for a character rate up to 340 MHz = 1x the
 *	character rate = 1/10 of the bit rate;
 *	above 340 MHz = 0.25x the character rate = 1/40 of the bit rate.
 *
 * `set' selects 1/40, clear 1/10. */
bool drm_scdc_set_high_tmds_clock_ratio(struct i2c_adapter *adapter, bool set)
{
	u8 config;
	int ret;

	ret = drm_scdc_readb(adapter, SCDC_TMDS_CONFIG, &config);
	if (ret < 0) {
		scdc_dbg("failed to read TMDS config: %d\n", ret);
		return false;
	}

	if (set)
		config |= SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;
	else
		config &= (u8)~SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	ret = drm_scdc_writeb(adapter, SCDC_TMDS_CONFIG, config);
	if (ret < 0) {
		scdc_dbg("failed to set TMDS clock ratio: %d\n", ret);
		return false;
	}

	/* The source waits at least 1 ms and at most 100 ms after writing
	 * the clock ratio before it changes the clock.  A short busy wait:
	 * callers are on the mode set path, where a sleep could stretch to
	 * a whole scheduler tick or end early on a signal. */
	lapic_delay_us(1000);
	return true;
}
