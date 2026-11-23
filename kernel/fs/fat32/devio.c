/* LikeOS - FAT filesystem driver: device I/O: bounded reads, THE write
 * chokepoint, discard, sync, the guard block device the page cache
 * writes through
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ===========================================================================
 * 4. Device I/O
 *
 * Reads go to the real device, bounded by the device and chunked.  Writes
 * and discards have exactly ONE way to the device each: fat_dev_write() and
 * fat_dev_discard().  They refuse unless the mount is armed read-write and
 * not latched read-only, and unless the sectors lie inside the region the
 * caller's kind of write may touch.  The page cache never sees the real
 * device: it gets fs->guard, whose write is the chokepoint.
 * =========================================================================== */

int fat_dev_read(fat_fs_t *fs, uint64_t lba, uint64_t count, void *buf)
{
	block_device_t *b = fs->bdev;
	uint64_t end, max_chunk;
	uint8_t *p = (uint8_t *)buf;

	might_sleep();
	if (!b || !b->read || !buf)
		return ST_IO;
	if (count == 0)
		return ST_OK;
	if (!fat_add_ok(lba, count, &end) || end > b->total_sectors) {
		WARN_RATELIMIT(1,
			       "fat: read of sectors %llu+%llu beyond the device (%lu sectors)",
			       (unsigned long long)lba,
			       (unsigned long long)count, b->total_sectors);
		return ST_IO;
	}
	max_chunk = FAT_MAX_IO_BYTES >> fs->dev_shift;
	if (max_chunk == 0)
		max_chunk = 1;
	while (count) {
		uint64_t n = count < max_chunk ? count : max_chunk;
		int r = b->read(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK)
			return r < 0 ? r : ST_IO;
		fs->st_dev_reads++;
		lba += n;
		count -= n;
		p += n << fs->dev_shift;
	}
	return ST_OK;
}

/* The device-sector range a kind of write may touch: [*lo, *hi).  0 for a
 * kind this volume has no region of. */
static int fat_write_range(fat_fs_t *fs, int kind, uint64_t *lo, uint64_t *hi)
{
	switch (kind) {
	case FAT_W_BOOT:
		*lo = 0;
		*hi = fs->boot_dev_sectors;
		return 1;
	case FAT_W_FSINFO:
		if (!fs->fsinfo_lba)
			return 0;
		*lo = fs->fsinfo_lba;
		*hi = fs->fsinfo_lba + fs->boot_dev_sectors;
		return 1;
	case FAT_W_FAT:
		*lo = fs->fat_lba[0];
		*hi = fs->fat_lba[fs->nfats - 1] +
		      (fs->fat_bytes >> fs->dev_shift);
		return 1;
	case FAT_W_ROOT:
		if (!fs->root_dev_sectors)
			return 0;
		*lo = fs->root_lba;
		*hi = fs->root_lba + fs->root_dev_sectors;
		return 1;
	case FAT_W_DATA:
		*lo = fs->data_lba;
		*hi = fs->data_lba + fs->data_dev_sectors;
		return 1;
	default:
		return 0;
	}
}

/* THE write chokepoint.  Every byte this driver ever stores goes through
 * here -- its own metadata writes and, through the guard device, the page
 * cache's writeback of file data. */
int fat_dev_write(fat_fs_t *fs, uint64_t lba, uint64_t count,
			 const void *buf, int kind)
{
	block_device_t *b = fs->bdev;
	uint64_t end, lo, hi, max_chunk;
	const uint8_t *p = (const uint8_t *)buf;

	might_sleep();
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) && fs->may_write &&
	    !fs->ro && !fs->ro_latched && fat_rwsem_held_excl(&fs->meta)) {
		/* The first real write of a mutation: the dirty marks go to
		 * the device (and are synced) before anything else does. */
		int ar = fat_arm_now(fs);

		if (ar != ST_OK) {
			fs->io_errors++;
			return ar;
		}
	}
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) || fs->ro ||
	    fs->ro_latched) {
		fs->io_errors++;
		/* After errors=remount-ro the refusal is the policy working;
		 * anything else is a write nobody was entitled to make. */
		if (fs->ro_latched) {
			if (fs->refused_logged < 5) {
				fs->refused_logged++;
				kprintf("fat: %s: write of sector %llu refused: file system is read-only after errors%s\n",
					b ? b->name : "?",
					(unsigned long long)lba,
					fs->refused_logged == 5 ?
						" (further refusals not logged)" :
						"");
			}
		} else {
			WARN_RATELIMIT(1,
				       "fat: write of sectors %llu+%llu refused: mount not armed read-write",
				       (unsigned long long)lba,
				       (unsigned long long)count);
		}
		return ST_ROFS;
	}
	if (!b || !b->write || !buf) {
		fs->io_errors++;
		return ST_IO;
	}
	if (count == 0)
		return ST_OK;
	if (!fat_write_range(fs, kind, &lo, &hi)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	if (hi > fs->vol_dev_sectors)
		hi = fs->vol_dev_sectors;
	if (hi > b->total_sectors)
		hi = b->total_sectors;
	if (!fat_add_ok(lba, count, &end) || lba < lo || end > hi) {
		WARN_RATELIMIT(1,
			       "fat: write of sectors %llu+%llu outside the allowed range %llu..%llu (kind %d) refused",
			       (unsigned long long)lba,
			       (unsigned long long)count,
			       (unsigned long long)lo, (unsigned long long)hi,
			       kind);
		return ST_INVALID;
	}
	max_chunk = FAT_MAX_IO_BYTES >> fs->dev_shift;
	if (max_chunk == 0)
		max_chunk = 1;
	while (count) {
		uint64_t n = count < max_chunk ? count : max_chunk;
		int r = b->write(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK) {
			fs->io_errors++;
			return r < 0 ? r : ST_IO;
		}
		fs->st_dev_writes++;
		fs->barrier_needed = 1;
		lba += n;
		count -= n;
		p += n << fs->dev_shift;
	}
	return ST_OK;
}

