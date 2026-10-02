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
// buffer.  The port does: it is freed by a status entry and by nothing
// else, so every entry is taken to mean what the hardware documentation
// says it means (i915_execlists_process_csb), and none is set aside.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/timer.h>
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

	if (i915_is_legacy(i915))
		return; /* the ring takes requests as they are made */
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
			if (e->nstale < (int)(sizeof(e->stale) / sizeof(e->stale[0])))
				e->stale[e->nstale++] = gone;
			else
				i915_context_put(gone);
		}
	}
	e->port_busy = 1;
	port_write(e, i915_lrc_descriptor(i915, lrc, e));
}

/* One entry of the status buffer, both words in one read: the hardware
 * writes them as one, and read as two they can straddle the write.  On a
 * part without a shared last-level cache the processor's copy of the line
 * is dropped first, as i915_hwsp_read() does. */
static uint64_t csb_read(struct i915_engine *e, int index)
{
	const volatile uint64_t *p =
		(const volatile uint64_t *)&e->hwsp[I915_HWS_CSB_BUF0_INDEX + index * 2];
	if (!(e->i915->info->flags & I915_INFO_HAS_LLC))
		__asm__ volatile("clflush (%0)" ::"r"(p) : "memory");
	return *p;
}

/* Does this entry say a context is being handed the engine (as opposed to
 * leaving it)?  The documented reading of the buffer, and the only
 * question asked of an entry.
 *
 * Gen8-11: a context starting (idle to active), or a preemption -- which
 * hands the engine to the context that was just submitted, and which the
 * hardware also reports for a lite restore of the running one.  Everything
 * else is the running context leaving: done, or switched out.
 *
 * Gen12: the entry carries the context switching away and the one switching
 * in.  No valid outgoing context, or a switch to a new queue, is a context
 * being handed the engine; anything else is one leaving. */
static int csb_promotes(struct i915_device *i915, uint64_t entry)
{
	uint32_t lower = (uint32_t)entry;
	uint32_t upper = (uint32_t)(entry >> 32);

	if (i915->info->gen >= 12) {
		int away_valid = ((upper & GEN12_CSB_SW_CTX_ID_MASK) >> GEN12_CSB_SW_CTX_ID_SHIFT) !=
				 GEN12_IDLE_CTX_ID;
		int new_queue = !!(lower & GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE);
		return !away_valid || new_queue;
	}
	return !!(lower & (GEN8_CTX_STATUS_IDLE_ACTIVE | GEN8_CTX_STATUS_PREEMPTED));
}

/* Returns whether the port was freed. */
static int csb_process(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint64_t fl;
	int left = 0;

	spin_lock_irqsave(&e->lock, &fl);
	int write = (int)(i915_hwsp_read(e, csb_write_index(e)) & 0xff);
	if (write >= e->csb_entries)
		write = e->csb_entries - 1;
	int head = e->csb_head;
	while (head != write) {
		head = (head + 1) % e->csb_entries;
		uint64_t entry = csb_read(e, head);
		if ((uint32_t)entry == 0xffffffffu) {
			/* not written yet: looked at again next time */
			head = (head + e->csb_entries - 1) % e->csb_entries;
			break;
		}
		e->hwsp[I915_HWS_CSB_BUF0_INDEX + head * 2] = 0xffffffffu;
		/* Every entry is read as the documentation reads it: the
		 * context in the port is starting, or it has left.  Nothing
		 * else about the entry decides -- not which other status
		 * bits are set, not the context id it names.  The port is
		 * freed here and nowhere else, so an entry set aside for any
		 * reason is a port that stays busy with nothing running in
		 * it: every request queued behind it waits for ever, and no
		 * hang is ever seen, since nothing is in flight. */
		left = csb_promotes(i915, entry) ? 0 : 1;
	}
	if (head != e->csb_head) {
		e->csb_head = head;
		i915_write32(i915, RING_CONTEXT_STATUS_PTR(e->mmio_base),
			     (GEN8_CSB_READ_PTR_MASK << 16) | ((uint32_t)head << 8));
	}
	if (left) {
		e->port_busy = 0;
		e->active_ctx = NULL;
		e->active_lrc = NULL;
		e->active_ctx_id = 0;
		i915_execlists_submit(e);
	}
	spin_unlock_irqrestore(&e->lock, fl);
	return left;
}

void i915_execlists_process_csb(struct i915_engine *e)
{
	if (i915_is_legacy(e->i915))
		return;
	int left = csb_process(e);
	i915_engine_drop_stale(e);
	if (left)
		i915_engine_retire(e);
}

void i915_execlists_process_csb_irq(struct i915_engine *e)
{
	if (i915_is_legacy(e->i915))
		return;
	(void)csb_process(e);
}

/* ---- the GT worker: hang detection and a safety net for lost interrupts -- */

#define HANGCHECK_NS 1500000000ULL
/* The longest the worker sleeps whatever it was told: a wake from
 * another processor that lands between its last look at the work flag
 * and its sleep is lost, and nothing else would wake it. */
#define GT_WORKER_NAP_MS 100
/* How soon the worker looks again while the engines have work. */
#define GT_WORKER_POLL_NS 4000000ULL

static void hangcheck_fire(hrtimer_t *t)
{
	struct i915_device *i915 = t->arg;
	i915->hangcheck_pending = 1;
	if (i915->gt_worker_ready)
		poll_notify_wq(&i915->gt_wq);
}

