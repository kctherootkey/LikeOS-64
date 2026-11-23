/* LikeOS - NTFS filesystem driver
 *
 * Page-cache superblock operations, iomap, stream data reads.
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
 * S10  Page-cache superblock operations
 *
 * Pure memory lookups over the stream's decoded runlist (ntfs.h section 1);
 * no I/O, no sleeping lock.  Called by the page cache under lock_map()
 * (metadata SHARED) or lock_io() (EXCLUSIVE).
 * =========================================================================== */

#define NTFS_PAGE_SHIFT 12

ntfs_fs_t *ntfs_sb_fs(vfs_superblock_t *sb)
{
	return (ntfs_fs_t *)sb->fs_private;
}

static unsigned long ntfs_sb_block_size(vfs_superblock_t *sb)
{
	return ntfs_sb_fs(sb)->exp_bs;
}

static const block_device_t *ntfs_sb_bdev(vfs_superblock_t *sb)
{
	return &ntfs_sb_fs(sb)->guard;
}

static unsigned long ntfs_sb_sector_size(vfs_superblock_t *sb)
{
	return ntfs_sb_fs(sb)->dev_ss;
}

static unsigned long ntfs_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return NTFS_EOC_MARKER;
}

/* Block N of the chain, or 0 (past the data size, or a NOFILL page). */
static unsigned long ntfs_sb_block_at(vfs_superblock_t *sb,
				      unsigned long chain_id,
				      unsigned long n)
{
	ntfs_fs_t *fs = ntfs_sb_fs(sb);
	ntfs_stream_t *st;
	uint64_t nblocks;

	if (!fs || !NTFS_BID_IS(chain_id) || NTFS_BID_INDEX(chain_id) != 0 ||
	    n > NTFS_BID_INDEX_MASK)
		return 0;
	st = ntfs_slot(fs, NTFS_BID_HANDLE(chain_id));
	if (!st)
		return 0;
	nblocks = (st->data_size + fs->exp_bs - 1) >> fs->exp_bits;
	if (n >= nblocks)
		return 0;
	if (ntfs_page_nofill(st, ((uint64_t)n << fs->exp_bits) >>
					 NTFS_PAGE_SHIFT))
		return 0;
	return NTFS_BID_ENC(st->handle, n);
}

static unsigned long ntfs_sb_next_block(vfs_superblock_t *sb, unsigned long bid)
{
	unsigned long nb;

	if (!NTFS_BID_IS(bid))
		return NTFS_BID_EOC;
	nb = ntfs_sb_block_at(sb, NTFS_BID_ENC(NTFS_BID_HANDLE(bid), 0),
			      NTFS_BID_INDEX(bid) + 1);
	return nb ? nb : NTFS_BID_EOC;
}

/* Device LBA of block `bid', 0 for a hole, a block wholly beyond the
 * initialized size or a block not placed yet (delayed allocation). */
static unsigned long ntfs_sb_block_to_lba(vfs_superblock_t *sb,
					  unsigned long bid)
{
	ntfs_fs_t *fs = ntfs_sb_fs(sb);
	ntfs_stream_t *st;
	uint64_t byte;
	int64_t lcn;

	if (!fs || !NTFS_BID_IS(bid))
		return 0;
	st = ntfs_slot(fs, NTFS_BID_HANDLE(bid));
	if (!st || (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
				 NTFS_ST_ENCRYPTED | NTFS_ST_WOF |
				 NTFS_ST_BAD | NTFS_ST_NOALLOC)))
		return 0;
	byte = (uint64_t)NTFS_BID_INDEX(bid) << fs->exp_bits;
	if (byte >= st->init_size || byte >= st->data_size)
		return 0;
	if (ntfs_rl_map(&st->rl, (int64_t)(byte >> fs->cluster_bits), &lcn,
			NULL) != ST_OK)
		return 0;
	if (lcn == NTFS_LCN_HOLE)
		return 0;
	return (unsigned long)((((uint64_t)lcn << fs->cluster_bits) +
				(byte & (fs->cluster_size - 1))) >>
			       fs->dev_ss_bits);
}

