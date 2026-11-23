// LikeOS - ext4 filesystem driver: inode reads and writeback, the inode cache, block
// mapping, the page cache plumbing (vfs_sb_ops_t), truncate and
// attribute changes
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Per-filesystem state
 *
 * Everything a mounted ext4 filesystem keeps besides its geometry
 * (ext4_fs_t): the journal transaction and checkpoint batch, the lock
 * hierarchy, the metadata block cache, scratch buffers, deferred inode
 * updates, the write-back buffer and the ordered-data set.  One per mount,
 * reached as fs->st, allocated by ext4_mount() and released by the unmount,
 * so any number of ext4 filesystems can be mounted side by side and none of
 * them shares a lock or a cached block with another.
 * =================================================================== */

/* ---- Inode fields beyond the original 128 bytes ---------------------------
 *
 * An inode larger than 128 bytes carries i_extra_isize more bytes of fields
 * after them; a field there exists only if that size covers it.  The
 * timestamps' _extra words hold the nanoseconds (bits 2..31) and two more
 * bits of the seconds (bits 0..1: the epochs after 2038); the owner's upper
 * 16 bits live in the osd2 area of the base inode (bytes 4..5 uid, 6..7 gid
 * of i_osd2), which every inode has. */

int ext4_inode_has_extra(const ext4_fs_t *fs, const ext4_inode *in,
				unsigned off)
{
	return fs->inode_size > EXT4_GOOD_OLD_ISIZE &&
	       off + 4 <= fs->inode_size &&
	       (unsigned)EXT4_GOOD_OLD_ISIZE + in->i_extra_isize >= off + 4;
}

static uint32_t ext4_time_extra(int64_t sec, uint32_t nsec)
{
	int64_t hi = (sec - (int64_t)(int32_t)(uint32_t)sec) >> 32;
	return (nsec << 2) | ((uint32_t)hi & 3u);
}

void ext4_time_decode(uint32_t lo, uint32_t extra, int has_extra,
			     uint64_t *sec, uint64_t *nsec)
{
	int64_t t = (int64_t)(int32_t)lo;

	if (has_extra) {
		t += (int64_t)(extra & 3u) << 32;
		*nsec = extra >> 2;
		if (*nsec > 999999999u)
			*nsec = 0;
	} else {
		*nsec = 0;
	}
	*sec = (uint64_t)t;
}

/* Set the chosen timestamps (EXT4_T_*) to sec.nsec. */
void ext4_stamp(const ext4_fs_t *fs, ext4_inode *in, unsigned which,
		       int64_t sec, uint32_t nsec)
{
	uint32_t lo = (uint32_t)sec, extra = ext4_time_extra(sec, nsec);

	if (which & EXT4_T_ATIME) {
		in->i_atime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_atime_extra)))
			in->i_atime_extra = extra;
	}
	if (which & EXT4_T_MTIME) {
		in->i_mtime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_mtime_extra)))
			in->i_mtime_extra = extra;
	}
	if (which & EXT4_T_CTIME) {
		in->i_ctime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_ctime_extra)))
			in->i_ctime_extra = extra;
	}
}

/* ...to the current time. */
void ext4_stamp_now(const ext4_fs_t *fs, ext4_inode *in, unsigned which)
{
	ext4_stamp(fs, in, which, (int64_t)timer_get_epoch(), 0);
}

uint32_t ext4_inode_uid(const ext4_inode *in)
{
	uint16_t hi;

	mm_memcpy(&hi, in->i_osd2 + 4, 2);
	return (uint32_t)in->i_uid | ((uint32_t)hi << 16);
}

uint32_t ext4_inode_gid(const ext4_inode *in)
{
	uint16_t hi;

	mm_memcpy(&hi, in->i_osd2 + 6, 2);
	return (uint32_t)in->i_gid | ((uint32_t)hi << 16);
}

void ext4_inode_set_uid(ext4_inode *in, uint32_t uid)
{
	uint16_t hi = (uint16_t)(uid >> 16);

	in->i_uid = (uint16_t)uid;
	mm_memcpy(in->i_osd2 + 4, &hi, 2);
}

void ext4_inode_set_gid(ext4_inode *in, uint32_t gid)
{
	uint16_t hi = (uint16_t)(gid >> 16);

	in->i_gid = (uint16_t)gid;
	mm_memcpy(in->i_osd2 + 6, &hi, 2);
}

/* ===================================================================
 * Journaled-writes transaction (ordered mode).
 *
 * A transaction spans one top-level operation: it begins on the outermost
 * ext4_io_lock(fs) and commits on the matching outermost ext4_io_unlock(fs).
 * While active, ext4_write_block() buffers the *structural* metadata it would
 * write (inode tables, directory blocks, allocation bitmaps) into st->txn
 * instead of writing it in place; commit then journals the whole set
 * atomically (descriptor + blocks + commit) and checkpoints it to the final
 * locations.  Data blocks are written in place during the op (ordered mode),
 * so committed metadata never references unwritten data.  Serialized entirely
 * by ext4_io_lock, so no extra locking is needed for st->txn.
 * =================================================================== */

static inline int ext4_txn_active(ext4_fs_t *fs)
{
	return fs->st->txn.active;
}

/* Read raw on-disk inode `ino` into `out`.  Also returns, when requested, the
 * physical block + byte offset of the inode (for O(1) writeback later). */
/* ---- Deferred inode updates ---------------------------------------------
 *
 * A write() changes two things about an inode that nothing else does: its
 * size and its timestamps.  Storing them is a read-modify-write of a
 * 4KB inode-table block through the journal, and it used to happen on every
 * single write() call -- so a program writing a file in 700-byte pieces paid
 * for one per piece, over and over, against the same block.
 *
 * The conventional answer is to keep the change in memory and store it later,
 * in one go.  What makes that safe is not the deferral itself but the rule
 * that comes with it: while an update is pending, the in-core copy -- not the
 * device -- is what every reader gets.  Without that, open() reads the size
 * off the device and a file closed and reopened reports the length it had at
 * whichever write last happened to store it, with reads stopping there.
 *
 * That rule is enforced in one place.  Both routes to an inode
 * (ext4_read_inode_loc, and ext4_get_inode_cached in front of it) apply the
 * pending change to what they hand back, so there is no such thing as a
 * reader that sees the stale value -- stat, open, the writeback path and
 * every future caller included.
 *
 * Only size and timestamps are ever deferred.  A write that ALLOCATES stores
 * the inode immediately, because the block map lives in the inode too, and a
 * deferred copy of that would leave the filesystem not knowing it owns the
 * blocks the data went into.
 *
 * Fixed size, and deliberately so: no allocation, therefore usable from
 * anywhere, and a program writing to more files at once than this holds does
 * not fail -- it falls back to storing immediately, which is what the driver
 * did for all of them before.
 */
