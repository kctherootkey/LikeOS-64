// LikeOS -- execution lists: handing contexts to the hardware and
// reading its status buffer.
//
// One context in the port at a time: the head of the engine's queue (and
// any requests behind it from the same context, which share a ring and
// need only the last tail) is written into its context image and the
// descriptor is written to the submit port.  The context status buffer
// says when the hardware has switched out (idle or complete); then the
// next context goes in.  Completion of the requests themselves is by
// sequence number (i915_engine.c), which does not depend on the status
// buffer -- so a missed status entry costs latency, never a lost fence.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

static inline int csb_write_index(struct i915_engine *e)
{
	return e->i915->info->gen >= 11 ? ICL_HWS_CSB_WRITE_INDEX : I915_HWS_CSB_WRITE_INDEX;
}

int i915_execlists_init_engine(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	int reset = e->csb_entries - 1;

	/* both pointers to the last entry, the mirror emptied */
	i915_write32(i915, RING_CONTEXT_STATUS_PTR(e->mmio_base),
		     (0xffffu << 16) | ((uint32_t)reset << 8) | (uint32_t)reset);
	for (int i = 0; i < 2 * e->csb_entries; i++)
		e->hwsp[I915_HWS_CSB_BUF0_INDEX + i] = 0xffffffffu;
	e->hwsp[csb_write_index(e)] = (uint32_t)reset;
	e->csb_head = reset;
	e->active_ctx = NULL;
	e->active_lrc = NULL;
	e->active_ctx_id = 0;
	e->port_busy = 0;
	return 0;
}

void i915_execlists_reset_engine(struct i915_engine *e)
{
	i915_execlists_init_engine(e);
}

