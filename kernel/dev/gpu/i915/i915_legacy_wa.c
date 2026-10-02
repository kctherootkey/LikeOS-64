// LikeOS -- workarounds, clock gating and GT-wide settings before
// Broadwell.
//
// What each part needs set before it renders correctly, in three
// places: the GT's own registers (kept across contexts, lost with a
// reset), the render engine's registers (written again whenever the
// engine is), and the settings a render context carries in its image
// (written once, by the first batch, into the default state every later
// context starts from).  Next to them: the clock gating each chipset
// wants disabled in its GT units, the bit-6 swizzling of tiled surfaces
// switched on to match the memory controller, the rings gen2/3 have and
// nobody uses kept idle, and the PPGTT switched on.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2012-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* A masked register: the bits named in the upper half are written. */
static void wa_masked_en(struct i915_device *i915, uint32_t reg, uint32_t bits)
{
	i915_write32(i915, reg, LEG_MASKED_EN(bits));
}

static void wa_masked_dis(struct i915_device *i915, uint32_t reg, uint32_t bits)
{
	i915_write32(i915, reg, LEG_MASKED_DIS(bits));
}

static void wa_masked_field_set(struct i915_device *i915, uint32_t reg, uint32_t mask, uint32_t val)
{
	i915_write32(i915, reg, LEG_MASKED_FIELD(mask, val));
}

static void wa_write_clr_set(struct i915_device *i915, uint32_t reg, uint32_t clr, uint32_t set)
{
	uint32_t old = i915_read32(i915, reg);
	uint32_t val = (old & ~clr) | set;

	if (val != old)
		i915_write32(i915, reg, val);
}

/* ---- the GT --------------------------------------------------------------------- */

static void gen4_gt_workarounds(struct i915_device *i915)
{
	/* WaDisable_RenderCache_OperationalFlush:gen4,ilk */
	wa_masked_dis(i915, CACHE_MODE_0, RC_OP_FLUSH_ENABLE);
}

static void g4x_gt_workarounds(struct i915_device *i915)
{
	gen4_gt_workarounds(i915);
	/* WaDisableRenderCachePipelinedFlush:g4x,ilk */
	wa_masked_en(i915, CACHE_MODE_0, CM0_PIPELINED_RENDER_FLUSH_DISABLE);
}

void leg_gt_workarounds(struct i915_device *i915)
{
	int p = i915->info->platform;

	switch (p) {
	case I915_PLATFORM_HASWELL:
		/* L3 caching of data atomics does not work */
		i915_write32(i915, HSW_SCRATCH1, HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE);
		wa_masked_en(i915, HSW_ROW_CHICKEN3, HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE);
		/* WaVSRefCountFullforceMissDisable:hsw */
		wa_write_clr_set(i915, GEN7_FF_THREAD_MODE, GEN7_FF_VS_REF_CNT_FFME, 0);
		break;
	case I915_PLATFORM_VALLEYVIEW:
		/* WaForceL3Serialization:vlv */
		wa_write_clr_set(i915, GEN7_L3SQCREG4, L3SQ_URB_READ_CAM_MATCH_DISABLE, 0);
		/* WaIncreaseL3CreditsForVLVB0:vlv (the hardware default) */
		i915_write32(i915, GEN7_L3SQCREG1, VLV_B0_WA_L3SQCREG1_VALUE);
		break;
	case I915_PLATFORM_IVYBRIDGE:
		/* WaDisableRHWOOptimizationForRenderHang:ivb */
		wa_masked_dis(i915, GEN7_COMMON_SLICE_CHICKEN1, GEN7_CSC1_RHWO_OPT_DISABLE_IN_RCC);
		/* WaApplyL3ControlAndL3ChickenMode:ivb */
		i915_write32(i915, GEN7_L3CNTLREG1, GEN7_WA_FOR_GEN7_L3_CONTROL);
		i915_write32(i915, GEN7_L3_CHICKEN_MODE_REGISTER, GEN7_WA_L3_CHICKEN_MODE);
		/* WaForceL3Serialization:ivb */
		wa_write_clr_set(i915, GEN7_L3SQCREG4, L3SQ_URB_READ_CAM_MATCH_DISABLE, 0);
		break;
	case I915_PLATFORM_SANDYBRIDGE:
		break;
	case I915_PLATFORM_IRONLAKE:
		g4x_gt_workarounds(i915);
		wa_masked_en(i915, _3D_CHICKEN2, _3D_CHICKEN2_WM_READ_PIPELINED);
		break;
	default:
		if (g_leg.is_g4x)
			g4x_gt_workarounds(i915);
		else if (leg_gen(i915) == 4)
			gen4_gt_workarounds(i915);
		break;
	}
}

