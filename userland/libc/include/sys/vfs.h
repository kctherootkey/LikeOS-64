/* sys/vfs.h - filesystem statistics */
#ifndef _SYS_VFS_H
#define _SYS_VFS_H

#include <stddef.h>

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef unsigned long fsid_t;

struct statfs {
    unsigned long f_type;     /* filesystem type magic */
    unsigned long f_bsize;    /* optimal transfer block size */
    unsigned long f_blocks;   /* total data blocks */
    unsigned long f_bfree;    /* free blocks */
    unsigned long f_bavail;   /* free blocks available to unprivileged users */
    unsigned long f_files;    /* total file nodes */
    unsigned long f_ffree;    /* free file nodes */
    fsid_t        f_fsid;     /* filesystem ID */
    unsigned long f_namelen;  /* maximum filename length */
    unsigned long f_frsize;   /* fragment size */
    unsigned long f_flags;    /* mount flags */
    unsigned long f_spare[4]; /* padding for future use */
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif /* _SYS_VFS_H */
