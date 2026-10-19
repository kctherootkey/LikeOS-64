// LikeOS -- registers of the display PLLs and port PHYs of display
// versions 9 to 13: Skylake's DPLLs, Broxton/Gemini Lake's DPIO PHYs and
// their port PLLs, the combo PHYs and combo PLLs of Ice Lake and later,
// the port clock routing (DPCLKA) of each Gen11/12 platform, and the
// MG (Ice Lake) and Dekel (Tiger Lake, Alder Lake-P) Type-C PHYs.
//
// Names i915_reg.h already has with the same meaning are guarded; where
// a field or address differs from what the hardware has (the BXT PHY
// group registers, the MG clock-top fields, a few Dekel fields), this
// header uses a name of its own so the two cannot be mixed up.
//
// Also declares the PLL entry points intel_display.h does not (yet)
// carry.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_DPLL_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_DPLL_REGS_H

/* ---- Skylake ------------------------------------------------------------------ */

#define SKL_DPLL_CFGCR1_DCO_FRACTION_SHIFT 9
#define SKL_DPLL_CFGCR2_PDIV_7_INVALID (5u << 2)

/* ---- Haswell / Broadwell -------------------------------------------------------- */

#define HSW_WRPLL_DIVIDER_REF_MASK 0xffu
#define HSW_WRPLL_DIVIDER_POST_MASK (0x3fu << 8)
#define HSW_WRPLL_DIVIDER_POST_SHIFT 8
#define HSW_WRPLL_DIVIDER_FB_MASK (0xffu << 16)
#define HSW_WRPLL_DIVIDER_FB_SHIFT 16
#define HSW_WRPLL_REF_MASK (3u << 28)

/* ---- Broxton / Gemini Lake: the DPIO PHYs ---------------------------------------- */
/*
 * Broxton: PHY0 (0x6C000) carries port B on its channel 0 and port C on
 * channel 1, PHY1 (0x162000) port A.  Gemini Lake: PHY0 port B, PHY1
 * port A, PHY2 (0x163000) port C, each single channel.  The registers of
 * a channel sit at per-register offsets from the PHY's base (channel 1's
 * are not at one fixed distance from channel 0's), and the lane
 * registers of a channel at 0x80 per lane within a pair and 0x200 per
 * pair.
 */
#define BXT_DPIO_PHY_BASE(phy) ((phy) == 0 ? 0x6C000u : (phy) == 1 ? 0x162000u : 0x163000u)
#define BXT_DPIO_PHY_REG(phy, reg) (BXT_DPIO_PHY_BASE(phy) + ((reg) - 0x6C000u))
#define BXT_DPIO_CH_REG(phy, ch, reg_ch0, reg_ch1) \
	(BXT_DPIO_PHY_BASE(phy) + ((ch) ? (reg_ch1) : (reg_ch0)) - 0x6C000u)
#define BXT_DPIO_LANE_OFFSET(lane) ((((lane) >> 1) * 0x200u) + (((lane) & 1) * 0x80u))

/* the PHY family control: common reset (the PHY's enable) */
#define BXT_DPIO_PHY_CTL_FAMILY(phy) ((phy) == 0 ? 0x64C90u : (phy) == 1 ? 0x64C80u : 0x64CA0u)
#define BXT_DPIO_COMMON_RESET_DIS (1u << 31)
/* the Punit's power request for the PHYs */
#define BXT_DPIO_GT_DISP_PWRON 0x138090
/* per port: lane power-down acknowledgements */
#define BXT_DPIO_PHY_CTL(port) (0x64C00u + (port) * 0x10u)

