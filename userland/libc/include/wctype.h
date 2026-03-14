/*
 * wctype.h - wide character classification for LikeOS
 * Minimal implementation treating wide chars as single-byte.
 */
#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long wctype_t;
typedef const int *wctrans_t;

static inline int iswalpha(wint_t wc) { return (wc >= 'A' && wc <= 'Z') || (wc >= 'a' && wc <= 'z'); }
static inline int iswdigit(wint_t wc) { return wc >= '0' && wc <= '9'; }
static inline int iswalnum(wint_t wc) { return iswalpha(wc) || iswdigit(wc); }
static inline int iswspace(wint_t wc) { return wc == ' ' || wc == '\t' || wc == '\n' || wc == '\r' || wc == '\f' || wc == '\v'; }
static inline int iswblank(wint_t wc) { return wc == ' ' || wc == '\t'; }
static inline int iswprint(wint_t wc) { return wc >= 0x20 && wc < 0x7F; }
static inline int iswpunct(wint_t wc) { return iswprint(wc) && !iswalnum(wc) && !iswspace(wc); }
static inline int iswupper(wint_t wc) { return wc >= 'A' && wc <= 'Z'; }
static inline int iswlower(wint_t wc) { return wc >= 'a' && wc <= 'z'; }
static inline int iswcntrl(wint_t wc) { return wc < 0x20 || wc == 0x7F; }
static inline int iswgraph(wint_t wc) { return wc > 0x20 && wc < 0x7F; }
static inline int iswxdigit(wint_t wc) { return iswdigit(wc) || (wc >= 'a' && wc <= 'f') || (wc >= 'A' && wc <= 'F'); }

static inline wctype_t wctype(const char *name)
{
    (void)name;
    return 0;
}

static inline int iswctype(wint_t wc, wctype_t type)
{
    (void)wc; (void)type;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _WCTYPE_H */
