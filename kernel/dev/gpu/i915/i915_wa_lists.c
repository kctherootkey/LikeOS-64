// LikeOS -- the workarounds and tuning settings of Gen12 and later graphics.
//
// Each list below is a set of register writes the hardware needs beyond
// its reset defaults: fixes for silicon bugs (the Wa_ numbers are Intel's
// names for them) and the performance guide's recommended settings.  The
// GT-wide list holds registers that keep their value across engine
// resets and are written once, after the GT is reset or powered up; the
// standalone media GT of Meteor Lake has a list of its own.  The engine
// list holds the registers of one engine and of the render domain, which
// an engine reset loses, so it is written at engine init and again after
// every reset.  The context list holds registers that are part of a
// render or compute context's saved state: they are emitted as register
// loads into the first batch a context runs, and the context carries them
// from then on.  The whitelist names the registers a client batch may
// write (or read) itself, programmed into the engine's FORCE_TO_NONPRIV
// slots, each entry carrying the access it grants.
//
// Xe2 and later parts (Lunar Lake, Battlemage, Panther Lake, Wildcat
// Lake, Nova Lake) have lists of their own at the end of this file; their
// rules are by IP version and stepping, those naming a graphics version
// applying to the primary GT and those naming a media version to the
// standalone media GT.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_wa_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>

/* The engine classes of the interface, as struct i915_engine has them. */
enum wa_engine_class {
	WA_RENDER = 0,
	WA_COPY = 1,
	WA_VIDEO_DECODE = 2,
	WA_VIDEO_ENHANCE = 3,
	WA_COMPUTE = 4,
};

#define WA_CCS_ENGINES \
	(I915_ENGINE_CCS0 | I915_ENGINE_CCS1 | I915_ENGINE_CCS2 | I915_ENGINE_CCS3)

/* ---- adding entries --------------------------------------------------------- */

static void wa_add(struct i915_wa_list *wal, uint32_t reg, uint32_t clear, uint32_t set,
		   uint32_t read_mask, int masked_reg)
{
	i915_wa_add(wal, reg, clear, set, read_mask, masked_reg ? I915_WA_MASKED : 0);
}

/* A register with one copy per slice, subslice or bank. */
static void wa_mcr_add(struct i915_wa_list *wal, uint32_t reg, uint32_t clear, uint32_t set,
		       uint32_t read_mask, int masked_reg)
{
	i915_wa_add(wal, reg, clear, set, read_mask,
		    (masked_reg ? I915_WA_MASKED : 0) | I915_WA_MCR);
}

static void wa_write_clr_set(struct i915_wa_list *wal, uint32_t reg, uint32_t clear,
			     uint32_t set)
{
	wa_add(wal, reg, clear, set, clear | set, 0);
}

static void wa_mcr_write_clr_set(struct i915_wa_list *wal, uint32_t reg, uint32_t clear,
				 uint32_t set)
{
	wa_mcr_add(wal, reg, clear, set, clear | set, 0);
}

static void wa_write_or(struct i915_wa_list *wal, uint32_t reg, uint32_t set)
{
	wa_write_clr_set(wal, reg, set, set);
}

static void wa_mcr_write_or(struct i915_wa_list *wal, uint32_t reg, uint32_t set)
{
	wa_mcr_write_clr_set(wal, reg, set, set);
}

static void wa_write_clr(struct i915_wa_list *wal, uint32_t reg, uint32_t clr)
{
	wa_write_clr_set(wal, reg, clr, 0);
}

static void wa_mcr_write_clr(struct i915_wa_list *wal, uint32_t reg, uint32_t clr)
{
	wa_mcr_write_clr_set(wal, reg, clr, 0);
}

/*
 * A "masked" register has its upper 16 bits documented as a write mask:
 * a write changes only the lower bits whose mask bit is set, so part of
 * the register is updated without reading it.  These helpers take the
 * lower 16 bits and compute the mask themselves.
 */
static void wa_masked_en(struct i915_wa_list *wal, uint32_t reg, uint32_t val)
{
	wa_add(wal, reg, 0, I915_MASKED_ENABLE(val), val, 1);
}

static void wa_mcr_masked_en(struct i915_wa_list *wal, uint32_t reg, uint32_t val)
{
	wa_mcr_add(wal, reg, 0, I915_MASKED_ENABLE(val), val, 1);
}

static void wa_masked_dis(struct i915_wa_list *wal, uint32_t reg, uint32_t val)
{
	wa_add(wal, reg, 0, I915_MASKED_DISABLE(val), val, 1);
}

static void wa_mcr_masked_dis(struct i915_wa_list *wal, uint32_t reg, uint32_t val)
{
	wa_mcr_add(wal, reg, 0, I915_MASKED_DISABLE(val), val, 1);
}

static void wa_masked_field_set(struct i915_wa_list *wal, uint32_t reg, uint32_t mask,
				uint32_t val)
{
	wa_add(wal, reg, 0, (mask << 16) | val, mask, 1);
}

static void wa_mcr_masked_field_set(struct i915_wa_list *wal, uint32_t reg, uint32_t mask,
				    uint32_t val)
{
	wa_mcr_add(wal, reg, 0, (mask << 16) | val, mask, 1);
}

/* ---- what the part is -------------------------------------------------------- */

static int is_platform(const struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

static int is_subplatform(const struct i915_device *i915, int subplatform)
{
	return i915->subplatform == subplatform;
}

/* Graphics version 12 (12.0 through 12.74). */
static int gt_ver_12(const struct i915_device *i915)
{
	return i915->gt_ip >= I915_IP(12, 0) && i915->gt_ip < I915_IP(13, 0);
}

/* The graphics IP is within [from, until], both inclusive. */
static int gt_ip_range(const struct i915_device *i915, unsigned from, unsigned until)
{
	return i915->gt_ip >= from && i915->gt_ip <= until;
}

/* The graphics IP is `ip' and its stepping within [from, until). */
static int gt_ip_step(const struct i915_device *i915, unsigned ip, unsigned from,
		      unsigned until)
{
	return i915->gt_ip == ip && i915->gt_step >= from && i915->gt_step < until;
}

/* Xe_LPG: Meteor Lake (12.70, 12.71) and Arrow Lake (12.74). */
static int is_xelpg(const struct i915_device *i915)
{
	return gt_ip_range(i915, I915_IP(12, 70), I915_IP(12, 74));
}

/* The A steppings of Xe_LPG 12.70 and 12.71. */
static int xelpg_a_step(const struct i915_device *i915)
{
	return gt_ip_step(i915, I915_IP(12, 70), I915_STEP_A0, I915_STEP_B0) ||
	       gt_ip_step(i915, I915_IP(12, 71), I915_STEP_A0, I915_STEP_B0);
}

/* The lists here are for graphics 12.0 up to, not including, Xe2. */
static int wa_lists_cover(const struct i915_device *i915)
{
	return i915->gt_ip >= I915_IP(12, 0) && i915->gt_ip < I915_IP(20, 0);
}

/*
 * The engine's register base.  A standalone media GT relocates only its
 * registers below 0x40000; its video engines' registers sit at their
 * usual offsets, so the base is used as it is.
 */
static uint32_t engine_base(const struct i915_engine *e)
{
	return e->mmio_base;
}

/*
 * The register base of video decode engine `instance' on the primary GT
 * (media_gt 0) or the standalone media GT (media_gt 1), or 0 when the
 * part has no such engine there.  Once the engines are set up they say
 * which exist; before that the descriptor does.
 */
static uint32_t vcs_gt_base(const struct i915_device *i915, unsigned instance, int media_gt)
{
	static const uint32_t bases[4] = { GEN11_BSD_RING_BASE, GEN11_BSD2_RING_BASE,
					   GEN11_BSD3_RING_BASE, GEN11_BSD4_RING_BASE };
	const struct i915_engine *e = &i915->engines[I915_VCS0 + instance];

	/* With a standalone media GT the video engines are all on it. */
	if (!!i915->has_media_gt != !!media_gt)
		return 0;
	if (i915->nengines)
		return e->present ? engine_base(e) : 0;
	if (!(i915->info->engine_mask & (I915_ENGINE_VCS0 << instance)))
		return 0;
	return bases[instance];
}

/*
 * The engine that carries the render domain's shared registers: those
 * not tied to one engine, lost when the render and compute engines are
 * reset together.  The render engine if the part has one, otherwise the
 * compute engine of the lowest instance.
 */
static int first_render_compute(const struct i915_device *i915, const struct i915_engine *e)
{
	uint32_t mask = i915->info->engine_mask;
	uint32_t ccs = (mask & WA_CCS_ENGINES) / I915_ENGINE_CCS0;

	if (e->gsi_offset)
		return 0;
	if (mask & I915_ENGINE_RCS0)
		return e->class == WA_RENDER && e->instance == 0;
	return e->class == WA_COMPUTE && ccs && e->instance == __builtin_ctz(ccs);
}

/* ---- the GT-wide list ------------------------------------------------------------ */

static void __set_mcr_steering(struct i915_wa_list *wal, uint32_t steering_reg,
			       unsigned slice, unsigned subslice)
{
	uint32_t mcr, mcr_mask;

	mcr = GEN11_MCR_SLICE(slice) | GEN11_MCR_SUBSLICE(subslice);
	mcr_mask = GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK;

	wa_write_clr_set(wal, steering_reg, mcr_mask, mcr);
}

static void icl_wa_init_mcr(struct i915_device *i915, struct i915_wa_list *wal)
{
	uint32_t ss = i915->subslice_mask[0];
	unsigned subslice;

	/*
	 * Although a part may have several subslices, reads must always be
	 * steered at the lowest one that isn't fused off.  With render power
	 * gating on, taking forcewake powers up only a single subslice (the
	 * "minconfig") when there is no real workload, and a read steered at
	 * a higher one risks returning zeroes or garbage.
	 */
	subslice = ss ? (unsigned)__builtin_ctz(ss) : 0;

	__set_mcr_steering(wal, GEN8_MCR_SELECTOR, 0, subslice);
}

/*
 * Though there are per-engine instances of these registers, they keep
 * their value through engine resets and so belong on the GT list rather
 * than on an engine's.
 */
static void wa_14011060649(struct i915_device *i915, struct i915_wa_list *wal)
{
	for (unsigned instance = 0; instance < 4; instance += 2) {
		uint32_t base = vcs_gt_base(i915, instance, 0);

		if (base)
			wa_write_or(wal, VDBOX_CGCTL3F10(base), IECPUNIT_CLKGATE_DIS);
	}
}

static void gen12_gt_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	icl_wa_init_mcr(i915, wal);

	/* Wa_14011060649:tgl,rkl,dg1,adl-s,adl-p */
	wa_14011060649(i915, wal);

	/* Wa_14011059788:tgl,rkl,adl-s,dg1,adl-p */
	wa_mcr_write_or(wal, GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);

	/*
	 * Wa_14015795083
	 *
	 * The firmware of some Gen12 parts locks MISCCPCTL, so the write may
	 * not stick; it is not read back to check.
	 */
	wa_add(wal, GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0, 0, 0);
}

