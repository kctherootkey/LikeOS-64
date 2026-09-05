// LikeOS-64 -- in-memory pseudo filesystems: the tree and its VFS glue.
#include <kernel/fs/pseudofs.h>
#include <kernel/uapi/status.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/dirent.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ---- tree ----------------------------------------------------------- */

struct pfs_node *pfs_node_new(struct pfs *fs, struct pfs_node *parent,
			      const char *name, enum pfs_type type)
{
	struct pfs_node *n = kalloc(sizeof(*n));

	if (!n)
		return NULL;
	mm_memset(n, 0, sizeof(*n));
	size_t k = 0;
	while (name[k] && k < sizeof(n->name) - 1) {
		n->name[k] = name[k];
		k++;
	}
	n->name[k] = 0;
	n->type = type;
	n->mode = type == PFS_DIR ? 0555 : (type == PFS_LINK ? 0777 : 0444);
	n->parent = parent;
	n->fs = fs;
	n->ino = ++fs->next_ino;
	n->transient = 1;
	return n;
}

void pfs_node_put(struct pfs_node *n)
{
	if (n && n->transient)
		kfree(n);
}

static struct pfs_node *pfs_child(struct pfs_node *dir, const char *name,
				  size_t len)
{
	for (struct pfs_node *c = dir->children; c; c = c->sibling) {
		size_t k = 0;
		while (k < len && c->name[k] == name[k])
			k++;
		if (k == len && c->name[k] == 0)
			return c;
	}
	if (dir->lookup) {
		char tmp[64];
		if (len >= sizeof(tmp))
			return NULL;
		mm_memcpy(tmp, name, len);
		tmp[len] = 0;
		return dir->lookup(dir, tmp);
	}
	return NULL;
}

#define PFS_WALK_MAX 512
#define PFS_LINK_HOPS 8

/* Defined below; the walk needs it to turn an absolute link target back
 * into a path relative to this file system's mount point. */
static const char *pfs_rel(struct pfs *fs, const char *path);

/* Bounded copy; returns the length the source needed, so a caller can see
 * that it did not fit. */
static size_t pfs_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t n = 0;

	while (src[n]) {
		if (n + 1 < cap)
			dst[n] = src[n];
		n++;
	}
	if (cap)
		dst[n < cap ? n : cap - 1] = 0;
	return n;
}

/* Resolve `.' and `..' in place, textually: every path here stays inside one
 * pseudo file system and its links are written to. */
static void pfs_normalize(char *path)
{
	char *out = path;
	const char *p = path;

	while (*p) {
		const char *e = p;
		size_t len;

		while (*e && *e != '/')
			e++;
		len = (size_t)(e - p);
		if (len == 1 && p[0] == '.') {
			/* nothing */
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			while (out > path && out[-1] != '/')
				out--;
			if (out > path)
				out--; /* the separator itself */
		} else if (len) {
			if (out > path)
				*out++ = '/';
			for (size_t k = 0; k < len; k++)
				*out++ = p[k];
		}
		p = e;
		while (*p == '/')
			p++;
	}
	*out = 0;
}

/* Walk `relpath' ("a/b/c", "" or "/" = root).  Each step releases the
 * transient node of the previous step; the final node is returned held. */
struct pfs_node *pfs_lookup(struct pfs *fs, const char *relpath)
{
	/* The path being walked, rewritten each time a link is crossed. */
	char work[PFS_WALK_MAX];
	unsigned hops = 0;

