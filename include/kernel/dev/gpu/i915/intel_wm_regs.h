// LikeOS -- registers of the display watermarks, the data buffer, CDCLK
// and the memory bandwidth mailbox.
//
// What intel_skl_wm.c, intel_hsw_wm.c, intel_cdclk_set.c and intel_bw.c
// program beyond the definitions already in i915_reg.h and
// intel_xelpdp_regs.h: the watermark and buffer-allocation fields as the
// later display versions widened them, the SAGV copies of the
// watermarks, the arbiter controls, the CDCLK divider/squasher/PLL
// fields, the pcode mailbox commands for CDCLK, SAGV and the memory
// subsystem, and the memory controller's registers mirrored into the
// MMIO window.  Every name already defined elsewhere with the same
// meaning is guarded; where an existing name has a narrower field, the
// wider one is given a name of its own here.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_WM_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_WM_REGS_H

#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>

/* ---- universal plane and cursor watermarks (Gen9+) -------------------------- */

#ifndef PLANE_WM
#define PLANE_WM(pipe, plane, level) (0x70240 + (pipe) * 0x1000 + (plane) * 0x100 + (level) * 4)
#endif
#ifndef PLANE_WM_TRANS
#define PLANE_WM_TRANS(pipe, plane) (0x70268 + (pipe) * 0x1000 + (plane) * 0x100)
#endif
/* Display version 13+ (integrated): the watermarks the hardware switches
 * to by itself while the memory changes frequency. */
#define PLANE_WM_SAGV(pipe, plane) (0x70258 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_WM_SAGV_TRANS(pipe, plane) (0x7025C + (pipe) * 0x1000 + (plane) * 0x100)
#ifndef PLANE_BUF_CFG
#define PLANE_BUF_CFG(pipe, plane) (0x7027C + (pipe) * 0x1000 + (plane) * 0x100)
#endif
#ifndef PLANE_NV12_BUF_CFG
#define PLANE_NV12_BUF_CFG(pipe, plane) (0x70278 + (pipe) * 0x1000 + (plane) * 0x100)
#endif
/* Display version 30+: the minimum and interim allocations the hardware
 * falls back to on its own. */
#define PLANE_MIN_BUF_CFG(pipe, plane) (0x70274 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_AUTO_MIN_DBUF_EN (1u << 31)
#define PLANE_MIN_DBUF_BLOCKS(x) (((uint32_t)(x) & 0x1fff) << 16)
#define PLANE_INTERIM_DBUF_BLOCKS(x) ((uint32_t)(x) & 0x1fff)

#ifndef CUR_WM
#define CUR_WM(pipe, level) (0x70140 + (pipe) * 0x1000 + (level) * 4)
#endif
#ifndef CUR_WM_TRANS
#define CUR_WM_TRANS(pipe) (0x70168 + (pipe) * 0x1000)
#endif
#define CUR_WM_SAGV(pipe) (0x70158 + (pipe) * 0x1000)
#define CUR_WM_SAGV_TRANS(pipe) (0x7015C + (pipe) * 0x1000)
#ifndef CUR_BUF_CFG
#define CUR_BUF_CFG(pipe) (0x7017C + (pipe) * 0x1000)
#endif

/* One watermark level: plane and cursor share the layout but the cursor's
 * block count is a bit narrower. */
#define SKL_WM_EN (1u << 31)
#define SKL_WM_IGNORE_LINES (1u << 30)
#define SKL_WM_AUTO_MIN_ALLOC_EN (1u << 29) /* display version 30+ */
#define SKL_WM_LINES(x) (((uint32_t)(x) & 0x1fff) << 14)
#define SKL_PLANE_WM_BLOCKS(x) ((uint32_t)(x) & 0x1fff)
#define SKL_CUR_WM_BLOCKS(x) ((uint32_t)(x) & 0xfff)

/* A plane's share of the data buffer: first and last block. */
#define SKL_PLANE_BUF_END(x) (((uint32_t)(x) & 0x1fff) << 16)
#define SKL_PLANE_BUF_START(x) ((uint32_t)(x) & 0x1fff)
#define SKL_CUR_BUF_END(x) (((uint32_t)(x) & 0xfff) << 16)
#define SKL_CUR_BUF_START(x) ((uint32_t)(x) & 0xfff)