static void dg1_gt_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	gen12_gt_workarounds_init(i915, wal);

	/* Wa_1409420604:dg1 */
	wa_mcr_write_or(wal, SUBSLICE_UNIT_LEVEL_CLKGATE2, CPSSUNIT_CLKGATE_DIS);

	/* Wa_1408615072:dg1 */
	/* Empirically, this register is unaffected by an engine reset. */
	wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE2, VSUNIT_CLKGATE_DIS_TGL);
}

static void dg2_gt_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	/*
	 * The default steering of the multicast registers (the slice and
	 * DSS in GEN8_MCR_SELECTOR, and the MCFG, SF and GAM selectors) is
	 * not part of this list: i915_mcr.c steers each access itself.
	 */

	/* Wa_14011060649:dg2 */
	wa_14011060649(i915, wal);

	if (is_subplatform(i915, I915_SUBPLATFORM_DG2_G10)) {
		/* Wa_22010523718:dg2 */
		wa_write_or(wal, UNSLICE_UNIT_LEVEL_CLKGATE, CG3DDISCFEG_CLKGATE_DIS);

		/* Wa_14011006942:dg2 */
		wa_mcr_write_or(wal, GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE, DSS_ROUTER_CLKGATE_DIS);
	}

	/* Wa_14014830051:dg2 */
	wa_mcr_write_clr(wal, SARB_CHICKEN1, COMP_CKN_IN);

	/*
	 * Wa_14015795083
	 * The register may be locked; the write is not read back to check.
	 */
	wa_add(wal, GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0, 0, 0);

	/* Wa_18018781329 */
	wa_mcr_write_or(wal, RENDER_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, COMP_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, XEHP_VDBX_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, XEHP_VEBX_MOD_CTRL, FORCE_MISS_FTLB);

	/* Wa_1509235366:dg2 */
	wa_mcr_write_or(wal, XEHP_GAMCNTRL_CTRL,
			INVALIDATION_BROADCAST_MODE_DIS | GLOBAL_INVALIDATION_MODE);

	/* Wa_14010648519:dg2 */
	wa_mcr_write_or(wal, XEHP_L3NODEARBCFG, XEHP_LNESPARE);
}

static void xelpg_gt_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	/* Wa_14018575942 / Wa_18018781329 */
	wa_mcr_write_or(wal, RENDER_MOD_CTRL, FORCE_MISS_FTLB);
	wa_mcr_write_or(wal, COMP_MOD_CTRL, FORCE_MISS_FTLB);

	/* Wa_22016670082 */
	wa_write_or(wal, GEN12_SQCNT1, GEN12_STRICT_RAR_ENABLE);

	if (xelpg_a_step(i915)) {
		/* Wa_14014830051 */
		wa_mcr_write_clr(wal, SARB_CHICKEN1, COMP_CKN_IN);

		/* Wa_14015795083 */
		wa_write_clr(wal, GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE);
	}

	/*
	 * Unlike older parts, no default steering is set up here: every
	 * multicast register access is steered explicitly.
	 */
}

static void wa_16021867713(struct i915_device *i915, struct i915_wa_list *wal)
{
	for (unsigned instance = 0; instance < 4; instance++) {
		uint32_t base = vcs_gt_base(i915, instance, 1);

		if (base)
			wa_write_or(wal, VDBOX_CGCTL3F1C(base), MFXPIPE_CLKGATE_DIS);
	}
}

static void xelpmp_gt_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	wa_16021867713(i915, wal);

	/*
	 * Wa_14018778641
	 * Wa_18018781329
	 *
	 * These registers are multicast on the primary GT, but the media
	 * GT's copies are ordinary single registers.
	 */
	wa_write_or(wal, XELPMP_GSC_MOD_CTRL, FORCE_MISS_FTLB);

	/*
	 * Wa_14018575942
	 *
	 * The issue shows up with media workloads on the video decode
	 * engines, VP9 encoding in particular.
	 */
	wa_write_or(wal, XELPMP_VDBX_MOD_CTRL, FORCE_MISS_FTLB);

	/* Wa_22016670082 */
	wa_write_or(wal, GEN12_SQCNT1, GEN12_STRICT_RAR_ENABLE);
}

/*
 * The performance guide's recommended register settings.  They are not
 * workarounds, but are programmed the same way so that they are written
 * again whenever they could have been lost.  The ones here survive engine
 * resets and are not part of any engine's context, so they only need
 * writing again after a full GT reset.
 */
static void gt_tuning_settings(struct i915_device *i915, struct i915_wa_list *wal)
{
	if (is_xelpg(i915)) {
		wa_mcr_write_or(wal, XEHP_L3SCQREG7, BLEND_FILL_CACHING_OPT_DIS);
		wa_mcr_write_or(wal, XEHP_SQCM, EN_32B_ACCESS);
	}

	if (is_platform(i915, I915_PLATFORM_DG2)) {
		wa_mcr_write_or(wal, XEHP_L3SCQREG7, BLEND_FILL_CACHING_OPT_DIS);
		wa_mcr_write_or(wal, XEHP_SQCM, EN_32B_ACCESS);
	}
}

static void xe2_gt_list(struct i915_device *i915, int media_gt, struct i915_wa_list *wal);
static void xe2_engine_list(struct i915_device *i915, const struct i915_engine *e,
			    struct i915_wa_list *wal);
static void xe2_ctx_list(struct i915_device *i915, const struct i915_engine *e,
			 struct i915_wa_list *wal);
static void xe2_whitelist(struct i915_device *i915, const struct i915_engine *e,
			  struct i915_wa_list *wal);

