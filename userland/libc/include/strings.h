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

static inline void bzero(void *s, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = 0;
}

static inline void bcopy(const void *src, void *dst, size_t n)
{
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = *s++;
}

static inline char *index(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}

static inline char *rindex(const char *s, int c)
{
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

#ifdef __cplusplus
}
#endif

#endif /* _STRINGS_H */