/* A leaf lock: nothing is taken under it and nothing under it sleeps, so it is
 * safe from any context that can reach an inode read.  Guarding the table this
 * way rather than relying on the metadata lock means a reader that reaches an
 * inode by some path not yet audited still sees the pending value. */

/* Record a pending size/time update.  Returns 0 if there was no room, in which
 * case the caller must store the inode itself. */
int ext4_di_record(ext4_fs_t *fs, unsigned long ino, uint64_t size, uint32_t mtime,
			  uint32_t ctime)
{
	uint64_t flags;
	int done = 0;

	if (ino == 0)
		return 0;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino == ino) {
			fs->st->di[i].size = size;
			fs->st->di[i].mtime = mtime;
			fs->st->di[i].ctime = ctime;
			done = 1;
			break;
		}
	}
	if (!done && fs->st->di_n < EXT4_DI_MAX) {
		fs->st->di[fs->st->di_n].ino = ino;
		fs->st->di[fs->st->di_n].size = size;
		fs->st->di[fs->st->di_n].mtime = mtime;
		fs->st->di[fs->st->di_n].ctime = ctime;
		fs->st->di_n++;
		done = 1;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	return done;
}

/* Forget the pending update for `ino`: it has just been stored, or the inode
 * it described no longer exists. */
void ext4_di_clear(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t flags;

	if (ino == 0)
		return;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino == ino) {
			fs->st->di[i] = fs->st->di[fs->st->di_n - 1];
			fs->st->di[fs->st->di_n - 1].ino = 0;
			fs->st->di_n--;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
}

/* Is there an update pending for this inode? */
static int ext4_di_pending(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t flags;
	int found = 0;

	if (ino == 0)
		return 0;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++)
		if (fs->st->di[i].ino == ino) {
			found = 1;
			break;
		}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	return found;
}

/* Overlay the pending update onto a freshly-read inode.
 *
 * Idempotent, so applying it twice on the way through the parsed-inode cache
 * costs nothing and cannot get it wrong.
 */
static void ext4_di_apply(ext4_fs_t *fs, unsigned long ino, ext4_inode *in)
{
	uint64_t flags;

	if (!in || ino == 0)
		return;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino != ino)
			continue;
		in->i_size_lo = (uint32_t)fs->st->di[i].size;
		if ((in->i_mode & S_IFMT) == S_IFREG)
			in->i_size_high = (uint32_t)(fs->st->di[i].size >> 32);
		ext4_stamp(fs, in, EXT4_T_MTIME, (int64_t)fs->st->di[i].mtime,
			   0);
		ext4_stamp(fs, in, EXT4_T_CTIME, (int64_t)fs->st->di[i].ctime,
			   0);
		break;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
}

int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino,
			       ext4_inode *out, unsigned long *out_block,
			       unsigned *out_off)
{
	if (ino == 0 || ino > fs->inodes_count)
		return ST_INVALID;
	unsigned int group = (ino - 1) / fs->inodes_per_group;
	unsigned int index = (ino - 1) % fs->inodes_per_group;

	ext4_group_desc gd;
	int st = ext4_read_group_desc(fs, group, &gd);
	if (st != ST_OK)
		return st;

	unsigned long itbl = ext4_inode_table_block(fs, &gd);
	unsigned long byte = (unsigned long)index * fs->inode_size;
	unsigned long blk = itbl + byte / fs->block_size;
	unsigned off = byte % fs->block_size;

	uint8_t *buf = (uint8_t *)ext4_bget(fs);
	if (!buf)
		return ST_NOMEM;
	st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		ext4_bput(fs, buf);
		return st;
	}

	/* Verify the inode's metadata_csum using the full on-disk
     * bytes still in `buf` (the struct copy below truncates them).  A mismatch
     * means the inode is corrupt — refuse to hand it back (ST_IO => -EIO) and
     * mark the filesystem errored (a bad-checksum error). */
	if (fs->has_metadata_csum) {
		const uint8_t *rp = buf + off;
		uint32_t got = ext4_inode_csum(fs, ino, rp);
		uint16_t slo;
		mm_memcpy(&slo, rp + EXT4_INO_CSUM_LO_OFF, 2);
		uint32_t want = slo, mask = 0xFFFFu;
		if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
			uint16_t extra;
			mm_memcpy(&extra,
				  rp + __builtin_offsetof(ext4_inode,
							  i_extra_isize),
				  2);
			if (extra >=
			    (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE) {
				uint16_t shi;
				mm_memcpy(&shi, rp + EXT4_INO_CSUM_HI_OFF, 2);
				want |= (uint32_t)shi << 16;
				mask = 0xFFFFFFFFu;
			}
		}
		if ((got & mask) != want) {
			kprintf("ext4: inode %lu metadata_csum mismatch "
				"(disk 0x%x computed 0x%x)\n",
				ino, want, got & mask);
			ext4_bput(fs, buf);
			ext4_fs_error(fs, "inode checksum mismatch", ino);
			return ST_IO;
		}
	}

	mm_memset(out, 0, sizeof(*out));
	unsigned copy =
		fs->inode_size < sizeof(*out) ? fs->inode_size : sizeof(*out);
	mm_memcpy(out, buf + off, copy);
	ext4_bput(fs, buf);

	/* What is on the device is not necessarily what the file is.  If a
	 * write has changed the size or the timestamps and that change has not
	 * been stored yet, this is where it is put back -- every reader of an
	 * inode arrives here or at the cache in front of it, so there is no
	 * route by which the stale value escapes. */
	ext4_di_apply(fs, ino, out);

	if (out_block)
		*out_block = blk;
	if (out_off)
		*out_off = off;
	return ST_OK;
}

/* Small parsed-inode cache.  Was a SINGLE entry, which path resolution
 * thrashed on every component (directory inode, then child inode, then the
 * next directory...), forcing an inode-table block read + inode checksum
 * verification per step.  A handful of entries covers a whole resolve.
 *
 * Copy-out API: with concurrent shared-mode readers, a pointer into the
 * cache array would be a use-after-recycle hazard (another reader's fill
 * can reuse the slot at any time).  Lookups therefore copy the record into
 * a caller-owned buffer under the spinlock; the disk fill on a miss runs
 * unlocked (a racing double-fill of the same inode is benign). */

