// LikeOS - ext4 filesystem driver: block groups: geometry, group descriptors and
// block/inode bitmaps
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Group descriptor + inode reads
 * =================================================================== */

/* ---- Block group geometry ------------------------------------------------
 *
 * Where the superblock copies and the group descriptor table live.  The
 * allocators need this to build the block bitmap of a group whose bitmap was
 * never written (BLOCK_UNINIT): its used blocks are exactly the metadata at
 * the start of the group plus whichever of its own bitmaps / inode table
 * blocks fall inside it.
 *
 * Only the classic layout is described: the descriptor table right behind
 * the superblock (no meta_bg), superblock copies in the groups sparse_super
 * names or in every group (no sparse_super2).  The UNINIT flags are honoured
 * on file systems with group descriptor checksums (metadata_csum or
 * gdt_csum) and that layout (ext4_uninit_honoured); on any other the bitmaps
 * on disk are used as they are. */

int ext4_uninit_honoured(const ext4_fs_t *fs)
{
	return fs->has_gd_csum &&
	       !(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) &&
	       !(fs->feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2);
}

unsigned long ext4_group_first_block(const ext4_fs_t *fs, unsigned g)
{
	return fs->first_data_block + (unsigned long)g * fs->blocks_per_group;
}

/* Blocks group `g` covers: blocks_per_group, less for a short last group. */
unsigned ext4_blocks_in_group(const ext4_fs_t *fs, unsigned g)
{
	if (g == fs->groups_count - 1)
		return (unsigned)(fs->blocks_count -
				  ext4_group_first_block(fs, g));
	return fs->blocks_per_group;
}

/* Blocks of the group descriptor table. */
unsigned ext4_gdt_blocks(const ext4_fs_t *fs)
{
	unsigned per_block = fs->block_size / fs->desc_size;
	return (fs->groups_count + per_block - 1) / per_block;
}

/* Blocks of one group's inode table. */
unsigned ext4_itable_blocks(const ext4_fs_t *fs)
{
	return (unsigned)(((unsigned long)fs->inodes_per_group *
				   fs->inode_size +
			   fs->block_size - 1) /
			  fs->block_size);
}

/* 1 if `a` is a power of `b` (b^1, b^2, ...). */
static int ext4_is_power_of(unsigned a, unsigned b)
{
	for (;;) {
		if (a < b)
			return 0;
		if (a == b)
			return 1;
		if (a % b)
			return 0;
		a /= b;
	}
}

/* Does group `g` carry a superblock (the primary or a backup)?  sparse_super:
 * groups 0, 1 and the powers of 3, 5 and 7; sparse_super2: group 0 and the
 * (at most two) groups the superblock names; neither: every group. */
