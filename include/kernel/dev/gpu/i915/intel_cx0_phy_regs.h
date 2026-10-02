// LikeOS -- registers of the C10 and C20 port PHYs (Meteor Lake onwards).
//
// From Meteor Lake on, a DDI's PHY is not mapped into the display's MMIO
// window.  The display reaches it through a per-port "PICA" block: the
// port buffer control words (lane power states, resets, width and
// reversal, the die-to-die link), the port clock control (clock muxes,
// spread spectrum, the requests that start a lane's PLL or reference
// clock), and a message bus per PHY lane over which the PHY's own 8-bit
// registers are read and written.  Those PHY-internal addresses are the
// second half of this file: the C10's vendor registers (its PLL words,
// common and transmitter settings, the swing overrides), the PIPE-defined
// transmitter controls, and the C20's indirect window into its SRAM,
// where two complete configuration contexts (A and B) live and the PHY
// switches between them on a toggle.
//
// The port blocks of DDI A and B sit next to the DDI registers; those of
// the Type-C ports start at 0x16F200, 0x200 apart.  Display version 20
// renumbered them into one range that starts with the Type-C ports (A
// and B come after TC4), which is why the macros take the version.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2012-2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_CX0_PHY_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_CX0_PHY_REGS_H

#include <kernel/dev/gpu/i915/i915_reg.h>

/* ---- the PICA port blocks (MMIO) ------------------------------------------- */

/* The port block index: the port itself, or on version 20 and later one
 * range that starts at TC1 with A and B after TC4. */
#define XELPDP_PORT_IDX(ver, port)                                             \
	((ver) >= 20 && (port) < PORT_TC1 ? PORT_TC1 + 4 + (port) : (port))
#define _XELPDP_PICK(idx, a, b, tc1, tc2)                                      \
	((idx) < PORT_TC1 ? (uint32_t)((a) + (idx) * ((b) - (a))) :            \
			    (uint32_t)((tc1) + ((idx) - PORT_TC1) * ((tc2) - (tc1))))
#define _XELPDP_PORT_REG(ver, port, a, b, tc1, tc2)                            \
	_XELPDP_PICK(XELPDP_PORT_IDX(ver, port), a, b, tc1, tc2)

/* The DDI clock the port is validated for, in kHz (the link symbol clock
 * for DisplayPort, the TMDS clock for HDMI); 0 while the port is off. */
#ifndef DDI_CLK_VALFREQ
#define DDI_CLK_VALFREQ(port) (0x64030 + (port) * 0x100)
#endif

/* DDI_BUF_CTL fields of version 14 and later that the older parts do not
 * have (the rest of DDI_BUF_CTL is in i915_reg.h). */
#ifndef XE2LPD_DDI_BUF_D2D_LINK_ENABLE
#define XE2LPD_DDI_BUF_D2D_LINK_ENABLE (1u << 29)
#define XE2LPD_DDI_BUF_D2D_LINK_STATE (1u << 28)
#endif
#ifndef DDI_BUF_PORT_DATA_MASK
#define DDI_BUF_PORT_DATA_MASK (3u << 18)
#define DDI_BUF_PORT_DATA_10BIT (0u << 18)
#define DDI_BUF_PORT_DATA_20BIT (1u << 18)
#define DDI_BUF_PORT_DATA_40BIT (2u << 18)
#endif

/* The message bus, one control/status pair per PHY lane (lane 0 or 1). */
#define XELPDP_PORT_M2P_MSGBUS_CTL(ver, port, lane)                            \
	(_XELPDP_PORT_REG(ver, port, 0x64040, 0x64140, 0x16F240, 0x16F440) + (lane) * 4)
#define XELPDP_PORT_M2P_TRANSACTION_PENDING (1u << 31)
#define XELPDP_PORT_M2P_COMMAND_TYPE_MASK (0xfu << 27)
#define XELPDP_PORT_M2P_COMMAND_WRITE_UNCOMMITTED (0x1u << 27)
#define XELPDP_PORT_M2P_COMMAND_WRITE_COMMITTED (0x2u << 27)
#define XELPDP_PORT_M2P_COMMAND_READ (0x3u << 27)
#define XELPDP_PORT_P2P_TRANSACTION_PENDING (1u << 24)
#define XELPDP_PORT_M2P_DATA_MASK (0xffu << 16)
#define XELPDP_PORT_M2P_DATA(val) (((uint32_t)(val) << 16) & XELPDP_PORT_M2P_DATA_MASK)
#define XELPDP_PORT_M2P_TRANSACTION_RESET (1u << 15)
#define XELPDP_PORT_M2P_ADDRESS_MASK 0xfffu
#define XELPDP_PORT_M2P_ADDRESS(val) ((uint32_t)(val) & XELPDP_PORT_M2P_ADDRESS_MASK)

