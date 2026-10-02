// LikeOS -- the display core clock, and the pcode mailbox it needs.
//
// CDCLK is the clock the display engine runs on; on Skylake it is
// derived from DPLL0, whose link rate the firmware chose (2.7 GHz gives
// 337.5/450/675 MHz, 1.35 GHz gives 308/432/617).  The firmware leaves
// CDCLK running for the panel it lit; the driver reads it, checks it is
// enough for the modes it sets, and only reprograms it when it must
// (which needs the pcode's consent: prepare, change, notify).
//
// Ice Lake and later run CDCLK from a PLL of their own, fed by whichever
// reference clock DSSM names; DG2 and Meteor Lake add a squasher that
// drops cycles of the divided clock in a 16-cycle waveform, so the
// frequency is the VCO divided, times the share of the waveform that is
// set.  From display version 20 (Lunar
// Lake) the memory side of the display (MDCLK) may run from the CDCLK
// PLL itself instead of CD2X, which makes the MDCLK:CDCLK ratio the VCO
// over CDCLK; from version 30 the power controller no longer wants a
// voltage level with a CDCLK change.  Changing the clock, and the
// watermarks' memory latencies, are intel_cdclk_set.c's and
// intel_skl_wm.c's.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* ---- the pcode mailbox ---------------------------------------------------- */

