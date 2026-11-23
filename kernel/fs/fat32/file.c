/* LikeOS - FAT filesystem driver: cluster chains of streams, the data read and
 * write paths, open, close, read, write, seek, truncate and attribute
 * changes
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* Give runs that nothing references back (an allocation whose append
 * failed). */
void fat_runs_unalloc(fat_fs_t *fs, const struct fat_run *runs,
			     unsigned nruns, uint32_t total, int pinned)
{
	if (pinned)
		return; /* lost clusters: see fat_stream_append() */
	for (unsigned i = 0; i < nruns; i++)
		fat_unmark_run(fs, runs[i].start, runs[i].len);
	fat_free_count_add(fs, total);
}

/* Grow a stream's allocation to `ncl' clusters: all or nothing.  Metadata
 * lock exclusive, writes allowed. */
static int fat_stream_grow(fat_fs_t *fs, fat_stream_t *st, uint32_t ncl)
{
	struct fat_run *runs;
	unsigned nruns;
	uint32_t add;
	int r, pinned = 0;

	fat_assert_excl(fs);
	if (ncl <= st->nclusters)
		return ST_OK;
	if (st->chain_bad)
		return ST_IO;
	add = ncl - st->nclusters;
	r = fat_alloc(fs, fat_st_last(st), add, &runs, &nruns);
	if (r != ST_OK)
		return r;
	r = fat_stream_append(fs, st, runs, nruns, &pinned);
	if (r != ST_OK)
		fat_runs_unalloc(fs, runs, nruns, add, pinned);
	kfree(runs);
	return r;
}

/* Cut a stream's allocation to `ncl' clusters: the in-memory state changes
 * now, the clusters are freed (deferred) and the new last cluster gets its
 * end mark at the commit (after the entry with the shorter size is stored).
 * Metadata lock exclusive. */
static int fat_stream_shrink(fat_fs_t *fs, fat_stream_t *st, uint32_t ncl)
{
	fat_extent_t *tail = NULL;
	uint32_t ntail = 0, cap = 0, newlast;
	int r = ST_OK;

	fat_assert_excl(fs);
	if (ncl >= st->nclusters)
		return ST_OK;
	/* The part that goes. */
	for (uint32_t i = 0; i < st->next; i++) {
		fat_extent_t *e = &st->ext[i];
		uint64_t eend = (uint64_t)e->fidx + e->len;

		if (eend <= ncl)
			continue;
		uint32_t skip = e->fidx < ncl ? ncl - e->fidx : 0;

		r = fat_ext_append(&tail, &ntail, &cap, e->fidx + skip,
				   e->disk + skip, e->len - skip);
		if (r != ST_OK) {
			if (tail)
				kfree(tail);
			return r;
		}
	}
	/* The part that stays. */
	{
		uint32_t keep = 0;

		for (uint32_t i = 0; i < st->next; i++) {
			fat_extent_t *e = &st->ext[i];

			if (e->fidx >= ncl)
				break;
			if ((uint64_t)e->fidx + e->len > ncl)
				e->len = ncl - e->fidx;
			keep = i + 1;
		}
		st->next = keep;
	}
	st->nclusters = ncl;
	newlast = fat_st_last(st);
	if (ncl == 0)
		st->first_cluster = 0;
	else
		r = fat_fat_defer(fs, newlast, fs->eoc_val);
	if (r == ST_OK)
		r = fat_free_extents(fs, tail, ntail);
	if (tail)
		kfree(tail);
	return r;
}

/* ===========================================================================
 * 19. The data paths
 *
 * FAT has no "valid data length": every byte below a file's size is what
 * its clusters hold.  So a file that grows -- a write past its end, a
 * truncate upwards -- gets zeros stored for everything between the old end
 * and the new data, before the entry with the new size (the commit stores
 * data first).  Pages cached for the range are zeroed and dirtied; the rest
 * of the range is zeroed on the device directly (whole pages past the old
 * end that nobody cached hold nothing worth reading).  A truncate downwards
 * zeroes the tail of the cached page that holds the new end, so that bytes
 * past the end are never shown again should the file grow.
 * =========================================================================== */

/* A page of the stream to write into, held (release with
 * pagecache_write_end / _abort).  `full': every byte will be replaced. */
static pc_page_t *fat_page_write(fat_fs_t *fs, fat_stream_t *st,
				 unsigned long pidx, int full, uint64_t old_size)
{
	unsigned long chain = fat_chain_of(st);
	pc_page_t *pg;

	pg = pagecache_get_for_write(chain, pidx, (unsigned long)old_size,
				     &fs->sb, chain, full);
	if (!pg && st->data_dirty) {
		/* Out of pages: this stream's own dirty ones are what the cache
		 * cannot give back.  Store them (they become clean and
		 * reclaimable) and try once more. */
		pagecache_flush_file(&fs->sb, chain);
		pagecache_shrink(64, 0);
		pg = pagecache_get_for_write(chain, pidx,
					     (unsigned long)old_size, &fs->sb,
					     chain, full);
	}
	return pg;
}

/* A pending run of device sectors to zero (fat_zero_range). */
struct fat_zrun {
	uint64_t lba;
	uint64_t count;
};