#define XELPDP_PORT_P2M_MSGBUS_STATUS(ver, port, lane)                         \
	(XELPDP_PORT_M2P_MSGBUS_CTL(ver, port, lane) + 8)
#define XELPDP_PORT_P2M_RESPONSE_READY (1u << 31)
#define XELPDP_PORT_P2M_COMMAND_TYPE_MASK (0xfu << 27)
#define XELPDP_PORT_P2M_COMMAND_TYPE_SHIFT 27
#define XELPDP_PORT_P2M_COMMAND_READ_ACK 0x4
#define XELPDP_PORT_P2M_COMMAND_WRITE_ACK 0x5
#define XELPDP_PORT_P2M_DATA_MASK (0xffu << 16)
#define XELPDP_PORT_P2M_DATA_SHIFT 16
#define XELPDP_PORT_P2M_ERROR_SET (1u << 15)

/* Timeouts of the port sequences. */
#define XELPDP_MSGBUS_TIMEOUT_US 1000
#define XELPDP_PCLK_PLL_ENABLE_TIMEOUT_US 3200
#define XELPDP_PCLK_PLL_DISABLE_TIMEOUT_US 20
#define XELPDP_PORT_BUF_SOC_READY_TIMEOUT_US 100
#define XELPDP_PORT_RESET_START_TIMEOUT_US 10
#define XELPDP_PORT_POWERDOWN_UPDATE_TIMEOUT_US 2000
#define XELPDP_PORT_RESET_END_TIMEOUT_US 15000
#define XELPDP_REFCLK_ENABLE_TIMEOUT_US 10

/* Port buffer control 1: width, data width, reversal, I/O select, the
 * die-to-die link, the PHY's ready and idle status. */
/* (named _REG: other headers have a version-less form of this register) */
#define XELPDP_PORT_BUF_CTL1_REG(ver, port)                                    \
	_XELPDP_PORT_REG(ver, port, 0x64004, 0x64104, 0x16F200, 0x16F400)
#define XELPDP_PORT_BUF_D2D_LINK_ENABLE (1u << 29)
#define XELPDP_PORT_BUF_D2D_LINK_STATE (1u << 28)
#define XELPDP_PORT_BUF_SOC_PHY_READY (1u << 24)
#define XELPDP_PORT_BUF_PORT_DATA_WIDTH_MASK (3u << 18)
#define XELPDP_PORT_BUF_PORT_DATA_10BIT (0u << 18)
#define XELPDP_PORT_BUF_PORT_DATA_20BIT (1u << 18)
#define XELPDP_PORT_BUF_PORT_DATA_40BIT (2u << 18)
#define XELPDP_PORT_REVERSAL (1u << 16)
#define XE3PLPDP_PHY_MODE_MASK (0xfu << 12)
#define XE3PLPDP_PHY_MODE_DP (0x3u << 12)
#define XELPDP_PORT_BUF_IO_SELECT_TBT (1u << 11)
#define XELPDP_PORT_BUF_PHY_IDLE (1u << 7)
#ifndef XELPDP_TC_PHY_OWNERSHIP
#define XELPDP_TC_PHY_OWNERSHIP (1u << 6)
#endif
#ifndef XELPDP_TCSS_POWER_REQUEST
#define XELPDP_TCSS_POWER_REQUEST (1u << 5)
#endif
#ifndef XELPDP_TCSS_POWER_STATE
#define XELPDP_TCSS_POWER_STATE (1u << 4)
#endif
#define XELPDP_PORT_WIDTH_MASK (7u << 1)
#define XELPDP_PORT_WIDTH(width) ((uint32_t)((width) == 3 ? 4 : (width) - 1) << 1)

/* Port buffer control 2: per PHY lane, the pipe reset, its status, and
 * the power-down state change (new state, then the update strobe). */
