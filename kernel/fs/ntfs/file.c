/* LikeOS - NTFS filesystem driver
 *
 * Stream data writes, truncate, direct I/O, open/close/read/write.
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
 * S30  Stream data: delayed allocation, buffered and direct writes,
 *      truncate, commits
 *
 * A stream's in-memory state (S9) runs AHEAD of its attribute on disk:
 *
 *   write()     copies into page-cache pages and dirties them.  Clusters
 *               that are not mapped yet are only RESERVED (st->dl, a list
 *               of VCN runs; fs->resv_clusters): ENOSPC is decided here,
 *               exactly.  data_size grows in memory; initialized_size
 *               grows over what the write covered (rounded up to the page,
 *               or to data_size), the gap from the old initialized size
 *               zeroed first (through the cache, for the clusters
 *               that exist; reserved ones are zeroed when they are
 *               placed; sparse holes read as zeros anyway).
 *   writeback   calls ntfs_sb_alloc_blocks() (the alloc_blocks hook) for
 *               dirty pages without a location: the reserved runs around
 *               them are placed in one allocation (contiguous where the
 *               bitmap allows, after the file's last cluster first), the
 *               runlist in memory updated, and every block of the new
 *               clusters below initialized_size that no dirty page will
 *               cover is zeroed on the device right away (no stale
 *               cluster contents are ever exposed).  The data is written
 *               by the page cache right after the hook, under the same
 *               lock_io() hold.
 *   commit      (fsync, close, sync, unmount, truncate, every
 *               NTFS_COMMIT_EVERY placed clusters): the stream's pages are
 *               flushed (so every cluster the runlist names holds its data
 *               -- data before reference), the leftover reservations placed
 *               or dropped, and the attribute rewritten with the runlist
 *               and sizes (S27).  Until then the disk keeps the old sizes
 *               and mapping: a power cut loses the unwritten tail, it never
 *               exposes old cluster contents or a size the data does not
 *               back (nothing is written to disk before the
 *               data is there, so nothing needs a rollback).
 *
 * The page-aligned rule: whenever a dirty page exists, initialized_size
 * is page aligned or equal to data_size, so the page that straddles it --
 * a NOFILL page, which writeback cannot store -- is never dirty.  A write
 * into that page first dirties it (installed with its zero tail), then
 * moves the initialized size past it.
 *
 * Resident data ($DATA inside the record) is written through: the value
 * is rebuilt and the record committed by every write, the cached NOFILL
 * pages dropped.  A write that makes the value larger than
 * NTFS_RES_DATA_MAX converts the stream: its pages are dirtied with the
 * old value, clusters reserved, the RESIDENT flag cleared in memory; the
 * record keeps the resident value until the commit replaces it by the
 * non-resident attribute after the data reached its clusters.
 *
 * Direct I/O (O_DIRECT, 0x4000 in the open flags; the VFS passes the flags
 * through unchanged): a device-sector aligned request on a non-resident,
 * uncompressed stream whose range is mapped below initialized_size goes
 * straight to the device through the chokepoint after the stream's dirty
 * pages were flushed, and the cached copies of the range are updated in
 * place (coherent); any other request falls back to the buffered path
 * followed by a flush (O_DIRECT promises the data reached the device).
 * Compressed, encrypted and WOF streams are refused or buffered.
 *
 * Compressed streams are written one compression unit at a time
 * (ntfs_cstream_rewrite()).
 * =========================================================================== */

#define NTFS_RES_DATA_MAX(fs) ((fs)->rec_size / 2 - 96)
/* Commit a stream after this many clusters were placed for it. */
#define NTFS_COMMIT_EVERY_BYTES (16ULL * 1024 * 1024)
/* Most clusters one placement takes beyond what writeback asked for. */
#define NTFS_DA_MAX 8192
#define NTFS_O_DIRECT 0x4000

/* Keep a stream with pending state from being evicted. */
static void ntfs_stream_pin(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	if (!st->pinned) {
		ntfs_stream_ref(fs, st);
		st->pinned = 1;
	}
}

void ntfs_stream_unpin(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	if (st->pinned) {
		st->pinned = 0;
		ntfs_stream_put(fs, st);
	}
}

static inline uint64_t ntfs_clusters_of(const ntfs_fs_t *fs, uint64_t bytes)
{
	return (bytes + fs->cluster_size - 1) >> fs->cluster_bits;
}

/* Reserve every unmapped, unreserved cluster of [v0, v1). */
static int ntfs_reserve(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t v0,
			int64_t v1)
{
	uint64_t need = 0, got = 0;
	int r;

	if (v1 <= v0)
		return ST_OK;
	r = ntfs_dl_uncovered(st, v0, v1, 0, &need);
	if (r != ST_OK || !need)
		return r;
	r = ntfs_resv_get(fs, need);
	if (r != ST_OK)
		return r;
	r = ntfs_dl_uncovered(st, v0, v1, 1, &got);
	if (r != ST_OK) {
		/* Partly added: keep what is in the list, return the rest. */
		ntfs_resv_put(fs, need - got);
		st->dl_resv += got;
		return r;
	}
	st->dl_resv += need;
	st->flags |= NTFS_ST_DELALLOC;
	return ST_OK;
}

/* Drop reservations at and beyond `vcn'. */
void ntfs_unreserve_from(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t vcn)
{
	uint64_t cut;

	if (!st->dl_n)
		return;
	cut = ntfs_dl_cut(st, vcn, (int64_t)1 << 62);
	if (cut > st->dl_resv)
		cut = st->dl_resv;
	st->dl_resv -= cut;
	ntfs_resv_put(fs, cut);
}

/* ---- placement (the alloc_blocks hook and the commit) ---- */

/* Write zeros to the blocks of the new run (vcn, lcn, len) that no dirty
 * page will cover and that lie below initialized_size; blocks
 * [skip0, skip1) (bytes) are the ones writeback is about to store. */
static int ntfs_zero_new(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t vcn,
			 int64_t lcn, int64_t len, uint64_t skip0,
			 uint64_t skip1)
{
	uint64_t b0 = (uint64_t)vcn << fs->cluster_bits;
	uint64_t b1 = (uint64_t)(vcn + len) << fs->cluster_bits;
	uint64_t zs = 0, ze = 0; /* pending zero range (bytes) */
	uint8_t *zero = NULL;
	int r = ST_OK;

	if (b1 > st->init_size)
		b1 = st->init_size;
	for (uint64_t b = b0; b < b1 || ze > zs; b += fs->exp_bs) {
		int need = 0;

		if (b < b1 && !(b >= skip0 && b < skip1)) {
			pc_page_t *pg = pagecache_lookup_held(
				&fs->sb, ntfs_chain(st), b / PAGE_SIZE);

			need = 1;
			if (pg) {
				if (pg->flags & PC_PAGE_DIRTY)
					need = 0;
				pagecache_put(pg);
			}
		}
		if (need && ze == b && ze - zs < 256 * 1024) {
			ze += fs->exp_bs;
			continue;
		}
		if (ze > zs) {
			uint64_t dev = ((uint64_t)lcn << fs->cluster_bits) +
				       (zs - b0);

			if (!zero) {
				zero = (uint8_t *)ntfs_zalloc(256 * 1024 +
							       fs->exp_bs);
				if (!zero)
					return ST_NOMEM;
			}
			r = ntfs_dev_write(fs, dev >> fs->dev_ss_bits,
					   (ze - zs) >> fs->dev_ss_bits, zero);
			if (r != ST_OK)
				break;
		}
		zs = ze = 0;
		if (need) {
			zs = b;
			ze = b + fs->exp_bs;
		}
		if (b >= b1)
			break;
	}
	ntfs_free(zero);
	return r;
}

/* Place the reserved clusters of [a0, a1) (VCNs).  [skip0, skip1) are
 * the bytes writeback stores right after this.  All or nothing. */
static int ntfs_place(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t a0,
		      int64_t a1, uint64_t skip0, uint64_t skip1)
{
	ntfs_runlist_t nrl;
	ntfs_run_t *runs = NULL;
	uint32_t nruns = 0, ri = 0;
	uint64_t n = 0;
	int64_t goal = -1, used_in_run = 0;
	int r;

	ntfs_assert_meta_excl(fs);
	for (uint32_t i = 0; i < st->dl_n; i++) {
		int64_t s = st->dl[i].vcn > a0 ? st->dl[i].vcn : a0;
		int64_t e = st->dl[i].vcn + st->dl[i].len < a1 ?
				    st->dl[i].vcn + st->dl[i].len :
				    a1;

		if (e > s)
			n += (uint64_t)(e - s);
	}
	if (!n)
		return VFS_SB_ALLOC_NONE;
	/* Grow in place: after the cluster before a0. */
	if (a0 > 0 && a0 - 1 < st->rl.end_vcn) {
		int64_t lcn;

		if (ntfs_rl_map(&st->rl, a0 - 1, &lcn, NULL) == ST_OK &&
		    lcn >= 0)
			goal = lcn + 1;
	}
	if (goal < 0 && st->rl.count) {
		const ntfs_run_t *last = &st->rl.runs[st->rl.count - 1];

		if (last->lcn >= 0)
			goal = last->lcn + last->len;
	}
	if (n > st->dl_resv) {
		WARN_ON_ONCE(1);
		return ST_IO;
	}
	/* The extent that ends the file's data is its tail. */
	r = ntfs_clusters_alloc(fs, goal, n,
				NTFS_AL_RESERVED |
					((uint64_t)a1 << fs->cluster_bits >=
						 st->data_size ?
						 NTFS_AL_TAIL :
						 0),
				&runs, &nruns);
	if (r != ST_OK)
		return r;
	st->dl_resv -= n; /* the allocation consumed fs->resv_clusters */
	r = ntfs_rl_copy(&nrl, &st->rl);
	for (uint32_t i = 0; r == ST_OK && i < st->dl_n; i++) {
		int64_t s = st->dl[i].vcn > a0 ? st->dl[i].vcn : a0;
		int64_t e = st->dl[i].vcn + st->dl[i].len < a1 ?
				    st->dl[i].vcn + st->dl[i].len :
				    a1;

		while (r == ST_OK && s < e) {
			int64_t take = runs[ri].len - used_in_run;

			if (take > e - s)
				take = e - s;
			r = ntfs_rl_set(&nrl, s, take,
					runs[ri].lcn + used_in_run);
			if (r == ST_OK)
				r = ntfs_zero_new(fs, st, s,
						  runs[ri].lcn + used_in_run,
						  take, skip0, skip1);
			s += take;
			used_in_run += take;
			if (used_in_run == runs[ri].len) {
				ri++;
				used_in_run = 0;
			}
		}
	}
	if (r != ST_OK) {
		/* Nothing references the clusters: back to free, and the
		 * reservation back to the stream. */
		for (uint32_t i = 0; i < nruns; i++)
			ntfs_clusters_free(fs, runs[i].lcn, (uint64_t)runs[i].len);
		fs->resv_clusters += n;
		st->dl_resv += n;
		ntfs_sf_publish(fs);
		ntfs_rl_free(&nrl);
		ntfs_free(runs);
		return r;
	}
	ntfs_free(runs);
	ntfs_rl_free(&st->rl);
	st->rl = nrl;
	ntfs_dl_cut(st, a0, a1 - a0);
	st->alloc_size = (uint64_t)st->rl.end_vcn << fs->cluster_bits;
	if (st->flags & NTFS_ST_SPARSE)
		st->comp_size += n << fs->cluster_bits;
	else
		st->comp_size = st->alloc_size;
	st->since_commit += n;
	if (!st->pl_hi || a0 < st->pl_lo)
		st->pl_lo = a0;
	if (a1 > st->pl_hi)
		st->pl_hi = a1;
	st->flags |= NTFS_ST_META;
	ntfs_stream_pin(fs, st);
	return ST_OK;
}