#ifndef PLANE_CTL
#define PLANE_CTL(pipe, plane) (0x70180 + (pipe) * 0x1000 + (plane) * 0x100)
#endif
#ifndef PLANE_SURF
#define PLANE_SURF(pipe, plane) (0x7019C + (pipe) * 0x1000 + (plane) * 0x100)
#endif
#define SKL_PLANE_CTL_ENABLE (1u << 31)

/* ---- the arbiter --------------------------------------------------------------- */

#define DISP_ARB_CTL 0x45000
#define DISP_FBC_WM_DIS (1u << 15)
#define DISP_ARB_CTL2 0x45004
#define DISP_DATA_PARTITION_5_6 (1u << 6)
#define DISP_IPC_ENABLE (1u << 3)

/* The line time of a pipe, in 1/8 us, and the same for IPS (Haswell and
 * Broadwell, from CDCLK). */
#define WM_LINETIME(pipe) (0x45270 + (pipe) * 4)
#define HSW_LINETIME(x) ((uint32_t)(x) & 0x1ff)
#define HSW_IPS_LINETIME(x) (((uint32_t)(x) & 0x1ff) << 16)

/* Lunar Lake on: how long the package may stay in a C-state. */
#define LNL_PKG_C_LATENCY 0x46460
#define LNL_ADDED_WAKE_TIME(x) (((uint32_t)(x) & 0x1fff) << 16)
#define LNL_PKG_C_LATENCY_FIELD(x) ((uint32_t)(x) & 0x1fff)
#define LNL_PKG_C_LATENCY_MAX 0x1fff

/* ---- the data buffer, MBUS (display version 11+) ------------------------------ */

#ifndef DBUF_CTL_S
#define DBUF_CTL_S(slice) ((slice) == 0 ? 0x45008 : (slice) == 1 ? 0x44FE8 : \
			   (slice) == 2 ? 0x44300 : 0x44304)
#endif
#define SKL_DBUF_POWER_REQUEST (1u << 31)
#define SKL_DBUF_POWER_STATE (1u << 30)
#define SKL_DBUF_TRACKER_STATE_SERVICE_MASK (0x1fu << 19)
#define SKL_DBUF_TRACKER_STATE_SERVICE(x) (((uint32_t)(x) & 0x1f) << 19)
#define SKL_DBUF_MIN_TRACKER_STATE_SERVICE_MASK (0x7u << 16) /* ADL-P+ */
#define SKL_DBUF_MIN_TRACKER_STATE_SERVICE(x) (((uint32_t)(x) & 0x7) << 16)
#define XE3P_DBUF_MIN_TRACKER_STATE_SERVICE_MASK (0x1fu << 16)
#define XE3P_DBUF_MIN_TRACKER_STATE_SERVICE(x) (((uint32_t)(x) & 0x1f) << 16)

#define SKL_MBUS_CTL 0x4438C
#define SKL_MBUS_JOIN (1u << 31)
#define SKL_MBUS_HASHING_MODE_MASK (1u << 30)
#define SKL_MBUS_HASHING_MODE_2x2 (0u << 30)
#define SKL_MBUS_HASHING_MODE_1x4 (1u << 30)
#define SKL_MBUS_JOIN_PIPE_SELECT_MASK (7u << 26)
#define SKL_MBUS_JOIN_PIPE_SELECT(pipe) (((uint32_t)(pipe) & 7) << 26)
#define SKL_MBUS_JOIN_PIPE_SELECT_NONE (7u << 26)
#define XE3P_MBUS_TRANSLATION_THROTTLE_MIN_MASK (0xfu << 13)
#define XE3P_MBUS_TRANSLATION_THROTTLE_MIN(x) (((uint32_t)(x) & 0xf) << 13)
#define MBUS_TRANSLATION_THROTTLE_MIN_MASK (0x7u << 13)
#define MBUS_TRANSLATION_THROTTLE_MIN(x) (((uint32_t)(x) & 0x7) << 13)

