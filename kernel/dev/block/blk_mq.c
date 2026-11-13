// LikeOS - the multi-queue block layer (blk-mq): tag sets, hardware and
// software contexts, freeze and quiesce, dispatch into the driver's
// hardware queues, request allocation and execution, completion, request
// timeouts, requeueing, polling and cancellation.  The contract is
// kernel/dev/block/blk_mq.h; the single-queue machinery it shares (the
// elevator, merging, plugging, the flush machinery, statistics, the
// requeue lists) lives in blk_queue.c and block.c.
//
// ---- The life of a disk request on a multi-queue queue ---------------------
//   blk_submit (block.c)  validate, split, the usage count (blk_mq_rq_enter:
//                         waits while the queue is frozen), the zone write
//                         plug, the flush sequence, a request slot, the
//                         task's plug -- then blk_mq_queue_insert():
//   insertion             the hctx of the submitting CPU and the request's
//                         type (poll / read / default) is chosen.  Pass-
//                         through, cache flushes, HEAD and PM requests go
//                         to the hctx's dispatch list; with mq-deadline the
//                         request goes into the queue's elevator (sorted,
//                         merged) and kblockd runs the hctx; without a
//                         scheduler it is issued to the driver right away
//                         when the hctx is idle, else queued on this CPU's
//                         software queue and the hctx is run.
//   dispatch              (blk_mq_run_hw_queue) the hctx's dispatch list
//                         first, then the elevator or the software queues;
//                         each request gets a DRIVER TAG of its hctx (its
//                         per-command data, the driver's command identity)
//                         and goes to ops->queue_rq with bd->last telling
//                         the driver whether more follow (ops->commit_rqs
//                         when the batch ended early).  Out of resources:
//                         back to the dispatch list, run again by the next
//                         completion (restart) or after a short delay.
//   the driver            blk_mq_start_request (the timeout clock), the
//                         hardware, blk_mq_complete_request from its
//                         interrupt: routed to the submitting CPU's block
//                         softirq or completed right there (several
//                         hardware queues: the interrupt is the submitter's
//                         already), ops->complete -> blk_mq_end_request.
//   the end               blk_request_complete (block.c): end_io, the
//                         waiter, statistics; the driver tag goes back when
//                         the last reference on the request's tag (a
//                         timeout scan or iterator may hold one) is gone,
//                         and the usage count drops last.
//
// ---- Locks (all leaves, irqsave) ---------------------------------------------
//   ctx->lock         a software queue's lists
//   hctx->lock        a hardware context's dispatch list
//   tags->lock        tags->rqs[] against the iterators, the restart list
//   q->lock           the elevator (blk_queue.c)
//   q->mq_freeze_spin freeze depth/state; q->mq_quiesce_lock quiesce depth
//   set->tag_list_lock (mutex, may sleep) the queues of a tag set
// None is held across a call into the driver (queue_rq, commit_rqs,
// queue_rqs, poll, timeout, complete, init/exit).  Freeze waits for the
// usage count, quiesce for the dispatchers inside each hctx (counted in
// hctx->dispatching before they look at the quiesced state, so a quiescer
// that set it first sees every one of them).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/timer.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* A driver out of a resource with nothing of the hctx's in flight is
 * asked again after this long (BLK_MQ_RESOURCE_DELAY, 3 ms). */
#define BLK_MQ_RESOURCE_DELAY_MS 3
/* A dispatcher that took a budget and found nothing (another took it). */
#define BLK_MQ_BUDGET_DELAY_MS 3
/* Async runs: this many on one CPU of the hctx before moving on. */
#define BLK_MQ_CPU_WORK_BATCH 8
/* A run that keeps finding work hands over to kblockd after this long. */
#define BLK_MQ_DISPATCH_MAX_NS (1000ULL * 1000ULL * 1000ULL)
/* Default request timeout of a tag set (ms). */
#define BLK_MQ_DEFAULT_TIMEOUT_MS 30000
/* The dispatch-busy average (as the single queue keeps it). */
#define BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT 8
#define BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR 4
/* nr_requests with mq-deadline: twice the depth, the depth counted up to
 * this (BLKDEV_DEFAULT_RQ). */
#define BLK_MQ_DEFAULT_RQ 128

/* ==========================================================================
 * Status codes
 * ========================================================================== */

/* The errno values of the C library's <errno.h> that the kernel's list
 * does not carry (only the block status mapping needs them). */
#ifndef EBADE
#define EBADE 52 /* invalid exchange: reservation conflict */
#endif
#ifndef EREMCHG
#define EREMCHG 78 /* remote address changed */
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS 109 /* too many references */
#endif

static const struct {
	int err;
	const char *name;
} blk_errors[BLK_STS_NR] = {
	[BLK_STS_OK] = { 0, "" },
	[BLK_STS_NOTSUPP] = { -EOPNOTSUPP, "operation not supported" },
	[BLK_STS_TIMEOUT] = { -ETIMEDOUT, "timeout" },
	[BLK_STS_NOSPC] = { -ENOSPC, "critical space allocation" },
	[BLK_STS_TRANSPORT] = { -ENOLINK, "recoverable transport" },
	[BLK_STS_TARGET] = { -EREMOTEIO, "critical target" },
	[BLK_STS_RESV_CONFLICT] = { -EBADE, "reservation conflict" },
	[BLK_STS_MEDIUM] = { -ENODATA, "critical medium" },
	[BLK_STS_PROTECTION] = { -EILSEQ, "protection" },
	[BLK_STS_RESOURCE] = { -ENOMEM, "kernel resource" },
	[BLK_STS_IOERR] = { -EIO, "I/O" },
	[BLK_STS_DM_REQUEUE] = { -EREMCHG, "dm internal retry" },
	[BLK_STS_AGAIN] = { -EAGAIN, "nonblocking retry" },
	[BLK_STS_DEV_RESOURCE] = { -EBUSY, "device resource" },
	[BLK_STS_ZONE_OPEN_RESOURCE] = { -ETOOMANYREFS, "open zones exceeded" },
	[BLK_STS_ZONE_ACTIVE_RESOURCE] = { -EOVERFLOW,
					   "active zones exceeded" },
	[BLK_STS_OFFLINE] = { -ENODEV, "device offline" },
	[BLK_STS_DURATION_LIMIT] = { -ETIME, "duration limit exceeded" },
	[18] = { -EIO, "I/O" },
	[BLK_STS_INVAL] = { -EINVAL, "invalid" },
};

int blk_status_to_errno(blk_status_t status)
{
	if ((unsigned int)status >= BLK_STS_NR) {
		WARN_RATELIMIT(1, "unknown block status %u", (unsigned)status);
		return -EIO;
	}
	return blk_errors[status].err;
}

blk_status_t errno_to_blk_status(int err)
{
	if (err == 0)
		return BLK_STS_OK;
	for (unsigned int i = 1; i < BLK_STS_NR; i++) {
		if (i == 18 || i == BLK_STS_DM_REQUEUE)
			continue;
		if (blk_errors[i].err == err)
			return (blk_status_t)i;
	}
	if (err == -ECANCELED)
		return BLK_STS_IOERR;
	return BLK_STS_IOERR;
}

const char *blk_status_to_str(blk_status_t status)
{
	if ((unsigned int)status >= BLK_STS_NR)
		return "<null>";
	return status == BLK_STS_OK ? "OK" : blk_errors[status].name;
}

const char *blk_op_str(int op)
{
	static const char *const names[BLK_OP_NR] = {
		[BLK_OP_READ] = "READ",
		[BLK_OP_WRITE] = "WRITE",
		[BLK_OP_FLUSH] = "FLUSH",
		[BLK_OP_DISCARD] = "DISCARD",
		[BLK_OP_SECURE_ERASE] = "SECURE_ERASE",
		[BLK_OP_WRITE_ZEROES] = "WRITE_ZEROES",
		[BLK_OP_DRV_IN] = "DRV_IN",
		[BLK_OP_DRV_OUT] = "DRV_OUT",
		[BLK_OP_ZONE_APPEND] = "ZONE_APPEND",
		[BLK_OP_ZONE_OPEN] = "ZONE_OPEN",
		[BLK_OP_ZONE_CLOSE] = "ZONE_CLOSE",
		[BLK_OP_ZONE_FINISH] = "ZONE_FINISH",
		[BLK_OP_ZONE_RESET] = "ZONE_RESET",
		[BLK_OP_ZONE_RESET_ALL] = "ZONE_RESET_ALL",
	};

	if (op < 0 || op >= BLK_OP_NR || !names[op])
		return "UNKNOWN";
	return names[op];
}

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

void blk_mq_msleep(uint32_t ms)
{
	if (blk_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0 &&
		       hrtimer_now_ns() < until)
			;
		return;
	}
	lapic_delay_ms(ms);
}

static inline uint32_t blk_mq_cpu(void)
{
	int cpu = blk_this_cpu();

	return (cpu < 0 || cpu >= MAX_CPUS) ? 0u : (uint32_t)cpu;
}

static inline struct blk_mq_tag_set *blk_mq_set(const struct blk_queue *q)
{
	return q->tag_set;
}

static inline int blk_mq_qflag(const struct blk_queue *q, uint64_t f)
{
	return (__atomic_load_n(&q->mq_flags, __ATOMIC_ACQUIRE) & f) != 0;
}

static inline void blk_mq_qflag_set(struct blk_queue *q, uint64_t f)
{
	(void)__atomic_fetch_or(&q->mq_flags, f, __ATOMIC_SEQ_CST);
}

static inline void blk_mq_qflag_clear(struct blk_queue *q, uint64_t f)
{
	(void)__atomic_fetch_and(&q->mq_flags, ~f, __ATOMIC_SEQ_CST);
}

static inline int blk_mq_hctx_state(const struct blk_mq_hw_ctx *hctx, int bit)
{
	return (__atomic_load_n(&hctx->state, __ATOMIC_ACQUIRE) &
		(1ul << bit)) != 0;
}

static inline int blk_mq_rqf_test(const struct blk_request *rq, uint32_t f)
{
	return (__atomic_load_n(&rq->mq_flags, __ATOMIC_ACQUIRE) & f) != 0;
}

static inline void blk_mq_rqf_set(struct blk_request *rq, uint32_t f)
{
	(void)__atomic_fetch_or(&rq->mq_flags, f, __ATOMIC_SEQ_CST);
}

static inline void blk_mq_rqf_clear(struct blk_request *rq, uint32_t f)
{
	(void)__atomic_fetch_and(&rq->mq_flags, ~f, __ATOMIC_SEQ_CST);
}

/* The PDU header of a tag. */
static inline struct blk_mq_pdu_hdr *blk_mq_hdr(struct blk_mq_tags *tags,
						unsigned int tag)
{
	return (struct blk_mq_pdu_hdr *)((uint8_t *)tags->pdu[tag] -
					 sizeof(struct blk_mq_pdu_hdr));
}

/* ==========================================================================
 * Queue state queries
 * ========================================================================== */

void *blk_queue_get_queuedata(const struct blk_queue *q)
{
	return q ? q->queuedata : NULL;
}

int blk_queue_dying(const struct blk_queue *q)
{
	if (!q)
		return 1;
	if (blk_queue_flags(q) & BLK_QUEUE_FL_DYING)
		return 1;
	if (blk_mq_qflag(q, BLK_QUEUE_MQ_DYING))
		return 1;
	return q->disk && (disk_flags(q->disk) & GENHD_FL_GONE);
}

int blk_queue_frozen(const struct blk_queue *q)
{
	return q && __atomic_load_n(&q->mq_frozen, __ATOMIC_ACQUIRE);
}

int blk_queue_quiesced(const struct blk_queue *q)
{
	return q && blk_mq_qflag(q, BLK_QUEUE_MQ_QUIESCED);
}

int blk_mq_hctx_stopped(const struct blk_mq_hw_ctx *hctx)
{
	/* A retired context never runs again, whoever starts it. */
	return (__atomic_load_n(&hctx->state, __ATOMIC_ACQUIRE) &
		((1ul << BLK_MQ_S_STOPPED) | (1ul << BLK_MQ_S_RETIRED))) != 0;
}

/* ==========================================================================
 * Walks of the hardware contexts outside the usage count
 *
 * Whoever indexes queue_hw_ctx[0..nr_hw_queues) without holding the usage
 * count -- runs of every hctx, stop/start, the quiesce wait, the timeout
 * work, mark-dead, sysfs, teardown's collection -- is a reader here.  An
 * update (blk_mq_update_nr_hw_queues on the frozen queue, or the final
 * teardown) sets mq_hctx_updating and waits until no reader is inside;
 * a reader that comes during an update backs off (a run is made again
 * when the update ends) or waits for its end.  Readers never sleep on
 * anything the updater holds while they are inside.
 * ========================================================================== */

static void blk_mq_hctx_read_end(struct blk_queue *q)
{
	if (__atomic_sub_fetch(&q->mq_hctx_readers, 1, __ATOMIC_SEQ_CST) == 0 &&
	    __atomic_load_n(&q->mq_hctx_updating, __ATOMIC_SEQ_CST))
		wake_up(&q->mq_freeze_wq);
}

/* 1: inside, the contexts stay until blk_mq_hctx_read_end.  0: an update
 * is under way (nothing to walk now). */
static int blk_mq_hctx_read_try(struct blk_queue *q)
{
	/* Counted before the look at the flag; the updater sets the flag
	 * before it looks at the count (both sequentially consistent). */
	(void)__atomic_add_fetch(&q->mq_hctx_readers, 1, __ATOMIC_SEQ_CST);
	if (likely(!__atomic_load_n(&q->mq_hctx_updating, __ATOMIC_SEQ_CST)))
		return 1;
	blk_mq_hctx_read_end(q);
	return 0;
}

/* Inside, waiting for an update to end first. */
static void blk_mq_hctx_read_wait(struct blk_queue *q)
{
	while (!blk_mq_hctx_read_try(q)) {
		if (blk_can_sleep())
			(void)wait_event_timeout(
				q->mq_freeze_wq,
				!__atomic_load_n(&q->mq_hctx_updating,
						 __ATOMIC_SEQ_CST),
				100);
		else
			lapic_delay_us(BLK_POLL_STEP_US);
	}
}

static void blk_mq_hctx_update_begin(struct blk_queue *q)
{
	WARN_ON_ONCE(__atomic_load_n(&q->mq_hctx_updating, __ATOMIC_RELAXED));
	__atomic_store_n(&q->mq_hctx_updating, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&q->mq_hctx_readers, __ATOMIC_SEQ_CST)) {
		if (blk_can_sleep())
			(void)wait_event_timeout(
				q->mq_freeze_wq,
				__atomic_load_n(&q->mq_hctx_readers,
						__ATOMIC_SEQ_CST) == 0,
				100);
		else
			lapic_delay_us(BLK_POLL_STEP_US);
	}
}

static void blk_mq_hctx_update_end(struct blk_queue *q)
{
	__atomic_store_n(&q->mq_hctx_updating, 0, __ATOMIC_SEQ_CST);
	wake_up(&q->mq_freeze_wq);
}

/* ==========================================================================
 * The usage count and freezing
 * ========================================================================== */

int64_t blk_mq_usage_sum(const struct blk_queue *q)
{
	int64_t sum = 0;

	if (!q || !q->mq_usage)
		return 0;
	/* Every count goes back to the slot it was taken in, so no slot is
	 * ever negative: a sum of 0 means every slot read 0, and a request
	 * alive across the whole scan is always seen (one that enters a slot
	 * already scanned found the queue frozen and leaves again). */
	for (uint32_t c = 0; c < MAX_CPUS; c++)
		sum += __atomic_load_n(&q->mq_usage[c].count, __ATOMIC_SEQ_CST);
	return sum;
}

/* The slot a new count goes to: the CPU it is taken on. */
static inline int blk_mq_usage_slot(void)
{
	return (int)blk_mq_cpu();
}

void blk_mq_usage_put(struct blk_queue *q, int slot)
{
	int64_t v;

	if (!q || !q->mq_usage)
		return;
	BUG_ON(slot < 0 || slot >= MAX_CPUS);
	v = __atomic_sub_fetch(&q->mq_usage[slot].count, 1, __ATOMIC_SEQ_CST);
	/* A count given back twice, or to another slot than it was taken
	 * in: the freeze and teardown waits would no longer be exact. */
	BUG_ON(v < 0);
	/* A freezer (or teardown) waits for the sum to reach zero: only the
	 * exit that drains the queue wakes it.  Every exit sees the sum after
	 * its own decrement (sequentially consistent), so the last of several
	 * concurrent exits always sees zero.  The submitters that wait on
	 * the same queue for the unfreeze are therefore not woken by every
	 * exit: each such wake could only find the queue still frozen. */
	if (__atomic_load_n(&q->mq_frozen, __ATOMIC_SEQ_CST) ||
	    blk_queue_dying(q)) {
		if (blk_mq_usage_sum(q) == 0)
			wake_up(&q->mq_freeze_wq);
		blk_queue_wake_waiters(q);
	}
}

/* One try in slot `slot': counted, and the queue was neither frozen nor
 * dying when it looked (after counting).  0, -EAGAIN (frozen), -ENODEV.
 * A frozen queue lets nobody in -- power-management requests neither
 * (suspend gates normal requests with BLK_QUEUE_FL_SUSPENDED, not with a
 * freeze), so a finished freeze wait really means nothing is inside. */
static int blk_mq_try_enter(struct blk_queue *q, int slot)
{
	/* Frozen or dying already: refused without touching the count, so a
	 * refused try is no exit that a freezer would have to look at. */
	if (blk_queue_dying(q))
		return -ENODEV;
	if (__atomic_load_n(&q->mq_frozen, __ATOMIC_SEQ_CST))
		return -EAGAIN;
	(void)__atomic_add_fetch(&q->mq_usage[slot].count, 1, __ATOMIC_SEQ_CST);
	if (blk_queue_dying(q)) {
		blk_mq_usage_put(q, slot);
		return -ENODEV;
	}
	if (__atomic_load_n(&q->mq_frozen, __ATOMIC_SEQ_CST)) {
		blk_mq_usage_put(q, slot);
		return -EAGAIN;
	}
	return 0;
}

/* Enter, waiting while frozen unless `nowait' or the caller cannot sleep.
 * On success *slot is where the count was taken. */
