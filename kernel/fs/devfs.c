// LikeOS-64 - devfs (device filesystem)
#include <kernel/fs/devfs.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/ke/timer.h>
#include <kernel/dev/rand/random.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/uapi/bug.h>

#define DEVFS_TYPE_TTY 1
#define DEVFS_TYPE_PTY_MASTER 2
#define DEVFS_TYPE_PTY_SLAVE 3
#define DEVFS_TYPE_DIR 4
#define DEVFS_TYPE_PTS_DIR 5
#define DEVFS_TYPE_RANDOM 6
#define DEVFS_TYPE_URANDOM 7
#define DEVFS_TYPE_NULL 8
#define DEVFS_TYPE_ZERO 9
#define DEVFS_TYPE_FB0 10
#define DEVFS_TYPE_INPUT_DIR 11
#define DEVFS_TYPE_EVDEV 12 /* /dev/input/eventN; unit in evdev_id */
#define DEVFS_TYPE_MAX DEVFS_TYPE_EVDEV

/* Device-node group owners; values must match /etc/group on the root fs. */
#define DEVFS_GID_TTY 5
#define DEVFS_GID_VIDEO 44
#define DEVFS_GID_INPUT 104

typedef struct {
	vfs_file_t vfs;
	int type;
	tty_t *tty;
	int pty_id;
	unsigned dir_pos;
	uint64_t fpos; // byte position (framebuffer device)
	int evdev_id; // input device unit (DEVFS_TYPE_EVDEV)
} devfs_file_t;

static vfs_ops_t g_devfs_ops;

static int is_path(const char *path, const char *match)
{
	return (kstrcmp(path, match) == 0);
}

static int is_prefix(const char *path, const char *prefix)
{
	size_t i = 0;
	while (prefix[i]) {
		if (path[i] != prefix[i])
			return 0;
		i++;
	}
	return 1;
}

long devfs_readdir(vfs_file_t *f, void *buf, long bytes);

