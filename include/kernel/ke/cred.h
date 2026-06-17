// LikeOS-64 - UNIX process credentials (P5)
//
// A per-task credential set: real / effective / saved-set user and group IDs,
// the filesystem IDs (which track the effective IDs), and the supplementary
// group list.  Embedded directly in task_t, so fork()/clone() inherit it for
// free via the task struct memcpy.  Fresh (non-fork) tasks inherit the spawning
// task's credentials; kernel tasks run privileged and the primordial boot task
// is the sole root origin — no task is silently born root.  Permission
// ENFORCEMENT (P5b) consults the effective/fs IDs; P5a only stores and reports
// them and implements the POSIX set*-id transitions.
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
    uint32_t uid,  gid;     /* real IDs                                   */
    uint32_t euid, egid;    /* effective IDs (used for permission checks) */
    uint32_t suid, sgid;    /* saved set-user/group IDs                   */
    uint32_t fsuid, fsgid;  /* filesystem IDs (track the effective IDs)   */
    uint32_t ngroups;       /* number of supplementary groups in use      */
    uint32_t groups[NGROUPS_MAX];
} cred_t;

/* Seed `c` as the root credential (all IDs 0, no supplementary groups). */
void cred_init_root(cred_t *c);

/* True if `c` is privileged (effective uid 0). */
int  cred_is_root(const cred_t *c);

/* True if `gid` is the effective gid or one of the supplementary groups. */
int  cred_in_group(const cred_t *c, uint32_t gid);

/* Access-check request bits (match the on-disk rwx layout: r=4,w=2,x=1). */
#define MAY_EXEC   1
#define MAY_WRITE  2
#define MAY_READ   4

/* Decide whether `c` may access a file of the given `mode` (full i_mode,
 * including the S_IFMT type bits) owned by fuid:fgid, for the `want` access
 * (MAY_* mask).  Uses the filesystem IDs (fsuid/fsgid) per POSIX.  Effective
 * uid 0 bypasses read/write; execute still requires at least one x bit (or a
 * directory, which is always searchable by root).  Returns 0 if allowed or
 * -EACCES if denied. */
int  cred_check_access(const cred_t *c, uint32_t mode,
                       uint32_t fuid, uint32_t fgid, int want);

/* Like cred_check_access but against the REAL uid/gid (for access(2)). */
int  cred_check_access_real(const cred_t *c, uint32_t mode,
                            uint32_t fuid, uint32_t fgid, int want);

/* POSIX set*-id transitions on a credential.  Return 0 on success or a
 * negative errno (-EPERM).  CRED_NOCHANGE leaves a field unchanged.
 * Privilege is "effective uid == 0". */
int  cred_setuid(cred_t *c, uint32_t uid);
int  cred_setgid(cred_t *c, uint32_t gid);
int  cred_seteuid(cred_t *c, uint32_t euid);
int  cred_setegid(cred_t *c, uint32_t egid);
int  cred_setresuid(cred_t *c, uint32_t r, uint32_t e, uint32_t s);
int  cred_setresgid(cred_t *c, uint32_t r, uint32_t e, uint32_t s);

#endif /* LIKEOS_CRED_H */
