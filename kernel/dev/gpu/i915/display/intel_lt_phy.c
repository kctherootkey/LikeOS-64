// LikeOS -- the "LT" port PHY of Nova Lake (display version 35).
//
// Nova Lake's DDIs each carry an LT PHY.  Like the C10/C20 PHYs before
// it, the display reaches it only through the port's PICA block: buffer
// control words for the lanes' power states and resets, a clock control
// word that muxes the port clock and requests the PHY's clock, and a
// message bus per PHY lane over which the PHY's 8-bit registers are read
// and written.  What is new is how the PLL is configured: three vendor
// registers select the protocol, rate and PLL, and thirteen windows, each
// a 16-bit address and four data bytes, carry the values of the PLL's
// own 32-bit registers; the PHY takes them over when its rate update bit
// is set, which goes to the MAC as a "PHY to PHY" (P2P) write that is
// acknowledged only once the MAC side has switched.  The PHY's MAC clock
// comes up at 1.62 Gbps out of reset; the requested rate is then
// programmed, the clock dropped and requested again at the new rate, and
// the PHY reports the finished rate change with a pulse status bit.  The
// PLL settings for the DisplayPort and eDP rates and the common HDMI
// clocks come from the hardware tables; any other HDMI TMDS clock is
// computed: a DCO frequency and loop divider within the PLL's range,
// then the feedback divider, loop filter and lock detector settings,
// all in 64-bit fixed point.  A PHY has two lanes, each with two
// transmitters; a Type-C port in DisplayPort alternate mode with pin
// assignment D owns only lane 0.  A Type-C port routed through
// Thunderbolt does not use the PHY's PLL: the port clock control selects
// one of the Thunderbolt clocks.
//
// Every sequence follows the hardware programming steps in order and
// keeps their numbers.  None of it has run on hardware; each step
// reports what it found.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_cx0_phy_regs.h>
#include <kernel/dev/gpu/i915/intel_lt_phy_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* PHY lanes as a mask: message bus transactions go to one lane, the
 * per-lane bits of the port registers are built from a mask. */
#define LT_LANE0 0x1
#define LT_LANE1 0x2
#define LT_BOTH_LANES (LT_LANE0 | LT_LANE1)

#define MB_WRITE_UNCOMMITTED 0
#define MB_WRITE_COMMITTED 1

/* The protocol ("mode") encoded in vendor register 0. */
#define MODE_DP 3
#define MODE_HDMI_20 4
#define MODE_HDMI_FRL 5

/* Fixed point with 32 fraction bits. */
#define Q32_TO_INT(x) ((x) >> 32)
#define Q32_TO_FRAC(x) ((x) & 0xFFFFFFFFULL)

#define DCO_MIN_FREQ_MHZ 11850
#define LT_REFCLK_KHZ 38400
#define TDC_RES_MULTIPLIER 10000000ULL

/* The sink's DPCD: it can take a link with 0.5% down-spread. */
#define LT_DP_MAX_DOWNSPREAD_0_5 (1u << 0)

/* The Type-C pin assignment the port was found in: D (two DP lanes,
 * the PHY's lane 1 left to USB). */
#define LT_TC_PIN_ASSIGNMENT_D 4

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- PLL states ------------------------------------------------------------- */

/* An LT PLL: the three vendor configuration words and the thirteen
 * register windows, address (MSB, LSB) and data (most significant byte
 * first), exactly as they go over the message bus. */
struct lt_pll {
	uint8_t config[3];
	uint8_t addr_msb[13];
	uint8_t addr_lsb[13];
	uint8_t data[13][4];
};

struct lt_entry {
	uint32_t clock; /* kHz */
	const char *name;
	const struct lt_pll *state;
};

/* What get_dp/get_hdmi worked out for a port, programmed at enable. */
struct lt_port_pll {
	int in_use;
	int tbt; /* runs from a Thunderbolt clock, not the PHY's PLL */
	int is_dp;
	int is_edp;
	int ssc;
	int lane_count;
	uint32_t port_clock; /* kHz: the DP link rate, the TMDS clock */
	struct lt_pll pll;
};

static struct lt_port_pll lt_port[INTEL_MAX_PORTS];

/* ---- the PLL tables ---------------------------------------------------------- */

/* DisplayPort and eDP link rates, HDMI TMDS clocks: the PLL settings for
 * each from the hardware tables.  Window 0..12 hold PLL registers 4, 3,
 * 5, 57, the loop filter, the lock detector (TDC), spread spectrum, bias
 * 2, bias trim, the DCO medium and fine tuning, the spread injection and
 * the survivability bonus, in that order. */
