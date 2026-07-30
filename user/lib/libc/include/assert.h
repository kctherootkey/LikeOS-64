/* assert.h - assertion macro for LikeOS libc */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  include <stdio.h>
#  include <stdlib.h>
#  define assert(expr) \
    ((expr) ? (void)0 \
            : (fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", \
                       #expr, __FILE__, __LINE__), abort()))
#endif

#ifdef __cplusplus
}
#endif

/* C11 spells the compile-time assertion _Static_assert and provides this
 * lowercase alias here.  Plenty of code uses the alias unqualified. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#endif /* _ASSERT_H */
