// LikeOS -- tmpfs: a filesystem kept entirely in RAM, mounted at /ram.
//
// Copyright (C) 2026 The LikeOS Project

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
 * The kernel mounts one instance at boot, at TMPFS_MOUNT (/ram), on every
 * system -- the live medium and an installed disk alike -- as an entry of
 * the mount table (vfs_mount_boot); /proc/mounts lists it as tmpfs at /ram.
 * mount -t tmpfs makes further instances anywhere else.
 *
 * Interface: the path-based vfs_ops_t every other filesystem here speaks,
 * reached through the VFS mount table (tmpfs_fstype, mount(2)), so the paths
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
#include <kernel/fs/mount.h>
#include <kernel/fs/pathbuf.h>
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
 * usable tmpfs (16 MB). */
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


/* ---- Structures --------------------------------------------------------- */

enum tfs_type { TFS_REG, TFS_DIR, TFS_LNK, TFS_SOCK, TFS_FIFO, TFS_CHR, TFS_BLK };

struct tfs_dentry;
struct tfs_sb;

/* The object: what stat() describes and what a handle holds. */
struct tfs_inode {
	struct tfs_sb *sb; /* the instance it belongs to                    */
	enum tfs_type type;
	uint32_t mode; /* permission bits (07777)                       */
	uint32_t uid, gid;
	uint64_t ino;
	uint64_t atime, mtime, ctime; /* seconds since the epoch       */
	uint32_t atime_ns, mtime_ns, ctime_ns; /* and their nanoseconds */
	uint64_t rdev; /* device nodes: the device number               */
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

/* One mounted instance: the boot-time /ram, and one per tmpfs mount(2).
 * Each has its own tree, its own frame budget and its own lock. */
struct tfs_sb {
	struct {
		spinlock_t lock; /* protects the three fields below           */
		volatile int held;
		volatile uint64_t owner; /* task id of the holder               */
		volatile int depth; /* reentrant holds by the owner              */
	} mtx;
	struct tfs_inode root_inode;
	struct tfs_dentry root;
	uint64_t next_ino;
	unsigned long cap_pages; /* frames the instance may hold in total */
	unsigned long used_pages; /* frames it holds now              */
	unsigned long inodes; /* objects alive (for statfs)           */
	unsigned long max_inodes; /* nr_inodes=, 0 for the frame cap   */
	unsigned long fsid; /* statfs f_fsid */
	/* Where it is mounted (an absolute symlink is resolved inside the
	 * instance when it points below there). */
	const vfs_mount_t *mnt;
};

static vfs_ops_t g_tfs_ops;

/* ---- The lock ------------------------------------------------------------ */

static void tfs_lock(struct tfs_sb *sb)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? (uint64_t)cur->id : (uint64_t)-2;

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&sb->mtx.lock, &fl);
		if (!sb->mtx.held) {
			sb->mtx.held = 1;
			sb->mtx.owner = me;
			sb->mtx.depth = 1;
			task_sleep_lock_taken();
			spin_unlock_irqrestore(&sb->mtx.lock, fl);
			return;
		}
		if (sb->mtx.owner == me) {
			sb->mtx.depth++;
			spin_unlock_irqrestore(&sb->mtx.lock, fl);
			return;
		}
		/* Park on the mutex as ext4's rwsem does: BLOCKED is set under
		 * the spinlock, so a release that happens after this unlock
		 * finds the task on the channel and wakes it. */
		if (cur && irqs_enabled()) {
			cur->wait_channel = (void *)&sb->mtx;
			task_set_blocked(cur);
			spin_unlock_irqrestore(&sb->mtx.lock, fl);
			sched_schedule();
		} else {
			spin_unlock_irqrestore(&sb->mtx.lock, fl);
			__asm__ volatile("pause");
		}
	}
}

static void tfs_unlock(struct tfs_sb *sb)
{
	uint64_t fl;
	int released = 0;

	spin_lock_irqsave(&sb->mtx.lock, &fl);
	if (sb->mtx.held && --sb->mtx.depth == 0) {
		sb->mtx.held = 0;
		sb->mtx.owner = (uint64_t)-1;
		released = 1;
		task_sleep_lock_dropped();
	}
	spin_unlock_irqrestore(&sb->mtx.lock, fl);
	if (released)
		sched_wake_channel((void *)&sb->mtx);
}

/* The reaper's hook: a task that died inside an operation must not keep
 * the tree locked forever. */
static int tfs_release_locks_for_task(vfs_mount_t *m, uint64_t task_id)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	uint64_t fl;
	int released = 0;

	spin_lock_irqsave(&sb->mtx.lock, &fl);
	if (sb->mtx.held && sb->mtx.owner == task_id) {
		sb->mtx.held = 0;
		sb->mtx.depth = 0;
		sb->mtx.owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&sb->mtx.lock, fl);
	if (released)
		sched_wake_channel((void *)&sb->mtx);
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
	n->ctime_ns = 0;
}

