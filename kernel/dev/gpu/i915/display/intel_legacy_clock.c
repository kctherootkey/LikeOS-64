// LikeOS -- display clocks of the Intel parts before DDI.
//
// The display core clock (CDCLK) bounds the pixel rate a pipe can run:
// up to 90% of it (95% on Cherryview), twice that for a gen2/3 pipe in
// double wide mode.  Most of these parts fix it at reset or let the BIOS
// strap it: the gen3/4 parts report it through the graphics clock
// configuration word in the device's PCI configuration space, combined
// with the host PLL frequency read from the memory controller (MCHBAR)
// on the parts whose CDCLK is a divider of it; Ironlake runs it at 450
// MHz, Sandy and Ivy Bridge at 400.  The raw clock that times the panel
// sequencer, the AUX channel and the backlight PWM is a quarter of the
// front side bus on the GMCH parts and a PCH register on the others.
// Valleyview and Cherryview read all of this over the sideband
// (intel_legacy_vlv.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2026 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/hal/pci.h>
#include <kernel/io/console.h>

/* the memory controller's registers, mirrored in the MMIO window */
#define LGC_MCHBAR_MIRROR 0x10000u
#define LGC_CLKCFG (LGC_MCHBAR_MIRROR + 0xc00u)
#define LGC_CLKCFG_FSB_400 (0u << 0)
#define LGC_CLKCFG_FSB_400_ALT (5u << 0)
#define LGC_CLKCFG_FSB_533 (1u << 0)
#define LGC_CLKCFG_FSB_667 (3u << 0)
#define LGC_CLKCFG_FSB_800 (2u << 0)
#define LGC_CLKCFG_FSB_1067 (6u << 0)
#define LGC_CLKCFG_FSB_1067_ALT (0u << 0)
#define LGC_CLKCFG_FSB_1333 (7u << 0)
#define LGC_CLKCFG_FSB_1333_ALT (4u << 0)
#define LGC_CLKCFG_FSB_1600_ALT (6u << 0)
#define LGC_CLKCFG_FSB_MASK (7u << 0)
#define LGC_CLKCFG_MEM_533 (1u << 4)
#define LGC_CLKCFG_MEM_667 (2u << 4)
#define LGC_CLKCFG_MEM_800 (3u << 4)
#define LGC_CLKCFG_MEM_MASK (7u << 4)
#define LGC_CSHRDDR3CTL (LGC_MCHBAR_MIRROR + 0x1a8u)
#define LGC_CSHRDDR3CTL_DDR3 (1u << 2)
#define LGC_HPLLVCO_MOBILE (LGC_MCHBAR_MIRROR + 0xc0fu)
#define LGC_HPLLVCO (LGC_MCHBAR_MIRROR + 0xc38u)

/* PCI configuration of the graphics device (and, on i85x, the host bridge) */
#define LGC_HPLLCC 0xc0
#define LGC_GC_CLOCK_CONTROL_MASK (0x7u << 0)
#define LGC_GC_CLOCK_133_200 0u
#define LGC_GC_CLOCK_100_200 1u
#define LGC_GC_CLOCK_100_133 2u
#define LGC_GC_CLOCK_133_266 3u
#define LGC_GC_CLOCK_133_200_2 4u
#define LGC_GC_CLOCK_133_266_2 5u
#define LGC_GC_CLOCK_166_266 6u
#define LGC_GC_CLOCK_166_250 7u
#define LGC_GCFGC 0xf0
#define LGC_GC_LOW_FREQUENCY_ENABLE (1u << 7)
#define LGC_GC_DISPLAY_CLOCK_190_200_MHZ (0u << 4)
#define LGC_GC_DISPLAY_CLOCK_333_320_MHZ (4u << 4)
#define LGC_GC_DISPLAY_CLOCK_267_MHZ_PNV (0u << 4)
#define LGC_GC_DISPLAY_CLOCK_333_MHZ_PNV (1u << 4)
#define LGC_GC_DISPLAY_CLOCK_444_MHZ_PNV (2u << 4)
#define LGC_GC_DISPLAY_CLOCK_200_MHZ_PNV (5u << 4)
#define LGC_GC_DISPLAY_CLOCK_133_MHZ_PNV (6u << 4)
#define LGC_GC_DISPLAY_CLOCK_167_MHZ_PNV (7u << 4)
#define LGC_GC_DISPLAY_CLOCK_MASK (7u << 4)

