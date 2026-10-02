// LikeOS -- the combo PHY ports of Ice Lake and later: their PLLs, the
// port clock routing, the PHY itself, and its signal levels.
//
// Which ports are combo ports, which PHY each one drives and which PLLs
// it may take is a per-platform matter:
//
//   Ice Lake            DDI A, B on PHY A, B; DPLL0/1
//   Elkhart/Jasper Lake DDI A, B, C on PHY A, B, C, and DDI D on PHY A
//                       (one or the other, by board); DPLL0/1, and DPLL4
//                       for anything but DDI A
//   Tiger Lake,
//   Alder Lake-P        DDI A, B on PHY A, B; DPLL0/1
//   Rocket Lake         DDI A, B, TC1, TC2 on PHY A-D; DPLL0/1/4
//   DG1                 DDI A, B, TC1, TC2 on PHY A-D; DPLL0/1 for A/B,
//                       DPLL2/3 for C/D
//   Alder Lake-S        DDI A, TC1-TC4 on PHY A-E; DPLL0-3
//
// A combo PLL is a DCO plus three dividers: for DisplayPort from the
// hardware's table per reference clock, for HDMI from a search
// (intel_dpll_calc.c).  The port is routed to its PLL through
// DPCLKA_CFGCR0 (or the platform's variant of it).  The PHY has a
// register file of its own: a one-time initialisation per PHY (process
// and voltage compensation, a reference current generator on the PHYs other
// PHYs take their compensation from), the lanes powered for the link
// width, and the voltage-swing/pre-emphasis levels, which are not a
// translation table in the DDI but a set of PHY registers written per
// level.  Type-C ports are intel_tc.c's.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2018-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dpll_calc.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* The combo PLL ids (the slots in display.dpll[]). */
#define ICL_DPLL0 0
#define ICL_DPLL1 1
#define EHL_DPLL4 2 /* Elkhart/Jasper Lake, Rocket Lake */
#define DG1_DPLL2 2 /* DG1, Alder Lake-S */
#define DG1_DPLL3 3
#define ICL_TBT_PLL 2 /* its configuration words' index on Ice/Tiger Lake */
#define MAX_COMBO_PLLS 4

#define PHY_A 0
#define PHY_B 1
#define PHY_C 2
#define PHY_D 3
#define PHY_E 4
#define MAX_COMBO_PHYS 5

/* ---- the platform ------------------------------------------------------------- */

static int plat(struct i915_device *i915)
{
	return i915->info->platform;
}

static int is_ehl_jsl(struct i915_device *i915)
{
	return plat(i915) == I915_PLATFORM_ELKHARTLAKE || plat(i915) == I915_PLATFORM_JASPERLAKE;
}

static int is_rkl(struct i915_device *i915)
{
	return plat(i915) == I915_PLATFORM_ROCKETLAKE;
}

static int is_dg1(struct i915_device *i915)
{
	return plat(i915) == I915_PLATFORM_DG1;
}

static int is_adls(struct i915_device *i915)
{
	return plat(i915) == I915_PLATFORM_ALDERLAKE_S;
}

static int is_adlp(struct i915_device *i915)
{
	return plat(i915) == I915_PLATFORM_ALDERLAKE_P;
}

static int gen12_plus(struct i915_device *i915)
{
	return i915->info->display_ver >= 12;
}

/* Tiger Lake U and Y (the GT2 parts) have their own HBR2 levels. */
static int is_tgl_uy(struct i915_device *i915)
{
	static const uint16_t ids[] = { 0x9A40, 0x9A49, 0x9A59, 0x9A78,
					0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8 };
	if (plat(i915) != I915_PLATFORM_TIGERLAKE)
		return 0;
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
		if (i915->devid == ids[i])
			return 1;
	return 0;
}

/* The combo PHY a port drives. */
static int phy_of(struct i915_device *i915, int port)
{
	if (is_adls(i915) && port >= PORT_TC1)
		return PHY_B + port - PORT_TC1;
	if ((is_dg1(i915) || is_rkl(i915)) && port >= PORT_TC1)
		return PHY_C + port - PORT_TC1;
	if (is_ehl_jsl(i915) && port == PORT_D)
		return PHY_A;
	return port;
}

static int is_combo_phy(struct i915_device *i915, int phy)
{
	if (phy < 0)
		return 0;
	if (is_adls(i915))
		return phy <= PHY_E;
	if (is_dg1(i915) || is_rkl(i915))
		return phy <= PHY_D;
	if (is_ehl_jsl(i915))
		return phy <= PHY_C;
	return phy <= PHY_B; /* Ice Lake, Tiger Lake, Alder Lake-P */
}

