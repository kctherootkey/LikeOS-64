/* LikeOS - NTFS filesystem driver
 *
 * The cluster allocator ($Bitmap), reservations, discard and trim.
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
 * S24, S30  Allocator helpers: run arrays and the reservation list
 *
 * The cluster allocator itself is S24 (below); delayed allocation, S30
 * (file.c).
 * =========================================================================== */

/* Append a run to an allocation result. */
int ntfs_runs_add(ntfs_run_t **runs, uint32_t *n, uint32_t *cap,
		  int64_t lcn, int64_t len)
{
	if (*n && (*runs)[*n - 1].lcn + (*runs)[*n - 1].len == lcn) {
		(*runs)[*n - 1].len += len;
		return ST_OK;
	}
	if (*n == *cap) {
		uint32_t nc = *cap ? *cap * 2 : 8;
		ntfs_run_t *nr;

		if (nc > NTFS_MAX_RUNS)
			return ST_NOMEM;
		nr = (ntfs_run_t *)kalloc(nc * sizeof(ntfs_run_t));
		if (!nr)
			return ST_NOMEM;
		if (*n)
			mm_memcpy(nr, *runs, *n * sizeof(ntfs_run_t));
		ntfs_free(*runs);
		*runs = nr;
		*cap = nc;
	}
	(*runs)[*n].vcn = 0;
	(*runs)[*n].lcn = lcn;
	(*runs)[*n].len = len;
	(*n)++;
	return ST_OK;
}

/* ---- the reservation list (st->dl) ---- */

static long ntfs_dl_find(const ntfs_stream_t *st, int64_t vcn)
{
	for (uint32_t i = 0; i < st->dl_n; i++)
		if (vcn >= st->dl[i].vcn && vcn < st->dl[i].vcn + st->dl[i].len)
			return (long)i;
	return -1;
}