static int fat_zrun_add(fat_fs_t *fs, struct fat_zrun *z, uint64_t lba,
			uint64_t count)
{
	int r;

	if (z->count && z->lba + z->count == lba) {
		z->count += count;
		return ST_OK;
	}
	if (z->count) {
		r = fat_dev_zero(fs, z->lba, z->count);
		if (r != ST_OK)
			return r;
	}
	z->lba = lba;
	z->count = count;
	return ST_OK;
}

/* Make bytes [from, to) of `st' read as zeros, in the cache and on the
 * device (stored before the entry that makes them part of the file).
 * `cur_size' is the size the file has now: pages wholly at or past it that
 * are not cached are zeroed on the device; every other page goes through
 * the cache.  The clusters under the range exist.  Metadata lock and the
 * stream's lock exclusive, writes allowed. */
static int fat_zero_range(fat_fs_t *fs, fat_stream_t *st, uint64_t from,
			  uint64_t to, uint64_t cur_size)
{
	unsigned long chain = fat_chain_of(st);
	struct fat_zrun z = { 0, 0 };
	unsigned n = 0;
	int r = ST_OK;

	fat_assert_excl(fs);
	while (from < to && r == ST_OK) {
		unsigned long pidx = (unsigned long)(from >> PAGE_SHIFT);
		uint64_t pstart = (uint64_t)pidx << PAGE_SHIFT;
		unsigned off = (unsigned)(from - pstart);
		uint64_t pend = fat_min64(to, pstart + PAGE_SIZE);
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, chain, pidx);

		if (!pg && pstart >= cur_size) {
			/* Nothing of the file there, nobody's copy: zero the
			 * page's blocks on the device. */
			for (uint64_t b = pstart; b < pstart + PAGE_SIZE;
			     b += fs->bs) {
				unsigned long lba = fat_st_block_lba(
					fs, st, (unsigned long)(b >> fs->bs_shift));

				if (!lba)
					continue;
				r = fat_zrun_add(fs, &z, lba,
						 fs->bs >> fs->dev_shift);
				if (r != ST_OK)
					break;
			}
			from = pstart + PAGE_SIZE;
			continue;
		}
		if (pg) {
			mm_memset(pg->data + off, 0, (size_t)(pend - from));
			pagecache_mark_dirty(pg);
			pagecache_put(pg);
		} else {
			pg = fat_page_write(fs, st, pidx, 0, cur_size);
			if (!pg) {
				r = ST_NOMEM;
				break;
			}
			mm_memset(pg->data + off, 0, (size_t)(pend - from));
			pagecache_write_end(pg);
		}
		st->data_dirty = 1;
		fat_note_dirty(st, pidx);
		if (++n % FAT_BALANCE_EVERY == 0)
			pagecache_balance_dirty(&fs->sb);
		from = pend;
	}
	if (r == ST_OK && z.count)
		r = fat_dev_zero(fs, z.lba, z.count);
	return r;
}

/* Pages a read takes under the locks before it copies them out. */
#define FAT_RBATCH 16

/* Read `bytes' at `pos' into `buf' (user memory, or a kernel page for demand
 * paging).  User memory is never touched with a lock held: under the stream
 * lock and the metadata lock (both shared) up to FAT_RBATCH pages are looked
 * up / read and HELD, the locks are dropped, then the bytes are copied out
 * of the held pages (a page truncated meanwhile is detached but keeps its
 * frame until put), and the next batch follows.  `ra' is the readahead
 * state to use, or NULL. */
static long fat_read_common(fat_fs_t *fs, fat_stream_t *st, void *buf,
			    long bytes, uint64_t pos, pc_readahead_t *ra)
{
	unsigned long chain = fat_chain_of(st);
	unsigned long copied = 0;
	long err = 0;

	if (st->is_dir)
		return ST_ISDIR;
	if (bytes < 0)
		return ST_INVALID;
	if (st->chain_bad)
		return ST_IO;
	while ((unsigned long)bytes > copied && !err) {
		pc_page_t *pages[FAT_RBATCH];
		unsigned offs[FAT_RBATCH];
		unsigned long lens[FAT_RBATCH];
		unsigned n = 0;
		uint64_t size, at = pos + copied;
		unsigned long want = (unsigned long)bytes - copied;

		fat_slock_shared(fs, st->handle);
		fat_meta_rlock(fs);
		size = st->size;
		if (at < size && want > size - at)
			want = (unsigned long)(size - at);
		while (at < size && want && n < FAT_RBATCH) {
			unsigned long pidx = (unsigned long)(at >> PAGE_SHIFT);
			unsigned off = (unsigned)(at & (PAGE_SIZE - 1));
			unsigned long chunk = PAGE_SIZE - off;
			pc_page_t *pg;

			if (chunk > want)
				chunk = want;
			pg = pagecache_get_held(chain, pidx, (unsigned long)size,
						&fs->sb, chain);
			if (!pg || !pg->data) {
				if (pg)
					pagecache_put(pg);
				err = ST_IO;
				break;
			}
			if (ra)
				pagecache_readahead(ra, chain, pidx,
						    (unsigned long)size, &fs->sb,
						    chain);
			pages[n] = pg;
			offs[n] = off;
			lens[n] = chunk;
			n++;
			at += chunk;
			want -= chunk;
		}
		fat_meta_runlock(fs);
		fat_sunlock_shared(fs, st->handle);
		if (n == 0)
			break;
		/* No lock held: a fault here strands nothing. */
		for (unsigned k = 0; k < n; k++) {
			smap_disable();
			mm_memcpy((uint8_t *)buf + copied, pages[k]->data + offs[k],
				  lens[k]);
			smap_enable();
			copied += lens[k];
			pagecache_put(pages[k]);
		}
	}
	if (copied)
		return (long)copied;
	return err ? err : 0;
}

