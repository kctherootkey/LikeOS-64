// LikeOS -- ring-buffer submission for the parts before Broadwell.
//
// Each engine has one ring, shared by every context.  A request is
// written straight into it: the caches and translations invalidated, the
// context's address space (Gen6/7: the PPGTT directory reloaded) and
// render state (gen4 on: MI_SET_CONTEXT saves the image of whatever ran
// before and restores this context's) switched to, the batch started,
// and a breadcrumb -- every cache the batch may have written flushed,
// the sequence number stored in the status page, a user interrupt -- and
// then the ring's tail register is moved past it.  Completion is the
// sequence number, read back by the same retirement code the Gen8
// engines use; the requests sit on the same in-flight list.
//
// What the commands look like changes with nearly every generation:
// gen2/3 flush with MI_FLUSH and write the breadcrumb many times over
// (the store can overtake the flush), gen4/5 add PIPE_CONTROL, Sandy
// Bridge needs a post-sync write in front of every flushing PIPE_CONTROL,
// Ivy Bridge a CS stall before a state-cache invalidation, and the
// non-render engines of Gen6/7 flush with MI_FLUSH_DW.  The i830 and 845
// cannot run a batch whose translations changed under the command
// streamer, so their batches are copied into a scratch area first.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The display side, where a GPU reset takes the display down with it
 * (gen2-4 except G4x): its state is saved before and restored after.
 * Optional: the reset goes ahead without them. */
extern void intel_legacy_display_reset_prepare(struct i915_device *i915) __attribute__((weak));
extern void intel_legacy_display_reset_finish(struct i915_device *i915) __attribute__((weak));

/* The ring position a request's commands start at, for replaying it
 * after a reset. */
#define LEG_RQ_HEAD(rq) ((rq)->ring_head)

#define EMIT_INVALIDATE (1u << 0)
#define EMIT_FLUSH (1u << 1)
#define EMIT_BARRIER (EMIT_INVALIDATE | EMIT_FLUSH)

/* Room a request needs at most: invalidation, address-space and context
 * switch, the i830 copy, the breadcrumb, and the context's settings in
 * front of the first batch. */
#define LEG_REQUEST_BYTES 2048
#define LEG_RING_SIZE (32 * 1024)
#define CACHELINE_BYTES 64

/* the GT scratch page's fields */
#define SCRATCH_DEFAULT 0
#define SCRATCH_RENDER_FLUSH 128

/* Contexts a request switched away from: kept until the switch that
 * saves their image has run. */
#define LEG_PENDING 64
struct leg_pending {
	struct i915_gem_context *ctx;
	uint32_t seqno;
};
static struct leg_pending g_pending[I915_NUM_ENGINES][LEG_PENDING];
static int g_npending[I915_NUM_ENGINES];

static inline struct leg_engine *leg_eng(const struct i915_engine *e)
{
	return &g_leg.eng[e->id];
}

static inline struct i915_ring *leg_ring(struct i915_engine *e)
{
	return &leg_eng(e)->rq_lrc.ring;
}

static inline uint32_t scratch_addr(int field)
{
	return g_leg.scratch_ggtt + field;
}

/* ---- the engines these parts have ------------------------------------------------- */

struct leg_engine_desc {
	enum i915_engine_id id;
	const char *name;
	uint8_t class;
	uint32_t mask;
};

static const struct leg_engine_desc leg_engines[] = {
	{ I915_RCS0, "rcs0", 0, I915_ENGINE_RCS0 },
	{ I915_BCS0, "bcs0", 1, I915_ENGINE_BCS0 },
	{ I915_VCS0, "vcs0", 2, I915_ENGINE_VCS0 },
	{ I915_VECS0, "vecs0", 3, I915_ENGINE_VECS0 },
};

static uint32_t leg_engine_mask(struct i915_device *i915)
{
	uint32_t m = I915_ENGINE_RCS0;

	if (g_leg.is_g4x || leg_gen(i915) >= 5)
		m |= I915_ENGINE_VCS0;
	if (leg_gen(i915) >= 6)
		m |= I915_ENGINE_BCS0;
	if (leg_is(i915, I915_PLATFORM_HASWELL))
		m |= I915_ENGINE_VECS0;
	if (i915->info->engine_mask)
		m &= i915->info->engine_mask;
	return m;
}

static uint32_t leg_mmio_base(struct i915_device *i915, enum i915_engine_id id)
{
	switch (id) {
	case I915_RCS0: return RENDER_RING_BASE;
	case I915_BCS0: return BLT_RING_BASE;
	case I915_VCS0: return leg_gen(i915) >= 6 ? GEN6_BSD_RING_BASE : BSD_RING_BASE;
	case I915_VECS0: return VEBOX_RING_BASE;
	default: return 0;
	}
}

static uint32_t leg_irq_mask(struct i915_device *i915, enum i915_engine_id id)
{
	int gen = leg_gen(i915);

	switch (id) {
	case I915_RCS0:
		return gen >= 5 ? GT_RENDER_USER_INTERRUPT : I915_USER_INTERRUPT;
	case I915_VCS0:
		if (gen >= 6)
			return GT_BSD_USER_INTERRUPT;
		return gen == 5 ? ILK_BSD_USER_INTERRUPT : I915_BSD_USER_INTERRUPT;
	case I915_BCS0:
		return GT_BLT_USER_INTERRUPT;
	case I915_VECS0:
		return PM_VEBOX_USER_INTERRUPT;
	default:
		return 0;
	}
}

static uint32_t leg_reset_domain(enum i915_engine_id id)
{
	switch (id) {
	case I915_RCS0: return GEN6_GRDOM_RENDER;
	case I915_BCS0: return GEN6_GRDOM_BLT;
	case I915_VCS0: return GEN6_GRDOM_MEDIA;
	case I915_VECS0: return GEN6_GRDOM_VECS;
	default: return GEN6_GRDOM_FULL;
	}
}

/* The render context image's size; 0 where there is none (gen2/3, and
 * every engine but render). */
static uint32_t leg_context_size(struct i915_device *i915, struct i915_engine *e)
{
	int gen = leg_gen(i915);
	uint32_t cxt;

	if (e->class != 0)
		return 0;
	switch (gen) {
	case 7:
		if (leg_is(i915, I915_PLATFORM_HASWELL))
			return HSW_CXT_TOTAL_SIZE;
		cxt = i915_read32(i915, GEN7_CXT_SIZE);
		return (GEN7_CXT_TOTAL_SIZE(cxt) * 64 + 4095) & ~4095u;
	case 6:
		cxt = i915_read32(i915, CXT_SIZE);
		return (GEN6_CXT_TOTAL_SIZE(cxt) * 64 + 4095) & ~4095u;
	case 5:
	case 4:
		/* the register and the documented layout disagree by a few
		 * cachelines; a page covers both */
		cxt = i915_read32(i915, CXT_SIZE) + 1;
		return (cxt * 64 + 4095) & ~4095u;
	default:
		return 0;
	}
}

/* ---- driver objects ----------------------------------------------------------------- */

