// LikeOS -- inotify: filesystem change events behind a descriptor.
//
// inotify_init1() makes an instance, inotify_add_watch() asks it to watch a
// file or directory for a set of events, and read() returns the events as
// they happen, one struct inotify_event each.  GLib's file monitor is built on
// exactly this, and so is every program that refreshes a folder view when
// another program changes it.
//
// Two tables hold the watches, both under one lock:
//
//   by identity (filesystem, inode) -- every object something watches has
//   one MARK here, carrying the watches of every instance on it.  Events
//   about an object itself are found this way, so a file watched under one
//   name is seen being written through another.
//
//   by path -- directory marks are also filed under the path they were
//   watched by.  An event about a name ("x was created in /a") is found by
//   looking up the name's parent path, which costs a string hash rather than
//   a path resolution.  The VFS has no directory object shared between
//   filesystems that could stand in for it, and the textual path is the one
//   thing every operation already has.  The price: a directory watched
//   through a symbolic link is filed under the link's path, and names
//   reached through the real path are not reported to it.  Renaming a
//   watched directory -- or one above it -- rewrites the recorded paths
//   (fsnotify_renamed()).
//
// Delivery collects the (instance, watch descriptor, event) triples under
// the table lock and queues them after dropping it, so the per-instance queue
// lock is never taken inside the table lock and nothing is allocated under
// either.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/device.h>
#include <kernel/fs/fsnotify.h>
#include <kernel/fs/namei.h>
#include <kernel/fs/file.h>
#include <kernel/uapi/inotify.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

/* Queued events per instance before IN_Q_OVERFLOW stands in for the rest. */
#define INOTIFY_MAX_QUEUED 16384
/* Watches per instance, and in the whole system. */
#define INOTIFY_MAX_WATCHES 8192
#define INOTIFY_MAX_WATCHES_ALL 65536
/* Instances in the whole system. */
#define INOTIFY_MAX_INSTANCES 1024

#define MARK_BUCKETS 256

/* The events a watch can ask for. */
#define IN_WATCHABLE IN_ALL_EVENTS
/* Bits that only go to the directory holding the name, and bits that only go
 * to the object itself. */
#define IN_DIR_ONLY (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)
#define IN_SELF_ONLY (IN_DELETE_SELF | IN_MOVE_SELF)

struct inotify_ctx;
struct fsn_mark;

struct inotify_watch {
	struct inotify_watch *mark_next; /* the mark's watches */
	struct inotify_watch *ctx_next; /* the instance's watches */
	struct inotify_ctx *ctx;
	struct fsn_mark *mark;
	int wd;
	uint32_t mask; /* event bits | IN_ONESHOT | IN_EXCL_UNLINK */
};

struct fsn_mark {
	struct fsn_mark *ino_next;
	struct fsn_mark *path_next; /* directory marks only */
	const void *fs;
	uint64_t ino;
	int isdir;
	int in_path_table;
	uint32_t path_hash;
	struct inotify_watch *watches;
	char path[VFS_MAX_PATH];
};

/* One queued event.  `ev' and its name are what read() copies out. */
struct inotify_rec {
	struct inotify_rec *next;
	uint32_t size; /* sizeof(struct inotify_event) + ev.len */
	struct inotify_event ev;
};

struct inotify_ctx {
	/* The descriptor's reference plus one per delivery in flight. */
	int refs;
	/* Protects the queue. */
	spinlock_t lock;
	struct wait_queue_head wq;
	struct inotify_rec *head, *tail;
	uint32_t nqueued;
	uint64_t bytes;
	/* Under g_lock. */
	struct inotify_watch *watches;
	int nwatches;
	int next_wd;
};

static spinlock_t g_lock = SPINLOCK_INIT("inotify");
static struct fsn_mark *g_by_ino[MARK_BUCKETS];
static struct fsn_mark *g_by_path[MARK_BUCKETS];
static int g_nwatches;
static int g_ninstances;
/* How many watches ask for each event bit; g_fsnotify_mask is the set of
 * bits with a non-zero count. */