int devfs_init(void)
{
	g_devfs_ops.open = devfs_open;
	g_devfs_ops.stat = devfs_stat;
	g_devfs_ops.read = devfs_read;
	g_devfs_ops.write = devfs_write;
	g_devfs_ops.seek = devfs_seek;
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

const vfs_ops_t *devfs_get_ops(void)
{
	return &g_devfs_ops;
}

static devfs_file_t *devfs_alloc_file(void)
{
	might_sleep();
	devfs_file_t *df = (devfs_file_t *)kalloc(sizeof(devfs_file_t));
	if (!df)
		return NULL;
	mm_memset(df, 0, sizeof(devfs_file_t));
	df->vfs.ops = &g_devfs_ops;
	df->vfs.fs_private = df;
	WARN_ON(df->vfs.fs_private != df);
	return df;
}

static int devfs_open_tty(tty_t *tty, vfs_file_t **out)
{
	BUG_ON(tty == NULL);
	BUG_ON(out == NULL);
	if (!tty || !out)
		return ST_INVALID;
	devfs_file_t *df = devfs_alloc_file();
	if (!df)
		return ST_NOMEM;
	df->type = DEVFS_TYPE_TTY;
	df->tty = tty;
	*out = &df->vfs;
	return ST_OK;
}

static int devfs_open_dir(int type, vfs_file_t **out)
{
	BUG_ON(out == NULL);
	if (!out)
		return ST_INVALID;
	devfs_file_t *df = devfs_alloc_file();
	if (!df)
		return ST_NOMEM;
	df->type = type;
	df->tty = NULL;
	df->pty_id = -1;
	*out = &df->vfs;
	return ST_OK;
}

static int devfs_open_pty_master(int *out_id, vfs_file_t **out)
{
	BUG_ON(out == NULL);
	if (!out)
		return ST_INVALID;
	int id = -1;
	if (tty_pty_allocate(&id) != 0) {
		return ST_BUSY;
	}
	WARN_ON(id <
		0); /* tty_pty_allocate succeeded but returned invalid id < 0 */
	devfs_file_t *df = devfs_alloc_file();
	if (!df)
		return ST_NOMEM;
	df->type = DEVFS_TYPE_PTY_MASTER;
	df->pty_id = id;
	*out = &df->vfs;
	if (out_id)
		*out_id = id;
	return ST_OK;
}

static int devfs_open_pty_slave(int id, vfs_file_t **out)
{
	tty_t *tty = tty_get_pty_slave(id);
	if (!tty)
		return ST_NOT_FOUND;
	tty_pty_slave_open(id);
	devfs_file_t *df = devfs_alloc_file();
	if (!df)
		return ST_NOMEM;
	df->type = DEVFS_TYPE_PTY_SLAVE;
	df->tty = tty;
	df->pty_id = id;
	*out = &df->vfs;
	tty_pty_slave_set_vf(id, &df->vfs); // diagnostic refcount visibility
	return ST_OK;
}

int devfs_open_for_task(const char *path, int flags, vfs_file_t **out,
			task_t *cur)
{
	(void)flags;
	if (!path || !out)
		return ST_INVALID;

	if (is_path(path, "/dev") || is_path(path, "/dev/")) {
		return devfs_open_dir(DEVFS_TYPE_DIR, out);
	}
	if (is_path(path, "/dev/pts") || is_path(path, "/dev/pts/")) {
		return devfs_open_dir(DEVFS_TYPE_PTS_DIR, out);
	}

	if (is_path(path, "/dev/tty") && cur) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
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
	if (is_path(path, "/dev/random")) {
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_RANDOM;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_path(path, "/dev/urandom")) {
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_URANDOM;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_path(path, "/dev/null")) {
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_NULL;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_path(path, "/dev/zero")) {
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_ZERO;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_path(path, "/dev/fb0")) {
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_FB0;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_path(path, "/dev/input") || is_path(path, "/dev/input/")) {
		return devfs_open_dir(DEVFS_TYPE_INPUT_DIR, out);
	}
	if (is_prefix(path, "/dev/input/event")) {
		const char *p = path + 16; // after "/dev/input/event"
		int id = 0;
		if (!*p)
			return ST_NOT_FOUND;
		while (*p) {
			if (*p < '0' || *p > '9')
				return ST_NOT_FOUND;
			id = id * 10 + (*p - '0');
			p++;
		}
		if (id >= EVDEV_NUM_UNITS)
			return ST_NOT_FOUND;
		devfs_file_t *df = devfs_alloc_file();
		if (!df)
			return ST_NOMEM;
		df->type = DEVFS_TYPE_EVDEV;
		df->evdev_id = id;
		*out = &df->vfs;
		return ST_OK;
	}
	if (is_prefix(path, "/dev/pts/")) {
		int id = 0;
		const char *p = path + 9;
		if (!*p)
			return ST_NOT_FOUND;
		while (*p) {
			if (*p < '0' || *p > '9')
				return ST_NOT_FOUND;
			id = id * 10 + (*p - '0');
			p++;
		}
		WARN_ON_ONCE(
			id >=
			16); /* PTY id >= TTY_MAX_PTYS: parsed out-of-range slave index */
		if (cur) {
			tty_t *tty = tty_get_pty_slave(id);
			if (tty && tty->fg_pgid == 0) {
				tty->fg_pgid = cur->pgid;
			}
		}
		return devfs_open_pty_slave(id, out);
	}
	return ST_NOT_FOUND;
}

int devfs_open(const char *path, int flags, vfs_file_t **out)
{
	// Fallback without task context: use console tty for /dev/tty
	return devfs_open_for_task(path, flags, out, NULL);
}

/* Decimal suffix of a device path (e.g. after "/dev/input/event"), or -1. */
static int devfs_parse_unit(const char *p)
{
	int id = 0;
	if (!*p)
		return -1;
	while (*p) {
		if (*p < '0' || *p > '9')
			return -1;
		id = id * 10 + (*p - '0');
		p++;
	}
	return id;
}

/* The owner (root), group and mode bits reported here are what the VFS
 * permission layer enforces device access against — i.e. device-node DAC is
 * driven entirely by this metadata.  Console and input/video nodes are
 * group-restricted (tty/input/video, Linux-conventional modes and
 * major:minor numbers); the pseudo devices stay world-accessible and the
 * /dev directories world-searchable (0755). */
int devfs_stat(const char *path, struct kstat *st)
{
	if (!path || !st)
		return ST_INVALID;
	mm_memset(st, 0, sizeof(*st));
	uint64_t now = timer_get_epoch(); /* real wall-clock seconds */
	if (is_path(path, "/dev") || is_path(path, "/dev/") ||
	    is_path(path, "/dev/pts") || is_path(path, "/dev/pts/") ||
	    is_path(path, "/dev/input") || is_path(path, "/dev/input/")) {
		st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
					 S_IXGRP | S_IROTH | S_IXOTH);
		st->st_nlink = 1;
		st->st_atime = now;
		st->st_mtime = now;
		st->st_ctime = now;
		return ST_OK;
	}
	uint32_t perm, gid = 0, rmaj, rmin;
	if (is_path(path, "/dev/tty")) {
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 5, rmin = 0;
	} else if (is_path(path, "/dev/console")) {
		perm = 0620, gid = DEVFS_GID_TTY, rmaj = 5, rmin = 1;
	} else if (is_path(path, "/dev/tty0")) {
		perm = 0620, gid = DEVFS_GID_TTY, rmaj = 4, rmin = 0;
	} else if (is_path(path, "/dev/ptmx")) {
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 5, rmin = 2;
	} else if (is_path(path, "/dev/random")) {
		perm = 0666, rmaj = 1, rmin = 8;
	} else if (is_path(path, "/dev/urandom")) {
		perm = 0666, rmaj = 1, rmin = 9;
	} else if (is_path(path, "/dev/null")) {
		perm = 0666, rmaj = 1, rmin = 3;
	} else if (is_path(path, "/dev/zero")) {
		perm = 0666, rmaj = 1, rmin = 5;
	} else if (is_path(path, "/dev/fb0")) {
		perm = 0660, gid = DEVFS_GID_VIDEO, rmaj = 29, rmin = 0;
	} else if (is_prefix(path, "/dev/input/event")) {
		int id = devfs_parse_unit(path + 16);
		if (id < 0)
			return ST_NOT_FOUND;
		perm = 0660, gid = DEVFS_GID_INPUT, rmaj = 13;
		rmin = 64 + (uint32_t)id;
	} else if (is_prefix(path, "/dev/pts/")) {
		int id = devfs_parse_unit(path + 9);
		if (id < 0)
			return ST_NOT_FOUND;
		/* Slaves stay world-rw: nodes are root-owned (no per-open
		 * chown), so 0620 would lock non-root sessions out of their
		 * own terminal. */
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 136;
		rmin = (uint32_t)id;
	} else {
		return ST_NOT_FOUND;
	}
	st->st_mode = S_IFCHR | perm;
	st->st_gid = gid;
	st->st_rdev = ((uint64_t)rmaj << 8) | rmin;
	st->st_nlink = 1;
	st->st_atime = now;
	st->st_mtime = now;
	st->st_ctime = now;
	return ST_OK;
}

