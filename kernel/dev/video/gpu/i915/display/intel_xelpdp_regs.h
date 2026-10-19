// LikeOS -- display registers of display version 14 and the shared ones it
// needs that the main register header does not carry.
//
// Meteor Lake and Arrow Lake ("Xe LPD+") reorganised the parts of the
// display engine around the port: the Type-C and AUX logic moved into a
// block of its own (PICA) with its own interrupt bank, the AUX channels
// request their power in their own control registers, the data buffer
// grew to four slices, a power-management handshake (PM demand) tells
// the Punit what the display is about to need, and memory latencies are
// published in registers instead of through the pcode mailbox.  The rest
// of this file are definitions the older platforms share with it (the
// hotplug bits of the Ice Point family, the panel sequencer's second
// instance, the DMC event registers) where the main header lacks them or
// carries them under names that mean something else.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_XELPDP_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_XELPDP_REGS_H

/* ---- display identification ----------------------------------------------- */

/* Display version 14 reports its IP version and stepping itself. */
#define GMD_ID_DISPLAY 0x510a0
#define GMD_ID_DISPLAY_ARCH_SHIFT 22
#define GMD_ID_DISPLAY_ARCH_MASK (0x3ffu << 22)
#define GMD_ID_DISPLAY_RELEASE_SHIFT 14
#define GMD_ID_DISPLAY_RELEASE_MASK (0xffu << 14)
#define GMD_ID_DISPLAY_STEP_MASK 0x3fu

/* ---- power wells, DC states, the reset handshake ----------------------------- */

/* Display version 13+: one well per pipe after PW1/PW2, in the driver's
 * control register (HSW_PWR_WELL_CTL2). */
#define XELPD_PW_CTL_IDX_PW_A 5
#define XELPD_PW_CTL_IDX_PW_B 6
#define XELPD_PW_CTL_IDX_PW_C 7
#define XELPD_PW_CTL_IDX_PW_D 8

/* DC_STATE_EN: the deep-state selection field and the bits around it. */
#define DC_STATE_EN_UPTO_DC3CO_DC5_DC6_MASK 0x3u
#define DC_STATE_EN_UPTO_DC3CO 0x3u
#define DC_STATE_EN_DC9_BIT (1u << 3)
#define DC_STATE_EN_CSR_MASK_CMTG_0 (1u << 10) /* read-only, display 13+ */
#define DC_STATE_EN_CSR_MASK_CMTG_1 (1u << 11) /* read-only, display 20+ */
#define HOLD_PHY_PG1_LATCH (1u << 20)
#define HOLD_PHY_CLKREQ_PG1_LATCH (1u << 21)

/* The clock gating of a pipe's DSS/DSC, held off while the pipe's well
 * goes down (display version 14+). */
#define CLKGATE_DIS_DSSDSC 0x46548
#define DSS_PIPE_GATING_DISABLED(pipe) (1u << (25 + 2 * (pipe)))

/* Battlemage: the display arbiter's end-of-burst on half blocks, set
 * before the first plane or cursor is enabled. */
#define CHICKEN_MISC_2 0x42084
#define BMG_DARB_HALF_BLK_END_BURST (1u << 27)

/* MBUS credits of the ABOXes, Gen11 and Gen12. */
#define MBUS_ABOX_CTL(x) ((x) == 0 ? 0x45038 : (x) == 1 ? 0x45048 : 0x4504C)
#define MBUS_ABOX_BW_CREDIT_MASK (3u << 20)
#define MBUS_ABOX_BW_CREDIT(x) ((uint32_t)(x) << 20)
#define MBUS_ABOX_B_CREDIT_MASK (0xfu << 16)
#define MBUS_ABOX_B_CREDIT(x) ((uint32_t)(x) << 16)
#define MBUS_ABOX_BT_CREDIT_POOL2_MASK (0x1fu << 8)
#define MBUS_ABOX_BT_CREDIT_POOL2(x) ((uint32_t)(x) << 8)
#define MBUS_ABOX_BT_CREDIT_POOL1_MASK (0x1fu << 0)
#define MBUS_ABOX_BT_CREDIT_POOL1(x) ((uint32_t)(x) << 0)

