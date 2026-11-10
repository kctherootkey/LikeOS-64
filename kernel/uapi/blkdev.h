// LikeOS block device UAPI - the BLK* ioctls of block device nodes
//
// Request numbers and argument types of the established block device ioctl
// interface, as programs written for it expect them: the standard disk
// utilities (lsblk, blkid, fdisk, sfdisk, wipefs, blkdiscard), e2fsprogs,
// hdparm and the like issue exactly these against /dev/sda, /dev/sda1,
// /dev/sr0.  Every request is in the 0x12 ("block") ioctl group; the
// numbers are fixed by that interface and must never be renumbered.
//
// Encoding: the requests from 93 to 105 predate the size/direction fields
// and are plain _IO(0x12, nr) even when they read or write an argument --
// that is part of the ABI, not an oversight.  The later ones carry their
// argument size (an 8-byte size_t or uint64_t on this 64-bit system).
//
// Argument conventions (what `arg' points to):
//   BLKROSET      const int *      0 = read-write, 1 = read-only (root only)
//   BLKROGET      int *            current read-only state
//   BLKRRPART     -                re-read the partition table (EBUSY while
//                                  a partition is in use)
//   BLKGETSIZE    unsigned long *  size in 512-byte sectors (whatever the
//                                  logical sector size is)
//   BLKFLSBUF     -                flush cached data and the drive cache
//   BLKRASET      unsigned long    read-ahead in 512-byte sectors (value)
//   BLKRAGET      long *
//   BLKFRASET     unsigned long    filesystem read-ahead (value)
//   BLKFRAGET     long *
//   BLKSECTSET    unsigned short * (obsolete; answered with -EINVAL)
//   BLKSECTGET    unsigned short * largest request in 512-byte sectors
//   BLKSSZGET     int *            logical sector size in bytes
//   BLKPG         struct blkpg_ioctl_arg *  add/delete/resize a partition
//   BLKBSZGET     int *            soft block size the device is opened with
//   BLKBSZSET     const int *      set it (power of two, >= sector size,
//                                  <= page size)
//   BLKGETSIZE64  uint64_t *       size in bytes
//   BLKDISCARD    const uint64_t[2]  {start, length} in bytes, both aligned
//                                  to the logical sector size
//   BLKIOMIN      unsigned int *   minimum preferred I/O size
//   BLKIOOPT      unsigned int *   optimal I/O size (0 = not reported)
//   BLKALIGNOFF   int *            alignment offset in bytes
//   BLKPBSZGET    unsigned int *   physical sector size
//   BLKDISCARDZEROES unsigned int * 1 if discarded data reads back as zero
//   BLKSECDISCARD const uint64_t[2]  secure discard of {start, length}
//   BLKROTATIONAL unsigned short * 1 = rotational, 0 = solid state
//   BLKZEROOUT    const uint64_t[2]  write zeroes over {start, length}
//   BLKGETDISKSEQ uint64_t *       the disk sequence number (changes with
//                                  every media or partition table change)
//
// Mirrored into the userspace libc include tree as <sys/mount.h>, where the
// established interface puts these definitions; keep both copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_BLKDEV_H_
#define _KERNEL_UAPI_BLKDEV_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#include <kernel/uapi/ioctl.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#endif

// ---- The original requests (plain _IO numbers, see above) ----------------
#define BLKROSET _IO(0x12, 93) // set device read-only (0 = read-write)
#define BLKROGET _IO(0x12, 94) // get read-only status (0 = read-write)
#define BLKRRPART _IO(0x12, 95) // re-read partition table
#define BLKGETSIZE _IO(0x12, 96) // size in 512-byte sectors (unsigned long *)
#define BLKFLSBUF _IO(0x12, 97) // flush buffer cache
#define BLKRASET _IO(0x12, 98) // set read ahead for block device
#define BLKRAGET _IO(0x12, 99) // get current read ahead setting
#define BLKFRASET _IO(0x12, 100) // set filesystem read-ahead
#define BLKFRAGET _IO(0x12, 101) // get filesystem read-ahead
#define BLKSECTSET _IO(0x12, 102) // set max sectors per request (obsolete)
#define BLKSECTGET _IO(0x12, 103) // get max sectors per request
#define BLKSSZGET _IO(0x12, 104) // get logical block size
#define BLKPG _IO(0x12, 105) // partition table manipulation (blkpg below)

// ---- Size-encoded requests --------------------------------------------------
#define BLKBSZGET _IOR(0x12, 112, size_t)
#define BLKBSZSET _IOW(0x12, 113, size_t)
#define BLKGETSIZE64 _IOR(0x12, 114, size_t) // device size in bytes
#define BLKDISCARD _IO(0x12, 119)
#define BLKIOMIN _IO(0x12, 120)
#define BLKIOOPT _IO(0x12, 121)
#define BLKALIGNOFF _IO(0x12, 122)
#define BLKPBSZGET _IO(0x12, 123)
#define BLKDISCARDZEROES _IO(0x12, 124)
#define BLKSECDISCARD _IO(0x12, 125)
#define BLKROTATIONAL _IO(0x12, 126)
#define BLKZEROOUT _IO(0x12, 127)
#define BLKGETDISKSEQ _IOR(0x12, 128, uint64_t)

// ---- BLKPG: partition manipulation ----------------------------------------
//
// The argument is a struct blkpg_ioctl_arg whose `data' points to a struct
// blkpg_partition.  `start' and `length' are in BYTES; `pno' is the
// partition number (1-based).  devname/volname are informational and
// ignored.  Adding a partition that overlaps another, or deleting/resizing
// one that is in use, fails with EBUSY.
#define BLKPG_ADD_PARTITION 1
#define BLKPG_DEL_PARTITION 2
#define BLKPG_RESIZE_PARTITION 3

#define BLKPG_DEVNAMELTH 64
#define BLKPG_VOLNAMELTH 64

struct blkpg_ioctl_arg {
	int op; // BLKPG_*
	int flags; // unused, 0
	int datalen; // sizeof(struct blkpg_partition)
	void *data; // struct blkpg_partition *
};

struct blkpg_partition {
	long long start; // starting offset in bytes
	long long length; // length in bytes
	int pno; // partition number
	char devname[BLKPG_DEVNAMELTH]; // partition name, like sda5 or c0d1p2
	char volname[BLKPG_VOLNAMELTH]; // volume label
};

// The established spellings of the BLKPG structure's size, used by programs
// that validate datalen.
#define BLKPG_PARTITION_SIZE ((int)sizeof(struct blkpg_partition))

// ---- Device-number majors of the disk naming scheme -------------------------
//
// What /dev/sdX, /dev/sdXN and /dev/srN report as their major number in
// st_rdev.  Sixteen minors per disk: minor 16*d is the whole disk d, minors
// 16*d+1 .. 16*d+15 its partitions 1..15.  Disks 0..15 (sda..sdp) use major
// 8; disks 16..127 continue on majors 65..71 the same way.
#define SCSI_DISK0_MAJOR 8
#define SCSI_DISK1_MAJOR 65
#define SCSI_DISK7_MAJOR 71
#define SCSI_CDROM_MAJOR 11

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_BLKDEV_H_
