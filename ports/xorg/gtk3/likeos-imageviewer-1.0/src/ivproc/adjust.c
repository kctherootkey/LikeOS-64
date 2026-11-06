// LikeOS -- LikeOS Image Viewer: the Adjust sliders.
//
// One call applies every slider of an IvAdjust in a fixed order chosen so
// that each control behaves the same whatever the others are set to:
//
//   1. denoise     edge-preserving (bilateral) smoothing of the input
//   2. per channel one 256-entry table per channel folds together white
//                  balance (warmth/tint) and exposure, both as gains on
//                  linear light with a soft highlight shoulder, and the
//                  global tone curve (blacks, whites, contrast, brightness)
//                  on the gamma-encoded value
//   3. local tone  highlights/shadows from an edge-preserving luminance
//                  base (a fast guided filter computed on a ~400 px copy
//                  and applied at full resolution, so the base follows
//                  edges and halos stay small); clarity = luminance minus
//                  a large-radius blur, weighted towards the midtones
//   4. colour      saturation and vibrance on chroma around luma;
//                  vibrance favours muted colours and protects skin
//   5. finish      vignette, fade, grain, gamut fit, quantise
//   6. sharpness   unsharp mask on luminance, in bands of rows
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

/* Long edge of the low-resolution luminance copy used by the local tone
 * operators.  Their radii are defined relative to it, which is what makes
 * a 2048 px preview and the full-size save look the same. */
#define IVP_SMALL_EDGE 400.0

void iv_adjust_reset(IvAdjust *a)
{
	if (a != NULL)
		memset(a, 0, sizeof(*a));
}

int iv_adjust_is_neutral(const IvAdjust *a)
{
	if (a == NULL)
		return 1;
	const double *f = &a->brightness;
	const int n = (int)(sizeof(*a) / sizeof(double));
	for (int i = 0; i < n; i++)
		if (fabs(f[i]) >= 0.5)	/* sliders are integral steps */
			return 0;
	return 1;
}

/* Slider -100..100 to -1..1, with the dead zone iv_adjust_is_neutral uses. */
static float unit(double v)
{
	if (!isfinite(v) || fabs(v) < 0.5)
		return 0.0f;
	return (float)ivp_clampd(v / 100.0, -1.0, 1.0);
}

/* ------------------------------------------------------------------------
 * Shared helpers (also used by styles.c)
 * --------------------------------------------------------------------- */

float *ivp_luma_small(const IvImage *img, int f, int *ow, int *oh)
{
	if (f < 1)
		f = 1;
	int sw = (img->w + f - 1) / f, sh = (img->h + f - 1) / f;
	float *p = ivp_zalloc((size_t)sw, (size_t)sh, sizeof(float));
	float *cnt = ivp_zalloc((size_t)sw, 1, sizeof(float));
	if (p == NULL || cnt == NULL) {
		free(p);
		free(cnt);
		return NULL;
	}
	for (int sy = 0; sy < sh; sy++) {
		float *row = p + (size_t)sy * sw;
		memset(cnt, 0, sizeof(float) * (size_t)sw);
		for (int y = sy * f; y < (sy + 1) * f && y < img->h; y++) {
			const uint8_t *s = ivp_px(img, 0, y);
			for (int x = 0; x < img->w; x++, s += 4) {
				int c = x / f;
				row[c] += ivp_luma(s[0], s[1], s[2]) * (1.0f / 255.0f);
				cnt[c] += 1.0f;
			}
		}
		for (int c = 0; c < sw; c++)
			row[c] = cnt[c] > 0 ? row[c] / cnt[c] : 0.0f;
	}
	free(cnt);
	*ow = sw;
	*oh = sh;
	return p;
}

