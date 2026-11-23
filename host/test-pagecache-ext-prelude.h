/* Host test prelude for the page-cache extensions (host/test-pagecache-ext.sh):
 * pagecache_install / pagecache_lookup_held, the coalesced-read mapping check
 * for blocks smaller than a page, and delayed allocation at writeback.
 *
 * Force-included into kernel/fs/pagecache.c, the stubs and the test.  Keeps
 * <kernel/ke/sched.h> out (its locks disable interrupts with instructions a
 * user process may not execute) and defines what the cache uses of it:
 * spinlocks that fail the test on recursion (a deadlock in the kernel) or on
 * an unlock of a free lock, a simulated interrupt flag, and a might_sleep()
 * that fails the test when called with a spinlock held or interrupts off.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_TEST_PAGECACHE_EXT_PRELUDE_H
#define HOST_TEST_PAGECACHE_EXT_PRELUDE_H
#define _KERNEL_SCHED_H_ 1

/* bug.h's interrupt-flag probe reads the real flag; the test simulates it. */
#define irqs_disabled __kh_irqs_disabled
#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#undef irqs_disabled

void test_fatal(const char *what, const char *name);
extern uint64_t test_irq_flag; /* 0x200: interrupts on */
extern int test_locks_held;

static inline int irqs_disabled(void)
{
	return !(test_irq_flag & 0x200);
}

#undef might_sleep
#define might_sleep()                                                    \
	do {                                                             \
		if (test_locks_held)                                     \
			test_fatal("might_sleep with a spinlock held",   \
				   __func__);                            \
		if (irqs_disabled())                                     \
			test_fatal("might_sleep with interrupts off",    \
				   __func__);                            \
	} while (0)

typedef struct {
	volatile int locked;
	const char *name;
} spinlock_t;
#define SPINLOCK_INIT(n) { 0, n }

static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->locked = 0;
	l->name = name;
}
static inline void spin_lock(spinlock_t *l)
{
	if (l->locked)
		test_fatal("recursive spin_lock (deadlock)", l->name);
	l->locked = 1;
	test_locks_held++;
}
static inline void spin_unlock(spinlock_t *l)
{
	if (!l->locked)
		test_fatal("spin_unlock of a free lock", l->name);
	l->locked = 0;
	test_locks_held--;
}
static inline void spin_lock_irqsave(spinlock_t *l, uint64_t *f)
{
	*f = test_irq_flag;
	test_irq_flag = 0;
	spin_lock(l);
}
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
	spin_unlock(l);
	test_irq_flag = f;
}

void sched_yield_in_kernel(void);

#endif
