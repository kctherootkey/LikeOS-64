// LikeOS - ext4 filesystem driver: open, stat, read, write, seek and close of
// files
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * VFS operations
 * =================================================================== */

int ext4_open_impl(ext4_fs_t *fs, const char *path, int flags, unsigned cmode,
			  vfs_file_t **out)
{
	if (!out || !path || !path[0] || !fs)
		return ST_INVALID;

	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	/* O_CREAT|O_EXCL fails on a name that exists -- a final symlink
	 * counts as one, wherever it leads.  Decided here, under the I/O lock
	 * that the create below holds too (ext4_open_locked), so two such
	 * opens of one name cannot both succeed. */
	if ((flags & O_CREAT) && (flags & O_EXCL)) {
		unsigned long lino = 0;

		if (rr == ST_OK ||
		    (rr == ST_NOT_FOUND &&
		     ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &lino, 0) ==
			     ST_OK))
			return ST_EXISTS;
	}
	if (rr != ST_OK) {
		/* A genuine I/O / corruption error (e.g. a bad inode metadata_csum)
         * must surface as -EIO, not be masked as "not found" — and must not
         * trigger O_CREAT, since the path may exist but simply be unreadable. */
		if (rr != ST_NOT_FOUND)
			return rr;
		/* Genuinely absent — create it if requested. */
		if (!(flags & O_CREAT))
			return ST_NOT_FOUND;
		/* Read-only (mounted so, or latched after an error): refused
		 * before anything is allocated. */
		if (ext4_is_ro(fs))
			return ST_ROFS;
		unsigned long parent = 0;
		char nm[256];
		if (ext4_resolve_parent(fs, path, &parent, nm, sizeof(nm)) !=
		    ST_OK)
			return ST_NOT_FOUND;
		unsigned nl = 0;
		while (nm[nl])
			nl++;
		/* Owner/group of the new file: the creating process. */
		unsigned puid = 0, pgid = 0, pmode = 0;
		ext4_inode pin0;
		if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
			puid = ext4_inode_uid(&pin0);
			pgid = ext4_inode_gid(&pin0);
			pmode = pin0.i_mode;
		}
		/* The mode the caller asked for, not a fixed 0644.  The
		 * set-id bits are deliberately NOT taken from an O_CREAT
		 * open: the new file's group comes from the parent directory
		 * (set-gid directory inheritance), so honouring S_ISGID here
		 * would let a caller create a file set-gid to a group it does
		 * not belong to.  A program that genuinely wants a set-id bit
		 * uses chmod(), which checks that. */
		unsigned nuid = puid, ngid = pgid,
			 nmode = S_IFREG | (cmode & 0777);
		ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);
		unsigned long newino = ext4_alloc_inode(fs, parent, 0);
		if (newino == 0)
			return ST_NOMEM;
		if (ext4_create_inode(fs, newino, nmode, nuid, ngid) != ST_OK) {
			ext4_abandon_new_inode(fs, newino, 0);
			return ST_IO;
		}
		if (ext4_dir_add(fs, parent, nm, nl, newino,
				 EXT4_FT_REG_FILE) != ST_OK) {
			ext4_abandon_new_inode(fs, newino, 0);
			return ST_IO;
		}
		ext4_inode_cache_flush(fs);
		ino = newino;
	}

	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;

	unsigned mode = in.i_mode;

	/* Opening a file or directory for writing on a read-only filesystem
	 * fails at the open, not at the first write (device nodes, FIFOs and
	 * sockets are not written through the filesystem). */
	if (ext4_is_ro(fs) &&
	    ((mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFDIR) &&
	    ((flags & (O_WRONLY | O_RDWR)) || (flags & O_TRUNC)))
		return ST_ROFS;

	/* O_TRUNC on an existing regular file: discard its data. */
	if ((flags & O_TRUNC) && (mode & S_IFMT) == S_IFREG &&
	    ext4_inode_size(&in) > 0 && !ext4_is_ro(fs)) {
		/* Cache first, blocks second -- same ordering rule as
		 * ext4_truncate_impl(), and for the same reason: a dirty page
		 * left pointing at a freed block is either unstorable for ever
		 * or written into whatever owns that block next. */
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_free_blocks_from(fs, ino, &in, 0);
		in.i_size_lo = 0;
		in.i_size_high = 0;
		ext4_stamp_now(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME);
		ext4_inode_cache_flush(fs);
		/* Superseded by the truncation -- see ext4_truncate_impl. */
		ext4_di_clear(fs, ino);
		ext4_write_inode_struct(fs, ino, &in);
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ino, 0));
	}

	/* Flush any deferred GDT/superblock updates from create/truncate
	 * alloc — but only on the exclusive (mutating-open) path; a plain
	 * shared open must not write metadata.  Deferred updates flush on
	 * the next mutator/fsync/commit anyway. */
	if (fs->meta_dirty && ext4_meta_held_excl(fs))
		ext4_flush_meta(fs);

	ext4_file_t *ef = (ext4_file_t *)kalloc(sizeof(ext4_file_t));
	if (!ef)
		return ST_NOMEM;
	mm_memset(ef, 0, sizeof(*ef));
	ef->fs = fs;
	ef->ino = ino;
	ef->mode = mode;
	ef->size = ext4_inode_size(&in);
	ef->pos = (flags & O_APPEND) ? ef->size : 0;
	ef->is_dir = ((mode & S_IFMT) == S_IFDIR);
	ef->vfs.ops = &ext4_vfs_ops;
	ef->vfs.fs_private = ef;

	if (!ef->is_dir && (mode & S_IFMT) == S_IFREG) {
		/* Attach an icache entry keyed by the encoded chain id so the
         * generic chain cache hangs off the same inode. */
		ef->inode = icache_get(&fs->sb, EXT4_BID_ENC(ino, 0), ef->size, mode, 0,
				       0, 0, 0, 0);
	}

	*out = &ef->vfs;
	return ST_OK;
}