float ivp_plane_sample(const float *p, int w, int h, float x, float y)
{
	x = ivp_clampf(x, 0.0f, (float)(w - 1));
	y = ivp_clampf(y, 0.0f, (float)(h - 1));
	int x0 = (int)x, y0 = (int)y;
	int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
	float fx = x - (float)x0, fy = y - (float)y0;
	const float *r0 = p + (size_t)y0 * w, *r1 = p + (size_t)y1 * w;
	float a = r0[x0] + (r0[x1] - r0[x0]) * fx;
	float b = r1[x0] + (r1[x1] - r1[x0]) * fx;
	return a + (b - a) * fy;
}

/* Normalised squared distances from the centre: nx2[x] + ny2[y] is 2 in
 * the corners, so sqrt(sum / 2) runs 0 (centre) .. 1 (corner) over an
 * ellipse fitted to the frame. */
void ivp_vignette_tables(int w, int h, float **nx2, float **ny2)
{
	*nx2 = malloc(sizeof(float) * (size_t)w);
	*ny2 = malloc(sizeof(float) * (size_t)h);
	if (*nx2 == NULL || *ny2 == NULL) {
		free(*nx2);
		free(*ny2);
		*nx2 = *ny2 = NULL;
		return;
	}
	for (int x = 0; x < w; x++) {
		float n = ((float)x + 0.5f - (float)w * 0.5f) / ((float)w * 0.5f);
		(*nx2)[x] = n * n;
	}
	for (int y = 0; y < h; y++) {
		float n = ((float)y + 0.5f - (float)h * 0.5f) / ((float)h * 0.5f);
		(*ny2)[y] = n * n;
	}
}

/* 0 in the middle, a wide feathered ramp, 1 in the corners. */
float ivp_vignette_falloff(float nx2, float ny2)
{
	float r = sqrtf((nx2 + ny2) * 0.5f);
	float t = ivp_smoothstep(0.28f, 1.0f, r);
	return t * (0.6f + 0.4f * t);
}

/* Deterministic grain, -1..1.  Value noise with cells of `cell` pixels;
 * below one pixel per cell the grain is finer than the pixel grid, so it
 * is approximated by per-pixel noise scaled to the averaged strength. */
float ivp_grain_sample(int x, int y, double cell, uint32_t seed)
{
	if (cell < 1.0)
		return ivp_hash_unit(ivp_hash2(x, y, seed)) * (float)cell;
	float gx = (float)((double)x / cell), gy = (float)((double)y / cell);
	int ix = (int)floorf(gx), iy = (int)floorf(gy);
	float fx = gx - (float)ix, fy = gy - (float)iy;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	float a = ivp_hash_unit(ivp_hash2(ix, iy, seed));
	float b = ivp_hash_unit(ivp_hash2(ix + 1, iy, seed));
	float c = ivp_hash_unit(ivp_hash2(ix, iy + 1, seed));
	float d = ivp_hash_unit(ivp_hash2(ix + 1, iy + 1, seed));
	float top = a + (b - a) * fx, bot = c + (d - c) * fx;
	/* Interpolated value noise has a lower spread than its corners;
	 * 1.6 brings it back to roughly unit range. */
	return ivp_clampf((top + (bot - top) * fy) * 1.6f, -1.0f, 1.0f);
}

/* ------------------------------------------------------------------------
 * Stage 1: denoise
 * --------------------------------------------------------------------- */

/*
 * Bilateral filter: each neighbour is weighted by its distance and by how
 * different its luma is, so flat areas are averaged while edges (large
 * luma steps) are left alone.  The colour channels share the luma-based
 * weights, which smooths chroma noise along with it.
 *
 * The filter runs in place over a ring of original rows (2r + 1 rows): a
 * row is copied into the ring before it is overwritten, so every output
 * row sees only unfiltered input.
 */
