/* LikeOS - exFAT filesystem driver
 *
 * Boot region, commit and clean marking, mount options, mount, unmount,
 * remount, probe.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/exfat/exfat.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/mount.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/exfat/exfat_int.h>

/* 1 once mount() brings up a working instance (exfat.h, section 6).  While
 * 0, probe() answers 0 so that "auto" never picks this driver, and mount()
 * refuses with ST_UNSUPPORTED. */
#define EXFAT_DRIVER_READY 1

static const char *const exfat_ro_reason_text[] = {
	[EXFAT_RO_NONE] = "",
	[EXFAT_RO_REQUESTED] = "read-only requested",
	[EXFAT_RO_DEVICE] = "the device is write-protected",
	[EXFAT_RO_BACKUP_BOOT] =
		"main boot region invalid, mounted from the backup boot region",
	[EXFAT_RO_REVISION] = "unsupported file system revision",
	[EXFAT_RO_TWO_FATS] = "two FATs (TexFAT) are not written",
	[EXFAT_RO_DIRTY] = "volume is dirty (run fsck.exfat, or mount with force)",
	[EXFAT_RO_MEDIA_FAILURE] = "volume reports a media failure",
	[EXFAT_RO_GEOMETRY] =
		"volume regions are not aligned to the device's sectors",
	[EXFAT_RO_NO_BITMAP] = "allocation bitmap missing or unusable",
	[EXFAT_RO_UPCASE] = "upcase table missing or damaged",
	[EXFAT_RO_ERRORS] = "file system errors (errors=remount-ro)",
	[EXFAT_RO_OWN_FREE] =
		"allocation bitmap marks the root directory, bitmap or upcase table free",
};

/* ===========================================================================
 * 6. Boot region
 * =========================================================================== */

/* The fields of a boot sector this driver uses, parsed and checked. */
struct exfat_geo {
	uint64_t vol_length; /* fs sectors */
	uint32_t fat_offset;
	uint32_t fat_length;
	uint32_t heap_offset;
	uint32_t cluster_count;
	uint32_t root_cluster;
	uint32_t serial;
	uint16_t revision;
	uint16_t flags;
	unsigned bps_shift;
	unsigned spc_shift;
	unsigned nfats;
	uint8_t percent;
};

/* Does `b' (one boot sector, at least 512 bytes) describe an exFAT volume
 * of the specification's revision 1?  The checks a probe makes: name,
 * zeroed legacy area, shifts in range, one or two FATs, non-empty regions,
 * the boot signature.  Whether the volume may be written is the mount's
 * business. */
static int exfat_boot_sane(const uint8_t *b)
{
	static const char name[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
	unsigned bps_shift, spc_shift;

	for (int i = 0; i < 8; i++)
		if (b[EXFAT_BS_FSNAME + i] != (uint8_t)name[i])
			return 0;
	for (int i = EXFAT_BS_MBZ; i < EXFAT_BS_MBZ + EXFAT_BS_MBZ_LEN; i++)
		if (b[i])
			return 0;
	if (b[EXFAT_BS_SIGNATURE] != 0x55 || b[EXFAT_BS_SIGNATURE + 1] != 0xAA)
		return 0;
	bps_shift = b[EXFAT_BS_BPS_SHIFT];
	spc_shift = b[EXFAT_BS_SPC_SHIFT];
	if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift)
		return 0;
	if (b[EXFAT_BS_NFATS] != 1 && b[EXFAT_BS_NFATS] != 2)
		return 0;
	if ((exfat_le16(b + EXFAT_BS_REVISION) >> 8) != 1)
		return 0;
	if (exfat_le32(b + EXFAT_BS_FAT_OFFSET) == 0 ||
	    exfat_le32(b + EXFAT_BS_FAT_LENGTH) == 0)
		return 0;
	if (exfat_le32(b + EXFAT_BS_HEAP_OFFSET) == 0 ||
	    exfat_le32(b + EXFAT_BS_CLUSTER_COUNT) == 0)
		return 0;
	if (exfat_le32(b + EXFAT_BS_ROOT_CLUSTER) < EXFAT_FIRST_CLUSTER)
		return 0;
	return 1;
}

/* Parse a boot sector that passed exfat_boot_sane() and check its geometry
 * against the specification's rules, every sum overflow-checked.  ST_OK or
 * ST_INVALID with a log line naming the broken rule. */
static int exfat_geo_parse(const uint8_t *b, struct exfat_geo *g,
			   const char *dev)
{
	uint64_t fat_end, heap_end, need_fat_bytes;

	mm_memset(g, 0, sizeof(*g));
	g->vol_length = exfat_le64(b + EXFAT_BS_VOL_LENGTH);
	g->fat_offset = exfat_le32(b + EXFAT_BS_FAT_OFFSET);
	g->fat_length = exfat_le32(b + EXFAT_BS_FAT_LENGTH);
	g->heap_offset = exfat_le32(b + EXFAT_BS_HEAP_OFFSET);
	g->cluster_count = exfat_le32(b + EXFAT_BS_CLUSTER_COUNT);
	g->root_cluster = exfat_le32(b + EXFAT_BS_ROOT_CLUSTER);
	g->serial = exfat_le32(b + EXFAT_BS_SERIAL);
	g->revision = exfat_le16(b + EXFAT_BS_REVISION);
	g->flags = exfat_le16(b + EXFAT_BS_FLAGS);
	g->bps_shift = b[EXFAT_BS_BPS_SHIFT];
	g->spc_shift = b[EXFAT_BS_SPC_SHIFT];
	g->nfats = b[EXFAT_BS_NFATS];
	g->percent = b[EXFAT_BS_PERCENT];

	if (g->fat_offset < EXFAT_BOOT_REGION_SECTORS * 2) {
		kprintf("exfat: %s: FAT offset %u inside the boot regions\n",
			dev, g->fat_offset);
		return ST_INVALID;
	}
	if (g->cluster_count > EXFAT_MAX_CLUSTER_COUNT) {
		kprintf("exfat: %s: cluster count %u too large\n", dev,
			g->cluster_count);
		return ST_INVALID;
	}
	/* One FAT must hold ClusterCount + 2 four-byte entries. */
	need_fat_bytes = ((uint64_t)g->cluster_count + 2) * 4;
	if (((uint64_t)g->fat_length << g->bps_shift) < need_fat_bytes) {
		kprintf("exfat: %s: FAT of %u sectors too small for %u clusters\n",
			dev, g->fat_length, g->cluster_count);
		return ST_INVALID;
	}
	fat_end = (uint64_t)g->fat_offset +
		  (uint64_t)g->fat_length * (uint64_t)g->nfats;
	if (fat_end > g->heap_offset) {
		kprintf("exfat: %s: FAT region overlaps the cluster heap\n",
			dev);
		return ST_INVALID;
	}
	heap_end = (uint64_t)g->heap_offset +
		   ((uint64_t)g->cluster_count << g->spc_shift);
	if (heap_end > g->vol_length) {
		kprintf("exfat: %s: cluster heap (%llu sectors) beyond the volume length (%llu)\n",
			dev, (unsigned long long)heap_end,
			(unsigned long long)g->vol_length);
		return ST_INVALID;
	}
	if (g->root_cluster < EXFAT_FIRST_CLUSTER ||
	    g->root_cluster > g->cluster_count + 1) {
		kprintf("exfat: %s: root directory cluster %u out of range\n",
			dev, g->root_cluster);
		return ST_INVALID;
	}
	return ST_OK;
}

/* Check one boot region (12 fs sectors at `r'): a sane boot sector and the
 * checksum sector repeating the checksum of the first eleven. */
