// LikeOS -- requests: one submission each, emitted into a context's ring.
//
// A request is the same three things on every generation: the caches and
// translations emptied, the batch started, and a breadcrumb -- every
// cache the batch may have written flushed, the sequence number written,
// an interrupt raised.  What each of those takes in commands changes from
// generation to generation; Gen12 adds the pre-parser, which runs ahead
// of the engine and must be held back while translations are thrown
// away, and the compression metadata's translation cache (the AUX
// table), which is emptied with the others.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
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

/* ---- Gen12 ------------------------------------------------------------------- */

#define EMIT_INVALIDATE 1u
#define EMIT_FLUSH 2u

static void emit_pipe_control(struct i915_ring *r, uint32_t group0, uint32_t group1,
			      uint32_t addr)
{
	emit(r, GFX_OP_PIPE_CONTROL(6) | group0);
	emit(r, group1);
	emit(r, addr);
	emit(r, 0);
	emit(r, 0);
	emit(r, 0);
}

static int has_rcs_state(const struct i915_engine *e)
{
	return e->class == 0 || e->class == 4;
}

static void emit_aux_inv(struct i915_ring *r, uint32_t reg)
{
	if (!reg)
		return;
	emit(r, MI_LOAD_REGISTER_IMM(1) | MI_LRI_MMIO_REMAP_EN);
	emit(r, reg);
	emit(r, AUX_INV);
	emit(r, MI_SEMAPHORE_WAIT_TOKEN | MI_SEMAPHORE_REGISTER_POLL | MI_SEMAPHORE_POLL |
		MI_SEMAPHORE_SAD_EQ_SDD);
	emit(r, 0);
	emit(r, reg);
	emit(r, 0);
	emit(r, 0);
}

/* Wa_14016712196: Meteor Lake and DG2 want an empty pipe control with a
 * depth flush ahead of every one that flushes or invalidates. */
static void emit_dummy_pipe_control(struct i915_device *i915, struct i915_ring *r, uint32_t addr)
{
	unsigned ip = i915->gt_ip;

	if ((ip >= I915_IP(12, 70) && ip <= I915_IP(12, 74)) ||
	    i915->info->platform == I915_PLATFORM_DG2)
		emit_pipe_control(r, 0, PIPE_CONTROL_DEPTH_CACHE_FLUSH, addr);
}

/* The render and compute engines: pipe controls.  Writes their post-sync
 * data into the scratch dword of the context's own status page. */
static void gen12_flush_rcs(struct i915_engine *e, struct i915_ring *r, unsigned mode)
{
	struct i915_device *i915 = e->i915;
	uint32_t aux = i915_engine_aux_inv_reg(i915, e);
	unsigned ip = i915->gt_ip;

	/* Invalidating the AUX table needs the memory traffic quiet
	 * first: the flush goes ahead of it even when only an
	 * invalidation was asked for. */
	if ((mode & EMIT_FLUSH) || aux) {
		uint32_t g0 = PIPE_CONTROL0_HDC_PIPELINE_FLUSH, g1 = 0;

		emit_dummy_pipe_control(i915, r, LRC_PPHWSP_SCRATCH_ADDR);
		/* Xe2 on: the untyped dataport cache is flushed by name
		 * (only from Xe2 on is that reliable); Meteor Lake's
		 * compression cache flush has no meaning there */
		if (ip >= I915_IP(20, 0))
			g0 |= PIPE_CONTROL0_UNTYPED_DATAPORT_CACHE_FLUSH;
		else if (ip >= I915_IP(12, 70))
			g0 |= PIPE_CONTROL_CCS_FLUSH;
		/* the L3 fabric flush comes with the AUX invalidation; the
		 * explicit one only for a real flush, before Meteor Lake */
		if ((mode & EMIT_FLUSH) && ip < I915_IP(12, 70))
			g1 |= PIPE_CONTROL_FLUSH_L3;
		g1 |= PIPE_CONTROL_TILE_CACHE_FLUSH | PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
		      PIPE_CONTROL_DEPTH_CACHE_FLUSH;
		/* Wa_1409600907: graphics 12.00 to 12.50 (and kept on the
		 * Gen12 parts after them that came before Xe2) */
		if (ip < I915_IP(20, 0))
			g1 |= PIPE_CONTROL_DEPTH_STALL;
		g1 |= PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE;
		g1 |= PIPE_CONTROL_STORE_DATA_INDEX | PIPE_CONTROL_QW_WRITE;
		g1 |= PIPE_CONTROL_CS_STALL;
		if (e->class == 4)
			g1 &= ~PIPE_CONTROL_3D_ENGINE_FLAGS;
		emit_pipe_control(r, g0, g1, LRC_PPHWSP_SCRATCH_ADDR);
	}
	if (mode & EMIT_INVALIDATE) {
		uint32_t f = PIPE_CONTROL_COMMAND_CACHE_INVALIDATE | PIPE_CONTROL_TLB_INVALIDATE |
			     PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
			     PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
			     PIPE_CONTROL_VF_CACHE_INVALIDATE | PIPE_CONTROL_CONST_CACHE_INVALIDATE |
			     PIPE_CONTROL_STATE_CACHE_INVALIDATE | PIPE_CONTROL_STORE_DATA_INDEX |
			     PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_CS_STALL;

		uint32_t f0 = 0;

		emit_dummy_pipe_control(i915, r, LRC_PPHWSP_SCRATCH_ADDR);
		if (e->class == 4)
			f &= ~PIPE_CONTROL_3D_ENGINE_FLAGS;
		/* Xe2 on: the L3's read-only cache goes with the vertex
		 * fetch cache */
		if (ip >= I915_IP(20, 0) && (f & PIPE_CONTROL_VF_CACHE_INVALIDATE))
			f0 |= PIPE_CONTROL0_L3_READ_ONLY_CACHE_INVALIDATE;
		/* the pre-parser must not run past the translation
		 * invalidation and fetch the batch through a stale one */
		emit(r, MI_PREPARSER_DISABLE(1));
		emit_pipe_control(r, f0, f, LRC_PPHWSP_SCRATCH_ADDR);
		emit_aux_inv(r, aux);
		emit(r, MI_PREPARSER_DISABLE(0));
	}
}

