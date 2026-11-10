// LikeOS - the per-disk request queue: elevator, merging, plugging,
// dispatch, completion accounting and the /proc/diskstats counters.
//
// Every extended disk has one struct blk_queue (blkdev.h).  blk_submit()
// validates a request and calls blk_queue_insert(); from there:
//
//   * DISPATCH LIST: flushes, pass-through commands, requests flagged HEAD
//     or PM, requests given back by the driver (an error-handling requeue,
//     a further step) and requests a dispatcher took but could not hand
//     over go on a plain ordered list that is always served first.  They
//     are never sorted or merged: their order IS their meaning.
//
//   * ELEVATOR (deadline policy): ordinary reads and writes go on two
//     lists per direction -- one sorted by sector, one in arrival order
//     with an expiry time (read_expire_ms / write_expire_ms).  Dispatch
//     sweeps the sorted list upwards from where the last request ended,
//     in batches of fifo_batch requests of one direction; a new batch
//     prefers reads, but not more than writes_starved times in a row
//     while writes wait; and whenever the oldest request of the chosen
//     direction has expired, the batch starts with it instead.  That is
//     the classic deadline scheduler: seeks stay short, nothing waits for
//     ever, reads (which someone is usually blocked on) are favoured.
//     With the "none" scheduler the elevator is plain arrival order and a
//     request that finds the queue idle goes to the driver directly.
//
//   * MERGING: a request whose sectors continue (back merge) or precede
//     (front merge) a queued request of the same kind is absorbed by it:
//     the queued one -- the carrier -- grows to cover both and carries the
//     other's buffers on its scatter/gather list; when it completes, every
//     absorbed request is completed with the part of the result that
//     concerns it (block.c).  A carrier borrows its widened sg array from
//     a per-queue pool, so merging never allocates under the lock, and
//     gets its own list and range back on completion.
//
//   * PLUGGING (per task): a task that submits a batch opens a plug; its
//     requests collect there and reach the queues together -- when it
//     finishes the plug, when the plug is full, or when the task sleeps.
//
//   * FLUSHES: PREFLUSH / FUA writes and FLUSH requests run through the
//     flush machinery: one flush of the queue's own serves every request
//     waiting for one.
//
//   * DISPATCH: a request for the elevator is inserted by its submitter
//     and the queue is run by kblockd on the submitting CPU; a submitter
//     that finishes its plug, a pass-through command and a cache flush run
//     the queue themselves.  A run takes the dispatch list first, then the
//     elevator's choice, gives each request a TAG (one of `depth', the
//     device's command queue depth: NCQ) and hands it to the driver
//     (ops->submit) WITHOUT any queue lock.  Several CPUs may dispatch at
//     once: the choice of the next request is serialised by the queue
//     lock, the tags by an atomic bitmap.  No tag free: dispatch stops and
//     marks the queue for a RESTART -- the completion that frees a tag
//     runs the queue again through kblockd, never from the completion
//     itself.  -EBUSY from the driver puts the request on the dispatch
//     list; the queue is run again at once, or, when a completion will
//     restart it anyway and the driver ran out of a resource, after
//     BLK_RESOURCE_DELAY_NS.  A run wanted where a task may not dispatch
//     -- a hard interrupt, the block softirq, a timer -- is handed to
//     kblockd (blk_work.c).
//
//   * TIMEOUTS: a dispatched request has a deadline, kept in its tag's
//     slot; one coarse timer per queue and a scan of the busy tags by
//     kblockd hand expired requests to the driver's ->timeout (its error
//     handling) without ever touching a request that may have completed.
//
//   * ACCOUNTING: per device (the disk and each partition) the counters
//     /proc/diskstats and /sys/block/<dev>/stat report -- completed
//     requests, merges, 512-byte sectors and milliseconds per group (read,
//     write, discard, flush), requests in flight, the time with something
//     in flight and the summed request time -- kept per CPU and summed by
//     the readers.
//
// Locking: q->lock is a leaf spinlock (irqsave) for the elevator and the
// dispatch list, taken by submitters and dispatchers only.  The
// completion of a request takes no queue lock at all: its tag goes back
// with an atomic operation, the statistics are per CPU, the restart is an
// atomic bit, and the elevator only learns of it through an atomic count.
// Requests given back from a completion go on the requeue list (its own
// small lock) and are inserted by kblockd; the flush machinery has its own
// lock.  Waiters (room under nr_requests, drain, quiesce) count
// themselves in q->nr_waiters before they look at their condition and
// sleep under q->wait_lock; whatever may satisfy them looks at the count
// after its change and wakes them under the same lock.  No queue lock is
// ever held across a call into the driver or into an end_io callback.
//
// The kernel's spinlocks do not stop preemption by themselves (and on a
// uniprocessor are only an interrupts-off section), so every lock here is
// taken with interrupts disabled even where no interrupt handler takes it.
//
// The pure parts -- the merge predicates, the elevator's choice -- take
// no lock themselves and run on the host in the boxed tests.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Merge pool geometry: arrays of at most this many sg elements, at most
 * this many arrays per queue (the bitmap is one uint64_t). */
#define BLK_MERGE_POOL_SEG_MAX 128
#define BLK_MERGE_POOL_N_MAX 16
#define BLK_MERGE_POOL_N_MIN 4

/* The decaying average of "the driver was busy" (dispatch_busy): each
 * dispatch moves it by 1/WEIGHT towards 0 or towards 1 << FACTOR. */
#define BLK_DISPATCH_BUSY_EWMA_WEIGHT 8
#define BLK_DISPATCH_BUSY_EWMA_FACTOR 4

/* A dispatcher that took a tag and then found nothing to dispatch has the
 * queue run again after this long. */
#define BLK_BUDGET_DELAY_NS (3ULL * 1000ULL * 1000ULL)

/* The queue's request slots by default: twice the dispatch depth, the
 * depth counted up to BLK_DEFAULT_NR_REQUESTS. */
static inline uint32_t blk_default_nr_requests(uint32_t depth)
{
	if (depth == 0)
		depth = 1;
	return 2 * (depth < BLK_DEFAULT_NR_REQUESTS ? depth :
						      BLK_DEFAULT_NR_REQUESTS);
}

/* A run that keeps finding requests goes on for at most this long (or
 * until the task should give the processor up); the rest of the queue is
 * left to kblockd. */
#define BLK_DISPATCH_MAX_NS (1000ULL * 1000ULL * 1000ULL)

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static inline int blk_rq_dir(const struct blk_request *req)
{
	return blk_op_is_write(req->op) ? 1 : 0;
}

/* The op a request is accounted under. */
static inline enum blk_op blk_rq_acct_op(const struct blk_request *req)
{
	return req->op;
}

int blk_stat_group(enum blk_op op)
{
	switch (op) {
	case BLK_OP_READ:
		return BLK_STAT_READ;
	case BLK_OP_WRITE:
	case BLK_OP_WRITE_ZEROES:
		return BLK_STAT_WRITE;
	case BLK_OP_DISCARD:
	case BLK_OP_SECURE_ERASE:
		return BLK_STAT_DISCARD;
	case BLK_OP_FLUSH:
		return BLK_STAT_FLUSH;
	default:
		return -1; /* pass-through is not disk I/O statistics */
	}
}

static inline uint64_t blk_ns_to_ms(uint64_t ns)
{
	return ns / 1000000ULL;
}

/* Rounded up: a deadline kept in milliseconds never comes early. */
static inline uint64_t blk_ns_to_ms_up(uint64_t ns)
{
	return (ns + 999999ULL) / 1000000ULL;
}

/* The statistics of a request's own device when that is a partition
 * (the disk's are always updated as well). */
static inline struct blk_part_stats *blk_rq_part_stats(struct blk_request *req)
{
	block_device_t *b = req->bdev;

	if (b && blk_bdev_is_extended(b) && b->part)
		return &b->part->stats;
	return NULL;
}

/* The queue's flags are changed with atomic operations under the queue
 * lock (the sysfs nomerges switch changes its bit under the lock with a
 * plain update) and read without it on the hot paths. */
static inline uint32_t blk_qflags(const struct blk_queue *q)
{
	return __atomic_load_n(&q->flags, __ATOMIC_ACQUIRE);
}

static inline void blk_qflags_set(struct blk_queue *q, uint32_t f)
{
	(void)__atomic_fetch_or(&q->flags, f, __ATOMIC_SEQ_CST);
}

static inline void blk_qflags_clear(struct blk_queue *q, uint32_t f)
{
	(void)__atomic_fetch_and(&q->flags, ~f, __ATOMIC_SEQ_CST);
}

static inline uint32_t blk_ld32(const volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* A count that is written under the queue lock and read without it: one
 * store of the new value, so a reader never sees half of an update. */
static inline void blk_cnt_inc_locked(volatile uint32_t *p)
{
	__atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + 1,
			 __ATOMIC_RELEASE);
}

static inline void blk_cnt_dec_locked(volatile uint32_t *p)
{
	uint32_t v = __atomic_load_n(p, __ATOMIC_RELAXED);

	WARN_ON(v == 0);
	if (v)
		__atomic_store_n(p, v - 1, __ATOMIC_RELEASE);
}

/* An atomic count that must not wrap below zero. */
static inline void blk_atomic_dec_nonzero(volatile uint32_t *p)
{
	uint32_t v = __atomic_load_n(p, __ATOMIC_RELAXED);

	do {
		if (WARN_ON(v == 0))
			return;
	} while (!__atomic_compare_exchange_n(p, &v, v - 1, 0,
					      __ATOMIC_SEQ_CST,
					      __ATOMIC_RELAXED));
}

/* ==========================================================================
 * Statistics accounting: per-CPU counters
 *
 * A CPU adds to its own slot of the device's counters with interrupts off
 * (no lock, nothing shared with the CPU that completes the request); a
 * reader sums the slots.  The busy time follows one stamp per device,
 * advanced with a compare-and-exchange: whoever moves it adds the interval
 * -- at the end of a request always, at the start only when something was
 * in flight during it.
 * ========================================================================== */

/* The slot of the calling CPU (slot 0 before per-CPU data is known). */
static inline uint32_t blk_stat_slot(void)
{
	int cpu = blk_this_cpu();

	return (cpu < 0 || cpu >= BLK_STAT_CPUS) ? 0u : (uint32_t)cpu;
}

static inline void blk_pcpu_add(uint64_t *p, uint64_t v)
{
	__atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + v,
			 __ATOMIC_RELAXED);
}

static inline void blk_pcpu_add_s(int64_t *p, int64_t v)
{
	__atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + v,
			 __ATOMIC_RELAXED);
}

/* Requests of the device in flight, per direction: the sum over the CPUs
 * (one CPU's count may be negative -- started here, completed there). */
static void blk_part_inflight(const struct blk_part_stats *s, uint64_t out[2])
{
	int64_t r = 0, w = 0;

	for (uint32_t i = 0; i < BLK_STAT_CPUS; i++) {
		r += __atomic_load_n(&s->cpu[i].in_flight[0], __ATOMIC_RELAXED);
		w += __atomic_load_n(&s->cpu[i].in_flight[1], __ATOMIC_RELAXED);
	}
	out[0] = r > 0 ? (uint64_t)r : 0;
	out[1] = w > 0 ? (uint64_t)w : 0;
}

/* Move the device's busy-time stamp to `now_ms'; the CPU that moves it
 * adds the interval to its own slot -- at the end of a request always, at
 * the start of one only when the device was busy meanwhile.  Interrupts
 * off. */
static void blk_update_io_ticks(struct blk_part_stats *s, uint32_t slot,
				uint64_t now_ms, int end)
{
	uint64_t stamp = __atomic_load_n(&s->stamp_ms, __ATOMIC_RELAXED);

	if (now_ms > stamp &&
	    __atomic_compare_exchange_n(&s->stamp_ms, &stamp, now_ms, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
		uint64_t inflight[2] = { 0, 0 };

		if (!end)
			blk_part_inflight(s, inflight);
		if (end || inflight[0] + inflight[1])
			blk_pcpu_add(&s->cpu[slot].io_ticks_ms, now_ms - stamp);
	}
}

/* A request starts counting (at insertion): the busy time is brought up
 * to now, the request is in flight. */
static void blk_account_start(struct blk_queue *q, struct blk_request *rq,
			      uint64_t now)
{
	enum blk_op op = blk_rq_acct_op(rq);
	int dir = blk_op_is_write(op) ? 1 : 0;
	struct blk_part_stats *ps, *ds;
	uint64_t fl, ms;
	uint32_t slot;

	if ((rq->flags & BLK_REQ_NO_ACCOUNT) || blk_stat_group(op) < 0)
		return;
	if (rq->bl.internal & BLK_RQI_ACCT)
		return;
	ps = blk_rq_part_stats(rq);
	ds = q->disk ? &q->disk->stats : NULL;
	ms = blk_ns_to_ms(now);
	fl = local_irq_save();
	slot = blk_stat_slot();
	if (ps)
		blk_update_io_ticks(ps, slot, ms, 0);
	if (ds)
		blk_update_io_ticks(ds, slot, ms, 0);
	if (ps)
		blk_pcpu_add_s(&ps->cpu[slot].in_flight[dir], 1);
	if (ds)
		blk_pcpu_add_s(&ds->cpu[slot].in_flight[dir], 1);
	local_irq_restore(fl);
	rq->bl.internal |= BLK_RQI_ACCT;
	(void)__atomic_add_fetch(&q->nr_accounted, 1, __ATOMIC_RELAXED);
}

/* A request stopped being a request of its own: it was merged into
 * another.  One merge more, one request less in flight. */
static void blk_account_merge(struct blk_queue *q, struct blk_request *absorbed)
{
	enum blk_op op = blk_rq_acct_op(absorbed);
	int g = blk_stat_group(op);
	int dir = blk_op_is_write(op) ? 1 : 0;
	struct blk_part_stats *ps, *ds;
	uint64_t fl;
	uint32_t slot;

	if (!(absorbed->bl.internal & BLK_RQI_ACCT))
		return;
	absorbed->bl.internal &= ~BLK_RQI_ACCT;
	blk_atomic_dec_nonzero(&q->nr_accounted);
	ps = blk_rq_part_stats(absorbed);
	ds = q->disk ? &q->disk->stats : NULL;
	fl = local_irq_save();
	slot = blk_stat_slot();
	if (ps) {
		if (g >= 0)
			blk_pcpu_add(&ps->cpu[slot].merges[g], 1);
		blk_pcpu_add_s(&ps->cpu[slot].in_flight[dir], -1);
	}
	if (ds) {
		if (g >= 0)
			blk_pcpu_add(&ds->cpu[slot].merges[g], 1);
		blk_pcpu_add_s(&ds->cpu[slot].in_flight[dir], -1);
	}
	local_irq_restore(fl);
}

/* A request completed: from the snapshot taken before its end_io ran
 * (the request itself may be gone by now). */
static void blk_account_done_one(struct blk_part_stats *st, uint32_t slot,
				 const struct blk_done_snap *s, uint64_t ms)
{
	blk_update_io_ticks(st, slot, ms, 1);
	if (s->group >= 0) {
		blk_pcpu_add(&st->cpu[slot].ios[s->group], 1);
		blk_pcpu_add(&st->cpu[slot].sectors[s->group], s->sect512);
		blk_pcpu_add(&st->cpu[slot].nsecs[s->group],
			     s->now_ns > s->start_ns ? s->now_ns - s->start_ns :
						       0);
	}
	blk_pcpu_add_s(&st->cpu[slot].in_flight[s->dir], -1);
}

static void blk_account_done(struct blk_queue *q, const struct blk_done_snap *s)
{
	uint64_t fl, ms = blk_ns_to_ms(s->now_ns);
	uint32_t slot;

	fl = local_irq_save();
	slot = blk_stat_slot();
	if (s->part_stats)
		blk_account_done_one(s->part_stats, slot, s, ms);
	if (s->disk_stats)
		blk_account_done_one(s->disk_stats, slot, s, ms);
	local_irq_restore(fl);
	blk_atomic_dec_nonzero(&q->nr_accounted);
}

/* ==========================================================================
 * Merging -- pure predicates (host-testable)
 * ========================================================================== */

/* Request flags that forbid merging, and flags that must agree for two
 * requests to become one. */
#define BLK_REQ_NOMERGE_MASK                                             \
	(BLK_REQ_NOMERGE | BLK_REQ_HEAD | BLK_REQ_PM | BLK_REQ_PREFLUSH | \
	 BLK_REQ_POLLED)
#define BLK_REQ_MERGE_MATCH_MASK                                 \
	(BLK_REQ_FUA | BLK_REQ_FAILFAST | BLK_REQ_NO_ACCOUNT |    \
	 BLK_REQ_NOUNMAP)

static int blk_rq_mergeable(const struct blk_request *r)
{
	if (r->flags & BLK_REQ_NOMERGE_MASK)
		return 0;
	if (r->drv_cmd)
		return 0;
	if (r->bl.seq)
		return 0;
	if (r->state == BLK_RQ_DISPATCHED || r->state == BLK_RQ_COMPLETE)
		return 0;
	switch (r->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE:
	case BLK_OP_DISCARD:
		return 1;
	default:
		return 0;
	}
}

uint32_t blk_sg_segments(const struct blk_sg_entry *e, uint32_t off,
			 uint32_t len, const struct blk_limits *lim)
{
	uint32_t seg_max = lim && lim->max_segment_size ?
				   lim->max_segment_size :
				   65536;

	if (!len)
		return 0;
	/* A physically contiguous range: one segment per max_segment_size;
	 * memory known only by its virtual address: one per page touched. */
	if (e->phys)
		return (len + seg_max - 1) / seg_max;
	{
		uint64_t start = (uint64_t)(unsigned long)e->virt + off;

		return (uint32_t)(((start & 4095ULL) + len + 4095ULL) >> 12);
	}
}

uint32_t blk_rq_nr_segments(const struct blk_request *rq,
			    const struct blk_limits *lim)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < rq->nsg; i++)
		n += blk_sg_segments(&rq->sg[i], 0, rq->sg[i].len, lim);
	return n;
}

