// LikeOS -- the pixel clock PLLs of the Intel parts before DDI.
//
// Every pipe of the GMCH parts (gen2-4) has a DPLL of its own: a
// reference clock (48 MHz on gen2, 96 MHz later, or the panel's spread
// spectrum source), an N divider, a feedback divider made of M1 and M2,
// and two post dividers P1 and P2.  The dot clock is
// ref * (5 * (M1 + 2) + (M2 + 2)) / (N + 2) / (P1 * P2); each platform
// limits every term, the VCO and the dot clock, and the dividers are
// found by a search against those limits.  Pineview's feedback is one
// combined counter and its N is a ring counter.  Ironlake to Ivy Bridge
// moved the PLLs into the PCH (two of them; Ibex Peak ties one to each
// transcoder, Cougar Point lets the transcoders pick and share), with a
// 120 MHz reference and a feedback "CB tune" bit.  Valleyview and
// Cherryview keep the pipe DPLL register but put the dividers into the
// DPIO PHY; their searches live here, their programming in
// intel_legacy_vlv.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2021 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the platforms' limits ------------------------------------------------------ */

struct lg_limit {
	struct {
		int min, max;
	} dot, vco, n, m, m1, m2, p, p1;
	struct {
		int dot_limit;
		int p2_slow, p2_fast;
	} p2;
};

static const struct lg_limit limits_i8xx_dac = {
	.dot = { 25000, 350000 }, .vco = { 908000, 1512000 },
	.n = { 2, 16 }, .m = { 96, 140 }, .m1 = { 18, 26 }, .m2 = { 6, 16 },
	.p = { 4, 128 }, .p1 = { 2, 33 },
	.p2 = { 165000, 4, 2 },
};

static const struct lg_limit limits_i8xx_dvo = {
	.dot = { 25000, 350000 }, .vco = { 908000, 1512000 },
	.n = { 2, 16 }, .m = { 96, 140 }, .m1 = { 18, 26 }, .m2 = { 6, 16 },
	.p = { 4, 128 }, .p1 = { 2, 33 },
	.p2 = { 165000, 4, 4 },
};

static const struct lg_limit limits_i8xx_lvds = {
	.dot = { 25000, 350000 }, .vco = { 908000, 1512000 },
	.n = { 2, 16 }, .m = { 96, 140 }, .m1 = { 18, 26 }, .m2 = { 6, 16 },
	.p = { 4, 128 }, .p1 = { 1, 6 },
	.p2 = { 165000, 14, 7 },
};

static const struct lg_limit limits_i9xx_sdvo = {
	.dot = { 20000, 400000 }, .vco = { 1400000, 2800000 },
	.n = { 1, 6 }, .m = { 70, 120 }, .m1 = { 8, 18 }, .m2 = { 3, 7 },
	.p = { 5, 80 }, .p1 = { 1, 8 },
	.p2 = { 200000, 10, 5 },
};

static const struct lg_limit limits_i9xx_lvds = {
	.dot = { 20000, 400000 }, .vco = { 1400000, 2800000 },
	.n = { 1, 6 }, .m = { 70, 120 }, .m1 = { 8, 18 }, .m2 = { 3, 7 },
	.p = { 7, 98 }, .p1 = { 1, 8 },
	.p2 = { 112000, 14, 7 },
};

static const struct lg_limit limits_g4x_sdvo = {
	.dot = { 25000, 270000 }, .vco = { 1750000, 3500000 },
	.n = { 1, 4 }, .m = { 104, 138 }, .m1 = { 17, 23 }, .m2 = { 5, 11 },
	.p = { 10, 30 }, .p1 = { 1, 3 },
	.p2 = { 270000, 10, 10 },
};

static const struct lg_limit limits_g4x_hdmi = {
	.dot = { 22000, 400000 }, .vco = { 1750000, 3500000 },
	.n = { 1, 4 }, .m = { 104, 138 }, .m1 = { 16, 23 }, .m2 = { 5, 11 },
	.p = { 5, 80 }, .p1 = { 1, 8 },
	.p2 = { 165000, 10, 5 },
};

static const struct lg_limit limits_g4x_single_lvds = {
	.dot = { 20000, 115000 }, .vco = { 1750000, 3500000 },
	.n = { 1, 3 }, .m = { 104, 138 }, .m1 = { 17, 23 }, .m2 = { 5, 11 },
	.p = { 28, 112 }, .p1 = { 2, 8 },
	.p2 = { 0, 14, 14 },
};

static const struct lg_limit limits_g4x_dual_lvds = {
	.dot = { 80000, 224000 }, .vco = { 1750000, 3500000 },
	.n = { 1, 3 }, .m = { 104, 138 }, .m1 = { 17, 23 }, .m2 = { 5, 11 },
	.p = { 14, 42 }, .p1 = { 2, 6 },
	.p2 = { 0, 7, 7 },
};

/* Pineview: N is a ring counter, the feedback one combined divider
 * (treated as m2, m1 unused). */
static const struct lg_limit limits_pnv_sdvo = {
	.dot = { 20000, 400000 }, .vco = { 1700000, 3500000 },
	.n = { 3, 6 }, .m = { 2, 256 }, .m1 = { 0, 0 }, .m2 = { 0, 254 },
	.p = { 5, 80 }, .p1 = { 1, 8 },
	.p2 = { 200000, 10, 5 },
};

static const struct lg_limit limits_pnv_lvds = {
	.dot = { 20000, 400000 }, .vco = { 1700000, 3500000 },
	.n = { 3, 6 }, .m = { 2, 256 }, .m1 = { 0, 0 }, .m2 = { 0, 254 },
	.p = { 7, 112 }, .p1 = { 1, 8 },
	.p2 = { 112000, 14, 14 },
};

/* Ironlake to Ivy Bridge: N, M1 and M2 are kept as (value - 2). */
static const struct lg_limit limits_ilk_dac = {
	.dot = { 25000, 350000 }, .vco = { 1760000, 3510000 },
	.n = { 1, 5 }, .m = { 79, 127 }, .m1 = { 12, 22 }, .m2 = { 5, 9 },
	.p = { 5, 80 }, .p1 = { 1, 8 },
	.p2 = { 225000, 10, 5 },
};

static const struct lg_limit limits_ilk_single_lvds = {
	.dot = { 25000, 350000 }, .vco = { 1760000, 3510000 },
	.n = { 1, 3 }, .m = { 79, 118 }, .m1 = { 12, 22 }, .m2 = { 5, 9 },
	.p = { 28, 112 }, .p1 = { 2, 8 },
	.p2 = { 225000, 14, 14 },
};

static const struct lg_limit limits_ilk_dual_lvds = {
	.dot = { 25000, 350000 }, .vco = { 1760000, 3510000 },
	.n = { 1, 3 }, .m = { 79, 127 }, .m1 = { 12, 22 }, .m2 = { 5, 9 },
	.p = { 14, 56 }, .p1 = { 2, 8 },
	.p2 = { 225000, 7, 7 },
};