static void tfs_touch_mtime(struct tfs_inode *n)
{
	n->mtime = n->ctime = timer_get_epoch();
	n->mtime_ns = n->ctime_ns = 0;
}

/* ---- Frames --------------------------------------------------------------- */

/* Make the file hold at least `want' frames; new ones are zero.  Fails with
 * ST_NOSPC against the cap, ST_NOMEM against the allocator.  Frames added
 * before a failure stay: they are zero and accounted for. */
static int tfs_grow_pages(struct tfs_inode *n, unsigned long want)
{
	struct tfs_sb *sb = n->sb;

	if (want <= n->npages)
		return ST_OK;
	if (want - n->npages > sb->cap_pages - sb->used_pages)
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
		sb->used_pages++;
	}
	return ST_OK;
}

/* Drop frames beyond `keep'. */
static void tfs_shrink_pages(struct tfs_inode *n, unsigned long keep)
{
	struct tfs_sb *sb = n->sb;

	while (n->npages > keep) {
		n->npages--;
		mm_free_physical_page(n->pages[n->npages]);
		sb->used_pages--;
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

static struct tfs_inode *tfs_inode_new(struct tfs_sb *sb, enum tfs_type type,
				       uint32_t mode)
{
	if (sb->max_inodes && sb->inodes >= sb->max_inodes)
		return NULL;
	struct tfs_inode *n = kalloc(sizeof(*n));

	if (!n)
		return NULL;
	mm_memset(n, 0, sizeof(*n));
	n->sb = sb;
	n->type = type;
	n->mode = mode & 07777;
	task_t *cur = sched_current();

	if (cur) {
		n->uid = cur->cred.fsuid;
		n->gid = cur->cred.fsgid;
	}
	n->ino = ++sb->next_ino;
	n->atime = n->mtime = n->ctime = timer_get_epoch();
	sb->inodes++;
	return n;
}

static void tfs_inode_free(struct tfs_inode *n)
{
	struct tfs_sb *sb = n->sb;

	tfs_shrink_pages(n, 0);
	if (n->target)
		kfree(n->target);
	sb->inodes--;
	kfree(n);
}

/* An object goes when it has no name left and nobody holds it open. */
static void tfs_inode_maybe_free(struct tfs_inode *n)
{
	if (n->nlink == 0 && n->refs == 0 && n != &n->sb->root_inode)
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

/* A path as the VFS hands it over -- relative to the mount point, with a
 * leading slash: "/a/b" -- without that slash: "a/b", "" for the root. */
static const char *tfs_rel(struct tfs_sb *sb, const char *path)
{
	(void)sb;
	if (!path)
		return NULL;
	while (*path == '/')
		path++;
	return path;
}

/* The part of an ABSOLUTE path (a symlink's target) below the instance's
 * mount point, without its leading slash: "/ram/a/b" -> "a/b", "/ram" ->
 * "".  NULL if it leads out of the instance. */
static const char *tfs_target_rel(struct tfs_sb *sb, const char *path)
{
	const char *mp = sb->mnt ? sb->mnt->path : NULL;
	size_t ml, k = 0;

	if (!path || !mp)
		return NULL;
	ml = kstrlen(mp);
	if (ml == 1) /* mounted on "/" */
		ml = 0;
	while (k < ml && path[k] == mp[k])
		k++;
	if (k != ml)
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
 * rename/link: no); one in the middle always is.
 *
 * `whole' is the path the VFS handed the operation, when `p' is (a prefix
 * of) it starting at byte `woff' -- NULL otherwise.  A link met on it is
 * offered to the VFS (vfs_link_hop), which follows it wherever in the name
 * space it leads: then NULL comes back, and the operation fails without
 * having changed anything, to be run again where the link leads.  Followed
 * here instead, a link whose target leaves the instance reads as missing. */
static struct tfs_dentry *tfs_resolve(struct tfs_sb *sb,
				      struct tfs_dentry *base, const char *p,
				      int follow, int hops, const char *whole,
				      size_t woff)
{
	struct tfs_dentry *cur = base;
	const char *p0 = p;

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
		/* "name/" names what is THROUGH a link: followed even by
		 * the operations that otherwise stop at one. */
		if (next->inode->type == TFS_LNK &&
		    (!last || follow || *p == '/')) {
			const char *t = next->inode->target;

			if (whole && hops == 0 &&
			    vfs_link_hop((vfs_mount_t *)sb->mnt, whole,
					 woff + (size_t)(s - p0),
					 woff + (size_t)(s - p0) + len, t))
				return NULL; /* the VFS follows it */
			if (t[0] == '/') {
				const char *r = tfs_target_rel(sb, t);

				if (!r)
					return NULL; /* leaves the instance */
				next = tfs_resolve(sb, &sb->root, r, 1,
						   hops + 1, NULL, 0);
			} else {
				next = tfs_resolve(sb, cur, t, 1, hops + 1,
						   NULL, 0);
			}
			if (!next)
				return NULL;
		}
		cur = next;
		/* ...and has to be a directory. */
		if (last && *p == '/' && cur->inode->type != TFS_DIR)
			return NULL;
	}
	return cur;
}

static struct tfs_dentry *tfs_lookup(struct tfs_sb *sb, const char *path,
				     int follow)
{
	const char *rel = tfs_rel(sb, path);

	if (!rel)
		return NULL;
	return tfs_resolve(sb, &sb->root, rel, follow, 0,
			   path[0] == '/' ? path : NULL, (size_t)(rel - path));
}

/* Split `path' into its parent directory (resolved, symlinks followed) and
 * its final name, copied to `name' (NUL-terminated).  The name must be a
 * real one: not empty, not "." or "..", not longer than VFS_NAME_MAX. */
static int tfs_split(struct tfs_sb *sb, const char *path,
		     struct tfs_dentry **dir_out, char *name, size_t namecap)
{
	const char *rel = tfs_rel(sb, path);

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
		dir = &sb->root;
	} else {
		/* The directory part, as a string of its own (a path
		 * buffer: never on the stack). */
		char *prefix;

		if (start >= PATHBUF_SIZE)
			return ST_NAMETOOLONG;
		prefix = pathbuf_get();
		if (!prefix)
			return ST_NOMEM;
		mm_memcpy(prefix, rel, start);
		prefix[start] = 0;
		dir = tfs_resolve(sb, &sb->root, prefix, 1, 0,
				  path[0] == '/' ? path : NULL,
				  (size_t)(rel - path));
		pathbuf_put(prefix);
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
	st->st_atime_nsec = n->atime_ns;
	st->st_mtime_nsec = n->mtime_ns;
	st->st_ctime_nsec = n->ctime_ns;
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
	case TFS_CHR:
		st->st_mode = S_IFCHR | n->mode;
		st->st_rdev = n->rdev;
		break;
	case TFS_BLK:
		st->st_mode = S_IFBLK | n->mode;
		st->st_rdev = n->rdev;
		break;
	default:
		st->st_mode = S_IFREG | n->mode;
		st->st_size = n->size;
		st->st_blocks = n->npages * (PAGE_SIZE / 512);
		break;
	}
}

static int tfs_stat_common(struct tfs_sb *sb, const char *path,
			   struct kstat *st, int follow)
{
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, follow);

	if (!d) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	tfs_fill_stat(d->inode, st);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_stat(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return tfs_stat_common((struct tfs_sb *)m->fs_data, path, st, 1);
}

static int tfs_lstat(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return tfs_stat_common((struct tfs_sb *)m->fs_data, path, st, 0);
}

static int tfs_fstat(vfs_file_t *f, struct kstat *st)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	tfs_fill_stat(tf->inode, st);
	tfs_unlock(sb);
	return ST_OK;
}

/* ---- open / close ---------------------------------------------------------- */

static int tfs_open_mode(vfs_mount_t *m, const char *path, int flags,
			 unsigned int mode, vfs_file_t **out)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	int acc = flags & 3;
	int writing = (acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC));

	if (!tfs_rel(sb, path))
		return ST_NOT_FOUND;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 1);
	struct tfs_inode *n;

	/* O_CREAT|O_EXCL fails on a name that exists.  Decided here, under
	 * the lock the create below holds too, so two such opens of one name
	 * cannot both succeed. */
	int excl = (flags & O_CREAT) && (flags & O_EXCL);

	if (d && excl) {
		tfs_unlock(sb);
		return ST_EXISTS;
	}
	if (!d) {
		/* A missing name is created when asked. */
		if (!(flags & O_CREAT)) {
			tfs_unlock(sb);
			return ST_NOT_FOUND;
		}
		struct tfs_dentry *dir;
		char name[VFS_NAME_MAX + 1];
		int rc = tfs_split(sb, path, &dir, name, sizeof(name));

		if (rc != ST_OK) {
			tfs_unlock(sb);
			return rc;
		}
		/* The lookup followed symlinks; a name that exists but leads
		 * nowhere (a dangling link) is not one to create under -- and
		 * for O_EXCL it is a name that exists. */
		if (tfs_find_child(dir, name, kstrlen(name))) {
			tfs_unlock(sb);
			return excl ? ST_EXISTS : ST_NOT_FOUND;
		}
		n = tfs_inode_new(sb, TFS_REG, mode);
		if (!n) {
			tfs_unlock(sb);
			return ST_NOMEM;
		}
		if (!tfs_dentry_add(dir, name, n)) {
			tfs_inode_free(n);
			tfs_unlock(sb);
			return ST_NOMEM;
		}
	} else {
		n = d->inode;
		if (n->type == TFS_DIR) {
			if (writing) {
				tfs_unlock(sb);
				return ST_INVALID; /* EISDIR has no ST_ code */
			}
		} else if (n->type != TFS_REG) {
			/* A socket or FIFO node is a name, not something to
			 * read. */
			tfs_unlock(sb);
			return ST_UNSUPPORTED;
		}
	}

	tfs_file_t *tf = kalloc(sizeof(*tf));

	if (!tf) {
		tfs_unlock(sb);
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
	tfs_unlock(sb);
	*out = &tf->vfs;
	return ST_OK;
}

/* Plain open: in-kernel openers, which never create here.  0644 is what a
 * creation through this path gets, the same default ext4 applies. */
static int tfs_open(vfs_mount_t *m, const char *path, int flags,
		    vfs_file_t **out)
{
	return tfs_open_mode(m, path, flags, 0644, out);
}

static int tfs_close(vfs_file_t *f)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_OK;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	struct tfs_inode *n = tf->inode;

	n->refs--;
	tfs_inode_maybe_free(n);
	tfs_unlock(sb);
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
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	long n = tfs_copy_out(tf->inode, buf, bytes, (unsigned long)tf->pos);

	if (n > 0)
		tf->pos += n;
	tfs_unlock(sb);
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
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	long n = tfs_copy_out(tf->inode, buf, bytes, (unsigned long)off);

	tfs_unlock(sb);
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
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	struct tfs_inode *n = tf->inode;

	if (f->flags & O_APPEND)
		tf->pos = (long)n->size;
	unsigned long off = (unsigned long)tf->pos;
	unsigned long end = off + (unsigned long)bytes;

	if (end < off || end > TFS_MAX_FILE_SIZE) {
		tfs_unlock(sb);
		return -ENOSPC; /* no EFBIG in this kernel */
	}
	/* Frames for the whole write first, so it is all or nothing: a
	 * write that half-lands and then fails is worse than one refused. */
	int rc = tfs_grow_pages(n, (end + PAGE_SIZE - 1) / PAGE_SIZE);

	if (rc != ST_OK) {
		tfs_unlock(sb);
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
	tfs_unlock(sb);
	return bytes;
}

static long tfs_seek(vfs_file_t *f, long offset, int whence)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return -EINVAL;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	if (tf->inode->type == TFS_DIR) {
		/* rewinddir(): only a rewind means anything for a listing. */
		if (whence == SEEK_SET && offset == 0)
			tf->dirpos = 0;
		tfs_unlock(sb);
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
		tfs_unlock(sb);
		return -EINVAL;
	}
	long np = base + offset;

	if (np < 0) {
		tfs_unlock(sb);
		return -EINVAL;
	}
	tf->pos = np;
	tfs_unlock(sb);
	return np;
}

static int tfs_truncate(vfs_file_t *f, unsigned long size)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	if (tf->inode->type != TFS_REG)
		return ST_INVALID;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	int rc = tfs_set_size(tf->inode, size);

	if (rc == ST_OK)
		tfs_touch_mtime(tf->inode);
	tfs_unlock(sb);
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
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	struct tfs_inode *dir = tf->inode;
	unsigned off = 0;
	unsigned long idx = 0;
	unsigned rl;

	if (tf->dirpos == 0) {
		rl = tfs_put_dirent(buf, (unsigned)bytes, off, ".", dir->ino,
				    DT_DIR);
		if (!rl) {
			tfs_unlock(sb);
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
	tfs_unlock(sb);
	return (long)off;
}

/* ---- namespace changes -------------------------------------------------------- */

/* Create a fresh object of `type' under a new name.  Shared by mkdir, mknod
 * and symlink; open() creates its own because it also needs the handle. */
static int tfs_create(struct tfs_sb *sb, const char *path, enum tfs_type type,
		      unsigned int mode, const char *target, uint64_t rdev)
{
	struct tfs_dentry *dir;
	char name[VFS_NAME_MAX + 1];
	int rc = tfs_split(sb, path, &dir, name, sizeof(name));

	if (rc != ST_OK)
		return rc;
	if (tfs_find_child(dir, name, kstrlen(name)))
		return ST_EXISTS;
	struct tfs_inode *n = tfs_inode_new(sb, type, mode);

	if (!n)
		return ST_NOMEM;
	n->rdev = rdev;
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

static int tfs_mkdir(vfs_mount_t *m, const char *path, unsigned int mode)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	int rc = tfs_create(sb, path, TFS_DIR, mode, NULL, 0);

	tfs_unlock(sb);
	return rc;
}

static int tfs_mknod(vfs_mount_t *m, const char *path, unsigned int mode,
		     uint64_t dev)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	enum tfs_type t;

	switch (mode & S_IFMT) {
	case S_IFSOCK:
		t = TFS_SOCK;
		break;
	case S_IFIFO:
		t = TFS_FIFO;
		break;
	case S_IFCHR:
		t = TFS_CHR;
		break;
	case S_IFBLK:
		t = TFS_BLK;
		break;
	case S_IFREG:
	case 0:
		t = TFS_REG;
		break;
	default:
		return ST_UNSUPPORTED;
	}
	tfs_lock(sb);
	int rc = tfs_create(sb, path, t, mode, NULL,
			    (t == TFS_CHR || t == TFS_BLK) ? dev : 0);

	tfs_unlock(sb);
	return rc;
}

static int tfs_symlink(vfs_mount_t *m, const char *target,
		       const char *linkpath)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	if (!target || !*target)
		return ST_INVALID;
	tfs_lock(sb);
	int rc = tfs_create(sb, linkpath, TFS_LNK, 0777, target, 0);

	tfs_unlock(sb);
	return rc;
}

