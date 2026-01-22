// LikeOS-64 - Minimal VFS implementation
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

static const vfs_ops_t* g_root_ops = 0;
static const vfs_ops_t* g_dev_ops = 0;

int vfs_init(void) { g_root_ops = 0; g_dev_ops = 0; return ST_OK; }
int vfs_register_root(const vfs_ops_t* ops) { if (!ops) return ST_INVALID; g_root_ops = ops; return ST_OK; }
int vfs_register_devfs(const vfs_ops_t* ops) { if (!ops) return ST_INVALID; g_dev_ops = ops; return ST_OK; }
int vfs_root_ready(void) { return g_root_ops != 0; }

static int vfs_is_dev_path(const char* path) {
    if (!path) return 0;
    if (path[0] != '/' || path[1] != 'd' || path[2] != 'e' || path[3] != 'v') return 0;
    if (path[4] == '\0' || path[4] == '/') return 1;
    return 0;
}

int vfs_open(const char* path, int flags, vfs_file_t** out) {
    if (vfs_is_dev_path(path)) {
        if (!g_dev_ops || !g_dev_ops->open) return ST_UNSUPPORTED;
        int ret = g_dev_ops->open(path, flags, out);
        if (ret == ST_OK && *out) {
            (*out)->refcount = 1;
            (*out)->flags = flags;
        }
        return ret;
    }
    if (!g_root_ops || !g_root_ops->open) return ST_UNSUPPORTED;
    int ret = g_root_ops->open(path, flags, out);
    if (ret == ST_OK && *out) {
        (*out)->refcount = 1;
        (*out)->flags = flags;
    }
    return ret;
}

int vfs_stat(const char* path, struct kstat* st) {
    if (vfs_is_dev_path(path)) {
        if (!g_dev_ops || !g_dev_ops->stat) return ST_UNSUPPORTED;
        return g_dev_ops->stat(path, st);
    }
    if (!g_root_ops || !g_root_ops->stat) return ST_UNSUPPORTED;
    return g_root_ops->stat(path, st);
}

int vfs_chdir(const char* path) {
    if (vfs_is_dev_path(path)) {
        if (!g_dev_ops || !g_dev_ops->chdir) return ST_UNSUPPORTED;
        return g_dev_ops->chdir(path);
    }
    if (!g_root_ops || !g_root_ops->chdir) return ST_UNSUPPORTED;
    return g_root_ops->chdir(path);
}

long vfs_read(vfs_file_t* f, void* buf, long bytes) { if (!f || !f->ops || !f->ops->read) return ST_INVALID; return f->ops->read(f, buf, bytes); }
long vfs_write(vfs_file_t* f, const void* buf, long bytes) { if (!f || !f->ops || !f->ops->write) return ST_INVALID; return f->ops->write(f, buf, bytes); }
long vfs_seek(vfs_file_t* f, long offset, int whence) { if (!f || !f->ops || !f->ops->seek) return -1; return f->ops->seek(f, offset, whence); }
long vfs_readdir(vfs_file_t* f, void* buf, long bytes) { if (!f || !f->ops || !f->ops->readdir) return ST_UNSUPPORTED; return f->ops->readdir(f, buf, bytes); }
int vfs_truncate(vfs_file_t* f, unsigned long size) { if (!f || !f->ops || !f->ops->truncate) return ST_UNSUPPORTED; return f->ops->truncate(f, size); }
int vfs_unlink(const char* path) { if (!g_root_ops || !g_root_ops->unlink) return ST_UNSUPPORTED; return g_root_ops->unlink(path); }
int vfs_rename(const char* oldpath, const char* newpath) { if (!g_root_ops || !g_root_ops->rename) return ST_UNSUPPORTED; return g_root_ops->rename(oldpath, newpath); }
int vfs_mkdir(const char* path, unsigned int mode) { if (!g_root_ops || !g_root_ops->mkdir) return ST_UNSUPPORTED; return g_root_ops->mkdir(path, mode); }
int vfs_rmdir(const char* path) { if (!g_root_ops || !g_root_ops->rmdir) return ST_UNSUPPORTED; return g_root_ops->rmdir(path); }

int vfs_close(vfs_file_t* f) {
    if (!f || !f->ops || !f->ops->close) return ST_INVALID;
    
    // Decrement refcount
    if (f->refcount > 1) {
        f->refcount--;
        return ST_OK;
    }
    
    // Actually close when refcount reaches 0
    return f->ops->close(f);
}

// Duplicate file descriptor - increment refcount
vfs_file_t* vfs_dup(vfs_file_t* f) {
    if (!f) return NULL;
    f->refcount++;
    return f;
}

// Just increment refcount
void vfs_incref(vfs_file_t* f) {
    if (f) f->refcount++;
}

size_t vfs_size(vfs_file_t* f) {
    if (!f) return 0;
    // vfs_file_t is embedded as the first member of fat32_file_t
    // so we can cast directly (or use fs_private which points to same)
    fat32_file_t* ff = (fat32_file_t*)f;
    return ff->size;
}
