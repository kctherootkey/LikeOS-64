// LikeOS-64 - Minimal VFS implementation
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/dirent.h"
#include "../../include/kernel/stat.h"

static const vfs_ops_t* g_root_ops = 0;
static const vfs_ops_t* g_dev_ops = 0;

/* ================================================================== */
/*  VFS file cache — keeps /lib/ shared libraries in kernel RAM       */
/*  so repeated exec() calls skip all FAT32 disk I/O.                 */
/* ================================================================== */

#define VFS_CACHE_MAX      8
#define VFS_CACHE_MAX_SIZE (512 * 1024)   /* 512KB per file max */
#define VFS_CACHE_PATH_LEN 128

typedef struct {
    char     path[VFS_CACHE_PATH_LEN];
    void    *data;
    size_t   size;
    int      valid;
} vfs_cache_entry_t;

static vfs_cache_entry_t vfs_fcache[VFS_CACHE_MAX];

/* Cached-file handle — reads from kernel memory, no disk I/O */
typedef struct {
    vfs_file_t  vfs;    /* must be first for casting */
    const void *data;
    size_t      size;
    size_t      pos;
} vfs_cached_file_t;

/* Forward declarations for cached-file ops */
static long vfs_cached_read(vfs_file_t* f, void* buf, long bytes);
static long vfs_cached_write(vfs_file_t* f, const void* buf, long bytes);
static long vfs_cached_seek(vfs_file_t* f, long offset, int whence);
static int  vfs_cached_close(vfs_file_t* f);

static const vfs_ops_t vfs_cached_ops = {
    0,                   /* open     */
    0,                   /* stat     */
    vfs_cached_read,     /* read     */
    vfs_cached_write,    /* write    */
    vfs_cached_seek,     /* seek     */
    0,                   /* readdir  */
    0,                   /* truncate */
    0,                   /* unlink   */
    0,                   /* rename   */
    0,                   /* mkdir    */
    0,                   /* rmdir    */
    0,                   /* chdir    */
    vfs_cached_close     /* close    */
};

static long vfs_cached_read(vfs_file_t* f, void* buf, long bytes) {
    vfs_cached_file_t* cf = (vfs_cached_file_t*)f;
    if (cf->pos >= cf->size) return 0;
    if (bytes < 0) return ST_INVALID;
    size_t avail = cf->size - cf->pos;
    if ((size_t)bytes > avail) bytes = (long)avail;
    smap_disable();
    mm_memcpy(buf, (const uint8_t*)cf->data + cf->pos, (size_t)bytes);
    smap_enable();
    cf->pos += (size_t)bytes;
    return bytes;
}

static long vfs_cached_write(vfs_file_t* f, const void* buf, long bytes) {
    (void)f; (void)buf; (void)bytes;
    return ST_UNSUPPORTED;   /* cached files are read-only */
}

static long vfs_cached_seek(vfs_file_t* f, long offset, int whence) {
    vfs_cached_file_t* cf = (vfs_cached_file_t*)f;
    long new_pos;
    switch (whence) {
    case 0: new_pos = offset; break;                           /* SEEK_SET */
    case 1: new_pos = (long)cf->pos  + offset; break;         /* SEEK_CUR */
    case 2: new_pos = (long)cf->size + offset; break;         /* SEEK_END */
    default: return -1;
    }
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (long)cf->size) new_pos = (long)cf->size;
    cf->pos = (size_t)new_pos;
    return new_pos;
}

static int vfs_cached_close(vfs_file_t* f) {
    kfree(f);   /* data stays in cache; only handle is freed */
    return ST_OK;
}

static int vfs_is_lib_path(const char* path) {
    return path && path[0] == '/' && path[1] == 'l' &&
           path[2] == 'i' && path[3] == 'b' && path[4] == '/';
}

