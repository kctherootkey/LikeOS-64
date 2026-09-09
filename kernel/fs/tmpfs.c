// LikeOS-64 -- tmpfs: a filesystem kept entirely in RAM, mounted at /ram.
/*
 * Why it exists: to take the disk out of an experiment.  luakit on this
 * system boots from a USB stick through ext4, and on every page it loads it
 * writes -- WebKit's HTTP cache, the cookie and HSTS databases, luakit's own
 * history, session and bookmark files -- and it fsyncs.  Whether that traffic
 * is what makes it slow can only be settled by running it with the same
 * files somewhere that costs nothing: here.  The launcher in
 * res/xorg/luakit-ram.sh points luakit's XDG directories below /ram.
 *
 * What it is: names (dentries) pointing at objects (inodes) -- directories,
 * regular files, symlinks, and the data-less socket/FIFO nodes -- with
 * regular-file contents held in 4 KB frames from the physical allocator.
 * Nothing is ever written anywhere else, nothing survives a reboot, and
 * fsync() is a no-op because there is nowhere to flush to.
 *
 * Names and objects are separate because of hard links: WebKit's HTTP cache
 * stores every response body once, under its hash in Blobs/, and gives each
 * record that uses it a second name with link(2).  A regular file therefore
 * has `nlink' names and lives until the last of them is removed AND the last
 * open handle is closed.  A directory has exactly one name (no links to
 * directories, as everywhere), kept in inode->self so ".." and readdir can
 * find their way back.
 *
 * What it is not: it is NOT the page cache.  A page-cache page is a copy of
 * something on disk and may be dropped and re-read at any time; a tmpfs
 * frame IS the file, so it stays until the file is truncated or deleted.
 * That is also why it is capped (TFS_RAM_FRACTION): every frame it holds is
 * one the rest of the system cannot reclaim, and a program filling it up
 * must get ENOSPC rather than take the machine down.
 *
 * Interface: the path-based vfs_ops_t every other filesystem here speaks,
 * reached through the VFS mount table (vfs_register_mount), so the paths
 * arriving are absolute and canonical -- no "." or "..", no double slashes
 * (namei's normalize_path guarantees it) -- and always start with the mount
 * prefix.  Permissions are decided by the VFS from stat(), like everywhere
 * else; this file only keeps the mode/uid/gid it is told to.
 *
 * Locking: ONE reentrant sleeping mutex for the whole tree (tfs_lock).  Every
 * operation is a walk plus a small change, and the copies to and from user
 * memory are memcpy-fast, so contention is not where time goes.  It sleeps
 * rather than spins because a user buffer may fault during a copy and the
 * fault may go to disk; it is reentrant because such a fault, if it lands
 * in a mapping of a file that lives here, comes back through read_at() on
 * the same task.  Pages are allocated under it, which is fine for a mutex
 * and would not be for a spinlock.
 */
#include <kernel/fs/tmpfs.h>
#include <kernel/fs/vfs.h>
#include <kernel/uapi/status.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- Configuration ---------------------------------------------------- */

/* At most 1/N of physical memory may be held in file frames. */
#define TFS_RAM_FRACTION 4
/* ...but never less than this many frames, so a small machine still has a
 * usable /ram (16 MB). */
#define TFS_MIN_CAP_PAGES 4096
/* Largest single file: 1 GB.  Keeps every size computation far from
 * overflowing, and nothing that belongs in RAM is bigger. */
#define TFS_MAX_FILE_SIZE (1UL << 30)
/* Symlink chains longer than this are refused (ELOOP territory). */
#define TFS_LINK_HOPS 8

/* What stat() and statfs() report.  TFS_MAGIC is TMPFS_MAGIC, so anything
 * that recognises a RAM filesystem by its statfs type recognises this one.
 * TFS_DEV is st_dev: different from ext4's, so a record lock keyed by
 * (st_dev, st_ino) on a file here can never collide with one on disk. */
#define TFS_MAGIC 0x01021994UL
#define TFS_DEV 0x746d7066UL /* "tmpf" */

#define TFS_MOUNT_LEN (sizeof(TMPFS_MOUNT) - 1)

/* ---- Structures --------------------------------------------------------- */

enum tfs_type { TFS_REG, TFS_DIR, TFS_LNK, TFS_SOCK, TFS_FIFO };

struct tfs_dentry;

/* The object: what stat() describes and what a handle holds. */
struct tfs_inode {
	enum tfs_type type;
	uint32_t mode; /* permission bits (07777)                       */
	uint32_t uid, gid;
	uint64_t ino;
	uint64_t atime, mtime, ctime; /* seconds since the epoch       */
	uint32_t nlink; /* names pointing here (a directory has one)    */
	int refs; /* open handles                                         */
	/* directories */
	struct tfs_dentry *children;
	uint32_t nchildren;
	struct tfs_dentry *self; /* its one name; NULL once removed     */
	/* regular files.  Frames are dense from 0 to npages-1; the bytes of
	 * the last frame beyond `size' are ALWAYS zero (kept so on every
	 * shrink), which is what makes a later extension read as zeros. */
	unsigned long size;
	uint64_t *pages; /* physical frame addresses                    */
	unsigned long npages; /* frames held                              */
	unsigned long pcap; /* entries the array can hold                 */
	/* symlinks */
	char *target;
};

/* A name in a directory. */
struct tfs_dentry {
	char *name; /* kalloc'd; "" for the root                     */
	struct tfs_inode *inode;
	struct tfs_dentry *parent; /* directory holding this name        */
	struct tfs_dentry *sibling; /* next name in that directory       */
};

