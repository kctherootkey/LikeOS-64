// LikeOS-64 -- extended attributes.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/fs/namei.h>


/* ===================================================================
 * Extended attributes (xattr).  The path ops take a trailing nofollow flag so
 * libc's l*-variants reuse the same syscall number.  Values/lists are bounced
 * through a kernel buffer capped at one block.  Permission model (root bypasses;
 * only non-root is checked): get/list need ancestor search; set/remove need
 * write on the target, and trusted.* is root-only.
 * =================================================================== */
#define XATTR_MAX_VALUE 4096 /* one block; covers ibody + future block   */

static int xattr_copy_name(uint64_t u_name, char *kname /*[256]*/)
{
	if (!validate_user_ptr(u_name, 1))
		return -EFAULT;
	size_t nl;
	int e = user_strnlen((const char *)u_name, 255, &nl);
	if (e)
		return e;
	e = copy_from_user(kname, (const void *)u_name, nl + 1);
	if (e)
		return e;
	kname[nl] = '\0';
	return 0;
}

static int xattr_has_prefix(const char *s, const char *pfx)
{
	while (*pfx) {
		if (*s++ != *pfx++)
			return 0;
	}
	return 1;
}


/* Namespace policy for a non-root caller setting/removing xattr `name` on a file
 * owned by `owner_uid`.  The `system.` namespace (which holds the POSIX ACLs) is
 * owner-controlled, like chmod — write permission is not sufficient.  Returns:
 *   0      -> allowed by ownership (system.*)
 *  -EPERM  -> denied (trusted.* at all; system.* when not the owner)
 *   1      -> defer: caller must still verify write permission (user.* etc.) */
static int xattr_ns_perm(task_t *cur, const char *name, uint32_t owner_uid)
{
	if (xattr_has_prefix(name, "trusted."))
		return -EPERM;
	if (xattr_has_prefix(name, "system."))
		return (owner_uid == cur->cred.fsuid) ? 0 : -EPERM;
	return 1;
}


/* The syscall ABI here passes at most 5 args, so setxattr's nofollow is carried
 * in a private high bit of `flags` (libc's lsetxattr sets it). */
#define XATTR_SYS_NOFOLLOW 0x40000000
int64_t sys_setxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
			    uint64_t size, uint64_t flags)
{
	int nofollow = (flags & XATTR_SYS_NOFOLLOW) ? 1 : 0;
	flags &= ~(uint64_t)XATTR_SYS_NOFOLLOW;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size > XATTR_MAX_VALUE)
		return -ENOSPC;
	uint8_t *kval = 0;
	if (size) {
		if (!validate_user_ptr(u_val, size))
			return -EFAULT;
		kval = (uint8_t *)kalloc(size);
		if (!kval)
			return -ENOMEM;
		if (copy_from_user(kval, (const void *)u_val, size)) {
			kfree(kval);
			return -EFAULT;
		}
	}
	if (cur->cred.euid != 0) {
		struct kstat st;
		int sr =
			nofollow ? vfs_lstat(kpath, &st) : vfs_stat(kpath, &st);
		if (sr == ST_OK) {
			int np = xattr_ns_perm(cur, kname, (uint32_t)st.st_uid);
			if (np < 0) {
				if (kval)
					kfree(kval);
				return np;
			}
			if (np > 0) { /* user.* etc.: need write perm */
				int pr = perm_access(cur, kpath, &st, MAY_WRITE,
						     0);
				if (pr < 0) {
					if (kval)
						kfree(kval);
					return pr;
				}
			}
		}
	}
	int r = vfs_setxattr(kpath, (int)nofollow, kname, kval, size,
			     (int)flags);
	if (kval)
		kfree(kval);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}


int64_t sys_getxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
			    uint64_t size, uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
	}
	if (size == 0) { /* query value size */
		int r = vfs_getxattr(kpath, (int)nofollow, kname, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	uint8_t *kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_getxattr(kpath, (int)nofollow, kname, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_val, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}


int64_t sys_listxattr(uint64_t u_path, uint64_t u_list, uint64_t size,
			     uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
	}
	if (size == 0) {
		int r = vfs_listxattr(kpath, (int)nofollow, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	char *kbuf = (char *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_listxattr(kpath, (int)nofollow, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_list, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}


int64_t sys_removexattr(uint64_t u_path, uint64_t u_name,
			       uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		struct kstat st;
		int sr =
			nofollow ? vfs_lstat(kpath, &st) : vfs_stat(kpath, &st);
		if (sr == ST_OK) {
			int np = xattr_ns_perm(cur, kname, (uint32_t)st.st_uid);
			if (np < 0)
				return np;
			if (np > 0) { /* user.* etc.: need write perm */
				int pr = perm_access(cur, kpath, &st, MAY_WRITE,
						     0);
				if (pr < 0)
					return pr;
			}
		}
	}
	int r = vfs_removexattr(kpath, (int)nofollow, kname);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}


int64_t sys_fsetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
			     uint64_t size, uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	/* An extended attribute belongs to an inode; a marker has none, and
	 * passing one to the VFS would dereference it. */
	if (fd_is_special(task_fds(cur)[fd]))
		return -EOPNOTSUPP;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size > XATTR_MAX_VALUE)
		return -ENOSPC;
	if (cur->cred.euid != 0) {
		if (xattr_has_prefix(kname, "trusted."))
			return -EPERM;
		if (xattr_has_prefix(
			    kname,
			    "system.")) { /* incl. POSIX ACLs: owner-only */
			struct kstat st;
			if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK &&
			    (uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
		}
	}
	uint8_t *kval = 0;
	if (size) {
		if (!validate_user_ptr(u_val, size))
			return -EFAULT;
		kval = (uint8_t *)kalloc(size);
		if (!kval)
			return -ENOMEM;
		if (copy_from_user(kval, (const void *)u_val, size)) {
			kfree(kval);
			return -EFAULT;
		}
	}
	int r = vfs_fsetxattr(task_fds(cur)[fd], kname, kval, size, (int)flags);
	if (kval)
		kfree(kval);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}


int64_t sys_fgetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
			     uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (fd_is_special(task_fds(cur)[fd]))
		return -EOPNOTSUPP;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size == 0) {
		int r = vfs_fgetxattr(task_fds(cur)[fd], kname, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	uint8_t *kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_fgetxattr(task_fds(cur)[fd], kname, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_val, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}


int64_t sys_flistxattr(uint64_t fd, uint64_t u_list, uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (size == 0) {
		int r = vfs_flistxattr(task_fds(cur)[fd], 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	char *kbuf = (char *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_flistxattr(task_fds(cur)[fd], kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_list, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}


int64_t sys_fremovexattr(uint64_t fd, uint64_t u_name)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		if (xattr_has_prefix(kname, "trusted."))
			return -EPERM;
		if (xattr_has_prefix(
			    kname,
			    "system.")) { /* incl. POSIX ACLs: owner-only */
			struct kstat st;
			if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK &&
			    (uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
		}
	}
	int r = vfs_fremovexattr(task_fds(cur)[fd], kname);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}

