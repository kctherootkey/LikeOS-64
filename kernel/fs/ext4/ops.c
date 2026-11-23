// LikeOS - ext4 filesystem driver: the VFS entry points (locking per the hierarchy)
// and the operations table
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

static int ext4_fsync(vfs_file_t *f)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	/* The journal was aborted by an error: what this file has in memory
	 * can no longer be made durable. */
	if (ext4_journal_aborted(fs))
		return ST_IO;
	ext4_io_lock(fs);
	if (!ef->is_dir) {
		pagecache_flush_file(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
		if (ef->inode)
			icache_flush((ic_inode_t *)ef->inode);
	}
	/* fsync means "this file is on the device when I return", and a size
	 * held only in memory is part of what that promises.  Everything
	 * pending is stored, not just this file's: the set is small, and one
	 * pass is cheaper than working out which of them matters. */
	ext4_di_flush(fs);
	ext4_flush_meta(fs);
	/* Merge this op into the batch, then durably commit + clean the journal: an
     * explicit sync is a consistency point, so a reboot after it must not replay.
     * journal_clean flushes the batch (one log commit + checkpoint) and syncs. */
	ext4_txn_flush(fs);
	/* A batch that could not be committed is not on the device. */
	int jerr = ext4_journal_clean(fs);
	/* Write-back defers data-write I/O errors past write(); surface them here. */
	int werr = fs->st->wb_err;
	fs->st->wb_err = 0;
	ext4_io_unlock(fs);
	return (werr || jerr != ST_OK) ? -EIO : 0;
}

/* Whole-filesystem sync (the sync(2) op, not tied to a file): flush deferred
 * metadata, commit any in-flight transaction, push the device, then mark the
 * journal clean so a reboot right after sync() does not replay.  Mirrors
 * ext4_fsync minus the per-file pagecache/icache flush. */
int ext4_sync_fs(ext4_fs_t *fs)
{
	if (!fs)
		return 0;
	/* Mounted without replaying its journal (read-only): there is
	 * nothing of ours to store, and storing the superblock would drop
	 * the flag that says the journal still needs replaying. */
	if (fs->mnt_opts & EXT4_MNT_NOLOAD)
		return 0;
	/* Aborted by an error: nothing more is committed.  The error
	 * details recorded meanwhile are stored all the same. */
	if (ext4_journal_aborted(fs)) {
		ext4_store_error_info(fs);
		return ST_IO;
	}
	ext4_io_lock(fs);
	ext4_resync_gd_from_bitmaps(
		fs); /* GD free counts/csums <- bitmaps (ground truth) */
	/* Sizes and timestamps held in memory are part of what sync() is being
	 * asked to make durable. */
	ext4_di_flush(fs);
	ext4_flush_meta(fs);
	ext4_txn_flush(fs);
	int jerr = ext4_journal_clean(fs);
	ext4_io_unlock(fs);
	return jerr;
}

static int ext4_sync_op(vfs_mount_t *m)
{
	return ext4_sync_fs(ext4_fs_of(m));
}

/* ---- Locked wrappers (serialise via the reentrant sleeping mutex) ---- */
/* ===================================================================
 * VFS entry points — the locking happens HERE, per the hierarchy
 * (per-inode → metadata → block-group).  Read-only ops take the metadata
 * lock SHARED and run concurrently; mutators take it EXCLUSIVE (the old
 * global-mutex semantics, transaction hooks included).  File-content ops
 * additionally take the file's inode lock so data reads (which transfer
 * with NO metadata lock) can never overlap a free/rewrite of the same
 * file's blocks.  Every lock is the instance's own: two mounted ext4
 * filesystems never wait for each other.
 * =================================================================== */

/* Fence helper for path-named ops that FREE file data (unlink, rename
 * over an existing file, O_TRUNC open): resolve the victim under the
 * shared lock, take its inode lock exclusive, then re-check the name
 * still maps to the same inode under the exclusive metadata lock (it may
 * have been renamed/replaced while unlocked).  Returns 1 with BOTH locks
 * held (*ino_out set) when fenced, 0 with NO locks held when the path
 * does not currently resolve (or keeps racing — caller falls back to the
 * plain exclusive path, where the data-free is safe anyway because the
 * name no longer reaches a file a reader could have mapped). */
