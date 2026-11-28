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
 * separate ~4KB USB transaction — painfully slow.
 *
 * The cache keeps every metadata block it has read, for as long as memory
 * allows: a hash on the block number finds a block, an LRU list (most
 * recently used first) says which blocks are cold, and under memory
 * pressure the reclaim thread frees the coldest through the cache's
 * shrinker.  It used to be 256 slots reused round robin, which on any
 * directory tree of real size re-read the same blocks from the device over
 * and over while memory sat unused.  A ceiling of a sixteenth of memory
 * keeps one filesystem from crowding out everything else between two
 * reclaim passes.
 *
 * The cache is read AND repopulated by concurrent shared-mode readers
 * (mutators hold the metadata lock exclusive, but reader–reader insertions
 * race), so it carries its own spinlock.  Copies in/out happen under the
 * lock (a block-size memcpy — sub-microsecond).  Memory is never allocated
 * or freed under it: a buffer is allocated before the lock is taken and
 * freed after it is dropped (freeing a kalloc'd block buffer can shoot down
 * other processors' TLBs, which must not happen with interrupts off).
 *
 * A block-sized buffer that is a whole page comes straight from the page
 * allocator and is reached through the direct map: a kalloc of 4 KB is a
 * "large" slab allocation costing two pages and a mapping of its own. */

static inline unsigned long ext4_mbc_bucket(unsigned long pbn)
{
	return (pbn * 2654435761UL) & (EXT4_MBC_HASH - 1);
}

static void ext4_mbc_lru_unlink(struct ext4_mbc_ent *e)
{
	e->lru_prev->lru_next = e->lru_next;
	e->lru_next->lru_prev = e->lru_prev;
	e->lru_prev = e->lru_next = NULL;
}

static void ext4_mbc_lru_push(struct ext4_state *st, struct ext4_mbc_ent *e)
{
	e->lru_next = st->mbc_lru.lru_next;
	e->lru_prev = &st->mbc_lru;
	st->mbc_lru.lru_next->lru_prev = e;
	st->mbc_lru.lru_next = e;
}

/* The entry for `pbn', or NULL.  Caller holds mbc_lock. */
static struct ext4_mbc_ent *ext4_mbc_find(struct ext4_state *st,
					  unsigned long pbn)
{
	struct ext4_mbc_ent *e;

	if (!st->mbc_hash)
		return NULL;
	for (e = st->mbc_hash[ext4_mbc_bucket(pbn)]; e; e = e->hnext)
		if (e->pbn == pbn)
			return e;
	return NULL;
}

/* Take an entry out of the hash and the LRU.  Caller holds mbc_lock; the
 * entry is the caller's to free afterwards, unlocked. */
static void ext4_mbc_detach(struct ext4_state *st, struct ext4_mbc_ent *e)
{
	struct ext4_mbc_ent **pp = &st->mbc_hash[ext4_mbc_bucket(e->pbn)];

	while (*pp && *pp != e)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = e->hnext;
	e->hnext = NULL;
	ext4_mbc_lru_unlink(e);
	st->mbc_count--;
}

static struct ext4_mbc_ent *ext4_mbc_ent_alloc(ext4_fs_t *fs)
{
	struct ext4_mbc_ent *e = (struct ext4_mbc_ent *)kalloc(sizeof(*e));
	unsigned size = fs->block_size;

	if (!e)
		return NULL;
	mm_memset(e, 0, sizeof(*e));
	if (size == PAGE_SIZE) {
		uint64_t phys = mm_allocate_physical_page();

		if (phys) {
			e->data = (uint8_t *)phys_to_virt(phys);
			e->page = 1;
		}
	} else {
		e->data = (uint8_t *)kalloc(size);
	}
	if (!e->data) {
		kfree(e);
		return NULL;
	}
	e->size = size;
	mm_buffercache_account((long)size);
	return e;
}

/* Unlocked: see the note on freeing above. */
static void ext4_mbc_ent_free(struct ext4_mbc_ent *e)
{
	if (!e)
		return;
	mm_buffercache_account(-(long)e->size);
	if (e->page)
		mm_free_physical_page(virt_to_phys(e->data));
	else
		kfree(e->data);
	kfree(e);
}

/* Free a list of detached entries (linked through hnext). */
static void ext4_mbc_free_list(struct ext4_mbc_ent *list)
{
	while (list) {
		struct ext4_mbc_ent *next = list->hnext;

		ext4_mbc_ent_free(list);
		list = next;
	}
}

/* Detach up to `nr' of the coldest entries (all of them for nr == ~0UL)
 * into a list for the caller to free unlocked.  Caller holds mbc_lock. */
static struct ext4_mbc_ent *ext4_mbc_take_cold(struct ext4_state *st,
					      unsigned long nr,
					      unsigned long *taken)
{
	struct ext4_mbc_ent *list = NULL;
	unsigned long n = 0;

	while (n < nr && st->mbc_lru.lru_prev != &st->mbc_lru) {
		struct ext4_mbc_ent *e = st->mbc_lru.lru_prev;

		ext4_mbc_detach(st, e);
		e->hnext = list;
		list = e;
		n++;
	}
	if (taken)
		*taken = n;
	return list;
}

/* ---- giving memory back ---------------------------------------------- */

static unsigned long ext4_mbc_shrink_count(void *arg)
{
	ext4_fs_t *fs = arg;

	return fs->st ? fs->st->mbc_count : 0;
}

/* From the reclaim thread.  Dropping a cached copy is always correct: the
 * device holds the same bytes, and newer ones live in the transaction and
 * checkpoint sets, which are consulted before the cache. */
static unsigned long ext4_mbc_shrink_scan(void *arg, unsigned long nr)
{
	ext4_fs_t *fs = arg;
	unsigned long freed = 0;

	while (freed < nr) {
		struct ext4_mbc_ent *list;
		unsigned long got;
		uint64_t flags;

		spin_lock_irqsave(&fs->st->mbc_lock, &flags);
		/* In batches: a long detach loop is a long interrupts-off
		 * stretch. */
		list = ext4_mbc_take_cold(fs->st,
					  nr - freed > 64 ? 64 : nr - freed,
					  &got);
		spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
		ext4_mbc_free_list(list);
		if (!got)
			break;
		freed += got;
	}
	return freed;
}

/* The most blocks one filesystem keeps: a sixteenth of memory. */
static unsigned long ext4_mbc_ceiling(ext4_fs_t *fs)
{
	unsigned long blocks = (unsigned long)(mm_get_usable_pages() / 16) *
			       (PAGE_SIZE / (fs->block_size ? fs->block_size :
							      PAGE_SIZE));

	return blocks < 256 ? 256 : blocks;
}

int ext4_mbc_init(ext4_fs_t *fs)
{
	struct ext4_state *st = fs->st;

	st->mbc_hash = (struct ext4_mbc_ent **)kalloc(EXT4_MBC_HASH *
						     sizeof(*st->mbc_hash));
	if (!st->mbc_hash)
		return ST_NOMEM;
	mm_memset(st->mbc_hash, 0, EXT4_MBC_HASH * sizeof(*st->mbc_hash));
	st->mbc_lru.lru_prev = st->mbc_lru.lru_next = &st->mbc_lru;
	st->mbc_count = 0;
	st->mbc_shrinker.name = "ext4-metadata";
	st->mbc_shrinker.count = ext4_mbc_shrink_count;
	st->mbc_shrinker.scan = ext4_mbc_shrink_scan;
	st->mbc_shrinker.arg = fs;
	vm_register_shrinker(&st->mbc_shrinker);
	return ST_OK;
}

void ext4_mbc_destroy(ext4_fs_t *fs)
{
	struct ext4_state *st = fs->st;

	if (!st || !st->mbc_hash)
		return;
	/* First: waits for a scan of this filesystem in progress. */
	vm_unregister_shrinker(&st->mbc_shrinker);
	ext4_mbc_invalidate(fs);
	kfree(st->mbc_hash);
	st->mbc_hash = NULL;
}

/* ---- checksum-verification cache ----
 * Metadata checksums (directory leaves, extent nodes, xattr blocks) were
 * re-verified on EVERY read of the block — a directory leaf is re-checked on
 * every lookup that scans it, so hot directories paid a full-block crc32c per
 * path component per syscall.  The block cache remembers when a cached copy
 * has already passed its type-specific check; the flag is cleared whenever the
 * cached content changes (fresh disk read, write-through update). */
int ext4_mbc_verified(ext4_fs_t *fs, unsigned long pbn)
{
	struct ext4_mbc_ent *e;
	uint64_t flags;
	int v = 0;

	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	e = ext4_mbc_find(fs->st, pbn);
	if (e)
		v = e->verified;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return v;
}

void ext4_mbc_mark_verified(ext4_fs_t *fs, unsigned long pbn)
{
	struct ext4_mbc_ent *e;
	uint64_t flags;

	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	e = ext4_mbc_find(fs->st, pbn);
	if (e)
		e->verified = 1;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

/* Forget every cached block (mount, journal replay, unmount). */
void ext4_mbc_invalidate(ext4_fs_t *fs)
{
	struct ext4_mbc_ent *list;
	uint64_t flags;

	if (!fs->st->mbc_hash)
		return;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	list = ext4_mbc_take_cold(fs->st, ~0UL, NULL);
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	ext4_mbc_free_list(list);
}

/* Drop a single block from the metadata cache (on free, so a later reuse of
 * the same physical block never serves stale cached content). */
void ext4_mbc_drop(ext4_fs_t *fs, unsigned long pbn)
{
	struct ext4_mbc_ent *e;
	uint64_t flags;

	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	e = ext4_mbc_find(fs->st, pbn);
	if (e)
		ext4_mbc_detach(fs->st, e);
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	ext4_mbc_ent_free(e);
}

/* Serve `pbn` from the cache into `buf`.  Returns 1 on hit. */
static int ext4_mbc_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	struct ext4_mbc_ent *e;
	uint64_t flags;
	int hit = 0;

	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	e = ext4_mbc_find(fs->st, pbn);
	if (e && e->size >= fs->block_size) {
		mm_memcpy(buf, e->data, fs->block_size);
		ext4_mbc_lru_unlink(e);
		ext4_mbc_lru_push(fs->st, e);
		hit = 1;
	}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return hit;
}

/* Insert a freshly-read block.  `verified` seeds the csum-checked flag. */
static void ext4_mbc_insert(ext4_fs_t *fs, unsigned long pbn, const void *buf,
			    int verified)
{
	struct ext4_state *st = fs->st;
	struct ext4_mbc_ent *e, *fresh, *spare = NULL, *cold = NULL;
	uint64_t flags;

	if (!st->mbc_hash)
		return;
	/* Allocated before the lock (see the note above); given back below
	 * if another reader got the block in first. */
	fresh = ext4_mbc_ent_alloc(fs);

	spin_lock_irqsave(&st->mbc_lock, &flags);
	/* DEDUPE — never create a second entry for the same pbn.  Two
	 * shared-mode readers (fine-grained locking allows concurrent readers)
	 * can both miss the same block, both sit in the sleeping disk read, and
	 * both arrive here.  A second entry for one block is fatal in slow
	 * motion: the write-side refresh updates the one it finds, the other
	 * keeps its old content, and once it is the one served, the next
	 * inode/bitmap RMW through it durably REVERTS every update made since
	 * (observed once: /tmp htree leaf loss + ~78 lost links_count
	 * increments + lost bitmap bits, e2fsck).  Update the existing entry in
	 * place instead.
	 *
	 * INVARIANT: both racing readers read the same disk content (anything
	 * newer lives in st->txn/st->ckpt, checked before the mbc; mutators hold
	 * the meta lock exclusive), so an existing entry MUST equal what we
	 * just read.  A mismatch means a stale/incoherent cached metadata
	 * block — the corruption, caught at the moment it exists. */
	e = ext4_mbc_find(st, pbn);
	if (e && e->size >= fs->block_size) {
		WARN_RATELIMIT(
			ext4_memcmp(e->data, buf, fs->block_size) != 0,
			"ext4 mbc: cached pbn %lu differs from disk (stale metadata cache entry — corruption)",
			pbn);
		mm_memcpy(e->data, buf, fs->block_size);
		e->verified = verified;
		ext4_mbc_lru_unlink(e);
		ext4_mbc_lru_push(st, e);
		spare = fresh;
	} else if (fresh) {
		if (e) {
			/* Cached at a smaller block size (a remount changed
			 * it): replaced. */
			ext4_mbc_detach(st, e);
			cold = e;
		} else if (st->mbc_count >= ext4_mbc_ceiling(fs)) {
			unsigned long got;

			cold = ext4_mbc_take_cold(st, 1, &got);
		}
		mm_memcpy(fresh->data, buf, fs->block_size);
		fresh->pbn = pbn;
		fresh->verified = verified;
		fresh->hnext = st->mbc_hash[ext4_mbc_bucket(pbn)];
		st->mbc_hash[ext4_mbc_bucket(pbn)] = fresh;
		ext4_mbc_lru_push(st, fresh);
		st->mbc_count++;
	}
	spin_unlock_irqrestore(&st->mbc_lock, flags);
	ext4_mbc_ent_free(spare);
	ext4_mbc_free_list(cold);
}

/* A block was just written to the device: its cached copy, if any, becomes
 * those bytes (and has to pass its check again). */
void ext4_mbc_written(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	struct ext4_mbc_ent *e, *stale = NULL;
	uint64_t flags;

	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	e = ext4_mbc_find(fs->st, pbn);
	if (e && e->size >= fs->block_size) {
		mm_memcpy(e->data, buf, fs->block_size);
		e->verified = 0; /* content changed */
	} else if (e) {
		ext4_mbc_detach(fs->st, e); /* stale size: no copy at all */
		stale = e;
	}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	ext4_mbc_ent_free(stale); /* only what this call detached */
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
	ext4_mbc_written(fs, pbn, buf);
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
