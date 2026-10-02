// LikeOS -- display power wells.
//
// The display engine is split into wells that can be powered down when
// unused.  Which wells exist and which register requests them differs
// by generation: Broadwell has one well for everything beyond pipe A and
// DDI A; Skylake power well 1 (pipe A, the eDP transcoder, AUX A), power
// well 2 (the other pipes, ports B-D) and a well per DDI; Broxton PW1/PW2
// and two DPIO PHYs; Ice Lake and Tiger Lake a chain of pipe wells and a
// well per DDI and per AUX channel in registers of their own (Rocket
// Lake without PW2, the Type-C ports' AUX in Thunderbolt mode through a
// well of its own).  Display version 13 (Alder Lake-P, DG2) turns the
// chain into a tree: PW1, then PW_A for pipe A and PW2 for everything
// else, with PW_B..PW_D per pipe behind PW2.  Meteor
// Lake keeps PW1 and PW2 and gives every pipe a well of its own (PW_A
// to PW_D, B to D behind PW2); its ports have no I/O wells any more (the
// PHY powers its lanes as part of the PLL sequence), and an AUX channel
// is powered by a request bit in its own control register.  Display
// version 20 (Lunar Lake) adds a well for PICA's Type-C half, which the
// Type-C ports' lanes and AUX channels sit behind; version 30 (Panther
// Lake) hangs PW_B off PW1 instead of PW2, so pipe B no longer needs
// PW2; Wildcat Lake has three pipes and two Type-C ports; Nova Lake's
// AUX channels report when their power is up.  Battlemage (14.01) is
// Meteor Lake's tree.  A domain
// names what a caller is about to touch; get/put keep the wells it
// needs up, refcounted.  A request goes through the driver's control
// register; the state bit answers when the well is up.
//
// The display core comes up in the order the hardware wants: deep power
// states off, the reset handshake with the south display armed, PW1, the
// clock (the firmware's, see intel_cdclk.c), the data buffer slices and
// the bus credits (intel_skl_wm.c's), then the arbiter defaults.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* A well: the register its request and state bits are in, and their
 * index there; or (Meteor Lake) an AUX channel, powered through the
 * request bit of its control register. */
enum well_kind {
	WELL_HSW = 0, /* request bit 2*idx+1, state bit 2*idx */
	WELL_XELPDP_AUX, /* reg = the channel's AUX_CH_CTL */
	WELL_ICL_COMBO_AUX, /* Ice Lake: an AUX well of a combo PHY */
	WELL_TC_AUX, /* Ice Lake to Alder Lake-P: a Type-C port's AUX well */
	WELL_TC_TBT_AUX, /* ... the same port in Thunderbolt mode */
	WELL_FIXED_AUX, /* DG2: an AUX well without a usable state bit */
	WELL_PICA, /* display 20+: PICA's Type-C half (XE2LPD_PICA_PW_CTL) */
};

struct well {
	uint32_t reg;
	int idx;
	int kind;
	int idx2; /* AUX wells: the channel; DDI wells: the port */
};

static int is_mtl(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_MTL;
}

/* The display IP release where the version alone does not tell the parts
 * apart: GMD_ID's, else the platform's. */
int intel_display_verx100(struct i915_device *i915)
{
	int ver = i915->info->display_ver;

	if (i915->display.display_verx100 && i915->display.display_verx100 / 100 == ver)
		return i915->display.display_verx100;
	if (i915->info->platform == I915_PLATFORM_BATTLEMAGE)
		return 1401;
	if (i915->info->platform == I915_PLATFORM_WILDCATLAKE)
		return 3002;
	return ver * 100;
}

/* The index of PW1 in the driver's control register. */
static int pw1_idx(struct i915_device *i915)
{
	return i915->info->display_ver >= 11 ? ICL_PW_CTL_IDX_PW_1 : SKL_PW_CTL_IDX_PW_1;
}

/* The power gates whose fuse distribution the hardware reports are the
 * pipe-chain wells from PW1 on (PW1..PW5, and the per-pipe wells of
 * display version 13+); PG0 is the hardware's own.  The fuses of a gate
 * are distributed once it powers up. */
static int well_pg(struct i915_device *i915, struct well w)
{
	if (w.kind != WELL_HSW || w.reg != HSW_PWR_WELL_CTL2 ||
	    i915->display.model == INTEL_DISPLAY_BDW)
		return -1;
	if (w.idx < pw1_idx(i915))
		return -1;
	return w.idx - pw1_idx(i915) + 1;
}

static void wait_fuses(struct i915_device *i915, int pg)
{
	for (int t = 0; t < 100; t++) {
		if (i915_read32(i915, SKL_FUSE_STATUS) & SKL_FUSE_PG_DIST_STATUS(pg))
			return;
		lapic_delay_us(10);
	}
	i915_dbg("[drm] i915: power gate %d: fuse distribution not reported done\n", pg);
}

