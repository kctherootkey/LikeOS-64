// LikeOS - ext4 filesystem driver: reading directories
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

static unsigned ext4_ft_to_dt(unsigned ft)
{
	switch (ft) {
	case EXT4_FT_REG_FILE:
		return DT_REG;
	case EXT4_FT_DIR:
		return DT_DIR;
	case EXT4_FT_CHRDEV:
		return DT_CHR;
	case EXT4_FT_BLKDEV:
		return DT_BLK;
	case EXT4_FT_FIFO:
		return DT_FIFO;
	case EXT4_FT_SOCK:
		return DT_SOCK;
	case EXT4_FT_SYMLINK:
		return DT_LNK;
	default:
		return DT_UNKNOWN;
	}
}

long ext4_readdir_impl(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !buf || bytes <= 0)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->is_dir)
		return -ENOTDIR;
	ext4_fs_t *fs = ef->fs;

	ext4_inode din;
	if (!ext4_get_inode_cached(fs, ef->ino, &din))
		return ST_IO;
	unsigned long dsize = ext4_inode_size(&din);
	uint32_t gen = din.i_generation; /* for the leaf csum */

	uint8_t *blk = (uint8_t *)ext4_bget(fs);
	if (!blk)
		return ST_NOMEM;

	unsigned out_off = 0;
	while (ef->dir_pos < dsize) {
		unsigned long b = ef->dir_pos / fs->block_size;
		unsigned in_blk = ef->dir_pos % fs->block_size;
		unsigned long pbn;
		int mrc = ext4_map_blk(fs, ef->ino, b, &pbn);
		if (mrc != ST_OK) {
			/* A damaged map: an error, never an empty block (the
			 * entries already returned stand; the next call
			 * starts here and reports it). */
			ext4_bput(fs, blk);
			return out_off ? (long)out_off : mrc;
		}
		if (pbn == 0) {
			ef->dir_pos = (b + 1) *
				      fs->block_size; /* skip sparse block */
			continue;
		}
		if (ext4_read_block(fs, pbn, blk) != ST_OK) {
			ef->dir_pos = (b + 1) * fs->block_size;
			continue;
		}
		if (!ext4_mbc_verified(fs, pbn)) {
			if (!ext4_dir_csum_ok(fs, ef->ino, gen, blk)) {
				ext4_bput(fs, blk);
				ext4_fs_error(fs,
					      "directory leaf checksum mismatch",
					      ef->ino);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, pbn);
		}
		int block_done = 0;
		while (in_blk + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de =
				(ext4_dir_entry_2 *)(blk + in_blk);
			unsigned rec = de->rec_len;
			if (rec < 8 || in_blk + rec > fs->block_size) {
				WARN_ON_ONCE(1);
				block_done = 1;
				break;
			}
			if (de->inode != 0 && de->name_len != 0) {
				unsigned namelen = de->name_len;
				unsigned reclen = (19 + namelen + 1 + 7) &
						  ~7u; /* align 8 */
				if (out_off + reclen > (unsigned)bytes) {
					/* Output buffer full; resume here next call. */
					ext4_bput(fs, blk);
					return out_off ? (long)out_off :
							 -EINVAL;
				}
				/* `buf' is the kernel buffer of ext4_readdir,
				 * copied to the caller after the lock is
				 * dropped. */
				struct dirent64 *ld =
					(struct dirent64 *)((uint8_t *)
									  buf +
								  out_off);
				ld->d_ino = de->inode;
				ld->d_off = (int64_t)(ef->dir_pos + rec);
				ld->d_reclen = (uint16_t)reclen;
				ld->d_type =
					(uint8_t)ext4_ft_to_dt(de->file_type);
				mm_memcpy(ld->d_name, de->name, namelen);
				ld->d_name[namelen] = '\0';
				out_off += reclen;
			}
			in_blk += rec;
			ef->dir_pos += rec;
		}
		if (block_done) {
			/* Advance to the next block boundary. */
			ef->dir_pos = (b + 1) * fs->block_size;
		}
	}
	ext4_bput(fs, blk);
	return (long)out_off;
}