/* Before a commit makes initialized_size cover clusters placed since the
 * last one: a page of them that is still dirty -- its writeback skipped
 * by the commit's flush because another writeback pass holds it, waiting
 * for the metadata lock -- is written now from the page.  Nothing
 * promises that pass comes before the record, and a cluster below
 * initialized_size must never show what it held before.  (The pass
 * writes the page again afterwards: same bytes.)  Metadata lock
 * exclusive; no writer of the stream runs. */
static int ntfs_write_placed_dirty(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint64_t b0, b1;
	int r = ST_OK;

	if (!st->pl_hi)
		return ST_OK;
	b0 = (uint64_t)st->pl_lo << fs->cluster_bits;
	b1 = (uint64_t)st->pl_hi << fs->cluster_bits;
	if (b1 > st->init_size)
		b1 = st->init_size;
	b0 &= ~((uint64_t)PAGE_SIZE - 1);
	for (uint64_t pb = b0; pb < b1 && r == ST_OK; pb += PAGE_SIZE) {
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, ntfs_chain(st),
						      pb / PAGE_SIZE);

		if (!pg)
			continue;
		if ((pg->flags & PC_PAGE_DIRTY) && pg->data) {
			for (uint64_t b = pb; b < pb + PAGE_SIZE && b < b1 &&
					      r == ST_OK;
			     b += fs->exp_bs) {
				int64_t lcn, left;

				if (ntfs_rl_map(&st->rl,
						(int64_t)(b >> fs->cluster_bits),
						&lcn, &left) != ST_OK ||
				    lcn < 0)
					continue; /* a hole: nothing to expose */
				r = ntfs_dev_write(
					fs,
					(((uint64_t)lcn << fs->cluster_bits) +
					 (b & (fs->cluster_size - 1))) >>
						fs->dev_ss_bits,
					fs->exp_bs >> fs->dev_ss_bits,
					pg->data + (b % PAGE_SIZE));
			}
		}
		pagecache_put(pg);
	}
	return r;
}

/* The alloc_blocks hook (vfs_sb.h): metadata lock exclusive, no stream
 * lock taken. */
int ntfs_sb_alloc_blocks(vfs_superblock_t *sb, unsigned long chain,
			 unsigned long first, unsigned long count)
{
	ntfs_fs_t *fs = ntfs_sb_fs(sb);
	ntfs_stream_t *st;
	uint64_t b0, b1;
	int64_t v0, v1, a0, a1;
	int r;

	if (!fs || !NTFS_BID_IS(chain))
		return VFS_SB_ALLOC_NONE;
	st = ntfs_slot(fs, NTFS_BID_HANDLE(chain));
	if (!st || !st->dl_n ||
	    (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
			  NTFS_ST_ENCRYPTED | NTFS_ST_WOF | NTFS_ST_BAD |
			  NTFS_ST_INDEX | NTFS_ST_SYSTEM)))
		return VFS_SB_ALLOC_NONE;
	ntfs_assert_meta_excl(fs);
	b0 = (uint64_t)first << fs->exp_bits;
	b1 = (uint64_t)(first + count) << fs->exp_bits;
	v0 = (int64_t)(b0 >> fs->cluster_bits);
	v1 = (int64_t)ntfs_clusters_of(fs, b1);
	/* Whole reserved runs from their start (contiguous files, and a
	 * non-sparse file never gets a gap), capped. */
	a0 = -1;
	a1 = v1;
	for (uint32_t i = 0; i < st->dl_n; i++) {
		int64_t s = st->dl[i].vcn, e = s + st->dl[i].len;

		if (e <= v0 || s >= v1)
			continue;
		if (a0 < 0 || s < a0)
			a0 = s;
		if (e > a1)
			a1 = e;
	}
	if (a0 < 0)
		return VFS_SB_ALLOC_NONE;
	if (a1 > v1 && a1 - a0 > NTFS_DA_MAX)
		a1 = (v1 > a0 + NTFS_DA_MAX) ? v1 : a0 + NTFS_DA_MAX;
	r = ntfs_place(fs, st, a0, a1, b0, b1);
	if (r < 0) {
		st->err = r;
		return r;
	}
	return r;
}

/* ---- the attribute commit ---- */

/* Write the stream's state into its attribute (see the section
 * comment).  Metadata lock exclusive; no writer of the stream runs. */
int ntfs_stream_commit(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint64_t errs = fs->io_errs, ncl;
	ntfs_eino_t *e = NULL;
	int r;

	ntfs_assert_meta_excl(fs);
	if (st->flags & (NTFS_ST_DEAD | NTFS_ST_SYSTEM | NTFS_ST_INDEX))
		return ST_OK;
	if (st->flags & NTFS_ST_DIRTY) {
		pagecache_flush_file(&fs->sb, ntfs_chain(st));
		if (fs->io_errs != errs) {
			st->err = ST_IO;
		} else if (st->err == ST_NOSPC || st->err == ST_NOMEM) {
			/* Writeback could not place some blocks: one more
			 * pass.  If that fails too, nothing is committed --
			 * dirty pages may still cover clusters below the
			 * initialized size that were never written, and
			 * committing would expose what they held. */
			st->err = ST_OK;
			pagecache_flush_file(&fs->sb, ntfs_chain(st));
			if (fs->io_errs != errs)
				st->err = ST_IO;
			if (st->err) {
				int e2 = st->err;

				if (e2 == ST_NOSPC || e2 == ST_NOMEM)
					st->err = ST_OK; /* retried later */
				return e2;
			}
		}
	}
	if (st->err)
		return st->err;
	if (!(st->flags & NTFS_ST_META)) {
		st->flags &= ~NTFS_ST_DIRTY;
		ntfs_stream_unpin(fs, st);
		return ST_OK;
	}
	if (fs->latched_ro || fs->ro)
		return ST_ROFS;
	ncl = ntfs_clusters_of(fs, st->data_size);
	/* Leftover reservations: beyond the data, dropped; inside it,
	 * placed now (zeroed below initialized_size). */
	ntfs_unreserve_from(fs, st, (int64_t)ncl);
	if (st->dl_n) {
		r = ntfs_place(fs, st, st->dl[0].vcn,
			       st->dl[st->dl_n - 1].vcn +
				       st->dl[st->dl_n - 1].len,
			       0, 0);
		if (r < 0) {
			st->err = r;
			return r;
		}
	}
	if ((uint64_t)st->rl.end_vcn < ncl) {
		if (!(st->flags & NTFS_ST_SPARSE))
			return ntfs_fs_error(fs, st->mft_no,
					     "unallocated data at commit");
		r = ntfs_rl_set(&st->rl, st->rl.end_vcn,
				(int64_t)ncl - st->rl.end_vcn, NTFS_LCN_HOLE);
		if (r != ST_OK)
			return r;
		st->alloc_size = (uint64_t)st->rl.end_vcn << fs->cluster_bits;
	}
	if (st->init_size > st->data_size)
		st->init_size = st->data_size;
	r = ntfs_write_placed_dirty(fs, st);
	if (r != ST_OK)
		goto out;
	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(st->mft_no, st->seq), &e);
	if (r != ST_OK)
		goto out;
	{
		uint16_t aflags = (st->flags & NTFS_ST_SPARSE) ?
					  NTFS_ATTR_IS_SPARSE :
					  0;

		r = ntfs_eino_set_nonres(
			e, st->type, st->name_len ? st->name : ntfs_unnamed,
			st->name_len, aflags, 0, &st->rl, st->alloc_size,
			st->data_size, st->init_size,
			(st->flags & NTFS_ST_SPARSE) ?
				ntfs_rl_allocated(&st->rl, 0, st->rl.end_vcn)
					<< fs->cluster_bits :
				0);
		if (r != ST_OK)
			goto out;
	}
	/* The file's times / attributes. */
	{
		long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL,
					0, 0);

		if (i >= 0 && !ntfs_ea_hdr(&e->a[i])->non_resident) {
			ntfs_standard_information_t *si =
				(ntfs_standard_information_t
					 *)(e->a[i].b +
					    ntfs_ea_hdr(&e->a[i])
						    ->data.resident.value_offset);

			if (st->mtime_pend) {
				si->last_data_change_time = st->mtime_pend;
				si->last_mft_change_time = st->mtime_pend;
				si->file_attributes |= NTFS_FILE_ATTR_ARCHIVE;
			}
			if ((st->flags & NTFS_ST_SPARSE) && !st->name_len)
				si->file_attributes |=
					NTFS_FILE_ATTR_SPARSE_FILE;
			if (st->old_rl.count && !st->name_len)
				si->file_attributes &=
					~NTFS_FILE_ATTR_COMPRESSED;
			e->a[i].dirty = 1;
		}
	}
	r = ntfs_eino_commit(e);
out:
	ntfs_eino_free(e);
	if (r != ST_OK) {
		st->err = r;
		return r;
	}
	/* The replaced compressed layout: unreferenced now. */
	if (st->old_rl.count && ntfs_barrier(fs) == ST_OK) {
		ntfs_rl_free_clusters(fs, &st->old_rl, 0, st->old_rl.end_vcn);
		ntfs_rl_free(&st->old_rl);
	}
	st->flags &= ~(NTFS_ST_META | NTFS_ST_WASRES | NTFS_ST_DIRTY);
	st->since_commit = 0;
	st->pl_lo = st->pl_hi = 0;
	st->mtime_pend = 0;
	ntfs_stream_unpin(fs, st);
	if (!st->name_len && st->type == NTFS_AT_DATA)
		ntfs_update_dups(fs, NTFS_MAKE_MREF(st->mft_no, st->seq));
	return ST_OK;
}

/* ---- resident data ---- */

/* Rewrite the resident value of `st' as `val' (vlen bytes) and commit. */
static int ntfs_res_commit(ntfs_fs_t *fs, ntfs_stream_t *st, uint8_t *val,
			   uint32_t vlen, int touch)
{
	ntfs_eino_t *e;
	int r;

	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(st->mft_no, st->seq), &e);
	if (r != ST_OK)
		return r;
	r = ntfs_eino_set_res(e, st->type,
			      st->name_len ? st->name : ntfs_unnamed,
			      st->name_len, 0, val, vlen);
	if (r == ST_OK && touch) {
		long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL,
					0, 0);

		if (i >= 0 && !ntfs_ea_hdr(&e->a[i])->non_resident) {
			ntfs_standard_information_t *si =
				(ntfs_standard_information_t
					 *)(e->a[i].b +
					    ntfs_ea_hdr(&e->a[i])
						    ->data.resident.value_offset);
			int64_t now = ntfs_now();

			si->last_data_change_time = now;
			si->last_mft_change_time = now;
			si->file_attributes |= NTFS_FILE_ATTR_ARCHIVE;
			e->a[i].dirty = 1;
		}
	}
	if (r == ST_OK)
		r = ntfs_eino_commit(e);
	ntfs_eino_free(e);
	if (r != ST_OK)
		return r;
	ntfs_free(st->res_data);
	st->res_data = val;
	st->res_len = vlen;
	st->data_size = st->init_size = st->alloc_size = vlen;
	pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
	if (!st->name_len && st->type == NTFS_AT_DATA)
		ntfs_update_dups(fs, NTFS_MAKE_MREF(st->mft_no, st->seq));
	return ST_OK;
}

