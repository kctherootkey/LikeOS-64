// LikeOS-64 -- the stat family, readlink, statfs and utimensat.
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

/* The text a /dev/fd/N symlink resolves to.  A descriptor carries no pathname
 * here, so anything that is not a named file gets the conventional
 * "kind:[id]" form and a regular file is identified by inode.  Returns the
 * length written (never NUL-terminated in the caller's count), or -EBADF. */
static int fd_link_target(task_t *cur, int fd, char *out, size_t cap)
{
	if (fd < 0 || fd >= TASK_MAX_FDS)
		return -EBADF;
	if (cap < 2)
		return -EINVAL;
	/* Held: the arms below read the socket's id, ask devfs for the path it
	 * was opened under, and fstat it -- all dereferences. */
	vfs_file_t *entry = fdget(cur, fd);
	uint64_t marker = (uint64_t)entry;
	int n;
	/* 0/1/2 with an empty slot (and the explicit console markers dup'ed
	 * from them) are the caller's terminal. */
	if (!entry) {
		if (!task_fd_is_console(cur, fd))
			return -EBADF;
		n = ksnprintf(out, cap, "/dev/tty");
	} else if (marker >= 1 && marker <= 3) {
		n = ksnprintf(out, cap, "/dev/tty");
	} else if (IS_SOCKET_FD(entry)) {
		n = ksnprintf(out, cap, "socket:[%d]", SOCKET_FD_IDX(entry));
	} else if (unix_sock_is(entry)) {
		/* The socket's own small id, never its address: this string is
		 * handed to userspace, and the descriptor now holds a kernel
		 * pointer. */
		n = ksnprintf(out, cap, "socket:[%d]",
			      (int)((unix_socket_t *)entry)->id);
	} else if (IS_EPOLL_FD(entry)) {
		n = ksnprintf(out, cap, "anon_inode:[eventpoll]");
	} else if (pipe_is_end(entry)) {
		n = ksnprintf(out, cap, "pipe:[%d]", fd);
	} else {
		/* A device node reports the /dev path it was opened under —
		 * the handle's own type is not enough (/dev/tty, /dev/console
		 * and /dev/tty0 share one type), which is why this used to
		 * answer a bare "/dev" for every one of them. */
		n = devfs_fpath(entry, out, cap);
		if (n < 0) {
			struct kstat st;
			mm_memset(&st, 0, sizeof(st));
			if (vfs_fstat(entry, &st) == ST_OK)
				n = ksnprintf(out, cap, "file:[%lu]",
					      (unsigned long)st.st_ino);
			else
				n = ksnprintf(out, cap, "file:[0]");
		}
	}
	/* ksnprintf reports what the format WOULD have produced; clamp to what
	 * actually fits so the length never overruns the caller's buffer. */
	if (n < 0)
		n = 0;
	if ((size_t)n > cap - 1)
		n = (int)cap - 1;
	if (entry)
		fdput(entry);
	return n;
}

static int64_t sys_stat_common(const char *path, uint64_t stat_buf,
			       int validate_path)
{
	if (!path || !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
		return -EFAULT;
	}
	if (validate_path && !validate_user_ptr((uint64_t)path, 1)) {
		return -EFAULT;
	}
	/* /dev/fd/N (and /dev/stdin|stdout|stderr) describe an open descriptor
	 * of the CALLER, so stat'ing one means fstat'ing that descriptor -
	 * programs handed such a path (process substitution) expect it to
	 * stat like the underlying object, not to be missing. */
	int devfd = devfs_fd_alias_target(path);
	if (devfd >= 0)
		return sys_fstat((uint64_t)devfd, stat_buf);
	// Security: Zero the struct to prevent leaking uninitialized kernel stack data
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	/* vfs_stat runs the ancestor search itself, BEFORE resolving the target,
     * so an unsearchable prefix still yields EACCES (not ENOENT). */
	int ret = vfs_stat(path, &st);
	if (ret != ST_OK) {
		if (ret == ST_NOT_FOUND)
			return -ENOENT;
		if (ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		if (ret == ST_ACCESS || ret == ST_PERM)
			return vfs_status_to_errno(ret);
		return -EINVAL;
	}
	// Security: Use SMAP-aware copy to user
	if (copy_to_user((void *)stat_buf, &st, sizeof(st)) != 0) {
		return -EFAULT;
	}
	return 0;
}

int64_t sys_stat(uint64_t pathname, uint64_t stat_buf)
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

	if (kpath[0] == '/' && cur->root[0] == '\0') {
		return sys_stat_common(kpath, stat_buf, 0);
	}
	char full[VFS_MAX_PATH];
	int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;
	return sys_stat_common(full, stat_buf, 0);
}