static int tfs_readlink(vfs_mount_t *m, const char *path, char *buf,
			unsigned long bufsz)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 0);

	if (!d) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_LNK) {
		tfs_unlock(sb);
		return ST_INVALID;
	}
	unsigned long k = 0;

	while (d->inode->target[k] && k < bufsz) {
		buf[k] = d->inode->target[k];
		k++;
	}
	tfs_unlock(sb);
	return (int)k;
}

/* A second name for an existing object.  Not for directories (a loop in
 * the tree is not worth the trouble, and no filesystem allows it); the
 * name itself is what is linked when `oldpath' is a symlink, unless
 * `follow' asks for what it points to.  The VFS has made sure both names
 * are on this instance. */
static int tfs_link(vfs_mount_t *m, const char *oldpath, const char *newpath,
		    int follow)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	struct tfs_dentry *dir;
	char name[VFS_NAME_MAX + 1];

	tfs_lock(sb);
	struct tfs_dentry *src = tfs_lookup(sb, oldpath, follow ? 1 : 0);

	if (!src) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (src->inode->type == TFS_DIR) {
		tfs_unlock(sb);
		return ST_PERM;
	}
	int rc = tfs_split(sb, newpath, &dir, name, sizeof(name));

	if (rc != ST_OK) {
		tfs_unlock(sb);
		return rc;
	}
	if (tfs_find_child(dir, name, kstrlen(name))) {
		tfs_unlock(sb);
		return ST_EXISTS;
	}
	if (!tfs_dentry_add(dir, name, src->inode)) {
		tfs_unlock(sb);
		return ST_NOMEM;
	}
	tfs_touch_ctime(src->inode);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_unlink(vfs_mount_t *m, const char *path)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 0);

	if (!d || d == &sb->root) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (d->inode->type == TFS_DIR) {
		tfs_unlock(sb);
		return ST_PERM; /* unlink(2) on a directory: EPERM */
	}
	tfs_dentry_remove(d);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_rmdir(vfs_mount_t *m, const char *path)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 0);

	if (!d || d == &sb->root) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_DIR) {
		tfs_unlock(sb);
		return ST_NOTDIR;
	}
	if (d->inode->nchildren) {
		tfs_unlock(sb);
		return ST_NOTEMPTY;
	}
	tfs_dentry_remove(d);
	tfs_unlock(sb);
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

