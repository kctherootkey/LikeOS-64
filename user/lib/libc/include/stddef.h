#ifndef _STDDEF_H
#define _STDDEF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long ssize_t;

/* wchar_t is standard in <stddef.h>.  Guarded so <wchar.h> and other headers
 * can define the identical type without a redefinition clash.  __WCHAR_TYPE__
 * is the compiler's own wide-char type (int on x86_64), so this always agrees
 * with the ABI.
 *
 * In C++ it is a built-in type and must not be typedef'd at all. */
#ifndef __cplusplus
#ifndef __wchar_t_defined
#define __wchar_t_defined
typedef __WCHAR_TYPE__ wchar_t;
#endif
#endif

/*
 * NULL.
 *
 * ((void*)0) is correct in C and WRONG in C++, where a void* does not convert
 * implicitly to another pointer type: every `f(ptr, NULL)` becomes "invalid
 * conversion from void* to T*".  __null is what the compiler provides for
 * exactly this case, and nullptr where the language has it.
 */
#ifdef __cplusplus
# if __cplusplus >= 201103L
#  define NULL nullptr
# else
#  define NULL __null
# endif
#else
# define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

/*
 * The most strictly aligned type the implementation supports (C11 7.19,
 * C++11 [support.types]).  It is what a general-purpose allocator has to return
 * storage aligned for, and the C++ standard library declares std::max_align_t
 * as an alias of this one -- so without it <cstddef> does not compile.
 *
 * Expressed as a struct of the two most-aligned fundamental types rather than
 * as an alignment attribute on a scalar, which is how every other libc spells
 * it and what keeps its alignment equal to __BIGGEST_ALIGNMENT__ (16 here)
 * without naming the number.
 */
#ifndef __max_align_t_defined
#define __max_align_t_defined
typedef struct {
	long long __max_align_ll
		__attribute__((__aligned__(__alignof__(long long))));
	long double __max_align_ld
		__attribute__((__aligned__(__alignof__(long double))));
} max_align_t;
#endif

#ifdef __cplusplus
}
#endif

#endif
