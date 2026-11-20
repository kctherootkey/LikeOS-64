// LikeOS - Minimal VFS implementation
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/vfs.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/ext4.h>
#include <kernel/fs/fat32.h>
#include <kernel/fs/namei.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/fs/fsnotify.h>

/* Is this a pointer the kernel could plausibly have stored?
 *
 * Kernel pointers live in the upper canonical half.  A free poison, a
 * truncated value or uninitialised memory is not one, and following it is a
 * general protection fault that halts the machine -- which is the worst
 * possible outcome for a reference-counting bug, because the report dies with
 * it, and with it every chance of learning who miscounted.  So the operations
 * that follow ->ops check it first: a released file then costs an error
 * return and a line of output instead of the machine. */
static inline int vfs_ptr_plausible(const void *p)
{
	return (uintptr_t)p >= 0xffff800000000000ULL;
}


/* Open-flag bits the permission layer interprets.  These mirror the stable
 * open(2) ABI values (see the syscall header); defined locally with guards so
 * the VFS does not have to depend on the syscall layer's header. */
#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_RDWR
#define O_RDWR 0x0002
#endif
#ifndef O_CREAT
#define O_CREAT 0x0040
#endif
#ifndef O_EXCL
#define O_EXCL 0x0080
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x0200
#endif
#ifndef O_APPEND
#define O_APPEND 0x0400
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif

/* Permission helpers used by the operation wrappers below; the canonical
 * vfs_permission / vfs_permission_traverse / vfs_access are declared in vfs.h. */
static int vfs_attr_allow_modify(const char *path, int is_append_write);
/* Raw stat dispatch with NO permission check — used by the permission layer
 * itself (the public vfs_stat below adds prefix-traversal enforcement, so the
 * permission helpers must use this raw form to avoid unbounded recursion). */
static int vfs_raw_stat(const char *path, struct kstat *st);
static int vfs_is_dev_path(const char *path);
static void vfs_notify_open(const char *path, int flags, int existed,
			    vfs_file_t *f);

/* ---- Canonical paths ---------------------------------------------------------
 *
 * Which mounted filesystem a path is on is decided from its text, by the
 * longest mount point it starts with.  That is only right for a canonical
 * path: "/mnt/x/../y" is not below /mnt/x, and "/mnt/./x/a" is.  Most
 * syscalls canonicalise before they get here, but some pass an absolute path
 * through as it was given, and a filesystem resolving "." and ".." itself
 * cannot see past its own root.  So every entry point below that takes a
 * path makes sure it is canonical first: a path with no "." or ".."
 * component and no doubled slash -- nearly every path -- is used as it is;
 * anything else is resolved lexically into `buf'.  A trailing slash is kept,
 * since it means "a directory" to the filesystem that sees it. */
const char *vfs_canon(const char *path, char *buf)
{
	if (!path || path[0] != '/')
		return path;
	size_t len = 0;
	int odd = 0;
	for (const char *p = path; *p; p++, len++) {
		if (p[0] != '/')
			continue;
		if (p[1] == '/')
			odd = 1;
		else if (p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
			odd = 1;
		else if (p[1] == '.' && p[2] == '.' &&
			 (p[3] == '/' || p[3] == '\0'))
			odd = 1;
	}
	if (!odd)
		return path;
	if (normalize_path("/", path, buf, VFS_MAX_PATH) != 0)
		return path; /* too long to fix: the filesystem decides */
	if (len > 1 && path[len - 1] == '/') {
		size_t n = kstrlen(buf);
		if (n > 1 && n + 1 < VFS_MAX_PATH) {
			buf[n] = '/';
			buf[n + 1] = '\0';
		}
	}
	return buf;
}

/* ---- Walking through symbolic links ------------------------------------------
 *
 * Every operation on a path runs inside a walk (struct vfs_walk, on the
 * caller's stack, published in task_t.vfs_walk while it runs).  The
 * filesystems walk the path themselves, component by component, through
 * their own name caches -- the VFS does not repeat that work.  When one meets
 * a symbolic link it is to follow, it hands the link to the walk
 * (vfs_link_hop) and fails the operation with ST_LINK, having changed
 * nothing; the walk works out where the link leads in the whole name space --
 * mounts and chroot included -- and runs the operation again on that path.
 * A path without links never leaves the filesystem's walk, so it costs what
 * it did.
 *
 * Paths here are lexical: ".." was resolved by vfs_canon() against the text,
 * and is resolved the same way in a link's target, joined to the directory
 * holding the link -- a real directory, since every link before it was
 * followed already.  So ".." out of a mount's root is the directory the mount
 * sits on, and ".." out of a chroot stays at its root.
 *
 * An operation nested inside another (a check made while one is under way)
 * starts no walk of its own: a link it meets on the outer operation's path
 * restarts the outer operation, one met elsewhere is followed by the
 * filesystem as before.  Once a link was handed over, every other link met on
 * the operation's path is refused the same way, so nothing in the operation
 * can get past the point where it is about to be restarted; and a status of
 * success always stands. */

#define VFS_LINK_HOPS 40

struct vfs_walk {
	task_t *owner; /* the task it is published in; NULL: nested */
	const char *path; /* the operation's path now (canonical) */
	int follow; /* a link in the last component is followed */
	int hopped; /* a link was handed over: `next' is where to go on */
	int err; /* it cannot be followed: the operation fails with this */
	unsigned hops;
	char buf[VFS_MAX_PATH]; /* the path, when it is not the caller's */
	char next[VFS_MAX_PATH];
};

static const char *vfs_walk_begin(struct vfs_walk *w, const char *path,
				  int follow)
{
	task_t *cur = sched_current();

	w->owner = NULL;
	w->follow = follow;
	w->hopped = 0;
	w->err = 0;
	w->hops = 0;
	w->path = vfs_canon(path, w->buf);
	if (cur && !cur->vfs_walk && w->path && w->path[0] == '/') {
		cur->vfs_walk = w;
		w->owner = cur;
	}
	return w->path;
}

/* After one run of the operation, with its status in *r: 1 to run it again
 * on w->path (a link was handed over), 0 when *r is the answer. */
static int vfs_walk_again(struct vfs_walk *w, int *r)
{
	unsigned long i;

	if (!w->owner || !w->hopped)
		return 0;
	w->hopped = 0;
	if (*r >= 0)
		return 0; /* done all the same: success stands */
	if (w->err) {
		*r = w->err;
		return 0;
	}
	if (++w->hops > VFS_LINK_HOPS) {
		*r = ST_LOOP;
		return 0;
	}
	for (i = 0; w->next[i] && i + 1 < VFS_MAX_PATH; i++)
		w->buf[i] = w->next[i];
	w->buf[i] = '\0';
	w->path = w->buf;
	return 1;
}

static void vfs_walk_end(struct vfs_walk *w)
{
	if (w->owner)
		w->owner->vfs_walk = NULL;
}

/* Run `expr' on the path `p_' -- and again on wherever the links met on it
 * lead -- leaving its status in `r'.  `w' is a struct vfs_walk of the
 * caller's, so `p_' (possibly pointing into it) is still good afterwards. */
#define VFS_WALK(w, p_, follow, r, expr)                                    \
	do {                                                                \
		for ((p_) = vfs_walk_begin(&(w), (p_), (follow));;          \
		     (p_) = (w).path) {                                     \
			(r) = (expr);                                       \
			if (!vfs_walk_again(&(w), &(r)))                    \
				break;                                      \
		}                                                           \
		vfs_walk_end(&(w));                                         \
	} while (0)

/* Where the link at op[ns_cs, ns_end) of the operation's path leads, with
 * the rest of the path after it, into w->next: from the task's root for an
 * absolute target, from the link's directory for a relative one, ".." taken
 * lexically and never above the root.  A trailing slash is kept. */
static int vfs_hop_join(struct vfs_walk *w, task_t *cur, const char *op,
			unsigned long ns_cs, unsigned long ns_end,
			const char *target)
{
	const char *root = (cur && cur->root[0]) ? cur->root : NULL;
	unsigned long rlen = root ? kstrlen(root) : 0, n = 0;
	char *j = w->next;
	char tmp[VFS_MAX_PATH];
	int trailing;

	/* The root counts only when the operation's path is inside it (it
	 * always is for a jailed task's own paths). */
	if (root && (rlen <= 1 || kstrncmp(op, root, rlen) != 0 ||
		     (op[rlen] != '/' && op[rlen] != '\0'))) {
		root = NULL;
		rlen = 0;
	}
#define HOP_PUT(c)                                                          \
	do {                                                                \
		if (n + 1 >= VFS_MAX_PATH)                                  \
			return ST_NAMETOOLONG;                              \
		j[n++] = (c);                                               \
	} while (0)
	if (target[0] != '/') {
		/* The directory holding the link, as the task sees it. */
		for (unsigned long i = rlen; i < ns_cs; i++)
			HOP_PUT(op[i]);
		if (n == 0 || j[n - 1] != '/')
			HOP_PUT('/');
	}
	for (const char *t = target; *t; t++)
		HOP_PUT(*t);
	for (const char *t = op + ns_end; *t; t++)
		HOP_PUT(*t);
	j[n] = '\0';
	trailing = n > 1 && j[n - 1] == '/';
	if (normalize_path("/", j, tmp, sizeof(tmp)) != 0)
		return ST_NAMETOOLONG;
	n = 0;
	if (root) {
		for (unsigned long i = 0; i < rlen; i++)
			HOP_PUT(root[i]);
		if (!(tmp[0] == '/' && tmp[1] == '\0'))
			for (const char *t = tmp; *t; t++)
				HOP_PUT(*t);
	} else {
		for (const char *t = tmp; *t; t++)
			HOP_PUT(*t);
	}
	if (trailing && !(n == 1 && j[0] == '/'))
		HOP_PUT('/');
	j[n] = '\0';
#undef HOP_PUT
	return ST_OK;
}

int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long comp_start,
		 unsigned long comp_end, const char *target)
{
	task_t *cur = sched_current();
	struct vfs_walk *w = cur ? (struct vfs_walk *)cur->vfs_walk : NULL;
	unsigned long mlen, ns_end;
	const char *op;

	if (!w || !m || !path || path[0] != '/' || !target ||
	    comp_start >= comp_end)
		return 0;
	/* Where the link is in the name space: the mount point, then the
	 * filesystem's own path.  It must lie on the operation's path, at a
	 * component boundary. */
	op = w->path;
	mlen = (m->full_paths || m->path_len <= 1) ? 0 : m->path_len;
	if (mlen && kstrncmp(op, m->path, mlen) != 0)
		return 0;
	if (kstrncmp(op + mlen, path, comp_end) != 0)
		return 0;
	ns_end = mlen + comp_end;
	if (op[ns_end] != '\0' && op[ns_end] != '/')
		return 0;
	/* The last component, which this operation does not follow (a
	 * trailing slash makes it a directory to look into: followed). */
	if (op[ns_end] == '\0' && !w->follow)
		return 0;
	if (w->hopped)
		return 1; /* going elsewhere already */
	w->hopped = 1;
	if (!target[0])
		w->err = ST_NOT_FOUND; /* an empty link leads nowhere */
	else if (m->flags & VFS_MS_NOSYMFOLLOW)
		w->err = ST_LOOP;
	else
		w->err = vfs_hop_join(w, cur, op, mlen + comp_start, ns_end,
				      target);
	return 1;
}