#define XELPDP_PORT_BUF_CTL2(ver, port) (XELPDP_PORT_BUF_CTL1_REG(ver, port) + 4)
#define XELPDP_LANE_PIPE_RESET(lane) ((lane) ? (1u << 30) : (1u << 31))
#define XELPDP_LANE_PHY_CURRENT_STATUS(lane) ((lane) ? (1u << 28) : (1u << 29))
#define XE3PLPDP_LANE_PHY_PULSE_STATUS(lane) ((lane) ? (1u << 26) : (1u << 27))
#define XELPDP_LANE_POWERDOWN_UPDATE(lane) ((lane) ? (1u << 24) : (1u << 25))
#define XELPDP_LANE_POWERDOWN_NEW_STATE(lane, val)                             \
	((lane) ? ((uint32_t)((val) & 0xf) << 16) : ((uint32_t)((val) & 0xf) << 20))
#define XELPDP_LANE_POWERDOWN_NEW_STATE_MASK 0xfu
#define XELPDP_POWER_STATE_READY_MASK (0xfu << 4)
#define XELPDP_POWER_STATE_READY(val) (((uint32_t)(val) & 0xf) << 4)

/* Port buffer control 3: the active power state, the lane stagger. */
#define XELPDP_PORT_BUF_CTL3(ver, port) (XELPDP_PORT_BUF_CTL1_REG(ver, port) + 8)
#define XELPDP_PLL_LANE_STAGGERING_DELAY_MASK (0xffu << 8)
#define XELPDP_PLL_LANE_STAGGERING_DELAY(val) (((uint32_t)(val) & 0xff) << 8)
#define XELPDP_POWER_STATE_ACTIVE_MASK 0xfu
#define XELPDP_POWER_STATE_ACTIVE(val) ((uint32_t)(val) & 0xf)
/* The power-down states. */
#define XELPDP_P0_STATE_ACTIVE 0x0
#define XELPDP_P2_STATE_READY 0x2
#define XE3PLPD_P4_STATE_DISABLE 0x4
#define XELPDP_P2PG_STATE_DISABLE 0x9
#define XELPDP_P4PG_STATE_DISABLE 0xC
#define XELPDP_P2_STATE_RESET 0x2

/* The message bus timer: how long a PHY lane may take to answer. */
#define XELPDP_PORT_MSGBUS_TIMER(ver, port, lane)                              \
	(_XELPDP_PORT_REG(ver, port, 0x640d8, 0x641d8, 0x16f258, 0x16f458) + (lane) * 4)
#define XELPDP_PORT_MSGBUS_TIMER_TIMED_OUT (1u << 31)
#define XELPDP_PORT_MSGBUS_TIMER_VAL_MASK 0xffffffu
#define XELPDP_PORT_MSGBUS_TIMER_VAL 0xa000u

/* Port clock control: per PHY lane the PCLK PLL and reference clock
 * request/ack pairs; the Thunderbolt clock request/ack; the DDI clock
 * select; forwarding, lane 1 as clock source, spread spectrum. */
#define XELPDP_PORT_CLOCK_CTL(ver, port)                                       \
	_XELPDP_PORT_REG(ver, port, 0x640E0, 0x641E0, 0x16F260, 0x16F460)
#define XELPDP_LANE_PCLK_PLL_REQUEST(lane) (1u << (31 - (lane) * 4))
#define XELPDP_LANE_PCLK_PLL_ACK(lane) (1u << (30 - (lane) * 4))
#define XELPDP_LANE_PCLK_REFCLK_REQUEST(lane) (1u << (29 - (lane) * 4))
#define XELPDP_LANE_PCLK_REFCLK_ACK(lane) (1u << (28 - (lane) * 4))
#define XELPDP_TBT_CLOCK_REQUEST (1u << 19)
#define XELPDP_TBT_CLOCK_ACK (1u << 18)
/* Version 30 widened the select by a bit. */
#define XELPDP_DDI_CLOCK_SELECT_SHIFT 12
#define XELPDP_DDI_CLOCK_SELECT_MASK(ver)                                      \
	((ver) >= 30 ? (0x1fu << 12) : (0xfu << 12))
