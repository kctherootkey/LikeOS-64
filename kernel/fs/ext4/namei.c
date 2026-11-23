// LikeOS - ext4 filesystem driver: name lookup, path resolution, htree (hash
// indexed) directories, directory entry insertion and removal, link,
// unlink, mkdir, mknod, rmdir and rename
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Directory traversal + path resolution
 * =================================================================== */

/* ---- Dentry cache -------------------------------------------------------
 *
 * Every path resolution used to read the directory off the device, component
 * by component, on every open, stat, access and unlink.  A program that opens
 * a few thousand files pays for a few thousand directory scans, and a $PATH
 * search pays for one per directory that does NOT hold the program.
 *
 * So the answers are remembered: name -> inode, keyed by the directory the
 * name is in.  Failures are remembered too -- a negative entry -- which is
 * what makes the $PATH case cheap, because "not here" is the answer that
 * dominates it.
 *
 * Two properties of this cache are worth stating rather than leaving to be
 * rediscovered:
 *
 *  - It is maintained, not invalidated.  A create adds the entry it just
 *    made, a delete removes exactly the one it removed.  Both happen in
 *    ext4_dir_add and ext4_dir_del, which is where EVERY directory mutation
 *    in this driver goes through -- create, unlink, mkdir, rmdir, rename,
 *    symlink and link all reach the device that way, so none of them can be
 *    forgotten here by being added later.
 *
 *  - Nothing needs a lock of its own.  Lookups run under the metadata lock
 *    SHARED and mutations under it EXCLUSIVE, so a mutation cannot interleave
 *    with a lookup; the cache's internal locking then handles two concurrent
 *    lookups.  Entries are made only AFTER the device operation has
 *    succeeded, so a failed mutation leaves nothing behind.
 *
 * The key is the inode number in the same encoded form the page and inode
 * caches use.  Encoding it keeps ext4's keys out of the range FAT32's cluster
 * numbers occupy: the caches are shared, and two filesystems must not be able
 * to name the same entry.
 */
#define EXT4_DC_PARENT(ino) EXT4_BID_ENC((ino), 0)

/* Longest name an ext4 directory entry can hold (name_len is one byte). */
#define EXT4_NAME_LEN 255

/* The lookup interface takes a NUL-terminated name; this driver passes names
 * as a pointer plus a length, which is not always the same thing.  Copying is
 * a few dozen bytes against the directory scan it avoids. */
static int ext4_dc_name(const char *name, unsigned name_len, char *out,
			unsigned cap)
{
	if (name_len == 0 || name_len >= cap)
		return 0;
	mm_memcpy(out, name, name_len);
	out[name_len] = '\0';
	return 1;
}

/* Consult the cache.  Returns ST_OK (with *out_ino / *out_ftype filled in),
 * ST_NOT_FOUND for a remembered failure, or ST_UNSUPPORTED for "not cached,
 * ask the device" -- a value no directory lookup produces, so a caller cannot
 * confuse it with a real answer. */
static int ext4_dc_lookup(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			  unsigned name_len, unsigned long *out_ino,
			  unsigned *out_ftype)
{
	char nm[EXT4_NAME_LEN + 1];
	dc_result_t res;

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return ST_UNSUPPORTED;

	switch (dcache_lookup_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm, &res)) {
	case DC_LOOKUP_FOUND:
		if (out_ino)
			*out_ino = res.ino;
		if (out_ftype)
			*out_ftype = res.attr;
		return ST_OK;
	case DC_LOOKUP_NEGATIVE:
		return ST_NOT_FOUND;
	default:
		return ST_UNSUPPORTED;
	}
}

static void ext4_dc_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype)
{
	char nm[EXT4_NAME_LEN + 1];

	if (child_ino == 0)
		return;
	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	/* The file type is stored as ext4 records it in the directory entry.
	 * The cache's own directory hint (bit 0x10) is left unset: nothing
	 * here reads it, because ext4 takes what it needs from the inode.
	 *
	 * The size is deliberately NOT stored, hence the zero.  ext4 keeps a
	 * file's size in its inode and nowhere else, so caching a copy of it
	 * against a NAME would mean every write had to find and update that
	 * copy -- and a missed update is a file that stats short and reads
	 * truncated.  A name resolves to an inode here; the inode answers for
	 * everything else. */
	dcache_insert_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm, child_ino, 0, ftype);
}

static void ext4_dc_add_negative(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
				 unsigned name_len)
{
	char nm[EXT4_NAME_LEN + 1];

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	dcache_insert_negative_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm);
}

static void ext4_dc_del(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len)
{
	char nm[EXT4_NAME_LEN + 1];

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	/* Removed rather than turned into a negative entry.  Both are correct,
	 * and removing is the one that stays correct if a future caller ever
	 * puts the name back without going through ext4_dir_add. */
	dcache_invalidate_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm);
}

/* Forget everything cached under `ino` as a directory.
 *
 * Needed at the two points where an inode number stops meaning what it meant:
 * when a directory is removed, and when a number is handed out again. */
void ext4_dc_drop_dir(ext4_fs_t *fs, unsigned long ino)
{
	if (ino == 0)
		return;
	dcache_invalidate_dir(&fs->sb, EXT4_DC_PARENT(ino));
}

/* Look up `name` (length name_len) in directory inode `dir_ino`, on the device.
 * On success returns ST_OK and sets *out_ino (and *out_ftype if non-NULL).
 *
 * *out_complete says whether every block of the directory was actually read.
 * A "not found" from a scan that skipped a block it could not read is not the
 * same statement as a "not found" from a scan that read all of them: the first
 * may be a transient device error, and remembering it would make that error
 * permanent for as long as the entry survives. */
static int ext4_dir_lookup_disk(ext4_fs_t *fs, unsigned long dir_ino,
				const char *name, unsigned name_len,
				unsigned long *out_ino, unsigned *out_ftype,
				int *out_complete)
{
	ext4_inode din;
	int complete = 1;

	if (out_complete)
		*out_complete = 0;
	if (!ext4_get_inode_cached(fs, dir_ino, &din) ||
	    (din.i_mode & S_IFMT) != S_IFDIR)
		return ST_NOT_FOUND;
	unsigned long dsize = ext4_inode_size(&din);
	unsigned long nblocks = (dsize + fs->block_size - 1) / fs->block_size;
	uint32_t gen = din.i_generation; /* for the leaf csum */

	uint8_t *blk = (uint8_t *)ext4_bget(fs);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn;
		int mrc = ext4_map_blk(fs, dir_ino, b, &pbn);
		if (mrc != ST_OK) {
			/* A damaged map (reported): the name cannot be
			 * looked for -- an error, not "not found". */
			ext4_bput(fs, blk);
			return mrc;
		}
		if (pbn == 0)
			continue; /* sparse dir block (rare): genuinely empty */
		if (ext4_read_block(fs, pbn, blk) != ST_OK) {
			complete = 0;
			continue;
		}
		if (!ext4_mbc_verified(fs, pbn)) {
			if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
				ext4_bput(fs, blk);
				ext4_fs_error(fs,
					      "directory leaf checksum mismatch",
					      dir_ino);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, pbn);
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size) {
				WARN_ON_ONCE(
					1); /* bad dir rec_len — stop scanning this block */
				complete = 0;
				break;
			}
			if (de->inode != 0 && de->name_len == name_len &&
			    ext4_memcmp(de->name, name, name_len) == 0) {
				*out_ino = de->inode;
				if (out_ftype)
					*out_ftype = de->file_type;
				ext4_bput(fs, blk);
				if (out_complete)
					*out_complete = 1;
				return ST_OK;
			}
			off += rec;
		}
	}
	ext4_bput(fs, blk);
	if (out_complete)
		*out_complete = complete;
	return ST_NOT_FOUND;
}

/*
 * Look up `name` in directory `dir_ino`, through the dentry cache.
 *
 * A hit answers without touching the device at all.  A miss does the scan
 * above and then records what it found, so the next caller does not repeat it.
 *
 * "." is not cached: path resolution never asks for it (it means the directory
 * that was already reached), so an entry for it would only take up room.
 */