static int ext4_fence_path_excl(ext4_fs_t *fs, const char *path, int follow,
				unsigned long *ino_out)
{
	if (!fs)
		return 0;
	for (int tries = 0; tries < 4; tries++) {
		unsigned long ino = 0;
		ext4_meta_rlock(fs);
		int rr = ext4_resolve_ex(fs, fs->st->cwd_ino, path, follow,
					 &ino, 0);
		ext4_meta_runlock(fs);
		if (rr != ST_OK || ino == 0)
			return 0;
		ext4_ilock_excl(fs, ino);
		ext4_io_lock(fs);
		unsigned long ino2 = 0;
		int rr2 = ext4_resolve_ex(fs, fs->st->cwd_ino, path, follow,
					  &ino2, 0);
		if (rr2 == ST_OK && ino2 == ino) {
			*ino_out = ino;
			return 1; /* both locks held */
		}
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, ino);
		/* raced with a rename/unlink of the name — retry */
	}
	return 0;
}

static int ext4_open_locked(ext4_fs_t *fs, const char *path, int flags,
			    unsigned cmode, vfs_file_t **out)
{
	if (!fs)
		return ST_INVALID;
	if (flags & (O_CREAT | O_TRUNC)) {
		/* A truncating open frees the file's data blocks: fence
		 * in-flight readers via the inode lock when the target
		 * already exists. */
		if (flags & O_TRUNC) {
			unsigned long ino = 0;
			if (ext4_fence_path_excl(fs, path, 1, &ino)) {
				int r = ext4_open_impl(fs, path, flags, cmode,
						       out);
				ext4_io_unlock(fs);
				ext4_iunlock_excl(fs, ino);
				return r;
			}
		}
		ext4_io_lock(fs);
		int r = ext4_open_impl(fs, path, flags, cmode, out);
		ext4_io_unlock(fs);
		return r;
	}
	ext4_meta_rlock(fs);
	int r = ext4_open_impl(fs, path, flags, cmode, out);
	ext4_meta_runlock(fs);
	return r;
}

/* ->open: no caller-supplied mode.  0666 is the POSIX default for creat(). */
static int ext4_open(vfs_mount_t *m, const char *path, int flags,
		     vfs_file_t **out)
{
	return ext4_open_locked(ext4_fs_of(m), path, flags, 0666, out);
}

/* ->open_mode: `mode` is already umask-adjusted by the syscall layer. */
static int ext4_open_mode(vfs_mount_t *m, const char *path, int flags,
			  unsigned int mode, vfs_file_t **out)
{
	return ext4_open_locked(ext4_fs_of(m), path, flags, mode, out);
}

static int ext4_stat_vfs(vfs_mount_t *m, const char *path, struct kstat *st)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	int r = ext4_stat_vfs_impl(fs, path, st);
	ext4_meta_runlock(fs);
	return r;
}
/* Read-your-writes for the data write-back buffer: if it holds `ino's
 * blocks, flush them under the exclusive lock before the read.  It cannot
 * refill for this inode during the read's batches: a writer needs the inode
 * lock exclusive, and every batch holds it shared. */
static void ext4_read_wb_flush(ext4_fs_t *fs, unsigned long ino)
{
	ext4_meta_rlock(fs);
	int wb_hit = (fs->st->wb_len && fs->st->wb_ino == ino);
	ext4_meta_runlock(fs);
	if (wb_hit) {
		ext4_io_lock(fs);
		ext4_wb_flush(fs);
		ext4_io_unlock(fs);
	}
}

/* read(): the locks are taken batch by batch inside (ext4_read_at_impl),
 * never across the copy to the caller's buffer. */
static long ext4_read(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_read_wb_flush(ef->fs, ef->ino);
	return ext4_read_impl(f, buf, bytes);
}
/* Read at an explicit offset, leaving the descriptor's position alone.
 *
 * Same locking as ext4_read above, with the position and the readahead
 * state supplied by the caller instead of taken from the handle. */
