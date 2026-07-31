/* assert.h - assertion macro for LikeOS libc */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

/* The one function assert() expands to.
 *
 * Deliberately the ONLY name the macro mentions, and deliberately in the
 * reserved __ namespace.  The macro used to expand to fprintf(stderr, ...) and
 * abort() directly, which breaks in any scope where the program has its own
 * object by one of those names -- a local `bool abort` is perfectly legal C and
 * made every assert() in that function fail to compile. */
void __assert_fail(const char *__expr, const char *__file, unsigned int __line,
		   const char *__func) __attribute__((noreturn));

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
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
