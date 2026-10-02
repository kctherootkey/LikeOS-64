// LikeOS -- Valleyview and Cherryview: sideband, clocks, power wells and DPIO PHYs.
//
// Bay Trail (Valleyview) and Braswell (Cherryview) hang their display off
// an SoC fabric instead of a memory controller hub.  Much of what the
// display needs lives in other units reached through the IOSF sideband, a
// message bus driven through a doorbell register in the display block: the
// Punit gates the power wells and runs the CDCLK frequency handshake, the
// CCK reports and divides the clocks (HPLL, CZCLK, the display and
// reference clocks), the Bunit holds the self-refresh exit latency, and
// the DPIO PHYs carry both the pipe PLLs and the analog lanes of the
// DisplayPort/HDMI ports.  Valleyview has one PHY with two channels (ports
// B and C, pipes A and B); Cherryview adds a second, one-channel PHY for
// port D and pipe C, and its PHYs want their common lanes and data lanes
// powered and gated through DISPLAY_PHY_CONTROL, a register that must
// never be read back, so a shadow copy of it is kept here.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the IOSF sideband: the units the display talks to ---------------------- */

#define IOSF_PORT_BUNIT 0x03u
#define IOSF_PORT_PUNIT 0x04u
#define IOSF_PORT_NC 0x11u
#define IOSF_PORT_DPIO 0x12u
#define IOSF_PORT_CCK 0x14u
#define IOSF_PORT_DPIO_2 0x1au
#define IOSF_PORT_FLISDSI 0x1bu
#define IOSF_PORT_CCU 0xa9u

#define VLV_SB_DEVFN_00 0u /* bus device 0, function 0 */
#define VLV_DPIO_DEVFN 0u

/* opcodes */
#define SB_MRD_NP 0x00u /* MMIO read, non-posted */
#define SB_MWR_NP 0x01u /* MMIO write, non-posted */
#define SB_CRRDDA_NP 0x06u /* private register read, dword addressing */
#define SB_CRWRDA_NP 0x07u /* private register write, dword addressing */

/* ---- Bunit, Punit and CCK registers ---------------------------------------- */

#define BUNIT_REG_BISOC 0x11u

#define PUNIT_REG_DSPSSPM 0x36u
#define DSPFREQSTAT_SHIFT_CHV 24
#define DSPFREQSTAT_MASK_CHV (0x1fu << DSPFREQSTAT_SHIFT_CHV)
#define DSPFREQGUAR_SHIFT_CHV 8
#define DSPFREQGUAR_MASK_CHV (0x1fu << DSPFREQGUAR_SHIFT_CHV)
#define DSPFREQSTAT_SHIFT 30
#define DSPFREQSTAT_MASK (0x3u << DSPFREQSTAT_SHIFT)
#define DSPFREQGUAR_SHIFT 14
#define DSPFREQGUAR_MASK (0x3u << DSPFREQGUAR_SHIFT)
#define DP_SSC_MASK(p) (0x3u << (2 * (p)))
#define DP_SSC_PWR_ON(p) (0x0u << (2 * (p)))
#define DP_SSC_PWR_GATE(p) (0x3u << (2 * (p)))
#define DP_SSS_MASK(p) (0x3u << (2 * (p) + 16))
#define DP_SSS_PWR_ON(p) (0x0u << (2 * (p) + 16))
#define DP_SSS_PWR_GATE(p) (0x3u << (2 * (p) + 16))

#define PUNIT_REG_PWRGT_CTRL 0x60u
#define PUNIT_REG_PWRGT_STATUS 0x61u
#define PUNIT_PWRGT_MASK(i) (3u << ((i) * 2))
#define PUNIT_PWRGT_PWR_ON(i) (0u << ((i) * 2))
#define PUNIT_PWRGT_PWR_GATE(i) (3u << ((i) * 2))

#define PUNIT_PWGT_IDX_DISP2D 3
#define PUNIT_PWGT_IDX_DPIO_CMN_BC 5
#define PUNIT_PWGT_IDX_DPIO_TX_B_LANES_01 6
#define PUNIT_PWGT_IDX_DPIO_TX_B_LANES_23 7
#define PUNIT_PWGT_IDX_DPIO_TX_C_LANES_01 8
#define PUNIT_PWGT_IDX_DPIO_TX_C_LANES_23 9
#define PUNIT_PWGT_IDX_DPIO_CMN_D 12

#define CCK_FUSE_REG 0x8u
#define CCK_FUSE_HPLL_FREQ_MASK 0x3u
#define CCK_CZ_CLOCK_CONTROL 0x62u
#define CCK_DISPLAY_CLOCK_CONTROL 0x6bu
#define CCK_DISPLAY_REF_CLOCK_CONTROL 0x6cu
#define CCK_FREQUENCY_STATUS (0x1fu << 8)
#define CCK_FREQUENCY_STATUS_SHIFT 8
#define CCK_FREQUENCY_VALUES 0x1fu

/* ---- PHY control in the display block (display-relative) -------------------- */

#define DPIO_CTL 0x2110u
#define DPIO_CMNRST (1u << 0)
#define DPIO_PHY_STATUS 0x6240u
#define DPLL_PORTD_READY_MASK 0xfu
/* Cherryview: never read DISPLAY_PHY_CONTROL, reading can corrupt it */
#define DISPLAY_PHY_CONTROL 0x60100u
#define PHY_CH_POWER_DOWN_OVRD_EN(phy, ch) (1u << (2 * (phy) + (ch) + 27))
#define PHY_LDO_DELAY_600NS 0x2u
#define PHY_LDO_SEQ_DELAY(delay, phy) ((uint32_t)(delay) << (2 * (phy) + 23))
#define PHY_CH_POWER_DOWN_OVRD(mask, phy, ch) ((uint32_t)(mask) << (8 * (phy) + 4 * (ch) + 11))
#define PHY_CH_DEEP_PSR 0x7u
#define PHY_CH_POWER_MODE(mode, phy, ch) ((uint32_t)(mode) << (6 * (phy) + 3 * (ch) + 2))
#define PHY_COM_LANE_RESET_DEASSERT(phy) (1u << (phy))
#define DISPLAY_PHY_STATUS 0x60104u
#define PHY_POWERGOOD(phy) ((phy) == 0 ? (1u << 31) : (1u << 30))
#define PHY_STATUS_CMN_LDO(phy, ch) (1u << (6 - (6 * (phy) + 3 * (ch))))
#define PHY_STATUS_SPLINE_LDO(phy, ch, spline) (1u << (8 - (6 * (phy) + 3 * (ch) + (spline))))

/* ---- DPIO PHY registers (sideband addresses) -------------------------------- */

#define DPIO_FIELD(v, shift, mask) (((uint32_t)(v) << (shift)) & (mask))

#define _VLV_CMN(dw) (0x8100u + (dw) * 4u)
#define _CHV_CMN(cl, dw) (0x8100u - (cl) * 0x80u + (dw) * 4u)
#define _VLV_PLL(ch, dw) (0x8000u + (uint32_t)(ch) * 0x20u + (dw) * 4u)
#define _CHV_PLL(ch, dw) (0x8000u + (uint32_t)(ch) * 0x180u + (dw) * 4u)
#define _VLV_REF(dw) (0x80a0u + ((dw) - 8u) * 4u)
#define _VLV_PCS(ch, spline, dw) (0x200u + (uint32_t)(ch) * 0x2400u + (spline) * 0x200u + (dw) * 4u)
#define _VLV_PCS_GRP(ch, dw) (0x8200u + (uint32_t)(ch) * 0x200u + (dw) * 4u)
#define _VLV_PCS_BCAST(dw) (0xc000u + (dw) * 4u)
#define _VLV_TX(ch, lane, dw) (0x80u + (uint32_t)(ch) * 0x2400u + (uint32_t)(lane) * 0x200u + (dw) * 4u)
#define _VLV_TX_GRP(ch, dw) (0x8280u + (uint32_t)(ch) * 0x200u + (dw) * 4u)

/* per pipe PLL (Valleyview) */
#define VLV_PLL_DW3(ch) _VLV_PLL(ch, 3u)
#define DPIO_S1_DIV(x) DPIO_FIELD(x, 28, 0x70000000u)
#define DPIO_S1_DIV_HDMIDP 1 /* 5, DAC 225-400M rate */
#define DPIO_K_DIV(x) DPIO_FIELD(x, 24, 0x0f000000u)
#define DPIO_P1_DIV(x) DPIO_FIELD(x, 21, 0x00e00000u)
#define DPIO_P2_DIV(x) DPIO_FIELD(x, 16, 0x001f0000u)
#define DPIO_N_DIV(x) DPIO_FIELD(x, 12, 0x0000f000u)
#define DPIO_ENABLE_CALIBRATION (1u << 11)
#define DPIO_M1_DIV(x) DPIO_FIELD(x, 8, 0x00000700u)
#define DPIO_M2_DIV(x) DPIO_FIELD(x, 0, 0x000000ffu)
#define VLV_PLL_DW5(ch) _VLV_PLL(ch, 5u)
#define VLV_PLL_DW7(ch) _VLV_PLL(ch, 7u)
#define VLV_PLL_DW16(ch) _VLV_PLL(ch, 16u)
#define VLV_PLL_DW17(ch) _VLV_PLL(ch, 17u)
#define VLV_PLL_DW18(ch) _VLV_PLL(ch, 18u)
#define VLV_PLL_DW19(ch) _VLV_PLL(ch, 19u)
#define VLV_REF_DW11 _VLV_REF(11u)
#define VLV_CMN_DW0 _VLV_CMN(0u)