void i915_wa_gt_list(struct i915_device *i915, int media_gt, struct i915_wa_list *wal)
{
	if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_gt_list(i915, media_gt, wal);
		return;
	}
	if (!wa_lists_cover(i915))
		return;

	if (media_gt) {
		/* The tuning settings above are all for the primary GT. */
		if (i915->has_media_gt && i915->media_ip == I915_IP(13, 0))
			xelpmp_gt_workarounds_init(i915, wal);
		return;
	}

	gt_tuning_settings(i915, wal);

	if (is_xelpg(i915))
		xelpg_gt_workarounds_init(i915, wal);
	else if (is_platform(i915, I915_PLATFORM_DG2))
		dg2_gt_workarounds_init(i915, wal);
	else if (is_platform(i915, I915_PLATFORM_DG1))
		dg1_gt_workarounds_init(i915, wal);
	else if (gt_ver_12(i915))
		gen12_gt_workarounds_init(i915, wal);
}

/* ---- the context list ------------------------------------------------------------- */

/*
 * Not workarounds but tuning settings DG2 (and Xe_LPG after it) should
 * have programmed.
 */
static void dg2_ctx_gt_tuning_init(struct i915_wa_list *wal)
{
	wa_mcr_masked_en(wal, CHICKEN_RASTER_2, TBIMR_FAST_CLIP);
	wa_mcr_write_clr_set(wal, XEHP_L3SQCREG5, L3_PWM_TIMER_INIT_VAL_MASK,
			     0x7f & L3_PWM_TIMER_INIT_VAL_MASK);
	wa_mcr_write_clr_set(wal, XEHP_FF_MODE2, FF_MODE2_TDS_TIMER_MASK, FF_MODE2_TDS_TIMER_128);
}

static void gen12_ctx_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	/*
	 * Wa_1409142259:tgl,dg1,adl-p,adl-n
	 * Wa_1409347922:tgl,dg1,adl-p
	 * Wa_1409252684:tgl,dg1,adl-p
	 * Wa_1409217633:tgl,dg1,adl-p
	 * Wa_1409207793:tgl,dg1,adl-p
	 * Wa_1409178076:tgl,dg1,adl-p,adl-n
	 * Wa_1408979724:tgl,dg1,adl-p,adl-n
	 * Wa_14010443199:tgl,rkl,dg1,adl-p,adl-n
	 * Wa_14010698770:tgl,rkl,dg1,adl-s,adl-p,adl-n
	 * Wa_1409342910:tgl,rkl,dg1,adl-s,adl-p,adl-n
	 * Wa_22010465259:tgl,rkl,dg1,adl-s,adl-p,adl-n
	 */
	wa_masked_en(wal, GEN11_COMMON_SLICE_CHICKEN3, GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);

	/* WaDisableGPGPUMidThreadPreemption:gen12 */
	wa_masked_field_set(wal, GEN8_CS_CHICKEN1(RENDER_RING_BASE),
			    GEN9_PREEMPT_GPGPU_LEVEL_MASK, GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);

	/*
	 * Wa_16011163337 - GS_TIMER
	 *
	 * TDS_TIMER: some parts call it Wa_1604555607, but it must be
	 * programmed on the ones that don't list that workaround as well.
	 *
	 * FF_MODE2 is further subject to Wa_1608008084: the register returns
	 * the wrong value when the CPU reads it.  Every field defaults to
	 * zero, so rather than a read-modify-write the TDS and GS timers are
	 * written outright: the clear mask is all ones so that no other bit
	 * is set by accident, and the value is not read back to check.
	 */
	wa_add(wal, GEN12_FF_MODE2, ~0u, FF_MODE2_TDS_TIMER_128 | FF_MODE2_GS_TIMER_224, 0, 0);

	if (!is_platform(i915, I915_PLATFORM_DG1)) {
		/* Wa_1806527549 */
		wa_masked_en(wal, HIZ_CHICKEN, HZ_DEPTH_TEST_LE_GE_OPT_DISABLE);

		/* Wa_1606376872 */
		wa_masked_en(wal, COMMON_SLICE_CHICKEN4, DISABLE_TDC_LOAD_BALANCING_CALC);
	}

	/* This bit enables the performance optimisation for fast clears. */
	wa_mcr_write_or(wal, GEN8_WM_CHICKEN2, WAIT_ON_DEPTH_STALL_DONE_DISABLE);
}

static void dg1_ctx_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	gen12_ctx_workarounds_init(i915, wal);

	/* Wa_1409044764 */
	wa_masked_dis(wal, GEN11_COMMON_SLICE_CHICKEN3, DG1_FLOAT_POINT_BLEND_OPT_STRICT_MODE_EN);

	/* Wa_22010493298 */
	wa_masked_en(wal, HIZ_CHICKEN, DG1_HZ_READ_SUPPRESSION_OPTIMIZATION_DISABLE);
}

static void dg2_ctx_workarounds_init(struct i915_wa_list *wal)
{
	dg2_ctx_gt_tuning_init(wal);

	/* Wa_16013271637:dg2 */
	wa_mcr_masked_en(wal, XEHP_SLICE_COMMON_ECO_CHICKEN1, MSC_MSAA_REODER_BUF_BYPASS_DISABLE);

	/* Wa_14014947963:dg2 */
	wa_masked_field_set(wal, VF_PREEMPTION, PREEMPTION_VERTEX_COUNT, 0x4000);

	/* Wa_18018764978:dg2 */
	wa_mcr_masked_en(wal, XEHP_PSS_MODE2, SCOREBOARD_STALL_FLUSH_CONTROL);

	/* Wa_18019271663:dg2 */
	wa_masked_en(wal, CACHE_MODE_1, MSAA_OPTIMIZATION_REDUC_DISABLE);

	/* Wa_14019877138:dg2 */
	wa_mcr_masked_en(wal, XEHP_PSS_CHICKEN, FD_END_COLLECT);
}

static void xelpg_ctx_gt_tuning_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	dg2_ctx_gt_tuning_init(wal);

	/*
	 * Because of Wa_16014892111, the DRAW_WATERMARK tuning of some early
	 * steppings is done in the indirect context batch instead.
	 */
	if (!xelpg_a_step(i915))
		wa_add(wal, DRAW_WATERMARK, VERT_WM_VAL, 0x3FF, 0, 0);
}

static void xelpg_ctx_workarounds_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	xelpg_ctx_gt_tuning_init(i915, wal);

	if (xelpg_a_step(i915)) {
		/* Wa_14014947963 */
		wa_masked_field_set(wal, VF_PREEMPTION, PREEMPTION_VERTEX_COUNT, 0x4000);

		/* Wa_16013271637 */
		wa_mcr_masked_en(wal, XEHP_SLICE_COMMON_ECO_CHICKEN1,
				 MSC_MSAA_REODER_BUF_BYPASS_DISABLE);

		/* Wa_18019627453 */
		wa_mcr_masked_en(wal, VFLSKPD, VF_PREFETCH_TLB_DIS);

		/* Wa_18018764978 */
		wa_mcr_masked_en(wal, XEHP_PSS_MODE2, SCOREBOARD_STALL_FLUSH_CONTROL);
	}

	/* Wa_18019271663 */
	wa_masked_en(wal, CACHE_MODE_1, MSAA_OPTIMIZATION_REDUC_DISABLE);

	/* Wa_14019877138 */
	wa_mcr_masked_en(wal, XEHP_PSS_CHICKEN, FD_END_COLLECT);
}

/*
 * Not a workaround the hardware team defined: it keeps the established
 * behaviour of nested MI_BATCH_BUFFER_START for clients.
 *
 * The per-context MI_MODE[12] decides whether the bits of a nested batch
 * buffer start keep their traditional meaning, or take the newer one
 * (from Tiger Lake on) that is not backward compatible but allows
 * nesting into third-level batches.  On Tiger Lake the new behaviour was
 * off unless a context opted in; Xe_HPG turns it on by default and needs
 * an explicit opt-out.  Clients expect the traditional meaning and none
 * uses third-level batches, so it is switched back to the legacy
 * behaviour on the parts whose default breaks compatibility.  Should a
 * client ever want third-level nesting, a context parameter could let it
 * opt in.
 */
static void fakewa_disable_nestedbb_mode(const struct i915_engine *e, struct i915_wa_list *wal)
{
	wa_masked_dis(wal, RING_MI_MODE(engine_base(e)), TGL_NESTED_BB_EN);
}

static void gen12_ctx_gt_mocs_init(struct i915_device *i915, const struct i915_engine *e,
				   struct i915_wa_list *wal)
{
	uint8_t mocs;