static uint32_t g_bitcount[32];
uint32_t g_fsnotify_mask;
static uint32_t g_cookie;

static const struct device_ops inotify_ops;

/* ---- small helpers ------------------------------------------------------ */

static unsigned long str_len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static uint32_t path_hash(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s) {
		h ^= (uint8_t)*s++;
		h *= 16777619u;
	}
	return h;
}

static unsigned ino_bucket(const void *fs, uint64_t ino)
{
	uint64_t h = ino * 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)fs;

	return (unsigned)(h >> 56) % MARK_BUCKETS;
}

/* The directory part of `path' into `out' ("/a/b" -> "/a", "/a" -> "/"), and
 * the offset of the last component in `*name'.  0 when there is none. */
static int split_path(const char *path, char *out, unsigned long outsz,
		      unsigned long *name)
{
	unsigned long len = str_len(path), slash = (unsigned long)-1;

	if (len == 0 || path[0] != '/')
		return 0;
	while (len > 1 && path[len - 1] == '/')
		len--;
	for (unsigned long i = 0; i < len; i++)
		if (path[i] == '/')
			slash = i;
	if (slash == (unsigned long)-1 || slash + 1 >= len)
		return 0;
	if (slash == 0) {
		if (outsz < 2)
			return 0;
		out[0] = '/';
		out[1] = '\0';
	} else {
		if (slash >= outsz)
			return 0;
		mm_memcpy(out, path, slash);
		out[slash] = '\0';
	}
	*name = slash + 1;
	return 1;
}

/* ---- the tables (all under g_lock) -------------------------------------- */

static struct fsn_mark *mark_by_ino(const void *fs, uint64_t ino)
{
	for (struct fsn_mark *m = g_by_ino[ino_bucket(fs, ino)]; m;
	     m = m->ino_next)
		if (m->fs == fs && m->ino == ino)
			return m;
	return NULL;
}

static struct fsn_mark *mark_by_path(const char *path)
{
	uint32_t h = path_hash(path);

	for (struct fsn_mark *m = g_by_path[h % MARK_BUCKETS]; m;
	     m = m->path_next)
		if (m->path_hash == h && str_eq(m->path, path))
			return m;
	return NULL;
}

static void path_table_add(struct fsn_mark *m)
{
	unsigned b;

	m->path_hash = path_hash(m->path);
	b = m->path_hash % MARK_BUCKETS;
	m->path_next = g_by_path[b];
	g_by_path[b] = m;
	m->in_path_table = 1;
}

static void path_table_del(struct fsn_mark *m)
{
	struct fsn_mark **pp;

	if (!m->in_path_table)
		return;
	pp = &g_by_path[m->path_hash % MARK_BUCKETS];
	while (*pp && *pp != m)
		pp = &(*pp)->path_next;
	if (*pp)
		*pp = m->path_next;
	m->in_path_table = 0;
}

static void mark_unlink(struct fsn_mark *m)
{
	struct fsn_mark **pp = &g_by_ino[ino_bucket(m->fs, m->ino)];

	while (*pp && *pp != m)
		pp = &(*pp)->ino_next;
	if (*pp)
		*pp = m->ino_next;
	path_table_del(m);
}

/* Keep g_fsnotify_mask equal to the union of the live watches' event bits. */
static void account(uint32_t old_mask, uint32_t new_mask)
{
	uint32_t m = 0;

	old_mask &= IN_WATCHABLE;
	new_mask &= IN_WATCHABLE;
	for (int b = 0; b < 32; b++) {
		uint32_t bit = 1u << b;

		if (old_mask & bit)
			g_bitcount[b]--;
		if (new_mask & bit)
			g_bitcount[b]++;
		if (g_bitcount[b])
			m |= bit;
	}
	__atomic_store_n(&g_fsnotify_mask, m, __ATOMIC_RELAXED);
}