static int pcode_wait_ready(struct i915_device *i915, uint32_t timeout_us)
{
	for (uint32_t t = 0; t < timeout_us; t += 10) {
		if (!(i915_read32(i915, GEN6_PCODE_MAILBOX) & GEN6_PCODE_READY))
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

int intel_pcode_read(struct i915_device *i915, uint32_t mbox, uint32_t *val,
		     uint32_t *val1)
{
	if (pcode_wait_ready(i915, 5000))
		return -EBUSY;
	i915_write32(i915, GEN6_PCODE_DATA, *val);
	i915_write32(i915, GEN6_PCODE_DATA1, val1 ? *val1 : 0);
	i915_write32(i915, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | mbox);
	if (pcode_wait_ready(i915, 50000))
		return -ETIMEDOUT;
	*val = i915_read32(i915, GEN6_PCODE_DATA);
	if (val1)
		*val1 = i915_read32(i915, GEN6_PCODE_DATA1);
	uint32_t err = i915_read32(i915, GEN6_PCODE_MAILBOX) & GEN6_PCODE_ERROR_MASK;
	return err ? -EIO : 0;
}

int intel_pcode_write(struct i915_device *i915, uint32_t mbox, uint32_t val)
{
	if (pcode_wait_ready(i915, 5000))
		return -EBUSY;
	i915_write32(i915, GEN6_PCODE_DATA, val);
	i915_write32(i915, GEN6_PCODE_DATA1, 0);
	i915_write32(i915, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | mbox);
	if (pcode_wait_ready(i915, 50000))
		return -ETIMEDOUT;
	uint32_t err = i915_read32(i915, GEN6_PCODE_MAILBOX) & GEN6_PCODE_ERROR_MASK;
	return err ? -EIO : 0;
}

/* ---- CDCLK ------------------------------------------------------------------ */

/* Haswell/Broadwell: the LCPLL runs CDCLK directly. */
static uint32_t bdw_cdclk_khz(struct i915_device *i915)
{
	uint32_t ctl = i915_read32(i915, LCPLL_CTL);
	if (ctl & LCPLL_CD_SOURCE_FCLK)
		return 800000;
	switch (ctl & LCPLL_CLK_FREQ_MASK) {
	case LCPLL_CLK_FREQ_54O_BDW: return 540000;
	case LCPLL_CLK_FREQ_337_5_BDW: return 337500;
	case LCPLL_CLK_FREQ_675_BDW: return 675000;
	default: return 450000;
	}
}

/* Broxton: the DE PLL from the 19.2 MHz reference, then a divider. */
static uint32_t bxt_cdclk_khz(struct i915_device *i915)
{
	uint32_t en = i915_read32(i915, BXT_DE_PLL_ENABLE);
	uint32_t ctl = i915_read32(i915, CDCLK_CTL);
	if (!(en & BXT_DE_PLL_PLL_ENABLE))
		return 19200;
	uint32_t ratio = i915_read32(i915, BXT_DE_PLL_CTL) & BXT_DE_PLL_RATIO_MASK;
	uint32_t vco = ratio * 19200;
	switch (ctl & BXT_CDCLK_CD2X_DIV_SEL_MASK) {
	case BXT_CDCLK_CD2X_DIV_SEL_1: return vco / 2;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5: return vco / 3;
	case BXT_CDCLK_CD2X_DIV_SEL_2: return vco / 4;
	default: return vco / 8;
	}
}

static uint32_t popcount16(uint32_t v)
{
	uint32_t n = 0;
	for (v &= 0xffff; v; v &= v - 1)
		n++;
	return n;
}

/* The voltage level a CDCLK frequency needs: Meteor Lake, Battlemage and
 * Lunar Lake step at 312, 480, 556.8 and 652.8 MHz (the Raptor Lake U
 * table); Tiger Lake at 312, 326.4, 556.8 and 652.8; Ice Lake at 312,
 * 556.8 and 652.8.  From version 30 the power controller does not take a
 * voltage index with the change: the level is 0. */
static uint8_t cdclk_voltage_level(struct i915_device *i915, uint32_t cdclk)
{
	static const uint32_t mtl[] = { 312000, 480000, 556800, 652800 };
	static const uint32_t tgl[] = { 312000, 326400, 556800, 652800 };
	static const uint32_t icl[] = { 312000, 556800, 652800 };
	const uint32_t *t;
	uint8_t n;

	if (i915->info->display_ver >= 30) {
		return 0;
	} else if (i915->info->display_ver >= 14) {
		t = mtl;
		n = 4;
	} else if (i915->info->display_ver >= 12) {
		t = tgl;
		n = 4;
	} else if (i915->info->display_ver == 11) {
		t = icl;
		n = 3;
	} else {
		return 0;
	}
	for (uint8_t i = 0; i < n; i++)
		if (cdclk <= t[i])
			return i;
	return (uint8_t)(n - 1);
}

/* Gen11+: the CDCLK PLL from whichever reference clock DSSM names, then
 * the CD2X divider and, on DG2 and from Meteor Lake on, the squasher
 * (Lunar Lake, Panther Lake and Nova Lake run their low frequencies
 * squashed and change between PLL ratios by crawling, so a squashed
 * waveform is the normal state there).  With the PLL off (or not
 * locked) CDCLK is the bypass: half of that reference clock on Gen12+,
 * 50 MHz on Gen11. */
static uint32_t icl_cdclk_khz(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	uint32_t en = i915_read32(i915, BXT_DE_PLL_ENABLE);
	uint32_t ctl = i915_read32(i915, CDCLK_CTL);
	uint32_t ref, div, cdclk;
	switch (i915_read32(i915, DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: ref = 19200; break;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: ref = 38400; break;
	default: ref = 24000; break;
	}
	/* DG2 always runs it from 38.4 MHz */
	if (i915->info->platform == I915_PLATFORM_DG2)
		ref = 38400;
	d->cdclk_ref_khz = ref;
	d->cdclk_vco_khz = 0;
	if (!(en & BXT_DE_PLL_PLL_ENABLE) || !(en & BXT_DE_PLL_LOCK))
		return i915->info->display_ver >= 12 ? ref / 2 : 50000;
	uint32_t vco = (en & CDCLK_PLL_RATIO_MASK) * ref;
	d->cdclk_vco_khz = vco;
	switch (ctl & BXT_CDCLK_CD2X_DIV_SEL_MASK) {
	case BXT_CDCLK_CD2X_DIV_SEL_1: div = 2; break;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5: div = 3; break;
	case BXT_CDCLK_CD2X_DIV_SEL_2: div = 4; break;
	default: div = 8; break;
	}
	/* the squasher: DG2 and display version 14 on */
	int has_squash = i915->info->display_ver >= 14 ||
			 i915->info->platform == I915_PLATFORM_DG2;
	uint32_t squash = has_squash ? i915_read32(i915, CDCLK_SQUASH_CTL) : 0;
	if (squash & CDCLK_SQUASH_ENABLE) {
		/* the waveform is the top `size' bits of the 16-bit field */
		uint32_t size = CDCLK_SQUASH_WINDOW_SIZE(squash) + 1;
		uint32_t wave = CDCLK_SQUASH_WAVEFORM(squash) >> (16 - size);
		cdclk = (popcount16(wave) * vco + size * div / 2) / (size * div);
	} else {
		cdclk = (vco + div / 2) / div;
	}
	return cdclk;
}

/* Display version 20+: where MDCLK comes from (CDCLK_CTL bit 25: the
 * CDCLK PLL, or CD2X as before) and the MDCLK:CDCLK ratio that follows,
 * which the DBUF trackers and the MBUS throttle are programmed with.  The
 * hardware sequence keeps MDCLK on the PLL from version 20 on. */
#define CDCLK_CTL_MDCLK_SOURCE_PLL (1u << 25)

static int mdclk_cdclk_ratio(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	if (i915->info->display_ver >= 20 && d->cdclk_khz && d->cdclk_vco_khz)
		return (int)((d->cdclk_vco_khz + d->cdclk_khz - 1) / d->cdclk_khz);
	return 2;
}

uint32_t intel_cdclk_khz(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	if (i915->info->display_ver >= 11)
		return icl_cdclk_khz(i915);
	switch (d->model) {
	case INTEL_DISPLAY_BDW: return bdw_cdclk_khz(i915);
	case INTEL_DISPLAY_BXT: return bxt_cdclk_khz(i915);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
	case INTEL_DISPLAY_MTL: return icl_cdclk_khz(i915);
	default: break;
	}
	uint32_t lcpll = i915_read32(i915, LCPLL1_CTL);
	uint32_t ctl = i915_read32(i915, CDCLK_CTL);
	uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);

	if (!(lcpll & LCPLL_PLL_ENABLE))
		return 24000; /* reference clock: DPLL0 off */
	/* DPLL0's link rate decides the family of CDCLK frequencies. */
	uint32_t rate = (ctrl1 >> DPLL_CTRL1_LINK_RATE_SHIFT(0)) & 7;
	int vco_8640 = (rate == DPLL_CTRL1_LINK_RATE_1080 ||
			rate == DPLL_CTRL1_LINK_RATE_2160);
	d->dpll0_link_rate_khz = 0;
	switch (rate) {
	case DPLL_CTRL1_LINK_RATE_2700: d->dpll0_link_rate_khz = 270000; break;
	case DPLL_CTRL1_LINK_RATE_1350: d->dpll0_link_rate_khz = 135000; break;
	case DPLL_CTRL1_LINK_RATE_810: d->dpll0_link_rate_khz = 81000; break;
	case DPLL_CTRL1_LINK_RATE_1620: d->dpll0_link_rate_khz = 162000; break;
	case DPLL_CTRL1_LINK_RATE_1080: d->dpll0_link_rate_khz = 108000; break;
	case DPLL_CTRL1_LINK_RATE_2160: d->dpll0_link_rate_khz = 216000; break;
	}
	switch (ctl & CDCLK_FREQ_SEL_MASK) {
	case CDCLK_FREQ_450_432:
		return vco_8640 ? 432000 : 450000;
	case CDCLK_FREQ_540:
		return 540000;
	case CDCLK_FREQ_337_308:
		return vco_8640 ? 308571 : 337500;
	case CDCLK_FREQ_675_617:
		return vco_8640 ? 617143 : 675000;
	}
	return 337500;
}

/* The raw clock the panel PWM and the sequencer count in.  Meteor Lake's
 * (and that of every part after it, Battlemage included: Meteor Point's
 * and Lunar Lake's south display kinds) is always 38.4 MHz and needs no
 * programming.  The Cannon Point family's is programmed from the
 * strap: 24 MHz as an integer divider, 19.2 MHz as 19 plus a fraction
 * of 1/5 (denominator 5, numerator 1).  Older PCHs report it. */
static uint32_t read_rawclk(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	if (d->model == INTEL_DISPLAY_BXT)
		return 19200;
	if (d->model == INTEL_DISPLAY_BDW)
		return 125000; /* Lynx/Wildcat Point */
	if (i915->pch == I915_PCH_MTP || i915->pch == I915_PCH_LNL)
		return 38400;
	if (i915->pch == I915_PCH_DG1 || i915->pch == I915_PCH_DG2) {
		/* numerator 2, denominator 4, divider 37: 38.4 MHz */
		i915_write32(i915, RAWCLK_FREQ,
			     CNP_RAWCLK_DEN(4) | CNP_RAWCLK_DIV(37) | ICP_RAWCLK_NUM(2));
		return 38400;
	}
	if (i915->pch >= I915_PCH_CNP && i915->pch <= I915_PCH_ADP) {
		uint32_t divider, fraction, raw;
		if (i915_read32(i915, SFUSE_STRAP) & SFUSE_STRAP_RAW_FREQUENCY) {
			divider = 24000;
			fraction = 0;
		} else {
			divider = 19000;
			fraction = 200;
		}
		raw = CNP_RAWCLK_DIV(divider / 1000);
		if (fraction) {
			raw |= CNP_RAWCLK_DEN((1000 + fraction / 2) / fraction - 1);
			if (i915->pch >= I915_PCH_ICP)
				raw |= ICP_RAWCLK_NUM(1);
		}
		i915_write32(i915, RAWCLK_FREQ, raw);
		return divider + fraction;
	}
	/* Sunrise/Kaby Point: the frequency in MHz in the low bits */
	uint32_t mhz = i915_read32(i915, RAWCLK_FREQ) & RAWCLK_FREQ_MASK;
	if (mhz < 19 || mhz > 25)
		return 24000;
	return mhz * 1000;
}

int intel_cdclk_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	d->cdclk_khz = intel_cdclk_khz(i915);
	d->cdclk_voltage_level = cdclk_voltage_level(i915, d->cdclk_khz);
	d->rawclk_khz = read_rawclk(i915);
	/* the whole configuration for the code that changes it */
	intel_cdclk_set_init(i915);
	i915_dbg("[drm] i915: CDCLK %u kHz (DPLL0 link %u kHz, CDCLK PLL %u kHz on %u kHz, voltage level %u), rawclk %u kHz\n",
		d->cdclk_khz, d->dpll0_link_rate_khz, d->cdclk_vco_khz, d->cdclk_ref_khz,
		d->cdclk_voltage_level, d->rawclk_khz);
	if (i915->info->display_ver >= 20)
		i915_dbg("[drm] i915: MDCLK from %s, MDCLK:CDCLK %d, MBUS %s\n",
			 (i915_read32(i915, CDCLK_CTL) & CDCLK_CTL_MDCLK_SOURCE_PLL) ? "the CDCLK PLL" :
										"CD2X",
			 mdclk_cdclk_ratio(i915),
			 (i915_read32(i915, MBUS_CTL_REG) & MBUS_CTL_JOIN) ? "joined" : "split");
	/* Gen11+ run as low as 172.8 MHz (two pixels a clock, enough for
	 * a 1080p panel): only a PLL left off is fatal there. */
	int icl_plus = i915->info->display_ver >= 11;
	if (icl_plus ? d->cdclk_vco_khz == 0 :
		       d->cdclk_khz < (d->model == INTEL_DISPLAY_BXT ? 144000u : 300000u)) {
		kprintf("[drm] i915: CDCLK %u kHz is too low for a display; the firmware left its PLL off\n",
			d->cdclk_khz);
		return -ENODEV;
	}
	return 0;
}