static int tfs_rename(vfs_mount_t *m, const char *oldpath, const char *newpath)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	struct tfs_dentry *ndir;
	char name[VFS_NAME_MAX + 1];

	tfs_lock(sb);
	struct tfs_dentry *src = tfs_lookup(sb, oldpath, 0);

	if (!src || src == &sb->root) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	int rc = tfs_split(sb, newpath, &ndir, name, sizeof(name));

	if (rc != ST_OK) {
		tfs_unlock(sb);
		return rc;
	}
	if (src->inode->type == TFS_DIR && tfs_is_below(ndir, src)) {
		tfs_unlock(sb);
		return ST_INVALID;
	}
	struct tfs_dentry *dst = tfs_find_child(ndir, name, kstrlen(name));

	if (dst == src) {
		tfs_unlock(sb);
		return ST_OK; /* renaming a name onto itself */
	}
	if (dst) {
		/* The target is replaced, with the same rules as unlink and
		 * rmdir: a directory only by a directory, and only when
		 * empty; a file never by a directory. */
		if (dst->inode->type == TFS_DIR) {
			if (src->inode->type != TFS_DIR) {
				tfs_unlock(sb);
				return ST_INVALID;
			}
			if (dst->inode->nchildren) {
				tfs_unlock(sb);
				return ST_NOTEMPTY;
			}
		} else if (src->inode->type == TFS_DIR) {
			tfs_unlock(sb);
			return ST_INVALID;
		}
		if (dst->inode == src->inode) {
			/* Two names for one object: POSIX says do nothing. */
			tfs_unlock(sb);
			return ST_OK;
		}
	}
	char *nn = tfs_strdup(name);

	if (!nn) {
		tfs_unlock(sb);
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
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_chdir(vfs_mount_t *m, const char *path)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 1);
	int ok = d && d->inode->type == TFS_DIR;

	tfs_unlock(sb);
	return ok ? ST_OK : ST_NOT_FOUND;
}

