// LikeOS-64 - Minimal VFS interface (single mount, read-only)
#ifndef LIKEOS_VFS_H
#define LIKEOS_VFS_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/stat.h>

// Basic types (avoid hosted headers in freestanding build)
typedef unsigned long size_t;
typedef long ssize_t; // signed size for read return
typedef unsigned long uintptr_t;

#define VFS_MAX_PATH 256

/* utimensat() nanosecond sentinels shared between the syscall layer and the
 * filesystem drivers' utimensat op (values match the userspace UTIME_NOW /
 * UTIME_OMIT constants so no translation is needed). */
#define VFS_UTIME_NOW   ((long)1073741823L)
#define VFS_UTIME_OMIT  ((long)1073741822L)

typedef struct vfs_file vfs_file_t;

/* Whole-filesystem statistics, filled by the statfs op.  Mirrors the fields
 * the statfs/fstatfs syscalls expose to userspace, but is filesystem-agnostic
 * so the syscall layer never references a specific driver's struct. */
struct vfs_statfs {
    unsigned long f_type;     /* filesystem type magic                       */
    unsigned long f_bsize;    /* optimal transfer block size                 */
    unsigned long f_frsize;   /* fragment size                               */
    unsigned long f_blocks;   /* total data blocks                           */
    unsigned long f_bfree;    /* free blocks                                 */
    unsigned long f_bavail;   /* free blocks available to unprivileged users */
    unsigned long f_files;    /* total file nodes                            */
    unsigned long f_ffree;    /* free file nodes                             */
    unsigned long f_fsid;     /* filesystem id (not meaningful here)         */
    unsigned long f_namelen;  /* maximum filename length                     */
};

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
    /* Force-release any per-filesystem locks owned by the given task.
     * Called from the scheduler's dead-thread reaper when a task that was
     * killed (e.g. via SIGINT) may have died inside a syscall while holding
     * filesystem-private locks.  Optional; may be NULL. */
    int (*release_locks_for_task)(uint64_t task_id);
    /* Flush a file's dirty data + metadata to disk (fsync).  Optional; when
     * NULL the syscall layer treats the flush as a no-op for this fs.  Lets
     * a filesystem whose cache key differs from the raw FAT32 start_cluster
     * (e.g. ext4) flush correctly. */
    int (*fsync)(vfs_file_t* f);

    /* ---- Optional UNIX-semantics operations -------------------------------
     * A filesystem lacking a given capability leaves the slot NULL; the
     * matching vfs_* wrapper then applies a legacy fallback (see vfs.c) so the
     * syscall layer never needs to know which filesystem is mounted. */

    /* Like stat(), but does NOT follow a final symlink.  NULL => no symlinks
     * on this fs, so the wrapper falls back to stat(). */
    int (*lstat)(const char* path, struct kstat* st);
    /* Create a symbolic link `linkpath` -> `target`.  NULL => unsupported. */
    int (*symlink)(const char* target, const char* linkpath);
    /* Read a symlink's target into buf (no NUL).  Returns byte count (>=0) or
     * a negative ST_ code.  NULL => path is not a symlink on this fs. */
    int (*readlink)(const char* path, char* buf, unsigned long bufsz);
    /* Create a hard link `newpath` -> `oldpath`.  NULL => unsupported. */
    int (*link)(const char* oldpath, const char* newpath);
    /* Change a path's permission bits.  NULL => fs has no perms (succeed). */
    int (*chmod)(const char* path, unsigned int mode);
    /* Change a path's owner/group (-1 leaves a field unchanged).  NULL =>
     * fs has no ownership (succeed). */
    int (*chown)(const char* path, int uid, int gid);
    /* chmod/chown addressed by an open handle.  NULL => succeed. */
    int (*fchmod)(vfs_file_t* f, unsigned int mode);
    int (*fchown)(vfs_file_t* f, int uid, int gid);
    /* Set a path's modification time (see VFS_UTIME_NOW / VFS_UTIME_OMIT).
     * NULL => fs manages times itself (succeed). */
    int (*utimensat)(const char* path, int64_t mtime_sec, long mtime_nsec);
    /* Fill whole-filesystem statistics.  NULL => unsupported. */
    int (*statfs)(struct vfs_statfs* out);
    /* Fill a kstat for an open handle (real mode/uid/gid).  NULL => the caller
     * cannot determine the owner (used by fd-based permission checks). */
    int (*fstat)(vfs_file_t* f, struct kstat* st);
    /* Per-INODE "no set-id bits left to strip" hint (analog of the reference's
     * S_NOSEC), shared across every handle to the inode.  mark==0 queries
     * (returns nonzero if clean); mark!=0 records the inode as clean.  The fs
     * clears it whenever the mode changes.  NULL => fs has no set-id bits, so
     * the wrapper reports "clean" and the write path never tries to strip. */
    int (*setid_clean)(vfs_file_t* f, int mark);
    /* Flush ALL of this filesystem's pending state to disk (the sync(2) op):
     * deferred metadata, any in-flight journal transaction, and — for a
     * journalled fs — mark the journal clean so a reboot right after does not
     * replay.  Optional; NULL => the wrapper treats a whole-fs sync as a no-op. */
    int (*sync)(void);
    /* Extended attributes.  Path ops take a `nofollow` flag (1 => operate on a
     * final symlink itself).  `name` is the full attribute name (e.g. "user.x").
     * get/list return the value/list size (or its size when buf is NULL/0), set
     * takes XATTR_CREATE/REPLACE flags.  Negative ST_ on error; NULL op => the
     * filesystem has no xattrs (the wrapper reports -EOPNOTSUPP). */
    int (*getxattr)(const char* path, int nofollow, const char* name, void* val, unsigned long size);
    int (*setxattr)(const char* path, int nofollow, const char* name, const void* val, unsigned long size, int flags);
    int (*listxattr)(const char* path, int nofollow, char* list, unsigned long size);
    int (*removexattr)(const char* path, int nofollow, const char* name);
    int (*fgetxattr)(vfs_file_t* f, const char* name, void* val, unsigned long size);
    int (*fsetxattr)(vfs_file_t* f, const char* name, const void* val, unsigned long size, int flags);
    int (*flistxattr)(vfs_file_t* f, char* list, unsigned long size);
    int (*fremovexattr)(vfs_file_t* f, const char* name);
    /* Fetch an attribute given an already-resolved inode number (no path walk).
     * Lets a caller that just stat()'d a file read e.g. its ACL without paying a
     * second path resolution.  NULL op => the wrapper reports -EOPNOTSUPP. */
    int (*getxattr_ino)(unsigned long ino, const char* name, void* val, unsigned long size);
} vfs_ops_t;