	if (pfs_strlcpy(work, relpath, sizeof(work)) >= sizeof(work))
		return NULL;
	pfs_normalize(work);

restart: {
	struct pfs_node *cur = &fs->root;
	char *p = work;

	while (*p == '/')
		p++;
	while (*p) {
		char *e = p;
		while (*e && *e != '/')
			e++;
		if (cur->type != PFS_DIR) {
			pfs_node_put(cur);
			return NULL;
		}
		struct pfs_node *next = pfs_child(cur, p, (size_t)(e - p));
		/* A transient parent stays alive while its child is looked
		 * up above; drop it now. */
		if (cur != &fs->root)
			pfs_node_put(cur);
		if (!next)
			return NULL;
		cur = next;

		char *rest = e;
		while (*rest == '/')
			rest++;

		/* A link with path still to come is not the answer, it is the
		 * way to it.  Stopping at one and demanding a directory made
		 * every conventional /sys path unwalkable: what a graphics
		 * driver asks about the device behind /dev/dri/card0 is
		 * /sys/dev/char/226:0/device/..., and every component of that
		 * after the first is reached through a link. */
		if (cur->type == PFS_LINK && *rest) {
			char next_path[PFS_WALK_MAX];
			const char *target = cur->link;
			size_t k;

			if (++hops > PFS_LINK_HOPS) {
				pfs_node_put(cur);
				return NULL; /* a loop, or too deep */
			}
			if (target[0] == '/') {
				/* Absolute: inside this file system or
				 * nowhere. */
				const char *rel = pfs_rel(fs, target);
				if (!rel) {
					pfs_node_put(cur);
					return NULL;
				}
				while (*rel == '/')
					rel++;
				k = pfs_strlcpy(next_path, rel,
						sizeof(next_path));
			} else {
				/* Relative to the directory holding the link,
				 * which is the path text up to this
				 * component. */
				size_t pre = (size_t)(p - work);

				if (pre >= sizeof(next_path)) {
					pfs_node_put(cur);
					return NULL;
				}
				for (k = 0; k < pre; k++)
					next_path[k] = work[k];
				next_path[k] = 0;
				k += pfs_strlcpy(next_path + k, target,
						 sizeof(next_path) - k);
			}
			if (k + 1 >= sizeof(next_path)) {
				pfs_node_put(cur);
				return NULL;
			}
			if (k && next_path[k - 1] != '/')
				next_path[k++] = '/';
			next_path[k] = 0;
			if (k + pfs_strlcpy(next_path + k, rest,
					    sizeof(next_path) - k) >=
			    sizeof(next_path)) {
				pfs_node_put(cur);
				return NULL;
			}
			pfs_node_put(cur);
			pfs_strlcpy(work, next_path, sizeof(work));
			pfs_normalize(work);
			goto restart;
		}
		p = rest;
	}
	return cur;
	}
}

static struct pfs_node *pfs_add(struct pfs *fs, const char *path,
				enum pfs_type type)
{
	/* Split off the last component; parents are created as directories
	 * on demand. */
	const char *p = path;
	struct pfs_node *dir = &fs->root;

	while (*p == '/')
		p++;
	for (;;) {
		const char *e = p;
		while (*e && *e != '/')
			e++;
		if (!*e) {
			struct pfs_node *n = pfs_child(dir, p, (size_t)(e - p));
			if (n)
				return n; /* already there */
			n = pfs_node_new(fs, dir, p, type);
			if (!n)
				return NULL;
			n->transient = 0;
			n->sibling = dir->children;
			dir->children = n;
			return n;
		}
		struct pfs_node *c = pfs_child(dir, p, (size_t)(e - p));
		if (!c) {
			char tmp[64];
			size_t l = (size_t)(e - p);
			if (l >= sizeof(tmp))
				return NULL;
			mm_memcpy(tmp, p, l);
			tmp[l] = 0;
			c = pfs_node_new(fs, dir, tmp, PFS_DIR);
			if (!c)
				return NULL;
			c->transient = 0;
			c->sibling = dir->children;
			dir->children = c;
		}
		dir = c;
		p = e;
		while (*p == '/')
			p++;
	}
}

struct pfs_node *pfs_mkdir(struct pfs *fs, const char *path)
{
	return pfs_add(fs, path, PFS_DIR);
}

struct pfs_node *pfs_add_file(struct pfs *fs, const char *path,
			      pfs_show_t show, void *arg, uint64_t arg2)
{
	struct pfs_node *n = pfs_add(fs, path, PFS_FILE);

	if (n) {
		n->show = show;
		n->arg = arg;
		n->arg2 = arg2;
	}
	return n;
}

struct pfs_node *pfs_add_link(struct pfs *fs, const char *path,
			      const char *target)
{
	struct pfs_node *n = pfs_add(fs, path, PFS_LINK);

	if (n) {
		size_t k = 0;
		while (target[k] && k < sizeof(n->link) - 1) {
			n->link[k] = target[k];
			k++;
		}
		n->link[k] = 0;
	}
	return n;
}

void pfs_set_dynamic(struct pfs_node *dir, pfs_list_t list, pfs_lookup_t lookup)
{
	dir->list = list;
	dir->lookup = lookup;
}

