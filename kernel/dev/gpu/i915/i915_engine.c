// LikeOS -- the engines: bring-up, status pages, retirement, reset.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>

struct engine_desc {
	enum i915_engine_id id;
	const char *name;
	uint8_t class, instance;
	uint32_t mask; /* I915_ENGINE_* of the device info */
	uint32_t base_gen9, base_gen11;
	uint32_t gt_iir, irq_shift; /* Gen8-10 */
	uint32_t fw_gen9, fw_gen11;
};

static const struct engine_desc engine_descs[] = {
	{ I915_RCS0, "rcs0", 0, 0, I915_ENGINE_RCS0, RENDER_RING_BASE, RENDER_RING_BASE, 0, 0, I915_FW_RENDER, I915_FW_RENDER },
	{ I915_BCS0, "bcs0", 1, 0, I915_ENGINE_BCS0, BLT_RING_BASE, BLT_RING_BASE, 0, 16, I915_FW_GT, I915_FW_GT },
	{ I915_VCS0, "vcs0", 2, 0, I915_ENGINE_VCS0, GEN6_BSD_RING_BASE, GEN11_BSD_RING_BASE, 1, 0, I915_FW_MEDIA, I915_FW_VDBOX0 },
	{ I915_VCS1, "vcs1", 2, 1, I915_ENGINE_VCS1, GEN8_BSD2_RING_BASE, GEN11_BSD2_RING_BASE, 1, 16, I915_FW_MEDIA, I915_FW_VDBOX1 },
	{ I915_VCS2, "vcs2", 2, 2, I915_ENGINE_VCS2, 0, GEN11_BSD3_RING_BASE, 0, 0, 0, I915_FW_VDBOX2 },
	{ I915_VCS3, "vcs3", 2, 3, I915_ENGINE_VCS3, 0, GEN11_BSD4_RING_BASE, 0, 0, 0, I915_FW_VDBOX3 },
	{ I915_VECS0, "vecs0", 3, 0, I915_ENGINE_VECS0, VEBOX_RING_BASE, GEN11_VEBOX_RING_BASE, 3, 0, I915_FW_MEDIA, I915_FW_VEBOX0 },
	{ I915_VECS1, "vecs1", 3, 1, I915_ENGINE_VECS1, 0, GEN11_VEBOX2_RING_BASE, 0, 0, 0, I915_FW_VEBOX1 },
	{ I915_CCS0, "ccs0", 4, 0, I915_ENGINE_CCS0, 0, GEN12_COMPUTE0_RING_BASE, 0, 0, 0, I915_FW_RENDER },
	{ I915_CCS1, "ccs1", 4, 1, I915_ENGINE_CCS1, 0, GEN12_COMPUTE1_RING_BASE, 0, 0, 0, I915_FW_RENDER },
	{ I915_CCS2, "ccs2", 4, 2, I915_ENGINE_CCS2, 0, GEN12_COMPUTE2_RING_BASE, 0, 0, 0, I915_FW_RENDER },
	{ I915_CCS3, "ccs3", 4, 3, I915_ENGINE_CCS3, 0, GEN12_COMPUTE3_RING_BASE, 0, 0, 0, I915_FW_RENDER },
};

struct i915_engine *i915_engine_by_id(struct i915_device *i915, int id)
{
	if (id < 0 || id >= I915_NUM_ENGINES || !i915->engines[id].present)
		return NULL;
	return &i915->engines[id];
}

struct i915_engine *i915_engine_by_class(struct i915_device *i915, int class, int instance)
{
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (e->present && e->class == class && e->instance == instance)
			return e;
	}
	return NULL;
}

/* ---- the hardware status page --------------------------------------------- */

