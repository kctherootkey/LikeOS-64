/*
 * readlinkat.c - readlinkat(2): read a symbolic link relative to a directory
 * descriptor.
 *
 * The kernel resolves the name against the directory the descriptor refers
 * to (or the working directory for AT_FDCWD), as it does for the other *at
 * calls.  An empty path would name the link the descriptor itself is open
 * on, which needs descriptors that can be open on a link; the kernel
 * answers ENOENT, as POSIX specifies for an empty path.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <unistd.h>
#include <errno.h>
#include "syscall.h"

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
	if (!path || !buf) {
		errno = EFAULT;
		return -1;
	}
	long ret = syscall4(SYS_READLINKAT, dirfd, (long)path, (long)buf,
			    (long)bufsiz);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (ssize_t)ret;
}
