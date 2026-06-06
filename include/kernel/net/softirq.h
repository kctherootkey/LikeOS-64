// LikeOS-64 — Deferred-work (softirq) framework
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

#ifndef LIKEOS_SOFTIRQ_H
#define LIKEOS_SOFTIRQ_H

#include "types.h"

// Softirq vector numbers (must stay <= 31; we use a uint32_t bitmask).
enum {
    SOFTIRQ_NET_RX = 0,    // process per-CPU RX skb queue
    SOFTIRQ_NET_TX = 1,    // future use
    SOFTIRQ_TIMER  = 2,    // future use
    NR_SOFTIRQ     = 32
};

typedef void (*softirq_fn_t)(void);

// Register a handler for a softirq vector (call once at boot).
void softirq_register(uint32_t nr, softirq_fn_t fn);

// Mark vector `nr` as pending on the current CPU and (if a ksoftirqd
// thread is waiting) wake it.  Safe from any context, including hard IRQ.
void softirq_raise(uint32_t nr);

// Mark vector `nr` as pending on a specific CPU (used for cross-CPU
// notifications, e.g. completing an RX queue we wrote to from another CPU).
void softirq_raise_on(uint32_t cpu, uint32_t nr);

// Drain all pending vectors on the current CPU.  Called from the IRQ tail
// (with interrupts about to be re-enabled by iret), from ksoftirqd, and
// from explicit polling sites.  Re-enables interrupts while running each
// handler; restores the caller's IF state on return.
void softirq_drain(void);

// Spawn one ksoftirqd kernel thread per online CPU.  Must be called after
// the scheduler and SMP are up.
void ksoftirqd_start_all(void);

#endif // LIKEOS_SOFTIRQ_H
