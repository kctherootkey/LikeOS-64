// LikeOS-64 - devfs (device filesystem)
#include "../../include/kernel/devfs.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/dirent.h"

#define DEVFS_TYPE_TTY       1
#define DEVFS_TYPE_PTY_MASTER 2
#define DEVFS_TYPE_PTY_SLAVE  3
#define DEVFS_TYPE_DIR        4
#define DEVFS_TYPE_PTS_DIR    5

typedef struct {
    vfs_file_t vfs;
    int type;
    tty_t* tty;
    int pty_id;
    unsigned dir_pos;
} devfs_file_t;

static vfs_ops_t g_devfs_ops;

static int is_path(const char* path, const char* match) {
    return (kstrcmp(path, match) == 0);
}

static int is_prefix(const char* path, const char* prefix) {
    size_t i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

long devfs_readdir(vfs_file_t* f, void* buf, long bytes);

int devfs_init(void) {
    g_devfs_ops.open = devfs_open;
    g_devfs_ops.stat = devfs_stat;
    g_devfs_ops.read = devfs_read;
    g_devfs_ops.write = devfs_write;
    g_devfs_ops.seek = NULL;
    g_devfs_ops.readdir = devfs_readdir;
    g_devfs_ops.truncate = NULL;
    g_devfs_ops.unlink = NULL;
    g_devfs_ops.rename = NULL;
    g_devfs_ops.mkdir = NULL;
    g_devfs_ops.rmdir = NULL;
    g_devfs_ops.chdir = devfs_chdir;
    g_devfs_ops.close = devfs_close;
    return 0;
}

const vfs_ops_t* devfs_get_ops(void) {
    return &g_devfs_ops;
}

static devfs_file_t* devfs_alloc_file(void) {
    devfs_file_t* df = (devfs_file_t*)kalloc(sizeof(devfs_file_t));
    if (!df) return NULL;
    mm_memset(df, 0, sizeof(devfs_file_t));
    df->vfs.ops = &g_devfs_ops;
    df->vfs.fs_private = df;
    return df;
}

static int devfs_open_tty(tty_t* tty, vfs_file_t** out) {
    if (!tty || !out) return ST_INVALID;
    devfs_file_t* df = devfs_alloc_file();
    if (!df) return ST_NOMEM;
    df->type = DEVFS_TYPE_TTY;
    df->tty = tty;
    *out = &df->vfs;
    return ST_OK;
}

static int devfs_open_dir(int type, vfs_file_t** out) {
    if (!out) return ST_INVALID;
    devfs_file_t* df = devfs_alloc_file();
    if (!df) return ST_NOMEM;
    df->type = type;
    df->tty = NULL;
    df->pty_id = -1;
    *out = &df->vfs;
    return ST_OK;
}

static int devfs_open_pty_master(int* out_id, vfs_file_t** out) {
    if (!out) return ST_INVALID;
    int id = -1;
    if (tty_pty_allocate(&id) != 0) {
        return ST_BUSY;
    }
    devfs_file_t* df = devfs_alloc_file();
    if (!df) return ST_NOMEM;
    df->type = DEVFS_TYPE_PTY_MASTER;
    df->pty_id = id;
    *out = &df->vfs;
    if (out_id) *out_id = id;
    return ST_OK;
}

static int devfs_open_pty_slave(int id, vfs_file_t** out) {
    tty_t* tty = tty_get_pty_slave(id);
    if (!tty) return ST_NOT_FOUND;
    tty_pty_slave_open(id);
    devfs_file_t* df = devfs_alloc_file();
    if (!df) return ST_NOMEM;
    df->type = DEVFS_TYPE_PTY_SLAVE;
    df->tty = tty;
    df->pty_id = id;
    *out = &df->vfs;
    return ST_OK;
}

int devfs_open_for_task(const char* path, int flags, vfs_file_t** out, task_t* cur) {
    (void)flags;
    if (!path || !out) return ST_INVALID;

    if (is_path(path, "/dev") || is_path(path, "/dev/")) {
        return devfs_open_dir(DEVFS_TYPE_DIR, out);
    }
    if (is_path(path, "/dev/pts") || is_path(path, "/dev/pts/")) {
        return devfs_open_dir(DEVFS_TYPE_PTS_DIR, out);
    }

    if (is_path(path, "/dev/tty") && cur) {
        tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
        if (tty && tty->fg_pgid == 0) {
            tty->fg_pgid = cur->pgid;
        }
        return devfs_open_tty(tty, out);
    }
    if (is_path(path, "/dev/console") || is_path(path, "/dev/tty0")) {
        return devfs_open_tty(tty_get_console(), out);
    }
    if (is_path(path, "/dev/ptmx")) {
        return devfs_open_pty_master(NULL, out);
    }
    if (is_prefix(path, "/dev/pts/")) {
        int id = 0;
        const char* p = path + 9;
        if (!*p) return ST_NOT_FOUND;
        while (*p) {
            if (*p < '0' || *p > '9') return ST_NOT_FOUND;
            id = id * 10 + (*p - '0');
            p++;
        }
        if (cur) {
            tty_t* tty = tty_get_pty_slave(id);
            if (tty && tty->fg_pgid == 0) {
                tty->fg_pgid = cur->pgid;
            }
        }
        return devfs_open_pty_slave(id, out);
    }
    return ST_NOT_FOUND;
}

int devfs_open(const char* path, int flags, vfs_file_t** out) {
    // Fallback without task context: use console tty for /dev/tty
    return devfs_open_for_task(path, flags, out, NULL);
}

int devfs_stat(const char* path, struct kstat* st) {
    if (!path || !st) return ST_INVALID;
    mm_memset(st, 0, sizeof(*st));
    if (is_path(path, "/dev") || is_path(path, "/dev/") || is_path(path, "/dev/pts") || is_path(path, "/dev/pts/")) {
        st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        st->st_nlink = 1;
        return ST_OK;
    }
    if (is_path(path, "/dev/tty") || is_path(path, "/dev/console") || is_path(path, "/dev/tty0") || is_path(path, "/dev/ptmx")) {
        st->st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        st->st_nlink = 1;
        return ST_OK;
    }
    if (is_prefix(path, "/dev/pts/")) {
        st->st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        st->st_nlink = 1;
        return ST_OK;
    }
    return ST_NOT_FOUND;
}

int devfs_chdir(const char* path) {
    if (!path) return ST_INVALID;
    if (is_path(path, "/dev") || is_path(path, "/dev/") || is_path(path, "/dev/pts") || is_path(path, "/dev/pts/")) {
        return ST_OK;
    }
    return ST_NOT_FOUND;
}

long devfs_read(vfs_file_t* f, void* buf, long bytes) {
    if (!f || !buf) return -EINVAL;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return -EINVAL;
    if (df->type == DEVFS_TYPE_TTY) {
        return tty_read(df->tty, buf, bytes, 0);
    }
    if (df->type == DEVFS_TYPE_PTY_SLAVE) {
        return tty_read(df->tty, buf, bytes, 0);
    }
    if (df->type == DEVFS_TYPE_PTY_MASTER) {
        return tty_pty_master_read(df->pty_id, buf, bytes, 0);
    }
    return -EINVAL;
}

long devfs_write(vfs_file_t* f, const void* buf, long bytes) {
    if (!f || !buf) return -EINVAL;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return -EINVAL;
    if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
        return tty_write(df->tty, buf, bytes);
    }
    if (df->type == DEVFS_TYPE_PTY_MASTER) {
        return tty_pty_master_write(df->pty_id, buf, bytes);
    }
    return -EINVAL;
}

