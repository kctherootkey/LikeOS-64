// LikeOS - block device core: the registry, the block layer's sleeping
// mutex, requests (submission, waiting, completion), the synchronous
// helpers every filesystem ends up in, reference counting and claims of
// block devices, and whole-system shutdown / suspend / resume of disks.
//
// ---- The registry ------------------------------------------------------------
// g_block_devices[] is the table block_get() reads.  It used to be compacted
// array of at most eight devices that only ever grew; it now has stable
// slots: a device keeps its slot for as long as it is registered, a removed
// device leaves a NULL hole, and block_count() is the high-water mark of
// slots ever used.  Code that remembers "I have tried slot N" (the storage
// bootstrap's tested mask) therefore stays right when a disk is unplugged
// and another one is plugged in.  Never-used slots are handed out before
// holes are reused, so a slot number is not recycled until the table has
// been filled once.
//
// ---- Requests ------------------------------------------------------------------
// struct blk_request (blkdev.h) is the unit of I/O.  The life of one:
//
//   blk_request_init / blk_request_alloc          state IDLE, ref 1
//   blk_submit       validate, remap a partition request to its disk,
//                    stamp, take the in-flight references, flush
//                    sequencing set-up, hand to the disk's queue
//                    (blk_queue.c)                  state QUEUED
//   the queue's dispatcher calls ops->submit         state DISPATCHED
//   the driver's interrupt handler calls blk_complete_request(): the
//   request goes to the block softirq of its submitting CPU (or of this
//   one when both share the last-level cache)
//   the softirq runs the driver's ->complete, which ends the request with
//   blk_request_complete()                           state COMPLETE
//   blk_wait / end_io see the status, the submitter releases it
//
// The softirq part never dispatches: when the freed tag lets a waiting
// request go, kblockd runs the queue (blk_queue.c).  Its order is the
// submitter's result first -- end_io and the waiter's wake-up -- and the
// queue's bookkeeping after: statistics, the tag, the restart.  The
// woken waiter may free the request at once, so what the bookkeeping
// needs is copied out before (struct blk_done_snap), and the in-flight
// reference on the block device -- which keeps the disk and its queue
// alive -- is dropped last.
//
// A device registered the old way (magic != BLOCK_DEVICE_MAGIC: the USB
// mass storage driver until it moves to genhd) has no queue: blk_submit
// executes its requests synchronously through the original read/write/
// sync callbacks and completes them before returning.
//
// ---- Completion and lifetime ---------------------------------------------------
// The one subtle rule of this file: the moment a request is marked done, a
// waiter on another CPU may return and free it (it usually lives on the
// waiter's stack).  So the finishing step (blk_rq_finish) publishes `done'
// and wakes the waiter while holding the request's done_lock -- the waiter
// tests `done' under the same lock and therefore cannot return before the
// finisher has let go -- and touches nothing of the request after that.
// The in-flight reference on the block device, taken by blk_submit, is
// dropped through a pointer copied out before the request was published,
// after the queue's side of the completion is through.
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

/* ==========================================================================
 * The registry
 * ========================================================================== */

// Spinlock for block device list access (leaf lock)
static spinlock_t block_lock = SPINLOCK_INIT("block");

static block_device_t *g_block_devices[BLOCK_MAX_DEVICES];
/* Slots ever used: the bound of a block_get() loop. */
static int g_block_hwm = 0;
/* Slots in use right now (diagnostics). */
static int g_block_live = 0;

int block_register(block_device_t *dev)
{
	BUG_ON(dev == NULL);
	WARN_ON(dev->read == NULL ||
		dev->write ==
			NULL); /* block_register: device missing read/write ops */
	if (!dev) {
		return ST_INVALID;
	}

	uint64_t flags;
	int slot = -1;

	spin_lock_irqsave(&block_lock, &flags);

	/* Registering the same object twice would give it two slots and make
	 * the second unregister find a stale one; refuse it loudly. */
	for (int i = 0; i < g_block_hwm; i++) {
		if (g_block_devices[i] == dev) {
			spin_unlock_irqrestore(&block_lock, flags);
			WARN(1, "block device %s registered twice (slot %d)",
			     dev->name ? dev->name : "?", i);
			return ST_EXISTS;
		}
	}

	/* A never-used slot first, a hole only when the table is full, so a
	 * slot number is not handed to a new device while the storage
	 * bootstrap may still remember the old one. */
	if (g_block_hwm < BLOCK_MAX_DEVICES) {
		slot = g_block_hwm++;
	} else {
		for (int i = 0; i < BLOCK_MAX_DEVICES; i++) {
			if (!g_block_devices[i]) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0) {
		spin_unlock_irqrestore(&block_lock, flags);
		kprintf(BLK_LOG_PREFIX
			"registry full (%d devices): %s not registered\n",
			BLOCK_MAX_DEVICES, dev->name ? dev->name : "?");
		return ST_ERR;
	}
	g_block_devices[slot] = dev;
	g_block_live++;
	dev->registry_index = slot;
	if (dev->magic == BLOCK_DEVICE_MAGIC)
		dev->flags |= BDEV_F_REGISTERED;

	spin_unlock_irqrestore(&block_lock, flags);
	return ST_OK;
}

int block_unregister(block_device_t *dev)
{
	uint64_t flags;
	int found = -1;

	if (!dev)
		return ST_INVALID;
	spin_lock_irqsave(&block_lock, &flags);
	/* The slot the device was given is where it is; a scan is the
	 * fallback for a registrant that overwrote its registry_index. */
	if (dev->registry_index >= 0 && dev->registry_index < g_block_hwm &&
	    g_block_devices[dev->registry_index] == dev) {
		found = dev->registry_index;
	} else {
		for (int i = 0; i < g_block_hwm; i++) {
			if (g_block_devices[i] == dev) {
				found = i;
				break;
			}
		}
	}
	if (found < 0) {
		spin_unlock_irqrestore(&block_lock, flags);
		return ST_NOT_FOUND;
	}
	g_block_devices[found] = NULL;
	g_block_live--;
	WARN_ON(g_block_live < 0);
	dev->registry_index = -1;
	if (dev->magic == BLOCK_DEVICE_MAGIC)
		dev->flags &= ~BDEV_F_REGISTERED;
	spin_unlock_irqrestore(&block_lock, flags);
	return ST_OK;
}

const block_device_t *block_get(int index)
{
	BUG_ON(index < 0 || index >= BLOCK_MAX_DEVICES);
	uint64_t flags;
	spin_lock_irqsave(&block_lock, &flags);

	if (index < 0 || index >= g_block_hwm) {
		spin_unlock_irqrestore(&block_lock, flags);
		return 0;
	}
	/* May be NULL: a hole left by a removed device. */
	const block_device_t *dev = g_block_devices[index];

	spin_unlock_irqrestore(&block_lock, flags);
	return dev;
}

int block_count(void)
{
	return __atomic_load_n(&g_block_hwm, __ATOMIC_ACQUIRE);
}

int block_sync(block_device_t *dev)
{
	if (!dev) {
		return ST_INVALID;
	}
	/* An extended device flushes through its queue, in order with the
	 * writes that went before it. */
	if (dev->magic == BLOCK_DEVICE_MAGIC)
		return blk_errno_to_status(blk_flush_sync(dev));
	if (dev->sync) {
		return dev->sync(dev);
	}
	return ST_OK; // No sync function = no-op (success)
}

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

int blk_status_to_errno(int st)
{
	switch (st) {
	case ST_OK:
		return 0;
	case ST_IO:
		return -EIO;
	case ST_TIMEOUT:
		return -ETIMEDOUT;
	case ST_NO_DEVICE:
		return -ENODEV;
	case ST_INVALID:
		return -EINVAL;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_ROFS:
		return -EROFS;
	case ST_BUSY:
		return -EBUSY;
	case ST_AGAIN:
		return -EAGAIN;
	case ST_UNSUPPORTED:
		return -EOPNOTSUPP;
	case ST_NOSPC:
		return -ENOSPC;
	case ST_ACCESS:
		return -EACCES;
	case ST_PERM:
		return -EPERM;
	case ST_NOT_FOUND:
		return -ENOENT;
	default:
		return st < 0 ? -EIO : 0;
	}
}

const char *blk_op_name(enum blk_op op)
{
	switch (op) {
	case BLK_OP_READ:
		return "READ";
	case BLK_OP_WRITE:
		return "WRITE";
	case BLK_OP_FLUSH:
		return "FLUSH";
	case BLK_OP_DISCARD:
		return "DISCARD";
	case BLK_OP_SECURE_ERASE:
		return "SECURE_ERASE";
	case BLK_OP_WRITE_ZEROES:
		return "WRITE_ZEROES";
	case BLK_OP_DRV_IN:
		return "DRV_IN";
	case BLK_OP_DRV_OUT:
		return "DRV_OUT";
	default:
		return "?";
	}
}

int blk_bdev_read_only(const block_device_t *bdev)
{
	if (!bdev)
		return 1;
	if (!blk_bdev_is_extended(bdev))
		return 0;
	if (bdev->ro)
		return 1;
	if (bdev->parent && bdev->parent->ro)
		return 1;
	if (bdev->disk && (bdev->disk->flags & GENHD_FL_RO))
		return 1;
	return 0;
}

/* Total bytes described by a request's scatter/gather list. */
static uint64_t blk_rq_sg_bytes(const struct blk_request *req)
{
	uint64_t total = 0;

	if (!req->sg)
		return 0;
	for (uint32_t i = 0; i < req->nsg; i++)
		total += req->sg[i].len;
	return total;
}

/* Bytes a request moves (what resid counts down from). */
static uint64_t blk_rq_data_bytes(const struct blk_request *req)
{
	if (!blk_op_has_data(req->op))
		return 0;
	if (req->op == BLK_OP_DRV_IN || req->op == BLK_OP_DRV_OUT)
		return blk_rq_sg_bytes(req);
	if (!req->bdev)
		return 0;
	return (uint64_t)req->count * req->bdev->sector_size;
}

/* ==========================================================================
 * The block layer's sleeping mutex
 * ========================================================================== */

/* The owner value of a holder that is not a scheduler task (boot, before
 * sched_init: there is exactly one such context). */
#define BLK_MUTEX_BOOT_OWNER ((uint64_t)-2)
#define BLK_MUTEX_NO_OWNER ((uint64_t)-1)

static uint64_t blk_mutex_self(void)
{
	task_t *cur = sched_current();

	return cur ? (uint64_t)(uint32_t)cur->id : BLK_MUTEX_BOOT_OWNER;
}

void blk_mutex_init(struct blk_mutex *m, const char *name)
{
	BUG_ON(m == NULL);
	m->locked = 0;
	m->owner = BLK_MUTEX_NO_OWNER;
	m->name = name ? name : "blk_mutex";
	spinlock_init(&m->wait_lock, m->name);
	m->contended = 0;
}

void blk_mutex_lock(struct blk_mutex *m)
{
	task_t *cur = sched_current();
	uint64_t me = blk_mutex_self();
	/* Sleeping needs both a task to park and interrupts to wake it with;
	 * otherwise this is boot (single-threaded) or an IRQs-off path, and a
	 * held mutex can only be held by another CPU: spin for it. */
	int can_sleep = cur != NULL && irqs_enabled();
	uint64_t spins = 0;
	int warned = 0;

	BUG_ON(m == NULL);
	if (can_sleep)
		might_sleep();
	/* Not recursive.  Reading owner without the lock is fine for this
	 * test: if it equals us, it is us, and nobody else can change it. */
	BUG_ON(m->locked && m->owner == me && cur != NULL);

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&m->wait_lock, &fl);
		if (!m->locked) {
			m->locked = 1;
			m->owner = me;
			spin_unlock_irqrestore(&m->wait_lock, fl);
			task_sleep_lock_taken();
			return;
		}
		m->contended++;
		if (can_sleep) {
			/* Publish the sleep under wait_lock: the holder's unlock
			 * clears `locked' under the same lock and then wakes the
			 * channel, so it either sees us blocked or we see the
			 * mutex free on the next pass. */
			cur->wait_channel = (void *)&m->locked;
			task_set_blocked(cur);
			spin_unlock_irqrestore(&m->wait_lock, fl);
			sched_schedule();
			continue;
		}
		spin_unlock_irqrestore(&m->wait_lock, fl);
		__asm__ volatile("pause" ::: "memory");
		if (++spins == 200000000ULL && !warned) {
			warned = 1;
			kprintf(BLK_LOG_PREFIX
				"mutex %s: spinning for a long time (owner %lld)\n",
				m->name, (long long)m->owner);
		}
	}
}

