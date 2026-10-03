// LikeOS -- the display PLLs of Skylake (and Kaby, Coffee, Comet Lake),
// and the dispatcher that picks each generation's PLL code.
//
// Four of them.  DPLL0 belongs to CDCLK: it runs at a VCO of 8100 or
// 8640 MHz the CDCLK code chose, and is never switched off here.  Its
// link-rate field can still be changed while it runs, as long as the
// new rate comes from the same VCO, and that is how an embedded panel is
// clocked: from DPLL0.  DPLL1-3 serve the other ports; for DisplayPort
// a PLL is given a link rate from a fixed list, for HDMI it runs in
// "HDMI mode" with dividers worked out from the pixel clock
// (intel_dpll_calc.c).  A port is routed to its PLL through DPLL_CTRL2.
// Two ports asking for the same configuration share one PLL.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dpll_calc.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

#define SKL_NUM_DPLLS 4

static uint32_t dpll_ctl_reg(int pll)
{
	switch (pll) {
	case 0: return LCPLL1_CTL;
	case 1: return LCPLL2_CTL;
	case 2: return WRPLL_CTL1;
	default: return WRPLL_CTL2;
	}
}

/* The rate field names HALF the port's clock: a DisplayPort link running
 * at 2.7 GHz is the 1350 entry, one at 1.62 GHz the 810 entry.  The six
 * entries are the six link rates a port can be clocked at here -- the
 * three DisplayPort rates below 8.1 GHz and the three extra embedded
 * ones.  Giving the field the link rate itself, as if it named the whole
 * clock, runs the port at twice the rate the sink was told to expect,
 * and the sink then never recovers a clock. */
static int link_rate_code(uint32_t link_rate_khz)
{
	switch (link_rate_khz / 2) {
	case 81000: return DPLL_CTRL1_LINK_RATE_810; /* 1.62 GHz */
	case 108000: return DPLL_CTRL1_LINK_RATE_1080; /* 2.16 GHz */
	case 135000: return DPLL_CTRL1_LINK_RATE_1350; /* 2.7 GHz */
	case 162000: return DPLL_CTRL1_LINK_RATE_1620; /* 3.24 GHz */
	case 216000: return DPLL_CTRL1_LINK_RATE_2160; /* 4.32 GHz */
	case 270000: return DPLL_CTRL1_LINK_RATE_2700; /* 5.4 GHz */
	default: return -1;
	}
}

/* The link rate a rate code stands for, in kHz (the inverse of the
 * above): twice the field's value. */
static uint32_t link_rate_of_code(uint32_t code)
{
	switch (code) {
	case DPLL_CTRL1_LINK_RATE_810: return 162000;
	case DPLL_CTRL1_LINK_RATE_1080: return 216000;
	case DPLL_CTRL1_LINK_RATE_1350: return 270000;
	case DPLL_CTRL1_LINK_RATE_1620: return 324000;
	case DPLL_CTRL1_LINK_RATE_2160: return 432000;
	case DPLL_CTRL1_LINK_RATE_2700: return 540000;
	default: return 0;
	}
}

/* The two VCOs DPLL0 runs at: 8640 MHz for the 2.16 and 4.32 GHz rates,
 * 8100 MHz for the others. */
static int rate_needs_vco_8640(uint32_t link_rate_khz)
{
	return link_rate_khz / 2 == 108000 || link_rate_khz / 2 == 216000;
}

int skl_dpll_rate_supported(uint32_t link_rate_khz)
{
	return link_rate_code(link_rate_khz) >= 0;
}

/* The WRPLL's reference clock in HDMI mode: CDCLK's (24 MHz on Skylake;
 * Gen11+ combo PLLs take theirs from DSSM, which the CDCLK code reads). */
static uint32_t skl_ref_khz(struct i915_device *i915)
{
	return i915->display.cdclk_ref_khz ? i915->display.cdclk_ref_khz : 24000;
}

