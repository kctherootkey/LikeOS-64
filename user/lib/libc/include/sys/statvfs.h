/* sys/statvfs.h - filesystem statistics (POSIX interface) */
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H

#include <sys/vfs.h>

/* statvfs and statfs share the same layout in this implementation */
struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

/* f_flag bits (POSIX ST_* plus the common BSD/extended mount flags). */
#define ST_RDONLY      0x0001 /* read-only filesystem */
#define ST_NOSUID      0x0002 /* setuid/setgid bits ignored */
#define ST_NODEV       0x0004 /* disallow access to device special files */
#define ST_NOEXEC      0x0008 /* disallow program execution */
#define ST_SYNCHRONOUS 0x0010 /* writes are synced immediately */
#define ST_MANDLOCK    0x0040 /* mandatory locking permitted */
#define ST_WRITE       0x0080 /* write to file/dir/symlink */
#define ST_APPEND      0x0100 /* append-only file */
#define ST_IMMUTABLE   0x0200 /* immutable file */
#define ST_NOATIME     0x0400 /* do not update access times */
#define ST_NODIRATIME  0x0800 /* do not update directory access times */
#define ST_RELATIME    0x1000 /* update atime relative to mtime/ctime */

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif /* _SYS_STATVFS_H */
