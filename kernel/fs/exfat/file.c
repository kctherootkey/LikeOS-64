/* LikeOS - exFAT filesystem driver
 *
 * ValidDataLength, the data paths, open, close, read, write, seek, stat,
 * attributes.
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
 * 19. ValidDataLength and the data paths
 *
 * Bytes of a file between ValidDataLength and DataLength read as zeros
 * whatever the clusters hold.  The page cache gets that for free for blocks
 * wholly past VDL (block_to_lba() answers 0).  The one page that straddles
 * a VDL which is short of the size and not on a block boundary -- only
 * files written elsewhere have one -- is NOFILL: block_at() answers 0 for
 * it and the driver fills it itself (exfat_nofill_get).
 *
 * This driver keeps VDL on a page boundary (or at the size): whenever it
 * moves VDL forward it first makes sure every page between the old and new
 * VDL is in the cache holding what the file must read there (the old data
 * below the old VDL, zeros above), then raises VDL, then dirties those
 * pages -- so writeback stores real content for every block the new VDL
 * covers, and no dirty page ever lacks a location.  Extending a file with
 * truncate() only raises DataLength: nothing is written for the new range.
 * =========================================================================== */

/* LBA of block n of the stream regardless of VDL (0 without a cluster). */
static unsigned long exfat_st_block_lba_raw(exfat_fs_t *fs,
					    const exfat_stream_t *st,
					    unsigned long n)
{
	uint64_t ci = (uint64_t)n >> fs->bpc_shift;
	uint32_t disk = exfat_st_cluster(st, ci);

	if (!disk)
		return 0;
	return (unsigned long)(fs->heap_lba +
			       ((uint64_t)(disk - EXFAT_FIRST_CLUSTER)
				<< (fs->cs_shift - fs->dev_shift)) +
			       (((uint64_t)n & ((1ULL << fs->bpc_shift) - 1))
				<< (fs->bs_shift - fs->dev_shift)));
}

/* The NOFILL page `pidx' of `st', held: from the cache, or read here --
 * the bytes below VDL from the device, zeros above -- and installed.
 * Metadata lock held. */
static pc_page_t *exfat_nofill_get(exfat_fs_t *fs, exfat_stream_t *st,
				   unsigned long pidx)
{
	unsigned long chain = exfat_chain_of(st);
	uint64_t page_start = (uint64_t)pidx << PAGE_SHIFT;
	uint64_t valid;
	pc_page_t *pg;
	uint8_t *buf;

	might_sleep();
	pg = pagecache_lookup_held(&fs->sb, chain, pidx);
	if (pg)
		return pg;
	if (st->vdl <= page_start)
		valid = 0;
	else
		valid = exfat_min64(st->vdl - page_start, PAGE_SIZE);
	buf = (uint8_t *)kcalloc(1, PAGE_SIZE);
	if (!buf)
		return NULL;
	for (uint64_t off = 0; off < valid; off += fs->bs) {
		unsigned long blk = (unsigned long)((page_start + off) >>
						    fs->bs_shift);
		unsigned long lba = exfat_st_block_lba_raw(fs, st, blk);
		uint64_t want = exfat_min64(fs->bs, valid - off);
		uint64_t secs = (want + fs->dev_ss - 1) >> fs->dev_shift;

		if (!lba)
			continue; /* no cluster: zeros */
		if (exfat_dev_read(fs, lba, secs, buf + off) != ST_OK) {
			kfree(buf);
			return NULL;
		}
	}
	/* Bytes at and past VDL must be zero even inside the last sector
	 * read. */
	pg = pagecache_install(&fs->sb, chain, pidx, buf, (unsigned long)valid,
			       1);
	kfree(buf);
	return pg;
}

/* A page of the stream to read, held. */
static pc_page_t *exfat_page_read(exfat_fs_t *fs, exfat_stream_t *st,
				  unsigned long pidx)
{
	unsigned long chain = exfat_chain_of(st);

	if (pidx == exfat_nofill_page(fs, st))
		return exfat_nofill_get(fs, st, pidx);
	return pagecache_get_held(chain, pidx, (unsigned long)st->size,
				  &fs->sb, chain);
}

/* A page of the stream to write into, held (release with
 * pagecache_write_end / _abort).  `full': every byte will be replaced. */
static pc_page_t *exfat_page_write(exfat_fs_t *fs, exfat_stream_t *st,
				   unsigned long pidx, int full,
				   uint64_t old_size)
{
	unsigned long chain = exfat_chain_of(st);

	pc_page_t *pg;

	if (pidx == exfat_nofill_page(fs, st))
		return exfat_nofill_get(fs, st, pidx);
	pg = pagecache_get_for_write(chain, pidx, (unsigned long)old_size,
				     &fs->sb, chain, full);
	if (!pg && st->data_dirty) {
		/* Out of pages: this stream's own dirty ones are what the
		 * cache cannot give back.  Store them (they become clean and
		 * reclaimable) and try once more. */
		pagecache_flush_file(&fs->sb, chain);
		pagecache_shrink(64, 0);
		pg = pagecache_get_for_write(chain, pidx,
					     (unsigned long)old_size, &fs->sb,
					     chain, full);
	}
	return pg;
}

/* Move VDL forward to cover `target' (rounded up to a page, capped at the
 * size), storing zeros (or the old data) for everything it newly covers.
 * Metadata lock and the stream's lock exclusive, armed. */
