// LikeOS -- Type-C ports (Ice Lake and Tiger Lake).
//
// A Type-C port's PHY sits behind the flexible I/O adapter (FIA), which
// muxes the connector between a DisplayPort alternate-mode sink, the
// Thunderbolt controller and a legacy (fixed) connector.  Before the
// port can be driven the display must take the PHY from the FIA, learn
// which mode the connector is in and how many lanes it was granted.
// The port's clock is then either its own MG PLL (Ice Lake) / DKL PLL
// (Tiger Lake) in the PHY, or the shared Thunderbolt PLL.  The signal
// levels are register writes into the PHY's per-lane words.
//
// None of this has run on hardware: the ThinkPad P50 this driver was
// written on has no Type-C PHY.  The sequences follow the programming
// guides; every step reports what it found.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

static int is_dkl(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_TGL;
}

/* The FIA that carries the port: one FIA for all ports, or ("modular")
 * one per pair. */
static void fia_of(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPSP(0));
	if (v & MODULAR_FIA_MASK) {
		o->tc_fia = o->tc_index / 2;
		o->tc_fia_idx = o->tc_index % 2;
	} else {
		o->tc_fia = 0;
		o->tc_fia_idx = o->tc_index;
	}
}

/* The DKL registers are banked: the index register selects which lane
 * (or, for 2, the PLL) a read or write reaches. */
static void dkl_bank(struct i915_device *i915, int tc, int bank)
{
	i915_write32(i915, HIP_INDEX_REG(tc), HIP_INDEX_VAL(tc, bank));
}

static uint32_t dkl_read(struct i915_device *i915, int tc, int bank, uint32_t reg)
{
	dkl_bank(i915, tc, bank);
	return i915_read32(i915, reg);
}

static void dkl_write(struct i915_device *i915, int tc, int bank, uint32_t reg, uint32_t v)
{
	dkl_bank(i915, tc, bank);
	i915_write32(i915, reg, v);
}

/* ---- the connector's mode ------------------------------------------------------ */

static int live_mode(struct i915_device *i915, struct intel_output *o)
{
	uint32_t sp = i915_read32(i915, PORT_TX_DFLEXDPSP(o->tc_fia));
	uint32_t sde = i915_read32(i915, SDEISR);
	if (sp == 0xffffffffu)
		return INTEL_TC_DISCONNECTED; /* the FIA is powered down */
	if (sp & TC_LIVE_STATE_TC(o->tc_fia_idx))
		return INTEL_TC_DP_ALT;
	if (sp & TC_LIVE_STATE_TBT(o->tc_fia_idx))
		return INTEL_TC_TBT_ALT;
	if (sde & SDE_TC_HOTPLUG_ICP(o->tc_index))
		return INTEL_TC_LEGACY;
	return INTEL_TC_DISCONNECTED;
}

static int phy_ready(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPPMS(o->tc_fia));
	if (v == 0xffffffffu)
		return 0;
	return !!(v & DP_PHY_MODE_STATUS_COMPLETED(o->tc_fia_idx));
}

static void take_ownership(struct i915_device *i915, struct intel_output *o, int take)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPCSSS(o->tc_fia));
	if (v == 0xffffffffu)
		return;
	if (take)
		v |= DP_PHY_MODE_STATUS_NOT_SAFE(o->tc_fia_idx);
	else
		v &= ~DP_PHY_MODE_STATUS_NOT_SAFE(o->tc_fia_idx);
	i915_write32(i915, PORT_TX_DFLEXDPCSSS(o->tc_fia), v);
}

/* The lanes the FIA granted in DP-alt mode, as a count. */
static uint32_t lane_mask(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPSP(o->tc_fia));
	return (v & DP_LANE_ASSIGNMENT_MASK(o->tc_fia_idx)) >> DP_LANE_ASSIGNMENT_SHIFT(o->tc_fia_idx);
}

int intel_tc_max_lanes(struct i915_device *i915, struct intel_output *o)
{
	if (o->tc_mode != INTEL_TC_DP_ALT)
		return 4;
	switch (lane_mask(i915, o)) {
	case 0x1: case 0x2: case 0x4: case 0x8: return 1;
	case 0x3: case 0xC: return 2;
	case 0xF: return 4;
	default: return 4;
	}
}

static uint32_t pin_assignment(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXPA1(o->tc_fia));
	return (v & DP_PIN_ASSIGNMENT_MASK(o->tc_fia_idx)) >> DP_PIN_ASSIGNMENT_SHIFT(o->tc_fia_idx);
}

/* Tell the FIA how many of its lanes the port will drive. */
static void set_fia_lanes(struct i915_device *i915, struct intel_output *o, int lanes)
{
	uint32_t v = i915_read32(i915, PORT_TX_DFLEXDPMLE1(o->tc_fia));
	uint32_t field;
	v &= ~DFLEXDPMLE1_DPMLETC_MASK(o->tc_fia_idx);
	switch (lanes) {
	case 1: field = o->lane_reversal ? 0x8 : 0x1; break;
	case 2: field = o->lane_reversal ? 0xC : 0x3; break;
	default: field = 0xF; break;
	}
	v |= DFLEXDPMLE1_DPMLETC(o->tc_fia_idx, field);
	i915_write32(i915, PORT_TX_DFLEXDPMLE1(o->tc_fia), v);
}

