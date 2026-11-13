// LikeOS - the multi-queue block layer contract (blk-mq)
//
// A fast storage controller (NVMe) has many hardware submission queues --
// typically one per CPU, each with its own interrupt -- and thousands of
// commands in flight.  The request queue of blkdev.h (one dispatch path,
// one 64-entry tag table) cannot feed it.  This header is the multi-queue
// layer on top of it, implemented in kernel/dev/block/blk_mq*.c with
// extensions of blk_queue.c, block.c, blk_softirq.c and blk_sysfs.c
// (Phase 1 agent P2), following the established blk-mq design:
//
//   * a TAG SET (struct blk_mq_tag_set) describes the hardware: how many
//     hardware queues, how deep each is, how many tags are reserved (for
//     driver-internal commands such as the NVMe asynchronous event
//     requests), the per-command driver data (cmd_size bytes, the "PDU",
//     allocated per tag by the block layer), and the driver's operations
//     (struct blk_mq_ops).  One tag set may serve several queues (all
//     namespaces of one NVMe controller share the I/O tag set; the
//     controller's admin queue has a tag set of its own);
//   * a HARDWARE CONTEXT (struct blk_mq_hw_ctx, "hctx") per hardware queue
//     and queue: its tags (a sbitmap-style allocator, so depths of
//     thousands), dispatch list, run work, and the CPUs that submit to it;
//   * QUEUE MAPS per hctx type (default, read, poll) assign every CPU to
//     one hctx of each type; the driver's map_queues() fills them, usually
//     with blk_mq_map_hw_queues() from the interrupt affinity of the
//     device, so a CPU submits to the queue whose interrupt it takes;
//   * FREEZE (no new requests enter, wait for all in flight to finish) and
//     QUIESCE (no dispatch into the driver, requests keep entering) are
//     the two ways a driver's recovery stops a queue;
//   * POLL queues have no interrupt: the submitter (or blk_mq_poll) reaps
//     completions with ops->poll(), optionally after a hybrid sleep of
//     about half the mean completion time;
//   * INTEGRITY (T10 PI / NVMe protection information), ZONED devices and
//     ATOMIC writes are described by queue limits and handled per request.
//
// Compatibility: a disk added WITHOUT a tag set (AHCI, usb_msd) keeps the
// exact single-queue behaviour of blkdev.h -- its blk_ops->submit path,
// elevator and 64-tag table are unchanged.  With a tag set (disk->tag_set
// set before disk_add, or blk_mq_alloc_disk), dispatch goes through
// blk_mq_ops->queue_rq per hardware context.
//
// Requests: struct blk_request (blkdev.h) is used unchanged as the unit of
// I/O -- submitter-allocated as before for disk I/O, or allocated from a
// queue with blk_mq_alloc_request() for driver-internal pass-through
// commands (the NVMe admin queue has no gendisk at all).  The per-command
// driver data lives with the TAG, not with the request: while a request
// holds tag t of hctx h, blk_mq_rq_to_pdu(rq) is the cmd_size bytes the
// tag set keeps for (h, t), and blk_mq_rq_from_pdu(pdu) leads back.
//
// Ownership: this header and the multi-queue parts of blkdev.h belong to
// P2 after Phase 0; other agents only call what is declared here and
// request changes through the coordinator.
//
// ---- Context rules --------------------------------------------------------
//   "any context"  interrupts may be disabled, from an interrupt handler or
//                  hrtimer callback, driver spinlocks held.
//   "may sleep"    process context, interrupts enabled, no spinlock held.
//
// ---- Locking --------------------------------------------------------------
// Every lock here is a LEAF below the driver's spinlocks in the NVMe order
// (kernel/dev/nvme/nvme.h): hctx->lock (dispatch list), tags' internal
// locks, the queue's timer and requeue locks.  The block layer never holds
// one of them while calling ops->queue_rq/commit_rqs/queue_rqs/poll/
// timeout/complete; a driver may call blk_mq_* functions with its own
// spinlocks held only where a function says "any context".
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_BLK_MQ_H
#define LIKEOS_BLK_MQ_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/cpumask.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/completion.h>
#include <kernel/uapi/blkzoned.h>

struct blk_request;
struct blk_queue;
struct gendisk;
struct blk_ops;
struct blk_limits;
struct blk_work;
struct pci_device;
struct blk_mq_tag_set;
struct blk_mq_hw_ctx;
struct blk_mq_ctx;

/* ==========================================================================
 * Status codes
 * ========================================================================== */

/* What a driver reports for a request (queue_rq's answer, end_request's
 * argument).  The block layer turns it into the request's -errno with
 * blk_status_to_errno() (the end_io/blk_wait world of blkdev.h). */
typedef uint8_t blk_status_t;

#define BLK_STS_OK ((blk_status_t)0)
#define BLK_STS_NOTSUPP ((blk_status_t)1) /* -EOPNOTSUPP */
#define BLK_STS_TIMEOUT ((blk_status_t)2) /* -ETIMEDOUT */
#define BLK_STS_NOSPC ((blk_status_t)3) /* -ENOSPC */
#define BLK_STS_TRANSPORT ((blk_status_t)4) /* -ENOLINK: path failed */
#define BLK_STS_TARGET ((blk_status_t)5) /* -EREMOTEIO: target error */
#define BLK_STS_RESV_CONFLICT ((blk_status_t)6) /* -EBADE */
#define BLK_STS_MEDIUM ((blk_status_t)7) /* -ENODATA: medium error */
#define BLK_STS_PROTECTION ((blk_status_t)8) /* -EILSEQ: PI check */
#define BLK_STS_RESOURCE ((blk_status_t)9) /* -ENOMEM: retry later */
#define BLK_STS_IOERR ((blk_status_t)10) /* -EIO */
#define BLK_STS_DM_REQUEUE ((blk_status_t)11) /* -EREMCHG (unused) */
#define BLK_STS_AGAIN ((blk_status_t)12) /* -EAGAIN: NOWAIT */
/* Out of a resource that a completion of THIS device frees: the queue
 * is rerun by the next completion, not by a timer. */
