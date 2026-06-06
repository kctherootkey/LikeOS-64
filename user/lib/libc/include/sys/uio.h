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

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UIO_H */
