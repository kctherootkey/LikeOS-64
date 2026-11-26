// LikeOS -- path resolution, permission checks and name-space syscalls.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/namei.h>
#include <kernel/fs/file.h>
#include <kernel/io/tty.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/ke/task_path.h>

/*
 * Resolve a path against THIS task's working directory (and chroot), in place.
 *
 * Every syscall that names a file has to do this before the VFS sees the name.
 * A relative path that gets through unresolved is resolved much further down,
 * against a single "current directory" that the entire system shares -- so it
 * names whatever directory some other process happened to change into last.
 * The result is a call that operates on a completely different file than the
 * caller meant, or reports that a file it had just successfully stat'd does not
 * exist.  Rename was the one that showed it: a mail client found its
 * configuration directory, failed to rename it, and refused to start.
 */
int canon_task_path(char *path, size_t size)
{
	task_t *cur = sched_current();
	char *full;
	int ret;
	size_t i;

	if (!cur || !path || size < 2)
		return -EINVAL;

	full = pathbuf_get();
	if (!full)
		return -ENOMEM;
	ret = build_at_path(cur, AT_FDCWD, path, full, PATHBUF_SIZE);
	if (ret != 0)
		goto out;

	for (i = 0; i + 1 < size && full[i]; i++)
		path[i] = full[i];
	path[i] = '\0';
	/* Truncating would name a different file; refuse instead. */
	if (full[i] != '\0')
		ret = -ENAMETOOLONG;
out:
	pathbuf_put(full);
	return ret;
}

/* A user path copied into a new path buffer (put by the caller): 0 with *out
 * set, or -errno (-EFAULT, -ENAMETOOLONG, -ENOMEM) with *out NULL. */
int user_path_get(uint64_t upath, char **out)
{
	char *b;
	int r;

	*out = NULL;
	b = pathbuf_get();
	if (!b)
		return -ENOMEM;
	r = copy_user_path((const char *)upath, b, PATHBUF_SIZE);
	if (r != 0) {
		pathbuf_put(b);
		return r;
	}
	*out = b;
	return 0;
}

/* A user path made absolute against the task's working directory (and
 * chroot), in a new path buffer: user_path_get() + canon_task_path(). */
int user_path_get_canon(uint64_t upath, char **out)
{
	int r = user_path_get(upath, out);

	if (r != 0)
		return r;
	r = canon_task_path(*out, PATHBUF_SIZE);
	if (r != 0) {
		pathbuf_put(*out);
		*out = NULL;
	}
	return r;
}

// Convert VFS status codes to negative errno values
int vfs_status_to_errno(int st)
{
	switch (st) {
	case ST_NOT_FOUND:
		return -ENOENT;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_INVALID:
		return -EINVAL;
	case ST_IO:
		return -EIO;
	case ST_EXISTS:
		return -EEXIST;
	case ST_BUSY:
		return -EBUSY;
	case ST_AGAIN:
		return -EAGAIN;
	case ST_NOTEMPTY:
		return -ENOTEMPTY;
	case ST_ROFS:
		return -EROFS;
	case ST_NOSPC:
		return -ENOSPC;
	case ST_NODATA:
		return -ENODATA;
	case ST_RANGE:
		return -ERANGE;
	case ST_UNSUPPORTED:
		return -EOPNOTSUPP;
	case ST_ACCESS:
		return -EACCES;
	case ST_PERM:
		return -EPERM;
	case ST_XDEV:
		return -EXDEV;
	case ST_NO_DEVICE:
		return -ENODEV;
	case ST_ISDIR:
		return -EISDIR;
	case ST_NOTDIR:
		return -ENOTDIR;
	case ST_LOOP:
	case ST_LINK: /* never meant to get out of the VFS */
		return -ELOOP;
	case ST_NAMETOOLONG:
		return -ENAMETOOLONG;
	case ST_NXIO:
		return -ENXIO;
	case ST_UCLEAN:
		return -EUCLEAN;
	default:
		return -EACCES;
	}
}

/* ---- Permission checks: thin adapters over the canonical VFS policy --------
 * The discretionary-access policy lives in the VFS now (vfs_permission and
 * friends — the one place every filesystem shares), so these are just adapters
 * that translate the VFS's ST_ result into the negative-errno the syscalls
 * return.  They let a credential-sensitive syscall screen an operation and
 * report a precise errno; the VFS re-checks authoritatively when the operation
 * actually runs, so removing any of these pre-checks would not weaken security. */