int ext4_dir_lookup(ext4_fs_t *fs, unsigned long dir_ino,
			   const char *name, unsigned name_len,
			   unsigned long *out_ino, unsigned *out_ftype)
{
	unsigned long ino = 0;
	unsigned ftype = 0;
	int complete = 0;
	int r;

	if (!out_ino)
		return ST_INVALID;

	r = ext4_dc_lookup(fs, dir_ino, name, name_len, &ino, &ftype);
	if (r != ST_UNSUPPORTED) {
		if (r == ST_OK) {
			*out_ino = ino;
			if (out_ftype)
				*out_ftype = ftype;
		}
		return r;
	}

	r = ext4_dir_lookup_disk(fs, dir_ino, name, name_len, &ino, &ftype,
				 &complete);
	if (r == ST_OK) {
		*out_ino = ino;
		if (out_ftype)
			*out_ftype = ftype;
		ext4_dc_add(fs, dir_ino, name, name_len, ino, ftype);
	} else if (r == ST_NOT_FOUND && complete) {
		/* Remembered only when the whole directory was read.  This is
		 * the entry that makes searching a $PATH cheap: most of the
		 * directories on it do not hold the program, and without it
		 * each of them is rescanned on every command. */
		ext4_dc_add_negative(fs, dir_ino, name, name_len);
	}
	return r;
}

/* Read a symlink's target into buf (NUL-terminated).  Fast symlinks (<60B,
 * i_blocks==0) store the target inline in i_block; longer "slow" symlinks
 * store it in data block 0.  Returns target length, -1 on error, or -2
 * when it does not fit in `cap' bytes. */
int ext4_read_symlink_target(ext4_fs_t *fs, unsigned long ino,
				    const ext4_inode *in, char *buf,
				    unsigned cap)
{
	unsigned long len = in->i_size_lo;
	if (len == 0)
		return -1;
	/* A target is never longer than a block, and one kept in the inode
	 * never longer than i_block: anything else is a corrupt inode, and
	 * copying it would read past the source. */
	if (len >= fs->block_size)
		return -1;
	if (in->i_blocks_lo == 0 && len >= sizeof(in->i_block))
		return -1;
	/* Longer than the caller's buffer (every caller gives a whole path,
	 * PATHBUF_SIZE, so this needs a target the VFS could never have
	 * written): refused rather than cut, which would lead elsewhere. */
	if (len >= cap)
		return -2;
	if (in->i_blocks_lo == 0) {
		mm_memcpy(buf, in->i_block, len);
	} else {
		unsigned long pbn = ext4_block_map(fs, ino, 0);
		if (pbn == 0)
			return -1;
		uint8_t *blk = (uint8_t *)ext4_bget(fs);
		if (!blk)
			return -1;
		if (ext4_read_sectors(fs->bdev,
				      fs->part_lba_offset +
					      pbn * fs->sectors_per_block,
				      fs->sectors_per_block, blk) != ST_OK) {
			ext4_bput(fs, blk);
			return -1;
		}
		mm_memcpy(buf, blk, len);
		ext4_bput(fs, blk);
	}
	buf[len] = '\0';
	return (int)len;
}

/* Resolve a path to an inode, following symlinks.  `start_ino` is the dir for
 * relative paths; `follow_final` controls whether a symlink in the LAST
 * component is followed (stat yes, lstat/readlink no).  `depth` guards loops. */
int ext4_resolve_ex(ext4_fs_t *fs, unsigned long start_ino,
			   const char *path, int follow_final,
			   unsigned long *out_ino, int depth)
{
	if (!path || !out_ino)
		return ST_INVALID;
	if (depth > 12)
		return ST_INVALID; /* ELOOP guard            */

	/* A trailing slash names the thing THROUGH the link, and it has to be
	 * a directory.
	 *
	 * "a/b/" means the same as "a/b/.", so the last component is followed
	 * even by the callers that otherwise stop at a link -- lstat and
	 * friends -- and the answer must be a directory or the path is wrong.
	 * Without this, listing a directory reached by a symbolic link
	 * reported the link itself however the name was written, because the
	 * tool asks with lstat and a trailing slash is exactly how one says
	 * "the directory, not the link". */
	size_t plen = 0;
	while (path[plen])
		plen++;
	int trailing_slash = (plen > 0 && path[plen - 1] == '/');
	if (trailing_slash)
		follow_final = 1;

	unsigned long cur = (path[0] == '/') ? EXT4_ROOT_INO : start_ino;
	const char *p = path;
	while (*p == '/')
		p++;

	char comp[256];
	while (*p) {
		unsigned ci = 0;
		const char *cstart = p;
		while (*p && *p != '/') {
			/* A cut name would be another name. */
			if (ci >= sizeof(comp) - 1)
				return ST_NAMETOOLONG;
			comp[ci++] = *p;
			p++;
		}
		comp[ci] = '\0';
		const char *cend = p;
		while (*p == '/')
			p++;
		int is_last = (*p == '\0');

		if (ci == 0 || (ci == 1 && comp[0] == '.'))
			continue;
		if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
			unsigned long parent;
			if (ext4_dir_lookup(fs, cur, "..", 2, &parent, 0) ==
			    ST_OK)
				cur = parent;
			continue;
		}
		unsigned long child;
		unsigned ftype = 0;
		int lr = ext4_dir_lookup(fs, cur, comp, ci, &child, &ftype);
		if (lr != ST_OK) /* a corrupt directory leaf
                                                     returns ST_IO; propagate it
                                                     rather than masking ENOENT  */
			return lr;

		ext4_inode cin;
		if (!ext4_get_inode_cached(fs, child, &cin))
			return ST_IO;
		if ((cin.i_mode & S_IFMT) == S_IFLNK &&
		    (!is_last || follow_final)) {
			/* A link's target is up to a whole path long: a path
			 * buffer, held across the nested resolve (12 deep at
			 * most). */
			char *target = pathbuf_get();
			if (!target)
				return ST_NOMEM;
			int tl = ext4_read_symlink_target(fs, child, &cin,
							  target, PATHBUF_SIZE);
			if (tl <= 0) {
				pathbuf_put(target);
				return tl == -2 ? ST_NAMETOOLONG : ST_IO;
			}
			/* The path the VFS handed us: the VFS follows the
			 * link, wherever in the name space it leads (another
			 * mount, the process's root, ".." past ours). */
			if (depth == 0 && path[0] == '/' &&
			    vfs_link_hop(fs->mnt, path,
					 (unsigned long)(cstart - path),
					 (unsigned long)(cend - path), target)) {
				pathbuf_put(target);
				return ST_LINK;
			}
			unsigned long linked;
			int r = ext4_resolve_ex(fs, cur, target, 1, &linked,
						depth + 1);
			pathbuf_put(target);
			if (r != ST_OK)
				return r;
			cur = linked;
		} else {
			cur = child;
		}
		if (!is_last) {
			ext4_inode d;
			if (!ext4_get_inode_cached(fs, cur, &d))
				return ST_IO; /* unreadable/corrupt component */
			if ((d.i_mode & S_IFMT) != S_IFDIR)
				return ST_NOT_FOUND;
		}
	}
	if (trailing_slash) {
		ext4_inode d;
		if (!ext4_get_inode_cached(fs, cur, &d))
			return ST_IO;
		if ((d.i_mode & S_IFMT) != S_IFDIR)
			return ST_NOT_FOUND; /* "file/" is not a path */
	}
	*out_ino = cur;
	return ST_OK;
}

/* Default resolver: relative to cwd, follows a final symlink. */
int ext4_resolve(ext4_fs_t *fs, const char *path, unsigned long *out_ino)
{
	return ext4_resolve_ex(fs, fs->st->cwd_ino, path, 1, out_ino, 0);
}

/* ---- directory entry insert / remove ---- */
static inline unsigned ext4_dirent_len(unsigned name_len)
{
	return (8 + name_len + 3) & ~3u;
}

/* Checksum a dx index block (root if is_root, else interior node).  e2fsck only
 * recomputes the csum over the on-disk dt_reserved (we keep it 0), so this is
 * self-consistent and clean.  No-op without metadata_csum. */
static void ext4_dx_csum_set(const ext4_fs_t *fs, unsigned long dir_ino,
			     uint32_t gen, uint8_t *blk, int is_root)
{
	if (!fs->has_metadata_csum)
		return;
	unsigned count_off = is_root ? 32 : 8;
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(blk + count_off);
	ext4_dx_tail *t =
		(ext4_dx_tail *)(blk + count_off + (unsigned)cl->limit * 8);
	t->dt_reserved = 0;
	uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
	uint32_t csum =
		ext4_crc32c(seed, blk, count_off + (unsigned)cl->count * 8);
	csum = ext4_crc32c(csum, t, 4); /* dt_reserved (0) */
	uint32_t z = 0;
	csum = ext4_crc32c(csum, &z, 4);
	t->dt_checksum = csum;
}