static int exfat_vdl_extend(exfat_fs_t *fs, exfat_stream_t *st,
			    uint64_t target)
{
	uint64_t v = st->vdl, nv;
	unsigned long p0, plast, chain = exfat_chain_of(st);
	pc_page_t *pg0 = NULL;
	unsigned n = 0;

	exfat_assert_excl(fs);
	if (target > st->size)
		target = st->size;
	if (target <= v)
		return ST_OK;
	nv = exfat_min64(st->size, exfat_round_up(target, PAGE_SIZE));
	p0 = (unsigned long)(v >> PAGE_SHIFT);
	plast = (unsigned long)((nv - 1) >> PAGE_SHIFT);
	/* The page holding the old VDL: its content (old data below, zeros
	 * above) read with the OLD mapping, before VDL moves. */
	if (v & (PAGE_SIZE - 1)) {
		pg0 = exfat_page_read(fs, st, p0);
		if (!pg0)
			return ST_IO;
	}
	st->vdl = nv;
	st->grew = 1;
	for (unsigned long p = p0; p <= plast; p++) {
		pc_page_t *pg;

		exfat_note_dirty(st, p);
		if (p == p0 && pg0) {
			pagecache_mark_dirty(pg0);
			pagecache_put(pg0);
			pg0 = NULL;
			continue;
		}
		/* Wholly past the old VDL: zeros, cached or fresh. */
		pg = pagecache_get_for_write(chain, p, (unsigned long)v,
					     &fs->sb, chain, 1);
		if (!pg) {
			pagecache_flush_file(&fs->sb, chain);
			pagecache_shrink(64, 0);
			pg = pagecache_get_for_write(chain, p, (unsigned long)v,
						     &fs->sb, chain, 1);
		}
		if (!pg) {
			/* Out of memory: VDL back to what is stored. */
			st->vdl = (uint64_t)p << PAGE_SHIFT;
			if (st->vdl < v)
				st->vdl = v;
			return ST_NOMEM;
		}
		pagecache_write_end(pg);
		st->data_dirty = 1;
		if (++n % EXFAT_BALANCE_EVERY == 0)
			pagecache_balance_dirty(&fs->sb);
	}
	st->data_dirty = 1;
	exfat_stream_mark_dirty(fs, st);
	return ST_OK;
}

/* Pages a read takes under the locks before it copies them out. */
#define EXFAT_RBATCH 16

/* Read `bytes' at `pos' into `buf' (user memory, or a kernel page for
 * demand paging).  User memory is never touched with a lock held: under the
 * stream lock and the metadata lock (both shared) up to EXFAT_RBATCH pages
 * are looked up / read and HELD, the locks are dropped, then the bytes are
 * copied out of the held pages (a page truncated meanwhile is detached but
 * keeps its frame until put), and the next batch follows.  `ra' is the
 * readahead state to use, or NULL. */