static int vfs_raw_stat(const char *path, struct kstat *st);
static int vfs_raw_lstat(const char *path, struct kstat *st);

/* `path' with every link on the way followed -- the last one too with
 * `follow' -- into `out' (VFS_MAX_PATH bytes): the name the object has with
 * no link left in it, as far as the path leads at all.  ST_OK, or the walk's
 * failure (ST_LOOP, ST_NAMETOOLONG). */
int vfs_resolve(const char *path, int follow, char *out)
{
	struct vfs_walk w;
	struct kstat st;
	unsigned long i;
	int r;

	VFS_WALK(w, path, follow, r,
		 follow ? vfs_raw_stat(path, &st) : vfs_raw_lstat(path, &st));
	/* (ST_LINK: nested in an operation that is about to restart on
	 * another path.) */
	if (r == ST_LOOP || r == ST_NAMETOOLONG || r == ST_LINK)
		return r;
	if (!path) {
		out[0] = '\0';
		return ST_INVALID;
	}
	for (i = 0; path[i] && i + 1 < VFS_MAX_PATH; i++)
		out[i] = path[i];
	out[i] = '\0';
	return ST_OK;
}

/* ============================ Permission layer ============================
 * The VFS is the canonical place for Unix discretionary access control: this
 * policy was consolidated out of the syscall layer so every filesystem — and
 * every path that reaches a file through the VFS — is checked uniformly.  The
 * ownership/mode/ACL data comes from the filesystem (stat + the ACL xattr); the
 * privileged caller (capable()) bypasses, which keeps the all-root boot and
 * every in-kernel VFS caller working unchanged.  Checks are permissive when a
 * path can't be resolved/stat'd, leaving the real op to report ENOENT/ENOTDIR. */

/* ---- POSIX access-ACL absence cache ----
 * A permission check on a non-root caller consults the filesystem for an access
 * ACL on every path component of every access.  That per-check inode/xattr read
 * made non-root file operations dramatically slower than root's (root bypasses
 * the whole check).  Access ACLs are rare — most inodes have none — so we
 * remember inodes proven to have no access ACL and skip the filesystem lookup
 * for them, running it at most once per inode (mirroring the kernel's i_acl
 * cache).  It is flushed whenever an ACL could have been added or removed. */
#define VFS_NOACL_CACHE 512
/* Keyed by the mount as well: inode numbers are only unique within one
 * filesystem, and an inode proven ACL-free on one must not vouch for the
 * inode with the same number on another. */
static unsigned long g_noacl_ino[VFS_NOACL_CACHE];
static const void *g_noacl_mnt[VFS_NOACL_CACHE];
static unsigned g_noacl_next;

static int noacl_cached(const void *mnt, unsigned long ino)
{
	if (ino == 0)
		return 0;
	for (unsigned i = 0; i < VFS_NOACL_CACHE; i++)
		if (g_noacl_ino[i] == ino && g_noacl_mnt[i] == mnt)
			return 1;
	return 0;
}

static void noacl_remember(const void *mnt, unsigned long ino)
{
	if (ino == 0 || noacl_cached(mnt, ino))
		return;
	g_noacl_ino[g_noacl_next] = ino;
	g_noacl_mnt[g_noacl_next] = mnt;
	g_noacl_next = (g_noacl_next + 1) % VFS_NOACL_CACHE;
}

static void vfs_acl_cache_flush(void)
{
	for (unsigned i = 0; i < VFS_NOACL_CACHE; i++) {
		g_noacl_ino[i] = 0;
		g_noacl_mnt[i] = NULL;
	}
	g_noacl_next = 0;
}

/* ---- directory-stat cache for the permission traverse ----
 * Path resolution is not cached, so the non-privileged permission traverse,
 * which re-resolves every ancestor prefix of every accessed path, was
 * O(depth^2) directory reads per open — invisible to root, which skips the
 * traverse entirely.  This caches the stat of recently-resolved paths so the
 * repeated ancestor lookups become cheap.  The cached metadata is
 * cred-independent (the per-caller permission decision is still computed fresh
 * from it), so the cache is shared across processes; a generation counter,
 * bumped by every metadata/namespace-changing VFS op, invalidates it. */
#define VFS_STATC 128
static struct {
	char path[256];
	struct kstat st;
	int valid;
} g_statc[VFS_STATC];
static unsigned g_statc_next;
static unsigned long g_meta_gen, g_statc_gen;

/* Invalidate the directory-stat cache: called by every op that changes a
 * file's metadata or the directory namespace. */
static void vfs_meta_bump(void)
{
	g_meta_gen++;
}

/* A mount or an unmount puts a different filesystem under some paths. */
void vfs_meta_changed(void)
{
	vfs_meta_bump();
	vfs_acl_cache_flush();
}

static int statc_get(const char *path, struct kstat *out)
{
	if (g_statc_gen != g_meta_gen) { /* stale: drop everything */
		for (unsigned i = 0; i < VFS_STATC; i++)
			g_statc[i].valid = 0;
		g_statc_gen = g_meta_gen;
		return 0;
	}
	for (unsigned i = 0; i < VFS_STATC; i++)
		if (g_statc[i].valid && kstrcmp(g_statc[i].path, path) == 0) {
			*out = g_statc[i].st;
			return 1;
		}
	return 0;
}

static void statc_put(const char *path, const struct kstat *st)
{
	size_t n = 0;
	while (path[n])
		n++;
	if (n >= sizeof(g_statc[0].path))
		return;
	unsigned idx = g_statc_next;
	g_statc_next = (g_statc_next + 1) % VFS_STATC;
	for (size_t i = 0; i <= n; i++)
		g_statc[idx].path[i] = path[i];
	g_statc[idx].st = *st;
	g_statc[idx].valid = 1;
}

/* stat used by the traverse: cache-first, so re-resolving shared ancestor
 * prefixes across many opens is cheap. */
static int traverse_stat(const char *path, struct kstat *st)
{
	if (statc_get(path, st))
		return ST_OK;
	int r = vfs_raw_stat(path, st);
	if (r == ST_OK)
		statc_put(path, st);
	return r;
}

/* Access check for a file whose stat is `st`, honouring a POSIX access ACL
 * (system.posix_acl_access) if present, else the mode bits.  `use_real` selects
 * the real vs effective/fs ids (access(2) uses the real ids).  ST_OK/ST_ACCESS. */
static int vfs_perm_access(const char *path, const struct kstat *st, int want,
			   int use_real)
{
	cred_t *c = current_cred();
	if (!c)
		return ST_OK; /* kernel context: privileged */
	uint32_t cuid = use_real ? c->uid : c->euid;
	const void *mkey = NULL;
	if (cuid != 0) {
		/* Only the identity is wanted, not a hold. */
		vfs_mount_t *km = vfs_mnt_get(path, NULL);
		mkey = km;
		vfs_mnt_put(km);
	}
	if (cuid != 0 && !noacl_cached(mkey, (unsigned long)st->st_ino)) {
		unsigned char acl[512];
		int n = vfs_getxattr_ino(path, (unsigned long)st->st_ino,
					 "system.posix_acl_access", acl,
					 sizeof(acl));
		if (n > 0) {
			int r = cred_acl_access(c, acl, (unsigned)n,
						(uint32_t)st->st_uid,
						(uint32_t)st->st_gid, want,
						use_real);
			if (r <= 0)
				return (r == 0) ? ST_OK :
						  ST_ACCESS; /* ACL decided */
			/* r == 1: no usable ACL -> fall through to mode bits */
		} else if (n == ST_NODATA) {
			/* Proven to have no access ACL: skip the lookup next
			 * time.  (Other errors are not cached.) */
			noacl_remember(mkey, (unsigned long)st->st_ino);
		}
	}
	int r = use_real ? cred_check_access_real(c, (uint32_t)st->st_mode,
						  (uint32_t)st->st_uid,
						  (uint32_t)st->st_gid, want) :
			   cred_check_access(c, (uint32_t)st->st_mode,
					     (uint32_t)st->st_uid,
					     (uint32_t)st->st_gid, want);
	return (r == 0) ? ST_OK : ST_ACCESS;
}

/* Permission decision against an already-resolved stat: the filesystem's own
 * hook first (e.g. FAT32 emulates permissive), then the generic ACL/mode-bit
 * check.  Callers that just stat'd the target use this directly to avoid
 * re-resolving the path. */
static int vfs_permission_st(const char *path, const struct kstat *st, int want,
			     int use_real)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	if (m && m->ops->permission) {
		int fr = m->ops->permission(m, rel, (unsigned long)st->st_ino,
					    want);
		if (fr != ST_UNSUPPORTED) {
			vfs_mnt_put(m);
			return fr;
		}
	}
	vfs_mnt_put(m);
	return vfs_perm_access(path, st, want, use_real);
}

/* Shared worker for vfs_permission (use_real=0) and vfs_access (use_real=1). */
static int vfs_permission_id(const char *path, int want, int use_real)
{
	BUG_ON(path == NULL);
	struct kstat st;
	int sr = vfs_raw_stat(path, &st);
	if (sr == ST_LINK)
		return ST_LINK; /* the walk goes on elsewhere */
	if (sr != ST_OK)
		return ST_OK; /* unresolved: defer to the real op */
	return vfs_permission_st(path, &st, want, use_real);
}

int vfs_permission(const char *path, int want)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_permission_id(path, want, 0));
	return r;
}

/* Access check against an already-resolved stat (no path walk), honouring the
 * ACL then the mode bits.  Lets a caller that just stat'd a file check it
 * without re-resolving.  `use_real` selects the real vs effective/fs ids. */
int vfs_check_access(const char *path, const struct kstat *st, int want,
		     int use_real)
{
	char cbuf_[VFS_MAX_PATH];
	cred_t *c = current_cred();
	path = vfs_canon(path, cbuf_);
	BUG_ON(path == NULL);
	BUG_ON(st == NULL);
	/* For a caller the ACL applies to, the ACL is read from the
	 * filesystem `st' came from: the one the path leads to once its
	 * links are followed -- the last one too, unless `st' describes the
	 * link itself. */
	if (c && (use_real ? c->uid : c->euid) != 0 &&
	    vfs_resolve(path, !S_ISLNK(st->st_mode), cbuf_) == ST_OK)
		path = cbuf_;
	return vfs_perm_access(path, st, want, use_real);
}