static unsigned long ntfs_sb_map_block(vfs_superblock_t *sb,
				       unsigned long chain_id, unsigned long n)
{
	unsigned long bid = ntfs_sb_block_at(sb, chain_id, n);

	if (!bid || !ntfs_sb_block_to_lba(sb, bid))
		return 0;
	return bid;
}

static int ntfs_sb_write_inode(vfs_superblock_t *sb, struct ic_inode *ino)
{
	(void)sb;
	(void)ino;
	/* The inode cache's size is a copy of the stream's; the driver
	 * commits a stream's attribute itself (S30: fsync, close, sync,
	 * unmount), after its data reached the device.  Nothing to do. */
	return ST_OK;
}

static void ntfs_sb_lock_io(vfs_superblock_t *sb)
{
	ntfs_meta_wlock(ntfs_sb_fs(sb));
}

static void ntfs_sb_unlock_io(vfs_superblock_t *sb)
{
	ntfs_meta_wunlock(ntfs_sb_fs(sb));
}

static void ntfs_sb_lock_map(vfs_superblock_t *sb)
{
	ntfs_meta_rlock(ntfs_sb_fs(sb));
}

static void ntfs_sb_unlock_map(vfs_superblock_t *sb)
{
	ntfs_meta_runlock(ntfs_sb_fs(sb));
}

static unsigned long ntfs_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

const vfs_sb_ops_t ntfs_sb_ops = {
	.block_size = ntfs_sb_block_size,
	.bdev = ntfs_sb_bdev,
	.sector_size = ntfs_sb_sector_size,
	.block_to_lba = ntfs_sb_block_to_lba,
	.end_of_chain_marker = ntfs_sb_end_of_chain,
	.next_block = ntfs_sb_next_block,
	.map_block = ntfs_sb_map_block,
	.block_at = ntfs_sb_block_at,
	.write_inode = ntfs_sb_write_inode,
	.lock_io = ntfs_sb_lock_io,
	.unlock_io = ntfs_sb_unlock_io,
	.lock_map = ntfs_sb_lock_map,
	.unlock_map = ntfs_sb_unlock_map,
	.reserved_meta_block = ntfs_sb_reserved_meta_block,
	/* Delayed allocation at writeback (S30). */
	.alloc_blocks = ntfs_sb_alloc_blocks,
};

/* ===========================================================================
 * S11  The iomap layer
 *
 * ntfs_iomap_begin() describes the extent of stream `st' that starts at
 * byte `pos' (at most `len' bytes):
 *
 *   NTFS_IOMAP_MAPPED      on the device at byte `pbyte' (initialized data)
 *   NTFS_IOMAP_HOLE        sparse run: reads as zeros, no clusters
 *   NTFS_IOMAP_UNWRITTEN   allocated (or not) but beyond initialized_size:
 *                          reads as zeros
 *   NTFS_IOMAP_DELALLOC    reserved, not placed: page cache only
 *   NTFS_IOMAP_INLINE      resident value: `inline_data'
 *   NTFS_IOMAP_COMPRESSED  inside a compression unit (unit-sized extent:
 *                          offset/length are the whole unit; HOLE when
 *                          the unit is all sparse)
 *   NTFS_IOMAP_ENCRYPTED   data that is never returned ($EFS / WOF)
 *
 * The answer never spans the initialized size, a run boundary or a unit
 * boundary.  Users: the read path (which pages are plain cache pages and
 * which are NOFILL), SEEK_DATA/SEEK_HOLE, and writeback, direct I/O
 * and truncate.  Callers hold the metadata lock (shared at least).
 * ntfs_iomap_end() closes the operation (writes: commits delalloc
 * reservations); for reads it only checks its arguments.
 * =========================================================================== */

