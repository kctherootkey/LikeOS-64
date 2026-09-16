// LikeOS -- contexts: an address space, an engine map, and a logical
// ring context per engine the context has run on.
//
// The first context an engine ever runs carries a register state the
// driver did not write (restore inhibited: the hardware uses its power-on
// defaults and saves the full state at the first switch-out).  That
// saved image is the template every later context on the engine starts
// from, patched with its own ring and page directory.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_renderstate.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The golden register state per engine: the state pages of the kernel
 * context after its first run.  NULL until captured. */
static uint8_t *g_golden[I915_NUM_ENGINES];
static uint32_t g_golden_pages[I915_NUM_ENGINES];

/* `linear' asks for one physically contiguous run, which only a buffer
 * the processor writes through a single pointer needs (a ring).  A
 * context image is read and written a page at a time, so it takes
 * ordinary scattered pages -- asking for 88 KB in one run would fail on
 * a machine whose memory is merely fragmented. */
struct drm_gem_object *i915_gem_alloc_bound(struct i915_device *i915, uint64_t size,
					    int uncached, int linear, uint32_t *ggtt)
{
	struct drm_gem_object *o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, size);
	if (!o)
		return NULL;
	if ((linear ? drm_gem_alloc_pages_contig(o) : drm_gem_alloc_pages(o)) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	if (i915->drm.drv->gem_init && i915->drm.drv->gem_init(o) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	if (i915_ggtt_bind_obj(i915, o, uncached, ggtt) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	struct i915_bo *bo = o->priv;
	bo->ggtt = *ggtt;
	bo->bound = 1;
	return o;
}

struct i915_gem_context *i915_context_create(struct i915_device *i915,
					     struct drm_file *fp, struct i915_vm *vm)
{
	struct i915_gem_context *ctx = kalloc(sizeof(*ctx));
	if (!ctx)
		return NULL;
	mm_memset(ctx, 0, sizeof(*ctx));
	ctx->refs = 1;
	ctx->i915 = i915;
	ctx->fp = fp;
	if (vm) {
		i915_vm_get(vm);
		ctx->vm = vm;
	} else {
		ctx->vm = i915_vm_create(i915);
		if (!ctx->vm) {
			kfree(ctx);
			return NULL;
		}
	}
	/* hardware ids: 1..2047 fit every generation's descriptor field */
	uint32_t id = __atomic_fetch_add(&i915->next_ctx_hw_id, 1, __ATOMIC_RELAXED);
	ctx->hw_id = (id % 2046) + 1;
	ctx->recoverable = 1;
	ctx->bannable = 1;
	ctx->persistent = 1;
	spinlock_init(&ctx->lock, "i915_ctx");
	/* the default engine map: the legacy ring numbering */
	ctx->engines[0] = i915_engine_by_id(i915, I915_RCS0); /* DEFAULT/RENDER */
	ctx->engines[1] = i915_engine_by_id(i915, I915_RCS0);
	ctx->engines[2] = i915_engine_by_id(i915, I915_VCS0); /* BSD */
	ctx->engines[3] = i915_engine_by_id(i915, I915_BCS0); /* BLT */
	ctx->engines[4] = i915_engine_by_id(i915, I915_VECS0); /* VEBOX */
	ctx->nengines = 5;
	ctx->explicit_engines = 0;
	return ctx;
}

void i915_context_get(struct i915_gem_context *ctx)
{
	__atomic_fetch_add(&ctx->refs, 1, __ATOMIC_ACQ_REL);
}

static void lrc_free(struct i915_device *i915, struct i915_lrc *lrc)
{
	i915_guc_lrc_release(i915, lrc);
	if (lrc->ring.obj) {
		drm_gem_put(lrc->ring.obj);
		lrc->ring.obj = NULL;
	}
	if (lrc->obj) {
		drm_gem_put(lrc->obj);
		lrc->obj = NULL;
	}
	lrc->regs = NULL;
}

void i915_context_put(struct i915_gem_context *ctx)
{
	if (!ctx)
		return;
	if (__atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	for (int i = 0; i < I915_CTX_LRC_SLOTS; i++)
		lrc_free(ctx->i915, &ctx->lrc[i]);
	i915_vm_put(ctx->vm);
	kfree(ctx);
}

/* The power and clock state every render context asks for: all the
 * slices, subslices and execution units the fuses enable. */
uint32_t i915_rpcs_for(struct i915_device *i915)
{
	unsigned slices = 0, subslices = 0;

	for (int s = 0; s < 4; s++) {
		if (!(i915->slice_mask & (1u << s)))
			continue;
		slices++;
		for (uint32_t m = i915->subslice_mask[s]; m; m >>= 1)
			subslices += m & 1;
	}
	unsigned eus = subslices ? i915->eu_total / subslices : 0;
	return i915_lrc_rpcs(i915->info->gen, !!(i915->info->flags & I915_INFO_IS_LP),
			     slices, subslices, eus);
}

/* Record the golden state from a context image that has run once. */
void i915_context_capture_golden(struct i915_gem_context *ctx, struct i915_engine *e)
{
	struct i915_lrc *lrc = &ctx->lrc[e->id];
	if (!lrc->obj || g_golden[e->id])
		return;
	struct i915_lrc_layout l;
	if (i915_lrc_layout(e->i915->info->gen_x10, e->class, &l) != 0)
		return;
	uint32_t pages = l.pages - 1; /* the state pages, not the ppHWSP */
	uint8_t *g = kalloc(pages * 4096);
	if (!g)
		return;
	for (uint32_t p = 0; p < pages; p++)
		mm_memcpy(g + p * 4096, phys_to_virt(lrc->obj->pages[1 + p]), 4096);
	g_golden[e->id] = g;
	g_golden_pages[e->id] = pages;
	e->golden_ready = 1;
}

struct i915_lrc *i915_context_lrc(struct i915_gem_context *ctx, struct i915_engine *e, int idx)
{
	struct i915_device *i915 = ctx->i915;
	struct i915_lrc_layout l;

	if (idx < 0 || idx >= I915_CTX_LRC_SLOTS)
		return NULL;
	struct i915_lrc *lrc = &ctx->lrc[idx];
	if (lrc->obj)
		return lrc;
	/* its own hardware id: the status buffer names the logical ring
	 * that switched, and two slots of one context are two of them */
	uint32_t id = __atomic_fetch_add(&i915->next_ctx_hw_id, 1, __ATOMIC_RELAXED);
	lrc->hw_id = (id % 2046) + 1;
	lrc->engine = e;
	if (i915_lrc_layout(i915->info->gen_x10, e->class, &l) != 0)
		return NULL;
	uint32_t ring_ggtt, img_ggtt;
	lrc->ring.obj = i915_gem_alloc_bound(i915, I915_RING_SIZE, 0, 1, &ring_ggtt);
	if (!lrc->ring.obj) {
		kprintf("[drm] i915: %s: no memory for a %u KB ring\n", e->name,
			I915_RING_SIZE / 1024);
		return NULL;
	}
	lrc->ring.ggtt = ring_ggtt;
	lrc->ring.size = I915_RING_SIZE;
	lrc->ring.head = lrc->ring.tail = 0;
	lrc->ring.vaddr = phys_to_virt(lrc->ring.obj->pages[0]);
	mm_memset(lrc->ring.vaddr, 0, I915_RING_SIZE);
	lrc->obj = i915_gem_alloc_bound(i915, (uint64_t)l.pages * 4096, 0, 0, &img_ggtt);
	if (!lrc->obj) {
		kprintf("[drm] i915: %s: no memory for a %u page context image\n",
			e->name, l.pages);
		lrc_free(i915, lrc);
		return NULL;
	}
	lrc->ggtt = img_ggtt;
	/* the whole image starts clear: the status page the engine keeps
	 * its own bookkeeping in, and every state page the golden copy
	 * does not cover */
	for (uint32_t p = 0; p < l.pages; p++)
		mm_memset(phys_to_virt(lrc->obj->pages[p]), 0, 4096);
	/* the image starts from the golden state when there is one */
	int inhibit = 1;
	if (g_golden[e->id]) {
		for (uint32_t p = 0; p < g_golden_pages[e->id] && p + 1 < l.pages; p++)
			mm_memcpy(phys_to_virt(lrc->obj->pages[1 + p]),
				  g_golden[e->id] + p * 4096, 4096);
		inhibit = 0;
	}
	lrc->regs = phys_to_virt(lrc->obj->pages[1]);
	i915_lrc_init_regs(lrc->regs, &l, i915->info->gen_x10, e->mmio_base, e->class,
			   ring_ggtt, I915_RING_SIZE, ctx->vm->pml4, inhibit,
			   i915_rpcs_for(i915), e->wa_bb_ggtt, e->wa_bb_bytes);
	if (!(i915->info->flags & I915_INFO_HAS_LLC)) {
		for (uint32_t p = 0; p < l.pages; p++) {
			uint8_t *va = phys_to_virt(lrc->obj->pages[p]);
			for (int i = 0; i < 4096; i += 64)
				__asm__ volatile("clflush (%0)" ::"r"(va + i) : "memory");
		}
		__asm__ volatile("mfence" ::: "memory");
	}
	lrc->initialised = !inhibit;
	return lrc;
}

uint64_t i915_lrc_descriptor(struct i915_device *i915, struct i915_lrc *lrc, struct i915_engine *e)
{
	uint64_t desc = GEN8_CTX_VALID | GEN8_CTX_ADDRESSING_MODE_LEGACY64;

	if (i915->info->gen == 8)
		desc |= GEN8_CTX_L3LLC_COHERENT;
	if (i915->info->gen < 11)
		desc |= GEN8_CTX_PRIVILEGE;
	desc |= (uint64_t)lrc->ggtt; /* page aligned: bits 12-31 */
	if (i915->info->gen >= 11) {
		desc |= (uint64_t)lrc->hw_id << GEN11_SW_CTX_ID_SHIFT;
		desc |= (uint64_t)e->instance << GEN11_ENGINE_INSTANCE_SHIFT;
		desc |= (uint64_t)e->class << GEN11_ENGINE_CLASS_SHIFT;
	} else {
		desc |= (uint64_t)lrc->hw_id << GEN8_CTX_ID_SHIFT;
	}
	return desc;
}

/* ---- the kernel context and the golden run ------------------------------------ */

/* Each engine runs one empty batch under a context of the driver's: the
 * hardware saves its default state into that context's image, which
 * becomes the template; and the run proves the engine executes at all. */
int i915_gt_golden_init(struct i915_device *i915)
{
	int ok = 0;

	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		if (!e->kctx) {
			e->kctx = i915_context_create(i915, NULL, NULL);
			if (!e->kctx)
				continue;
		}
		struct i915_gem_context *ctx = e->kctx;
		/* The batch, in the kernel context's space.  The render
		 * engine runs the null render state: every piece of 3D
		 * pipeline state set to a legal value and one vertex drawn
		 * through it, so that what the engine saves afterwards --
		 * the state every later context starts from -- is a
		 * working pipeline and not power-on noise.  The other
		 * engines run one instruction. */
		uint64_t page = mm_allocate_physical_page();
		if (!page)
			continue;
		uint32_t *b = phys_to_virt(page);
		const uint64_t batch_addr = 0x100000;
		uint32_t batch_len = 8;
		mm_memset(b, 0, 4096);
		if (e->class == 0 && i915->info->gen == 9 &&
		    i915_renderstate_gen9_build(b, 4096, batch_addr) == 0) {
			batch_len = i915_renderstate_gen9_bytes();
		} else {
			b[0] = MI_BATCH_BUFFER_END;
			b[1] = MI_NOOP;
		}
		for (int i = 0; i < 4096; i += 64)
			__asm__ volatile("clflush (%0)" ::"r"((uint8_t *)b + i) : "memory");
		__asm__ volatile("mfence" ::: "memory");
		/* The settings a context carries go in ahead of every batch
		 * (i915_request_submit), this one included. */
		if (i915_vm_bind(ctx->vm, batch_addr, &page, 1, 0) != 0) {
			mm_free_physical_page(page);
			continue;
		}
		struct i915_request *rq = i915_request_alloc(ctx, e);
		if (!rq) {
			mm_free_physical_page(page);
			continue;
		}
		if (i915_request_submit(rq, batch_addr, batch_len) != 0) {
			i915_request_put(rq);
			mm_free_physical_page(page);
			continue;
		}
		struct drm_fence *f = rq->fence;
		drm_fence_get(f);
		/* the engine holds the request now; this is done with it */
		i915_request_put(rq);
		rq = NULL;
		int rc = -ETIMEDOUT;
		for (int t = 0; t < 2000; t++) {
			i915_execlists_process_csb(e);
			i915_engine_retire(e);
			if (f->signaled) {
				rc = 0;
				break;
			}
			lapic_delay_us(500);
		}
		drm_fence_put(f);
		if (rc == 0) {
			/* wait until the context has left the port, so its
			 * image holds the saved state */
			for (int t = 0; t < 200 && e->port_busy; t++) {
				i915_execlists_process_csb(e);
				lapic_delay_us(500);
			}
			i915_context_capture_golden(ctx, e);
			ok++;
			i915_dbg("[drm] i915: %s: first batch ran (seqno %u, %s)\n", e->name,
				 e->hwsp[I915_HWS_SEQNO_INDEX],
				 e->golden_ready ? "golden state captured" : "no golden state");
		} else {
			kprintf("[drm] i915: %s: first batch did not complete (seqno %u, status %08x %08x, head %08x)\n",
				e->name, e->hwsp[I915_HWS_SEQNO_INDEX],
				i915_read32(i915, RING_EXECLIST_STATUS_LO(e->mmio_base)),
				i915_read32(i915, RING_EXECLIST_STATUS_HI(e->mmio_base)),
				i915_read32(i915, RING_ACTHD(e->mmio_base)));
			i915_engine_reset(e, "bring-up batch stuck");
		}
		i915_vm_unbind(ctx->vm, batch_addr, 1);
		mm_free_physical_page(page);
	}
	return ok ? 0 : -EIO;
}