/* Make a resident stream non-resident in memory (see the section
 * comment). */
static int ntfs_stream_unresident(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint32_t len = st->res_len;
	uint8_t *val = st->res_data;
	uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
	int r;

	/* The resident pages are NOFILL: drop them, the new ones are
	 * plain pages. */
	pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
	st->flags &= ~NTFS_ST_RESIDENT;
	st->flags |= NTFS_ST_WASRES | NTFS_ST_META;
	mm_memset(&st->rl, 0, sizeof(st->rl));
	st->alloc_size = 0;
	st->comp_size = 0;
	st->data_size = len;
	st->init_size = len;
	st->res_data = NULL;
	st->res_len = 0;
	r = ntfs_reserve(fs, st, 0, (int64_t)ntfs_clusters_of(fs, len));
	for (uint64_t p = 0; r == ST_OK && p < pages; p++) {
		pc_page_t *pg = pagecache_get_for_write(ntfs_chain(st), p, len,
							&fs->sb, ntfs_chain(st),
							1);
		uint64_t n = ntfs_min64(PAGE_SIZE, len - p * PAGE_SIZE);

		if (!pg) {
			r = ST_NOMEM;
			break;
		}
		mm_memcpy(pg->data, val + p * PAGE_SIZE, n);
		pagecache_write_end(pg);
	}
	if (r != ST_OK) {
		/* Back to resident (pages dropped, nothing on disk refers to
		 * new clusters).  Writeback the allocations above may have run
		 * placed some already: they go back to free. */
		pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
		ntfs_unreserve_from(fs, st, 0);
		if (st->rl.count) {
			if (ntfs_rl_free_clusters(fs, &st->rl, 0,
						  st->rl.end_vcn) != ST_OK)
				ntfs_fs_error(fs, st->mft_no,
					      "clusters of a failed conversion "
					      "leaked");
			ntfs_rl_free(&st->rl);
			mm_memset(&st->rl, 0, sizeof(st->rl));
		}
		st->since_commit = 0;
		st->pl_lo = st->pl_hi = 0;
		ntfs_stream_unpin(fs, st);
		st->flags |= NTFS_ST_RESIDENT;
		st->flags &= ~(NTFS_ST_WASRES | NTFS_ST_META);
		st->res_data = val;
		st->res_len = len;
		st->alloc_size = len;
		return r;
	}
	kfree(val);
	st->flags |= NTFS_ST_DIRTY;
	ntfs_stream_pin(fs, st);
	return ST_OK;
}

/* ---- compressed streams: writes per compression unit ----
 *
 * A compressed stream ($DATA with the LZNT1 flag, 16-cluster units) is
 * written unit by unit, synchronously, never through dirty pages:
 *
 *   1. the unit's current contents are read (decompressed through the
 *      page cache) unless the write covers every byte of it that is part
 *      of the file; the new bytes -- already copied from user
 *      memory before any lock was taken, so a short copy never reaches a
 *      unit -- are merged in, bytes past the end of the file are
 *      zero;
 *   2. an all-zero unit becomes a hole; otherwise it is compressed
 *      (ntfs_lznt1_compress) and stored compressed when that saves at
 *      least one cluster, else stored as it is (all 16 clusters);
 *   3. fresh clusters are allocated and the unit written into them in
 *      one pass; the clusters the unit had are only noted;
 *   4. after every unit of the request: device flush, then the attribute
 *      is rewritten (runlist, sizes, compressed size) in one commit, then
 *      another flush, then the old clusters are freed (the new data
 *      is on the device before anything refers to it, the old data stays
 *      referenced until then; a failure frees the new clusters and leaves
 *      the old mapping as it was);
 *   5. cached pages of the units show the new contents (they are never
 *      dirty: nothing is written back from them).  On failure the cached
 *      pages of the stream are dropped, so they are re-read from what the
 *      record describes.
 *
 * Sizes: the initialized size of a compressed stream equals its data
 * size afterwards.  Units holding bytes between an old initialized size
 * and the data size are rewritten (their stale bytes become zeros), as is
 * the unit that holds the old end of the file when the file grows, and
 * the unit that holds the new end when it shrinks (no stale byte past the
 * end can come back when the file grows again).  All offsets are 64-bit.
 * The whole stream is never decompressed at once: there is no size
 * limit. */

/* Is `u' one of the units this request rewrites? */
struct ntfs_cw_set {
	uint64_t a0, a1; /* written units [a0, a1], a1 < a0: none */
	uint64_t b0, b1; /* initialized-size repair */
	uint64_t c; /* the old / new end unit, ~0: none */
};

static int ntfs_cw_in(const struct ntfs_cw_set *s, uint64_t u)
{
	return (s->a1 >= s->a0 && u >= s->a0 && u <= s->a1) ||
	       (s->b1 >= s->b0 && u >= s->b0 && u <= s->b1) || u == s->c;
}

/* Rewrite the compressed stream `st' with `n' bytes of `src' at `pos'
 * (n may be 0) and the data size `nsize'.  File lock and metadata lock
 * exclusive.  Returns ST_OK or an error; nothing changed on error. */
