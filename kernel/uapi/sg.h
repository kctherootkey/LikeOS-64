// LikeOS SCSI generic UAPI - SG_IO and the sg_io_hdr interface
//
// The established SCSI pass-through interface: a program fills a struct
// sg_io_hdr with a command descriptor block, a data buffer and a sense
// buffer, issues ioctl(fd, SG_IO, &hdr) on a disk or optical node, and gets
// the SCSI status, the sense data and the residual count back.  hdparm,
// smartmontools, sdparm, eject, lsscsi-style tools and udev's identity
// probes all speak it.
//
// On a SATA disk the kernel runs the command through its SCSI/ATA
// translation (kernel/dev/ata/ata_sat.c): INQUIRY, READ CAPACITY, READ/WRITE,
// SYNCHRONIZE CACHE, MODE SENSE, UNMAP and above all ATA PASS-THROUGH (12)
// and (16), which carries a raw ATA command (IDENTIFY, SMART, SECURITY ...)
// to the drive.  On an optical drive (ATAPI) the CDB goes to the drive as a
// PACKET command unchanged.
//
// Only the synchronous SG_IO request and the informational requests below
// are implemented -- there is no /dev/sgN character device, so write()/
// read()-style asynchronous sg use answers ENOTTY.  SG_IO requires
// CAP_SYS_RAWIO (root) except for a short list of harmless read-only
// commands (INQUIRY, READ CAPACITY, TEST UNIT READY, REQUEST SENSE, MODE
// SENSE, REPORT LUNS and reads) on a node opened for reading.
//
// Mirrored into the userspace libc include tree as <scsi/sg.h>; keep both
// copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_SG_H_
#define _KERNEL_UAPI_SG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#else
#include <stddef.h>
#endif

// The interface version SG_GET_VERSION_NUM reports: 3.5.36, the version
// whose sg_io_hdr layout this is.
#define SG_VERSION_NUM 30536

typedef struct sg_iovec {
	void *iov_base;
	size_t iov_len;
} sg_iovec_t;

typedef struct sg_io_hdr {
	int interface_id; // [i] 'S' for SCSI generic (required)
	int dxfer_direction; // [i] data transfer direction, SG_DXFER_*
	unsigned char cmd_len; // [i] SCSI command length (6..16)
	unsigned char mx_sb_len; // [i] max length to write to sbp
	unsigned short iovec_count; // [i] 0 implies no scatter gather
	unsigned int dxfer_len; // [i] byte count of data transfer
	void *dxferp; // [i], [*io] points to data transfer memory
		      // or scatter gather list
	unsigned char *cmdp; // [i], [*i] points to command to perform
	void *sbp; // [i], [*o] points to sense_buffer memory
	unsigned int timeout; // [i] MAX_UINT->no timeout (unit: millisec)
	unsigned int flags; // [i] 0 -> default, see SG_FLAG...
	int pack_id; // [i->o] unused internally (normally)
	void *usr_ptr; // [i->o] unused internally
	unsigned char status; // [o] scsi status (SAM_STAT_*)
	unsigned char masked_status; // [o] shifted, masked scsi status
	unsigned char msg_status; // [o] messaging level data (optional)
	unsigned char sb_len_wr; // [o] byte count actually written to sbp
	unsigned short host_status; // [o] errors from host adapter (DID_*)
	unsigned short driver_status; // [o] errors from software driver
	int resid; // [o] dxfer_len - actual_transferred
	unsigned int duration; // [o] time taken by cmd (unit: millisec)
	unsigned int info; // [o] auxiliary information, SG_INFO_*
} sg_io_hdr_t;

// sg_io_hdr.dxfer_direction
#define SG_DXFER_NONE (-1) // e.g. a SCSI Test Unit Ready command
#define SG_DXFER_TO_DEV (-2) // e.g. a SCSI WRITE command
#define SG_DXFER_FROM_DEV (-3) // e.g. a SCSI READ command
#define SG_DXFER_TO_FROM_DEV (-4) // treated like SG_DXFER_FROM_DEV with the
				  // additional property that the buffer is
				  // copied to the kernel first
