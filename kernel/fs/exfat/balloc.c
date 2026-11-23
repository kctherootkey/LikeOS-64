/* LikeOS - exFAT filesystem driver
 *
 * The allocation bitmap, the cluster allocator, deferred frees.
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
 * 9. The allocation bitmap, the allocator, deferred frees
 * =========================================================================== */

/* Bit of cluster `cl': unit, byte in unit, bit. */
static inline void exfat_bm_pos(exfat_fs_t *fs, uint32_t cl, uint32_t *unit,
				unsigned *byte, unsigned *bit)
{
	uint64_t idx = (uint64_t)cl - EXFAT_FIRST_CLUSTER;
	uint64_t b = idx >> 3;

	*unit = (uint32_t)(b / fs->mcu_bytes);
	*byte = (unsigned)(b % fs->mcu_bytes);
	*bit = (unsigned)(idx & 7);
}

/* Is cluster `cl' allocated?  1, 0 or an ST_ error. */
int exfat_bm_test(exfat_fs_t *fs, uint32_t cl)
{
	uint32_t unit;
	unsigned byte, bit;
	exfat_mcu_t *u;
	int err, v;

	if (!exfat_cluster_ok(fs, cl))
		return ST_INVALID;
	exfat_bm_pos(fs, cl, &unit, &byte, &bit);
	u = exfat_mc_get(fs, EXFAT_MC_BITMAP, unit, &err);
	if (!u)
		return err;
	if (byte >= u->nbytes) {
		exfat_mc_put(fs, u);
		return ST_IO;
	}
	v = (u->data[byte] >> bit) & 1;
	exfat_mc_put(fs, u);
	return v;
}

/* Set (value 1) or clear (value 0) the bits of clusters [start, start+len).
 * Every bit must change: allocating an allocated cluster is a driver bug
 * (WARN), freeing a free one means the volume lied about its allocation
 * (corruption).  Metadata lock exclusive. */
int exfat_bm_change(exfat_fs_t *fs, uint32_t start, uint32_t len,
		    int value)
{
	uint64_t end;
	uint32_t cl = start;

	exfat_assert_excl(fs);
	if (len == 0)
		return ST_OK;
	if (!exfat_add_ok(start, (uint64_t)len - 1, &end) ||
	    !exfat_cluster_ok(fs, start) || !exfat_cluster_ok(fs, end)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	while ((uint64_t)cl <= end) {
		uint32_t unit;
		unsigned byte, bit, lo, hi = 0;
		int touched = 0, bad = 0;
		exfat_mcu_t *u;
		int err;

		exfat_bm_pos(fs, cl, &unit, &byte, &bit);
		u = exfat_mc_get(fs, EXFAT_MC_BITMAP, unit, &err);
		if (!u)
			return err;
		lo = byte;
		while ((uint64_t)cl <= end && byte < u->nbytes) {
			if (bit == 0 && (uint64_t)cl + 7 <= end) {
				/* A whole byte: every bit must flip. */
				uint8_t want = value ? 0xFF : 0x00;

				if (u->data[byte] != (uint8_t)~want) {
					bad = 1;
					break;
				}
				u->data[byte] = want;
				touched = 1;
				hi = byte;
				cl += 8;
				byte++;
				continue;
			}
			uint8_t m = (uint8_t)(1u << bit);

			if (!!(u->data[byte] & m) == !!value) {
				bad = 1;
				break;
			}
			if (value)
				u->data[byte] |= m;
			else
				u->data[byte] &= (uint8_t)~m;
			touched = 1;
			hi = byte;
			cl++;
			if (++bit == 8) {
				bit = 0;
				byte++;
			}
		}
		if (touched)
			exfat_mc_dirty(fs, u, lo, hi - lo + 1);
		exfat_mc_put(fs, u);
		if (bad) {
			if (value) {
				/* The allocator found these free a moment
				 * ago: a bug of ours, not the volume's. */
				WARN_ON_ONCE(1);
				return ST_IO;
			}
			return exfat_corrupt(fs, cl,
					     "freeing cluster %u, which is free",
					     cl);
		}
	}
	return ST_OK;
}

static inline unsigned exfat_popcount8(uint8_t v)
{
	v = (uint8_t)(v - ((v >> 1) & 0x55));
	v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
	return (unsigned)((v + (v >> 4)) & 0x0F);
}

/* Count the free clusters (statfs, the allocator).  Once per mount; the
 * count is kept up to date afterwards.  Metadata lock held (either mode:
 * concurrent shared counters compute the same number). */
int exfat_bm_count_free(exfat_fs_t *fs)
{
	uint64_t used = 0, bits_left = fs->cluster_count;
	uint64_t total = exfat_mc_region_bytes(fs, EXFAT_MC_BITMAP);
	uint32_t units, unit;

	exfat_assert_meta(fs);
	if (__atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE) >= 0)
		return ST_OK;
	if (!fs->have_bitmap)
		return ST_IO;
	units = (uint32_t)((total + fs->mcu_bytes - 1) / fs->mcu_bytes);
	for (unit = 0; unit < units && bits_left; unit++) {
		int err;
		exfat_mcu_t *u = exfat_mc_get(fs, EXFAT_MC_BITMAP, unit, &err);

		if (!u)
			return err;
		for (unsigned i = 0; i < u->nbytes && bits_left; i++) {
			uint8_t v = u->data[i];

			if (bits_left < 8) {
				v &= (uint8_t)((1u << bits_left) - 1);
				used += exfat_popcount8(v);
				bits_left = 0;
				break;
			}
			used += exfat_popcount8(v);
			bits_left -= 8;
		}
		exfat_mc_put(fs, u);
	}
	if (used > fs->cluster_count)
		used = fs->cluster_count;
	__atomic_store_n(&fs->free_clusters,
			 (int64_t)(fs->cluster_count - used), __ATOMIC_RELEASE);
	return ST_OK;
}