static struct drm_gem_object *leg_obj_alloc(struct i915_device *i915, uint64_t size, int contig)
{
	struct drm_gem_object *o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, size);

	if (!o)
		return NULL;
	if ((contig ? drm_gem_alloc_pages_contig(o) : drm_gem_alloc_pages(o)) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	if (i915->drm.drv->gem_init && i915->drm.drv->gem_init(o) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	return o;
}

/* Bound in the global space; the binding is the object's (released with
 * it). */
static int leg_obj_bind(struct i915_device *i915, struct drm_gem_object *o, enum leg_cache level,
			int mappable, uint32_t *ggtt)
{
	struct i915_bo *bo = o->priv;
	int rc = leg_ggtt_bind_level(i915, o, level, mappable, ggtt);

	if (rc)
		return rc;
	bo->ggtt = *ggtt;
	bo->bound = 1;
	bo->ggtt_uncached = level == LEG_CACHE_NONE;
	return 0;
}

static enum leg_cache leg_driver_level(struct i915_device *i915)
{
	return (i915->info->flags & I915_INFO_HAS_LLC) ? LEG_CACHE_LLC : LEG_CACHE_NONE;
}

static void leg_obj_flush(struct i915_device *i915, struct drm_gem_object *o)
{
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return;
	for (uint32_t p = 0; p < o->npages; p++)
		leg_clflush_range(phys_to_virt(o->pages[p]), 4096);
}

/* ---- emission --------------------------------------------------------------------- */

static inline void emit(struct i915_ring *r, uint32_t v)
{
	*(volatile uint32_t *)(r->vaddr + r->tail) = v;
	r->tail += 4;
}

static void ring_flush_cpu(struct i915_device *i915, struct i915_ring *r, uint32_t from,
			   uint32_t to)
{
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return;
	if (to > from)
		leg_clflush_range(r->vaddr + from, to - from);
}

/* "If the Ring Buffer Head Pointer and the Tail Pointer are on the same
 * cacheline, the Head Pointer must not be greater than the Tail
 * Pointer": the space between them keeps a cacheline back. */
static uint32_t ring_space(const struct i915_ring *r)
{
	return (r->head - r->tail - CACHELINE_BYTES) & (r->size - 1);
}

/* Room for `bytes' in one piece; the tail wraps (with no-ops to the
 * end) when the usable end is too close.  Room comes back as requests
 * retire. */
static int ring_reserve(struct i915_engine *e, uint32_t bytes)
{
	struct leg_engine *le = leg_eng(e);
	struct i915_ring *r = leg_ring(e);

	for (int t = 0; t < 200000; t++) {
		uint32_t wrap = r->tail + bytes > le->effective_size ? r->size - r->tail : 0;
		if (ring_space(r) >= bytes + wrap) {
			if (wrap) {
				uint32_t from = r->tail;
				while (r->tail < r->size)
					emit(r, MI_NOOP);
				ring_flush_cpu(e->i915, r, from, r->size);
				r->tail = 0;
			}
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

/* gen2/3: MI_FLUSH, with stores in between to give the flush time. */
static void gen2_emit_flush(struct i915_ring *r, unsigned mode)
{
	uint32_t cmd = MI_FLUSH;

	if (mode & EMIT_INVALIDATE)
		cmd |= MI_READ_FLUSH;
	emit(r, cmd);
	for (int n = 0; n < 12; n++) {
		emit(r, MI_STORE_DWORD_INDEX);
		emit(r, LEG_HWS_SCRATCH_ADDR);
		emit(r, 0);
		emit(r, MI_FLUSH | MI_NO_WRITE_FLUSH);
	}
	emit(r, cmd);
}

/* gen4/5 render: MI_FLUSH; an invalidation also needs a delay before the
 * command streamer sees what was written, which two post-sync writes
 * around a dozen flushes give it. */
static void gen4_emit_flush_rcs(struct i915_device *i915, struct i915_ring *r, unsigned mode)
{
	uint32_t cmd = MI_FLUSH;

	if (mode & EMIT_INVALIDATE) {
		cmd |= MI_EXE_FLUSH;
		if (g_leg.is_g4x || leg_gen(i915) == 5)
			cmd |= MI_INVALIDATE_ISP;
	}
	emit(r, cmd);
	if (mode & EMIT_INVALIDATE) {
		emit(r, GFX_OP_PIPE_CONTROL(4) | PIPE_CONTROL_QW_WRITE);
		emit(r, scratch_addr(SCRATCH_DEFAULT) | PIPE_CONTROL_GLOBAL_GTT);
		emit(r, 0);
		emit(r, 0);
		for (int i = 0; i < 12; i++)
			emit(r, MI_FLUSH);
		emit(r, GFX_OP_PIPE_CONTROL(4) | PIPE_CONTROL_QW_WRITE);
		emit(r, scratch_addr(SCRATCH_DEFAULT) | PIPE_CONTROL_GLOBAL_GTT);
		emit(r, 0);
		emit(r, 0);
	}
	emit(r, cmd);
}

static void gen4_emit_flush_vcs(struct i915_ring *r)
{
	emit(r, MI_FLUSH);
	emit(r, MI_NOOP);
}

/* Sandy Bridge: a PIPE_CONTROL with a non-zero post-sync operation in
 * front of any that flushes -- itself preceded by a CS stall at the
 * pixel scoreboard (the only stall bit that does not need the very
 * workaround it is part of). */
static void gen6_emit_post_sync_nonzero_flush(struct i915_ring *r)
{
	emit(r, GFX_OP_PIPE_CONTROL(5));
	emit(r, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_STALL_AT_SCOREBOARD);
	emit(r, scratch_addr(SCRATCH_RENDER_FLUSH) | PIPE_CONTROL_GLOBAL_GTT);
	emit(r, 0);
	emit(r, 0);
	emit(r, MI_NOOP);

	emit(r, GFX_OP_PIPE_CONTROL(5));
	emit(r, PIPE_CONTROL_QW_WRITE);
	emit(r, scratch_addr(SCRATCH_RENDER_FLUSH) | PIPE_CONTROL_GLOBAL_GTT);
	emit(r, 0);
	emit(r, 0);
	emit(r, MI_NOOP);
}

static void gen6_emit_flush_rcs(struct i915_ring *r, unsigned mode)
{
	uint32_t flags = 0;

	gen6_emit_post_sync_nonzero_flush(r);
	if (mode & EMIT_FLUSH) {
		flags |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		flags |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
		/* what follows (the breadcrumb) waits for the flush */
		flags |= PIPE_CONTROL_CS_STALL;
	}
	if (mode & EMIT_INVALIDATE) {
		flags |= PIPE_CONTROL_TLB_INVALIDATE;
		flags |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		/* a TLB invalidation needs a post-sync write */
		flags |= PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_CS_STALL;
	}
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, flags);
	emit(r, scratch_addr(SCRATCH_RENDER_FLUSH) | PIPE_CONTROL_GLOBAL_GTT);
	emit(r, 0);
}

static void gen7_emit_flush_rcs(struct i915_ring *r, unsigned mode)
{
	/* every fourth PIPE_CONTROL that is not purely a read-cache
	 * invalidation must stall the command streamer: all of them do,
	 * and a stall wants a post-sync write */
	uint32_t flags = PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB;

	if (mode & EMIT_FLUSH) {
		flags |= PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH;
		flags |= PIPE_CONTROL_DEPTH_CACHE_FLUSH;
		flags |= PIPE_CONTROL_DC_FLUSH_ENABLE;
		flags |= PIPE_CONTROL_FLUSH_ENABLE;
	}
	if (mode & EMIT_INVALIDATE) {
		flags |= PIPE_CONTROL_TLB_INVALIDATE;
		flags |= PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_VF_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_CONST_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_STATE_CACHE_INVALIDATE;
		flags |= PIPE_CONTROL_MEDIA_STATE_CLEAR;
		/* a CS stall ahead of a state-cache invalidation */
		emit(r, GFX_OP_PIPE_CONTROL(4));
		emit(r, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_STALL_AT_SCOREBOARD);
		emit(r, 0);
		emit(r, 0);
	}
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, flags);
	emit(r, scratch_addr(SCRATCH_RENDER_FLUSH));
	emit(r, 0);
}

/* The Gen6/7 copy, video and video-enhancement engines: a flush with a
 * store, so the commands after it are ordered behind the flush. */
static void gen6_emit_flush_dw(struct i915_ring *r, uint32_t flags)
{
	emit(r, MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW | flags);
	emit(r, LEG_HWS_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT);
	emit(r, 0);
	emit(r, MI_NOOP);
}

static void leg_emit_flush(struct i915_engine *e, struct i915_ring *r, unsigned mode)
{
	struct i915_device *i915 = e->i915;
	int gen = leg_gen(i915);

	if (e->class == 0) {
		if (gen >= 7)
			gen7_emit_flush_rcs(r, mode);
		else if (gen == 6)
			gen6_emit_flush_rcs(r, mode);
		else if (gen >= 4)
			gen4_emit_flush_rcs(i915, r, mode);
		else
			gen2_emit_flush(r, mode);
		return;
	}
	if (gen < 6) {
		gen4_emit_flush_vcs(r);
		return;
	}
	uint32_t inv = 0;
	if (mode & EMIT_INVALIDATE)
		inv = MI_INVALIDATE_TLB | (e->class == 2 ? MI_INVALIDATE_BSD : 0);
	gen6_emit_flush_dw(r, inv);
}

/* ---- the breadcrumb --------------------------------------------------------------- */

static void gen2_emit_breadcrumb(struct i915_ring *r, uint32_t seqno, int flush, int post)
{
	emit(r, MI_FLUSH);
	while (flush--) {
		emit(r, MI_STORE_DWORD_INDEX);
		emit(r, LEG_HWS_SCRATCH_ADDR);
		emit(r, seqno);
	}
	while (post--) {
		emit(r, MI_STORE_DWORD_INDEX);
		emit(r, LEG_HWS_SEQNO_ADDR);
		emit(r, seqno);
	}
	emit(r, MI_USER_INTERRUPT);
}

static void gen6_emit_breadcrumb_rcs(struct i915_engine *e, struct i915_ring *r, uint32_t seqno)
{
	/* the post-sync workaround first */
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_STALL_AT_SCOREBOARD);
	emit(r, 0);
	emit(r, 0);
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, PIPE_CONTROL_QW_WRITE);
	emit(r, scratch_addr(SCRATCH_DEFAULT) | PIPE_CONTROL_GLOBAL_GTT);
	emit(r, 0);
	/* then the flush, and with it the sequence number */
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
		  PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_CS_STALL);
	emit(r, (e->hwsp_ggtt + LEG_HWS_SEQNO_ADDR) | PIPE_CONTROL_GLOBAL_GTT);
	emit(r, seqno);
	emit(r, MI_USER_INTERRUPT);
	emit(r, MI_NOOP);
}