/* Everything both directions of a merge check have in common.  Returns the
 * limit on the combined sector count, 0 when they cannot merge at all.
 * The same limits the submission split enforces: a merged request never
 * needs more than one command of the driver. */
static uint64_t blk_rq_merge_common(const struct blk_request *a,
				    const struct blk_request *b,
				    const struct blk_limits *lim)
{
	uint64_t max;

	if (!a || !b || !lim || a == b)
		return 0;
	if (!blk_rq_mergeable(a) || !blk_rq_mergeable(b))
		return 0;
	if (a->op != b->op)
		return 0;
	/* The same device: statistics and read-only checks are per device,
	 * and the queue (the disk) must be the same in any case. */
	if (a->bdev != b->bdev)
		return 0;
	if (a->queue && b->queue && a->queue != b->queue)
		return 0;
	if ((a->flags ^ b->flags) & BLK_REQ_MERGE_MATCH_MASK)
		return 0;
	if (a->count == 0 || b->count == 0)
		return 0;
	if (a->op == BLK_OP_DISCARD) {
		/* Contiguous discards stay one range; any multi-range limit
		 * of at least one therefore allows it. */
		max = lim->max_discard_sectors;
		if (!max)
			return 0;
	} else {
		uint32_t segs = lim->max_segments ? lim->max_segments :
						    BLK_DEFAULT_MAX_SEGMENTS;

		max = lim->max_sectors ? lim->max_sectors :
					 lim->max_hw_sectors;
		if (!max)
			max = 0xFFFFFFFFULL;
		if (!a->sg || !b->sg || !a->nsg || !b->nsg)
			return 0;
		if ((uint64_t)blk_rq_nr_segments(a, lim) +
			    blk_rq_nr_segments(b, lim) >
		    segs)
			return 0;
	}
	if (max > 0xFFFFFFFFULL)
		max = 0xFFFFFFFFULL;
	if ((uint64_t)a->count + b->count > max)
		return 0;
	return max;
}

int blk_rq_can_back_merge(const struct blk_request *a,
			  const struct blk_request *b,
			  const struct blk_limits *lim)
{
	if (!blk_rq_merge_common(a, b, lim))
		return 0;
	/* b continues a: a's last sector + 1 is b's first. */
	if (a->sector + a->count < a->sector)
		return 0;
	return a->sector + a->count == b->sector;
}

int blk_rq_can_front_merge(const struct blk_request *a,
			   const struct blk_request *b,
			   const struct blk_limits *lim)
{
	if (!blk_rq_merge_common(a, b, lim))
		return 0;
	/* b ends where a begins. */
	if (b->sector + b->count < b->sector)
		return 0;
	return b->sector + b->count == a->sector;
}

/* ==========================================================================
 * List primitives (queue lock held)
 * ========================================================================== */

static void blk_sort_insert_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);
	struct blk_request *pos = q->sort_head[dir], *prev = NULL;

	lockdep_assert_held(&q->lock);
	/* Ascending sector; an equal sector goes after the existing ones so
	 * same-sector requests keep their order. */
	while (pos && pos->sector <= rq->sector) {
		prev = pos;
		pos = pos->sort_next;
	}
	rq->sort_prev = prev;
	rq->sort_next = pos;
	if (prev)
		prev->sort_next = rq;
	else
		q->sort_head[dir] = rq;
	if (pos)
		pos->sort_prev = rq;
}

static void blk_sort_remove_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);

	lockdep_assert_held(&q->lock);
	if (rq->sort_prev)
		rq->sort_prev->sort_next = rq->sort_next;
	else
		q->sort_head[dir] = rq->sort_next;
	if (rq->sort_next)
		rq->sort_next->sort_prev = rq->sort_prev;
	rq->sort_next = rq->sort_prev = NULL;
}

static void blk_fifo_add_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);

	lockdep_assert_held(&q->lock);
	rq->fifo_next = NULL;
	rq->fifo_prev = q->fifo_tail[dir];
	if (q->fifo_tail[dir])
		q->fifo_tail[dir]->fifo_next = rq;
	else
		q->fifo_head[dir] = rq;
	q->fifo_tail[dir] = rq;
}

static void blk_fifo_remove_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);

	lockdep_assert_held(&q->lock);
	if (rq->fifo_prev)
		rq->fifo_prev->fifo_next = rq->fifo_next;
	else
		q->fifo_head[dir] = rq->fifo_next;
	if (rq->fifo_next)
		rq->fifo_next->fifo_prev = rq->fifo_prev;
	else
		q->fifo_tail[dir] = rq->fifo_prev;
	rq->fifo_next = rq->fifo_prev = NULL;
}

/* Replace `old' by `rq' at old's place in the FIFO (a carrier inheriting
 * the earlier deadline of a request it absorbed). */
static void blk_fifo_replace_locked(struct blk_queue *q, struct blk_request *old,
				    struct blk_request *rq)
{
	int dir = blk_rq_dir(old);

	lockdep_assert_held(&q->lock);
	blk_fifo_remove_locked(q, rq);
	rq->fifo_prev = old->fifo_prev;
	rq->fifo_next = old->fifo_next;
	if (old->fifo_prev)
		old->fifo_prev->fifo_next = rq;
	else
		q->fifo_head[dir] = rq;
	if (old->fifo_next)
		old->fifo_next->fifo_prev = rq;
	else
		q->fifo_tail[dir] = rq;
	old->fifo_next = old->fifo_prev = NULL;
}

static void blk_elv_add_locked(struct blk_queue *q, struct blk_request *rq,
			       uint64_t now)
{
	int dir = blk_rq_dir(rq);
	uint32_t expire = dir ? q->write_expire_ms : q->read_expire_ms;

	rq->fifo_deadline_ns = now + (uint64_t)expire * 1000000ULL;
	blk_sort_insert_locked(q, rq);
	blk_fifo_add_locked(q, rq);
	rq->bl.internal |= BLK_RQI_SORTED | BLK_RQI_ELV;
	q->elv_inserted++;
	blk_cnt_inc_locked(&q->nr_queued[dir]);
}

static void blk_elv_remove_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);

	blk_sort_remove_locked(q, rq);
	blk_fifo_remove_locked(q, rq);
	rq->bl.internal &= ~BLK_RQI_SORTED;
	blk_cnt_dec_locked(&q->nr_queued[dir]);
}

static void blk_bypass_add_locked(struct blk_queue *q, struct blk_request *rq,
				  int at_head)
{
	lockdep_assert_held(&q->lock);
	rq->disp_next = NULL;
	if (at_head) {
		rq->disp_next = q->disp_head;
		q->disp_head = rq;
		if (!q->disp_tail)
			q->disp_tail = rq;
	} else {
		if (q->disp_tail)
			q->disp_tail->disp_next = rq;
		else
			q->disp_head = rq;
		q->disp_tail = rq;
	}
	rq->bl.internal |= BLK_RQI_BYPASS;
	blk_cnt_inc_locked(&q->nr_bypass);
}

/* Unlink `rq' from the bypass list given its predecessor (NULL = head). */
static void blk_bypass_unlink_locked(struct blk_queue *q,
				     struct blk_request *prev,
				     struct blk_request *rq)
{
	lockdep_assert_held(&q->lock);
	if (prev)
		prev->disp_next = rq->disp_next;
	else
		q->disp_head = rq->disp_next;
	if (q->disp_tail == rq)
		q->disp_tail = prev;
	rq->disp_next = NULL;
	rq->bl.internal &= ~BLK_RQI_BYPASS;
	blk_cnt_dec_locked(&q->nr_bypass);
}

/* Requests waiting in the queue (elevator and dispatch list).  Read
 * without the lock by the waiters' conditions. */
static inline uint32_t blk_queue_nr_queued(const struct blk_queue *q)
{
	return blk_ld32(&q->nr_queued[0]) + blk_ld32(&q->nr_queued[1]) +
	       blk_ld32(&q->nr_bypass);
}

/* A request's queue slot goes back (Waiting on a queue, below). */
static void blk_sched_tag_put(struct blk_queue *q, struct blk_request *rq);

/* ==========================================================================
 * The merge pool (queue lock held)
 * ========================================================================== */

/* Take a pool array.  Only merging (under the queue lock) takes them; a
 * completion gives one back without the lock (blk_merge_slot_put), so
 * the bitmap changes with atomic operations. */
static int blk_merge_slot_get_locked(struct blk_queue *q)
{
	lockdep_assert_held(&q->lock);
	for (uint32_t i = 0; i < q->merge_pool_n; i++) {
		uint64_t bit = 1ULL << i;

		if (__atomic_load_n(&q->merge_pool_used, __ATOMIC_ACQUIRE) & bit)
			continue;
		if (!(__atomic_fetch_or(&q->merge_pool_used, bit,
					__ATOMIC_ACQ_REL) &
		      bit))
			return (int)i;
	}
	return -1;
}

/* Give a pool array back.  Any context, no lock. */
static void blk_merge_slot_put(struct blk_queue *q, int slot)
{
	uint64_t old;

	if (slot < 0)
		return;
	BUG_ON((uint32_t)slot >= q->merge_pool_n);
	old = __atomic_fetch_and(&q->merge_pool_used, ~(1ULL << slot),
				 __ATOMIC_RELEASE);
	WARN_ON(!(old & (1ULL << slot)));
}

static struct blk_sg_entry *blk_merge_slot_array(struct blk_queue *q, int slot)
{
	return q->merge_pool + (size_t)slot * q->merge_pool_seg;
}

/* Remember a request's own state before its first merge.  For a data
 * request the sg list moves into a pool array.  0, or -1 when no pool
 * array is free (no merge then). */
static int blk_carrier_prepare_locked(struct blk_queue *q,
				      struct blk_request *a)
{
	if (a->bl.internal & BLK_RQI_CARRIER)
		return 0;
	if (blk_op_has_data(a->op)) {
		int slot;
		struct blk_sg_entry *arr;

		if (!q->merge_pool || a->nsg > q->merge_pool_seg)
			return -1;
		slot = blk_merge_slot_get_locked(q);
		if (slot < 0)
			return -1;
		arr = blk_merge_slot_array(q, slot);
		for (uint32_t i = 0; i < a->nsg; i++)
			arr[i] = a->sg[i];
		a->bl.own_sg = a->sg;
		a->bl.own_nsg = a->nsg;
		a->bl.own_sg_cap = a->sg_cap;
		a->sg = arr;
		a->sg_cap = q->merge_pool_seg;
		a->bl.merge_slot = slot;
	} else {
		a->bl.own_sg = a->sg;
		a->bl.own_nsg = a->nsg;
		a->bl.own_sg_cap = a->sg_cap;
		a->bl.merge_slot = -1;
	}
	a->bl.own_count = a->count;
	a->bl.own_lba = a->lba;
	a->bl.own_sector = a->sector;
	a->bl.internal |= BLK_RQI_CARRIER;
	return 0;
}

void blk_rq_restore_carrier(struct blk_request *req)
{
	if (!(req->bl.internal & BLK_RQI_CARRIER))
		return;
	req->sg = req->bl.own_sg;
	req->nsg = req->bl.own_nsg;
	req->sg_cap = req->bl.own_sg_cap;
	req->count = req->bl.own_count;
	req->lba = req->bl.own_lba;
	req->sector = req->bl.own_sector;
	req->bl.merge_slot = -1;
	req->bl.internal &= ~BLK_RQI_CARRIER;
}

/* `b' (no longer on any list) becomes part of `a'.  `front': b's sectors
 * precede a's.  The predicate has been checked. */
static int blk_merge_absorb_locked(struct blk_queue *q, struct blk_request *a,
				   struct blk_request *b, int front)
{
	struct blk_request *tail;

	lockdep_assert_held(&q->lock);
	if (blk_op_has_data(a->op) &&
	    a->nsg + b->nsg > (q->merge_pool ? q->merge_pool_seg : 0))
		return 0;
	if (blk_carrier_prepare_locked(q, a))
		return 0;
	/* b may itself be a carrier: its merged list joins a's, its own
	 * state comes back (its pool array is copied below first). */
	if (blk_op_has_data(a->op)) {
		if (front) {
			for (int i = (int)a->nsg - 1; i >= 0; i--)
				a->sg[i + b->nsg] = a->sg[i];
			for (uint32_t i = 0; i < b->nsg; i++)
				a->sg[i] = b->sg[i];
		} else {
			for (uint32_t i = 0; i < b->nsg; i++)
				a->sg[a->nsg + i] = b->sg[i];
		}
		a->nsg += b->nsg;
	}
	if (front) {
		a->sector = b->sector;
		a->lba = b->lba;
	}
	a->count += b->count;

	/* a's merged list: a, then everything already there, then b and
	 * b's own carried requests. */
	{
		struct blk_request *bchildren = b->merged_next;
		uint32_t bmerged = b->nr_merged;

		b->merged_next = NULL;
		b->nr_merged = 0;
		if (b->bl.internal & BLK_RQI_CARRIER) {
			blk_merge_slot_put(q, b->bl.merge_slot);
			blk_rq_restore_carrier(b);
		}
		b->merged_next = bchildren;
		tail = a;
		while (tail->merged_next)
			tail = tail->merged_next;
		tail->merged_next = b;
		a->nr_merged += 1 + bmerged;
	}
	b->bl.internal |= BLK_RQI_MERGED;
	b->state = BLK_RQ_QUEUED;
	/* It is no longer a request of the queue's own: its slot goes back
	 * (the caller wakes the waiters after dropping the lock). */
	blk_sched_tag_put(q, b);
	blk_account_merge(q, b);
	if (front)
		q->merges_front++;
	else
		q->merges_back++;
	return 1;
}

/* Would absorbing b into a work as far as the merge pool is concerned?
 * (The predicate has said yes already.)  Checked before a queued request
 * is taken off its lists, so a merge that cannot happen leaves it exactly
 * where it was, deadline included. */
static int blk_merge_feasible_locked(struct blk_queue *q,
				     const struct blk_request *a,
				     const struct blk_request *b)
{
	if (!blk_op_has_data(a->op))
		return 1;
	if (!q->merge_pool)
		return 0;
	if (a->nsg + b->nsg > q->merge_pool_seg)
		return 0;
	if (a->bl.internal & BLK_RQI_CARRIER)
		return 1;
	/* a needs a pool array of its own; b, if it is a carrier, gives
	 * its array back only after the copy. */
	return __atomic_load_n(&q->merge_pool_used, __ATOMIC_ACQUIRE) !=
	       (q->merge_pool_n >= 64 ? ~0ULL :
					((1ULL << q->merge_pool_n) - 1));
}

/* After a merge the carrier may now touch its neighbours in sector order:
 * absorb the successor, or let the predecessor absorb the carrier.  At
 * most one follow-up each way, which is all a single new request can
 * make possible. */
static void blk_merge_neighbours_locked(struct blk_queue *q,
					struct blk_request *a)
{
	struct blk_request *s = a->sort_next;
	struct blk_request *p;

