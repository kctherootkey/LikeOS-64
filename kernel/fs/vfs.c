// LikeOS-64 - Minimal VFS implementation
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/bug.h>

static const vfs_ops_t *g_root_ops = 0;
static const vfs_ops_t *g_dev_ops = 0;

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
#ifndef O_TRUNC
#define O_TRUNC 0x0200
#endif
#ifndef O_APPEND
#define O_APPEND 0x0400
#endif

/* Permission helpers used by the operation wrappers below; the canonical
 * vfs_permission / vfs_permission_traverse / vfs_access are declared in vfs.h. */
static int vfs_attr_allow_modify(const char *path, int is_append_write);
static const vfs_ops_t *vfs_ops_for_path(const char *path);
/* Raw stat dispatch with NO permission check — used by the permission layer
 * itself (the public vfs_stat below adds prefix-traversal enforcement, so the
 * permission helpers must use this raw form to avoid unbounded recursion). */
static int vfs_raw_stat(const char *path, struct kstat *st);

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
static unsigned long g_noacl_ino[VFS_NOACL_CACHE];
static unsigned g_noacl_next;

static int noacl_cached(unsigned long ino)
{
	if (ino == 0)
		return 0;
	for (unsigned i = 0; i < VFS_NOACL_CACHE; i++)
		if (g_noacl_ino[i] == ino)
			return 1;
	return 0;
}

static void noacl_remember(unsigned long ino)
{
	if (ino == 0 || noacl_cached(ino))
		return;
	g_noacl_ino[g_noacl_next] = ino;
	g_noacl_next = (g_noacl_next + 1) % VFS_NOACL_CACHE;
}

