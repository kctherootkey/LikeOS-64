// LikeOS -- Type-C ports (Ice Lake to Meteor Lake).
//
// A Type-C port's PHY sits behind the flexible I/O adapter (FIA), which
// muxes the connector between a DisplayPort alternate-mode sink, the
// Thunderbolt controller and a legacy (fixed) connector.  Before the
// port can be driven the display must take the PHY from the FIA, learn
// which mode the connector is in and how many lanes it was granted.
// The port's clock is then either its own MG PLL (Ice Lake) / DKL PLL
// (Tiger Lake, Alder Lake-P) in the PHY, or the shared Thunderbolt PLL.
// The signal levels are register writes into the PHY's per-lane words.
//
// The handshake differs per generation.  Ice Lake and Tiger Lake claim
// the PHY through the FIA's "not safe" bit once the FIA reports the mode
// switch complete; Alder Lake-P claims it through the port's buffer
// control and asks the Type-C subsystem whether the PHY is ready;
// Meteor Lake first asks the subsystem for power (PICA's port buffer
// control), then claims the PHY there, and reads the pin assignment to
// learn the lane count.  Whatever the generation, the subsystem must be
// kept out of its cold state while the display uses the PHY: by a pcode
// request on Tiger Lake, by holding the port's AUX power on Ice Lake
// (legacy ports) and Alder Lake-P, by the power request on Meteor Lake.
// On Meteor Lake the PHY's PLL, clocking and levels are the C10/C20
// code's (intel_cx0_phy.c); the PLL entry points here do nothing there.
//
// None of this has run on hardware: the ThinkPad P50 this driver was
// written on has no Type-C PHY.  The sequences follow the programming
// guides; every step reports what it found.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

static int is_dkl(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_TGL;
}

static int is_xelpdp(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_MTL;
}

/* Alder Lake-P's handshake (display version 13). */
static int is_adlp(struct i915_device *i915)
{
	return i915->info->display_ver == 13 && !is_xelpdp(i915);
}

/* The FIA that carries the port: Ice Lake has one for all ports; Tiger
 * Lake says whether its FIAs are "modular" (one per pair of ports); from
 * Alder Lake-P they always are. */
static void fia_of(struct i915_device *i915, struct intel_output *o)
{
	int modular;
	if (i915->info->display_ver == 11)
		modular = 0;
	else if (i915->info->display_ver >= 13)
		modular = 1;
	else
		modular = !!(i915_read32(i915, PORT_TX_DFLEXDPSP(0)) & MODULAR_FIA_MASK);
	if (modular) {
		o->tc_fia = o->tc_index / 2;
		o->tc_fia_idx = o->tc_index % 2;
	} else {
		o->tc_fia = 0;
		o->tc_fia_idx = o->tc_index;
	}
}

/* ---- the Dekel PHY's banked registers ------------------------------------------ */

/* A Dekel PHY shows 4 KB of its registers at a time; the bank (the PHY
 * address bits above 12) goes into the port's byte of HIP_INDEX first.
 * The index and the access that follows are one operation for whoever
 * else reaches into a Dekel PHY (the AUX power well's health check, the
 * PLL, the levels): one lock keeps them together. */
static spinlock_t dkl_lock = SPINLOCK_INIT("dkl phy");

static uint32_t dkl_select(struct i915_device *i915, int tc, uint32_t off)
{
	i915_write32(i915, DKL_HIP_INDEX_REG(tc), DKL_PHY_BANK(off) << DKL_HIP_INDEX_SHIFT(tc));
	return DKL_PHY_BASE(tc) + DKL_PHY_WINDOW(off);
}

uint32_t intel_dkl_phy_read(struct i915_device *i915, int tc, uint32_t off)
{
	uint32_t v;
	spin_lock(&dkl_lock);
	v = i915_read32(i915, dkl_select(i915, tc, off));
	spin_unlock(&dkl_lock);
	return v;
}

void intel_dkl_phy_write(struct i915_device *i915, int tc, uint32_t off, uint32_t val)
{
	spin_lock(&dkl_lock);
	i915_write32(i915, dkl_select(i915, tc, off), val);
	spin_unlock(&dkl_lock);
}

/* Read, clear, set, and write back only when something changed. */
void intel_dkl_phy_rmw(struct i915_device *i915, int tc, uint32_t off, uint32_t clear, uint32_t set)
{
	spin_lock(&dkl_lock);
	uint32_t reg = dkl_select(i915, tc, off);
	uint32_t old = i915_read32(i915, reg);
	uint32_t v = (old & ~clear) | set;
	if (v != old)
		i915_write32(i915, reg, v);
	spin_unlock(&dkl_lock);
}