static long exfat_read_common(exfat_fs_t *fs, exfat_stream_t *st, void *buf,
			      long bytes, uint64_t pos, pc_readahead_t *ra)
{
	unsigned long chain = exfat_chain_of(st);
	unsigned long copied = 0;
	long err = 0;

	if (st->is_dir)
		return ST_ISDIR;
	if (bytes < 0)
		return ST_INVALID;
	while ((unsigned long)bytes > copied && !err) {
		pc_page_t *pages[EXFAT_RBATCH];
		unsigned offs[EXFAT_RBATCH];
		unsigned long lens[EXFAT_RBATCH];
		unsigned n = 0;
		uint64_t size, at = pos + copied;
		unsigned long want = (unsigned long)bytes - copied;

		exfat_slock_shared(fs, st->handle);
		exfat_meta_rlock(fs);
		size = st->size;
		if (at < size && want > size - at)
			want = (unsigned long)(size - at);
		while (at < size && want && n < EXFAT_RBATCH) {
			unsigned long pidx = (unsigned long)(at >> PAGE_SHIFT);
			unsigned off = (unsigned)(at & (PAGE_SIZE - 1));
			unsigned long chunk = PAGE_SIZE - off;
			pc_page_t *pg;

			if (chunk > want)
				chunk = want;
			pg = exfat_page_read(fs, st, pidx);
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
		exfat_meta_runlock(fs);
		exfat_sunlock_shared(fs, st->handle);
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

/* Largest file size the volume could hold (all clusters), and the limit
 * of the 64-bit fields. */
static uint64_t exfat_max_file_size(exfat_fs_t *fs)
{
	uint64_t m = (uint64_t)fs->cluster_count << fs->cs_shift;

	return m > (1ULL << 62) ? (1ULL << 62) : m;
}

void exfat_touch_mtime(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_now(fs, &st->mtime, &st->mtime_cs, &st->mtime_utc);
	st->attr |= EXFAT_ATTR_ARCHIVE;
}

/* Write `bytes' from the KERNEL buffer `buf' at *ppos (the size for
 * O_APPEND).  The stream's lock exclusive and the metadata lock exclusive
 * held. */
static long exfat_write_impl(exfat_fs_t *fs, exfat_stream_t *st,
			     const void *buf, long bytes, uint64_t *ppos,
			     int append)
{
	uint64_t pos, end, old_size;
	unsigned long done = 0;
	unsigned n = 0;
	int r;

	exfat_assert_excl(fs);
	if (st->is_dir)
		return ST_ISDIR;
	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	r = exfat_writable(fs);
	if (r != ST_OK)
		return r;
	r = exfat_may_write(fs);
	if (r != ST_OK)
		return r;
	pos = append ? st->size : *ppos;
	if (!exfat_add_ok(pos, (uint64_t)bytes, &end))
		return ST_INVALID;
	if (end > exfat_max_file_size(fs)) {
		if (pos >= exfat_max_file_size(fs))
			return ST_NOSPC;
		end = exfat_max_file_size(fs);
	}
	/* Clusters first (allocate before reference); a full volume gets a
	 * short write if any byte fits. */
	{
		uint64_t need = (end + fs->cs - 1) >> fs->cs_shift;

		if (need > st->nclusters) {
			r = exfat_stream_grow(fs, st, (uint32_t)need);
			if (r == ST_NOSPC) {
				uint64_t have;

				exfat_bm_count_free(fs);
				have = (uint64_t)st->nclusters +
				       (uint64_t)(fs->free_clusters > 0 ?
							  fs->free_clusters :
							  0);
				if ((have << fs->cs_shift) <= pos)
					return ST_NOSPC;
				end = have << fs->cs_shift;
				r = exfat_stream_grow(fs, st, (uint32_t)have);
			}
			if (r != ST_OK)
				return r;
			exfat_stream_mark_dirty(fs, st);
		}
	}
	old_size = st->size;
	if (end > st->size) {
		st->size = end;
		st->grew = 1;
	}
	/* Everything between VDL and the write becomes zeros on the device. */
	if (pos > st->vdl) {
		r = exfat_vdl_extend(fs, st, pos);
		if (r != ST_OK)
			goto fail;
	}
	while (pos + done < end) {
		uint64_t at = pos + done;
		unsigned long pidx = (unsigned long)(at >> PAGE_SHIFT);
		unsigned off = (unsigned)(at & (PAGE_SIZE - 1));
		unsigned long chunk = PAGE_SIZE - off;
		uint64_t page_end;
		pc_page_t *pg;
		int full;

		if (chunk > end - at)
			chunk = (unsigned long)(end - at);
		full = (off == 0 && chunk == PAGE_SIZE);
		pg = exfat_page_write(fs, st, pidx, full, old_size);
		if (!pg) {
			r = ST_NOMEM;
			goto fail;
		}
		mm_memcpy(pg->data + off, (const uint8_t *)buf + done, chunk);
		/* VDL covers the whole page before it is dirtied. */
		page_end = exfat_min64(st->size,
				       ((uint64_t)pidx + 1) << PAGE_SHIFT);
		if (page_end > st->vdl)
			st->vdl = page_end;
		pagecache_write_end(pg);
		st->data_dirty = 1;
		exfat_note_dirty(st, pidx);
		done += chunk;
		if (++n % EXFAT_BALANCE_EVERY == 0)
			pagecache_balance_dirty(&fs->sb);
	}
	exfat_touch_mtime(fs, st);
	exfat_stream_mark_dirty(fs, st);
	*ppos = pos + done;
	pagecache_balance_dirty(&fs->sb);
	return (long)done;
fail:
	/* What was copied stays; the size and the allocation shrink back to
	 * cover exactly that (a DataLength short of its clusters is a
	 * corrupt file). */
	{
		uint64_t keep = exfat_max64(old_size, pos + done);

		if (keep > st->size)
			keep = st->size;
		if (keep < st->size) {
			st->size = keep;
			if (st->vdl > keep)
				st->vdl = keep;
		}
		exfat_stream_shrink(fs, st,
				    (uint32_t)((st->size + fs->cs - 1) >>
					       fs->cs_shift));
		if (done)
			exfat_touch_mtime(fs, st);
		exfat_stream_mark_dirty(fs, st);
	}
	if (done) {
		*ppos = pos + done;
		return (long)done;
	}
	return r;
}

/* Change a file's size.  Shrinking drops the pages and (deferred) the
 * clusters past the new end; growing allocates clusters but leaves VDL
 * where it is (the new range reads as zeros without being written).  The
 * stream's lock and the metadata lock exclusive, armed. */
static int exfat_truncate_impl(exfat_fs_t *fs, exfat_stream_t *st,
			       uint64_t nsize)
{
	unsigned long chain = exfat_chain_of(st);
	int r = ST_OK;

	exfat_assert_excl(fs);
	if (st->is_dir)
		return ST_ISDIR;
	if (nsize > exfat_max_file_size(fs))
		return ST_NOSPC;
	if (nsize < st->size) {
		uint32_t ncl = (uint32_t)((nsize + fs->cs - 1) >> fs->cs_shift);
		exfat_extent_t *tail;
		uint32_t ntail;

		/* ALL OR NOTHING: what can fail comes before anything
		 * changes. */
		r = exfat_shrink_prepare(fs, st, ncl, &tail, &ntail);
		if (r != ST_OK)
			return r;
		/* Pages past the end go (dirty ones too: their bytes are cut
		 * off), and the tail of the page holding the new end must read
		 * as zeros should the file grow again. */
		pagecache_invalidate_range(&fs->sb, chain, (unsigned long)nsize);
		if (nsize & (PAGE_SIZE - 1)) {
			pc_page_t *pg = pagecache_lookup_held(
				&fs->sb, chain,
				(unsigned long)(nsize >> PAGE_SHIFT));

			if (pg) {
				unsigned off = (unsigned)(nsize &
							  (PAGE_SIZE - 1));

				mm_memset(pg->data + off, 0, PAGE_SIZE - off);
				pagecache_put(pg);
			}
		}
		st->size = nsize;
		if (st->vdl > nsize)
			st->vdl = nsize;
		exfat_stream_mark_dirty(fs, st);
		r = exfat_shrink_apply(fs, st, ncl, tail, ntail);
	} else if (nsize == st->size) {
		/* No change, no new times (the size did not change). */
		return ST_OK;
	} else if (nsize > st->size) {
		uint32_t ncl = (uint32_t)((nsize + fs->cs - 1) >> fs->cs_shift);
		uint32_t ocl = st->nclusters;
		uint64_t v = st->vdl, osize = st->size;

		r = exfat_stream_grow(fs, st, ncl);
		if (r != ST_OK)
			return r;
		st->size = nsize;
		/* Keep VDL on a page boundary: the page holding it is stored
		 * with zeros past the old end now, so no page of the file
		 * ever straddles VDL. */
		if (v & (PAGE_SIZE - 1))
			r = exfat_vdl_extend(fs, st,
					     exfat_min64(nsize,
							 exfat_round_up(v,
									PAGE_SIZE)));
		if (r != ST_OK) {
			/* The page could not be read: nothing was dirtied,
			 * the file goes back to what it was. */
			st->size = osize;
			if (st->vdl > osize)
				st->vdl = osize;
			exfat_stream_shrink(fs, st, ocl);
			return r;
		}
		exfat_stream_mark_dirty(fs, st);
	}
	exfat_touch_mtime(fs, st);
	exfat_stream_mark_dirty(fs, st);
	return r;
}

/* ===========================================================================
 * 20. VFS operations
 *
 * Locking per exfat.h: read-only operations take the metadata lock
 * shared; mutations take it exclusive, after the stream lock of any file
 * whose data they free (unlink, rename over a file, O_TRUNC, truncate,
 * write).  Every mutation arms the mount first and ends with a commit, so
 * each namespace change is on the device (in order) when it returns.
 * =========================================================================== */

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

exfat_fs_t *exfat_fs_of(vfs_mount_t *m)
{
	return m ? (exfat_fs_t *)m->fs_data : NULL;
}

uint64_t exfat_ino(exfat_fs_t *fs, uint32_t first, uint32_t dir_first,
		   uint32_t index)
{
	(void)fs;
	if (first)
		return first;
	return EXFAT_INO_POS(dir_first, index);
}

uint64_t exfat_st_ino(exfat_fs_t *fs, const exfat_stream_t *st)
{
	if (st->is_root)
		return fs->root_cluster;
	return exfat_ino(fs, st->first_cluster,
			 st->parent ? st->parent->first_cluster :
				      (uint32_t)(st->key >> 24),
			 st->entry_index);
}

static unsigned exfat_mode_of(exfat_fs_t *fs, uint16_t attr)
{
	unsigned mode;

	if (attr & EXFAT_ATTR_DIR) {
		mode = S_IFDIR | (0777u & ~fs->opts.dmask);
	} else {
		mode = S_IFREG | (0777u & ~fs->opts.fmask);
		if (attr & EXFAT_ATTR_RO)
			mode &= ~0222u;
	}
	return mode;
}

/* Count the subdirectories of `d' (st_nlink of a directory), cached. */
static long exfat_count_subdirs(exfat_fs_t *fs, exfat_stream_t *d)
{
	exfat_diter_t it;
	uint64_t slots = exfat_dir_slots(d);
	long n = 0;

	if (d->subdirs >= 0)
		return d->subdirs;
	exfat_diter_init(&it, fs, d);
	for (uint64_t i = 0; i < slots;) {
		const uint8_t *e = exfat_diter_get(&it, i);

		if (!e || e[EXFAT_DE_TYPE] == EXFAT_ET_EOD)
			break;
		if (e[EXFAT_DE_TYPE] == EXFAT_ET_FILE) {
			unsigned sc = e[EXFAT_DE_SECONDARY_COUNT];

			if (exfat_le16(e + EXFAT_FILE_ATTR) & EXFAT_ATTR_DIR)
				n++;
			i += (sc >= EXFAT_SET_MIN_SECONDARY &&
			      sc <= EXFAT_SET_MAX_SECONDARY) ?
				     1 + sc :
				     1;
			continue;
		}
		i += (e[EXFAT_DE_TYPE] & EXFAT_ET_INUSE) ? exfat_skip_slots(e) :
							   1;
	}
	exfat_diter_done(&it);
	if (n > 65000)
		n = 65000;
	d->subdirs = n;
	return n;
}

static void exfat_stat_times(exfat_fs_t *fs, struct kstat *k, uint32_t mt,
			     uint8_t mcs, uint8_t mutc, uint32_t at,
			     uint8_t autc)
{
	exfat_time_decode(fs, mt, mcs, mutc, &k->st_mtime, &k->st_mtime_nsec);
	exfat_time_decode(fs, at, 0, autc, &k->st_atime, &k->st_atime_nsec);
	/* exFAT keeps no change time: the modification time stands in. */
	k->st_ctime = k->st_mtime;
	k->st_ctime_nsec = k->st_mtime_nsec;
}

static void exfat_stat_from_stream(exfat_fs_t *fs, exfat_stream_t *st,
				   struct kstat *k)
{
	mm_memset(k, 0, sizeof(*k));
	k->st_ino = exfat_st_ino(fs, st);
	k->st_uid = fs->opts.uid;
	k->st_gid = fs->opts.gid;
	k->st_blksize = fs->cs > PAGE_SIZE ? fs->cs : PAGE_SIZE;
	k->st_blocks = ((uint64_t)st->nclusters << fs->cs_shift) >> 9;
	if (st->is_dir) {
		k->st_mode = exfat_mode_of(fs, EXFAT_ATTR_DIR);
		k->st_nlink = (uint32_t)(2 + exfat_count_subdirs(fs, st));
		k->st_size = st->size;
	} else {
		k->st_mode = exfat_mode_of(fs, st->attr);
		k->st_nlink = st->orphan ? 0 : 1;
		k->st_size = st->size;
	}
	if (st->is_root) {
		/* The root has no entry set: no times of its own. */
		return;
	}
	exfat_stat_times(fs, k, st->mtime, st->mtime_cs, st->mtime_utc,
			 st->atime, st->atime_utc);
}

static void exfat_stat_from_dentry(exfat_fs_t *fs, exfat_stream_t *dir,
				   const exfat_dentry_info_t *di,
				   struct kstat *k)
{
	mm_memset(k, 0, sizeof(*k));
	k->st_ino = exfat_ino(fs, di->first_cluster, dir->first_cluster,
			      di->index);
	k->st_uid = fs->opts.uid;
	k->st_gid = fs->opts.gid;
	k->st_blksize = fs->cs > PAGE_SIZE ? fs->cs : PAGE_SIZE;
	k->st_blocks = exfat_round_up(di->size, fs->cs) >> 9;
	k->st_mode = exfat_mode_of(fs, di->attr);
	k->st_nlink = 1;
	k->st_size = di->size;
	exfat_stat_times(fs, k, di->mtime, di->mtime_cs, di->mtime_utc,
			 di->atime, di->atime_utc);
}

/* stat of a path.  Files are described from their entry set (or their
 * stream object when one exists: it is newer); directories through their
 * stream (st_nlink needs the subdirectory count).  Metadata lock shared. */
int exfat_stat_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
		      const char *path, struct kstat *k)
{
	struct exfat_walk w;
	int r = exfat_walk(fs, ctx, path, &w);

	if (r != ST_OK)
		return r;
	if (w.is_root || w.st) {
		exfat_stat_from_stream(fs, w.st, k);
		exfat_walk_release(&w);
		return ST_OK;
	}
	if (!w.found) {
		exfat_walk_release(&w);
		return ST_NOT_FOUND;
	}
	{
		exfat_stream_t *st = exfat_stream_find(
			fs, EXFAT_KEY(w.dir->first_cluster, w.di.index));

		if (!st && (w.di.attr & EXFAT_ATTR_DIR)) {
			int err;

			st = exfat_stream_get(fs, w.dir, &w.di, &err);
			if (!st) {
				exfat_walk_release(&w);
				return err;
			}
		}
		if (st) {
			exfat_stat_from_stream(fs, st, k);
			exfat_stream_put(st);
		} else {
			exfat_stat_from_dentry(fs, w.dir, &w.di, k);
		}
	}
	exfat_walk_release(&w);
	return ST_OK;
}

int exfat_stat_op(vfs_mount_t *m, const char *path, struct kstat *k)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	int r;

	if (!fs || !path || !k)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	exfat_meta_rlock(fs);
	r = exfat_stat_locked(fs, ctx, path, k);
	exfat_meta_runlock(fs);
	exfat_ctx_free(ctx);
	return r;
}

int exfat_fstat_op(vfs_file_t *f, struct kstat *k)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	exfat_fs_t *fs;

	if (!h || !k)
		return ST_INVALID;
	fs = h->fs;
	exfat_meta_rlock(fs);
	exfat_stat_from_stream(fs, h->st, k);
	exfat_meta_runlock(fs);
	return ST_OK;
}