int skl_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);

	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].ssc = 0;
		d->dpll[i].link_rate_khz = 0;
		d->dpll[i].cfgcr1 = d->dpll[i].cfgcr2 = 0;
	}
	/* DPLL0 is CDCLK's and always on; in_use counts the panels on it. */
	d->dpll[0].link_rate_khz =
		link_rate_of_code((ctrl1 & DPLL_CTRL1_LINK_RATE_MASK(0)) >> DPLL_CTRL1_LINK_RATE_SHIFT(0));
	i915_dbg("[drm] i915: DPLL_CTRL1 %08x, DPLL_CTRL2 %08x, status %08x\n",
		 ctrl1, i915_read32(i915, DPLL_CTRL2), i915_read32(i915, DPLL_STATUS));
	return 0;
}

/* A PLL's rate is programmed while it is STOPPED.  Writing the rate
 * field of a running PLL leaves it running at the old frequency, and its
 * lock bit -- never having dropped -- makes the wait below succeed at
 * once, so nothing says the port is clocked wrongly.  The firmware
 * leaves the panel's PLL running, so this is the usual case, not a
 * corner one. */
static void dpll_disable(struct i915_device *i915, int pll)
{
	uint32_t reg = dpll_ctl_reg(pll);
	uint32_t v = i915_read32(i915, reg);

	if (!(v & LCPLL_PLL_ENABLE))
		return;
	i915_write32(i915, reg, v & ~LCPLL_PLL_ENABLE);
	(void)i915_read32(i915, reg);
	for (int t = 0; t < 200; t++) {
		if (!(i915_read32(i915, DPLL_STATUS) & DPLL_LOCK(pll)))
			return;
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: DPLL%d still locked after being switched off\n", pll);
}

/* The PLL's six bits of DPLL_CTRL1: override, link rate, SSC, HDMI
 * mode.  Spread spectrum is never asked of these PLLs. */
static void write_ctrl1(struct i915_device *i915, int pll, uint32_t bits)
{
	uint32_t v = i915_read32(i915, DPLL_CTRL1);
	v &= ~(DPLL_CTRL1_HDMI_MODE(pll) | DPLL_CTRL1_SSC(pll) | DPLL_CTRL1_LINK_RATE_MASK(pll));
	v |= bits;
	i915_write32(i915, DPLL_CTRL1, v);
	(void)i915_read32(i915, DPLL_CTRL1);
}

/* DPLL1-3: CTRL1, the configuration words (zero for DisplayPort), on,
 * lock within 5 ms. */
static int dpll_enable(struct i915_device *i915, int pll, uint32_t ctrl1_bits, uint32_t cfgcr1,
		       uint32_t cfgcr2)
{
	uint32_t reg = dpll_ctl_reg(pll);

	dpll_disable(i915, pll);
	write_ctrl1(i915, pll, ctrl1_bits);
	i915_write32(i915, DPLL_CFGCR1(pll), cfgcr1);
	i915_write32(i915, DPLL_CFGCR2(pll), cfgcr2);
	(void)i915_read32(i915, DPLL_CFGCR1(pll));
	(void)i915_read32(i915, DPLL_CFGCR2(pll));
	i915_write32(i915, reg, i915_read32(i915, reg) | LCPLL_PLL_ENABLE);
	for (int t = 0; t < 500; t++) {
		if (i915_read32(i915, DPLL_STATUS) & DPLL_LOCK(pll))
			return 0;
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: DPLL%d did not lock\n", pll);
	return -ETIMEDOUT;
}

/* The embedded panel's output on a port, if that is what is there. */
static struct intel_output *edp_output(struct i915_device *i915, int port)
{
	for (int i = 0; i < i915->display.nout; i++)
		if (i915->display.outputs[i].port == port && i915->display.outputs[i].is_edp)
			return &i915->display.outputs[i];
	return NULL;
}

/* An embedded panel is clocked from DPLL0: the link-rate field is set
 * for the panel's rate while the PLL keeps running at CDCLK's VCO.  That
 * only works for a rate from the same VCO; the CDCLK code picked the
 * VCO, and a panel at a rate of the other one goes on a shared PLL
 * instead. */
static int skl_edp_on_dpll0(struct i915_device *i915, uint32_t link_rate_khz, int code)
{
	struct intel_display *d = &i915->display;
	uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);
	uint32_t cur = (ctrl1 & DPLL_CTRL1_LINK_RATE_MASK(0)) >> DPLL_CTRL1_LINK_RATE_SHIFT(0);
	int vco_8640 = cur == DPLL_CTRL1_LINK_RATE_1080 || cur == DPLL_CTRL1_LINK_RATE_2160;

	if (!(i915_read32(i915, LCPLL1_CTL) & LCPLL_PLL_ENABLE))
		return -ENODEV;
	if (vco_8640 != rate_needs_vco_8640(link_rate_khz)) {
		i915_dbg("[drm] i915: DPLL0 runs at a %s MHz VCO; the panel's %u kHz takes a shared PLL\n",
			 vco_8640 ? "8640" : "8100", link_rate_khz);
		return -ERANGE;
	}
	if (d->dpll[0].in_use && d->dpll[0].link_rate_khz != link_rate_khz)
		return -EBUSY;
	write_ctrl1(i915, 0, DPLL_CTRL1_OVERRIDE(0) | DPLL_CTRL1_LINK_RATE((uint32_t)code, 0));
	d->dpll[0].in_use++;
	d->dpll[0].is_hdmi = 0;
	d->dpll[0].link_rate_khz = link_rate_khz;
	return 0;
}

int skl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	struct intel_display *d = &i915->display;
	int code = link_rate_code(link_rate_khz);
	(void)ssc; /* these PLLs are not spread */

	if (code < 0)
		return -EINVAL;
	if (edp_output(i915, port) && skl_edp_on_dpll0(i915, link_rate_khz, code) == 0)
		return 0;
	/* a shared PLL already at this rate? */
	for (int i = 1; i < SKL_NUM_DPLLS; i++) {
		if (d->dpll[i].in_use && !d->dpll[i].is_hdmi &&
		    d->dpll[i].link_rate_khz == link_rate_khz) {
			d->dpll[i].in_use++;
			return i;
		}
	}
	for (int i = 1; i < SKL_NUM_DPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		if (dpll_enable(i915, i, DPLL_CTRL1_OVERRIDE(i) | DPLL_CTRL1_LINK_RATE((uint32_t)code, i),
				0, 0) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].ssc = 0;
		d->dpll[i].link_rate_khz = link_rate_khz;
		d->dpll[i].cfgcr1 = d->dpll[i].cfgcr2 = 0;
		i915_dbg("[drm] i915: DPLL%d at %u kHz for port %c\n", i, link_rate_khz, 'A' + port);
		return i;
	}
	return -ENOSPC;
}

