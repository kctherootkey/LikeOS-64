// LikeOS-64 -- requests: one submission each, emitted into a context's ring.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

struct i915_request *i915_request_alloc(struct i915_gem_context *ctx,
					struct i915_engine *e)
{
	struct i915_request *rq = kalloc(sizeof(*rq));
	if (!rq)
		return NULL;
	mm_memset(rq, 0, sizeof(*rq));
	rq->refs = 1; /* the caller's */
	i915_context_get(ctx);
	rq->ctx = ctx;
	rq->engine = e;
	rq->lrc_idx = (int)e->id; /* the legacy map: the engine's own slot */
	return rq;
}

void i915_request_get(struct i915_request *rq)
{
	if (rq)
		__sync_fetch_and_add(&rq->refs, 1);
}

void i915_request_put(struct i915_request *rq)
{
	if (!rq)
		return;
	if (__sync_sub_and_fetch(&rq->refs, 1) != 0)
		return;
	if (rq->objs) {
		for (uint32_t i = 0; i < rq->nobjs; i++)
			if (rq->objs[i])
				drm_gem_put(rq->objs[i]);
		kfree(rq->objs);
	}
	if (rq->fence)
		drm_fence_put(rq->fence);
	if (rq->ctx)
		i915_context_put(rq->ctx);
	kfree(rq);
}

/* ---- the ring ---------------------------------------------------------------- */