static unsigned devfs_write_dirent64(char* out, unsigned out_size, unsigned* out_off,
                                     const char* name, uint64_t ino, uint8_t type) {
    if (!out || !out_off || !name) return 0;
    unsigned name_len = 0;
    while (name[name_len] && name_len < 255) name_len++;
    unsigned reclen = (unsigned)sizeof(struct linux_dirent64) + name_len + 1;
    reclen = (reclen + 7u) & ~7u;
    if (*out_off + reclen > out_size) return 0;
    struct linux_dirent64* d = (struct linux_dirent64*)(out + *out_off);
    d->d_ino = ino;
    d->d_off = 0;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = type;
    char* dn = (char*)d->d_name;
    for (unsigned i = 0; i < name_len; ++i) dn[i] = name[i];
    dn[name_len] = '\0';
    *out_off += reclen;
    return 1;
}

long devfs_readdir(vfs_file_t* f, void* buf, long bytes) {
    if (!f || !buf || bytes <= 0) return -EINVAL;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return -EINVAL;
    if (df->type != DEVFS_TYPE_DIR && df->type != DEVFS_TYPE_PTS_DIR) {
        return -ENOTDIR;
    }
    if (df->dir_pos) {
        return 0;
    }

    unsigned out_off = 0;
    if (df->type == DEVFS_TYPE_DIR) {
        devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, "tty", 1, 2);
        devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, "console", 2, 2);
        devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, "tty0", 3, 2);
        devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, "ptmx", 4, 2);
        devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, "pts", 5, 4);
        df->dir_pos = 1;
        return (long)out_off;
    }

    for (int i = 0; i < 16; ++i) {
        if (!tty_pty_is_allocated(i)) continue;
        char name[8];
        int len = 0;
        int n = i;
        if (n == 0) {
            name[len++] = '0';
        } else {
            char tmp[8];
            int t = 0;
            while (n > 0 && t < 7) { tmp[t++] = (char)('0' + (n % 10)); n /= 10; }
            while (t > 0) { name[len++] = tmp[--t]; }
        }
        name[len] = '\0';
        if (!devfs_write_dirent64((char*)buf, (unsigned)bytes, &out_off, name, (uint64_t)(100 + i), 2)) {
            break;
        }
    }
    df->dir_pos = 1;
    return (long)out_off;
}

