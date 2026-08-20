// LikeOS-64 -- open/close and file-attribute syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/syscalls.h>
#include <kernel/fs/file.h>
#include <kernel/fs/namei.h>

static int64_t ftruncate_held(task_t *cur, vfs_file_t *f, uint64_t length);
static int64_t fchmod_held(task_t *cur, vfs_file_t *f, uint64_t mode);
static int64_t fchown_held(task_t *cur, vfs_file_t *f, uint64_t owner, uint64_t group);

int64_t sys_open(uint64_t pathname, uint64_t flags, uint64_t mode)
{
	might_sleep();
	task_t *cur = sched_current();
	BUG_ON(cur == NULL);
	if (!cur)
		return -EFAULT;

	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	vfs_file_t *file = NULL;
	const char *path = kpath;
	char full[VFS_MAX_PATH];
	if (path[0] != '/' || cur->root[0]) {
		int brest =
			build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
		if (brest != 0)
			return brest;
		path = full;
	}

	/* /dev/fd/N and friends: open == duplicate the caller's descriptor */
	int devfd = devfs_fd_alias_target(path);
	if (devfd >= 0)
		return sys_dup((uint64_t)devfd);

	/* No pre-flight permission screening here: vfs_open() enforces the whole
     * policy authoritatively (ancestor search, read/write mode on an existing
     * target, parent write+search for O_CREAT, immutable/append flags) and its
     * ST_ status maps to the same errno.  The duplicate screening made every
     * non-root open re-resolve the path several extra times. */
	int ret;
	if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
	    path[3] == 'v' && (path[4] == '/' || path[4] == '\0')) {
		int pr = devfs_open_perm(cur, path, flags);
		if (pr < 0)
			return pr;
		ret = devfs_open_for_task(path, (int)flags,
					  creat_mode(cur, mode), &file, cur);
		if (ret == ST_OK && file) {
			file->refcount = 1;
			file->flags = (int)flags;
		}
	} else {
		/* vfs_open_mode, not vfs_open: an O_CREAT open must create the
		 * file with the mode the caller asked for.  Discarding it made
		 * every created file 0644 -- so mkstemp(), which asks for 0600
		 * precisely so its temporary file is private, produced a
		 * world-readable one. */
		ret = vfs_open_mode(path, (int)flags, creat_mode(cur, mode),
				    &file);
	}
	if (ret != ST_OK || file == NULL) {
		return vfs_status_to_errno(ret);
	}

	/* Open FIRST, claim the descriptor after: the slot is claimed and
	 * filled in one locked step so two threads of the same process cannot
	 * be handed the same number (see fd_install_from). */
	int fd = fd_install(cur, file);
	if (fd < 0) {
		fd_release_entry(file);
		return fd;
	}
	/* O_CLOEXEC must be recorded: exec now honours FD_CLOEXEC instead of
	 * closing every descriptor, so a descriptor opened with O_CLOEXEC only
	 * disappears across exec if the flag is stored here. */
	if (flags & O_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	/* O_TRUNC modifies contents → drop set-id bits for a non-root caller. */
	if ((flags & O_TRUNC) && cur->cred.euid != 0)
		strip_setid_file(file);
	return fd;
}

// SYS_OPENAT - open a file relative to dirfd
int64_t sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
		   uint64_t mode)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path string to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	int ret;
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		ret = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
		if (ret != 0)
			return ret;
	}
	/* /dev/fd/N and friends: open == duplicate the caller's descriptor */
	int devfd = devfs_fd_alias_target(full);
	if (devfd >= 0)
		return sys_dup((uint64_t)devfd);

	vfs_file_t *file = NULL;
	if (full[0] == '/' && full[1] == 'd' && full[2] == 'e' &&
	    full[3] == 'v' && (full[4] == '/' || full[4] == '\0')) {
		int pr = devfs_open_perm(cur, full, flags);
		if (pr < 0)
			return pr;
		ret = devfs_open_for_task(full, (int)flags,
					  creat_mode(cur, mode), &file, cur);
		if (ret == ST_OK && file) {
			file->refcount = 1;
			file->flags = (int)flags;
		}
	} else {
		/* Same as sys_open: the creation mode must reach the fs. */
		ret = vfs_open_mode(full, (int)flags, creat_mode(cur, mode),
				    &file);
	}
	if (ret != ST_OK || file == NULL) {
		return vfs_status_to_errno(ret);
	}
	/* Claim the descriptor only once the object exists — see sys_open. */
	int fd = fd_install(cur, file);
	if (fd < 0) {
		fd_release_entry(file);
		return fd;
	}
	/* O_CLOEXEC must be recorded: exec now honours FD_CLOEXEC instead of
	 * closing every descriptor, so a descriptor opened with O_CLOEXEC only
	 * disappears across exec if the flag is stored here. */
	if (flags & O_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	/* O_TRUNC modifies contents → drop set-id bits for a non-root caller. */
	if ((flags & O_TRUNC) && cur->cred.euid != 0)
		strip_setid_file(file);
	return fd;
}