/* The largest size a file may have: 4 GiB - 1 (the size field), and no more
 * than the volume could hold. */
static uint64_t fat_max_file_size(fat_fs_t *fs)
{
	uint64_t m = (uint64_t)fs->cluster_count << fs->cs_shift;

	return m < FAT_MAX_FILE_SIZE ? m : FAT_MAX_FILE_SIZE;
}

/* A file's data changed: new modification time, archive bit. */
void fat_touch_mtime(fat_fs_t *fs, fat_stream_t *st)
{
	if (st->is_root)
		return;
	fat_now(fs, &st->mtime, &st->mdate, NULL);
	if (!st->is_dir)
		st->attr |= FAT_ATTR_ARCHIVE;
}

/* Write `bytes' from the KERNEL buffer `buf' at *ppos (the size for
 * O_APPEND).  The stream's lock exclusive and the metadata lock exclusive
 * held. */
static long fat_write_impl(fat_fs_t *fs, fat_stream_t *st, const void *buf,
			   long bytes, uint64_t *ppos, int append)
{
	uint64_t pos, end, old_size, maxsz = fat_max_file_size(fs);
	unsigned long done = 0;
	unsigned n = 0;
	int r;

	fat_assert_excl(fs);
	if (st->is_dir)
		return ST_ISDIR;
	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	if (st->chain_bad)
		return ST_IO;
	r = fat_writable(fs);
	if (r != ST_OK)
		return r;
	r = fat_may_write(fs);
	if (r != ST_OK)
		return r;
	pos = append ? st->size : *ppos;
	if (!fat_add_ok(pos, (uint64_t)bytes, &end))
		return ST_INVALID;
	if (end > maxsz) {
		if (pos >= maxsz)
			return ST_NOSPC;
		end = maxsz;
	}
	/* Clusters first (allocate before reference); a full volume gets a
	 * short write if any byte fits. */
	{
		uint64_t need = (end + fs->cs - 1) >> fs->cs_shift;

		if (need > st->nclusters) {
			r = fat_stream_grow(fs, st, (uint32_t)need);
			if (r == ST_NOSPC) {
				uint64_t have;

				fat_count_free(fs);
				have = (uint64_t)st->nclusters +
				       (uint64_t)(fs->free_clusters > 0 ?
							  fs->free_clusters :
							  0);
				if ((have << fs->cs_shift) <= pos)
					return ST_NOSPC;
				end = fat_min64(end, have << fs->cs_shift);
				r = fat_stream_grow(fs, st, (uint32_t)have);
			}
			if (r != ST_OK)
				return r;
			fat_stream_mark_dirty(fs, st);
		}
	}
	old_size = st->size;
	/* A write past the end: the gap reads as zeros, stored first. */
	if (pos > old_size) {
		r = fat_zero_range(fs, st, old_size, pos, old_size);
		if (r != ST_OK)
			goto fail;
	}
	while (pos + done < end) {
		uint64_t at = pos + done;
		unsigned long pidx = (unsigned long)(at >> PAGE_SHIFT);
		unsigned off = (unsigned)(at & (PAGE_SIZE - 1));
		unsigned long chunk = PAGE_SIZE - off;
		pc_page_t *pg;
		int full;

		if (chunk > end - at)
			chunk = (unsigned long)(end - at);
		full = (off == 0 && chunk == PAGE_SIZE);
		pg = fat_page_write(fs, st, pidx, full, old_size);
		if (!pg) {
			r = ST_NOMEM;
			goto fail;
		}
		mm_memcpy(pg->data + off, (const uint8_t *)buf + done, chunk);
		pagecache_write_end(pg);
		st->data_dirty = 1;
		fat_note_dirty(st, pidx);
		done += chunk;
		if (pos + done > st->size)
			st->size = pos + done;
		if (++n % FAT_BALANCE_EVERY == 0)
			pagecache_balance_dirty(&fs->sb);
	}
	fat_touch_mtime(fs, st);
	fat_stream_mark_dirty(fs, st);
	*ppos = pos + done;
	pagecache_balance_dirty(&fs->sb);
	return (long)done;
fail:
	/* What was copied stays; the allocation shrinks back to cover exactly
	 * the size (clusters past it would be lost otherwise). */
	if (st->size < old_size)
		st->size = old_size;
	fat_stream_shrink(fs, st,
			  (uint32_t)((st->size + fs->cs - 1) >> fs->cs_shift));
	if (done)
		fat_touch_mtime(fs, st);
	fat_stream_mark_dirty(fs, st);
	if (done) {
		*ppos = pos + done;
		return (long)done;
	}
	return r;
}

/* Change a file's size.  Shrinking drops the pages and (deferred) the
 * clusters past the new end; growing allocates clusters and stores zeros
 * for the new range.  The stream's lock and the metadata lock exclusive,
 * writes allowed. */