int ntfs_iomap_begin(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		     uint64_t len, int flags, ntfs_iomap_t *map)
{
	int64_t vcn, lcn, left;
	uint64_t end, run_end;

	(void)flags;
	ntfs_assert_meta_any(fs);
	mm_memset(map, 0, sizeof(*map));
	if (pos >= st->data_size || len == 0)
		return ST_INVALID;
	if (len > st->data_size - pos)
		len = st->data_size - pos;
	end = pos + len;
	map->offset = pos;
	if (st->flags & (NTFS_ST_ENCRYPTED | NTFS_ST_WOF | NTFS_ST_BAD)) {
		map->type = NTFS_IOMAP_ENCRYPTED;
		map->length = len;
		return ST_OK;
	}
	if (st->flags & NTFS_ST_RESIDENT) {
		map->type = NTFS_IOMAP_INLINE;
		map->length = len;
		map->inline_data = st->res_data + pos;
		return ST_OK;
	}
	if (st->flags & NTFS_ST_COMPRESSED) {
		uint64_t ustart = pos - pos % st->cu_bytes;
		uint64_t cuc = 1ULL << st->comp_unit;
		int64_t uvcn = (int64_t)(ustart >> fs->cluster_bits);
		uint64_t nalloc = 0;

		for (uint64_t i = 0; i < cuc;) {
			if (ntfs_rl_map(&st->rl, uvcn + (int64_t)i, &lcn,
					&left) != ST_OK)
				return ST_IO;
			if (left <= 0)
				return ST_IO;
			if ((uint64_t)left > cuc - i)
				left = (int64_t)(cuc - i);
			if (lcn != NTFS_LCN_HOLE)
				nalloc += (uint64_t)left;
			i += (uint64_t)left;
		}
		map->offset = ustart;
		map->length = ntfs_min64(st->cu_bytes, st->data_size - ustart);
		map->type = nalloc ? NTFS_IOMAP_COMPRESSED : NTFS_IOMAP_HOLE;
		return ST_OK;
	}
	if (pos >= st->init_size) {
		map->type = NTFS_IOMAP_UNWRITTEN;
		map->length = len;
		return ST_OK;
	}
	if (end > st->init_size)
		end = st->init_size;
	vcn = (int64_t)(pos >> fs->cluster_bits);
	if (st->dl_n) {
		/* Reserved, not placed yet: the data is in the page cache. */
		for (uint32_t i = 0; i < st->dl_n; i++) {
			int64_t s = st->dl[i].vcn, e = s + st->dl[i].len;

			if (vcn >= s && vcn < e) {
				run_end = (uint64_t)e << fs->cluster_bits;
				map->type = NTFS_IOMAP_DELALLOC;
				map->length = (end < run_end ? end : run_end) - pos;
				return ST_OK;
			}
		}
	}
	if (vcn >= st->rl.end_vcn && (st->flags & NTFS_ST_SPARSE)) {
		/* A sparse file extended in memory: a hole until committed. */
		map->type = NTFS_IOMAP_HOLE;
		map->length = end - pos;
		return ST_OK;
	}
	if (ntfs_rl_map(&st->rl, vcn, &lcn, &left) != ST_OK)
		return ST_IO;
	run_end = (uint64_t)(vcn + left) << fs->cluster_bits;
	if (end > run_end)
		end = run_end;
	map->length = end - pos;
	if (lcn == NTFS_LCN_HOLE) {
		map->type = NTFS_IOMAP_HOLE;
		return ST_OK;
	}
	map->type = NTFS_IOMAP_MAPPED;
	map->pbyte = ((uint64_t)lcn << fs->cluster_bits) +
		     (pos & (fs->cluster_size - 1));
	return ST_OK;
}

int ntfs_iomap_end(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		   uint64_t len, uint64_t done, int flags,
		   const ntfs_iomap_t *map)
{
	(void)fs;
	(void)st;
	(void)flags;
	if (WARN_ON_ONCE(done > len || pos < map->offset))
		return ST_INVALID;
	return ST_OK;
}