#define SKL_PIPE_MBUS_DBOX_CTL(pipe) (0x7003C + (pipe) * 0x1000)
#define SKL_MBUS_DBOX_B2B_TRANSACTIONS_MAX(x) ((uint32_t)(x) << 20)
#define SKL_MBUS_DBOX_B2B_TRANSACTIONS_DELAY(x) ((uint32_t)(x) << 17)
#define SKL_MBUS_DBOX_REGULATE_B2B_TRANSACTIONS_EN (1u << 16)
#define SKL_MBUS_DBOX_BW_CREDIT(x) ((uint32_t)(x) << 14)
#define SKL_MBUS_DBOX_BW_4CREDITS_MTL (2u << 14)
#define SKL_MBUS_DBOX_BW_8CREDITS_MTL (3u << 14)
#define SKL_MBUS_DBOX_B_CREDIT(x) ((uint32_t)(x) << 8)
#define SKL_MBUS_DBOX_I_CREDIT(x) ((uint32_t)(x) << 5)
#define SKL_MBUS_DBOX_A_CREDIT(x) ((uint32_t)(x) << 0)

#define SKL_MBUS_ABOX_CTL(x) ((x) == 0 ? 0x45038 : (x) == 1 ? 0x45048 : 0x4504C)
#define SKL_MBUS_ABOX_BW_CREDIT_MASK (3u << 20)
#define SKL_MBUS_ABOX_BW_CREDIT(x) ((uint32_t)(x) << 20)
#define SKL_MBUS_ABOX_B_CREDIT_MASK (0xfu << 16)
#define SKL_MBUS_ABOX_B_CREDIT(x) ((uint32_t)(x) << 16)
#define SKL_MBUS_ABOX_BT_CREDIT_POOL2_MASK (0x1fu << 8)
#define SKL_MBUS_ABOX_BT_CREDIT_POOL2(x) ((uint32_t)(x) << 8)
#define SKL_MBUS_ABOX_BT_CREDIT_POOL1_MASK (0x1fu << 0)
#define SKL_MBUS_ABOX_BT_CREDIT_POOL1(x) ((uint32_t)(x) << 0)

/* ---- Haswell / Broadwell watermarks ---------------------------------------------- */

#define HSW_WM0_PIPE(pipe) ((pipe) == 2 ? 0x45200 : 0x45100 + (pipe) * 4)
#define WM0_PIPE_PRIMARY(x) (((uint32_t)(x) & 0xffff) << 16)
#define WM0_PIPE_SPRITE(x) (((uint32_t)(x) & 0xff) << 8)
#define WM0_PIPE_CURSOR(x) ((uint32_t)(x) & 0xff)
#define HSW_WM1_LP 0x45108
#define HSW_WM2_LP 0x4510C
#define HSW_WM3_LP 0x45110
#define HSW_WM_LP_ENABLE (1u << 31)
#define WM_LP_LATENCY(x) (((uint32_t)(x) & 0x7f) << 24)
#define WM_LP_FBC_BDW(x) (((uint32_t)(x) & 0x1f) << 19)
#define WM_LP_FBC_ILK(x) (((uint32_t)(x) & 0xf) << 20)
#define WM_LP_PRIMARY(x) (((uint32_t)(x) & 0x7ff) << 8)
#define WM_LP_CURSOR(x) ((uint32_t)(x) & 0xff)
#define WM1S_LP_ILK 0x45120
#define WM2S_LP_IVB 0x45124
#define WM3S_LP_IVB 0x45128
#define WM_LP_SPRITE_ENABLE (1u << 31)
#define WM_LP_SPRITE(x) ((uint32_t)(x) & 0x7ff)
#define WM_MISC 0x45260
#define WM_MISC_DATA_PARTITION_5_6 (1u << 0)

/* ---- CDCLK ----------------------------------------------------------------------- */