#define BXT_DPIO_CL1CM_DW0(phy) BXT_DPIO_PHY_REG(phy, 0x6C000u)
#define BXT_DPIO_PHY_POWER_GOOD (1u << 16)
#define BXT_DPIO_PHY_RESERVED (1u << 7)
#define BXT_DPIO_CL1CM_DW9(phy) BXT_DPIO_PHY_REG(phy, 0x6C024u)
#define BXT_DPIO_IREF0RC_OFFSET_MASK (0xffu << 8)
#define BXT_DPIO_IREF0RC_OFFSET(x) ((uint32_t)(x) << 8)
#define BXT_DPIO_CL1CM_DW10(phy) BXT_DPIO_PHY_REG(phy, 0x6C028u)
#define BXT_DPIO_IREF1RC_OFFSET_MASK (0xffu << 8)
#define BXT_DPIO_IREF1RC_OFFSET(x) ((uint32_t)(x) << 8)
#define BXT_DPIO_CL1CM_DW28(phy) BXT_DPIO_PHY_REG(phy, 0x6C070u)
#define BXT_DPIO_OCL1_POWER_DOWN_EN (1u << 23)
#define BXT_DPIO_DW28_OLDO_DYN_PWR_DOWN_EN (1u << 22)
#define BXT_DPIO_SUS_CLK_CONFIG 0x3u
#define BXT_DPIO_CL2CM_DW6(phy) BXT_DPIO_PHY_REG(phy, 0x6C358u)
#define BXT_DPIO_DW6_OLDO_DYN_PWR_DOWN_EN (1u << 28)
#define BXT_DPIO_REF_DW3(phy) BXT_DPIO_PHY_REG(phy, 0x6C18Cu)
#define BXT_DPIO_GRC_DONE (1u << 22)
#define BXT_DPIO_REF_DW6(phy) BXT_DPIO_PHY_REG(phy, 0x6C198u)
#define BXT_DPIO_GRC_CODE_SHIFT 24
#define BXT_DPIO_GRC_CODE_MASK (0xffu << 24)
#define BXT_DPIO_GRC_CODE_FAST(x) ((uint32_t)(x) << 16)
#define BXT_DPIO_GRC_CODE_SLOW(x) ((uint32_t)(x) << 8)
#define BXT_DPIO_GRC_CODE_NOM(x) ((uint32_t)(x) << 0)
#define BXT_DPIO_REF_DW8(phy) BXT_DPIO_PHY_REG(phy, 0x6C1A0u)
#define BXT_DPIO_GRC_DIS (1u << 15)
#define BXT_DPIO_GRC_RDY_OVRD (1u << 1)

/* the port PLL */
#define BXT_DPIO_PORT_PLL_ENABLE(port) (0x46074u + (port) * 4)
#define BXT_DPIO_PLL_EBB_0(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6C034u, 0x6C340u)
#define BXT_DPIO_PLL_EBB_4(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6C038u, 0x6C344u)
#define BXT_DPIO_PLL(phy, ch, idx) (BXT_DPIO_CH_REG(phy, ch, 0x6C100u, 0x6C380u) + (idx) * 4)
#define BXT_DPIO_PLL_LOCK_THRESHOLD_MASK (7u << 1)
#define BXT_DPIO_PLL_LOCK_THRESHOLD(x) ((uint32_t)(x) << 1)
#define BXT_DPIO_PLL_M2_FRAC(x) ((uint32_t)(x) & 0x3fffffu)
#define BXT_DPIO_PLL_TARGET_CNT(x) ((uint32_t)(x) & 0x3ffu)

/* the lanes: PCS and TX words, per lane pair / lane and as a group */
#define BXT_DPIO_PCS_DW10_LN01(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6C428u, 0x6C828u)
#define BXT_DPIO_PCS_DW10_GRP(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6CC28u, 0x6CE28u)
#define BXT_DPIO_TX2_SWING_CALC_INIT (1u << 31)
#define BXT_DPIO_TX1_SWING_CALC_INIT (1u << 30)
#define BXT_DPIO_PCS_DW12_LN01(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6C430u, 0x6C830u)
#define BXT_DPIO_PCS_DW12_LN23(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6C630u, 0x6CA30u)
#define BXT_DPIO_PCS_DW12_GRP(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6CC30u, 0x6CE30u)
#define BXT_DPIO_TX_DW2_LN(phy, ch, lane) \
	(BXT_DPIO_CH_REG(phy, ch, 0x6C508u, 0x6C908u) + BXT_DPIO_LANE_OFFSET(lane))