/* The other engines: a flush with a dword write into the scratch of the
 * context's status page, which orders what follows behind the flush. */
static void gen12_flush_xcs(struct i915_engine *e, struct i915_ring *r, unsigned mode)
{
	uint32_t aux = i915_engine_aux_inv_reg(e->i915, e);
	uint32_t cmd = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW;

	if (mode & EMIT_INVALIDATE) {
		emit(r, MI_PREPARSER_DISABLE(1));
		cmd |= MI_INVALIDATE_TLB;
		if (e->class == 2)
			cmd |= MI_INVALIDATE_BSD;
		if (aux && e->class == 1)
			cmd |= MI_FLUSH_DW_CCS;
	}
	emit(r, cmd);
	emit(r, LRC_PPHWSP_SCRATCH_ADDR);
	emit(r, 0);
	emit(r, 0);
	emit_aux_inv(r, aux);
	if (mode & EMIT_INVALIDATE)
		emit(r, MI_PREPARSER_DISABLE(0));
}

static void gen12_flush(struct i915_engine *e, struct i915_ring *r, unsigned mode)
{
	if (has_rcs_state(e))
		gen12_flush_rcs(e, r, mode);
	else
		gen12_flush_xcs(e, r, mode);
}

/* The batch, with arbitration allowed while it runs.  From Xe_HP on the
 * predicate a client's batch may leave set is reset around it, by way of
 * the context's indirect context page (i915_lrc_wa_bb_gen12). */
