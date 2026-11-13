// LikeOS zoned block device UAPI - the BLK*ZONE* ioctls
//
// A zoned block device (an NVMe Zoned Namespace, a host-managed SMR disk)
// is divided into zones that must be written sequentially at their write
// pointer and reset as a whole.  These are the established requests that
// zone-aware programs (blkzone, f2fs/btrfs tools, fio) issue against the
// device node.  The numbers sit in the block ioctl group (0x12) and are
// fixed by that interface.
//
//   BLKREPORTZONE    struct blk_zone_report + room for nr_zones entries:
//                    report the zones from `sector' on; nr_zones comes
//                    back as the number filled in, flags as
//                    BLK_ZONE_REP_* output flags
//   BLKREPORTZONEV2  the same, `flags' also an input (BLK_ZONE_REP_CACHED)
//   BLKRESETZONE     struct blk_zone_range: reset the write pointers of
//                    the zones in the range (needs write access)
//   BLKOPENZONE, BLKCLOSEZONE, BLKFINISHZONE   explicit open / close /
//                    finish of the zones in the range
//   BLKGETZONESZ     uint32_t: the zone size in 512-byte sectors (0: not
//                    a zoned device)
//   BLKGETNRZONES    uint32_t: the number of zones
//
// Sectors are 512-byte units whatever the logical block size is.
//
// Mirrored byte-for-byte into the libc include tree, at the established
// path programs include it by; keep both copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_BLKZONED_H_
#define _KERNEL_UAPI_BLKZONED_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#include <kernel/uapi/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

enum blk_zone_type {
	BLK_ZONE_TYPE_CONVENTIONAL = 0x1, // no write pointer
	BLK_ZONE_TYPE_SEQWRITE_REQ = 0x2, // sequential writes required
	BLK_ZONE_TYPE_SEQWRITE_PREF = 0x3, // sequential writes preferred
};

enum blk_zone_cond {
	BLK_ZONE_COND_NOT_WP = 0x0, // conventional: no write pointer
	BLK_ZONE_COND_EMPTY = 0x1,
	BLK_ZONE_COND_IMP_OPEN = 0x2, // implicitly opened (by a write)
	BLK_ZONE_COND_EXP_OPEN = 0x3, // explicitly opened
	BLK_ZONE_COND_CLOSED = 0x4,
	BLK_ZONE_COND_READONLY = 0xD,
	BLK_ZONE_COND_FULL = 0xE,
	BLK_ZONE_COND_OFFLINE = 0xF,
	// Cached reports only: open or closed, not told apart.
	BLK_ZONE_COND_ACTIVE = 0xFF,
#define BLK_ZONE_COND_ACTIVE BLK_ZONE_COND_ACTIVE
};

enum blk_zone_report_flags {
	// Output: the capacity field of each zone is valid.
	BLK_ZONE_REP_CAPACITY = (1U << 0),
	// Input (BLKREPORTZONEV2): a report from the cached zone state.
	BLK_ZONE_REP_CACHED = (1U << 31),
#define BLK_ZONE_REP_CACHED BLK_ZONE_REP_CACHED
};

struct blk_zone {
	uint64_t start; // zone start sector
	uint64_t len; // zone length in sectors
	uint64_t wp; // write pointer position (sector)
	uint8_t type; // enum blk_zone_type
	uint8_t cond; // enum blk_zone_cond
	uint8_t non_seq; // non-sequential write resources active
	uint8_t reset; // a reset of the write pointer is recommended
	uint8_t resv[4];
	uint64_t capacity; // writable sectors (<= len), BLK_ZONE_REP_CAPACITY
	uint8_t reserved[24];
};

struct blk_zone_report {
	uint64_t sector; // where the report starts
	uint32_t nr_zones; // in: room in zones[]; out: entries filled
	uint32_t flags; // enum blk_zone_report_flags
	struct blk_zone zones[];
};

struct blk_zone_range {
	uint64_t sector;
	uint64_t nr_sectors;
};

#define BLKREPORTZONE _IOWR(0x12, 130, struct blk_zone_report)
#define BLKRESETZONE _IOW(0x12, 131, struct blk_zone_range)
#define BLKGETZONESZ _IOR(0x12, 132, uint32_t)
#define BLKGETNRZONES _IOR(0x12, 133, uint32_t)
#define BLKOPENZONE _IOW(0x12, 134, struct blk_zone_range)
#define BLKCLOSEZONE _IOW(0x12, 135, struct blk_zone_range)
#define BLKFINISHZONE _IOW(0x12, 136, struct blk_zone_range)
#define BLKREPORTZONEV2 _IOWR(0x12, 142, struct blk_zone_report)

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_BLKZONED_H_
