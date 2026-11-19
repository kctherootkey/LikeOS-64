// LikeOS -- register definitions the workaround lists use.
//
// The offsets and bits that i915_wa_lists.c writes on Gen12 and later
// graphics: chicken bits, clock-gating overrides and tuning registers of
// the GT, the per-engine registers at an engine's base, and the
// FORCE_TO_NONPRIV whitelist encoding.  Offsets below 0x40000 are
// relative to the GT they belong to (a standalone media GT has its copy
// of that range higher up).  A register marked MCR has one copy per
// slice, subslice or bank: a write reaches every copy, a read is steered
// at one.
//
// Every name is defined only if i915_reg.h has not defined it already.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2024 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_WA_REG_H
#define KERNEL_DEV_GPU_I915_WA_REG_H

#include <kernel/dev/video/gpu/i915/i915_reg.h>

/* ---- The registers the workaround lists of the primary GT write. */
#ifndef GEN8_MCR_SELECTOR
#define GEN8_MCR_SELECTOR 0xfdc
#endif
#ifndef GEN11_MCR_SLICE
#define GEN11_MCR_SLICE(slice) (((slice) & 0xfu) << 27)
#endif
#ifndef GEN11_MCR_SLICE_MASK
#define GEN11_MCR_SLICE_MASK 0x78000000u
#endif
#ifndef GEN11_MCR_SUBSLICE
#define GEN11_MCR_SUBSLICE(subslice) (((subslice) & 0x7u) << 24)
#endif
#ifndef GEN11_MCR_SUBSLICE_MASK
#define GEN11_MCR_SUBSLICE_MASK 0x07000000u
#endif
#ifndef GEN7_MISCCPCTL
#define GEN7_MISCCPCTL 0x9424
#endif
#ifndef GEN12_DOP_CLOCK_GATE_RENDER_ENABLE
#define GEN12_DOP_CLOCK_GATE_RENDER_ENABLE (1u << 1)
#endif
#ifndef UNSLICE_UNIT_LEVEL_CLKGATE
#define UNSLICE_UNIT_LEVEL_CLKGATE 0x9434
#endif
#ifndef CG3DDISCFEG_CLKGATE_DIS
#define CG3DDISCFEG_CLKGATE_DIS (1u << 17)
#endif
#ifndef UNSLICE_UNIT_LEVEL_CLKGATE2
#define UNSLICE_UNIT_LEVEL_CLKGATE2 0x94e4
#endif
#ifndef VSUNIT_CLKGATE_DIS_TGL
#define VSUNIT_CLKGATE_DIS_TGL (1u << 19)
#endif
#ifndef GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE
#define GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE 0x9524 /* MCR */
#endif
#ifndef DSS_ROUTER_CLKGATE_DIS
#define DSS_ROUTER_CLKGATE_DIS (1u << 28)
#endif
#ifndef SUBSLICE_UNIT_LEVEL_CLKGATE2
#define SUBSLICE_UNIT_LEVEL_CLKGATE2 0x9528 /* MCR */
#endif
#ifndef CPSSUNIT_CLKGATE_DIS
#define CPSSUNIT_CLKGATE_DIS (1u << 9)
#endif
#ifndef GEN10_DFR_RATIO_EN_AND_CHICKEN
#define GEN10_DFR_RATIO_EN_AND_CHICKEN 0x9550 /* MCR */
#endif
#ifndef DFR_DISABLE
#define DFR_DISABLE (1u << 9)
#endif
#ifndef GEN12_SQCNT1
#define GEN12_SQCNT1 0x8718
#endif
#ifndef GEN12_STRICT_RAR_ENABLE
#define GEN12_STRICT_RAR_ENABLE (1u << 23)
#endif
#ifndef XEHP_SQCM
#define XEHP_SQCM 0x8724 /* MCR */
#endif
#ifndef EN_32B_ACCESS
#define EN_32B_ACCESS (1u << 30)
#endif
#ifndef XEHP_L3NODEARBCFG
#define XEHP_L3NODEARBCFG 0xb0b4 /* MCR */
#endif
#ifndef XEHP_LNESPARE
#define XEHP_LNESPARE (1u << 19)
#endif
#ifndef XEHP_L3SCQREG7
#define XEHP_L3SCQREG7 0xb188 /* MCR */
#endif
#ifndef BLEND_FILL_CACHING_OPT_DIS
#define BLEND_FILL_CACHING_OPT_DIS (1u << 3)
#endif
#ifndef RENDER_MOD_CTRL
#define RENDER_MOD_CTRL 0xcf2c /* MCR */
#endif
#ifndef COMP_MOD_CTRL
#define COMP_MOD_CTRL 0xcf30 /* MCR */
#endif
#ifndef XELPMP_GSC_MOD_CTRL
#define XELPMP_GSC_MOD_CTRL 0xcf30 /* media GT only, not MCR there */
#endif
#ifndef XEHP_VDBX_MOD_CTRL
#define XEHP_VDBX_MOD_CTRL 0xcf34 /* MCR */
#endif
#ifndef XELPMP_VDBX_MOD_CTRL
#define XELPMP_VDBX_MOD_CTRL 0xcf34 /* media GT only, not MCR there */
#endif
#ifndef XEHP_VEBX_MOD_CTRL
#define XEHP_VEBX_MOD_CTRL 0xcf38 /* MCR */
#endif
#ifndef FORCE_MISS_FTLB
#define FORCE_MISS_FTLB (1u << 3)
#endif
#ifndef XEHP_GAMCNTRL_CTRL
#define XEHP_GAMCNTRL_CTRL 0xcf54 /* MCR */
#endif
#ifndef INVALIDATION_BROADCAST_MODE_DIS
#define INVALIDATION_BROADCAST_MODE_DIS (1u << 12)
#endif
#ifndef GLOBAL_INVALIDATION_MODE
#define GLOBAL_INVALIDATION_MODE (1u << 2)
#endif
#ifndef SARB_CHICKEN1
#define SARB_CHICKEN1 0xe90c /* MCR */
#endif
#ifndef COMP_CKN_IN
#define COMP_CKN_IN 0x60000000u /* bits 30:29 */
#endif