typedef struct {
	vfs_file_t vfs;
	struct tfs_inode *inode;
	long pos; /* regular files: byte position                     */
	unsigned long dirpos; /* directories: entries already returned   */
} tfs_file_t;

/* ---- State --------------------------------------------------------------- */

static struct tfs_inode g_root_inode;
static struct tfs_dentry g_root;
static uint64_t g_next_ino = 1;
static unsigned long g_cap_pages; /* frames /ram may hold in total     */
static unsigned long g_used_pages; /* frames it holds now              */
static unsigned long g_inodes; /* objects alive (for statfs)           */
static vfs_ops_t g_tfs_ops;

/* ---- The lock ------------------------------------------------------------ */

static struct {
	spinlock_t lock; /* protects the three fields below           */
	volatile int held;
	volatile uint64_t owner; /* task id of the holder               */
	volatile int depth; /* reentrant holds by the owner              */
} g_mtx;

static void tfs_lock(void)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? (uint64_t)cur->id : (uint64_t)-2;

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&g_mtx.lock, &fl);
		if (!g_mtx.held) {
			g_mtx.held = 1;
			g_mtx.owner = me;
			g_mtx.depth = 1;
			spin_unlock_irqrestore(&g_mtx.lock, fl);
			return;
		}
		if (g_mtx.owner == me) {
			g_mtx.depth++;
			spin_unlock_irqrestore(&g_mtx.lock, fl);
			return;
		}
		/* Park on the mutex as ext4's rwsem does: BLOCKED is set under
		 * the spinlock, so a release that happens after this unlock
		 * finds the task on the channel and wakes it. */
		if (cur && irqs_enabled()) {
			cur->wait_channel = (void *)&g_mtx;
			cur->state = TASK_BLOCKED;
			spin_unlock_irqrestore(&g_mtx.lock, fl);
			sched_schedule();
		} else {
			spin_unlock_irqrestore(&g_mtx.lock, fl);
			__asm__ volatile("pause");
		}
	}
}

static void tfs_unlock(void)
{
	uint64_t fl;
	int released = 0;

	spin_lock_irqsave(&g_mtx.lock, &fl);
	if (g_mtx.held && --g_mtx.depth == 0) {
		g_mtx.held = 0;
		g_mtx.owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&g_mtx.lock, fl);
	if (released)
		sched_wake_channel((void *)&g_mtx);
}

/* The reaper's hook: a task that died inside an operation must not keep
 * the tree locked forever. */
static int tfs_release_locks_for_task(uint64_t task_id)
{
	uint64_t fl;
	int released = 0;

	spin_lock_irqsave(&g_mtx.lock, &fl);
	if (g_mtx.held && g_mtx.owner == task_id) {
		g_mtx.held = 0;
		g_mtx.depth = 0;
		g_mtx.owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&g_mtx.lock, fl);
	if (released)
		sched_wake_channel((void *)&g_mtx);
	return ST_OK;
}

/* ---- Small helpers ------------------------------------------------------- */

static char *tfs_strdup(const char *s)
{
	size_t n = kstrlen(s);
	char *d = kalloc(n + 1);

	if (!d)
		return NULL;
	mm_memcpy(d, s, n + 1);
	return d;
}

static int tfs_name_eq(const char *name, const char *s, size_t len)
{
	size_t k = 0;

	while (k < len && name[k] == s[k])
		k++;
	return k == len && name[k] == 0;
}

static void tfs_touch_ctime(struct tfs_inode *n)
{
	n->ctime = timer_get_epoch();
}

static void tfs_touch_mtime(struct tfs_inode *n)
{
	n->mtime = n->ctime = timer_get_epoch();
}

/* ---- Frames --------------------------------------------------------------- */

/* Make the file hold at least `want' frames; new ones are zero.  Fails with
 * ST_NOSPC against the cap, ST_NOMEM against the allocator.  Frames added
 * before a failure stay: they are zero and accounted for. */
static int tfs_grow_pages(struct tfs_inode *n, unsigned long want)
{
	if (want <= n->npages)
		return ST_OK;
	if (want - n->npages > g_cap_pages - g_used_pages)
		return ST_NOSPC;
	if (want > n->pcap) {
		unsigned long ncap = n->pcap ? n->pcap : 8;

		while (ncap < want)
			ncap *= 2;
		uint64_t *np = kalloc(ncap * sizeof(uint64_t));

		if (!np)
			return ST_NOMEM;
		if (n->npages)
			mm_memcpy(np, n->pages, n->npages * sizeof(uint64_t));
		if (n->pages)
			kfree(n->pages);
		n->pages = np;
		n->pcap = ncap;
	}
	while (n->npages < want) {
		uint64_t p = mm_allocate_physical_page();

		if (!p)
			return ST_NOMEM;
		/* The allocator hands out zeroed frames in production, but a
		 * DEBUG build poisons them, and a file's new bytes must read
		 * as zero either way. */
		mm_memset(phys_to_virt(p), 0, PAGE_SIZE);
		n->pages[n->npages++] = p;
		g_used_pages++;
	}
	return ST_OK;
}

/* Drop frames beyond `keep'. */
static void tfs_shrink_pages(struct tfs_inode *n, unsigned long keep)
{
	while (n->npages > keep) {
		n->npages--;
		mm_free_physical_page(n->pages[n->npages]);
		g_used_pages--;
	}
	if (keep == 0 && n->pages) {
		kfree(n->pages);
		n->pages = NULL;
		n->pcap = 0;
	}
}

