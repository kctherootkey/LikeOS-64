/*
 * langinfo.h - language information for LikeOS
 * Stub - returns C/POSIX locale info.
 */
#ifndef _LANGINFO_H
#define _LANGINFO_H

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

static inline char *nl_langinfo(nl_item item)
{
    switch (item) {
        case CODESET:   return "ANSI_X3.4-1968";  /* ASCII */
        case D_T_FMT:   return "%a %b %e %H:%M:%S %Y";
        case D_FMT:     return "%m/%d/%y";
        case T_FMT:     return "%H:%M:%S";
        case AM_STR:    return "AM";
        case PM_STR:    return "PM";
        case RADIXCHAR: return ".";
        case THOUSEP:   return "";
        case YESEXPR:   return "^[yY]";
        case NOEXPR:    return "^[nN]";
        default:        return "";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* _LANGINFO_H */