int64_t sys_lstat(uint64_t pathname, uint64_t stat_buf)
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

	const char *p = kpath;
	char full[VFS_MAX_PATH];
	if (kpath[0] != '/' || cur->root[0]) {
		int ret =
			build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
		if (ret != 0)
			return ret;
		p = full;
	}
	/* lstat must NOT follow a final symlink.  vfs_lstat dispatches to the
     * filesystem's no-follow stat; on a filesystem without symlinks it
     * transparently falls back to plain stat.  vfs_lstat runs the ancestor
     * search itself (before existence is revealed). */
	if (!validate_user_ptr(stat_buf, sizeof(struct kstat)))
		return -EFAULT;
	/* /dev/fd/N, /dev/stdin, /dev/stdout, /dev/stderr are SYMLINKS, as on
	 * every other Unix, and lstat reports the link itself rather than what
	 * it points at.  Reporting the target's type here instead made `ls -l
	 * /dev/fd` describe the descriptor a caller happened to hold — listing
	 * the directory made ls's own directory handle show up as a
	 * subdirectory of /dev/fd, which is nonsense.  stat() (sys_stat_common)
	 * still follows through to the descriptor. */
	int devfd = devfs_fd_alias_target(p);
	if (devfd >= 0) {
		char target[64];
		int tlen = fd_link_target(cur, devfd, target, sizeof(target));
		if (tlen < 0)
			return tlen;
		struct kstat lst;
		mm_memset(&lst, 0, sizeof(lst));
		lst.st_mode = S_IFLNK | 0777;
		lst.st_nlink = 1;
		lst.st_size = tlen;
		lst.st_blksize = 4096;
		if (copy_to_user((void *)stat_buf, &lst, sizeof(lst)) != 0)
			return -EFAULT;
		return 0;
	}
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	int r = vfs_lstat(p, &st);
	if (r != ST_OK) {
		if (r == ST_ACCESS || r == ST_PERM)
			return vfs_status_to_errno(r);
		return (r == ST_NOT_FOUND) ? -ENOENT : -EINVAL;
	}
	if (copy_to_user((void *)stat_buf, &st, sizeof(st)) != 0)
		return -EFAULT;
	return 0;
}