/* Set the byte size, keeping the "zero beyond size" invariant. */
static int tfs_set_size(struct tfs_inode *n, unsigned long size)
{
	unsigned long want = (size + PAGE_SIZE - 1) / PAGE_SIZE;

	if (size > TFS_MAX_FILE_SIZE)
		return ST_NOSPC;
	if (size < n->size) {
		tfs_shrink_pages(n, want);
		/* Zero the tail of the (new) last frame past `size'. */
		if (size & (PAGE_SIZE - 1)) {
			unsigned long off = size & (PAGE_SIZE - 1);

			mm_memset((char *)phys_to_virt(n->pages[want - 1]) + off,
				  0, PAGE_SIZE - off);
		}
	} else {
		int rc = tfs_grow_pages(n, want);

		if (rc != ST_OK)
			return rc;
	}
	n->size = size;
	return ST_OK;
}

/* ---- Inodes and names ----------------------------------------------------- */

static struct tfs_inode *tfs_inode_new(enum tfs_type type, uint32_t mode)
{
	struct tfs_inode *n = kalloc(sizeof(*n));

	if (!n)
		return NULL;
	mm_memset(n, 0, sizeof(*n));
	n->type = type;
	n->mode = mode & 07777;
	task_t *cur = sched_current();

	if (cur) {
		n->uid = cur->cred.fsuid;
		n->gid = cur->cred.fsgid;
	}
	n->ino = ++g_next_ino;
	n->atime = n->mtime = n->ctime = timer_get_epoch();
	g_inodes++;
	return n;
}

static void tfs_inode_free(struct tfs_inode *n)
{
	tfs_shrink_pages(n, 0);
	if (n->target)
		kfree(n->target);
	g_inodes--;
	kfree(n);
}

/* An object goes when it has no name left and nobody holds it open. */
static void tfs_inode_maybe_free(struct tfs_inode *n)
{
	if (n->nlink == 0 && n->refs == 0 && n != &g_root_inode)
		tfs_inode_free(n);
}

/* Put an existing name record at the end of `dir', so a listing keeps
 * creation order. */
static void tfs_dentry_attach(struct tfs_dentry *dir, struct tfs_dentry *d)
{
	d->parent = dir;
	d->sibling = NULL;
	if (!dir->inode->children) {
		dir->inode->children = d;
	} else {
		struct tfs_dentry *t = dir->inode->children;

		while (t->sibling)
			t = t->sibling;
		t->sibling = d;
	}
	dir->inode->nchildren++;
	tfs_touch_mtime(dir->inode);
}

/* A new name for `inode' in `dir'.  Takes the link. */
static struct tfs_dentry *tfs_dentry_add(struct tfs_dentry *dir,
					 const char *name,
					 struct tfs_inode *inode)
{
	struct tfs_dentry *d = kalloc(sizeof(*d));

	if (!d)
		return NULL;
	mm_memset(d, 0, sizeof(*d));
	d->name = tfs_strdup(name);
	if (!d->name) {
		kfree(d);
		return NULL;
	}
	d->inode = inode;
	inode->nlink++;
	if (inode->type == TFS_DIR)
		inode->self = d;
	tfs_dentry_attach(dir, d);
	return d;
}

/* Take a name out of its directory without freeing anything. */
static void tfs_dentry_detach(struct tfs_dentry *d)
{
	struct tfs_dentry *dir = d->parent;
	struct tfs_dentry **pp = &dir->inode->children;

	while (*pp && *pp != d)
		pp = &(*pp)->sibling;
	if (*pp) {
		*pp = d->sibling;
		dir->inode->nchildren--;
	}
	d->parent = NULL;
	d->sibling = NULL;
	tfs_touch_mtime(dir->inode);
}

/* Remove a name for good: the object loses a link and goes if that was the
 * last one and nothing holds it open. */
static void tfs_dentry_remove(struct tfs_dentry *d)
{
	struct tfs_inode *n = d->inode;

	tfs_dentry_detach(d);
	if (n->type == TFS_DIR)
		n->self = NULL;
	n->nlink--;
	tfs_touch_ctime(n);
	kfree(d->name);
	kfree(d);
	tfs_inode_maybe_free(n);
}

static struct tfs_dentry *tfs_find_child(struct tfs_dentry *dir, const char *s,
					 size_t len)
{
	for (struct tfs_dentry *c = dir->inode->children; c; c = c->sibling)
		if (tfs_name_eq(c->name, s, len))
			return c;
	return NULL;
}

/* ---- Path resolution ----------------------------------------------------- */

/* The part of an absolute path below the mount point, without its leading
 * slash: "/ram/a/b" -> "a/b", "/ram" -> "".  NULL if not ours. */
static const char *tfs_rel(const char *path)
{
	size_t k = 0;

	if (!path)
		return NULL;
	while (k < TFS_MOUNT_LEN && path[k] == TMPFS_MOUNT[k])
		k++;
	if (k != TFS_MOUNT_LEN)
		return NULL;
	if (path[k] == 0)
		return path + k;
	if (path[k] != '/')
		return NULL;
	while (path[k] == '/')
		k++;
	return path + k;
}

/* Resolve `p' relative to `base' to a name.  `follow' says whether a
 * symlink in the FINAL component is followed (stat/open: yes; lstat/unlink/
 * rename/link: no); one in the middle always is.  A link whose target
 * leaves /ram cannot be followed from here and reads as missing. */