/* Take the PHY for the connector's current mode.  Returns 0 with
 * o->tc_mode set, or -ENODEV when nothing is there. */
int intel_tc_connect(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc)
		return 0;
	fia_of(i915, o);
	int mode = live_mode(i915, o);
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
	if (mode == INTEL_TC_TBT_ALT) {
		/* the Thunderbolt controller owns the PHY; the display only
		 * routes the port to the TBT PLL */
		o->tc_mode = mode;
		o->tc_owned = 1;
		o->tc_lanes = 4;
		i915_dbg("[drm] i915: port %s: Type-C in Thunderbolt mode\n",
			 intel_port_name(i915, o->port));
		return 0;
	}
	/* DP-alt or legacy: the PHY must have finished its mode switch */
	int ready = 0;
	for (int t = 0; t < 100; t++) {
		if (phy_ready(i915, o)) {
			ready = 1;
			break;
		}
		lapic_delay_us(100);
	}
	if (!ready && mode == INTEL_TC_DP_ALT) {
		kprintf("[drm] i915: port %s: Type-C PHY never became ready\n",
			intel_port_name(i915, o->port));
		return -ENODEV;
	}
	take_ownership(i915, o, 1);
	o->tc_mode = mode;
	o->tc_owned = 1;
	o->tc_lanes = (uint32_t)intel_tc_max_lanes(i915, o);
	if (mode == INTEL_TC_DP_ALT && live_mode(i915, o) != INTEL_TC_DP_ALT) {
		/* pulled while we were taking it */
		take_ownership(i915, o, 0);
		o->tc_owned = 0;
		o->tc_mode = INTEL_TC_DISCONNECTED;
		return -ENODEV;
	}
	i915_dbg("[drm] i915: port %s: Type-C in %s mode, %u lanes (pin assignment %x)\n",
		 intel_port_name(i915, o->port), mode == INTEL_TC_DP_ALT ? "DP-alt" : "legacy",
		 o->tc_lanes, pin_assignment(i915, o));
	return 0;
}

void intel_tc_disconnect(struct i915_device *i915, struct intel_output *o)
{
	if (!o->is_tc || !o->tc_owned)
		return;
	if (o->tc_mode != INTEL_TC_TBT_ALT)
		take_ownership(i915, o, 0);
	o->tc_owned = 0;
	o->tc_mode = INTEL_TC_DISCONNECTED;
}

/* ---- the MG/DKL PLL ------------------------------------------------------------- */

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

/* The dividers for a port clock: the DCO sits at 8.1 GHz for DP (an
 * exact multiple of every link rate) or between 8 and 10 GHz for HDMI,
 * and is divided by two post dividers from short lists. */
static int tc_pll_divisors(uint32_t clock_khz, int is_dp, int dkl, uint32_t *dco_khz,
			   struct tc_pll *s)
{
	static const uint8_t div1_vals[] = { 7, 5, 3, 2 };
	uint32_t dco_min = is_dp ? 8100000 : 7992000;
	uint32_t dco_max = is_dp ? 8100000 : 10000000;
	for (unsigned i = 0; i < sizeof(div1_vals); i++) {
		uint32_t div1 = div1_vals[i];
		for (int div2 = 10; div2 > 0; div2--) {
			uint32_t dco = div1 * (uint32_t)div2 * clock_khz * 5;
			uint32_t a_divratio, tlinedrv, inputsel, hsdiv;
			if (dco < dco_min || dco > dco_max)
				continue;
			if (div2 >= 2) {
				a_divratio = is_dp ? 10 : 5;
				tlinedrv = dkl ? 1 : 2;
			} else {
				a_divratio = 5;
				tlinedrv = 0;
			}
			inputsel = is_dp ? 0 : 1;
			switch (div1) {
			case 2: hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_2; break;
			case 3: hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_3; break;
			case 5: hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_5; break;
			default: hsdiv = MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_7; break;
			}
			*dco_khz = dco;
			s->refclkin_ctl = MG_REFCLKIN_CTL_OD_2_MUX(1);
			s->coreclkctl1 = MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO(a_divratio);
			s->hsclkctl = MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL(tlinedrv) |
				      MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL(inputsel) | hsdiv |
				      MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO((uint32_t)div2);
			return 0;
		}
	}
	return -EINVAL;
}