void ext4_inode_cache_flush(ext4_fs_t *fs)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++)
		fs->st->ic[i].ino = 0;
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
}

/* Fetch inode `ino` into caller-owned `out`.  Returns 1 on success, 0 on
 * failure. */
int ext4_get_inode_cached(ext4_fs_t *fs, unsigned long ino,
				 ext4_inode *out)
{
	if (ino == 0 || !out)
		return 0;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++)
		if (fs->st->ic[i].ino == ino) {
			mm_memcpy(out, &fs->st->ic[i].inode, sizeof(*out));
			spin_unlock_irqrestore(&fs->st->ic_lock, flags);
			/* The same rule as on the device path below: a pending
			 * size or timestamp is what the file actually has.
			 * This copy is normally already current -- the write
			 * path updates it in place -- but it is also whatever
			 * some earlier reader left here, so it is overlaid
			 * rather than trusted.  Doing it twice is harmless. */
			ext4_di_apply(fs, ino, out);
			return 1;
		}
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
	/* Miss: read from disk without the spinlock (sleeps on I/O). */
	if (ext4_read_inode_loc(fs, ino, out, 0, 0) != ST_OK)
		return 0;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	unsigned slot = fs->st->ic_next++ % EXT4_IC_ENTRIES;
	mm_memcpy(&fs->st->ic[slot].inode, out, sizeof(*out));
	fs->st->ic[slot].ino = ino;
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
	return 1;
}

/*
 * Replace the cached copy of one inode with a newer one.
 *
 * The alternative, and what the write path used to do, is to empty the whole
 * cache whenever any inode changes.  That is correct but throws away seven
 * other inodes to update one, and the very next thing a write does is map a
 * block through the inode it just changed -- so the entry it needs is read back
 * off the device, once per write() call.
 *
 * Updating in place keeps the map lookups that follow a write in memory, and
 * leaves the other entries alone.  Inserts if the inode was not cached: the
 * caller has the freshest copy there is, so it is worth keeping.
 */
void ext4_inode_cache_update(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in)
{
	uint64_t flags;

	if (ino == 0 || !in)
		return;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++) {
		if (fs->st->ic[i].ino == ino) {
			mm_memcpy(&fs->st->ic[i].inode, in, sizeof(fs->st->ic[i].inode));
			spin_unlock_irqrestore(&fs->st->ic_lock, flags);
			return;
		}
	}
	{
		unsigned slot = fs->st->ic_next++ % EXT4_IC_ENTRIES;

		mm_memcpy(&fs->st->ic[slot].inode, in, sizeof(fs->st->ic[slot].inode));
		fs->st->ic[slot].ino = ino;
	}
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
}

int ext4_map_in_blk(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		    unsigned long lidx, unsigned long *pbn)
{
	if (in->i_flags & EXT4_INODE_EXTENTS_FL)
		return ext4_extent_map(fs, ino, in, lidx, pbn);
	return ext4_indirect_map(fs, ino, in, lidx, pbn);
}

int ext4_map_blk(ext4_fs_t *fs, unsigned long ino, unsigned long lidx,
		 unsigned long *pbn)
{
	ext4_inode in;

	*pbn = 0;
	if (!ext4_get_inode_cached(fs, ino, &in))
		return ST_IO;
	return ext4_map_in_blk(fs, ino, &in, lidx, pbn);
}

/* Map (inode, logical block index) -> physical block number (0 = hole/EOF,
 * or the map could not be used). */
unsigned long ext4_block_map(ext4_fs_t *fs, unsigned long ino,
				    unsigned long lidx)
{
	unsigned long pbn;

	return ext4_map_blk(fs, ino, lidx, &pbn) == ST_OK ? pbn : 0;
}

/* The same from an inode image the caller holds, which may be newer than
 * the cached one (a block just mapped into it, the inode not yet stored). */
unsigned long ext4_map_in(ext4_fs_t *fs, unsigned long ino,
				 const ext4_inode *in, unsigned long lidx)
{
	unsigned long pbn;

	return ext4_map_in_blk(fs, ino, in, lidx, &pbn) == ST_OK ? pbn : 0;
}

/* Hand a mapped block to a map walk: joined to the run being gathered when
 * it continues it, else that run goes to the walk's fn and this one starts
 * the next.  A zero-length call hands over the last run. */
int ext4_walk_emit(ext4_fs_t *fs, struct ext4_walk *w, unsigned long lblk,
		   unsigned long pbn, unsigned long len)
{
	if (len && w->len && lblk == w->lblk + w->len &&
	    pbn == w->pbn + w->len) {
		w->len += len;
		return ST_OK;
	}
	int rc = w->len ? w->fn(fs, w->arg, w->lblk, w->pbn, w->len) : ST_OK;

	w->lblk = lblk;
	w->pbn = pbn;
	w->len = len;
	return rc;
}

/* ===================================================================
 * vfs_sb_ops_t — the generic cache <-> ext4 block plumbing.
 *
 * Chain ids are self-describing: EXT4_BID_ENC(ino, logical_index).  These
 * ops are therefore pure functions of the id (plus the on-disk extent map),
 * needing no per-FS reverse-map state.
 * =================================================================== */

static int ext4_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino);

static unsigned long ext4_sb_block_size(vfs_superblock_t *sb)
{
	return ((ext4_fs_t *)sb->fs_private)->block_size;
}

static const block_device_t *ext4_sb_bdev(vfs_superblock_t *sb)
{
	return ((ext4_fs_t *)sb->fs_private)->bdev;
}

static unsigned long ext4_sb_sector_size(vfs_superblock_t *sb)
{
	return ((ext4_fs_t *)sb->fs_private)->bdev->sector_size;
}

static unsigned long ext4_sb_block_to_lba(vfs_superblock_t *sb,
					  unsigned long bid)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!EXT4_BID_IS(bid))
		return 0;
	unsigned long pbn =
		ext4_block_map(fs, EXT4_BID_INO(bid), EXT4_BID_LIDX(bid));
	if (pbn == EXT4_HOLE_PBN)
		return 0; /* hole: pagecache zero-fills */
	return fs->part_lba_offset + pbn * fs->sectors_per_block;
}

static unsigned long ext4_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return EXT4_EOC_MARKER;
}