#define BLK_STS_DEV_RESOURCE ((blk_status_t)13) /* -EBUSY */
#define BLK_STS_ZONE_OPEN_RESOURCE ((blk_status_t)14) /* -ETOOMANYREFS */
#define BLK_STS_ZONE_ACTIVE_RESOURCE ((blk_status_t)15) /* -EOVERFLOW */
#define BLK_STS_OFFLINE ((blk_status_t)16) /* -ENODEV */
#define BLK_STS_DURATION_LIMIT ((blk_status_t)17) /* -ETIME */
#define BLK_STS_INVAL ((blk_status_t)19) /* -EINVAL */
#define BLK_STS_NR 20

/* Status <-> errno (any context; unknown values map to -EIO / IOERR). */
int blk_status_to_errno(blk_status_t status);
blk_status_t errno_to_blk_status(int err);
/* "I/O", "timeout", "critical medium" ... for logs. */
const char *blk_status_to_str(blk_status_t status);
/* Is `status' one a multipath layer may retry on another path? */
static inline int blk_path_error(blk_status_t status)
{
	switch (status) {
	case BLK_STS_NOTSUPP:
	case BLK_STS_NOSPC:
	case BLK_STS_TARGET:
	case BLK_STS_RESV_CONFLICT:
	case BLK_STS_MEDIUM:
	case BLK_STS_PROTECTION:
		return 0;
	default:
		return 1;
	}
}

/* ==========================================================================
 * Hardware context types and queue maps
 * ========================================================================== */

enum hctx_type {
	HCTX_TYPE_DEFAULT = 0, /* everything not below */
	HCTX_TYPE_READ = 1, /* reads (when the driver has read queues) */
	HCTX_TYPE_POLL = 2, /* polled I/O (BLK_REQ_POLLED), no interrupt */
	HCTX_MAX_TYPES
};

/* CPU -> hardware queue of one type.  mq_map[cpu] is the hctx index (an
 * index into the tag set's hctx space, queue_offset ..
 * queue_offset+nr_queues-1).  nr_queues 0: the type is unused and its
 * requests go to the default map. */
struct blk_mq_queue_map {
	unsigned int *mq_map; /* MAX_CPUS entries, the tag set's */
	unsigned int nr_queues;
	unsigned int queue_offset;
};

/* Spread the CPUs of `qmap' evenly over its queues (group_cpus_evenly,
 * no device affinity).  May sleep. */
void blk_mq_map_queues(struct blk_mq_queue_map *qmap);
/* Map from the interrupt affinity of `dev': hctx i of the map is served
 * by interrupt vector `offset' + i, and every CPU in that vector's
 * affinity mask (pci_irq_get_affinity) submits to hctx i.  CPUs no
 * vector covers fall back to blk_mq_map_queues.  May sleep. */
void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct pci_device *dev, unsigned int offset);
/* Number of hardware queues worth creating: one per possible CPU, capped
 * by `max_queues' (0: no cap) -- for drivers sizing their queue count. */
unsigned int blk_mq_num_possible_queues(unsigned int max_queues);
unsigned int blk_mq_num_online_queues(unsigned int max_queues);

/* ==========================================================================
 * Tags (sbitmap semantics)
 * ========================================================================== */

/* A scalable bitmap of `depth' tags split into words so concurrent
 * allocators on different CPUs touch different cache lines; each CPU
 * remembers where it last allocated (alloc_hint).  Waiters for a free tag
 * sleep on one of several wait queues, woken in batches (wake_batch
 * frees per wake-up).  blk_mq_tag.c owns the fields. */
struct blk_sbitmap_word {
	volatile uint64_t word; /* bit set = tag in use */
	volatile uint64_t cleared; /* deferred frees, folded in on demand */
} __attribute__((aligned(64)));

#define BLK_SBQ_WAIT_QUEUES 8

struct blk_sbitmap_queue {
	unsigned int depth;
	unsigned int shift; /* log2 bits per word */
	unsigned int map_nr; /* words */
	int round_robin; /* BLK_MQ_F_TAG_RR */
	struct blk_sbitmap_word *map;
	unsigned int alloc_hint[MAX_CPUS];
	unsigned int wake_batch;
	volatile unsigned int wake_index;
	struct wait_queue_head ws[BLK_SBQ_WAIT_QUEUES];
	volatile int ws_active;
	unsigned int min_shallow_depth;
	/* --- blk_mq_tag.c private ----------------------------------------- */
	volatile uint32_t completion_cnt; /* frees since the last wake batch */
	volatile uint32_t wakeup_cnt;
	void *map_raw; /* allocation behind `map' (64-byte aligned) */
};

/* The tags of one hardware queue (or of the whole set when
 * BLK_MQ_F_TAG_HCTX_SHARED).  Tags 0 .. nr_reserved_tags-1 are the
 * reserved pool (BLK_MQ_REQ_RESERVED), the rest normal. */
struct blk_mq_tags {
	unsigned int nr_tags; /* reserved + normal */
	unsigned int nr_reserved_tags;
	volatile unsigned int active_queues; /* queues sharing these tags */
	struct blk_sbitmap_queue bitmap_tags; /* normal */
	struct blk_sbitmap_queue breserved_tags; /* reserved */
	/* tag -> request at the driver (NULL when free).  Read by the
	 * timeout scan and blk_mq_tag_to_rq. */
	struct blk_request *volatile *rqs;
	/* tag -> the driver's per-command data (cmd_size bytes each,
	 * cache-line aligned), allocated once with the tags. */
	void **pdu;
	spinlock_t lock; /* rqs[] replacement against the iterators */

	/* --- blk_mq_tag.c private ----------------------------------------- */
	struct blk_mq_tag_set *set;
	unsigned int hctx_idx; /* the hctx these tags were made for */
	unsigned int pdu_stride; /* bytes per tag: header + cmd_size */
	void *pdu_mem; /* nr_tags * pdu_stride, 64-byte aligned */
	void *pdu_raw;
	/* Hardware contexts waiting for a tag to dispatch with (a shared tag
	 * map: the restart of a run that found no tag), under `lock'. */
	struct blk_mq_hw_ctx *restart_head;
	volatile uint32_t nr_restart;
	/* Wakers running hctxs they took off restart_head (outside `lock');
	 * a context going away waits for 0 (blk_mq_tag_wait_cancel_sync). */
	volatile uint32_t restart_running;
	refcount_t ref; /* hctxs using these tags (shared tags) */
};

