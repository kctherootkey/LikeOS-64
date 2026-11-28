// LikeOS - Minimal VFS interface (single mount, read-only)
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_VFS_H
#define LIKEOS_VFS_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/cred.h> /* cred_t, MAY_READ/MAY_WRITE/MAY_EXEC, capable() */

// Basic types (avoid hosted headers in freestanding build)
typedef unsigned long size_t;
typedef long ssize_t; // signed size for read return
typedef unsigned long uintptr_t;

/* Longest whole path, the terminating NUL included: PATH_MAX.  Never kept
 * on the stack -- a path lives in a path buffer (kernel/fs/pathbuf.h). */
#define VFS_MAX_PATH 4096
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

/* A mounted filesystem instance (kernel/fs/mount.c).  Every path operation
 * of vfs_ops_t receives the mount it runs on, which is how a driver that can
 * be mounted more than once finds its instance (m->fs_data). */
struct vfs_mount;
typedef struct vfs_mount vfs_mount_t;
struct vfs_superblock;
struct block_device;
struct pc_page; /* fs/pagecache.h */

/* Times for the utimens/futimens ops.  A nanosecond field may hold
 * VFS_UTIME_NOW (use the current time) or VFS_UTIME_OMIT (leave as it is). */
struct vfs_utimes {
	int64_t atime_sec;
	long atime_nsec;
	int64_t mtime_sec;
	long mtime_nsec;
};

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
	unsigned long f_flags; /* mount flags (ST_RDONLY ...), set by the VFS */
};