static void gen7_emit_breadcrumb_rcs(struct i915_engine *e, struct i915_ring *r, uint32_t seqno)
{
	emit(r, GFX_OP_PIPE_CONTROL(4));
	emit(r, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
		  PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE | PIPE_CONTROL_QW_WRITE |
		  PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_CS_STALL);
	emit(r, e->hwsp_ggtt + LEG_HWS_SEQNO_ADDR);
	emit(r, seqno);
	emit(r, MI_USER_INTERRUPT);
	emit(r, MI_NOOP);
}

static void gen6_emit_breadcrumb_xcs(struct i915_ring *r, uint32_t seqno)
{
	emit(r, MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX);
	emit(r, LEG_HWS_SEQNO_ADDR | MI_FLUSH_DW_USE_GTT);
	emit(r, seqno);
	emit(r, MI_USER_INTERRUPT);
}

/* Ivy Bridge and Haswell: the store of the flush may land after the
 * interrupt; the sequence number is stored again, many times, and
 * flushed once more before the interrupt goes. */
#define GEN7_XCS_WA 32
static void gen7_emit_breadcrumb_xcs(struct i915_ring *r, uint32_t seqno)
{
	emit(r, MI_FLUSH_DW | MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX);
	emit(r, LEG_HWS_SEQNO_ADDR | MI_FLUSH_DW_USE_GTT);
	emit(r, seqno);
	for (int i = 0; i < GEN7_XCS_WA; i++) {
		emit(r, MI_STORE_DWORD_INDEX);
		emit(r, LEG_HWS_SEQNO_ADDR);
		emit(r, seqno);
	}
	emit(r, MI_FLUSH_DW);
	emit(r, 0);
	emit(r, 0);
	emit(r, MI_USER_INTERRUPT);
	emit(r, MI_NOOP);
}

static void leg_emit_breadcrumb(struct i915_engine *e, struct i915_ring *r, uint32_t seqno)
{
	int gen = leg_gen(e->i915);

	if (gen >= 6) {
		if (e->class == 0) {
			if (gen == 7)
				gen7_emit_breadcrumb_rcs(e, r, seqno);
			else
				gen6_emit_breadcrumb_rcs(e, r, seqno);
		} else if (gen == 6) {
			gen6_emit_breadcrumb_xcs(r, seqno);
		} else {
			gen7_emit_breadcrumb_xcs(r, seqno);
		}
		return;
	}
	if (gen == 5)
		gen2_emit_breadcrumb(r, seqno, 8, 8);
	else
		gen2_emit_breadcrumb(r, seqno, 16, 8);
}

/* ---- the batch -------------------------------------------------------------------- */

/* The i830/845's command streamer reads a batch through translations it
 * caches and never refetches: the TLB entries it holds are evicted by a
 * blit that touches two pages, and the batch itself is copied into a
 * scratch area whose translations never change, and run from there. */
#define I830_BATCH_LIMIT (256 * 1024)
#define I830_TLB_ENTRIES 2

static int i830_emit_bb_start(struct i915_ring *r, uint32_t offset, uint32_t len, unsigned flags)
{
	uint32_t cs_offset = scratch_addr(SCRATCH_DEFAULT);

	emit(r, COLOR_BLT_CMD | BLT_WRITE_RGBA);
	emit(r, BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | 4096);
	emit(r, I830_TLB_ENTRIES << 16 | 4); /* load each page */
	emit(r, cs_offset);
	emit(r, 0xdeadbeef);
	emit(r, MI_NOOP);
	if (!(flags & I915_LEGACY_DISPATCH_PINNED)) {
		if (len > I830_BATCH_LIMIT)
			return -ENOSPC;
		if (!len)
			len = I830_BATCH_LIMIT;
		emit(r, SRC_COPY_BLT_CMD | BLT_WRITE_RGBA | (6 - 2));
		emit(r, BLT_DEPTH_32 | BLT_ROP_SRC_COPY | 4096);
		emit(r, ((len + 4095) / 4096) << 16 | 4096);
		emit(r, cs_offset);
		emit(r, 4096);
		emit(r, offset);
		emit(r, MI_FLUSH);
		emit(r, MI_NOOP);
		offset = cs_offset;
	}
	if (!(flags & I915_LEGACY_DISPATCH_SECURE))
		offset |= MI_BATCH_NON_SECURE;
	emit(r, MI_BATCH_BUFFER_START | MI_BATCH_GTT);
	emit(r, offset);
	return 0;
}

static int leg_emit_bb_start(struct i915_engine *e, struct i915_ring *r, uint32_t offset,
			     uint32_t len, unsigned flags)
{
	struct i915_device *i915 = e->i915;
	int gen = leg_gen(i915);
	int secure = flags & I915_LEGACY_DISPATCH_SECURE;

	if (gen >= 6) {
		uint32_t security;
		if (e->class == 0 && leg_is(i915, I915_PLATFORM_HASWELL))
			security = secure ? 0 : (MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW);
		else
			security = secure ? 0 : MI_BATCH_NON_SECURE_I965;
		emit(r, MI_BATCH_BUFFER_START | security);
		emit(r, offset);
		return 0;
	}
	if (gen >= 4) {
		emit(r, MI_BATCH_BUFFER_START | MI_BATCH_GTT | (secure ? 0 : MI_BATCH_NON_SECURE_I965));
		emit(r, offset);
		return 0;
	}
	if (leg_is(i915, I915_PLATFORM_I830) || leg_is(i915, I915_PLATFORM_I845G))
		return i830_emit_bb_start(r, offset, len, flags);
	emit(r, MI_BATCH_BUFFER_START | MI_BATCH_GTT);
	emit(r, offset | (secure ? 0 : MI_BATCH_NON_SECURE));
	return 0;
}

/* ---- address space and context switch --------------------------------------------- */

/* The PPGTT directory reloaded (every request: the hardware keeps the
 * directory with the context and the image may hold a stale copy), with
 * the flushes around it that have shown to be needed. */
static void leg_switch_mm(struct i915_engine *e, struct i915_ring *r)
{
	uint32_t base = e->mmio_base;

	if (!g_leg.ppgtt.present)
		return;
	leg_emit_flush(e, r, EMIT_FLUSH);
	emit(r, MI_LOAD_REGISTER_IMM(1));
	emit(r, RING_PP_DIR_DCLV(base));
	emit(r, PP_DIR_DCLV_2G);
	emit(r, MI_LOAD_REGISTER_IMM(1));
	emit(r, RING_PP_DIR_BASE(base));
	emit(r, g_leg.ppgtt.pp_dir);
	/* stall until the directory load is complete */
	emit(r, MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT);
	emit(r, RING_PP_DIR_BASE(base));
	emit(r, scratch_addr(SCRATCH_DEFAULT));
	emit(r, MI_LOAD_REGISTER_IMM(1));
	emit(r, RING_INSTPM(base));
	emit(r, LEG_MASKED_EN(INSTPM_TLB_INVALIDATE));
	leg_emit_flush(e, r, EMIT_FLUSH);
	leg_emit_flush(e, r, EMIT_INVALIDATE);
}

