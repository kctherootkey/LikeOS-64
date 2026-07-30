// LikeOS-64 - devfs (device filesystem)
#include <kernel/fs/devfs.h>
#include <kernel/mm/shm.h>
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
#define DEVFS_TYPE_FD_DIR 13 /* /dev/fd: the caller's own descriptors */
#define DEVFS_TYPE_SHM_DIR 14 /* /dev/shm: POSIX shared memory namespace  */
#define DEVFS_TYPE_SHM 15 /* /dev/shm/<name>; object in `shm`        */
#define DEVFS_TYPE_MAX DEVFS_TYPE_SHM

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
	shm_object_t *shm; // shared memory object (DEVFS_TYPE_SHM)
	/* The name this handle was opened under.  Several device paths share
	 * one type (/dev/tty, /dev/console and /dev/tty0 are all DEVFS_TYPE_TTY),
	 * so the type alone cannot say which node a descriptor refers to — and
	 * that is exactly what a /dev/fd/N symlink has to report. */
	char path[32];
} devfs_file_t;

static vfs_ops_t g_devfs_ops;

/* ftruncate() on a shared memory object sets how much memory it holds; this is
 * how a process decides the size of a region before mapping it.  No other
 * device node has a length to set. */
static int devfs_truncate(vfs_file_t *f, unsigned long size);
/* unlink() removes a name from /dev/shm.  Anything already holding the object
 * keeps working — see shm_unlink_name(). */
static int devfs_unlink(const char *path);

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

/* Virtual /dev/fd/N plus the /dev/stdin, /dev/stdout, /dev/stderr aliases:
 * opening one of these names duplicates the caller's own descriptor N, the
 * conventional Unix semantics.  This only classifies the path; the actual
 * duplication happens in the syscall layer, which owns the fd table (devfs
 * has no notion of the caller's descriptors).  Returns the descriptor
 * number the path names, or -1 if it is not one of these paths.  The
 * caller validates the descriptor itself (bad fd => EBADF), so the only
 * bound enforced here is against integer overflow. */
int devfs_fd_alias_target(const char *path)
{
	if (is_prefix(path, "/dev/fd/")) {
		const char *p = path + 8;
		if (*p < '0' || *p > '9')
			return -1;
		int n = 0;
		for (; *p >= '0' && *p <= '9'; p++) {
			n = n * 10 + (*p - '0');
			if (n > 65535)
				return -1;
		}
		return (*p == '\0') ? n : -1;
	}
	if (is_path(path, "/dev/stdin"))
		return 0;
	if (is_path(path, "/dev/stdout"))
		return 1;
	if (is_path(path, "/dev/stderr"))
		return 2;
	return -1;
}