// SYS_CLOSE - close a file descriptor
int64_t sys_close(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (fd >= TASK_MAX_FDS)
		return -EBADF;

	/* Closing a standard descriptor is ordinary and portable: a program
	 * about to hand itself a terminal does close(0) and then opens or dups
	 * the one it wants onto the descriptor that frees up.  Refusing it here
	 * made close() fail and the following dup() land on 3 instead, so the
	 * program kept the stdio it inherited (xterm's shell talked to the
	 * console rather than to its pty).
	 *
	 * The console has no object to release -- it IS the empty slot -- so
	 * the close is recorded in the flag byte instead. */
	if (fd < 3 && task_fds(cur)[fd] == NULL) {
		uint64_t cflags = 0;
		fds_lock(cur, &cflags);
		/* Re-tested under the lock: two threads of one process closing
		 * the same descriptor must not both be told they succeeded. */
		int already =
			task_get_fd_flags(cur, (unsigned)fd) & FD_STDIO_CLOSED;
		if (!already)
			task_set_fd_flags(cur, (unsigned)fd, FD_STDIO_CLOSED);
		fds_unlock(cur, cflags);
		return already ? -EBADF : 0;
	}

	/* Detach the descriptor from the table FIRST, under the shared-table
	 * lock, and release the object afterwards.  Two threads of the same
	 * process closing the same fd must not both reach the release (a
	 * double free), and a slot must never be observable as free while the
	 * object behind it is still being torn down — releasing can sleep
	 * (vfs_close → pagecache flush), which is exactly the window in which
	 * another thread's open() would claim the slot. */
	/* Before detaching: POSIX releases this process's record locks on the
	 * file when it closes ANY descriptor for it, even if others remain
	 * open.  Done here, while the descriptor is still valid, because the
	 * file's identity (dev/ino) is what the locks are keyed on. */
	{
		/* Under the table lock, and holding a reference of our own.
		 *
		 * The descriptor was read straight out of the table and used
		 * without either.  frlock_release_for_file() calls vfs_fstat()
		 * to learn the file's identity, and another thread closing the
		 * same descriptor in that window releases the last reference --
		 * so the fstat read ->ops out of freed memory and the machine
		 * took a general protection fault inside vfs_fstat with the
		 * allocator's poison in hand.
		 *
		 * The reference cannot simply be held across the lock either:
		 * fstat can sleep, and this is a spinlock with interrupts off.
		 * So the lock covers taking the reference, and the reference
		 * covers the work. */
		vfs_file_t *lf = NULL;
		uint64_t glflags = 0;

		fds_lock(cur, &glflags);
		{
			vfs_file_t *e = task_fds(cur)[fd];

			if (e && !IS_SOCKET_FD(e) && !unix_sock_is(e) &&
			    !IS_EPOLL_FD(e) && !pipe_is_end(e) &&
			    (uintptr_t)e > 3) {
				vfs_incref(e);
				lf = e;
			}
		}
		fds_unlock(cur, glflags);

		if (lf) {
			frlock_release_for_file(lf, (uint32_t)cur->tgid);
			vfs_close(lf);
		}
	}

	uint64_t lflags = 0;
	fds_lock(cur, &lflags);
	vfs_file_t *file = task_fds(cur)[fd];
	if (file) {
		task_fds(cur)[fd] = NULL;
		/* The slot is about to become free: drop its FD_CLOEXEC bit
		 * with it.  A stale bit left behind is inherited by whatever
		 * lands there next and makes that descriptor vanish across the
		 * next exec.
		 *
		 * For 0/1/2 the empty slot would otherwise read as the console
		 * again, silently reattaching a descriptor the process just
		 * closed (and one that had been redirected onto a pty at that,
		 * so the output would reappear on the terminal). */
		task_set_fd_flags(cur, (unsigned)fd,
				  fd < 3 ? FD_STDIO_CLOSED : 0);
	}
	fds_unlock(cur, lflags);

	if (!file)
		return -EBADF;

	/* Sockets report their own close status; everything else cannot fail
	 * in a way the caller could act on. */
	if (IS_SOCKET_FD(file))
		return sock_close(SOCKET_FD_IDX(file));
	if (unix_sock_is(file))
		return unix_close((unix_socket_t *)file);
	fd_release_entry(file);
	return 0;
}