/* The hidden header in front of every PDU: blk_mq_rq_from_pdu() reads
 * the request holding the tag from it. */
struct blk_mq_pdu_hdr {
	struct blk_request *volatile rq;
	uint32_t tag;
	uint32_t magic;
	struct blk_mq_tags *tags;
	uint64_t pad[5];
};
#define BLK_MQ_PDU_MAGIC 0x55445042u /* "BPDU" */

/* The in-flight request holding `tag' (NULL when none).  Any context. */
struct blk_request *blk_mq_tag_to_rq(struct blk_mq_tags *tags,
				     unsigned int tag);

/* Unique tag: hardware queue in the upper 16 bits, tag in the lower. */
#define BLK_MQ_UNIQUE_TAG_BITS 16
#define BLK_MQ_UNIQUE_TAG_MASK ((1u << BLK_MQ_UNIQUE_TAG_BITS) - 1)
uint32_t blk_mq_unique_tag(struct blk_request *rq);
static inline uint16_t blk_mq_unique_tag_to_hwq(uint32_t unique_tag)
{
	return (uint16_t)(unique_tag >> BLK_MQ_UNIQUE_TAG_BITS);
}
static inline uint16_t blk_mq_unique_tag_to_tag(uint32_t unique_tag)
{
	return (uint16_t)(unique_tag & BLK_MQ_UNIQUE_TAG_MASK);
}

/* ==========================================================================
 * The driver's operations
 * ========================================================================== */

/* What queue_rq is handed: the request, and whether it is the last one of
 * this batch (when false, more follow and the driver may defer ringing the
 * doorbell to commit_rqs or to the call with last == 1). */
struct blk_mq_queue_data {
	struct blk_request *rq;
	int last;
};

/* A completion batch (polling, and interrupt handlers that complete
 * several requests at once): requests collected through rq->rq_next and
 * finished together by `complete' (the driver's batch end, e.g.
 * nvme_pci_complete_batch) after the reaping loop released its locks. */
struct io_comp_batch {
	struct blk_request *req_list; /* through rq_next */
	int need_ts;
	void (*complete)(struct io_comp_batch *iob);
};
#define DEFINE_IO_COMP_BATCH(name) struct io_comp_batch name = { 0 }

/* Request lists (queue_rqs, batches): singly linked through rq->rq_next
 * with a tail pointer. */
struct rq_list {
	struct blk_request *head;
	struct blk_request *tail;
};

/* What a timeout handler wants done with a request that timed out (both
 * the single-queue blk_ops->timeout and blk_mq_ops->timeout). */
enum blk_eh_timer_return {
	BLK_EH_DONE = 0, /* the driver's error handling completes it */
	BLK_EH_RESET_TIMER = 1, /* not yet: another timeout period */
};

struct blk_mq_ops {
	/* Hand one request to the hardware.  Called with the request
	 * started (blk_mq_start_request is the DRIVER's call, done here
	 * once the command is built) from a context that may not sleep
	 * unless the tag set has BLK_MQ_F_BLOCKING; several CPUs may be in
	 * queue_rq of different hctxs, never two in the same hctx at once
	 * unless the driver serialises itself (NVMe: sq_lock).  Returns:
	 *   BLK_STS_OK               accepted; completion comes later
	 *   BLK_STS_RESOURCE         try again later (timer-driven rerun)
	 *   BLK_STS_DEV_RESOURCE     try again when one of this device's
	 *                            requests completes
	 *   anything else            the request fails with that status
	 * Required. */
	blk_status_t (*queue_rq)(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd);
	/* The batch ended without a queue_rq with last == 1 (the last one
	 * was refused): ring the doorbell for what was queued.  Optional. */
	void (*commit_rqs)(struct blk_mq_hw_ctx *hctx);
	/* Submit a whole list (a flushed plug) at once: the driver takes
	 * what it can from `rqlist' and leaves the rest on it for the block
	 * layer to dispatch one by one.  Every request on the list belongs
	 * to the same queue; they may belong to different hctxs.
	 * Optional. */
	void (*queue_rqs)(struct rq_list *rqlist);
	/* A started request passed its deadline (timeout work, process
	 * context, no block-layer lock held).  The block layer holds a
	 * reference: `rq' stays valid for the call, but it may complete
	 * concurrently (blk_mq_request_completed tells).  Returns
	 * BLK_EH_DONE (the driver completes it / it already completed) or
	 * BLK_EH_RESET_TIMER (another timeout period).  Optional: without
	 * it the timer is reset. */
	enum blk_eh_timer_return (*timeout)(struct blk_request *rq);
	/* Reap completions of a poll hctx: completed requests go on `iob'
	 * when given (the caller completes the batch), else are completed
	 * directly.  Returns the number found.  Any context the caller
	 * polls from.  Required for HCTX_TYPE_POLL maps. */
	int (*poll)(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob);
	/* The driver's half of a completion (blk_mq_complete_request ->
	 * block softirq of the submitting CPU, or inline from the batch).
	 * Ends the request with blk_mq_end_request() or requeues it.
	 * Required. */
	void (*complete)(struct blk_request *rq);
	/* hctx creation / teardown: bind the driver's queue to it
	 * (hctx->driver_data).  May sleep.  Optional. */
	int (*init_hctx)(struct blk_mq_hw_ctx *hctx, void *set_driver_data,
			 unsigned int hctx_idx);
	void (*exit_hctx)(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx);
	/* Per-tag driver data set up when the tags are allocated (`pdu'
	 * cmd_size bytes, zeroed) / torn down.  May sleep.  Optional. */
	int (*init_request)(struct blk_mq_tag_set *set, void *pdu,
			    unsigned int hctx_idx, unsigned int tag);
	void (*exit_request)(struct blk_mq_tag_set *set, void *pdu,
			     unsigned int hctx_idx, unsigned int tag);
	/* A request ended by the block layer while the driver had state
	 * attached (queue dying).  Optional. */
	void (*cleanup_rq)(struct blk_request *rq);
	/* Is the device busy (dispatch should back off)?  Optional. */
	int (*busy)(struct blk_queue *q);
	/* Fill set->map[] (nr_maps maps).  Optional: without it every CPU
	 * maps to queue 0 of the default type. */
	void (*map_queues)(struct blk_mq_tag_set *set);
	/* Ask the transport to abort one request the driver started
	 * (blk_mq_cancel_request on an in-flight request; NVMe: the Cancel
	 * command).  The block layer holds a reference on `rq' for the
	 * call; the request still ends through its normal completion (with
	 * whatever status the device gives it), never from here.  Returns 0
	 * when an abort was issued, -EBUSY / -EOPNOTSUPP / -errno when not.
	 * Process context, may sleep.  Optional: without it a started
	 * request cannot be cancelled (-EBUSY). */
	int (*cancel)(struct blk_request *rq);
};

