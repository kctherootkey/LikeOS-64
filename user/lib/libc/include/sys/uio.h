/*
 * Copyright (C) 2026 The LikeOS Project
 */

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

/* Per-call I/O flags of preadv2()/pwritev2() (the established values).
 *   RWF_HIPRI     high priority: the block layer may complete the request
 *                 by polling the device's completion queue (poll queues)
 *   RWF_DSYNC     this write is O_DSYNC: data on stable storage on return
 *   RWF_SYNC      this write is O_SYNC: data and metadata on stable storage
 *   RWF_NOWAIT    fail with EAGAIN instead of blocking (cache misses, locks)
 *   RWF_APPEND    this write appends, as if the file were open O_APPEND
 *   RWF_NOAPPEND  this write is positional even on an O_APPEND file
 *   RWF_ATOMIC    the write lands all or nothing (power-fail atomic); the
 *                 length and alignment must fit the device's atomic limits
 *   RWF_DONTCACHE drop the data from the page cache once the I/O is done */
#define RWF_HIPRI     0x00000001
#define RWF_DSYNC     0x00000002
#define RWF_SYNC      0x00000004
#define RWF_NOWAIT    0x00000008
#define RWF_APPEND    0x00000010
#define RWF_NOAPPEND  0x00000020
#define RWF_ATOMIC    0x00000040
#define RWF_DONTCACHE 0x00000080

/* preadv()/pwritev() with flags.  offset -1 reads/writes at the file
 * position and advances it, as readv()/writev() do. */
ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UIO_H */
