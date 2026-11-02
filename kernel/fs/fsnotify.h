// LikeOS -- filesystem change notification: what the VFS reports.
//
// The VFS calls these after an operation SUCCEEDED; the watch tables
// (kernel/fs/inotify.c) decide whether anybody is listening.  An event names
// its subject twice: by identity -- the filesystem and inode number, so a
// watch on a file sees it under every one of its names -- and by path, so a
// watch on the directory holding that name sees it with the name attached.
//
// Nothing here is free to call, but none of it costs anything while nothing
// is watched: fsnotify_wants() is one load of the union of every watch's
// mask, and the VFS consults it before resolving an identity.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_FSNOTIFY_H
#define KERNEL_FS_FSNOTIFY_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/inotify.h>

/* The union of the event bits of every live watch.  Zero when nothing is
 * watched. */
extern uint32_t g_fsnotify_mask;

static inline int fsnotify_wants(uint32_t mask)
{
	return (__atomic_load_n(&g_fsnotify_mask, __ATOMIC_RELAXED) & mask) !=
	       0;
}

/* Who an event is about.  `fs' is the operations table of the filesystem
 * that owns the object -- the only key that tells one filesystem from
 * another, as the disk filesystems all report st_dev 0. */
struct fsnotify_ident {
	const void *fs;
	uint64_t ino;
	uint32_t nlink;
	uint8_t isdir;
	uint8_t valid;
};

/* Report `mask' about the object `id' (may be NULL or invalid: no watch on
 * the object itself is told) that is named `path' (may be NULL: no watch on
 * the containing directory is told).
 *
 * The bits are split the conventional way: IN_CREATE, IN_DELETE, IN_MOVED_*
 * only go to the directory; IN_DELETE_SELF, IN_MOVE_SELF only to the object;
 * everything else to both.  `cookie' pairs IN_MOVED_FROM with IN_MOVED_TO.
 * When `check_name' is set the directory is told only if `path' still names
 * `id' -- for events found through an open descriptor, whose path is the
 * one it was opened with and may have been renamed away since.
 * `id->isdir' is honoured even when the identity is not valid: an event
 * that only goes to the directory needs to know no more than that. */
void fsnotify_event(const char *path, const struct fsnotify_ident *id,
		    uint32_t mask, uint32_t cookie, int check_name);

/* The object's last name is gone: IN_DELETE_SELF, then IN_IGNORED, to every
 * watch on it, and the watches are removed. */
void fsnotify_gone(const struct fsnotify_ident *id);

/* A directory was renamed: watches recorded under `oldpath' or below it are
 * now under `newpath'. */
void fsnotify_renamed(const char *oldpath, const char *newpath);

/* A fresh cookie for one rename. */
uint32_t fsnotify_cookie(void);

/* Implemented by the VFS: who `path' is (lstat when !follow), without any
 * permission check.  ST_OK or the lookup's status. */
int vfs_notify_ident(const char *path, int follow, struct fsnotify_ident *id);

#endif