int ext4_stat_fill(ext4_fs_t *fs, unsigned long ino, struct kstat *st)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	mm_memset(st, 0, sizeof(*st));
	st->st_dev = 0;
	st->st_ino = ino;
	st->st_mode = in.i_mode;
	st->st_nlink = in.i_links_count;
	st->st_uid = ext4_inode_uid(&in);
	st->st_gid = ext4_inode_gid(&in);
	st->st_size = ext4_inode_size(&in);
	if (S_ISCHR(in.i_mode) || S_ISBLK(in.i_mode)) {
		/* The device number, in either encoding (ext4_mknod_impl). */
		uint32_t w[2];
		mm_memcpy(w, in.i_block, sizeof(w));
		if (w[0])
			st->st_rdev = kdev_encode((w[0] >> 8) & 0xffu,
						  w[0] & 0xffu);
		else
			st->st_rdev = kdev_encode(
				(w[1] & 0xfff00u) >> 8,
				(w[1] & 0xffu) | ((w[1] >> 12) & 0xfff00u));
	}
	st->st_blksize = fs->block_size;
	st->st_blocks = in.i_blocks_lo; /* in 512-byte units */
	ext4_time_decode(in.i_atime, in.i_atime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_atime_extra)),
			 &st->st_atime, &st->st_atime_nsec);
	ext4_time_decode(in.i_mtime, in.i_mtime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_mtime_extra)),
			 &st->st_mtime, &st->st_mtime_nsec);
	ext4_time_decode(in.i_ctime, in.i_ctime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_ctime_extra)),
			 &st->st_ctime, &st->st_ctime_nsec);
	return ST_OK;
}

int ext4_stat_vfs_impl(ext4_fs_t *fs, const char *path, struct kstat *st)
{
	if (!st || !path || !fs)
		return ST_INVALID;
	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	if (rr != ST_OK) /* propagate ST_IO, not ENOENT */
		return rr;
	return ext4_stat_fill(fs, ino, st);
}

/* The file's current size.  Every open handle of a file shares one cached
 * inode, which every size change updates (write, truncate, an open that
 * truncates); the handle's own copy is only what the file had when the
 * handle was opened (or last changed through it), and another handle may
 * have grown or shortened the file since. */
static unsigned long ext4_file_size(ext4_file_t *ef)
{
	if (ef->inode)
		return __atomic_load_n(&((ic_inode_t *)ef->inode)->size,
				       __ATOMIC_RELAXED);
	return ef->size;
}

/* The page cache's page for page `idx' of the file, held, for a private
 * mapping to map itself (vfs_ops_t.cache_page).  Looked up -- and read in
 * on a miss -- under the same locks as a read (ext4_read_at_impl), after
 * the same write-behind flush the read entry points do (the caller,
 * ext4_cache_page_op).  NULL for a page not wholly inside the file: the
 * mapping then reads a copy, whose tail past the end is zeroed. */