/* Which PLL the port is routed to, and at what rate it runs. */
uint32_t skl_dpll_port_link_rate(struct i915_device *i915, int port)
{
	uint32_t ctrl2 = i915_read32(i915, DPLL_CTRL2);
	uint32_t ctrl1;
	uint32_t pll;

	if (!(ctrl2 & DPLL_CTRL2_DDI_SEL_OVERRIDE(port)) || (ctrl2 & DPLL_CTRL2_DDI_CLK_OFF(port)))
		return 0;
	pll = (ctrl2 & DPLL_CTRL2_DDI_CLK_SEL_MASK(port)) >> DPLL_CTRL2_DDI_CLK_SEL_SHIFT(port);
	if (pll >= SKL_NUM_DPLLS)
		return 0;
	if (!(i915_read32(i915, dpll_ctl_reg((int)pll)) & LCPLL_PLL_ENABLE))
		return 0;
	ctrl1 = i915_read32(i915, DPLL_CTRL1);
	if (ctrl1 & DPLL_CTRL1_HDMI_MODE(pll))
		return 0; /* dividers, not a link rate */
	return link_rate_of_code((ctrl1 & DPLL_CTRL1_LINK_RATE_MASK(pll)) >>
				 DPLL_CTRL1_LINK_RATE_SHIFT(pll));
}