static const struct lg_limit limits_ilk_single_lvds_100m = {
	.dot = { 25000, 350000 }, .vco = { 1760000, 3510000 },
	.n = { 1, 2 }, .m = { 79, 126 }, .m1 = { 12, 22 }, .m2 = { 5, 9 },
	.p = { 28, 112 }, .p1 = { 2, 8 },
	.p2 = { 225000, 14, 14 },
};

static const struct lg_limit limits_ilk_dual_lvds_100m = {
	.dot = { 25000, 350000 }, .vco = { 1760000, 3510000 },
	.n = { 1, 3 }, .m = { 79, 126 }, .m1 = { 12, 22 }, .m2 = { 5, 9 },
	.p = { 14, 42 }, .p1 = { 2, 6 },
	.p2 = { 225000, 7, 7 },
};

/* Valleyview / Cherryview: the limits of the fast (data rate) clock,
 * which are the strictest; p2 slow is the minimum, fast the maximum. */
static const struct lg_limit limits_vlv = {
	.dot = { 25000, 270000 }, .vco = { 4000000, 6000000 },
	.n = { 1, 7 }, .m1 = { 2, 3 }, .m2 = { 11, 156 }, .p1 = { 2, 3 },
	.p2 = { 0, 2, 20 },
};

static const struct lg_limit limits_chv = {
	.dot = { 25000, 540000 }, .vco = { 4800000, 6480000 },
	.n = { 1, 1 }, .m1 = { 2, 2 }, .m2 = { 24 << 22, 175 << 22 }, .p1 = { 2, 4 },
	.p2 = { 0, 1, 14 },
};

/* ---- what a set of dividers makes ------------------------------------------------ */

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

static int div_round_closest_i(int64_t n, int64_t d)
{
	if (!d)
		return 0;
	return (int)((n + d / 2) / d);
}

int lg_pnv_calc_dpll_params(int refclk, struct lg_dpll *clock)
{
	clock->m = clock->m2 + 2;
	clock->p = clock->p1 * clock->p2;
	clock->vco = clock->n == 0 ? 0 : div_round_closest_i((int64_t)refclk * clock->m, clock->n);
	clock->dot = clock->p == 0 ? 0 : div_round_closest_i(clock->vco, clock->p);
	return clock->dot;
}

static int i9xx_dpll_compute_m(const struct lg_dpll *dpll)
{
	return 5 * (dpll->m1 + 2) + (dpll->m2 + 2);
}

int lg_i9xx_calc_dpll_params(int refclk, struct lg_dpll *clock)
{
	clock->m = i9xx_dpll_compute_m(clock);
	clock->p = clock->p1 * clock->p2;
	clock->vco = clock->n + 2 == 0 ? 0 :
					 div_round_closest_i((int64_t)refclk * clock->m, clock->n + 2);
	clock->dot = clock->p == 0 ? 0 : div_round_closest_i(clock->vco, clock->p);
	return clock->dot;
}

int lg_vlv_calc_dpll_params(int refclk, struct lg_dpll *clock)
{
	clock->m = clock->m1 * clock->m2;
	clock->p = clock->p1 * clock->p2 * 5;
	clock->vco = clock->n == 0 ? 0 : div_round_closest_i((int64_t)refclk * clock->m, clock->n);
	clock->dot = clock->p == 0 ? 0 : div_round_closest_i(clock->vco, clock->p);
	return clock->dot;
}

int lg_chv_calc_dpll_params(int refclk, struct lg_dpll *clock)
{
	clock->m = clock->m1 * clock->m2;
	clock->p = clock->p1 * clock->p2 * 5;
	if (clock->n == 0) {
		clock->vco = 0;
	} else {
		uint64_t num = (uint64_t)(uint32_t)refclk * (uint64_t)(uint32_t)clock->m;
		uint64_t den = (uint64_t)clock->n << 22;
		clock->vco = (int)((num + den / 2) / den);
	}
	clock->dot = clock->p == 0 ? 0 : div_round_closest_i(clock->vco, clock->p);
	return clock->dot;
}

/* Do the dividers stay inside every limit of the platform? */
static int pll_is_valid(struct lg_display *d, const struct lg_limit *limit,
			const struct lg_dpll *clock)
{
	if (clock->n < limit->n.min || limit->n.max < clock->n)
		return 0;
	if (clock->p1 < limit->p1.min || limit->p1.max < clock->p1)
		return 0;
	if (clock->m2 < limit->m2.min || limit->m2.max < clock->m2)
		return 0;
	if (clock->m1 < limit->m1.min || limit->m1.max < clock->m1)
		return 0;
	if (!d->is_pnv && !d->is_vlv && !d->is_chv)
		if (clock->m1 <= clock->m2)
			return 0;
	if (!d->is_vlv && !d->is_chv) {
		if (clock->p < limit->p.min || limit->p.max < clock->p)
			return 0;
		if (clock->m < limit->m.min || limit->m.max < clock->m)
			return 0;
	}
	if (clock->vco < limit->vco.min || limit->vco.max < clock->vco)
		return 0;
	if (clock->dot < limit->dot.min || limit->dot.max < clock->dot)
		return 0;
	return 1;
}

/* P2: an LVDS panel keeps what its channel count needs (single or dual
 * is not switched); everything else picks by the dot clock. */
static int select_p2(struct lg_display *d, const struct lg_limit *limit,
		     const struct lg_config *cfg, int target)
{
	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS))
		return d->lvds_dual ? limit->p2.p2_fast : limit->p2.p2_slow;
	return target < limit->p2.dot_limit ? limit->p2.p2_slow : limit->p2.p2_fast;
}

/* The closest dot clock, trying every divider combination. */
static int i9xx_find_best_dpll(struct lg_display *d, const struct lg_limit *limit,
			       const struct lg_config *cfg, int target, int refclk,
			       struct lg_dpll *best)
{
	struct lg_dpll clock;
	int err = target;

	mm_memset(best, 0, sizeof(*best));
	mm_memset(&clock, 0, sizeof(clock));
	clock.p2 = select_p2(d, limit, cfg, target);
	for (clock.m1 = limit->m1.min; clock.m1 <= limit->m1.max; clock.m1++) {
		for (clock.m2 = limit->m2.min; clock.m2 <= limit->m2.max; clock.m2++) {
			if (clock.m2 >= clock.m1)
				break;
			for (clock.n = limit->n.min; clock.n <= limit->n.max; clock.n++) {
				for (clock.p1 = limit->p1.min; clock.p1 <= limit->p1.max;
				     clock.p1++) {
					lg_i9xx_calc_dpll_params(refclk, &clock);
					if (!pll_is_valid(d, limit, &clock))
						continue;
					int this_err = iabs(clock.dot - target);
					if (this_err < err) {
						*best = clock;
						err = this_err;
					}
				}
			}
		}
	}
	return err != target;
}

static int pnv_find_best_dpll(struct lg_display *d, const struct lg_limit *limit,
			      const struct lg_config *cfg, int target, int refclk,
			      struct lg_dpll *best)
{
	struct lg_dpll clock;
	int err = target;

