// LikeOS -- registers of the Synopsys port PHYs of DG2 (display version 13).
//
// DG2 (Arc A-series) has five PHYs, one per DDI it drives: DDI A, B and
// C, the repositioned DDI D, and the first Type-C DDI, which on this
// discrete part is a fixed DisplayPort/HDMI connector like the others.
// They are not the combo or Type-C PHYs of the integrated parts but a
// vendor design with its own PLL inside each one (the "MPLLB"), so there
// are no shared display PLLs to route a port to: a port's clock is its
// own PHY's PLL, programmed through the registers below and started from
// the port's PLL enable word.  The PHY block of PHY n sits 0x1000 * n
// above PHY A's; the transmitter equalisation words repeat per lane,
// 0x10 apart.
//
// The PLL is described entirely by seven words (charge pump, two
// divider words, the fractional feedback, the spread spectrum and its
// step) plus a reference-range field the firmware programs and the
// driver only checks.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2022 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_SNPS_PHY_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_SNPS_PHY_REGS_H

#include <kernel/uapi/types.h>

/* PHYs A..E.  DDI A, B, C use PHY A, B, C; the repositioned DDI D (port
 * index 7, where the register blocks of a "D" port live from display
 * version 13 on) uses PHY D; the first Type-C DDI (port index 3) uses
 * PHY E. */
#define SNPS_PHY_A 0
#define SNPS_PHY_B 1
#define SNPS_PHY_C 2
#define SNPS_PHY_D 3
#define SNPS_PHY_E 4
#define SNPS_NUM_PHYS 5

/* Port indices of DG2's DDIs in the driver's numbering. */
#define DG2_PORT_A 0
#define DG2_PORT_B 1
#define DG2_PORT_C 2
#define DG2_PORT_TC1 3
#define DG2_PORT_D_XELPD 7

/* ---- the PHY block ------------------------------------------------------------ */

#define _SNPS_PHY_A_BASE 0x168000
#define _SNPS_PHY_B_BASE 0x169000
#define _SNPS_PHY(phy) (_SNPS_PHY_A_BASE + (uint32_t)(phy) * (_SNPS_PHY_B_BASE - _SNPS_PHY_A_BASE))
#define _SNPS2(phy, reg) (_SNPS_PHY(phy) - _SNPS_PHY_A_BASE + (uint32_t)(reg))
#define _SNPS_LN(ln, phy, reg) _SNPS2(phy, (uint32_t)(reg) + (uint32_t)(ln) * 0x10)

/* MPLLB charge pump: integral and proportional gains and their gain
 * scalers. */
#define SNPS_PHY_MPLLB_CP(phy) _SNPS2(phy, 0x168000)
#define SNPS_PHY_MPLLB_CP_INT_MASK (0x7fu << 25)
#define SNPS_PHY_MPLLB_CP_INT(x) (((uint32_t)(x) << 25) & SNPS_PHY_MPLLB_CP_INT_MASK)
#define SNPS_PHY_MPLLB_CP_INT_GS_MASK (0x7fu << 17)
#define SNPS_PHY_MPLLB_CP_INT_GS(x) (((uint32_t)(x) << 17) & SNPS_PHY_MPLLB_CP_INT_GS_MASK)
#define SNPS_PHY_MPLLB_CP_PROP_MASK (0x7fu << 9)
#define SNPS_PHY_MPLLB_CP_PROP(x) (((uint32_t)(x) << 9) & SNPS_PHY_MPLLB_CP_PROP_MASK)
#define SNPS_PHY_MPLLB_CP_PROP_GS_MASK (0x7fu << 1)
#define SNPS_PHY_MPLLB_CP_PROP_GS(x) (((uint32_t)(x) << 1) & SNPS_PHY_MPLLB_CP_PROP_GS_MASK)

/* MPLLB dividers: the output dividers and modes, and the force bit that
 * keeps the PLL running once it has sampled the rest. */
#define SNPS_PHY_MPLLB_DIV(phy) _SNPS2(phy, 0x168004)
#define SNPS_PHY_MPLLB_FORCE_EN (1u << 31)
#define SNPS_PHY_MPLLB_DIV_CLK_EN (1u << 30)
#define SNPS_PHY_MPLLB_DIV5_CLK_EN (1u << 29)
#define SNPS_PHY_MPLLB_V2I_MASK (3u << 26)
#define SNPS_PHY_MPLLB_V2I(x) (((uint32_t)(x) << 26) & SNPS_PHY_MPLLB_V2I_MASK)
#define SNPS_PHY_MPLLB_FREQ_VCO_MASK (3u << 24)
#define SNPS_PHY_MPLLB_FREQ_VCO(x) (((uint32_t)(x) << 24) & SNPS_PHY_MPLLB_FREQ_VCO_MASK)
#define SNPS_PHY_MPLLB_DIV_MULTIPLIER_MASK (0xffu << 16)
#define SNPS_PHY_MPLLB_DIV_MULTIPLIER(x)                                       \
	(((uint32_t)(x) << 16) & SNPS_PHY_MPLLB_DIV_MULTIPLIER_MASK)
