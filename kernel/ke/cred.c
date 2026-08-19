// LikeOS-64 - UNIX process credentials
//
// The set*-id transition rules below follow POSIX: a privileged process
// (effective uid 0) may set IDs freely; an unprivileged process may only
// switch among its real, effective and saved-set IDs.  The filesystem IDs
// track the effective IDs (we do not expose a separate setfsuid/setfsgid).

#include <kernel/ke/cred.h>
#include <kernel/ke/sched.h> /* task_t, sched_current() */
#include <kernel/ke/syscall.h> /* EPERM, EACCES */
#include <kernel/uapi/stat.h> /* S_ISDIR */
#include <kernel/uapi/bug.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>

void cred_init_root(cred_t *c)
{
	BUG_ON(c == NULL);
	c->uid = c->gid = 0;
	c->euid = c->egid = 0;
	c->suid = c->sgid = 0;
	c->fsuid = c->fsgid = 0;
	c->ngroups = 0;
	for (unsigned i = 0; i < NGROUPS_MAX; i++)
		c->groups[i] = 0;
}

int cred_is_root(const cred_t *c)
{
	return c->euid == 0;
}

int cred_in_group(const cred_t *c, uint32_t gid)
{
	if (c->egid == gid)
		return 1;
	for (uint32_t i = 0; i < c->ngroups && i < NGROUPS_MAX; i++)
		if (c->groups[i] == gid)
			return 1;
	return 0;
}

int cred_setuid(cred_t *c, uint32_t uid)
{
	if (uid == CRED_NOCHANGE)
		return 0;
	if (c->euid == 0) { /* privileged: set all three */
		c->uid = c->euid = c->suid = uid;
		c->fsuid = uid;
		return 0;
	}
	if (uid == c->uid ||
	    uid == c->suid) { /* unprivileged: effective only */
		c->euid = uid;
		c->fsuid = uid;
		return 0;
	}
	return -EPERM;
}

int cred_setgid(cred_t *c, uint32_t gid)
{
	if (gid == CRED_NOCHANGE)
		return 0;
	if (c->euid == 0) { /* privilege is effective uid 0 */
		c->gid = c->egid = c->sgid = gid;
		c->fsgid = gid;
		return 0;
	}
	if (gid == c->gid || gid == c->sgid) {
		c->egid = gid;
		c->fsgid = gid;
		return 0;
	}
	return -EPERM;
}

int cred_seteuid(cred_t *c, uint32_t euid)
{
	if (euid == CRED_NOCHANGE)
		return 0;
	if (c->euid == 0 || euid == c->uid || euid == c->euid ||
	    euid == c->suid) {
		c->euid = euid;
		c->fsuid = euid;
		return 0;
	}
	return -EPERM;
}

int cred_setegid(cred_t *c, uint32_t egid)
{
	if (egid == CRED_NOCHANGE)
		return 0;
	if (c->euid == 0 || egid == c->gid || egid == c->egid ||
	    egid == c->sgid) {
		c->egid = egid;
		c->fsgid = egid;
		return 0;
	}
	return -EPERM;
}

int cred_setresuid(cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
	int priv = (c->euid == 0);
	if (!priv) { /* each target must already be ours */
		if (r != CRED_NOCHANGE && r != c->uid && r != c->euid &&
		    r != c->suid)
			return -EPERM;
		if (e != CRED_NOCHANGE && e != c->uid && e != c->euid &&
		    e != c->suid)
			return -EPERM;
		if (s != CRED_NOCHANGE && s != c->uid && s != c->euid &&
		    s != c->suid)
			return -EPERM;
	}
	if (r != CRED_NOCHANGE)
		c->uid = r;
	if (e != CRED_NOCHANGE) {
		c->euid = e;
		c->fsuid = e;
	}
	if (s != CRED_NOCHANGE)
		c->suid = s;
	return 0;
}

/* Core check against an explicit (cuid, cgid) identity — fs IDs for real file
 * access, real IDs for access(2). */
static int cred_access_impl(const cred_t *c, uint32_t mode, uint32_t fuid,
			    uint32_t fgid, int want, uint32_t cuid,
			    uint32_t cgid)
{
	if (cuid == 0) { /* privileged: bypass r/w */
		if (want & MAY_EXEC) { /* but exec needs an x bit */
			if (S_ISDIR(mode))
				return 0; /* dirs always searchable */
			if (mode & 0111)
				return 0;
			return -EACCES;
		}
		return 0;
	}
	int grp = (cgid == fgid);
	if (!grp)
		for (uint32_t i = 0; i < c->ngroups && i < NGROUPS_MAX; i++)
			if (c->groups[i] == fgid) {
				grp = 1;
				break;
			}
	uint32_t perm;
	if (cuid == fuid)
		perm = (mode >> 6) & 7; /* owner */
	else if (grp)
		perm = (mode >> 3) & 7; /* group */
	else
		perm = mode & 7; /* other */
	if ((int)(perm & (uint32_t)want) == want)
		return 0;
	return -EACCES;
}

