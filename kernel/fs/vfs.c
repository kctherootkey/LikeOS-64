// LikeOS-64 - Minimal VFS implementation
#include <kernel/fs/vfs.h>
#include <kernel/fs/fat32.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/bug.h>

static const vfs_ops_t* g_root_ops = 0;
static const vfs_ops_t* g_dev_ops = 0;

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
    BUG_ON(path == NULL);
    BUG_ON(out == NULL);
    if (vfs_is_dev_path(path)) {
        if (!g_dev_ops || !g_dev_ops->open) return ST_UNSUPPORTED;
        int ret = g_dev_ops->open(path, flags, out);
        if (ret == ST_OK && *out) {
            WARN_ON(*out == NULL);
            WARN_ON((*out)->ops == NULL);
            (*out)->refcount = 1;
            (*out)->flags = flags;
            (*out)->is_root_dir = 0;
            (*out)->dev_injected = 0;
        }
        WARN_ON(ret == ST_OK && *out == NULL);
        return ret;
    }

    if (!g_root_ops || !g_root_ops->open) return ST_UNSUPPORTED;
    int ret = g_root_ops->open(path, flags, out);
    if (ret == ST_OK && *out) {
        WARN_ON(*out == NULL);
        WARN_ON((*out)->ops == NULL);
        (*out)->refcount = 1;
        (*out)->flags = flags;
        (*out)->is_root_dir = vfs_is_root_path(path);
        (*out)->dev_injected = 0;
    }
    WARN_ON(ret == ST_OK && *out == NULL);
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

/* Pick the filesystem that owns a path: devfs for /dev*, otherwise the root.
 * (Single-root today; a future mount table would resolve the mountpoint here,
 * leaving every caller below unchanged.) */
static const vfs_ops_t* vfs_ops_for_path(const char* path) {
    return vfs_is_dev_path(path) ? g_dev_ops : g_root_ops;
}

/* ---- UNIX-semantics wrappers ------------------------------------------------
 * Each dispatches to the owning filesystem's op and supplies a legacy fallback
 * when that op is NULL, so the syscall layer stays filesystem-agnostic. */

int vfs_lstat(const char* path, struct kstat* st) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o) return ST_UNSUPPORTED;
    if (o->lstat) return o->lstat(path, st);
    if (o->stat)  return o->stat(path, st);   /* no symlinks: lstat == stat */
    return ST_UNSUPPORTED;
}

int vfs_symlink(const char* target, const char* linkpath) {
    const vfs_ops_t* o = vfs_ops_for_path(linkpath);
    if (!o || !o->symlink) return ST_UNSUPPORTED;
    return o->symlink(target, linkpath);
}

int vfs_readlink(const char* path, char* buf, unsigned long bufsz) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o || !o->readlink) return ST_INVALID;   /* not a symlink here */
    return o->readlink(path, buf, bufsz);
}

int vfs_link(const char* oldpath, const char* newpath) {
    /* A hard link's two ends share an inode, so both live on one filesystem;
     * route by the new name's owning fs. */
    const vfs_ops_t* o = vfs_ops_for_path(newpath);
    if (!o || !o->link) return ST_UNSUPPORTED;
    return o->link(oldpath, newpath);
}

int vfs_chmod(const char* path, unsigned int mode) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o) return ST_UNSUPPORTED;
    if (!o->chmod) return ST_OK;                 /* fs has no permission bits */
    return o->chmod(path, mode);
}

int vfs_chown(const char* path, int uid, int gid) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o) return ST_UNSUPPORTED;
    if (!o->chown) return ST_OK;                 /* fs has no ownership */
    return o->chown(path, uid, gid);
}

int vfs_fchmod(vfs_file_t* f, unsigned int mode) {
    if (!f || !f->ops) return ST_INVALID;
    if (!f->ops->fchmod) return ST_OK;
    return f->ops->fchmod(f, mode);
}

int vfs_fchown(vfs_file_t* f, int uid, int gid) {
    if (!f || !f->ops) return ST_INVALID;
    if (!f->ops->fchown) return ST_OK;
    return f->ops->fchown(f, uid, gid);
}

int vfs_utimensat(const char* path, int64_t mtime_sec, long mtime_nsec) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o) return ST_UNSUPPORTED;
    if (!o->utimensat) return ST_OK;             /* fs manages times itself */
    return o->utimensat(path, mtime_sec, mtime_nsec);
}

int vfs_statfs(const char* path, struct vfs_statfs* out) {
    const vfs_ops_t* o = vfs_ops_for_path(path);
    if (!o || !o->statfs) return ST_UNSUPPORTED;
    return o->statfs(out);
}