	/*
	 * Some blitter commands have no MOCS field and use the entry
	 * BLIT_CCTL names, which must be the uncached one.
	 */
	if (e->class == WA_COPY) {
		mocs = (uint8_t)i915_mocs_uc_index(i915);
		wa_write_clr_set(wal, BLIT_CCTL(engine_base(e)), BLIT_CCTL_MASK,
				 BLIT_CCTL_MOCS(mocs, mocs));
	}
}

/*
 * Not official workarounds but general context registers, programmed
 * through the same lists so that they are applied and checked the same
 * way.
 */
static void gen12_ctx_gt_fake_wa_init(struct i915_device *i915, const struct i915_engine *e,
				      struct i915_wa_list *wal)
{
	if (i915->gt_ip >= I915_IP(12, 55))
		fakewa_disable_nestedbb_mode(e, wal);

	gen12_ctx_gt_mocs_init(i915, e, wal);
}

void i915_wa_ctx_list(struct i915_device *i915, const struct i915_engine *e,
		      struct i915_wa_list *wal)
{
	if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_ctx_list(i915, e, wal);
		return;
	}
	if (!wa_lists_cover(i915))
		return;

	/* Applies to all engines. */
	gen12_ctx_gt_fake_wa_init(i915, e, wal);

	if (e->class != WA_RENDER)
		return;

	if (is_xelpg(i915))
		xelpg_ctx_workarounds_init(i915, wal);
	else if (is_platform(i915, I915_PLATFORM_DG2))
		dg2_ctx_workarounds_init(wal);
	else if (is_platform(i915, I915_PLATFORM_DG1))
		dg1_ctx_workarounds_init(i915, wal);
	else if (gt_ver_12(i915))
		gen12_ctx_workarounds_init(i915, wal);
}

/* ---- the whitelist ----------------------------------------------------------------- */

static int is_nonpriv_flags_valid(uint32_t flags)
{
	/* Only bits the slot defines. */
	if (flags & ~RING_FORCE_TO_NONPRIV_MASK_VALID)
		return 0;

	/* An access mode the hardware rejects. */
	if ((flags & RING_FORCE_TO_NONPRIV_ACCESS_MASK) == RING_FORCE_TO_NONPRIV_ACCESS_INVALID)
		return 0;

	return 1;
}

static void whitelist_reg_ext(struct i915_wa_list *wal, uint32_t reg, uint32_t flags)
{
	if (wal->count >= RING_MAX_NONPRIV_SLOTS)
		return;

	if (!is_nonpriv_flags_valid(flags))
		return;

	i915_wa_add(wal, reg | flags, 0, 0, 0, 0);
}

static void whitelist_reg(struct i915_wa_list *wal, uint32_t reg)
{
	whitelist_reg_ext(wal, reg, RING_FORCE_TO_NONPRIV_ACCESS_RW);
}

/* The slot holds only the offset: a multicast register is listed as any other. */
static void whitelist_mcr_reg(struct i915_wa_list *wal, uint32_t reg)
{
	whitelist_reg_ext(wal, reg, RING_FORCE_TO_NONPRIV_ACCESS_RW);
}

static void allow_read_ctx_timestamp(const struct i915_engine *e, struct i915_wa_list *wal)
{
	if (e->class != WA_RENDER)
		whitelist_reg_ext(wal, RING_CTX_TIMESTAMP(engine_base(e)),
				  RING_FORCE_TO_NONPRIV_ACCESS_RD);
}

static void tgl_whitelist_build(const struct i915_engine *e, struct i915_wa_list *w)
{
	allow_read_ctx_timestamp(e, w);

	switch (e->class) {
	case WA_RENDER:
		/*
		 * WaAllowPMDepthAndInvocationCountAccessFromUMD:tgl
		 * Wa_1408556865:tgl
		 *
		 * This covers four registers next to one another:
		 *   - PS_INVOCATION_COUNT
		 *   - PS_INVOCATION_COUNT_UDW
		 *   - PS_DEPTH_COUNT
		 *   - PS_DEPTH_COUNT_UDW
		 */
		whitelist_reg_ext(w, PS_INVOCATION_COUNT,
				  RING_FORCE_TO_NONPRIV_ACCESS_RD | RING_FORCE_TO_NONPRIV_RANGE_4);

		/*
		 * Wa_1808121037:tgl
		 * Wa_14012131227:dg1
		 * Wa_1508744258:tgl,rkl,dg1,adl-s,adl-p
		 */
		whitelist_reg(w, GEN7_COMMON_SLICE_CHICKEN1);

		/* Wa_1806527549:tgl */
		whitelist_reg(w, HIZ_CHICKEN);

		/* Required by a recommended tuning setting (not a workaround). */
		whitelist_reg(w, GEN11_COMMON_SLICE_CHICKEN3);
		break;
	default:
		break;
	}
}

static void dg2_whitelist_build(const struct i915_engine *e, struct i915_wa_list *w)
{
	switch (e->class) {
	case WA_RENDER:
		/* Required by a recommended tuning setting (not a workaround). */
		whitelist_mcr_reg(w, XEHP_COMMON_SLICE_CHICKEN3);
		whitelist_reg(w, GEN7_COMMON_SLICE_CHICKEN1);
		break;
	default:
		break;
	}
}

static void xelpg_whitelist_build(const struct i915_engine *e, struct i915_wa_list *w)
{
	switch (e->class) {
	case WA_RENDER:
		/* Required by a recommended tuning setting (not a workaround). */
		whitelist_mcr_reg(w, XEHP_COMMON_SLICE_CHICKEN3);
		whitelist_reg(w, GEN7_COMMON_SLICE_CHICKEN1);
		break;
	default:
		break;
	}
}

void i915_wa_whitelist(struct i915_device *i915, const struct i915_engine *e,
		       struct i915_wa_list *wal)
{
	if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_whitelist(i915, e, wal);
		return;
	}
	if (!wa_lists_cover(i915))
		return;

	if (e->gsi_offset)
		; /* none yet on the media GT */
	else if (is_xelpg(i915))
		xelpg_whitelist_build(e, wal);
	else if (is_platform(i915, I915_PLATFORM_DG2))
		dg2_whitelist_build(e, wal);
	else if (gt_ver_12(i915))
		tgl_whitelist_build(e, wal);
}

/* ---- the engine list ---------------------------------------------------------------- */

/*
 * Registers that no official workaround names, programmed through the
 * lists so that they are applied and checked the same way.
 */
static void engine_fake_wa_init(struct i915_device *i915, const struct i915_engine *e,
				struct i915_wa_list *wal)
{
	uint8_t mocs_w, mocs_r;

	/*
	 * RING_CMD_CCTL names the MOCS entry the command streamer uses for
	 * commands that have no way to name one.  It should normally be the
	 * uncached entry, although the specification recommends a
	 * write-back entry in some cases on some parts.  (A compute engine
	 * reading through the L3 with the write-back entry is for parts with
	 * L3 CCS reads, none of which are driven here.)
	 */
	mocs_r = (uint8_t)i915_mocs_uc_index(i915);
	mocs_w = (uint8_t)i915_mocs_uc_index(i915);

	wa_masked_field_set(wal, RING_CMD_CCTL(engine_base(e)), CMD_CCTL_MOCS_MASK,
			    CMD_CCTL_MOCS_OVERRIDE(mocs_w, mocs_r));
}