/* HDMI: the WRPLL in HDMI mode, dividers from skl_wrpll_calc(). */
int skl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	struct skl_wrpll_params p;

	if (skl_wrpll_calc(clock_khz, skl_ref_khz(i915), &p) != 0) {
		kprintf("[drm] i915: no WRPLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	uint32_t cfgcr1 = DPLL_CFGCR1_FREQ_ENABLE | DPLL_CFGCR1_DCO_FRACTION(p.dco_fraction) |
			  p.dco_integer;
	uint32_t cfgcr2 = DPLL_CFGCR2_QDIV_RATIO(p.qdiv_ratio) | DPLL_CFGCR2_QDIV_MODE(p.qdiv_mode) |
			  DPLL_CFGCR2_KDIV(p.kdiv) | DPLL_CFGCR2_PDIV(p.pdiv) | p.central_freq;

	for (int i = 1; i < SKL_NUM_DPLLS; i++) {
		if (d->dpll[i].in_use && d->dpll[i].is_hdmi && d->dpll[i].cfgcr1 == cfgcr1 &&
		    d->dpll[i].cfgcr2 == cfgcr2) {
			d->dpll[i].in_use++;
			return i;
		}
	}
	for (int i = 1; i < SKL_NUM_DPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		if (dpll_enable(i915, i, DPLL_CTRL1_OVERRIDE(i) | DPLL_CTRL1_HDMI_MODE(i), cfgcr1,
				cfgcr2) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 1;
		d->dpll[i].ssc = 0;
		d->dpll[i].link_rate_khz = skl_wrpll_khz(cfgcr1, cfgcr2, skl_ref_khz(i915));
		d->dpll[i].cfgcr1 = cfgcr1;
		d->dpll[i].cfgcr2 = cfgcr2;
		i915_dbg("[drm] i915: DPLL%d in HDMI mode at %u kHz for port %c (%08x/%08x)\n", i,
			 d->dpll[i].link_rate_khz, 'A' + port, cfgcr1, cfgcr2);
		return i;
	}
	return -ENOSPC;
}

void skl_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;

	if (pll < 0 || pll >= SKL_NUM_DPLLS)
		return;
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
	/* DPLL0 keeps running for CDCLK */
	if (pll == 0 || d->dpll[pll].in_use)
		return;
	uint32_t reg = dpll_ctl_reg(pll);
	i915_write32(i915, reg, i915_read32(i915, reg) & ~LCPLL_PLL_ENABLE);
	(void)i915_read32(i915, reg);
}

void skl_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	uint32_t v = i915_read32(i915, DPLL_CTRL2);
	v &= ~(DPLL_CTRL2_DDI_CLK_OFF(port) | DPLL_CTRL2_DDI_CLK_SEL_MASK(port));
	v |= DPLL_CTRL2_DDI_CLK_SEL((uint32_t)pll, port) |
	     DPLL_CTRL2_DDI_SEL_OVERRIDE(port);
	i915_write32(i915, DPLL_CTRL2, v);
	(void)i915_read32(i915, DPLL_CTRL2);
}

void skl_dpll_unroute_port(struct i915_device *i915, int port)
{
	uint32_t v = i915_read32(i915, DPLL_CTRL2);
	v |= DPLL_CTRL2_DDI_CLK_OFF(port);
	i915_write32(i915, DPLL_CTRL2, v);
	(void)i915_read32(i915, DPLL_CTRL2);
}

/* ---- the model dispatcher ------------------------------------------------------ */
/*
 * The display code above is Skylake's.  The other generations have their
 * PLLs in intel_dpll_bxt.c (Broxton/Gemini Lake), intel_dpll_icl.c (the
 * combo ports of Ice Lake and later) and intel_dpll_hsw.c
 * (Haswell/Broadwell); a Type-C port's clock is intel_tc.c's, Meteor
 * Lake's and DG2's are the PHYs' own.  These entry points pick by the
 * display model so the pipe code (intel_display.c) calls one thing.
 */
