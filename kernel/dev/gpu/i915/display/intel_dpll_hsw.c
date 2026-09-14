// LikeOS-64 -- the port clocks of Haswell and Broadwell.
//
// The LCPLL runs at 2.7 GHz (and drives CDCLK too); a DisplayPort port is
// clocked straight from it at 2.7, 1.35 or 0.81 GHz through its clock
// select, and needs no PLL of its own.  HDMI takes one of the two WRPLLs,
// with a reference divider, a feedback divider and a post divider found
// by search.  The buffer translations are a table per output kind
// written into the DDI like Skylake's.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

#define HSW_LCPLL_KHZ 2700000
#define HSW_NUM_WRPLLS 2

int hsw_dpll_rate_supported(uint32_t link_rate_khz)
{
	return link_rate_khz == 162000 || link_rate_khz == 270000 || link_rate_khz == 540000;
}

int hsw_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < INTEL_MAX_DPLLS; i++)
		d->dpll[i].in_use = 0;
	i915_dbg("[drm] i915: LCPLL %08x, WRPLL1 %08x, WRPLL2 %08x, SPLL %08x\n",
		 i915_read32(i915, LCPLL_CTL), i915_read32(i915, HSW_WRPLL_CTL(0)),
		 i915_read32(i915, HSW_WRPLL_CTL(1)), i915_read32(i915, SPLL_CTL));
	return 0;
}

/* DisplayPort: the LCPLL itself; the "PLL id" encodes the rate for the
 * clock select (10 + rate code so it cannot collide with a WRPLL). */
int hsw_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	(void)i915; (void)port; (void)ssc;
	switch (link_rate_khz) {
	case 540000: return 10;
	case 270000: return 11;
	case 162000: return 12;
	default: return -EINVAL;
	}
}

/* HDMI: WRPLL dividers.  out = LCPLL * n2 / (r2 * p), with the VCO
 * (LCPLL * n2 / r2) between 2.4 and 4.8 GHz; the search is over the
 * reference and post dividers. */
static int wrpll_calc(uint32_t clock_khz, uint32_t *r2, uint32_t *n2, uint32_t *p)
{
	uint64_t best_err = ~0ULL;
	int found = 0;
	for (uint32_t pp = 2; pp <= 64; pp += 2) {
		for (uint32_t rr = 1; rr <= 40; rr++) {
			uint64_t n = ((uint64_t)clock_khz * pp * rr + HSW_LCPLL_KHZ / 2) / HSW_LCPLL_KHZ;
			if (n < 1 || n > 255)
				continue;
			uint64_t vco = (uint64_t)HSW_LCPLL_KHZ * n / rr;
			if (vco < 2400000 || vco > 4800000)
				continue;
			uint64_t got = vco / pp;
			uint64_t err = got > clock_khz ? got - clock_khz : clock_khz - got;
			if (err < best_err) {
				best_err = err;
				*r2 = rr;
				*n2 = (uint32_t)n;
				*p = pp;
				found = 1;
			}
		}
	}
	return found ? 0 : -1;
}

int hsw_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	uint32_t r2, n2, p;
	(void)port;
	if (wrpll_calc(clock_khz, &r2, &n2, &p)) {
		kprintf("[drm] i915: no WRPLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	for (int i = 0; i < HSW_NUM_WRPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		uint32_t reg = HSW_WRPLL_CTL(i);
		i915_write32(i915, reg, i915_read32(i915, reg) & ~WRPLL_PLL_ENABLE);
		lapic_delay_us(20);
		uint32_t v = WRPLL_PLL_ENABLE | WRPLL_REF_LCPLL | WRPLL_DIVIDER_REFERENCE(r2) |
			     WRPLL_DIVIDER_POST(p) | WRPLL_DIVIDER_FEEDBACK(n2);
		i915_write32(i915, reg, v);
		(void)i915_read32(i915, reg);
		lapic_delay_us(20); /* the lock time */
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 1;
		d->dpll[i].link_rate_khz = clock_khz;
		return i;
	}
	return -ENOSPC;
}

void hsw_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (pll < 0 || pll >= HSW_NUM_WRPLLS)
		return;
	d->dpll[pll].in_use = 0;
	uint32_t reg = HSW_WRPLL_CTL(pll);
	i915_write32(i915, reg, i915_read32(i915, reg) & ~WRPLL_PLL_ENABLE);
}

