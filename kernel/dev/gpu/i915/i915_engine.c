// LikeOS -- the engines: bring-up, status pages, retirement, reset.
//
// From Gen11 on, which video engines a part has is a matter of fuses as
// much as of the platform: a GT1 part may have a decoder fused off, and
// an engine that is not there must not be touched.  Meteor Lake put the
// video engines on a GT of their own, with its own forcewake and reset;
// their ring registers stay where they were.  Xe2 names its compute
// engines in a fuse register, and gives all compute slices to the first
// of them where there are several (the CCS mode).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_wa_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
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
	/* The page the device writes and the processor reads: sequence
	 * numbers, context status.  Bound so that the two see the same
	 * memory -- through the shared last-level cache where the part has
	 * one, as the ring and the context image are.  Bound uncached (the
	 * way a scanout surface is) the device's writes went past the
	 * processor's caches, and a line the processor had read before
	 * stayed stale in them: a completed batch went unnoticed until the
	 * line happened to be evicted or the hang check looked, seconds
	 * later. */
	uint32_t ggtt;
	int uncached = !(i915->info->flags & I915_INFO_HAS_LLC);
	if (i915_ggtt_bind_obj(i915, o, uncached, &ggtt) != 0) {
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

/* ---- what the fuses leave (Gen11 on) ---------------------------------------- */

/* Gen11 hooks a scaler to every other video engine as counted among those
 * present; Gen12 to every even physical one, and to an odd one whose even
 * neighbour is fused off; Xe_HP media says per scaler whether it exists. */
static int vdbox_has_sfc(struct i915_device *i915, unsigned physical, unsigned logical,
			 uint32_t vdbox_mask)
{
	if (!(i915->sfc_mask & (1u << (physical / 2))))
		return 0;
	if (i915->media_ip >= I915_IP(12, 0))
		return (physical % 2 == 0) || !(vdbox_mask & (1u << (physical - 1)));
	if (i915->media_ip >= I915_IP(11, 0))
		return logical % 2 == 0;
	return 0;
}

/* Drop the video engines the fuses disable.  Older parts' fuse register
 * says what is disabled; Xe_HP media's says what is enabled. */
static uint32_t apply_media_fuses(struct i915_device *i915, uint32_t engine_mask)
{
	uint32_t gsi = i915->has_media_gt ? I915_MEDIA_GT_BASE : 0;
	unsigned logical = 0;

	i915->vdbox_sfc_access = 0;
	i915->sfc_mask = 0xff;
	if (i915->media_ip < I915_IP(11, 0))
		return engine_mask;
	uint32_t fuse = i915_read32(i915, gsi + GEN11_GT_VEBOX_VDBOX_DISABLE);
	if (i915->media_ip < I915_IP(12, 55))
		fuse = ~fuse;
	uint32_t vdbox = fuse & GEN11_GT_VDBOX_DISABLE_MASK;
	uint32_t vebox = (fuse & GEN11_GT_VEBOX_DISABLE_MASK) >> GEN11_GT_VEBOX_DISABLE_SHIFT;
	if (i915->media_ip >= I915_IP(12, 55))
		i915->sfc_mask = (uint8_t)XEHP_SFC_ENABLE(i915_read32(i915, gsi + HSW_PAVP_FUSE1));
	for (unsigned i = 0; i < 4; i++) {
		uint32_t bit = I915_ENGINE_VCS0 << i;
		if (!(engine_mask & bit)) {
			vdbox &= ~(1u << i);
			continue;
		}
		if (!(vdbox & (1u << i))) {
			engine_mask &= ~bit;
			kprintf("[drm] i915: vcs%u fused off\n", i);
			continue;
		}
		if (vdbox_has_sfc(i915, i, logical, vdbox))
			i915->vdbox_sfc_access |= (uint8_t)(1u << i);
		logical++;
	}
	for (unsigned i = 0; i < 2; i++) {
		uint32_t bit = I915_ENGINE_VECS0 << i;
		if ((engine_mask & bit) && !(vebox & (1u << i))) {
			engine_mask &= ~bit;
			kprintf("[drm] i915: vecs%u fused off\n", i);
		}
	}
	return engine_mask;
}

/* Xe2 on: the compute engines the fuses enable (read at every GT
 * initialisation: the GuC is told about them before the engines are set
 * up), and the CCS mode -- with more than one, all the compute slices go
 * to the first and only it is driven. */
void i915_xe2_compute_fuses(struct i915_device *i915)
{
	uint32_t ccs = (i915->info->engine_mask >> 8) & 0xf;
	unsigned n = 0;

	if (i915->gt_ip < I915_IP(20, 0))
		return;
	ccs &= XE2_CCS_EN(i915_read32(i915, XEHP_FUSE4));
	i915->ccs_fused = (uint8_t)ccs;
	for (uint32_t m = ccs; m; m >>= 1)
		n += m & 1;
	i915->ccs_mode = n > 1 ? 1 : 0;
}

/* The CCS mode register: every compute slice the fuses leave assigned to
 * the first compute engine, the rest disabled. */
uint32_t i915_xe2_ccs_mode_value(const struct i915_device *i915)
{
	uint32_t mode = 0xfffu; /* every slice disabled */
	unsigned first = 0;

	while (first < 4 && !(i915->ccs_fused & (1u << first)))
		first++;
	for (unsigned slice = 0; slice < 4; slice++) {
		if (!(i915->ccs_fused & (1u << slice)))
			continue;
		mode &= ~XEHP_CCS_MODE_CSLICE(slice, XEHP_CCS_MODE_CSLICE_MASK);
		mode |= XEHP_CCS_MODE_CSLICE(slice, first);
	}
	return mode | (0xfffu << 16);
}

/* Xe_HP: a compute engine whose quarter of the dual subslices is fused
 * off entirely is not there. */
static uint32_t apply_compute_fuses(struct i915_device *i915, uint32_t engine_mask)
{
	uint32_t ccs = (engine_mask >> 8) & 0xf;
	unsigned n = 0;

	if (i915->gt_ip >= I915_IP(20, 0)) {
		uint32_t keep = i915->ccs_fused;
		for (unsigned i = 0; i < 4; i++)
			if ((ccs & (1u << i)) && !(keep & (1u << i)))
				kprintf("[drm] i915: ccs%u fused off\n", i);
		/* in CCS mode 1 only the first carries work */
		if (i915->ccs_mode)
			keep &= ~(keep - 1);
		return (engine_mask & ~(0xfu << 8)) | ((ccs & keep) << 8);
	}
	for (uint32_t m = ccs; m; m >>= 1)
		n += m & 1;
	if (i915->gt_ip < I915_IP(12, 50) || n <= 1)
		return engine_mask;
	for (unsigned i = 0; i < 4; i++) {
		uint32_t quarter = (i915->dss_compute >> (i * 8)) & 0xff;
		if ((ccs & (1u << i)) && !quarter) {
			engine_mask &= ~(I915_ENGINE_CCS0 << i);
			kprintf("[drm] i915: ccs%u fused off\n", i);
		}
	}
	return engine_mask;
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

static int is_first_render_compute(const struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;

	if (e->id == I915_RCS0)
		return 1;
	if (e->class != 4 || i915->engines[I915_RCS0].present)
		return 0;
	for (int i = I915_CCS0; i < (int)e->id; i++)
		if (i915->engines[i].present)
			return 0;
	return 1;
}

/* Wa_16023105232, Wa_14025941587 (Xe2 on): an engine's idle delay below
 * its power context's wait time, and at least 5 us; the delay counts in
 * units of eight crystal periods, the wait in 640 ns units. */
static int wa_idledly_short(const struct i915_engine *e)
{
	const struct i915_device *i915 = e->i915;

	if (e->gsi_offset)
		return i915->media_ip >= I915_IP(13, 1) && i915->media_ip <= I915_IP(30, 0);
	return i915->gt_ip >= I915_IP(20, 1) && i915->gt_ip <= I915_IP(30, 1);
}

static int wa_idledly_min(const struct i915_engine *e)
{
	const struct i915_device *i915 = e->i915;

	if (e->gsi_offset)
		return i915->media_ip >= I915_IP(13, 1) && i915->media_ip <= I915_IP(35, 3);
	return i915->gt_ip >= I915_IP(20, 1) && i915->gt_ip <= I915_IP(35, 11);
}

int i915_engine_wa_idledly(const struct i915_engine *e)
{
	return wa_idledly_short(e) || wa_idledly_min(e);
}

static void adjust_idledly(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t units_ps = 8 * i915->ts_base_ps;
	uint32_t reg, maxcnt, idledly, ticks;
	int inhibit, applied = 0, clamped = 0;

	if (!i915_engine_wa_idledly(e) || !units_ps)
		return;
	reg = i915_read32(i915, XE2_RING_IDLEDLY(e->mmio_base));
	maxcnt = (i915_read32(i915, XE2_RING_PWRCTX_MAXCNT(e->mmio_base)) & XE2_IDLE_WAIT_TIME) * 640;
	inhibit = !!(reg & XE2_INHIBIT_SWITCH_UNTIL_PREEMPTED);
	idledly = (uint32_t)(((uint64_t)(reg & XE2_IDLE_DELAY) * units_ps + 500) / 1000);
	if (wa_idledly_min(e) && idledly < 5000) {
		idledly = 5000;
		applied = 1;
	}
	if (wa_idledly_short(e)) {
		if (inhibit) {
			reg &= ~XE2_INHIBIT_SWITCH_UNTIL_PREEMPTED;
			applied = 1;
		}
		if (idledly >= maxcnt) {
			idledly = maxcnt ? maxcnt - 1 : 0;
			clamped = 1;
			applied = 1;
		}
	}
	if (!applied)
		return;
	if (clamped)
		ticks = (uint32_t)((uint64_t)idledly * 1000 / units_ps);
	else
		ticks = (uint32_t)(((uint64_t)idledly * 1000 + units_ps - 1) / units_ps);
	/* rounding up must not reach the wait time either */
	if (!clamped && wa_idledly_short(e) &&
	    (uint64_t)ticks * units_ps >= (uint64_t)maxcnt * 1000) {
		idledly = maxcnt ? maxcnt - 1 : 0;
		ticks = (uint32_t)((uint64_t)idledly * 1000 / units_ps);
	}
	reg = (reg & ~XE2_IDLE_DELAY) | (ticks & XE2_IDLE_DELAY);
	i915_write32(i915, XE2_RING_IDLEDLY(e->mmio_base), reg);
}

/* Under the GuC the firmware schedules and owns the interrupt mask; the
 * driver still writes the settings, the cache-control tables, the
 * status page and the engine's mode (execution lists, running), and the
 * compute engines' share of the render slices. */
static void engine_hw_init_guc(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	i915_gt_apply_workarounds(i915, e);
	i915_mocs_init_engine(i915, e);
	i915_write32(i915, RING_HWSTAM(base), 0xffffffffu);
	i915_write32(i915, RING_HWS_PGA(base), e->hwsp_ggtt);
	i915_write32(i915, RING_MODE_GEN7(base), I915_MASKED_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));
	i915_write32(i915, RING_MI_MODE(base), I915_MASKED_DISABLE(STOP_RING));
	(void)i915_read32(i915, RING_MI_MODE(base));
	if (is_first_render_compute(e) && (i915->info->engine_mask & 0xf00)) {
		uint32_t rcu = GEN12_RCU_MODE_CCS_ENABLE;
		/* Xe2 and Xe3 with several compute engines: the slices are
		 * assigned by the CCS mode, not balanced */
		if (i915->ccs_mode && i915->gt_ip < I915_IP(35, 0))
			rcu |= XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE;
		i915_write32(i915, GEN12_RCU_MODE, I915_MASKED_ENABLE(rcu));
	}
	if (i915->gt_ip >= I915_IP(20, 0))
		adjust_idledly(e);
}

static int guc_owns_engines(struct i915_device *i915)
{
	return (i915->info->flags & I915_INFO_GUC_MANDATORY) && i915->guc.loaded;
}

static void engine_hw_init(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;

	if (guc_owns_engines(i915)) {
		engine_hw_init_guc(e);
		return;
	}

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
	/* Execution lists on.  Gen11 turned the bit around: the legacy
	 * ring buffer mode is switched off instead. */
	if (i915->info->gen >= 11)
		i915_write32(i915, RING_MODE_GEN7(base),
			     I915_MASKED_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));
	else
		i915_write32(i915, RING_MODE_GEN7(base),
			     I915_MASKED_ENABLE(GFX_RUN_LIST_ENABLE) |
				     I915_MASKED_DISABLE(GFX_REPLAY_MODE));
	(void)i915_read32(i915, RING_MODE_GEN7(base));
	/* The render and compute engines share their slices; the compute
	 * engines only get a share once the render engine's unit is told
	 * they exist.  Written with the first of them, since all of them
	 * are reset together. */
	if (is_first_render_compute(e) && (i915->info->engine_mask & 0xf00))
		i915_write32(i915, GEN12_RCU_MODE, I915_MASKED_ENABLE(GEN12_RCU_MODE_CCS_ENABLE));
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
		/* Under the GuC the context switches are the firmware's. */
		uint32_t bits = GT_RENDER_USER_INTERRUPT;
		if (!(i915->info->flags & I915_INFO_GUC_MANDATORY))
			bits |= GT_CONTEXT_SWITCH_INTERRUPT;
		/* enables per class pair; masks per engine pair (low/high).
		 * The media GT's engines report through the same registers. */
		i915_write32(i915, GEN11_RENDER_COPY_INTR_ENABLE, (bits << 16) | bits);
		i915_write32(i915, GEN11_VCS_VECS_INTR_ENABLE, (bits << 16) | bits);
		i915_write32(i915, GEN11_RCS0_RSVD_INTR_MASK, ~(bits << 16));
		i915_write32(i915, GEN11_BCS_RSVD_INTR_MASK, ~(bits << 16));
		i915_write32(i915, GEN11_VCS0_VCS1_INTR_MASK, ~((bits << 16) | bits));
		i915_write32(i915, GEN11_VCS2_VCS3_INTR_MASK, ~((bits << 16) | bits));
		i915_write32(i915, GEN11_VECS0_VECS1_INTR_MASK, ~((bits << 16) | bits));
		if (i915->info->engine_mask & 0xf00) {
			i915_write32(i915, GEN12_CCS_RSVD_INTR_ENABLE, bits << 16);
			i915_write32(i915, GEN12_CCS0_CCS1_INTR_MASK, ~((bits << 16) | bits));
			i915_write32(i915, GEN12_CCS2_CCS3_INTR_MASK, ~((bits << 16) | bits));
		}
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
/* One reset of the GT at `gsi'.  From Gen11 on the reset is asked twice:
 * on some parts the engines' registers are not cleared until shortly
 * after the reset reports done, and a second (no-op) reset waits that
 * out -- except on Meteor Lake on, where the request is costly and the
 * problem was not seen.  The state is given a moment to settle. */
static int gdrst(struct i915_device *i915, uint32_t gsi, uint32_t mask)
{
	int loops = (i915->info->gen >= 11 && i915->gt_ip < I915_IP(12, 70)) ? 2 : 1;
	int rc = 0;

	do {
		i915_write32_fw(i915, gsi + GEN6_GDRST, mask);
		rc = -ETIMEDOUT;
		for (int t = 0; t < 5000; t++) {
			if (!(i915_read32_fw(i915, gsi + GEN6_GDRST) & mask)) {
				rc = 0;
				break;
			}
			lapic_delay_us(10);
		}
	} while (rc == 0 && --loops);
	if (i915->info->gen >= 11)
		lapic_delay_us(50);
	return rc;
}

int i915_gt_reset_all(struct i915_device *i915)
{
	uint32_t mask = GEN6_GRDOM_FULL;

	if (i915_is_legacy(i915))
		return i915_legacy_gt_reset_all(i915);
	i915_fw_get(i915, I915_FW_ALL);
	/* Wa_22011100796: DG2 resets every engine on its own first (best
	 * effort), then the whole GT */
	if (i915->info->platform == I915_PLATFORM_DG2)
		(void)gdrst(i915, 0, GEN11_GRDOM_RENDER | GEN11_GRDOM_BLT | GEN11_GRDOM_MEDIA |
					     GEN11_GRDOM_MEDIA3 | GEN11_GRDOM_VECS | GEN11_GRDOM_VECS2);
	int rc = gdrst(i915, 0, mask);
	/* the media GT resets on its own */
	if (!rc && i915->has_media_gt)
		rc = gdrst(i915, I915_MEDIA_GT_BASE, GEN11_GRDOM_FULL);
	i915_fw_put(i915, I915_FW_ALL);
	if (rc)
		kprintf("[drm] i915: full GPU reset did not complete\n");
	return rc;
}

/* What the GT needs before any engine runs, after every GT reset: the
 * page attribute table, the GT-wide settings, the cache-control tables
 * shared by all engines, and the clock. */
/* Wa_16023588340 (Battlemage): the host's accesses of local memory go
 * through the L2; class of service 3 is given all of it (the page
 * attribute table leaves that class unused). */
static void xe2_enable_host_l2_vram(struct i915_device *i915)
{
	if (i915->gt_ip != I915_IP(20, 1))
		return;
	i915_mcr_write(i915, XE2_GAMREQSTRM_CTRL,
		       i915_mcr_read(i915, XE2_GAMREQSTRM_CTRL) | XE2_CG_DIS_CNTLBUS);
	i915_mcr_write(i915, XE2_L3CLOS_MASK(3), 0xf);
	if (i915->has_media_gt)
		i915_mcr_write(i915, I915_MEDIA_GT_BASE + XE2_L3CLOS_MASK(3), 0xf);
}

/* Xe2 and Xe3 discrete: what the GT wrote through the L3 into a surface
 * the display is about to scan out is flushed to memory -- the transient
 * entries (Battlemage: the whole L2, Wa_16023588340).  Xe3P and the
 * integrated parts flush at the end of every submission already. */
void i915_gt_td_flush(struct i915_device *i915)
{
	uint32_t reg, bit;

	if (!(i915->info->flags & I915_INFO_IS_DGFX) || i915->gt_ip < I915_IP(20, 0) ||
	    i915->gt_ip >= I915_IP(35, 0))
		return;
	if (i915->gt_ip == I915_IP(20, 1)) {
		reg = XE2_GLOBAL_INVAL;
		bit = 0x1;
	} else {
		reg = XE2_TDF_CTRL;
		bit = XE2_TRANSIENT_FLUSH_REQUEST;
	}
	i915_fw_get(i915, I915_FW_GT);
	i915_write32(i915, reg, bit);
	for (int t = 0; t < 100; t++) { /* 1 ms; 150 us is the worst case */
		if (!(i915_read32(i915, reg) & bit))
			break;
		lapic_delay_us(10);
	}
	i915_fw_put(i915, I915_FW_GT);
}

void i915_gt_init_hw(struct i915_device *i915)
{
	i915_gt_reset_all(i915);
	i915_xe2_compute_fuses(i915);
	i915_mcr_program_defaults(i915);
	xe2_enable_host_l2_vram(i915);
	i915_ggtt_program_pat(i915);
	i915_gt_apply_gt_workarounds(i915);
	i915_mocs_init(i915);
}

int i915_engines_init(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	uint32_t engine_mask = info->engine_mask;
	int n = 0;

	/* before Broadwell: rings instead of execution lists */
	if (i915_is_legacy(i915))
		return i915_legacy_engines_init(i915);
	/* The second video engine belongs to the GT3 and GT4 parts of the
	 * Gen8/Gen9 core families, not to the platform as a whole. */
	int gt = (i915->id && i915->id->gt) ? i915->id->gt : info->gt;
	if (gt >= 3 && (info->platform == I915_PLATFORM_BROADWELL ||
			info->platform == I915_PLATFORM_SKYLAKE ||
			info->platform == I915_PLATFORM_KABYLAKE ||
			info->platform == I915_PLATFORM_COFFEELAKE))
		engine_mask |= I915_ENGINE_VCS1;
	engine_mask = apply_media_fuses(i915, engine_mask);
	engine_mask = apply_compute_fuses(i915, engine_mask);

	mm_memset(i915->engines, 0, sizeof(i915->engines));
	for (unsigned i = 0; i < sizeof(engine_descs) / sizeof(engine_descs[0]); i++) {
		const struct engine_desc *d = &engine_descs[i];
		if (!(engine_mask & d->mask))
			continue;
		uint32_t base = info->gen >= 11 ? d->base_gen11 : d->base_gen9;
		if (!base)
			continue;
		/* the media GT's engines run under its own GuC: without it
		 * they are left out */
		if (i915->has_media_gt && (d->class == 2 || d->class == 3) &&
		    (info->flags & I915_INFO_GUC_MANDATORY) && !i915->media_guc.loaded)
			continue;
		struct i915_engine *e = &i915->engines[d->id];
		e->i915 = i915;
		e->id = d->id;
		e->name = d->name;
		e->class = d->class;
		e->instance = d->instance;
		e->present = 1;
		e->mmio_base = base;
		/* Meteor Lake: the video engines belong to the media GT */
		e->gsi_offset = (i915->has_media_gt && (d->class == 2 || d->class == 3)) ?
					I915_MEDIA_GT_BASE :
					0;
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

	/* where the GuC is already running, the GT was set up before it
	 * was loaded, and a reset now would take the firmware down too */
	if (!guc_owns_engines(i915))
		i915_gt_init_hw(i915);
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
	if (i915->ccs_mode)
		i915_write32(i915, XEHP_CCS_MODE, i915_xe2_ccs_mode_value(i915));
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
	if (i915_is_legacy(i915))
		return i915_legacy_engines_resume(i915);
	/* where the GuC is already running, the GT was set up before it
	 * was loaded, and a reset now would take the firmware down too */
	if (!guc_owns_engines(i915))
		i915_gt_init_hw(i915);
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
	if (i915->ccs_mode)
		i915_write32(i915, XEHP_CCS_MODE, i915_xe2_ccs_mode_value(i915));
	gt_irq_enable(i915);
	return 0;
}

void i915_engines_fini(struct i915_device *i915)
{
	if (i915_is_legacy(i915)) {
		i915_legacy_engines_fini(i915);
		return;
	}
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

static uint32_t hwsp_seqno(struct i915_engine *e)
{
	return i915_hwsp_read(e, I915_HWS_SEQNO_INDEX);
}

/* Interrupt context.  Nothing here may free: the last reference to a
 * request is the last reference to its objects and often to its
 * context, and dropping those unbinds and frees pages, frees kernel
 * memory (which may unmap it and wait for every other processor to
 * drop its translations) and, under the GuC, talks to the firmware --
 * none of which the code this interrupt cut into can be assumed to
 * allow.  The fences are signalled here, so that waiters wake at once;
 * the requests themselves are retired by the GT worker, and by every
 * waiter that polls. */
void i915_engine_irq(struct i915_engine *e, uint32_t bits)
{
	struct i915_device *i915 = e->i915;

	if ((bits & GT_CONTEXT_SWITCH_INTERRUPT) && !i915->guc.submission)
		i915_execlists_process_csb_irq(e);
	if (bits & GT_RENDER_USER_INTERRUPT) {
		e->user_irqs++;
		uint32_t passed = hwsp_seqno(e);
		if ((int32_t)(passed - e->last_retired) > 0) {
			drm_fence_signal_upto_ctx(&i915->drm, e->fence_context, passed);
			poll_notify_wq(&e->wq);
		}
	}
	i915->hangcheck_pending = 1;
	if (i915->gt_worker_ready)
		poll_notify_wq(&i915->gt_wq);
}

uint32_t i915_hwsp_read(struct i915_engine *e, unsigned index)
{
	const volatile uint32_t *p = &e->hwsp[index];
	if (!(e->i915->info->flags & I915_INFO_HAS_LLC))
		__asm__ volatile("clflush (%0)" ::"r"(p) : "memory");
	return *p;
}

/* Requests whose sequence the engine has passed: their fences signal,
 * their objects are released.  From the GT worker and from waiters --
 * thread context only (see i915_engine_irq). */
void i915_engine_retire(struct i915_engine *e)
{
	uint32_t passed = hwsp_seqno(e);
	struct i915_request *done = NULL, **dp = &done;
	uint64_t fl;

	if ((int32_t)(passed - e->last_retired) <= 0)
		return;
	spin_lock_irqsave(&e->lock, &fl);
	if ((int32_t)(passed - e->last_retired) > 0)
		e->last_retired = passed; /* never back: a slower retirer may come second */
	while (e->inflight && (int32_t)(passed - e->inflight->seqno) >= 0) {
		struct i915_request *rq = e->inflight;
		e->inflight = rq->next;
		if (!e->inflight)
			e->inflight_tail = NULL;
		rq->next = NULL;
		*dp = rq;
		dp = &rq->next;
	}
	/* under the GuC one logical ring is in flight at a time
	 * (i915_guc_submit): the next one goes once this one is done */
	if (e->i915->guc.submission && !e->inflight && e->queue)
		i915_guc_submit(e);
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

/* A fence waited for for seconds: which engine it belongs to and where
 * that engine is -- the sequence the engine wrote, the request the fence
 * stands for (queued, or with the hardware), the request at the head of
 * the engine and its context, and whether interrupts arrive at all. */
void i915_engine_report_stuck(struct i915_device *i915, uint64_t context, uint64_t seqno)
{
	struct i915_engine *e = NULL;
	uint64_t fl;

	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present && i915->engines[i].fence_context == context)
			e = &i915->engines[i];
	if (!e) {
		kprintf("[drm] i915:   the fence is not an engine's (stream %llx)\n",
			(unsigned long long)context);
		return;
	}
	uint32_t hw = i915_is_legacy(i915) ? 0 : hwsp_seqno(e);
	const char *where = "retired";
	uint32_t head = 0, head_id = 0, nq = 0, nf = 0;
	uint32_t head_file = 0, head_ctx = 0;
	spin_lock_irqsave(&e->lock, &fl);
	for (struct i915_request *rq = e->inflight; rq; rq = rq->next) {
		nf++;
		if (rq->seqno == (uint32_t)seqno)
			where = "with the hardware";
	}
	for (struct i915_request *rq = e->queue; rq; rq = rq->next) {
		nq++;
		if (rq->seqno == (uint32_t)seqno)
			where = "queued, not yet handed to the hardware";
	}
	if (e->inflight) {
		head = e->inflight->seqno;
		head_id = e->inflight->lrc ? e->inflight->lrc->guc_id : 0;
		if (e->inflight->ctx) {
			head_ctx = e->inflight->ctx->id;
			head_file = e->inflight->ctx->fp ? e->inflight->ctx->fp->file_id : 0;
		}
	}
	spin_unlock_irqrestore(&e->lock, fl);
	kprintf("[drm] i915:   %s: fence seqno %u is %s; engine wrote %u, retired %u, last issued %u; %u in flight (head %u, GuC id %u, context %u of file %u), %u queued\n",
		e->name, (uint32_t)seqno, where, hw, e->last_retired, e->next_seqno - 1, nf, head,
		head_id, head_ctx, head_file, nq);
	kprintf("[drm] i915:   %s: %u user interrupts, %llu interrupts in all (vector %d, last master %08x)%s\n",
		e->name, e->user_irqs, (unsigned long long)i915->irq_count, i915->irq_vector,
		i915->irq_last_master,
		i915->guc.submission ? (e->gsi_offset ? (i915->media_guc.ct_broken ? ", media GuC transport broken" : ", under the media GuC") :
						     (i915->guc.ct_broken ? ", GuC transport broken" : ", under the GuC")) :
				       "");
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
	if (e->i915->info->gen >= 11) {
		switch (e->id) {
		case I915_RCS0: return GEN11_GRDOM_RENDER;
		case I915_BCS0: return GEN11_GRDOM_BLT;
		case I915_VCS0: return GEN11_GRDOM_MEDIA;
		case I915_VCS1: return GEN11_GRDOM_MEDIA2;
		case I915_VCS2: return GEN11_GRDOM_MEDIA3;
		case I915_VCS3: return GEN11_GRDOM_MEDIA4;
		case I915_VECS0: return GEN11_GRDOM_VECS;
		case I915_VECS1: return GEN11_GRDOM_VECS2;
		/* the compute engines reset with the render engine */
		default: return GEN11_GRDOM_RENDER;
		}
	}
	switch (e->id) {
	case I915_RCS0: return GEN6_GRDOM_RENDER;
	case I915_BCS0: return GEN6_GRDOM_BLT;
	case I915_VCS0: return GEN6_GRDOM_MEDIA;
	case I915_VCS1: return GEN8_GRDOM_MEDIA2;
	case I915_VECS0: return GEN6_GRDOM_VECS;
	default: return GEN6_GRDOM_FULL;
	}
}

static void wait_pending_mi_fw(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t reg;

	switch (e->id) {
	case I915_RCS0:
	case I915_CCS0:
	case I915_CCS1:
	case I915_CCS2:
	case I915_CCS3: reg = MSG_IDLE_CS; break;
	case I915_BCS0: reg = MSG_IDLE_BCS; break;
	case I915_VCS0: reg = MSG_IDLE_VCS0; break;
	case I915_VCS1: reg = MSG_IDLE_VCS1; break;
	case I915_VCS2: reg = MSG_IDLE_VCS2; break;
	case I915_VCS3: reg = MSG_IDLE_VCS3; break;
	case I915_VECS0: reg = MSG_IDLE_VECS0; break;
	case I915_VECS1: reg = MSG_IDLE_VECS1; break;
	default: return;
	}
	uint32_t v = i915_read32(i915, reg);
	uint32_t pending = (v & (v >> 16) & MSG_IDLE_FW_MASK) >> MSG_IDLE_FW_SHIFT;
	if (!pending)
		return;
	lapic_delay_us(1);
	for (int t = 0; t < 500; t++) { /* 5 ms */
		if ((i915_read32(i915, GEN9_PWRGT_DOMAIN_STATUS) & pending) == pending)
			break;
		lapic_delay_us(10);
	}
	lapic_delay_us(1);
}

/* Ask the engine to get ready for its reset.  An engine stopped by a
 * catastrophic error cannot answer; the request is skipped for it and
 * the hardware clears the error itself. */
static void engine_reset_prepare(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	uint32_t reg = RING_RESET_CTL(e->mmio_base);
	uint32_t ack = i915_read32(i915, reg);
	uint32_t request, mask, want;

	if (ack & RESET_CTL_CAT_ERROR) {
		request = mask = RESET_CTL_CAT_ERROR;
		want = 0;
	} else if (!(ack & RESET_CTL_READY_TO_RESET)) {
		request = RESET_CTL_REQUEST_RESET;
		mask = want = RESET_CTL_READY_TO_RESET;
	} else {
		return;
	}
	i915_write32(i915, reg, I915_MASKED_ENABLE(request));
	for (int t = 0; t < 70; t++) {
		if ((i915_read32(i915, reg) & mask) == want)
			return;
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: %s: reset request timed out (%08x)\n", e->name, i915_read32(i915, reg));
}

/* The scaler and format converter a video engine (or the enhancement
 * engine paired with it) is using is reset with it: it is locked to the
 * engine first, and its reset bit joins the engine's.  On Gen12 a video
 * engine whose HCP unit holds the scaler of its enhancement neighbour
 * locks that one (Wa_14010733141).  Returns the engine whose lock was
 * taken, to be released after the reset. */
struct sfc_regs {
	uint32_t lock_reg, lock_bit, ack_reg, ack_bit, usage_reg, usage_bit, reset_bit;
};

static void sfc_regs_of(const struct i915_engine *e, struct sfc_regs *r)
{
	uint32_t base = e->mmio_base;

	if (e->class == 2) {
		r->lock_reg = GEN11_VCS_SFC_FORCED_LOCK(base);
		r->lock_bit = GEN11_VCS_SFC_FORCED_LOCK_BIT;
		r->ack_reg = GEN11_VCS_SFC_LOCK_STATUS(base);
		r->ack_bit = GEN11_VCS_SFC_LOCK_ACK_BIT;
		r->usage_reg = GEN11_VCS_SFC_LOCK_STATUS(base);
		r->usage_bit = GEN11_VCS_SFC_USAGE_BIT;
		r->reset_bit = GEN11_VCS_SFC_RESET_BIT(e->instance);
	} else {
		r->lock_reg = GEN11_VECS_SFC_FORCED_LOCK(base);
		r->lock_bit = GEN11_VECS_SFC_FORCED_LOCK_BIT;
		r->ack_reg = GEN11_VECS_SFC_LOCK_ACK(base);
		r->ack_bit = GEN11_VECS_SFC_LOCK_ACK_BIT;
		r->usage_reg = GEN11_VECS_SFC_USAGE(base);
		r->usage_bit = GEN11_VECS_SFC_USAGE_BIT;
		r->reset_bit = GEN11_VECS_SFC_RESET_BIT(e->instance);
	}
}

static struct i915_engine *sfc_lock(struct i915_engine *e, uint32_t *reset_mask)
{
	struct i915_device *i915 = e->i915;
	struct i915_engine *owner = e;
	struct sfc_regs r;
	int to_other = 0;

	if (e->class == 2) {
		if (!(i915->vdbox_sfc_access & (1u << e->instance)))
			return NULL;
	} else if (e->class != 3) {
		return NULL;
	}
	sfc_regs_of(e, &r);
	if (!(i915_read32(i915, r.usage_reg) & r.usage_bit)) {
		if (e->class != 2 || i915->info->gen != 12)
			return NULL;
		if (!(i915_read32(i915, GEN12_HCP_SFC_LOCK_STATUS(e->mmio_base)) &
		      GEN12_HCP_SFC_USAGE_BIT))
			return NULL;
		owner = i915_engine_by_id(i915, I915_VECS0 + e->instance / 2);
		if (!owner)
			return NULL;
		sfc_regs_of(owner, &r);
		to_other = 1;
	}
	i915_write32(i915, r.lock_reg, i915_read32(i915, r.lock_reg) | r.lock_bit);
	int acked = 0;
	for (int t = 0; t < 100; t++) {
		if (i915_read32(i915, r.ack_reg) & r.ack_bit) {
			acked = 1;
			break;
		}
		lapic_delay_us(10);
	}
	/* Reset the scaler with the engine if it is now locked to this
	 * engine, or if it was let go while being locked to the other. */
	int obtained = !!(i915_read32(i915, r.usage_reg) & r.usage_bit);
	if (obtained != to_other && acked)
		*reset_mask |= r.reset_bit;
	return owner;
}

static void sfc_unlock(struct i915_engine *e)
{
	struct sfc_regs r;

	sfc_regs_of(e, &r);
	i915_write32(e->i915, r.lock_reg, i915_read32(e->i915, r.lock_reg) & ~r.lock_bit);
}

int i915_engine_reset(struct i915_engine *e, const char *why)
{
	struct i915_device *i915 = e->i915;
	uint32_t base = e->mmio_base;
	uint64_t fl;

	if (i915_is_legacy(i915))
		return i915_legacy_engine_reset(e, why);
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
	/* Wa_22011802037: from Gen11 to before Meteor Lake (and its first
	 * stepping) the command streamer is stopped first, and any
	 * forcewake it asked for itself allowed to complete */
	if (i915->info->gen >= 11 &&
	    (i915->gt_ip < I915_IP(12, 70) ||
	     (i915->gt_ip == I915_IP(12, 70) && i915->gt_step < I915_STEP_B0))) {
		engine_stop(e);
		wait_pending_mi_fw(e);
	}
	/* ask, wait for ready, reset the domain, release */
	engine_reset_prepare(e);
	uint32_t dom = engine_reset_domain(e);
	struct i915_engine *sfc_locked = NULL;
	if (i915->info->gen >= 11)
		sfc_locked = sfc_lock(e, &dom);
	if (i915->info->gen >= 11) {
		(void)gdrst(i915, e->gsi_offset, dom);
	} else {
		i915_write32_fw(i915, GEN6_GDRST, dom);
		for (int t = 0; t < 5000; t++) {
			if (!(i915_read32_fw(i915, GEN6_GDRST) & dom))
				break;
			lapic_delay_us(10);
		}
	}
	if (sfc_locked)
		sfc_unlock(sfc_locked);
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
		if (e->resident[i] && e->nstale < (int)(sizeof(e->stale) / sizeof(e->stale[0])))
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
				/* the engine was stopped before the reset: a
				 * context saved meanwhile carries the stop bit */
				if (i915->info->gen_x10 >= 120 && l.mi_mode) {
					lrc->regs[l.mi_mode + 1] &= ~(uint32_t)STOP_RING;
					lrc->regs[l.mi_mode + 1] |= (uint32_t)STOP_RING << 16;
				}
			}
			lrc->ring.head = lrc->ring.tail = 0;
		}
		drm_fence_signal(rq->fence);
		i915_request_put(rq);
	}
	e->hwsp[I915_HWS_SEQNO_INDEX] = e->next_seqno - 1;
	__asm__ volatile("clflush (%0)" ::"r"(&e->hwsp[I915_HWS_SEQNO_INDEX]) : "memory");
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
	struct i915_gem_context *gone[sizeof(e->stale) / sizeof(e->stale[0])];
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
