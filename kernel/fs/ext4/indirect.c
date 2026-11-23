// LikeOS - ext4 filesystem driver: the classic (ext2/ext3) indirect block map:
// mapping, allocation and freeing
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* A block number read from inode `ino''s block map: 1 if the inode may use
 * it (0 is a hole), else 0, reported. */
static int ext4_ind_ptr_ok(ext4_fs_t *fs, unsigned long ino, unsigned long pbn)
{
	if (pbn == 0 || ext4_blk_valid(fs, ino, pbn, 1))
		return 1;
	ext4_bad_block(fs, ino, "block map", pbn, 1);
	return 0;
}

/* Classic ext2/3 indirect block mapping (for inodes without EXTENTS_FL):
 * ST_OK with *pbn the block or 0 (a hole), ST_IO, ST_NOMEM.  Every block
 * number on the way is checked before it is used: a pointer block outside
 * the data area is never read, and a data block there is never handed out
 * -- the corruption is reported and the lookup fails (ST_IO), as it does
 * when a pointer block cannot be read. */
int ext4_indirect_map(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		      unsigned long lidx, unsigned long *pbn)
{
	const uint32_t *direct = (const uint32_t *)in->i_block;
	unsigned long ppb = fs->block_size / 4;
	unsigned long ptr, idx[3];
	int levels;

	*pbn = 0;
	if (lidx < EXT4_NDIR_BLOCKS) {
		ptr = direct[lidx];
		levels = 0;
	} else if ((lidx -= EXT4_NDIR_BLOCKS) < ppb) {
		ptr = direct[EXT4_IND_BLOCK];
		levels = 1;
		idx[0] = lidx;
	} else if ((lidx -= ppb) < ppb * ppb) {
		ptr = direct[EXT4_DIND_BLOCK];
		levels = 2;
		idx[0] = lidx / ppb;
		idx[1] = lidx % ppb;
	} else {
		lidx -= ppb * ppb;
		if (lidx / (ppb * ppb) >= ppb)
			return ST_OK; /* past the largest map: nothing there */
		ptr = direct[EXT4_TIND_BLOCK];
		levels = 3;
		idx[0] = lidx / (ppb * ppb);
		idx[1] = (lidx % (ppb * ppb)) / ppb;
		idx[2] = lidx % ppb;
	}
	if (!ext4_ind_ptr_ok(fs, ino, ptr))
		return ST_IO;
	if (levels == 0 || ptr == 0) {
		*pbn = ptr;
		return ST_OK;
	}

	uint32_t *buf = (uint32_t *)ext4_bget(fs);
	if (!buf)
		return ST_NOMEM;
	int rc = ST_OK;
	for (int l = 0; l < levels; l++) {
		/* `ptr' is a valid, non-zero pointer block here. */
		if (ext4_read_block(fs, ptr, buf) != ST_OK) {
			rc = ST_IO;
			break;
		}
		ptr = buf[idx[l]];
		if (!ext4_ind_ptr_ok(fs, ino, ptr)) {
			rc = ST_IO;
			break;
		}
		if (ptr == 0)
			break; /* a hole */
		if (l == levels - 1)
			*pbn = ptr;
	}
	ext4_bput(fs, buf);
	return rc;
}

/* The runs mapped below pointer block `blk' (`level' 1: it points at data
 * blocks; 2, 3: at pointer blocks), whose first slot maps logical block
 * `base'. */
static int ext4_ind_walk_blk(ext4_fs_t *fs, struct ext4_walk *w,
			     unsigned long blk, int level, unsigned long base)
{
	unsigned long ppb = fs->block_size / 4, span = 1;
	int rc;

	for (int l = 1; l < level; l++)
		span *= ppb;
	if (!ext4_blk_valid(fs, w->ino, blk, 1)) {
		kprintf("ext4: inode %lu: block map points at block %lu, "
			"outside the data area\n",
			w->ino, blk);
		return ST_INVALID;
	}
	uint32_t *b = (uint32_t *)ext4_bget(fs);
	if (!b)
		return ST_NOMEM;
	rc = ext4_read_block(fs, blk, b);
	for (unsigned long i = 0; rc == ST_OK && i < ppb; i++) {
		unsigned long s = base + i * span;

		if (s >= w->nblocks)
			break;
		if (!b[i])
			continue;
		rc = level == 1 ? ext4_walk_emit(fs, w, s, b[i], 1) :
				  ext4_ind_walk_blk(fs, w, b[i], level - 1, s);
	}
	ext4_bput(fs, b);
	return rc;
}