static void stage_denoise(IvImage *img, float d, double ds)
{
	int r = (int)lround(2.5 * ds);
	r = ivp_clampi(r, 1, 4);
	const float sig_s = (float)r * 0.6f + 0.4f;
	const float sig_r = 3.0f + 22.0f * d;	/* in 8-bit luma steps */
	float wr[256];
	for (int i = 0; i < 256; i++)
		wr[i] = expf(-(float)(i * i) / (2.0f * sig_r * sig_r));
	const int kn = 2 * r + 1;
	float *wsp = malloc(sizeof(float) * (size_t)(kn * kn));
	const int w = img->w, h = img->h;
	const int ring_n = kn;
	uint8_t *ring = ivp_alloc((size_t)ring_n, (size_t)w, 4);
	uint8_t *lring = ivp_alloc((size_t)ring_n, (size_t)w, 1);
	if (wsp == NULL || ring == NULL || lring == NULL) {
		free(wsp);
		free(ring);
		free(lring);
		return;
	}
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++)
			wsp[(j + r) * kn + (i + r)] =
				expf(-(float)(i * i + j * j) / (2.0f * sig_s * sig_s));

	/* Load the first rows (0 .. r) into the ring. */
	int loaded = -1;
	for (int y = 0; y < h; y++) {
		int need = y + r < h ? y + r : h - 1;
		while (loaded < need) {
			loaded++;
			int slot = loaded % ring_n;
			const uint8_t *s = ivp_px(img, 0, loaded);
			memcpy(ring + (size_t)slot * w * 4, s, (size_t)w * 4);
			uint8_t *l = lring + (size_t)slot * w;
			for (int x = 0; x < w; x++, s += 4)
				l[x] = (uint8_t)((54 * s[0] + 183 * s[1] + 19 * s[2]) >> 8);
		}
		uint8_t *out = ivp_px(img, 0, y);
		int cslot = y % ring_n;
		const uint8_t *crow = lring + (size_t)cslot * w;
		for (int x = 0; x < w; x++) {
			int cl = crow[x];
			float sr = 0, sg = 0, sb = 0, sw = 0;
			for (int j = -r; j <= r; j++) {
				int yy = ivp_clampi(y + j, 0, h - 1);
				int slot = yy % ring_n;
				const uint8_t *rr = ring + (size_t)slot * w * 4;
				const uint8_t *ll = lring + (size_t)slot * w;
				const float *wrow = wsp + (j + r) * kn + r;
				for (int i = -r; i <= r; i++) {
					int xx = ivp_clampi(x + i, 0, w - 1);
					int dl = ll[xx] - cl;
					if (dl < 0)
						dl = -dl;
					float wt = wrow[i] * wr[dl];
					const uint8_t *p = rr + xx * 4;
					sr += wt * p[0];
					sg += wt * p[1];
					sb += wt * p[2];
					sw += wt;
				}
			}
			float inv = 1.0f / sw;
			out[x * 4 + 0] = ivp_u8(sr * inv);
			out[x * 4 + 1] = ivp_u8(sg * inv);
			out[x * 4 + 2] = ivp_u8(sb * inv);
		}
	}
	free(wsp);
	free(ring);
	free(lring);
}

/* ------------------------------------------------------------------------
 * Stage 2: per-channel tables
 * --------------------------------------------------------------------- */

typedef struct {
	float wb[3];		/* white balance gains (linear light) */
	float expo;		/* exposure gain */
	float blacks, whites, contrast, brightness; /* -1..1 */
} ToneParams;

/* The global tone curve on a gamma-encoded value.  Every piece keeps
 * 0 -> 0 and 1 -> 1 at neutral and is monotone over the slider range. */
static float tone_curve(const ToneParams *t, float v)
{
	if (t->blacks != 0.0f)
		v += t->blacks * 0.20f * (1.0f - ivp_smoothstep(0.0f, 0.6f, v));
	if (t->whites != 0.0f)
		v += t->whites * 0.20f * ivp_smoothstep(0.4f, 1.0f, v);
	v = ivp_clampf(v, 0.0f, 1.0f);
	if (t->contrast != 0.0f) {
		/* Power S-curve pivoting on mid-grey: slope k at the pivot,
		 * endpoints fixed; k < 1 flattens. */
		const float p = 0.5f;
		float c = t->contrast;
		float k = c > 0.0f ? 1.0f + 1.2f * c : 1.0f / (1.0f - 0.7f * c);
		if (v < p)
			v = p * powf(v / p, k);
		else
			v = 1.0f - (1.0f - p) * powf((1.0f - v) / (1.0f - p), k);
	}
	if (t->brightness != 0.0f)
		v = powf(v, exp2f(-0.75f * t->brightness));
	return v;
}

