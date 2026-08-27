#ifndef _STDINT_H
#define _STDINT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;
/* intmax_t is LONG, not long long: int64_t above is long, PRIdMAX in
 * <inttypes.h> prints with "l", and every LP64 system makes the same
 * choice.  It was long long once, which made printf("%li", (intmax_t)x)
 * a format-mismatch error in code that is correct everywhere else. */
typedef long intmax_t;
typedef unsigned long uintmax_t;

/* Fast types - use native register-width types for speed */
typedef int8_t    int_fast8_t;
typedef int32_t   int_fast16_t;
typedef int32_t   int_fast32_t;
typedef int64_t   int_fast64_t;
typedef uint8_t   uint_fast8_t;
typedef uint32_t  uint_fast16_t;
typedef uint32_t  uint_fast32_t;
typedef uint64_t  uint_fast64_t;

/* Least types */
typedef int8_t    int_least8_t;
typedef int16_t   int_least16_t;
typedef int32_t   int_least32_t;
typedef int64_t   int_least64_t;
typedef uint8_t   uint_least8_t;
typedef uint16_t  uint_least16_t;
typedef uint32_t  uint_least32_t;
typedef uint64_t  uint_least64_t;

#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define UINT8_MAX  (255)
#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535)
#define INT32_MIN  (-2147483648)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)
#define INT64_MIN  (-9223372036854775808L)
#define INT64_MAX  (9223372036854775807L)
#define UINT64_MAX (18446744073709551615UL)

/* Limits for the fast and least types.  C99 requires these alongside the
 * typedefs, and their absence is not cosmetic: gcc's libstdc++ probes for
 * exactly these macros to decide whether the platform has C99 <stdint.h>,
 * and without them it compiles <random> away to nothing -- no engines, no
 * distributions, no std::mt19937 anywhere in the C++ library (discovered
 * first via WebKit's NavigatorUAData, then hard-blocked LLVM).
 *
 * Each value mirrors the typedef above it.  int_fast16_t and int_fast32_t
 * are int32_t here (other libcs make them 64-bit), so these are NOT those
 * libcs' numbers -- macros that disagree with the types they describe would
 * pass the probe and be silently wrong. */
#define INT_FAST8_MIN    INT8_MIN
#define INT_FAST8_MAX    INT8_MAX
#define INT_FAST16_MIN   INT32_MIN
#define INT_FAST16_MAX   INT32_MAX
#define INT_FAST32_MIN   INT32_MIN
#define INT_FAST32_MAX   INT32_MAX
#define INT_FAST64_MIN   INT64_MIN
#define INT_FAST64_MAX   INT64_MAX
#define UINT_FAST8_MAX   UINT8_MAX
#define UINT_FAST16_MAX  UINT32_MAX
#define UINT_FAST32_MAX  UINT32_MAX
#define UINT_FAST64_MAX  UINT64_MAX

#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST16_MAX  INT16_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST32_MAX  INT32_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX

/* C99 integer-constant macros: append the suffix that makes a literal have the
 * named type.  They exist because there is no portable way to write, say, a
 * 64-bit constant — "1" is an int, and the suffix differs per model.  Code
 * that builds bit masks wider than int needs them, and without them the
 * expression silently truncates. */
#define INT8_C(c)   (c)
#define INT16_C(c)  (c)
#define INT32_C(c)  (c)
#define INT64_C(c)  (c ## L)

#define UINT8_C(c)  (c)
#define UINT16_C(c) (c)
#define UINT32_C(c) (c ## U)
#define UINT64_C(c) (c ## UL)

#define INTMAX_C(c)  INT64_C(c)
#define UINTMAX_C(c) UINT64_C(c)

#ifndef SIZE_MAX
#define SIZE_MAX    UINT64_MAX
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX   INT64_MAX
#endif
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

#ifdef __cplusplus
}
#endif

#endif
