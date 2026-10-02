// LikeOS -- the port PHYs of Broxton and Gemini Lake, and their PLLs.
//
// Every DDI here has a PLL of its own inside its DPIO PHY, programmed
// with a feedback divider (integer and 22-bit fraction), a reference
// divider and two post dividers, from a table for the DisplayPort rates
// and a search for HDMI pixel clocks (intel_dpll_calc.c).  The PHYs are
// Broxton's two (PHY0 dual channel for ports B and C, PHY1 for port A)
// or Gemini Lake's three (one per port).  A PHY is powered through the
// Punit and then initialised: compensation offsets, power gating, and --
// for a PHY without a compensation resistor of its own -- the
// calibrated resistance code copied over from PHY1, which therefore has
// to be up first.  The firmware does all of this for the ports it lit; a
// PHY found otherwise is brought up here.  Signal levels are register
// writes into the PHY's transmitter words, not a DDI translation table.
// There is no clock routing: a port's PLL is its own.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dpll_calc.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* ---- the PHYs ------------------------------------------------------------------- */

struct dpio_phy_info {
	int dual_channel;
	/* the PHY whose compensation code this one copies, or -1 for a
	 * PHY with its own resistor */
	int rcomp_phy;
	/* microseconds before the common reset is released */
	int reset_delay;
	/* the PHY's bit in the Punit's power-on register */
	uint32_t pwron_mask;
	int port[2]; /* the port on each channel */
};

static const struct dpio_phy_info bxt_phys[] = {
	{ 1, 1, 0, 1u << 0, { PORT_B, PORT_C } },
	{ 0, -1, 0, 1u << 1, { PORT_A, -1 } },
};

static const struct dpio_phy_info glk_phys[] = {
	{ 0, 1, 20, 1u << 0, { PORT_B, -1 } },
	{ 0, -1, 20, 1u << 3, { PORT_A, -1 } },
	{ 0, 1, 20, 1u << 1, { PORT_C, -1 } },
};

static int is_glk(struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_GEMINILAKE;
}

static const struct dpio_phy_info *phy_list(struct i915_device *i915, int *count)
{
	if (is_glk(i915)) {
		*count = (int)(sizeof(glk_phys) / sizeof(glk_phys[0]));
		return glk_phys;
	}
	*count = (int)(sizeof(bxt_phys) / sizeof(bxt_phys[0]));
	return bxt_phys;
}

static const struct dpio_phy_info *phy_info(struct i915_device *i915, int phy)
{
	int count;
	return &phy_list(i915, &count)[phy];
}

/* Which PHY and channel carry a port. */
static void port_to_phy_ch(struct i915_device *i915, int port, int *phy, int *ch)
{
	int count;
	const struct dpio_phy_info *phys = phy_list(i915, &count);

	for (int i = 0; i < count; i++) {
		if (phys[i].port[0] == port) {
			*phy = i;
			*ch = 0;
			return;
		}
		if (phys[i].dual_channel && phys[i].port[1] == port) {
			*phy = i;
			*ch = 1;
			return;
		}
	}
	kprintf("[drm] i915: no DPIO PHY for port %c\n", 'A' + port);
	*phy = 0;
	*ch = 0;
}

static void rmw(struct i915_device *i915, uint32_t reg, uint32_t clear, uint32_t set)
{
	i915_write32(i915, reg, (i915_read32(i915, reg) & ~clear) | set);
}

