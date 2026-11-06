// LikeOS -- LikeOS Image Viewer: high-quality resampling.
//
// Separable filtering in two passes.  For each output column (and row) a
// list of source taps and normalised weights is computed once; when the
// image shrinks the kernel is stretched by the scale factor so that every
// source pixel contributes (a proper low-pass instead of point sampling,
// which would alias).  Colours are filtered premultiplied by alpha so a
// transparent neighbour's (meaningless) colour never shows up as a fringe.
//
// The vertical pass consumes horizontally filtered rows from a small ring
// buffer, so the intermediate never holds more than the kernel height of
// rows -- memory stays proportional to one output row even for very large
// photos.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

#define IVP_PI 3.14159265358979323846

typedef struct {
	int first;	/* first source index */
	int count;	/* number of taps */
	float *w;	/* count weights, sum 1 */
} Taps;

typedef struct {
	Taps *t;
	float *pool;
	int max_count;
} TapTable;

static double filter_support(int q)
{
	switch (q) {
	case 0:
		return 1.0;
	case 1:
		return 2.0;
	default:
		return 3.0;
	}
}

static double sinc(double x)
{
	if (fabs(x) < 1e-8)
		return 1.0;
	x *= IVP_PI;
	return sin(x) / x;
}

static double filter_eval(int q, double x)
{
	x = fabs(x);
	switch (q) {
	case 0:		/* triangle */
		return x < 1.0 ? 1.0 - x : 0.0;
	case 1: {	/* Catmull-Rom (Keys, a = -0.5) */
		const double a = -0.5;
		if (x < 1.0)
			return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
		if (x < 2.0)
			return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
		return 0.0;
	}
	default:	/* Lanczos, three lobes */
		if (x < 3.0)
			return sinc(x) * sinc(x / 3.0);
		return 0.0;
	}
}

static void taps_free(TapTable *tt)
{
	free(tt->t);
	free(tt->pool);
	tt->t = NULL;
	tt->pool = NULL;
}

/* Taps for mapping src_n samples onto dst_n samples. */
static int taps_build(TapTable *tt, int src_n, int dst_n, int q)
{
	double scale = (double)dst_n / (double)src_n;
	/* Shrinking: stretch the kernel to the output pixel footprint. */
	double fscale = scale < 1.0 ? scale : 1.0;
	double support = filter_support(q) / fscale;
	int max_count = (int)ceil(support * 2.0) + 2;
	tt->t = calloc((size_t)dst_n, sizeof(Taps));
	tt->pool = ivp_alloc((size_t)dst_n, (size_t)max_count, sizeof(float));
	tt->max_count = max_count;
	if (tt->t == NULL || tt->pool == NULL) {
		taps_free(tt);
		return -1;
	}
	for (int i = 0; i < dst_n; i++) {
		/* Centre of output sample i in source coordinates. */
		double center = ((double)i + 0.5) / scale - 0.5;
		int first = (int)floor(center - support) + 1;
		int last = (int)ceil(center + support) - 1;
		if (last - first + 1 > max_count)
			last = first + max_count - 1;
		Taps *t = &tt->t[i];
		t->w = tt->pool + (size_t)i * (size_t)max_count;
		/* Accumulate into clamped indices: taps beyond the edge fold
		 * onto the edge sample (replicated border). */
		int lo = ivp_clampi(first, 0, src_n - 1);
		int hi = ivp_clampi(last, 0, src_n - 1);
		t->first = lo;
		t->count = hi - lo + 1;
		for (int k = 0; k < t->count; k++)
			t->w[k] = 0.0f;
		double sum = 0.0;
		for (int s = first; s <= last; s++) {
			double wgt = filter_eval(q, ((double)s - center) * fscale);
			int si = ivp_clampi(s, 0, src_n - 1);
			t->w[si - lo] += (float)wgt;
			sum += wgt;
		}
		if (fabs(sum) < 1e-12) {
			/* Degenerate: pick the nearest sample. */
			int si = ivp_clampi((int)lround(center), 0, src_n - 1);
			for (int k = 0; k < t->count; k++)
				t->w[k] = 0.0f;
			t->w[si - lo] = 1.0f;
		} else {
			for (int k = 0; k < t->count; k++)
				t->w[k] = (float)(t->w[k] / sum);
		}
	}
	return 0;
}