/* One byte of the MCHBAR mirror (the registers are byte wide; the bus
 * takes aligned dword reads). */
static uint8_t mchbar_read8(struct lg_display *d, uint32_t reg)
{
	uint32_t v = lg_rd_abs(d, reg & ~3u);
	return (uint8_t)(v >> ((reg & 3u) * 8));
}

static uint16_t gcfgc(struct lg_display *d)
{
	return pci_cfg_read16(d->i915->pci, LGC_GCFGC);
}

/* The host PLL that the gen3/4 CDCLK divides down. */
static uint32_t hpll_vco(struct lg_display *d)
{
	static const uint32_t blb_vco[8] = { 3200000, 4000000, 5333333, 4800000, 6400000 };
	static const uint32_t pnv_vco[8] = { 3200000, 4000000, 5333333, 4800000, 2666667 };
	static const uint32_t cl_vco[8] = { 3200000, 4000000, 5333333, 6400000,
					    3333333, 3566667, 4266667 };
	static const uint32_t elk_vco[8] = { 3200000, 4000000, 5333333, 4800000 };
	static const uint32_t ctg_vco[8] = { 3200000, 4000000, 5333333, 6400000,
					     2666667, 4266667 };
	const uint32_t *table;

	if (d->is_gm45)
		table = ctg_vco;
	else if (d->is_g4x)
		table = elk_vco;
	else if (d->is_i965gm)
		table = cl_vco;
	else if (d->is_pnv)
		table = pnv_vco;
	else if (d->is_g33)
		table = blb_vco;
	else
		return 0;
	uint8_t tmp = mchbar_read8(d, (d->is_pnv || d->mobile) ? LGC_HPLLVCO_MOBILE : LGC_HPLLVCO);
	uint32_t vco = table[tmp & 7];
	if (!vco)
		kprintf("[drm] i915: unknown host PLL frequency (HPLLVCO %02x)\n", tmp);
	return vco;
}

static uint32_t i85x_cdclk(struct lg_display *d)
{
	/* 852GM/GMV: 133 MHz only, and the HPLLCC encoding differs */
	if (d->i915->revid == 0x1)
		return 133333;
	uint32_t hpllcc = pci_cfg_read32(0, 0, 3, LGC_HPLLCC) & 0xffff;
	switch (hpllcc & LGC_GC_CLOCK_CONTROL_MASK) {
	case LGC_GC_CLOCK_133_200:
	case LGC_GC_CLOCK_133_200_2:
	case LGC_GC_CLOCK_100_200:
		return 200000;
	case LGC_GC_CLOCK_166_250:
		return 250000;
	case LGC_GC_CLOCK_100_133:
		return 133333;
	default: /* 133/266, 166/266 */
		return 266667;
	}
}

static uint32_t i915gm_cdclk(struct lg_display *d)
{
	uint16_t g = gcfgc(d);

	if (g & LGC_GC_LOW_FREQUENCY_ENABLE)
		return 133333;
	if ((g & LGC_GC_DISPLAY_CLOCK_MASK) == LGC_GC_DISPLAY_CLOCK_333_320_MHZ)
		return 333333;
	return 190000;
}

static uint32_t i945gm_cdclk(struct lg_display *d)
{
	uint16_t g = gcfgc(d);

	if (g & LGC_GC_LOW_FREQUENCY_ENABLE)
		return 133333;
	if ((g & LGC_GC_DISPLAY_CLOCK_MASK) == LGC_GC_DISPLAY_CLOCK_333_320_MHZ)
		return 320000;
	return 200000;
}

