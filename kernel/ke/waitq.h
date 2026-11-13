/*
 * waitq.h - wait queues, one per thing that can be waited FOR.
 *
 * The conventional arrangement, and the reason for it:
 *
 * A task that blocks does so on some particular object -- this socket, that
 * pipe, this terminal.  The queue of waiters therefore belongs to the OBJECT,
 * not to the scheduler, and waking it walks only the tasks that asked about
 * that object.  Cost is proportional to the number of waiters on the thing
 * that became ready, and to nothing else.
 *
 * What this replaces in the poll layer was the opposite: every poll(), select()
 * and epoll_wait() in the system parked on one global channel, and every I/O
 * event anywhere woke ALL of them by scanning the entire task list with
 * interrupts disabled.  A machine with twenty pollers turned one byte on one
 * socket into twenty wakeups, twenty descriptor re-scans and a task-list walk,
 * so the cost of doing anything grew with the number of processes running --
 * measured at ten wakeups per event, sixteen thousand a second, on an
 * otherwise idle desktop.  An idle xterm cost as much as a busy one, because
 * what it added was a task to walk, not work to do.
 *
 * Structure: a doubly linked list threaded through entries the WAITER owns,
 * so unlinking is O(1) and the queue head allocates nothing.  `pprev' points
 * at whatever pointer refers to this entry -- the head's `first' for the first
 * entry, the previous entry's `next' otherwise -- which is what lets a removal
 * work without walking from the front.
 *
 * Locking: each head has its own spinlock, taken with interrupts disabled
 * because producers wake from interrupt context (an arriving packet, a
 * keystroke).  The lock covers the list only; task states are changed through
 * the scheduler's own claim protocol, which is what makes a concurrent wake
 * and timeout safe.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _KERNEL_WAITQ_H_
#define _KERNEL_WAITQ_H_

/* spinlock_t, the irqsave helpers, task_t, sched_claim_wake() and
 * sched_enqueue_ready() all come from here. */
#include <kernel/ke/sched.h>

struct wait_queue_entry {
	struct wait_queue_entry *next;
	struct wait_queue_entry **pprev; /* NULL when not queued */
	task_t *task;
	uint64_t seq; /* the head's wake_seq when it was queued */
};

struct wait_queue_head {
	struct wait_queue_entry *first;
	spinlock_t lock;
	/* Counts the wake-all walks started on this head (wq_wake_all): an
	 * entry queued after a walk began carries its number and is left to
	 * the next walk. */
	uint64_t wake_seq;
};

/* A head must be initialised before use.  Objects that are zeroed on
 * allocation get a usable head from that alone -- first NULL is the empty
 * list -- but the spinlock wants its name, so say so explicitly wherever the
 * object is created. */
static inline void wq_head_init(struct wait_queue_head *h, const char *name)
{
	h->first = NULL;
	h->wake_seq = 0;
	spinlock_init(&h->lock, name);
}

/* Initialise a head that lives in a slot which gets RECYCLED.
 *
 * A queue must never be re-initialised while entries are on it: clearing
 * `first' orphans them, and the next wq_remove() then writes through a pprev
 * that refers to nothing, corrupting whichever list occupies the slot after
 * it.  A poller registers only for the length of one poll() call, but nothing
 * stops another thread closing the descriptor underneath it -- so a slot CAN
 * be reused with a waiter still attached.
 *
 * The head therefore outlives the object in the slot: the lock is named once
 * and the list is never cleared.  A stale waiter gets a spurious wake, which
 * it answers by re-scanning and finding nothing -- the cheap outcome.
 *
 * Callers that zero the whole containing struct must save and restore the head
 * around that; this cannot do it for them.
 */
static inline void wq_head_init_once(struct wait_queue_head *h,
				     const char *name)
{
	if (!h->lock.name)
		spinlock_init(&h->lock, name);
}

/* Zero `size' bytes at `obj' -- a slot that gets recycled -- except the
 * `nkeep' wait-queue heads in `keep', which are not written at all.
 *
 * Copying a head out and back in around a plain clear is not the same
 * thing.  A stale waiter can unlink itself from the head in between (the
 * case wq_head_init_once() is about), and it does so under the head's own
 * lock, which a copy does not take: the copy-back then undoes the unlink,
 * and the head points at an entry that is no longer on it -- and before long
 * at memory that is no longer an entry at all, which the next wake walks
 * into. */