static int ext4_bg_has_super(const ext4_fs_t *fs, unsigned g)
{
	if (g == 0)
		return 1;
	if (fs->feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2)
		return g == fs->sb_copy.s_backup_bgs[0] ||
		       g == fs->sb_copy.s_backup_bgs[1];
	if (g <= 1 ||
	    !(fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	if (!(g & 1))
		return 0;
	return ext4_is_power_of(g, 3) || ext4_is_power_of(g, 5) ||
	       ext4_is_power_of(g, 7);
}

/* Blocks at the very start of group `g` taken by its superblock copy, its GDT
 * copy and the reserved GDT blocks (online-resize headroom, which follows
 * every GDT copy). */
static unsigned ext4_num_base_meta_blocks(const ext4_fs_t *fs, unsigned g)
{
	if (!ext4_bg_has_super(fs, g))
		return 0;
	return 1 + ext4_gdt_blocks(fs) + fs->sb_copy.s_reserved_gdt_blocks;
}

int ext4_read_group_desc(ext4_fs_t *fs, unsigned int group,
				ext4_group_desc *out)
{
	if (group >= fs->groups_count || !fs->gdt)
		return ST_INVALID;
	*out = fs->gdt[group];
	return ST_OK;
}

unsigned long ext4_inode_table_block(ext4_fs_t *fs,
					    const ext4_group_desc *gd)
{
	unsigned long lo = gd->bg_inode_table_lo;
	unsigned long hi = (fs->desc_size >= 64) ? gd->bg_inode_table_hi : 0;
	return lo | (hi << 32);
}

static int ext4_write_gd(ext4_fs_t *fs, unsigned int group);

/* Flush deferred group-descriptor + superblock free-count updates.  These are
 * advisory (e2fsck can recompute them), so instead of writing them on every
 * allocation we batch them once per top-level write operation. */
/* Group-descriptor dirty tracking: which groups changed since the last flush.
 * Writing ALL group descriptors on every allocating op is fine for a 1-group
 * (tiny) fs but catastrophic on a large multi-hundred-group fs (it would journal
 * ~one block per group per op).  We instead remember the touched groups (a small
 * per-group flag, bounded by a [lo,hi] span) and write only those.  Serialized by
 * the I/O mutex like the rest of the metadata state. */

/* Persistent, physically-contiguous buffer for coalescing file-data writes into
 * DMA-sized transfers (allocated once at mount, reused under the I/O mutex). */

/* Persistent scratch block for ext4_write_gd (allocated once at mount).  A
 * per-call kalloc here could fail under memory pressure and SILENTLY drop a
 * group-descriptor write — leaving the on-disk GD (free count + bitmap csum)
 * stale relative to the just-written bitmap.  All callers hold the I/O mutex, so
 * one shared buffer is safe. */

/* ---- Write-back batching of file data ------------------------------------
 * File-data writes are accumulated into the large physically-contiguous buffer
 * st->wbounce and flushed to disk as ONE big device transfer (ext4_wb_flush),
 * coalescing many small write()s into a few large USB commands — the USB
 * per-command latency, not bandwidth, is the throughput limiter, so fewer/larger
 * commands is the whole win.  The buffer holds a single contiguous physical run
 * [st->wb_pbn, st->wb_pbn+st->wb_len) of inode st->wb_ino.
 *
 * data=ordered is preserved: the buffer is flushed (and made durable) BEFORE the
 * journal commits the metadata that references those blocks (ext4_journal_flush),
 * and before any read of them (ext4_read_impl) or free of them (free run).  All
 * accesses are under the I/O mutex, so no extra locking is needed.  Used only when
 * journalling is on; with no journal, data is written directly as before. */

/* Flush the write-back buffer to its physical home (no device sync here — the
 * caller adds one where ordering requires it).  Returns ST_OK if nothing pending
 * or the write succeeded. */
int ext4_wb_flush(ext4_fs_t *fs)
{
	if (!fs || fs->st->wb_len == 0)
		return ST_OK;
	unsigned len = fs->st->wb_len;
	unsigned long pbn = fs->st->wb_pbn;
	fs->st->wb_len = 0; /* reset before I/O (non-reentrant under the lock) */
	if (!ext4_write_target_ok(fs, pbn, len)) {
		fs->st->wb_err = ST_INVALID;
		return ST_INVALID;
	}
	int st = ext4_write_sectors(
		fs->bdev, fs->part_lba_offset + pbn * fs->sectors_per_block,
		(unsigned long)len * fs->sectors_per_block, fs->st->wbounce);
	if (st != ST_OK) {
		fs->st->wb_err = st;
		WARN_ON_ONCE(1);
	}
	return st;
}

void ext4_gd_dirty(ext4_fs_t *fs, unsigned g)
{
	fs->meta_dirty = 1;
	if ((!fs->st->gd_dirty || fs->st->gd_dirty_cap < fs->groups_count) &&
	    fs->groups_count) {
		uint8_t *nd = (uint8_t *)kalloc(fs->groups_count);
		if (nd) { /* one-time per mount (groups_count stable) */
			mm_memset(nd, 0, fs->groups_count);
			if (fs->st->gd_dirty)
				kfree(fs->st->gd_dirty);
			fs->st->gd_dirty = nd;
			fs->st->gd_dirty_cap = fs->groups_count;
		}
	}
	if (fs->st->gd_dirty && g < fs->st->gd_dirty_cap)
		fs->st->gd_dirty[g] = 1;
	if (g < fs->st->gd_lo)
		fs->st->gd_lo = g;
	if (g > fs->st->gd_hi)
		fs->st->gd_hi = g;
	if (g < fs->st->gd_seen_lo)
		fs->st->gd_seen_lo = g; /* ever-touched span (for the sync resync) */
	if (g > fs->st->gd_seen_hi)
		fs->st->gd_seen_hi = g;
}

void ext4_flush_meta(ext4_fs_t *fs)
{
	if (!fs->meta_dirty)
		return;
	if (fs->st->gd_lo <= fs->st->gd_hi) { /* write only the groups that changed */
		unsigned long per_block = fs->block_size / fs->desc_size;
		unsigned long last_blk = (unsigned long)-1;
		for (unsigned g = fs->st->gd_lo; g <= fs->st->gd_hi && g < fs->groups_count;
		     g++) {
			if (fs->st->gd_dirty && !fs->st->gd_dirty[g])
				continue; /* precise: skip clean groups */
			/* ext4_write_gd rewrites the entire descriptor block from fs->gdt[],
             * so one call per distinct block covers every dirty group in it. */
			unsigned long blk = (per_block ? g / per_block : 0);
			if (blk != last_blk) {
				ext4_write_gd(fs, g);
				last_blk = blk;
			}
			if (fs->st->gd_dirty)
				fs->st->gd_dirty[g] = 0;
		}
	}
	fs->st->gd_lo = (unsigned)-1;
	fs->st->gd_hi = 0;
	/* The superblock is no longer written per op — it rides the batch flush
     * (ext4_checkpoint) so it never costs a device write per allocating op. */
	fs->meta_dirty = 0;
}

/* Persist the in-memory group descriptor for `group` back to its GDT block. */
static int ext4_write_gd(ext4_fs_t *fs, unsigned int group)
{
	unsigned long gdt_start = fs->first_data_block + 1;
	unsigned long per_block = fs->block_size / fs->desc_size;
	unsigned long blk = gdt_start + group / per_block;
	uint8_t *buf = fs->st->gd_buf, *buf_owned = 0;
	if (!buf) {
		buf_owned = (uint8_t *)kalloc(fs->block_size);
		buf = buf_owned;
	}
	if (!buf)
		return ST_NOMEM;
	/* Rebuild the WHOLE descriptor block from the authoritative in-memory GDT
     * (fs->gdt[]) instead of read-modify-writing just this one descriptor.
     * Many groups share a descriptor block; an RMW re-reads the OTHER descriptors
     * from the cache/disk, which can be stale right after a checkpoint emptied the
     * batch (ext4_write_block_direct only refreshes already-cached entries), and
     * writing them back would clobber another group's just-persisted free count +
     * bitmap checksum — exactly the group-0/group-1 corruption e2fsck reported.
     * fs->gdt[] is always current, so reconstructing the block is both correct
     * and immune to any cache/disk staleness. */
	mm_memset(buf, 0, fs->block_size);
	unsigned long first =
		(blk - gdt_start) * per_block; /* first group in this block */
	unsigned copy = fs->desc_size < sizeof(ext4_group_desc) ?
				fs->desc_size :
				sizeof(ext4_group_desc);
	for (unsigned long k = 0; k < per_block; k++) {
		unsigned long g = first + k;
		if (g >= fs->groups_count)
			break;
		if (fs->has_gd_csum)
			fs->gdt[g].bg_checksum =
				ext4_gd_csum(fs, (uint32_t)g, &fs->gdt[g]);
		mm_memcpy(buf + k * fs->desc_size, &fs->gdt[g], copy);
	}
	int st = ext4_write_block(fs, blk, buf);
	if (buf_owned)
		kfree(buf_owned); /* never free the persistent scratch block */
	return st;
}

unsigned long ext4_gd_block_bitmap(ext4_fs_t *fs, unsigned g)
{
	unsigned long lo = fs->gdt[g].bg_block_bitmap_lo;
	unsigned long hi =
		(fs->desc_size >= 64) ? fs->gdt[g].bg_block_bitmap_hi : 0;
	return lo | (hi << 32);
}
unsigned long ext4_gd_inode_bitmap(ext4_fs_t *fs, unsigned g)
{
	unsigned long lo = fs->gdt[g].bg_inode_bitmap_lo;
	unsigned long hi =
		(fs->desc_size >= 64) ? fs->gdt[g].bg_inode_bitmap_hi : 0;
	return lo | (hi << 32);
}

/* ---- Bitmaps of groups that were never initialized -----------------------
 *
 * With group descriptor checksums, mke2fs (and resize2fs) leave most groups
 * flagged BLOCK_UNINIT and/or INODE_UNINIT and never write their bitmap
 * blocks: whatever those blocks hold -- zeroes on a fresh image, a previous
 * file system's data on a reused disk -- is NOT the bitmap.  The bitmap is
 * defined by the geometry instead:
 *   INODE_UNINIT  every inode free;
 *   BLOCK_UNINIT  used = the group's superblock / GDT / reserved-GDT copy
 *                 plus those of its own block bitmap, inode bitmap and
 *                 inode table blocks that lie inside the group (with flex_bg
 *                 they usually live in another group);
 * and in both, the bits past the end of the group are set (padding).
 *
 * Loading such a bitmap computes it; the first change made to it clears the
 * flag, fixes the free count to the computed value and writes the bitmap
 * with its checksum, all in the same transaction as the change itself, so
 * the descriptor and the bitmap on disk always describe the same state.
 * Only where ext4_uninit_honoured() says so; elsewhere the loaders read the
 * bitmap block as it is. */

/* Set the bitmap bits [start, end): the padding past a group's last block or
 * inode up to the end of the bitmap block. */
void ext4_mark_bitmap_end(unsigned start, unsigned end, uint8_t *bm)
{
	unsigned i;
	if (start >= end)
		return;
	for (i = start; i < ((start + 7) & ~7u) && i < end; i++)
		ext4_bm_set(bm, i);
	if (i < end)
		mm_memset(bm + (i >> 3), 0xFF, (end - i) >> 3);
}

/* Is physical block `b` inside group `g`? */
int ext4_block_in_group(const ext4_fs_t *fs, unsigned long b, unsigned g)
{
	if (b < fs->first_data_block)
		return 0;
	return (b - fs->first_data_block) / fs->blocks_per_group == g;
}

/* Compute the block bitmap of BLOCK_UNINIT group `g` into `bm`. */
int ext4_init_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm)
{
	mm_memset(bm, 0, fs->block_size);
	unsigned base = ext4_num_base_meta_blocks(fs, g);
	if ((base >> 3) >= fs->block_size) {
		ext4_fs_error(fs, "group metadata larger than its bitmap", 0);
		return EXT4_BM_CORRUPT;
	}
	for (unsigned b = 0; b < base; b++)
		ext4_bm_set(bm, b);
	unsigned long start = ext4_group_first_block(fs, g);
	unsigned long t = ext4_gd_block_bitmap(fs, g);
	if (ext4_block_in_group(fs, t, g))
		ext4_bm_set(bm, t - start);
	t = ext4_gd_inode_bitmap(fs, g);
	if (ext4_block_in_group(fs, t, g))
		ext4_bm_set(bm, t - start);
	unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[g]);
	unsigned long itb = ext4_itable_blocks(fs);
	for (t = itbl; t < itbl + itb; t++)
		if (ext4_block_in_group(fs, t, g))
			ext4_bm_set(bm, t - start);
	ext4_mark_bitmap_end(ext4_blocks_in_group(fs, g), fs->block_size * 8,
			     bm);
	return ST_OK;
}