#define XELPDP_DDI_CLOCK_SELECT_PREP(ver, val)                                 \
	(((uint32_t)(val) << 12) & XELPDP_DDI_CLOCK_SELECT_MASK(ver))
#define XELPDP_DDI_CLOCK_SELECT_GET(ver, val)                                  \
	(((uint32_t)(val) & XELPDP_DDI_CLOCK_SELECT_MASK(ver)) >> 12)
#define XELPDP_DDI_CLOCK_SELECT_NONE 0x0
#define XELPDP_DDI_CLOCK_SELECT_MAXPCLK 0x8
#define XELPDP_DDI_CLOCK_SELECT_DIV18CLK 0x9
#define XELPDP_DDI_CLOCK_SELECT_TBT_162 0xc
#define XELPDP_DDI_CLOCK_SELECT_TBT_270 0xd
#define XELPDP_DDI_CLOCK_SELECT_TBT_540 0xe
#define XELPDP_DDI_CLOCK_SELECT_TBT_810 0xf
#define XELPDP_DDI_CLOCK_SELECT_TBT_312_5 0x18
#define XELPDP_DDI_CLOCK_SELECT_TBT_625 0x19
#define XELPDP_FORWARD_CLOCK_UNGATE (1u << 10)
#define XELPDP_LANE1_PHY_CLOCK_SELECT (1u << 8)
#define XELPDP_SSC_ENABLE_PLLA (1u << 1)
#define XELPDP_SSC_ENABLE_PLLB (1u << 0)

/* The Type-C subsystem's mailbox. */
#ifndef TCSS_DISP_MAILBOX_IN_CMD
#define TCSS_DISP_MAILBOX_IN_CMD 0x161300
#define TCSS_DISP_MAILBOX_IN_CMD_RUN_BUSY (1u << 31)
#define TCSS_DISP_MAILBOX_IN_CMD_CMD_MASK 0xffu
#define TCSS_DISP_MAILBOX_IN_CMD_DATA(val) ((uint32_t)(val) & 0xff)
#define TCSS_DISP_MAILBOX_IN_DATA 0x161304
#endif

/* Version 30: an embedded panel may sit on a Type-C (C20) PHY. */
#define PICA_PHY_CONFIG_CONTROL 0x16FE68
#define EDP_ON_TYPEC (1u << 31)

/* ---- inside the PHY: message bus addresses (8-bit registers) ---------------- */

/* C10 vendor registers: the PLL's twenty words, common and transmitter
 * words, the control word that grants message bus access and commits. */
#define PHY_C10_VDR_PLL(idx) (0xC00 + (idx))
#define C10_PLL0_SSC_EN (1u << 0)
#define C10_PLL0_DIVCLK_EN (1u << 1)
#define C10_PLL0_DIV5CLK_EN (1u << 2)
#define C10_PLL0_WORDDIV2_EN (1u << 3)
#define C10_PLL0_FRACEN (1u << 4)
#define C10_PLL0_PMIX_EN (1u << 5)
#define C10_PLL0_ANA_FREQ_VCO_MASK (3u << 6)
#define C10_PLL1_DIV_MULTIPLIER_MASK 0xffu
#define C10_PLL2_MULTIPLIERL_MASK 0xffu
#define C10_PLL3_MULTIPLIERH_MASK 0xfu
#define C10_PLL8_SSC_UP_SPREAD (1u << 5)
#define C10_PLL9_FRACN_DENL_MASK 0xffu
#define C10_PLL10_FRACN_DENH_MASK 0xffu
#define C10_PLL11_FRACN_QUOT_L_MASK 0xffu
#define C10_PLL12_FRACN_QUOT_H_MASK 0xffu
#define C10_PLL13_FRACN_REM_L_MASK 0xffu
#define C10_PLL14_FRACN_REM_H_MASK 0xffu
#define C10_PLL15_TXCLKDIV_MASK (7u << 0)
#define C10_PLL15_HDMIDIV_MASK (7u << 3)
#define C10_PLL15_PIXELCLKDIV_MASK (3u << 6)
#define C10_PLL16_ANA_CPINT (0x7fu << 0)
#define C10_PLL16_ANA_CPINTGS_L (1u << 7)
#define C10_PLL17_ANA_CPINTGS_H_MASK (0x3fu << 0)
#define C10_PLL17_ANA_CPPROP_L_MASK (3u << 6)
#define C10_PLL18_ANA_CPPROP_H_MASK (0x1fu << 0)
#define C10_PLL18_ANA_CPPROPGS_L_MASK (7u << 5)
#define C10_PLL19_ANA_CPPROPGS_H_MASK (0xfu << 0)
#define C10_PLL19_ANA_V2I_MASK (3u << 4)
/* the PLL words 4..8 carry the spread spectrum (zero without it) */
#define C10_PLL_SSC_REG_START_IDX 4
#define C10_PLL_SSC_REG_COUNT 5

