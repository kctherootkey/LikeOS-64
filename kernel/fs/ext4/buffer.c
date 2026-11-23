// LikeOS - ext4 filesystem driver: device I/O of metadata blocks and the metadata
// block cache
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Low-level disk helpers
 * =================================================================== */

/* Read `count` device sectors (of bdev->sector_size bytes) at absolute
 * `lba' (chunked for the USB DMA limit, exactly like fat32's read_sectors). */
int ext4_read_sectors(const block_device_t *bdev, unsigned long lba,
			     unsigned long count, void *buf)
{
	BUG_ON(bdev == NULL);
	BUG_ON(buf == NULL);
	might_sleep();
	unsigned long off = 0;
	unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
	while (count > 0) {
		unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ) ?
					      EXT4_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->read((block_device_t *)bdev, lba, chunk,
				    (uint8_t *)buf + off);
		if (st != ST_OK)
			return st;
		lba += chunk;
		off += chunk * ss;
		count -= chunk;
	}
	return ST_OK;
}

/* The superblock is the 1024 bytes at byte EXT4_SUPERBLOCK_OFFSET of the
 * filesystem, whatever the device's sector size.  With sectors of up to 1024
 * bytes that is a whole number of sectors of its own; with 2048- or 4096-byte
 * sectors it is a part of the first sector (bytes 1024..2047 of it), which
 * has to be read whole and the superblock taken from inside it.  `part_lba':
 * the sector the filesystem starts at.  Copies the superblock to `out'. */
int ext4_sb_read_raw(const block_device_t *bdev, unsigned long part_lba,
			    void *out)
{
	unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
	unsigned long lba = part_lba + EXT4_SUPERBLOCK_OFFSET / ss;
	unsigned in = EXT4_SUPERBLOCK_OFFSET % ss;
	unsigned n = (in + sizeof(ext4_super_block) + ss - 1) / ss;
	uint8_t *raw = (uint8_t *)kalloc((unsigned long)n * ss);
	int st;

	if (!raw)
		return ST_NOMEM;
	st = ext4_read_sectors(bdev, lba, n, raw);
	if (st == ST_OK)
		mm_memcpy(out, raw + in, sizeof(ext4_super_block));
	kfree(raw);
	return st;
}

/* ---- Metadata block cache ----
 * ext4 reads inode-table blocks, group-descriptor blocks, directory blocks
 * and extent/indirect nodes constantly, and the same block is re-read many
 * times in one operation (e.g. `ls -la` stats every entry, each re-reading
 * the parent dir block + the inode-table block).  Without caching, each is a
 * separate ~4KB USB transaction — painfully slow.  This small round-robin
 * cache keeps the hottest metadata blocks in RAM.  All ext4_read_block()
 * calls happen under the I/O mutex, so the cache needs no extra locking.
 * NOTE: entries are invalidated on writes. */
/* Sized so a metadata-heavy operation's whole working set stays resident: a big
 * directory grows to many leaf/index blocks, and a create/delete loop touches
 * dozens of inode-table + bitmap + dir blocks.  256 * 4 KB = up to 1 MB, buffers
 * allocated on demand.  (Was 32, which thrashed on htree-sized directories.) */
/* The metadata block cache is read AND repopulated by concurrent shared-mode
 * readers (mutators hold the metadata lock exclusive, but reader–reader
 * insertions race), so it carries its own spinlock.  Copies in/out happen
 * under the lock (a block-size memcpy — sub-microsecond). */

/* ---- checksum-verification cache ----
 * Metadata checksums (directory leaves, extent nodes, xattr blocks) were
 * re-verified on EVERY read of the block — a directory leaf is re-checked on
 * every lookup that scans it, so hot directories paid a full-block crc32c per
 * path component per syscall.  The block cache remembers when a cached copy
 * has already passed its type-specific check; the flag is cleared whenever the
 * cached content changes (fresh disk read, write-through update). */
int ext4_mbc_verified(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	int v = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn) {
			v = fs->st->mbc[i].verified;
			break;
		}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return v;
}

void ext4_mbc_mark_verified(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	/* All matches, not just the first — symmetric with ext4_mbc_drop, so a
	 * duplicate entry (should no longer exist; insert dedupes) can never
	 * diverge in verified-state from the copy lookups serve. */
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn)
			fs->st->mbc[i].verified = 1;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

void ext4_mbc_invalidate(ext4_fs_t *fs)
{
	/* Detach the buffers under the lock, free them after releasing it —
	 * kfree of a block-sized buffer does a TLB shootdown and must not run
	 * with IRQs off under a spinlock. */
	uint8_t *bufs[EXT4_MBC_ENTRIES];
	uint64_t flags;
	long freed = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		fs->st->mbc[i].pbn = 0;
		bufs[i] = fs->st->mbc[i].data;
		fs->st->mbc[i].data = 0;
		freed += fs->st->mbc[i].size;
		fs->st->mbc[i].size = 0;
	}
	fs->st->mbc_next = 0;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	mm_buffercache_account(-freed);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (bufs[i])
			kfree(bufs[i]);
}