/* ---- open / close ---------------------------------------------------------- */

/* Make a handle for a held stream (that hold passes to the handle). */
static int exfat_handle_new(exfat_fs_t *fs, exfat_stream_t *st, int flags,
			    vfs_file_t **out)
{
	exfat_file_t *h = (exfat_file_t *)kcalloc(1, sizeof(*h));
	unsigned long chain = exfat_chain_of(st);

	if (!h) {
		exfat_stream_put(st);
		return ST_NOMEM;
	}
	h->vfs.ops = &exfat_vfs_ops;
	h->vfs.fs_private = h;
	h->fs = fs;
	h->st = st;
	h->open_flags = flags;
	h->pos = (flags & O_APPEND) ? (unsigned long)st->size : 0;
	if (st->is_dir)
		h->parent_ino = st->parent ? exfat_st_ino(fs, st->parent) :
					     exfat_st_ino(fs, st);
	__atomic_add_fetch(&st->opens, 1, __ATOMIC_SEQ_CST);
	h->ic = icache_get(&fs->sb, chain, (unsigned long)st->size, st->attr,
			   st->parent ? exfat_chain_of(st->parent) : chain, 0, 0,
			   0, 0);
	*out = &h->vfs;
	return ST_OK;
}

/* A walk that could not be made (memory, the device) -- as opposed to a
 * path that does not lead to a stream. */
