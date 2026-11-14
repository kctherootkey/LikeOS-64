// LikeOS -- in-memory pseudo filesystems: the tree and its VFS glue.
//
// Copyright (C) 2026 The LikeOS Project

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
	/* Nodes are made concurrently (lookups of dynamic directories,
	 * drivers adding files): one number each. */
	n->ino = __atomic_add_fetch(&fs->next_ino, 1, __ATOMIC_RELAXED);
	n->transient = 1;
	return n;
}

void pfs_node_put(struct pfs_node *n)
{
	if (n && n->transient) {
		if (n->release)
			n->release(n);
		kfree(n);
	}
}

static struct pfs_node *pfs_child(struct pfs_node *dir, const char *name,
				  size_t len)
{
	/* Pairs with the release store that publishes a new node. */
	for (struct pfs_node *c = __atomic_load_n(&dir->children,
						  __ATOMIC_ACQUIRE);
	     c; c = c->sibling) {
		size_t k = 0;
		while (k < len && c->name[k] == name[k])
			k++;
		if (k == len && c->name[k] == 0)
			return c;
	}
	if (dir->lookup) {
		char tmp[PFS_NAME_MAX];
		if (len >= sizeof(tmp))
			return NULL;
		mm_memcpy(tmp, name, len);
		tmp[len] = 0;
		return dir->lookup(dir, tmp);
	}
	return NULL;
}

#define PFS_WALK_MAX 512
/* The open flag of the established ABI (libc fcntl.h): do not follow a
 * final link. */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif
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

/* pfs_lookup, and when the node found is itself a link, the node the link
 * leads to: what open() and stat() of a path must see.  A program that opens
 * /sys/dev/block/8:0 or /sys/class/block/sda wants the device's directory,
 * not the link -- it goes on to read "size" relative to that descriptor, and
 * a descriptor on the link itself is no directory to read anything in.
 * lstat(), readlink() and O_NOFOLLOW keep pfs_lookup.  A link that cannot be
 * resolved inside this file system (an absolute target elsewhere, such as
 * /proc/self/exe, or a dangling one) is returned as the link itself, which is
 * how every such path behaved before. */
static struct pfs_node *pfs_lookup_follow(struct pfs *fs, const char *relpath)
{
	char path[PFS_WALK_MAX];
	struct pfs_node *n;

	if (pfs_strlcpy(path, relpath, sizeof(path)) >= sizeof(path))
		return NULL;
	pfs_normalize(path);
	n = pfs_lookup(fs, path);
	for (unsigned hops = 0; n && n->type == PFS_LINK; hops++) {
		char next[PFS_WALK_MAX];
		struct pfs_node *m;
		const char *target = n->link;
		size_t k = 0;

		if (hops >= PFS_LINK_HOPS)
			return n; /* a loop, or too deep: leave it a link */
		if (target[0] == '/') {
			const char *rel = pfs_rel(fs, target);

			if (!rel)
				return n; /* outside this file system */
			while (*rel == '/')
				rel++;
			if (pfs_strlcpy(next, rel, sizeof(next)) >=
			    sizeof(next))
				return n;
		} else {
			/* Relative to the directory holding the link. */
			size_t dir = 0;

			for (size_t i = 0; path[i]; i++)
				if (path[i] == '/')
					dir = i;
			for (k = 0; k < dir; k++)
				next[k] = path[k];
			if (dir)
				next[k++] = '/';
			next[k] = 0;
			if (k + pfs_strlcpy(next + k, target,
					    sizeof(next) - k) >= sizeof(next))
				return n;
		}
		pfs_normalize(next);
		m = pfs_lookup(fs, next);
		if (!m)
			return n; /* dangling: report the link */
		pfs_node_put(n);
		n = m;
		pfs_strlcpy(path, next, sizeof(path));
	}
	return n;
}

/* Adding to the tree is serialised -- drivers register files from several
 * threads (hotplug, probe work, init) -- while lookups walk the lists
 * without a lock: a node is complete, template applied, before the store
 * that makes it reachable, and nodes are never removed.  One lock for all
 * instances: adding is rare. */
static spinlock_t g_pfs_add_lock = SPINLOCK_INIT("pfs_add");

/* A static child of `dir' named name[0..len) (no dynamic lookup: the tree
 * being built is the static one).  g_pfs_add_lock. */