static void vfs_acl_cache_flush(void)
{
	for (unsigned i = 0; i < VFS_NOACL_CACHE; i++)
		g_noacl_ino[i] = 0;
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

static int statc_get(const char *path, struct kstat *out)
{
	if (g_statc_gen != g_meta_gen) { /* stale: drop everything */
		for (unsigned i = 0; i < VFS_STATC; i++)
			g_statc[i].valid = 0;
		g_statc_gen = g_meta_gen;
		return 0;
	}
	for (unsigned i = 0; i < VFS_STATC; i++)
		if (g_statc[i].valid && strcmp(g_statc[i].path, path) == 0) {
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
	if (cuid != 0 && !noacl_cached((unsigned long)st->st_ino)) {
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
			noacl_remember((unsigned long)st->st_ino);
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
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (o && o->permission) {
		int fr = o->permission(path, (unsigned long)st->st_ino, want);
		if (fr != ST_UNSUPPORTED)
			return fr;
	}
	return vfs_perm_access(path, st, want, use_real);
}

/* Shared worker for vfs_permission (use_real=0) and vfs_access (use_real=1). */
static int vfs_permission_id(const char *path, int want, int use_real)
{
	BUG_ON(path == NULL);
	const vfs_ops_t *o = vfs_ops_for_path(path);
	struct kstat st;
	if (!o || vfs_raw_stat(path, &st) != ST_OK)
		return ST_OK; /* unresolved: defer to the real op */
	return vfs_permission_st(path, &st, want, use_real);
}

int vfs_permission(const char *path, int want)
{
	return vfs_permission_id(path, want, 0);
}

/* Access check against an already-resolved stat (no path walk), honouring the
 * ACL then the mode bits.  Lets a caller that just stat'd a file check it
 * without re-resolving.  `use_real` selects the real vs effective/fs ids. */
int vfs_check_access(const char *path, const struct kstat *st, int want,
		     int use_real)
{
	BUG_ON(path == NULL);
	BUG_ON(st == NULL);
	return vfs_perm_access(path, st, want, use_real);
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
		if (traverse_stat(dir, &st) != ST_OK)
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
	return vfs_permission_traverse_id(path, 0);
}

int vfs_access(const char *path, int want)
{
	int tr = vfs_permission_traverse_id(path, 1);
	if (tr != ST_OK)
		return tr;
	return vfs_permission_id(path, want, 1);
}

/* Prefix traversal with an explicit real/effective id selection, exposed so the
 * access(2)/faccessat syscalls can search the prefix separately from the final
 * access check (they need to distinguish F_OK reachability and ENOENT). */
int vfs_access_traverse(const char *path, int use_real)
{
	return vfs_permission_traverse_id(path, use_real);
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
int vfs_permission_parent(const char *path, int want)
{
	if (capable())
		return ST_OK;
	char abuf[VFS_MAX_PATH];
	path = vfs_abspath(path, abuf, sizeof(abuf)); /* parent-of needs absolute */
	char parent[VFS_MAX_PATH];
	if (!vfs_parent_of(path, parent, sizeof(parent)))
		return ST_OK;
	int tr = vfs_permission_traverse(parent);
	if (tr != ST_OK)
		return tr;
	return vfs_permission(parent, want);
}

/* Remove/rename gate: parent write+search, plus the sticky bit (S_ISVTX, e.g.
 * /tmp at 1777) — a non-privileged task may then only remove/rename an entry it
 * owns, or when it owns the containing directory. */
int vfs_permission_remove(const char *path)
{
	if (capable())
		return ST_OK;
	char abuf[VFS_MAX_PATH];
	path = vfs_abspath(path, abuf, sizeof(abuf)); /* parent-of / raw_stat need it */
	int pr = vfs_permission_parent(path, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	char parent[VFS_MAX_PATH];
	if (!vfs_parent_of(path, parent, sizeof(parent)))
		return ST_OK;
	struct kstat pst;
	if (vfs_raw_stat(parent, &pst) != ST_OK)
		return ST_OK;
	if (!(pst.st_mode & S_ISVTX))
		return ST_OK; /* no sticky bit: parent write is enough */
	struct kstat st;
	if (vfs_raw_stat(path, &st) != ST_OK)
		return ST_OK;
	uint32_t me = current_fsuid();
	if ((uint32_t)st.st_uid == me || (uint32_t)pst.st_uid == me)
		return ST_OK;
	return ST_PERM;
}

/* Veto a modification forbidden by immutable/append-only inode flags, which
 * bind regardless of ownership (even the owner and the privileged caller must
 * clear the flag first).  `is_append_write` is 1 only for an append-mode write
 * (the lone modification an append-only file permits).  No-op for a filesystem
 * without the inode_flags op. */
static int vfs_attr_allow_modify_ino(const vfs_ops_t *o, unsigned long ino,
				     int is_append_write)
{
	if (!o || !o->inode_flags)
		return ST_OK;
	uint32_t fl = 0;
	if (o->inode_flags(ino, &fl) != ST_OK)
		return ST_OK;
	if (fl & VFS_ATTR_IMMUTABLE)
		return ST_PERM;
	if ((fl & VFS_ATTR_APPEND) && !is_append_write)
		return ST_PERM;
	return ST_OK;
}

static int vfs_attr_allow_modify(const char *path, int is_append_write)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->inode_flags)
		return ST_OK;
	struct kstat st;
	if (vfs_raw_stat(path, &st) != ST_OK)
		return ST_OK;
	return vfs_attr_allow_modify_ino(o, (unsigned long)st.st_ino,
					 is_append_write);
}

/* ================================================================== */

int vfs_init(void)
{
	g_root_ops = 0;
	g_dev_ops = 0;
	return ST_OK;
}
int vfs_register_root(const vfs_ops_t *ops)
{
	if (!ops)
		return ST_INVALID;
	g_root_ops = ops;
	return ST_OK;
}
int vfs_register_devfs(const vfs_ops_t *ops)
{
	if (!ops)
		return ST_INVALID;
	g_dev_ops = ops;
	return ST_OK;
}
int vfs_root_ready(void)
{
	return g_root_ops != 0;
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

static int vfs_is_root_path(const char *path)
{
	if (!path)
		return 0;
	// "/" or "/.." or "/." all resolve to root
	if (path[0] == '/' && path[1] == '\0')
		return 1;
	return 0;
}

int vfs_open(const char *path, int flags, vfs_file_t **out)
{
	BUG_ON(path == NULL);
	BUG_ON(out == NULL);

	/* Permission enforcement (canonical for every filesystem): search the
	 * path prefix, then check read/write on an existing target — or write on
	 * the parent directory when creating — plus any immutable/append-only
	 * inode flag.  Decided here so read()/write() need not re-check: the open
	 * mode the fd carries already encodes the granted access. */
	{
		int tr = vfs_permission_traverse(path);
		if (tr != ST_OK)
			return tr;
		int acc = flags & 3;
		int want = 0;
		if (acc == O_RDONLY || acc == O_RDWR)
			want |= MAY_READ;
		if (acc == O_WRONLY || acc == O_RDWR)
			want |= MAY_WRITE;
		if (flags & O_TRUNC)
			want |= MAY_WRITE;
		struct kstat est;
		int exists = (vfs_raw_stat(path, &est) == ST_OK);
		if (exists) {
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
				int im = vfs_attr_allow_modify_ino(
					vfs_ops_for_path(path),
					(unsigned long)est.st_ino, append_ok);
				if (im != ST_OK)
					return im;
			}
		} else if (flags & O_CREAT) {
			int pr = vfs_permission_parent(path,
						       MAY_WRITE | MAY_EXEC);
			if (pr != ST_OK)
				return pr;
		}
	}

	if (vfs_is_dev_path(path)) {
		if (!g_dev_ops || !g_dev_ops->open)
			return ST_UNSUPPORTED;
		int ret = g_dev_ops->open(path, flags, out);
		if (ret == ST_OK && *out) {
			WARN_ON(*out == NULL);
			WARN_ON((*out)->ops == NULL);
			(*out)->refcount = 1;
			(*out)->flags = flags;
			(*out)->is_root_dir = 0;
			(*out)->dev_injected = 0;
		}
		WARN_ON(ret == ST_OK && *out == NULL);
		return ret;
	}

	if (!g_root_ops || !g_root_ops->open)
		return ST_UNSUPPORTED;
	int ret = g_root_ops->open(path, flags, out);
	if (ret == ST_OK && *out) {
		WARN_ON(*out == NULL);
		WARN_ON((*out)->ops == NULL);
		(*out)->refcount = 1;
		(*out)->flags = flags;
		(*out)->is_root_dir = vfs_is_root_path(path);
		(*out)->dev_injected = 0;
	}
	WARN_ON(ret == ST_OK && *out == NULL);
	return ret;
}

/* Raw stat: dispatch to the owning filesystem with no permission check.  Used
 * by the permission layer (which would otherwise recurse through vfs_stat). */
static int vfs_raw_stat(const char *path, struct kstat *st)
{
	if (vfs_is_dev_path(path)) {
		if (!g_dev_ops || !g_dev_ops->stat)
			return ST_UNSUPPORTED;
		return g_dev_ops->stat(path, st);
	}
	if (!g_root_ops || !g_root_ops->stat)
		return ST_UNSUPPORTED;
	return g_root_ops->stat(path, st);
}

int vfs_stat(const char *path, struct kstat *st)
{
	/* stat(2) requires search (x) permission on every directory in the
	 * prefix; the file's own permissions are not consulted. */
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	return vfs_raw_stat(path, st);
}

int vfs_chdir(const char *path)
{
	/* Need search (x) on the prefix and on the target directory itself. */
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	int pr = vfs_permission(path, MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	if (vfs_is_dev_path(path)) {
		if (!g_dev_ops || !g_dev_ops->chdir)
			return ST_UNSUPPORTED;
		return g_dev_ops->chdir(path);
	}
	if (!g_root_ops || !g_root_ops->chdir)
		return ST_UNSUPPORTED;
	return g_root_ops->chdir(path);
}

/* Pick the filesystem that owns a path: devfs for /dev*, otherwise the root.
 * (Single-root today; a future mount table would resolve the mountpoint here,
 * leaving every caller below unchanged.) */
static const vfs_ops_t *vfs_ops_for_path(const char *path)
{
	return vfs_is_dev_path(path) ? g_dev_ops : g_root_ops;
}

/* Flush the root filesystem's pending metadata + journal (sync(2)).  No-op when
 * the root fs has no sync op (e.g. FAT32, which has no journal to clean). */
int vfs_sync(void)
{
	if (!g_root_ops || !g_root_ops->sync)
		return ST_OK;
	return g_root_ops->sync();
}

/* ---- UNIX-semantics wrappers ------------------------------------------------
 * Each dispatches to the owning filesystem's op and supplies a legacy fallback
 * when that op is NULL, so the syscall layer stays filesystem-agnostic. */

int vfs_lstat(const char *path, struct kstat *st)
{
	int tr = vfs_permission_traverse(path); /* search the prefix */
	if (tr != ST_OK)
		return tr;
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o)
		return ST_UNSUPPORTED;
	if (o->lstat)
		return o->lstat(path, st);
	if (o->stat)
		return o->stat(path, st); /* no symlinks: lstat == stat */
	return ST_UNSUPPORTED;
}

int vfs_symlink(const char *target, const char *linkpath)
{
	const vfs_ops_t *o = vfs_ops_for_path(linkpath);
	if (!o || !o->symlink)
		return ST_UNSUPPORTED;
	int pr = vfs_permission_parent(linkpath, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	vfs_meta_bump();
	return o->symlink(target, linkpath);
}

int vfs_readlink(const char *path, char *buf, unsigned long bufsz)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->readlink)
		return ST_INVALID; /* not a symlink here */
	return o->readlink(path, buf, bufsz);
}

int vfs_link(const char *oldpath, const char *newpath)
{
	/* A hard link's two ends share an inode, so both live on one filesystem;
     * route by the new name's owning fs. */
	const vfs_ops_t *o = vfs_ops_for_path(newpath);
	if (!o || !o->link)
		return ST_UNSUPPORTED;
	/* Need to reach the existing inode and write the new name's directory. */
	int tr = vfs_permission_traverse(oldpath);
	if (tr != ST_OK)
		return tr;
	int pr = vfs_permission_parent(newpath, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	vfs_meta_bump();
	return o->link(oldpath, newpath);
}

int vfs_chmod(const char *path, unsigned int mode)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o)
		return ST_UNSUPPORTED;
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	if (!capable()) {
		struct kstat st;
		if (vfs_raw_stat(path, &st) == ST_OK) {
			/* Only the file's owner may change its mode. */
			if ((uint32_t)st.st_uid != current_fsuid())
				return ST_PERM;
			/* A non-privileged owner cannot set the set-group-ID bit
			 * for a group it is not a member of. */
			if ((mode & S_ISGID) &&
			    !current_in_group((uint32_t)st.st_gid))
				mode &= ~(unsigned)S_ISGID;
		}
	}
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	if (!o->chmod)
		return ST_OK; /* fs has no permission bits */
	vfs_meta_bump();
	return o->chmod(path, mode);
}

int vfs_chown(const char *path, int uid, int gid)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o)
		return ST_UNSUPPORTED;
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	if (!capable()) {
		struct kstat st;
		if (vfs_raw_stat(path, &st) == ST_OK) {
			/* Changing the owner is privileged. */
			if (uid != -1 && (uint32_t)uid != (uint32_t)st.st_uid)
				return ST_PERM;
			/* Changing the group requires owning the file and being
			 * a member of the target group. */
			if (gid != -1 && (uint32_t)gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != current_fsuid())
					return ST_PERM;
				if (!current_in_group((uint32_t)gid))
					return ST_PERM;
			}
		}
	}
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	if (!o->chown)
		return ST_OK; /* fs has no ownership */
	vfs_meta_bump();
	return o->chown(path, uid, gid);
}

