// LikeOS -- the port PHY PLLs of Broxton and Gemini Lake.
//
// Every DDI here has a PLL of its own inside its PHY, programmed with a
// feedback divider (integer and fraction), a reference divider and two
// post dividers, from a table for the DisplayPort rates and a search for
// HDMI pixel clocks.  The two PHYs (one for port A, one for B and C) are
// powered and initialised by the firmware for the ports it lit; a PHY
// found off is brought up here.  Signal levels are register writes
// into the PHY's transmitter words, not a DDI translation table.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

#define BXT_REF_KHZ 100000 /* the PLLs run from a 100 MHz reference */

struct bxt_pll {
	uint32_t clock_khz;
	uint32_t p1, p2, n;
	uint32_t m2_int, m2_frac;
	int m2_frac_en;
	uint32_t vco_khz;
};

/* The DisplayPort rates (and the 100 MHz reference's dividers for them). */
static const struct bxt_pll dp_table[] = {
	{ 162000, 4, 2, 1, 32, 1677722, 1, 6480000 },
	{ 270000, 4, 1, 1, 27, 0, 0, 5400000 },
	{ 540000, 2, 1, 1, 27, 0, 0, 5400000 },
	{ 216000, 3, 2, 1, 32, 1677722, 1, 6480000 },
	{ 243000, 4, 1, 1, 24, 1258291, 1, 4860000 },
	{ 324000, 4, 1, 1, 32, 1677722, 1, 6480000 },
	{ 432000, 3, 1, 1, 32, 1677722, 1, 6480000 },
};

int bxt_dpll_rate_supported(uint32_t link_rate_khz)
{
	for (unsigned i = 0; i < sizeof(dp_table) / sizeof(dp_table[0]); i++)
		if (dp_table[i].clock_khz == link_rate_khz)
			return 1;
	return 0;
}

/* The search for an HDMI clock: VCO between 4.8 and 6.7 GHz with
 * p1 in 2..4, p2 in 1..20 even (or 1), m2 from the reference. */
static int bxt_calc(uint32_t clock_khz, struct bxt_pll *out)
{
	uint64_t best_err = ~0ULL;
	int found = 0;
	for (uint32_t p1 = 2; p1 <= 4; p1++) {
		for (uint32_t p2 = 1; p2 <= 20; p2 = (p2 == 1 ? 2 : p2 + 2)) {
			uint64_t vco = (uint64_t)clock_khz * p1 * p2 * 5 / 2; /* 2 = p0 */
			if (vco < 4800000 || vco > 6700000)
				continue;
			/* vco = 100 MHz * m2 / n, n = 1 */
			uint64_t m2_fp = (vco << 22) / BXT_REF_KHZ;
			uint32_t m2_int = (uint32_t)(m2_fp >> 22);
			uint32_t m2_frac = (uint32_t)(m2_fp & 0x3fffff);
			uint64_t got = ((uint64_t)m2_int * BXT_REF_KHZ +
					(((uint64_t)m2_frac * BXT_REF_KHZ) >> 22)) * 2 /
				       (p1 * p2 * 5);
			uint64_t err = got > clock_khz ? got - clock_khz : clock_khz - got;
			if (err < best_err) {
				best_err = err;
				out->clock_khz = clock_khz;
				out->p1 = p1;
				out->p2 = p2;
				out->n = 1;
				out->m2_int = m2_int;
				out->m2_frac = m2_frac;
				out->m2_frac_en = m2_frac != 0;
				out->vco_khz = (uint32_t)vco;
				found = 1;
			}
		}
	}
	return found ? 0 : -1;
}

