/* LikeOS - exFAT filesystem driver
 *
 * Streams (handle table, key table, extents), page-cache superblock
 * operations.
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

static exfat_stream_t *exfat_stream_by_handle(exfat_fs_t *fs, uint32_t h);

/* ===========================================================================
 * 12. Streams
 *
 * Every file or directory the driver works with is a stream object: what
 * its entry set says (kept authoritative in memory from the moment the
 * object exists -- a size grown by write() is here before it is on the
 * device), its allocation decoded into extents, and its page-cache handle.
 *
 * Two tables find them, both under fs->st_lock:
 *  - by HANDLE (the page cache's chain id carries it): a two-level array,
 *    free slots threaded into a free list;
 *  - by KEY, the location of the entry set: (first cluster of the parent
 *    directory, index of the File entry).  Directories never change their
 *    first cluster, so the key of a child is stable for as long as the
 *    entry set stays where it is; a rename re-keys the stream.  The root
 *    has its own key.
 *
 * A stream holds a reference on its parent directory's stream (directories
 * with live children stay cached).  Unreferenced streams sit on an LRU; the
 * oldest clean ones beyond EXFAT_STREAM_LRU_MAX are forgotten: removed from
 * the tables, their pages and inode-cache entry dropped, THEN their handle
 * freed -- a handle is reused only after every page of its old stream is
 * gone (exfat.h, section 1).
 * =========================================================================== */

/* A free slot holds its successor in the free list, tagged with bit 0. */
#define EXFAT_HSLOT_FREE(next) ((exfat_stream_t *)(((uintptr_t)(next) << 1) | 1))
#define EXFAT_HSLOT_IS_FREE(p) (((uintptr_t)(p)) & 1)
#define EXFAT_HSLOT_NEXT(p) ((uint32_t)(((uintptr_t)(p)) >> 1))

static inline unsigned exfat_khash(uint64_t key)
{
	return (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 54) % EXFAT_KHASH;
}

/* The stream of handle `h', or NULL.  Not held: valid while the caller
 * holds the metadata lock (streams are destroyed only by puts, and a
 * stream in use holds a reference). */