static int blk_mq_usage_get(struct blk_queue *q, int nowait, int *slot)
{
	int rc;

	for (;;) {
		int s = blk_mq_usage_slot();

		rc = blk_mq_try_enter(q, s);
		if (rc == 0) {
			*slot = s;
			return 0;
		}
		if (rc != -EAGAIN)
			return rc;
		/* Frozen: wait for the unfreeze, unless the submitter asked
		 * not to wait or cannot sleep (-EAGAIN, the established
		 * NOWAIT answer). */
		if (nowait || !blk_can_sleep())
			return -EAGAIN;
		wait_event(q->mq_freeze_wq,
			   !__atomic_load_n(&q->mq_frozen, __ATOMIC_SEQ_CST) ||
				   blk_queue_dying(q));
	}
}

/* The exported pair counts in slot 0: the caller may leave on another
 * CPU than it entered on, and has nowhere to keep a slot. */
int blk_mq_queue_enter(struct blk_queue *q, int nowait)
{
	int rc;

	if (!q || !q->mq_usage)
		return -ENODEV;
	for (;;) {
		rc = blk_mq_try_enter(q, 0);
		if (rc != -EAGAIN)
			return rc;
		if (nowait || !blk_can_sleep())
			return -EAGAIN;
		wait_event(q->mq_freeze_wq,
			   !__atomic_load_n(&q->mq_frozen, __ATOMIC_SEQ_CST) ||
				   blk_queue_dying(q));
	}
}

void blk_mq_queue_exit(struct blk_queue *q)
{
	blk_mq_usage_put(q, 0);
}

int blk_mq_rq_enter(struct blk_queue *q, struct blk_request *rq)
{
	int slot, rc;

	if (rq->bl.internal & BLK_RQI_Q_USAGE)
		return 0;
	if (!q->mq_usage)
		return -ENODEV;
	rc = blk_mq_usage_get(q, (rq->flags & BLK_REQ_NOWAIT) != 0, &slot);
	/* I/O to a disk whose device is gone (blk_mark_disk_dead) fails with
	 * -EIO, also when it waited for the queue (NOWAIT included); a
	 * pass-through command keeps the queue's -ENODEV. */
	if (rc == -ENODEV && !blk_rq_is_passthrough(rq) && q->disk &&
	    (disk_flags(q->disk) & GENHD_FL_GONE))
		rc = -EIO;
	if (rc)
		return rc;
	rq->bl.usage_slot = slot;
	rq->bl.internal |= BLK_RQI_Q_USAGE;
	return 0;
}

void blk_mq_rq_exit(struct blk_queue *q, struct blk_request *rq)
{
	if (!(rq->bl.internal & BLK_RQI_Q_USAGE))
		return;
	rq->bl.internal &= ~BLK_RQI_Q_USAGE;
	blk_mq_usage_put(q, rq->bl.usage_slot);
}

void blk_freeze_queue_start(struct blk_queue *q)
{
	uint64_t fl;
	int first;

	if (!q || !q->mq_usage)
		return;
	spin_lock_irqsave(&q->mq_freeze_spin, &fl);
	first = q->mq_freeze_depth++ == 0;
	if (first)
		__atomic_store_n(&q->mq_frozen, 1, __ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&q->mq_freeze_spin, fl);
	/* What is queued has to reach the driver for the count to drain. */
	if (first)
		blk_mq_run_hw_queues(q, !blk_can_sleep());
}

void blk_mq_freeze_queue_wait(struct blk_queue *q)
{
	if (!q || !q->mq_usage)
		return;
	might_sleep();
	wait_event(q->mq_freeze_wq, blk_mq_usage_sum(q) == 0);
}

int blk_mq_freeze_queue_wait_timeout(struct blk_queue *q,
				     unsigned long timeout_ms)
{
	if (!q || !q->mq_usage)
		return 1;
	might_sleep();
	return wait_event_timeout(q->mq_freeze_wq, blk_mq_usage_sum(q) == 0,
				  (long)timeout_ms) > 0;
}

void blk_mq_freeze_queue(struct blk_queue *q)
{
	blk_freeze_queue_start(q);
	blk_mq_freeze_queue_wait(q);
}

void blk_mq_unfreeze_queue(struct blk_queue *q)
{
	uint64_t fl;
	int last = 0;

	if (!q || !q->mq_usage)
		return;
	spin_lock_irqsave(&q->mq_freeze_spin, &fl);
	if (WARN_ON_ONCE(q->mq_freeze_depth <= 0)) {
		spin_unlock_irqrestore(&q->mq_freeze_spin, fl);
		return;
	}
	if (--q->mq_freeze_depth == 0) {
		__atomic_store_n(&q->mq_frozen, 0, __ATOMIC_SEQ_CST);
		last = 1;
	}
	spin_unlock_irqrestore(&q->mq_freeze_spin, fl);
	if (last)
		wake_up(&q->mq_freeze_wq);
}

/* ==========================================================================
 * Quiesce
 * ========================================================================== */

void blk_mq_quiesce_queue_nowait(struct blk_queue *q)
{
	uint64_t fl;

	if (!q || !blk_queue_is_mq(q))
		return;
	spin_lock_irqsave(&q->mq_quiesce_lock, &fl);
	if (q->quiesce_depth++ == 0)
		blk_mq_qflag_set(q, BLK_QUEUE_MQ_QUIESCED);
	spin_unlock_irqrestore(&q->mq_quiesce_lock, fl);
}

static int blk_mq_queue_dispatchers(struct blk_queue *q)
{
	int n = 0;

	/* During an update: counted as busy, the waiter looks again. */
	if (!blk_mq_hctx_read_try(q))
		return 1;
	for (unsigned int i = 0; i < q->nr_hw_queues; i++)
		n += __atomic_load_n(&q->queue_hw_ctx[i]->dispatching,
				     __ATOMIC_SEQ_CST);
	blk_mq_hctx_read_end(q);
	return n;
}

void blk_mq_wait_quiesce_done_queue(struct blk_queue *q)
{
	if (!q || !blk_queue_is_mq(q))
		return;
	blk_might_sleep();
	/* Pairs with the dispatcher's count before its look at QUIESCED. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!blk_can_sleep()) {
		while (blk_mq_queue_dispatchers(q))
			lapic_delay_us(BLK_POLL_STEP_US);
		return;
	}
	while (wait_event_timeout(q->mq_quiesce_wq,
				  blk_mq_queue_dispatchers(q) == 0, 1000) == 0)
		;
}

void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set)
{
	if (!set)
		return;
	blk_might_sleep();
	mutex_lock(&set->tag_list_lock);
	for (struct blk_queue *q = set->tag_list; q; q = q->set_next)
		blk_mq_wait_quiesce_done_queue(q);
	mutex_unlock(&set->tag_list_lock);
}

void blk_mq_quiesce_queue(struct blk_queue *q)
{
	blk_mq_quiesce_queue_nowait(q);
	blk_mq_wait_quiesce_done_queue(q);
}

void blk_mq_unquiesce_queue(struct blk_queue *q)
{
	uint64_t fl;
	int run = 0;

	if (!q || !blk_queue_is_mq(q))
		return;
	spin_lock_irqsave(&q->mq_quiesce_lock, &fl);
	if (WARN_ON_ONCE(q->quiesce_depth <= 0)) {
		spin_unlock_irqrestore(&q->mq_quiesce_lock, fl);
		return;
	}
	if (--q->quiesce_depth == 0) {
		blk_mq_qflag_clear(q, BLK_QUEUE_MQ_QUIESCED);
		run = 1;
	}
	spin_unlock_irqrestore(&q->mq_quiesce_lock, fl);
	/* Dispatch requests inserted while the queue was quiesced. */
	if (run)
		blk_mq_run_hw_queues(q, 1);
}

void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set)
{
	if (!set)
		return;
	might_sleep();
	mutex_lock(&set->tag_list_lock);
	for (struct blk_queue *q = set->tag_list; q; q = q->set_next)
		if (!(q->limits.features & BLK_FEAT_SKIP_TAGSET_QUIESCE))
			blk_mq_quiesce_queue_nowait(q);
	mutex_unlock(&set->tag_list_lock);
	blk_mq_wait_quiesce_done(set);
}

void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set)
{
	if (!set)
		return;
	might_sleep();
	mutex_lock(&set->tag_list_lock);
	for (struct blk_queue *q = set->tag_list; q; q = q->set_next)
		if (!(q->limits.features & BLK_FEAT_SKIP_TAGSET_QUIESCE))
			blk_mq_unquiesce_queue(q);
	mutex_unlock(&set->tag_list_lock);
}

/* ==========================================================================
 * Mapping: CPU -> hardware context
 * ========================================================================== */

static enum hctx_type blk_mq_type_of(const struct blk_queue *q,
				     enum blk_op op, uint32_t rq_flags)
{
	const struct blk_mq_tag_set *set = q->tag_set;
	enum hctx_type t = HCTX_TYPE_DEFAULT;

	if (rq_flags & BLK_REQ_POLLED)
		t = HCTX_TYPE_POLL;
	else if (op == BLK_OP_READ)
		t = HCTX_TYPE_READ;
	if (t != HCTX_TYPE_DEFAULT &&
	    (t >= set->nr_maps || !set->map[t].nr_queues))
		t = HCTX_TYPE_DEFAULT;
	return t;
}

static struct blk_mq_hw_ctx *blk_mq_map_queue_type(struct blk_queue *q,
						   enum hctx_type type,
						   unsigned int cpu)
{
	if (cpu >= MAX_CPUS)
		cpu = 0;
	if (q->queue_ctx && q->queue_ctx[cpu].hctxs[type])
		return q->queue_ctx[cpu].hctxs[type];
	BUG_ON(q->nr_hw_queues == 0);
	return q->queue_hw_ctx[q->tag_set->map[type].mq_map[cpu] %
			       q->nr_hw_queues];
}

struct blk_mq_hw_ctx *blk_mq_map_queue(struct blk_queue *q, uint32_t rq_flags,
				       unsigned int cpu)
{
	if (!q || !blk_queue_is_mq(q) || !q->nr_hw_queues)
		return NULL;
	return blk_mq_map_queue_type(q, blk_mq_type_of(q, BLK_OP_WRITE,
						       rq_flags),
				     cpu);
}

static struct blk_mq_hw_ctx *blk_mq_rq_map(struct blk_queue *q,
					   struct blk_request *rq,
					   unsigned int cpu)
{
	return blk_mq_map_queue_type(q, blk_mq_type_of(q, rq->op, rq->flags),
				     cpu);
}

/* Fill each hctx's CPU mask and software queue list from the set's maps
 * (blk_mq_map_swqueue). */
static void blk_mq_map_swqueue(struct blk_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;
	cpumask_t online;

	cpumask_online(&online);
	if (cpumask_empty(&online))
		cpumask_set_cpu(0, &online);
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

		cpumask_clear(&h->cpumask);
		h->nr_ctx = 0;
		h->dispatch_from = 0;
		h->type = HCTX_TYPE_DEFAULT;
		for (unsigned int w = 0; w < (MAX_CPUS + 63) / 64; w++)
			h->ctx_map[w] = 0;
	}
	for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
		struct blk_mq_ctx *ctx = &q->queue_ctx[cpu];
		int is_online = cpumask_test_cpu(cpu, &online);

		for (unsigned int j = 0; j < HCTX_MAX_TYPES; j++) {
			struct blk_mq_hw_ctx *h;
			unsigned int idx;

			if (j >= set->nr_maps || !set->map[j].nr_queues) {
				/* Unused type: the default one serves it. */
				idx = set->map[HCTX_TYPE_DEFAULT].mq_map[cpu];
				ctx->hctxs[j] =
					q->queue_hw_ctx[idx % q->nr_hw_queues];
				continue;
			}
			idx = set->map[j].mq_map[cpu];
			if (WARN_ON_ONCE(idx >= q->nr_hw_queues))
				idx = 0;
			h = q->queue_hw_ctx[idx];
			ctx->hctxs[j] = h;
			if (!is_online || cpumask_test_cpu(cpu, &h->cpumask))
				continue;
			cpumask_set_cpu(cpu, &h->cpumask);
			h->type = (enum hctx_type)j;
			BUG_ON(h->nr_ctx >= MAX_CPUS);
			ctx->index_hw[j] = (unsigned short)h->nr_ctx;
			h->ctxs[h->nr_ctx++] = ctx;
		}
	}
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

		h->next_cpu = cpumask_first(&h->cpumask);
		h->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}
}

/* ==========================================================================
 * Hardware contexts
 * ========================================================================== */

static void blk_mq_run_work_fn(void *arg);

static struct blk_mq_hw_ctx *blk_mq_alloc_hctx(struct blk_queue *q,
					       struct blk_mq_tag_set *set,
					       unsigned int idx)
{
	struct blk_mq_hw_ctx *h;

	h = (struct blk_mq_hw_ctx *)kcalloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->ctxs = (struct blk_mq_ctx **)kcalloc(MAX_CPUS, sizeof(void *));
	h->run_work = (struct blk_work *)kcalloc(1, sizeof(struct blk_work));
	if (!h->ctxs || !h->run_work) {
		if (h->ctxs)
			kfree(h->ctxs);
		if (h->run_work)
			kfree(h->run_work);
		kfree(h);
		return NULL;
	}
	spinlock_init(&h->lock, "blk_mq_hctx");
	blk_work_init(h->run_work, blk_mq_run_work_fn, h);
	h->queue = q;
	h->queue_num = idx;
	h->flags = set->flags;
	h->numa_node = -1;
	h->tags = (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) ? set->shared_tags :
							   set->tags[idx];
	h->dispatch_head = h->dispatch_tail = NULL;
	if (q->disk)
		ksnprintf(h->sysfs_base, sizeof(h->sysfs_base), "block/%s/mq/%u",
			  q->disk->disk_name, idx);
	else
		ksnprintf(h->sysfs_base, sizeof(h->sysfs_base), "mq/%u", idx);
	if (set->ops->init_hctx &&
	    set->ops->init_hctx(h, set->driver_data, idx)) {
		kfree(h->run_work);
		kfree(h->ctxs);
		kfree(h);
		return NULL;
	}
	return h;
}

