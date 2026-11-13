// LikeOS - completions: "this one thing has happened", waited for once
//
// A completion is a counter of events plus a wait queue.  One side does the
// work and calls complete() when it is done; the other side waits for it
// with wait_for_completion*().  Typical use: a synchronous command -- the
// submitter initialises a completion on its stack, hands the command to
// the hardware, and waits; the interrupt handler (or the block softirq)
// that sees the command finish calls complete().
//
// Semantics (the conventional ones):
//   * complete() adds one event and wakes one waiter; each successful wait
//     consumes one event.  Events are counted, so a complete() that comes
//     before the wait is not lost.
//   * complete_all() makes the completion permanently done (every present
//     and future waiter returns at once) until reinit_completion().
//   * complete()/complete_all() may be called from ANY context: a hard
//     interrupt handler, an hrtimer callback, the block softirq, with
//     spinlocks held.  They never sleep.
//   * The waits are process context only (might_sleep).  Where nothing may
//     sleep (boot before the scheduler, interrupts disabled) they poll the
//     counter instead -- the event must then come from another CPU or from
//     a device the caller drives some other way.
//   * Lifetime: complete() touches the completion only while it holds the
//     completion's own lock and never after releasing it, so a waiter may
//     free (or leave the stack frame of) the completion as soon as its wait
//     returns.  After complete_all() the owner must make sure every waiter
//     has returned before freeing it.
//
// Timeouts are milliseconds; the timed waits return the remaining time in
// milliseconds (at least 1) on success and 0 on timeout, the interruptible
// and killable ones -ERESTARTSYS when a signal ended the wait.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_COMPLETION_H
#define KERNEL_KE_COMPLETION_H

#include <kernel/ke/waitq.h>

/* `done' after complete_all(). */
#define COMPLETION_DONE_ALL 0xffffffffu

struct completion {
	volatile uint32_t done; /* events not yet consumed */
	struct wait_queue_head wait; /* its lock also protects `done' */
};

#define COMPLETION_INITIALIZER(name)                                     \
	{                                                                \
		.done = 0, .wait = WAIT_QUEUE_HEAD_INIT(#name)           \
	}
#define DECLARE_COMPLETION(name) \
	struct completion name = COMPLETION_INITIALIZER(name)
/* For a completion on the stack. */
#define DECLARE_COMPLETION_ONSTACK(name) \
	struct completion name = COMPLETION_INITIALIZER(name)

/* Initialise (any context).  Must not be used on a completion somebody
 * waits on. */
void init_completion(struct completion *x);
/* Forget the events (done = 0) for reuse; nobody may be waiting.  Any
 * context. */
void reinit_completion(struct completion *x);

/* Signal one event, wake one waiter.  Any context. */
void complete(struct completion *x);
/* Signal for good, wake every waiter.  Any context. */
void complete_all(struct completion *x);

/* Wait for (and consume) one event.  Uninterruptible.  Process context. */
void wait_for_completion(struct completion *x);
/* The same with a timeout: remaining ms (>= 1), or 0 when it timed out
 * (no event consumed then). */
long wait_for_completion_timeout(struct completion *x, long timeout_ms);
/* I/O waits: identical (kept for callers mirroring the conventional names;
 * time spent counts as I/O wait where that is accounted). */
void wait_for_completion_io(struct completion *x);
long wait_for_completion_io_timeout(struct completion *x, long timeout_ms);
/* Interruptible by any signal: 0, or -ERESTARTSYS (no event consumed). */
int wait_for_completion_interruptible(struct completion *x);
long wait_for_completion_interruptible_timeout(struct completion *x,
					       long timeout_ms);
/* Interruptible by SIGKILL only: 0, or -ERESTARTSYS. */
int wait_for_completion_killable(struct completion *x);
long wait_for_completion_killable_timeout(struct completion *x,
					  long timeout_ms);

/* Consume an event if one is there, never wait: 1 consumed, 0 none.  Any
 * context. */
int try_wait_for_completion(struct completion *x);
/* Is an event there (or complete_all done)?  Does not consume.  Any
 * context. */
int completion_done(struct completion *x);

#endif /* KERNEL_KE_COMPLETION_H */
