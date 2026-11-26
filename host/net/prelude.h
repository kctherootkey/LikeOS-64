/* Host test prelude for the network harness (host/test-net.sh).
 *
 * Force-included (-include) into every kernel-side unit of the harness: the
 * real kernel/net sources, the per-instance kernel services (kstubs.c) and
 * the per-instance glue (glue.c).  Each stack instance is compiled from
 * these units, linked into one relocatable object and has its global
 * symbols renamed (n0_..., n1_...), so the two instances never share a
 * global variable.
 *
 * The kernel's own headers are used as they are, except for the inline
 * functions that execute instructions a user process may not (cli/popfq,
 * rdmsr, %gs-relative reads) or that would spin for ever in a
 * single-threaded program (spin_lock).  Those are renamed while the headers
 * are read and replaced below with host versions that simulate the
 * interrupt flag and turn every misuse into a fatal test failure:
 *
 *   - spin_lock on a held lock: the harness runs one thread, so contention
 *     can only be recursion, which is a deadlock in the kernel;
 *   - a spin_trylock loop that never succeeds (the same deadlock, spelled
 *     as a trylock spin);
 *   - spin_unlock of a free lock;
 *   - sleeping with a spinlock held or interrupts disabled.
 *
 * BUG()/BUG_ON() print the kernel message and a host stack trace and abort
 * instead of executing ud2.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_PRELUDE_H
#define HOST_NET_PRELUDE_H

/* ---- read the kernel headers with the privileged inlines renamed ---- */
#define spinlock_init __kh_spinlock_init
#define spin_lock __kh_spin_lock
#define spin_trylock __kh_spin_trylock
#define spin_unlock __kh_spin_unlock
#define spin_is_locked __kh_spin_is_locked
#define local_irq_save __kh_local_irq_save
#define local_irq_restore __kh_local_irq_restore
#define irqs_enabled __kh_irqs_enabled
#define irqs_disabled __kh_irqs_disabled
#define spin_lock_irqsave __kh_spin_lock_irqsave
#define spin_unlock_irqrestore __kh_spin_unlock_irqrestore
#define timer_rdtsc __kh_timer_rdtsc
#define read_gs_base_msr __kh_read_gs_base_msr
#define this_cpu_id __kh_this_cpu_id

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/percpu.h>

#undef spinlock_init
#undef spin_lock
#undef spin_trylock
#undef spin_unlock
#undef spin_is_locked
#undef local_irq_save
#undef local_irq_restore
#undef irqs_enabled
#undef irqs_disabled
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#undef timer_rdtsc
#undef read_gs_base_msr
#undef this_cpu_id

#include "api-host.h"

/* ---- per-instance harness state (kstubs.c) ---- */
void test_fatal(const char *what, const char *name) __attribute__((noreturn));
void test_fatalf(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));
extern uint64_t test_irq_flag; /* 0x200 while "interrupts" are on */
extern int test_spins_held; /* spinlocks held right now */
extern uint64_t test_trylock_fails; /* consecutive failed trylocks */

/* ---- spinlocks: single-threaded, so contention == deadlock ---- */
static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->locked = 0;
	l->owner_cpu = 0xFFFFFFFF;
	l->name = name;
}

static inline void spin_lock(spinlock_t *l)
{
	if (l->locked)
		test_fatal("spin_lock of a lock already held (deadlock)",
			   l->name);
	l->locked = 1;
	l->owner_cpu = 0;
	test_spins_held++;
}

static inline int spin_trylock(spinlock_t *l)
{
	if (l->locked) {
		/* A trylock on a held lock can only be the caller's own
		 * lock.  Opportunistic users skip; a spin loop around it
		 * would never end, so a long run of failures is fatal. */
		if (++test_trylock_fails > 100000)
			test_fatal("spin_trylock loop on a lock held by the "
				   "same context (deadlock)",
				   l->name);
		return 0;
	}
	test_trylock_fails = 0;
	l->locked = 1;
	l->owner_cpu = 0;
	test_spins_held++;
	return 1;
}

static inline void spin_unlock(spinlock_t *l)
{
	if (!l->locked)
		test_fatal("spin_unlock of a lock that is not held", l->name);
	l->owner_cpu = 0xFFFFFFFF;
	l->locked = 0;
	test_spins_held--;
}

static inline int spin_is_locked(spinlock_t *l)
{
	return l->locked != 0;
}

static inline uint64_t local_irq_save(void)
{
	uint64_t f = test_irq_flag;

	test_irq_flag = 0;
	return f;
}

static inline void local_irq_restore(uint64_t f)
{
	test_irq_flag = f & 0x200;
}

static inline int irqs_enabled(void)
{
	return test_irq_flag != 0;
}

static inline int irqs_disabled(void)
{
	return test_irq_flag == 0;
}

static inline void spin_lock_irqsave(spinlock_t *l, uint64_t *f)
{
	*f = local_irq_save();
	spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
	spin_unlock(l);
	local_irq_restore(f);
}

static inline uint64_t timer_rdtsc(void)
{
	return hs_now_ns();
}

/* No GS base: every caller takes its "before percpu_init" branch, which
 * is CPU 0 -- the CPU the whole receive path runs on anyway. */
static inline uint64_t read_gs_base_msr(void)
{
	return 0;
}

static inline uint32_t this_cpu_id(void)
{
	return 0;
}

/* ---- BUG() without ud2: message, host stack trace, abort ---- */
void test_bug(const char *cond, const char *file, int line, const char *func)
	__attribute__((noreturn));
#undef BUG
#undef BUG_ON
#define BUG() test_bug(NULL, __FILE__, __LINE__, __func__)
#define BUG_ON(cond)                                                      \
	do {                                                              \
		if (unlikely(cond))                                       \
			test_bug(#cond, __FILE__, __LINE__, __func__);    \
	} while (0)

void bug_report_atomic_sleep(const char *file, int line, const char *func);
/* ---- might_sleep() always armed: interrupts off or a spinlock held ---- */
#undef might_sleep
#define might_sleep()                                                     \
	do {                                                              \
		if (unlikely(irqs_disabled() || test_spins_held))         \
			bug_report_atomic_sleep(__FILE__, __LINE__,       \
						__func__);                \
	} while (0)

#endif
