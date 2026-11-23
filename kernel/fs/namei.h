/*
 * Path resolution and permission checking for the syscall layer.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _KERNEL_FS_NAMEI_H
#define _KERNEL_FS_NAMEI_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/fs/vfs.h>
#include <kernel/uapi/stat.h>

int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
		  size_t out_size);
int canon_task_path(char *path, size_t size);
/* A real path (a descriptor's) as the task's chroot sees it; *inside 0 for
 * one outside the jail (then returned as it is). */
const char *jail_relative(task_t *cur, const char *real, int *inside);
/* A user path in a new path buffer (pathbuf.h; the caller puts it): as given,
 * or made absolute against the task's working directory and root.  0 with
 * *out set, or -errno with *out NULL. */
int user_path_get(uint64_t upath, char **out);
int user_path_get_canon(uint64_t upath, char **out);
unsigned int creat_mode(task_t *cur, uint64_t mode);
int devfs_open_perm(task_t *cur, const char *path, uint64_t flags);
int normalize_path(const char *base, const char *path, char *out,
		   size_t out_size);
/* The names in s[0, slen) (up to a NUL; slen may be (size_t)-1) applied to
 * the canonical absolute path out[0, *lenp) ("/" to start with), as
 * normalize_path() does: 0 or -ENAMETOOLONG. */
int normalize_path_append(char *out, size_t out_size, size_t *lenp,
			  const char *s, size_t slen);
int perm_access(task_t *cur, const char *path, const struct kstat *st,
		int want, int use_real);
int perm_traverse(const char *rawpath);
int perm_traverse_cred(const char *rawpath, int use_real);
unsigned setid_strip_bits(uint32_t mode);
void strip_setid_file(vfs_file_t *file);
int vfs_status_to_errno(int st);

#endif /* _KERNEL_FS_NAMEI_H */
