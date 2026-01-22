// LikeOS-64 - Minimal VFS interface (single mount, read-only)
#ifndef LIKEOS_VFS_H
#define LIKEOS_VFS_H

#include "status.h"
#include "stat.h"

// Basic types (avoid hosted headers in freestanding build)
typedef unsigned long size_t;
typedef long ssize_t; // signed size for read return
typedef unsigned long uintptr_t;

#define VFS_MAX_PATH 256

typedef struct vfs_file vfs_file_t;

typedef struct {
    int (*open)(const char* path, int flags, vfs_file_t** out);
    int (*stat)(const char* path, struct kstat* st);
    long (*read)(vfs_file_t* f, void* buf, long bytes);
    long (*write)(vfs_file_t* f, const void* buf, long bytes);
    long (*seek)(vfs_file_t* f, long offset, int whence);
    long (*readdir)(vfs_file_t* f, void* buf, long bytes);
    int (*truncate)(vfs_file_t* f, unsigned long size);
    int (*unlink)(const char* path);
    int (*rename)(const char* oldpath, const char* newpath);
    int (*mkdir)(const char* path, unsigned int mode);
    int (*rmdir)(const char* path);
    int (*chdir)(const char* path);
    int (*close)(vfs_file_t* f);
} vfs_ops_t;

struct vfs_file {
    const vfs_ops_t* ops;
    void* fs_private; // points to underlying FS-specific handle
    int refcount;     // Reference count for dup/fork
    int flags;        // O_CLOEXEC, O_RDONLY, etc.
};

// File descriptor flags
#define FD_CLOEXEC  0x1

int vfs_init(void);
int vfs_register_root(const vfs_ops_t* ops);
int vfs_register_devfs(const vfs_ops_t* ops);
int vfs_root_ready(void);
int vfs_open(const char* path, int flags, vfs_file_t** out);
int vfs_stat(const char* path, struct kstat* st);
int vfs_chdir(const char* path);
long vfs_read(vfs_file_t* f, void* buf, long bytes);
long vfs_write(vfs_file_t* f, const void* buf, long bytes);
long vfs_seek(vfs_file_t* f, long offset, int whence);
long vfs_readdir(vfs_file_t* f, void* buf, long bytes);
int vfs_truncate(vfs_file_t* f, unsigned long size);
int vfs_unlink(const char* path);
int vfs_rename(const char* oldpath, const char* newpath);
int vfs_mkdir(const char* path, unsigned int mode);
int vfs_rmdir(const char* path);
int vfs_close(vfs_file_t* f);
size_t vfs_size(vfs_file_t* f);
vfs_file_t* vfs_dup(vfs_file_t* f);  // Increment refcount and return same pointer
void vfs_incref(vfs_file_t* f);      // Increment refcount

#endif // LIKEOS_VFS_H