int vfs_fchmod(vfs_file_t *f, unsigned int mode)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fchmod)
		return ST_OK;
	vfs_meta_bump();
	return f->ops->fchmod(f, mode);
}

int vfs_fchown(vfs_file_t *f, int uid, int gid)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fchown)
		return ST_OK;
	vfs_meta_bump();
	return f->ops->fchown(f, uid, gid);
}

int vfs_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o)
		return ST_UNSUPPORTED;
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	if (!capable()) {
		struct kstat st;
		/* The owner may always set times; otherwise write permission on
		 * the file is required. */
		if (vfs_raw_stat(path, &st) == ST_OK &&
		    (uint32_t)st.st_uid != current_fsuid()) {
			int pr = vfs_permission(path, MAY_WRITE);
			if (pr != ST_OK)
				return pr;
		}
	}
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	if (!o->utimensat)
		return ST_OK; /* fs manages times itself */
	return o->utimensat(path, mtime_sec, mtime_nsec);
}

int vfs_statfs(const char *path, struct vfs_statfs *out)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->statfs)
		return ST_UNSUPPORTED;
	return o->statfs(out);
}

int vfs_fstatfs(vfs_file_t *f, struct vfs_statfs *out)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->statfs)
		return ST_UNSUPPORTED;
	return f->ops->statfs(out);
}