/* ---- Per-engine registers, at the engine's base. */
#ifndef RING_PSMI_CTL
#define RING_PSMI_CTL(base) ((base) + 0x50)
#endif
#ifndef GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE
#define GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE (1u << 7)
#endif
#ifndef GEN8_RC_SEMA_IDLE_MSG_DISABLE
#define GEN8_RC_SEMA_IDLE_MSG_DISABLE (1u << 12)
#endif
#ifndef RING_MI_MODE
#define RING_MI_MODE(base) ((base) + 0x9c)
#endif
#ifndef TGL_NESTED_BB_EN
#define TGL_NESTED_BB_EN (1u << 12)
#endif
#ifndef RING_CMD_CCTL
#define RING_CMD_CCTL(base) ((base) + 0xc4)
#endif
#ifndef CMD_CCTL_WRITE_OVERRIDE_MASK
#define CMD_CCTL_WRITE_OVERRIDE_MASK 0x00003f80u /* bits 13:7 */
#endif
#ifndef CMD_CCTL_READ_OVERRIDE_MASK
#define CMD_CCTL_READ_OVERRIDE_MASK 0x0000007fu /* bits 6:0 */
#endif
#ifndef CMD_CCTL_MOCS_MASK
#define CMD_CCTL_MOCS_MASK 0x00003fffu /* both of the above */
#endif
#ifndef CMD_CCTL_MOCS_OVERRIDE
#define CMD_CCTL_MOCS_OVERRIDE(write, read) \
	(((((write) << 1) << 7) & 0x3f80u) | (((read) << 1) & 0x7fu))
#endif
#ifndef ECOSKPD
#define ECOSKPD(base) ((base) + 0x1d0)
#endif
#ifndef XEHP_BLITTER_SCHEDULING_MODE_MASK
#define XEHP_BLITTER_SCHEDULING_MODE_MASK 0x00001800u /* bits 12:11 */
#endif
#ifndef XEHP_BLITTER_ROUND_ROBIN_MODE
#define XEHP_BLITTER_ROUND_ROBIN_MODE (1u << 11) /* field value 1 */
#endif
#ifndef BLIT_CCTL
#define BLIT_CCTL(base) ((base) + 0x204)
#endif
#ifndef BLIT_CCTL_DST_MOCS_MASK
#define BLIT_CCTL_DST_MOCS_MASK 0x00007f00u /* bits 14:8 */
#endif
#ifndef BLIT_CCTL_SRC_MOCS_MASK
#define BLIT_CCTL_SRC_MOCS_MASK 0x0000007fu /* bits 6:0 */
#endif
#ifndef BLIT_CCTL_MASK
#define BLIT_CCTL_MASK 0x00007f7fu /* both of the above */
#endif
#ifndef BLIT_CCTL_MOCS
#define BLIT_CCTL_MOCS(dst, src) (((((dst) << 1) << 8) & 0x7f00u) | (((src) << 1) & 0x7fu))
#endif
#ifndef RING_CTX_TIMESTAMP
#define RING_CTX_TIMESTAMP(base) ((base) + 0x3a8)
#endif
#ifndef VDBOX_CGCTL3F10
#define VDBOX_CGCTL3F10(base) ((base) + 0x3f10)
#endif
#ifndef IECPUNIT_CLKGATE_DIS
#define IECPUNIT_CLKGATE_DIS (1u << 22)
#endif
#ifndef VDBOX_CGCTL3F1C
#define VDBOX_CGCTL3F1C(base) ((base) + 0x3f1c)
#endif
#ifndef MFXPIPE_CLKGATE_DIS
#define MFXPIPE_CLKGATE_DIS (1u << 3)
#endif

/* ---- The FORCE_TO_NONPRIV whitelist: an entry is the register's offset with the
 * access a client gets and how many consecutive registers it covers in the
 * bits the offset leaves free. */