static int wait_mask(struct i915_device *i915, uint32_t reg, uint32_t mask, uint32_t value,
		     uint32_t timeout_us)
{
	for (uint32_t t = 0; t <= timeout_us; t += 10) {
		if ((i915_read32(i915, reg) & mask) == value)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static int dpio_phy_is_enabled(struct i915_device *i915, int phy)
{
	const struct dpio_phy_info *info = phy_info(i915, phy);

	if (!(i915_read32(i915, BXT_DPIO_GT_DISP_PWRON) & info->pwron_mask))
		return 0;
	if ((i915_read32(i915, BXT_DPIO_CL1CM_DW0(phy)) &
	     (BXT_DPIO_PHY_POWER_GOOD | BXT_DPIO_PHY_RESERVED)) != BXT_DPIO_PHY_POWER_GOOD) {
		i915_dbg("[drm] i915: DPIO PHY %d powered, but power hasn't settled\n", phy);
		return 0;
	}
	if (!(i915_read32(i915, BXT_DPIO_PHY_CTL_FAMILY(phy)) & BXT_DPIO_COMMON_RESET_DIS)) {
		i915_dbg("[drm] i915: DPIO PHY %d powered, but still in reset\n", phy);
		return 0;
	}
	return 1;
}

/* The calibrated resistance code a PHY reports. */
static uint32_t dpio_grc(struct i915_device *i915, int phy)
{
	return (i915_read32(i915, BXT_DPIO_REF_DW6(phy)) & BXT_DPIO_GRC_CODE_MASK) >>
	       BXT_DPIO_GRC_CODE_SHIFT;
}

static int check_reg(struct i915_device *i915, int phy, uint32_t reg, uint32_t mask,
		     uint32_t expected)
{
	uint32_t v = i915_read32(i915, reg);
	if ((v & mask) == expected)
		return 1;
	i915_dbg("[drm] i915: DPIO PHY %d reg %06x is %08x, expected %08x under %08x\n", phy, reg, v,
		 expected, mask);
	return 0;
}

/* Is a powered PHY set up the way the init below leaves it? */
static int dpio_phy_verify(struct i915_device *i915, int phy, uint32_t grc)
{
	const struct dpio_phy_info *info = phy_info(i915, phy);
	uint32_t mask;
	int ok = 1;

	ok &= check_reg(i915, phy, BXT_DPIO_CL1CM_DW9(phy), BXT_DPIO_IREF0RC_OFFSET_MASK,
			BXT_DPIO_IREF0RC_OFFSET(0xe4));
	ok &= check_reg(i915, phy, BXT_DPIO_CL1CM_DW10(phy), BXT_DPIO_IREF1RC_OFFSET_MASK,
			BXT_DPIO_IREF1RC_OFFSET(0xe4));
	mask = BXT_DPIO_OCL1_POWER_DOWN_EN | BXT_DPIO_DW28_OLDO_DYN_PWR_DOWN_EN |
	       BXT_DPIO_SUS_CLK_CONFIG;
	ok &= check_reg(i915, phy, BXT_DPIO_CL1CM_DW28(phy), mask, mask);
	if (info->dual_channel)
		ok &= check_reg(i915, phy, BXT_DPIO_CL2CM_DW6(phy), BXT_DPIO_DW6_OLDO_DYN_PWR_DOWN_EN,
				BXT_DPIO_DW6_OLDO_DYN_PWR_DOWN_EN);
	if (info->rcomp_phy != -1) {
		uint32_t code = BXT_DPIO_GRC_CODE_FAST(grc) | BXT_DPIO_GRC_CODE_SLOW(grc) |
				BXT_DPIO_GRC_CODE_NOM(grc);
		ok &= check_reg(i915, phy, BXT_DPIO_REF_DW6(phy), 0x00ffffffu, code);
		mask = BXT_DPIO_GRC_DIS | BXT_DPIO_GRC_RDY_OVRD;
		ok &= check_reg(i915, phy, BXT_DPIO_REF_DW8(phy), mask, mask);
	}
	return ok;
}

static void dpio_phy_init_one(struct i915_device *i915, int phy)
{
	const struct dpio_phy_info *info = phy_info(i915, phy);

	if (dpio_phy_is_enabled(i915, phy)) {
		uint32_t grc = info->rcomp_phy != -1 ? dpio_grc(i915, phy) : 0;
		if (dpio_phy_verify(i915, phy, grc))
			return; /* already enabled: left as it is */
		i915_dbg("[drm] i915: DPIO PHY %d enabled with an invalid state, reprogramming it\n",
			 phy);
	}
	rmw(i915, BXT_DPIO_GT_DISP_PWRON, 0, info->pwron_mask);
	/* The registers read all ones until the PHY is powered; then the
	 * reserved bit reads its default 0. */
	if (wait_mask(i915, BXT_DPIO_CL1CM_DW0(phy), BXT_DPIO_PHY_RESERVED | BXT_DPIO_PHY_POWER_GOOD,
		      BXT_DPIO_PHY_POWER_GOOD, 1000))
		kprintf("[drm] i915: timeout during DPIO PHY %d power on\n", phy);

	/* the PLL's compensation code offset */
	rmw(i915, BXT_DPIO_CL1CM_DW9(phy), BXT_DPIO_IREF0RC_OFFSET_MASK, BXT_DPIO_IREF0RC_OFFSET(0xE4));
	rmw(i915, BXT_DPIO_CL1CM_DW10(phy), BXT_DPIO_IREF1RC_OFFSET_MASK,
	    BXT_DPIO_IREF1RC_OFFSET(0xE4));
	/* power gating */
	rmw(i915, BXT_DPIO_CL1CM_DW28(phy), 0,
	    BXT_DPIO_OCL1_POWER_DOWN_EN | BXT_DPIO_DW28_OLDO_DYN_PWR_DOWN_EN | BXT_DPIO_SUS_CLK_CONFIG);
	if (info->dual_channel)
		rmw(i915, BXT_DPIO_CL2CM_DW6(phy), 0, BXT_DPIO_DW6_OLDO_DYN_PWR_DOWN_EN);
	if (info->rcomp_phy != -1) {
		/* This PHY has no compensation resistor: the code PHY1
		 * calibrated is copied in, and automatic calibration off. */
		if (wait_mask(i915, BXT_DPIO_REF_DW3(info->rcomp_phy), BXT_DPIO_GRC_DONE,
			      BXT_DPIO_GRC_DONE, 10000))
			kprintf("[drm] i915: timeout waiting for DPIO PHY %d's resistance calibration\n",
				info->rcomp_phy);
		uint32_t grc = dpio_grc(i915, info->rcomp_phy);
		i915_write32(i915, BXT_DPIO_REF_DW6(phy),
			     BXT_DPIO_GRC_CODE_FAST(grc) | BXT_DPIO_GRC_CODE_SLOW(grc) |
				     BXT_DPIO_GRC_CODE_NOM(grc));
		rmw(i915, BXT_DPIO_REF_DW8(phy), 0, BXT_DPIO_GRC_DIS | BXT_DPIO_GRC_RDY_OVRD);
	}
	if (info->reset_delay)
		lapic_delay_us((uint32_t)info->reset_delay);
	rmw(i915, BXT_DPIO_PHY_CTL_FAMILY(phy), 0, BXT_DPIO_COMMON_RESET_DIS);
	i915_dbg("[drm] i915: DPIO PHY %d initialised\n", phy);
}

static void dpio_phy_uninit(struct i915_device *i915, int phy)
{
	rmw(i915, BXT_DPIO_PHY_CTL_FAMILY(phy), BXT_DPIO_COMMON_RESET_DIS, 0);
	rmw(i915, BXT_DPIO_GT_DISP_PWRON, phy_info(i915, phy)->pwron_mask, 0);
}

/* A PHY that copies its compensation code needs the PHY it copies from
 * up while it is initialised; if that one was off it goes off again. */
static void dpio_phy_init(struct i915_device *i915, int phy)
{
	int rcomp = phy_info(i915, phy)->rcomp_phy;
	int was_enabled = 1;

	if (rcomp != -1)
		was_enabled = dpio_phy_is_enabled(i915, rcomp);
	if (!was_enabled)
		dpio_phy_init_one(i915, rcomp);
	dpio_phy_init_one(i915, phy);
	if (!was_enabled)
		dpio_phy_uninit(i915, rcomp);
}

int bxt_dpio_phy_init_port(struct i915_device *i915, int port)
{
	int phy, ch;
	port_to_phy_ch(i915, port, &phy, &ch);
	dpio_phy_init(i915, phy);
	return dpio_phy_is_enabled(i915, phy) ? 0 : -EIO;
}

int bxt_phy_init(struct i915_device *i915, int port)
{
	return bxt_dpio_phy_init_port(i915, port);
}

/* Lane latency optimisation for a DisplayPort link of `lanes' lanes. */
static void dpio_set_lane_optim(struct i915_device *i915, int port, int lanes)
{
	uint32_t mask;
	int phy, ch;

	switch (lanes) {
	case 2: mask = (1u << 2) | (1u << 0); break;
	case 4: mask = (1u << 3) | (1u << 2) | (1u << 0); break;
	default: mask = 0; break;
	}
	port_to_phy_ch(i915, port, &phy, &ch);
	for (int lane = 0; lane < 4; lane++)
		rmw(i915, BXT_DPIO_TX_DW14_LN(phy, ch, lane), BXT_DPIO_LATENCY_OPTIM,
		    (mask & (1u << lane)) ? BXT_DPIO_LATENCY_OPTIM : 0);
}

/* ---- the port PLL ------------------------------------------------------------- */

int bxt_dpll_rate_supported(uint32_t link_rate_khz)
{
	struct bxt_pll_dividers d;
	return bxt_dp_dividers(link_rate_khz, &d) == 0;
}

static int pll_wait(struct i915_device *i915, uint32_t reg, uint32_t bit, int set)
{
	return wait_mask(i915, reg, bit, set ? bit : 0, 200);
}

static void bxt_pll_disable(struct i915_device *i915, int port)
{
	uint32_t en = BXT_DPIO_PORT_PLL_ENABLE(port);

	rmw(i915, en, PORT_PLL_ENABLE, 0);
	(void)i915_read32(i915, en);
	if (is_glk(i915)) {
		rmw(i915, en, PORT_PLL_POWER_ENABLE, 0);
		if (pll_wait(i915, en, PORT_PLL_POWER_STATE, 0))
			kprintf("[drm] i915: port %c PLL power state not reset\n", 'A' + port);
	}
}

/* Program and lock the port's PLL for a port clock (the link rate for
 * DisplayPort, the TMDS clock for HDMI). */
static int bxt_pll_enable(struct i915_device *i915, int port, const struct bxt_pll_dividers *dv,
			  uint32_t clock_khz)
{
	uint32_t en = BXT_DPIO_PORT_PLL_ENABLE(port);
	uint32_t prop, integ, gain, targ, stagger, v;
	int phy, ch;

	/* the loop filter by VCO */
	if (dv->vco >= 6200000 && dv->vco <= 6700000) {
		prop = 4; integ = 9; gain = 3; targ = 8;
	} else if ((dv->vco > 5400000 && dv->vco < 6200000) ||
		   (dv->vco >= 4800000 && dv->vco < 5400000)) {
		prop = 5; integ = 11; gain = 3; targ = 9;
	} else if (dv->vco == 5400000) {
		prop = 3; integ = 8; gain = 1; targ = 9;
	} else {
		kprintf("[drm] i915: port %c: invalid PLL VCO %u kHz\n", 'A' + port, dv->vco);
		return -EINVAL;
	}
	/* the lane stagger by port clock */
	if (clock_khz > 270000)
		stagger = 0x18;
	else if (clock_khz > 135000)
		stagger = 0x0d;
	else if (clock_khz > 67000)
		stagger = 0x07;
	else if (clock_khz > 33000)
		stagger = 0x04;
	else
		stagger = 0x02;

	port_to_phy_ch(i915, port, &phy, &ch);
	/* a running PLL takes new dividers only after being stopped */
	if (i915_read32(i915, en) & PORT_PLL_ENABLE)
		bxt_pll_disable(i915, port);

	/* non-SSC reference */
	rmw(i915, en, 0, PORT_PLL_REF_SEL);
	if (is_glk(i915)) {
		rmw(i915, en, 0, PORT_PLL_POWER_ENABLE);
		if (pll_wait(i915, en, PORT_PLL_POWER_STATE, 1))
			kprintf("[drm] i915: port %c PLL power state not set\n", 'A' + port);
	}
	/* 10 bit clock off while programming */
	rmw(i915, BXT_DPIO_PLL_EBB_4(phy, ch), PORT_PLL_10BIT_CLK_ENABLE, 0);
	rmw(i915, BXT_DPIO_PLL_EBB_0(phy, ch), PORT_PLL_P1_MASK | PORT_PLL_P2_MASK,
	    PORT_PLL_P1(dv->p1) | PORT_PLL_P2(dv->p2));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 0), PORT_PLL_M2_INT_MASK, dv->m2 >> 22);
	rmw(i915, BXT_DPIO_PLL(phy, ch, 1), PORT_PLL_N_MASK, PORT_PLL_N(dv->n));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 2), PORT_PLL_M2_FRAC_MASK, BXT_DPIO_PLL_M2_FRAC(dv->m2));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 3), PORT_PLL_M2_FRAC_ENABLE,
	    (dv->m2 & 0x3fffff) ? PORT_PLL_M2_FRAC_ENABLE : 0);
	rmw(i915, BXT_DPIO_PLL(phy, ch, 6),
	    PORT_PLL_PROP_COEFF_MASK | PORT_PLL_INT_COEFF_MASK | PORT_PLL_GAIN_CTL_MASK,
	    PORT_PLL_PROP_COEFF(prop) | PORT_PLL_INT_COEFF(integ) | PORT_PLL_GAIN_CTL(gain));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 8), PORT_PLL_TARGET_CNT_MASK, BXT_DPIO_PLL_TARGET_CNT(targ));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 9), BXT_DPIO_PLL_LOCK_THRESHOLD_MASK,
	    BXT_DPIO_PLL_LOCK_THRESHOLD(5));
	rmw(i915, BXT_DPIO_PLL(phy, ch, 10), PORT_PLL_DCO_AMP_OVR_EN_H | PORT_PLL_DCO_AMP_MASK,
	    PORT_PLL_DCO_AMP(15) | PORT_PLL_DCO_AMP_OVR_EN_H);
	/* recalibrate with the new settings, 10 bit clock back on */
	v = i915_read32(i915, BXT_DPIO_PLL_EBB_4(phy, ch));
	v |= PORT_PLL_RECALIBRATE;
	i915_write32(i915, BXT_DPIO_PLL_EBB_4(phy, ch), v);
	v |= PORT_PLL_10BIT_CLK_ENABLE;
	i915_write32(i915, BXT_DPIO_PLL_EBB_4(phy, ch), v);

	rmw(i915, en, 0, PORT_PLL_ENABLE);
	(void)i915_read32(i915, en);
	if (pll_wait(i915, en, PORT_PLL_LOCK, 1)) {
		kprintf("[drm] i915: port %c PLL did not lock\n", 'A' + port);
		return -EIO;
	}
	if (is_glk(i915)) {
		v = i915_read32(i915, BXT_DPIO_TX_DW5_LN(phy, ch, 0));
		i915_write32(i915, BXT_DPIO_TX_DW5_GRP(phy, ch), v | BXT_DPIO_DCC_DELAY_RANGE_2);
	}
	/* The group register writes every lane; only the lane registers
	 * read back, lanes 0/1 standing for all. */
	v = i915_read32(i915, BXT_DPIO_PCS_DW12_LN01(phy, ch));
	v &= ~(LANE_STAGGER_MASK | LANESTAGGER_STRAP_OVRD);
	v |= LANESTAGGER_STRAP_OVRD | stagger;
	i915_write32(i915, BXT_DPIO_PCS_DW12_GRP(phy, ch), v);
	return 0;
}