int devfs_chdir(const char *path)
{
	if (!path)
		return ST_INVALID;
	if (is_path(path, "/dev") || is_path(path, "/dev/") ||
	    is_path(path, "/dev/pts") || is_path(path, "/dev/pts/") ||
	    is_path(path, "/dev/input") || is_path(path, "/dev/input/")) {
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

long devfs_read(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !buf)
		return -EINVAL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -EINVAL;
	WARN_ON(df->type < DEVFS_TYPE_TTY || df->type > DEVFS_TYPE_MAX);
	int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;
	if (df->type == DEVFS_TYPE_TTY) {
		WARN_ON(df->tty == NULL);
		return tty_read(df->tty, buf, bytes, nonblock);
	}
	if (df->type == DEVFS_TYPE_PTY_SLAVE) {
		WARN_ON(df->tty == NULL);
		WARN_ON(df->pty_id <
			0); /* PTY slave file with negative pty_id: state corruption */
		return tty_read(df->tty, buf, bytes, nonblock);
	}
	if (df->type == DEVFS_TYPE_PTY_MASTER) {
		WARN_ON(df->pty_id < 0);
		return tty_pty_master_read(df->pty_id, buf, bytes, nonblock);
	}
	if (df->type == DEVFS_TYPE_RANDOM) {
		smap_disable();
		int ret = random_get_bytes(buf, (size_t)bytes, 1);
		smap_enable();
		return ret < 0 ? -EIO : (long)ret;
	}
	if (df->type == DEVFS_TYPE_URANDOM) {
		smap_disable();
		int ret = random_get_bytes(buf, (size_t)bytes, 0);
		smap_enable();
		return ret < 0 ? -EIO : (long)ret;
	}
	if (df->type == DEVFS_TYPE_NULL) {
		return 0; /* always EOF */
	}
	if (df->type == DEVFS_TYPE_ZERO) {
		smap_disable();
		char *p = (char *)buf;
		for (long i = 0; i < bytes; i++)
			p[i] = 0;
		smap_enable();
		return bytes;
	}
	if (df->type == DEVFS_TYPE_FB0) {
		long r = fbdev_read(df->fpos, buf, bytes);
		if (r > 0)
			df->fpos += (uint64_t)r;
		return r;
	}
	if (df->type == DEVFS_TYPE_EVDEV) {
		return evdev_read(df->evdev_id, buf, bytes, nonblock);
	}
	return -EINVAL;
}

long devfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	if (!f || !buf)
		return -EINVAL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -EINVAL;
	WARN_ON(df->type < DEVFS_TYPE_TTY || df->type > DEVFS_TYPE_MAX);
	if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
		WARN_ON(df->tty ==
			NULL); /* TTY/PTY-slave write with NULL tty pointer: state corruption */
		return tty_write(df->tty, buf, bytes);
	}
	if (df->type == DEVFS_TYPE_PTY_MASTER) {
		WARN_ON(df->pty_id < 0);
		return tty_pty_master_write(df->pty_id, buf, bytes);
	}
	if (df->type == DEVFS_TYPE_RANDOM || df->type == DEVFS_TYPE_URANDOM) {
		// Writing to /dev/random mixes data into the entropy pool and can
		// trigger a reseed, so it is restricted to the privileged caller.
		if (!capable())
			return -EPERM;
		smap_disable();
		random_add_entropy(buf, (size_t)bytes);
		smap_enable();
		return bytes;
	}
	if (df->type == DEVFS_TYPE_NULL || df->type == DEVFS_TYPE_ZERO) {
		/* /dev/null and /dev/zero discard all writes. */
		return bytes;
	}
	if (df->type == DEVFS_TYPE_FB0) {
		long r = fbdev_write(df->fpos, buf, bytes);
		if (r > 0)
			df->fpos += (uint64_t)r;
		return r;
	}
	return -EINVAL;
}