/* ==========================================================================
 * Tag sets
 * ========================================================================== */

/* struct blk_mq_tag_set.flags */
#define BLK_MQ_F_TAG_QUEUE_SHARED (1u << 1) /* several queues share tags */
#define BLK_MQ_F_STACKING (1u << 2) /* stacked (multipath) device */
#define BLK_MQ_F_TAG_HCTX_SHARED (1u << 3) /* one tags for all hctxs */
#define BLK_MQ_F_BLOCKING (1u << 4) /* queue_rq may sleep */
#define BLK_MQ_F_TAG_RR (1u << 5) /* round-robin tag allocation */
#define BLK_MQ_F_NO_SCHED_BY_DEFAULT (1u << 6) /* "none" elevator */
#define BLK_MQ_F_MAX (1u << 7)

#define BLK_MQ_MAX_DEPTH 10240 /* tags per hardware queue */
#define BLK_MQ_NO_HCTX_IDX (~0u)

struct blk_mq_tag_set {
	const struct blk_mq_ops *ops;
	struct blk_mq_queue_map map[HCTX_MAX_TYPES];
	unsigned int nr_maps; /* 1 .. HCTX_MAX_TYPES */
	unsigned int nr_hw_queues; /* hctxs per queue */
	unsigned int queue_depth; /* tags per hctx, reserved included */
	unsigned int reserved_tags;
	unsigned int cmd_size; /* per-tag driver data (PDU) bytes */
	int numa_node; /* -1 */
	unsigned int timeout_ms; /* request timeout, 0: 30 s */
	unsigned int flags; /* BLK_MQ_F_* */
	void *driver_data; /* passed to init_hctx */

	/* --- blk_mq*.c private -------------------------------------------- */
	struct blk_mq_tags **tags; /* [nr_hw_queues] (or one shared) */
	struct blk_mq_tags *shared_tags; /* BLK_MQ_F_TAG_HCTX_SHARED */
	struct mutex tag_list_lock; /* tag_list, nr_hw_queues updates */
	struct blk_queue *tag_list; /* queues using the set (q->set_next) */
	unsigned int nr_queues; /* on tag_list */
	unsigned int alloc_flags;
};

/* Allocate the tags, PDUs and maps of `set' (the driver filled ops,
 * nr_hw_queues, queue_depth, reserved_tags, cmd_size, nr_maps, flags,
 * driver_data, timeout_ms); calls ops->map_queues.  0, -EINVAL (depth 0
 * or > BLK_MQ_MAX_DEPTH, nr_hw_queues 0), -ENOMEM (it retries with half
 * the depth down to reserved_tags + 1 first, as the established
 * allocator does, and reports the depth it got in queue_depth).  May
 * sleep. */
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set);
/* Same for a set filled by the helper below. */
int blk_mq_alloc_sq_tag_set(struct blk_mq_tag_set *set,
			    const struct blk_mq_ops *ops,
			    unsigned int queue_depth, unsigned int set_flags);
/* Free tags and maps; no queue may use the set any more.  May sleep. */
void blk_mq_free_tag_set(struct blk_mq_tag_set *set);
/* Change the number of hardware queues of every queue of the set (after
 * a controller reset obtained a different count): freezes them, remaps,
 * reallocates hctxs and tags, unfreezes.  Contexts of indexes that go
 * are exited once (ops->exit_hctx) and kept, stopped, until the queue's
 * memory goes; the tags of those indexes are freed.  When a queue cannot
 * get memory for more contexts the whole set falls back to the previous
 * count.  May sleep. */
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set,
				int nr_hw_queues);
/* Call fn(rq, data) for every STARTED request of every hctx of the set
 * (fn returns 0 to stop).  The request is held for the call.  May sleep
 * only if fn may. */
typedef int (*busy_tag_iter_fn)(struct blk_request *rq, void *data);
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *set, busy_tag_iter_fn fn,
			     void *data);
/* Wait until every request the driver completed with
 * blk_mq_complete_request has run its completion (after a controller
 * shutdown cancelled them).  A request counts until its driver ended it
 * (blk_mq_end_request makes it IDLE at once, even while a timeout scan
 * or an iterator still holds a reference on it).  May sleep. */
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *set);
/* Quiesce / unquiesce every queue of the set (recovery). */
void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set);
void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set);

/* ==========================================================================
 * Hardware contexts
 * ========================================================================== */

/* struct blk_mq_hw_ctx.state bits (atomic) */
#define BLK_MQ_S_STOPPED 0 /* blk_mq_stop_hw_queue */
#define BLK_MQ_S_TAG_ACTIVE 1 /* counted in tags->active_queues */
#define BLK_MQ_S_SCHED_RESTART 2 /* rerun when a tag frees */
#define BLK_MQ_S_INACTIVE 3 /* all its CPUs went offline */
#define BLK_MQ_S_RETIRED 4 /* left by a shrink of nr_hw_queues (exited) */

struct blk_mq_hw_ctx {
	/* --- what drivers use ---------------------------------------------- */
	void *driver_data; /* init_hctx sets it (NVMe: struct nvme_queue) */
	struct blk_queue *queue;
	unsigned int queue_num; /* index within the queue's hctxs */
	enum hctx_type type;
	struct blk_mq_tags *tags;
	unsigned int flags; /* the set's BLK_MQ_F_* */
	cpumask_t cpumask; /* CPUs that submit here */
	int numa_node;