	mm_memset(best, 0, sizeof(*best));
	mm_memset(&clock, 0, sizeof(clock));
	clock.p2 = select_p2(d, limit, cfg, target);
	for (clock.m1 = limit->m1.min; clock.m1 <= limit->m1.max; clock.m1++) {
		for (clock.m2 = limit->m2.min; clock.m2 <= limit->m2.max; clock.m2++) {
			for (clock.n = limit->n.min; clock.n <= limit->n.max; clock.n++) {
				for (clock.p1 = limit->p1.min; clock.p1 <= limit->p1.max;
				     clock.p1++) {
					lg_pnv_calc_dpll_params(refclk, &clock);
					if (!pll_is_valid(d, limit, &clock))
						continue;
					int this_err = iabs(clock.dot - target);
					if (this_err < err) {
						*best = clock;
						err = this_err;
					}
				}
			}
		}
	}
	return err != target;
}

/* G4X and the PCH PLLs: the hardware prefers a small N and large M1/M2
 * over a little precision, so the search keeps the first good enough
 * (within ~0.6%) result at the smallest N. */
static int g4x_find_best_dpll(struct lg_display *d, const struct lg_limit *limit,
			      const struct lg_config *cfg, int target, int refclk,
			      struct lg_dpll *best)
{
	struct lg_dpll clock;
	int max_n;
	int found = 0;
	int err_most = (target >> 8) + (target >> 9);

	mm_memset(best, 0, sizeof(*best));
	mm_memset(&clock, 0, sizeof(clock));
	clock.p2 = select_p2(d, limit, cfg, target);
	max_n = limit->n.max;
	for (clock.n = limit->n.min; clock.n <= max_n; clock.n++) {
		for (clock.m1 = limit->m1.max; clock.m1 >= limit->m1.min; clock.m1--) {
			for (clock.m2 = limit->m2.max; clock.m2 >= limit->m2.min; clock.m2--) {
				for (clock.p1 = limit->p1.max; clock.p1 >= limit->p1.min;
				     clock.p1--) {
					lg_i9xx_calc_dpll_params(refclk, &clock);
					if (!pll_is_valid(d, limit, &clock))
						continue;
					int this_err = iabs(clock.dot - target);
					if (this_err < err_most) {
						*best = clock;
						err_most = this_err;
						max_n = clock.n;
						found = 1;
					}
				}
			}
		}
	}
	return found;
}

/* Valleyview/Cherryview: is this set better than the best so far?  A
 * bigger P is preferred while the error stays small; Cherryview cares
 * only about P. */
static int vlv_pll_is_optimal(struct lg_display *d, int target, const struct lg_dpll *calc,
			      const struct lg_dpll *best, unsigned int best_error_ppm,
			      unsigned int *error_ppm)
{
	if (d->is_chv) {
		*error_ppm = 0;
		return calc->p > best->p;
	}
	if (!target)
		return 0;
	*error_ppm = (unsigned int)(1000000ULL * (uint64_t)iabs(target - calc->dot) /
				    (uint64_t)target);
	if (*error_ppm < 100 && calc->p > best->p) {
		*error_ppm = 0;
		return 1;
	}
	return *error_ppm + 10 < best_error_ppm;
}

static int vlv_find_best_dpll(struct lg_display *d, const struct lg_limit *limit, int target,
			      int refclk, struct lg_dpll *best)
{
	struct lg_dpll clock;
	unsigned int bestppm = 1000000;
	int max_n = limit->n.max < refclk / 19200 ? limit->n.max : refclk / 19200;
	int found = 0;

	mm_memset(best, 0, sizeof(*best));
	mm_memset(&clock, 0, sizeof(clock));
	for (clock.n = limit->n.min; clock.n <= max_n; clock.n++) {
		for (clock.p1 = limit->p1.max; clock.p1 >= limit->p1.min; clock.p1--) {
			for (clock.p2 = limit->p2.p2_fast; clock.p2 >= limit->p2.p2_slow;
			     clock.p2 -= clock.p2 > 10 ? 2 : 1) {
				clock.p = clock.p1 * clock.p2 * 5;
				for (clock.m1 = limit->m1.min; clock.m1 <= limit->m1.max;
				     clock.m1++) {
					unsigned int ppm;

					clock.m2 = div_round_closest_i((int64_t)target * clock.p *
									       clock.n,
								       (int64_t)refclk * clock.m1);
					lg_vlv_calc_dpll_params(refclk, &clock);
					if (!pll_is_valid(d, limit, &clock))
						continue;
					if (!vlv_pll_is_optimal(d, target, &clock, best, bestppm,
								&ppm))
						continue;
					*best = clock;
					bestppm = ppm;
					found = 1;
				}
			}
		}
	}
	return found;
}

static int chv_find_best_dpll(struct lg_display *d, const struct lg_limit *limit, int target,
			      int refclk, struct lg_dpll *best)
{
	struct lg_dpll clock;
	unsigned int best_error_ppm = 1000000;
	int found = 0;

	mm_memset(best, 0, sizeof(*best));
	mm_memset(&clock, 0, sizeof(clock));
	/* N is always 1 and M1 always 2 with the 100 MHz reference. */
	clock.n = 1;
	clock.m1 = 2;
	for (clock.p1 = limit->p1.max; clock.p1 >= limit->p1.min; clock.p1--) {
		for (clock.p2 = limit->p2.p2_fast; clock.p2 >= limit->p2.p2_slow;
		     clock.p2 -= clock.p2 > 10 ? 2 : 1) {
			unsigned int error_ppm;
			uint64_t num, den, m2;

			clock.p = clock.p1 * clock.p2 * 5;
			num = ((uint64_t)(uint32_t)target * (uint64_t)(clock.p * clock.n)) << 22;
			den = (uint64_t)refclk * (uint64_t)clock.m1;
			m2 = (num + den / 2) / den;
			if (m2 > (uint64_t)(0x7fffffff / clock.m1))
				continue;
			clock.m2 = (int)m2;
			lg_chv_calc_dpll_params(refclk, &clock);
			if (!pll_is_valid(d, limit, &clock))
				continue;
			if (!vlv_pll_is_optimal(d, target, &clock, best, best_error_ppm,
						&error_ppm))
				continue;
			*best = clock;
			best_error_ppm = error_ppm;
			found = 1;
		}
	}
	return found;
}

/* ---- register values ------------------------------------------------------------- */

static uint32_t i9xx_dpll_compute_fp(const struct lg_dpll *dpll)
{
	return (uint32_t)dpll->n << 16 | (uint32_t)dpll->m1 << 8 | (uint32_t)dpll->m2;
}

static uint32_t pnv_dpll_compute_fp(const struct lg_dpll *dpll)
{
	return (1u << dpll->n) << 16 | (uint32_t)dpll->m2;
}