static struct tfs_dentry *tfs_resolve(struct tfs_dentry *base, const char *p,
				      int follow, int hops)
{
	struct tfs_dentry *cur = base;

	if (hops > TFS_LINK_HOPS)
		return NULL;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *s = p;
		size_t len = 0;

		while (p[len] && p[len] != '/')
			len++;
		p += len;
		int last = 1;

		for (const char *q = p; *q; q++)
			if (*q != '/') {
				last = 0;
				break;
			}
		if (cur->inode->type != TFS_DIR)
			return NULL;
		struct tfs_dentry *next;

		if (len == 1 && s[0] == '.') {
			next = cur;
		} else if (len == 2 && s[0] == '.' && s[1] == '.') {
			next = cur->parent ? cur->parent : cur;
		} else {
			next = tfs_find_child(cur, s, len);
			if (!next)
				return NULL;
		}
		if (next->inode->type == TFS_LNK && (!last || follow)) {
			const char *t = next->inode->target;

			if (t[0] == '/') {
				const char *r = tfs_rel(t);

				if (!r)
					return NULL; /* leaves /ram */
				next = tfs_resolve(&g_root, r, 1, hops + 1);
			} else {
				next = tfs_resolve(cur, t, 1, hops + 1);
			}
			if (!next)
				return NULL;
		}
		cur = next;
	}
	return cur;
}

static struct tfs_dentry *tfs_lookup(const char *path, int follow)
{
	const char *rel = tfs_rel(path);

	if (!rel)
		return NULL;
	return tfs_resolve(&g_root, rel, follow, 0);
}

/* Split `path' into its parent directory (resolved, symlinks followed) and
 * its final name, copied to `name' (NUL-terminated).  The name must be a
 * real one: not empty, not "." or "..", not longer than VFS_NAME_MAX. */
static int tfs_split(const char *path, struct tfs_dentry **dir_out, char *name,
		     size_t namecap)
{
	const char *rel = tfs_rel(path);

	if (!rel || !*rel)
		return ST_INVALID; /* the root has no parent here */
	size_t end = kstrlen(rel);

	while (end > 0 && rel[end - 1] == '/')
		end--;
	if (end == 0)
		return ST_INVALID;
	size_t start = end;

	while (start > 0 && rel[start - 1] != '/')
		start--;
	size_t len = end - start;

	if (len == 0 || len > VFS_NAME_MAX || len >= namecap)
		return ST_INVALID;
	if ((len == 1 && rel[start] == '.') ||
	    (len == 2 && rel[start] == '.' && rel[start + 1] == '.'))
		return ST_INVALID;
	mm_memcpy(name, rel + start, len);
	name[len] = 0;

	struct tfs_dentry *dir;

	if (start == 0) {
		dir = &g_root;
	} else {
		char prefix[VFS_MAX_PATH];

		if (start >= sizeof(prefix))
			return ST_INVALID;
		mm_memcpy(prefix, rel, start);
		prefix[start] = 0;
		dir = tfs_resolve(&g_root, prefix, 1, 0);
		if (!dir || dir->inode->type != TFS_DIR)
			return ST_NOT_FOUND;
	}
	*dir_out = dir;
	return ST_OK;
}

/* ---- stat ----------------------------------------------------------------- */

static void tfs_fill_stat(const struct tfs_inode *n, struct kstat *st)
{
	mm_memset(st, 0, sizeof(*st));
	st->st_dev = TFS_DEV;
	st->st_ino = n->ino;
	st->st_uid = n->uid;
	st->st_gid = n->gid;
	st->st_nlink = n->nlink;
	st->st_blksize = PAGE_SIZE;
	st->st_atime = n->atime;
	st->st_mtime = n->mtime;
	st->st_ctime = n->ctime;
	switch (n->type) {
	case TFS_DIR: {
		/* The customary count: its own name, ".", and every
		 * subdirectory's "..". */
		uint32_t links = 2;

		for (const struct tfs_dentry *c = n->children; c; c = c->sibling)
			if (c->inode->type == TFS_DIR)
				links++;
		st->st_mode = S_IFDIR | n->mode;
		st->st_nlink = links;
		st->st_size = PAGE_SIZE;
		break;
	}
	case TFS_LNK:
		st->st_mode = S_IFLNK | 0777;
		st->st_size = kstrlen(n->target);
		break;
	case TFS_SOCK:
		st->st_mode = S_IFSOCK | n->mode;
		break;
	case TFS_FIFO:
		st->st_mode = S_IFIFO | n->mode;
		break;
	default:
		st->st_mode = S_IFREG | n->mode;
		st->st_size = n->size;
		st->st_blocks = n->npages * (PAGE_SIZE / 512);
		break;
	}
}

static int tfs_stat_common(const char *path, struct kstat *st, int follow)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, follow);

	if (!d) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	tfs_fill_stat(d->inode, st);
	tfs_unlock();
	return ST_OK;
}

static int tfs_stat(const char *path, struct kstat *st)
{
	return tfs_stat_common(path, st, 1);
}

static int tfs_lstat(const char *path, struct kstat *st)
{
	return tfs_stat_common(path, st, 0);
}

static int tfs_fstat(vfs_file_t *f, struct kstat *st)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	tfs_lock();
	tfs_fill_stat(tf->inode, st);
	tfs_unlock();
	return ST_OK;
}

/* ---- open / close ---------------------------------------------------------- */

