/*
 * wchar.h - wide character interface.
 *
 * wchar_t is 32 bits (the compiler's own wide type on x86_64), so a wide
 * character is a Unicode code point and nothing is lost converting to or from
 * one.  The multibyte encoding is UTF-8 in every locale; see
 * src/locale/multibyte.c for why that is a decision rather than a gap.
 */
#ifndef _WCHAR_H
#define _WCHAR_H

/*
 * <stdint.h> is deliberately NOT included, though it used to be.
 *
 * Nothing here needs it: wint_t is defined below in terms of `unsigned int',
 * and WCHAR_MIN/WCHAR_MAX come from the compiler's own __WCHAR_MIN__ and
 * __WCHAR_MAX__.  It was an include with no user.
 *
 * It was also a cycle waiting for the right consumer.  gnulib ships
 * replacement headers, and its <stdint.h> includes <wchar.h> to find
 * WCHAR_MIN/WCHAR_MAX.  With this header pulling <stdint.h> back in, a program
 * including <wchar.h> got:
 *
 *     gnulib wchar.h -> this header -> gnulib stdint.h -> gnulib wchar.h
 *
 * and the inner visit ran gnulib's declarations while this file was still four
 * lines in, so mbstate_t did not exist yet.  It failed as "unknown type name
 * 'mbstate_t'" inside gnulib's own header, which points at neither end of the
 * loop.  GnuTLS is where it surfaced; every gnulib-bearing package was exposed
 * to it.
 *
 * A header that includes only what it uses cannot be part of such a cycle.
 */
#include <stddef.h>
#include <stdarg.h>
#include <bits/multibyte.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wint_t must hold every wide character plus WEOF, and is unsigned so that
 * WEOF is distinct from every valid code point rather than aliasing one. */
#ifndef __wint_t_defined
#define __wint_t_defined
typedef unsigned int wint_t;
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN __WCHAR_MIN__
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX __WCHAR_MAX__
#endif

/* Conversion state for the restartable functions.  __count packs how many
 * bytes of the character in progress are still outstanding together with how
 * long the whole sequence is; __value holds the bits assembled so far.  Zero
 * in both is the initial state, which is what mbsinit() reports on. */
#ifndef __mbstate_t_defined
#define __mbstate_t_defined
typedef struct {
	unsigned int __count;
	unsigned int __value;
} mbstate_t;
#endif

/* Declared here as well as in <stdio.h>; see the note there. */
#ifndef __FILE_defined
#define __FILE_defined
typedef struct _IO_FILE FILE;
#endif

/* ---- Multibyte <-> wide conversion ---------------------------------- */
int mbsinit(const mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nmc, size_t len,
		  mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
		  mbstate_t *ps);
wint_t btowc(int c);
int wctob(wint_t c);

/* ---- Display width --------------------------------------------------- */
int wcwidth(wchar_t wc);
int wcswidth(const wchar_t *s, size_t n);

/* ---- String handling -------------------------------------------------- */
size_t wcslen(const wchar_t *s);
size_t wcsnlen(const wchar_t *s, size_t n);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *wcpcpy(wchar_t *d, const wchar_t *s);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscasecmp(const wchar_t *a, const wchar_t *b);
int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *h, const wchar_t *needle);
wchar_t *wcswcs(const wchar_t *h, const wchar_t *needle);
size_t wcsspn(const wchar_t *s, const wchar_t *set);
size_t wcscspn(const wchar_t *s, const wchar_t *set);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set);
wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **save);
wchar_t *wcsdup(const wchar_t *s);

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);

/* ---- Numeric conversion ---------------------------------------------- */
long wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
long long wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);
long double wcstold(const wchar_t *s, wchar_t **end);

/* ---- Wide character I/O ----------------------------------------------- */
wint_t fgetwc(FILE *stream);
wint_t getwc(FILE *stream);
wint_t getwchar(void);
wint_t fputwc(wchar_t wc, FILE *stream);
wint_t putwc(wchar_t wc, FILE *stream);
wint_t putwchar(wchar_t wc);
wint_t ungetwc(wint_t wc, FILE *stream);
wchar_t *fgetws(wchar_t *s, int n, FILE *stream);
int fputws(const wchar_t *s, FILE *stream);
int fwide(FILE *stream, int mode);

int fwprintf(FILE *stream, const wchar_t *format, ...);
int wprintf(const wchar_t *format, ...);
int swprintf(wchar_t *s, size_t n, const wchar_t *format, ...);
int vfwprintf(FILE *stream, const wchar_t *format, va_list ap);
int vwprintf(const wchar_t *format, va_list ap);
int vswprintf(wchar_t *s, size_t n, const wchar_t *format, va_list ap);

/* ---- Time ------------------------------------------------------------- */
struct tm;
size_t wcsftime(wchar_t *s, size_t n, const wchar_t *format,
		const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* _WCHAR_H */