static void blk_mq_free_hctx(struct blk_queue *q, struct blk_mq_hw_ctx *h)
{
	struct blk_mq_tag_set *set = q->tag_set;
	int retired;

	if (!h)
		return;
	retired = blk_mq_hctx_state(h, BLK_MQ_S_RETIRED);
	/* A retired context left the restart list and its tags when it was
	 * retired. */
	if (!retired)
		blk_mq_tag_wait_cancel_sync(h);
	if (blk_work_cancel_sync(h->run_work, 1000)) {
		WARN(1, BLK_LOG_PREFIX "%s: hctx run work does not finish; leaking it",
		     h->sysfs_base);
		return;
	}
	WARN_ON(h->dispatch_head != NULL);
	/* exit_hctx exactly once: a retired context had it at retirement. */
	if (!retired) {
		if (set && set->ops && set->ops->exit_hctx)
			set->ops->exit_hctx(h, h->queue_num);
		if (h->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
			blk_mq_tag_idle(h);
	}
	kfree(h->run_work);
	kfree(h->ctxs);
	kfree(h);
}

/* Teardown: every context goes.  Readers are waited for first; once the
 * count is 0 they find nothing to walk. */
static void blk_mq_free_hctxs(struct blk_queue *q)
{
	struct blk_mq_hw_ctx **hw;
	unsigned int n;

	blk_mq_hctx_update_begin(q);
	hw = q->queue_hw_ctx;
	n = q->nr_hw_queues;
	__atomic_store_n(&q->nr_hw_queues, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&q->queue_hw_ctx, NULL, __ATOMIC_SEQ_CST);
	q->mq_hctx_cap = 0;
	if (hw) {
		for (unsigned int i = 0; i < n; i++)
			blk_mq_free_hctx(q, hw[i]);
		kfree(hw);
	}
	while (q->mq_dead_hctx) {
		struct blk_mq_hw_ctx *h = q->mq_dead_hctx;

		q->mq_dead_hctx = h->dead_next;
		blk_mq_free_hctx(q, h);
	}
	blk_mq_hctx_update_end(q);
}

/* A context a shrink no longer needs (frozen queue, readers out): stopped
 * for good, taken off its tags' restart list, its run work waited for,
 * exited (ops->exit_hctx, once), and parked until the queue's memory goes
 * -- the tags of its index are freed by the caller, so it forgets them. */
static void blk_mq_retire_hctx(struct blk_queue *q, struct blk_mq_hw_ctx *h)
{
	struct blk_mq_tag_set *set = q->tag_set;

	(void)__atomic_fetch_or(&h->state,
				(1ul << BLK_MQ_S_STOPPED) |
					(1ul << BLK_MQ_S_INACTIVE) |
					(1ul << BLK_MQ_S_RETIRED),
				__ATOMIC_SEQ_CST);
	blk_mq_tag_wait_cancel_sync(h);
	if (blk_work_cancel_sync(h->run_work, 1000))
		WARN(1, BLK_LOG_PREFIX "%s: run work of a retired hctx does not finish",
		     h->sysfs_base);
	WARN_ON(h->dispatch_head != NULL);
	if (h->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		blk_mq_tag_idle(h);
	if (set->ops->exit_hctx)
		set->ops->exit_hctx(h, h->queue_num);
	h->nr_ctx = 0;
	cpumask_clear(&h->cpumask);
	h->tags = NULL;
	h->dead_next = q->mq_dead_hctx;
	q->mq_dead_hctx = h;
}

/* The queue follows a new hardware queue count of its set (frozen, the
 * readers out): contexts of indexes that remain are kept, new ones are
 * made, surplus ones are retired.  The array only grows (a shrink keeps
 * it).  0, or -ENOMEM with the queue exactly as it was (only a growth can
 * fail). */
static int blk_mq_realloc_hctxs(struct blk_queue *q, struct blk_mq_tag_set *set)
{
	unsigned int old = q->nr_hw_queues, nr = set->nr_hw_queues;
	struct blk_mq_hw_ctx **hw = q->queue_hw_ctx, **nhw = NULL;

	BUG_ON(!__atomic_load_n(&q->mq_hctx_updating, __ATOMIC_RELAXED));
	if (nr > q->mq_hctx_cap) {
		nhw = (struct blk_mq_hw_ctx **)kcalloc(nr, sizeof(void *));
		if (!nhw)
			return -ENOMEM;
		for (unsigned int i = 0; i < old; i++)
			nhw[i] = hw[i];
		hw = nhw;
	}
	for (unsigned int i = old; i < nr; i++) {
		hw[i] = blk_mq_alloc_hctx(q, set, i);
		if (!hw[i]) {
			for (unsigned int j = old; j < i; j++) {
				blk_mq_free_hctx(q, hw[j]);
				hw[j] = NULL;
			}
			if (nhw)
				kfree(nhw);
			return -ENOMEM;
		}
	}
	for (unsigned int i = nr; i < old; i++) {
		if (hw[i])
			blk_mq_retire_hctx(q, hw[i]);
		hw[i] = NULL;
	}
	for (unsigned int i = 0; i < nr && i < old; i++)
		hw[i]->tags = (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) ?
				      set->shared_tags :
				      set->tags[i];
	if (nhw) {
		struct blk_mq_hw_ctx **prev = q->queue_hw_ctx;

		__atomic_store_n(&q->queue_hw_ctx, nhw, __ATOMIC_RELEASE);
		q->mq_hctx_cap = nr;
		if (prev)
			kfree(prev);
	}
	__atomic_store_n(&q->nr_hw_queues, nr, __ATOMIC_RELEASE);
	return 0;
}

static int blk_mq_alloc_hctxs(struct blk_queue *q, struct blk_mq_tag_set *set)
{
	struct blk_mq_hw_ctx **hw;

	hw = (struct blk_mq_hw_ctx **)kcalloc(set->nr_hw_queues, sizeof(void *));
	if (!hw)
		return -ENOMEM;
	for (unsigned int i = 0; i < set->nr_hw_queues; i++) {
		hw[i] = blk_mq_alloc_hctx(q, set, i);
		if (!hw[i]) {
			for (unsigned int j = 0; j < i; j++)
				blk_mq_free_hctx(q, hw[j]);
			kfree(hw);
			return -ENOMEM;
		}
		if (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
			hw[i]->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
	}
	q->queue_hw_ctx = hw;
	q->mq_hctx_cap = set->nr_hw_queues;
	__atomic_store_n(&q->nr_hw_queues, set->nr_hw_queues, __ATOMIC_SEQ_CST);
	return 0;
}

/* Several queues share the tags of the set: every hctx of every queue
 * gets BLK_MQ_F_TAG_QUEUE_SHARED (fair share).  tag_list_lock held; the
 * queues are frozen around the change (no request counted under the old
 * mode may end under the new one). */
static void blk_mq_update_tag_set_shared(struct blk_mq_tag_set *set,
					 int shared, struct blk_queue *skip)
{
	lockdep_assert_held(&set->tag_list_lock);
	for (struct blk_queue *q = set->tag_list; q; q = q->set_next) {
		if (q == skip)
			continue;
		blk_mq_freeze_queue(q);
		for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
			struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

			if (shared) {
				h->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
			} else {
				blk_mq_tag_idle(h);
				h->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;
			}
		}
		blk_mq_unfreeze_queue(q);
	}
}

static void blk_mq_add_queue_tag_set(struct blk_mq_tag_set *set,
				     struct blk_queue *q)
{
	mutex_lock(&set->tag_list_lock);
	if (set->tag_list && !(set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)) {
		set->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
		blk_mq_update_tag_set_shared(set, 1, NULL);
	}
	if (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
		for (unsigned int i = 0; i < q->nr_hw_queues; i++)
			q->queue_hw_ctx[i]->flags |= BLK_MQ_F_TAG_QUEUE_SHARED;
	q->set_next = set->tag_list;
	set->tag_list = q;
	set->nr_queues++;
	mutex_unlock(&set->tag_list_lock);
}

static void blk_mq_del_queue_tag_set(struct blk_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;
	struct blk_queue **pp;

	mutex_lock(&set->tag_list_lock);
	for (pp = &set->tag_list; *pp && *pp != q; pp = &(*pp)->set_next)
		;
	if (*pp) {
		*pp = q->set_next;
		set->nr_queues--;
	}
	q->set_next = NULL;
	/* One queue left: it has the tags to itself again. */
	if (set->tag_list && !set->tag_list->set_next &&
	    (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)) {
		set->flags &= ~BLK_MQ_F_TAG_QUEUE_SHARED;
		blk_mq_update_tag_set_shared(set, 0, NULL);
	}
	mutex_unlock(&set->tag_list_lock);
}

uint32_t blk_mq_default_nr_requests(const struct blk_queue *q)
{
	const struct blk_mq_tag_set *set = q->tag_set;
	uint32_t depth = set->queue_depth - set->reserved_tags;
	uint32_t n;

	if (q->sched != BLK_SCHED_NONE) {
		n = depth < BLK_MQ_DEFAULT_RQ ? depth : BLK_MQ_DEFAULT_RQ;
		return 2 * n;
	}
	/* Without a scheduler the driver tags are the limit; the queue's
	 * slots cover every hardware queue's tags (see DEVIATIONS: one
	 * admission count per queue instead of a wait per hctx). */
	n = depth * (q->nr_hw_queues ? q->nr_hw_queues : 1);
	return n ? n : 1;
}

int blk_mq_init_queue(struct blk_queue *q, struct blk_mq_tag_set *set)
{
	int rc;

	BUG_ON(q == NULL || set == NULL);
	if (!set->tags || !set->nr_hw_queues)
		return -EINVAL;
	q->queue_ctx = (struct blk_mq_ctx *)kcalloc(
		1, sizeof(struct blk_mq_ctx) * MAX_CPUS + 64);
	q->queue_ctx_raw = q->queue_ctx;
	if (q->queue_ctx)
		q->queue_ctx = (struct blk_mq_ctx *)(((uintptr_t)q->queue_ctx +
						      63) &
						     ~(uintptr_t)63);
	q->mq_usage = (struct blk_mq_usage_cpu *)kcalloc(
		1, sizeof(struct blk_mq_usage_cpu) * MAX_CPUS + 64);
	q->mq_usage_raw = q->mq_usage;
	if (q->mq_usage)
		q->mq_usage = (struct blk_mq_usage_cpu *)(((uintptr_t)q->mq_usage +
							   63) &
							  ~(uintptr_t)63);
	if (!q->queue_ctx || !q->mq_usage) {
		rc = -ENOMEM;
		goto fail;
	}
	for (unsigned int c = 0; c < MAX_CPUS; c++) {
		struct blk_mq_ctx *ctx = &q->queue_ctx[c];

		spinlock_init(&ctx->lock, "blk_mq_ctx");
		ctx->cpu = c;
		ctx->queue = q;
	}
	spinlock_init(&q->mq_freeze_spin, "blk_mq_freeze");
	spinlock_init(&q->mq_quiesce_lock, "blk_mq_quiesce");
	spinlock_init(&q->mq_requeue_lock, "blk_mq_requeue");
	mutex_init(&q->mq_freeze_lock, "blk_mq_freeze_lock");
	wq_head_init(&q->mq_freeze_wq, "blk_mq_freeze_wq");
	wq_head_init(&q->mq_quiesce_wq, "blk_mq_quiesce_wq");
	refcount_set(&q->q_refs, 1);
	q->tag_set = set;
	rc = blk_mq_alloc_hctxs(q, set);
	if (rc)
		goto fail;
	blk_mq_map_swqueue(q);
	blk_mq_add_queue_tag_set(set, q);

	/* The queue's view of the device: depth per hardware queue, poll
	 * queues, classic polling unless the driver asks for a delay. */
	q->limits.queue_depth = set->queue_depth;
	if (set->nr_maps > HCTX_TYPE_POLL && set->map[HCTX_TYPE_POLL].nr_queues)
		q->limits.features |= BLK_FEAT_POLL;
	if (!q->limits.io_poll_delay)
		q->limits.io_poll_delay = -1;
	/* The default scheduler: none with several hardware queues, shared
	 * tags or when the driver asks for it; mq-deadline otherwise. */
	if (set->nr_hw_queues != 1 || (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) ||
	    (set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT) || !q->disk)
		q->sched = BLK_SCHED_NONE;
	else
		q->sched = BLK_SCHED_DEADLINE;
	q->depth = set->queue_depth > BLK_QUEUE_MAX_TAGS ? BLK_QUEUE_MAX_TAGS :
							   set->queue_depth;
	q->nr_requests = blk_mq_default_nr_requests(q);
	q->async_depth = q->nr_requests;
	q->mq_nr_requests_hw = set->queue_depth;
	blk_mq_qflag_set(q, BLK_QUEUE_MQ_REGISTERED);
	return 0;

fail:
	if (q->queue_ctx_raw)
		kfree(q->queue_ctx_raw);
	if (q->mq_usage_raw)
		kfree(q->mq_usage_raw);
	q->queue_ctx = q->queue_ctx_raw = NULL;
	q->mq_usage = q->mq_usage_raw = NULL;
	q->tag_set = NULL;
	return rc;
}

/* Hardware contexts go, the queue leaves its tag set (after teardown
 * drained it: the set may be freed by the driver afterwards). */
void blk_mq_exit_queue(struct blk_queue *q)
{
	if (!blk_mq_qflag(q, BLK_QUEUE_MQ_REGISTERED))
		return;
	blk_mq_qflag_clear(q, BLK_QUEUE_MQ_REGISTERED);
	blk_mq_del_queue_tag_set(q);
	blk_mq_free_hctxs(q);
}

void blk_mq_release_queue(struct blk_queue *q)
{
	if (!q)
		return;
	blk_mq_exit_queue(q);
	blk_zone_free_plugs(q);
	if (q->queue_ctx_raw)
		kfree(q->queue_ctx_raw);
	if (q->mq_usage_raw)
		kfree(q->mq_usage_raw);
	q->queue_ctx = q->queue_ctx_raw = NULL;
	q->mq_usage = q->mq_usage_raw = NULL;
}

/* ==========================================================================
 * Queues
 * ========================================================================== */

void blk_get_queue(struct blk_queue *q)
{
	if (q)
		refcount_inc(&q->q_refs);
}

void blk_put_queue(struct blk_queue *q)
{
	if (q && refcount_dec_and_test(&q->q_refs))
		blk_queue_free_now(q);
}

struct blk_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set,
				     const struct blk_limits *lim,
				     void *queuedata)
{
	struct blk_queue *q;
	struct blk_limits l;

	if (!set || !set->tags)
		return NULL;
	might_sleep();
	if (lim)
		l = *lim;
	else
		mm_memset(&l, 0, sizeof(l));
	if (blk_validate_limits(&l))
		return NULL;
	q = blk_queue_create(NULL, &l);
	if (!q)
		return NULL;
	q->queuedata = queuedata;
	if (blk_mq_init_queue(q, set)) {
		blk_queue_free_now(q);
		return NULL;
	}
	return q;
}

void blk_mq_destroy_queue(struct blk_queue *q)
{
	if (!q)
		return;
	might_sleep();
	/* Teardown leaves the tag set (blk_mq_exit_queue) once drained. */
	blk_queue_destroy(q);
	blk_put_queue(q);
}

struct gendisk *blk_mq_alloc_disk(struct blk_mq_tag_set *set,
				  const struct blk_limits *lim,
				  const struct blk_ops *ops, const char *name,
				  void *queuedata)
{
	struct gendisk *disk;
	struct blk_queue *q;
	struct blk_limits l;

	if (!set || !set->tags || !ops || !name)
		return NULL;
	might_sleep();
	disk = disk_alloc_named(BLK_DISK_NVME, ops, queuedata, name);
	if (!disk)
		return NULL;
	if (lim)
		l = *lim;
	else
		l = disk->limits;
	if (blk_validate_limits(&l)) {
		disk_del(disk);
		return NULL;
	}
	q = blk_queue_create(disk, &l);
	if (!q) {
		disk_del(disk);
		return NULL;
	}
	q->queuedata = queuedata;
	if (blk_mq_init_queue(q, set)) {
		blk_queue_free_now(q);
		disk_del(disk);
		return NULL;
	}
	disk->tag_set = set;
	disk->queuedata = queuedata;
	disk->queue = q;
	disk->limits = q->limits;
	return disk;
}

void blk_mark_disk_dead(struct gendisk *disk)
{
	struct blk_queue *q;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return;
	disk_set_flags(disk, GENHD_FL_GONE);
	q = disk->queue;
	if (!q)
		return;
	/* Everybody waiting to enter, or for a tag, looks again and finds
	 * the disk gone. */
	if (q->mq_usage)
		wake_up(&q->mq_freeze_wq);
	/* (During a resize nobody waits for a tag: the queue is frozen and
	 * drained, every tag waiter holds the usage count.) */
	if (blk_queue_is_mq(q) && blk_mq_hctx_read_try(q)) {
		for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
			struct blk_mq_tags *t = q->queue_hw_ctx[i]->tags;

			blk_sbq_wake_all(&t->bitmap_tags);
			blk_sbq_wake_all(&t->breserved_tags);
		}
		blk_mq_hctx_read_end(q);
	}
	blk_queue_wake_waiters(q);
	/* What is queued already is dispatched: the driver fails it (its
	 * not-ready path) instead of it waiting for the teardown. */
	if (blk_queue_is_mq(q))
		blk_mq_run_hw_queues(q, 1);
}

/* ==========================================================================
 * Tag sets
 * ========================================================================== */

static void blk_mq_clear_mq_map(struct blk_mq_queue_map *qmap)
{
	for (unsigned int c = 0; c < MAX_CPUS; c++)
		qmap->mq_map[c] = 0;
}

static void blk_mq_update_queue_map(struct blk_mq_tag_set *set)
{
	if (set->nr_maps == 1)
		set->map[HCTX_TYPE_DEFAULT].nr_queues = set->nr_hw_queues;
	if (set->ops->map_queues) {
		for (unsigned int i = 0; i < set->nr_maps; i++)
			blk_mq_clear_mq_map(&set->map[i]);
		set->ops->map_queues(set);
	} else {
		BUG_ON(set->nr_maps > 1);
		blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);
	}
	/* Whatever the driver did, every entry must name an existing hctx. */
	for (unsigned int i = 0; i < set->nr_maps; i++)
		for (unsigned int c = 0; c < MAX_CPUS; c++)
			if (WARN_ON_ONCE(set->map[i].mq_map[c] >=
					 set->nr_hw_queues))
				set->map[i].mq_map[c] = 0;
}

static void blk_mq_free_map_and_rqs(struct blk_mq_tag_set *set,
				    struct blk_mq_tags *tags)
{
	if (!tags)
		return;
	blk_mq_free_rqs(set, tags);
	blk_mq_free_tags(tags);
}

/* The tags of hardware queues from..to-1 (the shared map only with the
 * whole set). */
static void blk_mq_free_set_tags_range(struct blk_mq_tag_set *set,
				       unsigned int from, unsigned int to)
{
	if (!set->tags)
		return;
	if (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) {
		if (from == 0) {
			blk_mq_free_map_and_rqs(set, set->shared_tags);
			set->shared_tags = NULL;
		}
		for (unsigned int i = from; i < to; i++)
			set->tags[i] = NULL;
		return;
	}
	for (unsigned int i = from; i < to; i++) {
		blk_mq_free_map_and_rqs(set, set->tags[i]);
		set->tags[i] = NULL;
	}
}

static void blk_mq_free_set_tags(struct blk_mq_tag_set *set, unsigned int from)
{
	blk_mq_free_set_tags_range(set, from, set->nr_hw_queues);
}

/* Tags for hardware queues from..nr_hw_queues-1 at the set's depth.
 * -ENOMEM leaves nothing new allocated. */
static int blk_mq_alloc_set_tags(struct blk_mq_tag_set *set, unsigned int from)
{
	if (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) {
		if (!set->shared_tags) {
			set->shared_tags = blk_mq_alloc_map_and_rqs(
				set, BLK_MQ_NO_HCTX_IDX, set->queue_depth);
			if (!set->shared_tags)
				return -ENOMEM;
		}
		for (unsigned int i = from; i < set->nr_hw_queues; i++)
			set->tags[i] = set->shared_tags;
		return 0;
	}
	for (unsigned int i = from; i < set->nr_hw_queues; i++) {
		set->tags[i] = blk_mq_alloc_map_and_rqs(set, i, set->queue_depth);
		if (!set->tags[i]) {
			for (unsigned int j = from; j < i; j++) {
				blk_mq_free_map_and_rqs(set, set->tags[j]);
				set->tags[j] = NULL;
			}
			return -ENOMEM;
		}
	}
	return 0;
}

int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
{
	unsigned int ncpu = cpumask_nr_online();
	unsigned int want_depth;
	int rc;

	if (!set || !set->ops || !set->ops->queue_rq || !set->ops->complete)
		return -EINVAL;
	might_sleep();
	if (!set->nr_hw_queues)
		return -EINVAL;
	if (!set->queue_depth)
		return -EINVAL;
	if (set->queue_depth < set->reserved_tags + 1)
		return -EINVAL;
	if (set->reserved_tags > BLK_MQ_MAX_DEPTH)
		return -EINVAL;
	if (set->flags & ~(BLK_MQ_F_MAX - 1))
		return -EINVAL;
	if (!set->nr_maps)
		set->nr_maps = 1;
	else if (set->nr_maps > HCTX_MAX_TYPES)
		return -EINVAL;
	/* A poll map needs the driver's poll operation. */
	if (WARN_ON_ONCE(set->nr_maps > HCTX_TYPE_POLL && !set->ops->poll))
		return -EINVAL;
	if (set->queue_depth > BLK_MQ_MAX_DEPTH) {
		kprintf(BLK_LOG_PREFIX "tag set: reduced tag depth to %u\n",
			BLK_MQ_MAX_DEPTH);
		set->queue_depth = BLK_MQ_MAX_DEPTH;
	}
	/* More hardware queues than CPUs buy nothing with one map. */
	if (set->nr_maps == 1 && set->nr_hw_queues > ncpu)
		set->nr_hw_queues = ncpu;
	if (!set->timeout_ms)
		set->timeout_ms = BLK_MQ_DEFAULT_TIMEOUT_MS;
	mutex_init(&set->tag_list_lock, "blk_mq_tag_list");
	set->tag_list = NULL;
	set->nr_queues = 0;
	set->shared_tags = NULL;
	set->tags = (struct blk_mq_tags **)kcalloc(set->nr_hw_queues,
						   sizeof(void *));
	if (!set->tags)
		return -ENOMEM;
	for (unsigned int i = 0; i < set->nr_maps; i++) {
		set->map[i].mq_map = (unsigned int *)kcalloc(MAX_CPUS,
							     sizeof(unsigned int));
		if (!set->map[i].mq_map) {
			rc = -ENOMEM;
			goto fail_maps;
		}
		if (set->nr_maps == 1)
			set->map[i].nr_queues = set->nr_hw_queues;
	}
	blk_mq_update_queue_map(set);

	/* Tags and PDUs, at half the depth again and again while memory
	 * is short (down to reserved + 1), as the established allocator
	 * does. */
	want_depth = set->queue_depth;
	for (;;) {
		rc = blk_mq_alloc_set_tags(set, 0);
		if (!rc)
			break;
		set->queue_depth >>= 1;
		if (set->queue_depth < set->reserved_tags + 1) {
			rc = -ENOMEM;
			goto fail_maps;
		}
	}
	if (set->queue_depth != want_depth)
		kprintf(BLK_LOG_PREFIX "tag set: reduced tag depth (%u -> %u)\n",
			want_depth, set->queue_depth);
	return 0;

fail_maps:
	for (unsigned int i = 0; i < set->nr_maps; i++) {
		if (set->map[i].mq_map)
			kfree(set->map[i].mq_map);
		set->map[i].mq_map = NULL;
	}
	kfree(set->tags);
	set->tags = NULL;
	return rc;
}

