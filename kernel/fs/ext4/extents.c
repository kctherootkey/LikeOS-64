// LikeOS - ext4 filesystem driver: extent trees: mapping, growth, node splits,
// sorted insertion and freeing
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Logical -> physical block mapping (extents + classic indirect)
 * =================================================================== */

#define EXT4_EXT_DEPTH_MAX 5 /* levels below the root */

static int ext4_ext_hdr_ok(const ext4_fs_t *fs, const ext4_extent_header *h,
			   unsigned want_depth, int is_root);

/* Walk an extent tree to map logical block `lidx` -> physical block: ST_OK
 * with *pbn the block or 0 (a hole / past the end), ST_IO when the tree is
 * damaged or a node cannot be read, ST_NOMEM.  Every node is checked
 * before its entries are used (its header against its place in the tree,
 * its checksum, an index entry's block and the extent found against the
 * data area): damage is reported through ext4_fs_error and fails the
 * lookup -- it never reads as a hole. */
int ext4_extent_map(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		    unsigned long lidx, unsigned long *pbn)
{
	const uint8_t *node = in->i_block; /* root lives inline in the inode */
	const ext4_extent_header *root = (const ext4_extent_header *)node;
	uint8_t *heap = 0;
	int rc = ST_OK;

	*pbn = 0;
	if (root->eh_magic != EXT4_EXT_MAGIC ||
	    root->eh_depth > EXT4_EXT_DEPTH_MAX ||
	    !ext4_ext_hdr_ok(fs, root, root->eh_depth, 1)) {
		ext4_fs_error(fs, "bad extent tree root", ino);
		return ST_IO;
	}
	for (;;) {
		const ext4_extent_header *eh = (const ext4_extent_header *)node;
		unsigned entries = eh->eh_entries;
		if (eh->eh_depth == 0) {
			const ext4_extent *ex = (const ext4_extent *)(eh + 1);
			for (unsigned i = 0; i < entries; i++) {
				unsigned long start = ex[i].ee_block;
				unsigned len = ex[i].ee_len;
				if (len > 32768)
					len -= 32768; /* uninitialized extent */
				if (lidx >= start && lidx < start + len) {
					unsigned long phys =
						(unsigned long)ex[i]
							.ee_start_lo |
						((unsigned long)ex[i]
							 .ee_start_hi
						 << 32);
					if (!ext4_blk_valid(fs, ino, phys,
							    len)) {
						ext4_bad_block(fs, ino,
							       "extent", phys,
							       len);
						rc = ST_IO;
						goto done;
					}
					*pbn = phys + (lidx - start);
					goto done;
				}
			}
			break; /* no covering extent: hole / EOF */
		} else {
			const ext4_extent_idx *ix =
				(const ext4_extent_idx *)(eh + 1);
			int chosen = -1;
			for (unsigned i = 0; i < entries; i++) {
				if (ix[i].ei_block <= lidx)
					chosen = (int)i;
				else
					break;
			}
			if (chosen < 0)
				break;
			unsigned long child =
				(unsigned long)ix[chosen].ei_leaf_lo |
				((unsigned long)ix[chosen].ei_leaf_hi << 32);
			if (!ext4_blk_valid(fs, ino, child, 1)) {
				ext4_bad_block(fs, ino, "extent index", child,
					       1);
				rc = ST_IO;
				break;
			}
			if (!heap) {
				heap = (uint8_t *)ext4_bget(fs);
				if (!heap) {
					rc = ST_NOMEM;
					break;
				}
			}
			unsigned want = eh->eh_depth - 1u;
			if (ext4_read_block(fs, child, heap) != ST_OK) {
				rc = ST_IO;
				break;
			}
			if (!ext4_mbc_verified(fs, child)) {
				if (!ext4_extent_block_csum_ok(
					    fs, ino, in->i_generation, heap)) {
					ext4_fs_error(
						fs,
						"extent block checksum mismatch",
						ino);
					rc = ST_IO;
					break;
				}
				ext4_mbc_mark_verified(fs, child);
			}
			if (!ext4_ext_hdr_ok(fs,
					     (const ext4_extent_header *)heap,
					     want, 0)) {
				ext4_fs_error(fs, "bad extent tree node", ino);
				rc = ST_IO;
				break;
			}
			node = heap; /* descend */
		}
	}
done:
	if (heap)
		ext4_bput(fs, heap);
	return rc;
}

/* Entries an EXTERNAL extent block holds (reserving the 4-byte csum tail). */
static unsigned ext4_ext_extern_max(const ext4_fs_t *fs)
{
	return (fs->block_size - sizeof(ext4_extent_header) - 4) /
	       sizeof(ext4_extent);
}

/* Stamp the et_checksum tail of an external extent block (no-op w/o csum). */
static void ext4_ext_block_csum_set(const ext4_fs_t *fs, unsigned long ino,
				    uint32_t gen, uint8_t *blk)
{
	if (!fs->has_metadata_csum)
		return;
	const ext4_extent_header *eh = (const ext4_extent_header *)blk;
	unsigned long off = sizeof(ext4_extent_header) +
			    (unsigned long)eh->eh_max * sizeof(ext4_extent);
	if (off + 4 > fs->block_size)
		return;
	uint32_t seed = ext4_inode_csum_seed(fs, ino, gen);
	uint32_t c = ext4_crc32c(seed, blk, off);
	mm_memcpy(blk + off, &c, 4);
}