int devfs_close(vfs_file_t* f) {
    if (!f) return ST_INVALID;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (df) {
        if (df->type == DEVFS_TYPE_PTY_MASTER) {
            tty_pty_master_close(df->pty_id);
        } else if (df->type == DEVFS_TYPE_PTY_SLAVE) {
            tty_pty_slave_close(df->pty_id);
        }
        kfree(df);
    }
    return ST_OK;
}

int devfs_ioctl(vfs_file_t* f, unsigned long req, void* argp, task_t* cur) {
    if (!f || f->ops != &g_devfs_ops) return -ENOTTY;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return -ENOTTY;
    if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
        return tty_ioctl(df->tty, req, argp, cur);
    }
    if (df->type == DEVFS_TYPE_PTY_MASTER) {
        if (req == TIOCGPTN && argp) {
            *(int*)argp = df->pty_id;
            return 0;
        }
    }
    return -ENOTTY;
}

int devfs_fstat(vfs_file_t* f, struct kstat* st) {
    if (!f || f->ops != &g_devfs_ops || !st) return -EINVAL;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return -EINVAL;
    if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_MASTER || df->type == DEVFS_TYPE_PTY_SLAVE) {
        st->st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
        st->st_nlink = 1;
        st->st_size = 0;
        return 0;
    }
    return -EINVAL;
}

tty_t* devfs_get_tty(vfs_file_t* f) {
    if (!f || f->ops != &g_devfs_ops) return NULL;
    devfs_file_t* df = (devfs_file_t*)f->fs_private;
    if (!df) return NULL;
    if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
        return df->tty;
    }
    return NULL;
}

int devfs_is_devfile(vfs_file_t* f) {
    return (f && f->ops == &g_devfs_ops);
}
