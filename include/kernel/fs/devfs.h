#ifndef _KERNEL_DEVFS_H_
#define _KERNEL_DEVFS_H_

#include <kernel/fs/vfs.h>
#include <kernel/uapi/stat.h>
#include <kernel/io/tty.h>

int devfs_init(void);
int devfs_open(const char *path, int flags, vfs_file_t **out);
int devfs_open_for_task(const char *path, int flags, vfs_file_t **out,
			task_t *cur);
int devfs_stat(const char *path, struct kstat *st);
/* /dev/fd/N + /dev/stdin/stdout/stderr: returns the caller-relative fd the
 * path names (open = dup, performed by the syscall layer), or -1. */
int devfs_fd_alias_target(const char *path);
int devfs_chdir(const char *path);
int devfs_close(vfs_file_t *f);
long devfs_read(vfs_file_t *f, void *buf, long bytes);
long devfs_write(vfs_file_t *f, const void *buf, long bytes);
int devfs_ioctl(vfs_file_t *f, unsigned long req, void *argp, task_t *cur);
int devfs_fstat(vfs_file_t *f, struct kstat *st);
/* /dev path an open devfs handle was opened under; -1 if not a devfs handle. */
int devfs_fpath(vfs_file_t *f, char *out, size_t cap);
int devfs_is_devfile(vfs_file_t *f);
int devfs_is_fb0(vfs_file_t *f);
/* POSIX shared memory object behind a /dev/shm handle, or NULL. */
struct shm_object *devfs_shm_object(vfs_file_t *f);
int devfs_evdev_unit(vfs_file_t *f); // event-device unit or -1
long devfs_seek(vfs_file_t *f, long offset, int whence);

// Helpers for syscall layer
const vfs_ops_t *devfs_get_ops(void);
tty_t *devfs_get_tty(vfs_file_t *f);
int devfs_get_pty_master_id(vfs_file_t *f);

#endif
