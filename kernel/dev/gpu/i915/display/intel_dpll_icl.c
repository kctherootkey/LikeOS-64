// LikeOS -- the combo PHY ports of Ice Lake and later: their PLLs,
// the PHY itself, and its signal levels.
//
// A combo port (DDI A, B and -- on some parts -- C, D, E) has its PLL
// among the shared DPLL0/DPLL1 (plus DPLL4 on a few parts), programmed
// with a DCO frequency and three dividers (intel_dpll_calc.c works them
// out); the port is routed to a PLL through DPCLKA_CFGCR0.  The PHY has
// a register file of its own per port: a one-time initialisation (which
// the firmware has usually done for the panel's port, and which is then
// left alone), lane power, and the voltage-swing/pre-emphasis levels,
// which are not a translation table in the DDI here but a set of PHY
// registers written per level.  Type-C ports are another matter
// (intel_tc.c).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dpll_calc.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* The reference the PLLs run from, from DSSM. */
static uint32_t icl_ref_khz(struct i915_device *i915)
{
	switch (i915_read32(i915, DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: return 19200;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: return 38400;
	default: return 24000;
	}
}

static uint32_t cfgcr0_reg(struct i915_device *i915, int pll)
{
	return i915->display.model == INTEL_DISPLAY_TGL ? TGL_DPLL_CFGCR0(pll) :
							   ICL_DPLL_CFGCR0(pll);
}

static uint32_t cfgcr1_reg(struct i915_device *i915, int pll)
{
	return i915->display.model == INTEL_DISPLAY_TGL ? TGL_DPLL_CFGCR1(pll) :
							   ICL_DPLL_CFGCR1(pll);
}

/* Which combo PHY a port is (its index into the PHY register bases). */
static int phy_of(struct i915_device *i915, int port)
{
	(void)i915;
	return port; /* A=0, B=1, C=2 (EHL), D=3 (RKL), E=4 (ADL-S) */
}

#define ICL_NUM_COMBO_PLLS 2

int icl_dpll_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	for (int i = 0; i < INTEL_MAX_DPLLS; i++) {
		d->dpll[i].in_use = 0;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].link_rate_khz = 0;
	}
	i915_dbg("[drm] i915: combo PLLs: DPLL0 %08x/%08x DPLL1 %08x/%08x, DPCLKA %08x, ref %u kHz\n",
		 i915_read32(i915, ICL_DPLL_ENABLE(0)), i915_read32(i915, cfgcr0_reg(i915, 0)),
		 i915_read32(i915, ICL_DPLL_ENABLE(1)), i915_read32(i915, cfgcr0_reg(i915, 1)),
		 i915_read32(i915, ICL_DPCLKA_CFGCR0), icl_ref_khz(i915));
	return 0;
}