typedef struct {
	int (*open)(vfs_mount_t *m, const char *path, int flags,
		    vfs_file_t **out);
	int (*stat)(vfs_mount_t *m, const char *path, struct kstat *st);
	long (*read)(vfs_file_t *f, void *buf, long bytes);
	long (*write)(vfs_file_t *f, const void *buf, long bytes);
	long (*seek)(vfs_file_t *f, long offset, int whence);
	long (*readdir)(vfs_file_t *f, void *buf, long bytes);
	int (*truncate)(vfs_file_t *f, unsigned long size);
	int (*unlink)(vfs_mount_t *m, const char *path);
	int (*rename)(vfs_mount_t *m, const char *oldpath, const char *newpath);
	int (*mkdir)(vfs_mount_t *m, const char *path, unsigned int mode);
	int (*rmdir)(vfs_mount_t *m, const char *path);
	int (*chdir)(vfs_mount_t *m, const char *path);
	int (*close)(vfs_file_t *f);
	/* Force-release any per-filesystem locks owned by the given task.
     * Called from the scheduler's dead-thread reaper when a task that was
     * killed (e.g. via SIGINT) may have died inside a syscall while holding
     * filesystem-private locks.  Optional; may be NULL. */
	int (*release_locks_for_task)(vfs_mount_t *m, uint64_t task_id);
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
	int (*lstat)(vfs_mount_t *m, const char *path, struct kstat *st);
	/* Create a symbolic link `linkpath` -> `target`.  NULL => unsupported. */
	int (*symlink)(vfs_mount_t *m, const char *target, const char *linkpath);
	/* Read a symlink's target into buf (no NUL).  Returns byte count (>=0) or
     * a negative ST_ code.  NULL => path is not a symlink on this fs. */
	int (*readlink)(vfs_mount_t *m, const char *path, char *buf,
			unsigned long bufsz);
	/* Create a hard link `newpath` -> `oldpath`.  NULL => unsupported.
	 * `follow' says whether a symlink named by `oldpath' is followed (the
	 * link then names its target) or linked itself, which is the default
	 * of link()/linkat(). */
	int (*link)(vfs_mount_t *m, const char *oldpath, const char *newpath,
		    int follow);
	/* Change a path's permission bits.  NULL => fs has no perms (succeed). */
	int (*chmod)(vfs_mount_t *m, const char *path, unsigned int mode);
	/* Change a path's owner/group (-1 leaves a field unchanged).  NULL =>
     * fs has no ownership (succeed).  `nofollow' changes a final symlink
     * itself (lchown) instead of what it points to. */
	int (*chown)(vfs_mount_t *m, const char *path, int uid, int gid,
		     int nofollow);
	/* chmod/chown addressed by an open handle.  NULL => succeed. */
	int (*fchmod)(vfs_file_t *f, unsigned int mode);
	int (*fchown)(vfs_file_t *f, int uid, int gid);
	/* Set a path's access and modification times (struct vfs_utimes;
	 * each may be "now" or "leave alone"); the change time becomes the
	 * current time.  `nofollow' sets a final symlink's own times.  NULL =>
	 * fs manages times itself (succeed). */
	int (*utimens)(vfs_mount_t *m, const char *path, int nofollow,
		       const struct vfs_utimes *t);
	/* The same for an open handle (futimens).  NULL => succeed. */
	int (*futimens)(vfs_file_t *f, const struct vfs_utimes *t);
	/* Fill whole-filesystem statistics.  NULL => unsupported. */
	int (*statfs)(vfs_mount_t *m, struct vfs_statfs *out);
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
	int (*sync)(vfs_mount_t *m);
	/* Extended attributes.  Path ops take a `nofollow` flag (1 => operate on a
     * final symlink itself).  `name` is the full attribute name (e.g. "user.x").
     * get/list return the value/list size (or its size when buf is NULL/0), set
     * takes XATTR_CREATE/REPLACE flags.  Negative ST_ on error; NULL op => the
     * filesystem has no xattrs (the wrapper reports -EOPNOTSUPP). */
	int (*getxattr)(vfs_mount_t *m, const char *path, int nofollow,
			const char *name, void *val, unsigned long size);
	int (*setxattr)(vfs_mount_t *m, const char *path, int nofollow,
			const char *name, const void *val, unsigned long size,
			int flags);
	int (*listxattr)(vfs_mount_t *m, const char *path, int nofollow,
			 char *list, unsigned long size);
	int (*removexattr)(vfs_mount_t *m, const char *path, int nofollow,
			   const char *name);
	int (*fgetxattr)(vfs_file_t *f, const char *name, void *val,
			 unsigned long size);
	int (*fsetxattr)(vfs_file_t *f, const char *name, const void *val,
			 unsigned long size, int flags);
	int (*flistxattr)(vfs_file_t *f, char *list, unsigned long size);
	int (*fremovexattr)(vfs_file_t *f, const char *name);
	/* Fetch an attribute given an already-resolved inode number (no path walk).
     * Lets a caller that just stat()'d a file read e.g. its ACL without paying a
     * second path resolution.  NULL op => the wrapper reports -EOPNOTSUPP. */
	int (*getxattr_ino)(vfs_mount_t *m, unsigned long ino, const char *name,
			    void *val, unsigned long size);
	/* ---- Optional permission participation --------------------------------
	 * Filesystem-specific access decision, consulted by vfs_permission()
	 * BEFORE the generic mode/ACL check.  Returns ST_OK to allow outright,
	 * a negative ST_ code to deny outright, or ST_UNSUPPORTED to defer to the
	 * generic VFS check.  Used by an ownership-less fs (FAT32) to emulate
	 * permissive access.  NULL => always defer to the generic check. */
	int (*permission)(vfs_mount_t *m, const char *path, unsigned long ino,
			  int want);
	/* Report the VFS_ATTR_* flags for an already-resolved inode (immutable /
	 * append-only).  Lets the VFS veto modifications independently of the mode
	 * bits.  NULL => the filesystem has no such flags (none set). */
	int (*inode_flags)(vfs_mount_t *m, unsigned long ino,
			   uint32_t *out_flags);
	/* Create a node that owns no data: a socket (S_IFSOCK) or FIFO
	 * (S_IFIFO), or a character / block device node (S_IFCHR / S_IFBLK,
	 * `dev' its device number in the userland dev_t encoding), with the
	 * type in the high bits of `mode`.  Needed because a bound AF_UNIX
	 * socket must be visible in the filesystem — clients stat() the path
	 * and require S_IFSOCK before connecting.  NULL => the filesystem
	 * cannot represent such nodes (the wrapper reports -EOPNOTSUPP); a
	 * filesystem that keeps only some of the types answers ST_UNSUPPORTED
	 * for the others. */
	int (*mknod)(vfs_mount_t *m, const char *path, unsigned int mode,
		     uint64_t dev);
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
	int (*open_mode)(vfs_mount_t *m, const char *path, int flags,
			 unsigned int mode, vfs_file_t **out);
	/* Read at an explicit offset without touching the handle's position.
	 *
	 * Optional: vfs_pread() falls back to seek/read/seek-back for a
	 * filesystem that does not have it.  Worth having wherever a file can
	 * be mapped, because demand paging reads through this and a mapping
	 * shares its handle with the caller's open descriptor -- see
	 * vfs_pread(). */
	long (*read_at)(vfs_file_t *f, void *buf, long bytes, long off);
	/* The page cache's own page holding page `idx' of the file, held
	 * (pagecache_put() when done), or NULL.
	 *
	 * Optional, and only for whole pages that lie inside the file: a
	 * private mapping maps this frame itself instead of a copy of it,
	 * so every process mapping the same library page shares one frame
	 * (copied on its first write).  Without it, or for the last partial
	 * page of a file, demand paging reads a private copy through
	 * read_at. */
	struct pc_page *(*cache_page)(vfs_file_t *f, unsigned long idx);
	/* Write at an explicit offset without touching the handle's position
	 * (pwrite).  The position is the call's own: a concurrent read(),
	 * write() or lseek() on the same description neither moves it nor is
	 * moved by it.  Bytes written or a negative status.
	 *
	 * Optional: vfs_pwrite() falls back to seek/write/seek-back under the
	 * handle's page-in flag for a filesystem without it. */
	long (*write_at)(vfs_file_t *f, const void *buf, long bytes, long off);
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
	/* Change notification (kernel/fs/fsnotify.h): the inode number of the
	 * object this handle is open on, looked up at the first event that
	 * needs it and kept, since it cannot change while the handle is open.
	 * fsn_state: 0 not looked up yet, 1 fsn_ino/fsn_isdir valid, -1 this
	 * handle reports nothing (a device, or the lookup failed).  Meaningful
	 * only while at_path is set, and initialised with it. */
	uint64_t fsn_ino;
	int8_t fsn_state;
	uint8_t fsn_isdir;
	/* The mount the handle was opened through (set by vfs_open_common();
	 * NULL for a handle made some other way -- a device opened by the
	 * syscall layer directly, an anonymous file).  An open handle keeps
	 * its mount from being unmounted (vfs_mount.open_files). */
	vfs_mount_t *mnt;
};