static long ext4_read_at(vfs_file_t *f, void *buf, long bytes, long off)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	pc_readahead_t ra;

	if (!ef || !ef->fs || off < 0)
		return ST_INVALID;
	ra.last_page_index = 0;
	ra.sequential_count = 0;
	ra.ra_pages = 0;
	ext4_read_wb_flush(ef->fs, ef->ino);
	return ext4_read_at_impl(f, buf, bytes, (unsigned long)off, &ra);
}

/* Largest piece of a write copied in at a time (see ext4_write_common). */
#define EXT4_WCHUNK (256UL * 1024)

/* A write of `bytes' from `buf' (user memory) at *posp, or at the end of
 * the file with `append'.  User memory is never touched with a filesystem
 * lock held (see ext4_rwsem_t): the data is copied into a kernel buffer
 * first, then written under the inode lock and the metadata lock, both
 * exclusive -- a piece of up to EXT4_WCHUNK bytes at a time, each one an
 * operation of its own.  A write that fits in one piece is still atomic
 * against every other write and read of the file (an O_APPEND write of
 * it lands whole at the end); a larger one is so piece by piece. */
static long ext4_write_common(vfs_file_t *f, const void *buf, long bytes,
			      unsigned long *posp, int append)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs || !buf)
		return ST_INVALID;
	if (bytes < 0)
		return ST_INVALID;
	if (ef->is_dir)
		return -EISDIR;
	if (bytes == 0)
		return 0;
	ext4_fs_t *fs = ef->fs;
	unsigned long cap, done = 0;
	uint8_t *kbuf;
	int pooled = 0;
	long r = 0;

	/* A piece that fits a block comes from the recycled block buffers --
	 * small writes are the common case and need no allocation. */
	if ((unsigned long)bytes <= fs->block_size) {
		cap = fs->block_size;
		kbuf = (uint8_t *)ext4_bget(fs);
		pooled = 1;
	} else {
		cap = (unsigned long)bytes < EXT4_WCHUNK ?
			      (unsigned long)bytes :
			      EXT4_WCHUNK;
		kbuf = (uint8_t *)kalloc(cap);
		if (!kbuf) {
			cap = fs->block_size;
			kbuf = (uint8_t *)ext4_bget(fs);
			pooled = 1;
		}
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
		ext4_ilock_excl(fs, ef->ino);
		ext4_io_lock(fs);
		r = ext4_write_impl_at(f, kbuf, (long)n, posp, append);
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, ef->ino);
		if (r <= 0)
			break;
		done += (unsigned long)r;
		if ((unsigned long)r < n)
			break; /* short: the filesystem is full */
	}
	if (pooled)
		ext4_bput(fs, kbuf);
	else
		kfree(kbuf);
	if (done)
		return (long)done;
	return r;
}

/* write(): at the descriptor's position -- or, on a descriptor in append
 * mode (O_APPEND, which fcntl may switch at any time), at the end of the
 * file as it is when the write takes the lock, whoever made it that long. */
static long ext4_write(vfs_file_t *f, const void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	return ext4_write_common(f, buf, bytes, &ef->pos,
				 (f->flags & O_APPEND) != 0);
}
/* pwrite: the same at a position of the call's own; the description's is
 * neither read nor moved (ext4_seek touches it without any lock, so it may
 * not even be borrowed). */
static long ext4_write_at(vfs_file_t *f, const void *buf, long bytes, long off)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	unsigned long pos;

	if (!ef || !ef->fs || off < 0)
		return ST_INVALID;
	pos = (unsigned long)off;
	return ext4_write_common(f, buf, bytes, &pos, 0);
}
static long ext4_seek(vfs_file_t *f, long offset, int whence)
{
	/* Touches only per-open-file fields (pos; size is a single-word
	 * read) — no filesystem lock needed. */
	return ext4_seek_impl(f, offset, whence);
}
/* getdents: the entries are assembled in a kernel buffer under the
 * metadata lock (shared) and copied to the caller's buffer after it is
 * dropped. */