#define PHY_C10_VDR_CMN(idx) (0xC20 + (idx))
#define C10_CMN0_REF_RANGE (1u << 0)
#define C10_CMN0_REF_CLK_MPLLB_DIV (1u << 5)
#define C10_CMN3_TXVBOOST_MASK (7u << 5)
#define C10_CMN3_TXVBOOST(val) (((uint32_t)(val) << 5) & C10_CMN3_TXVBOOST_MASK)
#define PHY_C10_VDR_TX(idx) (0xC30 + (idx))
#define C10_TX0_TX_MPLLB_SEL (1u << 4)
#define C10_TX1_TERMCTL_MASK (7u << 5)
#define C10_TX1_TERMCTL(val) (((uint32_t)(val) << 5) & C10_TX1_TERMCTL_MASK)
#define PHY_C10_VDR_CONTROL(idx) (0xC70 + (idx) - 1)
#define C10_VDR_CTRL_MSGBUS_ACCESS (1u << 2)
#define C10_VDR_CTRL_MASTER_LANE (1u << 1)
#define C10_VDR_CTRL_UPDATE_CFG (1u << 0)
#define PHY_C10_VDR_CUSTOM_WIDTH 0xD02
#define C10_VDR_CUSTOM_WIDTH_MASK 3u
#define C10_VDR_CUSTOM_WIDTH_8_10 0u
#define PHY_C10_VDR_OVRD 0xD71
#define PHY_C10_VDR_OVRD_TX1 (1u << 0)
#define PHY_C10_VDR_OVRD_TX2 (1u << 2)
#define PHY_C10_VDR_PRE_OVRD_TX1 0xD80
#define C10_PHY_OVRD_LEVEL_MASK 0x3fu
#define C10_PHY_OVRD_LEVEL(val) ((uint32_t)(val) & C10_PHY_OVRD_LEVEL_MASK)
/* The swing overrides of transmitter `tx' (0, 1) of PHY lane `lane':
 * control 0 the pre-cursor, 1 the swing, 2 the post-cursor. */
#define PHY_CX0_VDROVRD_CTL(lane, tx, control)                                 \
	(PHY_C10_VDR_PRE_OVRD_TX1 + ((lane) ^ (tx)) * 0x10 + (control))

/* PIPE-defined registers: per transmitter (1, 2) of a PHY lane. */
#define PHY_CX0_TX_CONTROL(tx, control) (0x400 + ((tx) - 1) * 0x200 + (control))
#define CONTROL2_DISABLE_SINGLE_TX (1u << 6)
#define PHY_CMN1_CONTROL(tx, control) (0x800 + ((tx) - 1) * 0x200 + (control))
#define CONTROL0_MAC_TRANSMIT_LFPS (1u << 1)

/* C20: the indirect SRAM window and the vendor rate/width registers. */
#define PHY_C20_WR_ADDRESS_L 0xC02
#define PHY_C20_WR_ADDRESS_H 0xC03
#define PHY_C20_WR_DATA_L 0xC04
#define PHY_C20_WR_DATA_H 0xC05
#define PHY_C20_RD_ADDRESS_L 0xC06
#define PHY_C20_RD_ADDRESS_H 0xC07
#define PHY_C20_RD_DATA_L 0xC08
#define PHY_C20_RD_DATA_H 0xC09
#define PHY_C20_VDR_CUSTOM_SERDES_RATE 0xD00
#define PHY_C20_IS_HDMI_FRL (1u << 7)
#define PHY_C20_IS_DP (1u << 6)
#define PHY_C20_DP_RATE_MASK (0xfu << 1)
#define PHY_C20_DP_RATE(val) (((uint32_t)(val) << 1) & PHY_C20_DP_RATE_MASK)
#define PHY_C20_CONTEXT_TOGGLE (1u << 0)
#define PHY_C20_SERDES_RATE_MASK (PHY_C20_IS_DP | PHY_C20_DP_RATE_MASK | PHY_C20_IS_HDMI_FRL)
#define PHY_C20_VDR_HDMI_RATE 0xD01
#define PHY_C20_HDMI_RATE_MASK 3u
#define PHY_C20_HDMI_RATE(val) ((uint32_t)(val) & PHY_C20_HDMI_RATE_MASK)
#define PHY_C20_VDR_CUSTOM_WIDTH 0xD02
#define PHY_C20_CUSTOM_WIDTH_MASK 3u
#define PHY_C20_CUSTOM_WIDTH(val) ((uint32_t)(val) & PHY_C20_CUSTOM_WIDTH_MASK)