/* ---- the render engine ----------------------------------------------------------- */

void leg_engine_workarounds(struct i915_device *i915, struct i915_engine *e)
{
	int gen = leg_gen(i915);
	int p = i915->info->platform;

	if (gen < 4 || e->class != 0)
		return;
	if (p == I915_PLATFORM_HASWELL) {
		/* WaSampleCChickenBitEnable:hsw */
		wa_masked_en(i915, HSW_HALF_SLICE_CHICKEN3, HSW_SAMPLE_C_PERFORMANCE);
		/* HiZ raw stall optimization on */
		wa_masked_dis(i915, CACHE_MODE_0_GEN7, HIZ_RAW_STALL_OPT_DISABLE);
	}
	if (p == I915_PLATFORM_VALLEYVIEW) {
		/* WaDisableEarlyCull:vlv */
		wa_masked_en(i915, _3D_CHICKEN3, _3D_CHICKEN_SF_DISABLE_OBJEND_CULL);
		/* WaVSThreadDispatchOverride:ivb,vlv -- every thread type */
		wa_write_clr_set(i915, GEN7_FF_THREAD_MODE, GEN7_FF_SCHED_MASK,
				 GEN7_FF_TS_SCHED_HW | GEN7_FF_VS_SCHED_HW | GEN7_FF_DS_SCHED_HW);
		/* WaPsdDispatchEnable:vlv, WaDisablePSDDualDispatchEnable:vlv */
		wa_masked_en(i915, GEN7_HALF_SLICE_CHICKEN1,
			     GEN7_MAX_PS_THREAD_DEP | GEN7_PSD_SINGLE_PORT_DISPATCH_ENABLE);
	}
	if (p == I915_PLATFORM_IVYBRIDGE) {
		int gt = (i915->id && i915->id->gt) ? i915->id->gt : i915->info->gt;
		/* WaDisableEarlyCull:ivb (HiZ raw stall optimization stays
		 * off: it corrupts HiZ on GT1) */
		wa_masked_en(i915, _3D_CHICKEN3, _3D_CHICKEN_SF_DISABLE_OBJEND_CULL);
		/* WaVSThreadDispatchOverride:ivb,vlv */
		wa_write_clr_set(i915, GEN7_FF_THREAD_MODE, GEN7_FF_SCHED_MASK,
				 GEN7_FF_TS_SCHED_HW | GEN7_FF_VS_SCHED_HW | GEN7_FF_DS_SCHED_HW);
		/* WaDisablePSDDualDispatchEnable:ivb */
		if (gt == 1)
			wa_masked_en(i915, GEN7_HALF_SLICE_CHICKEN1,
				     GEN7_PSD_SINGLE_PORT_DISPATCH_ENABLE);
	}
	if (gen == 7) {
		/* WaBCSVCSTlbInvalidationMode:ivb,vlv,hsw */
		wa_masked_en(i915, RING_MODE_GEN7(RENDER_RING_BASE),
			     GFX_TLB_INVALIDATE_EXPLICIT | GFX_REPLAY_MODE);
		/* 16x4 hashing is the fastest in practice */
		wa_masked_field_set(i915, GEN7_GT_MODE, GEN6_WIZ_HASHING_MASK, GEN6_WIZ_HASHING_16x4);
	}
	if (gen == 6 || gen == 7)
		/* WaDisableAsyncFlipPerfMode:snb,ivb,hsw,vlv -- needed to use
		 * MI_WAIT_FOR_EVENT in the command streamer */
		wa_masked_en(i915, RING_MI_MODE(RENDER_RING_BASE), ASYNC_FLIP_PERF_DISABLE);
	if (gen == 6) {
		/* WaEnableFlushTlbInvalidationMode:snb */
		wa_masked_en(i915, GFX_MODE, GFX_TLB_INVALIDATE_EXPLICIT);
		/* WaDisableHiZPlanesWhenMSAAEnabled:snb */
		wa_masked_en(i915, _3D_CHICKEN, _3D_CHICKEN_HIZ_PLANE_DISABLE_MSAA_4X_SNB);
		/* WaStripsFansDisableFastClipPerformanceFix:snb, and the bit
		 * needed with more than 16 SF outputs in normal clip mode */
		wa_masked_en(i915, _3D_CHICKEN3,
			     _3D_CHICKEN3_SF_DISABLE_FASTCLIP_CULL |
				     _3D_CHICKEN3_SF_DISABLE_PIPELINED_ATTR_FETCH);
		wa_masked_field_set(i915, GEN6_GT_MODE, GEN6_WIZ_HASHING_MASK, GEN6_WIZ_HASHING_16x4);
		/* LRA replacement in the STC unit is not supported */
		wa_masked_dis(i915, CACHE_MODE_0, CM0_STC_EVICT_DISABLE_LRA_SNB);
	}
	if (gen >= 4 && gen <= 6)
		/* WaTimedSingleVertexDispatch:cl,bw,ctg,elk,ilk,snb */
		wa_masked_en(i915, RING_MI_MODE(RENDER_RING_BASE), VS_TIMER_DISPATCH);
	if (gen == 4)
		/* The constant buffer is executed as it is loaded from the
		 * image, with an address that may be stale by then: not
		 * restored at all (clients reload it every batch). */
		wa_masked_en(i915, ECOSKPD(RENDER_RING_BASE), ECO_CONSTANT_BUFFER_SR_DISABLE);
}

