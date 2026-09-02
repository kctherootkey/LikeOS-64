#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <stddef.h>
#include <sys/types.h>

#ifndef _STRUCT_IOVEC_DEFINED
#define _STRUCT_IOVEC_DEFINED
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

/* Positional vectored I/O at `offset', leaving the file position alone. */
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UIO_H */
