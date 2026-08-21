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

/* Resolve a descriptor to the object it names, with a reference held, or NULL
 * if the descriptor is not open.  The result may be any of the kinds a slot
 * can hold -- ask fd_is_special() before treating it as a vfs_file_t.  EVERY
 * successful call must be paired with exactly one fdput(); see the block
 * comment in kernel/fs/file.c for why reading the slot directly is not safe in
 * a process that has threads. */
/* Copy one task's descriptor table into another, taking a reference on every
 * entry.  For fork: see the definition for why it takes two passes. */
void fd_table_clone(task_t *dst, task_t *src);

vfs_file_t *fdget(task_t *task, int fd);

/* Take a SECOND hold on an entry the caller already holds, for a reference
 * that must outlive the lookup which produced it.  Released with fdput() like
 * any other.  Returns 0 for an entry that names nothing. */
int fdhold(vfs_file_t *entry);

void fdput(vfs_file_t *entry);
void fds_unlock(task_t *task, uint64_t flags);

#endif /* _KERNEL_FS_FILE_H */