static vfs_cache_entry_t* vfs_cache_find(const char* path) {
    for (int i = 0; i < VFS_CACHE_MAX; i++)
        if (vfs_fcache[i].valid && kstrcmp(vfs_fcache[i].path, path) == 0)
            return &vfs_fcache[i];
    return 0;
}

/* Read the whole file from the real FS and store it in the cache */
static vfs_cache_entry_t* vfs_cache_populate(const char* path) {
    if (!g_root_ops || !g_root_ops->open) return 0;

    vfs_file_t* file = 0;
    int ret = g_root_ops->open(path, 0, &file);
    if (ret != ST_OK || !file) return 0;

    fat32_file_t* ff = (fat32_file_t*)file;
    size_t sz = ff->size;
    if (sz == 0 || sz > VFS_CACHE_MAX_SIZE) {
        g_root_ops->close(file);
        return 0;
    }

    void* data = kalloc(sz);
    if (!data) { g_root_ops->close(file); return 0; }

    long rd = g_root_ops->read(file, data, (long)sz);
    g_root_ops->close(file);
    if (rd != (long)sz) { kfree(data); return 0; }

    /* Find a free slot */
    vfs_cache_entry_t* slot = 0;
    for (int i = 0; i < VFS_CACHE_MAX; i++)
        if (!vfs_fcache[i].valid) { slot = &vfs_fcache[i]; break; }
    if (!slot) { kfree(data); return 0; }   /* cache full */

    int pi = 0;
    while (path[pi] && pi < VFS_CACHE_PATH_LEN - 1) { slot->path[pi] = path[pi]; pi++; }
    slot->path[pi] = '\0';
    slot->data  = data;
    slot->size  = sz;
    slot->valid = 1;
    return slot;
}

/* Create a lightweight handle that reads from a cached entry */
static vfs_file_t* vfs_cache_make_handle(vfs_cache_entry_t* ent) {
    vfs_cached_file_t* cf = (vfs_cached_file_t*)kalloc(sizeof(*cf));
    if (!cf) return 0;
    mm_memset(cf, 0, sizeof(*cf));
    cf->vfs.ops      = &vfs_cached_ops;
    cf->vfs.refcount  = 1;
    cf->data = ent->data;
    cf->size = ent->size;
    cf->pos  = 0;
    return &cf->vfs;
}

/* ================================================================== */

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

static int vfs_is_root_path(const char* path) {
    if (!path) return 0;
    // "/" or "/.." or "/." all resolve to root
    if (path[0] == '/' && path[1] == '\0') return 1;
    return 0;
}