static struct pfs_node *pfs_static_child(struct pfs_node *dir,
					 const char *name, size_t len)
{
	for (struct pfs_node *c = dir->children; c; c = c->sibling) {
		size_t k = 0;
		while (k < len && c->name[k] == name[k])
			k++;
		if (k == len && c->name[k] == 0)
			return c;
	}
	return NULL;
}

/* The settable fields of `tmpl' onto `n'.  The data fields go first and
 * the callbacks that use them last, with release stores: a node that is
 * already reachable (an existing one a caller adds to again) never shows
 * a callback without the arguments it reads. */
static void pfs_apply(struct pfs_node *n, const struct pfs_node *t)
{
	if (t->mode)
		n->mode = t->mode;
	if (t->uid)
		n->uid = t->uid;
	if (t->gid)
		n->gid = t->gid;
	if (t->show || t->store || t->read_at || t->list || t->lookup ||
	    t->list_snap || t->create || t->unlink || t->arg || t->arg2) {
		n->arg = t->arg;
		n->arg2 = t->arg2;
	}
	if (t->link[0])
		mm_memcpy(n->link, t->link, sizeof(n->link));
	if (t->store_max)
		n->store_max = t->store_max;
	if (t->release)
		__atomic_store_n(&n->release, t->release, __ATOMIC_RELEASE);
	if (t->unlink)
		__atomic_store_n(&n->unlink, t->unlink, __ATOMIC_RELEASE);
	if (t->create)
		__atomic_store_n(&n->create, t->create, __ATOMIC_RELEASE);
	if (t->show)
		__atomic_store_n(&n->show, t->show, __ATOMIC_RELEASE);
	if (t->store)
		__atomic_store_n(&n->store, t->store, __ATOMIC_RELEASE);
	if (t->read_at)
		__atomic_store_n(&n->read_at, t->read_at, __ATOMIC_RELEASE);
	if (t->list_free)
		__atomic_store_n(&n->list_free, t->list_free, __ATOMIC_RELEASE);
	if (t->list_snap)
		__atomic_store_n(&n->list_snap, t->list_snap, __ATOMIC_RELEASE);
	if (t->list || t->lookup)
		pfs_set_dynamic(n, t->list, t->lookup);
}

/* Find or create `path' (parents are created as directories on demand);
 * a node created for the last component is of `type' and carries the
 * fields of `tmpl' (may be NULL) from the moment it is reachable.  An
 * existing node is returned as it is, `tmpl' applied to it. */
static struct pfs_node *pfs_add(struct pfs *fs, const char *path,
				enum pfs_type type, const struct pfs_node *tmpl)
{
	const char *p = path;
	struct pfs_node *dir = &fs->root;

	while (*p == '/')
		p++;
	for (;;) {
		const char *e = p;
		while (*e && *e != '/')
			e++;
		size_t l = (size_t)(e - p);
		int last = !*e;
		struct pfs_node *fresh = NULL, *c;
		char tmp[64];

		if (!last && l >= sizeof(tmp))
			return NULL;
		if (l >= sizeof(tmp))
			l = sizeof(tmp) - 1; /* the name a node can hold */
		mm_memcpy(tmp, p, l);
		tmp[l] = 0;
		for (;;) {
			uint64_t fl;

			spin_lock_irqsave(&g_pfs_add_lock, &fl);
			c = pfs_static_child(dir, tmp, l);
			if (c && last && tmpl)
				pfs_apply(c, tmpl);
			if (!c && fresh) {
				fresh->sibling = dir->children;
				/* complete before it is reachable */
				__atomic_store_n(&dir->children, fresh,
						 __ATOMIC_RELEASE);
				c = fresh;
				fresh = NULL;
			}
			spin_unlock_irqrestore(&g_pfs_add_lock, fl);
			if (c)
				break;
			fresh = pfs_node_new(fs, dir, tmp,
					     last ? type : PFS_DIR);
			if (!fresh)
				return NULL;
			fresh->transient = 0;
			if (last && tmpl)
				pfs_apply(fresh, tmpl);
		}
		if (fresh)
			kfree(fresh); /* another thread added it first */
		if (last)
			return c;
		dir = c;
		p = e;
		while (*p == '/')
			p++;
	}
}

struct pfs_node *pfs_mkdir(struct pfs *fs, const char *path)
{
	return pfs_add(fs, path, PFS_DIR, NULL);
}

