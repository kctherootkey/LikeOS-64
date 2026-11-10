/*
 * readlinkat.c - readlinkat(2): read a symbolic link relative to a directory
 * descriptor.
 *
 * The kernel has readlink but no readlinkat system call, so the directory's
 * path is taken from /proc/self/fd/<dirfd> -- the path the kernel recorded
 * when the directory was opened, which is also what it resolves the other
 * *at calls against -- and the link is read by its full name.  An absolute
 * path, or AT_FDCWD, is plain readlink().
 *
 * An empty path (reading the link the descriptor itself was opened on)
 * needs O_PATH descriptors, which this system does not have: ENOENT, as
 * POSIX specifies for an empty path.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
	char link[32], dir[PATH_MAX], full[PATH_MAX];
	struct stat st;
	int n;

	if (!path || !buf) {
		errno = EFAULT;
		return -1;
	}
	if (path[0] == '/' || dirfd == AT_FDCWD)
		return readlink(path, buf, bufsiz);
	if (path[0] == '\0') {
		errno = ENOENT;
		return -1;
	}

	if (fstat(dirfd, &st) < 0)
		return -1; /* EBADF */
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}

	snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
	n = readlink(link, dir, sizeof(dir) - 1);
	if (n < 0)
		return -1;
	dir[n] = '\0';
	if (dir[0] != '/') {
		/* Not a path the descriptor can be named by. */
		errno = ENOTDIR;
		return -1;
	}

	if ((size_t)snprintf(full, sizeof(full), "%s%s%s", dir,
			     (n > 0 && dir[n - 1] == '/') ? "" : "/",
			     path) >= sizeof(full)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return readlink(full, buf, bufsiz);
}