static int perm_st_errno(int st)
{
	return (st == ST_OK) ? 0 : vfs_status_to_errno(st);
}

/* Access check for a file whose stat is `st`, against the ACL then mode bits.
 * use_real selects the real vs effective/fs ids. */
int perm_access(task_t *cur, const char *path, const struct kstat *st,
		int want, int use_real)
{
	(void)cur; /* the VFS reads the current task's credentials itself */
	/* A device node's own grant first: devfs gives the user logged in on
	 * the console read and write on the seat's nodes (devfs_permission()).
	 * It only ever grants, so it is asked here, on the path every open
	 * and access() of a /dev node takes, and nowhere else -- every other
	 * filesystem's decision stays the ACL and the mode bits. */
	if (kstrncmp(path, "/dev/", 5) == 0 &&
	    vfs_fs_permission(path, (unsigned long)st->st_ino, want) == ST_OK)
		return 0;
	return perm_st_errno(vfs_check_access(path, st, want, use_real));
}

/* Search (x) permission on every ancestor directory of `path` (effective ids). */
int perm_traverse(const char *rawpath)
{
	return perm_st_errno(vfs_permission_traverse(rawpath));
}

/* Like perm_traverse but with an explicit real(1)/effective(0) id selection,
 * for access(2)/faccessat which screen the prefix with the real ids. */
int perm_traverse_cred(const char *rawpath, int use_real)
{
	return perm_st_errno(vfs_access_traverse(rawpath, use_real));
}

/* Write+search on the PARENT directory of `path` (create/remove/rename). */
static int perm_check_parent(const char *rawpath, int want)
{
	return perm_st_errno(vfs_permission_parent(rawpath, want));
}

/* Creating a name: the ancestors must be searchable -- finding the name takes
 * that -- then a name that is already there is EEXIST, and only then is write
 * permission on the parent asked for.  Whether a name exists is not a
 * question of being allowed to create it: `mkdir -p /var/lib/dbus' run by an
 * account that may not write /var/lib has to learn that the directory is
 * there, not that it may not make it. */
static int perm_check_create(const char *rawpath)
{
	struct kstat st;
	int tr = perm_traverse(rawpath);

	if (tr < 0)
		return tr;
	if (vfs_lstat(rawpath, &st) == ST_OK)
		return -EEXIST;
	return perm_check_parent(rawpath, MAY_WRITE | MAY_EXEC);
}

/* Remove/rename gate: parent write+search plus the directory's sticky-bit rule.
 * The sticky-bit ownership check now lives in the VFS (vfs_permission_remove). */
static int perm_check_remove(const char *rawpath)
{
	return perm_st_errno(vfs_permission_remove(rawpath));
}

/* The set-user/-group-ID bits a successful modification (write/chown) by a
 * non-privileged caller must clear, to stop a set-id file outliving a change to
 * its contents or ownership.  S_ISUID is always cleared; S_ISGID only when the
 * file is group-executable (otherwise that bit is a mandatory-lock marker, not
 * a privilege).  Returns the bits to clear (0 = nothing to do). */
unsigned setid_strip_bits(uint32_t mode)
{
	unsigned clr = 0;
	if (mode & S_ISUID)
		clr |= S_ISUID;
	if ((mode & S_ISGID) && (mode & S_IXGRP))
		clr |= S_ISGID;
	return clr;
}

/* Permission screen for /dev opens.  Devfs opens are dispatched directly to
 * devfs_open_for_task (they need the task context for /dev/tty), bypassing
 * vfs_open's canonical enforcement — so the DAC decision vfs_open would have
 * made is applied here instead: ancestor search plus the open mode against the
 * device node's reported ownership/mode bits. */