/* Free blocks a bitmap shows within the group's real extent. */
unsigned long ext4_bitmap_free_blocks(const ext4_fs_t *fs, unsigned g,
					     const uint8_t *bm)
{
	unsigned n = ext4_blocks_in_group(fs, g);
	unsigned long free_cnt = 0;
	for (unsigned b = 0; b < n; b++)
		if (!ext4_bm_test(bm, b))
			free_cnt++;
	return free_cnt;
}

/* Load group `g`'s block bitmap into `bm`: computed for a BLOCK_UNINIT group
 * (*uninit = 1), else read and checksum-verified.  ST_OK, an I/O status, or
 * EXT4_BM_CORRUPT.  Caller holds the group lock. */
int ext4_load_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit)
{
	*uninit = 0;
	if (ext4_uninit_honoured(fs) &&
	    (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT)) {
		if (g == 0) {
			ext4_fs_error(fs,
				      "block bitmap of group 0 marked uninitialized",
				      0);
			return EXT4_BM_CORRUPT;
		}
		int st = ext4_init_block_bitmap(fs, g, bm);
		if (st == ST_OK)
			*uninit = 1;
		return st;
	}
	int st = ext4_read_block(fs, ext4_gd_block_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	if (!ext4_block_bitmap_csum_ok(fs, g, bm)) {
		ext4_fs_error(fs, "block bitmap checksum mismatch", 0);
		return EXT4_BM_CORRUPT;
	}
	return ST_OK;
}

/* Load group `g`'s inode bitmap into `bm`: all-free (plus padding) for an
 * INODE_UNINIT group (*uninit = 1), else read and checksum-verified. */
int ext4_load_inode_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit)
{
	*uninit = 0;
	if (ext4_uninit_honoured(fs) &&
	    (fs->gdt[g].bg_flags & EXT4_BG_INODE_UNINIT)) {
		if (g == 0) {
			ext4_fs_error(fs,
				      "inode bitmap of group 0 marked uninitialized",
				      0);
			return EXT4_BM_CORRUPT;
		}
		mm_memset(bm, 0, fs->block_size);
		ext4_mark_bitmap_end(fs->inodes_per_group, fs->block_size * 8,
				     bm);
		*uninit = 1;
		return ST_OK;
	}
	int st = ext4_read_block(fs, ext4_gd_inode_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	if (!ext4_inode_bitmap_csum_ok(fs, g, bm)) {
		ext4_fs_error(fs, "inode bitmap checksum mismatch", 0);
		return EXT4_BM_CORRUPT;
	}
	return ST_OK;
}

/* The block bitmap of BLOCK_UNINIT group `g` is about to change: drop the
 * flag and set the descriptor's free count to what the computed bitmap shows
 * (`init_free`), before the caller applies its own delta.  The superblock
 * total moves by the same correction so it keeps equalling the sum of the
 * descriptors.  Caller holds the group lock and writes the bitmap. */
void ext4_bg_block_uninit_clear(ext4_fs_t *fs, unsigned g,
				       unsigned long init_free)
{
	unsigned long cur = fs->gdt[g].bg_free_blocks_count_lo;
	/* mkfs/resize2fs store exactly this count; a difference means the
	 * descriptor was wrong (fixed here) or the geometry computed above is. */
	WARN_RATELIMIT(cur != init_free,
		       "ext4: group %u BLOCK_UNINIT free count %lu, computed %lu",
		       g, cur, init_free);
	fs->gdt[g].bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
	fs->gdt[g].bg_free_blocks_count_lo = (uint16_t)init_free;
	fs->sb_copy.s_free_blocks_count_lo += (uint32_t)(init_free - cur);
	ext4_gd_dirty(fs, g);
}

/* Write out the computed block bitmap of BLOCK_UNINIT group `g` and clear the
 * flag, with no blocks changing hands: a group that gets an inode in use gets
 * a real block bitmap too.  Caller holds the group lock. */
int ext4_bg_block_bitmap_materialize(ext4_fs_t *fs, unsigned g)
{
	if (!ext4_uninit_honoured(fs) ||
	    !(fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT))
		return ST_OK;
	uint8_t *bbm = (uint8_t *)ext4_bget(fs);
	if (!bbm)
		return ST_NOMEM;
	int uninit = 0;
	int st = ext4_load_block_bitmap(fs, g, bbm, &uninit);
	if (st == ST_OK && uninit) {
		unsigned long init_free = ext4_bitmap_free_blocks(fs, g, bbm);
		ext4_block_bitmap_csum_set(fs, g, bbm);
		st = ext4_write_block(fs, ext4_gd_block_bitmap(fs, g), bbm);
		if (st == ST_OK)
			ext4_bg_block_uninit_clear(fs, g, init_free);
	}
	ext4_bput(fs, bbm);
	return st;
}

/* Self-heal: recompute every touched group's free-block count + block-bitmap
 * checksum directly from its (authoritative) bitmap, so the on-disk descriptors
 * can never drift from the bitmaps at a sync point — the same principle the mount
 * path applies to the superblock.  Called on sync/fsync/unmount, not per op.
 * Cheap: touched-group bitmaps are hot in the caches, so no extra disk reads. */
void ext4_resync_gd_from_bitmaps(ext4_fs_t *fs)
{
	if (fs->st->gd_seen_lo > fs->st->gd_seen_hi)
		return;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return;
	int is64 = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
	for (unsigned g = fs->st->gd_seen_lo;
	     g <= fs->st->gd_seen_hi && g < fs->groups_count; g++) {
		/* A BLOCK_UNINIT group has no bitmap on disk to resync from:
		 * its count and its computed bitmap agree by definition. */
		if (ext4_uninit_honoured(fs) &&
		    (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT))
			continue;
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		if (ext4_read_block(fs, bblk, bm) != ST_OK)
			continue;
		unsigned long free_cnt = 0;
		for (unsigned long b = 0; b < fs->blocks_per_group; b++)
			if (!ext4_bm_test(bm, b))
				free_cnt++;
		uint32_t cur = fs->gdt[g].bg_free_blocks_count_lo;
		if (is64)
			cur |= (uint32_t)fs->gdt[g].bg_free_blocks_count_hi
			       << 16;
		if (cur !=
		    (uint32_t)
			    free_cnt) { /* drifted: correct it from the bitmap */
			fs->gdt[g].bg_free_blocks_count_lo = (uint16_t)free_cnt;
			if (is64)
				fs->gdt[g].bg_free_blocks_count_hi =
					(uint16_t)(free_cnt >> 16);
		}
		ext4_block_bitmap_csum_set(
			fs, g,
			bm); /* keep the descriptor csum matching the bitmap */
		ext4_gd_dirty(fs, g);
	}
	kfree(bm);
	fs->st->gd_seen_lo = (unsigned)-1;
	fs->st->gd_seen_hi = 0; /* reset: only re-scan groups touched since now */
}

/* ===================================================================
 * Block numbers read from the disk
 *
 * A block number the driver finds on the disk -- in a block map, an extent
 * or extent index, the extended-attribute block field, a group descriptor,
 * a journal tag -- is checked before the block it names is read, written
 * or freed.  It has to lie inside the filesystem (above the superblock,
 * below the block count, which the mount checked against the device), and
 * it must not be one of the blocks holding the filesystem's own metadata.
 * Those form the system zone, built at mount: the superblock and
 * descriptor copies, each group's bitmaps and inode table, and the
 * journal's blocks, which only the journal inode may map.  A number that
 * fails is corruption: reported through ext4_fs_error (the errors= policy
 * applies), and the operation fails instead of touching the block.
 * =================================================================== */

/* Groups per flex group (1 without flex_bg or with an unusable size). */
static unsigned long ext4_flex_size(const ext4_fs_t *fs)
{
	unsigned lg = fs->sb_copy.s_log_groups_per_flex;

	if (!(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) ||
	    lg < 1 || lg > 31)
		return 1;
	return 1ul << lg;
}

/* Descriptor-table blocks at the start of group `g', behind its superblock
 * copy: the whole table where it is kept whole, with meta_bg the one block
 * of its meta group that the first, second and last group of it hold. */
static unsigned long ext4_bg_num_gdb(const ext4_fs_t *fs, unsigned g)
{
	unsigned long per_block = fs->block_size / fs->desc_size;
	unsigned long metagroup = g / per_block;
	int meta_bg = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) != 0;

	if (!meta_bg || metagroup < fs->sb_copy.s_first_meta_bg) {
		if (!ext4_bg_has_super(fs, g))
			return 0;
		return meta_bg ? fs->sb_copy.s_first_meta_bg :
				 ext4_gdt_blocks(fs);
	}
	unsigned long first = metagroup * per_block;
	if (g == first || g == first + 1 || g == first + per_block - 1)
		return 1;
	return 0;
}

