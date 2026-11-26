// LikeOS — Deferred-work (softirq) framework
//
// Hard interrupt handlers run with interrupts disabled and *must* return
// quickly so that:
//   - other devices' IRQs are serviced promptly,
//   - inter-processor IPIs (TLB shootdown, reschedule) can be acknowledged,
//   - the hard-IRQ stack does not deepen unbounded.
//
// Heavy work (parsing a packet, walking the TCP state machine, waking
// blocked sockets) is deferred to *softirq context*: a per-CPU set of
// pending work bits that is drained
//   (a) at the tail of every IRQ handler before iret,
//   (b) by a per-CPU `ksoftirqd` kernel thread under sustained load,
//   (c) anywhere a path explicitly polls (e.g. after re-enabling IRQs).
//
// Softirq handlers run with interrupts ENABLED, so they cannot delay TLB
// shootdown IPIs no matter how long they take.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_SOFTIRQ_H
#define LIKEOS_SOFTIRQ_H

#include <kernel/uapi/types.h>

// Softirq vector numbers (must stay <= 31; we use a uint32_t bitmask).
enum {
	SOFTIRQ_NET_RX = 0, // process per-CPU RX skb queue
	SOFTIRQ_NET_TX = 1, // future use
	SOFTIRQ_TIMER = 2, // drain the deferred connection-free queue
	SOFTIRQ_TCP_TIMER = 3, // run the per-connection TCP timer sweep
	/* Drain the deferred free queue of AF_UNIX sockets.  A vector of its
	 * own rather than sharing SOFTIRQ_TIMER, which TCP already registers a
	 * handler on -- one handler per vector, and registering twice warns
	 * and loses one of them. */
	SOFTIRQ_UNIX = 4,
	/* Network housekeeping that must not run in the hard timer
	 * interrupt: the DHCP lease timer (it builds and sends packets) and
	 * the NIC link watchdogs.  Raised on CPU 0 only (net.c). */
	SOFTIRQ_NET_HOUSEKEEPING = 5,
	/* The network timer wheel (kernel/net/net_timer.c): neighbour
	 * state timers, device watchdogs and the like.  Raised on CPU 0
	 * from the timer tick. */
	SOFTIRQ_NET_TIMER = 6,
	NR_SOFTIRQ = 32
};

typedef void (*softirq_fn_t)(void);

// Register a handler for a softirq vector (call once at boot).
void softirq_register(uint32_t nr, softirq_fn_t fn);

// Mark vector `nr` as pending on the current CPU and (if a ksoftirqd
// thread is waiting) wake it.  Safe from any context, including hard IRQ.
void softirq_raise(uint32_t nr);

// The same for a caller that already runs with interrupts disabled.
static inline void softirq_raise_irqoff(uint32_t nr)
{
	softirq_raise(nr);
}

// Mark vector `nr` as pending on a specific CPU (used for cross-CPU
// notifications, e.g. completing an RX queue we wrote to from another CPU).
void softirq_raise_on(uint32_t cpu, uint32_t nr);

// Run the pending vectors of the current CPU.  From a context with
// interrupts disabled (the IRQ tail) it only wakes the CPU's ksoftirqd;
// with interrupts enabled (ksoftirqd) it runs the vectors with interrupts
// enabled, at most MAX_SOFTIRQ_RESTART passes and MAX_SOFTIRQ_TIME_NS
// (kernel/net/softirq_core.h), and leaves the rest to ksoftirqd, which
// yields the processor before it goes on.  Restores the caller's interrupt
// state on return.  Not re-entered: a run already in progress on this CPU
// picks up whatever is raised meanwhile.
void softirq_drain(void);

// Diagnostics: pending mask, ksoftirqd state and per-vector run counts.
uint32_t softirq_pending_get(uint32_t cpu);
int ksoftirqd_state_get(uint32_t cpu);
uint64_t softirq_count_get(uint32_t cpu, uint32_t nr);
uint64_t softirq_limit_hits_get(uint32_t cpu);

// Spawn one ksoftirqd kernel thread per online CPU.  Must be called after
// the scheduler and SMP are up.
void ksoftirqd_start_all(void);

#endif // LIKEOS_SOFTIRQ_H