static int ntfs_cstream_rewrite(ntfs_fs_t *fs, ntfs_stream_t *st,
				uint64_t pos, const uint8_t *src, uint64_t n,
				uint64_t nsize)
{
	uint64_t cu = st->cu_bytes, cuc = 1ULL << st->comp_unit;
	uint64_t osize = st->data_size, oinit = st->init_size;
	uint64_t nunits = (nsize + cu - 1) / cu, lo, hi;
	uint64_t ncl_new = nunits * cuc, comp;
	struct ntfs_cw_set set;
	ntfs_runlist_t nrl, oldc, newc;
	struct ntfs_lznt1_ws *ws = NULL;
	uint8_t *ubuf = NULL, *cbuf = NULL;
	ntfs_eino_t *e = NULL;
	int r = ST_OK;

	ntfs_assert_meta_excl(fs);
	mm_memset(&oldc, 0, sizeof(oldc));
	mm_memset(&newc, 0, sizeof(newc));
	mm_memset(&nrl, 0, sizeof(nrl));
	/* The units to rewrite. */
	set.a0 = 1;
	set.a1 = 0;
	if (n) {
		set.a0 = pos / cu;
		set.a1 = (pos + n - 1) / cu;
	}
	set.b0 = 1;
	set.b1 = 0;
	if (oinit < osize && oinit < nsize) {
		uint64_t top = (osize < nsize ? osize : nsize);

		set.b0 = oinit / cu;
		set.b1 = (top - 1) / cu;
	}
	set.c = ~0ULL;
	if (nsize > osize && (osize % cu))
		set.c = osize / cu; /* old end: its tail becomes file data */
	else if (nsize < osize && (nsize % cu))
		set.c = nsize / cu; /* new end: its tail must read as zero */
	lo = ~0ULL;
	hi = 0;
	if (set.a1 >= set.a0) {
		lo = set.a0;
		hi = set.a1;
	}
	if (set.b1 >= set.b0) {
		lo = set.b0 < lo ? set.b0 : lo;
		hi = set.b1 > hi ? set.b1 : hi;
	}
	if (set.c != ~0ULL) {
		lo = set.c < lo ? set.c : lo;
		hi = set.c > hi ? set.c : hi;
	}
	if (hi >= nunits)
		hi = nunits ? nunits - 1 : 0;
	/* The new runlist: the old one cut or extended (as holes) to whole
	 * units of the new size; clusters cut off are freed afterwards. */
	r = ntfs_rl_copy(&nrl, &st->rl);
	if (r != ST_OK)
		return r;
	if ((uint64_t)nrl.end_vcn > ncl_new) {
		for (uint32_t i = 0; r == ST_OK && i < nrl.count; i++) {
			const ntfs_run_t *x = &nrl.runs[i];
			int64_t s = x->vcn > (int64_t)ncl_new ? x->vcn :
								 (int64_t)ncl_new;
			int64_t e2 = x->vcn + x->len;

			if (x->lcn == NTFS_LCN_HOLE || e2 <= s)
				continue;
			r = ntfs_runs_add(&oldc.runs, &oldc.count, &oldc.cap,
					  x->lcn + (s - x->vcn), e2 - s);
		}
		if (r == ST_OK)
			r = ntfs_rl_truncate(&nrl, (int64_t)ncl_new);
	} else if ((uint64_t)nrl.end_vcn < ncl_new) {
		r = ntfs_rl_set(&nrl, nrl.end_vcn,
				(int64_t)ncl_new - nrl.end_vcn, NTFS_LCN_HOLE);
	}
	if (r != ST_OK)
		goto fail;
	ubuf = (uint8_t *)kalloc(cu);
	cbuf = (uint8_t *)kalloc(cu);
	ws = (struct ntfs_lznt1_ws *)kalloc(sizeof(*ws));
	if (!ubuf || !cbuf || !ws) {
		r = ST_NOMEM;
		goto fail;
	}
	for (uint64_t u = lo; nunits && u <= hi && r == ST_OK; u++) {
		uint64_t us = u * cu, valid, keep, nclu = 0, clen;
		int64_t uvcn = (int64_t)(u * cuc);
		const uint8_t *data = NULL;
		ntfs_run_t *runs = NULL;
		uint32_t nr = 0;

		if (!ntfs_cw_in(&set, u))
			continue;
		valid = ntfs_min64(cu, nsize - us);
		/* 1. Old contents that stay part of the file. */
		keep = 0;
		if (us < osize && us < oinit)
			keep = ntfs_min64(ntfs_min64(cu, osize - us), oinit - us);
		if (keep > valid)
			keep = valid;
		mm_memset(ubuf, 0, cu);
		if (keep && !(n && pos <= us && pos + n >= us + keep)) {
			r = ntfs_stream_pread(fs, st, us, keep, ubuf, 0);
			if (r != ST_OK)
				break;
		}
		if (n && pos < us + cu && pos + n > us) {
			uint64_t a = pos > us ? pos : us;
			uint64_t b = pos + n < us + cu ? pos + n : us + cu;

			mm_memcpy(ubuf + (a - us), src + (a - pos), b - a);
		}
		if (valid < cu)
			mm_memset(ubuf + valid, 0, cu - valid);
		/* 2. Hole, compressed or stored. */
		if (!ntfs_all_zero(ubuf, valid)) {
			clen = ntfs_lznt1_compress(ubuf, (uint32_t)valid, cbuf,
						   (uint32_t)(cu -
							      fs->cluster_size),
						   ws);
			if (clen) {
				nclu = (clen + fs->cluster_size - 1) >>
				       fs->cluster_bits;
				data = cbuf;
			} else {
				nclu = cuc;
				data = ubuf;
			}
		}
		/* The unit's old clusters: freed after the commit. */
		for (uint32_t i = 0; r == ST_OK && i < st->rl.count; i++) {
			const ntfs_run_t *x = &st->rl.runs[i];
			int64_t s = x->vcn > uvcn ? x->vcn : uvcn;
			int64_t e2 = x->vcn + x->len < uvcn + (int64_t)cuc ?
					     x->vcn + x->len :
					     uvcn + (int64_t)cuc;

			if (x->lcn == NTFS_LCN_HOLE || e2 <= s)
				continue;
			r = ntfs_runs_add(&oldc.runs, &oldc.count, &oldc.cap,
					  x->lcn + (s - x->vcn), e2 - s);
		}
		if (r == ST_OK)
			r = ntfs_rl_set(&nrl, uvcn, (int64_t)cuc, NTFS_LCN_HOLE);
		if (r != ST_OK || !nclu)
			goto cached;
		/* 3. Fresh clusters, written now. */
		{
			int64_t goal = -1, lcn;

			if (ntfs_rl_map(&st->rl, uvcn, &lcn, NULL) == ST_OK &&
			    lcn >= 0)
				goal = lcn;
			r = ntfs_clusters_alloc(fs, goal, nclu, 0, &runs, &nr);
		}
		if (r != ST_OK)
			break;
		{
			uint64_t done = 0;

			for (uint32_t i = 0; i < nr; i++) {
				int rr = ntfs_runs_add(&newc.runs, &newc.count,
						       &newc.cap, runs[i].lcn,
						       runs[i].len);

				if (rr != ST_OK) {
					/* Not noted: give it back here. */
					ntfs_clusters_free(fs, runs[i].lcn,
							   (uint64_t)runs[i].len);
					if (r == ST_OK)
						r = rr;
					continue;
				}
				if (r == ST_OK)
					r = ntfs_dev_write(
						fs,
						ntfs_byte_to_lba(
							fs,
							(uint64_t)runs[i].lcn
								<< fs->cluster_bits),
						((uint64_t)runs[i].len
						 << fs->cluster_bits) >>
							fs->dev_ss_bits,
						data + (done << fs->cluster_bits));
				if (r == ST_OK)
					r = ntfs_rl_set(&nrl,
							uvcn + (int64_t)done,
							runs[i].len,
							runs[i].lcn);
				done += (uint64_t)runs[i].len;
			}
		}
		ntfs_free(runs);
		if (r != ST_OK)
			break;
cached:
		if (r != ST_OK)
			break;
		/* 5. The cached pages of the unit show its new contents. */
		for (uint64_t p = 0; p < cu / PAGE_SIZE; p++) {
			uint64_t pidx = us / PAGE_SIZE + p;
			pc_page_t *pg;

			if (pidx * PAGE_SIZE >= osize && pidx * PAGE_SIZE >= nsize)
				break;
			pg = pagecache_lookup_held(&fs->sb, ntfs_chain(st), pidx);
			if (pg) {
				if (pg->data)
					mm_memcpy(pg->data, ubuf + p * PAGE_SIZE,
						  PAGE_SIZE);
				pagecache_put(pg);
			}
		}
	}
	if (r != ST_OK)
		goto fail;
	/* 4. Data on the device, then the attribute, then the frees. */
	r = ntfs_barrier(fs);
	if (r != ST_OK)
		goto fail;
	comp = ntfs_rl_allocated(&nrl, 0, nrl.end_vcn) << fs->cluster_bits;
	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(st->mft_no, st->seq), &e);
	if (r == ST_OK)
		r = ntfs_eino_set_nonres(
			e, st->type, st->name_len ? st->name : ntfs_unnamed,
			st->name_len,
			NTFS_ATTR_IS_COMPRESSED |
				((st->flags & NTFS_ST_SPARSE) ? NTFS_ATTR_IS_SPARSE :
								0),
			st->comp_unit, &nrl, (uint64_t)nrl.end_vcn
						     << fs->cluster_bits,
			nsize, nsize, comp);
	if (r == ST_OK) {
		ntfs_standard_information_t *si = ntfs_eino_si(e);

		if (si) {
			int64_t now = ntfs_now();

			si->last_data_change_time = now;
			si->last_mft_change_time = now;
			si->file_attributes |= NTFS_FILE_ATTR_ARCHIVE;
			if (!st->name_len)
				si->file_attributes |= NTFS_FILE_ATTR_COMPRESSED;
		}
		r = ntfs_eino_commit(e);
	}
	ntfs_eino_free(e);
	e = NULL;
	if (r != ST_OK)
		goto fail;
	/* Published. */
	ntfs_rl_free(&st->rl);
	st->rl = nrl;
	mm_memset(&nrl, 0, sizeof(nrl));
	st->data_size = st->init_size = nsize;
	st->alloc_size = (uint64_t)st->rl.end_vcn << fs->cluster_bits;
	st->comp_size = comp;
	if (nsize < osize)
		pagecache_invalidate_range(&fs->sb, ntfs_chain(st), nsize);
	if (oldc.count && ntfs_barrier(fs) == ST_OK)
		for (uint32_t i = 0; i < oldc.count; i++)
			ntfs_clusters_free(fs, oldc.runs[i].lcn,
					   (uint64_t)oldc.runs[i].len);
	ntfs_free(oldc.runs);
	ntfs_free(newc.runs);
	ntfs_free(ubuf);
	ntfs_free(cbuf);
	ntfs_free(ws);
	if (!st->name_len && st->type == NTFS_AT_DATA)
		ntfs_update_dups(fs, NTFS_MAKE_MREF(st->mft_no, st->seq));
	return ST_OK;
fail:
	/* Nothing refers to the new clusters; the cache is re-read. */
	for (uint32_t i = 0; i < newc.count; i++)
		ntfs_clusters_free(fs, newc.runs[i].lcn,
				   (uint64_t)newc.runs[i].len);
	pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
	ntfs_rl_free(&nrl);
	ntfs_free(oldc.runs);
	ntfs_free(newc.runs);
	ntfs_free(ubuf);
	ntfs_free(cbuf);
	ntfs_free(ws);
	return r;
}

/* write() on a compressed stream: whole-unit rewrites (see above). */
static long ntfs_cstream_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
			       const uint8_t *src, uint64_t n)
{
	uint64_t end = pos + n;
	int r = ntfs_cstream_rewrite(fs, st, pos, src, n,
				     end > st->data_size ? end : st->data_size);

	return r == ST_OK ? (long)n : r;
}

/* ---- buffered writes ---- */

/* Zero [from, to) of the stream through the cache, for the parts whose
 * clusters exist (see the section comment).  Pages entirely at or beyond
 * `old_init' start as zeros; the page holding `old_init' is in the cache
 * already (the caller installed and holds it).  *reached: how far the
 * zeroing got. */
static int ntfs_zero_cached(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t from,
			    uint64_t to, uint64_t old_init, uint64_t *reached)
{
	*reached = from;
	while (from < to) {
		uint64_t idx = from / PAGE_SIZE, in = from % PAGE_SIZE;
		uint64_t n = ntfs_min64(PAGE_SIZE - in, to - from);
		int64_t vcn = (int64_t)(from >> fs->cluster_bits), lcn, left;
		pc_page_t *pg;
		int mapped = vcn < st->rl.end_vcn &&
			     ntfs_rl_map(&st->rl, vcn, &lcn, &left) == ST_OK &&
			     lcn != NTFS_LCN_HOLE;

		if (!mapped) {
			/* Reserved or a hole: zeros without a page (a
			 * reserved cluster is zeroed when it is placed). */
			uint64_t next = ((uint64_t)vcn + 1) << fs->cluster_bits;

			from = next < to ? next : to;
			*reached = from;
			continue;
		}
		pg = pagecache_get_for_write(ntfs_chain(st), idx, st->data_size,
					     &fs->sb, ntfs_chain(st),
					     idx * PAGE_SIZE >= old_init);
		if (!pg)
			return ST_NOMEM;
		mm_memset(pg->data + in, 0, n);
		pagecache_write_end(pg);
		st->flags |= NTFS_ST_DIRTY;
		from += n;
		*reached = from;
	}
	return ST_OK;
}

/* Write `n' bytes of `src' (kernel memory) at `pos' of `st'.  File lock
 * and metadata lock exclusive.  Bytes written or an ST_ error. */
