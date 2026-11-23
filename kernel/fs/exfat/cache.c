/* LikeOS - exFAT filesystem driver
 *
 * The metadata cache of the FAT region and the allocation bitmap.
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

/* ===========================================================================
 * 7. Metadata cache: the FAT region and the allocation bitmap
 *
 * Neither goes through the page cache.  The FAT region is not made of
 * clusters -- its last page could reach past its end into whatever follows
 * it, and writeback of such a page would store stale bytes there -- and the
 * order in which FAT and bitmap changes reach the device is the heart of
 * crash consistency (section 17), which background writeback would take
 * out of our hands.  So both live here: units of min(cluster, 4096) bytes,
 * each wholly inside its region (a bitmap unit inside one cluster), with a
 * dirty bit per device sector.  Only the commit (or a mutation running out
 * of room) writes dirty units, through the chokepoint.
 *
 * Concurrency: units are found and inserted under fs->mc_lock; a unit in use
 * holds a reference and cannot be dropped.  Unit contents change only under
 * the metadata lock exclusive, so readers (shared) never see a change in
 * progress.  I/O never happens under the spinlock.
 * =========================================================================== */

static inline unsigned exfat_mc_hash(int region, uint32_t unit)
{
	return (unsigned)(((uint32_t)region * 0x9E3779B1u) ^
			  (unit * 0x85EBCA6Bu)) %
	       EXFAT_MC_HASH;
}

/* Valid bytes of the region a unit index falls in, or 0 past its end. */
uint64_t exfat_mc_region_bytes(exfat_fs_t *fs, int region)
{
	if (region == EXFAT_MC_FAT) {
		/* Only the entries of real clusters, rounded to whole device
		 * sectors (the FAT region itself is longer when it was made
		 * for growth; the rest is never touched). */
		uint64_t need = ((uint64_t)fs->cluster_count + 2) * 4;

		return exfat_min64(exfat_round_up(need, fs->dev_ss),
				   exfat_round_up(fs->fat_bytes, fs->dev_ss));
	}
	/* Bitmap: the bytes covering ClusterCount bits, rounded to device
	 * sectors, never beyond the clusters the bitmap owns. */
	{
		uint64_t need = ((uint64_t)fs->cluster_count + 7) / 8;
		uint64_t own = 0;

		for (uint32_t i = 0; i < fs->bm_next; i++)
			own += (uint64_t)fs->bm_ext[i].len << fs->cs_shift;
		return exfat_min64(exfat_round_up(need, fs->dev_ss), own);
	}
}

/* Where unit `unit' of `region' lives: first device sector and length in
 * device sectors.  0 when the unit lies beyond the region. */
static int exfat_mc_locate(exfat_fs_t *fs, int region, uint32_t unit,
			   uint64_t *lba, unsigned *nsect)
{
	uint64_t off = (uint64_t)unit * fs->mcu_bytes;
	uint64_t total = exfat_mc_region_bytes(fs, region);
	uint64_t len;

	if (off >= total)
		return 0;
	len = exfat_min64(fs->mcu_bytes, total - off);
	*nsect = (unsigned)(len >> fs->dev_shift);
	if (*nsect == 0)
		return 0;
	if (region == EXFAT_MC_FAT) {
		*lba = fs->fat_lba + (off >> fs->dev_shift);
		return 1;
	}
	/* Bitmap: through its extents (a unit never spans two clusters). */
	{
		uint64_t ci = off >> fs->cs_shift;
		uint64_t in = off & (fs->cs - 1);

		for (uint32_t i = 0; i < fs->bm_next; i++) {
			const exfat_extent_t *e = &fs->bm_ext[i];

			if (ci >= e->fidx && ci < (uint64_t)e->fidx + e->len) {
				uint64_t cl = e->disk + (ci - e->fidx);

				*lba = fs->heap_lba +
				       ((cl - EXFAT_FIRST_CLUSTER)
					<< (fs->cs_shift - fs->dev_shift)) +
				       (in >> fs->dev_shift);
				return 1;
			}
		}
	}
	return 0;
}