/* Display chicken bits the init sequence touches on Gen12/13. */
#define GEN8_CHICKEN_DCPR_1 0x46430
#define DISABLE_FLR_SRC (1u << 15)
#define GEN11_CHICKEN_DCPR_2 0x46434
#define DCPR_MASK_MAXLATENCY_MEMUP_CLR (1u << 27)
#define DCPR_MASK_LPMODE (1u << 26)
#define DCPR_SEND_RESP_IMM (1u << 25)
#define DCPR_CLEAR_MEMSTAT_DIS (1u << 24)
#define XELPD_DISPLAY_ERR_FATAL_MASK 0x4421c
#ifndef SOUTH_DSPCLK_GATE_D
#define SOUTH_DSPCLK_GATE_D 0xc2020
#endif
#ifndef PCH_GMBUSUNIT_CLOCK_GATE_DISABLE
#define PCH_GMBUSUNIT_CLOCK_GATE_DISABLE (1u << 31)
#endif
#ifndef PCH_DPMGUNIT_CLOCK_GATE_DISABLE
#define PCH_DPMGUNIT_CLOCK_GATE_DISABLE (1u << 15)
#endif
#define BXT_GMBUS_GATING_DIS (1u << 14) /* GEN9_CLKGATE_DIS_4 */

/* Ice Lake's combo PHYs: the AUX lane of the PHY, and the override
 * a non-eDP AUX channel needs (display WA #1178). */
#define ICL_LANE_ENABLE_AUX (1u << 0)
#define O_FUNC_OVRD_EN (1u << 7)
#define O_LDO_BYPASS_CRI (1u << 0)

/* The Type-C ports' AUX wells of Ice Lake to Alder Lake-P: the
 * Thunderbolt ones, and the Dekel PHY's microcontroller health a
 * DP-alt/legacy AUX well waits for (bank 2 of the PHY). */
#define TGL_PW_CTL_IDX_AUX_TBT1 9
#define DKL_CMN_UC_DW27(tc) (0x168000 + (tc) * 0x1000 + 0x36C)
#define DKL_CMN_UC_DW27_BANK 2
#define DKL_CMN_UC_DW27_UC_HEALTH (1u << 15)

/* Type-C cold: the pcode requests that keep the Type-C subsystem out of
 * its cold state (Ice Lake legacy ports, Tiger Lake). */
#define ICL_PCODE_EXIT_TCCOLD 0x12
#define TGL_PCODE_TCCOLD 0x26
#define TGL_PCODE_EXIT_TCCOLD_DATA_L_EXIT_FAILED (1u << 0)
#define TGL_PCODE_EXIT_TCCOLD_DATA_L_BLOCK_REQ 0u
#define TGL_PCODE_EXIT_TCCOLD_DATA_L_UNBLOCK_REQ (1u << 0)

/* Alder Lake-P: the display's claim on a Type-C PHY is a bit of the
 * port's DDI_BUF_CTL. */
#define DDI_BUF_CTL_TC_PHY_OWNERSHIP (1u << 6)

/* The handshake that keeps the north display from being reset under a
 * PCH (and, on version 14, PICA) that is still talking to it. */
#define HSW_NDE_RSTWRN_OPT 0x46408
#define RESET_PCH_HANDSHAKE_ENABLE (1u << 4)
#define MTL_RESET_PICA_HANDSHAKE_EN (1u << 6)

/* ---- the data buffer and MBUS ------------------------------------------------ */

#ifndef DBUF_CTL_S
#define DBUF_CTL_S(slice) ((slice) == 0 ? 0x45008 : (slice) == 1 ? 0x44FE8 : \
			   (slice) == 2 ? 0x44300 : 0x44304)
#endif
#define DBUF_POWER_REQUEST_BIT (1u << 31)
#define DBUF_POWER_STATE_BIT (1u << 30)
#define DBUF_TRACKER_STATE_SERVICE_MASK (0x1fu << 19)
#define DBUF_TRACKER_STATE_SERVICE(x) ((uint32_t)(x) << 19)
#define DBUF_MIN_TRACKER_STATE_SERVICE_MASK (0x7u << 16)
#define DBUF_MIN_TRACKER_STATE_SERVICE(x) ((uint32_t)(x) << 16)
#define XELPDP_DBUF_SLICES 4
#define XELPDP_DBUF_SIZE 4096 /* blocks, all four slices */