/* Access check of an OPEN file against its own stat (effective ids): the
 * ACL is read from the handle, not looked up again by name, so the decision
 * is about exactly the file that was opened -- the exec path relies on this
 * (a name can be pointed somewhere else between two lookups). */
int vfs_check_access_file(vfs_file_t *f, const struct kstat *st, int want)
{
	cred_t *c = current_cred();

	BUG_ON(st == NULL);
	if (!c)
		return ST_OK; /* kernel context: privileged */
	if (c->euid != 0) {
		unsigned char acl[512];
		int n = vfs_fgetxattr(f, "system.posix_acl_access", acl,
				      sizeof(acl));

		if (n > 0) {
			int r = cred_acl_access(c, acl, (unsigned)n,
						(uint32_t)st->st_uid,
						(uint32_t)st->st_gid, want, 0);
			if (r <= 0)
				return (r == 0) ? ST_OK : ST_ACCESS;
			/* r == 1: no usable ACL -> the mode bits decide */
		}
	}
	return cred_check_access(c, (uint32_t)st->st_mode,
				 (uint32_t)st->st_uid, (uint32_t)st->st_gid,
				 want) == 0 ?
		       ST_OK :
		       ST_ACCESS;
}

/* The owning filesystem's own access decision for `path' (its ->permission
 * hook): ST_OK, a refusal, or ST_UNSUPPORTED when it has none or defers. */
int vfs_fs_permission(const char *path, unsigned long ino, int want)
{
	char cbuf_[VFS_MAX_PATH];
	path = vfs_canon(path, cbuf_);
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->permission)
		r = m->ops->permission(m, rel, ino, want);
	vfs_mnt_put(m);
	return r;
}

/* Return `path` made absolute in `buf` by joining the current task cwd when it
 * is relative (else `path` unchanged).  The VFS permission helpers below walk
 * the path textually and assume an absolute path; some syscalls hand the VFS a
 * cwd-relative path directly (there are no *at dirfd variants that reach here
 * unresolved), so resolving here keeps the DAC checks from being silently
 * skipped for a non-root caller. */
static const char *vfs_abspath(const char *path, char *buf, size_t bufsz)
{
	if (!path || path[0] == '/')
		return path;
	const char *cwd = current_cwd(); /* absolute; "/" if unset */
	size_t ci = 0;
	while (cwd[ci] && ci < bufsz - 2) {
		buf[ci] = cwd[ci];
		ci++;
	}
	if (ci == 0)
		buf[ci++] = '/';
	if (buf[ci - 1] != '/')
		buf[ci++] = '/';
	size_t pi = 0;
	while (path[pi] && ci < bufsz - 1)
		buf[ci++] = path[pi++];
	buf[ci] = '\0';
	return buf;
}

/* Per-component traversal: a non-privileged task needs search (x) on every
 * ancestor directory of `path`.  Walks "/", "/a", "/a/b" for "/a/b/c".
 * No-op for the privileged caller. */
static int vfs_permission_traverse_id(const char *path, int use_real)
{
	cred_t *c = current_cred();
	if (!c)
		return ST_OK;
	uint32_t cuid = use_real ? c->uid : c->euid;
	if (cuid == 0)
		return ST_OK; /* privileged: bypass */
	if (!path)
		return ST_OK;
	char abuf[VFS_MAX_PATH];
	path = vfs_abspath(path, abuf, sizeof(abuf));
	size_t len = 0;
	while (path[len])
		len++;
	while (len > 1 && path[len - 1] == '/')
		len--; /* ignore a trailing slash */
	if (len <= 1)
		return ST_OK; /* "/" has no ancestors */
	size_t last = 0;
	for (size_t i = 0; i < len; i++)
		if (path[i] == '/')
			last = i;
	char dir[VFS_MAX_PATH];
	for (size_t i = 0; i <= last; i++) { /* ancestors only */
		if (path[i] != '/')
			continue;
		if (i == 0) {
			dir[0] = '/';
			dir[1] = '\0';
		} else {
			if (i >= sizeof(dir))
				return ST_OK;
			for (size_t j = 0; j < i; j++)
				dir[j] = path[j];
			dir[i] = '\0';
		}
		struct kstat st;
		int sr = traverse_stat(dir, &st);
		if (sr == ST_LINK)
			return ST_LINK; /* the walk goes on elsewhere */
		if (sr != ST_OK)
			continue; /* defer to the resolver */
		if ((st.st_mode & S_IFMT) != S_IFDIR)
			continue;
		int pr = vfs_perm_access(dir, &st, MAY_EXEC, use_real);
		if (pr != ST_OK)
			return pr;
	}
	return ST_OK;
}

int vfs_permission_traverse(const char *path)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_permission_traverse_id(path, 0));
	return r;
}

static int vfs_access_id(const char *path, int want)
{
	int tr = vfs_permission_traverse_id(path, 1);
	if (tr != ST_OK)
		return tr;
	return vfs_permission_id(path, want, 1);
}

int vfs_access(const char *path, int want)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_access_id(path, want));
	return r;
}

/* Prefix traversal with an explicit real/effective id selection, exposed so the
 * access(2)/faccessat syscalls can search the prefix separately from the final
 * access check (they need to distinguish F_OK reachability and ENOENT). */
int vfs_access_traverse(const char *path, int use_real)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_permission_traverse_id(path, use_real));
	return r;
}

/* Absolute parent directory of `path` into `out`.  Returns 1, or 0 when there's
 * no directory component or it won't fit.  Assumes an absolute path. */
static int vfs_parent_of(const char *path, char *out, size_t outsz)
{
	if (!path || path[0] != '/')
		return 0;
	size_t len = 0;
	while (path[len])
		len++;
	while (len > 1 && path[len - 1] == '/')
		len--;
	size_t slash = (size_t)-1;
	for (size_t i = 0; i < len; i++)
		if (path[i] == '/')
			slash = i;
	if (slash == (size_t)-1)
		return 0;
	if (slash == 0) {
		out[0] = '/';
		out[1] = '\0';
	} else {
		if (slash >= outsz)
			return 0;
		for (size_t i = 0; i < slash; i++)
			out[i] = path[i];
		out[slash] = '\0';
	}
	return 1;
}

/* Access to the PARENT directory of `path` — for create/remove/rename, which
 * modify the containing directory (want is typically MAY_WRITE|MAY_EXEC). */
static int vfs_permission_parent_at(const char *path, int want)
{
	if (capable())
		return ST_OK;
	char abuf[VFS_MAX_PATH];
	path = vfs_abspath(path, abuf, sizeof(abuf)); /* parent-of needs absolute */
	char parent[VFS_MAX_PATH];
	if (!vfs_parent_of(path, parent, sizeof(parent)))
		return ST_OK;
	int tr = vfs_permission_traverse_id(parent, 0);
	if (tr != ST_OK)
		return tr;
	return vfs_permission_id(parent, want, 0);
}

int vfs_permission_parent(const char *path, int want)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_permission_parent_at(path, want));
	return r;
}

/* Remove/rename gate: parent write+search, plus the sticky bit (S_ISVTX, e.g.
 * /tmp at 1777) — a non-privileged task may then only remove/rename an entry it
 * owns, or when it owns the containing directory.  The entry is the name
 * itself: a symbolic link's own owner counts, not its target's. */
static int vfs_permission_remove_at(const char *path)
{
	if (capable())
		return ST_OK;
	char abuf[VFS_MAX_PATH];
	path = vfs_abspath(path, abuf, sizeof(abuf)); /* parent-of / raw_stat need it */
	int pr = vfs_permission_parent_at(path, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	char parent[VFS_MAX_PATH];
	if (!vfs_parent_of(path, parent, sizeof(parent)))
		return ST_OK;
	struct kstat pst;
	int sr = vfs_raw_stat(parent, &pst);
	if (sr == ST_LINK)
		return ST_LINK;
	if (sr != ST_OK)
		return ST_OK;
	if (!(pst.st_mode & S_ISVTX))
		return ST_OK; /* no sticky bit: parent write is enough */
	struct kstat st;
	sr = vfs_raw_lstat(path, &st);
	if (sr == ST_LINK)
		return ST_LINK;
	if (sr != ST_OK)
		return ST_OK;
	uint32_t me = current_fsuid();
	if ((uint32_t)st.st_uid == me || (uint32_t)pst.st_uid == me)
		return ST_OK;
	return ST_PERM;
}

int vfs_permission_remove(const char *path)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_permission_remove_at(path));
	return r;
}

/* Veto a modification forbidden by immutable/append-only inode flags, which
 * bind regardless of ownership (even the owner and the privileged caller must
 * clear the flag first).  `is_append_write` is 1 only for an append-mode write
 * (the lone modification an append-only file permits).  No-op for a filesystem
 * without the inode_flags op. */
static int vfs_attr_allow_modify_ino(vfs_mount_t *m, unsigned long ino,
				     int is_append_write)
{
	if (!m || !m->ops->inode_flags)
		return ST_OK;
	uint32_t fl = 0;
	if (m->ops->inode_flags(m, ino, &fl) != ST_OK)
		return ST_OK;
	if (fl & VFS_ATTR_IMMUTABLE)
		return ST_PERM;
	if ((fl & VFS_ATTR_APPEND) && !is_append_write)
		return ST_PERM;
	return ST_OK;
}

static int vfs_attr_allow_modify(const char *path, int is_append_write)
{
	vfs_mount_t *m = vfs_mnt_get(path, NULL);
	int r = ST_OK;

	if (m && m->ops->inode_flags) {
		struct kstat st;
		int sr = vfs_raw_stat(path, &st);
		if (sr == ST_OK)
			r = vfs_attr_allow_modify_ino(
				m, (unsigned long)st.st_ino, is_append_write);
		else if (sr == ST_LINK)
			r = ST_LINK; /* the walk goes on elsewhere */
	}
	vfs_mnt_put(m);
	return r;
}

/* A modification on a read-only mount. */
static int vfs_rofs(const vfs_mount_t *m)
{
	return (m && (m->flags & VFS_MS_RDONLY)) ? ST_ROFS : ST_OK;
}

/* Report the mount's device number where the mount asks for it. */
static void vfs_fix_dev(const vfs_mount_t *m, struct kstat *st)
{
	if (m && m->dev_override)
		st->st_dev = m->dev;
}

/* ================================================================== */

int vfs_init(void)
{
	/* The mount table is static (kernel/fs/mount.c): filesystems arrive
	 * through vfs_register_root(), vfs_register_devfs(),
	 * vfs_register_mount(), vfs_mount_boot() and mount(2).  These are the
	 * disk filesystems mount(2) can make; tmpfs registers itself with its
	 * boot instance (/ram). */
	vfs_fstype_register(&ext4_fstype);
	vfs_fstype_register(&fat32_fstype);
	return ST_OK;
}