/* ---- attributes ---------------------------------------------------------------- */

static int tfs_chmod(vfs_mount_t *m, const char *path, unsigned int mode)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, 1);

	if (!d) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (d->inode->type != TFS_LNK)
		d->inode->mode = mode & 07777;
	tfs_touch_ctime(d->inode);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_chown(vfs_mount_t *m, const char *path, int uid, int gid,
		     int nofollow)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, !nofollow);

	if (!d) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	if (uid != -1)
		d->inode->uid = (uint32_t)uid;
	if (gid != -1)
		d->inode->gid = (uint32_t)gid;
	tfs_touch_ctime(d->inode);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_fchmod(vfs_file_t *f, unsigned int mode)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	tf->inode->mode = mode & 07777;
	tfs_touch_ctime(tf->inode);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_fchown(vfs_file_t *f, int uid, int gid)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	if (uid != -1)
		tf->inode->uid = (uint32_t)uid;
	if (gid != -1)
		tf->inode->gid = (uint32_t)gid;
	tfs_touch_ctime(tf->inode);
	tfs_unlock(sb);
	return ST_OK;
}

/* One time of an inode as struct vfs_utimes asks. */
static void tfs_set_time(uint64_t *sec, uint32_t *ns, int64_t want_sec,
			 long want_ns)
{
	if (want_ns == VFS_UTIME_OMIT)
		return;
	if (want_ns == VFS_UTIME_NOW) {
		*sec = timer_get_epoch();
		*ns = 0;
		return;
	}
	*sec = (uint64_t)want_sec;
	*ns = (want_ns >= 0 && want_ns <= 999999999L) ? (uint32_t)want_ns : 0;
}