static void leg_mi_set_context(struct i915_engine *e, struct i915_ring *r, uint32_t ctx_ggtt,
			       uint32_t flags)
{
	struct i915_device *i915 = e->i915;
	int gen = leg_gen(i915);
	int num_engines = 0;

	if (leg_is(i915, I915_PLATFORM_HASWELL))
		num_engines = i915->nengines - 1;
	/* WaProgramMiArbOnOffAroundMiSetContext:ivb,vlv,hsw -- and on
	 * Haswell the other engines must not go to sleep across it */
	if (gen == 7) {
		emit(r, MI_ARB_ON_OFF | MI_ARB_DISABLE);
		if (num_engines > 0) {
			emit(r, MI_LOAD_REGISTER_IMM(num_engines));
			for (int i = 0; i < I915_NUM_ENGINES; i++) {
				struct i915_engine *s = &i915->engines[i];
				if (!s->present || s == e)
					continue;
				emit(r, RING_PSMI_CTL(s->mmio_base));
				emit(r, LEG_MASKED_EN(GEN6_PSMI_SLEEP_MSG_DISABLE));
			}
		}
	} else if (gen == 5) {
		/* only listed for early Ironlake steppings and around the
		 * power context; harmless since sync flush is never used */
		emit(r, MI_SUSPEND_FLUSH | MI_SUSPEND_FLUSH_EN);
	}
	emit(r, MI_NOOP);
	emit(r, MI_SET_CONTEXT);
	emit(r, ctx_ggtt | flags);
	/* WaMiSetContext_Hang:snb,ivb,vlv -- always followed by a no-op */
	emit(r, MI_NOOP);
	if (gen == 7) {
		if (num_engines > 0) {
			uint32_t last_reg = 0;
			emit(r, MI_LOAD_REGISTER_IMM(num_engines));
			for (int i = 0; i < I915_NUM_ENGINES; i++) {
				struct i915_engine *s = &i915->engines[i];
				if (!s->present || s == e)
					continue;
				last_reg = RING_PSMI_CTL(s->mmio_base);
				emit(r, last_reg);
				emit(r, LEG_MASKED_DIS(GEN6_PSMI_SLEEP_MSG_DISABLE));
			}
			/* a delay before the next switch */
			emit(r, MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT);
			emit(r, last_reg);
			emit(r, scratch_addr(SCRATCH_DEFAULT));
			emit(r, MI_NOOP);
		}
		emit(r, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	} else if (gen == 5) {
		emit(r, MI_SUSPEND_FLUSH);
	}
}

/* The context's render image on this engine (created on first use, from
 * the default state the first batch left when there is one). */
static struct i915_lrc *leg_context_image(struct i915_gem_context *ctx, struct i915_engine *e,
					  int slot)
{
	struct i915_device *i915 = e->i915;
	struct leg_engine *le = leg_eng(e);
	struct i915_lrc *lrc;
	struct drm_gem_object *o;
	uint32_t ggtt;
	enum leg_cache level;

	if (slot < 0 || slot >= I915_CTX_LRC_SLOTS)
		return NULL;
	lrc = &ctx->lrc[slot];
	if (lrc->obj)
		return lrc;
	o = leg_obj_alloc(i915, le->context_size, 0);
	if (!o)
		return NULL;
	for (uint32_t p = 0; p < o->npages; p++) {
		uint8_t *va = phys_to_virt(o->pages[p]);
		if (g_leg.golden && (uint64_t)(p + 1) * 4096 <= g_leg.golden_size)
			mm_memcpy(va, g_leg.golden + (uint64_t)p * 4096, 4096);
		else
			mm_memset(va, 0, 4096);
	}
	leg_obj_flush(i915, o);
	/* Ivy Bridge caches the image in L3 as well as the LLC; Valleyview
	 * has no L3 control in its entries and must not be made to snoop */
	level = leg_is(i915, I915_PLATFORM_IVYBRIDGE) ? LEG_CACHE_L3_LLC : leg_driver_level(i915);
	if (leg_obj_bind(i915, o, level, 0, &ggtt) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	lrc->obj = o;
	lrc->ggtt = ggtt;
	lrc->engine = e;
	lrc->regs = NULL;
	lrc->initialised = g_leg.golden != NULL;
	return lrc;
}

/* ---- contexts the engine has switched away from ----------------------------------- */

static void pending_release(struct i915_engine *e, int all)
{
	struct i915_gem_context *gone[LEG_PENDING];
	uint32_t passed = i915_hwsp_read(e, LEG_HWS_SEQNO_INDEX);
	int n = 0, k = 0;
	uint64_t fl;

	spin_lock_irqsave(&e->lock, &fl);
	for (int i = 0; i < g_npending[e->id]; i++) {
		struct leg_pending *p = &g_pending[e->id][i];
		if (all || (int32_t)(passed - p->seqno) >= 0)
			gone[n++] = p->ctx;
		else
			g_pending[e->id][k++] = *p;
	}
	g_npending[e->id] = k;
	spin_unlock_irqrestore(&e->lock, fl);
	for (int i = 0; i < n; i++)
		i915_context_put(gone[i]);
}

/* Called with e->lock held: the engine is about to load `ctx' with the
 * request of `seqno'; what it held before is saved by that switch. */
static void track_switch(struct i915_engine *e, struct i915_gem_context *ctx, uint32_t seqno)
{
	struct i915_gem_context *prev = e->resident[0];

	if (prev == ctx)
		return;
	i915_context_get(ctx);
	e->resident[0] = ctx;
	if (!prev)
		return;
	if (g_npending[e->id] < LEG_PENDING) {
		g_pending[e->id][g_npending[e->id]].ctx = prev;
		g_pending[e->id][g_npending[e->id]].seqno = seqno;
		g_npending[e->id]++;
	} else {
		/* full: kept for good rather than freed under the engine */
		kprintf("[drm] i915: %s: too many contexts awaiting their save\n", e->name);
	}
}

void leg_engine_signal(struct i915_engine *e)
{
	i915_engine_retire(e);
	if (g_npending[e->id])
		pending_release(e, 0);
}

void i915_legacy_engine_retire(struct i915_engine *e)
{
	leg_engine_signal(e);
}

/* ---- submission --------------------------------------------------------------------- */

/* Gen6's video engine wants every tail move bracketed: idle reporting off
 * (so the GT leaves RC6 for it), the context id cleared, the engine seen
 * awake, then the tail, then idle reporting back on. */
static void gen6_bsd_set_tail(struct i915_device *i915, struct i915_engine *e, uint32_t tail)
{
	i915_fw_get(i915, I915_FW_ALL);
	i915_write32_fw(i915, RING_PSMI_CTL(GEN6_BSD_RING_BASE),
			LEG_MASKED_EN(GEN6_PSMI_SLEEP_MSG_DISABLE));
	i915_write32_fw(i915, GEN6_BSD_RNCID, 0);
	i915_write32_fw(i915, GEN6_BSD_RNCID + 4, 0);
	int t;
	for (t = 0; t < 1000; t++) {
		if (!(i915_read32_fw(i915, RING_PSMI_CTL(GEN6_BSD_RING_BASE)) &
		      GEN6_BSD_SLEEP_INDICATOR))
			break;
		lapic_delay_us(1);
	}
	if (t == 1000)
		kprintf("[drm] i915: timed out waiting for the video engine to wake up\n");
	i915_write32(i915, RING_TAIL(e->mmio_base), tail);
	i915_write32_fw(i915, RING_PSMI_CTL(GEN6_BSD_RING_BASE),
			LEG_MASKED_DIS(GEN6_PSMI_SLEEP_MSG_DISABLE));
	i915_fw_put(i915, I915_FW_ALL);
}

static void leg_set_tail(struct i915_engine *e, uint32_t tail)
{
	struct i915_device *i915 = e->i915;

	__asm__ volatile("mfence" ::: "memory");
	if (leg_gen(i915) < 6)
		i915_legacy_chipset_flush(i915);
	if (leg_gen(i915) == 6 && e->id == I915_VCS0)
		gen6_bsd_set_tail(i915, e, tail);
	else
		i915_write32(i915, RING_TAIL(e->mmio_base), tail);
}

/* The request's commands into the ring and the request onto the
 * engine.  `pre' is what goes in front of the batch (the default state's
 * settings on the first one). */
static int leg_submit(struct i915_request *rq, uint64_t batch_addr, uint32_t batch_len,
		      unsigned flags, const uint32_t *pre, uint32_t npre)
{
	struct i915_engine *e = rq->engine;
	struct i915_device *i915 = e->i915;
	struct leg_engine *le = leg_eng(e);
	struct i915_gem_context *ctx = rq->ctx;
	struct i915_ring *r = leg_ring(e);
	struct i915_lrc *img = NULL;
	uint32_t seqno, start;
	uint64_t fl;
	int rc;

	if (ctx->banned)
		return -EIO;
	if (batch_addr >> 32)
		return -EINVAL;
	rq->lrc = &le->rq_lrc;
	if (le->context_size) {
		img = leg_context_image(ctx, e, rq->lrc_idx);
		if (!img)
			return -ENOMEM;
	}
	if (g_npending[e->id])
		pending_release(e, 0);
	rc = ring_reserve(e, LEG_REQUEST_BYTES + npre * 4);
	if (rc)
		return rc;
	start = r->tail;
	LEG_RQ_HEAD(rq) = start;

	/* every cache and translation, invalidated */
	leg_emit_flush(e, r, EMIT_INVALIDATE);
	leg_switch_mm(e, r);
	if (img) {
		uint32_t f = MI_SAVE_EXT_STATE_EN | MI_MM_SPACE_GTT;
		f |= img->initialised ? MI_RESTORE_EXT_STATE_EN : MI_RESTORE_INHIBIT;
		leg_mi_set_context(e, r, img->ggtt, f);
	}
	if (npre) {
		/* the settings, with everything flushed around them */
		leg_emit_flush(e, r, EMIT_BARRIER);
		for (uint32_t i = 0; i < npre; i++)
			emit(r, pre[i]);
		leg_emit_flush(e, r, EMIT_BARRIER);
	}
	rc = leg_emit_bb_start(e, r, (uint32_t)batch_addr, batch_len, flags);
	if (rc) {
		r->tail = start;
		return rc;
	}
	seqno = e->next_seqno++;
	leg_emit_breadcrumb(e, r, seqno);
	if (r->tail & 7)
		emit(r, MI_NOOP);
	ring_flush_cpu(i915, r, start, r->tail);

	rq->seqno = seqno;
	rq->ring_tail = r->tail;
	rq->fence = drm_fence_create_ctx(&i915->drm, e->fence_context, seqno);
	if (!rq->fence) {
		e->next_seqno--;
		r->tail = start;
		return -ENOMEM;
	}
	if (img)
		img->initialised = 1;
	/* the engine holds the request too, until it retires */
	i915_request_get(rq);
	spin_lock_irqsave(&e->lock, &fl);
	rq->next = NULL;
	rq->submitted = 1;
	rq->submitted_ns = hrtimer_now_ns();
	if (e->inflight_tail)
		e->inflight_tail->next = rq;
	else
		e->inflight = rq;
	e->inflight_tail = rq;
	if (img)
		track_switch(e, ctx, seqno);
	e->active_ctx = ctx;
	leg_set_tail(e, r->tail);
	spin_unlock_irqrestore(&e->lock, fl);
	return 0;
}

int i915_legacy_request_submit(struct i915_request *rq, uint64_t batch_addr, uint32_t batch_len,
			       unsigned dispatch_flags)
{
	return leg_submit(rq, batch_addr, batch_len, dispatch_flags, NULL, 0);
}

/* ---- engine hardware ------------------------------------------------------------------ */

static void set_hwstam(struct i915_engine *e, uint32_t mask)
{
	struct i915_device *i915 = e->i915;

	/* the render user interrupt stays unmasked: it papers over
	 * interrupts lost across a reset */
	if (e->class == 0) {
		if (leg_gen(i915) >= 6)
			mask &= ~1u;
		else
			mask &= ~(uint32_t)I915_USER_INTERRUPT;
	}
	/* per-engine HWSTAM only from Gen6 on; gen2's is 16 bits */
	if (leg_gen(i915) < 6 && e->class != 0)
		return;
	if (leg_gen(i915) >= 3)
		i915_write32(i915, RING_HWSTAM(e->mmio_base), mask);
	else
		leg_write16(i915, RING_HWSTAM(e->mmio_base), (uint16_t)mask);
}

static void set_hws_pga(struct i915_device *i915, uint64_t phys)
{
	uint32_t addr = (uint32_t)phys;

	if (leg_gen(i915) >= 4)
		addr |= (uint32_t)(phys >> 28) & 0xf0;
	i915_write32(i915, HWS_PGA, addr);
}

static void set_hwsp(struct i915_engine *e, uint32_t offset)
{
	struct i915_device *i915 = e->i915;
	uint32_t reg;

	/* Gen7 moved the status page registers away from the rings' */
	if (leg_gen(i915) == 7) {
		switch (e->id) {
		case I915_BCS0: reg = BLT_HWS_PGA_GEN7; break;
		case I915_VCS0: reg = BSD_HWS_PGA_GEN7; break;
		case I915_VECS0: reg = VEBOX_HWS_PGA_GEN7; break;
		default: reg = RENDER_HWS_PGA_GEN7; break;
		}
	} else if (leg_gen(i915) == 6) {
		reg = RING_HWS_PGA_GEN6(e->mmio_base);
	} else {
		reg = RING_HWS_PGA(e->mmio_base);
	}
	i915_write32(i915, reg, offset);
	(void)i915_read32(i915, reg);
}

/* Gen6/7: the status page's translation is cached by the command
 * streamer; a sync flush with a TLB invalidation drops it (the ring is
 * idle here). */
static void flush_cs_tlb(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	if (leg_gen(i915) < 6)
		return;
	if (!(i915_read32(i915, RING_MI_MODE(base)) & MODE_IDLE))
		i915_dbg("[drm] i915: %s not idle before sync flush\n", e->name);
	i915_write32(i915, RING_INSTPM(base), LEG_MASKED_EN(INSTPM_TLB_INVALIDATE | INSTPM_SYNC_FLUSH));
	(void)i915_wait_reg(i915, RING_INSTPM(base), INSTPM_SYNC_FLUSH, 0, 2000);
}

static int stop_ring(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	/* empty the ring by skipping to the end, then disable it */
	i915_write32(i915, RING_HEAD(base), i915_read32(i915, RING_TAIL(base)));
	(void)i915_read32(i915, RING_HEAD(base));
	i915_write32(i915, RING_CTL(base), 0);
	(void)i915_read32(i915, RING_CTL(base));
	i915_write32(i915, RING_HEAD(base), 0);
	i915_write32(i915, RING_TAIL(base), 0);
	return (i915_read32(i915, RING_HEAD(base)) & HEAD_ADDR) == 0;
}

static void stop_cs(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	if (leg_gen(i915) < 3)
		return;
	i915_write32(i915, RING_MI_MODE(base), LEG_MASKED_EN(STOP_RING));
	if (i915_wait_reg(i915, RING_MI_MODE(base), MODE_IDLE, MODE_IDLE, 100000) != 0 &&
	    (i915_read32(i915, RING_HEAD(base)) & HEAD_ADDR) !=
		    (i915_read32(i915, RING_TAIL(base)) & TAIL_ADDR))
		i915_dbg("[drm] i915: %s: timed out stopping the command streamer\n", e->name);
	(void)i915_read32(i915, RING_MI_MODE(base));
}

/* The ring programmed and started on its current head and tail. */
static int ring_resume(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct leg_engine *le = leg_eng(e);
	struct i915_ring *r = leg_ring(e);
	uint32_t base = e->mmio_base;
	int t;

	if (!stop_ring(e)) {
		/* G45 often needs a second try */
		for (t = 0; t < 20 && !stop_ring(e); t++)
			lapic_delay_us(1000);
	}
	if (le->hws_physical) {
		set_hws_pga(i915, le->hws_phys);
		set_hwstam(e, ~0u);
	} else {
		set_hwsp(e, e->hwsp_ggtt);
		set_hwstam(e, ~0u);
		flush_cs_tlb(e);
	}
	(void)i915_read32(i915, RING_HEAD(base));
	/* the new start must land after the clears above */
	i915_write32(i915, RING_START(base), r->ggtt);
	if (g_leg.ppgtt.present) {
		i915_write32(i915, RING_PP_DIR_DCLV(base), PP_DIR_DCLV_2G);
		i915_write32(i915, RING_PP_DIR_BASE(base), g_leg.ppgtt.pp_dir);
		if (leg_gen(i915) >= 7)
			i915_write32(i915, RING_MODE_GEN7(base), LEG_MASKED_EN(GFX_PPGTT_ENABLE));
	}
	/* an empty ring first; a head the engine refused would feed it
	 * garbage, so the write is repeated until it reads back */
	for (t = 0; t < 50000; t++) {
		i915_write32(i915, RING_HEAD(base), r->head);
		if (i915_read32(i915, RING_HEAD(base)) == r->head)
			break;
		lapic_delay_us(1);
	}
	i915_write32(i915, RING_TAIL(base), r->head);
	if (i915_read32(i915, RING_HEAD(base)) != i915_read32(i915, RING_TAIL(base))) {
		kprintf("[drm] i915: %s: failed to reset an empty ring: head %x tail %x\n", e->name,
			i915_read32(i915, RING_HEAD(base)), i915_read32(i915, RING_TAIL(base)));
		return -EIO;
	}
	i915_write32(i915, RING_CTL(base), RING_CTL_SIZE(r->size) | RING_VALID);
	if (i915_wait_reg(i915, RING_CTL(base), RING_VALID, RING_VALID, 5000) != 0) {
		kprintf("[drm] i915: %s: the ring did not start (ctl %08x head %08x tail %08x start %08x)\n",
			e->name, i915_read32(i915, RING_CTL(base)), i915_read32(i915, RING_HEAD(base)),
			i915_read32(i915, RING_TAIL(base)), i915_read32(i915, RING_START(base)));
		return -EIO;
	}
	if (leg_gen(i915) > 2) {
		i915_write32(i915, RING_MI_MODE(base), LEG_MASKED_DIS(STOP_RING));
		(void)i915_read32(i915, RING_MI_MODE(base));
	}
	/* the engine's own interrupt mask (Gen6 on) */
	if (leg_gen(i915) >= 6) {
		i915_write32(i915, RING_IMR(base), ~le->irq_enable_mask);
		(void)i915_read32(i915, RING_IMR(base));
	}
	if (r->tail != r->head)
		i915_write32(i915, RING_TAIL(base), r->tail);
	le->resumed = 1;
	return 0;
}

static int hwsp_alloc(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct leg_engine *le = leg_eng(e);
	struct drm_gem_object *o = leg_obj_alloc(i915, 4096, 1);

	if (!o)
		return -ENOMEM;
	le->hws_physical = g_leg.hws_physical && e->class == 0;
	if (le->hws_physical) {
		/* addressed physically; the register holds 32 bits (and
		 * from gen4 four more) */
		if (o->pages[0] + 4096 > g_leg.dma_limit) {
			drm_gem_put(o);
			return -ENOMEM;
		}
		le->hws_phys = o->pages[0];
		e->hwsp_ggtt = 0;
	} else {
		uint32_t ggtt;
		/* G33 hangs with the status page above 256 MB, and every
		 * part without an LLC has shown to dislike it high up: it
		 * goes behind the aperture there */
		int mappable = !(i915->info->flags & I915_INFO_HAS_LLC);
		if (leg_obj_bind(i915, o, leg_driver_level(i915), mappable, &ggtt) != 0) {
			drm_gem_put(o);
			return -ENOMEM;
		}
		e->hwsp_ggtt = ggtt;
	}
	e->hwsp_obj = o;
	e->hwsp = phys_to_virt(o->pages[0]);
	mm_memset((void *)e->hwsp, 0, 4096);
	leg_clflush_range((void *)e->hwsp, 4096);
	return 0;
}

static int ring_alloc(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct leg_engine *le = leg_eng(e);
	struct i915_ring *r = &le->rq_lrc.ring;
	struct drm_gem_object *o = leg_obj_alloc(i915, LEG_RING_SIZE, 1);
	uint32_t ggtt;

	if (!o)
		return -ENOMEM;
	if (leg_obj_bind(i915, o, leg_driver_level(i915), 0, &ggtt) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	r->obj = o;
	r->ggtt = ggtt;
	r->size = LEG_RING_SIZE;
	r->head = r->tail = 0;
	r->vaddr = phys_to_virt(o->pages[0]);
	mm_memset(r->vaddr, 0, LEG_RING_SIZE);
	leg_clflush_range(r->vaddr, LEG_RING_SIZE);
	/* the requests' logical ring: the engine's ring, nothing else */
	le->rq_lrc.obj = o;
	le->rq_lrc.ggtt = ggtt;
	le->rq_lrc.engine = e;
	le->effective_size = LEG_RING_SIZE;
	/* an i830/845 hangs with the tail in the ring's last two cachelines */
	if (leg_is(i915, I915_PLATFORM_I830) || leg_is(i915, I915_PLATFORM_I845G))
		le->effective_size -= 2 * CACHELINE_BYTES;
	return 0;
}

static int scratch_alloc(struct i915_device *i915)
{
	uint32_t size = leg_gen(i915) == 2 ? I830_BATCH_LIMIT : 4096;
	struct drm_gem_object *o;

	if (g_leg.scratch_obj)
		return 0;
	o = leg_obj_alloc(i915, size, 0);
	if (!o)
		return -ENOMEM;
	if (leg_obj_bind(i915, o, LEG_CACHE_NONE, 0, &g_leg.scratch_ggtt) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	g_leg.scratch_obj = o;
	return 0;
}

/* The GT as a whole, after power-on, resume or a reset that took it
 * down: clock gating, the settings the GT keeps outside any context,
 * swizzling, the rings nobody uses idled, the PPGTT switched on. */
static void leg_gt_init_hw(struct i915_device *i915)
{
	i915_fw_get(i915, I915_FW_ALL);
	leg_init_clock_gating(i915);
	if (g_leg.has_edram)
		leg_rmw(i915, HSW_IDICR, 0, IDIHASHMSK(0xf));
	if (leg_is(i915, I915_PLATFORM_HASWELL)) {
		int gt = (i915->id && i915->id->gt) ? i915->id->gt : i915->info->gt;
		i915_write32(i915, HSW_MI_PREDICATE_RESULT_2,
			     gt == 3 ? LOWER_SLICE_ENABLED : LOWER_SLICE_DISABLED);
	}
	leg_gt_workarounds(i915);
	leg_init_swizzling(i915);
	leg_init_unused_rings(i915);
	leg_ppgtt_enable(i915);
	i915_fw_put(i915, I915_FW_ALL);
}

static int engine_resume(struct i915_engine *e)
{
	i915_fw_get(e->i915, I915_FW_ALL);
	leg_engine_workarounds(e->i915, e);
	int rc = ring_resume(e);
	i915_fw_put(e->i915, I915_FW_ALL);
	return rc;
}

/* ---- reset ---------------------------------------------------------------------------- */

static int pci_gdrst_wait(struct i915_device *i915, uint8_t mask, uint8_t want)
{
	for (int t = 0; t < 5000; t++) {
		if ((pci_cfg_read8(i915->pci, I915_GDRST) & mask) == want)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* i915/i945/i965: assert for 50 us, wait for the acknowledge, clear. */
static int i915_do_reset(struct i915_device *i915)
{
	int rc;

	pci_cfg_write8(i915->pci, I915_GDRST, GRDOM_RESET_ENABLE);
	lapic_delay_us(50);
	rc = pci_gdrst_wait(i915, GRDOM_RESET_STATUS, GRDOM_RESET_STATUS);
	pci_cfg_write8(i915->pci, I915_GDRST, 0);
	lapic_delay_us(50);
	if (!rc)
		rc = pci_gdrst_wait(i915, GRDOM_RESET_STATUS, 0);
	return rc;
}

static int g33_do_reset(struct i915_device *i915)
{
	pci_cfg_write8(i915->pci, I915_GDRST, GRDOM_RESET_ENABLE);
	return pci_gdrst_wait(i915, GRDOM_RESET_ENABLE, 0);
}

static int g4x_do_reset(struct i915_device *i915)
{
	int rc;

	/* WaVcpClkGateDisableForMediaReset:ctg,elk */
	leg_rmw(i915, VDECCLK_GATE_D, 0, VCP_UNIT_CLOCK_GATE_DISABLE);
	(void)i915_read32(i915, VDECCLK_GATE_D);
	pci_cfg_write8(i915->pci, I915_GDRST, GRDOM_MEDIA | GRDOM_RESET_ENABLE);
	rc = pci_gdrst_wait(i915, GRDOM_RESET_ENABLE, 0);
	if (!rc) {
		pci_cfg_write8(i915->pci, I915_GDRST, GRDOM_RENDER | GRDOM_RESET_ENABLE);
		rc = pci_gdrst_wait(i915, GRDOM_RESET_ENABLE, 0);
	}
	pci_cfg_write8(i915->pci, I915_GDRST, 0);
	leg_rmw(i915, VDECCLK_GATE_D, VCP_UNIT_CLOCK_GATE_DISABLE, 0);
	(void)i915_read32(i915, VDECCLK_GATE_D);
	return rc;
}

static int ilk_do_reset(struct i915_device *i915)
{
	int rc;

	i915_write32(i915, ILK_GDSR, ILK_GRDOM_RENDER | ILK_GRDOM_RESET_ENABLE);
	rc = i915_wait_reg(i915, ILK_GDSR, ILK_GRDOM_RESET_ENABLE, 0, 5000);
	if (!rc) {
		i915_write32(i915, ILK_GDSR, ILK_GRDOM_MEDIA | ILK_GRDOM_RESET_ENABLE);
		rc = i915_wait_reg(i915, ILK_GDSR, ILK_GRDOM_RESET_ENABLE, 0, 5000);
	}
	i915_write32(i915, ILK_GDSR, 0);
	(void)i915_read32(i915, ILK_GDSR);
	return rc;
}

/* Gen6/7: the domains named; requested twice, as the engine state can
 * still be settling after the first acknowledge. */
static int gen6_hw_domain_reset(struct i915_device *i915, uint32_t mask)
{
	int rc = 0;

	for (int loops = 0; loops < 2 && rc == 0; loops++) {
		i915_write32_fw(i915, GEN6_GDRST, mask);
		rc = -ETIMEDOUT;
		for (int t = 0; t < 2000; t++) {
			if (!(i915_read32_fw(i915, GEN6_GDRST) & mask)) {
				rc = 0;
				break;
			}
			lapic_delay_us(1);
		}
	}
	lapic_delay_us(50);
	return rc;
}

/* The hardware reset of whatever the part can reset: one engine (Gen7),
 * or everything.  Returns -ENODEV where there is no reset (gen2). */
static int leg_hw_reset(struct i915_device *i915, uint32_t domains)
{
	int gen = leg_gen(i915);

	if (gen >= 6) {
		int rc;
		i915_fw_get(i915, I915_FW_ALL);
		rc = gen6_hw_domain_reset(i915, domains);
		i915_fw_put(i915, I915_FW_ALL);
		return rc;
	}
	if (gen == 5)
		return ilk_do_reset(i915);
	if (g_leg.is_g4x)
		return g4x_do_reset(i915);
	if (g_leg.is_g33)
		return g33_do_reset(i915);
	if (gen >= 3)
		return i915_do_reset(i915);
	return -ENODEV;
}

/* After a reset: for each engine, the first request that had not
 * completed.  On the engine that hung it is guilty: it fails, and the
 * ring restarts after it.  Elsewhere it is innocent and runs again from
 * its first command. */
static void leg_rewind(struct i915_engine *e, int hung)
{
	struct i915_device *i915 = e->i915;
	struct i915_ring *r = leg_ring(e);
	struct i915_request *guilty = NULL;
	uint64_t fl;

	i915_engine_retire(e);
	spin_lock_irqsave(&e->lock, &fl);
	struct i915_request *first = e->inflight;
	if (first && hung) {
		guilty = first;
		e->inflight = first->next;
		if (!e->inflight)
			e->inflight_tail = NULL;
		first->next = NULL;
		r->head = first->ring_tail;
		/* the status page says it is done, so what follows is
		 * counted from here */
		e->hwsp[LEG_HWS_SEQNO_INDEX] = first->seqno;
		e->last_retired = first->seqno;
	} else if (first) {
		r->head = LEG_RQ_HEAD(first);
		if (first->ctx)
			first->ctx->batch_pending++;
	} else {
		r->head = r->tail;
	}
	/* nothing is loaded after a reset */
	if (e->resident[0]) {
		if (g_npending[e->id] < LEG_PENDING) {
			g_pending[e->id][g_npending[e->id]].ctx = e->resident[0];
			g_pending[e->id][g_npending[e->id]].seqno = e->next_seqno - 1;
			g_npending[e->id]++;
		}
		e->resident[0] = NULL;
	}
	e->active_ctx = NULL;
	spin_unlock_irqrestore(&e->lock, fl);
	leg_clflush_range((void *)&e->hwsp[LEG_HWS_SEQNO_INDEX], 4);
	if (!guilty)
		return;
	if (guilty->ctx) {
		struct i915_gem_context *ctx = guilty->ctx;
		ctx->reset_count++;
		ctx->batch_active++;
		ctx->hang_count++;
		/* a context that asked not to be recovered is done at its
		 * first hang; others get three */
		if (ctx->bannable && (ctx->hang_count >= 3 || !ctx->recoverable))
			ctx->banned = 1;
		/* its saved state is nothing to run on */
		struct i915_lrc *img = &ctx->lrc[guilty->lrc_idx];
		if (guilty->lrc_idx >= 0 && guilty->lrc_idx < I915_CTX_LRC_SLOTS && img->obj)
			img->initialised = 0;
	}
	guilty->guilty = 1;
	guilty->fence->error = -EIO;
	drm_fence_signal(guilty->fence);
	i915_request_put(guilty);
	(void)i915;
}

static void leg_report_hang(struct i915_engine *e, const char *why)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	kprintf("[drm] i915: %s: reset (%s), seqno %u/%u, head %08x tail %08x active head %08x\n",
		e->name, why, i915_hwsp_read(e, LEG_HWS_SEQNO_INDEX), e->next_seqno - 1,
		i915_read32(i915, RING_HEAD(base)), i915_read32(i915, RING_TAIL(base)),
		i915_legacy_engine_acthd(e));
	if (leg_gen(i915) >= 4)
		kprintf("[drm] i915:   instruction %08x (fault %08x), instdone %08x, ps %08x, batch %08x\n",
			i915_read32(i915, RING_IPEHR(base)), i915_read32(i915, RING_IPEIR(base)),
			i915_read32(i915, RING_INSTDONE(base)), i915_read32(i915, RING_INSTPS(base)),
			i915_read32(i915, RING_BBADDR(base)));
	else
		kprintf("[drm] i915:   EIR %08x, PGTBL_ER %08x\n", i915_read32(i915, EIR),
			i915_read32(i915, PGTBL_ER));
}

int i915_legacy_engine_reset(struct i915_engine *e, const char *why)
{
	struct i915_device *i915 = e->i915;
	int engine_only = leg_gen(i915) == 7;
	int clobbers = g_leg.gpu_reset_clobbers_display;
	int rc;

	leg_report_hang(e, why);
	i915->gpu_reset_count++;
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *x = &i915->engines[i];
		if (!x->present || (engine_only && x != e))
			continue;
		stop_cs(x);
		if (!stop_ring(x))
			(void)stop_ring(x);
	}
	if (clobbers && intel_legacy_display_reset_prepare)
		intel_legacy_display_reset_prepare(i915);
	if (clobbers)
		i915_legacy_irq_suspend(i915);
	rc = leg_hw_reset(i915, engine_only ? leg_eng(e)->reset_domain : GEN6_GRDOM_FULL);
	if (rc == -ENODEV)
		kprintf("[drm] i915: %s cannot be reset; the hung batch is skipped\n", i915->info->name);
	else if (rc)
		kprintf("[drm] i915: the reset did not complete (%d)\n", rc);
	if (clobbers)
		i915_legacy_irq_resume(i915);
	if (clobbers && intel_legacy_display_reset_finish)
		intel_legacy_display_reset_finish(i915);
	if (!engine_only) {
		leg_gt_init_hw(i915);
		i915_legacy_fences_restore(i915);
	}
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *x = &i915->engines[i];
		if (!x->present || (engine_only && x != e))
			continue;
		leg_rewind(x, x == e);
		if (engine_resume(x) != 0)
			kprintf("[drm] i915: %s: did not restart after the reset\n", x->name);
		pending_release(x, 0);
		poll_notify_wq(&x->wq);
	}
	return 0;
}

int i915_legacy_gt_reset_all(struct i915_device *i915)
{
	int rc = 0;

	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		stop_cs(e);
		(void)stop_ring(e);
	}
	/* where a reset would take the display with it, the stopped rings
	 * are as far as it goes */
	if (!g_leg.gpu_reset_clobbers_display && leg_gen(i915) >= 3) {
		rc = leg_hw_reset(i915, GEN6_GRDOM_FULL);
		if (rc)
			kprintf("[drm] i915: full GPU reset did not complete\n");
	}
	return rc;
}

uint32_t i915_legacy_engine_acthd(struct i915_engine *e)
{
	if (leg_gen(e->i915) >= 4)
		return i915_read32(e->i915, RING_ACTHD(e->mmio_base));
	return i915_read32(e->i915, ACTHD_I830(RENDER_RING_BASE));
}

/* ---- bring-up -------------------------------------------------------------------------- */

static void engine_teardown(struct i915_engine *e)
{
	struct leg_engine *le = leg_eng(e);

	if (le->resumed) {
		stop_cs(e);
		(void)stop_ring(e);
		set_hwstam(e, ~0u);
	}
	if (e->resident[0]) {
		i915_context_put(e->resident[0]);
		e->resident[0] = NULL;
	}
	pending_release(e, 1);
	if (e->kctx) {
		i915_context_put(e->kctx);
		e->kctx = NULL;
	}
	if (le->rq_lrc.ring.obj) {
		drm_gem_put(le->rq_lrc.ring.obj);
		le->rq_lrc.ring.obj = NULL;
		le->rq_lrc.obj = NULL;
	}
	if (e->hwsp_obj) {
		drm_gem_put(e->hwsp_obj);
		e->hwsp_obj = NULL;
		e->hwsp = NULL;
	}
	le->resumed = 0;
	e->present = 0;
}

int i915_legacy_engines_init(struct i915_device *i915)
{
	uint32_t mask = leg_engine_mask(i915);
	int n = 0;

	mm_memset(i915->engines, 0, sizeof(i915->engines));
	mm_memset(g_leg.eng, 0, sizeof(g_leg.eng));
	mm_memset(g_npending, 0, sizeof(g_npending));
	if (scratch_alloc(i915) != 0) {
		kprintf("[drm] i915: no memory for the GT scratch area\n");
		return -ENOMEM;
	}
	/* a known state: everything the firmware may have left running
	 * reset, where that does not take the display down too */
	(void)i915_legacy_gt_reset_all(i915);
	/* swizzling is switched on to match what the memory controller
	 * says, before anything tiled is rendered */
	leg_detect_swizzle(i915);
	leg_gt_init_hw(i915);

	for (unsigned i = 0; i < sizeof(leg_engines) / sizeof(leg_engines[0]); i++) {
		const struct leg_engine_desc *d = &leg_engines[i];
		if (!(mask & d->mask))
			continue;
		struct i915_engine *e = &i915->engines[d->id];
		struct leg_engine *le = &g_leg.eng[d->id];
		e->i915 = i915;
		e->id = d->id;
		e->name = d->name;
		e->class = d->class;
		e->instance = 0;
		e->present = 1;
		e->mmio_base = leg_mmio_base(i915, d->id);
		e->fw_domain = leg_is(i915, I915_PLATFORM_VALLEYVIEW) && d->class != 0 ?
				       I915_FW_MEDIA :
				       I915_FW_RENDER;
		e->fence_context = 0x1000 + d->id;
		e->next_seqno = 1;
		spinlock_init(&e->lock, "i915_engine");
		wq_head_init(&e->wq, "i915_engine");
		le->irq_enable_mask = leg_irq_mask(i915, d->id);
		le->reset_domain = leg_reset_domain(d->id);
		i915_fw_get(i915, I915_FW_ALL);
		le->context_size = leg_context_size(i915, e);
		i915_fw_put(i915, I915_FW_ALL);
		if (le->context_size > (1u << 20))
			le->context_size = 0;
		e->context_size = le->context_size;
		if (hwsp_alloc(e) != 0 || ring_alloc(e) != 0) {
			kprintf("[drm] i915: %s: no memory for its ring or status page\n", e->name);
			engine_teardown(e);
			continue;
		}
		n++;
	}
	i915->nengines = n;
	if (!n)
		return -ENODEV;
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		if (engine_resume(e) != 0) {
			kprintf("[drm] i915: %s: the ring did not come up\n", e->name);
			engine_teardown(e);
			i915->nengines--;
		}
	}
	if (!i915->nengines)
		return -ENODEV;
	i915_legacy_rps_init(i915);
	kprintf("[drm] i915: %d engines (ring buffers):", i915->nengines);
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present)
			kprintf(" %s", i915->engines[i].name);
	if (g_leg.eng[I915_RCS0].context_size)
		kprintf(", render context %u KB", g_leg.eng[I915_RCS0].context_size / 1024);
	kprintf("\n");
	return 0;
}

