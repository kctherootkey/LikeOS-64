/* LikeOS - FAT filesystem driver: directory reading and modification: entry
 * sets, the directory write queue, free-slot search, directory growth,
 * readdir
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ===========================================================================
 * 14. Directory reading
 *
 * A directory is a stream of 32-byte entries, read through the page cache
 * like file data (its pages are never dirty: changes are written through
 * the directory write queue, section 16).  An entry never straddles a page.
 * The first entry whose name starts with 0x00 ends the directory; 0xE5
 * marks a free entry.  An entry set is up to 20 long-name entries (sequence
 * numbers counting down to 1, the first one flagged 0x40, every one carrying
 * the checksum of the short name) followed by the short entry.  Long-name
 * entries that do not form such a run in front of a short entry with their
 * checksum are orphans: ignored (the short entry then stands alone).  The
 * volume label and the "." / ".." entries are not files.
 * =========================================================================== */

void fat_diter_init(fat_diter_t *it, fat_fs_t *fs, fat_stream_t *d)
{
	it->fs = fs;
	it->d = d;
	it->pg = NULL;
	it->pidx = 0;
	it->err = ST_OK;
}

void fat_diter_done(fat_diter_t *it)
{
	if (it->pg)
		pagecache_put(it->pg);
	it->pg = NULL;
}

/* Entry `idx' of the directory: a pointer into a held cached page, valid
 * until the next call or fat_diter_done().  NULL past the end (err 0) or
 * on a read error (err set).  Metadata lock held. */
