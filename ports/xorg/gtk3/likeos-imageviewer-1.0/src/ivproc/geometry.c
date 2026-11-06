// LikeOS -- LikeOS Image Viewer: geometry -- quarter turns, mirroring,
// free rotation (straighten) with its crop-to-fit rectangle, and cropping.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

#define IVP_PI 3.14159265358979323846

IvImage *iv_rotate90(const IvImage *img, int quarter_turns_cw)
{
	if (img == NULL || img->px == NULL)
		return NULL;
	int q = ((quarter_turns_cw % 4) + 4) % 4;
	if (q == 0)
		return iv_image_copy(img);
	const int w = img->w, h = img->h;
	IvImage *d = (q == 2) ? iv_image_new(w, h) : iv_image_new(h, w);
	if (d == NULL)
		return NULL;
	for (int y = 0; y < h; y++) {
		const uint32_t *s = (const uint32_t *)(const void *)ivp_px(img, 0, y);
		for (int x = 0; x < w; x++) {
			int dx, dy;
			switch (q) {
			case 1:	/* 90 deg clockwise: (x, y) -> (h-1-y, x) */
				dx = h - 1 - y;
				dy = x;
				break;
			case 2:
				dx = w - 1 - x;
				dy = h - 1 - y;
				break;
			default: /* 270 deg clockwise: (x, y) -> (y, w-1-x) */
				dx = y;
				dy = w - 1 - x;
				break;
			}
			uint32_t *o = (uint32_t *)(void *)ivp_px(d, dx, dy);
			*o = s[x];
		}
	}
	return d;
}

void iv_flip(IvImage *img, int horizontal)
{
	if (img == NULL || img->px == NULL)
		return;
	if (horizontal) {
		for (int y = 0; y < img->h; y++) {
			uint32_t *row = (uint32_t *)(void *)ivp_px(img, 0, y);
			for (int a = 0, b = img->w - 1; a < b; a++, b--) {
				uint32_t t = row[a];
				row[a] = row[b];
				row[b] = t;
			}
		}
		return;
	}
	size_t n = (size_t)img->w * 4;
	uint8_t *tmp = malloc(n);
	if (tmp == NULL)
		return;
	for (int a = 0, b = img->h - 1; a < b; a++, b--) {
		memcpy(tmp, ivp_px(img, 0, a), n);
		memcpy(ivp_px(img, 0, a), ivp_px(img, 0, b), n);
		memcpy(ivp_px(img, 0, b), tmp, n);
	}
	free(tmp);
}

/*
 * The largest-area axis-aligned rectangle inside a w x h rectangle turned
 * by `degrees`.  Two regimes:
 *
 *  - When the short side is small compared with the long one (or the
 *    angle is near 45 degrees) the optimum touches the long sides only and
 *    is "half-constrained": its corners lie on the two long edges, giving
 *    x = short / 2 and the rectangle x / sin, x / cos.
 *  - Otherwise all four corners touch the rotated edges and the 2x2 linear
 *    system  W cos + H sin = w,  W sin + H cos = h  gives
 *    W = (w cos - h sin) / cos 2a,  H = (h cos - w sin) / cos 2a.
 *
 * Only |sin| and |cos| of the angle matter (the problem is symmetric).
 */
void iv_straighten_inner_rect(int w, int h, double degrees, double *out_w,
			      double *out_h)
{
	double ow = 0.0, oh = 0.0;
	if (w > 0 && h > 0) {
		double a = degrees * IVP_PI / 180.0;
		double sn = fabs(sin(a)), cs = fabs(cos(a));
		if (sn < 1e-12) {
			ow = w;
			oh = h;
		} else if (cs < 1e-12) {
			ow = h;
			oh = w;
		} else {
			int wide = w >= h;
			double lng = wide ? w : h, shrt = wide ? h : w;
			if (shrt <= 2.0 * sn * cs * lng || fabs(sn - cs) < 1e-10) {
				double x = 0.5 * shrt;
				if (wide) {
					ow = x / sn;
					oh = x / cs;
				} else {
					ow = x / cs;
					oh = x / sn;
				}
			} else {
				double cos2a = cs * cs - sn * sn;
				ow = (w * cs - h * sn) / cos2a;
				oh = (h * cs - w * sn) / cos2a;
			}
		}
	}
	if (out_w)
		*out_w = ow;
	if (out_h)
		*out_h = oh;
}

/*
 * Same, keeping the w:h aspect.  An axis-aligned s*w x s*h rectangle
 * centred in the turned one fits when its corners, turned back, are
 * inside: s (w cos + h sin) <= w and s (w sin + h cos) <= h.
 */
void iv_straighten_inner_rect_aspect(int w, int h, double degrees,
				     double *out_w, double *out_h)
{
	double ow = 0.0, oh = 0.0;
	if (w > 0 && h > 0) {
		double a = degrees * IVP_PI / 180.0;
		double sn = fabs(sin(a)), cs = fabs(cos(a));
		double s1 = w / (w * cs + h * sn);
		double s2 = h / (w * sn + h * cs);
		double s = s1 < s2 ? s1 : s2;
		ow = s * w;
		oh = s * h;
	}
	if (out_w)
		*out_w = ow;
	if (out_h)
		*out_h = oh;
}

/* Catmull-Rom weights for fractional offset t (0..1) of taps -1..2. */
static inline void cubic_weights(float t, float w[4])
{
	float t2 = t * t, t3 = t2 * t;
	w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
	w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
	w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
	w[3] = 0.5f * (t3 - t2);
}