int64_t sys_access(uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	const char *path = kpath;
	char full[VFS_MAX_PATH];
	if (path[0] != '/' || cur->root[0]) {
		int retb =
			build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
		if (retb != 0)
			return retb;
		path = full;
	}
	/* Ancestor search first (so an unsearchable prefix → EACCES, not
     * ENOENT, and F_OK requires reachability), then existence, then the mode.
     * access(2) checks the REAL uid/gid; R_OK/W_OK/X_OK (4/2/1) == MAY_*. */
	int tr = perm_traverse_cred(path,
				    1); /* real-id search to match the check */
	if (tr < 0)
		return tr;
	struct kstat st;
	int ret = vfs_stat(path, &st);
	if (ret != ST_OK) {
		if (ret == ST_NOT_FOUND)
			return -ENOENT;
		if (ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		return -EINVAL;
	}
	int want = (int)(mode & 7);
	if (want == 0)
		return 0; /* F_OK: exists and reachable */
	return perm_access(cur, path, &st, want, 1);
}

int64_t sys_faccessat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
		      uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		int ret = build_at_path(cur, (int)dirfd, kpath, full,
					sizeof(full));
		if (ret != 0)
			return ret;
	}
	/* AT_EACCESS checks with the effective/fs IDs; otherwise the real IDs — the
     * prefix search must use the same ids as the final check. */
	int use_real = (flags & AT_EACCESS) ? 0 : 1;
	int tr = perm_traverse_cred(
		full, use_real); /* ancestor search before existence */
	if (tr < 0)
		return tr;
	struct kstat st;
	int st_ret = vfs_stat(full, &st);
	if (st_ret != ST_OK) {
		if (st_ret == ST_NOT_FOUND)
			return -ENOENT;
		if (st_ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		return -EINVAL;
	}
	int want = (int)(mode & 7); /* R_OK/W_OK/X_OK == MAY_READ/WRITE/EXEC */
	if (want == 0)
		return 0; /* F_OK: exists and reachable */
	return perm_access(cur, full, &st, want, (flags & AT_EACCESS) ? 0 : 1);
}

