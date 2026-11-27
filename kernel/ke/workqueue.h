// LikeOS kernel - generic deferred work (work queues)
//
// Some work cannot be done where the need for it arises: an interrupt
// handler or a timer callback may not sleep, a lock may be held, or the
// work simply belongs to another context.  A work item (struct
// work_struct) names a function; queue_work() hands it to a work queue,
// whose worker threads run the function later in process context, with
// interrupts on, where it may sleep, take mutexes and talk to hardware
// that answers slowly.  A delayed work item (struct delayed_work) is
// queued only once a delay has passed, by a high-resolution timer.
//
// Guarantees (the conventional ones):
//
//   * PENDING: an item is either idle or pending.  Queueing a pending
//     item does nothing (queue_work returns false), so a burst of events
//     costs one run.  The bit is cleared just before the function runs,
//     so an event arriving while it runs queues it again and is not lost,
//     and the function may queue its own item again;
//   * an item never runs concurrently with itself, on any queue;
//   * cancel_work_sync / cancel_delayed_work_sync take the item off its
//     queue or timer, keep it from being queued again meanwhile, and wait
//     until a running instance has returned: afterwards the item is idle
//     and the caller may free it (if nothing else queues it again);
//   * flush_work waits for the instance pending or running when it was
//     called (not for later requeues -- an item that queues itself again
//     forever cannot be flushed into idleness, only cancelled);
//   * the worker does not touch an item after its function returned, so a
//     function may free the memory its item lives in.
//
// queue_work, queue_delayed_work, mod_delayed_work, cancel_work and
// cancel_delayed_work may be called from any context, interrupt handlers
// and timer callbacks included.  The _sync and flush functions sleep.
//
// Delays are given in milliseconds; msecs_to_jiffies() is the identity,
// for callers written against a tick-counting interface.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_WORKQUEUE_H
#define KERNEL_KE_WORKQUEUE_H

#include <kernel/uapi/types.h>
#include <kernel/ke/list.h>
#include <kernel/ke/hrtimer.h>

struct work_struct;
struct workqueue_struct;
typedef void (*work_func_t)(struct work_struct *work);

/* Item state bits (workqueue.c, under its lock). */
#define WORK_STATE_PENDING (1u << 0) /* queued, on its timer, or held */
#define WORK_STATE_QUEUED (1u << 1) /* on a queue's list */
#define WORK_STATE_TIMER (1u << 2) /* delayed: its timer is armed */
#define WORK_STATE_CANCELING (1u << 3) /* a _sync cancel holds PENDING */

#define WORK_INIT_MAGIC 0x574f524bu /* "WORK" */

struct work_struct {
	struct list_head entry; /* on wq->worklist while QUEUED */
	work_func_t func;
	struct workqueue_struct *wq; /* the queue it is (last) queued on */
	volatile uint32_t state; /* WORK_STATE_* */
	uint32_t nr_claimed; /* runs started (flush bookkeeping) */
	uint64_t qseq; /* insertion sequence on its queue (flushes) */
	uint32_t magic; /* WORK_INIT_MAGIC once initialised */
	uint32_t is_delayed; /* embedded in a struct delayed_work */
};

struct delayed_work {
	struct work_struct work;
	hrtimer_t timer;
	struct workqueue_struct *wq; /* the queue the timer queues on */
	uint32_t stale_fires; /* cancelled armings whose callback is due */
};

static inline struct delayed_work *to_delayed_work(struct work_struct *w)
{
	return container_of(w, struct delayed_work, work);
}

void __init_work(struct work_struct *work, work_func_t func, int delayed);
void __init_delayed_work(struct delayed_work *dwork, work_func_t func);
#define INIT_WORK(w, f) __init_work((w), (f), 0)
#define INIT_DELAYED_WORK(dw, f) __init_delayed_work((dw), (f))

/* A pending item (queued, waiting for its timer, or held by a cancel). */
static inline int work_pending(const struct work_struct *w)
{
	return (__atomic_load_n(&w->state, __ATOMIC_ACQUIRE) &
		WORK_STATE_PENDING) != 0;
}
static inline int delayed_work_pending(const struct delayed_work *dw)
{
	return work_pending(&dw->work);
}

/* Work queue flags. */
#define WQ_HIGHPRI (1u << 0) /* workers at the highest priority */
#define WQ_UNBOUND (1u << 1) /* accepted; every queue is unbound here */
#define WQ_FREEZABLE (1u << 2) /* accepted; there is no freezer */
#define WQ_MEM_RECLAIM (1u << 3) /* accepted; workers exist from creation */
#define WQ_ORDERED (1u << 4) /* one worker: items run one at a time, FIFO */
#define WQ_MAX_ACTIVE 16

/* A queue with `max_active' worker threads (0: the default, 4; clamped
 * to WQ_MAX_ACTIVE).  The name is copied.  NULL on -ENOMEM.  May sleep. */
struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
					 int max_active);
static inline struct workqueue_struct *
alloc_ordered_workqueue(const char *name, unsigned int flags)
{
	return alloc_workqueue(name, flags | WQ_ORDERED, 1);
}
/* Wait for every queued item to run, stop the workers, free the queue.
 * Queueing on it afterwards is a bug.  May sleep. */
void destroy_workqueue(struct workqueue_struct *wq);
/* Wait until everything queued on `wq' before the call has run. */
void flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);

/* The shared queues (valid once workqueue_init() ran). */
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_freezable_wq;

/* true when queued, false when it was pending already.  Any context. */
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay_ms);
/* Queue, or move the deadline of a pending item to `delay_ms' from now
 * (queued items go back to the timer).  true when it was pending. */
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
		      unsigned long delay_ms);
static inline bool schedule_work(struct work_struct *work)
{
	return queue_work(system_wq, work);
}
static inline bool schedule_delayed_work(struct delayed_work *dwork,
					 unsigned long delay_ms)
{
	return queue_delayed_work(system_wq, dwork, delay_ms);
}

/* Take a pending item off its queue / timer without waiting for a
 * running instance.  true when it was pending.  Any context. */
bool cancel_work(struct work_struct *work);
bool cancel_delayed_work(struct delayed_work *dwork);
/* The same, then wait until no instance runs; the item is idle on
 * return.  true when it was pending.  May sleep; never from the item's
 * own function. */
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
/* Wait for the pending/running instance.  true when there was one to
 * wait for.  flush_delayed_work queues a delayed item at once first.
 * May sleep; never from the item's own function. */
bool flush_work(struct work_struct *work);
bool flush_delayed_work(struct delayed_work *dwork);
/* Is the calling thread a worker running `work' right now. */
bool current_is_work(const struct work_struct *work);
/* Is the calling thread a work queue worker. */
bool current_is_workqueue_worker(void);
/* Is the calling thread a worker of `wq' (flushing `wq' from one would
 * wait for itself). */
bool current_is_worker_of(const struct workqueue_struct *wq);

static inline unsigned long msecs_to_jiffies(unsigned int ms)
{
	return ms;
}
static inline unsigned long usecs_to_jiffies(unsigned int us)
{
	return (us + 999u) / 1000u;
}

/* Create the shared queues.  Once, after sched_init(). */
void workqueue_init(void);

#endif /* KERNEL_KE_WORKQUEUE_H */