static int well_wait(struct i915_device *i915, struct well w, int up, uint32_t ms)
{
	for (uint32_t t = 0; t < ms * 100; t++) {
		uint32_t v = i915_read32(i915, w.reg);
		if (!!(v & HSW_PWR_WELL_CTL_STATE(w.idx)) == up)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* Meteor Lake: a pipe's DSS clock gating is held off around its well
 * going down and released once the well is back. */
static void pipe_well_gating(struct i915_device *i915, struct well w, int disable)
{
	if (!is_mtl(i915) || w.kind != WELL_HSW || w.reg != HSW_PWR_WELL_CTL2 ||
	    w.idx < XELPD_PW_CTL_IDX_PW_A || w.idx > XELPD_PW_CTL_IDX_PW_D)
		return;
	uint32_t bit = DSS_PIPE_GATING_DISABLED(w.idx - XELPD_PW_CTL_IDX_PW_A);
	uint32_t v = i915_read32(i915, CLKGATE_DIS_DSSDSC);
	i915_write32(i915, CLKGATE_DIS_DSSDSC, disable ? (v | bit) : (v & ~bit));
}

/* The Type-C AUX channel a well belongs to: its index among the
 * Type-C ports (from the first Type-C AUX channel). */
static int tc_aux_index(struct i915_device *i915, int aux)
{
	return i915->info->display_ver == 11 ? aux - PORT_C : aux - PORT_TC1;
}

/* The output an AUX channel serves, if any. */
static struct intel_output *aux_output(struct i915_device *i915, int aux)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < d->nout; i++)
		if (d->outputs[i].aux.port == aux || d->outputs[i].port == aux)
			return &d->outputs[i];
	return NULL;
}

/* Before a Type-C port's AUX well is requested: the channel's
 * Thunderbolt I/O select set for the Thunderbolt well, cleared for the
 * PHY's own; and on Ice Lake a legacy port's PHY brought out of its
 * cold state first (the well is what keeps it out afterwards). */
static void tc_aux_prepare(struct i915_device *i915, struct well w)
{
	uint32_t aux = DP_AUX_CH_CTL(w.idx2);
	uint32_t v = i915_read32(i915, aux);

	if (w.kind == WELL_TC_TBT_AUX)
		v |= DP_AUX_CH_CTL_TBT_IO;
	else
		v &= ~DP_AUX_CH_CTL_TBT_IO;
	i915_write32(i915, aux, v);
	if (w.kind == WELL_TC_AUX && i915->info->display_ver == 11) {
		struct intel_output *o = aux_output(i915, w.idx2);
		if (o && o->tc_mode == INTEL_TC_LEGACY) {
			int rc = -1;
			for (int tries = 0; tries < 3 && rc; tries++) {
				rc = intel_pcode_write(i915, ICL_PCODE_EXIT_TCCOLD, 0);
				if (rc)
					lapic_delay_ms(1);
			}
			/* the exit takes up to 1 ms */
			if (!rc)
				lapic_delay_ms(1);
		}
	}
}

/* After a Type-C port's own AUX well is up (Tiger Lake on): the Dekel
 * PHY's microcontroller reports it healthy. */
static void tc_aux_wait_uc(struct i915_device *i915, struct well w)
{
	int tc = tc_aux_index(i915, w.idx2);

	if (w.kind != WELL_TC_AUX || i915->info->display_ver < 12 || tc < 0)
		return;
	for (int t = 0; t < 10; t++) {
		if (intel_dkl_phy_read(i915, tc, DKL_CMN_UC_DW27_OFF) & DKL_CMN_UC_DW27_UC_HEALTH)
			return;
		lapic_delay_us(100);
	}
	kprintf("[drm] i915: Type-C port %d: the PHY's microcontroller did not report healthy\n",
		tc + 1);
}

/* Ice Lake: an AUX well of a combo PHY also needs the PHY's AUX lane,
 * and a channel that is not an eDP panel's the LDO bypass override. */
static void icl_combo_aux(struct i915_device *i915, struct well w, int on)
{
	int phy = w.idx2;
	uint32_t v = i915_read32(i915, ICL_PORT_CL_DW(12, phy));
	i915_write32(i915, ICL_PORT_CL_DW(12, phy),
		     on ? (v | ICL_LANE_ENABLE_AUX) : (v & ~ICL_LANE_ENABLE_AUX));
	if (!on)
		return;
	struct intel_output *o = aux_output(i915, w.idx2);
	if (o && o->is_edp)
		return;
	v = i915_read32(i915, ICL_PORT_TX_DW_AUX(6, phy));
	i915_write32(i915, ICL_PORT_TX_DW_AUX(6, phy), v | O_FUNC_OVRD_EN | O_LDO_BYPASS_CRI);
}

