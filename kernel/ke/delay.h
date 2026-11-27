// LikeOS kernel - delays and short sleeps
//
// Drivers wait for hardware in two ways.  A busy-wait (udelay, mdelay)
// spins the processor for the time asked; it works in any context, also
// with interrupts off or before there is a scheduler, and it is the only
// choice inside an interrupt handler or under a spinlock.  A sleep
// (msleep, usleep_range) gives the processor away and is woken by a
// high-resolution timer at the deadline; it may only be used where the
// caller may block: process context, interrupts on, no spinlock held.
//
// So that early boot code and late code can share one path, the sleeping
// forms fall back to busy-waiting by themselves when they are called
// before the scheduler runs or with interrupts disabled (as the wait
// queue primitives do).  That is a convenience for boot, not a licence:
// might_sleep() still reports a sleep with interrupts off once the
// system is up.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_DELAY_H
#define KERNEL_KE_DELAY_H

#include <kernel/uapi/types.h>

/* Busy-wait (any context). */
void udelay(unsigned long usecs);
void mdelay(unsigned long msecs);
/* Sleep at least `msecs' milliseconds; signals do not end it. */
void msleep(unsigned int msecs);
/* Sleep at least `msecs' ms or until a signal is pending for the caller:
 * the milliseconds left (0 when the time passed). */
unsigned long msleep_interruptible(unsigned int msecs);
/* Sleep between `min_us' and `max_us' microseconds (the timer is aimed at
 * the earliest point; the range lets a caller say how much slack it can
 * take).  Below ~10 us a busy-wait is used, as a sleep costs more. */
void usleep_range(unsigned long min_us, unsigned long max_us);
/* fsleep: the right one of udelay/usleep_range/msleep for `usecs'. */
void fsleep(unsigned long usecs);
/* May the caller sleep right now (scheduler running, interrupts on). */
int delay_can_sleep(void);

#endif /* KERNEL_KE_DELAY_H */