struct vfs_file {
    const vfs_ops_t* ops;
    void* fs_private; // points to underlying FS-specific handle
    int refcount;     // Reference count for dup/fork
    int flags;        // O_CLOEXEC, O_RDONLY, etc.
    int is_root_dir;  // True if this is the root "/" directory
    int dev_injected; // True if we've already injected /dev entry in readdir
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
/* UNIX-semantics wrappers — dispatch to the mounted filesystem's op (path ops
 * to the root fs, fd ops to the file's own fs) with a legacy fallback when the
 * op is NULL.  The syscall layer calls only these, never a driver directly. */
int vfs_lstat(const char* path, struct kstat* st);
int vfs_symlink(const char* target, const char* linkpath);
int vfs_readlink(const char* path, char* buf, unsigned long bufsz);
int vfs_link(const char* oldpath, const char* newpath);
int vfs_chmod(const char* path, unsigned int mode);
int vfs_chown(const char* path, int uid, int gid);
int vfs_fchmod(vfs_file_t* f, unsigned int mode);
int vfs_fchown(vfs_file_t* f, int uid, int gid);
int vfs_utimensat(const char* path, int64_t mtime_sec, long mtime_nsec);
int vfs_statfs(const char* path, struct vfs_statfs* out);
int vfs_fstatfs(vfs_file_t* f, struct vfs_statfs* out);
int vfs_fstat(vfs_file_t* f, struct kstat* st);
/* Per-inode set-id-strip fast-path (S_NOSEC analog).  vfs_setid_clean() returns
 * nonzero when the inode is known to have no set-id bits to strip (and for any
 * fs lacking the op); vfs_mark_setid_clean() records that state after a strip. */
int vfs_setid_clean(vfs_file_t* f);
void vfs_mark_setid_clean(vfs_file_t* f);

/* Extended attributes (see the vfs_ops_t xattr ops).  Return the value/list
 * size or a negative ST_ code; an fs without xattrs returns ST_UNSUPPORTED. */
int vfs_getxattr(const char* path, int nofollow, const char* name, void* val, unsigned long size);
int vfs_setxattr(const char* path, int nofollow, const char* name, const void* val, unsigned long size, int flags);
int vfs_listxattr(const char* path, int nofollow, char* list, unsigned long size);
int vfs_removexattr(const char* path, int nofollow, const char* name);
int vfs_fgetxattr(vfs_file_t* f, const char* name, void* val, unsigned long size);
int vfs_fsetxattr(vfs_file_t* f, const char* name, const void* val, unsigned long size, int flags);
int vfs_flistxattr(vfs_file_t* f, char* list, unsigned long size);
int vfs_fremovexattr(vfs_file_t* f, const char* name);
/* Fetch attribute `name` for the file at `path` using the already-known inode
 * number `ino` (from a prior stat), skipping the path resolution.  `path` only
 * selects the owning filesystem.  Returns the value size or a negative ST_. */
int vfs_getxattr_ino(const char* path, unsigned long ino, const char* name, void* val, unsigned long size);
/* Flush the root filesystem's pending metadata + journal to disk (sync(2)).
 * No-op when the root fs provides no sync op.  fs-independent. */
int vfs_sync(void);
size_t vfs_size(vfs_file_t* f);
vfs_file_t* vfs_dup(vfs_file_t* f);  // Increment refcount and return same pointer
void vfs_incref(vfs_file_t* f);      // Increment refcount

/* Force-release any filesystem-private locks owned by the given task id.
 * Used by the scheduler's dead-thread reaper to recover from tasks killed
 * mid-syscall while holding such locks. */
void vfs_release_locks_for_task(uint64_t task_id);

#endif // LIKEOS_VFS_H