/* per channel PCS */
#define VLV_PCS_DW0_GRP(ch) _VLV_PCS_GRP(ch, 0u)
#define VLV_PCS01_DW0(ch) _VLV_PCS(ch, 0u, 0u)
#define VLV_PCS23_DW0(ch) _VLV_PCS(ch, 1u, 0u)
#define DPIO_PCS_TX_LANE2_RESET (1u << 16)
#define DPIO_PCS_TX_LANE1_RESET (1u << 7)
#define VLV_PCS_DW1_GRP(ch) _VLV_PCS_GRP(ch, 1u)
#define VLV_PCS01_DW1(ch) _VLV_PCS(ch, 0u, 1u)
#define VLV_PCS23_DW1(ch) _VLV_PCS(ch, 1u, 1u)
#define CHV_PCS_REQ_SOFTRESET_EN (1u << 23)
#define DPIO_PCS_CLK_CRI_RXEB_EIOS_EN (1u << 22)
#define DPIO_PCS_CLK_CRI_RXDIGFILTSG_EN (1u << 21)
#define DPIO_PCS_CLK_DATAWIDTH_8_10 (1u << 6)
#define DPIO_PCS_CLK_SOFT_RESET (1u << 5)
#define VLV_PCS_DW8_GRP(ch) _VLV_PCS_GRP(ch, 8u)
#define VLV_PCS01_DW8(ch) _VLV_PCS(ch, 0u, 8u)
#define VLV_PCS23_DW8(ch) _VLV_PCS(ch, 1u, 8u)
#define DPIO_PCS_USEDCLKCHANNEL (1u << 21)
#define DPIO_PCS_USEDCLKCHANNEL_OVRRIDE (1u << 20)
#define VLV_PCS_DW9_GRP(ch) _VLV_PCS_GRP(ch, 9u)
#define VLV_PCS01_DW9(ch) _VLV_PCS(ch, 0u, 9u)
#define VLV_PCS23_DW9(ch) _VLV_PCS(ch, 1u, 9u)
#define DPIO_PCS_TX2MARGIN_MASK (0x7u << 13)
#define DPIO_PCS_TX2MARGIN_000 (0u << 13)
#define DPIO_PCS_TX1MARGIN_MASK (0x7u << 10)
#define DPIO_PCS_TX1MARGIN_000 (0u << 10)
#define VLV_PCS01_DW10(ch) _VLV_PCS(ch, 0u, 10u)
#define VLV_PCS23_DW10(ch) _VLV_PCS(ch, 1u, 10u)
#define DPIO_PCS_SWING_CALC_TX1_TX3 (1u << 31)
#define DPIO_PCS_SWING_CALC_TX0_TX2 (1u << 30)
#define DPIO_PCS_TX2DEEMP_MASK (0xfu << 24)
#define DPIO_PCS_TX2DEEMP_9P5 (0u << 24)
#define DPIO_PCS_TX1DEEMP_MASK (0xfu << 16)
#define DPIO_PCS_TX1DEEMP_9P5 (0u << 16)
#define VLV_PCS_DW11_GRP(ch) _VLV_PCS_GRP(ch, 11u)
#define VLV_PCS01_DW11(ch) _VLV_PCS(ch, 0u, 11u)
#define VLV_PCS23_DW11(ch) _VLV_PCS(ch, 1u, 11u)
#define DPIO_TX2_STAGGER_MASK(x) DPIO_FIELD(x, 24, 0x1f000000u)
#define DPIO_LANEDESKEW_STRAP_OVRD (1u << 3)
#define VLV_PCS_DW12_GRP(ch) _VLV_PCS_GRP(ch, 12u)
#define VLV_PCS01_DW12(ch) _VLV_PCS(ch, 0u, 12u)
#define VLV_PCS23_DW12(ch) _VLV_PCS(ch, 1u, 12u)
#define DPIO_TX2_STAGGER_MULT(x) DPIO_FIELD(x, 20, 0x00700000u)
#define DPIO_TX1_STAGGER_MULT(x) DPIO_FIELD(x, 16, 0x001f0000u)
#define DPIO_TX1_STAGGER_MASK(x) DPIO_FIELD(x, 8, 0x00001f00u)
#define DPIO_LANESTAGGER_STRAP_OVRD (1u << 6)
#define DPIO_LANESTAGGER_STRAP(x) DPIO_FIELD(x, 0, 0x0000001fu)
#define VLV_PCS_DW14_GRP(ch) _VLV_PCS_GRP(ch, 14u)
#define VLV_PCS_DW17_BCAST _VLV_PCS_BCAST(17u)
#define VLV_PCS_DW23_GRP(ch) _VLV_PCS_GRP(ch, 23u)

/* per lane TX */
#define VLV_TX_DW2_GRP(ch) _VLV_TX_GRP(ch, 2u)
#define VLV_TX_DW3_GRP(ch) _VLV_TX_GRP(ch, 3u)
#define VLV_TX_DW4_GRP(ch) _VLV_TX_GRP(ch, 4u)
#define VLV_TX_DW4(ch, lane) _VLV_TX(ch, lane, 4u)
#define VLV_TX_DW5_GRP(ch) _VLV_TX_GRP(ch, 5u)
#define DPIO_TX_OCALINIT_EN (1u << 31)
#define VLV_TX_DW11_GRP(ch) _VLV_TX_GRP(ch, 11u)
#define VLV_TX_DW14_GRP(ch) _VLV_TX_GRP(ch, 14u)
#define CHV_TX_DW2(ch, lane) _VLV_TX(ch, lane, 2u)
#define DPIO_SWING_MARGIN000_MASK (0xffu << 16)
#define DPIO_SWING_MARGIN000(x) DPIO_FIELD(x, 16, DPIO_SWING_MARGIN000_MASK)
#define DPIO_UNIQ_TRANS_SCALE_MASK (0xffu << 8)
#define DPIO_UNIQ_TRANS_SCALE(x) DPIO_FIELD(x, 8, DPIO_UNIQ_TRANS_SCALE_MASK)
#define CHV_TX_DW3(ch, lane) _VLV_TX(ch, lane, 3u)
#define DPIO_TX_UNIQ_TRANS_SCALE_EN (1u << 27)
#define CHV_TX_DW4(ch, lane) _VLV_TX(ch, lane, 4u)
#define DPIO_SWING_DEEMPH9P5_MASK (0xffu << 24)
#define DPIO_SWING_DEEMPH9P5(x) DPIO_FIELD(x, 24, DPIO_SWING_DEEMPH9P5_MASK)
#define CHV_TX_DW14(ch, lane) _VLV_TX(ch, lane, 14u)
#define DPIO_UPAR (1u << 30)

/* Cherryview PLL */
#define CHV_PLL_DW0(ch) _CHV_PLL(ch, 0u)
#define DPIO_CHV_M2_DIV(x) DPIO_FIELD(x, 0, 0x000000ffu)
#define CHV_PLL_DW1(ch) _CHV_PLL(ch, 1u)
#define DPIO_CHV_N_DIV(x) DPIO_FIELD(x, 8, 0x00000f00u)
#define DPIO_CHV_M1_DIV(x) DPIO_FIELD(x, 0, 0x00000007u)
#define DPIO_CHV_M1_DIV_BY_2 0
#define CHV_PLL_DW2(ch) _CHV_PLL(ch, 2u)
#define DPIO_CHV_M2_FRAC_DIV(x) DPIO_FIELD(x, 0, 0x003fffffu)
#define CHV_PLL_DW3(ch) _CHV_PLL(ch, 3u)
#define DPIO_CHV_FRAC_DIV_EN (1u << 16)
#define DPIO_CHV_FEEDFWD_GAIN_MASK 0xfu
#define DPIO_CHV_FEEDFWD_GAIN(x) DPIO_FIELD(x, 0, DPIO_CHV_FEEDFWD_GAIN_MASK)
#define CHV_PLL_DW6(ch) _CHV_PLL(ch, 6u)
#define DPIO_CHV_GAIN_CTRL(x) DPIO_FIELD(x, 16, 0x00070000u)
#define DPIO_CHV_INT_COEFF(x) DPIO_FIELD(x, 8, 0x00001f00u)
#define DPIO_CHV_PROP_COEFF(x) DPIO_FIELD(x, 0, 0x0000000fu)
#define CHV_PLL_DW8(ch) _CHV_PLL(ch, 8u)
#define DPIO_CHV_TDC_TARGET_CNT_MASK 0x3ffu
#define DPIO_CHV_TDC_TARGET_CNT(x) DPIO_FIELD(x, 0, DPIO_CHV_TDC_TARGET_CNT_MASK)
#define CHV_PLL_DW9(ch) _CHV_PLL(ch, 9u)
#define DPIO_CHV_INT_LOCK_THRESHOLD_MASK (0x7u << 1)
#define DPIO_CHV_INT_LOCK_THRESHOLD(x) DPIO_FIELD(x, 1, DPIO_CHV_INT_LOCK_THRESHOLD_MASK)
#define DPIO_CHV_INT_LOCK_THRESHOLD_SEL_COARSE (1u << 0)

/* Cherryview common lanes */
#define CHV_CMN_DW5_CH0 _CHV_CMN(0u, 5u)
#define CHV_BUFRIGHTENA1_MASK (3u << 20)
#define CHV_BUFRIGHTENA1_FORCE (3u << 20)
#define CHV_BUFLEFTENA1_MASK (3u << 22)
#define CHV_BUFLEFTENA1_FORCE (3u << 22)
#define CHV_CMN_DW13_CH0 _CHV_CMN(0u, 13u)
#define CHV_CMN_DW0_CH1 _CHV_CMN(1u, 0u)
#define DPIO_CHV_S1_DIV(x) DPIO_FIELD(x, 21, 0x00e00000u)
#define DPIO_CHV_P1_DIV(x) DPIO_FIELD(x, 13, 0x0000e000u)
#define DPIO_CHV_P2_DIV(x) DPIO_FIELD(x, 8, 0x00001f00u)
#define DPIO_CHV_K_DIV(x) DPIO_FIELD(x, 4, 0x000000f0u)
#define CHV_CMN_DW13(ch) ((ch) ? CHV_CMN_DW0_CH1 : CHV_CMN_DW13_CH0)
#define CHV_CMN_DW14_CH0 _CHV_CMN(0u, 14u)
#define CHV_CMN_DW1_CH1 _CHV_CMN(1u, 1u)
#define DPIO_AFC_RECAL (1u << 14)
#define DPIO_DCLKP_EN (1u << 13)
#define CHV_BUFLEFTENA2_MASK (3u << 17)
#define CHV_BUFLEFTENA2_FORCE (3u << 17)
#define CHV_BUFRIGHTENA2_MASK (3u << 19)
#define CHV_BUFRIGHTENA2_FORCE (3u << 19)
#define CHV_CMN_DW14(ch) ((ch) ? CHV_CMN_DW1_CH1 : CHV_CMN_DW14_CH0)
#define CHV_CMN_DW19_CH0 _CHV_CMN(0u, 19u)
#define CHV_CMN_DW6_CH1 _CHV_CMN(1u, 6u)
#define DPIO_DYNPWRDOWNEN_CH1 (1u << 28)
#define CHV_CMN_USEDCLKCHANNEL (1u << 13)
#define CHV_CMN_DW19(ch) ((ch) ? CHV_CMN_DW6_CH1 : CHV_CMN_DW19_CH0)
#define CHV_CMN_DW28 _CHV_CMN(0u, 28u)
#define DPIO_CL1POWERDOWNEN (1u << 23)
#define DPIO_DYNPWRDOWNEN_CH0 (1u << 22)
#define DPIO_SUS_CLK_CONFIG_GATE_CLKREQ 3u
#define CHV_CMN_DW30 _CHV_CMN(0u, 30u)
#define DPIO_CL2_LDOFUSE_PWRENB (1u << 6)

#define VLV_PIPE_A 0
#define VLV_PIPE_B 1
#define VLV_PIPE_C 2
#define VLV_DPIO_PHY0 0
#define VLV_DPIO_PHY1 1
#define VLV_DPIO_CH0 0
#define VLV_DPIO_CH1 1

/* ---- state ------------------------------------------------------------------ */

/* Cherryview: the shadow of DISPLAY_PHY_CONTROL, whether a PHY has been
 * fully reset since boot (before that the firmware may have left parts of
 * it in odd states, so its status is not checked), and per port whether
 * pre_pll_enable forced the second common lane on and must release it. */
static uint32_t chv_phy_control;
static int chv_phy_check[2];
static int chv_release_cl2_override[LG_PORT_D + 1];

/* ---- the sideband ------------------------------------------------------------ */

/* One message.  The doorbell is shared with the GT's power code: the GT
 * backend has the one implementation, and the one lock. */
static int vlv_sideband_rw(struct lg_display *d, uint32_t devfn, uint32_t port,
			   uint32_t opcode, uint32_t addr, uint32_t *val)
{
	int err = i915_legacy_vlv_sideband(d->i915, devfn, port, opcode, addr, val);

	if (err)
		i915_dbg("[drm] i915: IOSF sideband message failed (%d, port 0x%x addr 0x%x)\n",
			 err, port, addr);
	return err;
}

/* Private registers of a unit (dword addressing). */
static uint32_t vlv_cr_read(struct lg_display *d, uint32_t devfn, uint32_t port,
			    uint32_t addr)
{
	uint32_t val = 0;

	vlv_sideband_rw(d, devfn, port, SB_CRRDDA_NP, addr, &val);
	return val;
}