int64_t sys_chdir(uint64_t pathname)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	const char *cwd = (cur->cwd[0] != 0) ? cur->cwd : "/";
	int ret = normalize_path(cwd, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;
	struct kstat st;
	int vret = vfs_stat(full, &st);
	if (vret == ST_NOT_FOUND)
		return -ENOENT;
	if (vret == ST_IO)
		return -EIO; /* corrupt metadata, not "not a dir" */
	if (vret != ST_OK)
		return -ENOTDIR;
	if ((st.st_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;
	/* Entering a directory requires search on it and on every ancestor. */
	int tr = perm_traverse(full);
	if (tr < 0)
		return tr;
	int pr = perm_access(cur, full, &st, MAY_EXEC, 0);
	if (pr < 0)
		return pr;
	// Update FAT32 layer's cwd cluster
	vfs_chdir(full);
	// Update task cwd string with canonical absolute path
	mm_memset(cur->cwd, 0, sizeof(cur->cwd));
	size_t i = 0;
	for (; full[i] && i < sizeof(cur->cwd) - 1; ++i)
		cur->cwd[i] = full[i];
	cur->cwd[i] = '\0';
	return 0;
}

/* SYS_CHROOT — confine the calling task (and its future children) to a
 * subtree.  Privileged operation.  The target is resolved through the normal
 * path machinery, so a chroot INSIDE an existing jail nests correctly, and the
 * stored root is the real, canonical, already-jail-prefixed absolute path.
 * Enforcement happens in build_at_path/apply_chroot for every later textual
 * path; the caller is expected to chdir("/") afterwards, exactly as on other
 * Unix systems. */

int64_t sys_chroot(uint64_t pathname)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;
	if (!capable())
		return -EPERM;

	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	/* Resolve to a canonical absolute path WITH any current jail applied. */
	char full[VFS_MAX_PATH];
	int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;

	/* Target must exist and be a directory. */
	struct kstat st;
	int vret = vfs_stat(full, &st);
	if (vret == ST_NOT_FOUND)
		return -ENOENT;
	if (vret == ST_IO)
		return -EIO;
	if (vret != ST_OK)
		return -ENOTDIR;
	if ((st.st_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;

	/* Store as the new jail root (drop a trailing slash; "/" clears it). */
	size_t n = 0;
	while (full[n])
		n++;
	while (n > 1 && full[n - 1] == '/')
		n--;
	if (n >= sizeof(cur->root))
		return -ENAMETOOLONG;
	mm_memset(cur->root, 0, sizeof(cur->root));
	if (!(n == 1 && full[0] == '/')) {
		for (size_t i = 0; i < n; i++)
			cur->root[i] = full[i];
	}
	return 0;
}

int64_t sys_getcwd(uint64_t buf, uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(buf, size)) {
		return -EFAULT;
	}
	const char *src = (cur->cwd[0] != 0) ? cur->cwd : "/";
	size_t len = 0;
	while (src[len])
		len++;
	/* POSIX: EINVAL when size is 0, ERANGE when the path does not fit.
	 * Callers retry with a bigger buffer on ERANGE and give up on EINVAL,
	 * so reporting EINVAL for both made a long cwd unreadable. */
	if (size == 0)
		return -EINVAL;
	if (len + 1 > size)
		return -ERANGE;
	if (copy_to_user((void *)buf, src, len + 1) < 0) {
		return -EFAULT;
	}
	return (int64_t)buf;
}

int64_t sys_umask(uint64_t mask)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	/* The mask is process-wide: set it through the accessor so every thread
	 * of this process sees the change. */
	return (int64_t)task_set_umask(cur, (uint32_t)mask);
}

int64_t sys_ftruncate(uint64_t fd, uint64_t length)
{
	task_t *cur = sched_current();
	vfs_file_t *f;
	int64_t ret;

	if (!cur)
		return -EFAULT;
	/* Held across the call: the body reads the inode behind it and
	 * can sleep doing so. */
	f = fdget(cur, (int)fd);
	if (!f)
		return -EBADF;
	ret = ftruncate_held(cur, f, length);
	fdput(f);
	return ret;
}

static int64_t ftruncate_held(task_t *cur, vfs_file_t *f, uint64_t length)
{
	/* Only a real file has a length to set; POSIX gives EINVAL for the
	 * rest, and dereferencing a marker here would fault the kernel. */
	if (fd_is_special(f))
		return -EINVAL;
	int r = vfs_truncate(f, (unsigned long)length);
	/* Truncating contents drops set-id bits for a non-privileged caller,
     * same as write() (see strip_setid_file for the once-per-inode fast-path). */
	if (r >= 0 && cur->cred.euid != 0 && !vfs_setid_clean(f))
		strip_setid_file(f);
	return r;
}

/* chmod/chown persist on filesystems with UNIX perms; on one without them the
 * vfs layer succeeds silently (ST_OK) so legacy behavior is preserved. */
int64_t sys_chmod(uint64_t pathname, uint64_t mode)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	unsigned new_mode = (unsigned)mode;
	/* Only the file's owner (or root) may change its mode; and a non-root
     * caller not in the file's group cannot set the set-group-ID bit. */
	task_t *cur = sched_current();
	if (cur && cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			if ((uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
			if ((new_mode & S_ISGID) &&
			    !cred_in_group(&cur->cred, (uint32_t)st.st_gid))
				new_mode &= ~(unsigned)S_ISGID;
		}
	}
	int r = vfs_chmod(kpath, new_mode);
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}

int64_t sys_fchmod(uint64_t fd, uint64_t mode)
{
	task_t *cur = sched_current();
	vfs_file_t *f;
	int64_t ret;

	if (!cur)
		return -EFAULT;
	/* Held across the call: the body reads the inode behind it and
	 * can sleep doing so. */
	f = fdget(cur, (int)fd);
	if (!f)
		return -EBADF;
	ret = fchmod_held(cur, f, mode);
	fdput(f);
	return ret;
}

static int64_t fchmod_held(task_t *cur, vfs_file_t *f, uint64_t mode)
{
	if (fd_is_special(f))
		return 0; /* no perms to change */
	unsigned new_mode = (unsigned)mode;
	/* Only the owner (or root) may chmod, and a non-root caller not in the
     * file's group cannot set the set-group-ID bit.  Permissive if the fs can't
     * report the owner (vfs_fstat unsupported, e.g. the perm-less FAT path). */
	if (cur->cred.euid != 0) {
		struct kstat st;
		if (vfs_fstat(f, &st) == ST_OK) {
			if ((uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
			if ((new_mode & S_ISGID) &&
			    !cred_in_group(&cur->cred, (uint32_t)st.st_gid))
				new_mode &= ~(unsigned)S_ISGID;
		}
	}
	int r = vfs_fchmod(f, new_mode);
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}

int64_t sys_chown(uint64_t pathname, uint64_t owner, uint64_t group)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	/* Changing the owner is root-only; a non-root owner may change the
     * group of their own file to one of their groups. */
	task_t *cur = sched_current();
	int new_uid = (int)owner, new_gid = (int)group;
	if (cur && cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			if (new_uid != -1 &&
			    (uint32_t)new_uid != (uint32_t)st.st_uid)
				return -EPERM; /* owner change: root only */
			if (new_gid != -1 &&
			    (uint32_t)new_gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != cur->cred.fsuid)
					return -EPERM;
				if (!cred_in_group(&cur->cred,
						   (uint32_t)new_gid))
					return -EPERM;
			}
		}
	}
	int r = vfs_chown(kpath, new_uid, new_gid); /* -1 => leave unchanged */
	if (r == ST_OK && cur &&
	    cur->cred.euid != 0) { /* drop set-id on ownership change */
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
			if (clr)
				vfs_chmod(kpath, (unsigned)st.st_mode & ~clr);
		}
	}
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}