#define SKL_CDCLK_CTL 0x46000
#define CDCLK_FREQ_SEL_MASK_SKL (3u << 26)
#define MDCLK_SOURCE_SEL_MASK (1u << 25) /* display version 20+ */
#define MDCLK_SOURCE_SEL_CD2XCLK (0u << 25)
#define MDCLK_SOURCE_SEL_CDCLK_PLL (1u << 25)
#define CD2X_DIV_SEL_MASK (3u << 22)
#define CD2X_DIV_SEL_1 (0u << 22)
#define CD2X_DIV_SEL_1_5 (1u << 22)
#define CD2X_DIV_SEL_2 (2u << 22)
#define CD2X_DIV_SEL_4 (3u << 22)
#define BXT_CD2X_PIPE_MASK (3u << 20)
#define BXT_CD2X_PIPE(pipe) (((uint32_t)(pipe) & 3) << 20)
#define BXT_CD2X_PIPE_NONE (3u << 20)
#define ICL_CD2X_PIPE_MASK (7u << 19)
#define ICL_CD2X_PIPE(pipe) ((((uint32_t)(pipe) << 1) & 7) << 19)
#define ICL_CD2X_PIPE_NONE (7u << 19)
#define CDCLK_DIVMUX_CD_OVERRIDE (1u << 19) /* before Ice Lake */
#define CD2X_SSA_PRECHARGE_ENABLE (1u << 16) /* Broxton, Gemini Lake */
#define CDCLK_DECIMAL_MASK 0x7ffu

/* The squasher of DG2 and display version 14+: a 16-cycle waveform. */
#define CDCLK_SQUASH_REG 0x46008
#define CDCLK_SQUASH_EN (1u << 31)
#define CDCLK_SQUASH_WINDOW(x) (((uint32_t)(x) & 0xf) << 24)
#define CDCLK_SQUASH_WAVE(x) ((uint32_t)(x) & 0xffff)
#define CDCLK_SQUASH_WINDOW_GET(v) (((v) >> 24) & 0xf)
#define CDCLK_SQUASH_WAVE_GET(v) ((v) & 0xffff)

/* The DE PLL of Broxton, the CDCLK PLL of Cannon Lake and later. */
#define CDCLK_PLL_ENABLE 0x46070
#define CDCLK_PLL_EN (1u << 31)
#define CDCLK_PLL_LOCKED (1u << 30)
#define CDCLK_PLL_FREQ_REQ (1u << 23)
#define CDCLK_PLL_FREQ_REQ_ACK (1u << 22)
#define CDCLK_PLL_RATIO(x) ((uint32_t)(x) & 0xff)
#define BXT_DE_PLL_CTL_REG 0x6D000
#define BXT_DE_PLL_RATIO(x) ((uint32_t)(x) & 0xff)

#define SKL_DSSM_REG 0x51004
#define CNL_DSSM_CDCLK_PLL_REFCLK_24MHz (1u << 31)
#define ICL_DSSM_REFCLK_MASK (7u << 29)
#define ICL_DSSM_REFCLK_24MHz (0u << 29)
#define ICL_DSSM_REFCLK_19_2MHz (1u << 29)
#define ICL_DSSM_REFCLK_38_4MHz (2u << 29)
#define SKL_DFSM 0x51000
#define SKL_DFSM_CDCLK_LIMIT_MASK (3u << 23)
#define SKL_DFSM_CDCLK_LIMIT_675 (0u << 23)
#define SKL_DFSM_CDCLK_LIMIT_540 (1u << 23)
#define SKL_DFSM_CDCLK_LIMIT_450 (2u << 23)
#define SKL_DFSM_CDCLK_LIMIT_337_5 (3u << 23)
#define SWF_ILK(i) (0x4F000 + (i) * 4)

/* Haswell / Broadwell: the LCPLL drives CDCLK; switching its frequency
 * moves CDCLK to FCLK for the duration. */