/* Take watch `w' out of both lists.  Frees the mark when it was the last
 * watch on it; the watch itself is the caller's to free. */
static void watch_detach(struct inotify_watch *w)
{
	struct fsn_mark *m = w->mark;
	struct inotify_watch **pp = &m->watches;

	while (*pp && *pp != w)
		pp = &(*pp)->mark_next;
	if (*pp)
		*pp = w->mark_next;
	pp = &w->ctx->watches;
	while (*pp && *pp != w)
		pp = &(*pp)->ctx_next;
	if (*pp)
		*pp = w->ctx_next;
	w->ctx->nwatches--;
	g_nwatches--;
	account(w->mask, 0);
	if (!m->watches) {
		mark_unlink(m);
		kfree(m);
	}
}

/* ---- queueing ----------------------------------------------------------- */

static void ctx_get(struct inotify_ctx *c)
{
	__atomic_fetch_add(&c->refs, 1, __ATOMIC_ACQ_REL);
}

static void ctx_put(struct inotify_ctx *c)
{
	if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	for (struct inotify_rec *r = c->head, *n; r; r = n) {
		n = r->next;
		kfree(r);
	}
	kfree(c);
}

/* Queue one event on `c'.  An event identical to the last one still unread
 * is dropped: a stream of writes reads as one IN_MODIFY until somebody
 * looks. */
static void queue_event(struct inotify_ctx *c, int wd, uint32_t mask,
			uint32_t cookie, const char *name)
{
	unsigned long nl = name ? str_len(name) : 0;
	uint32_t len = nl ? (uint32_t)((nl + 1 + 15) & ~15ul) : 0;
	uint32_t size = (uint32_t)sizeof(struct inotify_event) + len;
	struct inotify_rec *r = kalloc(sizeof(*r) + len);
	uint64_t fl;

	if (r) {
		mm_memset(r, 0, sizeof(*r) + len);
		r->size = size;
		r->ev.wd = wd;
		r->ev.mask = mask;
		r->ev.cookie = cookie;
		r->ev.len = len;
		if (nl)
			mm_memcpy(r->ev.name, name, nl);
	}

	spin_lock_irqsave(&c->lock, &fl);
	struct inotify_rec *t = c->tail;
	if (r && t && t->ev.wd == wd && t->ev.mask == mask &&
	    t->ev.cookie == cookie && t->ev.len == len &&
	    (len == 0 || str_eq(t->ev.name, r->ev.name))) {
		spin_unlock_irqrestore(&c->lock, fl);
		kfree(r);
		return;
	}
	if (!r || c->nqueued >= INOTIFY_MAX_QUEUED) {
		/* One overflow record stands for everything dropped since. */
		if (t && t->ev.mask == IN_Q_OVERFLOW) {
			spin_unlock_irqrestore(&c->lock, fl);
			kfree(r);
			return;
		}
		if (!r) {
			spin_unlock_irqrestore(&c->lock, fl);
			r = kalloc(sizeof(*r));
			if (!r)
				return;
			spin_lock_irqsave(&c->lock, &fl);
		}
		mm_memset(r, 0, sizeof(*r));
		r->size = (uint32_t)sizeof(struct inotify_event);
		r->ev.wd = -1;
		r->ev.mask = IN_Q_OVERFLOW;
	}
	if (c->tail)
		c->tail->next = r;
	else
		c->head = r;
	c->tail = r;
	c->nqueued++;
	c->bytes += r->size;
	spin_unlock_irqrestore(&c->lock, fl);
	poll_notify_wq(&c->wq);
}

/* ---- delivery ----------------------------------------------------------- */

struct fsn_target {
	struct inotify_ctx *ctx;
	int wd;
	uint32_t mask; /* 0: only IN_IGNORED is due */
	uint32_t cookie;
	int named; /* carries the name */
	int ignored; /* follow with IN_IGNORED: the watch was removed */
};