/* ---- Mounts ---------------------------------------------------------------
 *
 * Mount flags, the values of the mount(2) interface (mirrored in the
 * userland <sys/mount.h>).  Only the ones the VFS acts on are listed with a
 * note; the rest are accepted and shown in /proc/mounts. */
#define VFS_MS_RDONLY 0x1UL /* every modification fails with EROFS */
#define VFS_MS_NOSUID 0x2UL /* exec ignores set-user/-group-ID bits */
#define VFS_MS_NODEV 0x4UL /* device nodes on it cannot be opened */
#define VFS_MS_NOEXEC 0x8UL /* nothing on it can be executed */
#define VFS_MS_SYNCHRONOUS 0x10UL
#define VFS_MS_REMOUNT 0x20UL /* change the flags of an existing mount */
#define VFS_MS_MANDLOCK 0x40UL
#define VFS_MS_DIRSYNC 0x80UL
#define VFS_MS_NOSYMFOLLOW 0x100UL
#define VFS_MS_NOATIME 0x400UL
#define VFS_MS_NODIRATIME 0x800UL
#define VFS_MS_BIND 0x1000UL
#define VFS_MS_MOVE 0x2000UL
#define VFS_MS_REC 0x4000UL
#define VFS_MS_SILENT 0x8000UL
#define VFS_MS_RELATIME 0x200000UL
#define VFS_MS_STRICTATIME 0x1000000UL
#define VFS_MS_LAZYTIME 0x2000000UL
/* The flags a mount keeps (the others are requests: REMOUNT, BIND, ...). */
#define VFS_MS_PERMANENT                                                    \
	(VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC |    \
	 VFS_MS_SYNCHRONOUS | VFS_MS_MANDLOCK | VFS_MS_DIRSYNC |           \
	 VFS_MS_NOSYMFOLLOW | VFS_MS_NOATIME | VFS_MS_NODIRATIME |         \
	 VFS_MS_RELATIME | VFS_MS_STRICTATIME | VFS_MS_LAZYTIME)