/* Push the inline root down into a new external block, making the root a single
 * index entry pointing to it (depth++).  Works at any depth (the copied block
 * keeps the root's old depth). */
static int ext4_ext_grow_depth(ext4_fs_t *fs, unsigned long ino, ext4_inode *in)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
	unsigned long b = ext4_alloc_one_block(fs);
	if (!b)
		return ST_NOSPC;
	uint8_t *buf = (uint8_t *)kalloc(bs);
	if (!buf) {
		ext4_free_block(fs, b);
		return ST_NOMEM;
	}
	mm_memset(buf, 0, bs);
	unsigned cnt = eh->eh_entries;
	/* The new root's lone index entry covers from the subtree's first logical
     * block (0 for a normal file, the first extent's block for a sparse one).
     * Read it from the still-intact inline root before we overwrite it below. */
	uint32_t first_block;
	mm_memcpy(&first_block, in->i_block + sizeof(ext4_extent_header), 4);
	mm_memcpy(buf, in->i_block,
		  sizeof(ext4_extent_header) +
			  (unsigned long)cnt * sizeof(ext4_extent));
	((ext4_extent_header *)buf)->eh_max = (uint16_t)ext4_ext_extern_max(fs);
	ext4_ext_block_csum_set(fs, ino, gen, buf);
	int st = ext4_write_block(fs, b, buf);
	kfree(buf);
	if (st != ST_OK) {
		ext4_free_block(fs, b);
		return st;
	}
	eh->eh_depth = (uint16_t)(eh->eh_depth + 1);
	eh->eh_entries = 1;
	eh->eh_max = 4;
	ext4_extent_idx *ix = (ext4_extent_idx *)(eh + 1);
	ix[0].ei_block = first_block;
	ix[0].ei_leaf_lo = (uint32_t)b;
	ix[0].ei_leaf_hi = (uint16_t)(b >> 32);
	ix[0].ei_unused = 0;
	in->i_blocks_lo += bs / 512; /* B is a metadata block */
	return ST_OK;
}

/* The rightmost leaf is full: build a fresh rightmost branch holding the new
 * extent (lidx,pbn,chunk).  pbuf[1..depth]/pblk[1..depth] are the descended
 * rightmost external nodes; level 0 is the inline root. */
static int ext4_ext_grow_branch(ext4_fs_t *fs, unsigned long ino,
				ext4_inode *in, uint8_t *pbuf[],
				unsigned long pblk[], unsigned depth,
				unsigned long lidx, unsigned long pbn,
				unsigned chunk)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	unsigned emax = ext4_ext_extern_max(fs);

	/* Lowest ancestor (level) with room for one more child pointer. */
	int A = (int)depth - 1;
	while (A >= 0) {
		ext4_extent_header *h =
			(A == 0) ? (ext4_extent_header *)in->i_block :
				   (ext4_extent_header *)pbuf[A];
		if (h->eh_entries < h->eh_max)
			break;
		A--;
	}
	if (A < 0) { /* whole path full: add a new level */
		int rc = ext4_ext_grow_depth(fs, ino, in);
		if (rc != ST_OK)
			return rc;
		A = 0;
		depth = ((ext4_extent_header *)in->i_block)
				->eh_depth; /* grew by 1 */
	}

	/* Allocate the new branch blocks: levels A+1 .. depth (leaf at depth). */
	unsigned nlev = depth - (unsigned)A;
	if (nlev > 8)
		return ST_NOSPC;
	unsigned long nb[8];
	unsigned got = 0;
	for (unsigned i = 0; i < nlev; i++) {
		unsigned long b = ext4_alloc_one_block(fs);
		if (!b) {
			for (unsigned j = 0; j < got; j++)
				ext4_free_block(fs, nb[j]);
			return ST_NOSPC;
		}
		nb[got++] = b;
	}
	uint8_t *buf = (uint8_t *)kalloc(bs);
	if (!buf) {
		for (unsigned j = 0; j < nlev; j++)
			ext4_free_block(fs, nb[j]);
		return ST_NOMEM;
	}

	/* Leaf (deepest new block = nb[nlev-1]). */
	mm_memset(buf, 0, bs);
	{
		ext4_extent_header *h = (ext4_extent_header *)buf;
		h->eh_magic = EXT4_EXT_MAGIC;
		h->eh_entries = 1;
		h->eh_max = (uint16_t)emax;
		h->eh_depth = 0;
		ext4_extent *e = (ext4_extent *)(h + 1);
		e->ee_block = (uint32_t)lidx;
		e->ee_len = (uint16_t)chunk;
		e->ee_start_hi = (uint16_t)(pbn >> 32);
		e->ee_start_lo = (uint32_t)pbn;
		ext4_ext_block_csum_set(fs, ino, gen, buf);
		if (ext4_write_block(fs, nb[nlev - 1], buf) != ST_OK)
			goto io_fail;
	}

	/* Interior new index nodes, level depth-1 .. A+1. */
	for (int lvl = (int)depth - 1; lvl >= A + 1; lvl--) {
		unsigned bi = (unsigned)(lvl - (A + 1));
		unsigned long childblk = nb[bi + 1];
		mm_memset(buf, 0, bs);
		ext4_extent_header *h = (ext4_extent_header *)buf;
		h->eh_magic = EXT4_EXT_MAGIC;
		h->eh_entries = 1;
		h->eh_max = (uint16_t)emax;
		h->eh_depth = (uint16_t)(depth - lvl);
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix->ei_block = (uint32_t)lidx;
		ix->ei_leaf_lo = (uint32_t)childblk;
		ix->ei_leaf_hi = (uint16_t)(childblk >> 32);
		ix->ei_unused = 0;
		ext4_ext_block_csum_set(fs, ino, gen, buf);
		if (ext4_write_block(fs, nb[bi], buf) != ST_OK)
			goto io_fail;
	}
	kfree(buf);

	/* Link the top new node (nb[0]) into the ancestor with room (path[A]). */
	if (A == 0) {
		ext4_extent_header *h = (ext4_extent_header *)in->i_block;
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix[h->eh_entries].ei_block = (uint32_t)lidx;
		ix[h->eh_entries].ei_leaf_lo = (uint32_t)nb[0];
		ix[h->eh_entries].ei_leaf_hi = (uint16_t)(nb[0] >> 32);
		ix[h->eh_entries].ei_unused = 0;
		h->eh_entries++; /* inline root persisted by caller */
	} else {
		ext4_extent_header *h = (ext4_extent_header *)pbuf[A];
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix[h->eh_entries].ei_block = (uint32_t)lidx;
		ix[h->eh_entries].ei_leaf_lo = (uint32_t)nb[0];
		ix[h->eh_entries].ei_leaf_hi = (uint16_t)(nb[0] >> 32);
		ix[h->eh_entries].ei_unused = 0;
		h->eh_entries++;
		ext4_ext_block_csum_set(fs, ino, gen, pbuf[A]);
		if (ext4_write_block(fs, pblk[A], pbuf[A]) != ST_OK)
			return ST_IO;
	}
	in->i_blocks_lo +=
		nlev * (bs / 512); /* all new branch blocks are metadata */
	return ST_OK;