/* What one event is delivered to.  Grows past the inline slots when many
 * instances watch the same object -- with the table lock held, which is
 * safe because the lock keeps interrupts off and an allocation with
 * interrupts off never waits for reclaim (kernel/mm/slab.c). */
struct fsn_batch {
	struct fsn_target inl[16];
	struct fsn_target *t;
	int n, cap;
	/* Watches removed while collecting (IN_ONESHOT, deletion), freed once
	 * the lock is dropped. */
	struct inotify_watch *dead;
};

static void batch_init(struct fsn_batch *b)
{
	b->t = b->inl;
	b->n = 0;
	b->cap = (int)(sizeof(b->inl) / sizeof(b->inl[0]));
	b->dead = NULL;
}

/* A fresh slot holding a reference on `c', or NULL when memory ran out (the
 * event is then lost for that instance). */
static struct fsn_target *batch_add(struct fsn_batch *b,
				    struct inotify_ctx *c)
{
	if (b->n == b->cap) {
		struct fsn_target *nt = kalloc(sizeof(*nt) * (size_t)b->cap * 2);

		if (!nt)
			return NULL;
		mm_memcpy(nt, b->t, sizeof(*nt) * (size_t)b->n);
		if (b->t != b->inl)
			kfree(b->t);
		b->t = nt;
		b->cap *= 2;
	}
	struct fsn_target *t = &b->t[b->n++];
	mm_memset(t, 0, sizeof(*t));
	ctx_get(c);
	t->ctx = c;
	return t;
}

/* Collect what the watches of `m' are told about `mask'.  Under g_lock. */
static void collect(struct fsn_batch *b, struct fsn_mark *m, uint32_t mask,
		    uint32_t isdir_bit, uint32_t cookie, int named)
{
	for (struct inotify_watch *w = m->watches, *next; w; w = next) {
		next = w->mark_next;
		uint32_t hit = w->mask & mask;

		if (!hit)
			continue;
		struct fsn_target *t = batch_add(b, w->ctx);
		if (!t)
			continue;
		t->wd = w->wd;
		t->mask = hit | isdir_bit;
		t->cookie = cookie;
		t->named = named;
		if (w->mask & IN_ONESHOT) {
			t->ignored = 1;
			watch_detach(w);
			w->mark_next = b->dead;
			b->dead = w;
		}
	}
}

/* Queue what was collected.  Called with no lock held. */
static void flush(struct fsn_batch *b, const char *name)
{
	for (int i = 0; i < b->n; i++) {
		struct fsn_target *t = &b->t[i];

		if (t->mask)
			queue_event(t->ctx, t->wd, t->mask, t->cookie,
				    t->named ? name : NULL);
		if (t->ignored)
			queue_event(t->ctx, t->wd, IN_IGNORED, 0, NULL);
		ctx_put(t->ctx);
	}
	for (struct inotify_watch *w = b->dead, *n; w; w = n) {
		n = w->mark_next;
		kfree(w);
	}
	if (b->t != b->inl)
		kfree(b->t);
	batch_init(b);
}