static void lrc_flush(struct i915_device *i915, struct i915_lrc *lrc)
{
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return;
	uint8_t *va = (uint8_t *)lrc->regs;
	for (int i = 0; i < 4096; i += 64)
		__asm__ volatile("clflush (%0)" ::"r"(va + i) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

static void port_write(struct i915_engine *e, uint64_t desc)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	if (i915->info->gen >= 11) {
		i915_write32(i915, RING_EXECLIST_SQ_CONTENTS(base) + 0, (uint32_t)desc);
		i915_write32(i915, RING_EXECLIST_SQ_CONTENTS(base) + 4, (uint32_t)(desc >> 32));
		i915_write32(i915, RING_EXECLIST_SQ_CONTENTS(base) + 8, 0);
		i915_write32(i915, RING_EXECLIST_SQ_CONTENTS(base) + 12, 0);
		i915_write32(i915, RING_EXECLIST_CONTROL(base), EL_CTRL_LOAD);
		(void)i915_read32(i915, RING_EXECLIST_CONTROL(base));
		return;
	}
	/* Gen8-10: port 1 (empty) first, then port 0, high dword before low. */
	i915_write32(i915, RING_ELSP(base), 0);
	i915_write32(i915, RING_ELSP(base), 0);
	i915_write32(i915, RING_ELSP(base), (uint32_t)(desc >> 32));
	i915_write32(i915, RING_ELSP(base), (uint32_t)desc);
	(void)i915_read32(i915, RING_EXECLIST_STATUS_LO(base));
}

/* Caller holds e->lock. */
void i915_execlists_submit(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;

	if (i915->guc.submission) {
		i915_guc_submit(e);
		return;
	}
	if (e->port_busy || !e->queue)
		return;
	struct i915_request *rq = e->queue;
	struct i915_gem_context *ctx = rq->ctx;
	struct i915_lrc *lrc = rq->lrc;
	if (!lrc || !lrc->obj)
		return; /* cannot happen: created at submit time */
	/* every leading request of this logical ring goes in one go */
	uint32_t tail = rq->ring_tail;
	struct i915_request *last = rq;
	while (last->next && last->next->lrc == lrc) {
		last = last->next;
		tail = last->ring_tail;
	}
	/* move [rq..last] from the queue to the in-flight list */
	e->queue = last->next;
	if (!e->queue)
		e->queue_tail = NULL;
	last->next = NULL;
	if (e->inflight_tail)
		e->inflight_tail->next = rq;
	else
		e->inflight = rq;
	e->inflight_tail = last;
	for (struct i915_request *r = rq; r; r = r->next) {
		r->submitted = 1;
		r->submitted_ns = hrtimer_now_ns();
	}
	/* the tail into the image, the descriptor into the port */
	struct i915_lrc_layout l;
	i915_lrc_layout(i915->info->gen_x10, e->class, &l);
	lrc->regs[l.ring_tail + 1] = tail;
	lrc_flush(i915, lrc);
	__asm__ volatile("mfence" ::: "memory");
	e->active_ctx = ctx;
	e->active_lrc = lrc;
	e->active_ctx_id = lrc->hw_id;
	if (e->resident[0] != ctx) {
		struct i915_gem_context *gone = e->resident[1];
		e->resident[1] = e->resident[0];
		e->resident[0] = ctx;
		i915_context_get(ctx);
		if (gone) {
			if (e->nstale < 4)
				e->stale[e->nstale++] = gone;
			else
				i915_context_put(gone);
		}
	}
	e->port_busy = 1;
	port_write(e, i915_lrc_descriptor(i915, lrc, e));
}

/* Put the requests of the context the hardware stopped short back at the
 * head of the queue: the image holds the head it stopped at, and the
 * next submission continues from there.  The last run of in-flight
 * requests is that context's, since a submission moves a whole run.
 * Caller holds e->lock. */
static void requeue_preempted(struct i915_engine *e)
{
	struct i915_request *run = NULL, *prev = NULL;

	for (struct i915_request *r = e->inflight; r; prev = r, r = r->next) {
		if (r->lrc == e->active_lrc && (!prev || prev->lrc != e->active_lrc))
			run = r;
	}
	if (!run)
		return;
	/* detach [run..inflight_tail] */
	struct i915_request *before = NULL;
	for (struct i915_request *r = e->inflight; r && r != run; r = r->next)
		before = r;
	if (before)
		before->next = NULL;
	else
		e->inflight = NULL;
	e->inflight_tail = before;
	/* and put it in front of the queue */
	struct i915_request *last = run;
	while (last->next)
		last = last->next;
	last->next = e->queue;
	e->queue = run;
	if (!e->queue_tail)
		e->queue_tail = last;
	for (struct i915_request *r = run; r != last->next; r = r->next)
		r->submitted = 0;
}

void i915_execlists_process_csb(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint64_t fl;
	int left = 0, preempted = 0;

	spin_lock_irqsave(&e->lock, &fl);
	int write = (int)(e->hwsp[csb_write_index(e)] & 0xff);
	if (write >= e->csb_entries)
		write = e->csb_entries - 1;
	int head = e->csb_head;
	while (head != write) {
		head = (head + 1) % e->csb_entries;
		uint32_t status = e->hwsp[I915_HWS_CSB_BUF0_INDEX + head * 2];
		uint32_t ctxid = e->hwsp[I915_HWS_CSB_BUF0_INDEX + head * 2 + 1];
		if (status == 0xffffffffu)
			break; /* not yet written */
		e->hwsp[I915_HWS_CSB_BUF0_INDEX + head * 2] = 0xffffffffu;
		/* Only the context in the port matters.  A context that went
		 * idle stays resident until the next one is loaded, and its
		 * "complete" arrives while that next one is already running:
		 * taking it for the running context's would free the port
		 * under a batch, and the next submission would stop that
		 * batch short -- with its work half done and its sequence
		 * number then passed by the batch that came after. */
		if (i915->info->gen < 11 && ctxid != e->active_ctx_id)
			continue;
		if (status & GEN8_CTX_STATUS_IDLE_ACTIVE)
			left = 0;
		if (status & GEN8_CTX_STATUS_PREEMPTED)
			preempted = 1;
		if (status & (GEN8_CTX_STATUS_COMPLETE | GEN8_CTX_STATUS_ACTIVE_IDLE |
			      GEN8_CTX_STATUS_PREEMPTED)) {
			/* the context is done, or idle in the port */
			left = 1;
		}
	}
	if (head != e->csb_head) {
		e->csb_head = head;
		i915_write32(i915, RING_CONTEXT_STATUS_PTR(e->mmio_base),
			     (GEN8_CSB_READ_PTR_MASK << 16) | ((uint32_t)head << 8));
	}
	if (left) {
		if (preempted)
			requeue_preempted(e);
		e->port_busy = 0;
		e->active_ctx = NULL;
		e->active_lrc = NULL;
		e->active_ctx_id = 0;
		i915_execlists_submit(e);
	}
	spin_unlock_irqrestore(&e->lock, fl);
	i915_engine_drop_stale(e);
	if (left)
		i915_engine_retire(e);
}

/* ---- the GT worker: hang detection and a safety net for lost interrupts -- */

#define HANGCHECK_NS 1500000000ULL

static void hangcheck_fire(hrtimer_t *t)
{
	struct i915_device *i915 = t->arg;
	i915->hangcheck_pending = 1;
	if (i915->gt_worker_ready)
		poll_notify_wq(&i915->gt_wq);
}

static void gt_worker_pass(struct i915_device *i915)
{
	i915_gt_check_faults(i915);
	i915_guc_ct_process(i915);
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		if (i915->guc.submission) {
			/* what a full transport left queued */
			uint64_t fl;
			spin_lock_irqsave(&e->lock, &fl);
			i915_guc_submit(e);
			spin_unlock_irqrestore(&e->lock, fl);
		} else {
			i915_execlists_process_csb(e);
		}
		i915_engine_retire(e);
		if (!e->inflight) {
			e->hang_strikes = 0;
			continue;
		}
		/* Progress is the sequence number or the active head moving. */
		uint32_t seqno = e->hwsp[I915_HWS_SEQNO_INDEX];
		uint32_t acthd = i915_read32(i915, RING_ACTHD(e->mmio_base));
		if (seqno != e->hang_seqno || acthd != e->hang_acthd) {
			e->hang_seqno = seqno;
			e->hang_acthd = acthd;
			e->hang_strikes = 0;
			continue;
		}
		if (++e->hang_strikes >= 3) {
			e->hang_strikes = 0;
			if (i915->guc.submission)
				/* the GuC's own preemption timeout resets a
				 * stuck context; the host only says so */
				kprintf("[drm] i915: %s: no progress under the GuC (seqno %u/%u)\n",
					e->name, seqno, e->next_seqno - 1);
			else
				i915_engine_reset(e, "no progress");
		}
	}
}