static void gen12_bb_start(struct i915_engine *e, struct i915_lrc *lrc, struct i915_ring *r,
			   uint64_t batch)
{
	struct i915_device *i915 = e->i915;

	emit(r, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	/* Xe2 on: no predication fixup, as the source of those parts has
	 * none (and their images carry no restore batch page for it) */
	if (i915->gt_ip >= I915_IP(12, 55) && i915->gt_ip < I915_IP(20, 0)) {
		struct i915_lrc_layout l;
		i915_lrc_layout(i915->info->gen_x10, e->class, &l);
		uint32_t wa = lrc->ggtt + (uint32_t)l.wa_bb_page * 4096;

		emit(r, MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT | MI_LRI_LRM_CS_MMIO);
		emit(r, RING_PREDICATE_RESULT(0));
		emit(r, wa + 4096 - 8);
		emit(r, 0);
		emit(r, MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT_HSW);
		emit(r, (uint32_t)batch);
		emit(r, (uint32_t)(batch >> 32));
		/* a stray predicate would stop the ring itself: the fixup */
		emit(r, MI_BATCH_BUFFER_START_GEN8);
		emit(r, wa + 2048);
		emit(r, 0);
		emit(r, MI_ARB_ON_OFF | MI_ARB_DISABLE);
		return;
	}
	emit(r, MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_PPGTT_HSW);
	emit(r, (uint32_t)batch);
	emit(r, (uint32_t)(batch >> 32));
	emit(r, MI_ARB_ON_OFF | MI_ARB_DISABLE);
	emit(r, MI_NOOP);
}

/* Wa_14014475959, Wa_16019325821, Wa_14019159160: under the GuC, the
 * render and compute engines of Meteor Lake (and DG2's compute engines)
 * must not be switched away from a context before the GuC says so: the
 * breadcrumb sets a dword of the context's status page and waits for the
 * GuC to clear it. */
static int uses_hold_switchout(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	unsigned ip = i915->gt_ip;

	if (!i915->guc.submission)
		return 0;
	if (e->class == 4 && ((ip == I915_IP(12, 70) && i915->gt_step < I915_STEP_B0) ||
			      i915->info->platform == I915_PLATFORM_DG2))
		return 1;
	return has_rcs_state(e) && ip >= I915_IP(12, 70) && ip <= I915_IP(12, 74);
}

#define HOLD_SWITCHOUT_PPHWSP_OFFSET 0x540

static void gen12_breadcrumb(struct i915_engine *e, struct i915_lrc *lrc, struct i915_ring *r,
			     uint32_t seqno, uint32_t seqno_addr)
{
	struct i915_device *i915 = e->i915;
	unsigned ip = i915->gt_ip;

	if (has_rcs_state(e)) {
		uint32_t f = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE |
			     PIPE_CONTROL_TILE_CACHE_FLUSH | PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
			     PIPE_CONTROL_DEPTH_CACHE_FLUSH | PIPE_CONTROL_DC_FLUSH_ENABLE |
			     PIPE_CONTROL_FLUSH_ENABLE;

		uint32_t f0 = PIPE_CONTROL0_HDC_PIPELINE_FLUSH;

		if (ip < I915_IP(12, 70))
			f |= PIPE_CONTROL_FLUSH_L3;
		emit_dummy_pipe_control(i915, r, 0);
		/* Wa_1409600907 */
		if (ip < I915_IP(12, 55))
			f |= PIPE_CONTROL_DEPTH_STALL;
		if (e->class == 4)
			f &= ~PIPE_CONTROL_3D_ENGINE_FLAGS;
		if (ip >= I915_IP(20, 0))
			f0 |= PIPE_CONTROL0_UNTYPED_DATAPORT_CACHE_FLUSH;
		emit_pipe_control(r, f0, f, 0);
		/* the sequence number, behind the flush */
		emit(r, GFX_OP_PIPE_CONTROL(6));
		emit(r, PIPE_CONTROL_FLUSH_ENABLE | PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE |
			  PIPE_CONTROL_GLOBAL_GTT_IVB);
		emit(r, seqno_addr);
		emit(r, 0);
		emit(r, seqno);
		emit(r, 0);
	} else {
		/* a stalling flush first; the write's own flush does not stall */
		emit(r, MI_FLUSH_DW + 1);
		emit(r, 0);
		emit(r, 0);
		emit(r, 0);
		emit(r, (MI_FLUSH_DW + 1) | MI_FLUSH_DW_OP_STOREDW);
		emit(r, seqno_addr | MI_FLUSH_DW_USE_GTT);
		emit(r, 0);
		emit(r, seqno);
	}
	emit(r, MI_USER_INTERRUPT);
	emit(r, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	if (uses_hold_switchout(e)) {
		uint32_t at = lrc->ggtt + HOLD_SWITCHOUT_PPHWSP_OFFSET;
		emit(r, MI_ATOMIC_INLINE | MI_ATOMIC_GLOBAL_GTT | MI_ATOMIC_CS_STALL |
			  MI_ATOMIC_MOVE);
		emit(r, at);
		emit(r, 0);
		emit(r, 1);
		/* an inline atomic is eleven dwords and a pad */
		for (int i = 0; i < 8; i++)
			emit(r, 0);
		emit(r, MI_SEMAPHORE_WAIT | MI_SEMAPHORE_GLOBAL_GTT | MI_SEMAPHORE_POLL |
			  MI_SEMAPHORE_SAD_EQ_SDD);
		emit(r, 0);
		emit(r, at);
		emit(r, 0);
	}
	/* at least one point a context switch may happen at per request */
	emit(r, MI_ARB_CHECK);
	emit(r, MI_NOOP);
}

int i915_request_submit(struct i915_request *rq, uint64_t batch_addr,
			uint32_t batch_len)
{
	if (i915_is_legacy(rq->engine->i915))
		return i915_legacy_request_submit(rq, batch_addr, batch_len, 0);
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
	uint32_t settings[192];
	uint32_t nset = lrc->settings_done ? 0 : i915_ctx_settings_emit(i915, e, settings, 192);
	int gen12 = i915->info->gen_x10 >= 120;
	if (ring_reserve(e, lrc, (gen12 ? 640 : 160) + nset * 4) != 0)
		return -EBUSY;
	struct i915_ring *r = &lrc->ring;
	uint32_t start = r->tail;
	uint32_t seqno_addr = e->hwsp_ggtt + I915_HWS_SEQNO_INDEX * 4;
	/* The batch, in the context's address space.  The address goes in
	 * the way the hardware reads one: 48 bits with bit 47 repeated
	 * through the top of the word. */
	uint64_t batch = (batch_addr & (1ULL << 47)) ?
				 (batch_addr | 0xFFFF000000000000ULL) :
				 (batch_addr & 0x0000FFFFFFFFFFFFULL);
	uint32_t seqno;

	if (gen12) {
		gen12_flush(e, r, EMIT_INVALIDATE);
		/* the context's settings, with everything flushed and
		 * invalidated behind them */
		if (nset) {
			for (uint32_t i = 0; i < nset; i++)
				emit(r, settings[i]);
			lrc->settings_done = 1;
			gen12_flush(e, r, EMIT_FLUSH | EMIT_INVALIDATE);
		}
		gen12_bb_start(e, lrc, r, batch);
		seqno = e->next_seqno++;
		gen12_breadcrumb(e, lrc, r, seqno, seqno_addr);
		if (r->tail & 7)
			emit(r, MI_NOOP);
		goto queue;
	}

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
	seqno = e->next_seqno++;
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
queue:
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
