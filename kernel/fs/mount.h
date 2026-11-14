// LikeOS -- the mount table, as the VFS dispatch sees it.
//
// Internal to kernel/fs: vfs.c finds the filesystem a path is on through
// these, and the drivers that can be mounted more than once use the cache
// release helper on unmount.  Everything else goes through vfs.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_MOUNT_H
#define KERNEL_FS_MOUNT_H

#include <kernel/fs/vfs.h>

/* The mount `path' is on -- the live mount whose mount point is the longest
 * prefix of `path' that ends at a component boundary -- with the path as the
 * driver must be handed it in *rel (relative to the mount point, "/" for the
 * mount point itself, or `path' unchanged for a driver that takes absolute
 * paths).  A relative path belongs to the root.  NULL when nothing is
 * mounted there (no root yet).
 *
 * The mount cannot be unmounted until vfs_mnt_put(): every path operation
 * runs between the two.  The hold belongs to the calling task (recorded in
 * task_t.mnt_held, dropped by its teardown if it dies holding it), so get
 * and put are made by the same task.  Taking it is lock-free; for the
 * boot-time mounts, which are never unmounted, it costs nothing at all. */
vfs_mount_t *vfs_mnt_get(const char *path, const char **rel);
void vfs_mnt_put(vfs_mount_t *m);

/* The root mount (the filesystem at "/"), or NULL; not held. */
vfs_mount_t *vfs_mnt_root(void);

/* A handle was opened / finally closed through `m'.  Open handles keep a
 * mount from being unmounted; the close that releases the last one of a
 * lazily unmounted mount finishes the unmount. */
void vfs_mnt_file_opened(vfs_mount_t *m);
void vfs_mnt_file_closed(vfs_mount_t *m);

/* `path' made canonical (no "." or ".." components, no doubled slashes) in
 * `buf' (VFS_MAX_PATH bytes), or `path' itself when it already is. */
const char *vfs_canon(const char *path, char *buf);

/* `path' with every symbolic link on it followed -- the last one too with
 * `follow' -- into `out' (VFS_MAX_PATH bytes), as far as the path leads.
 * ST_OK, or ST_LOOP / ST_NAMETOOLONG when the links cannot be followed
 * (ST_LINK inside an operation that is about to start over elsewhere). */
int vfs_resolve(const char *path, int follow, char *out);

/* The mount table changed: whatever the VFS cached about paths is stale. */
void vfs_meta_changed(void);

/* Call `fn' for every live mount, each held for the duration of its call. */
void vfs_mnt_for_each(void (*fn)(vfs_mount_t *m, void *arg), void *arg);

/* The same for every filesystem instance that still exists: the live mounts
 * and the lazily unmounted ones waiting for their last file, and with
 * `dying' also those an unmount is waiting to tear down.  Each is held for
 * the duration of its call, so none is torn down under `fn'. */
void vfs_mnt_for_each_instance(void (*fn)(vfs_mount_t *m, void *arg),
			       void *arg, int dying);

/* Is there a mount (other than the root) exactly at canonical `path'? */
int vfs_mnt_is_point(const char *path);

/* Bracket a rename of canonical `oldpath' to `newpath'.  When mounts lie
 * below `oldpath' the mount mutex is taken (*locked set) and held until
 * vfs_mnt_rename_end(), which moves their mount points along when the
 * rename happened (`renamed'); ST_NAMETOOLONG (nothing held) when a moved
 * mount point would no longer fit.  Must be ended before the rename puts
 * the mounts it holds. */
int vfs_mnt_rename_begin(const char *oldpath, const char *newpath, int *locked);
void vfs_mnt_rename_end(int locked, const char *oldpath, const char *newpath,
			int renamed);

/* For a driver's fstype->unmount: drop everything the shared caches (pages,
 * inodes, names) hold for the instance whose page-cache superblock is `sb',
 * waiting out writeback already running on it.  After this nothing outside
 * the driver refers to the instance. */
void vfs_sb_release_caches(struct vfs_superblock *sb);

/* Option parsing for fstype->mount/remount: walks a comma-separated option
 * string.  Each call returns the next option split into *key and *val (val
 * NULL for a bare word), copied into `tmp' (cap bytes); 0 at the end.
 * `*cursor' starts at the string. */
int vfs_next_option(const char **cursor, char *tmp, unsigned cap,
		    const char **key, const char **val);

#endif /* KERNEL_FS_MOUNT_H */