static int vlv_cr_write(struct lg_display *d, uint32_t devfn, uint32_t port,
			uint32_t addr, uint32_t val)
{
	return vlv_sideband_rw(d, devfn, port, SB_CRWRDA_NP, addr, &val);
}

uint32_t lg_vlv_punit_read(struct lg_display *d, uint32_t addr)
{
	return vlv_cr_read(d, VLV_SB_DEVFN_00, IOSF_PORT_PUNIT, addr);
}

int lg_vlv_punit_write(struct lg_display *d, uint32_t addr, uint32_t val)
{
	return vlv_cr_write(d, VLV_SB_DEVFN_00, IOSF_PORT_PUNIT, addr, val);
}

uint32_t lg_vlv_cck_read(struct lg_display *d, uint32_t reg)
{
	return vlv_cr_read(d, VLV_SB_DEVFN_00, IOSF_PORT_CCK, reg);
}

void lg_vlv_cck_write(struct lg_display *d, uint32_t reg, uint32_t val)
{
	vlv_cr_write(d, VLV_SB_DEVFN_00, IOSF_PORT_CCK, reg, val);
}

uint32_t lg_vlv_ccu_read(struct lg_display *d, uint32_t reg)
{
	return vlv_cr_read(d, VLV_SB_DEVFN_00, IOSF_PORT_CCU, reg);
}

void lg_vlv_ccu_write(struct lg_display *d, uint32_t reg, uint32_t val)
{
	vlv_cr_write(d, VLV_SB_DEVFN_00, IOSF_PORT_CCU, reg, val);
}

uint32_t lg_vlv_nc_read(struct lg_display *d, uint32_t addr)
{
	return vlv_cr_read(d, VLV_SB_DEVFN_00, IOSF_PORT_NC, addr & 0xffu);
}

static uint32_t vlv_bunit_read(struct lg_display *d, uint32_t reg)
{
	return vlv_cr_read(d, VLV_SB_DEVFN_00, IOSF_PORT_BUNIT, reg);
}

static void vlv_bunit_write(struct lg_display *d, uint32_t reg, uint32_t val)
{
	vlv_cr_write(d, VLV_SB_DEVFN_00, IOSF_PORT_BUNIT, reg, val);
}

/*
 * The DPIO port: on Valleyview the one PHY (ports B and C); on Cherryview
 * the second DPIO port carries the two-channel PHY0 (B and C) and the
 * first the one-channel PHY1 (D).
 */
static uint32_t vlv_dpio_port(struct lg_display *d, int phy)
{
	if (d->is_chv)
		return phy == VLV_DPIO_PHY0 ? IOSF_PORT_DPIO_2 : IOSF_PORT_DPIO;
	return IOSF_PORT_DPIO;
}

uint32_t lg_vlv_dpio_read(struct lg_display *d, int phy, uint32_t reg)
{
	uint32_t val = 0;

	vlv_sideband_rw(d, VLV_DPIO_DEVFN, vlv_dpio_port(d, phy), SB_MRD_NP, reg, &val);

	/* All ones usually means the PHY (or its channel) is powered off. */
	if (val == 0xffffffffu)
		i915_dbg("[drm] i915: DPIO PHY%d read reg 0x%x == 0x%x\n", phy, reg, val);

	return val;
}

void lg_vlv_dpio_write(struct lg_display *d, int phy, uint32_t reg, uint32_t val)
{
	vlv_sideband_rw(d, VLV_DPIO_DEVFN, vlv_dpio_port(d, phy), SB_MWR_NP, reg, &val);
}

uint32_t lg_vlv_flisdsi_read(struct lg_display *d, uint32_t reg)
{
	return vlv_cr_read(d, VLV_DPIO_DEVFN, IOSF_PORT_FLISDSI, reg);
}

void lg_vlv_flisdsi_write(struct lg_display *d, uint32_t reg, uint32_t val)
{
	vlv_cr_write(d, VLV_DPIO_DEVFN, IOSF_PORT_FLISDSI, reg, val);
}

/* Any unit by port number: the DPIO ports take MMIO opcodes, everything
 * else (the GPIO communities, say) private register opcodes. */
uint32_t lg_vlv_iosf_read(struct lg_display *d, int port, uint32_t reg)
{
	uint32_t val = 0;

	if (port == (int)IOSF_PORT_DPIO || port == (int)IOSF_PORT_DPIO_2)
		vlv_sideband_rw(d, VLV_DPIO_DEVFN, (uint32_t)port, SB_MRD_NP, reg, &val);
	else
		vlv_sideband_rw(d, VLV_SB_DEVFN_00, (uint32_t)port, SB_CRRDDA_NP, reg, &val);
	return val;
}

void lg_vlv_iosf_write(struct lg_display *d, int port, uint32_t reg, uint32_t val)
{
	if (port == (int)IOSF_PORT_DPIO || port == (int)IOSF_PORT_DPIO_2)
		vlv_sideband_rw(d, VLV_DPIO_DEVFN, (uint32_t)port, SB_MWR_NP, reg, &val);
	else
		vlv_sideband_rw(d, VLV_SB_DEVFN_00, (uint32_t)port, SB_CRWRDA_NP, reg, &val);
}

/* Poll a sideband register (Punit or CCK) every 500 us until
 * (val & mask) == want; 0 or -ETIMEDOUT, the last value in *last. */
static int vlv_sb_poll(struct lg_display *d, uint32_t (*rd)(struct lg_display *, uint32_t),
		       uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us,
		       uint32_t *last)
{
	uint32_t waited = 0, val;

	for (;;) {
		val = rd(d, reg);
		if ((val & mask) == want) {
			if (last)
				*last = val;
			return 0;
		}
		if (waited >= timeout_us) {
			if (last)
				*last = val;
			return -ETIMEDOUT;
		}
		lg_udelay(500);
		waited += 500;
	}
}

/* ---- PHY and channel of a port or pipe ------------------------------------------ */

int lg_vlv_port_to_phy(struct lg_display *d, int port)
{
	(void)d;
	switch (port) {
	case LG_PORT_D:
		return VLV_DPIO_PHY1;
	case LG_PORT_B:
	case LG_PORT_C:
	default:
		return VLV_DPIO_PHY0;
	}
}

int lg_vlv_port_to_channel(int port)
{
	switch (port) {
	case LG_PORT_C:
		return VLV_DPIO_CH1;
	case LG_PORT_B:
	case LG_PORT_D:
	default:
		return VLV_DPIO_CH0;
	}
}

int lg_vlv_pipe_to_phy(struct lg_display *d, int pipe)
{
	(void)d;
	switch (pipe) {
	case VLV_PIPE_C:
		return VLV_DPIO_PHY1;
	case VLV_PIPE_A:
	case VLV_PIPE_B:
	default:
		return VLV_DPIO_PHY0;
	}
}

int lg_vlv_pipe_to_channel(int pipe)
{
	switch (pipe) {
	case VLV_PIPE_B:
		return VLV_DPIO_CH1;
	case VLV_PIPE_A:
	case VLV_PIPE_C:
	default:
		return VLV_DPIO_CH0;
	}
}

/* ---- clocks ------------------------------------------------------------------- */

/* The HPLL VCO from the CCK fuses (kHz); everything else divides it. */
static uint32_t vlv_hpll_vco(struct lg_display *d)
{
	static const uint32_t vco_mhz[] = { 800, 1600, 2000, 2400 };
	uint32_t sel;

	if (!d->hpll_vco_khz) {
		sel = lg_vlv_cck_read(d, CCK_FUSE_REG) & CCK_FUSE_HPLL_FREQ_MASK;
		d->hpll_vco_khz = vco_mhz[sel] * 1000u;
		i915_dbg("[drm] i915: HPLL frequency: %u kHz\n", d->hpll_vco_khz);
	}
	return d->hpll_vco_khz;
}

/* A CCK clock: its input clock doubled over the divider field plus one. */
static uint32_t vlv_cck_clock(struct lg_display *d, const char *name, uint32_t reg,
			      uint32_t ref_khz)
{
	uint32_t val = lg_vlv_cck_read(d, reg);
	uint32_t divider = val & CCK_FREQUENCY_VALUES;

	if ((val & CCK_FREQUENCY_STATUS) != (divider << CCK_FREQUENCY_STATUS_SHIFT))
		i915_dbg("[drm] i915: %s change in progress\n", name);

	return DIV_ROUND_CLOSEST(ref_khz << 1, divider + 1);
}

static uint32_t vlv_hrawclk(struct lg_display *d)
{
	return vlv_cck_clock(d, "hrawclk", CCK_DISPLAY_REF_CLOCK_CONTROL, vlv_hpll_vco(d));
}

static uint32_t vlv_czclk(struct lg_display *d)
{
	if (!d->czclk_khz) {
		d->czclk_khz = vlv_cck_clock(d, "czclk", CCK_CZ_CLOCK_CONTROL, vlv_hpll_vco(d));
		i915_dbg("[drm] i915: CZ clock rate: %u kHz\n", d->czclk_khz);
	}
	return d->czclk_khz;
}

/* Is the display's power well up (disp2d on Valleyview, the pipe A well
 * on Cherryview)?  Without it the display registers do not answer. */
static int vlv_power_well_enabled(struct lg_display *d, int idx)
{
	uint32_t state = lg_vlv_punit_read(d, PUNIT_REG_PWRGT_STATUS) & PUNIT_PWRGT_MASK(idx);

	return state == PUNIT_PWRGT_PWR_ON(idx);
}

static int chv_pipe_power_well_enabled(struct lg_display *d)
{
	uint32_t state = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM) & DP_SSS_MASK(VLV_PIPE_A);

	return state == DP_SSS_PWR_ON(VLV_PIPE_A);
}

static int vlv_display_powered(struct lg_display *d)
{
	if (d->is_chv)
		return chv_pipe_power_well_enabled(d);
	return vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DISP2D);
}

/*
 * Read back CDCLK; the GMBUS clock is derived from it, so GMBUSFREQ (bits
 * 9:2 the number of CDCLK cycles in a 4 MHz period) follows it.
 */
static void vlv_update_cdclk(struct lg_display *d, int powered)
{
	d->cdclk_khz = vlv_cck_clock(d, "cdclk", CCK_DISPLAY_CLOCK_CONTROL, vlv_hpll_vco(d));

	if (powered)
		lg_wr(d, GMBUSFREQ_VLV, DIV_ROUND_UP(d->cdclk_khz, 1000));
}

static uint32_t vlv_calc_cdclk(struct lg_display *d, uint32_t min_cdclk)
{
	uint32_t freq_320 = ((vlv_hpll_vco(d) << 1) % 320000) != 0 ? 333333 : 320000;

	/*
	 * 200 MHz gives an unstable or solid colour picture, so it is only
	 * used with every pipe off.
	 */
	if (d->is_vlv && min_cdclk > freq_320)
		return 400000;
	else if (min_cdclk > 266667)
		return freq_320;
	else if (min_cdclk > 0)
		return 266667;
	else
		return 200000;
}

static uint32_t vlv_calc_voltage_level(struct lg_display *d, uint32_t cdclk)
{
	if (d->is_vlv) {
		if (cdclk >= 320000) /* the highest voltage for 400 MHz too */
			return 2;
		else if (cdclk >= 266667)
			return 1;
		else
			return 0;
	}

	/* Cherryview: the Punit just takes the CCK divider it should set. */
	return DIV_ROUND_CLOSEST(vlv_hpll_vco(d) << 1, cdclk) - 1;
}