static inline void wq_zero_around(void *obj, uint64_t size,
				  struct wait_queue_head *const *keep,
				  int nkeep)
{
	uint8_t *p = (uint8_t *)obj;

	for (uint64_t i = 0; i < size; i++) {
		int inside = 0;

		for (int k = 0; k < nkeep && !inside; k++) {
			uint8_t *h = (uint8_t *)keep[k];

			inside = p + i >= h && p + i < h + sizeof(*keep[k]);
		}
		if (!inside)
			p[i] = 0;
	}
}

static inline void wq_entry_init(struct wait_queue_entry *e, task_t *t)
{
	e->next = NULL;
	e->pprev = NULL;
	e->task = t;
	e->seq = 0;
}

/* True when the entry is currently on some queue. */
static inline int wq_entry_queued(const struct wait_queue_entry *e)
{
	return e->pprev != NULL;
}

/* The list operations with h->lock already held by the caller. */
/* Appended at the tail: waiters are woken in the order they came (a
 * wake-one -- complete(), a mutex unlock -- takes the longest waiter,
 * never the newest).  Finding the tail walks the list; the lists are
 * short (the waiters of one object), and the walk keeps the entries at
 * two pointers with an O(1) unlink. */
static inline void wq_add_locked(struct wait_queue_head *h,
				 struct wait_queue_entry *e)
{
	struct wait_queue_entry **link = &h->first;

	if (wq_entry_queued(e))
		return;
	while (*link)
		link = &(*link)->next;
	e->next = NULL;
	e->seq = h->wake_seq;
	*link = e;
	e->pprev = link;
}

static inline void wq_remove_locked(struct wait_queue_head *h,
				    struct wait_queue_entry *e)
{
	(void)h; /* the entry knows where it is linked */
	if (e->pprev) {
		*e->pprev = e->next;
		if (e->next)
			e->next->pprev = e->pprev;
		e->next = NULL;
		e->pprev = NULL;
	}
}

/* Put `e' on `h'.  Adding an entry that is already queued is a caller error;
 * it would corrupt whichever list it is on, so it is refused rather than
 * silently done twice. */
static inline void wq_add(struct wait_queue_head *h,
			  struct wait_queue_entry *e)
{
	uint64_t flags;

	if (wq_entry_queued(e))
		return;
	spin_lock_irqsave(&h->lock, &flags);
	e->seq = h->wake_seq;
	e->next = h->first;
	if (e->next)
		e->next->pprev = &e->next;
	h->first = e;
	e->pprev = &h->first;
	spin_unlock_irqrestore(&h->lock, flags);
}

static inline void wq_remove(struct wait_queue_head *h,
			     struct wait_queue_entry *e)
{
	uint64_t flags;

	spin_lock_irqsave(&h->lock, &flags);
	if (e->pprev) {
		*e->pprev = e->next;
		if (e->next)
			e->next->pprev = e->pprev;
		e->next = NULL;
		e->pprev = NULL;
	}
	spin_unlock_irqrestore(&h->lock, flags);
}

/* Wake everything on `h'.  Returns how many tasks were made runnable.
 *
 * The tasks are collected under the lock and enqueued after it is dropped:
 * sched_enqueue_ready() takes run-queue locks, and taking those beneath a
 * wait-queue lock would fix an ordering between the two that every other
 * caller would then have to honour.  The batch is bounded and the walk
 * restarts, in the same shape as the scheduler's own channel wake -- an entry
 * whose task is claimed is unlinked in the SAME pass, so a waiter can never be
 * left claimed but not enqueued.
 *
 * Only the entries that were queued when the call began are woken.  A task
 * woken by an earlier batch can run on another CPU, find its condition still
 * false and queue itself again (at the head) before the next batch is
 * collected; walking those again would never end while waiters keep doing
 * that, and the caller may have interrupts off for all of it.  A waiter that
 * queues itself after the call began looks at its condition after queueing,
 * so it sees whatever the waker changed before calling this.
 *
 * A waiter that loses the claim (it timed out, or another waker got there
 * first) is skipped and left on the queue for its owner to remove.
 */
static inline int wq_wake_all(struct wait_queue_head *h)
{
	uint64_t start = 0;
	int total = 0;

	for (;;) {
		task_t *batch[16];
		int n = 0;
		uint64_t flags;
		struct wait_queue_entry *e, *next;

		spin_lock_irqsave(&h->lock, &flags);
		if (!start)
			start = ++h->wake_seq;
		for (e = h->first; e && n < 16; e = next) {
			next = e->next;
			if (!e->task || e->seq >= start)
				continue;
			if (!sched_claim_wake(e->task, TASK_BLOCKED))
				continue;
			/* Claimed, so it must also leave the queue and be
			 * enqueued below -- see the note above. */
			if (e->pprev) {
				*e->pprev = e->next;
				if (e->next)
					e->next->pprev = e->pprev;
				e->next = NULL;
				e->pprev = NULL;
			}
			e->task->wait_channel = NULL;
			batch[n++] = e->task;
		}
		spin_unlock_irqrestore(&h->lock, flags);

		for (int i = 0; i < n; i++)
			sched_enqueue_ready(batch[i]);
		total += n;

		if (n < 16)
			break;
	}
	return total;
}