/* dx entry-array capacity (entries incl. the count/limit slot), reserving the
 * 8-byte dx_tail when metadata_csum is on. */
static unsigned ext4_dx_root_limit(const ext4_fs_t *fs)
{
	return (fs->block_size - 32 - (fs->has_metadata_csum ? 8 : 0)) / 8;
}
static unsigned ext4_dx_node_limit(const ext4_fs_t *fs)
{
	return (fs->block_size - 8 - (fs->has_metadata_csum ? 8 : 0)) / 8;
}

/* Insert (name,child,ftype) into an in-memory leaf buffer if it fits (does not
 * write or checksum).  Returns 1 on success, 0 if no room. */
static int ext4_leaf_insert(ext4_fs_t *fs, uint8_t *blk, const char *name,
			    unsigned name_len, unsigned long child_ino,
			    unsigned ftype)
{
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;
	unsigned need = ext4_dirent_len(name_len);
	unsigned off = 0;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		unsigned used =
			(de->inode == 0) ? 0 : ext4_dirent_len(de->name_len);
		if (rec - used >= need) {
			ext4_dir_entry_2 *ne;
			if (de->inode == 0) {
				ne = de;
				ne->rec_len = (uint16_t)rec;
			} else {
				de->rec_len = (uint16_t)used;
				ne = (ext4_dir_entry_2 *)(blk + off + used);
				ne->rec_len = (uint16_t)(rec - used);
			}
			ne->inode = (uint32_t)child_ino;
			ne->name_len = (uint8_t)name_len;
			ne->file_type = (uint8_t)ftype;
			mm_memcpy(ne->name, name, name_len);
			return 1;
		}
		off += rec;
	}
	return 0;
}

/* Append one block to directory `din` at its next logical index; persists the
 * inode (with the new extent + size) and invalidates the inode cache so the
 * block map sees it.  Returns the new logical block index, or ST_NOSPC. */
static long ext4_dir_grow(ext4_fs_t *fs, unsigned long dir_ino, ext4_inode *din)
{
	unsigned long nblocks = ext4_inode_size(din) / fs->block_size;
	if (ext4_alloc_blocks_for_file(fs, dir_ino, din, nblocks, 1) != 1)
		return ST_NOSPC;
	din->i_size_lo = (uint32_t)((nblocks + 1) * fs->block_size);
	if (ext4_write_inode_struct(fs, dir_ino, din) != ST_OK)
		return ST_IO;
	ext4_inode_cache_flush(fs);
	return (long)nblocks;
}

/* A sortable reference to a directory entry (in some block buffer) used while
 * redistributing entries during a leaf split. */
typedef struct {
	uint32_t hash;
	const ext4_dir_entry_2 *de;
} ext4_dx_ref;

static void ext4_dx_sort(ext4_dx_ref *r, int n) /* insertion sort by hash */
{
	for (int i = 1; i < n; i++) {
		ext4_dx_ref k = r[i];
		int j = i - 1;
		while (j >= 0 && r[j].hash > k.hash) {
			r[j + 1] = r[j];
			j--;
		}
		r[j + 1] = k;
	}
}

/* Lay refs[a..b) into a freshly initialised leaf block (no dot/dotdot), with the
 * last record padded to the block end, then checksum it. */
static void ext4_dx_build_leaf(ext4_fs_t *fs, unsigned long dir_ino,
			       uint32_t gen, uint8_t *blk,
			       const ext4_dx_ref *refs, int a, int b)
{
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;
	mm_memset(blk, 0, fs->block_size);
	unsigned off = 0, last_off = 0;
	for (int i = a; i < b; i++) {
		const ext4_dir_entry_2 *s = refs[i].de;
		unsigned rl = ext4_dirent_len(s->name_len);
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
		de->inode = s->inode;
		de->name_len = s->name_len;
		de->file_type = s->file_type;
		mm_memcpy(de->name, s->name, s->name_len);
		de->rec_len = (uint16_t)rl;
		last_off = off;
		off += rl;
	}
	if (b > a)
		((ext4_dir_entry_2 *)(blk + last_off))->rec_len =
			(uint16_t)(scan_limit - last_off);
	else {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)blk;
		de->inode = 0;
		de->rec_len = (uint16_t)scan_limit;
	}
	ext4_dir_csum_set(fs, dir_ino, gen, blk);
}

/* Insert (hash,block) into a dx index block's sorted entry array at sorted
 * position; assumes there is room (count < limit).  count_off = 32 root / 8 node. */
static void ext4_dx_insert_entry(uint8_t *blk, unsigned count_off,
				 uint32_t hash, uint32_t block)
{
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(blk + count_off);
	ext4_dx_entry *e =
		(ext4_dx_entry *)(blk + count_off); /* e[0].hash aliases cl */
	unsigned count = cl->count;
	unsigned pos = 1;
	while (pos < count && (e[pos].hash & ~1u) <= hash)
		pos++; /* keep ascending order */
	for (unsigned i = count; i > pos; i--)
		e[i] = e[i - 1]; /* pos>=1 so e[0] kept */
	e[pos].hash = hash;
	e[pos].block = block;
	cl->count = (uint16_t)(count + 1);
}

/* Write a directory block by logical index (maps via the extent tree). */
static int ext4_dir_write_lblk(ext4_fs_t *fs, unsigned long dir_ino,
			       unsigned long lblk, const uint8_t *buf)
{
	unsigned long pbn = ext4_block_map(fs, dir_ino, lblk);
	if (pbn == 0)
		return ST_IO;
	return ext4_write_block(fs, pbn, buf);
}

/* Convert a single-block linear directory (block 0 full) into an htree: move its
 * real entries to a new leaf (logical block 1) and rebuild block 0 as a dx_root
 * with one entry covering the whole hash range.  Leaves the new name to the
 * caller's htree add. */
static int ext4_htree_make_indexed(ext4_fs_t *fs, unsigned long dir_ino,
				   ext4_inode *din)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	int hv = fs->sb_copy.s_def_hash_version;
	if (hv < 0 || hv > 5)
		hv = EXT4_HASH_HALF_MD4;

	uint8_t *b0 = (uint8_t *)kalloc(bs);
	uint8_t *leaf = (uint8_t *)kalloc(bs);
	if (!b0 || !leaf) {
		if (b0)
			kfree(b0);
		if (leaf)
			kfree(leaf);
		return ST_NOMEM;
	}

	unsigned long pbn0 = ext4_block_map(fs, dir_ino, 0);
	if (pbn0 == 0 || ext4_read_block(fs, pbn0, b0) != ST_OK) {
		kfree(b0);
		kfree(leaf);
		return ST_IO;
	}

	/* Collect block 0's real entries (skip dot/dotdot) into the new leaf. */
	unsigned scan_limit =
		bs - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	mm_memset(leaf, 0, bs);
	{
		ext4_dir_entry_2 *e0 = (ext4_dir_entry_2 *)
			leaf; /* one empty spanning record */
		e0->inode = 0;
		e0->rec_len = (uint16_t)scan_limit;
	}
	unsigned off = 0;
	unsigned long parent_ino = EXT4_ROOT_INO;
	unsigned long self_ino = dir_ino;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(b0 + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		if (de->inode != 0) {
			int isdot = (de->name_len == 1 && de->name[0] == '.');
			int isdd = (de->name_len == 2 && de->name[0] == '.' &&
				    de->name[1] == '.');
			if (isdd)
				parent_ino = de->inode;
			else if (isdot)
				self_ino = de->inode;
			else if (!ext4_leaf_insert(fs, leaf, de->name,
						   de->name_len, de->inode,
						   de->file_type)) {
				kfree(b0);
				kfree(leaf);
				return ST_NOSPC; /* shouldn't happen: same data, more room */
			}
		}
		off += rec;
	}

	long lblk = ext4_dir_grow(fs, dir_ino,
				  din); /* new leaf = logical block 1 */
	if (lblk < 0) {
		kfree(b0);
		kfree(leaf);
		return (int)lblk;
	}
	ext4_dir_csum_set(fs, dir_ino, gen, leaf);
	if (ext4_dir_write_lblk(fs, dir_ino, (unsigned long)lblk, leaf) !=
	    ST_OK) {
		kfree(b0);
		kfree(leaf);
		return ST_IO;
	}

	/* Rebuild block 0 as the dx_root. */
	mm_memset(b0, 0, bs);
	ext4_dir_entry_2 *dot = (ext4_dir_entry_2 *)b0;
	dot->inode = (uint32_t)self_ino;
	dot->rec_len = 12;
	dot->name_len = 1;
	dot->file_type = EXT4_FT_DIR;
	dot->name[0] = '.';
	ext4_dir_entry_2 *dd = (ext4_dir_entry_2 *)(b0 + 12);
	dd->inode = (uint32_t)parent_ino;
	dd->rec_len = (uint16_t)(bs - 12);
	dd->name_len = 2;
	dd->file_type = EXT4_FT_DIR;
	dd->name[0] = '.';
	dd->name[1] = '.';
	ext4_dx_root_info *info = (ext4_dx_root_info *)(b0 + 24);
	info->reserved_zero = 0;
	info->hash_version = (uint8_t)hv;
	info->info_length = 8;
	info->indirect_levels = 0;
	info->unused_flags = 0;
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(b0 + 32);
	cl->limit = (uint16_t)ext4_dx_root_limit(fs);
	cl->count = 1;
	ext4_dx_entry *e = (ext4_dx_entry *)(b0 + 32);
	e[0].block = (uint32_t)lblk; /* whole hash range -> the one leaf */
	ext4_dx_csum_set(fs, dir_ino, gen, b0, 1);
	int st = ext4_dir_write_lblk(fs, dir_ino, 0, b0);
	kfree(b0);
	kfree(leaf);
	if (st != ST_OK)
		return st;

	din->i_flags |= EXT4_INODE_INDEX_FL;
	if (ext4_write_inode_struct(fs, dir_ino, din) != ST_OK)
		return ST_IO;
	ext4_inode_cache_flush(fs);
	return ST_OK;
}