struct pfs_node *pfs_add_file(struct pfs *fs, const char *path,
			      pfs_show_t show, void *arg, uint64_t arg2)
{
	struct pfs_node t;

	mm_memset(&t, 0, sizeof(t));
	t.show = show;
	t.arg = arg;
	t.arg2 = arg2;
	return pfs_add(fs, path, PFS_FILE, &t);
}

struct pfs_node *pfs_add_file_rw(struct pfs *fs, const char *path,
				 pfs_show_t show, pfs_store_t store, void *arg,
				 uint64_t arg2)
{
	struct pfs_node t;

	mm_memset(&t, 0, sizeof(t));
	t.show = show;
	t.store = store;
	t.arg = arg;
	t.arg2 = arg2;
	t.mode = 0644;
	return pfs_add(fs, path, PFS_FILE, &t);
}

struct pfs_node *pfs_add_link(struct pfs *fs, const char *path,
			      const char *target)
{
	struct pfs_node t;
	size_t k = 0;

	mm_memset(&t, 0, sizeof(t));
	while (target[k] && k < sizeof(t.link) - 1) {
		t.link[k] = target[k];
		k++;
	}
	t.link[k] = 0;
	return pfs_add(fs, path, PFS_LINK, &t);
}

struct pfs_node *pfs_add_node(struct pfs *fs, const char *path,
			      enum pfs_type type, const struct pfs_node *tmpl)
{
	return pfs_add(fs, path, type, tmpl);
}