static int tc_pll_calc(struct i915_device *i915, uint32_t clock_khz, int is_dp,
		       struct tc_pll *s)
{
	uint32_t refclk = ref_khz(i915);
	int dkl = is_dkl(i915);
	uint32_t dco_khz, m1div, m2div_int, m2div_rem, m2div_frac;
	uint32_t iref_ndiv, iref_trim, iref_pulse_w, prop_coeff, int_coeff;
	uint32_t tdc_targetcnt, feedfwgain;

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
	default: iref_ndiv = 2; iref_trim = 28; iref_pulse_w = 1; break;
	}
	/* tdc_targetcnt = round(2 / (3e-6 * 8 * 50 * 1.1) / refclk_mhz) */
	tdc_targetcnt = (1515150u * 1000u / refclk + 500u) / 1000u;
	if (dco_khz >= 9000000) {
		prop_coeff = 5;
		int_coeff = 10;
	} else {
		prop_coeff = 4;
		int_coeff = 8;
	}
	feedfwgain = m2div_rem ? (uint32_t)((uint64_t)m1div * 1000000u * 100u / (dco_khz * 3 / 10)) : 0;
	if (dkl) {
		s->div0 = DKL_PLL_DIV0_INTEG_COEFF(int_coeff) | DKL_PLL_DIV0_PROP_COEFF(prop_coeff) |
			  DKL_PLL_DIV0_FBPREDIV(m1div) | DKL_PLL_DIV0_FBDIV_INT(m2div_int);
		s->div1 = DKL_PLL_DIV1_IREF_TRIM(iref_trim) | DKL_PLL_DIV1_TDC_TARGET_CNT(tdc_targetcnt);
		s->ssc = DKL_PLL_SSC_IREF_NDIV_RATIO(iref_ndiv) | DKL_PLL_SSC_STEP_LEN(0) |
			 DKL_PLL_SSC_STEP_NUM(4);
		s->bias = (m2div_frac ? DKL_PLL_BIAS_FRAC_EN_H : 0) | DKL_PLL_BIAS_FBDIV_FRAC(m2div_frac);
		s->tdc_coldst_bias = DKL_PLL_TDC_SSC_STEP_SIZE(0) | DKL_PLL_TDC_FEED_FWD_GAIN(feedfwgain);
		s->lf = s->frac_lock = 0;
		s->bias_mask = s->tdc_mask = 0xffffffffu;
		return 0;
	}
	s->div0 = (m2div_rem ? MG_PLL_DIV0_FRACNEN_H : 0) | MG_PLL_DIV0_FBDIV_FRAC(m2div_frac) |
		  MG_PLL_DIV0_FBDIV_INT(m2div_int);
	s->div1 = MG_PLL_DIV1_IREF_NDIVRATIO(iref_ndiv) | MG_PLL_DIV1_DITHER_DIV_2 |
		  MG_PLL_DIV1_NDIVRATIO(1) | MG_PLL_DIV1_FBPREDIV(m1div);
	s->lf = MG_PLL_LF_TDCTARGETCNT(tdc_targetcnt) | MG_PLL_LF_AFCCNTSEL_512 |
		MG_PLL_LF_GAINCTRL(1) | MG_PLL_LF_INT_COEFF(int_coeff) | MG_PLL_LF_PROP_COEFF(prop_coeff);
	s->frac_lock = MG_PLL_FRAC_LOCK_TRUELOCK_CRIT_32 | MG_PLL_FRAC_LOCK_EARLYLOCK_CRIT_32 |
		       MG_PLL_FRAC_LOCK_LOCKTHRESH(10) | MG_PLL_FRAC_LOCK_DCODITHEREN |
		       MG_PLL_FRAC_LOCK_FEEDFWRDGAIN(feedfwgain);
	if (m2div_rem)
		s->frac_lock |= MG_PLL_FRAC_LOCK_FEEDFWRDCAL_EN;
	s->ssc = MG_PLL_SSC_TYPE(2) | MG_PLL_SSC_STEPLENGTH(0) | MG_PLL_SSC_STEPNUM(4) |
		 MG_PLL_SSC_FLLEN | MG_PLL_SSC_STEPSIZE(0);
	s->tdc_coldst_bias = MG_PLL_TDC_COLDST_COLDSTART | MG_PLL_TDC_COLDST_IREFINT_EN |
			     MG_PLL_TDC_COLDST_REFBIAS_START_PULSE_W(iref_pulse_w) |
			     MG_PLL_TDC_TDCOVCCORR_EN | MG_PLL_TDC_TDCSEL(3);
	s->bias = MG_PLL_BIAS_BIAS_GB_SEL(3) | MG_PLL_BIAS_INIT_DCOAMP(0x3F) |
		  MG_PLL_BIAS_BIAS_BONUS(10) | MG_PLL_BIAS_BIASCAL_EN | MG_PLL_BIAS_CTRIM(12) |
		  MG_PLL_BIAS_VREF_RDAC(4) | MG_PLL_BIAS_IREFTRIM(iref_trim);
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

static void mg_pll_write(struct i915_device *i915, int tc, const struct tc_pll *s)
{
	uint32_t v;
	v = i915_read32(i915, MG_REFCLKIN_CTL(tc));
	v = (v & ~MG_REFCLKIN_CTL_OD_2_MUX_MASK) | s->refclkin_ctl;
	i915_write32(i915, MG_REFCLKIN_CTL(tc), v);
	v = i915_read32(i915, MG_CLKTOP2_CORECLKCTL1(tc));
	v = (v & ~MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK) | s->coreclkctl1;
	i915_write32(i915, MG_CLKTOP2_CORECLKCTL1(tc), v);
	v = i915_read32(i915, MG_CLKTOP2_HSCLKCTL(tc));
	v &= ~(MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK | MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK |
	       MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK | MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK);
	i915_write32(i915, MG_CLKTOP2_HSCLKCTL(tc), v | s->hsclkctl);
	i915_write32(i915, MG_PLL_DIV0(tc), s->div0);
	i915_write32(i915, MG_PLL_DIV1(tc), s->div1);
	i915_write32(i915, MG_PLL_LF(tc), s->lf);
	i915_write32(i915, MG_PLL_FRAC_LOCK(tc), s->frac_lock);
	i915_write32(i915, MG_PLL_SSC(tc), s->ssc);
	v = i915_read32(i915, MG_PLL_BIAS(tc));
	v = (v & ~s->bias_mask) | s->bias;
	i915_write32(i915, MG_PLL_BIAS(tc), v);
	v = i915_read32(i915, MG_PLL_TDC_COLDST_BIAS(tc));
	v = (v & ~s->tdc_mask) | s->tdc_coldst_bias;
	i915_write32(i915, MG_PLL_TDC_COLDST_BIAS(tc), v);
	(void)i915_read32(i915, MG_PLL_TDC_COLDST_BIAS(tc));
}

