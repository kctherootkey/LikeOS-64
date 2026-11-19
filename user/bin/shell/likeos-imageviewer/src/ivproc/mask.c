// LikeOS -- LikeOS Image Viewer: mask painting and mask utilities (the
// Retouch and Background brushes paint into these masks).
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

void iv_mask_dab(uint8_t *mask, int w, int h, double x, double y, double r,
		 int value, double hardness)
{
	if (mask == NULL || w <= 0 || h <= 0 || !(r > 0.0) || !isfinite(x) ||
	    !isfinite(y))
		return;
	value = ivp_clampi(value, 0, 255);
	hardness = ivp_clampd(hardness, 0.0, 1.0);
	/* Full strength inside r_in, a smoothstep down to zero at r.  Half a
	 * pixel of falloff is kept even for a hard brush so its edge is
	 * anti-aliased rather than stair-stepped. */
	double r_in = r * hardness;
	if (r_in > r - 0.5)
		r_in = r - 0.5 > 0.0 ? r - 0.5 : 0.0;
	double span = r - r_in;
	int x0 = (int)floor(x - r), x1 = (int)ceil(x + r);
	int y0 = (int)floor(y - r), y1 = (int)ceil(y + r);
	x0 = ivp_clampi(x0, 0, w - 1);
	x1 = ivp_clampi(x1, 0, w - 1);
	y0 = ivp_clampi(y0, 0, h - 1);
	y1 = ivp_clampi(y1, 0, h - 1);
	if (x - r > w || y - r > h || x + r < 0 || y + r < 0)
		return;
	for (int py = y0; py <= y1; py++) {
		double dy = (double)py + 0.5 - y;
		uint8_t *row = mask + (size_t)py * (size_t)w;
		for (int px = x0; px <= x1; px++) {
			double dx = (double)px + 0.5 - x;
			double d = sqrt(dx * dx + dy * dy);
			if (d >= r)
				continue;
			double cov;
			if (d <= r_in) {
				cov = 1.0;
			} else {
				double t = (d - r_in) / span;
				cov = 1.0 - t * t * (3.0 - 2.0 * t);
			}
			/* Move towards the brush value by the coverage, never
			 * past it; repeated dabs along a stroke converge. */
			int m = row[px];
			double nv = m + ((double)value - m) * cov;
			int n = (int)lround(nv);
			if ((value >= m && n > m) || (value <= m && n < m))
				row[px] = (uint8_t)ivp_clampi(n, 0, 255);
		}
	}
}

void iv_mask_feather(uint8_t *mask, int w, int h, double radius)
{
	if (mask == NULL || w <= 0 || h <= 0 || !(radius > 0.0))
		return;
	/* Work in float so a long feather does not band. */
	float *p = ivp_alloc((size_t)w, (size_t)h, sizeof(float));
	if (p == NULL) {
		ivp_blur_generic(mask, IVP_U8, w, h, 1, (size_t)w, radius * 0.5,
				 radius * 0.5);
		return;
	}
	size_t n = (size_t)w * (size_t)h;
	for (size_t i = 0; i < n; i++)
		p[i] = mask[i];
	ivp_blur_plane(p, w, h, radius * 0.5);
	for (size_t i = 0; i < n; i++)
		mask[i] = ivp_u8(p[i]);
	free(p);
}

/* 1-D resample of n_src samples (stride ss) into n_dst (stride ds):
 * box coverage when shrinking, linear interpolation when enlarging. */
static void resample_1d(const float *src, int n_src, size_t ss, float *dst,
			int n_dst, size_t ds)
{
	double scale = (double)n_src / n_dst;
	for (int i = 0; i < n_dst; i++) {
		double v;
		if (scale > 1.0) {
			double f0 = i * scale, f1 = (i + 1) * scale;
			double acc = 0.0;
			for (int k = (int)f0; k < (int)ceil(f1) && k < n_src; k++) {
				double a = k > f0 ? k : f0;
				double b = k + 1 < f1 ? k + 1 : f1;
				acc += (b - a) * src[(size_t)k * ss];
			}
			v = acc / (f1 - f0);
		} else {
			double c = (i + 0.5) * scale - 0.5;
			int k = (int)floor(c);
			double t = c - k;
			int ka = ivp_clampi(k, 0, n_src - 1);
			int kb = ivp_clampi(k + 1, 0, n_src - 1);
			v = src[(size_t)ka * ss] * (1.0 - t) + src[(size_t)kb * ss] * t;
		}
		dst[(size_t)i * ds] = (float)v;
	}
}

void iv_mask_scale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw,
		   int dh)
{
	if (src == NULL || dst == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	if (sw == dw && sh == dh) {
		memcpy(dst, src, (size_t)sw * (size_t)sh);
		return;
	}
	/* Horizontal pass into a dw x sh float plane, then vertical. */
	float *row = malloc(sizeof(float) * (size_t)(sw > sh ? sw : sh));
	float *mid = ivp_alloc((size_t)dw, (size_t)sh, sizeof(float));
	float *col = malloc(sizeof(float) * (size_t)dh);
	if (row == NULL || mid == NULL || col == NULL) {
		free(row);
		free(mid);
		free(col);
		return;
	}
	for (int y = 0; y < sh; y++) {
		const uint8_t *s = src + (size_t)y * sw;
		for (int x = 0; x < sw; x++)
			row[x] = s[x];
		resample_1d(row, sw, 1, mid + (size_t)y * dw, dw, 1);
	}
	for (int x = 0; x < dw; x++) {
		resample_1d(mid + x, sh, (size_t)dw, col, dh, 1);
		for (int y = 0; y < dh; y++)
			dst[(size_t)y * dw + x] = ivp_u8(col[y]);
	}
	free(row);
	free(mid);
	free(col);
}

int iv_mask_bbox(const uint8_t *mask, int w, int h, int threshold, int *bx,
		 int *by, int *bw, int *bh)
{
	if (mask == NULL || w <= 0 || h <= 0)
		return 0;
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = mask + (size_t)y * w;
		for (int x = 0; x < w; x++) {
			if (row[x] >= threshold) {
				if (x < x0)
					x0 = x;
				if (x > x1)
					x1 = x;
				if (y < y0)
					y0 = y;
				y1 = y;
			}
		}
	}
	if (x1 < 0)
		return 0;
	if (bx)
		*bx = x0;
	if (by)
		*by = y0;
	if (bw)
		*bw = x1 - x0 + 1;
	if (bh)
		*bh = y1 - y0 + 1;
	return 1;
}
