// LikeOS-64 - Minimal VFS implementation
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

static const vfs_ops_t* g_root_ops = 0;

int vfs_init(void) { g_root_ops = 0; return ST_OK; }
int vfs_register_root(const vfs_ops_t* ops) { if (!ops) return ST_INVALID; g_root_ops = ops; return ST_OK; }

int vfs_open(const char* path, int flags, vfs_file_t** out) {
    if (!g_root_ops || !g_root_ops->open) return ST_UNSUPPORTED;
    int ret = g_root_ops->open(path, flags, out);
    if (ret == ST_OK && *out) {
        (*out)->refcount = 1;
        (*out)->flags = flags;
    }
    return ret;
}

long vfs_read(vfs_file_t* f, void* buf, long bytes) { if (!f || !f->ops || !f->ops->read) return ST_INVALID; return f->ops->read(f, buf, bytes); }
long vfs_write(vfs_file_t* f, const void* buf, long bytes) { if (!f || !f->ops || !f->ops->write) return ST_INVALID; return f->ops->write(f, buf, bytes); }
long vfs_seek(vfs_file_t* f, long offset, int whence) { if (!f || !f->ops || !f->ops->seek) return -1; return f->ops->seek(f, offset, whence); }
int vfs_truncate(vfs_file_t* f, unsigned long size) { if (!f || !f->ops || !f->ops->truncate) return ST_UNSUPPORTED; return f->ops->truncate(f, size); }

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