static exfat_mcu_t *exfat_mc_find_locked(exfat_fs_t *fs, int region,
					 uint32_t unit)
{
	for (exfat_mcu_t *u = fs->mc_hash[exfat_mc_hash(region, unit)]; u;
	     u = u->hnext)
		if (u->region == region && u->unit == unit)
			return u;
	return NULL;
}

static void exfat_mc_lru_unlink(exfat_fs_t *fs, exfat_mcu_t *u)
{
	if (u->lru_prev)
		u->lru_prev->lru_next = u->lru_next;
	else
		fs->mc_lru_head = u->lru_next;
	if (u->lru_next)
		u->lru_next->lru_prev = u->lru_prev;
	else
		fs->mc_lru_tail = u->lru_prev;
	u->lru_prev = u->lru_next = NULL;
}

static void exfat_mc_lru_push(exfat_fs_t *fs, exfat_mcu_t *u)
{
	u->lru_prev = NULL;
	u->lru_next = fs->mc_lru_head;
	if (fs->mc_lru_head)
		fs->mc_lru_head->lru_prev = u;
	fs->mc_lru_head = u;
	if (!fs->mc_lru_tail)
		fs->mc_lru_tail = u;
}

static void exfat_mc_hash_unlink(exfat_fs_t *fs, exfat_mcu_t *u)
{
	exfat_mcu_t **pp = &fs->mc_hash[exfat_mc_hash(u->region, u->unit)];

	while (*pp && *pp != u)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = u->hnext;
	u->hnext = NULL;
}

/* Drop clean, unused units beyond the cap (oldest first).  Any context that
 * may sleep (kfree), no spinlock held. */
static void exfat_mc_trim(exfat_fs_t *fs)
{
	exfat_mcu_t *victims = NULL;
	uint64_t flags;

	spin_lock_irqsave(&fs->mc_lock, &flags);
	for (exfat_mcu_t *u = fs->mc_lru_tail;
	     u && fs->mc_count > EXFAT_MC_MAX;) {
		exfat_mcu_t *prev = u->lru_prev;

		if (u->refs == 0 && !u->dirty) {
			exfat_mc_lru_unlink(fs, u);
			exfat_mc_hash_unlink(fs, u);
			fs->mc_count--;
			u->hnext = victims;
			victims = u;
		}
		u = prev;
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);
	while (victims) {
		exfat_mcu_t *n = victims->hnext;

		kfree(victims->data);
		kfree(victims);
		victims = n;
	}
}

/* Get unit `unit' of `region', held.  Reads it (and, for the FAT, up to
 * EXFAT_MC_READAHEAD following uncached units in the same transfer) on a
 * miss.  NULL with *err set on failure.  Metadata lock held (either mode). */