io_fail:
	kfree(buf);
	for (unsigned j = 0; j < nlev; j++)
		ext4_free_block(fs, nb[j]);
	return ST_IO;
}

static unsigned ext4_ext_insert_run(ext4_fs_t *fs, unsigned long ino,
				    ext4_inode *in, unsigned long lblk,
				    unsigned long pblk,
				    unsigned len); /* defined below */

/* Append `addlen` contiguous blocks (lidx,pbn..) to the inode's extent tree,
 * coalescing with the rightmost extent and growing the tree as needed.  Returns
 * the number of blocks actually mapped (== addlen on success, fewer if the tree
 * could not grow).  May allocate metadata blocks (caller flushes data bitmaps). */
unsigned ext4_ext_add(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			     unsigned long lidx, unsigned long pbn,
			     unsigned addlen)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	unsigned done = 0;
	ext4_extent_header *reh = (ext4_extent_header *)in->i_block;
	if (!(in->i_flags & EXT4_INODE_EXTENTS_FL) ||
	    reh->eh_magic != EXT4_EXT_MAGIC) {
		reh->eh_magic = EXT4_EXT_MAGIC;
		reh->eh_entries = 0;
		reh->eh_max = 4;
		reh->eh_depth = 0;
		reh->eh_generation = 0;
		in->i_flags |= EXT4_INODE_EXTENTS_FL;
	}

	while (addlen > 0) {
		unsigned depth = ((ext4_extent_header *)in->i_block)->eh_depth;
		/* The nodes on the way are written back with entries added:
		 * each has to be what its place in the tree says, and an index
		 * entry has to point into the data area, before it is used. */
		if (depth > EXT4_EXT_DEPTH_MAX ||
		    !ext4_ext_hdr_ok(fs, (ext4_extent_header *)in->i_block,
				     depth, 1)) {
			ext4_fs_error(fs, "bad extent tree root", ino);
			break;
		}
		uint8_t *pbuf[8] = { 0 };
		unsigned long pblk[8] = { 0 };
		uint8_t *node = in->i_block;
		int ok = 1;
		for (unsigned d = 0; d < depth; d++) {
			ext4_extent_header *h = (ext4_extent_header *)node;
			unsigned cnt = h->eh_entries;
			if (cnt == 0) {
				ok = 0;
				break;
			}
			ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
			unsigned long child =
				(unsigned long)ix[cnt - 1].ei_leaf_lo |
				((unsigned long)ix[cnt - 1].ei_leaf_hi << 32);
			if (!ext4_blk_valid(fs, ino, child, 1)) {
				ext4_bad_block(fs, ino, "extent index", child,
					       1);
				ok = 0;
				break;
			}
			uint8_t *cb = (uint8_t *)kalloc(bs);
			if (!cb || ext4_read_block(fs, child, cb) != ST_OK) {
				if (cb)
					kfree(cb);
				ok = 0;
				break;
			}
			pbuf[d + 1] = cb;
			pblk[d + 1] = child;
			node = cb;
			if (!ext4_ext_hdr_ok(fs, (ext4_extent_header *)cb,
					     depth - d - 1, 0)) {
				ext4_fs_error(fs, "bad extent tree node", ino);
				ok = 0;
				break;
			}
		}
		if (!ok) {
			for (unsigned d = 1; d <= depth; d++)
				if (pbuf[d])
					kfree(pbuf[d]);
			break;
		}

		ext4_extent_header *leh = (ext4_extent_header *)node;
		ext4_extent *ex = (ext4_extent *)(leh + 1);
		int is_inline = (depth == 0);
		unsigned chunk = addlen > 32768 ? 32768 : addlen;
		int handled = 0;

		if (leh->eh_entries > 0) {
			ext4_extent *last = &ex[leh->eh_entries - 1];
			unsigned llen = last->ee_len;
			if (llen > 32768)
				llen -= 32768;
			/* Not past the last extent: a hole inside the file
			 * is being filled, and the extent has to go where it
			 * belongs in the order, not at the end. */
			if (lidx < (unsigned long)last->ee_block + llen) {
				for (unsigned d = 1; d <= depth; d++)
					if (pbuf[d])
						kfree(pbuf[d]);
				return done + ext4_ext_insert_run(fs, ino, in,
								  lidx, pbn,
								  addlen);
			}
		}
		if (leh->eh_entries > 0) { /* coalesce with the last extent? */
			ext4_extent *last = &ex[leh->eh_entries - 1];
			unsigned llen = last->ee_len;
			if (llen > 32768)
				llen -= 32768;
			unsigned long lphys =
				(unsigned long)last->ee_start_lo |
				((unsigned long)last->ee_start_hi << 32);
			if (last->ee_block + llen == lidx &&
			    lphys + llen == pbn && llen < 32768) {
				unsigned c = 32768 - llen;
				if (c > addlen)
					c = addlen;
				last->ee_len = (uint16_t)(llen + c);
				chunk = c;
				handled = 1;
			}
		}
		if (!handled &&
		    leh->eh_entries < leh->eh_max) { /* add a new extent */
			ext4_extent *ne = &ex[leh->eh_entries];
			ne->ee_block = (uint32_t)lidx;
			ne->ee_len = (uint16_t)chunk;
			ne->ee_start_hi = (uint16_t)(pbn >> 32);
			ne->ee_start_lo = (uint32_t)pbn;
			leh->eh_entries++;
			handled = 1;
		}

		int rc = ST_OK;
		if (handled) {
			if (!is_inline) {
				ext4_ext_block_csum_set(fs, ino, gen, node);
				rc = ext4_write_block(fs, pblk[depth], node);
			} /* inline root persisted by the caller */
		} else {
			rc = ext4_ext_grow_branch(fs, ino, in, pbuf, pblk,
						  depth, lidx, pbn, chunk);
		}
		for (unsigned d = 1; d <= depth; d++)
			if (pbuf[d])
				kfree(pbuf[d]);
		if (rc != ST_OK)
			break;
		lidx += chunk;
		pbn += chunk;
		addlen -= chunk;
		done += chunk;
	}
	return done;
}

