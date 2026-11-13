// LikeOS - the general sleeping mutex (mutex.h)
//
// State (locked, owner) is protected by the wait queue head's spinlock.
// A locker that finds the mutex held queues itself and publishes its sleep
// UNDER that lock; the unlocker clears `locked' and claims the first
// sleeping waiter under the same lock, so a release can never fall between
// a locker's test and its sleep.  The woken waiter competes for the mutex
// again (no hand-over: a running locker may take it first, which keeps the
// lock cheap under light contention), and one that gives up -- a signal
// ended an interruptible lock -- passes the wake-up on if the mutex is
// free, so a waiter is never left asleep on a free mutex.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/mutex.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/io/console.h>

/* A held mutex is looked at again this often by its sleepers even without
 * a wake-up (a backstop only: unlock always wakes one). */
#define MUTEX_BACKSTOP_MS 1000

static uint64_t mutex_self(void)
{
	task_t *cur = sched_current();

	return cur ? (uint64_t)(uint32_t)cur->id : MUTEX_BOOT_OWNER;
}

void mutex_init(struct mutex *m, const char *name)
{
	BUG_ON(m == NULL);
	m->locked = 0;
	m->owner = MUTEX_NO_OWNER;
	m->name = name ? name : "mutex";
	wq_head_init(&m->wait, m->name);
	m->contended = 0;
}

void mutex_destroy(struct mutex *m)
{
	if (!m)
		return;
	WARN(m->locked, "mutex %s destroyed while held by %lld", m->name,
	     (long long)m->owner);
	WARN(m->wait.first != NULL, "mutex %s destroyed with waiters",
	     m->name);
}

/* Claim the first sleeping waiter (wait lock held); NULL when none. */
static task_t *mutex_claim_waiter_locked(struct mutex *m)
{
	struct wait_queue_entry *e;

	lockdep_assert_held(&m->wait.lock);
	for (e = m->wait.first; e; e = e->next) {
		if (!e->task || !sched_claim_wake(e->task, TASK_BLOCKED))
			continue;
		*e->pprev = e->next;
		if (e->next)
			e->next->pprev = e->pprev;
		e->next = NULL;
		e->pprev = NULL;
		e->task->wait_channel = NULL;
		e->task->wakeup_tick = 0;
		return e->task;
	}
	return NULL;
}

/* Unlink `e' if queued (wait lock held). */
static void mutex_unlink_locked(struct wait_queue_entry *e)
{
	if (!e->pprev)
		return;
	*e->pprev = e->next;
	if (e->next)
		e->next->pprev = e->pprev;
	e->next = NULL;
	e->pprev = NULL;
}

/* Signals that end a lock attempt in `mode' (WQ_WAIT_*). */
static int mutex_signal_ends(task_t *cur, int mode)
{
	if (mode == WQ_WAIT_INTERRUPTIBLE)
		return signal_pending(cur);
	if (mode == WQ_WAIT_KILLABLE)
		return task_fatal_signal_pending(cur);
	return 0;
}

