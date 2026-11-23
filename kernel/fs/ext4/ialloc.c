// LikeOS - ext4 filesystem driver: inode allocation, freeing and initialisation
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* bg_itable_unused: count of never-used inodes at the tail of the group's inode
 * table.  e2fsck honours it on metadata_csum / uninit_bg images (it skips that
 * tail during scanning), so allocating an inode past the tracked region must
 * shrink the count or e2fsck flags the freshly used inode as "in the unused
 * range" (and disagrees with the inode bitmap). */
unsigned ext4_itable_unused(const ext4_fs_t *fs, unsigned g)
{
	unsigned v = fs->gdt[g].bg_itable_unused_lo;
	if (fs->desc_size >= 64)
		v |= (unsigned)fs->gdt[g].bg_itable_unused_hi << 16;
	return v;
}
void ext4_itable_unused_set(ext4_fs_t *fs, unsigned g, unsigned v)
{
	fs->gdt[g].bg_itable_unused_lo = (uint16_t)(v & 0xFFFF);
	if (fs->desc_size >= 64)
		fs->gdt[g].bg_itable_unused_hi = (uint16_t)(v >> 16);
}

/* Allocate one free inode (preferring the parent's group). Returns 0 on fail. */
unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino,
				      int is_dir)
{
	unsigned pgroup =
		parent_ino ?
			(unsigned)((parent_ino - 1) / fs->inodes_per_group) :
			0;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return 0;
	for (unsigned gi = 0; gi < fs->groups_count; gi++) {
		unsigned g = (pgroup + gi) % fs->groups_count;
		if (fs->gdt[g].bg_free_inodes_count_lo == 0)
			continue;
		/* Group lock: this group's inode-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_inode_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			kfree(bm);
			return 0;
		}
		if (lst != ST_OK) {
			ext4_bg_unlock(fs, g);
			continue;
		}
		for (unsigned i = 0; i < fs->inodes_per_group; i++) {
			if (ext4_bm_test(bm, i))
				continue;
			unsigned long ino =
				(unsigned long)g * fs->inodes_per_group + i + 1;
			if (ino < fs->first_ino && ino != EXT4_ROOT_INO)
				continue;
			/* A group that gets an inode in use gets a real
			 * block bitmap as well. */
			if (ext4_bg_block_bitmap_materialize(fs, g) != ST_OK) {
				ext4_bg_unlock(fs, g);
				kfree(bm);
				return 0;
			}
			ext4_bm_set(bm, i);
			ext4_inode_bitmap_csum_set(fs, g, bm);
			/* shrink bg_itable_unused if we used an inode past the tracked front
             * region (as inode allocation is specified to). */
			if (fs->has_gd_csum) {
				unsigned off1 =
					i +
					1; /* 1-based offset within the group */
				unsigned used_front = fs->inodes_per_group -
						      ext4_itable_unused(fs, g);
				/* none at all in an INODE_UNINIT group, which
				 * is initialized by this allocation */
				if (uninit)
					used_front = 0;
				if (off1 > used_front)
					ext4_itable_unused_set(
						fs, g,
						fs->inodes_per_group - off1);
			}
			if (ext4_write_block(fs, bblk, bm) != ST_OK) {
				ext4_bg_unlock(fs, g);
				kfree(bm);
				return 0;
			}
			if (uninit)
				fs->gdt[g].bg_flags &=
					(uint16_t)~EXT4_BG_INODE_UNINIT;
			fs->gdt[g].bg_free_inodes_count_lo--;
			if (is_dir)
				fs->gdt[g].bg_used_dirs_count_lo++;
			fs->sb_copy.s_free_inodes_count--;
			ext4_gd_dirty(fs, g);
			ext4_bg_unlock(fs, g);
			/* This number almost certainly belonged to a file that
			 * has since been deleted.  Both caches are keyed by
			 * inode number, so whatever is still held under it
			 * describes the DEAD file: the new file would then be
			 * read back as the old one's bytes, at the new file's
			 * own (correctly stat'd) length — the right size, the
			 * wrong contents, and no error anywhere.
			 *
			 * Conventional practice attaches the cache to the
			 * in-core inode, which is torn down with the file, so
			 * a recycled number starts empty by construction.
			 * Keyed by number, the equivalent has to be explicit,
			 * and it belongs HERE rather than only at the points
			 * where an inode is freed: a file unlinked while still
			 * open has its pages faulted straight back in by its
			 * last reader, after the unlink invalidated them.
			 * Allocation is the one point every new file passes
			 * through, whatever became of the previous owner. */
			icache_remove(&fs->sb, EXT4_BID_ENC(ino, 0));
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
			/* Same argument for the names: if this number last
			 * belonged to a directory, whatever is still cached
			 * under it describes that directory's contents, and
			 * the new one would answer lookups with the dead
			 * one's children. */
			ext4_dc_drop_dir(fs, ino);
			/* And for a pending size: it describes the file that
			 * used to have this number.  Left in place it would be
			 * laid over the new file's inode on every read, giving
			 * it the dead one's length. */
			ext4_di_clear(fs, ino);
			kfree(bm);
			return ino;
		}
		ext4_bg_unlock(fs, g);
	}
	kfree(bm);
	return 0;
}