/* Free every data block of inode `in` from logical index `from` onward, and
 * trim the inline extent tree accordingly.  Used by truncate/unlink. */
/* Free blocks with logical index >= `from` in the subtree whose header is at
 * `node` (an inline root or an external block buffer), compacting the kept
 * entries.  Returns the number of entries kept; accumulates freed blocks (data
 * AND metadata) in *freed so the caller can fix i_blocks. */
unsigned ext4_ext_free_subtree(ext4_fs_t *fs, unsigned long ino,
				      uint32_t gen, uint8_t *node, int depth,
				      unsigned long from, unsigned long *freed)
{
	ext4_extent_header *eh = (ext4_extent_header *)node;
	if (eh->eh_magic != EXT4_EXT_MAGIC)
		return 0;
	/* `depth': what the node's place in the tree says it has (-1: the
	 * inline root, whose own header says).  A node that is not what its
	 * place says is left as it is -- nothing under it is freed and it is
	 * not freed either (a non-zero count tells the caller to keep it). */
	if (!ext4_ext_hdr_ok(fs, eh, depth < 0 ? eh->eh_depth : (unsigned)depth,
			     depth < 0) ||
	    eh->eh_depth > EXT4_EXT_DEPTH_MAX) {
		ext4_fs_error(fs, "bad extent tree node", ino);
		return eh->eh_entries ? eh->eh_entries : 1;
	}
	unsigned n = eh->eh_entries, keep = 0;

	if (eh->eh_depth == 0) { /* leaf: free data blocks */
		ext4_extent *ex = (ext4_extent *)(eh + 1);
		for (unsigned e = 0; e < n; e++) {
			unsigned long b0 = ex[e].ee_block;
			unsigned len = ex[e].ee_len;
			if (len > 32768)
				len -= 32768;
			unsigned long start =
				(unsigned long)ex[e].ee_start_lo |
				((unsigned long)ex[e].ee_start_hi << 32);
			if (b0 + len <= from) {
				if (keep != e)
					ex[keep] = ex[e];
				keep++;
				continue;
			}
			unsigned long cut = (from > b0) ? (from - b0) : 0;
			if (len > cut) { /* free the run in one pass */
				ext4_free_blocks_run(fs, start + cut,
						     len - cut);
				*freed += (len - cut);
			}
			if (cut > 0) {
				ex[keep] = ex[e];
				ex[keep].ee_len = (uint16_t)cut;
				keep++;
			}
		}
	} else { /* index: recurse into children */
		ext4_extent_idx *ix = (ext4_extent_idx *)(eh + 1);
		uint8_t *cb = (uint8_t *)kalloc(fs->block_size);
		if (!cb)
			return n; /* OOM: leave subtree intact */
		for (unsigned e = 0; e < n; e++) {
			unsigned long cstart = ix[e].ei_block;
			unsigned long cend =
				(e + 1 < n) ?
					(unsigned long)ix[e + 1].ei_block :
					~0UL;
			unsigned long child =
				(unsigned long)ix[e].ei_leaf_lo |
				((unsigned long)ix[e].ei_leaf_hi << 32);
			(void)cstart;
			if (cend <= from) { /* child entirely kept */
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
				continue;
			}
			if (!ext4_blk_valid(fs, ino, child, 1)) {
				ext4_bad_block(fs, ino, "extent index", child,
					       1);
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
				continue;
			}
			if (ext4_read_block(fs, child, cb) != ST_OK) {
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
				continue;
			}
			unsigned sub = ext4_ext_free_subtree(
				fs, ino, gen, cb, (int)eh->eh_depth - 1, from,
				freed);
			if (sub == 0) {
				ext4_free_block(fs, child);
				(*freed)++;
			} /* child emptied: drop */
			else {
				ext4_ext_block_csum_set(fs, ino, gen, cb);
				ext4_write_block(fs, child, cb);
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
			}
		}
		kfree(cb);
	}
	eh->eh_entries = (uint16_t)keep;
	return keep;
}

