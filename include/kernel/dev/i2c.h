// LikeOS -- a minimal I2C transfer interface.
//
// Display hardware carries several small I2C masters: the GMBUS controller
// of an integrated graphics device, a GPIO pair bit-banged when that
// controller is stuck, and the I2C-over-AUX tunnel a DisplayPort sink
// offers.  What reads through them -- an EDID block from a monitor, a
// DDC/CI register -- does not care which.  This is the common shape: a
// message list, one transfer entry point, no bus enumeration.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_I2C_H
#define KERNEL_DEV_I2C_H

#include <kernel/uapi/types.h>

#define I2C_M_RD 0x0001 /* read `len' bytes into `buf' */

struct i2c_msg {
	uint16_t addr; /* 7-bit slave address */
	uint16_t flags; /* I2C_M_* */
	uint16_t len;
	uint8_t *buf;
};

struct i2c_adapter {
	/* Transfer `n' messages back to back (one START per message, a
	 * repeated START between them, STOP after the last).  Returns the
	 * number of messages completed, or a negative errno. */
	int (*xfer)(struct i2c_adapter *a, struct i2c_msg *msgs, int n);
	void *priv;
	char name[24];
};

static inline int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m,
			       int n)
{
	if (!a || !a->xfer)
		return -1;
	return a->xfer(a, m, n);
}

#endif
