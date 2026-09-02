// LikeOS-64 - Minimal VFS interface (single mount, read-only)
#ifndef LIKEOS_VFS_H
#define LIKEOS_VFS_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/cred.h> /* cred_t, MAY_READ/MAY_WRITE/MAY_EXEC, capable() */

// Basic types (avoid hosted headers in freestanding build)
typedef unsigned long size_t;
typedef long ssize_t; // signed size for read return
typedef unsigned long uintptr_t;

#define VFS_MAX_PATH 256
/* Longest single name between two slashes, as POSIX NAME_MAX.  Anything that
 * takes a path apart must allow for this: a buffer that holds less does not
 * merely truncate, it turns the tail of one name into another component and
 * so names a different file. */
#define VFS_NAME_MAX 255

/* Inode attribute flags the VFS enforces independently of the rwx mode bits,
 * reported by a filesystem's optional inode_flags op.  These gate modification
 * regardless of ownership (even the owner / root must clear them first). */
#define VFS_ATTR_IMMUTABLE 0x01u /* no modification at all                  */
#define VFS_ATTR_APPEND 0x02u /* writes append-only; no truncate/remove  */

/* utimensat() nanosecond sentinels shared between the syscall layer and the
 * filesystem drivers' utimensat op (values match the userspace UTIME_NOW /
 * UTIME_OMIT constants so no translation is needed). */
#define VFS_UTIME_NOW ((long)1073741823L)
#define VFS_UTIME_OMIT ((long)1073741822L)

typedef struct vfs_file vfs_file_t;

/* Whole-filesystem statistics, filled by the statfs op.  Mirrors the fields
 * the statfs/fstatfs syscalls expose to userspace, but is filesystem-agnostic
 * so the syscall layer never references a specific driver's struct. */
struct vfs_statfs {
	unsigned long f_type; /* filesystem type magic                       */
	unsigned long f_bsize; /* optimal transfer block size                 */
	unsigned long
		f_frsize; /* fragment size                               */
	unsigned long
		f_blocks; /* total data blocks                           */
	unsigned long f_bfree; /* free blocks                                 */
	unsigned long
		f_bavail; /* free blocks available to unprivileged users */
	unsigned long f_files; /* total file nodes                            */
	unsigned long f_ffree; /* free file nodes                             */
	unsigned long f_fsid; /* filesystem id (not meaningful here)         */
	unsigned long
		f_namelen; /* maximum filename length                     */
};