#define MBUS_CTL_REG 0x4438C
#define MBUS_CTL_JOIN (1u << 31)
#define MBUS_HASHING_MODE_MASK (1u << 30)
#define MBUS_HASHING_MODE_2x2 (0u << 30)
#define MBUS_HASHING_MODE_1x4 (1u << 30)
#define MBUS_JOIN_PIPE_SELECT_MASK (7u << 26)
#define MBUS_JOIN_PIPE_SELECT(pipe) ((uint32_t)(pipe) << 26)
#define MBUS_JOIN_PIPE_SELECT_NONE (7u << 26)

#define PIPE_MBUS_DBOX_CTL(pipe) (0x7003C + (pipe) * 0x1000)
#define MBUS_DBOX_B2B_TRANSACTIONS_MAX(x) ((uint32_t)(x) << 20)
#define MBUS_DBOX_B2B_TRANSACTIONS_DELAY(x) ((uint32_t)(x) << 17)
#define MBUS_DBOX_REGULATE_B2B_TRANSACTIONS_EN (1u << 16)
#define MBUS_DBOX_BW_CREDIT(x) ((uint32_t)(x) << 14)
#define MBUS_DBOX_BW_4CREDITS_MTL (2u << 14)
#define MBUS_DBOX_BW_8CREDITS_MTL (3u << 14)
#define MBUS_DBOX_B_CREDIT(x) ((uint32_t)(x) << 8)
#define MBUS_DBOX_I_CREDIT(x) ((uint32_t)(x) << 5)
#define MBUS_DBOX_A_CREDIT(x) ((uint32_t)(x) << 0)

/* The arbiter's buddy logic, one instance per ABOX in use. */
#define BW_BUDDY_CTL(x) (0x45130 + (x) * 0x10)
#define BW_BUDDY_DISABLE (1u << 31)
#define BW_BUDDY_PAGE_MASK(x) (0x45134 + (x) * 0x10)

/* What the memory subsystem tells the display about itself. */
#define MTL_MEM_SS_INFO_GLOBAL 0x45700
#define MTL_N_OF_ENABLED_QGV_POINTS(v) (((v) >> 8) & 0xf)
#define MTL_N_OF_POPULATED_CH(v) (((v) >> 4) & 0xf)
#define MTL_DDR_TYPE(v) ((v) & 0xf)

/* Watermark latencies, two levels per register, in microseconds. */
#define MTL_LATENCY_LP0_LP1 0x45780
#define MTL_LATENCY_LP2_LP3 0x45784
#define MTL_LATENCY_LP4_LP5 0x45788
#define MTL_LATENCY_LEVEL_EVEN(v) ((v) & 0x1fff)
#define MTL_LATENCY_LEVEL_ODD(v) (((v) >> 16) & 0x1fff)
#define MTL_LATENCY_SAGV 0x4578c

/* ---- PM demand -------------------------------------------------------------- */