#include <kernel/dev/gpu/i915/intel_snps_phy.h>

/* Meteor Lake, DG2: every port has its PHY's own PLL, found through the
 * output on the port. */
static struct intel_output *port_output(struct i915_device *i915, int port)
{
	for (int i = 0; i < i915->display.nout; i++)
		if (i915->display.outputs[i].port == port)
			return &i915->display.outputs[i];
	return NULL;
}

static int is_icl_tgl(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL;
}

/* Haswell ULX parts stop at 2.7 GHz. */
static int is_hsw_ulx(struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_HASWELL &&
	       (i915->devid == 0x0A0E || i915->devid == 0x0A1E);
}

int intel_dpll_rate_supported(struct i915_device *i915, uint32_t link_rate_khz)
{
	switch (i915->display.model) {
	case INTEL_DISPLAY_MTL:
		/* the C10 ports' list; the C20 ones are a superset */
		return mtl_phy_rate_supported(i915, PORT_A, link_rate_khz);
	case INTEL_DISPLAY_DG2:
		return dg2_phy_rate_supported(i915, PORT_A, link_rate_khz);
	case INTEL_DISPLAY_BDW:
		if (is_hsw_ulx(i915) && link_rate_khz > 270000)
			return 0;
		return hsw_dpll_rate_supported(link_rate_khz);
	case INTEL_DISPLAY_BXT: return bxt_dpll_rate_supported(link_rate_khz);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_rate_supported(link_rate_khz);
	default: return skl_dpll_rate_supported(link_rate_khz);
	}
}

/* The fastest DisplayPort link a port of these generations takes: Ice
 * and Tiger Lake drive 8.1 GHz from a combo PHY only for an embedded
 * panel, Elkhart/Jasper Lake only for an external sink; Rocket Lake,
 * Alder Lake and DG1 everywhere. */
static uint32_t icl_max_link_rate(struct i915_device *i915, const struct intel_output *o)
{
	int platform = i915->info->platform;

	if (platform == I915_PLATFORM_ROCKETLAKE || platform == I915_PLATFORM_ALDERLAKE_S ||
	    platform == I915_PLATFORM_ALDERLAKE_P || platform == I915_PLATFORM_DG1)
		return 810000;
	if (platform == I915_PLATFORM_ELKHARTLAKE || platform == I915_PLATFORM_JASPERLAKE)
		return o->is_edp ? 540000 : 810000;
	if (!o->is_tc && !o->is_edp)
		return 540000;
	return 810000;
}

int intel_dpll_output_rate_supported(struct i915_device *i915, const struct intel_output *o,
				     uint32_t link_rate_khz)
{
	if (!o)
		return intel_dpll_rate_supported(i915, link_rate_khz);
	switch (i915->display.model) {
	case INTEL_DISPLAY_MTL:
		return mtl_phy_rate_supported(i915, o->port, link_rate_khz);
	case INTEL_DISPLAY_DG2:
		return dg2_phy_rate_supported(i915, o->port, link_rate_khz);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
		if (link_rate_khz > icl_max_link_rate(i915, o))
			return 0;
		/* A Type-C port's own PLL keeps its DCO at 8.1 GHz for
		 * DisplayPort, and the Thunderbolt PLL has four outputs:
		 * the four standard rates, no intermediate ones. */
		if (o->is_tc)
			return link_rate_khz == 162000 || link_rate_khz == 270000 ||
			       link_rate_khz == 540000 || link_rate_khz == 810000;
		return icl_dpll_rate_supported(link_rate_khz);
	default:
		return intel_dpll_rate_supported(i915, link_rate_khz);
	}
}