static int mutex_lock_common(struct mutex *m, int mode)
{
	task_t *cur = sched_current();
	uint64_t me = mutex_self();
	int can_sleep = cur != NULL && irqs_enabled();
	uint64_t spins = 0;
	int warned = 0;
	int requeue_front = 0; /* slept once: claimed waiters keep their turn */
	struct wait_queue_entry we;

	BUG_ON(m == NULL);
	if (can_sleep)
		might_sleep();
	/* Not recursive (reading owner unlocked is fine for this test: if it
	 * is us, nobody else can change it). */
	BUG_ON(m->locked && m->owner == me && cur != NULL);
	wq_entry_init(&we, cur);

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&m->wait.lock, &fl);
		if (!m->locked) {
			m->locked = 1;
			m->owner = me;
			mutex_unlink_locked(&we);
			spin_unlock_irqrestore(&m->wait.lock, fl);
			task_sleep_lock_taken();
			return 0;
		}
		if (!can_sleep) {
			spin_unlock_irqrestore(&m->wait.lock, fl);
			__asm__ volatile("pause" ::: "memory");
			if (++spins == 200000000ULL && !warned) {
				warned = 1;
				kprintf("mutex %s: spinning for a long time "
					"(owner %lld)\n",
					m->name, (long long)m->owner);
			}
			continue;
		}
		if (mutex_signal_ends(cur, mode)) {
			mutex_unlink_locked(&we);
			spin_unlock_irqrestore(&m->wait.lock, fl);
			return -EINTR;
		}
		m->contended++;
		/* First time: queued at the tail, so the unlocker's claim of
		 * the first waiter wakes the longest waiter.  A waiter that
		 * was claimed and lost the lock to a running locker goes back
		 * to the head: it keeps its turn. */
		if (!we.pprev) {
			if (requeue_front) {
				we.next = m->wait.first;
				if (we.next)
					we.next->pprev = &we.next;
				m->wait.first = &we;
				we.pprev = &m->wait.first;
			} else {
				wq_add_locked(&m->wait, &we);
			}
		}
		cur->wait_channel = &m->wait;
		cur->wakeup_tick = timer_ticks() +
				   timer_ms_to_ticks(MUTEX_BACKSTOP_MS) + 1;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&m->wait.lock, fl);
		sched_schedule();
		requeue_front = 1;
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		if (cur->state != TASK_RUNNING)
			cur->state = TASK_RUNNING;
		if (mode != WQ_WAIT_UNINTERRUPTIBLE &&
		    mutex_signal_ends(cur, mode)) {
			task_t *pass = NULL;

			/* Giving up after a wake-up: hand it on when the mutex
			 * is free, or that wake-up would be lost. */
			spin_lock_irqsave(&m->wait.lock, &fl);
			mutex_unlink_locked(&we);
			if (!m->locked)
				pass = mutex_claim_waiter_locked(m);
			spin_unlock_irqrestore(&m->wait.lock, fl);
			if (pass)
				sched_enqueue_ready(pass);
			return -EINTR;
		}
	}
}

void mutex_lock(struct mutex *m)
{
	(void)mutex_lock_common(m, WQ_WAIT_UNINTERRUPTIBLE);
}

int mutex_lock_interruptible(struct mutex *m)
{
	return mutex_lock_common(m, WQ_WAIT_INTERRUPTIBLE);
}

int mutex_lock_killable(struct mutex *m)
{
	return mutex_lock_common(m, WQ_WAIT_KILLABLE);
}

int mutex_trylock(struct mutex *m)
{
	uint64_t fl;
	int got = 0;

	BUG_ON(m == NULL);
	spin_lock_irqsave(&m->wait.lock, &fl);
	if (!m->locked) {
		m->locked = 1;
		m->owner = mutex_self();
		got = 1;
	}
	spin_unlock_irqrestore(&m->wait.lock, fl);
	if (got)
		task_sleep_lock_taken();
	return got;
}

void mutex_unlock(struct mutex *m)
{
	uint64_t fl;
	uint64_t me = mutex_self();
	task_t *next;

	BUG_ON(m == NULL);
	spin_lock_irqsave(&m->wait.lock, &fl);
	if (WARN(!m->locked || m->owner != me,
		 "mutex %s unlocked by %lld, held=%d owner=%lld", m->name,
		 (long long)me, m->locked, (long long)m->owner)) {
		spin_unlock_irqrestore(&m->wait.lock, fl);
		return;
	}
	m->locked = 0;
	m->owner = MUTEX_NO_OWNER;
	next = mutex_claim_waiter_locked(m);
	spin_unlock_irqrestore(&m->wait.lock, fl);
	task_sleep_lock_dropped();
	if (next)
		sched_enqueue_ready(next);
}

int mutex_held(const struct mutex *m)
{
	if (!m)
		return 0;
	return m->locked && m->owner == mutex_self();
}

int mutex_release_if_owner(struct mutex *m, uint64_t task_id)
{
	uint64_t fl;
	task_t *next = NULL;
	int released = 0;

	if (!m)
		return 0;
	spin_lock_irqsave(&m->wait.lock, &fl);
	if (m->locked && m->owner == task_id) {
		m->locked = 0;
		m->owner = MUTEX_NO_OWNER;
		next = mutex_claim_waiter_locked(m);
		released = 1;
	}
	spin_unlock_irqrestore(&m->wait.lock, fl);
	if (next)
		sched_enqueue_ready(next);
	return released;
}
