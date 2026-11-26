// LikeOS — network timer wheel
//
// See kernel/net/net_timer.h.
//
// The wheel has NET_TIMER_SLOTS slots; a timer lives in slot
// (expires % NET_TIMER_SLOTS).  A run visits the slots of every tick since
// the previous run (all of them when more than a wheel turn passed) and
// fires the timers in them whose deadline has come; timers of a later turn
// stay where they are.  Each timer is unlinked under the wheel lock just
// before its callback runs without the lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net_timer.h>
#include <kernel/net/softirq.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

#define NET_TIMER_SLOTS 512

static net_timer_t *wheel[NET_TIMER_SLOTS];
static spinlock_t wheel_lock = SPINLOCK_INIT("net_timer");
static uint64_t last_run; // last tick whose slot was visited
static volatile uint32_t armed;
static net_timer_t *volatile running; // callback in progress
static volatile int run_active; // a CPU is inside net_timer_run

static void unlink_locked(net_timer_t *t)
{
	*t->pprev = t->next;
	if (t->next)
		t->next->pprev = t->pprev;
	t->next = NULL;
	t->pprev = NULL;
	__atomic_store_n(&t->pending, 0, __ATOMIC_RELEASE);
	armed--;
}

static void link_locked(net_timer_t *t)
{
	net_timer_t **slot = &wheel[t->expires % NET_TIMER_SLOTS];

	t->next = *slot;
	if (t->next)
		t->next->pprev = &t->next;
	t->pprev = slot;
	*slot = t;
	__atomic_store_n(&t->pending, 1, __ATOMIC_RELEASE);
	armed++;
}

void net_timer_setup(net_timer_t *t, net_timer_fn_t fn)
{
	BUG_ON(fn == NULL);
	t->next = NULL;
	t->pprev = NULL;
	t->expires = 0;
	t->fn = fn;
	t->pending = 0;
}

int net_timer_mod(net_timer_t *t, uint64_t expires)
{
	uint64_t f;
	int was;

	BUG_ON(t->fn == NULL);
	spin_lock_irqsave(&wheel_lock, &f);
	was = t->pending;
	if (was)
		unlink_locked(t);
	/* A deadline at or before the last tick a run has served goes to
	 * the next tick's slot: that slot is still to be visited, a past
	 * one would not be for a whole wheel turn.  (This also keeps a
	 * callback that re-arms itself for "now" from being run again by
	 * the same run for ever.) */
	if (expires <= last_run)
		expires = last_run + 1;
	t->expires = expires;
	link_locked(t);
	spin_unlock_irqrestore(&wheel_lock, f);
	return was;
}

int net_timer_del(net_timer_t *t)
{
	uint64_t f;
	int was;

	spin_lock_irqsave(&wheel_lock, &f);
	was = t->pending;
	if (was)
		unlink_locked(t);
	spin_unlock_irqrestore(&wheel_lock, f);
	return was;
}

int net_timer_del_sync(net_timer_t *t)
{
	int was = net_timer_del(t);

	WARN_ON_ONCE(irqs_disabled());
	while (__atomic_load_n(&running, __ATOMIC_ACQUIRE) == t)
		__asm__ volatile("pause" ::: "memory");
	return was;
}

uint32_t net_timer_armed(void)
{
	return __atomic_load_n(&armed, __ATOMIC_RELAXED);
}

void net_timer_tick_hook(void)
{
	if (__atomic_load_n(&armed, __ATOMIC_RELAXED))
		softirq_raise(SOFTIRQ_NET_TIMER);
}

/* First due timer of `slot', unlinked; NULL when none is due. */
static net_timer_t *take_due_locked(uint32_t slot, uint64_t now)
{
	for (net_timer_t *t = wheel[slot]; t; t = t->next) {
		if (t->expires <= now) {
			unlink_locked(t);
			return t;
		}
	}
	return NULL;
}

void net_timer_run(void)
{
	uint64_t now = timer_ticks();
	uint64_t from, f;

	if (__atomic_exchange_n(&run_active, 1, __ATOMIC_ACQ_REL))
		return; // another CPU is running the wheel
	spin_lock_irqsave(&wheel_lock, &f);
	from = last_run + 1;
	if (now >= from && now - from >= NET_TIMER_SLOTS)
		from = now - (NET_TIMER_SLOTS - 1); // a full turn: every slot
	if (now > last_run)
		last_run = now; // from here on, arming for <= now goes to now+1
	for (uint64_t tick = from; tick <= now; tick++) {
		uint32_t slot = (uint32_t)(tick % NET_TIMER_SLOTS);
		net_timer_t *t;

		while ((t = take_due_locked(slot, now)) != NULL) {
			__atomic_store_n(&running, t, __ATOMIC_RELEASE);
			spin_unlock_irqrestore(&wheel_lock, f);
			t->fn(t);
			spin_lock_irqsave(&wheel_lock, &f);
			__atomic_store_n(&running, NULL, __ATOMIC_RELEASE);
		}
	}
	spin_unlock_irqrestore(&wheel_lock, f);
	__atomic_store_n(&run_active, 0, __ATOMIC_RELEASE);
}

void net_timer_init_subsys(void)
{
	last_run = timer_ticks();
	softirq_register(SOFTIRQ_NET_TIMER, net_timer_run);
}
