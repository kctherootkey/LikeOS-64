/* The per-task file-descriptor table. */
#ifndef _KERNEL_FS_FILE_H
#define _KERNEL_FS_FILE_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/fs/vfs.h>

vfs_file_t *fd_dup_entry(vfs_file_t *entry);
vfs_file_t *fd_dup_entry_at(task_t *cur, int fd);
int fd_install(task_t *task, vfs_file_t *file);
int fd_install_from(task_t *task, vfs_file_t *file, int from);
int fd_is_special(vfs_file_t *file);
void fds_lock(task_t *task, uint64_t *flags);
void fds_unlock(task_t *task, uint64_t flags);

#endif /* _KERNEL_FS_FILE_H */