static uint32_t i965_dpll_md(const struct lg_config *cfg)
{
	return (uint32_t)(cfg->pixel_multiplier - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT;
}

static uint32_t p2_bits(int p2)
{
	switch (p2) {
	case 5:
		return DPLL_DAC_SERIAL_P2_CLOCK_DIV_5;
	case 7:
		return DPLLB_LVDS_P2_CLOCK_DIV_7;
	case 10:
		return DPLL_DAC_SERIAL_P2_CLOCK_DIV_10;
	case 14:
		return DPLLB_LVDS_P2_CLOCK_DIV_14;
	default:
		return 0;
	}
}

static uint32_t i9xx_dpll(struct lg_display *d, const struct lg_config *cfg,
			  const struct lg_dpll *clock)
{
	uint32_t dpll = DPLL_VCO_ENABLE | DPLL_VGA_MODE_DIS;

	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS))
		dpll |= DPLLB_MODE_LVDS;
	else
		dpll |= DPLLB_MODE_DAC_SERIAL;
	if (d->is_i945g || d->is_i945gm || d->is_g33 || d->is_pnv)
		dpll |= (uint32_t)(cfg->pixel_multiplier - 1) << SDVO_MULTIPLIER_SHIFT_HIRES;
	if (lg_cfg_has(cfg, LG_OUTPUT_SDVO) || lg_cfg_has(cfg, LG_OUTPUT_HDMI))
		dpll |= DPLL_SDVO_HIGH_SPEED;
	if (cfg->has_dp_encoder)
		dpll |= DPLL_SDVO_HIGH_SPEED;
	/* P1 as a bitmask, the same for both FP registers */
	if (d->is_g4x) {
		dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA01_P1_POST_DIV_SHIFT;
		dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA1_P1_POST_DIV_SHIFT;
	} else if (d->is_pnv) {
		dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA01_P1_POST_DIV_SHIFT_PINEVIEW;
	} else {
		dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA01_P1_POST_DIV_SHIFT;
	}
	dpll |= p2_bits(clock->p2);
	if (d->ver >= 4)
		dpll |= 6u << PLL_LOAD_PULSE_PHASE_SHIFT;
	if (cfg->sdvo_tv_clock)
		dpll |= PLL_REF_INPUT_TVCLKINBC;
	else if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) && d->lvds_use_ssc)
		dpll |= PLLB_REF_INPUT_SPREADSPECTRUMIN;
	else
		dpll |= PLL_REF_INPUT_DREFCLK;
	return dpll;
}

static uint32_t i8xx_dpll(struct lg_display *d, const struct lg_config *cfg,
			  const struct lg_dpll *clock)
{
	uint32_t dpll = DPLL_VCO_ENABLE | DPLL_VGA_MODE_DIS;

	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS)) {
		dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA01_P1_POST_DIV_SHIFT;
	} else {
		if (clock->p1 == 2)
			dpll |= PLL_P1_DIVIDE_BY_TWO;
		else
			dpll |= (uint32_t)(clock->p1 - 2) << DPLL_FPA01_P1_POST_DIV_SHIFT;
		if (clock->p2 == 4)
			dpll |= PLL_P2_DIVIDE_BY_4;
	}
	/* The i830's muxed DVO pins only work with the VCO and the 2x clock
	 * on in both PLLs, so they stay on there and for every DVO. */
	if (d->is_i830 || lg_cfg_has(cfg, LG_OUTPUT_DVO))
		dpll |= DPLL_DVO_2X_MODE;
	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) && d->lvds_use_ssc)
		dpll |= PLLB_REF_INPUT_SPREADSPECTRUMIN;
	else
		dpll |= PLL_REF_INPUT_DREFCLK;
	return dpll;
}

/* The CB tune factor of the PCH PLL's feedback. */
static int ilk_fb_cb_factor(struct lg_display *d, const struct lg_config *cfg)
{
	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) &&
	    ((d->lvds_use_ssc && d->lvds_ssc_khz == 100000) ||
	     (d->pch == LG_PCH_IBX && d->lvds_dual)))
		return 25;
	if (cfg->sdvo_tv_clock)
		return 20;
	return 21;
}

static uint32_t ilk_dpll_compute_fp(const struct lg_dpll *clock, int factor)
{
	uint32_t fp = i9xx_dpll_compute_fp(clock);

	if (clock->m < factor * clock->n)
		fp |= FP_CB_TUNE;
	return fp;
}

static uint32_t ilk_dpll(struct lg_display *d, const struct lg_config *cfg,
			 const struct lg_dpll *clock)
{
	uint32_t dpll = DPLL_VCO_ENABLE;

	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS))
		dpll |= DPLLB_MODE_LVDS;
	else
		dpll |= DPLLB_MODE_DAC_SERIAL;
	dpll |= (uint32_t)(cfg->pixel_multiplier - 1) << PLL_REF_SDVO_HDMI_MULTIPLIER_SHIFT;
	if (lg_cfg_has(cfg, LG_OUTPUT_SDVO) || lg_cfg_has(cfg, LG_OUTPUT_HDMI))
		dpll |= DPLL_SDVO_HIGH_SPEED;
	if (cfg->has_dp_encoder)
		dpll |= DPLL_SDVO_HIGH_SPEED;
	/* With three pipes and two PLLs, an analog output keeps the high
	 * speed clock on too so that it can share a PLL with HDMI. */
	if (d->num_pipes == 3 && lg_cfg_has(cfg, LG_OUTPUT_ANALOG))
		dpll |= DPLL_SDVO_HIGH_SPEED;
	dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA01_P1_POST_DIV_SHIFT;
	dpll |= (1u << (clock->p1 - 1)) << DPLL_FPA1_P1_POST_DIV_SHIFT;
	dpll |= p2_bits(clock->p2);
	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) && d->lvds_use_ssc)
		dpll |= PLLB_REF_INPUT_SPREADSPECTRUMIN;
	else
		dpll |= PLL_REF_INPUT_DREFCLK;
	return dpll;
}

static uint32_t vlv_dpll(const struct lg_config *cfg)
{
	uint32_t dpll = DPLL_INTEGRATED_REF_CLK_VLV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS;

	if (cfg->pipe != 0)
		dpll |= DPLL_INTEGRATED_CRI_CLK_VLV;
	/* a DSI pipe runs from the DSI PLL; only its reference clock is needed */
	if (!lg_cfg_has(cfg, LG_OUTPUT_DSI))
		dpll |= DPLL_VCO_ENABLE | DPLL_EXT_BUFFER_ENABLE_VLV;
	return dpll;
}

static uint32_t chv_dpll(const struct lg_config *cfg)
{
	uint32_t dpll = DPLL_SSC_REF_CLK_CHV | DPLL_REF_CLK_ENABLE_VLV | DPLL_VGA_MODE_DIS;

	if (cfg->pipe != 0)
		dpll |= DPLL_INTEGRATED_CRI_CLK_VLV;
	if (!lg_cfg_has(cfg, LG_OUTPUT_DSI))
		dpll |= DPLL_VCO_ENABLE;
	return dpll;
}