static unsigned long ext4_sb_next_block(vfs_superblock_t *sb, unsigned long bid)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!EXT4_BID_IS(bid))
		return EXT4_BID_EOC;
	unsigned long ino = EXT4_BID_INO(bid);
	unsigned long lidx = EXT4_BID_LIDX(bid);
	/* Advance to the next logical block, ending the chain only at the real
	 * end of file — NOT at the first hole.  A sparse file (e.g. a linked
	 * binary with a page-aligned run of zeros, which mkfs stores as a hole)
	 * has unmapped blocks in the MIDDLE; the old "0 mapping means EOF" test
	 * truncated the chain there, so every block past the hole became
	 * unreadable and exec of such a binary failed with ENOEXEC.  The block
	 * count comes from the inode size; the read path zero-fills any block
	 * whose physical mapping is a hole (ext4_sb_block_to_lba returns 0). */
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in))
		return EXT4_BID_EOC;
	unsigned long size = ext4_inode_size(&in);
	unsigned long bs = fs->block_size ? fs->block_size : 4096;
	unsigned long total_blocks = (size + bs - 1) / bs;
	if (lidx + 1 >= total_blocks)
		return EXT4_BID_EOC;
	return EXT4_BID_ENC(ino, lidx + 1);
}

/* Block N of a chain, without walking (vfs_sb_ops_t.block_at): the same
 * answer as N calls of ext4_sb_next_block -- the positional id while N lies
 * inside the recorded size, holes included, 0 past it -- except that a
 * block whose map is damaged gives 0 too (see below). */
static unsigned long ext4_sb_block_at(vfs_superblock_t *sb,
				      unsigned long chain_id,
				      unsigned long block_index)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	ext4_inode in;

	if (!fs || !EXT4_BID_IS(chain_id) || block_index > 0xFFFFFFFFUL)
		return 0;
	unsigned long ino = EXT4_BID_INO(chain_id);
	if (ino == 0 || ino > fs->inodes_count ||
	    !ext4_get_inode_cached(fs, ino, &in))
		return 0;
	unsigned long bs = fs->block_size ? fs->block_size : 4096;
	unsigned long total = (ext4_inode_size(&in) + bs - 1) / bs;
	if (block_index >= total)
		return 0;
	/* A block whose map is damaged (or cannot be read) is no block:
	 * the cache then fails the read with EIO, where the mapping (0, a
	 * hole) would have it fill the page with zeros.  When blocks are
	 * smaller than a page and this is the first block of one, every
	 * block of the page is asked: the cache reads the rest of a page by
	 * stepping on from its first block. */
	unsigned long bpp = PAGE_SIZE / bs, n = 1;
	if (bpp > 1 && block_index % bpp == 0)
		n = bpp;
	for (unsigned long b = block_index; b < block_index + n && b < total;
	     b++) {
		unsigned long pbn;

		if (ext4_map_in_blk(fs, ino, &in, b, &pbn) != ST_OK)
			return 0;
	}
	return EXT4_BID_ENC(ino, block_index);
}

/* Map block N of a file directly.  ext4 block ids are positional -- block N of
 * inode I is EXT4_BID_ENC(I, N) -- so there is nothing to walk; the only
 * question is whether the block exists, and the extent tree answers it.
 *
 * This is deliberately NOT the size test that ext4_sb_next_block ends a chain
 * on.  The size is a second opinion about how much of the file is real, and
 * when it disagrees with the extent tree the extent tree is right: the write
 * path accepts a page for caching only after ext4_block_map says the block is
 * there, so a page that exists and is dirty has an extent behind it whatever
 * the recorded length happens to say at the moment writeback asks. */
static unsigned long ext4_sb_map_block(vfs_superblock_t *sb,
				       unsigned long chain_id,
				       unsigned long block_index)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!fs || !EXT4_BID_IS(chain_id))
		return 0;
	unsigned long ino = EXT4_BID_INO(chain_id);
	if (ino == 0 || ino > fs->inodes_count)
		return 0;
	/* A hole reads as zeroes and has no block to store into; block_to_lba
	 * makes the same call on the same value. */
	if (ext4_block_map(fs, ino, block_index) == 0)
		return 0;
	return EXT4_BID_ENC(ino, block_index);
}

static void ext4_sb_lock_io(vfs_superblock_t *sb)
{
	ext4_io_lock((ext4_fs_t *)sb->fs_private);
}
static void ext4_sb_unlock_io(vfs_superblock_t *sb)
{
	ext4_io_unlock((ext4_fs_t *)sb->fs_private);
}

/* Shared mapping lock for the pagecache: extent lookups (next_block /
 * block_to_lba) only read metadata, so concurrent readers may map in
 * parallel; mutators hold the metadata lock exclusive.  The pagecache
 * performs the data transfer with no fs lock — data-block lifetime is
 * fenced by the per-inode lock the read entry point holds. */
static void ext4_sb_lock_map(vfs_superblock_t *sb)
{
	ext4_meta_rlock((ext4_fs_t *)sb->fs_private);
}
static void ext4_sb_unlock_map(vfs_superblock_t *sb)
{
	ext4_meta_runlock((ext4_fs_t *)sb->fs_private);
}

static unsigned long ext4_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