void pfs_set_dynamic(struct pfs_node *dir, pfs_list_t list, pfs_lookup_t lookup)
{
	/* Release stores: whatever the caller set up for the callbacks (the
	 * node's arg/arg2) is visible before they are. */
	__atomic_store_n(&dir->lookup, lookup, __ATOMIC_RELEASE);
	__atomic_store_n(&dir->list, list, __ATOMIC_RELEASE);
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
	uint64_t dirpos; /* where the next getdents resumes: see pfs_readdir() */
	/* A directory listed through list_snap: this handle's snapshot, and
	 * whether a rewind asked for a fresh one. */
	void *dirsnap;
	int dirsnap_refresh;
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


/* open(O_CREAT) of a name that is not there: the directory's create
 * callback makes the node, when the directory has one. */
static struct pfs_node *pfs_create_child(struct pfs *fs, const char *rel)
{
	char dir[PFS_WALK_MAX];
	const char *name;
	struct pfs_node *d, *n = NULL;
	size_t len, cut;

	len = pfs_strlcpy(dir, rel, sizeof(dir));
	if (len >= sizeof(dir))
		return NULL;
	while (len > 0 && dir[len - 1] == '/')
		dir[--len] = 0;
	cut = len;
	while (cut > 0 && dir[cut - 1] != '/')
		cut--;
	name = dir + cut;
	if (!*name || (name[0] == '.' && (!name[1] ||
					  (name[1] == '.' && !name[2]))) ||
	    len - cut >= PFS_NAME_MAX)
		return NULL;
	{
		/* the parent's path, then the name kept apart from it */
		char leaf[PFS_NAME_MAX];

		pfs_strlcpy(leaf, name, sizeof(leaf));
		dir[cut] = 0;
		d = pfs_lookup_follow(fs, dir);
		if (d && d->type == PFS_DIR && !d->transient && d->create)
			n = d->create(d, leaf);
	}
	pfs_node_put(d);
	return n;
}

static int pfs_open_common(struct pfs *fs, const char *path, int flags,
			   vfs_file_t **out)
{
	const char *rel = pfs_rel(fs, path);

	if (!rel)
		return ST_NOT_FOUND;
	if ((flags & (O_CREAT | O_TRUNC)) && !(flags & (O_WRONLY | O_RDWR)))
		return ST_PERM;
	struct pfs_node *n = (flags & O_NOFOLLOW) ? pfs_lookup(fs, rel) :
						    pfs_lookup_follow(fs, rel);
	if (!n && (flags & O_CREAT))
		n = pfs_create_child(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
	/* Only a file with a store callback opens for writing (O_TRUNC on
	 * it is harmless: there is nothing to truncate). */
	if ((flags & (O_WRONLY | O_RDWR)) && (n->type != PFS_FILE || !n->store)) {
		pfs_node_put(n);
		return ST_PERM;
	}
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
	/* stat() follows a final link, lstat() does not (pfs_lookup_follow
	 * keeps links it cannot resolve here as links). */
	struct pfs_node *n = follow ? pfs_lookup_follow(fs, rel) :
				      pfs_lookup(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
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
	/* A read from the start renders the file again, as the established
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
	pfs_file_t *pf = f->fs_private;
	char tmp[PFS_STORE_MAX];

	if (!pf || !pf->node || pf->node->type != PFS_FILE || !pf->node->store)
		return -EACCES;
	if (bytes <= 0)
		return 0;
	if (pf->node->store_max > 0) {
		/* One record per write, delivered whole or not at all. */
		char *big;
		long r;

		if (bytes > pf->node->store_max)
			return -EFBIG;
		big = kalloc((size_t)bytes);
		if (!big)
			return -ENOMEM;
		smap_disable();
		mm_memcpy(big, buf, bytes);
		smap_enable();
		r = pf->node->store(pf->node, big, bytes);
		kfree(big);
		return r;
	}
	if (bytes > PFS_STORE_MAX)
		bytes = PFS_STORE_MAX;
	smap_disable();
	mm_memcpy(tmp, buf, bytes);
	smap_enable();
	return pf->node->store(pf->node, tmp, bytes);
}

static long pfs_seek(vfs_file_t *f, long offset, int whence)
{
	pfs_file_t *pf = f->fs_private;
	long np;

	if (!pf)
		return -EINVAL;
	/* A directory's offset is the resume position of pfs_readdir():
	 * rewinddir() seeks to 0, seekdir() to a d_off it was handed. */
	if (pf->node->type == PFS_DIR) {
		if (whence == 0 && offset >= 0)
			pf->dirpos = (uint64_t)offset;
		else if (!(whence == 1 && offset == 0))
			return -EINVAL;
		/* rewinddir(): a snapshot listing starts over from a fresh
		 * snapshot; any other position keeps the one it refers to. */
		if (whence == 0 && offset == 0)
			__atomic_store_n(&pf->dirsnap_refresh, 1,
					 __ATOMIC_RELEASE);
		return (long)pf->dirpos;
	}
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
			       const char *name, uint64_t ino, int type,
			       uint64_t next)
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
	d->d_off = (int64_t)next;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = (uint8_t)type;
	mm_memcpy(d->d_name, name, nl + 1);
	smap_enable();
	*off += reclen;
	return reclen;
}

/* Directory positions, as getdents resumes from them and d_off reports them:
 *
 *   0                 "."
 *   1                 ".."
 *   2                 the first static child
 *   3 + ino           the static child after the one with inode `ino'
 *   PFS_POS_DYN + k   the dynamic child with key >= k (pfs_list_t)
 *
 * Static children are visited in descending inode order, which is the order
 * of the list (pfs_add prepends, inodes only grow, nothing is removed); a
 * child added while a listing is under way has a higher inode and is simply
 * not in that listing.  Nothing here counts entries, so nothing shifts when
 * one comes or goes between two calls. */
#define PFS_POS_STATIC 2ULL
#define PFS_POS_DYN (1ULL << 32)

static long pfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	pfs_file_t *pf = f->fs_private;
	struct pfs_node *dir;
	unsigned off = 0;

	if (!pf || pf->node->type != PFS_DIR)
		return -ENOTDIR;
	dir = pf->node;
	for (;;) {
		uint64_t pos = pf->dirpos, next, ino;
		const char *name;
		char nm[PFS_NAME_MAX];
		int t;

		if (pos == 0) {
			name = ".";
			ino = dir->ino;
			t = DT_DIR;
			next = 1;
		} else if (pos == 1) {
			name = "..";
			ino = dir->parent ? dir->parent->ino : dir->ino;
			t = DT_DIR;
			next = PFS_POS_STATIC;
		} else if (pos < PFS_POS_DYN) {
			uint64_t bound = pos == PFS_POS_STATIC ? ~0ULL :
								 pos - 3;
			struct pfs_node *pick = NULL;

			for (struct pfs_node *c = __atomic_load_n(
				     &dir->children, __ATOMIC_ACQUIRE);
			     c; c = c->sibling)
				if (c->ino < bound && (!pick || c->ino > pick->ino))
					pick = c;
			if (!pick) {
				pf->dirpos = PFS_POS_DYN;
				continue;
			}
			name = pick->name;
			ino = pick->ino;
			t = pick->type == PFS_DIR ? DT_DIR :
			    (pick->type == PFS_LINK ? DT_LNK : DT_REG);
			next = 3 + pick->ino;
		} else {
			unsigned cursor = (unsigned)(pos - PFS_POS_DYN);
			pfs_list_snap_t ls =
				__atomic_load_n(&dir->list_snap, __ATOMIC_ACQUIRE);

			if (ls) {
				int refresh = __atomic_exchange_n(
					&pf->dirsnap_refresh, 0,
					__ATOMIC_ACQ_REL);
				int lr = ls(dir, &pf->dirsnap, refresh, &cursor,
					    nm, sizeof(nm), &t);

				if (lr < 0) {
					if (off == 0)
						return lr;
					break;
				}
				if (!lr)
					break;
			} else if (!dir->list ||
				   !dir->list(dir, &cursor, nm, sizeof(nm), &t)) {
				break;
			}
			name = nm;
			ino = dir->ino * 1000 + cursor;
			next = PFS_POS_DYN + cursor;
		}
		if (!pfs_put_dirent(buf, (unsigned)bytes, &off, name, ino, t, next)) {
			/* Not even one record fits: the caller's buffer is
			 * too small, which is its error, not end of file. */
			if (off == 0)
				return -EINVAL;
			break;
		}
		pf->dirpos = next;
	}
	return (long)off;
}