static inline int exfat_walk_failed(int r)
{
	return r == ST_NOMEM || r == ST_IO;
}

/* Resolve `path', take its stream's lock exclusive and then the metadata
 * lock exclusive, and check the path still leads to the same stream.
 * 1 with both locks held (*stp held), 0 with none (the path does not lead
 * to a stream right now), or the error that kept the walk from being
 * made (ST_NOMEM, ST_IO: none held). */
int exfat_fence(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
		exfat_stream_t **stp)
{
	for (int tries = 0; tries < 4; tries++) {
		struct exfat_walk w;
		exfat_stream_t *st, *st2;
		int r;

		exfat_meta_rlock(fs);
		r = exfat_walk_obj(fs, ctx, path, &w);
		if (r != ST_OK) {
			exfat_meta_runlock(fs);
			return exfat_walk_failed(r) ? r : 0;
		}
		st = w.st;
		w.st = NULL;
		exfat_walk_release(&w);
		exfat_meta_runlock(fs);

		exfat_slock_excl(fs, st->handle);
		exfat_meta_wlock(fs);
		r = exfat_walk_obj(fs, ctx, path, &w);
		st2 = (r == ST_OK) ? w.st : NULL;
		if (r == ST_OK)
			exfat_walk_release(&w);
		if (st2 == st) {
			*stp = st;
			return 1;
		}
		exfat_meta_wunlock(fs);
		exfat_sunlock_excl(fs, st->handle);
		exfat_stream_put(st);
		if (exfat_walk_failed(r))
			return r;
	}
	return 0;
}