static int pll_wait(struct i915_device *i915, uint32_t reg, uint32_t bit, int set,
		    uint32_t timeout_us)
{
	for (uint32_t t = 0; t < timeout_us; t += 10) {
		if (!!(i915_read32(i915, reg) & bit) == set)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static void pll_disable(struct i915_device *i915, int pll)
{
	uint32_t reg = ICL_DPLL_ENABLE(pll);
	uint32_t v = i915_read32(i915, reg);
	if (v & PLL_ENABLE) {
		i915_write32(i915, reg, v & ~PLL_ENABLE);
		pll_wait(i915, reg, PLL_LOCK, 0, 1000);
	}
	v = i915_read32(i915, reg);
	if (v & PLL_POWER_ENABLE) {
		i915_write32(i915, reg, v & ~PLL_POWER_ENABLE);
		pll_wait(i915, reg, PLL_POWER_STATE, 0, 1000);
	}
}

/* Power up, program, enable, wait for lock. */
static int pll_program(struct i915_device *i915, int pll, uint32_t cfgcr0, uint32_t cfgcr1)
{
	uint32_t reg = ICL_DPLL_ENABLE(pll);
	pll_disable(i915, pll);
	i915_write32(i915, reg, i915_read32(i915, reg) | PLL_POWER_ENABLE);
	if (pll_wait(i915, reg, PLL_POWER_STATE, 1, 1000) != 0) {
		kprintf("[drm] i915: DPLL%d did not power up\n", pll);
		return -EIO;
	}
	i915_write32(i915, cfgcr0_reg(i915, pll), cfgcr0);
	i915_write32(i915, cfgcr1_reg(i915, pll), cfgcr1);
	(void)i915_read32(i915, cfgcr1_reg(i915, pll));
	i915_write32(i915, reg, i915_read32(i915, reg) | PLL_ENABLE);
	if (pll_wait(i915, reg, PLL_LOCK, 1, 5000) != 0) {
		kprintf("[drm] i915: DPLL%d did not lock (%08x/%08x)\n", pll, cfgcr0, cfgcr1);
		return -EIO;
	}
	return 0;
}

static uint32_t cfgcr1_of(struct i915_device *i915, const struct icl_pll_params *p)
{
	uint32_t v = DPLL_CFGCR1_QDIV_RATIO(p->qdiv_ratio) | DPLL_CFGCR1_QDIV_MODE(p->qdiv_mode) |
		     DPLL_CFGCR1_KDIV(p->kdiv) | DPLL_CFGCR1_PDIV(p->pdiv);
	if (i915->display.model == INTEL_DISPLAY_TGL)
		v |= TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL;
	else
		v |= DPLL_CFGCR1_CENTRAL_FREQ_8400;
	return v;
}

static uint32_t link_rate_field(uint32_t link_khz)
{
	switch (link_khz) {
	case 540000: return DPLL_CFGCR0_LINK_RATE_2700;
	case 270000: return DPLL_CFGCR0_LINK_RATE_1350;
	case 162000: return DPLL_CFGCR0_LINK_RATE_810;
	case 324000: return DPLL_CFGCR0_LINK_RATE_1620;
	case 216000: return DPLL_CFGCR0_LINK_RATE_1080;
	case 432000: return DPLL_CFGCR0_LINK_RATE_2160;
	case 648000: return DPLL_CFGCR0_LINK_RATE_3240;
	case 810000: return DPLL_CFGCR0_LINK_RATE_4050;
	default: return 0xffffffffu;
	}
}

int icl_dpll_rate_supported(uint32_t link_rate_khz)
{
	return link_rate_field(link_rate_khz) != 0xffffffffu;
}

int icl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc)
{
	struct intel_display *d = &i915->display;
	struct icl_pll_params p;
	uint32_t rate = link_rate_field(link_rate_khz);
	(void)port;

	if (rate == 0xffffffffu || icl_combo_dp_params(link_rate_khz, icl_ref_khz(i915), &p))
		return -EINVAL;
	for (int i = 0; i < ICL_NUM_COMBO_PLLS; i++) {
		if (d->dpll[i].in_use && !d->dpll[i].is_hdmi &&
		    d->dpll[i].link_rate_khz == link_rate_khz && d->dpll[i].ssc == ssc) {
			d->dpll[i].in_use++;
			return i;
		}
	}
	for (int i = 0; i < ICL_NUM_COMBO_PLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		uint32_t cfgcr0 = rate | DPLL_CFGCR0_DCO_FRACTION(p.dco_fraction) | p.dco_integer;
		if (ssc)
			cfgcr0 |= DPLL_CFGCR0_SSC_ENABLE_ICL;
		if (pll_program(i915, i, cfgcr0, cfgcr1_of(i915, &p)) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 0;
		d->dpll[i].ssc = ssc;
		d->dpll[i].link_rate_khz = link_rate_khz;
		d->dpll[i].cfgcr0 = cfgcr0;
		i915_dbg("[drm] i915: DPLL%d at %u kHz for port %s\n", i, link_rate_khz,
			 intel_port_name(i915, port));
		return i;
	}
	return -ENOSPC;
}

int icl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	struct icl_pll_params p;
	(void)port;

	if (icl_combo_hdmi_params(clock_khz, icl_ref_khz(i915), &p)) {
		kprintf("[drm] i915: no combo PLL dividers for %u kHz\n", clock_khz);
		return -EINVAL;
	}
	for (int i = 0; i < ICL_NUM_COMBO_PLLS; i++) {
		if (d->dpll[i].in_use)
			continue;
		uint32_t cfgcr0 = DPLL_CFGCR0_HDMI_MODE | DPLL_CFGCR0_DCO_FRACTION(p.dco_fraction) |
				  p.dco_integer;
		if (pll_program(i915, i, cfgcr0, cfgcr1_of(i915, &p)) != 0)
			return -EIO;
		d->dpll[i].in_use = 1;
		d->dpll[i].is_hdmi = 1;
		d->dpll[i].ssc = 0;
		d->dpll[i].link_rate_khz = clock_khz;
		d->dpll[i].cfgcr0 = cfgcr0;
		return i;
	}
	return -ENOSPC;
}

void icl_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (pll < 0 || pll >= ICL_NUM_COMBO_PLLS)
		return;
	if (d->dpll[pll].in_use > 0)
		d->dpll[pll].in_use--;
	if (d->dpll[pll].in_use)
		return;
	pll_disable(i915, pll);
}

