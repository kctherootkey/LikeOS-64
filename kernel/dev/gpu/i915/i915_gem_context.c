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
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_renderstate.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>
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
	i915_context_default_engines(ctx);
	return ctx;
}

/* The default engine map: the legacy ring numbering, each ring the
 * first engine of its class a client sees (the first video engine is
 * the first one present, whichever the fuses left). */
void i915_context_default_engines(struct i915_gem_context *ctx)
{
	struct i915_device *i915 = ctx->i915;

	ctx->engines[0] = i915_engine_by_id(i915, I915_RCS0); /* DEFAULT/RENDER */
	ctx->engines[1] = i915_engine_by_id(i915, I915_RCS0);
	ctx->engines[2] = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 0); /* BSD */
	ctx->engines[3] = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_COPY, 0); /* BLT */
	ctx->engines[4] = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO_ENHANCE, 0); /* VEBOX */
	ctx->nengines = 5;
	ctx->explicit_engines = 0;
}

/* ---- the engines as clients name them ---------------------------------------- */

/* A client numbers the engines of a class densely, in the order of the
 * hardware's instances: a fused-off engine leaves no hole.  Meteor Lake
 * with its second decoder fused off has vcs0 and vcs2, which a client
 * knows as video instances 0 and 1.  Every (class, instance) a client
 * hands in or is handed goes through these two. */
int i915_engine_uabi_instance(const struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	int n = 0;

	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		const struct i915_engine *o = &i915->engines[i];
		if (o->present && o->class == e->class && o->instance < e->instance)
			n++;
	}
	return n;
}

struct i915_engine *i915_engine_lookup_user(struct i915_device *i915, int class, int instance)
{
	int n = 0;

	if (class < 0 || instance < 0)
		return NULL;
	/* the table runs in the order of the instances within a class */
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present || e->class != class)
			continue;
		if (n++ == instance)
			return e;
	}
	return NULL;
}

/* The logical instance (what the GuC and a parallel submission count
 * by): dense too, the video engines from Gen11 on numbered the even
 * instances first -- 0, 2, 4, 6, 1, 3, 5, 7 -- which keeps the pairs
 * that split a frame apart. */
int i915_engine_logical_instance(const struct i915_engine *e)
{
	static const uint8_t vcs_order[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
	struct i915_device *i915 = e->i915;
	int video = e->class == I915_ENGINE_CLASS_VIDEO && i915->media_ip >= I915_IP(11, 0);
	int n = 0;

	for (unsigned j = 0; j < 8; j++) {
		unsigned phys = video ? vcs_order[j] : j;
		if (phys == e->instance)
			return n;
		for (int i = 0; i < I915_NUM_ENGINES; i++) {
			const struct i915_engine *o = &i915->engines[i];
			if (o->present && o->class == e->class && o->instance == phys) {
				n++;
				break;
			}
		}
	}
	return n;
}

/* The video engine a submission on the legacy BSD ring goes to.  With
 * one video engine that one, whatever the selector says.  With more, the
 * selector names the first or the second as a client numbers them, or
 * neither -- and then the file keeps to one engine, picked (as good as at
 * random) at its first such submission, so a client's work stays in
 * order on one engine.  `file_pick' is the file's choice, 0 for none
 * yet; NULL for a selector that names nothing. */
struct i915_engine *i915_engine_legacy_bsd(struct i915_device *i915, uint32_t *file_pick,
					   unsigned bsd_flags)
{
	unsigned nvideo = 0;

	while (i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, (int)nvideo))
		nvideo++;
	if (nvideo <= 1)
		return i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 0);
	switch (bsd_flags) {
	case I915_EXEC_BSD_DEFAULT: {
		uint32_t pick = file_pick ? __atomic_load_n(file_pick, __ATOMIC_RELAXED) : 1;
		if (!pick) {
			uint32_t none = 0;
			uint32_t want = 1 + (uint32_t)(timer_rdtsc() % nvideo);
			__atomic_compare_exchange_n(file_pick, &none, want, 0, __ATOMIC_RELAXED,
						    __ATOMIC_RELAXED);
			pick = __atomic_load_n(file_pick, __ATOMIC_RELAXED);
		}
		return i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, (int)pick - 1);
	}
	case I915_EXEC_BSD_RING1:
		return i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 0);
	case I915_EXEC_BSD_RING2:
		return i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 1);
	default:
		return NULL;
	}
}

void i915_context_get(struct i915_gem_context *ctx)
{
	__atomic_fetch_add(&ctx->refs, 1, __ATOMIC_ACQ_REL);
}