	/* --- blk_mq*.c private -------------------------------------------- */
	spinlock_t lock; /* dispatch list */
	struct blk_request *dispatch_head, *dispatch_tail;
	volatile unsigned long state; /* BLK_MQ_S_* */
	volatile unsigned int nr_active; /* shared tags: requests in flight */
	unsigned int next_cpu; /* round-robin run CPU */
	int next_cpu_batch;
	volatile unsigned int dispatch_busy; /* EWMA of busy dispatches */
	struct blk_work *run_work; /* blkdev.h's blk_work, delayed runs */
	uint64_t queued, run, dispatched[8]; /* statistics, sysfs mq/ */
	/* Hybrid polling: mean completion time per direction and size
	 * bucket, sampled from the completions of this hctx. */
	uint64_t poll_stat_mean_ns[2];
	uint64_t poll_stat_nr[2];
	uint64_t poll_considered, poll_invoked, poll_success;
	char sysfs_base[128]; /* "block/nvme0n1/mq/3" */

	/* --- blk_mq*.c private -------------------------------------------- */
	struct blk_mq_ctx **ctxs; /* the software queues mapped here */
	unsigned int nr_ctx;
	unsigned int dispatch_from; /* round-robin ctx of a busy dispatch */
	/* Bit i: ctxs[i] has requests of this hctx's type queued. */
	volatile uint64_t ctx_map[(MAX_CPUS + 63) / 64];
	volatile uint32_t nr_dispatch; /* requests on the dispatch list */
	/* Contexts inside the dispatch (queue_rq/commit_rqs); quiesce waits
	 * for 0. */
	volatile int dispatching;
	/* Waiting for a tag of a shared map (tags->restart_head). */
	struct blk_mq_hw_ctx *restart_next;
	volatile int restart_queued;
	volatile uint32_t run_cpu_batch;
	uint64_t nr_queue_rq, nr_commit, nr_busy; /* statistics */
	uint64_t requeued, timeouts_handled;
	/* A context made unused by blk_mq_update_nr_hw_queues: kept (stopped)
	 * until the queue's memory goes, as a run that raced with the
	 * update may still look at it. */
	struct blk_mq_hw_ctx *dead_next;
};

/* A software queue: one per CPU and queue.  A request submitted on CPU c
 * without a scheduler waits on ctx[c]'s list of its hctx type until the
 * hctx it maps to runs. */
struct blk_mq_ctx {
	spinlock_t lock;
	struct blk_request *rq_head[HCTX_MAX_TYPES]; /* through disp_next */
	struct blk_request *rq_tail[HCTX_MAX_TYPES];
	unsigned int cpu;
	unsigned short index_hw[HCTX_MAX_TYPES]; /* index in hctx->ctxs */
	struct blk_mq_hw_ctx *hctxs[HCTX_MAX_TYPES];
	struct blk_queue *queue;
	uint64_t rq_dispatched[2], rq_merged, rq_completed[2];
} __attribute__((aligned(64)));

/* The hctx a request runs on / of a CPU for a type. */
struct blk_mq_hw_ctx *blk_mq_map_queue(struct blk_queue *q,
				       uint32_t rq_flags, unsigned int cpu);
/* Stop / start dispatch of one hctx or all (start runs them).  Any
 * context. */
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_stop_hw_queues(struct blk_queue *q);
void blk_mq_start_hw_queues(struct blk_queue *q);
void blk_mq_start_stopped_hw_queues(struct blk_queue *q, int async);
/* Run (dispatch pending requests).  async: through kblockd.  Any
 * context (sync runs from atomic context are made async). */
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, int async);
void blk_mq_run_hw_queues(struct blk_queue *q, int async);
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx,
			       unsigned long msecs);
int blk_mq_hctx_stopped(const struct blk_mq_hw_ctx *hctx);

/* ==========================================================================
 * Queues
 * ========================================================================== */

/* A queue with no disk (a controller's admin queue, a multipath node's
 * internal queue): hctxs from `set', limits `lim' (NULL: defaults),
 * queuedata for the driver.  Returns the queue or NULL (-ENOMEM).  May
 * sleep. */
struct blk_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set,
				     const struct blk_limits *lim,
				     void *queuedata);
/* Mark dying, fail what is queued, wait for in-flight requests, free the
 * hctxs; the memory goes with the last blk_put_queue.  May sleep. */
void blk_mq_destroy_queue(struct blk_queue *q);
void blk_get_queue(struct blk_queue *q);
void blk_put_queue(struct blk_queue *q);
/* A gendisk (kind BLK_DISK_NVME, name given) with a multi-queue queue on
 * `set'; the driver fills identity and capacity and calls disk_add().
 * NULL on -ENOMEM / name in use.  May sleep. */
struct gendisk *blk_mq_alloc_disk(struct blk_mq_tag_set *set,
				  const struct blk_limits *lim,
				  const struct blk_ops *ops, const char *name,
				  void *queuedata);
/* The driver's queuedata (blk_mq_alloc_queue/_disk). */
void *blk_queue_get_queuedata(const struct blk_queue *q);
int blk_queue_dying(const struct blk_queue *q);
/* Mark the disk dead (surprise removal): every new request fails with
 * -ENODEV at once, waiters are woken.  What is already queued is still
 * dispatched to the driver (which fails it) once the queue runs.  Any
 * context. */
void blk_mark_disk_dead(struct gendisk *disk);

/* ---- freeze: no new requests, wait for the ones in flight ---------------- */
/* Freezes nest (a counter); a request that enters a frozen queue waits
 * (or fails with -EAGAIN for BLK_MQ_REQ_NOWAIT).  start: any context;
 * wait/freeze: may sleep. */
void blk_freeze_queue_start(struct blk_queue *q);
void blk_mq_freeze_queue_wait(struct blk_queue *q);
/* 1 when drained in time, 0 on timeout. */
int blk_mq_freeze_queue_wait_timeout(struct blk_queue *q,
				     unsigned long timeout_ms);
void blk_mq_freeze_queue(struct blk_queue *q);
void blk_mq_unfreeze_queue(struct blk_queue *q);
/* The "non_owner" forms of the established interface: freeze started by
 * one context (a reset work) and ended by another.  Same here. */
