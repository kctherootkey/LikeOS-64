// LikeOS -- the C10 and C20 port PHYs of Meteor Lake and Arrow Lake.
//
// From display version 14 on, every DDI has a PHY of its own with its
// own PLL inside it, and the display reaches that PHY only through the
// port's "PICA" block: buffer control words for the lanes' power states
// and resets, a clock control word that muxes the port clock and starts
// a lane's PLL, and a message bus per PHY lane over which the PHY's
// internal 8-bit registers are read and written one transaction at a
// time.  DDI A and B carry a C10 PHY (DisplayPort/eDP up to 8.1 Gbps and
// HDMI): its PLL is twenty vendor registers written straight over the
// bus.  The Type-C ports carry a C20 PHY, whose configuration lives in
// SRAM reached through an address/data window; it holds two complete
// contexts (A and B), the new one is written into whichever is idle and
// the PHY switches over on a toggle bit.  A PHY has two lanes, each with
// two transmitters; a Type-C port in DisplayPort alternate mode with
// pin assignment D shares the PHY with USB and owns only lane 0.  A
// Type-C port routed through Thunderbolt does not use its PHY's PLL at
// all: the port clock control selects one of the Thunderbolt clocks.
//
// Lunar Lake keeps C10 PHYs on A and B; Panther Lake has one on A only
// (Wildcat Lake on A and B), every other port a C20; Battlemage's ports
// are all C20s with their SRAM contexts elsewhere.  From display version
// 20 the PICA blocks are numbered from the Type-C ports (A and B after
// TC4) and the die-to-die link is a bit of DDI_BUF_CTL.  Nova Lake has
// LT PHYs instead (intel_lt_phy.c): every entry point below hands over
// to that code there, the port buffer sequence staying the shared one.
//
// Every sequence here follows the hardware programming steps in order:
// port clock muxes, lanes out of reset into the ready state, the PLL
// written, the unused transmitters disabled, the PLL requested and its
// acknowledge awaited.  None of it has run on hardware; each step
// reports what it found.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2012-2025 Intel Corporation
// Portions Copyright (C) 2025 Synopsys, Inc.

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_cx0_phy_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/pci.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* PHY lanes as a mask: message bus transactions go to one lane, the
 * per-lane bits of the port registers are built from a mask. */
#define CX0_LANE0 0x1
#define CX0_LANE1 0x2
#define CX0_BOTH_LANES (CX0_LANE0 | CX0_LANE1)

#define MB_WRITE_UNCOMMITTED 0
#define MB_WRITE_COMMITTED 1

/* The PHYs' PLLs run from a 38.4 MHz reference on every part. */
#define CX0_REFCLK_KHZ 38400

/* ---- PLL states ------------------------------------------------------------- */

/* A C10 PLL: the transmitter and common words and the twenty PLL words,
 * exactly as they go over the message bus. */
struct c10_pll {
	uint8_t tx, cmn;
	uint8_t pll[20];
};

/* A C20 PLL: transmitter, common and MPLL words of one SRAM context.
 * Bit 7 of tx[0] chooses MPLLB (eleven words) or MPLLA (ten). */
struct c20_pll {
	uint16_t tx[3];
	uint16_t cmn[4];
	uint16_t mpll[11];
};

/* The C20's vendor registers: data width, protocol and rate. */
struct c20_vdr {
	uint8_t custom_width;
	uint8_t serdes_rate;
	uint8_t hdmi_rate;
};

struct c10_entry {
	uint32_t clock; /* kHz */
	const struct c10_pll *state;
};

struct c20_entry {
	uint32_t clock;
	const struct c20_pll *state;
};

/* What get_dp/get_hdmi worked out for a port, programmed at enable. */
struct cx0_port_pll {
	int in_use;
	int tbt; /* runs from a Thunderbolt clock, not the PHY's PLL */
	int is_c10;
	int is_dp;
	int ssc;
	uint32_t port_clock; /* kHz: the DP link rate, the TMDS clock */
	struct c10_pll c10;
	struct c20_pll c20;
	struct c20_vdr vdr;
};

static struct cx0_port_pll mtl_pll[INTEL_MAX_PORTS];
/* The last signal level asked for per port (re-applied when the buffer
 * is enabled, which is where the sequence wants it), and whether one
 * was. */
static int mtl_level[INTEL_MAX_PORTS];
static int mtl_level_set[INTEL_MAX_PORTS];
/* Version 30: the embedded panel's port is a C20 Type-C PHY. */
static int mtl_edp_typec;

/* ---- the PLL tables ---------------------------------------------------------- */

/* DisplayPort and eDP link rates, HDMI TMDS clocks: the PLL words for
 * each from the hardware tables.  The DP tables are with spread
 * spectrum; without it the C10's words 4..8 are written as 0. */
static const struct c10_pll c10_dp_rbr = {
	0x10, 0x21,
	{ 0xb4, 0x00, 0x30, 0x01, 0x26, 0x0c, 0x98, 0x46, 0x01, 0x01,
	  0x00, 0x00, 0xc0, 0x00, 0x00, 0x02, 0x84, 0x4f, 0xe5, 0x23 },
};

static const struct c10_pll c10_edp_r216 = {
	0x10, 0x21,
	{ 0x04, 0x00, 0xa2, 0x01, 0x33, 0x10, 0x75, 0xb3, 0x01, 0x01,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x85, 0x0f, 0xe6, 0x23 },
};

static const struct c10_pll c10_edp_r243 = {
	0x10, 0x21,
	{ 0x34, 0x00, 0xda, 0x01, 0x39, 0x12, 0xe3, 0xe9, 0x01, 0x01,
	  0x00, 0x00, 0x20, 0x00, 0x00, 0x02, 0x85, 0x8f, 0xe6, 0x23 },
};

static const struct c10_pll c10_dp_hbr1 = {
	0x10, 0x21,
	{ 0xf4, 0x00, 0xf8, 0x00, 0x20, 0x0a, 0x29, 0x10, 0x01, 0x01,
	  0x00, 0x00, 0xa0, 0x00, 0x00, 0x01, 0x84, 0x4f, 0xe5, 0x23 },
};

static const struct c10_pll c10_edp_r324 = {
	0x10, 0x21,
	{ 0xb4, 0x00, 0x30, 0x01, 0x26, 0x0c, 0x98, 0x46, 0x01, 0x01,
	  0x00, 0x00, 0xc0, 0x00, 0x00, 0x01, 0x85, 0x4f, 0xe6, 0x23 },
};

static const struct c10_pll c10_edp_r432 = {
	0x10, 0x21,
	{ 0x04, 0x00, 0xa2, 0x01, 0x33, 0x10, 0x75, 0xb3, 0x01, 0x01,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x85, 0x0f, 0xe6, 0x23 },
};

static const struct c10_pll c10_dp_hbr2 = {
	0x10, 0x21,
	{ 0xf4, 0x00, 0xf8, 0x00, 0x20, 0x0a, 0x29, 0x10, 0x01, 0x01,
	  0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x84, 0x4f, 0xe5, 0x23 },
};

static const struct c10_pll c10_edp_r675 = {
	0x10, 0x21,
	{ 0xb4, 0x00, 0x3e, 0x01, 0xa8, 0x0c, 0x33, 0x54, 0x01, 0x01,
	  0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x85, 0x8f, 0xe6, 0x23 },
};

static const struct c10_pll c10_dp_hbr3 = {
	0x10, 0x21,
	{ 0x34, 0x00, 0x84, 0x01, 0x30, 0x0f, 0x3d, 0x98, 0x01, 0x01,
	  0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x84, 0x0f, 0xe5, 0x23 },
};

static const struct c10_pll c10_hdmi_25_2 = {
	0x10, 0x01,
	{ 0x04, 0x00, 0xb2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x06, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_27_0 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01,
	  0x00, 0x00, 0x80, 0x00, 0x00, 0x0d, 0x06, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_74_25 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01,
	  0x00, 0x00, 0x58, 0x00, 0x00, 0x0b, 0x06, 0x0f, 0x85, 0x23 },
};

static const struct c10_pll c10_hdmi_148_5 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01,
	  0x00, 0x00, 0x58, 0x00, 0x00, 0x0a, 0x06, 0x0f, 0x85, 0x23 },
};

static const struct c10_pll c10_hdmi_594 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01,
	  0x00, 0x00, 0x58, 0x00, 0x00, 0x08, 0x06, 0x0f, 0x85, 0x23 },
};

