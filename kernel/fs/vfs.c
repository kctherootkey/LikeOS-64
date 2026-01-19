// LikeOS-64 - Minimal VFS implementation
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

static const vfs_ops_t* g_root_ops = 0;

int vfs_init(void) { g_root_ops = 0; return ST_OK; }
int vfs_register_root(const vfs_ops_t* ops) { if (!ops) return ST_INVALID; g_root_ops = ops; return ST_OK; }
int vfs_open(const char* path, vfs_file_t** out) { if (!g_root_ops || !g_root_ops->open) return ST_UNSUPPORTED; return g_root_ops->open(path, out); }
long vfs_read(vfs_file_t* f, void* buf, long bytes) { if (!f || !f->ops || !f->ops->read) return ST_INVALID; return f->ops->read(f, buf, bytes); }
int vfs_close(vfs_file_t* f) { if (!f || !f->ops || !f->ops->close) return ST_INVALID; return f->ops->close(f); }

size_t vfs_size(vfs_file_t* f) {
    if (!f) return 0;
    // vfs_file_t is embedded as the first member of fat32_file_t
    // so we can cast directly (or use fs_private which points to same)
    fat32_file_t* ff = (fat32_file_t*)f;
    return ff->size;
}