int blk_mq_alloc_sq_tag_set(struct blk_mq_tag_set *set,
			    const struct blk_mq_ops *ops,
			    unsigned int queue_depth, unsigned int set_flags)
{
	if (!set)
		return -EINVAL;
	mm_memset(set, 0, sizeof(*set));
	set->ops = ops;
	set->nr_hw_queues = 1;
	set->nr_maps = 1;
	set->queue_depth = queue_depth;
	set->numa_node = -1;
	set->flags = set_flags;
	return blk_mq_alloc_tag_set(set);
}

void blk_mq_free_tag_set(struct blk_mq_tag_set *set)
{
	if (!set || !set->tags)
		return;
	might_sleep();
	WARN(set->tag_list != NULL,
	     BLK_LOG_PREFIX "tag set freed while %u queue(s) still use it",
	     set->nr_queues);
	blk_mq_free_set_tags(set, 0);
	for (unsigned int i = 0; i < set->nr_maps; i++) {
		if (set->map[i].mq_map)
			kfree(set->map[i].mq_map);
		set->map[i].mq_map = NULL;
	}
	kfree(set->tags);
	set->tags = NULL;
}

void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues)
{
	unsigned int ncpu = cpumask_nr_online();
	unsigned int prev, nr;
	struct blk_queue *q;

	if (!set || !set->tags || nr_hw_queues < 1)
		return;
	might_sleep();
	nr = (unsigned int)nr_hw_queues;
	if (set->nr_maps == 1 && nr > ncpu)
		nr = ncpu;
	mutex_lock(&set->tag_list_lock);
	prev = set->nr_hw_queues;
	if (set->nr_maps == 1 && nr == prev) {
		mutex_unlock(&set->tag_list_lock);
		return;
	}
	/* Nothing may be in flight while the contexts change, and nobody may
	 * walk them: frozen first (the usage count drains), then the walkers
	 * outside the usage count are waited for. */
	for (q = set->tag_list; q; q = q->set_next)
		blk_mq_freeze_queue(q);
	for (q = set->tag_list; q; q = q->set_next)
		blk_mq_hctx_update_begin(q);

	if (nr > prev) {
		struct blk_mq_tags **ntags =
			(struct blk_mq_tags **)kcalloc(nr, sizeof(void *));

		if (ntags) {
			for (unsigned int i = 0; i < prev; i++)
				ntags[i] = set->tags[i];
			kfree(set->tags);
			set->tags = ntags;
			set->nr_hw_queues = nr;
			if (blk_mq_alloc_set_tags(set, prev)) {
				kprintf(BLK_LOG_PREFIX "tag set: no memory for %u hardware queues, keeping %u\n",
					nr, prev);
				set->nr_hw_queues = prev;
			}
		} else {
			kprintf(BLK_LOG_PREFIX "tag set: no memory for %u hardware queues, keeping %u\n",
				nr, prev);
		}
	} else {
		set->nr_hw_queues = nr;
	}

fallback:
	blk_mq_update_queue_map(set);
	for (q = set->tag_list; q; q = q->set_next) {
		if (blk_mq_realloc_hctxs(q, set)) {
			unsigned int want = set->nr_hw_queues;

			/* Only a growth allocates: the whole set goes back to
			 * the previous count (queues that grew already shrink
			 * again on the second pass) and the new tags go. */
			BUG_ON(want <= prev);
			kprintf(BLK_LOG_PREFIX "%s: no memory for %u hardware queues; staying at %u\n",
				q->disk ? q->disk->disk_name : "queue", want,
				prev);
			set->nr_hw_queues = prev;
			blk_mq_free_set_tags_range(set, prev, want);
			goto fallback;
		}
		if (set->flags & BLK_MQ_F_TAG_QUEUE_SHARED)
			for (unsigned int i = 0; i < q->nr_hw_queues; i++)
				q->queue_hw_ctx[i]->flags |=
					BLK_MQ_F_TAG_QUEUE_SHARED;
		blk_mq_map_swqueue(q);
		q->nr_requests = blk_mq_default_nr_requests(q);
		q->async_depth = q->nr_requests;
	}
	/* Tags of indexes no queue uses any more (their contexts retired and
	 * forgot them). */
	if (set->nr_hw_queues < prev)
		blk_mq_free_set_tags_range(set, set->nr_hw_queues, prev);

	for (q = set->tag_list; q; q = q->set_next)
		blk_mq_hctx_update_end(q);
	for (q = set->tag_list; q; q = q->set_next) {
		blk_mq_unfreeze_queue(q);
		/* Runs that backed off during the update. */
		blk_mq_run_hw_queues(q, 1);
	}
	mutex_unlock(&set->tag_list_lock);
}

/* ==========================================================================
 * Driver tags of requests
 * ========================================================================== */

static void blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx);

/* `rq' takes tag `tag' of its hctx: the PDU header and the tag table
 * name it, the tag reference is the base one. */
static void blk_mq_rq_set_tag(struct blk_request *rq, unsigned int tag)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct blk_mq_tags *tags = hctx->tags;

	BUG_ON(tag >= tags->nr_tags);
	VM_BUG_ON(__atomic_load_n(&tags->rqs[tag], __ATOMIC_RELAXED) != NULL &&
		  __atomic_load_n(&tags->rqs[tag], __ATOMIC_RELAXED) != rq);
	rq->tag = (int)tag;
	/* A new life at the driver (a flush step or a retry after a deferred
	 * end takes a new tag): not ended. */
	blk_mq_rqf_clear(rq, BLK_MQ_RQF_ENDED);
	refcount_set(&rq->mq_ref, 1);
	blk_mq_hdr(tags, tag)->rq = rq;
	if (hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED) {
		(void)__atomic_add_fetch(&hctx->nr_active, 1, __ATOMIC_RELAXED);
		blk_mq_rqf_set(rq, BLK_MQ_RQF_ACTIVE);
	}
	__atomic_store_n(&tags->rqs[tag], rq, __ATOMIC_RELEASE);
}

/* A driver tag for `rq' at dispatch (fair share, never waits).  1 when it
 * holds one. */
static int blk_mq_get_driver_tag(struct blk_request *rq)
{
	int tag;

	if (rq->tag >= 0)
		return 1;
	tag = blk_mq_get_tag(rq->mq_hctx, 0, 1, 1);
	if (tag < 0)
		return 0;
	blk_mq_rq_set_tag(rq, (unsigned int)tag);
	return 1;
}

/* The tag goes back: the table forgets the request under tags->lock (no
 * iterator can take it any more), then the bit is freed, and an hctx
 * whose dispatch waits for a tag is run. */
static void blk_mq_rq_detach_tag(struct blk_request *rq)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct blk_mq_tags *tags;
	unsigned int tag;
	uint64_t fl;

	if (rq->tag < 0)
		return;
	BUG_ON(hctx == NULL);
	tags = hctx->tags;
	tag = (unsigned int)rq->tag;
	BUG_ON(tag >= tags->nr_tags);
	spin_lock_irqsave(&tags->lock, &fl);
	if (__atomic_load_n(&tags->rqs[tag], __ATOMIC_RELAXED) == rq)
		__atomic_store_n(&tags->rqs[tag], NULL, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&tags->lock, fl);
	blk_mq_hdr(tags, tag)->rq = NULL;
	rq->tag = -1;
	if (blk_mq_rqf_test(rq, BLK_MQ_RQF_ACTIVE)) {
		blk_mq_rqf_clear(rq, BLK_MQ_RQF_ACTIVE);
		(void)__atomic_sub_fetch(&hctx->nr_active, 1, __ATOMIC_RELAXED);
	}
	blk_mq_put_tag(tags, tag);
	blk_mq_sched_restart(hctx);
}

/* The last tag reference of a request went. */
static void blk_mq_rq_released(struct blk_request *rq)
{
	struct blk_queue *q = rq->queue;

	if (rq->flags & BLK_REQ_MQ_ALLOC) {
		/* blk_mq_free_request's: tag, usage, memory. */
		blk_mq_rq_detach_tag(rq);
		if (q)
			blk_mq_rq_exit(q, rq);
		blk_request_free(rq);
		return;
	}
	/* The end that blk_request_complete deferred: the tag first (a flush
	 * step or a retry takes a new one), then the request ends.  It has
	 * been IDLE since the driver ended it (blk_mq_end_hook). */
	blk_mq_rq_detach_tag(rq);
	WARN_ON_ONCE(!blk_mq_rqf_test(rq, BLK_MQ_RQF_ENDED));
	blk_mq_rqf_clear(rq, BLK_MQ_RQF_ENDED);
	__atomic_store_n(&rq->mq_state, MQ_RQ_IDLE, __ATOMIC_RELEASE);
	blk_request_complete(rq, rq->drv_status, rq->drv_resid);
}

void blk_mq_put_rq_ref(struct blk_request *rq)
{
	if (refcount_dec_and_test(&rq->mq_ref))
		blk_mq_rq_released(rq);
}

/* Has the driver ended this tagged request already (its end deferred)? */
static inline int blk_mq_rq_ended(const struct blk_request *rq)
{
	return rq->tag >= 0 && !(rq->flags & BLK_REQ_MQ_ALLOC) &&
	       blk_mq_rqf_test(rq, BLK_MQ_RQF_ENDED);
}

/* Claim the one end of a tagged request: whoever sets the bit owns it.
 * A second end (a dispatcher's error end racing with a cancel iterator's
 * completion) loses here and is dropped -- its reference is not the base
 * one, and nothing of the end may run twice. */
static int blk_mq_end_claim(struct blk_request *rq)
{
	if (__atomic_fetch_or(&rq->mq_flags, BLK_MQ_RQF_ENDED,
			      __ATOMIC_SEQ_CST) &
	    BLK_MQ_RQF_ENDED) {
		WARN_RATELIMIT(1, "request %s sector %llu ended twice",
			       blk_op_str(rq->op),
			       (unsigned long long)rq->sector);
		return 0;
	}
	return 1;
}

/* The end of a tagged request whose ENDED bit the caller claimed. */
static void blk_mq_end_claimed(struct blk_request *rq, int status,
			       uint32_t resid)
{
	rq->drv_status = status;
	rq->drv_resid = resid;
	/* IDLE now, as the established free path makes it before it drops
	 * its base count: a timeout scan or an iterator that still holds one
	 * sees an idle request (never counted as completed-and-pending, never
	 * completed again); only the tag and the memory wait for the last
	 * reference. */
	__atomic_store_n(&rq->mq_state, MQ_RQ_IDLE, __ATOMIC_RELEASE);
	blk_mq_rqf_clear(rq, BLK_MQ_RQF_TIMED_OUT);
	blk_mq_put_rq_ref(rq);
}

int blk_mq_end_hook(struct blk_request *rq, int status, uint32_t resid)
{
	if (rq->tag < 0 || (rq->flags & BLK_REQ_MQ_ALLOC))
		return 0;
	if (blk_mq_end_claim(rq))
		blk_mq_end_claimed(rq, status, resid);
	return 1;
}

/* ==========================================================================
 * Running hardware queues
 * ========================================================================== */

static void blk_mq_run_dispatch(struct blk_mq_hw_ctx *hctx);

/* The CPU whose kblockd makes an asynchronous run: the CPUs of the hctx
 * in turn, BLK_MQ_CPU_WORK_BATCH runs each. */
static uint32_t blk_mq_hctx_next_cpu(struct blk_mq_hw_ctx *hctx)
{
	unsigned int cpu = hctx->next_cpu;

	if (cpumask_empty(&hctx->cpumask))
		return blk_mq_cpu();
	if (--hctx->next_cpu_batch <= 0 || cpu >= MAX_CPUS ||
	    !cpumask_test_cpu(cpu, &hctx->cpumask)) {
		unsigned int next = cpumask_next((int)cpu, &hctx->cpumask);

		if (next >= MAX_CPUS)
			next = cpumask_first(&hctx->cpumask);
		hctx->next_cpu = next;
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
		cpu = next;
	}
	return cpu < MAX_CPUS ? cpu : 0;
}

/* Is there anything for this hctx? */
static int blk_mq_hctx_has_pending(struct blk_mq_hw_ctx *hctx)
{
	struct blk_queue *q = hctx->queue;

	if (__atomic_load_n(&hctx->nr_dispatch, __ATOMIC_ACQUIRE))
		return 1;
	for (unsigned int w = 0; w < (MAX_CPUS + 63) / 64; w++)
		if (__atomic_load_n(&hctx->ctx_map[w], __ATOMIC_ACQUIRE))
			return 1;
	return blk_queue_elv_pending(q);
}

/* May the hctx be run right here?  In a context that may sleep on one of
 * its CPUs, before kblockd, or in a poller with interrupts off. */
static int blk_mq_run_here(struct blk_mq_hw_ctx *hctx)
{
	if (sched_current() == NULL || !blk_kblockd_running())
		return 1;
	if (blk_in_hardirq() || blk_in_softirq())
		return 0;
	if (!irqs_enabled())
		return !(hctx->flags & BLK_MQ_F_BLOCKING);
	if (!blk_can_sleep())
		return 0;
	return cpumask_empty(&hctx->cpumask) ||
	       cpumask_test_cpu(blk_mq_cpu(), &hctx->cpumask);
}

void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs)
{
	if (!hctx || blk_mq_hctx_stopped(hctx))
		return;
	if (!blk_kblockd_running() && msecs)
		return; /* the polled waiter asks again (blk_wait) */
	(void)kblockd_mod_delayed_work_on(blk_mq_hctx_next_cpu(hctx),
					  hctx->run_work,
					  (uint64_t)msecs * 1000000ULL);
}

void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, int async)
{
	struct blk_queue *q;

	if (!hctx)
		return;
	q = hctx->queue;
	if (blk_queue_quiesced(q) || blk_mq_hctx_stopped(hctx))
		return;
	if (!blk_mq_hctx_has_pending(hctx)) {
		/* Looked without a lock: once more behind a full barrier
		 * against an insertion that raced with it. */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!blk_mq_hctx_has_pending(hctx))
			return;
	}
	if (async || !blk_mq_run_here(hctx)) {
		blk_mq_delay_run_hw_queue(hctx, 0);
		return;
	}
	blk_mq_run_dispatch(hctx);
}

void blk_mq_run_hw_queues(struct blk_queue *q, int async)
{
	if (!q || !blk_queue_is_mq(q))
		return;
	/* During a resize: the update runs every hctx when it ends. */
	if (!blk_mq_hctx_read_try(q))
		return;
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

		if (blk_mq_hctx_stopped(h))
			continue;
		blk_mq_run_hw_queue(h, async);
	}
	blk_mq_hctx_read_end(q);
}

void blk_mq_run_queue(struct blk_queue *q, int async)
{
	blk_mq_run_hw_queues(q, async);
}

static void blk_mq_delay_run_hw_queues(struct blk_queue *q, unsigned long ms)
{
	if (!blk_mq_hctx_read_try(q))
		return;
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

		if (!blk_mq_hctx_stopped(h) && blk_mq_hctx_has_pending(h))
			blk_mq_delay_run_hw_queue(h, ms);
	}
	blk_mq_hctx_read_end(q);
}

static void blk_mq_run_work_fn(void *arg)
{
	struct blk_mq_hw_ctx *hctx = (struct blk_mq_hw_ctx *)arg;

	blk_mq_run_dispatch(hctx);
}

void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	if (!hctx)
		return;
	/* A pending run finds the bit and returns (a driver may stop the
	 * queue from inside its own queue_rq, i.e. from that run). */
	(void)__atomic_fetch_or(&hctx->state, 1ul << BLK_MQ_S_STOPPED,
				__ATOMIC_SEQ_CST);
}

void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	if (!hctx)
		return;
	(void)__atomic_fetch_and(&hctx->state, ~(1ul << BLK_MQ_S_STOPPED),
				 __ATOMIC_SEQ_CST);
	blk_mq_run_hw_queue(hctx, 0);
}

void blk_mq_stop_hw_queues(struct blk_queue *q)
{
	if (!q || !blk_queue_is_mq(q))
		return;
	blk_mq_hctx_read_wait(q);
	for (unsigned int i = 0; i < q->nr_hw_queues; i++)
		blk_mq_stop_hw_queue(q->queue_hw_ctx[i]);
	blk_mq_hctx_read_end(q);
}

void blk_mq_start_hw_queues(struct blk_queue *q)
{
	if (!q || !blk_queue_is_mq(q))
		return;
	blk_mq_hctx_read_wait(q);
	for (unsigned int i = 0; i < q->nr_hw_queues; i++)
		blk_mq_start_hw_queue(q->queue_hw_ctx[i]);
	blk_mq_hctx_read_end(q);
}

void blk_mq_start_stopped_hw_queues(struct blk_queue *q, int async)
{
	if (!q || !blk_queue_is_mq(q))
		return;
	blk_mq_hctx_read_wait(q);
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];

		if (!blk_mq_hctx_state(h, BLK_MQ_S_STOPPED))
			continue;
		(void)__atomic_fetch_and(&h->state, ~(1ul << BLK_MQ_S_STOPPED),
					 __ATOMIC_SEQ_CST);
		blk_mq_run_hw_queue(h, async);
	}
	blk_mq_hctx_read_end(q);
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

/* SCHED_RESTART: dispatch stopped with requests left; the next tag freed
 * on this hctx runs it again. */
