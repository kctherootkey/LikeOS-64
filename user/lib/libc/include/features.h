/*
 * LikeOS-64 features.h - Feature-test macro plumbing
 *
 * Not a standard header.  It exists because a great deal of portable software
 * includes it -- usually guarded by HAVE_FEATURES_H, sometimes not -- and
 * because the C++ runtime's platform configuration reaches for it directly.
 * A libc that is not glibc still needs to answer the questions asked of it.
 *
 * What this header does NOT do is claim to be glibc.  __GLIBC__ is deliberately
 * left undefined: code that tests for it is asking whether glibc's extensions
 * and quirks are present, and the honest answer is no.  The kernel build and
 * the port toolchain undefine __linux__ for exactly the same reason -- taking
 * another system's code path because a macro happened to be set is how a port
 * acquires bugs that are invisible at the point they are introduced.
 */

#ifndef _FEATURES_H
#define _FEATURES_H

/*
 * "Is the C library at least version maj.min of glibc?"  It is not glibc at
 * all, so the answer is always no.
 *
 * Defined rather than left out because it is used in #if directives without a
 * defined() guard -- notably by the C++ runtime's own platform header -- and an
 * undefined function-like macro there is a preprocessor error, not a false.
 * Answering no puts callers on the conservative path, which is the correct one:
 * every such test guards the use of a glibc extension.
 */
#define __GLIBC_PREREQ(maj, min) 0

/*
 * The feature-test macros a program sets before its first include.
 *
 * This libc does not gate its declarations on them -- everything it has is
 * always visible, which is the simplest contract and the one the headers
 * already assume.  They are normalised here anyway, so that software inspecting
 * them sees a consistent picture: _GNU_SOURCE implies the others, as it does
 * everywhere else.
 */
#ifdef _GNU_SOURCE
# undef  _POSIX_SOURCE
# define _POSIX_SOURCE 1
# undef  _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200809L
# undef  _XOPEN_SOURCE
# define _XOPEN_SOURCE 700
# undef  _DEFAULT_SOURCE
# define _DEFAULT_SOURCE 1
# undef  _BSD_SOURCE
# define _BSD_SOURCE 1
#endif

#if defined _XOPEN_SOURCE && !defined _POSIX_C_SOURCE
# if _XOPEN_SOURCE >= 700
#  define _POSIX_C_SOURCE 200809L
# elif _XOPEN_SOURCE >= 600
#  define _POSIX_C_SOURCE 200112L
# else
#  define _POSIX_C_SOURCE 199506L
# endif
#endif

/*
 * With no feature-test macro set at all, behave as a system whose headers
 * expose the usual set.  A program that asked for strict conformance by
 * defining __STRICT_ANSI__ (which -std=c99 and friends do) is left alone.
 */
#if !defined _POSIX_SOURCE && !defined _POSIX_C_SOURCE && \
    !defined _XOPEN_SOURCE && !defined __STRICT_ANSI__
# define _POSIX_SOURCE   1
# define _POSIX_C_SOURCE 200809L
# define _DEFAULT_SOURCE 1
#endif

/* The POSIX revision these headers are written to. */
#define __POSIX_VISIBLE 200809

#endif /* _FEATURES_H */