// Seek support for the framebuffer device (needed for pread/pwrite-style
// access; all other device nodes are stream-like and reject seeking).
long devfs_seek(vfs_file_t *f, long offset, int whence)
{
	devfs_file_t *df;
	uint64_t size = 0;
	long newpos;

	if (!f)
		return -EINVAL;
	df = (devfs_file_t *)f->fs_private;
	if (!df || df->type != DEVFS_TYPE_FB0)
		return -EINVAL;
	fbdev_get_phys(&size);
	switch (whence) {
	case 0: // SEEK_SET
		newpos = offset;
		break;
	case 1: // SEEK_CUR
		newpos = (long)df->fpos + offset;
		break;
	case 2: // SEEK_END
		newpos = (long)size + offset;
		break;
	default:
		return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	df->fpos = (uint64_t)newpos;
	return newpos;
}

static unsigned devfs_write_dirent64(char *out, unsigned out_size,
				     unsigned *out_off, const char *name,
				     uint64_t ino, uint8_t type)
{
	if (!out || !out_off || !name)
		return 0;
	unsigned name_len = 0;
	while (name[name_len] && name_len < 255)
		name_len++;
	unsigned reclen =
		(unsigned)sizeof(struct linux_dirent64) + name_len + 1;
	reclen = (reclen + 7u) & ~7u;
	WARN_ON(reclen % 8 != 0);
	if (*out_off + reclen > out_size)
		return 0;
	// SMAP-aware write to user buffer
	smap_disable();
	struct linux_dirent64 *d = (struct linux_dirent64 *)(out + *out_off);
	d->d_ino = ino;
	d->d_off = 0;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = type;
	char *dn = (char *)d->d_name;
	for (unsigned i = 0; i < name_len; ++i)
		dn[i] = name[i];
	dn[name_len] = '\0';
	smap_enable();
	*out_off += reclen;
	return 1;
}

long devfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !buf || bytes <= 0)
		return -EINVAL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -EINVAL;
	if (df->type != DEVFS_TYPE_DIR && df->type != DEVFS_TYPE_PTS_DIR &&
	    df->type != DEVFS_TYPE_INPUT_DIR) {
		return -ENOTDIR;
	}
	if (df->dir_pos) {
		return 0;
	}

	unsigned out_off = 0;
	if (df->type == DEVFS_TYPE_INPUT_DIR) {
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "event0", 200, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "event1", 201, 2);
		df->dir_pos = 1;
		return (long)out_off;
	}
	if (df->type == DEVFS_TYPE_DIR) {
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "tty", 1, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "console", 2, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "tty0", 3, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "ptmx", 4, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "pts", 5, 4);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "random", 6, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "urandom", 7, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "null", 8, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "zero", 9, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "fb0", 10, 2);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "input", 11, 4);
		df->dir_pos = 1;
		return (long)out_off;
	}

	for (int i = 0; i < 16; ++i) {
		if (!tty_pty_is_allocated(i))
			continue;
		char name[8];
		int len = 0;
		int n = i;
		if (n == 0) {
			name[len++] = '0';
		} else {
			char tmp[8];
			int t = 0;
			while (n > 0 && t < 7) {
				tmp[t++] = (char)('0' + (n % 10));
				n /= 10;
			}
			while (t > 0) {
				name[len++] = tmp[--t];
			}
		}
		name[len] = '\0';
		if (!devfs_write_dirent64((char *)buf, (unsigned)bytes,
					  &out_off, name, (uint64_t)(100 + i),
					  2)) {
			break;
		}
	}
	df->dir_pos = 1;
	return (long)out_off;
}