static int ext4_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *inode)
{
	if (!inode)
		return 0;
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	unsigned long ino = EXT4_BID_INO(inode->start_cluster);
	if (ino == 0 || ino > fs->inodes_count)
		return 0;
	/* This is reached from the generic sync path (pagecache_sync ->
	 * icache_flush_all) with NO ext4 lock held, yet it does a read-modify-
	 * write of an inode-table block THROUGH the journal/mbc tiers
	 * (ext4_read_block -> st->txn/st->ckpt/mbc, ext4_write_block -> st->txn
	 * capture).  Every other st->txn/mbc mutator holds ext4_io_lock (the
	 * invariant documented at ext4_read_block / the st->txn comment); this one
	 * did not.  Unlocked, it races a concurrent mkdir/dir-grow mutating
	 * st->txn: it can corrupt the st->txn array (concurrent append/realloc) or
	 * write back a whole inode block whose OTHER inode slots (e.g. a
	 * directory sharing the 4 KB block) still hold pre-grow values —
	 * reverting that directory's i_size.  Since ext4_dir_lookup linearly
	 * scans blocks 0..i_size/blocksize, a reverted (smaller) i_size makes it
	 * miss the just-added entry in the grown tail leaf -> spurious ENOENT,
	 * only ever on multi-block (large/htree) directories.  Take the
	 * (reentrant) io lock so the RMW is serialized and reads the current
	 * block.  Safe nested: same owner just bumps wdepth. */
	ext4_io_lock(fs);
	ext4_inode in;
	int rc = -1;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK) {
		/* INVARIANT: an ext4 directory's i_size only ever grows — entries
		 * are tombstoned (rec_len merge), never removed, and dir blocks are
		 * appended, never truncated.  Writing back a SMALLER i_size for a
		 * directory reverts it, which is the corruption signature: a stale
		 * inode-block RMW clobbered a concurrent dir-grow.  It shrinks the
		 * block range ext4_dir_lookup linearly scans, so it misses the
		 * just-added entry in the grown tail leaf (spurious ENOENT on
		 * large/htree dirs).  Catch the revert at the moment it is written. */
		WARN_RATELIMIT((in.i_mode & S_IFMT) == S_IFDIR &&
				       (uint32_t)inode->size < in.i_size_lo,
			       "ext4: dir ino %lu i_size reverting %u -> %u (stale inode-block RMW / corruption)",
			       ino, in.i_size_lo, (uint32_t)inode->size);
		/* Take the size from the generic cache only when the write path
		 * has nothing pending for this inode.
		 *
		 * When it does, the read above has already put that size in,
		 * and it is the better of the two: it is written by the code
		 * that computed the length, while this one is a copy kept per
		 * open handle, which two handles on the same file can move
		 * backwards between them.  Preferring it here would persist
		 * the shorter length AND discard the pending record in the
		 * same step, so the difference would not be recoverable. */
		if (!ext4_di_pending(fs, ino)) {
			/* ... and even then, only FORWARDS.
			 *
			 * ext4_di_pending(fs) is not a reliable guard on its own:
			 * ext4_di_record(fs) fails when its table is full, and the
			 * write path then stores the inode instead and leaves
			 * nothing pending -- so a growing file can arrive here
			 * with the generic copy still holding a length from
			 * before the write.  Persisting it shrinks the file to
			 * something it stopped being.
			 *
			 * The read above is the better value in that case: it
			 * comes from the inode with any pending record already
			 * applied.  A shrink that is real -- truncate -- gets to
			 * the device through ext4_truncate_impl, which stores the
			 * inode itself and clears the pending record, so the
			 * value read here is ALREADY the short one and there is
			 * nothing for this path to shorten.  Every remaining
			 * backwards step is therefore a stale copy, and taking it
			 * costs the tail of the file: the size is what
			 * ext4_sb_next_block derives the block chain's length
			 * from, so the pages past the reverted end can no longer
			 * be mapped, and writeback drops them with "dirty page
			 * ... has no block behind it". */
			uint64_t on_inode = ext4_inode_size(&in);
			if ((uint64_t)inode->size >= on_inode) {
				in.i_size_lo = (uint32_t)inode->size;
				if ((in.i_mode & S_IFMT) == S_IFREG)
					in.i_size_high =
						(uint32_t)(inode->size >> 32);
			} else {
				WARN_RATELIMIT(
					1,
					"ext4: ino %lu size revert refused %lu -> %lu (stale generic-cache copy)",
					ino, (unsigned long)on_inode,
					(unsigned long)inode->size);
			}
		}
		ext4_inode_cache_flush(fs);
		ext4_write_inode_struct(fs, ino, &in);
		rc = 0;
	}
	ext4_io_unlock(fs);
	return rc;
}

static const vfs_sb_ops_t ext4_sb_ops = {
	.block_size = ext4_sb_block_size,
	.bdev = ext4_sb_bdev,
	.sector_size = ext4_sb_sector_size,
	.block_to_lba = ext4_sb_block_to_lba,
	.end_of_chain_marker = ext4_sb_end_of_chain,
	.next_block = ext4_sb_next_block,
	.map_block = ext4_sb_map_block,
	.block_at = ext4_sb_block_at,
	.write_inode = ext4_sb_write_inode,
	.lock_io = ext4_sb_lock_io,
	.unlock_io = ext4_sb_unlock_io,
	.lock_map = ext4_sb_lock_map,
	.unlock_map = ext4_sb_unlock_map,
	.reserved_meta_block = ext4_sb_reserved_meta_block,
};

void ext4_sb_attach(ext4_fs_t *fs)
{
	fs->sb.ops = &ext4_sb_ops;
	fs->sb.fs_private = fs;
}

/* Persist a full inode struct back to the inode table (RMW the table block). */
int ext4_write_inode_struct(ext4_fs_t *fs, unsigned long ino,
				   const ext4_inode *in)
{
	if (ino == 0 || ino > fs->inodes_count)
		return ST_INVALID;
	unsigned int group = (ino - 1) / fs->inodes_per_group;
	unsigned int index = (ino - 1) % fs->inodes_per_group;
	unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[group]);
	unsigned long byte = (unsigned long)index * fs->inode_size;
	unsigned long blk = itbl + byte / fs->block_size;
	unsigned off = byte % fs->block_size;
	/* From the block pool, not kalloc: at a 4KB block size kalloc takes the
	 * allocator's large path -- a mapping set up and torn down, the
	 * teardown shooting down the other processors' TLBs -- and this runs on
	 * every metadata update the driver makes.  The pool hands back a buffer
	 * that already exists, and degrades to kalloc only if all of them are
	 * in use. */
	uint8_t *buf = (uint8_t *)ext4_bget(fs);
	if (!buf)
		return ST_NOMEM;
	int st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		ext4_bput(fs, buf);
		return st;
	}
	unsigned copy = fs->inode_size < sizeof(ext4_inode) ?
				fs->inode_size :
				sizeof(ext4_inode);
	/* INVARIANT: a live directory's i_size only grows (dir blocks are
	 * appended, never truncated; the only legitimate shrink writes are
	 * deletions, which carry i_links_count==0 — rmdir/unlink/rename all
	 * zero size and links in the same write).  Writing a SMALLER size for
	 * a still-linked directory over a larger on-disk one means this RMW is
	 * about to persist a stale inode image — the delayed-revert corruption
	 * (lost htree leaves + lost links_count, e2fsck "bad block number" /
	 * unconnected inodes).  Caught here because this is the generic path
	 * every mkdir/link-count/extent update flows through. */
	{
		const ext4_inode *old = (const ext4_inode *)(buf + off);
		WARN_RATELIMIT((old->i_mode & S_IFMT) == S_IFDIR &&
				       (in->i_mode & S_IFMT) == S_IFDIR &&
				       old->i_links_count != 0 &&
				       in->i_links_count != 0 &&
				       in->i_size_lo < old->i_size_lo,
			       "ext4: live dir ino %lu i_size reverting %u -> %u (links %u -> %u): stale inode RMW",
			       ino, old->i_size_lo, in->i_size_lo,
			       old->i_links_count, in->i_links_count);
	}
	mm_memcpy(buf + off, in, copy);
	ext4_inode_csum_set(fs, ino,
			    buf + off); /* over the full on-disk inode */
	st = ext4_write_block(fs, blk, buf);
	ext4_bput(fs, buf);
	ext4_inode_cache_drop(fs, ino); /* parsed-inode cache now stale */
	/* Whatever was pending for this inode has just been stored -- `in` came
	 * from a read, and every read has the pending change applied to it, so
	 * the bytes that went to the device include it.  Only on success: a
	 * failed write leaves the change still owed, and forgetting it here
	 * would lose it silently. */
	if (st == ST_OK)
		ext4_di_clear(fs, ino);
	return st;
}