/* The PFI credits between the display and the fabric follow the ratio of
 * CDCLK to CZCLK. */
static void vlv_program_pfi_credits(struct lg_display *d)
{
	uint32_t credits, default_credits;

	if (d->is_chv)
		default_credits = PFI_CREDIT(12);
	else
		default_credits = PFI_CREDIT(8);

	if (d->cdclk_khz >= vlv_czclk(d)) {
		/* Cherryview: 31 or 63 suggested */
		if (d->is_chv)
			credits = PFI_CREDIT_63;
		else
			credits = PFI_CREDIT(15);
	} else {
		credits = default_credits;
	}

	/* The default credits must be written before the new ones. */
	lg_wr(d, GCI_CONTROL, VGA_FAST_MODE_DISABLE | default_credits);
	lg_wr(d, GCI_CONTROL, VGA_FAST_MODE_DISABLE | credits | PFI_CREDIT_RESEND);

	if (lg_rd(d, GCI_CONTROL) & PFI_CREDIT_RESEND)
		i915_dbg("[drm] i915: PFI credit resend still pending\n");
}

static void vlv_set_cdclk(struct lg_display *d, uint32_t cdclk, uint32_t cmd)
{
	uint32_t val;

	switch (cdclk) {
	case 400000:
	case 333333:
	case 320000:
	case 266667:
	case 200000:
		break;
	default:
		kprintf("[drm] i915: unsupported CDCLK %u kHz\n", cdclk);
		return;
	}

	/* The Punit's DSPFREQ handshake: ask for the voltage level, wait for
	 * the status to report it. */
	val = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
	val &= ~DSPFREQGUAR_MASK;
	val |= cmd << DSPFREQGUAR_SHIFT;
	lg_vlv_punit_write(d, PUNIT_REG_DSPSSPM, val);

	if (vlv_sb_poll(d, lg_vlv_punit_read, PUNIT_REG_DSPSSPM, DSPFREQSTAT_MASK,
			cmd << DSPFREQSTAT_SHIFT, 50000, NULL))
		kprintf("[drm] i915: timed out waiting for CDCLK change\n");

	if (cdclk == 400000) {
		uint32_t divider = DIV_ROUND_CLOSEST(vlv_hpll_vco(d) << 1, cdclk) - 1;

		/* adjust the CDCLK divider */
		val = lg_vlv_cck_read(d, CCK_DISPLAY_CLOCK_CONTROL);
		val &= ~CCK_FREQUENCY_VALUES;
		val |= divider;
		lg_vlv_cck_write(d, CCK_DISPLAY_CLOCK_CONTROL, val);

		if (vlv_sb_poll(d, lg_vlv_cck_read, CCK_DISPLAY_CLOCK_CONTROL,
				CCK_FREQUENCY_STATUS, divider << CCK_FREQUENCY_STATUS_SHIFT,
				50000, NULL))
			kprintf("[drm] i915: timed out waiting for CDCLK change\n");
	}

	/*
	 * The self-refresh exit latency in the Bunit: high bandwidth
	 * configurations want a longer one so that the display fetch happens
	 * in time to avoid underruns.
	 */
	val = vlv_bunit_read(d, BUNIT_REG_BISOC);
	val &= ~0x7fu;
	if (cdclk == 400000)
		val |= 4500 / 250; /* 4.5 us */
	else
		val |= 3000 / 250; /* 3.0 us */
	vlv_bunit_write(d, BUNIT_REG_BISOC, val);

	vlv_update_cdclk(d, 1);
	vlv_program_pfi_credits(d);
}

static void chv_set_cdclk(struct lg_display *d, uint32_t cdclk, uint32_t cmd)
{
	uint32_t val;

	switch (cdclk) {
	case 333333:
	case 320000:
	case 266667:
	case 200000:
		break;
	default:
		kprintf("[drm] i915: unsupported CDCLK %u kHz\n", cdclk);
		return;
	}

	val = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
	val &= ~DSPFREQGUAR_MASK_CHV;
	val |= cmd << DSPFREQGUAR_SHIFT_CHV;
	lg_vlv_punit_write(d, PUNIT_REG_DSPSSPM, val);

	if (vlv_sb_poll(d, lg_vlv_punit_read, PUNIT_REG_DSPSSPM, DSPFREQSTAT_MASK_CHV,
			cmd << DSPFREQSTAT_SHIFT_CHV, 50000, NULL))
		kprintf("[drm] i915: timed out waiting for CDCLK change\n");

	vlv_update_cdclk(d, 1);
	vlv_program_pfi_credits(d);
}

void lg_vlv_clocks_init(struct lg_display *d)
{
	uint32_t val, level, target;
	int powered, pipe, any_on = 0;

	if (!d->is_vlv && !d->is_chv)
		return;

	vlv_hpll_vco(d);
	vlv_czclk(d);
	/* The display's raw clock (RAWCLK_FREQ_VLV is programmed from it by
	 * the power well code). */
	d->rawclk_khz = vlv_hrawclk(d);

	d->max_cdclk_khz = d->is_chv ? 320000 : 400000;
	/* One pixel a CDCLK cycle, with a 95% (CHV) or 90% guardband. */
	d->max_dotclk_khz = d->max_cdclk_khz * (d->is_chv ? 95u : 90u) / 100u;

	powered = vlv_display_powered(d);
	vlv_update_cdclk(d, powered);

	val = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
	if (d->is_vlv)
		level = (val & DSPFREQGUAR_MASK) >> DSPFREQGUAR_SHIFT;
	else
		level = (val & DSPFREQGUAR_MASK_CHV) >> DSPFREQGUAR_SHIFT_CHV;

	i915_dbg("[drm] i915: HPLL %u kHz, CZCLK %u kHz, rawclk %u kHz, CDCLK %u kHz (voltage level %u), max CDCLK %u kHz, max dot clock %u kHz\n",
		 d->hpll_vco_khz, d->czclk_khz, d->rawclk_khz, d->cdclk_khz, level,
		 d->max_cdclk_khz, d->max_dotclk_khz);

	if (!powered) {
		i915_dbg("[drm] i915: display power well off, CDCLK left as it is\n");
		return;
	}

	for (pipe = 0; pipe < d->num_pipes; pipe++)
		if (lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE)
			any_on = 1;
	if (any_on)
		return;

	/* Every pipe is off: run CDCLK at the most the part allows. */
	target = vlv_calc_cdclk(d, d->max_cdclk_khz);
	if (target == d->cdclk_khz)
		return;

	if (d->is_chv)
		chv_set_cdclk(d, target, vlv_calc_voltage_level(d, target));
	else
		vlv_set_cdclk(d, target, vlv_calc_voltage_level(d, target));

	i915_dbg("[drm] i915: CDCLK now %u kHz (asked for %u kHz)\n", d->cdclk_khz, target);
}

/* ---- power wells -------------------------------------------------------------- */

static int vlv_set_power_well(struct lg_display *d, int idx, int enable)
{
	uint32_t mask = PUNIT_PWRGT_MASK(idx);
	uint32_t state = enable ? PUNIT_PWRGT_PWR_ON(idx) : PUNIT_PWRGT_PWR_GATE(idx);
	uint32_t ctrl, val;

	val = lg_vlv_punit_read(d, PUNIT_REG_PWRGT_STATUS);
	if ((val & mask) == state)
		return 0;

	ctrl = lg_vlv_punit_read(d, PUNIT_REG_PWRGT_CTRL);
	ctrl &= ~mask;
	ctrl |= state;
	lg_vlv_punit_write(d, PUNIT_REG_PWRGT_CTRL, ctrl);

	if (vlv_sb_poll(d, lg_vlv_punit_read, PUNIT_REG_PWRGT_STATUS, mask, state, 100000,
			NULL)) {
		kprintf("[drm] i915: timeout setting power well %d state %08x (%08x)\n", idx,
			state, lg_vlv_punit_read(d, PUNIT_REG_PWRGT_CTRL));
		return -ETIMEDOUT;
	}
	return 0;
}

/* Cherryview: the pipe A well is the old disp2d well, and every pipe
 * needs it (the wells of pipes B and C do not exist). */
static int chv_set_pipe_power_well(struct lg_display *d, int enable)
{
	uint32_t state = enable ? DP_SSS_PWR_ON(VLV_PIPE_A) : DP_SSS_PWR_GATE(VLV_PIPE_A);
	uint32_t ctrl;

	ctrl = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
	if ((ctrl & DP_SSS_MASK(VLV_PIPE_A)) == state)
		return 0;

	ctrl &= ~DP_SSC_MASK(VLV_PIPE_A);
	ctrl |= enable ? DP_SSC_PWR_ON(VLV_PIPE_A) : DP_SSC_PWR_GATE(VLV_PIPE_A);
	lg_vlv_punit_write(d, PUNIT_REG_DSPSSPM, ctrl);

	if (vlv_sb_poll(d, lg_vlv_punit_read, PUNIT_REG_DSPSSPM, DP_SSS_MASK(VLV_PIPE_A),
			state, 100000, NULL)) {
		kprintf("[drm] i915: timeout setting power well state %08x (%08x)\n", state,
			lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM));
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * What the display needs once its well is up: the CRI clock source (so the
 * display and the VGA hotplug/load detection reference clock run; DSI wants
 * a reference clock too; DPLLs B and C misbehave with VGA mode on), the
 * clock gating, trickle feed off, the PND deadline calculation on, and the
 * raw clock frequency.
 */
static void vlv_display_power_well_init(struct lg_display *d)
{
	int pipe;
	uint32_t val;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		val = lg_rd(d, DPLL(d, pipe));
		val |= DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS;
		if (pipe != VLV_PIPE_A)
			val |= DPLL_INTEGRATED_CRI_CLK_VLV;
		lg_wr(d, DPLL(d, pipe), val);
	}

	/*
	 * A pipe may be driving a DSI panel from the firmware; keep
	 * DPOUNIT_CLOCK_GATE_DISABLE as it is so it does not get stuck.
	 */
	lg_rmw(d, DSPCLK_GATE_D, ~DPOUNIT_CLOCK_GATE_DISABLE, VRHUNIT_CLOCK_GATE_DISABLE);

	lg_wr(d, MI_ARB_VLV, MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE_VLV);
	lg_wr(d, CBR1_VLV, 0);

	if (!d->rawclk_khz)
		d->rawclk_khz = vlv_hrawclk(d);
	lg_wr(d, RAWCLK_FREQ_VLV, DIV_ROUND_CLOSEST(d->rawclk_khz, 1000));
}

static int vlv_dpio_cmn_power_well_enable(struct lg_display *d)
{
	int ret;

	/* The ref/CRI clock is on: >10 ns for cmnreset, >0 ns for sidereset. */
	lg_udelay(1);

	ret = vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_CMN_BC, 1);

	/*
	 * De-assert cmn_reset/side_reset (bit 0 of DPIO_CTL; the other bits,
	 * SFR settings and mode select, may stay 0).  Only on init and resume
	 * with both PLLs off, or DPIO and PLL synchronisation may be lost.
	 */
	lg_rmw(d, DPIO_CTL, 0, DPIO_CMNRST);
	return ret;
}

static void vlv_dpio_cmn_power_well_disable(struct lg_display *d)
{
	/* assert common reset */
	lg_rmw(d, DPIO_CTL, DPIO_CMNRST, 0);
	vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_CMN_BC, 0);
}