static void rcs_engine_wa_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	int tgl = is_platform(i915, I915_PLATFORM_TIGERLAKE);
	int rkl = is_platform(i915, I915_PLATFORM_ROCKETLAKE);
	int dg1 = is_platform(i915, I915_PLATFORM_DG1);
	int adls = is_platform(i915, I915_PLATFORM_ALDERLAKE_S);
	int adlp = is_platform(i915, I915_PLATFORM_ALDERLAKE_P);
	int dg2 = is_platform(i915, I915_PLATFORM_DG2);

	if (xelpg_a_step(i915)) {
		/* Wa_22014600077 */
		wa_mcr_masked_en(wal, GEN10_CACHE_MODE_SS, ENABLE_EU_COUNT_FOR_TDL_FLUSH);
	}

	if (xelpg_a_step(i915) || dg2) {
		/* Wa_1509727124 */
		wa_mcr_masked_en(wal, GEN10_SAMPLER_MODE, SC_DISABLE_POWER_OPTIMIZATION_EBB);
	}

	if (gt_ip_step(i915, I915_IP(12, 70), I915_STEP_A0, I915_STEP_B0) || dg2) {
		/* Wa_22012856258 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, GEN12_DISABLE_READ_SUPPRESSION);
	}

	if (dg2) {
		/*
		 * Wa_22010960976:dg2
		 * Wa_14013347512:dg2
		 */
		wa_mcr_masked_dis(wal, XEHP_HDC_CHICKEN0,
				  LSC_L1_FLUSH_CTL_3D_DATAPORT_FLUSH_EVENTS_MASK);
	}

	if (gt_ip_range(i915, I915_IP(12, 70), I915_IP(12, 71)) || dg2) {
		/* Wa_14015150844 */
		wa_mcr_add(wal, XEHP_HDC_CHICKEN0, 0,
			   I915_MASKED_ENABLE(DIS_ATOMIC_CHAINING_TYPED_WRITES), 0, 1);
	}

	if (dg2 || adlp || adls || dg1 || rkl || tgl) {
		/*
		 * Wa_1606700617:tgl,dg1,adl-p
		 * Wa_22010271021:tgl,rkl,dg1,adl-s,adl-p
		 * Wa_14010826681:tgl,dg1,rkl,adl-p
		 * Wa_18019627453:dg2
		 */
		wa_masked_en(wal, GEN9_CS_DEBUG_MODE1, FF_DOP_CLOCK_GATE_DISABLE);
	}

	if (adlp || adls || dg1 || rkl || tgl) {
		/* Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, GEN12_DISABLE_EARLY_READ);

		/*
		 * Wa_1407928979:tgl A*
		 * Wa_18011464164:tgl[B0+],dg1[B0+]
		 * Wa_22010931296:tgl[B0+],dg1[B0+]
		 * Wa_14010919138:rkl,dg1,adl-s,adl-p
		 */
		wa_write_or(wal, GEN7_FF_THREAD_MODE, GEN12_FF_TESSELATION_DOP_GATE_DISABLE);

		/* Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p */
		wa_mcr_masked_en(wal, GEN10_SAMPLER_MODE, ENABLE_SMALLPL);
	}

	if (adlp || adls || rkl || tgl) {
		/* Wa_1409804808 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, GEN12_PUSH_CONST_DEREF_HOLD_DIS);

		/* Wa_14010229206 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, GEN12_DISABLE_TDL_PUSH);
	}

	if (rkl || tgl || adlp) {
		/*
		 * Wa_1607297627
		 *
		 * Tiger Lake's and Rocket Lake's documentation lists this both
		 * as an A0-only workaround and as one for every stepping; it
		 * is applied to every stepping.
		 */
		wa_masked_en(wal, RING_PSMI_CTL(RENDER_RING_BASE),
			     GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE |
			     GEN8_RC_SEMA_IDLE_MSG_DISABLE);
	}

	/*
	 * Parts with fine-grained preemption let the kernel driver choose how
	 * preemption granularity is controlled: globally, through the
	 * kernel-only CS_DEBUG_MODE1 (0x20ec, the hardware default), or per
	 * context, through CS_CHICKEN1 (0x2580), which is saved and restored
	 * with the context and which a client may write from its own batch
	 * (MI_LOAD_REGISTER_IMM), so that each client picks its settings and
	 * changes them as it needs.  The per-context option is used: it lets
	 * clients carry out workarounds that need temporary changes to the
	 * preemption behaviour at run time.
	 *
	 *  - Wa_14015141709: on DG2 and early steppings of Meteor Lake,
	 *    CS_CHICKEN1[0] does not disable object-level preemption as it
	 *    should (nor would CS_DEBUG_MODE1[0]), so clients cannot turn
	 *    object-level preemption off on those parts.
	 *
	 *  - Wa_16013994831: a client may need to program CS_CHICKEN1[10]
	 *    under some run-time conditions, which takes effect only with
	 *    the per-context option.
	 */
	wa_masked_en(wal, GEN7_FF_SLICE_CS_CHICKEN1, GEN9_FFSC_PERCTX_PREEMPT_CTRL);
}

static void xcs_engine_wa_init(struct i915_device *i915, const struct i915_engine *e,
			       struct i915_wa_list *wal)
{
	/*
	 * Wa_16018031267, Wa_16018063123: the first copy engine of graphics
	 * 12.55 through 12.71 schedules its blits round-robin.
	 */
	if (!e->gsi_offset && gt_ip_range(i915, I915_IP(12, 55), I915_IP(12, 71)) &&
	    e->class == WA_COPY && e->instance == 0)
		wa_masked_field_set(wal, ECOSKPD(engine_base(e)),
				    XEHP_BLITTER_SCHEDULING_MODE_MASK,
				    XEHP_BLITTER_ROUND_ROBIN_MODE);
}

static void ccs_engine_wa_init(struct i915_wa_list *wal)
{
	/* Nothing yet for the compute engines alone. */
	(void)wal;
}

/*
 * The performance guide's recommended settings for registers that are not
 * part of any engine's context (those that are go on the context list).
 * Programmed through the lists so that they are written again at the
 * right times.
 */
static void add_render_compute_tuning_settings(struct i915_device *i915,
					       struct i915_wa_list *wal)
{
	if (is_xelpg(i915) || is_platform(i915, I915_PLATFORM_DG2))
		wa_mcr_write_clr_set(wal, RT_CTRL, STACKID_CTRL, STACKID_CTRL_512);

	/*
	 * This setting helps only the ATS-M designs (the Data Center GPU Flex
	 * cards built on DG2: 0x56c0, 0x56c1, 0x56c2); the default "age
	 * based" arbitration is best on ordinary DG2 and everything else.
	 */
	if (is_platform(i915, I915_PLATFORM_DG2) &&
	    (i915->devid == 0x56c0 || i915->devid == 0x56c1 || i915->devid == 0x56c2))
		wa_mcr_masked_field_set(wal, GEN9_ROW_CHICKEN4, THREAD_EX_ARB_MODE,
					THREAD_EX_ARB_MODE_RR_AFTER_DEP);

	if (gt_ver_12(i915) && i915->gt_ip < I915_IP(12, 55))
		wa_write_clr(wal, GEN8_GARBCNTL, GEN12_BUS_HASH_CTL_BIT_EXC);
}

/*
 * DG2's compute slices, all given to the first compute engine present,
 * the others marked as serving none.  (The compute slices present are
 * taken to be the compute engines the part has.)
 */
static uint32_t dg2_apply_ccs_mode(struct i915_device *i915)
{
	uint32_t cslices = (i915->info->engine_mask & WA_CCS_ENGINES) / I915_ENGINE_CCS0;
	uint32_t first_ccs = cslices ? (uint32_t)__builtin_ctz(cslices) : 0;
	uint32_t mode = 0;

	for (unsigned cslice = 0; cslice < I915_MAX_CCS; cslice++) {
		if (cslices & (1u << cslice))
			/* An available slice goes to the first engine... */
			mode |= XEHP_CCS_MODE_CSLICE(cslice, first_ccs);
		else
			/* ...and one that is not dispatches to none. */
			mode |= XEHP_CCS_MODE_CSLICE(cslice, XEHP_CCS_MODE_CSLICE_MASK);
	}

	return mode;
}

static void ccs_engine_wa_mode(struct i915_device *i915, struct i915_wa_list *wal)
{
	uint32_t mode;

	if (!is_platform(i915, I915_PLATFORM_DG2))
		return;

	/*
	 * Wa_14019159160: this workaround, among others, makes balancing
	 * work across the compute slices impractical, so automatic load
	 * balancing between them is switched off altogether.
	 */
	wa_masked_en(wal, GEN12_RCU_MODE, XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE);

	/*
	 * With automatic balancing off, every slice is assigned to a single
	 * compute engine ("CCS mode 1").
	 */
	mode = dg2_apply_ccs_mode(i915);
	wa_masked_en(wal, XEHP_CCS_MODE, mode);
}

/*
 * The workarounds of shared registers in the render reset domain that
 * are not tied to one engine.  The render and compute engines are reset
 * together and these registers lose their contents then, so they go on a
 * single render or compute engine's list.
 */
