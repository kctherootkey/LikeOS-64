/* assert.h - assertion macro for LikeOS libc */

/*
 * DELIBERATELY NOT GUARDED against repeated inclusion.
 *
 * C requires <assert.h> to define `assert` afresh on every inclusion, according
 * to whether NDEBUG is defined AT THAT MOMENT (C17 7.2p1).  A program may
 * legally write
 *
 *     #include <assert.h>
 *     ... assertions checked here ...
 *     #define NDEBUG
 *     #include <assert.h>
 *     ... and not here ...
 *
 * so this is the one standard header for which an `#ifndef _ASSERT_H` wrapper
 * is wrong.  It used to have one, and the way that surfaced was not a silently
 * disabled assertion: gnulib ships its own <assert.h> that wraps the system one
 * with #include_next, relying on exactly this behaviour.  With the macro
 * defined only on the first include, that wrapper produced a header defining
 * nothing, `assert(x)` became a call to an ordinary function named assert, and
 * every gettext program failed to link with "undefined reference to `assert'".
 *
 * Only the declaration below is guarded, since repeating it is pointless rather
 * than harmful.
 */

#ifndef _ASSERT_H_DECLARED
#define _ASSERT_H_DECLARED

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

#ifdef __cplusplus
}
#endif

#endif /* _ASSERT_H_DECLARED */

/* Redefined on every inclusion, which is the whole point of this header. */
#undef assert

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

/* C11 spells the compile-time assertion _Static_assert and provides this
 * lowercase alias here.  Plenty of code uses the alias unqualified.
 *
 * Not redefined on every inclusion, unlike assert: it does not depend on
 * NDEBUG, and a program that has defined its own must keep it. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