int bxt_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].link_rate_khz = 0;
	}
	return 0;
}

static struct intel_output *dp_output_on(struct i915_device *i915, int port)
{
	for (int i = 0; i < i915->display.nout; i++) {
		struct intel_output *o = &i915->display.outputs[i];
		if (o->port == port && (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP))
			return o;
	}
	return NULL;
}

/* One PLL per port: the "PLL id" is the port. */
int bxt_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	struct intel_display *d = &i915->display;
	struct intel_output *o = dp_output_on(i915, port);
	struct bxt_pll_dividers dv;
	(void)ssc; /* the port PLLs run from the non-SSC reference */

	if (port < PORT_A || port > PORT_C || bxt_dp_dividers(link_rate_khz, &dv))
		return -EINVAL;
	if (bxt_phy_init(i915, port))
		return -EIO;
	/* the lane latency optimisation goes in before the PLL */
	dpio_set_lane_optim(i915, port, o && o->lane_count ? o->lane_count : 4);
	if (bxt_pll_enable(i915, port, &dv, link_rate_khz))
		return -EIO;
	d->dpll[port].in_use = 1;
	d->dpll[port].is_hdmi = 0;
	d->dpll[port].link_rate_khz = link_rate_khz;
	return port;
}

int bxt_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	struct bxt_pll_dividers dv;

	if (port < PORT_A || port > PORT_C)
		return -EINVAL;
	if (bxt_hdmi_dividers(clock_khz, &dv)) {
		kprintf("[drm] i915: no port PLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	if (bxt_phy_init(i915, port))
		return -EIO;
	dpio_set_lane_optim(i915, port, 4); /* TMDS takes all four lanes */
	if (bxt_pll_enable(i915, port, &dv, dv.dot))
		return -EIO;
	d->dpll[port].in_use = 1;
	d->dpll[port].is_hdmi = 1;
	d->dpll[port].link_rate_khz = dv.dot;
	return port;
}

void bxt_dpll_put(struct i915_device *i915, int port)
{
	struct intel_display *d = &i915->display;
	if (port < PORT_A || port > PORT_C)
		return;
	d->dpll[port].in_use = 0;
	bxt_pll_disable(i915, port);
}

/* The port's PLL is its own and has no clock select or gate. */
void bxt_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	(void)i915;
	(void)port;
	(void)pll;
}

