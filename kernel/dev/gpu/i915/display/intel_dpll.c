// LikeOS -- the display PLLs of Skylake.
//
// Four of them.  DPLL0 belongs to CDCLK (the firmware programmed it and
// it is not touched); DPLL1-3 are shared between ports.  For DisplayPort a
// PLL is programmed with a link rate from a fixed list; for HDMI with
// dividers computed from the pixel clock.  A port is then routed to its
// PLL through DPLL_CTRL2.  PLLs are refcounted so two ports at the same
// DP link rate share one.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

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

int skl_dpll_rate_supported(uint32_t link_rate_khz)
{
	return link_rate_code(link_rate_khz) >= 0;
}

int skl_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);

	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].link_rate_khz = 0;
	}
	/* DPLL0 is CDCLK's: reserved.  Its rate field is the CDCLK source
	 * frequency; as a link rate it is twice that. */
	d->dpll[0].in_use = 1;
	d->dpll[0].link_rate_khz = d->dpll0_link_rate_khz * 2;
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

static int dpll_enable_wait(struct i915_device *i915, int pll)
{
	uint32_t reg = dpll_ctl_reg(pll);
	i915_write32(i915, reg, i915_read32(i915, reg) | LCPLL_PLL_ENABLE);
	for (int t = 0; t < 500; t++) {
		if (i915_read32(i915, DPLL_STATUS) & DPLL_LOCK(pll))
			return 0;
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: DPLL%d did not lock\n", pll);
	return -ETIMEDOUT;
}

int skl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz,
		     int ssc)
{
	struct intel_display *d = &i915->display;
	int code = link_rate_code(link_rate_khz);
	(void)port;

	if (code < 0)
		return -EINVAL;
	/* DPLL0 serves a panel too when CDCLK's source happens to be this
	 * link rate -- its rate field is half the link, like every other. */
	if (d->dpll[0].link_rate_khz == link_rate_khz && !ssc) {
		d->dpll[0].in_use++;
		return 0;
	}
	/* A shared PLL already at this rate? */
	for (int i = 1; i < INTEL_MAX_DPLLS; i++) {
		if (d->dpll[i].in_use && !d->dpll[i].is_hdmi &&
		    d->dpll[i].link_rate_khz == link_rate_khz &&
		    d->dpll[i].ssc == ssc) {
			d->dpll[i].in_use++;
			return i;
		}
	}
	for (int i = 1; i < INTEL_MAX_DPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		dpll_disable(i915, i);
		uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);
		ctrl1 &= ~(DPLL_CTRL1_HDMI_MODE(i) | DPLL_CTRL1_SSC(i) |
			   DPLL_CTRL1_LINK_RATE_MASK(i));
		ctrl1 |= DPLL_CTRL1_OVERRIDE(i) |
			 DPLL_CTRL1_LINK_RATE((uint32_t)code, i);
		if (ssc)
			ctrl1 |= DPLL_CTRL1_SSC(i);
		i915_write32(i915, DPLL_CTRL1, ctrl1);
		(void)i915_read32(i915, DPLL_CTRL1);
		if (dpll_enable_wait(i915, i) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].ssc = ssc;
		d->dpll[i].link_rate_khz = link_rate_khz;
		i915_dbg("[drm] i915: DPLL%d at %u kHz%s for port %c\n", i, link_rate_khz,
			ssc ? " with spread spectrum" : "", 'A' + port);
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

	if (ctrl2 & DPLL_CTRL2_DDI_CLK_OFF(port))
		return 0;
	pll = (ctrl2 & DPLL_CTRL2_DDI_CLK_SEL_MASK(port)) >>
	      DPLL_CTRL2_DDI_CLK_SEL_SHIFT(port);
	if (pll >= INTEL_MAX_DPLLS)
		return 0;
	if (!(i915_read32(i915, DPLL_STATUS) & DPLL_LOCK((int)pll)))
		return 0;
	ctrl1 = i915_read32(i915, DPLL_CTRL1);
	if (ctrl1 & DPLL_CTRL1_HDMI_MODE(pll))
		return 0; /* dividers, not a link rate */
	return link_rate_of_code((ctrl1 & DPLL_CTRL1_LINK_RATE_MASK(pll)) >>
				 DPLL_CTRL1_LINK_RATE_SHIFT(pll));
}

