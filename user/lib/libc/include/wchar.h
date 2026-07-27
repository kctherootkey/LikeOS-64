/*
 * wchar.h - wide character support stub for LikeOS
 * Minimal definitions; full wide character support not implemented.
 */
#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
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
typedef int wchar_t;

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

static inline int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    if (!s) return 0;
    if (n == 0) return -1;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return (*s != '\0') ? 1 : 0;
}

static inline int wctomb(char *s, wchar_t wc)
{
    if (!s) return 0;
    *s = (char)(wc & 0xFF);
    return 1;
}

static inline size_t mbstowcs(wchar_t *dest, const char *src, size_t n)
{
    size_t i;
    if (!src) return (size_t)-1;
    for (i = 0; i < n && src[i]; i++) {
        if (dest) dest[i] = (wchar_t)(unsigned char)src[i];
    }
    if (dest && i < n) dest[i] = 0;
    return i;
}

static inline size_t wcstombs(char *dest, const wchar_t *src, size_t n)
{
    size_t i;
    if (!src) return (size_t)-1;
    for (i = 0; i < n && src[i]; i++) {
        if (dest) dest[i] = (char)(src[i] & 0xFF);
    }
    if (dest && i < n) dest[i] = 0;
    return i;
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

#endif /* _WCHAR_H */