int i915_legacy_engines_resume(struct i915_device *i915)
{
	if (!i915->nengines)
		return 0;
	leg_gt_init_hw(i915);
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		struct i915_ring *r;
		uint64_t fl;
		if (!e->present)
			continue;
		r = leg_ring(e);
		/* nothing survives a suspend: the ring starts empty, the
		 * status page says everything issued is done */
		spin_lock_irqsave(&e->lock, &fl);
		r->head = r->tail;
		e->hwsp[LEG_HWS_SEQNO_INDEX] = e->next_seqno - 1;
		if (e->resident[0] && g_npending[e->id] < LEG_PENDING) {
			g_pending[e->id][g_npending[e->id]].ctx = e->resident[0];
			g_pending[e->id][g_npending[e->id]].seqno = e->next_seqno - 1;
			g_npending[e->id]++;
			e->resident[0] = NULL;
		}
		spin_unlock_irqrestore(&e->lock, fl);
		leg_clflush_range((void *)e->hwsp, 4096);
		leg_engine_signal(e);
		(void)engine_resume(e);
	}
	i915_legacy_rps_init(i915);
	return 0;
}

void i915_legacy_engines_fini(struct i915_device *i915)
{
	i915_legacy_rps_fini(i915);
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (e->present)
			engine_teardown(e);
	}
	i915->nengines = 0;
	if (g_leg.golden) {
		kfree(g_leg.golden);
		g_leg.golden = NULL;
		g_leg.golden_size = 0;
	}
	if (g_leg.scratch_obj) {
		drm_gem_put(g_leg.scratch_obj);
		g_leg.scratch_obj = NULL;
	}
}