#ifndef RING_FORCE_TO_NONPRIV_DENY
#define RING_FORCE_TO_NONPRIV_DENY (1u << 30)
#endif
#ifndef RING_FORCE_TO_NONPRIV_ACCESS_RW
#define RING_FORCE_TO_NONPRIV_ACCESS_RW (0u << 28)
#endif
#ifndef RING_FORCE_TO_NONPRIV_ACCESS_RD
#define RING_FORCE_TO_NONPRIV_ACCESS_RD (1u << 28)
#endif
#ifndef RING_FORCE_TO_NONPRIV_ACCESS_WR
#define RING_FORCE_TO_NONPRIV_ACCESS_WR (2u << 28)
#endif
#ifndef RING_FORCE_TO_NONPRIV_ACCESS_INVALID
#define RING_FORCE_TO_NONPRIV_ACCESS_INVALID (3u << 28)
#endif
#ifndef RING_FORCE_TO_NONPRIV_ACCESS_MASK
#define RING_FORCE_TO_NONPRIV_ACCESS_MASK (3u << 28)
#endif
#ifndef RING_FORCE_TO_NONPRIV_RANGE_1
#define RING_FORCE_TO_NONPRIV_RANGE_1 (0u << 0)
#endif
#ifndef RING_FORCE_TO_NONPRIV_RANGE_4
#define RING_FORCE_TO_NONPRIV_RANGE_4 (1u << 0)
#endif
#ifndef RING_FORCE_TO_NONPRIV_RANGE_16
#define RING_FORCE_TO_NONPRIV_RANGE_16 (2u << 0)
#endif
#ifndef RING_FORCE_TO_NONPRIV_RANGE_64
#define RING_FORCE_TO_NONPRIV_RANGE_64 (3u << 0)
#endif
#ifndef RING_FORCE_TO_NONPRIV_RANGE_MASK
#define RING_FORCE_TO_NONPRIV_RANGE_MASK (3u << 0)
#endif
#ifndef RING_FORCE_TO_NONPRIV_MASK_VALID
#define RING_FORCE_TO_NONPRIV_MASK_VALID 0x70000003u /* range, access, deny */
#endif
#ifndef RING_MAX_NONPRIV_SLOTS
#define RING_MAX_NONPRIV_SLOTS 12
#endif

/* ---- Engine bases. */
#ifndef RENDER_RING_BASE
#define RENDER_RING_BASE 0x02000
#endif
#ifndef BLT_RING_BASE
#define BLT_RING_BASE 0x22000
#endif
#ifndef GEN11_BSD_RING_BASE
#define GEN11_BSD_RING_BASE 0x1c0000
#endif
#ifndef GEN11_BSD2_RING_BASE
#define GEN11_BSD2_RING_BASE 0x1c4000
#endif
#ifndef GEN11_BSD3_RING_BASE
#define GEN11_BSD3_RING_BASE 0x1d0000
#endif
#ifndef GEN11_BSD4_RING_BASE
#define GEN11_BSD4_RING_BASE 0x1d4000
#endif

/* ---- Render and compute: command streamer registers at fixed offsets. */
#ifndef GEN7_FF_THREAD_MODE
#define GEN7_FF_THREAD_MODE 0x20a0
#endif
#ifndef GEN12_FF_TESSELATION_DOP_GATE_DISABLE
#define GEN12_FF_TESSELATION_DOP_GATE_DISABLE (1u << 19)
#endif
#ifndef GEN7_FF_SLICE_CS_CHICKEN1
#define GEN7_FF_SLICE_CS_CHICKEN1 0x20e0
#endif
#ifndef GEN9_FFSC_PERCTX_PREEMPT_CTRL
#define GEN9_FFSC_PERCTX_PREEMPT_CTRL (1u << 14)
#endif
#ifndef GEN9_CS_DEBUG_MODE1
#define GEN9_CS_DEBUG_MODE1 0x20ec
#endif
#ifndef FF_DOP_CLOCK_GATE_DISABLE
#define FF_DOP_CLOCK_GATE_DISABLE (1u << 1)
#endif
#ifndef PS_INVOCATION_COUNT
#define PS_INVOCATION_COUNT 0x2348
#endif
#ifndef GEN8_CS_CHICKEN1
#define GEN8_CS_CHICKEN1(base) ((base) + 0x580) /* 0x2580 on the render engine */
#endif
#ifndef GEN9_PREEMPT_GPGPU_LEVEL
#define GEN9_PREEMPT_GPGPU_LEVEL(hi, lo) (((hi) << 2) | ((lo) << 1))
#endif
#ifndef GEN9_PREEMPT_GPGPU_LEVEL_MASK
#define GEN9_PREEMPT_GPGPU_LEVEL_MASK 0x6u /* GEN9_PREEMPT_GPGPU_LEVEL(1, 1) */
#endif
#ifndef GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL
#define GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL 0x2u /* GEN9_PREEMPT_GPGPU_LEVEL(0, 1) */
#endif
#ifndef DRAW_WATERMARK
#define DRAW_WATERMARK 0x26c0
#endif
#ifndef VERT_WM_VAL
#define VERT_WM_VAL 0x000003ffu /* bits 9:0 */
#endif

