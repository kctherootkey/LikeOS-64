/*
 * uio.c - readv(2) / writev(2) wrappers around the SYS_READV / SYS_WRITEV
 * scatter-gather syscalls.  The kernel implementation processes the iovecs
 * sequentially; partial-transfer semantics on errors mid-stream are
 * documented under POSIX.1-2017 §readv/writev "RETURN VALUE".
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <sys/uio.h>
#include <errno.h>
#include "syscall.h"

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	long ret = syscall3(SYS_READV, fd, (long)iov, (long)iovcnt);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (ssize_t)ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	long ret = syscall3(SYS_WRITEV, fd, (long)iov, (long)iovcnt);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (ssize_t)ret;
}

/*
 * preadv2() / pwritev2().
 *
 * The flags reach the kernel in SYS_PREADV2 / SYS_PWRITEV2.  A kernel without
 * those calls answers ENOSYS; then a call with no flags is exactly
 * preadv()/pwritev() (readv()/writev() for offset -1) and is done that way,
 * while a call that asks for a flag fails with EOPNOTSUPP -- the flag's
 * promise (polling, atomicity, no blocking) cannot be kept by the plain
 * calls, so silently dropping it would be wrong.
 */
static ssize_t rw_v2_fallback(int write, int fd, const struct iovec *iov,
			      int iovcnt, off_t offset, int flags)
{
	if (flags != 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	if (offset == -1)
		return write ? writev(fd, iov, iovcnt) : readv(fd, iov, iovcnt);
	return write ? pwritev(fd, iov, iovcnt, offset)
		     : preadv(fd, iov, iovcnt, offset);
}

ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
{
	long ret = syscall5(SYS_PREADV2, fd, (long)iov, (long)iovcnt,
			    (long)offset, (long)flags);
	if (ret == -ENOSYS)
		return rw_v2_fallback(0, fd, iov, iovcnt, offset, flags);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (ssize_t)ret;
}

ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)
{
	long ret = syscall5(SYS_PWRITEV2, fd, (long)iov, (long)iovcnt,
			    (long)offset, (long)flags);
	if (ret == -ENOSYS)
		return rw_v2_fallback(1, fd, iov, iovcnt, offset, flags);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (ssize_t)ret;
}