/* ---- what a render context carries ----------------------------------------------- */

uint32_t leg_ctx_workarounds_emit(struct i915_device *i915, struct i915_engine *e, uint32_t *cs,
				  uint32_t max)
{
	uint32_t regs[4], vals[4];
	uint32_t n = 0, i = 0;

	if (e->class != 0)
		return 0;
	if (leg_gen(i915) == 6) {
		regs[n] = INSTPM;
		vals[n++] = LEG_MASKED_EN(INSTPM_FORCE_ORDERING);
		/* WaDisable_RenderCache_OperationalFlush:snb */
		regs[n] = CACHE_MODE_0;
		vals[n++] = LEG_MASKED_DIS(RC_OP_FLUSH_ENABLE);
	} else if (leg_gen(i915) == 7) {
		regs[n] = INSTPM;
		vals[n++] = LEG_MASKED_EN(INSTPM_FORCE_ORDERING);
		/* WaDisable_RenderCache_OperationalFlush:ivb,vlv,hsw */
		regs[n] = CACHE_MODE_0_GEN7;
		vals[n++] = LEG_MASKED_DIS(RC_OP_FLUSH_ENABLE);
		/* WaDisable4x2SubspanOptimization:ivb,hsw (set on VLV too) */
		regs[n] = CACHE_MODE_1;
		vals[n++] = LEG_MASKED_EN(PIXEL_SUBSPAN_COLLECT_OPT_DISABLE);
	}
	if (!n || max < 2 + 2 * n)
		return 0;
	cs[i++] = MI_LOAD_REGISTER_IMM(n);
	for (uint32_t k = 0; k < n; k++) {
		cs[i++] = regs[k];
		cs[i++] = vals[k];
	}
	cs[i++] = MI_NOOP;
	return i;
}