static void gt_worker_pass(struct i915_device *i915)
{
	/* The engines are judged for progress once per period, however
	 * often the worker runs: it also runs for every completion and
	 * every message of the GuCs, and three passes in a row that come
	 * in a burst say nothing about whether an engine is stuck. */
	uint64_t now = hrtimer_now_ns();
	int judge = now - i915->hangcheck_last_ns >= HANGCHECK_NS - HANGCHECK_NS / 8;

	if (judge) {
		i915->hangcheck_last_ns = now;
		i915_gt_check_faults(i915);
	}
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
		} else if (i915_is_legacy(i915)) {
			i915_legacy_engine_retire(e);
		} else {
			i915_execlists_process_csb(e);
		}
		i915_engine_drop_stale(e);
		i915_engine_retire(e);
		if (judge) {
			/* Requests completed since the last look and not one
			 * user interrupt: completion is being found by polling
			 * alone (every waiter and this worker poll, so nothing
			 * hangs, but every wait is late).  Said a few times. */
			if (e->last_retired != e->watch_retired && e->user_irqs == e->watch_irqs &&
			    e->noirq_said < 3) {
				e->noirq_said++;
				kprintf("[drm] i915: %s: requests up to seqno %u completed without a user interrupt (found by polling); %llu interrupts taken in all, vector %d, last master %08x\n",
					e->name, e->last_retired, (unsigned long long)i915->irq_count,
					i915->irq_vector, i915->irq_last_master);
			}
			e->watch_retired = e->last_retired;
			e->watch_irqs = e->user_irqs;
		}
		if (!e->inflight) {
			e->hang_strikes = 0;
			continue;
		}
		if (!judge)
			continue;
		/* Progress is the sequence number or the active head moving. */
		uint32_t seqno = i915_hwsp_read(e, I915_HWS_SEQNO_INDEX);
		uint32_t acthd = i915_is_legacy(i915) ? i915_legacy_engine_acthd(e) :
							i915_read32(i915, RING_ACTHD(e->mmio_base));
		if (seqno != e->hang_seqno || acthd != e->hang_acthd) {
			e->hang_seqno = seqno;
			e->hang_acthd = acthd;
			e->hang_strikes = 0;
			continue;
		}
		if (++e->hang_strikes >= 3) {
			e->hang_strikes = 0;
			if (i915->guc.submission) {
				/* the GuC resets a context that does not
				 * yield to a preemption: the host asks for one */
				static int said;
				if (said < 16) {
					uint32_t want = 0, id = 0;
					int more;
					uint64_t fl2;
					spin_lock_irqsave(&e->lock, &fl2);
					if (e->inflight) {
						want = e->inflight->seqno;
						id = e->inflight->lrc ? e->inflight->lrc->guc_id : 0;
					}
					more = e->queue != NULL;
					spin_unlock_irqrestore(&e->lock, fl2);
					said++;
					kprintf("[drm] i915: %s: no progress under the GuC (seqno %u/%u, waiting for %u, context id %u, %s queued)\n",
						e->name, seqno, e->next_seqno - 1, want, id,
						more ? "more" : "nothing");
				}
				int hrc = i915_guc_engine_hang(e);
				if (hrc && said < 16)
					kprintf("[drm] i915: %s: the GuC could not be asked to preempt (%d)\n",
						e->name, hrc);
			} else
				i915_engine_reset(e, "no progress");
		}
	}
}

static uint8_t g_gt_stack[16384] __attribute__((aligned(16)));

/* Anything handed to an engine and not yet retired, or waiting to be
 * handed to one? */
static int gt_busy(struct i915_device *i915)
{
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (e->present && (e->inflight || e->queue))
			return 1;
	}
	return 0;
}

static void gt_worker(void *arg)
{
	struct i915_device *i915 = arg;
	task_t *cur = sched_current();

	i915->hangcheck_last_ns = hrtimer_now_ns();
	hrtimer_init(&i915->hangcheck_timer, hangcheck_fire, i915);
	hrtimer_init(&i915->gt_poll_timer, hangcheck_fire, i915);
	hrtimer_start_rel(&i915->hangcheck_timer, HANGCHECK_NS);
	i915->gt_worker_ready = 1;
	for (;;) {
		if (i915->hangcheck_pending) {
			i915->hangcheck_pending = 0;
			gt_worker_pass(i915);
			hrtimer_start_rel(&i915->hangcheck_timer, HANGCHECK_NS);
			/* While the engines have work, the worker looks again
			 * within a few milliseconds whatever happens: a
			 * completion whose interrupt is lost (or never
			 * enabled) is retired -- and, under the GuC, the next
			 * context submitted, which waits for that -- that
			 * soon, not at the next nap's end. */
			if (gt_busy(i915))
				hrtimer_start_rel(&i915->gt_poll_timer, GT_WORKER_POLL_NS);
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
		cur->wakeup_tick = timer_ticks() + timer_ms_to_ticks(GT_WORKER_NAP_MS) + 1;
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(&i915->gt_wq, &we);
		/* woken by the deadline: whatever a lost wake was for, and
		 * the progress check if its period is up */
		if (i915->gt_ready)
			i915->hangcheck_pending = 1;
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
