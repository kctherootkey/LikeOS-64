/* Descriptor kinds that are not files: eventfd, timerfd, signalfd, memfd;
 * and the positional / preallocating file calls added alongside them. */
#include "syscall.h"
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <sys/futex.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/auxv.h>

static long ret_or_errno(long ret)
{
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return ret;
}

int eventfd(unsigned int initval, int flags)
{
	return (int)ret_or_errno(syscall2(SYS_EVENTFD2, (long)initval, (long)flags));
}

int eventfd_read(int fd, eventfd_t *value)
{
	return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value)
{
	return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

int timerfd_create(int clockid, int flags)
{
	return (int)ret_or_errno(syscall2(SYS_TIMERFD_CREATE, (long)clockid, (long)flags));
}

int timerfd_settime(int fd, int flags, const struct itimerspec *nv,
		    struct itimerspec *ov)
{
	return (int)ret_or_errno(syscall4(SYS_TIMERFD_SETTIME, (long)fd, (long)flags,
					  (long)nv, (long)ov));
}

int timerfd_gettime(int fd, struct itimerspec *cv)
{
	return (int)ret_or_errno(syscall2(SYS_TIMERFD_GETTIME, (long)fd, (long)cv));
}

int signalfd(int fd, const sigset_t *mask, int flags)
{
	return (int)ret_or_errno(syscall4(SYS_SIGNALFD4, (long)fd, (long)mask,
					  (long)sizeof(sigset_t), (long)flags));
}

int memfd_create(const char *name, unsigned int flags)
{
	return (int)ret_or_errno(syscall2(SYS_MEMFD_CREATE, (long)name, (long)flags));
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
	return ret_or_errno(syscall4(SYS_PREAD64, (long)fd, (long)buf, (long)count,
				     (long)offset));
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	return ret_or_errno(syscall4(SYS_PWRITE64, (long)fd, (long)buf, (long)count,
				     (long)offset));
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		ssize_t n = pread(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
		if (n < 0)
			return total ? total : -1;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;
	}
	return total;
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		ssize_t n = pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
		if (n < 0)
			return total ? total : -1;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;
	}
	return total;
}

int fdatasync(int fd)
{
	return (int)ret_or_errno(syscall1(SYS_FDATASYNC, (long)fd));
}

int fallocate(int fd, int mode, off_t offset, off_t len)
{
	return (int)ret_or_errno(syscall4(SYS_FALLOCATE, (long)fd, (long)mode,
					  (long)offset, (long)len));
}

int posix_fallocate(int fd, off_t offset, off_t len)
{
	long ret = syscall4(SYS_FALLOCATE, (long)fd, 0, (long)offset, (long)len);
	return ret < 0 ? (int)-ret : 0; /* POSIX: the error is returned */
}

/* secure_getenv: getenv() that answers NULL in a set-id process, so a
 * privileged program cannot be steered by its caller's environment. */
char *secure_getenv(const char *name)
{
	if (getauxval(AT_SECURE))
		return NULL;
	return getenv(name);
}

int get_nprocs(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int)n : 1;
}

int get_nprocs_conf(void)
{
	long n = sysconf(_SC_NPROCESSORS_CONF);
	return n > 0 ? (int)n : 1;
}

int prctl(int option, ...)
{
	va_list ap;
	unsigned long a1;

	va_start(ap, option);
	a1 = va_arg(ap, unsigned long);
	va_end(ap);
	switch (option) {
	case PR_SET_NAME:
		return pthread_setname_np(pthread_self(), (const char *)a1) ? -1 : 0;
	case PR_GET_NAME:
		return pthread_getname_np(pthread_self(), (char *)a1, 16) ? -1 : 0;
	case PR_GET_DUMPABLE:
		return 1;
	case PR_SET_DUMPABLE:
	case PR_SET_PDEATHSIG:
	case PR_SET_NO_NEW_PRIVS:
	case PR_SET_CHILD_SUBREAPER:
		return 0; /* accepted; nothing behind it here */
	case PR_GET_PDEATHSIG:
	case PR_GET_NO_NEW_PRIVS:
	case PR_GET_CHILD_SUBREAPER:
		if (a1)
			*(int *)a1 = 0;
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

long futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout,
	   uint32_t *uaddr2, uint32_t val3)
{
	long ret = syscall6(SYS_FUTEX, (long)uaddr, (long)op, (long)val,
			    (long)timeout, (long)uaddr2, (long)val3);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return ret;
}