int devfs_init(void)
{
	g_devfs_ops.open = devfs_open;
	g_devfs_ops.stat = devfs_stat;
	g_devfs_ops.read = devfs_read;
	g_devfs_ops.write = devfs_write;
	g_devfs_ops.seek = devfs_seek;
	g_devfs_ops.readdir = devfs_readdir;
	g_devfs_ops.truncate = devfs_truncate;
	g_devfs_ops.unlink = devfs_unlink;
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

static int devfs_open_impl(const char *path, int flags, vfs_file_t **out,
			   task_t *cur)
{
	if (!path || !out)
		return ST_INVALID;

	if (is_path(path, "/dev") || is_path(path, "/dev/")) {
		return devfs_open_dir(DEVFS_TYPE_DIR, out);
	}
	if (is_path(path, "/dev/pts") || is_path(path, "/dev/pts/")) {
		return devfs_open_dir(DEVFS_TYPE_PTS_DIR, out);
	}
	/* /dev/fd itself is a directory listing the caller's descriptors; the
	 * individual /dev/fd/N entries never reach devfs (the syscall layer
	 * turns opening one into a dup - see devfs_fd_alias_target). */
	if (is_path(path, "/dev/fd") || is_path(path, "/dev/fd/")) {
		return devfs_open_dir(DEVFS_TYPE_FD_DIR, out);
	}

	/* POSIX shared memory lives under /dev/shm.  Routing it through devfs
	 * means a handle on an object is an ordinary vfs_file, so dup, fork,
	 * exec, close refcounting, fstat, ftruncate and descriptor passing all
	 * work already instead of each needing a special case. */
	if (is_path(path, "/dev/shm") || is_path(path, "/dev/shm/")) {
		return devfs_open_dir(DEVFS_TYPE_SHM_DIR, out);
	}
	if (is_prefix(path, "/dev/shm/")) {
		const char *nm = path + 9;
		shm_object_t *obj;
		devfs_file_t *df;

		if (!*nm)
			return ST_INVALID;
		/* A name is a single component: nothing here is a directory. */
		for (const char *q = nm; *q; q++)
			if (*q == '/')
				return ST_INVALID;

		obj = shm_lookup_get(nm);
		if (!obj) {
			if (!(flags & O_CREAT))
				return ST_NOT_FOUND;
			obj = shm_create_get(nm, 0600);
			if (!obj)
				return ST_NOMEM;
		} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
			shm_put(obj);
			return ST_EXISTS;
		}

		df = devfs_alloc_file();
		if (!df) {
			shm_put(obj);
			return ST_NOMEM;
		}
		df->type = DEVFS_TYPE_SHM;
		df->shm = obj;
		df->fpos = 0;
		*out = &df->vfs;
		return ST_OK;
	}

	/* Opening a terminal makes it this process's CONTROLLING terminal, when
	 * all of the conventional conditions hold: the caller leads its own
	 * session, has no controlling terminal yet, did not pass O_NOCTTY, and
	 * the terminal does not already belong to another session.
	 *
	 * Programs rely on this rather than on TIOCSCTTY, which is a BSD
	 * extension they only reach for on platforms known to need it.  xterm is
	 * one: its child calls setsid(), opens the pts slave, and expects the
	 * open to have done this.  Without it the child kept NO controlling
	 * terminal, so /dev/tty resolved to the fallback -- the console -- and
	 * anything that deliberately talks to the terminal rather than to stdout
	 * went to the wrong screen.  ssh's "Are you sure you want to continue
	 * connecting" prompt appeared on the system console while the user sat
	 * in front of an xterm waiting for it. */
	if (cur && !(flags & O_NOCTTY) && cur->ctty == NULL &&
	    cur->sid == (int)cur->id) {
		tty_t *cand = NULL;
		if (is_prefix(path, "/dev/pts/")) {
			int pid = 0;
			const char *q = path + 9;
			if (*q) {
				for (; *q >= '0' && *q <= '9'; q++)
					pid = pid * 10 + (*q - '0');
				if (!*q)
					cand = tty_get_pty_slave(pid);
			}
		} else if (is_path(path, "/dev/console") ||
			   is_path(path, "/dev/tty0")) {
			cand = tty_get_console();
		}
		/* sid 0 means unclaimed; re-claiming our own session is a no-op. */
		if (cand && (cand->sid == 0 || cand->sid == cur->sid)) {
			cur->ctty = cand;
			cand->sid = cur->sid;
			if (cur->pgid > 0)
				cand->fg_pgid = cur->pgid;
		}
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
		fbdev_opened();
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

int devfs_open_for_task(const char *path, int flags, vfs_file_t **out,
			task_t *cur)
{
	int r = devfs_open_impl(path, flags, out, cur);
	/* Remember the name so a /dev/fd/N symlink can report which node the
	 * descriptor refers to.  Trailing-slash forms ("/dev/pts/") normalise
	 * to the directory itself. */
	if (r == ST_OK && out && *out) {
		devfs_file_t *df = (devfs_file_t *)(*out)->fs_private;
		if (df && path) {
			size_t i = 0;
			while (path[i] && i < sizeof(df->path) - 1) {
				df->path[i] = path[i];
				i++;
			}
			while (i > 1 && df->path[i - 1] == '/')
				i--;
			df->path[i] = '\0';
		}
	}
	return r;
}

int devfs_open(const char *path, int flags, vfs_file_t **out)
{
	// Fallback without task context: use console tty for /dev/tty
	return devfs_open_for_task(path, flags, out, NULL);
}

/* The /dev path an open devfs handle was created from, for /dev/fd/N symlink
 * targets.  Returns the length written, or -1 if `f` is not a devfs handle. */
int devfs_fpath(vfs_file_t *f, char *out, size_t cap)
{
	if (!f || f->ops != &g_devfs_ops || !out || cap == 0)
		return -1;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df || !df->path[0])
		return -1;
	size_t i = 0;
	while (df->path[i] && i < cap - 1) {
		out[i] = df->path[i];
		i++;
	}
	out[i] = '\0';
	return (int)i;
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
 * group-restricted (tty/input/video, conventional Unix modes and
 * major:minor numbers); the pseudo devices stay world-accessible and the
 * /dev directories world-searchable (0755). */
int devfs_stat(const char *path, struct kstat *st)
{
	if (!path || !st)
		return ST_INVALID;
	mm_memset(st, 0, sizeof(*st));
	uint64_t now = timer_get_epoch(); /* real wall-clock seconds */
	/* The shared memory namespace: world-writable and sticky like /tmp, so
	 * any user can create an object but only its owner can remove it. */
	if (is_path(path, "/dev/shm") || is_path(path, "/dev/shm/")) {
		st->st_mode = S_IFDIR | 01777;
		st->st_nlink = 1;
		st->st_atime = now;
		st->st_mtime = now;
		st->st_ctime = now;
		return ST_OK;
	}
	/* An individual object: a regular file whose length is how much memory
	 * it holds.  Without this, stat() on the path failed outright and the
	 * objects were invisible to anything that looks before it opens. */
	if (is_prefix(path, "/dev/shm/")) {
		shm_object_t *o = shm_lookup_get(path + 9);
		if (!o)
			return ST_NOT_FOUND;
		st->st_mode = S_IFREG | (o->mode & 0777);
		st->st_uid = o->uid;
		st->st_gid = o->gid;
		st->st_ino = o->ino;
		st->st_nlink = 1;
		st->st_size = (long)o->size;
		st->st_blksize = 4096;
		st->st_blocks = (long)(o->npages * 8);
		st->st_atime = now;
		st->st_mtime = now;
		st->st_ctime = now;
		shm_put(o);
		return ST_OK;
	}

	if (is_path(path, "/dev") || is_path(path, "/dev/") ||
	    is_path(path, "/dev/pts") || is_path(path, "/dev/pts/") ||
	    is_path(path, "/dev/fd") || is_path(path, "/dev/fd/") ||
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
	    is_path(path, "/dev/fd") || is_path(path, "/dev/fd/") ||
	    is_path(path, "/dev/input") || is_path(path, "/dev/input/") ||
	    is_path(path, "/dev/shm") || is_path(path, "/dev/shm/")) {
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
		(unsigned)sizeof(struct dirent64) + name_len + 1;
	reclen = (reclen + 7u) & ~7u;
	WARN_ON(reclen % 8 != 0);
	if (*out_off + reclen > out_size)
		return 0;
	// SMAP-aware write to user buffer
	smap_disable();
	struct dirent64 *d = (struct dirent64 *)(out + *out_off);
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
	    df->type != DEVFS_TYPE_INPUT_DIR && df->type != DEVFS_TYPE_FD_DIR &&
	    df->type != DEVFS_TYPE_SHM_DIR) {
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
		/* Descriptor aliases: the /dev/fd directory (DT_DIR) and the
		 * three standard-stream names, which are symlinks to the
		 * descriptor they name (DT_LNK), as on every other Unix. */
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "fd", 12, 4);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "stdin", 13, 10);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "stdout", 14, 10);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "stderr", 15, 10);
		devfs_write_dirent64((char *)buf, (unsigned)bytes, &out_off,
				     "shm", 16, 4);
		df->dir_pos = 1;
		return (long)out_off;
	}
	/* /dev/shm: one entry per live shared memory object. */
	if (df->type == DEVFS_TYPE_SHM_DIR) {
		char nm[SHM_NAME_MAX];
		for (unsigned i = 0; shm_enumerate(i, nm, sizeof(nm)); i++)
			devfs_write_dirent64((char *)buf, (unsigned)bytes,
					     &out_off, nm, 300 + i, 8 /*DT_REG*/);
		df->dir_pos = 1;
		return (long)out_off;
	}
	/* /dev/fd: one entry per descriptor the CALLING task has open. */
	if (df->type == DEVFS_TYPE_FD_DIR) {
		task_t *cur = sched_current();
		for (int i = 0; cur && i < TASK_MAX_FDS; i++) {
			/* 0/1/2 are always open (they may be console markers
			 * with a NULL slot); the rest need a live entry. */
			if (i > 2 && task_fds(cur)[i] == NULL)
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
				while (t > 0)
					name[len++] = tmp[--t];
			}
			name[len] = '\0';
			/* DT_LNK: these entries are symlinks to whatever the
			 * descriptor refers to, the conventional Unix shape.
			 * DT_UNKNOWN forced ls to lstat every one of them. */
			if (!devfs_write_dirent64((char *)buf, (unsigned)bytes,
						  &out_off, name,
						  (uint64_t)(300 + i), 10))
				break;
		}
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