static void lrc_free(struct i915_device *i915, struct i915_lrc *lrc, struct i915_vm *vm)
{
	i915_guc_lrc_release(i915, lrc, vm);
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
		lrc_free(ctx->i915, &ctx->lrc[i], ctx->vm);
	i915_vm_put(ctx->vm);
	kfree(ctx);
}

/* The power and clock state every render context asks for: all the
 * slices, subslices and execution units the fuses enable.
 *
 * Gen12 gates only whole slices (Tiger Lake and its relatives have one
 * slice, which the request names); Xe_HP and later gate nothing the
 * request controls, and it stays zero. */
uint32_t i915_rpcs_for(struct i915_device *i915)
{
	unsigned slices = 0, subslices = 0;

	if (i915->info->gen_x10 >= 125)
		return 0;
	if (i915->info->gen_x10 >= 120) {
		unsigned n = 0;
		for (uint32_t m = i915->slice_mask; m; m >>= 1)
			n += m & 1;
		if (!n)
			n = 1;
		return GEN8_RPCS_ENABLE | GEN8_RPCS_S_CNT_ENABLE |
		       ((n << GEN11_RPCS_S_CNT_SHIFT) & GEN11_RPCS_S_CNT_MASK);
	}

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

/* Gen12: the engine's AUX table invalidation register, absolute, or 0
 * where the engine has none or the part keeps its compression metadata
 * in memory directly (flat CCS: DG2). */
static uint32_t aux_inv_reg(struct i915_device *i915, const struct i915_engine *e)
{
	uint32_t reg;

	/* DG2's metadata is in memory; Xe2 on keeps it in memory too and
	 * has no AUX table at all */
	if (i915->info->platform == I915_PLATFORM_DG2 || i915->gt_ip >= I915_IP(20, 0))
		return 0;
	switch (e->id) {
	case I915_RCS0: reg = GEN12_CCS_AUX_INV; break;
	case I915_BCS0: reg = GEN12_BCS0_AUX_INV; break;
	case I915_VCS0: reg = GEN12_VD0_AUX_INV; break;
	case I915_VCS2: reg = GEN12_VD2_AUX_INV; break;
	case I915_VECS0: reg = GEN12_VE0_AUX_INV; break;
	case I915_CCS0: reg = GEN12_CCS0_AUX_INV; break;
	default: return 0;
	}
	return reg + e->gsi_offset;
}

uint32_t i915_engine_aux_inv_reg(struct i915_device *i915, const struct i915_engine *e)
{
	return i915->info->gen_x10 >= 120 ? aux_inv_reg(i915, e) : 0;
}

/* The scratch range the copy engine's per-context batch blits nothing
 * into (Wa_16018031267): the top 64 KB of every address space, which no
 * client is handed and which leads to the scratch page. */
#define CTX_RSVD_SCRATCH_ADDR (I915_VM_SIZE - 0x10000)

/* Gen12: the two batches the context carries in its image. */
static void lrc_write_wa_bb(struct i915_device *i915, struct i915_engine *e,
			    struct i915_lrc *lrc, const struct i915_lrc_layout *l,
			    uint32_t *ggtt, uint32_t *bytes)
{
	struct i915_lrc_wa_bb p;
	unsigned ip = i915->gt_ip;
	int dg2 = i915->info->platform == I915_PLATFORM_DG2;

	mm_memset(&p, 0, sizeof(p));
	p.image_ggtt = lrc->ggtt;
	p.aux_inv = aux_inv_reg(i915, e);
	p.render = e->class == 0;
	p.state_cache_inv = ip >= I915_IP(12, 0) && ip <= I915_IP(12, 10);
	p.icache_inv = i915->subplatform == I915_SUBPLATFORM_DG2_G11 &&
		       (e->class == 0 || e->class == 4);
	p.draw_watermark = dg2 || ((ip == I915_IP(12, 70) || ip == I915_IP(12, 71)) &&
				   i915->gt_step < I915_STEP_B0);
	p.fastcolor_blt = ip >= I915_IP(12, 55) && ip <= I915_IP(12, 71) && e->class == 1 &&
			  e->instance == 0;
	p.blt_mocs = (uint8_t)i915_mocs_uc_index(i915);
	p.scratch_addr = CTX_RSVD_SCRATCH_ADDR;
	*bytes = i915_lrc_wa_bb_gen12(phys_to_virt(lrc->obj->pages[l->wa_bb_page]),
				      phys_to_virt(lrc->obj->pages[l->wa_bb_page + 1]), l, &p);
	*ggtt = lrc->ggtt + (uint32_t)l->wa_bb_page * 4096;
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
	/* Wa_22016122933: the media GT of media IP 13.00 reads what it
	 * shares with the processor uncached */
	int shared_uc = e->gsi_offset && i915->media_ip == I915_IP(13, 0);
	lrc->ring.obj = i915_gem_alloc_bound(i915, I915_RING_SIZE, shared_uc, 1, &ring_ggtt);
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
	lrc->obj = i915_gem_alloc_bound(i915, (uint64_t)l.pages * 4096, shared_uc, 0, &img_ggtt);
	if (!lrc->obj) {
		kprintf("[drm] i915: %s: no memory for a %u page context image\n",
			e->name, l.pages);
		lrc_free(i915, lrc, NULL);
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
	uint32_t wa_ggtt = e->wa_bb_ggtt, wa_bytes = e->wa_bb_bytes;
	if (l.wa_bb_page)
		lrc_write_wa_bb(i915, e, lrc, &l, &wa_ggtt, &wa_bytes);
	i915_lrc_init_regs(lrc->regs, &l, i915->info->gen_x10, e->mmio_base, e->class,
			   ring_ggtt, I915_RING_SIZE, i915_vm_root(ctx->vm), inhibit,
			   i915_rpcs_for(i915), wa_ggtt, wa_bytes);
	if (i915_vm_3lvl(i915))
		i915_lrc_set_pdps(lrc->regs, &l, ctx->vm->pd32);
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

/* The descriptor the execution list port takes: the image's address,
 * the addressing mode, and the context id the status buffer will name.
 * The privilege bit is what lets the context's batches run through its
 * own address space; every generation wants it.  From Xe_HP on the id
 * moved up and the engine is no longer part of it. */
uint64_t i915_lrc_descriptor(struct i915_device *i915, struct i915_lrc *lrc, struct i915_engine *e)
{
	uint64_t desc = GEN8_CTX_VALID | GEN8_CTX_PRIVILEGE;

	desc |= i915_vm_3lvl(i915) ? GEN8_CTX_ADDRESSING_MODE_LEGACY32 :
				     GEN8_CTX_ADDRESSING_MODE_LEGACY64;

	if (i915->info->gen == 8)
		desc |= GEN8_CTX_L3LLC_COHERENT;
	desc |= (uint64_t)lrc->ggtt; /* page aligned: bits 12-31 */
	if (i915->info->gen_x10 >= 125) {
		desc |= (uint64_t)lrc->hw_id << XEHP_SW_CTX_ID_SHIFT;
	} else if (i915->info->gen >= 11) {
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

	if (i915_is_legacy(i915))
		return i915_legacy_gt_golden_init(i915);

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
		mm_write_lock(&i915->submit_lock);
		int src = i915_request_submit(rq, batch_addr, batch_len);
		mm_write_unlock(&i915->submit_lock);
		if (src != 0) {
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
			if (!i915->guc.submission)
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
			/* wait until the context has left the engine, so its
			 * image holds the saved state: the port frees with
			 * the execution lists; under the GuC the context's
			 * scheduling is switched off, which the GuC answers
			 * once the context is out */
			int saved = 1;
			if (i915->guc.submission) {
				saved = i915_guc_lrc_quiesce(i915, &ctx->lrc[e->id]) == 0;
			} else {
				for (int t = 0; t < 200 && e->port_busy; t++) {
					i915_execlists_process_csb(e);
					lapic_delay_us(500);
				}
			}
			if (saved)
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
			/* under the GuC the firmware resets engines itself */
			if (!i915->guc.submission)
				i915_engine_reset(e, "bring-up batch stuck");
		}
		i915_vm_unbind(ctx->vm, batch_addr, 1);
		mm_free_physical_page(page);
	}
	return ok ? 0 : -EIO;
}

/* ---- clearing local memory the processor cannot reach ------------------------- */

/* Where the copy engine's kernel context maps what it clears, and its
 * batch: well above the golden batch, 64 KB aligned for DG2. */
#define CLEAR_BATCH_ADDR 0x180000ULL
#define CLEAR_TARGET_ADDR 0x200000000ULL
#define CLEAR_CHUNK_PAGES 2048u /* 8 MB a blit */

/* The batch at CLEAR_BATCH_ADDR on the copy engine's kernel context, `n'
 * dwords, waited for up to two seconds. */
static int clear_run(struct i915_device *i915, struct i915_engine *e, uint32_t n)
{
	struct i915_request *rq = i915_request_alloc(e->kctx, e);
	int rc = -ETIMEDOUT;

	if (!rq)
		return -EIO;
	mm_write_lock(&i915->submit_lock);
	int src = i915_request_submit(rq, CLEAR_BATCH_ADDR, n * 4);
	mm_write_unlock(&i915->submit_lock);
	if (src != 0) {
		i915_request_put(rq);
		return -EIO;
	}
	struct drm_fence *f = rq->fence;
	drm_fence_get(f);
	i915_request_put(rq);
	for (int t = 0; t < 20000; t++) { /* 2 s */
		if (!i915->guc.submission)
			i915_execlists_process_csb(e);
		i915_engine_retire(e);
		if (f->signaled) {
			rc = f->error ? f->error : 0;
			break;
		}
		lapic_delay_us(100);
	}
	drm_fence_put(f);
	return rc;
}

/* Fill an object with zeroes by the copy engine and wait for it: the
 * local memory allocator's way of clearing memory beyond the processor's
 * window.  Xe_HP has the fast colour blit with its memory-type bit;
 * Gen12.0 the classic colour blit, a page a row. */
static int gt_clear_object(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_engine *e = i915_engine_by_id(i915, I915_BCS0);
	int rc = -EIO;

	if (!i915->gt_ready || !e || !e->kctx || !o->pages || !o->npages)
		return -ENODEV;
	struct i915_gem_context *ctx = e->kctx;
	uint64_t page = mm_allocate_physical_page();
	if (!page)
		return -ENOMEM;
	uint32_t *b = phys_to_virt(page), n = 0;
	uint32_t chunks = (o->npages + CLEAR_CHUNK_PAGES - 1) / CLEAR_CHUNK_PAGES;
	int lmem = i915_lmem_object(o);
	uint32_t mocs = i915_mocs_uc_index(i915) << 1;

	if (chunks * 16 + 4 > 1024) {
		mm_free_physical_page(page);
		return -E2BIG;
	}
	mm_memset(b, 0, 4096);
	for (uint32_t c = 0; c < chunks; c++) {
		uint64_t addr = CLEAR_TARGET_ADDR + (uint64_t)c * CLEAR_CHUNK_PAGES * 4096;
		uint32_t rows = o->npages - c * CLEAR_CHUNK_PAGES;
		if (rows > CLEAR_CHUNK_PAGES)
			rows = CLEAR_CHUNK_PAGES;
		if (i915->gt_ip >= I915_IP(12, 55)) {
			b[n++] = XY_FAST_COLOR_BLT_CMD | (2u << 19) | (16 - 2);
			b[n++] = XY_FAST_COLOR_BLT_MOCS(mocs) | (4096 - 1);
			b[n++] = 0;
			b[n++] = (rows << 16) | (4096 / 4);
			b[n++] = (uint32_t)addr;
			b[n++] = (uint32_t)(addr >> 32);
			b[n++] = lmem ? 0 : (1u << 31); /* system memory */
			for (int i = 0; i < 9; i++)
				b[n++] = 0; /* the colour, zero, and the rest */
		} else {
			b[n++] = XY_COLOR_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
			b[n++] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | 4096;
			b[n++] = 0;
			b[n++] = (rows << 16) | (4096 / 4);
			b[n++] = (uint32_t)addr;
			b[n++] = (uint32_t)(addr >> 32);
			b[n++] = 0; /* the colour */
			b[n++] = MI_NOOP;
		}
	}
	b[n++] = MI_BATCH_BUFFER_END;
	b[n++] = MI_NOOP;
	__asm__ volatile("sfence" ::: "memory");

	mm_write_lock(&i915->clear_lock);
	if (i915_vm_bind(ctx->vm, CLEAR_BATCH_ADDR, &page, 1, 0) != 0)
		goto out_unlock;
	if (i915_vm_bind(ctx->vm, CLEAR_TARGET_ADDR, o->pages, o->npages, 0) != 0)
		goto out_batch;
	rc = clear_run(i915, e, n);
	if (rc == -ETIMEDOUT)
		kprintf("[drm] i915: clearing %u pages of local memory did not complete\n",
			o->npages);
	i915_vm_unbind(ctx->vm, CLEAR_TARGET_ADDR, o->npages);
out_batch:
	i915_vm_unbind(ctx->vm, CLEAR_BATCH_ADDR, 1);
out_unlock:
	mm_write_unlock(&i915->clear_lock);
	mm_free_physical_page(page);
	return rc;
}

/* Where the compression-state clear reads its zeroes from: the state of
 * a 1024-page blit is 8 KB. */
#define CCS_ZERO_ADDR 0x1c0000ULL
#define CCS_CHUNK_PAGES 1024u

int i915_gt_clear_ccs(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_engine *e = i915_engine_by_id(i915, I915_BCS0);

	/* The discrete Xe2 parts keep no state for system memory and clear
	 * local memory's with the data; the integrated ones keep it for
	 * all memory, by physical address, whoever used the page last. */
	if (i915->gt_ip < I915_IP(20, 0) || !i915->has_flat_ccs ||
	    (i915->info->flags & I915_INFO_IS_DGFX))
		return 0;
	if (!i915->gt_ready || !e || !e->kctx || !o->pages || !o->npages)
		return 0;
	struct i915_gem_context *ctx = e->kctx;
	uint32_t chunks = (o->npages + CCS_CHUNK_PAGES - 1) / CCS_CHUNK_PAGES;
	uint32_t mocs = XE2_XY_CTRL_SURF_MOCS(i915_mocs_uc_index(i915));
	uint64_t zero[2] = { 0, 0 };
	uint64_t page = mm_allocate_physical_page();
	int rc = -ENOMEM;

	if (!page)
		return -ENOMEM;
	if (chunks * 5 + 6 > 1024) {
		mm_free_physical_page(page);
		return -E2BIG;
	}
	zero[0] = mm_allocate_physical_page();
	zero[1] = mm_allocate_physical_page();
	if (!zero[0] || !zero[1])
		goto out_zero;
	mm_memset(phys_to_virt(zero[0]), 0, 4096);
	mm_memset(phys_to_virt(zero[1]), 0, 4096);

	uint32_t *b = phys_to_virt(page), n = 0;
	mm_memset(b, 0, 4096);
	for (uint32_t c = 0; c < chunks; c++) {
		uint64_t dst = CLEAR_TARGET_ADDR + (uint64_t)c * CCS_CHUNK_PAGES * 4096;
		uint32_t pages = o->npages - c * CCS_CHUNK_PAGES;
		if (pages > CCS_CHUNK_PAGES)
			pages = CCS_CHUNK_PAGES;
		/* zeroes in, as they are; out into the state of the object */
		b[n++] = XY_CTRL_SURF_COPY_BLT | XY_CTRL_SURF_SRC_DIRECT |
			 XE2_XY_CTRL_SURF_PAGES(pages);
		b[n++] = (uint32_t)CCS_ZERO_ADDR;
		b[n++] = (uint32_t)(CCS_ZERO_ADDR >> 32) | mocs;
		b[n++] = (uint32_t)dst;
		b[n++] = (uint32_t)(dst >> 32) | mocs;
	}
	/* the state written out of the engine's own caches */
	b[n++] = (MI_FLUSH_DW + 1) | MI_FLUSH_DW_CCS;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = MI_BATCH_BUFFER_END;
	b[n++] = MI_NOOP;
	__asm__ volatile("sfence" ::: "memory");

	rc = -EIO;
	mm_write_lock(&i915->clear_lock);
	if (i915_vm_bind(ctx->vm, CLEAR_BATCH_ADDR, &page, 1, 0) != 0)
		goto out_unlock;
	if (i915_vm_bind(ctx->vm, CCS_ZERO_ADDR, zero, 2, 1) != 0)
		goto out_batch;
	/* the object through a compressing entry: that is what makes the
	 * engine reach its state */
	if (i915_vm_bind_pat(ctx->vm, CLEAR_TARGET_ADDR, o->pages, o->npages, XE2_PAT_UC_COMP) != 0)
		goto out_src;
	rc = clear_run(i915, e, n);
	if (rc == -ETIMEDOUT)
		kprintf("[drm] i915: clearing the compression state of %u pages did not complete\n",
			o->npages);
	i915_vm_unbind(ctx->vm, CLEAR_TARGET_ADDR, o->npages);
out_src:
	i915_vm_unbind(ctx->vm, CCS_ZERO_ADDR, 2);
out_batch:
	i915_vm_unbind(ctx->vm, CLEAR_BATCH_ADDR, 1);
out_unlock:
	mm_write_unlock(&i915->clear_lock);
out_zero:
	if (zero[0])
		mm_free_physical_page(zero[0]);
	if (zero[1])
		mm_free_physical_page(zero[1]);
	mm_free_physical_page(page);
	return rc;
}

/* Once the engines run: local memory the processor cannot see may be
 * handed out, cleared by the copy engine. */
void i915_gt_lmem_ready(struct i915_device *i915)
{
	if (i915_lmem_present(i915) && i915_engine_by_id(i915, I915_BCS0))
		i915_lmem_set_clear_hook(gt_clear_object);
}
