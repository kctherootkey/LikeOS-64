/* LikeOS - exFAT filesystem driver
 *
 * Directory reading and modification, the directory write queue, readdir.
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
 * 14. Directory reading
 *
 * A directory is a stream of 32-byte entries, read through the page cache
 * like file data (its pages are never dirty: changes are written through
 * the directory write queue, section 16).  An entry never straddles a page.
 * An entry set is a File entry followed by SecondaryCount secondaries: the
 * stream extension, the name entries, then any others (vendor entries,
 * unknown benign secondaries -- kept as they are).  A set is accepted only
 * when its structure, checksum and fields check out; anything else is
 * skipped with a rate-limited warning, never trusted.
 * =========================================================================== */

void exfat_diter_init(exfat_diter_t *it, exfat_fs_t *fs,
		      exfat_stream_t *d)
{
	it->fs = fs;
	it->d = d;
	it->pg = NULL;
	it->pidx = 0;
	it->err = ST_OK;
}

void exfat_diter_done(exfat_diter_t *it)
{
	if (it->pg)
		pagecache_put(it->pg);
	it->pg = NULL;
}

/* Entry `idx' of the directory: a pointer into a held cached page, valid
 * until the next call or exfat_diter_done().  NULL past the end (err 0) or
 * on a read error (err set).  Metadata lock held. */
const uint8_t *exfat_diter_get(exfat_diter_t *it, uint64_t idx)
{
	exfat_stream_t *d = it->d;
	uint64_t byte;
	unsigned long page;

	if (idx >= exfat_dir_slots(d))
		return NULL;
	byte = idx << EXFAT_DENTRY_SHIFT;
	page = (unsigned long)(byte >> PAGE_SHIFT);
	if (!it->pg || it->pidx != page) {
		unsigned long chain = exfat_chain_of(d);

		exfat_diter_done(it);
		it->pg = pagecache_get_held(chain, page, (unsigned long)d->size,
					    &it->fs->sb, chain);
		if (!it->pg) {
			it->err = ST_IO;
			return NULL;
		}
		it->pidx = page;
	}
	return it->pg->data + (byte & (PAGE_SIZE - 1));
}

/* Copy `count' entries from slot `idx' into `buf'. */
static int exfat_diter_copy(exfat_diter_t *it, uint64_t idx, unsigned count,
			    uint8_t *buf)
{
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *e = exfat_diter_get(it, idx + i);

		if (!e)
			return it->err ? it->err : ST_EOF;
		mm_memcpy(buf + i * EXFAT_DENTRY_SIZE, e, EXFAT_DENTRY_SIZE);
	}
	return ST_OK;
}

/* Slots to step over from an in-use entry that is not a File set: the
 * entry and, for primaries with a SecondaryCount, its secondaries. */
unsigned exfat_skip_slots(const uint8_t *e)
{
	uint8_t type = e[EXFAT_DE_TYPE];

	if (type & EXFAT_ET_SECONDARY)
		return 1; /* a stray secondary */
	if (exfat_primary_single(type))
		return 1;
	return 1u + e[EXFAT_DE_SECONDARY_COUNT];
}

/* Validate the set in `set' (the File entry and `set[1]' secondaries) and
 * describe it.  `name' receives the name (UTF-16, di->name_len units).
 *
 *   ST_OK          a set to use
 *   ST_IO          corrupt (bad structure, checksum, fields): skip it
 *   ST_UNSUPPORTED it carries a critical secondary this driver does not
 *                  know: the file cannot be used, but the set is intact
 *                  and stays untouched
 *
 * No warning here: the caller decides (a lookup reports, readdir skips). */