pc_page_t *ext4_cache_page_impl(vfs_file_t *f, unsigned long idx)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	pc_page_t *pg = NULL;

	if (!ef || !ef->fs || ef->is_dir)
		return NULL;

	ext4_fs_t *fs = ef->fs;
	unsigned long ino = ef->ino;
	unsigned long chain_id = EXT4_BID_ENC(ino, 0);

	if (idx > (~0UL / PAGE_SIZE) - 1)
		return NULL;
	ext4_ilock_shared(fs, ino);
	ext4_meta_rlock(fs);
	unsigned long size = ext4_file_size(ef);

	if ((idx + 1) * PAGE_SIZE <= size)
		pg = pagecache_get_held(chain_id, idx, size, &fs->sb, chain_id);
	ext4_meta_runlock(fs);
	ext4_iunlock_shared(fs, ino);
	if (pg && !pg->data) {
		pagecache_put(pg);
		pg = NULL;
	}
	return pg;
}

/* Pages a read takes under the locks before it copies them out. */
#define EXT4_RBATCH 16

/* Read `bytes' at `pos' into `buf' (user memory, or a kernel page for
 * demand paging).  User memory is never touched with a filesystem lock held
 * (see ext4_rwsem_t): under the inode lock and the metadata lock, both
 * shared, up to EXT4_RBATCH pages are looked up / read in and HELD, the
 * locks are dropped, then the bytes are copied out of the held pages (a page
 * truncated meanwhile is detached but keeps its frame until put), and the
 * next batch follows.
 *
 * The metadata lock is held while the pages are looked up because filling
 * one maps the file's blocks (the inode, extent blocks), and the read-your-
 * writes lookups behind that need it at least shared: it keeps the
 * transaction and checkpoint sets stable while they are consulted and
 * serialises the metadata block cache against a writer (without it, a
 * reader could insert a stale copy of a block over the one a concurrent
 * checkpoint just refreshed).  The inode lock keeps the file's blocks from
 * being freed or rewritten under the transfer.
 *
 * Called with no lock held, at an offset the CALLER names: demand paging
 * has no descriptor position and must not touch one (a mapping's handle is
 * the caller's open file, and a read() or lseek() on it while a fault was in
 * flight would move it).  `ra' is the readahead state to use and update; a
 * positioned read passes its own so it cannot disturb the descriptor's
 * sequential-access tracking. */
long ext4_read_at_impl(vfs_file_t *f, void *buf, long bytes,
			      unsigned long pos, pc_readahead_t *ra)
{
	if (!f || !buf)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs || ef->is_dir)
		return ef && ef->is_dir ? -EISDIR : ST_INVALID;
	if (bytes < 0)
		return ST_INVALID;

	ext4_fs_t *fs = ef->fs;
	unsigned long ino = ef->ino;
	unsigned long chain_id = EXT4_BID_ENC(ino, 0);
	unsigned long copied = 0;
	long err = 0;

	while ((unsigned long)bytes > copied && !err) {
		pc_page_t *pages[EXT4_RBATCH];
		unsigned offs[EXT4_RBATCH];
		unsigned lens[EXT4_RBATCH];
		unsigned n = 0;
		unsigned long at = pos + copied;
		unsigned long want = (unsigned long)bytes - copied;
		unsigned long size;

		ext4_ilock_shared(fs, ino);
		ext4_meta_rlock(fs);
		size = ext4_file_size(ef);
		if (at < size && want > size - at)
			want = size - at;
		while (at < size && want && n < EXT4_RBATCH) {
			unsigned long page_idx = at / PAGE_SIZE;
			unsigned page_off = at % PAGE_SIZE;
			unsigned chunk = PAGE_SIZE - page_off;

			if (chunk > want)
				chunk = (unsigned)want;
			/* Held: reclaim cannot free it before the copy. */
			pc_page_t *pg = pagecache_get_held(
				chain_id, page_idx, size, &fs->sb, chain_id);
			if (!pg || !pg->data) {
				pagecache_put(pg);
				err = ST_IO;
				break;
			}
			pagecache_readahead(ra, chain_id, page_idx, size,
					    &fs->sb, chain_id);
			pages[n] = pg;
			offs[n] = page_off;
			lens[n] = chunk;
			n++;
			at += chunk;
			want -= chunk;
		}
		ext4_meta_runlock(fs);
		ext4_iunlock_shared(fs, ino);
		if (n == 0)
			break;
		/* No lock held: a fault here strands nothing. */
		for (unsigned k = 0; k < n; k++) {
			smap_disable();
			mm_memcpy((uint8_t *)buf + copied,
				  pages[k]->data + offs[k], lens[k]);
			smap_enable();
			copied += lens[k];
			pagecache_put(pages[k]);
		}
	}
	if (copied)
		return (long)copied;
	return err;
}

/* The descriptor's read: the same thing at the descriptor's own position,
 * advancing it and keeping its readahead state. */