static const struct lt_pll lt_dp_rbr = {
	{ 0x83, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x05, 0x0a, 0x2a, 0x20 },
		{ 0x80, 0x00, 0x00, 0x00 },
		{ 0x04, 0x04, 0x82, 0x28 },
		{ 0xfa, 0x16, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x05, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x4b, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0a },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_hbr1 = {
	{ 0x8b, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x03, 0xca, 0x34, 0xa0 },
		{ 0xe0, 0x00, 0x00, 0x00 },
		{ 0x05, 0x04, 0x81, 0xad },
		{ 0xfa, 0x11, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x07, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x43, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0d },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_hbr2 = {
	{ 0x93, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x4d, 0x34, 0xa0 },
		{ 0xe0, 0x00, 0x00, 0x00 },
		{ 0x0a, 0x04, 0x81, 0xda },
		{ 0xfa, 0x11, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x07, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x43, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0d },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_hbr3 = {
	{ 0x9b, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x4a, 0x34, 0xa0 },
		{ 0xe0, 0x00, 0x00, 0x00 },
		{ 0x05, 0x04, 0x80, 0xa8 },
		{ 0xfa, 0x11, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x07, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x43, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0d },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_uhbr10 = {
	{ 0x43, 0x2d, 0x00 },
	{ 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, 0x86,
	  0x86, 0x86, 0x86, 0x86, 0x86, 0x86 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x0a, 0x20, 0x80 },
		{ 0x6a, 0xaa, 0xaa, 0xab },
		{ 0x00, 0x03, 0x04, 0x94 },
		{ 0xfa, 0x1c, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x04, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x45, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x5b, 0xe0, 0x08 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_uhbr13_5 = {
	{ 0xcb, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x02, 0x09, 0x2b, 0xe0 },
		{ 0x90, 0x00, 0x00, 0x00 },
		{ 0x08, 0x04, 0x80, 0xe0 },
		{ 0xfa, 0x15, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x06, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x49, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x57, 0xe0, 0x0c },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_dp_uhbr20 = {
	{ 0x53, 0x2d, 0x00 },
	{ 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, 0x86,
	  0x86, 0x86, 0x86, 0x86, 0x86, 0x86 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x0a, 0x20, 0x80 },
		{ 0x6a, 0xaa, 0xaa, 0xab },
		{ 0x00, 0x03, 0x04, 0x94 },
		{ 0xfa, 0x1c, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x04, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x45, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x5b, 0xe0, 0x08 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_edp_r216 = {
	{ 0xa3, 0x2d, 0x01 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x03, 0xca, 0x2a, 0x20 },
		{ 0x80, 0x00, 0x00, 0x00 },
		{ 0x06, 0x04, 0x81, 0xbc },
		{ 0xfa, 0x16, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x05, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x4b, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0a },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_edp_r243 = {
	{ 0xab, 0x2d, 0x01 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x03, 0xca, 0x2f, 0x60 },
		{ 0xb0, 0x00, 0x00, 0x00 },
		{ 0x06, 0x04, 0x81, 0xbc },
		{ 0xfa, 0x13, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x06, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x47, 0x48, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0c },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_edp_r324 = {
	{ 0xb3, 0x2d, 0x01 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x02, 0x8a, 0x2a, 0x20 },
		{ 0x80, 0x00, 0x00, 0x00 },
		{ 0x06, 0x04, 0x81, 0x28 },
		{ 0xfa, 0x16, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x05, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x4b, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0a },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_edp_r432 = {
	{ 0xbb, 0x2d, 0x01 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x4d, 0x2a, 0x20 },
		{ 0x80, 0x00, 0x00, 0x00 },
		{ 0x0c, 0x04, 0x81, 0xbc },
		{ 0xfa, 0x16, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x05, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x4b, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x5b, 0xe0, 0x0a },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_edp_r675 = {
	{ 0xdb, 0x2d, 0x01 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x01, 0x4a, 0x2b, 0xe0 },
		{ 0x90, 0x00, 0x00, 0x00 },
		{ 0x06, 0x04, 0x80, 0xa8 },
		{ 0xfa, 0x15, 0x83, 0x11 },
		{ 0x80, 0x0f, 0xf9, 0x53 },
		{ 0x84, 0x26, 0x06, 0x04 },
		{ 0x00, 0xe0, 0x01, 0x00 },
		{ 0x49, 0x48, 0x00, 0x00 },
		{ 0x27, 0x08, 0x00, 0x00 },
		{ 0x5a, 0x13, 0x29, 0x13 },
		{ 0x00, 0x57, 0xe0, 0x0c },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_hdmi_25_2 = {
	{ 0x84, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x0c, 0x15, 0x27, 0x60 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x08, 0x04, 0x98, 0x28 },
		{ 0x42, 0x00, 0x84, 0x10 },
		{ 0x80, 0x0f, 0xd9, 0xb5 },
		{ 0x86, 0x00, 0x00, 0x00 },
		{ 0x01, 0xa0, 0x01, 0x00 },
		{ 0x4b, 0x00, 0x00, 0x00 },
		{ 0x28, 0x00, 0x00, 0x00 },
		{ 0x00, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_hdmi_74_25 = {
	{ 0x84, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x04, 0x15, 0x26, 0xa0 },
		{ 0x60, 0x00, 0x00, 0x00 },
		{ 0x08, 0x04, 0x88, 0x28 },
		{ 0xfa, 0x0c, 0x84, 0x11 },
		{ 0x80, 0x0f, 0xd9, 0x53 },
		{ 0x86, 0x00, 0x00, 0x00 },
		{ 0x01, 0xa0, 0x01, 0x00 },
		{ 0x4b, 0x00, 0x00, 0x00 },
		{ 0x28, 0x00, 0x00, 0x00 },
		{ 0x00, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_hdmi_148_5 = {
	{ 0x84, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x02, 0x15, 0x26, 0xa0 },
		{ 0x60, 0x00, 0x00, 0x00 },
		{ 0x08, 0x04, 0x84, 0x28 },
		{ 0xfa, 0x0c, 0x84, 0x11 },
		{ 0x80, 0x0f, 0xd9, 0x53 },
		{ 0x86, 0x00, 0x00, 0x00 },
		{ 0x01, 0xa0, 0x01, 0x00 },
		{ 0x4b, 0x00, 0x00, 0x00 },
		{ 0x28, 0x00, 0x00, 0x00 },
		{ 0x00, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};

static const struct lt_pll lt_hdmi_594 = {
	{ 0x84, 0x2d, 0x00 },
	{ 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
	  0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	{ 0x10, 0x0c, 0x14, 0xe4, 0x0c, 0x10, 0x14,
	  0x18, 0x48, 0x40, 0x4c, 0x24, 0x44 },
	{
		{ 0x00, 0x4c, 0x02, 0x00 },
		{ 0x00, 0x95, 0x26, 0xa0 },
		{ 0x60, 0x00, 0x00, 0x00 },
		{ 0x08, 0x04, 0x81, 0x28 },
		{ 0xfa, 0x0c, 0x84, 0x11 },
		{ 0x80, 0x0f, 0xd9, 0x53 },
		{ 0x86, 0x00, 0x00, 0x00 },
		{ 0x01, 0xa0, 0x01, 0x00 },
		{ 0x4b, 0x00, 0x00, 0x00 },
		{ 0x28, 0x00, 0x00, 0x00 },
		{ 0x00, 0x14, 0x2a, 0x14 },
		{ 0x00, 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00, 0x00 },
	},
};


static const struct lt_entry lt_dp_tables[] = {
	{ 162000, "dp_rbr", &lt_dp_rbr },
	{ 270000, "dp_hbr1", &lt_dp_hbr1 },
	{ 540000, "dp_hbr2", &lt_dp_hbr2 },
	{ 810000, "dp_hbr3", &lt_dp_hbr3 },
	{ 1000000, "dp_uhbr10", &lt_dp_uhbr10 },
	{ 1350000, "dp_uhbr13_5", &lt_dp_uhbr13_5 },
	{ 2000000, "dp_uhbr20", &lt_dp_uhbr20 },
	{ 0, NULL, NULL },
};

static const struct lt_entry lt_edp_tables[] = {
	{ 162000, "dp_rbr", &lt_dp_rbr },
	{ 216000, "edp_r216", &lt_edp_r216 },
	{ 243000, "edp_r243", &lt_edp_r243 },
	{ 270000, "dp_hbr1", &lt_dp_hbr1 },
	{ 324000, "edp_r324", &lt_edp_r324 },
	{ 432000, "edp_r432", &lt_edp_r432 },
	{ 540000, "dp_hbr2", &lt_dp_hbr2 },
	{ 675000, "edp_r675", &lt_edp_r675 },
	{ 810000, "dp_hbr3", &lt_dp_hbr3 },
	{ 0, NULL, NULL },
};

static const struct lt_entry lt_hdmi_tables[] = {
	{ 25200, "hdmi_25_2", &lt_hdmi_25_2 },
	{ 74250, "hdmi_74_25", &lt_hdmi_74_25 },
	{ 148500, "hdmi_148_5", &lt_hdmi_148_5 },
	{ 594000, "hdmi_594", &lt_hdmi_594 },
	{ 0, NULL, NULL },
};

/* ---- buffer translations ------------------------------------------------------ */

/* One level: the swing select and level, and the three cursor
 * coefficients written into the transmitter's PIPE registers. */
struct lt_trans {
	uint8_t txswing, txswing_level;
	uint8_t pre_cursor, main_cursor, post_cursor;
};

/* DisplayPort 1.4 (and HDMI): ordered like the DisplayPort levels,
 * 400mV x {0, 3.5, 6, 9.5 dB}, 600mV x {0, 3.5, 6}, 800mV x {0, 3.5},
 * 1200mV.  HDMI takes the table's default entry, the first. */
static const struct lt_trans lt_trans_dp14[] = {
	{ 1, 0, 0, 21, 0 }, { 1, 1, 0, 24, 3 }, { 1, 2, 0, 28, 7 }, { 0, 3, 0, 35, 13 },
	{ 1, 1, 0, 27, 0 }, { 1, 2, 0, 31, 5 }, { 0, 3, 0, 37, 11 }, { 1, 2, 0, 35, 0 },
	{ 0, 3, 0, 41, 7 }, { 0, 3, 0, 48, 0 },
};
#define LT_TRANS_HDMI_DEFAULT 0

/* DisplayPort 2.1 (the 128b/132b rates): the sixteen TX FFE presets. */
static const struct lt_trans lt_trans_uhbr[] = {
	{ 0, 0, 0, 48, 0 }, { 0, 0, 0, 43, 5 }, { 0, 0, 0, 40, 8 }, { 0, 0, 0, 37, 11 },
	{ 0, 0, 0, 33, 15 }, { 0, 0, 2, 46, 0 }, { 0, 0, 2, 42, 4 }, { 0, 0, 2, 38, 8 },
	{ 0, 0, 2, 35, 11 }, { 0, 0, 2, 33, 13 }, { 0, 0, 4, 44, 0 }, { 0, 0, 4, 40, 4 },
	{ 0, 0, 4, 37, 7 }, { 0, 0, 4, 33, 11 }, { 0, 0, 8, 40, 0 }, { 1, 0, 2, 26, 2 },
};

/* eDP: the DisplayPort 1.4 order at the panel's lower swings. */
static const struct lt_trans lt_trans_edp[] = {
	{ 1, 0, 0, 12, 0 }, { 1, 1, 0, 13, 1 }, { 1, 2, 0, 15, 3 }, { 1, 3, 0, 19, 7 },
	{ 1, 1, 0, 14, 0 }, { 1, 2, 0, 16, 2 }, { 1, 3, 0, 21, 5 }, { 1, 2, 0, 18, 0 },
	{ 1, 3, 0, 22, 4 }, { 1, 3, 0, 26, 0 },
};

/* ---- the platform ------------------------------------------------------------- */

int intel_has_lt_phy(struct i915_device *i915)
{
	if (!i915 || !i915->info)
		return 0;
	return i915->info->platform == I915_PLATFORM_NOVALAKE_S ||
	       i915->info->platform == I915_PLATFORM_NOVALAKE_P || i915->info->display_ver >= 35;
}

/* The display version the PICA register layout is chosen by. */
static int lt_display_ver(struct i915_device *i915)
{
	return i915->info->display_ver ? i915->info->display_ver : 35;
}

/* ---- a port and its PHY ----------------------------------------------------- */

/* Everything the sequences need to know about the port they drive. */
struct lt {
	struct i915_device *i915;
	int port;
	int ver;
	int reversal;
	int dp_alt; /* Type-C in DisplayPort alternate mode */
	int tbt; /* Type-C through Thunderbolt */
	int pin_d; /* DP-alt with pin assignment D */
	int is_edp;
	uint8_t owned; /* the PHY lanes the display owns */
	const char *name;
};

static void lt_setup(struct lt *c, struct i915_device *i915, int port,
		     const struct intel_output *o)
{
	c->i915 = i915;
	c->port = port;
	c->ver = lt_display_ver(i915);
	c->reversal = o ? !!o->lane_reversal : 0;
	c->dp_alt = o && o->is_tc && o->tc_mode == INTEL_TC_DP_ALT;
	c->tbt = o && o->is_tc && o->tc_mode == INTEL_TC_TBT_ALT;
	c->is_edp = o ? !!o->is_edp : 0;
	c->name = intel_port_name(i915, port);
	/* In DP-alt mode with pin assignment D (two lanes or fewer granted)
	 * PHY lane 1 belongs to USB; the display owns lane 0 only. */
	c->owned = LT_BOTH_LANES;
	c->pin_d = 0;
	if (c->dp_alt) {
		uint32_t max = o->tc_lanes ? o->tc_lanes : 4;
		if (max <= 2)
			c->owned = LT_LANE0;
		/* the pin assignment as the port was found in, else what the
		 * granted lanes say of it */
		if (o->tc_pin_assignment)
			c->pin_d = o->tc_pin_assignment == LT_TC_PIN_ASSIGNMENT_D;
		else
			c->pin_d = max <= 2;
	}
}

static int port_ok(int port)
{
	return port >= 0 && port < INTEL_MAX_PORTS;
}

static uint32_t rd(struct lt *c, uint32_t reg)
{
	return i915_read32(c->i915, reg);
}

static void wr(struct lt *c, uint32_t reg, uint32_t v)
{
	i915_write32(c->i915, reg, v);
}

/* Read-modify-write; the write always goes out (several of these bits
 * are write-one-to-clear). */
static uint32_t rmw(struct lt *c, uint32_t reg, uint32_t clear, uint32_t set)
{
	uint32_t old = rd(c, reg);
	wr(c, reg, (old & ~clear) | set);
	return old;
}

/* Poll until (reg & mask) == value; the last value read goes to *out. */
static int wait_reg(struct lt *c, uint32_t reg, uint32_t mask, uint32_t value,
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

static int wait_set(struct lt *c, uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
	return wait_reg(c, reg, mask, mask, timeout_us, NULL);
}

static int wait_clear(struct lt *c, uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
	return wait_reg(c, reg, mask, 0, timeout_us, NULL);
}

static uint32_t clock_ctl(struct lt *c)
{
	return XELPDP_PORT_CLOCK_CTL(c->ver, c->port);
}

static uint32_t buf_ctl1(struct lt *c)
{
	return XELPDP_PORT_BUF_CTL1_REG(c->ver, c->port);
}

static uint32_t buf_ctl2(struct lt *c)
{
	return XELPDP_PORT_BUF_CTL2(c->ver, c->port);
}

static uint32_t buf_ctl5(struct lt *c)
{
	return XE3PLPD_PORT_BUF_CTL5(c->ver, c->port);
}

/* The per-lane status bits of PORT_BUF_CTL2 for the owned lanes. */
static uint32_t owned_pipe_reset(struct lt *c)
{
	return c->owned == LT_BOTH_LANES ? XELPDP_LANE_PIPE_RESET(0) | XELPDP_LANE_PIPE_RESET(1) :
					   XELPDP_LANE_PIPE_RESET(0);
}

static uint32_t owned_current_status(struct lt *c)
{
	return c->owned == LT_BOTH_LANES ?
		       XELPDP_LANE_PHY_CURRENT_STATUS(0) | XELPDP_LANE_PHY_CURRENT_STATUS(1) :
		       XELPDP_LANE_PHY_CURRENT_STATUS(0);
}

static uint32_t owned_pulse_status(struct lt *c)
{
	return c->owned == LT_BOTH_LANES ?
		       XE3PLPDP_LANE_PHY_PULSE_STATUS(0) | XE3PLPDP_LANE_PHY_PULSE_STATUS(1) :
		       XE3PLPDP_LANE_PHY_PULSE_STATUS(0);
}

/* ---- the message bus ------------------------------------------------------------ */

static int msgbus_reported;
static int p2p_reported;

static uint32_t msgbus_ctl(struct lt *c, int lane)
{
	return XELPDP_PORT_M2P_MSGBUS_CTL(c->ver, c->port, lane);
}

static uint32_t msgbus_status(struct lt *c, int lane)
{
	return XELPDP_PORT_P2M_MSGBUS_STATUS(c->ver, c->port, lane);
}

/* The response-ready and error flags are cleared by writing them. */
static void lt_clear_response_ready(struct lt *c, int lane)
{
	rmw(c, msgbus_status(c, lane), 0,
	    XELPDP_PORT_P2M_RESPONSE_READY | XELPDP_PORT_P2M_ERROR_SET);
}

/* The P2P status's response-ready flag is cleared by writing it as 0. */
static void lt_clear_status_p2p(struct lt *c, int lane)
{
	rmw(c, XE3PLPD_PORT_P2M_MSGBUS_STATUS_P2P(c->ver, c->port, lane),
	    XELPDP_PORT_P2M_RESPONSE_READY, 0);
}

/* Abort whatever the lane's bus is doing and bring it back to idle. */
static void lt_bus_reset(struct lt *c, int lane)
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
	lt_clear_response_ready(c, lane);
}

/* Wait for the PHY's answer to a read or committed write and check it is
 * the answer to that command. */
static int lt_wait_for_ack(struct lt *c, uint32_t command, int lane, uint32_t *val)
{
	if (wait_reg(c, msgbus_status(c, lane), XELPDP_PORT_P2M_RESPONSE_READY,
		     XELPDP_PORT_P2M_RESPONSE_READY, XELPDP_MSGBUS_TIMEOUT_US, val)) {
		i915_dbg("[drm] i915: port %s: PHY lane %d message ack timeout, status %08x\n",
			 c->name, lane, *val);
		if (!(rd(c, XELPDP_PORT_MSGBUS_TIMER(c->ver, c->port, lane)) &
		      XELPDP_PORT_MSGBUS_TIMER_TIMED_OUT))
			i915_dbg("[drm] i915: port %s: the PHY's bus timer saw no timeout\n",
				 c->name);
		lt_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	if (*val & XELPDP_PORT_P2M_ERROR_SET) {
		i915_dbg("[drm] i915: port %s: PHY lane %d %s error, status %08x\n", c->name, lane,
			 command == XELPDP_PORT_P2M_COMMAND_READ_ACK ? "read" : "write", *val);
		lt_bus_reset(c, lane);
		return -EINVAL;
	}
	if (((*val & XELPDP_PORT_P2M_COMMAND_TYPE_MASK) >> XELPDP_PORT_P2M_COMMAND_TYPE_SHIFT) !=
	    command) {
		i915_dbg("[drm] i915: port %s: PHY lane %d: not a %s response, status %08x\n",
			 c->name, lane,
			 command == XELPDP_PORT_P2M_COMMAND_READ_ACK ? "read" : "write", *val);
		lt_bus_reset(c, lane);
		return -EINVAL;
	}
	return 0;
}

static int lt_read_once(struct lt *c, int lane, uint16_t addr)
{
	uint32_t val = 0;
	int ret;

	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: previous PHY transaction still pending, resetting the bus\n",
			 c->name);
		lt_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	lt_clear_response_ready(c, lane);
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING |
	   XELPDP_PORT_M2P_COMMAND_READ | XELPDP_PORT_M2P_ADDRESS(addr));
	ret = lt_wait_for_ack(c, XELPDP_PORT_P2M_COMMAND_READ_ACK, lane, &val);
	if (ret < 0)
		return ret;
	lt_clear_response_ready(c, lane);
	/* Before version 30 the bus is reset after every transaction, which
	 * leaves it in a known state for the next one; the LT PHY's bus
	 * does not need it. */
	if (c->ver < 30)
		lt_bus_reset(c, lane);
	return (int)((val & XELPDP_PORT_P2M_DATA_MASK) >> XELPDP_PORT_P2M_DATA_SHIFT);
}

static uint8_t lt_read_lane(struct lt *c, int lane, uint16_t addr)
{
	int status = 0;

	/* three tries are enough for a read to get through */
	for (int i = 0; i < 3; i++) {
		status = lt_read_once(c, lane, addr);
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
static uint8_t lt_read(struct lt *c, uint8_t lane_mask, uint16_t addr)
{
	return lt_read_lane(c, lane_mask == LT_LANE1 ? 1 : 0, addr);
}

/* An uncommitted write is only queued in the PHY; a committed one is
 * acknowledged once it has taken effect. */
static int lt_write_once(struct lt *c, int lane, uint16_t addr, uint8_t data, int committed)
{
	uint32_t val = 0;

	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: previous PHY transaction still pending, resetting the bus\n",
			 c->name);
		lt_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	lt_clear_response_ready(c, lane);
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING |
	   (committed ? XELPDP_PORT_M2P_COMMAND_WRITE_COMMITTED :
			XELPDP_PORT_M2P_COMMAND_WRITE_UNCOMMITTED) |
	   XELPDP_PORT_M2P_DATA(data) | XELPDP_PORT_M2P_ADDRESS(addr));
	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: PHY write did not complete, resetting the bus\n",
			 c->name);
		lt_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	if (committed) {
		int ret = lt_wait_for_ack(c, XELPDP_PORT_P2M_COMMAND_WRITE_ACK, lane, &val);
		if (ret < 0)
			return ret;
	} else if (rd(c, msgbus_status(c, lane)) & XELPDP_PORT_P2M_ERROR_SET) {
		i915_dbg("[drm] i915: port %s: PHY lane %d write error\n", c->name, lane);
		lt_bus_reset(c, lane);
		return -EINVAL;
	}
	lt_clear_response_ready(c, lane);
	if (c->ver < 30)
		lt_bus_reset(c, lane);
	return 0;
}

static void lt_write_lane(struct lt *c, int lane, uint16_t addr, uint8_t data, int committed)
{
	for (int i = 0; i < 3; i++)
		if (lt_write_once(c, lane, addr, data, committed) == 0)
			return;
	if (!msgbus_reported) {
		msgbus_reported = 1;
		kprintf("[drm] i915: port %s: PHY write of %04x failed after 3 tries\n", c->name,
			addr);
	}
}

/* A write goes to every lane in the mask. */
static void lt_write(struct lt *c, uint8_t lane_mask, uint16_t addr, uint8_t data,
		     int committed)
{
	for (int lane = 0; lane < 2; lane++)
		if (lane_mask & (1u << lane))
			lt_write_lane(c, lane, addr, data, committed);
}

static void lt_rmw(struct lt *c, uint8_t lane_mask, uint16_t addr, uint8_t clear,
		   uint8_t set, int committed)
{
	for (int lane = 0; lane < 2; lane++) {
		if (!(lane_mask & (1u << lane)))
			continue;
		uint8_t old = lt_read_lane(c, lane, addr);
		uint8_t val = (uint8_t)((old & ~clear) | set);
		if (val != old)
			lt_write_lane(c, lane, addr, val, committed);
	}
}

/* A P2P write: a committed write the PHY forwards to its MAC side (the
 * MAC's vendor register at LT_PHY_MAC_VDR takes it); the acknowledge
 * comes once the MAC has it.  The PHY then needs a moment to settle
 * before the ready flags are cleared, the P2P one included. */
static int lt_p2p_write_once(struct lt *c, int lane, uint16_t addr, uint8_t data)
{
	uint32_t val = 0;
	int ack;

	if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_P2P_TRANSACTION_PENDING,
		       XELPDP_MSGBUS_TIMEOUT_US)) {
		i915_dbg("[drm] i915: port %s: previous P2P transaction still pending, resetting the bus\n",
			 c->name);
		lt_bus_reset(c, lane);
		return -ETIMEDOUT;
	}
	/* write the status back as read: whatever flags are up go */
	rmw(c, msgbus_status(c, lane), 0, 0);
	wr(c, msgbus_ctl(c, lane), XELPDP_PORT_P2P_TRANSACTION_PENDING |
	   XELPDP_PORT_M2P_COMMAND_WRITE_COMMITTED | XELPDP_PORT_M2P_DATA(data) |
	   XELPDP_PORT_M2P_ADDRESS(addr));
	ack = lt_wait_for_ack(c, XELPDP_PORT_P2M_COMMAND_WRITE_ACK, lane, &val);
	if (ack < 0)
		return ack;
	if (val & XELPDP_PORT_P2M_ERROR_SET) {
		i915_dbg("[drm] i915: port %s: PHY lane %d P2P write error, status %08x\n",
			 c->name, lane, val);
		lt_clear_status_p2p(c, lane);
		lt_bus_reset(c, lane);
		return -EINVAL;
	}
	lapic_delay_us(XE3PLPD_P2P_SETTLE_US);
	lt_clear_response_ready(c, lane);
	lt_clear_status_p2p(c, lane);
	return 0;
}

static void lt_p2p_write_lane(struct lt *c, int lane, uint16_t addr, uint8_t data)
{
	for (int i = 0; i < 3; i++)
		if (lt_p2p_write_once(c, lane, addr, data) == 0)
			return;
	if (!p2p_reported) {
		p2p_reported = 1;
		kprintf("[drm] i915: port %s: PHY P2P write of %04x failed after 3 tries\n",
			c->name, addr);
	}
}

static void lt_p2p_write(struct lt *c, uint8_t lane_mask, uint16_t addr, uint8_t data)
{
	for (int lane = 0; lane < 2; lane++)
		if (lane_mask & (1u << lane))
			lt_p2p_write_lane(c, lane, addr, data);
}

/* Before any PHY transaction: the bus timer of both lanes programmed, so
 * a PHY that does not answer is noticed.  (The bus also needs the display
 * kept out of DC5/DC6 and PSR paused, neither of which this driver
 * uses.) */
static void lt_transaction_begin(struct lt *c)
{
	for (int lane = 0; lane < 2; lane++)
		rmw(c, XELPDP_PORT_MSGBUS_TIMER(c->ver, c->port, lane),
		    XELPDP_PORT_MSGBUS_TIMER_VAL_MASK, XELPDP_PORT_MSGBUS_TIMER_VAL);
}

/* ---- port clocks ---------------------------------------------------------------- */

static int clock_matches(uint32_t a, uint32_t b)
{
	return (a > b ? a - b : b - a) <= 1;
}

static uint8_t lt_mode(const struct lt_pll *s)
{
	return s->config[0] & LT_PHY_VDR_MODE_ENCODING_MASK;
}

static uint8_t lt_rate(const struct lt_pll *s)
{
	return (s->config[0] & LT_PHY_VDR_RATE_ENCODING_MASK) >> LT_PHY_VDR_RATE_ENCODING_SHIFT;
}

static int lt_state_is_hdmi(const struct lt_pll *s)
{
	uint8_t mode = lt_mode(s);
	return mode == MODE_HDMI_20 || mode == MODE_HDMI_FRL;
}

static int lt_state_is_dp(const struct lt_pll *s)
{
	return lt_mode(s) == MODE_DP;
}

/* The DisplayPort link rate an encoded rate stands for. */
static uint32_t lt_dp_clock(uint8_t rate)
{
	switch (rate) {
	case 0: return 162000;
	case 1: return 270000;
	case 2: return 540000;
	case 3: return 810000;
	case 4: return 216000;
	case 5: return 243000;
	case 6: return 324000;
	case 7: return 432000;
	case 8: return 1000000;
	case 9: return 1350000;
	case 10: return 2000000;
	case 11: return 675000;
	default:
		i915_dbg("[drm] i915: LT PHY: unknown DP rate encoding %u\n", (unsigned)rate);
		return 0;
	}
}

/* An HDMI 2.1 FRL rate (the TMDS clocks in the same list count as FRL). */
static int hdmi_is_frl(uint32_t clock)
{
	static const uint32_t rates[] = { 300000, 600000, 800000, 1000000, 1200000 };

	for (int i = 0; i < ARRAY_LEN(rates); i++)
		if (clock_matches(clock, rates[i]))
			return 1;
	return 0;
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

/* A PLL register's 32-bit value as its window holds it. */
static uint32_t lt_regval(const struct lt_pll *s, int i)
{
	return (uint32_t)s->data[i][3] | (uint32_t)s->data[i][2] << 8 |
	       (uint32_t)s->data[i][1] << 16 | (uint32_t)s->data[i][0] << 24;
}

/* The TMDS clock an HDMI PLL produces.  The computation below packs
 * PLL_reg3 as (D4 << 21) + (D3 << 18) + (D1 << 15) + (m2div_int << 5)
 * and PLL_reg57 as (D7 << 24) + (postdiv << 15) + (D8 << 7) + D6, the
 * fields not overlapping; PLL_reg5 is the feedback divider's fraction.
 * With m2div = 2 * m2 and the DCO at loop_cnt * frequency * 5:
 * frequency = m2div * refclk / (D8 * 10). */
static uint32_t lt_hdmi_port_clock(const struct lt_pll *s)
{
	uint32_t d8, pll_reg_5, pll_reg_3, pll_reg_57, m2div_frac, m2div_int;
	uint64_t temp0, temp1;

	pll_reg_5 = lt_regval(s, 2);
	pll_reg_3 = lt_regval(s, 1);
	pll_reg_57 = lt_regval(s, 3);
	m2div_frac = pll_reg_5;

	d8 = (pll_reg_57 & (0xffu << 7)) >> 7;
	if (d8 == 0) {
		kprintf("[drm] i915: LT PHY: invalid HDMI PLL state, taking the lowest HDMI clock\n");
		return lt_hdmi_tables[0].clock;
	}
	m2div_int = (pll_reg_3 & (0x3ffu << 5)) >> 5;
	temp0 = ((uint64_t)m2div_frac * LT_REFCLK_KHZ) >> 32;
	temp1 = (uint64_t)m2div_int * LT_REFCLK_KHZ;
	return (uint32_t)((temp1 + temp0) / ((uint64_t)d8 * 10));
}

/* The port clock a PLL state produces.  For DP and eDP it is the rate the
 * state is encoded for: the PLL arithmetic does not give the link rates
 * exactly. */
static uint32_t lt_port_clock(const struct lt_pll *s)
{
	uint8_t mode = lt_mode(s);

	if (mode == MODE_DP)
		return lt_dp_clock(lt_rate(s));
	if (mode == MODE_HDMI_20)
		return lt_hdmi_port_clock(s);
	kprintf("[drm] i915: LT PHY: unsupported PHY mode %u\n", (unsigned)mode);
	return 25200;
}

/* ---- the HDMI PLL, computed ------------------------------------------------- */

/* For a TMDS clock the tables do not have: a DCO frequency within the
 * PLL's range that is an even multiple of the clock to within the ppm
 * allowed, the feedback divider for it, and the loop filter, lock
 * detector, spread spectrum, bias and DCO tuning words that go with it.
 * The arithmetic is kept exactly as the hardware sequence does it (Q32
 * fixed point, '+' to pack the fields), so the values match what has
 * been validated. */

struct phy_param {
	uint32_t val;
	uint32_t addr;
};

struct lt_phy_params {
	struct phy_param pll_reg4;
	struct phy_param pll_reg3;
	struct phy_param pll_reg5;
	struct phy_param pll_reg57;
	struct phy_param lf;
	struct phy_param tdc;
	struct phy_param ssc;
	struct phy_param bias2;
	struct phy_param bias_trim;
	struct phy_param dco_med;
	struct phy_param dco_fine;
	struct phy_param ssc_inj;
	struct phy_param surv_bonus;
};

/* A Q32 number times a 32-bit integer, kept in Q32 (wrapping). */
static uint64_t mul_q32_u32(uint64_t a_q32, uint32_t b)
{
	uint64_t p0, p1, carry, result;
	uint64_t x_hi = a_q32 >> 32;
	uint64_t x_lo = a_q32 & 0xFFFFFFFFULL;

	p0 = x_lo * (uint64_t)b;
	p1 = x_hi * (uint64_t)b;
	carry = p0 >> 32;
	result = (p1 << 32) + (carry << 32) + (p0 & 0xFFFFFFFFULL);
	return result;
}

/* The DCO frequency (MHz, Q32, rounded up to two decimals) and the loop
 * divider: even dividers from 2 up, first within 11.85-16.2 GHz, then
 * within 10-12 GHz, the ppm allowed widened on the third pass.  (Once the
 * low range is taken it stays for the later passes.) */
static int calculate_target_dco_and_loop_cnt(uint32_t frequency_khz, uint64_t *target_dco_mhz,
					     uint32_t *loop_cnt)
{
	uint32_t ppm_value = 1;
	uint32_t dco_min_freq = DCO_MIN_FREQ_MHZ;
	uint32_t dco_max_freq = 16200;
	uint32_t dco_min_freq_low = 10000;
	uint32_t dco_max_freq_low = 12000;
	uint64_t val = 0;
	uint64_t refclk_khz = LT_REFCLK_KHZ;
	uint64_t m2div = 0;
	uint64_t val_with_frac = 0;
	uint64_t ppm = 0;
	uint64_t temp0 = 0, temp1, scale;
	int ppm_cnt, dco_count, y;

	for (ppm_cnt = 0; ppm_cnt < 5; ppm_cnt++) {
		ppm_value = ppm_cnt == 2 ? 2 : 1;
		for (dco_count = 0; dco_count < 2; dco_count++) {
			if (dco_count == 1) {
				dco_min_freq = dco_min_freq_low;
				dco_max_freq = dco_max_freq_low;
			}
			for (y = 2; y <= 255; y += 2) {
				val = ((uint64_t)y * frequency_khz) / 200;
				if (!val)
					continue;
				m2div = (val << 32) / refclk_khz;
				m2div = mul_q32_u32(m2div, 500);
				val_with_frac = mul_q32_u32(m2div, (uint32_t)refclk_khz);
				val_with_frac = val_with_frac / 500;
				temp1 = Q32_TO_INT(val_with_frac);
				temp0 = (temp1 > val) ? (temp1 - val) : (val - temp1);
				ppm = temp0 / val;
				if (temp1 >= dco_min_freq && temp1 <= dco_max_freq &&
				    ppm < ppm_value) {
					/* Round to two places */
					scale = (1ULL << 32) / 100;
					temp0 = (val_with_frac + scale - 1) / scale;
					*target_dco_mhz = temp0 * scale;
					*loop_cnt = (uint32_t)y;
					return 1;
				}
			}
		}
	}
	return 0;
}

/* The PLL registers' addresses: the first PLL's, or the second's. */
static void set_phy_vdr_addresses(struct lt_phy_params *p, int pll_type)
{
	p->pll_reg4.addr = PLL_REG_ADDR(PLL_REG4_ADDR, pll_type);
	p->pll_reg3.addr = PLL_REG_ADDR(PLL_REG3_ADDR, pll_type);
	p->pll_reg5.addr = PLL_REG_ADDR(PLL_REG5_ADDR, pll_type);
	p->pll_reg57.addr = PLL_REG_ADDR(PLL_REG57_ADDR, pll_type);
	p->lf.addr = PLL_REG_ADDR(PLL_LF_ADDR, pll_type);
	p->tdc.addr = PLL_REG_ADDR(PLL_TDC_ADDR, pll_type);
	p->ssc.addr = PLL_REG_ADDR(PLL_SSC_ADDR, pll_type);
	p->bias2.addr = PLL_REG_ADDR(PLL_BIAS2_ADDR, pll_type);
	p->bias_trim.addr = PLL_REG_ADDR(PLL_BIAS_TRIM_ADDR, pll_type);
	p->dco_med.addr = PLL_REG_ADDR(PLL_DCO_MED_ADDR, pll_type);
	p->dco_fine.addr = PLL_REG_ADDR(PLL_DCO_FINE_ADDR, pll_type);
	p->ssc_inj.addr = PLL_REG_ADDR(PLL_SSC_INJ_ADDR, pll_type);
	p->surv_bonus.addr = PLL_REG_ADDR(PLL_SURV_BONUS_ADDR, pll_type);
}

/* Spread spectrum word: no spread (step size, length and log all 0),
 * the analog configuration for the DCO range. */
static void compute_ssc(struct lt_phy_params *p, uint32_t ana_cfg)
{
	uint32_t ssc_stepsize = 0;
	uint32_t ssc_steplen = 0;
	uint32_t ssc_steplog = 0;

	p->ssc.val = (1u << 31) | (ana_cfg << 24) | (ssc_steplog << 16) | (ssc_stepsize << 8) |
		     ssc_steplen;
}

static void compute_bias2(struct lt_phy_params *p)
{
	uint32_t ssc_en_local = 0;
	uint64_t dynctrl_ovrd_en = 0;

	p->bias2.val = (uint32_t)((dynctrl_ovrd_en << 31) | (ssc_en_local << 30) | (1u << 23) |
				  (1u << 24) | (32u << 16) | (1u << 8));
}

/* The lock detector: settling time, overrides, lock and unlock
 * thresholds (tighter with the fine time-to-digital resolution). */
static void compute_tdc(struct lt_phy_params *p, uint64_t tdc_fine)
{
	uint32_t settling_time = 15;
	uint32_t bias_ovr_en = 1;
	uint32_t coldstart = 1;
	uint32_t true_lock = 2;
	uint32_t early_lock = 1;
	uint32_t lock_ovr_en = 1;
	uint32_t lock_thr = tdc_fine ? 3 : 5;
	uint32_t unlock_thr = tdc_fine ? 5 : 11;

	p->tdc.val = (uint32_t)((2u << 30) + (settling_time << 16) + (bias_ovr_en << 15) +
				(lock_ovr_en << 14) + (coldstart << 12) + (true_lock << 10) +
				(early_lock << 8) + (unlock_thr << 4) + lock_thr);
}

static void compute_dco_med(struct lt_phy_params *p)
{
	uint32_t cselmed_en = 0;
	uint32_t cselmed_dyn_adj = 0;
	uint32_t cselmed_ratio = 39;
	uint32_t cselmed_thr = 8;

	p->dco_med.val = (cselmed_en << 31) + (cselmed_dyn_adj << 30) + (cselmed_ratio << 24) +
			 (cselmed_thr << 21);
}

static void compute_dco_fine(struct lt_phy_params *p, uint32_t dco_12g)
{
	uint32_t dco_fine0_tune_2_0 = dco_12g ? 4 : 3;
	uint32_t dco_fine1_tune_2_0 = 2;
	uint32_t dco_fine2_tune_2_0 = dco_12g ? 2 : 1;
	uint32_t dco_fine3_tune_2_0 = 5;
	uint32_t dco_dith0_tune_2_0 = dco_12g ? 4 : 3;
	uint32_t dco_dith1_tune_2_0 = 2;

	p->dco_fine.val = (dco_dith1_tune_2_0 << 19) + (dco_dith0_tune_2_0 << 16) +
			  (dco_fine3_tune_2_0 << 11) + (dco_fine2_tune_2_0 << 8) +
			  (dco_fine1_tune_2_0 << 3) + dco_fine0_tune_2_0;
}

/* One window: the register's address, its value most significant byte
 * first. */
static void lt_assign(struct lt_pll *s, int i, const struct phy_param *r)
{
	s->addr_msb[i] = (uint8_t)((r->addr >> 8) & 0xFF);
	s->addr_lsb[i] = (uint8_t)(r->addr & 0xFF);
	s->data[i][0] = (uint8_t)((r->val & 0xFF000000) >> 24);
	s->data[i][1] = (uint8_t)((r->val & 0x00FF0000) >> 16);
	s->data[i][2] = (uint8_t)((r->val & 0x0000FF00) >> 8);
	s->data[i][3] = (uint8_t)(r->val & 0x000000FF);
}

static int lt_calculate_hdmi_state(struct lt_pll *s, uint32_t frequency_khz)
{
	struct lt_phy_params p;
	uint32_t dco_fmin = DCO_MIN_FREQ_MHZ;
	uint64_t refclk_khz = LT_REFCLK_KHZ;
	uint32_t refclk_mhz_int = LT_REFCLK_KHZ / 1000;
	uint64_t m2div = 0;
	uint64_t target_dco_mhz = 0;
	uint64_t tdc_fine, tdc_targetcnt;
	uint64_t feedfwd_gain, feedfwd_cal_en;
	uint64_t tdc_res = 30;
	uint32_t prop_coeff;
	uint32_t int_coeff;
	uint32_t ndiv = 1;
	uint32_t m1div = 1, m2div_int, m2div_frac;
	uint32_t frac_en;
	uint32_t ana_cfg;
	uint32_t loop_cnt = 0;
	uint32_t gain_ctrl = 2;
	uint32_t postdiv = 0;
	uint32_t dco_12g = 0;
	uint32_t pll_type = 0;
	uint32_t d1 = 2, d3 = 5, d4 = 0, d5 = 0;
	uint32_t d6 = 0, d6_new = 0;
	uint32_t d7, d8 = 0;
	uint32_t bonus_7_0 = 0;
	uint32_t csel2fo = 11;
	uint32_t csel2fo_ovrd_en = 1;
	uint64_t temp0, temp1, temp2, temp3;

	mm_memset(&p, 0, sizeof(p));
	p.surv_bonus.val = (bonus_7_0 << 16);
	p.pll_reg4.val = (refclk_mhz_int << 17) + (ndiv << 9) + (1u << 4);
	p.bias_trim.val = (csel2fo_ovrd_en << 30) + (csel2fo << 24);
	p.ssc_inj.val = 0;

	/* 1. the DCO frequency and the loop divider */
	if (!calculate_target_dco_and_loop_cnt(frequency_khz, &target_dco_mhz, &loop_cnt))
		return -EINVAL;

	/* 2. the feedback divider, integer and fraction (Q32) */
	m2div = target_dco_mhz / (refclk_khz * ndiv * m1div);
	m2div = mul_q32_u32(m2div, 1000);
	if (Q32_TO_INT(m2div) > 511)
		return -EINVAL;
	m2div_int = (uint32_t)Q32_TO_INT(m2div);
	m2div_frac = (uint32_t)Q32_TO_FRAC(m2div);
	frac_en = (m2div_frac > 0) ? 1 : 0;

	/* 3. the time-to-digital converter's resolution and target count,
	 * and the feed-forward gain of a fractional divider */
	if (frac_en > 0)
		tdc_res = 70;
	else
		tdc_res = 36;
	tdc_fine = tdc_res > 50 ? 1 : 0;
	temp0 = tdc_res * 40 * 11;
	temp1 = (((4 * TDC_RES_MULTIPLIER) + temp0) * 500) / (temp0 * refclk_khz);
	temp2 = (temp0 * refclk_khz) / 1000;
	temp3 = ((8 * TDC_RES_MULTIPLIER) + temp2) / temp2;
	tdc_targetcnt = tdc_res < 50 ? (uint64_t)(int)temp1 : (uint64_t)(int)temp3;
	tdc_targetcnt = (uint64_t)(int)(tdc_targetcnt / 2);
	temp0 = mul_q32_u32(target_dco_mhz, (uint32_t)tdc_res);
	temp0 >>= 32;
	feedfwd_gain = (m2div_frac > 0 && temp0) ? (m1div * TDC_RES_MULTIPLIER) / temp0 : 0;
	feedfwd_cal_en = frac_en;

	/* 4. the loop coefficients and analog range by the DCO's range */
	temp0 = (uint32_t)Q32_TO_INT(target_dco_mhz);
	prop_coeff = (temp0 >= dco_fmin) ? 3 : 4;
	int_coeff = (temp0 >= dco_fmin) ? 7 : 8;
	ana_cfg = (temp0 >= dco_fmin) ? 8 : 6;
	dco_12g = (temp0 >= dco_fmin) ? 0 : 1;

	if (temp0 > 12960)
		d7 = 10;
	else
		d7 = 8;

	d8 = loop_cnt / 2;
	d4 = d8 * 2;

	/* 5. PLL registers 3, 5 and 57, and the loop filter */
	p.pll_reg3.val = (uint32_t)((d4 << 21) + (d3 << 18) + (d1 << 15) + (m2div_int << 5));
	p.pll_reg5.val = m2div_frac;
	postdiv = (d5 == 0) ? 9 : d5;
	d6_new = (d6 == 0) ? 40 : d6;
	p.pll_reg57.val = (d7 << 24) + (postdiv << 15) + (d8 << 7) + d6_new;
	p.lf.val = (uint32_t)(((uint64_t)frac_en << 31) + (1ULL << 30) + ((uint64_t)frac_en << 29) +
			      (feedfwd_cal_en << 28) + (tdc_fine << 27) +
			      ((uint64_t)gain_ctrl << 24) + (feedfwd_gain << 16) +
			      ((uint64_t)int_coeff << 12) + ((uint64_t)prop_coeff << 8) +
			      tdc_targetcnt);

	/* 6. the rest of the words */
	compute_ssc(&p, ana_cfg);
	compute_bias2(&p);
	compute_tdc(&p, tdc_fine);
	compute_dco_med(&p);
	compute_dco_fine(&p, dco_12g);

	/* 7. which PLL: the one below 12 GHz, or the other */
	pll_type = ((frequency_khz == 10000) || (frequency_khz == 20000) ||
		    (frequency_khz == 2500) || (dco_12g == 1)) ? 0 : 1;
	set_phy_vdr_addresses(&p, (int)pll_type);

	/* 8. into the windows: HDMI 2.0 mode */
	s->config[0] = 0x84;
	s->config[1] = 0x2d;
	lt_assign(s, 0, &p.pll_reg4);
	lt_assign(s, 1, &p.pll_reg3);
	lt_assign(s, 2, &p.pll_reg5);
	lt_assign(s, 3, &p.pll_reg57);
	lt_assign(s, 4, &p.lf);
	lt_assign(s, 5, &p.tdc);
	lt_assign(s, 6, &p.ssc);
	lt_assign(s, 7, &p.bias2);
	lt_assign(s, 8, &p.bias_trim);
	lt_assign(s, 9, &p.dco_med);
	lt_assign(s, 10, &p.dco_fine);
	lt_assign(s, 11, &p.ssc_inj);
	lt_assign(s, 12, &p.surv_bonus);
	return 0;
}

/* ---- the tables a port uses -------------------------------------------------- */

/* The entry whose state produces `clock' (every entry's state is checked
 * to produce the clock it is listed for at init). */
static const struct lt_entry *lt_find(const struct lt_entry *t, uint32_t clock)
{
	for (; t->state; t++)
		if (clock_matches(clock, lt_port_clock(t->state)))
			return t;
	return NULL;
}

static void mirror_dpll(struct i915_device *i915, int port, const struct lt_port_pll *p)
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

/* The DisplayPort 8b/10b rates (and the eDP ones) the PHY has PLL
 * settings for. */
int lt_phy_rate_supported(struct i915_device *i915, int port, uint32_t link_rate_khz)
{
	(void)i915;
	if (!port_ok(port) || dp_is_uhbr(link_rate_khz))
		return 0;
	for (const struct lt_entry *t = lt_edp_tables; t->state; t++)
		if (t->clock == link_rate_khz)
			return 1;
	for (const struct lt_entry *t = lt_dp_tables; t->state; t++)
		if (t->clock == link_rate_khz)
			return 1;
	return 0;
}

static int tbt_rate_ok(uint32_t rate)
{
	switch (rate) {
	case 162000:
	case 270000:
	case 540000:
	case 810000:
	case 1000000:
	case 2000000:
		return 1;
	default:
		return 0;
	}
}

int lt_phy_pll_get_dp(struct i915_device *i915, struct intel_output *o, uint32_t link_rate_khz,
		      int ssc)
{
	struct lt c;
	struct lt_port_pll n, *p = &n;
	const struct lt_entry *e = NULL;

	if (!o || !port_ok(o->port))
		return -EINVAL;
	lt_setup(&c, i915, o->port, o);
	mm_memset(p, 0, sizeof(*p));
	p->is_dp = 1;
	p->is_edp = c.is_edp;
	p->lane_count = o->lane_count ? o->lane_count : 4;

	if (c.tbt) {
		/* the Thunderbolt PLL: only its clock select is remembered */
		if (!tbt_rate_ok(link_rate_khz)) {
			kprintf("[drm] i915: port %s: no Thunderbolt clock for %u kHz\n", c.name,
				link_rate_khz);
			return -EINVAL;
		}
		p->tbt = 1;
		p->port_clock = link_rate_khz;
	} else {
		/* an embedded panel's table first, which has the DP rates too */
		if (c.is_edp)
			e = lt_find(lt_edp_tables, link_rate_khz);
		if (!e)
			e = lt_find(lt_dp_tables, link_rate_khz);
		if (!e) {
			kprintf("[drm] i915: port %s: no LT PLL settings for %u kHz\n", c.name,
				link_rate_khz);
			return -EINVAL;
		}
		mm_memcpy(&p->pll, e->state, sizeof(p->pll));
		/* vendor register 2 tells the PHY it drives a panel */
		if (c.is_edp)
			p->pll.config[2] = 1;
		/* spread spectrum where it is wanted and the sink takes the
		 * 0.5% down-spread */
		p->ssc = ssc && (o->dpcd[DP_MAX_DOWNSPREAD] & LT_DP_MAX_DOWNSPREAD_0_5);
		p->port_clock = lt_port_clock(&p->pll);
	}
	p->in_use = 1;
	mm_memcpy(&lt_port[o->port], p, sizeof(*p));
	mirror_dpll(i915, o->port, p);
	i915_dbg("[drm] i915: port %s: %s PLL for %s at %u kHz%s\n", c.name,
		 p->tbt ? "Thunderbolt" : "LT", c.is_edp ? "eDP" : "DP", p->port_clock,
		 p->ssc ? " with SSC" : "");
	return o->port;
}

int lt_phy_pll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz)
{
	struct lt c;
	struct lt_port_pll n, *p = &n;
	const struct lt_entry *e;
	int computed = 0;

	if (!o || !port_ok(o->port))
		return -EINVAL;
	lt_setup(&c, i915, o->port, o);
	if (c.tbt)
		return -EINVAL; /* no TMDS through Thunderbolt */
	mm_memset(p, 0, sizeof(*p));
	p->lane_count = 4;
	/* the tables first; the computed values are correct but may not be
	 * the best tuned */
	e = lt_find(lt_hdmi_tables, clock_khz);
	if (e) {
		mm_memcpy(&p->pll, e->state, sizeof(p->pll));
	} else if (lt_calculate_hdmi_state(&p->pll, clock_khz)) {
		kprintf("[drm] i915: port %s: no LT PLL settings for TMDS %u kHz\n", c.name,
			clock_khz);
		return -EINVAL;
	} else {
		computed = 1;
	}
	p->port_clock = lt_port_clock(&p->pll);
	p->in_use = 1;
	mm_memcpy(&lt_port[o->port], p, sizeof(*p));
	mirror_dpll(i915, o->port, p);
	i915_dbg("[drm] i915: port %s: LT PLL (%s) for TMDS %u kHz (asked %u)\n", c.name,
		 computed ? "computed" : "table", p->port_clock, clock_khz);
	return o->port;
}

void lt_phy_pll_put(struct i915_device *i915, int pll)
{
	if (!port_ok(pll))
		return;
	lt_port[pll].in_use = 0;
	mirror_dpll(i915, pll, &lt_port[pll]);
}

/* ---- port sequences ---------------------------------------------------------- */

static uint32_t powerdown_update(uint8_t lane_mask)
{
	return ((lane_mask & LT_LANE0) ? XELPDP_LANE_POWERDOWN_UPDATE(0) : 0) |
	       ((lane_mask & LT_LANE1) ? XELPDP_LANE_POWERDOWN_UPDATE(1) : 0);
}

static uint32_t powerdown_state(uint8_t lane_mask, uint8_t state)
{
	return ((lane_mask & LT_LANE0) ? XELPDP_LANE_POWERDOWN_NEW_STATE(0, state) : 0) |
	       ((lane_mask & LT_LANE1) ? XELPDP_LANE_POWERDOWN_NEW_STATE(1, state) : 0);
}

/* Move the lanes to a new power-down state: the state, then (with no
 * bus transaction in flight) the update strobe, which the hardware
 * clears once the lanes are there. */
static void lt_powerdown_change(struct lt *c, uint8_t lane_mask, uint8_t state)
{
	rmw(c, buf_ctl2(c), powerdown_state(LT_BOTH_LANES, XELPDP_LANE_POWERDOWN_NEW_STATE_MASK),
	    powerdown_state(lane_mask, state));
	for (int lane = 0; lane < 2; lane++) {
		if (!(lane_mask & (1u << lane)))
			continue;
		if (wait_clear(c, msgbus_ctl(c, lane), XELPDP_PORT_M2P_TRANSACTION_PENDING,
			       XELPDP_MSGBUS_TIMEOUT_US)) {
			i915_dbg("[drm] i915: port %s: PHY transaction pending at power change, resetting the bus\n",
				 c->name);
			lt_bus_reset(c, lane);
		}
	}
	rmw(c, buf_ctl2(c), powerdown_update(LT_BOTH_LANES), powerdown_update(lane_mask));
	if (wait_clear(c, buf_ctl2(c), powerdown_update(lane_mask),
		       XELPDP_PORT_POWERDOWN_UPDATE_TIMEOUT_US))
		kprintf("[drm] i915: port %s: PHY did not change power state (to %u)\n", c->name,
			(unsigned)state);
}

/* Which state "ready" and "active" are, and no lane stagger.  (The DC5
 * entry and exit words of the newer buffer control are the DMC
 * firmware's.) */
static void lt_setup_powerdown(struct lt *c)
{
	rmw(c, buf_ctl2(c), XELPDP_POWER_STATE_READY_MASK,
	    XELPDP_POWER_STATE_READY(XELPDP_P2_STATE_READY));
	rmw(c, XELPDP_PORT_BUF_CTL3(c->ver, c->port),
	    XELPDP_POWER_STATE_ACTIVE_MASK | XELPDP_PLL_LANE_STAGGERING_DELAY_MASK,
	    XELPDP_POWER_STATE_ACTIVE(XELPDP_P0_STATE_ACTIVE) | XELPDP_PLL_LANE_STAGGERING_DELAY(0));
}

/* Out of reset with the MAC clock running at its default 162 MHz: the
 * default MAC clock rate and DP PHY mode, the reset power state, the MAC
 * clock out of reset and requested, the forwarded clock ungated, the
 * owned lanes' pipes out of reset, and the PHY's rate calibration
 * awaited. */
static void lt_lane_reset(struct lt *c)
{
	uint32_t pipe_reset = owned_pipe_reset(c);
	uint32_t current_status = owned_current_status(c);
	uint32_t pulse_status = owned_pulse_status(c);

	rmw(c, buf_ctl5(c), XE3PLPD_MACCLK_RATE_MASK, XE3PLPD_MACCLK_RATE_DEF);
	rmw(c, buf_ctl1(c), XE3PLPDP_PHY_MODE_MASK, XE3PLPDP_PHY_MODE_DP);

	lt_setup_powerdown(c);
	lt_powerdown_change(c, c->owned, XELPDP_P2_STATE_RESET);

	rmw(c, buf_ctl5(c), XE3PLPD_MACCLK_RESET_0, 0);

	rmw(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_REQUEST(0), XELPDP_LANE_PCLK_PLL_REQUEST(0));
	if (wait_set(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_ACK(0),
		     XE3PLPD_MACCLK_TURNON_LATENCY_US))
		kprintf("[drm] i915: port %s: PHY MAC clock request not acknowledged\n", c->name);

	rmw(c, clock_ctl(c), XELPDP_FORWARD_CLOCK_UNGATE, XELPDP_FORWARD_CLOCK_UNGATE);

	rmw(c, buf_ctl2(c), pipe_reset | pulse_status, 0);
	if (wait_clear(c, buf_ctl2(c), current_status, XE3PLPD_RESET_END_LATENCY_US))
		kprintf("[drm] i915: port %s: PHY lanes did not leave reset\n", c->name);

	if (wait_set(c, buf_ctl2(c), pulse_status, XE3PLPD_RATE_CALIB_DONE_LATENCY_US))
		kprintf("[drm] i915: port %s: PHY rate calibration not done\n", c->name);

	rmw(c, buf_ctl2(c), pulse_status, 0);
}

/* Port clock control: reversal in the buffer, the forwarded clock
 * ungated, the MAC clock (or FRL's /18) selected, spread spectrum.  The
 * select value named MAXPCLK stands for the MAC clock with this PHY. */
static void lt_program_port_clock_ctl(struct lt *c, const struct lt_port_pll *p)
{
	uint32_t val = 0;

	rmw(c, buf_ctl1(c), XELPDP_PORT_REVERSAL, c->reversal ? XELPDP_PORT_REVERSAL : 0);
	val |= XELPDP_FORWARD_CLOCK_UNGATE;
	if (lt_state_is_hdmi(&p->pll) && hdmi_is_frl(p->port_clock))
		val |= XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, XELPDP_DDI_CLOCK_SELECT_DIV18CLK);
	else
		val |= XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, XELPDP_DDI_CLOCK_SELECT_MAXPCLK);
	val |= p->ssc ? XELPDP_SSC_ENABLE_PLLA : 0;
	rmw(c, clock_ctl(c),
	    XELPDP_LANE1_PHY_CLOCK_SELECT | XELPDP_FORWARD_CLOCK_UNGATE |
	    XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_SSC_ENABLE_PLLA | XELPDP_SSC_ENABLE_PLLB,
	    val);
}

/* Whether the PLL needs programming: the PHY comes out of reset at
 * 1.62 Gbps, and a DP link at exactly what the PHY already runs could
 * keep it.  As the hardware sequence has the comparison (against
 * 1620000, a value no kHz link rate takes) it never holds, so the PLL is
 * always reprogrammed; kept so. */
static int lt_config_changed(struct lt *c, const struct lt_port_pll *p)
{
	uint8_t val, rate;
	uint32_t clock;

	val = lt_read(c, LT_LANE0, LT_PHY_VDR_0_CONFIG);
	rate = (val & LT_PHY_VDR_RATE_ENCODING_MASK) >> LT_PHY_VDR_RATE_ENCODING_SHIFT;
	if (lt_state_is_dp(&p->pll)) {
		clock = lt_dp_clock(rate);
		if (p->port_clock == 1620000 && p->port_clock == clock)
			return 0;
	}
	return 1;
}

/* The PLL: the vendor configuration (mode and rate to the owned lanes,
 * the PLL type to lane 0, the panel flag to the owned lanes), then the
 * thirteen windows to lane 0, address then data, most significant data
 * byte first. */
static void lt_program_pll(struct lt *c, const struct lt_pll *s)
{
	lt_write(c, c->owned, LT_PHY_VDR_0_CONFIG, s->config[0], MB_WRITE_COMMITTED);
	lt_write(c, LT_LANE0, LT_PHY_VDR_1_CONFIG, s->config[1], MB_WRITE_COMMITTED);
	lt_write(c, c->owned, LT_PHY_VDR_2_CONFIG, s->config[2], MB_WRITE_COMMITTED);

	for (int i = 0; i <= 12; i++) {
		lt_write(c, LT_LANE0, (uint16_t)LT_PHY_VDR_X_ADDR_MSB(i), s->addr_msb[i],
			 MB_WRITE_COMMITTED);
		lt_write(c, LT_LANE0, (uint16_t)LT_PHY_VDR_X_ADDR_LSB(i), s->addr_lsb[i],
			 MB_WRITE_COMMITTED);
		for (int j = 3, k = 0; j >= 0; j--, k++)
			lt_write(c, LT_LANE0, (uint16_t)LT_PHY_VDR_X_DATAY(i, j), s->data[i][k],
				 MB_WRITE_COMMITTED);
	}
}

/* Enable the transmitters the link uses and disable the others.  Two
 * transmitters per PHY lane: TX 0 and 1 are lane 0's, 2 and 3 lane 1's.
 * Reversal moves a narrow link to the top transmitters; in DP-alt mode
 * the Type-C mux has already flipped the lanes, and a single lane goes
 * out on transmitter 0 with pin assignment D, else on transmitter 1.
 * Each lane enable is a P2P write. */
static void lt_enable_disable_tx(struct lt *c, int lane_count)
{
	uint8_t transmitter_mask;

	switch (lane_count) {
	case 1:
		transmitter_mask = c->reversal ? 0x8 : 0x1;
		if (c->dp_alt)
			transmitter_mask = c->pin_d ? 0x1 : 0x2;
		break;
	case 2:
		transmitter_mask = c->reversal ? 0xc : 0x3;
		if (c->dp_alt)
			transmitter_mask = 0x3;
		break;
	case 3:
		transmitter_mask = c->reversal ? 0xe : 0x7;
		if (c->dp_alt)
			transmitter_mask = 0x7;
		break;
	case 4:
		transmitter_mask = 0xf;
		break;
	default:
		i915_dbg("[drm] i915: port %s: %d lanes? enabling all four\n", c->name,
			 lane_count);
		transmitter_mask = 0xf;
		break;
	}

	for (int i = 0; i < 4; i++) {
		uint8_t lane_mask = i < 2 ? LT_LANE0 : LT_LANE1;
		int tx = i % 2;

		lt_p2p_write(c, lane_mask, (uint16_t)LT_PHY_TXY_CTL10(tx),
			     (transmitter_mask & (1u << i)) ? LT_PHY_TX_LANE_ENABLE : 0);
	}
}

static int ltpll_enable(struct lt *c, const struct lt_port_pll *p, int lane_count)
{
	uint32_t pulse_status = owned_pulse_status(c);
	uint8_t rate_update;
	int ret = 0;

	lt_transaction_begin(c);

	/* 1. Enable the MAC clock at its default 162 MHz frequency. */
	lt_lane_reset(c);

	/* 2. Program PORT_CLOCK_CTL: clock muxes, gating and SSC. */
	lt_program_port_clock_ctl(c, p);

	/* 3. Change the owned PHY lanes' power to the Ready state. */
	lt_powerdown_change(c, c->owned, XELPDP_P2_STATE_READY);

	/* 4. Read the PHY's VDR_0_CONFIG: the enabled PLL type, the encoded
	 * rate and mode. */
	if (lt_config_changed(c, p)) {
		/* 5. Program the PHY's internal PLL registers over the message
		 * bus for the frequency and protocol. */
		lt_program_pll(c, &p->pll);

		/* 6. The P2P transaction flow:
		 * 6.1. set the Rate Control VDR Update bit (0xCC4) on the
		 *      owned lanes,
		 * 6.2. poll for P2P Transaction Ready (the MAC's VDR at 0xC00
		 *      then holds the PCLKIN gate; the acknowledge is all
		 *      that is checked),
		 * 6.3. clear P2P Transaction Ready. */
		lt_p2p_write(c, c->owned, LT_PHY_RATE_UPDATE, LT_PHY_RATE_CONTROL_VDR_UPDATE);

		/* 7. PORT_CLOCK_CTL[PCLK PLL Request LN0] = 0. */
		rmw(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_REQUEST(0), 0);

		/* 8. Poll for PORT_CLOCK_CTL[PCLK PLL Ack LN0] = 0. */
		if (wait_clear(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_ACK(0),
			       XE3PLPD_MACCLK_TURNOFF_LATENCY_US))
			kprintf("[drm] i915: port %s: PHY MAC clock acknowledge did not drop\n",
				c->name);

		/* 9. (The voltage/frequency switch before the change is the
		 * CD clock code's.)
		 * 10. DDI_CLK_VALFREQ to the intended DDI clock. */
		wr(c, DDI_CLK_VALFREQ(c->port), link_symbol_clock(p->is_dp, p->port_clock));

		/* 11. PORT_CLOCK_CTL[PCLK PLL Request LN0] = 1. */
		rmw(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_REQUEST(0),
		    XELPDP_LANE_PCLK_PLL_REQUEST(0));

		/* 12. Poll for PORT_CLOCK_CTL[PCLK PLL Ack LN0] = 1. */
		if (wait_set(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_ACK(0),
			     XE3PLPD_MACCLK_TURNON_LATENCY_US)) {
			kprintf("[drm] i915: port %s: PHY PLL did not lock (%u kHz)\n", c->name,
				p->port_clock);
			ret = -ETIMEDOUT;
		}

		/* 13. Ungate the forwarded clock. */
		rmw(c, clock_ctl(c), XELPDP_FORWARD_CLOCK_UNGATE, XELPDP_FORWARD_CLOCK_UNGATE);

		/* 14. Clear PORT_BUF_CTL2[PHY Pulse Status]. */
		rmw(c, buf_ctl2(c), pulse_status, pulse_status);

		/* 15. Clear the Rate Control VDR Update bit on the owned
		 * lanes. */
		rate_update = lt_read(c, LT_LANE0, LT_PHY_RATE_UPDATE);
		rate_update &= (uint8_t)~LT_PHY_RATE_CONTROL_VDR_UPDATE;
		lt_write(c, c->owned, LT_PHY_RATE_UPDATE, rate_update, MB_WRITE_COMMITTED);

		/* 16. Poll for PORT_BUF_CTL2[PHY Pulse Status] = 1 on the
		 * owned lanes. */
		if (wait_set(c, buf_ctl2(c), pulse_status, XE3PLPD_RATE_CALIB_DONE_LATENCY_US))
			kprintf("[drm] i915: port %s: PHY PLL rate not changed\n", c->name);

		/* 17. Clear PORT_BUF_CTL2[PHY Pulse Status]. */
		rmw(c, buf_ctl2(c), pulse_status, pulse_status);
	} else {
		wr(c, DDI_CLK_VALFREQ(c->port), link_symbol_clock(p->is_dp, p->port_clock));
	}

	/* 18. (The voltage/frequency switch after the change is the CD
	 * clock code's.)
	 * 19. The owned lanes to Active, and the transmitters enabled or
	 * disabled. */
	lt_powerdown_change(c, c->owned, XELPDP_P0_STATE_ACTIVE);
	lt_enable_disable_tx(c, lane_count);
	return ret;
}

static void ltpll_disable(struct lt *c)
{
	uint32_t pipe_reset = owned_pipe_reset(c);
	uint32_t current_status = owned_current_status(c);
	uint32_t pulse_status = owned_pulse_status(c);

	lt_transaction_begin(c);

	/* 1. Clear PORT_BUF_CTL2[PHY Pulse Status]. */
	rmw(c, buf_ctl2(c), pulse_status, pulse_status);

	/* 2. The owned lanes' Pipe Reset = 1. */
	rmw(c, buf_ctl2(c), pipe_reset, pipe_reset);

	/* 3. Poll for the owned lanes' PHY Current Status = 1. */
	if (wait_set(c, buf_ctl2(c), current_status, XE3PLPD_RESET_START_LATENCY_US))
		kprintf("[drm] i915: port %s: PHY lanes did not enter reset\n", c->name);

	/* 4. Clear the owned lanes' PHY Pulse Status. */
	rmw(c, buf_ctl2(c), pulse_status, pulse_status);

	/* 5. (The voltage/frequency switch before the change is the CD
	 * clock code's.)
	 * 6. PORT_CLOCK_CTL[PCLK PLL Request LN0] = 0. */
	rmw(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_REQUEST(0), 0);

	/* 7. DDI_CLK_VALFREQ to 0. */
	wr(c, DDI_CLK_VALFREQ(c->port), 0);

	/* 8. Poll for PORT_CLOCK_CTL[PCLK PLL Ack LN0] = 0. */
	if (wait_clear(c, clock_ctl(c), XELPDP_LANE_PCLK_PLL_ACK(0),
		       XE3PLPD_MACCLK_TURNOFF_LATENCY_US))
		kprintf("[drm] i915: port %s: PHY MAC clock acknowledge did not drop\n", c->name);

	/* 9. (The voltage/frequency switch after the change is the CD
	 * clock code's.)
	 * 10. PORT_CLOCK_CTL: clocks deselected and gated. */
	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_FORWARD_CLOCK_UNGATE, 0);

	/* 11. PORT_BUF_CTL5[MacCLK Reset_0] = 1: the MAC clock into reset. */
	rmw(c, buf_ctl5(c), XE3PLPD_MACCLK_RESET_0, XE3PLPD_MACCLK_RESET_0);
}

/* The PHY's clock is up: the MAC clock request on lane 0 acknowledged. */
static int lt_pll_is_enabled(struct lt *c)
{
	return !!(rd(c, clock_ctl(c)) & XELPDP_LANE_PCLK_PLL_ACK(0));
}

/* ---- Thunderbolt ----------------------------------------------------------------- */

static uint32_t tbt_clock_select(uint32_t clock)
{
	switch (clock) {
	case 162000: return XELPDP_DDI_CLOCK_SELECT_TBT_162;
	case 270000: return XELPDP_DDI_CLOCK_SELECT_TBT_270;
	case 540000: return XELPDP_DDI_CLOCK_SELECT_TBT_540;
	case 810000: return XELPDP_DDI_CLOCK_SELECT_TBT_810;
	case 1000000: return XELPDP_DDI_CLOCK_SELECT_TBT_312_5;
	case 2000000: return XELPDP_DDI_CLOCK_SELECT_TBT_625;
	default: return XELPDP_DDI_CLOCK_SELECT_TBT_162;
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

/* The port takes one of the Thunderbolt PLL's clocks. */
static int tbt_clock_enable(struct lt *c, uint32_t port_clock)
{
	uint32_t val;
	int ret = 0;

	/* 1. PORT_CLOCK_CTL: the clock select and the forwarded clock */
	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_FORWARD_CLOCK_UNGATE,
	    XELPDP_DDI_CLOCK_SELECT_PREP(c->ver, tbt_clock_select(port_clock)) |
	    XELPDP_FORWARD_CLOCK_UNGATE);
	/* 2. read it back, 3. (the voltage/frequency switch is the CD clock
	 * code's), 4. request the Thunderbolt clock */
	val = rd(c, clock_ctl(c));
	val |= XELPDP_TBT_CLOCK_REQUEST;
	wr(c, clock_ctl(c), val);
	/* 5. wait for its acknowledge */
	if (wait_set(c, clock_ctl(c), XELPDP_TBT_CLOCK_ACK, 100)) {
		kprintf("[drm] i915: port %s: Thunderbolt clock not acknowledged\n", c->name);
		ret = -ETIMEDOUT;
	}
	/* 6. (CD clock code), 7. the DDI clock the port is validated at */
	wr(c, DDI_CLK_VALFREQ(c->port), link_symbol_clock(1, port_clock));
	return ret;
}

static void tbt_clock_disable(struct lt *c)
{
	/* 1. (CD clock code), 2. drop the Thunderbolt clock request */
	rmw(c, clock_ctl(c), XELPDP_TBT_CLOCK_REQUEST, 0);
	/* 3. wait for its acknowledge to go */
	if (wait_clear(c, clock_ctl(c), XELPDP_TBT_CLOCK_ACK, 10))
		kprintf("[drm] i915: port %s: Thunderbolt clock still acknowledged\n", c->name);
	/* 4. (CD clock code), 5. clocks deselected and gated */
	rmw(c, clock_ctl(c), XELPDP_DDI_CLOCK_SELECT_MASK(c->ver) | XELPDP_FORWARD_CLOCK_UNGATE, 0);
	/* 6. no DDI clock */
	wr(c, DDI_CLK_VALFREQ(c->port), 0);
}

/* The port buffer's I/O goes to the Thunderbolt controller or the PHY. */
static void io_select_tbt(struct lt *c, int tbt)
{
	rmw(c, buf_ctl1(c), XELPDP_PORT_BUF_IO_SELECT_TBT, tbt ? XELPDP_PORT_BUF_IO_SELECT_TBT : 0);
}

int lt_phy_pll_enable(struct i915_device *i915, struct intel_output *o, int lanes)
{
	struct lt c;
	struct lt_port_pll *p;
	int ret;

	if (!o || !port_ok(o->port))
		return -EINVAL;
	lt_setup(&c, i915, o->port, o);
	p = &lt_port[o->port];
	if (!p->in_use) {
		kprintf("[drm] i915: port %s: PHY PLL enabled without settings\n", c.name);
		return -EINVAL;
	}
	if (lanes < 1 || lanes > 4)
		lanes = p->is_dp ? (p->lane_count ? p->lane_count : 4) : 4;
	p->lane_count = lanes;
	if (c.tbt) {
		if (!p->tbt)
			return -EINVAL;
		/* the I/O goes to Thunderbolt before its clock is taken */
		io_select_tbt(&c, 1);
		ret = tbt_clock_enable(&c, p->port_clock);
	} else {
		if (p->tbt)
			return -EINVAL;
		ret = ltpll_enable(&c, p, lanes);
		io_select_tbt(&c, 0);
	}
	i915_dbg("[drm] i915: port %s: clock on (%u kHz, %d lanes): CLOCK_CTL %08x BUF_CTL1 %08x BUF_CTL2 %08x\n",
		 c.name, p->port_clock, lanes, rd(&c, clock_ctl(&c)), rd(&c, buf_ctl1(&c)),
		 rd(&c, buf_ctl2(&c)));
	return ret;
}

void lt_phy_pll_disable(struct i915_device *i915, struct intel_output *o)
{
	struct lt c;

	if (!o || !port_ok(o->port))
		return;
	lt_setup(&c, i915, o->port, o);
	if (c.tbt)
		tbt_clock_disable(&c);
	else
		ltpll_disable(&c);
	/* de-select Thunderbolt */
	io_select_tbt(&c, 0);
}

/* ---- signal levels ---------------------------------------------------------------- */

static int output_is_dp(const struct intel_output *o)
{
	return o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP;
}

/* The table for the link: DP 2.1 for the 128b/132b rates, the eDP one
 * for a panel, else DisplayPort 1.4 (which HDMI uses too). */
void lt_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	struct lt c;
	const struct lt_trans *t;
	int n, lane_count;
	int is_dp;
	uint32_t clock;

	if (!o || !port_ok(o->port))
		return;
	lt_setup(&c, i915, o->port, o);
	if (c.tbt)
		return; /* Thunderbolt drives the lanes itself */
	is_dp = output_is_dp(o);
	clock = o->link_rate_khz;
	if (!clock)
		clock = lt_port[o->port].port_clock;

	if (is_dp && dp_is_uhbr(clock)) {
		t = lt_trans_uhbr;
		n = ARRAY_LEN(lt_trans_uhbr);
	} else if (is_dp && c.is_edp) {
		t = lt_trans_edp;
		n = ARRAY_LEN(lt_trans_edp);
	} else {
		t = lt_trans_dp14;
		n = ARRAY_LEN(lt_trans_dp14);
	}
	if (is_dp) {
		if (level < 0)
			level = 0;
		if (level >= n)
			level = n - 1;
		lane_count = o->lane_count ? o->lane_count : 4;
	} else {
		if (level < 0 || level >= n)
			level = LT_TRANS_HDMI_DEFAULT;
		lane_count = 4;
	}

	lt_transaction_begin(&c);
	/* per transmitter of the link: swing, pre-, main and post-cursor */
	for (int ln = 0; ln < lane_count; ln++) {
		int lane = ln / 2, tx = ln % 2;
		uint8_t lane_mask = lane == 0 ? LT_LANE0 : LT_LANE1;

		if (!(lane_mask & c.owned))
			continue;
		lt_rmw(&c, lane_mask, (uint16_t)LT_PHY_TXY_CTL8(tx),
		       LT_PHY_TX_SWING_LEVEL_MASK | LT_PHY_TX_SWING_MASK,
		       (uint8_t)(LT_PHY_TX_SWING_LEVEL(t[level].txswing_level) |
				 LT_PHY_TX_SWING(t[level].txswing)),
		       MB_WRITE_COMMITTED);
		lt_rmw(&c, lane_mask, (uint16_t)LT_PHY_TXY_CTL2(tx), LT_PHY_TX_CURSOR_MASK,
		       (uint8_t)LT_PHY_TX_CURSOR(t[level].pre_cursor), MB_WRITE_COMMITTED);
		lt_rmw(&c, lane_mask, (uint16_t)LT_PHY_TXY_CTL3(tx), LT_PHY_TX_CURSOR_MASK,
		       (uint8_t)LT_PHY_TX_CURSOR(t[level].main_cursor), MB_WRITE_COMMITTED);
		lt_rmw(&c, lane_mask, (uint16_t)LT_PHY_TXY_CTL4(tx), LT_PHY_TX_CURSOR_MASK,
		       (uint8_t)LT_PHY_TX_CURSOR(t[level].post_cursor), MB_WRITE_COMMITTED);
	}
}

/* ---- what the firmware left running ----------------------------------------------- */

/* The PLL state as the PHY has it.  Only the vendor configuration words
 * are reliable: the PHY does not restore the windows' shadow copies after
 * power gating, so the data read back may be stale. */
static void lt_readout(struct lt *c, struct lt_pll *s)
{
	uint8_t lane = (c->owned & LT_LANE0) ? LT_LANE0 : LT_LANE1;

	lt_transaction_begin(c);
	mm_memset(s, 0, sizeof(*s));
	s->config[0] = lt_read(c, lane, LT_PHY_VDR_0_CONFIG);
	s->config[1] = lt_read(c, LT_LANE0, LT_PHY_VDR_1_CONFIG);
	s->config[2] = lt_read(c, lane, LT_PHY_VDR_2_CONFIG);
	for (int i = 0; i <= 12; i++)
		for (int j = 3, k = 0; j >= 0; j--, k++)
			s->data[i][k] = lt_read(c, LT_LANE0, (uint16_t)LT_PHY_VDR_X_DATAY(i, j));
}

uint32_t lt_phy_port_link_rate(struct i915_device *i915, struct intel_output *o)
{
	struct lt c;
	struct lt_pll s;
	uint32_t val, sel, rate;

	if (!o || !port_ok(o->port))
		return 0;
	lt_setup(&c, i915, o->port, o);
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
	if (!lt_pll_is_enabled(&c))
		return 0;
	if (c.tbt)
		return 0; /* the PHY clock of a port now in Thunderbolt mode */
	lt_readout(&c, &s);
	rate = lt_port_clock(&s);
	i915_dbg("[drm] i915: port %s: firmware LT PLL: %s %u kHz (config %02x %02x %02x, SSC %s)\n",
		 c.name, lt_state_is_dp(&s) ? "DP" : lt_state_is_hdmi(&s) ? "HDMI" : "?", rate,
		 s.config[0], s.config[1], s.config[2],
		 (val & XELPDP_SSC_ENABLE_PLLA) ? "on" : "off");
	return rate;
}

/* ---- once at display init ------------------------------------------------------------ */

static void dump_state(const char *name, const struct lt_pll *s)
{
	i915_dbg("[drm] i915: LT PLL state %s: config %02x %02x %02x\n", name, s->config[0],
		 s->config[1], s->config[2]);
	for (int i = 0; i <= 12; i++)
		i915_dbg("[drm] i915:   window %2d: %02x%02x = %02x%02x%02x%02x\n", i,
			 s->addr_msb[i], s->addr_lsb[i], s->data[i][0], s->data[i][1],
			 s->data[i][2], s->data[i][3]);
}

static int verify_clock(uint32_t clock, const char *name, const struct lt_pll *s,
			int precomputed)
{
	uint32_t clk = lt_port_clock(s);

	if (clock_matches(clk, clock))
		return 0;
	kprintf("[drm] i915: LT PLL state %s (%s): computed %u kHz, listed %u kHz\n", name,
		precomputed ? "table" : "computed", clk, clock);
	dump_state(name, s);
	return 1;
}

/* Every table entry produces the clock it is listed for, and so do the
 * computed HDMI settings for the HDMI entries' clocks. */
static void verify_tables(void)
{
	static const struct lt_entry *const all[] = {
		lt_dp_tables, lt_edp_tables, lt_hdmi_tables,
	};
	int bad = 0;

	for (int i = 0; i < ARRAY_LEN(all); i++) {
		for (const struct lt_entry *t = all[i]; t->state; t++) {
			struct lt_pll s;

			bad += verify_clock(t->clock, t->name, t->state, 1);
			if (all[i] != lt_hdmi_tables)
				continue;
			mm_memset(&s, 0, sizeof(s));
			if (lt_calculate_hdmi_state(&s, t->clock))
				continue;
			bad += verify_clock(t->clock, t->name, &s, 0);
		}
	}
	if (!bad)
		i915_dbg("[drm] i915: LT PHY PLL tables check out\n");
}

int lt_phy_init(struct i915_device *i915)
{
	/* DDI A and B, then the four Type-C ports */
	static const int ports[] = { PORT_A, PORT_B, PORT_TC1, PORT_TC1 + 1, PORT_TC1 + 2,
				     PORT_TC1 + 3 };

	mm_memset(lt_port, 0, sizeof(lt_port));
	msgbus_reported = 0;
	p2p_reported = 0;

	verify_tables();

	for (int i = 0; i < ARRAY_LEN(ports); i++) {
		int port = ports[i];
		struct lt c;

		lt_setup(&c, i915, port, NULL);
		i915_dbg("[drm] i915: port %s (LT): CLOCK_CTL %08x BUF_CTL1 %08x BUF_CTL2 %08x BUF_CTL5 %08x VALFREQ %u\n",
			 c.name, rd(&c, clock_ctl(&c)), rd(&c, buf_ctl1(&c)), rd(&c, buf_ctl2(&c)),
			 rd(&c, buf_ctl5(&c)), rd(&c, DDI_CLK_VALFREQ(port)));
	}
	return 0;
}
