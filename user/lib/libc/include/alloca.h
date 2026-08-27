/*
 * alloca.h - stack allocation.
 *
 * alloca() cannot be a function: the storage must live in the CALLER's
 * frame and vanish on the caller's return.  Every compiler this system
 * builds with provides it as a builtin, and the macro is the whole
 * implementation -- exactly how every other libc does it.
 */
#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#undef alloca
#define alloca(size) __builtin_alloca(size)

#endif /* _ALLOCA_H */
