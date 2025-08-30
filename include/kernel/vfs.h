// LikeOS-64 - Minimal VFS interface (single mount, read-only)
#ifndef LIKEOS_VFS_H
#define LIKEOS_VFS_H

#include "status.h"

// Basic types (avoid hosted headers in freestanding build)
typedef unsigned long size_t;
typedef long ssize_t; // signed size for read return
typedef unsigned long uintptr_t;

#define VFS_MAX_PATH 256

typedef struct vfs_file vfs_file_t;

typedef struct {
    int (*open)(const char* path, vfs_file_t** out);
    long (*read)(vfs_file_t* f, void* buf, long bytes);
    int (*close)(vfs_file_t* f);
} vfs_ops_t;

struct vfs_file {
    const vfs_ops_t* ops;
    void* fs_private; // points to underlying FS-specific handle
};

int vfs_init(void);
int vfs_register_root(const vfs_ops_t* ops);
int vfs_open(const char* path, vfs_file_t** out);
long vfs_read(vfs_file_t* f, void* buf, long bytes);
int vfs_close(vfs_file_t* f);

#endif // LIKEOS_VFS_H
