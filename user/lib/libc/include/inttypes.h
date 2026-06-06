/*
 * inttypes.h - C99 fixed-width formatting macros + integer routines.
 *
 * Light-weight version that only pulls in what portable applications
 * (notably tmux/libevent) need.  intmax_t / uintmax_t alias int64_t and
 * uint64_t since we are LP64 only.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

typedef struct { intmax_t quot, rem; } imaxdiv_t;

#define __PRI64_PREFIX  "ll"
#define __PRIPTR_PREFIX "l"

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 __PRI64_PREFIX "d"
#define PRIdMAX __PRI64_PREFIX "d"
#define PRIdPTR __PRIPTR_PREFIX "d"

#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 __PRI64_PREFIX "i"
#define PRIiMAX __PRI64_PREFIX "i"
#define PRIiPTR __PRIPTR_PREFIX "i"

#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 __PRI64_PREFIX "o"
#define PRIoMAX __PRI64_PREFIX "o"
#define PRIoPTR __PRIPTR_PREFIX "o"

#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 __PRI64_PREFIX "u"
#define PRIuMAX __PRI64_PREFIX "u"
#define PRIuPTR __PRIPTR_PREFIX "u"

#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 __PRI64_PREFIX "x"
#define PRIxMAX __PRI64_PREFIX "x"
#define PRIxPTR __PRIPTR_PREFIX "x"

#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 __PRI64_PREFIX "X"
#define PRIXMAX __PRI64_PREFIX "X"
#define PRIXPTR __PRIPTR_PREFIX "X"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 __PRI64_PREFIX "d"
#define SCNdMAX __PRI64_PREFIX "d"
#define SCNdPTR __PRIPTR_PREFIX "d"

#define SCNi8  "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 __PRI64_PREFIX "i"
#define SCNiMAX __PRI64_PREFIX "i"
#define SCNiPTR __PRIPTR_PREFIX "i"

#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 __PRI64_PREFIX "u"
#define SCNuMAX __PRI64_PREFIX "u"
#define SCNuPTR __PRIPTR_PREFIX "u"

#define SCNx8  "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 __PRI64_PREFIX "x"
#define SCNxMAX __PRI64_PREFIX "x"
#define SCNxPTR __PRIPTR_PREFIX "x"

#ifdef __cplusplus
extern "C" {
#endif

intmax_t    strtoimax(const char* nptr, char** endptr, int base);
uintmax_t   strtoumax(const char* nptr, char** endptr, int base);
intmax_t    imaxabs(intmax_t j);
imaxdiv_t   imaxdiv(intmax_t numer, intmax_t denom);

#ifdef __cplusplus
}
#endif

#endif /* _INTTYPES_H */
