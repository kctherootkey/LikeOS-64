// LikeOS -- LikeOS Image Viewer: auto enhance.
//
// Looks at the image's statistics and proposes Adjust slider values; the
// editor then shows them on the sliders, so the result stays editable.
//
//  - levels:   the 0.5 % / 99.5 % luma percentiles are moved towards black
//              and white through the Blacks / Whites sliders (the inverse
//              of their tone-curve terms in adjust.c)
//  - midtones: the median is brought towards mid-grey with Brightness
//  - contrast: a gentle S-curve when the luma spread is narrow
//  - shadows:  lifted when a large part of the image sits in the darks
//  - colour:   a white-balance estimate mixing grey-world (average of the
//              midtones) and white-patch (average of the brightest
//              unsaturated pixels), mapped onto Warmth / Tint, applied
//              only partly because a scene may legitimately be warm or cool
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

/* Same terms as tone_curve() in adjust.c. */
static float blacks_term(float v)
{
	return 0.20f * (1.0f - ivp_smoothstep(0.0f, 0.6f, v));
}

static float whites_term(float v)
{
	return 0.20f * ivp_smoothstep(0.4f, 1.0f, v);
}

static double percentile(const uint32_t *hist, uint64_t total, double q)
{
	uint64_t target = (uint64_t)((double)total * q);
	uint64_t acc = 0;
	for (int i = 0; i < 256; i++) {
		acc += hist[i];
		if (acc > target)
			return (double)i / 255.0;
	}
	return 1.0;
}

/* Gains of adjust.c's white balance (before normalisation). */
static void model_gains(double w, double t, double g[3])
{
	g[0] = (1.0 + IVP_WB_WARM * w) * (1.0 + IVP_WB_TINT_RB * t);
	g[1] = (1.0 + 0.1 * IVP_WB_WARM * w) * (1.0 - IVP_WB_TINT * t);
	g[2] = (1.0 - IVP_WB_WARM * w) * (1.0 + IVP_WB_TINT_RB * t);
}

