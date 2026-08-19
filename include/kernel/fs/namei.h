/* Path resolution and permission checking for the syscall layer. */
#ifndef _KERNEL_FS_NAMEI_H
#define _KERNEL_FS_NAMEI_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/fs/vfs.h>
#include <kernel/uapi/stat.h>

int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
			 size_t out_size);
int canon_task_path(char *path, size_t size);
unsigned int creat_mode(task_t *cur, uint64_t mode);
int devfs_open_perm(task_t *cur, const char *path, uint64_t flags);
int normalize_path(const char *base, const char *path, char *out,
			  size_t out_size);
int perm_access(task_t *cur, const char *path, const struct kstat *st,
		       int want, int use_real);
int perm_traverse(const char *rawpath);
int perm_traverse_cred(const char *rawpath, int use_real);
unsigned setid_strip_bits(uint32_t mode);
void strip_setid_file(vfs_file_t *file);
int vfs_status_to_errno(int st);

#endif /* _KERNEL_FS_NAMEI_H */