/* ===========================================================================
 * S13  Stream data: pages, NOFILL fills, byte-range reads, readahead
 *
 * ntfs_stream_page() returns page `idx' of a stream HELD (release with
 * pagecache_put()).  A plain page comes from pagecache_get_held(), which
 * maps its blocks through S10 and reads them, coalescing up to 64 KB of
 * contiguous blocks into one transfer.  A NOFILL page (ntfs.h section 2) is
 * looked up first (pagecache_lookup_held) and on a miss filled here and
 * published with pagecache_install():
 *
 *   resident value         copied from the stream's value copy;
 *   compressed unit        the whole unit (16 clusters) is read and
 *                          decompressed once and every page of it
 *                          installed -- the folio-sized batch of the
 *                          compressed path;
 *   initialized-size page  read through the runlist, bytes from the
 *                          initialized size on zeroed;
 *   encrypted / WOF        never filled: ST_UNSUPPORTED (no
 *                          path, readahead included, returns ciphertext).
 *
 * Bounce buffers are heap allocations; nothing larger than a few words
 * lives on the stack.
 * =========================================================================== */

static int ntfs_fill_resident(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
			      pc_page_t **out)
{
	uint64_t start = idx * PAGE_SIZE;
	uint64_t len;
	pc_page_t *pg;

	if (start >= st->res_len)
		len = 0;
	else
		len = ntfs_min64(PAGE_SIZE, st->res_len - start);
	pg = pagecache_install(&fs->sb, ntfs_chain(st), idx,
			       len ? st->res_data + start : NULL, len, 1);
	if (!pg)
		return ST_NOMEM;
	*out = pg;
	return ST_OK;
}

static int ntfs_fill_straddle(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
			      pc_page_t **out)
{
	uint64_t start = idx * PAGE_SIZE;
	uint64_t len = ntfs_min64(PAGE_SIZE, st->data_size - start);
	uint8_t *buf = (uint8_t *)kalloc(PAGE_SIZE);
	pc_page_t *pg;
	int r;

	if (!buf)
		return ST_NOMEM;
	r = ntfs_rl_pread(fs, &st->rl, start, len, st->init_size, buf);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	pg = pagecache_install(&fs->sb, ntfs_chain(st), idx, buf, len, 1);
	kfree(buf);
	if (!pg)
		return ST_NOMEM;
	*out = pg;
	return ST_OK;
}

/* Read the allocated clusters of compression unit starting at `uvcn' in
 * VCN order into `in'; returns the number of clusters read in *nalloc. */
static int ntfs_cu_read(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t uvcn,
			uint8_t *in, uint64_t *nalloc)
{
	uint64_t cuc = 1ULL << st->comp_unit;
	uint64_t got = 0;

	for (uint64_t i = 0; i < cuc;) {
		int64_t lcn, left;
		int r;

		if (ntfs_rl_map(&st->rl, uvcn + (int64_t)i, &lcn, &left) !=
			    ST_OK ||
		    left <= 0)
			return ST_IO;
		if ((uint64_t)left > cuc - i)
			left = (int64_t)(cuc - i);
		if (lcn != NTFS_LCN_HOLE) {
			r = ntfs_read_clusters(fs, lcn, (uint64_t)left,
					       in + (got << fs->cluster_bits));
			if (r != ST_OK)
				return r;
			got += (uint64_t)left;
		}
		i += (uint64_t)left;
	}
	*nalloc = got;
	return ST_OK;
}

