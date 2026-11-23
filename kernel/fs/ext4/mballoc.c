// LikeOS - ext4 filesystem driver: block allocation and freeing, blocks freed but
// not yet committed
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* Free a contiguous run of data blocks [start, start+len).  For each affected
 * group the bitmap is read, ALL the run's bits cleared, and the GD/super free
 * counts bumped — ONCE per group, not once per block.  Per-block freeing would
 * re-read + re-write the same group bitmap for every block: unlinking a 100 MB
 * file is 25600 single-bit RMWs, a multi-second stall that looks like a hang. */
/* ---- Blocks freed but not yet committed ---------------------------------
 *
 * Freeing a block clears its bit in the bitmap, but the bitmap reaches the
 * disk only with the journal batch.  Until that batch is committed, a crash
 * brings back the old owner -- the file or directory the operation removed
 * -- with the block still in it.  Handing the block out again before then
 * is safe for metadata (journalled with or after the free), but not for
 * file data, which is written in place at once: a crash would leave the
 * old owner holding the new file's bytes (a directory block overwritten by
 * file data, seen in the power-cut sweeps).  So a freed block is not
 * allocated again until the batch with its free is committed; the
 * allocators skip the runs recorded here, and commit the batch first when
 * only such blocks are left. */

/* Record a run of blocks the running operation freed. */
static void ext4_pf_add(ext4_fs_t *fs, unsigned long start, unsigned long len)
{
	struct ext4_state *st = fs->st;

	if (!fs->j_enabled || len == 0)
		return;
	if (st->pf_n > st->pf_merged &&
	    st->pf[st->pf_n - 1].start + st->pf[st->pf_n - 1].len == start) {
		st->pf[st->pf_n - 1].len += len;
		return;
	}
	if (st->pf_n == st->pf_cap) {
		unsigned nc = st->pf_cap ? st->pf_cap * 2 : 32;
		struct ext4_pf_run *np = (struct ext4_pf_run *)kalloc(
			nc * sizeof(struct ext4_pf_run));
		if (!np) {
			/* Not recordable: the last run takes this one in,
			 * covering the blocks between (over-protecting). */
			if (st->pf_n > st->pf_merged) {
				struct ext4_pf_run *r = &st->pf[st->pf_n - 1];
				unsigned long lo = r->start < start ? r->start :
								      start;
				unsigned long hi = r->start + r->len;

				if (start + len > hi)
					hi = start + len;
				r->start = lo;
				r->len = hi - lo;
			} else {
				WARN_ON_ONCE(1);
			}
			return;
		}
		if (st->pf_n)
			mm_memcpy(np, st->pf, st->pf_n * sizeof(*np));
		if (st->pf)
			kfree(st->pf);
		st->pf = np;
		st->pf_cap = nc;
	}
	st->pf[st->pf_n].start = start;
	st->pf[st->pf_n].len = len;
	st->pf_n++;
}

/* The batch was committed: the frees of the operations merged into it
 * (entries [0, upto)) are on disk, their blocks may be used again. */
void ext4_pf_release(ext4_fs_t *fs, unsigned upto)
{
	struct ext4_state *st = fs->st;

	if (upto > st->pf_n)
		upto = st->pf_n;
	if (upto == 0)
		return;
	for (unsigned i = upto; i < st->pf_n; i++)
		st->pf[i - upto] = st->pf[i];
	st->pf_n -= upto;
	st->pf_merged = st->pf_merged > upto ? st->pf_merged - upto : 0;
}

/* Mark the blocks of group `g' that are freed but not committed as in use
 * in `scan' (a copy of the group's bitmap the allocator searches).
 * Returns 1 if there were any. */
int ext4_pf_mask(ext4_fs_t *fs, unsigned g, uint8_t *scan)
{
	struct ext4_state *st = fs->st;
	unsigned long base = fs->first_data_block +
			     (unsigned long)g * fs->blocks_per_group;
	unsigned long end = base + fs->blocks_per_group;
	int any = 0;

	for (unsigned i = 0; i < st->pf_n; i++) {
		unsigned long a = st->pf[i].start;
		unsigned long b = a + st->pf[i].len;

		if (b <= base || a >= end)
			continue;
		if (a < base)
			a = base;
		if (b > end)
			b = end;
		for (unsigned long k = a; k < b; k++)
			ext4_bm_set(scan, k - base);
		any = 1;
	}
	return any;
}