#define blk_freeze_queue_start_non_owner(q) blk_freeze_queue_start(q)
#define blk_mq_unfreeze_queue_non_owner(q) blk_mq_unfreeze_queue(q)
int blk_queue_frozen(const struct blk_queue *q);
/* Enter / leave the queue for a submission (internal to the block layer;
 * exported for pass-through paths): 0 or -ENODEV (dying) / -EAGAIN
 * (frozen with nowait).  May sleep unless nowait. */
int blk_mq_queue_enter(struct blk_queue *q, int nowait);
void blk_mq_queue_exit(struct blk_queue *q);

/* ---- quiesce: no dispatch into the driver -------------------------------- */
/* After blk_mq_quiesce_queue() returns no queue_rq of the queue runs or
 * will run until unquiesce (nests).  _nowait: only sets the state;
 * blk_mq_wait_quiesce_done(set) then waits for running dispatches of
 * every queue of the set.  quiesce: may sleep; nowait/unquiesce: any
 * context. */
void blk_mq_quiesce_queue(struct blk_queue *q);
void blk_mq_quiesce_queue_nowait(struct blk_queue *q);
void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set);
void blk_mq_unquiesce_queue(struct blk_queue *q);
int blk_queue_quiesced(const struct blk_queue *q);

/* ---- requeue ---------------------------------------------------------------- */
/* Give a started request back: it is dispatched again (at the head) when
 * the requeue list is kicked (kick_requeue_list = 1 does that now).  Any
 * context. */
void blk_mq_requeue_request(struct blk_request *rq, int kick_requeue_list);
void blk_mq_kick_requeue_list(struct blk_queue *q);
void blk_mq_delay_kick_requeue_list(struct blk_queue *q, unsigned long msecs);

/* ==========================================================================
 * Requests on a multi-queue queue
 * ========================================================================== */

/* blk_mq_alloc_request flags */
#define BLK_MQ_REQ_NOWAIT (1u << 0) /* -EAGAIN instead of waiting for a tag */
#define BLK_MQ_REQ_RESERVED (1u << 1) /* from the reserved tags */
#define BLK_MQ_REQ_PM (1u << 2) /* allowed on a suspended queue */

/* A request for a driver-internal command on `q' (with or without a
 * disk): heap-allocated, initialised for `op' (BLK_OP_DRV_IN/DRV_OUT or
 * any data op), with a TAG already assigned on the hctx of this CPU (the
 * reserved pool for BLK_MQ_REQ_RESERVED), so blk_mq_rq_to_pdu() works at
 * once.  Returns the request or an ERR value: NULL with *err set
 * (-EAGAIN, -ENODEV dying, -ENOMEM).  May sleep unless NOWAIT. */
struct blk_request *blk_mq_alloc_request(struct blk_queue *q, int op,
					 uint32_t flags, int *err);
/* The same on a given hctx (NVMe connect-style per-queue commands). */
struct blk_request *blk_mq_alloc_request_hctx(struct blk_queue *q, int op,
					      uint32_t flags,
					      unsigned int hctx_idx, int *err);
/* Free a request from blk_mq_alloc_request (tag and memory).  Any context
 * once it completed. */
void blk_mq_free_request(struct blk_request *rq);
/* Attach a kernel buffer as the request's data (virt, any alignment the
 * queue's dma_alignment allows; split per page by the driver).  0,
 * -EINVAL, -ENOMEM.  Process context. */
int blk_rq_map_kern(struct blk_request *rq, void *kbuf, unsigned int len);
/* Execute a request from blk_mq_alloc_request: queued (at the head when
 * at_head) and run; nowait returns at once and calls rq->end_io from the
 * completion; blk_execute_rq waits (uninterruptible) and returns the
 * block status.  May sleep (blk_execute_rq). */
void blk_execute_rq_nowait(struct blk_request *rq, int at_head);
blk_status_t blk_execute_rq(struct blk_request *rq, int at_head);
/* Polled execution (a poll-queue request): spin on blk_rq_poll until it
 * completes.  Returns the status. */
blk_status_t blk_execute_rq_polled(struct blk_request *rq, int at_head);

/* The driver's per-command data of the tag `rq' holds (valid between the
 * tag assignment and the request's end) and back. */
void *blk_mq_rq_to_pdu(struct blk_request *rq);
struct blk_request *blk_mq_rq_from_pdu(void *pdu);

/* Request state, as the driver and the timeout handler see it. */
enum mq_rq_state {
	MQ_RQ_IDLE = 0, /* not at the driver */
	MQ_RQ_IN_FLIGHT = 1, /* blk_mq_start_request done */
	MQ_RQ_COMPLETE = 2, /* blk_mq_complete_request / end done */
};
enum mq_rq_state blk_mq_rq_state(const struct blk_request *rq);
static inline int blk_mq_request_started(const struct blk_request *rq)
{
	return blk_mq_rq_state(rq) != MQ_RQ_IDLE;
}
static inline int blk_mq_request_completed(const struct blk_request *rq)
{
	return blk_mq_rq_state(rq) == MQ_RQ_COMPLETE;
}

/* The driver starts the request's timeout clock (in queue_rq, right
 * before handing it to the hardware).  Any context. */
void blk_mq_start_request(struct blk_request *rq);
/* Mark complete without the completion path (the driver completes it
 * itself later, e.g. a request cancelled during reset).  Any context. */
void blk_mq_set_request_complete(struct blk_request *rq);
/* The hardware finished `rq': hand it to ops->complete (block softirq of
 * the submitting CPU, or here when it shares the cache).  Returns 1 when
 * the completion was routed elsewhere (remote), 0 when ops->complete ran
 * or will run on this CPU.  A request already completed (a timeout or
 * cancel won the race) is ignored and returns 0 -- the completion must
 * only happen once: blk_mq_complete_request_remote() is the variant that
 * returns 0 without completing locally so the caller can batch it.  Any
 * context. */
int blk_mq_complete_request(struct blk_request *rq);
/* _remote: 1 when routed to a softirq (or when the request had been
 * completed already -- nothing for the caller to do), 0 when the caller
 * must complete it itself (ops->complete or a batch). */