typedef struct {
	int (*open)(const char *path, int flags, vfs_file_t **out);
	int (*stat)(const char *path, struct kstat *st);
	long (*read)(vfs_file_t *f, void *buf, long bytes);
	long (*write)(vfs_file_t *f, const void *buf, long bytes);
	long (*seek)(vfs_file_t *f, long offset, int whence);
	long (*readdir)(vfs_file_t *f, void *buf, long bytes);
	int (*truncate)(vfs_file_t *f, unsigned long size);
	int (*unlink)(const char *path);
	int (*rename)(const char *oldpath, const char *newpath);
	int (*mkdir)(const char *path, unsigned int mode);
	int (*rmdir)(const char *path);
	int (*chdir)(const char *path);
	int (*close)(vfs_file_t *f);
	/* Force-release any per-filesystem locks owned by the given task.
     * Called from the scheduler's dead-thread reaper when a task that was
     * killed (e.g. via SIGINT) may have died inside a syscall while holding
     * filesystem-private locks.  Optional; may be NULL. */
	int (*release_locks_for_task)(uint64_t task_id);
	/* Flush a file's dirty data + metadata to disk (fsync).  Optional; when
     * NULL the syscall layer treats the flush as a no-op for this fs.  Lets
     * a filesystem whose cache key differs from the raw FAT32 start_cluster
     * (e.g. ext4) flush correctly. */
	int (*fsync)(vfs_file_t *f);

	/* ---- Optional UNIX-semantics operations -------------------------------
     * A filesystem lacking a given capability leaves the slot NULL; the
     * matching vfs_* wrapper then applies a legacy fallback (see vfs.c) so the
     * syscall layer never needs to know which filesystem is mounted. */

	/* Like stat(), but does NOT follow a final symlink.  NULL => no symlinks
     * on this fs, so the wrapper falls back to stat(). */
	int (*lstat)(const char *path, struct kstat *st);
	/* Create a symbolic link `linkpath` -> `target`.  NULL => unsupported. */
	int (*symlink)(const char *target, const char *linkpath);
	/* Read a symlink's target into buf (no NUL).  Returns byte count (>=0) or
     * a negative ST_ code.  NULL => path is not a symlink on this fs. */
	int (*readlink)(const char *path, char *buf, unsigned long bufsz);
	/* Create a hard link `newpath` -> `oldpath`.  NULL => unsupported. */
	int (*link)(const char *oldpath, const char *newpath);
	/* Change a path's permission bits.  NULL => fs has no perms (succeed). */
	int (*chmod)(const char *path, unsigned int mode);
	/* Change a path's owner/group (-1 leaves a field unchanged).  NULL =>
     * fs has no ownership (succeed). */
	int (*chown)(const char *path, int uid, int gid);
	/* chmod/chown addressed by an open handle.  NULL => succeed. */
	int (*fchmod)(vfs_file_t *f, unsigned int mode);
	int (*fchown)(vfs_file_t *f, int uid, int gid);
	/* Set a path's modification time (see VFS_UTIME_NOW / VFS_UTIME_OMIT).
     * NULL => fs manages times itself (succeed). */
	int (*utimensat)(const char *path, int64_t mtime_sec, long mtime_nsec);
	/* Fill whole-filesystem statistics.  NULL => unsupported. */
	int (*statfs)(struct vfs_statfs *out);
	/* Fill a kstat for an open handle (real mode/uid/gid).  NULL => the caller
     * cannot determine the owner (used by fd-based permission checks). */
	int (*fstat)(vfs_file_t *f, struct kstat *st);
	/* Per-INODE "no set-id bits left to strip" hint (analog of the reference's
     * S_NOSEC), shared across every handle to the inode.  mark==0 queries
     * (returns nonzero if clean); mark!=0 records the inode as clean.  The fs
     * clears it whenever the mode changes.  NULL => fs has no set-id bits, so
     * the wrapper reports "clean" and the write path never tries to strip. */
	int (*setid_clean)(vfs_file_t *f, int mark);
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
	int (*getxattr)(const char *path, int nofollow, const char *name,
			void *val, unsigned long size);
	int (*setxattr)(const char *path, int nofollow, const char *name,
			const void *val, unsigned long size, int flags);
	int (*listxattr)(const char *path, int nofollow, char *list,
			 unsigned long size);
	int (*removexattr)(const char *path, int nofollow, const char *name);
	int (*fgetxattr)(vfs_file_t *f, const char *name, void *val,
			 unsigned long size);
	int (*fsetxattr)(vfs_file_t *f, const char *name, const void *val,
			 unsigned long size, int flags);
	int (*flistxattr)(vfs_file_t *f, char *list, unsigned long size);
	int (*fremovexattr)(vfs_file_t *f, const char *name);
	/* Fetch an attribute given an already-resolved inode number (no path walk).
     * Lets a caller that just stat()'d a file read e.g. its ACL without paying a
     * second path resolution.  NULL op => the wrapper reports -EOPNOTSUPP. */
	int (*getxattr_ino)(unsigned long ino, const char *name, void *val,
			    unsigned long size);
	/* ---- Optional permission participation --------------------------------
	 * Filesystem-specific access decision, consulted by vfs_permission()
	 * BEFORE the generic mode/ACL check.  Returns ST_OK to allow outright,
	 * a negative ST_ code to deny outright, or ST_UNSUPPORTED to defer to the
	 * generic VFS check.  Used by an ownership-less fs (FAT32) to emulate
	 * permissive access.  NULL => always defer to the generic check. */
	int (*permission)(const char *path, unsigned long ino, int want);
	/* Report the VFS_ATTR_* flags for an already-resolved inode (immutable /
	 * append-only).  Lets the VFS veto modifications independently of the mode
	 * bits.  NULL => the filesystem has no such flags (none set). */
	int (*inode_flags)(unsigned long ino, uint32_t *out_flags);
	/* Create a node that owns no data: a socket (S_IFSOCK) or FIFO
	 * (S_IFIFO), with the type in the high bits of `mode`.  Needed because
	 * a bound AF_UNIX socket must be visible in the filesystem — clients
	 * stat() the path and require S_IFSOCK before connecting.  NULL => the
	 * filesystem cannot represent such nodes (the wrapper reports
	 * -EOPNOTSUPP). */
	int (*mknod)(const char *path, unsigned int mode);
	/* Like ->open, but carries the creation mode for O_CREAT.  A filesystem
	 * that does not list it gets NULL and the wrapper falls back to ->open,
	 * which creates with a default mode.
	 *
	 * ->open cannot simply gain the parameter, because the mode has to reach
	 * the filesystem that actually allocates the inode, and every other
	 * caller of ->open is opening a file that already exists.
	 *
	 * NOTE for anyone adding an op here: the per-filesystem tables USED to be
	 * positional, so inserting a member anywhere but the end silently
	 * renumbered every entry after it.  Adding this one after ->getxattr_ino
	 * did exactly that -- ext4's ->permission slot ended up holding
	 * ext4_inode_flags_op, which page-faulted on the first open during boot.
	 * The tables (ext4_vfs_ops, fat32_vfs_ops) are designated initialisers
	 * now, so a member may be added anywhere and a wrong slot is a compile
	 * error rather than a crash.  Keep them that way. */
	int (*open_mode)(const char *path, int flags, unsigned int mode,
			 vfs_file_t **out);
	/* Read at an explicit offset without touching the handle's position.
	 *
	 * Optional: vfs_pread() falls back to seek/read/seek-back for a
	 * filesystem that does not have it.  Worth having wherever a file can
	 * be mapped, because demand paging reads through this and a mapping
	 * shares its handle with the caller's open descriptor -- see
	 * vfs_pread(). */
	long (*read_at)(vfs_file_t *f, void *buf, long bytes, long off);
} vfs_ops_t;