static int hwsp_alloc(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct drm_gem_object *o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, 4096);
	if (!o)
		return -ENOMEM;
	if (drm_gem_alloc_pages_contig(o) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	if (i915->drm.drv->gem_init)
		i915->drm.drv->gem_init(o);
	uint32_t ggtt;
	if (i915_ggtt_bind_scanout(i915, o, &ggtt) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	((struct i915_bo *)o->priv)->ggtt = ggtt;
	((struct i915_bo *)o->priv)->bound = 1;
	e->hwsp_obj = o;
	e->hwsp_ggtt = ggtt;
	e->hwsp = phys_to_virt(o->pages[0]);
	mm_memset((void *)e->hwsp, 0, 4096);
	return 0;
}

/* ---- per-engine hardware init --------------------------------------------- */

static void engine_stop(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	i915_write32(i915, RING_MI_MODE(e->mmio_base), I915_MASKED_ENABLE(STOP_RING));
	for (int t = 0; t < 1000; t++) {
		if (i915_read32(i915, RING_MI_MODE(e->mmio_base)) & MODE_IDLE)
			break;
		lapic_delay_us(10);
	}
}

static void engine_hw_init(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	/* Idle, the ring registers zeroed (the contexts carry their own),
	 * the status page set, and interrupts of the two kinds the driver
	 * wants (user interrupt, context switch) unmasked at the engine. */
	engine_stop(e);
	i915_write32(i915, RING_CTL(base), 0);
	i915_write32(i915, RING_HEAD(base), 0);
	i915_write32(i915, RING_TAIL(base), 0);
	i915_write32(i915, RING_START(base), 0);
	i915_write32(i915, RING_HWS_PGA(base), e->hwsp_ggtt);
	(void)i915_read32(i915, RING_HWS_PGA(base));
	i915_write32(i915, RING_HWSTAM(base), 0xffffffffu);
	i915_write32(i915, RING_IMR(base),
		     ~(uint32_t)(GT_RENDER_USER_INTERRUPT | GT_CONTEXT_SWITCH_INTERRUPT));
	i915_write32(i915, RING_MI_MODE(base), I915_MASKED_DISABLE(STOP_RING));
	/* execution lists on, PPGTT 48-bit addressing */
	i915_write32(i915, RING_MODE_GEN7(base),
		     I915_MASKED_ENABLE(GFX_RUN_LIST_ENABLE) |
			     I915_MASKED_DISABLE(GFX_REPLAY_MODE));
	(void)i915_read32(i915, RING_MODE_GEN7(base));
	/* what this part needs set before it renders, and which registers
	 * a client's batch may write */
	i915_gt_apply_workarounds(i915, e);
	/* how each kind of surface is cached */
	i915_mocs_init_engine(i915, e);
	i915_execlists_init_engine(e);
}

/* Top-level GT interrupt enables (Gen8-10): each engine's user and
 * context-switch interrupts. */
static void gt_irq_enable(struct i915_device *i915)
{
	if (i915->info->gen >= 11) {
		uint32_t bits = (GT_RENDER_USER_INTERRUPT | GT_CONTEXT_SWITCH_INTERRUPT);
		/* enables per class pair; masks per engine pair (low/high) */
		i915_write32(i915, GEN11_RENDER_COPY_INTR_ENABLE, (bits << 16) | bits);
		i915_write32(i915, GEN11_VCS_VECS_INTR_ENABLE, (bits << 16) | bits);
		i915_write32(i915, GEN11_RCS0_RSVD_INTR_MASK, ~(bits << 16));
		i915_write32(i915, GEN11_BCS_RSVD_INTR_MASK, ~(bits << 16));
		i915_write32(i915, GEN11_VCS0_VCS1_INTR_MASK, ~((bits << 16) | bits));
		i915_write32(i915, GEN11_VCS2_VCS3_INTR_MASK, ~((bits << 16) | bits));
		i915_write32(i915, GEN11_VECS0_VECS1_INTR_MASK, ~((bits << 16) | bits));
		return;
	}
	uint32_t gt0 = 0, gt1 = 0, gt3 = 0;
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		uint32_t bits = GEN8_GT_IRQ_BITS(e->irq_shift);
		if (e->gt_iir == 0)
			gt0 |= bits;
		else if (e->gt_iir == 1)
			gt1 |= bits;
		else if (e->gt_iir == 3)
			gt3 |= bits;
	}
	i915_write32_fw(i915, GEN8_GT_IIR(0), ~0u);
	i915_write32_fw(i915, GEN8_GT_IMR(0), ~gt0);
	i915_write32_fw(i915, GEN8_GT_IER(0), gt0);
	i915_write32_fw(i915, GEN8_GT_IIR(1), ~0u);
	i915_write32_fw(i915, GEN8_GT_IMR(1), ~gt1);
	i915_write32_fw(i915, GEN8_GT_IER(1), gt1);
	i915_write32_fw(i915, GEN8_GT_IIR(3), ~0u);
	i915_write32_fw(i915, GEN8_GT_IMR(3), ~gt3);
	i915_write32_fw(i915, GEN8_GT_IER(3), gt3);
	(void)i915_read32_fw(i915, GEN8_GT_IER(3));
}