static int pfs_close(vfs_file_t *f)
{
	pfs_file_t *pf = f->fs_private;

	if (pf) {
		if (pf->data)
			kfree(pf->data);
		if (pf->dirsnap && pf->node->list_free)
			pf->node->list_free(pf->node, pf->dirsnap);
		pf->dirsnap = NULL;
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

/* The VFS entry points.  The instance is the mount's (vfs_register_mount()
 * is given the struct pfs as the mount's fs_data), and the path arrives
 * absolute, as each instance's own lookups expect. */
static int pfs_vop_open(vfs_mount_t *m, const char *p, int fl, vfs_file_t **o)
{
	return pfs_open_common((struct pfs *)m->fs_data, p, fl, o);
}

static int pfs_vop_stat(vfs_mount_t *m, const char *p, struct kstat *st)
{
	return pfs_stat_common((struct pfs *)m->fs_data, p, st, 1);
}

static int pfs_vop_lstat(vfs_mount_t *m, const char *p, struct kstat *st)
{
	return pfs_stat_common((struct pfs *)m->fs_data, p, st, 0);
}

static int pfs_vop_readlink(vfs_mount_t *m, const char *p, char *b,
			    unsigned long s)
{
	return pfs_readlink_common((struct pfs *)m->fs_data, p, b, s);
}

/* unlink(): only a file whose node says how it is removed. */
static int pfs_vop_unlink(vfs_mount_t *m, const char *p)
{
	struct pfs *fs = (struct pfs *)m->fs_data;
	const char *rel = pfs_rel(fs, p);
	struct pfs_node *n;
	int r;

	if (!rel)
		return ST_NOT_FOUND;
	n = pfs_lookup(fs, rel);
	if (!n)
		return ST_NOT_FOUND;
	if (n->type == PFS_DIR) {
		pfs_node_put(n);
		return ST_ISDIR;
	}
	if (n->type != PFS_FILE || !n->unlink) {
		pfs_node_put(n);
		return ST_PERM;
	}
	r = n->unlink(n);
	pfs_node_put(n);
	switch (r) {
	case 0:
		return ST_OK;
	case -ENOENT:
		return ST_NOT_FOUND;
	case -EACCES:
		return ST_ACCESS;
	case -EPERM:
		return ST_PERM;
	case -ENOMEM:
		return ST_NOMEM;
	case -EINVAL:
		return ST_INVALID;
	case -EROFS:
		return ST_ROFS;
	case -EBUSY:
		return ST_BUSY;
	default:
		return ST_IO;
	}
}

static int pfs_vop_chdir(vfs_mount_t *m, const char *p)
{
	struct kstat st;
	int r = pfs_stat_common((struct pfs *)m->fs_data, p, &st, 1);

	if (r != ST_OK)
		return r;
	return S_ISDIR(st.st_mode) ? ST_OK : ST_NOT_FOUND;
}


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
	fs->ops.open = pfs_vop_open;
	fs->ops.stat = pfs_vop_stat;
	fs->ops.lstat = pfs_vop_lstat;
	fs->ops.readlink = pfs_vop_readlink;
	fs->ops.chdir = pfs_vop_chdir;
	fs->ops.unlink = pfs_vop_unlink;
	(void)pfs_of_ops;
	(void)g_pfs_current_open;
}

const vfs_ops_t *pfs_ops(struct pfs *fs)
{
	return &fs->ops;
}