static int ntfs_fill_compressed(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
				pc_page_t **out)
{
	uint64_t pos = idx * PAGE_SIZE;
	uint64_t ustart = pos - pos % st->cu_bytes;
	uint64_t cuc = 1ULL << st->comp_unit;
	int64_t uvcn = (int64_t)(ustart >> fs->cluster_bits);
	uint8_t *in = NULL, *outb = NULL;
	uint64_t nalloc = 0;
	pc_page_t *want = NULL;
	int r;

	outb = (uint8_t *)kalloc(st->cu_bytes);
	in = (uint8_t *)kalloc(st->cu_bytes);
	if (!outb || !in) {
		r = ST_NOMEM;
		goto out;
	}
	r = ntfs_cu_read(fs, st, uvcn, in, &nalloc);
	if (r != ST_OK)
		goto out;
	if (nalloc == 0) {
		mm_memset(outb, 0, st->cu_bytes); /* sparse unit */
	} else if (nalloc == cuc) {
		mm_memcpy(outb, in, st->cu_bytes); /* stored uncompressed */
	} else {
		long n = ntfs_lznt1_decompress(in, nalloc << fs->cluster_bits,
					       outb, st->cu_bytes);

		if (n < 0) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt "
					  "compressed data at unit %llu",
				       (unsigned long long)st->mft_no,
				       (unsigned long long)(ustart /
							    st->cu_bytes));
			r = ST_IO;
			goto out;
		}
	}
	/* Bytes beyond the initialized size read as zeros. */
	if (st->init_size < ustart + st->cu_bytes) {
		uint64_t z = st->init_size > ustart ? st->init_size - ustart : 0;

		mm_memset(outb + z, 0, st->cu_bytes - z);
	}
	for (uint64_t p = 0; p < st->cu_bytes / PAGE_SIZE; p++) {
		uint64_t pidx = ustart / PAGE_SIZE + p;
		uint64_t pstart = pidx * PAGE_SIZE;
		uint64_t len;
		pc_page_t *pg;

		if (pstart >= st->data_size)
			break;
		len = ntfs_min64(PAGE_SIZE, st->data_size - pstart);
		pg = pagecache_install(&fs->sb, ntfs_chain(st), pidx,
				       outb + p * PAGE_SIZE, len, pidx == idx);
		if (pidx == idx)
			want = pg;
	}
	if (!want)
		r = ST_NOMEM;
out:
	ntfs_free(in);
	ntfs_free(outb);
	if (r == ST_OK)
		*out = want;
	return r;
}

/* ---- system-compressed (WOF) files ----
 *
 * The unnamed $DATA of such a file is a sparse placeholder of the file's
 * size; the contents live in the named $DATA stream "WofCompressedData":
 *
 *   [chunk table][chunk 0][chunk 1]...
 *
 * The file is cut into chunks of wof_chunk bytes (the last one shorter);
 * chunk i decompresses on its own into min(wof_chunk, size - i * chunk)
 * bytes.  The table holds chunk_count - 1 little-endian offsets (chunk 0
 * starts at 0), 4 bytes each for files below 4 GB, else 8, relative to the
 * end of the table; a chunk ends where the next one starts, the last one
 * at the end of the stream.  A chunk whose stored size equals its
 * uncompressed size was stored as it is.
 *
 * Every page of a chunk is decoded at once and installed into the page
 * cache like a compression unit.  Buffers and the decoder workspace are
 * allocated per call: no global workspace, no lock held across the
 * reads.  Every table entry is checked against the stream: the offsets
 * must grow, lie inside the stream, and no chunk may be larger
 * than the chunk size. */
static const uint16_t ntfs_wof_name[17] = { 'W', 'o', 'f', 'C', 'o', 'm',
					    'p', 'r', 'e', 's', 's', 'e',
					    'd', 'D', 'a', 't', 'a' };

/* Offset `i' of the chunk table (`esz' bytes per entry) of `ws'. */
static int ntfs_wof_entry(ntfs_fs_t *fs, ntfs_stream_t *ws, uint64_t i,
			  uint32_t esz, uint64_t *val)
{
	uint8_t b[8];
	int r;

	r = ntfs_stream_pread(fs, ws, i * esz, esz, b, 0);
	if (r != ST_OK)
		return r;
	*val = esz == 4 ? (uint64_t)ntfs_le32(b) : ntfs_le64(b);
	return ST_OK;
}

