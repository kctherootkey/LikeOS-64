// LikeOS-64 -- the display core clock, and the pcode mailbox it needs.
//
// CDCLK is the clock the display engine runs on; on Skylake it is
// derived from DPLL0, whose link rate the firmware chose (2.7 GHz gives
// 337.5/450/675 MHz, 1.35 GHz gives 308/432/617).  The firmware leaves
// CDCLK running for the panel it lit; the driver reads it, checks it is
// enough for the modes it sets, and only reprograms it when it must
// (which needs the pcode's consent: prepare, change, notify).
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
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

/* Gen11+: the CDCLK PLL from the reference DSSM names, then a divider. */
static uint32_t icl_cdclk_khz(struct i915_device *i915)
{
	uint32_t en = i915_read32(i915, BXT_DE_PLL_ENABLE);
	uint32_t ctl = i915_read32(i915, CDCLK_CTL);
	uint32_t ref;
	switch (i915_read32(i915, DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: ref = 19200; break;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: ref = 38400; break;
	default: ref = 24000; break;
	}
	if (!(en & BXT_DE_PLL_PLL_ENABLE))
		return ref / 2;
	uint32_t vco = (en & CDCLK_PLL_RATIO_MASK) * ref;
	switch (ctl & BXT_CDCLK_CD2X_DIV_SEL_MASK) {
	case BXT_CDCLK_CD2X_DIV_SEL_1: return vco / 2;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5: return vco / 3;
	case BXT_CDCLK_CD2X_DIV_SEL_2: return vco / 4;
	default: return vco / 8;
	}
}

uint32_t intel_cdclk_khz(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	switch (d->model) {
	case INTEL_DISPLAY_BDW: return bdw_cdclk_khz(i915);
	case INTEL_DISPLAY_BXT: return bxt_cdclk_khz(i915);
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL: return icl_cdclk_khz(i915);
	default: break;
	}
	uint32_t lcpll = i915_read32(i915, LCPLL1_CTL);
	uint32_t ctl = i915_read32(i915, CDCLK_CTL);
	uint32_t ctrl1 = i915_read32(i915, DPLL_CTRL1);

	if (!(lcpll & LCPLL_PLL_ENABLE))
		return 24000; /* the reference: DPLL0 off */
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

int intel_cdclk_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	d->cdclk_khz = intel_cdclk_khz(i915);
	d->rawclk_khz = 24000; /* Sunrise Point: 24 MHz raw clock */
	if (d->model == INTEL_DISPLAY_BXT)
		d->rawclk_khz = 19200;
	else if (d->model == INTEL_DISPLAY_BDW)
		d->rawclk_khz = 125000; /* Lynx/Wildcat Point */
	if (i915->pch >= I915_PCH_CNP) {
		uint32_t raw = i915_read32(i915, RAWCLK_FREQ);
		/* CNP+: divider in the low bits, 19.2 or 24 MHz */
		d->rawclk_khz = ((raw & RAWCLK_FREQ_MASK) + 1) * 1000;
		if (d->rawclk_khz < 19000 || d->rawclk_khz > 25000)
			d->rawclk_khz = 24000;
	}
	i915_dbg("[drm] i915: CDCLK %u kHz (DPLL0 link %u kHz), rawclk %u kHz\n",
		d->cdclk_khz, d->dpll0_link_rate_khz, d->rawclk_khz);
	if (d->cdclk_khz < (d->model == INTEL_DISPLAY_BXT ? 144000u : 300000u)) {
		kprintf("[drm] i915: CDCLK %u kHz is too low for a display; the firmware left its PLL off\n",
			d->cdclk_khz);
		return -ENODEV;
	}
	/* Memory latency for the watermarks: eight levels, in microseconds,
	 * two mailbox reads of four each. */
	uint32_t v = 0, v1 = 0;
	d->nlatency = 0;
	if (intel_pcode_read(i915, GEN9_PCODE_READ_MEM_LATENCY, &v, &v1) == 0) {
		d->mem_latency[0] = v & 0xff;
		d->mem_latency[1] = (v >> 8) & 0xff;
		d->mem_latency[2] = (v >> 16) & 0xff;
		d->mem_latency[3] = (v >> 24) & 0xff;
		v = 1;
		v1 = 0;
		if (intel_pcode_read(i915, GEN9_PCODE_READ_MEM_LATENCY, &v, &v1) == 0) {
			d->mem_latency[4] = v & 0xff;
			d->mem_latency[5] = (v >> 8) & 0xff;
			d->mem_latency[6] = (v >> 16) & 0xff;
			d->mem_latency[7] = (v >> 24) & 0xff;
			d->nlatency = 8;
		} else {
			d->nlatency = 4;
		}
		/* A zero level 0 means the firmware left it unset: 2 us is
		 * the conventional floor, and every level above it gets the
		 * same correction. */
		if (d->mem_latency[0] == 0) {
			for (int i = 0; i < d->nlatency; i++)
				d->mem_latency[i] += 2;
		}
		i915_dbg("[drm] i915: memory latency %u/%u/%u/%u us\n",
			d->mem_latency[0], d->mem_latency[1], d->mem_latency[2],
			d->mem_latency[3]);
	} else {
		d->mem_latency[0] = 2;
		d->nlatency = 1;
		kprintf("[drm] i915: pcode did not answer the latency query; level 0 = 2 us\n");
	}
	return 0;
}