/* The port takes its clock from the PLL: select it, then ungate. */
void icl_dpll_route_port(struct i915_device *i915, int port, int pll)
{
	int phy = phy_of(i915, port);
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	v &= ~ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy);
	v |= ICL_DPCLKA_CFGCR0_DDI_CLK_SEL((uint32_t)pll, phy);
	i915_write32(i915, ICL_DPCLKA_CFGCR0, v);
	(void)i915_read32(i915, ICL_DPCLKA_CFGCR0);
	v &= ~ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy);
	i915_write32(i915, ICL_DPCLKA_CFGCR0, v);
	(void)i915_read32(i915, ICL_DPCLKA_CFGCR0);
}

void icl_dpll_unroute_port(struct i915_device *i915, int port)
{
	int phy = phy_of(i915, port);
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	i915_write32(i915, ICL_DPCLKA_CFGCR0, v | ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy));
	(void)i915_read32(i915, ICL_DPCLKA_CFGCR0);
}

/* The rate of the PLL the port is routed to, or 0 (the firmware's
 * state, read before the first mode set). */
uint32_t icl_dpll_port_link_rate(struct i915_device *i915, int port)
{
	int phy = phy_of(i915, port);
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	if (v & ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy))
		return 0;
	int pll = (int)((v & ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy)) >> (phy * 2));
	if (pll >= ICL_NUM_COMBO_PLLS)
		return 0;
	if (!(i915_read32(i915, ICL_DPLL_ENABLE(pll)) & PLL_LOCK))
		return 0;
	uint32_t cfgcr0 = i915_read32(i915, cfgcr0_reg(i915, pll));
	if (cfgcr0 & DPLL_CFGCR0_HDMI_MODE)
		return 0;
	switch (cfgcr0 & DPLL_CFGCR0_LINK_RATE_MASK) {
	case DPLL_CFGCR0_LINK_RATE_2700: return 540000;
	case DPLL_CFGCR0_LINK_RATE_1350: return 270000;
	case DPLL_CFGCR0_LINK_RATE_810: return 162000;
	case DPLL_CFGCR0_LINK_RATE_1620: return 324000;
	case DPLL_CFGCR0_LINK_RATE_1080: return 216000;
	case DPLL_CFGCR0_LINK_RATE_2160: return 432000;
	case DPLL_CFGCR0_LINK_RATE_3240: return 648000;
	case DPLL_CFGCR0_LINK_RATE_4050: return 810000;
	default: return 0;
	}
}

/* ---- the PHY ------------------------------------------------------------------- */

/* Process/voltage compensation values, by what the PHY reports itself
 * to be. */
struct procmon {
	uint32_t dw1, dw9, dw10;
};