void fsnotify_event(const char *path, const struct fsnotify_ident *id,
		    uint32_t mask, uint32_t cookie, int check_name)
{
	struct fsn_batch b;
	char parent[VFS_MAX_PATH];
	unsigned long name = 0;
	int have_parent = 0;
	uint32_t self_mask = mask & ~IN_DIR_ONLY;
	uint32_t dir_mask = mask & ~IN_SELF_ONLY;
	uint32_t isdir = (id && id->isdir) ? IN_ISDIR : 0;
	uint64_t fl;

	if (!fsnotify_wants(mask))
		return;
	batch_init(&b);
	if (path && dir_mask && split_path(path, parent, sizeof(parent), &name))
		have_parent = 1;

	/* An event found through a descriptor names the path the descriptor
	 * was opened with.  If that name has since been renamed away, the
	 * directory must not hear about it -- but resolving the name costs a
	 * lookup, so only when such a directory is actually watched, and never
	 * where the lookup could not sleep. */
	if (have_parent && check_name) {
		int watched;

		spin_lock_irqsave(&g_lock, &fl);
		watched = mark_by_path(parent) != NULL;
		spin_unlock_irqrestore(&g_lock, fl);
		if (!watched) {
			have_parent = 0;
		} else if (id && id->valid && irqs_enabled()) {
			struct fsnotify_ident now;

			if (vfs_notify_ident(path, 1, &now) != ST_OK ||
			    now.fs != id->fs || now.ino != id->ino)
				have_parent = 0;
		}
	}

	spin_lock_irqsave(&g_lock, &fl);
	if (id && id->valid && self_mask) {
		struct fsn_mark *m = mark_by_ino(id->fs, id->ino);

		if (m)
			collect(&b, m, self_mask,
				(self_mask & IN_SELF_ONLY) ? 0 : isdir,
				(self_mask & IN_MOVE) ? cookie : 0, 0);
	}
	if (have_parent) {
		struct fsn_mark *m = mark_by_path(parent);

		if (m)
			collect(&b, m, dir_mask, isdir,
				(dir_mask & IN_MOVE) ? cookie : 0, 1);
	}
	spin_unlock_irqrestore(&g_lock, fl);
	flush(&b, have_parent ? path + name : NULL);
}

void fsnotify_gone(const struct fsnotify_ident *id)
{
	struct fsn_batch b;
	uint64_t fl;

	if (!id || !id->valid || !fsnotify_wants(~0u))
		return;
	batch_init(&b);
	spin_lock_irqsave(&g_lock, &fl);
	struct fsn_mark *m = mark_by_ino(id->fs, id->ino);
	/* Every watch is removed, whether or not it asked for IN_DELETE_SELF:
	 * the inode number may be reused for an unrelated object, and a watch
	 * left behind would report that object's events as this one's.  The
	 * last watch takes the mark with it. */
	while (m) {
		struct inotify_watch *w = m->watches;
		int last = w->mark_next == NULL;
		struct fsn_target *t = batch_add(&b, w->ctx);

		if (t) {
			t->wd = w->wd;
			t->mask = (w->mask & IN_DELETE_SELF) ? IN_DELETE_SELF :
							       0;
			t->ignored = 1;
		}
		watch_detach(w);
		w->mark_next = b.dead;
		b.dead = w;
		if (last)
			break;
	}
	spin_unlock_irqrestore(&g_lock, fl);
	flush(&b, NULL);
}

void fsnotify_renamed(const char *oldpath, const char *newpath)
{
	unsigned long ol = str_len(oldpath), nl = str_len(newpath);
	struct fsn_mark *moved = NULL;
	char tmp[VFS_MAX_PATH];
	uint64_t fl;

	if (!fsnotify_wants(~0u))
		return;
	spin_lock_irqsave(&g_lock, &fl);
	for (unsigned b = 0; b < MARK_BUCKETS; b++) {
		struct fsn_mark **pp = &g_by_path[b];

		while (*pp) {
			struct fsn_mark *m = *pp;
			int match = 1;

			for (unsigned long i = 0; i < ol; i++)
				if (m->path[i] != oldpath[i]) {
					match = 0;
					break;
				}
			if (match && m->path[ol] != '\0' && m->path[ol] != '/')
				match = 0;
			if (!match) {
				pp = &m->path_next;
				continue;
			}
			*pp = m->path_next;
			m->in_path_table = 0;
			m->path_next = moved;
			moved = m;
		}
	}
	while (moved) {
		struct fsn_mark *m = moved;
		unsigned long tail = str_len(m->path + ol);

		moved = m->path_next;
		if (nl + tail + 1 > sizeof(m->path)) {
			/* The new name does not fit: the directory can no
			 * longer be found by path, only by identity. */
			continue;
		}
		mm_memcpy(tmp, newpath, nl);
		mm_memcpy(tmp + nl, m->path + ol, tail + 1);
		mm_memcpy(m->path, tmp, nl + tail + 1);
		path_table_add(m);
	}
	spin_unlock_irqrestore(&g_lock, fl);
}

