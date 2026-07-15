// LikeOS-64 - UNIX process credentials
//
// A per-task credential set: real / effective / saved-set user and group IDs,
// the filesystem IDs (which track the effective IDs), and the supplementary
// group list.  Embedded directly in task_t, so fork()/clone() inherit it for
// free via the task struct memcpy.  Fresh (non-fork) tasks inherit the spawning
// task's credentials; kernel tasks run privileged and the primordial boot task
// is the sole root origin — no task is silently born root.  Permission
// enforcement consults the effective/fs IDs; this credential layer only stores
// and reports them and implements the POSIX set*-id transitions.
//
// Note: threads (CLONE_THREAD) currently get an independent copy rather than a
// shared credential — full POSIX shared-cred semantics is a later refinement.
#ifndef LIKEOS_CRED_H
#define LIKEOS_CRED_H

#include <kernel/uapi/types.h>

#define NGROUPS_MAX 32

/* Passed to the set*-id helpers to mean "leave this field unchanged"
 * (matches the (uid_t)-1 convention of setresuid/setreuid). */
#define CRED_NOCHANGE 0xFFFFFFFFu

typedef struct cred {
	uint32_t uid, gid; /* real IDs                                   */
	uint32_t euid, egid; /* effective IDs (used for permission checks) */
	uint32_t suid, sgid; /* saved set-user/group IDs                   */
	uint32_t fsuid, fsgid; /* filesystem IDs (track the effective IDs)   */
	uint32_t ngroups; /* number of supplementary groups in use      */
	uint32_t groups[NGROUPS_MAX];
} cred_t;

/* Seed `c` as the root credential (all IDs 0, no supplementary groups). */
void cred_init_root(cred_t *c);

/* True if `c` is privileged (effective uid 0). */
int cred_is_root(const cred_t *c);

/* True if `gid` is the effective gid or one of the supplementary groups. */
int cred_in_group(const cred_t *c, uint32_t gid);

/* Access-check request bits (match the on-disk rwx layout: r=4,w=2,x=1). */
#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/* Decide whether `c` may access a file of the given `mode` (full i_mode,
 * including the S_IFMT type bits) owned by fuid:fgid, for the `want` access
 * (MAY_* mask).  Uses the filesystem IDs (fsuid/fsgid) per POSIX.  Effective
 * uid 0 bypasses read/write; execute still requires at least one x bit (or a
 * directory, which is always searchable by root).  Returns 0 if allowed or
 * -EACCES if denied. */
int cred_check_access(const cred_t *c, uint32_t mode, uint32_t fuid,
		      uint32_t fgid, int want);

/* Like cred_check_access but against the REAL uid/gid (for access(2)). */
int cred_check_access_real(const cred_t *c, uint32_t mode, uint32_t fuid,
			   uint32_t fgid, int want);

/* Evaluate a POSIX access ACL (raw system.posix_acl_access xattr bytes) for the
 * `want` access on a file owned by fuid:fgid.  use_real selects the real vs the
 * effective/fs ids (to mirror access(2) vs normal checks).  Returns 0 (allow),
 * -EACCES (deny), or 1 if there is no usable ACL (caller falls back to the mode
 * bits).  Root (the selected uid == 0) always returns 1. */
int cred_acl_access(const cred_t *c, const void *acl, unsigned len,
		    uint32_t fuid, uint32_t fgid, int want, int use_real);

/* POSIX set*-id transitions on a credential.  Return 0 on success or a
 * negative errno (-EPERM).  CRED_NOCHANGE leaves a field unchanged.
 * Privilege is "effective uid == 0". */
int cred_setuid(cred_t *c, uint32_t uid);
int cred_setgid(cred_t *c, uint32_t gid);
int cred_seteuid(cred_t *c, uint32_t euid);
int cred_setegid(cred_t *c, uint32_t egid);
int cred_setresuid(cred_t *c, uint32_t r, uint32_t e, uint32_t s);
int cred_setresgid(cred_t *c, uint32_t r, uint32_t e, uint32_t s);

/* ---- Current-task credential accessors --------------------------------------
 * Thin helpers that fetch the calling task's credentials (via the scheduler),
 * so callers throughout the kernel don't reach into task internals.  When there
 * is no current task (very early boot / pure kernel context) the identity is
 * root, matching the rule that kernel context is privileged.  These take no
 * task argument by design — they always operate on the caller. */
cred_t *current_cred(void); /* &current->cred, or NULL in kernel context */
const char *current_cwd(void); /* current task cwd (absolute), "/" if unset */
uint32_t current_uid(void);
uint32_t current_euid(void);
uint32_t current_gid(void);
uint32_t current_egid(void);
uint32_t current_fsuid(void);
uint32_t current_fsgid(void);
int current_in_group(uint32_t gid); /* 1 if egid or a supplementary group */
int current_is_root(void); /* 1 if effective uid 0 */

/* The single privilege chokepoint.  Today this is simply "effective uid 0";
 * every privileged-operation gate in the kernel routes through it so a real
 * capability model can be introduced later without touching call sites.
 * Returns non-zero if the caller is permitted the privileged action. */
int capable(void);

#endif /* LIKEOS_CRED_H */