/* Soft highlight shoulder for gains above one: values up to `knee` pass
 * unchanged, [knee, gmax] is mapped onto [knee, 1] by a rational curve
 * with matching slope at the knee, so brightened highlights compress
 * instead of clipping flat. */
static float shoulder(float t, float gmax)
{
	const float knee = 0.75f;
	if (gmax <= 1.0f || t <= knee)
		return t > 1.0f ? 1.0f : t;
	float u = (t - knee) / (gmax - knee);
	if (u > 1.0f)
		u = 1.0f;
	float A = (gmax - knee) / (1.0f - knee);
	float s = A * u / (1.0f + (A - 1.0f) * u);
	return knee + (1.0f - knee) * s;
}

static void build_tables(const ToneParams *t, float lut[3][256])
{
	int linear = t->expo != 1.0f || t->wb[0] != 1.0f || t->wb[1] != 1.0f ||
		     t->wb[2] != 1.0f;
	float gmax = 0.0f;
	for (int c = 0; c < 3; c++)
		gmax = fmaxf(gmax, t->wb[c] * t->expo);
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < 256; i++) {
			float v = (float)i / 255.0f;
			if (linear) {
				float l = ivp_srgb_to_linear(v) * t->wb[c] * t->expo;
				v = ivp_linear_to_srgb(shoulder(l, gmax));
			}
			lut[c][i] = ivp_clampf(tone_curve(t, v), 0.0f, 1.0f);
		}
	}
}

/* White balance gains for warmth (amber <-> blue) and tint (magenta <->
 * green), normalised so a neutral grey keeps its luminance. */
static void wb_gains(float warmth, float tint, float g[3])
{
	g[0] = (1.0f + IVP_WB_WARM * warmth) * (1.0f + IVP_WB_TINT_RB * tint);
	g[1] = (1.0f + 0.1f * IVP_WB_WARM * warmth) * (1.0f - IVP_WB_TINT * tint);
	g[2] = (1.0f - IVP_WB_WARM * warmth) * (1.0f + IVP_WB_TINT_RB * tint);
	float y = 0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2];
	for (int c = 0; c < 3; c++)
		g[c] /= y;
}

/* ------------------------------------------------------------------------
 * Stage 3-5: the per-pixel pass
 * --------------------------------------------------------------------- */

typedef struct {
	float shadows, highlights, clarity, saturation, vibrance, vignette,
		fade, grain;
	double grain_cell;
} PixelParams;

/* Low-resolution planes for the local operators. */
typedef struct {
	int f, sw, sh;
	float *ga, *gb;		/* guided-filter coefficients (base) */
	float *blur;		/* large-radius blur (clarity) */
	int *x0, *x1;		/* per column: the two low-res columns ... */
	float *wx;		/* ... and the weight of the second */
	float *fy;		/* per row: low-res y */
} LocalPlanes;

static void local_free(LocalPlanes *lp)
{
	free(lp->ga);
	free(lp->gb);
	free(lp->blur);
	free(lp->x0);
	free(lp->x1);
	free(lp->wx);
	free(lp->fy);
	memset(lp, 0, sizeof(*lp));
}