/* Walk the classic block map of `in' (see struct ext4_walk). */
int ext4_ind_walk(ext4_fs_t *fs, struct ext4_walk *w, const ext4_inode *in)
{
	const uint32_t *p = (const uint32_t *)in->i_block;
	unsigned long ppb = fs->block_size / 4;
	unsigned long base = EXT4_NDIR_BLOCKS, span = ppb;
	int rc = ST_OK;

	for (unsigned long i = 0;
	     rc == ST_OK && i < EXT4_NDIR_BLOCKS && i < w->nblocks; i++)
		if (p[i])
			rc = ext4_walk_emit(fs, w, i, p[i], 1);
	for (int level = 1; rc == ST_OK && level <= 3; level++) {
		unsigned long b = p[EXT4_IND_BLOCK + level - 1];

		if (base >= w->nblocks)
			break;
		if (b)
			rc = ext4_ind_walk_blk(fs, w, b, level, base);
		base += span;
		span *= ppb;
	}
	return rc;
}

/* Allocate `count` data blocks for inode `in`, appending them as extents
 * starting at logical index `start_lidx`.  Returns the number allocated
 * (may be < count if the group/extent-root fills up). */
/* ===================================================================
 * Classic block map (ext2/ext3 inodes) -- WRITE support.
 *
 * i_block[0..11] point at the first twelve data blocks, i_block[12] at a
 * block of pointers to the next block_size/4 ones, i_block[13] at a block of
 * pointers to such blocks, i_block[14] one level deeper still.  A filesystem
 * without the extents feature (ext2, ext3) knows nothing else: an extent
 * inode there is corruption to every other reader.  And a file that an older
 * driver mapped this way on an extents filesystem keeps its map (rewriting
 * i_block as an extent root would lose every block it has).  Pointer blocks
 * carry no checksum; they are metadata (journalled, ext4_write_block).
 * =================================================================== */

/* A new inode's empty block map: an extent root where the filesystem has
 * extents, the classic map (i_block all zero) where it has not.  `in' was
 * zeroed by the caller. */
void ext4_new_block_map(const ext4_fs_t *fs, ext4_inode *in)
{
	if (!(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS))
		return;
	in->i_flags |= EXT4_INODE_EXTENTS_FL;
	ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = 4;
}

/* Whether i_block of `in' is a classic block map that may hold block
 * numbers: a regular file, directory or slow symbolic link without the
 * extents flag.  Not a fast symbolic link (its target is in i_block), an
 * inline-data inode, or a device node (its number is in i_block). */
int ext4_ind_mapped(const ext4_inode *in)
{
	unsigned type = in->i_mode & S_IFMT;

	if (in->i_flags &
	    (EXT4_INODE_EXTENTS_FL | EXT4_INODE_INLINE_DATA_FL))
		return 0;
	if (type == S_IFLNK)
		return ext4_inode_size(in) >= sizeof(in->i_block);
	return type == S_IFREG || type == S_IFDIR;
}

/* Does an allocation for `in' go into an extent tree?  Yes with the flag;
 * no on a filesystem without extents; on one with them, an inode without
 * the flag keeps the classic map it has -- unless nothing is mapped in it
 * yet, then it becomes an extent inode (ext4_ext_add). */
int ext4_alloc_extents(const ext4_fs_t *fs, const ext4_inode *in)
{
	const uint32_t *p = (const uint32_t *)in->i_block;

	if (in->i_flags & EXT4_INODE_EXTENTS_FL)
		return 1;
	if (!(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS))
		return 0;
	if (!ext4_ind_mapped(in))
		return 1;
	for (int i = 0; i < EXT4_N_BLOCKS; i++)
		if (p[i])
			return 0;
	return 1;
}

/* The pointer slots leading to logical block `lidx': off[0] in i_block,
 * off[1..] in the pointer blocks below it.  Returns the number of slots
 * (1: a direct block, 2..4: through one to three pointer blocks), or 0
 * past what the map can address. */
