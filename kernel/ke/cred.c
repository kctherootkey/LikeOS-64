// LikeOS-64 - UNIX process credentials (P5)
//
// The set*-id transition rules below follow POSIX: a privileged process
// (effective uid 0) may set IDs freely; an unprivileged process may only
// switch among its real, effective and saved-set IDs.  The filesystem IDs
// track the effective IDs (we do not expose a separate setfsuid/setfsgid).

#include <kernel/ke/cred.h>
#include <kernel/ke/syscall.h>   /* EPERM, EACCES */
#include <kernel/uapi/stat.h>    /* S_ISDIR */

void cred_init_root(cred_t *c)
{
    c->uid  = c->gid  = 0;
    c->euid = c->egid = 0;
    c->suid = c->sgid = 0;
    c->fsuid = c->fsgid = 0;
    c->ngroups = 0;
    for (unsigned i = 0; i < NGROUPS_MAX; i++)
        c->groups[i] = 0;
}

int cred_is_root(const cred_t *c) { return c->euid == 0; }

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
    if (c->euid == 0) {                       /* privileged: set all three */
        c->uid = c->euid = c->suid = uid;
        c->fsuid = uid;
        return 0;
    }
    if (uid == c->uid || uid == c->suid) {    /* unprivileged: effective only */
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
    if (c->euid == 0) {                       /* privilege is effective uid 0 */
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
    if (c->euid == 0 || euid == c->uid || euid == c->euid || euid == c->suid) {
        c->euid  = euid;
        c->fsuid = euid;
        return 0;
    }
    return -EPERM;
}

int cred_setegid(cred_t *c, uint32_t egid)
{
    if (egid == CRED_NOCHANGE)
        return 0;
    if (c->euid == 0 || egid == c->gid || egid == c->egid || egid == c->sgid) {
        c->egid  = egid;
        c->fsgid = egid;
        return 0;
    }
    return -EPERM;
}

int cred_setresuid(cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
    int priv = (c->euid == 0);
    if (!priv) {                              /* each target must already be ours */
        if (r != CRED_NOCHANGE && r != c->uid && r != c->euid && r != c->suid) return -EPERM;
        if (e != CRED_NOCHANGE && e != c->uid && e != c->euid && e != c->suid) return -EPERM;
        if (s != CRED_NOCHANGE && s != c->uid && s != c->euid && s != c->suid) return -EPERM;
    }
    if (r != CRED_NOCHANGE) c->uid  = r;
    if (e != CRED_NOCHANGE) { c->euid = e; c->fsuid = e; }
    if (s != CRED_NOCHANGE) c->suid = s;
    return 0;
}

/* Core check against an explicit (cuid, cgid) identity — fs IDs for real file
 * access, real IDs for access(2). */
static int cred_access_impl(const cred_t *c, uint32_t mode,
                            uint32_t fuid, uint32_t fgid, int want,
                            uint32_t cuid, uint32_t cgid)
{
    if (cuid == 0) {                          /* privileged: bypass r/w */
        if (want & MAY_EXEC) {                /* but exec needs an x bit */
            if (S_ISDIR(mode)) return 0;      /* dirs always searchable */
            if (mode & 0111) return 0;
            return -EACCES;
        }
        return 0;
    }
    int grp = (cgid == fgid);
    if (!grp)
        for (uint32_t i = 0; i < c->ngroups && i < NGROUPS_MAX; i++)
            if (c->groups[i] == fgid) { grp = 1; break; }
    uint32_t perm;
    if (cuid == fuid)   perm = (mode >> 6) & 7;   /* owner */
    else if (grp)       perm = (mode >> 3) & 7;   /* group */
    else                perm = mode & 7;          /* other */
    if ((int)(perm & (uint32_t)want) == want)
        return 0;
    return -EACCES;
}

/* Real file access: uses the filesystem IDs (which track the effective IDs). */
int cred_check_access(const cred_t *c, uint32_t mode,
                      uint32_t fuid, uint32_t fgid, int want)
{
    return cred_access_impl(c, mode, fuid, fgid, want, c->fsuid, c->fsgid);
}

/* access(2)/faccessat: POSIX checks against the REAL uid/gid, not effective. */
int cred_check_access_real(const cred_t *c, uint32_t mode,
                           uint32_t fuid, uint32_t fgid, int want)
{
    return cred_access_impl(c, mode, fuid, fgid, want, c->uid, c->gid);
}

int cred_setresgid(cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
    int priv = (c->euid == 0);                /* privilege is effective uid 0 */
    if (!priv) {
        if (r != CRED_NOCHANGE && r != c->gid && r != c->egid && r != c->sgid) return -EPERM;
        if (e != CRED_NOCHANGE && e != c->gid && e != c->egid && e != c->sgid) return -EPERM;
        if (s != CRED_NOCHANGE && s != c->gid && s != c->egid && s != c->sgid) return -EPERM;
    }
    if (r != CRED_NOCHANGE) c->gid  = r;
    if (e != CRED_NOCHANGE) { c->egid = e; c->fsgid = e; }
    if (s != CRED_NOCHANGE) c->sgid = s;
    return 0;
}