uint32_t fsnotify_cookie(void)
{
	uint32_t c;

	do
		c = __atomic_add_fetch(&g_cookie, 1, __ATOMIC_RELAXED);
	while (c == 0);
	return c;
}

/* ---- the descriptor ----------------------------------------------------- */

static int inotify_wait(struct inotify_ctx *c)
{
	task_t *cur = sched_current();

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&c->lock, &fl);
		if (c->head) {
			spin_unlock_irqrestore(&c->lock, fl);
			return 0;
		}
		if (signal_pending(cur)) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -EINTR;
		}
		struct wait_queue_entry we;
		wq_entry_init(&we, cur);
		wq_add(&c->wq, &we);
		cur->wait_channel = c;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&c->lock, fl);
		sched_schedule();
		wq_remove(&c->wq, &we);
	}
}

/* Whole events only: as many as fit, EINVAL when not even the first does. */
static long inotify_read(vfs_file_t *f, void *buf, long bytes, int nonblock)
{
	struct inotify_ctx *c = device_file_priv(f);
	struct inotify_rec *list, **tailp, *r;
	long total = 0;
	uint64_t fl;

	for (;;) {
		spin_lock_irqsave(&c->lock, &fl);
		if (c->head)
			break;
		spin_unlock_irqrestore(&c->lock, fl);
		if (nonblock)
			return -EAGAIN;
		int rc = inotify_wait(c);
		if (rc)
			return rc;
	}
	if ((long)c->head->size > bytes) {
		spin_unlock_irqrestore(&c->lock, fl);
		return -EINVAL;
	}
	/* Detach what fits; copy it out with the lock dropped, as the copy may
	 * fault. */
	list = NULL;
	tailp = &list;
	while ((r = c->head) && total + (long)r->size <= bytes) {
		c->head = r->next;
		if (!c->head)
			c->tail = NULL;
		c->nqueued--;
		c->bytes -= r->size;
		total += r->size;
		r->next = NULL;
		*tailp = r;
		tailp = &r->next;
	}
	spin_unlock_irqrestore(&c->lock, fl);

	long off = 0;
	int fault = 0;
	for (r = list; r;) {
		struct inotify_rec *n = r->next;

		if (!fault && copy_to_user((char *)buf + off, &r->ev, r->size))
			fault = 1;
		off += r->size;
		kfree(r);
		r = n;
	}
	return fault ? -EFAULT : total;
}

static short inotify_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct inotify_ctx *c = device_file_priv(f);

	poll_wait(pt, f, &c->wq);
	return ((events & POLLIN) && c->head) ? POLLIN : 0;
}

static long inotify_ioctl(vfs_file_t *f, unsigned long req, void *argp,
			  struct task *cur)
{
	struct inotify_ctx *c = device_file_priv(f);
	uint64_t fl;
	int n;

	(void)cur;
	if (req != 0x541B /* FIONREAD */)
		return -ENOTTY;
	if (!argp || !validate_user_ptr((uint64_t)(uintptr_t)argp, sizeof(n)))
		return -EFAULT;
	spin_lock_irqsave(&c->lock, &fl);
	n = (int)c->bytes;
	spin_unlock_irqrestore(&c->lock, fl);
	return copy_to_user(argp, &n, sizeof(n)) ? -EFAULT : 0;
}

