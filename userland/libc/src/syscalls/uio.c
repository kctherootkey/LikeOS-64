/*
 * uio.c - readv(2) / writev(2) wrappers around the SYS_READV / SYS_WRITEV
 * scatter-gather syscalls.  The kernel implementation processes the iovecs
 * sequentially; partial-transfer semantics on errors mid-stream are
 * documented under POSIX.1-2017 §readv/writev "RETURN VALUE".
 */
#include "../../include/sys/uio.h"
#include "../../include/errno.h"
#include "syscall.h"

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    long ret = syscall3(SYS_READV, fd, (long)iov, (long)iovcnt);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    long ret = syscall3(SYS_WRITEV, fd, (long)iov, (long)iovcnt);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}