static int exfat_boot_region_ok(const uint8_t *r, unsigned long ss)
{
	uint32_t sum;
	const uint8_t *cs;

	if (!exfat_boot_sane(r))
		return 0;
	if ((1UL << r[EXFAT_BS_BPS_SHIFT]) != ss)
		return 0;
	sum = exfat_boot_checksum(r, ss);
	cs = r + ss * EXFAT_BOOT_CHECKSUM_SECTOR;
	for (unsigned long i = 0; i + 4 <= ss; i += 4)
		if (exfat_le32(cs + i) != sum)
			return 0;
	return 1;
}

/* Read a boot region starting at byte `byte_off' of the device whose fs
 * sector size is `ss'.  The buffer (kalloc'd, freed by the caller) holds the
 * twelve sectors.  ST_OK, or a read error / ST_INVALID when the offset is
 * not on a device sector. */
static int exfat_read_region(exfat_fs_t *fs, uint64_t byte_off,
			     unsigned long ss, uint8_t **out)
{
	uint64_t bytes = (uint64_t)ss * EXFAT_BOOT_REGION_SECTORS;
	uint64_t dev_bytes = exfat_round_up(bytes, fs->dev_ss);
	uint8_t *buf;
	int r;

	*out = NULL;
	if (byte_off & (fs->dev_ss - 1))
		return ST_INVALID;
	buf = (uint8_t *)kalloc((size_t)dev_bytes);
	if (!buf)
		return ST_NOMEM;
	r = exfat_dev_read(fs, byte_off >> fs->dev_shift,
			   dev_bytes >> fs->dev_shift, buf);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	return ST_OK;
}

/* Find the boot region to mount from: the main one when it is sane and its
 * checksum holds; else the backup (read-only).  Fills *g, *from_backup. */
static int exfat_boot_load(exfat_fs_t *fs, struct exfat_geo *g,
			   int *from_backup)
{
	uint8_t *first = NULL, *region = NULL;
	unsigned long ss = 0;
	int r;

	*from_backup = 0;
	/* The first device sector holds the boot sector's first 512 bytes
	 * whatever the sector sizes are. */
	first = (uint8_t *)kalloc(fs->dev_ss);
	if (!first)
		return ST_NOMEM;
	r = exfat_dev_read(fs, 0, 1, first);
	if (r != ST_OK) {
		kfree(first);
		return r;
	}
	if (exfat_boot_sane(first)) {
		ss = 1UL << first[EXFAT_BS_BPS_SHIFT];
		r = exfat_read_region(fs, 0, ss, &region);
		if (r == ST_NOMEM) {
			kfree(first);
			return r;
		}
		if (r == ST_OK && exfat_boot_region_ok(region, ss) &&
		    exfat_geo_parse(region, g, fs->bdev->name) == ST_OK) {
			kfree(first);
			kfree(region);
			return ST_OK;
		}
		if (region)
			kfree(region);
		region = NULL;
	}
	kfree(first);

	/* The backup region: after the main one, whose sector size we may not
	 * know -- try every legal one whose region lands on a device sector. */
	for (unsigned shift = 9; shift <= 12; shift++) {
		unsigned long bss = 1UL << shift;

		r = exfat_read_region(fs, (uint64_t)bss * EXFAT_BACKUP_BOOT_FIRST,
				      bss, &region);
		if (r == ST_NOMEM)
			return r;
		if (r != ST_OK)
			continue;
		if (exfat_boot_region_ok(region, bss) &&
		    exfat_geo_parse(region, g, fs->bdev->name) == ST_OK) {
			kfree(region);
			*from_backup = 1;
			return ST_OK;
		}
		kfree(region);
		region = NULL;
	}
	return ST_NOT_FOUND;
}

/* Rewrite VolumeFlags / PercentInUse of the main boot sector.  The boot
 * checksum does not cover these bytes, so nothing else changes.  Reads the
 * sector fresh (the device holds the truth), patches, writes it through the
 * chokepoint and syncs.  Caller holds the metadata lock exclusive and has
 * armed the mount. */
static int exfat_boot_write_flags(exfat_fs_t *fs, int set_dirty,
				  int update_percent)
{
	uint64_t n = exfat_boot_dev_sectors(fs);
	uint8_t *buf = fs->boot_buf; /* metadata lock exclusive: ours */
	uint16_t flags;
	int r;

	exfat_assert_excl(fs);
	if (!buf)
		return ST_NOMEM;
	r = exfat_dev_read(fs, 0, n, buf);
	if (r != ST_OK)
		goto out;
	if (!exfat_boot_sane(buf) ||
	    exfat_le32(buf + EXFAT_BS_SERIAL) != fs->serial) {
		/* Not the volume we mounted any more (media changed). */
		r = exfat_corrupt(fs, 0, "boot sector changed under the mount");
		goto out;
	}
	flags = exfat_le16(buf + EXFAT_BS_FLAGS);
	if (set_dirty)
		flags |= EXFAT_VF_DIRTY;
	else
		flags &= (uint16_t)~EXFAT_VF_DIRTY;
	exfat_put16(buf + EXFAT_BS_FLAGS, flags);
	if (update_percent && fs->free_clusters >= 0 && fs->cluster_count) {
		uint64_t used = (uint64_t)fs->cluster_count -
				(uint64_t)fs->free_clusters;

		buf[EXFAT_BS_PERCENT] =
			(uint8_t)((used * 100) / fs->cluster_count);
	}
	r = exfat_dev_write(fs, 0, n, buf, EXFAT_W_BOOT);
	if (r == ST_OK)
		r = exfat_dev_sync(fs);
out:
	return r;
}

/* ===========================================================================
 * 17. Commit, arming, clean marking
 *
 * Without a journal, the order of writes is what keeps the volume
 * consistent should the device stop at any moment:
 *
 *   1. file data of streams whose entry set changes (size grows only after
 *      the data it covers is stored);
 *   2. the FAT and bitmap units: allocations (bits set, chains linked) --
 *      ALLOCATE BEFORE REFERENCE;
 *   3. a device sync when anything was written since the last one;
 *   4. the queued entry sets (new names, sizes, deletions);
 *   5. only now, with the entry sets that dropped them on the device
 *      (another sync), the frees: bits cleared, truncation end marks set in
 *      the FAT -- DEREFERENCE BEFORE FREE; then, with the cleared bits on
 *      the device too, the discards.
 *
 * A stop between any two steps leaves at worst clusters marked used that
 * nothing references (fsck reports them as lost, nothing is wrong with any
 * file).  Freed clusters cannot be reused before step 5 (their bits stay
 * set), so no new data ever lands in a cluster an old entry set on the
 * device still names.
 *
 * Arming: the first mutation of a mount sets VolumeDirty on the device
 * (and syncs) before anything else is written; a clean sync / unmount /
 * read-only remount stores everything, syncs, clears VolumeDirty (and
 * updates PercentInUse) and disarms.  A mount that only reads writes
 * nothing at all.
 * =========================================================================== */

void exfat_stream_mark_dirty(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_assert_excl(fs);
	if (st->is_root)
		return;
	st->meta_dirty = 1;
	if (!st->on_dirty_list) {
		exfat_stream_hold(st);
		st->on_dirty_list = 1;
		st->dnext = fs->dirty_head;
		fs->dirty_head = st;
	}
}

static void exfat_gate(exfat_fs_t *fs, int reason);

/* Are the clusters of the volume's own structures -- the root directory,
 * the allocation bitmap, the upcase table -- marked used in the bitmap?
 * The allocator would hand them out otherwise.  Checked once, before the
 * first write; a bitmap that fails it makes the mount read-only. */
