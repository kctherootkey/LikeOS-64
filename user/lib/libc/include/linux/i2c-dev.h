/*
 * <linux/i2c-dev.h> -- the I2C bus character devices /dev/i2c-N
 *
 * One node per I2C adapter (bus N, character major 89, minor N; root
 * only).  An open file carries a target address and flags of its own:
 *
 *   I2C_SLAVE        select the 7-bit (or 10-bit with I2C_TENBIT) target
 *                    address for read()/write() and I2C_SMBUS; -EBUSY when
 *                    a kernel driver is bound to a device at that address
 *   I2C_SLAVE_FORCE  the same without the busy check
 *   I2C_TENBIT       nonzero: 10-bit addressing
 *   I2C_PEC          nonzero: SMBus transactions with packet error checking
 *   I2C_FUNCS        the adapter's I2C_FUNC_* mask (into an unsigned long)
 *   I2C_RDWR         a combined transfer of up to I2C_RDWR_IOCTL_MAX_MSGS
 *                    messages (struct i2c_rdwr_ioctl_data), one STOP at
 *                    the end; each message at most 8192 bytes.  Returns
 *                    the number of messages transferred.
 *   I2C_SMBUS        one SMBus transaction (struct i2c_smbus_ioctl_data)
 *   I2C_RETRIES      times a transfer is retried after lost arbitration
 *   I2C_TIMEOUT      transfer timeout, in units of 10 ms
 *
 * read()/write() transfer plain bytes from/to the selected address (at
 * most 8192 per call).
 *
 * The request numbers and structure layouts are fixed by the established
 * interface and must never change.
 *
 * Copyright (C) 2026 The LikeOS Project
 * Portions Copyright (C) 1995-97 Simon G. Vogl, 1998-99 Frodo Looijaard
 */

#ifndef _LIKEOS_UAPI_LINUX_I2C_DEV_H
#define _LIKEOS_UAPI_LINUX_I2C_DEV_H

#include <linux/types.h>
#include <linux/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The argument is an unsigned long, except: I2C_FUNCS (pointer to an
 * unsigned long), I2C_RDWR (pointer to struct i2c_rdwr_ioctl_data) and
 * I2C_SMBUS (pointer to struct i2c_smbus_ioctl_data). */
#define I2C_RETRIES 0x0701
#define I2C_TIMEOUT 0x0702 /* in units of 10 ms */
#define I2C_SLAVE 0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_TENBIT 0x0704
#define I2C_FUNCS 0x0705
#define I2C_RDWR 0x0707
#define I2C_PEC 0x0708
#define I2C_SMBUS 0x0720

/* I2C_SMBUS argument. */
struct i2c_smbus_ioctl_data {
	__u8 read_write; /* I2C_SMBUS_READ / I2C_SMBUS_WRITE */
	__u8 command;
	__u32 size; /* I2C_SMBUS_QUICK ... I2C_SMBUS_I2C_BLOCK_DATA */
	union i2c_smbus_data *data;
};

/* I2C_RDWR argument. */
struct i2c_rdwr_ioctl_data {
	struct i2c_msg *msgs;
	__u32 nmsgs;
};

#define I2C_RDWR_IOCTL_MAX_MSGS 42
/* The historical misspelling, kept for compatibility. */
#define I2C_RDRW_IOCTL_MAX_MSGS I2C_RDWR_IOCTL_MAX_MSGS

#ifdef __cplusplus
}
#endif

#endif /* _LIKEOS_UAPI_LINUX_I2C_DEV_H */