void lg_dpll_compute_regs(struct lg_display *d, struct lg_config *cfg)
{
	const struct lg_dpll *clock = &cfg->dpll;

	cfg->fp0 = cfg->fp1 = cfg->dpll_hw = cfg->dpll_md = 0;
	if (d->is_chv) {
		cfg->dpll_hw = chv_dpll(cfg);
		cfg->dpll_md = i965_dpll_md(cfg);
	} else if (d->is_vlv) {
		cfg->dpll_hw = vlv_dpll(cfg);
		cfg->dpll_md = i965_dpll_md(cfg);
	} else if (d->pch != LG_PCH_NONE) {
		int factor = ilk_fb_cb_factor(d, cfg);
		cfg->fp0 = ilk_dpll_compute_fp(clock, factor);
		cfg->fp1 = cfg->fp0;
		cfg->dpll_hw = ilk_dpll(d, cfg, clock);
	} else if (d->ver == 2) {
		cfg->fp0 = i9xx_dpll_compute_fp(clock);
		cfg->fp1 = cfg->fp0;
		cfg->dpll_hw = i8xx_dpll(d, cfg, clock);
	} else {
		cfg->fp0 = d->is_pnv ? pnv_dpll_compute_fp(clock) : i9xx_dpll_compute_fp(clock);
		cfg->fp1 = cfg->fp0;
		cfg->dpll_hw = i9xx_dpll(d, cfg, clock);
		if (d->ver >= 4)
			cfg->dpll_md = i965_dpll_md(cfg);
	}
}

/* ---- the search per platform ------------------------------------------------------- */

static int lvds_refclk(struct lg_display *d, const struct lg_config *cfg, int refclk)
{
	if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) && d->lvds_use_ssc && d->lvds_ssc_khz)
		return (int)d->lvds_ssc_khz;
	return refclk;
}

int lg_dpll_compute(struct lg_display *d, struct lg_config *cfg)
{
	const struct lg_limit *limit;
	int refclk;
	int target = (int)cfg->port_clock;
	int ok = 1;

	if (cfg->pixel_multiplier < 1)
		cfg->pixel_multiplier = 1;
	if (d->is_vlv || d->is_chv) {
		refclk = 100000;
		if (lg_cfg_has(cfg, LG_OUTPUT_DSI)) {
			/* the DSI PLL clocks the pipe */
			mm_memset(&cfg->dpll, 0, sizeof(cfg->dpll));
			lg_dpll_compute_regs(d, cfg);
			return 0;
		}
		if (!cfg->clock_set) {
			if (d->is_chv)
				ok = chv_find_best_dpll(d, &limits_chv, target, refclk, &cfg->dpll);
			else
				ok = vlv_find_best_dpll(d, &limits_vlv, target, refclk, &cfg->dpll);
		}
		if (d->is_chv)
			lg_chv_calc_dpll_params(refclk, &cfg->dpll);
		else
			lg_vlv_calc_dpll_params(refclk, &cfg->dpll);
	} else if (d->pch != LG_PCH_NONE) {
		/* the CPU eDP port runs from its own PLL in the port */
		if (!cfg->has_pch_encoder)
			return 0;
		refclk = lvds_refclk(d, cfg, 120000);
		if (lg_cfg_has(cfg, LG_OUTPUT_LVDS)) {
			if (d->lvds_dual)
				limit = refclk == 100000 ? &limits_ilk_dual_lvds_100m :
							   &limits_ilk_dual_lvds;
			else
				limit = refclk == 100000 ? &limits_ilk_single_lvds_100m :
							   &limits_ilk_single_lvds;
		} else {
			limit = &limits_ilk_dac;
		}
		if (!cfg->clock_set)
			ok = g4x_find_best_dpll(d, limit, cfg, target, refclk, &cfg->dpll);
		lg_i9xx_calc_dpll_params(refclk, &cfg->dpll);
	} else if (d->is_g4x) {
		refclk = lvds_refclk(d, cfg, 96000);
		if (lg_cfg_has(cfg, LG_OUTPUT_LVDS))
			limit = d->lvds_dual ? &limits_g4x_dual_lvds : &limits_g4x_single_lvds;
		else if (lg_cfg_has(cfg, LG_OUTPUT_HDMI) || lg_cfg_has(cfg, LG_OUTPUT_ANALOG))
			limit = &limits_g4x_hdmi;
		else if (lg_cfg_has(cfg, LG_OUTPUT_SDVO))
			limit = &limits_g4x_sdvo;
		else
			limit = &limits_i9xx_sdvo;
		if (!cfg->clock_set)
			ok = g4x_find_best_dpll(d, limit, cfg, target, refclk, &cfg->dpll);
		lg_i9xx_calc_dpll_params(refclk, &cfg->dpll);
	} else if (d->is_pnv) {
		refclk = lvds_refclk(d, cfg, 96000);
		limit = lg_cfg_has(cfg, LG_OUTPUT_LVDS) ? &limits_pnv_lvds : &limits_pnv_sdvo;
		if (!cfg->clock_set)
			ok = pnv_find_best_dpll(d, limit, cfg, target, refclk, &cfg->dpll);
		lg_pnv_calc_dpll_params(refclk, &cfg->dpll);
	} else if (d->ver != 2) {
		refclk = lvds_refclk(d, cfg, 96000);
		limit = lg_cfg_has(cfg, LG_OUTPUT_LVDS) ? &limits_i9xx_lvds : &limits_i9xx_sdvo;
		if (!cfg->clock_set)
			ok = i9xx_find_best_dpll(d, limit, cfg, target, refclk, &cfg->dpll);
		lg_i9xx_calc_dpll_params(refclk, &cfg->dpll);
	} else {
		refclk = lvds_refclk(d, cfg, 48000);
		if (lg_cfg_has(cfg, LG_OUTPUT_LVDS))
			limit = &limits_i8xx_lvds;
		else if (lg_cfg_has(cfg, LG_OUTPUT_DVO))
			limit = &limits_i8xx_dvo;
		else
			limit = &limits_i8xx_dac;
		if (!cfg->clock_set)
			ok = i9xx_find_best_dpll(d, limit, cfg, target, refclk, &cfg->dpll);
		lg_i9xx_calc_dpll_params(refclk, &cfg->dpll);
	}
	if (!ok) {
		i915_dbg("[drm] i915: pipe %c: no DPLL dividers for %d kHz\n", 'A' + cfg->pipe,
			 target);
		return -EINVAL;
	}
	lg_dpll_compute_regs(d, cfg);
	cfg->port_clock = (uint32_t)cfg->dpll.dot;
	/* What the pipe actually runs at, as the dividers make it (TV-out
	 * keeps its own notion of the clock, DP the mode's). */
	if (!cfg->has_dp_encoder && !lg_cfg_has(cfg, LG_OUTPUT_TVOUT) &&
	    cfg->pixel_multiplier > 0 && cfg->dpll.dot > 0)
		cfg->t.clock = (uint32_t)cfg->dpll.dot / (uint32_t)cfg->pixel_multiplier;
	return 0;
}

/* ---- GMCH PLL enable / disable ---------------------------------------------------- */

void lg_i9xx_enable_pll(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	lg_wr(d, FP0(pipe), cfg->fp0);
	lg_wr(d, FP1(pipe), cfg->fp1);
	/* VGA mode has to be on while P1/P2 change, or the PLL keeps
	 * running on the old dividers although the register changed. */
	lg_wr(d, DPLL(d, pipe), cfg->dpll_hw & ~DPLL_VGA_MODE_DIS);
	lg_wr(d, DPLL(d, pipe), cfg->dpll_hw);
	lg_posting_read(d, DPLL(d, pipe));
	lg_udelay(150);
	if (d->ver >= 4) {
		lg_wr(d, DPLL_MD(d, pipe), cfg->dpll_md);
	} else {
		/* The pixel multiplier only takes once the PLL runs stably:
		 * write it again. */
		lg_wr(d, DPLL(d, pipe), cfg->dpll_hw);
	}
	/* three times, as the hardware wants it warmed up */
	for (int i = 0; i < 3; i++) {
		lg_wr(d, DPLL(d, pipe), cfg->dpll_hw);
		lg_posting_read(d, DPLL(d, pipe));
		lg_udelay(150);
	}
}