int vfs_fstat(vfs_file_t *f, struct kstat *st)
{
	if (!f || !f->ops)
		return ST_INVALID;
	if (!f->ops->fstat)
		return ST_UNSUPPORTED; /* fs can't report fd owner */
	return f->ops->fstat(f, st);
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
int vfs_getxattr(const char *path, int nofollow, const char *name, void *val,
		 unsigned long size)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->getxattr)
		return ST_UNSUPPORTED;
	return o->getxattr(path, nofollow, name, val, size);
}
int vfs_setxattr(const char *path, int nofollow, const char *name,
		 const void *val, unsigned long size, int flags)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->setxattr)
		return ST_UNSUPPORTED;
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	/* Setting an attribute (including a POSIX ACL) is reserved to the file's
	 * owner and the privileged caller. */
	if (!capable()) {
		struct kstat st;
		if (vfs_raw_stat(path, &st) == ST_OK &&
		    (uint32_t)st.st_uid != current_fsuid())
			return ST_PERM;
	}
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	vfs_acl_cache_flush(); /* an ACL may have been added/changed */
	return o->setxattr(path, nofollow, name, val, size, flags);
}
int vfs_listxattr(const char *path, int nofollow, char *list,
		  unsigned long size)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->listxattr)
		return ST_UNSUPPORTED;
	return o->listxattr(path, nofollow, list, size);
}
int vfs_removexattr(const char *path, int nofollow, const char *name)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->removexattr)
		return ST_UNSUPPORTED;
	int tr = vfs_permission_traverse(path);
	if (tr != ST_OK)
		return tr;
	if (!capable()) {
		struct kstat st;
		if (vfs_raw_stat(path, &st) == ST_OK &&
		    (uint32_t)st.st_uid != current_fsuid())
			return ST_PERM;
	}
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	vfs_acl_cache_flush(); /* an ACL may have been removed */
	return o->removexattr(path, nofollow, name);
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
	vfs_acl_cache_flush(); /* an ACL may have been added/changed */
	return f->ops->fsetxattr(f, name, val, size, flags);
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
	vfs_acl_cache_flush(); /* an ACL may have been removed */
	return f->ops->fremovexattr(f, name);
}
int vfs_getxattr_ino(const char *path, unsigned long ino, const char *name,
		     void *val, unsigned long size)
{
	const vfs_ops_t *o = vfs_ops_for_path(path);
	if (!o || !o->getxattr_ino)
		return ST_UNSUPPORTED;
	return o->getxattr_ino(ino, name, val, size);
}