static int wait_bit(struct i915_device *i915, uint32_t reg, uint32_t bit, int set, int ms)
{
	for (int t = 0; t < ms * 100; t++) {
		if (!!(i915_read32(i915, reg) & bit) == set)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* ---- Type-C cold ------------------------------------------------------------------ */

/* Tiger Lake: the Type-C subsystem is kept out of its cold state by a
 * pcode request, counted here.  A failed block is retried. */
static int tgl_tc_cold(struct i915_device *i915, int block)
{
	int rc = -1;
	for (int tries = 0; tries < 3 && rc; tries++) {
		uint32_t low = block ? TGL_PCODE_EXIT_TCCOLD_DATA_L_BLOCK_REQ :
				       TGL_PCODE_EXIT_TCCOLD_DATA_L_UNBLOCK_REQ;
		uint32_t high = 0;
		rc = intel_pcode_read(i915, TGL_PCODE_TCCOLD, &low, &high);
		if (rc == 0 && block && (low & TGL_PCODE_EXIT_TCCOLD_DATA_L_EXIT_FAILED))
			rc = -EIO;
		if (rc)
			lapic_delay_ms(1);
	}
	if (rc)
		kprintf("[drm] i915: Type-C cold %sblock failed\n", block ? "" : "un");
	return rc;
}

static int has_tc_cold_pcode(struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_TIGERLAKE;
}

/* The domain whose power keeps the subsystem out of TC-cold for this
 * port in mode `mode', or -1 where that is not a power well: Ice Lake's
 * legacy ports and Alder Lake-P's DP-alt/legacy ports hold their AUX
 * channel up. */
static int tc_cold_domain(struct i915_device *i915, struct intel_output *o, int mode)
{
	if (i915->info->display_ver == 11 && mode == INTEL_TC_LEGACY)
		return INTEL_PW_AUX_A + o->port;
	if (is_adlp(i915) && mode != INTEL_TC_TBT_ALT)
		return INTEL_PW_AUX_A + o->port;
	return -1;
}

static void tc_cold_block(struct i915_device *i915, struct intel_output *o, int mode)
{
	struct intel_display *d = &i915->display;
	int dom = tc_cold_domain(i915, o, mode);

	if (dom >= 0) {
		intel_power_get(i915, (enum intel_power_domain)dom);
	} else if (has_tc_cold_pcode(i915)) {
		if (d->tc_cold_refs++ == 0)
			tgl_tc_cold(i915, 1);
	}
}

static void tc_cold_unblock(struct i915_device *i915, struct intel_output *o, int mode)
{
	struct intel_display *d = &i915->display;
	int dom = tc_cold_domain(i915, o, mode);

	if (dom >= 0) {
		intel_power_put(i915, (enum intel_power_domain)dom);
	} else if (has_tc_cold_pcode(i915)) {
		if (d->tc_cold_refs > 0 && --d->tc_cold_refs == 0)
			tgl_tc_cold(i915, 0);
	}
}

/* ---- the connector's live state ------------------------------------------------- */

#define LIVE_DP_ALT (1u << INTEL_TC_DP_ALT)
#define LIVE_TBT (1u << INTEL_TC_TBT_ALT)
#define LIVE_LEGACY (1u << INTEL_TC_LEGACY)

/* A legacy port is one the VBT does not mark as USB-C or Thunderbolt;
 * without that information, any port showing the PCH's pin is. */
static void legacy_init(struct i915_device *i915, struct intel_output *o)
{
	struct intel_vbt *vbt = &i915->display.vbt;
	if (o->tc_legacy_known)
		return;
	o->tc_legacy_known = 1;
	o->tc_legacy = 0;
	if (vbt->valid && o->port >= 0 && o->port < INTEL_MAX_PORTS &&
	    vbt->port[o->port].typec_valid)
		o->tc_legacy = !vbt->port[o->port].dp_usb_type_c && !vbt->port[o->port].tbt;
	else
		o->tc_legacy = -1; /* unknown: the live state decides */
}

/* Which modes a sink shows up in, one bit per enum intel_tc_mode. */
static uint32_t live_status(struct i915_device *i915, struct intel_output *o)
{
	uint32_t mask = 0;
	uint32_t pch = i915_read32(i915, SDEISR);
	uint32_t pch_bit = ICP_SDE_TC_HOTPLUG(o->tc_index);

	if (is_xelpdp(i915)) {
		uint32_t pica = i915_read32(i915, PICAINTERRUPT_ISR);
		if (pica & XELPDP_DP_ALT_HOTPLUG(o->tc_index))
			mask |= LIVE_DP_ALT;
		if (pica & XELPDP_TBT_HOTPLUG(o->tc_index))
			mask |= LIVE_TBT;
		if (o->tc_legacy != 0 && (pch & pch_bit))
			mask |= LIVE_LEGACY;
		return mask;
	}
	if (is_adlp(i915)) {
		uint32_t cpu = i915_read32(i915, GEN11_DE_HPD_ISR);
		if (cpu & GEN11_TC_HOTPLUG(o->tc_index))
			mask |= LIVE_DP_ALT;
		if (cpu & GEN11_TBT_HOTPLUG(o->tc_index))
			mask |= LIVE_TBT;
		if (pch & pch_bit)
			mask |= LIVE_LEGACY;
		return mask;
	}
	/* Ice Lake, Tiger Lake: the FIA's live state, readable only out of
	 * TC-cold */
	tc_cold_block(i915, o, INTEL_TC_DP_ALT);
	uint32_t fia = i915_read32(i915, PORT_TX_DFLEXDPSP(o->tc_fia));
	pch = i915_read32(i915, SDEISR);
	tc_cold_unblock(i915, o, INTEL_TC_DP_ALT);
	if (fia == 0xffffffffu)
		return 0; /* still cold: nothing connected */
	if (fia & TC_LIVE_STATE_TBT(o->tc_fia_idx))
		mask |= LIVE_TBT;
	if (fia & TC_LIVE_STATE_TC(o->tc_fia_idx))
		mask |= LIVE_DP_ALT;
	if (pch & pch_bit)
		mask |= LIVE_LEGACY;
	return mask;
}

/* A live state in exactly one mode that the legacy flag says cannot
 * be is believed over the flag. */
static void fixup_legacy(struct intel_output *o, uint32_t mask)
{
	if (!mask || (mask & (mask - 1)))
		return;
	if (o->tc_legacy < 0) {
		o->tc_legacy = mask == LIVE_LEGACY;
		return;
	}
	uint32_t valid = o->tc_legacy ? LIVE_LEGACY : (LIVE_DP_ALT | LIVE_TBT);
	if (mask & ~valid)
		o->tc_legacy = !o->tc_legacy;
}

/* The highest mode present: legacy over DP-alt over Thunderbolt. */
static int mask_to_mode(uint32_t mask)
{
	if (mask & LIVE_LEGACY)
		return INTEL_TC_LEGACY;
	if (mask & LIVE_DP_ALT)
		return INTEL_TC_DP_ALT;
	if (mask & LIVE_TBT)
		return INTEL_TC_TBT_ALT;
	return INTEL_TC_DISCONNECTED;
}

int intel_tc_hpd_live(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc)
		return 0;
	fia_of(i915, o);
	legacy_init(i915, o);
	return live_status(i915, o) != 0;
}

/* ---- the PHY's readiness and ownership ---------------------------------------- */

static int phy_ready(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v;
	if (i915->info->display_ver >= 13) {
		/* the IOM firmware: ready to hand the PHY over */
		v = i915_read32(i915, TCSS_DDI_STATUS(o->tc_index));
		if (v == 0xffffffffu)
			return 0;
		return !!(v & TCSS_DDI_STATUS_READY);
	}
	v = i915_read32(i915, PORT_TX_DFLEXDPPMS(o->tc_fia));
	if (v == 0xffffffffu)
		return 0;
	return !!(v & DP_PHY_MODE_STATUS_COMPLETED(o->tc_fia_idx));
}

static int take_ownership(struct i915_device *i915, struct intel_output *o, int take)
{
	uint32_t reg, bit, v;

	if (is_xelpdp(i915)) {
		reg = XELPDP_PORT_BUF_CTL1_VER(i915->info->display_ver, o->port);
		bit = XELPDP_TC_PHY_OWNERSHIP;
	} else if (is_adlp(i915)) {
		reg = DDI_BUF_CTL(o->port);
		bit = DDI_BUF_CTL_TC_PHY_OWNERSHIP;
	} else {
		reg = PORT_TX_DFLEXDPCSSS(o->tc_fia);
		bit = DP_PHY_MODE_STATUS_NOT_SAFE(o->tc_fia_idx);
	}
	v = i915_read32(i915, reg);
	if (v == 0xffffffffu)
		return -EIO; /* in TC-cold */
	v = take ? (v | bit) : (v & ~bit);
	i915_write32(i915, reg, v);
	return 0;
}

/* Display version 30: the Type-C subsystem is told of every power
 * request change through its mailbox first, or its receiver-detect
 * clock handshake can be violated on the way into its low-power states. */
static void xelpdp_tc_power_request_wa(struct i915_device *i915, int enable)
{
	if (wait_bit(i915, TCSS_DISP_MAILBOX_IN_CMD, TCSS_DISP_MAILBOX_IN_CMD_RUN_BUSY, 0, 10)) {
		i915_dbg("[drm] i915: Type-C mailbox busy; power request not announced\n");
		return;
	}
	i915_write32(i915, TCSS_DISP_MAILBOX_IN_DATA, enable ? 1 : 0);
	i915_write32(i915, TCSS_DISP_MAILBOX_IN_CMD,
		     TCSS_DISP_MAILBOX_IN_CMD_RUN_BUSY | TCSS_DISP_MAILBOX_IN_CMD_DATA(0x1));
	if (wait_bit(i915, TCSS_DISP_MAILBOX_IN_CMD, TCSS_DISP_MAILBOX_IN_CMD_RUN_BUSY, 0, 10))
		i915_dbg("[drm] i915: Type-C mailbox did not take the power request\n");
}

/* Meteor Lake and later: the Type-C subsystem's power for the PHY,
 * requested in PICA's port buffer control; the PHY must also report
 * ready. */
static int xelpdp_tcss_power(struct i915_device *i915, struct intel_output *o, int on)
{
	uint32_t reg = XELPDP_PORT_BUF_CTL1_VER(i915->info->display_ver, o->port);
	uint32_t v;

	if (i915->info->display_ver == 30)
		xelpdp_tc_power_request_wa(i915, on);
	v = i915_read32(i915, reg);
	i915_write32(i915, reg, on ? (v | XELPDP_TCSS_POWER_REQUEST) :
				     (v & ~XELPDP_TCSS_POWER_REQUEST));
	if (on) {
		int ready = 0;
		for (int t = 0; t < 500; t++) {
			if (phy_ready(i915, o)) {
				ready = 1;
				break;
			}
			lapic_delay_ms(1);
		}
		if (!ready) {
			kprintf("[drm] i915: port %s: timeout waiting for the Type-C PHY to be ready\n",
				intel_port_name(i915, o->port));
			goto off;
		}
	}
	for (int t = 0; t < 25; t++) {
		if (!!(i915_read32(i915, reg) & XELPDP_TCSS_POWER_STATE) == !!on)
			return 0;
		lapic_delay_us(200);
	}
	i915_dbg("[drm] i915: port %s: Type-C subsystem power did not turn %s\n",
		 intel_port_name(i915, o->port), on ? "on" : "off");
	if (!on)
		return -ETIMEDOUT;
off:
	if (i915->info->display_ver == 30)
		xelpdp_tc_power_request_wa(i915, 0);
	v = i915_read32(i915, reg);
	i915_write32(i915, reg, v & ~XELPDP_TCSS_POWER_REQUEST);
	wait_bit(i915, reg, XELPDP_TCSS_POWER_STATE, 0, 5);
	return -ETIMEDOUT;
}

/* ---- lanes ------------------------------------------------------------------- */

/* The DP-alt pin assignment the PHY was set up for: 0 none, 1 = A ..
 * 6 = F. */
static uint32_t pin_assignment(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v;

	/* from display version 20 the Type-C subsystem reports it */
	if (i915->info->display_ver >= 20) {
		v = i915_read32(i915, TCSS_DDI_STATUS(o->tc_index));
		if (v == 0xffffffffu)
			return 0;
		return (v & TCSS_DDI_STATUS_PIN_ASSIGNMENT_MASK) >> TCSS_DDI_STATUS_PIN_ASSIGNMENT_SHIFT;
	}
	v = i915_read32(i915, PORT_TX_DFLEXPA1(o->tc_fia));
	if (v == 0xffffffffu)
		return 0;
	return (v & DP_PIN_ASSIGNMENT_MASK(o->tc_fia_idx)) >> DP_PIN_ASSIGNMENT_SHIFT(o->tc_fia_idx);
}

int intel_tc_pin_assignment(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc || o->tc_mode == INTEL_TC_TBT_ALT)
		return 0;
	return (int)pin_assignment(i915, o);
}