#define EXT4_DIRBUF_MAX (64UL * 1024)
static long ext4_readdir(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs || !buf || bytes <= 0)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	unsigned long cap = (unsigned long)bytes < EXT4_DIRBUF_MAX ?
				    (unsigned long)bytes :
				    EXT4_DIRBUF_MAX;
	uint8_t *kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf)
		return ST_NOMEM;
	ext4_meta_rlock(fs);
	long r = ext4_readdir_impl(f, kbuf, (long)cap);
	ext4_meta_runlock(fs);
	if (r > 0) {
		/* No lock held while user memory is written. */
		smap_disable();
		mm_memcpy(buf, kbuf, (unsigned long)r);
		smap_enable();
	}
	kfree(kbuf);
	return r;
}
static int ext4_truncate(vfs_file_t *f, unsigned long size)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_ilock_excl(fs, ef->ino);
	ext4_io_lock(fs);
	int r = ext4_truncate_impl(f, size);
	ext4_io_unlock(fs);
	ext4_iunlock_excl(fs, ef->ino);
	return r;
}
static int ext4_unlink(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	unsigned long ino = 0;
	if (!fs)
		return ST_INVALID;
	if (ext4_fence_path_excl(fs, path, 0, &ino)) {
		int r = ext4_unlink_impl(fs, path);
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, ino);
		return r;
	}
	ext4_io_lock(fs);
	int r = ext4_unlink_impl(fs, path);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_rename(vfs_mount_t *m, const char *o, const char *n)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	/* Only an existing DESTINATION file has its data freed (overwrite);
	 * fence it.  The source inode's data is untouched by rename. */
	unsigned long dst = 0;
	if (ext4_fence_path_excl(fs, n, 0, &dst)) {
		int r = ext4_rename_impl(fs, o, n);
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, dst);
		return r;
	}
	ext4_io_lock(fs);
	int r = ext4_rename_impl(fs, o, n);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_mkdir(vfs_mount_t *m, const char *path, unsigned int mode)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_mkdir_impl(fs, path, mode);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_mknod(vfs_mount_t *m, const char *path, unsigned int mode,
		      uint64_t dev)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_mknod_impl(fs, path, mode, dev);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_rmdir(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_rmdir_impl(fs, path);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_chdir(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	int r = ext4_chdir_impl(fs, path);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_close(vfs_file_t *f)
{
	/* icache_unref locks internally; nothing else is shared. */
	return ext4_close_impl(f);
}

static int ext4_release_locks_for_task(vfs_mount_t *m, uint64_t task_id)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !fs->st)
		return 0;
	int released = ext4_io_release_if_owner(fs, task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task) {
		released |= fs->bdev->release_locks_for_task(
			(block_device_t *)fs->bdev, task_id);
	}
	return released;
}

/* ---- Path ops that change an inode's attributes ---- */
static int ext4_symlink_op(vfs_mount_t *m, const char *target,
			   const char *linkpath)
{
	return ext4_symlink(ext4_fs_of(m), target, linkpath);
}
static int ext4_readlink_op(vfs_mount_t *m, const char *path, char *buf,
			    unsigned long bufsz)
{
	return ext4_readlink(ext4_fs_of(m), path, buf, bufsz);
}
static int ext4_link_op(vfs_mount_t *m, const char *oldpath,
			const char *newpath, int follow)
{
	return ext4_link(ext4_fs_of(m), oldpath, newpath, follow);
}
static int ext4_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	return ext4_chmod(ext4_fs_of(m), path, mode);
}
static int ext4_chown_op(vfs_mount_t *m, const char *path, int uid, int gid,
			 int nofollow)
{
	return ext4_chown(ext4_fs_of(m), path, uid, gid, nofollow);
}
static int ext4_lstat_op(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return ext4_lstat(ext4_fs_of(m), path, st);
}
static int ext4_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
			   const struct vfs_utimes *t)
{
	return ext4_utimens_impl(ext4_fs_of(m), path, nofollow, t);
}
static int ext4_futimens_op(vfs_file_t *f, const struct vfs_utimes *t)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !t)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	ext4_io_lock(fs);
	int r = ext4_utimens_ino(fs, ef->ino, t);
	ext4_io_unlock(fs);
	return r;
}