static int well_enable(struct i915_device *i915, struct well w)
{
	uint32_t v = i915_read32(i915, w.reg);

	if (w.kind == WELL_XELPDP_AUX) {
		if (v & XELPDP_DP_AUX_CH_CTL_POWER_REQUEST)
			return 0;
		i915_write32(i915, w.reg, v | XELPDP_DP_AUX_CH_CTL_POWER_REQUEST);
		/* Nova Lake's LT PHYs report the channel up within 2 ms. */
		if (intel_has_lt_phy(i915)) {
			for (int t = 0; t < 200; t++) {
				if (i915_read32(i915, w.reg) & XELPDP_DP_AUX_CH_CTL_POWER_STATUS)
					return 0;
				lapic_delay_us(10);
			}
			kprintf("[drm] i915: AUX channel %d power did not come up\n", w.idx2);
			return -ETIMEDOUT;
		}
		/* Before them the status bit does not say when the channel is
		 * up: the hardware wants a fixed 600 us after the request. */
		lapic_delay_us(600);
		return 0;
	}
	if (w.kind == WELL_PICA) {
		if ((v & XE2LPD_PICA_CTL_POWER_REQUEST) && (v & XE2LPD_PICA_CTL_POWER_STATUS))
			return 0;
		i915_write32(i915, w.reg, XE2LPD_PICA_CTL_POWER_REQUEST);
		for (int t = 0; t < 100; t++) {
			if (i915_read32(i915, w.reg) & XE2LPD_PICA_CTL_POWER_STATUS)
				return 0;
			lapic_delay_us(10);
		}
		kprintf("[drm] i915: PICA's Type-C power well did not come up\n");
		return -ETIMEDOUT;
	}
	if ((v & HSW_PWR_WELL_CTL_STATE(w.idx)) && (v & HSW_PWR_WELL_CTL_REQ(w.idx)))
		return 0;
	int pg = well_pg(i915, w);
	if (pg == 1) {
		wait_fuses(i915, 0); /* PG0 before PW1 */
		/* Alder Lake-P: no FLR source while PG1 comes up */
		if (i915->info->platform == I915_PLATFORM_ALDERLAKE_P)
			i915_write32(i915, GEN8_CHICKEN_DCPR_1,
				     i915_read32(i915, GEN8_CHICKEN_DCPR_1) | DISABLE_FLR_SRC);
	}
	if (w.kind == WELL_TC_AUX || w.kind == WELL_TC_TBT_AUX)
		tc_aux_prepare(i915, w);
	v = i915_read32(i915, w.reg);
	i915_write32(i915, w.reg, v | HSW_PWR_WELL_CTL_REQ(w.idx));
	if (w.kind == WELL_ICL_COMBO_AUX)
		icl_combo_aux(i915, w, 1);
	if (w.kind == WELL_FIXED_AUX) {
		/* DG2: the AUX wells' state is not to be watched; they are
		 * up 600 us after the request */
		lapic_delay_us(600);
		return 0;
	}
	/* Alder Lake-P's Type-C AUX wells may take up to 500 ms; a
	 * Thunderbolt one times out when no tunnel is up, which is fine */
	uint32_t ms = 10;
	if (w.kind == WELL_TC_AUX && i915->info->display_ver == 13)
		ms = 500;
	if (well_wait(i915, w, 1, ms) != 0 && w.kind != WELL_TC_TBT_AUX) {
		kprintf("[drm] i915: power well %d (register %05x) did not come up\n", w.idx,
			w.reg);
		return -ETIMEDOUT;
	}
	if (pg > 0)
		wait_fuses(i915, pg);
	tc_aux_wait_uc(i915, w);
	pipe_well_gating(i915, w, 0);
	return 0;
}

static void well_disable(struct i915_device *i915, struct well w)
{
	uint32_t v = i915_read32(i915, w.reg);

	if (w.kind == WELL_XELPDP_AUX) {
		if (!(v & XELPDP_DP_AUX_CH_CTL_POWER_REQUEST))
			return;
		i915_write32(i915, w.reg, v & ~XELPDP_DP_AUX_CH_CTL_POWER_REQUEST);
		if (intel_has_lt_phy(i915)) {
			for (int t = 0; t < 100; t++) {
				if (!(i915_read32(i915, w.reg) & XELPDP_DP_AUX_CH_CTL_POWER_STATUS))
					return;
				lapic_delay_us(10);
			}
			i915_dbg("[drm] i915: AUX channel %d did not power down\n", w.idx2);
			return;
		}
		lapic_delay_us(10);
		return;
	}
	if (w.kind == WELL_PICA) {
		if (!(v & XE2LPD_PICA_CTL_POWER_REQUEST))
			return;
		i915_write32(i915, w.reg, 0);
		for (int t = 0; t < 100; t++) {
			if (!(i915_read32(i915, w.reg) & XE2LPD_PICA_CTL_POWER_STATUS))
				return;
			lapic_delay_us(10);
		}
		i915_dbg("[drm] i915: PICA's Type-C power well did not go down\n");
		return;
	}
	if (!(v & HSW_PWR_WELL_CTL_REQ(w.idx)))
		return;
	pipe_well_gating(i915, w, 1);
	if (w.kind == WELL_ICL_COMBO_AUX)
		icl_combo_aux(i915, w, 0);
	i915_write32(i915, w.reg, v & ~HSW_PWR_WELL_CTL_REQ(w.idx));
}