int blk_mq_complete_request_remote(struct blk_request *rq);
/* Add `rq' to a polling batch instead of completing it (returns 0 when it
 * cannot be batched -- the caller then completes it normally). */
int blk_mq_add_to_batch(struct blk_request *rq, struct io_comp_batch *iob,
			int is_error,
			void (*complete)(struct io_comp_batch *iob));
/* End: account, end_io, wake the waiter, release the tag (the request's
 * memory is the submitter's / blk_mq_free_request's).  The driver's
 * ->complete calls this.  No driver spinlock held. */
void blk_mq_end_request(struct blk_request *rq, blk_status_t status);
/* The batch end: blk_mq_end_request for every request on iob->req_list
 * with BLK_STS_OK. */
void blk_mq_end_request_batch(struct io_comp_batch *iob);

/* ---- polling ----------------------------------------------------------- */
/* Poll the hctx of a polled request until it completed or nothing more
 * is found (flags BLK_POLL_ONESHOT: one pass).  Hybrid polling (queue
 * attribute io_poll_delay > 0, or -1... see blk_sysfs) sleeps about
 * half the mean completion time before the first pass.  Returns
 * completions found.  Any context that may spin. */
#define BLK_POLL_ONESHOT (1u << 0)
int blk_rq_poll(struct blk_request *rq, struct io_comp_batch *iob,
		unsigned int poll_flags);
int blk_mq_poll(struct blk_queue *q, uint32_t cookie,
		struct io_comp_batch *iob, unsigned int poll_flags);
int blk_rq_is_poll(const struct blk_request *rq);

/* ---- cancellation ------------------------------------------------------ */
/* Cancel a request.  One that has not reached the driver yet (on a
 * software queue, a dispatch list, the requeue list, the elevator or a
 * zone write plug) is taken off and ends with -ECANCELED: 0.  One the
 * driver started is handed to ops->cancel (the transport aborts it; it
 * ends through its normal completion): 0 when the abort was issued, the
 * driver's error otherwise, -EBUSY without ops->cancel.  -EBUSY also
 * when it is being dispatched or ended right now, or was merged into
 * another request; -EINVAL when it is not a multi-queue request.  A
 * request ends exactly once whatever races with the call.  The caller
 * owns `rq' (its submitter): it stays valid for the call.  Process
 * context (ops->cancel may sleep); without ops->cancel any context that
 * may take the queue locks. */
int blk_mq_cancel_request(struct blk_request *rq);

/* Stop the queue's timeout machinery and wait for it (the timer and the
 * timeout work): no timeout handler of `q' runs on return, and none
 * starts until a request arms the timer again.  Requeue and run work are
 * left alone.  May sleep; never from the queue's own timeout handler. */
void blk_sync_queue(struct blk_queue *q);

/* Requests of `q' started and not yet ended (the established
 * blk_mq_queue_inflight).  May sleep only if nothing. */
unsigned int blk_mq_queue_inflight(struct blk_queue *q);
/* Wait (bounded) until every hctx of `q' has left its dispatch.  May
 * sleep. */
void blk_mq_wait_quiesce_done_queue(struct blk_queue *q);

/* ---- request list helpers ---------------------------------------------- */
static inline void rq_list_init(struct rq_list *l)
{
	l->head = NULL;
	l->tail = NULL;
}
static inline int rq_list_empty(const struct rq_list *l)
{
	return l->head == NULL;
}

/* ==========================================================================
 * Integrity (protection information)
 * ========================================================================== */

/* struct blk_integrity.flags */
#define BLK_INTEGRITY_NOVERIFY (1u << 0) /* do not verify on read */
#define BLK_INTEGRITY_NOGENERATE (1u << 1) /* do not generate on write */
#define BLK_INTEGRITY_DEVICE_CAPABLE (1u << 2) /* device checks PI */
#define BLK_INTEGRITY_REF_TAG (1u << 3) /* ref tag = LBA (type 1/2) */
#define BLK_INTEGRITY_STACKED (1u << 4)

/* Guard/checksum of the protection information. */
enum blk_integrity_checksum {
	BLK_INTEGRITY_CSUM_NONE = 0, /* metadata without PI */
	BLK_INTEGRITY_CSUM_IP = 1, /* IP checksum (not used by NVMe) */
	BLK_INTEGRITY_CSUM_CRC = 2, /* T10 CRC16 (16-bit guard) */
	BLK_INTEGRITY_CSUM_CRC64 = 3, /* NVMe CRC64 (64-bit guard) */
};

/* A queue's integrity profile (part of struct blk_limits). */
struct blk_integrity {
	uint8_t flags; /* BLK_INTEGRITY_* */
	uint8_t csum_type; /* enum blk_integrity_checksum */
	uint8_t tuple_size; /* metadata bytes per interval */
	uint8_t pi_offset; /* PI tuple offset inside the metadata */
	uint8_t interval_exp; /* log2 of the protection interval */
	uint8_t tag_size; /* application tag bytes */
	uint8_t pi_tuple_size; /* 8 (16-bit guard) or 16 (64-bit guard) */
	uint8_t pad;
	uint32_t metadata_size; /* metadata bytes per logical block */
};

/* The integrity payload of a request: the metadata buffer that goes with
 * the data (kernel virtual, physically contiguous when phys != 0). */
#define BIP_BLOCK_INTEGRITY (1u << 0) /* block layer owns the buffer */
#define BIP_MAPPED_INTEGRITY (1u << 1) /* ref tags remapped */
#define BIP_CTRL_NOCHECK (1u << 2) /* disable controller checks */
#define BIP_DISK_NOCHECK (1u << 3) /* disable disk checks */
#define BIP_IP_CHECKSUM (1u << 4)
#define BIP_INTEGRITY_USER (1u << 5) /* user (pass-through) metadata */
#define BIP_CHECK_GUARD (1u << 6)
#define BIP_CHECK_REFTAG (1u << 7)
#define BIP_CHECK_APPTAG (1u << 8)

struct blk_integrity_payload {
	void *buf; /* metadata buffer */
	uint64_t phys; /* 0: translate buf */
	uint32_t len; /* bytes */
	uint32_t flags; /* BIP_* */
	uint64_t seed; /* reference tag seed (first LBA's interval) */
	uint16_t app_tag;
	uint16_t app_tag_mask;
};