#define BXT_DPIO_MARGIN_000_MASK (0xffu << 16)
#define BXT_DPIO_UNIQ_TRANS_SCALE_MASK (0xffu << 8)
#define BXT_DPIO_TX_DW3_LN(phy, ch, lane) \
	(BXT_DPIO_CH_REG(phy, ch, 0x6C50Cu, 0x6C90Cu) + BXT_DPIO_LANE_OFFSET(lane))
#define BXT_DPIO_TX_DW4_LN(phy, ch, lane) \
	(BXT_DPIO_CH_REG(phy, ch, 0x6C510u, 0x6C910u) + BXT_DPIO_LANE_OFFSET(lane))
#define BXT_DPIO_DE_EMPHASIS_MASK (0xffu << 24)
#define BXT_DPIO_TX_DW5_LN(phy, ch, lane) \
	(BXT_DPIO_CH_REG(phy, ch, 0x6C514u, 0x6C914u) + BXT_DPIO_LANE_OFFSET(lane))
#define BXT_DPIO_TX_DW5_GRP(phy, ch) BXT_DPIO_CH_REG(phy, ch, 0x6CD14u, 0x6CF14u)
#define BXT_DPIO_DCC_DELAY_RANGE_2 (1u << 8)
#define BXT_DPIO_TX_DW14_LN(phy, ch, lane) \
	(BXT_DPIO_CH_REG(phy, ch, 0x6C538u, 0x6C938u) + BXT_DPIO_LANE_OFFSET(lane))
#define BXT_DPIO_LATENCY_OPTIM (1u << 30)

/* ---- Ice Lake and later: the combo PHYs ------------------------------------------ */

#define ICL_COMBO_PROCESS_INFO_MASK (7u << 26)
#define ICL_COMBO_PROCESS_INFO_DOT_0 (0u << 26)
#define ICL_COMBO_PROCESS_INFO_DOT_1 (1u << 26)
#define ICL_COMBO_VOLTAGE_INFO_MASK (3u << 24)
#define ICL_COMBO_VOLTAGE_INFO_0_85V (0u << 24)
#define ICL_COMBO_VOLTAGE_INFO_0_95V (1u << 24)
#define ICL_COMBO_VOLTAGE_INFO_1_05V (2u << 24)
/* PORT_CL_DW10 */
#define ICL_COMBO_PWR_DOWN_LN_MASK (0xfu << 4)
#define ICL_COMBO_PWR_UP_ALL_LANES (0x0u << 4)
#define ICL_COMBO_PWR_DOWN_LN_3_2_1 (0xeu << 4)
#define ICL_COMBO_PWR_DOWN_LN_3_2 (0xcu << 4)
#define ICL_COMBO_PWR_DOWN_LN_2_1_0 (0x7u << 4)
#define ICL_COMBO_PWR_DOWN_LN_1_0 (0x3u << 4)
#define ICL_COMBO_EDP4K2K_MODE_OVRD_EN (1u << 3)
#define ICL_COMBO_EDP4K2K_MODE_OVRD_OPTIMIZED (1u << 2)
/* PORT_PCS_DW1 */
#define ICL_COMBO_DCC_MODE_SELECT_MASK (3u << 20)
#define ICL_COMBO_RUN_DCC_ONCE (0u << 20)
/* PORT_TX_DW5 */
#define ICL_COMBO_CURSOR_PROGRAM (1u << 26)
#define ICL_COMBO_COEFF_POLARITY (1u << 25)
/* PORT_TX_DW8 */
#define ICL_COMBO_ODCC_CLK_SEL (1u << 31)
#define ICL_COMBO_ODCC_CLK_DIV_SEL_MASK (3u << 29)
#define ICL_COMBO_ODCC_CLK_DIV_SEL_DIV2 (1u << 29)
/* PHY_MISC: the PHY's compensation power, and Elkhart Lake's DDI-D mux
 * on PHY A */