long ext4_read_impl(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	pc_readahead_t ra;
	long r;

	if (!ef)
		return ST_INVALID;
	ra = ef->ra;
	r = ext4_read_at_impl(f, buf, bytes, ef->pos, &ra);
	if (r > 0) {
		ef->pos += (unsigned long)r;
		ef->ra = ra;
	}
	return r;
}

long ext4_seek_impl(vfs_file_t *f, long offset, int whence)
{
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef)
		return ST_INVALID;
	long base;
	switch (whence) {
	case 0:
		base = 0;
		break; /* SEEK_SET */
	case 1:
		base = (long)ef->pos;
		break; /* SEEK_CUR */
	case 2:
		base = (long)ext4_file_size(ef);
		break; /* SEEK_END */
	default:
		return ST_INVALID;
	}
	long np = base + offset;
	if (np < 0)
		return ST_INVALID;
	ef->pos = (unsigned long)np;
	return np;
}

/* The last close of an inode unlinked while open (IC_ORPHAN, see
 * ext4_unlink_impl): now nothing can reach it any more, so its blocks and
 * its number go back, exactly the sequence unlink runs for a file nobody
 * had open.  Under the I/O lock like every other metadata change; a
 * read-only mount leaves the orphan for fsck. */
static void ext4_orphan_reap(ext4_fs_t *fs, unsigned long ino)
{
	ext4_inode in;

	if (!fs || ino == 0 || ext4_is_ro(fs))
		return;
	ext4_io_lock(fs);
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK &&
	    in.i_links_count == 0 && in.i_dtime == 0) {
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_free_blocks_from(fs, ino, &in, 0);
		in.i_dtime = (uint32_t)timer_get_epoch();
		in.i_size_lo = 0;
		in.i_size_high = 0;
		ext4_di_clear(fs, ino);
		ext4_write_inode_struct(fs, ino, &in);
		ext4_free_inode(fs, ino, 0);
		icache_remove(&fs->sb, EXT4_BID_ENC(ino, 0));
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_inode_cache_flush(fs);
		ext4_flush_meta(fs);
	}
	ext4_io_unlock(fs);
}

int ext4_close_impl(vfs_file_t *f)
{
	if (!f)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (ef) {
		ext4_fs_t *fs = ef->fs;
		unsigned long ino = ef->ino;
		int reap = 0;
		if (ef->inode)
			reap = icache_unref_flagged((ic_inode_t *)ef->inode,
						    IC_ORPHAN);
		kfree(ef);
		if (reap)
			ext4_orphan_reap(fs, ino);
	}
	return ST_OK;
}

int ext4_chdir_impl(ext4_fs_t *fs, const char *path)
{
	if (!path || !fs)
		return ST_INVALID;
	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	if (rr != ST_OK) /* propagate ST_IO, not ENOENT */
		return rr;
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in) ||
	    (in.i_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;
	fs->st->cwd_ino = ino;
	return ST_OK;
}

/*
 * Write zeros over [from, to) of a file, through the page cache.
 *
 * Allocation hands out blocks without clearing them, so a block that has just
 * become part of a file still holds what its previous owner put there.  Any
 * part of such a block that the write itself does not cover has to be cleared,
 * or reading it returns somebody else's data -- not merely wrong bytes, but
 * the contents of a file that was deleted.
 *
 * Doing it through the cache rather than the device means a read sees the
 * zeros at once instead of after a writeback, and a whole page of them costs
 * no device read on the way in.
 *
 * `file_size` is the size the file had BEFORE this write, which is what
 * decides whether a page is past the end and can be created empty.
 *
 * ONLY blocks the file actually has are written.  A range with no block
 * behind it is skipped, and that is not a shortcut -- it is the whole point:
 * an unmapped block already reads as zeros, so there is nothing stale in it to
 * cover.  Dirtying a page over one would also be actively wrong, because
 * writeback would then have nowhere to put it, would leave it dirty and
 * unreclaimable for good, and would say so once per page.
 *
 * Returns 0, or 1 if a page could not be obtained.
 */