static int pll_wait(struct i915_device *i915, uint32_t reg, uint32_t bit, int set)
{
	for (int t = 0; t < 1000; t++) {
		if (!!(i915_read32(i915, reg) & bit) == set)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static int bxt_pll_program(struct i915_device *i915, int port, const struct bxt_pll *p)
{
	uint32_t en = BXT_PORT_PLL_ENABLE(port);
	uint32_t v = i915_read32(i915, en);

	i915_write32(i915, en, (v & ~PORT_PLL_ENABLE) | PORT_PLL_REF_SEL);
	(void)i915_read32(i915, en);
	/* the loop filter coefficients by VCO */
	uint32_t prop, integ, gain, targ;
	if (p->vco_khz >= 6200000) {
		prop = 4; integ = 9; gain = 3; targ = 8;
	} else if (p->vco_khz >= 5400000) {
		prop = 5; integ = 11; gain = 3; targ = 9;
	} else {
		prop = 3; integ = 8; gain = 1; targ = 9;
	}
	v = i915_read32(i915, BXT_PORT_PLL_EBB_0(port));
	v &= ~(PORT_PLL_P1_MASK | PORT_PLL_P2_MASK);
	v |= PORT_PLL_P1(p->p1) | PORT_PLL_P2(p->p2);
	i915_write32(i915, BXT_PORT_PLL_EBB_0(port), v);
	v = i915_read32(i915, BXT_PORT_PLL_EBB_4(port));
	v |= PORT_PLL_10BIT_CLK_ENABLE;
	v &= ~PORT_PLL_RECALIBRATE;
	i915_write32(i915, BXT_PORT_PLL_EBB_4(port), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 0));
	v = (v & ~PORT_PLL_M2_INT_MASK) | p->m2_int;
	i915_write32(i915, BXT_PORT_PLL(port, 0), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 1));
	v = (v & ~PORT_PLL_N_MASK) | PORT_PLL_N(p->n);
	i915_write32(i915, BXT_PORT_PLL(port, 1), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 2));
	v = (v & ~PORT_PLL_M2_FRAC_MASK) | p->m2_frac;
	i915_write32(i915, BXT_PORT_PLL(port, 2), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 3));
	if (p->m2_frac_en)
		v |= PORT_PLL_M2_FRAC_ENABLE;
	else
		v &= ~PORT_PLL_M2_FRAC_ENABLE;
	i915_write32(i915, BXT_PORT_PLL(port, 3), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 6));
	v &= ~(PORT_PLL_PROP_COEFF_MASK | PORT_PLL_INT_COEFF_MASK | PORT_PLL_GAIN_CTL_MASK);
	v |= PORT_PLL_PROP_COEFF(prop) | PORT_PLL_INT_COEFF(integ) | PORT_PLL_GAIN_CTL(gain);
	i915_write32(i915, BXT_PORT_PLL(port, 6), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 8));
	v = (v & ~PORT_PLL_TARGET_CNT_MASK) | targ;
	i915_write32(i915, BXT_PORT_PLL(port, 8), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 9));
	v = (v & ~PORT_PLL_LOCK_THRESHOLD_MASK) | PORT_PLL_LOCK_THRESHOLD(5);
	i915_write32(i915, BXT_PORT_PLL(port, 9), v);
	v = i915_read32(i915, BXT_PORT_PLL(port, 10));
	v &= ~PORT_PLL_DCO_AMP_MASK;
	v |= PORT_PLL_DCO_AMP_OVR_EN_H | PORT_PLL_DCO_AMP(15);
	i915_write32(i915, BXT_PORT_PLL(port, 10), v);
	v = i915_read32(i915, BXT_PORT_PLL_EBB_4(port));
	i915_write32(i915, BXT_PORT_PLL_EBB_4(port), v | PORT_PLL_RECALIBRATE);
	(void)i915_read32(i915, BXT_PORT_PLL_EBB_4(port));
	/* on, and lock */
	v = i915_read32(i915, en);
	i915_write32(i915, en, v | PORT_PLL_ENABLE);
	if (pll_wait(i915, en, PORT_PLL_LOCK, 1) != 0) {
		kprintf("[drm] i915: port %c PLL did not lock\n", 'A' + port);
		return -EIO;
	}
	/* lane stagger for the lanes, from the clock */
	uint32_t stagger;
	if (p->clock_khz > 270000)
		stagger = 0x18;
	else if (p->clock_khz > 135000)
		stagger = 0x0d;
	else if (p->clock_khz > 67000)
		stagger = 0x07;
	else if (p->clock_khz > 33000)
		stagger = 0x04;
	else
		stagger = 0x02;
	v = i915_read32(i915, BXT_PORT_PCS_DW12_LN01(port));
	v &= ~LANE_STAGGER_MASK;
	v |= LANESTAGGER_STRAP_OVRD | stagger;
	i915_write32(i915, BXT_PORT_PCS_DW12_GRP(port), v);
	return 0;
}

int bxt_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < INTEL_MAX_DPLLS; i++)
		d->dpll[i].in_use = 0;
	return 0;
}

