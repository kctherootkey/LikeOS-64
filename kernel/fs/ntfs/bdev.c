/* LikeOS - NTFS filesystem driver
 *
 * Device I/O, the write chokepoint, the guard device, raw run writes.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define NTFS_INTERNAL 1 /* the driver-internal section of ntfs.h */
#include <kernel/fs/ntfs/ntfs.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/fs/ntfs/ntfs_int.h>

/* ===========================================================================
 * S4  Device I/O, the write chokepoint and the guard device
 *
 * Every LBA in this driver is in DEVICE sectors (fs->dev_ss bytes), never in
 * NTFS sectors: a 4096-byte-sector device holding a volume formatted with
 * 512-byte NTFS sectors would otherwise be addressed eight times too
 * far.  Byte offsets are converted with
 * ntfs_byte_to_lba(), which requires device-sector alignment.
 * =========================================================================== */

/* Read `count' device sectors at `lba' from the REAL device.  The one read
 * helper of the driver: bounded by the device, chunked, uninterruptible
 * (a metadata read never returns half-way because of a signal). */
int ntfs_dev_read(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		  void *buf)
{
	block_device_t *b = fs->bdev;
	uint64_t end;
	uint8_t *p = (uint8_t *)buf;

	might_sleep();
	BUG_ON(!buf);
	if (!b || !b->read)
		return ST_NO_DEVICE;
	if (!ntfs_add_ok(lba, count, &end) || end > fs->dev_sectors) {
		WARN_RATELIMIT(1, "ntfs: read beyond the device (lba %llu "
				  "count %llu)",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_IO;
	}
	while (count) {
		uint64_t n = count > NTFS_MAX_SECTORS_PER_READ ?
				     NTFS_MAX_SECTORS_PER_READ :
				     count;
		int r = b->read(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK)
			return ST_IO;
		lba += n;
		p += n * fs->dev_ss;
		count -= n;
	}
	return ST_OK;
}

/* Read `len' bytes at volume byte offset `off' (both device-sector
 * aligned). */
int ntfs_dev_pread(ntfs_fs_t *fs, uint64_t off, uint64_t len, void *buf)
{
	if ((off | len) & (fs->dev_ss - 1)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	return ntfs_dev_read(fs, ntfs_byte_to_lba(fs, off),
			     len >> fs->dev_ss_bits, buf);
}

/* Read `n' clusters starting at `lcn' into `buf'. */
int ntfs_read_clusters(ntfs_fs_t *fs, int64_t lcn, uint64_t n,
		       void *buf)
{
	uint64_t end;

	if (lcn < 0 || !ntfs_add_ok((uint64_t)lcn, n, &end) ||
	    end > fs->nr_clusters)
		return ST_IO;
	return ntfs_dev_pread(fs, (uint64_t)lcn << fs->cluster_bits,
			      n << fs->cluster_bits, buf);
}

/* THE write chokepoint (ntfs.h section 3).  Nothing else in this file
 * writes to a device.  Refused unless the mount is read-write and not
 * latched read-only; the range must lie inside the volume.
 *
 * Lazy arming: the first write of a read-write mount arms it here, right
 * before it reaches the device (ntfs_arm(): $LogFile emptied, $Volume
 * marked dirty, device synced), so a mount that never changes anything
 * never writes, and an operation that turns out to be a no-op or fails
 * before its first write does not arm either.  Every caller holds the
 * metadata lock exclusive (the page cache's writeback holds lock_io()).
 *
 * A failed device write latches the mount read-only: from then on nothing
 * is written (the volume keeps its dirty flag), and the error reaches
 * fsync()/close()/sync() through fs->io_errs. */
int ntfs_dev_write(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		   const void *buf)
{
	block_device_t *b = fs ? fs->bdev : NULL;
	uint64_t end, vol_end;

	might_sleep();
	if (!fs || !b || !buf)
		return ST_INVALID;
	if (!ntfs_writable(fs)) {
		WARN_RATELIMIT(1, "ntfs: write refused on a read-only mount "
				  "(lba %llu count %llu)",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_ROFS;
	}
	vol_end = (fs->nr_clusters << fs->cluster_bits) >> fs->dev_ss_bits;
	/* Inside the cluster area, and never the boot sectors: nothing this
	 * driver writes lives in $Boot (cluster 0 and the first 8 KB) or in
	 * the backup boot sector behind the last cluster. */
	if (!ntfs_add_ok(lba, count, &end) || end > fs->dev_sectors ||
	    end > vol_end || lba < ntfs_boot_bytes(fs) >> fs->dev_ss_bits) {
		WARN_RATELIMIT(1, "ntfs: write outside the volume or into the "
				  "boot sectors (lba %llu count %llu)",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_INVALID;
	}
	if (!b->write)
		return ST_ROFS;
	if (!fs->armed && !fs->arming) {
		int r;

		WARN_ON_ONCE(!ntfs_meta_held_excl(fs));
		r = ntfs_arm(fs);
		if (r != ST_OK)
			return r;
	}
	fs->unflushed++;
	if (b->write(b, (unsigned long)lba, (unsigned long)count, buf) !=
	    ST_OK) {
		fs->io_errs++;
		ntfs_latch(fs, "a device write failed");
		return ST_IO;
	}
	return ST_OK;
}

/* Discard through the chokepoint's rules.  Two callers: the `discard'
 * option (freed clusters, after the cleared bitmap is on the device, so
 * the mount is armed) and an explicit trim of the free space
 * (ntfs_trim_fs(), `explicit' set: allowed on any read-write mount and
 * without arming -- discarding unallocated clusters changes nothing the
 * filesystem describes).  Every caller holds the metadata lock exclusive
 * and has checked the clusters free in $Bitmap under it. */
int ntfs_dev_discard(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		     int explicit)
{
	uint64_t end;
	int r;

	might_sleep();
	if (!fs || !fs->bdev)
		return ST_INVALID;
	if (!ntfs_writable(fs) || fs->hibernated ||
	    (!explicit && (!fs->armed || !fs->opt.discard))) {
		WARN_RATELIMIT(1, "ntfs: discard refused (lba %llu count %llu)",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_ROFS;
	}
	if (!ntfs_add_ok(lba, count, &end) || end > fs->dev_sectors)
		return ST_INVALID;
	r = blk_discard_sync(fs->bdev, lba, count, 0);
	return r == 0 ? ST_OK : ST_IO;
}

/* Flush the device's write cache (only ever after this mount wrote). */
int ntfs_dev_flush(ntfs_fs_t *fs)
{
	might_sleep();
	if (!fs->armed || !fs->bdev || !fs->bdev->sync || !fs->unflushed)
		return ST_OK;
	fs->unflushed = 0;
	if (fs->bdev->sync(fs->bdev) != ST_OK) {
		fs->io_errs++;
		ntfs_latch(fs, "a device cache flush failed");
		return ST_IO;
	}
	return ST_OK;
}

/* The guard device's operations: the page cache reads through the real
 * device and writes only through the chokepoint. */
static ntfs_fs_t *ntfs_guard_fs(block_device_t *dev)
{
	return (ntfs_fs_t *)((uint8_t *)dev - __builtin_offsetof(ntfs_fs_t, guard));
}

static int ntfs_guard_read(block_device_t *dev, unsigned long lba,
			   unsigned long count, void *buf)
{
	ntfs_fs_t *fs = ntfs_guard_fs(dev);

	return ntfs_dev_read(fs, lba, count, buf);
}

static int ntfs_guard_write(block_device_t *dev, unsigned long lba,
			    unsigned long count, const void *buf)
{
	return ntfs_dev_write(ntfs_guard_fs(dev), lba, count, buf);
}

static int ntfs_guard_sync(block_device_t *dev)
{
	ntfs_fs_t *fs = ntfs_guard_fs(dev);

	if (!fs->bdev || !fs->bdev->sync)
		return ST_OK;
	/* A read-only mount never wrote anything: nothing to flush. */
	if (!fs->armed)
		return ST_OK;
	return fs->bdev->sync(fs->bdev);
}

void ntfs_guard_init(ntfs_fs_t *fs)
{
	mm_memset(&fs->guard, 0, sizeof(fs->guard));
	fs->guard.name = fs->bdev->name;
	fs->guard.sector_size = fs->bdev->sector_size;
	fs->guard.total_sectors = fs->bdev->total_sectors;
	fs->guard.read = ntfs_guard_read;
	fs->guard.write = ntfs_guard_write;
	fs->guard.sync = ntfs_guard_sync;
}

/* ---- raw writes through a runlist ---- */

/* Write `len' bytes at byte `off' of the attribute described by `rl'
 * (`off' and `len' device-sector aligned): every piece mapped through the
 * runlist and written through the chokepoint.  A hole or an unmapped
 * piece is a caller error (ST_IO). */
int ntfs_rl_pwrite_raw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		       uint64_t off, const uint8_t *buf, uint64_t len)
{
	might_sleep();
	if ((off | len) & (fs->dev_ss - 1)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	while (len) {
		int64_t vcn = (int64_t)(off >> fs->cluster_bits);
		int64_t lcn, left;
		uint64_t in = off & (fs->cluster_size - 1);
		uint64_t n, dev;
		int r;

		r = ntfs_rl_map(rl, vcn, &lcn, &left);
		if (r != ST_OK || lcn == NTFS_LCN_HOLE || left <= 0) {
			WARN_RATELIMIT(1, "ntfs: write to an unmapped VCN %lld",
				       (long long)vcn);
			return ST_IO;
		}
		n = ((uint64_t)left << fs->cluster_bits) - in;
		if (n > len)
			n = len;
		if (n > 1024ULL * 1024ULL)
			n = 1024ULL * 1024ULL;
		dev = ((uint64_t)lcn << fs->cluster_bits) + in;
		r = ntfs_dev_write(fs, dev >> fs->dev_ss_bits,
				   n >> fs->dev_ss_bits, buf);
		if (r != ST_OK)
			return r;
		off += n;
		buf += n;
		len -= n;
	}
	return ST_OK;
}

/* Write `len' bytes at byte `off' of the attribute described by `rl',
 * any alignment: whole device sectors are written, the bytes around the
 * range read back from the device first (beyond the initialized size too:
 * they are written back unchanged).  Neighbouring records of a large
 * device block survive this way. */
int ntfs_rl_pwrite_rmw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		       uint64_t off, const uint8_t *src, uint64_t len)
{
	uint64_t a0, a1, end = off + len;
	uint8_t *bounce;
	int r;

	a0 = off & ~((uint64_t)fs->dev_ss - 1);
	a1 = (end + fs->dev_ss - 1) & ~((uint64_t)fs->dev_ss - 1);
	if (a0 == off && a1 == end)
		return ntfs_rl_pwrite_raw(fs, rl, off, src, len);
	/* Arm before the bytes around the range are read: arming itself
	 * writes (record 3 and its mirror copy, which can share a device
	 * block with the record written here), and a bounce read before it
	 * would put the old bytes back. */
	if (!fs->armed && !fs->arming) {
		r = ntfs_arm(fs);
		if (r != ST_OK)
			return r;
	}
	bounce = (uint8_t *)kalloc(a1 - a0);
	if (!bounce)
		return ST_NOMEM;
	r = ntfs_rl_pread(fs, rl, a0, a1 - a0, ~0ULL, bounce);
	if (r == ST_OK) {
		mm_memcpy(bounce + (off - a0), src, len);
		r = ntfs_rl_pwrite_raw(fs, rl, a0, bounce, a1 - a0);
	}
	kfree(bounce);
	return r;
}

/* Write `len' bytes at `off' of the non-resident metadata stream `st'
 * through to the device (ntfs_rl_pwrite_rmw()), then update the stream's
 * cached pages in place.  The metadata lock is held exclusive, so no
 * reader copies out of those pages meanwhile; they are never dirty
 * (metadata is never left to writeback). */
int ntfs_wt_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t off,
		  const uint8_t *src, uint64_t len)
{
	uint64_t end;
	int r;

	might_sleep();
	ntfs_assert_meta_excl(fs);
	if (!len)
		return ST_OK;
	if (!ntfs_add_ok(off, len, &end) || end > st->alloc_size ||
	    (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED))) {
		WARN_RATELIMIT(1, "ntfs: metadata write outside stream %u",
			       st->handle);
		return ST_IO;
	}
	r = ntfs_rl_pwrite_rmw(fs, &st->rl, off, src, len);
	if (r != ST_OK)
		return r;
	/* The cached pages show the new bytes (only those: the edges keep
	 * what the cache had, which is what a read shows for them). */
	for (uint64_t pos = off; pos < end;) {
		uint64_t idx = pos / PAGE_SIZE;
		uint64_t in = pos % PAGE_SIZE;
		uint64_t n = ntfs_min64(PAGE_SIZE - in, end - pos);
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, ntfs_chain(st),
						      idx);

		if (pg) {
			if (pg->data && pg->data + in != src + (pos - off))
				ntfs_memmove(pg->data + in, src + (pos - off),
					     n);
			pagecache_put(pg);
		}
		pos += n;
	}
	return ST_OK;
}