void lg_i9xx_disable_pll(struct lg_display *d, int pipe)
{
	/* the i830 keeps both pipes and their PLLs running */
	if (d->is_i830)
		return;
	lg_wr(d, DPLL(d, pipe), DPLL_VGA_MODE_DIS);
	lg_posting_read(d, DPLL(d, pipe));
}

/* The i830 needs both pipes running all the time; an unused one shows
 * 640x480 at 60 Hz from its own PLL (about 25.175 MHz). */
void lg_i830_enable_pipe(struct lg_display *d, int pipe)
{
	struct lg_dpll clock = { .n = 2, .m1 = 18, .m2 = 7, .p1 = 13, .p2 = 4 };
	uint32_t dpll, fp;

	if (lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE)
		return;
	lg_i9xx_calc_dpll_params(48000, &clock);
	fp = i9xx_dpll_compute_fp(&clock);
	dpll = DPLL_DVO_2X_MODE | DPLL_VGA_MODE_DIS |
	       ((uint32_t)(clock.p1 - 2) << DPLL_FPA01_P1_POST_DIV_SHIFT) |
	       PLL_P2_DIVIDE_BY_4 | PLL_REF_INPUT_DREFCLK | DPLL_VCO_ENABLE;
	i915_dbg("[drm] i915: pipe %c kept running at 640x480 (dot %d kHz)\n", 'A' + pipe,
		 clock.dot);
	lg_wr(d, TRANS_HTOTAL(d, pipe), ((800u - 1) << 16) | (640 - 1));
	lg_wr(d, TRANS_HBLANK(d, pipe), ((800u - 1) << 16) | (640 - 1));
	lg_wr(d, TRANS_HSYNC(d, pipe), ((752u - 1) << 16) | (656 - 1));
	lg_wr(d, TRANS_VTOTAL(d, pipe), ((525u - 1) << 16) | (480 - 1));
	lg_wr(d, TRANS_VBLANK(d, pipe), ((525u - 1) << 16) | (480 - 1));
	lg_wr(d, TRANS_VSYNC(d, pipe), ((492u - 1) << 16) | (490 - 1));
	lg_wr(d, PIPESRC(d, pipe), ((640u - 1) << 16) | (480 - 1));
	lg_wr(d, FP0(pipe), fp);
	lg_wr(d, FP1(pipe), fp);
	lg_wr(d, DPLL(d, pipe), dpll & ~DPLL_VGA_MODE_DIS);
	lg_wr(d, DPLL(d, pipe), dpll);
	lg_posting_read(d, DPLL(d, pipe));
	lg_udelay(150);
	lg_wr(d, DPLL(d, pipe), dpll);
	for (int i = 0; i < 3; i++) {
		lg_wr(d, DPLL(d, pipe), dpll);
		lg_posting_read(d, DPLL(d, pipe));
		lg_udelay(150);
	}
	lg_wr(d, PIPECONF(d, pipe), PIPECONF_ENABLE);
	lg_posting_read(d, PIPECONF(d, pipe));
	for (int i = 0; i < 100 && !lg_pipe_scanline_moving(d, pipe); i++)
		lg_mdelay(1);
}

void lg_i830_disable_pipe(struct lg_display *d, int pipe)
{
	lg_wr(d, PIPECONF(d, pipe), 0);
	lg_posting_read(d, PIPECONF(d, pipe));
	for (int i = 0; i < 100 && lg_pipe_scanline_moving(d, pipe); i++)
		lg_mdelay(1);
	lg_wr(d, DPLL(d, pipe), DPLL_VGA_MODE_DIS);
	lg_posting_read(d, DPLL(d, pipe));
}

/* ---- the PCH PLLs ---------------------------------------------------------------- */

int lg_pch_dpll_reserve(struct lg_display *d, struct lg_config *cfg,
			struct lg_config *const cfgs[LG_MAX_PIPES])
{
	int unused = -1;

	cfg->pch_dpll = -1;
	if (!cfg->has_pch_encoder)
		return 0;
	if (d->pch == LG_PCH_IBX) {
		/* Ibex Peak: PLL A feeds transcoder A, B feeds B */
		cfg->pch_dpll = cfg->pipe;
		return 0;
	}
	for (int id = 0; id < 2; id++) {
		int users = 0, same = 0;
		for (int p = 0; p < LG_MAX_PIPES; p++) {
			const struct lg_config *o = cfgs[p];
			if (!o || p == cfg->pipe || !o->has_pch_encoder || o->pch_dpll != id)
				continue;
			users++;
			if (o->dpll_hw == cfg->dpll_hw && o->fp0 == cfg->fp0 && o->fp1 == cfg->fp1)
				same++;
		}
		if (users && same == users) {
			cfg->pch_dpll = id;
			i915_dbg("[drm] i915: pipe %c shares PCH PLL %c\n", 'A' + cfg->pipe,
				 'A' + id);
			return 0;
		}
		if (!users && unused < 0)
			unused = id;
	}
	if (unused < 0) {
		i915_dbg("[drm] i915: pipe %c: no PCH PLL free\n", 'A' + cfg->pipe);
		return -EINVAL;
	}
	cfg->pch_dpll = unused;
	return 0;
}

void lg_pch_dpll_enable(struct lg_display *d, const struct lg_config *cfg)
{
	int id = cfg->pch_dpll;
	int pipe = cfg->pipe;

	if (id < 0 || id > 1)
		return;
	/* Cougar Point: the transcoder's clock select comes before the PLL
	 * takes the pixel multiplier. */
	if (d->pch == LG_PCH_CPT) {
		uint32_t sel = lg_rd(d, PCH_DPLL_SEL);
		sel |= TRANS_DPLL_ENABLE(pipe);
		if (id == 1)
			sel |= TRANS_DPLLB_SEL(pipe);
		else
			sel &= ~TRANS_DPLLB_SEL(pipe);
		lg_wr(d, PCH_DPLL_SEL, sel);
	}
	d->pch_dpll[id].pipe_mask |= 1u << pipe;
	if (d->pch_dpll[id].active_mask) {
		/* already running for another transcoder with this clock */
		d->pch_dpll[id].active_mask |= 1u << pipe;
		return;
	}
	d->pch_dpll[id].active_mask |= 1u << pipe;
	d->pch_dpll[id].dpll = cfg->dpll_hw;
	d->pch_dpll[id].fp0 = cfg->fp0;
	d->pch_dpll[id].fp1 = cfg->fp1;
	lg_wr(d, PCH_FP0(id), cfg->fp0);
	lg_wr(d, PCH_FP1(id), cfg->fp1);
	lg_wr(d, PCH_DPLL(id), cfg->dpll_hw);
	lg_posting_read(d, PCH_DPLL(id));
	lg_udelay(150);
	/* the multiplier takes only once the clocks are stable */
	lg_wr(d, PCH_DPLL(id), cfg->dpll_hw);
	lg_posting_read(d, PCH_DPLL(id));
	lg_udelay(200);
}