/* The lanes the FIA granted in DP-alt mode, as a mask. */
static uint32_t lane_mask(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPSP(o->tc_fia));
	return (v & DP_LANE_ASSIGNMENT_MASK(o->tc_fia_idx)) >> DP_LANE_ASSIGNMENT_SHIFT(o->tc_fia_idx);
}

int intel_tc_max_lanes(struct i915_device *i915, struct intel_output *o)
{
	if (o->tc_mode != INTEL_TC_DP_ALT)
		return 4;
	if (is_xelpdp(i915)) {
		/* pin assignment D carries two lanes, C and E four */
		switch (o->tc_pin_assignment) {
		case 0: return 0;
		case 3: case 5: return 4;
		default: return 2;
		}
	}
	switch (lane_mask(i915, o)) {
	case 0x1: case 0x2: case 0x4: case 0x8: return 1;
	case 0x3: case 0xC: return 2;
	case 0xF: return 4;
	default: return 1;
	}
}

/* Tell the FIA how many of its lanes the port will drive (not on
 * Meteor Lake, where the PHY takes them from the pin assignment). */
static void set_fia_lanes(struct i915_device *i915, struct intel_output *o, int lanes)
{
	uint32_t v, field;

	if (is_xelpdp(i915))
		return;
	v = i915_read32(i915, PORT_TX_DFLEXDPMLE1(o->tc_fia));
	v &= ~DFLEXDPMLE1_DPMLETC_MASK(o->tc_fia_idx);
	switch (lanes) {
	case 1: field = o->lane_reversal ? 0x8 : 0x1; break;
	case 2: field = o->lane_reversal ? 0xC : 0x3; break;
	default: field = 0xF; break;
	}
	v |= DFLEXDPMLE1_DPMLETC(o->tc_fia_idx, field);
	i915_write32(i915, PORT_TX_DFLEXDPMLE1(o->tc_fia), v);
}

/* ---- connect / disconnect --------------------------------------------------- */

/* A DP-alt sink must still be there once the PHY is the display's, and
 * have at least one lane; a legacy connector is always four lanes. */
static int verify_mode(struct i915_device *i915, struct intel_output *o, int mode)
{
	if (mode == INTEL_TC_LEGACY)
		return 1;
	if (!(live_status(i915, o) & LIVE_DP_ALT)) {
		i915_dbg("[drm] i915: port %s: the Type-C sink went away\n",
			 intel_port_name(i915, o->port));
		return 0;
	}
	return intel_tc_max_lanes(i915, o) >= 1;
}

/* Bring the PHY into `mode'; returns 1 when it is the display's. */
static int phy_connect(struct i915_device *i915, struct intel_output *o, int mode)
{
	o->tc_mode = mode;
	o->tc_pin_assignment = 0;
	if (mode == INTEL_TC_TBT_ALT) {
		/* the Thunderbolt controller owns the PHY */
		tc_cold_block(i915, o, mode);
		return 1;
	}
	if (is_xelpdp(i915)) {
		if (xelpdp_tcss_power(i915, o, 1))
			goto fail;
		take_ownership(i915, o, 1);
		o->tc_pin_assignment = (int)pin_assignment(i915, o);
		if (!verify_mode(i915, o, mode)) {
			take_ownership(i915, o, 0);
			xelpdp_tcss_power(i915, o, 0);
			goto fail;
		}
		return 1;
	}
	if (is_adlp(i915)) {
		/* the ownership bit is in the port's buffer control, which
		 * needs the port's lanes powered */
		enum intel_power_domain lanes = (enum intel_power_domain)(INTEL_PW_DDI_A + o->port);
		intel_power_get(i915, lanes);
		if (take_ownership(i915, o, 1) && mode != INTEL_TC_LEGACY) {
			intel_power_put(i915, lanes);
			goto fail;
		}
		if (!phy_ready(i915, o) && mode != INTEL_TC_LEGACY) {
			take_ownership(i915, o, 0);
			intel_power_put(i915, lanes);
			goto fail;
		}
		tc_cold_block(i915, o, mode);
		o->tc_pin_assignment = (int)pin_assignment(i915, o);
		if (!verify_mode(i915, o, mode)) {
			tc_cold_unblock(i915, o, mode);
			take_ownership(i915, o, 0);
			intel_power_put(i915, lanes);
			goto fail;
		}
		intel_power_put(i915, lanes);
		return 1;
	}
	/* Ice Lake, Tiger Lake */
	tc_cold_block(i915, o, mode);
	int ready = 0;
	for (int t = 0; t < 10; t++) {
		if (phy_ready(i915, o)) {
			ready = 1;
			break;
		}
		lapic_delay_ms(1);
	}
	if ((!ready || take_ownership(i915, o, 1)) && mode != INTEL_TC_LEGACY) {
		kprintf("[drm] i915: port %s: can't take the Type-C PHY (ready %d)\n",
			intel_port_name(i915, o->port), ready);
		tc_cold_unblock(i915, o, mode);
		goto fail;
	}
	o->tc_pin_assignment = (int)pin_assignment(i915, o);
	if (!verify_mode(i915, o, mode)) {
		take_ownership(i915, o, 0);
		tc_cold_unblock(i915, o, mode);
		goto fail;
	}
	return 1;
fail:
	o->tc_mode = INTEL_TC_DISCONNECTED;
	return 0;
}

static void phy_disconnect(struct i915_device *i915, struct intel_output *o)
{
	int mode = o->tc_mode;

	if (is_xelpdp(i915)) {
		if (mode == INTEL_TC_LEGACY || mode == INTEL_TC_DP_ALT) {
			take_ownership(i915, o, 0);
			xelpdp_tcss_power(i915, o, 0);
		}
		tc_cold_unblock(i915, o, mode);
		return;
	}
	if (is_adlp(i915)) {
		enum intel_power_domain lanes = (enum intel_power_domain)(INTEL_PW_DDI_A + o->port);
		intel_power_get(i915, lanes);
		tc_cold_unblock(i915, o, mode);
		if (mode == INTEL_TC_LEGACY || mode == INTEL_TC_DP_ALT)
			take_ownership(i915, o, 0);
		intel_power_put(i915, lanes);
		return;
	}
	if (mode == INTEL_TC_LEGACY || mode == INTEL_TC_DP_ALT)
		take_ownership(i915, o, 0);
	tc_cold_unblock(i915, o, mode);
}

/* Take the PHY for the connector's current mode.  Returns 0 with
 * o->tc_mode set, or -ENODEV when nothing is there. */
int intel_tc_connect(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc)
		return 0;
	/* Wildcat Lake has two Type-C ports */
	if (i915->info->platform == I915_PLATFORM_WILDCATLAKE && o->tc_index >= 2)
		return -ENODEV;
	fia_of(i915, o);
	legacy_init(i915, o);
	uint32_t mask = live_status(i915, o);
	fixup_legacy(o, mask);
	int mode = mask_to_mode(mask);
	if (mode == INTEL_TC_DISCONNECTED) {
		if (o->tc_owned)
			intel_tc_disconnect(i915, o);
		o->tc_mode = mode;
		return -ENODEV;
	}
	if (o->tc_owned && o->tc_mode == mode)
		return 0;
	if (o->tc_owned)
		intel_tc_disconnect(i915, o);
	int ok = phy_connect(i915, o, mode);
	/* a port that will not go into the mode its sink shows falls back
	 * to its default one: legacy for a legacy port, else Thunderbolt */
	int fallback = o->tc_legacy > 0 ? INTEL_TC_LEGACY : INTEL_TC_TBT_ALT;
	if (!ok && mode != fallback)
		ok = phy_connect(i915, o, fallback);
	if (!ok) {
		kprintf("[drm] i915: port %s: the Type-C PHY could not be connected\n",
			intel_port_name(i915, o->port));
		return -ENODEV;
	}
	o->tc_owned = 1;
	o->aux.tbt_io = (o->tc_mode == INTEL_TC_TBT_ALT);
	o->tc_lanes = (uint32_t)intel_tc_max_lanes(i915, o);
	i915_dbg("[drm] i915: port %s: Type-C in %s mode, %u lanes (pin assignment %d)\n",
		 intel_port_name(i915, o->port),
		 o->tc_mode == INTEL_TC_DP_ALT ? "DP-alt" :
		 o->tc_mode == INTEL_TC_TBT_ALT ? "Thunderbolt" : "legacy",
		 o->tc_lanes, o->tc_pin_assignment);
	return 0;
}