long vfs_read(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !f->ops || !f->ops->read)
		return ST_INVALID;
	return f->ops->read(f, buf, bytes);
}
long vfs_write(vfs_file_t *f, const void *buf, long bytes)
{
	if (!f || !f->ops || !f->ops->write)
		return ST_INVALID;
	return f->ops->write(f, buf, bytes);
}
long vfs_seek(vfs_file_t *f, long offset, int whence)
{
	if (!f || !f->ops || !f->ops->seek)
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

	// If this is the root directory and we haven't injected /dev yet, inject it first
	if (f->is_root_dir && !f->dev_injected && g_dev_ops) {
		// Calculate size for "dev" entry
		unsigned short reclen =
			(unsigned short)(sizeof(struct dirent64) +
					 4); // "dev" + null
		reclen = (reclen + 7) & ~7; // Align to 8 bytes
		WARN_ON(reclen % 8 != 0);

		if (bytes >= reclen) {
			// Build entry in kernel buffer first, then copy to user
			struct dirent64 ent;
			ent.d_ino = 2; // Fake inode for /dev
			ent.d_off = reclen;
			ent.d_reclen = reclen;
			ent.d_type = DT_DIR;
			ent.d_name[0] = 'd';
			ent.d_name[1] = 'e';
			ent.d_name[2] = 'v';
			ent.d_name[3] = '\0';

			// SMAP-aware copy to user buffer
			smap_disable();
			mm_memcpy(out, &ent, sizeof(ent));
			smap_enable();

			out += reclen;
			bytes -= reclen;
			total += reclen;
			f->dev_injected = 1;
		}
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
	return f->ops->truncate(f, size);
}
int vfs_unlink(const char *path)
{
	if (!g_root_ops || !g_root_ops->unlink)
		return ST_UNSUPPORTED;
	int pr = vfs_permission_remove(path);
	if (pr != ST_OK)
		return pr;
	int im = vfs_attr_allow_modify(path, 0); /* immutable/append: no unlink */
	if (im != ST_OK)
		return im;
	vfs_meta_bump();
	return g_root_ops->unlink(path);
}
int vfs_rename(const char *oldpath, const char *newpath)
{
	if (!g_root_ops || !g_root_ops->rename)
		return ST_UNSUPPORTED;
	/* Renaming removes the old name (parent write + sticky) and creates the
	 * new one (new parent write); the source inode's flags must permit it. */
	int pr = vfs_permission_remove(oldpath);
	if (pr != ST_OK)
		return pr;
	pr = vfs_permission_parent(newpath, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	int im = vfs_attr_allow_modify(oldpath, 0);
	if (im != ST_OK)
		return im;
	vfs_meta_bump();
	return g_root_ops->rename(oldpath, newpath);
}
int vfs_mkdir(const char *path, unsigned int mode)
{
	if (!g_root_ops || !g_root_ops->mkdir)
		return ST_UNSUPPORTED;
	int pr = vfs_permission_parent(path, MAY_WRITE | MAY_EXEC);
	if (pr != ST_OK)
		return pr;
	vfs_meta_bump();
	return g_root_ops->mkdir(path, mode);
}
int vfs_rmdir(const char *path)
{
	if (!g_root_ops || !g_root_ops->rmdir)
		return ST_UNSUPPORTED;
	int pr = vfs_permission_remove(path);
	if (pr != ST_OK)
		return pr;
	int im = vfs_attr_allow_modify(path, 0);
	if (im != ST_OK)
		return im;
	vfs_meta_bump();
	return g_root_ops->rmdir(path);
}

void vfs_release_locks_for_task(uint64_t task_id)
{
	if (g_root_ops && g_root_ops->release_locks_for_task)
		g_root_ops->release_locks_for_task(task_id);
	if (g_dev_ops && g_dev_ops->release_locks_for_task)
		g_dev_ops->release_locks_for_task(task_id);
}

int vfs_close(vfs_file_t *f)
{
	BUG_ON(f == NULL);
	if (!f || !f->ops || !f->ops->close)
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
		__sync_fetch_and_add(&f->refcount, 1); // undo
		WARN(1, "vfs_close refcount underflow on %p (old=%d)", f, old);
		kprintf("vfs_close: BUG refcount underflow on %p (old=%d)\n", f,
			old);
		return ST_INVALID;
	}

	// Actually close when refcount reaches 0 (old was 1, now 0)
	return f->ops->close(f);
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
