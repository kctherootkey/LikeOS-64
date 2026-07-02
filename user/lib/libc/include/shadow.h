/*
 * shadow.h - shadow password database (/etc/shadow) for LikeOS
 */
#ifndef _SHADOW_H
#define _SHADOW_H

#include <sys/types.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spwd {
    char *sp_namp;    /* login name */
    char *sp_pwdp;    /* encrypted password */
    long  sp_lstchg;  /* days since epoch of last change */
    long  sp_min;     /* min days between changes */
    long  sp_max;     /* max days password is valid */
    long  sp_warn;    /* days before expiry to warn */
    long  sp_inact;   /* days after expiry until account disabled */
    long  sp_expire;  /* days since epoch when account expires */
    unsigned long sp_flag; /* reserved */
};

struct spwd *getspnam(const char *name);
int getspnam_r(const char *name, struct spwd *spbuf, char *buf, size_t buflen,
               struct spwd **spbufp);

struct spwd *getspent(void);
void setspent(void);
void endspent(void);

int putspent(const struct spwd *p, FILE *stream);
struct spwd *fgetspent(FILE *stream);
struct spwd *sgetspent(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* _SHADOW_H */
