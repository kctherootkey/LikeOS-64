/*
 * grp.h - group database (/etc/group) for LikeOS
 */
#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char  *gr_name;    /* group name */
    char  *gr_passwd;  /* group password (usually "x") */
    gid_t  gr_gid;     /* group ID */
    char **gr_mem;     /* NULL-terminated array of member names */
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result);
int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result);

struct group *getgrent(void);
void setgrent(void);
void endgrent(void);

int putgrent(const struct group *grp, FILE *stream);
struct group *fgetgrent(FILE *stream);

/* Supplementary groups: build the list for `user` from /etc/group (plus the
 * primary `group`) and install it with setgroups(). */
int initgroups(const char *user, gid_t group);

#ifdef __cplusplus
}
#endif

#endif /* _GRP_H */