#define HSW_LCPLL_CTL 0x130040
#define HSW_LCPLL_PLL_DISABLE (1u << 31)
#define HSW_LCPLL_PLL_LOCK (1u << 30)
#define HSW_LCPLL_CLK_FREQ_MASK (3u << 26)
#define HSW_LCPLL_CLK_FREQ_450 (0u << 26)
#define HSW_LCPLL_CLK_FREQ_540_BDW (1u << 26)
#define HSW_LCPLL_CLK_FREQ_337_5_BDW (2u << 26)
#define HSW_LCPLL_CLK_FREQ_675_BDW (3u << 26)
#define HSW_LCPLL_CD_CLOCK_DISABLE (1u << 25)
#define HSW_LCPLL_ROOT_CD_CLOCK_DISABLE (1u << 24)
#define HSW_LCPLL_CD2X_CLOCK_DISABLE (1u << 23)
#define HSW_LCPLL_POWER_DOWN_ALLOW (1u << 22)
#define HSW_LCPLL_CD_SOURCE_FCLK (1u << 21)
#define HSW_LCPLL_CD_SOURCE_FCLK_DONE (1u << 19)
#define HSW_CDCLK_FREQ 0x46200
#define HSW_FUSE_STRAP 0x42014
#define HSW_CDCLK_LIMIT (1u << 24)

/* ---- the pcode mailbox -------------------------------------------------------- */

#define PCODE_MAILBOX 0x138124
#define PCODE_READY (1u << 31)
#define PCODE_ERROR_MASK 0xffu
#define PCODE_SUCCESS 0x0
#define PCODE_ILLEGAL_CMD 0x1
#define PCODE_GEN7_TIMEOUT 0x2
#define PCODE_GEN7_ILLEGAL_DATA 0x3
#define PCODE_GEN11_ILLEGAL_SUBCOMMAND 0x4
#define PCODE_GEN11_LOCKED 0x6
#define PCODE_GEN7_MIN_FREQ_OUT_OF_RANGE 0x10
#define PCODE_GEN11_REJECTED 0x11
#define PCODE_DATA 0x138128
#define PCODE_DATA1 0x13812C

#define BDW_PCODE_DISPLAY_FREQ_CHANGE_REQ 0x18
#define HSW_PCODE_DE_WRITE_FREQ_REQ 0x17
#define PCODE_SKL_CDCLK_CONTROL 0x7
#define PCODE_SKL_CDCLK_PREPARE_FOR_CHANGE 0x3
#define PCODE_SKL_CDCLK_READY_FOR_CHANGE 0x1
#define PCODE_GEN9_READ_MEM_LATENCY 0x6
#define GEN9_PCODE_SAGV_CONTROL 0x21
#define GEN9_SAGV_DISABLE 0x0
#define GEN9_SAGV_IS_DISABLED 0x1
#define GEN9_SAGV_ENABLE 0x3
#define GEN12_PCODE_READ_SAGV_BLOCK_TIME_US 0x23
#define ICL_PCODE_MEM_SUBSYSYSTEM_INFO 0xd
#define ICL_PCODE_MEM_SS_READ_GLOBAL_INFO (0x0u << 8)
#define ICL_PCODE_MEM_SS_READ_QGV_POINT_INFO(point) (((uint32_t)(point) << 16) | (0x1u << 8))
#define ADL_PCODE_MEM_SS_READ_PSF_GV_INFO (0x2u << 8)
#define ICL_PCODE_SAGV_DE_MEM_SS_CONFIG 0xe
#define ICL_PCODE_REP_QGV_MASK (3u << 0)
#define ICL_PCODE_REP_QGV_SAFE (0u << 0)
#define ADLS_PCODE_REP_PSF_MASK (3u << 2)
#define ADLS_PCODE_REP_PSF_SAFE (0u << 2)
#define ICL_PCODE_REQ_QGV_PT_MASK 0xffu
#define ICL_PCODE_REQ_QGV_PT(x) ((uint32_t)(x) & 0xff)
#define ADLS_PCODE_REQ_PSF_PT_MASK (7u << 8)
#define ADLS_PCODE_REQ_PSF_PT(x) (((uint32_t)(x) & 7) << 8)
/* DG2: the display's CDCLK, pipe count and voltage, told to the pcode. */
#define DISPLAY_TO_PCODE_CDCLK_VALID (1u << 27)
#define DISPLAY_TO_PCODE_PIPE_COUNT_VALID (1u << 31)
#define DISPLAY_TO_PCODE_CDCLK(x) (((uint32_t)(x) & 0x3ff) << 16)
#define DISPLAY_TO_PCODE_PIPE_COUNT(x) (((uint32_t)(x) & 7) << 28)
#define DISPLAY_TO_PCODE_VOLTAGE(x) ((uint32_t)(x) & 3)
#define DISPLAY_TO_PCODE_VOLTAGE_MAX 3
#define DISPLAY_TO_PCODE_CDCLK_MAX 0x28D

