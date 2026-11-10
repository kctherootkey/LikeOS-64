/*
 * likeos-smartmontools.h - kernel interface types the OS backend needs.
 *
 * smartmontools' SG_IO backend (and its NVMe ioctl header) takes the
 * fixed-width __u8 .. __u64 types and the version 4 SCSI generic header
 * (struct sg_io_v4, used by the block SCSI generic nodes) from kernel
 * headers.  The SG_IO requests themselves and the version 3 header are in
 * the LikeOS C library (<scsi/sg.h>); patch 0001 includes this file for the
 * rest.
 *
 * The layout of struct sg_io_v4 is the established one.  The LikeOS kernel
 * takes SG_IO with the version 3 header (interface_id 'S') on /dev/sdX and
 * /dev/srX, which is what smartmontools tries first and then keeps using; a
 * version 4 header ('Q') is only sent if that first attempt fails, and to
 * the RAID controller nodes (/dev/bsg/...) LikeOS does not have.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_SMARTMONTOOLS_H
#define LIKEOS_SMARTMONTOOLS_H

#include <stdint.h>

typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;

#define BSG_PROTOCOL_SCSI		0
#define BSG_SUB_PROTOCOL_SCSI_CMD	0
#define BSG_SUB_PROTOCOL_SCSI_TMF	1
#define BSG_SUB_PROTOCOL_SCSI_TRANSPORT	2

#define BSG_FLAG_Q_AT_TAIL	0x10
#define BSG_FLAG_Q_AT_HEAD	0x20

/* Fields marked in are read by the kernel, out are written back by it. */
struct sg_io_v4 {
	__s32 guard;			/* in: 'Q' */
	__u32 protocol;			/* in: BSG_PROTOCOL_* */
	__u32 subprotocol;		/* in: BSG_SUB_PROTOCOL_* */

	__u32 request_len;		/* in: length of the command block */
	__u64 request;			/* in: address of the command block */
	__u64 request_tag;		/* in */
	__u32 request_attr;		/* in */
	__u32 request_priority;		/* in */
	__u32 request_extra;		/* in: unused */
	__u32 max_response_len;		/* in: size of the sense buffer */
	__u64 response;			/* in: address of the sense buffer */

	__u32 dout_iovec_count;		/* in: 0 = dout_xferp is one buffer */
	__u32 dout_xfer_len;		/* in: bytes to the device */
	__u32 din_iovec_count;		/* in: 0 = din_xferp is one buffer */
	__u32 din_xfer_len;		/* in: bytes from the device */
	__u64 dout_xferp;		/* in: data to the device */
	__u64 din_xferp;		/* in: buffer for data from the device */

	__u32 timeout;			/* in: milliseconds */
	__u32 flags;			/* in: BSG_FLAG_* */
	__u64 usr_ptr;			/* in, out: passed through */
	__u32 spare_in;

	__u32 driver_status;		/* out */
	__u32 transport_status;		/* out */
	__u32 device_status;		/* out: SCSI status byte */
	__u32 retry_delay;		/* out */
	__u32 info;			/* out: SG_INFO_* */
	__u32 duration;			/* out: milliseconds */
	__u32 response_len;		/* out: sense bytes written */
	__s32 din_resid;		/* out: din_xfer_len - bytes read */
	__s32 dout_resid;		/* out: dout_xfer_len - bytes written */
	__u64 generated_tag;		/* out */
	__u32 spare_out;

	__u32 padding;
};

/* 160 bytes is the size of the established layout. */
typedef char likeos_sg_io_v4_size_check[sizeof(struct sg_io_v4) == 160 ? 1 : -1];

#endif /* LIKEOS_SMARTMONTOOLS_H */