	if (s && blk_rq_can_back_merge(a, s, &q->limits) &&
	    blk_merge_feasible_locked(q, a, s)) {
		/* a takes over s's FIFO place when s is the older one. */
		if (s->fifo_deadline_ns < a->fifo_deadline_ns) {
			a->fifo_deadline_ns = s->fifo_deadline_ns;
			blk_fifo_replace_locked(q, s, a);
			blk_sort_remove_locked(q, s);
			s->bl.internal &= ~BLK_RQI_SORTED;
			blk_cnt_dec_locked(&q->nr_queued[blk_rq_dir(s)]);
		} else {
			blk_elv_remove_locked(q, s);
		}
		if (!blk_merge_absorb_locked(q, a, s, 0))
			WARN(1, "feasible merge failed");
		return; /* a changed; one follow-up is enough */
	}
	p = a->sort_prev;
	if (p && blk_rq_can_back_merge(p, a, &q->limits) &&
	    blk_merge_feasible_locked(q, p, a)) {
		if (a->fifo_deadline_ns < p->fifo_deadline_ns) {
			p->fifo_deadline_ns = a->fifo_deadline_ns;
			blk_fifo_replace_locked(q, a, p);
			blk_sort_remove_locked(q, a);
			a->bl.internal &= ~BLK_RQI_SORTED;
			blk_cnt_dec_locked(&q->nr_queued[blk_rq_dir(a)]);
		} else {
			blk_elv_remove_locked(q, a);
		}
		if (!blk_merge_absorb_locked(q, p, a, 0))
			WARN(1, "feasible merge failed");
	}
}

/* Try to merge a new request into a queued one.  1 when it was absorbed. */
static int blk_try_merge_locked(struct blk_queue *q, struct blk_request *rq)
{
	int dir = blk_rq_dir(rq);

	lockdep_assert_held(&q->lock);
	if (blk_qflags(q) & BLK_QUEUE_FL_NOMERGES)
		return 0;
	if (!blk_rq_mergeable(rq))
		return 0;
	for (struct blk_request *a = q->sort_head[dir]; a; a = a->sort_next) {
		if (a->sector > rq->sector + rq->count)
			break; /* sorted: nothing further can touch rq */
		if (blk_rq_can_back_merge(a, rq, &q->limits)) {
			if (!blk_merge_absorb_locked(q, a, rq, 0))
				return 0;
			blk_merge_neighbours_locked(q, a);
			return 1;
		}
		if (blk_rq_can_front_merge(a, rq, &q->limits)) {
			if (!blk_merge_absorb_locked(q, a, rq, 1))
				return 0;
			/* a's start moved down: keep the sort order exact */
			blk_sort_remove_locked(q, a);
			blk_sort_insert_locked(q, a);
			blk_merge_neighbours_locked(q, a);
			return 1;
		}
	}
	return 0;
}

/* ==========================================================================
 * The elevator's choice (queue lock held)
 * ========================================================================== */

/* The first request of direction `dir' at or after the sector where the
 * last dispatch in that direction ended (the sweep's next stop). */
static struct blk_request *blk_elv_next_locked(struct blk_queue *q, int dir)
{
	for (struct blk_request *r = q->sort_head[dir]; r; r = r->sort_next)
		if (r->sector >= q->next_sector[dir])
			return r;
	return NULL;
}

static int blk_fifo_expired_locked(struct blk_queue *q, int dir, uint64_t now)
{
	struct blk_request *h = q->fifo_head[dir];

	return h && h->fifo_deadline_ns <= now;
}

static struct blk_request *blk_pick_elevator_locked(struct blk_queue *q,
						    uint64_t now)
{
	struct blk_request *rq;
	int dir;

	if (q->sched == BLK_SCHED_NONE) {
		struct blk_request *r = q->fifo_head[0], *w = q->fifo_head[1];

		if (r && w)
			rq = r->submit_ns <= w->submit_ns ? r : w;
		else
			rq = r ? r : w;
		if (rq) {
			dir = blk_rq_dir(rq);
			q->batch_dir = dir;
			q->batching = 0;
		}
		return rq;
	}

	/* Continue the running batch while it has a next stop. */
	dir = q->batch_dir;
	rq = q->nr_queued[dir] ? blk_elv_next_locked(q, dir) : NULL;
	if (rq && q->batching < q->fifo_batch && !blk_fifo_expired_locked(q, dir, now))
		goto dispatch;

	/* A new batch: reads first, unless writes have been passed over
	 * writes_starved times already. */
	if (q->nr_queued[0]) {
		if (q->nr_queued[1] && q->starved++ >= q->writes_starved)
			goto dispatch_writes;
		dir = 0;
		goto find_request;
	}
	if (q->nr_queued[1]) {
dispatch_writes:
		q->starved = 0;
		dir = 1;
		goto find_request;
	}
	return NULL;

find_request:
	rq = blk_elv_next_locked(q, dir);
	/* An expired request, or a sweep that reached the top: start over
	 * from the oldest request of that direction. */
	if (blk_fifo_expired_locked(q, dir, now) || !rq)
		rq = q->fifo_head[dir];
	q->batching = 0;
	q->batch_dir = dir;

dispatch:
	q->batching++;
	return rq;
}

/* The elevator's next request, unlinked from its lists, or NULL.  Not
 * while the queue is suspended (only power-management requests, which
 * go on the dispatch list, may run then). */
static struct blk_request *blk_elv_dispatch_locked(struct blk_queue *q,
						   uint64_t now)
{
	struct blk_request *rq;

	lockdep_assert_held(&q->lock);
	if (blk_qflags(q) & BLK_QUEUE_FL_SUSPENDED)
		return NULL;
	if (!q->nr_queued[0] && !q->nr_queued[1])
		return NULL;
	rq = blk_pick_elevator_locked(q, now);
	if (!rq)
		return NULL;
	blk_elv_remove_locked(q, rq);
	q->next_sector[blk_rq_dir(rq)] = rq->sector + rq->count;
	return rq;
}

/* Does the elevator hold something that may be dispatched now?  Without
 * the lock: a hint the dispatcher confirms under it. */
static int blk_elv_has_work(const struct blk_queue *q)
{
	if (blk_qflags(q) & BLK_QUEUE_FL_SUSPENDED)
		return 0;
	return blk_ld32(&q->nr_queued[0]) || blk_ld32(&q->nr_queued[1]);
}

/* Take what may go now off the dispatch list: all of it, or, while the
 * queue is suspended, the power-management requests only.  Returned as
 * a list through disp_next, in order.  Queue lock held. */
static struct blk_request *blk_take_dispatch_list_locked(struct blk_queue *q)
{
	struct blk_request *head = NULL, **tailp = &head;
	struct blk_request *prev = NULL, *rq = q->disp_head;
	int suspended = (blk_qflags(q) & BLK_QUEUE_FL_SUSPENDED) != 0;

	lockdep_assert_held(&q->lock);
	while (rq) {
		struct blk_request *next = rq->disp_next;

		if (!suspended || (rq->flags & BLK_REQ_PM)) {
			blk_bypass_unlink_locked(q, prev, rq);
			*tailp = rq;
			tailp = &rq->disp_next;
		} else {
			prev = rq;
		}
		rq = next;
	}
	*tailp = NULL;
	return head;
}

/* ==========================================================================
 * Waiting on a queue
 *
 * A waiter counts itself in q->nr_waiters BEFORE it looks at its
 * condition, and looks and goes to sleep under q->wait_lock.  Whatever may
 * satisfy a condition -- a completion giving a tag back, a dispatcher
 * taking requests out of the elevator, a context leaving the queue --
 * makes its change first and then reads nr_waiters (a full barrier in
 * between): seeing 0, it knows any waiter that comes later will see the
 * change; seeing more, it wakes them under q->wait_lock, which a waiter
 * holds from its look at the condition until it is asleep.  So nothing
 * that completes a request needs the queue lock, and no wake-up is lost.
 * ========================================================================== */

typedef int (*blk_q_cond_t)(struct blk_queue *q);

/* The slot limit for a request: reads may use all nr_requests slots,
 * everything else async_depth of them. */
static uint32_t blk_sched_limit(const struct blk_queue *q, int read)
{
	uint32_t lim = q->nr_requests;
	uint32_t ad = blk_ld32(&q->async_depth);

	if (!read && ad && ad < lim)
		lim = ad;
	return lim ? lim : 1;
}

static int blk_cond_room(struct blk_queue *q)
{
	return (blk_qflags(q) & BLK_QUEUE_FL_DYING) ||
	       blk_ld32(&q->nr_sched) < blk_sched_limit(q, 1);
}

static int blk_cond_room_async(struct blk_queue *q)
{
	return (blk_qflags(q) & BLK_QUEUE_FL_DYING) ||
	       blk_ld32(&q->nr_sched) < blk_sched_limit(q, 0);
}

/* Take a slot when one is free under `limit'.  1 when taken. */
static int blk_sched_tag_try(struct blk_queue *q, uint32_t limit)
{
	uint32_t n = __atomic_load_n(&q->nr_sched, __ATOMIC_RELAXED);

	do {
		if (n >= limit)
			return 0;
	} while (!__atomic_compare_exchange_n(&q->nr_sched, &n, n + 1, 0,
					      __ATOMIC_SEQ_CST,
					      __ATOMIC_RELAXED));
	return 1;
}

/* Give a slot back (completion, or a merge that made the request part of
 * another).  The caller wakes the waiters. */
static void blk_sched_tag_put(struct blk_queue *q, struct blk_request *rq)
{
	if (!(rq->bl.internal & BLK_RQI_SCHED_TAG))
		return;
	rq->bl.internal &= ~BLK_RQI_SCHED_TAG;
	blk_atomic_dec_nonzero(&q->nr_sched);
}

static int blk_cond_drained(struct blk_queue *q)
{
	return blk_queue_nr_queued(q) == 0 && blk_ld32(&q->nr_requeue) == 0 &&
	       blk_queue_nr_in_flight(q) == 0 && blk_ld32(&q->active) == 0;
}

static int blk_cond_quiet(struct blk_queue *q)
{
	return blk_queue_nr_in_flight(q) == 0 && blk_ld32(&q->active) == 0;
}

/* After any change that may satisfy a waiter.  Any context, no queue
 * lock needed (q->lock may be held). */
static void blk_queue_wake(struct blk_queue *q)
{
	uint64_t fl;

	/* The change the caller made is visible before the count is read:
	 * pairs with the waiter's increment before its look. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&q->nr_waiters, __ATOMIC_SEQ_CST))
		return;
	spin_lock_irqsave(&q->wait_lock, &fl);
	/* Waiters queue themselves under wait_lock, so `first' is exact. */
	if (q->wait_drain.first)
		wq_wake_all(&q->wait_drain);
	if (q->wait_free.first && blk_cond_room(q))
		wq_wake_all(&q->wait_free);
	/* Writers wait for room under async_depth: woken only when there is
	 * some, not by every slot that comes free under nr_requests. */
	if (q->wait_free_async.first && blk_cond_room_async(q))
		wq_wake_all(&q->wait_free_async);
	spin_unlock_irqrestore(&q->wait_lock, fl);
}

static int blk_queue_wait_cond(struct blk_queue *q, struct wait_queue_head *wq,
			       blk_q_cond_t cond, uint32_t timeout_ms)
{
	uint64_t fl;
	int rc;

	/* Satisfied already: no waiter count, no lock. */
	if (cond(q))
		return 0;
	if (!blk_can_sleep()) {
		/* Boot or interrupts off: drive the driver ourselves.  The
		 * time is counted from the delays, which works before any
		 * clock is calibrated (it can only run long). */
		uint64_t waited_us = 0;

		for (;;) {
			blk_flush_own_done();
			if (cond(q))
				return 0;
			if (q->disk && q->disk->ops && q->disk->ops->poll)
				q->disk->ops->poll(q->disk);
			/* Completions the poll put on this CPU's block
			 * softirq list run here: no interrupt tail comes to
			 * run them while we poll with interrupts off. */
			if (sched_current() == NULL ||
			    (!blk_in_hardirq() && !blk_in_softirq()))
				blk_softirq_poll();
			/* Interrupts off with the scheduler running: kblockd
			 * may never get this processor, its work is ours. */
			if (!irqs_enabled())
				blk_work_run_pending();
			if (timeout_ms && waited_us >= (uint64_t)timeout_ms * 1000)
				return -ETIMEDOUT;
			lapic_delay_us(BLK_POLL_STEP_US);
			waited_us += BLK_POLL_STEP_US;
		}
	}

	might_sleep();
	(void)__atomic_add_fetch(&q->nr_waiters, 1, __ATOMIC_SEQ_CST);
	{
		task_t *cur = sched_current();
		uint64_t deadline = timeout_ms ?
					    hrtimer_now_ns() +
						    (uint64_t)timeout_ms * 1000000ULL :
					    0;

		for (;;) {
			struct wait_queue_entry we;
			uint64_t now, until;
			int polled;

			blk_flush_own_done();
			spin_lock_irqsave(&q->wait_lock, &fl);
			if (cond(q)) {
				spin_unlock_irqrestore(&q->wait_lock, fl);
				rc = 0;
				break;
			}
			now = hrtimer_now_ns();
			if (deadline && now >= deadline) {
				spin_unlock_irqrestore(&q->wait_lock, fl);
				rc = -ETIMEDOUT;
				break;
			}
			/* A driver without interrupts completes only when
			 * polled: wake up every millisecond to do it. */
			polled = (blk_qflags(q) & BLK_QUEUE_FL_POLLED) != 0;
			until = deadline;
			if (polled && (!until || until > now + 1000000ULL))
				until = now + 1000000ULL;
			wq_entry_init(&we, cur);
			wq_add(wq, &we);
			cur->wait_channel = q;
			cur->wakeup_tick =
				until ? timer_ticks() +
						timer_ns_to_ticks(until - now) + 1 :
					0;
			task_set_blocked(cur);
			spin_unlock_irqrestore(&q->wait_lock, fl);
			if (until)
				hrtimer_wait_arm(cur, until);
			sched_schedule();
			if (until)
				hrtimer_wait_disarm(cur);
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
			wq_remove(wq, &we);
			if (polled && q->disk && q->disk->ops &&
			    q->disk->ops->poll)
				q->disk->ops->poll(q->disk);
		}
	}
	(void)__atomic_sub_fetch(&q->nr_waiters, 1, __ATOMIC_SEQ_CST);
	return rc;
}

int blk_queue_wait_room(struct blk_queue *q)
{
	if (!q)
		return -ENODEV;
	(void)blk_queue_wait_cond(q, &q->wait_free, blk_cond_room, 0);
	return (blk_qflags(q) & BLK_QUEUE_FL_DYING) ? -ENODEV : 0;
}

int blk_queue_get_sched_tag(struct blk_queue *q, struct blk_request *req)
{
	int read = req->op == BLK_OP_READ;
	uint32_t limit;

	if (!q)
		return -ENODEV;
	if (req->bl.internal & BLK_RQI_SCHED_TAG)
		return 0;
	/* Requests that must not wait (the head of the queue, power
	 * management) and callers that cannot sleep take a slot beyond the
	 * limit: they have no other way to make progress. */
	if (!blk_can_sleep() || (req->flags & (BLK_REQ_HEAD | BLK_REQ_PM))) {
		if (blk_qflags(q) & BLK_QUEUE_FL_DYING)
			return -ENODEV;
		(void)__atomic_add_fetch(&q->nr_sched, 1, __ATOMIC_SEQ_CST);
		req->bl.internal |= BLK_RQI_SCHED_TAG;
		return 0;
	}
	/* Try first, then wait and try again (a woken waiter may find the
	 * slot taken by another one). */
	for (;;) {
		if (blk_qflags(q) & BLK_QUEUE_FL_DYING)
			return -ENODEV;
		limit = blk_sched_limit(q, read);
		if (blk_sched_tag_try(q, limit)) {
			req->bl.internal |= BLK_RQI_SCHED_TAG;
			return 0;
		}
		if (read)
			(void)blk_queue_wait_cond(q, &q->wait_free,
						  blk_cond_room, 0);
		else
			(void)blk_queue_wait_cond(q, &q->wait_free_async,
						  blk_cond_room_async, 0);
	}
}

/* The count is taken before DYING is looked at, and teardown sets DYING
 * before it waits for the count: an entering submitter either sees DYING
 * or is waited for. */
int blk_queue_enter(struct blk_queue *q)
{
	if (!q)
		return -ENODEV;
	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	if (blk_qflags(q) & BLK_QUEUE_FL_DYING) {
		blk_queue_leave(q);
		return -ENODEV;
	}
	return 0;
}

void blk_queue_leave(struct blk_queue *q)
{
	if (!q)
		return;
	blk_atomic_dec_nonzero(&q->active);
	blk_queue_wake(q);
}

int blk_queue_drain(struct blk_queue *q, uint32_t timeout_ms)
{
	if (!q)
		return 0;
	blk_might_sleep();
	/* What the caller holds on its plug goes in first. */
	if (blk_can_sleep())
		blk_flush_plug(sched_current()->plug, 0);
	blk_queue_run(q);
	return blk_queue_wait_cond(q, &q->wait_drain, blk_cond_drained,
				   timeout_ms);
}