/* ===================================================================
 * Extent tree: sorted insertion with node splits.
 *
 * The append path above (ext4_ext_add) covers a file growing at its end.  A
 * write filling a hole inside the file needs its extent placed where it
 * belongs in the tree, and that is what this section does, the way the
 * on-disk format requires it:
 *   - the entries of every node stay sorted by logical block;
 *   - an index entry's key is the first logical block of its child, at
 *     every level (fsck checks the equality, not just the order), so a
 *     change to a node's first entry is carried up as far as it is the
 *     first entry (ext4_ext_correct_index);
 *   - a full node is split in two under a parent with room; a full path
 *     grows the tree by one level at the root (ext4_ext_grow_depth).
 * The new extent is merged into a neighbour it continues on disk and in the
 * file, when both are initialized and the sum still fits one extent.
 *
 * A path holds one buffer per level; level 0 is the inode's inline root
 * (never written here -- the caller stores the inode), levels 1..depth are
 * external blocks, written back through the journal with their checksum
 * tail.  Callers hold the metadata lock exclusively.
 * =================================================================== */

struct ext4_ext_path {
	unsigned depth;
	uint8_t *node[EXT4_EXT_DEPTH_MAX + 1];
	unsigned long blk[EXT4_EXT_DEPTH_MAX + 1];
	/* Entry chosen at each level: the last one starting at or before the
	 * block looked up (index levels: the first one when none does);
	 * -1 in a leaf where no extent starts at or before it. */
	int pos[EXT4_EXT_DEPTH_MAX + 1];
};

static inline ext4_extent_header *ext4_ext_hdr(const struct ext4_ext_path *p,
					       unsigned lvl)
{
	return (ext4_extent_header *)p->node[lvl];
}

static inline ext4_extent *ext4_ext_first(const struct ext4_ext_path *p,
					  unsigned lvl)
{
	return (ext4_extent *)(ext4_ext_hdr(p, lvl) + 1);
}

static inline ext4_extent_idx *ext4_ext_first_idx(const struct ext4_ext_path *p,
						  unsigned lvl)
{
	return (ext4_extent_idx *)(ext4_ext_hdr(p, lvl) + 1);
}

/* Blocks an extent maps (an uninitialized one stores its length + 32768). */
static inline unsigned ext4_ext_len(const ext4_extent *e)
{
	return e->ee_len > 32768 ? e->ee_len - 32768u : e->ee_len;
}

static inline unsigned long ext4_ext_pblk(const ext4_extent *e)
{
	return (unsigned long)e->ee_start_lo |
	       ((unsigned long)e->ee_start_hi << 32);
}

static inline unsigned long ext4_idx_pblk(const ext4_extent_idx *ix)
{
	return (unsigned long)ix->ei_leaf_lo |
	       ((unsigned long)ix->ei_leaf_hi << 32);
}

/* Store an initialized extent. */
static void ext4_ext_store(ext4_extent *e, unsigned long lblk,
			   unsigned long pblk, unsigned len)
{
	BUG_ON(len == 0 || len > 32768);
	e->ee_block = (uint32_t)lblk;
	e->ee_len = (uint16_t)len;
	e->ee_start_hi = (uint16_t)(pblk >> 32);
	e->ee_start_lo = (uint32_t)pblk;
}

/* Can `b' be folded into `a' (a immediately before b, in the file and on
 * the device, both initialized, and the sum still fits one extent)? */
static int ext4_ext_mergeable(const ext4_extent *a, const ext4_extent *b)
{
	unsigned la = ext4_ext_len(a), lb = ext4_ext_len(b);

	if (a->ee_len > 32768 || b->ee_len > 32768)
		return 0;
	if ((unsigned long)a->ee_block + la != b->ee_block)
		return 0;
	if (ext4_ext_pblk(a) + la != ext4_ext_pblk(b))
		return 0;
	return la + lb <= 32768;
}

static void ext4_ext_path_free(struct ext4_ext_path *p)
{
	for (unsigned l = 1; l <= p->depth && l <= EXT4_EXT_DEPTH_MAX; l++)
		if (p->node[l]) {
			kfree(p->node[l]);
			p->node[l] = 0;
		}
}