/* Does `rq' carry protection information / metadata? */
int blk_integrity_rq(const struct blk_request *rq);
/* The profile of a queue, NULL when it has none (metadata_size 0). */
const struct blk_integrity *blk_get_integrity(const struct gendisk *disk);
/* Generate PI for a write / verify it after a read, for the data and
 * metadata of `rq' (T10 type 1/2/3 with CRC16, NVMe CRC64): 0 or
 * BLK_STS_PROTECTION.  Pure on the buffers.  Any context. */
blk_status_t blk_integrity_generate(struct blk_request *rq);
blk_status_t blk_integrity_verify(struct blk_request *rq);
/* Remap reference tags from the virtual seed to the physical LBA before
 * dispatch (prepare) and back after completion (complete), type 1/2. */
void blk_integrity_prepare(struct blk_request *rq);
void blk_integrity_complete(struct blk_request *rq, unsigned int nr_bytes);
/* Attach a metadata buffer (kernel virtual) to `rq'.  0 / -errno. */
int blk_rq_integrity_map_kern(struct blk_request *rq, void *buf,
			      unsigned int len, uint32_t bip_flags);
/* CRC16 T10-DIF (poly 0x8BB7) and CRC64 NVMe (Rocksoft poly
 * 0xAD93D23594C93659, reflected) over a buffer, continuing `crc'. */
uint16_t crc_t10dif_update(uint16_t crc, const void *buf, size_t len);
uint64_t crc64_nvme(uint64_t crc, const void *buf, size_t len);

/* ==========================================================================
 * Zoned block devices
 * ========================================================================== */

/* report_zones callback: called per zone in order; return 0 to go on,
 * -errno to stop with that error. */
typedef int (*report_zones_cb)(struct blk_zone *zone, unsigned int idx,
			       void *data);
/* Report up to nr_zones zones from `sector' through cb (the disk's
 * blk_ops->report_zones).  Returns the number reported or -errno.  May
 * sleep. */
int blkdev_report_zones(struct gendisk *disk, uint64_t sector,
			unsigned int nr_zones, report_zones_cb cb, void *data);
/* A zone management op (BLK_OP_ZONE_RESET/OPEN/CLOSE/FINISH) over
 * [sector, sector+nr_sectors) (whole zones), or RESET_ALL.  May sleep. */
int blkdev_zone_mgmt(struct gendisk *disk, int op, uint64_t sector,
		     uint64_t nr_sectors);
/* After a zoned disk changed (scan, revalidate): read every zone,
 * check the layout (equal zone sizes but the last, sizes a power of
 * two), set nr_zones, the conventional zone bitmap and the write
 * pointers of the zone write plugs.  0 or -errno.  May sleep. */
int blk_revalidate_disk_zones(struct gendisk *disk);
/* Zone number of a sector / sectors per zone (0: not zoned). */
unsigned int disk_zone_no(const struct gendisk *disk, uint64_t sector);
uint64_t bdev_zone_sectors(const struct gendisk *disk);
int blk_queue_is_zoned(const struct blk_queue *q);
/* Fill a report entry from the driver's values: zone write plugging keeps
 * its own write pointer; this hands the driver's report through it. */
int disk_report_zone(struct gendisk *disk, struct blk_zone *zone,
		     unsigned int idx, void *cb_data);
/* The zoned ioctls (BLKREPORTZONE, BLKRESETZONE, BLKOPENZONE,
 * BLKCLOSEZONE, BLKFINISHZONE, BLKGETZONESZ, BLKGETNRZONES), called by
 * blkdev_ioctl.  -ENOTTY when not one of them.  May sleep. */
long blkdev_zone_ioctl(struct gendisk *disk, unsigned long cmd, void *argp,
		       int open_flags);

/* ==========================================================================
 * Atomic writes (RWF_ATOMIC) and write streams
 * ========================================================================== */

/* Is an atomic write of [lba, lba+count) on `disk' allowed: within
 * atomic_write_unit_min/max, a power of two, naturally aligned, not
 * crossing atomic_write_boundary?  0 or -EINVAL. */
int blk_validate_atomic_write(const struct gendisk *disk, uint64_t sector,
			      uint32_t bytes);

/* ==========================================================================
 * Persistent reservations (IOC_PR_*)
 * ========================================================================== */

/* What a disk offers for the IOC_PR_* ioctls (blk_ops->pr_ops).  Return
 * 0, a positive pr_status (PR_STS_*) when the device refused, or -errno.
 * May sleep. */
struct pr_keys {
	uint32_t generation;
	uint32_t num_keys; /* in: room in keys[]; out: registered */
	uint64_t keys[];
};
struct pr_held_reservation {
	uint64_t key;
	uint32_t generation;
	uint32_t type; /* enum pr_type, 0 none */
};
struct pr_ops {
	int (*pr_register)(struct gendisk *disk, uint64_t old_key,
			   uint64_t new_key, uint32_t flags);
	int (*pr_reserve)(struct gendisk *disk, uint64_t key, uint32_t type,
			  uint32_t flags);
	int (*pr_release)(struct gendisk *disk, uint64_t key, uint32_t type);
	int (*pr_preempt)(struct gendisk *disk, uint64_t old_key,
			  uint64_t new_key, uint32_t type, int abort);
	int (*pr_clear)(struct gendisk *disk, uint64_t key);
	int (*pr_read_keys)(struct gendisk *disk, struct pr_keys *keys_info);
	int (*pr_read_reservation)(struct gendisk *disk,
				   struct pr_held_reservation *rsv);
};
/* The IOC_PR_* ioctls through disk->ops->pr_ops (blkdev_ioctl calls it):
 * -ENOTTY when not one of them, -EOPNOTSUPP without pr_ops.  May sleep. */
long blkdev_pr_ioctl(struct gendisk *disk, unsigned long cmd, void *argp,
		     int open_flags);

/* ==========================================================================
 * Debug
 * ========================================================================== */

/* Name of a BLK_OP_* ("READ", "WRITE", "ZONE_APPEND" ...). */
const char *blk_op_str(int op);

#endif /* LIKEOS_BLK_MQ_H */
