// LikeOS - Block device abstraction
//
// A block device is anything that stores fixed-size sectors addressed by a
// logical block address: a whole disk (sda), one partition of it (sda1), an
// optical drive (sr0), a USB stick.  This header is the part every user of
// the block layer has always included -- the filesystems (ext4, fat32), the
// page cache, the storage bootstrap and the USB mass storage driver -- and it
// keeps its original shape: the first eight members of struct block_device
// and the four functions below behave exactly as they always did.
//
// What the extended block layer adds (request queues, asynchronous requests,
// scatter/gather, disks and partitions, device nodes, ioctls, statistics) is
// declared in <kernel/dev/block/blkdev.h>.  The extra members it needs in
// struct block_device are appended here, AFTER the original ones, so code
// that fills in only the original members (a driver that registers with
// block_register() the old way) still compiles and still works: everything
// it does not set stays zero, and zero means "legacy device" (magic is not
// BLOCK_DEVICE_MAGIC), which the block layer handles through the original
// synchronous read/write callbacks.
//
// Return conventions in THIS header (unchanged): ST_* status codes from
// <kernel/uapi/status.h>.  The extended API in blkdev.h returns 0 or a
// negative errno instead; blk_errno_to_status() converts.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_BLOCK_H
#define LIKEOS_BLOCK_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h> /* refcount_t */

/* How many block devices -- whole disks, partitions and optical drives
 * together -- can be registered at once.  Raised from 8 for the disk layer:
 * a SATA disk with a handful of partitions, an optical drive and a USB
 * stick with its own partitions already needed more than 8.  Registry
 * slots are never compacted (see block_get()), so this is also the limit
 * on how many devices may have been registered since boot minus those
 * removed whose slot was reused. */
#define BLOCK_MAX_DEVICES 64

/* struct block_device.magic of a device registered through the extended
 * block layer (disk_add(), partition registration).  Anything else -- in
 * practice 0 -- is a legacy registrant that only provides the original
 * synchronous callbacks.  "BLKD". */
#define BLOCK_DEVICE_MAGIC 0x424C4B44u

typedef struct block_device block_device_t;

/* The extended layer's types, defined in blkdev.h.  Only pointers to them
 * live in struct block_device, so this header does not need their layout. */
struct gendisk;
struct blk_partition;
struct blk_queue;
struct blk_ops;

typedef int (*block_read_fn)(block_device_t *dev, unsigned long lba,
			     unsigned long count, void *buf);
typedef int (*block_write_fn)(block_device_t *dev, unsigned long lba,
			      unsigned long count, const void *buf);
typedef int (*block_sync_fn)(block_device_t *dev);
/* Force-release any per-device sleeping locks held by the given task id.
 * Used by the FS-layer release_locks_for_task hook to recover from a task
 * that was killed mid-I/O while holding the device's own sleeping mutex
 * (e.g. usb_msd's per-device io_locked, which a fat32_io_lock holder
 * acquires nested when reading/writing sectors). */
typedef int (*block_release_locks_fn)(block_device_t *dev, uint64_t task_id);

/* struct block_device.flags (extended devices only) */
#define BDEV_F_WHOLE_DISK (1u << 0) /* the disk itself (sda), not a part */
#define BDEV_F_PARTITION (1u << 1) /* a partition (sda1); parent is set */
#define BDEV_F_CDROM (1u << 2) /* an optical drive (sr0) */
#define BDEV_F_REGISTERED (1u << 3) /* in the registry (block_get finds it) */
#define BDEV_F_NODE (1u << 4) /* has its /dev node */
#define BDEV_F_DEAD (1u << 5) /* hot-removed: every new request -ENODEV */
#define BDEV_F_SUSPENDED (1u << 6) /* blk_suspend_all() put it to sleep */
#define BDEV_F_EXCL (1u << 7) /* claimed exclusively (mounted) */

struct block_device {
	/* ---- The original members.  Every existing user relies on these;
	 * their meaning, types and order never change. ------------------- */
	const char *name; /* "sda", "sda1", "sr0", "usb0" */
	unsigned int sector_size; // bytes per sector (logical block size)
	unsigned long total_sectors; /* in sector_size units */
	block_read_fn read; /* synchronous; ST_* */
	block_write_fn write; /* synchronous; ST_* */
	block_sync_fn sync; // Optional: flush write cache to media
	block_release_locks_fn release_locks_for_task; // Optional
	void *driver_data; // pointer to underlying driver device

