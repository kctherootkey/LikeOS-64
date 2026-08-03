/*
 * wctype.h - wide character classification and case mapping.
 *
 * The answers come from the Unicode Character Database (see
 * host/gen-unicode-tables.py), so these classify every script, not just ASCII.
 */
#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque tokens produced by wctype()/wctrans().  Small integers rather than
 * pointers, so that an invalid one is rejected instead of dereferenced. */
typedef unsigned long wctype_t;
typedef long wctrans_t;

int iswalpha(wint_t wc);
int iswdigit(wint_t wc);
int iswalnum(wint_t wc);
int iswspace(wint_t wc);
int iswblank(wint_t wc);
int iswprint(wint_t wc);
int iswgraph(wint_t wc);
int iswpunct(wint_t wc);
int iswupper(wint_t wc);
int iswlower(wint_t wc);
int iswcntrl(wint_t wc);
int iswxdigit(wint_t wc);

wint_t towupper(wint_t wc);
wint_t towlower(wint_t wc);

wctype_t wctype(const char *name);
int iswctype(wint_t wc, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t wc, wctrans_t trans);

#ifdef __cplusplus
}
#endif

#endif /* _WCTYPE_H */