/* Bicubic sample of the premultiplied source at (sx, sy) in pixel-centre
 * coordinates; taps outside the image are transparent. */
static void sample_bicubic(const IvImage *img, float sx, float sy, uint8_t out[4])
{
	int ix = (int)floorf(sx), iy = (int)floorf(sy);
	float wx[4], wy[4];
	cubic_weights(sx - (float)ix, wx);
	cubic_weights(sy - (float)iy, wy);
	float acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 4; j++) {
		int yy = iy - 1 + j;
		if (yy < 0 || yy >= img->h)
			continue;
		const uint8_t *row = ivp_px(img, 0, yy);
		float rowacc[4] = { 0, 0, 0, 0 };
		for (int i = 0; i < 4; i++) {
			int xx = ix - 1 + i;
			if (xx < 0 || xx >= img->w)
				continue;
			const uint8_t *p = row + xx * 4;
			float a = (float)p[3] * (1.0f / 255.0f);
			rowacc[0] += wx[i] * (float)p[0] * a;
			rowacc[1] += wx[i] * (float)p[1] * a;
			rowacc[2] += wx[i] * (float)p[2] * a;
			rowacc[3] += wx[i] * (float)p[3];
		}
		for (int c = 0; c < 4; c++)
			acc[c] += wy[j] * rowacc[c];
	}
	float a = acc[3];
	if (a < 0.5f) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}
	float ac = a > 255.0f ? 255.0f : a;
	for (int c = 0; c < 3; c++) {
		float v = acc[c];
		if (v > ac)
			v = ac;
		out[c] = ivp_u8(v * 255.0f / ac);
	}
	out[3] = ivp_u8(a);
}

IvImage *iv_rotate_free(const IvImage *img, double degrees, int crop_to_fit)
{
	if (img == NULL || img->px == NULL)
		return NULL;
	if (!isfinite(degrees))
		return NULL;
	double rad = degrees * IVP_PI / 180.0;
	double cs = cos(rad), sn = sin(rad);
	if (fabs(sn) < 1e-9 && cs > 0.0)
		return iv_image_copy(img);

	const int w = img->w, h = img->h;
	int ow, oh;
	if (crop_to_fit) {
		double rw, rh;
		if (crop_to_fit == 2)
			iv_straighten_inner_rect_aspect(w, h, degrees, &rw, &rh);
		else
			iv_straighten_inner_rect(w, h, degrees, &rw, &rh);
		/* Floor with a small tolerance so an exact fit is not lost to
		 * rounding, and never round outward into the empty corners. */
		ow = (int)floor(rw + 1e-6);
		oh = (int)floor(rh + 1e-6);
	} else {
		ow = (int)ceil(fabs(w * cs) + fabs(h * sn) - 1e-6);
		oh = (int)ceil(fabs(w * sn) + fabs(h * cs) - 1e-6);
	}
	if (ow < 1)
		ow = 1;
	if (oh < 1)
		oh = 1;
	IvImage *d = iv_image_new(ow, oh);
	if (d == NULL)
		return NULL;

	/* Output pixel centre (X, Y) relative to the output centre, turned
	 * back by -angle, lands on the source position.  Clockwise on screen
	 * (y pointing down) is the ordinary rotation matrix. */
	const double ocx = ow * 0.5, ocy = oh * 0.5;
	const double icx = w * 0.5, icy = h * 0.5;
	for (int y = 0; y < oh; y++) {
		double dy = (double)y + 0.5 - ocy;
		uint8_t *row = ivp_px(d, 0, y);
		for (int x = 0; x < ow; x++) {
			double dx = (double)x + 0.5 - ocx;
			double sx = dx * cs + dy * sn + icx - 0.5;
			double sy = -dx * sn + dy * cs + icy - 0.5;
			if (sx < -2.0 || sy < -2.0 || sx > w + 1.0 || sy > h + 1.0) {
				row[x * 4 + 0] = row[x * 4 + 1] = 0;
				row[x * 4 + 2] = row[x * 4 + 3] = 0;
				continue;
			}
			sample_bicubic(img, (float)sx, (float)sy, row + x * 4);
		}
	}

	if (crop_to_fit) {
		/* The inner rectangle is entirely covered by the source, but
		 * the bicubic taps of its outermost pixels may reach past the
		 * source edge and lose a little alpha; for an opaque source
		 * the result must stay opaque. */
		if (iv_image_is_opaque(img)) {
			for (int y = 0; y < oh; y++) {
				uint8_t *row = ivp_px(d, 0, y);
				for (int x = 0; x < ow; x++)
					row[x * 4 + 3] = 255;
			}
		}
	}
	return d;
}

IvImage *iv_crop(const IvImage *img, int x, int y, int w, int h)
{
	if (img == NULL || img->px == NULL)
		return NULL;
	/* Clip the requested rectangle to the image. */
	int x0 = ivp_clampi(x, 0, img->w), y0 = ivp_clampi(y, 0, img->h);
	long x1l = (long)x + (long)w, y1l = (long)y + (long)h;
	int x1 = (int)(x1l < 0 ? 0 : (x1l > img->w ? img->w : x1l));
	int y1 = (int)(y1l < 0 ? 0 : (y1l > img->h ? img->h : y1l));
	if (x1 <= x0 || y1 <= y0)
		return NULL;
	IvImage *d = iv_image_new(x1 - x0, y1 - y0);
	if (d == NULL)
		return NULL;
	for (int r = y0; r < y1; r++)
		memcpy(ivp_px(d, 0, r - y0), ivp_px(img, x0, r),
		       (size_t)(x1 - x0) * 4);
	return d;
}