int devfs_open_perm(task_t *cur, const char *path, uint64_t flags)
{
	if (cur->cred.euid == 0)
		return 0;
	/* /dev/tty is the caller's controlling terminal, and a caller without
	 * one -- or with one its session no longer holds -- has none to open.
	 * devfs refuses the first case for everyone; the second is refused
	 * here for anyone but root. */
	if (kstrcmp(path, "/dev/tty") == 0 &&
	    (!cur->ctty || cur->ctty->sid != cur->sid))
		return -ENXIO;
	int tr = perm_traverse(path);
	if (tr < 0)
		return tr;
	int want = 0, acc = (int)(flags & 3);
	/* Access mode 3 (ioctls only) needs both, as in vfs_open_common(). */
	if (acc == O_RDONLY || acc == O_RDWR || acc == 3)
		want |= MAY_READ;
	if (acc == O_WRONLY || acc == O_RDWR || acc == 3)
		want |= MAY_WRITE;
	if (flags & O_TRUNC)
		want |= MAY_WRITE;
	if (!want)
		return 0;
	struct kstat est;
	if (vfs_stat(path, &est) != ST_OK)
		return 0; /* unresolved: let the open report ENOENT */
	return perm_access(cur, path, &est, want, 0);
}

/* Drop the set-id bits from an open file after a content modification.  Caller
 * gates on non-root + success.  Runs at most once per INODE: the first call
 * evaluates the mode (one inode read) and clears any set-id bits, then marks
 * the inode "clean" (shared across every handle) so later writes short-circuit
 * — the reference's S_NOSEC amortisation.  The fs clears the hint on any mode
 * change, so a re-added set-id bit is re-evaluated even via another fd.  A
 * no-op when the fs can't report the mode (e.g. the perm-less FAT path). */
void strip_setid_file(vfs_file_t *file)
{
	if (vfs_setid_clean(file))
		return; /* already evaluated for this inode */
	struct kstat st;
	if (vfs_fstat(file, &st) == ST_OK) {
		unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
		if (clr)
			vfs_fchmod(file, (unsigned)st.st_mode & ~clr);
	}
	vfs_mark_setid_clean(file); /* mark AFTER fchmod's invalidation */
}

// SYS_OPEN - open a file
/* The mode a create-style syscall should hand the filesystem: the requested
 * permission bits minus the caller's umask, which is what POSIX specifies.
 * Every path that creates a name goes through here so the mask cannot be
 * applied in one place and forgotten in another. */
unsigned int creat_mode(task_t *cur, uint64_t mode)
{
	return (unsigned int)mode & 0777 & ~task_umask(cur) & 0777;
}

/* The errno for a failed unlink, rmdir or rename.  Callers act on it --
 * remove() falls back to rmdir() on EISDIR, a file manager copies and
 * deletes on EXDEV -- so every status the filesystems report keeps its
 * meaning; only a status with no errno of its own is EINVAL. */
static int64_t name_op_errno(int st)
{
	if (st == ST_ERR || st == ST_TIMEOUT || st == ST_EOF ||
	    st < ST_NXIO || st > ST_OK)
		return -EINVAL;
	return vfs_status_to_errno(st);
}

int64_t sys_unlink(uint64_t pathname)
{
	char *kpath;
	int64_t ret;
	int st;

	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	int cret = user_path_get_canon(pathname, &kpath);
	if (cret != 0)
		return cret;

	ret = perm_check_remove(kpath); /* parent write+search, + sticky bit */
	if (ret < 0)
		goto out;
	st = vfs_unlink(kpath);
	ret = (st == ST_OK) ? 0 : name_op_errno(st);
out:
	pathbuf_put(kpath);
	return ret;
}

static int64_t rename_paths(const char *koldpath, const char *knewpath);
static int64_t mkdir_path(task_t *cur, const char *kpath, uint64_t mode);
static int64_t link_paths(const char *kold, const char *knew, int follow);
static int64_t symlink_path(const char *ktarget, const char *klink);

int64_t sys_rename(uint64_t oldpath, uint64_t newpath)
{
	if (!validate_user_ptr(oldpath, 1) || !validate_user_ptr(newpath, 1))
		return -EFAULT;

	// Copy user paths to kernel buffers first
	char *koldpath = NULL, *knewpath = NULL;
	int64_t ret = user_path_get(oldpath, &koldpath);
	if (ret == 0)
		ret = user_path_get(newpath, &knewpath);
	if (ret == 0)
		ret = canon_task_path(koldpath, PATHBUF_SIZE);
	if (ret == 0)
		ret = canon_task_path(knewpath, PATHBUF_SIZE);
	if (ret == 0)
		ret = rename_paths(koldpath, knewpath);
	pathbuf_put(knewpath);
	pathbuf_put(koldpath);
	return ret;
}