int blk_queue_quiesce(struct blk_queue *q, uint32_t timeout_ms)
{
	if (!q)
		return 0;
	blk_might_sleep();
	return blk_queue_wait_cond(q, &q->wait_drain, blk_cond_quiet,
				   timeout_ms);
}

/* ==========================================================================
 * The tag table
 *
 * A request at the driver holds one of the queue's tags (at most `depth'
 * of them are in use).  A tag is taken with a compare-and-exchange on the
 * busy bitmap by the dispatcher and given back with an atomic AND by the
 * completion -- after the request's slot was set idle, so the timeout scan
 * never takes a later request for the one it found there.  The slot's
 * word holds the state, a generation and the deadline (see blkdev.h):
 * the scan claims an expired request with one compare-and-exchange on it,
 * and a completion racing with that claim simply wins or loses it.
 * ========================================================================== */

#define BLK_TAG_STATE_MASK 0x3ULL
#define BLK_TAG_GEN_SHIFT 2
#define BLK_TAG_GEN_MASK 0xFFFFULL
#define BLK_TAG_DL_SHIFT 18

static inline uint64_t blk_tag_word(uint64_t dl_ms, uint64_t gen,
				    uint32_t state)
{
	return (dl_ms << BLK_TAG_DL_SHIFT) |
	       ((gen & BLK_TAG_GEN_MASK) << BLK_TAG_GEN_SHIFT) |
	       (state & BLK_TAG_STATE_MASK);
}

static inline uint32_t blk_tag_state(uint64_t w)
{
	return (uint32_t)(w & BLK_TAG_STATE_MASK);
}

static inline uint64_t blk_tag_gen(uint64_t w)
{
	return (w >> BLK_TAG_GEN_SHIFT) & BLK_TAG_GEN_MASK;
}

static inline uint64_t blk_tag_deadline(uint64_t w)
{
	return w >> BLK_TAG_DL_SHIFT;
}

/* A free tag below the depth, or -1 (all taken, or `depth' requests are
 * at the driver already -- a depth lowered by error handling leaves the
 * requests above it alone, but no new one goes out until they are back
 * under it). */
static int blk_tag_get(struct blk_queue *q)
{
	uint64_t busy = __atomic_load_n(&q->tag_busy, __ATOMIC_RELAXED);
	uint32_t depth = blk_ld32(&q->depth);
	uint64_t mask;

	if (depth == 0)
		depth = 1;
	if (depth > BLK_QUEUE_MAX_TAGS)
		depth = BLK_QUEUE_MAX_TAGS;
	mask = depth >= 64 ? ~0ULL : ((1ULL << depth) - 1);
	for (;;) {
		uint64_t free_bits = ~busy & mask;
		int t;

		if (!free_bits || blk_popcount64(busy) >= depth)
			return -1;
		t = __builtin_ctzll(free_bits);
		if (__atomic_compare_exchange_n(&q->tag_busy, &busy,
						busy | (1ULL << t), 0,
						__ATOMIC_SEQ_CST,
						__ATOMIC_RELAXED))
			return t;
	}
}

/* The request's tag: 1 when it holds one. */
static int blk_rq_get_tag(struct blk_queue *q, struct blk_request *rq)
{
	int t;

	if (rq->bl.tag >= 0)
		return 1;
	t = blk_tag_get(q);
	if (t < 0)
		return 0;
	rq->bl.tag = t;
	return 1;
}

/* Give a tag back: the slot goes idle first (a scan that read it sees
 * the change and forgets what it read), then the bit is cleared -- the
 * full barrier of the atomic AND orders both before whatever the caller
 * does next (the restart check, the waiters' count).  Any context. */
static void blk_tag_put(struct blk_queue *q, int tag)
{
	struct blk_tag_slot *s;
	uint64_t w, old;

	BUG_ON(tag < 0 || tag >= BLK_QUEUE_MAX_TAGS);
	s = &q->tags[tag];
	w = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
	(void)__atomic_exchange_n(&s->word,
				  blk_tag_word(0, blk_tag_gen(w), BLK_TAG_IDLE),
				  __ATOMIC_SEQ_CST);
	__atomic_store_n(&s->rq, NULL, __ATOMIC_RELAXED);
	old = __atomic_fetch_and(&q->tag_busy, ~(1ULL << tag),
				 __ATOMIC_SEQ_CST);
	WARN_ON_ONCE(!(old & (1ULL << tag)));
}

static void blk_rq_put_tag(struct blk_queue *q, struct blk_request *rq)
{
	int t = rq->bl.tag;

	if (t < 0)
		return;
	rq->bl.tag = -1;
	blk_tag_put(q, t);
}

/* ==========================================================================
 * The timeout timer
 *
 * ONE coarse timer per queue: a dispatch arms it only when it is idle or
 * when the new deadline (rounded up by BLK_TIMER_SLACK_NS, at most
 * BLK_MAX_TIMER_NS ahead) is earlier than the pending expiry by at least
 * half the slack; a completion never touches it.  The arming is serialised
 * by q->timer_lock (dispatchers run on several CPUs); the callback (hard
 * interrupt) takes no queue lock: it only queues timeout_work on kblockd.
 * ========================================================================== */

/* One firing fewer outstanding (the callback's last access to the queue,
 * or a cancel that caught an arming). */
static void blk_timer_fire_put(struct blk_queue *q)
{
	blk_atomic_dec_nonzero(&q->timer_fires);
}

/* Arm or move the timer to `at' (absolute).  timer_lock held. */
static void blk_queue_mod_timeout_timer_locked(struct blk_queue *q, uint64_t at)
{
	lockdep_assert_held(&q->timer_lock);
	if (blk_qflags(q) & BLK_QUEUE_FL_DYING)
		return;
	if (!hrtimer_cancel(&q->timeout_timer))
		(void)__atomic_add_fetch(&q->timer_fires, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&q->timeout_expires, at, __ATOMIC_RELEASE);
	hrtimer_start(&q->timeout_timer, at);
}

/* Disarm; a firing caught before it ran is taken off the count.
 * timer_lock held. */
static void blk_queue_disarm_timer_locked(struct blk_queue *q)
{
	lockdep_assert_held(&q->timer_lock);
	if (hrtimer_cancel(&q->timeout_timer))
		blk_timer_fire_put(q);
}

static void blk_queue_mod_timeout_timer(struct blk_queue *q, uint64_t at)
{
	uint64_t fl;

	spin_lock_irqsave(&q->timer_lock, &fl);
	blk_queue_mod_timeout_timer_locked(q, at);
	spin_unlock_irqrestore(&q->timer_lock, fl);
}

/* A request with `deadline' went to the driver. */
static void blk_add_timer(struct blk_queue *q, uint64_t deadline, uint64_t now)
{
	uint64_t expiry = deadline + BLK_TIMER_SLACK_NS;
	uint64_t maxt = now + BLK_MAX_TIMER_NS + BLK_TIMER_SLACK_NS;
	uint64_t fl;

	if (expiry > maxt)
		expiry = maxt;
	/* Looked at without the lock first -- the common case on every
	 * dispatch is a timer armed early enough already, and then nothing
	 * is to be done -- and again under it before anything changes. */
	if (hrtimer_is_armed(&q->timeout_timer)) {
		uint64_t cur = __atomic_load_n(&q->timeout_expires,
					       __ATOMIC_ACQUIRE);

		if (expiry >= cur || cur - expiry < BLK_TIMER_SLACK_NS / 2)
			return;
	}
	spin_lock_irqsave(&q->timer_lock, &fl);
	if (hrtimer_is_armed(&q->timeout_timer)) {
		/* Due no later than this deadline needs (within the slack):
		 * leave it -- moving it would cost a timer reprogramming on
		 * nearly every dispatch. */
		if (expiry >= q->timeout_expires ||
		    q->timeout_expires - expiry < BLK_TIMER_SLACK_NS / 2) {
			spin_unlock_irqrestore(&q->timer_lock, fl);
			return;
		}
	}
	blk_queue_mod_timeout_timer_locked(q, expiry);
	spin_unlock_irqrestore(&q->timer_lock, fl);
}

/* The timer (hard interrupt): only hand the scan to kblockd.  The work
 * item is queued before the firing count drops, so blk_queue_free() --
 * which waits for the count first and then for the item -- cannot miss
 * it.  Before kblockd runs nobody could make the scan in process context:
 * look again a second later. */
static void blk_queue_timeout_timer_fn(hrtimer_t *t)
{
	struct blk_queue *q = (struct blk_queue *)t->arg;

	if (!(blk_qflags(q) & BLK_QUEUE_FL_DYING)) {
		if (blk_kblockd_running())
			(void)kblockd_schedule_work(&q->timeout_work);
		else
			blk_queue_mod_timeout_timer(q, hrtimer_now_ns() +
							       BLK_TIMER_SLACK_NS);
	}
	blk_timer_fire_put(q);
}

/* A request's timeout: the submitter's, else the default -- twice that
 * for a cache flush, which may have a whole cache to write. */
static uint32_t blk_rq_timeout_ms(const struct blk_request *rq)
{
	if (rq->timeout_ms)
		return rq->timeout_ms;
	if (rq->op == BLK_OP_FLUSH)
		return 2 * BLK_DEFAULT_TIMEOUT_MS;
	return BLK_DEFAULT_TIMEOUT_MS;
}

/* The request goes to the driver under its tag: the slot learns what the
 * timeout scan needs (published with the new generation), the queue's
 * timer covers the deadline. */
static void blk_queue_start_request(struct blk_queue *q, struct blk_request *rq,
				    uint64_t now)
{
	struct blk_tag_slot *s;
	uint32_t tmo = blk_rq_timeout_ms(rq);
	uint64_t dl_ms = blk_ns_to_ms_up(now + (uint64_t)tmo * 1000000ULL);
	uint64_t w;

	BUG_ON(rq->bl.tag < 0 || rq->bl.tag >= BLK_QUEUE_MAX_TAGS);
	s = &q->tags[rq->bl.tag];
	w = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
	VM_BUG_ON(blk_tag_state(w) != BLK_TAG_IDLE);
	VM_BUG_ON(rq->state != BLK_RQ_QUEUED);
	rq->state = BLK_RQ_DISPATCHED;
	rq->dispatch_ns = now;
	__atomic_store_n(&s->rq, rq, __ATOMIC_RELAXED);
	__atomic_store_n(&s->driver_data, rq->driver_data, __ATOMIC_RELAXED);
	__atomic_store_n(&s->dispatch_ns, now, __ATOMIC_RELAXED);
	__atomic_store_n(&s->rq_flags, rq->flags, __ATOMIC_RELAXED);
	__atomic_store_n(&s->timeout_ms, tmo, __ATOMIC_RELAXED);
	__atomic_store_n(&s->word,
			 blk_tag_word(dl_ms, blk_tag_gen(w) + 1,
				      BLK_TAG_IN_FLIGHT),
			 __ATOMIC_RELEASE);
	(void)__atomic_add_fetch(&q->dispatched, 1, __ATOMIC_RELAXED);
	blk_add_timer(q, dl_ms * 1000000ULL, now);
}

/* The driver did not take it (-EBUSY): back to queued, its tag back. */
static void blk_queue_unstart_request(struct blk_queue *q,
				      struct blk_request *rq)
{
	blk_rq_put_tag(q, rq);
	rq->state = BLK_RQ_QUEUED;
	(void)__atomic_add_fetch(&q->requeued, 1, __ATOMIC_RELAXED);
}

/* ==========================================================================
 * Dispatch
 *
 * Who runs the queue:
 *
 *   - kblockd, asynchronously, on the CPU that asked: after a submission
 *     into the elevator (the submitter only inserts), when a completion
 *     freed a tag that dispatch was waiting for (the restart), for the
 *     requests given back to the queue (requeue work), when the driver
 *     was out of a resource, and for every run asked for where
 *     dispatching is not allowed (a hard interrupt, the block softirq);
 *   - the caller, synchronously: a task finishing its plug, a pass-
 *     through command or cache flush being executed, the "none"
 *     scheduler's direct issue, a poller with interrupts off, and
 *     everything before kblockd exists (boot).
 *
 * Any number of contexts may run the queue at once.  Each takes the
 * dispatch list or the elevator's next request under the queue lock,
 * gives it a tag with an atomic operation and hands it to the driver with
 * no lock held.  A completion never dispatches by itself: the doorbell of
 * the next command is written by a task, not by an interrupt handler or
 * the softirq that runs on its tail.
 * ========================================================================== */

/* May the queue be run right here?  In a context that may sleep (a task,
 * interrupts enabled, not a hard interrupt and not the block softirq),
 * yes; before kblockd exists (boot, or it could not be started) as well,
 * since nobody else would; and in a task that polls with interrupts off
 * (it is not an interrupt handler, and nothing else will run while it
 * waits).  Everywhere else the run is kblockd's. */
static int blk_queue_run_here(void)
{
	if (blk_can_sleep() || sched_current() == NULL ||
	    !blk_kblockd_running())
		return 1;
	return !irqs_enabled() && !blk_in_hardirq() && !blk_in_softirq();
}

/* The CPU whose kblockd makes an asynchronous run: the one asking (a
 * single hardware queue serves every CPU, so no other CPU is a better
 * choice). */
static uint32_t blk_queue_run_cpu(void)
{
	int cpu = blk_this_cpu();

	return cpu < 0 ? 0u : (uint32_t)cpu;
}

/* Is there anything to dispatch, and may it be dispatched?  Without any
 * lock. */
static int blk_queue_has_work(const struct blk_queue *q)
{
	return blk_ld32(&q->nr_bypass) || blk_elv_has_work(q);
}

static int blk_queue_need_run(const struct blk_queue *q)
{
	if (blk_qflags(q) & (BLK_QUEUE_FL_DYING | BLK_QUEUE_FL_STOPPED))
		return 0;
	return blk_queue_has_work(q);
}

/* Restart (BLK_QS_SCHED_RESTART): dispatch stopped with requests left
 * for want of a tag, or after the driver refused one taken from the
 * dispatch list.  The completion that frees a tag runs the queue. */
static void blk_queue_mark_restart(struct blk_queue *q)
{
	if (__atomic_load_n(&q->state, __ATOMIC_ACQUIRE) & BLK_QS_SCHED_RESTART)
		return;
	(void)__atomic_fetch_or(&q->state, BLK_QS_SCHED_RESTART,
				__ATOMIC_SEQ_CST);
}

static int blk_queue_needs_restart(const struct blk_queue *q)
{
	return (__atomic_load_n(&q->state, __ATOMIC_SEQ_CST) &
		BLK_QS_SCHED_RESTART) != 0;
}

/* Run the queue asynchronously after `delay_ns' (0: now) on this CPU's
 * kblockd.  The pending run holds a count of
 * `active' -- as a run in progress would -- so drain, quiesce and teardown
 * wait for it.  Any context. */
static void blk_queue_delay_run(struct blk_queue *q, uint64_t delay_ns)
{
	if (blk_qflags(q) & (BLK_QUEUE_FL_STOPPED | BLK_QUEUE_FL_DYING))
		return;
	if (!blk_kblockd_running()) {
		/* Before kblockd an item queued from a task runs inline.  A
		 * delayed retry would run into the same refusal at once: the
		 * polled waiter asks again instead (blk_wait).  A run asked
		 * for while one of this queue is going on is left to that
		 * one, which repeats -- the inline runs never nest. */
		if (delay_ns)
			return;
		if (__atomic_load_n(&q->inline_runs, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&q->inline_rerun, 1, __ATOMIC_RELEASE);
			return;
		}
	}
	if (!__atomic_exchange_n(&q->run_pending, 1, __ATOMIC_SEQ_CST))
		(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	(void)kblockd_mod_delayed_work_on(blk_queue_run_cpu(), &q->run_work,
					  delay_ns);
}

void blk_queue_run_async(struct blk_queue *q)
{
	if (!q)
		return;
	if (!blk_queue_need_run(q)) {
		/* Looked at without a lock: look once more behind a full
		 * barrier, which orders the look after whatever change the
		 * caller made (a tag back, a request added) against a
		 * concurrent start of the queue that looked before it. */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!blk_queue_need_run(q))
			return;
	}
	blk_queue_delay_run(q, 0);
}

/* The completion's restart: the bit is cleared
 * before the run looks for work, and a dispatcher that puts requests back
 * looks at the bit after putting them back -- one of the two sees the
 * other. */
