/* LikeOS - FAT filesystem driver: FAT entries (12, 16 and 32 bits) and the
 * cluster allocator: free count, free runs, deferred frees and marks
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ===========================================================================
 * 8. FAT entries and chains
 *
 * Entries are 12, 16 or 28 bits (the top four bits of a FAT32 entry are
 * reserved and kept as found when an entry is written).  Values: 0 free,
 * 1 reserved, 2 .. cluster_count + 1 the next cluster, the bad-cluster mark,
 * and end-of-chain from eoc_min up.  Anything else in a chain is
 * corruption.
 * =========================================================================== */

/* Byte offset of entry `cl' inside one FAT. */
static inline uint64_t fat_ent_off(fat_fs_t *fs, uint32_t cl)
{
	switch (fs->fat_bits) {
	case 12:
		return (uint64_t)cl + (cl >> 1);
	case 16:
		return (uint64_t)cl * 2;
	default:
		return (uint64_t)cl * 4;
	}
}

/* Bytes of one FAT holding entries 0 .. n - 1. */
uint64_t fat_ent_bytes(int bits, uint64_t n)
{
	switch (bits) {
	case 12:
		return (n * 3 + 1) / 2;
	case 16:
		return n * 2;
	default:
		return n * 4;
	}
}

void fat_fatcur_done(fat_fs_t *fs, struct fat_fatcur *c)
{
	if (c->u)
		fat_mc_put(fs, c->u);
	c->u = NULL;
}

/* The unit holding entry `cl' through the cursor, and the entry's offset in
 * it.  Metadata lock held. */
int fat_ent_locate(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			  unsigned *in)
{
	uint64_t off = fat_ent_off(fs, cl);
	uint32_t unit = (uint32_t)(off / fs->mcu_bytes);
	unsigned need = fs->fat_bits == 32 ? 4 : 2;
	int err;

	if (!c->u || c->u->unit != unit) {
		fat_fatcur_done(fs, c);
		c->u = fat_mc_get(fs, unit, &err);
		if (!c->u)
			return err;
	}
	off %= fs->mcu_bytes;
	if (off + need > c->u->nbytes)
		return fat_corrupt(fs, cl, "FAT entry %u beyond the FAT", cl);
	*in = (unsigned)off;
	return ST_OK;
}

/* FAT[cl] for any entry of the FAT (0 and 1 included).  Metadata lock
 * held. */
int fat_ent_get(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
		       uint32_t *val)
{
	unsigned in;
	int r;

	if (cl >= fs->cluster_end)
		return ST_INVALID;
	r = fat_ent_locate(fs, c, cl, &in);
	if (r != ST_OK)
		return r;
	switch (fs->fat_bits) {
	case 12: {
		uint32_t v = fat_le16(c->u->data + in);

		*val = (cl & 1) ? (v >> 4) : (v & 0xFFF);
		break;
	}
	case 16:
		*val = fat_le16(c->u->data + in);
		break;
	default:
		*val = fat_le32(c->u->data + in) & FAT32_MASK;
		break;
	}
	return ST_OK;
}

/* FAT[cl] = val in the cache (written at the commit).  Metadata lock
 * exclusive.  The reserved top bits of a FAT32 entry are kept. */
int fat_ent_put(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
		       uint32_t val)
{
	unsigned in, len;
	uint8_t *p;

	fat_assert_excl(fs);
	if (cl >= fs->cluster_end) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	{
		int r = fat_ent_locate(fs, c, cl, &in);

		if (r != ST_OK)
			return r;
	}
	p = c->u->data + in;
	switch (fs->fat_bits) {
	case 12: {
		uint16_t old = fat_le16(p), nv;

		if (cl & 1)
			nv = (uint16_t)((old & 0x000F) | ((val & 0xFFF) << 4));
		else
			nv = (uint16_t)((old & 0xF000) | (val & 0xFFF));
		if (nv == old)
			return ST_OK;
		fat_put16(p, nv);
		len = 2;
		break;
	}
	case 16:
		if (fat_le16(p) == (uint16_t)val)
			return ST_OK;
		fat_put16(p, (uint16_t)val);
		len = 2;
		break;
	default: {
		uint32_t old = fat_le32(p);
		uint32_t nv = (old & ~FAT32_MASK) | (val & FAT32_MASK);

		if (nv == old)
			return ST_OK;
		fat_put32(p, nv);
		len = 4;
		break;
	}
	}
	fat_mc_dirty(fs, c->u, in, len);
	return ST_OK;
}