static int ntfs_fill_wof(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
			 pc_page_t **out)
{
	uint64_t pos = idx * PAGE_SIZE, chunk = st->wof_chunk;
	uint64_t ci, nchunks, tbl, start, end, clen, ulen, cstart;
	uint32_t esz;
	ntfs_stream_t *ws = NULL;
	uint8_t *in = NULL, *outb = NULL;
	void *work = NULL;
	pc_page_t *want = NULL;
	int r;

	if (st->wof_fmt == NTFS_WOF_NONE || !chunk || chunk % PAGE_SIZE)
		return ST_UNSUPPORTED; /* a format this driver cannot read */
	ci = pos / chunk;
	nchunks = (st->data_size + chunk - 1) / chunk;
	if (!nchunks || ci >= nchunks)
		return ST_IO;
	cstart = ci * chunk;
	ulen = ntfs_min64(chunk, st->data_size - cstart);
	esz = st->data_size >= (1ULL << 32) ? 8 : 4;
	tbl = (nchunks - 1) * esz;
	r = ntfs_stream_get(fs, NTFS_MAKE_MREF(st->mft_no, st->seq),
			    NTFS_AT_DATA, ntfs_wof_name, 17, 0, &ws);
	if (r == ST_NOT_FOUND)
		r = ST_IO; /* the placeholder without its data */
	if (r != ST_OK)
		return r;
	if (ws->flags & (NTFS_ST_ENCRYPTED | NTFS_ST_WOF | NTFS_ST_BAD) ||
	    ws->data_size < tbl) {
		r = ST_IO;
		goto out;
	}
	start = 0;
	end = ws->data_size - tbl;
	if (ci > 0) {
		r = ntfs_wof_entry(fs, ws, ci - 1, esz, &start);
		if (r != ST_OK)
			goto out;
	}
	if (ci + 1 < nchunks) {
		r = ntfs_wof_entry(fs, ws, ci, esz, &end);
		if (r != ST_OK)
			goto out;
	}
	if (end <= start || end > ws->data_size - tbl || end - start > ulen) {
		r = ST_IO;
		goto out;
	}
	clen = end - start;
	in = (uint8_t *)kalloc(clen);
	outb = (uint8_t *)kalloc(chunk);
	if (!in || !outb) {
		r = ST_NOMEM;
		goto out;
	}
	r = ntfs_stream_pread(fs, ws, tbl + start, clen, in, 0);
	if (r != ST_OK)
		goto out;
	if (clen == ulen) {
		mm_memcpy(outb, in, ulen); /* stored */
	} else {
		int lzx = st->wof_fmt == NTFS_WOF_LZX;

		work = kalloc(lzx ? NTFS_LZX_WS_SIZE : NTFS_XPRESS_WS_SIZE);
		if (!work) {
			r = ST_NOMEM;
			goto out;
		}
		r = lzx ? ntfs_lzx_decompress(in, (uint32_t)clen, outb,
					      (uint32_t)ulen, work) :
			  ntfs_xpress_decompress(in, (uint32_t)clen, outb,
						 (uint32_t)ulen, work);
		if (r != ST_OK) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt "
					  "system-compressed chunk %llu",
				       (unsigned long long)st->mft_no,
				       (unsigned long long)ci);
			r = ST_IO;
			goto out;
		}
	}
	if (ulen < chunk)
		mm_memset(outb + ulen, 0, chunk - ulen);
	for (uint64_t p = 0; p < chunk / PAGE_SIZE; p++) {
		uint64_t pidx = cstart / PAGE_SIZE + p;
		uint64_t pstart = pidx * PAGE_SIZE;
		pc_page_t *pg;

		if (pstart >= st->data_size)
			break;
		pg = pagecache_install(&fs->sb, ntfs_chain(st), pidx,
				       outb + p * PAGE_SIZE,
				       ntfs_min64(PAGE_SIZE,
						  st->data_size - pstart),
				       pidx == idx);
		if (pidx == idx)
			want = pg;
	}
	if (!want)
		r = ST_NOMEM;
out:
	ntfs_free(work);
	ntfs_free(in);
	ntfs_free(outb);
	if (ws)
		ntfs_stream_put(fs, ws);
	if (r == ST_OK)
		*out = want;
	return r;
}

/* Page `idx' of `st', held.  The metadata lock is held (shared at least);
 * for user data the file lock as well. */