/* umount2() flags. */
#define VFS_MNT_FORCE 0x1
#define VFS_MNT_DETACH 0x2
#define VFS_MNT_EXPIRE 0x4
#define VFS_UMOUNT_NOFOLLOW 0x8

#define VFS_FSTYPE_MAX 16
#define VFS_MOUNT_OPTS_MAX 160

/* A filesystem type that mount(2) can instantiate. */
typedef struct vfs_fstype {
	const char *name; /* what /proc/mounts shows: "ext4", "vfat"      */
	/* Other names mount(2) accepts for it ("ext2", "fat32", ...), NULL
	 * terminated; NULL for none.  The name used is what is shown. */
	const char *const *aliases;
	/* Needs a block device as its source (else the source is a label). */
	int needs_device;
	/* Bring up an instance: fill m->ops, m->fs_data and (for a
	 * filesystem in the page cache) m->sb.  `bdev' is the claimed device
	 * or NULL, `flags' the VFS_MS_* flags, `data' the option string (may
	 * be NULL).  ST_OK or an ST_ code; on failure nothing is left behind. */
	int (*mount)(vfs_mount_t *m, struct block_device *bdev,
		     unsigned long flags, const char *data);
	/* Last step of an unmount, called with nothing open on the mount and
	 * its pages and inodes already stored: commit what is left, drop
	 * every cache entry of the instance and free it.  ST_OK, or an error
	 * that leaves the instance mounted and intact. */
	int (*unmount)(vfs_mount_t *m);
	/* Change a live instance's flags (read-only <-> read-write) and
	 * options.  NULL: only changes the filesystem need not act on. */
	int (*remount)(vfs_mount_t *m, unsigned long flags, const char *data);
	/* Append the filesystem's own options ("uid=1000,umask=022") to
	 * /proc/mounts' option column.  Returns the length written (without
	 * a NUL), at most cap - 1.  NULL: none. */
	long (*show_options)(vfs_mount_t *m, char *buf, long cap);
	/* Does `bdev' hold this filesystem?  For fstype "auto".  NULL: the
	 * type is never chosen automatically. */
	int (*probe)(struct block_device *bdev);
} vfs_fstype_t;

struct vfs_mount {
	/* --- lookup state (kernel/fs/mount.c) --- */
	volatile uint32_t seq; /* odd while the slot is being rewritten   */
	volatile int state; /* VFS_MNT_*                                */
	volatile long active; /* path operations in flight on it          */
	volatile long open_files; /* handles open through it              */
	volatile long open_writers; /* of those, open for writing (counted
				     * on every mount, the boot ones too) */
	int pinned; /* registered at boot, never unmounted: not counted    */
	int full_paths; /* the driver is handed absolute paths, not paths
			 * relative to the mount point (devfs, /proc, /sys) */
	int inject; /* listed in the root directory by the VFS             */
	/* --- identity --- */
	int id; /* mount id (/proc/self/mountinfo)                       */
	int parent_id;
	/* Both VFS_MAX_PATH bytes, given to the slot the first time it is
	 * used and kept by it for good (slots are reused, never freed), so a
	 * lock-free reader of a stale slot still reads valid memory. */
	char *path; /* canonical mount point, "/" for the root */
	unsigned path_len;
	char *source; /* "/dev/sda2", "tmpfs", "proc"      */
	char fstype[VFS_FSTYPE_MAX];
	unsigned long flags; /* VFS_MS_* (VFS_MS_PERMANENT bits only)     */
	char options[VFS_MOUNT_OPTS_MAX]; /* filesystem option string   */
	const char *show_flags; /* fixed option text for /proc/mounts, or NULL */
	/* Device number: shown in mountinfo, and -- with dev_override -- the
	 * st_dev every stat on the mount reports (unique per mount, so tools
	 * comparing st_dev see where one filesystem ends). */
	uint64_t dev;
	int dev_override;
	/* --- the instance --- */
	const vfs_ops_t *ops;
	const vfs_fstype_t *type; /* NULL for a boot-time pseudo mount     */
	void *fs_data; /* the driver's instance                           */
	struct vfs_superblock *sb; /* page-cache superblock, or NULL      */
	struct block_device *bdev; /* claimed device, or NULL             */
};