static int local_build(const IvImage *img, const float lut[3][256],
		       int need_base, int need_blur, LocalPlanes *lp)
{
	memset(lp, 0, sizeof(*lp));
	int lng = img->w > img->h ? img->w : img->h;
	int f = (int)ceil((double)lng / IVP_SMALL_EDGE);
	if (f < 1)
		f = 1;
	int sw = (img->w + f - 1) / f, sh = (img->h + f - 1) / f;
	lp->f = f;
	lp->sw = sw;
	lp->sh = sh;
	float *L = ivp_zalloc((size_t)sw, (size_t)sh, sizeof(float));
	float *cnt = ivp_zalloc((size_t)sw, 1, sizeof(float));
	lp->x0 = malloc(sizeof(int) * (size_t)img->w);
	lp->x1 = malloc(sizeof(int) * (size_t)img->w);
	lp->wx = malloc(sizeof(float) * (size_t)img->w);
	lp->fy = malloc(sizeof(float) * (size_t)img->h);
	if (L == NULL || cnt == NULL || lp->x0 == NULL || lp->x1 == NULL ||
	    lp->wx == NULL || lp->fy == NULL) {
		free(L);
		free(cnt);
		local_free(lp);
		return -1;
	}
	/* Box-average the tone-mapped luma: the local operators see the
	 * image as stage 2 leaves it. */
	for (int sy = 0; sy < sh; sy++) {
		float *row = L + (size_t)sy * sw;
		memset(cnt, 0, sizeof(float) * (size_t)sw);
		for (int y = sy * f; y < (sy + 1) * f && y < img->h; y++) {
			const uint8_t *s = ivp_px(img, 0, y);
			for (int x = 0; x < img->w; x++, s += 4) {
				int c = x / f;
				row[c] += ivp_luma(lut[0][s[0]], lut[1][s[1]], lut[2][s[2]]);
				cnt[c] += 1.0f;
			}
		}
		for (int c = 0; c < sw; c++)
			row[c] /= cnt[c];
	}
	free(cnt);
	for (int x = 0; x < img->w; x++) {
		float fx = ivp_clampf(((float)x + 0.5f) / (float)f - 0.5f, 0.0f,
				      (float)(sw - 1));
		lp->x0[x] = (int)fx;
		lp->x1[x] = lp->x0[x] + 1 < sw ? lp->x0[x] + 1 : lp->x0[x];
		lp->wx[x] = fx - (float)lp->x0[x];
	}
	for (int y = 0; y < img->h; y++)
		lp->fy[y] = ((float)y + 0.5f) / (float)f - 0.5f;

	double rs = (double)(sw > sh ? sw : sh) / IVP_SMALL_EDGE;
	if (need_base) {
		lp->ga = ivp_alloc((size_t)sw, (size_t)sh, sizeof(float));
		lp->gb = ivp_alloc((size_t)sw, (size_t)sh, sizeof(float));
		int r = (int)lround(8.0 * rs);
		if (r < 1)
			r = 1;
		if (lp->ga == NULL || lp->gb == NULL ||
		    ivp_guided_self(L, sw, sh, r, 0.012f, lp->ga, lp->gb) < 0) {
			free(L);
			local_free(lp);
			return -1;
		}
	}
	if (need_blur) {
		lp->blur = L;
		ivp_blur_plane(lp->blur, sw, sh, 10.0 * rs);
		L = NULL;
	}
	free(L);
	return 0;
}