/* Drop a single block from the metadata cache (on free, so a later reuse of
 * the same physical block never serves stale cached content). */
void ext4_mbc_drop(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn)
			fs->st->mbc[i].pbn = 0;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

/* Serve `pbn` from the cache into `buf`.  Returns 1 on hit. */
static int ext4_mbc_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	uint64_t flags;
	int hit = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
			mm_memcpy(buf, fs->st->mbc[i].data, fs->block_size);
			hit = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return hit;
}

/* Insert a freshly-read block.  `verified` seeds the csum-checked flag. */
static void ext4_mbc_insert(ext4_fs_t *fs, unsigned long pbn, const void *buf,
			    int verified)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	/* DEDUPE — never create a second entry for the same pbn.  Two
	 * shared-mode readers (fine-grained locking allows concurrent readers)
	 * can both miss the same block, both sit in the sleeping disk read, and
	 * both arrive here.  Blind slot assignment then caches the block TWICE.
	 * Duplicates are fatal in slow motion: lookup and the write-side refresh
	 * in ext4_write_block_direct touch only the FIRST match, so the second
	 * copy keeps its old content; once round-robin reuse evicts the first
	 * (fresh) copy, the stale duplicate becomes authoritative, and the next
	 * inode/bitmap RMW through it durably REVERTS every update made since
	 * the duplicate was created (observed: /tmp htree leaf loss + ~78 lost
	 * links_count increments + lost bitmap bits, e2fsck).  Update the
	 * existing entry in place instead.
	 *
	 * INVARIANT: both racing readers read the same disk content (anything
	 * newer lives in st->txn/st->ckpt, checked before the mbc; mutators hold
	 * the meta lock exclusive), so an existing entry MUST equal what we
	 * just read.  A mismatch means a stale/incoherent cached metadata
	 * block — the corruption, caught at the moment it exists. */
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
			WARN_RATELIMIT(
				ext4_memcmp(fs->st->mbc[i].data, buf,
					    fs->block_size) != 0,
				"ext4 mbc: cached pbn %lu differs from disk (stale metadata cache entry — corruption)",
				pbn);
			mm_memcpy(fs->st->mbc[i].data, buf, fs->block_size);
			fs->st->mbc[i].verified = verified;
			spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
			return;
		}
	}
	unsigned slot = fs->st->mbc_next++ % EXT4_MBC_ENTRIES;
	if (!fs->st->mbc[slot].data) {
		/* One-time buffer allocation for this slot.  kalloc's
		 * allocation path takes only spinlocks (no sleep, no TLB
		 * shootdown — that is on the free path), so this is safe
		 * with IRQs off; it happens once per slot per mount. */
		fs->st->mbc[slot].data = (uint8_t *)kalloc(fs->block_size);
		if (!fs->st->mbc[slot].data) {
			spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
			return;
		}
		fs->st->mbc[slot].size = fs->block_size;
		mm_buffercache_account((long)fs->block_size);
	}
	mm_memcpy(fs->st->mbc[slot].data, buf, fs->block_size);
	fs->st->mbc[slot].pbn = pbn;
	fs->st->mbc[slot].verified = verified;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

/* Read one filesystem block (block_size bytes) by physical block number,
 * serving from / populating the metadata block cache. */
int ext4_read_block(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	if (pbn == 0)
		return ST_INVALID;
	/* Read-your-writes: the current op's buffer wins, then the epoch's committed-
     * but-not-yet-checkpointed metadata (whose final disk location is still
     * stale), then the cache, then disk. */
	/* st->txn/st->ckpt are only mutated under the metadata lock held EXCLUSIVE;
	 * every caller of this function holds it at least SHARED, so these
	 * lookups need no further locking.  The mbc is repopulated by
	 * concurrent shared readers and locks internally. */
	if (fs->st->txn.active && ext4_txn_lookup(fs, pbn, buf))
		return ST_OK;
	if (fs->st->ckpt.n && ext4_ckpt_lookup(fs, pbn, buf))
		return ST_OK;
	if (ext4_mbc_lookup(fs, pbn, buf))
		return ST_OK;
	unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
	int st = ext4_read_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
	if (st != ST_OK)
		return st;
	ext4_mbc_insert(fs, pbn, buf, 0); /* fresh from disk: not yet checked */
	return ST_OK;
}