#define ICL_COMBO_PHY_MISC(phy) (0x64C00u + (phy) * 4)
#define ICL_COMBO_PHY_MISC_MUX_DDID (1u << 28)
#define ICL_COMBO_PHY_MISC_DE_IO_COMP_PWR_DOWN (1u << 23)

/* ---- Ice Lake and later: the combo PLLs and the port clock routing ------------- */

/* The PLL enables: DPLL0, DPLL1, then by platform the third and fourth
 * (Alder Lake-S DPLL2 at 0x46018, DPLL3 at 0x46030; DG1 DPLL2/3 at
 * 0x46030/0x46034; Rocket Lake's DPLL4 at 0x46018; Elkhart Lake's DPLL4
 * at the first MG PLL's enable, 0x46030). */
#define GEN11_DPLL0_ENABLE 0x46010u
#define GEN11_DPLL1_ENABLE 0x46014u
#define ADLS_DPLL2_ENABLE 0x46018u
#define ADLS_DPLL3_ENABLE 0x46030u
#define DG1_DPLL2_ENABLE 0x46030u
#define DG1_DPLL3_ENABLE 0x46034u
#define EHL_DPLL4_ENABLE 0x46030u
#define GEN11_TBT_PLL_ENABLE 0x46020u
#define ICL_MG_PLL_ENABLE(tc) (0x46030u + (tc) * 4)
#define ADLP_PORTTC_PLL_ENABLE(tc) (0x46038u + (tc) * 8)

/* the combo PLLs' configuration words, per platform */
#define ICL_DPLL_CFGCR0_N(pll) (0x164000u + (pll) * 0x80) /* DPLL0, 1, 4 (EHL); TBT = 2 */
#define ICL_DPLL_CFGCR1_N(pll) (0x164004u + (pll) * 0x80)
#define TGL_DPLL0_CFGCR0 0x164284u
#define TGL_DPLL1_CFGCR0 0x16428Cu
#define TGL_TBTPLL_CFGCR0_REG 0x16429Cu
#define TGL_DPLL0_CFGCR1 0x164288u
#define TGL_DPLL1_CFGCR1 0x164290u
#define TGL_TBTPLL_CFGCR1_REG 0x1642A0u
#define RKL_DPLL4_CFGCR0 0x164294u /* also Alder Lake-S DPLL2 */
#define RKL_DPLL4_CFGCR1 0x164298u
#define ADLS_DPLL3_CFGCR0 0x1642C0u
#define ADLS_DPLL3_CFGCR1 0x1642C4u
#define DG1_DPLL2_CFGCR0 0x16C284u
#define DG1_DPLL3_CFGCR0 0x16C28Cu
#define DG1_DPLL2_CFGCR1 0x16C288u
#define DG1_DPLL3_CFGCR1 0x16C290u
#define GEN11_DPLL_CFGCR0_SSC_ENABLE (1u << 25)
#define GEN11_DPLL_CFGCR0_DCO_FRACTION_SHIFT 10
#define GEN11_DPLL_CFGCR1_QDIV_RATIO_SHIFT 10
#define GEN11_DPLL_CFGCR1_QDIV_MODE (1u << 9)
#define GEN11_DPLL_CFGCR1_KDIV_SHIFT 6
#define GEN11_DPLL_CFGCR1_PDIV_SHIFT 2

/* DPCLKA_CFGCR0: which PLL clocks a combo PHY, and the per-PHY / per
 * Type-C port clock gates.  The gate bits are not in PHY order on Ice
 * Lake (A 10, B 11, C 24); Rocket Lake has them at 10 + PHY. */
#define GEN11_DPCLKA_CFGCR0 0x164280u
#define ICL_DPCLKA_DDI_CLK_OFF(phy) \
	(1u << ((phy) == 0 ? 10 : (phy) == 1 ? 11 : (phy) == 2 ? 24 : (phy) == 3 ? 4 : 5))