static const struct procmon procmon_values[] = {
	{ 0x00000000, 0x62AB67BB, 0x51914F96 }, /* 0.85 V, dot 0 */
	{ 0x00000000, 0x86E172C7, 0x77CA5EAB }, /* 0.95 V, dot 0 */
	{ 0x00000000, 0x93F87FE1, 0x8AE871C5 }, /* 0.95 V, dot 1 */
	{ 0x00000000, 0x98FA82DD, 0x89E46DC1 }, /* 1.05 V, dot 0 */
	{ 0x00440000, 0x9A00AB25, 0x8AE38FF1 }, /* 1.05 V, dot 1 */
};

static const struct procmon *procmon_for(uint32_t dw3)
{
	uint32_t process = (dw3 & PROCESS_INFO_MASK) >> PROCESS_INFO_SHIFT;
	uint32_t voltage = (dw3 & VOLTAGE_INFO_MASK) >> VOLTAGE_INFO_SHIFT;
	switch (voltage << 4 | process) {
	case 0x00: return &procmon_values[0];
	case 0x10: return &procmon_values[1];
	case 0x11: return &procmon_values[2];
	case 0x20: return &procmon_values[3];
	case 0x21: return &procmon_values[4];
	default: return NULL;
	}
}

/* Is the PHY initialised (the firmware does it for the ports it lit)?
 * If not, do it: the compensation values, the reference generator, the
 * lane power-down enable. */
int icl_combo_phy_init(struct i915_device *i915, int port)
{
	int phy = phy_of(i915, port);
	uint32_t dw0 = i915_read32(i915, ICL_PORT_COMP_DW(0, phy));

	if (dw0 & COMP_INIT)
		return 0;
	i915_dbg("[drm] i915: initialising combo PHY %d\n", phy);
	if (i915->display.model == INTEL_DISPLAY_ICL && port <= PORT_B) {
		uint32_t misc = i915_read32(i915, ICL_PHY_MISC(port));
		i915_write32(i915, ICL_PHY_MISC(port), misc & ~ICL_PHY_MISC_DE_IO_COMP_PWR_DOWN);
	}
	const struct procmon *pm = procmon_for(i915_read32(i915, ICL_PORT_COMP_DW(3, phy)));
	if (!pm) {
		kprintf("[drm] i915: combo PHY %d reports a process/voltage this driver has no values for\n",
			phy);
		return -ENODEV;
	}
	uint32_t dw1 = i915_read32(i915, ICL_PORT_COMP_DW(1, phy));
	i915_write32(i915, ICL_PORT_COMP_DW(1, phy), (dw1 & 0xffff) | pm->dw1);
	i915_write32(i915, ICL_PORT_COMP_DW(9, phy), pm->dw9);
	i915_write32(i915, ICL_PORT_COMP_DW(10, phy), pm->dw10);
	uint32_t dw8 = i915_read32(i915, ICL_PORT_COMP_DW(8, phy));
	i915_write32(i915, ICL_PORT_COMP_DW(8, phy), dw8 | IREFGEN);
	i915_write32(i915, ICL_PORT_COMP_DW(0, phy), dw0 | COMP_INIT);
	uint32_t cl5 = i915_read32(i915, ICL_PORT_CL_DW(5, phy));
	i915_write32(i915, ICL_PORT_CL_DW(5, phy), cl5 | CL_POWER_DOWN_ENABLE);
	(void)i915_read32(i915, ICL_PORT_CL_DW(5, phy));
	return 0;
}

/* The lanes the port will drive stay powered; the others go down. */
void icl_combo_phy_power_lanes(struct i915_device *i915, int port, int lanes, int reversed)
{
	int phy = phy_of(i915, port);
	uint32_t down;
	switch (lanes) {
	case 1: down = reversed ? 0x7 : 0xe; break;
	case 2: down = reversed ? 0x3 : 0xc; break;
	default: down = 0; break;
	}
	uint32_t v = i915_read32(i915, ICL_PORT_CL_DW(10, phy));
	v &= ~PWR_DOWN_LN_MASK;
	v |= PWR_DOWN_LN(down);
	i915_write32(i915, ICL_PORT_CL_DW(10, phy), v);
	(void)i915_read32(i915, ICL_PORT_CL_DW(10, phy));
}

