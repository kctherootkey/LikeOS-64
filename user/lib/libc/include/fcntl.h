#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>  /* mode_t, off_t */

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
/* unlinkat(): remove a directory instead of a file.  Shares its value with
 * AT_EACCESS, as on the reference system -- the two are used by different
 * syscalls and never appear together. */
#define AT_REMOVEDIR 0x200

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
/* POSIX advisory record locking.  Values are the conventional x86-64 ones so
 * the ABI matches what software expects to find. */
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7

#define F_RDLCK         0       /* shared read lock */
#define F_WRLCK         1       /* exclusive write lock */
#define F_UNLCK         2       /* release */

/* l_len of 0 means "to end of file", which is how a whole-file lock is spelled
 * (l_start 0, l_len 0).  Locks are ADVISORY: they do not stop a process that
 * never asks, they coordinate ones that do. */
struct flock {
        short l_type;           /* F_RDLCK / F_WRLCK / F_UNLCK */
        short l_whence;         /* SEEK_SET / SEEK_CUR / SEEK_END */
        off_t l_start;
        off_t l_len;
        pid_t l_pid;            /* F_GETLK: pid holding the conflicting lock */
};


// File descriptor flags
#define FD_CLOEXEC      1

// File access
int open(const char* pathname, int flags, ...);
int openat(int dirfd, const char* pathname, int flags, ...);
int creat(const char* pathname, mode_t mode);

#endif