/* Exchange the cached FAT entry of every held-back link with the value its
 * record keeps: the first call puts the old values (what the device holds)
 * into the cache and keeps the links in the records, the second one puts
 * the links back.  A cluster has one record at most (fat_link_note), so the
 * order does not matter.  Stops at the first entry it cannot reach (none
 * can fail: the units are dirty, hence cached); the caller then swaps back
 * what it swapped by calling again with the count it got.  Metadata lock
 * exclusive. */
int fat_links_swap(fat_fs_t *fs, unsigned *done)
{
	struct fat_fatcur cur = { NULL };
	unsigned n = *done ? *done : fs->nlnk;
	int r = ST_OK;

	fat_assert_excl(fs);
	*done = 0;
	for (unsigned i = 0; i < n; i++) {
		uint32_t cl = fs->lnk[i].cluster, now;

		r = fat_ent_get(fs, &cur, cl, &now);
		if (r == ST_OK)
			r = fat_ent_put(fs, &cur, cl, fs->lnk[i].value);
		if (r != ST_OK)
			break;
		fs->lnk[i].value = now;
		*done = i + 1;
	}
	fat_fatcur_done(fs, &cur);
	return r;
}

/* Room for one more held-back link (before anything changes): commits
 * first when the window holds the most links it may.  Metadata lock
 * exclusive. */