void hsw_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	uint32_t sel;
	switch (pll) {
	case 10: sel = PORT_CLK_SEL_LCPLL_2700; break;
	case 11: sel = PORT_CLK_SEL_LCPLL_1350; break;
	case 12: sel = PORT_CLK_SEL_LCPLL_810; break;
	case 0:
	case 1: sel = PORT_CLK_SEL_WRPLL((uint32_t)pll); break;
	default: sel = PORT_CLK_SEL_NONE; break;
	}
	i915_write32(i915, PORT_CLK_SEL(port), sel);
	(void)i915_read32(i915, PORT_CLK_SEL(port));
}

void hsw_dpll_unroute_port(struct i915_device *i915, int port)
{
	i915_write32(i915, PORT_CLK_SEL(port), PORT_CLK_SEL_NONE);
	(void)i915_read32(i915, PORT_CLK_SEL(port));
}

uint32_t hsw_dpll_port_link_rate(struct i915_device *i915, int port)
{
	switch (i915_read32(i915, PORT_CLK_SEL(port)) & PORT_CLK_SEL_MASK) {
	case PORT_CLK_SEL_LCPLL_2700: return 540000;
	case PORT_CLK_SEL_LCPLL_1350: return 270000;
	case PORT_CLK_SEL_LCPLL_810: return 162000;
	default: return 0;
	}
}

/* ---- Broadwell's DDI translations ----------------------------------------------- */

struct buf_trans {
	uint32_t lo, hi;
};

static const struct buf_trans bdw_dp[] = {
	{ 0x00FFFFFF, 0x0006000E }, { 0x00D75FFF, 0x0005000A }, { 0x00C30FFF, 0x00040006 },
	{ 0x80AAAFFF, 0x000B0000 }, { 0x00FFFFFF, 0x0005000A }, { 0x00D75FFF, 0x000C0004 },
	{ 0x80C30FFF, 0x000B0000 }, { 0x00FFFFFF, 0x00040006 }, { 0x80D75FFF, 0x000B0000 },
};
static const struct buf_trans bdw_edp[] = {
	{ 0x00FFFFFF, 0x00000012 }, { 0x00EBAFFF, 0x00020011 }, { 0x00C71FFF, 0x0006000F },
	{ 0x00AAAFFF, 0x000E000A }, { 0x00FFFFFF, 0x00020011 }, { 0x00DB6FFF, 0x0005000F },
	{ 0x00BEEFFF, 0x000A000C }, { 0x00FFFFFF, 0x0005000F }, { 0x00DB6FFF, 0x000A000C },
	{ 0x00FFFFFF, 0x000A000C },
};
static const struct buf_trans bdw_hdmi[] = {
	{ 0x00FFFFFF, 0x0007000E }, { 0x00D75FFF, 0x000E000A }, { 0x00BEFFFF, 0x00140006 },
	{ 0x80B2CFFF, 0x001B0002 }, { 0x00FFFFFF, 0x000E000A }, { 0x00DB6FFF, 0x00160005 },
	{ 0x80C71FFF, 0x001A0002 }, { 0x00F31FFF, 0x00140006 }, { 0x80D75FFF, 0x001A0002 },
	{ 0x80FFFFFF, 0x001B0002 },
};
#define BDW_HDMI_DEFAULT_LEVEL 7

void bdw_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o, int level)
{
	const struct buf_trans *t;
	unsigned n;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) {
		t = bdw_hdmi;
		n = sizeof(bdw_hdmi) / sizeof(bdw_hdmi[0]);
		if (level < 0 || (unsigned)level >= n)
			level = BDW_HDMI_DEFAULT_LEVEL;
		/* the HDMI level goes into entry 9, which the port then uses */
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, 9), t[level].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, 9), t[level].hi);
		return;
	}
	if (o->is_edp) {
		t = bdw_edp;
		n = sizeof(bdw_edp) / sizeof(bdw_edp[0]);
	} else {
		t = bdw_dp;
		n = sizeof(bdw_dp) / sizeof(bdw_dp[0]);
	}
	for (unsigned i = 0; i < n && i < 10; i++) {
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, (int)i), t[i].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, (int)i), t[i].hi);
	}
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	v &= ~DDI_BUF_EMP_MASK;
	v |= DDI_BUF_TRANS_SELECT((uint32_t)level);
	i915_write32(i915, DDI_BUF_CTL(o->port), v);
}