#define XELPDP_INITIATE_PMDEMAND_REQUEST(dw) (0x45230 + 4 * (dw))
/* dword 0 */
#define XELPDP_PMDEMAND_QCLK_GV_BW_SHIFT 16
#define XELPDP_PMDEMAND_QCLK_GV_BW_MASK (0xffffu << 16)
#define XELPDP_PMDEMAND_VOLTAGE_INDEX_SHIFT 12
#define XELPDP_PMDEMAND_VOLTAGE_INDEX_MASK (0x7u << 12)
#define XELPDP_PMDEMAND_QCLK_GV_INDEX_SHIFT 8
#define XELPDP_PMDEMAND_QCLK_GV_INDEX_MASK (0xfu << 8)
#define XELPDP_PMDEMAND_PIPES_SHIFT 6
#define XELPDP_PMDEMAND_PIPES_MASK (0x3u << 6)
/* version 30: the pipe count takes bits 7:4; no DBUF or scaler counts */
#define XE3_PMDEMAND_PIPES_SHIFT 4
#define XE3_PMDEMAND_PIPES_MASK (0xfu << 4)
#define XELPDP_PMDEMAND_DBUFS_SHIFT 4
#define XELPDP_PMDEMAND_DBUFS_MASK (0x3u << 4)
#define XELPDP_PMDEMAND_PHYS_SHIFT 0
#define XELPDP_PMDEMAND_PHYS_MASK (0x7u << 0)
/* dword 1 */
#define XELPDP_PMDEMAND_REQ_ENABLE (1u << 31)
#define XELPDP_PMDEMAND_CDCLK_FREQ_SHIFT 20
#define XELPDP_PMDEMAND_CDCLK_FREQ_MASK (0x7ffu << 20)
#define XELPDP_PMDEMAND_DDICLK_FREQ_SHIFT 8
#define XELPDP_PMDEMAND_DDICLK_FREQ_MASK (0x7ffu << 8)
#define XELPDP_PMDEMAND_SCALERS_SHIFT 4
#define XELPDP_PMDEMAND_SCALERS_MASK (0x7u << 4)
#define XELPDP_PMDEMAND_PLLS_SHIFT 0
#define XELPDP_PMDEMAND_PLLS_MASK (0x7u << 0)
#define GEN12_DCPR_STATUS_1 0x46440
#define XELPDP_PMDEMAND_INFLIGHT_STATUS (1u << 26)
#define XELPD_CHICKEN_DCPR_3 0x46438
#define DMD_RSP_TIMEOUT_DISABLE (1u << 19)
/* the response in the DE miscellaneous interrupt bank */
#define XELPDP_PMDEMAND_RSPTOUT_ERR (1u << 27)
#define XELPDP_PMDEMAND_RSP (1u << 3)

/* ---- CDCLK and the raw clock ------------------------------------------------ */

#define CDCLK_SQUASH_CTL 0x46008
#define CDCLK_SQUASH_ENABLE (1u << 31)
#define CDCLK_SQUASH_WINDOW_SIZE(v) (((v) >> 24) & 0xf)
#define CDCLK_SQUASH_WAVEFORM(v) ((v) & 0xffff)

/* Cannon Point and later: the raw clock is programmed as an integer
 * divider and, for 19.2 MHz, a fraction. */
#define CNP_RAWCLK_DIV(div) ((uint32_t)(div) << 16)
#define CNP_RAWCLK_DEN(den) ((uint32_t)(den) << 26)
#define ICP_RAWCLK_NUM(num) ((uint32_t)(num) << 11)
#define SFUSE_STRAP_RAW_FREQUENCY (1u << 8)

/* ---- AUX channels -------------------------------------------------------------- */

/* A and B in the north display as before; USBC1-4 in PICA, 0x200
 * apart.  `aux' is the channel index: 0 = A, 1 = B, 3.. = USBC1.. */
#ifndef XELPDP_DP_AUX_CH_CTL
#define XELPDP_DP_AUX_CH_CTL(aux) ((aux) < 3 ? 0x64010 + (aux) * 0x100 : \
				   0x16F210 + ((aux) - 3) * 0x200)
#define XELPDP_DP_AUX_CH_DATA(aux, i) (XELPDP_DP_AUX_CH_CTL(aux) + 4 + (i) * 4)
#endif
#ifndef XELPDP_DP_AUX_CH_CTL_POWER_REQUEST
#define XELPDP_DP_AUX_CH_CTL_POWER_REQUEST (1u << 19)
#define XELPDP_DP_AUX_CH_CTL_POWER_STATUS (1u << 18)
#endif
/* Display version 20 renumbered PICA's channels into one range that
 * starts with USBC1, A and B following USBC4; the registers themselves
 * are the same.  `ver' is the display version. */
#define XE2LPD_AUX_CH_IDX(aux) ((aux) >= 3 ? (aux) : 3 + 4 + (aux))
#define XELPDP_DP_AUX_CH_CTL_VER(ver, aux)                                      \
	XELPDP_DP_AUX_CH_CTL((ver) >= 20 ? XE2LPD_AUX_CH_IDX(aux) : (aux))