void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir)
{
	if (ino == 0 || ino > fs->inodes_count)
		return;
	/* Release the external xattr block (i_file_acl), if any, so deleting a file
     * that carried a large/ACL attribute doesn't leak the block. */
	{
		ext4_inode in;
		if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK &&
		    in.i_file_acl_lo)
			ext4_free_block(fs, in.i_file_acl_lo);
	}
	unsigned g = (ino - 1) / fs->inodes_per_group;
	unsigned i = (ino - 1) % fs->inodes_per_group;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return;
	/* Group lock: serialises this group's bitmap read-modify-write. */
	ext4_bg_lock(fs, g);
	unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
	int uninit = 0;
	/* An INODE_UNINIT group loads as all-free: nothing in it to release. */
	if (ext4_load_inode_bitmap(fs, g, bm, &uninit) == ST_OK && !uninit) {
		if (ext4_bm_test(bm, i)) {
			ext4_bm_clear(bm, i);
			ext4_inode_bitmap_csum_set(fs, g, bm);
			ext4_write_block(fs, bblk, bm);
			fs->gdt[g].bg_free_inodes_count_lo++;
			if (is_dir && fs->gdt[g].bg_used_dirs_count_lo)
				fs->gdt[g].bg_used_dirs_count_lo--;
			fs->sb_copy.s_free_inodes_count++;
			ext4_gd_dirty(fs, g);
		}
	}
	ext4_bg_unlock(fs, g);
	kfree(bm);
}

/* Decide a new inode's owner/group/mode (POSIX semantics):
 *   - the creating process owns the inode (its fs-uid);
 *   - if the parent directory is set-group-ID, the new inode takes the parent's
 *     group, AND a new directory also inherits the set-group-ID bit; a new
 *     non-directory that is group-executable + set-group-ID, created by an
 *     unprivileged process that is not in the parent's group, has its
 *     set-group-ID bit dropped (no privilege escalation via group);
 *   - otherwise the group is the creator's fs-gid.
 * With no current task (early/kernel-time creation) inherit the parent's ids so
 * the boot-time, all-root tree is built exactly as before. */
void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode,
			    int is_dir, unsigned *uid, unsigned *gid,
			    unsigned *mode)
{
	task_t *cur = sched_current();
	if (!cur) {
		*uid = puid;
		*gid = pgid;
		return;
	}
	*uid = (unsigned)cur->cred.fsuid;
	if (pmode & S_ISGID) {
		*gid = pgid;
		if (is_dir) {
			*mode |= S_ISGID; /* dirs always inherit it   */
		} else if (cur->cred.fsuid != 0 &&
			   (*mode & (S_ISGID | S_IXGRP)) ==
				   (S_ISGID | S_IXGRP) &&
			   !cred_in_group(&cur->cred, pgid)) {
			*mode &= ~(
				unsigned)S_ISGID; /* not a member: strip sgid */
		}
	} else {
		*gid = (unsigned)cur->cred.fsgid;
	}
}

/* Initialise + write a brand-new regular-file inode (owner/group/mode decided by
 * ext4_init_owner — the creating process). */
/* Write a freshly allocated inode, zeroing the ENTIRE on-disk slot first so a
 * reused inode does not inherit the previous occupant's in-inode (ibody) xattrs.
 * ext4_write_inode_struct deliberately preserves the xattr tail (for updates),
 * which would be wrong here. */
int ext4_write_inode_new(ext4_fs_t *fs, unsigned long ino,
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
	uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
	if (!buf)
		return ST_NOMEM;
	int st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		kfree(buf);
		return st;
	}
	mm_memset(buf + off, 0,
		  fs->inode_size); /* clear slot incl. the xattr tail */
	unsigned copy = fs->inode_size < sizeof(ext4_inode) ?
				fs->inode_size :
				sizeof(ext4_inode);
	mm_memcpy(buf + off, in, copy);
	ext4_inode_csum_set(fs, ino, buf + off);
	st = ext4_write_block(fs, blk, buf);
	kfree(buf);
	return st;
}

/* Undo an inode allocation whose creation failed part-way.  The slot may
 * never have been written (a never-zeroed inode table leaves stale bytes in
 * it, a zeroed one a slot whose checksum is wrong), so it is first stored as
 * a deleted inode -- zeroed, dtime set, checksummed -- and only then released
 * the normal way, which reads it back.  Blocks the caller already attached
 * must have been freed before. */
void ext4_abandon_new_inode(ext4_fs_t *fs, unsigned long ino, int is_dir)
{
	ext4_inode dead;
	mm_memset(&dead, 0, sizeof(dead));
	dead.i_dtime = (uint32_t)timer_get_epoch();
	if (fs->inode_size > 128)
		dead.i_extra_isize = 32;
	ext4_write_inode_new(fs, ino, &dead);
	ext4_inode_cache_drop(fs, ino);
	ext4_free_inode(fs, ino, is_dir);
}

int ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
			     unsigned uid, unsigned gid)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)mode;
	ext4_inode_set_uid(&in, (uint32_t)uid);
	ext4_inode_set_gid(&in, (uint32_t)gid);
	in.i_links_count = 1;
	ext4_new_block_map(fs, &in);
	uint64_t now = timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;
	return ext4_write_inode_new(fs, ino,
				    &in); /* zeroes stale ibody xattrs */
}