static struct well W(uint32_t reg, int idx)
{
	struct well w = { reg, idx, WELL_HSW, 0 };
	return w;
}

static struct well WAUX(struct i915_device *i915, int aux)
{
	struct well w = { XELPDP_DP_AUX_CH_CTL_VER(i915->info->display_ver, aux), aux,
			  WELL_XELPDP_AUX, aux };
	return w;
}

static struct well WPICA(void)
{
	struct well w = { XE2LPD_PICA_PW_CTL, 0, WELL_PICA, 0 };
	return w;
}

/* ---- the domain-to-well maps of Ice Lake to Alder Lake-P and DG2 --------------- */

static int plat_is(struct i915_device *i915, int p)
{
	return i915->info->platform == p;
}

/* Does the part drive its Type-C ports through Type-C PHYs (as opposed
 * to Rocket Lake, Alder Lake-S, DG1 and DG2, whose "TC" ports are combo
 * or SNPS PHYs)? */
static int has_tc_phys(struct i915_device *i915)
{
	return (i915->info->flags & I915_INFO_HAS_TC_PHY) && !(i915->info->flags & I915_INFO_IS_DGFX);
}

/* The AUX well of channel `aux': its own, or for a Type-C port in
 * Thunderbolt mode the Thunderbolt one. */
static struct well aux_well(struct i915_device *i915, int aux)
{
	struct well w = { ICL_PWR_WELL_CTL_AUX2, ICL_PW_CTL_IDX_AUX(aux), WELL_HSW, aux };
	int ver = i915->info->display_ver;
	int is_tc = 0;

	if (plat_is(i915, I915_PLATFORM_DG2)) {
		w.kind = WELL_FIXED_AUX;
		return w;
	}
	if (has_tc_phys(i915)) {
		if (ver == 11)
			is_tc = aux >= PORT_C && aux <= PORT_F;
		else if (ver == 12)
			is_tc = aux >= PORT_TC1 && aux <= PORT_TC1 + 5;
		else
			is_tc = aux >= PORT_TC1 && aux <= PORT_TC1 + 3;
	}
	if (is_tc) {
		struct intel_output *o = aux_output(i915, aux);
		int tc = tc_aux_index(i915, aux);
		w.kind = WELL_TC_AUX;
		if (o && o->is_tc && o->tc_mode == INTEL_TC_TBT_ALT) {
			w.kind = WELL_TC_TBT_AUX;
			w.idx = ver == 11 ? ICL_PW_CTL_IDX_AUX_TBT(tc) : TGL_PW_CTL_IDX_AUX_TBT(tc);
		}
		return w;
	}
	if (plat_is(i915, I915_PLATFORM_ICELAKE) && (aux == PORT_A || aux == PORT_B))
		w.kind = WELL_ICL_COMBO_AUX;
	return w;
}

static struct well ddi_well(int port)
{
	struct well w = { ICL_PWR_WELL_CTL_DDI2, ICL_PW_CTL_IDX_DDI(port), WELL_HSW, port };
	return w;
}

/* Which ports' lanes and AUX channels sit in PW1 (the rest are behind
 * the pipe-well chain): the combo ports A-C of Tiger Lake and its
 * relatives, A and B elsewhere; only A on Ice Lake. */
static int port_in_pw1(struct i915_device *i915, int port)
{
	int ver = i915->info->display_ver;

	if (ver == 11)
		return port == PORT_A;
	if (ver == 12 && plat_is(i915, I915_PLATFORM_TIGERLAKE))
		return port <= PORT_C;
	if (ver == 12 && plat_is(i915, I915_PLATFORM_ALDERLAKE_S))
		return port == PORT_A;
	return port == PORT_A || port == PORT_B;
}