/* Real file access: uses the filesystem IDs (which track the effective IDs). */
int cred_check_access(const cred_t *c, uint32_t mode, uint32_t fuid,
		      uint32_t fgid, int want)
{
	return cred_access_impl(c, mode, fuid, fgid, want, c->fsuid, c->fsgid);
}

/* access(2)/faccessat: POSIX checks against the REAL uid/gid, not effective. */
int cred_check_access_real(const cred_t *c, uint32_t mode, uint32_t fuid,
			   uint32_t fgid, int want)
{
	return cred_access_impl(c, mode, fuid, fgid, want, c->uid, c->gid);
}

/* ---- POSIX ACL (system.posix_acl_access) ----
 * On-disk format (little-endian): a 4-byte version, then entries each with a
 * 2-byte tag and 2-byte perm, plus a 4-byte id for ACL_USER / ACL_GROUP only.
 * Entries are ordered: USER_OBJ, named USERs, GROUP_OBJ, named GROUPs, MASK,
 * OTHER.  The mask limits named-user and all group permissions. */
#define ACL_EA_VERSION 0x0001
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

static inline uint16_t acl_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t acl_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
/* Group match mirroring cred_access_impl: the selected primary gid + the
 * supplementary groups (NOT egid), so ACL and mode-bit checks agree. */
static int acl_in_group(const cred_t *c, uint32_t gid, uint32_t cgid)
{
	if (cgid == gid)
		return 1;
	for (uint32_t i = 0; i < c->ngroups && i < NGROUPS_MAX; i++)
		if (c->groups[i] == gid)
			return 1;
	return 0;
}

int cred_acl_access(const cred_t *c, const void *acl, unsigned len,
		    uint32_t fuid, uint32_t fgid, int want, int use_real)
{
	uint32_t cuid = use_real ? c->uid : c->fsuid;
	uint32_t cgid = use_real ? c->gid : c->fsgid;
	if (cuid == 0)
		return 1; /* root: not restricted by the ACL */
	if (!acl || len < 4)
		return 1;
	const uint8_t *p = (const uint8_t *)acl;
	if (acl_le32(p) != ACL_EA_VERSION)
		return 1;
	p += 4;
	len -= 4;
	want &= (MAY_READ | MAY_WRITE | MAY_EXEC);

	/* Pass 1: find the mask (limits named-user and group entries). */
	int mask = 7;
	for (const uint8_t *q = p; (q + 4) <= (p + len);) {
		uint16_t tag = acl_le16(q);
		unsigned esz = (tag == ACL_USER || tag == ACL_GROUP) ? 8u : 4u;
		if (q + esz > p + len)
			break;
		if (tag == ACL_MASK)
			mask = acl_le16(q + 2) & 7;
		q += esz;
	}

	/* Pass 2: the POSIX permission algorithm. */
	int found_group = 0, group_ok = 0;
	while (len >= 4) {
		uint16_t tag = acl_le16(p);
		int perm = acl_le16(p + 2) & 7;
		unsigned esz = (tag == ACL_USER || tag == ACL_GROUP) ? 8u : 4u;
		if (len < esz)
			break;
		uint32_t id = (esz == 8) ? acl_le32(p + 4) : 0;
		switch (tag) {
		case ACL_USER_OBJ:
			if (cuid == fuid)
				return ((perm & want) == want) ? 0 : -EACCES;
			break;
		case ACL_USER:
			if (cuid == id)
				return ((perm & mask & want) == want) ? 0 :
									-EACCES;
			break;
		case ACL_GROUP_OBJ:
			if (acl_in_group(c, fgid, cgid)) {
				found_group = 1;
				if ((perm & mask & want) == want)
					group_ok = 1;
			}
			break;
		case ACL_GROUP:
			if (acl_in_group(c, id, cgid)) {
				found_group = 1;
				if ((perm & mask & want) == want)
					group_ok = 1;
			}
			break;
		case ACL_OTHER:
			if (found_group)
				return group_ok ? 0 : -EACCES;
			return ((perm & want) == want) ? 0 : -EACCES;
		default:
			break; /* ACL_MASK / unknown: skip */
		}
		p += esz;
		len -= esz;
	}
	return -EACCES; /* malformed (no OTHER entry) */
}