void iv_auto_enhance(const IvImage *img, IvAdjust *out)
{
	if (out == NULL)
		return;
	iv_adjust_reset(out);
	if (img == NULL || img->px == NULL)
		return;

	/* Point-sample about 512 x 512 positions: plenty for statistics. */
	int lng = img->w > img->h ? img->w : img->h;
	int step = lng / 512;
	if (step < 1)
		step = 1;

	uint32_t hist[256];
	memset(hist, 0, sizeof(hist));
	uint64_t total = 0;
	double sum = 0.0, sum2 = 0.0;
	float lin[256];
	ivp_build_srgb_lut(lin);
	double gw[3] = { 0, 0, 0 };
	uint64_t gw_n = 0;
	/* White patch: track a histogram of "brightness" of unsaturated
	 * pixels, then average those above its 99th percentile. */
	uint32_t wp_hist[256];
	memset(wp_hist, 0, sizeof(wp_hist));
	uint64_t wp_total = 0;

	for (int y = step / 2; y < img->h; y += step) {
		const uint8_t *row = ivp_px(img, 0, y);
		for (int x = step / 2; x < img->w; x += step) {
			const uint8_t *p = row + x * 4;
			if (p[3] < 128)
				continue;
			int l8 = (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8;
			hist[l8]++;
			total++;
			double l = l8 / 255.0;
			sum += l;
			sum2 += l * l;
			int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
			int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
			double sat = mx > 0 ? (double)(mx - mn) / mx : 0.0;
			if (l8 > 38 && l8 < 230 && sat < 0.6) {
				gw[0] += lin[p[0]];
				gw[1] += lin[p[1]];
				gw[2] += lin[p[2]];
				gw_n++;
			}
			if (sat < 0.35 && mx < 253) {
				wp_hist[mx]++;
				wp_total++;
			}
		}
	}
	if (total < 16)
		return;

	double mean = sum / (double)total;
	double sd = sqrt(fmax(0.0, sum2 / (double)total - mean * mean));
	double p_lo = percentile(hist, total, 0.005);
	double p_hi = percentile(hist, total, 0.995);
	double med = percentile(hist, total, 0.5);
	double dark_frac = 0.0;
	for (int i = 0; i < 40; i++)
		dark_frac += hist[i];
	dark_frac /= (double)total;
	double clip_hi = (double)hist[255] / (double)total;

	/* -- levels ---------------------------------------------------------- */
	double blacks = 0.0, whites = 0.0;
	if (p_lo > 0.015) {
		/* blacks * term(p_lo) = target - p_lo */
		double t = blacks_term((float)p_lo);
		if (t > 1e-3)
			blacks = (0.008 - p_lo) / t;
		blacks = ivp_clampd(blacks, -0.6, 0.0);
	}
	if (p_hi < 0.965) {
		double t = whites_term((float)p_hi);
		if (t > 1e-3)
			whites = (0.985 - p_hi) / t;
		whites = ivp_clampd(whites, 0.0, 0.6);
	}

	/* The median after the level change, approximately. */
	double med2 = med + blacks * blacks_term((float)med) + whites * whites_term((float)med);
	med2 = ivp_clampd(med2, 0.02, 0.98);

	/* -- midtones -------------------------------------------------------- */
	double brightness = 0.0;
	const double target = 0.46;
	if (fabs(med2 - target) > 0.04) {
		/* v^gamma maps med2 to target; gamma = 2^(-0.75 b). */
		double gamma = log(target) / log(med2);
		brightness = -log2(gamma) / 0.75;
		brightness *= 0.6;	/* go part of the way */
		brightness = ivp_clampd(brightness, -0.4, 0.45);
	}

	/* -- contrast -------------------------------------------------------- */
	double contrast = ivp_clampd(0.10 + (0.20 - sd) * 1.5, 0.0, 0.30);

	/* -- shadows / highlights ------------------------------------------ */
	double shadows = 0.0, highlights = 0.0;
	if (dark_frac > 0.25)
		shadows = ivp_clampd((dark_frac - 0.25) * 1.2 + 0.15, 0.0, 0.45);
	if (clip_hi > 0.02)
		highlights = -ivp_clampd(clip_hi * 4.0, 0.1, 0.35);

	/* -- white balance --------------------------------------------------- */
	double warmth = 0.0, tint = 0.0;
	if (gw_n > 64) {
		double est[3];
		for (int c = 0; c < 3; c++)
			est[c] = gw[c] / (double)gw_n;
		if (wp_total > 64) {
			/* Mean of the brightest 1 % of the unsaturated pixels. */
			uint64_t want = wp_total / 100 + 1, got = 0;
			int thr = 255;
			while (thr > 0 && got < want)
				got += wp_hist[thr--];
			double wp[3] = { 0, 0, 0 };
			uint64_t n = 0;
			for (int y = step / 2; y < img->h; y += step) {
				const uint8_t *row = ivp_px(img, 0, y);
				for (int x = step / 2; x < img->w; x += step) {
					const uint8_t *p = row + x * 4;
					if (p[3] < 128)
						continue;
					int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) :
							       (p[1] > p[2] ? p[1] : p[2]);
					int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) :
							       (p[1] < p[2] ? p[1] : p[2]);
					double sat = mx > 0 ? (double)(mx - mn) / mx : 0.0;
					if (sat < 0.35 && mx < 253 && mx > thr) {
						wp[0] += lin[p[0]];
						wp[1] += lin[p[1]];
						wp[2] += lin[p[2]];
						n++;
					}
				}
			}
			if (n > 0) {
				/* Blend in chromaticity: normalise both to equal
				 * green first. */
				double gwn = est[1] > 1e-6 ? est[1] : 1e-6;
				double wpn = wp[1] > 1e-9 ? wp[1] : 1e-9;
				for (int c = 0; c < 3; c++)
					est[c] = 0.6 * est[c] / gwn + 0.4 * wp[c] / wpn;
			}
		}
		/* Gains that would neutralise the estimate. */
		if (est[0] > 1e-6 && est[1] > 1e-6 && est[2] > 1e-6) {
			double want_rb = est[2] / est[0];	/* gr / gb */
			/* (1 + W w) / (1 - W w) = q */
			double q = want_rb;
			warmth = (q - 1.0) / (IVP_WB_WARM * (q + 1.0));
			warmth = ivp_clampd(warmth, -1.0, 1.0);
			/* Tint: bisection on the green balance. */
			double want_g = sqrt(est[0] * est[2]) / est[1];
			double lo = -1.0, hi = 1.0;
			for (int it = 0; it < 40; it++) {
				double mid = 0.5 * (lo + hi);
				double g[3];
				model_gains(warmth, mid, g);
				double ratio = g[1] / sqrt(g[0] * g[2]);
				/* ratio decreases as tint rises */
				if (ratio > want_g)
					lo = mid;
				else
					hi = mid;
			}
			tint = 0.5 * (lo + hi);
			/* Most of the way, but not all: a sunset is meant to
			 * be warm.  Nothing for tiny casts. */
			warmth *= 0.75;
			tint *= 0.75;
			if (fabs(warmth) < 0.03)
				warmth = 0.0;
			if (fabs(tint) < 0.03)
				tint = 0.0;
			warmth = ivp_clampd(warmth, -0.7, 0.7);
			tint = ivp_clampd(tint, -0.6, 0.6);
		}
	}

	out->blacks = round(blacks * 100.0);
	out->whites = round(whites * 100.0);
	out->brightness = round(brightness * 100.0);
	out->contrast = round(contrast * 100.0);
	out->shadows = round(shadows * 100.0);
	out->highlights = round(highlights * 100.0);
	out->warmth = round(warmth * 100.0);
	out->tint = round(tint * 100.0);
}