/* One PLL per port: the "PLL id" is the port. */
int bxt_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	struct intel_display *d = &i915->display;
	(void)ssc;
	for (unsigned i = 0; i < sizeof(dp_table) / sizeof(dp_table[0]); i++) {
		if (dp_table[i].clock_khz != link_rate_khz)
			continue;
		if (bxt_phy_init(i915, port))
			return -EIO;
		if (bxt_pll_program(i915, port, &dp_table[i]))
			return -EIO;
		d->dpll[port].in_use = 1;
		d->dpll[port].is_hdmi = 0;
		d->dpll[port].link_rate_khz = link_rate_khz;
		return port;
	}
	return -EINVAL;
}

int bxt_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	struct bxt_pll p;
	if (bxt_calc(clock_khz, &p)) {
		kprintf("[drm] i915: no port PLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	if (bxt_phy_init(i915, port))
		return -EIO;
	if (bxt_pll_program(i915, port, &p))
		return -EIO;
	d->dpll[port].in_use = 1;
	d->dpll[port].is_hdmi = 1;
	d->dpll[port].link_rate_khz = clock_khz;
	return port;
}

void bxt_dpll_put(struct i915_device *i915, int port)
{
	struct intel_display *d = &i915->display;
	if (port < 0 || port >= INTEL_MAX_PORTS)
		return;
	d->dpll[port].in_use = 0;
	uint32_t en = BXT_PORT_PLL_ENABLE(port);
	i915_write32(i915, en, i915_read32(i915, en) & ~PORT_PLL_ENABLE);
	pll_wait(i915, en, PORT_PLL_LOCK, 0);
}

/* The port's PLL is its own: nothing to route, but the clock is gated
 * in the same register the Skylake ports use. */
void bxt_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	(void)pll;
	uint32_t v = i915_read32(i915, DPLL_CTRL2);
	v &= ~DPLL_CTRL2_DDI_CLK_OFF(port);
	i915_write32(i915, DPLL_CTRL2, v);
}

void bxt_dpll_unroute_port(struct i915_device *i915, int port)
{
	uint32_t v = i915_read32(i915, DPLL_CTRL2);
	i915_write32(i915, DPLL_CTRL2, v | DPLL_CTRL2_DDI_CLK_OFF(port));
}

uint32_t bxt_dpll_port_link_rate(struct i915_device *i915, int port)
{
	if (!(i915_read32(i915, BXT_PORT_PLL_ENABLE(port)) & PORT_PLL_LOCK))
		return 0;
	uint32_t ebb0 = i915_read32(i915, BXT_PORT_PLL_EBB_0(port));
	uint32_t p1 = (ebb0 & PORT_PLL_P1_MASK) >> 13;
	uint32_t p2 = (ebb0 & PORT_PLL_P2_MASK) >> 8;
	uint32_t m2_int = i915_read32(i915, BXT_PORT_PLL(port, 0)) & PORT_PLL_M2_INT_MASK;
	uint32_t m2_frac = i915_read32(i915, BXT_PORT_PLL(port, 2)) & PORT_PLL_M2_FRAC_MASK;
	int frac_en = !!(i915_read32(i915, BXT_PORT_PLL(port, 3)) & PORT_PLL_M2_FRAC_ENABLE);
	if (!p1 || !p2)
		return 0;
	uint64_t vco = (uint64_t)m2_int * BXT_REF_KHZ;
	if (frac_en)
		vco += ((uint64_t)m2_frac * BXT_REF_KHZ) >> 22;
	for (unsigned i = 0; i < sizeof(dp_table) / sizeof(dp_table[0]); i++)
		if (dp_table[i].p1 == p1 && dp_table[i].p2 == p2 && dp_table[i].m2_int == m2_int)
			return dp_table[i].clock_khz;
	return (uint32_t)(vco * 2 / (p1 * p2 * 5));
}

/* ---- the PHY ------------------------------------------------------------------- */

/* Power and initialise the port's PHY unless the firmware already did. */
int bxt_phy_init(struct i915_device *i915, int port)
{
	int phy = BXT_PORT_PHY(port);
	uint32_t pwron = i915_read32(i915, BXT_P_CR_GT_DISP_PWRON);
	if (!(pwron & GT_DISPLAY_POWER_ON(phy))) {
		i915_write32(i915, BXT_P_CR_GT_DISP_PWRON, pwron | GT_DISPLAY_POWER_ON(phy));
		for (int t = 0; t < 1000; t++) {
			if (i915_read32(i915, BXT_PORT_CL1CM_DW0(phy)) & PHY_POWER_GOOD)
				break;
			lapic_delay_us(10);
		}
	}
	uint32_t fam = i915_read32(i915, BXT_PHY_CTL_FAMILY(phy));
	if (fam & COMMON_RESET_DIS)
		return 0; /* initialised (by the firmware, or earlier here) */
	i915_dbg("[drm] i915: initialising DPIO PHY %d\n", phy);
	if (!(i915_read32(i915, BXT_PORT_CL1CM_DW0(phy)) & PHY_POWER_GOOD)) {
		kprintf("[drm] i915: DPIO PHY %d has no power\n", phy);
		return -EIO;
	}
	i915_write32(i915, BXT_PHY_CTL_FAMILY(phy), fam | COMMON_RESET_DIS);
	(void)i915_read32(i915, BXT_PHY_CTL_FAMILY(phy));
	return 0;
}