/* ---- The 3D pipeline's chicken registers (context state). */
#ifndef GEN8_WM_CHICKEN2
#define GEN8_WM_CHICKEN2 0x5584 /* MCR */
#endif
#ifndef WAIT_ON_DEPTH_STALL_DONE_DISABLE
#define WAIT_ON_DEPTH_STALL_DONE_DISABLE (1u << 5)
#endif
#ifndef CHICKEN_RASTER_2
#define CHICKEN_RASTER_2 0x6208 /* MCR */
#endif
#ifndef TBIMR_FAST_CLIP
#define TBIMR_FAST_CLIP (1u << 5)
#endif
#ifndef VFLSKPD
#define VFLSKPD 0x62a8 /* MCR */
#endif
#ifndef VF_PREFETCH_TLB_DIS
#define VF_PREFETCH_TLB_DIS (1u << 5)
#endif
#ifndef GEN12_FF_MODE2
#define GEN12_FF_MODE2 0x6604
#endif
#ifndef XEHP_FF_MODE2
#define XEHP_FF_MODE2 0x6604 /* MCR */
#endif
#ifndef FF_MODE2_GS_TIMER_MASK
#define FF_MODE2_GS_TIMER_MASK 0xff000000u /* bits 31:24 */
#endif
#ifndef FF_MODE2_GS_TIMER_224
#define FF_MODE2_GS_TIMER_224 (224u << 24)
#endif
#ifndef FF_MODE2_TDS_TIMER_MASK
#define FF_MODE2_TDS_TIMER_MASK 0x00ff0000u /* bits 23:16 */
#endif
#ifndef FF_MODE2_TDS_TIMER_128
#define FF_MODE2_TDS_TIMER_128 (4u << 16)
#endif
#ifndef CACHE_MODE_1
#define CACHE_MODE_1 0x7004
#endif
#ifndef MSAA_OPTIMIZATION_REDUC_DISABLE
#define MSAA_OPTIMIZATION_REDUC_DISABLE (1u << 11)
#endif
#ifndef GEN7_COMMON_SLICE_CHICKEN1
#define GEN7_COMMON_SLICE_CHICKEN1 0x7010
#endif
#ifndef HIZ_CHICKEN
#define HIZ_CHICKEN 0x7018
#endif
#ifndef HZ_DEPTH_TEST_LE_GE_OPT_DISABLE
#define HZ_DEPTH_TEST_LE_GE_OPT_DISABLE (1u << 13)
#endif
#ifndef DG1_HZ_READ_SUPPRESSION_OPTIMIZATION_DISABLE
#define DG1_HZ_READ_SUPPRESSION_OPTIMIZATION_DISABLE (1u << 14)
#endif
#ifndef XEHP_PSS_MODE2
#define XEHP_PSS_MODE2 0x703c /* MCR */
#endif
#ifndef SCOREBOARD_STALL_FLUSH_CONTROL
#define SCOREBOARD_STALL_FLUSH_CONTROL (1u << 5)
#endif
#ifndef XEHP_PSS_CHICKEN
#define XEHP_PSS_CHICKEN 0x7044 /* MCR */
#endif
#ifndef FD_END_COLLECT
#define FD_END_COLLECT (1u << 5)
#endif
#ifndef COMMON_SLICE_CHICKEN4
#define COMMON_SLICE_CHICKEN4 0x7300
#endif
#ifndef DISABLE_TDC_LOAD_BALANCING_CALC
#define DISABLE_TDC_LOAD_BALANCING_CALC (1u << 6)
#endif
#ifndef GEN11_COMMON_SLICE_CHICKEN3
#define GEN11_COMMON_SLICE_CHICKEN3 0x7304
#endif
#ifndef XEHP_COMMON_SLICE_CHICKEN3
#define XEHP_COMMON_SLICE_CHICKEN3 0x7304 /* MCR */
#endif
#ifndef GEN12_DISABLE_CPS_AWARE_COLOR_PIPE
#define GEN12_DISABLE_CPS_AWARE_COLOR_PIPE (1u << 9)
#endif
#ifndef DG1_FLOAT_POINT_BLEND_OPT_STRICT_MODE_EN
#define DG1_FLOAT_POINT_BLEND_OPT_STRICT_MODE_EN (1u << 12)
#endif
#ifndef XEHP_SLICE_COMMON_ECO_CHICKEN1
#define XEHP_SLICE_COMMON_ECO_CHICKEN1 0x731c /* MCR */
#endif
#ifndef MSC_MSAA_REODER_BUF_BYPASS_DISABLE
#define MSC_MSAA_REODER_BUF_BYPASS_DISABLE (1u << 14)
#endif
#ifndef VF_PREEMPTION
#define VF_PREEMPTION 0x83a4
#endif
#ifndef PREEMPTION_VERTEX_COUNT
#define PREEMPTION_VERTEX_COUNT 0x0000ffffu /* bits 15:0 */
#endif
#ifndef VFG_PREEMPTION_CHICKEN
#define VFG_PREEMPTION_CHICKEN 0x83b4
#endif
#ifndef POLYGON_TRIFAN_LINELOOP_DISABLE
#define POLYGON_TRIFAN_LINELOOP_DISABLE (1u << 4)
#endif