/*
 * Store every inode with a pending size/timestamp update.
 *
 * The data comes first, and not as a courtesy: a size that reaches the device
 * before the blocks it covers describes, after a crash, blocks that were never
 * written -- which is to say whatever their previous owner left in them.  So
 * the pages of exactly these files are stored first, in one pass, and only
 * then the inodes.
 *
 * Each store clears its own record (see ext4_write_inode_struct), so the list
 * empties itself and an inode whose write failed stays on it.
 *
 * The caller must hold the metadata lock exclusively: this writes metadata.
 */
void ext4_di_flush(ext4_fs_t *fs)
{
	unsigned long inos[EXT4_DI_MAX];
	uint64_t sizes[EXT4_DI_MAX];
	unsigned long ids[EXT4_DI_MAX];
	unsigned n = 0;
	uint64_t flags;

	might_sleep();
	if (!fs)
		return;

	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		inos[n] = fs->st->di[i].ino;
		sizes[n] = fs->st->di[i].size;
		n++;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	if (n == 0)
		return;

	for (unsigned i = 0; i < n; i++)
		ids[i] = EXT4_BID_ENC(inos[i], 0);
	pagecache_flush_fileset(&fs->sb, ids, n);

	for (unsigned i = 0; i < n; i++) {
		ext4_inode in;

		/* Read brings the pending change back with it, so what is
		 * written is the current state of the file. */
		if (ext4_read_inode_loc(fs, inos[i], &in, 0, 0) != ST_OK)
			continue;
		/* The read must have come back carrying the pending size.
		 *
		 * If it did not, ext4_di_apply is not reaching this path, and
		 * the write below is about to store the size the device
		 * already had and then drop the record -- which is the one way
		 * this design loses a file's length outright.  Nothing else
		 * would report it: the write succeeds, the file simply ends up
		 * shorter than it was.  Whoever holds the lock cannot have
		 * changed it underneath us, so a mismatch is a broken overlay
		 * and not a race. */
		WARN_RATELIMIT(ext4_inode_size(&in) != sizes[i],
			       "ext4: inode %lu pending size %lu not applied on read (got %lu) - deferred size would be lost",
			       inos[i], (unsigned long)sizes[i],
			       ext4_inode_size(&in));
		ext4_write_inode_struct(fs, inos[i], &in);
	}
}

/* Allocate up to `count` data blocks for inode `ino`/`in`, appending them as
 * extents starting at logical `start_lidx`.  Returns the number mapped.
 *
 * Two phases so that growing the extent tree (which allocates metadata blocks
 * from the same bitmaps) never races the data-block allocation:
 *   A. grab contiguous runs of free data blocks and WRITE the bitmaps;
 *   B. map the runs into the extent tree (may now safely allocate metadata).
 * Returns < count if free space is too fragmented for one call (the caller
 * re-invokes) or the tree can't grow (ENOSPC). */
#define EXT4_ALLOC_RUNS_MAX 64
unsigned ext4_alloc_blocks_for_file(ext4_fs_t *fs, unsigned long ino,
					   ext4_inode *in,
					   unsigned long start_lidx,
					   unsigned count)
{
	if (count == 0)
		return 0;
	struct {
		unsigned long pbn;
		unsigned len;
	} runs[EXT4_ALLOC_RUNS_MAX];
	unsigned nruns = 0, phys = 0;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	uint8_t *scan = (uint8_t *)kalloc(fs->block_size);
	if (!bm || !scan) {
		if (bm)
			kfree(bm);
		if (scan)
			kfree(scan);
		return 0;
	}

	/* First pass: reserve physical data blocks as contiguous runs; persist
	 * bitmaps.  Blocks freed by operations not yet committed are skipped
	 * (`scan' is the bitmap with them marked, see ext4_pf_add); a second
	 * round follows the commit of the batch that freed them when the first
	 * found too little. */
	for (int round = 0; round < 2; round++) {
		for (unsigned g = 0; g < fs->groups_count && phys < count &&
				     nruns < EXT4_ALLOC_RUNS_MAX;
		     g++) {
			if (fs->gdt[g].bg_free_blocks_count_lo == 0)
				continue;
			/* Group lock: this group's block-bitmap read-modify-write. */
			ext4_bg_lock(fs, g);
			unsigned long bblk = ext4_gd_block_bitmap(fs, g);
			int uninit = 0;
			int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
			if (lst == EXT4_BM_CORRUPT) {
				ext4_bg_unlock(fs, g);
				round = 2; /* no second round either */
				break;
			}
			if (lst != ST_OK) {
				ext4_bg_unlock(fs, g);
				continue;
			}
			unsigned long init_free =
				uninit ? ext4_bitmap_free_blocks(fs, g, bm) : 0;
			unsigned long base =
				fs->first_data_block +
				(unsigned long)g * fs->blocks_per_group;
			const uint8_t *t = bm;
			if (fs->st->pf_n) {
				mm_memcpy(scan, bm, fs->block_size);
				if (ext4_pf_mask(fs, g, scan))
					t = scan;
			}
			unsigned group_alloc = 0, i = 0;
			/* Only the blocks the group has: a damaged bitmap
			 * may show the padding past the last one free. */
			unsigned nb = ext4_blocks_in_group(fs, g);
			int bad = 0;
			while (i < nb && phys < count &&
			       nruns < EXT4_ALLOC_RUNS_MAX && !bad) {
				if (ext4_bm_test(t, i)) {
					i++;
					continue;
				}
				unsigned rstart = i, rlen = 0;
				while (i < nb && !ext4_bm_test(t, i) &&
				       phys < count && rlen < 32768) {
					/* A free bit on a metadata block is a
					 * damaged bitmap: the allocation stops
					 * short of it. */
					if (!ext4_blk_valid(fs, 0, base + i,
							    1)) {
						ext4_bad_block(
							fs, 0,
							"block bitmap marks "
							"metadata free",
							base + i, 1);
						bad = 1;
						break;
					}
					ext4_bm_set(bm, i);
					i++;
					rlen++;
					phys++;
					group_alloc++;
				}
				if (rlen) {
					runs[nruns].pbn = base + rstart;
					runs[nruns].len = rlen;
					nruns++;
				}
			}
			if (group_alloc) {
				if (uninit)
					ext4_bg_block_uninit_clear(fs, g,
								   init_free);
				ext4_block_bitmap_csum_set(fs, g, bm);
				ext4_write_block(fs, bblk, bm);
				fs->gdt[g].bg_free_blocks_count_lo -=
					group_alloc;
				fs->sb_copy.s_free_blocks_count_lo -=
					group_alloc;
				ext4_gd_dirty(fs, g);
			}
			ext4_bg_unlock(fs, g);
			if (bad) {
				round = 2; /* no second round either */
				break;
			}
		}
		if (phys >= count || nruns >= EXT4_ALLOC_RUNS_MAX || round ||
		    !ext4_pf_commit_for_space(fs))
			break; /* (round 2: corrupt bitmap, stop) */
	}
	kfree(scan);
	kfree(bm);

	/* Second pass: map runs into the extent tree, or the classic block map
	 * of an ext2/ext3 inode (metadata alloc is safe now). */
	unsigned mapped = 0;
	int use_ext = ext4_alloc_extents(fs, in);
	for (unsigned r = 0; r < nruns; r++) {
		unsigned got =
			use_ext ? ext4_ext_add(fs, ino, in, start_lidx + mapped,
					       runs[r].pbn, runs[r].len) :
				  ext4_ind_add(fs, ino, in, start_lidx + mapped,
					       runs[r].pbn, runs[r].len);
		mapped += got;
		if (got < runs[r].len) {
			/* Tree couldn't grow: free the unmapped reservations (this run's tail
             * + all later runs) so they aren't leaked. */
			for (unsigned k = got; k < runs[r].len; k++)
				ext4_free_block(fs, runs[r].pbn + k);
			for (unsigned r2 = r + 1; r2 < nruns; r2++)
				for (unsigned k = 0; k < runs[r2].len; k++)
					ext4_free_block(fs, runs[r2].pbn + k);
			break;
		}
	}
	in->i_blocks_lo += mapped * (fs->block_size / 512); /* data blocks */
	return mapped;
}

