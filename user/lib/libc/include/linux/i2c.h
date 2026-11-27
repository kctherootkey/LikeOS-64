/*
 * <linux/i2c.h> -- the I2C message and SMBus data interface
 *
 * What programs that drive I2C buses from user space (i2c-tools,
 * libi2c, firmware updaters, sensor daemons) hand to the I2C character
 * devices /dev/i2c-N (see <linux/i2c-dev.h>):
 *
 *   struct i2c_msg        one message of a combined transfer (I2C_RDWR):
 *                         target address, I2C_M_* flags, length, buffer.
 *                         The messages of one transfer are joined by
 *                         repeated STARTs and end with a single STOP.
 *   I2C_FUNC_*            what an adapter can do (I2C_FUNCS answers a
 *                         mask of these).
 *   union i2c_smbus_data  the payload of an SMBus transaction (I2C_SMBUS):
 *                         byte, word, or block with block[0] = length.
 *   I2C_SMBUS_*           SMBus direction and transaction types.
 *
 * The layout and the numeric values are fixed by the established
 * interface and must never change.
 *
 * Copyright (C) 2026 The LikeOS Project
 * Portions Copyright (C) 1995-2000 Simon G. Vogl
 */

#ifndef _LIKEOS_UAPI_LINUX_I2C_H
#define _LIKEOS_UAPI_LINUX_I2C_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One message.  `addr' is the 7-bit (or, with I2C_M_TEN, 10-bit) target
 * address without the direction bit; `len' bytes are written from or read
 * into `buf'. */
struct i2c_msg {
	__u16 addr;
	__u16 flags;
#define I2C_M_RD 0x0001 /* read from the target (always 0x0001) */
#define I2C_M_TEN 0x0010 /* 10-bit address; needs I2C_FUNC_10BIT_ADDR */
#define I2C_M_DMA_SAFE 0x0200 /* kernel use only */
#define I2C_M_RECV_LEN 0x0400 /* first received byte is the length;
				 * needs I2C_FUNC_SMBUS_READ_BLOCK_DATA */
#define I2C_M_NO_RD_ACK 0x0800 /* the next flags need */
#define I2C_M_IGNORE_NAK 0x1000 /* I2C_FUNC_PROTOCOL_MANGLING */
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NOSTART 0x4000 /* needs I2C_FUNC_NOSTART */
#define I2C_M_STOP 0x8000 /* STOP after this message;
			   * needs I2C_FUNC_PROTOCOL_MANGLING */
	__u16 len;
	__u8 *buf;
};

/* Adapter functionality (I2C_FUNCS). */
#define I2C_FUNC_I2C 0x00000001
#define I2C_FUNC_10BIT_ADDR 0x00000002
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004
#define I2C_FUNC_SMBUS_PEC 0x00000008
#define I2C_FUNC_NOSTART 0x00000010
#define I2C_FUNC_SLAVE 0x00000020
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000
#define I2C_FUNC_SMBUS_QUICK 0x00010000
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000
#define I2C_FUNC_SMBUS_HOST_NOTIFY 0x10000000

#define I2C_FUNC_SMBUS_BYTE \
	(I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA \
	(I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA \
	(I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA \
	(I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK \
	(I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)
#define I2C_FUNC_SMBUS_EMUL                                               \
	(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |                     \
	 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |            \
	 I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA |     \
	 I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)
#define I2C_FUNC_SMBUS_EMUL_ALL                                           \
	(I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA |           \
	 I2C_FUNC_SMBUS_BLOCK_PROC_CALL)

/* SMBus payload.  block[0] is the length of a block transfer; one byte
 * more than the SMBus maximum is reserved for compatibility. */
#define I2C_SMBUS_BLOCK_MAX 32
union i2c_smbus_data {
	__u8 byte;
	__u16 word;
	__u8 block[I2C_SMBUS_BLOCK_MAX + 2];
};

/* SMBus direction. */
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0

/* SMBus transaction types (I2C_SMBUS `size'). */
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7
#define I2C_SMBUS_I2C_BLOCK_DATA 8

#ifdef __cplusplus
}
#endif

#endif /* _LIKEOS_UAPI_LINUX_I2C_H */