/* Split the full leaf at logical `leaf_lblk` (whose contents are in `leafbuf`),
 * inserting the new entry; returns the lowest hash of the upper half in
 * *split_hash and the new leaf's logical block in *new_lblk. */
static int ext4_htree_split_leaf(ext4_fs_t *fs, unsigned long dir_ino,
				 ext4_inode *din, int hv, uint8_t *leafbuf,
				 unsigned long leaf_lblk, const char *name,
				 unsigned name_len, unsigned long child_ino,
				 unsigned ftype, uint32_t *split_hash,
				 unsigned long *new_lblk)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	unsigned scan_limit =
		bs - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	/* Two fresh output buffers: the refs point INTO leafbuf, so neither output may
     * be leafbuf (ext4_dx_build_leaf memsets its destination first). */
	/* One ref per live entry plus the new one.  A dirent takes at least 8
	 * header bytes, so bs/8 + 1 always suffices -- a ref is 16 bytes, so
	 * sizing this as one block held only 256, and a leaf of short names
	 * (up to 340 in a 4 KiB block) could never be split. */
	unsigned cap = bs / 8 + 1;
	ext4_dx_ref *refs = (ext4_dx_ref *)kalloc(cap * sizeof(ext4_dx_ref));
	uint8_t *lowleaf = (uint8_t *)kalloc(bs);
	uint8_t *newleaf = (uint8_t *)kalloc(bs);
	if (!refs || !lowleaf || !newleaf) {
		if (refs)
			kfree(refs);
		if (lowleaf)
			kfree(lowleaf);
		if (newleaf)
			kfree(newleaf);
		return ST_NOMEM;
	}
	int n = 0;

	unsigned off = 0;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(leafbuf + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		if (de->inode != 0 && (unsigned)n < cap) {
			refs[n].de = de;
			refs[n].hash =
				ext4_dx_hash(fs, hv, de->name, de->name_len, 0);
			n++;
		}
		off += rec;
	}
	/* Add the new entry via a synthetic dirent. */
	uint8_t newent[8 + 256];
	ext4_dir_entry_2 *nde = (ext4_dir_entry_2 *)newent;
	nde->inode = (uint32_t)child_ino;
	nde->rec_len = 0;
	nde->name_len = (uint8_t)name_len;
	nde->file_type = (uint8_t)ftype;
	mm_memcpy(nde->name, name, name_len);
	if ((unsigned)n >= cap) {
		kfree(refs);
		kfree(lowleaf);
		kfree(newleaf);
		return ST_NOSPC;
	}
	refs[n].de = nde;
	refs[n].hash = ext4_dx_hash(fs, hv, name, name_len, 0);
	n++;

	ext4_dx_sort(refs, n);

	/* Split by accumulated size so both halves fit. */
	unsigned total = 0;
	for (int i = 0; i < n; i++)
		total += ext4_dirent_len(refs[i].de->name_len);
	unsigned acc = 0;
	int m = 1;
	for (int i = 0; i < n; i++) {
		acc += ext4_dirent_len(refs[i].de->name_len);
		if (acc * 2 >= total) {
			m = i + 1;
			break;
		}
	}
	if (m < 1)
		m = 1;
	if (m > n - 1)
		m = n - 1;

	long nl = ext4_dir_grow(fs, dir_ino, din);
	if (nl < 0) {
		kfree(refs);
		kfree(lowleaf);
		kfree(newleaf);
		return (int)nl;
	}

	uint32_t sh = refs[m].hash;
	if (m > 0 && refs[m - 1].hash == refs[m].hash)
		sh |= 1; /* collision continuation */

	ext4_dx_build_leaf(fs, dir_ino, gen, lowleaf, refs, 0,
			   m); /* lower half -> old block */
	ext4_dx_build_leaf(fs, dir_ino, gen, newleaf, refs, m,
			   n); /* upper half -> new block */

	int st = ext4_dir_write_lblk(fs, dir_ino, leaf_lblk, lowleaf);
	if (st == ST_OK)
		st = ext4_dir_write_lblk(fs, dir_ino, (unsigned long)nl,
					 newleaf);
	kfree(refs);
	kfree(lowleaf);
	kfree(newleaf);
	if (st != ST_OK)
		return st;
	*split_hash = sh;
	*new_lblk = (unsigned long)nl;
	return ST_OK;
}

