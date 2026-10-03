/* Host test prelude for the fence library (host/test-drm-fence.sh).
 *
 * Stands in for <kernel/ke/sched.h>, whose locks disable interrupts with
 * instructions a user process may not execute: the guard below keeps the
 * real header out, and what the fence code uses of it is defined here --
 * spinlocks that fail the test on recursion (a deadlock in the kernel)
 * and an interrupt flag the test can look at.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_TEST_DRM_FENCE_PRELUDE_H
#define HOST_TEST_DRM_FENCE_PRELUDE_H
#define _KERNEL_SCHED_H_ 1

#include <kernel/uapi/types.h>
#include <kernel/ke/fpu.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/cred.h>

void test_fatal(const char *what, const char *name);
extern uint64_t test_irq_flag; /* 0x200: interrupts on */
extern int test_locks_held;

typedef struct {
	volatile int held;
	const char *name;
} spinlock_t;
#define SPINLOCK_INIT(n) { 0, n }

static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->held = 0;
	l->name = name;
}
static inline void spin_lock(spinlock_t *l)
{
	if (l->held)
		test_fatal("recursive spin_lock (deadlock)", l->name);
	l->held = 1;
	test_locks_held++;
}
static inline void spin_unlock(spinlock_t *l)
{
	if (!l->held)
		test_fatal("spin_unlock of a free lock", l->name);
	l->held = 0;
	test_locks_held--;
}
static inline uint64_t local_irq_save(void)
{
	uint64_t f = test_irq_flag;
	test_irq_flag = 0;
	return f;
}
static inline void local_irq_restore(uint64_t f)
{
	test_irq_flag = f;
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

typedef enum { TASK_RUNNING = 0, TASK_READY, TASK_BLOCKED } task_state_t;
typedef struct task {
	uint64_t id;
	int tgid;
	char comm[16];
	void *wait_channel;
	volatile task_state_t state;
	uint64_t wakeup_tick;
} task_t;

task_t *sched_current(void);
int sched_claim_wake(task_t *t, task_state_t from);
void sched_enqueue_ready(task_t *t);
void sched_schedule(void);

typedef struct {
	int writer;
	const char *name;
} mm_rwsem_t;
#define _KERNEL_MM_RWSEM_H_ 1
static inline void mm_rwsem_init(mm_rwsem_t *s, const char *n)
{
	s->writer = 0;
	s->name = n;
}
static inline void mm_write_lock(mm_rwsem_t *s)
{
	if (s->writer)
		test_fatal("recursive resv lock", s->name);
	s->writer = 1;
}
static inline void mm_write_unlock(mm_rwsem_t *s)
{
	s->writer = 0;
}
static inline bool mm_rwsem_is_write_locked(const mm_rwsem_t *s)
{
	return s->writer != 0;
}

#endif