static void blk_queue_sched_restart(struct blk_queue *q)
{
	if (!blk_queue_needs_restart(q))
		return;
	(void)__atomic_fetch_and(&q->state, ~BLK_QS_SCHED_RESTART,
				 __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	blk_queue_run_async(q);
}

/* The decaying average of how often the driver was busy. */
static void blk_queue_update_dispatch_busy(struct blk_queue *q, int busy)
{
	uint32_t ewma = __atomic_load_n(&q->dispatch_busy, __ATOMIC_RELAXED);

	if (!ewma && !busy)
		return;
	ewma *= BLK_DISPATCH_BUSY_EWMA_WEIGHT - 1;
	if (busy)
		ewma += 1u << BLK_DISPATCH_BUSY_EWMA_FACTOR;
	ewma /= BLK_DISPATCH_BUSY_EWMA_WEIGHT;
	__atomic_store_n(&q->dispatch_busy, ewma, __ATOMIC_RELAXED);
}

static uint32_t blk_rq_bytes_for_resid(const struct blk_request *rq)
{
	return (uint32_t)((uint64_t)rq->count *
			  (rq->bdev ? rq->bdev->sector_size : 512));
}

/* Put requests a dispatcher could not hand over back at the tail of the
 * dispatch list.  Their tags go back: a request waiting on the list does
 * not count as being at the driver (quiesce and suspend wait for that). */
static void blk_queue_putback_list(struct blk_queue *q, struct blk_request *list)
{
	uint64_t fl;

	for (struct blk_request *r = list; r; r = r->disp_next)
		blk_rq_put_tag(q, r);
	spin_lock_irqsave(&q->lock, &fl);
	while (list) {
		struct blk_request *rq = list;

		list = rq->disp_next;
		rq->disp_next = NULL;
		blk_bypass_add_locked(q, rq, 0);
	}
	spin_unlock_irqrestore(&q->lock, fl);
}

/* Hand the requests on `*list' to the driver, in order
 * Returns 1 when everything went (more may be
 * possible), 0 when requests were left -- they are back on the dispatch
 * list and the queue is run again as the situation asks: at once when no
 * restart is pending (nothing else would run it), after
 * BLK_RESOURCE_DELAY_NS when the driver was out of a resource or no tag
 * (budget) was free and a restart is pending anyway. */
static int blk_queue_dispatch_list(struct blk_queue *q,
				   struct blk_request **list)
{
	struct gendisk *disk = q->disk;
	int needs_resource = 0;

	if (!*list)
		return 0;
	while (*list) {
		struct blk_request *rq = *list;
		uint64_t now;
		int rc;

		if (rq->bl.tag < 0 && !blk_rq_get_tag(q, rq)) {
			/* No tag -- the tag is the device's dispatch budget
			 * as well: the completion that frees one restarts
			 * the queue, and as for a budget that was not there
			 * a delayed run backs that up.  One may have been
			 * freed between the failed attempt and the mark --
			 * try once more (at worst this costs one run too
			 * many). */
			blk_queue_mark_restart(q);
			if (!blk_rq_get_tag(q, rq)) {
				needs_resource = 1;
				break;
			}
		}
		*list = rq->disp_next;
		rq->disp_next = NULL;
		now = hrtimer_now_ns();
		blk_queue_start_request(q, rq, now);

		rc = disk->ops->submit(disk, rq);

		if (likely(rc == 0))
			continue;
		if (rc == -EBUSY) {
			/* The driver is out of a resource: back to the
			 * head of what is left. */
			needs_resource = 1;
			blk_queue_unstart_request(q, rq);
			rq->disp_next = *list;
			*list = rq;
			break;
		}
		/* Rejected: it completes with the driver's error (its tag
		 * goes back with the completion). */
		if (!(rq->flags & BLK_REQ_QUIET))
			WARN_RATELIMIT(1, "%s: driver refused %s sector %llu (%d)",
				       disk->disk_name, blk_op_name(rq->op),
				       (unsigned long long)rq->sector, rc);
		blk_request_complete(rq, rc, blk_rq_bytes_for_resid(rq));
	}

	if (*list) {
		struct blk_request *left = *list;

		*list = NULL;
		blk_queue_putback_list(q, left);
		/* Back on the list before the restart bit is read: pairs
		 * with the barrier of blk_queue_sched_restart. */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!blk_queue_needs_restart(q))
			blk_queue_run_async(q);
		else if (needs_resource)
			blk_queue_delay_run(q, BLK_RESOURCE_DELAY_NS);
		blk_queue_update_dispatch_busy(q, 1);
		blk_queue_wake(q);
		return 0;
	}
	blk_queue_update_dispatch_busy(q, 0);
	return 1;
}

/* One round from the elevator: its next
 * requests -- one when the driver has been busy, else as many as tags
 * allow -- then to the driver.  The tag plays the part of the device's
 * dispatch budget and is taken BEFORE a request leaves the elevator: when
 * the device is full the requests stay where they are sorted and can
 * still merge, and the completion that frees a tag restarts the queue.
 * -EAGAIN: requests turned up on the dispatch list meanwhile (they go
 * first: run again); 1 when something went and more may; 0 otherwise. */
static int __blk_queue_do_dispatch_sched(struct blk_queue *q)
{
	struct blk_request *list = NULL, **tailp = &list;
	uint32_t max_dispatch, count = 0;
	int busy = 0, dispatched = 0, run_queue = 0;
	uint64_t fl;

	max_dispatch = blk_ld32(&q->dispatch_busy) ? 1 : q->nr_requests;
	do {
		struct blk_request *rq;
		int tag;

		if (!blk_elv_has_work(q))
			break;
		if (blk_ld32(&q->nr_bypass)) {
			busy = 1;
			break;
		}
		tag = blk_tag_get(q);
		if (tag < 0) {
			/* The device is full: the completion that frees a
			 * tag restarts the queue.  One may have been freed
			 * between the attempt and the mark -- try once
			 * more. */
			blk_queue_mark_restart(q);
			tag = blk_tag_get(q);
			if (tag < 0)
				break;
		}
		spin_lock_irqsave(&q->lock, &fl);
		rq = blk_elv_dispatch_locked(q, hrtimer_now_ns());
		spin_unlock_irqrestore(&q->lock, fl);
		if (!rq) {
			/* Another dispatcher took it.  The tag held meanwhile
			 * may have kept a third dispatcher from taking a
			 * request that arrived since (it marked the restart,
			 * but no completion will come for a tag that only
			 * went back): run the queue again shortly. */
			blk_tag_put(q, tag);
			run_queue = 1;
			break;
		}
		rq->bl.tag = tag;
		*tailp = rq;
		tailp = &rq->disp_next;
		rq->disp_next = NULL;
		count++;
	} while (count < max_dispatch);

	if (!count) {
		if (run_queue)
			blk_queue_delay_run(q, BLK_BUDGET_DELAY_NS);
	} else {
		dispatched = blk_queue_dispatch_list(q, &list);
	}
	if (busy)
		return -EAGAIN;
	return dispatched;
}

/* Rounds from the elevator while they go well,
 * but no longer than BLK_DISPATCH_MAX_NS and not past the moment this
 * task should give the processor up: then kblockd carries on. */
static int blk_queue_do_dispatch_sched(struct blk_queue *q)
{
	uint64_t end = hrtimer_now_ns() + BLK_DISPATCH_MAX_NS;
	int ret;

	for (;;) {
		ret = __blk_queue_do_dispatch_sched(q);
		if (ret != 1)
			break;
		if (sched_need_resched() || hrtimer_now_ns() > end) {
			blk_queue_delay_run(q, 0);
			break;
		}
	}
	return ret;
}

/* "none", the queue busy: one request at a time while the driver takes
 * them, the tag (budget) taken first. */
static int blk_queue_do_dispatch_one(struct blk_queue *q)
{
	uint64_t fl;

	for (;;) {
		struct blk_request *rq;
		int tag;

		if (blk_ld32(&q->nr_bypass))
			return -EAGAIN;
		if (!blk_elv_has_work(q))
			return 0;
		tag = blk_tag_get(q);
		if (tag < 0) {
			blk_queue_mark_restart(q);
			tag = blk_tag_get(q);
			if (tag < 0)
				return 0;
		}
		spin_lock_irqsave(&q->lock, &fl);
		rq = blk_elv_dispatch_locked(q, hrtimer_now_ns());
		spin_unlock_irqrestore(&q->lock, fl);
		if (!rq) {
			/* As in a round from the elevator: the tag held
			 * meanwhile may have turned a dispatcher away. */
			blk_tag_put(q, tag);
			blk_queue_delay_run(q, BLK_BUDGET_DELAY_NS);
			return 0;
		}
		rq->bl.tag = tag;
		rq->disp_next = NULL;
		if (!blk_queue_dispatch_list(q, &rq))
			return 0;
	}
}

/* One pass: the dispatch list first --
 * marked for a restart, since what was refused before may be refused
 * again -- and only when all of it went, the elevator. */
static int __blk_queue_dispatch(struct blk_queue *q)
{
	struct blk_request *list = NULL;
	int need_dispatch;
	uint64_t fl;

	if (blk_ld32(&q->nr_bypass)) {
		spin_lock_irqsave(&q->lock, &fl);
		list = blk_take_dispatch_list_locked(q);
		spin_unlock_irqrestore(&q->lock, fl);
	}
	if (list) {
		blk_queue_mark_restart(q);
		if (!blk_queue_dispatch_list(q, &list))
			return 0;
		need_dispatch = 1;
	} else {
		need_dispatch = blk_ld32(&q->dispatch_busy) != 0;
	}

	if (q->sched != BLK_SCHED_NONE)
		return blk_queue_do_dispatch_sched(q);
	if (need_dispatch)
		return blk_queue_do_dispatch_one(q);
	/* "none", idle: everything waiting goes at once. */
	{
		struct blk_request **tailp = &list;
		struct blk_request *rq;

		spin_lock_irqsave(&q->lock, &fl);
		while ((rq = blk_elv_dispatch_locked(q, hrtimer_now_ns()))) {
			*tailp = rq;
			tailp = &rq->disp_next;
			rq->disp_next = NULL;
		}
		spin_unlock_irqrestore(&q->lock, fl);
		if (list)
			(void)blk_queue_dispatch_list(q, &list);
	}
	return 0;
}

/* Dispatch: a pass, and when requests
 * turned up on the dispatch list during it, another; when even that one
 * found new ones, kblockd makes the next. */
static void blk_queue_dispatch_requests(struct blk_queue *q)
{
	int boot;

	/* Counted before the queue's state is looked at -- as a submitter
	 * enters -- so teardown, which sets DYING and then waits for the
	 * count, either is seen here or waits for this run. */
	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	if (blk_qflags(q) & (BLK_QUEUE_FL_STOPPED | BLK_QUEUE_FL_DYING)) {
		blk_queue_leave(q);
		return;
	}
	boot = !blk_kblockd_running();
	if (boot)
		(void)__atomic_add_fetch(&q->inline_runs, 1, __ATOMIC_SEQ_CST);
	do {
		if (boot)
			__atomic_store_n(&q->inline_rerun, 0, __ATOMIC_RELEASE);
		if (__blk_queue_dispatch(q) == -EAGAIN &&
		    __blk_queue_dispatch(q) == -EAGAIN)
			blk_queue_run_async(q);
	} while (boot &&
		 __atomic_exchange_n(&q->inline_rerun, 0, __ATOMIC_ACQ_REL) &&
		 !(blk_qflags(q) & (BLK_QUEUE_FL_STOPPED | BLK_QUEUE_FL_DYING)));
	if (boot)
		blk_atomic_dec_nonzero(&q->inline_runs);
	blk_queue_leave(q);
}

void blk_queue_run(struct blk_queue *q)
{
	if (!q)
		return;
	if (!blk_queue_need_run(q)) {
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!blk_queue_need_run(q))
			return;
	}
	if (!blk_queue_run_here()) {
		blk_queue_delay_run(q, 0);
		return;
	}
	blk_queue_dispatch_requests(q);
}

/* kblockd's run of the queue (q->run_work). */
static void blk_queue_run_work_fn(void *arg)
{
	struct blk_queue *q = (struct blk_queue *)arg;
	int took = (int)__atomic_exchange_n(&q->run_pending, 0,
					    __ATOMIC_SEQ_CST);

	blk_queue_dispatch_requests(q);
	if (took)
		blk_queue_leave(q);
}

/* ==========================================================================
 * Submission: putting a request into the queue
 * ========================================================================== */

static void blk_insert_flush(struct blk_queue *q, struct blk_request *rq);

/* Where blk_queue_add_request() put a request. */
#define BLK_ADD_NONE 0 /* merged, ended, or in the flush machinery */
#define BLK_ADD_ELV 1 /* the elevator */
#define BLK_ADD_DISPATCH 2 /* the dispatch list */

/* End a request that was to go into a dying queue: a driver context it
 * still carries (a request given back for its next step) goes back to the
 * driver first.  `from_schedule': the caller is the scheduler's plug hook,
 * where an end_io must not run -- the end goes through the block softirq.
 * No lock held. */
static void blk_queue_fail_insert(struct blk_queue *q, struct blk_request *req,
				  int from_schedule)
{
	struct gendisk *disk = q->disk;

	if (req->driver_data && disk && disk->ops && disk->ops->cleanup_rq)
		disk->ops->cleanup_rq(req);
	req->driver_data = NULL;
	req->bl.internal |= BLK_RQI_FLUSH_END;
	if (from_schedule)
		blk_request_end_deferred(req, -ENODEV,
					 blk_rq_bytes_for_resid(req));
	else
		blk_request_complete(req, -ENODEV, blk_rq_bytes_for_resid(req));
}

/* Requests that never go through the elevator: their order is their
 * meaning (pass-through, the cache flush, HEAD and PM). */
static int blk_rq_is_bypass(const struct blk_request *req)
{
	return req->op == BLK_OP_FLUSH || req->op == BLK_OP_DRV_IN ||
	       req->op == BLK_OP_DRV_OUT ||
	       (req->flags & (BLK_REQ_HEAD | BLK_REQ_PM));
}

/* Put `req' into the queue -- the flush machinery, the dispatch list or
 * the elevator (merging) -- without running it.  BLK_ADD_*. */
static int blk_queue_add_request(struct blk_queue *q, struct blk_request *req,
				 int from_schedule)
{
	uint64_t fl, now;

	BUG_ON(q == NULL || req == NULL);
	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	now = hrtimer_now_ns();
	req->state = BLK_RQ_QUEUED;
	req->bl.tag = -1;
	/* The inserting CPU: the completion is finished there. */
	req->submit_cpu = blk_this_cpu();
	spin_lock_irqsave(&q->lock, &fl);
	/* Under the lock: teardown sets DYING and empties the lists under
	 * it, so a request is either failed here or collected there. */
	if (unlikely(blk_qflags(q) & BLK_QUEUE_FL_DYING)) {
		spin_unlock_irqrestore(&q->lock, fl);
		blk_queue_fail_insert(q, req, from_schedule);
		return BLK_ADD_NONE;
	}
	blk_account_start(q, req, now);
	if (req->bl.internal & BLK_RQI_FLUSH_SEQ) {
		spin_unlock_irqrestore(&q->lock, fl);
		blk_insert_flush(q, req);
		return BLK_ADD_NONE;
	}
	if (blk_rq_is_bypass(req)) {
		blk_bypass_add_locked(q, req, (req->flags & BLK_REQ_HEAD) != 0);
		spin_unlock_irqrestore(&q->lock, fl);
		return BLK_ADD_DISPATCH;
	}
	if (blk_try_merge_locked(q, req)) {
		/* Absorbed; nothing new to dispatch, a slot came free. */
		spin_unlock_irqrestore(&q->lock, fl);
		blk_queue_wake(q);
		return BLK_ADD_NONE;
	}
	blk_elv_add_locked(q, req, now);
	spin_unlock_irqrestore(&q->lock, fl);
	return BLK_ADD_ELV;
}

/* The "none" scheduler: a request that finds
 * the queue idle -- not stopped, nothing waiting before it, a tag free --
 * goes to the driver right here; else it is queued and the queue run. */
static void blk_queue_try_issue_directly(struct blk_queue *q,
					 struct blk_request *rq)
{
	struct gendisk *disk = q->disk;
	uint64_t fl, now = hrtimer_now_ns();
	int rc;