/* A full GPU reset: the render, media and blitter domains at once.  Done
 * once at bring-up so the engines start from a known state whatever the
 * firmware left running. */
int i915_gt_reset_all(struct i915_device *i915)
{
	uint32_t mask = GEN6_GRDOM_FULL;
	i915_fw_get(i915, I915_FW_ALL);
	i915_write32_fw(i915, GEN6_GDRST, mask);
	int rc = -ETIMEDOUT;
	for (int t = 0; t < 5000; t++) {
		if (!(i915_read32_fw(i915, GEN6_GDRST) & mask)) {
			rc = 0;
			break;
		}
		lapic_delay_us(10);
	}
	i915_fw_put(i915, I915_FW_ALL);
	if (rc)
		kprintf("[drm] i915: full GPU reset did not complete\n");
	return rc;
}

int i915_engines_init(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	int n = 0;

	mm_memset(i915->engines, 0, sizeof(i915->engines));
	for (unsigned i = 0; i < sizeof(engine_descs) / sizeof(engine_descs[0]); i++) {
		const struct engine_desc *d = &engine_descs[i];
		if (!(info->engine_mask & d->mask))
			continue;
		uint32_t base = info->gen >= 11 ? d->base_gen11 : d->base_gen9;
		if (!base)
			continue;
		struct i915_engine *e = &i915->engines[d->id];
		e->i915 = i915;
		e->id = d->id;
		e->name = d->name;
		e->class = d->class;
		e->instance = d->instance;
		e->present = 1;
		e->mmio_base = base;
		e->gt_iir = d->gt_iir;
		e->irq_shift = d->irq_shift;
		e->fw_domain = info->gen >= 11 ? d->fw_gen11 : d->fw_gen9;
		e->csb_entries = info->gen >= 11 ? GEN11_CSB_ENTRIES : GEN8_CSB_ENTRIES;
		e->fence_context = 0x1000 + d->id;
		e->next_seqno = 1;
		spinlock_init(&e->lock, "i915_engine");
		wq_head_init(&e->wq, "i915_engine");
		if (hwsp_alloc(e) != 0) {
			kprintf("[drm] i915: %s: no status page\n", e->name);
			e->present = 0;
			continue;
		}
		n++;
	}
	i915->nengines = n;
	if (!n)
		return -ENODEV;

	i915_gt_reset_all(i915);
	i915_ggtt_program_pat(i915);
	/* Render standby: whatever the firmware left, the engines stay
	 * awake -- a standby state restores its own idea of the settings
	 * above, and this driver has no power policy yet. */
	if (i915->info->gen >= 8 && i915->info->gen <= 10) {
		i915_dbg("[drm] i915: render standby as found: RC_CONTROL %08x, RC_STATE %08x, RP_CONTROL %08x\n",
			 i915_read32(i915, GEN6_RC_CONTROL), i915_read32(i915, GEN6_RC_STATE),
			 i915_read32(i915, GEN6_RP_CONTROL));
		i915_write32(i915, GEN6_RC_CONTROL, 0);
	}
	i915_rps_init(i915);
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		/* what every context restore of this engine runs */
		i915_wa_bb_init(i915, e);
		engine_hw_init(e);
	}
	gt_irq_enable(i915);
	kprintf("[drm] i915: %d engines:", n);
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present)
			kprintf(" %s", i915->engines[i].name);
	kprintf("\n");
	return 0;
}