/* Adjust the free-cluster count by `delta' -- unless it is not known yet
 * (-1: nothing counted since the mount), then it stays unknown and the
 * next count reads the bitmap, which already holds the change. */
void exfat_free_count_add(exfat_fs_t *fs, int64_t delta)
{
	int64_t v = __atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE);

	if (v < 0)
		return;
	v += delta;
	if (v < 0) {
		WARN_ON_ONCE(1);
		v = 0;
	}
	if (v > (int64_t)fs->cluster_count)
		v = fs->cluster_count;
	__atomic_store_n(&fs->free_clusters, v, __ATOMIC_RELEASE);
}

/* Find the first free run at or after cluster `from' (no wrap), at most
 * `maxlen' long.  *start = 0 when none.  Metadata lock held. */
static int exfat_bm_find_run(exfat_fs_t *fs, uint32_t from, uint32_t maxlen,
			     uint32_t *start, uint32_t *len)
{
	uint64_t last = (uint64_t)fs->cluster_count + 1;
	uint64_t cl = from;
	uint32_t run_start = 0, run_len = 0;

	*start = 0;
	*len = 0;
	if (cl < EXFAT_FIRST_CLUSTER)
		cl = EXFAT_FIRST_CLUSTER;
	while (cl <= last) {
		uint32_t unit;
		unsigned byte, bit;
		exfat_mcu_t *u;
		int err;

		exfat_bm_pos(fs, (uint32_t)cl, &unit, &byte, &bit);
		u = exfat_mc_get(fs, EXFAT_MC_BITMAP, unit, &err);
		if (!u)
			return err;
		while (cl <= last && byte < u->nbytes) {
			uint8_t v = u->data[byte];

			if (bit == 0 && v == 0xFF && run_len == 0 &&
			    cl + 7 <= last) {
				cl += 8;
				byte++;
				continue;
			}
			if (!((v >> bit) & 1)) {
				if (run_len == 0)
					run_start = (uint32_t)cl;
				run_len++;
				if (run_len >= maxlen) {
					exfat_mc_put(fs, u);
					*start = run_start;
					*len = run_len;
					return ST_OK;
				}
			} else if (run_len) {
				exfat_mc_put(fs, u);
				*start = run_start;
				*len = run_len;
				return ST_OK;
			}
			cl++;
			if (++bit == 8) {
				bit = 0;
				byte++;
			}
		}
		exfat_mc_put(fs, u);
	}
	if (run_len) {
		*start = run_start;
		*len = run_len;
	}
	return ST_OK;
}