/* Add an entry to an htree directory (din already has INDEX_FL). */
static int ext4_htree_add(ext4_fs_t *fs, unsigned long dir_ino, ext4_inode *din,
			  const char *name, unsigned name_len,
			  unsigned long child_ino, unsigned ftype)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	uint8_t *root = (uint8_t *)kalloc(bs);
	uint8_t *node = (uint8_t *)kalloc(bs);
	uint8_t *leaf = (uint8_t *)kalloc(bs);
	if (!root || !node || !leaf) {
		if (root)
			kfree(root);
		if (node)
			kfree(node);
		if (leaf)
			kfree(leaf);
		return ST_NOMEM;
	}

	int rc = ST_IO;
	unsigned long pbn = ext4_block_map(fs, dir_ino, 0);
	if (pbn == 0 || ext4_read_block(fs, pbn, root) != ST_OK)
		goto out;

	ext4_dx_root_info *info = (ext4_dx_root_info *)(root + 24);
	int hv = info->hash_version;
	int levels = info->indirect_levels;
	if (hv < 0 || hv > 5)
		hv = EXT4_HASH_HALF_MD4;
	uint32_t hash = ext4_dx_hash(fs, hv, name, name_len, 0);

	/* Descend root -> [node] -> leaf, remembering whether the parent of the leaf
     * is the root (depth 0) or the interior node (depth 1). */
	ext4_dx_entry *re = (ext4_dx_entry *)(root + 32);
	ext4_dx_countlimit *rcl = (ext4_dx_countlimit *)(root + 32);
	unsigned rcount = rcl->count, rat = 0;
	for (unsigned i = 1; i < rcount; i++) {
		if ((re[i].hash & ~1u) <= hash)
			rat = i;
		else
			break;
	}

	unsigned long leaf_lblk;
	uint8_t *parent;
	unsigned parent_off;
	unsigned long node_lblk = 0;
	int parent_is_root;
	if (levels == 0) {
		leaf_lblk = re[rat].block;
		parent = root;
		parent_off = 32;
		parent_is_root = 1;
	} else if (levels == 1) {
		node_lblk = re[rat].block;
		unsigned long npbn = ext4_block_map(fs, dir_ino, node_lblk);
		if (npbn == 0 || ext4_read_block(fs, npbn, node) != ST_OK)
			goto out;
		ext4_dx_entry *ne = (ext4_dx_entry *)(node + 8);
		ext4_dx_countlimit *ncl = (ext4_dx_countlimit *)(node + 8);
		unsigned ncount = ncl->count, nat = 0;
		for (unsigned i = 1; i < ncount; i++) {
			if ((ne[i].hash & ~1u) <= hash)
				nat = i;
			else
				break;
		}
		leaf_lblk = ne[nat].block;
		parent = node;
		parent_off = 8;
		parent_is_root = 0;
	} else {
		rc = ST_NOSPC;
		goto out;
	} /* depth>1: node split / depth 2 unsupported */

	unsigned long lpbn = ext4_block_map(fs, dir_ino, leaf_lblk);
	if (lpbn == 0 || ext4_read_block(fs, lpbn, leaf) != ST_OK)
		goto out;

	if (ext4_leaf_insert(fs, leaf, name, name_len, child_ino, ftype)) {
		ext4_dir_csum_set(fs, dir_ino, gen, leaf);
		rc = ext4_dir_write_lblk(fs, dir_ino, leaf_lblk, leaf);
		goto out;
	}

	/* Leaf full: split it, then add (split_hash,new_leaf) to the parent index. */
	uint32_t split_hash;
	unsigned long new_lblk;
	rc = ext4_htree_split_leaf(fs, dir_ino, din, hv, leaf, leaf_lblk, name,
				   name_len, child_ino, ftype, &split_hash,
				   &new_lblk);
	if (rc != ST_OK)
		goto out;
	/* din/extents changed in the split; re-read root & node to map blocks safely. */
	ext4_inode_cache_flush(fs);

	ext4_dx_countlimit *pcl = (ext4_dx_countlimit *)(parent + parent_off);
	if (pcl->count < pcl->limit) { /* room in the parent */
		ext4_dx_insert_entry(parent, parent_off, split_hash,
				     (uint32_t)new_lblk);
		ext4_dx_csum_set(fs, dir_ino, gen, parent, parent_is_root);
		rc = ext4_dir_write_lblk(
			fs, dir_ino, parent_is_root ? 0 : node_lblk, parent);
		goto out;
	}

	if (!parent_is_root) {
		rc = ST_NOSPC;
		goto out;
	} /* interior node full: node split unsupported */

	/* Root full at depth 0: grow to depth 1.  Move all root entries into a new
     * node block, point the root at it, then insert into the (roomy) node. */
	long nl = ext4_dir_grow(fs, dir_ino, din);
	if (nl < 0) {
		rc = (int)nl;
		goto out;
	}
	mm_memset(node, 0, bs);
	ext4_dir_entry_2 *fake = (ext4_dir_entry_2 *)node;
	fake->inode = 0;
	fake->rec_len = (uint16_t)bs;
	fake->name_len = 0;
	fake->file_type = 0;
	ext4_dx_entry *nentry = (ext4_dx_entry *)(node + 8);
	unsigned cnt = rcl->count;
	for (unsigned i = 0; i < cnt; i++)
		nentry[i] = re[i]; /* copies blocks (and hashes for i>=1) */
	ext4_dx_countlimit *ncl = (ext4_dx_countlimit *)(node + 8);
	ncl->limit = (uint16_t)ext4_dx_node_limit(fs);
	ncl->count = (uint16_t)cnt; /* aliases nentry[0].hash */
	ext4_dx_insert_entry(node, 8, split_hash, (uint32_t)new_lblk);
	ext4_dx_csum_set(fs, dir_ino, gen, node, 0);
	rc = ext4_dir_write_lblk(fs, dir_ino, (unsigned long)nl, node);
	if (rc != ST_OK)
		goto out;

	rcl->limit = (uint16_t)ext4_dx_root_limit(fs);
	rcl->count = 1;
	re[0].block = (uint32_t)nl; /* sole root entry -> the node */
	info->indirect_levels = 1;
	ext4_dx_csum_set(fs, dir_ino, gen, root, 1);
	rc = ext4_dir_write_lblk(fs, dir_ino, 0, root);

out:
	kfree(root);
	kfree(node);
	kfree(leaf);
	return rc;
}

static int ext4_dir_add_disk(ext4_fs_t *fs, unsigned long dir_ino,
			     const char *name, unsigned name_len,
			     unsigned long child_ino, unsigned ftype)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return ST_IO;
	if (din.i_flags & EXT4_INODE_INDEX_FL) /* hash-indexed dir */
		return ext4_htree_add(fs, dir_ino, &din, name, name_len,
				      child_ino, ftype);
	unsigned long dsize = ext4_inode_size(&din);
	unsigned long nblocks = dsize / fs->block_size;
	unsigned need = ext4_dirent_len(name_len);
	/* With metadata_csum the last 12 bytes of every leaf hold the checksum tail
     * (a fake entry) and must never be allocated into. */
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;

	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn;
		int mrc = ext4_map_blk(fs, dir_ino, b, &pbn);
		if (mrc != ST_OK) {
			/* A damaged map (reported): not a place to skip. */
			kfree(blk);
			return mrc;
		}
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return ST_IO;
		}
		unsigned off = 0;
		while (off + 8 <= scan_limit) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > scan_limit)
				break;
			unsigned used = (de->inode == 0) ?
						0 :
						ext4_dirent_len(de->name_len);
			if (rec - used >= need) {
				ext4_dir_entry_2 *ne;
				if (de->inode == 0) {
					ne = de; /* reuse empty slot          */
					ne->rec_len = (uint16_t)rec;
				} else {
					de->rec_len = (uint16_t)
						used; /* shrink, split off the tail */
					ne = (ext4_dir_entry_2 *)(blk + off +
								  used);
					ne->rec_len = (uint16_t)(rec - used);
				}
				ne->inode = (uint32_t)child_ino;
				ne->name_len = (uint8_t)name_len;
				ne->file_type = (uint8_t)ftype;
				mm_memcpy(ne->name, name, name_len);
				ext4_dir_csum_set(fs, dir_ino, din.i_generation,
						  blk);
				int st = ext4_write_block(fs, pbn, blk);
				kfree(blk);
				return st;
			}
			off += rec;
		}
	}

	/* No room and this is still a single-block dir: if the fs supports dir_index,
     * convert to an htree (at the 1->2 block transition)
     * and add via the index path. */
	if (nblocks == 1 && !(din.i_flags & EXT4_INODE_INDEX_FL) &&
	    (fs->sb_copy.s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX)) {
		kfree(blk);
		int cv = ext4_htree_make_indexed(fs, dir_ino, &din);
		if (cv != ST_OK)
			return cv;
		return ext4_htree_add(fs, dir_ino, &din, name, name_len,
				      child_ino, ftype);
	}

	/* No room in existing blocks: append a fresh directory block. */
	unsigned alloc =
		ext4_alloc_blocks_for_file(fs, dir_ino, &din, nblocks, 1);
	if (alloc != 1) {
		kfree(blk);
		return ST_NOMEM;
	}
	/* The block just mapped is in `din' only (not yet stored): map it from
	 * there -- through every level of its extent tree or block map. */
	ext4_inode_cache_flush(fs);
	unsigned long pbn = ext4_map_in(fs, dir_ino, &din, nblocks);
	if (pbn == 0) {
		ext4_free_blocks_from(fs, dir_ino, &din, nblocks);
		kfree(blk);
		return ST_IO;
	}
	mm_memset(blk, 0, fs->block_size);
	ext4_dir_entry_2 *ne = (ext4_dir_entry_2 *)blk;
	ne->inode = (uint32_t)child_ino;
	ne->rec_len = (uint16_t)(fs->block_size - tail);
	ne->name_len = (uint8_t)name_len;
	ne->file_type = (uint8_t)ftype;
	mm_memcpy(ne->name, name, name_len);
	ext4_dir_csum_set(fs, dir_ino, din.i_generation, blk);
	int st = ext4_write_block(fs, pbn, blk);
	kfree(blk);
	if (st != ST_OK)
		return st;
	/* grow directory size + persist the inode (with the new extent). */
	din.i_size_lo = (uint32_t)(dsize + fs->block_size);
	ext4_write_inode_struct(fs, dir_ino, &din);
	return ST_OK;
}