void ext4_free_blocks_from(ext4_fs_t *fs, unsigned long ino,
				  ext4_inode *in, unsigned long from)
{
	if (!(in->i_flags & EXT4_INODE_EXTENTS_FL)) {
		if (ext4_ind_mapped(in))
			ext4_ind_free_from(fs, ino, in, from);
		return;
	}
	ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
	if (eh->eh_magic != EXT4_EXT_MAGIC)
		return;
	unsigned long freed = 0;
	ext4_ext_free_subtree(fs, ino, in->i_generation, in->i_block, -1, from,
			      &freed);
	if (eh->eh_entries == 0) { /* fully freed: reset to empty leaf root */
		eh->eh_depth = 0;
		eh->eh_max = 4;
	}
	unsigned long fsub = freed * (fs->block_size / 512);
	in->i_blocks_lo =
		(in->i_blocks_lo >= fsub) ? (in->i_blocks_lo - fsub) : 0;
}

int ext4_truncate_impl(vfs_file_t *f, unsigned long size)
{
	if (!f)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (ef->is_dir)
		return -EISDIR;

	ext4_inode in;
	if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	unsigned long cur = ext4_inode_size(&in);

	if (size < cur) {
		unsigned long from =
			(size + fs->block_size - 1) / fs->block_size;

		/* The part of the new last block past the new end must read
		 * as zeros when the file grows again (by a write past it, a
		 * truncate or an allocation), so it is cleared now, while the
		 * block is still the file's. */
		if (size % fs->block_size &&
		    ext4_zero_range(fs, EXT4_BID_ENC(ef->ino, 0), cur, size,
				    (unsigned long)from * fs->block_size))
			return ST_IO;

		/* Drop the cached pages BEFORE the blocks under them go away.
		 *
		 * The invalidation used to come twenty lines further down, past
		 * two inode writes, which left a window in which the file's
		 * dirty pages referred to blocks the allocator had already
		 * taken back.  A writeback landing there has two ways to be
		 * wrong and took both: for a page whose block is gone it finds
		 * nothing to store, warns, and leaves the page dirty -- so it
		 * is never reclaimable and the warning repeats for the life of
		 * the boot ("dirty page ... has no block behind it") -- and for
		 * one whose block has since been handed to another file, it
		 * writes this file's bytes into that one.
		 *
		 * Only the pages AT OR PAST the new end are dropped.  The ones
		 * below it are the part of the file that SURVIVES the truncate,
		 * and for a writer that has not synced they are the only copy
		 * of it -- discarding them loses data the caller never asked to
		 * lose.  They are stored first, while the blocks under them
		 * still belong to this file, so the store cannot land in
		 * another file's blocks. */
		pagecache_invalidate_range(&fs->sb, EXT4_BID_ENC(ef->ino, 0), size);
		pagecache_flush_file(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
		ext4_free_blocks_from(fs, ef->ino, &in, from);

		/* With blocks smaller than a page, the page holding the new
		 * end is kept (it has data below the end) -- and with it the
		 * cached bytes of the blocks after the new last one, which
		 * were just freed.  They are a hole from now on, which must
		 * read as zeros when the file grows over it again (a write
		 * further out skips an unmapped block, see ext4_zero_range),
		 * so the cached copy is cleared.  Nothing is dirtied: there is
		 * no block under those bytes to store them in. */
		unsigned long cut = (unsigned long)from * fs->block_size;
		if (cut % PAGE_SIZE) {
			pc_page_t *pg = pagecache_lookup_held(
				&fs->sb, EXT4_BID_ENC(ef->ino, 0),
				cut / PAGE_SIZE);
			if (pg) {
				if (pg->data)
					mm_memset(pg->data + cut % PAGE_SIZE, 0,
						  PAGE_SIZE - cut % PAGE_SIZE);
				pagecache_put(pg);
			}
		}
	}
	in.i_size_lo = (uint32_t)size;
	if ((in.i_mode & S_IFMT) == S_IFREG)
		in.i_size_high = (uint32_t)(size >> 32);
	uint64_t now = timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME, (int64_t)now, 0);
	ext4_inode_cache_flush(fs);
	/* Any size a write left pending is superseded by this one, and dropped
	 * before the store rather than after it: if the store fails the file
	 * keeps the size it has on the device, which is what a failed truncate
	 * should leave behind -- not the length it had before the truncate was
	 * asked for. */
	ext4_di_clear(fs, ef->ino);
	int st = ext4_write_inode_struct(fs, ef->ino, &in);
	ef->size = size;
	if (ef->pos > size)
		ef->pos = size;
	if (ef->inode)
		icache_update_size((ic_inode_t *)ef->inode, size);
	/* NOTHING is invalidated here, and nothing may be.
	 *
	 * This used to drop the file's whole page cache on EVERY truncate,
	 * including the ones that free no blocks at all -- and
	 * pagecache_invalidate_file(&fs->sb, ) is a DISCARD: it takes each page off the
	 * dirty list and frees the frame without storing it.
	 *
	 * ftruncate(fd, size) with size equal to the length just written is a
	 * routine end-of-transfer step rather than an odd one -- scp(1) ends
	 * every incoming file with it -- and it threw the entire transfer away.
	 * The inode kept the new size and the extents kept the new blocks, but
	 * the bytes never left memory, so reading the file back returned
	 * whatever the previous owner of those blocks had left in them: a
	 * freshly copied file came out the right length and full of some other
	 * file's contents.
	 *
	 * The shrink path above drops exactly the pages whose blocks it frees,
	 * at the point where doing so is correct, so there is nothing left for
	 * this to invalidate.  A truncate that grows the file, or leaves the
	 * length alone, has no stale page to drop in the first place. */
	icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
	ext4_flush_meta(fs);
	return st == ST_OK ? ST_OK : ST_IO;
}

