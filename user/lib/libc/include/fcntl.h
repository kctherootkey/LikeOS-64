#ifndef _FCNTL_H
#define _FCNTL_H

// File open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_NDELAY    O_NONBLOCK
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000
/* O_SYNC/O_DSYNC: writes reach storage before returning.  The kernel does not
 * distinguish these yet, but software (ssh, sqlite) passes them; accept the
 * standard bits so open() flag handling is source-compatible. */
#define O_DSYNC     0x1000
#define O_SYNC      0x101000
#define O_RSYNC     O_SYNC
#define O_LARGEFILE 0 /* 64-bit off_t already; no separate large-file mode */

// Special dirfd for *at() syscalls
#define AT_FDCWD    -100

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

// File descriptor flags
#define FD_CLOEXEC      1

// File access
int open(const char* pathname, int flags, ...);
int openat(int dirfd, const char* pathname, int flags, ...);

#endif
