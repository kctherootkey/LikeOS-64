/*
 * wcstr.c - the wide-character string functions.
 *
 * These mirror their <string.h> counterparts exactly, operating on wchar_t
 * units rather than bytes.  They were previously static inlines in <wchar.h>;
 * as real symbols a caller can take their address, a shared library resolves
 * one copy instead of one per translation unit, and a program that only
 * declares them (as configure-generated code likes to) still links.
 */
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "unicode.h"

size_t wcslen(const wchar_t *s)
{
	const wchar_t *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t wcsnlen(const wchar_t *s, size_t n)
{
	size_t i = 0;
	while (i < n && s[i])
		i++;
	return i;
}

wchar_t *wcscpy(wchar_t *d, const wchar_t *s)
{
	wchar_t *p = d;
	while ((*p++ = *s++) != L'\0')
		;
	return d;
}

wchar_t *wcpcpy(wchar_t *d, const wchar_t *s)
{
	while ((*d = *s++) != L'\0')
		d++;
	return d;
}

wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0; /* padded, per the standard; not necessarily terminated */
	return d;
}

wchar_t *wcscat(wchar_t *d, const wchar_t *s)
{
	wchar_t *p = d;
	while (*p)
		p++;
	while ((*p++ = *s++) != L'\0')
		;
	return d;
}

wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
	wchar_t *p = d;
	while (*p)
		p++;
	while (n-- && *s)
		*p++ = *s++;
	*p = 0; /* always terminated, unlike wcsncpy */
	return d;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (*a < *b) ? -1 : (*a > *b) ? 1 : 0;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (*a < *b) ? -1 : 1;
		if (*a == L'\0')
			break;
	}
	return 0;
}

int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
	wint_t ca, cb;
	for (;;) {
		ca = towlower((wint_t)*a);
		cb = towlower((wint_t)*b);
		if (ca != cb || ca == 0)
			break;
		a++;
		b++;
	}
	return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	wint_t ca = 0, cb = 0;
	for (; n; n--, a++, b++) {
		ca = towlower((wint_t)*a);
		cb = towlower((wint_t)*b);
		if (ca != cb || ca == 0)
			break;
	}
	if (!n)
		return 0;
	return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

/* Collation is code point order: this system has one locale, and pretending
 * otherwise would make strcoll and wcscoll disagree with each other. */
int wcscoll(const wchar_t *a, const wchar_t *b)
{
	return wcscmp(a, b);
}

size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)
{
	size_t len = wcslen(s);
	if (d && len < n)
		wcscpy(d, s);
	else if (d && n)
		d[0] = 0;
	return len;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
	for (;; s++) {
		if (*s == c)
			return (wchar_t *)s;
		if (*s == L'\0')
			return 0;
	}
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
	const wchar_t *last = 0;
	for (;; s++) {
		if (*s == c)
			last = s;
		if (*s == L'\0')
			return (wchar_t *)last;
	}
}

wchar_t *wcsstr(const wchar_t *h, const wchar_t *nd)
{
	if (!*nd)
		return (wchar_t *)h;
	for (; *h; h++) {
		const wchar_t *a = h, *b = nd;
		while (*a && *b && *a == *b) {
			a++;
			b++;
		}
		if (!*b)
			return (wchar_t *)h;
	}
	return 0;
}

wchar_t *wcswcs(const wchar_t *h, const wchar_t *nd)
{
	return wcsstr(h, nd);
}

size_t wcsspn(const wchar_t *s, const wchar_t *set)
{
	size_t n = 0;
	for (; s[n]; n++) {
		const wchar_t *p = set;
		while (*p && *p != s[n])
			p++;
		if (!*p)
			break;
	}
	return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *set)
{
	size_t n = 0;
	for (; s[n]; n++) {
		const wchar_t *p = set;
		while (*p && *p != s[n])
			p++;
		if (*p)
			break;
	}
	return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set)
{
	s += wcscspn(s, set);
	return *s ? (wchar_t *)s : 0;
}