#define XELPDP_DP_AUX_CH_DATA_VER(ver, aux, i)                                  \
	XELPDP_DP_AUX_CH_DATA((ver) >= 20 ? XE2LPD_AUX_CH_IDX(aux) : (aux), i)

/* Display version 20+: the power well of PICA's Type-C half (the Type-C
 * ports' lanes and AUX channels), requested in a register of its own. */
#define XE2LPD_PICA_PW_CTL 0x16fe04
#define XE2LPD_PICA_CTL_POWER_REQUEST (1u << 31)
#define XE2LPD_PICA_CTL_POWER_STATUS (1u << 30)

/* ---- Type-C (PICA side) --------------------------------------------------------- */

/* The port buffer control of PICA: the Type-C subsystem's power request
 * and the display's claim on the PHY.  DDI A/B in the north display,
 * the Type-C ports in PICA. */
#ifndef XELPDP_PORT_BUF_CTL1
#define XELPDP_PORT_BUF_CTL1(port) ((port) < 3 ? 0x64004 + (port) * 0x100 : \
				    0x16F200 + ((port) - 3) * 0x200)
#endif
/* The same with display version 20's renumbering (A and B after TC4). */
#define XELPDP_PORT_BUF_CTL1_VER(ver, port)                                     \
	XELPDP_PORT_BUF_CTL1((ver) >= 20 && (port) < 3 ? 3 + 4 + (port) : (port))
#ifndef XELPDP_TC_PHY_OWNERSHIP
#define XELPDP_TC_PHY_OWNERSHIP (1u << 6)
#endif
#ifndef XELPDP_TCSS_POWER_REQUEST
#define XELPDP_TCSS_POWER_REQUEST (1u << 5)
#endif
#ifndef XELPDP_TCSS_POWER_STATE
#define XELPDP_TCSS_POWER_STATE (1u << 4)
#endif

/* Whether the IOM has the PHY ready to be handed to the display. */
#define TCSS_DDI_STATUS(tc) (0x161500 + (tc) * 4)
#define TCSS_DDI_STATUS_READY (1u << 2)
#define TCSS_DDI_STATUS_HPD_LIVE_STATUS_TBT (1u << 1)
/* The Type-C subsystem's mailbox from the display (display version 30
 * announces a TCSS power request through it first). */
#ifndef TCSS_DISP_MAILBOX_IN_CMD
#define TCSS_DISP_MAILBOX_IN_CMD 0x161300
#define TCSS_DISP_MAILBOX_IN_CMD_RUN_BUSY (1u << 31)
#define TCSS_DISP_MAILBOX_IN_CMD_CMD_MASK 0xffu
#define TCSS_DISP_MAILBOX_IN_CMD_DATA(val) ((uint32_t)(val) & 0xff)
#define TCSS_DISP_MAILBOX_IN_DATA 0x161304
#endif
/* Display version 20+: the DP-alt pin assignment (1 = A .. 6 = F) the
 * IOM set the port up for, here rather than in the FIA. */
#define TCSS_DDI_STATUS_PIN_ASSIGNMENT_SHIFT 25
#define TCSS_DDI_STATUS_PIN_ASSIGNMENT_MASK (0xfu << 25)

/* ---- hotplug -------------------------------------------------------------- */

/* PICA's interrupt bank: DP-alt and Thunderbolt hotplug per Type-C port
 * (the same layout as the GEN11 DE HPD bank), the AUX done bits. */
#define PICAINTERRUPT_ISR 0x16FE50
#define PICAINTERRUPT_IMR 0x16FE54
#define PICAINTERRUPT_IIR 0x16FE58
#define PICAINTERRUPT_IER 0x16FE5C
#define XELPDP_DP_ALT_HOTPLUG(tc) (1u << (16 + (tc)))
#define XELPDP_DP_ALT_HOTPLUG_MASK (0xfu << 16)
#define XELPDP_AUX_TC(tc) (1u << (8 + (tc)))
#define XELPDP_AUX_TC_MASK (0xfu << 8)
#define XELPDP_TBT_HOTPLUG(tc) (1u << (tc))
#define XELPDP_TBT_HOTPLUG_MASK 0xfu
#define XELPDP_PORT_HOTPLUG_CTL(tc) (0x16F270 + (tc) * 0x200)
#define XELPDP_TBT_HOTPLUG_ENABLE (1u << 6)
#define XELPDP_TBT_HPD_LONG_DETECT (1u << 5)
#define XELPDP_TBT_HPD_SHORT_DETECT (1u << 4)
#define XELPDP_DP_ALT_HOTPLUG_ENABLE (1u << 2)
#define XELPDP_DP_ALT_HPD_LONG_DETECT (1u << 1)
#define XELPDP_DP_ALT_HPD_SHORT_DETECT (1u << 0)