static int exfat_own_clusters_ok(exfat_fs_t *fs)
{
	const exfat_extent_t *e;
	uint32_t n;
	exfat_extent_t *up = NULL;
	uint32_t up_next = 0, up_cap = 0, up_ncl = 0;
	int bad = 0, r;

	for (int which = 0; which < 3 && !bad; which++) {
		if (which == 0) {
			e = fs->root->ext;
			n = fs->root->next;
		} else if (which == 1) {
			e = fs->bm_ext;
			n = fs->bm_next;
		} else {
			if (!fs->up_first || !fs->up_len)
				break;
			r = exfat_chain_decode(fs, fs->up_first, 0,
					       (fs->up_len + fs->cs - 1) >>
						       fs->cs_shift,
					       0, &up, &up_next, &up_cap,
					       &up_ncl);
			if (r != ST_OK) {
				bad = 1;
				break;
			}
			e = up;
			n = up_next;
		}
		for (uint32_t i = 0; i < n && !bad; i++)
			for (uint32_t k = 0; k < e[i].len && !bad; k++) {
				r = exfat_bm_test(fs, e[i].disk + k);
				if (r != 1)
					bad = 1;
			}
	}
	if (up)
		kfree(up);
	return !bad;
}

/* A mutation is about to change the volume: may it?  ST_OK when the mount
 * is writable; then the chokepoint arms it (VolumeDirty) right before the
 * first real write -- an operation that ends up changing nothing writes
 * nothing.  Metadata lock exclusive. */
int exfat_may_write(exfat_fs_t *fs)
{
	exfat_assert_excl(fs);
	if (fs->ro || fs->ro_latched)
		return ST_ROFS;
	if (!fs->have_bitmap)
		return ST_ROFS;
	if (!fs->meta_bits_checked) {
		if (!exfat_own_clusters_ok(fs)) {
			exfat_gate(fs, EXFAT_RO_OWN_FREE);
			kprintf("exfat: %s: read-only: %s\n", exfat_name_of(fs),
				exfat_ro_reason_text[EXFAT_RO_OWN_FREE]);
			return ST_ROFS;
		}
		fs->meta_bits_checked = 1;
	}
	fs->may_write = 1;
	return ST_OK;
}

/* Set VolumeDirty on the device and sync: from the chokepoint, at the first
 * real write after exfat_may_write().  Metadata lock exclusive. */
int exfat_arm_now(exfat_fs_t *fs)
{
	int r;

	exfat_assert_excl(fs);
	if (fs->armed)
		return ST_OK;
	__atomic_store_n(&fs->armed, 1, __ATOMIC_RELEASE);
	if (fs->was_dirty)
		return ST_OK; /* VolumeDirty is set already */
	r = exfat_boot_write_flags(fs, 1, 0);
	if (r != ST_OK) {
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		kprintf("exfat: %s: cannot mark the volume dirty (%d); refusing to write\n",
			exfat_name_of(fs), r);
		return r == ST_ROFS ? ST_ROFS : ST_IO;
	}
	return ST_OK;
}

/* Drop every deferred change without storing it (a latched read-only mount
 * cannot store anything).  Metadata lock exclusive. */
static void exfat_commit_abandon(exfat_fs_t *fs)
{
	for (unsigned i = 0; i < fs->ndirw; i++) {
		pagecache_put(fs->dirw[i].pg);
		exfat_stream_put(fs->dirw[i].d);
	}
	fs->ndirw = 0;
	fs->npf = 0;
	fs->pf_clusters = 0;
	fs->ndfat = 0;
	while (fs->dirty_head) {
		exfat_stream_t *st = fs->dirty_head;

		fs->dirty_head = st->dnext;
		st->dnext = NULL;
		st->on_dirty_list = 0;
		st->meta_dirty = 0;
		exfat_stream_put(st);
	}
}

/* Does `st' still have dirty pages in its noted range?  After the
 * commit's flush that means pages another write-back pass had taken (they
 * wait for the lock this task holds): the entry set that would cover them
 * must wait for them. */
static int exfat_stream_dirty_left(exfat_fs_t *fs, exfat_stream_t *st)
{
	unsigned long chain = exfat_chain_of(st);

	if (st->dhi < st->dlo)
		return 0;
	for (unsigned long p = st->dlo; p <= st->dhi; p++) {
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, chain, p);
		int dirty;

		if (!pg)
			continue;
		dirty = (__atomic_load_n(&pg->flags, __ATOMIC_ACQUIRE) &
			 PC_PAGE_DIRTY) != 0;
		pagecache_put(pg);
		if (dirty)
			return 1;
	}
	return 0;
}

/* Steps 2 to 4 of the commit alone: allocations, barrier, the queued entry
 * sets.  Used when the queue fills up while the commit itself is running
 * (storing many streams' entry sets): those sets are independent of each
 * other, and the frees (step 5) stay for the end of the commit, after
 * EVERY set that drops their clusters is on the device. */
int exfat_commit_queue(exfat_fs_t *fs)
{
	int r, r2;

	exfat_assert_excl(fs);
	r = exfat_mc_flush(fs);
	if (fs->ndirw && fs->barrier_needed) {
		r2 = exfat_dev_sync(fs);
		if (r == ST_OK)
			r = r2;
	}
	if (r != ST_OK)
		return r; /* the sets wait: their allocations are not stored */
	return exfat_dirw_flush(fs);
}