static void general_render_compute_wa_init(struct i915_device *i915, struct i915_wa_list *wal)
{
	int dg2 = is_platform(i915, I915_PLATFORM_DG2);

	add_render_compute_tuning_settings(i915, wal);

	/*
	 * Not a workaround (though sometimes called WaSetInidrectStateOverride):
	 * it lets a client that refers to sampler states through the
	 * BindlessSamplerStateBaseAddress have their border colours relative
	 * to the DynamicStateBaseAddress instead, rather than copying the
	 * border colours into both heaps.
	 */
	wa_mcr_masked_en(wal, GEN10_SAMPLER_MODE, GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE);

	if (gt_ip_step(i915, I915_IP(12, 70), I915_STEP_B0, I915_STEP_FOREVER) ||
	    gt_ip_step(i915, I915_IP(12, 71), I915_STEP_B0, I915_STEP_FOREVER) ||
	    gt_ip_range(i915, I915_IP(12, 74), I915_IP(12, 74))) {
		/* Wa_14017856879 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN3, MTL_DISABLE_FIX_FOR_EOT_FLUSH);

		/* Wa_14020495402 */
		wa_mcr_masked_en(wal, GEN8_ROW_CHICKEN2, XELPG_DISABLE_TDL_SVHS_GATING);
	}

	if (xelpg_a_step(i915))
		/*
		 * Wa_14017066071
		 * Wa_14017654203
		 */
		wa_mcr_masked_en(wal, GEN10_SAMPLER_MODE, MTL_DISABLE_SAMPLER_SC_OOO);

	if (gt_ip_step(i915, I915_IP(12, 71), I915_STEP_A0, I915_STEP_B0))
		/* Wa_22015279794 */
		wa_mcr_masked_en(wal, GEN10_CACHE_MODE_SS, DISABLE_PREFETCH_INTO_IC);

	if (xelpg_a_step(i915) || dg2) {
		/* Wa_22013037850 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, DISABLE_128B_EVICTION_COMMAND_UDW);

		/* Wa_18017747507 */
		wa_masked_en(wal, VFG_PREEMPTION_CHICKEN, POLYGON_TRIFAN_LINELOOP_DISABLE);
	}

	if (xelpg_a_step(i915) || dg2) {
		/* Wa_22014226127 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0, DISABLE_D8_D16_COASLESCE);
	}

	if (dg2) {
		/* Wa_14015227452:dg2,pvc */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, XEHP_DIS_BBL_SYSPIPE);

		/*
		 * Wa_16011620976:dg2_g11
		 * Wa_22015475538:dg2
		 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, DIS_CHAIN_2XSIMD8);

		/* Wa_18028616096 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, UGM_FRAGMENT_THRESHOLD_TO_3);
	}

	if (is_subplatform(i915, I915_SUBPLATFORM_DG2_G11)) {
		/*
		 * Wa_22012826095:dg2
		 * Wa_22013059131:dg2
		 */
		wa_mcr_write_clr_set(wal, LSC_CHICKEN_BIT_0_UDW, MAXREQS_PER_BANK,
				     (2u << 5) & MAXREQS_PER_BANK);

		/* Wa_22013059131:dg2 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0, FORCE_1_SUB_MESSAGE_PER_FRAGMENT);

		/*
		 * Wa_22012654132
		 *
		 * Register 0xe420 is write-only on DG2 (Wa_14012342262) and
		 * cannot be read back, so the readback check is skipped.
		 */
		wa_mcr_add(wal, GEN10_CACHE_MODE_SS, 0,
			   I915_MASKED_ENABLE(ENABLE_PREFETCH_INTO_IC), 0, 1);
	}
}

void i915_wa_engine_list(struct i915_device *i915, const struct i915_engine *e,
			 struct i915_wa_list *wal)
{
	if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_engine_list(i915, e, wal);
		return;
	}
	if (!wa_lists_cover(i915))
		return;

	engine_fake_wa_init(i915, e, wal);

	/*
	 * The common workarounds of the render domain go on just one render
	 * or compute engine's list: they are reset with the whole domain.
	 */
	if (first_render_compute(i915, e)) {
		general_render_compute_wa_init(i915, wal);
		ccs_engine_wa_mode(i915, wal);
	}

	if (e->class == WA_COMPUTE)
		ccs_engine_wa_init(wal);
	else if (e->class == WA_RENDER)
		rcs_engine_wa_init(i915, wal);
	else
		xcs_engine_wa_init(i915, e, wal);
}

/* ---- Xe2, Xe3 and Xe3P ----------------------------------------------------------- */

/* The media IP is within [from, until] (on the standalone media GT). */
static int media_ip_range(const struct i915_device *i915, unsigned from, unsigned until)
{
	return i915->has_media_gt && i915->media_ip >= from && i915->media_ip <= until;
}

static int media_ip_step(const struct i915_device *i915, unsigned ip, unsigned from,
			 unsigned until)
{
	return i915->has_media_gt && i915->media_ip == ip && i915->media_step >= from &&
	       i915->media_step < until;
}

/* The firmware left the flat compression metadata enabled (Xe2 on). */
static int xe2_has_flat_ccs(const struct i915_device *i915)
{
	return i915->has_flat_ccs;
}

/* Wa_18041344222: the dual subslices fused in leave a group out between
 * two groups that have some. */
static int xe2_discontiguous_dss_groups(const struct i915_device *i915)
{
	uint32_t dss = i915->dss_geometry | i915->dss_compute;
	unsigned per = i915->dss_per_group ? i915->dss_per_group : 4;
	int last = -1;

	for (unsigned i = 0; i < 32; i++) {
		if (!(dss & (1u << i)))
			continue;
		int group = (int)(i / per);
		if (group != last) {
			if (group - last > 1)
				return 1;
			last = group;
		}
	}
	return 0;
}

static void xe2_media_vdbox(struct i915_device *i915, struct i915_wa_list *wal,
			    uint32_t reg_off, uint32_t bits)
{
	for (unsigned instance = 0; instance < 4; instance++) {
		uint32_t base = vcs_gt_base(i915, instance, 1);

		if (base)
			wa_write_or(wal, base + reg_off, bits);
	}
}

/* The GT-wide list and the tuning settings of the GT: the primary GT's
 * by graphics IP, the media GT's by media IP. */