#define VFS_MNT_FREE 0
#define VFS_MNT_LIVE 1 /* reachable by path                            */
#define VFS_MNT_DYING 2 /* being unmounted                              */
#define VFS_MNT_DETACHED 3 /* lazily unmounted: gone from the name space,
			    * torn down when its last file closes        */

/* What a descriptor was opened for, from its access mode alone: read with
 * O_RDONLY or O_RDWR, write with O_WRONLY or O_RDWR.  Access mode 3 is for
 * ioctls and gives neither; O_APPEND is where a write goes, not leave to
 * write.  The open checked exactly this much (vfs_open_common()). */
static inline int file_may_read(const struct vfs_file *f)
{
	int acc = f->flags & 3;

	return acc == 0 /* O_RDONLY */ || acc == 2 /* O_RDWR */;
}

static inline int file_may_write(const struct vfs_file *f)
{
	int acc = f->flags & 3;

	return acc == 1 /* O_WRONLY */ || acc == 2 /* O_RDWR */;
}

// File descriptor flags
#define FD_CLOEXEC 0x1

int vfs_init(void);
/* The root filesystem, mounted by the storage bootstrap: `fs_data' is its
 * instance, `sb' its page-cache superblock (or NULL), `type' its fstype (for
 * remounting; may be NULL). */
int vfs_register_root(const vfs_ops_t *ops, void *fs_data,
		      struct vfs_superblock *sb, const vfs_fstype_t *type,
		      const char *fstype);
/* Name the root's device for /proc/mounts ("/dev/sda2") once it is known;
 * `dev' its device number. */
void vfs_root_set_source(const char *source, uint64_t dev);
int vfs_register_devfs(const vfs_ops_t *ops);
/* A further boot-time filesystem at `prefix' ("/sys", "/proc"); paths under
 * it go to `ops' -- absolute when `full_paths', else relative to the mount
 * point like any mounted filesystem's.  The name is listed in the root
 * directory.  `fstype' is what /proc/mounts shows, `fs_data' the driver's
 * instance (may be NULL), `show' a fixed option text for /proc/mounts
 * (NULL: from the flags). */
int vfs_register_mount(const char *prefix, const vfs_ops_t *ops,
		       const char *fstype, void *fs_data, unsigned long flags,
		       const char *show, int full_paths);
/* A boot-time instance of the mountable filesystem `type' at `prefix'
 * (tmpfs at /ram), made by type->mount with `flags' (VFS_MS_*) and the
 * option string `data' (may be NULL).  Like the other boot-time mounts it is
 * never unmounted and its name is listed in the root directory; /proc/mounts
 * shows it like a mount(2) one.  ST_OK or an ST_* error. */
int vfs_mount_boot(const char *prefix, const vfs_fstype_t *type,
		   unsigned long flags, const char *data);
int vfs_mount_name(int index, const char **name_out);
int vfs_root_ready(void);

/* Filesystem types mount(2) can create (kernel/fs/mount.c). */
int vfs_fstype_register(const vfs_fstype_t *type);
/* mount(2) / umount2(2) on canonical absolute paths (the target's symbolic
 * links are resolved here).  0 or a negative errno. */
int vfs_mount(const char *source, const char *target, const char *fstype,
	      unsigned long flags, const char *data);
int vfs_umount(const char *target, int flags);
/* Flush one filesystem (syncfs(2)): its pages, inodes, metadata and the
 * device cache. */
int vfs_syncfs(vfs_file_t *f);
/* The mount table as /proc/mounts (info == 0) or /proc/self/mountinfo
 * (info == 1) shows it, appended at `pos' (pfs_printf conventions: the
 * returned length may exceed cap, the caller then retries larger). */
long vfs_mounts_show(char *buf, long cap, long pos, int info);
/* The VFS_MS_* flags of the mount `path' is on (0 when none). */
unsigned long vfs_path_mount_flags(const char *path);
unsigned long vfs_file_mount_flags(vfs_file_t *f);
/* A path's st_dev as stat() reports it (the mount's device number). */
int vfs_path_dev(const char *path, uint64_t *dev);
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
/* The file's ops->cache_page, or NULL when it has none (see there). */
struct pc_page *vfs_cache_page(vfs_file_t *f, unsigned long idx);
long vfs_write(vfs_file_t *f, const void *buf, long bytes);
/* Write `bytes' at `off' without moving the handle's position (pwrite):
 * the filesystem's ->write_at, else seek/write/seek-back under the handle's
 * page-in flag.  Bytes written or a negative status. */