/* Signal levels: swing selector (DW2), scalar (DW7), cursor and second
 * post-cursor coefficients (DW4).  The DisplayPort tables are ordered
 * 400mV x4, 600mV x3, 800mV x2, 1200mV x1 like the Skylake ones. */
struct combo_level {
	uint8_t dw2_swing_sel, dw7_n_scalar, dw4_cursor_coeff, dw4_post_cursor_2;
};

static const struct combo_level icl_dp_hbr2[] = {
	{ 0xA, 0x35, 0x3F, 0x00 }, { 0xA, 0x4F, 0x37, 0x08 }, { 0xC, 0x71, 0x2F, 0x10 },
	{ 0x6, 0x7F, 0x2B, 0x14 }, { 0xA, 0x4C, 0x3F, 0x00 }, { 0xC, 0x73, 0x34, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x10 }, { 0xC, 0x6C, 0x3C, 0x03 }, { 0x6, 0x7F, 0x35, 0x0A },
	{ 0x6, 0x7F, 0x3F, 0x00 },
};
static const struct combo_level icl_edp_low[] = {
	{ 0x0, 0x7F, 0x3F, 0x00 }, { 0x8, 0x7F, 0x38, 0x07 }, { 0x1, 0x7F, 0x33, 0x0C },
	{ 0x9, 0x7F, 0x31, 0x0E }, { 0x8, 0x7F, 0x3F, 0x00 }, { 0x1, 0x7F, 0x38, 0x07 },
	{ 0x9, 0x7F, 0x35, 0x0A }, { 0x1, 0x7F, 0x3F, 0x00 }, { 0x9, 0x7F, 0x38, 0x07 },
	{ 0x9, 0x7F, 0x3F, 0x00 },
};
static const struct combo_level icl_hdmi[] = {
	{ 0xA, 0x60, 0x3F, 0x00 }, { 0xB, 0x73, 0x36, 0x09 }, { 0x6, 0x7F, 0x31, 0x0E },
	{ 0xB, 0x73, 0x3F, 0x00 }, { 0x6, 0x7F, 0x37, 0x08 }, { 0x6, 0x7F, 0x3F, 0x00 },
	{ 0x6, 0x7F, 0x35, 0x0A },
};
static const struct combo_level tgl_dp_hbr[] = {
	{ 0xA, 0x32, 0x3F, 0x00 }, { 0xA, 0x4F, 0x37, 0x08 }, { 0xC, 0x71, 0x2F, 0x10 },
	{ 0x6, 0x7D, 0x2B, 0x14 }, { 0xA, 0x4C, 0x3F, 0x00 }, { 0xC, 0x73, 0x34, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x10 }, { 0xC, 0x6C, 0x3C, 0x03 }, { 0x6, 0x7F, 0x35, 0x0A },
	{ 0x6, 0x7F, 0x3F, 0x00 },
};
static const struct combo_level tgl_dp_hbr2[] = {
	{ 0xA, 0x35, 0x3F, 0x00 }, { 0xA, 0x4F, 0x37, 0x08 }, { 0xC, 0x63, 0x2F, 0x10 },
	{ 0x6, 0x7F, 0x2B, 0x14 }, { 0xA, 0x47, 0x3F, 0x00 }, { 0xC, 0x63, 0x34, 0x0B },
	{ 0x6, 0x7F, 0x2F, 0x10 }, { 0xC, 0x61, 0x3C, 0x03 }, { 0x6, 0x7B, 0x35, 0x0A },
	{ 0x6, 0x7F, 0x3F, 0x00 },
};
#define ICL_HDMI_DEFAULT_LEVEL 3

static const struct combo_level *level_table(struct i915_device *i915,
					     const struct intel_output *o, int *n)
{
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) {
		*n = (int)(sizeof(icl_hdmi) / sizeof(icl_hdmi[0]));
		return icl_hdmi;
	}
	if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std) {
		*n = 10;
		return icl_edp_low;
	}
	if (i915->display.model == INTEL_DISPLAY_TGL) {
		*n = 10;
		return o->link_rate_khz > 270000 ? tgl_dp_hbr2 : tgl_dp_hbr;
	}
	*n = 10;
	return icl_dp_hbr2;
}