/* Signal levels: margin, scale and de-emphasis per level, the Broxton
 * tables in the DisplayPort order (400mV x4, 600 x3, 800 x2, 1200). */
struct bxt_level {
	uint8_t margin, scale, enable, deemphasis;
};
static const struct bxt_level bxt_dp[] = {
	{ 52, 0x9A, 0, 128 }, { 78, 0x9A, 0, 85 }, { 104, 0x9A, 0, 64 }, { 154, 0x9A, 0, 43 },
	{ 77, 0x9A, 0, 128 }, { 116, 0x9A, 0, 85 }, { 154, 0x9A, 0, 64 }, { 102, 0x9A, 0, 128 },
	{ 154, 0x9A, 0, 85 }, { 154, 0x9A, 1, 128 },
};
static const struct bxt_level bxt_edp[] = {
	{ 26, 0, 0, 128 }, { 38, 0, 0, 112 }, { 48, 0, 0, 96 }, { 54, 0, 0, 69 },
	{ 32, 0, 0, 128 }, { 48, 0, 0, 104 }, { 54, 0, 0, 85 }, { 43, 0, 0, 128 },
	{ 54, 0, 0, 101 }, { 48, 0, 0, 128 },
};
static const struct bxt_level bxt_hdmi[] = {
	{ 52, 0x9A, 0, 128 }, { 52, 0x9A, 0, 85 }, { 52, 0x9A, 0, 64 }, { 42, 0x9A, 0, 43 },
	{ 77, 0x9A, 0, 128 }, { 77, 0x9A, 0, 85 }, { 77, 0x9A, 0, 64 }, { 102, 0x9A, 0, 128 },
	{ 102, 0x9A, 0, 85 }, { 154, 0x9A, 1, 128 },
};

void bxt_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	const struct bxt_level *t;
	int n = 10;
	int port = o->port;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		t = bxt_hdmi;
	else if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std)
		t = bxt_edp;
	else
		t = bxt_dp;
	if (level < 0 || level >= n)
		level = (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) ? 9 : 0;
	const struct bxt_level *l = &t[level];

	/* the swing calculation off while the values change */
	uint32_t v = i915_read32(i915, BXT_PORT_PCS_DW10_LN01(port));
	v &= ~(TX2_SWING_CALC_EN | TX1_SWING_CALC_EN);
	i915_write32(i915, BXT_PORT_PCS_DW10_GRP(port), v);
	v = i915_read32(i915, BXT_PORT_TX_DW2_LN0(port));
	v &= ~(TX_MARGIN_MASK | 0xff00u);
	v |= MARGIN_000(l->margin) | UNIQ_TRANS_SCALE(l->scale);
	i915_write32(i915, BXT_PORT_TX_DW2_GRP(port), v);
	v = i915_read32(i915, BXT_PORT_TX_DW3_LN0(port));
	v &= ~(SCALE_DCOMP_METHOD | UNIQE_TRANGE_EN_METHOD);
	if (l->enable)
		v |= SCALE_DCOMP_METHOD;
	i915_write32(i915, BXT_PORT_TX_DW3_GRP(port), v);
	v = i915_read32(i915, BXT_PORT_TX_DW4_LN0(port));
	v &= ~(0xffu << 24);
	v |= DE_EMPHASIS(l->deemphasis);
	i915_write32(i915, BXT_PORT_TX_DW4_GRP(port), v);
	v = i915_read32(i915, BXT_PORT_PCS_DW10_LN01(port));
	v |= TX2_SWING_CALC_EN | TX1_SWING_CALC_EN;
	i915_write32(i915, BXT_PORT_PCS_DW10_GRP(port), v);
	(void)i915_read32(i915, BXT_PORT_PCS_DW10_GRP(port));
}