static int fat_truncate_impl(fat_fs_t *fs, fat_stream_t *st, uint64_t nsize)
{
	unsigned long chain = fat_chain_of(st);
	int r = ST_OK;

	fat_assert_excl(fs);
	if (st->is_dir)
		return ST_ISDIR;
	if (nsize > fat_max_file_size(fs))
		return nsize > FAT_MAX_FILE_SIZE ? ST_INVALID : ST_NOSPC;
	if (st->chain_bad)
		return ST_IO;
	if (nsize < st->size) {
		uint32_t ncl = (uint32_t)((nsize + fs->cs - 1) >> fs->cs_shift);

		/* Pages past the end go (dirty ones too: their bytes are cut
		 * off), and the tail of the page holding the new end must read
		 * as zeros should the file grow again. */
		pagecache_invalidate_range(&fs->sb, chain, (unsigned long)nsize);
		if (nsize & (PAGE_SIZE - 1)) {
			pc_page_t *pg = pagecache_lookup_held(
				&fs->sb, chain, (unsigned long)(nsize >> PAGE_SHIFT));

			if (pg) {
				unsigned off = (unsigned)(nsize & (PAGE_SIZE - 1));

				mm_memset(pg->data + off, 0, PAGE_SIZE - off);
				pagecache_put(pg);
			}
		}
		st->size = nsize;
		fat_stream_mark_dirty(fs, st);
		r = fat_stream_shrink(fs, st, ncl);
	} else if (nsize == st->size) {
		/* No change, no new times (the size did not change). */
		return ST_OK;
	} else {
		uint32_t ncl = (uint32_t)((nsize + fs->cs - 1) >> fs->cs_shift);
		uint64_t old = st->size;

		r = fat_stream_grow(fs, st, ncl);
		if (r != ST_OK)
			return r;
		fat_stream_mark_dirty(fs, st);
		r = fat_zero_range(fs, st, old, nsize, old);
		if (r != ST_OK) {
			/* Back to the old size: the clusters go again. */
			fat_stream_shrink(fs, st,
					  (uint32_t)((old + fs->cs - 1) >>
						     fs->cs_shift));
			return r;
		}
		st->size = nsize;
	}
	fat_touch_mtime(fs, st);
	fat_stream_mark_dirty(fs, st);
	return r;
}

/* ===========================================================================
 * 20. VFS operations
 *
 * Locking per exfat.h: read-only operations take the metadata lock shared;
 * mutations take it exclusive, after the stream lock of any file whose data
 * they free (unlink, rename over a file, O_TRUNC, truncate, write).  Every
 * mutation arms the mount first and ends with a commit, so each namespace
 * change is on the device (in order) when it returns.
 * =========================================================================== */

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

/* ---- open / close ---------------------------------------------------------- */

/* Make a handle for a held stream (that hold passes to the handle). */
static int fat_handle_new(fat_fs_t *fs, fat_stream_t *st, int flags,
			  vfs_file_t **out)
{
	fat_file_t *h = (fat_file_t *)kcalloc(1, sizeof(*h));
	unsigned long chain = fat_chain_of(st);

	if (!h) {
		fat_stream_put(st);
		return ST_NOMEM;
	}
	h->vfs.ops = &fat_vfs_ops;
	h->vfs.fs_private = h;
	h->fs = fs;
	h->st = st;
	h->open_flags = flags;
	h->pos = (flags & O_APPEND) ? (unsigned long)st->size : 0;
	if (st->is_dir)
		h->parent_ino = st->parent ? fat_st_ino(fs, st->parent) :
					     fat_st_ino(fs, st);
	__atomic_add_fetch(&st->opens, 1, __ATOMIC_SEQ_CST);
	h->ic = icache_get(&fs->sb, chain, (unsigned long)st->size, st->attr,
			   st->parent ? fat_chain_of(st->parent) : chain, 0, 0,
			   st->mtime, st->mdate);
	*out = &h->vfs;
	return ST_OK;
}

/* Build and place the entry set of a new object named ctx->name (`n'
 * units, folded ctx->up, key ctx->key) in w->dir, its short entry from
 * `proto'.  *di describes the placed set.  Metadata lock exclusive, writes
 * allowed. */
int fat_place_set(fat_fs_t *fs, fat_ctx_t *ctx, struct fat_walk *w,
			 unsigned n, const uint8_t *proto,
			 fat_dentry_info_t *di)
{
	uint8_t lcase;
	uint64_t idx;
	unsigned count;
	int r, need_lfn;

	r = fat_sfn_make(fs, ctx, w->dir, ctx->name, n, &lcase, -1);
	if (r < 0)
		return r;
	need_lfn = (r == 0);
	count = fat_set_build(ctx->set, ctx->name, n, need_lfn, ctx->sfn, lcase,
			      proto);
	r = fat_dir_alloc_slots(fs, w->dir, count, &idx);
	if (r != ST_OK)
		return r;
	r = fat_dir_write(fs, w->dir, idx, count, ctx->set);
	if (r != ST_OK)
		return r;
	fat_di_from_sfn(fs, ctx->set + (count - 1) * FAT_DENTRY_SIZE,
			(uint32_t)(idx + count - 1), di);
	di->lfn_count = (uint8_t)(count - 1);
	di->lname_len = need_lfn ? (uint16_t)n : 0;
	/* Negative entries of both names go; the long name is remembered. */
	fat_dc_forget_set(fs, w->dir, di, ctx->name, ctx->tmpk, ctx->tmpu);
	fat_dc_forget(fs, w->dir, ctx->key);
	fat_dc_remember(fs, w->dir, ctx->key, di);
	return ST_OK;
}