#define SG_DXFER_UNKNOWN (-5) // unknown data direction

// sg_io_hdr.flags
#define SG_FLAG_DIRECT_IO 1 // default is indirect IO
#define SG_FLAG_UNUSED_LUN_INHIBIT 2 // default is overwrite lun in SCSI-2
#define SG_FLAG_MMAP_IO 4 // request memory mapped IO
#define SG_FLAG_NO_DXFER 0x10000 // no transfer of kernel buffers to/from
				 // user space (debug indirect IO)
#define SG_FLAG_Q_AT_TAIL 0x10 // default for SG_IO is queue at head
#define SG_FLAG_Q_AT_HEAD 0x20

// sg_io_hdr.info
#define SG_INFO_OK_MASK 0x1
#define SG_INFO_OK 0x0 // no sense, host nor driver "noise"
#define SG_INFO_CHECK 0x1 // something abnormal happened
#define SG_INFO_DIRECT_IO_MASK 0x6
#define SG_INFO_INDIRECT_IO 0x0 // data xfer via kernel buffers (or no xfer)
#define SG_INFO_DIRECT_IO 0x2 // direct IO requested and performed
#define SG_INFO_MIXED_IO 0x4 // part direct, part indirect IO

// sg_io_hdr.host_status
#define SG_ERR_DID_OK 0x00 // NO error
#define SG_ERR_DID_NO_CONNECT 0x01 // couldn't connect before timeout period
#define SG_ERR_DID_BUS_BUSY 0x02 // BUS stayed busy through time out period
#define SG_ERR_DID_TIME_OUT 0x03 // TIMED OUT for other reason
#define SG_ERR_DID_BAD_TARGET 0x04 // BAD target, device not responding?
#define SG_ERR_DID_ABORT 0x05 // told to abort for some other reason
#define SG_ERR_DID_PARITY 0x06 // parity error
#define SG_ERR_DID_ERROR 0x07 // internal error
#define SG_ERR_DID_RESET 0x08 // reset by somebody
#define SG_ERR_DID_BAD_INTR 0x09 // received an unexpected interrupt
#define SG_ERR_DID_PASSTHROUGH 0x0a // force command past mid-layer
#define SG_ERR_DID_SOFT_ERROR 0x0b // the low level driver wants a retry

// sg_io_hdr.driver_status
#define SG_ERR_DRIVER_OK 0x00
#define SG_ERR_DRIVER_BUSY 0x01
#define SG_ERR_DRIVER_SOFT 0x02
#define SG_ERR_DRIVER_MEDIA 0x03
#define SG_ERR_DRIVER_ERROR 0x04
#define SG_ERR_DRIVER_INVALID 0x05
#define SG_ERR_DRIVER_TIMEOUT 0x06
#define SG_ERR_DRIVER_HARD 0x07
#define SG_ERR_DRIVER_SENSE 0x08 // sense data was written to sbp

// ---- SG_GET_SCSI_ID ---------------------------------------------------------
typedef struct sg_scsi_id {
	int host_no; // as in "scsi<n>" where 'n' is one of 0, 1, 2 etc
	int channel;
	int scsi_id; // scsi id of target device
	int lun;
	int scsi_type; // TYPE_... defined in <scsi/scsi.h>
	short h_cmd_per_lun; // host (adapter) maximum commands per lun
	short d_queue_depth; // device (or adapter) maximum queue length
	int unused[2]; // probably find a good use, set 0 for now
} sg_scsi_id_t;