static void dkl_pll_write(struct i915_device *i915, int tc, const struct tc_pll *s)
{
	uint32_t v;
	/* the PLL registers live in bank 2 */
	v = dkl_read(i915, tc, 2, DKL_REFCLKIN_CTL(tc));
	v = (v & ~MG_REFCLKIN_CTL_OD_2_MUX_MASK) | s->refclkin_ctl;
	dkl_write(i915, tc, 2, DKL_REFCLKIN_CTL(tc), v);
	v = dkl_read(i915, tc, 2, DKL_CLKTOP2_CORECLKCTL1(tc));
	v = (v & ~MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK) | s->coreclkctl1;
	dkl_write(i915, tc, 2, DKL_CLKTOP2_CORECLKCTL1(tc), v);
	v = dkl_read(i915, tc, 2, DKL_CLKTOP2_HSCLKCTL(tc));
	v &= ~(MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK | MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK |
	       MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK | MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK);
	dkl_write(i915, tc, 2, DKL_CLKTOP2_HSCLKCTL(tc), v | s->hsclkctl);
	v = dkl_read(i915, tc, 2, DKL_PLL_DIV0(tc));
	v = (v & ~DKL_PLL_DIV0_MASK) | s->div0;
	dkl_write(i915, tc, 2, DKL_PLL_DIV0(tc), v);
	v = dkl_read(i915, tc, 2, DKL_PLL_DIV1(tc));
	v &= ~(DKL_PLL_DIV1_IREF_TRIM_MASK | DKL_PLL_DIV1_TDC_TARGET_CNT_MASK);
	dkl_write(i915, tc, 2, DKL_PLL_DIV1(tc), v | s->div1);
	v = dkl_read(i915, tc, 2, DKL_PLL_SSC(tc));
	v &= ~(DKL_PLL_SSC_IREF_NDIV_RATIO_MASK | DKL_PLL_SSC_STEP_LEN_MASK |
	       DKL_PLL_SSC_STEP_NUM_MASK | DKL_PLL_SSC_EN);
	dkl_write(i915, tc, 2, DKL_PLL_SSC(tc), v | s->ssc);
	v = dkl_read(i915, tc, 2, DKL_PLL_BIAS(tc));
	v &= ~(DKL_PLL_BIAS_FRAC_EN_H | DKL_PLL_BIAS_FBDIV_FRAC_MASK);
	dkl_write(i915, tc, 2, DKL_PLL_BIAS(tc), v | s->bias);
	v = dkl_read(i915, tc, 2, DKL_PLL_TDC_COLDST_BIAS(tc));
	v &= ~(DKL_PLL_TDC_SSC_STEP_SIZE_MASK | DKL_PLL_TDC_FEED_FWD_GAIN_MASK);
	dkl_write(i915, tc, 2, DKL_PLL_TDC_COLDST_BIAS(tc), v | s->tdc_coldst_bias);
	(void)i915_read32(i915, DKL_PLL_TDC_COLDST_BIAS(tc));
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

/* Power, program, enable and lock the port's PLL. */
static int tc_pll_enable(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz,
			 int is_dp)
{
	struct tc_pll s;
	int tc = o->tc_index;
	uint32_t en = MG_PLL_ENABLE(tc);
	if (tc_pll_calc(i915, clock_khz, is_dp, &s)) {
		kprintf("[drm] i915: port %s: no Type-C PLL dividers for %u kHz\n",
			intel_port_name(i915, o->port), clock_khz);
		return -EINVAL;
	}
	i915_write32(i915, en, i915_read32(i915, en) | PLL_POWER_ENABLE);
	if (wait_bit(i915, en, PLL_POWER_STATE, 1, 1)) {
		kprintf("[drm] i915: port %s: Type-C PLL did not power up\n",
			intel_port_name(i915, o->port));
		return -EIO;
	}
	if (is_dkl(i915))
		dkl_pll_write(i915, tc, &s);
	else
		mg_pll_write(i915, tc, &s);
	i915_write32(i915, en, i915_read32(i915, en) | PLL_ENABLE);
	if (wait_bit(i915, en, PLL_LOCK, 1, 1)) {
		kprintf("[drm] i915: port %s: Type-C PLL did not lock\n",
			intel_port_name(i915, o->port));
		return -EIO;
	}
	return 0;
}

static void tc_pll_disable(struct i915_device *i915, int tc)
{
	uint32_t en = MG_PLL_ENABLE(tc);
	i915_write32(i915, en, i915_read32(i915, en) & ~PLL_ENABLE);
	wait_bit(i915, en, PLL_LOCK, 0, 1);
	i915_write32(i915, en, i915_read32(i915, en) & ~PLL_POWER_ENABLE);
	wait_bit(i915, en, PLL_POWER_STATE, 0, 1);
}

/* The Thunderbolt PLL: fixed dividers, shared by every port in TBT mode. */
static int tbt_pll_enable(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	if (d->tbt_pll_users++)
		return 0;
	if (i915_read32(i915, TBT_PLL_ENABLE) & PLL_LOCK)
		return 0; /* running (the firmware's) */
	uint32_t refclk = ref_khz(i915);
	uint32_t cfgcr0 = refclk == 24000 ? DPLL_CFGCR0_DCO_FRACTION(0) | 0x10E :
					    DPLL_CFGCR0_DCO_FRACTION(0x4000) | 0x151;
	uint32_t cfgcr1 = DPLL_CFGCR1_QDIV_RATIO(0) | DPLL_CFGCR1_QDIV_MODE(0) |
			  DPLL_CFGCR1_KDIV(1) | DPLL_CFGCR1_PDIV(4);
	uint32_t r0 = is_dkl(i915) ? TGL_TBTPLL_CFGCR0 : ICL_TBTPLL_CFGCR0;
	uint32_t r1 = is_dkl(i915) ? TGL_TBTPLL_CFGCR1 : ICL_TBTPLL_CFGCR1;
	if (!is_dkl(i915))
		cfgcr1 |= DPLL_CFGCR1_CENTRAL_FREQ_8400;
	i915_write32(i915, TBT_PLL_ENABLE, i915_read32(i915, TBT_PLL_ENABLE) | PLL_POWER_ENABLE);
	if (wait_bit(i915, TBT_PLL_ENABLE, PLL_POWER_STATE, 1, 1))
		return -EIO;
	i915_write32(i915, r0, cfgcr0);
	i915_write32(i915, r1, cfgcr1);
	(void)i915_read32(i915, r1);
	i915_write32(i915, TBT_PLL_ENABLE, i915_read32(i915, TBT_PLL_ENABLE) | PLL_ENABLE);
	if (wait_bit(i915, TBT_PLL_ENABLE, PLL_LOCK, 1, 1)) {
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
	i915_write32(i915, TBT_PLL_ENABLE, i915_read32(i915, TBT_PLL_ENABLE) & ~PLL_ENABLE);
	wait_bit(i915, TBT_PLL_ENABLE, PLL_LOCK, 0, 1);
	i915_write32(i915, TBT_PLL_ENABLE, i915_read32(i915, TBT_PLL_ENABLE) & ~PLL_POWER_ENABLE);
}

/* The PLL id handed back: the port's own slot, or the shared TBT one. */
int intel_tc_dpll_get_dp(struct i915_device *i915, struct intel_output *o,
			 uint32_t link_rate_khz, int ssc)
{
	struct intel_display *d = &i915->display;
	(void)ssc;
	if (!o->tc_owned && intel_tc_connect(i915, o))
		return -ENODEV;
	if (o->tc_mode == INTEL_TC_TBT_ALT) {
		if (link_rate_khz != 162000 && link_rate_khz != 270000 &&
		    link_rate_khz != 540000 && link_rate_khz != 810000)
			return -EINVAL;
		if (tbt_pll_enable(i915))
			return -EIO;
		return INTEL_TBT_PLL_ID;
	}
	int slot = INTEL_TC_PLL_BASE + o->tc_index;
	if (slot >= INTEL_MAX_DPLLS)
		return -ENOSPC;
	if (tc_pll_enable(i915, o, link_rate_khz, 1))
		return -EIO;
	d->dpll[slot].in_use = 1;
	d->dpll[slot].is_hdmi = 0;
	d->dpll[slot].link_rate_khz = link_rate_khz;
	return slot;
}

int intel_tc_dpll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz)
{
	struct intel_display *d = &i915->display;
	if (!o->tc_owned && intel_tc_connect(i915, o))
		return -ENODEV;
	if (o->tc_mode == INTEL_TC_TBT_ALT)
		return -EINVAL; /* no TMDS through Thunderbolt */
	int slot = INTEL_TC_PLL_BASE + o->tc_index;
	if (slot >= INTEL_MAX_DPLLS)
		return -ENOSPC;
	if (tc_pll_enable(i915, o, clock_khz, 0))
		return -EIO;
	d->dpll[slot].in_use = 1;
	d->dpll[slot].is_hdmi = 1;
	d->dpll[slot].link_rate_khz = clock_khz;
	return slot;
}

void intel_tc_dpll_put(struct i915_device *i915, int pll)
{
	struct intel_display *d = &i915->display;
	if (pll == INTEL_TBT_PLL_ID) {
		tbt_pll_put(i915);
		return;
	}
	if (pll < INTEL_TC_PLL_BASE || pll >= INTEL_MAX_DPLLS)
		return;
	d->dpll[pll].in_use = 0;
	tc_pll_disable(i915, pll - INTEL_TC_PLL_BASE);
}

/* ---- the port's clock select ---------------------------------------------------- */

void intel_tc_route_port(struct i915_device *i915, struct intel_output *o, int pll)
{
	uint32_t sel = ICL_DDI_CLK_SEL_MG;
	if (pll == INTEL_TBT_PLL_ID) {
		switch (o->link_rate_khz) {
		case 162000: sel = ICL_DDI_CLK_SEL_TBT_162; break;
		case 270000: sel = ICL_DDI_CLK_SEL_TBT_270; break;
		case 540000: sel = ICL_DDI_CLK_SEL_TBT_540; break;
		default: sel = ICL_DDI_CLK_SEL_TBT_810; break;
		}
	}
	i915_write32(i915, ICL_DDI_CLK_SEL(o->port), sel);
	(void)i915_read32(i915, ICL_DDI_CLK_SEL(o->port));
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	i915_write32(i915, ICL_DPCLKA_CFGCR0, v & ~ICL_DPCLKA_CFGCR0_TC_CLK_OFF(o->tc_index));
	(void)i915_read32(i915, ICL_DPCLKA_CFGCR0);
}

void intel_tc_unroute_port(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	i915_write32(i915, ICL_DPCLKA_CFGCR0, v | ICL_DPCLKA_CFGCR0_TC_CLK_OFF(o->tc_index));
	i915_write32(i915, ICL_DDI_CLK_SEL(o->port), ICL_DDI_CLK_SEL_NONE);
	(void)i915_read32(i915, ICL_DDI_CLK_SEL(o->port));
}

/* What the firmware left the port running at: only a Thunderbolt select
 * names a rate outright; an MG/DKL PLL's rate is not read back. */
uint32_t intel_tc_port_link_rate(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, ICL_DPCLKA_CFGCR0);
	if (v & ICL_DPCLKA_CFGCR0_TC_CLK_OFF(o->tc_index))
		return 0;
	switch (i915_read32(i915, ICL_DDI_CLK_SEL(o->port)) & ICL_DDI_CLK_SEL_MASK) {
	case ICL_DDI_CLK_SEL_TBT_162: return 162000;
	case ICL_DDI_CLK_SEL_TBT_270: return 270000;
	case ICL_DDI_CLK_SEL_TBT_540: return 540000;
	case ICL_DDI_CLK_SEL_TBT_810: return 810000;
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
	if (!o->is_tc || o->tc_mode == INTEL_TC_TBT_ALT)
		return;
	set_fia_lanes(i915, o, lanes);
	if (is_dkl(i915)) {
		ln0 = dkl_read(i915, tc, 0, DKL_DP_MODE(tc));
		ln1 = dkl_read(i915, tc, 1, DKL_DP_MODE(tc));
	} else {
		ln0 = i915_read32(i915, MG_DP_MODE(tc, 0));
		ln1 = i915_read32(i915, MG_DP_MODE(tc, 1));
	}
	ln0 &= ~(MG_DP_MODE_CFG_DP_X1_MODE | MG_DP_MODE_CFG_DP_X2_MODE);
	ln1 &= ~(MG_DP_MODE_CFG_DP_X1_MODE | MG_DP_MODE_CFG_DP_X2_MODE);
	switch (pin_assignment(i915, o)) {
	case 0x0: /* legacy: both lane pairs are the port's */
		if (lanes == 1) {
			ln1 |= MG_DP_MODE_CFG_DP_X1_MODE;
		} else {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x1:
		if (lanes == 4) {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x2:
		if (lanes == 2) {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	case 0x3: case 0x5: case 0x4: case 0x6:
		if (lanes == 1) {
			ln0 |= MG_DP_MODE_CFG_DP_X1_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X1_MODE;
		} else {
			ln0 |= MG_DP_MODE_CFG_DP_X2_MODE;
			ln1 |= MG_DP_MODE_CFG_DP_X2_MODE;
		}
		break;
	default:
		break;
	}
	if (is_dkl(i915)) {
		dkl_write(i915, tc, 0, DKL_DP_MODE(tc), ln0);
		dkl_write(i915, tc, 1, DKL_DP_MODE(tc), ln1);
	} else {
		i915_write32(i915, MG_DP_MODE(tc, 0), ln0);
		i915_write32(i915, MG_DP_MODE(tc, 1), ln1);
	}
}

/* Ice Lake gates the MG PHY's clocks while a link trains. */
void intel_tc_clock_gating(struct i915_device *i915, struct intel_output *o, int enable)
{
	int tc = o->tc_index;
	uint32_t bits, v;
	if (!o->is_tc || is_dkl(i915))
		return;
	bits = MG_DP_MODE_CFG_TR2PWR_GATING | MG_DP_MODE_CFG_TRPWR_GATING |
	       MG_DP_MODE_CFG_CLNPWR_GATING | MG_DP_MODE_CFG_DIGPWR_GATING |
	       MG_DP_MODE_CFG_GAONPWR_GATING;
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_DP_MODE(tc, ln));
		v = enable ? (v | bits) : (v & ~bits);
		i915_write32(i915, MG_DP_MODE(tc, ln), v);
	}
	bits = MG_MISC_SUS0_CFG_TR2PWR_GATING | MG_MISC_SUS0_CFG_CL2PWR_GATING |
	       MG_MISC_SUS0_CFG_GAONPWR_GATING | MG_MISC_SUS0_CFG_TRPWR_GATING |
	       MG_MISC_SUS0_CFG_CL1PWR_GATING | MG_MISC_SUS0_CFG_DGPWR_GATING;
	v = i915_read32(i915, MG_MISC_SUS0(tc));
	if (enable)
		v |= bits | MG_MISC_SUS0_SUSCLK_DYNCLKGATE_MODE(3);
	else
		v &= ~(bits | MG_MISC_SUS0_SUSCLK_DYNCLKGATE_MODE_MASK);
	i915_write32(i915, MG_MISC_SUS0(tc), v);
}

/* The MG PHY's levels: the three de-emphasis override fields, in the
 * DisplayPort order (400mV x4, 600 x3, 800 x2, 1200). */
struct mg_level {
	uint8_t ovr_5_0, ovr_11_6, ovr_17_12;
};
static const struct mg_level mg_rbr_hbr[] = {
	{ 0x0, 0x1B, 0x00 }, { 0x0, 0x23, 0x08 }, { 0x0, 0x2D, 0x12 }, { 0x0, 0x2B, 0x1B },
	{ 0x0, 0x23, 0x00 }, { 0x0, 0x2B, 0x0B }, { 0x0, 0x30, 0x14 }, { 0x0, 0x2D, 0x00 },
	{ 0x0, 0x2E, 0x0B }, { 0x0, 0x2E, 0x00 },
};
static const struct mg_level mg_hbr2_hbr3[] = {
	{ 0x0, 0x1D, 0x00 }, { 0x0, 0x24, 0x0B }, { 0x0, 0x2D, 0x15 }, { 0x0, 0x2E, 0x1B },
	{ 0x0, 0x24, 0x00 }, { 0x0, 0x2D, 0x0B }, { 0x0, 0x2E, 0x14 }, { 0x0, 0x2D, 0x00 },
	{ 0x0, 0x2E, 0x0B }, { 0x0, 0x2E, 0x00 },
};

static void mg_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	int tc = o->tc_index;
	const struct mg_level *t = o->link_rate_khz > 270000 ? mg_hbr2_hbr3 : mg_rbr_hbr;
	int is_dp = (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
	uint32_t link = is_dp ? o->link_rate_khz : o->link_rate_khz;
	if (level < 0 || level >= 10)
		level = is_dp ? 0 : 9;
	const struct mg_level *l = &t[level];
	uint32_t v;
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_TX1_LINK_PARAMS(tc, ln));
		i915_write32(i915, MG_TX1_LINK_PARAMS(tc, ln), v & ~CRI_USE_FS32);
		v = i915_read32(i915, MG_TX2_LINK_PARAMS(tc, ln));
		i915_write32(i915, MG_TX2_LINK_PARAMS(tc, ln), v & ~CRI_USE_FS32);
	}
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_TX1_SWINGCTRL(tc, ln));
		v = (v & ~CRI_TXDEEMPH_OVERRIDE_17_12_MASK) | CRI_TXDEEMPH_OVERRIDE_17_12(l->ovr_17_12);
		i915_write32(i915, MG_TX1_SWINGCTRL(tc, ln), v);
		v = i915_read32(i915, MG_TX2_SWINGCTRL(tc, ln));
		v = (v & ~CRI_TXDEEMPH_OVERRIDE_17_12_MASK) | CRI_TXDEEMPH_OVERRIDE_17_12(l->ovr_17_12);
		i915_write32(i915, MG_TX2_SWINGCTRL(tc, ln), v);
	}
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_TX1_DRVCTRL(tc, ln));
		v &= ~(CRI_TXDEEMPH_OVERRIDE_11_6_MASK | CRI_TXDEEMPH_OVERRIDE_5_0_MASK);
		v |= CRI_TXDEEMPH_OVERRIDE_5_0(l->ovr_5_0) | CRI_TXDEEMPH_OVERRIDE_11_6(l->ovr_11_6) |
		     CRI_TXDEEMPH_OVERRIDE_EN;
		i915_write32(i915, MG_TX1_DRVCTRL(tc, ln), v);
		v = i915_read32(i915, MG_TX2_DRVCTRL(tc, ln));
		v &= ~(CRI_TXDEEMPH_OVERRIDE_11_6_MASK | CRI_TXDEEMPH_OVERRIDE_5_0_MASK);
		v |= CRI_TXDEEMPH_OVERRIDE_5_0(l->ovr_5_0) | CRI_TXDEEMPH_OVERRIDE_11_6(l->ovr_11_6) |
		     CRI_TXDEEMPH_OVERRIDE_EN;
		i915_write32(i915, MG_TX2_DRVCTRL(tc, ln), v);
	}
	/* the clock hub and the duty-cycle correction follow the rate */
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_CLKHUB(tc, ln));
		v = link < 300000 ? (v | CFG_LOW_RATE_LKREN_EN) : (v & ~CFG_LOW_RATE_LKREN_EN);
		i915_write32(i915, MG_CLKHUB(tc, ln), v);
	}
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_TX1_DCC(tc, ln));
		v &= ~CFG_AMI_CK_DIV_OVERRIDE_VAL_MASK;
		if (link <= 500000)
			v &= ~CFG_AMI_CK_DIV_OVERRIDE_EN;
		else
			v |= CFG_AMI_CK_DIV_OVERRIDE_EN | CFG_AMI_CK_DIV_OVERRIDE_VAL(1);
		i915_write32(i915, MG_TX1_DCC(tc, ln), v);
		v = i915_read32(i915, MG_TX2_DCC(tc, ln));
		v &= ~CFG_AMI_CK_DIV_OVERRIDE_VAL_MASK;
		if (link <= 500000)
			v &= ~CFG_AMI_CK_DIV_OVERRIDE_EN;
		else
			v |= CFG_AMI_CK_DIV_OVERRIDE_EN | CFG_AMI_CK_DIV_OVERRIDE_VAL(1);
		i915_write32(i915, MG_TX2_DCC(tc, ln), v);
	}
	for (int ln = 0; ln < 2; ln++) {
		v = i915_read32(i915, MG_TX1_PISO_READLOAD(tc, ln));
		i915_write32(i915, MG_TX1_PISO_READLOAD(tc, ln), v | CRI_CALCINIT);
		v = i915_read32(i915, MG_TX2_PISO_READLOAD(tc, ln));
		i915_write32(i915, MG_TX2_PISO_READLOAD(tc, ln), v | CRI_CALCINIT);
	}
}

