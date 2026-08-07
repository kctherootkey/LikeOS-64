/*
 * strings.h - BSD string functions for LikeOS
 */
#ifndef _STRINGS_H
#define _STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

/* Least significant set bit, numbered from 1; 0 if no bits are set. */
int ffs(int i);
int ffsl(long i);
int ffsll(long long i);

/* These four are the BSD spellings that predate the <string.h> ones.  Each is
 * guarded because headers elsewhere still define them as macros for platforms
 * that lack them — X11's Xfuncs.h does exactly that:
 *
 *     #define bzero(b,len) memset(b, 0, len)
 *
 * and with the macro already in scope, a function definition of the same name
 * expands into nonsense at its parameter list.  Skipping our definition when
 * the name is already taken lets both conventions coexist, which is what a
 * caller including both headers needs. */
#ifndef bzero
static __inline void bzero(void *s, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = 0;
}
#endif

#ifndef bcopy
static __inline void bcopy(const void *src, void *dst, size_t n)
{
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = *s++;
}
#endif

#ifndef index
static __inline char *index(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}
#endif

#ifndef rindex
static __inline char *rindex(const char *s, int c)
{
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* _STRINGS_H */