static void stage_pixels(IvImage *img, const float lut[3][256],
			 const PixelParams *pp, const LocalPlanes *lp)
{
	const int w = img->w, h = img->h;
	float *nx2 = NULL, *ny2 = NULL;
	if (pp->vignette != 0.0f) {
		ivp_vignette_tables(w, h, &nx2, &ny2);
		if (nx2 == NULL)
			return;
	}
	const int need_hsv = pp->vibrance != 0.0f;
	const float sat_f = 1.0f + pp->saturation;
	const float grain_amp = fabsf(pp->grain) * 0.11f;

	/* The low-resolution planes interpolated to the current row (the
	 * vertical half of the bilinear sample, done once per row). */
	float *rows = NULL;
	if (lp->ga != NULL || lp->blur != NULL) {
		rows = ivp_alloc((size_t)lp->sw, 3, sizeof(float));
		if (rows == NULL) {
			free(nx2);
			free(ny2);
			return;
		}
	}
	float *ra = rows, *rb = rows ? rows + lp->sw : NULL;
	float *rl = rows ? rows + 2 * lp->sw : NULL;

	for (int y = 0; y < h; y++) {
		uint8_t *px = ivp_px(img, 0, y);
		if (rows != NULL) {
			float fy = ivp_clampf(lp->fy[y], 0.0f, (float)(lp->sh - 1));
			int y0 = (int)fy, y1 = y0 + 1 < lp->sh ? y0 + 1 : y0;
			float ty = fy - (float)y0;
			const float *planes[3] = { lp->ga, lp->gb, lp->blur };
			float *dst[3] = { ra, rb, rl };
			for (int q = 0; q < 3; q++) {
				if (planes[q] == NULL)
					continue;
				const float *p0 = planes[q] + (size_t)y0 * lp->sw;
				const float *p1 = planes[q] + (size_t)y1 * lp->sw;
				for (int i = 0; i < lp->sw; i++)
					dst[q][i] = p0[i] + (p1[i] - p0[i]) * ty;
			}
		}
		for (int x = 0; x < w; x++, px += 4) {
			float r = lut[0][px[0]], g = lut[1][px[1]], b = lut[2][px[2]];
			float L = ivp_luma(r, g, b);

			/* -- local tone -------------------------------------- */
			if (lp->ga != NULL) {
				int c0 = lp->x0[x], c1 = lp->x1[x];
				float tx = lp->wx[x];
				float a = ra[c0] + (ra[c1] - ra[c0]) * tx;
				float bb = rb[c0] + (rb[c1] - rb[c0]) * tx;
				float base = ivp_clampf(a * L + bb, 0.0f, 1.0f);
				float gain = 1.0f;
				if (pp->shadows != 0.0f) {
					float sw = 1.0f - ivp_smoothstep(0.0f, 0.62f, base);
					sw *= sw;
					gain *= pp->shadows > 0.0f ?
						1.0f + 1.35f * pp->shadows * sw :
						1.0f + 0.55f * pp->shadows * sw;
				}
				float hw = ivp_smoothstep(0.42f, 1.0f, base);
				if (pp->highlights < 0.0f)
					gain *= 1.0f + 0.48f * pp->highlights * hw;
				r *= gain;
				g *= gain;
				b *= gain;
				if (pp->highlights > 0.0f) {
					float k = 1.0f - 0.45f * pp->highlights * hw;
					r = 1.0f - (1.0f - r) * k;
					g = 1.0f - (1.0f - g) * k;
					b = 1.0f - (1.0f - b) * k;
				}
			}
			if (lp->blur != NULL) {
				int c0 = lp->x0[x], c1 = lp->x1[x];
				float bl = rl[c0] + (rl[c1] - rl[c0]) * lp->wx[x];
				float Ln = ivp_clampf(ivp_luma(r, g, b), 0.0f, 1.0f);
				float m = 4.0f * Ln * (1.0f - Ln);
				float k = pp->clarity > 0.0f ? 1.6f : 0.9f;
				float dlt = pp->clarity * k * (L - bl) * (0.25f + 0.75f * m);
				r += dlt;
				g += dlt;
				b += dlt;
			}

			/* -- colour -------------------------------------------- */
			if (sat_f != 1.0f || need_hsv) {
				float Y = ivp_luma(r, g, b);
				float f = sat_f;
				if (need_hsv) {
					float hue, s;
					ivp_hue_sat(ivp_clampf(r, 0, 1), ivp_clampf(g, 0, 1),
						    ivp_clampf(b, 0, 1), &hue, &s);
					if (pp->vibrance > 0.0f) {
						/* Skin sits around orange (hue ~ 10..50 deg)
						 * at moderate saturation. */
						float skin = ivp_smoothstep(2.0f, 18.0f, hue) *
							     (1.0f - ivp_smoothstep(38.0f, 56.0f, hue)) *
							     (1.0f - ivp_smoothstep(0.55f, 0.85f, s));
						float muted = (1.0f - s) * (1.0f - s);
						f *= 1.0f + 1.2f * pp->vibrance * muted *
							     (1.0f - 0.7f * skin);
					} else {
						f *= 1.0f + 0.85f * pp->vibrance * (1.0f - 0.5f * s);
					}
				}
				r = Y + (r - Y) * f;
				g = Y + (g - Y) * f;
				b = Y + (b - Y) * f;
			}

			/* -- finish -------------------------------------------- */
			if (nx2 != NULL) {
				float fall = ivp_vignette_falloff(nx2[x], ny2[y]);
				if (pp->vignette < 0.0f) {
					float k = 1.0f + 0.78f * pp->vignette * fall;
					r *= k;
					g *= k;
					b *= k;
				} else {
					float k = 1.0f - 0.75f * pp->vignette * fall;
					r = 1.0f - (1.0f - r) * k;
					g = 1.0f - (1.0f - g) * k;
					b = 1.0f - (1.0f - b) * k;
				}
			}
			if (pp->fade > 0.0f) {
				float Y = ivp_luma(r, g, b);
				float ds = 0.25f * pp->fade;
				r += (Y - r) * ds;
				g += (Y - g) * ds;
				b += (Y - b) * ds;
				float lift = 0.11f * pp->fade;
				float span = 1.0f - lift - 0.06f * pp->fade;
				r = lift + r * span;
				g = lift + g * span;
				b = lift + b * span;
			} else if (pp->fade < 0.0f) {
				float k = -0.10f * pp->fade;
				float inv = 1.0f / (1.0f - k);
				r = (r - k) * inv;
				g = (g - k) * inv;
				b = (b - k) * inv;
			}
			if (grain_amp > 0.0f) {
				float Y = ivp_clampf(ivp_luma(r, g, b), 0.0f, 1.0f);
				float n = ivp_grain_sample(x, y, pp->grain_cell, 0x51A7u) *
					  grain_amp * (0.35f + 2.6f * Y * (1.0f - Y));
				r += n;
				g += n;
				b += n;
			}
			ivp_gamut_fit(&r, &g, &b);
			px[0] = ivp_to_u8(r);
			px[1] = ivp_to_u8(g);
			px[2] = ivp_to_u8(b);
		}
	}
	free(rows);
	free(nx2);
	free(ny2);
}