/*
 * Valleyview: the PHY's sideband reset needs the common lane power gated
 * and ungated again; ungating alone does not reset it enough for the ports
 * and lanes to run.  Skipped when the firmware left the display running.
 */
static void vlv_cmnlane_wa(struct lg_display *d)
{
	if (vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DPIO_CMN_BC) &&
	    vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DISP2D) &&
	    (lg_rd(d, DPIO_CTL) & DPIO_CMNRST))
		return;

	i915_dbg("[drm] i915: toggling display PHY side reset\n");

	/* the common lanes need the DPLL registers */
	vlv_set_power_well(d, PUNIT_PWGT_IDX_DISP2D, 1);
	vlv_display_power_well_init(d);

	vlv_dpio_cmn_power_well_disable(d);
}

#define BITS_SET(val, bits) (((val) & (bits)) == (bits))

/*
 * Cherryview: what DISPLAY_PHY_STATUS should say for the shadowed control
 * value and the common lane wells, waited for (the PHY may still be busy
 * calibrating, so its power state can take a while to follow).
 */
static void chv_phy_status_wait(struct lg_display *d)
{
	uint32_t phy_control = chv_phy_control;
	uint32_t phy_status = 0;
	uint32_t phy_status_mask = 0xffffffffu;

	/* Not before the PHY was fully reset once: the firmware can leave it
	 * not entirely powered down. */
	if (!chv_phy_check[VLV_DPIO_PHY0])
		phy_status_mask &= ~(PHY_STATUS_CMN_LDO(0, 0) | PHY_STATUS_SPLINE_LDO(0, 0, 0) |
				     PHY_STATUS_SPLINE_LDO(0, 0, 1) | PHY_STATUS_CMN_LDO(0, 1) |
				     PHY_STATUS_SPLINE_LDO(0, 1, 0) |
				     PHY_STATUS_SPLINE_LDO(0, 1, 1));

	if (!chv_phy_check[VLV_DPIO_PHY1])
		phy_status_mask &= ~(PHY_STATUS_CMN_LDO(1, 0) | PHY_STATUS_SPLINE_LDO(1, 0, 0) |
				     PHY_STATUS_SPLINE_LDO(1, 0, 1));

	if (vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DPIO_CMN_BC)) {
		phy_status |= PHY_POWERGOOD(VLV_DPIO_PHY0);

		/* the override is only used to enable lanes */
		if ((phy_control & PHY_CH_POWER_DOWN_OVRD_EN(0, 0)) == 0)
			phy_control |= PHY_CH_POWER_DOWN_OVRD(0xf, 0, 0);
		if ((phy_control & PHY_CH_POWER_DOWN_OVRD_EN(0, 1)) == 0)
			phy_control |= PHY_CH_POWER_DOWN_OVRD(0xf, 0, 1);

		/* CL1 is on whenever anything is on in either channel */
		if (BITS_SET(phy_control,
			     PHY_CH_POWER_DOWN_OVRD(0xf, 0, 0) | PHY_CH_POWER_DOWN_OVRD(0xf, 0, 1)))
			phy_status |= PHY_STATUS_CMN_LDO(0, 0);

		/* The DPLL B check covers pipe B on port B with CL2 up but
		 * every lane of the second channel down. */
		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0xf, 0, 1)) &&
		    (lg_rd(d, DPLL(d, VLV_PIPE_B)) & DPLL_VCO_ENABLE) == 0)
			phy_status |= PHY_STATUS_CMN_LDO(0, 1);

		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0x3, 0, 0)))
			phy_status |= PHY_STATUS_SPLINE_LDO(0, 0, 0);
		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0xc, 0, 0)))
			phy_status |= PHY_STATUS_SPLINE_LDO(0, 0, 1);

		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0x3, 0, 1)))
			phy_status |= PHY_STATUS_SPLINE_LDO(0, 1, 0);
		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0xc, 0, 1)))
			phy_status |= PHY_STATUS_SPLINE_LDO(0, 1, 1);
	}

	if (vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DPIO_CMN_D)) {
		phy_status |= PHY_POWERGOOD(VLV_DPIO_PHY1);

		if ((phy_control & PHY_CH_POWER_DOWN_OVRD_EN(1, 0)) == 0)
			phy_control |= PHY_CH_POWER_DOWN_OVRD(0xf, 1, 0);

		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0xf, 1, 0)))
			phy_status |= PHY_STATUS_CMN_LDO(1, 0);

		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0x3, 1, 0)))
			phy_status |= PHY_STATUS_SPLINE_LDO(1, 0, 0);
		if (BITS_SET(phy_control, PHY_CH_POWER_DOWN_OVRD(0xc, 1, 0)))
			phy_status |= PHY_STATUS_SPLINE_LDO(1, 0, 1);
	}

	phy_status &= phy_status_mask;

	if (lg_wait(d, DISPLAY_PHY_STATUS, phy_status_mask, phy_status, 10000))
		kprintf("[drm] i915: unexpected PHY_STATUS 0x%08x, expected 0x%08x (PHY_CONTROL=0x%08x)\n",
			lg_rd(d, DISPLAY_PHY_STATUS) & phy_status_mask, phy_status,
			chv_phy_control);
}

#undef BITS_SET

/*
 * The initial shadow of DISPLAY_PHY_CONTROL (the register can get corrupted
 * when read, so it never is), rebuilt from the wells and the lane status.
 * With every lane of a channel down the override stays off with no power
 * down bits, as after a port disable; otherwise the override is on with the
 * lanes as they are.
 */
static void chv_phy_control_init(struct lg_display *d)
{
	uint32_t status;
	unsigned int mask;

	chv_phy_control = PHY_LDO_SEQ_DELAY(PHY_LDO_DELAY_600NS, 0) |
			  PHY_LDO_SEQ_DELAY(PHY_LDO_DELAY_600NS, 1) |
			  PHY_CH_POWER_MODE(PHY_CH_DEEP_PSR, 0, 0) |
			  PHY_CH_POWER_MODE(PHY_CH_DEEP_PSR, 0, 1) |
			  PHY_CH_POWER_MODE(PHY_CH_DEEP_PSR, 1, 0);

	if (vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DPIO_CMN_BC)) {
		status = lg_rd(d, DPLL(d, VLV_PIPE_A));

		mask = status & DPLL_PORTB_READY_MASK;
		if (mask == 0xf)
			mask = 0x0;
		else
			chv_phy_control |= PHY_CH_POWER_DOWN_OVRD_EN(0, 0);
		chv_phy_control |= PHY_CH_POWER_DOWN_OVRD(mask, 0, 0);

		mask = (status & DPLL_PORTC_READY_MASK) >> 4;
		if (mask == 0xf)
			mask = 0x0;
		else
			chv_phy_control |= PHY_CH_POWER_DOWN_OVRD_EN(0, 1);
		chv_phy_control |= PHY_CH_POWER_DOWN_OVRD(mask, 0, 1);

		chv_phy_control |= PHY_COM_LANE_RESET_DEASSERT(0);
		chv_phy_check[VLV_DPIO_PHY0] = 0;
	} else {
		chv_phy_check[VLV_DPIO_PHY0] = 1;
	}

	if (vlv_power_well_enabled(d, PUNIT_PWGT_IDX_DPIO_CMN_D)) {
		status = lg_rd(d, DPIO_PHY_STATUS);

		mask = status & DPLL_PORTD_READY_MASK;
		if (mask == 0xf)
			mask = 0x0;
		else
			chv_phy_control |= PHY_CH_POWER_DOWN_OVRD_EN(1, 0);
		chv_phy_control |= PHY_CH_POWER_DOWN_OVRD(mask, 1, 0);

		chv_phy_control |= PHY_COM_LANE_RESET_DEASSERT(1);
		chv_phy_check[VLV_DPIO_PHY1] = 0;
	} else {
		chv_phy_check[VLV_DPIO_PHY1] = 1;
	}

	i915_dbg("[drm] i915: initial PHY_CONTROL=0x%08x\n", chv_phy_control);
	/* written when the common lane wells are enabled */
}

static int chv_dpio_cmn_power_well_enable(struct lg_display *d, int phy)
{
	uint32_t tmp;
	int ret;

	/* The ref/CRI clock is on: >10 ns for cmnreset, >0 ns for sidereset. */
	lg_udelay(1);
	ret = vlv_set_power_well(d, phy == VLV_DPIO_PHY0 ? PUNIT_PWGT_IDX_DPIO_CMN_BC :
							  PUNIT_PWGT_IDX_DPIO_CMN_D, 1);

	/* the PHY's power good signal */
	if (lg_wait(d, DISPLAY_PHY_STATUS, PHY_POWERGOOD(phy), PHY_POWERGOOD(phy), 1000)) {
		kprintf("[drm] i915: display PHY %d is not powered up\n", phy);
		ret = -ETIMEDOUT;
	}

	/* dynamic power down on */
	tmp = lg_vlv_dpio_read(d, phy, CHV_CMN_DW28);
	tmp |= DPIO_DYNPWRDOWNEN_CH0 | DPIO_CL1POWERDOWNEN | DPIO_SUS_CLK_CONFIG_GATE_CLKREQ;
	lg_vlv_dpio_write(d, phy, CHV_CMN_DW28, tmp);

	if (phy == VLV_DPIO_PHY0) {
		tmp = lg_vlv_dpio_read(d, phy, CHV_CMN_DW6_CH1);
		tmp |= DPIO_DYNPWRDOWNEN_CH1;
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW6_CH1, tmp);
	} else {
		/* Force the CL2 that PHY1 does not have off; it may still
		 * save some power. */
		tmp = lg_vlv_dpio_read(d, phy, CHV_CMN_DW30);
		tmp |= DPIO_CL2_LDOFUSE_PWRENB;
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW30, tmp);
	}

	chv_phy_control |= PHY_COM_LANE_RESET_DEASSERT(phy);
	lg_wr(d, DISPLAY_PHY_CONTROL, chv_phy_control);

	i915_dbg("[drm] i915: enabled DPIO PHY%d (PHY_CONTROL=0x%08x)\n", phy, chv_phy_control);

	chv_phy_status_wait(d);
	return ret;
}

static void chv_dpio_cmn_power_well_disable(struct lg_display *d, int phy)
{
	chv_phy_control &= ~PHY_COM_LANE_RESET_DEASSERT(phy);
	lg_wr(d, DISPLAY_PHY_CONTROL, chv_phy_control);

	vlv_set_power_well(d, phy == VLV_DPIO_PHY0 ? PUNIT_PWGT_IDX_DPIO_CMN_BC :
						    PUNIT_PWGT_IDX_DPIO_CMN_D, 0);

	i915_dbg("[drm] i915: disabled DPIO PHY%d (PHY_CONTROL=0x%08x)\n", phy, chv_phy_control);

	/* fully reset now: its status can be checked from here on */
	chv_phy_check[phy] = 1;

	chv_phy_status_wait(d);
}