	rq->state = BLK_RQ_QUEUED;
	rq->bl.tag = -1;
	rq->submit_cpu = blk_this_cpu();
	spin_lock_irqsave(&q->lock, &fl);
	if (unlikely(blk_qflags(q) & BLK_QUEUE_FL_DYING)) {
		spin_unlock_irqrestore(&q->lock, fl);
		blk_queue_fail_insert(q, rq, 0);
		return;
	}
	blk_account_start(q, rq, now);
	/* The queue keeps merging without a scheduler. */
	if (blk_try_merge_locked(q, rq)) {
		spin_unlock_irqrestore(&q->lock, fl);
		blk_queue_wake(q);
		return;
	}
	if ((blk_qflags(q) & (BLK_QUEUE_FL_STOPPED | BLK_QUEUE_FL_SUSPENDED)) ||
	    blk_ld32(&q->dispatch_busy) || !blk_queue_run_here() ||
	    blk_queue_nr_queued(q) || !blk_rq_get_tag(q, rq)) {
		int async = blk_ld32(&q->dispatch_busy) != 0;

		blk_elv_add_locked(q, rq, now);
		spin_unlock_irqrestore(&q->lock, fl);
		if (async)
			blk_queue_run_async(q);
		else
			blk_queue_run(q);
		return;
	}
	spin_unlock_irqrestore(&q->lock, fl);

	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	blk_queue_start_request(q, rq, now);
	rc = disk->ops->submit(disk, rq);
	if (likely(rc == 0)) {
		blk_queue_update_dispatch_busy(q, 0);
	} else if (rc == -EBUSY) {
		blk_queue_update_dispatch_busy(q, 1);
		blk_queue_unstart_request(q, rq);
		spin_lock_irqsave(&q->lock, &fl);
		blk_bypass_add_locked(q, rq, 0);
		spin_unlock_irqrestore(&q->lock, fl);
		blk_queue_run(q);
	} else {
		blk_queue_update_dispatch_busy(q, 0);
		if (!(rq->flags & BLK_REQ_QUIET))
			WARN_RATELIMIT(1, "%s: driver refused %s sector %llu (%d)",
				       disk->disk_name, blk_op_name(rq->op),
				       (unsigned long long)rq->sector, rc);
		blk_request_complete(rq, rc, blk_rq_bytes_for_resid(rq));
	}
	blk_queue_leave(q);
}

void blk_queue_insert(struct blk_queue *q, struct blk_request *req)
{
	BUG_ON(q == NULL || req == NULL);
	/* Without a scheduler an ordinary request is tried at the driver
	 * directly. */
	if (q->sched == BLK_SCHED_NONE && !blk_rq_is_bypass(req) &&
	    !(req->bl.internal & BLK_RQI_FLUSH_SEQ)) {
		blk_queue_try_issue_directly(q, req);
		return;
	}
	switch (blk_queue_add_request(q, req, 0)) {
	case BLK_ADD_DISPATCH:
		/* Pass-through, HEAD/PM and the cache flush: executed by
		 * their submitter, which runs the queue itself. */
		blk_queue_run(q);
		break;
	case BLK_ADD_ELV:
		/* Into the elevator: kblockd on this CPU dispatches. */
		blk_queue_run_async(q);
		break;
	default:
		break;
	}
}

/* ==========================================================================
 * Completion: the queue's side
 *
 * block.c ends a request in this order: its end_io and the wake-up of its
 * waiter first (the submitter learns the result as early as possible),
 * then -- from a snapshot taken before, since the woken waiter may free
 * the request at once -- the statistics, the elevator's completion count,
 * the merge pool array and the tag, the restart of a queue that waits for
 * a tag, and the wake-up of submitters and drainers.  None of it takes a
 * queue lock.
 * ========================================================================== */

void blk_queue_done_prepare(struct blk_queue *q, struct blk_request *req,
			    struct blk_done_snap *snap)
{
	enum blk_op op = blk_rq_acct_op(req);

	(void)q;
	snap->tag = req->bl.tag;
	req->bl.tag = -1;
	snap->merge_slot = req->bl.merge_slot;
	/* the array is no longer the carrier's (its restore follows) */
	req->bl.merge_slot = -1;
	snap->status = req->status;
	snap->completed = 1 + req->nr_merged;
	snap->elv = (req->bl.internal & BLK_RQI_ELV) != 0;
	req->bl.internal &= ~BLK_RQI_ELV;
	snap->sched_tag = (req->bl.internal & BLK_RQI_SCHED_TAG) != 0;
	req->bl.internal &= ~BLK_RQI_SCHED_TAG;
	snap->acct = (req->bl.internal & BLK_RQI_ACCT) != 0;
	req->bl.internal &= ~BLK_RQI_ACCT;
	snap->group = blk_stat_group(op);
	snap->dir = blk_op_is_write(op) ? 1 : 0;
	snap->start_ns = req->submit_ns;
	snap->now_ns = req->complete_ns ? req->complete_ns : hrtimer_now_ns();
	snap->sect512 = 0;
	if (req->bdev && req->bdev->sector_size && op != BLK_OP_FLUSH)
		snap->sect512 = (uint64_t)req->count * req->bdev->sector_size /
				512;
	snap->disk_stats = NULL;
	snap->part_stats = NULL;
	if (snap->acct) {
		snap->disk_stats = q && q->disk ? &q->disk->stats : NULL;
		snap->part_stats = blk_rq_part_stats(req);
	}
}

void blk_queue_done_snap(struct blk_queue *q, const struct blk_done_snap *snap)
{
	BUG_ON(q == NULL || snap == NULL);
	if (snap->acct)
		blk_account_done(q, snap);
	(void)__atomic_add_fetch(&q->completed, snap->completed,
				 __ATOMIC_RELAXED);
	if (snap->status) {
		(void)__atomic_add_fetch(&q->errors, 1, __ATOMIC_RELAXED);
		if (snap->status == -ETIMEDOUT)
			(void)__atomic_add_fetch(&q->timeouts, 1,
						 __ATOMIC_RELAXED);
	}
	/* All the elevator learns of a completion (its finish hook). */
	if (snap->elv)
		(void)__atomic_add_fetch(&q->elv_completed, 1,
					 __ATOMIC_RELAXED);
	blk_merge_slot_put(q, snap->merge_slot);
	if (snap->tag >= 0)
		blk_tag_put(q, snap->tag);
	/* Its queue slot: a submitter waiting for one goes on (the wake
	 * below). */
	if (snap->sched_tag)
		blk_atomic_dec_nonzero(&q->nr_sched);
	/* The tag is free: a dispatch that stopped for want of one goes on,
	 * through kblockd. */
	blk_queue_sched_restart(q);
	blk_queue_wake(q);
}

void blk_queue_done(struct blk_queue *q, struct blk_request *req)
{
	struct blk_done_snap snap;

	BUG_ON(q == NULL || req == NULL);
	blk_queue_done_prepare(q, req, &snap);
	blk_queue_done_snap(q, &snap);
}

/* ==========================================================================
 * Requests given back to the queue
 *
 * A completion (block softirq) that gives a request back -- a retry after
 * error handling, a further step of a request the driver continues, a
 * flush sequence's data step -- and the flush machinery's own flush
 * request never insert into the elevator from where they are: they go on
 * the requeue lists under q->requeue_lock, and the requeue work on
 * kblockd puts them on the dispatch list and runs the queue.
 * ========================================================================== */

/* End a request that waits in the queue with -ENODEV (the queue is
 * dying): a driver context it still carries (a request waiting for its
 * next step) goes back to the driver first.  No lock held. */
static void blk_queue_fail_queued(struct blk_queue *q, struct blk_request *rq)
{
	struct gendisk *disk = q->disk;

	if (rq->driver_data && disk && disk->ops && disk->ops->cleanup_rq)
		disk->ops->cleanup_rq(rq);
	rq->driver_data = NULL;
	blk_request_complete(rq, -ENODEV, blk_rq_bytes_for_resid(rq));
}

static void blk_requeue_list_add_locked(struct blk_request **head,
					struct blk_request **tail,
					struct blk_request *rq)
{
	rq->disp_next = NULL;
	if (*tail)
		(*tail)->disp_next = rq;
	else
		*head = rq;
	*tail = rq;
}

/* Have kblockd insert what waits on the requeue lists
 * on this CPU. */
static void blk_queue_kick_requeue(struct blk_queue *q)
{
	(void)kblockd_mod_delayed_work_on(blk_queue_run_cpu(),
					  &q->requeue_work, 0);
}

/* Queue `rq' for the requeue work: `flush' for the flush request.
 * Returns 0, or -ENODEV when the queue is dying (the caller fails it).
 * May be called with flush_lock held (requeue_lock nests inside it). */
static int blk_queue_add_requeue(struct blk_queue *q, struct blk_request *rq,
				 int flush)
{
	uint64_t fl;

	spin_lock_irqsave(&q->requeue_lock, &fl);
	/* Teardown empties these lists after setting DYING, under this
	 * lock: a request is either failed by its giver or collected. */
	if (blk_qflags(q) & BLK_QUEUE_FL_DYING) {
		spin_unlock_irqrestore(&q->requeue_lock, fl);
		return -ENODEV;
	}
	if (flush)
		blk_requeue_list_add_locked(&q->rq_flush_head,
					    &q->rq_flush_tail, rq);
	else
		blk_requeue_list_add_locked(&q->requeue_head,
					    &q->requeue_tail, rq);
	(void)__atomic_add_fetch(&q->nr_requeue, 1, __ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&q->requeue_lock, fl);
	return 0;
}

/* kblockd's requeue work: a request the driver
 * continues (it carries its context) goes to the tail of the dispatch
 * list, any other to its head; the flush request to the head; then the
 * queue runs here. */
static void blk_queue_requeue_work_fn(void *arg)
{
	struct blk_queue *q = (struct blk_queue *)arg;
	struct blk_request *list, *flist;
	uint64_t fl;

	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	spin_lock_irqsave(&q->requeue_lock, &fl);
	list = q->requeue_head;
	flist = q->rq_flush_head;
	q->requeue_head = q->requeue_tail = NULL;
	q->rq_flush_head = q->rq_flush_tail = NULL;
	spin_unlock_irqrestore(&q->requeue_lock, fl);

	while (list || flist) {
		struct blk_request *rq;
		int flush = list == NULL;

		if (!flush) {
			rq = list;
			list = rq->disp_next;
		} else {
			rq = flist;
			flist = rq->disp_next;
		}
		rq->disp_next = NULL;
		spin_lock_irqsave(&q->lock, &fl);
		if (unlikely(blk_qflags(q) & BLK_QUEUE_FL_DYING)) {
			spin_unlock_irqrestore(&q->lock, fl);
			blk_queue_fail_queued(q, rq);
		} else {
			blk_bypass_add_locked(q, rq,
					      flush || rq->driver_data == NULL);
			spin_unlock_irqrestore(&q->lock, fl);
		}
		blk_atomic_dec_nonzero(&q->nr_requeue);
	}
	blk_queue_run(q);
	blk_queue_leave(q);
}

/* Put a dispatched request back and have kblockd dispatch it again.
 * `keep': the driver continues the request (driver_data stays).  Any
 * context; no elevator lock. */
static void blk_queue_give_back(struct blk_queue *q, struct blk_request *req,
				int keep)
{
	BUG_ON(q == NULL || req == NULL);
	if (WARN(req->state != BLK_RQ_DISPATCHED,
		 "requeue of a request that is not dispatched (state %d)",
		 (int)req->state))
		return;
	/* Its tag goes back now (the requeue lets the driver tag go); the
	 * next dispatch takes a new one. */
	blk_rq_put_tag(q, req);
	req->state = BLK_RQ_QUEUED;
	if (!keep) {
		req->driver_data = NULL;
		req->retries++;
	}
	(void)__atomic_add_fetch(&q->requeued, 1, __ATOMIC_RELAXED);
	if (blk_queue_add_requeue(q, req, 0)) {
		blk_queue_fail_queued(q, req);
		blk_queue_wake(q);
		return;
	}
	blk_queue_kick_requeue(q);
	blk_queue_wake(q);
}

void blk_queue_requeue(struct blk_queue *q, struct blk_request *req)
{
	blk_queue_give_back(q, req, 0);
}

void blk_queue_continue(struct blk_queue *q, struct blk_request *req)
{
	blk_queue_give_back(q, req, 1);
}

/* ==========================================================================
 * The flush machinery
 *
 * A write that must reach stable media -- PREFLUSH (the cache emptied
 * before it), FUA on a disk without native FUA (the cache emptied after
 * it) -- and a FLUSH request run through a sequence of steps: PREFLUSH,
 * DATA, POSTFLUSH, DONE (BLK_SEQ_*; the steps a request does not need
 * count as done from the start).  A request whose next step is a flush
 * waits on the queue's pending flush list; the queue issues ONE flush
 * request of its own (q->flush_rq) for everything waiting there, and when
 * it completes, every request it served moves on to its next step.  A
 * data step and the flush request go to the requeue lists, from which
 * kblockd puts them at the head of the dispatch list.  While data steps
 * are in flight a new flush waits up to BLK_FLUSH_PENDING_TIMEOUT_NS so
 * that it can serve their post-flushes too.  A FLUSH request of a
 * submitter never reaches the driver itself.  All under q->flush_lock,
 * which completions take (never the elevator lock); requests whose
 * sequence is DONE are ended after it (blk_flush_end_request).
 * ========================================================================== */

/* The next step of a request in a flush sequence. */
static uint32_t blk_flush_cur_seq(const struct blk_request *rq)
{
	uint32_t todo = BLK_SEQ_ACTIONS & ~rq->bl.seq;

	return todo ? (todo & (~todo + 1)) : BLK_SEQ_DONE;
}

static void blk_flush_list_add_locked(struct blk_queue *q, int idx,
				      struct blk_request *rq)
{
	rq->bl.flush_next = NULL;
	if (q->flush_tail[idx])
		q->flush_tail[idx]->bl.flush_next = rq;
	else
		q->flush_list[idx] = rq;
	q->flush_tail[idx] = rq;
}

/* Issue the queue's flush request for what waits on the pending list, if
 * none runs and the pending ones have waited long enough with data steps
 * in flight.  Returns 1 when the requeue work must run.  flush_lock
 * held. */
static int blk_kick_flush_locked(struct blk_queue *q, uint64_t now)
{
	int pidx = q->flush_pending_idx;
	struct blk_request *first = q->flush_list[pidx];
	struct blk_request *f = &q->flush_rq;

	lockdep_assert_held(&q->flush_lock);
	if (q->flush_pending_idx != q->flush_running_idx || !first)
		return 0;
	if (q->flush_data_in_flight &&
	    now < q->flush_pending_since + BLK_FLUSH_PENDING_TIMEOUT_NS)
		return 0;
	q->flush_pending_idx ^= 1;

	blk_request_init(f, first->bdev, BLK_OP_FLUSH, 0, 0);
	f->flags = BLK_REQ_NO_ACCOUNT |
		   (first->flags & (BLK_REQ_PM | BLK_REQ_FAILFAST |
				    BLK_REQ_QUIET | BLK_REQ_POLLED));
	f->queue = q;
	f->disk = q->disk;
	f->submit_cpu = first->submit_cpu;
	f->submit_ns = now;
	f->bl.internal |= BLK_RQI_FLUSH_RQ;
	f->bl.tag = -1;
	f->state = BLK_RQ_QUEUED;
	if (blk_queue_add_requeue(q, f, 1)) {
		/* Dying: teardown fails what waits on the flush lists. */
		q->flush_pending_idx ^= 1;
		f->state = BLK_RQ_IDLE;
		return 0;
	}
	return 1;
}

/* Move `rq' past the step `seq' (0: none, just placed) with `error'.
 * Returns 1 when the requeue work must run; a request whose sequence is
 * DONE is linked onto *done for blk_flush_end_request(), as is one that
 * meets a dying queue.  flush_lock held. */
static int blk_flush_complete_seq_locked(struct blk_queue *q,
					 struct blk_request *rq, uint32_t seq,
					 int error, struct blk_request **done)
{
	uint32_t next;
	int kick = 0;

	lockdep_assert_held(&q->flush_lock);
	VM_BUG_ON(rq->bl.seq & seq);
	rq->bl.seq |= seq;
	next = error ? BLK_SEQ_DONE : blk_flush_cur_seq(rq);
	/* A dying queue issues no flush any more: what would wait for one
	 * ends now.  (Teardown sets DYING before it empties the flush lists
	 * under this lock: a request is either ended here or collected
	 * there.) */
	if (next != BLK_SEQ_DONE && next != BLK_SEQ_DATA &&
	    (blk_qflags(q) & BLK_QUEUE_FL_DYING)) {
		error = -ENODEV;
		next = BLK_SEQ_DONE;
	}

	switch (next) {
	case BLK_SEQ_PREFLUSH:
	case BLK_SEQ_POSTFLUSH:
		if (!q->flush_list[q->flush_pending_idx])
			q->flush_pending_since = hrtimer_now_ns();
		blk_flush_list_add_locked(q, q->flush_pending_idx, rq);
		break;
	case BLK_SEQ_DATA:
		/* to the head of the queue; kblockd dispatches it */
		rq->state = BLK_RQ_QUEUED;
		rq->bl.tag = -1;
		if (blk_queue_add_requeue(q, rq, 0)) {
			rq->status = -ENODEV;
			rq->bl.flush_next = *done;
			*done = rq;
			break;
		}
		q->flush_data_in_flight++;
		kick = 1;
		break;
	default: /* DONE */
		if (error)
			rq->status = error;
		rq->bl.flush_next = *done;
		*done = rq;
		break;
	}
	return blk_kick_flush_locked(q, hrtimer_now_ns()) || kick;
}