static uint32_t g33_cdclk(struct lg_display *d)
{
	static const uint8_t div_3200[] = { 12, 10, 8, 7, 5, 16 };
	static const uint8_t div_4000[] = { 14, 12, 10, 8, 6, 20 };
	static const uint8_t div_4800[] = { 20, 14, 12, 10, 8, 24 };
	static const uint8_t div_5333[] = { 20, 16, 12, 12, 8, 28 };
	const uint8_t *div;
	uint32_t vco = hpll_vco(d);
	unsigned sel = (gcfgc(d) >> 4) & 7;

	d->hpll_vco_khz = vco;
	if (sel >= sizeof(div_3200))
		goto fail;
	switch (vco) {
	case 3200000: div = div_3200; break;
	case 4000000: div = div_4000; break;
	case 4800000: div = div_4800; break;
	case 5333333: div = div_5333; break;
	default: goto fail;
	}
	return DIV_ROUND_CLOSEST(vco, div[sel]);
fail:
	kprintf("[drm] i915: cannot tell CDCLK (host PLL %u kHz, GCFGC %04x)\n", vco, gcfgc(d));
	return 190476;
}

static uint32_t pnv_cdclk(struct lg_display *d)
{
	switch (gcfgc(d) & LGC_GC_DISPLAY_CLOCK_MASK) {
	case LGC_GC_DISPLAY_CLOCK_267_MHZ_PNV: return 266667;
	case LGC_GC_DISPLAY_CLOCK_333_MHZ_PNV: return 333333;
	case LGC_GC_DISPLAY_CLOCK_444_MHZ_PNV: return 444444;
	case LGC_GC_DISPLAY_CLOCK_200_MHZ_PNV: return 200000;
	case LGC_GC_DISPLAY_CLOCK_167_MHZ_PNV: return 166667;
	default: return 133333;
	}
}

static uint32_t i965gm_cdclk(struct lg_display *d)
{
	static const uint8_t div_3200[] = { 16, 10, 8 };
	static const uint8_t div_4000[] = { 20, 12, 10 };
	static const uint8_t div_5333[] = { 24, 16, 14 };
	const uint8_t *div;
	uint32_t vco = hpll_vco(d);
	unsigned sel = ((gcfgc(d) >> 8) & 0x1f) - 1;

	d->hpll_vco_khz = vco;
	if (sel >= sizeof(div_3200))
		goto fail;
	switch (vco) {
	case 3200000: div = div_3200; break;
	case 4000000: div = div_4000; break;
	case 5333333: div = div_5333; break;
	default: goto fail;
	}
	return DIV_ROUND_CLOSEST(vco, div[sel]);
fail:
	kprintf("[drm] i915: cannot tell CDCLK (host PLL %u kHz, GCFGC %04x)\n", vco, gcfgc(d));
	return 200000;
}

static uint32_t gm45_cdclk(struct lg_display *d)
{
	uint32_t vco = hpll_vco(d);
	unsigned sel = (gcfgc(d) >> 12) & 1;

	d->hpll_vco_khz = vco;
	switch (vco) {
	case 2666667:
	case 4000000:
	case 5333333:
		return sel ? 333333 : 222222;
	case 3200000:
		return sel ? 320000 : 228571;
	default:
		kprintf("[drm] i915: cannot tell CDCLK (host PLL %u kHz)\n", vco);
		return 222222;
	}
}

/* The front side bus strap (not necessarily the bus as configured). */
static uint32_t fsb_freq(struct lg_display *d)
{
	uint32_t fsb = lg_rd_abs(d, LGC_CLKCFG) & LGC_CLKCFG_FSB_MASK;

	if (d->is_pnv || d->mobile) {
		switch (fsb) {
		case LGC_CLKCFG_FSB_400: return 400000;
		case LGC_CLKCFG_FSB_533: return 533333;
		case LGC_CLKCFG_FSB_667: return 666667;
		case LGC_CLKCFG_FSB_800: return 800000;
		case LGC_CLKCFG_FSB_1067: return 1066667;
		case LGC_CLKCFG_FSB_1333: return 1333333;
		default: return 1333333;
		}
	}
	switch (fsb) {
	case LGC_CLKCFG_FSB_400_ALT: return 400000;
	case LGC_CLKCFG_FSB_533: return 533333;
	case LGC_CLKCFG_FSB_667: return 666667;
	case LGC_CLKCFG_FSB_800: return 800000;
	case LGC_CLKCFG_FSB_1067_ALT: return 1066667;
	case LGC_CLKCFG_FSB_1333_ALT: return 1333333;
	case LGC_CLKCFG_FSB_1600_ALT: return 1600000;
	default: return 1333333;
	}
}

