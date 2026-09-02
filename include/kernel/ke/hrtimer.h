// LikeOS-64 -- high-resolution timers.
//
// The scheduler tick is periodic and coarse (100-200 Hz as calibrated), and
// every timed wait used to be rounded up to it.  That is fine for a shell
// and useless for a display refresh, a media pipeline or a runtime's timed
// waits, all of which want a callback within tens of microseconds of a
// nanosecond deadline.
//
// A timer here is a deadline on the monotonic nanosecond clock and a
// callback.  The queue is programmed into an HPET comparator in one-shot
// mode when the platform has one; otherwise it is scanned from the periodic
// tick, which keeps every user working at tick granularity.  Callbacks run
// in interrupt context on the CPU that took the timer interrupt, with
// interrupts disabled: they may wake tasks (sched_claim_wake +
// sched_enqueue_ready, poll_notify_wq) and re-arm timers, and nothing else.
#ifndef KERNEL_KE_HRTIMER_H
#define KERNEL_KE_HRTIMER_H

#include <kernel/uapi/types.h>

struct hrtimer;
typedef void (*hrtimer_fn_t)(struct hrtimer *t);

typedef struct hrtimer {
	uint64_t expires_ns; /* monotonic deadline */
	hrtimer_fn_t fn;
	void *arg;
	struct hrtimer *next; /* queue link, valid while armed */
	volatile int armed;
} hrtimer_t;

/* Monotonic time in nanoseconds (the CLOCK_MONOTONIC the timers run on). */
uint64_t hrtimer_now_ns(void);

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg);
/* Arm for an absolute monotonic deadline.  Re-arming an armed timer moves
 * it.  A deadline already in the past fires at the next opportunity. */
void hrtimer_start(hrtimer_t *t, uint64_t abs_ns);
static inline void hrtimer_start_rel(hrtimer_t *t, uint64_t rel_ns)
{
	hrtimer_start(t, hrtimer_now_ns() + rel_ns);
}
/* Disarm.  Returns 1 if the timer was armed (and so has not fired), 0 if
 * it had already fired or was never started.  Safe against a concurrent
 * expiry: on return the callback is not running and will not run. */
int hrtimer_cancel(hrtimer_t *t);
static inline int hrtimer_is_armed(const hrtimer_t *t)
{
	return t->armed;
}

/* Sleep the calling task until `abs_ns' (monotonic) or a signal.
 * Returns 0 when the deadline passed, -EINTR when a signal cut it short
 * (with *remaining_ns, if non-NULL, set to what was left). */
int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns);

/* Platform hooks. */
void hrtimer_init_hw(void); /* after timer_init_hpet(), before use */
void hrtimer_irq(void); /* the HPET comparator interrupt */
void hrtimer_tick(void); /* from the periodic tick (fallback + safety net) */
int hrtimer_is_highres(void); /* 1 when an HPET comparator drives it */

#endif