int cred_setresgid(cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
	int priv = (c->euid == 0); /* privilege is effective uid 0 */
	if (!priv) {
		if (r != CRED_NOCHANGE && r != c->gid && r != c->egid &&
		    r != c->sgid)
			return -EPERM;
		if (e != CRED_NOCHANGE && e != c->gid && e != c->egid &&
		    e != c->sgid)
			return -EPERM;
		if (s != CRED_NOCHANGE && s != c->gid && s != c->egid &&
		    s != c->sgid)
			return -EPERM;
	}
	if (r != CRED_NOCHANGE)
		c->gid = r;
	if (e != CRED_NOCHANGE) {
		c->egid = e;
		c->fsgid = e;
	}
	if (s != CRED_NOCHANGE)
		c->sgid = s;
	return 0;
}

/* ---- Current-task credential accessors ------------------------------------ */

cred_t *current_cred(void)
{
	task_t *t = sched_current();
	return t ? &t->cred : NULL;
}

/* Current task's canonical absolute working directory; "/" when unset or in
 * kernel context.  Used by the VFS to resolve a relative path for the ancestor
 * search-permission traversal. */
const char *current_cwd(void)
{
	task_t *t = sched_current();
	return (t && t->cwd[0]) ? t->cwd : "/";
}

uint32_t current_uid(void)
{
	cred_t *c = current_cred();
	return c ? c->uid : 0; /* no task == kernel context == root */
}

uint32_t current_euid(void)
{
	cred_t *c = current_cred();
	return c ? c->euid : 0;
}

uint32_t current_gid(void)
{
	cred_t *c = current_cred();
	return c ? c->gid : 0;
}

uint32_t current_egid(void)
{
	cred_t *c = current_cred();
	return c ? c->egid : 0;
}

uint32_t current_fsuid(void)
{
	cred_t *c = current_cred();
	return c ? c->fsuid : 0;
}

uint32_t current_fsgid(void)
{
	cred_t *c = current_cred();
	return c ? c->fsgid : 0;
}

int current_in_group(uint32_t gid)
{
	cred_t *c = current_cred();
	return c ? cred_in_group(c, gid) : 1; /* kernel context: in any group */
}

int current_is_root(void)
{
	return current_euid() == 0;
}

int capable(void)
{
	/* The lone privilege test today: effective uid 0.  Routing every gate
	 * through here means a future capability set slots in at one place. */
	return current_euid() == 0;
}

/* ---- Cross-process access control ----------------------------------------- */

/* One thread's credentials against the caller's, for the given mode.  Split
 * out so the group walk in task_may_access() applies exactly the same rule to
 * every member without duplicating it. */
static int cred_may_access_one(const cred_t *c, const cred_t *t, int mode)
{
	/* The caller's EFFECTIVE uid must match all three of the target's, so a
	 * process that changed identity through a setuid exec is opaque to the
	 * user who started it -- its state is no longer theirs. */
	if (!(c->euid == t->uid && c->euid == t->euid && c->euid == t->suid))
		return -EPERM;

	/* Reading a map is one thing; taking over execution is another, so
	 * attaching demands the same three-way match on the GROUP ids too.  A
	 * setgid binary's secrets are worth exactly as much as a setuid one's,
	 * and checking uids alone would hand every one of them over. */
	if (mode == ACCESS_ATTACH &&
	    !(c->egid == t->gid && c->egid == t->egid && c->egid == t->sgid))
		return -EPERM;

	return 0;
}

int task_may_access(const task_t *target, int mode)
{
	task_t *cur = sched_current();

	if (!target)
		return -ESRCH;
	if (!cur)
		return 0; /* kernel context is privileged by definition */
	if (capable())
		return 0; /* root may observe and control anything */

	/* Taking control of a process whose privileges were raised by a set-id
	 * exec is refused outright for the unprivileged caller.  The id
	 * comparison below would already refuse it in the normal case, but a
	 * binary that dropped back to the invoking user still holds whatever
	 * it read while it was privileged, and its memory is what an attacker
	 * is after. */
	if (mode == ACCESS_ATTACH && !task_dumpable((task_t *)target))
		return -EPERM;

	/* Evaluated against EVERY thread of the target's group, strictest
	 * answer winning.  Credentials here are per-task, not per-thread-group
	 * (see cred.h), so two threads of one process can hold different ids
	 * indefinitely -- and they share one address space.  Checking only the
	 * thread that was named would let a caller reach a privileged thread's
	 * memory by picking whichever sibling happened to be weakest. */
	const task_t *leader = target->group_leader ? target->group_leader :
						      target;

	if (!task_ptr_ok(leader)) {
		/* A ring we cannot trust is not a ring we can clear anyone
		 * against.  Deny rather than guess. */
		WARN_ON_ONCE(1);
		return -EPERM;
	}

	const task_t *t = leader;
	int guard = 0;

	do {
		if (cred_may_access_one(&cur->cred, &t->cred, mode) != 0)
			return -EPERM;

		t = t->thread_group_next;

		/* Bounded exactly as the thread-group kill walk is, and for the
		 * same reason: this runs under g_task_list_lock with interrupts
		 * off, where following a broken ring faults unrecoverably and
		 * wedges the machine still holding the lock.  Deny on a ring we
		 * cannot walk -- whatever broke it is a separate defect that
		 * must not become an access-control bypass. */
		if (!task_ptr_ok(t) || ++guard > TASK_GROUP_KILL_MAX * 4) {
			WARN_ON_ONCE(1);
			return -EPERM;
		}
	} while (t != leader);

	return 0;
}


