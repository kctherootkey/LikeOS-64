// LikeOS - the out-of-line half of the wait_event family (waitq.h)
//
// The macros in waitq.h evaluate the caller's condition; everything else --
// queueing, publishing the sleep, taking it back, the timed sleep with its
// hrtimer, signals, and the polling fallback where nothing may sleep --
// lives here so the expansion at every call site stays small.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>

/* While polling, the condition is looked at again after this many pauses
 * (a few microseconds), and the clock is read only then. */
#define WQ_POLL_PAUSES 64

/* The scheduler-tick backstop of a sleep without a deadline: the task is
 * woken this often to look at its condition again even if a waker were
 * lost.  The protocol does not lose wakers; this only bounds the damage
 * of a driver that changes the condition and forgets wake_up(). */
#define WQ_BACKSTOP_NS (1000ULL * 1000ULL * 1000ULL)

int task_fatal_signal_pending(task_t *t)
{
	if (!t)
		return 0;
	return sigismember_k(&t->signals.pending, SIGKILL) ? 1 : 0;
}

uint64_t wq_deadline_after_ms(long ms)
{
	uint64_t now = hrtimer_now_ns();

	if (now == 0)
		now = 1; /* 0 means "no deadline" */
	if (ms <= 0)
		return now;
	return now + (uint64_t)ms * 1000000ULL;
}

void wq_waiter_init(struct wq_waiter *w, struct wait_queue_head *h,
		    uint64_t deadline_ns, int mode)
{
	task_t *cur = sched_current();

	BUG_ON(w == NULL || h == NULL);
	w->head = h;
	w->deadline_ns = deadline_ns;
	w->mode = mode;
	w->blocked = 0;
	/* Sleeping needs a task to park and interrupts to be woken by. */
	w->task = (cur && irqs_enabled()) ? cur : NULL;
	wq_entry_init(&w->entry, w->task);
	if (w->task)
		might_sleep();
}

/* Publish the sleep of a queued waiter. */
static void wq_waiter_publish(struct wq_waiter *w)
{
	task_t *cur = w->task;
	uint64_t now, tick_ns;

	cur->wait_channel = w->head;
	now = hrtimer_now_ns();
	if (w->deadline_ns)
		tick_ns = w->deadline_ns > now ? w->deadline_ns - now : 0;
	else
		tick_ns = WQ_BACKSTOP_NS;
	cur->wakeup_tick = timer_ticks() + timer_ns_to_ticks(tick_ns) + 1;
	task_set_blocked(cur);
	w->blocked = 1;
}

void wq_waiter_prepare(struct wq_waiter *w)
{
	if (!w->task)
		return;
	if (!wq_entry_queued(&w->entry))
		wq_add(w->head, &w->entry);
	wq_waiter_publish(w);
}

void wq_waiter_prepare_locked(struct wq_waiter *w)
{
	lockdep_assert_held(&w->head->lock);
	if (!w->task)
		return;
	wq_add_locked(w->head, &w->entry);
	wq_waiter_publish(w);
}

/* Take back a published sleep that is not going to happen.  A waker that
 * claimed the task meanwhile (BLOCKED -> READY) has queued it to run;
 * passing through the scheduler once consumes that. */
static void wq_waiter_unblock(struct wq_waiter *w)
{
	task_t *cur = w->task;
	task_state_t expected = TASK_BLOCKED;

	if (!cur || !w->blocked)
		return;
	w->blocked = 0;
	if (!__atomic_compare_exchange_n(&cur->state, &expected, TASK_RUNNING,
					 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		sched_schedule();
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	if (cur->state != TASK_RUNNING)
		cur->state = TASK_RUNNING;
}

void wq_waiter_finish(struct wq_waiter *w)
{
	if (!w->task)
		return;
	wq_waiter_unblock(w);
	wq_remove(w->head, &w->entry);
}

static int wq_signal_ends_wait(const struct wq_waiter *w)
{
	if (!w->task)
		return 0;
	if (w->mode == WQ_WAIT_INTERRUPTIBLE)
		return signal_pending(w->task);
	if (w->mode == WQ_WAIT_KILLABLE)
		return task_fatal_signal_pending(w->task);
	return 0;
}

int wq_waiter_sleep(struct wq_waiter *w)
{
	task_t *cur = w->task;

	if (!cur) {
		/* Polling: nothing may sleep here. */
		for (int i = 0; i < WQ_POLL_PAUSES; i++)
			__asm__ volatile("pause" ::: "memory");
		if (w->deadline_ns && hrtimer_now_ns() >= w->deadline_ns)
			return 1;
		return 0;
	}

	/* A signal that is already pending ends an interruptible wait before
	 * it sleeps (it would only be woken at once). */
	if (wq_signal_ends_wait(w)) {
		wq_waiter_finish(w);
		return -ERESTARTSYS;
	}
	if (w->deadline_ns && hrtimer_now_ns() >= w->deadline_ns) {
		wq_waiter_finish(w);
		return 1;
	}

	if (w->deadline_ns)
		hrtimer_wait_arm(cur, w->deadline_ns);
	w->blocked = 0;
	sched_schedule();
	if (w->deadline_ns)
		hrtimer_wait_disarm(cur);
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	if (cur->state != TASK_RUNNING)
		cur->state = TASK_RUNNING;

	if (wq_signal_ends_wait(w)) {
		wq_remove(w->head, &w->entry);
		return -ERESTARTSYS;
	}
	if (w->deadline_ns && hrtimer_now_ns() >= w->deadline_ns) {
		wq_remove(w->head, &w->entry);
		return 1;
	}
	/* Still queued (a wake-up unlinks the entry, a tick or a spurious
	 * wake-up does not): prepare() queues it again when needed. */
	return 0;
}