/* Allocation found nothing but blocks freed by uncommitted operations:
 * commit the batch (the running operation's own frees stay pending) so
 * that those of earlier operations can be used.  Returns 1 if that may
 * have made blocks available. */
int ext4_pf_commit_for_space(ext4_fs_t *fs)
{
	unsigned before = fs->st->pf_n;

	if (!fs->j_enabled || fs->st->pf_merged == 0)
		return 0;
	ext4_journal_flush(fs);
	return fs->st->pf_n < before;
}

void ext4_free_blocks_run(ext4_fs_t *fs, unsigned long start,
				 unsigned long len)
{
	/* Blocks outside the data area -- a damaged block map or extent --
	 * are not freed: clearing their bits would hand metadata, or bits
	 * past the end of the filesystem, to the next allocation. */
	if (!ext4_blk_valid(fs, 0, start, len)) {
		ext4_bad_block(fs, 0, "freeing blocks", start, len);
		return;
	}
	ext4_pf_add(fs, start, len);
	/* If the run being freed overlaps the write-back buffer, flush it first so the
     * buffer never holds data for now-free (and possibly soon-reallocated) blocks. */
	if (fs->st->wb_len > 0 && start < fs->st->wb_pbn + fs->st->wb_len &&
	    fs->st->wb_pbn < start + len)
		ext4_wb_flush(fs);
	uint8_t *bm = 0;
	while (len > 0) {
		if (start < fs->first_data_block) {
			start++;
			len--;
			continue;
		}
		unsigned g = (unsigned)((start - fs->first_data_block) /
					fs->blocks_per_group);
		if (g >= fs->groups_count)
			break;
		unsigned long gbase = fs->first_data_block +
				      (unsigned long)g * fs->blocks_per_group;
		unsigned long i =
			start - gbase; /* first index within group   */
		unsigned long n = fs->blocks_per_group -
				  i; /* blocks of the run in group */
		if (n > len)
			n = len;
		/* A freed block's journal copy must never be replayed over its
		 * next use.  Both copies are in memory only, so forget them --
		 * see ext4_drop_journal_copy(fs).  This used to force a full
		 * durable checkpoint instead, which made every unlink of a
		 * recently-written file wait on the device. */
		for (unsigned long k = 0; k < n; k++)
			ext4_drop_journal_copy(fs, start + k);
		if (!bm) {
			bm = (uint8_t *)kalloc(fs->block_size);
			if (!bm)
				return;
		}
		/* Group lock: this group's block-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			kfree(bm);
			return;
		}
		if (lst == ST_OK) {
			unsigned long init_free =
				uninit ? ext4_bitmap_free_blocks(fs, g, bm) : 0;
			unsigned long cleared = 0;
			for (unsigned long k = 0; k < n; k++)
				if (ext4_bm_test(bm, i + k)) {
					ext4_bm_clear(bm, i + k);
					cleared++;
				}
			if (cleared) {
				if (uninit)
					ext4_bg_block_uninit_clear(fs, g,
								   init_free);
				ext4_block_bitmap_csum_set(fs, g, bm);
				ext4_write_block(fs, bblk, bm);
				fs->gdt[g].bg_free_blocks_count_lo =
					(uint16_t)(fs->gdt[g]
							   .bg_free_blocks_count_lo +
						   cleared);
				fs->sb_copy.s_free_blocks_count_lo +=
					(uint32_t)cleared;
				ext4_gd_dirty(fs, g);
			}
		}
		ext4_bg_unlock(fs, g);
		for (unsigned long k = 0; k < n; k++)
			ext4_mbc_drop(fs, start +
				      k); /* avoid stale cache if reallocated */
		start += n;
		len -= n;
	}
	if (bm)
		kfree(bm);
}

void ext4_free_block(ext4_fs_t *fs, unsigned long pbn)
{
	ext4_free_blocks_run(fs, pbn, 1);
}