static int icl_family_wells(struct i915_device *i915, enum intel_power_domain d, int port,
			    int aux, int pipe, struct well *out)
{
	int ver = i915->info->display_ver;
	int rkl = plat_is(i915, I915_PLATFORM_ROCKETLAKE);
	int n = 0;

	out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_1);
	if (d == INTEL_PW_DISPLAY_CORE || d == INTEL_PW_GMBUS || d == INTEL_PW_TRANSCODER_EDP)
		return n;
	if (ver >= 13) {
		/* PW1 -> PW_A; PW1 -> PW2 -> PW_B/C/D */
		if (pipe == 0) {
			out[n++] = W(HSW_PWR_WELL_CTL2, XELPD_PW_CTL_IDX_PW_A);
			return n;
		}
		if (pipe > 0) {
			out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_2);
			out[n++] = W(HSW_PWR_WELL_CTL2, XELPD_PW_CTL_IDX_PW_A + pipe);
			return n;
		}
	} else {
		if (pipe == 0)
			return n;
		if (pipe > 0) {
			/* PW2 (not on Rocket Lake) -> PW3 -> PW4 -> PW5 */
			if (!rkl)
				out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_2);
			out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_3);
			if (pipe >= 2)
				out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_4);
			if (pipe >= 3 && ver == 12)
				out[n++] = W(HSW_PWR_WELL_CTL2, TGL_PW_CTL_IDX_PW_5);
			return n;
		}
	}
	int p = port >= 0 ? port : aux;
	if (p < 0)
		return n;
	if (!port_in_pw1(i915, p)) {
		if (ver >= 13 || !rkl)
			out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_2);
		if (ver < 13)
			out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_3);
	}
	if (port >= 0)
		out[n++] = ddi_well(port);
	else
		out[n++] = aux_well(i915, aux);
	return n;
}

/* The wells a domain needs, in the order they come up (each on the one
 * before it); returns the count (0: always-on). */
static int domain_wells(struct i915_device *i915, enum intel_power_domain d,
			struct well *out, int cap)
{
	enum intel_display_model model = i915->display.model;
	int n = 0;
	int port = -1;
	int aux = -1;
	int pipe = -1;

	if (d >= INTEL_PW_DDI_A && d <= INTEL_PW_DDI_I)
		port = d - INTEL_PW_DDI_A;
	else if (d >= INTEL_PW_AUX_A && d <= INTEL_PW_AUX_I)
		aux = d - INTEL_PW_AUX_A;
	else if (d >= INTEL_PW_PIPE_A && d <= INTEL_PW_PIPE_D)
		pipe = d - INTEL_PW_PIPE_A;
	(void)cap;

	/* DG2 is driven by whichever model the display code picks for it;
	 * its wells are Alder Lake-P's. */
	if (plat_is(i915, I915_PLATFORM_DG2) && model != INTEL_DISPLAY_MTL)
		return icl_family_wells(i915, d, port, aux, pipe, out);

	switch (model) {
	case INTEL_DISPLAY_BDW:
		/* pipe A, its transcoder, DDI A and AUX A are always on */
		if (pipe == 0 || d == INTEL_PW_TRANSCODER_EDP || port == 0 || aux == 0 ||
		    d == INTEL_PW_DISPLAY_CORE)
			return 0;
		out[n++] = W(HSW_PWR_WELL_CTL2, HSW_PW_CTL_IDX_GLOBAL);
		return n;
	case INTEL_DISPLAY_SKL:
		if (d == INTEL_PW_DISPLAY_CORE) {
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
			return n;
		}
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
		if (pipe == 0 || d == INTEL_PW_TRANSCODER_EDP || aux == 0 || port == 0 ||
		    port == 4) {
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_A_E);
			return n;
		}
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_2);
		if (port == 1)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_B);
		else if (port == 2)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_C);
		else if (port == 3)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_D);
		return n;
	case INTEL_DISPLAY_BXT:
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
		if (d == INTEL_PW_DISPLAY_CORE || pipe == 0 || d == INTEL_PW_TRANSCODER_EDP ||
		    aux == 0 || port == 0)
			return n;
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_2);
		return n;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
	case INTEL_DISPLAY_DG2:
		return icl_family_wells(i915, d, port, aux, pipe, out);
	case INTEL_DISPLAY_MTL: {
		int ver = i915->info->display_ver;
		/* the Type-C ports there are: two on Wildcat Lake, four
		 * elsewhere */
		int ntc = intel_display_verx100(i915) == 3002 ? 2 : 4;

		/* PW1 carries the core, transcoder A, DDI A and B, the DBUF
		 * and the shared functions; from version 20 PICA's Type-C
		 * half is held with it (the display core's domain keeps it). */
		out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_1);
		if (d == INTEL_PW_DISPLAY_CORE && ver >= 20) {
			out[n++] = WPICA();
			return n;
		}
		if (d == INTEL_PW_DISPLAY_CORE || d == INTEL_PW_GMBUS ||
		    d == INTEL_PW_TRANSCODER_EDP)
			return n;
		if (aux >= 0) {
			/* A, B and USBC1..; on version 14 PICA itself is
			 * always on, so the channel's request is all; from
			 * 20 a Type-C channel is behind PICA's Type-C well. */
			if (aux == 2 || aux > PORT_TC1 - 1 + ntc)
				return n;
			if (ver >= 20 && aux >= PORT_TC1)
				out[n++] = WPICA();
			out[n++] = WAUX(i915, aux);
			return n;
		}
		if (pipe == 0) {
			out[n++] = W(HSW_PWR_WELL_CTL2, XELPD_PW_CTL_IDX_PW_A);
			return n;
		}
		/* version 30: PW_B hangs off PW1 */
		if (pipe == 1 && ver >= 30) {
			out[n++] = W(HSW_PWR_WELL_CTL2, XELPD_PW_CTL_IDX_PW_B);
			return n;
		}
		if (port >= 0 && port < PORT_TC1)
			return n; /* DDI A/B: the PHY's lanes are the PLL's business */
		/* pipes B (C from version 30) to D and the Type-C ports'
		 * lanes are behind PW2 (and from version 20 the lanes behind
		 * PICA's Type-C well too) */
		out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_2);
		if (pipe > 0)
			out[n++] = W(HSW_PWR_WELL_CTL2, XELPD_PW_CTL_IDX_PW_A + pipe);
		else if (port >= PORT_TC1 && ver >= 20)
			out[n++] = WPICA();
		return n;
	}
	}
	return 0;
}