struct vfs_file {
	const vfs_ops_t *ops;
	void *fs_private; // points to underlying FS-specific handle
	int refcount; // Reference count for dup/fork
	int flags; // O_CLOEXEC, O_RDONLY, etc.
	int is_root_dir; // True if this is the root "/" directory
	int dev_injected; // True if we've already injected /dev entry in readdir
	/* Demand-paging page-in serialisation (mm_handle_demand_fault):
	 * protects THIS handle's seek/read/seek-back sequence against a
	 * concurrent faulting task sharing the handle (fork family).  Per-file
	 * on purpose: a global flag serialised every page-in in the system and
	 * starved cold-starting processes for seconds under parallel load. */
	volatile int pagein_busy;
	volatile int64_t pagein_owner; /* holder task id; stale when !busy */
	/* Absolute path this handle was opened with, or NULL.
	 *
	 * Recorded so a descriptor can be used as the dirfd of an *at()
	 * syscall: openat/unlinkat/fstatat/faccessat resolve a relative name
	 * against the directory the descriptor refers to, and nothing else in
	 * this structure can say which directory that is.
	 *
	 * Owned by the VFS layer -- allocated in vfs_open_common() and freed in
	 * vfs_close(), because the filesystem's ->close frees the structure
	 * itself and would take this with it. */
	char *at_path;
};

// File descriptor flags
#define FD_CLOEXEC 0x1

int vfs_init(void);
int vfs_register_root(const vfs_ops_t *ops);
int vfs_register_devfs(const vfs_ops_t *ops);
/* A further filesystem at `prefix' ("/sys", "/proc"); paths under it go to
 * `ops'.  The name is listed in the root directory. */
int vfs_register_mount(const char *prefix, const vfs_ops_t *ops);
int vfs_mount_name(int index, const char **name_out);
int vfs_root_ready(void);
int vfs_open(const char *path, int flags, vfs_file_t **out);
/* open() with the creation mode for O_CREAT.  `mode` is the FINAL mode -- the
 * caller has already applied its umask, matching the convention vfs_mknod()
 * documents.  Passing the raw mode from userspace would silently ignore the
 * umask, which is how "create it 0600" ends up world-readable. */
int vfs_open_mode(const char *path, int flags, unsigned int mode,
		  vfs_file_t **out);
int vfs_stat(const char *path, struct kstat *st);
int vfs_chdir(const char *path);
long vfs_read(vfs_file_t *f, void *buf, long bytes);
/* Read `bytes' at `off'.  Does not consult or move the handle's position when
 * the filesystem can read positionally; falls back to seek/read/seek-back
 * under the handle's page-in flag when it cannot. */
long vfs_pread(vfs_file_t *f, void *buf, long bytes, long off);
long vfs_write(vfs_file_t *f, const void *buf, long bytes);
long vfs_seek(vfs_file_t *f, long offset, int whence);
long vfs_readdir(vfs_file_t *f, void *buf, long bytes);
int vfs_truncate(vfs_file_t *f, unsigned long size);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_mkdir(const char *path, unsigned int mode);
/* Create a node with no data: S_IFSOCK or S_IFIFO in the mode's type bits. */
int vfs_mknod(const char *path, unsigned int mode);
int vfs_rmdir(const char *path);
int vfs_close(vfs_file_t *f);
/* UNIX-semantics wrappers — dispatch to the mounted filesystem's op (path ops
 * to the root fs, fd ops to the file's own fs) with a legacy fallback when the
 * op is NULL.  The syscall layer calls only these, never a driver directly. */
int vfs_lstat(const char *path, struct kstat *st);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, unsigned long bufsz);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_chmod(const char *path, unsigned int mode);
int vfs_chown(const char *path, int uid, int gid);
int vfs_fchmod(vfs_file_t *f, unsigned int mode);
int vfs_fchown(vfs_file_t *f, int uid, int gid);
int vfs_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec);
int vfs_statfs(const char *path, struct vfs_statfs *out);
int vfs_fstatfs(vfs_file_t *f, struct vfs_statfs *out);
int vfs_fstat(vfs_file_t *f, struct kstat *st);