/* ---- The L3 and the units of the subslices. */
#ifndef GEN8_GARBCNTL
#define GEN8_GARBCNTL 0xb004
#endif
#ifndef GEN12_BUS_HASH_CTL_BIT_EXC
#define GEN12_BUS_HASH_CTL_BIT_EXC (1u << 7)
#endif
#ifndef XEHP_L3SQCREG5
#define XEHP_L3SQCREG5 0xb158 /* MCR */
#endif
#ifndef L3_PWM_TIMER_INIT_VAL_MASK
#define L3_PWM_TIMER_INIT_VAL_MASK 0x000003ffu /* bits 9:0 */
#endif
#ifndef GEN10_SAMPLER_MODE
#define GEN10_SAMPLER_MODE 0xe18c /* MCR */
#endif
#ifndef GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE
#define GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE (1u << 0)
#endif
#ifndef MTL_DISABLE_SAMPLER_SC_OOO
#define MTL_DISABLE_SAMPLER_SC_OOO (1u << 3)
#endif
#ifndef SC_DISABLE_POWER_OPTIMIZATION_EBB
#define SC_DISABLE_POWER_OPTIMIZATION_EBB (1u << 9)
#endif
#ifndef ENABLE_SMALLPL
#define ENABLE_SMALLPL (1u << 15)
#endif
#ifndef GEN10_CACHE_MODE_SS
#define GEN10_CACHE_MODE_SS 0xe420 /* MCR */
#endif
#ifndef DISABLE_PREFETCH_INTO_IC
#define DISABLE_PREFETCH_INTO_IC (1u << 3)
#endif
#ifndef ENABLE_PREFETCH_INTO_IC
#define ENABLE_PREFETCH_INTO_IC (1u << 3)
#endif
#ifndef ENABLE_EU_COUNT_FOR_TDL_FLUSH
#define ENABLE_EU_COUNT_FOR_TDL_FLUSH (1u << 10)
#endif
#ifndef GEN9_ROW_CHICKEN4
#define GEN9_ROW_CHICKEN4 0xe48c /* MCR */
#endif
#ifndef THREAD_EX_ARB_MODE
#define THREAD_EX_ARB_MODE 0x0000000cu /* bits 3:2 */
#endif
#ifndef THREAD_EX_ARB_MODE_RR_AFTER_DEP
#define THREAD_EX_ARB_MODE_RR_AFTER_DEP (2u << 2)
#endif
#ifndef GEN12_DISABLE_TDL_PUSH
#define GEN12_DISABLE_TDL_PUSH (1u << 9)
#endif
#ifndef XEHP_DIS_BBL_SYSPIPE
#define XEHP_DIS_BBL_SYSPIPE (1u << 11)
#endif
#ifndef GEN9_ROW_CHICKEN3
#define GEN9_ROW_CHICKEN3 0xe49c /* MCR */
#endif
#ifndef MTL_DISABLE_FIX_FOR_EOT_FLUSH
#define MTL_DISABLE_FIX_FOR_EOT_FLUSH (1u << 9)
#endif
#ifndef GEN8_ROW_CHICKEN2
#define GEN8_ROW_CHICKEN2 0xe4f4 /* MCR */
#endif
#ifndef XELPG_DISABLE_TDL_SVHS_GATING
#define XELPG_DISABLE_TDL_SVHS_GATING (1u << 1)
#endif
#ifndef GEN12_PUSH_CONST_DEREF_HOLD_DIS
#define GEN12_PUSH_CONST_DEREF_HOLD_DIS (1u << 8)
#endif
#ifndef GEN12_DISABLE_EARLY_READ
#define GEN12_DISABLE_EARLY_READ (1u << 14)
#endif
#ifndef GEN12_DISABLE_READ_SUPPRESSION
#define GEN12_DISABLE_READ_SUPPRESSION (1u << 15)
#endif
#ifndef RT_CTRL
#define RT_CTRL 0xe530 /* MCR */
#endif
#ifndef STACKID_CTRL
#define STACKID_CTRL 0x00000060u /* bits 6:5 */
#endif
#ifndef STACKID_CTRL_512
#define STACKID_CTRL_512 (2u << 5)
#endif
#ifndef XEHP_HDC_CHICKEN0
#define XEHP_HDC_CHICKEN0 0xe5f0 /* MCR */
#endif
#ifndef DIS_ATOMIC_CHAINING_TYPED_WRITES
#define DIS_ATOMIC_CHAINING_TYPED_WRITES (1u << 3)
#endif
#ifndef LSC_L1_FLUSH_CTL_3D_DATAPORT_FLUSH_EVENTS_MASK
#define LSC_L1_FLUSH_CTL_3D_DATAPORT_FLUSH_EVENTS_MASK 0x00003800u /* bits 13:11 */
#endif
#ifndef LSC_CHICKEN_BIT_0
#define LSC_CHICKEN_BIT_0 0xe7c8 /* MCR */
#endif
#ifndef FORCE_1_SUB_MESSAGE_PER_FRAGMENT
#define FORCE_1_SUB_MESSAGE_PER_FRAGMENT (1u << 15)
#endif
#ifndef DISABLE_D8_D16_COASLESCE
#define DISABLE_D8_D16_COASLESCE (1u << 30)
#endif
#ifndef LSC_CHICKEN_BIT_0_UDW
#define LSC_CHICKEN_BIT_0_UDW 0xe7cc /* MCR: the upper half of the 64-bit LSC_CHICKEN_BIT_0 */
#endif
#ifndef DISABLE_128B_EVICTION_COMMAND_UDW
#define DISABLE_128B_EVICTION_COMMAND_UDW (1u << 4) /* bit 36 of the pair */
#endif
#ifndef MAXREQS_PER_BANK
#define MAXREQS_PER_BANK 0x000000e0u /* bits 39:37 of the pair */
#endif
#ifndef DIS_CHAIN_2XSIMD8
#define DIS_CHAIN_2XSIMD8 (1u << 23) /* bit 55 of the pair */
#endif
#ifndef UGM_FRAGMENT_THRESHOLD_TO_3
#define UGM_FRAGMENT_THRESHOLD_TO_3 (1u << 26) /* bit 58 of the pair */
#endif