static int vfs_is_dev_path(const char *path)
{
	if (!path)
		return 0;
	if (path[0] != '/' || path[1] != 'd' || path[2] != 'e' ||
	    path[3] != 'v')
		return 0;
	if (path[4] == '\0' || path[4] == '/')
		return 1;
	return 0;
}


/* Remember the absolute path a handle was opened with, so the descriptor can
 * later serve as the dirfd of an *at() syscall.
 *
 * Stored for every handle rather than only for directories: telling the two
 * apart here would mean an extra fstat on every open, and open() is already
 * doing path resolution and disk I/O -- a short string copy is the cheaper of
 * the two.  A failed allocation is not an error: at_path stays NULL and the
 * *at() syscalls then report ENOTDIR for that descriptor, which is what they
 * did for every descriptor before this existed. */
static void vfs_set_at_path(vfs_file_t *f, const char *path)
{
	unsigned long n = 0;

	if (!f)
		return;
	f->at_path = NULL;
	f->fsn_ino = 0;
	f->fsn_state = 0;
	f->fsn_isdir = 0;
	if (!path)
		return;
	while (path[n])
		n++;
	if (n == 0 || n >= VFS_MAX_PATH)
		return;
	f->at_path = (char *)kalloc(n + 1);
	if (!f->at_path)
		return;
	for (unsigned long i = 0; i <= n; i++)
		f->at_path[i] = path[i];
}

/* Open the /dev node `dpath' as an open of it by name does: through devfs
 * with the opening task, which is what makes a terminal the controlling one
 * (O_NOCTTY respected) and resolves /dev/tty to the caller's own.  A
 * non-privileged caller without a controlling terminal has no /dev/tty to
 * open (ENXIO), as when it names /dev/tty itself (devfs_open_perm). */
static int vfs_open_devfs(const char *dpath, int flags, unsigned int cmode,
			  vfs_file_t **out)
{
	task_t *cur = sched_current();

	if (cur && cur->cred.euid != 0 && kstrcmp(dpath, "/dev/tty") == 0 &&
	    (!cur->ctty || cur->ctty->sid != cur->sid))
		return ST_NXIO;
	return devfs_open_for_task(dpath, flags, cmode, out, cur);
}

/* Shared body of vfs_open() and vfs_open_mode().
 *
 * `cmode` is the mode a newly created file gets (already umask-adjusted by the
 * caller).  It is only consulted when the filesystem provides ->open_mode and
 * the open actually creates something. */
static int vfs_open_common(const char *path, int flags, unsigned int cmode,
			   vfs_file_t **out, int *existed)
{
	BUG_ON(path == NULL);
	BUG_ON(out == NULL);
	struct kstat est;
	int exists;

	/* Permission enforcement (canonical for every filesystem): search the
	 * path prefix, then check read/write on an existing target — or write on
	 * the parent directory when creating — plus any immutable/append-only
	 * inode flag.  Decided here so read()/write() need not re-check: the open
	 * mode the fd carries already encodes the granted access. */
	int acc = flags & 3;
	int want = 0;
	{
		int tr = vfs_permission_traverse_id(path, 0);
		if (tr != ST_OK)
			return tr;
		/* A symbolic link as the last component: O_CREAT|O_EXCL
		 * finds a name that exists, wherever it leads; O_NOFOLLOW
		 * refuses to open it.  (The walk does not follow it for
		 * either: see vfs_open_follows.) */
		if ((flags & O_NOFOLLOW) ||
		    ((flags & O_CREAT) && (flags & O_EXCL))) {
			struct kstat lst;
			int lr = vfs_raw_lstat(path, &lst);

			if (lr == ST_LINK)
				return ST_LINK;
			if (lr == ST_OK && S_ISLNK(lst.st_mode))
				return ((flags & O_CREAT) && (flags & O_EXCL)) ?
					       ST_EXISTS :
					       ST_LOOP;
		}
		/* Access mode 3 opens for ioctls only, and needs read AND
		 * write permission for it -- it is not a way around both:
		 * read() and write() refuse such a descriptor (read_write.c). */
		if (acc == O_RDONLY || acc == O_RDWR || acc == 3)
			want |= MAY_READ;
		if (acc == O_WRONLY || acc == O_RDWR || acc == 3)
			want |= MAY_WRITE;
		if (flags & O_TRUNC)
			want |= MAY_WRITE;
		int sr = vfs_raw_stat(path, &est);
		if (sr == ST_LINK)
			return ST_LINK; /* the walk goes on elsewhere */
		exists = (sr == ST_OK);
		*existed = exists;
		if (exists) {
			/*
			 * O_CREAT|O_EXCL means "create this, and fail if it is
			 * already there".  It was accepted and ignored, so such
			 * an open SUCCEEDED on an existing file -- and that is
			 * the one guarantee every unique-temporary-file scheme
			 * rests on.  mkstemp() generates a candidate name, opens
			 * it this way, and treats success as proof the name was
			 * its own; with the check missing, the very first
			 * candidate always "succeeded" and every caller in the
			 * process got THE SAME temporary file.
			 *
			 * A mail client encoding a message showed it plainly:
			 * the body part and the attachment part were written to
			 * one file, so the attachment arrived containing the
			 * message text.  Lock files, "atomic" create-then-rename
			 * and anything else built on this primitive were equally
			 * unprotected.
			 *
			 * Decided before the access checks below: the file's own
			 * permissions do not matter when the answer is that it
			 * exists at all.
			 */
			if ((flags & O_CREAT) && (flags & O_EXCL))
				return ST_EXISTS;
			if (want) {
				/* est was just resolved: decide on it directly
				 * instead of re-resolving the path. */
				int pr = vfs_permission_st(path, &est, want, 0);
				if (pr != ST_OK)
					return pr;
			}
			if (want & MAY_WRITE) {
				int append_ok = ((flags & O_APPEND) &&
						 !(flags & O_TRUNC)) ?
							1 :
							0;
				vfs_mount_t *im = vfs_mnt_get(path, NULL);
				int r = vfs_attr_allow_modify_ino(
					im, (unsigned long)est.st_ino,
					append_ok);
				vfs_mnt_put(im);
				if (r != ST_OK)
					return r;
			}
		} else if (flags & O_CREAT) {
			int pr = vfs_permission_parent_at(path,
							  MAY_WRITE | MAY_EXEC);
			if (pr != ST_OK)
				return pr;
		}
	}

	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	if (!m)
		return ST_UNSUPPORTED;
	int ret;

	/* A read-only mount: no writing, no creating, no truncating. */
	if ((m->flags & VFS_MS_RDONLY) &&
	    ((want & MAY_WRITE) || ((flags & O_CREAT) && !exists))) {
		ret = ST_ROFS;
		goto out;
	}

	/* A device node kept on an ordinary filesystem opens the device it
	 * names: the driver is found by its number, among the nodes devfs
	 * has, and the open is devfs's -- with the opening task, as an open
	 * of the /dev node by name has it, so a terminal becomes the
	 * controlling one (unless O_NOCTTY).  The same for a path that a
	 * symbolic link led into /dev.  The path the caller used stays the
	 * handle's own (at_path below). */
	{
		char dpath[VFS_MAX_PATH];
		const char *devp = NULL;

		if (exists && !m->full_paths &&
		    (S_ISCHR(est.st_mode) || S_ISBLK(est.st_mode))) {
			if (m->flags & VFS_MS_NODEV) {
				ret = ST_ACCESS;
				goto out;
			}
			if (devfs_path_for_rdev(S_ISBLK(est.st_mode) ? 1 : 0,
						est.st_rdev, dpath,
						sizeof(dpath)) != ST_OK) {
				ret = ST_NO_DEVICE;
				goto out;
			}
			devp = dpath;
		} else if (m->ops == devfs_get_ops()) {
			devp = path;
		}
		if (!devp)
			goto not_device;
		vfs_mount_t *dm = vfs_mnt_get(devp, &rel);
		if (!dm || dm->ops != devfs_get_ops()) {
			vfs_mnt_put(dm);
			ret = ST_NO_DEVICE;
			goto out;
		}
		ret = vfs_open_devfs(devp, flags, cmode, out);
		if (ret == ST_OK && *out) {
			(*out)->refcount = 1;
			(*out)->flags = flags;
			(*out)->is_root_dir = 0;
			(*out)->dev_injected = 0;
			vfs_set_at_path(*out, path);
			(*out)->mnt = dm;
			vfs_mnt_file_opened(dm);
			if (file_may_write(*out))
				__atomic_add_fetch(&dm->open_writers, 1,
						   __ATOMIC_SEQ_CST);
		}
		vfs_mnt_put(dm);
		goto out;
	}
not_device:

	if (!m->ops->open && !m->ops->open_mode) {
		ret = ST_UNSUPPORTED;
		goto out;
	}
	/* ->open_mode when the filesystem has it, so an O_CREAT open creates
	 * with the mode the caller asked for instead of a fixed default. */
	ret = m->ops->open_mode ? m->ops->open_mode(m, rel, flags, cmode, out) :
				  m->ops->open(m, rel, flags, out);
	if (ret == ST_OK && *out) {
		WARN_ON((*out)->ops == NULL);
		(*out)->refcount = 1;
		(*out)->flags = flags;
		(*out)->is_root_dir = (m == vfs_mnt_root() && path[0] == '/' &&
				       path[1] == '\0');
		(*out)->dev_injected = 0;
		vfs_set_at_path(*out, path);
		(*out)->mnt = m;
		vfs_mnt_file_opened(m);
		if (file_may_write(*out))
			__atomic_add_fetch(&m->open_writers, 1,
					   __ATOMIC_SEQ_CST);
	}
	WARN_ON(ret == ST_OK && *out == NULL);
out:
	vfs_mnt_put(m);
	return ret;
}

/* Is a symbolic link as the last component followed by this open?  Not
 * with O_NOFOLLOW, nor by O_CREAT|O_EXCL (the name exists). */
static int vfs_open_follows(int flags)
{
	return !(flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL));
}

int vfs_open_mode(const char *path, int flags, unsigned int mode,
		  vfs_file_t **out)
{
	struct vfs_walk w;
	int existed = 1;
	int r;

	VFS_WALK(w, path, vfs_open_follows(flags), r,
		 vfs_open_common(path, flags, mode, out, &existed));
	if (r == ST_OK && *out)
		vfs_notify_open(path, flags, existed, *out);
	return r;
}

/* Plain open().  Used by every in-kernel opener, none of which creates a file
 * with a caller-chosen mode; 0666 is the POSIX default for a creat-style open
 * and is only reached if such a caller ever does pass O_CREAT. */
int vfs_open(const char *path, int flags, vfs_file_t **out)
{
	return vfs_open_mode(path, flags, 0666, out);
}