int vfs_open(const char* path, int flags, vfs_file_t** out) {
    if (vfs_is_dev_path(path)) {
        if (!g_dev_ops || !g_dev_ops->open) return ST_UNSUPPORTED;
        int ret = g_dev_ops->open(path, flags, out);
        if (ret == ST_OK && *out) {
            (*out)->refcount = 1;
            (*out)->flags = flags;
            (*out)->is_root_dir = 0;
            (*out)->dev_injected = 0;
        }
        return ret;
    }

    /* Check VFS file cache for /lib/ files (shared libraries) */
    if (vfs_is_lib_path(path) && flags == 0) {
        vfs_cache_entry_t* ent = vfs_cache_find(path);
        if (!ent) ent = vfs_cache_populate(path);
        if (ent) {
            vfs_file_t* cf = vfs_cache_make_handle(ent);
            if (cf) {
                cf->flags = flags;
                cf->is_root_dir = 0;
                cf->dev_injected = 0;
                *out = cf;
                return ST_OK;
            }
        }
        /* Fall through to normal open if cache failed */
    }

    if (!g_root_ops || !g_root_ops->open) return ST_UNSUPPORTED;
    int ret = g_root_ops->open(path, flags, out);
    if (ret == ST_OK && *out) {
        (*out)->refcount = 1;
        (*out)->flags = flags;
        (*out)->is_root_dir = vfs_is_root_path(path);
        (*out)->dev_injected = 0;
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

long vfs_readdir(vfs_file_t* f, void* buf, long bytes) {
    if (!f || !f->ops || !f->ops->readdir) return ST_UNSUPPORTED;
    
    unsigned char* out = (unsigned char*)buf;
    long total = 0;
    
    // If this is the root directory and we haven't injected /dev yet, inject it first
    if (f->is_root_dir && !f->dev_injected && g_dev_ops) {
        // Calculate size for "dev" entry
        unsigned short reclen = (unsigned short)(sizeof(struct linux_dirent64) + 4); // "dev" + null
        reclen = (reclen + 7) & ~7;  // Align to 8 bytes
        
        if (bytes >= reclen) {
            // Build entry in kernel buffer first, then copy to user
            struct linux_dirent64 ent;
            ent.d_ino = 2;  // Fake inode for /dev
            ent.d_off = reclen;
            ent.d_reclen = reclen;
            ent.d_type = DT_DIR;
            ent.d_name[0] = 'd';
            ent.d_name[1] = 'e';
            ent.d_name[2] = 'v';
            ent.d_name[3] = '\0';
            
            // SMAP-aware copy to user buffer
            smap_disable();
            mm_memcpy(out, &ent, sizeof(ent));
            smap_enable();
            
            out += reclen;
            bytes -= reclen;
            total += reclen;
            f->dev_injected = 1;
        }
    }
    
    // Now get remaining entries from underlying FS
    long ret = f->ops->readdir(f, out, bytes);
    if (ret > 0) {
        total += ret;
    } else if (ret < 0 && total == 0) {
        return ret;  // Error and no /dev was injected
    }
    
    return total;
}

int vfs_truncate(vfs_file_t* f, unsigned long size) { if (!f || !f->ops || !f->ops->truncate) return ST_UNSUPPORTED; return f->ops->truncate(f, size); }
int vfs_unlink(const char* path) { if (!g_root_ops || !g_root_ops->unlink) return ST_UNSUPPORTED; return g_root_ops->unlink(path); }
int vfs_rename(const char* oldpath, const char* newpath) { if (!g_root_ops || !g_root_ops->rename) return ST_UNSUPPORTED; return g_root_ops->rename(oldpath, newpath); }
int vfs_mkdir(const char* path, unsigned int mode) { if (!g_root_ops || !g_root_ops->mkdir) return ST_UNSUPPORTED; return g_root_ops->mkdir(path, mode); }
int vfs_rmdir(const char* path) { if (!g_root_ops || !g_root_ops->rmdir) return ST_UNSUPPORTED; return g_root_ops->rmdir(path); }

int vfs_close(vfs_file_t* f) {
     if (!f || !f->ops || !f->ops->close) return ST_INVALID;
    
    // Atomically decrement refcount; only the thread that transitions 1→0 closes
    int old = __sync_fetch_and_sub(&f->refcount, 1);
    if (old > 1) {
        return ST_OK;
    }
    
    // Actually close when refcount reaches 0
    return f->ops->close(f);
}

// Duplicate file descriptor - increment refcount
vfs_file_t* vfs_dup(vfs_file_t* f) {
    if (!f) return NULL;

    __sync_fetch_and_add(&f->refcount, 1);
    return f;
}

// Just increment refcount
void vfs_incref(vfs_file_t* f) {
    if (f) __sync_fetch_and_add(&f->refcount, 1);
}

size_t vfs_size(vfs_file_t* f) {
    if (!f) return 0;
    /* Cached files have a different layout */
    if (f->ops == &vfs_cached_ops) {
        vfs_cached_file_t* cf = (vfs_cached_file_t*)f;
        return cf->size;
    }
    // vfs_file_t is embedded as the first member of fat32_file_t
    // so we can cast directly (or use fs_private which points to same)
    fat32_file_t* ff = (fat32_file_t*)f;
    return ff->size;
}