static void xe2_gt_list(struct i915_device *i915, int media_gt, struct i915_wa_list *wal)
{
	unsigned ip = i915->gt_ip;

	if (media_gt) {
		if (!i915->has_media_gt)
			return;
		/* Wa_16021867713 */
		if (media_ip_range(i915, I915_IP(13, 0), I915_IP(30, 2)))
			xe2_media_vdbox(i915, wal, 0x3f1c, MFXPIPE_CLKGATE_DIS);
		/* Wa_14019449301 */
		if (media_ip_range(i915, I915_IP(13, 1), I915_IP(20, 0)))
			xe2_media_vdbox(i915, wal, 0x3f08, XE2_CG3DDISHRS_CLKGATE_DIS);
		/* Wa_14017421178 */
		if (media_ip_range(i915, I915_IP(20, 0), I915_IP(20, 0)))
			xe2_media_vdbox(i915, wal, 0x3f10, IECPUNIT_CLKGATE_DIS);
		/* Wa_16021865536 */
		if (media_ip_range(i915, I915_IP(30, 0), I915_IP(30, 2)))
			xe2_media_vdbox(i915, wal, 0x3f10, IECPUNIT_CLKGATE_DIS);
		/* Wa_14021486841 */
		if (media_ip_step(i915, I915_IP(30, 0), I915_STEP_A0, I915_STEP_B0))
			xe2_media_vdbox(i915, wal, 0x3f10, XE2_RAMDFTUNIT_CLKGATE_DIS);
		/* Wa_16028005424 */
		if (media_ip_range(i915, I915_IP(13, 1), I915_IP(35, 0)))
			wa_write_or(wal, XE3_GUC_INTR_CHICKEN, XE3_DISABLE_SIGNALING_ENGINES);
		/* Wa_14026578760 */
		if (media_ip_range(i915, I915_IP(35, 3), I915_IP(35, 3)))
			wa_mcr_write_or(wal, XE3P_GAMSTLB_CTRL, XE3P_DIS_PEND_GPA_LINK);

		/* tuning: the media GT's L3 */
		if (media_ip_range(i915, I915_IP(20, 0), I915_IP(34, 99)))
			wa_mcr_write_clr_set(wal, XE2LPM_L3SQCREG5, L3_PWM_TIMER_INIT_VAL_MASK, 0x7f);
		if (media_ip_range(i915, I915_IP(20, 0), 0xffff)) {
			wa_write_clr_set(wal, XE2LPM_CCCHKNREG1, XE2_ENCOMPPERFFIX, XE2_L3CMPCTRL);
			wa_mcr_write_or(wal, XE2LPM_L3SQCREG3, XE2_COMPPWOVERFETCHEN);
			if (xe2_has_flat_ccs(i915))
				wa_mcr_write_or(wal, XE2LPM_L3SQCREG2, XE2_COMPMEMRD256BOVRFETCHEN);
		}
		if (media_ip_range(i915, I915_IP(13, 1), 0xffff))
			wa_mcr_write_clr_set(wal, XE2_STATELESS_COMPRESSION_CTRL,
					     XE2_UNIFIED_COMPRESSION_FORMAT, 0);
		if (media_ip_range(i915, I915_IP(20, 0), I915_IP(20, 0)))
			wa_mcr_write_or(wal, XE2LPM_SCRATCH3_LBCF, XE2_RWFLUSHALLEN);
		return;
	}

	/* the primary GT */
	/* Wa_16028005424 */
	if (gt_ip_range(i915, I915_IP(30, 0), I915_IP(30, 5)) ||
	    gt_ip_step(i915, I915_IP(35, 10), I915_STEP_A0, I915_STEP_B0))
		wa_write_or(wal, XE3_GUC_INTR_CHICKEN, XE3_DISABLE_SIGNALING_ENGINES);
	/* Wa_14026578760 */
	if (gt_ip_range(i915, I915_IP(35, 10), I915_IP(35, 11)))
		wa_mcr_write_or(wal, XE3P_GAMSTLB_CTRL, XE3P_DIS_PEND_GPA_LINK);
	/* Wa_16025250150 */
	if (ip == I915_IP(20, 1)) {
		wa_mcr_write_clr_set(wal, XE2_LSN_VC_REG2, XE2_LSN_WGT_MASK,
				     XE2_LSN_LNI_WGT(1) | XE2_LSN_LNE_WGT(1) | XE2_LSN_DIM_X_WGT(1) |
					     XE2_LSN_DIM_Y_WGT(1) | XE2_LSN_DIM_Z_WGT(1));
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, XE2_L3_128B_256B_WRT_DIS);
	}
	/* Wa_14021871409 */
	if (gt_ip_step(i915, I915_IP(30, 0), I915_STEP_A0, I915_STEP_B0))
		wa_write_or(wal, XE3_UNSLCGCTL9454, XE3_LSCFE_CLKGATE_DIS);
	if (gt_ip_step(i915, I915_IP(35, 10), I915_STEP_A0, I915_STEP_B0)) {
		/* Wa_14025160223 */
		wa_mcr_write_or(wal, XE3P_MMIOATSREQLIMIT_GAM_WALK_3D, XE3P_DIS_ATS_WRONLY_PG);
		/* Wa_14026144927, Wa_16029437861, Wa_14026127056 */
		wa_mcr_write_or(wal, XE2_L3SQCREG2,
				XE3P_L3_SQ_DISABLE_COAMA_2WAY_COH | XE3P_L3_SQ_DISABLE_COAMA);
		/* Wa_14025635424 */
		wa_mcr_write_or(wal, XE3P_GAMSTLB_CTRL2, XE3P_STLB_SINGLE_BANK_MODE);
	}

	/* tuning */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(34, 99))) {
		wa_mcr_write_clr_set(wal, XEHP_L3SQCREG5, L3_PWM_TIMER_INIT_VAL_MASK, 0x7f);
		if (xe2_has_flat_ccs(i915))
			wa_mcr_write_clr_set(wal, XE2_CCCHKNREG1, XE2_ENCOMPPERFFIX, XE2_L3CMPCTRL);
		wa_mcr_write_or(wal, XE2_L3SQCREG3, XE2_COMPPWOVERFETCHEN);
	}
	if (ip >= I915_IP(20, 1)) {
		if (xe2_has_flat_ccs(i915))
			wa_mcr_write_or(wal, XE2_L3SQCREG2, XE2_COMPMEMRD256BOVRFETCHEN);
		wa_mcr_write_clr_set(wal, XE2_STATELESS_COMPRESSION_CTRL,
				     XE2_UNIFIED_COMPRESSION_FORMAT, 0);
	}
	if (ip == I915_IP(20, 4))
		wa_mcr_write_or(wal, XE2_SCRATCH3_LBCF, XE2_RWFLUSHALLEN);
	if (ip >= I915_IP(35, 10) && !(i915->info->flags & I915_INFO_IS_DGFX))
		wa_mcr_write_clr_set(wal, XE3P_GAMSTLB_CTRL, XE3P_BANK_HASH_MODE,
				     XE3P_BANK_HASH_4KB_MODE);
}

/* Is the engine on the GT the rule's IP names: a graphics rule matches
 * the primary GT's engines, a media rule the media GT's. */
static int xe2_gfx(const struct i915_engine *e)
{
	return !e->gsi_offset;
}

static void xe2_engine_list(struct i915_device *i915, const struct i915_engine *e,
			    struct i915_wa_list *wal)
{
	unsigned ip = i915->gt_ip;
	uint32_t base = engine_base(e);
	int gfx = xe2_gfx(e);
	int first_rc = first_render_compute(i915, e);
	uint8_t mocs_r, mocs_w;

	/* the default cache control entry of commands that name none:
	 * the uncached one; a discrete part's compute engines read through
	 * the write-back one */
	mocs_w = mocs_r = (uint8_t)i915_mocs_uc_index(i915);
	if (e->class == WA_COMPUTE && (i915->info->flags & I915_INFO_IS_DGFX))
		mocs_r = (uint8_t)i915_mocs_wb_index(i915);
	wa_masked_field_set(wal, RING_CMD_CCTL(base), CMD_CCTL_MOCS_MASK,
			    CMD_CCTL_MOCS_OVERRIDE(mocs_w, mocs_r));
	/* Priority_Mem_Read */
	if (gfx)
		wa_masked_en(wal, XE2_CSFE_CHICKEN1(base), XE2_CS_PRIORITY_MEM_READ);

	/* Wa_16021639441 */
	if ((gfx && gt_ip_range(i915, I915_IP(20, 1), I915_IP(20, 4))) ||
	    (!gfx && media_ip_range(i915, I915_IP(13, 1), I915_IP(20, 0))))
		wa_masked_en(wal, XE2_CSFE_CHICKEN1(base),
			     XE2_GHWSP_CSB_REPORT_DIS | XE2_PPHWSP_CSB_AND_TIMESTAMP_REPORT_DIS);
	/* tuning: no context switch reports to the global status page */
	if (gfx && ip >= I915_IP(35, 0))
		wa_masked_en(wal, XE2_CSFE_CHICKEN1(base), XE2_GHWSP_CSB_REPORT_DIS);
	/* Wa_16023105232 */
	if ((gfx && gt_ip_range(i915, I915_IP(20, 1), I915_IP(30, 1))) ||
	    (!gfx && media_ip_range(i915, I915_IP(13, 1), I915_IP(30, 0))))
		wa_masked_en(wal, RING_PSMI_CTL(base), XE2_RC_SEMA_IDLE_MSG_DISABLE);

	if (!gfx)
		return;