int i915_engines_resume(struct i915_device *i915)
{
	if (!i915->nengines)
		return 0;
	i915_gt_reset_all(i915);
	i915_ggtt_program_pat(i915);
	if (i915->info->gen >= 8 && i915->info->gen <= 10)
		i915_write32(i915, GEN6_RC_CONTROL, 0);
	i915_rps_init(i915);
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		i915_wa_bb_init(i915, e);
		engine_hw_init(e);
	}
	gt_irq_enable(i915);
	return 0;
}

void i915_engines_fini(struct i915_device *i915)
{
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present)
			continue;
		engine_stop(e);
		for (int r = 0; r < 2; r++) {
			if (e->resident[r])
				i915_context_put(e->resident[r]);
			e->resident[r] = NULL;
		}
		i915_engine_drop_stale(e);
		if (e->kctx) {
			i915_context_put(e->kctx);
			e->kctx = NULL;
		}
		i915_wa_bb_fini(e);
		if (e->hwsp_obj) {
			drm_gem_put(e->hwsp_obj);
			e->hwsp_obj = NULL;
		}
		e->present = 0;
	}
	i915->nengines = 0;
}

/* ---- interrupts, retirement ------------------------------------------------- */

void i915_engine_irq(struct i915_engine *e, uint32_t bits)
{
	if (bits & GT_CONTEXT_SWITCH_INTERRUPT)
		i915_execlists_process_csb(e);
	if (bits & GT_RENDER_USER_INTERRUPT)
		i915_engine_retire(e);
}

static uint32_t hwsp_seqno(struct i915_engine *e)
{
	return e->hwsp[I915_HWS_SEQNO_INDEX];
}

/* Requests whose sequence the engine has passed: their fences signal,
 * their objects are released.  From the interrupt and from waiters. */
void i915_engine_retire(struct i915_engine *e)
{
	uint32_t passed = hwsp_seqno(e);
	struct i915_request *done = NULL, **dp = &done;
	uint64_t fl;

	if ((int32_t)(passed - e->last_retired) <= 0)
		return;
	spin_lock_irqsave(&e->lock, &fl);
	e->last_retired = passed;
	while (e->inflight && (int32_t)(passed - e->inflight->seqno) >= 0) {
		struct i915_request *rq = e->inflight;
		e->inflight = rq->next;
		if (!e->inflight)
			e->inflight_tail = NULL;
		rq->next = NULL;
		*dp = rq;
		dp = &rq->next;
	}
	spin_unlock_irqrestore(&e->lock, fl);
	drm_fence_signal_upto_ctx(&e->i915->drm, e->fence_context, passed);
	while (done) {
		struct i915_request *rq = done;
		done = rq->next;
		/* the engine has read the ring this far */
		if (rq->lrc && rq->lrc->obj)
			rq->lrc->ring.head = rq->ring_tail;
		i915_request_put(rq);
	}
	poll_notify_wq(&e->wq);
}

int i915_engine_idle(struct i915_engine *e, uint32_t timeout_ms)
{
	for (uint32_t t = 0; t < timeout_ms * 10; t++) {
		i915_engine_retire(e);
		if (!e->inflight && !e->queue)
			return 0;
		lapic_delay_us(100);
	}
	return -ETIMEDOUT;
}

/* ---- reset --------------------------------------------------------------------- */

static uint32_t engine_reset_domain(struct i915_engine *e)
{
	switch (e->id) {
	case I915_RCS0: return GEN6_GRDOM_RENDER;
	case I915_BCS0: return GEN6_GRDOM_BLT;
	case I915_VCS0: return GEN6_GRDOM_MEDIA;
	case I915_VCS1: return GEN8_GRDOM_MEDIA2;
	case I915_VECS0: return GEN6_GRDOM_VECS;
	default: return GEN6_GRDOM_FULL;
	}
}