#define RKL_DPCLKA_DDI_CLK_OFF(phy) (1u << ((phy) + 10))
#define ICL_DPCLKA_TC_CLK_OFF(tc) (1u << ((tc) < 3 ? (tc) + 12 : (tc) - 3 + 21))
#define ICL_DPCLKA_DDI_CLK_SEL_SHIFT(phy) ((phy) * 2)
#define RKL_DPCLKA_DDI_CLK_SEL_SHIFT(phy) \
	((phy) == 0 ? 0 : (phy) == 1 ? 2 : (phy) == 2 ? 4 : 27)
/* DG1: one register for PHYs A/B (DPLL0/1), one for C/D (DPLL2/3), each
 * with one select and one gate per PHY */
#define DG1_DPCLKA_CFGCR0(phy) ((phy) < 2 ? 0x164280u : 0x16C280u)
#define DG1_DPCLKA_DDI_CLK_OFF(phy) (1u << (((phy) % 2) + 10))
#define DG1_DPCLKA_DDI_CLK_SEL_SHIFT(phy) (((phy) % 2) * 2)
/* Alder Lake-S: PHYs A, B, C in the first register, D, E in a second;
 * the gates stay in the first at Ice Lake's positions */
#define ADLS_DPCLKA_CFGCR(phy) ((phy) < 3 ? 0x164280u : 0x1642BCu)
#define ADLS_DPCLKA_DDI_SHIFT(phy) (((phy) % 3) * 2)

/* DDI_BUF_CTL fields of the Type-C ports of display versions 11-13:
 * the PHY link rate (Alder Lake-P), the lane enable stagger in link
 * symbols */
#define GEN11_DDI_BUF_PHY_LINK_RATE_MASK (0xfu << 20)
#define GEN11_DDI_BUF_PHY_LINK_RATE(r) ((uint32_t)(r) << 20)
#define GEN11_DDI_BUF_LANE_STAGGER_DELAY_MASK (0xffu << 8)
#define GEN11_DDI_BUF_LANE_STAGGER_DELAY(s) (((uint32_t)(s) & 0xffu) << 8)
#define GEN11_DDI_BUF_CTL_TC_PHY_OWNERSHIP (1u << 6)

/* the Type-C port clock select, per port */
#define GEN11_DDI_CLK_SEL(port) (0x46100u + (port) * 4)

/* ---- Ice Lake: the MG PHY ----------------------------------------------------------- */

#define ICL_MG_BASE(tc) (0x168000u + (tc) * 0x1000u)
#define ICL_MG_LN(tc, ln) (ICL_MG_BASE(tc) + (ln) * 0x400u)
#define ICL_MG_CLKHUB(tc, ln) (ICL_MG_LN(tc, ln) + 0x39Cu)
#define ICL_MG_CFG_LOW_RATE_LKREN_EN (1u << 11)
/* the clock top's high-speed clock control (MG and Dekel alike) */
#define MGDKL_HSCLKCTL_CORE_INPUTSEL(x) ((uint32_t)(x) << 16)
#define MGDKL_HSCLKCTL_CORE_INPUTSEL_MASK (1u << 16)
#define MGDKL_HSCLKCTL_TLINEDRV_CLKSEL(x) ((uint32_t)(x) << 14)
#define MGDKL_HSCLKCTL_TLINEDRV_CLKSEL_MASK (3u << 14)
#define MGDKL_HSCLKCTL_HSDIV_RATIO_MASK (3u << 12)
#define MGDKL_HSCLKCTL_HSDIV_RATIO_2 (0u << 12)
#define MGDKL_HSCLKCTL_HSDIV_RATIO_3 (1u << 12)
#define MGDKL_HSCLKCTL_HSDIV_RATIO_5 (2u << 12)
#define MGDKL_HSCLKCTL_HSDIV_RATIO_7 (3u << 12)
#define MGDKL_HSCLKCTL_DSDIV_RATIO(x) ((uint32_t)(x) << 8)
#define MGDKL_HSCLKCTL_DSDIV_RATIO_SHIFT 8
#define MGDKL_HSCLKCTL_DSDIV_RATIO_MASK (0xfu << 8)
#define MGDKL_REFCLKIN_CTL_OD_2_MUX(x) ((uint32_t)(x) << 8)
#define MGDKL_REFCLKIN_CTL_OD_2_MUX_MASK (7u << 8)
#define MGDKL_CORECLKCTL1_A_DIVRATIO(x) ((uint32_t)(x) << 8)
#define MGDKL_CORECLKCTL1_A_DIVRATIO_MASK (0xffu << 8)
#define ICL_MG_PLL_DIV0_FBDIV_FRAC_MASK (0x3fffffu << 8)
#define ICL_MG_PLL_DIV0_FBDIV_INT_MASK 0xffu
#define ICL_MG_PLL_DIV1_FBPREDIV_MASK 0xfu
#define ICL_MG_PLL_FRAC_LOCK_FEEDFWRDCAL_EN (1u << 8)

