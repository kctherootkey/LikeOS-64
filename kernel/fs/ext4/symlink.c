// LikeOS - ext4 filesystem driver: symbolic links
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Symlinks, hard links, chmod/chown, lstat.
 * =================================================================== */

int ext4_symlink(ext4_fs_t *fs, const char *target, const char *linkpath)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!target || !linkpath || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, linkpath, &parent, name, sizeof(name)) !=
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
	unsigned tlen = 0;
	while (target[tlen])
		tlen++;
	if (tlen >= fs->block_size) {
		ext4_io_unlock(fs);
		return ST_NAMETOOLONG; /* a target is at most a block */
	}

	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin;
	if (ext4_read_inode_loc(fs, parent, &pin, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin);
		pgid = ext4_inode_gid(&pin);
		pmode = pin.i_mode;
	}
	unsigned nuid = puid, ngid = pgid, nmode = S_IFLNK | 0777;
	ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);

	unsigned long nino = ext4_alloc_inode(fs, parent, 0);
	if (nino == 0) {
		ext4_io_unlock(fs);
		return ST_NOMEM;
	}
	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 1;
	in.i_size_lo = tlen;
	uint32_t now = (uint32_t)timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;

	if (tlen < 60) { /* fast symlink — inline target  */
		mm_memcpy(in.i_block, target, tlen);
		ext4_inode_cache_flush(fs);
		ext4_write_inode_new(
			fs, nino,
			&in); /* fresh inode: clear stale ibody xattrs */
	} else { /* slow symlink — one data block  */
		ext4_new_block_map(fs, &in);
		if (ext4_alloc_blocks_for_file(fs, nino, &in, 0, 1) != 1) {
			ext4_abandon_new_inode(fs, nino, 0);
			ext4_flush_meta(fs);
			ext4_io_unlock(fs);
			return ST_NOMEM;
		}
		unsigned long pbn = ext4_map_in(fs, nino, &in, 0);
		uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
		if (!blk) {
			ext4_free_blocks_from(fs, nino, &in, 0);
			ext4_abandon_new_inode(fs, nino, 0);
			ext4_flush_meta(fs);
			ext4_io_unlock(fs);
			return ST_NOMEM;
		}
		mm_memset(blk, 0, fs->block_size);
		mm_memcpy(blk, target, tlen);
		if (ext4_write_target_ok(fs, pbn, 1))
			ext4_write_sectors(fs->bdev,
					   fs->part_lba_offset +
						   pbn * fs->sectors_per_block,
					   fs->sectors_per_block, blk);
		kfree(blk);
		ext4_inode_cache_flush(fs);
		ext4_write_inode_new(
			fs, nino,
			&in); /* fresh inode: clear stale ibody xattrs */
	}
	if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_SYMLINK) !=
	    ST_OK) {
		ext4_abandon_new_inode(fs, nino, 0);
		ext4_flush_meta(fs);
		ext4_io_unlock(fs);
		return ST_IO;
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	ext4_io_unlock(fs);
	return ST_OK;
}

int ext4_readlink(ext4_fs_t *fs, const char *path, char *buf, unsigned long bufsiz)
{
	if (!path || !buf || !fs || bufsiz == 0)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	if (ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &ino, 0) != ST_OK) {
		ext4_meta_runlock(fs);
		return ST_NOT_FOUND;
	}
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in)) {
		ext4_meta_runlock(fs);
		return ST_IO;
	}
	if ((in.i_mode & S_IFMT) != S_IFLNK) {
		ext4_meta_runlock(fs);
		return ST_INVALID;
	}
	char *tmp = pathbuf_get();
	if (!tmp) {
		ext4_meta_runlock(fs);
		return ST_NOMEM;
	}
	int tl = ext4_read_symlink_target(fs, ino, &in, tmp, PATHBUF_SIZE);
	ext4_meta_runlock(fs);
	if (tl < 0) {
		pathbuf_put(tmp);
		return tl == -2 ? ST_NAMETOOLONG : ST_IO;
	}
	unsigned long n = (unsigned long)tl;
	if (n > bufsiz)
		n = bufsiz; /* readlink truncates, no NUL     */
	mm_memcpy(buf, tmp, n);
	pathbuf_put(tmp);
	return (int)n;
}