/* ---- DG2's compute slices: which compute engine each one serves. */
#ifndef GEN12_RCU_MODE
#define GEN12_RCU_MODE 0x14800
#endif
#ifndef XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE
#define XEHP_RCU_MODE_FIXED_SLICE_CCS_MODE (1u << 1)
#endif
#ifndef XEHP_CCS_MODE
#define XEHP_CCS_MODE 0x14804
#endif
#ifndef XEHP_CCS_MODE_CSLICE_MASK
#define XEHP_CCS_MODE_CSLICE_MASK 0x7u /* CCS0-3 and a value for none */
#endif
#ifndef XEHP_CCS_MODE_CSLICE_WIDTH
#define XEHP_CCS_MODE_CSLICE_WIDTH 3
#endif
#ifndef XEHP_CCS_MODE_CSLICE
#define XEHP_CCS_MODE_CSLICE(cslice, ccs) ((ccs) << ((cslice) * 3))
#endif
#ifndef I915_MAX_CCS
#define I915_MAX_CCS 4
#endif

/* ---- Gen11 (Ice Lake, Elkhart Lake, Jasper Lake): the GT, engine and context settings. */
#ifndef GEN11_GACB_PERF_CTRL
#define GEN11_GACB_PERF_CTRL 0x4b80
#endif
#ifndef GEN11_HASH_CTRL_MASK
#define GEN11_HASH_CTRL_MASK ((0x3u << 12) | (0xfu << 0))
#endif
#ifndef GEN11_HASH_CTRL_BIT0
#define GEN11_HASH_CTRL_BIT0 (1u << 0)
#endif
#ifndef GEN11_HASH_CTRL_BIT4
#define GEN11_HASH_CTRL_BIT4 (1u << 12)
#endif
#ifndef GEN11_LSN_UNSLCVC
#define GEN11_LSN_UNSLCVC 0xb43c
#endif
#ifndef GEN11_LSN_UNSLCVC_GAFS_HALF_SF_MAXALLOC
#define GEN11_LSN_UNSLCVC_GAFS_HALF_SF_MAXALLOC (1u << 7)
#endif
#ifndef GEN11_LSN_UNSLCVC_GAFS_HALF_CL2_MAXALLOC
#define GEN11_LSN_UNSLCVC_GAFS_HALF_CL2_MAXALLOC (1u << 9)
#endif
#ifndef GEN8_GAMW_ECO_DEV_RW_IA
#define GEN8_GAMW_ECO_DEV_RW_IA 0x4080
#endif
#ifndef GAMW_ECO_DEV_CTX_RELOAD_DISABLE
#define GAMW_ECO_DEV_CTX_RELOAD_DISABLE (1u << 7)
#endif
#ifndef GAMT_CHKN_BIT_REG
#define GAMT_CHKN_BIT_REG 0x4ab8
#endif
#ifndef GAMT_CHKN_DISABLE_L3_COH_PIPE
#define GAMT_CHKN_DISABLE_L3_COH_PIPE (1u << 31)
#endif
#ifndef VSUNIT_CLKGATE_DIS
#define VSUNIT_CLKGATE_DIS (1u << 3) /* UNSLICE_UNIT_LEVEL_CLKGATE */
#endif
#ifndef HSUNIT_CLKGATE_DIS
#define HSUNIT_CLKGATE_DIS (1u << 8) /* UNSLICE_UNIT_LEVEL_CLKGATE */
#endif
#ifndef PSDUNIT_CLKGATE_DIS
#define PSDUNIT_CLKGATE_DIS (1u << 5) /* UNSLICE_UNIT_LEVEL_CLKGATE2 */
#endif
#ifndef GWUNIT_CLKGATE_DIS
#define GWUNIT_CLKGATE_DIS (1u << 16) /* GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE */
#endif
#ifndef GEN11_SLICE_UNIT_LEVEL_CLKGATE
#define GEN11_SLICE_UNIT_LEVEL_CLKGATE 0x94d4
#endif
#ifndef L3_CLKGATE_DIS
#define L3_CLKGATE_DIS (1u << 16)
#endif
#ifndef L3_CR2X_CLKGATE_DIS
#define L3_CR2X_CLKGATE_DIS (1u << 17)
#endif
#ifndef GEN8_L3CNTLREG
#define GEN8_L3CNTLREG 0x7034
#endif
#ifndef GEN8_ERRDETBCTRL
#define GEN8_ERRDETBCTRL (1u << 9)
#endif
#ifndef ICL_HDC_MODE
#define ICL_HDC_MODE 0xe5f4 /* MCR */
#endif
#ifndef FLOAT_BLEND_OPTIMIZATION_ENABLE
#define FLOAT_BLEND_OPTIMIZATION_ENABLE (1u << 4) /* GEN10_CACHE_MODE_SS */
#endif
#ifndef GEN11_SAMPLER_ENABLE_HEADLESS_MSG
#define GEN11_SAMPLER_ENABLE_HEADLESS_MSG (1u << 5) /* GEN10_SAMPLER_MODE */
#endif
#ifndef IVB_FBC_RT_BASE
#define IVB_FBC_RT_BASE 0x7020
#endif
#ifndef IVB_FBC_RT_BASE_UPPER
#define IVB_FBC_RT_BASE_UPPER 0x7024
#endif
#ifndef ILK_FBC_RT_VALID
#define ILK_FBC_RT_VALID (1u << 0)
#endif
#ifndef GEN11_DIS_PICK_2ND_EU
#define GEN11_DIS_PICK_2ND_EU (1u << 7) /* GEN9_ROW_CHICKEN4 */
#endif
#ifndef _3D_CHICKEN3_AA_LINE_QUALITY_FIX_ENABLE
#define _3D_CHICKEN3_AA_LINE_QUALITY_FIX_ENABLE (1u << 5)
#endif
#ifndef GEN11_ARBITRATION_PRIO_ORDER_MASK
#define GEN11_ARBITRATION_PRIO_ORDER_MASK (0x3fu << 22) /* GEN8_GARBCNTL */
#endif
#ifndef GEN11_HASH_CTRL_EXCL_MASK
#define GEN11_HASH_CTRL_EXCL_MASK 0x7fu /* GEN8_GARBCNTL */
#endif
#ifndef GEN11_HASH_CTRL_EXCL_BIT0
#define GEN11_HASH_CTRL_EXCL_BIT0 0x1u
#endif
#ifndef GEN11_GLBLINVL
#define GEN11_GLBLINVL 0xb404
#endif
#ifndef GEN11_BANK_HASH_ADDR_EXCL_MASK
#define GEN11_BANK_HASH_ADDR_EXCL_MASK (0x7fu << 5)
#endif
#ifndef GEN11_BANK_HASH_ADDR_EXCL_BIT0
#define GEN11_BANK_HASH_ADDR_EXCL_BIT0 (1u << 5)
#endif
#ifndef GEN11_LQSC_CLEAN_EVICT_DISABLE
#define GEN11_LQSC_CLEAN_EVICT_DISABLE (1u << 6) /* GEN8_L3SQCREG4 */
#endif
#ifndef GEN7_SARCHKMD
#define GEN7_SARCHKMD 0xb000
#endif
#ifndef GEN7_DISABLE_SAMPLER_PREFETCH
#define GEN7_DISABLE_SAMPLER_PREFETCH (1u << 30)
#endif
#ifndef GEN11_SCRATCH2
#define GEN11_SCRATCH2 0xb140 /* MCR */
#endif
#ifndef GEN11_COHERENT_PARTIAL_WRITE_MERGE_ENABLE
#define GEN11_COHERENT_PARTIAL_WRITE_MERGE_ENABLE (1u << 19)
#endif
#ifndef GEN11_ENABLE_32_PLANE_MODE
#define GEN11_ENABLE_32_PLANE_MODE (1u << 7) /* GEN9_CSFE_CHICKEN1_RCS */
#endif
#ifndef GEN9_SLICE_COMMON_ECO_CHICKEN1
#define GEN9_SLICE_COMMON_ECO_CHICKEN1 0x731c
#endif
#ifndef GEN11_VCS2_MOCS_BASE
#define GEN11_VCS2_MOCS_BASE 0x10000 /* Ice Lake's third video engine */
#endif