/* ---- Tiger Lake / Alder Lake-P: the Dekel PHY ------------------------------------- */
/*
 * A Dekel PHY has more than the 4 KB its MMIO window shows: the window
 * is at 0x168000 + 0x1000 per Type-C port, and the top bits of a PHY
 * address (its "bank", the bits above 12) go into HIP_INDEX first --
 * one byte per port, ports 0-3 in the first index register and 4-5 in
 * the second.  The addresses below are the PHY's own: bank in bits
 * 15:12, window offset in bits 11:0.
 */
#define DKL_PHY_BASE(tc) (0x168000u + (tc) * 0x1000u)
#define DKL_HIP_INDEX_REG(tc) ((tc) < 4 ? 0x1010A0u : 0x1010A4u)
#define DKL_HIP_INDEX_SHIFT(tc) (8 * ((tc) % 4))
#define DKL_PHY_BANK(off) (((off) >> 12) & 0xfu)
#define DKL_PHY_WINDOW(off) ((off) & 0xfffu)
#define DKL_PHY_LN(ln0, ln1, ln) ((ln) ? (ln1) : (ln0))

#define DKL_PCS_DW5_LN(ln) DKL_PHY_LN(0x0014u, 0x1014u, ln)
#define DKL_PLL_DIV0_OFF 0x2200u
#define DKL_PLL_DIV1_OFF 0x2204u
#define DKL_PLL_SSC_OFF 0x2210u
#define DKL_PLL_BIAS_OFF 0x2214u
#define DKL_PLL_TDC_COLDST_BIAS_OFF 0x2218u
#define DKL_REFCLKIN_CTL_OFF 0x212Cu
#define DKL_CLKTOP2_HSCLKCTL_OFF 0x20D4u
#define DKL_CLKTOP2_CORECLKCTL1_OFF 0x20D8u
#define DKL_TX_DPCNTL0_LN(ln) DKL_PHY_LN(0x02C0u, 0x12C0u, ln)
#define DKL_TX_DPCNTL1_LN(ln) DKL_PHY_LN(0x02C4u, 0x12C4u, ln)
#define DKL_TX_DPCNTL2_LN(ln) DKL_PHY_LN(0x02C8u, 0x12C8u, ln)
#define DKL_TX_PMD_LANE_SUS_LN(ln) DKL_PHY_LN(0x0D00u, 0x1D00u, ln)
#define DKL_DP_MODE_LN(ln) DKL_PHY_LN(0x00A0u, 0x10A0u, ln)
#define DKL_CMN_UC_DW27_OFF 0x236Cu

#define DKL_PLL_DIV0_AFC_STARTUP_MASK (7u << 25)
#define DKL_PLL_DIV0_INTEG_COEFF_MASK (0x1fu << 16)
#define DKL_PLL_DIV0_PROP_COEFF_MASK (0xfu << 12)
#define DKL_PLL_DIV0_FBPREDIV_MASK (0xfu << 8)
#define DKL_PLL_DIV0_FBDIV_INT_MASK 0xffu
#define DKL_PLL_DIV0_FIELDS (DKL_PLL_DIV0_INTEG_COEFF_MASK | DKL_PLL_DIV0_PROP_COEFF_MASK | \
			     DKL_PLL_DIV0_FBPREDIV_MASK | DKL_PLL_DIV0_FBDIV_INT_MASK)
