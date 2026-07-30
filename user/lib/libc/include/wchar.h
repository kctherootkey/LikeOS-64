/*
 * wchar.h - wide character support stub for LikeOS
 * Minimal definitions; full wide character support not implemented.
 */
#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <bits/multibyte.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN 0
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX 0x7FFFFFFF
#endif

typedef int wint_t;
#ifndef __wchar_t_defined
#define __wchar_t_defined
typedef int wchar_t;
#endif

/* Multibyte conversion state */
typedef struct {
    unsigned int __count;
    unsigned int __value;
} mbstate_t;

/* Minimal multibyte functions */
static inline int mbsinit(const mbstate_t *ps)
{
    return (!ps || ps->__count == 0);
}

static inline size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (!s) return 0;
    if (n == 0) return (size_t)-2;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return (*s != '\0') ? 1 : 0;
}

static inline size_t mbrlen(const char *s, size_t n, mbstate_t *ps)
{
    return mbrtowc(0, s, n, ps);
}

static inline size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    (void)ps;
    if (!s) return 1;
    *s = (char)(wc & 0xFF);
    return 1;
}









static inline int wcwidth(wchar_t wc)
{
    if (wc == 0) return 0;
    if (wc < 32 || wc == 127) return -1;
    return 1;
}

static inline size_t wcslen(const wchar_t *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static inline wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c) return (wchar_t *)s;
        if (*s == L'\0') return 0;
    }
}

static inline wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = 0;
    for (;; s++) {
        if (*s == c) last = s;
        if (*s == L'\0') return (wchar_t *)last;
    }
}

static inline int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a < *b) ? -1 : (*a > *b) ? 1 : 0;
}

static inline int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        if (*a == L'\0') break;
    }
    return 0;
}

static inline int wcscoll(const wchar_t *a, const wchar_t *b)
{
    return wcscmp(a, b);  /* C locale collation */
}

static inline wchar_t *wcscpy(wchar_t *dest, const wchar_t *src)
{
    wchar_t *d = dest;
    while ((*d++ = *src++) != L'\0')
        ;
    return dest;
}

static inline wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (; n; n--, s++)
        if (*s == c) return (wchar_t *)s;
    return 0;
}

static inline int wcswidth(const wchar_t *s, size_t n)
{
    int w = 0;
    for (; n && *s; n--, s++) {
        int cw = wcwidth(*s);
        if (cw < 0) return -1;
        w += cw;
    }
    return w;
}

static inline wchar_t towlower(wchar_t wc)
{
    if (wc >= 'A' && wc <= 'Z') return wc + 32;
    return wc;
}

static inline wchar_t towupper(wchar_t wc)
{
    if (wc >= 'a' && wc <= 'z') return wc - 32;
    return wc;
}

static inline int btowc(int c)
{
    if (c == -1) return WEOF;
    return (wint_t)(unsigned char)c;
}

static inline int wctob(wint_t c)
{
    if (c == WEOF || c > 255) return -1;
    return (int)(unsigned char)c;
}

static inline size_t mbsrtowcs(wchar_t *dest, const char **src, size_t len, mbstate_t *ps)
{
    (void)ps;
    if (!src || !*src) return 0;
    return mbstowcs(dest, *src, len);
}

static inline size_t wcsrtombs(char *dest, const wchar_t **src, size_t len, mbstate_t *ps)
{
    (void)ps;
    if (!src || !*src) return 0;
    return wcstombs(dest, *src, len);
}

#ifdef __cplusplus
}
#endif


/* The rest of the wide-character string interface.  These mirror their <string.h>
 * counterparts exactly, operating on wchar_t units rather than bytes, and are
 * defined here as inlines for the same reason the existing ones are: they are
 * short, and a caller that includes this header should not need a library
 * symbol for a two-line loop. */
static inline wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;          /* pad, per the standard */
    return d;
}

static inline wchar_t *wcscat(wchar_t *d, const wchar_t *s)
{
    wchar_t *p = d;
    while (*p) p++;
    while ((*p++ = *s++)) ;
    return d;
}

static inline wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
    wchar_t *p = d;
    while (*p) p++;
    while (n-- && *s) *p++ = *s++;
    *p = 0;                                /* always terminated, unlike wcsncpy */
    return d;
}

static inline wchar_t *wcsstr(const wchar_t *h, const wchar_t *nd)
{
    if (!*nd) return (wchar_t *)h;
    for (; *h; h++) {
        const wchar_t *a = h, *b = nd;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (wchar_t *)h;
    }
    return 0;
}

static inline size_t wcsspn(const wchar_t *s, const wchar_t *set)
{
    size_t n = 0;
    for (; s[n]; n++) {
        const wchar_t *p = set;
        while (*p && *p != s[n]) p++;
        if (!*p) break;
    }
    return n;
}

static inline size_t wcscspn(const wchar_t *s, const wchar_t *set)
{
    size_t n = 0;
    for (; s[n]; n++) {
        const wchar_t *p = set;
        while (*p && *p != s[n]) p++;
        if (*p) break;
    }
    return n;
}

static inline wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set)
{
    s += wcscspn(s, set);
    return *s ? (wchar_t *)s : 0;
}

/* Reentrant: the caller owns the cursor, as with strtok_r. */
static inline wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **save)
{
    wchar_t *tok;
    if (!save) return 0;
    if (!s) s = *save;
    if (!s) return 0;
    s += wcsspn(s, sep);
    if (!*s) { *save = 0; return 0; }
    tok = s;
    s += wcscspn(s, sep);
    if (*s) { *s = 0; *save = s + 1; } else { *save = 0; }
    return tok;
}

static inline wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return d;
}

static inline wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else { while (n--) d[n] = s[n]; }      /* overlapping: copy backwards */
    return d;
}

static inline wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = c;
    return d;
}

static inline int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

#endif /* _WCHAR_H */