int blk_mutex_trylock(struct blk_mutex *m)
{
	uint64_t fl;
	int got = 0;

	BUG_ON(m == NULL);
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (!m->locked) {
		m->locked = 1;
		m->owner = blk_mutex_self();
		got = 1;
	}
	spin_unlock_irqrestore(&m->wait_lock, fl);
	if (got)
		task_sleep_lock_taken();
	return got;
}

void blk_mutex_unlock(struct blk_mutex *m)
{
	uint64_t fl;
	uint64_t me = blk_mutex_self();

	BUG_ON(m == NULL);
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (WARN(!m->locked || m->owner != me,
		 "blk_mutex %s unlocked by %lld, held=%d owner=%lld", m->name,
		 (long long)me, m->locked, (long long)m->owner)) {
		spin_unlock_irqrestore(&m->wait_lock, fl);
		return;
	}
	m->locked = 0;
	m->owner = BLK_MUTEX_NO_OWNER;
	spin_unlock_irqrestore(&m->wait_lock, fl);
	task_sleep_lock_dropped();
	sched_wake_channel((void *)&m->locked);
}

int blk_mutex_held(const struct blk_mutex *m)
{
	if (!m)
		return 0;
	return m->locked && m->owner == blk_mutex_self();
}

int blk_mutex_release_if_owner(struct blk_mutex *m, uint64_t task_id)
{
	uint64_t fl;
	int released = 0;

	if (!m)
		return 0;
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (m->locked && m->owner == task_id) {
		m->locked = 0;
		m->owner = BLK_MUTEX_NO_OWNER;
		released = 1;
	}
	spin_unlock_irqrestore(&m->wait_lock, fl);
	if (released) {
		kprintf(BLK_LOG_PREFIX
			"mutex %s released on behalf of dead task %llu\n",
			m->name, (unsigned long long)task_id);
		sched_wake_channel((void *)&m->locked);
	}
	return released;
}

/* ==========================================================================
 * Request objects
 * ========================================================================== */

void blk_request_init(struct blk_request *req, block_device_t *bdev,
		      enum blk_op op, uint64_t lba, uint32_t count)
{
	BUG_ON(req == NULL);
	mm_memset(req, 0, sizeof(*req));
	req->magic = BLK_REQUEST_MAGIC;
	req->op = op;
	req->bdev = bdev;
	req->lba = lba;
	req->count = count;
	req->sg = req->sg_inline;
	req->nsg = 0;
	req->sg_cap = BLK_SG_INLINE;
	req->state = BLK_RQ_IDLE;
	spinlock_init(&req->done_lock, "blk_req");
	wq_head_init(&req->wq, "blk_req_wq");
	refcount_set(&req->ref, 1);
	req->bl.merge_slot = -1;
	req->bl.tag = -1;
	req->submit_cpu = -1;
}

/* Make room for one more element.  0 or -ENOMEM. */
static int blk_request_grow_sg(struct blk_request *req)
{
	uint32_t ncap;
	struct blk_sg_entry *nsg;

	if (req->nsg < req->sg_cap)
		return 0;
	ncap = req->sg_cap < 8 ? 8 : req->sg_cap * 2;
	if (ncap < req->sg_cap || ncap > 65536)
		return -ENOMEM;
	nsg = (struct blk_sg_entry *)kcalloc(ncap, sizeof(*nsg));
	if (!nsg)
		return -ENOMEM;
	if (req->nsg)
		mm_memcpy(nsg, req->sg, req->nsg * sizeof(*nsg));
	if (req->sg_alloc)
		kfree(req->sg_alloc);
	req->sg_alloc = nsg;
	req->sg = nsg;
	req->sg_cap = ncap;
	return 0;
}

int blk_request_add_buf(struct blk_request *req, void *buf, uint32_t len)
{
	BUG_ON(req == NULL);
	if (WARN_ON(req->magic != BLK_REQUEST_MAGIC))
		return -EINVAL;
	if (!buf || len == 0)
		return -EINVAL;
	if (WARN(req->state != BLK_RQ_IDLE,
		 "adding a buffer to a submitted request (state %d)",
		 (int)req->state))
		return -EBUSY;
	/* A buffer that continues the previous virtual range joins it: the
	 * driver translates page by page anyway, and fewer elements mean
	 * fewer PRD entries and more room for merging. */
	if (req->nsg) {
		struct blk_sg_entry *prev = &req->sg[req->nsg - 1];
		if (!prev->phys && (uint8_t *)prev->virt + prev->len == buf &&
		    (uint64_t)prev->len + len <= 0xFFFFFFFFULL) {
			prev->len += len;
			return 0;
		}
	}
	if (blk_request_grow_sg(req))
		return -ENOMEM;
	req->sg[req->nsg].virt = buf;
	req->sg[req->nsg].phys = 0;
	req->sg[req->nsg].len = len;
	req->sg[req->nsg].flags = 0;
	req->nsg++;
	return 0;
}

int blk_request_add_phys(struct blk_request *req, uint64_t phys, uint32_t len)
{
	BUG_ON(req == NULL);
	if (WARN_ON(req->magic != BLK_REQUEST_MAGIC))
		return -EINVAL;
	if (!phys || len == 0)
		return -EINVAL;
	if (WARN(req->state != BLK_RQ_IDLE,
		 "adding a range to a submitted request (state %d)",
		 (int)req->state))
		return -EBUSY;
	/* Physically adjacent to the previous element: one element. */
	if (req->nsg) {
		struct blk_sg_entry *prev = &req->sg[req->nsg - 1];
		if (prev->phys && prev->phys + prev->len == phys &&
		    (uint64_t)prev->len + len <= 0xFFFFFFFFULL) {
			prev->len += len;
			return 0;
		}
	}
	if (blk_request_grow_sg(req))
		return -ENOMEM;
	req->sg[req->nsg].virt = phys_to_virt(phys);
	req->sg[req->nsg].phys = phys;
	req->sg[req->nsg].len = len;
	req->sg[req->nsg].flags = 0;
	req->nsg++;
	return 0;
}

/* Heap requests whose last reference was dropped where freeing is not
 * allowed (interrupt context), linked through disp_next. */
static spinlock_t g_rq_defer_lock = SPINLOCK_INIT("blk_rq_defer");
static struct blk_request *g_rq_defer_head;

static void blk_request_destroy(struct blk_request *req)
{
	if (req->sg_alloc) {
		kfree(req->sg_alloc);
		req->sg_alloc = NULL;
	}
	req->sg = NULL;
	req->magic = 0; /* a late use trips the magic checks */
	if (req->bl.internal & BLK_RQI_HEAP)
		kfree(req);
}

void blk_request_defer_destroy(struct blk_request *req)
{
	uint64_t fl;
	int first;

	spin_lock_irqsave(&g_rq_defer_lock, &fl);
	first = g_rq_defer_head == NULL;
	req->disp_next = g_rq_defer_head;
	g_rq_defer_head = req;
	spin_unlock_irqrestore(&g_rq_defer_lock, fl);
	/* kblockd frees it.  Only the request that finds the list empty
	 * kicks the reap: the reap takes the whole list, and a list that was
	 * not empty has a kick of its own still to be served (the reap
	 * empties it before anything else could see it empty) -- a request
	 * split into many pieces costs one kblockd run, not one per piece. */
	if (first)
		genhd_kick_reap();
}

void blk_request_destroy_list(struct blk_request *list)
{
	while (list) {
		struct blk_request *next = list->disp_next;

		blk_request_destroy(list);
		list = next;
	}
}

static void blk_request_destroy_any(struct blk_request *req)
{
	struct blk_cpl_ctx *c;

	/* Inline where a release may happen (outside a completion section,
	 * whose caller may still use the request), before there is any
	 * scheduler at all, and in the block softirq: the request's memory
	 * goes back to the slab allocator there (irq-safe), as a completed
	 * split piece does when its last reference is dropped. */
	if (blk_can_release_now() || sched_current() == NULL ||
	    (blk_in_softirq() && !blk_in_hardirq())) {
		blk_request_destroy(req);
		return;
	}
	/* Inside a completion section: its end destroys it. */
	c = blk_cpl_current();
	if (c) {
		req->disp_next = c->rqs;
		c->rqs = req;
		return;
	}
	blk_request_defer_destroy(req);
}