long pfs_printf(char *buf, long cap, long pos, const char *fmt, ...)
{
	char tmp[256];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	int n = kvsnprintf(tmp, sizeof(tmp), fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return pos;
	for (int i = 0; i < n; i++)
		if (pos + i < cap)
			buf[pos + i] = tmp[i];
	return pos + n;
}

/* ---- VFS glue --------------------------------------------------------- */

typedef struct {
	vfs_file_t vfs;
	struct pfs *fs;
	struct pfs_node *node;
	char *data; /* file contents, generated at open and at each rewind */
	long cap; /* bytes allocated at data */
	long len;
	long pos;
	unsigned dirpos;
} pfs_file_t;

/* Strip the mount prefix: "/sys/bus/pci" -> "bus/pci". */
static const char *pfs_rel(struct pfs *fs, const char *path)
{
	const char *m = fs->mount;
	size_t k = 0;

	while (m[k] && path[k] == m[k])
		k++;
	if (m[k] != 0)
		return NULL;
	if (path[k] != 0 && path[k] != '/')
		return NULL;
	return path + k;
}

static long pfs_generate(struct pfs_node *n, char **out, long *cap_out)
{
	long cap = 4096;

	for (int tries = 0; tries < 4; tries++) {
		char *buf = kalloc(cap);
		if (!buf)
			return -ENOMEM;
		long len = (n->show && !n->read_at) ? n->show(n, buf, cap) : 0;
		if (len < 0) {
			kfree(buf);
			return len;
		}
		if (len <= cap) {
			*out = buf;
			*cap_out = cap;
			return len;
		}
		kfree(buf);
		cap = len + 1;
	}
	return -E2BIG;
}

static int pfs_fill_stat(struct pfs_node *n, struct kstat *st)
{
	uint64_t now = timer_get_epoch();

	mm_memset(st, 0, sizeof(*st));
	st->st_ino = n->ino;
	st->st_uid = n->uid;
	st->st_gid = n->gid;
	st->st_nlink = 1;
	st->st_blksize = 4096;
	st->st_atime = now;
	st->st_mtime = now;
	st->st_ctime = now;
	switch (n->type) {
	case PFS_DIR:
		st->st_mode = S_IFDIR | (n->mode & 07777);
		break;
	case PFS_LINK:
		st->st_mode = S_IFLNK | 0777;
		st->st_size = 0;
		for (const char *q = n->link; *q; q++)
			st->st_size++;
		break;
	default:
		st->st_mode = S_IFREG | (n->mode & 07777);
		/* Size is unknown until generated; a plausible non-zero
		 * value keeps "is it empty" probes honest. */
		st->st_size = 4096;
		break;
	}
	return ST_OK;
}

static struct pfs *pfs_of_ops(const vfs_ops_t *ops)
{
	/* The ops table is embedded in the fs; recover the container. */
	return (struct pfs *)((char *)ops - __builtin_offsetof(struct pfs, ops));
}

static struct pfs *g_pfs_current_open; /* set per call by the wrappers */

/* Each mounted instance gets its own trampolines, so the VFS call carries
 * no fs pointer.  Two instances is what this kernel has; a third would
 * need a third set.  */

static int pfs_open_common(struct pfs *fs, const char *path, int flags,
			   vfs_file_t **out)
{
	const char *rel = pfs_rel(fs, path);

	if (!rel)
		return ST_NOT_FOUND;
	if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)))
		return ST_PERM;
	struct pfs_node *n = pfs_lookup(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
	pfs_file_t *pf = kalloc(sizeof(*pf));
	if (!pf) {
		pfs_node_put(n);
		return ST_NOMEM;
	}
	mm_memset(pf, 0, sizeof(*pf));
	pf->vfs.ops = &fs->ops;
	pf->vfs.fs_private = pf;
	pf->fs = fs;
	pf->node = n;
	if (n->type == PFS_FILE) {
		long len = pfs_generate(n, &pf->data, &pf->cap);
		if (len < 0) {
			pfs_node_put(n);
			kfree(pf);
			return len == -ENOMEM ? ST_NOMEM : ST_IO;
		}
		pf->len = len;
	}
	*out = &pf->vfs;
	return ST_OK;
}