/* The DKL PHY's levels: swing, pre-shoot and de-emphasis per level. */
struct dkl_level {
	uint8_t vswing, preshoot, deemph;
};
static const struct dkl_level dkl_dp_hbr[] = {
	{ 0x7, 0x0, 0x00 }, { 0x5, 0x0, 0x05 }, { 0x2, 0x0, 0x0B }, { 0x0, 0x0, 0x18 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x08 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x0B }, { 0x0, 0x0, 0x00 },
};
static const struct dkl_level dkl_dp_hbr2[] = {
	{ 0x7, 0x0, 0x00 }, { 0x5, 0x0, 0x05 }, { 0x2, 0x0, 0x0B }, { 0x0, 0x0, 0x19 },
	{ 0x5, 0x0, 0x00 }, { 0x2, 0x0, 0x08 }, { 0x0, 0x0, 0x14 }, { 0x2, 0x0, 0x00 },
	{ 0x0, 0x0, 0x0B }, { 0x0, 0x0, 0x00 },
};
static const struct dkl_level dkl_hdmi[] = {
	{ 0x7, 0x0, 0x0 }, { 0x6, 0x0, 0x0 }, { 0x4, 0x0, 0x0 }, { 0x2, 0x0, 0x0 },
	{ 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x5 }, { 0x0, 0x0, 0x8 }, { 0x0, 0x4, 0x0 },
	{ 0x0, 0x4, 0x5 }, { 0x0, 0x4, 0x8 },
};
#define DKL_HDMI_DEFAULT_LEVEL 4

