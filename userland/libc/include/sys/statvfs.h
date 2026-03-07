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

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif /* _SYS_STATVFS_H */