static int64_t rename_paths(const char *koldpath, const char *knewpath)
{
	int pr = perm_check_remove(
		koldpath); /* remove source (+ sticky)        */
	if (pr < 0)
		return pr;
	pr = perm_check_remove(
		knewpath); /* write dest (+ sticky on overwrite) */
	if (pr < 0)
		return pr;
	int st = vfs_rename(koldpath, knewpath);
	if (st == ST_OK)
		return 0;
	return name_op_errno(st);
}

/* renameat2(olddirfd, oldpath, newdirfd, newpath, flags): rename() with
 * each name relative to its own directory descriptor.  RENAME_NOREPLACE
 * refuses an existing new name with EEXIST; RENAME_EXCHANGE (swapping two
 * names) is not something the filesystems here can do in one step, and is
 * refused with EINVAL. */
int64_t sys_renameat2(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd,
		      uint64_t newpath, uint64_t flags)
{
	task_t *cur = sched_current();
	char *kold = NULL, *knew = NULL, *fold = NULL, *fnew = NULL;
	int64_t c;

	if (!cur)
		return -EFAULT;
	if (flags & ~((uint64_t)RENAME_NOREPLACE | RENAME_EXCHANGE))
		return -EINVAL;
	if (flags & RENAME_EXCHANGE)
		return -EINVAL;
	c = user_path_get(oldpath, &kold);
	if (c)
		goto out;
	c = user_path_get(newpath, &knew);
	if (c)
		goto out;
	if (!kold[0] || !knew[0]) {
		c = -ENOENT;
		goto out;
	}
	fold = pathbuf_get();
	fnew = pathbuf_get();
	if (!fold || !fnew) {
		c = -ENOMEM;
		goto out;
	}
	c = build_at_path(cur, (int)olddirfd, kold, fold, PATHBUF_SIZE);
	if (c)
		goto out;
	c = build_at_path(cur, (int)newdirfd, knew, fnew, PATHBUF_SIZE);
	if (c)
		goto out;
	if (flags & RENAME_NOREPLACE) {
		struct kstat st;
		/* Checked, not held: a name that appears between the check
		 * and the rename is replaced. */
		if (vfs_lstat(fnew, &st) == ST_OK) {
			c = -EEXIST;
			goto out;
		}
	}
	c = rename_paths(fold, fnew);
out:
	pathbuf_put(fnew);
	pathbuf_put(fold);
	pathbuf_put(knew);
	pathbuf_put(kold);
	return c;
}

int64_t sys_mkdir(uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char *kpath;
	int64_t ret = user_path_get_canon(pathname, &kpath);
	if (ret != 0)
		return ret;
	ret = mkdir_path(cur, kpath, mode);
	pathbuf_put(kpath);
	return ret;
}

static int64_t mkdir_path(task_t *cur, const char *kpath, uint64_t mode)
{
	int pr = perm_check_create(kpath);
	if (pr < 0)
		return pr;
	/* umask applies to directories too; the raw mode was being passed
	 * straight through, so `mkdir -m 700` and a default 0777 mkdir both
	 * ignored the caller's mask. */
	int st = vfs_mkdir(kpath, creat_mode(cur, mode));
	if (st == ST_OK)
		return 0;
	if (st == ST_EXISTS)
		return -EEXIST;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	if (st == ST_ROFS || st == ST_NOSPC || st == ST_ACCESS ||
	    st == ST_PERM || st == ST_NOTDIR)
		return vfs_status_to_errno(st);
	return -EINVAL;
}

/* mkdirat(dirfd, path, mode) */
int64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	char *kpath = NULL, *full = NULL;
	int64_t c;

	if (!cur)
		return -EFAULT;
	c = user_path_get(pathname, &kpath);
	if (c)
		return c;
	if (!kpath[0]) {
		c = -ENOENT;
		goto out;
	}
	full = pathbuf_get();
	if (!full) {
		c = -ENOMEM;
		goto out;
	}
	c = build_at_path(cur, (int)dirfd, kpath, full, PATHBUF_SIZE);
	if (c)
		goto out;
	c = mkdir_path(cur, full, mode);
out:
	pathbuf_put(full);
	pathbuf_put(kpath);
	return c;
}