/* ---- Gen9 platform settings beyond Skylake's. */
#ifndef GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION
#define GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION (1u << 8) /* COMMON_SLICE_CHICKEN2 */
#endif
#ifndef GEN8_HALF_SLICE_CHICKEN1
#define GEN8_HALF_SLICE_CHICKEN1 0xe100 /* MCR */
#endif
#ifndef GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE
#define GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE (1u << 4)
#endif
#ifndef GEN9_WM_CHICKEN3
#define GEN9_WM_CHICKEN3 0x5588
#endif
#ifndef GEN9_FACTOR_IN_CLR_VAL_HIZ
#define GEN9_FACTOR_IN_CLR_VAL_HIZ (1u << 9)
#endif
#ifndef FF_SLICE_CS_CHICKEN2
#define FF_SLICE_CS_CHICKEN2 0x20e4
#endif
#ifndef GEN9_POOLED_EU_LOAD_BALANCING_FIX_DISABLE
#define GEN9_POOLED_EU_LOAD_BALANCING_FIX_DISABLE (1u << 10)
#endif

/* ---- Gen8 (Broadwell, Cherryview): context and clock-gating settings. */
#ifndef INSTPM
#define INSTPM 0x20c0
#endif
#ifndef INSTPM_FORCE_ORDERING
#define INSTPM_FORCE_ORDERING (1u << 7)
#endif
#ifndef HDC_DONOT_FETCH_MEM_WHEN_MASKED
#define HDC_DONOT_FETCH_MEM_WHEN_MASKED (1u << 11) /* HDC_CHICKEN0 */
#endif
#ifndef HDC_FENCE_DEST_SLM_DISABLE
#define HDC_FENCE_DEST_SLM_DISABLE (1u << 14) /* HDC_CHICKEN0 */
#endif
#ifndef CACHE_MODE_0_GEN7
#define CACHE_MODE_0_GEN7 0x7000
#endif
#ifndef HIZ_RAW_STALL_OPT_DISABLE
#define HIZ_RAW_STALL_OPT_DISABLE (1u << 2)
#endif
#ifndef GEN7_GT_MODE
#define GEN7_GT_MODE 0x7008
#endif
#ifndef GEN6_WIZ_HASHING
#define GEN6_WIZ_HASHING(hi, lo) (((hi) << 9) | ((lo) << 7))
#endif
#ifndef GEN6_WIZ_HASHING_MASK
#define GEN6_WIZ_HASHING_MASK GEN6_WIZ_HASHING(1, 1)
#endif
#ifndef GEN6_WIZ_HASHING_16x4
#define GEN6_WIZ_HASHING_16x4 GEN6_WIZ_HASHING(1, 0)
#endif
#ifndef DOP_CLOCK_GATING_DISABLE
#define DOP_CLOCK_GATING_DISABLE (1u << 0) /* GEN8_ROW_CHICKEN2 */
#endif
#ifndef CHV_HZ_8X8_MODE_IN_1X
#define CHV_HZ_8X8_MODE_IN_1X (1u << 15) /* HIZ_CHICKEN */
#endif
#ifndef GEN6_UCGCTL1
#define GEN6_UCGCTL1 0x9400
#endif
#ifndef GEN6_CSUNIT_CLOCK_GATE_DISABLE
#define GEN6_CSUNIT_CLOCK_GATE_DISABLE (1u << 7)
#endif
#ifndef GEN6_EU_TCUNIT_CLOCK_GATE_DISABLE
#define GEN6_EU_TCUNIT_CLOCK_GATE_DISABLE (1u << 16)
#endif
#ifndef GEN8_UCGCTL6
#define GEN8_UCGCTL6 0x9430
#endif
#ifndef GEN8_SDEUNIT_CLOCK_GATE_DISABLE
#define GEN8_SDEUNIT_CLOCK_GATE_DISABLE (1u << 14)
#endif
#ifndef HSW_ECOCHK_ARB_PRIO_SOL
#define HSW_ECOCHK_ARB_PRIO_SOL (1u << 6) /* GAM_ECOCHK */
#endif
#ifndef GEN7_FF_VS_REF_CNT_FFME
#define GEN7_FF_VS_REF_CNT_FFME (1u << 15) /* GEN7_FF_THREAD_MODE */
#endif
#ifndef GEN8_FF_DS_REF_CNT_FFME
#define GEN8_FF_DS_REF_CNT_FFME (1u << 19)
#endif
#ifndef GEN7_DOP_CLOCK_GATE_ENABLE
#define GEN7_DOP_CLOCK_GATE_ENABLE (1u << 0) /* GEN7_MISCCPCTL */
#endif
#ifndef GEN8_L3SQCREG1
#define GEN8_L3SQCREG1 0xb100 /* MCR */
#endif
#ifndef L3_PRIO_CREDITS_MASK
#define L3_PRIO_CREDITS_MASK ((0x1fu << 19) | (0x1fu << 14))
#endif
#ifndef L3_GENERAL_PRIO_CREDITS
#define L3_GENERAL_PRIO_CREDITS(x) ((((uint32_t)(x)) >> 1) << 19)
#endif
#ifndef L3_HIGH_PRIO_CREDITS
#define L3_HIGH_PRIO_CREDITS(x) ((((uint32_t)(x)) >> 1) << 14)
#endif

#endif