int i915_engine_reset(struct i915_engine *e, const char *why)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;
	uint64_t fl;

	kprintf("[drm] i915: %s: reset (%s), seqno %u/%u, head %08x\n", e->name, why,
		hwsp_seqno(e), e->next_seqno - 1, i915_read32(i915, RING_ACTHD(base)));
	/* What it was doing: the instruction in hand, which units have not
	 * finished (a zero bit is a unit still busy), where in the batch it
	 * was, and the ring words around the head when the head is in the
	 * ring -- a hang at a flush after a batch is a pipeline that never
	 * drained, and the busy units say which. */
	{
		uint64_t acthd = ((uint64_t)i915_read32(i915, RING_ACTHD_UDW(base)) << 32) |
				 i915_read32(i915, RING_ACTHD(base));
		uint64_t bb = ((uint64_t)i915_read32(i915, RING_BBADDR_UDW_(base)) << 32) |
			      i915_read32(i915, RING_BBADDR(base));
		kprintf("[drm] i915:   instruction %08x (fault %08x), instdone %08x, ps %08x, sc %08x, sampler %08x, row %08x\n",
			i915_read32(i915, RING_IPEHR(base)), i915_read32(i915, RING_IPEIR(base)),
			i915_read32(i915, RING_INSTDONE(base)), i915_read32(i915, RING_INSTPS(base)),
			e->class == 0 ? i915_read32(i915, GEN7_SC_INSTDONE) : 0,
			e->class == 0 ? i915_read32(i915, GEN7_SAMPLER_INSTDONE) : 0,
			e->class == 0 ? i915_read32(i915, GEN7_ROW_INSTDONE) : 0);
		kprintf("[drm] i915:   ring head %08x tail %08x, batch %llx (state %08x), active head %llx, status %08x %08x, context %u\n",
			i915_read32(i915, RING_HEAD(base)), i915_read32(i915, RING_TAIL(base)),
			(unsigned long long)bb, i915_read32(i915, RING_BBSTATE(base)),
			(unsigned long long)acthd,
			i915_read32(i915, RING_EXECLIST_STATUS_LO(base)),
			i915_read32(i915, RING_EXECLIST_STATUS_HI(base)), e->active_ctx_id);
		struct i915_gem_context *actx = e->active_ctx;
		struct i915_lrc *alrc = e->active_lrc;
		if (I915_DEBUG && alrc && alrc->ring.vaddr && (acthd & 0x1fffff) < I915_RING_SIZE &&
		    acthd < 0x100000000ULL) {
			uint32_t *rv = (uint32_t *)alrc->ring.vaddr;
			uint32_t off = (uint32_t)(acthd & (I915_RING_SIZE - 1)) / 4;
			uint32_t from = off >= 12 ? off - 12 : 0;
			kprintf("[drm] i915:   ring words from %04x:", from * 4);
			for (uint32_t i = from; i < from + 16 && i < I915_RING_SIZE / 4; i++)
				kprintf(" %08x", rv[i]);
			kprintf("\n");
		}
		/* The batch itself, through the context's own tables: its first
		 * words (what a client's batch begins with is recognisable) and
		 * the words around the batch head, which is where the engine
		 * was when it was in the batch, or where it left it. */
		if (I915_DEBUG && actx && actx->vm && bb) {
			uint64_t start = bb & ~0xfffULL; /* the client's batches are page-aligned */
			for (int which = 0; which < 2; which++) {
				uint64_t at = which ? (bb & ~0x3ULL) - 32 : start;
				uint64_t phys = i915_vm_lookup(actx->vm, at & 0x0000FFFFFFFFFFFFULL);
				if (!phys) {
					kprintf("[drm] i915:   batch %s %llx: nothing bound there\n",
						which ? "around head" : "start",
						(unsigned long long)at);
					continue;
				}
				uint32_t *bv = phys_to_virt(phys & ~0xfffULL);
				uint32_t o = (uint32_t)(phys & 0xfff) / 4;
				kprintf("[drm] i915:   batch %s %llx:", which ? "around head" : "start",
					(unsigned long long)at);
				for (uint32_t i = 0; i < 16 && o + i < 1024; i++)
					kprintf(" %08x", bv[o + i]);
				kprintf("\n");
			}
		}
	}
	i915->gpu_reset_count++;
	i915_fw_get(i915, I915_FW_ALL);
	/* ask, wait for ready, reset the domain, release */
	i915_write32(i915, RING_RESET_CTL(base), I915_MASKED_ENABLE(RESET_CTL_REQUEST_RESET));
	for (int t = 0; t < 700; t++) {
		if (i915_read32(i915, RING_RESET_CTL(base)) & RESET_CTL_READY_TO_RESET)
			break;
		lapic_delay_us(10);
	}
	uint32_t dom = engine_reset_domain(e);
	i915_write32_fw(i915, GEN6_GDRST, dom);
	for (int t = 0; t < 5000; t++) {
		if (!(i915_read32_fw(i915, GEN6_GDRST) & dom))
			break;
		lapic_delay_us(10);
	}
	i915_write32(i915, RING_RESET_CTL(base), I915_MASKED_DISABLE(RESET_CTL_REQUEST_RESET));
	i915_fw_put(i915, I915_FW_ALL);

	/* Everything in flight is lost: the requests fail, their contexts
	 * are marked, and the hardware starts over. */
	spin_lock_irqsave(&e->lock, &fl);
	struct i915_request *lost = e->inflight;
	e->inflight = e->inflight_tail = NULL;
	e->active_ctx = NULL;
	e->active_lrc = NULL;
	e->active_ctx_id = 0;
	e->port_busy = 0;
	/* nothing stays loaded across a reset */
	for (int i = 0; i < 2; i++) {
		if (e->resident[i] && e->nstale < 4)
			e->stale[e->nstale++] = e->resident[i];
		e->resident[i] = NULL;
	}
	spin_unlock_irqrestore(&e->lock, fl);
	i915_engine_drop_stale(e);
	int first = 1;
	while (lost) {
		struct i915_request *rq = lost;
		lost = rq->next;
		rq->next = NULL;
		if (rq->ctx) {
			rq->ctx->reset_count++;
			if (first) {
				rq->ctx->batch_active++;
				rq->ctx->hang_count++;
				/* A context that asked not to be recovered is
				 * done at its first hang: its client replaces
				 * it as soon as a submission fails, and the
				 * state the engine saved of it mid-hang is
				 * nothing to run on.  Others get three. */
				if (rq->ctx->bannable &&
				    (rq->ctx->hang_count >= 3 || !rq->ctx->recoverable))
					rq->ctx->banned = 1;
			} else {
				rq->ctx->batch_pending++;
			}
		}
		rq->fence->error = -EIO;
		rq->guilty = first;
		first = 0;
		/* The engine restarted: nothing of this context's ring was
		 * necessarily read, and none of it will be now.  Empty it,
		 * in the image as well, so the software and the hardware
		 * agree about where the next request begins. */
		if (rq->lrc) {
			struct i915_lrc *lrc = rq->lrc;
			struct i915_lrc_layout l;
			if (lrc->obj && lrc->regs &&
			    i915_lrc_layout(i915->info->gen_x10, e->class, &l) == 0) {
				lrc->regs[l.ring_head + 1] = 0;
				lrc->regs[l.ring_tail + 1] = 0;
			}
			lrc->ring.head = lrc->ring.tail = 0;
		}
		drm_fence_signal(rq->fence);
		i915_request_put(rq);
	}
	e->hwsp[I915_HWS_SEQNO_INDEX] = e->next_seqno - 1;
	e->last_retired = e->next_seqno - 1;
	engine_hw_init(e);
	/* whatever was queued but not submitted runs now */
	spin_lock_irqsave(&e->lock, &fl);
	i915_execlists_submit(e);
	spin_unlock_irqrestore(&e->lock, fl);
	i915_engine_drop_stale(e);
	poll_notify_wq(&e->wq);
	return 0;
}

void i915_engine_drop_stale(struct i915_engine *e)
{
	struct i915_gem_context *gone[4];
	int n;
	uint64_t fl;

	spin_lock_irqsave(&e->lock, &fl);
	n = e->nstale;
	for (int i = 0; i < n; i++)
		gone[i] = e->stale[i];
	e->nstale = 0;
	spin_unlock_irqrestore(&e->lock, fl);
	for (int i = 0; i < n; i++)
		i915_context_put(gone[i]);
}