int devfs_close(vfs_file_t *f)
{
	if (!f)
		return ST_INVALID;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (df) {
		if (df->type == DEVFS_TYPE_PTY_MASTER) {
			WARN_ON(df->pty_id < 0);
			tty_pty_master_close(df->pty_id);
		} else if (df->type == DEVFS_TYPE_PTY_SLAVE) {
			WARN_ON(df->pty_id <
				0); /* PTY slave close with negative pty_id: state corruption */
			tty_pty_slave_close(df->pty_id);
		} else if (df->type == DEVFS_TYPE_EVDEV) {
			/* Auto-release an exclusive grab held through this
			 * handle (covers process exit without EVIOCGRAB(0)). */
			task_t *cur = sched_current();
			if (cur)
				evdev_release_grab_for(df->evdev_id, cur->id);
		}
		kfree(df);
	}
	return ST_OK;
}

int devfs_ioctl(vfs_file_t *f, unsigned long req, void *argp, task_t *cur)
{
	if (!f || f->ops != &g_devfs_ops)
		return -ENOTTY;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -ENOTTY;
	if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
		return tty_ioctl(df->tty, req, argp, cur);
	}
	if (df->type == DEVFS_TYPE_PTY_MASTER) {
		WARN_ON(df->pty_id <
			0); /* PTY master ioctl with negative pty_id: state corruption */
		if (req == TIOCGPTN && argp) {
			smap_disable();
			*(int *)argp = df->pty_id;
			smap_enable();
			return 0;
		}
		/* Forward all other ioctls to the slave tty (TIOCSWINSZ, TIOCGWINSZ,
         * TCGETS, TCSETS, TIOCGPGRP, TIOCSPGRP …).  tmux calls
         * ioctl(ptm_fd, TIOCSWINSZ, &ws) to resize each pane after a split;
         * without this the ioctl returns ENOTTY and the server exits. */
		tty_t *slave = tty_get_pty_slave(df->pty_id);
		if (slave)
			return tty_ioctl(slave, req, argp, cur);
	}
	if (df->type == DEVFS_TYPE_FB0) {
		return fbdev_ioctl(req, argp, cur);
	}
	if (df->type == DEVFS_TYPE_EVDEV) {
		return evdev_ioctl(df->evdev_id, req, argp, cur);
	}
	return -ENOTTY;
}