int ext4_zero_range(ext4_fs_t *fs, unsigned long chain_id,
			   unsigned long file_size, unsigned long from,
			   unsigned long to)
{
	unsigned long ino = EXT4_BID_INO(chain_id);
	unsigned long bs = fs->block_size;
	unsigned long done = 0;

	might_sleep();
	if (bs == 0)
		return 1;
	while (from < to) {
		/* One block at a time, because whether it needs clearing is a
		 * per-block question: a page can span several of them, and a
		 * file can have one mapped next to one that is not. */
		unsigned long lb = from / bs;
		unsigned long blk_end = (lb + 1) * bs;
		unsigned long seg_end = (to < blk_end) ? to : blk_end;

		unsigned long pbn;

		if (ext4_map_blk(fs, ino, lb, &pbn) != ST_OK)
			return 1; /* a damaged map (reported) */
		if (pbn == 0) {
			from = seg_end; /* a hole: already reads as zeros */
			continue;
		}

		while (from < seg_end) {
			unsigned long page_idx = from / PAGE_SIZE;
			unsigned page_off = (unsigned)(from % PAGE_SIZE);
			unsigned avail = PAGE_SIZE - page_off;
			unsigned chunk = (seg_end - from < avail) ?
						 (unsigned)(seg_end - from) :
						 avail;
			int full_page = (page_off == 0 && chunk == PAGE_SIZE);
			pc_page_t *pg = pagecache_get_for_write(
				chain_id, page_idx, file_size, &fs->sb,
				chain_id, full_page);

			if (!pg || !pg->data)
				return 1;
			BUG_ON(page_off + chunk > PAGE_SIZE);
			mm_memset(pg->data + page_off, 0, chunk);
			ext4_ordered_add(fs, ino);
			pagecache_write_end(pg);
			from += chunk;

			/* A hole can be far larger than the write that created
			 * it -- a seek to a gigabyte followed by one byte is a
			 * gigabyte of it -- and every page of it is dirty and
			 * therefore unreclaimable until stored.  Give the
			 * writeback a turn on the way through rather than
			 * filling memory first. */
			if (++done >= PC_DIRTY_WRITEBACK_BATCH) {
				done = 0;
				pagecache_balance_dirty(&fs->sb);
			}
		}
	}
	return 0;
}

/* The write itself, at *posp -- the description's position for write(), a
 * position of the call's own for pwrite() -- or, with `append', at the end
 * of the file; *posp is moved past what it wrote.  `buf' is kernel memory
 * (ext4_write_common copied it in).  Called with the inode lock exclusive
 * and the I/O lock. */