/* ---- the first batch on each engine ---------------------------------------------------- */

static int wait_request(struct i915_engine *e, struct drm_fence *f, uint32_t ms)
{
	for (uint32_t t = 0; t < ms * 2; t++) {
		leg_engine_signal(e);
		if (f->signaled)
			return f->error ? -EIO : 0;
		lapic_delay_us(500);
	}
	return -ETIMEDOUT;
}

/* One batch through the engine under `ctx', waited for. */
static int run_batch(struct i915_engine *e, struct i915_gem_context *ctx, uint32_t batch,
		     uint32_t len, const uint32_t *pre, uint32_t npre)
{
	struct i915_device *i915 = e->i915;
	struct i915_request *rq = i915_request_alloc(ctx, e);
	struct drm_fence *f;
	int rc;

	if (!rq)
		return -ENOMEM;
	mm_write_lock(&i915->submit_lock);
	rc = leg_submit(rq, batch, len, I915_LEGACY_DISPATCH_SECURE | I915_LEGACY_DISPATCH_PINNED,
			pre, npre);
	mm_write_unlock(&i915->submit_lock);
	if (rc) {
		i915_request_put(rq);
		return rc;
	}
	f = rq->fence;
	drm_fence_get(f);
	i915_request_put(rq);
	rc = wait_request(e, f, 1000);
	drm_fence_put(f);
	return rc;
}