int exfat_commit(exfat_fs_t *fs)
{
	exfat_stream_t *retry = NULL;
	uint64_t freed = 0;
	int r = ST_OK, r2, alloc_ok, fr;

	exfat_assert_excl(fs);
	might_sleep();
	if (fs->in_commit) {
		/* Never nested: a commit inside a commit would free clusters
		 * before the outer one has stored the sets that drop them. */
		WARN_ON_ONCE(1);
		return ST_BUSY;
	}
	if (!fs->dirty_head && !fs->ndirw && !fs->npf && !fs->ndfat &&
	    !fs->mc_dirty)
		return ST_OK;
	if (fs->ro_latched || fs->ro || (!fs->armed && !fs->may_write)) {
		WARN_ON_ONCE(!fs->ro_latched && !fs->ro);
		exfat_commit_abandon(fs);
		return ST_ROFS;
	}
	fs->in_commit = 1;

	/* 1. Data of the changed streams, then their entry sets into the
	 *    queue -- only for streams whose data is all stored: a set never
	 *    describes data that is not on the device (a write error keeps
	 *    the old set, the stream stays dirty and the error is reported
	 *    by fsync / close / sync). */
	while (fs->dirty_head) {
		exfat_stream_t *st = fs->dirty_head;
		int keep = 0;

		fs->dirty_head = st->dnext;
		st->dnext = NULL;
		if (st->data_dirty) {
			uint64_t e0 = fs->io_errors;

			pagecache_flush_file(&fs->sb, exfat_chain_of(st));
			if (fs->io_errors != e0) {
				st->wb_err = ST_IO;
				keep = 1;
				if (r == ST_OK)
					r = ST_IO;
			} else if (exfat_stream_dirty_left(fs, st)) {
				keep = 1; /* next commit */
			} else {
				st->data_dirty = 0;
				st->dlo = 1;
				st->dhi = 0;
			}
		}
		if (keep) {
			st->dnext = retry;
			retry = st; /* keeps its hold and on_dirty_list */
			continue;
		}
		st->on_dirty_list = 0;
		if (st->meta_dirty) {
			st->meta_dirty = 0;
			r2 = exfat_stream_store(fs, st);
			if (r2 != ST_OK && r == ST_OK)
				r = r2;
		}
		st->grew = 0;
		exfat_stream_put(st);
	}
	/* 2. Allocations (bitmap, barrier, FAT). */
	alloc_ok = exfat_mc_flush(fs) == ST_OK;
	if (!alloc_ok && r == ST_OK)
		r = ST_IO;
	/* 3. Barrier. */
	if (alloc_ok && fs->ndirw && fs->barrier_needed) {
		alloc_ok = exfat_dev_sync(fs) == ST_OK;
		if (!alloc_ok && r == ST_OK)
			r = ST_IO;
	}
	/* 4. Entry sets -- not while the allocations they rely on are not
	 *    stored (they stay queued for the next commit). */
	if (alloc_ok) {
		r2 = exfat_dirw_flush(fs);
		if (r2 != ST_OK && r == ST_OK)
			r = r2;
	}
	/* 5. Frees, once nothing on the device references them: the FAT
	 *    end marks of truncated chains first, a barrier, then the
	 *    bitmap bits, a barrier, then the discards. */
	if (fs->npf || fs->ndfat) {
		if (r != ST_OK || fs->ndirw) {
			/* Not every dereferencing write made it: the clusters
			 * stay allocated (lost, never doubly used). */
			fs->needs_check = 1;
			fs->npf = 0;
			fs->pf_clusters = 0;
			fs->ndfat = 0;
			goto out;
		}
		if (fs->barrier_needed) {
			/* Nothing done yet: all of it is tried again by the
			 * next commit. */
			r = exfat_dev_sync(fs);
			if (r != ST_OK)
				goto out;
		}
		fr = ST_OK;
		for (unsigned i = 0; i < fs->ndfat && fr == ST_OK; i++) {
			struct exfat_fatw w = fs->dfat[i];
			uint64_t off = (uint64_t)w.cluster * 4;
			uint32_t unit = (uint32_t)(off / fs->mcu_bytes);
			int err;
			exfat_mcu_t *u;

			u = exfat_mc_get(fs, EXFAT_MC_FAT, unit, &err);
			if (!u) {
				fr = err;
				break;
			}
			off %= fs->mcu_bytes;
			exfat_put32(u->data + off, w.value);
			exfat_mc_dirty(fs, u, (unsigned)off, 4);
			exfat_mc_put(fs, u);
		}
		fs->ndfat = 0;
		if (fr == ST_OK)
			fr = exfat_mc_flush_region(fs, EXFAT_MC_FAT);
		if (fr == ST_OK && fs->barrier_needed && fs->npf)
			fr = exfat_dev_sync(fs);
		/* A bit is cleared only once the end marks are stored. */
		for (unsigned i = 0; i < fs->npf && fr == ST_OK; i++) {
			fr = exfat_bm_change(fs, fs->pf[i].start, fs->pf[i].len,
					     0);
			if (fr == ST_OK)
				freed += fs->pf[i].len;
		}
		exfat_free_count_add(fs, (int64_t)freed);
		if (fr == ST_OK)
			fr = exfat_mc_flush_region(fs, EXFAT_MC_BITMAP);
		if (fr == ST_OK && fs->opts.discard && fs->npf) {
			/* Discard only once the cleared bits are durable. */
			fr = exfat_dev_sync(fs);
			for (unsigned i = 0; i < fs->npf && fr == ST_OK; i++)
				exfat_dev_discard(
					fs,
					fs->heap_lba +
						((uint64_t)(fs->pf[i].start -
							    EXFAT_FIRST_CLUSTER)
						 << (fs->cs_shift - fs->dev_shift)),
					(uint64_t)fs->pf[i].len
						<< (fs->cs_shift - fs->dev_shift));
		}
		if (fr != ST_OK) {
			/* Every entry set is stored: the operations are done.
			 * What could not be finished is only the release of
			 * clusters nothing references any more -- a FAT chain
			 * longer than its file, clusters still marked used.
			 * Never doubly used; lost until the volume is checked,
			 * so it stays marked dirty. */
			fs->needs_check = 1;
			if (!fs->free_fail_logged) {
				fs->free_fail_logged = 1;
				kprintf("exfat: %s: freed clusters could not be released (%d): they stay allocated until the volume is checked\n",
					exfat_name_of(fs), fr);
			}
		}
		fs->npf = 0;
		fs->pf_clusters = 0;
	}
out:
	/* Streams whose data is not all stored stay dirty. */
	while (retry) {
		exfat_stream_t *st = retry;

		retry = st->dnext;
		st->dnext = fs->dirty_head;
		fs->dirty_head = st;
	}
	fs->in_commit = 0;
	fs->st_commits++;
	return r;
}

/* Store everything and mark the volume clean.  sync(2), unmount, remount
 * read-only.  A write error anywhere (data or metadata) leaves VolumeDirty
 * set and is returned.  Metadata lock exclusive. */
static int exfat_clean(exfat_fs_t *fs)
{
	uint64_t e0 = fs->io_errors;
	int r = ST_OK, r2;

	exfat_assert_excl(fs);
	if (!fs->armed && !fs->may_write) {
		/* No mutation was ever allowed: nothing to do -- and nothing
		 * may be written now. */
		if (fs->dirty_head || fs->ndirw || fs->npf || fs->ndfat)
			exfat_commit_abandon(fs);
		return ST_OK;
	}
	if (fs->ro_latched) {
		exfat_commit_abandon(fs);
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		fs->may_write = 0;
		return ST_IO;
	}
	/* Every dirty page of the volume first (data of streams not on the
	 * dirty list too); a stream whose pages another pass had taken is
	 * tried again. */
	for (int tries = 0; tries < 4; tries++) {
		pagecache_flush_sb(&fs->sb);
		r = exfat_commit(fs);
		if (r != ST_OK || !fs->dirty_head)
			break;
	}
	if (r == ST_OK && fs->dirty_head)
		r = ST_BUSY;
	r2 = exfat_mc_flush(fs);
	if (r == ST_OK)
		r = r2;
	if (fs->armed) {
		r2 = exfat_dev_sync(fs);
		if (r == ST_OK)
			r = r2;
	}
	if (r == ST_OK && fs->io_errors != e0)
		r = ST_IO;
	if (!fs->armed) {
		/* The mutations changed nothing on the device. */
		if (r == ST_OK)
			fs->may_write = 0;
		return r;
	}
	if (r == ST_OK && fs->needs_check && !fs->check_logged) {
		fs->check_logged = 1;
		kprintf("exfat: %s: clusters could not be released earlier: the volume stays marked dirty (check it)\n",
			exfat_name_of(fs));
	}
	if (r == ST_OK && !fs->errors_seen && !fs->was_dirty &&
	    !fs->ro_latched && !fs->needs_check)
		r = exfat_boot_write_flags(fs, 0, 1);
	if (r == ST_OK || fs->ro_latched) {
		__atomic_store_n(&fs->armed, 0, __ATOMIC_RELEASE);
		fs->may_write = 0;
	}
	return r;
}

/* ---- whole-filesystem operations ------------------------------------------ */

static int exfat_statfs_op(vfs_mount_t *m, struct vfs_statfs *out)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	int64_t fr;

	if (!fs || !out)
		return ST_INVALID;
	exfat_meta_rlock(fs);
	if (fs->have_bitmap)
		exfat_bm_count_free(fs);
	fr = __atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE);
	exfat_meta_runlock(fs);
	if (fr < 0)
		fr = 0;
	mm_memset(out, 0, sizeof(*out));
	out->f_type = EXFAT_STATFS_MAGIC;
	out->f_bsize = fs->cs;
	out->f_frsize = fs->cs;
	out->f_blocks = fs->cluster_count;
	out->f_bfree = (unsigned long)fr;
	out->f_bavail = (unsigned long)fr;
	out->f_files = 0;
	out->f_ffree = 0;
	out->f_fsid = fs->serial;
	out->f_namelen = EXFAT_NAME_MAX;
	return ST_OK;
}

static int exfat_sync_op(vfs_mount_t *m)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	int r;

	if (!fs)
		return ST_INVALID;
	exfat_meta_wlock(fs);
	r = exfat_clean(fs);
	exfat_meta_wunlock(fs);
	return r;
}

/* Access decisions: without ownership options every user may do what the
 * reported mode bits suggest for the owner (like FAT, the volume stores no
 * owners) -- except writing a read-only file, which the generic check of
 * the mode bits then decides (only the privileged pass).  With any
 * ownership option the generic check of uid/gid/mode decides. */