/* vfs_ops adapters: address fchmod/fchown by open handle and fill the generic
 * statfs struct, so the syscall layer never calls an ext4_* symbol directly. */
static int ext4_fchmod_op(vfs_file_t *f, unsigned int mode)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !ef->ino)
		return ST_OK; /* not an ext4 handle: no-op */
	return ext4_fchmod_ino(ef->fs, ef->ino, mode);
}
static int ext4_fchown_op(vfs_file_t *f, int uid, int gid)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !ef->ino)
		return ST_OK;
	return ext4_fchown_ino(ef->fs, ef->ino, uid, gid);
}
static int ext4_statfs_op(vfs_mount_t *m, struct vfs_statfs *out)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!out || !fs)
		return ST_INVALID;
	unsigned long bs, blk, bf, ba, fi, ff, nl, ty;
	if (ext4_get_statfs(fs, &bs, &blk, &bf, &ba, &fi, &ff, &nl, &ty) != 0)
		return ST_IO;
	out->f_type = ty;
	out->f_bsize = bs;
	out->f_frsize = bs;
	out->f_blocks = blk;
	out->f_bfree = bf;
	out->f_bavail = ba;
	out->f_files = fi;
	out->f_ffree = ff;
	out->f_fsid = 0;
	out->f_namelen = nl;
	return ST_OK;
}

/* Fill a kstat for an open ext4 handle (real mode/uid/gid) — backs the fd-based
 * permission checks (fchmod/fchown ownership). */
static int ext4_fstat_op(vfs_file_t *f, struct kstat *st)
{
	if (!f || !st)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_meta_rlock(fs);
	int r = ext4_stat_fill(fs, ef->ino, st);
	ext4_meta_runlock(fs);
	return r;
}

/* Per-inode set-id-strip hint (S_NOSEC analog).  Lives on the shared icache
 * entry, so every handle to the inode sees the same state.  Cheap: just a flag
 * bit, no I/O or io_lock — a race only costs a redundant strip evaluation. */
static int ext4_setid_clean_op(vfs_file_t *f, int mark)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	ic_inode_t *ic = ef ? (ic_inode_t *)ef->inode : 0;
	if (!ic) {
		/* No shared inode entry to carry the hint.  A regular file only lands
         * here if icache_get(&fs->sb, ) failed at open (OOM): report "not clean" so the
         * strip still runs every modify (it reads the mode directly) — safe,
         * just unamortised.  Directories/special files have no inode by design
         * and must never be stripped (e.g. a set-group-ID dir), so: clean. */
		if (mark)
			return 1; /* nowhere to record it */
		return (ef && !ef->is_dir) ? 0 : 1;
	}
	if (mark) {
		ic->flags |= (uint32_t)IC_SETID_CLEAN;
		return 1;
	}
	return (ic->flags & IC_SETID_CLEAN) ? 1 : 0;
}

/* ---- xattr VFS ops ----  Each takes the I/O lock (which begins a journalled
 * transaction); the set/remove paths refuse cleanly on a read-only/errored fs.
 * The full attribute name is parsed into a namespace index + suffix here. */