/* Raw stat: dispatch to the owning filesystem with no permission check.  Used
 * by the permission layer (which would otherwise recurse through vfs_stat). */
static int vfs_raw_stat(const char *path, struct kstat *st)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->stat) {
		r = m->ops->stat(m, rel, st);
		if (r == ST_OK)
			vfs_fix_dev(m, st);
	}
	vfs_mnt_put(m);
	return r;
}

/* The same, not following a final symlink. */
static int vfs_raw_lstat(const char *path, struct kstat *st)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m) {
		if (m->ops->lstat)
			r = m->ops->lstat(m, rel, st);
		else if (m->ops->stat)
			r = m->ops->stat(m, rel, st);
		if (r == ST_OK)
			vfs_fix_dev(m, st);
	}
	vfs_mnt_put(m);
	return r;
}

static int vfs_stat_at(const char *path, struct kstat *st)
{
	/* stat(2) requires search (x) permission on every directory in the
	 * prefix; the file's own permissions are not consulted. */
	int tr = vfs_permission_traverse_id(path, 0);
	if (tr != ST_OK)
		return tr;
	return vfs_raw_stat(path, st);
}

int vfs_stat(const char *path, struct kstat *st)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_stat_at(path, st));
	return r;
}

static int vfs_chdir_at(const char *path)
{
	/* Need search (x) on the prefix and on the target directory itself. */
	int tr = vfs_permission_traverse_id(path, 0);
	if (tr != ST_OK)
		return tr;
	int pr = vfs_permission_id(path, MAY_EXEC, 0);
	if (pr != ST_OK)
		return pr;
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->chdir)
		r = m->ops->chdir(m, rel);
	vfs_mnt_put(m);
	return r;
}

int vfs_chdir(const char *path)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_chdir_at(path));
	return r;
}

/* ---- change notification ----------------------------------------------------
 * What happened is reported after it succeeded (kernel/fs/fsnotify.h).  An
 * identity is only looked up while something is watched; with no watch
 * anywhere every hook below costs one load. */

static int vfs_notify_ident_at(const char *path, int follow,
			       struct fsnotify_ident *id)
{
	struct kstat st;
	int r;

	id->valid = 0;
	id->isdir = 0;
	/* The mount is the filesystem's identity: two mounted instances of
	 * one driver have overlapping inode numbers. */
	vfs_mount_t *m = vfs_mnt_get(path, NULL);
	vfs_mnt_put(m);
	if (!m)
		return ST_UNSUPPORTED;
	r = follow ? vfs_raw_stat(path, &st) : vfs_raw_lstat(path, &st);
	if (r != ST_OK)
		return r;
	id->fs = m;
	id->ino = st.st_ino;
	id->nlink = (uint32_t)st.st_nlink;
	id->isdir = S_ISDIR(st.st_mode) ? 1 : 0;
	id->valid = 1;
	return ST_OK;
}

int vfs_notify_ident(const char *path, int follow, struct fsnotify_ident *id)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, follow, r, vfs_notify_ident_at(path, follow, id));
	return r;
}

/* The identity of what `path' names, when anything is watched at all. */
static void vfs_notify_path_ident(const char *path, int follow,
				  struct fsnotify_ident *id)
{
	id->valid = 0;
	id->isdir = 0;
	if (fsnotify_wants(~0u))
		(void)vfs_notify_ident(path, follow, id);
}

/* The identity of the object an open handle is on.  The inode number is
 * asked for once and kept in the handle.  Devices report nothing: a terminal
 * written a line at a time would otherwise pay for a lookup per write. */
static int vfs_notify_file_ident(vfs_file_t *f, struct fsnotify_ident *id)
{
	id->valid = 0;
	id->isdir = 0;
	/* Read once, with acquire: the release store below publishes fsn_ino
	 * and fsn_isdir, and through a plain load an optimising compiler may
	 * read those before the state that says they are there. */
	int fsn_state = __atomic_load_n(&f->fsn_state, __ATOMIC_ACQUIRE);

	if (!f->at_path || fsn_state < 0)
		return 0;
	if (fsn_state == 0) {
		struct kstat st;

		if (vfs_is_dev_path(f->at_path) ||
		    !f->ops->fstat || f->ops->fstat(f, &st) != ST_OK) {
			f->fsn_state = -1;
			return 0;
		}
		f->fsn_ino = st.st_ino;
		f->fsn_isdir = S_ISDIR(st.st_mode) ? 1 : 0;
		__atomic_store_n(&f->fsn_state, 1, __ATOMIC_RELEASE);
	}
	if (f->mnt) {
		id->fs = f->mnt;
	} else {
		vfs_mount_t *m = vfs_mnt_get(f->at_path, NULL);
		vfs_mnt_put(m);
		id->fs = m;
	}
	id->ino = f->fsn_ino;
	id->nlink = 0;
	id->isdir = f->fsn_isdir;
	id->valid = 1;
	return 1;
}

/* `mask' about the object behind an open handle. */
static void vfs_notify_file(vfs_file_t *f, uint32_t mask)
{
	struct fsnotify_ident id;

	if (!fsnotify_wants(mask) || !f->at_path)
		return;
	if (vfs_notify_file_ident(f, &id))
		fsnotify_event(f->at_path, &id, mask, 0, 1);
}

/* A successful open: IN_CREATE when it made the file, IN_MODIFY when O_TRUNC
 * emptied an existing one, and IN_OPEN. */
static void vfs_notify_open(const char *path, int flags, int existed,
			    vfs_file_t *f)
{
	struct fsnotify_ident id;

	if (!fsnotify_wants(IN_CREATE | IN_MODIFY | IN_OPEN))
		return;
	vfs_notify_file_ident(f, &id);
	if (!existed && (flags & O_CREAT))
		fsnotify_event(path, &id, IN_CREATE, 0, 0);
	else if ((flags & O_TRUNC) && file_may_write(f))
		fsnotify_event(path, &id, IN_MODIFY, 0, 0);
	fsnotify_event(path, &id, IN_OPEN, 0, 0);
}

/* A name was made: IN_CREATE to its directory. */
static void vfs_notify_created(const char *path, int isdir)
{
	struct fsnotify_ident id;

	if (!fsnotify_wants(IN_CREATE))
		return;
	id.valid = 0;
	id.isdir = isdir ? 1 : 0;
	fsnotify_event(path, &id, IN_CREATE, 0, 0);
}

/* A name was removed.  `id' was taken before the removal.  The directory
 * hears IN_DELETE; the object loses its watches if that was its last name,
 * and otherwise learns that its link count changed. */
static void vfs_notify_removed(const char *path, const struct fsnotify_ident *id,
			       int isdir)
{
	if (!fsnotify_wants(~0u))
		return;
	struct fsnotify_ident d = *id;

	if (isdir)
		d.isdir = 1;
	fsnotify_event(path, &d, IN_DELETE, 0, 0);
	if (!d.valid)
		return;
	if (d.isdir || d.nlink <= 1)
		fsnotify_gone(&d);
	else
		fsnotify_event(NULL, &d, IN_ATTRIB, 0, 0);
}

/* IN_ATTRIB about what `path' names. */
static void vfs_notify_attrib(const char *path, int follow)
{
	struct fsnotify_ident id;

	if (!fsnotify_wants(IN_ATTRIB))
		return;
	vfs_notify_path_ident(path, follow, &id);
	fsnotify_event(path, &id, IN_ATTRIB, 0, 0);
}


/* ---- UNIX-semantics wrappers ------------------------------------------------
 * Each dispatches to the owning filesystem's op and supplies a legacy fallback
 * when that op is NULL, so the syscall layer stays filesystem-agnostic. */

static int vfs_lstat_at(const char *path, struct kstat *st)
{
	int tr = vfs_permission_traverse_id(path, 0); /* search the prefix */
	if (tr != ST_OK)
		return tr;
	return vfs_raw_lstat(path, st);
}

int vfs_lstat(const char *path, struct kstat *st)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_lstat_at(path, st));
	return r;
}

static int vfs_symlink_at(const char *target, const char *linkpath)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(linkpath, &rel);
	int r;

	if (!m || !m->ops->symlink) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_parent_at(linkpath, MAY_WRITE | MAY_EXEC);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_meta_bump();
	r = m->ops->symlink(m, target, rel);
	if (r == ST_OK)
		vfs_notify_created(linkpath, 0);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_symlink(const char *target, const char *linkpath)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, linkpath, 0, r, vfs_symlink_at(target, linkpath));
	return r;
}

static int vfs_readlink_at(const char *path, char *buf, unsigned long bufsz)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_INVALID; /* not a symlink here */

	if (m && m->ops->readlink)
		r = m->ops->readlink(m, rel, buf, bufsz);
	vfs_mnt_put(m);
	return r;
}

int vfs_readlink(const char *path, char *buf, unsigned long bufsz)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_readlink_at(path, buf, bufsz));
	return r;
}

/* A path for an operation on two names (link, rename), which the VFS cannot
 * restart on one of them: every link on the way to the last component is
 * followed here, beforehand -- and the last one too with `follow' -- so the
 * filesystem finds none.  Into `out'; `path' itself when nothing changed. */
static int vfs_two_name_path(const char *path, int follow, char *out,
			     const char **res)
{
	int r = vfs_resolve(path, follow, out);

	if (r == ST_LINK) {
		/* Nested in an operation that is about to restart. */
		*res = vfs_canon(path, out);
		return ST_OK;
	}
	*res = out;
	return r;
}

int vfs_link(const char *oldpath, const char *newpath, int follow)
{
	char obuf_[VFS_MAX_PATH], nbuf_[VFS_MAX_PATH];
	int r;

	r = vfs_two_name_path(oldpath, follow, obuf_, &oldpath);
	if (r == ST_OK)
		r = vfs_two_name_path(newpath, 0, nbuf_, &newpath);
	if (r != ST_OK)
		return r;
	/* A hard link's two ends share an inode, so both live on one
	 * filesystem: one mount, or EXDEV. */
	const char *orel, *nrel;
	vfs_mount_t *mo = vfs_mnt_get(oldpath, &orel);
	vfs_mount_t *m = vfs_mnt_get(newpath, &nrel);

	if (!m || !m->ops->link) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	if (mo != m) {
		r = ST_XDEV;
		goto out;
	}
	/* Need to reach the existing inode and write the new name's directory. */
	r = vfs_permission_traverse_id(oldpath, 0);
	if (r == ST_OK)
		r = vfs_permission_parent_at(newpath, MAY_WRITE | MAY_EXEC);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_meta_bump();
	r = m->ops->link(m, orel, nrel, follow);
	if (r == ST_OK) {
		vfs_notify_created(newpath, 0);
		vfs_notify_attrib(newpath, 0); /* the link count went up */
	}
out:
	vfs_mnt_put(m);
	vfs_mnt_put(mo);
	return r;
}