void intel_tc_disconnect(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc || !o->tc_owned)
		return;
	phy_disconnect(i915, o);
	o->tc_owned = 0;
	o->tc_mode = INTEL_TC_DISCONNECTED;
	o->aux.tbt_io = 0;
}

/* ---- the MG/DKL PLL ------------------------------------------------------------- */

/* A Type-C port's own PLL in its PHY: the MG PHY's (Ice Lake) or the
 * Dekel PHY's (Tiger Lake, Alder Lake-P), the same design with its
 * registers in other places.  The DCO is 8.1 GHz for DisplayPort -- a
 * multiple of every standard link rate -- or between 8 and 10 GHz for
 * HDMI, fed through two post dividers (div1 from {7, 5, 3, 2}, div2 from
 * 10 down to 1) and a fixed 5 to the port. */
struct tc_pll {
	uint32_t refclkin_ctl, coreclkctl1, hsclkctl;
	uint32_t div0, div1, lf, frac_lock, ssc, bias, tdc_coldst_bias;
	uint32_t bias_mask, tdc_mask;
};

static uint32_t ref_khz(struct i915_device *i915)
{
	switch (i915_read32(i915, DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: return 19200;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: return 38400;
	default: return 24000;
	}
}

static int tc_pll_divisors(uint32_t clock_khz, int is_dp, int dkl, uint32_t *dco_khz,
			   struct tc_pll *s)
{
	static const uint8_t div1_vals[] = { 7, 5, 3, 2 };
	const int use_ssc = 0;
	uint32_t dco_min = is_dp ? 8100000 : use_ssc ? 8000000 : 7992000;
	uint32_t dco_max = is_dp ? 8100000 : 10000000;

	for (unsigned i = 0; i < sizeof(div1_vals); i++) {
		uint32_t div1 = div1_vals[i];
		for (uint32_t div2 = 10; div2 > 0; div2--) {
			uint64_t dco = (uint64_t)div1 * div2 * clock_khz * 5;
			uint32_t a_divratio, tlinedrv, inputsel, hsdiv;
			if (dco < dco_min || dco > dco_max)
				continue;
			if (div2 >= 2) {
				/* the values that work for DP-alt, not what the
				 * Tiger Lake algorithm would give */
				a_divratio = is_dp ? 10 : 5;
				tlinedrv = dkl ? 1 : 2;
			} else {
				a_divratio = 5;
				tlinedrv = 0;
			}
			inputsel = is_dp ? 0 : 1;
			switch (div1) {
			case 2: hsdiv = MGDKL_HSCLKCTL_HSDIV_RATIO_2; break;
			case 3: hsdiv = MGDKL_HSCLKCTL_HSDIV_RATIO_3; break;
			case 5: hsdiv = MGDKL_HSCLKCTL_HSDIV_RATIO_5; break;
			default: hsdiv = MGDKL_HSCLKCTL_HSDIV_RATIO_7; break;
			}
			*dco_khz = (uint32_t)dco;
			s->refclkin_ctl = MGDKL_REFCLKIN_CTL_OD_2_MUX(1);
			s->coreclkctl1 = MGDKL_CORECLKCTL1_A_DIVRATIO(a_divratio);
			s->hsclkctl = MGDKL_HSCLKCTL_TLINEDRV_CLKSEL(tlinedrv) |
				      MGDKL_HSCLKCTL_CORE_INPUTSEL(inputsel) | hsdiv |
				      MGDKL_HSCLKCTL_DSDIV_RATIO(div2);
			return 0;
		}
	}
	return -EINVAL;
}

/* The specification works in real numbers; this is the same in integer
 * arithmetic, divisions put off as long as they can be. */
static int tc_pll_calc(struct i915_device *i915, uint32_t clock_khz, int is_dp,
		       struct tc_pll *s)
{
	uint32_t refclk = ref_khz(i915);
	int dkl = is_dkl(i915);
	const int use_ssc = 0;
	uint32_t dco_khz, m1div, m2div_int, m2div_rem, m2div_frac;
	uint32_t iref_ndiv, iref_trim, iref_pulse_w, prop_coeff, int_coeff;
	uint32_t tdc_targetcnt, feedfwgain;
	uint32_t ssc_stepsize = 0, ssc_steplen = 0, ssc_steplog = 4;

	if (tc_pll_divisors(clock_khz, is_dp, dkl, &dco_khz, s))
		return -EINVAL;
	m1div = 2;
	m2div_int = dco_khz / (refclk * m1div);
	if (m2div_int > 255) {
		if (!dkl) {
			m1div = 4;
			m2div_int = dco_khz / (refclk * m1div);
		}
		if (m2div_int > 255)
			return -EINVAL;
	}
	m2div_rem = dco_khz % (refclk * m1div);
	m2div_frac = (uint32_t)(((uint64_t)m2div_rem << 22) / (refclk * m1div));
	switch (refclk) {
	case 19200: iref_ndiv = 1; iref_trim = 28; iref_pulse_w = 1; break;
	case 24000: iref_ndiv = 1; iref_trim = 25; iref_pulse_w = 2; break;
	case 38400: iref_ndiv = 2; iref_trim = 28; iref_pulse_w = 1; break;
	default: return -EINVAL;
	}
	/* tdc_targetcnt = int(2 / (0.000003 * 8 * 50 * 1.1) / refclk_mhz + 0.5),
	 * rearranged so nothing is divided early */
	tdc_targetcnt = (2 * 1000 * 100000 * 10 / (132 * refclk) + 5) / 10;
	/* dco_khz over 10 keeps the dividend in 32 bits */
	feedfwgain = (use_ssc || m2div_rem > 0) ? m1div * 1000000 * 100 / (dco_khz * 3 / 10) : 0;
	if (dco_khz >= 9000000) {
		prop_coeff = 5;
		int_coeff = 10;
	} else {
		prop_coeff = 4;
		int_coeff = 8;
	}
	if (dkl) {
		s->div0 = DKL_PLL_DIV0_INTEG_COEFF(int_coeff) | DKL_PLL_DIV0_PROP_COEFF(prop_coeff) |
			  DKL_PLL_DIV0_FBPREDIV(m1div) | DKL_PLL_DIV0_FBDIV_INT(m2div_int);
		s->div1 = DKL_PLL_DIV1_IREF_TRIM(iref_trim) | DKL_PLL_DIV1_TDC_TARGET_CNT(tdc_targetcnt);
		s->ssc = DKL_PLL_SSC_IREF_NDIV_RATIO(iref_ndiv) | DKL_PLL_SSC_STEP_LEN(ssc_steplen) |
			 DKL_PLL_SSC_STEP_NUM(ssc_steplog) | (use_ssc ? DKL_PLL_SSC_EN : 0);
		s->bias = (m2div_frac ? DKL_PLL_BIAS_FRAC_EN_H : 0) | DKL_PLL_BIAS_FBDIV_FRAC(m2div_frac);
		s->tdc_coldst_bias = DKL_PLL_TDC_SSC_STEP_SIZE(ssc_stepsize) |
				     DKL_PLL_TDC_FEED_FWD_GAIN(feedfwgain);
		s->lf = s->frac_lock = 0;
		s->bias_mask = s->tdc_mask = 0xffffffffu;
		return 0;
	}
	s->div0 = (m2div_rem > 0 ? MG_PLL_DIV0_FRACNEN_H : 0) | MG_PLL_DIV0_FBDIV_FRAC(m2div_frac) |
		  MG_PLL_DIV0_FBDIV_INT(m2div_int);
	s->div1 = MG_PLL_DIV1_IREF_NDIVRATIO(iref_ndiv) | MG_PLL_DIV1_DITHER_DIV_2 |
		  MG_PLL_DIV1_NDIVRATIO(1) | MG_PLL_DIV1_FBPREDIV(m1div);
	s->lf = MG_PLL_LF_TDCTARGETCNT(tdc_targetcnt) | MG_PLL_LF_AFCCNTSEL_512 |
		MG_PLL_LF_GAINCTRL(1) | MG_PLL_LF_INT_COEFF(int_coeff) | MG_PLL_LF_PROP_COEFF(prop_coeff);
	s->frac_lock = MG_PLL_FRAC_LOCK_TRUELOCK_CRIT_32 | MG_PLL_FRAC_LOCK_EARLYLOCK_CRIT_32 |
		       MG_PLL_FRAC_LOCK_LOCKTHRESH(10) | MG_PLL_FRAC_LOCK_DCODITHEREN |
		       MG_PLL_FRAC_LOCK_FEEDFWRDGAIN(feedfwgain);
	if (use_ssc || m2div_rem > 0)
		s->frac_lock |= ICL_MG_PLL_FRAC_LOCK_FEEDFWRDCAL_EN;
	s->ssc = (use_ssc ? MG_PLL_SSC_EN : 0) | MG_PLL_SSC_TYPE(2) |
		 MG_PLL_SSC_STEPLENGTH(ssc_steplen) | MG_PLL_SSC_STEPNUM(ssc_steplog) |
		 MG_PLL_SSC_FLLEN | MG_PLL_SSC_STEPSIZE(ssc_stepsize);
	s->tdc_coldst_bias = MG_PLL_TDC_COLDST_COLDSTART | MG_PLL_TDC_COLDST_IREFINT_EN |
			     MG_PLL_TDC_COLDST_REFBIAS_START_PULSE_W(iref_pulse_w) |
			     MG_PLL_TDC_TDCOVCCORR_EN | MG_PLL_TDC_TDCSEL(3);
	s->bias = MG_PLL_BIAS_BIAS_GB_SEL(3) | MG_PLL_BIAS_INIT_DCOAMP(0x3F) |
		  MG_PLL_BIAS_BIAS_BONUS(10) | MG_PLL_BIAS_BIASCAL_EN | MG_PLL_BIAS_CTRIM(12) |
		  MG_PLL_BIAS_VREF_RDAC(4) | MG_PLL_BIAS_IREFTRIM(iref_trim);
	/* at 38.4 MHz only the cold start bit of the TDC word is ours and
	 * none of the bias word */
	if (refclk == 38400) {
		s->tdc_mask = MG_PLL_TDC_COLDST_COLDSTART;
		s->bias_mask = 0;
	} else {
		s->tdc_mask = s->bias_mask = 0xffffffffu;
	}
	s->tdc_coldst_bias &= s->tdc_mask;
	s->bias &= s->bias_mask;
	return 0;
}

int intel_tc_pll_clock_ok(struct i915_device *i915, uint32_t clock_khz, int is_dp)
{
	struct tc_pll s;
	return tc_pll_calc(i915, clock_khz, is_dp, &s) == 0;
}

static void rmw(struct i915_device *i915, uint32_t reg, uint32_t clear, uint32_t set)
{
	i915_write32(i915, reg, (i915_read32(i915, reg) & ~clear) | set);
}

/* Some of these words have reserved fields: read-modify-write under a
 * mask, the masks fixed or (bias, TDC) from the calculation. */
static void mg_pll_write(struct i915_device *i915, int tc, const struct tc_pll *s)
{
	rmw(i915, MG_REFCLKIN_CTL(tc), MGDKL_REFCLKIN_CTL_OD_2_MUX_MASK, s->refclkin_ctl);
	rmw(i915, MG_CLKTOP2_CORECLKCTL1(tc), MGDKL_CORECLKCTL1_A_DIVRATIO_MASK, s->coreclkctl1);
	rmw(i915, MG_CLKTOP2_HSCLKCTL(tc),
	    MGDKL_HSCLKCTL_TLINEDRV_CLKSEL_MASK | MGDKL_HSCLKCTL_CORE_INPUTSEL_MASK |
		    MGDKL_HSCLKCTL_HSDIV_RATIO_MASK | MGDKL_HSCLKCTL_DSDIV_RATIO_MASK,
	    s->hsclkctl);
	i915_write32(i915, MG_PLL_DIV0(tc), s->div0);
	i915_write32(i915, MG_PLL_DIV1(tc), s->div1);
	i915_write32(i915, MG_PLL_LF(tc), s->lf);
	i915_write32(i915, MG_PLL_FRAC_LOCK(tc), s->frac_lock);
	i915_write32(i915, MG_PLL_SSC(tc), s->ssc);
	rmw(i915, MG_PLL_BIAS(tc), s->bias_mask, s->bias);
	rmw(i915, MG_PLL_TDC_COLDST_BIAS(tc), s->tdc_mask, s->tdc_coldst_bias);
	(void)i915_read32(i915, MG_PLL_TDC_COLDST_BIAS(tc));
}

/* Every word read-modify-written; all of them sit behind the same bank
 * index, in the PLL's building block. */
static void dkl_pll_write(struct i915_device *i915, int tc, const struct tc_pll *s)
{
	intel_dkl_phy_rmw(i915, tc, DKL_REFCLKIN_CTL_OFF, MGDKL_REFCLKIN_CTL_OD_2_MUX_MASK,
			  s->refclkin_ctl);
	intel_dkl_phy_rmw(i915, tc, DKL_CLKTOP2_CORECLKCTL1_OFF, MGDKL_CORECLKCTL1_A_DIVRATIO_MASK,
			  s->coreclkctl1);
	intel_dkl_phy_rmw(i915, tc, DKL_CLKTOP2_HSCLKCTL_OFF,
			  MGDKL_HSCLKCTL_TLINEDRV_CLKSEL_MASK | MGDKL_HSCLKCTL_CORE_INPUTSEL_MASK |
				  MGDKL_HSCLKCTL_HSDIV_RATIO_MASK | MGDKL_HSCLKCTL_DSDIV_RATIO_MASK,
			  s->hsclkctl);
	intel_dkl_phy_rmw(i915, tc, DKL_PLL_DIV0_OFF, DKL_PLL_DIV0_FIELDS, s->div0);
	intel_dkl_phy_rmw(i915, tc, DKL_PLL_DIV1_OFF,
			  DKL_PLL_DIV1_IREF_TRIM_FIELD | DKL_PLL_DIV1_TDC_TARGET_CNT_FIELD, s->div1);
	intel_dkl_phy_rmw(i915, tc, DKL_PLL_SSC_OFF,
			  DKL_PLL_SSC_IREF_NDIV_RATIO_MASK | DKL_PLL_SSC_STEP_LEN_MASK |
				  DKL_PLL_SSC_STEP_NUM_MASK | DKL_PLL_SSC_EN,
			  s->ssc);
	intel_dkl_phy_rmw(i915, tc, DKL_PLL_BIAS_OFF,
			  DKL_PLL_BIAS_FRAC_EN_H | DKL_PLL_BIAS_FBDIV_FRAC_MASK, s->bias);
	intel_dkl_phy_rmw(i915, tc, DKL_PLL_TDC_COLDST_BIAS_OFF,
			  DKL_PLL_TDC_SSC_STEP_SIZE_MASK | DKL_PLL_TDC_FEED_FWD_GAIN_MASK,
			  s->tdc_coldst_bias);
	(void)intel_dkl_phy_read(i915, tc, DKL_PLL_TDC_COLDST_BIAS_OFF);
}

/* The enable of a Type-C port's PLL: Alder Lake-P moved them. */
static uint32_t tc_pll_enable_reg(struct i915_device *i915, int tc)
{
	if (i915->info->platform == I915_PLATFORM_ALDERLAKE_P)
		return ADLP_PORTTC_PLL_ENABLE(tc);
	return ICL_MG_PLL_ENABLE(tc);
}

static void tc_pll_disable(struct i915_device *i915, int tc)
{
	uint32_t en = tc_pll_enable_reg(i915, tc);

	if (i915_read32(i915, en) & PLL_ENABLE) {
		rmw(i915, en, PLL_ENABLE, 0);
		if (wait_bit(i915, en, PLL_LOCK, 0, 1))
			kprintf("[drm] i915: Type-C PLL %d still locked\n", tc + 1);
	}
	if (i915_read32(i915, en) & PLL_POWER_ENABLE) {
		rmw(i915, en, PLL_POWER_ENABLE, 0);
		if (wait_bit(i915, en, PLL_POWER_STATE, 0, 1))
			kprintf("[drm] i915: Type-C PLL %d power not disabled\n", tc + 1);
	}
}

/* Power, program, enable and lock the port's PLL. */
static int tc_pll_enable(struct i915_device *i915, struct intel_output *o, const struct tc_pll *s)
{
	int tc = o->tc_index;
	uint32_t en = tc_pll_enable_reg(i915, tc);

	tc_pll_disable(i915, tc); /* the firmware's, at other dividers */
	rmw(i915, en, 0, PLL_POWER_ENABLE);
	if (wait_bit(i915, en, PLL_POWER_STATE, 1, 1)) {
		kprintf("[drm] i915: port %s: Type-C PLL did not power up\n",
			intel_port_name(i915, o->port));
		return -EIO;
	}
	if (is_dkl(i915))
		dkl_pll_write(i915, tc, s);
	else
		mg_pll_write(i915, tc, s);
	rmw(i915, en, 0, PLL_ENABLE);
	if (wait_bit(i915, en, PLL_LOCK, 1, 1)) {
		kprintf("[drm] i915: port %s: Type-C PLL did not lock\n",
			intel_port_name(i915, o->port));
		return -EIO;
	}
	return 0;
}

/* The shared Thunderbolt PLL, refcounted over the ports in Thunderbolt
 * mode (intel_dpll_icl.c programs it). */
static int tbt_pll_get(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	if (d->tbt_pll_users++)
		return 0;
	if (icl_tbt_pll_enable(i915)) {
		d->tbt_pll_users--;
		kprintf("[drm] i915: the Thunderbolt PLL did not lock\n");
		return -EIO;
	}
	return 0;
}

static void tbt_pll_put(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	if (d->tbt_pll_users > 0)
		d->tbt_pll_users--;
	if (d->tbt_pll_users)
		return;
	icl_tbt_pll_disable(i915);
}

/* The port's own PLL for a port clock; the slot it is tracked in is the
 * port's. */
static int tc_pll_get(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz,
		      int is_dp)
{
	struct intel_display *d = &i915->display;
	int slot = INTEL_TC_PLL_BASE + o->tc_index;
	struct tc_pll s;

	if (slot >= INTEL_MAX_DPLLS)
		return -ENOSPC;
	if (tc_pll_calc(i915, clock_khz, is_dp, &s)) {
		kprintf("[drm] i915: port %s: no Type-C PLL dividers for %u kHz\n",
			intel_port_name(i915, o->port), clock_khz);
		return -EINVAL;
	}
	if (d->dpll[slot].in_use && d->dpll[slot].is_hdmi == !is_dp &&
	    d->dpll[slot].link_rate_khz == clock_khz) {
		d->dpll[slot].in_use++;
		return slot;
	}
	if (tc_pll_enable(i915, o, &s))
		return -EIO;
	d->dpll[slot].in_use = 1;
	d->dpll[slot].is_hdmi = !is_dp;
	d->dpll[slot].ssc = 0;
	d->dpll[slot].link_rate_khz = clock_khz;
	i915_dbg("[drm] i915: port %s: Type-C PLL at %u kHz\n", intel_port_name(i915, o->port),
		 clock_khz);
	return slot;
}

/* The PLL id handed back: the port's own slot, or the shared TBT one. */
int intel_tc_dpll_get_dp(struct i915_device *i915, struct intel_output *o,
			 uint32_t link_rate_khz, int ssc)
{
	(void)ssc; /* not spread */
	if (is_xelpdp(i915))
		return -ENODEV; /* the C20 PHY's PLL: intel_cx0_phy.c */
	if (!o->tc_owned && intel_tc_connect(i915, o))
		return -ENODEV;
	if (o->tc_mode == INTEL_TC_TBT_ALT) {
		if (link_rate_khz != 162000 && link_rate_khz != 270000 &&
		    link_rate_khz != 540000 && link_rate_khz != 810000)
			return -EINVAL;
		if (tbt_pll_get(i915))
			return -EIO;
		return INTEL_TBT_PLL_ID;
	}
	return tc_pll_get(i915, o, link_rate_khz, 1);
}

int intel_tc_dpll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz)
{
	if (is_xelpdp(i915))
		return -ENODEV; /* the C20 PHY's PLL: intel_cx0_phy.c */
	if (!o->tc_owned && intel_tc_connect(i915, o))
		return -ENODEV;
	if (o->tc_mode == INTEL_TC_TBT_ALT)
		return -EINVAL; /* no TMDS through Thunderbolt */
	return tc_pll_get(i915, o, clock_khz, 0);
}

