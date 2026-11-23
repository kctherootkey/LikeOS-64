/* LikeOS - exFAT filesystem driver
 *
 * FAT entries, cluster chains, growing and shrinking streams.
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
 * 8. FAT entries and chains
 * =========================================================================== */

/* A FAT reading cursor: keeps the last unit held across lookups, so a chain
 * walk touches the cache once per unit. */
struct exfat_fatcur {
	exfat_mcu_t *u;
};

static void exfat_fatcur_done(exfat_fs_t *fs, struct exfat_fatcur *c)
{
	if (c->u)
		exfat_mc_put(fs, c->u);
	c->u = NULL;
}

/* FAT[cl] through the cursor.  Metadata lock held. */
static int exfat_fat_read(exfat_fs_t *fs, struct exfat_fatcur *c, uint32_t cl,
			  uint32_t *val)
{
	uint64_t off;
	uint32_t unit;
	int err;

	if (!exfat_cluster_ok(fs, cl))
		return ST_INVALID;
	off = (uint64_t)cl * 4;
	unit = (uint32_t)(off / fs->mcu_bytes);
	if (!c->u || c->u->region != EXFAT_MC_FAT || c->u->unit != unit) {
		exfat_fatcur_done(fs, c);
		c->u = exfat_mc_get(fs, EXFAT_MC_FAT, unit, &err);
		if (!c->u)
			return err;
	}
	off %= fs->mcu_bytes;
	if (off + 4 > c->u->nbytes)
		return exfat_corrupt(fs, cl, "FAT entry %u beyond the FAT", cl);
	*val = exfat_le32(c->u->data + off);
	return ST_OK;
}

/* Remove a deferred FAT write of `cl' (it is being written now). */
static void exfat_fat_cancel_deferred(exfat_fs_t *fs, uint32_t cl)
{
	for (unsigned i = 0; i < fs->ndfat; i++)
		if (fs->dfat[i].cluster == cl) {
			fs->dfat[i] = fs->dfat[fs->ndfat - 1];
			fs->ndfat--;
			return;
		}
}

/* FAT[cl] = val, in the cache (written at the commit).  Metadata lock
 * exclusive; the mount is armed. */