static int tfs_open_mode(const char *path, int flags, unsigned int mode,
			 vfs_file_t **out)
{
	int acc = flags & 3;
	int writing = (acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC));

	if (!tfs_rel(path))
		return ST_NOT_FOUND;
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 1);
	struct tfs_inode *n;

	if (!d) {
		/* O_EXCL on an existing name was already refused by the
		 * VFS; here a missing name is created when asked. */
		if (!(flags & O_CREAT)) {
			tfs_unlock();
			return ST_NOT_FOUND;
		}
		struct tfs_dentry *dir;
		char name[VFS_NAME_MAX + 1];
		int rc = tfs_split(path, &dir, name, sizeof(name));

		if (rc != ST_OK) {
			tfs_unlock();
			return rc;
		}
		/* The lookup followed symlinks; a name that exists but leads
		 * nowhere (a dangling link) is not one to create under. */
		if (tfs_find_child(dir, name, kstrlen(name))) {
			tfs_unlock();
			return ST_NOT_FOUND;
		}
		n = tfs_inode_new(TFS_REG, mode);
		if (!n) {
			tfs_unlock();
			return ST_NOMEM;
		}
		if (!tfs_dentry_add(dir, name, n)) {
			tfs_inode_free(n);
			tfs_unlock();
			return ST_NOMEM;
		}
	} else {
		n = d->inode;
		if (n->type == TFS_DIR) {
			if (writing) {
				tfs_unlock();
				return ST_INVALID; /* EISDIR has no ST_ code */
			}
		} else if (n->type != TFS_REG) {
			/* A socket or FIFO node is a name, not something to
			 * read. */
			tfs_unlock();
			return ST_UNSUPPORTED;
		}
	}

	tfs_file_t *tf = kalloc(sizeof(*tf));

	if (!tf) {
		tfs_unlock();
		return ST_NOMEM;
	}
	mm_memset(tf, 0, sizeof(*tf));
	tf->vfs.ops = &g_tfs_ops;
	tf->vfs.fs_private = tf;
	tf->inode = n;
	n->refs++;
	if (n->type == TFS_REG) {
		if ((flags & O_TRUNC) && n->size) {
			tfs_set_size(n, 0); /* shrinking cannot fail */
			tfs_touch_mtime(n);
		}
		tf->pos = (flags & O_APPEND) ? (long)n->size : 0;
	}
	tfs_unlock();
	*out = &tf->vfs;
	return ST_OK;
}

/* Plain open: in-kernel openers, which never create here.  0644 is what a
 * creation through this path gets, the same default ext4 applies. */
static int tfs_open(const char *path, int flags, vfs_file_t **out)
{
	return tfs_open_mode(path, flags, 0644, out);
}

static int tfs_close(vfs_file_t *f)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_OK;
	tfs_lock();
	struct tfs_inode *n = tf->inode;

	n->refs--;
	tfs_inode_maybe_free(n);
	tfs_unlock();
	kfree(tf);
	return ST_OK;
}

/* ---- read / write / seek ---------------------------------------------------- */

/* Copy [off, off+bytes) of the file to the user buffer.  Called with the
 * lock held; returns bytes copied (0 at or past EOF). */
static long tfs_copy_out(struct tfs_inode *n, void *buf, long bytes,
			 unsigned long off)
{
	if (off >= n->size)
		return 0;
	unsigned long end = off + (unsigned long)bytes;

	if (end > n->size)
		end = n->size;
	unsigned long done = 0;

	while (off + done < end) {
		unsigned long pos = off + done;
		unsigned long in = pos & (PAGE_SIZE - 1);
		unsigned long chunk = PAGE_SIZE - in;

		if (chunk > end - pos)
			chunk = end - pos;
		const char *src =
			(const char *)phys_to_virt(n->pages[pos / PAGE_SIZE]) +
			in;
		smap_disable();
		mm_memcpy((char *)buf + done, src, chunk);
		smap_enable();
		done += chunk;
	}
	return (long)done;
}

static long tfs_read(vfs_file_t *f, void *buf, long bytes)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return -EINVAL;
	if (bytes < 0)
		return -EINVAL;
	if (tf->inode->type == TFS_DIR)
		return -EISDIR;
	if (bytes == 0)
		return 0;
	tfs_lock();
	long n = tfs_copy_out(tf->inode, buf, bytes, (unsigned long)tf->pos);

	if (n > 0)
		tf->pos += n;
	tfs_unlock();
	return n;
}

/* Positional read for demand paging: does not move the handle's position,
 * so a page-in never disturbs the reader sharing the descriptor. */
static long tfs_read_at(vfs_file_t *f, void *buf, long bytes, long off)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf || off < 0 || bytes < 0)
		return -EINVAL;
	if (tf->inode->type == TFS_DIR)
		return -EISDIR;
	if (bytes == 0)
		return 0;
	tfs_lock();
	long n = tfs_copy_out(tf->inode, buf, bytes, (unsigned long)off);

	tfs_unlock();
	return n;
}

static long tfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return -EINVAL;
	if (bytes < 0)
		return -EINVAL;
	if (tf->inode->type != TFS_REG)
		return -EISDIR;
	if (bytes == 0)
		return 0;
	tfs_lock();
	struct tfs_inode *n = tf->inode;

	if (f->flags & O_APPEND)
		tf->pos = (long)n->size;
	unsigned long off = (unsigned long)tf->pos;
	unsigned long end = off + (unsigned long)bytes;

	if (end < off || end > TFS_MAX_FILE_SIZE) {
		tfs_unlock();
		return -ENOSPC; /* no EFBIG in this kernel */
	}
	/* Frames for the whole write first, so it is all or nothing: a
	 * write that half-lands and then fails is worse than one refused. */
	int rc = tfs_grow_pages(n, (end + PAGE_SIZE - 1) / PAGE_SIZE);

	if (rc != ST_OK) {
		tfs_unlock();
		return rc == ST_NOSPC ? -ENOSPC : -ENOMEM;
	}
	unsigned long done = 0;

	while (off + done < end) {
		unsigned long pos = off + done;
		unsigned long in = pos & (PAGE_SIZE - 1);
		unsigned long chunk = PAGE_SIZE - in;

		if (chunk > end - pos)
			chunk = end - pos;
		char *dst = (char *)phys_to_virt(n->pages[pos / PAGE_SIZE]) + in;

		smap_disable();
		mm_memcpy(dst, (const char *)buf + done, chunk);
		smap_enable();
		done += chunk;
	}
	if (end > n->size)
		n->size = end;
	tf->pos = (long)end;
	tfs_touch_mtime(n);
	tfs_unlock();
	return bytes;
}