void exfat_unfence(exfat_fs_t *fs, exfat_stream_t *st)
{
	exfat_meta_wunlock(fs);
	exfat_sunlock_excl(fs, st->handle);
	exfat_stream_put(st);
}

static int exfat_open_common(vfs_mount_t *m, const char *path, int flags,
			     unsigned int mode, vfs_file_t **out)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct exfat_walk w;
	exfat_stream_t *st = NULL;
	int acc = flags & 3;
	int writing = (acc == O_WRONLY || acc == O_RDWR);
	int r;

	if (!fs || !path || !out)
		return ST_INVALID;
	*out = NULL;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;

	/* O_TRUNC of an existing file frees its data: fence readers. */
	if ((flags & O_TRUNC) && writing) {
		exfat_stream_t *v;
		int fr = exfat_fence(fs, ctx, path, &v);

		if (fr < 0) {
			exfat_ctx_free(ctx);
			return fr;
		}
		if (fr) {
			if ((flags & O_CREAT) && (flags & O_EXCL)) {
				r = ST_EXISTS;
			} else if (v->is_dir) {
				r = ST_ISDIR;
			} else if ((r = exfat_writable(fs)) == ST_OK &&
				   (r = exfat_may_write(fs)) == ST_OK) {
				if (v->size)
					r = exfat_truncate_impl(fs, v, 0);
				if (r == ST_OK)
					r = exfat_commit(fs);
			}
			if (r == ST_OK) {
				exfat_stream_hold(v);
				r = exfat_handle_new(fs, v, flags, out);
			}
			exfat_unfence(fs, v);
			exfat_ctx_free(ctx);
			return r;
		}
	}
	if (flags & O_CREAT) {
		exfat_meta_wlock(fs);
		r = exfat_walk(fs, ctx, path, &w);
		if (r == ST_OK) {
			if (w.is_root || w.found) {
				if ((flags & O_EXCL)) {
					r = ST_EXISTS;
				} else if (w.is_root) {
					st = w.st;
					w.st = NULL;
				} else {
					st = exfat_stream_get(fs, w.dir, &w.di,
							      &r);
				}
			} else {
				r = exfat_create_locked(fs, ctx, &w, mode, &st);
			}
			exfat_walk_release(&w);
		}
		if (r == ST_OK && st) {
			if (st->is_dir && writing)
				r = ST_ISDIR;
			else if ((flags & O_DIRECTORY) && !st->is_dir)
				r = ST_NOTDIR;
			if (r == ST_OK)
				r = exfat_handle_new(fs, st, flags, out);
			else
				exfat_stream_put(st);
		}
		exfat_meta_wunlock(fs);
		exfat_ctx_free(ctx);
		return r;
	}
	exfat_meta_rlock(fs);
	r = exfat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		st = w.st;
		w.st = NULL;
		exfat_walk_release(&w);
		if (st->is_dir && writing)
			r = ST_ISDIR;
		else if ((flags & O_DIRECTORY) && !st->is_dir)
			r = ST_NOTDIR;
		else if (writing && exfat_writable(fs) != ST_OK)
			r = ST_ROFS;
		if (r == ST_OK)
			r = exfat_handle_new(fs, st, flags, out);
		else
			exfat_stream_put(st);
	}
	exfat_meta_runlock(fs);
	exfat_ctx_free(ctx);
	return r;
}

int exfat_open_op(vfs_mount_t *m, const char *path, int flags,
		  vfs_file_t **out)
{
	return exfat_open_common(m, path, flags, 0666, out);
}

int exfat_open_mode_op(vfs_mount_t *m, const char *path, int flags,
		       unsigned int mode, vfs_file_t **out)
{
	return exfat_open_common(m, path, flags, mode, out);
}

/* The last close of an unlinked file releases its clusters: its pages go
 * first (none may be written into clusters that are free again), then the
 * clusters, then a commit.  Stream lock and metadata lock exclusive. */
static int exfat_release_orphan(exfat_fs_t *fs, exfat_stream_t *st)
{
	int r;

	exfat_assert_excl(fs);
	if (!st->orphan || st->opens != 0)
		return ST_OK;
	if (st->orphan_held) {
		st->orphan_held = 0;
		fs->orphans_held--;
	}
	if (st->nclusters == 0)
		return ST_OK;
	pagecache_invalidate_file(&fs->sb, exfat_chain_of(st));
	st->data_dirty = 0;
	if (exfat_writable(fs) != ST_OK || exfat_may_write(fs) != ST_OK) {
		/* Cannot store anything: the clusters stay lost until fsck. */
		st->nclusters = 0;
		st->next = 0;
		return ST_ROFS;
	}
	r = exfat_free_reserve(fs, st->next, 0);
	if (r == ST_OK)
		r = exfat_free_extents(fs, st->ext, st->next);
	if (r != ST_OK)
		fs->needs_check = 1; /* the rest stays lost */
	st->nclusters = 0;
	st->next = 0;
	st->first_cluster = 0;
	if (r == ST_OK)
		r = exfat_commit(fs);
	return r;
}