static void tfs_utimes_inode(struct tfs_inode *n, const struct vfs_utimes *t)
{
	tfs_set_time(&n->atime, &n->atime_ns, t->atime_sec, t->atime_nsec);
	tfs_set_time(&n->mtime, &n->mtime_ns, t->mtime_sec, t->mtime_nsec);
	tfs_touch_ctime(n);
}

static int tfs_utimens(vfs_mount_t *m, const char *path, int nofollow,
		       const struct vfs_utimes *t)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	struct tfs_dentry *d = tfs_lookup(sb, path, !nofollow);

	if (!d) {
		tfs_unlock(sb);
		return ST_NOT_FOUND;
	}
	tfs_utimes_inode(d->inode, t);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_futimens(vfs_file_t *f, const struct vfs_utimes *t)
{
	tfs_file_t *tf = f->fs_private;

	if (!tf)
		return ST_INVALID;
	struct tfs_sb *sb = tf->inode->sb;
	tfs_lock(sb);
	tfs_utimes_inode(tf->inode, t);
	tfs_unlock(sb);
	return ST_OK;
}

static int tfs_statfs(vfs_mount_t *m, struct vfs_statfs *out)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	tfs_lock(sb);
	mm_memset(out, 0, sizeof(*out));
	out->f_type = TFS_MAGIC;
	out->f_bsize = PAGE_SIZE;
	out->f_frsize = PAGE_SIZE;
	out->f_blocks = sb->cap_pages;
	out->f_bfree = sb->cap_pages - sb->used_pages;
	out->f_bavail = out->f_bfree;
	/* Objects are limited only by memory unless nr_inodes= says
	 * otherwise; report the frame cap as the object cap so the numbers
	 * are at least plausible. */
	unsigned long files = sb->max_inodes ? sb->max_inodes : sb->cap_pages;
	out->f_files = files;
	out->f_ffree = files > sb->inodes ? files - sb->inodes : 0;
	out->f_fsid = sb->fsid;
	out->f_namelen = VFS_NAME_MAX;
	tfs_unlock(sb);
	return ST_OK;
}

/* ---- instances ----------------------------------------------------------------- */

/* An empty tree: the root directory alone. */
static void tfs_sb_init(struct tfs_sb *sb, unsigned long cap_pages,
			uint32_t root_mode, uint32_t uid, uint32_t gid)
{
	static unsigned long next_fsid = TFS_DEV;

	mm_memset(sb, 0, sizeof(*sb));
	spinlock_init(&sb->mtx.lock, "tmpfs");
	sb->mtx.owner = (uint64_t)-1;
	sb->cap_pages = cap_pages;
	sb->next_ino = 1;
	sb->fsid = __atomic_fetch_add(&next_fsid, 1, __ATOMIC_RELAXED);
	sb->root_inode.sb = sb;
	sb->root_inode.type = TFS_DIR;
	sb->root_inode.mode = root_mode & 07777;
	sb->root_inode.uid = uid;
	sb->root_inode.gid = gid;
	sb->root_inode.ino = 1;
	sb->root_inode.nlink = 1;
	sb->root_inode.atime = sb->root_inode.mtime = sb->root_inode.ctime =
		timer_get_epoch();
	sb->root_inode.self = &sb->root;
	sb->root.name = "";
	sb->root.inode = &sb->root_inode;
	sb->inodes = 1;
}