void intel_tc_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (is_xelpdp(i915))
		return;
	if (pll == INTEL_TBT_PLL_ID) {
		tbt_pll_put(i915);
		return;
	}
	if (pll < INTEL_TC_PLL_BASE || pll >= INTEL_MAX_DPLLS)
		return;
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
	if (d->dpll[pll].in_use)
		return;
	tc_pll_disable(i915, pll - INTEL_TC_PLL_BASE);
}

/* ---- the port's clock select ---------------------------------------------------- */

/* The DDI clock select names the port's own PLL ("MG") or one of the
 * Thunderbolt PLL's four outputs; then the port's clock gate opens. */
void intel_tc_route_port(struct i915_device *i915, struct intel_output *o, int pll)
{
	uint32_t sel = ICL_DDI_CLK_SEL_MG;
	if (is_xelpdp(i915))
		return;
	if (pll == INTEL_TBT_PLL_ID) {
		switch (o->link_rate_khz) {
		case 162000: sel = ICL_DDI_CLK_SEL_TBT_162; break;
		case 270000: sel = ICL_DDI_CLK_SEL_TBT_270; break;
		case 540000: sel = ICL_DDI_CLK_SEL_TBT_540; break;
		case 810000: sel = ICL_DDI_CLK_SEL_TBT_810; break;
		default: sel = ICL_DDI_CLK_SEL_NONE; break;
		}
	}
	i915_write32(i915, GEN11_DDI_CLK_SEL(o->port), sel);
	(void)i915_read32(i915, GEN11_DDI_CLK_SEL(o->port));
	rmw(i915, GEN11_DPCLKA_CFGCR0, ICL_DPCLKA_TC_CLK_OFF(o->tc_index), 0);
	(void)i915_read32(i915, GEN11_DPCLKA_CFGCR0);
}