/* ==========================================================================
 * Waiting for a condition: the wait_event family (kernel/ke/waitq.c)
 * ==========================================================================
 *
 * wait_event*(wq, condition, ...) puts the calling task to sleep on `wq'
 * until `condition' (any C expression, evaluated again after every wake-up,
 * so it must have no side effects beyond what repeating it allows) is true.
 * Whoever makes the condition true calls wake_up(&wq) AFTER changing it.
 *
 * The protocol that makes this race-free is the scheduler's: the waiter
 * queues itself on `wq' and publishes TASK_BLOCKED (a full barrier) and only
 * THEN evaluates the condition; a waker changes the condition and only then
 * walks the queue.  Either the waiter sees the new condition, or the waker
 * sees a blocked task to wake.  When the condition turns out true after the
 * task published TASK_BLOCKED, the waiter takes its own sleep back
 * (BLOCKED -> RUNNING by compare-and-exchange); if a waker claimed it in
 * that window it passes through sched_schedule() once, which returns at
 * once because the task is already queued to run.
 *
 * Timeouts are milliseconds.  The deadline is exact: an hrtimer wakes the
 * task at the deadline (hrtimer_wait_arm) and the scheduler tick backs it
 * up.  Return values (as the conventional interface defines them):
 *   wait_event(wq, c)                             void
 *   wait_event_timeout(wq, c, ms)                 0: timed out with c still
 *                                                 false; else the remaining
 *                                                 milliseconds, at least 1
 *   wait_event_interruptible_timeout(wq, c, ms)   >0 remaining ms, 0 timed
 *                                                 out, -ERESTARTSYS
 *   wait_event_killable(wq, c)                    0, or -ERESTARTSYS on a
 *                                                 fatal signal (SIGKILL)
 *
 * Context: process context.  might_sleep() is asserted.  Where the caller
 * cannot sleep -- no scheduler task yet (boot), or interrupts disabled --
 * the same macros poll the condition with `pause' instead (the event must
 * then come from another CPU or a device the caller polls inside the
 * condition); signals are not looked at while polling.
 *
 * NEVER call these with a spinlock held, from a hard interrupt handler, an
 * hrtimer callback or the block softirq.
 *
 * ERESTARTSYS is turned into EINTR or a restart by the system call exit
 * path; a driver passes it up unchanged.
 */

#define WQ_WAIT_UNINTERRUPTIBLE 0
#define WQ_WAIT_INTERRUPTIBLE 1 /* any unblocked signal ends the wait */
#define WQ_WAIT_KILLABLE 2 /* only SIGKILL ends the wait */

/* One waiter's bookkeeping, on its stack for the length of the wait.  The
 * fields belong to waitq.c. */
struct wq_waiter {
	struct wait_queue_entry entry;
	struct wait_queue_head *head;
	task_t *task; /* NULL: polling (cannot sleep) */
	uint64_t deadline_ns; /* absolute monotonic, 0 = none */
	int mode; /* WQ_WAIT_* */
	int blocked; /* TASK_BLOCKED published, not yet slept/undone */
};

/* Set up `w' for a wait on `h' with an absolute deadline (0 none).  Decides
 * once whether this wait sleeps or polls. */
void wq_waiter_init(struct wq_waiter *w, struct wait_queue_head *h,
		    uint64_t deadline_ns, int mode);
/* Queue the waiter (if not queued yet) and publish TASK_BLOCKED.  Called
 * before every evaluation of the condition.  Nothing to do when polling. */
void wq_waiter_prepare(struct wq_waiter *w);
/* The same with the head's own lock held by the caller. */
void wq_waiter_prepare_locked(struct wq_waiter *w);
/* The condition is true: take the sleep back, leave the queue. */
void wq_waiter_finish(struct wq_waiter *w);
/* The condition is false: sleep (or pause once when polling) until woken
 * or the deadline.  Returns 0 to evaluate the condition again, 1 when the
 * deadline has passed, -ERESTARTSYS when the mode's signals are pending.
 * On a non-zero return the waiter has left the queue already. */