unsigned long ext4_calc_overhead(ext4_fs_t *fs)
{
	unsigned long per_block = fs->block_size / fs->desc_size;
	int meta_bg = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) != 0;
	unsigned long itb = ((unsigned long)fs->inodes_per_group *
				     fs->inode_size +
			     fs->block_size - 1) /
			    fs->block_size;
	unsigned long ov = fs->first_data_block;

	/* bigalloc counts in clusters, which this sum does not know: the
	 * superblock's own figure is taken there when it is sane. */
	if (fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) {
		uint32_t shift = fs->sb_copy.s_log_cluster_size -
				 fs->sb_copy.s_log_block_size;
		unsigned long v = (unsigned long)fs->sb_copy.s_overhead_clusters
				  << (shift < 16 ? shift : 0);

		if (v && v <= fs->blocks_count)
			return v;
	}
	for (unsigned g = 0; g < fs->groups_count; g++) {
		int sup = ext4_bg_has_super(fs, g);

		/* The superblock copy, the descriptor blocks kept in the
		 * group, the blocks reserved behind a whole table for it to
		 * grow, the two bitmaps and the inode table.  Where flex_bg
		 * places a group's bitmaps and inode table does not change
		 * the total. */
		ov += (unsigned long)sup + ext4_bg_num_gdb(fs, g) + 2 + itb;
		if (sup && (!meta_bg || g / per_block < fs->sb_copy.s_first_meta_bg))
			ov += fs->sb_copy.s_reserved_gdt_blocks;
	}
	/* The journal inside the filesystem. */
	if ((fs->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) &&
	    fs->journal_inum) {
		unsigned long jb = fs->j_maxlen;

		if (!jb) {
			ext4_inode ji;

			if (ext4_read_inode_loc(fs, fs->journal_inum, &ji, NULL,
						NULL) == ST_OK)
				jb = (unsigned long)((((uint64_t)ji.i_size_high
						       << 32) |
						      ji.i_size_lo) /
						     fs->block_size);
		}
		ov += jb;
	}
	return ov < fs->blocks_count ? ov : 0;
}