/* C20 SRAM: the two contexts' transmitter, common and MPLL words (word
 * `idx' sits at base - idx).  The discrete Xe2 part (Battlemage) has its
 * contexts elsewhere. */
#define _MTL_C20_A_TX_CNTX_CFG 0xCF2E
#define _MTL_C20_B_TX_CNTX_CFG 0xCF2A
#define _MTL_C20_A_CMN_CNTX_CFG 0xCDAA
#define _MTL_C20_B_CMN_CNTX_CFG 0xCDA5
#define _MTL_C20_A_MPLLA_CFG 0xCCF0
#define _MTL_C20_B_MPLLA_CFG 0xCCE5
#define _MTL_C20_A_MPLLB_CFG 0xCB5A
#define _MTL_C20_B_MPLLB_CFG 0xCB4E
#define _XE2HPD_C20_A_TX_CNTX_CFG 0xCF5E
#define _XE2HPD_C20_B_TX_CNTX_CFG 0xCF5A
#define _XE2HPD_C20_A_CMN_CNTX_CFG 0xCE8E
#define _XE2HPD_C20_B_CMN_CNTX_CFG 0xCE89
#define _XE2HPD_C20_A_MPLLA_CFG 0xCE58
#define _XE2HPD_C20_B_MPLLA_CFG 0xCE4D
#define _XE2HPD_C20_A_MPLLB_CFG 0xCCC2
#define _XE2HPD_C20_B_MPLLB_CFG 0xCCB6

#define PHY_C20_A_TX_CNTX_CFG(xe2hpd, idx)                                     \
	(((xe2hpd) ? _XE2HPD_C20_A_TX_CNTX_CFG : _MTL_C20_A_TX_CNTX_CFG) - (idx))
#define PHY_C20_B_TX_CNTX_CFG(xe2hpd, idx)                                     \
	(((xe2hpd) ? _XE2HPD_C20_B_TX_CNTX_CFG : _MTL_C20_B_TX_CNTX_CFG) - (idx))
#define PHY_C20_A_CMN_CNTX_CFG(xe2hpd, idx)                                    \
	(((xe2hpd) ? _XE2HPD_C20_A_CMN_CNTX_CFG : _MTL_C20_A_CMN_CNTX_CFG) - (idx))
#define PHY_C20_B_CMN_CNTX_CFG(xe2hpd, idx)                                    \
	(((xe2hpd) ? _XE2HPD_C20_B_CMN_CNTX_CFG : _MTL_C20_B_CMN_CNTX_CFG) - (idx))
#define PHY_C20_A_MPLLA_CNTX_CFG(xe2hpd, idx)                                  \
	(((xe2hpd) ? _XE2HPD_C20_A_MPLLA_CFG : _MTL_C20_A_MPLLA_CFG) - (idx))
#define PHY_C20_B_MPLLA_CNTX_CFG(xe2hpd, idx)                                  \
	(((xe2hpd) ? _XE2HPD_C20_B_MPLLA_CFG : _MTL_C20_B_MPLLA_CFG) - (idx))
#define PHY_C20_A_MPLLB_CNTX_CFG(xe2hpd, idx)                                  \
	(((xe2hpd) ? _XE2HPD_C20_A_MPLLB_CFG : _MTL_C20_A_MPLLB_CFG) - (idx))
#define PHY_C20_B_MPLLB_CNTX_CFG(xe2hpd, idx)                                  \
	(((xe2hpd) ? _XE2HPD_C20_B_MPLLB_CFG : _MTL_C20_B_MPLLB_CFG) - (idx))