int exfat_close_op(vfs_file_t *f)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	exfat_fs_t *fs;
	exfat_stream_t *st;
	int r = ST_OK, last, wrote;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	st = h->st;
	/* Decrement under the stream lock: unlink decides between freeing
	 * and orphaning under it, exclusive. */
	exfat_slock_shared(fs, st->handle);
	last = __atomic_sub_fetch(&st->opens, 1, __ATOMIC_SEQ_CST) == 0;
	exfat_sunlock_shared(fs, st->handle);
	if ((last && st->orphan) || h->wrote) {
		exfat_slock_excl(fs, st->handle);
		exfat_meta_wlock(fs);
		if (st->orphan) {
			r = exfat_release_orphan(fs, st);
			if (r == ST_ROFS)
				r = ST_OK; /* the clusters stay lost */
		} else if ((fs->armed || fs->may_write) &&
			   (st->meta_dirty || st->data_dirty || fs->ndirw)) {
			r = exfat_commit(fs);
			/* A latched mount could not store what this handle
			 * wrote: that is an error to the writer. */
		}
		if (h->wrote && st->wb_err) {
			if (r == ST_OK)
				r = st->wb_err;
			st->wb_err = 0;
		}
		exfat_meta_wunlock(fs);
		exfat_sunlock_excl(fs, st->handle);
	}
	wrote = h->wrote;
	if (h->ic)
		icache_unref(h->ic);
	exfat_stream_put(st);
	kfree(h);
	/* Read-only after errors: what this handle wrote is not stored. */
	if (r == ST_ROFS)
		r = wrote ? ST_IO : ST_OK;
	return r;
}

/* ---- read / write / seek ------------------------------------------------- */

long exfat_read_op(vfs_file_t *f, void *buf, long bytes)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	long r;

	if (!h || !buf)
		return ST_INVALID;
	r = exfat_read_common(h->fs, h->st, buf, bytes, h->pos, &h->ra);
	if (r > 0)
		h->pos += (unsigned long)r;
	return r;
}

long exfat_read_at_op(vfs_file_t *f, void *buf, long bytes, long off)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	pc_readahead_t ra;

	if (!h || !buf || off < 0)
		return ST_INVALID;
	mm_memset(&ra, 0, sizeof(ra));
	return exfat_read_common(h->fs, h->st, buf, bytes, (uint64_t)off, &ra);
}

/* Largest piece of a write copied into the kernel and written under one
 * lock hold. */
#define EXFAT_WCHUNK (256UL << 10)

/* write() / pwrite(): the user's bytes are copied into a kernel buffer with
 * NO lock held, then written under the stream lock and the metadata lock
 * (exclusive), EXFAT_WCHUNK at a time.  A write up to that size is one
 * operation (O_APPEND included); a larger one is a sequence of them. */
static long exfat_write_common(exfat_file_t *h, const void *buf, long bytes,
			       uint64_t *ppos, int append)
{
	exfat_fs_t *fs = h->fs;
	unsigned long cap, done = 0;
	uint8_t *kbuf;
	long r = 0;

	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	if (h->st->is_dir)
		return ST_ISDIR;
	cap = (unsigned long)bytes < EXFAT_WCHUNK ? (unsigned long)bytes :
						     EXFAT_WCHUNK;
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
		exfat_slock_excl(fs, h->st->handle);
		exfat_meta_wlock(fs);
		r = exfat_write_impl(fs, h->st, kbuf, (long)n, ppos, append);
		if (r > 0)
			h->wrote = 1;
		exfat_meta_wunlock(fs);
		exfat_sunlock_excl(fs, h->st->handle);
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

long exfat_write_op(vfs_file_t *f, const void *buf, long bytes)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	uint64_t pos;
	long r;

	if (!h || !buf)
		return ST_INVALID;
	pos = h->pos;
	r = exfat_write_common(h, buf, bytes, &pos,
			       (f->flags & O_APPEND) != 0);
	if (r > 0)
		h->pos = (unsigned long)pos;
	return r;
}

long exfat_write_at_op(vfs_file_t *f, const void *buf, long bytes,
		       long off)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	uint64_t pos;

	if (!h || !buf || off < 0)
		return ST_INVALID;
	pos = (uint64_t)off;
	return exfat_write_common(h, buf, bytes, &pos, 0);
}

long exfat_seek_op(vfs_file_t *f, long offset, int whence)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
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
	h->pos = (unsigned long)np;
	return np;
}

/* ---- truncate ----------------------------------------------------------------- */

int exfat_truncate_op(vfs_file_t *f, unsigned long size)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	exfat_fs_t *fs;
	int r;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	if (h->st->is_dir)
		return ST_ISDIR;
	exfat_slock_excl(fs, h->st->handle);
	exfat_meta_wlock(fs);
	r = exfat_writable(fs);
	if (r == ST_OK)
		r = exfat_may_write(fs);
	if (r == ST_OK)
		r = exfat_truncate_impl(fs, h->st, size);
	if (r == ST_OK)
		r = exfat_commit(fs);
	exfat_meta_wunlock(fs);
	exfat_sunlock_excl(fs, h->st->handle);
	return r;
}

/* ---- chdir, attributes, times -------------------------------------------- */

/* Apply a mode to a stream: no write permission for anybody sets the
 * read-only attribute, any write permission clears it.  Directories keep
 * their attributes (the bit means nothing for them here).  Metadata lock
 * exclusive. */
static int exfat_chmod_stream(exfat_fs_t *fs, exfat_stream_t *st,
			      unsigned int mode)
{
	uint16_t attr;
	int r;

	exfat_assert_excl(fs);
	if (st->is_root || st->is_dir)
		return ST_OK;
	attr = st->attr;
	if (mode & 0222)
		attr &= (uint16_t)~EXFAT_ATTR_RO;
	else
		attr |= EXFAT_ATTR_RO;
	if (attr == st->attr)
		return ST_OK;
	r = exfat_writable(fs);
	if (r == ST_OK)
		r = exfat_may_write(fs);
	if (r != ST_OK)
		return r;
	st->attr = attr;
	exfat_stream_mark_dirty(fs, st);
	return exfat_commit(fs);
}