#define SNPS_PHY_MPLLB_PMIX_EN (1u << 10)
#define SNPS_PHY_MPLLB_DP2_MODE (1u << 9)
#define SNPS_PHY_MPLLB_WORD_DIV2_EN (1u << 8)
#define SNPS_PHY_MPLLB_TX_CLK_DIV_MASK (7u << 5)
#define SNPS_PHY_MPLLB_TX_CLK_DIV_SHIFT 5
#define SNPS_PHY_MPLLB_TX_CLK_DIV(x) (((uint32_t)(x) << 5) & SNPS_PHY_MPLLB_TX_CLK_DIV_MASK)
#define SNPS_PHY_MPLLB_SHIM_DIV32_CLK_SEL (1u << 0)

/* The fractional part of the feedback divider: enable, denominator... */
#define SNPS_PHY_MPLLB_FRACN1(phy) _SNPS2(phy, 0x168008)
#define SNPS_PHY_MPLLB_FRACN_EN (1u << 31)
#define SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN (1u << 30)
#define SNPS_PHY_MPLLB_FRACN_DEN_MASK 0xffffu
#define SNPS_PHY_MPLLB_FRACN_DEN(x) ((uint32_t)(x) & SNPS_PHY_MPLLB_FRACN_DEN_MASK)

/* ...and the quotient and remainder over it. */
#define SNPS_PHY_MPLLB_FRACN2(phy) _SNPS2(phy, 0x16800C)
#define SNPS_PHY_MPLLB_FRACN_REM_MASK (0xffffu << 16)
#define SNPS_PHY_MPLLB_FRACN_REM_SHIFT 16
#define SNPS_PHY_MPLLB_FRACN_REM(x) (((uint32_t)(x) << 16) & SNPS_PHY_MPLLB_FRACN_REM_MASK)
#define SNPS_PHY_MPLLB_FRACN_QUOT_MASK 0xffffu
#define SNPS_PHY_MPLLB_FRACN_QUOT(x) ((uint32_t)(x) & SNPS_PHY_MPLLB_FRACN_QUOT_MASK)

/* Spread spectrum: on, the direction, the peak deviation and the step. */
#define SNPS_PHY_MPLLB_SSCEN(phy) _SNPS2(phy, 0x168014)
#define SNPS_PHY_MPLLB_SSC_EN (1u << 31)
#define SNPS_PHY_MPLLB_SSC_UP_SPREAD (1u << 30)
#define SNPS_PHY_MPLLB_SSC_PEAK_MASK (0xfffffu << 10)
#define SNPS_PHY_MPLLB_SSC_PEAK(x) (((uint32_t)(x) << 10) & SNPS_PHY_MPLLB_SSC_PEAK_MASK)

#define SNPS_PHY_MPLLB_SSCSTEP(phy) _SNPS2(phy, 0x168018)
#define SNPS_PHY_MPLLB_SSC_STEPSIZE_MASK (0x1fffffu << 11)
#define SNPS_PHY_MPLLB_SSC_STEPSIZE(x)                                         \
	(((uint32_t)(x) << 11) & SNPS_PHY_MPLLB_SSC_STEPSIZE_MASK)

/* The integer feedback multiplier, the input clock's pre-divider, and the
 * HDMI dividers. */
#define SNPS_PHY_MPLLB_DIV2(phy) _SNPS2(phy, 0x16801C)
#define SNPS_PHY_MPLLB_HDMI_PIXEL_CLK_DIV_MASK (3u << 18)
#define SNPS_PHY_MPLLB_HDMI_PIXEL_CLK_DIV(x)                                   \
	(((uint32_t)(x) << 18) & SNPS_PHY_MPLLB_HDMI_PIXEL_CLK_DIV_MASK)