/* ---- recycled scratch block buffers --------------------------------------
 * A block-sized kalloc is a "large" slab allocation: it takes 2 contiguous
 * physical pages (bitmap scan under a global lock), page-table maps them, and
 * the matching kfree unmaps with a cross-CPU TLB shootdown.  The hot metadata
 * paths (resolve, dir lookup, inode read, xattr/ACL read) each burn several of
 * those per operation, which made every path syscall cost milliseconds.  These
 * scratch buffers are allocated once and recycled instead.  Guarded by a
 * spinlock so early-boot/mount callers outside ext4_io_lock stay safe; when
 * the pool is exhausted (or a remount grew block_size) we fall back to kalloc,
 * and ext4_bput(fs) routes foreign pointers back to kfree. */

void *ext4_bget(ext4_fs_t *fs)
{
	unsigned want = fs->block_size;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->bpool_lock, &flags);
	for (int i = 0; i < EXT4_BPOOL; i++) {
		if (fs->st->bpool[i].busy)
			continue;
		if (fs->st->bpool[i].data && fs->st->bpool[i].size < want) {
			mm_buffercache_account(-(long)fs->st->bpool[i].size);
			kfree(fs->st->bpool[i].data); /* stale from a smaller mount */
			fs->st->bpool[i].data = 0;
		}
		if (!fs->st->bpool[i].data) {
			spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
			uint8_t *nb = (uint8_t *)kalloc(want);
			if (!nb)
				return 0;
			spin_lock_irqsave(&fs->st->bpool_lock, &flags);
			if (fs->st->bpool[i].busy || fs->st->bpool[i].data) {
				/* raced: someone claimed the slot meanwhile */
				spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
				return nb; /* use it as a foreign buffer */
			}
			fs->st->bpool[i].data = nb;
			fs->st->bpool[i].size = want;
			mm_buffercache_account((long)want);
		}
		fs->st->bpool[i].busy = 1;
		uint8_t *p = fs->st->bpool[i].data;
		spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
		mm_memset(p, 0, want); /* match kalloc's zeroed contract */
		return p;
	}
	spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
	return kalloc(want); /* pool exhausted: degrade gracefully */
}

void ext4_bput(ext4_fs_t *fs, void *p)
{
	if (!p)
		return;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->bpool_lock, &flags);
	for (int i = 0; i < EXT4_BPOOL; i++)
		if (fs->st->bpool[i].data == (uint8_t *)p) {
			WARN_ON_ONCE(!fs->st->bpool[i].busy);
			fs->st->bpool[i].busy = 0;
			spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
			return;
		}
	spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
	kfree(p); /* fallback allocation */
}

/* ===================================================================
 * Write support — allocation, metadata writeback, file lifecycle.
 *
 * Metadata (inodes, bitmaps, group descs, directory blocks) is written via
 * ext4_write_block (write-through to the metadata cache).  File data bypasses
 * that cache and uses raw sector I/O, with the pagecache invalidated after a
 * write so reads re-fetch fresh data.  All of this runs under ext4_io_lock.
 * Perf note: metadata writeback here is eager (not batched the way fat32
 * defers FAT-sector flushes).
 * =================================================================== */

int ext4_write_sectors(const block_device_t *bdev, unsigned long lba,
			      unsigned long count, const void *buf)
{
	if (!bdev || !bdev->write)
		return ST_UNSUPPORTED;
	might_sleep();
	unsigned long off = 0;
	unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
	while (count > 0) {
		unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ) ?
					      EXT4_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->write((block_device_t *)bdev, lba, chunk,
				     (const uint8_t *)buf + off);
		if (st != ST_OK)
			return st;
		lba += chunk;
		off += chunk * ss;
		count -= chunk;
	}
	return ST_OK;
}

/* The last check before a block is written: it has to be inside the
 * filesystem (whose size the mount checked against the device).  Block
 * numbers from the disk are checked where they are read (ext4_blk_valid);
 * one that gets here outside the filesystem is a damaged value all the
 * same, reported, and the write is refused. */
int ext4_write_target_ok(ext4_fs_t *fs, unsigned long pbn, unsigned long count)
{
	if (pbn != 0 && count != 0 && pbn + count > pbn &&
	    pbn + count <= fs->blocks_count)
		return 1;
	ext4_bad_block(fs, 0, "write", pbn, count);
	return 0;
}

/* Write a metadata block straight to its final location, keeping any cached
 * copy coherent.  Bypasses the journal — used for checkpointing and outside
 * of transactions. */
int ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn,
				   const void *buf)
{
	if (pbn == 0)
		return ST_INVALID;
	if (fs->read_only) /* error latch / read-only mount: refuse writes */
		return ST_ROFS;
	if (!ext4_write_target_ok(fs, pbn, 1))
		return ST_INVALID;
	unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
	int st = ext4_write_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
	if (st != ST_OK)
		return st;
	{
		/* Refresh EVERY cached copy, not just the first match (mirror of
		 * ext4_mbc_drop).  Refreshing only the first left a duplicate
		 * entry stale forever — the delayed-revert corruption (see the
		 * dedupe comment in ext4_mbc_insert).  ext4_mbc_insert now
		 * dedupes, so >1 match means duplicate creation slipped through
		 * some path — surface it. */
		int matches = 0;
		uint64_t flags;
		spin_lock_irqsave(&fs->st->mbc_lock, &flags);
		for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
			if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
				mm_memcpy(fs->st->mbc[i].data, buf, fs->block_size);
				fs->st->mbc[i].verified = 0; /* content changed */
				matches++;
			}
		spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
		WARN_ON_ONCE(
			matches >
			1); /* duplicate mbc entries for one pbn: insert dedup bypassed */
	}
	return ST_OK;
}

/* Write a metadata block.  Inside an active journaled transaction this only
 * buffers the block (it is journaled + checkpointed atomically at commit);
 * otherwise it goes straight to its final location. */
int ext4_write_block(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	if (pbn == 0)
		return ST_INVALID;
	if (!ext4_write_target_ok(fs, pbn, 1))
		return ST_INVALID;
	if (fs->st->txn.active && fs && fs->j_enabled) {
		ext4_txn_capture(fs, pbn, buf);
		return ST_OK;
	}
	return ext4_write_block_direct(fs, pbn, buf);
}

/* Store the superblock (see ext4_sb_read_raw for where it lives).  On a
 * device whose sectors are larger than the superblock the sector holding it
 * is read, patched and written back, so the bytes around it (the boot area
 * before it, the rest of block 0 after it) stay what they are. */
int ext4_write_super(ext4_fs_t *fs)
{
	if (fs->has_metadata_csum)
		fs->sb_copy.s_checksum = ext4_sb_csum(&fs->sb_copy);
	unsigned ss = fs->bdev->sector_size ? fs->bdev->sector_size : 512;
	unsigned long lba = fs->part_lba_offset + EXT4_SUPERBLOCK_OFFSET / ss;
	unsigned in = EXT4_SUPERBLOCK_OFFSET % ss;
	unsigned sects = (in + sizeof(ext4_super_block) + ss - 1) / ss;
	if (in == 0 && sizeof(ext4_super_block) % ss == 0)
		return ext4_write_sectors(fs->bdev, lba, sects, &fs->sb_copy);

	uint8_t *raw = (uint8_t *)kalloc((unsigned long)sects * ss);
	int st;

	if (!raw)
		return ST_NOMEM;
	st = ext4_read_sectors(fs->bdev, lba, sects, raw);
	if (st == ST_OK) {
		mm_memcpy(raw + in, &fs->sb_copy, sizeof(ext4_super_block));
		st = ext4_write_sectors(fs->bdev, lba, sects, raw);
	}
	kfree(raw);
	return st;
}

/* ---- Device cache-flush barrier -------------------------------------------
 *
 * The journal's crash-safety is an ordering argument — commit durable before
 * checkpoint, checkpoint durable before s_start=0 — and each "durable" step is
 * one bdev->sync (SCSI SYNCHRONIZE CACHE 10 on USB MSD).
 *
 * Not every device implements that command.  VMware's virtual USB storage
 * refuses it with ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE; QEMU
 * honours it.  A refusal is not automatically unsafe: a device with no volatile
 * write cache has nothing to flush, and its writes are already durable when the
 * command completes, so the ordering above still holds without any flush.  Only
 * a device that caches AND refuses to flush is unsafe, and we cannot tell those
 * apart from here (such devices tend not to implement MODE SENSE either, and
 * probing it wedges VMware's bulk endpoint).
 *
 * So: probe once at mount and simply stop re-issuing a command the device has
 * already refused — the callers all used to discard the result anyway, which
 * cost a failing USB round-trip on every barrier. */

int ext4_dev_sync(ext4_fs_t *fs, const char *where)
{
	if (!fs || !fs->bdev || !fs->bdev->sync || fs->st->flush_unsupported)
		return ST_OK;
	int rc = fs->bdev->sync((block_device_t *)fs->bdev);
	if (rc != ST_OK)
		WARN_RATELIMIT(1, "ext4: device flush failed (%s, rc=%d)", where,
			       rc);
	return rc;
}

/* Probe cache-flush support once, before any barrier can run, and latch it. */
void ext4_probe_dev_sync(ext4_fs_t *fs)
{
	fs->st->flush_unsupported = 0;
	if (!fs || !fs->bdev || !fs->bdev->sync) {
		fs->st->flush_unsupported = 1;
		return;
	}
	if (fs->bdev->sync((block_device_t *)fs->bdev) != ST_OK) {
		fs->st->flush_unsupported = 1;
		kprintf("ext4: device implements no cache flush; relying on write completion for ordering\n");
	}
}