int wq_waiter_sleep(struct wq_waiter *w);
/* Absolute deadline `ms' milliseconds from now (monotonic ns); ms < 0 is
 * treated as 0 (already expired), so callers may pass a signed count. */
uint64_t wq_deadline_after_ms(long ms);
/* Has `t' got SIGKILL pending?  (The killable waits.) */
int task_fatal_signal_pending(task_t *t);

/* Wake every waiter of `h' (wake_up and wake_up_all are the same here:
 * there are no exclusive waiters on a wait_event queue). */
#define wake_up(h) ((void)wq_wake_all(h))
#define wake_up_all(h) ((void)wq_wake_all(h))

/* Static initialiser of a head: struct wait_queue_head h =
 * WAIT_QUEUE_HEAD_INIT("name"); */
#define WAIT_QUEUE_HEAD_INIT(n)                              \
	{                                                    \
		.first = NULL, .lock = SPINLOCK_INIT(n)      \
	}
#define DECLARE_WAIT_QUEUE_HEAD(name) \
	struct wait_queue_head name = WAIT_QUEUE_HEAD_INIT(#name)

/* The core: returns 1 when the condition became true, 0 when the deadline
 * passed with it false (evaluated once more at the deadline), or
 * -ERESTARTSYS. */
#define __wait_event_core(wq_head, condition, deadline_ns, mode)            \
	({                                                                  \
		struct wq_waiter __wqw;                                     \
		long __wqret;                                               \
		wq_waiter_init(&__wqw, &(wq_head), (deadline_ns), (mode));  \
		for (;;) {                                                  \
			wq_waiter_prepare(&__wqw);                          \
			if (condition) {                                    \
				wq_waiter_finish(&__wqw);                   \
				__wqret = 1;                                \
				break;                                      \
			}                                                   \
			__wqret = wq_waiter_sleep(&__wqw);                  \
			if (__wqret == 1) {                                 \
				__wqret = (condition) ? 1 : 0;              \
				break;                                      \
			}                                                   \
			if (__wqret < 0) {                                  \
				/* A waiter woken for its condition must  \
				 * not fail for a signal that came along: \
				 * it consumes what it was woken for. */   \
				if (condition)                              \
					__wqret = 1;                        \
				break;                                      \
			}                                                   \
		}                                                           \
		__wqret;                                                    \
	})

/* The timed forms report the remaining milliseconds (at least 1) when the
 * condition became true; the deadline is fixed once, before the loop. */
#define __wait_event_timed(wq_head, condition, ms, mode)                     \
	({                                                                   \
		uint64_t __wqdl = wq_deadline_after_ms((long)(ms));          \
		long __wqr = __wait_event_core(wq_head, condition, __wqdl,   \
					       mode);                        \
		if (__wqr == 1) {                                            \
			uint64_t __wqnow = hrtimer_now_ns();                 \
			__wqr = __wqnow >= __wqdl ?                          \
					1 :                                  \
					(long)((__wqdl - __wqnow + 999999ULL) / \
					       1000000ULL);                  \
		}                                                            \
		__wqr;                                                       \
	})

#define wait_event(wq_head, condition)                                   \
	do {                                                             \
		if (!(condition))                                        \
			(void)__wait_event_core(wq_head, condition, 0,   \
						WQ_WAIT_UNINTERRUPTIBLE);\
	} while (0)

#define wait_event_timeout(wq_head, condition, timeout_ms)                 \
	({                                                                 \
		long __wet = (condition) ? 1 : 0;                          \
		if (!__wet)                                                \
			__wet = __wait_event_timed(wq_head, condition,     \
						   timeout_ms,             \
						   WQ_WAIT_UNINTERRUPTIBLE); \
		__wet;                                                     \
	})

#define wait_event_interruptible_timeout(wq_head, condition, timeout_ms)   \
	({                                                                 \
		long __weit = (condition) ? 1 : 0;                         \
		if (!__weit)                                               \
			__weit = __wait_event_timed(wq_head, condition,    \
						    timeout_ms,            \
						    WQ_WAIT_INTERRUPTIBLE); \
		__weit;                                                    \
	})

#define wait_event_killable(wq_head, condition)                            \
	({                                                                 \
		long __wek = 0;                                            \
		if (!(condition))                                          \
			__wek = __wait_event_core(wq_head, condition, 0,   \
						  WQ_WAIT_KILLABLE);       \
		__wek < 0 ? __wek : 0;                                     \
	})

#endif /* _KERNEL_WAITQ_H_ */