void blk_reap_deferred_requests(void)
{
	struct blk_request *list;
	uint64_t fl;

	if (!__atomic_load_n(&g_rq_defer_head, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&g_rq_defer_lock, &fl);
	list = g_rq_defer_head;
	g_rq_defer_head = NULL;
	spin_unlock_irqrestore(&g_rq_defer_lock, fl);
	while (list) {
		struct blk_request *next = list->disp_next;
		blk_request_destroy(list);
		list = next;
	}
}

void blk_request_release(struct blk_request *req)
{
	if (!req)
		return;
	if (WARN(req->magic != BLK_REQUEST_MAGIC,
		 "releasing a request that is not initialised (magic %08x)",
		 req->magic))
		return;
	if (WARN(req->state == BLK_RQ_QUEUED ||
			 req->state == BLK_RQ_DISPATCHED,
		 "releasing request %s lba %llu while it is in flight",
		 blk_op_name(req->op), (unsigned long long)req->lba))
		return;
	if (refcount_dec_and_test(&req->ref))
		blk_request_destroy_any(req);
}

struct blk_request *blk_request_alloc(block_device_t *bdev, enum blk_op op,
				      uint64_t lba, uint32_t count)
{
	struct blk_request *req;

	blk_reap_deferred_requests();
	req = (struct blk_request *)kalloc(sizeof(*req));
	if (!req)
		return NULL;
	blk_request_init(req, bdev, op, lba, count);
	req->bl.internal |= BLK_RQI_HEAP;
	return req;
}

void blk_request_free(struct blk_request *req)
{
	if (!req)
		return;
	if (WARN(req->magic != BLK_REQUEST_MAGIC,
		 "freeing a request that is not initialised"))
		return;
	WARN_ON(!(req->bl.internal & BLK_RQI_HEAP));
	/* Called from end_io the block layer still holds its reference; the
	 * memory goes when that is dropped at the end of the completion. */
	if (refcount_dec_and_test(&req->ref))
		blk_request_destroy_any(req);
}

/* ==========================================================================
 * Finishing a request
 * ========================================================================== */

block_device_t *blk_rq_finish_keep_bdev(struct blk_request *req)
{
	uint64_t fl;
	int last, heap;
	block_device_t *ref_bdev = NULL;

	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	/* What has to be done after `done' is published is copied out now:
	 * from then on the request may be gone. */
	if (req->bl.internal & BLK_RQI_INFLIGHT_REF) {
		ref_bdev = req->bdev;
		req->bl.internal &= ~BLK_RQI_INFLIGHT_REF;
	}
	req->state = BLK_RQ_COMPLETE;
	__atomic_thread_fence(__ATOMIC_RELEASE);

	/* end_io first: an asynchronous submitter learns the result here and
	 * may free the request (blk_request_free drops ITS reference only;
	 * the block layer's keeps the memory alive until below). */
	if (req->end_io)
		req->end_io(req, req->status);

	spin_lock_irqsave(&req->done_lock, &fl);
	req->done = 1;
	wq_wake_all(&req->wq);
	heap = (req->bl.internal & BLK_RQI_HEAP) != 0;
	last = refcount_dec_and_test(&req->ref);
	spin_unlock_irqrestore(&req->done_lock, fl);

	if (last) {
		/* Only a heap request may end here: a stack request's
		 * submitter holds its reference until after blk_wait. */
		if (heap)
			blk_request_destroy_any(req);
		else
			WARN_RATELIMIT(1,
				       "stack request released before it completed");
	}
	return ref_bdev;
}

void blk_rq_finish(struct blk_request *req)
{
	block_device_t *ref_bdev = blk_rq_finish_keep_bdev(req);

	if (ref_bdev)
		blk_bdev_put_any(ref_bdev);
}

/* ==========================================================================
 * Legacy devices: synchronous execution through the original callbacks
 * ========================================================================== */

static int blk_submit_legacy(struct blk_request *req)
{
	block_device_t *dev = req->bdev;
	uint32_t ss = dev->sector_size ? dev->sector_size : 512;
	int err = 0;
	uint64_t done_bytes = 0;

	switch (req->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE: {
		uint64_t lba = req->lba;

		if (req->count == 0 || req->lba + req->count < req->lba ||
		    req->lba + req->count > dev->total_sectors)
			return -EINVAL;
		if (req->op == BLK_OP_READ ? !dev->read : !dev->write)
			return -EOPNOTSUPP;
		/* Each element must be whole sectors: the old interface moves
		 * sectors to and from one contiguous buffer. */
		for (uint32_t i = 0; i < req->nsg; i++)
			if (req->sg[i].len % ss)
				return -EINVAL;
		req->bl.internal |= BLK_RQI_LEGACY;
		req->state = BLK_RQ_DISPATCHED;
		req->submit_ns = hrtimer_now_ns();
		req->dispatch_ns = req->submit_ns;
		for (uint32_t i = 0; i < req->nsg && !err; i++) {
			unsigned long n = req->sg[i].len / ss;
			int st;

			if (req->op == BLK_OP_READ)
				st = dev->read(dev, (unsigned long)lba, n,
					       req->sg[i].virt);
			else
				st = dev->write(dev, (unsigned long)lba, n,
						req->sg[i].virt);
			if (st != ST_OK) {
				err = blk_status_to_errno(st);
				break;
			}
			lba += n;
			done_bytes += req->sg[i].len;
		}
		break;
	}
	case BLK_OP_FLUSH:
		req->bl.internal |= BLK_RQI_LEGACY;
		req->state = BLK_RQ_DISPATCHED;
		req->submit_ns = hrtimer_now_ns();
		if (dev->sync) {
			int st = dev->sync(dev);
			if (st != ST_OK)
				err = blk_status_to_errno(st);
		}
		break;
	default:
		/* No discard, zeroing or pass-through behind the old
		 * callbacks: rejected, the request is untouched. */
		return -EOPNOTSUPP;
	}
	req->status = err;
	{
		uint64_t want = blk_rq_data_bytes(req);
		uint64_t resid = want > done_bytes ? want - done_bytes : 0;
		req->resid_bytes = resid > 0xFFFFFFFFULL ? 0xFFFFFFFFu :
							   (uint32_t)resid;
	}
	req->complete_ns = hrtimer_now_ns();
	refcount_inc(&req->ref); /* the finishing step drops it */
	blk_rq_finish(req);
	return 0;
}

/* ==========================================================================
 * Submission
 * ========================================================================== */

/* Check the data list of a READ/WRITE/DRV request against the device. */
static int blk_validate_sg(const struct blk_request *req,
			   const struct blk_queue *q)
{
	uint32_t align = q ? q->limits.dma_alignment : 1;
	uint64_t total = 0;

	if (!req->sg || req->nsg == 0)
		return -EINVAL;
	if (req->nsg > req->sg_cap)
		return -EINVAL;
	for (uint32_t i = 0; i < req->nsg; i++) {
		const struct blk_sg_entry *e = &req->sg[i];

		if (!e->virt || e->len == 0)
			return -EINVAL;
		/* The controller's alignment rule (AHCI: even address and
		 * even length); a misaligned caller bounces first. */
		if ((((uint64_t)e->virt) | e->len) & align)
			return -EINVAL;
		total += e->len;
	}
	if (req->op == BLK_OP_READ || req->op == BLK_OP_WRITE) {
		if (total != (uint64_t)req->count * req->bdev->sector_size)
			return -EINVAL;
	}
	return 0;
}

/* Range and capability validation of an extended-device request. */
static int blk_validate_extended(struct blk_request *req)
{
	block_device_t *bdev = req->bdev;
	struct gendisk *disk = bdev->disk;
	struct blk_queue *q = bdev->queue;
	uint64_t end;

	if (!disk || !q || disk->magic != GENDISK_MAGIC)
		return -ENODEV;
	if ((bdev->flags & BDEV_F_DEAD) || (disk->flags & GENHD_FL_DEAD))
		return -ENODEV;
	if ((bdev->parent && (bdev->parent->flags & BDEV_F_DEAD)))
		return -ENODEV;
	if (!disk->ops || !disk->ops->submit)
		return -ENODEV;

	switch (req->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE:
	case BLK_OP_DISCARD:
	case BLK_OP_SECURE_ERASE:
	case BLK_OP_WRITE_ZEROES:
		if (!bdev->ready)
			return (disk->flags & GENHD_FL_REMOVABLE) &&
					       !(disk->flags &
						 GENHD_FL_MEDIA_PRESENT) ?
				       -ENOMEDIUM :
				       -ENODEV;
		if (req->count == 0)
			return -EINVAL;
		end = req->lba + req->count;
		if (end < req->lba || end > bdev->total_sectors)
			return -EINVAL;
		break;
	case BLK_OP_FLUSH:
		if (req->count != 0)
			return -EINVAL;
		break;
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		/* Pass-through: the command object carries everything; it
		 * may address a drive with no medium (TEST UNIT READY). */
		if (!req->drv_cmd)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	if (blk_op_is_write(req->op) && req->op != BLK_OP_DRV_OUT &&
	    blk_bdev_read_only(bdev))
		return -EROFS;

	switch (req->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE:
		return blk_validate_sg(req, q);
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		return req->nsg ? blk_validate_sg(req, q) : 0;
	case BLK_OP_DISCARD:
		if (!q->limits.discard_granularity ||
		    !q->limits.max_discard_sectors)
			return -EOPNOTSUPP;
		if (req->count > q->limits.max_discard_sectors)
			return -EINVAL;
		return 0;
	case BLK_OP_SECURE_ERASE:
		if (!q->limits.secure_erase || !q->limits.max_discard_sectors)
			return -EOPNOTSUPP;
		if (req->count > q->limits.max_discard_sectors)
			return -EINVAL;
		return 0;
	case BLK_OP_WRITE_ZEROES:
		if (!q->limits.max_write_zeroes_sectors)
			return -EOPNOTSUPP;
		if (req->count > q->limits.max_write_zeroes_sectors)
			return -EINVAL;
		return 0;
	default:
		return 0;
	}
}

/* Decide the flush sequence of a write.  PREFLUSH and FUA are the block
 * layer's to honour: the driver only ever sees plain FLUSH requests and,
 * on a device that has it, the FUA bit. */
/* The flush policy of a request: which of the steps PREFLUSH, DATA and
 * POSTFLUSH it needs.  A request that needs a flush step goes through the
 * flush machinery (blk_queue.c) with the steps it does not need counted
 * as done; a plain data request does not.  FUA reaches the driver only on
 * a disk with native FUA (else it becomes the POSTFLUSH step), PREFLUSH
 * never does. */
static void blk_setup_flush_sequence(struct blk_request *req)
{
	block_device_t *bdev = req->bdev;
	int cache = bdev->flush_supported;
	uint32_t policy = 0;

	req->bl.seq = 0;
	if (req->op != BLK_OP_WRITE && req->op != BLK_OP_FLUSH) {
		/* The two flags mean nothing on anything but a write. */
		req->flags &= ~(BLK_REQ_PREFLUSH | BLK_REQ_FUA);
		return;
	}
	if (req->count && req->op == BLK_OP_WRITE)
		policy |= BLK_SEQ_DATA;
	if (cache) {
		if ((req->flags & BLK_REQ_PREFLUSH) || req->op == BLK_OP_FLUSH)
			policy |= BLK_SEQ_PREFLUSH;
		if ((req->flags & BLK_REQ_FUA) && !bdev->fua_supported &&
		    (policy & BLK_SEQ_DATA))
			policy |= BLK_SEQ_POSTFLUSH;
	}
	req->flags &= ~BLK_REQ_PREFLUSH;
	/* No volatile cache: every write is already durable when it
	 * completes, and FUA is a no-op the driver need not see. */
	if (!cache || !bdev->fua_supported)
		req->flags &= ~BLK_REQ_FUA;
	if (!(policy & (BLK_SEQ_PREFLUSH | BLK_SEQ_POSTFLUSH)))
		return;
	req->bl.seq = BLK_SEQ_ACTIONS & ~policy;
	req->bl.internal |= BLK_RQI_FLUSH_SEQ;
}

/* ==========================================================================
 * Splitting at submission
 *
 * A request larger than one command of its disk -- more sectors than the
 * queue's limit for its op (max_sectors, max_discard_sectors,
 * max_write_zeroes_sectors), or more segments than max_segments as the
 * controller counts them (blk_sg_segments) -- is split here, before it
 * reaches the queue, into pieces that each fit one command.  The pieces
 * are requests of their own (sorted, merged within the same limits,
 * dispatched and completed independently); the submitter's request ends
 * when the last piece has, with the first error and the bytes done up to
 * the first piece that failed.
 * ========================================================================== */

/* The sector limit of one request of `op' on `q' (0: no data op). */
static uint64_t blk_rq_max_sectors(const struct blk_queue *q, enum blk_op op)
{
	const struct blk_limits *l = &q->limits;

	switch (op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE:
		return l->max_sectors ? l->max_sectors : l->max_hw_sectors;
	case BLK_OP_DISCARD:
	case BLK_OP_SECURE_ERASE:
		return l->max_discard_sectors;
	case BLK_OP_WRITE_ZEROES:
		return l->max_write_zeroes_sectors;
	default:
		return 0;
	}
}

/* The sectors the first piece of `req' from sector `done' (sg cursor
 * idx/off) may carry: what the sector limit allows, cut where the
 * segment limit is reached (at a sector boundary). */
static uint64_t blk_split_piece(const struct blk_queue *q,
				const struct blk_request *req, uint64_t done,
				uint32_t idx, uint32_t off, uint32_t ss)
{
	uint64_t left = (uint64_t)req->count - done;
	uint64_t max = blk_rq_max_sectors(q, req->op);
	uint32_t seg_limit = q->limits.max_segments ? q->limits.max_segments :
						      BLK_DEFAULT_MAX_SEGMENTS;
	uint64_t want, bytes = 0;
	uint32_t segs = 0;

	if (max && max < left)
		left = max;
	if (!blk_op_has_data(req->op))
		return left;
	want = left * ss;
	while (bytes < want && idx < req->nsg) {
		const struct blk_sg_entry *e = &req->sg[idx];
		uint32_t take = e->len - off;
		uint32_t n;

		if (take > want - bytes)
			take = (uint32_t)(want - bytes);
		n = blk_sg_segments(e, off, take, &q->limits);
		if (segs + n > seg_limit) {
			/* What still fits of this element: whole pages (or
			 * segments) up to the limit. */
			uint32_t room = seg_limit - segs;
			uint32_t fit = 0;

			while (room && fit < take) {
				uint32_t step;

				if (e->phys) {
					step = q->limits.max_segment_size ?
						       q->limits.max_segment_size :
						       65536;
				} else {
					uint64_t at =
						(uint64_t)(unsigned long)e->virt +
						off + fit;
					step = 4096 - (uint32_t)(at & 4095);
				}
				if (step > take - fit)
					step = take - fit;
				fit += step;
				room--;
			}
			bytes += fit;
			break;
		}
		segs += n;
		bytes += take;
		idx++;
		off = 0;
	}
	return bytes / ss;
}

static void blk_split_finish(struct blk_request *req);

/* A piece ended: account it on the submitter's request; the last one ends
 * that. */
static void blk_split_end_io(struct blk_request *piece, int status)
{
	struct blk_request *req = piece->private_data;
	uint32_t ss = req->bdev && req->bdev->sector_size ?
			      req->bdev->sector_size :
			      512;
	uint64_t start = (piece->lba - req->lba) * ss;
	uint64_t total = (uint64_t)piece->count * ss;
	uint64_t fl;
	int last;

	if (!blk_op_has_data(piece->op))
		total = 0;
	spin_lock_irqsave(&req->done_lock, &fl);
	if (status) {
		uint64_t ok = piece->resid_bytes && piece->resid_bytes < total ?
				      total - piece->resid_bytes :
				      0;

		if (!req->status)
			req->status = status;
		if (start + ok < req->bl.split_done_bytes)
			req->bl.split_done_bytes = start + ok;
	}
	last = --req->bl.split_pending == 0;
	spin_unlock_irqrestore(&req->done_lock, fl);
	blk_request_free(piece);
	if (last)
		blk_split_finish(req);
}

/* Every piece is through: end the submitter's request. */
static void blk_split_finish(struct blk_request *req)
{
	uint64_t total = blk_op_has_data(req->op) ? blk_rq_data_bytes(req) : 0;

	req->complete_ns = hrtimer_now_ns();
	req->resid_bytes = 0;
	if (req->status && total > req->bl.split_done_bytes)
		req->resid_bytes = (uint32_t)(total - req->bl.split_done_bytes);
	blk_rq_finish(req);
}

/* Add `len' bytes of `req''s list from element idx, offset off, to the
 * piece.  0 or -ENOMEM. */
static int blk_split_copy_sg(struct blk_request *piece,
			     const struct blk_request *req, uint32_t *idx,
			     uint32_t *off, uint64_t len)
{
	while (len) {
		const struct blk_sg_entry *e = &req->sg[*idx];
		uint32_t take = e->len - *off;
		int rc;

		if (take > len)
			take = (uint32_t)len;
		if (e->phys)
			rc = blk_request_add_phys(piece, e->phys + *off, take);
		else
			rc = blk_request_add_buf(piece,
						 (uint8_t *)e->virt + *off,
						 take);
		if (rc)
			return rc;
		len -= take;
		*off += take;
		if (*off == e->len) {
			(*idx)++;
			*off = 0;
		}
	}
	return 0;
}

static int blk_rq_needs_split(const struct blk_queue *q,
			      const struct blk_request *req)
{
	uint32_t ss = req->bdev && req->bdev->sector_size ?
			      req->bdev->sector_size :
			      512;
	uint64_t max = blk_rq_max_sectors(q, req->op);

	if (req->op == BLK_OP_FLUSH || req->op == BLK_OP_DRV_IN ||
	    req->op == BLK_OP_DRV_OUT || req->count == 0)
		return 0;
	if (max && req->count > max)
		return 1;
	return blk_op_has_data(req->op) &&
	       blk_split_piece(q, req, 0, 0, 0, ss) < req->count;
}

/* Split `req' (validated, remapped, its in-flight references taken, the
 * queue entered) into pieces and submit each.  Returns 0: the request is
 * the pieces' now. */
static int blk_submit_split(struct blk_queue *q, struct blk_request *req)
{
	uint32_t ss = req->bdev->sector_size ? req->bdev->sector_size : 512;
	uint64_t done = 0;
	uint32_t idx = 0, off = 0;
	struct blk_plug plug;
	uint64_t fl;
	int last, plugged;

	req->status = 0;
	req->state = BLK_RQ_QUEUED;
	req->bl.internal |= BLK_RQI_SPLIT;
	req->bl.split_pending = 1; /* the submission's own count */
	req->bl.split_done_bytes = blk_op_has_data(req->op) ?
					   blk_rq_data_bytes(req) :
					   0;
	blk_queue_leave(q); /* each piece enters on its own */

	/* The pieces collect on a plug and reach the queue together, sorted
	 * and merged there, when the loop is through (or earlier, when the
	 * plug fills up or the task has to sleep).  Inside a plug of the
	 * caller's this one is a no-op and the caller's collects them.  Only
	 * a task that may sleep has a plug (it lives in the task). */
	plugged = blk_can_sleep();
	if (plugged)
		blk_start_plug(&plug);
	while (done < req->count) {
		uint64_t n = blk_split_piece(q, req, done, idx, off, ss);
		struct blk_request *piece;
		int rc = 0;

		if (n == 0) {
			rc = -EINVAL; /* not one sector fits a command */
		} else {
			piece = blk_request_alloc(req->bdev, req->op,
						  req->lba + done, (uint32_t)n);
			if (!piece)
				rc = -ENOMEM;
		}
		if (!rc) {
			/* Every piece but the last is final as it is: it
			 * does not merge (the last one may). */
			piece->flags = req->flags;
			if (done + n < req->count)
				piece->flags |= BLK_REQ_NOMERGE;
			/* The cache is emptied before the first piece. */
			if (done)
				piece->flags &= ~BLK_REQ_PREFLUSH;
			piece->timeout_ms = req->timeout_ms;
			piece->end_io = blk_split_end_io;
			piece->private_data = req;
			if (blk_op_has_data(req->op))
				rc = blk_split_copy_sg(piece, req, &idx, &off,
						       n * ss);
			if (!rc) {
				spin_lock_irqsave(&req->done_lock, &fl);
				req->bl.split_pending++;
				spin_unlock_irqrestore(&req->done_lock, fl);
				rc = blk_submit(piece);
				if (rc) {
					spin_lock_irqsave(&req->done_lock, &fl);
					req->bl.split_pending--;
					spin_unlock_irqrestore(&req->done_lock,
							       fl);
				}
			}
			if (rc)
				blk_request_free(piece);
		}
		if (rc) {
			/* The rest is not submitted: the request fails from
			 * here on. */
			uint64_t at = done * ss;

			spin_lock_irqsave(&req->done_lock, &fl);
			if (!req->status)
				req->status = rc;
			if (blk_op_has_data(req->op) &&
			    at < req->bl.split_done_bytes)
				req->bl.split_done_bytes = at;
			spin_unlock_irqrestore(&req->done_lock, fl);
			break;
		}
		done += n;
	}
	if (plugged)
		blk_finish_plug(&plug);

	spin_lock_irqsave(&req->done_lock, &fl);
	last = --req->bl.split_pending == 0;
	spin_unlock_irqrestore(&req->done_lock, fl);
	if (last)
		blk_split_finish(req);
	return 0;
}

int blk_submit(struct blk_request *req)
{
	block_device_t *bdev;
	struct blk_queue *q;
	int rc;

	BUG_ON(req == NULL);
	if (WARN(req->magic != BLK_REQUEST_MAGIC,
		 "submitting an uninitialised request (magic %08x)",
		 req->magic))
		return -EINVAL;
	if (WARN(req->state != BLK_RQ_IDLE,
		 "request submitted twice (state %d); re-initialise it first",
		 (int)req->state))
		return -EBUSY;
	bdev = req->bdev;
	if (!bdev)
		return -ENODEV;
	if ((unsigned)req->op >= BLK_OP_NR)
		return -EINVAL;
	if (blk_can_release_now()) {
		blk_reap_deferred_requests();
		genhd_reap_deferred();
	}

	if (!blk_bdev_is_extended(bdev))
		return blk_submit_legacy(req);

	rc = blk_validate_extended(req);
	if (rc)
		return rc;
	q = bdev->queue;

	/* Hold the queue while the request goes in: a disk being deleted
	 * marks its queue dying and then waits for everyone inside. */
	rc = blk_queue_enter(q);
	if (rc)
		return rc;

	req->disk = bdev->disk;
	req->queue = q;
	/* The remap: a partition's sector 0 is its start on the disk.  For
	 * FLUSH and pass-through the value is informational only. */
	req->sector = req->lba + bdev->start_lba;
	req->submit_ns = hrtimer_now_ns();
	req->status = 0;
	req->resid_bytes = 0;
	req->done = 0;
	req->merged_next = NULL;
	req->nr_merged = 0;
	req->bl.merge_slot = -1;
	req->bl.tag = -1;
	req->bl.internal &= BLK_RQI_HEAP;
	if (req->timeout_ms == 0)
		req->timeout_ms = BLK_DEFAULT_TIMEOUT_MS;

	/* The in-flight references: the request's own (dropped by the
	 * finishing step) and the device's (a partition pins its disk). */
	refcount_inc(&req->ref);
	blk_bdev_get(bdev);
	req->bl.internal |= BLK_RQI_INFLIGHT_REF;

	/* A FLUSH on a device without a volatile cache has nothing to do. */
	if (req->op == BLK_OP_FLUSH && !bdev->flush_supported) {
		req->state = BLK_RQ_COMPLETE;
		req->complete_ns = req->submit_ns;
		blk_queue_leave(q);
		blk_rq_finish(req);
		return 0;
	}

	/* Larger than one command of the disk: split into pieces, each a
	 * request of its own. */
	if (blk_rq_needs_split(q, req))
		return blk_submit_split(q, req);

	blk_setup_flush_sequence(req);

	/* One of the queue's request slots (nr_requests), held until the
	 * request has completed: a submitter that may sleep waits for one;
	 * one that cannot, and a HEAD/PM request, takes one regardless -- it
	 * has no other way to make progress. */
	rc = blk_queue_get_sched_tag(q, req);
	if (rc) {
		/* Dying (perhaps while we waited): undo and reject. */
		req->bl.internal &= ~(BLK_RQI_INFLIGHT_REF | BLK_RQI_FLUSH_SEQ);
		req->bl.seq = 0;
		refcount_dec_and_test(&req->ref);
		blk_queue_leave(q);
		blk_bdev_put_any(bdev);
		return rc;
	}

	/* On the submitting task's plug, or into the queue now. */
	req->state = BLK_RQ_QUEUED;
	if (blk_add_rq_to_plug(req))
		return 0;
	blk_queue_insert(q, req);
	blk_queue_leave(q);
	return 0;
}

/* ==========================================================================
 * Waiting
 * ========================================================================== */

static int blk_rq_is_done(struct blk_request *req)
{
	uint64_t fl;
	int d;

	/* Under done_lock: the finisher publishes `done' and wakes us while
	 * holding it, so once we have seen it we know the finisher is out of
	 * the request and our caller may free it. */
	spin_lock_irqsave(&req->done_lock, &fl);
	d = req->done;
	spin_unlock_irqrestore(&req->done_lock, fl);
	return d;
}

int blk_wait(struct blk_request *req)
{
	struct gendisk *disk;
	int polled;

	BUG_ON(req == NULL);
	if (WARN(req->magic != BLK_REQUEST_MAGIC,
		 "waiting for an uninitialised request"))
		return -EINVAL;
	if (WARN(req->state == BLK_RQ_IDLE,
		 "waiting for a request that was never submitted"))
		return -EINVAL;

	disk = req->disk;
	/* A waiter that keeps a done queue (an error handler) may be the one
	 * holding this request's completion: finish what it collected. */
	blk_flush_own_done();
	polled = !blk_can_sleep() || (req->flags & BLK_REQ_POLLED) ||
		 (req->queue && (req->queue->flags & BLK_QUEUE_FL_POLLED));

	if (polled) {
		/* Drive the completion ourselves.  Before the scheduler (and
		 * with interrupts off) this is the only way it can happen;
		 * after it, a driver without interrupts gets the same help,
		 * and we yield between attempts instead of burning the CPU. */
		for (;;) {
			if (blk_rq_is_done(req))
				break;
			if (disk && disk->ops && disk->ops->poll)
				disk->ops->poll(disk);
			/* Completions waiting for this CPU's block softirq
			 * (a polled driver completing from a task) run in
			 * the poller. */
			if (!blk_in_hardirq() && !blk_in_softirq())
				blk_softirq_poll();
			blk_flush_own_done();
			/* Interrupts off: kblockd may never get this
			 * processor -- run its work (queue runs) here, or the
			 * request may wait for a dispatch that never comes;
			 * and the queue's timeout timer cannot fire, so the
			 * timeouts are checked here as well. */
			if (!irqs_enabled()) {
				blk_work_run_pending();
				blk_queue_poll_timeouts(req->queue);
				/* A driver that refused with nothing in flight
				 * is asked again by the poller (no kblockd
				 * delay can run here). */
				if (req->queue && req->state == BLK_RQ_QUEUED)
					blk_queue_run(req->queue);
			}
			if (blk_rq_is_done(req))
				break;
			if (blk_can_sleep())
				sched_yield_in_kernel();
			else
				lapic_delay_us(BLK_POLL_STEP_US);
		}
	} else {
		task_t *cur = sched_current();

		might_sleep();
		for (;;) {
			struct wait_queue_entry we;
			uint64_t fl;

			spin_lock_irqsave(&req->done_lock, &fl);
			if (req->done) {
				spin_unlock_irqrestore(&req->done_lock, fl);
				break;
			}
			wq_entry_init(&we, cur);
			wq_add(&req->wq, &we);
			cur->wait_channel = req;
			/* Unbounded: the request's timeout is the driver's
			 * business; it completes the request after its error
			 * handling, which wakes us. */
			cur->wakeup_tick = 0;
			task_set_blocked(cur);
			spin_unlock_irqrestore(&req->done_lock, fl);
			sched_schedule();
			cur->wait_channel = NULL;
			wq_remove(&req->wq, &we);
		}
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return req->status;
}

int blk_execute(struct blk_request *req)
{
	int rc = blk_submit(req);

	if (rc)
		return rc;
	return blk_wait(req);
}

/* ==========================================================================
 * Completion
 * ========================================================================== */

/* Map the result of a merged request onto the requests it carried.  The
 * transfer covered [S, S+N) of the disk; `resid' bytes at its end did not
 * happen.  A request wholly inside the transferred part succeeded; any
 * other gets the error (or -EIO for a short transfer without one) and the
 * bytes beyond the transferred part as its residue. */
static void blk_rq_result_for_range(uint64_t start, uint64_t count,
				    uint64_t done_end, int status, uint32_t ss,
				    int *out_status, uint32_t *out_resid)
{
	uint64_t end = start + count;

	if (end <= done_end) {
		*out_status = 0;
		*out_resid = 0;
		return;
	}
	*out_status = status ? status : -EIO;
	{
		uint64_t from = start > done_end ? start : done_end;
		uint64_t bytes = (end - from) * ss;
		*out_resid = bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFu :
						     (uint32_t)bytes;
	}
}

void blk_request_complete(struct blk_request *req, int status,
			  uint32_t resid_bytes)
{
	struct blk_queue *q;
	struct blk_request *children;
	struct blk_done_snap snap;
	block_device_t *ref_bdev;
	int own_status = status;
	uint32_t own_resid = resid_bytes;

	BUG_ON(req == NULL);
	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	if (unlikely(req->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "completing a request with bad magic %08x",
			       req->magic);
		return;
	}
	if (unlikely(req->state == BLK_RQ_COMPLETE ||
		     req->state == BLK_RQ_IDLE)) {
		WARN_RATELIMIT(1,
			       "request %s sector %llu completed twice (state %d)",
			       blk_op_name(req->op),
			       (unsigned long long)req->sector,
			       (int)req->state);
		return;
	}
	if (unlikely(status > 0)) {
		WARN_RATELIMIT(1, "positive completion status %d", status);
		status = -EIO;
		own_status = status;
	}
	q = req->queue;

	/* The flush machinery: the queue's own flush request, or the data
	 * step of a request in a flush sequence, moves its sequence on; the
	 * request ends here once its sequence is over (FLUSH_END). */
	if (q && (req->bl.internal & (BLK_RQI_FLUSH_RQ | BLK_RQI_FLUSH_SEQ)) &&
	    !(req->bl.internal & BLK_RQI_FLUSH_END)) {
		blk_flush_complete(q, req, status, resid_bytes);
		return;
	}
	req->bl.internal &= ~(BLK_RQI_FLUSH_SEQ | BLK_RQI_FLUSH_END);
	req->bl.seq = 0;

	{
		uint64_t total = blk_rq_data_bytes(req);
		if (resid_bytes > total)
			resid_bytes = (uint32_t)total;
	}
	req->complete_ns = hrtimer_now_ns();
	req->status = status;
	req->resid_bytes = resid_bytes;

	/* A failed request is an error line, not a kernel warning: at most
	 * 10 in 5 seconds (the count of the dropped ones follows). */
	if (status && !(req->flags & BLK_REQ_QUIET)) {
		static uint64_t win_ns;
		static uint32_t in_win, missed;
		uint64_t now = hrtimer_now_ns();

		if (now - win_ns > 5000000000ULL) {
			if (missed)
				kprintf("[blk] %u I/O error messages suppressed\n",
					missed);
			win_ns = now;
			in_win = 0;
			missed = 0;
		}
		if (in_win < 10) {
			in_win++;
			kprintf("[blk] I/O error %d, dev %s, sector %llu op %s count %u\n",
				status, req->bdev && req->bdev->name ?
						req->bdev->name :
						"?",
				(unsigned long long)req->sector,
				blk_op_name(req->op), req->count);
		} else {
			missed++;
		}
	}

	/* Merged requests: decide each one's result from the carrier's
	 * range before the carrier is put back to its own. */
	children = req->merged_next;
	if (children) {
		uint32_t ss = req->bdev ? req->bdev->sector_size : 512;
		uint64_t S = req->sector, N = req->count;
		uint64_t done_end;

		if (!blk_op_has_data(req->op)) {
			done_end = status ? S : S + N;
		} else if (status == 0 && resid_bytes == 0) {
			done_end = S + N;
		} else if (status && resid_bytes == 0) {
			done_end = S; /* nothing known to be done */
		} else {
			uint64_t total = N * ss;
			uint64_t done = resid_bytes >= total ?
						0 :
						total - resid_bytes;
			done_end = S + done / ss;
		}
		for (struct blk_request *c = children; c; c = c->merged_next) {
			int cs;
			uint32_t cr;

			blk_rq_result_for_range(c->sector, c->count, done_end,
						status, ss, &cs, &cr);
			if (!blk_op_has_data(c->op))
				cr = 0;
			c->status = cs;
			c->resid_bytes = cr;
			c->complete_ns = req->complete_ns;
		}
		if (req->bl.internal & BLK_RQI_CARRIER) {
			blk_rq_result_for_range(req->bl.own_sector,
						req->bl.own_count, done_end,
						status, ss, &own_status,
						&own_resid);
			if (!blk_op_has_data(req->op))
				own_resid = 0;
		}
	} else {
		own_status = status;
		own_resid = resid_bytes;
	}

	/* What the queue's side of the completion needs -- the tag, the
	 * merge pool array, the statistics of the whole merged transfer --
	 * copied out now: once the waiter below is woken it may free the
	 * request.  The carrier still describes the merged transfer here. */
	if (q)
		blk_queue_done_prepare(q, req, &snap);

	/* Put the carrier back to what its submitter built. */
	req->merged_next = NULL;
	req->nr_merged = 0;
	if (req->bl.internal & BLK_RQI_CARRIER)
		blk_rq_restore_carrier(req);
	req->status = own_status;
	req->resid_bytes = own_resid;

	/* The submitters learn their results first: the carried requests
	 * (each is an independent object, on the same device as the carrier,
	 * whose reference outlives theirs), then the carrier, whose end_io or
	 * waiter may free it. */
	while (children) {
		struct blk_request *c = children;
		children = c->merged_next;
		c->merged_next = NULL;
		c->bl.internal &= ~BLK_RQI_MERGED;
		blk_rq_finish(c);
	}
	ref_bdev = blk_rq_finish_keep_bdev(req);

	/* Then the queue: statistics, the elevator's count, the tag, the
	 * restart of a dispatch that waits for a tag, the submitters and
	 * drainers waiting on the queue.  From the copy alone. */
	if (q)
		blk_queue_done_snap(q, &snap);

	/* Last, the device reference: until here it kept the disk, and with
	 * it the queue, from going away under the step above. */
	if (ref_bdev)
		blk_bdev_put_any(ref_bdev);
}

/* ==========================================================================
 * The hand-off from the driver's interrupt handler to the block softirq
 * ========================================================================== */

/* The block softirq's half of a completion (softirq_entry.fn): the
 * driver's ->complete -- or the end of the request with the result the
 * driver recorded -- inside a completion section, whose end hands the
 * releases parked on it to kblockd (genhd.c).  Also run directly by an
 * error handler or a poller (process context). */
static void blk_rq_softirq_done(struct blk_softirq_entry *e)
{
	struct blk_request *req = (struct blk_request *)
		((char *)e - __builtin_offsetof(struct blk_request,
						softirq_entry));
	struct gendisk *disk = req->disk;
	struct blk_cpl_ctx cpl;

	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	blk_cpl_begin(&cpl);
	if (disk && disk->ops && disk->ops->complete)
		disk->ops->complete(req);
	else
		blk_request_complete(req, req->drv_status, req->drv_resid);
	blk_cpl_end(&cpl);
}

/* The block softirq's end of a request that never reached its driver
 * (blk_request_end_deferred): no driver ->complete, only the end with the
 * recorded result, inside a completion section like any other. */
static void blk_rq_softirq_end(struct blk_softirq_entry *e)
{
	struct blk_request *req = (struct blk_request *)
		((char *)e - __builtin_offsetof(struct blk_request,
						softirq_entry));
	struct blk_cpl_ctx cpl;

	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	blk_cpl_begin(&cpl);
	blk_request_complete(req, req->drv_status, req->drv_resid);
	blk_cpl_end(&cpl);
}

void blk_request_end_deferred(struct blk_request *req, int status,
			      uint32_t resid_bytes)
{
	BUG_ON(req == NULL);
	if (unlikely(req->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "ending a request with bad magic %08x",
			       req->magic);
		return;
	}
	req->drv_status = status;
	req->drv_resid = resid_bytes;
	req->softirq_entry.next = NULL;
	req->softirq_entry.fn = blk_rq_softirq_end;
	/* This CPU's softirq: the caller is a task on its way to sleep (the
	 * scheduler's plug hook), the end runs at the next interrupt tail or
	 * in kblksoftirqd -- not inside the hook.  Before the softirq
	 * framework runs this ends the request right here. */
	blk_softirq_complete_local(&req->softirq_entry);
}

void blk_flush_own_done(void)
{
	struct blk_cpl_ctx *c;

	if (!blk_can_sleep())
		return;
	c = blk_cpl_collector();
	if (c && c->done_head)
		blk_cpl_flush_done(c);
}

/* No interrupt will come to run the block softirq for this caller: a
 * poller with interrupts off (boot before the scheduler, a waiter of a
 * polled driver) -- its completion is a polled one, finished in the
 * caller.  A hard interrupt handler and the softirq itself run with
 * interrupts off at times as well; they are not pollers. */
static int blk_rq_complete_inline(void)
{
	if (irqs_enabled())
		return 0;
	if (sched_current() == NULL)
		return 1;
	return !blk_in_hardirq() && !blk_in_softirq();
}

/* The queue's rq_affinity as routing flags: 0 none, 1 the submitter's
 * cache domain, 2 the submitter itself. */
static unsigned int blk_rq_affinity_flags(const struct blk_request *req)
{
	const struct blk_queue *q = req->queue;
	uint8_t aff = q ? __atomic_load_n(&q->rq_affinity, __ATOMIC_RELAXED) :
			  1;

	if (aff >= 2)
		return BLK_SOFTIRQ_SAME_COMP | BLK_SOFTIRQ_SAME_FORCE;
	return aff ? BLK_SOFTIRQ_SAME_COMP : 0;
}

void blk_complete_request(struct blk_request *req, int status,
			  uint32_t resid_bytes)
{
	unsigned int flags;

	BUG_ON(req == NULL);
	if (unlikely(req->magic != BLK_REQUEST_MAGIC)) {
		WARN_RATELIMIT(1, "completing a request with bad magic %08x",
			       req->magic);
		return;
	}
	VM_WARN_ON(req->state != BLK_RQ_DISPATCHED);
	req->drv_status = status;
	req->drv_resid = resid_bytes;
	req->softirq_entry.next = NULL;
	req->softirq_entry.fn = blk_rq_softirq_done;
	/* An error handler finishing the commands its recovery ended: the
	 * request waits on its done queue until it let go of its mutex. */
	{
		struct blk_cpl_ctx *c = blk_cpl_collector();

		if (c) {
			if (c->done_tail)
				c->done_tail->next = &req->softirq_entry;
			else
				c->done_head = &req->softirq_entry;
			c->done_tail = &req->softirq_entry;
			return;
		}
	}
	/* Where the softirq part runs follows the queue's rq_affinity: 1
	 * (the default) the submitting CPU's cache domain -- on a machine
	 * whose CPUs do not share their last-level cache that is the
	 * submitter itself, reached with an IPI; 2 the submitting CPU
	 * always; 0 the CPU the interrupt came to. */
	flags = blk_rq_affinity_flags(req);
	/* A hard interrupt never finishes inline: its driver holds its port
	 * lock here, which the softirq part's re-issue paths take.  The
	 * routing either hands the entry to the submitter's softirq (IPI) or
	 * answers "here" -- and "here" is this CPU's softirq raised at the
	 * interrupt's tail, never a call of the entry now.  The queue run a
	 * completion may need is then asked for from that softirq, i.e. on
	 * the CPU that finishes the request, and goes to its kblockd. */
	if (blk_in_hardirq()) {
		if (!blk_softirq_complete_remote(&req->softirq_entry,
						 req->submit_cpu, flags))
			blk_softirq_complete_local(&req->softirq_entry);
		return;
	}
	/* Here and now for a poller, or when deferring gains nothing (a
	 * single CPU serves the queue and it is the submitter). */
	if (blk_rq_complete_inline())
		flags |= BLK_SOFTIRQ_POLLED;
	if (!blk_softirq_complete_remote(&req->softirq_entry, req->submit_cpu,
					 flags))
		blk_rq_softirq_done(&req->softirq_entry);
}

void blk_complete_request_direct(struct blk_request *req, int status,
				 uint32_t resid_bytes)
{
	BUG_ON(req == NULL);
	VM_WARN_ON(req->state != BLK_RQ_DISPATCHED);
	might_sleep();
	req->drv_status = status;
	req->drv_resid = resid_bytes;
	req->softirq_entry.next = NULL;
	req->softirq_entry.fn = blk_rq_softirq_done;
	blk_rq_softirq_done(&req->softirq_entry);
}

/* ==========================================================================
 * Synchronous helpers
 * ========================================================================== */

/* Largest single request the sync helpers build for a device. */
static uint32_t blk_sync_chunk(const block_device_t *bdev)
{
	uint32_t ss = bdev->sector_size ? bdev->sector_size : 512;
	uint32_t chunk = 0;

	if (blk_bdev_is_extended(bdev) && bdev->queue) {
		const struct blk_limits *l = &bdev->queue->limits;
		chunk = l->max_sectors ? l->max_sectors : l->max_hw_sectors;
	} else if (blk_bdev_is_extended(bdev)) {
		chunk = bdev->max_sectors;
	}
	if (!chunk)
		chunk = 256;
	/* At most 1 MiB per request: the bounce buffer of an unaligned
	 * caller is that big, and bigger buys nothing. */
	if ((uint64_t)chunk * ss > 1024 * 1024)
		chunk = (1024 * 1024) / ss;
	if (!chunk)
		chunk = 1;
	return chunk;
}

static uint32_t blk_dma_align_mask(const block_device_t *bdev)
{
	if (blk_bdev_is_extended(bdev) && bdev->queue)
		return bdev->queue->limits.dma_alignment;
	return 0;
}

static int blk_rw_sync(block_device_t *bdev, enum blk_op op, uint64_t lba,
		       uint32_t count, void *buf, uint32_t flags)
{
	uint32_t ss, chunk;
	uint32_t align;
	uint8_t *bounce = NULL;
	uint8_t *p = (uint8_t *)buf;
	int rc = 0;
	int first = 1;

	if (!bdev || !buf)
		return -EINVAL;
	if (count == 0)
		return 0;
	blk_might_sleep();
	ss = bdev->sector_size ? bdev->sector_size : 512;
	align = blk_dma_align_mask(bdev);
	/* One request for the whole range (blk_submit splits it at the
	 * disk's limits); in bounce-buffer sized pieces only when the buffer
	 * must be bounced, or for a device without a queue. */
	if ((((uint64_t)buf) & align) || !blk_bdev_is_extended(bdev))
		chunk = blk_sync_chunk(bdev);
	else
		chunk = (1u << 30) / ss;
	if (((uint64_t)buf) & align) {
		/* An address the controller cannot use: bounce through an
		 * aligned kernel buffer (kalloc returns 8-byte aligned). */
		bounce = (uint8_t *)kalloc((size_t)chunk * ss);
		if (!bounce)
			return -ENOMEM;
	}
	while (count) {
		struct blk_request req;
		uint32_t n = count < chunk ? count : chunk;
		uint8_t *io = bounce ? bounce : p;

		if (bounce && op == BLK_OP_WRITE)
			mm_memcpy(bounce, p, (size_t)n * ss);
		blk_request_init(&req, bdev, op, lba, n);
		req.flags = flags | BLK_REQ_SYNC;
		/* PREFLUSH concerns what was written BEFORE this call: only
		 * the first chunk needs it. */
		if (!first)
			req.flags &= ~BLK_REQ_PREFLUSH;
		rc = blk_request_add_buf(&req, io, n * ss);
		/* One request: no plug of its own (a request larger than one
		 * command is split, and its pieces plugged, by blk_submit). */
		if (!rc)
			rc = blk_execute(&req);
		blk_request_release(&req);
		if (rc)
			break;
		if (bounce && op == BLK_OP_READ)
			mm_memcpy(p, bounce, (size_t)n * ss);
		p += (size_t)n * ss;
		lba += n;
		count -= n;
		first = 0;
	}
	if (bounce)
		kfree(bounce);
	return rc;
}

int blk_read_sync(block_device_t *bdev, uint64_t lba, uint32_t count, void *buf)
{
	return blk_rw_sync(bdev, BLK_OP_READ, lba, count, buf, 0);
}

int blk_read_sync_flags(block_device_t *bdev, uint64_t lba, uint32_t count,
			void *buf, uint32_t flags)
{
	return blk_rw_sync(bdev, BLK_OP_READ, lba, count, buf,
			   flags & ~(BLK_REQ_FUA | BLK_REQ_PREFLUSH));
}

int blk_write_sync(block_device_t *bdev, uint64_t lba, uint32_t count,
		   const void *buf, uint32_t flags)
{
	return blk_rw_sync(bdev, BLK_OP_WRITE, lba, count, (void *)buf, flags);
}

int blk_flush_sync(block_device_t *bdev)
{
	struct blk_request req;
	int rc;

	if (!bdev)
		return -EINVAL;
	blk_might_sleep();
	if (!blk_bdev_is_extended(bdev)) {
		if (bdev->sync)
			return blk_status_to_errno(bdev->sync(bdev));
		return 0;
	}
	if (!bdev->flush_supported)
		return 0;
	blk_request_init(&req, bdev, BLK_OP_FLUSH, 0, 0);
	req.flags = BLK_REQ_SYNC;
	rc = blk_execute(&req);
	blk_request_release(&req);
	return rc;
}

/* FLUSH that runs on a suspended queue (BLK_REQ_PM): what suspend uses
 * for a disk whose driver has no suspend operation of its own. */
static int blk_flush_pm(block_device_t *bdev)
{
	struct blk_request req;
	int rc;

	if (!bdev->flush_supported)
		return 0;
	blk_request_init(&req, bdev, BLK_OP_FLUSH, 0, 0);
	req.flags = BLK_REQ_SYNC | BLK_REQ_PM;
	rc = blk_execute(&req);
	blk_request_release(&req);
	return rc;
}

int blk_discard_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     int secure)
{
	uint64_t max;
	int rc = 0;

	if (!bdev)
		return -EINVAL;
	if (count == 0)
		return 0;
	blk_might_sleep();
	if (!blk_bdev_is_extended(bdev) || !bdev->queue)
		return -EOPNOTSUPP;
	max = bdev->queue->limits.max_discard_sectors;
	if (!max)
		return -EOPNOTSUPP;
	if (max > 0xFFFFFFFFULL)
		max = 0xFFFFFFFFULL;
	if (lba + count < lba || lba + count > bdev->total_sectors)
		return -EINVAL;
	while (count) {
		struct blk_request req;
		uint32_t n = (uint32_t)(count < max ? count : max);

		blk_request_init(&req, bdev,
				 secure ? BLK_OP_SECURE_ERASE : BLK_OP_DISCARD,
				 lba, n);
		req.flags = BLK_REQ_SYNC;
		rc = blk_execute(&req);
		blk_request_release(&req);
		if (rc)
			break;
		lba += n;
		count -= n;
	}
	return rc;
}

/* Zeroing by writing zeroes, for a device without a write-zeroes
 * command (or when it refused). */
static int blk_zeroout_by_writes(block_device_t *bdev, uint64_t lba,
				 uint64_t count)
{
	uint32_t ss = bdev->sector_size ? bdev->sector_size : 512;
	uint32_t chunk = blk_sync_chunk(bdev);
	uint8_t *zero;
	int rc = 0;

	/* 256 KiB of zeroes at most, written over and over. */
	if ((uint64_t)chunk * ss > 256 * 1024)
		chunk = (256 * 1024) / ss;
	if (!chunk)
		chunk = 1;
	zero = (uint8_t *)kcalloc(chunk, ss);
	if (!zero)
		return -ENOMEM;
	while (count) {
		uint32_t n = (uint32_t)(count < chunk ? count : chunk);

		rc = blk_write_sync(bdev, lba, n, zero, 0);
		if (rc)
			break;
		lba += n;
		count -= n;
	}
	kfree(zero);
	return rc;
}

int blk_zeroout_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     uint32_t flags)
{
	uint64_t max = 0;
	int rc = 0;

	if (!bdev)
		return -EINVAL;
	if (count == 0)
		return 0;
	blk_might_sleep();
	if (lba + count < lba || lba + count > bdev->total_sectors)
		return -EINVAL;
	if (blk_bdev_is_extended(bdev) && bdev->queue)
		max = bdev->queue->limits.max_write_zeroes_sectors;
	if (!max)
		return blk_zeroout_by_writes(bdev, lba, count);
	if (max > 0xFFFFFFFFULL)
		max = 0xFFFFFFFFULL;
	while (count) {
		struct blk_request req;
		uint32_t n = (uint32_t)(count < max ? count : max);

		blk_request_init(&req, bdev, BLK_OP_WRITE_ZEROES, lba, n);
		req.flags = BLK_REQ_SYNC | (flags & BLK_REQ_NOUNMAP);
		rc = blk_execute(&req);
		blk_request_release(&req);
		if (rc == -EOPNOTSUPP) {
			/* The device turned the command down after all (a
			 * drive that advertises it but rejects it): write the
			 * zeroes ourselves. */
			return blk_zeroout_by_writes(bdev, lba, count);
		}
		if (rc)
			break;
		lba += n;
		count -= n;
	}
	return rc;
}

/* ==========================================================================
 * The legacy callbacks of extended devices
 * ========================================================================== */

int blk_legacy_read(block_device_t *dev, unsigned long lba,
		    unsigned long count, void *buf)
{
	uint8_t *p = (uint8_t *)buf;

	if (!dev || (!buf && count))
		return ST_INVALID;
	while (count) {
		uint32_t n = count > 0x10000UL ? 0x10000u : (uint32_t)count;
		int rc = blk_read_sync(dev, lba, n, p);

		if (rc)
			return blk_errno_to_status(rc);
		p += (size_t)n * dev->sector_size;
		lba += n;
		count -= n;
	}
	return ST_OK;
}

int blk_legacy_write(block_device_t *dev, unsigned long lba,
		     unsigned long count, const void *buf)
{
	const uint8_t *p = (const uint8_t *)buf;

	if (!dev || (!buf && count))
		return ST_INVALID;
	while (count) {
		uint32_t n = count > 0x10000UL ? 0x10000u : (uint32_t)count;
		int rc = blk_write_sync(dev, lba, n, p, 0);

		if (rc)
			return blk_errno_to_status(rc);
		p += (size_t)n * dev->sector_size;
		lba += n;
		count -= n;
	}
	return ST_OK;
}

int blk_legacy_sync(block_device_t *dev)
{
	if (!dev)
		return ST_INVALID;
	return blk_errno_to_status(blk_flush_sync(dev));
}

/* ==========================================================================
 * References, opens and claims
 * ========================================================================== */

void blk_bdev_get(block_device_t *bdev)
{
	if (!bdev || !blk_bdev_is_extended(bdev))
		return;
	refcount_inc(&bdev->refcount);
	/* The whole disk's lifetime is the gendisk's; a partition holds a
	 * disk reference of its own for as long as it exists. */
	if (!bdev->part && bdev->disk)
		disk_get(bdev->disk);
}

static void blk_bdev_put_common(block_device_t *bdev, int any_context)
{
	if (!bdev || !blk_bdev_is_extended(bdev))
		return;
	if (bdev->part) {
		struct blk_partition *p = bdev->part;

		if (refcount_dec_and_test(&bdev->refcount)) {
			if (!any_context || blk_can_release_now())
				blk_partition_release(p);
			else
				genhd_defer_partition_release(p);
		}
		return;
	}
	/* Whole disk: the embedded bdev keeps a count of its own as a mirror
	 * for diagnostics; the gendisk's decides the lifetime. */
	(void)refcount_dec_and_test(&bdev->refcount);
	if (bdev->disk) {
		if (any_context)
			disk_put_any(bdev->disk);
		else
			disk_put(bdev->disk);
	}
}

void blk_bdev_put(block_device_t *bdev)
{
	blk_might_sleep();
	blk_bdev_put_common(bdev, 0);
}

void blk_bdev_put_any(block_device_t *bdev)
{
	blk_bdev_put_common(bdev, 1);
}

int blk_bdev_open(block_device_t *bdev, int flags)
{
	struct gendisk *disk;
	int rc = 0;

	if (!bdev)
		return -ENODEV;
	if (!blk_bdev_is_extended(bdev))
		return 0;
	blk_might_sleep();
	disk = bdev->disk;
	if (!disk || (bdev->flags & BDEV_F_DEAD) ||
	    (disk->flags & GENHD_FL_DEAD))
		return -ENODEV;
	blk_bdev_get(bdev);
	/* The driver's hook (media checks, door locks) runs without the
	 * registry mutex: it may do I/O. */
	if (disk->ops && disk->ops->open) {
		rc = disk->ops->open(bdev, flags);
		if (rc) {
			blk_bdev_put(bdev);
			return rc;
		}
	}
	blk_mutex_lock(&g_disk_registry_mutex);
	if ((bdev->flags & BDEV_F_DEAD) || (disk->flags & GENHD_FL_DEAD)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		if (disk->ops && disk->ops->release)
			disk->ops->release(bdev);
		blk_bdev_put(bdev);
		return -ENODEV;
	}
	bdev->open_count++;
	blk_mutex_unlock(&g_disk_registry_mutex);
	return 0;
}

void blk_bdev_close(block_device_t *bdev)
{
	struct gendisk *disk;

	if (!bdev || !blk_bdev_is_extended(bdev))
		return;
	blk_might_sleep();
	disk = bdev->disk;
	blk_mutex_lock(&g_disk_registry_mutex);
	if (WARN_ON(bdev->open_count <= 0))
		bdev->open_count = 1;
	bdev->open_count--;
	blk_mutex_unlock(&g_disk_registry_mutex);
	if (disk && disk->ops && disk->ops->release)
		disk->ops->release(bdev);
	blk_bdev_put(bdev);
}

int blk_bdev_claim(block_device_t *bdev, void *holder)
{
	int rc = 0;

	if (!bdev || !holder)
		return -EINVAL;
	if (!blk_bdev_is_extended(bdev))
		return 0;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (bdev->flags & BDEV_F_DEAD) {
		rc = -ENODEV;
	} else if (bdev->holder && bdev->holder != holder) {
		rc = -EBUSY;
	} else if (bdev->part) {
		/* A partition cannot be claimed while someone else holds
		 * the whole disk exclusively. */
		block_device_t *whole = bdev->parent;
		if (whole && whole->holder && whole->holder != holder)
			rc = -EBUSY;
	} else if (bdev->disk) {
		/* And the whole disk not while one of its partitions is
		 * held (mounted) by someone else. */
		struct gendisk *disk = bdev->disk;
		for (int i = 1; i <= DISK_MAX_PARTS && !rc; i++) {
			struct blk_partition *p = disk->part[i];
			if (p && p->bdev.holder && p->bdev.holder != holder)
				rc = -EBUSY;
		}
	}
	if (!rc) {
		bdev->holder = holder;
		bdev->flags |= BDEV_F_EXCL;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

void blk_bdev_unclaim(block_device_t *bdev, void *holder)
{
	if (!bdev || !blk_bdev_is_extended(bdev))
		return;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (WARN(bdev->holder != holder,
		 "%s released by a holder that does not hold it",
		 bdev->name ? bdev->name : "?")) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return;
	}
	bdev->holder = NULL;
	bdev->flags &= ~BDEV_F_EXCL;
	blk_mutex_unlock(&g_disk_registry_mutex);
}

/* ==========================================================================
 * Whole-system power management
 * ========================================================================== */

#define BLK_PM_MAX (DISK_MAX_DISKS + DISK_MAX_SR)

/* Collect the live disks with a reference each, newest first (reverse
 * registration order: a disk that came later may sit on top of an older
 * one, and goes down first).  Registry mutex held. */
static int blk_collect_disks_locked(struct gendisk **list, int cap,
				    int want_suspended)
{
	int n = 0;

	for (struct gendisk *d = genhd_first_locked(); d && n < cap;
	     d = genhd_next_locked(d)) {
		if (!(d->flags & GENHD_FL_UP) || (d->flags & GENHD_FL_DEAD))
			continue;
		if (want_suspended >= 0 &&
		    !!(d->flags & GENHD_FL_SUSPENDED) != want_suspended)
			continue;
		disk_get(d);
		list[n++] = d;
	}
	/* insertion sort by add_ns, newest first */
	for (int i = 1; i < n; i++) {
		struct gendisk *k = list[i];
		int j = i - 1;
		while (j >= 0 && list[j]->add_ns < k->add_ns) {
			list[j + 1] = list[j];
			j--;
		}
		list[j + 1] = k;
	}
	return n;
}

static const char *blk_shutdown_mode_name(enum blk_shutdown_mode mode)
{
	switch (mode) {
	case BLK_SHUTDOWN_REBOOT:
		return "reboot";
	case BLK_SHUTDOWN_POWEROFF:
		return "power-off";
	case BLK_SHUTDOWN_HALT:
		return "halt";
	default:
		return "?";
	}
}

static volatile int g_blk_shutdown_done;
static struct gendisk *g_shutdown_list[BLK_PM_MAX];

void blk_shutdown_all(enum blk_shutdown_mode mode)
{
	int n, locked, errors = 0;

	/* Once per boot: sys_reboot() does it with interrupts on, and the
	 * ACPI power-off path that follows must not do it again (it runs
	 * with interrupts off and the other processors stopped). */
	if (__atomic_exchange_n(&g_blk_shutdown_done, 1, __ATOMIC_SEQ_CST))
		return;
	blk_might_sleep();

	if (blk_can_sleep()) {
		blk_mutex_lock(&g_disk_registry_mutex);
		locked = 1;
	} else {
		/* Interrupts off, other CPUs stopped: a holder of the mutex
		 * will never let go.  Walk without it as a last resort. */
		locked = blk_mutex_trylock(&g_disk_registry_mutex);
		if (!locked)
			kprintf(BLK_LOG_PREFIX
				"shutdown: disk registry busy, walking it unlocked\n");
	}
	n = blk_collect_disks_locked(g_shutdown_list, BLK_PM_MAX, -1);
	if (locked)
		blk_mutex_unlock(&g_disk_registry_mutex);

	if (n)
		kprintf(BLK_LOG_PREFIX "%s: shutting down %d disk%s\n",
			blk_shutdown_mode_name(mode), n, n == 1 ? "" : "s");
	for (int i = 0; i < n; i++) {
		struct gendisk *d = g_shutdown_list[i];
		int rc = 0;

		/* Removed while we were getting here (our reference keeps the
		 * memory): nothing left to flush. */
		if (d->flags & GENHD_FL_DEAD) {
			disk_put_any(d);
			continue;
		}

		kprintf(BLK_LOG_PREFIX "%s: %s\n", d->disk_name,
			mode == BLK_SHUTDOWN_REBOOT ?
				"synchronizing cache" :
				"synchronizing cache and stopping the drive");
		if (d->queue) {
			rc = blk_queue_drain(d->queue, 10000);
			if (rc)
				kprintf(BLK_LOG_PREFIX
					"%s: requests still in flight after 10 s (%d)\n",
					d->disk_name, rc);
		}
		if (d->ops && d->ops->shutdown)
			rc = d->ops->shutdown(d, mode);
		else if (d->bdev.flush_supported)
			rc = blk_flush_sync(&d->bdev);
		else
			rc = 0;
		if (rc) {
			errors++;
			kprintf(BLK_LOG_PREFIX "%s: shutdown failed (%d)\n",
				d->disk_name, rc);
		}
		disk_put_any(d);
	}
	if (n)
		kprintf(BLK_LOG_PREFIX "%d disk%s shut down, %d error%s\n", n,
			n == 1 ? "" : "s", errors, errors == 1 ? "" : "s");
}

int blk_disk_suspend(struct gendisk *disk)
{
	int rc;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	if (disk->flags & GENHD_FL_SUSPENDED)
		return 0;
	if (disk->flags & GENHD_FL_DEAD)
		return -ENODEV;
	blk_might_sleep();
	/* From here on only power-management requests are dispatched; the
	 * ones already at the driver are allowed to finish. */
	blk_queue_suspend(disk->queue);
	rc = blk_queue_quiesce(disk->queue, 30000);
	if (rc) {
		kprintf(BLK_LOG_PREFIX "%s: suspend: in-flight requests did not drain (%d)\n",
			disk->disk_name, rc);
		blk_queue_resume(disk->queue);
		return rc;
	}
	if (disk->ops && disk->ops->suspend)
		rc = disk->ops->suspend(disk);
	else
		rc = blk_flush_pm(&disk->bdev);
	if (rc) {
		kprintf(BLK_LOG_PREFIX "%s: suspend failed (%d)\n",
			disk->disk_name, rc);
		blk_queue_resume(disk->queue);
		return rc;
	}
	blk_mutex_lock(&g_disk_registry_mutex);
	disk->flags |= GENHD_FL_SUSPENDED;
	disk->bdev.flags |= BDEV_F_SUSPENDED;
	for (int i = 1; i <= DISK_MAX_PARTS; i++)
		if (disk->part[i])
			disk->part[i]->bdev.flags |= BDEV_F_SUSPENDED;
	blk_mutex_unlock(&g_disk_registry_mutex);
	kprintf(BLK_LOG_PREFIX "%s: suspended\n", disk->disk_name);
	return 0;
}

int blk_disk_resume(struct gendisk *disk)
{
	int rc = 0;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	if (!(disk->flags & GENHD_FL_SUSPENDED))
		return 0;
	blk_might_sleep();
	if (disk->ops && disk->ops->resume)
		rc = disk->ops->resume(disk);
	/* The queue runs again even when the device did not come back: its
	 * requests then fail at the driver instead of waiting forever. */
	blk_mutex_lock(&g_disk_registry_mutex);
	disk->flags &= ~GENHD_FL_SUSPENDED;
	disk->bdev.flags &= ~BDEV_F_SUSPENDED;
	for (int i = 1; i <= DISK_MAX_PARTS; i++)
		if (disk->part[i])
			disk->part[i]->bdev.flags &= ~BDEV_F_SUSPENDED;
	blk_mutex_unlock(&g_disk_registry_mutex);
	blk_queue_resume(disk->queue);
	if (rc)
		kprintf(BLK_LOG_PREFIX "%s: resume failed (%d)\n",
			disk->disk_name, rc);
	else
		kprintf(BLK_LOG_PREFIX "%s: resumed\n", disk->disk_name);
	return rc;
}

/* Serialises whole-system and per-disk suspend/resume.  Never held
 * together with the registry mutex. */
struct blk_mutex g_blk_pm_mutex = BLK_MUTEX_INIT("blk_pm");
static struct gendisk *g_pm_list[BLK_PM_MAX];

int blk_suspend_all(void)
{
	int n, rc = 0, i;

	blk_might_sleep();
	blk_mutex_lock(&g_blk_pm_mutex);
	/* The list (with a reference per disk) is taken under the registry
	 * mutex; the suspends run without it. */
	blk_mutex_lock(&g_disk_registry_mutex);
	n = blk_collect_disks_locked(g_pm_list, BLK_PM_MAX, 0);
	blk_mutex_unlock(&g_disk_registry_mutex);
	for (i = 0; i < n; i++) {
		rc = blk_disk_suspend(g_pm_list[i]);
		if (rc)
			break;
	}
	if (rc) {
		/* Roll back: everything suspended so far comes back, in the
		 * opposite order. */
		kprintf(BLK_LOG_PREFIX
			"suspend of %s failed (%d): resuming %d disk%s\n",
			g_pm_list[i]->disk_name, rc, i, i == 1 ? "" : "s");
		for (int j = i - 1; j >= 0; j--)
			(void)blk_disk_resume(g_pm_list[j]);
	} else if (n) {
		kprintf(BLK_LOG_PREFIX "%d disk%s suspended\n", n,
			n == 1 ? "" : "s");
	}
	for (int k = 0; k < n; k++)
		disk_put(g_pm_list[k]);
	blk_mutex_unlock(&g_blk_pm_mutex);
	return rc;
}

int blk_resume_all(void)
{
	int n, first_err = 0;

	blk_might_sleep();
	blk_mutex_lock(&g_blk_pm_mutex);
	blk_mutex_lock(&g_disk_registry_mutex);
	n = blk_collect_disks_locked(g_pm_list, BLK_PM_MAX, 1);
	blk_mutex_unlock(&g_disk_registry_mutex);
	/* Oldest first: the reverse of suspend. */
	for (int i = n - 1; i >= 0; i--) {
		int rc = blk_disk_resume(g_pm_list[i]);
		if (rc && !first_err)
			first_err = rc;
	}
	for (int k = 0; k < n; k++)
		disk_put(g_pm_list[k]);
	blk_mutex_unlock(&g_blk_pm_mutex);
	return first_err;
}