/* HDMI: the WRPLL divider search.  The DCO must land between 8400 and
 * 9600 MHz with one of three central frequencies; the AFE clock (5x the
 * pixel clock) is the DCO over p0*p1*p2.  Dividers as the hardware
 * accepts them: p0 in {1,2,3,7}, p2 in {1,2,3,5}, p1 = 1..255, product
 * even from 4 to 98 (or 1, 2, 3, 5, 7 with p1 = 1). */
static const uint32_t dco_central[3] = { 8400000, 9000000, 9600000 };

struct wrpll_params {
	uint32_t dco_freq; /* kHz */
	uint32_t central;
	uint32_t p0, p1, p2;
	uint64_t deviation;
};

static int split_divider(uint32_t div, uint32_t *p0, uint32_t *p1, uint32_t *p2)
{
	static const uint32_t even[] = { 4, 6, 8, 10, 12, 14, 16, 18, 20,
					 24, 28, 30, 32, 36, 40, 42, 44,
					 48, 52, 54, 56, 60, 64, 66, 68,
					 70, 72, 76, 78, 80, 84, 88, 90,
					 92, 96, 98 };
	(void)even;
	if (div == 3) { *p0 = 3; *p1 = 1; *p2 = 1; return 1; }
	if (div == 5) { *p0 = 5 ; *p1 = 1; *p2 = 1; return 1; }
	if (div == 7) { *p0 = 7; *p1 = 1; *p2 = 1; return 1; }
	if (div == 1 || div == 2)
		return 0;
	if (div % 2)
		return 0;
	uint32_t half = div / 2;
	if (half == 1) { *p0 = 2; *p1 = 1; *p2 = 1; return 1; }
	if (half == 2) { *p0 = 2; *p1 = 1; *p2 = 2; return 1; }
	if (half == 3) { *p0 = 3; *p1 = 1; *p2 = 2; return 1; }
	if (half == 5) { *p0 = 5; *p1 = 1; *p2 = 2; return 1; }
	if (half == 7) { *p0 = 7; *p1 = 1; *p2 = 2; return 1; }
	if (half % 2 == 0) { *p0 = 2; *p1 = half / 2; *p2 = 2; return 1; }
	if (half % 3 == 0) { *p0 = 3; *p1 = half / 3; *p2 = 2; return 1; }
	if (half % 7 == 0) { *p0 = 7; *p1 = half / 7; *p2 = 2; return 1; }
	return 0;
}

static int wrpll_compute(uint32_t clock_khz, struct wrpll_params *best)
{
	uint32_t afe = clock_khz * 5;
	static const uint32_t dividers[] = { 3, 5, 7, 4, 6, 8, 10, 12, 14, 16, 18,
					     20, 24, 28, 30, 32, 36, 40, 42, 44,
					     48, 52, 54, 56, 60, 64, 66, 68, 70,
					     72, 76, 78, 80, 84, 88, 90, 92, 96,
					     98 };
	best->deviation = ~0ULL;
	for (unsigned c = 0; c < 3; c++) {
		for (unsigned i = 0; i < sizeof(dividers) / sizeof(dividers[0]); i++) {
			uint32_t div = dividers[i];
			uint32_t p0, p1, p2;
			uint64_t dco = (uint64_t)afe * div;
			if (dco < 8400000 || dco > 9600000)
				continue;
			if (!split_divider(div, &p0, &p1, &p2))
				continue;
			uint64_t dev = dco > dco_central[c] ? dco - dco_central[c] :
							       dco_central[c] - dco;
			/* allowed: +1% / -6% of the central frequency */
			if (dco > dco_central[c] && dev * 100 > dco_central[c])
				continue;
			if (dco < dco_central[c] && dev * 100 > (uint64_t)dco_central[c] * 6)
				continue;
			if (dev < best->deviation) {
				best->deviation = dev;
				best->dco_freq = (uint32_t)dco;
				best->central = c;
				best->p0 = p0;
				best->p1 = p1;
				best->p2 = p2;
			}
		}
	}
	return best->deviation == ~0ULL ? -1 : 0;
}