/* Set one timestamp per struct vfs_utimes (seconds + nanoseconds, "now" or
 * "leave alone"). */
static void ext4_apply_utime(const ext4_fs_t *fs, ext4_inode *in,
			     unsigned which, int64_t sec, long nsec)
{
	if (nsec == VFS_UTIME_OMIT)
		return;
	if (nsec == VFS_UTIME_NOW) {
		ext4_stamp_now(fs, in, which);
		return;
	}
	if (nsec < 0 || nsec > 999999999L)
		nsec = 0;
	ext4_stamp(fs, in, which, sec, (uint32_t)nsec);
}

/* utimensat: the access and modification times of `ino' as `t' asks, and
 * the change time to now, as any change of an inode's attributes does.
 * Under the I/O lock. */
int ext4_utimens_ino(ext4_fs_t *fs, unsigned long ino,
			    const struct vfs_utimes *t)
{
	/* This inode's own pending size/time update (and only its own: the
	 * other files' stay deferred) goes to the device with the times
	 * set here -- the read below brings it along, and the store clears
	 * it, so it cannot later put the old modification time back over
	 * this one.  A size may only reach the device after the data it
	 * covers, so the file's pages are stored first, before the inode is
	 * read: whatever storing them changes in the inode is then in what
	 * is written back. */
	if (ext4_di_pending(fs, ino)) {
		unsigned long id = EXT4_BID_ENC(ino, 0);

		pagecache_flush_fileset(&fs->sb, &id, 1);
	}
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	ext4_apply_utime(fs, &in, EXT4_T_ATIME, t->atime_sec, t->atime_nsec);
	ext4_apply_utime(fs, &in, EXT4_T_MTIME, t->mtime_sec, t->mtime_nsec);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	int rc = ext4_write_inode_struct(fs, ino, &in);
	return rc == ST_OK ? ST_OK : ST_IO;
}

int ext4_utimens_impl(ext4_fs_t *fs, const char *path, int nofollow,
			     const struct vfs_utimes *t)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs || !t)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino = 0;
	int rr = ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, &ino, 0);
	if (rr == ST_OK)
		rr = ext4_utimens_ino(fs, ino, t);
	ext4_io_unlock(fs);
	return rr; /* ST_IO is propagated, not turned into ENOENT */
}

static int ext4_set_mode_locked(ext4_fs_t *fs, unsigned long ino, unsigned mode)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	in.i_mode = (uint16_t)((in.i_mode & S_IFMT) | (mode & 07777));
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	/* A mode change can re-add a set-id bit: invalidate the per-inode strip
     * hint (S_NOSEC analog) for every handle, so the next non-root modify
     * re-evaluates.  Peek the cache without taking a ref. */
	ic_inode_t *ic = icache_lookup(&fs->sb, EXT4_BID_ENC(ino, 0));
	if (ic)
		ic->flags &= ~(uint32_t)IC_SETID_CLEAN;
	return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

static int ext4_set_owner_locked(ext4_fs_t *fs, unsigned long ino, int uid,
				 int gid)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	/* -1 leaves a field as it is; every other value is an id, those
	 * above 2^31 included (the full 32 bits are kept). */
	if (uid != -1)
		ext4_inode_set_uid(&in, (uint32_t)uid);
	if (gid != -1)
		ext4_inode_set_gid(&in, (uint32_t)gid);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

int ext4_chmod(ext4_fs_t *fs, const char *path, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_resolve(fs, path, &ino);
	if (r == ST_OK)
		r = ext4_set_mode_locked(fs, ino, mode);
	ext4_io_unlock(fs);
	return r;
}

int ext4_chown(ext4_fs_t *fs, const char *path, int uid, int gid,
		      int nofollow)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, &ino, 0);
	if (r == ST_OK)
		r = ext4_set_owner_locked(fs, ino, uid, gid);
	ext4_io_unlock(fs);
	return r;
}

/* fd-based fchmod/fchown: chmod/chown by inode. */
int ext4_fchmod_ino(ext4_fs_t *fs, unsigned long ino, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!ino || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_set_mode_locked(fs, ino, mode);
	ext4_io_unlock(fs);
	return r;
}

int ext4_fchown_ino(ext4_fs_t *fs, unsigned long ino, int uid, int gid)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!ino || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_set_owner_locked(fs, ino, uid, gid);
	ext4_io_unlock(fs);
	return r;
}

int ext4_lstat(ext4_fs_t *fs, const char *path, struct kstat *st)
{
	if (!path || !st || !fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &ino,
				0); /* no follow final */
	if (r == ST_OK)
		r = ext4_stat_fill(fs, ino, st);
	ext4_meta_runlock(fs);
	return r;
}