/* POSIX advisory record locks (kernel/fs/frlock.c).  Ownership is the process
 * (tgid); locks are dropped when it closes any descriptor for the file, or
 * exits -- both hooks below must stay wired, or a dead process's lock blocks
 * every later one forever. */
struct task;
/* Declared by tag, not the typedef: k_flock_t lives in ke/syscall.h, and this
 * header must not depend on the syscall layer to be includable. */
struct k_flock;
int frlock_fcntl(vfs_file_t *f, int cmd, struct k_flock *flp, struct task *cur);
void frlock_release_for_task(uint32_t pid);
void frlock_release_for_file(vfs_file_t *f, uint32_t pid);
/* Per-inode set-id-strip fast-path (S_NOSEC analog).  vfs_setid_clean() returns
 * nonzero when the inode is known to have no set-id bits to strip (and for any
 * fs lacking the op); vfs_mark_setid_clean() records that state after a strip. */
int vfs_setid_clean(vfs_file_t *f);
void vfs_mark_setid_clean(vfs_file_t *f);

/* Extended attributes (see the vfs_ops_t xattr ops).  Return the value/list
 * size or a negative ST_ code; an fs without xattrs returns ST_UNSUPPORTED. */
int vfs_getxattr(const char *path, int nofollow, const char *name, void *val,
		 unsigned long size);
int vfs_setxattr(const char *path, int nofollow, const char *name,
		 const void *val, unsigned long size, int flags);
int vfs_listxattr(const char *path, int nofollow, char *list,
		  unsigned long size);
int vfs_removexattr(const char *path, int nofollow, const char *name);
int vfs_fgetxattr(vfs_file_t *f, const char *name, void *val,
		  unsigned long size);
int vfs_fsetxattr(vfs_file_t *f, const char *name, const void *val,
		  unsigned long size, int flags);
int vfs_flistxattr(vfs_file_t *f, char *list, unsigned long size);
int vfs_fremovexattr(vfs_file_t *f, const char *name);
/* Fetch attribute `name` for the file at `path` using the already-known inode
 * number `ino` (from a prior stat), skipping the path resolution.  `path` only
 * selects the owning filesystem.  Returns the value size or a negative ST_. */
int vfs_getxattr_ino(const char *path, unsigned long ino, const char *name,
		     void *val, unsigned long size);
/* ---- Permission enforcement (the canonical place for DAC) -------------------
 * The VFS is where Unix permission policy lives; the syscall layer only copies
 * arguments and dispatches, and the filesystem drivers only supply ownership /
 * ACL / inode-flag data.  The vfs_* operation wrappers call these internally,
 * so most callers never invoke them directly — the exceptions are access(2)/
 * faccessat (real-id check, no open) and execve (MAY_EXEC on the image).
 *
 * `want` is a mask of MAY_READ/MAY_WRITE/MAY_EXEC (from cred.h).  All return 0
 * when permitted or a negative ST_ code (mapped to errno by the syscall layer).
 * They are permissive when a path can't be resolved/stat'd, leaving the real
 * operation to report ENOENT/ENOTDIR — and a no-op for the privileged caller. */

/* May the current task access `path` for `want`, using the effective/fs IDs?
 * Generic mode/ACL check plus the filesystem's optional `permission` hook. */
int vfs_permission(const char *path, int want);
/* Search (x) permission on every ancestor directory of `path` (effective IDs). */
int vfs_permission_traverse(const char *path);
/* access(2)/faccessat: traversal + access check against the REAL uid/gid. */
int vfs_access(const char *path, int want);
/* Prefix traversal with explicit real(1)/effective(0) id selection. */
int vfs_access_traverse(const char *path, int use_real);
/* Write+search on the PARENT directory of `path` (create/remove/rename). */
int vfs_permission_parent(const char *path, int want);
/* Remove/rename gate: parent write+search plus the sticky-bit ownership rule. */
int vfs_permission_remove(const char *path);
/* Access check against an already-resolved stat `st` (no path walk); honours
 * the ACL then the mode bits.  `use_real` selects real vs effective/fs ids. */
int vfs_check_access(const char *path, const struct kstat *st, int want,
		     int use_real);

/* Flush the root filesystem's pending metadata + journal to disk (sync(2)).
 * No-op when the root fs provides no sync op.  fs-independent. */
int vfs_sync(void);
size_t vfs_size(vfs_file_t *f);
vfs_file_t *
vfs_dup(vfs_file_t *f); // Increment refcount and return same pointer
void vfs_incref(vfs_file_t *f); // Increment refcount

/* Force-release any filesystem-private locks owned by the given task id.
 * Used by the scheduler's dead-thread reaper to recover from tasks killed
 * mid-syscall while holding such locks. */
void vfs_release_locks_for_task(uint64_t task_id);

#endif // LIKEOS_VFS_H