/* mknodat(dirfd, path, mode, dev): a regular file, FIFO, socket, or -- for
 * the privileged caller -- a character or block device node numbered
 * `dev'.  The permission bits are reduced by the umask. */
int64_t sys_mknodat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
		    uint64_t dev)
{
	task_t *cur = sched_current();
	char *kpath = NULL, *full = NULL;
	unsigned type = (unsigned)mode & S_IFMT;
	int64_t c;

	if (!cur)
		return -EFAULT;
	if (type != 0 && type != S_IFREG && type != S_IFIFO &&
	    type != S_IFSOCK && type != S_IFCHR && type != S_IFBLK)
		return -EINVAL; /* directories are mkdir's; links symlink's */
	if ((type == S_IFCHR || type == S_IFBLK) && !capable())
		return -EPERM;
	c = user_path_get(pathname, &kpath);
	if (c)
		return c;
	if (!kpath[0]) {
		c = -ENOENT;
		goto out;
	}
	full = pathbuf_get();
	if (!full) {
		c = -ENOMEM;
		goto out;
	}
	c = build_at_path(cur, (int)dirfd, kpath, full, PATHBUF_SIZE);
	if (c)
		goto out;
	c = perm_check_create(full);
	if (c < 0)
		goto out;
	/* The set-id and sticky bits are kept; the umask takes the rest. */
	unsigned perm = creat_mode(cur, mode) | ((unsigned)mode & 07000);
	int st;
	if (type == 0 || type == S_IFREG) {
		vfs_file_t *f = NULL;
		st = vfs_open_mode(full, O_WRONLY | O_CREAT | O_EXCL, perm, &f);
		if (st == ST_OK && f)
			vfs_close(f);
	} else {
		st = vfs_mknod(full, type | perm, dev);
	}
	if (st == ST_OK)
		c = 0;
	else if (st == ST_UNSUPPORTED)
		c = -EPERM; /* the filesystem cannot hold such a node */
	else
		c = vfs_status_to_errno(st);
out:
	pathbuf_put(full);
	pathbuf_put(kpath);
	return c;
}

int64_t sys_rmdir(uint64_t pathname)
{
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char *kpath;
	int64_t ret;
	int st;
	int cret = user_path_get_canon(pathname, &kpath);
	if (cret != 0)
		return cret;

	ret = perm_check_remove(kpath); /* parent write+search, + sticky bit */
	if (ret < 0)
		goto out;
	st = vfs_rmdir(kpath);
	ret = (st == ST_OK) ? 0 : name_op_errno(st);
out:
	pathbuf_put(kpath);
	return ret;
}

/* unlinkat(dirfd, path, flags) -- remove a name relative to a directory fd.
 *
 * One syscall covering both unlink() and rmdir(), which is how POSIX defines
 * it: AT_REMOVEDIR selects the directory case.  Everything else -- the
 * relative-path resolution against dirfd (and the caller's chroot), the
 * parent write+search check and the sticky-bit rule -- is the same machinery
 * the non-at versions use, so the two cannot drift apart in policy. */
int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;
	/* Reject flags we do not implement rather than ignoring them: a caller
	 * that passes one is asking for behaviour we would not deliver. */
	if (flags & ~((uint64_t)AT_REMOVEDIR))
		return -EINVAL;

	char *kpath = NULL, *full = NULL;
	int64_t ret;
	int st;

	ret = user_path_get(pathname, &kpath);
	if (ret != 0)
		return ret;

	full = pathbuf_get();
	if (!full) {
		ret = -ENOMEM;
		goto out;
	}
	ret = build_at_path(cur, (int)dirfd, kpath, full, PATHBUF_SIZE);
	if (ret != 0)
		goto out;

	ret = perm_check_remove(full);
	if (ret < 0)
		goto out;

	st = (flags & AT_REMOVEDIR) ? vfs_rmdir(full) : vfs_unlink(full);
	ret = (st == ST_OK) ? 0 : name_op_errno(st);
out:
	pathbuf_put(full);
	pathbuf_put(kpath);
	return ret;
}