int exfat_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct exfat_walk w;
	int r;

	if (!fs || !path)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	exfat_meta_wlock(fs);
	r = exfat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = exfat_chmod_stream(fs, w.st, mode);
		exfat_walk_release(&w);
	}
	exfat_meta_wunlock(fs);
	exfat_ctx_free(ctx);
	return r;
}

int exfat_fchmod_op(vfs_file_t *f, unsigned int mode)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	int r;

	if (!h)
		return ST_INVALID;
	exfat_meta_wlock(h->fs);
	r = exfat_chmod_stream(h->fs, h->st, mode);
	exfat_meta_wunlock(h->fs);
	return r;
}

/* Set the times (exfat.h: 10 ms resolution for the modification time, 2 s
 * for the access time, UTC offset recorded).  Metadata lock exclusive. */
static int exfat_utimes_stream(exfat_fs_t *fs, exfat_stream_t *st,
			       const struct vfs_utimes *t)
{
	uint64_t now = timer_get_realtime_ns();
	int changed = 0, r;

	exfat_assert_excl(fs);
	if (st->is_root)
		return ST_OK; /* the root has no entry set to hold times */
	r = exfat_writable(fs);
	if (r == ST_OK)
		r = exfat_may_write(fs);
	if (r != ST_OK)
		return r;
	if (t->mtime_nsec != VFS_UTIME_OMIT) {
		int64_t s = t->mtime_nsec == VFS_UTIME_NOW ?
				    (int64_t)(now / 1000000000ULL) :
				    t->mtime_sec;
		long ns = t->mtime_nsec == VFS_UTIME_NOW ?
				  (long)(now % 1000000000ULL) :
				  t->mtime_nsec;

		exfat_time_encode(fs, s, ns, &st->mtime, &st->mtime_cs,
				  &st->mtime_utc);
		changed = 1;
	}
	if (t->atime_nsec != VFS_UTIME_OMIT) {
		int64_t s = t->atime_nsec == VFS_UTIME_NOW ?
				    (int64_t)(now / 1000000000ULL) :
				    t->atime_sec;
		long ns = t->atime_nsec == VFS_UTIME_NOW ?
				  (long)(now % 1000000000ULL) :
				  t->atime_nsec;

		exfat_time_encode(fs, s, ns, &st->atime, NULL, &st->atime_utc);
		changed = 1;
	}
	if (!changed)
		return ST_OK;
	exfat_stream_mark_dirty(fs, st);
	return exfat_commit(fs);
}

int exfat_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
		     const struct vfs_utimes *t)
{
	exfat_fs_t *fs = exfat_fs_of(m);
	exfat_ctx_t *ctx;
	struct exfat_walk w;
	int r;

	(void)nofollow; /* no symbolic links on exFAT */
	if (!fs || !path || !t)
		return ST_INVALID;
	ctx = exfat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	exfat_meta_wlock(fs);
	r = exfat_walk_obj(fs, ctx, path, &w);
	if (r == ST_OK) {
		r = exfat_utimes_stream(fs, w.st, t);
		exfat_walk_release(&w);
	}
	exfat_meta_wunlock(fs);
	exfat_ctx_free(ctx);
	return r;
}

int exfat_futimens_op(vfs_file_t *f, const struct vfs_utimes *t)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	int r;

	if (!h || !t)
		return ST_INVALID;
	exfat_meta_wlock(h->fs);
	r = exfat_utimes_stream(h->fs, h->st, t);
	exfat_meta_wunlock(h->fs);
	return r;
}

/* fsync: the file's data, every pending entry set, a device sync.  Any
 * write-back error of the file since the last report is returned (and
 * reported once). */
int exfat_fsync_op(vfs_file_t *f)
{
	exfat_file_t *h = f ? (exfat_file_t *)f->fs_private : NULL;
	exfat_stream_t *st;
	exfat_fs_t *fs;
	int r = ST_OK;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	st = h->st;
	for (int tries = 0;; tries++) {
		exfat_meta_wlock(fs);
		r = ST_OK;
		if (!((fs->armed || fs->may_write) && !fs->ro_latched &&
		      !fs->ro))
			break;
		if (st->data_dirty) {
			uint64_t e0 = fs->io_errors;

			pagecache_flush_file(&fs->sb, exfat_chain_of(st));
			if (fs->io_errors != e0)
				st->wb_err = ST_IO;
		}
		r = exfat_commit(fs);
		if (r == ST_OK && fs->armed)
			r = exfat_dev_sync(fs);
		/* Still dirty after the commit, without an error: pages of
		 * this file are held by a writeback pass that waits for the
		 * lock held here, so its entry set could not be stored.  Let
		 * that pass finish and try again -- fsync() returns only
		 * once all of it is on the device. */
		if (r != ST_OK || st->wb_err || !st->on_dirty_list ||
		    !st->data_dirty)
			break;
		if (tries >= EXFAT_FSYNC_TRIES) {
			r = ST_IO;
			break;
		}
		exfat_meta_wunlock(fs);
		sched_yield_in_kernel();
	}
	if (fs->ro_latched && (h->wrote || st->data_dirty))
		r = ST_IO;
	if (st->wb_err) {
		if (r == ST_OK)
			r = st->wb_err;
		st->wb_err = 0;
	}
	if (r == ST_OK && h->werr) {
		r = h->werr;
		h->werr = 0;
	}
	exfat_meta_wunlock(fs);
	return r;
}