static int ext4_ind_path(unsigned long ppb, unsigned long lidx, unsigned off[4])
{
	if (lidx < EXT4_NDIR_BLOCKS) {
		off[0] = (unsigned)lidx;
		return 1;
	}
	lidx -= EXT4_NDIR_BLOCKS;
	if (lidx < ppb) {
		off[0] = EXT4_IND_BLOCK;
		off[1] = (unsigned)lidx;
		return 2;
	}
	lidx -= ppb;
	if (lidx < ppb * ppb) {
		off[0] = EXT4_DIND_BLOCK;
		off[1] = (unsigned)(lidx / ppb);
		off[2] = (unsigned)(lidx % ppb);
		return 3;
	}
	lidx -= ppb * ppb;
	if (lidx < ppb * ppb * ppb) {
		off[0] = EXT4_TIND_BLOCK;
		off[1] = (unsigned)(lidx / (ppb * ppb));
		off[2] = (unsigned)((lidx / ppb) % ppb);
		off[3] = (unsigned)(lidx % ppb);
		return 4;
	}
	return 0;
}

/* Map `len' blocks pbn.. at logical lidx.. into the classic block map of
 * inode `ino'/`in', allocating the pointer blocks the path needs (zeroed,
 * counted in i_blocks).  The data blocks are already allocated (and their
 * bitmaps stored, see ext4_alloc_blocks_for_file).  Returns the number of
 * blocks mapped (fewer on ENOSPC or past the map's reach). */
unsigned ext4_ind_add(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			     unsigned long lidx, unsigned long pbn, unsigned len)
{
	unsigned long ppb = fs->block_size / 4;
	uint32_t *iblk = (uint32_t *)in->i_block;
	/* One buffer per pointer-block level, kept while consecutive blocks
	 * share it: level l holds the block a level-(l-1) slot points at. */
	uint32_t *buf[3] = { 0, 0, 0 };
	unsigned long bno[3] = { 0, 0, 0 };
	int dirty[3] = { 0, 0, 0 };
	unsigned done = 0;
	int ok = 1;

	for (int l = 0; l < 3; l++) {
		buf[l] = (uint32_t *)ext4_bget(fs);
		if (!buf[l])
			ok = 0;
	}
	while (ok && done < len) {
		unsigned off[4];
		int d = ext4_ind_path(ppb, lidx + done, off);
		unsigned long b = pbn + done;
		uint32_t *parent = iblk;

		if (d == 0 || b > 0xFFFFFFFFUL)
			break; /* not addressable by a 32-bit map */
		for (int l = 1; l < d && ok; l++) {
			uint32_t *slot = &parent[off[l - 1]];
			int pl = l - 2; /* parent's buffer level (-1: inode) */

			if (*slot == 0) {
				unsigned long nb = ext4_alloc_one_block(fs);
				if (nb == 0 || nb > 0xFFFFFFFFUL) {
					if (nb)
						ext4_free_block(fs, nb);
					ok = 0;
					break;
				}
				if (dirty[l - 1])
					ext4_write_block(fs, bno[l - 1],
							 buf[l - 1]);
				mm_memset(buf[l - 1], 0, fs->block_size);
				bno[l - 1] = nb;
				dirty[l - 1] = 1;
				*slot = (uint32_t)nb;
				if (pl >= 0)
					dirty[pl] = 1;
				in->i_blocks_lo += fs->block_size / 512;
			} else if (bno[l - 1] != *slot) {
				if (!ext4_blk_valid(fs, ino, *slot, 1)) {
					ext4_bad_block(fs, ino, "block map",
						       *slot, 1);
					ok = 0;
					break;
				}
				if (dirty[l - 1])
					ext4_write_block(fs, bno[l - 1],
							 buf[l - 1]);
				dirty[l - 1] = 0;
				bno[l - 1] = 0;
				if (ext4_read_block(fs, *slot, buf[l - 1]) !=
				    ST_OK) {
					ok = 0;
					break;
				}
				bno[l - 1] = *slot;
			}
			parent = buf[l - 1];
		}
		if (!ok)
			break;
		parent[off[d - 1]] = (uint32_t)b;
		if (d >= 2)
			dirty[d - 2] = 1;
		done++;
	}
	for (int l = 0; l < 3; l++) {
		if (buf[l] && dirty[l])
			ext4_write_block(fs, bno[l], buf[l]);
		if (buf[l])
			ext4_bput(fs, buf[l]);
	}
	return done;
}

