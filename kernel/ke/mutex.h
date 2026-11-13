// LikeOS - the general sleeping mutex
//
// A lock that is held across things that sleep -- a controller reset, a
// namespace scan, a firmware download -- and so cannot be a spinlock.
// It has the semantics of the block layer's blk_mutex (blkdev.h), which it
// generalises for every subsystem, with a wait queue of its own instead of
// a scheduler-wide channel and wake-one hand-off on unlock:
//
//   * not recursive: locking a mutex the caller holds is a BUG;
//   * owned: only the holder unlocks (WARN otherwise); the owner is the
//     scheduler task id, or MUTEX_BOOT_OWNER before the scheduler runs;
//   * mutex_lock() may sleep.  Where nothing may sleep (no scheduler task
//     yet, or interrupts disabled) it takes a free mutex at once and spins
//     for a held one -- only another CPU can hold it then;
//   * mutex_lock_interruptible()/_killable() give up with -EINTR on a
//     pending signal (any / SIGKILL);
//   * mutex_trylock() and mutex_unlock() never sleep; neither may be used
//     from a hard interrupt handler (a mutex is a process-context lock);
//   * holding one counts as a sleeping lock for the tracer
//     (task_sleep_lock_taken/dropped);
//   * lockdep_assert_held(&m) works on it (bug.h tests ->locked), and
//     mutex_held(&m) says whether the CALLER holds it.
//
// Lock ordering is the user's business; the NVMe stack's order is in
// kernel/dev/nvme/nvme.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_MUTEX_H
#define KERNEL_KE_MUTEX_H

#include <kernel/ke/waitq.h>
#include <kernel/uapi/bug.h>

#define MUTEX_NO_OWNER ((uint64_t)-1)
#define MUTEX_BOOT_OWNER ((uint64_t)-2)

struct mutex {
	volatile int locked; /* 0 free, 1 held (lockdep_assert_held reads it) */
	volatile uint64_t owner; /* task id of the holder, MUTEX_NO_OWNER */
	const char *name;
	/* Sleepers; its spinlock also protects locked/owner. */
	struct wait_queue_head wait;
	uint32_t contended; /* times a locker had to wait (diagnostics) */
};

#define MUTEX_INITIALIZER(n)                                              \
	{                                                                 \
		.locked = 0, .owner = MUTEX_NO_OWNER, .name = (n),        \
		.wait = WAIT_QUEUE_HEAD_INIT(n), .contended = 0           \
	}
#define DEFINE_MUTEX(var) struct mutex var = MUTEX_INITIALIZER(#var)

/* Any context. */
void mutex_init(struct mutex *m, const char *name);
/* May sleep. */
void mutex_lock(struct mutex *m);
/* May sleep; 0 taken, -EINTR when a signal (any / SIGKILL) is pending. */
int mutex_lock_interruptible(struct mutex *m);
int mutex_lock_killable(struct mutex *m);
/* 1 taken, 0 busy.  Never sleeps; not from a hard interrupt. */
int mutex_trylock(struct mutex *m);
/* Owner only.  Never sleeps; not from a hard interrupt. */
void mutex_unlock(struct mutex *m);
/* Held by anybody (racy snapshot, for diagnostics and asserts). */
static inline int mutex_is_locked(const struct mutex *m)
{
	return __atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) != 0;
}
/* Held by the calling task. */
int mutex_held(const struct mutex *m);
/* Orphan recovery for a task killed while holding `m': 1 when released. */
int mutex_release_if_owner(struct mutex *m, uint64_t task_id);
/* Destroy: WARNs when still held or waited on.  Any context. */
void mutex_destroy(struct mutex *m);

/* lockdep-style assertion that the CALLER holds `m' (DEBUG builds warn). */
#ifdef DEBUG
#define mutex_assert_held(m)                                              \
	WARN(!mutex_held(m), "mutex %s not held by the caller", (m)->name)
#else
#define mutex_assert_held(m) \
	do {                 \
		(void)(m);   \
	} while (0)
#endif

#endif /* KERNEL_KE_MUTEX_H */
