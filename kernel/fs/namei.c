// LikeOS-64 -- path resolution, permission checks and name-space syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/namei.h>

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
	char full[VFS_MAX_PATH];
	int ret;
	size_t i;

	if (!cur || !path || size < 2)
		return -EINVAL;

	ret = build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
	if (ret != 0)
		return ret;

	for (i = 0; i + 1 < size && full[i]; i++)
		path[i] = full[i];
	path[i] = '\0';
	/* Truncating would name a different file; refuse instead. */
	if (full[i] != '\0')
		return -ENAMETOOLONG;
	return 0;
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
	int tr = perm_traverse(path);
	if (tr < 0)
		return tr;
	int want = 0, acc = (int)(flags & 3);
	if (acc == O_RDONLY || acc == O_RDWR)
		want |= MAY_READ;
	if (acc == O_WRONLY || acc == O_RDWR)
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

int64_t sys_unlink(uint64_t pathname)
{
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_remove(
		kpath); /* parent write+search, + sticky bit */
	if (pr < 0)
		return pr;
	int st = vfs_unlink(kpath);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	return -EINVAL;
}

int64_t sys_rename(uint64_t oldpath, uint64_t newpath)
{
	if (!validate_user_ptr(oldpath, 1) || !validate_user_ptr(newpath, 1))
		return -EFAULT;

	// Copy user paths to kernel buffers first
	char koldpath[VFS_MAX_PATH], knewpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)oldpath, koldpath,
				  sizeof(koldpath));
	if (cret != 0)
		return cret;
	cret = copy_user_path((const char *)newpath, knewpath,
			      sizeof(knewpath));
	if (cret != 0)
		return cret;

	cret = canon_task_path(koldpath, sizeof(koldpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(knewpath, sizeof(knewpath));
	if (cret != 0)
		return cret;

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
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	return -EINVAL;
}

int64_t sys_mkdir(uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_parent(kpath,
				   MAY_WRITE | MAY_EXEC); /* write the dir */
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
	return -EINVAL;
}

int64_t sys_rmdir(uint64_t pathname)
{
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_remove(
		kpath); /* parent write+search, + sticky bit */
	if (pr < 0)
		return pr;
	int st = vfs_rmdir(kpath);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOTEMPTY)
		return -ENOTEMPTY;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	return -EINVAL;
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

	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	int brest = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
	if (brest != 0)
		return brest;

	int pr = perm_check_remove(full);
	if (pr < 0)
		return pr;

	int st = (flags & AT_REMOVEDIR) ? vfs_rmdir(full) : vfs_unlink(full);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOTEMPTY)
		return -ENOTEMPTY;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	return -EINVAL;
}

int64_t sys_link(uint64_t oldpath, uint64_t newpath)
{
	char kold[VFS_MAX_PATH], knew[VFS_MAX_PATH];
	int c = copy_user_path((const char *)oldpath, kold, sizeof(kold));
	if (c)
		return c;
	c = copy_user_path((const char *)newpath, knew, sizeof(knew));
	if (c)
		return c;
	c = canon_task_path(kold, sizeof(kold));
	if (c)
		return c;
	c = canon_task_path(knew, sizeof(knew));
	if (c)
		return c;
	int pr = perm_check_parent(
		knew, MAY_WRITE | MAY_EXEC); /* write the new dir */
	if (pr < 0)
		return pr;
	int r = vfs_link(kold, knew);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no hard links  */
	return vfs_status_to_errno(r);
}

int64_t sys_symlink(uint64_t target, uint64_t linkpath)
{
	char ktarget[VFS_MAX_PATH], klink[VFS_MAX_PATH];
	int c = copy_user_path((const char *)target, ktarget, sizeof(ktarget));
	if (c)
		return c;
	c = copy_user_path((const char *)linkpath, klink, sizeof(klink));
	if (c)
		return c;
	/* Only the link's own name.  `target` is the link's CONTENT, stored
	 * verbatim: resolving it would turn a relative symlink into an absolute
	 * one naming a different file. */
	c = canon_task_path(klink, sizeof(klink));
	if (c)
		return c;
	int pr = perm_check_parent(
		klink, MAY_WRITE | MAY_EXEC); /* write the new dir */
	if (pr < 0)
		return pr;
	int r = vfs_symlink(ktarget, klink);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no symlinks    */
	return vfs_status_to_errno(r);
}