/* Create the file of the walk's last component.  Metadata lock exclusive.
 * The new stream (held) in *out. */
static int fat_create_locked(fat_fs_t *fs, fat_ctx_t *ctx, struct fat_walk *w,
			     unsigned int mode, fat_stream_t **out)
{
	fat_dentry_info_t di;
	uint8_t proto[FAT_DENTRY_SIZE];
	int r, n;
	uint8_t attr = FAT_ATTR_ARCHIVE;

	fat_assert_excl(fs);
	if (!w->last || w->trailing_slash)
		return w->is_root ? ST_EXISTS : ST_INVALID;
	r = fat_writable(fs);
	if (r != ST_OK)
		return r;
	n = fat_name_prepare(fs, ctx, w->last, w->last_len, 1);
	if (n < 0)
		return n;
	r = fat_may_write(fs);
	if (r != ST_OK)
		return r;
	if (!(mode & 0222))
		attr |= FAT_ATTR_RO;
	fat_proto_new(fs, proto, attr, 0, 0);
	r = fat_place_set(fs, ctx, w, (unsigned)n, proto, &di);
	if (r != ST_OK)
		return r;
	fat_touch_mtime(fs, w->dir);
	fat_stream_mark_dirty(fs, w->dir);
	r = fat_commit(fs);
	if (r != ST_OK)
		return r;
	{
		int err;

		*out = fat_stream_get(fs, w->dir, &di, &err);
		if (!*out)
			return err;
	}
	return ST_OK;
}

/* Resolve `path', take its stream's lock exclusive and then the metadata
 * lock exclusive, and check the path still leads to the same stream.  1 with
 * both locks held (*stp held), 0 with none (the path does not lead to a
 * stream right now).  `quiet': a file whose chain is corrupt is taken
 * anyway, its stream without allocation (unlink). */
int fat_fence(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
		     fat_stream_t **stp, int quiet)
{
	for (int tries = 0; tries < 4; tries++) {
		struct fat_walk w;
		fat_stream_t *st, *st2 = NULL;
		int r, err;

		fat_meta_rlock(fs);
		r = fat_walk(fs, ctx, path, &w);
		if (r == ST_OK && !w.is_root && !w.st) {
			if (!w.found) {
				fat_walk_release(&w);
				r = ST_NOT_FOUND;
			} else {
				w.st = fat_stream_get_ex(fs, w.dir, &w.di, quiet,
							 &err);
				if (!w.st) {
					fat_walk_release(&w);
					r = err;
				}
			}
		}
		if (r != ST_OK) {
			fat_meta_runlock(fs);
			return 0;
		}
		st = w.st;
		w.st = NULL;
		fat_walk_release(&w);
		fat_meta_runlock(fs);

		fat_slock_excl(fs, st->handle);
		fat_meta_wlock(fs);
		r = fat_walk(fs, ctx, path, &w);
		if (r == ST_OK) {
			if (w.is_root || w.st)
				st2 = w.st;
			else if (w.found)
				st2 = fat_stream_find(
					fs, FAT_KEY(fat_dir_keycl(w.dir),
						    w.di.index));
			if (st2 && st2 != w.st)
				fat_stream_put(st2); /* compared only */
			fat_walk_release(&w);
		}
		if (st2 == st) {
			*stp = st;
			return 1;
		}
		fat_meta_wunlock(fs);
		fat_sunlock_excl(fs, st->handle);
		fat_stream_put(st);
	}
	return 0;
}

void fat_unfence(fat_fs_t *fs, fat_stream_t *st)
{
	fat_meta_wunlock(fs);
	fat_sunlock_excl(fs, st->handle);
	fat_stream_put(st);
}