int ntfs_stream_page(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
		     pc_page_t **out)
{
	unsigned long chain = ntfs_chain(st);
	pc_page_t *pg;

	*out = NULL;
	if (idx * PAGE_SIZE >= st->data_size)
		return ST_INVALID;
	if (st->flags & NTFS_ST_BAD)
		return ST_IO;
	if (!ntfs_page_nofill(st, idx)) {
		pg = pagecache_get_held(chain, idx, st->data_size, &fs->sb,
					chain);
		if (!pg || !pg->data) {
			pagecache_put(pg);
			return ST_IO;
		}
		*out = pg;
		return ST_OK;
	}
	pg = pagecache_lookup_held(&fs->sb, chain, idx);
	if (pg) {
		*out = pg;
		return ST_OK;
	}
	if (st->flags & NTFS_ST_ENCRYPTED)
		return ST_UNSUPPORTED;
	if (st->flags & NTFS_ST_WOF)
		return ntfs_fill_wof(fs, st, idx, out);
	if (st->flags & NTFS_ST_RESIDENT)
		return ntfs_fill_resident(fs, st, idx, out);
	if (st->flags & NTFS_ST_COMPRESSED)
		return ntfs_fill_compressed(fs, st, idx, out);
	return ntfs_fill_straddle(fs, st, idx, out);
}

/* Copy [off, off + len) of `st' into `buf' (a user buffer when `to_user').
 * ST_OK, or the first error; the range is validated without
 * additions that could wrap. */
int ntfs_stream_pread(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t off,
		      uint64_t len, void *buf, int to_user)
{
	uint8_t *dst = (uint8_t *)buf;

	might_sleep();
	if (off > st->data_size || len > st->data_size - off)
		return ST_IO;
	while (len) {
		uint64_t idx = off / PAGE_SIZE;
		uint64_t in = off % PAGE_SIZE;
		uint64_t n = ntfs_min64(PAGE_SIZE - in, len);
		pc_page_t *pg;
		int r = ntfs_stream_page(fs, st, idx, &pg);

		if (r != ST_OK)
			return r;
		if (to_user)
			smap_disable();
		mm_memcpy(dst, pg->data + in, n);
		if (to_user)
			smap_enable();
		pagecache_put(pg);
		dst += n;
		off += n;
		len -= n;
	}
	return ST_OK;
}

/* The pages of [pos, pos + bytes) of `st' (at most NTFS_RD_PAGES), held,
 * for the read path: the caller holds the file lock and the metadata lock
 * shared while they are taken, and copies out of them only after it let
 * go of both (no user memory is touched under a filesystem lock; a held
 * page stays valid even if it is dropped from the cache meanwhile).
 * *got: pages taken; returns ST_OK or the error at the first page. */
int ntfs_stream_read_pages(ntfs_fs_t *fs, ntfs_stream_t *st,
			   uint64_t pos, uint64_t bytes,
			   pc_page_t **pages, uint32_t *got,
			   pc_readahead_t *ra)
{
	unsigned long chain = ntfs_chain(st);
	uint64_t end;

	*got = 0;
	if ((st->flags & NTFS_ST_ENCRYPTED) ||
	    ((st->flags & NTFS_ST_WOF) && st->wof_fmt == NTFS_WOF_NONE))
		return ST_UNSUPPORTED;
	if (pos >= st->data_size)
		return ST_OK;
	end = pos + ntfs_min64(bytes, st->data_size - pos);
	while (pos < end && *got < NTFS_RD_PAGES) {
		uint64_t idx = pos / PAGE_SIZE;
		pc_page_t *pg;
		int r = ntfs_stream_page(fs, st, idx, &pg);

		if (r != ST_OK)
			return *got ? ST_OK : r;
		pages[(*got)++] = pg;
		/* Folio-style batching: the page cache grows a sequential
		 * window up to 64 KB and fetches it with coalesced reads.
		 * NOFILL streams batch per compression unit instead. */
		if (ra && !(st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
					 NTFS_ST_WOF)))
			pagecache_readahead(ra, chain, idx, st->data_size,
					    &fs->sb, chain);
		pos = (idx + 1) * PAGE_SIZE;
	}
	return ST_OK;
}