	/* ---- Extended block layer (blkdev.h).  Valid only when magic ==
	 * BLOCK_DEVICE_MAGIC; a legacy registrant leaves all of it zero. -- */
	uint32_t magic; /* BLOCK_DEVICE_MAGIC */
	uint32_t flags; /* BDEV_F_* */
	/* The operations of the disk this device belongs to (a partition
	 * shares its disk's).  Async submit, ioctl, power management. */
	const struct blk_ops *ops;
	/* The whole disk this device is, or is part of. */
	struct gendisk *disk;
	/* For a partition: its record (number, GUIDs, name); NULL for a whole
	 * disk.  Accessors in blkdev.h: blk_bdev_partuuid() and friends. */
	struct blk_partition *part;
	/* For a partition: the whole-disk device; NULL for a whole disk. */
	struct block_device *parent;
	/* The request queue requests to this device go through (a partition
	 * uses its disk's). */
	struct blk_queue *queue;
	/* First sector of this device on the parent (0 for a whole disk), in
	 * the parent's logical sectors.  blk_submit() adds it. */
	uint64_t start_lba;
	/* st_rdev of the /dev node: major 8 (sd) / 65-71 (sd beyond the 16th
	 * disk) / 11 (sr), see genhd.c and blkdev.h DISK_* constants. */
	uint32_t major, minor;
	/* Geometry and limits (bytes unless noted), copied from the disk's
	 * queue limits when the device is registered. */
	uint32_t physical_sector_size; /* 512, 4096 ... */
	uint32_t alignment_offset; /* bytes from LBA 0 to the first physical
				    * sector boundary (BLKALIGNOFF) */
	uint32_t io_min; /* BLKIOMIN: preferred minimum I/O */
	uint32_t io_opt; /* BLKIOOPT: optimal I/O, 0 = not reported */
	uint32_t max_sectors; /* largest single request, sectors */
	uint32_t queue_depth; /* commands the device accepts at once */
	uint32_t discard_granularity; /* bytes, 0 = no discard */
	uint64_t discard_max_sectors; /* largest discard, sectors */
	uint64_t write_zeroes_max_sectors; /* 0 = emulated by writes */
	uint8_t ro; /* read-only (BLKROGET/BLKROSET, write-protected media) */
	uint8_t removable; /* removable media (optical, USB) */
	uint8_t rotational; /* 1 = spinning disk, 0 = solid state */
	uint8_t flush_supported; /* volatile write cache that FLUSH empties */
	uint8_t fua_supported; /* forced unit access writes */
	uint8_t discard_zeroes_data; /* discarded sectors read back as 0 */
	/* Replaces the storage bootstrap's look at usb_msd_device_t.ready:
	 * the device answers I/O (media present, link up, configured). */
	volatile uint8_t ready;
	uint8_t pad0;
	/* Lifetime: one reference for the registry, one per open handle, one
	 * per mount and one per in-flight request.  blk_bdev_get/put(). */
	refcount_t refcount;
	/* Slot in the registry (block_get(index)), -1 when not registered. */
	int registry_index;
	/* Open handles through /dev (and mounts), and the exclusive holder
	 * (a mounted filesystem) -- BLKRRPART refuses while a partition of the
	 * disk is held.  Protected by the disk registry mutex. */
	int open_count;
	void *holder;
	/* Changes every time the disk's media or partition table changes
	 * (BLKGETDISKSEQ); a partition reports its disk's. */
	uint64_t diskseq;
};

/* Add a device to the registry.  Legacy registrants (magic != MAGIC) are
 * accepted exactly as before.  Extended devices are normally registered by
 * disk_add() / the partition scanner, which call this after filling in the
 * extended members.  Returns ST_OK, ST_INVALID (NULL), ST_ERR (full). */
int block_register(block_device_t *dev);
/* Remove a device from the registry: its slot reads NULL from then on and
 * may be reused by a later registration.  ST_OK or ST_NOT_FOUND. */
int block_unregister(block_device_t *dev);
/* The device in registry slot `index', or NULL for an empty slot (never
 * used, or its device was removed).  Slots are stable: a device keeps its
 * index for as long as it is registered, so callers that remember "I have
 * tried slot N" (the storage bootstrap) stay correct across hot-plug. */
const block_device_t *block_get(int index);
/* The number of slots in use or ever used -- the bound for a block_get()
 * loop, which must tolerate NULL holes. */
int block_count(void);
int block_sync(block_device_t *dev);

#endif // LIKEOS_BLOCK_H