static int fat_open_common(vfs_mount_t *m, const char *path, int flags,
			   unsigned int mode, vfs_file_t **out)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	fat_stream_t *st = NULL;
	int acc = flags & 3;
	int writing = (acc == O_WRONLY || acc == O_RDWR);
	int r;

	if (!fs || !path || !out)
		return ST_INVALID;
	*out = NULL;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;

	/* O_TRUNC of an existing file frees its data: fence readers. */
	if ((flags & O_TRUNC) && writing) {
		fat_stream_t *v;

		if (fat_fence(fs, ctx, path, &v, 0)) {
			if ((flags & O_CREAT) && (flags & O_EXCL)) {
				r = ST_EXISTS;
			} else if (v->is_dir) {
				r = ST_ISDIR;
			} else if ((r = fat_writable(fs)) == ST_OK &&
				   (r = fat_may_write(fs)) == ST_OK) {
				if (v->size)
					r = fat_truncate_impl(fs, v, 0);
				if (r == ST_OK)
					r = fat_commit(fs);
			}
			if (r == ST_OK) {
				fat_stream_hold(v);
				r = fat_handle_new(fs, v, flags, out);
			}
			fat_unfence(fs, v);
			fat_ctx_free(ctx);
			return r;
		}
	}
	if (flags & O_CREAT) {
		fat_meta_wlock(fs);
		r = fat_walk(fs, ctx, path, &w);
		if (r == ST_OK) {
			if (w.is_root || w.found) {
				if ((flags & O_EXCL)) {
					r = ST_EXISTS;
				} else if (w.is_root) {
					st = w.st;
					w.st = NULL;
				} else if (w.st) {
					st = w.st;
					w.st = NULL;
				} else {
					st = fat_stream_get(fs, w.dir, &w.di, &r);
				}
			} else {
				r = fat_create_locked(fs, ctx, &w, mode, &st);
			}
			fat_walk_release(&w);
		}
		if (r == ST_OK && st) {
			if (st->is_dir && writing)
				r = ST_ISDIR;
			else if ((flags & O_DIRECTORY) && !st->is_dir)
				r = ST_NOTDIR;
			else if (writing && fat_writable(fs) != ST_OK)
				r = ST_ROFS;
			if (r == ST_OK)
				r = fat_handle_new(fs, st, flags, out);
			else
				fat_stream_put(st);
		}
		fat_meta_wunlock(fs);
		fat_ctx_free(ctx);
		return r;
	}
	fat_meta_rlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		st = w.st;
		w.st = NULL;
		fat_walk_release(&w);
		if (st->is_dir && writing)
			r = ST_ISDIR;
		else if ((flags & O_DIRECTORY) && !st->is_dir)
			r = ST_NOTDIR;
		else if (writing && fat_writable(fs) != ST_OK)
			r = ST_ROFS;
		if (r == ST_OK)
			r = fat_handle_new(fs, st, flags, out);
		else
			fat_stream_put(st);
	}
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	return r;
}

int fat_open_op(vfs_mount_t *m, const char *path, int flags,
		       vfs_file_t **out)
{
	return fat_open_common(m, path, flags, 0666, out);
}

int fat_open_mode_op(vfs_mount_t *m, const char *path, int flags,
			    unsigned int mode, vfs_file_t **out)
{
	return fat_open_common(m, path, flags, mode, out);
}

/* The last close of an unlinked file releases its clusters: its pages go
 * first (none may be written into clusters that are free again), then the
 * clusters, then a commit.  Stream lock and metadata lock exclusive. */
static int fat_release_orphan(fat_fs_t *fs, fat_stream_t *st)
{
	int r;

	fat_assert_excl(fs);
	if (!st->orphan || st->opens != 0 || st->nclusters == 0)
		return ST_OK;
	pagecache_invalidate_file(&fs->sb, fat_chain_of(st));
	st->data_dirty = 0;
	if (fat_writable(fs) != ST_OK || fat_may_write(fs) != ST_OK) {
		/* Cannot store anything: the clusters stay lost until fsck. */
		st->nclusters = 0;
		st->next = 0;
		return ST_ROFS;
	}
	r = fat_free_extents(fs, st->ext, st->next);
	st->nclusters = 0;
	st->next = 0;
	st->first_cluster = 0;
	if (r == ST_OK)
		r = fat_commit(fs);
	return r;
}

int fat_close_op(vfs_file_t *f)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	fat_fs_t *fs;
	fat_stream_t *st;
	int r = ST_OK, last, wrote;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	st = h->st;
	/* Decrement under the stream lock: unlink decides between freeing and
	 * orphaning under it, exclusive. */
	fat_slock_shared(fs, st->handle);
	last = __atomic_sub_fetch(&st->opens, 1, __ATOMIC_SEQ_CST) == 0;
	fat_sunlock_shared(fs, st->handle);
	if ((last && st->orphan) || h->wrote) {
		fat_slock_excl(fs, st->handle);
		fat_meta_wlock(fs);
		if (st->orphan) {
			r = fat_release_orphan(fs, st);
			if (r == ST_ROFS)
				r = ST_OK; /* the clusters stay lost */
		} else if ((fs->armed || fs->may_write) &&
			   (st->meta_dirty || st->data_dirty || fs->ndirw)) {
			r = fat_commit(fs);
			/* flush: the written file is on the device when its
			 * last handle closes. */
			if (r == ST_OK && last && fs->opts.flush && fs->armed)
				r = fat_dev_sync(fs);
		}
		if (h->wrote && st->wb_err) {
			if (r == ST_OK)
				r = st->wb_err;
			st->wb_err = 0;
		}
		fat_meta_wunlock(fs);
		fat_sunlock_excl(fs, st->handle);
	}
	wrote = h->wrote;
	if (h->ic)
		icache_unref(h->ic);
	fat_stream_put(st);
	kfree(h);
	/* Read-only after errors: what this handle wrote is not stored. */
	if (r == ST_ROFS)
		r = wrote ? ST_IO : ST_OK;
	return r;
}

/* ---- read / write / seek ------------------------------------------------- */

long fat_read_op(vfs_file_t *f, void *buf, long bytes)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	long r;

	if (!h || !buf)
		return ST_INVALID;
	r = fat_read_common(h->fs, h->st, buf, bytes, h->pos, &h->ra);
	if (r > 0)
		h->pos += (unsigned long)r;
	return r;
}

long fat_read_at_op(vfs_file_t *f, void *buf, long bytes, long off)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	pc_readahead_t ra;

	if (!h || !buf || off < 0)
		return ST_INVALID;
	mm_memset(&ra, 0, sizeof(ra));
	return fat_read_common(h->fs, h->st, buf, bytes, (uint64_t)off, &ra);
}