/* Horizontal pass of one source row into premultiplied float RGBA. */
static void hpass_row(const IvImage *src, int y, const TapTable *tx, int dw,
		      float *out, float *pm)
{
	const uint8_t *s = ivp_px(src, 0, y);
	for (int x = 0; x < src->w; x++) {
		float a = (float)s[x * 4 + 3] * (1.0f / 255.0f);
		pm[x * 4 + 0] = (float)s[x * 4 + 0] * a;
		pm[x * 4 + 1] = (float)s[x * 4 + 1] * a;
		pm[x * 4 + 2] = (float)s[x * 4 + 2] * a;
		pm[x * 4 + 3] = (float)s[x * 4 + 3];
	}
	for (int x = 0; x < dw; x++) {
		const Taps *t = &tx->t[x];
		float r = 0, g = 0, b = 0, a = 0;
		const float *p = pm + (size_t)t->first * 4;
		for (int k = 0; k < t->count; k++, p += 4) {
			float wk = t->w[k];
			r += p[0] * wk;
			g += p[1] * wk;
			b += p[2] * wk;
			a += p[3] * wk;
		}
		out[x * 4 + 0] = r;
		out[x * 4 + 1] = g;
		out[x * 4 + 2] = b;
		out[x * 4 + 3] = a;
	}
}

IvImage *iv_image_scale(const IvImage *img, int w, int h, int quality)
{
	if (img == NULL || img->px == NULL || w <= 0 || h <= 0)
		return NULL;
	if (quality < 0)
		quality = 0;
	if (quality > 2)
		quality = 2;
	if (w == img->w && h == img->h)
		return iv_image_copy(img);

	IvImage *dst = iv_image_new(w, h);
	if (dst == NULL)
		return NULL;
	TapTable tx, ty;
	memset(&tx, 0, sizeof(tx));
	memset(&ty, 0, sizeof(ty));
	if (taps_build(&tx, img->w, w, quality) < 0 ||
	    taps_build(&ty, img->h, h, quality) < 0) {
		taps_free(&tx);
		taps_free(&ty);
		iv_image_free(dst);
		return NULL;
	}

	/* Ring of horizontally filtered rows; it must hold the widest
	 * vertical tap span. */
	int ring_n = ty.max_count + 1;
	float *ring = ivp_alloc((size_t)ring_n, (size_t)w * 4, sizeof(float));
	int *ring_row = malloc(sizeof(int) * (size_t)ring_n);
	float *pm = ivp_alloc((size_t)img->w, 4, sizeof(float));
	if (ring == NULL || ring_row == NULL || pm == NULL) {
		free(ring);
		free(ring_row);
		free(pm);
		taps_free(&tx);
		taps_free(&ty);
		iv_image_free(dst);
		return NULL;
	}
	for (int i = 0; i < ring_n; i++)
		ring_row[i] = -1;

	float *acc = ivp_alloc((size_t)w, 4, sizeof(float));
	if (acc == NULL) {
		free(ring);
		free(ring_row);
		free(pm);
		taps_free(&tx);
		taps_free(&ty);
		iv_image_free(dst);
		return NULL;
	}
	for (int y = 0; y < h; y++) {
		const Taps *t = &ty.t[y];
		memset(acc, 0, sizeof(float) * (size_t)w * 4);
		for (int k = 0; k < t->count; k++) {
			int sy = t->first + k;
			int slot = sy % ring_n;
			float *row = ring + (size_t)slot * (size_t)w * 4;
			if (ring_row[slot] != sy) {
				hpass_row(img, sy, &tx, w, row, pm);
				ring_row[slot] = sy;
			}
			float wk = t->w[k];
			for (int i = 0; i < w * 4; i++)
				acc[i] += row[i] * wk;
		}
		uint8_t *d = ivp_px(dst, 0, y);
		for (int x = 0; x < w; x++) {
			float a = acc[x * 4 + 3];
			if (a < 0.5f) {
				d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = d[x * 4 + 3] = 0;
				continue;
			}
			/* Ringing of the sharper kernels can overshoot; clamp the
			 * colour to the (also clamped) alpha so the result stays
			 * a valid premultiplied value before dividing. */
			float ac = a > 255.0f ? 255.0f : a;
			float inv = 255.0f / ac;
			for (int c = 0; c < 3; c++) {
				float v = acc[x * 4 + c];
				if (v > ac)
					v = ac;
				d[x * 4 + c] = ivp_u8(v * inv);
			}
			d[x * 4 + 3] = ivp_u8(a);
		}
	}
	free(acc);
	free(ring);
	free(ring_row);
	free(pm);
	taps_free(&tx);
	taps_free(&ty);
	return dst;
}