/* Reentrant: the caller owns the cursor, as with strtok_r. */
wchar_t *wcstok(wchar_t *s, const wchar_t *sep, wchar_t **save)
{
	wchar_t *tok;
	if (!save)
		return 0;
	if (!s)
		s = *save;
	if (!s)
		return 0;
	s += wcsspn(s, sep);
	if (!*s) {
		*save = 0;
		return 0;
	}
	tok = s;
	s += wcscspn(s, sep);
	if (*s) {
		*s = 0;
		*save = s + 1;
	} else {
		*save = 0;
	}
	return tok;
}

wchar_t *wcsdup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = malloc(n * sizeof(wchar_t));
	if (!p)
		return 0;
	for (size_t i = 0; i < n; i++)
		p[i] = s[i];
	return p;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
	for (; n; n--, s++)
		if (*s == c)
			return (wchar_t *)s;
	return 0;
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
	return d;
}

wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n)
{
	if (d < s) {
		for (size_t i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		while (n--)
			d[n] = s[n]; /* overlapping: copy backwards */
	}
	return d;
}

wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)
{
	for (size_t i = 0; i < n; i++)
		d[i] = c;
	return d;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

/* ---- Numeric conversion ------------------------------------------------
 *
 * Implemented by transcribing the numeric prefix to bytes and handing it to
 * the narrow conversions.  Every character a number can contain is ASCII, so
 * the transcription is exact; doing it this way means one parser rather than
 * two that can disagree about, say, hex floats.                            */

#define WCSTO_BUF 128

static size_t wcs_numeric_prefix(const wchar_t *s, char *buf, size_t bufsz)
{
	size_t i = 0;
	while (s[i] && i + 1 < bufsz && (unsigned)s[i] < 0x80) {
		buf[i] = (char)s[i];
		i++;
	}
	buf[i] = '\0';
	return i;
}

long wcstol(const wchar_t *s, wchar_t **end, int base)
{
	char buf[WCSTO_BUF];
	char *e;
	size_t n = wcs_numeric_prefix(s, buf, sizeof(buf));
	long v = strtol(buf, &e, base);
	if (end)
		*end = (wchar_t *)s + ((size_t)(e - buf) <= n ? (size_t)(e - buf) : n);
	return v;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base)
{
	char buf[WCSTO_BUF];
	char *e;
	size_t n = wcs_numeric_prefix(s, buf, sizeof(buf));
	unsigned long v = strtoul(buf, &e, base);
	if (end)
		*end = (wchar_t *)s + ((size_t)(e - buf) <= n ? (size_t)(e - buf) : n);
	return v;
}

long long wcstoll(const wchar_t *s, wchar_t **end, int base)
{
	char buf[WCSTO_BUF];
	char *e;
	size_t n = wcs_numeric_prefix(s, buf, sizeof(buf));
	long long v = strtoll(buf, &e, base);
	if (end)
		*end = (wchar_t *)s + ((size_t)(e - buf) <= n ? (size_t)(e - buf) : n);
	return v;
}

unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base)
{
	char buf[WCSTO_BUF];
	char *e;
	size_t n = wcs_numeric_prefix(s, buf, sizeof(buf));
	unsigned long long v = strtoull(buf, &e, base);
	if (end)
		*end = (wchar_t *)s + ((size_t)(e - buf) <= n ? (size_t)(e - buf) : n);
	return v;
}

double wcstod(const wchar_t *s, wchar_t **end)
{
	char buf[WCSTO_BUF];
	char *e;
	size_t n = wcs_numeric_prefix(s, buf, sizeof(buf));
	double v = strtod(buf, &e);
	if (end)
		*end = (wchar_t *)s + ((size_t)(e - buf) <= n ? (size_t)(e - buf) : n);
	return v;
}

float wcstof(const wchar_t *s, wchar_t **end)
{
	return (float)wcstod(s, end);
}

long double wcstold(const wchar_t *s, wchar_t **end)
{
	return (long double)wcstod(s, end);
}