/*
 * Add `name` to `dir_ino` and tell the dentry cache about it.
 *
 * The cache is updated only once the entry is on the device, so a failed add
 * leaves nothing claiming the name exists.  Every way this driver creates a
 * name -- create, mkdir, mknod, symlink, link, the destination half of a
 * rename -- arrives here, which is why the maintenance is here and not spread
 * across all of them.
 *
 * A name that had a negative entry gets it replaced rather than merely
 * dropped: the insert removes whatever was there first.  That is the case
 * where a program stats a file, does not find it, creates it, and stats it
 * again -- and must find it the second time.
 */
int ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype)
{
	int st = ext4_dir_add_disk(fs, dir_ino, name, name_len, child_ino,
				   ftype);

	if (st == ST_OK)
		ext4_dc_add(fs, dir_ino, name, name_len, child_ino, ftype);
	return st;
}

/* Remove `name` from directory `dir_ino`.  Returns the removed child inode in
 * *out_child (and its file_type in *out_ft).  Coalesces the freed record into
 * the previous entry. */
static int ext4_dir_del_disk(ext4_fs_t *fs, unsigned long dir_ino,
			     const char *name, unsigned name_len,
			     unsigned long *out_child, unsigned *out_ft)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return ST_IO;
	unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
	unsigned scan_limit = fs->block_size -
			      (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn;
		int mrc = ext4_map_blk(fs, dir_ino, b, &pbn);
		if (mrc != ST_OK) {
			/* A damaged map (reported): not a place to skip. */
			kfree(blk);
			return mrc;
		}
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return ST_IO;
		}
		unsigned off = 0, prev = (unsigned)-1;
		while (off + 8 <= scan_limit) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > scan_limit)
				break;
			if (de->inode != 0 && de->name_len == name_len &&
			    ext4_memcmp(de->name, name, name_len) == 0) {
				if (out_child)
					*out_child = de->inode;
				if (out_ft)
					*out_ft = de->file_type;
				if (prev != (unsigned)-1) {
					ext4_dir_entry_2 *pd =
						(ext4_dir_entry_2 *)(blk +
								     prev);
					pd->rec_len =
						(uint16_t)(pd->rec_len + rec);
				} else {
					de->inode =
						0; /* first entry in block: just blank it */
				}
				ext4_dir_csum_set(fs, dir_ino, din.i_generation,
						  blk);
				int st = ext4_write_block(fs, pbn, blk);
				kfree(blk);
				return st;
			}
			prev = off;
			off += rec;
		}
	}
	kfree(blk);
	return ST_NOT_FOUND;
}

/*
 * Remove `name` from `dir_ino` and tell the dentry cache about it.
 *
 * The counterpart of ext4_dir_add, and the same argument: unlink, rmdir and
 * both halves of a rename all remove names through here, so this is the one
 * place that has to remember to say so.
 */
static int ext4_dir_del(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long *out_child,
			unsigned *out_ft)
{
	unsigned long child = 0;
	unsigned ft = 0;
	int st;

	st = ext4_dir_del_disk(fs, dir_ino, name, name_len, &child, &ft);
	if (out_child)
		*out_child = child;
	if (out_ft)
		*out_ft = ft;
	if (st != ST_OK)
		return st;

	ext4_dc_del(fs, dir_ino, name, name_len);

	/* A name that led to a directory takes that directory's own cached
	 * entries with it.
	 *
	 * Two cases need it.  A removed directory leaves "." and ".." behind,
	 * keyed by its inode number, waiting for that number to be handed to
	 * something else.  And a directory MOVED by a rename keeps a ".."
	 * that now points at the parent it no longer has -- rename removes the
	 * old name through here, so dropping the entries here covers it.
	 *
	 * EXT4_FT_UNKNOWN is treated the same way: a filesystem without the
	 * filetype feature records every entry as unknown, and guessing wrong
	 * in that direction only costs a rescan. */
	if (ft == EXT4_FT_DIR || ft == EXT4_FT_UNKNOWN)
		ext4_dc_drop_dir(fs, child);
	return st;
}

/* Split a path into parent directory inode + final component name. */
int ext4_resolve_parent(ext4_fs_t *fs, const char *path,
			       unsigned long *parent_ino, char *name_out,
			       unsigned cap)
{
	int last = -1;
	for (int i = 0; path[i]; i++)
		if (path[i] == '/')
			last = i;
	const char *base = (last >= 0) ? path + last + 1 : path;
	unsigned bl = 0;
	while (base[bl])
		bl++;
	if (bl == 0 || bl >= cap)
		return ST_INVALID;
	for (unsigned i = 0; i <= bl; i++)
		name_out[i] = base[i];

	if (last <= 0) { /* parent is root ('/foo') or cwd ('foo') */
		*parent_ino = (last == 0) ? EXT4_ROOT_INO : fs->st->cwd_ino;
		return ST_OK;
	}
	/* The directory part, as a string of its own: a path buffer (it is
	 * up to a whole path long). */
	if (last >= PATHBUF_SIZE)
		return ST_NAMETOOLONG;
	char *pdir = pathbuf_get();
	if (!pdir)
		return ST_NOMEM;
	for (int i = 0; i < last; i++)
		pdir[i] = path[i];
	pdir[last] = '\0';
	int r = ext4_resolve(fs, pdir, parent_ino);
	pathbuf_put(pdir);
	return r;
}

int ext4_unlink_impl(ext4_fs_t *fs, const char *path)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;

	unsigned long child = 0;
	unsigned ft = 0;
	/* Look up first so we can reject directories and recover the inode. */
	if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode cin;
	if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK)
		return ST_IO;
	if ((cin.i_mode & S_IFMT) == S_IFDIR)
		return ST_ISDIR;

	if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK)
		return ST_NOT_FOUND;

	if (cin.i_links_count > 0)
		cin.i_links_count--;
	if (cin.i_links_count == 0 &&
	    icache_flag_if_referenced(&fs->sb, EXT4_BID_ENC(child, 0), IC_ORPHAN)) {
		/* Still open somewhere.  The name is gone, but the inode and
		 * its blocks stay until the last close (ext4_orphan_reap):
		 * freeing them now hands the number to the next mkdir or
		 * creat, and the open handle's later writes then land in
		 * THAT file -- seen as a fresh directory whose size fell
		 * back to zero because a temp file's writer outlived its
		 * unlink.  Such inodes belong on the orphan list for the
		 * same reason. */
		ext4_stamp_now(fs, &cin, EXT4_T_CTIME);
		ext4_write_inode_struct(fs, child, &cin);
	} else if (cin.i_links_count == 0) {
		/* Cache before blocks -- see ext4_truncate_impl().  An unlinked
		 * file can still have dirty pages: a writer that closed without
		 * fsync, or a reader still holding it open. */
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
		ext4_free_blocks_from(fs, child, &cin, 0);
		cin.i_dtime = (uint32_t)timer_get_epoch();
		cin.i_size_lo = 0;
		cin.i_size_high = 0;
		ext4_write_inode_struct(fs, child, &cin);
		ext4_free_inode(fs, child, 0);
		icache_remove(&fs->sb, EXT4_BID_ENC(child, 0));
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	} else {
		ext4_stamp_now(fs, &cin, EXT4_T_CTIME);
		ext4_write_inode_struct(fs, child, &cin);
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* Fill a fresh directory block with "." (self) and ".." (parent) entries. */
static void ext4_init_dir_block(ext4_fs_t *fs, uint8_t *blk,
				unsigned long self_ino,
				unsigned long parent_ino, uint32_t gen)
{
	unsigned bs = fs->block_size;
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	mm_memset(blk, 0, bs);
	ext4_dir_entry_2 *dot = (ext4_dir_entry_2 *)blk;
	dot->inode = (uint32_t)self_ino;
	dot->rec_len = 12;
	dot->name_len = 1;
	dot->file_type = EXT4_FT_DIR;
	dot->name[0] = '.';
	ext4_dir_entry_2 *dd = (ext4_dir_entry_2 *)(blk + 12);
	dd->inode = (uint32_t)parent_ino;
	dd->rec_len = (uint16_t)(bs - 12 - tail);
	dd->name_len = 2;
	dd->file_type = EXT4_FT_DIR;
	dd->name[0] = '.';
	dd->name[1] = '.';
	ext4_dir_csum_set(fs, self_ino, gen,
			  blk); /* lay out + checksum the tail */
}

/* Return 1 if the directory contains only "." and "..". */
static int ext4_dir_is_empty(ext4_fs_t *fs, unsigned long dir_ino)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return 0;
	unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return 0;
	int empty = 1;
	for (unsigned long b = 0; b < nblocks && empty; b++) {
		unsigned long pbn;
		if (ext4_map_blk(fs, dir_ino, b, &pbn) != ST_OK) {
			/* A damaged map (reported): refuse the rmdir, as
			 * for a corrupt block below. */
			kfree(blk);
			return 0;
		}
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return 0; /* corrupt -> treat as "not empty": refuse the rmdir */
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size)
				break;
			if (de->inode != 0 &&
			    !((de->name_len == 1 && de->name[0] == '.') ||
			      (de->name_len == 2 && de->name[0] == '.' &&
			       de->name[1] == '.'))) {
				empty = 0;
				break;
			}
			off += rec;
		}
	}
	kfree(blk);
	return empty;
}