/* The PHY's code for a link rate in DDI_BUF_CTL (Alder Lake-P). */
static uint32_t ddi_buf_phy_link_rate(uint32_t link_rate_khz)
{
	switch (link_rate_khz) {
	case 216000: return GEN11_DDI_BUF_PHY_LINK_RATE(4);
	case 243000: return GEN11_DDI_BUF_PHY_LINK_RATE(5);
	case 270000: return GEN11_DDI_BUF_PHY_LINK_RATE(1);
	case 324000: return GEN11_DDI_BUF_PHY_LINK_RATE(6);
	case 432000: return GEN11_DDI_BUF_PHY_LINK_RATE(7);
	case 540000: return GEN11_DDI_BUF_PHY_LINK_RATE(2);
	case 810000: return GEN11_DDI_BUF_PHY_LINK_RATE(3);
	default: return GEN11_DDI_BUF_PHY_LINK_RATE(0); /* 1.62 GHz */
	}
}

uint32_t intel_dpll_ddi_buf_ctl_bits(struct i915_device *i915, const struct intel_output *o,
				     uint32_t *mask)
{
	uint32_t v = 0;

	*mask = 0;
	if (!o->is_tc || i915->info->display_ver < 11 || i915->info->display_ver > 13 ||
	    !is_icl_tgl(i915))
		return 0;
	if (i915->info->platform == I915_PLATFORM_ALDERLAKE_P) {
		*mask |= GEN11_DDI_BUF_PHY_LINK_RATE_MASK;
		v |= ddi_buf_phy_link_rate(o->link_rate_khz);
		/* the PHY is the display's, outside Thunderbolt mode */
		if (o->tc_mode != INTEL_TC_TBT_ALT)
			v |= GEN11_DDI_BUF_CTL_TC_PHY_OWNERSHIP;
	}
	/* The lane enables are staggered by at least 100 ns: the link
	 * symbols (10 bits each) in 100 ns at this rate, rounded up. */
	*mask |= GEN11_DDI_BUF_LANE_STAGGER_DELAY_MASK;
	v |= GEN11_DDI_BUF_LANE_STAGGER_DELAY((o->link_rate_khz + 10 * 1000 - 1) / (10 * 1000));
	return v;
}

/* ---- HDMI: what the PLLs and PHYs can carry -------------------------------------- */

/* The source's TMDS ceiling by generation: HDMI 2.0 rates (594 MHz, and
 * the 600 MHz of HDMI 2.0's top character rate from display version 13
 * and on Alder Lake-S) from version 10 on -- Gemini Lake included,
 * Broxton not --, 300 MHz from Haswell to Skylake.  A board can lower it
 * per port (the VBT's HDMI data rate, intel_hdmi_source_max_tmds_khz()). */
uint32_t intel_dpll_hdmi_max_tmds_khz(struct i915_device *i915)
{
	int ver = i915->info->display_ver;

	if (ver >= 13 || i915->info->platform == I915_PLATFORM_ALDERLAKE_S)
		return 600000;
	if (ver >= 10)
		return 594000;
	if (ver >= 8 || i915->info->platform == I915_PLATFORM_HASWELL)
		return 300000; /* Haswell to Skylake and Broxton */
	if (ver >= 5)
		return 225000;
	return 165000;
}