/* The shm layer speaks negative errnos internally; the VFS op contract is in
 * status_t.  The two overlap numerically without meaning the same thing —
 * -ENOENT is -2, which as a status_t is ST_UNSUPPORTED — so a value crossing
 * this boundary untranslated surfaces as a completely unrelated error. */
static int devfs_shm_status(int rc)
{
	switch (rc) {
	case 0:
		return ST_OK;
	case -ENOENT:
		return ST_NOT_FOUND;
	case -EEXIST:
		return ST_EXISTS;
	case -ENOMEM:
		return ST_NOMEM;
	case -EBUSY:
		return ST_BUSY;
	case -EINVAL:
	default:
		return ST_INVALID;
	}
}

static int devfs_truncate(vfs_file_t *f, unsigned long size)
{
	devfs_file_t *df;

	if (!f || f->ops != &g_devfs_ops)
		return ST_INVALID;
	df = (devfs_file_t *)f->fs_private;
	if (!df || df->type != DEVFS_TYPE_SHM || !df->shm)
		return ST_INVALID; /* no other device node has a settable length */
	return devfs_shm_status(shm_set_size(df->shm, size));
}

static int devfs_unlink(const char *path)
{
	const char *nm;

	if (!path)
		return ST_INVALID;
	/* Only the shared memory namespace has removable names; every other
	 * /dev node is owned by a driver. */
	if (!is_prefix(path, "/dev/shm/"))
		return ST_PERM;
	nm = path + 9;
	if (!*nm)
		return ST_INVALID;
	for (const char *q = nm; *q; q++)
		if (*q == '/')
			return ST_INVALID;
	return devfs_shm_status(shm_unlink_name(nm));
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
			/* Release an exclusive grab held through THIS handle.
			 * Keyed on the handle, not on the closing task: a
			 * descriptor is not always closed by the task that
			 * opened it, and matching on the task left the grab in
			 * place -- which suppressed every pointer event from
			 * then on. */
			evdev_release_grab_by_owner(df->evdev_id, f);
		} else if (df->type == DEVFS_TYPE_FB0) {
			/* On the last close, put the console back on screen:
			 * whoever had the framebuffer mapped has been drawing
			 * over it, and with no virtual terminals there is no
			 * other point at which the console would be redrawn. */
			fbdev_closed();
		} else if (df->type == DEVFS_TYPE_SHM) {
			/* Drops this handle's reference; an already-unlinked
			 * object releases its pages when the last one goes. */
			shm_put(df->shm);
			df->shm = NULL;
		}
		kfree(df);
	}
	return ST_OK;
}