void intel_tc_unroute_port(struct i915_device *i915, struct intel_output *o)
{
	if (is_xelpdp(i915))
		return;
	rmw(i915, GEN11_DPCLKA_CFGCR0, 0, ICL_DPCLKA_TC_CLK_OFF(o->tc_index));
	(void)i915_read32(i915, GEN11_DPCLKA_CFGCR0);
	i915_write32(i915, GEN11_DDI_CLK_SEL(o->port), ICL_DDI_CLK_SEL_NONE);
	(void)i915_read32(i915, GEN11_DDI_CLK_SEL(o->port));
}

/* The port clock of the port's own PLL, from its dividers. */
static uint32_t tc_pll_read_khz(struct i915_device *i915, int tc)
{
	uint32_t refclk = ref_khz(i915);
	uint32_t m1, m2_int, m2_frac, div1, div2, hsclkctl;

	if (!(i915_read32(i915, tc_pll_enable_reg(i915, tc)) & PLL_ENABLE))
		return 0;
	if (is_dkl(i915)) {
		uint32_t div0 = intel_dkl_phy_read(i915, tc, DKL_PLL_DIV0_OFF);
		uint32_t bias = intel_dkl_phy_read(i915, tc, DKL_PLL_BIAS_OFF);
		m1 = (div0 & DKL_PLL_DIV0_FBPREDIV_MASK) >> DKL_PLL_DIV0_FBPREDIV_SHIFT;
		m2_int = div0 & DKL_PLL_DIV0_FBDIV_INT_MASK;
		m2_frac = (bias & DKL_PLL_BIAS_FRAC_EN_H) ?
				  (bias & DKL_PLL_BIAS_FBDIV_FRAC_MASK) >> DKL_PLL_BIAS_FBDIV_SHIFT : 0;
		hsclkctl = intel_dkl_phy_read(i915, tc, DKL_CLKTOP2_HSCLKCTL_OFF);
	} else {
		uint32_t div0 = i915_read32(i915, MG_PLL_DIV0(tc));
		m1 = i915_read32(i915, MG_PLL_DIV1(tc)) & ICL_MG_PLL_DIV1_FBPREDIV_MASK;
		m2_int = div0 & ICL_MG_PLL_DIV0_FBDIV_INT_MASK;
		m2_frac = (div0 & MG_PLL_DIV0_FRACNEN_H) ? (div0 & ICL_MG_PLL_DIV0_FBDIV_FRAC_MASK) >> 8 : 0;
		hsclkctl = i915_read32(i915, MG_CLKTOP2_HSCLKCTL(tc));
	}
	switch (hsclkctl & MGDKL_HSCLKCTL_HSDIV_RATIO_MASK) {
	case MGDKL_HSCLKCTL_HSDIV_RATIO_2: div1 = 2; break;
	case MGDKL_HSCLKCTL_HSDIV_RATIO_3: div1 = 3; break;
	case MGDKL_HSCLKCTL_HSDIV_RATIO_5: div1 = 5; break;
	default: div1 = 7; break;
	}
	div2 = (hsclkctl & MGDKL_HSCLKCTL_DSDIV_RATIO_MASK) >> MGDKL_HSCLKCTL_DSDIV_RATIO_SHIFT;
	if (div2 == 0)
		div2 = 1; /* 0 is the same as 1: no division */
	uint64_t tmp = (uint64_t)m1 * m2_int * refclk + (((uint64_t)m1 * m2_frac * refclk) >> 22);
	return (uint32_t)(tmp / (5 * div1 * div2));
}