static int well_needed(struct i915_device *i915, struct well w)
{
	struct well ws[8];
	for (int d = 0; d < INTEL_PW_COUNT; d++) {
		if (!i915->display.pw_count[d])
			continue;
		int n = domain_wells(i915, (enum intel_power_domain)d, ws, 8);
		for (int i = 0; i < n; i++)
			if (ws[i].reg == w.reg && ws[i].idx == w.idx)
				return 1;
	}
	return 0;
}

void intel_power_get(struct i915_device *i915, enum intel_power_domain d)
{
	struct well ws[8];
	int n = domain_wells(i915, d, ws, 8);

	i915->display.pw_count[d]++;
	for (int i = 0; i < n; i++)
		well_enable(i915, ws[i]);
}

void intel_power_put(struct i915_device *i915, enum intel_power_domain d)
{
	struct well ws[8];
	int n = domain_wells(i915, d, ws, 8);

	if (i915->display.pw_count[d] > 0)
		i915->display.pw_count[d]--;
	/* PW1 (and Broadwell's one well) stay up while the driver runs. */
	for (int i = n - 1; i >= 1; i--)
		if (!well_needed(i915, ws[i]))
			well_disable(i915, ws[i]);
	/* A Type-C AUX domain is mapped by the port's current mode; if the
	 * mode changed while it was held, the other well of the pair may
	 * be the one that is up. */
	if (n >= 1 && (ws[n - 1].kind == WELL_TC_AUX || ws[n - 1].kind == WELL_TC_TBT_AUX)) {
		struct well o = ws[n - 1];
		int aux = o.idx2, tc = tc_aux_index(i915, aux);
		if (o.kind == WELL_TC_AUX) {
			o.kind = WELL_TC_TBT_AUX;
			o.idx = i915->info->display_ver == 11 ? ICL_PW_CTL_IDX_AUX_TBT(tc) :
								TGL_PW_CTL_IDX_AUX_TBT(tc);
		} else {
			o.kind = WELL_TC_AUX;
			o.idx = ICL_PW_CTL_IDX_AUX(aux);
		}
		if (!well_needed(i915, o))
			well_disable(i915, o);
	}
}

/* ---- DC states ------------------------------------------------------------------ */

/* The DC_STATE_EN bits that select a deep state on this display. */
static uint32_t dc_mask(struct i915_device *i915)
{
	enum intel_display_model m = i915->display.model;
	uint32_t mask = DC_STATE_EN_UPTO_DC5;

	if (i915->info->display_ver >= 12)
		mask |= DC_STATE_EN_UPTO_DC3CO | DC_STATE_EN_UPTO_DC6 | DC_STATE_EN_DC9;
	else if (i915->info->display_ver == 11)
		mask |= DC_STATE_EN_UPTO_DC6 | DC_STATE_EN_DC9;
	else if (m == INTEL_DISPLAY_BXT)
		mask |= DC_STATE_EN_DC9;
	else
		mask |= DC_STATE_EN_UPTO_DC6;
	return mask;
}

/* All deep states off.  The DMC is known to hold on to DC6 for a while
 * after it was turned off, so the write is repeated until the register
 * reads back as written several times over (the read-only bits of
 * display version 13+ aside). */