// ---- SG_GET_REQUEST_TABLE (always reports nothing outstanding) -------------
typedef struct sg_req_info {
	char req_state; // 0 -> not used, 1 -> written, 2 -> ready to read
	char orphan; // 0 -> normal request, 1 -> from interrupted SG_IO
	char sg_io_owned; // 0 -> complete with read(), 1 -> owned by SG_IO
	char problem; // 0 -> no problem detected, 1 -> error to report
	int pack_id; // pack_id associated with request
	void *usr_ptr; // user provided pointer (in new interface)
	unsigned int duration; // millisecs elapsed since written (req_state==1)
			       // or request duration (req_state==2)
	int unused;
} sg_req_info_t;

// ---- Requests (plain numbers in the 0x22 group) -----------------------------
#define SG_EMULATED_HOST 0x2203 // int *: true for emulated host adapter
				// (ATA through SCSI translation: 1)
#define SG_SET_TRANSFORM 0x2204 // obsolete
#define SG_GET_TRANSFORM 0x2205 // obsolete
#define SG_SET_RESERVED_SIZE 0x2275 // int *: reserved buffer size
#define SG_GET_RESERVED_SIZE 0x2272 // int *
#define SG_GET_SCSI_ID 0x2276 // struct sg_scsi_id *
#define SG_SET_FORCE_LOW_DMA 0x2279 // int *: obsolete, ignored
#define SG_GET_LOW_DMA 0x227a // int *: 0
#define SG_SET_FORCE_PACK_ID 0x227b // int *
#define SG_GET_PACK_ID 0x227c // int *: -1 (nothing waiting)
#define SG_GET_NUM_WAITING 0x227d // int *: 0
#define SG_GET_SG_TABLESIZE 0x227F // int *: max scatter/gather elements
#define SG_GET_VERSION_NUM 0x2282 // int *: SG_VERSION_NUM
#define SG_SCSI_RESET 0x2284 // int *: SG_SCSI_RESET_*
#define SG_IO 0x2285 // sg_io_hdr_t *: synchronous command
#define SG_GET_REQUEST_TABLE 0x2286 // sg_req_info_t[SG_MAX_QUEUE]
#define SG_SET_KEEP_ORPHAN 0x2287 // int *
#define SG_GET_KEEP_ORPHAN 0x2288 // int *
#define SG_GET_ACCESS_COUNT 0x2289 // int *: open count of the node
#define SG_SET_TIMEOUT 0x2201 // int *: default timeout in USER_HZ ticks
#define SG_GET_TIMEOUT 0x2202 // returns that timeout as the ioctl value
#define SG_GET_COMMAND_Q 0x2270 // int *: 1 when command queuing is on
#define SG_SET_COMMAND_Q 0x2271 // int *
#define SG_SET_DEBUG 0x227e // int *: ignored
#define SG_NEXT_CMD_LEN 0x2283 // int *: obsolete for SG_IO

// SG_SCSI_RESET argument values
#define SG_SCSI_RESET_NOTHING 0
#define SG_SCSI_RESET_DEVICE 1
#define SG_SCSI_RESET_BUS 2
#define SG_SCSI_RESET_HOST 3
#define SG_SCSI_RESET_TARGET 4
#define SG_SCSI_RESET_NO_ESCALATE 0x100

// ---- Defaults and limits ------------------------------------------------------
#define SG_MAX_SENSE 16 // the old fixed-size sense buffer
#define SG_MAX_QUEUE 16 // entries of SG_GET_REQUEST_TABLE
#define SG_SCATTER_SZ (8 * 4096) // default reserved buffer size
#define SG_DEFAULT_RETRIES 0
#define SG_DEF_FORCE_PACK_ID 0
#define SG_DEF_KEEP_ORPHAN 0
#define SG_DEF_RESERVED_SIZE SG_SCATTER_SZ
#define SG_MAX_CDB_SIZE 16 // longest CDB SG_IO accepts on an ATA disk
#define SG_DEFAULT_TIMEOUT_MS 60000 // sg_io_hdr.timeout == 0 means this

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_SG_H_