/* Point a directory's ".." entry at a new parent (used by cross-dir rename). */
static void ext4_dir_set_dotdot(ext4_fs_t *fs, unsigned long dir_ino,
				unsigned long new_parent)
{
	ext4_inode di;
	uint32_t gen = ext4_get_inode_cached(fs, dir_ino, &di) ?
			       di.i_generation :
			       0;
	unsigned long pbn = ext4_block_map(fs, dir_ino, 0);
	if (pbn == 0)
		return;
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return;
	if (ext4_read_block(fs, pbn, blk) == ST_OK) {
		if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			kfree(blk);
			return;
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size)
				break;
			if (de->name_len == 2 && de->name[0] == '.' &&
			    de->name[1] == '.') {
				de->inode = (uint32_t)new_parent;
				ext4_dir_csum_set(fs, dir_ino, gen,
						  blk); /* re-stamp the tail */
				ext4_write_block(fs, pbn, blk);
				break;
			}
			off += rec;
		}
	}
	kfree(blk);
}

/* Link counts are 16 bits.  A file takes at most EXT4_LINK_MAX links; so
 * does a directory (each subdirectory's ".." is one) -- except on a
 * filesystem with dir_nlink, where a directory past it counts 1, meaning
 * "more than the field holds", and stays at 1 until its subdirectories
 * are counted again by fsck. */
#define EXT4_LINK_MAX 65000

/* May directory `ino' take one more subdirectory (its ".." link)? */
static int ext4_dir_link_ok(ext4_fs_t *fs, unsigned long ino)
{
	ext4_inode in;

	if (fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_DIR_NLINK)
		return 1;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return 1; /* the operation reports the error itself */
	return in.i_links_count < EXT4_LINK_MAX;
}

/* Adjust an inode's link count by `delta` and bump ctime. */
static void ext4_adjust_links(ext4_fs_t *fs, unsigned long ino, int delta)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return;
	if ((in.i_mode & S_IFMT) == S_IFDIR) {
		/* 1: uncounted (dir_nlink, see EXT4_LINK_MAX) -- it stays 1;
		 * 2 is the least a directory has ("." and its entry). */
		unsigned n = in.i_links_count;

		if (delta > 0 && n != 1) {
			n += (unsigned)delta;
			if (n > EXT4_LINK_MAX)
				n = 1;
		} else if (delta < 0 && n > 2) {
			n = (n > (unsigned)(-delta) + 2) ? n + delta : 2;
		}
		in.i_links_count = (uint16_t)n;
	} else if (delta < 0 && in.i_links_count < (unsigned)(-delta))
		in.i_links_count = 0;
	else
		in.i_links_count = (uint16_t)(in.i_links_count + delta);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_struct(fs, ino, &in);
}

int ext4_mkdir_impl(ext4_fs_t *fs, const char *path, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK)
		return ST_EXISTS;

	/* Owner/group of the new directory: the creating process (a set-group-ID
     * parent passes its group down and the new dir inherits the set-group-ID
     * bit via ext4_init_owner). */
	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin0;
	if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin0);
		pgid = ext4_inode_gid(&pin0);
		pmode = pin0.i_mode;
	}
	unsigned nuid = puid, ngid = pgid;
	unsigned nmode = S_IFDIR | (mode ? (mode & 0777) : 0755);
	ext4_init_owner(puid, pgid, pmode, 1, &nuid, &ngid, &nmode);
	if (!ext4_dir_link_ok(fs, parent))
		return ST_NOSPC; /* the parent's link count is full */
	unsigned long nino = ext4_alloc_inode(fs, parent, 1);
	if (nino == 0)
		return ST_NOMEM;

	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 2; /* "." + the entry in parent       */
	ext4_new_block_map(fs, &in);
	uint32_t now = (uint32_t)timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;

	if (ext4_alloc_blocks_for_file(fs, nino, &in, 0, 1) != 1) {
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_NOMEM;
	}
	in.i_size_lo = fs->block_size;
	unsigned long pbn = ext4_map_in(fs, nino, &in, 0);
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk) {
		ext4_free_blocks_from(fs, nino, &in, 0);
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_NOMEM;
	}
	ext4_init_dir_block(fs, blk, nino, parent, in.i_generation);
	ext4_write_block(fs, pbn, blk); /* dir block is metadata           */
	kfree(blk);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_new(fs, nino,
			     &in); /* fresh inode: clear stale ibody xattrs */

	if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_DIR) != ST_OK) {
		ext4_free_blocks_from(fs, nino, &in, 0);
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_IO;
	}
	ext4_adjust_links(fs, parent,
			  +1); /* new dir's ".." references parent */
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* Create a node that owns no data blocks: a socket or a FIFO.
 *
 * Simpler than ext4_mkdir_impl above — there is no directory block to
 * allocate, no "." / ".." to write, and the parent's link count is untouched.
 * The point of it is that a bound AF_UNIX socket has to EXIST in the
 * filesystem: clients stat() the path and refuse to connect unless it reports
 * S_IFSOCK, so an absent entry (or a plain file standing in for one) fails
 * every connect rather than merely looking untidy in ls. */
/* The directory-entry file type for an inode mode. */
static unsigned ext4_mode_to_ft(unsigned mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return EXT4_FT_DIR;
	case S_IFLNK:
		return EXT4_FT_SYMLINK;
	case S_IFCHR:
		return EXT4_FT_CHRDEV;
	case S_IFBLK:
		return EXT4_FT_BLKDEV;
	case S_IFIFO:
		return EXT4_FT_FIFO;
	case S_IFSOCK:
		return EXT4_FT_SOCK;
	default:
		return EXT4_FT_REG_FILE;
	}
}

/* Create a node that owns no blocks: a socket, a FIFO, or a character or
 * block device node.  Such an inode has neither the extents flag nor an
 * extent header -- fsck rejects a special file that claims to map blocks --
 * and a device node keeps its number in i_block: the old encoding (major
 * and minor below 256, in i_block[0]) when it fits, the new one (in
 * i_block[1], i_block[0] zero) otherwise. */