int skl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	struct wrpll_params p;
	(void)port;

	if (wrpll_compute(clock_khz, &p) != 0) {
		kprintf("[drm] i915: no WRPLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	for (int i = 1; i < INTEL_MAX_DPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		dpll_disable(i915, i);
		/* DCO integer and fractional parts of dco/24MHz */
		uint32_t dco_int = p.dco_freq / 24000;
		uint32_t dco_frac = (uint32_t)(((uint64_t)(p.dco_freq % 24000) << 15) / 24000);
		uint32_t cfgcr1 = DPLL_CFGCR1_FREQ_ENABLE |
				  DPLL_CFGCR1_DCO_FRACTION(dco_frac) | dco_int;
		uint32_t pdiv, kdiv, qdiv_mode = 0, qdiv = 1;
		switch (p.p0) {
		case 1: pdiv = 0; break;
		case 2: pdiv = 1; break;
		case 3: pdiv = 2; break;
		case 7: pdiv = 4; break;
		default: pdiv = 1; break;
		}
		switch (p.p2) {
		case 5: kdiv = 0; break;
		case 2: kdiv = 1; break;
		case 3: kdiv = 2; break;
		default: kdiv = 3; break;
		}
		if (p.p1 != 1) {
			qdiv_mode = 1;
			qdiv = p.p1;
		}
		uint32_t cfgcr2 = DPLL_CFGCR2_QDIV_RATIO(qdiv) |
				  DPLL_CFGCR2_QDIV_MODE(qdiv_mode) |
				  DPLL_CFGCR2_KDIV(kdiv) | DPLL_CFGCR2_PDIV(pdiv) |
				  p.central;
		uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);
		ctrl1 &= ~(DPLL_CTRL1_SSC(i) | DPLL_CTRL1_LINK_RATE_MASK(i));
		ctrl1 |= DPLL_CTRL1_OVERRIDE(i) | DPLL_CTRL1_HDMI_MODE(i);
		i915_write32(i915, DPLL_CTRL1, ctrl1);
		i915_write32(i915, DPLL_CFGCR1(i), cfgcr1);
		i915_write32(i915, DPLL_CFGCR2(i), cfgcr2);
		(void)i915_read32(i915, DPLL_CFGCR2(i));
		if (dpll_enable_wait(i915, i) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 1;
		d->dpll[i].ssc = 0;
		d->dpll[i].link_rate_khz = clock_khz;
		d->dpll[i].cfgcr1 = cfgcr1;
		d->dpll[i].cfgcr2 = cfgcr2;
		return i;
	}
	return -ENOSPC;
}

void skl_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;

	if (pll < 0 || pll >= INTEL_MAX_DPLLS)
		return;
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
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
 * PLLs in intel_dpll_bxt.c (Broxton/Gemini Lake), intel_dpll_icl.c (Ice
 * Lake and Tiger Lake's combo ports) and intel_dpll_hsw.c
 * (Haswell/Broadwell); a Type-C port's clock is intel_tc.c's.  These
 * entry points pick by the display model so the pipe code
 * (intel_display.c) calls one thing.
 */
#include <kernel/dev/gpu/i915/intel_display.h>

int intel_dpll_rate_supported(struct i915_device *i915, uint32_t link_rate_khz)
{
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_rate_supported(link_rate_khz);
	case INTEL_DISPLAY_BXT: return bxt_dpll_rate_supported(link_rate_khz);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_rate_supported(link_rate_khz);
	default: return skl_dpll_rate_supported(link_rate_khz);
	}
}

int intel_dpll_init(struct i915_device *i915)
{
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW: return hsw_dpll_init(i915);
	case INTEL_DISPLAY_BXT: return bxt_dpll_init(i915);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_dpll_init(i915);
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

static int is_tc_pll(int pll)
{
	return pll == INTEL_TBT_PLL_ID || (pll >= INTEL_TC_PLL_BASE && pll < INTEL_MAX_DPLLS);
}

int intel_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
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
	if ((i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL) &&
	    is_tc_pll(pll)) {
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
