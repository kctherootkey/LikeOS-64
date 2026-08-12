/* LikeOS-64 — VFS superblock interface.
 *
 * Filesystem-independent abstraction between the cache layers
 * (dcache / icache / pagecache) and a concrete filesystem driver
 * (FAT32 today, EXT4 later).  Caches dispatch all FS-specific I/O
 * through `vfs_sb_ops_t`, so they never reference fat32_fs_t,
 * fat32_io_lock, fat32_next_cluster_cached, or any other driver
 * symbol directly.
 *
 *   dcache / icache / pagecache
 *               │
 *               ▼
 *           vfs_superblock_t  (this header)
 *               │
 *      ┌────────┴────────┐
 *      ▼                 ▼
 *    FAT32             EXT4
 *
 * Conventions
 * -----------
 *   block_id      – Filesystem-native data-area identifier.  On FAT32 this is
 *                   the cluster number; on extent-based filesystems it is the
 *                   first block of an extent.  The caches store and pass it
 *                   around as an opaque `unsigned long`.
 *   inode_no      – Filesystem-native inode identifier.  On FAT32 we reuse the
 *                   file's first cluster (a de-facto inode number); on EXT4
 *                   this is the inode-table index.
 *   end_of_chain  – Driver-defined sentinel returned by next_block() at the
 *                   end of a file's block chain.  FAT32 uses 0x0FFFFFF8;
 *                   EXT4 can use 0.  Caches treat any value >= the marker
 *                   (and 0) as "no more blocks".
 */
#ifndef _KERNEL_VFS_SB_H_
#define _KERNEL_VFS_SB_H_

#include <kernel/uapi/types.h>
#include <kernel/dev/block/block.h>

struct ic_inode; /* forward — see icache.h                            */
struct vfs_superblock;
typedef struct vfs_superblock vfs_superblock_t;

typedef struct vfs_sb_ops {
	/* Size, in bytes, of one filesystem block.  FAT32 cluster size, EXT4
     * block size.  Pagecache uses this to decide pages-per-block vs
     * blocks-per-page. */
	unsigned long (*block_size)(vfs_superblock_t *sb);

	/* Underlying block device. */
	const block_device_t *(*bdev)(vfs_superblock_t *sb);

	/* Bytes per disk sector (typically 512). */
	unsigned long (*sector_size)(vfs_superblock_t *sb);

	/* Translate an FS-native block_id to an absolute LBA on bdev() (already
     * including any partition offset).  Returns 0 for block_id values that
     * cannot be mapped. */
	unsigned long (*block_to_lba)(vfs_superblock_t *sb,
				      unsigned long block_id);

	/* Threshold for the end-of-chain sentinel.  Cache code treats any
     * block_id value >= this (and == 0) as "no more data". */
	unsigned long (*end_of_chain_marker)(vfs_superblock_t *sb);

	/* Walk the block chain by one step.  Returns the next block_id after
     * `cur_block`, or 0 / end_of_chain_marker at EOF.  Caller must hold
     * lock_io() to keep the on-disk allocator state stable. */
	unsigned long (*next_block)(vfs_superblock_t *sb,
				    unsigned long cur_block);

	/* Map a file's Nth block directly, without walking to it.  OPTIONAL:
     * null for a filesystem whose blocks can only be reached in order.
     *
     * next_block() describes a CHAIN, which is what a FAT is; a filesystem
     * with a block map (ext4's extent tree) can answer for block N straight
     * out, and the two answers are not always the same.  A walk has to decide
     * where the file ends, and it does that from the recorded size -- so a
     * size that lags the allocation ends the walk short of blocks the file
     * demonstrably owns.  The map has no such second opinion.
     *
     * That matters for writeback, which must not conclude "no block" from a
     * cache or a length when the filesystem itself can still map the page:
     * the page is DIRTY, so the alternative to storing it is losing it and
     * pinning its memory for ever.  Returns a block_id, or 0 if the block
     * genuinely has nothing behind it (a hole, or past the end).  Called with
     * lock_map() held shared. */
	unsigned long (*map_block)(vfs_superblock_t *sb, unsigned long chain_id,
				   unsigned long block_index);

	/* Persist a dirty inode's metadata to disk.  The driver translates the
     * generic ic_inode_t fields (size, etc.) plus its own fs_private data
     * back into the on-disk inode/dirent layout.  Takes lock_io() as needed
     * internally.  Returns 0 on success. */
	int (*write_inode)(vfs_superblock_t *sb, struct ic_inode *inode);

	/* Filesystem-level sleeping mutex.  Held by cache code across multi-step
     * disk operations (chain walks, page flushes) so the driver's allocator
     * sees a consistent view. */
	void (*lock_io)(vfs_superblock_t *sb);
	void (*unlock_io)(vfs_superblock_t *sb);

	/* OPTIONAL shared-mode mapping lock.  When a driver provides these, the
     * pagecache holds them (instead of lock_io) around the block-mapping
     * calls only (next_block / block_to_lba) and performs the data transfer
     * with NO filesystem lock held — the driver must then fence data-block
     * lifetime itself (e.g. per-inode locks that exclude truncate/free
     * while a read is in flight).  Drivers that leave these NULL keep the
     * old behaviour: lock_io held across mapping AND transfer. */
	void (*lock_map)(vfs_superblock_t *sb);
	void (*unlock_map)(vfs_superblock_t *sb);

	/* A block_id that the driver reserves for metadata and that user data
     * must never be written to (FAT32 root cluster).  Pagecache's flush
     * paths guard against this to avoid corrupting the root dir.  Drivers
     * with no such concept may return 0. */
	unsigned long (*reserved_meta_block)(vfs_superblock_t *sb);
} vfs_sb_ops_t;