static void dkl_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	int tc = o->tc_index;
	const struct dkl_level *t;
	int is_dp = (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
	if (!is_dp)
		t = dkl_hdmi;
	else
		t = o->link_rate_khz > 270000 ? dkl_dp_hbr2 : dkl_dp_hbr;
	if (level < 0 || level >= 10)
		level = is_dp ? 0 : DKL_HDMI_DEFAULT_LEVEL;
	const struct dkl_level *l = &t[level];
	uint32_t val = DKL_TX_PRESHOOT_COEFF(l->preshoot) | DKL_TX_DE_EMPAHSIS_COEFF(l->deemph) |
		       DKL_TX_VSWING_CONTROL(l->vswing);
	uint32_t mask = DKL_TX_PRESHOOT_COEFF_MASK | DKL_TX_DE_EMPAHSIS_COEFF_MASK |
			DKL_TX_VSWING_CONTROL_MASK;
	for (int ln = 0; ln < 2; ln++) {
		uint32_t v;
		dkl_write(i915, tc, ln, DKL_TX_PMD_LANE_SUS(tc), 0);
		v = dkl_read(i915, tc, ln, DKL_TX_DPCNTL0(tc));
		dkl_write(i915, tc, ln, DKL_TX_DPCNTL0(tc), (v & ~mask) | val);
		v = dkl_read(i915, tc, ln, DKL_TX_DPCNTL1(tc));
		dkl_write(i915, tc, ln, DKL_TX_DPCNTL1(tc), (v & ~mask) | val);
		v = dkl_read(i915, tc, ln, DKL_TX_DPCNTL2(tc));
		dkl_write(i915, tc, ln, DKL_TX_DPCNTL2(tc), v & ~DKL_TX_DP20BITMODE);
	}
}

void intel_tc_set_signal_level(struct i915_device *i915, struct intel_output *o, int level)
{
	if (!o->is_tc || o->tc_mode == INTEL_TC_TBT_ALT)
		return; /* Thunderbolt drives its own PHY */
	if (is_dkl(i915))
		dkl_set_signal_level(i915, o, level);
	else
		mg_set_signal_level(i915, o, level);
}