static void inotify_release(vfs_file_t *f)
{
	struct inotify_ctx *c = device_file_priv(f);
	struct inotify_watch *dead = NULL;
	uint64_t fl;

	spin_lock_irqsave(&g_lock, &fl);
	while (c->watches) {
		struct inotify_watch *w = c->watches;

		watch_detach(w);
		w->mark_next = dead;
		dead = w;
	}
	g_ninstances--;
	spin_unlock_irqrestore(&g_lock, fl);
	for (struct inotify_watch *w = dead, *n; w; w = n) {
		n = w->mark_next;
		kfree(w);
	}
	ctx_put(c);
}

static const struct device_ops inotify_ops = {
	.read = inotify_read,
	.poll = inotify_poll,
	.ioctl = inotify_ioctl,
	.release = inotify_release,
};

/* ---- syscalls ----------------------------------------------------------- */

int64_t sys_inotify_init1(uint64_t flags)
{
	task_t *cur = sched_current();
	uint64_t fl;

	if (!cur)
		return -EFAULT;
	if (flags & ~(uint64_t)(IN_CLOEXEC | IN_NONBLOCK))
		return -EINVAL;
	struct inotify_ctx *c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	mm_memset(c, 0, sizeof(*c));
	c->refs = 1;
	c->next_wd = 1;
	spinlock_init(&c->lock, "inotify");
	wq_head_init(&c->wq, "inotify");

	spin_lock_irqsave(&g_lock, &fl);
	if (g_ninstances >= INOTIFY_MAX_INSTANCES) {
		spin_unlock_irqrestore(&g_lock, fl);
		kfree(c);
		return -EMFILE;
	}
	g_ninstances++;
	spin_unlock_irqrestore(&g_lock, fl);

	vfs_file_t *file = device_anon_file(&inotify_ops, c, "anon_inode:inotify",
					    O_RDONLY | ((flags & IN_NONBLOCK) ?
								O_NONBLOCK :
								0));
	if (!file) {
		spin_lock_irqsave(&g_lock, &fl);
		g_ninstances--;
		spin_unlock_irqrestore(&g_lock, fl);
		kfree(c);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	if (flags & IN_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

/* The instance behind descriptor `fd', held; NULL with the error in *err. */
static vfs_file_t *inotify_fdget(task_t *cur, uint64_t fd, int *err)
{
	vfs_file_t *f = fdget(cur, (int)fd);

	if (!f) {
		*err = -EBADF;
		return NULL;
	}
	if (device_file_ops(f) != &inotify_ops) {
		fdput(f);
		*err = -EINVAL;
		return NULL;
	}
	return f;
}

int64_t sys_inotify_add_watch(uint64_t fd, uint64_t path_ptr, uint64_t mask)
{
	task_t *cur = sched_current();
	uint32_t m32 = (uint32_t)mask;
	uint32_t events = m32 & IN_WATCHABLE;
	int follow = !(m32 & IN_DONT_FOLLOW);
	char kpath[VFS_MAX_PATH];
	struct kstat st;
	struct fsnotify_ident id;
	int err = 0;
	uint64_t fl;

	if (!cur)
		return -EFAULT;
	if (!events)
		return -EINVAL;
	if ((m32 & IN_MASK_ADD) && (m32 & IN_MASK_CREATE))
		return -EINVAL;
	if (!validate_user_ptr(path_ptr, 1))
		return -EFAULT;
	int cret = copy_user_path((const char *)path_ptr, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	vfs_file_t *f = inotify_fdget(cur, fd, &err);
	if (!f)
		return err;
	struct inotify_ctx *c = device_file_priv(f);

	/* The same checks stat() makes (search permission on the way), then
	 * read permission on the object: a watch reveals what happens to it. */
	int st_rc = follow ? vfs_stat(kpath, &st) : vfs_lstat(kpath, &st);
	if (st_rc != ST_OK) {
		fdput(f);
		return vfs_status_to_errno(st_rc);
	}
	if ((m32 & IN_ONLYDIR) && !S_ISDIR(st.st_mode)) {
		fdput(f);
		return -ENOTDIR;
	}
	if (!S_ISLNK(st.st_mode)) {
		int pr = vfs_permission(kpath, MAY_READ);
		if (pr != ST_OK) {
			fdput(f);
			return vfs_status_to_errno(pr);
		}
	}
	if (vfs_notify_ident(kpath, follow, &id) != ST_OK || !id.valid) {
		fdput(f);
		return -ENOENT;
	}

	/* Allocated up front: nothing is allocated under the table lock. */
	struct fsn_mark *nm = kalloc(sizeof(*nm));
	struct inotify_watch *nw = kalloc(sizeof(*nw));
	if (!nm || !nw) {
		kfree(nm);
		kfree(nw);
		fdput(f);
		return -ENOMEM;
	}
	mm_memset(nm, 0, sizeof(*nm));
	mm_memset(nw, 0, sizeof(*nw));

	uint32_t want = events | (m32 & (IN_ONESHOT | IN_EXCL_UNLINK));
	int64_t ret;

	spin_lock_irqsave(&g_lock, &fl);
	struct fsn_mark *mk = mark_by_ino(id.fs, id.ino);
	struct inotify_watch *w = NULL;
	if (mk)
		for (w = mk->watches; w && w->ctx != c; w = w->mark_next)
			;
	if (w) {
		/* This instance already watches the object: change the
		 * mask, keep the descriptor. */
		if (m32 & IN_MASK_CREATE) {
			ret = -EEXIST;
		} else {
			uint32_t nmask = (m32 & IN_MASK_ADD) ? (w->mask | want) :
							       want;
			account(w->mask, nmask);
			w->mask = nmask;
			ret = w->wd;
		}
	} else if (c->nwatches >= INOTIFY_MAX_WATCHES ||
		   g_nwatches >= INOTIFY_MAX_WATCHES_ALL) {
		ret = -ENOSPC;
	} else {
		if (!mk) {
			mk = nm;
			nm = NULL;
			mk->fs = id.fs;
			mk->ino = id.ino;
			mk->isdir = id.isdir;
			mm_memcpy(mk->path, kpath, str_len(kpath) + 1);
			unsigned b = ino_bucket(mk->fs, mk->ino);
			mk->ino_next = g_by_ino[b];
			g_by_ino[b] = mk;
			if (mk->isdir)
				path_table_add(mk);
		}
		w = nw;
		nw = NULL;
		w->ctx = c;
		w->mark = mk;
		w->mask = want;
		w->wd = c->next_wd++;
		if (c->next_wd <= 0)
			c->next_wd = 1;
		w->mark_next = mk->watches;
		mk->watches = w;
		w->ctx_next = c->watches;
		c->watches = w;
		c->nwatches++;
		g_nwatches++;
		account(0, want);
		ret = w->wd;
	}
	spin_unlock_irqrestore(&g_lock, fl);
	kfree(nm);
	kfree(nw);
	fdput(f);
	return ret;
}

int64_t sys_inotify_rm_watch(uint64_t fd, uint64_t wd)
{
	task_t *cur = sched_current();
	struct inotify_watch *w;
	int err = 0;
	uint64_t fl;

	if (!cur)
		return -EFAULT;
	vfs_file_t *f = inotify_fdget(cur, fd, &err);
	if (!f)
		return err;
	struct inotify_ctx *c = device_file_priv(f);

	spin_lock_irqsave(&g_lock, &fl);
	for (w = c->watches; w && w->wd != (int)wd; w = w->ctx_next)
		;
	if (w)
		watch_detach(w);
	spin_unlock_irqrestore(&g_lock, fl);
	if (!w) {
		fdput(f);
		return -EINVAL;
	}
	queue_event(c, (int)wd, IN_IGNORED, 0, NULL);
	kfree(w);
	fdput(f);
	return 0;
}