void bxt_dpll_unroute_port(struct i915_device *i915, int port)
{
	(void)i915;
	(void)port;
}

/* The port clock the PLL runs at, from its dividers (0 when off). */
uint32_t bxt_dpll_port_link_rate(struct i915_device *i915, int port)
{
	struct bxt_pll_dividers dv;
	int phy, ch;

	if (port < PORT_A || port > PORT_C)
		return 0;
	if (!(i915_read32(i915, BXT_DPIO_PORT_PLL_ENABLE(port)) & PORT_PLL_ENABLE))
		return 0;
	port_to_phy_ch(i915, port, &phy, &ch);
	uint32_t ebb0 = i915_read32(i915, BXT_DPIO_PLL_EBB_0(phy, ch));
	dv.p1 = (ebb0 & PORT_PLL_P1_MASK) >> 13;
	dv.p2 = (ebb0 & PORT_PLL_P2_MASK) >> 8;
	dv.n = (i915_read32(i915, BXT_DPIO_PLL(phy, ch, 1)) & PORT_PLL_N_MASK) >> 8;
	dv.m1 = 2;
	dv.m2 = (i915_read32(i915, BXT_DPIO_PLL(phy, ch, 0)) & PORT_PLL_M2_INT_MASK) << 22;
	if (i915_read32(i915, BXT_DPIO_PLL(phy, ch, 3)) & PORT_PLL_M2_FRAC_ENABLE)
		dv.m2 |= i915_read32(i915, BXT_DPIO_PLL(phy, ch, 2)) & PORT_PLL_M2_FRAC_MASK;
	if (!dv.p1 || !dv.p2 || !dv.n)
		return 0;
	return bxt_pll_calc_params(&dv);
}