static int vfs_chmod_at(const char *path, unsigned int mode)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_traverse_id(path, 0);
	if (r != ST_OK)
		goto out;
	if (!capable()) {
		struct kstat st;
		int sr = vfs_raw_stat(path, &st);
		if (sr == ST_LINK) {
			r = ST_LINK;
			goto out;
		}
		if (sr == ST_OK) {
			/* Only the file's owner may change its mode. */
			if ((uint32_t)st.st_uid != current_fsuid()) {
				r = ST_PERM;
				goto out;
			}
			/* A non-privileged owner cannot set the set-group-ID bit
			 * for a group it is not a member of. */
			if ((mode & S_ISGID) &&
			    !current_in_group((uint32_t)st.st_gid))
				mode &= ~(unsigned)S_ISGID;
		}
	}
	r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	if (!m->ops->chmod)
		goto out; /* fs has no permission bits */
	vfs_meta_bump();
	r = m->ops->chmod(m, rel, mode);
	if (r == ST_OK)
		vfs_notify_attrib(path, 1);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_chmod(const char *path, unsigned int mode)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_chmod_at(path, mode));
	return r;
}

static int vfs_chown_at(const char *path, int uid, int gid, int nofollow)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_traverse_id(path, 0);
	if (r != ST_OK)
		goto out;
	if (!capable()) {
		struct kstat st;
		int sr = nofollow ? vfs_raw_lstat(path, &st) :
				    vfs_raw_stat(path, &st);
		if (sr == ST_LINK) {
			r = ST_LINK;
			goto out;
		}
		if (sr == ST_OK) {
			/* Changing the owner is privileged. */
			if (uid != -1 && (uint32_t)uid != (uint32_t)st.st_uid) {
				r = ST_PERM;
				goto out;
			}
			/* Changing the group requires owning the file and being
			 * a member of the target group. */
			if (gid != -1 && (uint32_t)gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != current_fsuid() ||
				    !current_in_group((uint32_t)gid)) {
					r = ST_PERM;
					goto out;
				}
			}
		}
	}
	/* The immutable/append-only flags belong to the object changed: the
	 * link itself under nofollow. */
	if (!nofollow)
		r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	if (!m->ops->chown)
		goto out; /* fs has no ownership */
	vfs_meta_bump();
	r = m->ops->chown(m, rel, uid, gid, nofollow);
	if (r == ST_OK)
		vfs_notify_attrib(path, !nofollow);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_chown(const char *path, int uid, int gid, int nofollow)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r, vfs_chown_at(path, uid, gid, nofollow));
	return r;
}

int vfs_fchmod(vfs_file_t *f, unsigned int mode)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fchmod)
		return ST_OK;
	if (vfs_rofs(f->mnt) != ST_OK)
		return ST_ROFS;
	vfs_meta_bump();
	int r = f->ops->fchmod(f, mode);
	if (r == ST_OK)
		vfs_notify_file(f, IN_ATTRIB);
	return r;
}

int vfs_fchown(vfs_file_t *f, int uid, int gid)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fchown)
		return ST_OK;
	if (vfs_rofs(f->mnt) != ST_OK)
		return ST_ROFS;
	vfs_meta_bump();
	int r = f->ops->fchown(f, uid, gid);
	if (r == ST_OK)
		vfs_notify_file(f, IN_ATTRIB);
	return r;
}

/* Does `t' ask for a time of the caller's choosing (rather than "now" or
 * "unchanged")?  Only the owner may do that; "now" is open to anyone who
 * may write the file. */
static int vfs_utimes_explicit(const struct vfs_utimes *t)
{
	return (t->atime_nsec != VFS_UTIME_NOW &&
		t->atime_nsec != VFS_UTIME_OMIT) ||
	       (t->mtime_nsec != VFS_UTIME_NOW &&
		t->mtime_nsec != VFS_UTIME_OMIT);
}

static int vfs_utimes_check(const struct kstat *st, const struct vfs_utimes *t,
			    int may_write)
{
	if (capable() || (uint32_t)st->st_uid == current_fsuid())
		return ST_OK;
	if (vfs_utimes_explicit(t))
		return ST_PERM;
	return may_write ? ST_OK : ST_ACCESS;
}

static int vfs_utimens_at(const char *path, int nofollow,
			  const struct vfs_utimes *t)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !t) {
		r = m ? ST_INVALID : ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_traverse_id(path, 0);
	if (r != ST_OK)
		goto out;
	{
		struct kstat st;
		r = nofollow ? vfs_raw_lstat(path, &st) :
			       vfs_raw_stat(path, &st);
		if (r != ST_OK)
			goto out;
		/* Both times "leave as they are": nothing changes, and
		 * nothing has to be allowed. */
		if (t->atime_nsec == VFS_UTIME_OMIT &&
		    t->mtime_nsec == VFS_UTIME_OMIT)
			goto out;
		if (!capable() && (uint32_t)st.st_uid != current_fsuid()) {
			int w = (vfs_permission_st(path, &st, MAY_WRITE, 0) ==
				 ST_OK);
			r = vfs_utimes_check(&st, t, w);
			if (r != ST_OK)
				goto out;
		}
	}
	if (!nofollow)
		r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	if (!m->ops->utimens)
		goto out; /* fs manages times itself */
	r = m->ops->utimens(m, rel, nofollow, t);
	if (r == ST_OK)
		vfs_notify_attrib(path, !nofollow);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_utimens(const char *path, int nofollow, const struct vfs_utimes *t)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r, vfs_utimens_at(path, nofollow, t));
	return r;
}

int vfs_futimens(vfs_file_t *f, const struct vfs_utimes *t)
{
	struct kstat st;
	int r;

	if (!f || !f->ops || !t)
		return ST_INVALID;
	if (t->atime_nsec == VFS_UTIME_OMIT && t->mtime_nsec == VFS_UTIME_OMIT)
		return ST_OK;
	if (!capable()) {
		if (vfs_fstat(f, &st) != ST_OK)
			return ST_PERM;
		r = vfs_utimes_check(&st, t, file_may_write(f));
		if (r != ST_OK)
			return r;
	}
	if (vfs_rofs(f->mnt) != ST_OK)
		return ST_ROFS;
	if (!f->ops->futimens)
		return ST_OK;
	r = f->ops->futimens(f, t);
	if (r == ST_OK)
		vfs_notify_file(f, IN_ATTRIB);
	return r;
}

/* The statfs flag word for a mount's flags (ST_RDONLY ... as statvfs
 * defines them: the same bits as the mount flags, but for relatime). */
static unsigned long vfs_statfs_flags(unsigned long mf)
{
	unsigned long f = mf & (VFS_MS_RDONLY | VFS_MS_NOSUID | VFS_MS_NODEV |
				VFS_MS_NOEXEC | VFS_MS_SYNCHRONOUS |
				VFS_MS_MANDLOCK | VFS_MS_NOATIME |
				VFS_MS_NODIRATIME);
	if (mf & VFS_MS_RELATIME)
		f |= 0x1000UL; /* ST_RELATIME */
	return f;
}

static int vfs_statfs_at(const char *path, struct vfs_statfs *out)
{
	/* The filesystem the path leads to, links followed: statfs never
	 * walks the path itself, so a stat does it. */
	struct kstat st;
	int sr = vfs_raw_stat(path, &st);
	if (sr == ST_LINK)
		return ST_LINK;
	vfs_mount_t *m = vfs_mnt_get(path, NULL);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->statfs) {
		r = m->ops->statfs(m, out);
		if (r == ST_OK)
			out->f_flags = vfs_statfs_flags(m->flags);
	}
	vfs_mnt_put(m);
	return r;
}

int vfs_statfs(const char *path, struct vfs_statfs *out)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 1, r, vfs_statfs_at(path, out));
	return r;
}

int vfs_fstatfs(vfs_file_t *f, struct vfs_statfs *out)
{
	if (!f || !f->ops)
		return ST_INVALID;
	vfs_mount_t *m = f->mnt;
	int held = 0, r = ST_UNSUPPORTED;

	if (!m && f->at_path) {
		m = vfs_mnt_get(f->at_path, NULL);
		held = 1;
	}
	if (m && m->ops->statfs) {
		r = m->ops->statfs(m, out);
		if (r == ST_OK)
			out->f_flags = vfs_statfs_flags(m->flags);
	}
	if (held)
		vfs_mnt_put(m);
	return r;
}

int vfs_fstat(vfs_file_t *f, struct kstat *st)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fstat)
		return ST_UNSUPPORTED; /* fs can't report fd owner */
	int r = f->ops->fstat(f, st);
	if (r == ST_OK)
		vfs_fix_dev(f->mnt, st);
	return r;
}

int vfs_setid_clean(vfs_file_t *f)
{
	if (f && f->ops && f->ops->setid_clean)
		return f->ops->setid_clean(f, 0);
	return 1; /* fs has no set-id bits: report clean so writes never strip */
}

void vfs_mark_setid_clean(vfs_file_t *f)
{
	if (f && f->ops && f->ops->setid_clean)
		f->ops->setid_clean(f, 1);
}

/* ---- extended attributes ---- (fs without xattrs => ST_UNSUPPORTED => EOPNOTSUPP) */
static int vfs_getxattr_at(const char *path, int nofollow, const char *name,
			   void *val, unsigned long size)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->getxattr)
		r = m->ops->getxattr(m, rel, nofollow, name, val, size);
	vfs_mnt_put(m);
	return r;
}
int vfs_getxattr(const char *path, int nofollow, const char *name, void *val,
		 unsigned long size)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r,
		 vfs_getxattr_at(path, nofollow, name, val, size));
	return r;
}
static int vfs_setxattr_at(const char *path, int nofollow, const char *name,
			   const void *val, unsigned long size, int flags)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->setxattr) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_traverse_id(path, 0);
	if (r != ST_OK)
		goto out;
	/* Setting an attribute (including a POSIX ACL) is reserved to the file's
	 * owner and the privileged caller. */
	if (!capable()) {
		struct kstat st;
		int sr = nofollow ? vfs_raw_lstat(path, &st) :
				    vfs_raw_stat(path, &st);
		if (sr == ST_LINK) {
			r = ST_LINK;
			goto out;
		}
		if (sr == ST_OK && (uint32_t)st.st_uid != current_fsuid()) {
			r = ST_PERM;
			goto out;
		}
	}
	r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_acl_cache_flush(); /* an ACL may have been added/changed */
	r = m->ops->setxattr(m, rel, nofollow, name, val, size, flags);
	if (r == ST_OK)
		vfs_notify_attrib(path, !nofollow);