/* Free a whole tree (an unmount: nothing is open on it any more). */
static void tfs_free_tree(struct tfs_dentry *dir)
{
	struct tfs_dentry *c = dir->inode->children;

	while (c) {
		struct tfs_dentry *next = c->sibling;
		struct tfs_inode *n = c->inode;

		if (n->type == TFS_DIR)
			tfs_free_tree(c);
		/* What remains is the name; the object goes with its last. */
		n->nlink--;
		kfree(c->name);
		kfree(c);
		if (n->nlink == 0) {
			WARN_ON(n->refs != 0);
			tfs_inode_free(n);
		}
		c = next;
	}
	dir->inode->children = NULL;
	dir->inode->nchildren = 0;
}

/* The default size of an instance: a share of memory, never too little. */
static unsigned long tfs_default_cap(void)
{
	memory_stats_t ms;
	unsigned long cap;

	mm_get_memory_stats(&ms);
	cap = ms.total_pages / TFS_RAM_FRACTION;
	if (cap < TFS_MIN_CAP_PAGES)
		cap = TFS_MIN_CAP_PAGES;
	return cap;
}

/* A number with an optional k/m/g suffix (size=, nr_inodes=); 0 when it
 * is not one. */
static int tfs_parse_size(const char *v, unsigned long *out, int *percent)
{
	unsigned long n = 0;
	int digits = 0;

	*percent = 0;
	while (*v >= '0' && *v <= '9') {
		n = n * 10 + (unsigned long)(*v - '0');
		v++;
		digits++;
	}
	if (!digits)
		return 0;
	switch (*v) {
	case 'k':
	case 'K':
		n <<= 10;
		v++;
		break;
	case 'm':
	case 'M':
		n <<= 20;
		v++;
		break;
	case 'g':
	case 'G':
		n <<= 30;
		v++;
		break;
	case '%':
		*percent = 1;
		v++;
		break;
	}
	if (*v)
		return 0;
	*out = n;
	return 1;
}

static unsigned long tfs_parse_octal(const char *v, int *ok)
{
	unsigned long n = 0;

	*ok = (*v != 0);
	for (; *v; v++) {
		if (*v < '0' || *v > '7') {
			*ok = 0;
			return 0;
		}
		n = n * 8 + (unsigned long)(*v - '0');
	}
	return n;
}

static unsigned long tfs_parse_dec(const char *v, int *ok)
{
	unsigned long n = 0;

	*ok = (*v != 0);
	for (; *v; v++) {
		if (*v < '0' || *v > '9') {
			*ok = 0;
			return 0;
		}
		n = n * 10 + (unsigned long)(*v - '0');
	}
	return n;
}

/* The options of a tmpfs mount: size= (bytes, k/m/g, or a percentage of
 * memory), nr_inodes=, mode= (octal, of the root directory), uid=, gid=.
 * Anything else is accepted and ignored.  ST_OK or ST_INVALID for a value
 * that is not one. */
static int tfs_parse_options(const char *data, unsigned long *cap,
			     unsigned long *max_inodes, uint32_t *mode,
			     uint32_t *uid, uint32_t *gid)
{
	const char *cur = data;
	char tmp[64];
	const char *key, *val;

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val)) {
		int ok = 1, pct = 0;
		unsigned long n = 0;

		if (!val)
			continue;
		if (kstrcmp(key, "size") == 0) {
			ok = tfs_parse_size(val, &n, &pct);
			if (ok && pct) {
				memory_stats_t ms;
				mm_get_memory_stats(&ms);
				n = ms.total_pages * n / 100;
			} else if (ok) {
				n = (n + PAGE_SIZE - 1) / PAGE_SIZE;
			}
			if (ok && n == 0)
				ok = 0;
			if (ok)
				*cap = n;
		} else if (kstrcmp(key, "nr_inodes") == 0) {
			ok = tfs_parse_size(val, &n, &pct) && !pct;
			if (ok)
				*max_inodes = n;
		} else if (kstrcmp(key, "mode") == 0) {
			n = tfs_parse_octal(val, &ok);
			if (ok)
				*mode = (uint32_t)n & 07777;
		} else if (kstrcmp(key, "uid") == 0) {
			n = tfs_parse_dec(val, &ok);
			if (ok)
				*uid = (uint32_t)n;
		} else if (kstrcmp(key, "gid") == 0) {
			n = tfs_parse_dec(val, &ok);
			if (ok)
				*gid = (uint32_t)n;
		}
		if (!ok)
			return ST_INVALID;
	}
	return ST_OK;
}

