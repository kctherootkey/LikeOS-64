// LikeOS -- registers of Nova Lake's "LT" port PHY (display version 35).
//
// Nova Lake's DDIs carry the LT PHY.  The display still reaches it
// through the per-port PICA block of the C10/C20 PHYs (buffer control,
// clock control, a message bus per PHY lane; intel_cx0_phy_regs.h has
// those), plus two words of its own: a fifth buffer control word that
// holds the MAC clock's reset and default rate, and a second message bus
// status per lane for "PHY to PHY" (P2P) writes, which the PHY forwards
// to its MAC side and acknowledges only once the MAC has taken them.
// Inside the PHY, the 8-bit registers reached over the bus are of three
// kinds: the PIPE-defined transmitter controls (swing, the three cursor
// coefficients, the lane enable), the vendor registers that select the
// protocol and rate and form a window of thirteen 16-bit-address/32-bit-
// data pairs through which the PLL's own registers are written, and the
// rate update strobe that makes the PHY switch to what was written.  The
// PLL register addresses at the end are those window targets; each PLL
// register exists twice, for the PLL below and above 12 GHz.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_LT_PHY_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_LT_PHY_REGS_H

#include <kernel/dev/gpu/i915/intel_cx0_phy_regs.h>

/* ---- latencies of the port sequences --------------------------------------- */

#define XE3PLPD_MSGBUS_TIMEOUT_FAST_US 500
#define XE3PLPD_MACCLK_TURNON_LATENCY_US 2000
#define XE3PLPD_MACCLK_TURNOFF_LATENCY_US 10
#define XE3PLPD_RATE_CALIB_DONE_LATENCY_US 1000
#define XE3PLPD_RESET_START_LATENCY_US 10
#define XE3PLPD_PWRDN_TO_RDY_LATENCY_US 10
#define XE3PLPD_RESET_END_LATENCY_US 2000
/* How long the PHY needs to settle after a P2P write. */
#define XE3PLPD_P2P_SETTLE_US 150

/* ---- the PICA port blocks (MMIO) ------------------------------------------- */

/* Port buffer control 5: the MAC clock's reset and its rate (the default,
 * 0x1f, is the 162 MHz the PHY comes up with). */
#define XE3PLPD_PORT_BUF_CTL5(ver, port) (XELPDP_PORT_BUF_CTL1_REG(ver, port) + 0x34)
#define XE3PLPD_MACCLK_RESET_0 (1u << 11)
#define XE3PLPD_MACCLK_RATE_MASK 0x1fu
#define XE3PLPD_MACCLK_RATE_DEF 0x1fu

/* The P2P message bus status of PHY lane `lane' (0, 1): its response
 * ready flag is cleared by writing it as zero. */
#define XE3PLPD_PORT_P2M_MSGBUS_STATUS_P2P(ver, port, lane)                    \
	(XELPDP_PORT_M2P_MSGBUS_CTL(ver, port, 0) + 0x60 + (lane) * 4)
#define XE3LPD_PORT_P2M_ADDR_MASK 0xfffu

/* ---- inside the PHY: message bus addresses (8-bit registers) ---------------- */

/* The MAC's vendor register a P2P write lands in. */
#define LT_PHY_MAC_VDR 0xC00
#define LT_PHY_PCLKIN_GATE (1u << 0)

/* PIPE-defined transmitter registers, per transmitter `idx' (0, 1) of a
 * PHY lane: the swing, the pre-, main and post-cursor, the lane enable. */
#define LT_PHY_TXY_CTL2(idx) (0x402 + 0x200 * (idx))
#define LT_PHY_TXY_CTL3(idx) (0x403 + 0x200 * (idx))
#define LT_PHY_TXY_CTL4(idx) (0x404 + 0x200 * (idx))
#define LT_PHY_TX_CURSOR_MASK 0x3fu
#define LT_PHY_TX_CURSOR(val) ((uint32_t)(val) & LT_PHY_TX_CURSOR_MASK)

#define LT_PHY_TXY_CTL8(idx) (0x408 + 0x200 * (idx))
#define LT_PHY_TX_SWING_LEVEL_MASK (0xfu << 4)
#define LT_PHY_TX_SWING_LEVEL(val) (((uint32_t)(val) << 4) & LT_PHY_TX_SWING_LEVEL_MASK)
#define LT_PHY_TX_SWING_MASK (1u << 3)
#define LT_PHY_TX_SWING(val) (((uint32_t)(val) << 3) & LT_PHY_TX_SWING_MASK)

#define LT_PHY_TXY_CTL10(idx) (0x40A + 0x200 * (idx))
#define LT_PHY_TX_LANE_ENABLE (1u << 0)

/* Vendor registers: 0 the PLL enable, the encoded rate and protocol
 * ("mode"); 1 the PLL type; 2 set for an embedded panel. */
#define LT_PHY_VDR_0_CONFIG 0xC02
#define LT_PHY_VDR_DP_PLL_ENABLE (1u << 7)
#define LT_PHY_VDR_1_CONFIG 0xC03
#define LT_PHY_VDR_RATE_ENCODING_MASK (0xfu << 3)
#define LT_PHY_VDR_RATE_ENCODING_SHIFT 3
#define LT_PHY_VDR_MODE_ENCODING_MASK 0x7u
#define LT_PHY_VDR_2_CONFIG 0xCC3

/* The thirteen PLL register windows: a 16-bit address (MSB, LSB), then
 * the 32-bit data, byte `y' (3 the most significant) at 0xC06 + 3 - y. */
#define LT_PHY_VDR_X_ADDR_MSB(idx) (0xC04 + 0x6 * (idx))
#define LT_PHY_VDR_X_ADDR_LSB(idx) (0xC05 + 0x6 * (idx))
#define LT_PHY_VDR_X_DATAY(idx, y) ((0xC06 + (3 - (y))) + 0x6 * (idx))

/* The rate update: set, the PHY switches; cleared again afterwards. */
#define LT_PHY_RATE_UPDATE 0xCC4
#define LT_PHY_RATE_CONTROL_VDR_UPDATE (1u << 0)

/* ---- the PLL's registers, written through the windows ---------------------- */

#define PLL_REG4_ADDR 0x8510
#define PLL_REG3_ADDR 0x850C
#define PLL_REG5_ADDR 0x8514
#define PLL_REG57_ADDR 0x85E4
#define PLL_LF_ADDR 0x860C
#define PLL_TDC_ADDR 0x8610
#define PLL_SSC_ADDR 0x8614
#define PLL_BIAS2_ADDR 0x8618
#define PLL_BIAS_TRIM_ADDR 0x8648
#define PLL_DCO_MED_ADDR 0x8640
#define PLL_DCO_FINE_ADDR 0x864C
#define PLL_SSC_INJ_ADDR 0x8624
#define PLL_SURV_BONUS_ADDR 0x8644
/* the second PLL's copy of each register */
#define PLL_TYPE_OFFSET 0x200
#define PLL_REG_ADDR(base, pll_type) ((pll_type) ? (base) + PLL_TYPE_OFFSET : (base))

#endif