/* The end of a flush sequence: the request ends like any other. */
static void blk_flush_end_request(struct blk_request *rq)
{
	int status = rq->status;
	uint32_t resid = rq->resid_bytes;

	rq->bl.internal |= BLK_RQI_FLUSH_END;
	/* a pre-flush failed: the data never went out */
	if (status && !(rq->bl.seq & BLK_SEQ_DATA) && blk_op_has_data(rq->op))
		resid = blk_rq_bytes_for_resid(rq);
	blk_request_complete(rq, status, resid);
}

static void blk_flush_end_list(struct blk_request *done)
{
	while (done) {
		struct blk_request *rq = done;

		done = rq->bl.flush_next;
		rq->bl.flush_next = NULL;
		blk_flush_end_request(rq);
	}
}

/* A request enters the flush machinery (blk_insert_flush). */
static void blk_insert_flush(struct blk_queue *q, struct blk_request *rq)
{
	struct blk_request *done = NULL;
	uint64_t fl;
	int kick;

	rq->status = 0;
	rq->resid_bytes = 0;
	spin_lock_irqsave(&q->flush_lock, &fl);
	kick = blk_flush_complete_seq_locked(q, rq, 0, 0, &done);
	spin_unlock_irqrestore(&q->flush_lock, fl);
	if (kick)
		blk_queue_kick_requeue(q);
	blk_flush_end_list(done);
}

void blk_flush_complete(struct blk_queue *q, struct blk_request *rq,
			int status, uint32_t resid)
{
	struct blk_request *done = NULL;
	uint64_t fl;
	int kick = 0, tag;

	/* Its tag first (the flush request's end and the data step's end
	 * both let it go), the state machine under its own lock. */
	tag = rq->bl.tag;
	rq->bl.tag = -1;
	spin_lock_irqsave(&q->flush_lock, &fl);
	if (rq->bl.internal & BLK_RQI_FLUSH_RQ) {
		/* The queue's flush ended: everything it served moves on. */
		int ridx = q->flush_running_idx;
		struct blk_request *list = q->flush_list[ridx];

		q->flush_list[ridx] = q->flush_tail[ridx] = NULL;
		q->flush_running_idx ^= 1;
		rq->state = BLK_RQ_IDLE;
		while (list) {
			struct blk_request *r = list;

			list = r->bl.flush_next;
			r->bl.flush_next = NULL;
			kick |= blk_flush_complete_seq_locked(
				q, r, blk_flush_cur_seq(r), status, &done);
		}
	} else {
		/* A data step ended. */
		WARN_ON(q->flush_data_in_flight == 0);
		if (q->flush_data_in_flight)
			q->flush_data_in_flight--;
		rq->resid_bytes = resid;
		if (!status && resid)
			status = -EIO; /* short, and nothing to flush it with */
		kick |= blk_flush_complete_seq_locked(q, rq, BLK_SEQ_DATA,
						      status, &done);
	}
	kick |= blk_kick_flush_locked(q, hrtimer_now_ns());
	spin_unlock_irqrestore(&q->flush_lock, fl);
	if (tag >= 0)
		blk_tag_put(q, tag);
	/* The tag the step held is free: a dispatch waiting for one goes on. */
	blk_queue_sched_restart(q);
	blk_queue_wake(q);
	if (kick)
		blk_queue_kick_requeue(q);
	blk_flush_end_list(done);
}

void blk_queue_stop(struct blk_queue *q)
{
	uint64_t fl;

	if (!q)
		return;
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_set(q, BLK_QUEUE_FL_STOPPED);
	spin_unlock_irqrestore(&q->lock, fl);
}

void blk_queue_start(struct blk_queue *q)
{
	uint64_t fl;

	if (!q)
		return;
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_clear(q, BLK_QUEUE_FL_STOPPED);
	spin_unlock_irqrestore(&q->lock, fl);
	blk_queue_run(q);
}

/* Restart dispatch the way error handling ends it: the queue is no longer
 * stopped, and what waits on the requeue list and in the elevator is run
 * by kblockd -- never inline in the caller, which may be the error
 * handler's own thread.  Any context. */
void blk_queue_start_async(struct blk_queue *q)
{
	uint64_t fl;

	if (!q)
		return;
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_clear(q, BLK_QUEUE_FL_STOPPED);
	spin_unlock_irqrestore(&q->lock, fl);
	blk_queue_kick_requeue(q);
	blk_queue_run_async(q);
}

/* ==========================================================================
 * Request timeouts
 *
 * The timer's work walks the busy tags.  For each it reads the slot's
 * word, the request pointer and what else the report needs, and the word
 * once more: unchanged, all of it belongs to one dispatch.  An expired
 * request is claimed with a compare-and-exchange of the word (IN_FLIGHT
 * -> TIMED_OUT, the same generation): a completion that got there first
 * makes the claim fail, and once claimed the dispatch is handed to the
 * driver's ->timeout exactly once.  The request itself is never read --
 * the driver only compares the pointer with its own commands.  The timer
 * is re-armed for the earliest deadline left.
 * ========================================================================== */

/* Expired requests handled per pass of the work (the rest: next pass). */
#define BLK_TIMEOUT_BATCH 32

struct blk_expired {
	struct blk_request *rq;
	void *driver_data;
	uint64_t word; /* as claimed: TIMED_OUT */
	uint64_t ms; /* at the driver for this long */
	uint32_t tag;
	uint32_t timeout_ms;
	uint8_t quiet;
};

/* The driver's verdict on one expired request; another period when it
 * asks for one and the request is still the one at the tag. */