static int exfat_fat_write(exfat_fs_t *fs, struct exfat_fatcur *c,
			   uint32_t cl, uint32_t val)
{
	uint64_t off;
	uint32_t unit;
	int err;

	exfat_assert_excl(fs);
	if (!exfat_cluster_ok(fs, cl)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	exfat_fat_cancel_deferred(fs, cl);
	off = (uint64_t)cl * 4;
	unit = (uint32_t)(off / fs->mcu_bytes);
	if (!c->u || c->u->region != EXFAT_MC_FAT || c->u->unit != unit) {
		exfat_fatcur_done(fs, c);
		c->u = exfat_mc_get(fs, EXFAT_MC_FAT, unit, &err);
		if (!c->u)
			return err;
	}
	off %= fs->mcu_bytes;
	if (off + 4 > c->u->nbytes)
		return ST_IO;
	if (exfat_le32(c->u->data + off) == val)
		return ST_OK;
	exfat_put32(c->u->data + off, val);
	exfat_mc_dirty(fs, c->u, (unsigned)off, 4);
	return ST_OK;
}

/* Ask for FAT[cl] = val once the dereferencing entry sets are on the device
 * (a truncation's new end of chain).  Metadata lock exclusive. */
static int exfat_fat_defer(exfat_fs_t *fs, uint32_t cl, uint32_t val)
{
	exfat_assert_excl(fs);
	exfat_fat_cancel_deferred(fs, cl);
	if (fs->ndfat >= EXFAT_DEFERRED_FAT_MAX) {
		int r = fs->in_commit ? ST_BUSY : exfat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	if (fs->ndfat >= fs->dfat_cap) {
		unsigned nc = fs->dfat_cap ? fs->dfat_cap * 2 : 32;
		struct exfat_fatw *n;

		if (nc > EXFAT_DEFERRED_FAT_MAX)
			nc = EXFAT_DEFERRED_FAT_MAX;
		n = (struct exfat_fatw *)krealloc(fs->dfat, nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		fs->dfat = n;
		fs->dfat_cap = nc;
	}
	fs->dfat[fs->ndfat].cluster = cl;
	fs->dfat[fs->ndfat].value = val;
	fs->ndfat++;
	return ST_OK;
}

/* Append an extent to an array (merging with the last one when it simply
 * continues it).  ST_NOMEM past EXFAT_MAX_EXTENTS. */
static int exfat_ext_append(exfat_extent_t **ext, uint32_t *next,
			    uint32_t *cap, uint32_t fidx, uint32_t disk,
			    uint32_t len)
{
	if (len == 0)
		return ST_OK;
	if (*next > 0) {
		exfat_extent_t *l = &(*ext)[*next - 1];

		if (l->fidx + l->len == fidx && l->disk + l->len == disk) {
			l->len += len;
			return ST_OK;
		}
	}
	if (*next >= *cap) {
		uint32_t nc = *cap ? *cap * 2 : 4;
		exfat_extent_t *n;

		if (nc > EXFAT_MAX_EXTENTS)
			nc = EXFAT_MAX_EXTENTS;
		if (*next >= nc)
			return ST_NOMEM;
		n = (exfat_extent_t *)krealloc(*ext, nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		*ext = n;
		*cap = nc;
	}
	(*ext)[*next].fidx = fidx;
	(*ext)[*next].disk = disk;
	(*ext)[*next].len = len;
	(*ext)[*next]._pad = 0;
	(*next)++;
	return ST_OK;
}

/* Decode a stream's allocation into extents.
 *
 *   nofat      the NoFatChain flag: `want' clusters from `first', no FAT
 *   want       clusters the stream must have (from its DataLength), or 0
 *              to follow the chain to its end (the root directory), then
 *              bounded by `max'
 *
 * Every cluster number is range-checked; a chain that ends early, holds a
 * bad or free mark, or leaves the heap is corrupt (ST_IO).  The walk is
 * bounded by `want' (or `max'), so a cycle in the FAT cannot hang it.
 * Metadata lock held. */
int exfat_chain_decode(exfat_fs_t *fs, uint32_t first, int nofat,
		       uint64_t want, uint64_t max, exfat_extent_t **ext,
		       uint32_t *next, uint32_t *cap, uint32_t *ncl)
{
	struct exfat_fatcur cur = { NULL };
	uint64_t count = 0, limit;
	uint32_t cl = first;
	int r = ST_OK;

	*next = 0;
	*ncl = 0;
	if (first == 0 && want == 0)
		return ST_OK;
	if (!exfat_cluster_ok(fs, first))
		return exfat_corrupt(fs, first, "first cluster %u out of range",
				     first);
	if (nofat) {
		uint64_t last;

		if (want == 0 || want > fs->cluster_count ||
		    !exfat_add_ok(first, want - 1, &last) ||
		    !exfat_cluster_ok(fs, last))
			return exfat_corrupt(fs, first,
					     "contiguous run %u+%llu leaves the heap",
					     first, (unsigned long long)want);
		r = exfat_ext_append(ext, next, cap, 0, first, (uint32_t)want);
		if (r == ST_OK)
			*ncl = (uint32_t)want;
		return r;
	}
	limit = want ? want : max;
	if (limit > fs->cluster_count)
		limit = fs->cluster_count;
	for (;;) {
		uint32_t val = 0;

		r = exfat_ext_append(ext, next, cap, (uint32_t)count, cl, 1);
		if (r != ST_OK)
			goto out;
		count++;
		if (want && count == want)
			break; /* the chain may go on: DataLength rules */
		r = exfat_fat_read(fs, &cur, cl, &val);
		if (r != ST_OK)
			goto out;
		if (val == EXFAT_FAT_EOC) {
			if (want) {
				r = exfat_corrupt(fs, first,
						  "chain from %u ends after %llu of %llu clusters",
						  first,
						  (unsigned long long)count,
						  (unsigned long long)want);
				goto out;
			}
			break;
		}
		if (!exfat_cluster_ok(fs, val)) {
			r = exfat_corrupt(fs, first,
					  "chain from %u holds FAT value 0x%x at cluster %u",
					  first, val, cl);
			goto out;
		}
		if (count >= limit) {
			r = exfat_corrupt(fs, first,
					  "chain from %u longer than %llu clusters (loop?)",
					  first, (unsigned long long)limit);
			goto out;
		}
		cl = val;
	}
	*ncl = (uint32_t)count;
out:
	exfat_fatcur_done(fs, &cur);
	if (r != ST_OK)
		*next = 0;
	return r;
}

/* The disk cluster holding stream cluster `ci', or 0.  Binary search. */
uint32_t exfat_ext_lookup(const exfat_extent_t *ext, uint32_t n,
			  uint64_t ci)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const exfat_extent_t *e = &ext[mid];

		if (ci < e->fidx)
			hi = mid;
		else if (ci >= (uint64_t)e->fidx + e->len)
			lo = mid + 1;
		else
			return e->disk + (uint32_t)(ci - e->fidx);
	}
	return 0;
}

/* ===========================================================================
 * 18. Cluster chains of streams
 * =========================================================================== */

/* The value a deferred FAT write holds for `cl', or 0 when none. */
static int exfat_fat_deferred(exfat_fs_t *fs, uint32_t cl, uint32_t *val)
{
	for (unsigned i = 0; i < fs->ndfat; i++)
		if (fs->dfat[i].cluster == cl) {
			*val = fs->dfat[i].value;
			return 1;
		}
	return 0;
}

/* Note that FAT[cl] (a stream's old last cluster, `old' before) now links
 * to new clusters: exfat_mc_flush_links() writes it after them.  The first
 * note of a cluster keeps the value the device has. */
static void exfat_link_note(exfat_fs_t *fs, uint32_t cl, uint32_t old)
{
	for (unsigned i = 0; i < fs->nlinks; i++)
		if (fs->links[i].cluster == cl)
			return;
	if (fs->nlinks == EXFAT_LINKS_MAX) {
		/* Full: what is noted goes to the device now (two steps,
		 * as always), then this one starts the next list. */
		if (exfat_mc_flush(fs) != ST_OK)
			return; /* written in one step, at worst */
	}
	fs->links[fs->nlinks].cluster = cl;
	fs->links[fs->nlinks].old = old;
	fs->nlinks++;
}

/* Append the allocated runs to a stream: extents and FAT links.  A stream
 * that is empty or contiguous stays (becomes) NoFatChain while the runs
 * continue it without a gap; otherwise it becomes a FAT chain, its existing
 * clusters linked first.  The root directory is always a FAT chain.
 *
 * ALL OR NOTHING.  The extent array is grown before anything changes, so
 * the only step that can fail is a FAT write (a metadata unit that cannot
 * be read or allocated); then the one FAT entry of the stream that was
 * changed -- the old last cluster's -- gets its old value back (and its
 * deferred truncation mark, if it had one), and the stream in memory is
 * untouched.  Every other FAT entry written belongs to clusters the stream
 * does not reach (the runs, which the caller frees, or the clusters of a
 * stream that stays NoFatChain).  Should even that restore fail, *pinned
 * says so: the caller must then keep the runs allocated (lost clusters, not
 * clusters a chain on the device may lead into while they are free).
 * Metadata lock exclusive, armed; the runs are allocated in the bitmap. */
int exfat_stream_append(exfat_fs_t *fs, exfat_stream_t *st,
			const struct exfat_run *runs, unsigned nruns,
			int *pinned)
{
	struct exfat_fatcur cur = { NULL };
	uint32_t last = exfat_st_last(st), old_last_val = 0, def_val = 0;
	uint64_t total = 0;
	int contiguous = 1, nofat, chained, had_def = 0, touched_last = 0;
	int r = ST_OK;

	exfat_assert_excl(fs);
	*pinned = 0;
	if (nruns == 0)
		return ST_OK;
	for (unsigned i = 0; i < nruns; i++) {
		total += runs[i].len;
		if (i > 0 && runs[i].start != runs[i - 1].start + runs[i - 1].len)
			contiguous = 0;
	}
	if (st->nclusters && runs[0].start != last + 1)
		contiguous = 0;
	if ((uint64_t)st->nclusters + total > fs->cluster_count)
		return ST_NOSPC;
	nofat = !st->is_root &&
		(st->nclusters == 0 || (st->sflags & EXFAT_SF_NO_FAT_CHAIN));
	/* Was the stream a FAT chain the device can follow?  Then its last
	 * cluster's entry is the one link a failure must put back. */
	chained = st->nclusters && !nofat;

	/* 1. Room in the extent array first: appending cannot fail later. */
	if ((uint64_t)st->next + nruns > st->ext_cap) {
		uint64_t want = (uint64_t)st->next + nruns;
		uint64_t nc = st->ext_cap ? (uint64_t)st->ext_cap * 2 : 4;
		exfat_extent_t *n;

		if (nc < want)
			nc = want;
		if (nc > EXFAT_MAX_EXTENTS)
			nc = EXFAT_MAX_EXTENTS;
		if (nc < want)
			return ST_NOMEM;
		n = (exfat_extent_t *)krealloc(st->ext, (size_t)nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		st->ext = n;
		st->ext_cap = (uint32_t)nc;
	}
	if (chained) {
		r = exfat_fat_read(fs, &cur, last, &old_last_val);
		if (r != ST_OK) {
			exfat_fatcur_done(fs, &cur);
			return r;
		}
		had_def = exfat_fat_deferred(fs, last, &def_val);
	}

	/* 2. The FAT. */
	if (nofat && !contiguous && st->nclusters) {
		/* Conversion: link the existing clusters in the FAT.  The
		 * stream stays NoFatChain until step 3, so these entries are
		 * nobody's business should a later write fail. */
		for (uint32_t i = 0; i < st->next && r == ST_OK; i++) {
			const exfat_extent_t *e = &st->ext[i];

			for (uint32_t k = 0; k + 1 < e->len && r == ST_OK; k++)
				r = exfat_fat_write(fs, &cur, e->disk + k,
						    e->disk + k + 1);
			if (r == ST_OK && i + 1 < st->next)
				r = exfat_fat_write(fs, &cur,
						    e->disk + e->len - 1,
						    st->ext[i + 1].disk);
		}
	}
	if (!(nofat && contiguous)) {
		/* FAT chain: link the old end to the first new cluster, the
		 * runs internally and to each other, and end the chain. */
		if (st->nclusters && r == ST_OK) {
			touched_last = 1;
			r = exfat_fat_write(fs, &cur, last, runs[0].start);
		}
		for (unsigned i = 0; i < nruns && r == ST_OK; i++) {
			for (uint32_t k = 0; k + 1 < runs[i].len && r == ST_OK;
			     k++)
				r = exfat_fat_write(fs, &cur,
						    runs[i].start + k,
						    runs[i].start + k + 1);
			if (r != ST_OK)
				break;
			r = exfat_fat_write(fs, &cur,
					    runs[i].start + runs[i].len - 1,
					    i + 1 < nruns ? runs[i + 1].start :
							    EXFAT_FAT_EOC);
		}
	}
	if (r != ST_OK) {
		/* Undo the one link the stream's chain can lead through. */
		if (chained && touched_last) {
			int r2 = exfat_fat_write(fs, &cur, last, old_last_val);

			if (r2 == ST_OK && had_def)
				r2 = exfat_fat_defer(fs, last, def_val);
			if (r2 != ST_OK) {
				*pinned = 1;
				WARN_RATELIMIT(1, "exfat: %s: cannot undo a FAT link (%d): %u clusters stay allocated",
					       exfat_name_of(fs), r2,
					       (unsigned)total);
			}
		}
		exfat_fatcur_done(fs, &cur);
		return r;
	}
	exfat_fatcur_done(fs, &cur);
	if (chained && touched_last)
		exfat_link_note(fs, last, old_last_val);

	/* 3. Memory: cannot fail (room reserved above). */
	for (unsigned i = 0; i < nruns; i++) {
		r = exfat_ext_append(&st->ext, &st->next, &st->ext_cap,
				     st->nclusters, runs[i].start, runs[i].len);
		BUG_ON(r != ST_OK); /* capacity was reserved */
		st->nclusters += runs[i].len;
	}
	if (st->first_cluster == 0 || st->nclusters == total)
		st->first_cluster = runs[0].start;
	if (!st->is_root) {
		st->sflags |= EXFAT_SF_ALLOC_POSSIBLE;
		if (nofat && contiguous)
			st->sflags |= EXFAT_SF_NO_FAT_CHAIN;
		else
			st->sflags &= (uint8_t)~EXFAT_SF_NO_FAT_CHAIN;
	}
	return ST_OK;
}

/* Give runs that nothing references back to the bitmap (an allocation whose
 * append failed). */
void exfat_runs_unalloc(exfat_fs_t *fs, const struct exfat_run *runs,
			unsigned nruns, uint32_t total, int pinned)
{
	if (pinned) {
		fs->needs_check = 1;
		return; /* lost clusters: see exfat_stream_append() */
	}
	for (unsigned i = 0; i < nruns; i++)
		exfat_bm_change(fs, runs[i].start, runs[i].len, 0);
	exfat_free_count_add(fs, total);
}

/* Grow a stream's allocation to `ncl' clusters: all or nothing.  Metadata
 * lock exclusive, armed. */
int exfat_stream_grow(exfat_fs_t *fs, exfat_stream_t *st, uint32_t ncl)
{
	struct exfat_run *runs;
	unsigned nruns;
	uint32_t add;
	int r, pinned = 0;

	exfat_assert_excl(fs);
	if (ncl <= st->nclusters)
		return ST_OK;
	add = ncl - st->nclusters;
	r = exfat_alloc(fs, exfat_st_last(st), add, &runs, &nruns);
	if (r != ST_OK)
		return r;
	r = exfat_stream_append(fs, st, runs, nruns, &pinned);
	if (r != ST_OK)
		exfat_runs_unalloc(fs, runs, nruns, add, pinned);
	kfree(runs);
	return r;
}

/* What cutting a stream's allocation to `ncl' clusters frees -- the
 * extents past it, in *tail (kfree) and *ntail -- with room for those
 * deferred frees and the new end mark reserved.  Nothing changes: the
 * caller may still back out.  Metadata lock exclusive. */
int exfat_shrink_prepare(exfat_fs_t *fs, exfat_stream_t *st,
			 uint32_t ncl, exfat_extent_t **tail,
			 uint32_t *ntail)
{
	int fat = !(st->sflags & EXFAT_SF_NO_FAT_CHAIN);
	uint32_t cap = 0;
	int r;

	exfat_assert_excl(fs);
	*tail = NULL;
	*ntail = 0;
	if (ncl >= st->nclusters)
		return ST_OK;
	for (uint32_t i = 0; i < st->next; i++) {
		exfat_extent_t *e = &st->ext[i];
		uint64_t eend = (uint64_t)e->fidx + e->len;

		if (eend <= ncl)
			continue;
		uint32_t skip = e->fidx < ncl ? ncl - e->fidx : 0;

		r = exfat_ext_append(tail, ntail, &cap, e->fidx + skip,
				     e->disk + skip, e->len - skip);
		if (r != ST_OK)
			goto fail;
	}
	r = exfat_free_reserve(fs, *ntail, fat && ncl > 0);
	if (r == ST_OK)
		return ST_OK;
fail:
	if (*tail)
		kfree(*tail);
	*tail = NULL;
	*ntail = 0;
	return r;
}

/* The stream in memory cut to `ncl' clusters. */
static void exfat_shrink_cut(exfat_stream_t *st, uint32_t ncl)
{
	uint32_t keep = 0;

	for (uint32_t i = 0; i < st->next; i++) {
		exfat_extent_t *e = &st->ext[i];

		if (e->fidx >= ncl)
			break;
		if ((uint64_t)e->fidx + e->len > ncl)
			e->len = ncl - e->fidx;
		keep = i + 1;
	}
	st->next = keep;
	st->nclusters = ncl;
	if (ncl == 0) {
		st->first_cluster = 0;
		st->sflags &= (uint8_t)~EXFAT_SF_NO_FAT_CHAIN;
	}
}

/* Cut a stream's allocation to `ncl' clusters with what
 * exfat_shrink_prepare() found: the in-memory state changes now, the
 * clusters are freed (deferred) and, for a FAT chain, the new last cluster
 * gets its end mark at the commit (after the entry set with the shorter
 * length is stored).  Takes `tail'.  Fails only when a full deferred list
 * cannot be committed (a device error): then what is left of the tail stays
 * allocated, lost.  Metadata lock exclusive. */
int exfat_shrink_apply(exfat_fs_t *fs, exfat_stream_t *st,
		       uint32_t ncl, exfat_extent_t *tail,
		       uint32_t ntail)
{
	int fat = !(st->sflags & EXFAT_SF_NO_FAT_CHAIN);
	int r = ST_OK;

	exfat_assert_excl(fs);
	if (ncl >= st->nclusters) {
		if (tail)
			kfree(tail);
		return ST_OK;
	}
	exfat_shrink_cut(st, ncl);
	if (fat && ncl > 0)
		r = exfat_fat_defer(fs, exfat_st_last(st), EXFAT_FAT_EOC);
	if (r == ST_OK)
		r = exfat_free_extents(fs, tail, ntail);
	if (r != ST_OK)
		fs->needs_check = 1;
	if (tail)
		kfree(tail);
	return r;
}

/* Cut a stream's allocation to `ncl' clusters, where backing out is not an
 * option (a failed write gives back what it allocated): the stream in
 * memory is cut whatever happens.  Should the frees not be possible (no
 * memory), the clusters past the cut stay allocated, lost -- and for a FAT
 * chain the chain on the device stays longer than the file -- until the
 * volume is checked: it stays marked dirty.  Metadata lock exclusive. */
int exfat_stream_shrink(exfat_fs_t *fs, exfat_stream_t *st, uint32_t ncl)
{
	exfat_extent_t *tail;
	uint32_t ntail;
	int r;

	exfat_assert_excl(fs);
	if (ncl >= st->nclusters)
		return ST_OK;
	r = exfat_shrink_prepare(fs, st, ncl, &tail, &ntail);
	if (r != ST_OK) {
		exfat_shrink_cut(st, ncl);
		fs->needs_check = 1;
		return r;
	}
	return exfat_shrink_apply(fs, st, ncl, tail, ntail);
}

/* The clusters of a set's vendor allocation secondaries (the set is about
 * to be deleted), all in one extent array: *ext (kfree) and *n.  Decoded
 * before the set goes, so that a failure here changes nothing.  Metadata
 * lock exclusive. */
int exfat_secondary_allocs(exfat_fs_t *fs, const uint8_t *set,
			   exfat_extent_t **out, uint32_t *nout)
{
	unsigned sc = set[EXFAT_DE_SECONDARY_COUNT];
	uint32_t ocap = 0;

	*out = NULL;
	*nout = 0;
	for (unsigned i = 2; i <= sc; i++) {
		const uint8_t *x = set + i * EXFAT_DENTRY_SIZE;
		uint8_t t = x[EXFAT_DE_TYPE];
		uint32_t first;
		uint64_t len, want;
		exfat_extent_t *ext = NULL;
		uint32_t next = 0, cap = 0, ncl = 0;
		int r;

		if (t == EXFAT_ET_NAME || t == EXFAT_ET_STREAM ||
		    !(x[EXFAT_DE_GSFLAGS] & EXFAT_SF_ALLOC_POSSIBLE))
			continue;
		first = exfat_le32(x + EXFAT_DE_FIRST_CLUSTER);
		len = exfat_le64(x + EXFAT_DE_DATA_LENGTH);
		if (!first || !len)
			continue;
		want = (len + fs->cs - 1) >> fs->cs_shift;
		r = exfat_chain_decode(fs, first,
				       (x[EXFAT_DE_GSFLAGS] &
					EXFAT_SF_NO_FAT_CHAIN) != 0,
				       want, 0, &ext, &next, &cap, &ncl);
		for (uint32_t k = 0; r == ST_OK && k < next; k++)
			r = exfat_ext_append(out, nout, &ocap, 0, ext[k].disk,
					     ext[k].len);
		if (ext)
			kfree(ext);
		if (r != ST_OK) {
			if (*out)
				kfree(*out);
			*out = NULL;
			*nout = 0;
			return r;
		}
	}
	return ST_OK;
}