int64_t sys_link(uint64_t oldpath, uint64_t newpath)
{
	char *kold = NULL, *knew = NULL;
	int64_t c = user_path_get(oldpath, &kold);
	if (c == 0)
		c = user_path_get(newpath, &knew);
	if (c == 0)
		c = canon_task_path(kold, PATHBUF_SIZE);
	if (c == 0)
		c = canon_task_path(knew, PATHBUF_SIZE);
	/* A final symlink is linked itself, as linkat() does by default. */
	if (c == 0)
		c = link_paths(kold, knew, 0);
	pathbuf_put(knew);
	pathbuf_put(kold);
	return c;
}

static int64_t link_paths(const char *kold, const char *knew, int follow)
{
	/* The existing name is looked up first: a missing one is ENOENT
	 * whatever the new name is. */
	struct kstat ost;
	int lr = follow ? vfs_stat(kold, &ost) : vfs_lstat(kold, &ost);
	if (lr != ST_OK)
		return vfs_status_to_errno(lr);
	if (S_ISDIR(ost.st_mode))
		return -EPERM; /* no hard links to directories */
	int pr = perm_check_create(knew);
	if (pr < 0)
		return pr;
	int r = vfs_link(kold, knew, follow);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no hard links  */
	return vfs_status_to_errno(r);
}

/* linkat(olddirfd, oldpath, newdirfd, newpath, flags): link() with each
 * name relative to its own directory descriptor.  A final symlink in
 * oldpath is linked itself unless AT_SYMLINK_FOLLOW; with AT_EMPTY_PATH an
 * empty oldpath names the file olddirfd is open on (privileged). */
int64_t sys_linkat(uint64_t olddirfd, uint64_t oldpath, uint64_t newdirfd,
		   uint64_t newpath, uint64_t flags)
{
	task_t *cur = sched_current();
	char *kold = NULL, *knew = NULL, *fold = NULL, *fnew = NULL;
	int64_t c;

	if (!cur)
		return -EFAULT;
	if (flags & ~((uint64_t)AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))
		return -EINVAL;
	c = user_path_get(oldpath, &kold);
	if (c)
		goto out;
	c = user_path_get(newpath, &knew);
	if (c)
		goto out;
	if (!knew[0]) {
		c = -ENOENT;
		goto out;
	}
	fold = pathbuf_get();
	fnew = pathbuf_get();
	if (!fold || !fnew) {
		c = -ENOMEM;
		goto out;
	}
	if (!kold[0]) {
		if (!(flags & AT_EMPTY_PATH) || !capable()) {
			c = -ENOENT;
			goto out;
		}
		/* The descriptor's own file, by the path it was opened as. */
		vfs_file_t *df = fdget(cur, (int)olddirfd);
		if (!df) {
			c = -EBADF;
			goto out;
		}
		c = 0;
		if (fd_is_special(df) || !df->at_path)
			c = -ENOENT;
		else
			kstrncpy(fold, df->at_path, PATHBUF_SIZE);
		fdput(df);
		if (c)
			goto out;
		fold[PATHBUF_SIZE - 1] = '\0';
	} else {
		c = build_at_path(cur, (int)olddirfd, kold, fold, PATHBUF_SIZE);
		if (c)
			goto out;
	}
	c = build_at_path(cur, (int)newdirfd, knew, fnew, PATHBUF_SIZE);
	if (c)
		goto out;
	c = link_paths(fold, fnew, (flags & AT_SYMLINK_FOLLOW) ? 1 : 0);
out:
	pathbuf_put(fnew);
	pathbuf_put(fold);
	pathbuf_put(knew);
	pathbuf_put(kold);
	return c;
}

int64_t sys_symlink(uint64_t target, uint64_t linkpath)
{
	char *ktarget = NULL, *klink = NULL;
	int64_t c = user_path_get(target, &ktarget);
	if (c == 0)
		c = user_path_get(linkpath, &klink);
	/* Only the link's own name.  `target` is the link's CONTENT, stored
	 * verbatim: resolving it would turn a relative symlink into an absolute
	 * one naming a different file. */
	if (c == 0)
		c = canon_task_path(klink, PATHBUF_SIZE);
	if (c == 0)
		c = symlink_path(ktarget, klink);
	pathbuf_put(klink);
	pathbuf_put(ktarget);
	return c;
}

