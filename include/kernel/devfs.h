#ifndef _KERNEL_DEVFS_H_
#define _KERNEL_DEVFS_H_

#include "vfs.h"
#include "stat.h"
#include "tty.h"

int devfs_init(void);
int devfs_open(const char* path, int flags, vfs_file_t** out);
int devfs_open_for_task(const char* path, int flags, vfs_file_t** out, task_t* cur);
int devfs_stat(const char* path, struct kstat* st);
int devfs_chdir(const char* path);
int devfs_close(vfs_file_t* f);
long devfs_read(vfs_file_t* f, void* buf, long bytes);
long devfs_write(vfs_file_t* f, const void* buf, long bytes);
int devfs_ioctl(vfs_file_t* f, unsigned long req, void* argp, task_t* cur);
int devfs_fstat(vfs_file_t* f, struct kstat* st);
int devfs_is_devfile(vfs_file_t* f);

// Helpers for syscall layer
const vfs_ops_t* devfs_get_ops(void);
tty_t* devfs_get_tty(vfs_file_t* f);

#endif