static int exfat_permission_op(vfs_mount_t *m, const char *path,
			       unsigned long ino, int want)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct kstat k;
	int r;

	(void)ino;
	if (!fs)
		return ST_UNSUPPORTED;
	if (fs->opts.perm_opts)
		return ST_UNSUPPORTED;
	if (!(want & MAY_WRITE) || !path)
		return ST_OK;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_OK;
	exfat_meta_rlock(fs);
	r = exfat_stat_locked(fs, ctx, path, &k);
	exfat_meta_runlock(fs);
	exfat_ctx_free(ctx);
	if (r == ST_OK && S_ISREG(k.st_mode) && !(k.st_mode & 0222))
		return ST_UNSUPPORTED;
	return ST_OK;
}

static int exfat_release_locks_op(vfs_mount_t *m, uint64_t task_id)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	int released = 0;

	if (!fs)
		return 0;
	released |= exfat_rwsem_release_owner(&fs->alloc, task_id);
	released |= exfat_rwsem_release_owner(&fs->meta, task_id);
	for (int i = 0; i < EXFAT_SLOCKS; i++)
		released |= exfat_rwsem_release_owner(&fs->slock[i], task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task)
		released |= fs->bdev->release_locks_for_task(fs->bdev, task_id);
	return released;
}

const vfs_ops_t exfat_vfs_ops = {
	.open = exfat_open_op,
	.open_mode = exfat_open_mode_op,
	.stat = exfat_stat_op,
	.lstat = exfat_stat_op,
	.fstat = exfat_fstat_op,
	.read = exfat_read_op,
	.read_at = exfat_read_at_op,
	.write = exfat_write_op,
	.write_at = exfat_write_at_op,
	.seek = exfat_seek_op,
	.readdir = exfat_readdir_op,
	.truncate = exfat_truncate_op,
	.unlink = exfat_unlink_op,
	.rename = exfat_rename_op,
	.mkdir = exfat_mkdir_op,
	.rmdir = exfat_rmdir_op,
	.chdir = exfat_chdir_op,
	.close = exfat_close_op,
	.release_locks_for_task = exfat_release_locks_op,
	.fsync = exfat_fsync_op,
	.chmod = exfat_chmod_op,
	.fchmod = exfat_fchmod_op,
	.utimens = exfat_utimens_op,
	.futimens = exfat_futimens_op,
	.statfs = exfat_statfs_op,
	.sync = exfat_sync_op,
	.permission = exfat_permission_op,
	/* No symbolic links, hard links, device nodes, owners or extended
	 * attributes on exFAT: those operations stay NULL and the VFS
	 * answers for them. */
};

/* ===========================================================================
 * 21. Mount options
 * =========================================================================== */

static int exfat_opt_num(const char *v, unsigned base, uint32_t *out)
{
	uint64_t n = 0;

	if (!v || !*v)
		return 0;
	for (; *v; v++) {
		unsigned d;

		if (*v >= '0' && *v <= '9')
			d = (unsigned)(*v - '0');
		else
			return 0;
		if (d >= base)
			return 0;
		n = n * base + d;
		if (n > 0xFFFFFFFFULL)
			return 0;
	}
	*out = (uint32_t)n;
	return 1;
}

static int exfat_opt_int(const char *v, int *out)
{
	int neg = 0;
	uint32_t n;

	if (!v)
		return 0;
	if (*v == '-' || *v == '+') {
		neg = *v == '-';
		v++;
	}
	if (!exfat_opt_num(v, 10, &n) || n > 100000)
		return 0;
	*out = neg ? -(int)n : (int)n;
	return 1;
}

/* Parse `data' over the values in `o' (comma separated, vfs_next_option).
 * Unknown options -- the generic flags among them -- are accepted and have
 * no effect.  ST_INVALID for a value that is not one; `o' is then
 * unchanged. */
