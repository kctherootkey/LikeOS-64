/*
 * bits/multibyte.h - the four multibyte<->wide conversions.
 *
 * INTERNAL: include <stdlib.h> or <wchar.h>, not this file.
 *
 * The C standard declares mbtowc/wctomb/mbstowcs/wcstombs in <stdlib.h>, while
 * <wchar.h> is where the rest of the wide-character interface lives.  Both
 * headers therefore have to expose them, and code that includes only <stdlib.h>
 * — which libX11's locale layer does — must still see them.  Keeping the one
 * definition here means the two headers cannot drift apart.
 *
 * These implement the single-byte ("C") locale: one byte is one character.
 * That is the locale this system runs in; UTF-8 conversion is a later change,
 * and it belongs here when it happens.
 */
#ifndef _BITS_MULTIBYTE_H
#define _BITS_MULTIBYTE_H

#include <stddef.h>

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

#endif /* _BITS_MULTIBYTE_H */
