// LikeOS kernel - delays and short sleeps
//
// Busy-waits go to the local APIC timer's calibrated delay loop (which
// falls back to the PIT before calibration).  Sleeps park the task on a
// high-resolution timer at an absolute monotonic deadline, so a sleep is
// never shorter than asked and wakes within the timer's resolution, not
// at the next scheduler tick.  A signal that ends the timer sleep early is
// absorbed (the rest is slept) except for msleep_interruptible.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/delay.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>
#include <kernel/uapi/bug.h>

/* Below this a sleep costs more than it saves: busy-wait instead. */
#define DELAY_SLEEP_MIN_US 10u

int delay_can_sleep(void)
{
	return sched_current() != NULL && !irqs_disabled();
}

void udelay(unsigned long usecs)
{
	/* lapic_delay_us takes 32 bits; long waits go in chunks. */
	while (usecs) {
		unsigned long chunk = usecs > 1000000ul ? 1000000ul : usecs;

		lapic_delay_us((uint32_t)chunk);
		usecs -= chunk;
	}
}

void mdelay(unsigned long msecs)
{
	while (msecs) {
		unsigned long chunk = msecs > 100ul ? 100ul : msecs;

		lapic_delay_ms((uint32_t)chunk);
		msecs -= chunk;
	}
}

/* Sleep until the absolute deadline; a signal does not cut it short. */
static void sleep_until_ns(uint64_t until)
{
	while (hrtimer_sleep_until(until, NULL) != 0)
		; /* a signal woke the task early: sleep the rest */
}

void msleep(unsigned int msecs)
{
	if (!delay_can_sleep()) {
		mdelay(msecs);
		return;
	}
	might_sleep();
	sleep_until_ns(hrtimer_now_ns() + (uint64_t)msecs * 1000000ull);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
	uint64_t rem = 0;

	if (!delay_can_sleep()) {
		mdelay(msecs);
		return 0;
	}
	might_sleep();
	if (hrtimer_sleep_until(hrtimer_now_ns() + (uint64_t)msecs * 1000000ull,
				&rem) == 0)
		return 0;
	/* Round the remainder up: a caller loops on "time left". */
	return (unsigned long)((rem + 999999ull) / 1000000ull);
}

void usleep_range(unsigned long min_us, unsigned long max_us)
{
	if (WARN_ON_ONCE(max_us < min_us))
		max_us = min_us;
	if (min_us < DELAY_SLEEP_MIN_US || !delay_can_sleep()) {
		udelay(min_us);
		return;
	}
	might_sleep();
	/* Aim at the start of the range: the timer is precise, so the end of
	 * the range is never needed as slack here. */
	sleep_until_ns(hrtimer_now_ns() + (uint64_t)min_us * 1000ull);
}

void fsleep(unsigned long usecs)
{
	if (usecs < DELAY_SLEEP_MIN_US)
		udelay(usecs);
	else if (usecs < 20000ul)
		usleep_range(usecs, 2 * usecs);
	else
		msleep((unsigned int)((usecs + 999ul) / 1000ul));
}