/* Free a run of data blocks gathered by ext4_ind_free_tree. */
static void ext4_ind_run_flush(ext4_fs_t *fs, unsigned long *rs,
			       unsigned long *rl)
{
	if (*rl)
		ext4_free_blocks_run(fs, *rs, *rl);
	*rl = 0;
}

/* Free what pointer block `blk' (`level' 1: it points at data blocks; 2, 3:
 * at pointer blocks of the level below) maps at logical index >= `from';
 * `base' is the logical index of its first slot.  Freed blocks (data and
 * pointer blocks) are counted in *freed.  Returns 1 if nothing is left
 * mapped under it (the caller frees the block itself), 0 if it keeps some
 * (it is stored with the freed slots cleared). */
static int ext4_ind_free_tree(ext4_fs_t *fs, unsigned long ino,
			      unsigned long blk, int level,
			      unsigned long base, unsigned long from,
			      unsigned long *freed)
{
	unsigned long ppb = fs->block_size / 4;
	unsigned long span = 1;
	unsigned long rs = 0, rl = 0;
	int kept = 0, changed = 0;
	uint32_t *b = (uint32_t *)ext4_bget(fs);

	for (int l = 1; l < level; l++)
		span *= ppb;
	if (!b)
		return 0; /* no memory: leave the subtree as it is */
	/* A pointer block outside the data area is neither read nor
	 * rewritten nor freed: the slot naming it stays, reported. */
	if (!ext4_blk_valid(fs, ino, blk, 1)) {
		ext4_bad_block(fs, ino, "block map", blk, 1);
		ext4_bput(fs, b);
		return 0;
	}
	if (ext4_read_block(fs, blk, b) != ST_OK) {
		ext4_bput(fs, b);
		return 0;
	}
	for (unsigned long i = 0; i < ppb; i++) {
		unsigned long s = base + i * span;

		if (!b[i])
			continue;
		if (s + span <= from) {
			kept = 1;
			continue;
		}
		if (level == 1) { /* a data block (s >= from here) */
			if (rl && rs + rl == b[i]) {
				rl++;
			} else {
				ext4_ind_run_flush(fs, &rs, &rl);
				rs = b[i];
				rl = 1;
			}
			(*freed)++;
		} else if (ext4_ind_free_tree(fs, ino, b[i], level - 1, s, from,
					      freed)) {
			ext4_free_block(fs, b[i]);
			(*freed)++;
		} else {
			kept = 1;
			continue;
		}
		b[i] = 0;
		changed = 1;
	}
	ext4_ind_run_flush(fs, &rs, &rl);
	if (kept && changed)
		ext4_write_block(fs, blk, b);
	ext4_bput(fs, b);
	return !kept;
}

/* Free every block of the classic map of `in' at logical index >= `from'
 * (data blocks, and pointer blocks left with nothing under them); fixes
 * i_blocks.  The caller stores the inode. */
void ext4_ind_free_from(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			unsigned long from)
{
	uint32_t *p = (uint32_t *)in->i_block;
	unsigned long ppb = fs->block_size / 4;
	unsigned long freed = 0, rs = 0, rl = 0;
	unsigned long base = EXT4_NDIR_BLOCKS, span = ppb;

	for (unsigned long i = from; i < EXT4_NDIR_BLOCKS; i++) {
		if (!p[i])
			continue;
		if (rl && rs + rl == p[i]) {
			rl++;
		} else {
			ext4_ind_run_flush(fs, &rs, &rl);
			rs = p[i];
			rl = 1;
		}
		freed++;
		p[i] = 0;
	}
	ext4_ind_run_flush(fs, &rs, &rl);
	for (int level = 1; level <= 3; level++) {
		unsigned slot = EXT4_IND_BLOCK + (unsigned)(level - 1);

		if (p[slot] && base + span > from &&
		    ext4_ind_free_tree(fs, ino, p[slot], level, base, from,
				       &freed)) {
			ext4_free_block(fs, p[slot]);
			freed++;
			p[slot] = 0;
		}
		base += span;
		span *= ppb;
	}
	unsigned long sub = freed * (fs->block_size / 512);
	in->i_blocks_lo = (in->i_blocks_lo >= sub) ? in->i_blocks_lo - sub : 0;
}
