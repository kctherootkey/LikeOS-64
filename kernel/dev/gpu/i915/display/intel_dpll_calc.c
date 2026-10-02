// LikeOS -- pure PLL arithmetic for the display PLLs: the combo PHY PLL
// of Ice Lake and later, Skylake's and Haswell's WRPLLs in HDMI mode and
// the Broxton/Gemini Lake port PLL.  Kept free of kernel headers so the
// build host can run it (host/test-dpll-icl.sh).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2023 Intel Corporation

#include <kernel/dev/gpu/i915/intel_dpll_calc.h>

static uint64_t udiff(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

/* ---- Ice Lake and later: the combo PHY PLL ------------------------------------- */

/* The combo PLL's DCO and dividers for the DisplayPort link rates, per
 * reference clock, as the hardware's tuning tables give them -- already
 * in register encoding (pdiv 0x1/0x2/0x4/0x8 = 2/3/5/7, kdiv 1/2/4 =
 * 1/2/3).  The 38.4 MHz reference uses the 19.2 MHz table: the PLL
 * halves it. */
struct dp_entry {
	uint32_t link_khz;
	uint16_t dco_integer, dco_fraction;
	uint8_t pdiv, kdiv, qdiv_mode, qdiv_ratio;
};

static const struct dp_entry dp_24mhz[] = {
	{ 540000, 0x151, 0x4000, 0x2, 1, 0, 0 },
	{ 270000, 0x151, 0x4000, 0x2, 2, 0, 0 },
	{ 162000, 0x151, 0x4000, 0x4, 2, 0, 0 },
	{ 324000, 0x151, 0x4000, 0x4, 1, 0, 0 },
	{ 216000, 0x168, 0x0000, 0x1, 2, 1, 2 },
	{ 432000, 0x168, 0x0000, 0x1, 2, 0, 0 },
	{ 648000, 0x195, 0x0000, 0x2, 1, 0, 0 },
	{ 810000, 0x151, 0x4000, 0x1, 1, 0, 0 },
};

static const struct dp_entry dp_19_2mhz[] = {
	{ 540000, 0x1A5, 0x7000, 0x2, 1, 0, 0 },
	{ 270000, 0x1A5, 0x7000, 0x2, 2, 0, 0 },
	{ 162000, 0x1A5, 0x7000, 0x4, 2, 0, 0 },
	{ 324000, 0x1A5, 0x7000, 0x4, 1, 0, 0 },
	{ 216000, 0x1C2, 0x0000, 0x1, 2, 1, 2 },
	{ 432000, 0x1C2, 0x0000, 0x1, 2, 0, 0 },
	{ 648000, 0x1FA, 0x2000, 0x2, 1, 0, 0 },
	{ 810000, 0x1A5, 0x7000, 0x1, 1, 0, 0 },
};

int icl_combo_dp_params(uint32_t link_khz, uint32_t ref_khz,
			struct icl_pll_params *out)
{
	const struct dp_entry *t = ref_khz == 24000 ? dp_24mhz : dp_19_2mhz;
	for (unsigned i = 0; i < sizeof(dp_24mhz) / sizeof(dp_24mhz[0]); i++) {
		if (t[i].link_khz != link_khz)
			continue;
		out->dco_integer = t[i].dco_integer;
		out->dco_fraction = t[i].dco_fraction;
		out->pdiv = t[i].pdiv;
		out->kdiv = t[i].kdiv;
		out->qdiv_mode = t[i].qdiv_mode;
		out->qdiv_ratio = t[i].qdiv_ratio;
		return 0;
	}
	return -1;
}

/* What the DCO counts: a 38.4 MHz reference clock is halved by the PLL. */
static uint32_t icl_wrpll_ref(uint32_t ref_khz)
{
	return ref_khz == 38400 ? 19200 : ref_khz;
}

/* How a total divider is split over the three post dividers. */
static void icl_wrpll_multipliers(uint32_t div, uint32_t *p, uint32_t *q, uint32_t *k)
{
	*p = *q = *k = 0;
	if (div % 2 == 0) {
		if (div == 2) {
			*p = 2; *q = 1; *k = 1;
		} else if (div % 4 == 0) {
			*p = 2; *q = div / 4; *k = 2;
		} else if (div % 6 == 0) {
			*p = 3; *q = div / 6; *k = 2;
		} else if (div % 5 == 0) {
			*p = 5; *q = div / 10; *k = 2;
		} else if (div % 14 == 0) {
			*p = 7; *q = div / 14; *k = 2;
		}
	} else {
		if (div == 3 || div == 5 || div == 7) {
			*p = div; *q = 1; *k = 1;
		} else { /* 9, 15, 21 */
			*p = div / 3; *q = 1; *k = 3;
		}
	}
}

int icl_combo_hdmi_params(uint32_t clock_khz, uint32_t ref_khz,
			  struct icl_pll_params *out)
{
	static const uint8_t dividers[] = { 2, 4, 6, 8, 10, 12, 14, 16,
					    18, 20, 24, 28, 30, 32, 36, 40,
					    42, 44, 48, 50, 52, 54, 56, 60,
					    64, 66, 68, 70, 72, 76, 78, 80,
					    84, 88, 90, 92, 96, 98, 100, 102,
					    3, 5, 7, 9, 15, 21 };
	const uint32_t dco_min = 7998000, dco_max = 10000000;
	const uint32_t dco_mid = (dco_min + dco_max) / 2;
	uint64_t afe = (uint64_t)clock_khz * 5;
	uint32_t best_div = 0;
	uint64_t best_dco = 0, best_centrality = ~0ULL;
	uint32_t p, q, k;

	for (unsigned i = 0; i < sizeof(dividers); i++) {
		uint64_t dco = afe * dividers[i];
		if (dco > dco_max || dco < dco_min)
			continue;
		uint64_t centrality = udiff(dco, dco_mid);
		if (centrality < best_centrality) {
			best_centrality = centrality;
			best_div = dividers[i];
			best_dco = dco;
		}
	}
	if (!best_div)
		return -1;
	icl_wrpll_multipliers(best_div, &p, &q, &k);
	if (!p || !q || !k)
		return -1;
	uint64_t dco_fp = (best_dco << 15) / icl_wrpll_ref(ref_khz);
	out->dco_integer = (uint16_t)(dco_fp >> 15);
	out->dco_fraction = (uint16_t)(dco_fp & 0x7fff);
	out->pdiv = p == 2 ? 0x1 : p == 3 ? 0x2 : p == 5 ? 0x4 : 0x8;
	out->kdiv = k == 1 ? 1 : k == 2 ? 2 : 4;
	out->qdiv_ratio = (uint8_t)q;
	out->qdiv_mode = q == 1 ? 0 : 1;
	return 0;
}

uint32_t icl_combo_pll_khz(const struct icl_pll_params *p, uint32_t ref_khz)
{
	uint32_t ref = icl_wrpll_ref(ref_khz);
	uint32_t pdiv, kdiv, qdiv;
	switch (p->pdiv) {
	case 0x1: pdiv = 2; break;
	case 0x2: pdiv = 3; break;
	case 0x4: pdiv = 5; break;
	case 0x8: pdiv = 7; break;
	default: return 0;
	}
	switch (p->kdiv) {
	case 1: kdiv = 1; break;
	case 2: kdiv = 2; break;
	case 4: kdiv = 3; break;
	default: return 0;
	}
	qdiv = p->qdiv_mode ? p->qdiv_ratio : 1;
	if (!qdiv)
		return 0;
	uint32_t dco = p->dco_integer * ref + (uint32_t)(((uint64_t)p->dco_fraction * ref) / 0x8000);
	/* the port clock is the DCO over the dividers, over 5 (AFE) */
	return dco / (pdiv * kdiv * qdiv * 5);
}

/* ---- Skylake: the WRPLL in HDMI mode --------------------------------------------- */

struct skl_wrpll_ctx {
	uint64_t min_deviation; /* best so far, in 0.01% */
	uint64_t central_freq;
	uint64_t dco_freq;
	uint32_t p;
};

/* The DCO must be within +1%/-6% of its central frequency. */
#define SKL_DCO_MAX_PDEVIATION 100
#define SKL_DCO_MAX_NDEVIATION 600

static void skl_wrpll_try_divider(struct skl_wrpll_ctx *ctx, uint64_t central_freq,
				  uint64_t dco_freq, uint32_t divider)
{
	uint64_t deviation = 10000 * udiff(dco_freq, central_freq) / central_freq;

	if (dco_freq >= central_freq) {
		if (deviation < SKL_DCO_MAX_PDEVIATION && deviation < ctx->min_deviation) {
			ctx->min_deviation = deviation;
			ctx->central_freq = central_freq;
			ctx->dco_freq = dco_freq;
			ctx->p = divider;
		}
	} else if (deviation < SKL_DCO_MAX_NDEVIATION && deviation < ctx->min_deviation) {
		ctx->min_deviation = deviation;
		ctx->central_freq = central_freq;
		ctx->dco_freq = dco_freq;
		ctx->p = divider;
	}
}

static void skl_wrpll_multipliers(uint32_t p, uint32_t *p0, uint32_t *p1, uint32_t *p2)
{
	if (p % 2 == 0) {
		uint32_t half = p / 2;
		if (half == 1 || half == 2 || half == 3 || half == 5) {
			*p0 = 2; *p1 = 1; *p2 = half;
		} else if (half % 2 == 0) {
			*p0 = 2; *p1 = half / 2; *p2 = 2;
		} else if (half % 3 == 0) {
			*p0 = 3; *p1 = half / 3; *p2 = 2;
		} else if (half % 7 == 0) {
			*p0 = 7; *p1 = half / 7; *p2 = 2;
		}
	} else if (p == 3 || p == 9) { /* 3, 5, 7, 9, 15, 21, 35 */
		*p0 = 3; *p1 = 1; *p2 = p / 3;
	} else if (p == 5 || p == 7) {
		*p0 = p; *p1 = 1; *p2 = 1;
	} else if (p == 15) {
		*p0 = 3; *p1 = 1; *p2 = 5;
	} else if (p == 21) {
		*p0 = 7; *p1 = 1; *p2 = 3;
	} else if (p == 35) {
		*p0 = 7; *p1 = 1; *p2 = 5;
	}
}

int skl_wrpll_calc(uint32_t clock_khz, uint32_t ref_khz, struct skl_wrpll_params *out)
{
	static const uint64_t dco_central_freq[3] = { 8400000000ULL, 9000000000ULL,
						      9600000000ULL };
	static const uint8_t even_dividers[] = { 4, 6, 8, 10, 12, 14, 16, 18, 20,
						 24, 28, 30, 32, 36, 40, 42, 44,
						 48, 52, 54, 56, 60, 64, 66, 68,
						 70, 72, 76, 78, 80, 84, 88, 90,
						 92, 96, 98 };
	static const uint8_t odd_dividers[] = { 3, 5, 7, 9, 15, 21, 35 };
	static const struct {
		const uint8_t *list;
		unsigned n;
	} dividers[] = {
		{ even_dividers, sizeof(even_dividers) },
		{ odd_dividers, sizeof(odd_dividers) },
	};
	struct skl_wrpll_ctx ctx = { .min_deviation = ~0ULL };
	uint64_t afe_clock = (uint64_t)clock_khz * 1000 * 5; /* 5x the pixel clock, Hz */
	uint32_t p0 = 0, p1 = 0, p2 = 0;

	for (unsigned d = 0; d < 2; d++) {
		for (unsigned dco = 0; dco < 3; dco++) {
			for (unsigned i = 0; i < dividers[d].n; i++) {
				uint32_t p = dividers[d].list[i];
				skl_wrpll_try_divider(&ctx, dco_central_freq[dco], p * afe_clock, p);
				/* nothing beats a zero deviation */
				if (ctx.min_deviation == 0)
					goto found;
			}
		}
found:
		/* an even divider, if one fits, is preferred */
		if (d == 0 && ctx.p)
			break;
	}
	if (!ctx.p)
		return -1;
	skl_wrpll_multipliers(ctx.p, &p0, &p1, &p2);
	if (!p0 || !p1 || !p2)
		return -1;

	switch (ctx.central_freq) {
	case 9600000000ULL: out->central_freq = 0; break;
	case 9000000000ULL: out->central_freq = 1; break;
	default: out->central_freq = 3; break; /* 8400 MHz */
	}
	switch (p0) {
	case 1: out->pdiv = 0; break;
	case 2: out->pdiv = 1; break;
	case 3: out->pdiv = 2; break;
	default: out->pdiv = 4; break; /* 7 */
	}
	switch (p2) {
	case 5: out->kdiv = 0; break;
	case 2: out->kdiv = 1; break;
	case 3: out->kdiv = 2; break;
	default: out->kdiv = 3; break; /* 1 */
	}
	out->qdiv_ratio = p1;
	out->qdiv_mode = p1 == 1 ? 0 : 1;

	/* the DCO in reference multiples: integer part, then the fraction
	 * in 1/0x8000 steps (worked in units of a millionth of a
	 * reference period) */
	uint64_t dco_freq = (uint64_t)p0 * p1 * p2 * afe_clock;
	out->dco_integer = (uint32_t)(dco_freq / ((uint64_t)ref_khz * 1000));
	out->dco_fraction = (uint32_t)(((dco_freq / (ref_khz / 1000)) -
					(uint64_t)out->dco_integer * 1000000) * 0x8000 / 1000000);
	return 0;
}

uint32_t skl_wrpll_khz(uint32_t cfgcr1, uint32_t cfgcr2, uint32_t ref_khz)
{
	uint32_t p0, p1, p2;
	switch ((cfgcr2 >> 2) & 7) {
	case 0: p0 = 1; break;
	case 1: p0 = 2; break;
	case 2: p0 = 3; break;
	case 4: /* 7 */
	case 5: /* 7 with bit 0 set: some firmware writes it; the hardware ignores the bit */
		p0 = 7;
		break;
	default: return 0;
	}
	switch ((cfgcr2 >> 5) & 3) {
	case 0: p2 = 5; break;
	case 1: p2 = 2; break;
	case 2: p2 = 3; break;
	default: p2 = 1; break;
	}
	p1 = (cfgcr2 & (1u << 7)) ? (cfgcr2 >> 8) & 0xff : 1;
	if (!p1)
		return 0;
	uint32_t dco = (cfgcr1 & 0x1ff) * ref_khz;
	dco += ((cfgcr1 >> 9) & 0x7fff) * ref_khz / 0x8000;
	return dco / (p0 * p1 * p2 * 5);
}

/* ---- Haswell/Broadwell: the WRPLL from the LCPLL --------------------------------- */

#define LC_FREQ 2700
#define LC_FREQ_2K ((uint64_t)LC_FREQ * 2000)
#define P_MIN 2
#define P_MAX 64
#define P_INC 2
/* what the PLL is good for: its reference after R between 48 and 400
 * MHz, its VCO between 2.4 and 4.8 GHz */
#define REF_MIN 48
#define REF_MAX 400
#define VCO_MIN 2400
#define VCO_MAX 4800

struct hsw_wrpll_rnp {
	uint32_t p, n2, r2;
};

/* How far from the requested clock (in ppm) the common modes may land. */
static uint32_t hsw_wrpll_budget(uint32_t clock_hz)
{
	switch (clock_hz) {
	case 25175000: case 25200000: case 27000000: case 27027000:
	case 37762500: case 37800000: case 40500000: case 40541000:
	case 54000000: case 54054000: case 59341000: case 59400000:
	case 72000000: case 74176000: case 74250000: case 81000000:
	case 81081000: case 89012000: case 89100000: case 108000000:
	case 108108000: case 111264000: case 111375000: case 148352000:
	case 148500000: case 162000000: case 162162000: case 222525000:
	case 222750000: case 296703000: case 297000000:
		return 0;
	case 233500000: case 245250000: case 247750000: case 253250000:
	case 298000000:
		return 1500;
	case 169128000: case 169500000: case 179500000: case 202000000:
		return 2000;
	case 256250000: case 262500000: case 270000000: case 272500000:
	case 273750000: case 280750000: case 281250000: case 286000000:
	case 291750000:
		return 4000;
	case 267250000: case 268500000:
		return 5000;
	default:
		return 1000;
	}
}

static void hsw_wrpll_update_rnp(uint64_t freq2k, uint32_t budget, uint32_t r2, uint32_t n2,
				 uint32_t p, struct hsw_wrpll_rnp *best)
{
	uint64_t a, b, c, d, diff, diff_best;

	if (best->p == 0) {
		best->p = p;
		best->n2 = n2;
		best->r2 = r2;
		return;
	}
	/* The output is LC_FREQ * N / (P * R), compared with freq2k in
	 * ppm: above the budget, the closer one wins; within it, the one
	 * with the higher reference times VCO, N / (P * R^2). */
	a = freq2k * budget * p * r2;
	b = freq2k * budget * best->p * best->r2;
	diff = udiff(freq2k * p * r2, LC_FREQ_2K * n2);
	diff_best = udiff(freq2k * best->p * best->r2, LC_FREQ_2K * best->n2);
	c = 1000000 * diff;
	d = 1000000 * diff_best;

	if (a < c && b < d) {
		/* both above the budget: the closer */
		if (best->p * best->r2 * diff < p * r2 * diff_best) {
			best->p = p;
			best->n2 = n2;
			best->r2 = r2;
		}
	} else if (a >= c && b < d) {
		/* this one within, the best so far not */
		best->p = p;
		best->n2 = n2;
		best->r2 = r2;
	} else if (a >= c && b >= d) {
		/* both within: the higher n2 / (r2 * r2) */
		if (n2 * best->r2 * best->r2 > best->n2 * r2 * r2) {
			best->p = p;
			best->n2 = n2;
			best->r2 = r2;
		}
	}
}

void hsw_wrpll_calc(uint32_t clock_khz, uint32_t *r2_out, uint32_t *n2_out, uint32_t *p_out)
{
	uint32_t clock_hz = clock_khz * 1000;
	uint64_t freq2k = clock_hz / 100;
	uint32_t budget = hsw_wrpll_budget(clock_hz);
	struct hsw_wrpll_rnp best = { 0, 0, 0 };

	/* 540 MHz: the LCPLL passed straight through */
	if (freq2k == 5400000) {
		*n2_out = 2;
		*p_out = 1;
		*r2_out = 2;
		return;
	}
	/* Ref = LC_FREQ / R within REF_MIN..REF_MAX, with R2 = 2R; VCO =
	 * N * Ref within VCO_MIN..VCO_MAX, with N2 = 2N. */
	for (uint32_t r2 = LC_FREQ * 2 / REF_MAX + 1; r2 <= LC_FREQ * 2 / REF_MIN; r2++) {
		for (uint32_t n2 = VCO_MIN * r2 / LC_FREQ + 1; n2 <= VCO_MAX * r2 / LC_FREQ; n2++) {
			for (uint32_t p = P_MIN; p <= P_MAX; p += P_INC)
				hsw_wrpll_update_rnp(freq2k, budget, r2, n2, p, &best);
		}
	}
	*n2_out = best.n2;
	*p_out = best.p;
	*r2_out = best.r2;
}

uint32_t hsw_wrpll_khz(uint32_t r2, uint32_t n2, uint32_t p)
{
	if (!p || !r2)
		return 0;
	/* p and r carry a fixed-point part */
	return (2700000u * n2 / 10) / (p * r2) * 2;
}

/* ---- Broxton / Gemini Lake: the port PLL ---------------------------------------- */

#define BXT_REFCLK_KHZ 100000

uint32_t bxt_pll_calc_params(struct bxt_pll_dividers *d)
{
	uint64_t m = (uint64_t)d->m1 * d->m2;
	uint32_t p = d->p1 * d->p2 * 5;
	uint64_t den = (uint64_t)d->n << 22;

	d->vco = den ? (uint32_t)(((uint64_t)BXT_REFCLK_KHZ * m + den / 2) / den) : 0;
	d->dot = p ? (d->vco + p / 2) / p : 0;
	return d->dot;
}

/* the DisplayPort rates (m2 in 22.22 fixed point) */
static const struct bxt_pll_dividers bxt_dp_clk_val[] = {
	{ 4, 2, 1, 2, 0x819999a, 0, 162000 }, /* 32.4 */
	{ 4, 1, 1, 2, 0x6c00000, 0, 270000 }, /* 27.0 */
	{ 2, 1, 1, 2, 0x6c00000, 0, 540000 }, /* 27.0 */
	{ 3, 2, 1, 2, 0x819999a, 0, 216000 }, /* 32.4 */
	{ 4, 1, 1, 2, 0x6133333, 0, 243000 }, /* 24.3 */
	{ 4, 1, 1, 2, 0x819999a, 0, 324000 }, /* 32.4 */
	{ 3, 1, 1, 2, 0x819999a, 0, 432000 }, /* 32.4 */
};

int bxt_dp_dividers(uint32_t link_khz, struct bxt_pll_dividers *out)
{
	for (unsigned i = 0; i < sizeof(bxt_dp_clk_val) / sizeof(bxt_dp_clk_val[0]); i++) {
		if (bxt_dp_clk_val[i].dot != link_khz)
			continue;
		*out = bxt_dp_clk_val[i];
		bxt_pll_calc_params(out);
		return 0;
	}
	return -1;
}

/* the limits the dividers must keep */
#define BXT_DOT_MIN 25000
#define BXT_DOT_MAX 594000
#define BXT_VCO_MIN 4800000
#define BXT_VCO_MAX 6700000
#define BXT_P1_MIN 2
#define BXT_P1_MAX 4
#define BXT_P2_SLOW 1
#define BXT_P2_FAST 20
#define BXT_M2_MIN (2u << 22)
#define BXT_M2_MAX (255u << 22)

static int bxt_pll_valid(const struct bxt_pll_dividers *d)
{
	if (d->n != 1 || d->m1 != 2)
		return 0;
	if (d->p1 < BXT_P1_MIN || d->p1 > BXT_P1_MAX)
		return 0;
	if (d->m2 < BXT_M2_MIN || d->m2 > BXT_M2_MAX)
		return 0;
	if (d->vco < BXT_VCO_MIN || d->vco > BXT_VCO_MAX)
		return 0;
	if (d->dot < BXT_DOT_MIN || d->dot > BXT_DOT_MAX)
		return 0;
	return 1;
}

int bxt_hdmi_dividers(uint32_t clock_khz, struct bxt_pll_dividers *out)
{
	struct bxt_pll_dividers d, best = { 0, 0, 0, 0, 0, 0, 0 };
	uint32_t best_error_ppm = 1000000;
	int found = 0;

	if (!clock_khz)
		return -1;
	/* n is always 1 and m1 always 2 from the 100 MHz reference */
	d.n = 1;
	d.m1 = 2;
	for (d.p1 = BXT_P1_MAX; d.p1 >= BXT_P1_MIN; d.p1--) {
		for (d.p2 = BXT_P2_FAST; d.p2 >= BXT_P2_SLOW; d.p2 -= d.p2 > 10 ? 2 : 1) {
			uint32_t p = d.p1 * d.p2 * 5;
			uint64_t m2 = (((uint64_t)clock_khz * p * d.n << 22) +
				       BXT_REFCLK_KHZ * d.m1 / 2) / (BXT_REFCLK_KHZ * d.m1);
			if (m2 > 0x7fffffffu / d.m1)
				continue;
			d.m2 = (uint32_t)m2;
			bxt_pll_calc_params(&d);
			if (!bxt_pll_valid(&d))
				continue;
			/* A better p over a smaller error when the error is
			 * small anyway; else the error has to improve by more
			 * than 10 ppm. */
			uint32_t error_ppm = (uint32_t)(1000000ULL * udiff(clock_khz, d.dot) /
							clock_khz);
			int better;
			if (error_ppm < 100 && p > best.p1 * best.p2 * 5) {
				error_ppm = 0;
				better = 1;
			} else {
				better = error_ppm + 10 < best_error_ppm;
			}
			if (!better)
				continue;
			best = d;
			best_error_ppm = error_ppm;
			found = 1;
		}
	}
	if (!found)
		return -1;
	*out = best;
	return 0;
}
