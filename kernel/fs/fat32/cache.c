/* LikeOS - FAT filesystem driver: the metadata cache of the FAT region and
 * cluster chain decoding into extents
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ===========================================================================
 * 7. Metadata cache: the FAT region
 *
 * The FAT does not go through the page cache: its last page could reach
 * past it into whatever follows, and the order in which FAT changes reach
 * the device is the heart of crash consistency (section 17), which
 * background writeback would take out of our hands.  So it lives here, in
 * units of 4 KB (FAT16, FAT32) or the whole FAT (FAT12, whose 12-bit entries
 * straddle bytes: one unit keeps every entry inside a unit), with a dirty
 * bit per device sector.  Units are read from the FAT in use and written to
 * every FAT copy (or only the active one when FAT32 mirroring is off).
 * Only the commit (or a mutation running out of room) writes dirty units,
 * through the chokepoint.
 *
 * Concurrency: units are found and inserted under fs->mc_lock; a unit in use
 * holds a reference and cannot be dropped.  Unit contents change only under
 * the metadata lock exclusive, so readers (shared) never see a change in
 * progress.  I/O never happens under the spinlock.
 * =========================================================================== */

static inline unsigned fat_mc_hash(uint32_t unit)
{
	return (unsigned)((unit * 0x85EBCA6Bu) >> 8) % FAT_MC_HASH;
}

/* Bytes of one FAT the cache covers: the entries of real clusters, rounded
 * to whole device sectors, never beyond the FAT. */
static uint64_t fat_mc_region_bytes(fat_fs_t *fs)
{
	return fat_min64(fat_round_up(fs->fat_used_bytes, fs->dev_ss),
			 fs->fat_bytes);
}

/* Where unit `unit' lives: its byte offset inside one FAT and its length in
 * device sectors.  0 when the unit lies beyond the region. */
static int fat_mc_locate(fat_fs_t *fs, uint32_t unit, uint64_t *off,
			 unsigned *nsect)
{
	uint64_t o = (uint64_t)unit * fs->mcu_bytes;
	uint64_t total = fat_mc_region_bytes(fs);
	uint64_t len;

	if (o >= total)
		return 0;
	len = fat_min64(fs->mcu_bytes, total - o);
	*nsect = (unsigned)(len >> fs->dev_shift);
	if (*nsect == 0 || *nsect > FAT_MC_MAX_SECTORS)
		return 0;
	*off = o;
	return 1;
}

static fat_mcu_t *fat_mc_find_locked(fat_fs_t *fs, uint32_t unit)
{
	for (fat_mcu_t *u = fs->mc_hash[fat_mc_hash(unit)]; u; u = u->hnext)
		if (u->unit == unit)
			return u;
	return NULL;
}

static void fat_mc_lru_unlink(fat_fs_t *fs, fat_mcu_t *u)
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

static void fat_mc_lru_push(fat_fs_t *fs, fat_mcu_t *u)
{
	u->lru_prev = NULL;
	u->lru_next = fs->mc_lru_head;
	if (fs->mc_lru_head)
		fs->mc_lru_head->lru_prev = u;
	fs->mc_lru_head = u;
	if (!fs->mc_lru_tail)
		fs->mc_lru_tail = u;
}

static void fat_mc_hash_unlink(fat_fs_t *fs, fat_mcu_t *u)
{
	fat_mcu_t **pp = &fs->mc_hash[fat_mc_hash(u->unit)];

	while (*pp && *pp != u)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = u->hnext;
	u->hnext = NULL;
}

/* Drop clean, unused units beyond the cap (oldest first).  Any context that
 * may sleep (kfree), no spinlock held. */
static void fat_mc_trim(fat_fs_t *fs)
{
	fat_mcu_t *victims = NULL;
	uint64_t flags;

	spin_lock_irqsave(&fs->mc_lock, &flags);
	for (fat_mcu_t *u = fs->mc_lru_tail; u && fs->mc_count > FAT_MC_MAX;) {
		fat_mcu_t *prev = u->lru_prev;

		if (u->refs == 0 && !u->dirty) {
			fat_mc_lru_unlink(fs, u);
			fat_mc_hash_unlink(fs, u);
			fs->mc_count--;
			u->hnext = victims;
			victims = u;
		}
		u = prev;
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);
	while (victims) {
		fat_mcu_t *n = victims->hnext;

		kfree(victims->data);
		kfree(victims);
		victims = n;
	}
}