static int exfat_parse_options(const char *data, struct exfat_opts *o)
{
	struct exfat_opts n = *o;
	const char *cur = data;
	char tmp[VFS_MOUNT_OPTS_MAX];
	const char *key, *val;
	uint32_t umask = 0, v;
	int have_umask = 0, have_fmask = 0, have_dmask = 0, iv;

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val)) {
		if (exfat_streq(key, "uid")) {
			if (!exfat_opt_num(val, 10, &v))
				return ST_INVALID;
			n.uid = v;
			n.perm_opts = 1;
		} else if (exfat_streq(key, "gid")) {
			if (!exfat_opt_num(val, 10, &v))
				return ST_INVALID;
			n.gid = v;
			n.perm_opts = 1;
		} else if (exfat_streq(key, "umask")) {
			if (!exfat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			umask = v;
			have_umask = 1;
			n.perm_opts = 1;
		} else if (exfat_streq(key, "fmask")) {
			if (!exfat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.fmask = v;
			have_fmask = 1;
			n.perm_opts = 1;
		} else if (exfat_streq(key, "dmask")) {
			if (!exfat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.dmask = v;
			have_dmask = 1;
			n.perm_opts = 1;
		} else if (exfat_streq(key, "allow_utime")) {
			if (!exfat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.allow_utime = v;
		} else if (exfat_streq(key, "iocharset")) {
			if (!val || (!exfat_streq(val, "utf8") &&
				     !exfat_streq(val, "utf-8")))
				return ST_INVALID;
			n.iocharset_given = 1;
		} else if (exfat_streq(key, "utf8")) {
			if (val && !exfat_streq(val, "1") &&
			    !exfat_streq(val, "yes") && !exfat_streq(val, "true"))
				return ST_INVALID;
		} else if (exfat_streq(key, "errors")) {
			if (!val)
				return ST_INVALID;
			if (exfat_streq(val, "remount-ro"))
				n.errors = EXFAT_ERRORS_RO;
			else if (exfat_streq(val, "continue"))
				n.errors = EXFAT_ERRORS_CONTINUE;
			else if (exfat_streq(val, "panic"))
				n.errors = EXFAT_ERRORS_PANIC;
			else
				return ST_INVALID;
		} else if (exfat_streq(key, "discard")) {
			n.discard = 1;
		} else if (exfat_streq(key, "nodiscard")) {
			n.discard = 0;
		} else if (exfat_streq(key, "force")) {
			n.force = 1;
		} else if (exfat_streq(key, "time")) {
			if (!val)
				return ST_INVALID;
			if (exfat_streq(val, "local"))
				n.time_local = 1;
			else if (exfat_streq(val, "utc"))
				n.time_local = 0;
			else
				return ST_INVALID;
		} else if (exfat_streq(key, "time_offset")) {
			if (!exfat_opt_int(val, &iv) || iv < -720 || iv > 840 ||
			    iv % 15)
				return ST_INVALID;
			n.time_offset = iv;
			n.time_local = 1;
		}
	}
	if (have_umask) {
		if (!have_fmask)
			n.fmask = umask;
		if (!have_dmask)
			n.dmask = umask;
	}
	*o = n;
	return ST_OK;
}

static void exfat_opts_default(struct exfat_opts *o)
{
	mm_memset(o, 0, sizeof(*o));
	o->fmask = EXFAT_DEFAULT_MASK;
	o->dmask = EXFAT_DEFAULT_MASK;
	o->allow_utime = 0xFFFFFFFFu;
	o->errors = EXFAT_ERRORS_RO;
}

/* "uid=..,gid=..,fmask=..,dmask=..,iocharset=utf8,errors=.." plus what is
 * set of allow_utime, discard, force, time. */
static long exfat_fst_show_options(vfs_mount_t *m, char *buf, long cap)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	static const char *const errs[] = { "remount-ro", "continue", "panic" };
	long n = 0;
	int k;

	if (!fs || !buf || cap < 1)
		return 0;
#define EXFAT_SHOW(...)                                                        \
	do {                                                                   \
		if (n < cap) {                                                 \
			k = ksnprintf(buf + n, (size_t)(cap - n), __VA_ARGS__); \
			if (k > 0)                                             \
				n += k;                                        \
		}                                                              \
	} while (0)
	EXFAT_SHOW("uid=%u,gid=%u,fmask=%04o,dmask=%04o", (unsigned)fs->opts.uid,
		   (unsigned)fs->opts.gid, (unsigned)fs->opts.fmask,
		   (unsigned)fs->opts.dmask);
	if (fs->opts.allow_utime != 0xFFFFFFFFu)
		EXFAT_SHOW(",allow_utime=%04o", (unsigned)fs->opts.allow_utime);
	EXFAT_SHOW(",iocharset=utf8,errors=%s",
		   errs[fs->opts.errors >= 0 && fs->opts.errors <= 2 ?
				fs->opts.errors :
				0]);
	if (fs->opts.discard)
		EXFAT_SHOW(",discard");
	if (fs->opts.force)
		EXFAT_SHOW(",force");
	if (fs->opts.time_local) {
		EXFAT_SHOW(",time=local");
		if (fs->opts.time_offset)
			EXFAT_SHOW(",time_offset=%d", fs->opts.time_offset);
	}
#undef EXFAT_SHOW
	if (n >= cap)
		n = cap - 1;
	buf[n] = 0;
	return n;
}

/* ===========================================================================
 * 22. Mount, unmount, remount, probe
 * =========================================================================== */

/* Read the boot sector of `bdev' and recognise it.  One bounded read. */
static int exfat_probe_boot(struct block_device *bdev)
{
	const block_device_t *b = bdev;
	unsigned long ss;
	uint8_t *buf;
	int ok = 0;

	might_sleep();
	if (!b || !b->read)
		return 0;
	ss = b->sector_size ? b->sector_size : 512;
	if (ss < 512 || ss > 4096 || (ss & (ss - 1)) || b->total_sectors < 2)
		return 0;
	buf = (uint8_t *)kalloc(ss);
	if (!buf)
		return 0;
	if (b->read((block_device_t *)b, 0, 1, buf) == ST_OK)
		ok = exfat_boot_sane(buf);
	kfree(buf);
	return ok;
}

/* Read a metadata stream of the root directory (the upcase table) into a
 * kalloc'd buffer of `len' bytes, through its cluster chain. */
static int exfat_read_meta_stream(exfat_fs_t *fs, uint32_t first, uint64_t len,
				  int nofat, uint8_t **out)
{
	exfat_extent_t *ext = NULL;
	uint32_t next = 0, cap = 0, ncl = 0;
	uint64_t want = (len + fs->cs - 1) >> fs->cs_shift, done = 0;
	uint8_t *buf, *cl;
	int r;

	*out = NULL;
	r = exfat_chain_decode(fs, first, nofat, want, 0, &ext, &next, &cap,
			       &ncl);
	if (r != ST_OK) {
		if (ext)
			kfree(ext);
		return r;
	}
	buf = (uint8_t *)kalloc((size_t)len);
	cl = (uint8_t *)kalloc(fs->cs > EXFAT_MAX_IO_BYTES ? EXFAT_MAX_IO_BYTES :
							     fs->cs);
	if (!buf || !cl) {
		if (buf)
			kfree(buf);
		if (cl)
			kfree(cl);
		kfree(ext);
		return ST_NOMEM;
	}
	for (uint32_t i = 0; i < next && done < len && r == ST_OK; i++) {
		for (uint32_t k = 0; k < ext[i].len && done < len && r == ST_OK;
		     k++) {
			uint64_t lba = fs->heap_lba +
				       ((uint64_t)(ext[i].disk + k -
						   EXFAT_FIRST_CLUSTER)
					<< (fs->cs_shift - fs->dev_shift));
			uint64_t in_cl = 0;

			while (in_cl < fs->cs && done < len && r == ST_OK) {
				uint64_t piece = exfat_min64(
					fs->cs - in_cl,
					exfat_min64(EXFAT_MAX_IO_BYTES,
						    exfat_round_up(len - done,
								   fs->dev_ss)));
				uint64_t copy;

				r = exfat_dev_read(fs,
						   lba + (in_cl >> fs->dev_shift),
						   piece >> fs->dev_shift, cl);
				copy = exfat_min64(piece, len - done);
				if (r == ST_OK)
					mm_memcpy(buf + done, cl, copy);
				done += copy;
				in_cl += piece;
			}
		}
	}
	kfree(cl);
	kfree(ext);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	return ST_OK;
}

/* Find and load the allocation bitmap, the upcase table and the label from
 * the root directory.  Metadata lock exclusive (mount). */
static int exfat_load_root_meta(exfat_fs_t *fs, int *bitmap_ok, int *upcase_ok)
{
	exfat_diter_t it;
	exfat_stream_t *root = fs->root;
	uint64_t slots = exfat_dir_slots(root);
	int r = ST_OK, found_up = 0;
	uint32_t up_first = 0, up_sum = 0;
	uint64_t up_len = 0;

	*bitmap_ok = 0;
	*upcase_ok = 0;
	exfat_diter_init(&it, fs, root);
	for (uint64_t i = 0; i < slots; i++) {
		const uint8_t *e = exfat_diter_get(&it, i);
		uint8_t t;

		if (!e) {
			r = it.err;
			break;
		}
		t = e[EXFAT_DE_TYPE];
		if (t == EXFAT_ET_EOD)
			break;
		if (t == EXFAT_ET_BITMAP && !*bitmap_ok) {
			int which = e[EXFAT_BITMAP_FLAGS] & 1;
			uint32_t first = exfat_le32(e + EXFAT_DE_FIRST_CLUSTER);
			uint64_t len = exfat_le64(e + EXFAT_DE_DATA_LENGTH);
			uint64_t need = ((uint64_t)fs->cluster_count + 7) / 8;
			uint32_t ncl;

			if (which != (fs->nfats == 2 ? fs->active_fat : 0))
				continue;
			if (len < need ||
			    len > ((uint64_t)fs->cluster_count << fs->cs_shift) ||
			    !exfat_cluster_ok(fs, first)) {
				kprintf("exfat: %s: allocation bitmap entry invalid (cluster %u, %llu bytes)\n",
					fs->bdev->name, first,
					(unsigned long long)len);
				continue;
			}
			fs->bm_first = first;
			fs->bm_bytes = len;
			{
				uint32_t cap = 0;

				r = exfat_chain_decode(
					fs, first, 0,
					(len + fs->cs - 1) >> fs->cs_shift, 0,
					&fs->bm_ext, &fs->bm_next, &cap, &ncl);
			}
			if (r != ST_OK) {
				fs->bm_next = 0;
				r = ST_OK;
				continue;
			}
			*bitmap_ok = 1;
		} else if (t == EXFAT_ET_UPCASE && !found_up) {
			found_up = 1;
			up_sum = exfat_le32(e + EXFAT_UPCASE_CHECKSUM);
			up_first = exfat_le32(e + EXFAT_DE_FIRST_CLUSTER);
			up_len = exfat_le64(e + EXFAT_DE_DATA_LENGTH);
			fs->up_first = up_first;
			fs->up_len = up_len;
		} else if (t == EXFAT_ET_LABEL) {
			unsigned n = e[EXFAT_LABEL_COUNT];

			if (n > EXFAT_LABEL_MAX)
				n = EXFAT_LABEL_MAX;
			for (unsigned k = 0; k < n; k++)
				fs->label[k] = exfat_le16(e + EXFAT_LABEL_CHARS +
							  2 * k);
			fs->label_len = n;
		}
		if ((t & EXFAT_ET_INUSE) && !(t & EXFAT_ET_SECONDARY) &&
		    !exfat_primary_single(t))
			i += e[EXFAT_DE_SECONDARY_COUNT];
	}
	exfat_diter_done(&it);
	if (r != ST_OK)
		return r;
	if (found_up) {
		uint8_t *raw = NULL;

		if (up_len == 0 || up_len > EXFAT_UPCASE_MAX_BYTES ||
		    (up_len & 1) || !exfat_cluster_ok(fs, up_first)) {
			kprintf("exfat: %s: upcase table entry invalid\n",
				fs->bdev->name);
		} else if (exfat_read_meta_stream(fs, up_first, up_len, 0,
						  &raw) == ST_OK) {
			if (exfat_table_checksum(0, raw, up_len) != up_sum)
				kprintf("exfat: %s: upcase table checksum mismatch\n",
					fs->bdev->name);
			else if (exfat_upcase_expand(raw, up_len, fs->upcase) != 0)
				kprintf("exfat: %s: upcase table malformed\n",
					fs->bdev->name);
			else
				*upcase_ok = 1;
			kfree(raw);
		}
	}
	if (!*upcase_ok)
		exfat_upcase_builtin(fs->upcase);
	return ST_OK;
}

/* Free every stream of an instance (unmount, failed mount).  Nothing is
 * open; the caches were released. */
static void exfat_streams_destroy(exfat_fs_t *fs)
{
	while (fs->reap_head) {
		exfat_stream_t *st = fs->reap_head;

		fs->reap_head = st->dnext;
		exfat_stream_free(st);
	}
	for (unsigned b = 0; b < EXFAT_KHASH; b++) {
		exfat_stream_t *st = fs->khash[b];

		while (st) {
			exfat_stream_t *n = st->hnext;

			exfat_stream_free(st);
			st = n;
		}
		fs->khash[b] = NULL;
	}
	fs->root = NULL;
	fs->lru_head = fs->lru_tail = NULL;
	fs->lru_count = 0;
	fs->dirty_head = NULL;
	if (fs->htab) {
		for (uint32_t i = 0; i < EXFAT_HDIR; i++)
			if (fs->htab[i])
				kfree(fs->htab[i]);
		kfree(fs->htab);
		fs->htab = NULL;
	}
}

static void exfat_fs_free(exfat_fs_t *fs)
{
	/* First: waits for a scan of this filesystem in progress. */
	vm_unregister_shrinker(&fs->shrinker);
	exfat_streams_destroy(fs);
	exfat_mc_destroy(fs);
	if (fs->upcase)
		kfree(fs->upcase);
	if (fs->boot_buf)
		kfree(fs->boot_buf);
	if (fs->bm_ext)
		kfree(fs->bm_ext);
	if (fs->pf)
		kfree(fs->pf);
	if (fs->dfat)
		kfree(fs->dfat);
	kfree(fs);
}

/* Record a reason the mount is read-only.  Every reason is kept (a bit
 * each): lifting one -- "force" lifts the dirty one, a read-write remount
 * the requested one -- leaves the mount read-only while any other holds. */
static void exfat_gate(exfat_fs_t *fs, int reason)
{
	fs->gates |= 1u << reason;
	fs->ro = 1;
}

/* The reasons in `mask' as one line ("a; b"). */
static void exfat_gates_text(unsigned mask, char *buf, unsigned long cap)
{
	unsigned long n = 0;

	buf[0] = 0;
	for (int i = 1; i < EXFAT_RO_NREASONS; i++) {
		int k;

		if (!(mask & (1u << i)) || n + 3 >= cap)
			continue;
		k = ksnprintf(buf + n, (size_t)(cap - n), "%s%s",
			      n ? "; " : "", exfat_ro_reason_text[i]);
		if (k > 0)
			n += (unsigned long)k;
		if (n >= cap)
			n = cap - 1;
	}
}

static int exfat_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			   unsigned long flags, const char *data)
{
	struct exfat_opts o;
	struct exfat_geo g;
	exfat_fs_t *fs;
	block_device_t *b = bdev;
	int from_backup = 0, bitmap_ok = 0, upcase_ok = 0, r;
	unsigned long ss;

	if (!m || !bdev)
		return ST_INVALID;
	if (!EXFAT_DRIVER_READY)
		return ST_UNSUPPORTED;
	exfat_opts_default(&o);
	if (exfat_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	ss = b->sector_size ? b->sector_size : 512;
	if (ss < 512 || ss > 4096 || !exfat_is_pow2(ss) || !b->read)
		return ST_INVALID;
	if (!exfat_probe_boot(bdev))
		return ST_NOT_FOUND;

	fs = (exfat_fs_t *)kcalloc(1, sizeof(*fs));
	if (!fs)
		return ST_NOMEM;
	fs->bdev = b;
	fs->mnt = m;
	fs->dev_ss = (unsigned)ss;
	fs->dev_shift = exfat_ilog2(ss);
	fs->opts = o;
	fs->free_clusters = -1;
	fs->alloc_cursor = EXFAT_FIRST_CLUSTER;
	fs->next_handle = EXFAT_HANDLE_FIRST_DYNAMIC;
	exfat_rwsem_init(&fs->meta, "exfat_meta");
	exfat_rwsem_init(&fs->alloc, "exfat_alloc");
	for (int i = 0; i < EXFAT_SLOCKS; i++)
		exfat_rwsem_init(&fs->slock[i], "exfat_stream");
	spinlock_init(&fs->st_lock, "exfat_streams");
	spinlock_init(&fs->mc_lock, "exfat_mcache");
	exfat_guard_init(fs);
	fs->sb.ops = &exfat_sb_ops;
	fs->sb.fs_private = fs;

	r = exfat_boot_load(fs, &g, &from_backup);
	if (r != ST_OK) {
		kfree(fs);
		return r == ST_NOMEM ? ST_NOMEM : ST_NOT_FOUND;
	}
	fs->fs_shift = g.bps_shift;
	fs->spc_shift = g.spc_shift;
	fs->cs_shift = g.bps_shift + g.spc_shift;
	fs->cs = 1UL << fs->cs_shift;
	if (fs->cs < fs->dev_ss ||
	    (((uint64_t)g.fat_offset << g.bps_shift) & (fs->dev_ss - 1)) ||
	    (((uint64_t)g.heap_offset << g.bps_shift) & (fs->dev_ss - 1))) {
		kprintf("exfat: %s: volume regions do not fall on %lu-byte device sectors\n",
			b->name, ss);
		kfree(fs);
		return ST_INVALID;
	}
	fs->bs_shift = fs->cs_shift < 12 ? fs->cs_shift : 12;
	fs->bs = 1UL << fs->bs_shift;
	fs->bpc_shift = fs->cs_shift - fs->bs_shift;
	fs->mcu_bytes = fs->bs;
	fs->cluster_count = g.cluster_count;
	fs->root_cluster = g.root_cluster;
	fs->serial = g.serial;
	fs->revision = g.revision;
	fs->vol_flags_mount = g.flags;
	fs->percent_mount = g.percent;
	fs->nfats = (int)g.nfats;
	fs->active_fat = (g.nfats == 2 && (g.flags & EXFAT_VF_ACTIVE_FAT)) ? 1 : 0;
	fs->vol_dev_sectors = exfat_min64((g.vol_length << g.bps_shift) >>
						  fs->dev_shift,
					  b->total_sectors);
	fs->fat_lba = (((uint64_t)g.fat_offset +
			(uint64_t)fs->active_fat * g.fat_length)
		       << g.bps_shift) >>
		      fs->dev_shift;
	fs->fat_bytes = (uint64_t)g.fat_length << g.bps_shift;
	fs->heap_lba = ((uint64_t)g.heap_offset << g.bps_shift) >> fs->dev_shift;

	/* Read-only gates: one log line, the mount still works. */
	if (flags & VFS_MS_RDONLY)
		exfat_gate(fs, EXFAT_RO_REQUESTED);
	if (blk_bdev_read_only(b))
		exfat_gate(fs, EXFAT_RO_DEVICE);
	if (from_backup)
		exfat_gate(fs, EXFAT_RO_BACKUP_BOOT);
	if ((g.revision >> 8) != 1)
		exfat_gate(fs, EXFAT_RO_REVISION);
	if (g.nfats != 1)
		exfat_gate(fs, EXFAT_RO_TWO_FATS);
	if (g.flags & EXFAT_VF_MEDIA_FAILURE)
		exfat_gate(fs, EXFAT_RO_MEDIA_FAILURE);
	if (g.flags & EXFAT_VF_DIRTY) {
		if (o.force)
			fs->was_dirty = 1;
		else
			exfat_gate(fs, EXFAT_RO_DIRTY);
	}
	if ((fs->fat_bytes & (fs->dev_ss - 1)) ||
	    (((uint64_t)g.heap_offset + ((uint64_t)g.cluster_count
					 << g.spc_shift))
	     << g.bps_shift) > (uint64_t)b->total_sectors * fs->dev_ss)
		exfat_gate(fs, EXFAT_RO_GEOMETRY);

	fs->htab = (exfat_stream_t ***)kcalloc(EXFAT_HDIR, sizeof(*fs->htab));
	fs->upcase = (uint16_t *)kalloc(EXFAT_UPCASE_ENTRIES * sizeof(uint16_t));
	/* The boot sector buffer of VolumeDirty updates, ready: arming (the
	 * first write of an operation) never fails for want of memory. */
	fs->boot_buf = (uint8_t *)kalloc((size_t)(exfat_boot_dev_sectors(fs)
						  << fs->dev_shift));
	if (!fs->htab || !fs->upcase || !fs->boot_buf) {
		exfat_fs_free(fs);
		return ST_NOMEM;
	}

	/* The root directory stream. */
	exfat_meta_wlock(fs);
	{
		exfat_stream_t *root =
			(exfat_stream_t *)kcalloc(1, sizeof(*root));

		if (!root) {
			r = ST_NOMEM;
			goto fail_locked;
		}
		root->fs = fs;
		root->is_root = 1;
		root->is_dir = 1;
		root->attr = EXFAT_ATTR_DIR;
		root->key = EXFAT_KEY_ROOT;
		root->first_cluster = fs->root_cluster;
		root->subdirs = -1;
		r = exfat_stream_decode(fs, root);
		if (r == ST_OK)
			r = exfat_handle_alloc(fs, root);
		if (r != ST_OK) {
			if (root->ext)
				kfree(root->ext);
			kfree(root);
			kprintf("exfat: %s: root directory unreadable (%d)\n",
				b->name, r);
			r = ST_IO;
			goto fail_locked;
		}
		root->refs = 1; /* the instance's */
		exfat_khash_insert_locked(fs, root);
		fs->root = root;
		fs->nstreams = 1;
	}
	r = exfat_load_root_meta(fs, &bitmap_ok, &upcase_ok);
	if (r != ST_OK) {
		kprintf("exfat: %s: root directory unreadable (%d)\n", b->name,
			r);
		r = ST_IO;
		goto fail_locked;
	}
	fs->have_bitmap = bitmap_ok;
	if (!bitmap_ok)
		exfat_gate(fs, EXFAT_RO_NO_BITMAP);
	if (!upcase_ok) {
		fs->upcase_builtin = 1;
		exfat_gate(fs, EXFAT_RO_UPCASE);
	}
	/* A metadata read error at mount latches nothing: errors are
	 * counted from here on. */
	fs->errors_seen = 0;
	fs->ro_latched = 0;
	exfat_meta_wunlock(fs);

	if (fs->ro && !(flags & VFS_MS_RDONLY)) {
		char why[256];

		exfat_gates_text(fs->gates & ~(1u << EXFAT_RO_REQUESTED), why,
				 sizeof(why));
		kprintf("exfat: %s: mounted read-only: %s\n", b->name, why);
		m->flags |= VFS_MS_RDONLY;
	}
	if (fs->was_dirty)
		kprintf("exfat: %s: volume was not cleanly unmounted; mounted read-write because of 'force' (it stays marked dirty)\n",
			b->name);
	exfat_shrinker_register(fs);
	m->ops = &exfat_vfs_ops;
	m->fs_data = fs;
	m->sb = &fs->sb;
	return ST_OK;

fail_locked:
	exfat_meta_wunlock(fs);
	vfs_sb_release_caches(&fs->sb);
	exfat_fs_free(fs);
	return r;
}

/* The end of an unmount: nothing is open, the VFS has stored the pages and
 * inodes.  Store what is left, mark the volume clean, let go of every cache
 * entry and free the instance. */
static int exfat_fst_unmount(vfs_mount_t *m)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	int r;

	if (!fs)
		return ST_INVALID;
	exfat_meta_wlock(fs);
	r = exfat_clean(fs);
	/* Whatever could not be stored is lost now: the device is going. */
	if (fs->dirty_head || fs->ndirw || fs->npf || fs->ndfat)
		exfat_commit_abandon(fs);
	exfat_meta_wunlock(fs);
	if (r != ST_OK)
		kprintf("exfat: %s: could not store everything before unmount (%d)\n",
			exfat_name_of(fs), r);
	vfs_sb_release_caches(&fs->sb);
	exfat_fs_free(fs);
	m->fs_data = NULL;
	m->sb = NULL;
	return ST_OK;
}

/* Read-only <-> read-write and new options.  Going read-only stores
 * everything and marks the volume clean (the VFS refuses while files are
 * open for writing); going read-write is refused when a gate still holds. */
static int exfat_fst_remount(vfs_mount_t *m, unsigned long flags,
			     const char *data)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	struct exfat_opts o;
	int want_ro = (flags & VFS_MS_RDONLY) != 0;
	int r = ST_OK;

	if (!fs)
		return ST_INVALID;
	o = fs->opts;
	if (data && exfat_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	exfat_meta_wlock(fs);
	if (want_ro && !fs->ro && fs->orphans_held && !fs->ro_latched) {
		/* Unlinked files still open: their clusters are freed at the
		 * last close, which a read-only mount could not store -- they
		 * would stay allocated on a volume marked clean. */
		exfat_meta_wunlock(fs);
		return ST_BUSY;
	}
	if (want_ro && !fs->ro) {
		r = exfat_clean(fs);
		if (r != ST_OK && !fs->ro_latched) {
			exfat_meta_wunlock(fs);
			return r;
		}
		exfat_gate(fs, EXFAT_RO_REQUESTED);
		r = ST_OK;
	} else if (!want_ro && fs->ro) {
		unsigned left = fs->gates & ~(1u << EXFAT_RO_REQUESTED);

		/* "force" lifts the dirty gate only; the device's write
		 * protection is asked again. */
		if (o.force)
			left &= ~(1u << EXFAT_RO_DIRTY);
		if (blk_bdev_read_only(fs->bdev))
			left |= 1u << EXFAT_RO_DEVICE;
		else
			left &= ~(1u << EXFAT_RO_DEVICE);
		if (fs->ro_latched) {
			r = ST_ROFS;
		} else if (left) {
			char why[256];

			exfat_gates_text(left, why, sizeof(why));
			kprintf("exfat: %s: stays read-only: %s\n",
				exfat_name_of(fs), why);
			r = ST_ROFS;
		} else {
			if (fs->gates & (1u << EXFAT_RO_DIRTY))
				fs->was_dirty = 1;
			fs->gates = 0;
			fs->ro = 0;
		}
	}
	if (r == ST_OK)
		fs->opts = o;
	exfat_meta_wunlock(fs);
	return r;
}

static int exfat_fst_probe(struct block_device *bdev)
{
	return EXFAT_DRIVER_READY && exfat_probe_boot(bdev);
}

const vfs_fstype_t exfat_fstype = {
	.name = "exfat",
	.aliases = NULL,
	.needs_device = 1,
	.mount = exfat_fst_mount,
	.unmount = exfat_fst_unmount,
	.remount = exfat_fst_remount,
	.show_options = exfat_fst_show_options,
	.probe = exfat_fst_probe,
};