/* Is a node header sane for its place in the tree? */
static int ext4_ext_hdr_ok(const ext4_fs_t *fs, const ext4_extent_header *h,
			   unsigned want_depth, int is_root)
{
	unsigned max = is_root ? 4 : ext4_ext_extern_max(fs);

	/* An index node (depth > 0) with no entries points nowhere. */
	return h->eh_magic == EXT4_EXT_MAGIC && h->eh_depth == want_depth &&
	       h->eh_max != 0 && h->eh_max <= max &&
	       h->eh_entries <= h->eh_max &&
	       !(h->eh_entries == 0 && want_depth > 0);
}

/* Look `lblk' up: fill `p' with the path from the root to the leaf that
 * would hold it.  ST_OK, or ST_IO for a damaged tree (reported). */
static int ext4_ext_find(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			 unsigned long lblk, struct ext4_ext_path *p)
{
	ext4_extent_header *root = (ext4_extent_header *)in->i_block;

	mm_memset(p, 0, sizeof(*p));
	p->node[0] = in->i_block;
	if (root->eh_magic != EXT4_EXT_MAGIC ||
	    root->eh_depth > EXT4_EXT_DEPTH_MAX ||
	    !ext4_ext_hdr_ok(fs, root, root->eh_depth, 1)) {
		ext4_fs_error(fs, "bad extent tree root", ino);
		return ST_IO;
	}
	p->depth = root->eh_depth;
	for (unsigned lvl = 0;; lvl++) {
		ext4_extent_header *h = ext4_ext_hdr(p, lvl);
		unsigned n = h->eh_entries;
		int pos = -1;

		if (lvl == p->depth) {
			ext4_extent *ex = (ext4_extent *)(h + 1);

			for (unsigned i = 0; i < n && ex[i].ee_block <= lblk;
			     i++)
				pos = (int)i;
			p->pos[lvl] = pos;
			return ST_OK;
		}
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);

		if (n == 0) {
			ext4_fs_error(fs, "empty extent index node", ino);
			ext4_ext_path_free(p);
			return ST_IO;
		}
		pos = 0;
		for (unsigned i = 1; i < n && ix[i].ei_block <= lblk; i++)
			pos = (int)i;
		p->pos[lvl] = pos;

		unsigned long child = ext4_idx_pblk(&ix[pos]);
		if (!ext4_blk_valid(fs, ino, child, 1)) {
			ext4_bad_block(fs, ino, "extent index", child, 1);
			ext4_ext_path_free(p);
			return ST_IO;
		}
		uint8_t *buf = (uint8_t *)kalloc(fs->block_size);

		if (!buf) {
			ext4_ext_path_free(p);
			return ST_NOMEM;
		}
		p->node[lvl + 1] = buf;
		p->blk[lvl + 1] = child;
		if (ext4_read_block(fs, child, buf) != ST_OK) {
			ext4_ext_path_free(p);
			return ST_IO;
		}
		if (!ext4_mbc_verified(fs, child)) {
			if (!ext4_extent_block_csum_ok(fs, ino, in->i_generation,
						       buf)) {
				ext4_fs_error(fs,
					      "extent block checksum mismatch",
					      ino);
				ext4_ext_path_free(p);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, child);
		}
		if (!ext4_ext_hdr_ok(fs, (ext4_extent_header *)buf,
				     p->depth - lvl - 1, 0)) {
			ext4_fs_error(fs, "bad extent tree node", ino);
			ext4_ext_path_free(p);
			return ST_IO;
		}
	}
}

/* Store the node at `lvl' (the inline root is stored with the inode). */
static int ext4_ext_dirty(ext4_fs_t *fs, unsigned long ino,
			  const ext4_inode *in, struct ext4_ext_path *p,
			  unsigned lvl)
{
	if (lvl == 0)
		return ST_OK;
	ext4_ext_block_csum_set(fs, ino, in->i_generation, p->node[lvl]);
	return ext4_write_block(fs, p->blk[lvl], p->node[lvl]);
}

static uint32_t ext4_ext_node_key(const struct ext4_ext_path *p, unsigned lvl)
{
	if (lvl == p->depth)
		return ext4_ext_first(p, lvl)[0].ee_block;
	return ext4_ext_first_idx(p, lvl)[0].ei_block;
}

/* The first entry of the node at `lvl' changed: give its index entry the
 * new key, and the same one level up for as long as the entry changed is
 * the first of its node. */
static void ext4_ext_correct_index(ext4_fs_t *fs, unsigned long ino,
				   const ext4_inode *in,
				   struct ext4_ext_path *p, unsigned lvl)
{
	if (ext4_ext_hdr(p, lvl)->eh_entries == 0)
		return;
	uint32_t key = ext4_ext_node_key(p, lvl);

	for (unsigned l = lvl; l > 0; l--) {
		ext4_extent_idx *ix = &ext4_ext_first_idx(p, l - 1)[p->pos[l - 1]];

		if (ix->ei_block == key)
			break;
		ix->ei_block = key;
		ext4_ext_dirty(fs, ino, in, p, l - 1);
		if (p->pos[l - 1] != 0)
			break;
	}
}

