/*
 * pwd.h - user database (/etc/passwd) for LikeOS
 */
#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;    /* username */
    char  *pw_passwd;  /* password (usually "x"; real hash is in shadow) */
    uid_t  pw_uid;     /* user ID */
    gid_t  pw_gid;     /* group ID */
    char  *pw_gecos;   /* real name / comment */
    char  *pw_dir;     /* home directory */
    char  *pw_shell;   /* login shell */
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result);

struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

int putpwent(const struct passwd *pwd, FILE *stream);
struct passwd *fgetpwent(FILE *stream);
int fgetpwent_r(FILE *stream, struct passwd *pwd, char *buf, size_t buflen,
                struct passwd **result);

#ifdef __cplusplus
}
#endif

#endif /* _PWD_H */
