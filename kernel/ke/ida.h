// LikeOS - ID allocator (ida): small integer IDs handed out and returned
//
// Instance numbers (nvme0, nvme1 ...; subsystem and namespace-head
// instances), dynamic minor numbers (the extended block major, character
// device minors) and similar small-integer names come from one of these:
// the lowest free ID in a requested range, returned to the pool when the
// object goes.  The pool is a bitmap that grows on demand.
//
// Rules:
//   * ida_alloc*() may allocate (process context, may sleep only as far as
//     kalloc may); never from an interrupt handler.  ida_free() and
//     ida_is_allocated() take only the ida's spinlock: any context.
//   * The same ID is never handed out twice while allocated; freeing an ID
//     that is not allocated WARNs.
//   * IDs are 0 .. IDA_MAX.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_IDA_H
#define KERNEL_KE_IDA_H

#include <kernel/ke/sched.h> /* spinlock_t */

#define IDA_MAX 0x7fffffff

struct ida {
	spinlock_t lock;
	uint64_t *bits; /* nwords 64-bit words, kalloc'd, NULL while empty */
	uint32_t nwords;
	uint32_t nr_allocated;
	const char *name;
};

#define IDA_INIT(n)                                                       \
	{                                                                 \
		.lock = SPINLOCK_INIT(n), .bits = NULL, .nwords = 0,       \
		.nr_allocated = 0, .name = (n)                            \
	}
#define DEFINE_IDA(var) struct ida var = IDA_INIT(#var)

void ida_init(struct ida *ida, const char *name);
/* Lowest free ID in [min, max] (inclusive): the ID, -ENOSPC when the range
 * is full, -ENOMEM, -EINVAL (min > max or max > IDA_MAX). */
int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max);
static inline int ida_alloc(struct ida *ida)
{
	return ida_alloc_range(ida, 0, IDA_MAX);
}
static inline int ida_alloc_min(struct ida *ida, unsigned int min)
{
	return ida_alloc_range(ida, min, IDA_MAX);
}
static inline int ida_alloc_max(struct ida *ida, unsigned int max)
{
	return ida_alloc_range(ida, 0, max);
}
/* Claim one specific ID: 0, -EBUSY when taken, -ENOMEM. */
int ida_alloc_exact(struct ida *ida, unsigned int id);
void ida_free(struct ida *ida, unsigned int id);
int ida_is_allocated(struct ida *ida, unsigned int id);
/* Free everything (the bitmap memory too).  No ID may be in use by
 * anybody afterwards. */
void ida_destroy(struct ida *ida);

#endif /* KERNEL_KE_IDA_H */