/* FIONBIO: the ioctl spelling of fcntl(F_SETFL, O_NONBLOCK).
 *
 * It is a property of the DESCRIPTOR, not of the device behind it, so it is
 * handled here for every device rather than in each driver -- the tty layer
 * does not even receive the vfs_file_t whose flags it would have to change.
 *
 * Not optional: programs pick one spelling or the other by #ifdef and cannot
 * fall back at runtime.  xterm uses this one on its pty master, and without it
 * exits at startup with "Reason: main: ioctl() failed on FIONBIO" -- an error
 * about a call that simply was not implemented. */
#define DEVFS_FIONBIO 0x5421UL

int devfs_ioctl(vfs_file_t *f, unsigned long req, void *argp, task_t *cur)
{
	if (!f || f->ops != &g_devfs_ops)
		return -ENOTTY;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df)
		return -ENOTTY;

	if (req == DEVFS_FIONBIO) {
		int on;

		if (!argp)
			return -EFAULT;
		smap_disable();
		on = *(const int *)argp;
		smap_enable();
		if (on)
			f->flags |= O_NONBLOCK;
		else
			f->flags &= ~O_NONBLOCK;
		return 0;
	}
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
		return evdev_ioctl(df->evdev_id, req, argp, cur, f);
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
	/* The directory handles (/dev, /dev/pts, /dev/input, /dev/fd) are not
	 * character devices; without these cases fstat() fell through to the
	 * caller's "unknown - call it a regular file" default, so an open
	 * directory descriptor listed as -rw-r--r--. */
	case DEVFS_TYPE_DIR:
	case DEVFS_TYPE_PTS_DIR:
	case DEVFS_TYPE_INPUT_DIR:
	case DEVFS_TYPE_FD_DIR:
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 1;
		st->st_size = 0;
		return 0;
	case DEVFS_TYPE_SHM_DIR:
		/* World-writable and sticky, like /tmp: any user may create an
		 * object, but only its owner may remove it. */
		st->st_mode = S_IFDIR | 01777;
		st->st_nlink = 1;
		st->st_size = 0;
		return 0;
	case DEVFS_TYPE_SHM:
		/* A shared memory object is a regular file as far as callers
		 * are concerned — ftruncate sets its length and fstat reports
		 * it, which is exactly how a client learns how much was
		 * shared with it. */
		if (!df->shm)
			return -EINVAL;
		st->st_mode = S_IFREG | (df->shm->mode & 0777);
		st->st_uid = df->shm->uid;
		st->st_gid = df->shm->gid;
		st->st_ino = df->shm->ino;
		st->st_nlink = 1;
		st->st_size = (long)df->shm->size;
		st->st_blksize = 4096;
		st->st_blocks = (long)(df->shm->npages * 8);
		return 0;
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

/* The shared memory object behind a handle, or NULL.  mmap() uses this to map
 * the object's own physical pages rather than allocating fresh ones — which is
 * the entire point: two unrelated processes must end up looking at the same
 * memory. */
shm_object_t *devfs_shm_object(vfs_file_t *f)
{
	if (!f || f->ops != &g_devfs_ops)
		return NULL;
	devfs_file_t *df = (devfs_file_t *)f->fs_private;
	if (!df || df->type != DEVFS_TYPE_SHM)
		return NULL;
	return df->shm;
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