static int64_t symlink_path(const char *ktarget, const char *klink)
{
	if (!ktarget[0])
		return -ENOENT;
	int pr = perm_check_create(klink);
	if (pr < 0)
		return pr;
	int r = vfs_symlink(ktarget, klink);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no symlinks    */
	return vfs_status_to_errno(r);
}

/* symlinkat(target, newdirfd, linkpath): the link's own name relative to
 * the descriptor; the target is stored as written. */
int64_t sys_symlinkat(uint64_t target, uint64_t newdirfd, uint64_t linkpath)
{
	task_t *cur = sched_current();
	char *ktarget = NULL, *klink = NULL, *full = NULL;
	int64_t c;

	if (!cur)
		return -EFAULT;
	c = user_path_get(target, &ktarget);
	if (c)
		goto out;
	c = user_path_get(linkpath, &klink);
	if (c)
		goto out;
	if (!klink[0]) {
		c = -ENOENT;
		goto out;
	}
	full = pathbuf_get();
	if (!full) {
		c = -ENOMEM;
		goto out;
	}
	c = build_at_path(cur, (int)newdirfd, klink, full, PATHBUF_SIZE);
	if (c)
		goto out;
	c = symlink_path(ktarget, full);
out:
	pathbuf_put(full);
	pathbuf_put(klink);
	pathbuf_put(ktarget);
	return c;
}

/* Append the components of s[0, slen) (up to a NUL) to the canonical
 * absolute path out[0, *lenp): "." is dropped, ".." takes the last component
 * off (never above "/"), any other name is added after a slash.
 * -ENAMETOOLONG for a name longer than VFS_NAME_MAX or a result that would
 * not fit in `out_size' bytes. */
int normalize_path_append(char *out, size_t out_size, size_t *lenp,
			  const char *s, size_t slen)
{
	size_t len = *lenp;
	size_t i = 0;

	while (i < slen && s[i]) {
		size_t st, n;

		while (i < slen && s[i] == '/')
			i++;
		if (i >= slen || !s[i])
			break;
		st = i;
		while (i < slen && s[i] && s[i] != '/')
			i++;
		n = i - st;
		/* One name, which POSIX allows to be NAME_MAX bytes and no
		 * more.  A longer one is refused, never cut: the tail of a
		 * cut name would name a different file. */
		if (n > VFS_NAME_MAX)
			return -ENAMETOOLONG;
		if (n == 1 && s[st] == '.')
			continue;
		if (n == 2 && s[st] == '.' && s[st + 1] == '.') {
			/* Back to the slash before the last name. */
			while (len > 1 && out[len - 1] != '/')
				len--;
			if (len > 1)
				len--;
			out[len] = '\0';
			continue;
		}
		if (len + (len > 1 ? 1 : 0) + n >= out_size)
			return -ENAMETOOLONG;
		if (len > 1)
			out[len++] = '/';
		for (size_t k = 0; k < n; k++)
			out[len++] = s[st + k];
		out[len] = '\0';
	}
	*lenp = len;
	return 0;
}

/* `path' -- relative to the absolute directory `base' unless it is absolute
 * itself -- as a canonical absolute path in `out' (`out_size' bytes, which
 * must not overlap either input): no ".", "..", doubled or trailing slash.
 * The two are taken a name at a time, never joined into one string first,
 * so only the result has to fit.  0, -EINVAL, or -ENAMETOOLONG. */
int normalize_path(const char *base, const char *path, char *out,
		   size_t out_size)
{
	size_t len = 1;
	int r;

	if (!path || !out || out_size < 2)
		return -EINVAL;
	out[0] = '/';
	out[1] = '\0';
	if (path[0] != '/') {
		r = normalize_path_append(out, out_size, &len,
					  (base && base[0]) ? base : "/",
					  (size_t)-1);
		if (r != 0)
			return r;
	}
	return normalize_path_append(out, out_size, &len, path, (size_t)-1);
}

/* Prepend the task's chroot root to an already-canonical absolute path.
 * `abs` starts with '/', has no ".." (normalize_path guarantees both), so the
 * result stays inside the jail.  A no-op when the task is not chrooted. */