/* ---- swizzling, unused rings, the PPGTT ----------------------------------------------- */

void leg_init_swizzling(struct i915_device *i915)
{
	int gen = leg_gen(i915);

	if (gen < 5 || g_leg.swizzle_x == I915_BIT_6_SWIZZLE_NONE)
		return;
	leg_rmw(i915, DISP_ARB_CTL, 0, DISP_TILE_SURFACE_SWIZZLING);
	if (gen == 5)
		return;
	leg_rmw(i915, TILECTL, 0, TILECTL_SWZCTL);
	if (gen == 6)
		i915_write32(i915, ARB_MODE, LEG_MASKED_EN(ARB_MODE_SWIZZLE_SNB));
	else if (gen == 7)
		i915_write32(i915, ARB_MODE, LEG_MASKED_EN(ARB_MODE_SWIZZLE_IVB));
}

static void init_unused_ring(struct i915_device *i915, uint32_t base)
{
	i915_write32(i915, RING_CTL(base), 0);
	i915_write32(i915, RING_HEAD(base), 0);
	i915_write32(i915, RING_TAIL(base), 0);
	i915_write32(i915, RING_START(base), 0);
}

/* The i830 can leave rings it has but nobody uses "active" (head
 * different from tail) after a resume, which keeps the package out of
 * C3. */
void leg_init_unused_rings(struct i915_device *i915)
{
	if (leg_is(i915, I915_PLATFORM_I830)) {
		init_unused_ring(i915, PRB1_BASE);
		init_unused_ring(i915, SRB0_BASE);
		init_unused_ring(i915, SRB1_BASE);
		init_unused_ring(i915, SRB2_BASE);
		init_unused_ring(i915, SRB3_BASE);
	} else if (leg_gen(i915) == 2) {
		init_unused_ring(i915, SRB0_BASE);
		init_unused_ring(i915, SRB1_BASE);
	} else if (leg_gen(i915) == 3) {
		init_unused_ring(i915, PRB1_BASE);
		init_unused_ring(i915, PRB2_BASE);
	}
}

void leg_ppgtt_enable(struct i915_device *i915)
{
	if (!g_leg.ppgtt.present)
		return;
	if (leg_gen(i915) == 7) {
		uint32_t ecochk;
		leg_rmw(i915, GAC_ECO_BITS, 0, ECOBITS_PPGTT_CACHE64B);
		ecochk = i915_read32(i915, GAM_ECOCHK);
		if (leg_is(i915, I915_PLATFORM_HASWELL)) {
			ecochk |= ECOCHK_PPGTT_WB_HSW;
		} else {
			ecochk |= ECOCHK_PPGTT_LLC_IVB;
			ecochk &= ~ECOCHK_PPGTT_GFDT_IVB;
		}
		i915_write32(i915, GAM_ECOCHK, ecochk);
		return;
	}
	leg_rmw(i915, GAC_ECO_BITS, 0, ECOBITS_SNB_BIT | ECOBITS_PPGTT_CACHE64B);
	leg_rmw(i915, GAB_CTL, 0, GAB_CTL_CONT_AFTER_PAGEFAULT);
	leg_rmw(i915, GAM_ECOCHK, 0, ECOCHK_SNB_BIT | ECOCHK_PPGTT_CACHE64B);
	i915_write32(i915, GFX_MODE, LEG_MASKED_EN(GFX_PPGTT_ENABLE));
}

/* ---- clock gating ---------------------------------------------------------------------- */

