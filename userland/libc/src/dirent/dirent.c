#include "../../include/dirent.h"
#include "../../include/unistd.h"
#include "../../include/fcntl.h"
#include "../../include/errno.h"
#include "../../include/string.h"
#include "../../include/stdlib.h"

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

DIR* opendir(const char* name) {
    if (!name) {
        errno = EINVAL;
        return NULL;
    }
    int fd = openat(AT_FDCWD, name, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    DIR* dirp = (DIR*)malloc(sizeof(DIR));
    if (!dirp) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    dirp->fd = fd;
    dirp->buf_pos = 0;
    dirp->buf_len = 0;
    return dirp;
}

struct dirent* readdir(DIR* dirp) {
    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }
    while (1) {
        if (dirp->buf_pos >= dirp->buf_len) {
            int n = getdents64(dirp->fd, dirp->buf, sizeof(dirp->buf));
            if (n <= 0) {
                return NULL;
            }
            dirp->buf_len = n;
            dirp->buf_pos = 0;
        }
        struct linux_dirent64* d = (struct linux_dirent64*)(dirp->buf + dirp->buf_pos);
        if (d->d_reclen == 0) {
            return NULL;
        }
        dirp->buf_pos += d->d_reclen;
        dirp->current.d_ino = d->d_ino;
        dirp->current.d_off = (uint64_t)d->d_off;
        dirp->current.d_reclen = d->d_reclen;
        dirp->current.d_type = d->d_type;
        size_t len = strlen(d->d_name);
        if (len >= sizeof(dirp->current.d_name)) {
            len = sizeof(dirp->current.d_name) - 1;
        }
        memcpy(dirp->current.d_name, d->d_name, len);
        dirp->current.d_name[len] = '\0';
        return &dirp->current;
    }
}

int closedir(DIR* dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    int fd = dirp->fd;
    free(dirp);
    return close(fd);
}