static uint8_t g_gt_stack[16384] __attribute__((aligned(16)));

static void gt_worker(void *arg)
{
	struct i915_device *i915 = arg;
	task_t *cur = sched_current();

	i915->gt_worker_ready = 1;
	hrtimer_init(&i915->hangcheck_timer, hangcheck_fire, i915);
	hrtimer_start_rel(&i915->hangcheck_timer, HANGCHECK_NS);
	for (;;) {
		if (i915->hangcheck_pending) {
			i915->hangcheck_pending = 0;
			gt_worker_pass(i915);
			hrtimer_start_rel(&i915->hangcheck_timer, HANGCHECK_NS);
		}
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&i915->gt_wq, &we);
		if (i915->hangcheck_pending) {
			local_irq_restore(fl);
			wq_remove(&i915->gt_wq, &we);
			continue;
		}
		cur->wait_channel = &i915->gt_wq;
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		cur->wait_channel = NULL;
		wq_remove(&i915->gt_wq, &we);
	}
}

int i915_gt_workers_start(struct i915_device *i915)
{
	if (!i915->gt_ready || i915->gt_worker)
		return 0;
	wq_head_init(&i915->gt_wq, "i915_gt");
	i915->gt_worker = sched_add_task(gt_worker, i915, g_gt_stack, sizeof(g_gt_stack));
	return i915->gt_worker ? 0 : -ENOMEM;
}