int lg_vlv_power_init(struct lg_display *d)
{
	int ret = 0;

	if (!d->is_vlv && !d->is_chv)
		return -ENODEV;

	if (d->is_chv) {
		chv_phy_control_init(d);

		if (chv_set_pipe_power_well(d, 1))
			ret = -ETIMEDOUT;
		vlv_display_power_well_init(d);

		if (chv_dpio_cmn_power_well_enable(d, VLV_DPIO_PHY0))
			ret = -ETIMEDOUT;
		if (chv_dpio_cmn_power_well_enable(d, VLV_DPIO_PHY1))
			ret = -ETIMEDOUT;

		lg_wr(d, DISPLAY_PHY_CONTROL, chv_phy_control);
	} else {
		vlv_cmnlane_wa(d);

		if (vlv_set_power_well(d, PUNIT_PWGT_IDX_DISP2D, 1))
			ret = -ETIMEDOUT;
		vlv_display_power_well_init(d);

		if (vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_B_LANES_01, 1) ||
		    vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_B_LANES_23, 1) ||
		    vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_C_LANES_01, 1) ||
		    vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_C_LANES_23, 1))
			ret = -ETIMEDOUT;

		if (vlv_dpio_cmn_power_well_enable(d))
			ret = -ETIMEDOUT;
	}

	if (ret)
		kprintf("[drm] i915: %s display power wells did not all come up\n",
			d->is_chv ? "Cherryview" : "Valleyview");
	else
		i915_dbg("[drm] i915: display power wells and DPIO PHYs up\n");
	return ret;
}

void lg_vlv_power_fini(struct lg_display *d)
{
	int pipe;

	if (!d->is_vlv && !d->is_chv)
		return;

	/* The common lanes carry the PLLs: leave everything on while one runs. */
	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		if (lg_rd(d, DPLL(d, pipe)) & DPLL_VCO_ENABLE) {
			i915_dbg("[drm] i915: DPLL %d still on, display power left up\n", pipe);
			return;
		}
	}

	if (d->is_chv) {
		if (d->num_pipes > VLV_PIPE_C)
			chv_dpio_cmn_power_well_disable(d, VLV_DPIO_PHY1);
		chv_dpio_cmn_power_well_disable(d, VLV_DPIO_PHY0);
		chv_set_pipe_power_well(d, 0);
	} else {
		vlv_dpio_cmn_power_well_disable(d);
		vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_C_LANES_23, 0);
		vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_C_LANES_01, 0);
		vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_B_LANES_23, 0);
		vlv_set_power_well(d, PUNIT_PWGT_IDX_DPIO_TX_B_LANES_01, 0);
		vlv_set_power_well(d, PUNIT_PWGT_IDX_DISP2D, 0);
	}
}

/* ---- Cherryview lane power gating ---------------------------------------------- */

/* Force a channel's lanes on through the override (or release it);
 * returns whether the override was on before. */
static int chv_phy_powergate_ch(struct lg_display *d, int phy, int ch, int override)
{
	int was_override = !!(chv_phy_control & PHY_CH_POWER_DOWN_OVRD_EN(phy, ch));

	if (!!override == was_override)
		return was_override;

	if (override)
		chv_phy_control |= PHY_CH_POWER_DOWN_OVRD_EN(phy, ch);
	else
		chv_phy_control &= ~PHY_CH_POWER_DOWN_OVRD_EN(phy, ch);

	lg_wr(d, DISPLAY_PHY_CONTROL, chv_phy_control);

	i915_dbg("[drm] i915: power gating DPIO PHY%d CH%d (PHY_CONTROL=0x%08x)\n", phy, ch,
		 chv_phy_control);

	chv_phy_status_wait(d);
	return was_override;
}

/*
 * `mask' is the channel's lane power-down override field: a set bit powers
 * that lane down while the override is on (a 2-lane link passes 0xc, a
 * 4-lane one 0x0); with override off the lanes follow the port, and 0x0
 * leaves the power-down bits cleared, as after a port disable.
 */
void lg_chv_phy_powergate_lanes(struct lg_display *d, struct lg_output *o, int override,
				unsigned int mask)
{
	int phy = lg_vlv_port_to_phy(d, o->port);
	int ch = lg_vlv_port_to_channel(o->port);

	chv_phy_control &= ~PHY_CH_POWER_DOWN_OVRD(0xf, phy, ch);
	chv_phy_control |= PHY_CH_POWER_DOWN_OVRD(mask & 0xf, phy, ch);

	if (override)
		chv_phy_control |= PHY_CH_POWER_DOWN_OVRD_EN(phy, ch);
	else
		chv_phy_control &= ~PHY_CH_POWER_DOWN_OVRD_EN(phy, ch);

	lg_wr(d, DISPLAY_PHY_CONTROL, chv_phy_control);

	i915_dbg("[drm] i915: power gating DPIO PHY%d CH%d lanes 0x%x (PHY_CONTROL=0x%08x)\n",
		 phy, ch, mask, chv_phy_control);

	chv_phy_status_wait(d);
}

/* ---- the pipe PLLs -------------------------------------------------------------- */

static int vlv_cfg_has_dp(const struct lg_config *cfg)
{
	return cfg->has_dp_encoder || lg_cfg_has(cfg, LG_OUTPUT_DP) ||
	       lg_cfg_has(cfg, LG_OUTPUT_EDP);
}

/* PLL B's opamp always calibrates to its maximum of 0x3f: force it on with
 * a reasonable value instead. */
static void vlv_pllb_recal_opamp(struct lg_display *d, int phy, int ch)
{
	uint32_t tmp;

	tmp = lg_vlv_dpio_read(d, phy, VLV_PLL_DW17(ch));
	tmp &= 0xffffff00u;
	tmp |= 0x00000030u;
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW17(ch), tmp);

	tmp = lg_vlv_dpio_read(d, phy, VLV_REF_DW11);
	tmp &= 0x00ffffffu;
	tmp |= 0x8c000000u;
	lg_vlv_dpio_write(d, phy, VLV_REF_DW11, tmp);

	tmp = lg_vlv_dpio_read(d, phy, VLV_PLL_DW17(ch));
	tmp &= 0xffffff00u;
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW17(ch), tmp);

	tmp = lg_vlv_dpio_read(d, phy, VLV_REF_DW11);
	tmp &= 0x00ffffffu;
	tmp |= 0xb0000000u;
	lg_vlv_dpio_write(d, phy, VLV_REF_DW11, tmp);
}

static void vlv_prepare_pll(struct lg_display *d, const struct lg_config *cfg)
{
	const struct lg_dpll *clock = &cfg->dpll;
	int pipe = cfg->pipe;
	int ch = lg_vlv_pipe_to_channel(pipe);
	int phy = lg_vlv_pipe_to_phy(d, pipe);
	uint32_t tmp, coreclk;

	/* PLL B needs special handling */
	if (pipe == VLV_PIPE_B)
		vlv_pllb_recal_opamp(d, phy, ch);

	/* Tx target for the periodic Rcomp update */
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW17_BCAST, 0x0100000fu);

	/* target IRef off on the PLL */
	tmp = lg_vlv_dpio_read(d, phy, VLV_PLL_DW16(ch));
	tmp &= 0x00ffffffu;
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW16(ch), tmp);

	/* fast lock off */
	lg_vlv_dpio_write(d, phy, VLV_CMN_DW0, 0x610);

	/* idtafcrecal before the PLL is enabled */
	tmp = DPIO_M1_DIV(clock->m1) | DPIO_M2_DIV(clock->m2) | DPIO_P1_DIV(clock->p1) |
	      DPIO_P2_DIV(clock->p2) | DPIO_N_DIV(clock->n) | DPIO_K_DIV(1);

	/* The post divider for digital outputs (the DAC one is unstable). */
	tmp |= DPIO_S1_DIV(DPIO_S1_DIV_HDMIDP);
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW3(ch), tmp);

	tmp |= DPIO_ENABLE_CALIBRATION;
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW3(ch), tmp);

	/* HBR and RBR LPF coefficients */
	if (cfg->port_clock == 162000 || lg_cfg_has(cfg, LG_OUTPUT_ANALOG) ||
	    lg_cfg_has(cfg, LG_OUTPUT_HDMI))
		lg_vlv_dpio_write(d, phy, VLV_PLL_DW18(ch), 0x009f0003u);
	else
		lg_vlv_dpio_write(d, phy, VLV_PLL_DW18(ch), 0x00d0000fu);

	if (vlv_cfg_has_dp(cfg)) {
		/* the SSC source */
		if (pipe == VLV_PIPE_A)
			lg_vlv_dpio_write(d, phy, VLV_PLL_DW5(ch), 0x0df40000u);
		else
			lg_vlv_dpio_write(d, phy, VLV_PLL_DW5(ch), 0x0df70000u);
	} else {
		/* HDMI or VGA: the bend source */
		if (pipe == VLV_PIPE_A)
			lg_vlv_dpio_write(d, phy, VLV_PLL_DW5(ch), 0x0df70000u);
		else
			lg_vlv_dpio_write(d, phy, VLV_PLL_DW5(ch), 0x0df40000u);
	}

	coreclk = lg_vlv_dpio_read(d, phy, VLV_PLL_DW7(ch));
	coreclk = (coreclk & 0x0000ff00u) | 0x01c00000u;
	if (vlv_cfg_has_dp(cfg))
		coreclk |= 0x01000000u;
	lg_vlv_dpio_write(d, phy, VLV_PLL_DW7(ch), coreclk);

	lg_vlv_dpio_write(d, phy, VLV_PLL_DW19(ch), 0x87871000u);
}

void lg_vlv_enable_pll(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	/* refclk on first, VCO still off */
	lg_wr(d, DPLL(d, pipe), cfg->dpll_hw & ~(DPLL_VCO_ENABLE | DPLL_EXT_BUFFER_ENABLE_VLV));

	if (cfg->dpll_hw & DPLL_VCO_ENABLE) {
		vlv_prepare_pll(d, cfg);

		lg_wr(d, DPLL(d, pipe), cfg->dpll_hw);
		lg_posting_read(d, DPLL(d, pipe));
		lg_udelay(150);

		if (lg_wait(d, DPLL(d, pipe), DPLL_LOCK_VLV, DPLL_LOCK_VLV, 1000))
			kprintf("[drm] i915: DPLL %d failed to lock\n", pipe);
	}

	lg_wr(d, DPLL_MD(d, pipe), cfg->dpll_md);
	lg_posting_read(d, DPLL_MD(d, pipe));
}