/* Largest piece of a write copied into the kernel and written under one
 * lock hold. */
#define FAT_WCHUNK (256UL << 10)

/* write() / pwrite(): the user's bytes are copied into a kernel buffer with
 * NO lock held, then written under the stream lock and the metadata lock
 * (exclusive), FAT_WCHUNK at a time.  A write up to that size is one
 * operation (O_APPEND included); a larger one is a sequence of them. */
static long fat_write_common(fat_file_t *h, const void *buf, long bytes,
			     uint64_t *ppos, int append)
{
	fat_fs_t *fs = h->fs;
	unsigned long cap, done = 0;
	uint8_t *kbuf;
	long r = 0;

	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	if (h->st->is_dir)
		return ST_ISDIR;
	cap = (unsigned long)bytes < FAT_WCHUNK ? (unsigned long)bytes :
						  FAT_WCHUNK;
	kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf && cap > PAGE_SIZE) {
		cap = PAGE_SIZE;
		kbuf = (uint8_t *)kalloc(cap);
	}
	if (!kbuf)
		return ST_NOMEM;
	while (done < (unsigned long)bytes) {
		unsigned long n = (unsigned long)bytes - done;

		if (n > cap)
			n = cap;
		/* No lock held while user memory is read. */
		smap_disable();
		mm_memcpy(kbuf, (const uint8_t *)buf + done, n);
		smap_enable();
		fat_slock_excl(fs, h->st->handle);
		fat_meta_wlock(fs);
		r = fat_write_impl(fs, h->st, kbuf, (long)n, ppos, append);
		if (r > 0)
			h->wrote = 1;
		fat_meta_wunlock(fs);
		fat_sunlock_excl(fs, h->st->handle);
		if (r <= 0)
			break;
		done += (unsigned long)r;
		if ((unsigned long)r < n)
			break; /* short: the volume is full */
	}
	kfree(kbuf);
	if (done)
		return (long)done;
	return r;
}

long fat_write_op(vfs_file_t *f, const void *buf, long bytes)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	uint64_t pos;
	long r;

	if (!h || !buf)
		return ST_INVALID;
	pos = h->pos;
	r = fat_write_common(h, buf, bytes, &pos, (f->flags & O_APPEND) != 0);
	if (r > 0)
		h->pos = (unsigned long)pos;
	return r;
}

long fat_write_at_op(vfs_file_t *f, const void *buf, long bytes,
			    long off)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	uint64_t pos;

	if (!h || !buf || off < 0)
		return ST_INVALID;
	pos = (uint64_t)off;
	return fat_write_common(h, buf, bytes, &pos, 0);
}

long fat_seek_op(vfs_file_t *f, long offset, int whence)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	long base, np;

	if (!h)
		return ST_INVALID;
	if (h->st->is_dir) {
		/* rewinddir(); a directory has no byte position. */
		if (whence == 0 && offset == 0)
			h->dirpos = 0;
		return 0;
	}
	switch (whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = (long)h->pos;
		break;
	case 2:
		base = (long)h->st->size;
		break;
	default:
		return ST_INVALID;
	}
	np = base + offset;
	if (np < 0)
		return ST_INVALID;
	/* A position is only a number: nothing about the file changes until
	 * a write lands there (and then the gap is zeroed). */
	h->pos = (unsigned long)np;
	return np;
}

/* ---- truncate ----------------------------------------------------------------- */

int fat_truncate_op(vfs_file_t *f, unsigned long size)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	fat_fs_t *fs;
	int r;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	if (h->st->is_dir)
		return ST_ISDIR;
	fat_slock_excl(fs, h->st->handle);
	fat_meta_wlock(fs);
	r = fat_writable(fs);
	if (r == ST_OK)
		r = fat_may_write(fs);
	if (r == ST_OK)
		r = fat_truncate_impl(fs, h->st, size);
	if (r == ST_OK)
		r = fat_commit(fs);
	fat_meta_wunlock(fs);
	fat_sunlock_excl(fs, h->st->handle);
	return r;
}

/* Store a new attribute byte of a stream.  Metadata lock exclusive. */
int fat_set_attr(fat_fs_t *fs, fat_stream_t *st, uint8_t attr)
{
	int r;

	fat_assert_excl(fs);
	if (attr == st->attr)
		return ST_OK;
	r = fat_writable(fs);
	if (r == ST_OK)
		r = fat_may_write(fs);
	if (r != ST_OK)
		return r;
	st->attr = attr;
	fat_stream_mark_dirty(fs, st);
	return fat_commit(fs);
}

/* chmod: of the read and execute bits nothing can change; the write bits
 * are the read-only attribute -- all of them (as the mask allows) or none,
 * where the attribute applies (files; directories with rodir).  Anything
 * else cannot be stored: EPERM, or silent success with quiet.  Metadata
 * lock exclusive. */