int i915_legacy_gt_golden_init(struct i915_device *i915)
{
	struct drm_gem_object *bo;
	uint32_t *b, ggtt;
	int ok = 0;

	/* the batches run from the global space (dispatched secure) */
	bo = leg_obj_alloc(i915, 4096, 1);
	if (!bo)
		return -ENOMEM;
	if (leg_obj_bind(i915, bo, LEG_CACHE_NONE, 0, &ggtt) != 0) {
		drm_gem_put(bo);
		return -ENOMEM;
	}
	b = phys_to_virt(bo->pages[0]);

	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		struct leg_engine *le = &g_leg.eng[i];
		uint32_t len = 8;
		uint32_t pre[96];
		uint32_t npre = 0;
		int rc;

		if (!e->present)
			continue;
		if (!e->kctx) {
			e->kctx = i915_context_create(i915, NULL, NULL);
			if (!e->kctx)
				continue;
		}
		mm_memset(b, 0, 4096);
		if (e->class == 0 && i915_legacy_renderstate_bytes(leg_gen(i915)) &&
		    i915_legacy_renderstate_build(leg_gen(i915), b, 4096, ggtt) == 0) {
			len = i915_legacy_renderstate_bytes(leg_gen(i915));
		} else {
			b[0] = MI_BATCH_BUFFER_END;
			b[1] = MI_NOOP;
		}
		leg_clflush_range(b, 4096);
		if (e->class == 0)
			npre = leg_ctx_workarounds_emit(i915, e, pre, 96);

		if (!le->context_size) {
			rc = run_batch(e, e->kctx, ggtt, len, pre, npre);
		} else {
			/* The default state: a fresh context loaded without
			 * restoring anything, the settings and the null
			 * state run in it, then the kernel context loaded
			 * so the first one's image is saved. */
			struct i915_gem_context *dflt = i915_context_create(i915, NULL, NULL);
			if (!dflt)
				continue;
			rc = run_batch(e, dflt, ggtt, len, pre, npre);
			if (rc == 0) {
				b[0] = MI_BATCH_BUFFER_END;
				b[1] = MI_NOOP;
				leg_clflush_range(b, 64);
				rc = run_batch(e, e->kctx, ggtt, 8, NULL, 0);
			}
			if (rc == 0 && !g_leg.golden) {
				struct i915_lrc *img = &dflt->lrc[e->id];
				uint8_t *g = kalloc(le->context_size);
				if (g && img->obj) {
					for (uint32_t p = 0; p < img->obj->npages; p++) {
						uint8_t *va = phys_to_virt(img->obj->pages[p]);
						leg_clflush_range(va, 4096);
						mm_memcpy(g + (uint64_t)p * 4096, va, 4096);
					}
					g_leg.golden = g;
					g_leg.golden_size = le->context_size;
					e->golden_ready = 1;
				} else if (g) {
					kfree(g);
				}
			}
			i915_context_put(dflt);
		}
		if (rc == 0) {
			ok++;
			i915_dbg("[drm] i915: %s: first batch ran (seqno %u%s)\n", e->name,
				 i915_hwsp_read(e, LEG_HWS_SEQNO_INDEX),
				 e->golden_ready ? ", default context state saved" : "");
		} else {
			kprintf("[drm] i915: %s: first batch did not complete (%d; seqno %u, head %08x, acthd %08x)\n",
				e->name, rc, i915_hwsp_read(e, LEG_HWS_SEQNO_INDEX),
				i915_read32(i915, RING_HEAD(e->mmio_base)),
				i915_legacy_engine_acthd(e));
			i915_legacy_engine_reset(e, "bring-up batch stuck");
		}
	}
	/* the batch page goes once nothing can still be reading it */
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present)
			(void)i915_engine_idle(&i915->engines[i], 200);
	drm_gem_put(bo);
	return ok ? 0 : -EIO;
}

