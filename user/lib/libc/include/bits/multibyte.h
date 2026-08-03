/*
 * bits/multibyte.h - the non-restartable multibyte<->wide conversions.
 *
 * INTERNAL: include <stdlib.h> or <wchar.h>, not this file.
 *
 * The C standard declares mbtowc/wctomb/mbstowcs/wcstombs in <stdlib.h>, while
 * <wchar.h> is where the rest of the wide-character interface lives.  Both
 * headers therefore have to expose them, and code that includes only <stdlib.h>
 * -- which libX11's locale layer does -- must still see them.  Keeping the one
 * declaration here means the two headers cannot drift apart.
 *
 * The encoding is UTF-8; the implementation is in src/locale/multibyte.c.
 */
#ifndef _BITS_MULTIBYTE_H
#define _BITS_MULTIBYTE_H

#include <stddef.h>

int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);

#endif /* _BITS_MULTIBYTE_H */
