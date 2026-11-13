// LikeOS - completions (completion.h)
//
// The counter lives under the wait queue head's own spinlock, which the
// waker holds for everything it does to the completion: it counts the
// event and unlinks the waiter(s) it claims, then lets go of the lock and
// only afterwards puts the claimed tasks on their run queues -- touching
// the tasks, never the completion again.  So a waiter that sees the event
// may return and free the completion at once.  The waiter side looks at
// the counter under the same lock and publishes its sleep (waitq.h's
// waiter, queued with the lock held) only when there is no event: an
// event either is seen or finds that task to wake, and complete() never
// claims a task that is about to take some other event instead -- which
// would leave a second waiter asleep with an event pending.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/completion.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>

/* Waiters claimed per pass of complete_all() (on-stack batch). */
#define COMPLETION_WAKE_BATCH 32

void init_completion(struct completion *x)
{
	BUG_ON(x == NULL);
	x->done = 0;
	wq_head_init(&x->wait, "completion");
}

void reinit_completion(struct completion *x)
{
	BUG_ON(x == NULL);
	WARN_ON_ONCE(x->wait.first != NULL);
	__atomic_store_n(&x->done, 0, __ATOMIC_RELEASE);
}

/* Claim and unlink up to `max' waiters of `x' (its lock held).  Entries
 * whose task cannot be claimed (still running towards its sleep, or woken
 * by a timeout or another waker) are left: such a task looks at the
 * counter again before it sleeps. */
static int completion_claim_locked(struct completion *x, task_t **batch,
				   int max)
{
	struct wait_queue_entry *e, *next;
	int n = 0;

	lockdep_assert_held(&x->wait.lock);
	for (e = x->wait.first; e && n < max; e = next) {
		next = e->next;
		if (!e->task || !sched_claim_wake(e->task, TASK_BLOCKED))
			continue;
		if (e->pprev) {
			*e->pprev = e->next;
			if (e->next)
				e->next->pprev = e->pprev;
			e->next = NULL;
			e->pprev = NULL;
		}
		e->task->wait_channel = NULL;
		e->task->wakeup_tick = 0;
		batch[n++] = e->task;
	}
	return n;
}

void complete(struct completion *x)
{
	task_t *t[1];
	uint64_t fl;
	int n;

	BUG_ON(x == NULL);
	spin_lock_irqsave(&x->wait.lock, &fl);
	if (x->done != COMPLETION_DONE_ALL) {
		/* More events than waiters ever consume would be a caller bug;
		 * saturate rather than wrap into "done for good". */
		if (!WARN_ON_ONCE(x->done == COMPLETION_DONE_ALL - 1))
			x->done++;
	}
	n = completion_claim_locked(x, t, 1);
	spin_unlock_irqrestore(&x->wait.lock, fl);
	if (n)
		sched_enqueue_ready(t[0]);
}

void complete_all(struct completion *x)
{
	BUG_ON(x == NULL);
	for (int first = 1;; first = 0) {
		task_t *batch[COMPLETION_WAKE_BATCH];
		uint64_t fl;
		int n;

		spin_lock_irqsave(&x->wait.lock, &fl);
		if (first)
			x->done = COMPLETION_DONE_ALL;
		n = completion_claim_locked(x, batch, COMPLETION_WAKE_BATCH);
		spin_unlock_irqrestore(&x->wait.lock, fl);
		for (int i = 0; i < n; i++)
			sched_enqueue_ready(batch[i]);
		if (n < COMPLETION_WAKE_BATCH)
			break;
	}
}

int try_wait_for_completion(struct completion *x)
{
	uint64_t fl;
	int got = 0;

	BUG_ON(x == NULL);
	/* Unlocked peek first: the common "not yet" costs no lock. */
	if (!__atomic_load_n(&x->done, __ATOMIC_ACQUIRE))
		return 0;
	spin_lock_irqsave(&x->wait.lock, &fl);
	if (x->done) {
		if (x->done != COMPLETION_DONE_ALL)
			x->done--;
		got = 1;
	}
	spin_unlock_irqrestore(&x->wait.lock, fl);
	return got;
}

