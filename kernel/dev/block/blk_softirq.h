// LikeOS -- the storage stack's own softirq (BLOCK softirq) and the
// per-CPU kblockd workers.
//
// A storage interrupt handler does the minimum with interrupts off: it
// acknowledges the controller, collects the finished commands and does
// the transport-level bookkeeping for them (slot release, result
// taskfile).  Everything that touches the block layer -- ending the
// request, copying a bounce buffer, waking the task that waits for the
// I/O, accounting, starting the next request -- runs afterwards in the
// block softirq: on the same CPU, right after the hard interrupt returned
// (at the interrupt tail, with interrupts enabled), or on the CPU that
// submitted the request when that CPU does not share the last-level cache
// with the interrupted one (an inter-processor interrupt carries it
// there).  Under sustained load the per-CPU thread kblksoftirqd/N takes
// over, so the softirq never monopolises a CPU.
//
// This framework is dedicated to the storage stack.  It shares nothing
// with the networking softirq vectors.
//
// Queue runs that must not happen in softirq (restart of a queue after a
// completion freed a tag, the timeout scan, requeue work) go to kblockd:
// one worker thread per CPU, a work item queued on the current CPU or on
// a chosen CPU, optionally delayed.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_BLK_SOFTIRQ_H
#define LIKEOS_BLK_SOFTIRQ_H

#include <kernel/uapi/types.h>

/* One unit of deferred completion.  Embedded in its owner (a block
 * request, a passthrough request); the owner recovers itself with
 * container_of-style arithmetic in fn.  `next' belongs to the framework
 * while the entry is queued.  An entry is queued at most once at a time
 * (the owner guarantees it: a request completes once). */
struct blk_softirq_entry {
	struct blk_softirq_entry *next;
	void (*fn)(struct blk_softirq_entry *e);
};

/* ---- set-up (kernel/ke/init.c) ---- */

/* Per-CPU done lists, the IPI vector, the cache topology table.  Boot,
 * interrupts off, before the first storage interrupt can arrive. */
void blk_softirq_init(void);
/* Start kblksoftirqd/N and kblockd/N on every online CPU.  After the
 * scheduler and SMP are up.  Before this, completions requested from
 * process context run inline and kblockd items run inline. */
void blk_softirq_start(void);
/* 1 once blk_softirq_start() ran. */
int blk_softirq_running(void);

/* ---- completion (any context, including hard interrupt) ---- */

/* Complete `e' on the CPU that should finish it: when `submit_cpu' is
 * this CPU, is negative, or shares the last-level cache with this CPU,
 * the entry goes on this CPU's done list and the block softirq is raised
 * here; otherwise it goes on submit_cpu's done list and an IPI raises the
 * softirq there.  Never runs fn synchronously from hard-interrupt
 * context.  From process context before blk_softirq_start() the entry
 * runs inline. */
void blk_softirq_complete(struct blk_softirq_entry *e, int submit_cpu);
/* Queue on this CPU's done list and raise, unconditionally local. */
void blk_softirq_complete_local(struct blk_softirq_entry *e);

/* The routing decision of blk_softirq_complete(), with the request
 * queue's properties spelled out, for a caller that finishes a request
 * itself when deferring gains nothing.  Returns 1 when `e' was handed to
 * a softirq (this CPU's, or submit_cpu's through an IPI), 0 when the
 * caller must run e->fn(e) now, in its own context:
 *   - BLK_SOFTIRQ_POLLED: a polled request;
 *   - the hardware queue is served by one CPU only (a single-queue
 *     device on a one-CPU system, or BLK_SOFTIRQ_HCTX_ONE_CTX) and that
 *     CPU is submit_cpu, which is this one;
 *   - BLK_SOFTIRQ_MULTI_HWQ and no IPI is needed;
 *   - from process context before blk_softirq_start().
 * BLK_SOFTIRQ_SAME_COMP: complete on the submitter's cache domain
 * (queue attribute rq_affinity >= 1); BLK_SOFTIRQ_SAME_FORCE: on the
 * submitter itself (rq_affinity 2).  Any context. */
#define BLK_SOFTIRQ_SAME_COMP 0x1u
#define BLK_SOFTIRQ_SAME_FORCE 0x2u
#define BLK_SOFTIRQ_POLLED 0x4u
#define BLK_SOFTIRQ_MULTI_HWQ 0x8u
#define BLK_SOFTIRQ_HCTX_ONE_CTX 0x10u
int blk_softirq_complete_remote(struct blk_softirq_entry *e, int submit_cpu,
				unsigned int flags);

/* 1 when CPUs a and b share the last-level cache. */
int blk_cpus_share_cache(uint32_t a, uint32_t b);

/* ---- context queries ---- */

/* 1 while this CPU runs block softirq handlers (interrupt tail or
 * kblksoftirqd).  Code that may sleep must not run then. */
int blk_in_softirq(void);
/* 1 while this CPU is inside a hard interrupt handler (between the
 * entry of the dispatch and its tail). */
int blk_in_hardirq(void);

/* ---- interrupt tail (kernel/ke/interrupt.c, irq.c) ---- */

/* Run this CPU's pending block softirq work.  Called at the tail of
 * every hard interrupt dispatch (with interrupts off; they are enabled
 * while handlers run and the caller's state is restored), and by
 * kblksoftirqd.  Does nothing when called nested inside a hard interrupt
 * or while this CPU already runs the block softirq.  Bounded per call
 * (restart count and time budget); the rest goes to kblksoftirqd/N. */
void blk_softirq_irq_tail(void);
/* Hard interrupt accounting: every interrupt and IPI dispatch calls
 * enter first and exit before blk_softirq_irq_tail() (interrupts off). */
void blk_hardirq_enter(void);
void blk_hardirq_exit(void);
/* The block-softirq IPI arrived (ipi_handler, IPI_BLOCK_SOFTIRQ_VECTOR):
 * raise the softirq on this CPU; the IPI's tail runs it. */
void blk_softirq_ipi(void);
/* Process context (a poller, interrupts on or off, not inside a hard
 * interrupt or a softirq run): run this CPU's pending block softirq work
 * now, in the caller.  With interrupts off the entries run with them off,
 * and entries another CPU queued here are run although their IPI cannot
 * be taken yet.  Bounded per call like an interrupt-tail run; a polling
 * loop calls it again.  Before and after blk_softirq_start().  The
 * counterpart of blk_work_run_pending() for waits that poll. */
void blk_softirq_poll(void);

/* ---- kblockd: per-CPU worker threads ---- */

/* Defined in blkdev.h (struct blk_work, blk_work_init); kblockd queues
 * them per CPU from here on. */
struct blk_work;
/* Queue on the current CPU's kblockd.  Any context.  1 when queued, 0
 * when already pending (not queued twice). */
int kblockd_schedule_work(struct blk_work *w);
/* Queue on `cpu''s kblockd after `delay_ns' (0 = now).  When already
 * pending, the delay is modified to the new value.  Any context.  0 when
 * the item was idle and is queued now, 1 when it was pending (on a list
 * or on its timer) and has been re-aimed. */
int kblockd_mod_delayed_work_on(uint32_t cpu, struct blk_work *w,
				uint64_t delay_ns);

#endif // LIKEOS_BLK_SOFTIRQ_H