/* ---- signal levels ------------------------------------------------------------ */

/* Margin, scale, scale enable and de-emphasis per level, in the
 * DisplayPort order (400 mV x4, 600 x3, 800 x2, 1200). */
struct bxt_level {
	uint8_t margin, scale, enable, deemphasis;
};
static const struct bxt_level bxt_dp[] = {
	{ 52, 0x9A, 0, 128 }, { 78, 0x9A, 0, 85 }, { 104, 0x9A, 0, 64 }, { 154, 0x9A, 0, 43 },
	{ 77, 0x9A, 0, 128 }, { 116, 0x9A, 0, 85 }, { 154, 0x9A, 0, 64 }, { 102, 0x9A, 0, 128 },
	{ 154, 0x9A, 0, 85 }, { 154, 0x9A, 1, 128 },
};
/* low voltage swing panels: 200, 250, 300 mV */
static const struct bxt_level bxt_edp[] = {
	{ 26, 0, 0, 128 }, { 38, 0, 0, 112 }, { 48, 0, 0, 96 }, { 54, 0, 0, 69 },
	{ 32, 0, 0, 128 }, { 48, 0, 0, 104 }, { 54, 0, 0, 85 }, { 43, 0, 0, 128 },
	{ 54, 0, 0, 101 }, { 48, 0, 0, 128 },
};
/* HDMI: the last entry (1200 mV) is the default */
static const struct bxt_level bxt_hdmi[] = {
	{ 52, 0x9A, 0, 128 }, { 52, 0x9A, 0, 85 }, { 52, 0x9A, 0, 64 }, { 42, 0x9A, 0, 43 },
	{ 77, 0x9A, 0, 128 }, { 77, 0x9A, 0, 85 }, { 77, 0x9A, 0, 64 }, { 102, 0x9A, 0, 128 },
	{ 102, 0x9A, 0, 85 }, { 154, 0x9A, 1, 128 },
};
#define BXT_N_LEVELS 10
#define BXT_HDMI_DEFAULT_LEVEL (BXT_N_LEVELS - 1)

