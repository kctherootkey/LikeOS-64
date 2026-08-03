/*
 * langinfo.h - locale information items.
 *
 * nl_langinfo(CODESET) reports "UTF-8": that is the encoding the console, the
 * terminal emulator and the tools on this system all use, and a great deal of
 * software will not enable its multibyte handling until it sees that answer.
 */
#ifndef _LANGINFO_H
#define _LANGINFO_H

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;

#define CODESET    0
#define D_T_FMT    1
#define D_FMT      2
#define T_FMT      3
#define AM_STR     4
#define PM_STR     5
#define DAY_1      6
#define DAY_2      7
#define DAY_3      8
#define DAY_4      9
#define DAY_5      10
#define DAY_6      11
#define DAY_7      12
#define MON_1      13
#define MON_2      14
#define MON_3      15
#define MON_4      16
#define MON_5      17
#define MON_6      18
#define MON_7      19
#define MON_8      20
#define MON_9      21
#define MON_10     22
#define MON_11     23
#define MON_12     24
#define RADIXCHAR  25
#define THOUSEP    26
#define YESEXPR    27
#define NOEXPR     28
#define ERA        29
#define T_FMT_AMPM 30
#define ABDAY_1    31
#define ABDAY_2    32
#define ABDAY_3    33
#define ABDAY_4    34
#define ABDAY_5    35
#define ABDAY_6    36
#define ABDAY_7    37
#define ABMON_1    38
#define ABMON_2    39
#define ABMON_3    40
#define ABMON_4    41
#define ABMON_5    42
#define ABMON_6    43
#define ABMON_7    44
#define ABMON_8    45
#define ABMON_9    46
#define ABMON_10   47
#define ABMON_11   48
#define ABMON_12   49
#define CRNCYSTR   50
#define ERA_D_FMT  51
#define ERA_D_T_FMT 52
#define ERA_T_FMT  53
#define ALT_DIGITS 54

/* Aliases the standard spells this way. */
#define RADIXCHAR_ RADIXCHAR
#define DECIMAL_POINT RADIXCHAR
#define THOUSANDS_SEP THOUSEP

char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif

#endif /* _LANGINFO_H */