/* ---- the memory controller (mirrored into the MMIO window) ----------------------- */

#define MCHBAR_MIRROR_BASE_SNB 0x140000
#define MCH_SSKPD (MCHBAR_MIRROR_BASE_SNB + 0x5d10) /* 64 bits, two reads */
#define SKL_MAD_INTER_CHANNEL (MCHBAR_MIRROR_BASE_SNB + 0x5000)
#define SKL_DRAM_DDR_TYPE_MASK 0x3u
#define SKL_DRAM_DDR_TYPE_DDR4 0x0u
#define SKL_DRAM_DDR_TYPE_DDR3 0x1u
#define SKL_DRAM_DDR_TYPE_LPDDR3 0x2u
#define SKL_DRAM_DDR_TYPE_LPDDR4 0x3u
#define SKL_MAD_DIMM_CH0 (MCHBAR_MIRROR_BASE_SNB + 0x500C)
#define SKL_MAD_DIMM_CH1 (MCHBAR_MIRROR_BASE_SNB + 0x5010)
#define BXT_D_CR_DRP0_DUNIT_START 8
#define BXT_D_CR_DRP0_DUNIT_END 11
#define BXT_D_CR_DRP0_DUNIT(x) (MCHBAR_MIRROR_BASE_SNB + 0x1000 + ((x) - 8) * 0x200)
#define SA_PERF_STATUS_0_0_0_MCHBAR_PC (MCHBAR_MIRROR_BASE_SNB + 0x5918)
#define DG1_QCLK_RATIO(v) (((v) >> 2) & 0xff)
#define DG1_QCLK_REFERENCE (1u << 10)
#define SKL_MC_BIOS_DATA_0_0_0_MCHBAR_PCU (MCHBAR_MIRROR_BASE_SNB + 0x5e04)
#define DG1_GEAR_TYPE (1u << 16)
#define MCHBAR_CH0_CR_TC_PRE_0_0_0_MCHBAR (MCHBAR_MIRROR_BASE_SNB + 0x4000)
#define MCHBAR_CH0_CR_TC_PRE_0_0_0_MCHBAR_HIGH (MCHBAR_MIRROR_BASE_SNB + 0x4004)

/* What display version 14+ reads instead of asking the pcode. */
#define MEM_SS_INFO_GLOBAL 0x45700
#define MEM_SS_DDR_TYPE(v) ((v) & 0xf)
#define MEM_SS_POPULATED_CH(v) (((v) >> 4) & 0xf)
#define MEM_SS_ENABLED_QGV_POINTS(v) (((v) >> 8) & 0xf)
#define MEM_SS_INFO_QGV_POINT_LOW(point) (0x45710 + (point) * 8)
#define MEM_SS_INFO_QGV_POINT_HIGH(point) (0x45710 + (point) * 8 + 4)
#define MTL_TRCD(v) (((v) >> 24) & 0xff)
#define MTL_TRP(v) (((v) >> 16) & 0xff)
#define MTL_DCLK(v) ((v) & 0xffff)
#define MTL_TRAS(v) (((v) >> 8) & 0x1ff)
#define MTL_TRDPRE(v) ((v) & 0xff)
#define WM_LATENCY_LP0_LP1 0x45780
#define WM_LATENCY_LP2_LP3 0x45784
#define WM_LATENCY_LP4_LP5 0x45788
#define WM_LATENCY_EVEN(v) ((v) & 0x1fff)
#define WM_LATENCY_ODD(v) (((v) >> 16) & 0x1fff)
#define WM_LATENCY_SAGV 0x4578c
#define WM_LATENCY_QCLK_SAGV(v) ((v) & 0x1fff)

/* ---- pipes ------------------------------------------------------------------------- */

#ifndef PIPE_FRMCOUNT
#define PIPE_FRMCOUNT(pipe) (0x70040 + (pipe) * 0x1000)
#endif

#endif