void lg_pch_dpll_disable(struct lg_display *d, const struct lg_config *cfg)
{
	int id = cfg->pch_dpll;
	int pipe = cfg->pipe;

	if (d->pch == LG_PCH_CPT)
		lg_rmw(d, PCH_DPLL_SEL, TRANS_DPLL_ENABLE(pipe) | TRANS_DPLLB_SEL(pipe), 0);
	if (id < 0 || id > 1)
		return;
	d->pch_dpll[id].pipe_mask &= ~(1u << pipe);
	if (!(d->pch_dpll[id].active_mask & (1u << pipe)))
		return;
	d->pch_dpll[id].active_mask &= ~(1u << pipe);
	if (d->pch_dpll[id].active_mask)
		return;
	lg_wr(d, PCH_DPLL(id), 0);
	lg_posting_read(d, PCH_DPLL(id));
	lg_udelay(200);
}

/* The PCH's reference clocks.  A panel wants the spread spectrum source
 * (and, on CPU eDP, the CPU output of it); without a panel the SSC source
 * may go off unless a running PLL still uses it.  Sources are switched
 * one at a time with a settle delay each. */
void lg_pch_refclk_init(struct lg_display *d)
{
	int has_lvds = 0, has_cpu_edp = 0, has_panel = 0, has_ck505, can_ssc;
	int using_ssc_source = 0;
	uint32_t val, final;
	int use_ssc = d->lvds_use_ssc;

	if (d->pch == LG_PCH_NONE)
		return;
	for (int i = 0; i < d->nout; i++) {
		struct lg_output *o = &d->out[i];
		if (o->type == LG_OUTPUT_LVDS) {
			has_panel = 1;
			has_lvds = 1;
		} else if (o->type == LG_OUTPUT_EDP) {
			has_panel = 1;
			if (o->port == LG_PORT_A)
				has_cpu_edp = 1;
		}
	}
	if (d->pch == LG_PCH_IBX) {
		has_ck505 = d->vbt.display_clock_mode;
		can_ssc = has_ck505;
	} else {
		has_ck505 = 0;
		can_ssc = 1;
	}
	for (int id = 0; id < 2; id++) {
		uint32_t t = lg_rd(d, PCH_DPLL(id));
		if (!(t & DPLL_VCO_ENABLE))
			continue;
		if ((t & PLL_REF_INPUT_MASK) == PLLB_REF_INPUT_SPREADSPECTRUMIN) {
			using_ssc_source = 1;
			break;
		}
	}
	i915_dbg("[drm] i915: PCH refclk: panel %d lvds %d ck505 %d ssc in use %d\n", has_panel,
		 has_lvds, has_ck505, using_ssc_source);

	val = lg_rd(d, PCH_DREF_CONTROL);
	final = val;
	final &= ~DREF_NONSPREAD_SOURCE_MASK;
	final |= has_ck505 ? DREF_NONSPREAD_CK505_ENABLE : DREF_NONSPREAD_SOURCE_ENABLE;
	final &= ~DREF_SSC_SOURCE_MASK;
	final &= ~DREF_CPU_SOURCE_OUTPUT_MASK;
	final &= ~DREF_SSC1_ENABLE;
	if (has_panel) {
		final |= DREF_SSC_SOURCE_ENABLE;
		if (use_ssc && can_ssc)
			final |= DREF_SSC1_ENABLE;
		if (has_cpu_edp)
			final |= (use_ssc && can_ssc) ? DREF_CPU_SOURCE_OUTPUT_DOWNSPREAD :
							DREF_CPU_SOURCE_OUTPUT_NONSPREAD;
		else
			final |= DREF_CPU_SOURCE_OUTPUT_DISABLE;
	} else if (using_ssc_source) {
		final |= DREF_SSC_SOURCE_ENABLE;
		final |= DREF_SSC1_ENABLE;
	}
	if (final == val)
		return;

	/* the non-spread source always on */
	val &= ~DREF_NONSPREAD_SOURCE_MASK;
	val |= has_ck505 ? DREF_NONSPREAD_CK505_ENABLE : DREF_NONSPREAD_SOURCE_ENABLE;
	if (has_panel) {
		val &= ~DREF_SSC_SOURCE_MASK;
		val |= DREF_SSC_SOURCE_ENABLE;
		/* SSC on before the CPU output uses it */
		if (use_ssc && can_ssc)
			val |= DREF_SSC1_ENABLE;
		else
			val &= ~DREF_SSC1_ENABLE;
		lg_wr(d, PCH_DREF_CONTROL, val);
		lg_posting_read(d, PCH_DREF_CONTROL);
		lg_udelay(200);
		val &= ~DREF_CPU_SOURCE_OUTPUT_MASK;
		if (has_cpu_edp)
			val |= (use_ssc && can_ssc) ? DREF_CPU_SOURCE_OUTPUT_DOWNSPREAD :
						      DREF_CPU_SOURCE_OUTPUT_NONSPREAD;
		else
			val |= DREF_CPU_SOURCE_OUTPUT_DISABLE;
		lg_wr(d, PCH_DREF_CONTROL, val);
		lg_posting_read(d, PCH_DREF_CONTROL);
		lg_udelay(200);
	} else {
		val &= ~DREF_CPU_SOURCE_OUTPUT_MASK;
		val |= DREF_CPU_SOURCE_OUTPUT_DISABLE;
		lg_wr(d, PCH_DREF_CONTROL, val);
		lg_posting_read(d, PCH_DREF_CONTROL);
		lg_udelay(200);
		if (!using_ssc_source) {
			val &= ~DREF_SSC_SOURCE_MASK;
			val |= DREF_SSC_SOURCE_DISABLE;
			val &= ~DREF_SSC1_ENABLE;
			lg_wr(d, PCH_DREF_CONTROL, val);
			lg_posting_read(d, PCH_DREF_CONTROL);
			lg_udelay(200);
		}
	}
	if (val != final)
		i915_dbg("[drm] i915: PCH refclk ended at %08x, wanted %08x\n", val, final);
}

/* ---- reading back what a PLL runs at ------------------------------------------------- */

static int lowest_bit(uint32_t v)
{
	for (int i = 0; i < 32; i++)
		if (v & (1u << i))
			return i + 1;
	return 0;
}