int completion_done(struct completion *x)
{
	uint64_t fl;
	int done;

	BUG_ON(x == NULL);
	if (!__atomic_load_n(&x->done, __ATOMIC_ACQUIRE))
		return 0;
	/* Taking the lock makes sure complete() has finished with `x' (the
	 * caller may free it after a true answer). */
	spin_lock_irqsave(&x->wait.lock, &fl);
	done = x->done != 0;
	spin_unlock_irqrestore(&x->wait.lock, fl);
	return done;
}

/* The wait itself.  The counter is looked at under the completion's lock,
 * and the sleep is published under that same lock only when the counter
 * is zero, so complete() (which counts and claims under it) only ever
 * claims a task that really waits for the event it adds.  A woken waiter
 * consumes an event when there is one, even when a signal or the deadline
 * came along meanwhile; without one it reports the signal or the timeout.
 * Returns 1 (consumed), 0 (deadline passed) or -ERESTARTSYS. */
static long completion_wait(struct completion *x, uint64_t deadline_ns,
			    int mode)
{
	struct wq_waiter w;
	uint64_t fl;
	long ret = 0, slept = 0;

	BUG_ON(x == NULL);
	wq_waiter_init(&w, &x->wait, deadline_ns, mode);
	spin_lock_irqsave(&x->wait.lock, &fl);
	for (;;) {
		if (x->done) {
			if (x->done != COMPLETION_DONE_ALL)
				x->done--;
			ret = 1;
			break;
		}
		if (slept < 0) {
			ret = slept;
			break;
		}
		if (slept == 1) {
			ret = 0;
			break;
		}
		wq_waiter_prepare_locked(&w);
		spin_unlock_irqrestore(&x->wait.lock, fl);
		slept = wq_waiter_sleep(&w);
		spin_lock_irqsave(&x->wait.lock, &fl);
	}
	/* Still queued after a wake-up that was no claim (the backstop
	 * tick): leave the queue before the lock goes. */
	wq_remove_locked(&x->wait, &w.entry);
	spin_unlock_irqrestore(&x->wait.lock, fl);
	return ret;
}

/* The timed forms: the remaining milliseconds (at least 1) when an event
 * was consumed, 0 on timeout, -ERESTARTSYS. */
static long completion_wait_timeout(struct completion *x, long timeout_ms,
				    int mode)
{
	uint64_t deadline = wq_deadline_after_ms(timeout_ms);
	long r = completion_wait(x, deadline, mode);
	uint64_t now;

	if (r != 1)
		return r;
	now = hrtimer_now_ns();
	if (now >= deadline)
		return 1;
	return (long)((deadline - now + 999999ULL) / 1000000ULL);
}

void wait_for_completion(struct completion *x)
{
	(void)completion_wait(x, 0, WQ_WAIT_UNINTERRUPTIBLE);
}

long wait_for_completion_timeout(struct completion *x, long timeout_ms)
{
	return completion_wait_timeout(x, timeout_ms, WQ_WAIT_UNINTERRUPTIBLE);
}

void wait_for_completion_io(struct completion *x)
{
	wait_for_completion(x);
}

long wait_for_completion_io_timeout(struct completion *x, long timeout_ms)
{
	return wait_for_completion_timeout(x, timeout_ms);
}

int wait_for_completion_interruptible(struct completion *x)
{
	long r = completion_wait(x, 0, WQ_WAIT_INTERRUPTIBLE);

	return r < 0 ? (int)r : 0;
}

long wait_for_completion_interruptible_timeout(struct completion *x,
					       long timeout_ms)
{
	return completion_wait_timeout(x, timeout_ms, WQ_WAIT_INTERRUPTIBLE);
}

int wait_for_completion_killable(struct completion *x)
{
	long r = completion_wait(x, 0, WQ_WAIT_KILLABLE);

	return r < 0 ? (int)r : 0;
}

long wait_for_completion_killable_timeout(struct completion *x,
					  long timeout_ms)
{
	return completion_wait_timeout(x, timeout_ms, WQ_WAIT_KILLABLE);
}