/* The PLLs' reference clock, from DSSM. */
static uint32_t icl_ref_khz(struct i915_device *i915)
{
	switch (i915_read32(i915, DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: return 19200;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: return 38400;
	default: return 24000;
	}
}

static void rmw(struct i915_device *i915, uint32_t reg, uint32_t clear, uint32_t set)
{
	i915_write32(i915, reg, (i915_read32(i915, reg) & ~clear) | set);
}

static int wait_bit(struct i915_device *i915, uint32_t reg, uint32_t bit, int set,
		    uint32_t timeout_us)
{
	for (uint32_t t = 0; t <= timeout_us; t += 10) {
		if (!!(i915_read32(i915, reg) & bit) == set)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* ---- the combo PLLs ---------------------------------------------------------- */

static uint32_t pll_enable_reg(struct i915_device *i915, int id)
{
	if (is_dg1(i915)) {
		static const uint32_t dg1[] = { GEN11_DPLL0_ENABLE, GEN11_DPLL1_ENABLE,
						DG1_DPLL2_ENABLE, DG1_DPLL3_ENABLE };
		return dg1[id & 3];
	}
	if (is_ehl_jsl(i915) && id == EHL_DPLL4)
		return EHL_DPLL4_ENABLE;
	switch (id) {
	case 0: return GEN11_DPLL0_ENABLE;
	case 1: return GEN11_DPLL1_ENABLE;
	case 2: return ADLS_DPLL2_ENABLE; /* Rocket Lake's DPLL4 too */
	default: return ADLS_DPLL3_ENABLE;
	}
}

static void pll_cfgcr_regs(struct i915_device *i915, int id, uint32_t *r0, uint32_t *r1)
{
	if (is_adls(i915)) {
		static const uint32_t c0[] = { TGL_DPLL0_CFGCR0, TGL_DPLL1_CFGCR0, RKL_DPLL4_CFGCR0,
					       ADLS_DPLL3_CFGCR0 };
		static const uint32_t c1[] = { TGL_DPLL0_CFGCR1, TGL_DPLL1_CFGCR1, RKL_DPLL4_CFGCR1,
					       ADLS_DPLL3_CFGCR1 };
		*r0 = c0[id & 3];
		*r1 = c1[id & 3];
	} else if (is_dg1(i915)) {
		static const uint32_t c0[] = { TGL_DPLL0_CFGCR0, TGL_DPLL1_CFGCR0, DG1_DPLL2_CFGCR0,
					       DG1_DPLL3_CFGCR0 };
		static const uint32_t c1[] = { TGL_DPLL0_CFGCR1, TGL_DPLL1_CFGCR1, DG1_DPLL2_CFGCR1,
					       DG1_DPLL3_CFGCR1 };
		*r0 = c0[id & 3];
		*r1 = c1[id & 3];
	} else if (is_rkl(i915)) {
		*r0 = TGL_DPLL0_CFGCR0 + (uint32_t)id * 8;
		*r1 = TGL_DPLL0_CFGCR1 + (uint32_t)id * 8;
	} else if (gen12_plus(i915)) {
		/* Tiger Lake, Alder Lake-P: DPLL0, DPLL1, the Thunderbolt PLL */
		static const uint32_t c0[] = { TGL_DPLL0_CFGCR0, TGL_DPLL1_CFGCR0,
					       TGL_TBTPLL_CFGCR0_REG };
		static const uint32_t c1[] = { TGL_DPLL0_CFGCR1, TGL_DPLL1_CFGCR1,
					       TGL_TBTPLL_CFGCR1_REG };
		*r0 = c0[id > 2 ? 2 : id];
		*r1 = c1[id > 2 ? 2 : id];
	} else {
		/* Ice Lake: at 0x80 per PLL; Elkhart Lake's DPLL4 is the fifth */
		int idx = (is_ehl_jsl(i915) && id == EHL_DPLL4) ? 4 : id;
		*r0 = ICL_DPLL_CFGCR0_N(idx);
		*r1 = ICL_DPLL_CFGCR1_N(idx);
	}
}

/* Halve the DCO fraction on the parts whose PLL doubles it at a 38.4 MHz
 * reference: Elkhart Lake from B0 and every display version 12 part. */
static int div_frac_wa(struct i915_device *i915)
{
	int ehl_b0 = plat(i915) == I915_PLATFORM_ELKHARTLAKE && i915->revid >= 1;
	return (ehl_b0 || gen12_plus(i915)) && icl_ref_khz(i915) == 38400;
}

static void calc_cfgcr(struct i915_device *i915, const struct icl_pll_params *p, uint32_t *cfgcr0,
		       uint32_t *cfgcr1)
{
	uint32_t frac = p->dco_fraction;

	if (div_frac_wa(i915))
		frac = (frac + 1) / 2;
	*cfgcr0 = DPLL_CFGCR0_DCO_FRACTION(frac) | p->dco_integer;
	*cfgcr1 = DPLL_CFGCR1_QDIV_RATIO(p->qdiv_ratio) | DPLL_CFGCR1_QDIV_MODE(p->qdiv_mode) |
		  DPLL_CFGCR1_KDIV(p->kdiv) | DPLL_CFGCR1_PDIV(p->pdiv);
	*cfgcr1 |= gen12_plus(i915) ? TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL :
				      DPLL_CFGCR1_CENTRAL_FREQ_8400;
}

/* The port clock a CFGCR pair runs at (kHz). */
static uint32_t cfgcr_khz(struct i915_device *i915, uint32_t cfgcr0, uint32_t cfgcr1)
{
	struct icl_pll_params p;
	uint32_t frac = (cfgcr0 & DPLL_CFGCR0_DCO_FRACTION_MASK) >> GEN11_DPLL_CFGCR0_DCO_FRACTION_SHIFT;

	if (div_frac_wa(i915))
		frac *= 2;
	p.dco_integer = (uint16_t)(cfgcr0 & DPLL_CFGCR0_DCO_INTEGER_MASK);
	p.dco_fraction = (uint16_t)frac;
	p.pdiv = (uint8_t)((cfgcr1 & DPLL_CFGCR1_PDIV_MASK) >> GEN11_DPLL_CFGCR1_PDIV_SHIFT);
	p.kdiv = (uint8_t)((cfgcr1 & DPLL_CFGCR1_KDIV_MASK) >> GEN11_DPLL_CFGCR1_KDIV_SHIFT);
	p.qdiv_mode = !!(cfgcr1 & GEN11_DPLL_CFGCR1_QDIV_MODE);
	p.qdiv_ratio = (uint8_t)((cfgcr1 & DPLL_CFGCR1_QDIV_RATIO_MASK) >> GEN11_DPLL_CFGCR1_QDIV_RATIO_SHIFT);
	return icl_combo_pll_khz(&p, icl_ref_khz(i915));
}

/* The PLLs a port may take, lowest id first. */
static uint32_t combo_pll_mask(struct i915_device *i915, int port)
{
	uint32_t mask;

	if (is_adls(i915))
		mask = (1u << ICL_DPLL0) | (1u << ICL_DPLL1) | (1u << DG1_DPLL2) | (1u << DG1_DPLL3);
	else if (is_dg1(i915))
		mask = (port == PORT_D || port == PORT_E) ? (1u << DG1_DPLL2) | (1u << DG1_DPLL3) :
							    (1u << ICL_DPLL0) | (1u << ICL_DPLL1);
	else if (is_rkl(i915) || (is_ehl_jsl(i915) && port != PORT_A))
		mask = (1u << ICL_DPLL0) | (1u << ICL_DPLL1) | (1u << EHL_DPLL4);
	else
		mask = (1u << ICL_DPLL0) | (1u << ICL_DPLL1);
	/* Rocket Lake and Alder Lake-S: the HD port may hold some of them */
	if (is_rkl(i915) || is_adls(i915)) {
		uint32_t hti = i915_read32(i915, 0x45050); /* HDPORT_STATE */
		if (hti & 1u)
			mask &= ~((hti >> 12) & 0xfu);
	}
	return mask;
}

static void combo_pll_disable(struct i915_device *i915, uint32_t reg)
{
	uint32_t v = i915_read32(i915, reg);

	if (v & PLL_ENABLE) {
		i915_write32(i915, reg, v & ~PLL_ENABLE);
		if (wait_bit(i915, reg, PLL_LOCK, 0, 1000))
			kprintf("[drm] i915: PLL %05x still locked\n", reg);
	}
	v = i915_read32(i915, reg);
	if (v & PLL_POWER_ENABLE) {
		i915_write32(i915, reg, v & ~PLL_POWER_ENABLE);
		if (wait_bit(i915, reg, PLL_POWER_STATE, 0, 1000))
			kprintf("[drm] i915: PLL %05x power not disabled\n", reg);
	}
}

/* Power up, configure, enable, lock (600 us, given 1 ms). */
static int combo_pll_program(struct i915_device *i915, uint32_t en, uint32_t r0, uint32_t r1,
			     uint32_t cfgcr0, uint32_t cfgcr1)
{
	/* a running PLL (the firmware's) is stopped first */
	combo_pll_disable(i915, en);
	rmw(i915, en, 0, PLL_POWER_ENABLE);
	if (wait_bit(i915, en, PLL_POWER_STATE, 1, 1000)) {
		kprintf("[drm] i915: PLL %05x did not power up\n", en);
		return -EIO;
	}
	i915_write32(i915, r0, cfgcr0);
	i915_write32(i915, r1, cfgcr1);
	(void)i915_read32(i915, r1);
	rmw(i915, en, 0, PLL_ENABLE);
	if (wait_bit(i915, en, PLL_LOCK, 1, 1000)) {
		kprintf("[drm] i915: PLL %05x did not lock (%08x/%08x)\n", en, cfgcr0, cfgcr1);
		return -EIO;
	}
	return 0;
}

int icl_dpll_rate_supported(uint32_t link_rate_khz)
{
	struct icl_pll_params p;
	return icl_combo_dp_params(link_rate_khz, 24000, &p) == 0;
}

/* Find a PLL in the port's set already running this configuration, or
 * program a free one. */
static int combo_pll_get(struct i915_device *i915, int port, uint32_t cfgcr0, uint32_t cfgcr1,
			 int is_hdmi, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	uint32_t mask = combo_pll_mask(i915, port);
	int free_id = -1;

	for (int id = 0; id < MAX_COMBO_PLLS; id++) {
		if (!(mask & (1u << id)))
			continue;
		if (!d->dpll[id].in_use) {
			if (free_id < 0)
				free_id = id;
			continue;
		}
		if (d->dpll[id].cfgcr0 == cfgcr0 && d->dpll[id].cfgcr1 == cfgcr1) {
			d->dpll[id].in_use++;
			return id;
		}
	}
	if (free_id < 0)
		return -ENOSPC;
	uint32_t r0, r1;
	pll_cfgcr_regs(i915, free_id, &r0, &r1);
	if (combo_pll_program(i915, pll_enable_reg(i915, free_id), r0, r1, cfgcr0, cfgcr1))
		return -EIO;
	d->dpll[free_id].in_use = 1;
	d->dpll[free_id].is_hdmi = is_hdmi;
	d->dpll[free_id].ssc = 0;
	d->dpll[free_id].cfgcr0 = cfgcr0;
	d->dpll[free_id].cfgcr1 = cfgcr1;
	d->dpll[free_id].link_rate_khz = clock_khz;
	i915_dbg("[drm] i915: DPLL%d at %u kHz for port %s (%08x/%08x)\n",
		 (is_ehl_jsl(i915) || is_rkl(i915)) && free_id == EHL_DPLL4 ? 4 : free_id, clock_khz,
		 intel_port_name(i915, port), cfgcr0, cfgcr1);
	return free_id;
}

int icl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	struct icl_pll_params p;
	uint32_t cfgcr0, cfgcr1;
	(void)ssc; /* these PLLs are not spread */

	if (icl_combo_dp_params(link_rate_khz, icl_ref_khz(i915), &p))
		return -EINVAL;
	calc_cfgcr(i915, &p, &cfgcr0, &cfgcr1);
	return combo_pll_get(i915, port, cfgcr0, cfgcr1, 0, link_rate_khz);
}

int icl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct icl_pll_params p;
	uint32_t cfgcr0, cfgcr1;

	if (icl_combo_hdmi_params(clock_khz, icl_ref_khz(i915), &p)) {
		kprintf("[drm] i915: no combo PLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	calc_cfgcr(i915, &p, &cfgcr0, &cfgcr1);
	return combo_pll_get(i915, port, cfgcr0, cfgcr1, 1, cfgcr_khz(i915, cfgcr0, cfgcr1));
}

void icl_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (pll < 0 || pll >= MAX_COMBO_PLLS)
		return;
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
	if (d->dpll[pll].in_use)
		return;
	d->dpll[pll].cfgcr0 = d->dpll[pll].cfgcr1 = 0;
	combo_pll_disable(i915, pll_enable_reg(i915, pll));
}

/* ---- the Thunderbolt PLL (for intel_tc.c) ------------------------------------- */

/* Its dividers are fixed: the ports pick 1.62..8.1 GHz from its outputs
 * at their clock select.  Display version 12 has other values. */
int icl_tbt_pll_enable(struct i915_device *i915)
{
	struct icl_pll_params p = { 0, 0, 0, 0, 0, 0 };
	uint32_t ref = icl_ref_khz(i915);
	uint32_t cfgcr0, cfgcr1, r0, r1;

	if (gen12_plus(i915)) {
		if (ref == 24000) {
			p.dco_integer = 0x43;
			p.dco_fraction = 0x4000;
		} else {
			p.dco_integer = 0x54;
			p.dco_fraction = 0x3000;
		}
	} else {
		p.pdiv = 0x4; /* 5 */
		p.kdiv = 1;
		if (ref == 24000) {
			p.dco_integer = 0x151;
			p.dco_fraction = 0x4000;
		} else {
			p.dco_integer = 0x1A5;
			p.dco_fraction = 0x7000;
		}
	}
	calc_cfgcr(i915, &p, &cfgcr0, &cfgcr1);
	pll_cfgcr_regs(i915, ICL_TBT_PLL, &r0, &r1);
	if ((i915_read32(i915, GEN11_TBT_PLL_ENABLE) & PLL_LOCK) &&
	    i915_read32(i915, r0) == cfgcr0 && i915_read32(i915, r1) == cfgcr1)
		return 0; /* running as wanted (the firmware's) */
	return combo_pll_program(i915, GEN11_TBT_PLL_ENABLE, r0, r1, cfgcr0, cfgcr1);
}

void icl_tbt_pll_disable(struct i915_device *i915)
{
	combo_pll_disable(i915, GEN11_TBT_PLL_ENABLE);
}

/* ---- the port clock routing ------------------------------------------------------ */

/* The select of a PHY's PLL, the select's mask, and the gate bit, in the
 * register the platform has them in. */
struct dpclka {
	uint32_t reg, sel_shift, off_bit;
	uint32_t sel_val; /* the select value for the PLL id */
};

static struct dpclka dpclka_of(struct i915_device *i915, int phy, int pll)
{
	struct dpclka c;

	if (is_adls(i915)) {
		c.reg = ADLS_DPCLKA_CFGCR(phy);
		c.sel_shift = ADLS_DPCLKA_DDI_SHIFT(phy);
		c.off_bit = ICL_DPCLKA_DDI_CLK_OFF(phy);
		c.sel_val = (uint32_t)pll;
	} else if (is_dg1(i915)) {
		c.reg = DG1_DPCLKA_CFGCR0(phy);
		c.sel_shift = DG1_DPCLKA_DDI_CLK_SEL_SHIFT(phy);
		c.off_bit = DG1_DPCLKA_DDI_CLK_OFF(phy);
		c.sel_val = (uint32_t)pll % 2;
	} else if (is_rkl(i915)) {
		c.reg = GEN11_DPCLKA_CFGCR0;
		c.sel_shift = RKL_DPCLKA_DDI_CLK_SEL_SHIFT(phy);
		c.off_bit = RKL_DPCLKA_DDI_CLK_OFF(phy);
		c.sel_val = (uint32_t)pll;
	} else {
		c.reg = GEN11_DPCLKA_CFGCR0;
		c.sel_shift = ICL_DPCLKA_DDI_CLK_SEL_SHIFT(phy);
		c.off_bit = ICL_DPCLKA_DDI_CLK_OFF(phy);
		c.sel_val = (uint32_t)pll;
	}
	return c;
}

/* Elkhart/Jasper Lake's DDI C and D must also have the (absent) MG
 * clock selected in their DDI clock select to be ungated. */
static int ehl_needs_ddi_clk_sel(struct i915_device *i915, int port)
{
	return is_ehl_jsl(i915) && port >= PORT_C;
}

/* The port takes its clock from the PLL: select it, then ungate -- two
 * separate writes. */
void icl_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	struct dpclka c = dpclka_of(i915, phy_of(i915, port), pll);

	if (is_dg1(i915) && ((pll < DG1_DPLL2) != (phy_of(i915, port) < PHY_C))) {
		kprintf("[drm] i915: port %s cannot take DPLL%d\n", intel_port_name(i915, port), pll);
		return;
	}
	if (ehl_needs_ddi_clk_sel(i915, port))
		i915_write32(i915, GEN11_DDI_CLK_SEL(port), ICL_DDI_CLK_SEL_MG);
	rmw(i915, c.reg, 3u << c.sel_shift, c.sel_val << c.sel_shift);
	(void)i915_read32(i915, c.reg);
	rmw(i915, c.reg, c.off_bit, 0);
	(void)i915_read32(i915, c.reg);
}

void icl_dpll_unroute_port(struct i915_device *i915, int port)
{
	struct dpclka c = dpclka_of(i915, phy_of(i915, port), 0);

	rmw(i915, c.reg, 0, c.off_bit);
	(void)i915_read32(i915, c.reg);
	if (ehl_needs_ddi_clk_sel(i915, port))
		i915_write32(i915, GEN11_DDI_CLK_SEL(port), ICL_DDI_CLK_SEL_NONE);
}

/* The rate of the PLL the port is routed to, or 0 (the firmware's
 * state, read before the first mode set). */
uint32_t icl_dpll_port_link_rate(struct i915_device *i915, int port)
{
	int phy = phy_of(i915, port);
	struct dpclka c = dpclka_of(i915, phy, 0);
	uint32_t v = i915_read32(i915, c.reg);
	uint32_t r0, r1;
	int pll;

	if (!is_combo_phy(i915, phy) || (v & c.off_bit))
		return 0;
	if (ehl_needs_ddi_clk_sel(i915, port) &&
	    (i915_read32(i915, GEN11_DDI_CLK_SEL(port)) & ICL_DDI_CLK_SEL_MASK) == ICL_DDI_CLK_SEL_NONE)
		return 0;
	pll = (int)((v >> c.sel_shift) & 3);
	if (is_dg1(i915) && phy >= PHY_C)
		pll += DG1_DPLL2;
	if (!(combo_pll_mask(i915, port) & (1u << pll)) && pll > ICL_DPLL1)
		return 0;
	if (!(i915_read32(i915, pll_enable_reg(i915, pll)) & PLL_ENABLE))
		return 0;
	pll_cfgcr_regs(i915, pll, &r0, &r1);
	return cfgcr_khz(i915, i915_read32(i915, r0), i915_read32(i915, r1));
}

/* ---- the PHY ------------------------------------------------------------------- */

/* Process/voltage compensation values, by what the PHY reports itself
 * to be; anything unknown gets the 0.85 V ones. */
struct procmon {
	const char *name;
	uint32_t dw1, dw9, dw10;
};

static const struct procmon procmon_values[] = {
	{ "0.85V dot0 (low-voltage)", 0x00000000, 0x62AB67BB, 0x51914F96 },
	{ "0.95V dot0", 0x00000000, 0x86E172C7, 0x77CA5EAB },
	{ "0.95V dot1", 0x00000000, 0x93F87FE1, 0x8AE871C5 },
	{ "1.05V dot0", 0x00000000, 0x98FA82DD, 0x89E46DC1 },
	{ "1.05V dot1", 0x00440000, 0x9A00AB25, 0x8AE38FF1 },
};

static const struct procmon *procmon_for(struct i915_device *i915, int phy)
{
	uint32_t v = i915_read32(i915, ICL_PORT_COMP_DW(3, phy));

	switch (v & (ICL_COMBO_PROCESS_INFO_MASK | ICL_COMBO_VOLTAGE_INFO_MASK)) {
	case ICL_COMBO_VOLTAGE_INFO_0_95V | ICL_COMBO_PROCESS_INFO_DOT_0: return &procmon_values[1];
	case ICL_COMBO_VOLTAGE_INFO_0_95V | ICL_COMBO_PROCESS_INFO_DOT_1: return &procmon_values[2];
	case ICL_COMBO_VOLTAGE_INFO_1_05V | ICL_COMBO_PROCESS_INFO_DOT_0: return &procmon_values[3];
	case ICL_COMBO_VOLTAGE_INFO_1_05V | ICL_COMBO_PROCESS_INFO_DOT_1: return &procmon_values[4];
	case ICL_COMBO_VOLTAGE_INFO_0_85V | ICL_COMBO_PROCESS_INFO_DOT_0:
		return &procmon_values[0];
	default:
		i915_dbg("[drm] i915: combo PHY %c reports process/voltage %08x\n", 'A' + phy, v);
		return &procmon_values[0];
	}
}

/* PHY_MISC exists (or is to be programmed) only on some PHYs: A and B on
 * Elkhart Lake, Rocket Lake and DG1, A alone on Alder Lake-S. */
static int has_phy_misc(struct i915_device *i915, int phy)
{
	if (is_adls(i915))
		return phy == PHY_A;
	if (is_ehl_jsl(i915) || is_rkl(i915) || is_dg1(i915))
		return phy < PHY_C;
	return 1;
}

/* The PHYs with a compensation resistor, which the others take their
 * compensation from: A everywhere; C on Rocket Lake and DG1 (for D);
 * D on Alder Lake-S (for E).  Their reference generator must be on. */
static int phy_is_master(struct i915_device *i915, int phy)
{
	if (phy == PHY_A)
		return 1;
	if (is_adls(i915))
		return phy == PHY_D;
	if (is_dg1(i915) || is_rkl(i915))
		return phy == PHY_C;
	return 0;
}

/* Elkhart Lake's PHY A drives DDI D (an external connector) instead of
 * DDI A when the VBT has a child device on D and none on A. */
static int ehl_vbt_ddi_d_present(struct i915_device *i915)
{
	struct intel_vbt *vbt = &i915->display.vbt;
	int a, dd;

	if (!vbt->valid)
		return 0;
	a = vbt->port[PORT_A].present;
	dd = vbt->port[PORT_D].present;
	if (dd && !a)
		return 1;
	if (dd)
		kprintf("[drm] i915: the VBT has both an internal and an external display on PHY A; configuring for the internal one\n");
	return 0;
}

static int combo_phy_enabled(struct i915_device *i915, int phy)
{
	int comp = !!(i915_read32(i915, ICL_PORT_COMP_DW(0, phy)) & COMP_INIT);

	if (!has_phy_misc(i915, phy))
		return comp;
	return !(i915_read32(i915, ICL_COMBO_PHY_MISC(phy)) & ICL_COMBO_PHY_MISC_DE_IO_COMP_PWR_DOWN) &&
	       comp;
}

static int check_phy_reg(struct i915_device *i915, int phy, uint32_t reg, uint32_t mask,
			 uint32_t expected)
{
	uint32_t v = i915_read32(i915, reg);
	if ((v & mask) == expected)
		return 1;
	i915_dbg("[drm] i915: combo PHY %c reg %06x is %08x, expected %08x under %08x\n", 'A' + phy,
		 reg, v, expected, mask);
	return 0;
}

static int combo_phy_verify(struct i915_device *i915, int phy)
{
	const struct procmon *pm;
	int ok = 1;

	if (!combo_phy_enabled(i915, phy))
		return 0;
	if (gen12_plus(i915)) {
		ok &= check_phy_reg(i915, phy, ICL_PORT_TX_DW_LN(8, 0, phy),
				    ICL_COMBO_ODCC_CLK_SEL | ICL_COMBO_ODCC_CLK_DIV_SEL_MASK,
				    ICL_COMBO_ODCC_CLK_SEL | ICL_COMBO_ODCC_CLK_DIV_SEL_DIV2);
		ok &= check_phy_reg(i915, phy, ICL_PORT_PCS_DW_LN(1, 0, phy),
				    ICL_COMBO_DCC_MODE_SELECT_MASK, ICL_COMBO_RUN_DCC_ONCE);
	}
	pm = procmon_for(i915, phy);
	ok &= check_phy_reg(i915, phy, ICL_PORT_COMP_DW(1, phy), (0xffu << 16) | 0xffu, pm->dw1);
	ok &= check_phy_reg(i915, phy, ICL_PORT_COMP_DW(9, phy), ~0u, pm->dw9);
	ok &= check_phy_reg(i915, phy, ICL_PORT_COMP_DW(10, phy), ~0u, pm->dw10);
	if (phy_is_master(i915, phy)) {
		ok &= check_phy_reg(i915, phy, ICL_PORT_COMP_DW(8, phy), IREFGEN, IREFGEN);
		if (is_ehl_jsl(i915))
			ok &= check_phy_reg(i915, phy, ICL_COMBO_PHY_MISC(phy), ICL_COMBO_PHY_MISC_MUX_DDID,
					    ehl_vbt_ddi_d_present(i915) ? ICL_COMBO_PHY_MISC_MUX_DDID : 0);
	}
	ok &= check_phy_reg(i915, phy, ICL_PORT_CL_DW(5, phy), CL_POWER_DOWN_ENABLE,
			    CL_POWER_DOWN_ENABLE);
	return ok;
}

static void combo_phy_init_one(struct i915_device *i915, int phy)
{
	const struct procmon *pm;
	uint32_t v;

	if (combo_phy_verify(i915, phy))
		return; /* the firmware's, and right */
	pm = procmon_for(i915, phy);
	i915_dbg("[drm] i915: initialising combo PHY %c (voltage/process: %s)\n", 'A' + phy, pm->name);
	if (has_phy_misc(i915, phy)) {
		v = i915_read32(i915, ICL_COMBO_PHY_MISC(phy));
		if (is_ehl_jsl(i915) && phy == PHY_A) {
			v &= ~ICL_COMBO_PHY_MISC_MUX_DDID;
			if (ehl_vbt_ddi_d_present(i915))
				v |= ICL_COMBO_PHY_MISC_MUX_DDID;
		}
		v &= ~ICL_COMBO_PHY_MISC_DE_IO_COMP_PWR_DOWN;
		i915_write32(i915, ICL_COMBO_PHY_MISC(phy), v);
	}
	if (gen12_plus(i915)) {
		v = i915_read32(i915, ICL_PORT_TX_DW_LN(8, 0, phy));
		v &= ~ICL_COMBO_ODCC_CLK_DIV_SEL_MASK;
		v |= ICL_COMBO_ODCC_CLK_SEL | ICL_COMBO_ODCC_CLK_DIV_SEL_DIV2;
		i915_write32(i915, ICL_PORT_TX_DW_GRP(8, phy), v);
		v = i915_read32(i915, ICL_PORT_PCS_DW_LN(1, 0, phy));
		v &= ~ICL_COMBO_DCC_MODE_SELECT_MASK;
		v |= ICL_COMBO_RUN_DCC_ONCE;
		i915_write32(i915, ICL_PORT_PCS_DW_GRP(1, phy), v);
	}
	rmw(i915, ICL_PORT_COMP_DW(1, phy), (0xffu << 16) | 0xffu, pm->dw1);
	i915_write32(i915, ICL_PORT_COMP_DW(9, phy), pm->dw9);
	i915_write32(i915, ICL_PORT_COMP_DW(10, phy), pm->dw10);
	if (phy_is_master(i915, phy))
		rmw(i915, ICL_PORT_COMP_DW(8, phy), 0, IREFGEN);
	rmw(i915, ICL_PORT_COMP_DW(0, phy), 0, COMP_INIT);
	rmw(i915, ICL_PORT_CL_DW(5, phy), 0, CL_POWER_DOWN_ENABLE);
	(void)i915_read32(i915, ICL_PORT_CL_DW(5, phy));
}

/* Every combo PHY of the platform, the masters first; done at display
 * init and again before a port is used (a PHY found set up is left
 * alone). */
static void combo_phys_init(struct i915_device *i915)
{
	for (int phy = 0; phy < MAX_COMBO_PHYS; phy++)
		if (is_combo_phy(i915, phy))
			combo_phy_init_one(i915, phy);
}

int icl_combo_phy_init(struct i915_device *i915, int port)
{
	if (!is_combo_phy(i915, phy_of(i915, port)))
		return -ENODEV;
	combo_phys_init(i915);
	return combo_phy_enabled(i915, phy_of(i915, port)) ? 0 : -EIO;
}

int icl_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].link_rate_khz = 0;
		d->dpll[i].cfgcr0 = d->dpll[i].cfgcr1 = 0;
	}
	combo_phys_init(i915);
	/* Elkhart Lake's DPLL4 needs the display out of its DC states while
	 * it runs; this driver never lets the display into them. */
	i915_dbg("[drm] i915: combo PLLs: DPLL0 %08x DPLL1 %08x, DPCLKA %08x, ref %u kHz\n",
		 i915_read32(i915, GEN11_DPLL0_ENABLE), i915_read32(i915, GEN11_DPLL1_ENABLE),
		 i915_read32(i915, GEN11_DPCLKA_CFGCR0), icl_ref_khz(i915));
	return 0;
}