static void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (blk_mq_hctx_state(hctx, BLK_MQ_S_SCHED_RESTART))
		return;
	(void)__atomic_fetch_or(&hctx->state, 1ul << BLK_MQ_S_SCHED_RESTART,
				__ATOMIC_SEQ_CST);
}

static void blk_mq_sched_restart(struct blk_mq_hw_ctx *hctx)
{
	if (!blk_mq_hctx_state(hctx, BLK_MQ_S_SCHED_RESTART))
		return;
	(void)__atomic_fetch_and(&hctx->state, ~(1ul << BLK_MQ_S_SCHED_RESTART),
				 __ATOMIC_SEQ_CST);
	/* Ordered against the dispatcher putting requests back: it looks
	 * at the bit after the put-back (blk_mq_dispatch_rq_list). */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	blk_mq_run_hw_queue(hctx, 1);
}

static void blk_mq_update_dispatch_busy(struct blk_mq_hw_ctx *hctx, int busy)
{
	uint32_t ewma = __atomic_load_n(&hctx->dispatch_busy, __ATOMIC_RELAXED);

	if (!ewma && !busy)
		return;
	ewma *= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT - 1;
	if (busy)
		ewma += 1u << BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR;
	ewma /= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT;
	__atomic_store_n(&hctx->dispatch_busy, ewma, __ATOMIC_RELAXED);
}

/* The hctx dispatch list (hctx->lock). */
static void blk_mq_dispatch_add_locked(struct blk_mq_hw_ctx *hctx,
				       struct blk_request *rq, int at_head)
{
	lockdep_assert_held(&hctx->lock);
	rq->disp_next = NULL;
	if (at_head) {
		rq->disp_next = hctx->dispatch_head;
		hctx->dispatch_head = rq;
		if (!hctx->dispatch_tail)
			hctx->dispatch_tail = rq;
	} else {
		if (hctx->dispatch_tail)
			hctx->dispatch_tail->disp_next = rq;
		else
			hctx->dispatch_head = rq;
		hctx->dispatch_tail = rq;
	}
	rq->bl.internal |= BLK_RQI_MQ_DISPATCH;
	__atomic_store_n(&hctx->nr_dispatch, hctx->nr_dispatch + 1,
			 __ATOMIC_RELEASE);
}

/* Put a list (through disp_next, in order) on the dispatch list: at its
 * tail, or its head. */
static void blk_mq_dispatch_splice(struct blk_mq_hw_ctx *hctx,
				   struct blk_request *list, int at_head)
{
	uint64_t fl;

	spin_lock_irqsave(&hctx->lock, &fl);
	if (at_head) {
		struct blk_request *rev = NULL;

		/* Reverse so that adding each at the head keeps the order. */
		while (list) {
			struct blk_request *n = list->disp_next;

			list->disp_next = rev;
			rev = list;
			list = n;
		}
		while (rev) {
			struct blk_request *n = rev->disp_next;

			blk_mq_dispatch_add_locked(hctx, rev, 1);
			rev = n;
		}
	} else {
		while (list) {
			struct blk_request *n = list->disp_next;

			blk_mq_dispatch_add_locked(hctx, list, 0);
			list = n;
		}
	}
	spin_unlock_irqrestore(&hctx->lock, fl);
}

/* Take the dispatch list: all of it, or, while the queue is suspended,
 * the power-management requests only. */
static struct blk_request *blk_mq_take_dispatch(struct blk_mq_hw_ctx *hctx)
{
	struct blk_request *head = NULL, **tailp = &head;
	int suspended = (blk_queue_flags(hctx->queue) &
			 BLK_QUEUE_FL_SUSPENDED) != 0;
	uint64_t fl;

	if (!__atomic_load_n(&hctx->nr_dispatch, __ATOMIC_ACQUIRE))
		return NULL;
	spin_lock_irqsave(&hctx->lock, &fl);
	{
		struct blk_request *prev = NULL, *rq = hctx->dispatch_head;
		uint32_t taken = 0;

		while (rq) {
			struct blk_request *next = rq->disp_next;

			if (!suspended || (rq->flags & BLK_REQ_PM)) {
				if (prev)
					prev->disp_next = next;
				else
					hctx->dispatch_head = next;
				if (hctx->dispatch_tail == rq)
					hctx->dispatch_tail = prev;
				rq->disp_next = NULL;
				rq->bl.internal &= ~BLK_RQI_MQ_DISPATCH;
				*tailp = rq;
				tailp = &rq->disp_next;
				taken++;
			} else {
				prev = rq;
			}
			rq = next;
		}
		__atomic_store_n(&hctx->nr_dispatch, hctx->nr_dispatch - taken,
				 __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&hctx->lock, fl);
	return head;
}

/* Hand one request to the driver.  The request holds its driver tag. */
static blk_status_t blk_mq_issue(struct blk_mq_hw_ctx *hctx,
				 struct blk_request *rq, int last)
{
	const struct blk_mq_ops *ops = hctx->queue->tag_set->ops;
	struct blk_mq_queue_data bd;
	blk_status_t ret;

	VM_BUG_ON(rq->magic != BLK_REQUEST_MAGIC);
	BUG_ON(rq->tag < 0 || rq->mq_hctx != hctx);
	rq->state = BLK_RQ_DISPATCHED;
	rq->dispatch_ns = hrtimer_now_ns();
	bd.rq = rq;
	bd.last = last;
	ret = ops->queue_rq(hctx, &bd);
	hctx->nr_queue_rq++;
	if (ret == BLK_STS_RESOURCE || ret == BLK_STS_DEV_RESOURCE) {
		/* Not taken: back to queued, the start (if the driver made
		 * it) undone. */
		rq->state = BLK_RQ_QUEUED;
		if (__atomic_load_n(&rq->mq_state, __ATOMIC_ACQUIRE) !=
		    MQ_RQ_IDLE) {
			__atomic_store_n(&rq->mq_state, MQ_RQ_IDLE,
					 __ATOMIC_RELEASE);
			blk_mq_rqf_clear(rq, BLK_MQ_RQF_TIMED_OUT);
		}
	}
	return ret;
}

static void blk_mq_commit_rqs(struct blk_mq_hw_ctx *hctx, int queued)
{
	const struct blk_mq_ops *ops = hctx->queue->tag_set->ops;

	if (ops->commit_rqs && queued) {
		ops->commit_rqs(hctx);
		hctx->nr_commit++;
	}
}

/* A dispatch found no tag of a shared map: wait on the tags' restart
 * list (a non-shared map is restarted through SCHED_RESTART). */
static int blk_mq_mark_tag_wait(struct blk_mq_hw_ctx *hctx,
				struct blk_request *rq)
{
	if (!(hctx->flags & (BLK_MQ_F_TAG_QUEUE_SHARED |
			     BLK_MQ_F_TAG_HCTX_SHARED))) {
		blk_mq_sched_mark_restart_hctx(hctx);
		/* A tag may have come free between the try and the mark. */
		return blk_mq_get_driver_tag(rq);
	}
	(void)blk_mq_tag_wait_restart(hctx);
	/* ...and once more after joining the list (the free that would
	 * have woken us may have been before it). */
	if (blk_mq_get_driver_tag(rq)) {
		blk_mq_tag_wait_cancel(hctx);
		return 1;
	}
	return 0;
}

/* Hand the requests on *list (all of one hctx, through disp_next) to the
 * driver in order.  Returns 1 when something was dispatched (or failed
 * by the driver), 0 when nothing went: what is left is back on the
 * hctx's dispatch list and a rerun is arranged. */
static int blk_mq_dispatch_rq_list(struct blk_mq_hw_ctx *hctx,
				   struct blk_request **list)
{
	int queued = 0, errors = 0, needs_resource = 0, no_tag = 0;
	blk_status_t ret = BLK_STS_OK;

	if (!*list)
		return 0;
	do {
		struct blk_request *rq = *list;

		WARN_ON_ONCE(rq->mq_hctx != hctx);
		if (!blk_mq_get_driver_tag(rq) &&
		    !blk_mq_mark_tag_wait(hctx, rq)) {
			no_tag = 1;
			break;
		}
		*list = rq->disp_next;
		rq->disp_next = NULL;
		ret = blk_mq_issue(hctx, rq, *list == NULL);
		switch (ret) {
		case BLK_STS_OK:
			queued++;
			break;
		case BLK_STS_RESOURCE:
			needs_resource = 1;
			__attribute__((fallthrough));
		case BLK_STS_DEV_RESOURCE:
			rq->disp_next = *list;
			*list = rq;
			hctx->nr_busy++;
			goto out;
		default:
			errors++;
			blk_mq_end_request(rq, ret);
			break;
		}
	} while (*list);
out:
	/* The driver was told more would follow, which was not true. */
	if (*list || ret != BLK_STS_OK)
		blk_mq_commit_rqs(hctx, queued);
	if (*list) {
		struct blk_request *left = *list;
		int needs_restart;

		*list = NULL;
		blk_mq_dispatch_splice(hctx, left, 0);
		/* Back on the list before the restart bit is read: pairs with
		 * blk_mq_sched_restart. */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		needs_restart = blk_mq_hctx_state(hctx, BLK_MQ_S_SCHED_RESTART);
		if (!needs_restart ||
		    (no_tag && !__atomic_load_n(&hctx->restart_queued,
						__ATOMIC_ACQUIRE)))
			blk_mq_run_hw_queue(hctx, 1);
		else if (needs_resource)
			blk_mq_delay_run_hw_queue(hctx, BLK_MQ_RESOURCE_DELAY_MS);
		blk_mq_update_dispatch_busy(hctx, 1);
		return 0;
	}
	blk_mq_update_dispatch_busy(hctx, 0);
	return (queued + errors) != 0;
}

/* Dispatch a list whose requests may belong to several hctxs (the
 * elevator's choice): each hctx's run of it in turn. */
static int blk_mq_dispatch_multi(struct blk_request *list)
{
	int dispatched = 0;

	while (list) {
		struct blk_mq_hw_ctx *h = list->mq_hctx;
		struct blk_request *mine = NULL, **mt = &mine;
		struct blk_request *rest = NULL, **rt = &rest;

		while (list) {
			struct blk_request *n = list->disp_next;

			list->disp_next = NULL;
			if (list->mq_hctx == h) {
				*mt = list;
				mt = &list->disp_next;
			} else {
				*rt = list;
				rt = &list->disp_next;
			}
			list = n;
		}
		if (blk_queue_quiesced(h->queue) || blk_mq_hctx_stopped(h)) {
			/* That hctx may not dispatch now: its own run
			 * takes them later. */
			blk_mq_dispatch_splice(h, mine, 0);
		} else {
			(void)__atomic_add_fetch(&h->dispatching, 1,
						 __ATOMIC_SEQ_CST);
			if (blk_queue_quiesced(h->queue)) {
				blk_mq_dispatch_splice(h, mine, 0);
			} else {
				dispatched |= blk_mq_dispatch_rq_list(h, &mine);
			}
			(void)__atomic_sub_fetch(&h->dispatching, 1,
						 __ATOMIC_SEQ_CST);
			if (blk_queue_quiesced(h->queue))
				wake_up(&h->queue->mq_quiesce_wq);
		}
		list = rest;
	}
	return dispatched;
}

/* One round from the elevator (mq-deadline): a request at a time while
 * the driver is busy, else up to nr_requests; the driver tag is taken as
 * the request leaves the elevator (no point taking more than the device
 * can hold).  -EAGAIN: requests appeared on the dispatch list (they go
 * first); 1: something went and more may; 0 otherwise. */
static int __blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	struct blk_queue *q = hctx->queue;
	struct blk_request *list = NULL, **tailp = &list;
	uint32_t max_dispatch, count = 0;
	int busy = 0, multi = 0, run_queue = 0, dispatched = 0;

	max_dispatch = __atomic_load_n(&hctx->dispatch_busy, __ATOMIC_RELAXED) ?
			       1 :
			       q->nr_requests;
	do {
		struct blk_request *rq;

		if (!blk_queue_elv_pending(q))
			break;
		if (__atomic_load_n(&hctx->nr_dispatch, __ATOMIC_ACQUIRE)) {
			busy = 1;
			break;
		}
		rq = blk_queue_elv_take(q);
		if (!rq) {
			run_queue = 1;
			break;
		}
		*tailp = rq;
		tailp = &rq->disp_next;
		rq->disp_next = NULL;
		count++;
		if (rq->mq_hctx != hctx)
			multi = 1;
		/* No tag: stop taking from the elevator -- the device is
		 * full, and the requests merge and sort better there. */
		if (!blk_mq_get_driver_tag(rq))
			break;
	} while (count < max_dispatch);

	if (!count) {
		if (run_queue)
			blk_mq_delay_run_hw_queues(q, BLK_MQ_BUDGET_DELAY_MS);
	} else if (multi) {
		dispatched = blk_mq_dispatch_multi(list);
	} else {
		dispatched = blk_mq_dispatch_rq_list(hctx, &list);
	}
	if (busy)
		return -EAGAIN;
	return dispatched != 0;
}

static int blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	uint64_t end = hrtimer_now_ns() + BLK_MQ_DISPATCH_MAX_NS;
	int ret;

	for (;;) {
		ret = __blk_mq_do_dispatch_sched(hctx);
		if (ret != 1)
			break;
		if (sched_need_resched() || hrtimer_now_ns() > end) {
			blk_mq_delay_run_hw_queue(hctx, 0);
			break;
		}
	}
	return ret;
}

/* Take the requests of one software queue (ctx->lock). */
static struct blk_request *blk_mq_ctx_take(struct blk_mq_hw_ctx *hctx,
					   struct blk_mq_ctx *ctx, int one)
{
	enum hctx_type t = hctx->type;
	unsigned int bit = ctx->index_hw[t];
	struct blk_request *rq;
	uint64_t fl;

	spin_lock_irqsave(&ctx->lock, &fl);
	rq = ctx->rq_head[t];
	if (rq) {
		if (one) {
			ctx->rq_head[t] = rq->disp_next;
			if (!ctx->rq_head[t])
				ctx->rq_tail[t] = NULL;
			rq->disp_next = NULL;
		} else {
			ctx->rq_head[t] = ctx->rq_tail[t] = NULL;
		}
		for (struct blk_request *r = rq; r; r = r->disp_next)
			r->bl.internal &= ~BLK_RQI_MQ_CTX;
	}
	if (!ctx->rq_head[t])
		(void)__atomic_fetch_and(&hctx->ctx_map[bit / 64],
					 ~(1ULL << (bit % 64)),
					 __ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&ctx->lock, fl);
	return rq;
}

/* Everything on the hctx's software queues (blk_mq_flush_busy_ctxs). */
static struct blk_request *blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx)
{
	struct blk_request *head = NULL, **tailp = &head;

	for (unsigned int i = 0; i < hctx->nr_ctx; i++) {
		struct blk_request *l;

		if (!(__atomic_load_n(&hctx->ctx_map[i / 64], __ATOMIC_ACQUIRE) &
		      (1ULL << (i % 64))))
			continue;
		l = blk_mq_ctx_take(hctx, hctx->ctxs[i], 0);
		*tailp = l;
		while (*tailp)
			tailp = &(*tailp)->disp_next;
	}
	return head;
}

static int blk_mq_ctx_map_any(const struct blk_mq_hw_ctx *hctx)
{
	for (unsigned int w = 0; w < (MAX_CPUS + 63) / 64; w++)
		if (__atomic_load_n(&hctx->ctx_map[w], __ATOMIC_ACQUIRE))
			return 1;
	return 0;
}

/* The driver is busy and there is no scheduler: one request per software
 * queue in turn (fairness between CPUs) while the driver takes them. */
static int blk_mq_do_dispatch_ctx(struct blk_mq_hw_ctx *hctx)
{
	unsigned int from = hctx->dispatch_from;
	int ret = 0;

	for (;;) {
		struct blk_request *rq = NULL;
		unsigned int tried = 0;

		if (__atomic_load_n(&hctx->nr_dispatch, __ATOMIC_ACQUIRE)) {
			ret = -EAGAIN;
			break;
		}
		if (!blk_mq_ctx_map_any(hctx) || !hctx->nr_ctx)
			break;
		while (!rq && tried < hctx->nr_ctx) {
			unsigned int i = (from + tried) % hctx->nr_ctx;

			tried++;
			if (__atomic_load_n(&hctx->ctx_map[i / 64],
					    __ATOMIC_ACQUIRE) &
			    (1ULL << (i % 64)))
				rq = blk_mq_ctx_take(hctx, hctx->ctxs[i], 1);
			if (rq)
				from = (i + 1) % hctx->nr_ctx;
		}
		if (!rq) {
			blk_mq_delay_run_hw_queues(hctx->queue,
						   BLK_MQ_BUDGET_DELAY_MS);
			break;
		}
		if (!blk_mq_dispatch_rq_list(hctx, &rq))
			break;
	}
	hctx->dispatch_from = from;
	return ret;
}

/* One pass over an hctx (__blk_mq_sched_dispatch_requests). */
static int __blk_mq_sched_dispatch(struct blk_mq_hw_ctx *hctx)
{
	struct blk_queue *q = hctx->queue;
	struct blk_request *list;
	int need_dispatch;

	list = blk_mq_take_dispatch(hctx);
	if (list) {
		/* What the driver refused before may be refused again: the
		 * next tag freed runs us (restart). */
		blk_mq_sched_mark_restart_hctx(hctx);
		if (!blk_mq_dispatch_rq_list(hctx, &list))
			return 0;
		need_dispatch = 1;
	} else {
		need_dispatch = __atomic_load_n(&hctx->dispatch_busy,
						__ATOMIC_RELAXED) != 0;
	}
	if (blk_queue_flags(q) & BLK_QUEUE_FL_SUSPENDED)
		return 0;
	/* The elevator: mq-deadline, or what a switch to "none" left in
	 * it. */
	if (q->sched != BLK_SCHED_NONE || blk_queue_elv_pending(q))
		return blk_mq_do_dispatch_sched(hctx);
	if (need_dispatch)
		return blk_mq_do_dispatch_ctx(hctx);
	list = blk_mq_flush_busy_ctxs(hctx);
	if (list)
		(void)blk_mq_dispatch_rq_list(hctx, &list);
	return 0;
}