static int pfs_stat_common(struct pfs *fs, const char *path, struct kstat *st,
			   int follow)
{
	const char *rel = pfs_rel(fs, path);

	if (!rel)
		return ST_NOT_FOUND;
	struct pfs_node *n = pfs_lookup(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
	(void)follow; /* links are reported as links; the VFS resolves */
	int r = pfs_fill_stat(n, st);
	pfs_node_put(n);
	return r;
}

static int pfs_readlink_common(struct pfs *fs, const char *path, char *buf,
			       unsigned long bufsz)
{
	const char *rel = pfs_rel(fs, path);

	if (!rel)
		return ST_NOT_FOUND;
	struct pfs_node *n = pfs_lookup(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
	if (n->type != PFS_LINK) {
		pfs_node_put(n);
		return ST_INVALID;
	}
	unsigned long k = 0;
	while (n->link[k] && k < bufsz) {
		buf[k] = n->link[k];
		k++;
	}
	pfs_node_put(n);
	return (int)k;
}

static long pfs_read(vfs_file_t *f, void *buf, long bytes)
{
	pfs_file_t *pf = f->fs_private;

	if (!pf || pf->node->type != PFS_FILE)
		return -EISDIR;
	if (pf->node->read_at) {
		long n = pf->node->read_at(pf->node, (uint64_t)pf->pos, buf,
					   bytes);
		if (n > 0)
			pf->pos += n;
		return n;
	}
	/* A read from the start renders the file again, as the reference
	 * procfs does (seq_file), and readers rely on that: a monitor that
	 * keeps /proc/meminfo open and rewinds it every few seconds -- WebKit's
	 * memory pressure monitor is one -- otherwise reads the numbers from
	 * open() for as long as the process lives.  Rendered in place when it
	 * fits, so a second thread reading the same open file at most sees a
	 * torn line, never a freed buffer. */
	if (pf->pos == 0 && pf->node->show && pf->data) {
		long len = pf->node->show(pf->node, pf->data, pf->cap);

		if (len >= 0 && len <= pf->cap) {
			pf->len = len;
		} else if (len > pf->cap) {
			char *fresh;
			long fcap;

			len = pfs_generate(pf->node, &fresh, &fcap);
			if (len >= 0) {
				char *old = pf->data;

				pf->data = fresh;
				pf->cap = fcap;
				pf->len = len;
				kfree(old);
			}
		}
	}
	if (pf->pos >= pf->len)
		return 0;
	long n = pf->len - pf->pos;
	if (n > bytes)
		n = bytes;
	smap_disable();
	mm_memcpy(buf, pf->data + pf->pos, n);
	smap_enable();
	pf->pos += n;
	return n;
}

static long pfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	(void)f;
	(void)buf;
	(void)bytes;
	return -EACCES;
}

static long pfs_seek(vfs_file_t *f, long offset, int whence)
{
	pfs_file_t *pf = f->fs_private;
	long np;

	if (!pf)
		return -EINVAL;
	switch (whence) {
	case 0:
		np = offset;
		break;
	case 1:
		np = pf->pos + offset;
		break;
	case 2:
		np = pf->len + offset;
		break;
	default:
		return -EINVAL;
	}
	if (np < 0)
		return -EINVAL;
	pf->pos = np;
	return np;
}

static unsigned pfs_put_dirent(char *buf, unsigned cap, unsigned *off,
			       const char *name, uint64_t ino, int type)
{
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned reclen = (unsigned)(sizeof(struct dirent64) + nl + 1 + 7) & ~7u;
	if (*off + reclen > cap)
		return 0;
	struct dirent64 *d = (struct dirent64 *)(buf + *off);
	smap_disable();
	d->d_ino = ino;
	d->d_off = *off + reclen;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = (uint8_t)type;
	mm_memcpy(d->d_name, name, nl + 1);
	smap_enable();
	*off += reclen;
	return reclen;
}

static long pfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	pfs_file_t *pf = f->fs_private;
	unsigned off = 0;

	if (!pf || pf->node->type != PFS_DIR)
		return -ENOTDIR;
	if (pf->dirpos)
		return 0; /* one shot: everything fits in one call */
	if (!pfs_put_dirent(buf, (unsigned)bytes, &off, ".", pf->node->ino, DT_DIR))
		return -EINVAL;
	pfs_put_dirent(buf, (unsigned)bytes, &off, "..",
		       pf->node->parent ? pf->node->parent->ino : pf->node->ino,
		       DT_DIR);
	for (struct pfs_node *c = pf->node->children; c; c = c->sibling) {
		int t = c->type == PFS_DIR ? DT_DIR :
			(c->type == PFS_LINK ? DT_LNK : DT_REG);
		if (!pfs_put_dirent(buf, (unsigned)bytes, &off, c->name, c->ino, t))
			break;
	}
	if (pf->node->list) {
		char nm[64];
		int t;
		for (unsigned i = 0; pf->node->list(pf->node, i, nm, sizeof(nm), &t); i++) {
			if (!pfs_put_dirent(buf, (unsigned)bytes, &off, nm,
					    pf->node->ino * 1000 + i + 1, t))
				break;
		}
	}
	pf->dirpos = 1;
	return (long)off;
}

