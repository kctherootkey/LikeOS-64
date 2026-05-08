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

#ifndef __restrict
# define __restrict restrict
#endif

#ifndef __containerof
# define __containerof(ptr, type, member) \
    ((type*)((char*)(ptr) - __builtin_offsetof(type, member)))
#endif

#endif /* _SYS_CDEFS_H */