#define SNPS_PHY_MPLLB_HDMI_DIV_MASK (7u << 15)
#define SNPS_PHY_MPLLB_HDMI_DIV(x) (((uint32_t)(x) << 15) & SNPS_PHY_MPLLB_HDMI_DIV_MASK)
#define SNPS_PHY_MPLLB_REF_CLK_DIV_MASK (7u << 12)
#define SNPS_PHY_MPLLB_REF_CLK_DIV_SHIFT 12
#define SNPS_PHY_MPLLB_REF_CLK_DIV(x) (((uint32_t)(x) << 12) & SNPS_PHY_MPLLB_REF_CLK_DIV_MASK)
#define SNPS_PHY_MPLLB_MULTIPLIER_MASK 0xfffu
#define SNPS_PHY_MPLLB_MULTIPLIER(x) ((uint32_t)(x) & SNPS_PHY_MPLLB_MULTIPLIER_MASK)

/* The input clock's range: programmed by the firmware, read back by
 * the driver only to check it against what the tables expect. */
#define SNPS_PHY_REF_CONTROL(phy) _SNPS2(phy, 0x168188)
#define SNPS_PHY_REF_CONTROL_REF_RANGE_MASK (0x1fu << 27)
#define SNPS_PHY_REF_CONTROL_REF_RANGE(x)                                      \
	(((uint32_t)(x) << 27) & SNPS_PHY_REF_CONTROL_REF_RANGE_MASK)

/* The power state the lanes drop to while the panel self-refreshes. */
#define SNPS_PHY_TX_REQ(phy) _SNPS2(phy, 0x168200)
#define SNPS_PHY_TX_REQ_LN_DIS_PWR_STATE_PSR_MASK (3u << 30)
#define SNPS_PHY_TX_REQ_LN_DIS_PWR_STATE_PSR(x)                                \
	(((uint32_t)(x) << 30) & SNPS_PHY_TX_REQ_LN_DIS_PWR_STATE_PSR_MASK)

/* Transmitter equalisation, per lane: main cursor (the swing), the
 * post-cursor (de-emphasis) and the pre-cursor (pre-shoot). */
#define SNPS_PHY_TX_EQ(ln, phy) _SNPS_LN(ln, phy, 0x168300)
#define SNPS_PHY_TX_EQ_MAIN_MASK (0x3fu << 18)
#define SNPS_PHY_TX_EQ_MAIN(x) (((uint32_t)(x) << 18) & SNPS_PHY_TX_EQ_MAIN_MASK)
#define SNPS_PHY_TX_EQ_POST_MASK (0x3fu << 10)
#define SNPS_PHY_TX_EQ_POST(x) (((uint32_t)(x) << 10) & SNPS_PHY_TX_EQ_POST_MASK)
#define SNPS_PHY_TX_EQ_PRE_MASK (0x3fu << 2)
#define SNPS_PHY_TX_EQ_PRE(x) (((uint32_t)(x) << 2) & SNPS_PHY_TX_EQ_PRE_MASK)

/* ---- the port PLL enable words and the PHY's misc control ---------------------- */

/* The PLL enable of PHY A..D: DPLL0's and DPLL1's addresses for A and B,
 * the next two for C and D.  PHY E's is the first Type-C PLL's word. */
#define _DG2_DPLL0_ENABLE 0x46010
#define _DG2_PLL3_ENABLE 0x4601C
#define _DG2_TC_PLL_ENABLE 0x46030
#define DG2_PLL_ENABLE(phy)                                                    \
	((phy) >= SNPS_PHY_E ? (uint32_t)_DG2_TC_PLL_ENABLE :                  \
	 (phy) >= SNPS_PHY_D ? (uint32_t)_DG2_PLL3_ENABLE :                    \
			       (uint32_t)(_DG2_DPLL0_ENABLE + (phy) * 4))
#define DG2_PLL_ENABLE_BIT (1u << 31)
#define DG2_PLL_LOCK (1u << 30)
#define DG2_PLL_POWER_ENABLE (1u << 27)
#define DG2_PLL_POWER_STATE (1u << 26)

/* PHY misc control: PHY E's sits where a PHY F's would. */
#define _DG2_PHY_MISC_A 0x64C00
#define _DG2_PHY_MISC_TC1 0x64C14
#define DG2_PHY_MISC(phy)                                                      \
	((phy) == SNPS_PHY_E ? (uint32_t)_DG2_PHY_MISC_TC1 :                   \
			       (uint32_t)(_DG2_PHY_MISC_A + (phy) * 4))
#define DG2_PHY_DE_IO_COMP_PWR_DOWN (1u << 23)
/* While a transmitter's calibration and adaptation are running its
 * acknowledge bit is set; all clear means the PHY is ready. */
#define DG2_PHY_DP_TX_ACK_MASK (0xfu << 20)

#endif