out:
	vfs_mnt_put(m);
	return r;
}
int vfs_setxattr(const char *path, int nofollow, const char *name,
		 const void *val, unsigned long size, int flags)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r,
		 vfs_setxattr_at(path, nofollow, name, val, size, flags));
	return r;
}
static int vfs_listxattr_at(const char *path, int nofollow, char *list,
			    unsigned long size)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->listxattr)
		r = m->ops->listxattr(m, rel, nofollow, list, size);
	vfs_mnt_put(m);
	return r;
}
int vfs_listxattr(const char *path, int nofollow, char *list,
		  unsigned long size)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r,
		 vfs_listxattr_at(path, nofollow, list, size));
	return r;
}
static int vfs_removexattr_at(const char *path, int nofollow, const char *name)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->removexattr) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_traverse_id(path, 0);
	if (r != ST_OK)
		goto out;
	if (!capable()) {
		struct kstat st;
		int sr = nofollow ? vfs_raw_lstat(path, &st) :
				    vfs_raw_stat(path, &st);
		if (sr == ST_LINK) {
			r = ST_LINK;
			goto out;
		}
		if (sr == ST_OK && (uint32_t)st.st_uid != current_fsuid()) {
			r = ST_PERM;
			goto out;
		}
	}
	r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_acl_cache_flush(); /* an ACL may have been removed */
	r = m->ops->removexattr(m, rel, nofollow, name);
	if (r == ST_OK)
		vfs_notify_attrib(path, !nofollow);
out:
	vfs_mnt_put(m);
	return r;
}
int vfs_removexattr(const char *path, int nofollow, const char *name)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, !nofollow, r, vfs_removexattr_at(path, nofollow, name));
	return r;
}
int vfs_fgetxattr(vfs_file_t *f, const char *name, void *val,
		  unsigned long size)
{
	if (!f || !f->ops || !f->ops->fgetxattr)
		return ST_UNSUPPORTED;
	return f->ops->fgetxattr(f, name, val, size);
}
int vfs_fsetxattr(vfs_file_t *f, const char *name, const void *val,
		  unsigned long size, int flags)
{
	if (!f || !f->ops || !f->ops->fsetxattr)
		return ST_UNSUPPORTED;
	if (vfs_rofs(f->mnt) != ST_OK)
		return ST_ROFS;
	vfs_acl_cache_flush(); /* an ACL may have been added/changed */
	int r = f->ops->fsetxattr(f, name, val, size, flags);
	if (r == ST_OK)
		vfs_notify_file(f, IN_ATTRIB);
	return r;
}
int vfs_flistxattr(vfs_file_t *f, char *list, unsigned long size)
{
	if (!f || !f->ops || !f->ops->flistxattr)
		return ST_UNSUPPORTED;
	return f->ops->flistxattr(f, list, size);
}
int vfs_fremovexattr(vfs_file_t *f, const char *name)
{
	if (!f || !f->ops || !f->ops->fremovexattr)
		return ST_UNSUPPORTED;
	if (vfs_rofs(f->mnt) != ST_OK)
		return ST_ROFS;
	vfs_acl_cache_flush(); /* an ACL may have been removed */
	int r = f->ops->fremovexattr(f, name);
	if (r == ST_OK)
		vfs_notify_file(f, IN_ATTRIB);
	return r;
}
int vfs_getxattr_ino(const char *path, unsigned long ino, const char *name,
		     void *val, unsigned long size)
{
	char cbuf_[VFS_MAX_PATH];
	path = vfs_canon(path, cbuf_);
	vfs_mount_t *m = vfs_mnt_get(path, NULL);
	int r = ST_UNSUPPORTED;

	if (m && m->ops->getxattr_ino)
		r = m->ops->getxattr_ino(m, ino, name, val, size);
	vfs_mnt_put(m);
	return r;
}

long vfs_read(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops))
		return ST_INVALID;
	if (!f->ops->read)
		return ST_INVALID;
	long r = f->ops->read(f, buf, bytes);
	if (r > 0)
		vfs_notify_file(f, IN_ACCESS);
	return r;
}
/* Read at an offset the caller names.
 *
 * The point of it is what it does NOT do: consult or move f->pos.  A file's
 * position belongs to the descriptor, and a mapping shares its handle with
 * that descriptor -- mmap references the caller's open file, and fork shares
 * it again.  So a page-in that seeks the handle, reads and seeks back is
 * racing every read() and lseek() the program makes on the same descriptor,
 * and loses by filling the page from the wrong part of the file.  The
 * reference has no such race because paging there reads by index and never
 * looks at the descriptor at all; this is that property, expressed as a call.
 *
 * A filesystem without ->read_at still gets the old sequence, but under the
 * handle's page-in flag, which is at least the same serialisation pread(2)
 * already uses. */
long vfs_pread(vfs_file_t *f, void *buf, long bytes, long off)
{
	if (!f || !vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops))
		return ST_INVALID;
	if (off < 0)
		return ST_INVALID;
	if (f->ops->read_at)
		return f->ops->read_at(f, buf, bytes, off);
	if (!f->ops->read || !f->ops->seek)
		return ST_INVALID;

	while (__atomic_test_and_set(&f->pagein_busy, __ATOMIC_ACQUIRE))
		sched_yield_in_kernel();
	task_t *cur = sched_current();
	f->pagein_owner = cur ? (int64_t)cur->id : -1;

	long r = ST_INVALID;
	long saved = f->ops->seek(f, 0, 1 /* SEEK_CUR */);
	if (saved >= 0 && f->ops->seek(f, off, 0 /* SEEK_SET */) >= 0) {
		r = f->ops->read(f, buf, bytes);
		f->ops->seek(f, saved, 0);
	}
	f->pagein_owner = -1;
	__atomic_clear(&f->pagein_busy, __ATOMIC_RELEASE);
	return r;
}

long vfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	if (!f || !vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops))
		return ST_INVALID;
	if (!f->ops->write)
		return ST_INVALID;
	long r = f->ops->write(f, buf, bytes);
	if (r > 0)
		vfs_notify_file(f, IN_MODIFY);
	return r;
}

/* Write at an offset the caller names, the counterpart of vfs_pread(): the
 * description's position is neither used nor moved.  A filesystem with
 * ->write_at keeps the position out of it entirely; for one without, the
 * seek/write/seek-back sequence runs under the handle's page-in flag, which
 * serialises it against the other users of that sequence (vfs_pread's
 * fallback, demand paging) -- not against read() or lseek() on the same
 * description, which is why the op exists. */
long vfs_pwrite(vfs_file_t *f, const void *buf, long bytes, long off)
{
	long r;

	if (!f || !vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops))
		return ST_INVALID;
	if (off < 0)
		return ST_INVALID;
	if (f->ops->write_at) {
		r = f->ops->write_at(f, buf, bytes, off);
		if (r > 0)
			vfs_notify_file(f, IN_MODIFY);
		return r;
	}

	while (__atomic_test_and_set(&f->pagein_busy, __ATOMIC_ACQUIRE))
		sched_yield_in_kernel();
	task_t *cur = sched_current();
	f->pagein_owner = cur ? (int64_t)cur->id : -1;

	long saved = vfs_seek(f, 0, 1 /* SEEK_CUR */);
	if (saved < 0) {
		r = saved;
		goto unlock;
	}
	long at = vfs_seek(f, off, 0 /* SEEK_SET */);
	if (at < 0) {
		r = at;
		goto unlock;
	}
	r = vfs_write(f, buf, bytes);
	vfs_seek(f, saved, 0);
unlock:
	f->pagein_owner = -1;
	__atomic_clear(&f->pagein_busy, __ATOMIC_RELEASE);
	return r;
}
long vfs_seek(vfs_file_t *f, long offset, int whence)
{
	if (!f || !vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops))
		return -1;
	if (!f->ops->seek)
		return -1;
	return f->ops->seek(f, offset, whence);
}

long vfs_readdir(vfs_file_t *f, void *buf, long bytes)
{
	VM_BUG_ON(f == NULL);
	VM_BUG_ON(buf == NULL);
	if (!f || !f->ops || !f->ops->readdir)
		return ST_UNSUPPORTED;

	unsigned char *out = (unsigned char *)buf;
	long total = 0;

	/* The root directory lists what is not on the root filesystem as
	 * well: the mount points (/proc, /sys, ...) and /dev.  Injected once
	 * per open, ahead of the filesystem's own entries.
	 *
	 * Each record is laid out exactly as ext4_readdir_impl() lays out its
	 * own -- the name at d_name, byte 19, and d_reclen = 19 + name + NUL
	 * rounded up to 8 -- because that is where every reader takes the
	 * name from.  These used to copy the name after sizeof(struct
	 * dirent64), which is 24 with the flexible member padded, so byte 19
	 * held a zero and every mount point listed as a nameless directory.
	 * find joined "/" and "" into "/", recursed into the root again at
	 * every level, printed "/" each time and overflowed its stack. */
	if (f->is_root_dir && !f->dev_injected) {
		for (int mi = 0;; mi++) {
			const char *mn;

			if (!vfs_mount_name(mi, &mn))
				break;
			unsigned nl = 0;
			while (mn[nl])
				nl++;
			unsigned short rl = (unsigned short)((19 + nl + 1 + 7) & ~7u);
			if (bytes < rl)
				break;
			uint8_t rec[24 + 256];
			struct dirent64 *e = (struct dirent64 *)rec;

			mm_memset(rec, 0, sizeof(rec));
			e->d_ino = 2 + (uint64_t)mi;
			e->d_off = rl;
			e->d_reclen = rl;
			e->d_type = DT_DIR;
			mm_memcpy(e->d_name, mn, nl + 1);
			smap_disable();
			mm_memcpy(out, rec, rl);
			smap_enable();
			out += rl;
			bytes -= rl;
			total += rl;
		}
		f->dev_injected = 1;
	}

	// Now get remaining entries from underlying FS
	long ret = f->ops->readdir(f, out, bytes);
	if (ret > 0) {
		total += ret;
	} else if (ret < 0 && total == 0) {
		return ret; // Error and no /dev was injected
	}

	return total;
}