/* ------------------------------------------------------------------------
 * Stage 6: sharpness
 * --------------------------------------------------------------------- */

/*
 * Unsharp mask on luminance only (sharpening colour channels separately
 * produces coloured fringes).  The luma of the whole image is taken first
 * as bytes; the blur then runs over bands of rows with a halo, so the
 * float working set stays small for big photos.
 */
static void stage_sharpen(IvImage *img, float s, double ds)
{
	const int w = img->w, h = img->h;
	double sigma = (s > 0.0f ? 1.0 : 1.4) * ds;
	if (sigma < 0.35)
		sigma = 0.35;
	const float amt = s > 0.0f ? 1.7f * s : 0.95f * s;
	int halo = (int)ceil(sigma * 3.0) + 2;
	const int band = 128;
	uint8_t *l8 = ivp_alloc((size_t)w, (size_t)h, 1);
	float *buf = ivp_alloc((size_t)w, (size_t)(band + 2 * halo), sizeof(float));
	if (l8 == NULL || buf == NULL) {
		free(l8);
		free(buf);
		return;
	}
	for (int y = 0; y < h; y++) {
		const uint8_t *p = ivp_px(img, 0, y);
		uint8_t *l = l8 + (size_t)y * w;
		for (int x = 0; x < w; x++, p += 4)
			l[x] = (uint8_t)((54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8);
	}
	for (int y0 = 0; y0 < h; y0 += band) {
		int y1 = y0 + band < h ? y0 + band : h;
		int a0 = y0 - halo > 0 ? y0 - halo : 0;
		int a1 = y1 + halo < h ? y1 + halo : h;
		int bh = a1 - a0;
		for (int y = a0; y < a1; y++) {
			const uint8_t *l = l8 + (size_t)y * w;
			float *d = buf + (size_t)(y - a0) * w;
			for (int x = 0; x < w; x++)
				d[x] = (float)l[x];
		}
		ivp_blur_plane(buf, w, bh, sigma);
		for (int y = y0; y < y1; y++) {
			const uint8_t *l = l8 + (size_t)y * w;
			const float *bl = buf + (size_t)(y - a0) * w;
			uint8_t *p = ivp_px(img, 0, y);
			for (int x = 0; x < w; x++, p += 4) {
				float d = (float)l[x] - bl[x];
				if (amt > 0.0f) {
					/* Leave grain-sized wiggles alone: below
					 * ~1.5 levels the "detail" is mostly noise. */
					float ad = fabsf(d);
					d *= ivp_smoothstep(0.8f, 3.0f, ad);
				}
				float dl = amt * d;
				p[0] = ivp_u8((float)p[0] + dl);
				p[1] = ivp_u8((float)p[1] + dl);
				p[2] = ivp_u8((float)p[2] + dl);
			}
		}
	}
	free(l8);
	free(buf);
}

/* ------------------------------------------------------------------------
 * Entry points
 * --------------------------------------------------------------------- */

void iv_adjust_apply_ex(IvImage *img, const IvAdjust *a, double detail_scale)
{
	if (img == NULL || img->px == NULL || a == NULL || iv_adjust_is_neutral(a))
		return;
	if (!(detail_scale > 0.0) || !isfinite(detail_scale))
		detail_scale = 1.0;
	if (detail_scale > 4.0)
		detail_scale = 4.0;

	float dn = unit(a->denoise);
	if (dn > 0.0f)
		stage_denoise(img, dn, detail_scale);

	ToneParams tp;
	tp.blacks = unit(a->blacks);
	tp.whites = unit(a->whites);
	tp.contrast = unit(a->contrast);
	tp.brightness = unit(a->brightness);
	tp.expo = exp2f(2.0f * unit(a->exposure));
	wb_gains(unit(a->warmth), unit(a->tint), tp.wb);

	PixelParams pp;
	pp.shadows = unit(a->shadows);
	pp.highlights = unit(a->highlights);
	pp.clarity = unit(a->clarity);
	pp.saturation = unit(a->saturation);
	pp.vibrance = unit(a->vibrance);
	pp.vignette = unit(a->vignette);
	pp.fade = unit(a->fade);
	pp.grain = unit(a->grain);
	pp.grain_cell = (pp.grain > 0.0f ? 2.2 : 1.1) * detail_scale;

	int need_tone = tp.blacks != 0.0f || tp.whites != 0.0f ||
			tp.contrast != 0.0f || tp.brightness != 0.0f ||
			unit(a->exposure) != 0.0f || unit(a->warmth) != 0.0f ||
			unit(a->tint) != 0.0f;
	int need_base = pp.shadows != 0.0f || pp.highlights != 0.0f;
	int need_blur = pp.clarity != 0.0f;
	int need_pix = need_tone || need_base || need_blur ||
		       pp.saturation != 0.0f || pp.vibrance != 0.0f ||
		       pp.vignette != 0.0f || pp.fade != 0.0f || pp.grain != 0.0f;

	if (need_pix) {
		float (*lut)[256] = malloc(sizeof(float) * 3 * 256);
		if (lut == NULL)
			return;
		build_tables(&tp, lut);
		LocalPlanes lp;
		memset(&lp, 0, sizeof(lp));
		if ((need_base || need_blur) &&
		    local_build(img, (const float (*)[256])lut, need_base,
				need_blur, &lp) < 0) {
			free(lut);
			return;
		}
		stage_pixels(img, (const float (*)[256])lut, &pp, &lp);
		local_free(&lp);
		free(lut);
	}

	float sh = unit(a->sharpness);
	if (sh != 0.0f)
		stage_sharpen(img, sh, detail_scale);
}

void iv_adjust_apply(IvImage *img, const IvAdjust *a)
{
	iv_adjust_apply_ex(img, a, 1.0);
}