static const struct c10_pll c10_hdmi_27027 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xcc, 0x9c, 0xcb, 0xcc, 0x0d, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_28320 = {
	0x10, 0x01,
	{ 0x04, 0x00, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_30240 = {
	0x10, 0x01,
	{ 0x04, 0x00, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_31500 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0xa0, 0x00, 0x00, 0x0c, 0x09, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_36000 = {
	0x10, 0x01,
	{ 0xc4, 0x00, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_40000 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0x55, 0x55, 0x55, 0x0c, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_49500 = {
	0x10, 0x01,
	{ 0x74, 0x00, 0xae, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x20, 0x00, 0x00, 0x0c, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_50000 = {
	0x10, 0x01,
	{ 0x74, 0x00, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0x2a, 0xa9, 0xaa, 0x0c, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_57284 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x77, 0x57, 0x77, 0x77, 0x0c, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_58000 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0xd5, 0x55, 0x55, 0x0c, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_65000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0xb5, 0x55, 0x55, 0x0b, 0x09, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_71000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0xf5, 0x55, 0x55, 0x0b, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_74176 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x44, 0x44, 0x44, 0x44, 0x0b, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_75000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x20, 0x00, 0x00, 0x0b, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_78750 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x08, 0x00, 0x00, 0x0b, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_85500 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x10, 0x00, 0x00, 0x0b, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_88750 = {
	0x10, 0x01,
	{ 0x74, 0x00, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0x72, 0xa9, 0xaa, 0x0b, 0x09, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_106500 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0xf0, 0x00, 0x00, 0x0b, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_108000 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x80, 0x00, 0x00, 0x0b, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_115500 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x50, 0x00, 0x00, 0x0b, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_119000 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xd6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0xf5, 0x55, 0x55, 0x0b, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_135000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x50, 0x00, 0x00, 0x0a, 0x09, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_138500 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0x22, 0xa9, 0xaa, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_147160 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0xa5, 0x55, 0x55, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_148352 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x44, 0x44, 0x44, 0x44, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_154000 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0x35, 0x55, 0x55, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_162000 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x60, 0x00, 0x00, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_167000 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0xfa, 0xa9, 0xaa, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_197802 = {
	0x10, 0x01,
	{ 0x74, 0x00, 0xae, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x99, 0x05, 0x98, 0x99, 0x0a, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_198000 = {
	0x10, 0x01,
	{ 0x74, 0x00, 0xae, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x20, 0x00, 0x00, 0x0a, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_209800 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xba, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0x45, 0x55, 0x55, 0x0a, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_241500 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0xc8, 0x00, 0x00, 0x0a, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_262750 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0x6c, 0xa9, 0xaa, 0x09, 0x09, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_268500 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0xec, 0x00, 0x00, 0x09, 0x09, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_296703 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x33, 0x44, 0x33, 0x33, 0x09, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_297000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x00, 0x58, 0x00, 0x00, 0x09, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_319750 = {
	0x10, 0x01,
	{ 0xb4, 0x00, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0xaa, 0x44, 0xa9, 0xaa, 0x09, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_497750 = {
	0x10, 0x01,
	{ 0x34, 0x00, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0x9f, 0x55, 0x55, 0x09, 0x08, 0xcf, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_592000 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x55, 0x15, 0x55, 0x55, 0x08, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c10_pll c10_hdmi_593407 = {
	0x10, 0x01,
	{ 0xf4, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xff,
	  0xff, 0x3b, 0x44, 0xba, 0xbb, 0x08, 0x08, 0x8f, 0x84, 0x23 },
};

static const struct c20_pll c20_dp_rbr = {
	{ 0xbe88, 0x5800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x50a8, 0x2120, 0xcd9a, 0xbfc1, 0x5ab8, 0x4c34,
	  0x2000, 0x0001, 0x6000, 0x0000, 0x0000 },
};

static const struct c20_pll c20_dp_hbr1 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x308c, 0x2110, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81,
	  0x2000, 0x0001, 0x5000, 0x0000, 0x0000 },
};

static const struct c20_pll c20_dp_hbr2 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x108c, 0x2108, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81,
	  0x2000, 0x0001, 0x5000, 0x0000, 0x0000 },
};

static const struct c20_pll c20_dp_hbr3 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x10d2, 0x2108, 0x8d98, 0xbfc1, 0x7166, 0x5f42,
	  0x2000, 0x0001, 0x7800, 0x0000, 0x0000 },
};

static const struct c20_pll c20_dp_uhbr10 = {
	{ 0xbe21, 0xe800, 0x0000 },
	{ 0x0700, 0x0005, 0x0000, 0x0000 },
	{ 0x3104, 0xd105, 0xc025, 0xc025, 0x8c00, 0x759a,
	  0x4000, 0x0003, 0x3555, 0x0001 },
};

static const struct c20_pll c20_dp_uhbr13_5 = {
	{ 0xbea0, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x015f, 0x2205, 0x1b17, 0xffc1, 0xe100, 0xbd00,
	  0x2000, 0x0001, 0x4800, 0x0000, 0x0000 },
};

static const struct c20_pll c20_dp_uhbr20 = {
	{ 0xbe20, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x3104, 0xd105, 0x9217, 0x9217, 0x8c00, 0x759a,
	  0x4000, 0x0003, 0x3555, 0x0001 },
};

static const struct c20_pll xe2hpd_c20_edp_r216 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x50e1, 0x2120, 0x8e18, 0xbfc1, 0x9000, 0x78f6,
	  0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
};

static const struct c20_pll xe2hpd_c20_edp_r243 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x50fd, 0x2120, 0x8f18, 0xbfc1, 0xa200, 0x8814,
	  0x2000, 0x0001, 0x1000, 0x0000, 0x0000 },
};

static const struct c20_pll xe2hpd_c20_edp_r324 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x30a8, 0x2110, 0xcd9a, 0xbfc1, 0x6c00, 0x5ab8,
	  0x2000, 0x0001, 0x6000, 0x0000, 0x0000 },
};

static const struct c20_pll xe2hpd_c20_edp_r432 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x30e1, 0x2110, 0x8e18, 0xbfc1, 0x9000, 0x78f6,
	  0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
};

static const struct c20_pll xe2hpd_c20_edp_r675 = {
	{ 0xbe88, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x10af, 0x2108, 0xce1a, 0xbfc1, 0x7080, 0x5e80,
	  0x2000, 0x0001, 0x6400, 0x0000, 0x0000 },
};

static const struct c20_pll xe2hpd_c20_dp_uhbr13_5 = {
	{ 0xbea0, 0x4800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x015f, 0x2205, 0x1b17, 0xffc1, 0xbd00, 0x9ec3,
	  0x2000, 0x0001, 0x4800, 0x0000, 0x0000 },
};

static const struct c20_pll c20_hdmi_27_0 = {
	{ 0xbe88, 0x9800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0xa0e0, 0x7d80, 0x0906, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x8000, 0x0000, 0x0001 },
};

static const struct c20_pll c20_hdmi_74_25 = {
	{ 0xbe88, 0x9800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x609a, 0x7d40, 0xca06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x5800, 0x0000, 0x0001 },
};

static const struct c20_pll c20_hdmi_148_5 = {
	{ 0xbe88, 0x9800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x409a, 0x7d20, 0xca06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x5800, 0x0000, 0x0001 },
};

static const struct c20_pll c20_hdmi_594 = {
	{ 0xbe88, 0x9800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x009a, 0x7d08, 0xca06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x5800, 0x0000, 0x0001 },
};

static const struct c20_pll c20_hdmi_300 = {
	{ 0xbe98, 0x8800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x309c, 0x2110, 0xca06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x2000, 0x0000, 0x0004 },
};

static const struct c20_pll c20_hdmi_600 = {
	{ 0xbe98, 0x8800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x109c, 0x2108, 0xca06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x2000, 0x0000, 0x0004 },
};

static const struct c20_pll c20_hdmi_800 = {
	{ 0xbe98, 0x8800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x10d0, 0x2108, 0x4a06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0003, 0x2aaa, 0x0002, 0x0004 },
};

static const struct c20_pll c20_hdmi_1000 = {
	{ 0xbe98, 0x8800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x1104, 0x2108, 0x0a06, 0xbe40, 0x0000, 0x0000,
	  0x2200, 0x0003, 0x3555, 0x0001, 0x0004 },
};

static const struct c20_pll c20_hdmi_1200 = {
	{ 0xbe98, 0x8800, 0x0000 },
	{ 0x0500, 0x0005, 0x0000, 0x0000 },
	{ 0x1138, 0x2108, 0x5486, 0xfe40, 0x0000, 0x0000,
	  0x2200, 0x0001, 0x4000, 0x0000, 0x0004 },
};

static const struct c10_entry c10_dp_tables[] = {
	{ 162000, &c10_dp_rbr },
	{ 270000, &c10_dp_hbr1 },
	{ 540000, &c10_dp_hbr2 },
	{ 810000, &c10_dp_hbr3 },
	{ 0, NULL },
};

static const struct c10_entry c10_edp_tables[] = {
	{ 162000, &c10_dp_rbr },
	{ 216000, &c10_edp_r216 },
	{ 243000, &c10_edp_r243 },
	{ 270000, &c10_dp_hbr1 },
	{ 324000, &c10_edp_r324 },
	{ 432000, &c10_edp_r432 },
	{ 540000, &c10_dp_hbr2 },
	{ 675000, &c10_edp_r675 },
	{ 810000, &c10_dp_hbr3 },
	{ 0, NULL },
};

static const struct c20_entry c20_dp_tables[] = {
	{ 162000, &c20_dp_rbr },
	{ 270000, &c20_dp_hbr1 },
	{ 540000, &c20_dp_hbr2 },
	{ 810000, &c20_dp_hbr3 },
	{ 1000000, &c20_dp_uhbr10 },
	{ 1350000, &c20_dp_uhbr13_5 },
	{ 2000000, &c20_dp_uhbr20 },
	{ 0, NULL },
};

static const struct c20_entry xe2hpd_c20_edp_tables[] = {
	{ 162000, &c20_dp_rbr },
	{ 216000, &xe2hpd_c20_edp_r216 },
	{ 243000, &xe2hpd_c20_edp_r243 },
	{ 270000, &c20_dp_hbr1 },
	{ 324000, &xe2hpd_c20_edp_r324 },
	{ 432000, &xe2hpd_c20_edp_r432 },
	{ 540000, &c20_dp_hbr2 },
	{ 675000, &xe2hpd_c20_edp_r675 },
	{ 810000, &c20_dp_hbr3 },
	{ 0, NULL },
};

static const struct c20_entry xe2hpd_c20_dp_tables[] = {
	{ 162000, &c20_dp_rbr },
	{ 270000, &c20_dp_hbr1 },
	{ 540000, &c20_dp_hbr2 },
	{ 810000, &c20_dp_hbr3 },
	{ 1000000, &c20_dp_uhbr10 },
	{ 1350000, &xe2hpd_c20_dp_uhbr13_5 },
	{ 0, NULL },
};

static const struct c20_entry xe3lpd_c20_dp_edp_tables[] = {
	{ 162000, &c20_dp_rbr },
	{ 216000, &xe2hpd_c20_edp_r216 },
	{ 243000, &xe2hpd_c20_edp_r243 },
	{ 270000, &c20_dp_hbr1 },
	{ 324000, &xe2hpd_c20_edp_r324 },
	{ 432000, &xe2hpd_c20_edp_r432 },
	{ 540000, &c20_dp_hbr2 },
	{ 675000, &xe2hpd_c20_edp_r675 },
	{ 810000, &c20_dp_hbr3 },
	{ 1000000, &c20_dp_uhbr10 },
	{ 1350000, &xe2hpd_c20_dp_uhbr13_5 },
	{ 2000000, &c20_dp_uhbr20 },
	{ 0, NULL },
};

static const struct c10_entry c10_hdmi_tables[] = {
	{ 25200, &c10_hdmi_25_2 },
	{ 27000, &c10_hdmi_27_0 },
	{ 27027, &c10_hdmi_27027 },
	{ 28320, &c10_hdmi_28320 },
	{ 30240, &c10_hdmi_30240 },
	{ 31500, &c10_hdmi_31500 },
	{ 36000, &c10_hdmi_36000 },
	{ 40000, &c10_hdmi_40000 },
	{ 49500, &c10_hdmi_49500 },
	{ 50000, &c10_hdmi_50000 },
	{ 57284, &c10_hdmi_57284 },
	{ 58000, &c10_hdmi_58000 },
	{ 65000, &c10_hdmi_65000 },
	{ 71000, &c10_hdmi_71000 },
	{ 74176, &c10_hdmi_74176 },
	{ 74250, &c10_hdmi_74_25 },
	{ 75000, &c10_hdmi_75000 },
	{ 78750, &c10_hdmi_78750 },
	{ 85500, &c10_hdmi_85500 },
	{ 88750, &c10_hdmi_88750 },
	{ 106500, &c10_hdmi_106500 },
	{ 108000, &c10_hdmi_108000 },
	{ 115500, &c10_hdmi_115500 },
	{ 119000, &c10_hdmi_119000 },
	{ 135000, &c10_hdmi_135000 },
	{ 138500, &c10_hdmi_138500 },
	{ 147160, &c10_hdmi_147160 },
	{ 148352, &c10_hdmi_148352 },
	{ 148500, &c10_hdmi_148_5 },
	{ 154000, &c10_hdmi_154000 },
	{ 162000, &c10_hdmi_162000 },
	{ 167000, &c10_hdmi_167000 },
	{ 197802, &c10_hdmi_197802 },
	{ 198000, &c10_hdmi_198000 },
	{ 209800, &c10_hdmi_209800 },
	{ 241500, &c10_hdmi_241500 },
	{ 262750, &c10_hdmi_262750 },
	{ 268500, &c10_hdmi_268500 },
	{ 296703, &c10_hdmi_296703 },
	{ 297000, &c10_hdmi_297000 },
	{ 319750, &c10_hdmi_319750 },
	{ 497750, &c10_hdmi_497750 },
	{ 592000, &c10_hdmi_592000 },
	{ 593407, &c10_hdmi_593407 },
	{ 594000, &c10_hdmi_594 },
	{ 0, NULL },
};

static const struct c20_entry c20_hdmi_tables[] = {
	{ 27000, &c20_hdmi_27_0 },
	{ 74250, &c20_hdmi_74_25 },
	{ 148500, &c20_hdmi_148_5 },
	{ 594000, &c20_hdmi_594 },
	{ 300000, &c20_hdmi_300 },
	{ 600000, &c20_hdmi_600 },
	{ 800000, &c20_hdmi_800 },
	{ 1000000, &c20_hdmi_1000 },
	{ 1200000, &c20_hdmi_1200 },
	{ 0, NULL },
};

/* ---- buffer translations ------------------------------------------------------ */

/* One level: the swing and the two cursor coefficients written into the
 * transmitter's override words. */
struct cx0_trans {
	uint8_t vswing, pre_cursor, post_cursor;
};

/* C10, every protocol: ordered like the DisplayPort levels, 400mV x
 * {0, 3.5, 6, 9.5 dB}, 600mV x {0, 3.5, 6}, 800mV x {0, 3.5}, 1200mV;
 * HDMI takes the last. */
static const struct cx0_trans c10_trans_dp14[] = {
	{ 26, 0, 0 }, { 33, 0, 6 }, { 38, 0, 11 }, { 43, 0, 19 }, { 39, 0, 0 },
	{ 45, 0, 7 }, { 46, 0, 13 }, { 46, 0, 0 }, { 55, 0, 7 }, { 62, 0, 0 },
};
#define C10_TRANS_HDMI_DEFAULT 9

/* C20, DisplayPort 1.4 (and eDP), same order. */
static const struct cx0_trans c20_trans_dp14[] = {
	{ 20, 0, 0 }, { 24, 0, 4 }, { 30, 0, 9 }, { 34, 0, 14 }, { 29, 0, 0 },
	{ 34, 0, 5 }, { 38, 0, 10 }, { 36, 0, 0 }, { 40, 0, 6 }, { 48, 0, 0 },
};

/* C20, HDMI 2.0: five presets, the first the default. */
static const struct cx0_trans c20_trans_hdmi[] = {
	{ 48, 0, 0 }, { 38, 4, 6 }, { 36, 4, 8 }, { 34, 4, 10 }, { 32, 4, 12 },
};
#define C20_TRANS_HDMI_DEFAULT 0

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- the platform ------------------------------------------------------------- */

/* The display version the sequences are gated on.  The Xe2/Xe3 parts are
 * only named by this driver (no display version in their descriptor), so
 * it comes from the platform there; the discrete Xe2 part (Battlemage)
 * has a version 14 display with its own C20 SRAM layout. */
static int cx0_display_ver(struct i915_device *i915)
{
	switch (i915->info->platform) {
	case I915_PLATFORM_BATTLEMAGE:
		return 14;
	case I915_PLATFORM_LUNARLAKE:
		return i915->info->display_ver ? i915->info->display_ver : 20;
	case I915_PLATFORM_PANTHERLAKE:
	case I915_PLATFORM_WILDCATLAKE:
		return i915->info->display_ver ? i915->info->display_ver : 30;
	case I915_PLATFORM_NOVALAKE_S:
	case I915_PLATFORM_NOVALAKE_P:
		return i915->info->display_ver ? i915->info->display_ver : 35;
	default:
		return i915->info->display_ver;
	}
}

static int cx0_is_xe2hpd(struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_BATTLEMAGE;
}

/* Which ports have a C10 PHY: A and B on Meteor/Arrow Lake and Lunar
 * Lake, A alone on Panther Lake, A and B on Wildcat Lake, none on
 * Battlemage (every PHY a C20). */
static int cx0_port_is_c10(struct i915_device *i915, int port)
{
	int ver = cx0_display_ver(i915);

	if (i915->info->platform == I915_PLATFORM_WILDCATLAKE ||
	    intel_display_verx100(i915) == 3002)
		return port <= PORT_B;
	if (i915->info->platform == I915_PLATFORM_PANTHERLAKE)
		return port == PORT_A;
	if (cx0_is_xe2hpd(i915))
		return 0;
	if (ver == 14 || ver == 20)
		return port < PORT_C;
	return 0;
}

/* ---- a port and its PHY ----------------------------------------------------- */

/* Everything the sequences need to know about the port they drive. */
struct cx0 {
	struct i915_device *i915;
	int port;
	int ver;
	int xe2hpd;
	int c10;
	int reversal;
	int dp_alt; /* Type-C in DisplayPort alternate mode */
	int legacy; /* Type-C wired to a fixed connector */
	int tbt; /* Type-C through Thunderbolt */
	int is_edp;
	uint8_t owned; /* the PHY lanes the display owns */
	const char *name;
};

static void cx0_setup(struct cx0 *c, struct i915_device *i915, int port,
		      const struct intel_output *o)
{
	c->i915 = i915;
	c->port = port;
	c->ver = cx0_display_ver(i915);
	c->xe2hpd = cx0_is_xe2hpd(i915);
	c->c10 = cx0_port_is_c10(i915, port);
	c->reversal = o ? !!o->lane_reversal : 0;
	c->dp_alt = o && o->is_tc && o->tc_mode == INTEL_TC_DP_ALT;
	c->legacy = o && o->is_tc && o->tc_mode == INTEL_TC_LEGACY;
	c->tbt = o && o->is_tc && o->tc_mode == INTEL_TC_TBT_ALT;
	c->is_edp = o ? !!o->is_edp : 0;
	c->name = intel_port_name(i915, port);
	/* In DP-alt mode with pin assignment D (two lanes or fewer granted)
	 * PHY lane 1 belongs to USB; the display owns lane 0 only. */
	c->owned = CX0_BOTH_LANES;
	if (c->dp_alt) {
		uint32_t max = o->tc_lanes ? o->tc_lanes : 4;
		if (max <= 2)
			c->owned = CX0_LANE0;
	}
}

static int port_ok(int port)
{
	return port >= 0 && port < INTEL_MAX_PORTS;
}

static uint32_t rd(struct cx0 *c, uint32_t reg)
{
	return i915_read32(c->i915, reg);
}

static void wr(struct cx0 *c, uint32_t reg, uint32_t v)
{
	i915_write32(c->i915, reg, v);
}

/* Read-modify-write; the write always goes out (several of these bits
 * are write-one-to-clear). */
static uint32_t rmw(struct cx0 *c, uint32_t reg, uint32_t clear, uint32_t set)
{
	uint32_t old = rd(c, reg);
	wr(c, reg, (old & ~clear) | set);
	return old;
}

/* Poll until (reg & mask) == value; the last value read goes to *out. */
static int wait_reg(struct cx0 *c, uint32_t reg, uint32_t mask, uint32_t value,
		    uint32_t timeout_us, uint32_t *out)
{
	uint32_t step = timeout_us > 2000 ? 10 : 1;
	uint32_t v = 0;

	for (uint32_t t = 0;; t += step) {
		v = rd(c, reg);
		if ((v & mask) == value) {
			if (out)
				*out = v;
			return 0;
		}
		if (t >= timeout_us)
			break;
		lapic_delay_us(step);
	}
	if (out)
		*out = v;
	return -ETIMEDOUT;
}

static int wait_set(struct cx0 *c, uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
	return wait_reg(c, reg, mask, mask, timeout_us, NULL);
}

static int wait_clear(struct cx0 *c, uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
	return wait_reg(c, reg, mask, 0, timeout_us, NULL);
}

/* ---- the message bus ------------------------------------------------------------ */

static int msgbus_reported;

static uint32_t msgbus_ctl(struct cx0 *c, int lane)
{
	return XELPDP_PORT_M2P_MSGBUS_CTL(c->ver, c->port, lane);
}

static uint32_t msgbus_status(struct cx0 *c, int lane)
{
	return XELPDP_PORT_P2M_MSGBUS_STATUS(c->ver, c->port, lane);
}

/* The response-ready and error flags are cleared by writing them. */
static void cx0_clear_response_ready(struct cx0 *c, int lane)
{
	rmw(c, msgbus_status(c, lane), 0,
	    XELPDP_PORT_P2M_RESPONSE_READY | XELPDP_PORT_P2M_ERROR_SET);
}

/* Abort whatever the lane's bus is doing and bring it back to idle. */
static void cx0_bus_reset(struct cx0 *c, int lane)
{
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_RESET);
	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_RESET,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		if (!msgbus_reported) {
			msgbus_reported = 1;
			kprintf("[drm] i915: port %s: PHY message bus does not return to idle\n",
				c->name);
		}
		return;
	}
	cx0_clear_response_ready(c, lane);
}

/* Wait for the PHY's answer to a read or committed write and check it is
 * the answer to that command. */
static int cx0_wait_for_ack(struct cx0 *c, uint32_t command, int lane, uint32_t *val)
{
	if (wait_reg(c, msgbus_status(c, lane), XELPDP_PORT_P2M_RESPONSE_READY,
		     XELPDP_PORT_P2M_RESPONSE_READY, XELPDP_MSGBUS_TIMEOUT_US, val)) {
		i915_dbg("[drm] i915: port %s: PHY lane %d message ack timeout, status %08x\n",
			 c->name, lane, *val);
		if (!(rd(c, XELPDP_PORT_MSGBUS_TIMER(c->ver, c->port, lane)) &
		      XELPDP_PORT_MSGBUS_TIMER_TIMED_OUT))
			i915_dbg("[drm] i915: port %s: the PHY's bus timer saw no timeout\n",
				 c->name);
		cx0_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	if (*val & XELPDP_PORT_P2M_ERROR_SET) {
		i915_dbg("[drm] i915: port %s: PHY lane %d %s error, status %08x\n", c->name, lane,
			 command == XELPDP_PORT_P2M_COMMAND_READ_ACK ? "read" : "write", *val);
		cx0_bus_reset(c, lane);
		return -EINVAL;
	}
	if (((*val & XELPDP_PORT_P2M_COMMAND_TYPE_MASK) >> XELPDP_PORT_P2M_COMMAND_TYPE_SHIFT) !=
	    command) {
		i915_dbg("[drm] i915: port %s: PHY lane %d: not a %s response, status %08x\n",
			 c->name, lane,
			 command == XELPDP_PORT_P2M_COMMAND_READ_ACK ? "read" : "write", *val);
		cx0_bus_reset(c, lane);
		return -EINVAL;
	}
	return 0;
}

static int cx0_read_once(struct cx0 *c, int lane, uint16_t addr)
{
	uint32_t val = 0;
	int ret;

	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: previous PHY transaction still pending, resetting the bus\n",
			 c->name);
		cx0_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	cx0_clear_response_ready(c, lane);
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING |
	   XELPDP_PORT_M2P_COMMAND_READ | XELPDP_PORT_M2P_ADDRESS(addr));
	ret = cx0_wait_for_ack(c, XELPDP_PORT_P2M_COMMAND_READ_ACK, lane, &val);
	if (ret < 0)
		return ret;
	cx0_clear_response_ready(c, lane);
	/* Before version 30 the bus is reset after every transaction, which
	 * leaves it in a known state for the next one. */
	if (c->ver < 30)
		cx0_bus_reset(c, lane);
	return (int)((val & XELPDP_PORT_P2M_DATA_MASK) >> XELPDP_PORT_P2M_DATA_SHIFT);
}

static uint8_t cx0_read_lane(struct cx0 *c, int lane, uint16_t addr)
{
	int status = 0;

	/* three tries are enough for a read to get through */
	for (int i = 0; i < 3; i++) {
		status = cx0_read_once(c, lane, addr);
		if (status >= 0)
			return (uint8_t)status;
	}
	if (!msgbus_reported) {
		msgbus_reported = 1;
		kprintf("[drm] i915: port %s: PHY read of %04x failed after 3 tries\n", c->name,
			addr);
	}
	return 0;
}

/* A read goes to one lane: the mask has exactly one bit. */
static uint8_t cx0_read(struct cx0 *c, uint8_t lane_mask, uint16_t addr)
{
	return cx0_read_lane(c, lane_mask == CX0_LANE1 ? 1 : 0, addr);
}

/* An uncommitted write is only queued in the PHY; a committed one is
 * acknowledged once it has taken effect. */
static int cx0_write_once(struct cx0 *c, int lane, uint16_t addr, uint8_t data, int committed)
{
	uint32_t val = 0;

	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: previous PHY transaction still pending, resetting the bus\n",
			 c->name);
		cx0_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	cx0_clear_response_ready(c, lane);
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING |
	   (committed ? XELPDP_PORT_M2P_COMMAND_WRITE_COMMITTED :
			XELPDP_PORT_M2P_COMMAND_WRITE_UNCOMMITTED) |
	   XELPDP_PORT_M2P_DATA(data) | XELPDP_PORT_M2P_ADDRESS(addr));
	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: PHY write did not complete, resetting the bus\n",
			 c->name);
		cx0_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	if (committed) {
		int ret = cx0_wait_for_ack(c, XELPDP_PORT_P2M_COMMAND_WRITE_ACK, lane, &val);
		if (ret < 0)
			return ret;
	} else if (rd(c, msgbus_status(c, lane)) & XELPDP_PORT_P2M_ERROR_SET) {
		i915_dbg("[drm] i915: port %s: PHY lane %d write error\n", c->name, lane);
		cx0_bus_reset(c, lane);
		return -EINVAL;
	}
	cx0_clear_response_ready(c, lane);
	if (c->ver < 30)
		cx0_bus_reset(c, lane);
	return 0;
}

static void cx0_write_lane(struct cx0 *c, int lane, uint16_t addr, uint8_t data, int committed)
{
	for (int i = 0; i < 3; i++)
		if (cx0_write_once(c, lane, addr, data, committed) == 0)
			return;
	if (!msgbus_reported) {
		msgbus_reported = 1;
		kprintf("[drm] i915: port %s: PHY write of %04x failed after 3 tries\n", c->name,
			addr);
	}
}

/* A write goes to every lane in the mask. */
static void cx0_write(struct cx0 *c, uint8_t lane_mask, uint16_t addr, uint8_t data,
		      int committed)
{
	for (int lane = 0; lane < 2; lane++)
		if (lane_mask & (1u << lane))
			cx0_write_lane(c, lane, addr, data, committed);
}

static void cx0_rmw(struct cx0 *c, uint8_t lane_mask, uint16_t addr, uint8_t clear,
		    uint8_t set, int committed)
{
	for (int lane = 0; lane < 2; lane++) {
		if (!(lane_mask & (1u << lane)))
			continue;
		uint8_t old = cx0_read_lane(c, lane, addr);
		uint8_t val = (uint8_t)((old & ~clear) | set);
		if (val != old)
			cx0_write_lane(c, lane, addr, val, committed);
	}
}

/* The C20's SRAM: a 16-bit word through the address/data window. */
static void c20_sram_write(struct cx0 *c, uint8_t lane_mask, uint16_t addr, uint16_t data)
{
	cx0_write(c, lane_mask, PHY_C20_WR_ADDRESS_H, (uint8_t)(addr >> 8), MB_WRITE_UNCOMMITTED);
	cx0_write(c, lane_mask, PHY_C20_WR_ADDRESS_L, (uint8_t)(addr & 0xff), MB_WRITE_UNCOMMITTED);
	cx0_write(c, lane_mask, PHY_C20_WR_DATA_H, (uint8_t)(data >> 8), MB_WRITE_UNCOMMITTED);
	cx0_write(c, lane_mask, PHY_C20_WR_DATA_L, (uint8_t)(data & 0xff), MB_WRITE_COMMITTED);
}

static uint16_t c20_sram_read(struct cx0 *c, uint8_t lane_mask, uint16_t addr)
{
	uint16_t val;

	cx0_write(c, lane_mask, PHY_C20_RD_ADDRESS_H, (uint8_t)(addr >> 8), MB_WRITE_UNCOMMITTED);
	cx0_write(c, lane_mask, PHY_C20_RD_ADDRESS_L, (uint8_t)(addr & 0xff), MB_WRITE_COMMITTED);
	val = cx0_read(c, lane_mask, PHY_C20_RD_DATA_H);
	val = (uint16_t)(val << 8);
	val |= cx0_read(c, lane_mask, PHY_C20_RD_DATA_L);
	return val;
}

/* Before any PHY transaction: the bus timer of both lanes programmed, so
 * a PHY that does not answer is noticed.  (The bus also needs the display
 * kept out of DC5/DC6, which this driver never enters.) */
static void cx0_transaction_begin(struct cx0 *c)
{
	for (int lane = 0; lane < 2; lane++)
		rmw(c, XELPDP_PORT_MSGBUS_TIMER(c->ver, c->port, lane),
		    XELPDP_PORT_MSGBUS_TIMER_VAL_MASK, XELPDP_PORT_MSGBUS_TIMER_VAL);
}

/* The C10's vendor registers are reached over the bus only while the
 * access bit is set; the update bit then makes the PHY take what was
 * written (with the master-lane bit for the PLL words). */
static void c10_msgbus_access_begin(struct cx0 *c, uint8_t lane_mask)
{
	if (!c->c10)
		return;
	cx0_rmw(c, lane_mask, PHY_C10_VDR_CONTROL(1), 0, C10_VDR_CTRL_MSGBUS_ACCESS,
		MB_WRITE_COMMITTED);
}

static void c10_msgbus_access_commit(struct cx0 *c, uint8_t lane_mask, int master_lane)
{
	uint8_t val = C10_VDR_CTRL_UPDATE_CFG;

	if (!c->c10)
		return;
	if (master_lane)
		val |= C10_VDR_CTRL_MASTER_LANE;
	cx0_rmw(c, lane_mask, PHY_C10_VDR_CONTROL(1), 0, val, MB_WRITE_COMMITTED);
}

/* ---- PLL arithmetic ------------------------------------------------------------- */

static uint32_t div_round_closest_u32(uint32_t x, uint32_t d)
{
	return (x + d / 2) / d;
}

static uint64_t div_round_closest_u64(uint64_t x, uint64_t d)
{
	return (x + d / 2) / d;
}

static int ilog2_u64(uint64_t v)
{
	return 63 - __builtin_clzll(v);
}

static int clock_matches(uint32_t a, uint32_t b)
{
	return (a > b ? a - b : b - a) <= 1;
}

/* The port clock a C10 PLL produces: the 38.4 MHz input times the feedback
 * multiplier (with its fraction), divided by ten and by the transmitter
 * divider, doubled for HDMI. */
static uint32_t c10_port_clock(const struct c10_pll *s)
{
	uint32_t frac_quot = 0, frac_rem = 0, frac_den = 1;
	uint32_t multiplier, tx_clk_div, hdmi_div, refclk = CX0_REFCLK_KHZ;
	uint64_t tmp;

	if (s->pll[0] & C10_PLL0_FRACEN) {
		frac_quot = (uint32_t)s->pll[12] << 8 | s->pll[11];
		frac_rem = (uint32_t)s->pll[14] << 8 | s->pll[13];
		frac_den = (uint32_t)s->pll[10] << 8 | s->pll[9];
		if (!frac_den)
			frac_den = 1;
	}
	multiplier = (((uint32_t)(s->pll[3] & C10_PLL3_MULTIPLIERH_MASK) << 8 | s->pll[2]) / 2) + 16;
	tx_clk_div = s->pll[15] & C10_PLL15_TXCLKDIV_MASK;
	hdmi_div = (s->pll[15] & C10_PLL15_HDMIDIV_MASK) >> 3;
	tmp = (uint64_t)refclk * ((multiplier << 16) + frac_quot) +
	      div_round_closest_u32(refclk * frac_rem, frac_den);
	tmp = div_round_closest_u64(tmp, (uint64_t)10 << (tx_clk_div + 16));
	return (uint32_t)tmp * (hdmi_div ? 2 : 1);
}

static int c20_uses_mpllb(const struct c20_pll *s)
{
	return !!(s->tx[0] & C20_PHY_USE_MPLLB);
}

/* The port clock a C20 context produces, from MPLLB (DP 1.4, HDMI) or
 * MPLLA (the DP 2.0 10 and 20 Gbps rates). */
static uint32_t c20_port_clock(const struct c20_pll *s)
{
	uint32_t frac, frac_en, frac_quot, frac_rem, frac_den;
	uint32_t multiplier, refclk = CX0_REFCLK_KHZ;
	uint32_t tx_clk_div, ref_clk_mpllb_div, fb_clk_div4_en;
	uint32_t ref, vco, tx_rate_mult;
	uint32_t tx_rate = s->tx[0] & C20_PHY_TX_RATE;

	if (c20_uses_mpllb(s)) {
		tx_rate_mult = 1;
		frac_en = !!(s->mpll[6] & C20_MPLLB_FRACEN);
		frac_quot = s->mpll[8];
		frac_rem = s->mpll[9];
		frac_den = s->mpll[7];
		multiplier = s->mpll[0] & C20_MULTIPLIER_MASK;
		tx_clk_div = (s->mpll[0] & C20_MPLLB_TX_CLK_DIV_MASK) >> 13;
		ref_clk_mpllb_div = (s->mpll[6] & C20_REF_CLK_MPLLB_DIV_MASK) >> 10;
		fb_clk_div4_en = 0;
	} else {
		tx_rate_mult = 2;
		frac_en = !!(s->mpll[6] & C20_MPLLA_FRACEN);
		frac_quot = s->mpll[8];
		frac_rem = s->mpll[9];
		frac_den = s->mpll[7];
		multiplier = s->mpll[0] & C20_MULTIPLIER_MASK;
		tx_clk_div = (s->mpll[1] & C20_MPLLA_TX_CLK_DIV_MASK) >> 8;
		ref_clk_mpllb_div = (s->mpll[6] & C20_REF_CLK_MPLLB_DIV_MASK) >> 10;
		fb_clk_div4_en = !!(s->mpll[0] & C20_FB_CLK_DIV4_EN);
	}
	if (frac_en)
		frac = frac_quot + (frac_den ? div_round_closest_u32(frac_rem, frac_den) : 0);
	else
		frac = 0;
	ref = div_round_closest_u32(refclk * (1u << (1 + fb_clk_div4_en)), 1u << ref_clk_mpllb_div);
	vco = (uint32_t)div_round_closest_u64(((uint64_t)ref * ((multiplier << (17 - 2)) + frac)) >> 17,
					      10);
	return vco << tx_rate_mult >> tx_clk_div >> tx_rate;
}

static int c10_state_is_dp(const struct c10_pll *s)
{
	return !(s->pll[15] & C10_PLL15_HDMIDIV_MASK);
}

/* An HDMI 2.1 FRL rate (the TMDS clocks in the same list count as FRL). */
static int hdmi_is_frl(uint32_t clock)
{
	return clock == 300000 || clock == 600000 || clock == 800000 || clock == 1000000 ||
	       clock == 1200000;
}

static int dp_is_uhbr(uint32_t rate)
{
	return rate == 1000000 || rate == 1350000 || rate == 2000000;
}

/* What DDI_CLK_VALFREQ wants: the link symbol clock (DP: a 10-bit symbol
 * per link-rate unit, 32 bits on the 128b/132b rates), the TMDS clock, or
 * an FRL rate over 18. */
static uint32_t link_symbol_clock(int is_dp, uint32_t clock)
{
	if (is_dp)
		return dp_is_uhbr(clock) ? (clock * 10 + 16) / 32 : clock;
	if (hdmi_is_frl(clock))
		return (clock * 10 + 9) / 18;
	return clock;
}

/* ---- the C10's HDMI PLL, computed ------------------------------------------- */

/* For a TMDS clock the tables do not have: the feedback multiplier and
 * fraction for the VCO, and the charge-pump settings interpolated from
 * the PHY vendor's characterisation curves.  The arithmetic is kept
 * exactly as the vendor's sequence does it (64-bit fixed point, the
 * 32-bit truncation of the long divisions included), so the values match
 * what has been validated on hardware. */

/* x axis: VCO frequencies, one curve per v2i point */
static const uint64_t c10_curve_freq_hz[2][8] = {
	{ 2500000000ULL, 3000000000ULL, 3000000000ULL, 3500000000ULL, 3500000000ULL,
	  4000000000ULL, 4000000000ULL, 5000000000ULL },
	{ 4000000000ULL, 4600000000ULL, 4601000000ULL, 5400000000ULL, 5401000000ULL,
	  6600000000ULL, 6601000000ULL, 8001000000ULL }
};

/* y axis heights, times 1000000000 */
static const uint64_t c10_curve_0[2][8] = {
	{ 41174500, 48605500, 42973700, 49433100, 42408600, 47681900, 40297400, 49131400 },
	{ 82056800, 94420700, 82323400, 96370600, 81273300, 98630100, 81728700, 99105700 }
};

static const uint64_t c10_curve_1[2][8] = {
	{ 73300000000000ULL, 66000000000000ULL, 83100000000000ULL, 75300000000000ULL,
	  99700000000000ULL, 92300000000000ULL, 125000000000000ULL, 110000000000000ULL },
	{ 53700000000000ULL, 47700000000000ULL, 62200000000000ULL, 54400000000000ULL,
	  75100000000000ULL, 63400000000000ULL, 90600000000000ULL, 76300000000000ULL }
};

/* times 1000000000000 */
static const uint64_t c10_curve_2[2][8] = {
	{ 2415790000ULL, 3136460000ULL, 2581990000ULL, 3222670000ULL, 2529330000ULL,
	  3042020000ULL, 2336970000ULL, 3191460000ULL },
	{ 4808390000ULL, 5994250000ULL, 4832730000ULL, 6193730000ULL, 4737700000ULL,
	  6428750000ULL, 4779200000ULL, 6479340000ULL }
};

#define SNPS_HDMI_4999MHZ 4999999900ULL
#define SNPS_HDMI_9999MHZ (2 * SNPS_HDMI_4999MHZ)
#define SNPS_HDMI_16GHZ 16000000000ULL
#define SNPS_CURVE0_MULTIPLIER 1000000000ULL
#define SNPS_CURVE1_MULTIPLIER 100ULL
#define SNPS_CURVE2_MULTIPLIER 1000000000000ULL

/* A division whose divisor the hardware sequence takes as 32 bits. */
static uint64_t div_u64_by_u32(uint64_t n, uint64_t base)
{
	uint32_t b = (uint32_t)base;
	return b ? n / b : 0;
}

/* Rounding-up division in wrapping 64-bit arithmetic. */
static uint64_t div_round_up_u64(uint64_t n, uint64_t d)
{
	return d ? (n + d - 1) / d : 0;
}

static uint64_t isqrt_u64(uint64_t x)
{
	uint64_t r = 0, bit = 1ULL << 62;

	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* Linear interpolation between two curve points, slope in 1/100000
 * steps (two's-complement wrapping, as the sequence computes it). */
static uint64_t snps_interp(uint64_t x, uint64_t x1, uint64_t x2, uint64_t y1, uint64_t y2)
{
	uint64_t dydx = div_round_up_u64((y2 - y1) * 100000ULL, x2 - x1);
	return y1 + div_round_up_u64(dydx * (x - x1), 100000ULL);
}

static void snps_ana_cp(uint64_t vco_clk, uint32_t refclk_postscalar, int mpll_ana_v2i,
			int c, int a, uint32_t *ana_cp_int, uint32_t *ana_cp_prop)
{
	uint64_t vco_div_refclk_float, c0, c1, c2, temp;
	uint64_t c2_scaled1, c2_scaled2, svd1, svd2, adj1, adj2, cp_int, cp_prop;
	uint64_t c2_scaled_int, product, sq;

	vco_div_refclk_float = vco_clk * (1000000000000ULL / refclk_postscalar);
	c0 = snps_interp(vco_clk, c10_curve_freq_hz[c][a], c10_curve_freq_hz[c][a + 1],
			 c10_curve_0[c][a], c10_curve_0[c][a + 1]);
	c2 = snps_interp(vco_clk, c10_curve_freq_hz[c][a], c10_curve_freq_hz[c][a + 1],
			 c10_curve_2[c][a], c10_curve_2[c][a + 1]);
	c1 = snps_interp(vco_clk, c10_curve_freq_hz[c][a], c10_curve_freq_hz[c][a + 1],
			 c10_curve_1[c][a], c10_curve_1[c][a + 1]);
	c1 = c1 / SNPS_CURVE1_MULTIPLIER;

	/* curve 2 scaled by v2i, for the integral and the proportional part */
	temp = c2 * (uint64_t)(4 - mpll_ana_v2i);
	c2_scaled1 = temp / 16000;
	c2_scaled2 = temp / 160;

	svd1 = 112008301ULL * (vco_div_refclk_float / 100000);
	adj1 = SNPS_CURVE2_MULTIPLIER *
	       div_u64_by_u32(svd1, c0 * (c1 / SNPS_CURVE0_MULTIPLIER));
	cp_int = div_round_closest_u64(div_u64_by_u32(adj1, c2_scaled1), SNPS_CURVE2_MULTIPLIER);
	cp_int = cp_int < 1 ? 1 : cp_int > 127 ? 127 : cp_int;
	*ana_cp_int = (uint32_t)cp_int;

	c2_scaled_int = c2_scaled1 * cp_int;
	product = c1 * (c2_scaled_int * (c0 / SNPS_CURVE0_MULTIPLIER));
	sq = isqrt_u64(div_round_up_u64(product, vco_div_refclk_float) *
		       (1000000000000ULL / 55));
	svd2 = (vco_div_refclk_float + 1000000 - 1) / 1000000;
	adj2 = 1460281ULL * div_round_up_u64(sq * svd2, c1);
	cp_prop = div_round_up_u64(adj2, c2_scaled2);
	cp_prop = cp_prop < 1 ? 1 : cp_prop > 127 ? 127 : cp_prop;
	*ana_cp_prop = (uint32_t)cp_prop;
}

static void c10_compute_hdmi(struct c10_pll *s, uint64_t pixel_clock)
{
	const uint32_t refclk = 38400000, prescaler_divider = 0;
	const uint32_t ana_cp_int_gs = 30, ana_cp_prop_gs = 28;
	const uint32_t ssc_up_spread = 1, mpll_div5_en = 1, hdmi_div = 1;
	uint64_t datarate = pixel_clock * 10000;
	uint32_t refclk_postscalar = refclk >> prescaler_divider;
	uint32_t tx_clk_div, vco_div_refclk_integer, vco_div_refclk_fracn;
	uint32_t fracn_quot, fracn_rem, fracn_den, fracn_en, pmix_en, multiplier;
	uint32_t ana_cp_int, ana_cp_prop;
	uint64_t vco_clk, vco_rem;
	int mpll_ana_v2i, ana_freq_vco = 0, c, a = 0;

	/* the v2i point by data rate */
	if (datarate <= SNPS_HDMI_9999MHZ) {
		mpll_ana_v2i = 2;
		tx_clk_div = (uint32_t)ilog2_u64(SNPS_HDMI_9999MHZ / datarate);
	} else {
		mpll_ana_v2i = 3;
		tx_clk_div = (uint32_t)ilog2_u64(SNPS_HDMI_16GHZ / datarate);
	}
	vco_clk = (datarate << tx_clk_div) >> 1;

	vco_div_refclk_integer = (uint32_t)(vco_clk / refclk_postscalar);
	vco_rem = vco_clk % refclk_postscalar;
	/* From here on the sequence works with the quotient of that division
	 * in place of the VCO frequency (the band and the charge-pump curves
	 * are evaluated on it). */
	vco_clk = vco_clk / refclk_postscalar;
	vco_div_refclk_fracn = (uint32_t)((vco_rem << 32) / refclk_postscalar);

	fracn_quot = vco_div_refclk_fracn >> 16;
	fracn_rem = vco_div_refclk_fracn & 0xffff;
	fracn_rem = fracn_rem - (fracn_rem >> 15);
	fracn_den = 0xffff;
	fracn_en = (fracn_quot != 0 || fracn_rem != 0) ? 1 : 0;
	pmix_en = fracn_en;
	multiplier = (vco_div_refclk_integer - 16) * 2;

	/* one curve per v2i range; find the segment */
	c = mpll_ana_v2i - 2;
	for (int i = 0; i < 8; i += 2) {
		if (vco_clk <= c10_curve_freq_hz[c][i + 1]) {
			a = i;
			ana_freq_vco = 3 - (a >> 1);
			break;
		}
	}
	snps_ana_cp(vco_clk, refclk_postscalar, mpll_ana_v2i, c, a, &ana_cp_int, &ana_cp_prop);

	s->tx = 0x10;
	s->cmn = 0x1;
	for (int i = 0; i < 20; i++)
		s->pll[i] = 0;
	s->pll[0] = (uint8_t)((mpll_div5_en ? C10_PLL0_DIV5CLK_EN : 0) |
			      (fracn_en ? C10_PLL0_FRACEN : 0) | (pmix_en ? C10_PLL0_PMIX_EN : 0) |
			      (((uint32_t)ana_freq_vco << 6) & C10_PLL0_ANA_FREQ_VCO_MASK));
	s->pll[2] = (uint8_t)(multiplier & C10_PLL2_MULTIPLIERL_MASK);
	s->pll[3] = (uint8_t)((multiplier >> 8) & C10_PLL3_MULTIPLIERH_MASK);
	s->pll[8] = ssc_up_spread ? C10_PLL8_SSC_UP_SPREAD : 0;
	s->pll[9] = (uint8_t)(fracn_den & 0xff);
	s->pll[10] = (uint8_t)((fracn_den >> 8) & 0xff);
	s->pll[11] = (uint8_t)(fracn_quot & 0xff);
	s->pll[12] = (uint8_t)((fracn_quot >> 8) & 0xff);
	s->pll[13] = (uint8_t)(fracn_rem & 0xff);
	s->pll[14] = (uint8_t)((fracn_rem >> 8) & 0xff);
	s->pll[15] = (uint8_t)((tx_clk_div & C10_PLL15_TXCLKDIV_MASK) |
			       ((hdmi_div << 3) & C10_PLL15_HDMIDIV_MASK));
	s->pll[16] = (uint8_t)((ana_cp_int & C10_PLL16_ANA_CPINT) |
			       ((ana_cp_int_gs << 7) & C10_PLL16_ANA_CPINTGS_L));
	s->pll[17] = (uint8_t)(((ana_cp_int_gs >> 1) & C10_PLL17_ANA_CPINTGS_H_MASK) |
			       ((ana_cp_prop << 6) & C10_PLL17_ANA_CPPROP_L_MASK));
	s->pll[18] = (uint8_t)(((ana_cp_prop >> 2) & C10_PLL18_ANA_CPPROP_H_MASK) |
			       ((ana_cp_prop_gs << 5) & C10_PLL18_ANA_CPPROPGS_L_MASK));
	s->pll[19] = (uint8_t)(((ana_cp_prop_gs >> 3) & C10_PLL19_ANA_CPPROPGS_H_MASK) |
			       (((uint32_t)mpll_ana_v2i << 4) & C10_PLL19_ANA_V2I_MASK));
}

/* ---- the C20's HDMI (TMDS) PLL, computed ------------------------------------- */

/* Some Arrow Lake S parts share their graphics device id with other Arrow
 * Lake parts; the host bridge tells them apart. */
static int is_arrowlake_s_by_host_bridge(void)
{
	uint16_t id = (uint16_t)(pci_cfg_read32(0, 0, 0, 0) >> 16);
	return id == 0x7D1C || id == 0x7D2D || id == 0x7D2E || id == 0x7D2F;
}

static int is_meteorlake_u(struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_METEORLAKE &&
	       (i915->devid == 0x7D40 || i915->devid == 0x7D45);
}

/* Transmitter word 1 for TMDS: the misc field depends on the part. */
static uint16_t c20_hdmi_tmds_tx_cfg_1(struct i915_device *i915)
{
	uint32_t tx_misc, tx_dcc_cal_dac_ctrl_range = 8, tx_term_ctrl = 2;

	if (cx0_display_ver(i915) >= 20) {
		tx_misc = 5;
		tx_term_ctrl = 4;
	} else if (cx0_is_xe2hpd(i915)) {
		tx_misc = 0;
	} else if (is_meteorlake_u(i915) || is_arrowlake_s_by_host_bridge()) {
		tx_misc = 3;
	} else {
		tx_misc = 7;
	}
	return (uint16_t)(C20_PHY_TX_MISC(tx_misc) |
			  C20_PHY_TX_DCC_CAL_RANGE(tx_dcc_cal_dac_ctrl_range) |
			  C20_PHY_TX_DCC_BYPASS | C20_PHY_TX_TERM_CTL(tx_term_ctrl));
}

#define C20_REFCLK_HZ 38400000ULL
#define C20_CLOCK_4999MHZ 4999999999ULL
#define C20_CLOCK_9999MHZ 9999999999ULL
#define C20_DATARATE_3000M 3000000000ULL
#define C20_DATARATE_3500M 3500000000ULL
#define C20_DATARATE_4000M 4000000000ULL

/* The MPLLB for a TMDS clock between 25.175 and 600 MHz: the transmitter
 * divider and VCO shift by data rate, the feedback multiplier with a
 * 16-bit fraction, the VCO band. */
static int c20_compute_hdmi(struct i915_device *i915, uint32_t port_clock, struct c20_pll *s)
{
	uint64_t datarate, mpll_tx_clk_div, vco_freq_shift, vco_freq, multiplier;
	uint64_t mpll_multiplier, mpll_fracn_quot, mpll_fracn_rem;
	uint32_t mpllb_ana_freq_vco;
	uint8_t mpll_div_multiplier;

	if (port_clock < 25175 || port_clock > 600000)
		return -EINVAL;

	datarate = (uint64_t)port_clock * 1000 * 10;
	mpll_tx_clk_div = (uint64_t)ilog2_u64(C20_CLOCK_9999MHZ / datarate);
	vco_freq_shift = (uint64_t)ilog2_u64(C20_CLOCK_4999MHZ * 256 / datarate);
	vco_freq = (datarate << vco_freq_shift) >> 8;
	multiplier = (vco_freq << 28) / (C20_REFCLK_HZ >> 4);
	mpll_multiplier = 2 * (multiplier >> 32);
	mpll_fracn_quot = (multiplier >> 16) & 0xffff;
	mpll_fracn_rem = multiplier & 0xffff;
	/* an 8-bit field: the value is taken modulo 256, then capped */
	mpll_div_multiplier = (uint8_t)((vco_freq * 16 + (datarate >> 1)) / datarate);

	if (vco_freq <= C20_DATARATE_3000M)
		mpllb_ana_freq_vco = 3;
	else if (vco_freq <= C20_DATARATE_3500M)
		mpllb_ana_freq_vco = 2;
	else if (vco_freq <= C20_DATARATE_4000M)
		mpllb_ana_freq_vco = 1;
	else
		mpllb_ana_freq_vco = 0;

	s->tx[0] = 0xbe88;
	s->tx[1] = c20_hdmi_tmds_tx_cfg_1(i915);
	s->tx[2] = 0x0000;
	s->cmn[0] = 0x0500;
	s->cmn[1] = 0x0005;
	s->cmn[2] = 0x0000;
	s->cmn[3] = 0x0000;
	s->mpll[0] = (uint16_t)(C20_MPLL_TX_CLK_DIV(mpll_tx_clk_div) |
				C20_MPLL_MULTIPLIER(mpll_multiplier));
	s->mpll[1] = (uint16_t)(C20_CAL_DAC_CODE(31) | C20_WORD_CLK_DIV |
				C20_MPLL_DIV_MULTIPLIER(mpll_div_multiplier));
	s->mpll[2] = (uint16_t)(C20_MPLLB_ANA_FREQ_VCO(mpllb_ana_freq_vco) | C20_CP_PROP(20) |
				C20_CP_INT(6));
	s->mpll[3] = (uint16_t)(C20_V2I(2) | C20_CP_PROP_GS(30) | C20_CP_INT_GS(28));
	s->mpll[4] = 0x0000;
	s->mpll[5] = 0x0000;
	s->mpll[6] = (uint16_t)(C20_MPLLB_FRACEN | C20_SSC_UP_SPREAD);
	s->mpll[7] = 0xffff;
	s->mpll[8] = (uint16_t)mpll_fracn_quot;
	s->mpll[9] = (uint16_t)mpll_fracn_rem;
	s->mpll[10] = (uint16_t)C20_HDMI_DIV(1);
	return 0;
}

/* ---- the C20's vendor registers --------------------------------------------- */

static uint8_t c20_dp_rate(uint32_t clock)
{
	switch (clock) {
	case 162000: return 0;
	case 270000: return 1;
	case 540000: return 2;
	case 810000: return 3;
	case 216000: return 4;
	case 243000: return 5;
	case 324000: return 6;
	case 432000: return 7;
	case 1000000: return 8;
	case 1350000: return 9;
	case 2000000: return 10;
	case 648000: return 11;
	case 675000: return 12;
	default: return 0;
	}
}

static uint8_t c20_hdmi_rate(uint32_t clock)
{
	if (clock >= 25175 && clock <= 600000)
		return 0;
	switch (clock) {
	case 300000:
	case 600000:
	case 1200000:
		return 1;
	case 800000:
		return 2;
	case 1000000:
		return 3;
	default:
		return 0;
	}
}

/* Data width (0: 10-bit, 1: HDMI FRL, 2: DP 2.0), protocol and rate. */
static void c20_calc_vdr(struct c20_vdr *vdr, int is_dp, uint32_t clock)
{
	if (is_dp && dp_is_uhbr(clock))
		vdr->custom_width = 2;
	else if (hdmi_is_frl(clock))
		vdr->custom_width = 1;
	else
		vdr->custom_width = 0;
	vdr->serdes_rate = 0;
	vdr->hdmi_rate = 0;
	if (is_dp) {
		vdr->serdes_rate = (uint8_t)(PHY_C20_IS_DP | PHY_C20_DP_RATE(c20_dp_rate(clock)));
	} else {
		if (hdmi_is_frl(clock))
			vdr->serdes_rate = PHY_C20_IS_HDMI_FRL;
		vdr->hdmi_rate = c20_hdmi_rate(clock);
	}
}

static void c20_program_vdr(struct cx0 *c, const struct c20_vdr *vdr, uint8_t owned)
{
	cx0_rmw(c, owned, PHY_C20_VDR_CUSTOM_WIDTH, PHY_C20_CUSTOM_WIDTH_MASK,
		vdr->custom_width & PHY_C20_CUSTOM_WIDTH_MASK, MB_WRITE_COMMITTED);
	cx0_rmw(c, owned, PHY_C20_VDR_CUSTOM_SERDES_RATE, PHY_C20_SERDES_RATE_MASK,
		vdr->serdes_rate & PHY_C20_SERDES_RATE_MASK, MB_WRITE_COMMITTED);
	if (vdr->serdes_rate & PHY_C20_IS_DP)
		return;
	cx0_rmw(c, CX0_BOTH_LANES, PHY_C20_VDR_HDMI_RATE, PHY_C20_HDMI_RATE_MASK,
		vdr->hdmi_rate & PHY_C20_HDMI_RATE_MASK, MB_WRITE_COMMITTED);
}

/* ---- the tables a port uses -------------------------------------------------- */

static const struct c20_entry *c20_tables(struct cx0 *c, int is_dp)
{
	if (!is_dp)
		return c20_hdmi_tables;
	if (c->is_edp) {
		if (mtl_edp_typec)
			return xe3lpd_c20_dp_edp_tables;
		if (c->xe2hpd)
			return xe2hpd_c20_edp_tables;
	}
	if (c->ver >= 30)
		return xe3lpd_c20_dp_edp_tables;
	if (c->xe2hpd)
		return xe2hpd_c20_dp_tables;
	return c20_dp_tables;
}

static const struct c10_entry *c10_find(const struct c10_entry *t, uint32_t clock)
{
	for (; t->state; t++)
		if (clock_matches(clock, c10_port_clock(t->state)))
			return t;
	return NULL;
}

static const struct c20_entry *c20_find(const struct c20_entry *t, uint32_t clock)
{
	for (; t->state; t++)
		if (clock_matches(clock, c20_port_clock(t->state)))
			return t;
	return NULL;
}

/* Without spread spectrum the C10's spread words are zero. */
static void c10_drop_ssc(struct c10_pll *s, int ssc)
{
	if (ssc)
		return;
	for (int i = C10_PLL_SSC_REG_START_IDX;
	     i < C10_PLL_SSC_REG_START_IDX + C10_PLL_SSC_REG_COUNT; i++)
		s->pll[i] = 0;
}

static void mirror_dpll(struct i915_device *i915, int port, const struct cx0_port_pll *p)
{
	struct intel_dpll_state *d;

	if (port >= INTEL_MAX_DPLLS)
		return;
	d = &i915->display.dpll[port];
	d->in_use = p->in_use;
	d->is_hdmi = p->in_use && !p->is_dp;
	d->ssc = p->ssc;
	d->link_rate_khz = p->in_use ? p->port_clock : 0;
}

/* The DisplayPort 8b/10b rates a port's PHY has PLL settings for. */
int mtl_phy_rate_supported(struct i915_device *i915, int port, uint32_t link_rate_khz)
{
	struct cx0 c;

	if (intel_has_lt_phy(i915))
		return lt_phy_rate_supported(i915, port, link_rate_khz);
	if (!port_ok(port) || dp_is_uhbr(link_rate_khz))
		return 0;
	cx0_setup(&c, i915, port, NULL);
	if (c.c10) {
		/* the eDP table is the DisplayPort one plus the eDP rates */
		for (const struct c10_entry *t = c10_edp_tables; t->state; t++)
			if (t->clock == link_rate_khz)
				return 1;
		return 0;
	}
	for (int edp = 0; edp < 2; edp++) {
		c.is_edp = edp;
		for (const struct c20_entry *t = c20_tables(&c, 1); t->state; t++)
			if (t->clock == link_rate_khz)
				return 1;
	}
	return 0;
}

static int tbt_rate_ok(struct cx0 *c, uint32_t rate)
{
	switch (rate) {
	case 162000:
	case 270000:
	case 540000:
	case 810000:
		return 1;
	case 1000000:
	case 2000000:
		return c->ver >= 30;
	default:
		return 0;
	}
}

int mtl_phy_pll_get_dp(struct i915_device *i915, struct intel_output *o,
		       uint32_t link_rate_khz, int ssc)
{
	struct cx0 c;
	struct cx0_port_pll n, *p = &n;

	if (intel_has_lt_phy(i915))
		return lt_phy_pll_get_dp(i915, o, link_rate_khz, ssc);
	if (!o || !port_ok(o->port))
		return -EINVAL;
	cx0_setup(&c, i915, o->port, o);
	mm_memset(p, 0, sizeof(*p));
	p->is_dp = 1;
	p->ssc = !!ssc;
	p->is_c10 = c.c10;

	if (c.tbt) {
		if (!tbt_rate_ok(&c, link_rate_khz)) {
			kprintf("[drm] i915: port %s: no Thunderbolt clock for %u kHz\n", c.name,
				link_rate_khz);
			return -EINVAL;
		}
		p->tbt = 1;
		p->ssc = 0;
		p->port_clock = link_rate_khz;
	} else if (c.c10) {
		const struct c10_entry *e =
			c10_find(c.is_edp ? c10_edp_tables : c10_dp_tables, link_rate_khz);
		if (!e) {
			kprintf("[drm] i915: port %s: no C10 PLL settings for %u kHz\n", c.name,
				link_rate_khz);
			return -EINVAL;
		}
		mm_memcpy(&p->c10, e->state, sizeof(p->c10));
		c10_drop_ssc(&p->c10, p->ssc);
		p->port_clock = c10_port_clock(&p->c10);
	} else {
		const struct c20_entry *e = c20_find(c20_tables(&c, 1), link_rate_khz);
		if (!e) {
			kprintf("[drm] i915: port %s: no C20 PLL settings for %u kHz\n", c.name,
				link_rate_khz);
			return -EINVAL;
		}
		mm_memcpy(&p->c20, e->state, sizeof(p->c20));
		c20_calc_vdr(&p->vdr, 1, link_rate_khz);
		p->port_clock = c20_port_clock(&p->c20);
	}
	p->in_use = 1;
	mm_memcpy(&mtl_pll[o->port], p, sizeof(*p));
	mirror_dpll(i915, o->port, p);
	i915_dbg("[drm] i915: port %s: %s PLL for DP at %u kHz%s\n", c.name,
		 p->tbt ? "Thunderbolt" : c.c10 ? "C10" : "C20", p->port_clock,
		 p->ssc ? " with SSC" : "");
	return o->port;
}

int mtl_phy_pll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz)
{
	struct cx0 c;
	struct cx0_port_pll n, *p = &n;

	if (intel_has_lt_phy(i915))
		return lt_phy_pll_get_hdmi(i915, o, clock_khz);
	if (!o || !port_ok(o->port))
		return -EINVAL;
	cx0_setup(&c, i915, o->port, o);
	if (c.tbt)
		return -EINVAL; /* no TMDS through Thunderbolt */
	mm_memset(p, 0, sizeof(*p));
	p->is_c10 = c.c10;
	if (c.c10) {
		/* the tables first; the computed values are correct but may
		 * not be the best tuned */
		const struct c10_entry *e = c10_find(c10_hdmi_tables, clock_khz);
		if (e)
			mm_memcpy(&p->c10, e->state, sizeof(p->c10));
		else
			c10_compute_hdmi(&p->c10, clock_khz);
		c10_drop_ssc(&p->c10, 0);
		p->port_clock = c10_port_clock(&p->c10);
	} else {
		const struct c20_entry *e = c20_find(c20_tables(&c, 0), clock_khz);
		if (e) {
			mm_memcpy(&p->c20, e->state, sizeof(p->c20));
		} else if (c20_compute_hdmi(i915, clock_khz, &p->c20)) {
			kprintf("[drm] i915: port %s: no C20 PLL settings for TMDS %u kHz\n",
				c.name, clock_khz);
			return -EINVAL;
		}
		c20_calc_vdr(&p->vdr, 0, clock_khz);
		p->port_clock = c20_port_clock(&p->c20);
	}
	p->in_use = 1;
	mm_memcpy(&mtl_pll[o->port], p, sizeof(*p));
	mirror_dpll(i915, o->port, p);
	i915_dbg("[drm] i915: port %s: %s PLL for TMDS %u kHz (asked %u)\n", c.name,
		 c.c10 ? "C10" : "C20", p->port_clock, clock_khz);
	return o->port;
}

int mtl_phy_hdmi_clock_ok(struct i915_device *i915, const struct intel_output *o,
			  uint32_t clock_khz)
{
	struct cx0 c;
	struct c20_pll s;

	/* the LT PHYs decide when their PLL is asked for */
	if (intel_has_lt_phy(i915))
		return 1;
	if (!o || !port_ok(o->port) || !clock_khz)
		return 0;
	cx0_setup(&c, i915, o->port, o);
	if (c.tbt)
		return 0; /* no TMDS through Thunderbolt */
	/* the C10's settings are computed for any clock its table lacks */
	if (c.c10)
		return 1;
	if (c20_find(c20_tables(&c, 0), clock_khz))
		return 1;
	return c20_compute_hdmi(i915, clock_khz, &s) == 0;
}

void mtl_phy_pll_put(struct i915_device *i915, int pll)
{
	if (intel_has_lt_phy(i915)) {
		lt_phy_pll_put(i915, pll);
		return;
	}
	if (!port_ok(pll))
		return;
	mtl_pll[pll].in_use = 0;
	mirror_dpll(i915, pll, &mtl_pll[pll]);
}

/* ---- port sequences ---------------------------------------------------------- */

static uint32_t clock_ctl(struct cx0 *c)
{
	return XELPDP_PORT_CLOCK_CTL(c->ver, c->port);
}

static uint32_t buf_ctl1(struct cx0 *c)
{
	return XELPDP_PORT_BUF_CTL1_REG(c->ver, c->port);
}

static uint32_t buf_ctl2(struct cx0 *c)
{
	return XELPDP_PORT_BUF_CTL2(c->ver, c->port);
}

static uint32_t lanes_bits(uint8_t lane_mask, uint32_t lane0, uint32_t lane1)
{
	return ((lane_mask & CX0_LANE0) ? lane0 : 0) | ((lane_mask & CX0_LANE1) ? lane1 : 0);
}

static uint32_t powerdown_update(uint8_t lane_mask)
{
	return lanes_bits(lane_mask, XELPDP_LANE_POWERDOWN_UPDATE(0),
			  XELPDP_LANE_POWERDOWN_UPDATE(1));
}

static uint32_t powerdown_state(uint8_t lane_mask, uint8_t state)
{
	return lanes_bits(lane_mask, XELPDP_LANE_POWERDOWN_NEW_STATE(0, state),
			  XELPDP_LANE_POWERDOWN_NEW_STATE(1, state));
}

static uint32_t pclk_refclk_request(uint8_t lane_mask)
{
	return lanes_bits(lane_mask, XELPDP_LANE_PCLK_REFCLK_REQUEST(0),
			  XELPDP_LANE_PCLK_REFCLK_REQUEST(1));
}

static uint32_t pclk_refclk_ack(uint8_t lane_mask)
{
	return lanes_bits(lane_mask, XELPDP_LANE_PCLK_REFCLK_ACK(0), XELPDP_LANE_PCLK_REFCLK_ACK(1));
}

static uint32_t pclk_pll_request(uint8_t lane_mask)
{
	return lanes_bits(lane_mask, XELPDP_LANE_PCLK_PLL_REQUEST(0),
			  XELPDP_LANE_PCLK_PLL_REQUEST(1));
}

static uint32_t pclk_pll_ack(uint8_t lane_mask)
{
	return lanes_bits(lane_mask, XELPDP_LANE_PCLK_PLL_ACK(0), XELPDP_LANE_PCLK_PLL_ACK(1));
}

/* Move the lanes to a new power-down state: the state, then (with no
 * bus transaction in flight) the update strobe, which the hardware
 * clears once the lanes are there. */
static void cx0_powerdown_change(struct cx0 *c, uint8_t lane_mask, uint8_t state)
{
	rmw(c, buf_ctl2(c), powerdown_state(CX0_BOTH_LANES, XELPDP_LANE_POWERDOWN_NEW_STATE_MASK),
	    powerdown_state(lane_mask, state));
	for (int lane = 0; lane < 2; lane++) {
		if (!(lane_mask & (1u << lane)))
			continue;
		if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
			       XELPDP_MSGBUS_TIMEOUT_US)) {
			i915_dbg("[drm] i915: port %s: PHY transaction pending at power change, resetting the bus\n",
				 c->name);
			cx0_bus_reset(c, lane);
		}
	}
	rmw(c, buf_ctl2(c), powerdown_update(CX0_BOTH_LANES), powerdown_update(lane_mask));
	if (wait_clear(c, buf_ctl2(c), powerdown_update(lane_mask),
		       XELPDP_PORT_POWERDOWN_UPDATE_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY did not change power state (to %u)\n", c->name,
			(unsigned)state);
}

/* Which state "ready" and "active" are, and no lane stagger. */
static void cx0_setup_powerdown(struct cx0 *c)
{
	rmw(c, buf_ctl2(c), XELPDP_POWER_STATE_READY_MASK,
	    XELPDP_POWER_STATE_READY(XELPDP_P2_STATE_READY));
	rmw(c, XELPDP_PORT_BUF_CTL3(c->ver, c->port),
	    XELPDP_POWER_STATE_ACTIVE_MASK | XELPDP_PLL_LANE_STAGGERING_DELAY_MASK,
	    XELPDP_POWER_STATE_ACTIVE(XELPDP_P0_STATE_ACTIVE) | XELPDP_PLL_LANE_STAGGERING_DELAY(0));
}

/* Out of reset: wait for the SoC side of the PHY, put the owned lanes'
 * pipes in reset, request a reference clock on the lane that carries
 * the clock, set the reset power state, and release the pipes. */
static void cx0_lane_reset(struct cx0 *c)
{
	uint8_t owned = c->owned;
	uint8_t lane_mask = c->reversal ? CX0_LANE1 : CX0_LANE0;
	uint32_t pipe_reset = owned == CX0_BOTH_LANES ?
			      XELPDP_LANE_PIPE_RESET(0) | XELPDP_LANE_PIPE_RESET(1) :
			      XELPDP_LANE_PIPE_RESET(0);
	uint32_t status = owned == CX0_BOTH_LANES ?
			  XELPDP_LANE_PHY_CURRENT_STATUS(0) | XELPDP_LANE_PHY_CURRENT_STATUS(1) :
			  XELPDP_LANE_PHY_CURRENT_STATUS(0);

	if (wait_set(c, buf_ctl1(c), XELPDP_PORT_BUF_SOC_PHY_READY,
		     XELPDP_PORT_BUF_SOC_READY_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY did not come out of SoC reset\n", c->name);

	rmw(c, buf_ctl2(c), pipe_reset, pipe_reset);
	if (wait_set(c, buf_ctl2(c), status, XELPDP_PORT_RESET_START_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY lanes did not enter reset\n", c->name);

	rmw(c, clock_ctl(c), pclk_refclk_request(owned), pclk_refclk_request(lane_mask));
	if (wait_reg(c, clock_ctl(c), pclk_refclk_ack(owned), pclk_refclk_ack(lane_mask),
		     XELPDP_REFCLK_ENABLE_TIMEOUT_US, NULL))
		kprintf("[drm] i915: port %s: PHY reference clock request not acknowledged\n",
			c->name);

	cx0_powerdown_change(c, CX0_BOTH_LANES, XELPDP_P2_STATE_RESET);
	cx0_setup_powerdown(c);

	rmw(c, buf_ctl2(c), pipe_reset, 0);
	if (wait_clear(c, buf_ctl2(c), status, XELPDP_PORT_RESET_END_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY lanes did not leave reset\n", c->name);
}

/* Enable the transmitters the link uses and disable the others (only on
 * the PHY lanes the display owns). */
static void cx0_program_phy_lane(struct cx0 *c, int lane_count)
{
	uint8_t owned = c->owned;
	uint8_t disables;

	c10_msgbus_access_begin(c, owned);
	if (c->reversal)
		disables = (uint8_t)(0xfu >> lane_count);
	else
		disables = (uint8_t)(0xfu << lane_count);
	/* one lane in DP-alt mode goes out on the second transmitter */
	if (c->dp_alt && lane_count == 1) {
		disables &= (uint8_t)~0x3u;
		disables |= 0x1;
	}
	for (int i = 0; i < 4; i++) {
		int tx = i % 2 + 1;
		uint8_t lane_mask = i < 2 ? CX0_LANE0 : CX0_LANE1;

		if (!(owned & lane_mask))
			continue;
		cx0_rmw(c, lane_mask, PHY_CX0_TX_CONTROL(tx, 2), CONTROL2_DISABLE_SINGLE_TX,
			(disables & (1u << i)) ? CONTROL2_DISABLE_SINGLE_TX : 0,
			MB_WRITE_COMMITTED);
	}
	c10_msgbus_access_commit(c, owned, 0);
}

/* The C10 PLL: the words go to lane 0 (the master lane), the data width
 * to both, then the update. */
static void c10_pll_program(struct cx0 *c, const struct c10_pll *s)
{
	c10_msgbus_access_begin(c, CX0_BOTH_LANES);
	for (int i = 0; i < 20; i++)
		cx0_write(c, CX0_LANE0, (uint16_t)PHY_C10_VDR_PLL(i), s->pll[i],
			  (i % 4) ? MB_WRITE_UNCOMMITTED : MB_WRITE_COMMITTED);
	cx0_write(c, CX0_LANE0, PHY_C10_VDR_CMN(0), s->cmn, MB_WRITE_COMMITTED);
	cx0_write(c, CX0_LANE0, PHY_C10_VDR_TX(0), s->tx, MB_WRITE_COMMITTED);
	cx0_rmw(c, CX0_BOTH_LANES, PHY_C10_VDR_CUSTOM_WIDTH, C10_VDR_CUSTOM_WIDTH_MASK,
		C10_VDR_CUSTOM_WIDTH_8_10, MB_WRITE_COMMITTED);
	c10_msgbus_access_commit(c, CX0_LANE0, 1);
}

/* The C20 PLL: written into the context not in use, then the PHY told
 * to switch.  A legacy Type-C port switching between HDMI and DP first
 * clears the MPLLB calibration banks. */
static void c20_pll_program(struct cx0 *c, const struct c20_pll *s, const struct c20_vdr *vdr)
{
	uint8_t owned = c->owned;
	int cntx, i;

	cntx = !!(cx0_read(c, CX0_LANE0, PHY_C20_VDR_CUSTOM_SERDES_RATE) & PHY_C20_CONTEXT_TOGGLE);

	/* (not in DP-alt or Thunderbolt mode: the banks stay) */
	if (c->legacy) {
		for (i = 0; i < 4; i++)
			c20_sram_write(c, CX0_LANE0,
				       (uint16_t)RAWLANEAONX_DIG_TX_MPLLB_CAL_DONE_BANK(i), 0);
		lapic_delay_us(4000);
	}
	for (i = 0; i < 3; i++)
		c20_sram_write(c, CX0_LANE0,
			       (uint16_t)(cntx ? PHY_C20_A_TX_CNTX_CFG(c->xe2hpd, i) :
						 PHY_C20_B_TX_CNTX_CFG(c->xe2hpd, i)),
			       s->tx[i]);
	for (i = 0; i < 4; i++)
		c20_sram_write(c, CX0_LANE0,
			       (uint16_t)(cntx ? PHY_C20_A_CMN_CNTX_CFG(c->xe2hpd, i) :
						 PHY_C20_B_CMN_CNTX_CFG(c->xe2hpd, i)),
			       s->cmn[i]);
	if (c20_uses_mpllb(s)) {
		for (i = 0; i < 11; i++)
			c20_sram_write(c, CX0_LANE0,
				       (uint16_t)(cntx ? PHY_C20_A_MPLLB_CNTX_CFG(c->xe2hpd, i) :
							 PHY_C20_B_MPLLB_CNTX_CFG(c->xe2hpd, i)),
				       s->mpll[i]);
	} else {
		for (i = 0; i < 10; i++)
			c20_sram_write(c, CX0_LANE0,
				       (uint16_t)(cntx ? PHY_C20_A_MPLLA_CNTX_CFG(c->xe2hpd, i) :
							 PHY_C20_B_MPLLA_CNTX_CFG(c->xe2hpd, i)),
				       s->mpll[i]);
	}
	/* the width for the protocol, then DP or HDMI rate */
	c20_program_vdr(c, vdr, owned);
	/* toggle to the context just written */
	cx0_rmw(c, owned, PHY_C20_VDR_CUSTOM_SERDES_RATE, PHY_C20_CONTEXT_TOGGLE,
		cntx ? 0 : PHY_C20_CONTEXT_TOGGLE, MB_WRITE_COMMITTED);
}

/* Port clock control: reversal in the buffer, which lane sources the
 * clock, the forwarded clock ungated, the PHY's clock (or FRL's /18)
 * selected, spread spectrum on the PLL in use. */
static void cx0_program_port_clock_ctl(struct cx0 *c, const struct cx0_port_pll *p)
{
	uint32_t val = 0;

	rmw(c, buf_ctl1(c), XELPDP_PORT_REVERSAL, c->reversal ? XELPDP_PORT_REVERSAL : 0);
	if (c->reversal)
		val |= XELPDP_LANE1_PHY_CLOCK_SELECT;
	val |= XELPDP_FORWARD_CLOCK_UNGATE;
	if (!p->is_dp && hdmi_is_frl(p->port_clock))
		val |= XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, XELPDP_DDI_CLOCK_SELECT_DIV18CLK);
	else
		val |= XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, XELPDP_DDI_CLOCK_SELECT_MAXPCLK);
	/* the DP 2.0 10 and 20 Gbps rates run from MPLLA */
	if (clock_matches(p->port_clock, 1000000) || clock_matches(p->port_clock, 2000000))
		val |= p->ssc ? XELPDP_SSC_ENABLE_PLLA : 0;
	else
		val |= p->ssc ? XELPDP_SSC_ENABLE_PLLB : 0;
	rmw(c, clock_ctl(c),
	    XELPDP_LANE1_PHY_CLOCK_SELECT | XELPDP_FORWARD_CLOCK_UNGATE |
	    XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_SSC_ENABLE_PLLA | XELPDP_SSC_ENABLE_PLLB,
	    val);
}

static int cx0pll_enable(struct cx0 *c, const struct cx0_port_pll *p, int lane_count)
{
	uint8_t maxpclk_lane = c->reversal ? CX0_LANE1 : CX0_LANE0;
	int ret = 0;

	cx0_transaction_begin(c);
	/* DP-alt never reverses: the Type-C mux flips the lanes itself */
	if (c->reversal && c->dp_alt)
		i915_dbg("[drm] i915: port %s: lane reversal in DP-alt mode\n", c->name);

	/* 1. port clock muxes, gating and spread spectrum */
	cx0_program_port_clock_ctl(c, p);
	/* 2. the PHY out of reset */
	cx0_lane_reset(c);
	/* 3. both lanes to ready */
	cx0_powerdown_change(c, CX0_BOTH_LANES, XELPDP_P2_STATE_READY);
	/* 4. (the bus timer was set at the transaction's start)
	 * 5. the PLL */
	if (c->c10)
		c10_pll_program(c, &p->c10);
	else
		c20_pll_program(c, &p->c20, &p->vdr);
	/* 6. the owned lanes' transmitters */
	cx0_program_phy_lane(c, lane_count);
	/* 8. the DDI clock the port is validated at */
	wr(c, DDI_CLK_VALFREQ(c->port), link_symbol_clock(p->is_dp, p->port_clock));
	/* 9. request the PLL on the lane that carries the clock, 10. wait */
	rmw(c, clock_ctl(c), pclk_pll_request(CX0_BOTH_LANES), pclk_pll_request(maxpclk_lane));
	if (wait_reg(c, clock_ctl(c), pclk_pll_ack(CX0_BOTH_LANES), pclk_pll_ack(maxpclk_lane),
		     XELPDP_PCLK_PLL_ENABLE_TIMEOUT_US, NULL)) {
		kprintf("[drm] i915: port %s: PHY PLL did not lock (%u kHz)\n", c->name,
			p->port_clock);
		ret = -ETIMEDOUT;
	}
	/* 12. HDMI on a C10: toggle the power state through active and back
	 * to ready on both lanes, which removes lane-to-lane skew */
	if (!p->is_dp && c->c10) {
		cx0_powerdown_change(c, CX0_BOTH_LANES, XELPDP_P0_STATE_ACTIVE);
		cx0_powerdown_change(c, CX0_BOTH_LANES, XELPDP_P2_STATE_READY);
	}
	return ret;
}

/* The disabled power state: power-gated P2 for C10 (and the eDP C20s of
 * Battlemage port A and of version 30), power-gated P4 otherwise. */
static uint8_t cx0_disable_state(struct cx0 *c)
{
	if (c->c10)
		return XELPDP_P2PG_STATE_DISABLE;
	if ((c->xe2hpd && c->port == PORT_A) || (c->ver >= 30 && c->is_edp))
		return XELPDP_P2PG_STATE_DISABLE;
	return XELPDP_P4PG_STATE_DISABLE;
}

static void cx0pll_disable(struct cx0 *c)
{
	cx0_transaction_begin(c);
	/* 1. the lanes to the disabled state */
	cx0_powerdown_change(c, CX0_BOTH_LANES, cx0_disable_state(c));
	/* 3. drop the PLL and reference requests, 4. no DDI clock */
	rmw(c, clock_ctl(c), pclk_pll_request(CX0_BOTH_LANES) | pclk_refclk_request(CX0_BOTH_LANES),
	    0);
	wr(c, DDI_CLK_VALFREQ(c->port), 0);
	/* 5. wait for the acks to go */
	if (wait_clear(c, clock_ctl(c),
		       pclk_pll_ack(CX0_BOTH_LANES) | pclk_refclk_ack(CX0_BOTH_LANES),
		       XELPDP_PCLK_PLL_DISABLE_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY PLL did not unlock\n", c->name);
	/* 7. clock deselected and gated */
	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver), 0);
	rmw(c, clock_ctl(c), XELPDP_FORWARD_CLOCK_UNGATE, 0);
}

static int cx0_pll_is_enabled(struct cx0 *c)
{
	uint8_t lane = c->reversal ? CX0_LANE1 : CX0_LANE0;
	return !!(rd(c, clock_ctl(c)) & pclk_pll_request(lane));
}

/* ---- Thunderbolt ----------------------------------------------------------------- */

static uint32_t tbt_clock_select(struct cx0 *c, uint32_t clock)
{
	switch (clock) {
	case 162000: return XELPDP_DDI_CLOCK_SELECT_TBT_162;
	case 270000: return XELPDP_DDI_CLOCK_SELECT_TBT_270;
	case 540000: return XELPDP_DDI_CLOCK_SELECT_TBT_540;
	case 810000: return XELPDP_DDI_CLOCK_SELECT_TBT_810;
	case 1000000:
		return c->ver >= 30 ? XELPDP_DDI_CLOCK_SELECT_TBT_312_5 :
				      XELPDP_DDI_CLOCK_SELECT_TBT_162;
	case 2000000:
		return c->ver >= 30 ? XELPDP_DDI_CLOCK_SELECT_TBT_625 :
				      XELPDP_DDI_CLOCK_SELECT_TBT_162;
	default:
		return XELPDP_DDI_CLOCK_SELECT_TBT_162;
	}
}

static uint32_t tbt_clock_rate(uint32_t sel)
{
	switch (sel) {
	case XELPDP_DDI_CLOCK_SELECT_TBT_162: return 162000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_270: return 270000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_540: return 540000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_810: return 810000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_312_5: return 1000000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_625: return 2000000;
	default: return 0;
	}
}

/* The port takes one of the Thunderbolt PLL's clocks: select and ungate
 * it, request it, wait for the acknowledge. */
static int tbt_clock_enable(struct cx0 *c, uint32_t port_clock)
{
	uint32_t val;
	int ret = 0;

	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_FORWARD_CLOCK_UNGATE,
	    XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, tbt_clock_select(c, port_clock)) |
	    XELPDP_FORWARD_CLOCK_UNGATE);
	val = rd(c, clock_ctl(c));
	val |= XELPDP_TBT_CLOCK_REQUEST;
	wr(c, clock_ctl(c), val);
	if (wait_set(c, clock_ctl(c), XELPDP_TBT_CLOCK_ACK, 100)) {
		kprintf("[drm] i915: port %s: Thunderbolt clock not acknowledged\n", c->name);
		ret = -ETIMEDOUT;
	}
	wr(c, DDI_CLK_VALFREQ(c->port), link_symbol_clock(1, port_clock));
	return ret;
}

static void tbt_clock_disable(struct cx0 *c)
{
	rmw(c, clock_ctl(c), XELPDP_TBT_CLOCK_REQUEST, 0);
	if (wait_clear(c, clock_ctl(c), XELPDP_TBT_CLOCK_ACK, 10))
		kprintf("[drm] i915: port %s: Thunderbolt clock still acknowledged\n", c->name);
	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_FORWARD_CLOCK_UNGATE, 0);
	wr(c, DDI_CLK_VALFREQ(c->port), 0);
}

/* The port buffer's I/O goes to the Thunderbolt controller or the PHY. */
static void io_select_tbt(struct cx0 *c, int tbt)
{
	rmw(c, buf_ctl1(c), XELPDP_PORT_BUF_IO_SELECT_TBT, tbt ? XELPDP_PORT_BUF_IO_SELECT_TBT : 0);
}

int mtl_phy_pll_enable(struct i915_device *i915, struct intel_output *o, int lanes)
{
	struct cx0 c;
	struct cx0_port_pll *p;
	int ret;

	if (intel_has_lt_phy(i915))
		return lt_phy_pll_enable(i915, o, lanes);
	if (!o || !port_ok(o->port))
		return -EINVAL;
	cx0_setup(&c, i915, o->port, o);
	p = &mtl_pll[o->port];
	if (!p->in_use) {
		kprintf("[drm] i915: port %s: PHY PLL enabled without settings\n", c.name);
		return -EINVAL;
	}
	if (lanes < 1 || lanes > 4)
		lanes = 4;
	if (c.tbt) {
		if (!p->tbt)
			return -EINVAL;
		/* the I/O goes to Thunderbolt before its clock is taken */
		io_select_tbt(&c, 1);
		ret = tbt_clock_enable(&c, p->port_clock);
	} else {
		if (p->tbt)
			return -EINVAL;
		ret = cx0pll_enable(&c, p, lanes);
		io_select_tbt(&c, 0);
	}
	i915_dbg("[drm] i915: port %s: clock on (%u kHz, %d lanes): CLOCK_CTL %08x BUF_CTL1 %08x BUF_CTL2 %08x\n",
		 c.name, p->port_clock, lanes, rd(&c, clock_ctl(&c)), rd(&c, buf_ctl1(&c)),
		 rd(&c, buf_ctl2(&c)));
	return ret;
}

void mtl_phy_pll_disable(struct i915_device *i915, struct intel_output *o)
{
	struct cx0 c;

	if (intel_has_lt_phy(i915)) {
		lt_phy_pll_disable(i915, o);
		return;
	}
	if (!o || !port_ok(o->port))
		return;
	cx0_setup(&c, i915, o->port, o);
	if (c.tbt)
		tbt_clock_disable(&c);
	else
		cx0pll_disable(&c);
	/* de-select Thunderbolt */
	io_select_tbt(&c, 0);
}

/* ---- signal levels ---------------------------------------------------------------- */

static int output_is_dp(const struct intel_output *o)
{
	return o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP;
}

/* The C10's transmitter boost and termination: the external DP link at
 * 5.4 and 8.1 Gbps and HDMI take the stronger settings. */
static uint8_t c10_tx_vboost(const struct intel_output *o, uint32_t clock)
{
	if (output_is_dp(o))
		return (!o->is_edp && (clock == 540000 || clock == 810000)) ? 5 : 4;
	return 5;
}

static uint8_t c10_tx_term_ctl(const struct intel_output *o, uint32_t clock)
{
	if (output_is_dp(o))
		return (!o->is_edp && (clock == 540000 || clock == 810000)) ? 5 : 2;
	return 6;
}

static void cx0_set_signal_levels(struct cx0 *c, const struct intel_output *o, int level)
{
	const struct cx0_trans *t;
	int n, hdmi_default, lane_count;
	int is_dp = output_is_dp(o);
	uint8_t owned = c->owned;
	uint32_t clock = o->link_rate_khz;

	if (c->tbt)
		return; /* Thunderbolt drives the lanes itself */
	if (!clock && port_ok(o->port))
		clock = mtl_pll[o->port].port_clock;

	if (c->c10) {
		t = c10_trans_dp14;
		n = ARRAY_LEN(c10_trans_dp14);
		hdmi_default = C10_TRANS_HDMI_DEFAULT;
	} else if (!is_dp) {
		t = c20_trans_hdmi;
		n = ARRAY_LEN(c20_trans_hdmi);
		hdmi_default = C20_TRANS_HDMI_DEFAULT;
	} else {
		t = c20_trans_dp14;
		n = ARRAY_LEN(c20_trans_dp14);
		hdmi_default = n - 1;
	}
	if (is_dp) {
		if (level < 0)
			level = 0;
		if (level >= n)
			level = n - 1;
		lane_count = o->lane_count ? o->lane_count : 4;
	} else {
		if (level < 0 || level >= n)
			level = hdmi_default;
		lane_count = 4;
	}

	cx0_transaction_begin(c);
	c10_msgbus_access_begin(c, owned);
	if (c->c10) {
		cx0_rmw(c, owned, PHY_C10_VDR_CMN(3), C10_CMN3_TXVBOOST_MASK,
			(uint8_t)C10_CMN3_TXVBOOST(c10_tx_vboost(o, clock)), MB_WRITE_UNCOMMITTED);
		cx0_rmw(c, owned, PHY_C10_VDR_TX(1), C10_TX1_TERMCTL_MASK,
			(uint8_t)C10_TX1_TERMCTL(c10_tx_term_ctl(o, clock)), MB_WRITE_COMMITTED);
	}
	/* per transmitter of the link: pre-cursor, swing, post-cursor */
	for (int ln = 0; ln < lane_count; ln++) {
		int lane = ln / 2, tx = ln % 2;
		uint8_t lane_mask = lane == 0 ? CX0_LANE0 : CX0_LANE1;

		if (!(lane_mask & owned))
			continue;
		cx0_rmw(c, lane_mask, (uint16_t)PHY_CX0_VDROVRD_CTL(lane, tx, 0),
			C10_PHY_OVRD_LEVEL_MASK, (uint8_t)C10_PHY_OVRD_LEVEL(t[level].pre_cursor),
			MB_WRITE_COMMITTED);
		cx0_rmw(c, lane_mask, (uint16_t)PHY_CX0_VDROVRD_CTL(lane, tx, 1),
			C10_PHY_OVRD_LEVEL_MASK, (uint8_t)C10_PHY_OVRD_LEVEL(t[level].vswing),
			MB_WRITE_COMMITTED);
		cx0_rmw(c, lane_mask, (uint16_t)PHY_CX0_VDROVRD_CTL(lane, tx, 2),
			C10_PHY_OVRD_LEVEL_MASK, (uint8_t)C10_PHY_OVRD_LEVEL(t[level].post_cursor),
			MB_WRITE_COMMITTED);
	}
	/* the override enables */
	cx0_rmw(c, owned, PHY_C10_VDR_OVRD, 0, PHY_C10_VDR_OVRD_TX1 | PHY_C10_VDR_OVRD_TX2,
		MB_WRITE_COMMITTED);
	c10_msgbus_access_commit(c, owned, 0);
}

void mtl_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	struct cx0 c;

	if (!o || !port_ok(o->port))
		return;
	mtl_level[o->port] = level;
	mtl_level_set[o->port] = 1;
	if (intel_has_lt_phy(i915)) {
		lt_phy_set_signal_level(i915, o, level);
		return;
	}
	cx0_setup(&c, i915, o->port, o);
	cx0_set_signal_levels(&c, o, level);
}

/* ---- the port buffer --------------------------------------------------------------- */

/* The die-to-die link between the display and the port: in the port
 * buffer up to version 14, in DDI_BUF_CTL from 20. */
static void ddi_d2d(struct cx0 *c, int enable)
{
	uint32_t reg, bit, state;

	if (c->ver >= 20) {
		reg = DDI_BUF_CTL(c->port);
		bit = XE2LPD_DDI_BUF_D2D_LINK_ENABLE;
		state = XE2LPD_DDI_BUF_D2D_LINK_STATE;
	} else {
		reg = buf_ctl1(c);
		bit = XELPDP_PORT_BUF_D2D_LINK_ENABLE;
		state = XELPDP_PORT_BUF_D2D_LINK_STATE;
	}
	rmw(c, reg, enable ? 0 : bit, enable ? bit : 0);
	if (wait_reg(c, reg, state, enable ? state : 0, 100, NULL))
		kprintf("[drm] i915: port %s: die-to-die link did not %s\n", c->name,
			enable ? "come up" : "go down");
}

/* The buffer leaves idle within 10 ms of being enabled. */
static void wait_buf_active(struct cx0 *c)
{
	if (wait_clear(c, buf_ctl1(c), XELPDP_PORT_BUF_PHY_IDLE, 10000))
		kprintf("[drm] i915: port %s: buffer did not become active\n", c->name);
}

void mtl_ddi_buf_enable(struct i915_device *i915, struct intel_output *o, int lanes)
{
	struct cx0 c;
	uint32_t buf_ctl;
	int is_dp;

	if (!o || !port_ok(o->port))
		return;
	cx0_setup(&c, i915, o->port, o);
	is_dp = output_is_dp(o);
	if (lanes < 1 || lanes > 4)
		lanes = is_dp ? (o->lane_count ? o->lane_count : 4) : 4;

	io_select_tbt(&c, c.tbt);
	/* the die-to-die link first */
	ddi_d2d(&c, 1);
	/* then the voltage swing (the level asked for last, or the first) */
	if (intel_has_lt_phy(i915))
		lt_phy_set_signal_level(i915, o, mtl_level_set[o->port] ? mtl_level[o->port] :
								       (is_dp ? 0 : -1));
	else
		cx0_set_signal_levels(&c, o, mtl_level_set[o->port] ? mtl_level[o->port] :
								       (is_dp ? 0 : -1));
	/* the port buffer: width, data width (10-bit symbols for 8b/10b; an
	 * HDMI port keeps its own), reversal */
	if (is_dp)
		rmw(&c, buf_ctl1(&c), XELPDP_PORT_WIDTH_MASK | XELPDP_PORT_BUF_PORT_DATA_WIDTH_MASK,
		    XELPDP_PORT_WIDTH(lanes) | XELPDP_PORT_BUF_PORT_DATA_10BIT |
		    (c.reversal ? XELPDP_PORT_REVERSAL : 0));
	else
		rmw(&c, buf_ctl1(&c), XELPDP_PORT_WIDTH_MASK | XELPDP_PORT_REVERSAL,
		    XELPDP_PORT_WIDTH(lanes) | (c.reversal ? XELPDP_PORT_REVERSAL : 0));
	/* DDI_BUF_CTL on: the port starts taking data */
	buf_ctl = DDI_PORT_WIDTH((uint32_t)lanes);
	if (c.reversal)
		buf_ctl |= DDI_BUF_PORT_REVERSAL;
	if (is_dp)
		buf_ctl |= DDI_BUF_TRANS_SELECT(0) | DDI_BUF_PORT_DATA_10BIT;
	if (c.ver >= 20)
		buf_ctl |= XE2LPD_DDI_BUF_D2D_LINK_ENABLE;
	wr(&c, DDI_BUF_CTL(o->port), buf_ctl | DDI_BUF_CTL_ENABLE);
	(void)rd(&c, DDI_BUF_CTL(o->port));
	wait_buf_active(&c);
	i915_dbg("[drm] i915: port %s: buffer on, %d lanes: DDI_BUF_CTL %08x BUF_CTL1 %08x\n",
		 c.name, lanes, rd(&c, DDI_BUF_CTL(o->port)), rd(&c, buf_ctl1(&c)));
}

void mtl_ddi_buf_disable(struct i915_device *i915, struct intel_output *o)
{
	struct cx0 c;

	if (!o || !port_ok(o->port))
		return;
	cx0_setup(&c, i915, o->port, o);
	rmw(&c, DDI_BUF_CTL(o->port), DDI_BUF_CTL_ENABLE, 0);
	/* the PHY reports idle within 10 ms */
	if (wait_set(&c, buf_ctl1(&c), XELPDP_PORT_BUF_PHY_IDLE, 10000))
		kprintf("[drm] i915: port %s: buffer did not go idle\n", c.name);
	ddi_d2d(&c, 0);
}

/* ---- what the firmware left running ----------------------------------------------- */

static void c10_readout(struct cx0 *c, struct c10_pll *s)
{
	cx0_transaction_begin(c);
	/* the vendor registers need the access bit even for reading */
	c10_msgbus_access_begin(c, CX0_LANE0);
	for (int i = 0; i < 20; i++)
		s->pll[i] = cx0_read(c, CX0_LANE0, (uint16_t)PHY_C10_VDR_PLL(i));
	s->cmn = cx0_read(c, CX0_LANE0, PHY_C10_VDR_CMN(0));
	s->tx = cx0_read(c, CX0_LANE0, PHY_C10_VDR_TX(0));
}

/* The context the PHY is running from, as the PHY has it. */
static void c20_readout(struct cx0 *c, struct c20_pll *s, struct c20_vdr *vdr)
{
	uint8_t serdes;
	int cntx, i;

	cx0_transaction_begin(c);
	serdes = cx0_read(c, CX0_LANE0, PHY_C20_VDR_CUSTOM_SERDES_RATE);
	cntx = !!(serdes & PHY_C20_CONTEXT_TOGGLE);
	vdr->custom_width = cx0_read(c, CX0_LANE0, PHY_C20_VDR_CUSTOM_WIDTH) &
			    PHY_C20_CUSTOM_WIDTH_MASK;
	vdr->serdes_rate = serdes & PHY_C20_SERDES_RATE_MASK;
	if (!(vdr->serdes_rate & PHY_C20_IS_DP))
		vdr->hdmi_rate = cx0_read(c, CX0_LANE0, PHY_C20_VDR_HDMI_RATE) &
				 PHY_C20_HDMI_RATE_MASK;
	else
		vdr->hdmi_rate = 0;
	for (i = 0; i < 3; i++)
		s->tx[i] = c20_sram_read(c, CX0_LANE0,
					 (uint16_t)(cntx ? PHY_C20_B_TX_CNTX_CFG(c->xe2hpd, i) :
							   PHY_C20_A_TX_CNTX_CFG(c->xe2hpd, i)));
	for (i = 0; i < 4; i++)
		s->cmn[i] = c20_sram_read(c, CX0_LANE0,
					  (uint16_t)(cntx ? PHY_C20_B_CMN_CNTX_CFG(c->xe2hpd, i) :
							    PHY_C20_A_CMN_CNTX_CFG(c->xe2hpd, i)));
	if (c20_uses_mpllb(s)) {
		for (i = 0; i < 11; i++)
			s->mpll[i] = c20_sram_read(c, CX0_LANE0,
						   (uint16_t)(cntx ? PHY_C20_B_MPLLB_CNTX_CFG(c->xe2hpd, i) :
								     PHY_C20_A_MPLLB_CNTX_CFG(c->xe2hpd, i)));
	} else {
		for (i = 0; i < 10; i++)
			s->mpll[i] = c20_sram_read(c, CX0_LANE0,
						   (uint16_t)(cntx ? PHY_C20_B_MPLLA_CNTX_CFG(c->xe2hpd, i) :
								     PHY_C20_A_MPLLA_CNTX_CFG(c->xe2hpd, i)));
		s->mpll[10] = 0;
	}
}

uint32_t mtl_phy_port_link_rate(struct i915_device *i915, struct intel_output *o)
{
	struct cx0 c;
	uint32_t val, sel, rate;

	if (intel_has_lt_phy(i915))
		return lt_phy_port_link_rate(i915, o);
	if (!o || !port_ok(o->port))
		return 0;
	cx0_setup(&c, i915, o->port, o);
	val = rd(&c, clock_ctl(&c));
	sel = XELPDP_DDI_CLOCK_SELECT_GET(c.ver, val);
	if (!(val & XELPDP_FORWARD_CLOCK_UNGATE))
		return 0;

	/* a Thunderbolt clock names its rate */
	if (sel != XELPDP_DDI_CLOCK_SELECT_MAXPCLK && sel != XELPDP_DDI_CLOCK_SELECT_DIV18CLK) {
		if (!(val & XELPDP_TBT_CLOCK_REQUEST) || !(val & XELPDP_TBT_CLOCK_ACK))
			return 0;
		rate = tbt_clock_rate(sel);
		i915_dbg("[drm] i915: port %s: firmware left a Thunderbolt clock, %u kHz\n",
			 c.name, rate);
		return rate;
	}
	if (!cx0_pll_is_enabled(&c))
		return 0;
	if (c.c10) {
		struct c10_pll s;
		c10_readout(&c, &s);
		rate = c10_port_clock(&s);
		i915_dbg("[drm] i915: port %s: firmware C10 PLL: %s %u kHz (pll0 %02x pll15 %02x)\n",
			 c.name, c10_state_is_dp(&s) ? "DP" : "HDMI", rate, s.pll[0], s.pll[15]);
	} else {
		struct c20_pll s;
		struct c20_vdr vdr;
		c20_readout(&c, &s, &vdr);
		rate = c20_port_clock(&s);
		i915_dbg("[drm] i915: port %s: firmware C20 PLL: %s %u kHz (tx0 %04x serdes %02x)\n",
			 c.name, (vdr.serdes_rate & PHY_C20_IS_DP) ? "DP" : "HDMI", rate, s.tx[0],
			 vdr.serdes_rate);
	}
	return rate;
}

/* ---- once at display init ------------------------------------------------------------ */

/* Every table entry produces the clock it is listed for, and so do the
 * computed HDMI settings for the HDMI entries' clocks. */
static void verify_tables(struct i915_device *i915)
{
	static const struct c10_entry *const c10_all[] = {
		c10_edp_tables, c10_dp_tables, c10_hdmi_tables,
	};
	static const struct c20_entry *const c20_all[] = {
		xe2hpd_c20_edp_tables, c20_dp_tables, xe2hpd_c20_dp_tables,
		xe3lpd_c20_dp_edp_tables, c20_hdmi_tables,
	};
	int bad = 0;

	for (int i = 0; i < ARRAY_LEN(c10_all); i++) {
		for (const struct c10_entry *t = c10_all[i]; t->state; t++) {
			uint32_t clk = c10_port_clock(t->state);
			if (!clock_matches(clk, t->clock)) {
				kprintf("[drm] i915: C10 table entry for %u kHz gives %u kHz\n",
					t->clock, clk);
				bad++;
			}
			if (c10_all[i] == c10_hdmi_tables) {
				struct c10_pll s;
				c10_compute_hdmi(&s, t->clock);
				clk = c10_port_clock(&s);
				if (!clock_matches(clk, t->clock)) {
					kprintf("[drm] i915: computed C10 PLL for %u kHz gives %u kHz\n",
						t->clock, clk);
					bad++;
				}
			}
		}
	}
	for (int i = 0; i < ARRAY_LEN(c20_all); i++) {
		for (const struct c20_entry *t = c20_all[i]; t->state; t++) {
			uint32_t clk = c20_port_clock(t->state);
			if (!clock_matches(clk, t->clock)) {
				kprintf("[drm] i915: C20 table entry for %u kHz gives %u kHz\n",
					t->clock, clk);
				bad++;
			}
			if (c20_all[i] == c20_hdmi_tables) {
				struct c20_pll s;
				if (c20_compute_hdmi(i915, t->clock, &s))
					continue;
				clk = c20_port_clock(&s);
				if (!clock_matches(clk, t->clock)) {
					kprintf("[drm] i915: computed C20 PLL for %u kHz gives %u kHz\n",
						t->clock, clk);
					bad++;
				}
			}
		}
	}
	if (!bad)
		i915_dbg("[drm] i915: C10/C20 PLL tables check out\n");
}

/* Version 30: a dedicated PHY comes out of reset in a power state that
 * keeps the package out of S0ix.  One that will not be used soon is
 * brought up at 1.62 Gbps and disabled again, which leaves it in the
 * disabled state. */
static void power_save_wa(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	uint32_t done = 0;

	for (int i = 0; i < d->nout && i < INTEL_MAX_OUTPUTS; i++) {
		struct intel_output *o = &d->outputs[i];
		struct cx0_port_pll p;
		const struct c10_entry *e;
		struct cx0 c;

		if (!o->present || !port_ok(o->port) || (done & (1u << o->port)))
			continue;
		done |= 1u << o->port;
		cx0_setup(&c, i915, o->port, o);
		if (!c.c10 || cx0_pll_is_enabled(&c))
			continue;
		e = c10_find(c10_edp_tables, 162000);
		if (!e)
			continue;
		mm_memset(&p, 0, sizeof(p));
		mm_memcpy(&p.c10, e->state, sizeof(p.c10));
		c10_drop_ssc(&p.c10, 0);
		p.is_c10 = 1;
		p.is_dp = 1;
		p.port_clock = c10_port_clock(&p.c10);
		i915_dbg("[drm] i915: port %s: PHY to its low-power state\n", c.name);
		cx0pll_enable(&c, &p, 4);
		cx0pll_disable(&c);
	}
}

int mtl_phy_init(struct i915_device *i915)
{
	int ver = cx0_display_ver(i915);

	mm_memset(mtl_pll, 0, sizeof(mtl_pll));
	for (int i = 0; i < INTEL_MAX_PORTS; i++) {
		mtl_level[i] = 0;
		mtl_level_set[i] = 0;
	}
	msgbus_reported = 0;
	mtl_edp_typec = 0;
	if (ver >= 30)
		mtl_edp_typec = !!(i915_read32(i915, PICA_PHY_CONFIG_CONTROL) & EDP_ON_TYPEC);
	i915->display.edp_on_typec = (uint8_t)mtl_edp_typec;
	if (intel_has_lt_phy(i915))
		return lt_phy_init(i915);

	verify_tables(i915);

	/* DDI A and B, then the four Type-C ports */
	static const int ports[] = { PORT_A, PORT_B, PORT_TC1, PORT_TC1 + 1, PORT_TC1 + 2,
				     PORT_TC1 + 3 };
	for (int i = 0; i < ARRAY_LEN(ports); i++) {
		int port = ports[i];
		struct cx0 c;

		cx0_setup(&c, i915, port, NULL);
		i915_dbg("[drm] i915: port %s (%s): CLOCK_CTL %08x BUF_CTL1 %08x BUF_CTL2 %08x VALFREQ %u\n",
			 c.name, c.c10 ? "C10" : "C20", rd(&c, clock_ctl(&c)), rd(&c, buf_ctl1(&c)),
			 rd(&c, buf_ctl2(&c)), rd(&c, DDI_CLK_VALFREQ(port)));
	}
	if (ver == 30)
		power_save_wa(i915);
	return 0;
}