/* The south display of the Ice Point family and Meteor Point: DDI pins
 * from bit 16, Type-C (legacy) pins from bit 24, and on Meteor Point the
 * PICA bank's summary in bit 31. */
#define ICP_SDE_DDI_HOTPLUG(pin) (1u << (16 + (pin)))
#define ICP_SDE_TC_HOTPLUG(tc) (1u << (24 + (tc)))
#define SDE_GMBUS_ICP (1u << 23)
#define SDE_PICAINTERRUPT (1u << 31)
#define SHPD_FILTER_CNT 0xc4038
#define SHPD_FILTER_CNT_500_ADJ 0x001D9
#define SHPD_FILTER_CNT_250 0x000F8
/* SOUTH_CHICKEN1: the hotplug polarity of Meteor Point's pins, and the
 * Ice Point family's mux for the second panel sequencer. */
#define INVERT_DDIE_HPD (1u << 28)
#define INVERT_DDID_HPD_MTP (1u << 27)
#define INVERT_TC4_HPD (1u << 26)
#define INVERT_TC3_HPD (1u << 25)
#define INVERT_TC2_HPD (1u << 24)
#define INVERT_TC1_HPD (1u << 23)
#define INVERT_DDIC_HPD (1u << 17)
#define INVERT_DDIB_HPD (1u << 16)
#define INVERT_DDIA_HPD (1u << 15)
#define ICP_SECOND_PPS_IO_SELECT (1u << 2)

/* DG1/DG2's Type-C pin sits one bit higher than Ice Point's. */
#define DG1_SDE_TC_HOTPLUG(tc) (1u << (25 + (tc)))
#define INVERT_DDID_HPD (1u << 18)
/* Cannon Point (display WA #1179): the chassis clock request duration. */
#define CHASSIS_CLK_REQ_DURATION_MASK (0xfu << 8)
#define CHASSIS_CLK_REQ_DURATION(x) ((uint32_t)(x) << 8)
#define SPT_PWM_GRANULARITY (1u << 0)

/* Broxton's utility pin, which carries the second backlight PWM. */
#define UTIL_PIN_PIPE(x) ((uint32_t)(x) << 29)
#define UTIL_PIN_MODE_PWM (1u << 24)
#define UTIL_PIN_POLARITY (1u << 22)

/* PCH_PORT_HOTPLUG as Broxton uses it: the enable of each pin, and the
 * polarity bit next to it that the board's VBT may ask for. */
#define HPD_PORTA_HOTPLUG_ENABLE (1u << 28)
#define HPD_BXT_DDIA_INVERT (1u << 27)
#define HPD_PORTC_HOTPLUG_ENABLE (1u << 12)
#define HPD_BXT_DDIC_INVERT (1u << 11)
#define HPD_PORTB_HOTPLUG_ENABLE (1u << 4)
#define HPD_BXT_DDIB_INVERT (1u << 3)

/* ---- the display microcontrollers ------------------------------------------- */

/* Event handlers: eight per controller, a control and a handler address
 * each.  The main DMC's at 0x8F000, the pipe DMCs' at their own base. */