static int pfs_close(vfs_file_t *f)
{
	pfs_file_t *pf = f->fs_private;

	if (pf) {
		if (pf->data)
			kfree(pf->data);
		pfs_node_put(pf->node);
		kfree(pf);
	}
	return ST_OK;
}

static int pfs_fstat(vfs_file_t *f, struct kstat *st)
{
	pfs_file_t *pf = f->fs_private;

	if (!pf)
		return -EINVAL;
	pfs_fill_stat(pf->node, st);
	if (pf->node->type == PFS_FILE)
		st->st_size = (uint64_t)pf->len;
	return 0;
}

/* Per-instance trampolines. */
#define PFS_INSTANCE(N)                                                       \
	static struct pfs *g_pfs_##N;                                         \
	static int pfs_##N##_open(const char *p, int fl, vfs_file_t **o)      \
	{                                                                     \
		return pfs_open_common(g_pfs_##N, p, fl, o);                  \
	}                                                                     \
	static int pfs_##N##_stat(const char *p, struct kstat *st)            \
	{                                                                     \
		return pfs_stat_common(g_pfs_##N, p, st, 1);                  \
	}                                                                     \
	static int pfs_##N##_lstat(const char *p, struct kstat *st)           \
	{                                                                     \
		return pfs_stat_common(g_pfs_##N, p, st, 0);                  \
	}                                                                     \
	static int pfs_##N##_readlink(const char *p, char *b, unsigned long s) \
	{                                                                     \
		return pfs_readlink_common(g_pfs_##N, p, b, s);               \
	}                                                                     \
	static int pfs_##N##_chdir(const char *p)                             \
	{                                                                     \
		struct kstat st;                                              \
		int r = pfs_stat_common(g_pfs_##N, p, &st, 1);                \
		if (r != ST_OK)                                               \
			return r;                                             \
		return S_ISDIR(st.st_mode) ? ST_OK : ST_NOT_FOUND;            \
	}

PFS_INSTANCE(0)
PFS_INSTANCE(1)

static int g_pfs_count;

void pfs_init(struct pfs *fs, const char *mount)
{
	mm_memset(fs, 0, sizeof(*fs));
	fs->mount = mount;
	fs->root.type = PFS_DIR;
	fs->root.mode = 0555;
	fs->root.fs = fs;
	fs->root.ino = 1;
	fs->next_ino = 1;
	fs->ops.read = pfs_read;
	fs->ops.write = pfs_write;
	fs->ops.seek = pfs_seek;
	fs->ops.readdir = pfs_readdir;
	fs->ops.close = pfs_close;
	fs->ops.fstat = pfs_fstat;
	if (g_pfs_count == 0) {
		g_pfs_0 = fs;
		fs->ops.open = pfs_0_open;
		fs->ops.stat = pfs_0_stat;
		fs->ops.lstat = pfs_0_lstat;
		fs->ops.readlink = pfs_0_readlink;
		fs->ops.chdir = pfs_0_chdir;
	} else if (g_pfs_count == 1) {
		g_pfs_1 = fs;
		fs->ops.open = pfs_1_open;
		fs->ops.stat = pfs_1_stat;
		fs->ops.lstat = pfs_1_lstat;
		fs->ops.readlink = pfs_1_readlink;
		fs->ops.chdir = pfs_1_chdir;
	} else {
		BUG_ON(1); /* more instances than trampolines */
	}
	g_pfs_count++;
	(void)pfs_of_ops;
	(void)g_pfs_current_open;
}

const vfs_ops_t *pfs_ops(struct pfs *fs)
{
	return &fs->ops;
}