static void dc_states_disable(struct i915_device *i915)
{
	uint32_t ro = 0;

	if (i915->info->display_ver >= 20)
		ro = DC_STATE_EN_CSR_MASK_CMTG_1 | DC_STATE_EN_CSR_MASK_CMTG_0;
	else if (i915->info->display_ver >= 13 && i915->info->platform != I915_PLATFORM_DG2)
		ro = DC_STATE_EN_CSR_MASK_CMTG_0;
	uint32_t v = i915_read32(i915, DC_STATE_EN);
	uint32_t want = v & ~dc_mask(i915);
	int rewrites = 0, rereads = 0;

	if (v == want)
		return;
	i915_write32(i915, DC_STATE_EN, want);
	do {
		v = i915_read32(i915, DC_STATE_EN);
		if ((v & ~ro) != (want & ~ro)) {
			i915_write32(i915, DC_STATE_EN, want);
			rewrites++;
			rereads = 0;
		} else if (rereads++ > 5) {
			break;
		}
	} while (rewrites < 100);
	if ((v & ~ro) != (want & ~ro))
		kprintf("[drm] i915: DC states would not turn off (DC_STATE_EN %08x)\n", v);
}

/* ---- the arbiter ------------------------------------------------------------------ */

/* Meteor Lake on: the arbiter's buddy page mask, by the kind of memory
 * and its channel count, as the memory subsystem reports them.  Memory
 * the table does not know gets the buddy logic switched off (before
 * version 20; from 20 it is left alone).  The discrete parts after DG1
 * do not use the buddy registers. */
static void mtl_bw_buddy_init(struct i915_device *i915)
{
	/* MTL_MEM_SS_INFO_GLOBAL's DDR type codes */
	enum { DDR4 = 0, DDR5 = 1, LPDDR5 = 2, LPDDR4 = 3 };
	static const struct {
		uint8_t channels, type;
		uint8_t page_mask;
	} table[] = {
		{ 1, DDR4, 0xF }, { 1, DDR5, 0xF }, { 2, LPDDR4, 0x1C }, { 2, LPDDR5, 0x1C },
		{ 2, DDR4, 0x1F }, { 2, DDR5, 0x1E }, { 4, LPDDR4, 0x38 }, { 4, LPDDR5, 0x38 },
	};
	uint32_t info, type, channels;
	int found = -1;

	if (i915->info->flags & I915_INFO_IS_DGFX)
		return;
	info = i915_read32(i915, MTL_MEM_SS_INFO_GLOBAL);
	type = MTL_DDR_TYPE(info);
	channels = MTL_N_OF_POPULATED_CH(info);

	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (table[i].channels == channels && table[i].type == type) {
			found = (int)i;
			break;
		}
	for (int abox = 0; abox < 2; abox++) {
		if (found < 0 && i915->info->display_ver < 20)
			i915_write32(i915, BW_BUDDY_CTL(abox), BW_BUDDY_DISABLE);
		else if (found >= 0)
			i915_write32(i915, BW_BUDDY_PAGE_MASK(abox), table[found].page_mask);
	}
	i915_dbg("[drm] i915: memory type %u, %u channels: %s\n", type, channels,
		 found < 0 ? "buddy logic off" : "buddy page mask set");
}

/* ---- display core bring-up -------------------------------------------------------- */

/* The handshake that keeps the north display from being reset while the
 * south display (and Meteor Lake's PICA) still depends on it.  Parts
 * without a PCH turn it off. */
static void pch_reset_handshake(struct i915_device *i915)
{
	int enable = (i915->info->flags & I915_INFO_HAS_PCH) && i915->pch != I915_PCH_NONE;
	uint32_t bits = RESET_PCH_HANDSHAKE_ENABLE;

	/* Version 35 has no handshake to arm.  From 14 the south display is
	 * on the SoC (or the card): the platform's PCH kind is what says
	 * there is one, not the descriptor's PCH flag. */
	if (i915->info->display_ver >= 35)
		return;
	if (i915->info->display_ver >= 14)
		enable = i915->pch != I915_PCH_NONE && i915->pch != I915_PCH_UNKNOWN;

	if (i915->info->display_ver >= 14)
		bits |= MTL_RESET_PICA_HANDSHAKE_EN;
	uint32_t v = i915_read32(i915, HSW_NDE_RSTWRN_OPT);
	uint32_t nv = enable ? (v | bits) : (v & ~bits);
	if (nv != v)
		i915_write32(i915, HSW_NDE_RSTWRN_OPT, nv);
}

/* The display IP stepping and release: version 14 on reports them in
 * GMD_ID (four substeps to a letter); earlier parts are left unknown
 * here.  An IP version that disagrees with the descriptor's is not
 * believed. */