/* ---- what the interface reports ---------------------------------------------------------- */


static int coherent_ggtt(struct i915_device *i915)
{
	int p = i915->info->platform;

	if (leg_gen(i915) == 2 || p == I915_PLATFORM_I915G || p == I915_PLATFORM_VALLEYVIEW)
		return 0;
	return 1;
}

int i915_legacy_getparam(struct i915_device *i915, int param, int *value)
{
	switch (param) {
	case I915_PARAM_NUM_FENCES_AVAIL:
		*value = i915->num_fences;
		return 1;
	case I915_PARAM_HAS_ALIASING_PPGTT:
		/* the one PPGTT every context shares on Gen6/7, none before */
		*value = g_leg.ppgtt.present ? 1 : 0;
		return 1;
	case I915_PARAM_HAS_LLC:
		*value = !!(i915->info->flags & I915_INFO_HAS_LLC);
		return 1;
	case I915_PARAM_HAS_WT:
		/* write-through needs Haswell's eDRAM */
		*value = g_leg.has_edram;
		return 1;
	case I915_PARAM_HAS_SEMAPHORES:
	case I915_PARAM_HAS_SECURE_BATCHES:
	case I915_PARAM_CMD_PARSER_VERSION:
	case I915_PARAM_HAS_SCHEDULER:
		/* no cross-engine semaphores, no privileged batches, no
		 * command parser, submission in order */
		*value = 0;
		return 1;
	case I915_PARAM_HAS_CONTEXT_ISOLATION:
		*value = g_leg.eng[I915_RCS0].context_size ? 1 : 0;
		return 1;
	case I915_PARAM_MMAP_GTT_COHERENT:
		*value = coherent_ggtt(i915);
		return 1;
	default:
		return 0;
	}
}