int intel_dpll_hdmi_clock_valid(struct i915_device *i915, const struct intel_output *o,
				uint32_t clock_khz)
{
	int platform = i915->info->platform;

	if (clock_khz < 25000 || clock_khz > intel_dpll_hdmi_max_tmds_khz(i915))
		return -ERANGE;
	/* the frequencies each PLL cannot synthesise */
	if (platform == I915_PLATFORM_GEMINILAKE && clock_khz > 446666 && clock_khz < 480000)
		return -ERANGE;
	if ((platform == I915_PLATFORM_GEMINILAKE || platform == I915_PLATFORM_BROXTON) &&
	    clock_khz > 223333 && clock_khz < 240000)
		return -ERANGE;
	/* the combo PHYs' PLLs (Ice Lake to Alder Lake), and the Type-C
	 * PHYs' from Ice Lake on (Meteor Lake's included) */
	if (is_icl_tgl(i915) && o && !o->is_tc && clock_khz > 500000 && clock_khz < 533200)
		return -ERANGE;
	if (o && o->is_tc && clock_khz > 500000 && clock_khz < 532800)
		return -ERANGE;
	/* and the divider search has to come up with something */
	switch (i915->display.model) {
	case INTEL_DISPLAY_SKL: {
		struct skl_wrpll_params p;
		return skl_wrpll_calc(clock_khz, skl_ref_khz(i915), &p) ? -ERANGE : 0;
	}
	case INTEL_DISPLAY_BXT: {
		struct bxt_pll_dividers b;
		return bxt_hdmi_dividers(clock_khz, &b) ? -ERANGE : 0;
	}
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: {
		struct icl_pll_params p;
		if (o && o->is_tc)
			return intel_tc_pll_clock_ok(i915, clock_khz, 0) ? 0 : -ERANGE;
		return icl_combo_hdmi_params(clock_khz, skl_ref_khz(i915), &p) ? -ERANGE : 0;
	}
	/* the port PHYs' own PLLs: asked without taking them */
	case INTEL_DISPLAY_MTL:
		return mtl_phy_hdmi_clock_ok(i915, o, clock_khz) ? 0 : -ERANGE;
	case INTEL_DISPLAY_DG2:
		return dg2_phy_hdmi_clock_ok(i915, o, clock_khz) ? 0 : -ERANGE;
	default:
		return 0;
	}
}

int intel_dpll_init(struct i915_device *i915)
{
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_init(i915);
	case INTEL_DISPLAY_BXT: return bxt_dpll_init(i915);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_init(i915);
	case INTEL_DISPLAY_MTL: return 0; /* the PHYs: mtl_phy_init() */
	case INTEL_DISPLAY_DG2: return 0; /* the PHYs: dg2_phy_init() */
	default: return skl_dpll_init(i915);
	}
}

/* The Type-C output on a port, if the port is one (Ice Lake and later). */
static struct intel_output *tc_output(struct i915_device *i915, int port)
{
	for (int i = 0; i < i915->display.nout; i++)
		if (i915->display.outputs[i].port == port && i915->display.outputs[i].is_tc)
			return &i915->display.outputs[i];
	return NULL;
}

/* A PLL id handed out by intel_tc.c: the Thunderbolt PLL, or a Type-C
 * port's own (slots after the combo PLLs; only parts with Type-C PHYs
 * have them -- on the others those slots are combo PLLs). */
static int is_tc_pll(struct i915_device *i915, int pll)
{
	if (!(i915->info->flags & I915_INFO_HAS_TC_PHY))
		return 0;
	return pll == INTEL_TBT_PLL_ID || (pll >= INTEL_TC_PLL_BASE && pll < INTEL_MAX_DPLLS);
}

int intel_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		struct intel_output *po = port_output(i915, port);
		return po ? mtl_phy_pll_get_dp(i915, po, link_rate_khz, ssc) : -ENODEV;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		struct intel_output *po = port_output(i915, port);
		return po ? dg2_phy_pll_get_dp(i915, po, link_rate_khz, ssc) : -ENODEV;
	}
	struct intel_output *o = tc_output(i915, port);
	if (o)
		return intel_tc_dpll_get_dp(i915, o, link_rate_khz, ssc);
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_get_dp(i915, port, link_rate_khz, ssc);
	case INTEL_DISPLAY_BXT: return bxt_dpll_get_dp(i915, port, link_rate_khz, ssc);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_get_dp(i915, port, link_rate_khz, ssc);
	default: return skl_dpll_get_dp(i915, port, link_rate_khz, ssc);
	}
}