static int fat_chmod_stream(fat_fs_t *fs, fat_stream_t *st, unsigned int mode)
{
	unsigned cur, mask, perm, wmask;
	int can_ro;
	uint8_t attr;

	fat_assert_excl(fs);
	if (st->is_root)
		return fs->opts.quiet ? ST_OK : ST_PERM;
	mask = st->is_dir ? fs->opts.dmask : fs->opts.fmask;
	cur = fat_mode_of(fs, st->attr, st->name83);
	/* Bits the mount's mask removes anyway are not asked for. */
	perm = mode & 0777u & ~mask;
	wmask = 0222u & ~mask;
	if ((mode & 07000u) || (perm & 0555u) != (cur & 0555u))
		return fs->opts.quiet ? ST_OK : ST_PERM;
	can_ro = st->is_dir ? (fs->opts.rodir && wmask) : (wmask != 0);
	if (can_ro) {
		if ((perm & 0222u) && (perm & 0222u) != wmask)
			return fs->opts.quiet ? ST_OK : ST_PERM;
	} else if ((perm & 0222u) != wmask) {
		return fs->opts.quiet ? ST_OK : ST_PERM;
	}
	if (!can_ro)
		return ST_OK;
	attr = st->attr;
	if (perm & 0222u)
		attr &= (uint8_t)~FAT_ATTR_RO;
	else
		attr |= FAT_ATTR_RO;
	return fat_set_attr(fs, st, attr);
}

int fat_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_wlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = fat_chmod_stream(fs, w.st, mode);
		fat_walk_release(&w);
	}
	fat_meta_wunlock(fs);
	fat_ctx_free(ctx);
	return r;
}

int fat_fchmod_op(vfs_file_t *f, unsigned int mode)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	int r;

	if (!h)
		return ST_INVALID;
	fat_meta_wlock(h->fs);
	r = fat_chmod_stream(h->fs, h->st, mode);
	fat_meta_wunlock(h->fs);
	return r;
}

/* chown: the volume stores no owners.  Giving a file the owner the mount
 * shows is a no-op; any other owner fails with EPERM (quiet: succeeds). */
static int fat_chown_check(fat_fs_t *fs, int uid, int gid)
{
	if ((uid != -1 && (uint32_t)uid != fs->opts.uid) ||
	    (gid != -1 && (uint32_t)gid != fs->opts.gid))
		return fs->opts.quiet ? ST_OK : ST_PERM;
	return ST_OK;
}

int fat_chown_op(vfs_mount_t *m, const char *path, int uid, int gid,
			int nofollow)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct kstat k;
	int r;

	(void)nofollow; /* no symbolic links on FAT */
	if (!fs || !path)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_rlock(fs);
	r = fat_stat_locked(fs, ctx, path, &k);
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	if (r != ST_OK)
		return r;
	return fat_chown_check(fs, uid, gid);
}

int fat_fchown_op(vfs_file_t *f, int uid, int gid)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;

	if (!h)
		return ST_INVALID;
	return fat_chown_check(h->fs, uid, gid);
}

/* Set the times: the modification time (2 s), the access date.  Metadata
 * lock exclusive. */
static int fat_utimes_stream(fat_fs_t *fs, fat_stream_t *st,
			     const struct vfs_utimes *t)
{
	uint64_t now = timer_get_realtime_ns();
	uint16_t mt = st->mtime, md = st->mdate, ad = st->adate;
	int r;

	fat_assert_excl(fs);
	if (st->is_root)
		return ST_OK; /* the root has no entry to hold times */
	if (t->mtime_nsec != VFS_UTIME_OMIT) {
		int64_t s = t->mtime_nsec == VFS_UTIME_NOW ?
				    (int64_t)(now / 1000000000ULL) :
				    t->mtime_sec;
		long ns = t->mtime_nsec == VFS_UTIME_NOW ?
				  (long)(now % 1000000000ULL) :
				  t->mtime_nsec;

		fat_time_encode(fs, s, ns, &mt, &md, NULL);
	}
	if (t->atime_nsec != VFS_UTIME_OMIT) {
		int64_t s = t->atime_nsec == VFS_UTIME_NOW ?
				    (int64_t)(now / 1000000000ULL) :
				    t->atime_sec;
		long ns = t->atime_nsec == VFS_UTIME_NOW ?
				  (long)(now % 1000000000ULL) :
				  t->atime_nsec;

		fat_time_encode(fs, s, ns, NULL, &ad, NULL);
	}
	if (mt == st->mtime && md == st->mdate && ad == st->adate)
		return ST_OK; /* nothing to store */
	r = fat_writable(fs);
	if (r == ST_OK)
		r = fat_may_write(fs);
	if (r != ST_OK)
		return r;
	st->mtime = mt;
	st->mdate = md;
	st->adate = ad;
	fat_stream_mark_dirty(fs, st);
	return fat_commit(fs);
}

int fat_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
			  const struct vfs_utimes *t)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct fat_walk w;
	int r;

	(void)nofollow; /* no symbolic links on FAT */
	if (!fs || !path || !t)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_wlock(fs);
	r = fat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = fat_utimes_stream(fs, w.st, t);
		fat_walk_release(&w);
	}
	fat_meta_wunlock(fs);
	fat_ctx_free(ctx);
	return r;
}

int fat_futimens_op(vfs_file_t *f, const struct vfs_utimes *t)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	int r;

	if (!h || !t)
		return ST_INVALID;
	fat_meta_wlock(h->fs);
	r = fat_utimes_stream(h->fs, h->st, t);
	fat_meta_wunlock(h->fs);
	return r;
}