/* THE discard chokepoint: data-area sectors only, only when armed, only with
 * the `discard' option.  The caller has made sure the freed FAT entries are
 * on the device first (section 17). */
int fat_dev_discard(fat_fs_t *fs, uint64_t lba, uint64_t count)
{
	block_device_t *b = fs->bdev;
	uint64_t end;
	int r;

	might_sleep();
	if (!fs->opts.discard)
		return ST_OK;
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) || fs->ro ||
	    fs->ro_latched) {
		if (!fs->ro_latched)
			WARN_RATELIMIT(1,
				       "fat: discard of sectors %llu+%llu refused: mount not armed",
				       (unsigned long long)lba,
				       (unsigned long long)count);
		return ST_ROFS;
	}
	if (!b || count == 0)
		return ST_OK;
	if (!fat_add_ok(lba, count, &end) || lba < fs->data_lba ||
	    end > fs->data_lba + fs->data_dev_sectors ||
	    end > fs->vol_dev_sectors || end > b->total_sectors) {
		WARN_RATELIMIT(1,
			       "fat: discard of sectors %llu+%llu outside the data area refused",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_INVALID;
	}
	if (!blk_bdev_is_extended(b) || !b->discard_max_sectors)
		return ST_OK; /* the device has no discard: nothing to do */
	r = blk_discard_sync(b, lba, count, 0);
	if (r == 0) {
		fs->st_discards++;
		return ST_OK;
	}
	/* A refused discard loses nothing: the clusters are free either way. */
	return ST_OK;
}

int fat_dev_sync(fat_fs_t *fs)
{
	block_device_t *b = fs->bdev;

	might_sleep();
	if (!b || !b->sync) {
		fs->barrier_needed = 0;
		return ST_OK;
	}
	if (b->sync(b) != ST_OK)
		return ST_IO;
	fs->barrier_needed = 0;
	return ST_OK;
}

/* ---- the guard device the page cache writes through ---------------------- */

static fat_fs_t *fat_fs_of_guard(block_device_t *dev)
{
	return (fat_fs_t *)((char *)dev - __builtin_offsetof(fat_fs_t, guard));
}

static int fat_guard_read(block_device_t *dev, unsigned long lba,
			  unsigned long count, void *buf)
{
	return fat_dev_read(fat_fs_of_guard(dev), lba, count, buf);
}

static int fat_guard_write(block_device_t *dev, unsigned long lba,
			   unsigned long count, const void *buf)
{
	/* File pages: the data area, nothing else. */
	return fat_dev_write(fat_fs_of_guard(dev), lba, count, buf,
			     FAT_W_DATA);
}

static int fat_guard_sync(block_device_t *dev)
{
	return fat_dev_sync(fat_fs_of_guard(dev));
}

void fat_guard_init(fat_fs_t *fs)
{
	mm_memset(&fs->guard, 0, sizeof(fs->guard));
	fs->guard.name = fs->bdev->name;
	fs->guard.sector_size = fs->bdev->sector_size;
	fs->guard.total_sectors = fs->bdev->total_sectors;
	fs->guard.read = fat_guard_read;
	fs->guard.write = fat_guard_write;
	fs->guard.sync = fat_guard_sync;
	/* magic stays 0: a legacy device as far as anybody can tell, so the
	 * extended members (all zero) are never consulted. */
}

/* Zero `count' device sectors from `lba' of the data area (directory
 * clusters before they become reachable, file ranges a file grows over).
 * Through the chokepoint, in FAT_ZERO_CHUNK pieces. */
int fat_dev_zero(fat_fs_t *fs, uint64_t lba, uint64_t count)
{
	uint64_t per = FAT_ZERO_CHUNK >> fs->dev_shift;
	uint8_t *z;
	int r = ST_OK;

	might_sleep();
	if (count == 0)
		return ST_OK;
	if (per == 0)
		per = 1;
	z = (uint8_t *)kcalloc(1, (size_t)(fat_min64(count, per)
					   << fs->dev_shift));
	if (!z)
		return ST_NOMEM;
	while (count && r == ST_OK) {
		uint64_t n = count < per ? count : per;

		r = fat_dev_write(fs, lba, n, z, FAT_W_DATA);
		lba += n;
		count -= n;
	}
	kfree(z);
	if (r == ST_OK)
		fs->barrier_needed = 1;
	return r;
}