/* Write one level into the PHY: the sequence the hardware wants --
 * training mode off, the loadgen selects, the sus clock, the values,
 * training mode on. */
void icl_combo_set_signal_level(struct i915_device *i915, struct intel_output *o,
				int level, int lanes)
{
	int phy = phy_of(i915, o->port);
	int n;
	const struct combo_level *t = level_table(i915, o, &n);
	int is_dp = (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);

	if (level < 0 || level >= n)
		level = is_dp ? 0 : ICL_HDMI_DEFAULT_LEVEL;
	const struct combo_level *l = &t[level];

	uint32_t v = i915_read32(i915, ICL_PORT_PCS_DW_LN(1, 0, phy));
	if (is_dp)
		v |= COMMON_KEEPER_EN;
	else
		v &= ~COMMON_KEEPER_EN;
	i915_write32(i915, ICL_PORT_PCS_DW_GRP(1, phy), v);

	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v & ~TX_TRAINING_EN);

	/* the load generator on the lanes that need it at low rates */
	for (int ln = 0; ln < 4; ln++) {
		uint32_t dw4 = i915_read32(i915, ICL_PORT_TX_DW_LN(4, ln, phy));
		dw4 &= ~LOADGEN_SELECT;
		if (is_dp && o->link_rate_khz <= 600000 &&
		    ((lanes == 4 && ln >= 1) || (lanes < 4 && (ln == 1 || ln == 2))))
			dw4 |= LOADGEN_SELECT;
		i915_write32(i915, ICL_PORT_TX_DW_LN(4, ln, phy), dw4);
	}
	v = i915_read32(i915, ICL_PORT_CL_DW(5, phy));
	i915_write32(i915, ICL_PORT_CL_DW(5, phy), v | SUS_CLOCK_CONFIG);

	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	v &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK | TAP2_DISABLE | TAP3_DISABLE);
	v |= SCALING_MODE_SEL(0x2) | TAP2_DISABLE | TAP3_DISABLE | RTERM_SELECT(0x6);
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v);

	v = i915_read32(i915, ICL_PORT_TX_DW_LN(2, 0, phy));
	v &= ~(SWING_SEL_LOWER_MASK | SWING_SEL_UPPER_MASK | RCOMP_SCALAR_MASK);
	v |= SWING_SEL_UPPER(l->dw2_swing_sel) | SWING_SEL_LOWER(l->dw2_swing_sel) |
	     RCOMP_SCALAR(0x98);
	i915_write32(i915, ICL_PORT_TX_DW_GRP(2, phy), v);

	for (int ln = 0; ln < 4; ln++) {
		uint32_t dw4 = i915_read32(i915, ICL_PORT_TX_DW_LN(4, ln, phy));
		dw4 &= ~(POST_CURSOR_1_MASK | POST_CURSOR_2_MASK | CURSOR_COEFF_MASK);
		dw4 |= POST_CURSOR_1(0) | POST_CURSOR_2(l->dw4_post_cursor_2) |
		       CURSOR_COEFF(l->dw4_cursor_coeff);
		i915_write32(i915, ICL_PORT_TX_DW_LN(4, ln, phy), dw4);
	}
	v = i915_read32(i915, ICL_PORT_TX_DW_LN(7, 0, phy));
	v &= ~N_SCALAR_MASK;
	v |= N_SCALAR(l->dw7_n_scalar);
	i915_write32(i915, ICL_PORT_TX_DW_GRP(7, phy), v);

	v = i915_read32(i915, ICL_PORT_TX_DW_LN(5, 0, phy));
	i915_write32(i915, ICL_PORT_TX_DW_GRP(5, phy), v | TX_TRAINING_EN);
	(void)i915_read32(i915, ICL_PORT_TX_DW_GRP(5, phy));
}