static void blk_mq_run_dispatch(struct blk_mq_hw_ctx *hctx)
{
	struct blk_queue *q = hctx->queue;

	/* Counted before the quiesced state is looked at: a quiescer that
	 * set it first waits for us. */
	(void)__atomic_add_fetch(&hctx->dispatching, 1, __ATOMIC_SEQ_CST);
	/* A dying queue (disk marked dead, or being torn down) still
	 * dispatches what is queued: the driver fails it (its not-ready
	 * path) or completes it, so the usage count drains -- a queue that
	 * refused would strand those requests until teardown, and teardown
	 * itself waits for them. */
	if (!blk_queue_quiesced(q) && !blk_mq_hctx_stopped(hctx)) {
		hctx->run++;
		if (__blk_mq_sched_dispatch(hctx) == -EAGAIN &&
		    __blk_mq_sched_dispatch(hctx) == -EAGAIN)
			blk_mq_run_hw_queue(hctx, 1);
	}
	(void)__atomic_sub_fetch(&hctx->dispatching, 1, __ATOMIC_SEQ_CST);
	if (blk_queue_quiesced(q))
		wake_up(&q->mq_quiesce_wq);
}

/* ==========================================================================
 * Insertion
 * ========================================================================== */

/* Onto the hctx dispatch list (pass-through, flush, requeued). */
static void blk_mq_request_bypass_insert(struct blk_request *rq, int at_head)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	uint64_t fl;

	spin_lock_irqsave(&hctx->lock, &fl);
	blk_mq_dispatch_add_locked(hctx, rq, at_head);
	spin_unlock_irqrestore(&hctx->lock, fl);
}

/* Onto the submitting CPU's software queue. */
static void blk_mq_ctx_insert(struct blk_request *rq, unsigned int cpu,
			      int at_head)
{
	struct blk_queue *q = rq->queue;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct blk_mq_ctx *ctx = &q->queue_ctx[cpu < MAX_CPUS ? cpu : 0];
	enum hctx_type t = hctx->type;
	unsigned int bit;
	uint64_t fl;

	/* The ctx of a CPU this hctx does not serve (a type that fell back
	 * to the default map): the hctx's own first ctx carries it. */
	if (ctx->hctxs[t] != hctx) {
		if (!hctx->nr_ctx) {
			blk_mq_request_bypass_insert(rq, at_head);
			return;
		}
		ctx = hctx->ctxs[0];
	}
	bit = ctx->index_hw[t];
	spin_lock_irqsave(&ctx->lock, &fl);
	rq->disp_next = NULL;
	if (at_head) {
		rq->disp_next = ctx->rq_head[t];
		ctx->rq_head[t] = rq;
		if (!ctx->rq_tail[t])
			ctx->rq_tail[t] = rq;
	} else {
		if (ctx->rq_tail[t])
			ctx->rq_tail[t]->disp_next = rq;
		else
			ctx->rq_head[t] = rq;
		ctx->rq_tail[t] = rq;
	}
	rq->bl.internal |= BLK_RQI_MQ_CTX;
	(void)__atomic_fetch_or(&hctx->ctx_map[bit / 64], 1ULL << (bit % 64),
				__ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&ctx->lock, fl);
}

/* blk_mq_try_issue_directly: an idle hctx without scheduler takes the
 * request straight from the submitter. */
static void blk_mq_try_issue_directly(struct blk_mq_hw_ctx *hctx,
				      struct blk_request *rq, unsigned int cpu)
{
	struct blk_queue *q = hctx->queue;
	blk_status_t ret;

	(void)__atomic_add_fetch(&hctx->dispatching, 1, __ATOMIC_SEQ_CST);
	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(q) ||
	    (blk_queue_flags(q) & BLK_QUEUE_FL_SUSPENDED)) {
		(void)__atomic_sub_fetch(&hctx->dispatching, 1,
					 __ATOMIC_SEQ_CST);
		if (blk_queue_quiesced(q))
			wake_up(&q->mq_quiesce_wq);
		blk_mq_ctx_insert(rq, cpu, 0);
		blk_mq_run_hw_queue(hctx, 0);
		return;
	}
	if (!blk_mq_get_driver_tag(rq)) {
		(void)__atomic_sub_fetch(&hctx->dispatching, 1,
					 __ATOMIC_SEQ_CST);
		if (blk_queue_quiesced(q))
			wake_up(&q->mq_quiesce_wq);
		blk_mq_ctx_insert(rq, cpu, 0);
		blk_mq_run_hw_queue(hctx, (rq->flags & BLK_REQ_NOWAIT) != 0);
		return;
	}
	ret = blk_mq_issue(hctx, rq, 1);
	switch (ret) {
	case BLK_STS_OK:
		blk_mq_update_dispatch_busy(hctx, 0);
		break;
	case BLK_STS_RESOURCE:
	case BLK_STS_DEV_RESOURCE:
		blk_mq_update_dispatch_busy(hctx, 1);
		blk_mq_request_bypass_insert(rq, 0);
		break;
	default:
		blk_mq_update_dispatch_busy(hctx, 0);
		blk_mq_end_request(rq, ret);
		break;
	}
	(void)__atomic_sub_fetch(&hctx->dispatching, 1, __ATOMIC_SEQ_CST);
	if (blk_queue_quiesced(q))
		wake_up(&q->mq_quiesce_wq);
	if (ret == BLK_STS_RESOURCE || ret == BLK_STS_DEV_RESOURCE)
		blk_mq_run_hw_queue(hctx, 0);
}

/* Map, account and place a request; `direct': it may be issued right
 * here.  Returns the hctx to run (NULL: nothing to run). */
static struct blk_mq_hw_ctx *blk_mq_place(struct blk_queue *q,
					  struct blk_request *rq,
					  int from_schedule, int *run_async)
{
	unsigned int cpu = blk_mq_cpu();
	struct blk_mq_hw_ctx *hctx;

	BUG_ON(q == NULL || rq == NULL);
	VM_BUG_ON(rq->magic != BLK_REQUEST_MAGIC);
	*run_async = from_schedule;
	rq->state = BLK_RQ_QUEUED;
	rq->submit_cpu = blk_this_cpu();
	if (!rq->mq_hctx)
		rq->mq_hctx = blk_mq_rq_map(q, rq, cpu);
	hctx = rq->mq_hctx;
	if (unlikely(blk_queue_dying(q) || !q->nr_hw_queues)) {
		blk_queue_fail_rq(q, rq, from_schedule);
		return NULL;
	}
	blk_queue_account_start_rq(q, rq);
	if (rq->bl.internal & BLK_RQI_FLUSH_SEQ) {
		blk_queue_insert_flush(q, rq);
		return NULL;
	}
	if (blk_queue_rq_is_bypass(rq)) {
		/* Pass-through, the cache flush, HEAD and PM requests: to
		 * the dispatch list (a flush at its head), run here. */
		blk_mq_request_bypass_insert(
			rq, rq->op == BLK_OP_FLUSH || (rq->flags & BLK_REQ_HEAD));
		return hctx;
	}
	if (q->sched != BLK_SCHED_NONE) {
		/* mq-deadline: sorted and merged; kblockd dispatches. */
		*run_async = 1;
		if (!blk_queue_elv_insert(q, rq))
			return NULL; /* merged into a queued request */
		return hctx;
	}
	if (!from_schedule && blk_mq_run_here(hctx) &&
	    !(__atomic_load_n(&hctx->dispatch_busy, __ATOMIC_RELAXED) &&
	      (q->nr_hw_queues == 1 || !(rq->flags & BLK_REQ_SYNC))) &&
	    !__atomic_load_n(&hctx->nr_dispatch, __ATOMIC_ACQUIRE)) {
		blk_mq_try_issue_directly(hctx, rq, cpu);
		return NULL;
	}
	blk_mq_ctx_insert(rq, cpu, 0);
	if (__atomic_load_n(&hctx->dispatch_busy, __ATOMIC_RELAXED))
		*run_async = 1;
	return hctx;
}

void blk_mq_queue_insert(struct blk_queue *q, struct blk_request *rq,
			 int from_schedule)
{
	int async;
	struct blk_mq_hw_ctx *hctx = blk_mq_place(q, rq, from_schedule, &async);

	if (hctx)
		blk_mq_run_hw_queue(hctx, async);
}

void blk_mq_requeue_insert(struct blk_queue *q, struct blk_request *rq)
{
	BUG_ON(q == NULL || rq == NULL);
	rq->bl.internal &= ~BLK_RQI_MQ_REQUEUE;
	if (!rq->mq_hctx)
		rq->mq_hctx = blk_mq_rq_map(q, rq,
					    rq->submit_cpu >= 0 ?
						    (unsigned int)rq->submit_cpu :
						    blk_mq_cpu());
	rq->state = BLK_RQ_QUEUED;
	/* A request the driver prepared already goes behind what waits; a
	 * flush and anything else in front (the established order). */
	blk_mq_request_bypass_insert(rq, !(rq->flags & BLK_REQ_DONTPREP) ||
						 (rq->bl.internal &
						  BLK_RQI_FLUSH_RQ));
}

struct blk_request **blk_mq_collect_queued(struct blk_queue *q,
					   struct blk_request **tailp)
{
	uint64_t fl;

	if (!q->queue_ctx)
		return tailp;
	blk_mq_hctx_read_wait(q);
	for (unsigned int c = 0; c < MAX_CPUS; c++) {
		struct blk_mq_ctx *ctx = &q->queue_ctx[c];

		spin_lock_irqsave(&ctx->lock, &fl);
		for (unsigned int t = 0; t < HCTX_MAX_TYPES; t++) {
			struct blk_request *l = ctx->rq_head[t];
			struct blk_mq_hw_ctx *h = ctx->hctxs[t];

			if (!l)
				continue;
			ctx->rq_head[t] = ctx->rq_tail[t] = NULL;
			/* The pending bit goes with the list, under the same
			 * lock an insertion sets it under. */
			if (h) {
				unsigned int bit = ctx->index_hw[t];

				(void)__atomic_fetch_and(&h->ctx_map[bit / 64],
							 ~(1ULL << (bit % 64)),
							 __ATOMIC_SEQ_CST);
			}
			while (l) {
				struct blk_request *n = l->disp_next;

				l->bl.internal &= ~BLK_RQI_MQ_CTX;
				*tailp = l;
				tailp = &l->disp_next;
				l = n;
			}
		}
		spin_unlock_irqrestore(&ctx->lock, fl);
	}
	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_hw_ctx *h = q->queue_hw_ctx[i];
		struct blk_request *l;

		spin_lock_irqsave(&h->lock, &fl);
		l = h->dispatch_head;
		h->dispatch_head = h->dispatch_tail = NULL;
		__atomic_store_n(&h->nr_dispatch, 0, __ATOMIC_RELEASE);
		spin_unlock_irqrestore(&h->lock, fl);
		while (l) {
			struct blk_request *n = l->disp_next;

			l->bl.internal &= ~BLK_RQI_MQ_DISPATCH;
			*tailp = l;
			tailp = &l->disp_next;
			l = n;
		}
	}
	blk_mq_hctx_read_end(q);
	/* Writes a zone write plug holds (accepted, not yet queued). */
	tailp = blk_zone_collect_plugged(q, tailp);
	*tailp = NULL;
	return tailp;
}

void blk_mq_wait_usage_zero(struct blk_queue *q)
{
	if (!q || !q->mq_usage)
		return;
	blk_might_sleep();
	wake_up(&q->mq_freeze_wq);
	if (!blk_can_sleep()) {
		while (blk_mq_usage_sum(q) != 0)
			lapic_delay_us(BLK_POLL_STEP_US);
		return;
	}
	while (wait_event_timeout(q->mq_freeze_wq, blk_mq_usage_sum(q) == 0,
				  10000) == 0)
		kprintf(BLK_LOG_PREFIX "%s: still waiting for %lld request%s before removal\n",
			q->disk ? q->disk->disk_name : "queue",
			(long long)blk_mq_usage_sum(q),
			blk_mq_usage_sum(q) == 1 ? "" : "s");
}

/* ==========================================================================
 * Plug flush: merging, ops->queue_rqs batching
 * ========================================================================== */

/* At most this many hctxs counted as dispatching for one batch. */
#define BLK_MQ_BATCH_HCTX 4

void blk_mq_flush_plug_list(struct blk_queue *q, struct blk_request *list,
			    int from_schedule)
{
	const struct blk_mq_ops *ops = q->tag_set ? q->tag_set->ops : NULL;
	struct blk_mq_hw_ctx *runh[BLK_MQ_BATCH_HCTX];
	int nrun = 0, run_all = 0;

	/* Without a scheduler nothing merges in the queue: neighbours that
	 * continue each other merge here, as the plug merge does. */
	if (q->sched == BLK_SCHED_NONE && !blk_queue_dying(q)) {
		for (struct blk_request *a = list; a && a->disp_next;) {
			struct blk_request *b = a->disp_next;

			if (!(a->bl.internal & BLK_RQI_FLUSH_SEQ) &&
			    !(b->bl.internal & BLK_RQI_FLUSH_SEQ) &&
			    blk_queue_merge_pair(q, a, b)) {
				a->disp_next = b->disp_next;
				b->disp_next = NULL;
				/* b is part of a now: it no longer holds the
				 * queue on its own. */
				blk_mq_rq_exit(q, b);
				blk_queue_leave(q);
				continue;
			}
			a = b;
		}
	}

	/* The batch: the driver takes a whole list at once (one doorbell). */
	if (ops && ops->queue_rqs && q->sched == BLK_SCHED_NONE &&
	    !from_schedule && list && list->disp_next &&
	    !blk_queue_quiesced(q) && !blk_queue_dying(q) &&
	    !(blk_queue_flags(q) & BLK_QUEUE_FL_SUSPENDED)) {
		struct rq_list batch;
		struct blk_mq_hw_ctx *hs[BLK_MQ_BATCH_HCTX];
		int nh = 0, ok = 1, nbatched = 0;
		unsigned int cpu = blk_mq_cpu();

		rq_list_init(&batch);
		while (list) {
			struct blk_request *rq = list;
			struct blk_mq_hw_ctx *h;
			int seen = 0;

			if ((rq->bl.internal & BLK_RQI_FLUSH_SEQ) ||
			    blk_queue_rq_is_bypass(rq))
				break;
			if (!rq->mq_hctx)
				rq->mq_hctx = blk_mq_rq_map(q, rq, cpu);
			h = rq->mq_hctx;
			if (blk_mq_hctx_stopped(h) || !blk_mq_run_here(h))
				break;
			for (int i = 0; i < nh; i++)
				if (hs[i] == h)
					seen = 1;
			if (!seen) {
				if (nh == BLK_MQ_BATCH_HCTX)
					break;
				(void)__atomic_add_fetch(&h->dispatching, 1,
							 __ATOMIC_SEQ_CST);
				hs[nh++] = h;
				if (blk_queue_quiesced(q)) {
					ok = 0;
					break;
				}
			}
			if (!blk_mq_get_driver_tag(rq))
				break;
			list = rq->disp_next;
			rq->disp_next = NULL;
			rq->submit_cpu = blk_this_cpu();
			blk_queue_account_start_rq(q, rq);
			rq->state = BLK_RQ_DISPATCHED;
			rq->dispatch_ns = hrtimer_now_ns();
			rq->rq_next = NULL;
			if (batch.tail)
				batch.tail->rq_next = rq;
			else
				batch.head = rq;
			batch.tail = rq;
			nbatched++;
		}
		if (ok && batch.head) {
			struct blk_mq_hw_ctx *ch = NULL;
			blk_status_t ret = BLK_STS_OK;
			int queued = 0;

			ops->queue_rqs(&batch);
			/* What the driver did not take, one by one.  `last'
			 * is only true for the list's final request: every
			 * hctx the run leaves, and the run's end after a
			 * request that did not go, gets commit_rqs, or the
			 * requests it took with last == 0 would wait for a
			 * doorbell that is never written. */
			while (batch.head) {
				struct blk_request *rq = batch.head;

				batch.head = rq->rq_next;
				rq->rq_next = NULL;
				rq->state = BLK_RQ_QUEUED;
				if (ch != rq->mq_hctx) {
					if (ch) {
						blk_mq_commit_rqs(ch, queued);
						queued = 0;
					}
					ch = rq->mq_hctx;
				}
				ret = blk_mq_issue(ch, rq, batch.head == NULL);
				if (ret == BLK_STS_OK) {
					queued++;
					continue;
				}
				if (ret == BLK_STS_RESOURCE ||
				    ret == BLK_STS_DEV_RESOURCE) {
					/* Out of resources: this one and the
					 * rest wait on the dispatch lists. */
					blk_mq_request_bypass_insert(rq, 0);
					while (batch.head) {
						rq = batch.head;
						batch.head = rq->rq_next;
						rq->rq_next = NULL;
						rq->state = BLK_RQ_QUEUED;
						blk_mq_request_bypass_insert(rq,
									     0);
					}
					run_all = 1;
					break;
				}
				blk_mq_end_request(rq, ret);
			}
			if (ret != BLK_STS_OK)
				blk_mq_commit_rqs(ch, queued);
			batch.tail = NULL;
		} else {
			/* Quiesced meanwhile: the batch waits on the
			 * dispatch lists. */
			while (batch.head) {
				struct blk_request *rq = batch.head;

				batch.head = rq->rq_next;
				rq->rq_next = NULL;
				rq->state = BLK_RQ_QUEUED;
				blk_mq_request_bypass_insert(rq, 0);
			}
			run_all = 1;
		}
		for (int i = 0; i < nh; i++) {
			(void)__atomic_sub_fetch(&hs[i]->dispatching, 1,
						 __ATOMIC_SEQ_CST);
		}
		if (blk_queue_quiesced(q))
			wake_up(&q->mq_quiesce_wq);
		/* Each batched request held the queue (blk_submit) until it
		 * was in. */
		while (nbatched-- > 0)
			blk_queue_leave(q);
	}

	while (list) {
		struct blk_request *rq = list;
		struct blk_mq_hw_ctx *h;
		int async;

		list = rq->disp_next;
		rq->disp_next = NULL;
		h = blk_mq_place(q, rq, 1, &async);
		if (h) {
			int seen = 0;

			for (int i = 0; i < nrun; i++)
				if (runh[i] == h)
					seen = 1;
			if (!seen) {
				if (nrun < BLK_MQ_BATCH_HCTX)
					runh[nrun++] = h;
				else
					run_all = 1;
			}
		}
		blk_queue_leave(q);
	}
	for (int i = 0; i < nrun; i++)
		blk_mq_run_hw_queue(runh[i], from_schedule);
	if (run_all)
		blk_mq_run_hw_queues(q, from_schedule);
}