int exfat_set_parse(exfat_fs_t *fs, const uint8_t *set,
		    exfat_dentry_info_t *di, uint16_t *name)
{
	unsigned sc = set[EXFAT_DE_SECONDARY_COUNT];
	const uint8_t *se = set + EXFAT_DENTRY_SIZE;
	unsigned nlen, nent;
	uint16_t want;

	if (set[EXFAT_DE_TYPE] != EXFAT_ET_FILE || sc < EXFAT_SET_MIN_SECONDARY ||
	    sc > EXFAT_SET_MAX_SECONDARY)
		return ST_IO;
	want = exfat_le16(set + EXFAT_DE_SET_CHECKSUM);
	if (exfat_set_checksum(set, sc + 1) != want)
		return ST_IO;
	if (se[EXFAT_DE_TYPE] != EXFAT_ET_STREAM)
		return ST_IO;
	nlen = se[EXFAT_STREAM_NAME_LEN];
	if (nlen == 0 || nlen > EXFAT_NAME_MAX)
		return ST_IO;
	nent = (nlen + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;
	if (1 + nent > sc)
		return ST_IO;
	for (unsigned i = 0; i < nent; i++) {
		const uint8_t *ne = set + (2 + i) * EXFAT_DENTRY_SIZE;
		unsigned take = nlen - i * EXFAT_NAME_PER_ENTRY;

		if (ne[EXFAT_DE_TYPE] != EXFAT_ET_NAME)
			return ST_IO;
		if (take > EXFAT_NAME_PER_ENTRY)
			take = EXFAT_NAME_PER_ENTRY;
		for (unsigned k = 0; k < take; k++)
			name[i * EXFAT_NAME_PER_ENTRY + k] =
				exfat_le16(ne + EXFAT_NAME_CHARS_OFF + 2 * k);
	}
	mm_memset(di, 0, sizeof(*di));
	/* The secondaries after the names. */
	for (unsigned i = 2 + nent; i <= sc; i++) {
		const uint8_t *x = set + i * EXFAT_DENTRY_SIZE;
		uint8_t t = x[EXFAT_DE_TYPE];

		if ((t & (EXFAT_ET_INUSE | EXFAT_ET_SECONDARY)) !=
		    (EXFAT_ET_INUSE | EXFAT_ET_SECONDARY))
			return ST_IO; /* not a secondary in use */
		if (t == EXFAT_ET_NAME || t == EXFAT_ET_STREAM)
			return ST_IO; /* misplaced */
		if (!(t & EXFAT_ET_BENIGN))
			return ST_UNSUPPORTED; /* unknown critical */
		if ((x[EXFAT_DE_GSFLAGS] & EXFAT_SF_ALLOC_POSSIBLE) &&
		    exfat_le32(x + EXFAT_DE_FIRST_CLUSTER))
			di->has_alloc_secondaries = 1;
	}
	di->secondary_count = (uint8_t)sc;
	di->attr = exfat_le16(set + EXFAT_FILE_ATTR);
	di->ctime = exfat_le32(set + EXFAT_FILE_CTIME);
	di->mtime = exfat_le32(set + EXFAT_FILE_MTIME);
	di->atime = exfat_le32(set + EXFAT_FILE_ATIME);
	di->ctime_cs = set[EXFAT_FILE_CTIME_10MS];
	di->mtime_cs = set[EXFAT_FILE_MTIME_10MS];
	di->ctime_utc = set[EXFAT_FILE_CTIME_UTC];
	di->mtime_utc = set[EXFAT_FILE_MTIME_UTC];
	di->atime_utc = set[EXFAT_FILE_ATIME_UTC];
	di->sflags = se[EXFAT_DE_GSFLAGS];
	di->name_len = (uint8_t)nlen;
	di->name_hash = exfat_le16(se + EXFAT_STREAM_NAME_HASH);
	di->vdl = exfat_le64(se + EXFAT_STREAM_VDL);
	di->first_cluster = exfat_le32(se + EXFAT_STREAM_FIRST_CLUSTER);
	di->size = exfat_le64(se + EXFAT_STREAM_DATA_LENGTH);
	if (!(di->sflags & EXFAT_SF_ALLOC_POSSIBLE)) {
		/* No allocation possible: nothing to read. */
		if (di->first_cluster || di->size)
			return ST_IO;
	}
	if (di->vdl > di->size)
		return ST_IO;
	if (di->first_cluster && !exfat_cluster_ok(fs, di->first_cluster))
		return ST_IO;
	if (di->size && !di->first_cluster)
		return ST_IO;
	if (di->size > ((uint64_t)fs->cluster_count << fs->cs_shift))
		return ST_IO;
	if ((di->attr & EXFAT_ATTR_DIR) && di->size > EXFAT_MAX_DIR_BYTES)
		return ST_IO;
	return ST_OK;
}

/* Read and parse the set whose File entry is slot `idx'.  The raw set goes
 * to `set' (EXFAT_SET_MAX_BYTES), the name to `name'.  Returns as
 * exfat_set_parse(), or ST_NOT_FOUND when slot `idx' holds no File entry in
 * use, or a read error. */
int exfat_set_read(exfat_diter_t *it, uint64_t idx, uint8_t *set,
		   exfat_dentry_info_t *di, uint16_t *name)
{
	const uint8_t *e = exfat_diter_get(it, idx);
	unsigned sc;
	int r;

	if (!e)
		return it->err ? it->err : ST_NOT_FOUND;
	if (e[EXFAT_DE_TYPE] != EXFAT_ET_FILE)
		return ST_NOT_FOUND;
	sc = e[EXFAT_DE_SECONDARY_COUNT];
	if (sc < EXFAT_SET_MIN_SECONDARY || sc > EXFAT_SET_MAX_SECONDARY)
		return ST_IO;
	if (idx + sc >= exfat_dir_slots(it->d))
		return ST_IO; /* runs past the directory's end */
	r = exfat_diter_copy(it, idx, sc + 1, set);
	if (r != ST_OK)
		return r == ST_EOF ? ST_IO : r;
	r = exfat_set_parse(it->fs, set, di, name);
	if (r == ST_OK || r == ST_UNSUPPORTED)
		di->index = (uint32_t)idx;
	return r;
}

/* Two names equal under the volume's folding? */
int exfat_name_eq(exfat_fs_t *fs, const uint16_t *a, const uint16_t *up,
		  unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (exfat_upc(fs, a[i]) != up[i])
			return 0;
	return 1;
}

/* ===========================================================================
 * 16. Directory modification
 *
 * Entry sets are changed in the cached directory pages, under the metadata
 * lock exclusive, and the changed bytes are queued (the pages held, so they
 * cannot be reclaimed with the change in them).  The commit writes the
 * queued sectors -- after the FAT and bitmap changes they depend on are on
 * the device (section 17).  Directory pages are never marked dirty in the
 * page cache: its background writeback must not decide this order.
 * =========================================================================== */

/* Queue bytes [lo, hi) of page `pidx' of directory `d' (held page `pg'). */
static int exfat_dirw_add(exfat_fs_t *fs, exfat_stream_t *d, unsigned long pidx,
			  pc_page_t **pg, unsigned lo, unsigned hi)
{
	for (unsigned i = 0; i < fs->ndirw; i++) {
		struct exfat_dirw *q = &fs->dirw[i];

		if (q->d == d && q->pidx == pidx) {
			if (lo < q->lo)
				q->lo = lo;
			if (hi > q->hi)
				q->hi = hi;
			return ST_OK;
		}
	}
	if (fs->ndirw >= EXFAT_DIRW_MAX)
		return ST_BUSY;
	fs->dirw[fs->ndirw].pg = *pg;
	fs->dirw[fs->ndirw].d = d;
	fs->dirw[fs->ndirw].pidx = pidx;
	fs->dirw[fs->ndirw].lo = lo;
	fs->dirw[fs->ndirw].hi = hi;
	fs->ndirw++;
	exfat_stream_hold(d);
	*pg = NULL; /* the queue owns the hold now */
	return ST_OK;
}

/* The held, queued page `pidx' of `d', taking it from the queue when it is
 * there already. */
static pc_page_t *exfat_dirw_page(exfat_fs_t *fs, exfat_stream_t *d,
				  unsigned long pidx, int *queued)
{
	unsigned long chain = exfat_chain_of(d);

	for (unsigned i = 0; i < fs->ndirw; i++)
		if (fs->dirw[i].d == d && fs->dirw[i].pidx == pidx) {
			*queued = 1;
			return fs->dirw[i].pg;
		}
	*queued = 0;
	return pagecache_get_held(chain, pidx, (unsigned long)d->size, &fs->sb,
				  chain);
}

/* Write `count' entries from `buf' at slot `idx' of directory `d': into the
 * cached pages, queued for the commit.  Metadata lock exclusive. */
int exfat_dir_write(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count, const uint8_t *buf)
{
	exfat_assert_excl(fs);
	if (idx + count > exfat_dir_slots(d)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	/* Room for the pages this touches (a set spans at most two), so a
	 * set is never half queued. */
	if (fs->ndirw + 2 > EXFAT_DIRW_MAX) {
		int r = fs->in_commit ? exfat_commit_queue(fs) :
					exfat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	for (unsigned i = 0; i < count;) {
		uint64_t byte = (idx + i) << EXFAT_DENTRY_SHIFT;
		unsigned long pidx = (unsigned long)(byte >> PAGE_SHIFT);
		unsigned off = (unsigned)(byte & (PAGE_SIZE - 1));
		unsigned n = (PAGE_SIZE - off) >> EXFAT_DENTRY_SHIFT;
		int queued;
		pc_page_t *pg;

		if (n > count - i)
			n = count - i;
		pg = exfat_dirw_page(fs, d, pidx, &queued);
		if (!pg)
			return ST_IO;
		mm_memcpy(pg->data + off, buf + i * EXFAT_DENTRY_SIZE,
			  n * EXFAT_DENTRY_SIZE);
		if (!queued) {
			int r = exfat_dirw_add(fs, d, pidx, &pg, off,
					       off + n * EXFAT_DENTRY_SIZE);

			if (pg)
				pagecache_put(pg);
			if (r != ST_OK)
				return r;
		} else {
			int r = exfat_dirw_add(fs, d, pidx, &pg, off,
					       off + n * EXFAT_DENTRY_SIZE);

			if (r != ST_OK)
				return r;
		}
		i += n;
	}
	return ST_OK;
}

/* Mark `count' entries from slot `idx' unused (InUse cleared, the rest of
 * each entry kept). */
int exfat_dir_clear(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count, uint8_t *scratch)
{
	exfat_diter_t it;
	int r;

	exfat_diter_init(&it, fs, d);
	r = exfat_diter_copy(&it, idx, count, scratch);
	exfat_diter_done(&it);
	if (r != ST_OK)
		return r == ST_EOF ? ST_IO : r;
	for (unsigned i = 0; i < count; i++)
		scratch[i * EXFAT_DENTRY_SIZE] &= (uint8_t)~EXFAT_ET_INUSE;
	return exfat_dir_write(fs, d, idx, count, scratch);
}

/* Write every queued directory change to the device and release the
 * queue.  Part of the commit; metadata lock exclusive. */
int exfat_dirw_flush(exfat_fs_t *fs)
{
	unsigned keep = 0;
	int ret = ST_OK;

	exfat_assert_excl(fs);
	for (unsigned i = 0; i < fs->ndirw; i++) {
		struct exfat_dirw *q = &fs->dirw[i];
		exfat_stream_t *d = q->d;
		uint64_t pbyte = (uint64_t)q->pidx << PAGE_SHIFT;
		unsigned lo = q->lo & ~(fs->dev_ss - 1);
		unsigned hi = (unsigned)exfat_round_up(q->hi, fs->dev_ss);
		unsigned run_off = 0, run_len = 0;
		unsigned long run_lba = 0;
		int r = ST_OK;

		/* Sector by sector through the stream's blocks (a page may
		 * span clusters that are not adjacent on the device); every
		 * run of consecutive device sectors -- across cluster
		 * boundaries too -- goes out as ONE write, so an entry set
		 * placed on adjacent sectors is one device command. */
		for (unsigned off = lo; off < hi && r == ST_OK;
		     off += fs->dev_ss) {
			uint64_t sb = pbyte + off;
			unsigned long blk = (unsigned long)(sb >> fs->bs_shift);
			unsigned long lba = exfat_st_block_lba(fs, d, blk);

			if (lba)
				lba += (unsigned long)((sb & (fs->bs - 1)) >>
						       fs->dev_shift);
			if (run_len && lba && lba == run_lba + (run_len >>
								 fs->dev_shift)) {
				run_len += fs->dev_ss;
				continue;
			}
			if (run_len)
				r = exfat_dev_write(fs, run_lba,
						    run_len >> fs->dev_shift,
						    q->pg->data + run_off,
						    EXFAT_W_HEAP);
			run_len = 0;
			if (!lba) {
				/* Beyond the directory's clusters: nothing to
				 * store there (cannot happen for written
				 * slots). */
				WARN_ON_ONCE(sb < d->size);
				continue;
			}
			run_lba = lba;
			run_off = off;
			run_len = fs->dev_ss;
		}
		if (r == ST_OK && run_len)
			r = exfat_dev_write(fs, run_lba, run_len >> fs->dev_shift,
					    q->pg->data + run_off, EXFAT_W_HEAP);
		if (r != ST_OK) {
			/* Not on the device: stays queued (the next commit
			 * tries again; the frees that depend on it wait). */
			fs->dirw[keep++] = *q;
			if (ret == ST_OK)
				ret = r;
			continue;
		}
		pagecache_put(q->pg);
		exfat_stream_put(d);
		q->pg = NULL;
		q->d = NULL;
	}
	fs->ndirw = keep;
	return ret;
}

/* Build a fresh entry set: File, Stream, names.  Returns the number of
 * entries.  SetChecksum is filled. */
unsigned exfat_set_build(exfat_fs_t *fs, uint8_t *set,
			 const uint16_t *name, unsigned nlen,
			 uint16_t hash, uint16_t attr, uint8_t sflags,
			 uint32_t first, uint64_t size, uint64_t vdl)
{
	unsigned nent = (nlen + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;
	unsigned count = 2 + nent;
	uint8_t *fe = set, *se = set + EXFAT_DENTRY_SIZE;
	uint32_t ts;
	uint8_t cs10, utc;

	mm_memset(set, 0, count * EXFAT_DENTRY_SIZE);
	fe[EXFAT_DE_TYPE] = EXFAT_ET_FILE;
	fe[EXFAT_DE_SECONDARY_COUNT] = (uint8_t)(count - 1);
	exfat_put16(fe + EXFAT_FILE_ATTR, attr);
	exfat_now(fs, &ts, &cs10, &utc);
	exfat_put32(fe + EXFAT_FILE_CTIME, ts);
	exfat_put32(fe + EXFAT_FILE_MTIME, ts);
	exfat_put32(fe + EXFAT_FILE_ATIME, ts);
	fe[EXFAT_FILE_CTIME_10MS] = cs10;
	fe[EXFAT_FILE_MTIME_10MS] = cs10;
	fe[EXFAT_FILE_CTIME_UTC] = utc;
	fe[EXFAT_FILE_MTIME_UTC] = utc;
	fe[EXFAT_FILE_ATIME_UTC] = utc;
	se[EXFAT_DE_TYPE] = EXFAT_ET_STREAM;
	se[EXFAT_DE_GSFLAGS] = sflags;
	se[EXFAT_STREAM_NAME_LEN] = (uint8_t)nlen;
	exfat_put16(se + EXFAT_STREAM_NAME_HASH, hash);
	exfat_put64(se + EXFAT_STREAM_VDL, vdl);
	exfat_put32(se + EXFAT_STREAM_FIRST_CLUSTER, first);
	exfat_put64(se + EXFAT_STREAM_DATA_LENGTH, size);
	for (unsigned i = 0; i < nent; i++) {
		uint8_t *ne = set + (2 + i) * EXFAT_DENTRY_SIZE;

		ne[EXFAT_DE_TYPE] = EXFAT_ET_NAME;
		for (unsigned k = 0; k < EXFAT_NAME_PER_ENTRY; k++) {
			unsigned ci = i * EXFAT_NAME_PER_ENTRY + k;

			exfat_put16(ne + EXFAT_NAME_CHARS_OFF + 2 * k,
				    ci < nlen ? name[ci] : 0);
		}
	}
	exfat_put16(fe + EXFAT_DE_SET_CHECKSUM, exfat_set_checksum(set, count));
	return count;
}

/* Rebuild `old' (a valid set) under a new name into `out': the File entry
 * and the stream entry keep every field (NameLength and NameHash change),
 * the name entries are new, and every other secondary of the old set
 * (vendor entries, unknown benign ones) follows unchanged.  Returns the
 * number of entries, or 0 when they would not fit in a set. */
unsigned exfat_set_rename(const uint8_t *old, const uint16_t *name,
			  unsigned nlen, uint16_t hash, uint8_t *out)
{
	unsigned osc = old[EXFAT_DE_SECONDARY_COUNT];
	unsigned onlen = old[EXFAT_DENTRY_SIZE + EXFAT_STREAM_NAME_LEN];
	unsigned onent = (onlen + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;
	unsigned nent = (nlen + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;
	unsigned extra = osc - 1 - onent;
	unsigned count = 2 + nent + extra;

	if (count > EXFAT_SET_MAX_ENTRIES)
		return 0;
	mm_memcpy(out, old, 2 * EXFAT_DENTRY_SIZE);
	out[EXFAT_DE_SECONDARY_COUNT] = (uint8_t)(count - 1);
	out[EXFAT_DENTRY_SIZE + EXFAT_STREAM_NAME_LEN] = (uint8_t)nlen;
	exfat_put16(out + EXFAT_DENTRY_SIZE + EXFAT_STREAM_NAME_HASH, hash);
	for (unsigned i = 0; i < nent; i++) {
		uint8_t *ne = out + (2 + i) * EXFAT_DENTRY_SIZE;

		mm_memset(ne, 0, EXFAT_DENTRY_SIZE);
		ne[EXFAT_DE_TYPE] = EXFAT_ET_NAME;
		for (unsigned k = 0; k < EXFAT_NAME_PER_ENTRY; k++) {
			unsigned ci = i * EXFAT_NAME_PER_ENTRY + k;

			exfat_put16(ne + EXFAT_NAME_CHARS_OFF + 2 * k,
				    ci < nlen ? name[ci] : 0);
		}
	}
	mm_memcpy(out + (2 + nent) * EXFAT_DENTRY_SIZE,
		  old + (2 + onent) * EXFAT_DENTRY_SIZE,
		  extra * EXFAT_DENTRY_SIZE);
	exfat_put16(out + EXFAT_DE_SET_CHECKSUM, exfat_set_checksum(out, count));
	return count;
}

/* Grow directory `d' by `ncl' zeroed clusters.  Metadata lock exclusive,
 * armed. */
static int exfat_dir_grow(exfat_fs_t *fs, exfat_stream_t *d, uint32_t ncl)
{
	struct exfat_run *runs;
	unsigned nruns;
	uint64_t new_size;
	int r, pinned = 0;

	exfat_assert_excl(fs);
	new_size = d->size + ((uint64_t)ncl << fs->cs_shift);
	if (new_size > EXFAT_MAX_DIR_BYTES)
		return ST_NOSPC;
	r = exfat_alloc(fs, exfat_st_last(d), ncl, &runs, &nruns);
	if (r != ST_OK)
		return r;
	/* Zero them on the device BEFORE anything references them: stale
	 * bytes there would read as entries. */
	for (unsigned i = 0; i < nruns && r == ST_OK; i++)
		r = exfat_dev_zero(fs,
				   fs->heap_lba +
					   ((uint64_t)(runs[i].start -
						       EXFAT_FIRST_CLUSTER)
					    << (fs->cs_shift - fs->dev_shift)),
				   (uint64_t)runs[i].len
					   << (fs->cs_shift - fs->dev_shift));
	if (r == ST_OK)
		r = exfat_stream_append(fs, d, runs, nruns, &pinned);
	if (r != ST_OK) {
		/* Not referenced (the append undid itself): straight back. */
		exfat_runs_unalloc(fs, runs, nruns, ncl, pinned);
		kfree(runs);
		return r;
	}
	kfree(runs);
	d->size = new_size;
	d->vdl = new_size;
	if (d->is_root)
		/* The root's length IS its FAT chain: the zeroed clusters
		 * and their own FAT entries must be on the device before the
		 * link to them is (exfat_mc_flush, exfat_mc_flush_links). */
		fs->root_zero_pending = 1;
	else
		exfat_stream_mark_dirty(fs, d);
	/* The page that held the old end (when clusters are smaller than a
	 * page) keeps zeros past it, which is what the new cluster holds. */
	return ST_OK;
}

/* Find `need' consecutive free slots in `d' (unused entries or past the end
 * marker), growing the directory when there are none.  *idx the first one.
 * Metadata lock exclusive, armed. */
/* Entries per 512-byte unit: the smallest sector any device has, so the
 * largest piece of a directory one write is sure to store whole. */
#define EXFAT_SET_UNIT_ENTRIES 16u

/* May a set of `count' entries lie at slot `idx' of `d'?  So that a power
 * cut can never leave half a set on the device:
 *   - a set of up to 16 entries lies inside one 512-byte unit (one device
 *     sector write); a longer one starts on a unit;
 *   - no set crosses a page (one queued directory write);
 *   - the device sectors under it are consecutive (one device command).
 * Slots past the directory's clusters do not fit. */
int exfat_slot_fits(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count)
{
	uint64_t first = idx << EXFAT_DENTRY_SHIFT;
	uint64_t last = ((idx + count) << EXFAT_DENTRY_SHIFT) - 1;
	unsigned long b0, b1, l0;

	if (idx + count > exfat_dir_slots(d))
		return 0;
	if (count <= EXFAT_SET_UNIT_ENTRIES) {
		if ((first >> 9) != (last >> 9))
			return 0;
	} else if (first & 511) {
		return 0;
	}
	if ((first >> PAGE_SHIFT) != (last >> PAGE_SHIFT))
		return 0;
	b0 = (unsigned long)(first >> fs->bs_shift);
	b1 = (unsigned long)(last >> fs->bs_shift);
	if (b0 == b1)
		return 1;
	l0 = exfat_st_block_lba(fs, d, b0);
	if (!l0)
		return 0;
	for (unsigned long b = b0 + 1; b <= b1; b++)
		if (exfat_st_block_lba(fs, d, b) !=
		    l0 + ((b - b0) << (fs->bs_shift - fs->dev_shift)))
			return 0;
	return 1;
}

/* One scan for `need' free slots (unused entries or past the end marker)
 * placed as exfat_slot_fits() requires -- or, with `any', anywhere.
 * 1 found (*idx), 0 none, < 0 error. */
static int exfat_dir_find_slots(exfat_fs_t *fs, exfat_stream_t *d,
				unsigned need, int any, uint64_t *idx)
{
	exfat_diter_t it;
	uint64_t i = 0, slots = exfat_dir_slots(d), run_start = 0, run = 0;
	int r = 0;

	exfat_diter_init(&it, fs, d);
	while (i < slots) {
		const uint8_t *e = exfat_diter_get(&it, i);
		uint8_t type;

		if (!e) {
			r = it.err ? it.err : ST_IO;
			break;
		}
		type = e[EXFAT_DE_TYPE];
		if (type == EXFAT_ET_EOD) {
			/* Everything from here on is free. */
			if (!run)
				run_start = i;
			for (uint64_t c = run_start; c + need <= slots; c++)
				if (any || exfat_slot_fits(fs, d, c, need)) {
					*idx = c;
					r = 1;
					break;
				}
			break;
		}
		if (!(type & EXFAT_ET_INUSE)) {
			if (!run)
				run_start = i;
			run++;
			if (run >= need) {
				uint64_t c = i + 1 - need;

				if (any || exfat_slot_fits(fs, d, c, need)) {
					*idx = c;
					r = 1;
					break;
				}
			}
			i++;
			continue;
		}
		run = 0;
		if (type == EXFAT_ET_FILE) {
			unsigned sc = e[EXFAT_DE_SECONDARY_COUNT];

			i += (sc >= EXFAT_SET_MIN_SECONDARY &&
			      sc <= EXFAT_SET_MAX_SECONDARY) ?
				     1 + sc :
				     1;
		} else {
			i += exfat_skip_slots(e);
		}
	}
	exfat_diter_done(&it);
	return r;
}

/* A set placed past the end-of-directory marker, on the boundary its
 * placement asks for, would sit behind end markers (0x00) -- and a reader
 * stops at the first of them.  The slots between the last entry and the set
 * become unused entries (a deleted File entry, 0x05, the rest zero) before
 * the set is written; they are queued first, so they reach the device first
 * (a stop in between leaves a few unused entries, nothing else).  Metadata
 * lock exclusive, outside the commit (uses the commit's scratch). */
static int exfat_dir_fill_gap(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx)
{
	exfat_diter_t it;
	uint64_t from = idx;
	int r = ST_OK;

	exfat_assert_excl(fs);
	exfat_diter_init(&it, fs, d);
	while (from > 0) {
		const uint8_t *e = exfat_diter_get(&it, from - 1);

		if (!e) {
			r = it.err ? it.err : ST_IO;
			break;
		}
		if (e[EXFAT_DE_TYPE] != EXFAT_ET_EOD)
			break;
		from--;
	}
	exfat_diter_done(&it);
	if (r != ST_OK)
		return r;
	while (from < idx) {
		unsigned n = (unsigned)exfat_min64(idx - from,
						   EXFAT_SET_MAX_ENTRIES);

		mm_memset(fs->wb_set, 0, n * EXFAT_DENTRY_SIZE);
		for (unsigned k = 0; k < n; k++)
			fs->wb_set[k * EXFAT_DENTRY_SIZE] =
				EXFAT_ET_FILE & (uint8_t)~EXFAT_ET_INUSE;
		r = exfat_dir_write(fs, d, from, n, fs->wb_set);
		if (r != ST_OK)
			return r;
		from += n;
	}
	return ST_OK;
}

/* Find `need' free slots for a new entry set, placed so that it is stored
 * in one piece (exfat_slot_fits), growing the directory when there is no
 * such place.  Only where even fresh clusters cannot give one (a set longer
 * than a sector whose new clusters are not adjacent) does any free run do.
 * Metadata lock exclusive, writes allowed. */
int exfat_dir_alloc_slots(exfat_fs_t *fs, exfat_stream_t *d,
			  unsigned need, uint64_t *idx)
{
	int r;

	exfat_assert_excl(fs);
	if (!d->first_cluster)
		/* A directory without a cluster is corrupt (its entries could
		 * not be told apart from another such directory's). */
		return exfat_corrupt(fs, d->entry_index,
				     "directory without a cluster");
	for (int round = 0; round < 3; round++) {
		uint64_t bytes;
		uint32_t ncl;

		r = exfat_dir_find_slots(fs, d, need, 0, idx);
		if (r != 0)
			return r < 0 ? r : exfat_dir_fill_gap(fs, d, *idx);
		/* Room for the set plus a unit of alignment slack -- and at
		 * least a page: a directory that is a FAT chain cannot grow
		 * without a moment in which its chain on the device is longer
		 * than its DataLength (the FAT link is stored before the
		 * parent's entry set; fsck repairs that by trimming the tail,
		 * nothing is lost), so it grows seldom, in big steps. */
		bytes = (uint64_t)(need + EXFAT_SET_UNIT_ENTRIES)
			<< EXFAT_DENTRY_SHIFT;
		if (bytes < PAGE_SIZE)
			bytes = PAGE_SIZE;
		ncl = (uint32_t)((bytes + fs->cs - 1) >> fs->cs_shift);
		r = exfat_dir_grow(fs, d, ncl);
		if (r == ST_NOSPC)
			break; /* full (volume or 256 MB): any free run then */
		if (r != ST_OK)
			return r;
	}
	r = exfat_dir_find_slots(fs, d, need, 1, idx);
	if (r > 0)
		return exfat_dir_fill_gap(fs, d, *idx);
	return r < 0 ? r : ST_NOSPC;
}

/* Is the directory empty (no entry in use besides, for the root, the
 * volume's own)?  1 / 0 / error. */
int exfat_dir_empty(exfat_fs_t *fs, exfat_stream_t *d)
{
	exfat_diter_t it;
	uint64_t slots = exfat_dir_slots(d);
	int r = 1;

	exfat_diter_init(&it, fs, d);
	for (uint64_t i = 0; i < slots; i++) {
		const uint8_t *e = exfat_diter_get(&it, i);

		if (!e) {
			r = it.err ? it.err : 1;
			break;
		}
		if (e[EXFAT_DE_TYPE] == EXFAT_ET_EOD)
			break;
		if (e[EXFAT_DE_TYPE] & EXFAT_ET_INUSE) {
			r = 0;
			break;
		}
	}
	exfat_diter_done(&it);
	return r;
}

/* Write a stream's in-memory state into its entry set (queued).  The set is
 * re-read and checked to still be this stream's: a mismatch means the
 * directory changed behind our back (corruption).  Metadata lock exclusive. */
int exfat_stream_store(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_dentry_info_t di;
	exfat_diter_t it;
	uint8_t *set = fs->wb_set;
	uint8_t *se = set + EXFAT_DENTRY_SIZE;
	unsigned count;
	int r;

	exfat_assert_excl(fs);
	if (st->is_root || st->orphan || !st->parent)
		return ST_OK;
	exfat_diter_init(&it, fs, st->parent);
	r = exfat_set_read(&it, st->entry_index, set, &di, fs->wb_name);
	exfat_diter_done(&it);
	if (r == ST_NOT_FOUND || r == ST_IO ||
	    di.secondary_count != st->secondary_count)
		return exfat_corrupt(fs, st->entry_index,
				     "entry set of a file changed under it (slot %u)",
				     st->entry_index);
	if (r != ST_OK)
		return r;
	count = di.secondary_count + 1u;
	exfat_put16(set + EXFAT_FILE_ATTR, st->attr);
	exfat_put32(set + EXFAT_FILE_CTIME, st->ctime);
	exfat_put32(set + EXFAT_FILE_MTIME, st->mtime);
	exfat_put32(set + EXFAT_FILE_ATIME, st->atime);
	set[EXFAT_FILE_CTIME_10MS] = st->ctime_cs;
	set[EXFAT_FILE_MTIME_10MS] = st->mtime_cs;
	set[EXFAT_FILE_CTIME_UTC] = st->ctime_utc;
	set[EXFAT_FILE_MTIME_UTC] = st->mtime_utc;
	set[EXFAT_FILE_ATIME_UTC] = st->atime_utc;
	se[EXFAT_DE_GSFLAGS] = st->sflags;
	exfat_put64(se + EXFAT_STREAM_VDL, st->vdl);
	exfat_put32(se + EXFAT_STREAM_FIRST_CLUSTER, st->first_cluster);
	exfat_put64(se + EXFAT_STREAM_DATA_LENGTH, st->size);
	exfat_put16(set + EXFAT_DE_SET_CHECKSUM, exfat_set_checksum(set, count));
	return exfat_dir_write(fs, st->parent, st->entry_index, count, set);
}

/* ---- readdir ---------------------------------------------------------------- */

/* One record (laid out as the other filesystems do: name at byte 19, the
 * record rounded to 8), into the KERNEL buffer readdir assembles under the
 * lock.  0 when it does not fit. */
static unsigned exfat_put_dirent(void *buf, unsigned cap, unsigned off,
				 const char *name, unsigned nl, uint64_t ino,
				 int type, uint64_t next_pos)
{
	unsigned reclen = (19 + nl + 1 + 7) & ~7u;
	struct dirent64 *d;

	if (off + reclen > cap)
		return 0;
	d = (struct dirent64 *)((char *)buf + off);
	d->d_ino = ino;
	d->d_off = (int64_t)next_pos;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = (uint8_t)type;
	mm_memcpy(d->d_name, name, nl);
	d->d_name[nl] = 0;
	return reclen;
}

/* Largest batch of records one readdir call returns. */
#define EXFAT_READDIR_MAX (64UL << 10)

/* readdir: the records are assembled in a kernel buffer under the
 * metadata lock (shared) and copied to the caller after it is dropped. */
long exfat_readdir_op(vfs_file_t *f, void *ubuf, long ubytes)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	exfat_fs_t *fs;
	exfat_stream_t *d;
	exfat_ctx_t *ctx;
	exfat_diter_t it;
	unsigned off = 0, rl;
	uint64_t slots, saved_pos;
	long ret, bytes;
	void *buf;

	if (!h || !ubuf)
		return ST_INVALID;
	d = h->st;
	if (!d->is_dir)
		return ST_NOTDIR;
	if (ubytes <= 0)
		return ST_INVALID;
	fs = h->fs;
	bytes = ubytes > (long)EXFAT_READDIR_MAX ? (long)EXFAT_READDIR_MAX :
						    ubytes;
	buf = kalloc((size_t)bytes);
	if (!buf)
		return ST_NOMEM;
	ctx = exfat_ctx_new();
	if (!ctx) {
		kfree(buf);
		return ST_NOMEM;
	}
	saved_pos = h->dirpos;
	exfat_meta_rlock(fs);
	if (h->dirpos == 0) {
		rl = exfat_put_dirent(buf, (unsigned)bytes, off, ".", 1,
				      exfat_st_ino(fs, d), DT_DIR, 1);
		if (!rl) {
			ret = ST_INVALID;
			goto out;
		}
		off += rl;
		h->dirpos = 1;
	}
	if (h->dirpos == 1) {
		rl = exfat_put_dirent(buf, (unsigned)bytes, off, "..", 2,
				      h->parent_ino, DT_DIR, 2);
		if (!rl) {
			ret = off;
			goto out;
		}
		off += rl;
		h->dirpos = EXFAT_DIRPOS_FIRST;
	}
	slots = exfat_dir_slots(d);
	exfat_diter_init(&it, fs, d);
	for (uint64_t i = h->dirpos - EXFAT_DIRPOS_FIRST; i < slots;) {
		const uint8_t *e = exfat_diter_get(&it, i);
		exfat_dentry_info_t di;
		uint64_t ino;
		int r, nl;

		if (!e) {
			if (it.err && off == 0) {
				exfat_diter_done(&it);
				ret = it.err;
				goto out;
			}
			break;
		}
		if (e[EXFAT_DE_TYPE] == EXFAT_ET_EOD) {
			h->dirpos = slots + EXFAT_DIRPOS_FIRST;
			break;
		}
		if (!(e[EXFAT_DE_TYPE] & EXFAT_ET_INUSE)) {
			i++;
			h->dirpos = i + EXFAT_DIRPOS_FIRST;
			continue;
		}
		if (e[EXFAT_DE_TYPE] != EXFAT_ET_FILE) {
			i += exfat_skip_slots(e);
			h->dirpos = i + EXFAT_DIRPOS_FIRST;
			continue;
		}
		r = exfat_set_read(&it, i, ctx->set, &di, ctx->name);
		if (r != ST_OK) {
			unsigned sc = ctx->set[EXFAT_DE_SECONDARY_COUNT];

			/* Listed without it; corruption all the same: the
			 * errors= policy applies (read-only by default), so
			 * nothing is written next to a set nobody can trust. */
			if (r == ST_IO)
				(void)exfat_corrupt(fs, i,
						    "%s: entry set at slot %llu skipped",
						    exfat_name_of(fs),
						    (unsigned long long)i);
			if (r != ST_IO && r != ST_UNSUPPORTED &&
			    r != ST_NOT_FOUND) {
				if (off == 0) {
					exfat_diter_done(&it);
					ret = r;
					goto out;
				}
				break;
			}
			i += (r == ST_UNSUPPORTED && sc >= 2 && sc <= 18) ?
				     1 + sc :
				     1;
			h->dirpos = i + EXFAT_DIRPOS_FIRST;
			continue;
		}
		nl = exfat_utf16_to_utf8(ctx->name, di.name_len, ctx->u8,
					 sizeof(ctx->u8));
		if (nl <= 0) {
			i += 1 + di.secondary_count;
			h->dirpos = i + EXFAT_DIRPOS_FIRST;
			continue;
		}
		{
			exfat_stream_t *s = exfat_stream_find(
				fs, EXFAT_KEY(d->first_cluster, i));

			if (s) {
				ino = exfat_st_ino(fs, s);
				exfat_stream_put(s);
			} else {
				ino = exfat_ino(fs, di.first_cluster,
						d->first_cluster, (uint32_t)i);
			}
		}
		rl = exfat_put_dirent(buf, (unsigned)bytes, off, ctx->u8,
				      (unsigned)nl, ino,
				      (di.attr & EXFAT_ATTR_DIR) ? DT_DIR :
								   DT_REG,
				      i + 1 + di.secondary_count +
					      EXFAT_DIRPOS_FIRST);
		if (!rl) {
			if (off == 0) {
				exfat_diter_done(&it);
				ret = ST_INVALID;
				goto out;
			}
			break;
		}
		off += rl;
		i += 1 + di.secondary_count;
		h->dirpos = i + EXFAT_DIRPOS_FIRST;
	}
	exfat_diter_done(&it);
	ret = off;
out:
	exfat_meta_runlock(fs);
	exfat_ctx_free(ctx);
	if (ret > 0) {
		/* No lock held while user memory is written. */
		smap_disable();
		mm_memcpy(ubuf, buf, (size_t)ret);
		smap_enable();
	} else if (ret < 0) {
		h->dirpos = saved_pos;
	}
	kfree(buf);
	return ret;
}