int normalize_path(const char *base, const char *path, char *out,
		   size_t out_size)
{
	if (!path || !out || out_size < 2)
		return -EINVAL;
	const char *base_path = (base && base[0]) ? base : "/";
	char combined[VFS_MAX_PATH];
	size_t ci = 0;

	if (path[0] == '/') {
		// Absolute path: copy as-is into combined
		while (path[ci] && ci < sizeof(combined) - 1) {
			combined[ci] = path[ci];
			ci++;
		}
	} else {
		// Relative path: base + '/' + path
		size_t bi = 0;
		while (base_path[bi] && ci < sizeof(combined) - 1) {
			combined[ci++] = base_path[bi++];
		}
		if (ci == 0 || combined[ci - 1] != '/') {
			if (ci < sizeof(combined) - 1)
				combined[ci++] = '/';
		}
		size_t pi = 0;
		while (path[pi] && ci < sizeof(combined) - 1) {
			combined[ci++] = path[pi++];
		}
	}
	combined[ci] = '\0';

	// Normalize combined into out
	size_t out_len = 0;
	size_t seg_stack[64];
	size_t seg_top = 0;

	out[out_len++] = '/';
	size_t i = 0;
	while (combined[i]) {
		while (combined[i] == '/')
			i++;
		if (!combined[i])
			break;
		/* One name, which POSIX allows to be NAME_MAX bytes.
		 *
		 * This buffer used to hold 64, and the loop below simply
		 * stopped copying when it filled -- without advancing past the
		 * rest of the name.  The remainder was then taken for the NEXT
		 * component, so "…/averylongname.ext" quietly became
		 * "…/averylongnam/e.ext": a different file, in a directory that
		 * does not exist.  Every path with a component over 63
		 * characters was affected, which is why creating one could
		 * succeed and removing it could not. */
		char segment[VFS_NAME_MAX + 1];
		size_t si = 0;
		while (combined[i] && combined[i] != '/') {
			if (si >= sizeof(segment) - 1)
				return -ENAMETOOLONG;
			segment[si++] = combined[i++];
		}
		segment[si] = '\0';

		if (segment[0] == '\0' ||
		    (segment[0] == '.' && segment[1] == '\0')) {
			continue;
		}
		if (segment[0] == '.' && segment[1] == '.' &&
		    segment[2] == '\0') {
			if (seg_top > 0) {
				out_len = seg_stack[--seg_top];
				out[out_len] = '\0';
			} else {
				out_len = 1;
				out[1] = '\0';
			}
			continue;
		}

		if (out_len > 1 && out[out_len - 1] != '/') {
			if (out_len < out_size - 1)
				out[out_len++] = '/';
		}
		if (out_len >= out_size - 1)
			return -EINVAL;
		seg_stack[seg_top++] = out_len;
		for (size_t j = 0; j < si && out_len < out_size - 1; ++j) {
			out[out_len++] = segment[j];
		}
		out[out_len] = '\0';
		if (seg_top >= (sizeof(seg_stack) / sizeof(seg_stack[0]))) {
			return -EINVAL;
		}
	}

	if (out_len > 1 && out[out_len - 1] == '/') {
		out[out_len - 1] = '\0';
	} else {
		out[out_len] = '\0';
	}
	return 0;
}

/* Prepend the task's chroot root to an already-canonical absolute path.
 * `abs` starts with '/', has no ".." (normalize_path guarantees both), so the
 * result stays inside the jail.  A no-op when the task is not chrooted. */
static int apply_chroot(task_t *cur, char *abs, size_t out_size)
{
	if (!cur || cur->root[0] == '\0')
		return 0;
	size_t rlen = 0;
	while (cur->root[rlen])
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
			abs[i] = cur->root[i];
	} else {
		for (size_t i = alen + 1; i-- > 0;)
			abs[i + rlen] = abs[i];
		for (size_t i = 0; i < rlen; i++)
			abs[i] = cur->root[i];
	}
	return 0;
}

int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
		  size_t out_size)
{
	const char *base;

	if (!cur || !path || !out || out_size < 2)
		return -EINVAL;

	/* An ABSOLUTE path ignores dirfd entirely, as POSIX requires -- the
	 * descriptor is not even required to be valid in that case. */
	if (path[0] == '/' || dirfd == AT_FDCWD) {
		base = (cur->cwd[0] != 0) ? cur->cwd : "/";
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
		df = task_fds(cur)[dirfd];
		if (!df)
			return -EBADF;
		/* The marker descriptors (sockets, epoll, pipes, the console)
		 * are not files and have no path; a directory is required. */
		if (IS_SOCKET_FD(df) || unix_sock_is(df) || IS_EPOLL_FD(df) ||
		    pipe_is_end(df) || (uintptr_t)df <= 3)
			return -ENOTDIR;
		if (!df->at_path)
			return -ENOTDIR;
		/* It must really be a directory: resolving "file" against a
		 * regular file would otherwise invent a path that looks valid
		 * and refers to nothing. */
		{
			struct kstat dst;
			if (vfs_fstat(df, &dst) != ST_OK)
				return -ENOTDIR;
			if (!S_ISDIR(dst.st_mode))
				return -ENOTDIR;
		}
		base = df->at_path;
	}

	int r = normalize_path(base, path, out, out_size);
	if (r != 0)
		return r;
	return apply_chroot(cur, out, out_size);
}