long ext4_write_impl_at(vfs_file_t *f, const void *buf, long bytes,
			       unsigned long *posp, int append)
{
	if (!f || !buf)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	if (ef->is_dir)
		return -EISDIR;
	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	ext4_fs_t *fs = ef->fs;
	if (fs->read_only)
		return ST_ROFS; /* error latch / read-only mount */

	ext4_inode in;
	if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK)
		return ST_IO;

	unsigned long cur_size = ext4_inode_size(&in);
	if (append)
		*posp = cur_size;
	unsigned long wpos = *posp;
	unsigned long end = wpos + (unsigned long)bytes;
	/* Whether this call gave the file new blocks.  Extents have to be on the
	 * device before the data they describe, so that case still stores the
	 * inode; a write within blocks the file already owns does not. */
	int allocated_blocks = 0;
	/* Whether the first / last block this write touches had to be created
	 * for it.  A block that did not exist a moment ago holds whatever its
	 * previous owner left, so the parts of it this write does NOT cover
	 * have to be cleared -- see the zeroing below. */
	int head_created = 0, tail_created = 0;

	/* A write past the end of the file is given blocks for the range it
	 * covers and nothing else: the stretch between the old end and the
	 * write stays a hole, which reads as zeros without owning a block.
	 *
	 * This used to allocate every block from the old end up to the new
	 * one.  A small write at a large offset -- a cache or database file
	 * written sparsely -- then owned the whole gap, and since a block that
	 * is mapped must not show its previous owner's data, the hole clearing
	 * below wrote zeros over all of it: gigabytes, synchronously, through
	 * the page cache to the device, inside a single write() and with the
	 * filesystem's metadata lock held the whole time.  Every other process
	 * that needed a page from this filesystem waited behind it.
	 *
	 * The loop below creates exactly the blocks the write lands on, inside
	 * the file or past its end alike. */

	/* The blocks the write lands on that the file does not have -- past
	 * its end, or in a hole inside it (a file extended by ftruncate has a
	 * size and no blocks at all; an earlier seek-and-write leaves a hole
	 * behind it).  The range this write covers is checked block by block,
	 * and whatever is missing is created.  Only that range -- the rest of
	 * the file stays sparse, which is what it is for.
	 */
	{
		unsigned long bs = fs->block_size;
		unsigned long first_lb = wpos / bs;
		unsigned long last_lb =
			(end > wpos) ? (end - 1) / bs : first_lb;
		unsigned long lb = first_lb;

		/* ext4_block_map reads the inode through the parsed cache, so
		 * an allocation it has not been told about still looks like a
		 * hole -- and this loop would then try to fill the same gap
		 * again, mapping two extents over one logical block.  Tell it
		 * first, and again after every allocation below. */
		ext4_inode_cache_update(fs, ef->ino, &in);

		while (lb <= last_lb) {
			unsigned long run_start, run = 0, pbn;
			int mrc;

			/* A block whose map is damaged is not a hole to fill:
			 * the write fails (the damage was reported). */
			if ((mrc = ext4_map_blk(fs, ef->ino, lb, &pbn)) !=
			    ST_OK) {
				ext4_flush_meta(fs);
				return mrc;
			}
			if (pbn != 0) {
				lb++;
				continue;
			}
			run_start = lb;
			while (lb <= last_lb) {
				if ((mrc = ext4_map_blk(fs, ef->ino, lb,
							&pbn)) != ST_OK) {
					ext4_flush_meta(fs);
					return mrc;
				}
				if (pbn != 0)
					break;
				run++;
				lb++;
			}

			unsigned got = ext4_alloc_blocks_for_file(
				fs, ef->ino, &in, run_start, (unsigned)run);

			if (got == 0) {
				ext4_flush_meta(fs);
				return -ENOSPC;
			}
			allocated_blocks = 1;
			if (run_start == first_lb)
				head_created = 1;
			if (run_start + got > last_lb)
				tail_created = 1;
			ext4_inode_cache_update(fs, ef->ino, &in);

			if (got < run) {
				/* Ran out part way through the gap: the write
				 * can only reach as far as what was created. */
				unsigned long max_end = (run_start + got) * bs;

				if (max_end <= wpos) {
					ext4_flush_meta(fs);
					return -ENOSPC;
				}
				end = max_end;
				bytes = (long)(end - wpos);
				break;
			}
		}
	}

	/* Persist the inode ONCE: new extents (if any) + final size + mtime, so
     * the data loop's block_map sees the extents and we avoid a second write. */
	unsigned long newsize = (end > cur_size) ? end : cur_size;
	in.i_size_lo = (uint32_t)newsize;
	if ((in.i_mode & S_IFMT) == S_IFREG)
		in.i_size_high = (uint32_t)(newsize >> 32);
	{
		uint64_t now = timer_get_epoch();
		ext4_stamp(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME, (int64_t)now, 0);
	}
	/* The in-core copy is the current one from here on, and the block
	 * mapping that writeback performs reads through it -- so it is updated
	 * rather than dropped.  Emptying the whole cache, which is what this
	 * did, meant re-reading from the device the very inode the next line of
	 * this function needs. */
	ext4_inode_cache_update(fs, ef->ino, &in);

	/* The inode reaches the device now only if it has to.
	 *
	 * It has to when this write ALLOCATED, because the block map is part
	 * of the inode: a deferred copy of that would leave the filesystem not
	 * knowing it owns the blocks the data is about to go into, and the
	 * writeback that follows would have nowhere to put the pages.
	 *
	 * A write within blocks the file already owns changes only the size
	 * and the timestamps, and those are recorded instead -- one small
	 * memory update in place of a read-modify-write of a 4KB inode-table
	 * block through the journal, per write() call.  A program writing a
	 * file in 700-byte pieces crosses a block boundary once every six of
	 * them, so five out of six stop paying for it.
	 *
	 * What makes that safe is ext4_di_apply: every route to an inode puts
	 * the pending change back before handing it over, so open(), stat()
	 * and the writeback path all see the size the file actually has, not
	 * the one the device last heard about.  If there is no room to record
	 * it, it is stored the old way -- slower, never wrong.
	 *
	 * The record is made even when the inode is also stored below, and
	 * that is not redundant.  ext4_sb_write_inode -- the generic sync path
	 * -- takes the size from the per-open-handle copy in the generic inode
	 * cache UNLESS ext4_di_pending(fs) says this path has something better,
	 * and that copy is exactly the one its own comment warns "two handles
	 * on the same file can move backwards between them".  Recording only
	 * in the non-allocating case left the guard false for every write that
	 * GREW a file: a sync landing between the allocation and the next
	 * update of that handle wrote the shorter length back over the one
	 * this write computed.  The file then claims fewer blocks than it
	 * owns, and since ext4_sb_next_block derives the chain length from the
	 * size, the pages past the reverted end have nothing to map to --
	 * writeback finds no block, warns ("dirty page ... has no block behind
	 * it"), leaves them dirty and therefore unreclaimable, and the tail of
	 * the file never reaches the device. */
	int size_recorded =
		ext4_di_record(fs, ef->ino, newsize, in.i_mtime, in.i_ctime);
	if (allocated_blocks || !size_recorded) {
		ext4_write_inode_struct(fs, ef->ino, &in);
		/* Publish the copy that actually reached the device, not the
		 * one on the way there: ext4_write_inode_struct may adjust the
		 * inode as it stores it, and a cached copy missing those
		 * adjustments would hand them back to the next writer to store
		 * again. */
		ext4_inode_cache_update(fs, ef->ino, &in);
	}

	/* Write the data.  When journalling is on, full blocks are accumulated into
     * the persistent write-back buffer (st->wbounce) and flushed to disk as ONE big
     * device transfer (ext4_wb_flush) — coalescing many small write()s into a few
     * large USB commands, which is most of the sequential-write throughput.  The
     * buffer OUTLIVES the write() (that is the point); it is flushed when it fills,
     * on a non-contiguous write, before the journal commits the referencing
     * metadata (data=ordered, see ext4_journal_flush), and before any read/free of
     * these blocks.  Partial edge blocks are read-modify-written directly (after
     * flushing pending data so the on-disk block is current).  With NO journal,
     * data is written directly as before — no deferral. */
	unsigned long write_start = wpos;
	unsigned long pos = wpos, remaining = (unsigned long)bytes,
		      written = 0;
	unsigned long chain_id = EXT4_BID_ENC(ef->ino, 0);
	const uint8_t *src = (const uint8_t *)buf;
	int io_err = 0;
	/* The data goes into the page cache, not to the device.
	 *
	 * write() owes the caller that a later read sees these bytes.  It does
	 * not owe them a disk that has already been touched.  Putting them in
	 * the cache and marking the page dirty satisfies that at once, and
	 * leaves the device work to be done later, in large pieces, by
	 * writeback or by an explicit fsync.
	 *
	 * What this replaces went to the device inside write() itself, and had
	 * to: a write covering part of a block became a read of that block, a
	 * merge, and a write back, synchronously, every time.  A program
	 * writing a file in small pieces paid a device round trip per piece,
	 * and repeated writes to the same block paid it repeatedly.  Here the
	 * second and later writes to a page are memory copies.
	 *
	 * It also settles an inconsistency: reads have always come from this
	 * cache while writes went past it, so a file could be read back as its
	 * old contents until something dropped the page. */

	/* A write that starts past the end of the file leaves a gap behind it,
	 * and the gap must read as zeros.
	 *
	 * Most of it does by itself: the blocks between the old end and the
	 * write are not allocated (see above), and a hole reads as zeros.  What
	 * does not is the part of the old last block beyond the old end of
	 * file -- that block is mapped, and its tail holds whatever was there
	 * before (a previous owner's data, or bytes a truncate cut off).  So
	 * the gap is cleared through the same cache the data goes through,
	 * and ext4_zero_range skips every block the file does not have: only
	 * that tail, at most one block, is written.
	 *
	 * An ordinary append starts at exactly the old end of file, so there
	 * is no gap and this does nothing at all. */
	if (write_start > cur_size) {
		unsigned long hend = (write_start < end) ? write_start : end;

		if (ext4_zero_range(fs, chain_id, cur_size, cur_size, hend))
			io_err = 1;
	}

	/* The same argument for a block created INSIDE the file.
	 *
	 * Filling a gap gives the file blocks that are not new to the device,
	 * and this write covers only its own range of them.  Every block
	 * between the first and the last is covered end to end; the two at the
	 * edges can stick out, and what sticks out is still the previous
	 * owner's data.  Before this it read as zeros, because an unmapped
	 * block does, and it has to go on reading as zeros now that it is
	 * mapped. */
	if (!io_err && head_created) {
		unsigned long bs = fs->block_size;
		unsigned long head_from = (write_start / bs) * bs;

		if (head_from < write_start &&
		    ext4_zero_range(fs, chain_id, cur_size, head_from,
				    write_start))
			io_err = 1;
	}
	if (!io_err && tail_created && end > write_start) {
		unsigned long bs = fs->block_size;
		unsigned long tail_to = ((end - 1) / bs + 1) * bs;

		if (tail_to > end &&
		    ext4_zero_range(fs, chain_id, cur_size, end, tail_to))
			io_err = 1;
	}

	while (remaining && !io_err) {
		unsigned long page_idx = pos / PAGE_SIZE;
		unsigned page_off = (unsigned)(pos % PAGE_SIZE);
		unsigned avail = PAGE_SIZE - page_off;
		unsigned chunk = (remaining < avail) ? (unsigned)remaining :
						       avail;
		/* Replacing the page entirely means its previous contents need
		 * not be fetched first. */
		int full_page = (page_off == 0 && chunk == PAGE_SIZE);
		pc_page_t *pg;

		/* Every block behind this page must already exist.
		 *
		 * The old loop checked this before writing, because it wrote to
		 * the block directly and needed its address.  Caching the write
		 * removes the need for the address here -- but not the need for
		 * the check.  A page with no block behind it cannot be written
		 * back at all: writeback would find nothing to map it to, leave
		 * it dirty, and move on.  The data would be lost with no error
		 * ever reported, the page would never become reclaimable, and
		 * enough of them would exhaust memory.  Refuse the write
		 * instead, exactly as before. */
		{
			unsigned long bs = fs->block_size;
			unsigned long first = ((unsigned long)page_idx *
					       PAGE_SIZE) /
					      bs;
			unsigned long nblk = (PAGE_SIZE + bs - 1) / bs;

			if (nblk == 0)
				nblk = 1;
			for (unsigned long b = 0; b < nblk; b++) {
				unsigned long byte_off =
					((unsigned long)page_idx * PAGE_SIZE) +
					b * bs;

				/* Only blocks this write actually covers. */
				if (byte_off >= pos + chunk)
					break;
				if (byte_off + bs <= pos)
					continue;
				if (ext4_block_map(fs, ef->ino, first + b) ==
				    0) {
					io_err = 1;
					break;
				}
			}
			if (io_err)
				break;
		}

		pg = pagecache_get_for_write(chain_id, page_idx, cur_size,
					     &fs->sb, chain_id, full_page);
		if (!pg || !pg->data) {
			io_err = 1;
			break;
		}
		BUG_ON(page_off + chunk > PAGE_SIZE);
		/* Two threads writing the same page do not race here, and it
		 * is worth saying why rather than leaving it to be worked out
		 * again: ext4_write takes the per-inode lock EXCLUSIVELY, so
		 * only one write() to a given file is ever inside this loop.
		 * Two threads writing the same page of the same file are
		 * serialised by that lock; two writing different files never
		 * share a page, because a page belongs to one inode.
		 *
		 * What the lock does NOT cover is reclaim and writeback on
		 * another processor, which is what the page is held for --
		 * see pagecache_get_for_write. */
		mm_memcpy(pg->data + page_off, src + written, chunk);
		/* This transaction now owes an ordered store for this file. */
		ext4_ordered_add(fs, ef->ino);
		/* Publish and release together.  The page was held from the
		 * moment it was handed over: until the copy is finished it is
		 * an ordinary clean page that reclaim on another processor
		 * would be entitled to take, and the copy would then land in
		 * somebody else's memory. */
		pagecache_write_end(pg);

		pos += chunk;
		written += chunk;
		remaining -= chunk;
	}

	if (written == 0 && io_err)
		return ST_IO;

	wpos += written;
	*posp = wpos;
	/* The file's size, as every handle of it sees it: the larger of what
	 * it was when this write took the lock and where the write ended --
	 * never this handle's own idea of it, which another handle may have
	 * outgrown meanwhile. */
	ef->size = (wpos > cur_size) ? wpos : cur_size;
	if (ef->inode)
		icache_update_size((ic_inode_t *)ef->inode, ef->size);
	/* The cached pages are NOT dropped here.
	 *
	 * They hold what was just written, and until writeback runs they are
	 * the only copy of it -- discarding them discards the write.  That is
	 * exactly what the line this replaces did once the data stopped going
	 * straight to the device: an append or an overwrite reported success
	 * and then read back as the file's previous contents, while a write
	 * starting at offset zero of an empty file survived, because the
	 * old-page test cannot be true for it.
	 *
	 * Dropping them was right while a write went past the cache and left it
	 * holding a stale copy.  The cache is now where the data is, and there
	 * is nothing else to tell.
	 *
	 * The block-chain cache still needs telling: the allocation above may
	 * have given the file new blocks, so the map from file offset to disk
	 * block has changed, and writeback is about to use it. */
	(void)write_start;
	icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ef->ino, 0));

	/* Having produced dirty pages, take a turn at storing them if too many
	 * are waiting.  Charged to the writer deliberately: it is what makes a
	 * program writing faster than the device can absorb slow down, instead
	 * of the machine filling with data it is not allowed to discard. */
	pagecache_balance_dirty(&fs->sb);
	ext4_flush_meta(fs); /* batch the deferred GDT/superblock writeback */
	return (long)written;
}
