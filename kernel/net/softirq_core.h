// LikeOS — the softirq processing loop
//
// One run of the pending vectors of a CPU: take the pending mask, run every
// vector in it (lowest number first) with interrupts enabled, and repeat
// while vectors were raised again -- but at most `max_restart' times and
// for at most `time_limit_ns'.  When a limit stops the loop with work still
// pending, the caller hands the rest to the CPU's ksoftirqd, which yields
// the processor between runs.  A flood of interrupts that keeps re-raising
// a vector therefore cannot monopolise a CPU, and a burst still gets
// handled promptly.
//
// Kept free of any kernel state so the policy can be exercised on its own
// (host tests); kernel/net/softirq.c feeds it the per-CPU state.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_SOFTIRQ_CORE_H
#define LIKEOS_SOFTIRQ_CORE_H

#include <kernel/uapi/types.h>

/* Restart limit and time limit of one run (the values other mature
 * kernels use). */
#define MAX_SOFTIRQ_RESTART 10
#define MAX_SOFTIRQ_TIME_NS (2ULL * 1000 * 1000)

typedef void (*softirq_loop_fn_t)(void);

struct softirq_loop {
	volatile uint32_t *pending; // the CPU's pending mask
	softirq_loop_fn_t const *handlers; // [32]
	uint64_t *counts; // per-vector run counts [32], may be NULL
	uint64_t (*now_ns)(void); // monotonic clock
	int (*should_yield)(void); // e.g. a reschedule is due; may be NULL
	void (*irq_enable)(void);
	void (*irq_disable)(void);
	uint64_t time_limit_ns;
	int max_restart;
	// out
	int restarts; // passes beyond the first
	int stopped_by_limit; // 1: work left behind by a limit
};

/* Runs with interrupts disabled on entry and on return (the handlers run
 * with them enabled).  Returns the mask still pending when a limit ended
 * the run, 0 when everything was drained. */
static inline uint32_t softirq_loop_run(struct softirq_loop *l)
{
	uint64_t start = l->now_ns ? l->now_ns() : 0;
	int left_restarts = l->max_restart;

	l->restarts = 0;
	l->stopped_by_limit = 0;
	for (;;) {
		uint32_t mask = __atomic_exchange_n(l->pending, 0,
						    __ATOMIC_ACQ_REL);
		uint32_t again;

		if (mask == 0)
			return 0;
		l->irq_enable();
		while (mask) {
			uint32_t i = (uint32_t)__builtin_ctz(mask);

			mask &= mask - 1;
			if (l->handlers[i]) {
				if (l->counts)
					l->counts[i]++;
				l->handlers[i]();
			}
		}
		l->irq_disable();
		again = __atomic_load_n(l->pending, __ATOMIC_ACQUIRE);
		if (!again)
			return 0;
		if (--left_restarts <= 0 ||
		    (l->now_ns && l->now_ns() - start >= l->time_limit_ns) ||
		    (l->should_yield && l->should_yield())) {
			l->stopped_by_limit = 1;
			return again;
		}
		l->restarts++;
	}
}

#endif // LIKEOS_SOFTIRQ_CORE_H