long ntfs_stream_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		       const uint8_t *src, uint64_t n)
{
	uint64_t end, old_init, old_size, done = 0, fin;
	pc_page_t *held = NULL;
	int64_t v0, v1;
	int r;

	ntfs_assert_meta_excl(fs);
	if (!n)
		return 0;
	if (st->flags & (NTFS_ST_ENCRYPTED | NTFS_ST_WOF))
		return ST_UNSUPPORTED;
	if (st->flags & NTFS_ST_BAD)
		return ST_IO;
	if ((st->flags & (NTFS_ST_INDEX | NTFS_ST_SYSTEM | NTFS_ST_PROTECTED)) ||
	    (st->mft_no < NTFS_FILE_FIRST_USER &&
	     st->mft_no != NTFS_FILE_ROOT))
		return ST_PERM; /* the volume's own metadata */
	if (!ntfs_add_ok(pos, n, &end) || end > (1ULL << 62))
		return ST_RANGE;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	if (st->flags & NTFS_ST_COMPRESSED)
		return ntfs_cstream_write(fs, st, pos, src, n);
	if (st->flags & NTFS_ST_RESIDENT) {
		uint64_t nsz = end > st->data_size ? end : st->data_size;

		if (nsz <= NTFS_RES_DATA_MAX(fs)) {
			uint8_t *val = (uint8_t *)ntfs_zalloc(nsz ? nsz : 1);

			if (!val)
				return ST_NOMEM;
			if (st->res_len)
				mm_memcpy(val, st->res_data, st->res_len);
			mm_memcpy(val + pos, src, n);
			r = ntfs_res_commit(fs, st, val, (uint32_t)nsz, 1);
			if (r != ST_OK) {
				kfree(val);
				return r;
			}
			return (long)n;
		}
		r = ntfs_stream_unresident(fs, st);
		if (r != ST_OK)
			return r;
	}
	old_init = st->init_size;
	old_size = st->data_size;
	/* Reserve: the gap of a non-sparse file and the written range. */
	v0 = (int64_t)(pos >> fs->cluster_bits);
	v1 = (int64_t)ntfs_clusters_of(fs, end);
	if (!(st->flags & NTFS_ST_SPARSE) && st->rl.end_vcn < v0)
		v0 = st->rl.end_vcn;
	r = ntfs_reserve(fs, st, v0, v1);
	if (r == ST_NOSPC && end - pos > fs->cluster_size && fs->bm_valid) {
		/* A short write up to what fits. */
		uint64_t room = fs->free_clusters > fs->resv_clusters +
							    NTFS_META_FLOOR ?
					fs->free_clusters - fs->resv_clusters -
						NTFS_META_FLOOR :
					0;
		/* `room' more clusters from v0 on (the gap first) */
		uint64_t cut = (uint64_t)v0 + room < (uint64_t)v1 ?
				       ((uint64_t)v0 + room) << fs->cluster_bits :
				       0;

		if (room && cut > pos && cut < end) {
			end = cut;
			n = end - pos;
			v1 = (int64_t)ntfs_clusters_of(fs, end);
			r = ntfs_reserve(fs, st, v0, v1);
		}
	}
	if (r != ST_OK)
		return r;
	/* The page that holds the old initialized size is put in the cache
	 * (with its zero tail) and held BEFORE any size moves: afterwards it
	 * could be read from the device with the old bytes past that size. */
	if ((old_init & (PAGE_SIZE - 1)) &&
	    (end > old_init ||
	     (old_init < old_size && pos / PAGE_SIZE <= old_init / PAGE_SIZE &&
	      (end - 1) / PAGE_SIZE >= old_init / PAGE_SIZE))) {
		r = ntfs_stream_page(fs, st, old_init / PAGE_SIZE, &held);
		if (r != ST_OK) {
			held = NULL;
			goto fail;
		}
	}
	if (end > st->data_size)
		st->data_size = end;
	/* The initialized size moves to where this write will leave it
	 * BEFORE any page is dirtied: a writeback nested in the copy (page
	 * reclaim) may place reserved clusters of the range, and it zeroes
	 * what no dirty page covers below the initialized size -- with the
	 * old size it would leave stale cluster contents under the new one.
	 * The page holding the old size is dirtied here when the write
	 * touches it, so the size covers that page too (page-aligned rule). */
	{
		uint64_t want = end;
		uint64_t is;

		if (held)
			want = want > old_init ? want :
						 (old_init + PAGE_SIZE - 1) &
							 ~((uint64_t)PAGE_SIZE - 1);
		is = (want + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
		if (is > st->data_size)
			is = st->data_size;
		if (is > st->init_size)
			st->init_size = is;
	}
	fin = pos;
	if (pos > old_init) {
		uint64_t reached;

		/* The gap from the old initialized size. */
		r = ntfs_zero_cached(fs, st, old_init, pos, old_init, &reached);
		if (r != ST_OK) {
			fin = reached;
			goto settle;
		}
	}
	while (done < n) {
		uint64_t p = pos + done;
		uint64_t idx = p / PAGE_SIZE, in = p % PAGE_SIZE;
		uint64_t c = ntfs_min64(PAGE_SIZE - in, n - done);
		int full = (in == 0 && c == PAGE_SIZE) ||
			   idx * PAGE_SIZE >= old_init;
		pc_page_t *pg = pagecache_get_for_write(ntfs_chain(st), idx,
							st->data_size, &fs->sb,
							ntfs_chain(st), full);

		if (!pg) {
			r = ST_NOMEM;
			break;
		}
		mm_memcpy(pg->data + in, src + done, c);
		pagecache_write_end(pg);
		done += c;
	}
	fin = pos + done;
	if (done)
		st->flags |= NTFS_ST_DIRTY;
settle:
	/* Sizes: data_size ends where the data (or the zeroed gap) does;
	 * initialized_size covers it, page aligned or at data_size (the
	 * page-aligned rule). */
	{
		uint64_t ds = fin > old_size ? fin : old_size;
		uint64_t is = (fin + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

		st->data_size = ds;
		if (is > ds)
			is = ds;
		if (fin <= old_init)
			is = old_init;
		/* The page that holds the old initialized size was dirtied
		 * (with zeros past that size): the size moves to its end, so
		 * that the page is a plain one writeback can store. */
		if (held && fin > (old_init & ~((uint64_t)PAGE_SIZE - 1)) &&
		    (done || pos > old_init)) {
			uint64_t pe = (old_init + PAGE_SIZE - 1) &
				      ~((uint64_t)PAGE_SIZE - 1);

			if (is < pe)
				is = pe;
			if (is > ds)
				is = ds;
		}
		st->init_size = is > old_init ? is : old_init;
		if (st->init_size > st->data_size)
			st->init_size = st->data_size;
	}
	if (held)
		pagecache_put(held);
	ntfs_unreserve_from(fs, st, (int64_t)ntfs_clusters_of(fs, st->data_size));
	if (st->data_size != old_size || done)
		st->flags |= NTFS_ST_META;
	if (st->flags & (NTFS_ST_META | NTFS_ST_DIRTY))
		ntfs_stream_pin(fs, st);
	if (done) {
		st->mtime_pend = ntfs_now();
		return (long)done;
	}
	return r != ST_OK ? r : ST_NOMEM;
fail:
	if (held)
		pagecache_put(held);
	ntfs_unreserve_from(fs, st, (int64_t)ntfs_clusters_of(fs, st->data_size));
	return r;
}

/* ---- truncate ---- */

/* Set the stream's size.  File lock and metadata lock exclusive. */
int ntfs_stream_truncate(ntfs_fs_t *fs, ntfs_stream_t *st,
			 uint64_t size)
{
	ntfs_runlist_t nrl, freed;
	uint64_t ncl, old_size = st->data_size, old_init = st->init_size;
	ntfs_eino_t *e = NULL;
	int r;

	ntfs_assert_meta_excl(fs);
	if (size == st->data_size)
		return ST_OK; /* nothing changes: no write at all */
	if (st->flags & (NTFS_ST_ENCRYPTED | NTFS_ST_WOF))
		return ST_UNSUPPORTED;
	if ((st->flags & (NTFS_ST_INDEX | NTFS_ST_SYSTEM | NTFS_ST_PROTECTED)) ||
	    (st->mft_no < NTFS_FILE_FIRST_USER &&
	     st->mft_no != NTFS_FILE_ROOT))
		return ST_PERM; /* the volume's own metadata */
	if (st->flags & (NTFS_ST_BAD))
		return ST_IO;
	if (st->flags & NTFS_ST_COMPRESSED) {
		if (size > (1ULL << 62))
			return ST_RANGE;
		r = ntfs_write_begin(fs);
		if (r == ST_OK)
			r = ntfs_cstream_rewrite(fs, st, 0, NULL, 0, size);
		return r;
	}
	if (size > (1ULL << 62))
		return ST_RANGE;
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	if (st->flags & NTFS_ST_RESIDENT) {
		if (size <= NTFS_RES_DATA_MAX(fs)) {
			uint8_t *val = (uint8_t *)ntfs_zalloc(size ? size : 1);

			if (!val)
				return ST_NOMEM;
			mm_memcpy(val, st->res_data,
				  ntfs_min64(size, st->res_len));
			r = ntfs_res_commit(fs, st, val, (uint32_t)size, 1);
			if (r != ST_OK)
				kfree(val);
			return r;
		}
		r = ntfs_stream_unresident(fs, st);
		if (r != ST_OK)
			return r;
		old_size = st->data_size;
		old_init = st->init_size;
	}
	/* Everything pending reaches the device first: the commit below
	 * describes only data that is there (nothing is dropped before
	 * the new size is durable). */
	r = ntfs_stream_commit(fs, st);
	if (r != ST_OK)
		return r;
	ncl = ntfs_clusters_of(fs, size);
	mm_memset(&freed, 0, sizeof(freed));
	r = ntfs_rl_copy(&nrl, &st->rl);
	if (r != ST_OK)
		return r;
	if (size < old_size) {
		/* The clusters beyond the new end, for freeing after. */
		for (uint32_t i = 0; r == ST_OK && i < nrl.count; i++) {
			const ntfs_run_t *x = &nrl.runs[i];
			int64_t s = x->vcn > (int64_t)ncl ? x->vcn : (int64_t)ncl;
			int64_t xe = x->vcn + x->len;

			if (xe > s && x->lcn != NTFS_LCN_HOLE)
				r = ntfs_runs_add(&freed.runs, &freed.count,
						  &freed.cap,
						  x->lcn + (s - x->vcn), xe - s);
		}
		if (r == ST_OK)
			r = ntfs_rl_truncate(&nrl, (int64_t)ncl);
	} else if ((st->flags & NTFS_ST_SPARSE) || fs->opt.sparse) {
		/* Sparse extension: a hole. */
		if ((int64_t)ncl > nrl.end_vcn)
			r = ntfs_rl_set(&nrl, nrl.end_vcn,
					(int64_t)ncl - nrl.end_vcn,
					NTFS_LCN_HOLE);
	} else if ((int64_t)ncl > nrl.end_vcn) {
		/* Allocated now, beyond the initialized size (never read). */
		ntfs_run_t *runs = NULL;
		uint32_t nr = 0;
		int64_t goal = -1;

		if (nrl.count && nrl.runs[nrl.count - 1].lcn >= 0)
			goal = nrl.runs[nrl.count - 1].lcn +
			       nrl.runs[nrl.count - 1].len;
		r = ntfs_clusters_alloc(fs, goal,
					ncl - (uint64_t)nrl.end_vcn, 0, &runs,
					&nr);
		for (uint32_t i = 0; r == ST_OK && i < nr; i++)
			r = ntfs_rl_append(&nrl, nrl.end_vcn, runs[i].lcn,
					   runs[i].len);
		if (r != ST_OK)
			for (uint32_t i = 0; i < nr; i++)
				ntfs_clusters_free(fs, runs[i].lcn,
						   (uint64_t)runs[i].len);
		else
			for (uint32_t i = 0; i < nr; i++) {
				r = ntfs_runs_add(&freed.runs, &freed.count,
						  &freed.cap, runs[i].lcn,
						  runs[i].len); /* undo list */
				if (r != ST_OK) {
					/* not on the undo list: back now */
					for (uint32_t j = i; j < nr; j++)
						ntfs_clusters_free(
							fs, runs[j].lcn,
							(uint64_t)runs[j].len);
					break;
				}
			}
		ntfs_free(runs);
	}
	if (r != ST_OK)
		goto fail;
	/* Commit the new attribute. */
	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(st->mft_no, st->seq), &e);
	if (r == ST_OK) {
		int sparse = (st->flags & NTFS_ST_SPARSE) ||
			     (size > old_size && fs->opt.sparse);
		uint64_t init = old_init < size ? old_init : size;
		uint64_t alloc = (uint64_t)nrl.end_vcn << fs->cluster_bits;

		r = ntfs_eino_set_nonres(
			e, st->type, st->name_len ? st->name : ntfs_unnamed,
			st->name_len, sparse ? NTFS_ATTR_IS_SPARSE : 0, 0, &nrl,
			alloc, size, init,
			sparse ? ntfs_rl_allocated(&nrl, 0, nrl.end_vcn)
						<< fs->cluster_bits :
				 0);
		if (r == ST_OK) {
			long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION,
						NULL, 0, 0);

			if (i >= 0 && !ntfs_ea_hdr(&e->a[i])->non_resident) {
				ntfs_standard_information_t *si =
					(ntfs_standard_information_t
						 *)(e->a[i].b +
						    ntfs_ea_hdr(&e->a[i])
							    ->data.resident
							    .value_offset);
				int64_t now = ntfs_now();

				si->last_data_change_time = now;
				si->last_mft_change_time = now;
				if (sparse && !st->name_len)
					si->file_attributes |=
						NTFS_FILE_ATTR_SPARSE_FILE;
				e->a[i].dirty = 1;
			}
		}
		if (r == ST_OK)
			r = ntfs_eino_commit(e);
		ntfs_eino_free(e);
		if (r == ST_OK && sparse)
			st->flags |= NTFS_ST_SPARSE;
	}
	if (r != ST_OK)
		goto fail;
	/* The new state in memory; the cache follows. */
	if (size < old_size) {
		pc_page_t *pg;

		pagecache_invalidate_range(&fs->sb, ntfs_chain(st), size);
		/* The cached tail of the last page reads as
		 * zeros (it may be extended again later). */
		if (size & (PAGE_SIZE - 1)) {
			pg = pagecache_lookup_held(&fs->sb, ntfs_chain(st),
						   size / PAGE_SIZE);
			if (pg) {
				mm_memset(pg->data + (size & (PAGE_SIZE - 1)),
					  0, PAGE_SIZE - (size & (PAGE_SIZE - 1)));
				pagecache_put(pg);
			}
		}
		ntfs_unreserve_from(fs, st, (int64_t)ncl);
	}
	ntfs_rl_free(&st->rl);
	st->rl = nrl;
	st->data_size = size;
	if (st->init_size > size)
		st->init_size = size;
	st->alloc_size = (uint64_t)nrl.end_vcn << fs->cluster_bits;
	st->comp_size = (st->flags & NTFS_ST_SPARSE) ?
				ntfs_rl_allocated(&nrl, 0, nrl.end_vcn)
					<< fs->cluster_bits :
				st->alloc_size;
	if (size > old_size) {
		ntfs_free(freed.runs);
	} else {
		/* Dereferenced on the device: free (after a barrier). */
		if (freed.count && ntfs_barrier(fs) == ST_OK)
			for (uint32_t i = 0; i < freed.count; i++)
				ntfs_clusters_free(fs, freed.runs[i].lcn,
						   (uint64_t)freed.runs[i].len);
		ntfs_free(freed.runs);
	}
	if (!st->name_len && st->type == NTFS_AT_DATA)
		ntfs_update_dups(fs, NTFS_MAKE_MREF(st->mft_no, st->seq));
	return ST_OK;
fail:
	if (size > old_size)
		for (uint32_t i = 0; i < freed.count; i++)
			ntfs_clusters_free(fs, freed.runs[i].lcn,
					   (uint64_t)freed.runs[i].len);
	ntfs_free(freed.runs);
	ntfs_rl_free(&nrl);
	return r;
}