int ext4_blk_valid(const ext4_fs_t *fs, unsigned long ino, unsigned long pbn,
		   unsigned long count)
{
	if (count == 0 || pbn <= fs->first_data_block || pbn + count < pbn ||
	    pbn + count > fs->blocks_count)
		return 0;
	const struct ext4_sysz *z = fs->st->sysz;
	unsigned n = fs->st->sysz_n, lo = 0, hi = n;

	/* The first run that ends after pbn (the runs are sorted and do not
	 * overlap, so their ends are sorted too). */
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;

		if (z[mid].start + z[mid].len <= pbn)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo < n && z[lo].start < pbn + count; lo++)
		if (ino == 0 || z[lo].ino != ino)
			return 0;
	return 1;
}

int ext4_check_descriptors(ext4_fs_t *fs)
{
	int flex = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) != 0;
	unsigned long sb_block = fs->first_data_block;
	unsigned long last_bg_block = sb_block + ext4_bg_num_gdb(fs, 0);
	unsigned long first = fs->first_data_block;
	unsigned long itb = ext4_itable_blocks(fs);

	for (unsigned g = 0; g < fs->groups_count; g++) {
		/* With flex_bg a group's metadata may lie in any group. */
		unsigned long last = (g == fs->groups_count - 1 || flex) ?
					     fs->blocks_count - 1 :
					     first + fs->blocks_per_group - 1;
		struct {
			const char *name;
			unsigned long blk, len;
		} loc[3] = {
			{ "block bitmap", ext4_gd_block_bitmap(fs, g), 1 },
			{ "inode bitmap", ext4_gd_inode_bitmap(fs, g), 1 },
			{ "inode table", ext4_inode_table_block(fs, &fs->gdt[g]),
			  itb },
		};

		for (int k = 0; k < 3; k++) {
			unsigned long s = loc[k].blk;
			unsigned long e = s + loc[k].len - 1;
			const char *why = 0;

			if (s < first || e > last || e < s)
				why = "not in its group";
			else if (s <= last_bg_block)
				why = "on the superblock or the group "
				      "descriptors";
			if (why) {
				kprintf("ext4: %s of group %u (block %lu) is "
					"%s; not mounted\n",
					loc[k].name, g, s, why);
				return ST_INVALID;
			}
		}
		if (!flex)
			first += fs->blocks_per_group;
	}
	return ST_OK;
}

