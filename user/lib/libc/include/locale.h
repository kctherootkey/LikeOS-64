/*
 * locale.h - locale selection.
 *
 * One locale is implemented and its character encoding is UTF-8.  setlocale()
 * still remembers and reports the name it was given, because that is what
 * programs test before enabling their multibyte code paths.  See
 * src/locale/locale.c.
 */
#ifndef _LOCALE_H
#define _LOCALE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MESSAGES 3
#define LC_MONETARY 4
#define LC_NUMERIC  5
#define LC_TIME     6

/* Masks for newlocale().  One bit per category, in the same order. */
#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_ALL_MASK      (LC_COLLATE_MASK | LC_CTYPE_MASK | LC_MESSAGES_MASK | \
                          LC_MONETARY_MASK | LC_NUMERIC_MASK | LC_TIME_MASK)

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
    char  int_p_cs_precedes;
    char  int_p_sep_by_space;
    char  int_n_cs_precedes;
    char  int_n_sep_by_space;
    char  int_p_sign_posn;
    char  int_n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* Per-thread locale objects.  There is one locale here, so a locale_t is a
 * token; the interface exists because software uses it to make formatting
 * independent of the global locale -- which it already is. */
struct __locale_struct { int __unused_field; };
typedef struct __locale_struct *locale_t;

#define LC_GLOBAL_LOCALE ((locale_t)-1)

locale_t newlocale(int mask, const char *locale, locale_t base);
locale_t duplocale(locale_t loc);
void freelocale(locale_t loc);
locale_t uselocale(locale_t loc);

#ifdef __cplusplus
}
#endif

#endif /* _LOCALE_H */