static void chv_prepare_pll(struct lg_display *d, const struct lg_config *cfg)
{
	const struct lg_dpll *clock = &cfg->dpll;
	int ch = lg_vlv_pipe_to_channel(cfg->pipe);
	int phy = lg_vlv_pipe_to_phy(d, cfg->pipe);
	uint32_t tmp, loopfilter, tribuf_calcntr;
	uint32_t m2 = (uint32_t)clock->m2;
	uint32_t m2_frac = m2 & 0x3fffffu;

	/* p1 and p2 dividers */
	lg_vlv_dpio_write(d, phy, CHV_CMN_DW13(ch),
			  DPIO_CHV_S1_DIV(5) | DPIO_CHV_P1_DIV(clock->p1) |
				  DPIO_CHV_P2_DIV(clock->p2) | DPIO_CHV_K_DIV(1));

	/* feedback post divider: m2 */
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW0(ch), DPIO_CHV_M2_DIV(m2 >> 22));

	/* feedback reference divider: n and m1 */
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW1(ch),
			  DPIO_CHV_M1_DIV(DPIO_CHV_M1_DIV_BY_2) | DPIO_CHV_N_DIV(1));

	/* m2 fraction */
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW2(ch), DPIO_CHV_M2_FRAC_DIV(m2_frac));

	/* m2 fraction enable */
	tmp = lg_vlv_dpio_read(d, phy, CHV_PLL_DW3(ch));
	tmp &= ~(DPIO_CHV_FEEDFWD_GAIN_MASK | DPIO_CHV_FRAC_DIV_EN);
	tmp |= DPIO_CHV_FEEDFWD_GAIN(2);
	if (m2_frac)
		tmp |= DPIO_CHV_FRAC_DIV_EN;
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW3(ch), tmp);

	/* digital lock detect threshold */
	tmp = lg_vlv_dpio_read(d, phy, CHV_PLL_DW9(ch));
	tmp &= ~(DPIO_CHV_INT_LOCK_THRESHOLD_MASK | DPIO_CHV_INT_LOCK_THRESHOLD_SEL_COARSE);
	tmp |= DPIO_CHV_INT_LOCK_THRESHOLD(0x5);
	if (!m2_frac)
		tmp |= DPIO_CHV_INT_LOCK_THRESHOLD_SEL_COARSE;
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW9(ch), tmp);

	/* loop filter */
	if (clock->vco == 5400000) {
		loopfilter = DPIO_CHV_PROP_COEFF(0x3) | DPIO_CHV_INT_COEFF(0x8) |
			     DPIO_CHV_GAIN_CTRL(0x1);
		tribuf_calcntr = 0x9;
	} else if (clock->vco <= 6200000) {
		loopfilter = DPIO_CHV_PROP_COEFF(0x5) | DPIO_CHV_INT_COEFF(0xB) |
			     DPIO_CHV_GAIN_CTRL(0x3);
		tribuf_calcntr = 0x9;
	} else if (clock->vco <= 6480000) {
		loopfilter = DPIO_CHV_PROP_COEFF(0x4) | DPIO_CHV_INT_COEFF(0x9) |
			     DPIO_CHV_GAIN_CTRL(0x3);
		tribuf_calcntr = 0x8;
	} else {
		/* not supported: the limits of the highest case */
		loopfilter = DPIO_CHV_PROP_COEFF(0x4) | DPIO_CHV_INT_COEFF(0x9) |
			     DPIO_CHV_GAIN_CTRL(0x3);
		tribuf_calcntr = 0;
	}
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW6(ch), loopfilter);

	tmp = lg_vlv_dpio_read(d, phy, CHV_PLL_DW8(ch));
	tmp &= ~DPIO_CHV_TDC_TARGET_CNT_MASK;
	tmp |= DPIO_CHV_TDC_TARGET_CNT(tribuf_calcntr);
	lg_vlv_dpio_write(d, phy, CHV_PLL_DW8(ch), tmp);

	/* AFC recalibration */
	lg_vlv_dpio_write(d, phy, CHV_CMN_DW14(ch),
			  lg_vlv_dpio_read(d, phy, CHV_CMN_DW14(ch)) | DPIO_AFC_RECAL);
}

void lg_chv_enable_pll(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	int ch = lg_vlv_pipe_to_channel(pipe);
	int phy = lg_vlv_pipe_to_phy(d, pipe);
	uint32_t tmp;

	/* refclk and SSC on first, VCO still off */
	lg_wr(d, DPLL(d, pipe), cfg->dpll_hw & ~DPLL_VCO_ENABLE);

	if (cfg->dpll_hw & DPLL_VCO_ENABLE) {
		chv_prepare_pll(d, cfg);

		/* the 10 bit clock to the display controller back on */
		tmp = lg_vlv_dpio_read(d, phy, CHV_CMN_DW14(ch));
		tmp |= DPIO_DCLKP_EN;
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW14(ch), tmp);

		/* >100 ns between the dclkp enable and the PLL enable */
		lg_udelay(1);

		lg_wr(d, DPLL(d, pipe), cfg->dpll_hw);

		if (lg_wait(d, DPLL(d, pipe), DPLL_LOCK_VLV, DPLL_LOCK_VLV, 1000))
			kprintf("[drm] i915: PLL %d failed to lock\n", pipe);
	}

	if (pipe != VLV_PIPE_A) {
		/*
		 * The DPLL_MD of pipes B and C is missing: chicken bits
		 * propagate the value written to DPLLB_MD to either pipe.
		 */
		lg_wr(d, CBR4_VLV, CBR_DPLLBMD_PIPE(pipe));
		lg_wr(d, DPLL_MD(d, VLV_PIPE_B), cfg->dpll_md);
		lg_wr(d, CBR4_VLV, 0);

		/* DPLL B's VGA mode causes problems too; it must stay off. */
		if ((lg_rd(d, DPLL(d, VLV_PIPE_B)) & DPLL_VGA_MODE_DIS) == 0)
			i915_dbg("[drm] i915: DPLL B VGA mode is on\n");
	} else {
		lg_wr(d, DPLL_MD(d, pipe), cfg->dpll_md);
		lg_posting_read(d, DPLL_MD(d, pipe));
	}
}

void lg_vlv_disable_pll(struct lg_display *d, int pipe)
{
	uint32_t val;

	val = DPLL_INTEGRATED_REF_CLK_VLV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS;
	if (pipe != VLV_PIPE_A)
		val |= DPLL_INTEGRATED_CRI_CLK_VLV;

	lg_wr(d, DPLL(d, pipe), val);
	lg_posting_read(d, DPLL(d, pipe));
}

void lg_chv_disable_pll(struct lg_display *d, int pipe)
{
	int ch = lg_vlv_pipe_to_channel(pipe);
	int phy = lg_vlv_pipe_to_phy(d, pipe);
	uint32_t val;

	val = DPLL_SSC_REF_CLK_CHV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS;
	if (pipe != VLV_PIPE_A)
		val |= DPLL_INTEGRATED_CRI_CLK_VLV;

	lg_wr(d, DPLL(d, pipe), val);
	lg_posting_read(d, DPLL(d, pipe));

	/* the 10 bit clock to the display controller off */
	val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW14(ch));
	val &= ~DPIO_DCLKP_EN;
	lg_vlv_dpio_write(d, phy, CHV_CMN_DW14(ch), val);
}

/*
 * Run a pipe's PLL with no pipe behind it, for eDP divider settings (the
 * VLV/CHV panel power sequencer locks onto a port only while the pipe's
 * PLL runs), and stop it again.
 */
int lg_vlv_force_pll_on(struct lg_display *d, int pipe, const struct lg_dpll *dpll)
{
	struct lg_config cfg;

	mm_memset(&cfg, 0, sizeof(cfg));
	cfg.pipe = pipe;
	cfg.output = -1;
	cfg.pch_dpll = -1;
	cfg.pixel_multiplier = 1;
	cfg.dpll = *dpll;
	cfg.clock_set = 1;
	cfg.output_types = 1u << LG_OUTPUT_EDP;
	cfg.has_dp_encoder = 1;

	lg_dpll_compute_regs(d, &cfg);

	if (d->is_chv)
		lg_chv_enable_pll(d, &cfg);
	else
		lg_vlv_enable_pll(d, &cfg);

	return 0;
}

void lg_vlv_force_pll_off(struct lg_display *d, int pipe)
{
	if (d->is_chv)
		lg_chv_disable_pll(d, pipe);
	else
		lg_vlv_disable_pll(d, pipe);
}

/* ---- the port lanes: Valleyview ------------------------------------------------- */

/* The lanes a port runs: the DP link width, all four for HDMI. */
static int vlv_cfg_lanes(const struct lg_config *cfg)
{
	return cfg->lane_count > 0 ? cfg->lane_count : 4;
}

void lg_vlv_set_phy_signal_level(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, uint32_t demph_reg_value,
				 uint32_t preemph_reg_value, uint32_t uniqtranscale_reg_value,
				 uint32_t tx3_demph)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);

	(void)cfg;

	lg_vlv_dpio_write(d, phy, VLV_TX_DW5_GRP(ch), 0x00000000u);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW4_GRP(ch), demph_reg_value);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW2_GRP(ch), uniqtranscale_reg_value);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW3_GRP(ch), 0x0C782040u);

	if (tx3_demph)
		lg_vlv_dpio_write(d, phy, VLV_TX_DW4(ch, 3), tx3_demph);

	lg_vlv_dpio_write(d, phy, VLV_PCS_DW11_GRP(ch), 0x00030000u);
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW9_GRP(ch), preemph_reg_value);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW5_GRP(ch), DPIO_TX_OCALINIT_EN);
}

void lg_vlv_phy_pre_pll_enable(struct lg_display *d, struct lg_output *o,
			       const struct lg_config *cfg)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);

	(void)cfg;

	/* Tx lane resets to their defaults */
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW0_GRP(ch),
			  DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET);
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW1_GRP(ch),
			  DPIO_PCS_CLK_CRI_RXEB_EIOS_EN | DPIO_PCS_CLK_CRI_RXDIGFILTSG_EN |
				  DPIO_PCS_CLK_DATAWIDTH_8_10 | DPIO_PCS_CLK_SOFT_RESET);

	/* fix up the inter-pair skew failure */
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW12_GRP(ch), 0x00750f00u);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW11_GRP(ch), 0x00001500u);
	lg_vlv_dpio_write(d, phy, VLV_TX_DW14_GRP(ch), 0x40400000u);
}

void lg_vlv_phy_pre_encoder_enable(struct lg_display *d, struct lg_output *o,
				   const struct lg_config *cfg)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);
	uint32_t val;

	/* the clock channels of this port */
	val = DPIO_PCS_USEDCLKCHANNEL_OVRRIDE;
	if (cfg->pipe == VLV_PIPE_B)
		val |= DPIO_PCS_USEDCLKCHANNEL;
	val |= 0xc4;
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW8_GRP(ch), val);

	/* the lane clock */
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW14_GRP(ch), 0x00760018u);
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW23_GRP(ch), 0x00400888u);
}

void lg_vlv_phy_reset_lanes(struct lg_display *d, struct lg_output *o,
			    const struct lg_config *cfg)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);

	(void)cfg;

	lg_vlv_dpio_write(d, phy, VLV_PCS_DW0_GRP(ch), 0x00000000u);
	lg_vlv_dpio_write(d, phy, VLV_PCS_DW1_GRP(ch), 0x00e00060u);
}

/* ---- the port lanes: Cherryview ------------------------------------------------- */