struct vfs_superblock {
	const vfs_sb_ops_t *ops;
	void *fs_private; /* fat32_fs_t*, ext4_fs_t*, ...        */
};

/* Globally-registered root superblock.  Installed by the FS mount path
 * (fat32_mount today, ext4_mount tomorrow).  NULL before mount.  The
 * caches use this when they need to flush back without an explicit sb. */
extern vfs_superblock_t *g_root_sb;

/* Convenience wrappers (compile-out at -O). */
static inline unsigned long vfs_sb_block_size(vfs_superblock_t *sb)
{
	return sb->ops->block_size(sb);
}

static inline const block_device_t *vfs_sb_bdev(vfs_superblock_t *sb)
{
	return sb->ops->bdev(sb);
}

static inline unsigned long vfs_sb_sector_size(vfs_superblock_t *sb)
{
	return sb->ops->sector_size(sb);
}

static inline unsigned long vfs_sb_block_to_lba(vfs_superblock_t *sb,
						unsigned long block_id)
{
	return sb->ops->block_to_lba(sb, block_id);
}

static inline unsigned long vfs_sb_end_of_chain(vfs_superblock_t *sb)
{
	return sb->ops->end_of_chain_marker(sb);
}

static inline unsigned long vfs_sb_next_block(vfs_superblock_t *sb,
					      unsigned long cur)
{
	return sb->ops->next_block(sb, cur);
}

/* 0 when the filesystem cannot map a block directly -- indistinguishable, to
 * the caller, from "there is no such block", which is the safe reading. */
static inline unsigned long vfs_sb_map_block(vfs_superblock_t *sb,
					     unsigned long chain_id,
					     unsigned long block_index)
{
	if (!sb || !sb->ops || !sb->ops->map_block)
		return 0;
	return sb->ops->map_block(sb, chain_id, block_index);
}

static inline int vfs_sb_write_inode(vfs_superblock_t *sb, struct ic_inode *ino)
{
	return sb->ops->write_inode(sb, ino);
}

static inline void vfs_sb_lock_io(vfs_superblock_t *sb)
{
	sb->ops->lock_io(sb);
}

static inline void vfs_sb_unlock_io(vfs_superblock_t *sb)
{
	sb->ops->unlock_io(sb);
}

static inline unsigned long vfs_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	return sb->ops->reserved_meta_block(sb);
}

#endif /* _KERNEL_VFS_SB_H_ */