int vfs_fstatfs(vfs_file_t* f, struct vfs_statfs* out) {
    if (!f || !f->ops) return ST_INVALID;
    if (!f->ops->statfs) return ST_UNSUPPORTED;
    return f->ops->statfs(out);
}

int vfs_fstat(vfs_file_t* f, struct kstat* st) {
    if (!f || !f->ops) return ST_INVALID;
    if (!f->ops->fstat) return ST_UNSUPPORTED;   /* fs can't report fd owner */
    return f->ops->fstat(f, st);
}

int vfs_setid_clean(vfs_file_t* f) {
    if (f && f->ops && f->ops->setid_clean) return f->ops->setid_clean(f, 0);
    return 1;   /* fs has no set-id bits: report clean so writes never strip */
}

void vfs_mark_setid_clean(vfs_file_t* f) {
    if (f && f->ops && f->ops->setid_clean) f->ops->setid_clean(f, 1);
}

long vfs_read(vfs_file_t* f, void* buf, long bytes) { if (!f || !f->ops || !f->ops->read) return ST_INVALID; return f->ops->read(f, buf, bytes); }
long vfs_write(vfs_file_t* f, const void* buf, long bytes) { if (!f || !f->ops || !f->ops->write) return ST_INVALID; return f->ops->write(f, buf, bytes); }
long vfs_seek(vfs_file_t* f, long offset, int whence) { if (!f || !f->ops || !f->ops->seek) return -1; return f->ops->seek(f, offset, whence); }

long vfs_readdir(vfs_file_t* f, void* buf, long bytes) {
    VM_BUG_ON(f == NULL);
    VM_BUG_ON(buf == NULL);
    if (!f || !f->ops || !f->ops->readdir) return ST_UNSUPPORTED;
    
    unsigned char* out = (unsigned char*)buf;
    long total = 0;
    
    // If this is the root directory and we haven't injected /dev yet, inject it first
    if (f->is_root_dir && !f->dev_injected && g_dev_ops) {
        // Calculate size for "dev" entry
        unsigned short reclen = (unsigned short)(sizeof(struct linux_dirent64) + 4); // "dev" + null
        reclen = (reclen + 7) & ~7;  // Align to 8 bytes
        WARN_ON(reclen % 8 != 0);
        
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

void vfs_release_locks_for_task(uint64_t task_id) {
    if (g_root_ops && g_root_ops->release_locks_for_task)
        g_root_ops->release_locks_for_task(task_id);
    if (g_dev_ops && g_dev_ops->release_locks_for_task)
        g_dev_ops->release_locks_for_task(task_id);
}

int vfs_close(vfs_file_t* f) {
    BUG_ON(f == NULL);
     if (!f || !f->ops || !f->ops->close) return ST_INVALID;
    
    // Atomically decrement refcount; only the thread that transitions 1→0 closes
    int old = __sync_fetch_and_sub(&f->refcount, 1);
    WARN_ON(old < 0);
    WARN_ON_ONCE(old > 65536);  /* refcount suspiciously large: vfs_dup/vfs_incref without matching vfs_close */
    if (old > 1) {
        return ST_OK;
    }

    // Guard against refcount underflow (double-close).
    // If old <= 0, someone already closed this file; undo the decrement and bail.
    if (old <= 0) {
        __sync_fetch_and_add(&f->refcount, 1);  // undo
        WARN(1, "vfs_close refcount underflow on %p (old=%d)", f, old);
        kprintf("vfs_close: BUG refcount underflow on %p (old=%d)\n", f, old);
        return ST_INVALID;
    }
    
    // Actually close when refcount reaches 0 (old was 1, now 0)
    return f->ops->close(f);
}

// Duplicate file descriptor - increment refcount
vfs_file_t* vfs_dup(vfs_file_t* f) {
    BUG_ON(f == NULL);
    if (!f) return NULL;
    WARN_ON(f->refcount <= 0);  /* duplicating a file with zero/negative refcount: file was already closed */

    __sync_fetch_and_add(&f->refcount, 1);
    return f;
}

// Just increment refcount
void vfs_incref(vfs_file_t* f) {
    if (f) __sync_fetch_and_add(&f->refcount, 1);
}

size_t vfs_size(vfs_file_t* f) {
    if (!f) return 0;
    WARN_ON(f->ops == NULL);
    // vfs_file_t is embedded as the first member of fat32_file_t
    // so we can cast directly (or use fs_private which points to same)
    fat32_file_t* ff = (fat32_file_t*)f;
    return ff->size;
}
