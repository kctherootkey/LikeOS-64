/*
 * unicode.h - internal interface to the generated Unicode character tables.
 *
 * Not installed: <wctype.h> and <wchar.h> are the public face of this.  The
 * tables themselves live in unicode.c, which host/gen-unicode-tables.c
 * produces.
 */
#ifndef _LIBC_UNICODE_H
#define _LIBC_UNICODE_H

#include <stddef.h>

struct range {
	unsigned lo, hi;
};

struct delta_range {
	unsigned lo, hi;
	int delta;
};

int __uni_in_range(const struct range *tab, size_t n, unsigned cp);

int __uni_isalpha(unsigned cp);
int __uni_isupper(unsigned cp);
int __uni_islower(unsigned cp);
int __uni_ispunct(unsigned cp);
int __uni_isprint(unsigned cp);
int __uni_isspace(unsigned cp);
int __uni_iscntrl(unsigned cp);
int __uni_isblank(unsigned cp);
int __uni_isgraph(unsigned cp);
int __uni_isalnum(unsigned cp);
int __uni_isdigit(unsigned cp);
int __uni_isxdigit(unsigned cp);

/* Terminal columns: -1 (not printable), 0, 1 or 2. */
int __uni_width(unsigned cp);

unsigned __uni_toupper(unsigned cp);
unsigned __uni_tolower(unsigned cp);

#endif /* _LIBC_UNICODE_H */
