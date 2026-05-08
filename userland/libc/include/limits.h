/*
 * limits.h - implementation limits for LikeOS
 */
#ifndef _LIMITS_H
#define _LIMITS_H

/* Number of bits in a char */
#define CHAR_BIT    8

/* Minimum and maximum values a signed char can hold */
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127

/* Maximum value an unsigned char can hold */
#define UCHAR_MAX   255

/* Minimum and maximum values a char can hold (signed) */
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

/* Minimum and maximum values a signed short int can hold */
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767

/* Maximum value an unsigned short int can hold */
#define USHRT_MAX   65535

/* Minimum and maximum values a signed int can hold */
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647

/* Maximum value an unsigned int can hold */
#define UINT_MAX    4294967295U

/* Minimum and maximum values a signed long int can hold */
#define LONG_MIN    (-9223372036854775807L - 1L)
#define LONG_MAX    9223372036854775807L

/* Maximum value an unsigned long int can hold */
#define ULONG_MAX   18446744073709551615UL

/* Minimum and maximum values a signed long long int can hold */
#define LLONG_MIN   (-9223372036854775807LL - 1LL)
#define LLONG_MAX   9223372036854775807LL

/* Maximum value an unsigned long long int can hold */
#define ULLONG_MAX  18446744073709551615ULL

/* Bit-width macros (C23 / gnulib compatibility) */
#define CHAR_WIDTH      8
#define SCHAR_WIDTH     8
#define UCHAR_WIDTH     8
#define SHRT_WIDTH      16
#define USHRT_WIDTH     16
#define INT_WIDTH       32
#define UINT_WIDTH      32
#define LONG_WIDTH      64
#define ULONG_WIDTH     64
#define LLONG_WIDTH     64
#define ULLONG_WIDTH    64

/* Maximum bytes in a multibyte character (C locale) */
#define MB_LEN_MAX  4

/* POSIX limits */
#define PATH_MAX    4096
#define NAME_MAX    255
#define LINE_MAX    2048
#define PIPE_BUF    4096
#define OPEN_MAX    256
#define ARG_MAX     131072
#define NGROUPS_MAX 32
#define IOV_MAX     1024
#define _POSIX_PATH_MAX  256
#define _POSIX_NAME_MAX  14

/* Size limits */
#define SSIZE_MAX   LONG_MAX
#define SIZE_MAX    ULONG_MAX

#endif /* _LIMITS_H */
