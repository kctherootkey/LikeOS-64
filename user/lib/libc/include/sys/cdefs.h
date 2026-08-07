/*
 * sys/cdefs.h - BSD-style compiler attribute macros.
 *
 * Provides the small set of __dead, __unused, __packed, __used,
 * __printflike, etc. macros that BSD-derived sources expect.
 */
#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

#ifdef __cplusplus
# define __BEGIN_DECLS extern "C" {
# define __END_DECLS   }
#else
# define __BEGIN_DECLS
# define __END_DECLS
#endif

#ifndef __GNUC_PREREQ__
# define __GNUC_PREREQ__(maj, min) \
    ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#endif

#ifndef __unused
# define __unused      __attribute__((__unused__))
#endif
#ifndef __used
# define __used        __attribute__((__used__))
#endif
#ifndef __dead
# define __dead        __attribute__((__noreturn__))
#endif
#ifndef __dead2
# define __dead2       __attribute__((__noreturn__))
#endif
#ifndef __packed
# define __packed      __attribute__((__packed__))
#endif
#ifndef __aligned
# define __aligned(x)  __attribute__((__aligned__(x)))
#endif
#ifndef __weak
# define __weak        __attribute__((__weak__))
#endif
#ifndef __pure
# define __pure        __attribute__((__pure__))
#endif
#ifndef __printflike
# define __printflike(fmtarg, firstvararg) \
    __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#endif
#ifndef __scanflike
# define __scanflike(fmtarg, firstvararg) \
    __attribute__((__format__(__scanf__, fmtarg, firstvararg)))
#endif
#ifndef __nonnull
# define __nonnull(x)  __attribute__((__nonnull__ x))
#endif

#ifndef __DECONST
# define __DECONST(type, var) ((type)(uintptr_t)(const void*)(var))
#endif

#ifndef __predict_true
# define __predict_true(x)  __builtin_expect(!!(x), 1)
#endif
#ifndef __predict_false
# define __predict_false(x) __builtin_expect(!!(x), 0)
#endif

/* __restrict
 *
 * GCC and clang provide this as a KEYWORD, in C++ as well as in C, so there is
 * nothing to define for them -- and defining it anyway is actively wrong in
 * C++, where `restrict` is not a keyword at all.  `#define __restrict restrict`
 * turned every use of it into an undeclared identifier, which is how including
 * this header before <regex.h> made regcomp's prototype a syntax error.
 *
 * The fallbacks are for a compiler that provides neither: nothing in C++ and in
 * C89, where the qualifier does not exist and dropping it is always safe, and
 * the real keyword from C99 on.
 */
#if !defined __GNUC__ && !defined __clang__ && !defined __restrict
# if defined __cplusplus || __STDC_VERSION__ < 199901L
#  define __restrict
# else
#  define __restrict restrict
# endif
#endif

#ifndef __containerof
# define __containerof(ptr, type, member) \
    ((type*)((char*)(ptr) - __builtin_offsetof(type, member)))
#endif

/* glibc-compatible __THROW / __nothrow markers (used by host syslog.h etc.) */
#ifndef __THROW
# define __THROW
#endif
#ifndef __THROWNL
# define __THROWNL
#endif
#ifndef __LEAF
# define __LEAF
#endif
#ifndef __LEAF_ATTR
# define __LEAF_ATTR
#endif
#ifndef __nonnull
# define __nonnull(...)
#endif
#ifndef __wur
# define __wur
#endif
#ifndef __REDIRECT
# define __REDIRECT(name, proto, alias) name proto
#endif
#ifndef __REDIRECT_NTH
# define __REDIRECT_NTH(name, proto, alias) name proto __THROW
#endif

/* va_list alias used by some host headers (e.g. syslog.h) */
#ifndef __gnuc_va_list
typedef __builtin_va_list __gnuc_va_list;
#endif

#endif /* _SYS_CDEFS_H */