/* Make room in the leaf of `p' for one more entry, without inserting it:
 * split the deepest full node whose parent has room (moving its upper half
 * to a new block), or, when every node on the path is full, add a level at
 * the root.  One step per call; the caller looks the block up again and
 * repeats until the leaf has room.  Frees the path. */
static int ext4_ext_make_room(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			      struct ext4_ext_path *p)
{
	int parent = -1;

	for (int l = (int)p->depth - 1; l >= 0; l--)
		if (ext4_ext_hdr(p, (unsigned)l)->eh_entries <
		    ext4_ext_hdr(p, (unsigned)l)->eh_max) {
			parent = l;
			break;
		}
	if (parent < 0) {
		ext4_ext_path_free(p);
		if (((ext4_extent_header *)in->i_block)->eh_depth >=
		    EXT4_EXT_DEPTH_MAX)
			return ST_NOSPC;
		return ext4_ext_grow_depth(fs, ino, in);
	}

	unsigned lvl = (unsigned)parent + 1;
	ext4_extent_header *h = ext4_ext_hdr(p, lvl);
	unsigned n = h->eh_entries, half = n / 2;
	unsigned long nb;

	/* A full node of fewer than two entries cannot be split at its middle.
	 * The driver never writes one (an external node gets the block's whole
	 * capacity), but a node read from the disk may carry any eh_max from 1
	 * up (ext4_ext_hdr_ok accepts what the format allows): damaged or
	 * foreign metadata is an error to report, not a kernel bug. */
	if (n < 2) {
		ext4_ext_path_free(p);
		ext4_fs_error(fs, "extent node too small to split", ino);
		return ST_IO;
	}
	nb = ext4_alloc_one_block(fs);
	if (!nb) {
		ext4_ext_path_free(p);
		return ST_NOSPC;
	}
	uint8_t *buf = (uint8_t *)kalloc(fs->block_size);

	if (!buf) {
		ext4_free_block(fs, nb);
		ext4_ext_path_free(p);
		return ST_NOMEM;
	}
	mm_memset(buf, 0, fs->block_size);
	ext4_extent_header *nh = (ext4_extent_header *)buf;

	nh->eh_magic = EXT4_EXT_MAGIC;
	nh->eh_depth = h->eh_depth;
	nh->eh_max = (uint16_t)ext4_ext_extern_max(fs);
	nh->eh_entries = (uint16_t)(n - half);
	/* Index entries and extents are both 12 bytes. */
	mm_memcpy(nh + 1, (uint8_t *)(h + 1) + (unsigned long)half * 12,
		  (unsigned long)(n - half) * 12);
	uint32_t key = ((ext4_extent_idx *)(nh + 1))->ei_block;

	ext4_ext_block_csum_set(fs, ino, in->i_generation, buf);
	int rc = ext4_write_block(fs, nb, buf);

	kfree(buf);
	if (rc != ST_OK) {
		ext4_free_block(fs, nb);
		ext4_ext_path_free(p);
		return rc;
	}
	h->eh_entries = (uint16_t)half;
	mm_memset((uint8_t *)(h + 1) + (unsigned long)half * 12, 0,
		  (unsigned long)(n - half) * 12);
	ext4_ext_dirty(fs, ino, in, p, lvl);

	/* The new node's entry goes right after the split node's. */
	ext4_extent_header *ph = ext4_ext_hdr(p, (unsigned)parent);
	ext4_extent_idx *pix = ext4_ext_first_idx(p, (unsigned)parent);
	unsigned at = (unsigned)p->pos[parent] + 1;

	BUG_ON(ph->eh_entries >= ph->eh_max || at > ph->eh_entries);
	for (unsigned i = ph->eh_entries; i > at; i--)
		pix[i] = pix[i - 1];
	pix[at].ei_block = key;
	pix[at].ei_leaf_lo = (uint32_t)nb;
	pix[at].ei_leaf_hi = (uint16_t)(nb >> 32);
	pix[at].ei_unused = 0;
	ph->eh_entries++;
	ext4_ext_dirty(fs, ino, in, p, (unsigned)parent);
	in->i_blocks_lo += fs->block_size / 512; /* nb is a metadata block */
	ext4_ext_path_free(p);
	return ST_OK;
}

/* Insert the initialized extent [lblk, lblk+len) -> pblk (one extent's
 * worth at most, not overlapping anything mapped), folded into a neighbour
 * it continues where it can.  ST_OK / ST_NOSPC / ST_NOMEM / ST_IO; on
 * failure nothing was inserted. */