/* Free clusters following `last' (exclusive), up to `maxlen'. */
static int exfat_bm_free_after(exfat_fs_t *fs, uint32_t last, uint32_t maxlen,
			       uint32_t *len)
{
	uint32_t s, l;
	int r;

	*len = 0;
	if (!last || (uint64_t)last + 1 > (uint64_t)fs->cluster_count + 1)
		return ST_OK;
	r = exfat_bm_test(fs, last + 1);
	if (r < 0)
		return r;
	if (r == 1)
		return ST_OK;
	r = exfat_bm_find_run(fs, last + 1, maxlen, &s, &l);
	if (r != ST_OK)
		return r;
	if (s == last + 1)
		*len = l;
	return ST_OK;
}

/* Allocate `n' clusters, preferring contiguity: first the clusters right
 * after `last' (the stream's current end), then ONE free run big enough for
 * the rest (next fit from the allocation cursor, wrapping once), and only
 * when there is none, the free runs in order.  The runs are appended to
 * *runs (kalloc'd array, caller frees).  The bits are set in the bitmap
 * before this returns (allocate before reference).  ST_NOSPC when the
 * volume has fewer than `n' free clusters (nothing is allocated then).
 * Metadata lock exclusive; the mount is armed. */
int exfat_alloc(exfat_fs_t *fs, uint32_t last, uint32_t n,
		struct exfat_run **runs, unsigned *nruns)
{
	unsigned cap = 0;
	uint32_t got = 0;
	int r;

	exfat_assert_excl(fs);
	*runs = NULL;
	*nruns = 0;
	if (n == 0)
		return ST_OK;
	if (!fs->have_bitmap)
		return ST_ROFS;
	r = exfat_bm_count_free(fs);
	if (r != ST_OK)
		return r;
	if ((uint64_t)fs->free_clusters < n)
		return ST_NOSPC;

	exfat_alloc_lock(fs);

#define EXFAT_ADD_RUN(s_, l_)                                                 \
	do {                                                                  \
		if (*nruns >= cap) {                                          \
			unsigned nc_ = cap ? cap * 2 : 8;                     \
			struct exfat_run *nr_ = (struct exfat_run *)krealloc( \
				*runs, nc_ * sizeof(**runs));                 \
			if (!nr_) {                                           \
				r = ST_NOMEM;                                 \
				goto fail;                                    \
			}                                                     \
			*runs = nr_;                                          \
			cap = nc_;                                            \
		}                                                             \
		(*runs)[*nruns].start = (s_);                                 \
		(*runs)[*nruns].len = (l_);                                   \
		(*nruns)++;                                                   \
		r = exfat_bm_change(fs, (s_), (l_), 1);                       \
		if (r != ST_OK) {                                             \
			(*nruns)--;                                           \
			goto fail;                                            \
		}                                                             \
		got += (l_);                                                  \
	} while (0)

	/* 1. Continue the stream in place. */
	if (last) {
		uint32_t l;

		r = exfat_bm_free_after(fs, last, n, &l);
		if (r != ST_OK)
			goto fail;
		if (l)
			EXFAT_ADD_RUN(last + 1, l);
	}
	/* 2. One run for the rest: next fit from the cursor, then from the
	 *    start of the heap. */
	if (got < n) {
		uint32_t need = n - got;

		for (int pass = 0; pass < 2 && got < n; pass++) {
			uint32_t from = pass == 0 ? fs->alloc_cursor :
						    EXFAT_FIRST_CLUSTER;
			uint64_t stop = pass == 0 ?
						(uint64_t)fs->cluster_count + 2 :
						(uint64_t)fs->alloc_cursor;

			while ((uint64_t)from < stop) {
				uint32_t s, l;

				r = exfat_bm_find_run(fs, from, need, &s, &l);
				if (r != ST_OK)
					goto fail;
				if (!s || (pass == 1 && (uint64_t)s >= stop))
					break;
				if (l >= need) {
					EXFAT_ADD_RUN(s, need);
					break;
				}
				from = s + l;
			}
		}
	}
	/* 3. Fragmented: whatever runs there are, in order. */
	{
		uint32_t from = fs->alloc_cursor;
		int wrapped = 0;

		while (got < n) {
			uint32_t s, l;

			r = exfat_bm_find_run(fs, from, n - got, &s, &l);
			if (r != ST_OK)
				goto fail;
			if (!s) {
				if (wrapped)
					break;
				wrapped = 1;
				from = EXFAT_FIRST_CLUSTER;
				continue;
			}
			EXFAT_ADD_RUN(s, l);
			from = s + l;
		}
	}
#undef EXFAT_ADD_RUN
	if (got < n) {
		/* The free count said there was room: the bitmap disagrees
		 * with it.  Undo and report. */
		r = exfat_corrupt(fs, n, "free count larger than the bitmap's");
		goto fail;
	}
	exfat_free_count_add(fs, -(int64_t)n);
	{
		struct exfat_run *lr = &(*runs)[*nruns - 1];
		uint64_t nxt = (uint64_t)lr->start + lr->len;

		fs->alloc_cursor = nxt > (uint64_t)fs->cluster_count + 1 ?
					   EXFAT_FIRST_CLUSTER :
					   (uint32_t)nxt;
	}
	exfat_alloc_unlock(fs);
	return ST_OK;
fail:
	/* Give back what was taken (never written anywhere yet). */
	for (unsigned i = 0; i < *nruns; i++)
		exfat_bm_change(fs, (*runs)[i].start, (*runs)[i].len, 0);
	kfree(*runs);
	*runs = NULL;
	*nruns = 0;
	exfat_alloc_unlock(fs);
	return r;
}