static int apply_chroot(task_t *cur, char *abs, size_t out_size)
{
	const char *root = task_root(cur); /* (the caller's own) */

	if (!cur || root[0] == '\0')
		return 0;
	size_t rlen = 0;
	while (root[rlen])
		rlen++;
	/* "/" inside the jail is just the jail root itself. */
	size_t alen = 0;
	while (abs[alen])
		alen++;
	int only_slash = (alen == 1 && abs[0] == '/');
	size_t need = rlen + (only_slash ? 0 : alen) + 1;
	if (need > out_size)
		return -ENAMETOOLONG;
	/* Shift abs right by rlen (unless it is bare "/"), then copy root in. */
	if (only_slash) {
		for (size_t i = 0; i <= rlen; i++)
			abs[i] = root[i];
	} else {
		for (size_t i = alen + 1; i-- > 0;)
			abs[i + rlen] = abs[i];
		for (size_t i = 0; i < rlen; i++)
			abs[i] = root[i];
	}
	return 0;
}

/* A real path (a descriptor's at_path) as the task in its chroot sees it: in
 * a chroot the real path starts with the root, and the part below it is the
 * path within the jail (where ".." stops at the jail's top; the root goes in
 * front again afterwards).  A path outside the jail (a descriptor opened
 * before the chroot) leads where it leads: itself, *inside 0, no root to be
 * put in front.  Without a root: `real' itself, *inside 1. */
const char *jail_relative(task_t *cur, const char *real, int *inside)
{
	const char *root = task_root(cur);
	size_t rlen = 0;

	*inside = 1;
	while (root[rlen])
		rlen++;
	if (!rlen)
		return real;
	if (kstrncmp(real, root, rlen) == 0 &&
	    (real[rlen] == '/' || real[rlen] == '\0'))
		return real[rlen] ? real + rlen : "/";
	*inside = 0;
	return real;
}

/* NOTE: `base' can point INTO the directory descriptor's file (its at_path),
 * so the hold taken on that descriptor has to outlive normalize_path(), which
 * is what copies the string out.  That is why this has a single exit rather
 * than releasing as soon as the checks pass. */
int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
		  size_t out_size)
{
	const char *base;
	vfs_file_t *held = NULL;
	int jail = 1; /* the result is inside the task's root */
	int ret;

	if (!cur || !path || !out || out_size < 2)
		return -EINVAL;
	/* An empty path names nothing (POSIX: ENOENT) -- not the directory it
	 * would be relative to.  The *at() calls that take AT_EMPTY_PATH deal
	 * with "" themselves before they get here. */
	if (!path[0])
		return -ENOENT;

	/* An ABSOLUTE path ignores dirfd entirely, as POSIX requires -- the
	 * descriptor is not even required to be valid in that case. */
	if (path[0] == '/' || dirfd == AT_FDCWD) {
		base = task_cwd(cur); /* (the caller's own) */
	} else {
		/* Relative to the directory the descriptor refers to.
		 *
		 * This used to return ENOTDIR for every dirfd that was not
		 * AT_FDCWD, which meant the whole *at() family silently did
		 * not work: openat(), fstatat(), faccessat() and unlinkat()
		 * all fail the moment a caller passes a real descriptor, which
		 * is the entire reason those calls exist. */
		vfs_file_t *df;

		if (dirfd < 0 || dirfd >= (int)TASK_MAX_FDS)
			return -EBADF;
		/* Held: at_path is read off it below, and the resolution that
		 * follows can sleep. */
		df = fdget(cur, dirfd);
		if (!df)
			return -EBADF;
		held = df;
		/* The marker descriptors (sockets, epoll, pipes, the console)
		 * are not files and have no path; a directory is required. */
		if (fd_is_special(df)) {
			ret = -ENOTDIR;
			goto out;
		}
		if (!df->at_path) {
			ret = -ENOTDIR;
			goto out;
		}
		/* It must really be a directory: resolving "file" against a
		 * regular file would otherwise invent a path that looks valid
		 * and refers to nothing. */
		{
			struct kstat dst;
			if (vfs_fstat(df, &dst) != ST_OK) {
				ret = -ENOTDIR;
				goto out;
			}
			if (!S_ISDIR(dst.st_mode)) {
				ret = -ENOTDIR;
				goto out;
			}
		}
		base = jail_relative(cur, df->at_path, &jail);
	}

	ret = normalize_path(base, path, out, out_size);
	if (ret == 0 && jail)
		ret = apply_chroot(cur, out, out_size);
out:
	if (held)
		fdput(held);
	return ret;
}