static long tfs_seek(vfs_file_t *f, long offset, int whence)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return -EINVAL;
	tfs_lock();
	if (tf->inode->type == TFS_DIR) {
		/* rewinddir(): only a rewind means anything for a listing. */
		if (whence == SEEK_SET && offset == 0)
			tf->dirpos = 0;
		tfs_unlock();
		return 0;
	}
	long base;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = tf->pos;
		break;
	case SEEK_END:
		base = (long)tf->inode->size;
		break;
	default:
		tfs_unlock();
		return -EINVAL;
	}
	long np = base + offset;

	if (np < 0) {
		tfs_unlock();
		return -EINVAL;
	}
	tf->pos = np;
	tfs_unlock();
	return np;
}

static int tfs_truncate(vfs_file_t *f, unsigned long size)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	if (tf->inode->type != TFS_REG)
		return ST_INVALID;
	tfs_lock();
	int rc = tfs_set_size(tf->inode, size);

	if (rc == ST_OK)
		tfs_touch_mtime(tf->inode);
	tfs_unlock();
	return rc;
}

/* ---- readdir ---------------------------------------------------------------- */

/* One record, laid out exactly as ext4's: the name at byte 19, the record
 * 19 + name + NUL rounded up to 8.  Written to USER memory. */
static unsigned tfs_put_dirent(char *buf, unsigned cap, unsigned off,
			       const char *name, uint64_t ino, int type)
{
	unsigned nl = (unsigned)kstrlen(name);
	unsigned reclen = (19 + nl + 1 + 7) & ~7u;

	if (off + reclen > cap)
		return 0;
	struct dirent64 *d = (struct dirent64 *)(buf + off);

	smap_disable();
	d->d_ino = ino;
	d->d_off = off + reclen;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = (uint8_t)type;
	mm_memcpy(d->d_name, name, nl + 1);
	smap_enable();
	return reclen;
}

static int tfs_dt(const struct tfs_inode *n)
{
	switch (n->type) {
	case TFS_DIR:
		return DT_DIR;
	case TFS_LNK:
		return DT_LNK;
	case TFS_SOCK:
		return DT_SOCK;
	case TFS_FIFO:
		return DT_FIFO;
	default:
		return DT_REG;
	}
}

/* Entries are numbered: 0 ".", 1 "..", then the children in list order.
 * Each call continues where the last one stopped and returns 0 when
 * everything has been delivered, which is how getdents() knows to stop. */
static long tfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf || tf->inode->type != TFS_DIR)
		return -ENOTDIR;
	if (bytes < 0)
		return -EINVAL;
	tfs_lock();
	struct tfs_inode *dir = tf->inode;
	unsigned off = 0;
	unsigned long idx = 0;
	unsigned rl;

	if (tf->dirpos == 0) {
		rl = tfs_put_dirent(buf, (unsigned)bytes, off, ".", dir->ino,
				    DT_DIR);
		if (!rl) {
			tfs_unlock();
			return -EINVAL; /* buffer cannot hold one record */
		}
		off += rl;
		tf->dirpos = 1;
	}
	if (tf->dirpos == 1) {
		/* ".." of a directory whose name is gone, or of the root, is
		 * itself. */
		uint64_t pino = dir->ino;

		if (dir->self && dir->self->parent)
			pino = dir->self->parent->inode->ino;
		rl = tfs_put_dirent(buf, (unsigned)bytes, off, "..", pino,
				    DT_DIR);
		if (!rl)
			goto out;
		off += rl;
		tf->dirpos = 2;
	}
	idx = 2;
	for (struct tfs_dentry *c = dir->children; c; c = c->sibling, idx++) {
		if (idx < tf->dirpos)
			continue;
		rl = tfs_put_dirent(buf, (unsigned)bytes, off, c->name,
				    c->inode->ino, tfs_dt(c->inode));
		if (!rl)
			break;
		off += rl;
		tf->dirpos = idx + 1;
	}
out:
	tfs_unlock();
	return (long)off;
}

/* ---- namespace changes -------------------------------------------------------- */

/* Create a fresh object of `type' under a new name.  Shared by mkdir, mknod
 * and symlink; open() creates its own because it also needs the handle. */
static int tfs_create(const char *path, enum tfs_type type, unsigned int mode,
		      const char *target)
{
	struct tfs_dentry *dir;
	char name[VFS_NAME_MAX + 1];
	int rc = tfs_split(path, &dir, name, sizeof(name));

	if (rc != ST_OK)
		return rc;
	if (tfs_find_child(dir, name, kstrlen(name)))
		return ST_EXISTS;
	struct tfs_inode *n = tfs_inode_new(type, mode);

	if (!n)
		return ST_NOMEM;
	if (target) {
		n->target = tfs_strdup(target);
		if (!n->target) {
			tfs_inode_free(n);
			return ST_NOMEM;
		}
	}
	if (!tfs_dentry_add(dir, name, n)) {
		tfs_inode_free(n);
		return ST_NOMEM;
	}
	return ST_OK;
}