/* ---- direct I/O ---- */

/* Is [pos, pos + n) of `st' plain mapped data below initialized_size,
 * device-sector aligned? */
static int ntfs_dio_ok(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		       uint64_t n)
{
	uint64_t end;

	if ((pos | n) & (fs->dev_ss - 1) || !n)
		return 0;
	if (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
			 NTFS_ST_ENCRYPTED | NTFS_ST_WOF | NTFS_ST_BAD |
			 NTFS_ST_INDEX | NTFS_ST_SYSTEM))
		return 0;
	if (!ntfs_add_ok(pos, n, &end) || end > st->init_size)
		return 0;
	for (uint64_t p = pos; p < end;) {
		int64_t lcn, left;

		if (ntfs_rl_map(&st->rl, (int64_t)(p >> fs->cluster_bits), &lcn,
				&left) != ST_OK ||
		    lcn == NTFS_LCN_HOLE)
			return 0;
		p = ((p >> fs->cluster_bits) + (uint64_t)left)
		    << fs->cluster_bits;
	}
	return 1;
}

/* Direct write of `n' bytes (kernel bounce) at `pos', mapped data only. */
static long ntfs_dio_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
			   const uint8_t *src, uint64_t n)
{
	uint64_t errs = fs->io_errs;
	int r;

	if ((st->flags & (NTFS_ST_INDEX | NTFS_ST_SYSTEM | NTFS_ST_PROTECTED)) ||
	    (st->mft_no < NTFS_FILE_FIRST_USER &&
	     st->mft_no != NTFS_FILE_ROOT))
		return ST_PERM; /* the volume's own metadata */
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		return r;
	/* Dirty pages of the stream reach the device first, so nothing
	 * older overwrites the direct data later. */
	pagecache_flush_file(&fs->sb, ntfs_chain(st));
	if (fs->io_errs != errs)
		return ST_IO;
	r = ntfs_rl_pwrite_raw(fs, &st->rl, pos, src, n);
	if (r != ST_OK)
		return r;
	/* Cached copies follow (coherent). */
	for (uint64_t p = pos; p < pos + n;) {
		uint64_t idx = p / PAGE_SIZE, in = p % PAGE_SIZE;
		uint64_t c = ntfs_min64(PAGE_SIZE - in, pos + n - p);
		pc_page_t *pg = pagecache_lookup_held(&fs->sb, ntfs_chain(st),
						      idx);

		if (pg) {
			mm_memcpy(pg->data + in, src + (p - pos), c);
			pagecache_put(pg);
		}
		p += c;
	}
	st->mtime_pend = ntfs_now();
	st->flags |= NTFS_ST_META;
	ntfs_stream_pin(fs, st);
	return (long)n;
}

/* Direct read into a kernel bounce: flush, then the device. */
static int ntfs_dio_read(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
			 uint8_t *dst, uint64_t n)
{
	return ntfs_rl_pread(fs, &st->rl, pos, n, st->init_size, dst);
}

/* ===========================================================================
 * S21  VFS operations
 *
 * The read side and the entry points of the write side (S32 holds
 * the write-specific ones).  Path operations that only read take the
 * metadata lock SHARED for their whole duration; reads take the file's
 * content lock SHARED and the metadata lock SHARED to collect held pages,
 * then copy to user memory with no lock held.  Changes take the file
 * lock EXCLUSIVE (data) and the metadata lock EXCLUSIVE (S32).  An
 * O_CREAT open takes the metadata lock exclusive; O_TRUNC truncates after
 * the open (an empty file is left alone).
 * =========================================================================== */

#define NTFS_O_ACCMODE 3
#define NTFS_O_NOFOLLOW 0x20000
#define NTFS_O_DIRECTORY 0x10000

/* Should a directory entry be listed?  (DOS-only names never, system
 * files and $MFT per the options, dot / hidden files per the options.) */
int ntfs_entry_listed(ntfs_fs_t *fs, uint64_t dir_no,
		      const ntfs_index_entry_t *e)
{
	const ntfs_file_name_t *fn = ntfs_ie_fn(e);
	uint64_t no = NTFS_MREF_NO(e->data.indexed_file);
	const uint16_t *nm = ntfs_fn_name(fn);

	if (fn->file_name_type == NTFS_FILE_NAME_DOS)
		return 0;
	if (no == dir_no)
		return 0; /* the root's own "." entry */
	if (no == NTFS_FILE_MFT)
		return 0;
	if (no < NTFS_FILE_FIRST_USER && !fs->opt.show_sys)
		return 0;
	if (fs->opt.hide_dot && ntfs_le16(&nm[0]) == '.')
		return 0;
	if (fs->opt.nohidden && (fn->file_attributes & NTFS_FILE_ATTR_HIDDEN))
		return 0;
	if (fs->opt.streams == NTFS_STREAMS_WINDOWS) {
		for (uint32_t i = 0; i < fn->file_name_length; i++)
			if (ntfs_le16(&nm[i]) == ':')
				return 0;
	}
	return 1;
}

/* ===========================================================================
 * S32  Write-side VFS operations, sync, the clean-unmount sequence
 *
 * Locking: data changes take the file's content lock EXCLUSIVE, then the
 * metadata lock EXCLUSIVE (ntfs.h section 4); namespace and attribute
 * changes take the metadata lock exclusive alone (the data blocks of a
 * file are freed only when it is not open, so no reader can be inside
 * them).  User memory is never touched while either lock is held: write()
 * copies its source into a kernel buffer first, chunk by chunk (64 KB),
 * and read()/readdir() copy out after unlocking.
 *
 * sync: every pending stream committed, the device flushed; the
 * dirty flag is never cleared here.  Clean unmount / remount read-only:
 * everything committed and flushed, and only when no error was recorded
 * and every step succeeded, VOLUME_IS_DIRTY is cleared and the device
 * flushed again.  A failure leaves the flag set (chkdsk
 * runs next time) and makes a remount read-only fail (the mount stays
 * read-write; unmount retries).
 * =========================================================================== */

#define NTFS_WCHUNK (64UL * 1024)
#define NTFS_O_APPEND 0x0400

void ntfs_wlock_file(ntfs_fs_t *fs, uint64_t no)
{
	ntfs_flock_excl(fs, no);
	ntfs_meta_wlock(fs);
}

void ntfs_wunlock_file(ntfs_fs_t *fs, uint64_t no)
{
	ntfs_meta_wunlock(fs);
	ntfs_funlock_excl(fs, no);
}

/* Commit `st' if it has pending state (file lock + metadata lock taken
 * here). */
int ntfs_commit_locked(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	int r = ST_OK;

	ntfs_wlock_file(fs, st->mft_no);
	if (st->flags & (NTFS_ST_META | NTFS_ST_DIRTY) || st->err)
		r = ntfs_stream_commit(fs, st);
	if (r == ST_OK && st->err) {
		r = st->err;
	}
	ntfs_wunlock_file(fs, st->mft_no);
	return r;
}

/* Commit every stream with pending state (sync, unmount). */
int ntfs_commit_all(ntfs_fs_t *fs)
{
	int r = ST_OK;

	for (uint32_t h = NTFS_HANDLE_FIRST_DYNAMIC; h < NTFS_SLOTS; h++) {
		ntfs_stream_t *st;
		uint64_t fl;
		int rr;

		spin_lock_irqsave(&fs->hdl_lock, &fl);
		st = ntfs_slot(fs, h);
		if (st && (st->flags & (NTFS_ST_META | NTFS_ST_DIRTY)) &&
		    !(st->flags & NTFS_ST_DEAD)) {
			if (st->refs++ == 0)
				ntfs_lru_del(fs, st);
		} else {
			st = NULL;
		}
		spin_unlock_irqrestore(&fs->hdl_lock, fl);
		if (!st)
			continue;
		rr = ntfs_commit_locked(fs, st);
		if (rr != ST_OK && r == ST_OK)
			r = rr;
		ntfs_stream_put(fs, st);
	}
	return r;
}

/* The common write: `bytes' of user memory `ubuf' at *pos (append: at
 * the end).  Returns bytes written or an ST_ error. */