void lg_chv_set_phy_signal_level(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, uint32_t deemph_reg_value,
				 uint32_t margin_reg_value, int uniq_trans_scale)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);
	int lanes = vlv_cfg_lanes(cfg);
	uint32_t val;
	int i;

	/* clear calc init */
	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW10(ch));
	val &= ~(DPIO_PCS_SWING_CALC_TX0_TX2 | DPIO_PCS_SWING_CALC_TX1_TX3);
	val &= ~(DPIO_PCS_TX1DEEMP_MASK | DPIO_PCS_TX2DEEMP_MASK);
	val |= DPIO_PCS_TX1DEEMP_9P5 | DPIO_PCS_TX2DEEMP_9P5;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW10(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW10(ch));
		val &= ~(DPIO_PCS_SWING_CALC_TX0_TX2 | DPIO_PCS_SWING_CALC_TX1_TX3);
		val &= ~(DPIO_PCS_TX1DEEMP_MASK | DPIO_PCS_TX2DEEMP_MASK);
		val |= DPIO_PCS_TX1DEEMP_9P5 | DPIO_PCS_TX2DEEMP_9P5;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW10(ch), val);
	}

	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW9(ch));
	val &= ~(DPIO_PCS_TX1MARGIN_MASK | DPIO_PCS_TX2MARGIN_MASK);
	val |= DPIO_PCS_TX1MARGIN_000 | DPIO_PCS_TX2MARGIN_000;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW9(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW9(ch));
		val &= ~(DPIO_PCS_TX1MARGIN_MASK | DPIO_PCS_TX2MARGIN_MASK);
		val |= DPIO_PCS_TX1MARGIN_000 | DPIO_PCS_TX2MARGIN_000;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW9(ch), val);
	}

	/* swing de-emphasis */
	for (i = 0; i < lanes; i++) {
		val = lg_vlv_dpio_read(d, phy, CHV_TX_DW4(ch, i));
		val &= ~DPIO_SWING_DEEMPH9P5_MASK;
		val |= DPIO_SWING_DEEMPH9P5(deemph_reg_value);
		lg_vlv_dpio_write(d, phy, CHV_TX_DW4(ch, i), val);
	}

	/* swing margin */
	for (i = 0; i < lanes; i++) {
		val = lg_vlv_dpio_read(d, phy, CHV_TX_DW2(ch, i));

		val &= ~DPIO_SWING_MARGIN000_MASK;
		val |= DPIO_SWING_MARGIN000(margin_reg_value);

		/*
		 * The unique transition scale value matters even with the
		 * scale disabled; always the same value.
		 */
		val &= ~DPIO_UNIQ_TRANS_SCALE_MASK;
		val |= DPIO_UNIQ_TRANS_SCALE(0x9a);

		lg_vlv_dpio_write(d, phy, CHV_TX_DW2(ch, i), val);
	}

	/*
	 * The unique transition scale select: bit 27 for both channels (the
	 * documentation names bit 26 for channel 1, likely a typo).
	 */
	for (i = 0; i < lanes; i++) {
		val = lg_vlv_dpio_read(d, phy, CHV_TX_DW3(ch, i));
		if (uniq_trans_scale)
			val |= DPIO_TX_UNIQ_TRANS_SCALE_EN;
		else
			val &= ~DPIO_TX_UNIQ_TRANS_SCALE_EN;
		lg_vlv_dpio_write(d, phy, CHV_TX_DW3(ch, i), val);
	}

	/* start the swing calculation */
	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW10(ch));
	val |= DPIO_PCS_SWING_CALC_TX0_TX2 | DPIO_PCS_SWING_CALC_TX1_TX3;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW10(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW10(ch));
		val |= DPIO_PCS_SWING_CALC_TX0_TX2 | DPIO_PCS_SWING_CALC_TX1_TX3;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW10(ch), val);
	}
}

static void chv_data_lane_soft_reset(struct lg_display *d, int phy, int ch, int lanes,
				     int reset)
{
	uint32_t val;

	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW0(ch));
	if (reset)
		val &= ~(DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET);
	else
		val |= DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW0(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW0(ch));
		if (reset)
			val &= ~(DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET);
		else
			val |= DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW0(ch), val);
	}

	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW1(ch));
	val |= CHV_PCS_REQ_SOFTRESET_EN;
	if (reset)
		val &= ~DPIO_PCS_CLK_SOFT_RESET;
	else
		val |= DPIO_PCS_CLK_SOFT_RESET;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW1(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW1(ch));
		val |= CHV_PCS_REQ_SOFTRESET_EN;
		if (reset)
			val &= ~DPIO_PCS_CLK_SOFT_RESET;
		else
			val |= DPIO_PCS_CLK_SOFT_RESET;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW1(ch), val);
	}
}

void lg_chv_data_lane_soft_reset(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, int reset)
{
	chv_data_lane_soft_reset(d, lg_vlv_port_to_phy(d, o->port),
				 lg_vlv_port_to_channel(o->port), vlv_cfg_lanes(cfg), reset);
}

void lg_chv_phy_pre_pll_enable(struct lg_display *d, struct lg_output *o,
			       const struct lg_config *cfg)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);
	int pipe = cfg->pipe;
	int lanes = vlv_cfg_lanes(cfg);
	unsigned int lane_mask = ~((1u << lanes) - 1) & 0xfu; /* the unused lanes */
	uint32_t val;

	/* The second common lane must be tricked into life, or the PLL of
	 * pipe B cannot even be reached. */
	if (ch == VLV_DPIO_CH0 && pipe == VLV_PIPE_B && o->port >= 0 && o->port <= LG_PORT_D)
		chv_release_cl2_override[o->port] =
			!chv_phy_powergate_ch(d, VLV_DPIO_PHY0, VLV_DPIO_CH1, 1);

	lg_chv_phy_powergate_lanes(d, o, 1, lane_mask);

	/* data lane reset asserted */
	chv_data_lane_soft_reset(d, phy, ch, lanes, 1);

	/* left/right clock distribution */
	if (pipe != VLV_PIPE_B) {
		val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW5_CH0);
		val &= ~(CHV_BUFLEFTENA1_MASK | CHV_BUFRIGHTENA1_MASK);
		if (ch == VLV_DPIO_CH0)
			val |= CHV_BUFLEFTENA1_FORCE;
		if (ch == VLV_DPIO_CH1)
			val |= CHV_BUFRIGHTENA1_FORCE;
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW5_CH0, val);
	} else {
		val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW1_CH1);
		val &= ~(CHV_BUFLEFTENA2_MASK | CHV_BUFRIGHTENA2_MASK);
		if (ch == VLV_DPIO_CH0)
			val |= CHV_BUFLEFTENA2_FORCE;
		if (ch == VLV_DPIO_CH1)
			val |= CHV_BUFRIGHTENA2_FORCE;
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW1_CH1, val);
	}

	/* clock channel usage */
	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW8(ch));
	val |= DPIO_PCS_USEDCLKCHANNEL_OVRRIDE;
	if (pipe == VLV_PIPE_B)
		val |= DPIO_PCS_USEDCLKCHANNEL;
	else
		val &= ~DPIO_PCS_USEDCLKCHANNEL;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW8(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW8(ch));
		val |= DPIO_PCS_USEDCLKCHANNEL_OVRRIDE;
		if (pipe == VLV_PIPE_B)
			val |= DPIO_PCS_USEDCLKCHANNEL;
		else
			val &= ~DPIO_PCS_USEDCLKCHANNEL;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW8(ch), val);
	}

	/* The common lane is picked by the port here, not by the pipe. */
	val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW19(ch));
	if (pipe == VLV_PIPE_B)
		val |= CHV_CMN_USEDCLKCHANNEL;
	else
		val &= ~CHV_CMN_USEDCLKCHANNEL;
	lg_vlv_dpio_write(d, phy, CHV_CMN_DW19(ch), val);
}

void lg_chv_phy_pre_encoder_enable(struct lg_display *d, struct lg_output *o,
				   const struct lg_config *cfg)
{
	int ch = lg_vlv_port_to_channel(o->port);
	int phy = lg_vlv_port_to_phy(d, o->port);
	int lanes = vlv_cfg_lanes(cfg);
	uint32_t data, stagger, val;
	int i;

	/* the hardware manages the TX FIFO reset source */
	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW11(ch));
	val &= ~DPIO_LANEDESKEW_STRAP_OVRD;
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW11(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW11(ch));
		val &= ~DPIO_LANEDESKEW_STRAP_OVRD;
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW11(ch), val);
	}

	/* Tx lane latency: the upar bit */
	for (i = 0; i < lanes; i++) {
		if (lanes == 1)
			data = 0;
		else
			data = (i == 1) ? 0 : DPIO_UPAR;
		lg_vlv_dpio_write(d, phy, CHV_TX_DW14(ch, i), data);
	}

	/* data lane stagger */
	if (cfg->port_clock > 270000)
		stagger = 0x18;
	else if (cfg->port_clock > 135000)
		stagger = 0xd;
	else if (cfg->port_clock > 67500)
		stagger = 0x7;
	else if (cfg->port_clock > 33750)
		stagger = 0x4;
	else
		stagger = 0x2;

	val = lg_vlv_dpio_read(d, phy, VLV_PCS01_DW11(ch));
	val |= DPIO_TX2_STAGGER_MASK(0x1f);
	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW11(ch), val);

	if (lanes > 2) {
		val = lg_vlv_dpio_read(d, phy, VLV_PCS23_DW11(ch));
		val |= DPIO_TX2_STAGGER_MASK(0x1f);
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW11(ch), val);
	}

	lg_vlv_dpio_write(d, phy, VLV_PCS01_DW12(ch),
			  DPIO_LANESTAGGER_STRAP(stagger) | DPIO_LANESTAGGER_STRAP_OVRD |
				  DPIO_TX1_STAGGER_MASK(0x1f) | DPIO_TX1_STAGGER_MULT(6) |
				  DPIO_TX2_STAGGER_MULT(0));

	if (lanes > 2) {
		lg_vlv_dpio_write(d, phy, VLV_PCS23_DW12(ch),
				  DPIO_LANESTAGGER_STRAP(stagger) | DPIO_LANESTAGGER_STRAP_OVRD |
					  DPIO_TX1_STAGGER_MASK(0x1f) | DPIO_TX1_STAGGER_MULT(7) |
					  DPIO_TX2_STAGGER_MULT(5));
	}

	/* data lane reset released */
	chv_data_lane_soft_reset(d, phy, ch, lanes, 0);
}

void lg_chv_phy_release_cl2_override(struct lg_display *d, struct lg_output *o)
{
	if (o->port < 0 || o->port > LG_PORT_D)
		return;

	if (chv_release_cl2_override[o->port]) {
		chv_phy_powergate_ch(d, VLV_DPIO_PHY0, VLV_DPIO_CH1, 0);
		chv_release_cl2_override[o->port] = 0;
	}
}

void lg_chv_phy_post_pll_disable(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg)
{
	int phy = lg_vlv_port_to_phy(d, o->port);
	uint32_t val;

	/* left/right clock distribution off */
	if (cfg->pipe != VLV_PIPE_B) {
		val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW5_CH0);
		val &= ~(CHV_BUFLEFTENA1_MASK | CHV_BUFRIGHTENA1_MASK);
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW5_CH0, val);
	} else {
		val = lg_vlv_dpio_read(d, phy, CHV_CMN_DW1_CH1);
		val &= ~(CHV_BUFLEFTENA2_MASK | CHV_BUFRIGHTENA2_MASK);
		lg_vlv_dpio_write(d, phy, CHV_CMN_DW1_CH1, val);
	}

	/*
	 * Leave the power down bit clear for at least one lane, so that
	 * forcing the channel on powers something when it is otherwise
	 * unused.  With the port off and the override released the lanes
	 * power down anyway.
	 */
	lg_chv_phy_powergate_lanes(d, o, 0, 0x0);
}

/* Wait (up to a second) for the port's lane ready bits: ports B and C
 * report in DPLL A, port D in DPIO_PHY_STATUS. */
void lg_vlv_wait_port_ready(struct lg_display *d, struct lg_output *o,
			    unsigned int expected_mask)
{
	uint32_t port_mask, reg;

	switch (o->port) {
	case LG_PORT_C:
		port_mask = DPLL_PORTC_READY_MASK;
		reg = DPLL(d, VLV_PIPE_A);
		expected_mask <<= 4;
		break;
	case LG_PORT_D:
		port_mask = DPLL_PORTD_READY_MASK;
		reg = DPIO_PHY_STATUS;
		break;
	case LG_PORT_B:
	default:
		port_mask = DPLL_PORTB_READY_MASK;
		reg = DPLL(d, VLV_PIPE_A);
		break;
	}

	if (lg_wait(d, reg, port_mask, expected_mask, 1000000))
		kprintf("[drm] i915: timed out waiting for %s port ready: got 0x%x, expected 0x%x\n",
			o->name, lg_rd(d, reg) & port_mask, expected_mask);
}