static int ext4_ext_insert(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			   unsigned long lblk, unsigned long pblk, unsigned len)
{
	struct ext4_ext_path p;
	ext4_extent ne;

	ext4_ext_store(&ne, lblk, pblk, len);
	for (unsigned tries = 0; tries < 4 * (EXT4_EXT_DEPTH_MAX + 2);
	     tries++) {
		int rc = ext4_ext_find(fs, ino, in, lblk, &p);

		if (rc != ST_OK)
			return rc;
		unsigned d = p.depth;
		ext4_extent_header *h = ext4_ext_hdr(&p, d);
		ext4_extent *ex = ext4_ext_first(&p, d);
		int at = p.pos[d];
		unsigned n = h->eh_entries;

		if ((at >= 0 && (unsigned long)ex[at].ee_block +
						ext4_ext_len(&ex[at]) >
					lblk) ||
		    (at + 1 < (int)n &&
		     ex[at + 1].ee_block < lblk + (unsigned long)len)) {
			WARN(1, "ext4: inode %lu: extent %lu+%u overlaps the tree",
			     ino, lblk, len);
			ext4_ext_path_free(&p);
			return ST_IO;
		}
		if (at >= 0 && ext4_ext_mergeable(&ex[at], &ne)) {
			unsigned nl = ext4_ext_len(&ex[at]) + len;

			ext4_ext_store(&ex[at], ex[at].ee_block,
				       ext4_ext_pblk(&ex[at]), nl);
			if (at + 1 < (int)n &&
			    ext4_ext_mergeable(&ex[at], &ex[at + 1])) {
				nl += ext4_ext_len(&ex[at + 1]);
				ext4_ext_store(&ex[at], ex[at].ee_block,
					       ext4_ext_pblk(&ex[at]), nl);
				for (unsigned i = (unsigned)at + 1; i + 1 < n;
				     i++)
					ex[i] = ex[i + 1];
				h->eh_entries--;
				mm_memset(&ex[h->eh_entries], 0, sizeof(*ex));
			}
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		if (at + 1 < (int)n && ext4_ext_mergeable(&ne, &ex[at + 1])) {
			ext4_ext_store(&ex[at + 1], lblk, pblk,
				       len + ext4_ext_len(&ex[at + 1]));
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			if (at + 1 == 0)
				ext4_ext_correct_index(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		if (n < h->eh_max) {
			unsigned i0 = (unsigned)(at + 1);

			for (unsigned i = n; i > i0; i--)
				ex[i] = ex[i - 1];
			ex[i0] = ne;
			h->eh_entries++;
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			if (i0 == 0)
				ext4_ext_correct_index(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		rc = ext4_ext_make_room(fs, ino, in, &p);
		if (rc != ST_OK)
			return rc;
	}
	WARN(1, "ext4: inode %lu: no room for an extent after splitting",
	     ino);
	return ST_IO;
}

/* Map the blocks [lblk, lblk+len) -> pblk.. as initialized extents,
 * extent-sized piece by piece.  Returns the blocks mapped. */
static unsigned ext4_ext_insert_run(ext4_fs_t *fs, unsigned long ino,
				    ext4_inode *in, unsigned long lblk,
				    unsigned long pblk, unsigned len)
{
	unsigned done = 0;

	while (done < len) {
		unsigned c = len - done > 32768 ? 32768 : len - done;

		if (ext4_ext_insert(fs, ino, in, lblk + done, pblk + done, c) !=
		    ST_OK)
			break;
		done += c;
	}
	return done;
}

/* The runs mapped by the extent node `node' (`is_root': the inline root),
 * which its parent says has depth `want'. */
static int ext4_ext_walk_node(ext4_fs_t *fs, struct ext4_walk *w,
			      uint32_t gen, const uint8_t *node, unsigned want,
			      int is_root)
{
	const ext4_extent_header *h = (const ext4_extent_header *)node;
	int rc = ST_OK;

	if (want > EXT4_EXT_DEPTH_MAX || !ext4_ext_hdr_ok(fs, h, want, is_root)) {
		kprintf("ext4: inode %lu: bad extent tree node\n", w->ino);
		return ST_INVALID;
	}
	if (h->eh_depth == 0) {
		const ext4_extent *ex = (const ext4_extent *)(h + 1);

		for (unsigned i = 0; rc == ST_OK && i < h->eh_entries; i++) {
			unsigned long s = ex[i].ee_block;
			unsigned long len = ext4_ext_len(&ex[i]);

			if (s >= w->nblocks || len == 0)
				continue;
			if (len > w->nblocks - s)
				len = w->nblocks - s;
			rc = ext4_walk_emit(fs, w, s, ext4_ext_pblk(&ex[i]),
					    len);
		}
		return rc;
	}
	const ext4_extent_idx *ix = (const ext4_extent_idx *)(h + 1);
	uint8_t *buf = (uint8_t *)ext4_bget(fs);

	if (!buf)
		return ST_NOMEM;
	for (unsigned i = 0; rc == ST_OK && i < h->eh_entries; i++) {
		unsigned long child = ext4_idx_pblk(&ix[i]);

		if (ix[i].ei_block >= w->nblocks)
			break;
		if (!ext4_blk_valid(fs, w->ino, child, 1)) {
			kprintf("ext4: inode %lu: extent index points at block "
				"%lu, outside the data area\n",
				w->ino, child);
			rc = ST_INVALID;
			break;
		}
		rc = ext4_read_block(fs, child, buf);
		if (rc != ST_OK)
			break;
		if (!ext4_extent_block_csum_ok(fs, w->ino, gen, buf)) {
			kprintf("ext4: inode %lu: extent block %lu checksum "
				"mismatch\n",
				w->ino, child);
			rc = ST_INVALID;
			break;
		}
		rc = ext4_ext_walk_node(fs, w, gen, buf, h->eh_depth - 1u, 0);
	}
	ext4_bput(fs, buf);
	return rc;
}

/* Walk the extent tree of `in' (see struct ext4_walk). */
int ext4_ext_walk(ext4_fs_t *fs, struct ext4_walk *w, const ext4_inode *in)
{
	const ext4_extent_header *root = (const ext4_extent_header *)in->i_block;

	return ext4_ext_walk_node(fs, w, in->i_generation, in->i_block,
				  root->eh_depth, 1);
}