static long ntfs_write_common(ntfs_file_t *nf, const void *ubuf, long bytes,
			      uint64_t *pos, int append, int direct)
{
	ntfs_fs_t *fs = nf->fs;
	ntfs_stream_t *st = nf->st;
	uint64_t no = NTFS_MREF_NO(nf->mref), done = 0;
	uint8_t *kbuf;
	long ret = 0;

	if (nf->is_dir)
		return ST_ISDIR;
	if (!ubuf || bytes < 0)
		return ST_INVALID;
	if (!bytes)
		return 0;
	if (!st)
		return ST_IO;
	if (!ntfs_writable(fs))
		return ST_ROFS;
	kbuf = (uint8_t *)kalloc(NTFS_WCHUNK);
	if (!kbuf)
		return ST_NOMEM;
	while (done < (uint64_t)bytes) {
		uint64_t c = ntfs_min64(NTFS_WCHUNK, (uint64_t)bytes - done);
		uint64_t at;
		long w;

		/* The source into the kernel first (no lock held). */
		if (copy_from_user(kbuf, (const uint8_t *)ubuf + done, c)) {
			ret = done ? 0 : ST_INVALID;
			break;
		}
		ntfs_wlock_file(fs, no);
		at = append ? st->data_size : *pos + done;
		if (direct && ntfs_dio_ok(fs, st, at, c))
			w = ntfs_dio_write(fs, st, at, kbuf, c);
		else
			w = ntfs_stream_write(fs, st, at, kbuf, c);
		if (w > 0 && direct && !(st->flags & NTFS_ST_RESIDENT)) {
			/* O_DIRECT: on the device when write() returns. */
			int cr = ntfs_stream_commit(fs, st);

			if (cr != ST_OK)
				w = cr;
		} else if (w > 0 && (st->since_commit << fs->cluster_bits) >=
					    NTFS_COMMIT_EVERY_BYTES) {
			ntfs_stream_commit(fs, st);
		}
		if (w > 0) {
			if (nf->inode)
				icache_update_size((ic_inode_t *)nf->inode,
						   st->data_size);
			pagecache_balance_dirty(&fs->sb);
		}
		ntfs_wunlock_file(fs, no);
		if (w < 0) {
			ret = w;
			break;
		}
		done += (uint64_t)w;
		if ((uint64_t)w < c)
			break;
	}
	kfree(kbuf);
	return done ? (long)done : ret;
}

long ntfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	ntfs_file_t *nf = ntfs_nf(f);
	uint64_t pos;
	long r;
	int append;

	if (!nf)
		return ST_INVALID;
	if (!file_may_write(f))
		return ST_ACCESS; /* not open for writing */
	append = (f->flags & NTFS_O_APPEND) != 0;
	pos = nf->pos;
	r = ntfs_write_common(nf, buf, bytes, &pos, append,
			      (f->flags & NTFS_O_DIRECT) != 0);
	if (r > 0) {
		if (append)
			nf->pos = nf->st ? (unsigned long)nf->st->data_size :
					   nf->pos;
		else
			nf->pos += (unsigned long)r;
	}
	return r;
}

long ntfs_write_at(vfs_file_t *f, const void *buf, long bytes,
		   long off)
{
	ntfs_file_t *nf = ntfs_nf(f);
	uint64_t pos;

	if (!nf || off < 0)
		return ST_INVALID;
	if (!file_may_write(f))
		return ST_ACCESS;
	pos = (uint64_t)off;
	return ntfs_write_common(nf, buf, bytes, &pos,
				 (f->flags & NTFS_O_APPEND) != 0,
				 (f->flags & NTFS_O_DIRECT) != 0);
}

int ntfs_truncate(vfs_file_t *f, unsigned long size)
{
	ntfs_file_t *nf = ntfs_nf(f);
	ntfs_fs_t *fs;
	int r;

	if (!nf)
		return ST_INVALID;
	if (nf->is_dir)
		return ST_ISDIR;
	fs = nf->fs;
	if (!nf->st)
		return ST_IO;
	if (!ntfs_writable(fs))
		return ST_ROFS;
	ntfs_wlock_file(fs, NTFS_MREF_NO(nf->mref));
	r = ntfs_stream_truncate(fs, nf->st, size);
	if (r == ST_OK && nf->inode)
		icache_update_size((ic_inode_t *)nf->inode, nf->st->data_size);
	ntfs_wunlock_file(fs, NTFS_MREF_NO(nf->mref));
	return r;
}

/* ---- open / close ---- */

static int ntfs_open_impl(ntfs_fs_t *fs, const char *path, int flags,
			  unsigned int cmode, int *created, vfs_file_t **out)
{
	ntfs_res_t *res = NULL;
	ntfs_inode_t *ino = NULL;
	ntfs_file_t *nf = NULL;
	ntfs_stream_t *st = NULL;
	int r, sg = 0;

	*out = NULL;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!res || !ino) {
		r = ST_NOMEM;
		goto fail;
	}
	r = ntfs_resolve(fs, path,
			 !(flags & NTFS_O_NOFOLLOW) &&
				 !((flags & O_CREAT) && (flags & O_EXCL)),
			 res);
	if (r == ST_NOT_FOUND && (flags & O_CREAT) && res->last_missing) {
		/* Create it (the caller holds the metadata lock exclusive
		 * for an O_CREAT open). */
		uint16_t *u = (uint16_t *)kalloc(2U * NTFS_MAX_NAME_LEN);
		uint32_t ulen = 0;
		uint8_t ntype = 0;
		uint64_t mref = 0;
		ntfs_mkargs_t mk;

		if (!u) {
			r = ST_NOMEM;
			goto fail;
		}
		if (!ntfs_meta_held_excl(fs) || !ntfs_writable(fs)) {
			kfree(u);
			r = ST_ROFS;
			goto fail;
		}
		if (flags & NTFS_O_DIRECTORY) {
			kfree(u);
			r = ST_INVALID;
			goto fail;
		}
		r = ntfs_new_name(fs, res->lname, res->llen, u, &ulen, &ntype);
		mk.kind = NTFS_MK_FILE;
		mk.mode = S_IFREG | (cmode & 07777);
		mk.target = NULL;
		mk.dev = 0;
		if (r == ST_OK)
			r = ntfs_create_in(fs, res->missing_dir, u, ulen, ntype,
					   &mk, &mref);
		kfree(u);
		if (r != ST_OK)
			goto fail;
		ntfs_dc_forget(fs, res->missing_dir, res->lname, res->llen);
		res->mref = mref;
		res->parent = res->missing_dir;
		res->is_stream = 0;
		*created = 1;
	} else if (r != ST_OK) {
		goto fail;
	} else if ((flags & O_CREAT) && (flags & O_EXCL) && !res->is_stream) {
		r = ST_EXISTS; /* a stream's own existence is checked below */
		goto fail;
	}
	if (((flags & NTFS_O_ACCMODE) == 1 || (flags & NTFS_O_ACCMODE) == 2 ||
	     (flags & O_TRUNC)) &&
	    !ntfs_writable(fs)) {
		r = ST_ROFS;
		goto fail;
	}
	if (((flags & NTFS_O_ACCMODE) == 1 || (flags & NTFS_O_ACCMODE) == 2 ||
	     (flags & O_TRUNC)) &&
	    ntfs_protected(fs, res->mref)) {
		r = ST_PERM; /* the volume's own metadata */
		goto fail;
	}
	r = ntfs_inode_load(fs, res->mref, ino);
	if (r != ST_OK)
		goto fail;
	if ((flags & NTFS_O_NOFOLLOW) && ntfs_inode_is_link(ino) &&
	    !res->is_stream) {
		r = ST_LOOP;
		goto fail;
	}
	if ((flags & NTFS_O_DIRECTORY) &&
	    (res->is_stream || !ino->is_dir || ntfs_inode_is_link(ino))) {
		r = ST_NOTDIR;
		goto fail;
	}
	if (ino->is_dir && !res->is_stream &&
	    ((flags & NTFS_O_ACCMODE) == 1 || (flags & NTFS_O_ACCMODE) == 2 ||
	     (flags & O_TRUNC))) {
		r = ST_ISDIR;
		goto fail;
	}
	nf = (ntfs_file_t *)ntfs_zalloc(sizeof(*nf));
	if (!nf) {
		r = ST_NOMEM;
		goto fail;
	}
	nf->fs = fs;
	nf->mref = ntfs_ino_mref(ino);
	if (ino->wof_format != ~0U ||
	    (ino->has_reparse && ino->reparse_tag == NTFS_IO_REPARSE_TAG_WOF))
		sg |= NTFS_SG_WOF;
	if (ino->wof_format != ~0U)
		sg |= NTFS_SG_WOF_FMT(ino->wof_format);
	if (ino->fattr & NTFS_FILE_ATTR_ENCRYPTED)
		sg |= NTFS_SG_ENCRYPTED;
	if (res->is_stream) {
		/* With streams_interface=windows stream names match
		 * ignoring case; otherwise exactly. */
		if (fs->opt.streams == NTFS_STREAMS_WINDOWS)
			sg |= NTFS_SG_IC;
		r = ntfs_stream_get(fs, nf->mref, NTFS_AT_DATA, res->sname,
				    res->snlen, sg & ~NTFS_SG_WOF, &st);
		if (r == ST_OK && (flags & O_CREAT) && (flags & O_EXCL)) {
			ntfs_stream_put(fs, st);
			st = NULL;
			r = ST_EXISTS;
		} else if (r == ST_NOT_FOUND && (flags & O_CREAT)) {
			/* Create the stream (the base file must exist:
			 * creating a stream never creates its file). */
			if (!ntfs_meta_held_excl(fs) || !ntfs_writable(fs))
				r = ST_ROFS;
			else
				r = ntfs_stream_create(fs, nf->mref,
						       res->sname,
						       res->snlen);
			if (r == ST_OK) {
				*created = 1;
				r = ntfs_stream_get(fs, nf->mref,
						    NTFS_AT_DATA, res->sname,
						    res->snlen,
						    sg & ~NTFS_SG_WOF, &st);
			}
		}
		if (r != ST_OK)
			goto fail;
		nf->is_ads = 1;
		nf->mode = S_IFREG | (0777 & ~fs->opt.fmask);
	} else if (ino->is_dir && !ntfs_inode_is_link(ino)) {
		r = ntfs_dir_stream(fs, nf->mref, &st);
		if (r != ST_OK)
			goto fail;
		nf->is_dir = 1;
		nf->mode = ntfs_inode_mode(fs, ino);
		nf->rd_parent = ino->parent ? ino->parent : nf->mref;
		nf->rd_key = (uint16_t *)kalloc(NTFS_MAX_NAME_LEN * 2);
		if (!nf->rd_key) {
			r = ST_NOMEM;
			goto fail;
		}
	} else {
		nf->mode = ntfs_inode_mode(fs, ino);
		if (ino->has_data) {
			r = ntfs_stream_get(fs, nf->mref, NTFS_AT_DATA,
					    ntfs_unnamed, 0, sg, &st);
			if (r == ST_UNSUPPORTED && (sg & NTFS_SG_WOF))
				r = ST_OK; /* opened; reads answer EOPNOTSUPP */
			if (r != ST_OK)
				goto fail;
		}
	}
	nf->st = st;
	nf->vfs.ops = &ntfs_vfs_ops;
	nf->vfs.fs_private = nf;
	if (st && !nf->is_dir)
		nf->inode = icache_get(&fs->sb, ntfs_chain(st), st->data_size,
				       nf->mode, 0, 0, 0, 0, 0);
	if (ntfs_ofile_open(fs, NTFS_MREF_NO(nf->mref)) != ST_OK) {
		if (nf->inode)
			icache_unref((ic_inode_t *)nf->inode);
		nf->inode = NULL;
		r = ST_NOMEM;
		goto fail;
	}
	*out = &nf->vfs;
	kfree(res);
	kfree(ino);
	return ST_OK;
