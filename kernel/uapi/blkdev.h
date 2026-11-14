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
// The mount(2) flags and the mount/umount prototypes follow at the end.
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

// ---- Mounting -----------------------------------------------------------------
//
// mount(2) flags (the `mountflags' argument).  The kernel keeps the
// RDONLY .. LAZYTIME group with the mount and acts on RDONLY (every change
// fails with EROFS), NOSUID (exec ignores set-user/-group-ID bits), NODEV
// (device nodes cannot be opened) and NOEXEC (nothing runs from it); the
// others are shown in /proc/mounts.  REMOUNT changes the flags of an
// existing mount (read-only <-> read-write included).  BIND and MOVE are
// not offered (EINVAL).
#define MS_RDONLY 0x1
#define MS_NOSUID 0x2
#define MS_NODEV 0x4
#define MS_NOEXEC 0x8
#define MS_SYNCHRONOUS 0x10
#define MS_REMOUNT 0x20
#define MS_MANDLOCK 0x40
#define MS_DIRSYNC 0x80
#define MS_NOSYMFOLLOW 0x100
#define MS_NOATIME 0x400
#define MS_NODIRATIME 0x800
#define MS_BIND 0x1000
#define MS_MOVE 0x2000
#define MS_REC 0x4000
#define MS_SILENT 0x8000
#define MS_RELATIME 0x200000
#define MS_STRICTATIME 0x1000000
#define MS_LAZYTIME 0x2000000
#define MS_MGC_VAL 0xC0ED0000 // old magic in the high half; ignored
#define MS_MGC_MSK 0xffff0000

// umount2(2) flags.  FORCE is accepted and changes nothing (open files keep
// a filesystem); DETACH takes a busy filesystem out of the name space at
// once and finishes the unmount when the last file open on it is closed.
#define MNT_FORCE 0x1
#define MNT_DETACH 0x2
#define MNT_EXPIRE 0x4
#define UMOUNT_NOFOLLOW 0x8

#ifndef __LIKEOS_KERNEL__
// source: a block device ("/dev/sda2", "/dev/disk/by-uuid/..."), or a label
// for a filesystem without one ("tmpfs").  fstype: "ext4" ("ext3", "ext2"),
// "vfat" ("fat32", "msdos"), "tmpfs", or "auto".  data: the filesystem's
// option string ("uid=1000,umask=022", "size=64m,mode=1777"), or NULL.
int mount(const char *source, const char *target, const char *fstype,
	  unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);
#endif

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_BLKDEV_H_
