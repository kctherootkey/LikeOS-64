// LikeOS -- vmwgfx: the small vocabulary the driver's object code is written in.
//
// The resource, binding, view and command-verifier code of this driver is
// written against a handful of idioms that the rest of the kernel spells
// differently or not at all: error codes carried inside a pointer, fixed-size
// bitmaps with find-next-bit iteration, a compile-time assertion and a few
// numeric limits.  They live here, in one place, so that every file of the
// driver uses the same definitions; gpu_util.h supplies the rest (sized
// integers, list_head, min/max, container_of).
//
// Everything is guarded, so a definition that another graphics header brings
// first wins and an identical one here stays out of its way.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_COMPAT_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_COMPAT_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/dev/gpu/gpu_util.h>

/* ---- numeric limits ---------------------------------------------------- */

#ifndef INT_MAX
#define INT_MAX 0x7fffffff
#endif
#ifndef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#endif
#ifndef U32_MAX
#define U32_MAX 0xffffffffu
#endif
#ifndef U16_MAX
#define U16_MAX 0xffffu
#endif
#ifndef S32_MAX
#define S32_MAX INT_MAX
#endif

/* ---- error codes inside pointers --------------------------------------
 *
 * A lookup that can fail for several reasons returns either the object or
 * the negated errno disguised as a pointer into the last page of the
 * address space, which no object ever occupies.  NULL stays a valid
 * "nothing here" where a caller needs to tell the two apart. */

#ifndef MAX_ERRNO
#define MAX_ERRNO 4095
#endif
#ifndef IS_ERR_VALUE
#define IS_ERR_VALUE(x) \
	((unsigned long)(x) >= (unsigned long)-MAX_ERRNO)
#endif
#ifndef ERR_PTR
#define ERR_PTR(err) ((void *)(long)(err))
#endif
#ifndef PTR_ERR
#define PTR_ERR(ptr) ((long)(ptr))
#endif
#ifndef IS_ERR
#define IS_ERR(ptr) IS_ERR_VALUE(ptr)
#endif
#ifndef IS_ERR_OR_NULL
#define IS_ERR_OR_NULL(ptr) (!(ptr) || IS_ERR_VALUE(ptr))
#endif
#ifndef ERR_CAST
#define ERR_CAST(ptr) ((void *)(ptr))
#endif
#ifndef PTR_ERR_OR_ZERO
#define PTR_ERR_OR_ZERO(ptr) (IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0)
#endif

/* ---- compile-time checks ---------------------------------------------- */

#ifndef BUILD_BUG_ON
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), "BUILD_BUG_ON(" #cond ")")
#endif

/* ---- page arithmetic --------------------------------------------------- */

#ifndef VMW_PAGE_SHIFT
#define VMW_PAGE_SHIFT 12
#endif
#ifndef PFN_UP
#define PFN_UP(x) (((uint64_t)(x) + (1ULL << VMW_PAGE_SHIFT) - 1) >> VMW_PAGE_SHIFT)
#endif
#ifndef PFN_DOWN
#define PFN_DOWN(x) ((uint64_t)(x) >> VMW_PAGE_SHIFT)
#endif

/* ---- fixed-size bitmaps -------------------------------------------------
 *
 * Non-atomic: every bitmap in this driver is protected by the lock of the
 * structure that embeds it. */

#ifndef BITS_PER_LONG
#define BITS_PER_LONG 64
#endif
#ifndef BITS_TO_LONGS
#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, BITS_PER_LONG)
#endif
#ifndef DECLARE_BITMAP
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#endif

static inline void vmw_bitmap_set_bit(unsigned long nr, unsigned long *addr)
{
	addr[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG);
}

static inline void vmw_bitmap_clear_bit(unsigned long nr, unsigned long *addr)
{
	addr[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}

static inline bool vmw_bitmap_test_bit(unsigned long nr,
				       const unsigned long *addr)
{
	return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1UL;
}

/* The first set bit at or after `offset', or `size' if there is none. */
static inline unsigned long vmw_bitmap_find_next_bit(const unsigned long *addr,
						     unsigned long size,
						     unsigned long offset)
{
	while (offset < size) {
		unsigned long word = addr[offset / BITS_PER_LONG] >>
				     (offset % BITS_PER_LONG);

		if (word) {
			offset += (unsigned long)__builtin_ctzl(word);
			return offset < size ? offset : size;
		}
		offset = (offset / BITS_PER_LONG + 1) * BITS_PER_LONG;
	}
	return size;
}

#ifndef __set_bit
#define __set_bit(nr, addr) vmw_bitmap_set_bit((nr), (addr))
#endif
#ifndef __clear_bit
#define __clear_bit(nr, addr) vmw_bitmap_clear_bit((nr), (addr))
#endif
#ifndef test_bit
#define test_bit(nr, addr) vmw_bitmap_test_bit((nr), (addr))
#endif
#ifndef find_next_bit
#define find_next_bit(addr, size, offset) \
	vmw_bitmap_find_next_bit((addr), (size), (offset))
#endif
#ifndef find_first_bit
#define find_first_bit(addr, size) vmw_bitmap_find_next_bit((addr), (size), 0)
#endif
#ifndef bitmap_zero
#define bitmap_zero(dst, nbits) \
	mm_memset((dst), 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long))
#endif
#ifndef for_each_set_bit
#define for_each_set_bit(bit, addr, size)                       \
	for ((bit) = find_first_bit((addr), (size)); (bit) < (size); \
	     (bit) = find_next_bit((addr), (size), (bit) + 1))
#endif

/* ---- logging ------------------------------------------------------------
 *
 * Client mistakes (a malformed command stream, a bad handle) are answered
 * with an error code, not a console line: a client looping on its mistake
 * must not be able to flood the console.  The macros keep their arguments
 * type-checked and print nothing. */

#ifndef VMW_DEBUG_USER
#define VMW_DEBUG_USER(fmt, ...)                       \
	do {                                           \
		if (0)                                 \
			kprintf(fmt, ##__VA_ARGS__);   \
	} while (0)
#endif
#ifndef VMW_DEBUG_KMS
#define VMW_DEBUG_KMS(fmt, ...) VMW_DEBUG_USER(fmt, ##__VA_ARGS__)
#endif

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_COMPAT_H */