static void read_display_step(struct i915_device *i915)
{
	i915->display.display_step = 0xff;
	i915->display.display_verx100 = 0;
	if (i915->info->display_ver < 14)
		return;
	uint32_t v = i915_read32(i915, GMD_ID_DISPLAY);
	if (!v || v == 0xffffffffu)
		return;
	uint32_t arch = (v & GMD_ID_DISPLAY_ARCH_MASK) >> GMD_ID_DISPLAY_ARCH_SHIFT;
	uint32_t rel = (v & GMD_ID_DISPLAY_RELEASE_MASK) >> GMD_ID_DISPLAY_RELEASE_SHIFT;
	i915->display.display_step = (uint8_t)(v & GMD_ID_DISPLAY_STEP_MASK);
	if (arch == i915->info->display_ver && rel < 100)
		i915->display.display_verx100 = (uint16_t)(arch * 100 + rel);
	else
		kprintf("[drm] i915: GMD_ID reports display %u.%02u, the device table %u\n", arch,
			rel, i915->info->display_ver);
}

/* Display core bring-up, in the order the hardware wants: deep states
 * off, the reset handshake, PW1 (and the misc I/O well on Skylake), the
 * CDCLK (already running from the firmware; see intel_cdclk.c), the data
 * buffer, then the bus defaults.  PW1 is never released. */
int intel_power_init(struct i915_device *i915)
{
	enum intel_display_model model = i915->display.model;
	struct intel_display *d = &i915->display;

	for (int dom = 0; dom < INTEL_PW_COUNT; dom++)
		d->pw_count[dom] = 0;
	if (model == INTEL_DISPLAY_BDW) {
		/* The one well is left as the firmware had it: PW1-style
		 * bring-up does not exist here. */
		d->pw_count[INTEL_PW_DISPLAY_CORE] = 1;
		return 0;
	}
	read_display_step(i915);
	/* DC states off while the driver runs (until DMC is loaded and
	 * wanted): a display that goes to DC5/DC6 under a driver that did
	 * not ask for it loses register state. */
	dc_states_disable(i915);
	/* Tiger and Alder Point: the DPMG unit's clock gating off */
	if (i915->pch == I915_PCH_TGP || i915->pch == I915_PCH_ADP)
		i915_write32(i915, SOUTH_DSPCLK_GATE_D,
			     i915_read32(i915, SOUTH_DSPCLK_GATE_D) |
				     PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	pch_reset_handshake(i915);

	if (well_enable(i915, W(HSW_PWR_WELL_CTL2, pw1_idx(i915))) != 0)
		return -EIO;
	if (model == INTEL_DISPLAY_SKL)
		well_enable(i915, W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_MISC_IO));
	/* from version 20 PICA's Type-C half stays up with PW1 */
	if (is_mtl(i915) && i915->info->display_ver >= 20)
		well_enable(i915, WPICA());
	if (i915->info->display_ver == 14) {
		/* the PHYs' PG1 latches, held by the firmware across reset */
		uint32_t v = i915_read32(i915, DC_STATE_EN);
		if (v & (HOLD_PHY_PG1_LATCH | HOLD_PHY_CLKREQ_PG1_LATCH))
			i915_write32(i915, DC_STATE_EN,
				     v & ~(HOLD_PHY_PG1_LATCH | HOLD_PHY_CLKREQ_PG1_LATCH));
	}
	if (is_mtl(i915))
		intel_pmdemand_init(i915);

	/* the data buffer's slices (with the PM demand request for them
	 * first on version 14 on) and the bus credits (intel_skl_wm.c) */
	intel_dbuf_hw_init(i915);
	if (is_mtl(i915))
		mtl_bw_buddy_init(i915);
	/* Battlemage: the arbiter's half-block end of burst off before any
	 * plane or cursor is enabled (the value 1 in the low bit is what
	 * the hardware sequence writes with it) */
	if (intel_display_verx100(i915) == 1401) {
		uint32_t v = i915_read32(i915, CHICKEN_MISC_2);
		i915_write32(i915, CHICKEN_MISC_2, (v & ~BMG_DARB_HALF_BLK_END_BURST) | 1u);
	}
	if (i915->info->display_ver == 12 || i915->info->display_ver == 13) {
		/* the DCPR's memory-up handling (Gen12/13) */
		i915_write32(i915, GEN11_CHICKEN_DCPR_2,
			     i915_read32(i915, GEN11_CHICKEN_DCPR_2) | DCPR_CLEAR_MEMSTAT_DIS |
				     DCPR_SEND_RESP_IMM | DCPR_MASK_LPMODE |
				     DCPR_MASK_MAXLATENCY_MEMUP_CLR);
	}
	if (i915->info->display_ver == 13)
		i915_write32(i915, XELPD_DISPLAY_ERR_FATAL_MASK, ~0u);

	d->pw_count[INTEL_PW_DISPLAY_CORE] = 1;
	return 0;
}

void intel_power_fini(struct i915_device *i915)
{
	/* Leave every well the firmware had on: we may be handing the
	 * screen back to its framebuffer. */
	(void)i915;
}