fail:
	if (st)
		ntfs_stream_put(fs, st);
	if (nf) {
		ntfs_free(nf->rd_key);
		kfree(nf);
	}
	ntfs_free(res);
	ntfs_free(ino);
	return r;
}

int ntfs_open_mode(vfs_mount_t *m, const char *path, int flags,
		   unsigned int mode, vfs_file_t **out)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	int r, created = 0, excl = (flags & O_CREAT) != 0;

	if (!fs || !path || !out)
		return ST_INVALID;
	if (excl)
		ntfs_meta_wlock(fs);
	else
		ntfs_meta_rlock(fs);
	r = ntfs_open_impl(fs, path, flags, mode, &created, out);
	if (excl)
		ntfs_meta_wunlock(fs);
	else
		ntfs_meta_runlock(fs);
	/* O_TRUNC of an existing file (an empty one is left alone: no
	 * write for nothing). */
	if (r == ST_OK && (flags & O_TRUNC) && !created) {
		ntfs_file_t *nf = ntfs_nf(*out);

		if (nf->st && !nf->is_dir && nf->st->data_size) {
			r = ntfs_truncate(&nf->vfs, 0);
			if (r != ST_OK) {
				ntfs_close(&nf->vfs);
				*out = NULL;
			}
		}
	}
	return r;
}

int ntfs_open(vfs_mount_t *m, const char *path, int flags,
	      vfs_file_t **out)
{
	return ntfs_open_mode(m, path, flags, 0666, out);
}

int ntfs_close(vfs_file_t *f)
{
	ntfs_file_t *nf = ntfs_nf(f);
	ntfs_fs_t *fs;
	int r = ST_OK;

	if (!nf)
		return ST_INVALID;
	fs = nf->fs;
	/* What was written is committed: data, then the attribute. */
	if (nf->st && !nf->is_dir &&
	    ((nf->st->flags & (NTFS_ST_META | NTFS_ST_DIRTY)) || nf->st->err)) {
		r = ntfs_commit_locked(fs, nf->st);
		nf->st->err = 0;
	}
	if (nf->inode)
		icache_unref((ic_inode_t *)nf->inode);
	if (nf->st)
		ntfs_stream_put(fs, nf->st);
	/* The last close of a file whose last name is gone deletes it. */
	if (ntfs_ofile_close(fs, NTFS_MREF_NO(nf->mref))) {
		ntfs_meta_wlock(fs);
		if (ntfs_writable(fs)) {
			int rr = ntfs_file_delete(fs, nf->mref);

			if (rr != ST_OK && r == ST_OK)
				r = rr;
		}
		ntfs_meta_wunlock(fs);
	}
	ntfs_free(nf->rd_key);
	kfree(nf);
	return r;
}

/* ---- read ---- */

/* O_DIRECT read: dirty pages of the stream reach the device first, then
 * an aligned, mapped range is read straight from it (through a kernel
 * buffer, copied out after unlocking); anything else is buffered.
 * Returns ST_AGAIN for "use the buffered path". */
static long ntfs_read_direct(ntfs_file_t *nf, void *buf, long bytes,
			     uint64_t pos)
{
	ntfs_fs_t *fs = nf->fs;
	uint64_t no = NTFS_MREF_NO(nf->mref), n;
	uint8_t *kbuf;
	long r;

	if (bytes <= 0 || (uint64_t)bytes > 1024 * 1024)
		return ST_AGAIN;
	if (nf->st->flags & NTFS_ST_ENCRYPTED)
		return ST_UNSUPPORTED;
	if (nf->st->flags & NTFS_ST_WOF)
		return ST_AGAIN; /* system-compressed: buffered reads */
	kbuf = (uint8_t *)kalloc((unsigned long)bytes);
	if (!kbuf)
		return ST_AGAIN;
	ntfs_wlock_file(fs, no);
	n = pos < nf->st->data_size ?
		    ntfs_min64((uint64_t)bytes, nf->st->data_size - pos) :
		    0;
	if (!n) {
		r = 0;
	} else if (!ntfs_dio_ok(fs, nf->st, pos, n)) {
		r = ST_AGAIN;
	} else {
		uint64_t errs = fs->io_errs;

		if (nf->st->flags & NTFS_ST_DIRTY)
			pagecache_flush_file(&fs->sb, ntfs_chain(nf->st));
		r = fs->io_errs != errs ? ST_IO :
		    ntfs_dio_read(fs, nf->st, pos, kbuf, n) == ST_OK ? (long)n :
								       ST_IO;
	}
	ntfs_wunlock_file(fs, no);
	if (r > 0 && copy_to_user(buf, kbuf, (size_t)r))
		r = ST_INVALID;
	kfree(kbuf);
	return r;
}

static long ntfs_read_common(ntfs_file_t *nf, void *buf, long bytes,
			     uint64_t pos, pc_readahead_t *ra)
{
	ntfs_fs_t *fs = nf->fs;
	uint64_t no = NTFS_MREF_NO(nf->mref), done = 0;
	pc_page_t **pages;
	long r = 0;

	if (nf->is_dir)
		return ST_ISDIR;
	if (!buf || bytes < 0)
		return ST_INVALID;
	if (!nf->st || bytes == 0)
		return 0;
	if (nf->vfs.flags & NTFS_O_DIRECT) {
		r = ntfs_read_direct(nf, buf, bytes, pos);
		if (r != ST_AGAIN)
			return r;
		r = 0;
	}
	pages = (pc_page_t **)kalloc(NTFS_RD_PAGES * sizeof(pc_page_t *));
	if (!pages)
		return ST_NOMEM;
	while (done < (uint64_t)bytes) {
		uint64_t p = pos + done, size;
		uint32_t got = 0;
		int rr, fault = 0;

		ntfs_flock_shared(fs, no);
		ntfs_meta_rlock(fs);
		size = nf->st->data_size;
		rr = ntfs_stream_read_pages(fs, nf->st, p,
					    (uint64_t)bytes - done, pages, &got,
					    ra);
		ntfs_meta_runlock(fs);
		ntfs_funlock_shared(fs, no);
		/* Copy out with no lock held. */
		for (uint32_t i = 0; i < got; i++) {
			uint64_t q = pos + done;
			uint64_t in = q % PAGE_SIZE;
			uint64_t c;

			c = ntfs_min64(PAGE_SIZE - in, (uint64_t)bytes - done);
			if (q >= size)
				c = 0;
			else if (c > size - q)
				c = size - q;
			if (!fault && c &&
			    copy_to_user((uint8_t *)buf + done, pages[i]->data + in,
					 c))
				fault = 1;
			if (!fault)
				done += c;
			pagecache_put(pages[i]);
		}
		if (fault) {
			r = done ? 0 : ST_INVALID;
			break;
		}
		if (rr != ST_OK) {
			r = rr;
			break;
		}
		if (!got || pos + done >= size)
			break;
	}
	kfree(pages);
	return done ? (long)done : r;
}

long ntfs_read(vfs_file_t *f, void *buf, long bytes)
{
	ntfs_file_t *nf = ntfs_nf(f);
	pc_readahead_t ra;
	long r;

	if (!nf)
		return ST_INVALID;
	ra = nf->ra;
	r = ntfs_read_common(nf, buf, bytes, nf->pos, &ra);
	if (r > 0) {
		nf->pos += (unsigned long)r;
		nf->ra = ra;
	}
	return r;
}

long ntfs_read_at(vfs_file_t *f, void *buf, long bytes, long off)
{
	ntfs_file_t *nf = ntfs_nf(f);
	pc_readahead_t ra;

	if (!nf || off < 0)
		return ST_INVALID;
	pc_readahead_init(&ra);
	return ntfs_read_common(nf, buf, bytes, (uint64_t)off, &ra);
}

/* ---- seek (SEEK_SET/CUR/END, SEEK_DATA = 3, SEEK_HOLE = 4) ---- */

static long ntfs_seek_data_hole(ntfs_file_t *nf, uint64_t pos, int want_data)
{
	ntfs_fs_t *fs = nf->fs;
	ntfs_stream_t *st = nf->st;
	uint64_t size = st ? st->data_size : 0;
	long r = ST_NXIO;
	int found = 0;

	if (pos >= size)
		return ST_NXIO;
	ntfs_meta_rlock(fs);
	while (pos < size) {
		ntfs_iomap_t map;
		int data;

		if (ntfs_iomap_begin(fs, st, pos, size - pos, NTFS_IOMAP_REPORT,
				     &map) != ST_OK) {
			r = ST_IO;
			found = 1;
			break;
		}
		data = map.type != NTFS_IOMAP_HOLE &&
		       map.type != NTFS_IOMAP_UNWRITTEN;
		ntfs_iomap_end(fs, st, pos, size - pos, 0, NTFS_IOMAP_REPORT,
			       &map);
		if (data == want_data) {
			r = (long)(map.offset > pos ? map.offset : pos);
			found = 1;
			break;
		}
		if (map.offset + map.length <= pos) {
			r = ST_IO; /* no progress: corrupt mapping */
			found = 1;
			break;
		}
		pos = map.offset + map.length;
	}
	ntfs_meta_runlock(fs);
	/* No hole before the end: the end of file is one. */
	if (!found && !want_data)
		r = (long)size;
	return r;
}

long ntfs_seek(vfs_file_t *f, long offset, int whence)
{
	ntfs_file_t *nf = ntfs_nf(f);
	long base, np;

	if (!nf)
		return ST_INVALID;
	if (nf->is_dir) {
		/* Directory cookies are entry ordinals (see readdir).  The
		 * position is the ordinal of the next entry, or -- after a
		 * seek, until the skipping is done -- the one sought. */
		uint64_t cur = nf->rd_ord > nf->rd_skip ? nf->rd_ord :
							  nf->rd_skip;

		if (whence == 0 && offset >= 0) {
			/* Always from the start: the entries may have
			 * changed since the ordinal was handed out. */
			nf->rd_ord = 0;
			nf->rd_key_valid = 0;
			nf->rd_eof = 0;
			nf->rd_skip = (uint64_t)offset;
			return offset;
		}
		if (whence == 1 && offset == 0)
			return (long)cur;
		return ST_INVALID;
	}
	switch (whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = (long)nf->pos;
		break;
	case 2:
		base = nf->st ? (long)nf->st->data_size : 0;
		break;
	case 3:
	case 4:
		if (offset < 0)
			return ST_NXIO;
		np = ntfs_seek_data_hole(nf, (uint64_t)offset, whence == 3);
		if (np >= 0)
			nf->pos = (unsigned long)np;
		return np;
	default:
		return ST_INVALID;
	}
	if ((offset > 0 && base > 0x7FFFFFFFFFFFFFFFL - offset) ||
	    (offset < 0 && base < (-0x7FFFFFFFFFFFFFFFL - 1) - offset))
		return ST_INVALID;
	np = base + offset;
	if (np < 0)
		return ST_INVALID;
	nf->pos = (unsigned long)np;
	return np;
}