int fat_link_reserve(fat_fs_t *fs)
{
	fat_assert_excl(fs);
	if (fs->nlnk >= FAT_LINKS_MAX && !fs->in_commit) {
		int r = fat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	if (fs->nlnk >= fs->lnk_cap) {
		unsigned nc = fs->lnk_cap ? fs->lnk_cap * 2 : 32;
		struct fat_fatw *n;

		n = (struct fat_fatw *)krealloc(fs->lnk, nc * sizeof(*n));
		if (!n)
			return ST_NOMEM;
		fs->lnk = n;
		fs->lnk_cap = nc;
	}
	return ST_OK;
}

/* Hold back the link just put into FAT[cl] (its entry was `old'): stored
 * only once the clusters it leads to are.  A cluster linked again in the
 * same window keeps its first record (the old value the device has).
 * Capacity reserved by fat_link_reserve().  1 when a record was added.
 * Metadata lock exclusive. */
int fat_link_note(fat_fs_t *fs, uint32_t cl, uint32_t old)
{
	fat_assert_excl(fs);
	for (unsigned i = 0; i < fs->nlnk; i++)
		if (fs->lnk[i].cluster == cl)
			return 0;
	if (WARN_ON_ONCE(fs->nlnk >= fs->lnk_cap))
		return 0;
	fs->lnk[fs->nlnk].cluster = cl;
	fs->lnk[fs->nlnk].value = old;
	fs->nlnk++;
	return 1;
}

/* Forget the record of `cl' (its link was undone: the old value is back in
 * the cache).  Metadata lock exclusive. */
void fat_link_forget(fat_fs_t *fs, uint32_t cl)
{
	fat_assert_excl(fs);
	for (unsigned i = 0; i < fs->nlnk; i++)
		if (fs->lnk[i].cluster == cl) {
			fs->lnk[i] = fs->lnk[fs->nlnk - 1];
			fs->nlnk--;
			return;
		}
}

/* Write every dirty unit: allocations and links.  Zeroed directory
 * clusters (and the data of directories made by mkdir), whose only reference
 * is a FAT link, must be on the device before that link: a barrier first
 * whenever anything was written since the last one.  A chain that grew
 * links its old last cluster -- which a stored entry may reach -- to the new
 * clusters: those links go last, after the new clusters' own entries (the
 * runs and their end marks) and a barrier, so that a stop in between leaves
 * lost clusters, never a stored chain running into free ones.  The commit's
 * free phase applies its own order (section 17).  Metadata lock exclusive. */
static int fat_mc_flush_locked(fat_fs_t *fs);

int fat_mc_flush(fat_fs_t *fs)
{
	int r;

	fat_assert_excl(fs);
	if (fs->mc_dirty == 0 && fs->nlnk == 0)
		return ST_OK;
	if (fs->mc_flushing)
		return ST_BUSY; /* never nested */
	fs->mc_flushing = 1;
	r = fat_mc_flush_locked(fs);
	fs->mc_flushing = 0;
	return r;
}

static int fat_mc_flush_locked(fat_fs_t *fs)
{
	int r, r2;

	if (fs->barrier_needed) {
		r = fat_dev_sync(fs);
		if (r != ST_OK)
			return r;
	}
	if (fs->nlnk) {
		unsigned done = 0;

		/* 1. Everything but the links (their entries keep the values
		 *    they had, which the device holds already). */
		r = fat_links_swap(fs, &done);
		if (r == ST_OK)
			r = fat_mc_flush_region(fs);
		if (done) {
			/* back: exactly the records swapped above */
			r2 = fat_links_swap(fs, &done);
			if (r == ST_OK)
				r = r2;
			if (r2 != ST_OK) {
				/* Cannot happen (the units are cached); the
				 * cache must not keep old values for links. */
				WARN_ON_ONCE(1);
				fat_fs_error(fs, "FAT cache inconsistent", 0);
			}
		}
		if (r == ST_OK && fs->barrier_needed)
			r = fat_dev_sync(fs);
		if (r != ST_OK)
			return r;
	}
	/* 2. The links (and whatever else is still dirty). */
	r = fat_mc_flush_region(fs);
	if (r == ST_OK)
		fs->nlnk = 0;
	return r;
}

/* FAT[cl] of a data cluster.  Metadata lock held. */
int fat_fat_read(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			uint32_t *val)
{
	if (!fat_cluster_ok(fs, cl))
		return ST_INVALID;
	return fat_ent_get(fs, c, cl, val);
}

/* Remove a deferred FAT write of `cl' (it is being written now). */
static void fat_fat_cancel_deferred(fat_fs_t *fs, uint32_t cl)
{
	for (unsigned i = 0; i < fs->ndfat; i++)
		if (fs->dfat[i].cluster == cl) {
			fs->dfat[i] = fs->dfat[fs->ndfat - 1];
			fs->ndfat--;
			return;
		}
}

/* FAT[cl] = val for a data cluster, in the cache.  Metadata lock exclusive;
 * the mount may write. */
int fat_fat_write(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			 uint32_t val)
{
	fat_assert_excl(fs);
	if (!fat_cluster_ok(fs, cl)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	fat_fat_cancel_deferred(fs, cl);
	return fat_ent_put(fs, c, cl, val);
}

/* Ask for FAT[cl] = val once the dereferencing directory entries are on the
 * device (a truncation's new end of chain).  Metadata lock exclusive. */
int fat_fat_defer(fat_fs_t *fs, uint32_t cl, uint32_t val)
{
	fat_assert_excl(fs);
	fat_fat_cancel_deferred(fs, cl);
	if (fs->ndfat >= FAT_DEFERRED_FAT_MAX) {
		int r = fs->in_commit ? ST_BUSY : fat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	if (fs->ndfat >= fs->dfat_cap) {
		unsigned nc = fs->dfat_cap ? fs->dfat_cap * 2 : 32;
		struct fat_fatw *n;

		if (nc > FAT_DEFERRED_FAT_MAX)
			nc = FAT_DEFERRED_FAT_MAX;
		n = (struct fat_fatw *)krealloc(fs->dfat, nc * sizeof(*n));
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

/* The value a deferred FAT write holds for `cl'; 0 when none. */
int fat_fat_deferred(fat_fs_t *fs, uint32_t cl, uint32_t *val)
{
	for (unsigned i = 0; i < fs->ndfat; i++)
		if (fs->dfat[i].cluster == cl) {
			*val = fs->dfat[i].value;
			return 1;
		}
	return 0;
}

/* ===========================================================================
 * 9. The allocator, deferred frees
 *
 * The FAT is its own allocation map: an entry of 0 is a free cluster.  The
 * free count is counted once per mount (statfs or the first allocation; the
 * FSInfo value is trusted only with usefree) and kept up to date.  Freed
 * clusters keep their FAT values until the commit has the directory entries
 * that dropped them on the device (dereference before free), so the
 * allocator cannot hand them out earlier.
 * =========================================================================== */

/* Count the free clusters.  Once per mount; the count is kept up to date
 * afterwards.  Metadata lock held (either mode: concurrent shared counters
 * compute the same number). */
int fat_count_free(fat_fs_t *fs)
{
	struct fat_fatcur cur = { NULL };
	uint64_t nfree = 0;
	int r = ST_OK;

	fat_assert_meta(fs);
	if (__atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE) >= 0)
		return ST_OK;
	for (uint32_t cl = FAT_FIRST_CLUSTER; cl < fs->cluster_end; cl++) {
		uint32_t v;

		/* Whole units at a time on FAT16 / FAT32. */
		if (fs->fat_bits != 12 && cur.u &&
		    fat_ent_off(fs, cl) / fs->mcu_bytes == cur.u->unit) {
			unsigned in = (unsigned)(fat_ent_off(fs, cl) %
						 fs->mcu_bytes);

			if (fs->fat_bits == 16)
				v = fat_le16(cur.u->data + in);
			else
				v = fat_le32(cur.u->data + in) & FAT32_MASK;
		} else {
			r = fat_ent_get(fs, &cur, cl, &v);
			if (r != ST_OK)
				break;
		}
		if (v == FAT_ENT_FREE)
			nfree++;
	}
	fat_fatcur_done(fs, &cur);
	if (r != ST_OK)
		return r;
	__atomic_store_n(&fs->free_clusters, (int64_t)nfree, __ATOMIC_RELEASE);
	if (fs->fsinfo_valid && fs->fsinfo_free != (uint32_t)nfree)
		fs->fsinfo_dirty = 1;
	return ST_OK;
}

/* Adjust the free-cluster count by `delta' -- unless it is not known yet
 * (-1), then it stays unknown and the next count reads the FAT, which
 * already holds the change. */
void fat_free_count_add(fat_fs_t *fs, int64_t delta)
{
	int64_t v = __atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE);

	fs->fsinfo_dirty = 1;
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
static int fat_find_run(fat_fs_t *fs, uint32_t from, uint32_t maxlen,
			uint32_t *start, uint32_t *len)
{
	struct fat_fatcur cur = { NULL };
	uint32_t run_start = 0, run_len = 0;
	int r = ST_OK;

	*start = 0;
	*len = 0;
	if (from < FAT_FIRST_CLUSTER)
		from = FAT_FIRST_CLUSTER;
	for (uint32_t cl = from; cl < fs->cluster_end; cl++) {
		uint32_t v;

		r = fat_ent_get(fs, &cur, cl, &v);
		if (r != ST_OK)
			break;
		if (v == FAT_ENT_FREE) {
			if (run_len == 0)
				run_start = cl;
			if (++run_len >= maxlen)
				break;
		} else if (run_len) {
			break;
		}
	}
	fat_fatcur_done(fs, &cur);
	if (r != ST_OK)
		return r;
	if (run_len) {
		*start = run_start;
		*len = run_len;
	}
	return ST_OK;
}

/* Free clusters right after `last' (exclusive), up to `maxlen'. */
static int fat_free_after(fat_fs_t *fs, uint32_t last, uint32_t maxlen,
			  uint32_t *len)
{
	uint32_t s, l;
	int r;

	*len = 0;
	if (!last || (uint64_t)last + 1 >= fs->cluster_end)
		return ST_OK;
	r = fat_find_run(fs, last + 1, maxlen, &s, &l);
	if (r != ST_OK)
		return r;
	if (s == last + 1)
		*len = l;
	return ST_OK;
}

/* Mark clusters [start, start + len) allocated (an end mark each: the
 * stream's append links them).  Every entry must be free: an allocated one
 * here is a driver bug.  Metadata lock exclusive. */
static int fat_mark_run(fat_fs_t *fs, uint32_t start, uint32_t len)
{
	struct fat_fatcur cur = { NULL };
	int r = ST_OK;

	for (uint32_t i = 0; i < len && r == ST_OK; i++) {
		uint32_t v;

		r = fat_ent_get(fs, &cur, start + i, &v);
		if (r != ST_OK)
			break;
		if (v != FAT_ENT_FREE) {
			WARN_ON_ONCE(1);
			r = ST_IO;
			break;
		}
		r = fat_ent_put(fs, &cur, start + i, fs->eoc_val);
	}
	fat_fatcur_done(fs, &cur);
	return r;
}

/* Give clusters back that nothing references yet (never stored as part of
 * a chain an entry names): their entries become free at once. */
void fat_unmark_run(fat_fs_t *fs, uint32_t start, uint32_t len)
{
	struct fat_fatcur cur = { NULL };

	for (uint32_t i = 0; i < len; i++)
		if (fat_ent_put(fs, &cur, start + i, FAT_ENT_FREE) != ST_OK)
			break;
	fat_fatcur_done(fs, &cur);
}

/* Allocate `n' clusters, preferring contiguity: first the clusters right
 * after `last' (the stream's current end), then ONE free run big enough for
 * the rest (next fit from the allocation cursor, wrapping once), and only
 * when there is none, the free runs in order.  The runs are appended to
 * *runs (kalloc'd array, caller frees); every cluster is marked used (an
 * end mark) before this returns.  ST_NOSPC when the volume has fewer than
 * `n' free clusters (nothing is allocated then).  Metadata lock exclusive;
 * the mount may write. */
int fat_alloc(fat_fs_t *fs, uint32_t last, uint32_t n,
		     struct fat_run **runs, unsigned *nruns)
{
	unsigned cap = 0;
	uint32_t got = 0;
	int r;

	fat_assert_excl(fs);
	*runs = NULL;
	*nruns = 0;
	if (n == 0)
		return ST_OK;
	r = fat_count_free(fs);
	if (r != ST_OK)
		return r;
	if ((uint64_t)fs->free_clusters < n)
		return ST_NOSPC;

	fat_alloc_lock(fs);

#define FAT_ADD_RUN(s_, l_)                                                 \
	do {                                                                \
		if (*nruns >= cap) {                                        \
			unsigned nc_ = cap ? cap * 2 : 8;                   \
			struct fat_run *nr_ = (struct fat_run *)krealloc(   \
				*runs, nc_ * sizeof(**runs));               \
			if (!nr_) {                                         \
				r = ST_NOMEM;                               \
				goto fail;                                  \
			}                                                   \
			*runs = nr_;                                        \
			cap = nc_;                                          \
		}                                                           \
		(*runs)[*nruns].start = (s_);                               \
		(*runs)[*nruns].len = (l_);                                 \
		(*nruns)++;                                                 \
		r = fat_mark_run(fs, (s_), (l_));                           \
		if (r != ST_OK) {                                           \
			fat_unmark_run(fs, (s_), (l_));                     \
			(*nruns)--;                                         \
			goto fail;                                          \
		}                                                           \
		got += (l_);                                                \
	} while (0)

	/* 1. Continue the stream in place. */
	if (last) {
		uint32_t l;

		r = fat_free_after(fs, last, n, &l);
		if (r != ST_OK)
			goto fail;
		if (l)
			FAT_ADD_RUN(last + 1, l);
	}
	/* 2. One run for the rest: next fit from the cursor, then from the
	 *    start of the data area. */
	if (got < n) {
		uint32_t need = n - got;

		for (int pass = 0; pass < 2 && got < n; pass++) {
			uint32_t from = pass == 0 ? fs->alloc_cursor :
						    FAT_FIRST_CLUSTER;
			uint64_t stop = pass == 0 ? (uint64_t)fs->cluster_end :
						    (uint64_t)fs->alloc_cursor;

			while ((uint64_t)from < stop) {
				uint32_t s, l;

				r = fat_find_run(fs, from, need, &s, &l);
				if (r != ST_OK)
					goto fail;
				if (!s || (pass == 1 && (uint64_t)s >= stop))
					break;
				if (l >= need) {
					FAT_ADD_RUN(s, need);
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

			r = fat_find_run(fs, from, n - got, &s, &l);
			if (r != ST_OK)
				goto fail;
			if (!s) {
				if (wrapped)
					break;
				wrapped = 1;
				from = FAT_FIRST_CLUSTER;
				continue;
			}
			FAT_ADD_RUN(s, l);
			from = s + l;
		}
	}
#undef FAT_ADD_RUN
	if (got < n) {
		/* The free count said there was room: the FAT disagrees with
		 * it.  Undo and report. */
		r = fat_corrupt(fs, n, "free count larger than the FAT's");
		goto fail;
	}
	fat_free_count_add(fs, -(int64_t)n);
	{
		struct fat_run *lr = &(*runs)[*nruns - 1];
		uint64_t nxt = (uint64_t)lr->start + lr->len;

		fs->alloc_cursor = nxt >= fs->cluster_end ? FAT_FIRST_CLUSTER :
							    (uint32_t)nxt;
	}
	fat_alloc_unlock(fs);
	return ST_OK;
fail:
	/* Give back what was taken (never referenced by anything yet). */
	for (unsigned i = 0; i < *nruns; i++)
		fat_unmark_run(fs, (*runs)[i].start, (*runs)[i].len);
	kfree(*runs);
	*runs = NULL;
	*nruns = 0;
	fat_alloc_unlock(fs);
	return r;
}

/* Free clusters [start, start + len) -- LATER: their FAT entries keep their
 * values until the commit has the directory entries that no longer
 * reference them on the device (dereference before free).  Callers change
 * the in-memory state first, so the early commit this may run sees a
 * consistent volume.  Metadata lock exclusive. */
static int fat_free_defer(fat_fs_t *fs, uint32_t start, uint32_t len)
{
	uint64_t end;

	fat_assert_excl(fs);
	if (len == 0)
		return ST_OK;
	if (!fat_add_ok(start, (uint64_t)len - 1, &end) ||
	    !fat_cluster_ok(fs, start) || !fat_cluster_ok(fs, end))
		return fat_corrupt(fs, start, "freeing clusters %u+%u out of range",
				   start, len);
	if (fs->npf > 0) {
		struct fat_run *l = &fs->pf[fs->npf - 1];

		if ((uint64_t)l->start + l->len == start) {
			l->len += len;
			fs->pf_clusters += len;
			return ST_OK;
		}
	}
	if (fs->npf >= FAT_PENDING_FREE_MAX) {
		int r = fs->in_commit ? ST_BUSY : fat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	if (fs->npf >= fs->pf_cap) {
		unsigned nc = fs->pf_cap ? fs->pf_cap * 2 : 64;
		struct fat_run *n;

		if (nc > FAT_PENDING_FREE_MAX)
			nc = FAT_PENDING_FREE_MAX;
		n = (struct fat_run *)krealloc(fs->pf, nc * sizeof(*n));
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
int fat_free_extents(fat_fs_t *fs, const fat_extent_t *ext, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		int r = fat_free_defer(fs, ext[i].disk, ext[i].len);

		if (r != ST_OK)
			return r;
	}
	return ST_OK;
}

/* Apply the deferred frees: their FAT entries become free.  Every entry
 * must still be in use (a free one means the volume lied about its
 * allocation).  Part of the commit; metadata lock exclusive. */
int fat_apply_frees(fat_fs_t *fs)
{
	struct fat_fatcur cur = { NULL };
	int r = ST_OK;

	for (unsigned i = 0; i < fs->npf && r == ST_OK; i++) {
		for (uint32_t k = 0; k < fs->pf[i].len && r == ST_OK; k++) {
			uint32_t cl = fs->pf[i].start + k, v;

			r = fat_ent_get(fs, &cur, cl, &v);
			if (r != ST_OK)
				break;
			if (v == FAT_ENT_FREE) {
				r = fat_corrupt(fs, cl,
						"freeing cluster %u, which is free",
						cl);
				break;
			}
			r = fat_ent_put(fs, &cur, cl, FAT_ENT_FREE);
		}
	}
	fat_fatcur_done(fs, &cur);
	return r;
}