int ext4_mknod_impl(ext4_fs_t *fs, const char *path, unsigned mode,
			   uint64_t dev)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned type = mode & S_IFMT;
	if (type != S_IFSOCK && type != S_IFIFO && type != S_IFCHR &&
	    type != S_IFBLK)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK)
		return ST_EXISTS;

	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin0;
	if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin0);
		pgid = ext4_inode_gid(&pin0);
		pmode = pin0.i_mode;
	}
	unsigned nuid = puid, ngid = pgid;
	unsigned nmode = mode;
	ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);

	unsigned long nino = ext4_alloc_inode(fs, parent, 0);
	if (nino == 0)
		return ST_NOMEM;

	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 1;
	in.i_flags = 0;
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;
	ext4_stamp_now(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME);
	if (type == S_IFCHR || type == S_IFBLK) {
		uint32_t ma = kdev_major(dev), mi = kdev_minor(dev);
		uint32_t w[2] = { 0, 0 };

		if (ma < 256 && mi < 256)
			w[0] = (ma << 8) | mi;
		else
			w[1] = (mi & 0xffu) | (ma << 8) | ((mi & ~0xffu) << 12);
		mm_memcpy(in.i_block, w, sizeof(w));
	}
	ext4_inode_cache_flush(fs);
	ext4_write_inode_new(fs, nino, &in);

	unsigned ft = ext4_mode_to_ft(mode);

	if (ext4_dir_add(fs, parent, name, nl, nino, ft) != ST_OK) {
		ext4_abandon_new_inode(fs, nino, 0);
		ext4_flush_meta(fs);
		return ST_IO;
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

int ext4_rmdir_impl(ext4_fs_t *fs, const char *path)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	if (nl == 1 && name[0] == '.')
		return ST_INVALID;

	unsigned long child = 0;
	unsigned ft = 0;
	if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode cin;
	if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK)
		return ST_IO;
	if ((cin.i_mode & S_IFMT) != S_IFDIR)
		return ST_NOTDIR;
	if (!ext4_dir_is_empty(fs, child))
		return ST_NOTEMPTY;

	if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK)
		return ST_NOT_FOUND;
	/* Cache before blocks -- see ext4_truncate_impl(). */
	pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	ext4_free_blocks_from(fs, child, &cin, 0);
	cin.i_links_count = 0;
	cin.i_size_lo = 0;
	cin.i_dtime = (uint32_t)timer_get_epoch();
	ext4_write_inode_struct(fs, child, &cin);
	ext4_free_inode(fs, child, 1);
	icache_remove(&fs->sb, EXT4_BID_ENC(child, 0));
	pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	ext4_adjust_links(fs, parent, -1); /* child's ".." is gone            */
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

int ext4_rename_impl(ext4_fs_t *fs, const char *oldp, const char *newp)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!oldp || !newp || !fs)
		return ST_INVALID;
	unsigned long op = 0, np = 0;
	char oname[256], nname[256];
	if (ext4_resolve_parent(fs, oldp, &op, oname, sizeof(oname)) != ST_OK)
		return ST_NOT_FOUND;
	if (ext4_resolve_parent(fs, newp, &np, nname, sizeof(nname)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned onl = 0;
	while (oname[onl])
		onl++;
	unsigned nnl = 0;
	while (nname[nnl])
		nnl++;

	unsigned long src = 0;
	unsigned sft = 0;
	if (ext4_dir_lookup(fs, op, oname, onl, &src, &sft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode sin;
	if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK)
		return ST_IO;
	int src_is_dir = ((sin.i_mode & S_IFMT) == S_IFDIR);

	/* A directory cannot move below itself: walking up from the new
	 * parent must not meet it, or the subtree would hang off nothing. */
	if (src_is_dir && op != np) {
		unsigned long cur = np;

		if (!ext4_dir_link_ok(fs, np))
			return ST_NOSPC; /* np's link count is full */

		for (unsigned depth = 0; cur != EXT4_ROOT_INO; depth++) {
			unsigned long up = 0;

			if (cur == src)
				return ST_INVALID;
			if (depth > 4096 ||
			    ext4_dir_lookup(fs, cur, "..", 2, &up, 0) !=
				    ST_OK ||
			    up == cur)
				break;
			cur = up;
		}
	}

	/* If the destination exists, remove it: a file replaced by a file, an
     * empty directory by a directory.  A file never replaces a directory,
     * nor a directory a file. */
	unsigned long dst = 0;
	unsigned dft = 0;
	if (ext4_dir_lookup(fs, np, nname, nnl, &dst, &dft) == ST_OK) {
		if (dst == src)
			return ST_OK; /* renaming to itself              */
		ext4_inode din;
		if (ext4_read_inode_loc(fs, dst, &din, 0, 0) != ST_OK)
			return ST_IO;
		int dst_is_dir = (din.i_mode & S_IFMT) == S_IFDIR;

		if (dst_is_dir && !src_is_dir)
			return ST_ISDIR;
		if (!dst_is_dir && src_is_dir)
			return ST_NOTDIR;
		if (dst_is_dir && !ext4_dir_is_empty(fs, dst))
			return ST_NOTEMPTY;
		if (dst_is_dir) {
			/* An empty directory is replaced, removed the way
			 * rmdir removes one (ext4_rmdir_impl). */
			ext4_dir_del(fs, np, nname, nnl, 0, 0);
			pagecache_invalidate_file(&fs->sb,
						  EXT4_BID_ENC(dst, 0));
			ext4_free_blocks_from(fs, dst, &din, 0);
			din.i_links_count = 0;
			din.i_size_lo = 0;
			din.i_dtime = (uint32_t)timer_get_epoch();
			ext4_write_inode_struct(fs, dst, &din);
			ext4_free_inode(fs, dst, 1);
			icache_remove(&fs->sb, EXT4_BID_ENC(dst, 0));
			pagecache_invalidate_file(&fs->sb,
						  EXT4_BID_ENC(dst, 0));
			ext4_adjust_links(fs, np, -1); /* dst's ".." gone */
			ext4_inode_cache_flush(fs);
			goto replaced;
		}
		ext4_dir_del(fs, np, nname, nnl, 0, 0);
		if (din.i_links_count > 0)
			din.i_links_count--;
		if (din.i_links_count == 0 &&
		    icache_flag_if_referenced(&fs->sb, EXT4_BID_ENC(dst, 0), IC_ORPHAN)) {
			/* Open somewhere: kept until its last close, as in
			 * ext4_unlink_impl(). */
			ext4_stamp_now(fs, &din, EXT4_T_CTIME);
			ext4_write_inode_struct(fs, dst, &din);
		} else if (din.i_links_count == 0) {
			/* Cache before blocks -- see ext4_truncate_impl().
			 * The file being replaced is an ordinary one and may
			 * well have dirty pages of its own. */
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(dst, 0));
			ext4_free_blocks_from(fs, dst, &din, 0);
			din.i_size_lo = 0;
			din.i_dtime = (uint32_t)timer_get_epoch();
			ext4_write_inode_struct(fs, dst, &din);
			ext4_free_inode(fs, dst, 0);
			icache_remove(&fs->sb, EXT4_BID_ENC(dst, 0));
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(dst, 0));
		} else {
			ext4_write_inode_struct(fs, dst, &din);
		}
		ext4_inode_cache_flush(fs);
	}
replaced:

	if (ext4_dir_add(fs, np, nname, nnl, src, sft) != ST_OK)
		return ST_IO;
	ext4_dir_del(fs, op, oname, onl, 0, 0);

	if (src_is_dir && op != np) {
		ext4_dir_set_dotdot(fs, src, np);
		ext4_adjust_links(fs, op,
				  -1); /* src's old ".." left op          */
		ext4_adjust_links(fs, np,
				  +1); /* src's new ".." references np     */
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* A hard link `newpath' to what `oldpath' names -- with `follow', to the
 * target of a final symlink, else to the symlink itself (link()'s and
 * linkat()'s default). */
int ext4_link(ext4_fs_t *fs, const char *oldpath, const char *newpath,
		     int follow)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!oldpath || !newpath || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long src;
	if (ext4_resolve_ex(fs, fs->st->cwd_ino, oldpath, follow ? 1 : 0, &src,
			    0) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_NOT_FOUND;
	}
	ext4_inode sin;
	if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_IO;
	}
	if ((sin.i_mode & S_IFMT) == S_IFDIR) {
		ext4_io_unlock(fs);
		return ST_INVALID;
	}
	if (sin.i_links_count >= EXT4_LINK_MAX) { /* no room in the count */
		ext4_io_unlock(fs);
		return ST_NOSPC;
	}
	unsigned long parent;
	char name[256];
	if (ext4_resolve_parent(fs, newpath, &parent, name, sizeof(name)) !=
	    ST_OK) {
		ext4_io_unlock(fs);
		return ST_NOT_FOUND;
	}
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK) {
		ext4_io_unlock(fs);
		return ST_EXISTS;
	}
	unsigned ft = ext4_mode_to_ft(sin.i_mode);
	if (ext4_dir_add(fs, parent, name, nl, src, ft) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_IO;
	}
	sin.i_links_count++;
	ext4_stamp_now(fs, &sin, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_struct(fs, src, &sin);
	ext4_flush_meta(fs);
	ext4_io_unlock(fs);
	return ST_OK;
}