const uint8_t *fat_diter_get(fat_diter_t *it, uint64_t idx)
{
	fat_stream_t *d = it->d;
	uint64_t byte;
	unsigned long page;

	if (idx >= fat_dir_slots(d))
		return NULL;
	byte = idx << FAT_DENTRY_SHIFT;
	page = (unsigned long)(byte >> PAGE_SHIFT);
	if (!it->pg || it->pidx != page) {
		unsigned long chain = fat_chain_of(d);

		fat_diter_done(it);
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
int fat_diter_copy(fat_diter_t *it, uint64_t idx, unsigned count,
			  uint8_t *buf)
{
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *e = fat_diter_get(it, idx + i);

		if (!e)
			return it->err ? it->err : ST_EOF;
		mm_memcpy(buf + i * FAT_DENTRY_SIZE, e, FAT_DENTRY_SIZE);
	}
	return ST_OK;
}

/* ---- entry kinds ---- */

/* Describe a short entry. */
void fat_di_from_sfn(fat_fs_t *fs, const uint8_t *e, uint32_t idx,
			    fat_dentry_info_t *di)
{
	mm_memset(di, 0, sizeof(*di));
	di->index = idx;
	mm_memcpy(di->name83, e, 11);
	di->attr = e[FAT_DE_ATTR];
	di->ntres = e[FAT_DE_NTRES];
	di->ctime_cs = e[FAT_DE_CTIME_CS];
	di->ctime = fat_le16(e + FAT_DE_CTIME);
	di->cdate = fat_le16(e + FAT_DE_CDATE);
	di->adate = fat_le16(e + FAT_DE_ADATE);
	di->mtime = fat_le16(e + FAT_DE_MTIME);
	di->mdate = fat_le16(e + FAT_DE_MDATE);
	di->first_cluster = fat_de_cluster(fs, e);
	di->size = fat_le32(e + FAT_DE_SIZE);
	di->chksum = fat_lfn_checksum(e);
}

/* Is what a short entry says possible on this volume?  ST_OK or ST_IO. */
static int fat_di_check(fat_fs_t *fs, const fat_dentry_info_t *di)
{
	if (di->first_cluster && !fat_cluster_ok(fs, di->first_cluster))
		return ST_IO;
	if (di->attr & FAT_ATTR_DIR) {
		if (!di->first_cluster)
			return ST_IO;
		return ST_OK;
	}
	if ((uint64_t)di->size > ((uint64_t)fs->cluster_count << fs->cs_shift))
		return ST_IO;
	if (di->size && !di->first_cluster)
		return ST_IO;
	return ST_OK;
}

/* Read the entry set whose short entry is slot `idx': the short entry into
 * *di, the long name (when a valid run of long-name entries stands in front
 * of it) into `lname' (di->lname_len units, room for FAT_LNAME_UNITS).
 *
 *   ST_OK         a set to use
 *   ST_NOT_FOUND  slot `idx' holds no short entry of a file or directory
 *   ST_IO         the short entry describes something impossible
 *                 (*di is filled; the caller decides: a lookup reports,
 *                 readdir skips)
 *   or a read error.  Metadata lock held. */
int fat_set_read(fat_diter_t *it, uint64_t idx, fat_dentry_info_t *di,
			uint16_t *lname)
{
	const uint8_t *e = fat_diter_get(it, idx);
	unsigned count = 0, len = 0;

	if (!e)
		return it->err ? it->err : ST_NOT_FOUND;
	if (!fat_de_is_object(e))
		return ST_NOT_FOUND;
	fat_di_from_sfn(it->fs, e, (uint32_t)idx, di);
	/* The long-name entries, walking back from the short entry: sequence
	 * 1, 2, ... with the checksum, until the one flagged last. */
	for (unsigned k = 1; k <= FAT_LFN_MAX_ENTRIES && k <= idx; k++) {
		const uint8_t *le = fat_diter_get(it, idx - k);
		uint16_t *dst = lname + (k - 1) * FAT_LFN_CHARS;

		if (!le) {
			if (it->err)
				return it->err;
			break;
		}
		if (!fat_de_in_use(le) || !fat_de_is_lfn(le) ||
		    (le[FAT_LFN_ORD] & FAT_LFN_ORD_MASK) != k ||
		    le[FAT_LFN_CHKSUM] != di->chksum || le[FAT_LFN_TYPE] != 0)
			break;
		for (unsigned u = 0; u < 5; u++)
			dst[u] = fat_le16(le + FAT_LFN_NAME1 + 2 * u);
		for (unsigned u = 0; u < 6; u++)
			dst[5 + u] = fat_le16(le + FAT_LFN_NAME2 + 2 * u);
		for (unsigned u = 0; u < 2; u++)
			dst[11 + u] = fat_le16(le + FAT_LFN_NAME3 + 2 * u);
		if (le[FAT_LFN_ORD] & FAT_LFN_LAST) {
			count = k;
			break;
		}
	}
	if (count) {
		unsigned max = count * FAT_LFN_CHARS;

		while (len < max && lname[len] != 0)
			len++;
		if (len == 0 || len > FAT_NAME_MAX)
			count = 0; /* empty or too long: not a name */
	}
	di->lfn_count = (uint8_t)count;
	di->lname_len = (uint16_t)(count ? len : 0);
	return fat_di_check(it->fs, di);
}

/* The name a set is listed under: its long name, or its short name as
 * shown.  Into `out' (FAT_LNAME_UNITS units); returns the length. */
unsigned fat_set_name(fat_fs_t *fs, const fat_dentry_info_t *di,
			     const uint16_t *lname, uint16_t *out)
{
	if (di->lname_len) {
		if (out != lname)
			mm_memcpy(out, lname, di->lname_len * sizeof(uint16_t));
		return di->lname_len;
	}
	return fat_sfn_display(fs, di->name83, di->ntres, out);
}

/* Does the set answer to the folded name ctx->up (`n' units)?  Its long
 * name and its short name both do. */
int fat_set_matches(fat_fs_t *fs, fat_ctx_t *ctx,
			   const fat_dentry_info_t *di, const uint16_t *lname,
			   unsigned n)
{
	unsigned sl;

	if (di->lname_len && fat_name_eq(lname, di->lname_len, ctx->up, n))
		return 1;
	sl = fat_sfn_display(fs, di->name83, di->ntres, ctx->sname);
	return sl && fat_name_eq(ctx->sname, sl, ctx->up, n);
}

int fat_sfn_scan(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *d,
			unsigned ncand, int64_t exclude)
{
	fat_diter_t it;
	uint64_t slots = fat_dir_slots(d);
	int r = ST_OK;

	fat_assert_meta(fs);
	for (unsigned k = 0; k < ncand; k++)
		ctx->sfn_taken[k] = 0;
	fat_diter_init(&it, fs, d);
	for (uint64_t i = 0; i < slots; i++) {
		const uint8_t *e = fat_diter_get(&it, i);

		if (!e) {
			r = it.err ? it.err : ST_OK;
			break;
		}
		if (e[0] == FAT_NAME_END)
			break;
		if (!fat_de_in_use(e) || fat_de_is_lfn(e) ||
		    (e[FAT_DE_ATTR] & FAT_ATTR_VOLUME))
			continue;
		if ((int64_t)i == exclude)
			continue;
		for (unsigned k = 0; k < ncand; k++) {
			if (ctx->sfn_taken[k])
				continue;
			int same = 1;

			for (unsigned b = 0; b < 11; b++)
				if (e[b] != ctx->sfn_cand[k][b]) {
					same = 0;
					break;
				}
			if (same)
				ctx->sfn_taken[k] = 1;
		}
	}
	fat_diter_done(&it);
	return r;
}

/* ===========================================================================
 * 15. Name lookup and path resolution
 * =========================================================================== */

/* The held, queued page `pidx' of `d', taking it from the queue when it is
 * there already. */
static pc_page_t *fat_dirw_page(fat_fs_t *fs, fat_stream_t *d,
				unsigned long pidx, int *queued)
{
	unsigned long chain = fat_chain_of(d);

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
int fat_dir_write(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count, const uint8_t *buf)
{
	fat_assert_excl(fs);
	if (idx + count > fat_dir_slots(d)) {
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	/* Room for the pages this touches (a set spans at most two), so a set
	 * is never half queued. */
	if (fs->ndirw + 2 > FAT_DIRW_MAX) {
		int r = fs->in_commit ? fat_commit_queue(fs) : fat_commit(fs);

		if (r != ST_OK)
			return r;
	}
	for (unsigned i = 0; i < count;) {
		uint64_t byte = (idx + i) << FAT_DENTRY_SHIFT;
		unsigned long pidx = (unsigned long)(byte >> PAGE_SHIFT);
		unsigned off = (unsigned)(byte & (PAGE_SIZE - 1));
		unsigned n = (PAGE_SIZE - off) >> FAT_DENTRY_SHIFT;
		int queued, r;
		pc_page_t *pg;

		if (n > count - i)
			n = count - i;
		pg = fat_dirw_page(fs, d, pidx, &queued);
		if (!pg)
			return ST_IO;
		mm_memcpy(pg->data + off, buf + i * FAT_DENTRY_SIZE,
			  n * FAT_DENTRY_SIZE);
		r = fat_dirw_add(fs, d, pidx, &pg, off,
				 off + n * FAT_DENTRY_SIZE);
		if (!queued && pg)
			pagecache_put(pg);
		if (r != ST_OK)
			return r;
		i += n;
	}
	return ST_OK;
}

/* Mark `count' entries from slot `idx' free (0xE5 in the first byte, the
 * rest of each entry kept, as every implementation does: undelete tools
 * rely on it). */
int fat_dir_clear(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count, uint8_t *scratch)
{
	fat_diter_t it;
	int r;

	fat_diter_init(&it, fs, d);
	r = fat_diter_copy(&it, idx, count, scratch);
	fat_diter_done(&it);
	if (r != ST_OK)
		return r == ST_EOF ? ST_IO : r;
	for (unsigned i = 0; i < count; i++)
		scratch[i * FAT_DENTRY_SIZE] = FAT_NAME_DELETED;
	return fat_dir_write(fs, d, idx, count, scratch);
}

/* Write every queued directory change to the device and release the
 * queue.  Part of the commit; metadata lock exclusive. */
int fat_dirw_flush(fat_fs_t *fs)
{
	unsigned keep = 0;
	int ret = ST_OK;

	fat_assert_excl(fs);
	for (unsigned i = 0; i < fs->ndirw; i++) {
		struct fat_dirw *q = &fs->dirw[i];
		fat_stream_t *d = q->d;
		uint64_t pbyte = (uint64_t)q->pidx << PAGE_SHIFT;
		unsigned lo = q->lo & ~(fs->dev_ss - 1);
		unsigned hi = (unsigned)fat_round_up(q->hi, fs->dev_ss);
		unsigned run_off = 0, run_len = 0;
		unsigned long run_lba = 0;
		int kind = d->fixed_root ? FAT_W_ROOT : FAT_W_DATA;
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
			unsigned long lba = fat_st_block_lba(fs, d, blk);

			if (lba)
				lba += (unsigned long)((sb & (fs->bs - 1)) >>
						       fs->dev_shift);
			if (sb >= d->size)
				lba = 0; /* past the directory (fixed root) */
			if (run_len && lba &&
			    lba == run_lba + (run_len >> fs->dev_shift)) {
				run_len += fs->dev_ss;
				continue;
			}
			if (run_len)
				r = fat_dev_write(fs, run_lba,
						  run_len >> fs->dev_shift,
						  q->pg->data + run_off, kind);
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
			r = fat_dev_write(fs, run_lba, run_len >> fs->dev_shift,
					  q->pg->data + run_off, kind);
		if (r != ST_OK) {
			/* Not on the device: stays queued (the next commit
			 * tries again; the frees that depend on it wait). */
			fs->dirw[keep++] = *q;
			if (ret == ST_OK)
				ret = r;
			continue;
		}
		pagecache_put(q->pg);
		fat_stream_put(d);
		q->pg = NULL;
		q->d = NULL;
	}
	fs->ndirw = keep;
	return ret;
}

/* Put `n' long-name units starting at unit `base' of `name' (`len' units)
 * into long-name entry `e': the name, then one 0x0000 terminator when the
 * name ends inside the entry, then 0xFFFF fill. */
static void fat_lfn_fill(uint8_t *e, const uint16_t *name, unsigned len,
			 unsigned base)
{
	for (unsigned u = 0; u < FAT_LFN_CHARS; u++) {
		unsigned i = base + u;
		uint16_t v = i < len ? name[i] : (i == len ? 0x0000 : 0xFFFF);
		unsigned off = u < 5 ? FAT_LFN_NAME1 + 2 * u :
			       u < 11 ? FAT_LFN_NAME2 + 2 * (u - 5) :
					FAT_LFN_NAME3 + 2 * (u - 11);

		fat_put16(e + off, v);
	}
}

/* Build an entry set: the long-name entries of `name' (`len' units) when
 * `need_lfn', then the short entry `sfn' -- which is the 32 bytes of
 * `proto' (a short entry whose fields are kept: times, attributes, cluster,
 * size) with the new name and case bits.  Returns the number of entries. */
unsigned fat_set_build(uint8_t *set, const uint16_t *name, unsigned len,
			      int need_lfn, const uint8_t *sfn, uint8_t lcase,
			      const uint8_t *proto)
{
	unsigned nl = need_lfn ? (len + FAT_LFN_CHARS - 1) / FAT_LFN_CHARS : 0;
	uint8_t chk = fat_lfn_checksum(sfn);
	uint8_t *se = set + nl * FAT_DENTRY_SIZE;

	for (unsigned k = 0; k < nl; k++) {
		uint8_t *e = set + k * FAT_DENTRY_SIZE;
		unsigned ord = nl - k;

		mm_memset(e, 0, FAT_DENTRY_SIZE);
		e[FAT_LFN_ORD] = (uint8_t)(ord | (k == 0 ? FAT_LFN_LAST : 0));
		e[FAT_LFN_ATTR] = FAT_ATTR_LFN;
		e[FAT_LFN_TYPE] = 0;
		e[FAT_LFN_CHKSUM] = chk;
		fat_lfn_fill(e, name, len, (ord - 1) * FAT_LFN_CHARS);
	}
	mm_memcpy(se, proto, FAT_DENTRY_SIZE);
	mm_memcpy(se, sfn, 11);
	se[FAT_DE_NTRES] = (uint8_t)((se[FAT_DE_NTRES] &
				      ~(FAT_CASE_LOWER_BASE | FAT_CASE_LOWER_EXT)) |
				     lcase);
	return nl + 1;
}

/* A fresh short entry prototype: attributes, the current time as creation,
 * modification and access time, first cluster and size. */
void fat_proto_new(fat_fs_t *fs, uint8_t *e, uint8_t attr,
			  uint32_t first, uint32_t size)
{
	uint16_t t, d;
	uint8_t cs;

	mm_memset(e, 0, FAT_DENTRY_SIZE);
	e[FAT_DE_ATTR] = attr;
	fat_now(fs, &t, &d, &cs);
	e[FAT_DE_CTIME_CS] = cs;
	fat_put16(e + FAT_DE_CTIME, t);
	fat_put16(e + FAT_DE_CDATE, d);
	fat_put16(e + FAT_DE_ADATE, d);
	fat_put16(e + FAT_DE_MTIME, t);
	fat_put16(e + FAT_DE_MDATE, d);
	fat_de_set_cluster(fs, e, first);
	fat_put32(e + FAT_DE_SIZE, size);
}

/* Grow directory `d' by `ncl' zeroed clusters.  A FAT directory's length is
 * its chain: the zeroed clusters reach the device before the link does
 * (fat_mc_flush syncs first).  Metadata lock exclusive, writes allowed. */
static int fat_dir_grow(fat_fs_t *fs, fat_stream_t *d, uint32_t ncl)
{
	struct fat_run *runs;
	unsigned nruns;
	uint64_t new_size;
	int r, pinned = 0;

	fat_assert_excl(fs);
	if (d->fixed_root)
		return ST_NOSPC; /* the FAT12/16 root cannot grow */
	new_size = d->size + ((uint64_t)ncl << fs->cs_shift);
	if (new_size > FAT_MAX_DIR_BYTES)
		return ST_NOSPC;
	r = fat_alloc(fs, fat_st_last(d), ncl, &runs, &nruns);
	if (r != ST_OK)
		return r;
	/* Zero them on the device BEFORE anything references them: stale
	 * bytes there would read as entries. */
	for (unsigned i = 0; i < nruns && r == ST_OK; i++)
		r = fat_dev_zero(fs, fat_cl_lba(fs, runs[i].start),
				 (uint64_t)runs[i].len
					 << (fs->cs_shift - fs->dev_shift));
	if (r == ST_OK)
		r = fat_stream_append(fs, d, runs, nruns, &pinned);
	if (r != ST_OK) {
		/* Not referenced (the append undid itself): straight back. */
		fat_runs_unalloc(fs, runs, nruns, ncl, pinned);
		kfree(runs);
		return r;
	}
	kfree(runs);
	d->size = new_size;
	/* The page that held the old end (when clusters are smaller than a
	 * page) keeps zeros past it, which is what the new cluster holds. */
	return ST_OK;
}

/* Entries per 512-byte unit: the smallest sector any device has, so the
 * largest piece of a directory one write is sure to store whole. */
#define FAT_SET_UNIT_ENTRIES 16u

/* May a set of `count' entries lie at slot `idx' of `d'?  So that a power
 * cut can never leave half a set on the device:
 *   - a set of up to 16 entries lies inside one 512-byte unit (one device
 *     sector write); a longer one starts on a unit;
 *   - no set crosses a page (one queued directory write);
 *   - the device sectors under it are consecutive (one device command).
 * Slots past the directory do not fit. */
int fat_slot_fits(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count)
{
	uint64_t first = idx << FAT_DENTRY_SHIFT;
	uint64_t last = ((idx + count) << FAT_DENTRY_SHIFT) - 1;
	unsigned long b0, b1, l0;

	if (idx + count > fat_dir_slots(d))
		return 0;
	if (count <= FAT_SET_UNIT_ENTRIES) {
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
	l0 = fat_st_block_lba(fs, d, b0);
	if (!l0)
		return 0;
	for (unsigned long b = b0 + 1; b <= b1; b++)
		if (fat_st_block_lba(fs, d, b) !=
		    l0 + ((b - b0) << (fs->bs_shift - fs->dev_shift)))
			return 0;
	return 1;
}

/* One scan for `need' free slots (free entries or past the end marker)
 * placed as fat_slot_fits() requires -- or, with `any', anywhere.  1 found
 * (*idx; *at_end when it lies at or past the end marker), 0 none, < 0
 * error. */
static int fat_dir_find_slots(fat_fs_t *fs, fat_stream_t *d, unsigned need,
			      int any, uint64_t *idx, int *at_end)
{
	fat_diter_t it;
	uint64_t i = 0, slots = fat_dir_slots(d), run_start = 0, run = 0;
	int r = 0;

	*at_end = 0;
	fat_diter_init(&it, fs, d);
	while (i < slots) {
		const uint8_t *e = fat_diter_get(&it, i);

		if (!e) {
			r = it.err ? it.err : ST_IO;
			break;
		}
		if (e[0] == FAT_NAME_END) {
			/* Everything from here on is free. */
			if (!run)
				run_start = i;
			for (uint64_t c = run_start; c + need <= slots; c++)
				if (any || fat_slot_fits(fs, d, c, need)) {
					*idx = c;
					*at_end = c + need > i;
					r = 1;
					break;
				}
			break;
		}
		if (e[0] == FAT_NAME_DELETED) {
			if (!run)
				run_start = i;
			run++;
			if (run >= need) {
				uint64_t c = i + 1 - need;

				if (any || fat_slot_fits(fs, d, c, need)) {
					*idx = c;
					r = 1;
					break;
				}
			}
			i++;
			continue;
		}
		run = 0;
		i++;
	}
	fat_diter_done(&it);
	return r;
}

/* A set placed past the end marker, on the boundary its placement asks for,
 * would sit behind end markers (0x00) -- and a reader stops at the first of
 * them.  The slots between the last entry and the set become free entries
 * (0xE5) before the set is written; they are queued first, so they reach
 * the device first (a stop in between leaves a few free entries, nothing
 * else).  And the slot after a set that took the end marker's place must
 * end the directory again: whatever a careless writer left there behind
 * the old marker would otherwise come to light.  Metadata lock exclusive.
 * The entries come from a small buffer of its own: fat_dir_write() may run
 * a commit (queue full), which uses the commit's scratch. */
#define FAT_GAP_CHUNK 8u
static int fat_dir_fill_gap(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			    unsigned need, int at_end)
{
	uint8_t gap[FAT_GAP_CHUNK * FAT_DENTRY_SIZE];
	fat_diter_t it;
	uint64_t from = idx, after = idx + need;
	int r = ST_OK, seal = 0;

	fat_assert_excl(fs);
	fat_diter_init(&it, fs, d);
	while (from > 0) {
		const uint8_t *e = fat_diter_get(&it, from - 1);

		if (!e) {
			r = it.err ? it.err : ST_IO;
			break;
		}
		if (e[0] != FAT_NAME_END)
			break;
		from--;
	}
	if (r == ST_OK && at_end && after < fat_dir_slots(d)) {
		const uint8_t *e = fat_diter_get(&it, after);

		if (!e)
			r = it.err ? it.err : ST_IO;
		else if (e[0] != FAT_NAME_END)
			seal = 1;
	}
	fat_diter_done(&it);
	if (r != ST_OK)
		return r;
	while (from < idx) {
		unsigned n = (unsigned)fat_min64(idx - from, FAT_GAP_CHUNK);

		mm_memset(gap, 0, n * FAT_DENTRY_SIZE);
		for (unsigned k = 0; k < n; k++)
			gap[k * FAT_DENTRY_SIZE] = FAT_NAME_DELETED;
		r = fat_dir_write(fs, d, from, n, gap);
		if (r != ST_OK)
			return r;
		from += n;
	}
	if (seal) {
		mm_memset(gap, 0, FAT_DENTRY_SIZE);
		r = fat_dir_write(fs, d, after, 1, gap);
	}
	return r;
}

/* Find `need' free slots for a new entry set, placed so that it is stored
 * in one piece (fat_slot_fits), growing the directory when there is no such
 * place.  Only where even fresh clusters cannot give one (the FAT12/16 root,
 * a full volume, a set longer than a sector whose new clusters are not
 * adjacent) does any free run do.  Metadata lock exclusive, writes
 * allowed. */
int fat_dir_alloc_slots(fat_fs_t *fs, fat_stream_t *d, unsigned need,
			       uint64_t *idx)
{
	int r, at_end;

	fat_assert_excl(fs);
	for (int round = 0; round < 3; round++) {
		uint64_t bytes;
		uint32_t ncl;

		r = fat_dir_find_slots(fs, d, need, 0, idx, &at_end);
		if (r != 0)
			return r < 0 ? r :
				       fat_dir_fill_gap(fs, d, *idx, need, at_end);
		/* Room for the set plus a unit of alignment slack -- and at
		 * least a page or a cluster: a directory grows seldom, in
		 * big steps. */
		bytes = (uint64_t)(need + FAT_SET_UNIT_ENTRIES)
			<< FAT_DENTRY_SHIFT;
		if (bytes < PAGE_SIZE)
			bytes = PAGE_SIZE;
		ncl = (uint32_t)((bytes + fs->cs - 1) >> fs->cs_shift);
		r = fat_dir_grow(fs, d, ncl);
		if (r == ST_NOSPC && ncl > 1)
			r = fat_dir_grow(fs, d, 1); /* the last few clusters */
		if (r == ST_NOSPC)
			break; /* full (volume, root region or 2 MB) */
		if (r != ST_OK)
			return r;
	}
	r = fat_dir_find_slots(fs, d, need, 1, idx, &at_end);
	if (r > 0)
		return fat_dir_fill_gap(fs, d, *idx, need, at_end);
	return r < 0 ? r : ST_NOSPC;
}

/* Is the directory empty (no entry naming a file or directory)?  1 / 0 /
 * error. */
int fat_dir_empty(fat_fs_t *fs, fat_stream_t *d)
{
	fat_diter_t it;
	uint64_t slots = fat_dir_slots(d);
	int r = 1;

	fat_diter_init(&it, fs, d);
	for (uint64_t i = 0; i < slots; i++) {
		const uint8_t *e = fat_diter_get(&it, i);

		if (!e) {
			r = it.err ? it.err : 1;
			break;
		}
		if (e[0] == FAT_NAME_END)
			break;
		if (fat_de_is_object(e)) {
			r = 0;
			break;
		}
	}
	fat_diter_done(&it);
	return r;
}

/* Write a stream's in-memory state into its short entry (queued).  The
 * entry is re-read and must still carry the stream's short name: anything
 * else means the directory changed behind our back (corruption).  Metadata
 * lock exclusive. */
int fat_stream_store(fat_fs_t *fs, fat_stream_t *st)
{
	fat_diter_t it;
	uint8_t *e = fs->wb_set;
	int r;

	fat_assert_excl(fs);
	if (st->is_root || st->orphan || !st->parent)
		return ST_OK;
	fat_diter_init(&it, fs, st->parent);
	r = fat_diter_copy(&it, st->entry_index, 1, e);
	fat_diter_done(&it);
	if (r != ST_OK)
		return r == ST_EOF ? ST_IO : r;
	for (unsigned i = 0; i < 11; i++)
		if (e[i] != st->name83[i])
			return fat_corrupt(fs, st->entry_index,
					   "short entry of a file changed under it (slot %u)",
					   st->entry_index);
	e[FAT_DE_ATTR] = st->attr;
	e[FAT_DE_NTRES] = st->ntres;
	e[FAT_DE_CTIME_CS] = st->ctime_cs;
	fat_put16(e + FAT_DE_CTIME, st->ctime);
	fat_put16(e + FAT_DE_CDATE, st->cdate);
	fat_put16(e + FAT_DE_ADATE, st->adate);
	fat_put16(e + FAT_DE_MTIME, st->mtime);
	fat_put16(e + FAT_DE_MDATE, st->mdate);
	fat_de_set_cluster(fs, e, st->first_cluster);
	fat_put32(e + FAT_DE_SIZE, st->is_dir ? 0 : (uint32_t)st->size);
	return fat_dir_write(fs, st->parent, st->entry_index, 1, e);
}

/* ===========================================================================
 * 17. Commit, arming, clean marking
 *
 * Without a journal, the order of writes is what keeps the volume
 * consistent should the device stop at any moment:
 *
 *   1. file data of streams whose entry changes (a size grows only after
 *      the data it covers -- zeros of a gap included -- is stored);
 *   2. a barrier, then the FAT: allocations (end marks and links of new
 *      clusters, zeroed directory clusters already on the device) --
 *      ALLOCATE BEFORE REFERENCE;
 *   3. a device sync when anything was written since the last one;
 *   4. the queued directory entries (new names, sizes, deletions);
 *   5. only now, with the entries that dropped them on the device (another
 *      sync), the frees: end marks of truncated chains, a sync, then the
 *      freed entries set to 0 -- DEREFERENCE BEFORE FREE; then, with the
 *      freed entries on the device too, the discards.
 *
 * A stop between any two steps leaves at worst clusters in use that nothing
 * references (fsck reports them as lost; nothing is wrong with any file) --
 * and, while a chain grows, a chain longer than its file (fat32.h).  Freed
 * clusters cannot be reused before step 5 (their entries keep their
 * values), so no new data ever lands in a cluster an old entry on the
 * device still names.
 *
 * Arming: the first real write of a mount marks the volume dirty -- the
 * clean bit of FAT[1] cleared (FAT16/32), the boot sector's state byte
 * dirty -- and syncs, before anything else is written.  A clean sync /
 * unmount / read-only remount stores everything, the FSInfo sector, syncs,
 * sets both marks back and syncs again.  A mount that only reads writes
 * nothing at all.
 * =========================================================================== */

void fat_stream_mark_dirty(fat_fs_t *fs, fat_stream_t *st)
{
	fat_assert_excl(fs);
	if (st->is_root)
		return;
	st->meta_dirty = 1;
	if (!st->on_dirty_list) {
		fat_stream_hold(st);
		st->on_dirty_list = 1;
		st->dnext = fs->dirty_head;
		fs->dirty_head = st;
	}
}

/* ---- readdir ---------------------------------------------------------------- */

/* One record (laid out as the other filesystems do: name at byte 19, the
 * record rounded to 8), into the KERNEL buffer readdir assembles under the
 * lock.  0 when it does not fit. */
static unsigned fat_put_dirent(void *buf, unsigned cap, unsigned off,
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
#define FAT_READDIR_MAX (64UL << 10)

/* readdir: the records are assembled in a kernel buffer under the metadata
 * lock (shared) and copied to the caller after it is dropped. */
long fat_readdir_op(vfs_file_t *f, void *ubuf, long ubytes)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	fat_fs_t *fs;
	fat_stream_t *d;
	fat_ctx_t *ctx;
	fat_diter_t it;
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
	bytes = ubytes > (long)FAT_READDIR_MAX ? (long)FAT_READDIR_MAX : ubytes;
	buf = kalloc((size_t)bytes);
	if (!buf)
		return ST_NOMEM;
	ctx = fat_ctx_new();
	if (!ctx) {
		kfree(buf);
		return ST_NOMEM;
	}
	saved_pos = h->dirpos;
	fat_meta_rlock(fs);
	if (h->dirpos == 0) {
		rl = fat_put_dirent(buf, (unsigned)bytes, off, ".", 1,
				    fat_st_ino(fs, d), DT_DIR, 1);
		if (!rl) {
			ret = ST_INVALID;
			goto out;
		}
		off += rl;
		h->dirpos = 1;
	}
	if (h->dirpos == 1) {
		rl = fat_put_dirent(buf, (unsigned)bytes, off, "..", 2,
				    h->parent_ino, DT_DIR, 2);
		if (!rl) {
			ret = off;
			goto out;
		}
		off += rl;
		h->dirpos = FAT_DIRPOS_FIRST;
	}
	slots = fat_dir_slots(d);
	fat_diter_init(&it, fs, d);
	for (uint64_t i = h->dirpos - FAT_DIRPOS_FIRST; i < slots;) {
		const uint8_t *e = fat_diter_get(&it, i);
		fat_dentry_info_t di;
		uint64_t ino;
		unsigned nu;
		int r, nl;

		if (!e) {
			if (it.err && off == 0) {
				fat_diter_done(&it);
				ret = it.err;
				goto out;
			}
			break;
		}
		if (e[0] == FAT_NAME_END) {
			h->dirpos = slots + FAT_DIRPOS_FIRST;
			break;
		}
		if (!fat_de_is_object(e)) {
			i++;
			h->dirpos = i + FAT_DIRPOS_FIRST;
			continue;
		}
		r = fat_set_read(&it, i, &di, ctx->name);
		if (r != ST_OK) {
			if (r == ST_IO)
				WARN_RATELIMIT(1, "fat: %s: impossible entry at slot %llu skipped",
					       fat_name_of(fs),
					       (unsigned long long)i);
			if (r != ST_IO && r != ST_NOT_FOUND) {
				if (off == 0) {
					fat_diter_done(&it);
					ret = r;
					goto out;
				}
				break;
			}
			i++;
			h->dirpos = i + FAT_DIRPOS_FIRST;
			continue;
		}
		nu = fat_set_name(fs, &di, ctx->name, ctx->name2);
		nl = nu ? fat_utf16_to_ext(fs, ctx->name2, nu, ctx->u8,
					   sizeof(ctx->u8)) :
			  -1;
		if (nl <= 0) {
			i++;
			h->dirpos = i + FAT_DIRPOS_FIRST;
			continue;
		}
		{
			fat_stream_t *s = fat_stream_find(
				fs, FAT_KEY(fat_dir_keycl(d), i));

			if (s) {
				ino = fat_st_ino(fs, s);
				fat_stream_put(s);
			} else {
				ino = fat_ino(di.first_cluster, fat_dir_keycl(d),
					      (uint32_t)i);
			}
		}
		rl = fat_put_dirent(buf, (unsigned)bytes, off, ctx->u8,
				    (unsigned)nl, ino,
				    (di.attr & FAT_ATTR_DIR) ? DT_DIR : DT_REG,
				    i + 1 + FAT_DIRPOS_FIRST);
		if (!rl) {
			if (off == 0) {
				fat_diter_done(&it);
				ret = ST_INVALID;
				goto out;
			}
			break;
		}
		off += rl;
		i++;
		h->dirpos = i + FAT_DIRPOS_FIRST;
	}
	fat_diter_done(&it);
	ret = off;
out:
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
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
