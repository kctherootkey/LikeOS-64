// LikeOS — network timers
//
// A timer is a deadline in timer ticks and a callback.  Armed timers sit in
// a hashed wheel (one slot per tick modulo the wheel size); the network
// timer softirq (SOFTIRQ_NET_TIMER, CPU 0, raised from the timer tick while
// any timer is armed) walks the slots of the ticks that have passed and
// runs every timer whose deadline is due.
//
// Callbacks run in softirq context: interrupts enabled, no lock held, never
// two at once.  They may re-arm their own timer or any other one.
//
// Lifetime: net_timer_del() and net_timer_mod() return 1 when they removed
// an arming (whose callback then does not run for it).  An object whose
// pending timer holds a reference on it drops that reference in the
// callback, or after a successful del -- the neighbour code follows that
// pattern, so it never has to wait for a running callback.  An owner that
// frees memory the callback uses without such a reference calls
// net_timer_del_sync() (process or softirq context, never from the
// callback itself), which also waits until a running callback returned.
//
// Lock order: the wheel lock is a leaf; it may be taken under any other
// network lock (net_timer_mod under a neighbour's lock), and no callback
// runs with it held.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NET_TIMER_H
#define LIKEOS_NET_TIMER_H

#include <kernel/uapi/types.h>

struct net_timer;
typedef void (*net_timer_fn_t)(struct net_timer *t);

typedef struct net_timer {
	struct net_timer *next; // wheel slot linkage
	struct net_timer **pprev;
	uint64_t expires; // tick
	net_timer_fn_t fn;
	volatile int pending; // in the wheel
} net_timer_t;

void net_timer_init_subsys(void); // net_init(): registers the softirq

void net_timer_setup(net_timer_t *t, net_timer_fn_t fn);

// Arm (or move) for the absolute tick `expires'.  A deadline in the past
// fires at the next run.  Returns 1 if the timer was pending before.
int net_timer_mod(net_timer_t *t, uint64_t expires);

// Disarm.  Returns 1 if it was pending (its callback will not run for that
// arming), 0 if it was not -- its callback may then be running right now.
int net_timer_del(net_timer_t *t);

// Disarm and wait for a running callback of this timer to return.  Never
// from that callback; never with interrupts disabled.
int net_timer_del_sync(net_timer_t *t);

static inline int net_timer_pending(const net_timer_t *t)
{
	return __atomic_load_n(&t->pending, __ATOMIC_ACQUIRE);
}

// The timer tick (net_timer_tick, CPU 0): raise the softirq when timers
// are armed.
void net_timer_tick_hook(void);

// Run the due timers (the softirq handler; exported for the host tests).
void net_timer_run(void);

// Number of armed timers (diagnostics, tests).
uint32_t net_timer_armed(void);

// ---- timespec timeouts given by user space (recvmmsg) ----

/* The deadline `sec' s + `nsec' ns (validated: both >= 0, nsec below one
 * second) after `now' (ns).  Returns 0 with *deadline set, or 1 when the
 * sum does not fit in 64 bits of nanoseconds (~584 years): a timeout that
 * never runs out. */
static inline int net_ts_deadline(uint64_t now, int64_t sec, int64_t nsec,
				  uint64_t *deadline)
{
	uint64_t room = ~0ULL - now;

	if ((uint64_t)nsec > room ||
	    (uint64_t)sec > (room - (uint64_t)nsec) / 1000000000ULL)
		return 1;
	*deadline = now + (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
	return 0;
}

/* The time left at `now' as {sec, nsec}: until `deadline', or -- for a
 * timeout that never runs out -- the given {sec, nsec} less the time
 * spent since `start'. */
static inline void net_ts_left(uint64_t now, uint64_t start,
			       uint64_t deadline, int forever, int64_t sec,
			       int64_t nsec, int64_t out[2])
{
	if (forever) {
		uint64_t spent = now > start ? now - start : 0;
		int64_t s = sec - (int64_t)(spent / 1000000000ULL);
		int64_t ns = nsec - (int64_t)(spent % 1000000000ULL);

		if (ns < 0) {
			ns += 1000000000LL;
			s--;
		}
		out[0] = s;
		out[1] = ns;
	} else {
		uint64_t left = deadline > now ? deadline - now : 0;

		out[0] = (int64_t)(left / 1000000000ULL);
		out[1] = (int64_t)(left % 1000000000ULL);
	}
}

#endif // LIKEOS_NET_TIMER_H