/* Event-device unit of an evdev handle, or -1 (poll dispatch helper). */
int devfs_evdev_unit(vfs_file_t *f)
{
	if (!f || f->ops != &g_devfs_ops)
		return -1;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df || df->type != DEVFS_TYPE_EVDEV)
		return -1;
	return df->evdev_id;
}

int devfs_fstat(vfs_file_t *f, struct kstat *st)
{
	if (!f || f->ops != &g_devfs_ops || !st)
		return -EINVAL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -EINVAL;
	uint32_t perm, gid = 0, rmaj, rmin;
	switch (df->type) {
	case DEVFS_TYPE_TTY: /* console opens: report the /dev/tty identity */
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 5, rmin = 0;
		break;
	case DEVFS_TYPE_PTY_MASTER:
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 5, rmin = 2;
		break;
	case DEVFS_TYPE_PTY_SLAVE:
		perm = 0666, gid = DEVFS_GID_TTY, rmaj = 136;
		rmin = (uint32_t)df->pty_id;
		break;
	case DEVFS_TYPE_RANDOM:
		perm = 0666, rmaj = 1, rmin = 8;
		break;
	case DEVFS_TYPE_URANDOM:
		perm = 0666, rmaj = 1, rmin = 9;
		break;
	case DEVFS_TYPE_NULL:
		perm = 0666, rmaj = 1, rmin = 3;
		break;
	case DEVFS_TYPE_ZERO:
		perm = 0666, rmaj = 1, rmin = 5;
		break;
	case DEVFS_TYPE_FB0:
		perm = 0660, gid = DEVFS_GID_VIDEO, rmaj = 29, rmin = 0;
		break;
	case DEVFS_TYPE_EVDEV:
		perm = 0660, gid = DEVFS_GID_INPUT, rmaj = 13;
		rmin = 64 + (uint32_t)df->evdev_id;
		break;
	default:
		return -EINVAL;
	}
	st->st_mode = S_IFCHR | perm;
	st->st_gid = gid;
	st->st_rdev = ((uint64_t)rmaj << 8) | rmin;
	st->st_nlink = 1;
	st->st_size = 0;
	return 0;
}

/* Framebuffer-device test for the mmap path: /dev/fb0 handles map the
 * framebuffer BAR rather than file contents. */
int devfs_is_fb0(vfs_file_t *f)
{
	if (!f || f->ops != &g_devfs_ops)
		return 0;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	return df && df->type == DEVFS_TYPE_FB0;
}

tty_t *devfs_get_tty(vfs_file_t *f)
{
	if (!f || f->ops != &g_devfs_ops)
		return NULL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return NULL;
	if (df->type == DEVFS_TYPE_TTY || df->type == DEVFS_TYPE_PTY_SLAVE) {
		return df->tty;
	}
	return NULL;
}

/* Returns the pty master id for a /dev/ptmx-opened vfs_file_t,
 * or -1 if the file is not a pty master. */
int devfs_get_pty_master_id(vfs_file_t *f)
{
	if (!f || f->ops != &g_devfs_ops)
		return -1;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df || df->type != DEVFS_TYPE_PTY_MASTER)
		return -1;
	return df->pty_id;
}

int devfs_is_devfile(vfs_file_t *f)
{
	return (f && f->ops == &g_devfs_ops);
}
