/*
 * pwd.h - password/user database for LikeOS
 * Stub implementation - LikeOS is single-user.
 */
#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;    /* username */
    char  *pw_passwd;  /* password (unused) */
    uid_t  pw_uid;     /* user ID */
    gid_t  pw_gid;     /* group ID */
    char  *pw_gecos;   /* real name */
    char  *pw_dir;     /* home directory */
    char  *pw_shell;   /* shell program */
};

static inline struct passwd *getpwuid(uid_t uid)
{
    (void)uid;
    static struct passwd pw = {
        .pw_name   = "root",
        .pw_passwd = "",
        .pw_uid    = 0,
        .pw_gid    = 0,
        .pw_gecos  = "root",
        .pw_dir    = "/",
        .pw_shell  = "/bin/sh",
    };
    return &pw;
}

static inline struct passwd *getpwnam(const char *name)
{
    (void)name;
    return getpwuid(0);
}

static inline struct passwd *getpwent(void)
{
    static int _called = 0;
    if (_called) return (struct passwd *)0;
    _called = 1;
    return getpwuid(0);
}

static inline void endpwent(void)
{
    /* nothing to do */
}

static inline void setpwent(void)
{
    /* nothing to do */
}

#ifdef __cplusplus
}
#endif

#endif /* _PWD_H */