long vfs_pwrite(vfs_file_t *f, const void *buf, long bytes, long off);
long vfs_seek(vfs_file_t *f, long offset, int whence);
long vfs_readdir(vfs_file_t *f, void *buf, long bytes);
int vfs_truncate(vfs_file_t *f, unsigned long size);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_mkdir(const char *path, unsigned int mode);
/* Create a node with no data: S_IFSOCK, S_IFIFO, S_IFCHR or S_IFBLK in the
 * mode's type bits; `dev' the device number of a device node. */
int vfs_mknod(const char *path, unsigned int mode, uint64_t dev);
int vfs_rmdir(const char *path);
int vfs_close(vfs_file_t *f);
/* UNIX-semantics wrappers — dispatch to the mounted filesystem's op (path ops
 * to the root fs, fd ops to the file's own fs) with a legacy fallback when the
 * op is NULL.  The syscall layer calls only these, never a driver directly. */
int vfs_lstat(const char *path, struct kstat *st);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, unsigned long bufsz);
int vfs_link(const char *oldpath, const char *newpath, int follow);
int vfs_chmod(const char *path, unsigned int mode);
int vfs_chown(const char *path, int uid, int gid, int nofollow);
int vfs_fchmod(vfs_file_t *f, unsigned int mode);
int vfs_fchown(vfs_file_t *f, int uid, int gid);
int vfs_utimens(const char *path, int nofollow, const struct vfs_utimes *t);
int vfs_futimens(vfs_file_t *f, const struct vfs_utimes *t);
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
int vfs_check_access_file(vfs_file_t *f, const struct kstat *st, int want);
int vfs_check_access(const char *path, const struct kstat *st, int want,
		     int use_real);
/* The owning filesystem's ->permission hook alone (ST_UNSUPPORTED: none). */
int vfs_fs_permission(const char *path, unsigned long ino, int want);

/* Flush every mounted filesystem's pending metadata + journal to disk
 * (sync(2)).  fs-independent. */
int vfs_sync(void);
size_t vfs_size(vfs_file_t *f);
vfs_file_t *
vfs_dup(vfs_file_t *f); // Increment refcount and return same pointer
void vfs_incref(vfs_file_t *f); // Increment refcount
/* Take a reference unless the last one is already gone (its close has begun).
 * Returns 1 when taken.  `f' must be known to be allocated. */
int vfs_tryget(vfs_file_t *f);

/* Force-release any filesystem-private locks owned by the given task id.
 * Used by the scheduler's dead-thread reaper to recover from tasks killed
 * mid-syscall while holding such locks.  Reaches every filesystem instance
 * that still exists, unmounted-but-busy and being-unmounted ones included. */
void vfs_release_locks_for_task(uint64_t task_id);
/* Drop the mount holds (vfs_mount.active) a dead task took for a path
 * operation it never finished (kernel/fs/mount.c).  From the task's
 * teardown, after vfs_release_locks_for_task(). */
void vfs_mnt_release_task(struct task *t);

/* ---- Symbolic links ---------------------------------------------------------
 *
 * Symbolic links are followed by the VFS, so a link can lead from one
 * mounted filesystem into another: an absolute target from the process's
 * root (its chroot), a relative one from the link's directory, ".." across a
 * mount's root into the directory it is mounted on.  A filesystem walking a
 * path it was handed calls vfs_link_hop() when it meets a link it would
 * follow -- any link before the last component, and the last one when the
 * operation follows it -- with the path, the byte offsets of the link's
 * component in it, and the link's target.  1: the VFS takes the walk over
 * from there, and the filesystem returns ST_LINK at once, having changed
 * nothing; the operation is then run again on the path the link leads to.
 * 0: the VFS does not take it (no operation of its own is in progress, or
 * the path is not the operation's), and the filesystem follows the link
 * itself as it always did.  A path with no link on it costs nothing. */
int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long comp_start,
		 unsigned long comp_end, const char *target);

#endif // LIKEOS_VFS_H