/* ===================================================================
 * Extent-tree WRITE support — growth at the end, to arbitrary depth.
 *
 * Blocks added past the last extent (a file or directory growing at its
 * end, the common case) go in at the RIGHTMOST path of the tree; anything
 * else -- a hole being filled -- is handed to the general sorted insertion
 * further down (ext4_ext_insert_run).
 * That makes growth a clean "add to the rightmost leaf; on full, build a new
 * rightmost branch under the lowest ancestor with room; if none, grow a new
 * level on top".  External index/leaf blocks carry the et_checksum tail
 * (ext4_ext_block_csum_set); the inline root is covered by the inode csum.
 * Metadata blocks are allocated via ext4_alloc_one_block, so the caller must
 * have already persisted the data-block bitmaps (see the two-pass
 * reserve-then-map allocator) to avoid double-allocation.
 * =================================================================== */

/* ---- external xattr block (i_file_acl) ----  Large attributes that
 * don't fit the inode slack live in one shared-format block: a 32-byte header
 * then forward-growing entries and backward-growing values, with per-entry and
 * whole-block hashes (legacy, always present) plus a crc32c (metadata_csum).
 * All three are validated by e2fsck, so they must be byte-exact. */

/* Allocate one block from the bitmaps, not attached to any inode (for the xattr
 * block).  Returns the physical block number or 0.  Caller adjusts i_blocks. */
unsigned long ext4_alloc_one_block(ext4_fs_t *fs)
{
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	uint8_t *scan = (uint8_t *)kalloc(fs->block_size);
	if (!bm || !scan) {
		if (bm)
			kfree(bm);
		if (scan)
			kfree(scan);
		return 0;
	}
	unsigned long pbn = 0;
	/* Blocks freed by uncommitted operations are skipped, and committed
	 * for a second round if nothing else is left, as in
	 * ext4_alloc_blocks_for_file. */
	for (int round = 0; round < 2 && !pbn; round++) {
		if (round && !ext4_pf_commit_for_space(fs))
			break;
		for (unsigned g = 0; g < fs->groups_count && !pbn; g++) {
			if (fs->gdt[g].bg_free_blocks_count_lo == 0)
				continue;
			/* Group lock: this group's block-bitmap read-modify-write. */
			ext4_bg_lock(fs, g);
			unsigned long bblk = ext4_gd_block_bitmap(fs, g);
			int uninit = 0;
			int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
			if (lst == EXT4_BM_CORRUPT) {
				ext4_bg_unlock(fs, g);
				round = 2;
				break;
			}
			if (lst != ST_OK) {
				ext4_bg_unlock(fs, g);
				continue;
			}
			unsigned long base =
				fs->first_data_block +
				(unsigned long)g * fs->blocks_per_group;
			const uint8_t *t = bm;
			if (fs->st->pf_n) {
				mm_memcpy(scan, bm, fs->block_size);
				if (ext4_pf_mask(fs, g, scan))
					t = scan;
			}
			/* Only the blocks the group has: a damaged bitmap
			 * may show the padding past the last one free. */
			unsigned nb = ext4_blocks_in_group(fs, g);
			for (unsigned i = 0; i < nb; i++) {
				if (ext4_bm_test(t, i))
					continue;
				/* A free bit on a metadata block is a damaged
				 * bitmap: nothing is allocated from it. */
				if (!ext4_blk_valid(fs, 0, base + i, 1)) {
					ext4_bad_block(fs, 0,
						       "block bitmap marks "
						       "metadata free",
						       base + i, 1);
					round = 2;
					break;
				}
				if (uninit)
					ext4_bg_block_uninit_clear(
						fs, g,
						ext4_bitmap_free_blocks(fs, g,
									bm));
				ext4_bm_set(bm, i);
				ext4_block_bitmap_csum_set(fs, g, bm);
				ext4_write_block(fs, bblk, bm);
				fs->gdt[g].bg_free_blocks_count_lo--;
				fs->sb_copy.s_free_blocks_count_lo--;
				ext4_gd_dirty(fs, g);
				pbn = base + i;
				break;
			}
			ext4_bg_unlock(fs, g);
			if (round == 2)
				break; /* damaged bitmap: stop */
		}
	}
	kfree(scan);
	kfree(bm);
	return pbn;
}
