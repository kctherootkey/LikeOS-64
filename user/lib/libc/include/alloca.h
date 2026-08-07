/*
 * alloca.h - allocate on the stack
 *
 * Storage that is released when the calling function returns, rather than by a
 * matching free().  Not standard C -- no standard has ever specified it -- but
 * every Unix provides this header, and software includes it behind a configure
 * test for HAVE_ALLOCA_H.  Cairo is where its absence first showed: the test
 * failed, so Cairo skipped its own guarded include, and then called alloca()
 * with nothing in scope to declare it.
 *
 * The macro is the whole implementation.  __builtin_alloca is not a library
 * call at all -- the compiler adjusts the stack pointer in the caller's own
 * frame, which is the only way this can work: a real function would release
 * the storage as it returned.  That is also why there is no version of this in
 * libc.so, here or anywhere else.
 *
 * The declaration beside it exists for form, as it does on other systems: it
 * gives the name a type for code that inspects it, and it is what makes
 * `#undef alloca` followed by a call fail at link time rather than silently
 * producing something that does not work.
 */

#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *alloca(size_t __size);

#ifdef __cplusplus
}
#endif

/* Undefined first so that including this header twice, or after something else
 * has defined the name, leaves exactly one definition. */
#undef alloca
#define alloca(size) __builtin_alloca(size)

#endif /* _ALLOCA_H */
