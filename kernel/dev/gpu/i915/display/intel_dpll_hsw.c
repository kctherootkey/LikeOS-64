// LikeOS -- the port clocks of Haswell and Broadwell.
//
// The LCPLL runs at 2.7 GHz (and drives CDCLK too); a DisplayPort port is
// clocked straight from it at 2.7, 1.35 or 0.81 GHz through its clock
// select, and needs no PLL of its own.  HDMI takes one of the two WRPLLs,
// with a reference divider, a feedback divider and a post divider found
// by search (intel_dpll_calc.c), the LCPLL as reference.  Two HDMI ports
// at the same clock share a WRPLL.  The buffer translations are a table
// per output kind written into the DDI like Skylake's.
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

#define HSW_NUM_WRPLLS 2
/* the "PLL ids" of the three LCPLL outputs, apart from the WRPLLs' */
#define HSW_LCPLL_2700_ID 10
#define HSW_LCPLL_1350_ID 11
#define HSW_LCPLL_810_ID 12

/* The LCPLL's three DisplayPort outputs.  (Haswell ULX parts stop at 2.7
 * GHz; the dispatcher knows those.) */
int hsw_dpll_rate_supported(uint32_t link_rate_khz)
{
	return link_rate_khz == 162000 || link_rate_khz == 270000 || link_rate_khz == 540000;
}

int hsw_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].link_rate_khz = 0;
		d->dpll[i].cfgcr1 = 0;
	}
	i915_dbg("[drm] i915: LCPLL %08x, WRPLL1 %08x, WRPLL2 %08x, SPLL %08x\n",
		 i915_read32(i915, LCPLL_CTL), i915_read32(i915, HSW_WRPLL_CTL(0)),
		 i915_read32(i915, HSW_WRPLL_CTL(1)), i915_read32(i915, SPLL_CTL));
	return 0;
}

/* DisplayPort: the LCPLL itself; the "PLL id" names the output for the
 * clock select.  It is always on: nothing to enable. */
int hsw_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	(void)i915; (void)port; (void)ssc;
	switch (link_rate_khz / 2) {
	case 270000: return HSW_LCPLL_2700_ID;
	case 135000: return HSW_LCPLL_1350_ID;
	case 81000: return HSW_LCPLL_810_ID;
	default: return -EINVAL;
	}
}

/* HDMI: a WRPLL from the LCPLL with dividers from hsw_wrpll_calc(); the
 * enable bit goes in with them, and the PLL is given 20 us to settle. */
int hsw_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	uint32_t r2, n2, p;
	(void)port;

	hsw_wrpll_calc(clock_khz, &r2, &n2, &p);
	if (!p || !r2 || !n2) {
		kprintf("[drm] i915: no WRPLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	uint32_t val = WRPLL_PLL_ENABLE | WRPLL_REF_LCPLL | WRPLL_DIVIDER_REFERENCE(r2) |
		       WRPLL_DIVIDER_FEEDBACK(n2) | WRPLL_DIVIDER_POST(p);
	for (int i = 0; i < HSW_NUM_WRPLLS; i++) {
		if (d->dpll[i].in_use && d->dpll[i].cfgcr1 == val) {
			d->dpll[i].in_use++;
			return i;
		}
	}
	for (int i = 0; i < HSW_NUM_WRPLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		uint32_t reg = HSW_WRPLL_CTL(i);
		/* the firmware may have left it running with other dividers */
		if (i915_read32(i915, reg) & WRPLL_PLL_ENABLE) {
			i915_write32(i915, reg, i915_read32(i915, reg) & ~WRPLL_PLL_ENABLE);
			(void)i915_read32(i915, reg);
			lapic_delay_us(20);
		}
		i915_write32(i915, reg, val);
		(void)i915_read32(i915, reg);
		lapic_delay_us(20);
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 1;
		d->dpll[i].cfgcr1 = val;
		d->dpll[i].link_rate_khz = hsw_wrpll_khz(r2, n2, p);
		i915_dbg("[drm] i915: WRPLL%d at %u kHz (r2 %u n2 %u p %u)\n", i + 1,
			 d->dpll[i].link_rate_khz, r2, n2, p);
		return i;
	}
	return -ENOSPC;
}

void hsw_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (pll < 0 || pll >= HSW_NUM_WRPLLS)
		return; /* the LCPLL outputs stay on */
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
	if (d->dpll[pll].in_use)
		return;
	d->dpll[pll].cfgcr1 = 0;
	uint32_t reg = HSW_WRPLL_CTL(pll);
	i915_write32(i915, reg, i915_read32(i915, reg) & ~WRPLL_PLL_ENABLE);
	(void)i915_read32(i915, reg);
}