int64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group)
{
	task_t *cur = sched_current();
	vfs_file_t *f;
	int64_t ret;

	if (!cur)
		return -EFAULT;
	/* Held across the call: the body reads the inode behind it and
	 * can sleep doing so. */
	f = fdget(cur, (int)fd);
	if (!f)
		return -EBADF;
	ret = fchown_held(cur, f, owner, group);
	fdput(f);
	return ret;
}

static int64_t fchown_held(task_t *cur, vfs_file_t *f, uint64_t owner, uint64_t group)
{
	if (fd_is_special(f))
		return 0; /* no ownership to change */
	int new_uid = (int)owner, new_gid = (int)group;
	/* Owner change is root-only; a non-root owner may regroup to one of
     * their groups (same rule as path chown).  Permissive if owner unknown. */
	if (cur->cred.euid != 0) {
		struct kstat st;
		if (vfs_fstat(f, &st) == ST_OK) {
			if (new_uid != -1 &&
			    (uint32_t)new_uid != (uint32_t)st.st_uid)
				return -EPERM;
			if (new_gid != -1 &&
			    (uint32_t)new_gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != cur->cred.fsuid)
					return -EPERM;
				if (!cred_in_group(&cur->cred,
						   (uint32_t)new_gid))
					return -EPERM;
			}
		}
	}
	int r = vfs_fchown(f, new_uid, new_gid);
	if (r == ST_OK &&
	    cur->cred.euid != 0) { /* drop set-id on ownership change */
		struct kstat st;
		if (vfs_fstat(f, &st) == ST_OK) {
			unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
			if (clr)
				vfs_fchmod(f,
					   (unsigned)st.st_mode & ~clr);
		}
	}
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}
