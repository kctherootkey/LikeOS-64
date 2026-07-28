#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long ssize_t;

/* wchar_t is standard in <stddef.h>.  Guarded so <wchar.h> and other headers
 * can define the identical type without a redefinition clash.  __WCHAR_TYPE__
 * is the compiler's own wide-char type (int on x86_64), so this always agrees
 * with the ABI. */
#ifndef __wchar_t_defined
#define __wchar_t_defined
typedef __WCHAR_TYPE__ wchar_t;
#endif

#define NULL ((void*)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