/* The lanes the port will drive stay powered; the others go down. */
void icl_combo_phy_power_lanes(struct i915_device *i915, int port, int lanes, int reversed)
{
	int phy = phy_of(i915, port);
	uint32_t mask;

	if (!is_combo_phy(i915, phy))
		return;
	switch (lanes) {
	case 1: mask = reversed ? ICL_COMBO_PWR_DOWN_LN_2_1_0 : ICL_COMBO_PWR_DOWN_LN_3_2_1; break;
	case 2: mask = reversed ? ICL_COMBO_PWR_DOWN_LN_1_0 : ICL_COMBO_PWR_DOWN_LN_3_2; break;
	default: mask = ICL_COMBO_PWR_UP_ALL_LANES; break;
	}
	rmw(i915, ICL_PORT_CL_DW(10, phy), ICL_COMBO_PWR_DOWN_LN_MASK, mask);
	(void)i915_read32(i915, ICL_PORT_CL_DW(10, phy));
}

/* ---- signal levels ------------------------------------------------------------ */

/* Per level: the swing selector (DW2), the scalar (DW7), the cursor and
 * the two post-cursor coefficients (DW4).  The DisplayPort tables are
 * ordered 400 mV x4, 600 x3, 800 x2, 1200 x1 by the request they answer
 * (the voltages in the hardware's terms are other). */