exfat_mcu_t *exfat_mc_get(exfat_fs_t *fs, int region, uint32_t unit,
			  int *err)
{
	exfat_mcu_t *u, *batch[EXFAT_MC_READAHEAD];
	uint64_t lba, flags;
	unsigned nsect, n = 0, total_sect = 0;
	uint8_t *buf = NULL;
	int r;

	might_sleep();
	exfat_assert_meta(fs);
	*err = ST_OK;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	u = exfat_mc_find_locked(fs, region, unit);
	if (u) {
		u->refs++;
		exfat_mc_lru_unlink(fs, u);
		exfat_mc_lru_push(fs, u);
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		return u;
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);

	if (!exfat_mc_locate(fs, region, unit, &lba, &nsect)) {
		*err = exfat_corrupt(fs, unit,
				     "metadata unit %u of region %d beyond its end",
				     unit, region);
		return NULL;
	}

	/* A mutation holding many dirty units writes them early rather than
	 * letting the cache grow without bound.  Early is always allowed:
	 * the units hold allocations only, frees are applied at the commit
	 * (section 17). */
	if (fs->mc_dirty > EXFAT_MC_MAX && exfat_rwsem_held_excl(&fs->meta) &&
	    fs->armed)
		exfat_mc_flush(fs);

	/* Build the batch: this unit plus following ones, contiguous on the
	 * device and not cached yet (FAT only: chains are walked forward). */
	for (uint32_t k = 0; k < EXFAT_MC_READAHEAD; k++) {
		uint64_t l2;
		unsigned s2;

		if (k > 0) {
			if (region != EXFAT_MC_FAT)
				break;
			if (!exfat_mc_locate(fs, region, unit + k, &l2, &s2))
				break;
			if (l2 != lba + total_sect)
				break;
			spin_lock_irqsave(&fs->mc_lock, &flags);
			int cached = exfat_mc_find_locked(fs, region,
							  unit + k) != NULL;
			spin_unlock_irqrestore(&fs->mc_lock, flags);
			if (cached)
				break;
		} else {
			s2 = nsect;
		}
		u = (exfat_mcu_t *)kcalloc(1, sizeof(*u));
		if (!u)
			break;
		u->data = (uint8_t *)kalloc(fs->mcu_bytes);
		if (!u->data) {
			kfree(u);
			break;
		}
		u->region = (uint8_t)region;
		u->unit = unit + k;
		u->nsect = (uint16_t)s2;
		u->nbytes = s2 << fs->dev_shift;
		u->lba = lba + total_sect;
		batch[n++] = u;
		total_sect += s2;
	}
	if (n == 0) {
		*err = ST_NOMEM;
		return NULL;
	}
	if (n == 1) {
		r = exfat_dev_read(fs, batch[0]->lba, batch[0]->nsect,
				   batch[0]->data);
	} else {
		buf = (uint8_t *)kalloc((size_t)total_sect << fs->dev_shift);
		if (!buf) {
			/* Read just the one asked for. */
			for (unsigned k = 1; k < n; k++) {
				kfree(batch[k]->data);
				kfree(batch[k]);
			}
			n = 1;
			r = exfat_dev_read(fs, batch[0]->lba, batch[0]->nsect,
					   batch[0]->data);
		} else {
			r = exfat_dev_read(fs, batch[0]->lba, total_sect, buf);
			if (r == ST_OK) {
				uint64_t off = 0;

				for (unsigned k = 0; k < n; k++) {
					mm_memcpy(batch[k]->data, buf + off,
						  batch[k]->nbytes);
					off += batch[k]->nbytes;
				}
			}
			kfree(buf);
		}
	}
	if (r != ST_OK) {
		for (unsigned k = 0; k < n; k++) {
			kfree(batch[k]->data);
			kfree(batch[k]);
		}
		WARN_RATELIMIT(1, "exfat: %s: metadata read at sector %llu failed (%d)",
			       exfat_name_of(fs),
			       (unsigned long long)lba, r);
		*err = ST_IO;
		return NULL;
	}

	/* Insert; another reader may have inserted any of them meanwhile --
	 * then its copy wins and ours is dropped (they are equal: both came
	 * off the device while nobody could change it). */
	exfat_mcu_t *result = NULL, *drop = NULL;

	spin_lock_irqsave(&fs->mc_lock, &flags);
	for (unsigned k = 0; k < n; k++) {
		exfat_mcu_t *have = exfat_mc_find_locked(fs, region,
							 batch[k]->unit);

		if (have) {
			batch[k]->hnext = drop;
			drop = batch[k];
			if (k == 0) {
				have->refs++;
				result = have;
			}
			continue;
		}
		unsigned hb = exfat_mc_hash(region, batch[k]->unit);

		batch[k]->hnext = fs->mc_hash[hb];
		fs->mc_hash[hb] = batch[k];
		exfat_mc_lru_push(fs, batch[k]);
		fs->mc_count++;
		if (k == 0) {
			batch[k]->refs = 1;
			result = batch[k];
		}
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);
	while (drop) {
		exfat_mcu_t *nx = drop->hnext;

		kfree(drop->data);
		kfree(drop);
		drop = nx;
	}
	if (fs->mc_count > EXFAT_MC_MAX)
		exfat_mc_trim(fs);
	return result;
}

void exfat_mc_put(exfat_fs_t *fs, exfat_mcu_t *u)
{
	uint64_t flags;

	if (!u)
		return;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	WARN_ON(u->refs <= 0);
	if (u->refs > 0)
		u->refs--;
	spin_unlock_irqrestore(&fs->mc_lock, flags);
}