static int tfs_mkdir(const char *path, unsigned int mode)
{
	tfs_lock();
	int rc = tfs_create(path, TFS_DIR, mode, NULL);

	tfs_unlock();
	return rc;
}

static int tfs_mknod(const char *path, unsigned int mode)
{
	enum tfs_type t;

	switch (mode & S_IFMT) {
	case S_IFSOCK:
		t = TFS_SOCK;
		break;
	case S_IFIFO:
		t = TFS_FIFO;
		break;
	case S_IFREG:
	case 0:
		t = TFS_REG;
		break;
	default:
		return ST_UNSUPPORTED;
	}
	tfs_lock();
	int rc = tfs_create(path, t, mode, NULL);

	tfs_unlock();
	return rc;
}

static int tfs_symlink(const char *target, const char *linkpath)
{
	if (!target || !*target)
		return ST_INVALID;
	tfs_lock();
	int rc = tfs_create(linkpath, TFS_LNK, 0777, target);

	tfs_unlock();
	return rc;
}

static int tfs_readlink(const char *path, char *buf, unsigned long bufsz)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 0);

	if (!d) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_LNK) {
		tfs_unlock();
		return ST_INVALID;
	}
	unsigned long k = 0;

	while (d->inode->target[k] && k < bufsz) {
		buf[k] = d->inode->target[k];
		k++;
	}
	tfs_unlock();
	return (int)k;
}

/* A second name for an existing object.  Not for directories (a loop in
 * the tree is not worth the trouble, and no filesystem allows it); the
 * name itself is what is linked when `oldpath' is a symlink. */
static int tfs_link(const char *oldpath, const char *newpath)
{
	struct tfs_dentry *dir;
	char name[VFS_NAME_MAX + 1];

	/* The VFS routes by the new name; the old one has to be here too,
	 * since two names of one object cannot straddle filesystems. */
	if (!tfs_rel(oldpath))
		return ST_XDEV;
	tfs_lock();
	struct tfs_dentry *src = tfs_lookup(oldpath, 0);

	if (!src) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (src->inode->type == TFS_DIR) {
		tfs_unlock();
		return ST_PERM;
	}
	int rc = tfs_split(newpath, &dir, name, sizeof(name));

	if (rc != ST_OK) {
		tfs_unlock();
		return rc;
	}
	if (tfs_find_child(dir, name, kstrlen(name))) {
		tfs_unlock();
		return ST_EXISTS;
	}
	if (!tfs_dentry_add(dir, name, src->inode)) {
		tfs_unlock();
		return ST_NOMEM;
	}
	tfs_touch_ctime(src->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_unlink(const char *path)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 0);

	if (!d || d == &g_root) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (d->inode->type == TFS_DIR) {
		tfs_unlock();
		return ST_PERM; /* unlink(2) on a directory: EPERM */
	}
	tfs_dentry_remove(d);
	tfs_unlock();
	return ST_OK;
}

static int tfs_rmdir(const char *path)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 0);

	if (!d || d == &g_root) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_DIR) {
		tfs_unlock();
		return ST_INVALID; /* ENOTDIR has no ST_ code */
	}
	if (d->inode->nchildren) {
		tfs_unlock();
		return ST_NOTEMPTY;
	}
	tfs_dentry_remove(d);
	tfs_unlock();
	return ST_OK;
}

/* Is `d' equal to `anc' or somewhere below it?  Guards against moving a
 * directory into itself. */
static int tfs_is_below(const struct tfs_dentry *d, const struct tfs_dentry *anc)
{
	for (; d; d = d->parent)
		if (d == anc)
			return 1;
	return 0;
}

static int tfs_rename(const char *oldpath, const char *newpath)
{
	struct tfs_dentry *ndir;
	char name[VFS_NAME_MAX + 1];

	tfs_lock();
	struct tfs_dentry *src = tfs_lookup(oldpath, 0);

	if (!src || src == &g_root) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	int rc = tfs_split(newpath, &ndir, name, sizeof(name));

	if (rc != ST_OK) {
		tfs_unlock();
		return rc;
	}
	if (src->inode->type == TFS_DIR && tfs_is_below(ndir, src)) {
		tfs_unlock();
		return ST_INVALID;
	}
	struct tfs_dentry *dst = tfs_find_child(ndir, name, kstrlen(name));

	if (dst == src) {
		tfs_unlock();
		return ST_OK; /* renaming a name onto itself */
	}
	if (dst) {
		/* The target is replaced, with the same rules as unlink and
		 * rmdir: a directory only by a directory, and only when
		 * empty; a file never by a directory. */
		if (dst->inode->type == TFS_DIR) {
			if (src->inode->type != TFS_DIR) {
				tfs_unlock();
				return ST_INVALID;
			}
			if (dst->inode->nchildren) {
				tfs_unlock();
				return ST_NOTEMPTY;
			}
		} else if (src->inode->type == TFS_DIR) {
			tfs_unlock();
			return ST_INVALID;
		}
		if (dst->inode == src->inode) {
			/* Two names for one object: POSIX says do nothing. */
			tfs_unlock();
			return ST_OK;
		}
	}
	char *nn = tfs_strdup(name);

	if (!nn) {
		tfs_unlock();
		return ST_NOMEM;
	}
	if (dst)
		tfs_dentry_remove(dst);
	/* The name moves, the object stays: handles, links and (for a
	 * directory) inode->self all remain valid. */
	tfs_dentry_detach(src);
	kfree(src->name);
	src->name = nn;
	tfs_dentry_attach(ndir, src);
	tfs_touch_ctime(src->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_chdir(const char *path)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 1);
	int ok = d && d->inode->type == TFS_DIR;

	tfs_unlock();
	return ok ? ST_OK : ST_NOT_FOUND;
}