#define DKL_PLL_DIV1_IREF_TRIM_FIELD (0x1fu << 16)
#define DKL_PLL_DIV1_TDC_TARGET_CNT_FIELD 0xffu
#define DKL_PLL_BIAS_FBDIV_SHIFT 8
#define DKL_TX_DEEMPH_COEFF(x) ((uint32_t)(x) << 8)
#define DKL_TX_DEEMPH_COEFF_MASK (0x1fu << 8)
#define DKL_TX_PRESHOOT_FIELD_MASK (0x1fu << 13)
#define DKL_TX_VSWING_FIELD_MASK 0x7u
#define DKL_TX_DPCNTL2_LOADGENSELECT_TX1_MASK (3u << 3)
#define DKL_TX_DPCNTL2_LOADGENSELECT_TX1(x) ((uint32_t)(x) << 3)
#define DKL_TX_DPCNTL2_LOADGENSELECT_TX2_MASK (3u << 5)
#define DKL_TX_DPCNTL2_LOADGENSELECT_TX2(x) ((uint32_t)(x) << 5)
#define DKL_TX_LOADGEN_SHARING_PMD_DISABLE_BIT (1u << 12)

/* ---- entry points beyond intel_display.h ----------------------------------------- */

struct i915_device;
struct intel_output;

/* Can the PLL behind this output be given the DisplayPort link rate,
 * and does the platform allow it on this kind of port?  (The per-port
 * form of intel_dpll_rate_supported(): Type-C PLLs have no eDP
 * intermediate rates, Ice/Tiger Lake combo ports take 8.1 GHz only for
 * eDP, Elkhart Lake's eDP stops at 5.4 GHz, Haswell ULX at 2.7 GHz.) */
int intel_dpll_output_rate_supported(struct i915_device *i915, const struct intel_output *o,
				     uint32_t link_rate_khz);
/* Can the port's PLL and PHY carry this TMDS clock (kHz)?  0 when they
 * can, -ERANGE when not: the platform's maximum (before any sink or
 * scrambling limit) and the gaps the PLLs cannot synthesise. */
int intel_dpll_hdmi_clock_valid(struct i915_device *i915, const struct intel_output *o,
				uint32_t clock_khz);
/* What a Type-C port's DDI_BUF_CTL needs besides width, reversal and
 * level when a DisplayPort link is enabled (display versions 11-13):
 * the bits to set, and in *mask the fields they replace. */
uint32_t intel_dpll_ddi_buf_ctl_bits(struct i915_device *i915, const struct intel_output *o,
				     uint32_t *mask);
/* The TMDS clock ceiling of the source on this platform, kHz. */
uint32_t intel_dpll_hdmi_max_tmds_khz(struct i915_device *i915);

/* The Dekel PHY's banked registers, under the one lock that keeps the
 * bank index and the access together (intel_tc.c); `off' is a PHY
 * address as above. */
uint32_t intel_dkl_phy_read(struct i915_device *i915, int tc, uint32_t off);
void intel_dkl_phy_write(struct i915_device *i915, int tc, uint32_t off, uint32_t val);
void intel_dkl_phy_rmw(struct i915_device *i915, int tc, uint32_t off, uint32_t clear,
		       uint32_t set);

/* The shared Thunderbolt PLL: fixed dividers, a combo PLL in all but
 * name (intel_dpll_icl.c); intel_tc.c refcounts it. */
int icl_tbt_pll_enable(struct i915_device *i915);
void icl_tbt_pll_disable(struct i915_device *i915);

/* Can a Type-C port's own PLL (MG or Dekel) be set to this port clock
 * (kHz; the link rate for DisplayPort, the TMDS clock for HDMI)? */
int intel_tc_pll_clock_ok(struct i915_device *i915, uint32_t clock_khz, int is_dp);

/* Broxton/Gemini Lake: bring up (if needed) the DPIO PHY that carries a
 * port -- and the PHY it copies its compensation from (intel_dpll_bxt.c). */
int bxt_dpio_phy_init_port(struct i915_device *i915, int port);

#endif