static uint64_t blk_queue_rq_timed_out(struct blk_queue *q,
				       const struct blk_expired *e)
{
	struct gendisk *disk = q->disk;
	enum blk_eh_timer_return ret = BLK_EH_RESET_TIMER;
	struct blk_tag_slot *s = &q->tags[e->tag];
	uint64_t now, dl_ms, w;

	if (disk && disk->ops && disk->ops->timeout) {
		ret = disk->ops->timeout(disk, e->rq, e->driver_data);
		if (ret == BLK_EH_DONE)
			return 0;
		WARN_ON_ONCE(ret != BLK_EH_RESET_TIMER);
	}
	now = hrtimer_now_ns();
	dl_ms = blk_ns_to_ms_up(now + (uint64_t)e->timeout_ms * 1000000ULL);
	w = e->word;
	if (!__atomic_compare_exchange_n(&s->word, &w,
					 blk_tag_word(dl_ms, blk_tag_gen(w),
						      BLK_TAG_IN_FLIGHT),
					 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
		return 0; /* completed meanwhile */
	blk_add_timer(q, dl_ms * 1000000ULL, now);
	return dl_ms;
}

/* The scan.  `rearm': arm the timer for the earliest deadline left (the
 * timeout work); a polled scan leaves the timer alone. */
static void blk_queue_timeout_scan(struct blk_queue *q, int rearm)
{
	struct blk_expired exp[BLK_TIMEOUT_BATCH];
	uint64_t now, now_ms, next = 0, busy;
	int n = 0, more = 0;

	(void)__atomic_add_fetch(&q->active, 1, __ATOMIC_SEQ_CST);
	now = hrtimer_now_ns();
	now_ms = blk_ns_to_ms(now);
	busy = __atomic_load_n(&q->tag_busy, __ATOMIC_ACQUIRE);
	while (busy) {
		uint32_t t = (uint32_t)__builtin_ctzll(busy);
		struct blk_tag_slot *s = &q->tags[t];
		struct blk_request *rq;
		void *dd;
		uint64_t w, w2, dns, dl;
		uint32_t rfl, tmo;

		busy &= busy - 1;
		w = __atomic_load_n(&s->word, __ATOMIC_ACQUIRE);
		if (blk_tag_state(w) != BLK_TAG_IN_FLIGHT)
			continue;
		rq = __atomic_load_n(&s->rq, __ATOMIC_RELAXED);
		dd = __atomic_load_n(&s->driver_data, __ATOMIC_RELAXED);
		dns = __atomic_load_n(&s->dispatch_ns, __ATOMIC_RELAXED);
		rfl = __atomic_load_n(&s->rq_flags, __ATOMIC_RELAXED);
		tmo = __atomic_load_n(&s->timeout_ms, __ATOMIC_RELAXED);
		/* The reads above complete before the word is read again. */
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		w2 = __atomic_load_n(&s->word, __ATOMIC_RELAXED);
		if (w2 != w)
			continue; /* ended, or a new dispatch */
		dl = blk_tag_deadline(w);
		if (now_ms < dl) {
			if (!next || dl < next)
				next = dl;
			continue;
		}
		if (n == BLK_TIMEOUT_BATCH) {
			more = 1;
			continue;
		}
		w2 = blk_tag_word(dl, blk_tag_gen(w), BLK_TAG_TIMED_OUT);
		if (!__atomic_compare_exchange_n(&s->word, &w, w2, 0,
						 __ATOMIC_SEQ_CST,
						 __ATOMIC_RELAXED))
			continue; /* completed meanwhile */
		exp[n].rq = rq;
		exp[n].driver_data = dd;
		exp[n].word = w2;
		exp[n].ms = now > dns ? (now - dns) / 1000000ULL : 0;
		exp[n].tag = t;
		exp[n].timeout_ms = tmo;
		exp[n].quiet = (rfl & BLK_REQ_QUIET) != 0;
		n++;
	}

	for (int i = 0; i < n; i++) {
		uint64_t dl;

		if (!exp[i].quiet)
			kprintf(BLK_LOG_PREFIX "%s: request timed out after %llu ms\n",
				q->disk ? q->disk->disk_name : "?",
				(unsigned long long)exp[i].ms);
		dl = blk_queue_rq_timed_out(q, &exp[i]);
		/* Its timer started again: its new deadline counts. */
		if (dl && (!next || dl < next))
			next = dl;
	}

	/* A forward rolling timer: armed for the earliest deadline left, idle
	 * when nothing is in flight. */
	if (next && rearm)
		blk_queue_mod_timeout_timer(q, next * 1000000ULL);
	blk_queue_leave(q);
	if (more && rearm)
		(void)kblockd_schedule_work(&q->timeout_work);
}

static void blk_queue_timeout_work_fn(void *arg)
{
	blk_queue_timeout_scan((struct blk_queue *)arg, 1);
}

void blk_queue_poll_timeouts(struct blk_queue *q)
{
	uint64_t now_ms, busy;

	if (!q)
		return;
	/* Only what is due: a polled waiter calls this every few
	 * microseconds. */
	now_ms = blk_ns_to_ms(hrtimer_now_ns());
	busy = __atomic_load_n(&q->tag_busy, __ATOMIC_ACQUIRE);
	while (busy) {
		uint32_t t = (uint32_t)__builtin_ctzll(busy);
		uint64_t w = __atomic_load_n(&q->tags[t].word, __ATOMIC_ACQUIRE);

		busy &= busy - 1;
		if (blk_tag_state(w) == BLK_TAG_IN_FLIGHT &&
		    now_ms >= blk_tag_deadline(w)) {
			blk_queue_timeout_scan(q, 0);
			return;
		}
	}
}

/* ==========================================================================
 * Plugging: per task
 *
 * A task that submits a batch opens a plug (blk_start_plug, on its
 * stack): the requests it submits meanwhile -- not flushes, not
 * pass-through -- collect on the plug instead of reaching their queue one
 * by one.  The plug is flushed into the queues (sorted and merged by the
 * elevator there) when the task finishes it, when it holds
 * BLK_MAX_REQUEST_COUNT requests (twice that across several queues) or a
 * request of BLK_PLUG_FLUSH_SIZE bytes or more, and when the task is
 * about to sleep (blk_flush_plug from sched_schedule).  Flushed on
 * finish, the queues are run by the task; flushed on its way to sleep,
 * by kblockd -- and a request that meets a dying queue there is ended
 * through the block softirq, not inside the scheduler's hook.  No timer.
 * ========================================================================== */

void blk_start_plug(struct blk_plug *plug)
{
	task_t *cur = sched_current();

	BUG_ON(plug == NULL);
	/* Nested plugs are a no-op: the outermost one is flushed. */
	if (!cur || cur->plug)
		return;
	plug->head = plug->tail = NULL;
	plug->rq_count = 0;
	plug->multiple_queues = 0;
	cur->plug = plug;
}

/* Insert what the plug holds into its queues, then run each queue once:
 * here (the task finishing its plug), or by kblockd (`from_schedule':
 * the task is going to sleep). */
static void blk_flush_plug_list(struct blk_plug *plug, int from_schedule)
{
	struct blk_request *list = plug->head;
	struct blk_queue *runq[BLK_MAX_REQUEST_COUNT * 2];
	int nrun = 0;

	plug->head = plug->tail = NULL;
	plug->rq_count = 0;
	plug->multiple_queues = 0;
	while (list) {
		struct blk_request *rq = list;
		struct blk_queue *q = rq->queue;
		int seen = 0;

		list = rq->disp_next;
		rq->disp_next = NULL;
		(void)blk_queue_add_request(q, rq, from_schedule);
		for (int i = 0; i < nrun; i++)
			if (runq[i] == q)
				seen = 1;
		if (!seen && nrun < (int)(sizeof(runq) / sizeof(runq[0])))
			runq[nrun++] = q;
		/* each request holds the queue (blk_submit) until it is in */
		blk_queue_leave(q);
	}
	for (int i = 0; i < nrun; i++) {
		if (from_schedule)
			blk_queue_run_async(runq[i]);
		else
			blk_queue_run(runq[i]);
	}
}

void blk_flush_plug(struct blk_plug *plug, int from_schedule)
{
	if (plug && plug->head)
		blk_flush_plug_list(plug, from_schedule);
}

void blk_finish_plug(struct blk_plug *plug)
{
	task_t *cur = sched_current();

	if (!cur || plug != cur->plug)
		return;
	blk_flush_plug(plug, 0);
	cur->plug = NULL;
}

/* Collect `rq' on the submitting task's plug.  1 when it was taken (the
 * request keeps the queue entered until it is flushed), 0 when it goes
 * to its queue now. */
int blk_add_rq_to_plug(struct blk_request *rq)
{
	task_t *cur;
	struct blk_plug *plug;
	struct blk_request *last;

	if (!blk_can_sleep())
		return 0;
	cur = sched_current();
	plug = cur ? (struct blk_plug *)cur->plug : NULL;
	if (!plug)
		return 0;
	/* Flushes and pass-through go to the queue at once, as do requests
	 * that must not wait (head of the queue, power management). */
	if ((rq->bl.internal & BLK_RQI_FLUSH_SEQ) || blk_rq_is_bypass(rq))
		return 0;
	last = plug->tail;
	if (plug->rq_count >= (plug->multiple_queues ?
				       2 * BLK_MAX_REQUEST_COUNT :
				       BLK_MAX_REQUEST_COUNT) ||
	    (last && !(blk_qflags(rq->queue) & BLK_QUEUE_FL_NOMERGES) &&
	     (uint64_t)last->count *
			     (last->bdev ? last->bdev->sector_size : 512) >=
		     BLK_PLUG_FLUSH_SIZE)) {
		blk_flush_plug_list(plug, 0);
		last = NULL;
	}
	if (!plug->multiple_queues && last && last->queue != rq->queue)
		plug->multiple_queues = 1;
	rq->disp_next = NULL;
	if (plug->tail)
		plug->tail->disp_next = rq;
	else
		plug->head = rq;
	plug->tail = rq;
	plug->rq_count++;
	return 1;
}

/* ==========================================================================
 * Settings
 * ========================================================================== */

void blk_queue_set_depth(struct blk_queue *q, uint32_t depth)
{
	uint64_t fl;

	if (!q)
		return;
	if (depth == 0)
		depth = 1;
	if (depth > BLK_QUEUE_MAX_TAGS) {
		kprintf(BLK_LOG_PREFIX "%s: queue depth %u capped at %u\n",
			q->disk ? q->disk->disk_name : "?", depth,
			(unsigned)BLK_QUEUE_MAX_TAGS);
		depth = BLK_QUEUE_MAX_TAGS;
	}
	spin_lock_irqsave(&q->lock, &fl);
	if (depth != q->depth && q->disk)
		kprintf(BLK_LOG_PREFIX "%s: queue depth %u -> %u\n",
			q->disk->disk_name, q->depth, depth);
	__atomic_store_n(&q->depth, depth, __ATOMIC_RELEASE);
	q->limits.queue_depth = depth;
	/* The slots follow the depth upwards (a queue created before its
	 * device's queueing was known starts at depth 1). */
	if (q->nr_requests < blk_default_nr_requests(depth)) {
		q->nr_requests = blk_default_nr_requests(depth);
		__atomic_store_n(&q->async_depth, q->nr_requests,
				 __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&q->lock, fl);
	blk_queue_run(q);
}

static int blk_name_is(const char *a, const char *b)
{
	/* compare, ignoring one trailing newline in `a' (sysfs writes) */
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	if (*b)
		return 0;
	return *a == 0 || (*a == '\n' && a[1] == 0);
}

int blk_queue_set_scheduler(struct blk_queue *q, const char *name)
{
	uint64_t fl;
	enum blk_sched s;

	if (!q || !name)
		return -EINVAL;
	if (blk_name_is(name, "mq-deadline") || blk_name_is(name, "deadline"))
		s = BLK_SCHED_DEADLINE;
	else if (blk_name_is(name, "none") || blk_name_is(name, "noop"))
		s = BLK_SCHED_NONE;
	else
		return -EINVAL;
	spin_lock_irqsave(&q->lock, &fl);
	/* The lists are the same for both policies; only the choice of the
	 * next request differs, so a switch needs no migration. */
	q->sched = s;
	q->batching = q->fifo_batch; /* start a fresh batch */
	spin_unlock_irqrestore(&q->lock, fl);
	if (q->disk)
		kprintf(BLK_LOG_PREFIX "%s: I/O scheduler %s\n",
			q->disk->disk_name, blk_queue_sched_name(q));
	blk_queue_run(q);
	return 0;
}

const char *blk_queue_sched_name(const struct blk_queue *q)
{
	return q && q->sched == BLK_SCHED_NONE ? "none" : "mq-deadline";
}

long blk_queue_sched_show(const struct blk_queue *q, char *buf, long cap)
{
	const char *t = q && q->sched == BLK_SCHED_NONE ? "[none] mq-deadline\n" :
							  "none [mq-deadline]\n";
	long n = 0;

	while (t[n]) {
		if (n < cap)
			buf[n] = t[n];
		n++;
	}
	return n;
}

void blk_queue_suspend(struct blk_queue *q)
{
	uint64_t fl;

	if (!q)
		return;
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_set(q, BLK_QUEUE_FL_SUSPENDED);
	spin_unlock_irqrestore(&q->lock, fl);
}

void blk_queue_resume(struct blk_queue *q)
{
	uint64_t fl;

	if (!q)
		return;
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_clear(q, BLK_QUEUE_FL_SUSPENDED);
	spin_unlock_irqrestore(&q->lock, fl);
	blk_queue_run(q);
}

/* ==========================================================================
 * Statistics snapshots
 * ========================================================================== */

void blk_queue_stats_snapshot(struct blk_queue *q,
			      const struct blk_part_stats *s,
			      struct blk_disk_stats *out)
{
	uint64_t inflight[2], nsecs_all = 0;

	(void)q;
	if (!s || !out)
		return;
	mm_memset(out, 0, sizeof(*out));
	blk_part_inflight(s, inflight);
	/* Bring the busy time up to now so an idle reader sees it advance
	 * while a long request is in flight. */
	if (inflight[0] + inflight[1]) {
		uint64_t fl = local_irq_save();

		blk_update_io_ticks((struct blk_part_stats *)s, blk_stat_slot(),
				    blk_ns_to_ms(hrtimer_now_ns()), 1);
		local_irq_restore(fl);
	}
	for (uint32_t c = 0; c < BLK_STAT_CPUS; c++) {
		const struct blk_stat_cpu *p = &s->cpu[c];

		for (int g = 0; g < BLK_STAT_NR; g++) {
			out->ios[g] += __atomic_load_n(&p->ios[g],
						       __ATOMIC_RELAXED);
			out->merges[g] += __atomic_load_n(&p->merges[g],
							  __ATOMIC_RELAXED);
			out->sectors[g] += __atomic_load_n(&p->sectors[g],
							   __ATOMIC_RELAXED);
			out->ticks_ms[g] += __atomic_load_n(&p->nsecs[g],
							    __ATOMIC_RELAXED);
		}
		out->io_ticks_ms += __atomic_load_n(&p->io_ticks_ms,
						    __ATOMIC_RELAXED);
	}
	/* ticks_ms held nanoseconds while summing. */
	for (int g = 0; g < BLK_STAT_NR; g++) {
		nsecs_all += out->ticks_ms[g];
		out->ticks_ms[g] = blk_ns_to_ms(out->ticks_ms[g]);
	}
	out->time_in_queue_ms = blk_ns_to_ms(nsecs_all);
	out->in_flight[0] = inflight[0];
	out->in_flight[1] = inflight[1];
	out->stamp_ns = __atomic_load_n(&s->stamp_ms, __ATOMIC_RELAXED) *
			1000000ULL;
}

void blk_queue_inflight(struct blk_queue *q, const struct blk_part_stats *s,
			uint64_t out[2])
{
	(void)q;
	out[0] = out[1] = 0;
	if (!s)
		return;
	blk_part_inflight(s, out);
}

/* ==========================================================================
 * Creation and teardown
 * ========================================================================== */

/* Fill the gaps of a driver's limits with the conventional defaults and
 * make the relations between them hold. */
static void blk_limits_normalize(struct blk_limits *l, const char *name)
{
	if (!l->logical_block_size || (l->logical_block_size &
				       (l->logical_block_size - 1)) ||
	    l->logical_block_size < 512 || l->logical_block_size > 65536) {
		if (l->logical_block_size)
			kprintf(BLK_LOG_PREFIX
				"%s: logical block size %u is invalid, using 512\n",
				name, l->logical_block_size);
		l->logical_block_size = 512;
	}
	if (l->physical_block_size < l->logical_block_size ||
	    (l->physical_block_size & (l->physical_block_size - 1)))
		l->physical_block_size = l->logical_block_size;
	if (!l->io_min)
		l->io_min = l->physical_block_size;
	if (l->alignment_offset >= l->physical_block_size)
		l->alignment_offset %= l->physical_block_size;
	if (!l->max_hw_sectors)
		l->max_hw_sectors = 256;
	if (!l->max_sectors || l->max_sectors > l->max_hw_sectors)
		l->max_sectors = l->max_hw_sectors;
	if (!l->max_segments)
		l->max_segments = BLK_DEFAULT_MAX_SEGMENTS;
	if (!l->max_segment_size)
		l->max_segment_size = 65536;
	if (!l->queue_depth)
		l->queue_depth = 1;
	if (!l->discard_granularity)
		l->max_discard_sectors = 0;
	if (l->max_discard_sectors && !l->max_discard_segments)
		l->max_discard_segments = 1;
}

struct blk_queue *blk_queue_create(struct gendisk *disk,
				   const struct blk_limits *lim)
{
	struct blk_queue *q;
	const char *name = disk ? disk->disk_name : "?";

	q = (struct blk_queue *)kcalloc(1, sizeof(*q));
	if (!q)
		return NULL;
	q->disk = disk;
	spinlock_init(&q->lock, "blk_queue");
	spinlock_init(&q->wait_lock, "blk_queue_wait");
	spinlock_init(&q->requeue_lock, "blk_requeue");
	spinlock_init(&q->timer_lock, "blk_queue_timer");
	spinlock_init(&q->flush_lock, "blk_flush");
	if (lim)
		q->limits = *lim;
	blk_limits_normalize(&q->limits, name);
	q->sched = BLK_SCHED_DEADLINE;
	q->read_expire_ms = BLK_DEFAULT_READ_EXPIRE_MS;
	q->write_expire_ms = BLK_DEFAULT_WRITE_EXPIRE_MS;
	q->fifo_batch = BLK_DEFAULT_FIFO_BATCH;
	q->writes_starved = BLK_DEFAULT_WRITES_STARVED;
	/* No batch is running yet: the first dispatch starts one (and goes
	 * through the read/write choice and the starvation count). */
	q->batching = q->fifo_batch;
	if (q->limits.queue_depth > BLK_QUEUE_MAX_TAGS)
		q->limits.queue_depth = BLK_QUEUE_MAX_TAGS;
	q->depth = q->limits.queue_depth;
	q->nr_requests = blk_default_nr_requests(q->depth);
	q->async_depth = q->nr_requests;
	q->rq_affinity = 1;
	q->read_ahead_kb = 128;
	hrtimer_init(&q->timeout_timer, blk_queue_timeout_timer_fn, q);
	blk_work_init(&q->run_work, blk_queue_run_work_fn, q);
	blk_work_init(&q->timeout_work, blk_queue_timeout_work_fn, q);
	blk_work_init(&q->requeue_work, blk_queue_requeue_work_fn, q);
	wq_head_init(&q->wait_free, "blk_wait_free");
	wq_head_init(&q->wait_free_async, "blk_wait_free_async");
	wq_head_init(&q->wait_drain, "blk_wait_drain");

	/* The merge pool.  Without it the queue works, it only never
	 * merges data requests. */
	q->merge_pool_seg = q->limits.max_segments;
	if (q->merge_pool_seg > BLK_MERGE_POOL_SEG_MAX)
		q->merge_pool_seg = BLK_MERGE_POOL_SEG_MAX;
	q->merge_pool_n = q->depth * 2;
	if (q->merge_pool_n < BLK_MERGE_POOL_N_MIN)
		q->merge_pool_n = BLK_MERGE_POOL_N_MIN;
	if (q->merge_pool_n > BLK_MERGE_POOL_N_MAX)
		q->merge_pool_n = BLK_MERGE_POOL_N_MAX;
	q->merge_pool = (struct blk_sg_entry *)kcalloc(
		(size_t)q->merge_pool_n * q->merge_pool_seg,
		sizeof(struct blk_sg_entry));
	if (!q->merge_pool) {
		kprintf(BLK_LOG_PREFIX "%s: no memory for the merge pool; requests will not be merged\n",
			name);
		q->merge_pool_n = 0;
	}
	q->merge_pool_used = 0;
	return q;
}

/* Move a list kept through disp_next onto *tailp, counting it; returns
 * the new tail pointer. */
static struct blk_request **blk_collect_list(struct blk_request *list,
					     struct blk_request **tailp,
					     uint32_t *count)
{
	while (list) {
		struct blk_request *n = list->disp_next;

		*tailp = list;
		tailp = &list->disp_next;
		list = n;
		(*count)++;
	}
	*tailp = NULL;
	return tailp;
}

void blk_queue_destroy(struct blk_queue *q)
{
	uint64_t fl;
	struct blk_request *fail = NULL, **tailp = &fail;
	struct blk_request *flush_fail = NULL;
	int rc;

	if (!q)
		return;
	blk_might_sleep();
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_set(q, BLK_QUEUE_FL_DYING);
	blk_qflags_clear(q, BLK_QUEUE_FL_SUSPENDED);
	/* Everything that has not reached the driver fails with -ENODEV.
	 * Collected under the lock, completed after it. */
	while (q->disp_head) {
		struct blk_request *rq = q->disp_head;

		blk_bypass_unlink_locked(q, NULL, rq);
		*tailp = rq;
		tailp = &rq->disp_next;
	}
	for (int dir = 0; dir < 2; dir++) {
		while (q->fifo_head[dir]) {
			struct blk_request *rq = q->fifo_head[dir];

			blk_elv_remove_locked(q, rq);
			rq->disp_next = NULL;
			*tailp = rq;
			tailp = &rq->disp_next;
		}
	}
	*tailp = NULL;
	spin_unlock_irqrestore(&q->lock, fl);

	/* What waits on the requeue lists (DYING is set: nothing is added
	 * there any more). */
	spin_lock_irqsave(&q->requeue_lock, &fl);
	{
		uint32_t n = 0;

		tailp = blk_collect_list(q->rq_flush_head, tailp, &n);
		tailp = blk_collect_list(q->requeue_head, tailp, &n);
		q->requeue_head = q->requeue_tail = NULL;
		q->rq_flush_head = q->rq_flush_tail = NULL;
		/* A requeue work under way counts down what it took itself. */
		(void)__atomic_sub_fetch(&q->nr_requeue, n, __ATOMIC_SEQ_CST);
	}
	spin_unlock_irqrestore(&q->requeue_lock, fl);

	/* Requests waiting for a flush step that was not issued yet. */
	spin_lock_irqsave(&q->flush_lock, &fl);
	{
		int pidx = q->flush_pending_idx;

		if (pidx != q->flush_running_idx || q->flush_list[pidx]) {
			struct blk_request *w = q->flush_list[pidx];

			q->flush_list[pidx] = q->flush_tail[pidx] = NULL;
			while (w) {
				struct blk_request *n = w->bl.flush_next;

				w->status = -ENODEV;
				w->bl.flush_next = flush_fail;
				flush_fail = w;
				w = n;
			}
		}
	}
	spin_unlock_irqrestore(&q->flush_lock, fl);

	/* No new arming after DYING (the arming checks it under the timer
	 * lock); a firing already under way finds DYING and only drops its
	 * count. */
	spin_lock_irqsave(&q->timer_lock, &fl);
	blk_queue_disarm_timer_locked(q);
	spin_unlock_irqrestore(&q->timer_lock, fl);
	/* Submitters waiting for room see DYING. */
	blk_queue_wake(q);

	while (fail) {
		struct blk_request *rq = fail;

		fail = rq->disp_next;
		rq->disp_next = NULL;
		blk_queue_fail_queued(q, rq);
	}
	blk_flush_end_list(flush_fail);

	/* What is at the driver must come back from it (the driver fails
	 * it after a hot removal).  No timeout can make that safe, so wait,
	 * and say so every 10 s. */
	for (;;) {
		rc = blk_queue_wait_cond(q, &q->wait_drain, blk_cond_quiet,
					 10000);
		if (!rc)
			break;
		kprintf(BLK_LOG_PREFIX "%s: still waiting for %u in-flight request%s before removal\n",
			q->disk ? q->disk->disk_name : "?",
			blk_queue_nr_in_flight(q),
			blk_queue_nr_in_flight(q) == 1 ? "" : "s");
	}
	/* The memory stays until the disk's last reference (blk_queue_free):
	 * a holder of an open handle may still look at the queue and must
	 * find it dying, not freed. */
}

void blk_queue_free(struct blk_queue *q)
{
	uint64_t fl;
	uint32_t fires = 0;

	if (!q)
		return;
	/* hrtimer_cancel() does not wait for a callback that already runs on
	 * another processor, and the callback touches the queue until it
	 * drops its firing count: wait until the count of outstanding
	 * firings is 0.  Bounded: a callback that never finishes leaks the
	 * queue rather than freeing memory that is still in use. */
	spin_lock_irqsave(&q->lock, &fl);
	blk_qflags_set(q, BLK_QUEUE_FL_DYING);
	spin_unlock_irqrestore(&q->lock, fl);
	for (uint32_t spin = 0; spin < 100000; spin++) {
		spin_lock_irqsave(&q->timer_lock, &fl);
		blk_queue_disarm_timer_locked(q);
		spin_unlock_irqrestore(&q->timer_lock, fl);
		fires = __atomic_load_n(&q->timer_fires, __ATOMIC_ACQUIRE);
		if (!fires)
			break;
		lapic_delay_us(10);
	}
	if (fires) {
		WARN(1, BLK_LOG_PREFIX "%s: queue timer callback does not finish; leaking the queue",
		     q->disk ? q->disk->disk_name : "?");
		return;
	}
	/* A run, a requeue or a timeout scan handed to kblockd must not find
	 * the memory gone: take it off the list, or wait for the run under
	 * way -- bounded like the timer wait above. */
	if (blk_work_cancel_sync(&q->run_work, 1000) ||
	    blk_work_cancel_sync(&q->requeue_work, 1000) ||
	    blk_work_cancel_sync(&q->timeout_work, 1000)) {
		WARN(1, BLK_LOG_PREFIX "%s: queue work on kblockd does not finish; leaking the queue",
		     q->disk ? q->disk->disk_name : "?");
		return;
	}
	WARN_ON(blk_queue_nr_in_flight(q) != 0);
	WARN_ON(q->disp_head != NULL || q->fifo_head[0] || q->fifo_head[1]);
	WARN_ON(q->requeue_head != NULL || q->rq_flush_head != NULL);
	WARN_ON(q->wait_free.first != NULL || q->wait_free_async.first != NULL ||
		q->wait_drain.first != NULL);
	if (q->merge_pool)
		kfree(q->merge_pool);
	q->merge_pool = NULL;
	kfree(q);
}