/* Free clusters [start, start + len) -- LATER: they stay allocated in the
 * bitmap until the commit has the entry sets that no longer reference them
 * on the device (dereference before free).  Callers change the in-memory
 * state first, so the early commit this may run sees a consistent volume.
 * Metadata lock exclusive. */
static int exfat_free_defer(exfat_fs_t *fs, uint32_t start, uint32_t len)
{
	uint64_t end;

	exfat_assert_excl(fs);
	if (len == 0)
		return ST_OK;
	if (!exfat_add_ok(start, (uint64_t)len - 1, &end) ||
	    !exfat_cluster_ok(fs, start) || !exfat_cluster_ok(fs, end))
		return exfat_corrupt(fs, start, "freeing clusters %u+%u out of range",
				     start, len);
	if (fs->npf > 0) {
		struct exfat_run *l = &fs->pf[fs->npf - 1];

		if ((uint64_t)l->start + l->len == start) {
			l->len += len;
			fs->pf_clusters += len;
			return ST_OK;
		}
	}
	if (fs->npf >= EXFAT_PENDING_FREE_MAX) {
		int r = fs->in_commit ? ST_BUSY : exfat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	if (fs->npf >= fs->pf_cap) {
		unsigned nc = fs->pf_cap ? fs->pf_cap * 2 : 64;
		struct exfat_run *n;

		if (nc > EXFAT_PENDING_FREE_MAX)
			nc = EXFAT_PENDING_FREE_MAX;
		n = (struct exfat_run *)krealloc(fs->pf, nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		fs->pf = n;
		fs->pf_cap = nc;
	}
	fs->pf[fs->npf].start = start;
	fs->pf[fs->npf].len = len;
	fs->npf++;
	fs->pf_clusters += len;
	return ST_OK;
}

/* Free every cluster an extent array describes (deferred). */
int exfat_free_extents(exfat_fs_t *fs, const exfat_extent_t *ext,
		       uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		int r = exfat_free_defer(fs, ext[i].disk, ext[i].len);

		if (r != ST_OK)
			return r;
	}
	return ST_OK;
}

/* Room for `runs' more deferred frees and `fats' more deferred FAT writes,
 * made BEFORE an operation changes anything: the frees that follow its
 * change cannot fail for want of memory then (a full list is committed
 * instead, which only a device error can fail).  Metadata lock exclusive. */
int exfat_free_reserve(exfat_fs_t *fs, uint64_t runs, unsigned fats)
{
	uint64_t want = (uint64_t)fs->npf + runs;

	exfat_assert_excl(fs);
	if (want > EXFAT_PENDING_FREE_MAX)
		want = EXFAT_PENDING_FREE_MAX;
	if (want > fs->pf_cap) {
		struct exfat_run *n = (struct exfat_run *)krealloc(
			fs->pf, (size_t)want * sizeof(*n));

		if (!n)
			return ST_NOMEM;
		fs->pf = n;
		fs->pf_cap = (unsigned)want;
	}
	want = (uint64_t)fs->ndfat + fats;
	if (want > EXFAT_DEFERRED_FAT_MAX)
		want = EXFAT_DEFERRED_FAT_MAX;
	if (want > fs->dfat_cap) {
		struct exfat_fatw *n = (struct exfat_fatw *)krealloc(
			fs->dfat, (size_t)want * sizeof(*n));

		if (!n)
			return ST_NOMEM;
		fs->dfat = n;
		fs->dfat_cap = (unsigned)want;
	}
	return ST_OK;
}
