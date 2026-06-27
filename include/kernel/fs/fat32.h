// LikeOS-64 - FAT32 read-only skeleton
#ifndef LIKEOS_FAT32_H
#define LIKEOS_FAT32_H

#include <kernel/uapi/status.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>

typedef struct fat32_fs {
	const block_device_t *bdev;
	unsigned long fat_start_lba;
	unsigned long data_start_lba;
	unsigned long
		part_lba_offset; // partition/superfloppy base LBA (0 for superfloppy)
	unsigned int sectors_per_cluster;
	unsigned int bytes_per_sector;
	unsigned int root_cluster;
	unsigned int num_fats;
	unsigned long fat_size_sectors;
	/* VFS-layer superblock.  sb.fs_private points back at this fat32_fs_t,
     * and sb.ops dispatches the generic cache <-> FS calls
     * (block_to_lba / next_block / write_inode / lock_io ...).  The
     * caches reach the FS only through this struct. */
	vfs_superblock_t sb;
} fat32_fs_t;

typedef struct {
	vfs_file_t vfs; // must be first for casting
	fat32_fs_t *fs;
	unsigned long start_cluster;
	unsigned long size;
	unsigned long pos;
	unsigned long current_cluster; // cluster we have loaded
	int is_dir;
	unsigned long dir_iter_cluster;
	unsigned int dir_iter_index;
	unsigned long parent_cluster;
	unsigned long dirent_cluster;
	unsigned int dirent_index;
	char name83[12]; // 8.3 name (11 chars + null)
	// Page cache read-ahead state (embedded to avoid header dependency)
	unsigned long ra_last_page; // last page accessed
	int ra_seq_count; // consecutive sequential accesses
	int ra_pages; // current read-ahead window size
	// Inode cache reference (opaque pointer to avoid header dependency)
	void *inode; // ic_inode_t* from icache
	/* Dirent-update batching.  fat32_write_impl used to call
     * fat32_update_dirent() (a full read-modify-write of the parent
     * directory's cluster = ~64 KB of USB traffic) on EVERY write that
     * extended the file size.  Setting this flag instead defers the
     * persist to fat32_close (or an explicit fsync) — for a streaming
     * 100 MB curl/dd download that drops dirent traffic from ~3 GB
     * back to a single ~32 KB write at close. */
	int dirent_dirty;
} fat32_file_t;

// FAT32 attribute flags
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20

int fat32_mount(const block_device_t *bdev, fat32_fs_t *out);
int fat32_vfs_register_root(fat32_fs_t *fs);
void fat32_list_root(void (*cb)(const char *, unsigned attr,
				unsigned long size));
void fat32_debug_dump_root(void);
int fat32_dir_list(unsigned long cluster,
		   void (*cb)(const char *, unsigned attr, unsigned long size));
int fat32_dir_find(unsigned long start_cluster, const char *name,
		   unsigned *attr, unsigned long *first_cluster,
		   unsigned long *size);
unsigned long fat32_root_cluster(void);
void fat32_set_cwd(unsigned long cluster);
unsigned long fat32_get_cwd(void);
// Extended path resolution (supports relative, '.', '..')
int fat32_resolve_path(unsigned long start_cluster, const char *path,
		       unsigned *attr, unsigned long *first_cluster,
		       unsigned long *size);
// Metadata/stat helper
int fat32_stat(unsigned long start_cluster, const char *path, unsigned *attr,
	       unsigned long *first_cluster, unsigned long *size);
// Get parent cluster of directory (returns same cluster for root)
unsigned long fat32_parent_cluster(unsigned long dir_cluster);

// Write helpers
int fat32_unlink_path(const char *path);
int fat32_rename_path(const char *oldpath, const char *newpath);
int fat32_mkdir_path(const char *path);
int fat32_rmdir_path(const char *path);

/* Filesystem statistics for statfs(2) */
typedef struct {
	unsigned long f_bsize; /* optimal transfer block size */
	unsigned long f_frsize; /* fragment size (same as bsize for FAT32) */
	unsigned long f_blocks; /* total data blocks */
	unsigned long f_bfree; /* free blocks */
	unsigned long
		f_bavail; /* free blocks available to unprivileged users */
	unsigned long f_files; /* total file nodes */
	unsigned long f_ffree; /* free file nodes */
	unsigned long f_fsid; /* filesystem ID (not meaningful here) */
	unsigned long f_namelen; /* maximum filename length */
	unsigned long f_type; /* filesystem type magic */
} fat32_statfs_t;

/* Fill fs_info with current filesystem statistics.  Returns 0 on success. */
int fat32_get_statfs(fat32_statfs_t *info);

/* Update mtime of a file at path.  mtime_nsec == KRN_UTIME_NOW means
 * set to current time; KRN_UTIME_OMIT leaves it unchanged. */
#define KRN_UTIME_NOW ((long)1073741823L)
#define KRN_UTIME_OMIT ((long)1073741822L)
int fat32_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec);

/* Page cache support: exported FAT32 internals */
extern fat32_fs_t *g_root_fs;
void fat32_io_lock(void);
void fat32_io_unlock(void);
/* If the FAT32 I/O mutex is currently held by the given task id, force-release
 * it and wake any waiters.  Used by the task-exit path (via the VFS layer) to
 * recover from a task killed mid-write while inside the FS. */
int fat32_io_release_if_owner(uint64_t task_id);
unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster);

#endif // LIKEOS_FAT32_H