static exfat_stream_t *exfat_stream_by_handle(exfat_fs_t *fs, uint32_t h)
{
	exfat_stream_t *st = NULL;
	uint64_t flags;

	if (h < EXFAT_HANDLE_FIRST_DYNAMIC || h > EXFAT_HANDLE_MAX)
		return NULL;
	spin_lock_irqsave(&fs->st_lock, &flags);
	if (fs->htab) {
		exfat_stream_t **chunk = fs->htab[h >> EXFAT_HCHUNK_SHIFT];

		if (chunk) {
			st = chunk[h & (EXFAT_HCHUNK - 1)];
			if (EXFAT_HSLOT_IS_FREE(st) || (st && st->dead))
				st = NULL;
		}
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

/* Take a handle and point it at `st'.  ST_OK or ST_NOMEM / ST_NOSPC. */
int exfat_handle_alloc(exfat_fs_t *fs, exfat_stream_t *st)
{
	uint64_t flags;
	uint32_t h;
	exfat_stream_t **fresh = NULL;

	for (;;) {
		spin_lock_irqsave(&fs->st_lock, &flags);
		if (fs->hfree_head) {
			h = fs->hfree_head;
		} else if (fs->next_handle <= EXFAT_HANDLE_MAX) {
			h = fs->next_handle;
		} else {
			spin_unlock_irqrestore(&fs->st_lock, flags);
			if (fresh)
				kfree(fresh);
			return ST_NOSPC;
		}
		exfat_stream_t **chunk = fs->htab[h >> EXFAT_HCHUNK_SHIFT];

		if (!chunk && fresh) {
			fs->htab[h >> EXFAT_HCHUNK_SHIFT] = fresh;
			chunk = fresh;
			fresh = NULL;
		}
		if (!chunk) {
			spin_unlock_irqrestore(&fs->st_lock, flags);
			fresh = (exfat_stream_t **)kcalloc(EXFAT_HCHUNK,
							   sizeof(*fresh));
			if (!fresh)
				return ST_NOMEM;
			continue;
		}
		if (h == fs->hfree_head) {
			exfat_stream_t *slot = chunk[h & (EXFAT_HCHUNK - 1)];

			fs->hfree_head = EXFAT_HSLOT_NEXT(slot);
		} else {
			fs->next_handle++;
		}
		chunk[h & (EXFAT_HCHUNK - 1)] = st;
		st->handle = h;
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (fresh)
			kfree(fresh);
		return ST_OK;
	}
}

/* Give a handle back.  Its stream's pages are gone already. */
static void exfat_handle_free(exfat_fs_t *fs, uint32_t h)
{
	uint64_t flags;
	exfat_stream_t **chunk;

	if (h < EXFAT_HANDLE_FIRST_DYNAMIC)
		return;
	spin_lock_irqsave(&fs->st_lock, &flags);
	chunk = fs->htab[h >> EXFAT_HCHUNK_SHIFT];
	if (chunk) {
		chunk[h & (EXFAT_HCHUNK - 1)] = EXFAT_HSLOT_FREE(fs->hfree_head);
		fs->hfree_head = h;
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
}

static void exfat_lru_unlink_locked(exfat_fs_t *fs, exfat_stream_t *st)
{
	if (!st->on_lru)
		return;
	if (st->lru_prev)
		st->lru_prev->lru_next = st->lru_next;
	else
		fs->lru_head = st->lru_next;
	if (st->lru_next)
		st->lru_next->lru_prev = st->lru_prev;
	else
		fs->lru_tail = st->lru_prev;
	st->lru_prev = st->lru_next = NULL;
	st->on_lru = 0;
	fs->lru_count--;
}

static void exfat_lru_push_locked(exfat_fs_t *fs, exfat_stream_t *st)
{
	st->lru_prev = NULL;
	st->lru_next = fs->lru_head;
	if (fs->lru_head)
		fs->lru_head->lru_prev = st;
	fs->lru_head = st;
	if (!fs->lru_tail)
		fs->lru_tail = st;
	st->on_lru = 1;
	fs->lru_count++;
}

static void exfat_khash_unlink_locked(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_stream_t **pp;

	if (!st->hashed)
		return;
	pp = &fs->khash[exfat_khash(st->key)];
	while (*pp && *pp != st)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = st->hnext;
	st->hnext = NULL;
	st->hashed = 0;
}

void exfat_khash_insert_locked(exfat_fs_t *fs, exfat_stream_t *st)
{
	unsigned b = exfat_khash(st->key);

	st->hnext = fs->khash[b];
	fs->khash[b] = st;
	st->hashed = 1;
}

static exfat_stream_t *exfat_khash_find_locked(exfat_fs_t *fs, uint64_t key)
{
	for (exfat_stream_t *s = fs->khash[exfat_khash(key)]; s; s = s->hnext)
		if (s->key == key && !s->dead)
			return s;
	return NULL;
}

/* A new reference to a stream found through a table (under st_lock). */
static void exfat_stream_hold_locked(exfat_fs_t *fs, exfat_stream_t *st)
{
	if (st->refs++ == 0)
		exfat_lru_unlink_locked(fs, st);
}

void exfat_stream_hold(exfat_stream_t *st)
{
	uint64_t flags;

	spin_lock_irqsave(&st->fs->st_lock, &flags);
	exfat_stream_hold_locked(st->fs, st);
	spin_unlock_irqrestore(&st->fs->st_lock, flags);
}

/* Free a stream that is out of every table. */
void exfat_stream_free(exfat_stream_t *st)
{
	if (st->ext)
		kfree(st->ext);
	kfree(st);
}

/* Forget a dead stream: its pages, its inode-cache entry, its handle, and
 * the hold it had on its parent (returned for the caller to put). */
static exfat_stream_t *exfat_stream_destroy(exfat_stream_t *st)
{
	exfat_fs_t *fs = st->fs;
	exfat_stream_t *parent = st->parent;
	unsigned long chain = exfat_chain_of(st);

	/* No page of it is dirty: streams with unstored data are never
	 * evicted, and an orphan's pages are dropped on purpose. */
	pagecache_invalidate_file(&fs->sb, chain);
	icache_remove(&fs->sb, chain);
	if (st->is_dir && st->first_cluster)
		dcache_invalidate_dir(&fs->sb, st->first_cluster);
	exfat_handle_free(fs, st->handle);
	st->parent = NULL;
	exfat_stream_free(st);
	return parent;
}

static void exfat_stream_put_one(exfat_stream_t *st);

/* Evict unreferenced clean streams beyond the LRU cap (oldest first).
 * Callable with the metadata lock held in either mode (no I/O).  A loop:
 * the parent references the evicted ones drop are put without trimming
 * again (no recursion up the directory tree). */
static void exfat_stream_trim(exfat_fs_t *fs)
{
	exfat_assert_excl(fs);
	for (;;) {
		exfat_stream_t *victim = NULL, *parent;
		uint64_t flags;

		spin_lock_irqsave(&fs->st_lock, &flags);
		if (fs->lru_count > EXFAT_STREAM_LRU_MAX) {
			for (exfat_stream_t *s = fs->lru_tail; s;
			     s = s->lru_prev) {
				if (s->refs == 0 && !s->data_dirty &&
				    !s->meta_dirty && !s->is_root) {
					victim = s;
					break;
				}
			}
		}
		if (victim) {
			exfat_lru_unlink_locked(fs, victim);
			exfat_khash_unlink_locked(fs, victim);
			victim->dead = 1;
			fs->nstreams--;
		}
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (!victim)
			return;
		parent = exfat_stream_destroy(victim);
		if (parent)
			exfat_stream_put_one(parent);
	}
}

/* Drop a reference.  The last one parks the stream on the LRU -- or, for
 * an orphan (unlinked, its clusters handed to the free machinery by the
 * last close), forgets it: at once when this task holds the metadata lock
 * exclusive (then its hold on its parent goes the same way, a loop), else
 * at the end of the next exclusive hold.  No LRU trimming here. */
static void exfat_stream_put_one(exfat_stream_t *st)
{
	while (st) {
		exfat_fs_t *fs = st->fs;
		int excl = exfat_rwsem_held_excl(&fs->meta);
		uint64_t flags;
		int destroy = 0;

		spin_lock_irqsave(&fs->st_lock, &flags);
		WARN_ON(st->refs <= 0);
		if (st->refs > 0 && --st->refs == 0) {
			if (st->orphan) {
				exfat_khash_unlink_locked(fs, st);
				st->dead = 1;
				fs->nstreams--;
				if (excl) {
					destroy = 1;
				} else {
					st->dnext = fs->reap_head;
					fs->reap_head = st;
				}
			} else {
				exfat_lru_push_locked(fs, st);
			}
		}
		spin_unlock_irqrestore(&fs->st_lock, flags);
		st = destroy ? exfat_stream_destroy(st) : NULL;
	}
}

/* exfat_stream_put_one() and, for a task holding the metadata lock
 * exclusive while the LRU is over its cap, a trim. */
void exfat_stream_put(exfat_stream_t *st)
{
	exfat_fs_t *fs;

	if (!st)
		return;
	fs = st->fs;
	exfat_stream_put_one(st);
	if (fs->lru_count > EXFAT_STREAM_LRU_MAX &&
	    exfat_rwsem_held_excl(&fs->meta))
		exfat_stream_trim(fs);
}

/* The end of an exclusive hold: destroy the parked orphans and trim the
 * LRU.  Metadata lock exclusive. */
void exfat_stream_maint(exfat_fs_t *fs)
{
	for (;;) {
		exfat_stream_t *st;
		uint64_t flags;

		spin_lock_irqsave(&fs->st_lock, &flags);
		st = fs->reap_head;
		if (st)
			fs->reap_head = st->dnext;
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (!st)
			break;
		st->dnext = NULL;
		exfat_stream_put_one(exfat_stream_destroy(st));
	}
	if (fs->lru_count > EXFAT_STREAM_LRU_MAX)
		exfat_stream_trim(fs);
}

/* Fill a stream's allocation from its entry set.  Metadata lock held. */
int exfat_stream_decode(exfat_fs_t *fs, exfat_stream_t *st)
{
	uint64_t want = 0;
	int nofat = (st->sflags & EXFAT_SF_NO_FAT_CHAIN) != 0;
	int r;

	if (st->is_root) {
		r = exfat_chain_decode(fs, st->first_cluster, 0, 0,
				       EXFAT_MAX_DIR_BYTES >> fs->cs_shift,
				       &st->ext, &st->next, &st->ext_cap,
				       &st->nclusters);
		if (r == ST_OK)
			st->size = st->vdl = (uint64_t)st->nclusters
					     << fs->cs_shift;
		return r;
	}
	if (st->size)
		want = (st->size + fs->cs - 1) >> fs->cs_shift;
	if (want > fs->cluster_count)
		return exfat_corrupt(fs, st->first_cluster,
				     "stream of %llu bytes larger than the volume",
				     (unsigned long long)st->size);
	if (want == 0) {
		/* An empty stream may still name a cluster (some writers keep
		 * one); it is ignored and released at the next change. */
		st->nclusters = 0;
		st->next = 0;
		return ST_OK;
	}
	if (st->first_cluster == 0)
		return exfat_corrupt(fs, st->entry_index,
				     "stream of %llu bytes without clusters",
				     (unsigned long long)st->size);
	return exfat_chain_decode(fs, st->first_cluster, nofat, want, 0,
				  &st->ext, &st->next, &st->ext_cap,
				  &st->nclusters);
}

/* The stream of the entry set `di' in directory `parent', held: the
 * existing object when there is one (its in-memory state wins over the
 * entry set), else a new one decoded from `di'.  Metadata lock held. */
exfat_stream_t *exfat_stream_get(exfat_fs_t *fs, exfat_stream_t *parent,
				 const exfat_dentry_info_t *di,
				 int *err)
{
	uint64_t key = EXFAT_KEY(parent->first_cluster, di->index);
	exfat_stream_t *st, *have;
	uint64_t flags;
	int r;

	exfat_assert_meta(fs);
	*err = ST_OK;
	spin_lock_irqsave(&fs->st_lock, &flags);
	st = exfat_khash_find_locked(fs, key);
	if (st)
		exfat_stream_hold_locked(fs, st);
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (st)
		return st;

	st = (exfat_stream_t *)kcalloc(1, sizeof(*st));
	if (!st) {
		*err = ST_NOMEM;
		return NULL;
	}
	st->fs = fs;
	st->key = key;
	st->entry_index = di->index;
	st->secondary_count = di->secondary_count;
	st->attr = di->attr;
	st->sflags = di->sflags;
	st->first_cluster = di->first_cluster;
	st->size = di->size;
	st->is_dir = (di->attr & EXFAT_ATTR_DIR) != 0;
	st->vdl = st->is_dir ? di->size : exfat_min64(di->vdl, di->size);
	st->ctime = di->ctime;
	st->mtime = di->mtime;
	st->atime = di->atime;
	st->ctime_cs = di->ctime_cs;
	st->mtime_cs = di->mtime_cs;
	st->ctime_utc = di->ctime_utc;
	st->mtime_utc = di->mtime_utc;
	st->atime_utc = di->atime_utc;
	st->subdirs = -1;
	st->dlo = 1;
	st->dhi = 0;
	if (st->is_dir && st->size > EXFAT_MAX_DIR_BYTES) {
		kfree(st);
		*err = exfat_corrupt(fs, di->first_cluster,
				     "directory of %llu bytes",
				     (unsigned long long)di->size);
		return NULL;
	}
	r = exfat_stream_decode(fs, st);
	if (r != ST_OK) {
		if (st->ext)
			kfree(st->ext);
		kfree(st);
		*err = r;
		return NULL;
	}
	r = exfat_handle_alloc(fs, st);
	if (r != ST_OK) {
		if (st->ext)
			kfree(st->ext);
		kfree(st);
		*err = r;
		return NULL;
	}
	st->refs = 1;

	spin_lock_irqsave(&fs->st_lock, &flags);
	have = exfat_khash_find_locked(fs, key);
	if (have) {
		/* Another lookup made it meanwhile: use that one. */
		exfat_stream_hold_locked(fs, have);
		spin_unlock_irqrestore(&fs->st_lock, flags);
		exfat_handle_free(fs, st->handle);
		exfat_stream_free(st);
		return have;
	}
	exfat_khash_insert_locked(fs, st);
	exfat_stream_hold_locked(fs, parent);
	st->parent = parent;
	fs->nstreams++;
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

/* The existing stream object of a location, held, or NULL. */
exfat_stream_t *exfat_stream_find(exfat_fs_t *fs, uint64_t key)
{
	exfat_stream_t *st;
	uint64_t flags;

	spin_lock_irqsave(&fs->st_lock, &flags);
	st = exfat_khash_find_locked(fs, key);
	if (st)
		exfat_stream_hold_locked(fs, st);
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

/* Move a stream to a new location (rename).  Metadata lock exclusive. */
void exfat_stream_rekey(exfat_fs_t *fs, exfat_stream_t *st,
			exfat_stream_t *new_parent, uint32_t index,
			uint8_t secondary_count)
{
	exfat_stream_t *old_parent;
	uint64_t flags;

	exfat_assert_excl(fs);
	spin_lock_irqsave(&fs->st_lock, &flags);
	exfat_khash_unlink_locked(fs, st);
	st->key = EXFAT_KEY(new_parent->first_cluster, index);
	st->entry_index = index;
	st->secondary_count = secondary_count;
	exfat_khash_insert_locked(fs, st);
	old_parent = st->parent;
	if (old_parent != new_parent) {
		exfat_stream_hold_locked(fs, new_parent);
		st->parent = new_parent;
	} else {
		old_parent = NULL;
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (old_parent)
		exfat_stream_put(old_parent);
}

/* Unlink a stream from its name: it keeps its clusters while referenced
 * and is forgotten at its last put.  Metadata lock exclusive. */
void exfat_stream_orphan(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_stream_t *parent;
	uint64_t flags;

	exfat_assert_excl(fs);
	spin_lock_irqsave(&fs->st_lock, &flags);
	exfat_khash_unlink_locked(fs, st);
	st->orphan = 1;
	parent = st->parent;
	st->parent = NULL;
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (parent)
		exfat_stream_put(parent);
}

/* Bytes the stream's clusters can hold. */
static inline uint64_t exfat_alloc_bytes(exfat_fs_t *fs, const exfat_stream_t *st)
{
	return (uint64_t)st->nclusters << fs->cs_shift;
}

/* ===========================================================================
 * 13. Page-cache superblock operations (exfat.h, section 1)
 *
 * Pure memory lookups over the decoded extents, under the metadata lock
 * (shared through lock_map, exclusive through lock_io).
 * =========================================================================== */

static inline exfat_fs_t *exfat_fs_of_sb(vfs_superblock_t *sb)
{
	return (exfat_fs_t *)sb->fs_private;
}

/* The page that straddles ValidDataLength of a file whose VDL is short of
 * its size and not on a block boundary: the one page the driver fills
 * itself (section 19).  (unsigned long)-1 when there is none. */
unsigned long exfat_nofill_page(exfat_fs_t *fs, const exfat_stream_t *st)
{
	if (st->is_dir || st->vdl >= st->size || !(st->vdl & (fs->bs - 1)))
		return (unsigned long)-1;
	return (unsigned long)(st->vdl >> PAGE_SHIFT);
}

static unsigned long exfat_st_block_at(exfat_fs_t *fs, const exfat_stream_t *st,
				       unsigned long n)
{
	uint64_t blocks = (uint64_t)st->nclusters << fs->bpc_shift;

	if (n >= blocks)
		return 0;
	if (((uint64_t)n << fs->bs_shift) >> PAGE_SHIFT ==
	    exfat_nofill_page(fs, st))
		return 0;
	return EXFAT_BID_ENC(st->handle, n);
}

/* LBA of block n of the stream; 0 for a block wholly past ValidDataLength
 * (reads as zeros, never written) or without a cluster. */
unsigned long exfat_st_block_lba(exfat_fs_t *fs, const exfat_stream_t *st,
				 unsigned long n)
{
	uint64_t ci = (uint64_t)n >> fs->bpc_shift;
	uint32_t disk;

	if (ci >= st->nclusters)
		return 0;
	if (((uint64_t)n << fs->bs_shift) >= st->vdl)
		return 0;
	disk = exfat_st_cluster(st, ci);
	if (!disk)
		return 0;
	return (unsigned long)(fs->heap_lba +
			       ((uint64_t)(disk - EXFAT_FIRST_CLUSTER)
				<< (fs->cs_shift - fs->dev_shift)) +
			       (((uint64_t)n & ((1ULL << fs->bpc_shift) - 1))
				<< (fs->bs_shift - fs->dev_shift)));
}

static unsigned long exfat_sb_block_size(vfs_superblock_t *sb)
{
	return exfat_fs_of_sb(sb)->bs;
}

static const block_device_t *exfat_sb_bdev(vfs_superblock_t *sb)
{
	return &exfat_fs_of_sb(sb)->guard;
}

static unsigned long exfat_sb_sector_size(vfs_superblock_t *sb)
{
	return exfat_fs_of_sb(sb)->dev_ss;
}

static unsigned long exfat_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return EXFAT_EOC_MARKER;
}

static unsigned long exfat_sb_block_at(vfs_superblock_t *sb,
				       unsigned long chain,
				       unsigned long index)
{
	exfat_fs_t *fs = exfat_fs_of_sb(sb);
	exfat_stream_t *st;

	if (!EXFAT_BID_IS(chain) || EXFAT_BID_INDEX(chain) != 0)
		return 0;
	st = exfat_stream_by_handle(fs, (uint32_t)EXFAT_BID_HANDLE(chain));
	if (!st)
		return 0;
	return exfat_st_block_at(fs, st, index);
}

static unsigned long exfat_sb_next_block(vfs_superblock_t *sb,
					 unsigned long cur)
{
	exfat_fs_t *fs = exfat_fs_of_sb(sb);
	exfat_stream_t *st;
	unsigned long r;

	if (!EXFAT_BID_IS(cur))
		return EXFAT_BID_EOC;
	st = exfat_stream_by_handle(fs, (uint32_t)EXFAT_BID_HANDLE(cur));
	if (!st)
		return EXFAT_BID_EOC;
	r = exfat_st_block_at(fs, st, EXFAT_BID_INDEX(cur) + 1);
	return r ? r : EXFAT_BID_EOC;
}

static unsigned long exfat_sb_block_to_lba(vfs_superblock_t *sb,
					   unsigned long bid)
{
	exfat_fs_t *fs = exfat_fs_of_sb(sb);
	exfat_stream_t *st;

	if (!EXFAT_BID_IS(bid))
		return 0;
	st = exfat_stream_by_handle(fs, (uint32_t)EXFAT_BID_HANDLE(bid));
	if (!st)
		return 0;
	return exfat_st_block_lba(fs, st, EXFAT_BID_INDEX(bid));
}

static unsigned long exfat_sb_map_block(vfs_superblock_t *sb,
					unsigned long chain,
					unsigned long index)
{
	unsigned long id = exfat_sb_block_at(sb, chain, index);

	if (!id || !exfat_sb_block_to_lba(sb, id))
		return 0;
	return id;
}

static void exfat_sb_lock_io(vfs_superblock_t *sb)
{
	exfat_meta_wlock(exfat_fs_of_sb(sb));
}

static void exfat_sb_unlock_io(vfs_superblock_t *sb)
{
	exfat_rwsem_write_unlock(&exfat_fs_of_sb(sb)->meta);
}

static void exfat_sb_lock_map(vfs_superblock_t *sb)
{
	exfat_meta_rlock(exfat_fs_of_sb(sb));
}

static void exfat_sb_unlock_map(vfs_superblock_t *sb)
{
	exfat_meta_runlock(exfat_fs_of_sb(sb));
}

static unsigned long exfat_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

/* Generic inode flush (icache_flush): the stream's entry set, if it has
 * changes, goes to the device with a commit. */
static int exfat_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino)
{
	exfat_fs_t *fs = exfat_fs_of_sb(sb);
	exfat_stream_t *st;
	int r = ST_OK;

	if (!ino || !EXFAT_BID_IS(ino->start_cluster))
		return 0;
	exfat_meta_wlock(fs);
	st = exfat_stream_by_handle(fs,
				    (uint32_t)EXFAT_BID_HANDLE(ino->start_cluster));
	if (st && st->meta_dirty && fs->armed)
		r = exfat_commit(fs);
	exfat_meta_wunlock(fs);
	return r == ST_OK ? 0 : -1;
}

const vfs_sb_ops_t exfat_sb_ops = {
	.block_size = exfat_sb_block_size,
	.bdev = exfat_sb_bdev,
	.sector_size = exfat_sb_sector_size,
	.block_to_lba = exfat_sb_block_to_lba,
	.end_of_chain_marker = exfat_sb_end_of_chain,
	.next_block = exfat_sb_next_block,
	.map_block = exfat_sb_map_block,
	.block_at = exfat_sb_block_at,
	.write_inode = exfat_sb_write_inode,
	.lock_io = exfat_sb_lock_io,
	.unlock_io = exfat_sb_unlock_io,
	.lock_map = exfat_sb_lock_map,
	.unlock_map = exfat_sb_unlock_map,
	.reserved_meta_block = exfat_sb_reserved_meta_block,
	.alloc_blocks = NULL, /* allocation happens in write() */
};