static void ring_flush(struct i915_device *i915, struct i915_ring *r,
		       uint32_t from, uint32_t to)
{
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return;
	for (uint32_t off = from & ~63u; off < to; off += 64)
		__asm__ volatile("clflush (%0)" ::"r"(r->vaddr + off) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

/* Room for `bytes' at the tail of the ring.
 *
 * Everything between the head and the tail is work the engine has been
 * given and has not finished; only the rest of the ring may be written.
 * The head moves when a request retires (i915_engine_retire records the
 * ring position each completed request ended at).  Without that, the
 * head stays where the ring was created and the driver writes over
 * commands the engine has not read yet: the batches that follow are
 * whatever was half-overwritten, so some drawing happens, some does
 * not, and the screen fills with work that never ran.
 *
 * A command is never split across the end: when the rest of the ring is
 * too short it is filled with no-ops and the tail goes back to the
 * start, which needs the head to be far enough in for that to be free.
 * The tail is never allowed to meet the head, since the engine reads
 * that as an empty ring. */
static int ring_reserve(struct i915_engine *e, struct i915_lrc *lrc, uint32_t bytes)
{
	struct i915_ring *r = &lrc->ring;
	uint32_t need = (bytes + 7) & ~7u;

	/* Room comes back as the engine retires what is ahead; a client
	 * that has queued more than the ring holds waits for it, for as
	 * long as the engine is making progress (the hang check restarts
	 * the engine if it is not). */
	for (int t = 0; t < 200000; t++) {
		uint32_t head = r->head;
		uint32_t tail = r->tail;

		if (tail >= head) {
			uint32_t to_end = r->size - tail;
			/* room before the end, keeping the ring from looking
			 * empty when the head is at the start */
			if (to_end > need + (head == 0 ? 8u : 0u))
				return 0;
			/* wrap, if the start is free enough */
			if (head > need + 8) {
				uint32_t *p = (uint32_t *)(r->vaddr + tail);
				for (uint32_t i = 0; i < to_end / 4; i++)
					p[i] = MI_NOOP;
				ring_flush(e->i915, r, tail, r->size);
				r->tail = 0;
				return 0;
			}
		} else if (head - tail > need + 8) {
			return 0;
		}
		i915_engine_retire(e);
		lapic_delay_us(50);
	}
	if (!e->ring_full_said) {
		e->ring_full_said = 1;
		kprintf("[drm] i915: %s: the ring stayed full (head %u, tail %u of %u)\n", e->name,
			r->head, r->tail, r->size);
	}
	return -EBUSY;
}

static inline void emit(struct i915_ring *r, uint32_t v)
{
	*(uint32_t *)(r->vaddr + r->tail) = v;
	r->tail += 4;
}

int i915_request_submit(struct i915_request *rq, uint64_t batch_addr,
			uint32_t batch_len)
{
	struct i915_engine *e = rq->engine;
	struct i915_device *i915 = e->i915;
	struct i915_gem_context *ctx = rq->ctx;
	struct i915_lrc *lrc = i915_context_lrc(ctx, e, rq->lrc_idx);
	uint64_t fl;
	(void)batch_len;

	if (!lrc)
		return -ENOMEM;
	rq->lrc = lrc;
	if (ctx->banned)
		return -EIO;
	int render = (e->class == 0);
	/* The register state this context runs with, ahead of its first
	 * work.  Once is enough: the engine saves it with the context and
	 * restores it with the context from then on. */
	uint32_t settings[128];
	uint32_t nset = lrc->settings_done ? 0 : i915_ctx_settings_emit(i915, e, settings, 128);
	if (ring_reserve(e, lrc, 160 + nset * 4) != 0)
		return -EBUSY;
	struct i915_ring *r = &lrc->ring;
	uint32_t start = r->tail;
	uint32_t seqno_addr = e->hwsp_ggtt + I915_HWS_SEQNO_INDEX * 4;

	/* Every batch starts with the engine's caches emptied: the sampler
	 * and state caches keep lines across batches and across contexts,
	 * and a batch that samples a surface the previous batch wrote --
	 * a glyph added to a font's atlas, then drawn -- would otherwise
	 * read the lines it cached before the write.  The translations go
	 * with them; a binding may have changed since the last batch. */
	{
		if (render) {
			/* Gen9 wants an empty PIPE_CONTROL ahead of one that
			 * invalidates the vertex-fetch cache; nothing else
			 * goes in front of the invalidation. */
			emit(r, GFX_OP_PIPE_CONTROL(6));
			emit(r, 0);
			emit(r, 0);
			emit(r, 0);
			emit(r, 0);
			emit(r, 0);
			emit(r, GFX_OP_PIPE_CONTROL(6));
			emit(r, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE |
				  PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
				  PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
				  PIPE_CONTROL_VF_CACHE_INVALIDATE |
				  PIPE_CONTROL_CONST_CACHE_INVALIDATE |
				  PIPE_CONTROL_STATE_CACHE_INVALIDATE |
				  PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB);
			emit(r, e->hwsp_ggtt + I915_HWS_SCRATCH_INDEX * 4);
			emit(r, 0);
			emit(r, 0);
			emit(r, 0);
		} else {
			emit(r, (MI_FLUSH_DW + 1) | MI_INVALIDATE_TLB |
				  (e->class == 2 ? MI_INVALIDATE_BSD : 0));
			emit(r, (e->hwsp_ggtt + I915_HWS_SCRATCH_INDEX * 4) | MI_FLUSH_DW_USE_GTT);
			emit(r, 0);
			emit(r, 0);
		}
	}
	for (uint32_t i = 0; i < nset; i++)
		emit(r, settings[i]);
	if (nset)
		lrc->settings_done = 1;
	/* The batch, in the context's address space.  The address goes in
	 * the way the hardware reads one: 48 bits with bit 47 repeated
	 * through the top of the word. */
	uint64_t batch = (batch_addr & (1ULL << 47)) ?
				 (batch_addr | 0xFFFF000000000000ULL) :
				 (batch_addr & 0x0000FFFFFFFFFFFFULL);
	/* arbitration is allowed while the batch runs and nowhere else in
	 * the ring: the switch points a batch offers are the client's */
	emit(r, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	emit(r, MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT_HSW);
	emit(r, (uint32_t)batch);
	emit(r, (uint32_t)(batch >> 32));
	emit(r, MI_ARB_ON_OFF | MI_ARB_DISABLE);

	/* The breadcrumb: flush, write the sequence, interrupt.  On the
	 * render engine these are two commands, as the part wants them:
	 * first the flush of every cache the batch may have written, with
	 * nothing to write and the streamer stalled until it is done, then
	 * the write of the sequence number.  One command asked to do both
	 * completed after a batch that had drawn nothing and never after
	 * one that had. */
	uint32_t seqno = e->next_seqno++;
	if (render) {
		emit(r, GFX_OP_PIPE_CONTROL(6));
		emit(r, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
			  PIPE_CONTROL_DEPTH_CACHE_FLUSH | PIPE_CONTROL_DC_FLUSH_ENABLE |
			  PIPE_CONTROL_FLUSH_ENABLE);
		emit(r, 0);
		emit(r, 0);
		emit(r, 0);
		emit(r, 0);
		emit(r, GFX_OP_PIPE_CONTROL(6));
		emit(r, PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_CS_STALL |
			  PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_FLUSH_ENABLE);
		emit(r, seqno_addr);
		emit(r, 0);
		emit(r, seqno);
		emit(r, 0);
	} else {
		emit(r, (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW);
		emit(r, seqno_addr | MI_FLUSH_DW_USE_GTT);
		emit(r, 0);
		emit(r, seqno);
	}
	emit(r, MI_USER_INTERRUPT);
	emit(r, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	emit(r, MI_NOOP);
	if (r->tail & 7)
		emit(r, MI_NOOP);
	ring_flush(i915, r, start, r->tail);

	rq->seqno = seqno;
	rq->ring_tail = r->tail;
	rq->fence = drm_fence_create_ctx(&i915->drm, e->fence_context, seqno);
	if (!rq->fence) {
		e->next_seqno--;
		r->tail = start;
		return -ENOMEM;
	}
	/* the engine's own reference, released when the batch retires */
	i915_request_get(rq);
	spin_lock_irqsave(&e->lock, &fl);
	rq->next = NULL;
	if (e->queue_tail)
		e->queue_tail->next = rq;
	else
		e->queue = rq;
	e->queue_tail = rq;
	i915_execlists_submit(e);
	spin_unlock_irqrestore(&e->lock, fl);
	i915_engine_drop_stale(e);
	return 0;
}