/* Record bytes [off, off + len) of a held unit as changed.  Metadata lock
 * exclusive. */
void exfat_mc_dirty(exfat_fs_t *fs, exfat_mcu_t *u, unsigned off,
		    unsigned len)
{
	uint64_t flags;
	unsigned first, last;

	exfat_assert_excl(fs);
	if (len == 0)
		return;
	BUG_ON(off + len > u->nbytes);
	first = off >> fs->dev_shift;
	last = (off + len - 1) >> fs->dev_shift;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	if (!u->dirty)
		fs->mc_dirty++;
	for (unsigned s = first; s <= last; s++)
		u->dirty |= (uint8_t)(1u << s);
	spin_unlock_irqrestore(&fs->mc_lock, flags);
}

/* Does `region' have dirty units? */
static int exfat_mc_region_dirty(exfat_fs_t *fs, int region)
{
	uint64_t flags;
	int any = 0;

	if (!fs->mc_dirty)
		return 0;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	for (exfat_mcu_t *u = fs->mc_lru_head; u && !any; u = u->lru_next)
		if (u->dirty && u->region == region)
			any = 1;
	spin_unlock_irqrestore(&fs->mc_lock, flags);
	return any;
}

/* Write the dirty units of one region (runs of dirty sectors) through the
 * chokepoint.  The bitmap goes lowest address first, the FAT highest
 * address first: a stream grows into clusters after its last one (the
 * allocator looks there first), so the new clusters' entries mostly reach
 * the device before the entry that links to them even where the order is
 * not enforced (exfat_mc_flush_links enforces it for the growth of the
 * streams it knows).  Metadata lock exclusive. */
int exfat_mc_flush_region(exfat_fs_t *fs, int region)
{
	int desc = region == EXFAT_MC_FAT;

	exfat_assert_excl(fs);
	might_sleep();
	for (;;) {
		exfat_mcu_t *best = NULL;
		uint64_t flags;
		uint8_t mask;
		int r = ST_OK;

		/* The dirty unit of this region with the lowest (highest)
		 * address (few dirty units at a time: a scan). */
		spin_lock_irqsave(&fs->mc_lock, &flags);
		for (exfat_mcu_t *u = fs->mc_lru_head; u; u = u->lru_next)
			if (u->dirty && u->region == region &&
			    (!best || (desc ? u->lba > best->lba :
					      u->lba < best->lba)))
				best = u;
		if (best)
			best->refs++;
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		if (!best)
			return ST_OK;
		mask = best->dirty;
		for (unsigned k = 0; k < best->nsect;) {
			/* Runs of dirty sectors [sct, e), in address order
			 * or (desc) from the top down. */
			unsigned sct = desc ? best->nsect - 1u - k : k, e, lo;

			if (!(mask & (1u << sct))) {
				k++;
				continue;
			}
			lo = e = sct;
			if (desc) {
				while (lo > 0 && (mask & (1u << (lo - 1))))
					lo--;
				e = sct + 1;
				k += e - lo;
			} else {
				while (e < best->nsect && (mask & (1u << e)))
					e++;
				k = e;
			}
			r = exfat_dev_write(fs, best->lba + lo, e - lo,
					    best->data +
						    ((unsigned long)lo
						     << fs->dev_shift),
					    EXFAT_W_META);
			if (r != ST_OK)
				break;
		}
		spin_lock_irqsave(&fs->mc_lock, &flags);
		if (r == ST_OK) {
			best->dirty &= (uint8_t)~mask;
			if (!best->dirty)
				fs->mc_dirty--;
		}
		best->refs--;
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		if (r != ST_OK)
			return r; /* the unit stays dirty */
	}
}