/* The GMCH/PCH divider registers back into a clock. */
static uint32_t i9xx_clock_from_regs(struct lg_display *d, int pipe, uint32_t dpll,
				     uint32_t fp0, uint32_t fp1, int pch)
{
	struct lg_dpll clock;
	uint32_t fp = (dpll & DISPLAY_RATE_SELECT_FPA1) ? fp1 : fp0;
	int refclk;

	if ((dpll & PLL_REF_INPUT_MASK) == PLLB_REF_INPUT_SPREADSPECTRUMIN && d->lvds_ssc_khz)
		refclk = (int)d->lvds_ssc_khz;
	else if (pch)
		refclk = 120000;
	else if (d->ver != 2)
		refclk = 96000;
	else
		refclk = 48000;

	mm_memset(&clock, 0, sizeof(clock));
	clock.m1 = (int)((fp & FP_M1_DIV_MASK) >> FP_M1_DIV_SHIFT);
	if (d->is_pnv) {
		clock.n = lowest_bit((fp & FP_N_PINEVIEW_DIV_MASK) >> FP_N_DIV_SHIFT) - 1;
		clock.m2 = (int)((fp & FP_M2_PINEVIEW_DIV_MASK) >> FP_M2_DIV_SHIFT);
	} else {
		clock.n = (int)((fp & FP_N_DIV_MASK) >> FP_N_DIV_SHIFT);
		clock.m2 = (int)((fp & FP_M2_DIV_MASK) >> FP_M2_DIV_SHIFT);
	}
	if (d->ver != 2) {
		if (d->is_pnv)
			clock.p1 = lowest_bit((dpll & DPLL_FPA01_P1_POST_DIV_MASK_PINEVIEW) >>
					      DPLL_FPA01_P1_POST_DIV_SHIFT_PINEVIEW);
		else
			clock.p1 = lowest_bit((dpll & DPLL_FPA01_P1_POST_DIV_MASK) >>
					      DPLL_FPA01_P1_POST_DIV_SHIFT);
		switch (dpll & DPLL_MODE_MASK) {
		case DPLLB_MODE_DAC_SERIAL:
			clock.p2 = (dpll & DPLL_DAC_SERIAL_P2_CLOCK_DIV_5) ? 5 : 10;
			break;
		case DPLLB_MODE_LVDS:
			clock.p2 = (dpll & DPLLB_LVDS_P2_CLOCK_DIV_7) ? 7 : 14;
			break;
		default:
			return 0;
		}
		if (d->is_pnv)
			return (uint32_t)lg_pnv_calc_dpll_params(refclk, &clock);
		return (uint32_t)lg_i9xx_calc_dpll_params(refclk, &clock);
	}
	/* gen2: the LVDS of the i85x encodes P1 as a bit, the rest as a value */
	{
		uint32_t lvds = lg_rd(d, LVDS);
		if (d->is_i85x && (lvds & LVDS_PORT_EN) &&
		    (int)((lvds & LVDS_PIPE_SEL_MASK) >> 30) == pipe) {
			clock.p1 = lowest_bit((dpll & DPLL_FPA01_P1_POST_DIV_MASK_I830_LVDS) >>
					      DPLL_FPA01_P1_POST_DIV_SHIFT);
			clock.p2 = (lvds & LVDS_CLKB_POWER_UP) ? 7 : 14;
		} else {
			if (dpll & PLL_P1_DIVIDE_BY_TWO)
				clock.p1 = 2;
			else
				clock.p1 = (int)((dpll & DPLL_FPA01_P1_POST_DIV_MASK_I830) >>
						 DPLL_FPA01_P1_POST_DIV_SHIFT) + 2;
			clock.p2 = (dpll & PLL_P2_DIVIDE_BY_4) ? 4 : 2;
		}
	}
	return (uint32_t)lg_i9xx_calc_dpll_params(refclk, &clock);
}

/* DPIO divider fields (Valleyview: PLL DW3; Cherryview: CMN DW13 and PLL
 * DW0..DW3 of the channel). */
#define LGP_VLV_PLL_DW3(ch) (0x8000u + (uint32_t)(ch) * 0x20u + 3u * 4u)
#define LGP_CHV_PLL_DW(ch, dw) (0x8000u + (uint32_t)(ch) * 0x180u + (uint32_t)(dw) * 4u)
#define LGP_CHV_CMN_DW13(ch) ((ch) ? 0x8080u : 0x8134u)

uint32_t lg_dpll_get_port_clock(struct lg_display *d, int pipe)
{
	if (d->is_vlv || d->is_chv) {
		uint32_t dpll = lg_rd(d, DPLL(d, pipe));
		struct lg_dpll clock;
		int phy = lg_vlv_pipe_to_phy(d, pipe);
		int ch = lg_vlv_pipe_to_channel(pipe);

		if (!(dpll & DPLL_VCO_ENABLE))
			return 0;
		mm_memset(&clock, 0, sizeof(clock));
		if (d->is_chv) {
			uint32_t cmn13 = lg_vlv_dpio_read(d, phy, LGP_CHV_CMN_DW13(ch));
			uint32_t dw0 = lg_vlv_dpio_read(d, phy, LGP_CHV_PLL_DW(ch, 0));
			uint32_t dw1 = lg_vlv_dpio_read(d, phy, LGP_CHV_PLL_DW(ch, 1));
			uint32_t dw2 = lg_vlv_dpio_read(d, phy, LGP_CHV_PLL_DW(ch, 2));
			uint32_t dw3 = lg_vlv_dpio_read(d, phy, LGP_CHV_PLL_DW(ch, 3));
			clock.m1 = (dw1 & 0x7u) == 0 ? 2 : 0;
			clock.m2 = (int)((dw0 & 0xffu) << 22);
			if (dw3 & (1u << 16))
				clock.m2 |= (int)(dw2 & 0x3fffffu);
			clock.n = (int)((dw1 >> 8) & 0xfu);
			clock.p1 = (int)((cmn13 >> 13) & 0x7u);
			clock.p2 = (int)((cmn13 >> 8) & 0x1fu);
			return (uint32_t)lg_chv_calc_dpll_params(100000, &clock);
		}
		uint32_t dw3 = lg_vlv_dpio_read(d, phy, LGP_VLV_PLL_DW3(ch));
		clock.m1 = (int)((dw3 >> 8) & 0x7u);
		clock.m2 = (int)(dw3 & 0xffu);
		clock.n = (int)((dw3 >> 12) & 0xfu);
		clock.p1 = (int)((dw3 >> 21) & 0x7u);
		clock.p2 = (int)((dw3 >> 16) & 0x1fu);
		return (uint32_t)lg_vlv_calc_dpll_params(100000, &clock);
	}
	if (d->pch != LG_PCH_NONE) {
		int id = pipe;
		if (d->pch == LG_PCH_CPT) {
			uint32_t sel = lg_rd(d, PCH_DPLL_SEL);
			if (!(sel & TRANS_DPLL_ENABLE(pipe)))
				return 0;
			id = (sel & TRANS_DPLLB_SEL(pipe)) ? 1 : 0;
		}
		if (id > 1)
			return 0;
		uint32_t dpll = lg_rd(d, PCH_DPLL(id));
		if (!(dpll & DPLL_VCO_ENABLE))
			return 0;
		return i9xx_clock_from_regs(d, pipe, dpll, lg_rd(d, PCH_FP0(id)),
					    lg_rd(d, PCH_FP1(id)), 1);
	}
	uint32_t dpll = lg_rd(d, DPLL(d, pipe));
	if (!(dpll & DPLL_VCO_ENABLE))
		return 0;
	return i9xx_clock_from_regs(d, pipe, dpll, lg_rd(d, FP0(pipe)), lg_rd(d, FP1(pipe)), 0);
}