/* Get unit `unit', held.  Reads it (and up to FAT_MC_READAHEAD following
 * uncached units in the same transfer) on a miss.  NULL with *err set on
 * failure.  Metadata lock held (either mode). */
fat_mcu_t *fat_mc_get(fat_fs_t *fs, uint32_t unit, int *err)
{
	fat_mcu_t *u, *batch[FAT_MC_READAHEAD];
	uint64_t off, flags;
	unsigned nsect, n = 0, total_sect = 0;
	uint8_t *buf = NULL;
	int r;

	might_sleep();
	fat_assert_meta(fs);
	*err = ST_OK;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	u = fat_mc_find_locked(fs, unit);
	if (u) {
		u->refs++;
		fat_mc_lru_unlink(fs, u);
		fat_mc_lru_push(fs, u);
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		return u;
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);

	if (!fat_mc_locate(fs, unit, &off, &nsect)) {
		*err = fat_corrupt(fs, unit, "FAT unit %u beyond the FAT", unit);
		return NULL;
	}

	/* A mutation holding many dirty units writes them early rather than
	 * letting the cache grow without bound.  Early is always allowed:
	 * the units hold allocations only, frees are applied at the commit
	 * (section 17). */
	if (fs->mc_dirty > FAT_MC_MAX && fat_rwsem_held_excl(&fs->meta) &&
	    fs->armed && !fs->mc_flushing)
		fat_mc_flush(fs);

	/* Build the batch: this unit plus following uncached ones (chains
	 * are walked forward, free space is counted forward). */
	for (uint32_t k = 0; k < FAT_MC_READAHEAD; k++) {
		uint64_t o2;
		unsigned s2;

		if (k > 0) {
			if (!fat_mc_locate(fs, unit + k, &o2, &s2))
				break;
			spin_lock_irqsave(&fs->mc_lock, &flags);
			int cached = fat_mc_find_locked(fs, unit + k) != NULL;
			spin_unlock_irqrestore(&fs->mc_lock, flags);
			if (cached)
				break;
		} else {
			o2 = off;
			s2 = nsect;
		}
		u = (fat_mcu_t *)kcalloc(1, sizeof(*u));
		if (!u)
			break;
		u->data = (uint8_t *)kalloc(fs->mcu_bytes);
		if (!u->data) {
			kfree(u);
			break;
		}
		u->unit = unit + k;
		u->nsect = (uint16_t)s2;
		u->nbytes = s2 << fs->dev_shift;
		u->off = o2;
		batch[n++] = u;
		total_sect += s2;
	}
	if (n == 0) {
		*err = ST_NOMEM;
		return NULL;
	}
	if (n == 1) {
		r = fat_dev_read(fs, fat_mc_lba(fs, fs->active_fat, batch[0]->off),
				 batch[0]->nsect, batch[0]->data);
	} else {
		buf = (uint8_t *)kalloc((size_t)total_sect << fs->dev_shift);
		if (!buf) {
			/* Read just the one asked for. */
			for (unsigned k = 1; k < n; k++) {
				kfree(batch[k]->data);
				kfree(batch[k]);
			}
			n = 1;
			r = fat_dev_read(fs,
					 fat_mc_lba(fs, fs->active_fat,
						    batch[0]->off),
					 batch[0]->nsect, batch[0]->data);
		} else {
			r = fat_dev_read(fs,
					 fat_mc_lba(fs, fs->active_fat,
						    batch[0]->off),
					 total_sect, buf);
			if (r == ST_OK) {
				uint64_t o = 0;

				for (unsigned k = 0; k < n; k++) {
					mm_memcpy(batch[k]->data, buf + o,
						  batch[k]->nbytes);
					o += batch[k]->nbytes;
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
		WARN_RATELIMIT(1, "fat: %s: FAT read at byte %llu failed (%d)",
			       fat_name_of(fs), (unsigned long long)off, r);
		*err = ST_IO;
		return NULL;
	}

	/* Insert; another reader may have inserted any of them meanwhile --
	 * then its copy wins and ours is dropped (they are equal: both came
	 * off the device while nobody could change it). */
	fat_mcu_t *result = NULL, *drop = NULL;

	spin_lock_irqsave(&fs->mc_lock, &flags);
	for (unsigned k = 0; k < n; k++) {
		fat_mcu_t *have = fat_mc_find_locked(fs, batch[k]->unit);

		if (have) {
			batch[k]->hnext = drop;
			drop = batch[k];
			if (k == 0) {
				have->refs++;
				result = have;
			}
			continue;
		}
		unsigned hb = fat_mc_hash(batch[k]->unit);

		batch[k]->hnext = fs->mc_hash[hb];
		fs->mc_hash[hb] = batch[k];
		fat_mc_lru_push(fs, batch[k]);
		fs->mc_count++;
		if (k == 0) {
			batch[k]->refs = 1;
			result = batch[k];
		}
	}
	spin_unlock_irqrestore(&fs->mc_lock, flags);
	while (drop) {
		fat_mcu_t *nx = drop->hnext;

		kfree(drop->data);
		kfree(drop);
		drop = nx;
	}
	if (fs->mc_count > FAT_MC_MAX)
		fat_mc_trim(fs);
	return result;
}

void fat_mc_put(fat_fs_t *fs, fat_mcu_t *u)
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
void fat_mc_dirty(fat_fs_t *fs, fat_mcu_t *u, unsigned off, unsigned len)
{
	uint64_t flags;
	unsigned first, last;

	fat_assert_excl(fs);
	if (len == 0)
		return;
	BUG_ON(off + len > u->nbytes);
	first = off >> fs->dev_shift;
	last = (off + len - 1) >> fs->dev_shift;
	spin_lock_irqsave(&fs->mc_lock, &flags);
	if (!u->dirty)
		fs->mc_dirty++;
	for (unsigned s = first; s <= last; s++)
		u->dirty |= 1u << s;
	spin_unlock_irqrestore(&fs->mc_lock, flags);
}

/* Write the dirty units (runs of dirty sectors, lowest address first) to
 * every FAT copy written, through the chokepoint.  Metadata lock
 * exclusive. */
int fat_mc_flush_region(fat_fs_t *fs)
{
	fat_assert_excl(fs);
	might_sleep();
	for (;;) {
		fat_mcu_t *best = NULL;
		uint64_t flags;
		uint32_t mask;
		int r = ST_OK;

		/* The dirty unit with the lowest address (few dirty units at
		 * a time: a scan). */
		spin_lock_irqsave(&fs->mc_lock, &flags);
		for (fat_mcu_t *u = fs->mc_lru_head; u; u = u->lru_next)
			if (u->dirty && (!best || u->off < best->off))
				best = u;
		if (best)
			best->refs++;
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		if (!best)
			return ST_OK;
		mask = best->dirty;
		for (unsigned copy = 0; copy < fs->nfats && r == ST_OK; copy++) {
			if (!fs->mirror && copy != fs->active_fat)
				continue;
			for (unsigned sct = 0; sct < best->nsect;) {
				unsigned e;

				if (!(mask & (1u << sct))) {
					sct++;
					continue;
				}
				e = sct;
				while (e < best->nsect && (mask & (1u << e)))
					e++;
				r = fat_dev_write(
					fs,
					fat_mc_lba(fs, copy, best->off) + sct,
					e - sct,
					best->data + ((unsigned long)sct
						      << fs->dev_shift),
					FAT_W_FAT);
				if (r != ST_OK)
					break;
				sct = e;
			}
		}
		spin_lock_irqsave(&fs->mc_lock, &flags);
		if (r == ST_OK) {
			/* Bits set meanwhile cannot happen (the metadata lock
			 * is ours); clear exactly what was written. */
			best->dirty &= ~mask;
			if (!best->dirty)
				fs->mc_dirty--;
		}
		best->refs--;
		spin_unlock_irqrestore(&fs->mc_lock, flags);
		if (r != ST_OK)
			return r; /* the unit stays dirty */
	}
}

/* Forget every unit (unmount, failed mount).  Dirty ones are dropped: the
 * caller has flushed what can be flushed. */
void fat_mc_destroy(fat_fs_t *fs)
{
	for (unsigned b = 0; b < FAT_MC_HASH; b++) {
		fat_mcu_t *u = fs->mc_hash[b];

		while (u) {
			fat_mcu_t *n = u->hnext;

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

/* Is `v' an end-of-chain mark / a link to a data cluster? */
static inline int fat_val_eoc(fat_fs_t *fs, uint32_t v)
{
	return v >= fs->eoc_min;
}

static inline int fat_val_link(fat_fs_t *fs, uint32_t v)
{
	return v >= FAT_FIRST_CLUSTER && v < fs->cluster_end;
}

/* Append an extent to an array (merging with the last one when it simply
 * continues it).  ST_NOMEM past FAT_MAX_EXTENTS. */
int fat_ext_append(fat_extent_t **ext, uint32_t *next, uint32_t *cap,
			  uint32_t fidx, uint32_t disk, uint32_t len)
{
	if (len == 0)
		return ST_OK;
	if (*next > 0) {
		fat_extent_t *l = &(*ext)[*next - 1];

		if (l->fidx + l->len == fidx && l->disk + l->len == disk) {
			l->len += len;
			return ST_OK;
		}
	}
	if (*next >= *cap) {
		uint32_t nc = *cap ? *cap * 2 : 4;
		fat_extent_t *n;

		if (nc > FAT_MAX_EXTENTS)
			nc = FAT_MAX_EXTENTS;
		if (*next >= nc)
			return ST_NOMEM;
		n = (fat_extent_t *)krealloc(*ext, nc * sizeof(*n));
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

/* Does a decoded chain use some cluster twice (a loop inside the length the
 * file claims)?  The extents' disk ranges, sorted (heapsort on a copy), must
 * not overlap.  1 overlap, 0 none, < 0 ST_NOMEM. */
static int fat_ext_overlap(const fat_extent_t *ext, uint32_t n)
{
	uint32_t *s, *l;
	int bad = 0;

	if (n < 2)
		return 0;
	if (n == 2) {
		const fat_extent_t *a = &ext[0], *b = &ext[1];

		return !((uint64_t)a->disk + a->len <= b->disk ||
			 (uint64_t)b->disk + b->len <= a->disk);
	}
	s = (uint32_t *)kalloc((size_t)n * sizeof(uint32_t));
	l = (uint32_t *)kalloc((size_t)n * sizeof(uint32_t));
	if (!s || !l) {
		if (s)
			kfree(s);
		if (l)
			kfree(l);
		return ST_NOMEM;
	}
	for (uint32_t i = 0; i < n; i++) {
		s[i] = ext[i].disk;
		l[i] = ext[i].len;
	}
	/* Heapsort by start (no recursion, O(n log n)): the first half of the
	 * loop builds the heap, the second takes the largest out each round. */
	{
		uint32_t start = n / 2, end = n;

		for (;;) {
			uint32_t root, child;

			if (start > 0) {
				start--;
			} else {
				end--;
				if (end == 0)
					break;
				uint32_t ts = s[end], tl = l[end];

				s[end] = s[0];
				l[end] = l[0];
				s[0] = ts;
				l[0] = tl;
			}
			root = start;
			while ((child = 2 * root + 1) < end) {
				if (child + 1 < end && s[child + 1] > s[child])
					child++;
				if (s[root] >= s[child])
					break;
				uint32_t ts = s[root], tl = l[root];

				s[root] = s[child];
				l[root] = l[child];
				s[child] = ts;
				l[child] = tl;
				root = child;
			}
		}
	}
	for (uint32_t i = 1; i < n && !bad; i++)
		if ((uint64_t)s[i - 1] + l[i - 1] > s[i])
			bad = 1;
	kfree(s);
	kfree(l);
	return bad;
}

/* Decode a chain into extents.
 *
 *   want     clusters the object must have (a file: from its size), or 0 to
 *            follow the chain to its end (a directory), then bounded by
 *            `max'
 *   quiet    report nothing (the caller decides: an unlink of a corrupt
 *            file reports after the name is gone)
 *
 * Every cluster number is range-checked; a chain that ends early, holds a
 * free, reserved or bad mark, leaves the data area or uses a cluster twice
 * is corrupt (ST_IO).  The last cluster of a file must carry an end mark or
 * a link (a chain longer than its file is tolerated; a free last cluster is
 * not: the allocator would hand it out).  The walk is bounded by `want' (or
 * `max'), so a cycle in the FAT cannot hang it.  Metadata lock held. */
int fat_chain_decode(fat_fs_t *fs, uint32_t first, uint64_t want,
			    uint64_t max, int quiet, fat_extent_t **ext,
			    uint32_t *next, uint32_t *cap, uint32_t *ncl)
{
	struct fat_fatcur cur = { NULL };
	uint64_t count = 0, limit;
	uint32_t cl = first;
	int r = ST_OK;

#define FAT_CHAIN_BAD(...)                                          \
	do {                                                        \
		r = quiet ? ST_IO : fat_corrupt(fs, first, __VA_ARGS__); \
		goto out;                                           \
	} while (0)

	*next = 0;
	*ncl = 0;
	if (first == 0 && want == 0)
		return ST_OK;
	if (!fat_cluster_ok(fs, first)) {
		if (quiet)
			return ST_IO;
		return fat_corrupt(fs, first, "first cluster %u out of range",
				   first);
	}
	limit = want ? want : max;
	if (limit > fs->cluster_count)
		limit = fs->cluster_count;
	if (want > fs->cluster_count) {
		if (quiet)
			return ST_IO;
		return fat_corrupt(fs, first,
				   "chain of %llu clusters on a volume of %u",
				   (unsigned long long)want, fs->cluster_count);
	}
	for (;;) {
		uint32_t val = 0;

		r = fat_ext_append(ext, next, cap, (uint32_t)count, cl, 1);
		if (r != ST_OK)
			goto out;
		count++;
		r = fat_fat_read(fs, &cur, cl, &val);
		if (r != ST_OK)
			goto out;
		if (want && count == want) {
			/* The file's last cluster: an end mark, or a link
			 * (the chain goes on past the size: tolerated). */
			if (!fat_val_eoc(fs, val) && !fat_val_link(fs, val))
				FAT_CHAIN_BAD("chain from %u: last cluster %u holds 0x%x",
					      first, cl, val);
			break;
		}
		if (fat_val_eoc(fs, val)) {
			if (want)
				FAT_CHAIN_BAD("chain from %u ends after %llu of %llu clusters",
					      first, (unsigned long long)count,
					      (unsigned long long)want);
			break;
		}
		if (!fat_val_link(fs, val))
			FAT_CHAIN_BAD("chain from %u holds FAT value 0x%x at cluster %u",
				      first, val, cl);
		if (count >= limit)
			FAT_CHAIN_BAD("chain from %u longer than %llu clusters (loop?)",
				      first, (unsigned long long)limit);
		cl = val;
	}
	{
		int ov = fat_ext_overlap(*ext, *next);

		if (ov < 0) {
			r = ov;
			goto out;
		}
		if (ov)
			FAT_CHAIN_BAD("chain from %u uses a cluster twice", first);
	}
	*ncl = (uint32_t)count;
out:
#undef FAT_CHAIN_BAD
	fat_fatcur_done(fs, &cur);
	if (r != ST_OK)
		*next = 0;
	return r;
}

/* The disk cluster holding stream cluster `ci', or 0.  Binary search. */
uint32_t fat_ext_lookup(const fat_extent_t *ext, uint32_t n, uint64_t ci)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const fat_extent_t *e = &ext[mid];

		if (ci < e->fidx)
			hi = mid;
		else if (ci >= (uint64_t)e->fidx + e->len)
			lo = mid + 1;
		else
			return e->disk + (uint32_t)(ci - e->fidx);
	}
	return 0;
}