/* transmitter word 0 and 1 */
#define C20_PHY_TX_RATE 7u
#define C20_PHY_USE_MPLLB (1u << 7)
#define C20_PHY_TX_MISC_MASK 0xffu
#define C20_PHY_TX_MISC(val) ((uint32_t)(val) & C20_PHY_TX_MISC_MASK)
#define C20_PHY_TX_DCC_CAL_RANGE_MASK (0xfu << 8)
#define C20_PHY_TX_DCC_CAL_RANGE(val) (((uint32_t)(val) << 8) & C20_PHY_TX_DCC_CAL_RANGE_MASK)
#define C20_PHY_TX_DCC_BYPASS (1u << 12)
#define C20_PHY_TX_TERM_CTL_MASK (7u << 13)
#define C20_PHY_TX_TERM_CTL(val) (((uint32_t)(val) << 13) & C20_PHY_TX_TERM_CTL_MASK)
/* MPLLA words */
#define C20_MPLLA_FRACEN (1u << 14)
#define C20_FB_CLK_DIV4_EN (1u << 13)
#define C20_MPLLA_TX_CLK_DIV_MASK (7u << 8)
/* MPLLB words */
#define C20_MPLLB_TX_CLK_DIV_MASK (7u << 13)
#define C20_MPLLB_FRACEN (1u << 13)
#define C20_REF_CLK_MPLLB_DIV_MASK (7u << 10)
#define C20_MULTIPLIER_MASK 0xfffu

/* The swing words of a C20 lane. */
#define C20_PHY_VSWING_PREEMPH_MASK 0x3fu
#define C20_PHY_VSWING_PREEMPH(val) ((uint32_t)(val) & C20_PHY_VSWING_PREEMPH_MASK)

/* The MPLLB calibration-done banks, cleared on a protocol switch. */
#define RAWLANEAONX_DIG_TX_MPLLB_CAL_DONE_BANK(idx) (0x303D + (idx))

/* Fields of the MPLLB words the HDMI (TMDS) computation fills in. */
#define C20_SSC_UP_SPREAD (1u << 9)
#define C20_WORD_CLK_DIV (1u << 8)
#define C20_MPLL_TX_CLK_DIV(val) (((uint32_t)(val) << 13) & C20_MPLLB_TX_CLK_DIV_MASK)
#define C20_MPLL_MULTIPLIER(val) ((uint32_t)(val) & C20_MULTIPLIER_MASK)
#define C20_MPLLB_ANA_FREQ_VCO_MASK (3u << 14)
#define C20_MPLLB_ANA_FREQ_VCO(val) (((uint32_t)(val) << 14) & C20_MPLLB_ANA_FREQ_VCO_MASK)
#define C20_MPLL_DIV_MULTIPLIER_MASK 0xffu
#define C20_MPLL_DIV_MULTIPLIER(val) ((uint32_t)(val) & C20_MPLL_DIV_MULTIPLIER_MASK)
#define C20_CAL_DAC_CODE_MASK (0x1fu << 10)
#define C20_CAL_DAC_CODE(val) (((uint32_t)(val) << 10) & C20_CAL_DAC_CODE_MASK)
#define C20_CP_INT_GS_MASK 0x7fu
#define C20_CP_INT_GS(val) ((uint32_t)(val) & C20_CP_INT_GS_MASK)
#define C20_CP_PROP_GS_MASK (0x7fu << 7)
#define C20_CP_PROP_GS(val) (((uint32_t)(val) << 7) & C20_CP_PROP_GS_MASK)
#define C20_CP_INT_MASK 0x7fu
#define C20_CP_INT(val) ((uint32_t)(val) & C20_CP_INT_MASK)
#define C20_CP_PROP_MASK (0x7fu << 7)
#define C20_CP_PROP(val) (((uint32_t)(val) << 7) & C20_CP_PROP_MASK)
#define C20_V2I_MASK (3u << 14)
#define C20_V2I(val) (((uint32_t)(val) << 14) & C20_V2I_MASK)
#define C20_HDMI_DIV_MASK 7u
#define C20_HDMI_DIV(val) ((uint32_t)(val) & C20_HDMI_DIV_MASK)

#endif