struct combo_level {
	uint8_t dw2_swing_sel, dw7_n_scalar, dw4_cursor_coeff, dw4_post_cursor_2,
		dw4_post_cursor_1;
};

#define N(t) ((int)(sizeof(t) / sizeof((t)[0])))

static const struct combo_level icl_dp_hbr2_edp_hbr3[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x71, 0x2F, 0x00, 0x10 }, { 0x6, 0x7F, 0x2B, 0x00, 0x14 },
	{ 0xA, 0x4C, 0x3F, 0x00, 0x00 }, { 0xC, 0x73, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x6C, 0x3C, 0x00, 0x03 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level icl_edp_hbr2[] = {
	{ 0x0, 0x7F, 0x3F, 0x00, 0x00 }, { 0x8, 0x7F, 0x38, 0x00, 0x07 },
	{ 0x1, 0x7F, 0x33, 0x00, 0x0C }, { 0x9, 0x7F, 0x31, 0x00, 0x0E },
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x1, 0x7F, 0x38, 0x00, 0x07 },
	{ 0x9, 0x7F, 0x35, 0x00, 0x0A }, { 0x1, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0x9, 0x7F, 0x38, 0x00, 0x07 }, { 0x9, 0x7F, 0x3F, 0x00, 0x00 },
};
/* HDMI on every combo PHY; the last entry is the default */
static const struct combo_level icl_hdmi[] = {
	{ 0xA, 0x60, 0x3F, 0x00, 0x00 }, { 0xB, 0x73, 0x36, 0x00, 0x09 },
	{ 0x6, 0x7F, 0x31, 0x00, 0x0E }, { 0xB, 0x73, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x37, 0x00, 0x08 }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A },
};
static const struct combo_level ehl_dp[] = {
	{ 0xA, 0x33, 0x3F, 0x00, 0x00 }, { 0xA, 0x47, 0x38, 0x00, 0x07 },
	{ 0xC, 0x64, 0x33, 0x00, 0x0C }, { 0x6, 0x7F, 0x2F, 0x00, 0x10 },
	{ 0xA, 0x46, 0x3F, 0x00, 0x00 }, { 0xC, 0x64, 0x37, 0x00, 0x08 },
	{ 0x6, 0x7F, 0x32, 0x00, 0x0D }, { 0xC, 0x61, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x37, 0x00, 0x08 }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level ehl_edp_hbr2[] = {
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x8, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0x1, 0x7F, 0x3D, 0x00, 0x02 }, { 0xA, 0x35, 0x39, 0x00, 0x06 },
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x1, 0x7F, 0x3C, 0x00, 0x03 },
	{ 0xA, 0x35, 0x39, 0x00, 0x06 }, { 0x1, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0xA, 0x35, 0x38, 0x00, 0x07 }, { 0xA, 0x35, 0x3F, 0x00, 0x00 },
};
static const struct combo_level jsl_edp_hbr[] = {
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x8, 0x7F, 0x38, 0x00, 0x07 },
	{ 0x1, 0x7F, 0x33, 0x00, 0x0C }, { 0xA, 0x35, 0x36, 0x00, 0x09 },
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x1, 0x7F, 0x38, 0x00, 0x07 },
	{ 0xA, 0x35, 0x35, 0x00, 0x0A }, { 0x1, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0xA, 0x35, 0x38, 0x00, 0x07 }, { 0xA, 0x35, 0x3F, 0x00, 0x00 },
};
static const struct combo_level jsl_edp_hbr2[] = {
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x8, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0x1, 0x7F, 0x3D, 0x00, 0x02 }, { 0xA, 0x35, 0x38, 0x00, 0x07 },
	{ 0x8, 0x7F, 0x3F, 0x00, 0x00 }, { 0x1, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0xA, 0x35, 0x3A, 0x00, 0x05 }, { 0x1, 0x7F, 0x3F, 0x00, 0x00 },
	{ 0xA, 0x35, 0x38, 0x00, 0x07 }, { 0xA, 0x35, 0x3F, 0x00, 0x00 },
};
static const struct combo_level dg1_dp_rbr_hbr[] = {
	{ 0xA, 0x32, 0x3F, 0x00, 0x00 }, { 0xA, 0x48, 0x35, 0x00, 0x0A },
	{ 0xC, 0x63, 0x2F, 0x00, 0x10 }, { 0x6, 0x7F, 0x2C, 0x00, 0x13 },
	{ 0xA, 0x43, 0x3F, 0x00, 0x00 }, { 0xC, 0x60, 0x36, 0x00, 0x09 },
	{ 0x6, 0x7F, 0x30, 0x00, 0x0F }, { 0xC, 0x60, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x37, 0x00, 0x08 }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level dg1_dp_hbr2_hbr3[] = {
	{ 0xA, 0x32, 0x3F, 0x00, 0x00 }, { 0xA, 0x48, 0x35, 0x00, 0x0A },
	{ 0xC, 0x63, 0x2F, 0x00, 0x10 }, { 0x6, 0x7F, 0x2C, 0x00, 0x13 },
	{ 0xA, 0x43, 0x3F, 0x00, 0x00 }, { 0xC, 0x60, 0x36, 0x00, 0x09 },
	{ 0x6, 0x7F, 0x30, 0x00, 0x0F }, { 0xC, 0x58, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level tgl_dp_hbr[] = {
	{ 0xA, 0x32, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x71, 0x2F, 0x00, 0x10 }, { 0x6, 0x7D, 0x2B, 0x00, 0x14 },
	{ 0xA, 0x4C, 0x3F, 0x00, 0x00 }, { 0xC, 0x73, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x6C, 0x3C, 0x00, 0x03 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level tgl_dp_hbr2[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x63, 0x2F, 0x00, 0x10 }, { 0x6, 0x7F, 0x2B, 0x00, 0x14 },
	{ 0xA, 0x47, 0x3F, 0x00, 0x00 }, { 0xC, 0x63, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x61, 0x3C, 0x00, 0x03 },
	{ 0x6, 0x7B, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level tgl_uy_dp_hbr2[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x36, 0x00, 0x09 },
	{ 0xC, 0x60, 0x32, 0x00, 0x0D }, { 0xC, 0x7F, 0x2D, 0x00, 0x12 },
	{ 0xC, 0x47, 0x3F, 0x00, 0x00 }, { 0xC, 0x6F, 0x36, 0x00, 0x09 },
	{ 0x6, 0x7D, 0x32, 0x00, 0x0D }, { 0x6, 0x60, 0x3C, 0x00, 0x03 },
	{ 0x6, 0x7F, 0x34, 0x00, 0x0B }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level rkl_dp_hbr[] = {
	{ 0xA, 0x2F, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x63, 0x2F, 0x00, 0x10 }, { 0x6, 0x7D, 0x2A, 0x00, 0x15 },
	{ 0xA, 0x4C, 0x3F, 0x00, 0x00 }, { 0xC, 0x73, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x6E, 0x3E, 0x00, 0x01 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level rkl_dp_hbr2_hbr3[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x50, 0x38, 0x00, 0x07 },
	{ 0xC, 0x61, 0x33, 0x00, 0x0C }, { 0x6, 0x7F, 0x2E, 0x00, 0x11 },
	{ 0xA, 0x47, 0x3F, 0x00, 0x00 }, { 0xC, 0x5F, 0x38, 0x00, 0x07 },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x5F, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7E, 0x36, 0x00, 0x09 }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level adls_dp_hbr2_hbr3[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x63, 0x31, 0x00, 0x0E }, { 0x6, 0x7F, 0x2C, 0x00, 0x13 },
	{ 0xA, 0x47, 0x3F, 0x00, 0x00 }, { 0xC, 0x63, 0x37, 0x00, 0x08 },
	{ 0x6, 0x73, 0x32, 0x00, 0x0D }, { 0xC, 0x58, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level adls_edp_hbr2[] = {
	{ 0x9, 0x73, 0x3D, 0x00, 0x02 }, { 0x9, 0x7A, 0x3C, 0x00, 0x03 },
	{ 0x9, 0x7F, 0x3B, 0x00, 0x04 }, { 0x4, 0x6C, 0x33, 0x00, 0x0C },
	{ 0x2, 0x73, 0x3A, 0x00, 0x05 }, { 0x2, 0x7C, 0x38, 0x00, 0x07 },
	{ 0x4, 0x5A, 0x36, 0x00, 0x09 }, { 0x4, 0x57, 0x3D, 0x00, 0x02 },
	{ 0x4, 0x65, 0x38, 0x00, 0x07 }, { 0x4, 0x6C, 0x3A, 0x00, 0x05 },
};
static const struct combo_level adls_edp_hbr3[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x63, 0x31, 0x00, 0x0E }, { 0x6, 0x7F, 0x2C, 0x00, 0x13 },
	{ 0xA, 0x47, 0x3F, 0x00, 0x00 }, { 0xC, 0x63, 0x37, 0x00, 0x08 },
	{ 0x6, 0x73, 0x32, 0x00, 0x0D }, { 0xC, 0x58, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level adlp_dp_hbr[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x71, 0x31, 0x00, 0x0E }, { 0x6, 0x7F, 0x2C, 0x00, 0x13 },
	{ 0xA, 0x4C, 0x3F, 0x00, 0x00 }, { 0xC, 0x73, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x00, 0x10 }, { 0xC, 0x7C, 0x3C, 0x00, 0x03 },
	{ 0x6, 0x7F, 0x35, 0x00, 0x0A }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
/* also the embedded panel's above 5.4 GHz */
static const struct combo_level adlp_dp_hbr2_hbr3[] = {
	{ 0xA, 0x35, 0x3F, 0x00, 0x00 }, { 0xA, 0x4F, 0x37, 0x00, 0x08 },
	{ 0xC, 0x71, 0x30, 0x00, 0x0F }, { 0x6, 0x7F, 0x2B, 0x00, 0x14 },
	{ 0xA, 0x4C, 0x3F, 0x00, 0x00 }, { 0xC, 0x73, 0x34, 0x00, 0x0B },
	{ 0x6, 0x7F, 0x30, 0x00, 0x0F }, { 0xC, 0x63, 0x3F, 0x00, 0x00 },
	{ 0x6, 0x7F, 0x38, 0x00, 0x07 }, { 0x6, 0x7F, 0x3F, 0x00, 0x00 },
};
static const struct combo_level adlp_edp_up_to_hbr2[] = {
	{ 0x4, 0x50, 0x38, 0x00, 0x07 }, { 0x4, 0x58, 0x35, 0x00, 0x0A },
	{ 0x4, 0x60, 0x34, 0x00, 0x0B }, { 0x4, 0x6A, 0x32, 0x00, 0x0D },
	{ 0x4, 0x5E, 0x38, 0x00, 0x07 }, { 0x4, 0x61, 0x36, 0x00, 0x09 },
	{ 0x4, 0x6B, 0x34, 0x00, 0x0B }, { 0x4, 0x69, 0x39, 0x00, 0x06 },
	{ 0x4, 0x73, 0x37, 0x00, 0x08 }, { 0x4, 0x7A, 0x38, 0x00, 0x07 },
};

#define T(t) do { *n = N(t); return t; } while (0)

/* The port clock the levels go with: the link rate, or the TMDS clock
 * of the PLL the HDMI port runs from. */
static uint32_t port_clock(struct i915_device *i915, const struct intel_output *o, int hdmi)
{
	if (!hdmi)
		return o->link_rate_khz;
	if (o->pll >= 0 && o->pll < INTEL_MAX_DPLLS)
		return i915->display.dpll[o->pll].link_rate_khz;
	return 0;
}

static const struct combo_level *level_table(struct i915_device *i915,
					     const struct intel_output *o, uint32_t clock, int *n)
{
	int hdmi = o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI;
	int edp = o->is_edp;
	int low_vswing = edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std;

	if (hdmi)
		T(icl_hdmi);
	if (plat(i915) == I915_PLATFORM_JASPERLAKE) {
		if (low_vswing) {
			if (clock > 270000)
				T(jsl_edp_hbr2);
			T(jsl_edp_hbr);
		}
		T(icl_dp_hbr2_edp_hbr3);
	}
	if (plat(i915) == I915_PLATFORM_ELKHARTLAKE) {
		if (low_vswing) {
			if (clock > 270000)
				T(ehl_edp_hbr2);
			T(icl_edp_hbr2);
		}
		T(ehl_dp);
	}
	if (!gen12_plus(i915)) {
		/* Ice Lake */
		if (edp && clock > 540000)
			T(icl_dp_hbr2_edp_hbr3);
		if (low_vswing)
			T(icl_edp_hbr2);
		T(icl_dp_hbr2_edp_hbr3);
	}
	if (is_adlp(i915)) {
		if (edp && clock > 540000)
			T(adlp_dp_hbr2_hbr3);
		if (low_vswing)
			T(adlp_edp_up_to_hbr2);
		if (clock > 270000)
			T(adlp_dp_hbr2_hbr3);
		T(adlp_dp_hbr);
	}
	if (is_adls(i915)) {
		if (edp && clock > 540000)
			T(adls_edp_hbr3);
		if (low_vswing)
			T(adls_edp_hbr2);
		if (clock > 270000)
			T(adls_dp_hbr2_hbr3);
		T(tgl_dp_hbr);
	}
	/* Tiger Lake, Rocket Lake, DG1: above 5.4 GHz a panel takes Ice
	 * Lake's table */
	if (edp && clock > 540000)
		T(icl_dp_hbr2_edp_hbr3);
	if (low_vswing)
		T(icl_edp_hbr2);
	if (is_rkl(i915)) {
		if (clock > 270000)
			T(rkl_dp_hbr2_hbr3);
		T(rkl_dp_hbr);
	}
	if (is_dg1(i915)) {
		if (clock > 270000)
			T(dg1_dp_hbr2_hbr3);
		T(dg1_dp_rbr_hbr);
	}
	if (clock > 270000) {
		if (is_tgl_uy(i915))
			T(tgl_uy_dp_hbr2);
		T(tgl_dp_hbr2);
	}
	T(tgl_dp_hbr);
}

#undef T

/* LOADGEN_SELECT by lane: up to 6 GHz on lanes 1-3 of a four-lane link,
 * lanes 1-2 of a narrower one; above it on none. */
static uint32_t loadgen_select(uint32_t clock, int lanes, int lane)
{
	if (clock > 600000)
		return 0;
	if (lanes == 4)
		return lane >= 1 ? LOADGEN_SELECT : 0;
	return (lane == 1 || lane == 2) ? LOADGEN_SELECT : 0;
}

/* Write one level into the PHY: keeper, loadgen, the sus clock,
 * training off, the values, training on (which triggers the update). */
void icl_combo_set_signal_level(struct i915_device *i915, struct intel_output *o,
				int level, int lanes)
{
	int phy = phy_of(i915, o->port);
	int hdmi = o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI;
	uint32_t clock = port_clock(i915, o, hdmi);
	const struct combo_level *t, *l;
	uint32_t v;
	int n;

	if (!is_combo_phy(i915, phy))
		return;
	t = level_table(i915, o, clock, &n);
	if (hdmi) {
		lanes = 4;
		if (level < 0 || level >= n)
			level = n - 1; /* the table's default */
	} else if (level < 0) {
		level = 0;
	} else if (level >= n) {
		level = n - 1;
	}
	if (lanes != 1 && lanes != 2)
		lanes = 4;
	l = &t[level];

	/* 1. the common keeper on for DisplayPort, off for HDMI */
	v = i915_read32(i915, ICL_PORT_PCS_DW_LN(1, 0, phy));
	if (hdmi)
		v &= ~COMMON_KEEPER_EN;
	else
		v |= COMMON_KEEPER_EN;
	i915_write32(i915, ICL_PORT_PCS_DW_GRP(1, phy), v);
	/* 2. the load generator, per lane */
	for (int ln = 0; ln < 4; ln++)
		rmw(i915, ICL_PORT_TX_DW_LN(4, ln, phy), LOADGEN_SELECT,
		    loadgen_select(clock, lanes, ln));
	/* 3. the sus clock */
	rmw(i915, ICL_PORT_CL_DW(5, phy), 0, SUS_CLOCK_CONFIG);
	/* 4. training off while the values change */
	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v & ~TX_TRAINING_EN);
	/* 5. the values; an embedded panel's 4K2K override stays off (the
	 * high-efficiency mode it belongs to is not used) */
	if (o->is_edp)
		rmw(i915, ICL_PORT_CL_DW(10, phy),
		    ICL_COMBO_EDP4K2K_MODE_OVRD_EN | ICL_COMBO_EDP4K2K_MODE_OVRD_OPTIMIZED, 0);
	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	v &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK | ICL_COMBO_COEFF_POLARITY |
	       ICL_COMBO_CURSOR_PROGRAM | TAP2_DISABLE | TAP3_DISABLE);
	v |= SCALING_MODE_SEL(0x2) | RTERM_SELECT(0x6) | TAP3_DISABLE;
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v);
	for (int ln = 0; ln < 4; ln++)
		rmw(i915, ICL_PORT_TX_DW_LN(2, ln, phy),
		    SWING_SEL_UPPER_MASK | SWING_SEL_LOWER_MASK | RCOMP_SCALAR_MASK,
		    SWING_SEL_UPPER(l->dw2_swing_sel) | SWING_SEL_LOWER(l->dw2_swing_sel) |
			    RCOMP_SCALAR(0x98));
	/* lane by lane: the group write would undo the loadgen selects */
	for (int ln = 0; ln < 4; ln++)
		rmw(i915, ICL_PORT_TX_DW_LN(4, ln, phy),
		    POST_CURSOR_1_MASK | POST_CURSOR_2_MASK | CURSOR_COEFF_MASK,
		    POST_CURSOR_1(l->dw4_post_cursor_1) | POST_CURSOR_2(l->dw4_post_cursor_2) |
			    CURSOR_COEFF(l->dw4_cursor_coeff));
	for (int ln = 0; ln < 4; ln++)
		rmw(i915, ICL_PORT_TX_DW_LN(7, ln, phy), N_SCALAR_MASK, N_SCALAR(l->dw7_n_scalar));
	/* 6. training on: the PHY takes the values */
	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v | TX_TRAINING_EN);
	(void)i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
}