/* Add [vcn, vcn + len) (not overlapping what is there). */
static int ntfs_dl_add(ntfs_stream_t *st, int64_t vcn, int64_t len)
{
	uint32_t i;

	for (i = 0; i < st->dl_n && st->dl[i].vcn < vcn; i++)
		;
	/* Merge with neighbours. */
	if (i > 0 && st->dl[i - 1].vcn + st->dl[i - 1].len == vcn) {
		st->dl[i - 1].len += len;
		if (i < st->dl_n && st->dl[i - 1].vcn + st->dl[i - 1].len ==
					     st->dl[i].vcn) {
			st->dl[i - 1].len += st->dl[i].len;
			for (uint32_t k = i + 1; k < st->dl_n; k++)
				st->dl[k - 1] = st->dl[k];
			st->dl_n--;
		}
		return ST_OK;
	}
	if (i < st->dl_n && vcn + len == st->dl[i].vcn) {
		st->dl[i].vcn = vcn;
		st->dl[i].len += len;
		return ST_OK;
	}
	if (st->dl_n == st->dl_cap) {
		uint32_t nc = st->dl_cap ? st->dl_cap * 2 : 8;
		ntfs_dext_t *n;

		if (nc > NTFS_MAX_RUNS)
			return ST_NOMEM;
		n = (ntfs_dext_t *)kalloc(nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		if (st->dl_n)
			mm_memcpy(n, st->dl, st->dl_n * sizeof(*n));
		ntfs_free(st->dl);
		st->dl = n;
		st->dl_cap = nc;
	}
	for (uint32_t k = st->dl_n; k > i; k--)
		st->dl[k] = st->dl[k - 1];
	st->dl[i].vcn = vcn;
	st->dl[i].len = len;
	st->dl_n++;
	return ST_OK;
}

/* Remove [vcn, vcn + len) from the list; returns the clusters removed. */
uint64_t ntfs_dl_cut(ntfs_stream_t *st, int64_t vcn, int64_t len)
{
	int64_t end = vcn + len;
	uint64_t cut = 0;

	for (uint32_t i = 0; i < st->dl_n;) {
		ntfs_dext_t *x = &st->dl[i];
		int64_t xe = x->vcn + x->len;
		int64_t s = x->vcn > vcn ? x->vcn : vcn;
		int64_t e = xe < end ? xe : end;

		if (e <= s) {
			i++;
			continue;
		}
		cut += (uint64_t)(e - s);
		if (s == x->vcn && e == xe) {
			for (uint32_t k = i + 1; k < st->dl_n; k++)
				st->dl[k - 1] = st->dl[k];
			st->dl_n--;
			continue;
		}
		if (s == x->vcn) {
			x->len = xe - e;
			x->vcn = e;
		} else if (e == xe) {
			x->len = s - x->vcn;
		} else {
			/* A hole in the middle: split (room permitting; the
			 * tail is re-added). */
			int64_t tail_vcn = e, tail_len = xe - e;

			x->len = s - x->vcn;
			if (ntfs_dl_add(st, tail_vcn, tail_len) != ST_OK) {
				/* Could not split: keep the whole tail
				 * reserved (over-reserving is harmless). */
				x->len = xe - x->vcn;
				cut -= (uint64_t)(e - s);
				i++;
				continue;
			}
		}
		i++;
	}
	if (st->dl_n == 0)
		st->flags &= ~NTFS_ST_DELALLOC;
	return cut;
}

/* Clusters of [v0, v1) neither mapped nor reserved; with `add' they are
 * added to the list (the caller reserved them). */
int ntfs_dl_uncovered(ntfs_stream_t *st, int64_t v0, int64_t v1,
		      int add, uint64_t *count)
{
	int64_t v = v0;

	*count = 0;
	while (v < v1) {
		int64_t lcn, left, seg_end;
		long di;

		if (v < st->rl.end_vcn &&
		    ntfs_rl_map(&st->rl, v, &lcn, &left) == ST_OK) {
			seg_end = v + left < v1 ? v + left : v1;
			if (lcn != NTFS_LCN_HOLE) {
				v = seg_end;
				continue;
			}
		} else {
			seg_end = v1;
			if (v < st->rl.end_vcn) {
				return ST_IO; /* unmappable, not a hole */
			}
		}
		/* [v, seg_end) is unallocated: minus what is reserved. */
		while (v < seg_end) {
			di = ntfs_dl_find(st, v);
			if (di >= 0) {
				int64_t de = st->dl[di].vcn + st->dl[di].len;

				v = de < seg_end ? de : seg_end;
				continue;
			}
			{
				int64_t e = seg_end;

				for (uint32_t k = 0; k < st->dl_n; k++)
					if (st->dl[k].vcn > v && st->dl[k].vcn < e)
						e = st->dl[k].vcn;
				if (add) {
					int r = ntfs_dl_add(st, v, e - v);

					if (r != ST_OK)
						return r; /* *count: what was added */
				}
				*count += (uint64_t)(e - v);
				v = e;
			}
		}
	}
	return ST_OK;
}

/* ===========================================================================
 * S24  The cluster allocator ($Bitmap)
 *
 * $Bitmap holds one bit per cluster.  It is read and changed through the
 * page cache of its reserved stream (NTFS_HANDLE_BITMAP): a page holds the
 * bits of 32768 clusters; a change sets or clears bits in the cached page
 * and writes the changed bytes through to the device at once
 * (ntfs_wt_write(), whole device sectors).  The page is never dirty.
 *
 * Free-space accounting (statfs): fs->free_clusters is
 * counted from the bitmap the first time it is needed (bounded by the
 * volume's cluster count, never by the bitmap's size) and kept
 * current by every allocation and free -- a free counts only the bits it
 * actually cleared.  fs->resv_clusters counts what delayed allocation has
 * promised to write() (S30): a reservation succeeds only while
 *
 *     free_clusters - resv_clusters - NTFS_META_FLOOR >= wanted
 *
 * so write() reports ENOSPC exactly and writeback never runs out; metadata
 * (MFT records, index blocks, attribute lists) may use the floor.
 *
 * Placement:
 *   - the MFT zone [zone_start, zone_end) -- 12.5 % of the volume per
 *     mft_zone_multiplier step, from $MFT's start -- is kept for $MFT
 *     growth; data goes elsewhere first and into the zone only when the
 *     rest is full (the zone then shrinks; no request is refused
 *     because the zone is empty);
 *   - a `goal' (the cluster after the run a file already ends with) is
 *     tried first, so files grow in place;
 *   - then the first free run long enough for the whole request (next fit
 *     from the last allocation, within a scan budget), then any free runs:
 *     a request may be satisfied by several runs, in VCN order; the caller
 *     maps every one of them (a "contiguous" request never gets a
 *     second run it does not know about -- NTFS_AL_CONTIG either gets one
 *     run or fails);
 *   - a failed goal restarts the search from scratch; every
 *     candidate cluster is checked against the volume size.
 *
 * Ordering: bits are set (written) before anything references the
 * clusters, and cleared only after the last reference is gone from the
 * device (callers: S27, S30, S31); ntfs_barrier() flushes the device
 * cache between the two steps so a power cut cannot reorder them.
 * =========================================================================== */

/* Bitmap pages scanned for one contiguous run before settling for
 * fragments. */
#define NTFS_CONTIG_BUDGET 128

static void ntfs_discard_freed(ntfs_fs_t *fs, int64_t lcn, uint64_t count);

int ntfs_barrier(ntfs_fs_t *fs)
{
	int r;

	if (!fs->armed)
		return ST_OK; /* nothing was written yet */
	r = ntfs_dev_flush(fs);
	/* The cleared bits of queued frees are durable now. */
	if (r == ST_OK && fs->disc_n)
		ntfs_discard_run(fs);
	return r;
}

/* Publish the counters to statfs. */
void ntfs_sf_publish(ntfs_fs_t *fs)
{
	uint64_t fl;

	spin_lock_irqsave(&fs->sf_lock, &fl);
	if (fs->bm_valid && fs->mftbm_valid) {
		fs->sf_free_clusters = fs->free_clusters; /* statfs subtracts
							   * the reservations */
		fs->sf_free_records = fs->free_records;
		fs->sf_valid = 1;
	} else {
		fs->sf_valid = 0;
	}
	spin_unlock_irqrestore(&fs->sf_lock, fl);
}

/* Count the free clusters once (metadata lock held). */
static int ntfs_bm_count(ntfs_fs_t *fs)
{
	uint64_t fc = 0, zone;
	int r;

	if (fs->bm_valid)
		return ST_OK;
	if (!fs->st_bitmap)
		return ST_IO;
	r = ntfs_bitmap_free(fs, fs->st_bitmap, fs->nr_clusters, &fc);
	if (r != ST_OK)
		return r;
	fs->free_clusters = fc;
	fs->bm_valid = 1;
	zone = (fs->nr_clusters / 8) * (uint64_t)fs->opt.mft_zone_mult;
	fs->zone_start = fs->mft_lcn;
	fs->zone_end = fs->mft_lcn + zone;
	if (fs->zone_end > fs->nr_clusters)
		fs->zone_end = fs->nr_clusters;
	if (fs->data_hint < fs->zone_end || fs->data_hint >= fs->nr_clusters)
		fs->data_hint = fs->zone_end < fs->nr_clusters ? fs->zone_end :
								 0;
	ntfs_sf_publish(fs);
	return ST_OK;
}

/* Page `idx' of $Bitmap, held. */
static int ntfs_bm_page(ntfs_fs_t *fs, uint64_t idx, pc_page_t **pg)
{
	if (idx * PAGE_SIZE >= fs->st_bitmap->data_size)
		return ST_IO;
	return ntfs_stream_page(fs, fs->st_bitmap, idx, pg);
}

static inline int ntfs_bit(const uint8_t *b, uint64_t i)
{
	return (b[i >> 3] >> (i & 7)) & 1;
}

/* Clear bits from `lcn' on, at most `max', below `end' (clusters free in a
 * row); ST_OK with *n (0 when `lcn' is in use). */
static int ntfs_bm_run(ntfs_fs_t *fs, uint64_t lcn, uint64_t max, uint64_t end,
		       uint64_t *n)
{
	uint64_t c = lcn;

	*n = 0;
	if (end > fs->nr_clusters)
		end = fs->nr_clusters;
	while (c < end && c - lcn < max) {
		uint64_t idx = c / NTFS_BM_PAGE_BITS;
		uint64_t pend = (idx + 1) * NTFS_BM_PAGE_BITS;
		pc_page_t *pg;
		int r = ntfs_bm_page(fs, idx, &pg);

		if (r != ST_OK)
			return r;
		if (pend > end)
			pend = end;
		while (c < pend && c - lcn < max) {
			uint64_t bit = c - idx * NTFS_BM_PAGE_BITS;

			if (!(bit & 63) && pend - c >= 64 && max - (c - lcn) >= 64 &&
			    ntfs_le64(pg->data + bit / 8) == 0) {
				c += 64;
				continue;
			}
			if (ntfs_bit(pg->data, bit))
				break;
			c++;
		}
		pagecache_put(pg);
		if (c < pend && c - lcn < max)
			break; /* a set bit */
	}
	*n = c - lcn;
	return ST_OK;
}

/* The first clear bit in [from, to); ST_NOT_FOUND when none.  At most
 * `budget' pages are looked at (0: no limit); *scanned counts them. */
int ntfs_bm_next_free(ntfs_fs_t *fs, uint64_t from, uint64_t to,
		      uint64_t *out, uint32_t budget, uint32_t *scanned)
{
	uint64_t c = from;

	if (to > fs->nr_clusters)
		to = fs->nr_clusters;
	while (c < to) {
		uint64_t idx = c / NTFS_BM_PAGE_BITS;
		uint64_t pend = (idx + 1) * NTFS_BM_PAGE_BITS;
		pc_page_t *pg;
		int r;

		if (budget && *scanned >= budget)
			return ST_NOT_FOUND;
		(*scanned)++;
		r = ntfs_bm_page(fs, idx, &pg);
		if (r != ST_OK)
			return r;
		if (pend > to)
			pend = to;
		while (c < pend) {
			uint64_t bit = c - idx * NTFS_BM_PAGE_BITS;

			if (!(bit & 63) && pend - c >= 64 &&
			    ntfs_le64(pg->data + bit / 8) == ~0ULL) {
				c += 64;
				continue;
			}
			if (!ntfs_bit(pg->data, bit)) {
				pagecache_put(pg);
				*out = c;
				return ST_OK;
			}
			c++;
		}
		pagecache_put(pg);
	}
	return ST_NOT_FOUND;
}

/* Set (val 1) or clear (val 0) bits [lcn, lcn + n) of $Bitmap and write
 * the changed bytes through.  *changed counts the bits that really
 * changed; a bit already in the wanted state is corruption when setting
 * (a cluster in use twice) and reported by the caller. */
static int ntfs_bm_set(ntfs_fs_t *fs, uint64_t lcn, uint64_t n, int val,
		       uint64_t *changed)
{
	uint64_t c = lcn, end = lcn + n;
	int r = ST_OK;

	*changed = 0;
	if (lcn >= fs->nr_clusters || n > fs->nr_clusters - lcn)
		return ST_IO;
	while (c < end) {
		uint64_t idx = c / NTFS_BM_PAGE_BITS;
		uint64_t pbase = idx * NTFS_BM_PAGE_BITS;
		uint64_t pend = ntfs_min64(pbase + NTFS_BM_PAGE_BITS, end);
		uint64_t b0 = (c - pbase) / 8, b1 = (pend - pbase + 7) / 8;
		pc_page_t *pg;

		r = ntfs_bm_page(fs, idx, &pg);
		if (r != ST_OK)
			return r;
		for (uint64_t i = c - pbase; i < pend - pbase; i++) {
			uint8_t m = (uint8_t)(1U << (i & 7));

			if (!(pg->data[i >> 3] & m) == !val)
				continue;
			if (val)
				pg->data[i >> 3] |= m;
			else
				pg->data[i >> 3] &= (uint8_t)~m;
			(*changed)++;
		}
		r = ntfs_wt_write(fs, fs->st_bitmap, idx * PAGE_SIZE + b0,
				  pg->data + b0, b1 - b0);
		if (r != ST_OK) {
			/* The cached page must not claim what the device
			 * does not have: drop it. */
			pagecache_put(pg);
			pagecache_invalidate_file(&fs->sb,
						  ntfs_chain(fs->st_bitmap));
			fs->bm_valid = 0;
			return r;
		}
		pagecache_put(pg);
		c = pend;
	}
	return ST_OK;
}

/* Reserve `n' clusters for delayed allocation (exact ENOSPC). */
int ntfs_resv_get(ntfs_fs_t *fs, uint64_t n)
{
	int r;

	ntfs_assert_meta_excl(fs);
	r = ntfs_bm_count(fs);
	if (r != ST_OK)
		return r;
	if (fs->free_clusters < fs->resv_clusters + NTFS_META_FLOOR ||
	    n > fs->free_clusters - fs->resv_clusters - NTFS_META_FLOOR)
		return ST_NOSPC;
	fs->resv_clusters += n;
	ntfs_sf_publish(fs);
	return ST_OK;
}

void ntfs_resv_put(ntfs_fs_t *fs, uint64_t n)
{
	WARN_ON(n > fs->resv_clusters);
	fs->resv_clusters -= n > fs->resv_clusters ? fs->resv_clusters : n;
	ntfs_sf_publish(fs);
}

/* Free `count' clusters at `lcn': the bits are cleared (only those set
 * count), the counters follow, the discard hook runs.  The caller
 * has made sure nothing on the device references them any more (S27:
 * ntfs_barrier() after the dereferencing record write). */
int ntfs_clusters_free(ntfs_fs_t *fs, int64_t lcn, uint64_t count)
{
	uint64_t cleared = 0;
	int r;

	ntfs_assert_meta_excl(fs);
	if (!count)
		return ST_OK;
	if (lcn < 0 || (uint64_t)lcn >= fs->nr_clusters ||
	    count > fs->nr_clusters - (uint64_t)lcn)
		return ntfs_fs_error(fs, 0, "freeing clusters outside the volume");
	r = ntfs_bm_count(fs);
	if (r == ST_OK) {
		ntfs_alloc_lock(fs);
		r = ntfs_bm_set(fs, (uint64_t)lcn, count, 0, &cleared);
		ntfs_alloc_unlock(fs);
	}
	if (r != ST_OK) {
		ntfs_leaked(fs); /* still marked in use */
		return r;
	}
	/* Only what really changed is counted back. */
	fs->free_clusters += cleared;
	ntfs_sf_publish(fs);
	if (cleared != count) {
		/* Not discarded: a cluster that was free already may belong
		 * to another file (the bitmap was wrong about it). */
		ntfs_fs_error(fs, 0, "freed clusters that were already free");
		return ST_OK;
	}
	ntfs_discard_freed(fs, lcn, count);
	return ST_OK;
}

/* Free every allocated run of `rl' in [v0, v1). */
int ntfs_rl_free_clusters(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
			  int64_t v0, int64_t v1)
{
	int r = ST_OK;

	for (uint32_t i = 0; i < rl->count; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		int64_t s = x->vcn > v0 ? x->vcn : v0;
		int64_t e = x->vcn + x->len < v1 ? x->vcn + x->len : v1;
		int rr;

		if (e <= s || x->lcn == NTFS_LCN_HOLE)
			continue;
		rr = ntfs_clusters_free(fs, x->lcn + (s - x->vcn),
					(uint64_t)(e - s));
		if (rr != ST_OK && r == ST_OK)
			r = rr;
	}
	return r;
}

/* Search one region [lo, hi) starting at `start' (wrapping to lo) for a
 * run.  `want_len' > 1 asks for a run of that length (scan budget
 * applies), 1 for the first free cluster. */
static int ntfs_region_find(ntfs_fs_t *fs, uint64_t lo, uint64_t hi,
			    uint64_t start, uint64_t want_len,
			    uint64_t *lcn, uint64_t *len)
{
	uint32_t scanned = 0, budget = want_len > 1 ? NTFS_CONTIG_BUDGET : 0;

	if (lo >= hi)
		return ST_NOT_FOUND;
	if (start < lo || start >= hi)
		start = lo;
	for (int pass = 0; pass < 2; pass++) {
		uint64_t c = pass ? lo : start;
		uint64_t to = pass ? start : hi;

		while (c < to) {
			uint64_t f, n;
			int r = ntfs_bm_next_free(fs, c, to, &f, budget,
						  &scanned);

			if (r == ST_NOT_FOUND)
				break;
			if (r != ST_OK)
				return r;
			r = ntfs_bm_run(fs, f, want_len, hi, &n);
			if (r != ST_OK)
				return r;
			if (n >= want_len) {
				*lcn = f;
				*len = n;
				return ST_OK;
			}
			c = f + n + 1;
		}
	}
	return ST_NOT_FOUND;
}

/* Take run (lcn, n) for an allocation in progress: set its bits now, so
 * that no later search of the same allocation finds it again. */
static int ntfs_alloc_take(ntfs_fs_t *fs, ntfs_run_t **out, uint32_t *nout,
			   uint32_t *cap, uint64_t lcn, uint64_t n)
{
	uint64_t changed = 0;
	int r;

	/* $Boot's clusters are never handed out, whatever $Bitmap says. */
	if (lcn < (ntfs_boot_bytes(fs) + fs->cluster_size - 1) >>
			  fs->cluster_bits)
		return ntfs_fs_error(fs, 0, "the allocator met a free boot "
					    "cluster");
	r = ntfs_bm_set(fs, lcn, n, 1, &changed);

	if (r == ST_OK && changed != n) {
		/* Some bit was set already: the search saw them clear a moment
		 * ago under the allocator lock, so the bitmap is not what it
		 * seemed.  Nothing is undone -- clearing the range would free
		 * the clusters that were in use before; the bits this call set
		 * stay set (a leak a check reclaims). */
		fs->free_clusters -= changed <= fs->free_clusters ?
					     changed :
					     fs->free_clusters;
		ntfs_sf_publish(fs);
		return ntfs_fs_error(fs, 0, "allocated a cluster in use");
	}
	if (r != ST_OK)
		return r;
	r = ntfs_runs_add(out, nout, cap, (int64_t)lcn, (int64_t)n);
	if (r != ST_OK) {
		uint64_t ch;

		ntfs_bm_set(fs, lcn, n, 0, &ch);
	}
	return r;
}

/* Clusters $BadClus:$Bad lists as bad but $Bitmap shows free are
 * marked in use before the first allocation of a read-write mount (not at
 * mount time: a mount that never changes anything never writes). */
static int ntfs_bad_reserve(ntfs_fs_t *fs)
{
	int r = ST_OK;

	if (fs->bad_done)
		return ST_OK;
	for (uint32_t i = 0; r == ST_OK && i < fs->bad_rl.count; i++) {
		uint64_t changed = 0;

		if (fs->bad_rl.runs[i].lcn == NTFS_LCN_HOLE)
			continue;
		ntfs_alloc_lock(fs);
		r = ntfs_bm_set(fs, (uint64_t)fs->bad_rl.runs[i].lcn,
				(uint64_t)fs->bad_rl.runs[i].len, 1, &changed);
		ntfs_alloc_unlock(fs);
		if (r == ST_OK && changed) {
			fs->free_clusters -= changed;
			kprintf("ntfs: %s: %llu bad cluster(s) marked in use\n",
				fs->bdev->name, (unsigned long long)changed);
		}
	}
	if (r == ST_OK) {
		fs->bad_done = 1;
		ntfs_sf_publish(fs);
	}
	return r;
}

/* Allocate `count' clusters (see the section comment).  On success the
 * bits are set on the device and *runs / *nruns (kfree) list the runs in
 * the order the caller maps them (their `vcn' fields are 0).  ST_NOSPC
 * when the volume cannot hold them; nothing is left allocated on any
 * failure. */
int ntfs_clusters_alloc(ntfs_fs_t *fs, int64_t goal, uint64_t count,
			int flags, ntfs_run_t **runs, uint32_t *nruns)
{
	ntfs_run_t *out = NULL;
	uint32_t nout = 0, cap = 0;
	uint64_t avail, left = count, regions[3][2];
	int r = ST_OK;

	ntfs_assert_meta_excl(fs);
	*runs = NULL;
	*nruns = 0;
	if (!count)
		return ST_OK;
	r = ntfs_bm_count(fs);
	if (r != ST_OK)
		return r;
	/* Known-bad clusters are marked in use first -- only once the
	 * allocation can be made at all (a refused one writes nothing). */
	for (int pass = 0; pass < 2; pass++) {
		avail = fs->free_clusters;
		if (!(flags & NTFS_AL_RESERVED)) {
			uint64_t keep = fs->resv_clusters +
					((flags & NTFS_AL_META) ? 0 :
								  NTFS_META_FLOOR);

			avail = avail > keep ? avail - keep : 0;
		}
		if (count > avail)
			return ST_NOSPC;
		if (pass == 0) {
			r = ntfs_bad_reserve(fs);
			if (r != ST_OK)
				return r;
		}
	}
	ntfs_alloc_lock(fs);
	/* Regions in order of preference. */
	if (flags & NTFS_AL_MFT) {
		regions[0][0] = fs->zone_start;
		regions[0][1] = fs->zone_end;
		regions[1][0] = fs->zone_end;
		regions[1][1] = fs->nr_clusters;
		regions[2][0] = 0;
		regions[2][1] = fs->zone_start;
	} else {
		regions[0][0] = fs->zone_end;
		regions[0][1] = fs->nr_clusters;
		regions[1][0] = 0;
		regions[1][1] = fs->zone_start;
		regions[2][0] = fs->zone_start;
		regions[2][1] = fs->zone_end;
	}
	/* 1. The goal: grow in place. */
	if (goal >= 0 && (uint64_t)goal < fs->nr_clusters) {
		uint64_t n;

		r = ntfs_bm_run(fs, (uint64_t)goal, left, fs->nr_clusters, &n);
		if (r != ST_OK)
			goto fail;
		if (n && (!(flags & NTFS_AL_CONTIG) || n == left)) {
			r = ntfs_alloc_take(fs, &out, &nout, &cap, (uint64_t)goal,
					    n);
			if (r != ST_OK)
				goto fail;
			left -= n;
		}
	}
	/* 2a. preallocated_size=: the tail of a growing file goes where the
	 * free extent is at least preallocated_size longer than needed, and
	 * the next-fit position for other data moves past that room, so the
	 * file's next appends continue in place (step 1) and small appends do
	 * not fragment its runlist.  Nothing beyond the data is allocated on
	 * the device: the room stays free, there is nothing to trim when the
	 * file is closed and nothing stale can show up in it. */
	if (left && (flags & NTFS_AL_TAIL) && !(flags & NTFS_AL_MFT) &&
	    fs->opt.prealloc >= fs->cluster_size) {
		uint64_t extra = fs->opt.prealloc >> fs->cluster_bits;

		for (int i = 0; left && i < 3; i++) {
			uint64_t lcn = 0, n = 0;

			r = ntfs_region_find(fs, regions[i][0], regions[i][1],
					     fs->data_hint, left + extra, &lcn,
					     &n);
			if (r == ST_NOT_FOUND)
				continue;
			if (r != ST_OK)
				goto fail;
			r = ntfs_alloc_take(fs, &out, &nout, &cap, lcn, left);
			if (r != ST_OK)
				goto fail;
			left = 0;
		}
		r = ST_OK;
	}
	/* 2. One run for everything that is left (a fresh search,
	 * whatever the goal attempt did). */
	for (int i = 0; left && i < 3; i++) {
		uint64_t lcn = 0, n = 0;
		uint64_t start = (flags & NTFS_AL_MFT) ? regions[i][0] :
							   fs->data_hint;

		r = ntfs_region_find(fs, regions[i][0], regions[i][1], start,
				     left, &lcn, &n);
		if (r == ST_NOT_FOUND)
			continue;
		if (r != ST_OK)
			goto fail;
		r = ntfs_alloc_take(fs, &out, &nout, &cap, lcn, left);
		if (r != ST_OK)
			goto fail;
		left = 0;
	}
	if (left && (flags & NTFS_AL_CONTIG)) {
		r = ST_NOSPC;
		goto fail;
	}
	/* 3. Fragments, region by region (the bits taken so far are set:
	 * the search passes over them). */
	for (int i = 0; left && i < 3; i++) {
		uint64_t c = regions[i][0];

		while (left && c < regions[i][1]) {
			uint64_t f, n;
			uint32_t scanned = 0;

			r = ntfs_bm_next_free(fs, c, regions[i][1], &f, 0,
					      &scanned);
			if (r == ST_NOT_FOUND)
				break;
			if (r != ST_OK)
				goto fail;
			r = ntfs_bm_run(fs, f, left, regions[i][1], &n);
			if (r != ST_OK)
				goto fail;
			if (!n) {
				c = f + 1;
				continue;
			}
			r = ntfs_alloc_take(fs, &out, &nout, &cap, f, n);
			if (r != ST_OK)
				goto fail;
			left -= n;
			c = f + n;
		}
	}
	if (left) {
		/* The count said there was room: the bitmap disagrees. */
		r = ntfs_fs_error(fs, 0, "free cluster count is wrong");
		goto fail;
	}
	fs->free_clusters -= count;
	if (flags & NTFS_AL_RESERVED)
		ntfs_resv_put(fs, count);
	if (!(flags & NTFS_AL_MFT)) {
		uint64_t last = (uint64_t)(out[nout - 1].lcn +
					   out[nout - 1].len);

		/* A file's tail keeps its growing room (step 2a). */
		if ((flags & NTFS_AL_TAIL) &&
		    fs->opt.prealloc >= fs->cluster_size &&
		    (fs->opt.prealloc >> fs->cluster_bits) <
			    fs->nr_clusters - last)
			last += fs->opt.prealloc >> fs->cluster_bits;
		fs->data_hint = last < fs->nr_clusters ? last : fs->zone_end;
	}
	/* Data that had to go into the MFT zone shrinks it. */
	for (uint32_t k = 0; k < nout && !(flags & NTFS_AL_MFT); k++)
		if ((uint64_t)out[k].lcn >= fs->zone_start &&
		    (uint64_t)out[k].lcn < fs->zone_end)
			fs->zone_end = (uint64_t)out[k].lcn;
	ntfs_alloc_unlock(fs);
	ntfs_sf_publish(fs);
	*runs = out;
	*nruns = nout;
	return ST_OK;
fail:
	/* All or nothing: every run taken goes back. */
	for (uint32_t k = 0; k < nout; k++) {
		uint64_t ch;

		ntfs_bm_set(fs, (uint64_t)out[k].lcn, (uint64_t)out[k].len, 0,
			    &ch);
	}
	ntfs_alloc_unlock(fs);
	ntfs_free(out);
	return r;
}

/* ---- discard of freed clusters (option `discard') and trim ----
 *
 * A freed extent must not be discarded before the cleared bits that free
 * it are durable (a crash in between would leave $Bitmap saying "in use"
 * for clusters whose contents are gone -- harmless -- but the other order
 * matters more: the record that referenced the clusters was rewritten and
 * flushed before the bits were cleared, S27).  ntfs_discard_freed() only
 * queues the extent; ntfs_barrier() -- the device flush between ordered
 * steps, and the one at sync and unmount -- discards the queue after its
 * flush succeeded.  At that point each queued extent is checked against
 * $Bitmap again (under the metadata lock exclusive, which every allocation
 * holds): clusters allocated again in the meantime are skipped, so data
 * just placed there is never discarded.
 *
 * The device discards in units of its discard granularity: every range is
 * cut to whole units, its start aligned UP and its END aligned DOWN
 * (aligning the length instead would let a discard run past the free
 * extent into allocated clusters); a range without one whole unit is
 * skipped.  Sizes are 64-bit throughout. */

/* The first free extent in [from, to): *f its start, *n its length;
 * ST_NOT_FOUND when there is none.  Bitmap pages are indexed with
 * NTFS_BM_PAGE_BITS = PAGE_SIZE * 8 clusters each. */
static int ntfs_bm_free_extent(ntfs_fs_t *fs, uint64_t from, uint64_t to,
			       uint64_t *f, uint64_t *n)
{
	uint32_t scanned = 0;
	int r = ntfs_bm_next_free(fs, from, to, f, 0, &scanned);

	if (r != ST_OK)
		return r;
	return ntfs_bm_run(fs, *f, ~0ULL, to, n);
}

/* Discard the whole granularity units inside clusters [lcn, lcn + n). */
static int ntfs_discard_range(ntfs_fs_t *fs, uint64_t lcn, uint64_t n,
			      int explicit, uint64_t *done)
{
	uint64_t gran = fs->bdev->discard_granularity;
	uint64_t b0, b1, lba, cnt;
	int r;

	if (!gran)
		gran = fs->dev_ss;
	if (gran & (gran - 1) || gran < fs->dev_ss)
		gran = fs->dev_ss; /* odd reports: whole sectors only */
	b0 = lcn << fs->cluster_bits;
	b1 = (lcn + n) << fs->cluster_bits;
	b0 = (b0 + gran - 1) & ~(gran - 1);
	b1 &= ~(gran - 1);
	if (b1 <= b0)
		return ST_OK;
	lba = b0 >> fs->dev_ss_bits;
	cnt = (b1 - b0) >> fs->dev_ss_bits;
	while (cnt) {
		uint64_t m = cnt;

		if (fs->bdev->discard_max_sectors &&
		    m > fs->bdev->discard_max_sectors)
			m = fs->bdev->discard_max_sectors;
		r = ntfs_dev_discard(fs, lba, m, explicit);
		if (r != ST_OK)
			return r;
		lba += m;
		cnt -= m;
	}
	if (done)
		*done += (b1 - b0) >> fs->cluster_bits;
	return ST_OK;
}

/* Discard the still-free parts of [lcn, lcn + n) (bits checked now). */
static int ntfs_discard_free_part(ntfs_fs_t *fs, uint64_t lcn, uint64_t n,
				  int explicit, uint64_t *done)
{
	uint64_t end = lcn + n;

	while (lcn < end) {
		uint64_t f, len;
		int r = ntfs_bm_free_extent(fs, lcn, end, &f, &len);

		if (r == ST_NOT_FOUND)
			return ST_OK;
		if (r != ST_OK)
			return r;
		if (f >= end || !len)
			return ST_OK;
		if (len > end - f)
			len = end - f;
		r = ntfs_discard_range(fs, f, len, explicit, done);
		if (r != ST_OK)
			return r;
		lcn = f + len;
	}
	return ST_OK;
}

/* Discard what was queued (the device was just flushed).  Failures are
 * not errors of the filesystem: the clusters stay free either way. */
void ntfs_discard_run(ntfs_fs_t *fs)
{
	uint32_t n = fs->disc_n;

	ntfs_assert_meta_excl(fs);
	fs->disc_n = 0;
	if (!fs->opt.discard || !ntfs_writable(fs) || !fs->armed)
		return;
	for (uint32_t i = 0; i < n; i++) {
		if (ntfs_discard_free_part(fs, (uint64_t)fs->disc[i].lcn,
					   (uint64_t)fs->disc[i].len, 0,
					   &fs->discarded) != ST_OK)
			break;
	}
}

/* The discard hook of ntfs_clusters_free(): queue the extent (merged with
 * its neighbour when they touch); a full queue is flushed and discarded
 * right away. */
static void ntfs_discard_freed(ntfs_fs_t *fs, int64_t lcn, uint64_t count)
{
	if (!fs->opt.discard || !count || lcn < 0)
		return;
	if (fs->disc_n) {
		ntfs_run_t *l = &fs->disc[fs->disc_n - 1];

		if (l->lcn + l->len == lcn) {
			l->len += (int64_t)count;
			return;
		}
		if (lcn + (int64_t)count == l->lcn) {
			l->lcn = lcn;
			l->len += (int64_t)count;
			return;
		}
	}
	if (fs->disc_n == NTFS_DISC_MAX) {
		if (ntfs_dev_flush(fs) == ST_OK)
			ntfs_discard_run(fs);
		else
			fs->disc_n = 0; /* never discard unflushed frees */
	}
	fs->disc[fs->disc_n].vcn = 0;
	fs->disc[fs->disc_n].lcn = lcn;
	fs->disc[fs->disc_n].len = (int64_t)count;
	fs->disc_n++;
}

/* FITRIM-style trim of the free space in bytes [start, start + len):
 * every free extent of at least `minlen' bytes (at least the discard
 * granularity) inside the range is discarded in whole units.  Holds the
 * metadata lock exclusive, so nothing is allocated meanwhile.  *trimmed:
 * bytes discarded.  The VFS has no ioctl hook for filesystems, so no
 * system call reaches this yet; it is kept complete and tested by the
 * host unit test. */
NTFS_MAYBE_UNUSED static int ntfs_trim_fs(ntfs_fs_t *fs, uint64_t start,
				    uint64_t len, uint64_t minlen,
				    uint64_t *trimmed)
{
	uint64_t gran, c0, c1, lcn, done = 0, minc;
	int r = ST_OK;

	*trimmed = 0;
	if (!fs->bdev || !fs->bdev->discard_granularity)
		return ST_UNSUPPORTED;
	ntfs_meta_wlock(fs);
	if (!ntfs_writable(fs) || fs->hibernated) {
		ntfs_meta_wunlock(fs);
		return ST_ROFS;
	}
	gran = fs->bdev->discard_granularity;
	if (minlen < gran)
		minlen = gran; /* 64-bit maximum */
	minc = (minlen + fs->cluster_size - 1) >> fs->cluster_bits;
	/* Clusters wholly inside the byte range: start UP, end DOWN. */
	c0 = (start + fs->cluster_size - 1) >> fs->cluster_bits;
	c1 = len > ~0ULL - start ? ~0ULL : start + len;
	c1 >>= fs->cluster_bits;
	if (c1 > fs->nr_clusters)
		c1 = fs->nr_clusters;
	r = ntfs_bm_count(fs);
	lcn = c0;
	while (r == ST_OK && lcn < c1) {
		uint64_t f, n;

		r = ntfs_bm_free_extent(fs, lcn, c1, &f, &n);
		if (r == ST_NOT_FOUND) {
			r = ST_OK;
			break;
		}
		if (r != ST_OK || f >= c1 || !n)
			break;
		if (n > c1 - f)
			n = c1 - f;
		if (n >= minc)
			r = ntfs_discard_range(fs, f, n, 1, &done);
		lcn = f + n;
	}
	ntfs_meta_wunlock(fs);
	*trimmed = done << fs->cluster_bits;
	return r;
}