/* ==========================================================================
 * Requests on a multi-queue queue
 * ========================================================================== */

void *blk_mq_rq_to_pdu(struct blk_request *rq)
{
	BUG_ON(rq == NULL);
	if (WARN_ON_ONCE(rq->tag < 0 || !rq->mq_hctx))
		return NULL;
	BUG_ON((unsigned int)rq->tag >= rq->mq_hctx->tags->nr_tags);
	return rq->mq_hctx->tags->pdu[rq->tag];
}

struct blk_request *blk_mq_rq_from_pdu(void *pdu)
{
	struct blk_mq_pdu_hdr *h;

	if (!pdu)
		return NULL;
	h = (struct blk_mq_pdu_hdr *)((uint8_t *)pdu -
				      sizeof(struct blk_mq_pdu_hdr));
	VM_BUG_ON(h->magic != BLK_MQ_PDU_MAGIC);
	return h->rq;
}

enum mq_rq_state blk_mq_rq_state(const struct blk_request *rq)
{
	return (enum mq_rq_state)__atomic_load_n(&rq->mq_state,
						 __ATOMIC_ACQUIRE);
}

/* `hctx_idx' < 0: the submitting CPU's default hctx. */
static struct blk_request *blk_mq_alloc_rq_common(struct blk_queue *q, int op,
						  uint32_t flags, int hctx_idx,
						  int *err)
{
	struct blk_mq_hw_ctx *hctx;
	struct blk_request *rq;
	int rc, tag, slot;

	if (err)
		*err = 0;
	if (!q || !blk_queue_is_mq(q) || (unsigned)op >= BLK_OP_NR ||
	    !q->mq_usage) {
		if (err)
			*err = -EINVAL;
		return NULL;
	}
	if (!(flags & BLK_MQ_REQ_NOWAIT))
		blk_might_sleep();
	rc = blk_mq_usage_get(q, (flags & BLK_MQ_REQ_NOWAIT) != 0, &slot);
	if (rc) {
		if (err)
			*err = rc;
		return NULL;
	}
	/* Inside the usage count the contexts cannot change (a resize
	 * freezes first): the index is checked and used here. */
	if (hctx_idx >= 0) {
		if ((unsigned int)hctx_idx >= q->nr_hw_queues) {
			blk_mq_usage_put(q, slot);
			if (err)
				*err = -EINVAL;
			return NULL;
		}
		hctx = q->queue_hw_ctx[hctx_idx];
	} else {
		hctx = blk_mq_map_queue_type(q, HCTX_TYPE_DEFAULT, blk_mq_cpu());
	}
	rq = blk_request_alloc(q->disk ? &q->disk->bdev : NULL,
			       (enum blk_op)op, 0, 0);
	if (!rq) {
		blk_mq_usage_put(q, slot);
		if (err)
			*err = -ENOMEM;
		return NULL;
	}
	rq->queue = q;
	rq->disk = q->disk;
	rq->flags |= BLK_REQ_MQ_ALLOC | BLK_REQ_NO_ACCOUNT;
	if (flags & BLK_MQ_REQ_PM)
		rq->flags |= BLK_REQ_PM;
	if (flags & BLK_MQ_REQ_RESERVED)
		rq->flags |= BLK_REQ_RESERVED;
	if (flags & BLK_MQ_REQ_NOWAIT)
		rq->flags |= BLK_REQ_NOWAIT;
	rq->mq_flags = flags & (BLK_MQ_REQ_NOWAIT | BLK_MQ_REQ_RESERVED |
				BLK_MQ_REQ_PM);
	rq->bl.usage_slot = slot;
	rq->bl.internal |= BLK_RQI_Q_USAGE;
	rq->timeout_ms = q->tag_set->timeout_ms;
	rq->mq_hctx = hctx;
	tag = blk_mq_get_tag(hctx, (flags & BLK_MQ_REQ_RESERVED) != 0,
			     (flags & BLK_MQ_REQ_NOWAIT) != 0,
			     !(flags & BLK_MQ_REQ_RESERVED));
	if (tag < 0) {
		rq->bl.internal &= ~BLK_RQI_Q_USAGE;
		blk_mq_usage_put(q, slot);
		blk_request_free(rq);
		if (err)
			*err = blk_queue_dying(q) ? -ENODEV : -EAGAIN;
		return NULL;
	}
	blk_mq_rq_set_tag(rq, (unsigned int)tag);
	return rq;
}

struct blk_request *blk_mq_alloc_request(struct blk_queue *q, int op,
					 uint32_t flags, int *err)
{
	return blk_mq_alloc_rq_common(q, op, flags, -1, err);
}

struct blk_request *blk_mq_alloc_request_hctx(struct blk_queue *q, int op,
					      uint32_t flags,
					      unsigned int hctx_idx, int *err)
{
	if (!q || !blk_queue_is_mq(q) || hctx_idx > 0x7fffffffu) {
		if (err)
			*err = -EINVAL;
		return NULL;
	}
	/* Like the established call: only without waiting (the caller is
	 * bound to that hardware queue, not to this CPU). */
	if (WARN_ON_ONCE(!(flags & BLK_MQ_REQ_NOWAIT))) {
		if (err)
			*err = -EINVAL;
		return NULL;
	}
	return blk_mq_alloc_rq_common(q, op, flags, (int)hctx_idx, err);
}

void blk_mq_free_request(struct blk_request *rq)
{
	if (!rq)
		return;
	if (WARN(rq->magic != BLK_REQUEST_MAGIC,
		 "freeing a multi-queue request that is not initialised"))
		return;
	if (WARN_ON_ONCE(!(rq->flags & BLK_REQ_MQ_ALLOC)))
		return;
	WARN_ON_ONCE(rq->state == BLK_RQ_QUEUED ||
		     rq->state == BLK_RQ_DISPATCHED);
	blk_mq_put_rq_ref(rq);
}

int blk_rq_map_kern(struct blk_request *rq, void *kbuf, unsigned int len)
{
	uint32_t align;

	if (!rq || !kbuf || !len)
		return -EINVAL;
	align = rq->queue ? rq->queue->limits.dma_alignment : 3;
	if ((((uint64_t)(uintptr_t)kbuf) | len) & align)
		return -EINVAL;
	return blk_request_add_buf(rq, kbuf, len);
}

void blk_execute_rq_nowait(struct blk_request *rq, int at_head)
{
	struct blk_queue *q;

	BUG_ON(rq == NULL);
	q = rq->queue;
	BUG_ON(q == NULL || rq->mq_hctx == NULL);
	if (WARN(rq->state != BLK_RQ_IDLE && rq->state != BLK_RQ_COMPLETE,
		 "executing a request in flight (state %d)", (int)rq->state))
		return;
	rq->state = BLK_RQ_QUEUED;
	rq->done = 0;
	rq->status = 0;
	rq->resid_bytes = 0;
	rq->submit_ns = hrtimer_now_ns();
	rq->submit_cpu = blk_this_cpu();
	if (!rq->timeout_ms)
		rq->timeout_ms = q->tag_set->timeout_ms;
	/* The finishing step drops this (blk_rq_finish). */
	refcount_inc(&rq->ref);
	if (unlikely(blk_queue_dying(q))) {
		/* Its disk gone (blk_mark_disk_dead): an I/O error. */
		blk_request_complete(rq,
				     q->disk && (disk_flags(q->disk) &
						 GENHD_FL_GONE) ?
					     -EIO :
					     -ENODEV,
				     0);
		return;
	}
	blk_mq_request_bypass_insert(rq, at_head);
	blk_mq_run_hw_queue(rq->mq_hctx, 0);
}

blk_status_t blk_execute_rq(struct blk_request *rq, int at_head)
{
	int rc;

	blk_execute_rq_nowait(rq, at_head);
	if (blk_rq_is_poll(rq))
		return blk_execute_rq_polled(rq, -1);
	rc = blk_wait(rq);
	return errno_to_blk_status(rc);
}