#define DMC_EVENT_HANDLER_COUNT_GEN12 8
#define DMC_MAIN_REG_BASE 0x8F000
#define DMC_ADLP_PIPE_REG_BASE 0x5F000
#define DMC_TGL_PIPE_REG_BASE 0x92000
#define DMC_EVT_HTP_OFF 0x004
#define DMC_EVT_CTL_OFF 0x034
#define DMC_EVT_CTL_ENABLE (1u << 31)
#define DMC_EVT_CTL_TYPE_SHIFT 16
#define DMC_EVT_CTL_TYPE_EDGE_0_1 3u
#define DMC_EVT_CTL_EVENT_ID_SHIFT 8
#define DMC_EVT_CTL_EVENT_ID_MASK (0xffu << 8)
#define DMC_EVENT_FALSE 0x1u
#define MAINDMC_EVENT_VBLANK_DELAYED_A 0x2eu
#define MAINDMC_EVENT_VBLANK_A 0x32u
#define MAINDMC_EVENT_CLK_MSEC 0xbfu
/* The windows the register writes of an image may fall in. */
#define DMC_MMIO_START_RANGE 0x80000
#define DMC_MMIO_END_RANGE 0x8FFFF
#define TGL_MAIN_MMIO_START 0x8F000
#define TGL_MAIN_MMIO_END 0x8FFFF
#define TGL_PIPE_MMIO_START(id) (0x92000 + ((id) - 1) * 0x4000)
#define TGL_PIPE_MMIO_END(id) (0x93FFF + ((id) - 1) * 0x4000)
#define ADLP_PIPE_MMIO_START 0x5F000
#define ADLP_PIPE_MMIO_END 0x5FFFF
/* Pipe DMC enables. */
#define PIPEDMC_CONTROL(pipe) (0x45250 + (pipe) * 4)
#define PIPEDMC_ENABLE (1u << 0)
#define MTL_PIPEDMC_CONTROL 0x45250
#define PIPEDMC_ENABLE_MTL(pipe) (1u << ((pipe) * 4))
/* Clock gating of the pipe DMCs while they are loaded. */
#define MTL_PIPEDMC_GATING_DIS(pipe) (1u << (15 - (pipe)))
#define CLKGATE_DIS_PSL_EXT(pipe) (0x4654C + (pipe) * 4)
#define PIPEDMC_GATING_DIS (1u << 12)
/* Display version 20+: the pipe DMCs' own interrupt and its mask, their
 * flip queues (control, scanline window, the five queues' head pointers,
 * the atomic tail pointers), which configuration each pipe DMC takes its
 * timestamps from, and (version 35) the DC state counters' enable. */
#define PIPEDMC_REG(pipe, a) ((a) + (uint32_t)(pipe) * 0x400)
#define PIPEDMC_INTERRUPT(pipe) PIPEDMC_REG(pipe, 0x5f190)
#define PIPEDMC_INTERRUPT_MASK(pipe) PIPEDMC_REG(pipe, 0x5f194)
#define PIPEDMC_FLIPQ_PROG_DONE (1u << 3)
#define PIPEDMC_ERROR (1u << 2)
#define PIPEDMC_GTT_FAULT (1u << 1)
#define PIPEDMC_ATS_FAULT (1u << 0)
#define PIPEDMC_FQ_CTRL(pipe) PIPEDMC_REG(pipe, 0x5f078)
#define PIPEDMC_FPQ_ATOMIC_TP(pipe) PIPEDMC_REG(pipe, 0x5f0a0)
#define PIPEDMC_SCANLINECMPLOWER(pipe) PIPEDMC_REG(pipe, 0x5f120)
#define PIPEDMC_SCANLINECMPUPPER(pipe) PIPEDMC_REG(pipe, 0x5f124)
#define PIPEDMC_FLIPQ_COUNT 5
#define PIPEDMC_FPQ_HP(pipe, fq)                                                \
	PIPEDMC_REG(pipe, (fq) < 2 ? 0x5f128 + (fq) * 0x10 : 0x5f168 + ((fq) - 2) * 0xc)
#define PIPEDMC_FPQ_CHP(pipe, fq)                                               \
	PIPEDMC_REG(pipe, (fq) < 2 ? 0x5f130 + (fq) * 0x10 : 0x5f170 + ((fq) - 2) * 0xc)
#define DMC_FQ_W2_PTS_CFG_SEL 0x8f240
#define DMC_W2_PTS_CONFIG_SELECT(pipe) ((uint32_t)(pipe) << ((pipe) * 8))
#define DC_COUNT_EN 0x457B4
#define DC_COUNT_EN_COUNTER_ENABLE (1u << 31)

#endif