/* Real credential syscalls.  Operate on the current task's embedded
 * cred; the set*-id permission rules live in cred.c.  Enforcement of file
 * permissions against these IDs is done by the perm_check and perm_traverse helpers above. */
int64_t sys_getuid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.uid : 0;
}

int64_t sys_geteuid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.euid : 0;
}

int64_t sys_getgid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.gid : 0;
}

int64_t sys_getegid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.egid : 0;
}


int64_t sys_setuid(uint64_t uid)
{
	task_t *c = sched_current();
	return c ? cred_setuid(&c->cred, (uint32_t)uid) : -EPERM;
}

int64_t sys_setgid(uint64_t gid)
{
	task_t *c = sched_current();
	return c ? cred_setgid(&c->cred, (uint32_t)gid) : -EPERM;
}

int64_t sys_seteuid(uint64_t uid)
{
	task_t *c = sched_current();
	return c ? cred_seteuid(&c->cred, (uint32_t)uid) : -EPERM;
}

int64_t sys_setegid(uint64_t gid)
{
	task_t *c = sched_current();
	return c ? cred_setegid(&c->cred, (uint32_t)gid) : -EPERM;
}


int64_t sys_getgroups(uint64_t size, uint64_t list)
{
	task_t *c = sched_current();
	uint32_t n = c ? c->cred.ngroups : 0;
	if (size == 0)
		return (int64_t)n; /* query count */
	if (size < n)
		return -EINVAL;
	if (n == 0)
		return 0;
	if (!validate_user_ptr(list, sizeof(int) * (size_t)n))
		return -EFAULT;
	int tmp[NGROUPS_MAX];
	for (uint32_t i = 0; i < n; i++)
		tmp[i] = (int)c->cred.groups[i];
	if (copy_to_user((void *)list, tmp, sizeof(int) * (size_t)n) < 0)
		return -EFAULT;
	return (int64_t)n;
}


int64_t sys_setgroups(uint64_t size, uint64_t list)
{
	task_t *c = sched_current();
	if (!c)
		return -EPERM;
	if (c->cred.euid != 0)
		return -EPERM; /* privileged only */
	if (size > NGROUPS_MAX)
		return -EINVAL;
	if (size == 0) {
		c->cred.ngroups = 0;
		return 0;
	}
	if (!validate_user_ptr(list, sizeof(int) * (size_t)size))
		return -EFAULT;
	int tmp[NGROUPS_MAX];
	if (copy_from_user(tmp, (const void *)list,
			   sizeof(int) * (size_t)size) < 0)
		return -EFAULT;
	for (uint64_t i = 0; i < size; i++)
		c->cred.groups[i] = (uint32_t)tmp[i];
	c->cred.ngroups = (uint32_t)size;
	return 0;
}


int64_t sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
	task_t *c = sched_current();
	return c ? cred_setresuid(&c->cred, (uint32_t)ruid, (uint32_t)euid,
				  (uint32_t)suid) :
		   -EPERM;
}

int64_t sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
	task_t *c = sched_current();
	return c ? cred_setresgid(&c->cred, (uint32_t)rgid, (uint32_t)egid,
				  (uint32_t)sgid) :
		   -EPERM;
}

int64_t sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
	task_t *c = sched_current();
	if (!c)
		return -EFAULT;
	int vals[3] = { (int)c->cred.uid, (int)c->cred.euid,
			(int)c->cred.suid };
	uint64_t ptrs[3] = { ruid, euid, suid };
	for (int i = 0; i < 3; i++) {
		if (!ptrs[i])
			continue;
		if (!validate_user_ptr(ptrs[i], sizeof(int)))
			return -EFAULT;
		if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(int)) < 0)
			return -EFAULT;
	}
	return 0;
}

int64_t sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
	task_t *c = sched_current();
	if (!c)
		return -EFAULT;
	int vals[3] = { (int)c->cred.gid, (int)c->cred.egid,
			(int)c->cred.sgid };
	uint64_t ptrs[3] = { rgid, egid, sgid };
	for (int i = 0; i < 3; i++) {
		if (!ptrs[i])
			continue;
		if (!validate_user_ptr(ptrs[i], sizeof(int)))
			return -EFAULT;
		if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(int)) < 0)
			return -EFAULT;
	}
	return 0;
}