void hsw_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	uint32_t sel;
	switch (pll) {
	case HSW_LCPLL_2700_ID: sel = PORT_CLK_SEL_LCPLL_2700; break;
	case HSW_LCPLL_1350_ID: sel = PORT_CLK_SEL_LCPLL_1350; break;
	case HSW_LCPLL_810_ID: sel = PORT_CLK_SEL_LCPLL_810; break;
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

/* Haswell's and Broadwell's tunings differ; Broadwell adds a table for
 * low-voltage-swing eDP panels. */
static const struct buf_trans hsw_dp[] = {
	{ 0x00FFFFFF, 0x0006000E }, { 0x00D75FFF, 0x0005000A }, { 0x00C30FFF, 0x00040006 },
	{ 0x80AAAFFF, 0x000B0000 }, { 0x00FFFFFF, 0x0005000A }, { 0x00D75FFF, 0x000C0004 },
	{ 0x80C30FFF, 0x000B0000 }, { 0x00FFFFFF, 0x00040006 }, { 0x80D75FFF, 0x000B0000 },
};
static const struct buf_trans hsw_hdmi[] = {
	{ 0x00FFFFFF, 0x0006000E }, { 0x00E79FFF, 0x000E000C }, { 0x00D75FFF, 0x0005000A },
	{ 0x00FFFFFF, 0x0005000A }, { 0x00E79FFF, 0x001D0007 }, { 0x00D75FFF, 0x000C0004 },
	{ 0x00FFFFFF, 0x00040006 }, { 0x80E79FFF, 0x00030002 }, { 0x00FFFFFF, 0x00140005 },
	{ 0x00FFFFFF, 0x000C0004 }, { 0x00FFFFFF, 0x001C0003 }, { 0x80FFFFFF, 0x00030002 },
};
#define HSW_HDMI_DEFAULT_LEVEL 6
static const struct buf_trans bdw_dp[] = {
	{ 0x00FFFFFF, 0x0007000E }, { 0x00D75FFF, 0x000E000A }, { 0x00BEFFFF, 0x00140006 },
	{ 0x80B2CFFF, 0x001B0002 }, { 0x00FFFFFF, 0x000E000A }, { 0x00DB6FFF, 0x00160005 },
	{ 0x80C71FFF, 0x001A0002 }, { 0x00F7DFFF, 0x00180004 }, { 0x80D75FFF, 0x001B0002 },
};
static const struct buf_trans bdw_edp[] = {
	{ 0x00FFFFFF, 0x00000012 }, { 0x00EBAFFF, 0x00020011 }, { 0x00C71FFF, 0x0006000F },
	{ 0x00AAAFFF, 0x000E000A }, { 0x00FFFFFF, 0x00020011 }, { 0x00DB6FFF, 0x0005000F },
	{ 0x00BEEFFF, 0x000A000C }, { 0x00FFFFFF, 0x0005000F }, { 0x00DB6FFF, 0x000A000C },
};
static const struct buf_trans bdw_hdmi[] = {
	{ 0x00FFFFFF, 0x0007000E }, { 0x00D75FFF, 0x000E000A }, { 0x00BEFFFF, 0x00140006 },
	{ 0x00FFFFFF, 0x0009000D }, { 0x00FFFFFF, 0x000E000A }, { 0x00D7FFFF, 0x00140006 },
	{ 0x80CB2FFF, 0x001B0002 }, { 0x00FFFFFF, 0x00140006 }, { 0x80E79FFF, 0x001B0002 },
	{ 0x80FFFFFF, 0x001B0002 },
};
#define BDW_HDMI_DEFAULT_LEVEL 7

void bdw_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o, int level)
{
	const struct buf_trans *t;
	unsigned n;
	int hsw = i915->info->platform == I915_PLATFORM_HASWELL;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) {
		t = hsw ? hsw_hdmi : bdw_hdmi;
		n = hsw ? sizeof(hsw_hdmi) / sizeof(hsw_hdmi[0]) : sizeof(bdw_hdmi) / sizeof(bdw_hdmi[0]);
		if (level < 0 || (unsigned)level >= n)
			level = hsw ? HSW_HDMI_DEFAULT_LEVEL : BDW_HDMI_DEFAULT_LEVEL;
		/* the HDMI level goes into entry 9, which the port then uses */
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, 9), t[level].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, 9), t[level].hi);
		return;
	}
	if (hsw) {
		t = hsw_dp;
		n = sizeof(hsw_dp) / sizeof(hsw_dp[0]);
	} else if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std) {
		t = bdw_edp;
		n = sizeof(bdw_edp) / sizeof(bdw_edp[0]);
	} else {
		t = bdw_dp;
		n = sizeof(bdw_dp) / sizeof(bdw_dp[0]);
	}
	if (level < 0)
		level = 0;
	if ((unsigned)level >= n)
		level = (int)n - 1;
	for (unsigned i = 0; i < n && i < 10; i++) {
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, (int)i), t[i].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, (int)i), t[i].hi);
	}
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	v &= ~DDI_BUF_EMP_MASK;
	v |= DDI_BUF_TRANS_SELECT((uint32_t)level);
	i915_write32(i915, DDI_BUF_CTL(o->port), v);
}