blk_status_t blk_execute_rq_polled(struct blk_request *rq, int at_head)
{
	if (at_head >= 0)
		blk_execute_rq_nowait(rq, at_head);
	for (;;) {
		uint64_t fl;
		int d;

		spin_lock_irqsave(&rq->done_lock, &fl);
		d = rq->done;
		spin_unlock_irqrestore(&rq->done_lock, fl);
		if (d)
			break;
		(void)blk_rq_poll(rq, NULL, 0);
		if (!blk_can_sleep()) {
			blk_softirq_poll();
			blk_work_run_pending();
		}
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return errno_to_blk_status(rq->status);
}

/* ==========================================================================
 * The driver's side: start, complete, end
 * ========================================================================== */

void blk_mq_start_request(struct blk_request *rq)
{
	struct blk_queue *q;
	uint64_t now, tmo;

	BUG_ON(rq == NULL);
	q = rq->queue;
	BUG_ON(q == NULL || rq->mq_hctx == NULL);
	WARN_ON_ONCE(blk_mq_rq_state(rq) != MQ_RQ_IDLE);
	now = hrtimer_now_ns();
	tmo = rq->timeout_ms ? rq->timeout_ms : q->tag_set->timeout_ms;
	if (!tmo)
		tmo = BLK_MQ_DEFAULT_TIMEOUT_MS;
	__atomic_store_n(&rq->deadline_ns, now + tmo * 1000000ULL,
			 __ATOMIC_RELAXED);
	blk_mq_rqf_clear(rq, BLK_MQ_RQF_TIMED_OUT);
	blk_queue_add_timer_ns(q, rq->deadline_ns, now);
	__atomic_store_n(&rq->mq_state, MQ_RQ_IN_FLIGHT, __ATOMIC_RELEASE);
	/* The tag table names the request (the timeout scan finds it). */
	if (rq->tag >= 0)
		__atomic_store_n(&rq->mq_hctx->tags->rqs[rq->tag], rq,
				 __ATOMIC_RELEASE);
	if (blk_integrity_rq(rq) && rq->op == BLK_OP_WRITE)
		blk_integrity_prepare(rq);
	if (rq->flags & BLK_REQ_POLLED)
		rq->poll_cookie = rq->mq_hctx->queue_num;
}

void blk_mq_set_request_complete(struct blk_request *rq)
{
	BUG_ON(rq == NULL);
	__atomic_store_n(&rq->mq_state, MQ_RQ_COMPLETE, __ATOMIC_RELEASE);
}

/* The softirq half (rq->softirq_entry.fn). */
static void blk_mq_softirq_done(struct blk_softirq_entry *e)
{
	struct blk_request *rq = (struct blk_request *)
		((char *)e - __builtin_offsetof(struct blk_request,
						softirq_entry));
	struct blk_cpl_ctx cpl;

	VM_BUG_ON(rq->magic != BLK_REQUEST_MAGIC);
	blk_cpl_begin(&cpl);
	rq->queue->tag_set->ops->complete(rq);
	blk_cpl_end(&cpl);
}

/* Mark complete: 1 when this call did it (it was in flight or idle), 0
 * when it was complete already (a timeout or cancel came first). */
static int blk_mq_mark_complete(struct blk_request *rq)
{
	int old = __atomic_load_n(&rq->mq_state, __ATOMIC_ACQUIRE);

	for (;;) {
		if (old == MQ_RQ_COMPLETE)
			return 0;
		/* Ended already (its end only waits for the last reference):
		 * a late completion or a cancel must not run it again.  The
		 * end sets ENDED before it makes the request IDLE, so a
		 * request seen IN_FLIGHT here either fails the exchange below
		 * or was not ended yet. */
		if (blk_mq_rq_ended(rq))
			return 0;
		if (__atomic_compare_exchange_n(&rq->mq_state, &old,
						MQ_RQ_COMPLETE, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return 1;
	}
}

/* The routing decision: 1 when handed to a softirq (this CPU's or the
 * submitter's), 0 when the caller completes in its own context. */
static int blk_mq_route_completion(struct blk_request *rq)
{
	struct blk_queue *q = rq->queue;
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	unsigned int flags = 0;
	uint8_t aff = __atomic_load_n(&q->rq_affinity, __ATOMIC_RELAXED);

	if (aff >= 2)
		flags |= BLK_SOFTIRQ_SAME_COMP | BLK_SOFTIRQ_SAME_FORCE;
	else if (aff)
		flags |= BLK_SOFTIRQ_SAME_COMP;
	if (q->nr_hw_queues > 1)
		flags |= BLK_SOFTIRQ_MULTI_HWQ;
	if (hctx && hctx->nr_ctx == 1)
		flags |= BLK_SOFTIRQ_HCTX_ONE_CTX;
	/* A polled request, or a poller with interrupts off (nobody else
	 * would ever run a softirq for it): complete here. */
	if ((rq->flags & BLK_REQ_POLLED) ||
	    (!irqs_enabled() && !blk_in_hardirq() && !blk_in_softirq()))
		flags |= BLK_SOFTIRQ_POLLED;
	rq->softirq_entry.next = NULL;
	rq->softirq_entry.fn = blk_mq_softirq_done;
	return blk_softirq_complete_remote(&rq->softirq_entry, rq->submit_cpu,
					   flags);
}

int blk_mq_complete_request_remote(struct blk_request *rq)
{
	BUG_ON(rq == NULL);
	if (unlikely(rq->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "completing a request with bad magic %08x",
			       rq->magic);
		return 1;
	}
	/* Completed already (timeout, cancel): nothing more to do -- report
	 * "taken" so the caller does not complete it a second time. */
	if (!blk_mq_mark_complete(rq))
		return 1;
	return blk_mq_route_completion(rq);
}

int blk_mq_complete_request(struct blk_request *rq)
{
	BUG_ON(rq == NULL);
	if (unlikely(rq->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "completing a request with bad magic %08x",
			       rq->magic);
		return 0;
	}
	if (!blk_mq_mark_complete(rq))
		return 0;
	if (blk_mq_route_completion(rq))
		return 1;
	/* Here: the interrupt is the submitter's own (several hardware
	 * queues), a poller, a timeout or a cancel. */
	rq->queue->tag_set->ops->complete(rq);
	return 0;
}

int blk_mq_add_to_batch(struct blk_request *rq, struct io_comp_batch *iob,
			int is_error, void (*complete)(struct io_comp_batch *iob))
{
	if (!iob || !rq)
		return 0;
	/* blk_mq_end_request_batch ends every request with OK. */
	if (is_error)
		return 0;
	if (!iob->complete)
		iob->complete = complete;
	else if (iob->complete != complete)
		return 0;
	rq->rq_next = iob->req_list;
	iob->req_list = rq;
	return 1;
}

/* Hybrid polling: the mean completion time per direction. */
static void blk_mq_poll_stats_add(struct blk_request *rq)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	uint64_t now, dt, mean;
	int dir;

	if (!hctx || !(rq->flags & BLK_REQ_POLLED) || !rq->dispatch_ns)
		return;
	now = hrtimer_now_ns();
	if (now <= rq->dispatch_ns)
		return;
	dt = now - rq->dispatch_ns;
	dir = rq_data_dir(rq) ? 1 : 0;
	mean = hctx->poll_stat_mean_ns[dir];
	/* A moving average over the last ~8 completions. */
	hctx->poll_stat_mean_ns[dir] = hctx->poll_stat_nr[dir] ?
					       (mean * 7 + dt) / 8 :
					       dt;
	hctx->poll_stat_nr[dir]++;
}

void blk_mq_end_request(struct blk_request *rq, blk_status_t status)
{
	int err, claimed;
	uint32_t resid;

	BUG_ON(rq == NULL);
	if (unlikely(rq->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "ending a request with bad magic %08x",
			       rq->magic);
		return;
	}
	/* A tagged request ends once (a completion that lost to a
	 * dispatcher's end, or the other way round): the end is claimed
	 * before any of it runs, so a racing second end does not remap the
	 * protection information back a second time either. */
	claimed = rq->tag >= 0 && !(rq->flags & BLK_REQ_MQ_ALLOC);
	if (claimed) {
		if (unlikely(!blk_mq_end_claim(rq)))
			return;
		/* Completed for good already (its last reference went):
		 * nothing of it may be touched as a live request. */
		if (unlikely(rq->state == BLK_RQ_COMPLETE ||
			     rq->state == BLK_RQ_IDLE)) {
			WARN_RATELIMIT(1,
				       "request %s sector %llu completed twice (state %d)",
				       blk_op_str(rq->op),
				       (unsigned long long)rq->sector,
				       (int)rq->state);
			blk_mq_rqf_clear(rq, BLK_MQ_RQF_ENDED);
			return;
		}
	}
	/* Protection information: remapped back and verified for a read
	 * the block layer protects (blk_integrity.c). */
	if (blk_integrity_rq(rq))
		status = blk_integrity_end(rq, status);
	blk_mq_poll_stats_add(rq);
	err = blk_status_to_errno(status);
	resid = status ? (uint32_t)(blk_rq_payload_bytes(rq) > 0xFFFFFFFFULL ?
					    0xFFFFFFFFu :
					    blk_rq_payload_bytes(rq)) :
			 0;
	if (rq->flags & BLK_REQ_MQ_ALLOC) {
		/* The allocation keeps the tag until blk_mq_free_request. */
		__atomic_store_n(&rq->mq_state, MQ_RQ_IDLE, __ATOMIC_RELEASE);
	}
	if (claimed) {
		/* What blk_request_complete's end hook does, with the end
		 * already ours. */
		blk_mq_end_claimed(rq, err, resid);
		return;
	}
	blk_request_complete(rq, err, resid);
}

void blk_mq_end_request_batch(struct io_comp_batch *iob)
{
	struct blk_request *list;

	if (!iob)
		return;
	list = iob->req_list;
	iob->req_list = NULL;
	/* The list was built in reverse: end in completion order. */
	{
		struct blk_request *rev = NULL;

		while (list) {
			struct blk_request *n = list->rq_next;

			list->rq_next = rev;
			rev = list;
			list = n;
		}
		list = rev;
	}
	while (list) {
		struct blk_request *rq = list;

		list = rq->rq_next;
		rq->rq_next = NULL;
		blk_mq_end_request(rq, BLK_STS_OK);
	}
}

/* ==========================================================================
 * Requeue
 * ========================================================================== */

void blk_mq_requeue_request(struct blk_request *rq, int kick_requeue_list)
{
	struct blk_queue *q;

	BUG_ON(rq == NULL);
	q = rq->queue;
	BUG_ON(q == NULL);
	/* Ended already: its end only waits for the last tag reference;
	 * putting it back on a list would hand a finished request to the
	 * driver again (and to its submitter after it freed it). */
	if (unlikely(blk_mq_rq_ended(rq))) {
		WARN_RATELIMIT(1, "requeue of an ended request (%s sector %llu)",
			       blk_op_str(rq->op),
			       (unsigned long long)rq->sector);
		return;
	}
	if (blk_mq_request_started(rq)) {
		__atomic_store_n(&rq->mq_state, MQ_RQ_IDLE, __ATOMIC_RELEASE);
		blk_mq_rqf_clear(rq, BLK_MQ_RQF_TIMED_OUT);
	}
	rq->state = BLK_RQ_QUEUED;
	if (rq->mq_hctx)
		rq->mq_hctx->requeued++;
	/* The driver tag (and with it the per-command data the driver keeps
	 * across retries) stays with the request. */
	if (blk_queue_requeue_add(q, rq)) {
		/* Dying: it ends here. */
		blk_request_complete(rq, -ENODEV, (uint32_t)blk_rq_payload_bytes(rq));
		return;
	}
	if (kick_requeue_list)
		blk_mq_kick_requeue_list(q);
}

void blk_mq_kick_requeue_list(struct blk_queue *q)
{
	if (q)
		blk_queue_kick_requeue_ns(q, 0);
}

void blk_mq_delay_kick_requeue_list(struct blk_queue *q, unsigned long msecs)
{
	if (q)
		blk_queue_kick_requeue_ns(q, (uint64_t)msecs * 1000000ULL);
}

/* ==========================================================================
 * Timeouts
 * ========================================================================== */

struct blk_mq_expired {
	uint64_t start;
	uint64_t next;
	int has_timedout;
};

static int blk_mq_req_expired(struct blk_request *rq, struct blk_mq_expired *e)
{
	uint64_t deadline;

	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
		return 0;
	if (blk_mq_rqf_test(rq, BLK_MQ_RQF_TIMED_OUT))
		return 0;
	deadline = __atomic_load_n(&rq->deadline_ns, __ATOMIC_ACQUIRE);
	if (e->start >= deadline)
		return 1;
	if (!e->next || deadline < e->next)
		e->next = deadline;
	return 0;
}

static int blk_mq_check_expired(struct blk_request *rq, void *data)
{
	struct blk_mq_expired *e = (struct blk_mq_expired *)data;

	if (blk_mq_req_expired(rq, e)) {
		e->has_timedout = 1;
		return 0;
	}
	return 1;
}

static void blk_mq_rq_timed_out(struct blk_request *rq)
{
	struct blk_queue *q = rq->queue;
	const struct blk_mq_ops *ops = q->tag_set->ops;

	blk_mq_rqf_set(rq, BLK_MQ_RQF_TIMED_OUT);
	if (rq->mq_hctx)
		rq->mq_hctx->timeouts_handled++;
	(void)__atomic_add_fetch(&q->timeouts, 1, __ATOMIC_RELAXED);
	if (ops->timeout) {
		enum blk_eh_timer_return ret = ops->timeout(rq);

		if (ret == BLK_EH_DONE)
			return;
		WARN_ON_ONCE(ret != BLK_EH_RESET_TIMER);
	}
	/* Another period (blk_add_timer). */
	{
		uint64_t now = hrtimer_now_ns();
		uint64_t tmo = rq->timeout_ms ? rq->timeout_ms :
						q->tag_set->timeout_ms;

		__atomic_store_n(&rq->deadline_ns, now + tmo * 1000000ULL,
				 __ATOMIC_RELEASE);
		blk_mq_rqf_clear(rq, BLK_MQ_RQF_TIMED_OUT);
		blk_queue_add_timer_ns(q, rq->deadline_ns, now);
	}
}

static int blk_mq_handle_expired(struct blk_request *rq, void *data)
{
	struct blk_mq_expired *e = (struct blk_mq_expired *)data;

	if (blk_mq_req_expired(rq, e))
		blk_mq_rq_timed_out(rq);
	return 1;
}

/* Walk the busy tags of every distinct tag map of the queue.  The caller
 * is inside blk_mq_hctx_read_try. */
static void blk_mq_queue_tag_busy_iter(struct blk_queue *q,
				       int (*fn)(struct blk_request *, void *),
				       void *data)
{
	struct blk_mq_tags *seen[8];
	int nseen = 0;

	for (unsigned int i = 0; i < q->nr_hw_queues; i++) {
		struct blk_mq_tags *t = q->queue_hw_ctx[i]->tags;
		int dup = 0;

		for (int j = 0; j < nseen; j++)
			if (seen[j] == t)
				dup = 1;
		if (dup)
			continue;
		if (nseen < 8)
			seen[nseen++] = t;
		if (!blk_mq_tags_iter(t, q, NULL, 1, fn, data))
			return;
	}
}

void blk_mq_timeout_work(struct blk_queue *q)
{
	struct blk_mq_expired e;

	if (!q || !blk_queue_is_mq(q))
		return;
	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	/* A dying queue still times out what its driver holds (a removal
	 * waits for exactly those requests, and only the driver's timeout
	 * handling gets them back from a hung device); teardown turns the
	 * timer off for good once they drained (BLK_QUEUE_FL_NOTIMER). */
	if (blk_queue_flags(q) & BLK_QUEUE_FL_NOTIMER)
		goto out;
	/* A resize under way: the queue is frozen and drained, nothing can
	 * time out (a request started afterwards arms the timer again). */
	if (!blk_mq_hctx_read_try(q))
		goto out;
	if (!q->nr_hw_queues)
		goto unlock;
	e.start = hrtimer_now_ns();
	e.next = 0;
	e.has_timedout = 0;
	blk_mq_queue_tag_busy_iter(q, blk_mq_check_expired, &e);
	if (e.has_timedout) {
		e.next = 0;
		blk_mq_queue_tag_busy_iter(q, blk_mq_handle_expired, &e);
	}
	if (e.next) {
		blk_queue_mod_timer_ns(q, e.next);
	} else {
		/* A forward rolling timer: nothing pending for a while --
		 * the hctxs are idle (their share of a shared map goes). */
		for (unsigned int i = 0; i < q->nr_hw_queues; i++)
			if (!blk_mq_hctx_has_pending(q->queue_hw_ctx[i]))
				blk_mq_tag_idle(q->queue_hw_ctx[i]);
	}
unlock:
	blk_mq_hctx_read_end(q);
out:
	(void)__atomic_sub_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	blk_queue_wake_waiters(q);
}

/* ==========================================================================
 * Polling
 * ========================================================================== */

int blk_rq_is_poll(const struct blk_request *rq)
{
	return rq && rq->mq_hctx && rq->mq_hctx->type == HCTX_TYPE_POLL;
}

static int blk_hctx_poll(struct blk_queue *q, struct blk_mq_hw_ctx *hctx,
			 struct io_comp_batch *iob, unsigned int flags)
{
	const struct blk_mq_ops *ops = q->tag_set->ops;
	task_t *cur = sched_current();
	int ret;

	if (!ops->poll)
		return 0;
	hctx->poll_considered++;
	do {
		hctx->poll_invoked++;
		ret = ops->poll(hctx, iob);
		if (ret > 0) {
			hctx->poll_success++;
			return ret;
		}
		if (cur && blk_can_sleep() && signal_pending(cur))
			return 1;
		if (ret < 0 || (flags & BLK_POLL_ONESHOT))
			break;
		__asm__ volatile("pause");
	} while (!sched_need_resched());
	return 0;
}

/* Hybrid polling: sleep about half the mean completion time (or the
 * configured delay) before spinning, once per request. */
static void blk_mq_poll_hybrid(struct blk_queue *q, struct blk_request *rq)
{
	int32_t delay = q->limits.io_poll_delay;
	uint64_t ns;

	if (delay < 0 || blk_mq_rqf_test(rq, BLK_MQ_RQF_POLL_SLEPT) ||
	    !blk_can_sleep())
		return;
	if (delay > 0) {
		ns = (uint64_t)delay * 1000ULL;
	} else {
		int dir = rq_data_dir(rq) ? 1 : 0;

		if (!rq->mq_hctx->poll_stat_nr[dir])
			return;
		ns = (rq->mq_hctx->poll_stat_mean_ns[dir] + 1) / 2;
	}
	blk_mq_rqf_set(rq, BLK_MQ_RQF_POLL_SLEPT);
	if (!ns)
		return;
	/* Sleep until the request should be close to done, unless it
	 * completed already. */
	if (rq->dispatch_ns && hrtimer_now_ns() < rq->dispatch_ns + ns &&
	    blk_mq_rq_state(rq) == MQ_RQ_IN_FLIGHT)
		(void)hrtimer_sleep_until(rq->dispatch_ns + ns, NULL);
}

int blk_rq_poll(struct blk_request *rq, struct io_comp_batch *iob,
		unsigned int poll_flags)
{
	struct blk_queue *q;
	int ret;

	if (!rq || !blk_rq_is_poll(rq))
		return 0;
	q = rq->queue;
	if (!q || blk_queue_dying(q))
		return 0;
	blk_mq_poll_hybrid(q, rq);
	ret = blk_hctx_poll(q, rq->mq_hctx, iob, poll_flags);
	if (iob && iob->req_list && iob->complete)
		iob->complete(iob);
	return ret;
}

int blk_mq_poll(struct blk_queue *q, uint32_t cookie, struct io_comp_batch *iob,
		unsigned int poll_flags)
{
	int ret = 0;

	if (!q || !blk_queue_is_mq(q))
		return 0;
	if (!(q->limits.features & BLK_FEAT_POLL))
		return 0;
	if (!blk_mq_hctx_read_try(q))
		return 0;
	if (cookie < q->nr_hw_queues)
		ret = blk_hctx_poll(q, q->queue_hw_ctx[cookie], iob, poll_flags);
	blk_mq_hctx_read_end(q);
	return ret;
}

void blk_mq_wait_poll(struct blk_request *rq)
{
	(void)blk_rq_poll(rq, NULL, BLK_POLL_ONESHOT);
}

/* ==========================================================================
 * Cancellation
 * ========================================================================== */

/* Unlink `rq' from a singly linked list through disp_next.  1 found. */
static int blk_mq_unlink(struct blk_request **head, struct blk_request **tail,
			 struct blk_request *rq)
{
	struct blk_request *prev = NULL;

	for (struct blk_request *r = *head; r; prev = r, r = r->disp_next) {
		if (r != rq)
			continue;
		if (prev)
			prev->disp_next = r->disp_next;
		else
			*head = r->disp_next;
		if (*tail == r)
			*tail = prev;
		r->disp_next = NULL;
		return 1;
	}
	return 0;
}

/* A request the driver has (or is being handed): ask the transport to
 * abort it.  Its end comes through the normal completion, so nothing here
 * can end it a second time; the count taken here keeps the request and
 * its tag for the call even if that completion races with us. */
static int blk_mq_cancel_started(struct blk_queue *q, struct blk_request *rq)
{
	const struct blk_mq_ops *ops = q->tag_set ? q->tag_set->ops : NULL;
	int rc;

	if (!ops || !ops->cancel)
		return -EBUSY;
	if (rq->tag < 0 || !refcount_inc_not_zero(&rq->mq_ref))
		return -EBUSY; /* no tag: it ended, or it is not at the driver */
	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT || blk_mq_rq_ended(rq)) {
		/* Being completed, or ended: nothing left to abort. */
		blk_mq_put_rq_ref(rq);
		return -EBUSY;
	}
	might_sleep();
	rc = ops->cancel(rq);
	blk_mq_put_rq_ref(rq);
	return rc;
}

int blk_mq_cancel_request(struct blk_request *rq)
{
	struct blk_queue *q;
	int found = 0;
	uint64_t fl;

	if (!rq || rq->magic != BLK_REQUEST_MAGIC)
		return -EINVAL;
	q = rq->queue;
	if (!q || !blk_queue_is_mq(q))
		return -EINVAL;
	if (blk_mq_request_started(rq) ||
	    (rq->state == BLK_RQ_DISPATCHED && rq->tag >= 0))
		return blk_mq_cancel_started(q, rq);
	if (rq->state == BLK_RQ_DISPATCHED)
		return -EBUSY;
	if (rq->bl.internal & BLK_RQI_MERGED)
		return -EBUSY;
	if (rq->bl.internal & BLK_RQI_MQ_CTX) {
		for (unsigned int c = 0; c < MAX_CPUS && !found; c++) {
			struct blk_mq_ctx *ctx = &q->queue_ctx[c];

			spin_lock_irqsave(&ctx->lock, &fl);
			for (unsigned int t = 0; t < HCTX_MAX_TYPES && !found;
			     t++)
				found = blk_mq_unlink(&ctx->rq_head[t],
						      &ctx->rq_tail[t], rq);
			if (found)
				rq->bl.internal &= ~BLK_RQI_MQ_CTX;
			spin_unlock_irqrestore(&ctx->lock, fl);
		}
	}
	if (!found && (rq->bl.internal & BLK_RQI_MQ_DISPATCH) && rq->mq_hctx) {
		struct blk_mq_hw_ctx *h = rq->mq_hctx;

		spin_lock_irqsave(&h->lock, &fl);
		found = blk_mq_unlink(&h->dispatch_head, &h->dispatch_tail, rq);
		if (found) {
			rq->bl.internal &= ~BLK_RQI_MQ_DISPATCH;
			__atomic_store_n(&h->nr_dispatch, h->nr_dispatch - 1,
					 __ATOMIC_RELEASE);
		}
		spin_unlock_irqrestore(&h->lock, fl);
	}
	if (!found && (rq->bl.internal & BLK_RQI_SORTED))
		found = blk_queue_elv_cancel(q, rq);
	if (!found && (rq->bl.internal & BLK_RQI_MQ_REQUEUE))
		found = blk_queue_requeue_cancel(q, rq);
	if (!found && (rq->bl.internal & BLK_RQI_ZONE_PLUGGED))
		found = blk_zone_cancel_plugged(rq);
	if (!found)
		return -EBUSY;
	blk_request_complete(rq, -ECANCELED, (uint32_t)blk_rq_payload_bytes(rq));
	return 0;
}

/* ==========================================================================
 * Counts and sysfs
 * ========================================================================== */

static int blk_mq_count_started(struct blk_request *rq, void *data)
{
	(void)rq;
	(*(unsigned int *)data)++;
	return 1;
}

unsigned int blk_mq_queue_inflight(struct blk_queue *q)
{
	unsigned int n = 0;

	if (!q || !blk_queue_is_mq(q))
		return 0;
	/* During a resize the queue is frozen and drained: 0. */
	if (!blk_mq_hctx_read_try(q))
		return 0;
	blk_mq_queue_tag_busy_iter(q, blk_mq_count_started, &n);
	blk_mq_hctx_read_end(q);
	return n;
}

unsigned int blk_mq_nr_at_driver(struct blk_queue *q)
{
	return blk_mq_queue_inflight(q);
}

/* mq/<n>/<attr>: 0 cpu_list, 1 nr_tags, 2 nr_reserved_tags, 3 dispatched
 * (queue_rq calls), 4 run, 5 busy, 6 tags_in_use, 7 dispatch_busy, 8
 * type, 9 poll stats, 10 timeouts handled, 11 requeued. */
long blk_mq_hctx_show(struct blk_queue *q, unsigned int idx, int attr,
		      char *buf, long cap)
{
	struct blk_mq_hw_ctx *h;
	long n = 0;

	if (!q || !blk_queue_is_mq(q) || cap <= 0)
		return -ENOENT;
	blk_mq_hctx_read_wait(q);
	if (idx >= q->nr_hw_queues) {
		blk_mq_hctx_read_end(q);
		return -ENOENT;
	}
	h = q->queue_hw_ctx[idx];
	switch (attr) {
	case 0: {
		int first = 1;

		for (unsigned int c = cpumask_first(&h->cpumask); c < MAX_CPUS;
		     c = cpumask_next((int)c, &h->cpumask)) {
			n += ksnprintf(buf + n, (size_t)(cap - n),
				       first ? "%u" : ", %u", c);
			first = 0;
			if (n >= cap)
				break;
		}
		if (n < cap)
			n += ksnprintf(buf + n, (size_t)(cap - n), "\n");
		break;
	}
	case 1:
		n = ksnprintf(buf, (size_t)cap, "%u\n", h->tags->nr_tags);
		break;
	case 2:
		n = ksnprintf(buf, (size_t)cap, "%u\n",
			      h->tags->nr_reserved_tags);
		break;
	case 3:
		n = ksnprintf(buf, (size_t)cap, "%llu\n",
			      (unsigned long long)h->nr_queue_rq);
		break;
	case 4:
		n = ksnprintf(buf, (size_t)cap, "%llu\n",
			      (unsigned long long)h->run);
		break;
	case 5:
		n = ksnprintf(buf, (size_t)cap, "%llu\n",
			      (unsigned long long)h->nr_busy);
		break;
	case 6:
		n = ksnprintf(buf, (size_t)cap, "%u\n", blk_mq_tags_busy(h->tags));
		break;
	case 7:
		n = ksnprintf(buf, (size_t)cap, "%u\n", h->dispatch_busy);
		break;
	case 8:
		n = ksnprintf(buf, (size_t)cap, "%s\n",
			      h->type == HCTX_TYPE_POLL ? "poll" :
			      h->type == HCTX_TYPE_READ ? "read" :
							  "default");
		break;
	case 9:
		n = ksnprintf(buf, (size_t)cap,
			      "considered=%llu\ninvoked=%llu\nsuccess=%llu\nmean_read_ns=%llu\nmean_write_ns=%llu\n",
			      (unsigned long long)h->poll_considered,
			      (unsigned long long)h->poll_invoked,
			      (unsigned long long)h->poll_success,
			      (unsigned long long)h->poll_stat_mean_ns[0],
			      (unsigned long long)h->poll_stat_mean_ns[1]);
		break;
	case 10:
		n = ksnprintf(buf, (size_t)cap, "%llu\n",
			      (unsigned long long)h->timeouts_handled);
		break;
	case 11:
		n = ksnprintf(buf, (size_t)cap, "%llu\n",
			      (unsigned long long)h->requeued);
		break;
	default:
		n = -ENOENT;
		break;
	}
	blk_mq_hctx_read_end(q);
	return n > cap ? cap : n;
}