int intel_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		struct intel_output *po = port_output(i915, port);
		return po ? mtl_phy_pll_get_hdmi(i915, po, clock_khz) : -ENODEV;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		struct intel_output *po = port_output(i915, port);
		return po ? dg2_phy_pll_get_hdmi(i915, po, clock_khz) : -ENODEV;
	}
	struct intel_output *o = tc_output(i915, port);
	if (o)
		return intel_tc_dpll_get_hdmi(i915, o, clock_khz);
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_get_hdmi(i915, port, clock_khz);
	case INTEL_DISPLAY_BXT: return bxt_dpll_get_hdmi(i915, port, clock_khz);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_get_hdmi(i915, port, clock_khz);
	default: return skl_dpll_get_hdmi(i915, port, clock_khz);
	}
}

void intel_dpll_put(struct i915_device *i915, int pll)
{
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		mtl_phy_pll_put(i915, pll);
		return;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		dg2_phy_pll_put(i915, pll);
		return;
	}
	if (is_icl_tgl(i915) && is_tc_pll(i915, pll)) {
		intel_tc_dpll_put(i915, pll);
		return;
	}
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: hsw_dpll_put(i915, pll); break;
	case INTEL_DISPLAY_BXT: bxt_dpll_put(i915, pll); break;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: icl_dpll_put(i915, pll); break;
	default: skl_dpll_put(i915, pll); break;
	}
}

void intel_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	if (i915->display.model == INTEL_DISPLAY_MTL || i915->display.model == INTEL_DISPLAY_DG2) {
		/* enabling the PHY's PLL is what clocks the port */
		struct intel_output *po = port_output(i915, port);
		int lanes = 4;
		(void)pll;
		if (!po)
			return;
		if (po->type == INTEL_OUTPUT_DP || po->type == INTEL_OUTPUT_EDP)
			lanes = po->lane_count ? po->lane_count : 4;
		if ((i915->display.model == INTEL_DISPLAY_MTL ? mtl_phy_pll_enable(i915, po, lanes) :
								dg2_phy_pll_enable(i915, po, lanes)))
			kprintf("[drm] i915: port %s: the PHY's PLL did not lock\n",
				intel_port_name(i915, port));
		return;
	}
	struct intel_output *o = tc_output(i915, port);
	if (o) {
		intel_tc_route_port(i915, o, pll);
		return;
	}
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: hsw_dpll_route_port(i915, port, pll); break;
	case INTEL_DISPLAY_BXT: bxt_dpll_route_port(i915, port, pll); break;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: icl_dpll_route_port(i915, port, pll); break;
	default: skl_dpll_route_port(i915, port, pll); break;
	}
}

void intel_dpll_unroute_port(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_MTL || i915->display.model == INTEL_DISPLAY_DG2) {
		struct intel_output *po = port_output(i915, port);
		if (po && i915->display.model == INTEL_DISPLAY_MTL)
			mtl_phy_pll_disable(i915, po);
		else if (po)
			dg2_phy_pll_disable(i915, po);
		return;
	}
	struct intel_output *o = tc_output(i915, port);
	if (o) {
		intel_tc_unroute_port(i915, o);
		return;
	}
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: hsw_dpll_unroute_port(i915, port); break;
	case INTEL_DISPLAY_BXT: bxt_dpll_unroute_port(i915, port); break;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: icl_dpll_unroute_port(i915, port); break;
	default: skl_dpll_unroute_port(i915, port); break;
	}
}

uint32_t intel_dpll_port_link_rate(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		struct intel_output *po = port_output(i915, port);
		return po ? mtl_phy_port_link_rate(i915, po) : 0;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		struct intel_output *po = port_output(i915, port);
		return po ? dg2_phy_port_link_rate(i915, po) : 0;
	}
	struct intel_output *o = tc_output(i915, port);
	if (o)
		return intel_tc_port_link_rate(i915, o);
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_port_link_rate(i915, port);
	case INTEL_DISPLAY_BXT: return bxt_dpll_port_link_rate(i915, port);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_port_link_rate(i915, port);
	default: return skl_dpll_port_link_rate(i915, port);
	}
}