	if (e->class == WA_RENDER) {
		/* Wa_14021402888 */
		if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(30, 5)))
			wa_mcr_masked_en(wal, XE2_HALF_SLICE_CHICKEN7, XE2_CLEAR_OPTIMIZATION_DISABLE);
	}
	if (!first_rc)
		return;

	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(20, 4))) {
		/* Wa_18032247524 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0, XE2_SEQUENTIAL_ACCESS_UPGRADE_DISABLE);
		/* Wa_16018712365 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, XE2_ALLOC_DPA_STARVE_FIX_DIS);
		/* Wa_14020338487 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN3, XE2_EUPEND_CHK_FLUSH_DIS);
		/* Wa_14018471104 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, XE2_ENABLE_SMP_LD_RENDER_SURFACE_CONTROL);
		/* Wa_18034896535, Wa_16021540221 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, XE2_DISABLE_TDL_PUSH);
	}
	/* Wa_13012615864 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(30, 5)))
		wa_mcr_masked_en(wal, XE2_TDL_TSL_CHICKEN, XE2_RES_CHK_SPR_DIS);
	/* Wa_18041344222 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(30, 0)) && xe2_discontiguous_dss_groups(i915))
		wa_mcr_masked_en(wal, XE2_TDL_CHICKEN, XE2_EUSTALL_PERF_SAMPLING_DISABLE);
	/* Wa_16018610683 */
	if (ip == I915_IP(20, 4))
		wa_mcr_masked_en(wal, XE2_TDL_TSL_CHICKEN, XE2_SLM_WMTP_RESTORE);
	/* Wa_16018737384 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(29, 99)))
		wa_mcr_masked_en(wal, XE2_ROW_CHICKEN, XE2_EARLY_EOT_DIS);
	/* Wa_14019811474 */
	if (ip == I915_IP(20, 1))
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0, XE2_WR_REQ_CHAINING_DIS);
	/* Wa_14021821874, Wa_14022954250 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(20, 2)))
		wa_mcr_masked_en(wal, XE2_TDL_TSL_CHICKEN, XE2_STK_ID_RESTRICT);
	if (gt_ip_step(i915, I915_IP(30, 0), I915_STEP_A0, I915_STEP_B0)) {
		/* Wa_18034896535 */
		wa_mcr_masked_en(wal, GEN9_ROW_CHICKEN4, XE2_DISABLE_TDL_PUSH);
		/* Wa_16024792527 */
		wa_mcr_masked_field_set(wal, GEN10_SAMPLER_MODE, XE3_SMP_WAIT_FETCH_MERGING_COUNTER,
					XE3_SMP_FORCE_128B_OVERFETCH);
	}
	/* Wa_14023061436 */
	if (gt_ip_range(i915, I915_IP(30, 0), I915_IP(30, 5)))
		wa_mcr_masked_en(wal, XE2_TDL_CHICKEN, XE3_QID_WAIT_FOR_THREAD_NOT_RUN_DISABLE);
	if (gt_ip_step(i915, I915_IP(35, 10), I915_STEP_A0, I915_STEP_B0)) {
		/* Wa_22021149932, Wa_14026290593 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW, XE3P_SAMPLER_LD_LSC_DISABLE);
		/* Wa_14025676848, Wa_14026270459 */
		wa_mcr_write_or(wal, LSC_CHICKEN_BIT_0_UDW,
				XE3P_LSCFE_SAME_ADDRESS_ATOMICS_COALESCING_DISABLE);
		/* Wa_16028951944 */
		wa_mcr_masked_en(wal, XE3P_ROW_CHICKEN5, XE3P_CPSS_AWARE_DIS);
		/* Wa_18044193044 */
		wa_mcr_masked_en(wal, XE2_TDL_CHICKEN, XE3P_BIT_APQ_OPT_DIS);
	}

	/* tuning: no null query for the any-hit shader */
	wa_mcr_write_or(wal, RT_CTRL, XE2_DIS_NULL_QUERY);
	/* tuning: TileY 2x2 walk */
	if (ip >= I915_IP(35, 10))
		wa_mcr_masked_en(wal, XE3P_TDL_TSL_CHICKEN2, XE3P_TILEY_LOCALID);
}

/* What a render or compute context carries (the primary GT's engines
 * only: the media GT has none of these). */
static void xe2_ctx_list(struct i915_device *i915, const struct i915_engine *e,
			 struct i915_wa_list *wal)
{
	unsigned ip = i915->gt_ip;

	if (!xe2_gfx(e))
		return;
	/* a compute front end the fuses say cannot preempt mid-thread
	 * preempts at thread group boundaries */
	if ((e->class == WA_RENDER || e->class == WA_COMPUTE) &&
	    (i915_read32(i915, XEHP_FUSE4) & XE2_CFEG_WMTP_DISABLE))
		wa_masked_field_set(wal, GEN8_CS_CHICKEN1(engine_base(e)),
				    GEN9_PREEMPT_GPGPU_LEVEL_MASK,
				    GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
	if (e->class != WA_RENDER)
		return;

	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(20, 4))) {
		/* Wa_14019877138 */
		wa_mcr_masked_en(wal, XEHP_PSS_CHICKEN, FD_END_COLLECT);
		/* Wa_14019386621 */
		wa_masked_en(wal, XE2_VF_SCRATCHPAD, XE2_VFG_TED_CREDIT_INTERFACE_DISABLE);
		/* Wa_14019988906 */
		wa_mcr_masked_en(wal, XEHP_PSS_CHICKEN, XE2_FLSH_IGNORES_PSD);
		/* Wa_18033852989 */
		wa_mcr_masked_en(wal, GEN7_COMMON_SLICE_CHICKEN1, XE2_DISABLE_BOTTOM_CLIP_RECTANGLE_TEST);
		/* Wa_15016589081 */
		wa_mcr_masked_en(wal, XE2_CHICKEN_RASTER_1, XE2_DIS_CLIP_NEGATIVE_BOUNDING_BOX);
	}
	/* Wa_14026781792 */
	if (gt_ip_range(i915, I915_IP(30, 0), I915_IP(35, 10)))
		wa_mcr_write_or(wal, XE2_FF_MODE, XE3_DIS_TE_PATCH_CTRL);
	/* Wa_14021567978 */
	wa_mcr_masked_en(wal, CHICKEN_RASTER_2, TBIMR_FAST_CLIP);
	/* Wa_14020756599 */
	if (ip == I915_IP(20, 4) || ip == I915_IP(20, 1) ||
	    (i915->has_media_gt && i915->media_ip == I915_IP(20, 0)))
		wa_mcr_masked_en(wal, XE2_WM_CHICKEN3, XE2_HIZ_PLANE_COMPRESSION_DIS);
	/* Wa_14021490052 */
	if (ip == I915_IP(20, 4) || ip == I915_IP(20, 1) ||
	    gt_ip_step(i915, I915_IP(30, 0), I915_STEP_A0, I915_STEP_B0)) {
		wa_mcr_write_or(wal, XE2_FF_MODE, XE2_DIS_MESH_PARTIAL_AUTOSTRIP | XE2_DIS_MESH_AUTOSTRIP);
		wa_mcr_masked_en(wal, VFLSKPD, XE2_DIS_PARTIAL_AUTOSTRIP | XE2_DIS_AUTOSTRIP);
	}
	/* Wa_22021007897 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(20, 2)) ||
	    gt_ip_range(i915, I915_IP(30, 0), I915_IP(30, 5)))
		wa_mcr_masked_en(wal, COMMON_SLICE_CHICKEN4, XE2_SBE_PUSH_CONSTANT_BEHIND_FIX_ENABLE);
	/* Wa_14024681466 */
	if (gt_ip_range(i915, I915_IP(30, 0), I915_IP(30, 5)))
		wa_mcr_masked_en(wal, XEHP_SLICE_COMMON_ECO_CHICKEN1, XE3_FAST_CLEAR_VALIGN_FIX);
	/* Wa_15016589081 */
	if (gt_ip_step(i915, I915_IP(30, 0), I915_STEP_A0, I915_STEP_B0))
		wa_mcr_masked_en(wal, XE2_CHICKEN_RASTER_1, XE2_DIS_CLIP_NEGATIVE_BOUNDING_BOX);

	/* tuning: the windower's hardware filtering */
	if (gt_ip_range(i915, I915_IP(30, 0), I915_IP(35, 99)))
		wa_mcr_masked_en(wal, COMMON_SLICE_CHICKEN4, XE3_HW_FILTERING);
	/* tuning: the vertex shader's hit maximum */
	if (ip == I915_IP(20, 1))
		wa_mcr_write_clr_set(wal, XE2_FF_MODE, XE2_VS_HIT_MAX_VALUE_MASK,
				     XE2_VS_HIT_MAX_VALUE_MASK);
}

static void xe2_whitelist(struct i915_device *i915, const struct i915_engine *e,
			  struct i915_wa_list *wal)
{
	if (!xe2_gfx(e) || e->class != WA_RENDER)
		return;
	/* Wa_16020183090 */
	if (gt_ip_step(i915, I915_IP(20, 4), I915_STEP_A0, I915_STEP_B0))
		whitelist_reg_ext(wal, XE2_CSBE_DEBUG_STATUS(RENDER_RING_BASE),
				  RING_FORCE_TO_NONPRIV_ACCESS_RW);
	/* Wa_14024997852 */
	if (gt_ip_range(i915, I915_IP(20, 1), I915_IP(30, 5)) ||
	    gt_ip_step(i915, I915_IP(35, 10), I915_STEP_A0, I915_STEP_B0)) {
		whitelist_mcr_reg(wal, XE2_FF_MODE);
		whitelist_mcr_reg(wal, VFLSKPD);
	}
}