/* The FAT of streams that grew, in two steps.  The entry of a stream's old
 * last cluster -- the link that makes the new clusters part of its chain --
 * goes back to what it was before the growth (fs->links), everything else
 * is written (the new clusters' links and end marks), a barrier, and then
 * the links themselves.  A chain on the device is thus whole at every
 * moment: it never runs from a new link into entries not yet written --
 * stale entries of clusters freed earlier, which may lead into other files'
 * clusters, or free ones.  For the root directory, whose length IS its
 * chain, that is what keeps it readable; for a file it keeps the chain past
 * its length (a power cut before its entry set is stored) to clusters of
 * its own.  Metadata lock exclusive. */
static int exfat_mc_flush_links(exfat_fs_t *fs)
{
	uint32_t cur[EXFAT_LINKS_MAX];
	exfat_mcu_t *u[EXFAT_LINKS_MAX];
	unsigned n = fs->nlinks;
	int r;

	exfat_assert_excl(fs);
	for (unsigned i = 0; i < n; i++) {
		uint64_t off = (uint64_t)fs->links[i].cluster * 4;
		uint64_t flags;

		/* The unit is cached (dirty since the link was written): a
		 * lookup, never I/O or eviction here. */
		spin_lock_irqsave(&fs->mc_lock, &flags);
		u[i] = exfat_mc_find_locked(fs, EXFAT_MC_FAT,
					    (uint32_t)(off / fs->mcu_bytes));
		if (u[i])
			u[i]->refs++;
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		if (!u[i])
			continue; /* written as it is, at worst */
		off %= fs->mcu_bytes;
		cur[i] = exfat_le32(u[i]->data + off);
		exfat_put32(u[i]->data + off, fs->links[i].old);
	}
	r = exfat_mc_flush_region(fs, EXFAT_MC_FAT);
	/* The links back, whatever happened: memory holds the truth. */
	for (unsigned i = n; i-- > 0;) {
		uint64_t off;

		if (!u[i])
			continue;
		off = ((uint64_t)fs->links[i].cluster * 4) % fs->mcu_bytes;
		exfat_put32(u[i]->data + off, cur[i]);
		exfat_mc_dirty(fs, u[i], (unsigned)off, 4);
		exfat_mc_put(fs, u[i]);
	}
	if (r == ST_OK && fs->barrier_needed)
		r = exfat_dev_sync(fs);
	if (r == ST_OK)
		fs->nlinks = 0; /* the caller writes the links now */
	return r;
}

/* Write every dirty unit, in the order allocations need: the BITMAP first
 * (a cluster is marked used before anything can lead to it), a device
 * sync, then the FAT (the links into the new clusters).  Zeroed root
 * directory clusters, whose only reference is the FAT link, are on the
 * device before that link too.  The commit's free phase uses the reverse
 * order itself (section 17).  Metadata lock exclusive. */
int exfat_mc_flush(exfat_fs_t *fs)
{
	int r;

	exfat_assert_excl(fs);
	if (fs->mc_dirty == 0 && !fs->root_zero_pending)
		return ST_OK;
	r = exfat_mc_flush_region(fs, EXFAT_MC_BITMAP);
	if (r != ST_OK)
		return r;
	if (exfat_mc_region_dirty(fs, EXFAT_MC_FAT)) {
		if (fs->barrier_needed) {
			r = exfat_dev_sync(fs);
			if (r != ST_OK)
				return r;
		}
		if (fs->nlinks) {
			r = exfat_mc_flush_links(fs);
			if (r != ST_OK)
				return r;
		}
		fs->root_zero_pending = 0;
		r = exfat_mc_flush_region(fs, EXFAT_MC_FAT);
	}
	return r;
}

/* Forget every unit (unmount, failed mount).  Dirty ones are dropped: the
 * caller has flushed what can be flushed. */
void exfat_mc_destroy(exfat_fs_t *fs)
{
	for (unsigned b = 0; b < EXFAT_MC_HASH; b++) {
		exfat_mcu_t *u = fs->mc_hash[b];

		while (u) {
			exfat_mcu_t *n = u->hnext;

			WARN_ON(u->refs != 0);
			kfree(u->data);
			kfree(u);
			u = n;
		}
		fs->mc_hash[b] = NULL;
	}
	fs->mc_lru_head = fs->mc_lru_tail = NULL;
	fs->mc_count = 0;
	fs->mc_dirty = 0;
}