/* ---- attributes ---------------------------------------------------------------- */

static int tfs_chmod(const char *path, unsigned int mode)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 1);

	if (!d) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_LNK)
		d->inode->mode = mode & 07777;
	tfs_touch_ctime(d->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_chown(const char *path, int uid, int gid)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 1);

	if (!d) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (uid != -1)
		d->inode->uid = (uint32_t)uid;
	if (gid != -1)
		d->inode->gid = (uint32_t)gid;
	tfs_touch_ctime(d->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_fchmod(vfs_file_t *f, unsigned int mode)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	tfs_lock();
	tf->inode->mode = mode & 07777;
	tfs_touch_ctime(tf->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_fchown(vfs_file_t *f, int uid, int gid)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	tfs_lock();
	if (uid != -1)
		tf->inode->uid = (uint32_t)uid;
	if (gid != -1)
		tf->inode->gid = (uint32_t)gid;
	tfs_touch_ctime(tf->inode);
	tfs_unlock();
	return ST_OK;
}

static int tfs_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec)
{
	tfs_lock();
	struct tfs_dentry *d = tfs_lookup(path, 1);

	if (!d) {
		tfs_unlock();
		return ST_NOT_FOUND;
	}
	if (mtime_nsec == VFS_UTIME_NOW)
		d->inode->mtime = timer_get_epoch();
	else if (mtime_nsec != VFS_UTIME_OMIT)
		d->inode->mtime = (uint64_t)mtime_sec;
	d->inode->ctime = timer_get_epoch();
	tfs_unlock();
	return ST_OK;
}

static int tfs_statfs(struct vfs_statfs *out)
{
	tfs_lock();
	mm_memset(out, 0, sizeof(*out));
	out->f_type = TFS_MAGIC;
	out->f_bsize = PAGE_SIZE;
	out->f_frsize = PAGE_SIZE;
	out->f_blocks = g_cap_pages;
	out->f_bfree = g_cap_pages - g_used_pages;
	out->f_bavail = out->f_bfree;
	/* Objects are limited only by memory; report the frame cap as the
	 * object cap so the numbers are at least plausible. */
	out->f_files = g_cap_pages;
	out->f_ffree = g_cap_pages > g_inodes ? g_cap_pages - g_inodes : 0;
	out->f_fsid = TFS_DEV;
	out->f_namelen = VFS_NAME_MAX;
	tfs_unlock();
	return ST_OK;
}

/* ---- init ---------------------------------------------------------------------- */

void tmpfs_init(void)
{
	memory_stats_t ms;

	spinlock_init(&g_mtx.lock, "tmpfs");
	g_mtx.owner = (uint64_t)-1;

	mm_get_memory_stats(&ms);
	g_cap_pages = ms.total_pages / TFS_RAM_FRACTION;
	if (g_cap_pages < TFS_MIN_CAP_PAGES)
		g_cap_pages = TFS_MIN_CAP_PAGES;

	mm_memset(&g_root_inode, 0, sizeof(g_root_inode));
	g_root_inode.type = TFS_DIR;
	/* Like /tmp: anyone may create here, only the owner may remove. */
	g_root_inode.mode = 01777;
	g_root_inode.ino = 1;
	g_root_inode.nlink = 1;
	g_root_inode.atime = g_root_inode.mtime = g_root_inode.ctime =
		timer_get_epoch();
	g_root_inode.self = &g_root;
	mm_memset(&g_root, 0, sizeof(g_root));
	g_root.name = "";
	g_root.inode = &g_root_inode;
	g_inodes = 1;

	/* Designated initialisers, as the vfs.h note asks: the slots are
	 * named, so a member added to vfs_ops_t cannot shift these. */
	g_tfs_ops = (vfs_ops_t){
		.open = tfs_open,
		.open_mode = tfs_open_mode,
		.stat = tfs_stat,
		.lstat = tfs_lstat,
		.fstat = tfs_fstat,
		.read = tfs_read,
		.read_at = tfs_read_at,
		.write = tfs_write,
		.seek = tfs_seek,
		.readdir = tfs_readdir,
		.truncate = tfs_truncate,
		.unlink = tfs_unlink,
		.rename = tfs_rename,
		.mkdir = tfs_mkdir,
		.rmdir = tfs_rmdir,
		.mknod = tfs_mknod,
		.symlink = tfs_symlink,
		.readlink = tfs_readlink,
		.link = tfs_link,
		.chdir = tfs_chdir,
		.close = tfs_close,
		.chmod = tfs_chmod,
		.chown = tfs_chown,
		.fchmod = tfs_fchmod,
		.fchown = tfs_fchown,
		.utimensat = tfs_utimensat,
		.statfs = tfs_statfs,
		.release_locks_for_task = tfs_release_locks_for_task,
		/* No fsync/sync: there is nothing to flush to.  No xattrs. */
	};

	if (vfs_register_mount(TMPFS_MOUNT, &g_tfs_ops) != ST_OK) {
		kprintf("tmpfs: could not register %s\n", TMPFS_MOUNT);
		return;
	}
	kprintf("tmpfs: %s ready, up to %lu MB\n", TMPFS_MOUNT,
		g_cap_pages * (PAGE_SIZE / 1024) / 1024);
}