void bxt_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	int hdmi = o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI;
	const struct bxt_level *t;
	int phy, ch, lanes;
	uint32_t v;

	if (hdmi)
		t = bxt_hdmi;
	else if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std)
		t = bxt_edp;
	else
		t = bxt_dp;
	if (level < 0 || level >= BXT_N_LEVELS)
		level = hdmi ? BXT_HDMI_DEFAULT_LEVEL : 0;
	lanes = hdmi ? 4 : (o->lane_count ? o->lane_count : 4);
	const struct bxt_level *l = &t[level];

	port_to_phy_ch(i915, o->port, &phy, &ch);
	/* the swing calculation off while the values change (lanes 0/1
	 * read, the group written) */
	v = i915_read32(i915, BXT_DPIO_PCS_DW10_LN01(phy, ch));
	v &= ~(BXT_DPIO_TX2_SWING_CALC_INIT | BXT_DPIO_TX1_SWING_CALC_INIT);
	i915_write32(i915, BXT_DPIO_PCS_DW10_GRP(phy, ch), v);
	for (int lane = 0; lane < lanes; lane++)
		rmw(i915, BXT_DPIO_TX_DW2_LN(phy, ch, lane),
		    BXT_DPIO_MARGIN_000_MASK | BXT_DPIO_UNIQ_TRANS_SCALE_MASK,
		    MARGIN_000(l->margin) | UNIQ_TRANS_SCALE(l->scale));
	for (int lane = 0; lane < lanes; lane++) {
		rmw(i915, BXT_DPIO_TX_DW3_LN(phy, ch, lane), SCALE_DCOMP_METHOD,
		    l->enable ? SCALE_DCOMP_METHOD : 0);
		v = i915_read32(i915, BXT_DPIO_TX_DW3_LN(phy, ch, lane));
		if ((v & UNIQE_TRANGE_EN_METHOD) && !(v & SCALE_DCOMP_METHOD))
			kprintf("[drm] i915: port %c lane %d: scaling off while the unique transition range method is on\n",
				'A' + o->port, lane);
	}
	for (int lane = 0; lane < lanes; lane++)
		rmw(i915, BXT_DPIO_TX_DW4_LN(phy, ch, lane), BXT_DPIO_DE_EMPHASIS_MASK,
		    DE_EMPHASIS(l->deemphasis));
	v = i915_read32(i915, BXT_DPIO_PCS_DW10_LN01(phy, ch));
	v |= BXT_DPIO_TX2_SWING_CALC_INIT | BXT_DPIO_TX1_SWING_CALC_INIT;
	i915_write32(i915, BXT_DPIO_PCS_DW10_GRP(phy, ch), v);
	(void)i915_read32(i915, BXT_DPIO_PCS_DW10_LN01(phy, ch));
}