int vfs_truncate(vfs_file_t *f, unsigned long size)
{
	if (!f || !f->ops || !f->ops->truncate)
		return ST_UNSUPPORTED;
	/* Write access (and the immutable/append-only veto) were enforced when
	 * the handle was opened for writing, so no path-based recheck is needed
	 * here — an fd opened read-only cannot reach a writable fs truncate op. */
	int r = f->ops->truncate(f, size);
	if (r == ST_OK)
		vfs_notify_file(f, IN_MODIFY);
	return r;
}
static int vfs_unlink_at(const char *path)
{
	/* Dispatch by path, not to the root filesystem: /dev/shm names live in
	 * devfs, and sending their unlink to ext4 would report ENOENT for an
	 * object that plainly exists. */
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->unlink) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_remove_at(path);
	if (r == ST_OK)
		r = vfs_attr_allow_modify(path, 0); /* immutable/append: no unlink */
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	struct fsnotify_ident id;
	vfs_notify_path_ident(path, 0, &id);
	vfs_meta_bump();
	r = m->ops->unlink(m, rel);
	if (r == ST_OK)
		vfs_notify_removed(path, &id, 0);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_unlink(const char *path)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_unlink_at(path));
	return r;
}
int vfs_rename(const char *oldpath, const char *newpath)
{
	char obuf_[VFS_MAX_PATH], nbuf_[VFS_MAX_PATH];
	int r, locked = 0;

	/* Neither name is followed if it is a link; the links on the way to
	 * them are, here, beforehand. */
	r = vfs_two_name_path(oldpath, 0, obuf_, &oldpath);
	if (r == ST_OK)
		r = vfs_two_name_path(newpath, 0, nbuf_, &newpath);
	if (r != ST_OK)
		return r;
	/* Neither end may be a mount point: a mount is in use by what is
	 * mounted on it. */
	if (vfs_mnt_is_point(oldpath) || vfs_mnt_is_point(newpath))
		return ST_BUSY;
	/* By path, like unlink: a name below a mount point belongs to that
	 * filesystem.  Both names must be on the same one -- a rename cannot
	 * move data between filesystems, and EXDEV is what tells mv to copy
	 * and delete instead. */
	const char *orel, *nrel;
	vfs_mount_t *m = vfs_mnt_get(oldpath, &orel);
	vfs_mount_t *mn = vfs_mnt_get(newpath, &nrel);

	if (!m || !m->ops->rename) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	if (mn != m) {
		r = ST_XDEV;
		goto out;
	}
	if (m != vfs_mnt_root() && ((orel[0] == '/' && orel[1] == '\0') ||
				    (nrel[0] == '/' && nrel[1] == '\0'))) {
		r = ST_BUSY;
		goto out;
	}
	/* Renaming removes the old name (parent write + sticky) and creates the
	 * new one (new parent write); the source inode's flags must permit it. */
	r = vfs_permission_remove_at(oldpath);
	if (r == ST_OK)
		r = vfs_permission_parent_at(newpath, MAY_WRITE | MAY_EXEC);
	if (r == ST_OK)
		r = vfs_attr_allow_modify(oldpath, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	struct fsnotify_ident src, dst;
	vfs_notify_path_ident(oldpath, 0, &src);
	vfs_notify_path_ident(newpath, 0, &dst);
	vfs_meta_bump();
	/* A directory with mounts below it takes its mount points along:
	 * their paths move with it, under the mount mutex. */
	r = vfs_mnt_rename_begin(oldpath, newpath, &locked);
	if (r != ST_OK)
		goto out;
	r = m->ops->rename(m, orel, nrel);
	vfs_mnt_rename_end(locked, oldpath, newpath, r == ST_OK);
	if (r == ST_OK && fsnotify_wants(~0u)) {
		uint32_t cookie = fsnotify_cookie();

		fsnotify_event(oldpath, &src, IN_MOVED_FROM, cookie, 0);
		fsnotify_event(newpath, &src, IN_MOVED_TO | IN_MOVE_SELF,
			       cookie, 0);
		/* A name that was there before was replaced. */
		if (dst.valid && src.valid &&
		    (dst.fs != src.fs || dst.ino != src.ino)) {
			if (dst.isdir || dst.nlink <= 1)
				fsnotify_gone(&dst);
			else
				fsnotify_event(NULL, &dst, IN_ATTRIB, 0, 0);
		}
		if (src.valid && src.isdir)
			fsnotify_renamed(oldpath, newpath);
	}
out:
	vfs_mnt_put(mn);
	vfs_mnt_put(m);
	return r;
}
/* Create a socket, FIFO or device node.  Same permission rule as creating any
 * other name: write+search on the containing directory, and a device node is
 * the privileged caller's to make.  The caller is expected to have already
 * applied its umask to `mode`. */
static int vfs_mknod_at(const char *path, unsigned int mode, uint64_t dev)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->mknod) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !capable()) {
		r = ST_PERM;
		goto out;
	}
	r = vfs_permission_parent_at(path, MAY_WRITE | MAY_EXEC);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_meta_bump();
	r = m->ops->mknod(m, rel, mode, dev);
	if (r == ST_OK)
		vfs_notify_created(path, 0);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_mknod(const char *path, unsigned int mode, uint64_t dev)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_mknod_at(path, mode, dev));
	return r;
}

static int vfs_mkdir_at(const char *path, unsigned int mode)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->mkdir) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	r = vfs_permission_parent_at(path, MAY_WRITE | MAY_EXEC);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	vfs_meta_bump();
	r = m->ops->mkdir(m, rel, mode);
	if (r == ST_OK)
		vfs_notify_created(path, 1);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_mkdir(const char *path, unsigned int mode)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_mkdir_at(path, mode));
	return r;
}

static int vfs_rmdir_at(const char *path)
{
	const char *rel;
	vfs_mount_t *m = vfs_mnt_get(path, &rel);
	int r;

	if (!m || !m->ops->rmdir) {
		r = ST_UNSUPPORTED;
		goto out;
	}
	/* A mount point is in use by the mount on it (one being unmounted
	 * too: its name is still the mount's). */
	if ((rel[0] == '/' && rel[1] == '\0' && m != vfs_mnt_root()) ||
	    vfs_mnt_is_point(path)) {
		r = ST_BUSY;
		goto out;
	}
	r = vfs_permission_remove_at(path);
	if (r == ST_OK)
		r = vfs_attr_allow_modify(path, 0);
	if (r == ST_OK)
		r = vfs_rofs(m);
	if (r != ST_OK)
		goto out;
	struct fsnotify_ident id;
	vfs_notify_path_ident(path, 0, &id);
	vfs_meta_bump();
	r = m->ops->rmdir(m, rel);
	if (r == ST_OK)
		vfs_notify_removed(path, &id, 1);
out:
	vfs_mnt_put(m);
	return r;
}

int vfs_rmdir(const char *path)
{
	struct vfs_walk w;
	int r;

	VFS_WALK(w, path, 0, r, vfs_rmdir_at(path));
	return r;
}

static void vfs_release_one(vfs_mount_t *m, void *arg)
{
	if (m->ops->release_locks_for_task)
		m->ops->release_locks_for_task(m, *(uint64_t *)arg);
}

void vfs_release_locks_for_task(uint64_t task_id)
{
	/* Advisory record locks go with the process.  A lock that outlived its
	 * owner would block every later process permanently, and the F_SETLKW
	 * wait would spin on it with nothing left to release it. */
	frlock_release_for_task((uint32_t)task_id);

	/* Every filesystem instance: each has its own locks -- the lazily
	 * unmounted ones and those being unmounted too, which a lock left
	 * held would keep from ever being torn down. */
	vfs_mnt_for_each_instance(vfs_release_one, &task_id, 1);
}

int vfs_close(vfs_file_t *f)
{
	BUG_ON(f == NULL);
	if (!f)
		return ST_INVALID;
	/* Before ->ops is followed, not after: a released file's memory holds
	 * the allocator's poison, and reading a function pointer out of that
	 * is what turns a reference-counting bug into a halted machine. */
	if (!vfs_ptr_plausible(f) || !vfs_ptr_plausible(f->ops)) {
		kprintf("vfs_close: release of an already-released file %p by caller=%p\n",
			f, __builtin_return_address(0));
		return ST_INVALID;
	}
	if (!f->ops->close)
		return ST_INVALID;

	// Atomically decrement refcount; only the thread that transitions 1→0 closes
	int old = __sync_fetch_and_sub(&f->refcount, 1);
	WARN_ON(old < 0);
	WARN_ON_ONCE(
		old >
		65536); /* refcount suspiciously large: vfs_dup/vfs_incref without matching vfs_close */
	if (old > 1) {
		return ST_OK;
	}

	// Guard against refcount underflow (double-close).
	// If old <= 0, someone already closed this file; undo the decrement and bail.
	if (old <= 0) {
		/* Someone released a reference they did not hold.  Undo it and
		 * say enough to find them: the object address alone proves a
		 * double close happened but not what was closed or by whom.
		 *
		 * The caller's return address is the useful half -- symbolize
		 * it on the build host with
		 *     addr2line -f -e build/kernel.elf <caller>
		 * (build unstripped first: make NO_STRIP=1).  at_path names the
		 * file when it has one, which is what identifies a file-backed
		 * mapping released twice as opposed to a descriptor. */
		void *caller = __builtin_return_address(0);

		__sync_fetch_and_add(&f->refcount, 1); // undo
		WARN(1, "vfs_close refcount underflow on %p (old=%d)", f, old);
		kprintf("vfs_close: BUG refcount underflow on %p (old=%d) path=%s caller=%p\n",
			f, old, f->at_path ? f->at_path : "(none)", caller);
		return ST_INVALID;
	}

	/* Actually close when refcount reaches 0 (old was 1, now 0).
	 *
	 * at_path is released FIRST: ->close frees the vfs_file itself, so
	 * reading the pointer afterwards would be a use-after-free.  The close
	 * is reported before that, while the path is still there. */
	if (f->at_path)
		vfs_notify_file(f, file_may_write(f) ? IN_CLOSE_WRITE :
						       IN_CLOSE_NOWRITE);
	if (f->at_path) {
		kfree(f->at_path);
		f->at_path = NULL;
	}
	/* ->close frees the handle: what the mount needs is read first, and
	 * the mount is told only after the filesystem is done with it. */
	vfs_mount_t *m = f->mnt;
	int was_writer = file_may_write(f);
	int r = f->ops->close(f);
	if (m) {
		if (was_writer)
			__atomic_sub_fetch(&m->open_writers, 1,
					   __ATOMIC_SEQ_CST);
		vfs_mnt_file_closed(m);
	}
	return r;
}

// Duplicate file descriptor - increment refcount
vfs_file_t *vfs_dup(vfs_file_t *f)
{
	BUG_ON(f == NULL);
	if (!f)
		return NULL;
	WARN_ON(f->refcount <=
		0); /* duplicating a file with zero/negative refcount: file was already closed */

	__sync_fetch_and_add(&f->refcount, 1);
	return f;
}

// Just increment refcount
void vfs_incref(vfs_file_t *f)
{
	if (f)
		__sync_fetch_and_add(&f->refcount, 1);
}

size_t vfs_size(vfs_file_t *f)
{
	if (!f)
		return 0;
	WARN_ON(f->ops == NULL);
	/* Ask the file's own filesystem for its size via the fstat op, so this stays
     * filesystem-independent (fat32, ext4, ...) instead of casting to a
     * driver-specific handle. */
	struct kstat st;
	if (vfs_fstat(f, &st) == ST_OK)
		return (size_t)st.st_size;
	return 0;
}