/* What the firmware left the port running at: a Thunderbolt output by
 * its select, the port's own PLL from its dividers. */
uint32_t intel_tc_port_link_rate(struct i915_device *i915, struct intel_output *o)
{
	if (is_xelpdp(i915))
		return 0;
	if (i915_read32(i915, GEN11_DPCLKA_CFGCR0) & ICL_DPCLKA_TC_CLK_OFF(o->tc_index))
		return 0;
	switch (i915_read32(i915, GEN11_DDI_CLK_SEL(o->port)) & ICL_DDI_CLK_SEL_MASK) {
	case ICL_DDI_CLK_SEL_TBT_162: return 162000;
	case ICL_DDI_CLK_SEL_TBT_270: return 270000;
	case ICL_DDI_CLK_SEL_TBT_540: return 540000;
	case ICL_DDI_CLK_SEL_TBT_810: return 810000;
	case ICL_DDI_CLK_SEL_MG: return tc_pll_read_khz(i915, o->tc_index);
	default: return 0;
	}
}

/* ---- the PHY ------------------------------------------------------------------- */

/* Which lanes of the PHY carry the link, from the connector's pin
 * assignment and the lane count; also tells the FIA the count. */
void intel_tc_program_dp_mode(struct i915_device *i915, struct intel_output *o, int lanes)
{
	int tc = o->tc_index;
	uint32_t ln0, ln1;
	int pin;

	if (!o->is_tc || o->tc_mode == INTEL_TC_TBT_ALT || is_xelpdp(i915))
		return;
	set_fia_lanes(i915, o, lanes);
	if (is_dkl(i915)) {
		ln0 = intel_dkl_phy_read(i915, tc, DKL_DP_MODE_LN(0));
		ln1 = intel_dkl_phy_read(i915, tc, DKL_DP_MODE_LN(1));
	} else {
		ln0 = i915_read32(i915, MG_DP_MODE(tc, 0));
		ln1 = i915_read32(i915, MG_DP_MODE(tc, 1));
	}
	ln0 &= ~(MG_DP_MODE_CFG_DP_X1_MODE | MG_DP_MODE_CFG_DP_X2_MODE);
	ln1 &= ~(MG_DP_MODE_CFG_DP_X1_MODE | MG_DP_MODE_CFG_DP_X2_MODE);
	/* a legacy connector has no pin assignment */
	pin = o->tc_mode == INTEL_TC_LEGACY ? 0 : (int)pin_assignment(i915, o);
	switch (pin) {
	case 0x0: /* legacy: both lane pairs are the port's */
		if (lanes == 1) {
			ln1 |= MG_DP_MODE_CFG_DP_X1_MODE;
		} else {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x1: /* A */
		if (lanes == 4) {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x2: /* B */
		if (lanes == 2) {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x3: case 0x5: /* C, E */
	case 0x4: case 0x6: /* D, F */
		if (lanes == 1) {
			ln0 |= MG_DP_MODE_CFG_DP_X1_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X1_MODE;
		} else {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	default:
		i915_dbg("[drm] i915: port %s: unknown pin assignment %d\n",
			 intel_port_name(i915, o->port), pin);
		break;
	}
	if (is_dkl(i915)) {
		intel_dkl_phy_write(i915, tc, DKL_DP_MODE_LN(0), ln0);
		intel_dkl_phy_write(i915, tc, DKL_DP_MODE_LN(1), ln1);
		/* the writes do not always take: checked and repeated */
		if (intel_dkl_phy_read(i915, tc, DKL_DP_MODE_LN(0)) != ln0)
			intel_dkl_phy_write(i915, tc, DKL_DP_MODE_LN(0), ln0);
		if (intel_dkl_phy_read(i915, tc, DKL_DP_MODE_LN(1)) != ln1)
			intel_dkl_phy_write(i915, tc, DKL_DP_MODE_LN(1), ln1);
	} else {
		i915_write32(i915, MG_DP_MODE(tc, 0), ln0);
		i915_write32(i915, MG_DP_MODE(tc, 1), ln1);
	}
}

/* The PHYs' dynamic clock gating is left as the hardware has it. */
void intel_tc_clock_gating(struct i915_device *i915, struct intel_output *o, int enable)
{
	(void)i915;
	(void)o;
	(void)enable;
}

/* The port clock the levels go with: the link rate, or the TMDS clock
 * of the port's PLL. */
static uint32_t level_clock(struct i915_device *i915, const struct intel_output *o, int is_dp)
{
	if (is_dp)
		return o->link_rate_khz;
	if (o->pll >= 0 && o->pll < INTEL_MAX_DPLLS)
		return i915->display.dpll[o->pll].link_rate_khz;
	return 0;
}

/* The MG PHY's levels: the three de-emphasis override fields, in the
 * DisplayPort order (400 mV x4, 600 x3, 800 x2, 1200). */
struct mg_level {
	uint8_t ovr_11_6, ovr_5_0, ovr_17_12;
};
static const struct mg_level mg_rbr_hbr[] = {
	{ 0x18, 0x00, 0x00 }, { 0x1D, 0x00, 0x05 }, { 0x24, 0x00, 0x0C }, { 0x2B, 0x00, 0x14 },
	{ 0x21, 0x00, 0x00 }, { 0x2B, 0x00, 0x08 }, { 0x30, 0x00, 0x0F }, { 0x31, 0x00, 0x03 },
	{ 0x34, 0x00, 0x0B }, { 0x3F, 0x00, 0x00 },
};
static const struct mg_level mg_hbr2_hbr3[] = {
	{ 0x18, 0x00, 0x00 }, { 0x1D, 0x00, 0x05 }, { 0x24, 0x00, 0x0C }, { 0x2B, 0x00, 0x14 },
	{ 0x26, 0x00, 0x00 }, { 0x2C, 0x00, 0x07 }, { 0x33, 0x00, 0x0C }, { 0x2E, 0x00, 0x00 },
	{ 0x36, 0x00, 0x09 }, { 0x3F, 0x00, 0x00 },
};
/* HDMI presets 1-10: 400..1000 mV, then full swing with -1.5..-3 dB; the
 * last is the default */
static const struct mg_level mg_hdmi[] = {
	{ 0x1A, 0x0, 0x0 }, { 0x20, 0x0, 0x0 }, { 0x29, 0x0, 0x0 }, { 0x32, 0x0, 0x0 },
	{ 0x3F, 0x0, 0x0 }, { 0x3A, 0x0, 0x5 }, { 0x39, 0x0, 0x6 }, { 0x38, 0x0, 0x7 },
	{ 0x37, 0x0, 0x8 }, { 0x36, 0x0, 0x9 },
};
#define TC_N_LEVELS 10

static void mg_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	int tc = o->tc_index;
	int is_dp = (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
	uint32_t clock = level_clock(i915, o, is_dp);
	const struct mg_level *t;

	if (!is_dp)
		t = mg_hdmi;
	else
		t = clock > 270000 ? mg_hbr2_hbr3 : mg_rbr_hbr;
	if (level < 0 || level >= TC_N_LEVELS)
		level = is_dp ? 0 : TC_N_LEVELS - 1;
	const struct mg_level *l = &t[level];

	for (int ln = 0; ln < 2; ln++) {
		rmw(i915, MG_TX1_LINK_PARAMS(tc, ln), CRI_USE_FS32, 0);
		rmw(i915, MG_TX2_LINK_PARAMS(tc, ln), CRI_USE_FS32, 0);
	}
	for (int ln = 0; ln < 2; ln++) {
		rmw(i915, MG_TX1_SWINGCTRL(tc, ln), CRI_TXDEEMPH_OVERRIDE_17_12_MASK,
		    CRI_TXDEEMPH_OVERRIDE_17_12(l->ovr_17_12));
		rmw(i915, MG_TX2_SWINGCTRL(tc, ln), CRI_TXDEEMPH_OVERRIDE_17_12_MASK,
		    CRI_TXDEEMPH_OVERRIDE_17_12(l->ovr_17_12));
	}
	for (int ln = 0; ln < 2; ln++) {
		uint32_t val = CRI_TXDEEMPH_OVERRIDE_11_6(l->ovr_11_6) |
			       CRI_TXDEEMPH_OVERRIDE_5_0(l->ovr_5_0) | CRI_TXDEEMPH_OVERRIDE_EN;
		uint32_t mask = CRI_TXDEEMPH_OVERRIDE_11_6_MASK | CRI_TXDEEMPH_OVERRIDE_5_0_MASK;
		rmw(i915, MG_TX1_DRVCTRL(tc, ln), mask, val);
		rmw(i915, MG_TX2_DRVCTRL(tc, ln), mask, val);
	}
	/* the clock hub and the duty-cycle correction follow the port
	 * clock; a legacy connector has both TX1 and TX2 of each lane on */
	for (int ln = 0; ln < 2; ln++)
		rmw(i915, ICL_MG_CLKHUB(tc, ln), ICL_MG_CFG_LOW_RATE_LKREN_EN,
		    clock < 300000 ? ICL_MG_CFG_LOW_RATE_LKREN_EN : 0);
	for (int ln = 0; ln < 2; ln++) {
		uint32_t mask = CFG_AMI_CK_DIV_OVERRIDE_VAL_MASK | CFG_AMI_CK_DIV_OVERRIDE_EN;
		uint32_t val = clock > 500000 ?
				       CFG_AMI_CK_DIV_OVERRIDE_VAL(1) | CFG_AMI_CK_DIV_OVERRIDE_EN : 0;
		rmw(i915, MG_TX1_DCC(tc, ln), mask, val);
		rmw(i915, MG_TX2_DCC(tc, ln), mask, val);
	}
	for (int ln = 0; ln < 2; ln++) {
		rmw(i915, MG_TX1_PISO_READLOAD(tc, ln), 0, CRI_CALCINIT);
		rmw(i915, MG_TX2_PISO_READLOAD(tc, ln), 0, CRI_CALCINIT);
	}
}

/* The Dekel PHY's levels: swing, pre-shoot and de-emphasis per level. */
struct dkl_level {
	uint8_t vswing, preshoot, deemph;
};
static const struct dkl_level tgl_dkl_dp_hbr[] = {
	{ 0x7, 0x0, 0x00 }, { 0x5, 0x0, 0x05 }, { 0x2, 0x0, 0x0B }, { 0x0, 0x0, 0x18 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x08 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x0B }, { 0x0, 0x0, 0x00 },
};
static const struct dkl_level tgl_dkl_dp_hbr2[] = {
	{ 0x7, 0x0, 0x00 }, { 0x5, 0x0, 0x05 }, { 0x2, 0x0, 0x0B }, { 0x0, 0x0, 0x19 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x08 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x0B }, { 0x0, 0x0, 0x00 },
};
static const struct dkl_level adlp_dkl_dp_hbr[] = {
	{ 0x7, 0x0, 0x01 }, { 0x5, 0x0, 0x06 }, { 0x2, 0x0, 0x0B }, { 0x0, 0x0, 0x17 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x08 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x0B }, { 0x0, 0x0, 0x00 },
};
static const struct dkl_level adlp_dkl_dp_hbr2_hbr3[] = {
	{ 0x7, 0x0, 0x00 }, { 0x5, 0x0, 0x04 }, { 0x2, 0x0, 0x0A }, { 0x0, 0x0, 0x18 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x06 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x09 }, { 0x0, 0x0, 0x00 },
};
/* HDMI presets 1-10, the last the default */
static const struct dkl_level tgl_dkl_hdmi[] = {
	{ 0x7, 0x0, 0x0 }, { 0x6, 0x0, 0x0 }, { 0x4, 0x0, 0x0 }, { 0x2, 0x0, 0x0 },
	{ 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x5 }, { 0x0, 0x0, 0x6 }, { 0x0, 0x0, 0x7 },
	{ 0x0, 0x0, 0x8 }, { 0x0, 0x0, 0xA },
};

/* Early Alder Lake-P (display steppings before D0; not Alder Lake-N or
 * Raptor Lake) shares the PMD load generator in a way that fails at
 * 594 MHz TMDS and 1.62 GHz DisplayPort. */
static int adlp_loadgen_sharing_wa(struct i915_device *i915)
{
	uint16_t id = i915->devid;
	return i915->info->platform == I915_PLATFORM_ALDERLAKE_P && i915->revid < 0xC &&
	       (id & 0xff00) != 0xA700 && (id & 0xfff0) != 0x46D0;
}

static void dkl_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	int tc = o->tc_index;
	int is_dp = (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
	int adlp = i915->info->platform == I915_PLATFORM_ALDERLAKE_P;
	uint32_t clock = level_clock(i915, o, is_dp);
	const struct dkl_level *t;

	if (!is_dp)
		t = tgl_dkl_hdmi;
	else if (adlp)
		t = clock > 270000 ? adlp_dkl_dp_hbr2_hbr3 : adlp_dkl_dp_hbr;
	else
		t = clock > 270000 ? tgl_dkl_dp_hbr2 : tgl_dkl_dp_hbr;
	if (level < 0 || level >= TC_N_LEVELS)
		level = is_dp ? 0 : TC_N_LEVELS - 1;
	const struct dkl_level *l = &t[level];
	uint32_t val = DKL_TX_PRESHOOT_COEFF(l->preshoot) | DKL_TX_DEEMPH_COEFF(l->deemph) |
		       DKL_TX_VSWING_CONTROL(l->vswing);
	uint32_t mask = DKL_TX_PRESHOOT_FIELD_MASK | DKL_TX_DEEMPH_COEFF_MASK |
			DKL_TX_VSWING_FIELD_MASK;

	for (int ln = 0; ln < 2; ln++) {
		if (adlp_loadgen_sharing_wa(i915)) {
			int off = (!is_dp && clock == 594000) || (is_dp && clock == 162000);
			intel_dkl_phy_rmw(i915, tc, DKL_TX_DPCNTL2_LN(ln),
					  DKL_TX_LOADGEN_SHARING_PMD_DISABLE_BIT,
					  off ? DKL_TX_LOADGEN_SHARING_PMD_DISABLE_BIT : 0);
		}
		intel_dkl_phy_write(i915, tc, DKL_TX_PMD_LANE_SUS_LN(ln), 0);
		intel_dkl_phy_rmw(i915, tc, DKL_TX_DPCNTL0_LN(ln), mask, val);
		intel_dkl_phy_rmw(i915, tc, DKL_TX_DPCNTL1_LN(ln), mask, val);
		intel_dkl_phy_rmw(i915, tc, DKL_TX_DPCNTL2_LN(ln), DKL_TX_DP20BITMODE, 0);
		if (adlp) {
			uint32_t sel;
			if (!is_dp)
				sel = ln == 0 ? DKL_TX_DPCNTL2_LOADGENSELECT_TX1(0) |
							DKL_TX_DPCNTL2_LOADGENSELECT_TX2(2) :
						DKL_TX_DPCNTL2_LOADGENSELECT_TX1(3) |
							DKL_TX_DPCNTL2_LOADGENSELECT_TX2(3);
			else
				sel = DKL_TX_DPCNTL2_LOADGENSELECT_TX1(0) |
				      DKL_TX_DPCNTL2_LOADGENSELECT_TX2(0);
			intel_dkl_phy_rmw(i915, tc, DKL_TX_DPCNTL2_LN(ln),
					  DKL_TX_DPCNTL2_LOADGENSELECT_TX1_MASK |
						  DKL_TX_DPCNTL2_LOADGENSELECT_TX2_MASK,
					  sel);
		}
	}
}

void intel_tc_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	if (!o->is_tc || o->tc_mode == INTEL_TC_TBT_ALT || is_xelpdp(i915))
		return; /* Thunderbolt drives its own PHY; C20: intel_cx0_phy.c */
	if (is_dkl(i915))
		dkl_set_signal_level(i915, o, level);
	else
		mg_set_signal_level(i915, o, level);
}