static int tfs_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			 unsigned long flags, const char *data)
{
	unsigned long cap = tfs_default_cap(), max_inodes = 0;
	uint32_t mode = 01777, uid = 0, gid = 0;

	(void)bdev;
	(void)flags; /* read-only is the VFS's to enforce */
	if (tfs_parse_options(data, &cap, &max_inodes, &mode, &uid, &gid) !=
	    ST_OK)
		return ST_INVALID;
	struct tfs_sb *sb = kalloc(sizeof(*sb));

	if (!sb)
		return ST_NOMEM;
	tfs_sb_init(sb, cap, mode, uid, gid);
	sb->max_inodes = max_inodes;
	sb->mnt = m;
	m->ops = &g_tfs_ops;
	m->fs_data = sb;
	m->sb = NULL; /* not in the page cache */
	return ST_OK;
}

static int tfs_fst_unmount(vfs_mount_t *m)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;

	if (!sb)
		return ST_BUSY;
	tfs_lock(sb);
	tfs_free_tree(&sb->root);
	tfs_unlock(sb);
	WARN_ON(sb->used_pages != 0);
	m->fs_data = NULL;
	kfree(sb);
	return ST_OK;
}

/* A remount may change the size and the root's mode and owner. */
static int tfs_fst_remount(vfs_mount_t *m, unsigned long flags,
			   const char *data)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	unsigned long cap = sb->cap_pages, max_inodes = sb->max_inodes;
	uint32_t mode = sb->root_inode.mode, uid = sb->root_inode.uid,
		 gid = sb->root_inode.gid;

	(void)flags;
	if (tfs_parse_options(data, &cap, &max_inodes, &mode, &uid, &gid) !=
	    ST_OK)
		return ST_INVALID;
	tfs_lock(sb);
	if (cap < sb->used_pages ||
	    (max_inodes && max_inodes < sb->inodes)) {
		tfs_unlock(sb);
		return ST_BUSY; /* would not hold what is there */
	}
	sb->cap_pages = cap;
	sb->max_inodes = max_inodes;
	sb->root_inode.mode = mode;
	sb->root_inode.uid = uid;
	sb->root_inode.gid = gid;
	tfs_unlock(sb);
	return ST_OK;
}

static long tfs_fst_show_options(vfs_mount_t *m, char *buf, long cap)
{
	struct tfs_sb *sb = (struct tfs_sb *)m->fs_data;
	int n;

	if (!sb || cap < 1)
		return 0;
	n = ksnprintf(buf, (size_t)cap, "size=%luk,mode=%o",
		      sb->cap_pages * (PAGE_SIZE / 1024),
		      (unsigned)sb->root_inode.mode);
	if (n > 0 && (sb->root_inode.uid || sb->root_inode.gid) && n < cap)
		n += ksnprintf(buf + n, (size_t)(cap - n), ",uid=%u,gid=%u",
			       sb->root_inode.uid, sb->root_inode.gid);
	if (n > 0 && sb->max_inodes && n < cap)
		n += ksnprintf(buf + n, (size_t)(cap - n), ",nr_inodes=%lu",
			       sb->max_inodes);
	return n < 0 ? 0 : (n >= cap ? cap - 1 : n);
}

const vfs_fstype_t tmpfs_fstype = {
	.name = "tmpfs",
	.aliases = NULL,
	.needs_device = 0,
	.mount = tfs_fst_mount,
	.unmount = tfs_fst_unmount,
	.remount = tfs_fst_remount,
	.show_options = tfs_fst_show_options,
	.probe = NULL,
};

/* ---- init ---------------------------------------------------------------------- */

void tmpfs_init(void)
{
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
		.utimens = tfs_utimens,
		.futimens = tfs_futimens,
		.statfs = tfs_statfs,
		.release_locks_for_task = tfs_release_locks_for_task,
		/* No fsync/sync: there is nothing to flush to.  No xattrs. */
	};

	/* Without size= an instance may hold up to 1/TFS_RAM_FRACTION of
	 * memory; without mode= its root is 1777. */
	if (vfs_fstype_register(&tmpfs_fstype) != ST_OK)
		kprintf("tmpfs: could not register the filesystem type\n");

	/* /ram, mounted by the kernel at boot: the default size (a quarter
	 * of memory) and the default root, 1777 like /tmp -- anyone may
	 * create here, only the owner may remove.  Never unmounted. */
	int r = vfs_mount_boot(TMPFS_MOUNT, &tmpfs_fstype, 0, NULL);
	if (r != ST_OK) {
		kprintf("tmpfs: could not mount %s (%d)\n", TMPFS_MOUNT, r);
		return;
	}
	kprintf("tmpfs: %s ready, up to %lu MB\n", TMPFS_MOUNT,
		tfs_default_cap() * (PAGE_SIZE / 1024) / 1024);
}