void leg_init_clock_gating(struct i915_device *i915)
{
	int p = i915->info->platform;
	int gen = leg_gen(i915);

	if (p == I915_PLATFORM_HASWELL) {
		/* WaCatErrorRejectionIssue:hsw */
		leg_rmw(i915, GEN7_SQ_CHICKEN_MBCUNIT_CONFIG, 0, GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB);
		/* WaSwitchSolVfFArbitrationPriority:hsw */
		leg_rmw(i915, GAM_ECOCHK, 0, HSW_ECOCHK_ARB_PRIO_SOL);
	} else if (p == I915_PLATFORM_IVYBRIDGE) {
		int gt = (i915->id && i915->id->gt) ? i915->id->gt : i915->info->gt;
		/* WaDisableBackToBackFlipFix:ivb */
		i915_write32(i915, IVB_CHICKEN3,
			     CHICKEN3_DGMG_REQ_OUT_FIX_DISABLE | CHICKEN3_DGMG_DONE_FIX_DISABLE);
		i915_write32(i915, GEN7_ROW_CHICKEN2, LEG_MASKED_EN(DOP_CLOCK_GATING_DISABLE));
		if (gt != 1)
			i915_write32(i915, GEN7_ROW_CHICKEN2_GT2,
				     LEG_MASKED_EN(DOP_CLOCK_GATING_DISABLE));
		/* WaDisableRCZUnitClockGating:ivb */
		i915_write32(i915, GEN6_UCGCTL2, GEN6_RCZUNIT_CLOCK_GATE_DISABLE);
		/* WaCatErrorRejectionIssue:ivb */
		leg_rmw(i915, GEN7_SQ_CHICKEN_MBCUNIT_CONFIG, 0, GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB);
		leg_rmw(i915, GEN6_MBCUNIT_SNPCR, GEN6_MBC_SNPCR_MASK, GEN6_MBC_SNPCR_MED);
	} else if (p == I915_PLATFORM_VALLEYVIEW) {
		/* WaDisableBackToBackFlipFix:vlv */
		i915_write32(i915, IVB_CHICKEN3,
			     CHICKEN3_DGMG_REQ_OUT_FIX_DISABLE | CHICKEN3_DGMG_DONE_FIX_DISABLE);
		/* WaDisableDopClockGating:vlv */
		i915_write32(i915, GEN7_ROW_CHICKEN2, LEG_MASKED_EN(DOP_CLOCK_GATING_DISABLE));
		/* WaCatErrorRejectionIssue:vlv */
		leg_rmw(i915, GEN7_SQ_CHICKEN_MBCUNIT_CONFIG, 0, GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB);
		/* WaDisableRCZUnitClockGating:vlv */
		i915_write32(i915, GEN6_UCGCTL2, GEN6_RCZUNIT_CLOCK_GATE_DISABLE);
		/* WaDisableL3Bank2xClockGate:vlv */
		leg_rmw(i915, GEN7_UCGCTL4, 0, GEN7_L3BANK2X_CLOCK_GATE_DISABLE);
		/* WaDisableVLVClockGating_VBIIssue:vlv -- vblank events
		 * would be reported late */
		i915_write32(i915, VLV_GUNIT_CLOCK_GATE, GCFG_DIS);
	} else if (gen == 6) {
		leg_rmw(i915, GEN6_UCGCTL1, 0,
			GEN6_BLBUNIT_CLOCK_GATE_DISABLE | GEN6_CSUNIT_CLOCK_GATE_DISABLE);
		/* WaDisableRCCUnitClockGating:snb, WaDisableRCPBUnitClockGating:snb
		 * -- without them Z writes go out of order after a while */
		i915_write32(i915, GEN6_UCGCTL2,
			     GEN6_RCPBUNIT_CLOCK_GATE_DISABLE | GEN6_RCCUNIT_CLOCK_GATE_DISABLE);
	} else if (gen == 5) {
		i915_write32(i915, PCH_3DCGDIS0,
			     MARIUNIT_CLOCK_GATE_DISABLE | SVSMUNIT_CLOCK_GATE_DISABLE);
		i915_write32(i915, PCH_3DCGDIS1, VFMUNIT_CLOCK_GATE_DISABLE);
	} else if (g_leg.is_g4x) {
		i915_write32(i915, RENCLK_GATE_D1, 0);
		i915_write32(i915, RENCLK_GATE_D2,
			     VF_UNIT_CLOCK_GATE_DISABLE | GS_UNIT_CLOCK_GATE_DISABLE |
				     CL_UNIT_CLOCK_GATE_DISABLE);
		i915_write32(i915, RAMCLK_GATE_D, 0);
	} else if (p == I915_PLATFORM_I965GM) {
		i915_write32(i915, RENCLK_GATE_D1, I965_RCC_CLOCK_GATE_DISABLE);
		i915_write32(i915, RENCLK_GATE_D2, 0);
		i915_write32(i915, RAMCLK_GATE_D, 0);
		leg_write16(i915, DEUC, 0);
		i915_write32(i915, MI_ARB_STATE, LEG_MASKED_EN(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (p == I915_PLATFORM_I965G) {
		i915_write32(i915, RENCLK_GATE_D1,
			     I965_RCZ_CLOCK_GATE_DISABLE | I965_RCC_CLOCK_GATE_DISABLE |
				     I965_RCPB_CLOCK_GATE_DISABLE | I965_ISC_CLOCK_GATE_DISABLE |
				     I965_FBC_CLOCK_GATE_DISABLE);
		i915_write32(i915, RENCLK_GATE_D2, 0);
		i915_write32(i915, MI_ARB_STATE, LEG_MASKED_EN(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (gen == 3) {
		uint32_t dstate = i915_read32(i915, D_STATE);
		dstate |= DSTATE_PLL_D3_OFF | DSTATE_GFX_CLOCK_GATING | DSTATE_DOT_CLOCK_GATING;
		i915_write32(i915, D_STATE, dstate);
		if (p == I915_PLATFORM_PINEVIEW)
			i915_write32(i915, ECOSKPD(RENDER_RING_BASE), LEG_MASKED_EN(ECO_GATING_CX_ONLY));
		/* IIR "flip pending" means done with this bit clear */
		i915_write32(i915, ECOSKPD(RENDER_RING_BASE), LEG_MASKED_DIS(ECO_FLIP_DONE));
		/* interrupts wake from C3 */
		i915_write32(i915, INSTPM, LEG_MASKED_EN(INSTPM_AGPBUSY_INT_EN));
		/* render writes complete in C3 */
		i915_write32(i915, MI_ARB_STATE, LEG_MASKED_EN(MI_ARB_C3_LP_WRITE_ENABLE));
		i915_write32(i915, MI_ARB_STATE, LEG_MASKED_EN(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (p == I915_PLATFORM_I85X || p == I915_PLATFORM_I865G) {
		i915_write32(i915, RENCLK_GATE_D1, SV_CLOCK_GATE_DISABLE);
		/* interrupts wake from C3 */
		i915_write32(i915, MI_STATE,
			     LEG_MASKED_EN(MI_AGPBUSY_INT_EN) | LEG_MASKED_DIS(MI_AGPBUSY_830_MODE));
		i915_write32(i915, MEM_MODE, LEG_MASKED_EN(MEM_DISPLAY_TRICKLE_FEED_DISABLE));
		/* compression ignores 3D activity: the driver tracks
		 * rendering itself */
		i915_write32(i915, SCPD0, LEG_MASKED_EN(SCPD_FBC_IGNORE_3D));
	} else if (gen == 2) {
		i915_write32(i915, MEM_MODE,
			     LEG_MASKED_EN(MEM_DISPLAY_A_TRICKLE_FEED_DISABLE) |
				     LEG_MASKED_EN(MEM_DISPLAY_B_TRICKLE_FEED_DISABLE));
	}
}