static int ext4_xattr_resolve(ext4_fs_t *fs, const char *path, int nofollow,
			      unsigned long *ino)
{
	return ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, ino, 0);
}
static int ext4_getxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    const char *name, void *val, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_get_ino(fs, ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
/* Fetch by already-resolved inode number, skipping the path walk (used by the
 * permission code, which has the inode from its preceding stat). */
static int ext4_getxattr_ino_op(vfs_mount_t *m, unsigned long ino,
				const char *name, void *val, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !name || ino == 0)
		return ST_INVALID;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	int r = ext4_xattr_get_ino(fs, ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_setxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    const char *name, const void *val,
			    unsigned long size, int flags)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_UNSUPPORTED; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_set_ino(fs, ino, idx, suf, slen, val, size,
				       flags, 0);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_listxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			     char *list, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_list_ino(fs, ino, list, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_removexattr_op(vfs_mount_t *m, const char *path, int nofollow,
			       const char *name)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_set_ino(fs, ino, idx, suf, slen, 0, 0, 0, 1);
	ext4_io_unlock(fs);
	return r;
}
static ext4_file_t *ext4_ef(vfs_file_t *f)
{
	if (!f || f->ops != &ext4_vfs_ops || !f->fs_private)
		return 0;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	return ef->fs ? ef : 0;
}
static int ext4_fgetxattr_op(vfs_file_t *f, const char *name, void *val,
			     unsigned long size)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	int r = ext4_xattr_get_ino(fs, ef->ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_fsetxattr_op(vfs_file_t *f, const char *name, const void *val,
			     unsigned long size, int flags)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_UNSUPPORTED; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	int r = ext4_xattr_set_ino(fs, ef->ino, idx, suf, slen, val, size,
				   flags, 0);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_flistxattr_op(vfs_file_t *f, char *list, unsigned long size)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_meta_rlock(fs);
	int r = ext4_xattr_list_ino(fs, ef->ino, list, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_fremovexattr_op(vfs_file_t *f, const char *name)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	int r = ext4_xattr_set_ino(fs, ef->ino, idx, suf, slen, 0, 0, 0, 1);
	ext4_io_unlock(fs);
	return r;
}

/* Report the VFS_ATTR_* flags (immutable / append-only) for an inode, so the
 * VFS can veto modifications independently of the rwx mode bits.  ext4 stores
 * these in the on-disk inode's i_flags; this is the filesystem-specific input
 * to the otherwise-generic VFS permission check. */
static int ext4_inode_flags_op(vfs_mount_t *m, unsigned long ino,
			       uint32_t *out_flags)
{
	if (!out_flags)
		return ST_INVALID;
	*out_flags = 0;
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_NO_DEVICE;
	ext4_inode in;
	ext4_meta_rlock(fs);
	int r = ext4_read_inode_loc(fs, ino, &in, 0, 0);
	ext4_meta_runlock(fs);
	if (r != ST_OK)
		return ST_IO;
	if (in.i_flags & EXT4_INODE_IMMUTABLE_FL)
		*out_flags |= VFS_ATTR_IMMUTABLE;
	if (in.i_flags & EXT4_INODE_APPEND_FL)
		*out_flags |= VFS_ATTR_APPEND;
	return ST_OK;
}

const vfs_ops_t ext4_vfs_ops = {
	.open = ext4_open,
	.stat = ext4_stat_vfs,
	.read = ext4_read,
	.read_at = ext4_read_at,
	.write_at = ext4_write_at,
	.write = ext4_write,
	.seek = ext4_seek,
	.readdir = ext4_readdir,
	.truncate = ext4_truncate,
	.unlink = ext4_unlink,
	.rename = ext4_rename,
	.mkdir = ext4_mkdir,
	.rmdir = ext4_rmdir,
	.chdir = ext4_chdir,
	.close = ext4_close,
	.release_locks_for_task = ext4_release_locks_for_task,
	.fsync = ext4_fsync,
	.lstat = ext4_lstat_op,
	.symlink = ext4_symlink_op,
	.readlink = ext4_readlink_op,
	.link = ext4_link_op,
	.chmod = ext4_chmod_op,
	.chown = ext4_chown_op,
	.fchmod = ext4_fchmod_op,
	.fchown = ext4_fchown_op,
	.utimens = ext4_utimens_op,
	.futimens = ext4_futimens_op,
	.statfs = ext4_statfs_op,
	.fstat = ext4_fstat_op,
	.setid_clean = ext4_setid_clean_op,
	.sync = ext4_sync_op,
	.getxattr = ext4_getxattr_op,
	.setxattr = ext4_setxattr_op,
	.listxattr = ext4_listxattr_op,
	.removexattr = ext4_removexattr_op,
	.fgetxattr = ext4_fgetxattr_op,
	.fsetxattr = ext4_fsetxattr_op,
	.flistxattr = ext4_flistxattr_op,
	.fremovexattr = ext4_fremovexattr_op,
	.getxattr_ino = ext4_getxattr_ino_op,
	.inode_flags = ext4_inode_flags_op,
	.mknod = ext4_mknod,
	.open_mode = ext4_open_mode,
};