static uint32_t pnv_mem_freq(struct lg_display *d)
{
	switch (lg_rd_abs(d, LGC_CLKCFG) & LGC_CLKCFG_MEM_MASK) {
	case LGC_CLKCFG_MEM_533: return 533333;
	case LGC_CLKCFG_MEM_667: return 666667;
	case LGC_CLKCFG_MEM_800: return 800000;
	default: return 0;
	}
}

void lg_clocks_init(struct lg_display *d)
{
	uint32_t cdclk;
	int guardband = 90;

	if (d->is_vlv || d->is_chv) {
		lg_vlv_clocks_init(d);
		if (!d->max_dotclk_khz)
			d->max_dotclk_khz = d->max_cdclk_khz * (d->is_chv ? 95u : 90u) / 100u;
		i915_dbg("[drm] i915: CDCLK %u kHz (max %u), dot clock up to %u kHz, raw clock %u kHz\n",
			 d->cdclk_khz, d->max_cdclk_khz, d->max_dotclk_khz, d->rawclk_khz);
		return;
	}
	if (d->is_snb || d->is_ivb)
		cdclk = 400000;
	else if (d->is_ilk)
		cdclk = 450000;
	else if (d->is_gm45)
		cdclk = gm45_cdclk(d);
	else if (d->is_g4x)
		cdclk = g33_cdclk(d);
	else if (d->is_i965gm)
		cdclk = i965gm_cdclk(d);
	else if (d->is_i965g)
		cdclk = 400000;
	else if (d->is_pnv)
		cdclk = pnv_cdclk(d);
	else if (d->is_g33)
		cdclk = g33_cdclk(d);
	else if (d->is_i945gm)
		cdclk = i945gm_cdclk(d);
	else if (d->is_i945g)
		cdclk = 400000;
	else if (d->is_i915gm)
		cdclk = i915gm_cdclk(d);
	else if (d->is_i915g)
		cdclk = 333333;
	else if (d->is_i865)
		cdclk = 266667;
	else if (d->is_i85x)
		cdclk = i85x_cdclk(d);
	else if (d->is_i845)
		cdclk = 200000;
	else
		cdclk = 133333;
	d->cdclk_khz = cdclk;
	/* fixed on every one of these parts */
	d->max_cdclk_khz = cdclk;
	/* gen2/3 pipes can run two pixels per clock (double wide) */
	d->max_dotclk_khz = (d->ver < 4 ? 2u : 1u) * cdclk * (uint32_t)guardband / 100u;

	if (d->pch != LG_PCH_NONE) {
		d->rawclk_khz = (lg_rd(d, PCH_RAWCLK_FREQ) & RAWCLK_FREQ_MASK) * 1000u;
	} else if (d->ver >= 3) {
		d->fsb_khz = fsb_freq(d);
		/* the raw clock is a quarter of the FSB */
		d->rawclk_khz = DIV_ROUND_CLOSEST(d->fsb_khz, 4u);
	} else {
		d->rawclk_khz = 0;
	}
	if (d->is_pnv) {
		d->mem_khz = pnv_mem_freq(d);
		d->is_ddr3 = !!(lg_rd_abs(d, LGC_CSHRDDR3CTL) & LGC_CSHRDDR3CTL_DDR3);
	}
	i915_dbg("[drm] i915: CDCLK %u kHz, dot clock up to %u kHz, raw clock %u kHz%s\n",
		 d->cdclk_khz, d->max_dotclk_khz, d->rawclk_khz,
		 d->is_pnv ? (d->is_ddr3 ? ", DDR3" : ", DDR2") : "");
}