/* Sort the runs by start block (heapsort: no recursion, no extra memory). */
static void ext4_sysz_sort(struct ext4_sysz *z, unsigned long n)
{
	struct ext4_sysz t;

	for (unsigned long i = n / 2; i-- > 0;)
		for (unsigned long r = i, c; (c = 2 * r + 1) < n; r = c) {
			if (c + 1 < n && z[c + 1].start > z[c].start)
				c++;
			if (z[r].start >= z[c].start)
				break;
			t = z[r];
			z[r] = z[c];
			z[c] = t;
		}
	for (unsigned long end = n; end-- > 1;) {
		t = z[0];
		z[0] = z[end];
		z[end] = t;
		for (unsigned long r = 0, c; (c = 2 * r + 1) < end; r = c) {
			if (c + 1 < end && z[c + 1].start > z[c].start)
				c++;
			if (z[r].start >= z[c].start)
				break;
			t = z[r];
			z[r] = z[c];
			z[c] = t;
		}
	}
}

int ext4_setup_system_zone(ext4_fs_t *fs, const struct ext4_sysz *extra,
			   unsigned nextra)
{
	unsigned long flex = ext4_flex_size(fs);
	unsigned long itb = ext4_itable_blocks(fs);
	unsigned long cap = 4ul * fs->groups_count + nextra, n = 0;
	struct ext4_sysz *z;

	if (fs->st->sysz) {
		kfree(fs->st->sysz);
		fs->st->sysz = 0;
		fs->st->sysz_n = 0;
	}
	z = (struct ext4_sysz *)kalloc(cap * sizeof(*z));
	if (!z)
		return ST_NOMEM;
	for (unsigned g = 0; g < fs->groups_count; g++) {
		/* The superblock and descriptor copies: of the first groups
		 * and the first of every flex group, as the copies of the
		 * others are rewritten only by tools that know them. */
		if (ext4_bg_has_super(fs, g) && (g < 5 || g % flex == 0)) {
			unsigned long s = ext4_group_first_block(fs, g);
			unsigned long len = 1 + ext4_bg_num_gdb(fs, g);

			if (s < fs->blocks_count) {
				if (len > fs->blocks_count - s)
					len = fs->blocks_count - s;
				z[n++] = (struct ext4_sysz){ s, len, 0 };
			}
		}
		z[n++] = (struct ext4_sysz){ ext4_gd_block_bitmap(fs, g), 1, 0 };
		z[n++] = (struct ext4_sysz){ ext4_gd_inode_bitmap(fs, g), 1, 0 };
		z[n++] = (struct ext4_sysz){
			ext4_inode_table_block(fs, &fs->gdt[g]), itb, 0
		};
	}
	for (unsigned i = 0; i < nextra; i++)
		z[n++] = extra[i];
	ext4_sysz_sort(z, n);

	/* Overlapping runs are corruption: two groups' metadata in one
	 * place, or a journal block that is also a bitmap.  Runs that touch
	 * and have the same owner become one. */
	unsigned long m = 0;
	for (unsigned long i = 0; i < n; i++) {
		if (m && z[i].start < z[m - 1].start + z[m - 1].len) {
			kprintf("ext4: metadata blocks %lu-%lu overlap blocks "
				"%lu-%lu; not mounted\n",
				z[i].start, z[i].start + z[i].len - 1,
				z[m - 1].start,
				z[m - 1].start + z[m - 1].len - 1);
			kfree(z);
			return ST_INVALID;
		}
		if (m && z[i].start == z[m - 1].start + z[m - 1].len &&
		    z[i].ino == z[m - 1].ino) {
			z[m - 1].len += z[i].len;
			continue;
		}
		z[m++] = z[i];
	}
	fs->st->sysz = z;
	fs->st->sysz_n = (unsigned)m;
	return ST_OK;
}