int64_t sys_fstat(uint64_t fd, uint64_t stat_buf)
{
	/* One exit, so the descriptor's hold is released on every path.  The
	 * arms that answer before any lookup leave `file' NULL and release
	 * nothing. */
	vfs_file_t *file = NULL;
	int64_t ret;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(stat_buf, sizeof(struct kstat))) {
		return -EFAULT;
	}
	// Security: Zero the struct to prevent leaking uninitialized kernel stack data
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	st.st_dev = 0;
	st.st_ino = 0;
	st.st_rdev = 0;
	st.st_nlink = 1;
	st.st_uid = 0;
	st.st_gid = 0;
	st.st_blksize = 4096;
	st.st_blocks = 0;
	st.st_atime = 0;
	st.st_mtime = 0;
	st.st_ctime = 0;
	if (task_fd_is_console(cur, fd)) {
		st.st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_rdev = ((uint64_t)5 << 8) | (fd & 0xff); /* tty major=5 */
		st.st_size = 0;
		// Security: Use SMAP-aware copy to user
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	/* Held for the whole classification below, which dereferences it. */
	file = fdget(cur, (int)fd);
	if (!file) {
		ret = -EBADF;
		goto out;
	}

	/* Classify the tagged fd-table MARKERS before anything dereferences
	 * `file'.  A socket, an AF_UNIX socket, an epoll instance and a dup'ed
	 * console descriptor are all stored as small integers, not pointers, so
	 * handing one to devfs_fstat() reads ->ops out of a bogus address and
	 * faults the KERNEL.  fstat() on an AF_UNIX socket did exactly that:
	 * 0x30009 is UNIX_SOCKET_FD_BASE + 9, and Claws Mail took the whole
	 * system down with it on an ordinary fstat of its own socket.
	 *
	 * fd_link_target() above already classifies in this order; this is the
	 * same set, and pipe_is_end() deliberately rejects every marker so it
	 * cannot be relied on to catch them. */
	uint64_t marker = (uint64_t)file;
	if (marker >= 1 && marker <= 3) {
		/* Console stdio marker planted by dup2. */
		st.st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_rdev = ((uint64_t)5 << 8) | (marker - 1);
		st.st_size = 0;
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	if (IS_SOCKET_FD(file) || unix_sock_is(file)) {
		st.st_mode = S_IFSOCK | (S_IRUSR | S_IWUSR);
		/* A UNIX socket descriptor is a kernel pointer now, so the
		 * inode number comes from the socket's own small id.  The
		 * value goes to userspace; the address must not. */
		st.st_ino = unix_sock_is(file) ?
				    (unsigned long)((unix_socket_t *)file)->id :
				    marker;
		st.st_size = 0;
		st.st_blksize = 4096;
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	if (IS_EPOLL_FD(file)) {
		/* An anonymous inode: no type bits of its own, reported the way
		 * the conventional interface does -- a regular file the caller
		 * can neither read nor write through ordinary calls. */
		st.st_mode = S_IFREG | (S_IRUSR | S_IWUSR);
		st.st_ino = marker;
		st.st_size = 0;
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}

	if (pipe_is_end(file)) {
		st.st_mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_size = 0;
		// Security: Use SMAP-aware copy to user
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	if (devfs_fstat(file, &st) == 0) {
		// Security: Use SMAP-aware copy to user
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	/* Regular file: report the REAL inode metadata (mode/uid/gid/size/
	 * times) from the filesystem.  fstat() used to hardcode 0644 root:root,
	 * so a program that opens a file and enforces its permission bits via
	 * fstat — sshd rejecting a host key that is not 0600, for one — saw the
	 * wrong mode even though stat() on the path reported the truth. */
	if (vfs_fstat(file, &st) == ST_OK) {
		// Security: Use SMAP-aware copy to user
		ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
		goto out;
	}
	/* Filesystem cannot report fd metadata: sane regular-file default. */
	st.st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	st.st_size = vfs_size(file);
	// Security: Use SMAP-aware copy to user
	ret = copy_to_user((void *)stat_buf, &st, sizeof(st));
	goto out;

out:
	if (file)
		fdput(file);
	return ret;
}

int64_t sys_fstatat(uint64_t dirfd, uint64_t pathname, uint64_t stat_buf,
		    uint64_t flags)
{
	(void)flags;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1) ||
	    !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
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
	return sys_stat_common(full, stat_buf, 0);
}

int64_t sys_readlink(uint64_t pathname, uint64_t buf, uint64_t bufsiz)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	int tr = perm_traverse(kpath); /* search on every ancestor dir */
	if (tr < 0)
		return tr;
	if (bufsiz == 0)
		return -EINVAL;
	if (!validate_user_ptr(buf, 1))
		return -EFAULT;
	/* /dev/fd/N and the standard-stream aliases are symlinks (see the
	 * lstat path); `ls -l` reads them to print the "-> target" and errors
	 * out if the read fails. */
	int devfd = devfs_fd_alias_target(kpath);
	if (devfd >= 0) {
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		char target[64];
		int tlen = fd_link_target(cur, devfd, target, sizeof(target));
		if (tlen < 0)
			return tlen;
		if ((unsigned long)tlen > bufsiz)
			tlen = (int)bufsiz;
		if (copy_to_user((void *)buf, target, (size_t)tlen) != 0)
			return -EFAULT;
		return tlen;
	}
	char kbuf[256];
	unsigned long n = bufsiz;
	if (n > sizeof(kbuf))
		n = sizeof(kbuf);
	int r = vfs_readlink(kpath, kbuf,
			     n); /* <0 on error / not-a-symlink   */
	if (r < 0)
		return vfs_status_to_errno(r);
	if (copy_to_user((void *)buf, kbuf, (size_t)r) != 0)
		return -EFAULT;
	return r; /* byte count (no NUL)           */
}

// utimensat: set a path's modification time via the owning filesystem.
int64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
		      uint64_t flags)
{
	(void)flags;

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	/* Reject AT_EMPTY_PATH (0x1000) - not supported */
	if (flags & 0x1000)
		return -EINVAL;

	/* If pathname is NULL/empty, we'd need dirfd to be a real fd — not supported */
	if (!pathname)
		return -EFAULT;

	char kpath[VFS_MAX_PATH];
	size_t plen;
	int err = user_strnlen((const char *)pathname, VFS_MAX_PATH, &plen);
	if (err)
		return err;
	err = copy_from_user(kpath, (const void *)pathname, plen + 1);
	if (err)
		return err;

	/* Canonicalise against the task cwd / dirfd so the VFS gets an absolute
	 * path: a relative path skips the ancestor search-permission traversal
	 * (and trips a warning) for a non-root caller. */
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
		int bret = build_at_path(cur, (int)dirfd, kpath, full,
					 sizeof(full));
		if (bret != 0)
			return bret;
	}

	int64_t mtime_sec = 0;
	/* The UTIME_NOW / UTIME_OMIT sentinels (1073741823 / 1073741822) match the
     * VFS_UTIME_* protocol values, so the userspace nsec passes straight
     * through; default (no times given) means "set to now". */
	long mtime_nsec = VFS_UTIME_NOW;

	if (times) {
		/* times points to struct timespec[2]: [0]=atime, [1]=mtime */
		struct k_timespec ts[2];
		if (!validate_user_ptr(times, sizeof(ts)))
			return -EFAULT;
		err = copy_from_user(ts, (const void *)times, sizeof(ts));
		if (err)
			return err;

		mtime_sec = ts[1].tv_sec;
		mtime_nsec = (long)ts[1].tv_nsec;
	}

	/* vfs_utimensat routes to the owning filesystem (devfs has no timestamps,
     * so it succeeds silently). */
	int r = vfs_utimensat(full, mtime_sec, mtime_nsec);
	if (r == ST_NOT_FOUND || r == ST_INVALID)
		return -ENOENT;
	if (r == ST_NOMEM)
		return -ENOMEM;
	if (r == ST_IO)
		return -EIO;
	if (r != ST_OK)
		return -EIO;
	return 0;
}

// Userspace struct statfs layout (must match user/lib/libc/include/sys/vfs.h)
typedef struct {
	unsigned long f_type;
	unsigned long f_bsize;
	unsigned long f_blocks;
	unsigned long f_bfree;
	unsigned long f_bavail;
	unsigned long f_files;
	unsigned long f_ffree;
	unsigned long f_fsid;
	unsigned long f_namelen;
	unsigned long f_frsize;
	unsigned long f_flags;
	unsigned long f_spare[4];
} user_statfs_t;

// statfs: get filesystem statistics for the given path
int64_t sys_statfs(uint64_t u_path, uint64_t u_buf)
{
	if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
		return -EFAULT;

	char kpath[VFS_MAX_PATH];
	size_t plen;
	int err = user_strnlen((const char *)u_path, VFS_MAX_PATH, &plen);
	if (err)
		return err;
	err = copy_from_user(kpath, (const void *)u_path, plen + 1);
	if (err)
		return err;
	err = canon_task_path(kpath, sizeof(kpath));
	if (err)
		return err;

	/* Route to the filesystem owning the path (devfs reports unsupported). */
	struct vfs_statfs vsf;
	mm_memset(&vsf, 0, sizeof(vsf));
	int r = vfs_statfs(kpath, &vsf);
	if (r == ST_UNSUPPORTED)
		return -ENOSYS;
	if (r != ST_OK)
		return -EIO;

	// Translate the generic struct to userspace layout
	user_statfs_t uinfo;
	mm_memset(&uinfo, 0, sizeof(uinfo));
	uinfo.f_type = vsf.f_type;
	uinfo.f_bsize = vsf.f_bsize;
	uinfo.f_blocks = vsf.f_blocks;
	uinfo.f_bfree = vsf.f_bfree;
	uinfo.f_bavail = vsf.f_bavail;
	uinfo.f_files = vsf.f_files;
	uinfo.f_ffree = vsf.f_ffree;
	uinfo.f_fsid = vsf.f_fsid;
	uinfo.f_namelen = vsf.f_namelen;
	uinfo.f_frsize = vsf.f_frsize;
	uinfo.f_flags = 0;

	return copy_to_user((void *)u_buf, &uinfo, sizeof(uinfo));
}

// fstatfs: get filesystem statistics for an open file descriptor
int64_t sys_fstatfs(uint64_t fd, uint64_t u_buf)
{
	if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
		return -EFAULT;
	if (fd >= MAX_FDS)
		return -EBADF;

	task_t *cur = sched_current();
	if (!cur)
		return -EBADF;

	/* Stats of the filesystem the descriptor's file lives on.  Descriptors not
     * backed by a real file (stdio/pipe/socket) have no filesystem of their
     * own, so report the root filesystem instead of dereferencing them. */
	struct vfs_statfs vsf;
	mm_memset(&vsf, 0, sizeof(vsf));
	vfs_file_t *file = fdget(cur, (int)fd);
	int r;

	if (!file)
		return -EBADF;
	r = fd_is_special(file) ? vfs_statfs("/", &vsf) :
				  vfs_fstatfs(file, &vsf);
	/* Nothing below touches the file, only the statistics copied out of
	 * it, so the hold ends here. */
	fdput(file);
	if (r == ST_UNSUPPORTED)
		return -ENOSYS;
	if (r != ST_OK)
		return -EIO;

	user_statfs_t uinfo;
	mm_memset(&uinfo, 0, sizeof(uinfo));
	uinfo.f_type = vsf.f_type;
	uinfo.f_bsize = vsf.f_bsize;
	uinfo.f_blocks = vsf.f_blocks;
	uinfo.f_bfree = vsf.f_bfree;
	uinfo.f_bavail = vsf.f_bavail;
	uinfo.f_files = vsf.f_files;
	uinfo.f_ffree = vsf.f_ffree;
	uinfo.f_fsid = vsf.f_fsid;
	uinfo.f_namelen = vsf.f_namelen;
	uinfo.f_frsize = vsf.f_frsize;
	uinfo.f_flags = 0;

	return copy_to_user((void *)u_buf, &uinfo, sizeof(uinfo));
}
