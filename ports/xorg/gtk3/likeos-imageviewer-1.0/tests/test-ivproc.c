// LikeOS -- LikeOS Image Viewer: unit tests of the processing library.
//
// Usage: test-ivproc [suite ...]   (no argument or "all": every suite)
//
// Environment:
//   IVPROC_OUTDIR   directory for before/after PNGs of the retouch and
//                   background tests (nothing is written when unset)
//   IVPROC_AVATAR   a PNG screenshot; IVPROC_AVATAR_RECT="x,y,w,h" picks
//                   the photo inside it for the real-image background test
//   IVPROC_FUZZ     iterations per fuzz seed (default 20000)
//
// Exit status: 0 when every check passed.
//
// Copyright (C) 2026 The LikeOS Project

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ivproc.h"
#include "testpng.h"

/* ------------------------------------------------------------------------
 * Framework
 * --------------------------------------------------------------------- */

static int g_fail, g_checks;
static const char *g_outdir;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		g_checks++;                                                  \
		if (!(cond)) {                                               \
			g_fail++;                                            \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
			printf(__VA_ARGS__);                                 \
			printf("\n");                                        \
		}                                                            \
	} while (0)

static void info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void save_png(const char *name, const IvImage *img)
{
	if (g_outdir == NULL || img == NULL)
		return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", g_outdir, name);
	if (tp_write_png(path, img) == 0)
		info("wrote %s", path);
}

static void save_mask(const char *name, const uint8_t *m, int w, int h)
{
	if (g_outdir == NULL)
		return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", g_outdir, name);
	if (tp_write_mask_png(path, m, w, h) == 0)
		info("wrote %s", path);
}

/* Deterministic pseudo random numbers for the generators. */
static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double rndf(void)
{
	return (rnd() & 0xFFFFFF) / 16777216.0;
}

static inline uint8_t *PX(const IvImage *img, int x, int y)
{
	return img->px + (size_t)y * img->stride + (size_t)x * 4;
}

static uint8_t clamp8(double v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)(v + 0.5));
}

static int max_diff(const IvImage *a, const IvImage *b)
{
	if (a->w != b->w || a->h != b->h)
		return 9999;
	int m = 0;
	for (int y = 0; y < a->h; y++)
		for (int x = 0; x < a->w * 4; x++) {
			int d = abs((int)PX(a, 0, y)[x] - (int)PX(b, 0, y)[x]);
			if (d > m)
				m = d;
		}
	return m;
}

static double luma_of(const uint8_t *p)
{
	return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

/* Mean and standard deviation of luma over a rectangle. */
static void stats(const IvImage *img, int x0, int y0, int x1, int y1,
		  double *mean, double *sd)
{
	double s = 0, s2 = 0;
	long n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			double l = luma_of(PX(img, x, y));
			s += l;
			s2 += l * l;
			n++;
		}
	*mean = s / n;
	if (sd)
		*sd = sqrt(fmax(0.0, s2 / n - (*mean) * (*mean)));
}

/* A photo-like test image: colour gradients, soft blobs, fine noise. */
static IvImage *gen_photo(int w, int h, uint32_t seed)
{
	IvImage *img = iv_image_new(w, h);
	rng_state = seed;
	double cx[6], cy[6], cr[6], cc[6][3];
	for (int i = 0; i < 6; i++) {
		cx[i] = rndf() * w;
		cy[i] = rndf() * h;
		cr[i] = (0.1 + 0.25 * rndf()) * (w > h ? w : h);
		for (int c = 0; c < 3; c++)
			cc[i][c] = rndf() * 120 - 60;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double v[3] = { 70 + 110.0 * x / w, 90 + 90.0 * y / h, 170 - 80.0 * x / w };
			for (int i = 0; i < 6; i++) {
				double d2 = ((x - cx[i]) * (x - cx[i]) + (y - cy[i]) * (y - cy[i])) /
					    (cr[i] * cr[i]);
				double g = exp(-d2);
				for (int c = 0; c < 3; c++)
					v[c] += cc[i][c] * g;
			}
			double n = (rndf() - 0.5) * 10;
			uint8_t *p = PX(img, x, y);
			for (int c = 0; c < 3; c++)
				p[c] = clamp8(v[c] + n);
			p[3] = 255;
		}
	return img;
}

/* Value noise texture (grass/sand like), amplitude a around base colour. */
static void add_texture(IvImage *img, double amp, double cell, uint32_t seed)
{
	int gw = (int)(img->w / cell) + 3, gh = (int)(img->h / cell) + 3;
	double *g = malloc(sizeof(double) * gw * gh);
	rng_state = seed;
	for (int i = 0; i < gw * gh; i++)
		g[i] = rndf() * 2 - 1;
	for (int y = 0; y < img->h; y++)
		for (int x = 0; x < img->w; x++) {
			double fx = x / cell, fy = y / cell;
			int ix = (int)fx, iy = (int)fy;
			double tx = fx - ix, ty = fy - iy;
			tx = tx * tx * (3 - 2 * tx);
			ty = ty * ty * (3 - 2 * ty);
			double a = g[iy * gw + ix], b = g[iy * gw + ix + 1];
			double c = g[(iy + 1) * gw + ix], d = g[(iy + 1) * gw + ix + 1];
			double v = (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
			uint8_t *p = PX(img, x, y);
			for (int k = 0; k < 3; k++)
				p[k] = clamp8(p[k] + v * amp * (k == 1 ? 1.0 : 0.8));
		}
	free(g);
}

static void draw_disc(IvImage *img, double cx, double cy, double r,
		      const uint8_t col[3], uint8_t *mask)
{
	for (int y = 0; y < img->h; y++)
		for (int x = 0; x < img->w; x++) {
			double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
			double cov = r + 0.5 - d;
			if (cov <= 0)
				continue;
			if (cov > 1)
				cov = 1;
			uint8_t *p = PX(img, x, y);
			for (int c = 0; c < 3; c++)
				p[c] = clamp8(p[c] + (col[c] - p[c]) * cov);
			if (mask && d < r + 3)	/* the user paints a little wider */
				mask[(size_t)y * img->w + x] = 255;
		}
}

/* ------------------------------------------------------------------------
 * image
 * --------------------------------------------------------------------- */

static void test_image(void)
{
	IvImage *a = iv_image_new(17, 9);
	CHECK(a && a->stride == 68 && a->px[0] == 0 && a->px[3] == 0, "new image zeroed");
	CHECK(iv_image_new(0, 5) == NULL && iv_image_new(5, -1) == NULL, "bad sizes rejected");
	for (int i = 0; i < 17 * 9 * 4; i++)
		a->px[i] = (uint8_t)(i * 7);
	IvImage *b = iv_image_copy(a);
	CHECK(b && max_diff(a, b) == 0, "copy identical");
	CHECK(!iv_image_is_opaque(a), "not opaque");
	/* RGB import */
	uint8_t rgb[3 * 4 * 2 + 8];
	for (int i = 0; i < (int)sizeof(rgb); i++)
		rgb[i] = (uint8_t)(i * 11);
	IvImage *c = iv_image_new_from_data(rgb, 4, 2, 16, 3);
	CHECK(c && PX(c, 1, 1)[0] == rgb[16 + 3] && PX(c, 1, 1)[3] == 255, "RGB import with stride");
	CHECK(iv_image_is_opaque(c), "imported RGB opaque");
	/* cairo round trip: exact for opaque, close for translucent */
	IvImage *g = gen_photo(40, 30, 7);
	for (int y = 0; y < 30; y++)
		for (int x = 0; x < 40; x++)
			PX(g, x, y)[3] = (uint8_t)(x < 20 ? 255 : 60 + x * 4);
	uint8_t *cr = malloc(40 * 4 * 30);
	iv_image_to_cairo(g, cr, 160);
	IvImage *back = iv_image_from_cairo(cr, 40, 30, 160);
	int worst_opaque = 0, worst_tr = 0;
	for (int y = 0; y < 30; y++)
		for (int x = 0; x < 40; x++)
			for (int k = 0; k < 4; k++) {
				int d = abs(PX(g, x, y)[k] - PX(back, x, y)[k]);
				if (x < 20 && d > worst_opaque)
					worst_opaque = d;
				if (x >= 20 && d > worst_tr)
					worst_tr = d;
			}
	CHECK(worst_opaque == 0, "cairo round trip opaque exact (%d)", worst_opaque);
	CHECK(worst_tr <= 2, "cairo round trip translucent within 2 (%d)", worst_tr);
	free(cr);
	iv_image_free(a);
	iv_image_free(b);
	iv_image_free(c);
	iv_image_free(g);
	iv_image_free(back);
}

/* ------------------------------------------------------------------------
 * scale
 * --------------------------------------------------------------------- */

static void test_scale(void)
{
	for (int q = 0; q <= 2; q++) {
		IvImage *flat = iv_image_new(97, 61);
		for (int i = 0; i < 97 * 61; i++) {
			flat->px[i * 4 + 0] = 200;
			flat->px[i * 4 + 1] = 100;
			flat->px[i * 4 + 2] = 30;
			flat->px[i * 4 + 3] = 255;
		}
		IvImage *d = iv_image_scale(flat, 33, 20, q);
		IvImage *u = iv_image_scale(flat, 211, 150, q);
		int ok = 1;
		for (int i = 0; i < 33 * 20 && ok; i++)
			ok = abs(d->px[i * 4] - 200) <= 1 && abs(d->px[i * 4 + 1] - 100) <= 1 &&
			     d->px[i * 4 + 3] == 255;
		for (int i = 0; i < 211 * 150 && ok; i++)
			ok = abs(u->px[i * 4] - 200) <= 1 && u->px[i * 4 + 3] == 255;
		CHECK(ok, "quality %d: flat image stays flat", q);
		iv_image_free(d);
		iv_image_free(u);

		/* 1-px checkerboard shrunk 4x averages to mid-grey (no
		 * aliasing / moire). */
		IvImage *cb = iv_image_new(256, 256);
		for (int y = 0; y < 256; y++)
			for (int x = 0; x < 256; x++) {
				uint8_t v = ((x + y) & 1) ? 255 : 0;
				uint8_t *p = PX(cb, x, y);
				p[0] = p[1] = p[2] = v;
				p[3] = 255;
			}
		IvImage *s = iv_image_scale(cb, 64, 64, q);
		int lo = 255, hi = 0;
		for (int y = 2; y < 62; y++)
			for (int x = 2; x < 62; x++) {
				int v = PX(s, x, y)[0];
				if (v < lo)
					lo = v;
				if (v > hi)
					hi = v;
			}
		CHECK(lo >= 120 && hi <= 135, "quality %d: checkerboard -> grey %d..%d", q, lo, hi);
		iv_image_free(cb);
		iv_image_free(s);

		/* Premultiplied: transparent red neighbours must not tint
		 * opaque blue. */
		IvImage *t = iv_image_new(64, 64);
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++) {
				uint8_t *p = PX(t, x, y);
				if (x < 32) {
					p[0] = 255;	/* invisible red */
					p[3] = 0;
				} else {
					p[2] = 255;
					p[3] = 255;
				}
			}
		IvImage *ts = iv_image_scale(t, 21, 21, q);
		int red = 0;
		for (int y = 0; y < 21; y++)
			for (int x = 0; x < 21; x++)
				if (PX(ts, x, y)[3] > 0 && PX(ts, x, y)[0] > red)
					red = PX(ts, x, y)[0];
		CHECK(red <= 2, "quality %d: no colour bleed from transparent pixels (red %d)", q, red);
		iv_image_free(t);
		iv_image_free(ts);
		iv_image_free(flat);
	}
	/* Timing: a 4000x3000 photo to the 2048 proxy with Lanczos. */
	IvImage *big = gen_photo(4000, 3000, 3);
	double t0 = now_ms();
	IvImage *pr = iv_image_scale(big, 2048, 1536, 2);
	double t1 = now_ms();
	CHECK(pr != NULL, "large downscale");
	info("Lanczos3 4000x3000 -> 2048x1536: %.0f ms", t1 - t0);
	iv_image_free(big);
	iv_image_free(pr);
}

/* ------------------------------------------------------------------------
 * adjust
 * --------------------------------------------------------------------- */

static IvImage *apply_one(const IvImage *src, size_t field, double v)
{
	IvImage *o = iv_image_copy(src);
	IvAdjust a;
	iv_adjust_reset(&a);
	((double *)&a)[field] = v;
	iv_adjust_apply(o, &a);
	return o;
}

#define FIELD(name) (offsetof(IvAdjust, name) / sizeof(double))

static void test_adjust(void)
{
	IvImage *src = gen_photo(320, 240, 11);
	IvAdjust a;
	iv_adjust_reset(&a);
	CHECK(iv_adjust_is_neutral(&a), "reset is neutral");
	IvImage *o = iv_image_copy(src);
	iv_adjust_apply(o, &a);
	CHECK(max_diff(o, src) == 0, "neutral adjust = identity");
	a.exposure = 0.3;	/* below one slider step */
	iv_adjust_apply(o, &a);
	CHECK(max_diff(o, src) <= 1, "sub-step value = identity");
	iv_image_free(o);

	/* Every field moves the image and keeps it valid. */
	const char *names[] = { "brightness", "exposure", "contrast", "highlights",
				"shadows", "whites", "blacks", "vignette",
				"saturation", "vibrance", "warmth", "tint",
				"clarity", "sharpness", "denoise", "fade", "grain" };
	for (size_t f = 0; f < sizeof(IvAdjust) / sizeof(double); f++) {
		for (int s = -1; s <= 1; s += 2) {
			if (f == FIELD(denoise) && s < 0)
				continue;	/* documented no-op */
			IvImage *r = apply_one(src, f, 100.0 * s);
			int d = max_diff(r, src);
			int alpha_ok = 1;
			for (int i = 0; i < r->w * r->h; i++)
				alpha_ok &= r->px[i * 4 + 3] == 255;
			CHECK(d > 3 && alpha_ok, "%s %+d changes the image (max diff %d)",
			      names[f], s * 100, d);
			iv_image_free(r);
		}
	}

	double m0, s0, m1, s1;
	stats(src, 0, 0, src->w, src->h, &m0, &s0);
	IvImage *r = apply_one(src, FIELD(exposure), 60);
	stats(r, 0, 0, r->w, r->h, &m1, NULL);
	CHECK(m1 > m0 + 15, "exposure + brightens (%.1f -> %.1f)", m0, m1);
	iv_image_free(r);
	r = apply_one(src, FIELD(exposure), -60);
	stats(r, 0, 0, r->w, r->h, &m1, NULL);
	CHECK(m1 < m0 - 15, "exposure - darkens (%.1f -> %.1f)", m0, m1);
	iv_image_free(r);
	r = apply_one(src, FIELD(contrast), 70);
	stats(r, 0, 0, r->w, r->h, &m1, &s1);
	CHECK(s1 > s0 * 1.2 && fabs(m1 - m0) < 12, "contrast + spreads (sd %.1f -> %.1f)", s0, s1);
	iv_image_free(r);
	r = apply_one(src, FIELD(contrast), -70);
	stats(r, 0, 0, r->w, r->h, &m1, &s1);
	CHECK(s1 < s0 * 0.85, "contrast - flattens (sd %.1f -> %.1f)", s0, s1);
	iv_image_free(r);
	r = apply_one(src, FIELD(brightness), 60);
	stats(r, 0, 0, r->w, r->h, &m1, NULL);
	CHECK(m1 > m0 + 10, "brightness + (%.1f -> %.1f)", m0, m1);
	iv_image_free(r);

	r = apply_one(src, FIELD(saturation), -100);
	int grey = 1;
	for (int i = 0; i < r->w * r->h; i++) {
		uint8_t *p = r->px + i * 4;
		if (abs(p[0] - p[1]) > 1 || abs(p[1] - p[2]) > 1)
			grey = 0;
	}
	CHECK(grey, "saturation -100 = greyscale");
	iv_image_free(r);

	/* Warmth / tint direction on a neutral grey. */
	IvImage *g = iv_image_new(64, 64);
	for (int i = 0; i < 64 * 64; i++) {
		g->px[i * 4] = g->px[i * 4 + 1] = g->px[i * 4 + 2] = 128;
		g->px[i * 4 + 3] = 255;
	}
	r = apply_one(g, FIELD(warmth), 60);
	CHECK(r->px[0] > r->px[2] + 20, "warmth + = amber (%d,%d,%d)", r->px[0], r->px[1], r->px[2]);
	iv_image_free(r);
	r = apply_one(g, FIELD(warmth), -60);
	CHECK(r->px[2] > r->px[0] + 20, "warmth - = blue (%d,%d,%d)", r->px[0], r->px[1], r->px[2]);
	iv_image_free(r);
	r = apply_one(g, FIELD(tint), 60);
	CHECK(r->px[1] + 10 < r->px[0] && r->px[1] + 10 < r->px[2], "tint + = magenta (%d,%d,%d)",
	      r->px[0], r->px[1], r->px[2]);
	iv_image_free(r);
	r = apply_one(g, FIELD(tint), -60);
	CHECK(r->px[1] > r->px[0] + 10, "tint - = green (%d,%d,%d)", r->px[0], r->px[1], r->px[2]);
	iv_image_free(r);
	/* Vignette darkens corners only. */
	r = apply_one(g, FIELD(vignette), -80);
	CHECK(PX(r, 0, 0)[0] < 80 && abs(PX(r, 32, 32)[0] - 128) <= 2,
	      "vignette - corners %d centre %d", PX(r, 0, 0)[0], PX(r, 32, 32)[0]);
	iv_image_free(r);
	r = apply_one(g, FIELD(fade), 80);
	CHECK(r->px[0] != 128, "fade changes grey");
	iv_image_free(r);
	/* Grain: deterministic and adds texture. */
	r = apply_one(g, FIELD(grain), 80);
	IvImage *r2 = apply_one(g, FIELD(grain), 80);
	double gm, gs;
	stats(r, 0, 0, 64, 64, &gm, &gs);
	CHECK(max_diff(r, r2) == 0 && gs > 2.0 && fabs(gm - 128) < 3, "grain deterministic, sd %.2f", gs);
	iv_image_free(r);
	iv_image_free(r2);
	iv_image_free(g);

	/* Shadows lift dark areas much more than bright ones; highlights
	 * the reverse. */
	IvImage *hl = iv_image_new(200, 100);
	for (int y = 0; y < 100; y++)
		for (int x = 0; x < 200; x++) {
			uint8_t v = x < 100 ? 40 : 215;
			uint8_t *p = PX(hl, x, y);
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	r = apply_one(hl, FIELD(shadows), 80);
	int dark_gain = PX(r, 30, 50)[0] - 40, bright_gain = PX(r, 170, 50)[0] - 215;
	CHECK(dark_gain > 20 && abs(bright_gain) <= 4, "shadows +: dark +%d, bright %+d",
	      dark_gain, bright_gain);
	/* Halo check: next to the edge the bright side stays bright. */
	CHECK(PX(r, 104, 50)[0] >= 205, "shadows +: little halo on the bright side (%d)", PX(r, 104, 50)[0]);
	iv_image_free(r);
	r = apply_one(hl, FIELD(highlights), -80);
	dark_gain = PX(r, 30, 50)[0] - 40;
	bright_gain = PX(r, 170, 50)[0] - 215;
	CHECK(bright_gain < -15 && abs(dark_gain) <= 4, "highlights -: bright %+d, dark %+d",
	      bright_gain, dark_gain);
	iv_image_free(r);
	iv_image_free(hl);

	/* Denoise reduces noise on a flat patch but keeps a strong edge. */
	IvImage *nz = iv_image_new(128, 64);
	rng_state = 99;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 128; x++) {
			double base = x < 64 ? 60 : 190;
			uint8_t v = clamp8(base + (rndf() - 0.5) * 24);
			uint8_t *p = PX(nz, x, y);
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	double n0, ns0, n1, ns1;
	stats(nz, 4, 4, 56, 60, &n0, &ns0);
	r = apply_one(nz, FIELD(denoise), 80);
	stats(r, 4, 4, 56, 60, &n1, &ns1);
	CHECK(ns1 < ns0 * 0.6, "denoise: noise sd %.2f -> %.2f", ns0, ns1);
	CHECK(PX(r, 62, 30)[0] < 100 && PX(r, 66, 30)[0] > 150, "denoise keeps the edge");
	iv_image_free(r);
	/* Sharpness raises local contrast at the edge. */
	r = apply_one(nz, FIELD(sharpness), 80);
	double e0 = PX(nz, 64, 30)[0] - (double)PX(nz, 63, 30)[0];
	double e1 = PX(r, 64, 30)[0] - (double)PX(r, 63, 30)[0];
	CHECK(e1 >= e0, "sharpness keeps/increases the step (%.0f -> %.0f)", e0, e1);
	iv_image_free(r);
	iv_image_free(nz);

	/* Timing: global operators on a 2048 px proxy. */
	IvImage *proxy = gen_photo(2048, 1536, 5);
	IvAdjust all;
	iv_adjust_reset(&all);
	all.brightness = 20;
	all.exposure = 15;
	all.contrast = 25;
	all.highlights = -30;
	all.shadows = 30;
	all.whites = 10;
	all.blacks = -10;
	all.vignette = -20;
	all.saturation = 15;
	all.vibrance = 20;
	all.warmth = 10;
	all.tint = 5;
	all.fade = 10;
	double t0 = now_ms();
	iv_adjust_apply_ex(proxy, &all, 0.5);
	double t1 = now_ms();
	info("global adjust (13 sliders) on 2048x1536: %.0f ms", t1 - t0);
	CHECK(t1 - t0 < 600, "global adjust on the proxy is interactive (%.0f ms)", t1 - t0);
	IvAdjust det;
	iv_adjust_reset(&det);
	det.clarity = 40;
	det.sharpness = 40;
	det.denoise = 30;
	det.grain = 20;
	t0 = now_ms();
	iv_adjust_apply_ex(proxy, &det, 0.5);
	t1 = now_ms();
	info("detail adjust (clarity, sharpness, denoise, grain) on 2048x1536: %.0f ms", t1 - t0);
	iv_image_free(proxy);
	iv_image_free(src);
}

/* ------------------------------------------------------------------------
 * styles
 * --------------------------------------------------------------------- */

static void test_styles(void)
{
	static const char *forbidden[] = { "Punch", "Golden", "Radiate", "Warm contrast",
					   "Calm", "Cool light", "Vivid cool", "Dramatic cool",
					   "Dramatic warm", "Vivid warm" };
	CHECK(IV_STYLE_COUNT - 1 >= 16, "at least 16 styles (%d)", IV_STYLE_COUNT - 1);
	CHECK(strcmp(iv_style_name(IV_STYLE_NONE), "Original") == 0, "NONE is Original");
	CHECK(strcmp(iv_style_name(IV_STYLE_COUNT), "") == 0, "out of range name empty");
	IvImage *src = gen_photo(160, 120, 21);
	for (int s = 1; s < IV_STYLE_COUNT; s++) {
		const char *n = iv_style_name((IvStyle)s);
		CHECK(n && *n, "style %d has a name", s);
		for (int k = 0; k < s; k++)
			CHECK(strcmp(n, iv_style_name((IvStyle)k)) != 0, "style name %s unique", n);
		for (size_t f = 0; f < sizeof(forbidden) / sizeof(forbidden[0]); f++)
			CHECK(strcmp(n, forbidden[f]) != 0, "style name %s is our own", n);
		IvImage *a = iv_image_copy(src);
		iv_style_apply(a, (IvStyle)s, 0.0);
		CHECK(max_diff(a, src) == 0, "%s at intensity 0 = identity", n);
		iv_style_apply(a, (IvStyle)s, 1.0);
		int d = max_diff(a, src);
		CHECK(d > 8, "%s changes the image (%d)", n, d);
		IvImage *h = iv_image_copy(src);
		iv_style_apply(h, (IvStyle)s, 0.5);
		int dh = max_diff(h, src);
		CHECK(dh < d && dh > 0, "%s half intensity is in between (%d < %d)", n, dh, d);
		info("style %-12s max change %3d", n, d);
		iv_image_free(a);
		iv_image_free(h);
	}
	IvImage *a = iv_image_copy(src);
	iv_style_apply(a, IV_STYLE_NOIR, 1.0);
	int mono = 1;
	for (int i = 0; i < a->w * a->h; i++) {
		uint8_t *p = a->px + i * 4;
		if (abs(p[0] - p[1]) > 2 || abs(p[1] - p[2]) > 2)
			mono = 0;
	}
	CHECK(mono, "Noir is monochrome");
	iv_image_free(a);
	/* A strip of every style for a look. */
	if (g_outdir) {
		IvImage *sheet = iv_image_new(160 * 7, 120 * 3);
		for (int s = 0; s < IV_STYLE_COUNT && s < 21; s++) {
			IvImage *t = iv_image_copy(src);
			iv_style_apply(t, (IvStyle)s, 1.0);
			int ox = (s % 7) * 160, oy = (s / 7) * 120;
			for (int y = 0; y < 120; y++)
				memcpy(PX(sheet, ox, oy + y), PX(t, 0, y), 160 * 4);
			iv_image_free(t);
		}
		save_png("styles-sheet.png", sheet);
		iv_image_free(sheet);
	}
	iv_image_free(src);
}

/* ------------------------------------------------------------------------
 * enhance
 * --------------------------------------------------------------------- */

static void test_enhance(void)
{
	/* Dull, low-contrast, blue-cast image. */
	IvImage *src = gen_photo(300, 200, 31);
	for (int i = 0; i < 300 * 200; i++) {
		uint8_t *p = src->px + i * 4;
		for (int c = 0; c < 3; c++)
			p[c] = (uint8_t)(90 + p[c] * 0.35);
		p[2] = clamp8(p[2] * 1.15 + 6);
		p[0] = clamp8(p[0] * 0.92);
	}
	IvAdjust a;
	iv_auto_enhance(src, &a);
	info("auto enhance: blacks %.0f whites %.0f bright %.0f contrast %.0f shadows %.0f "
	     "highlights %.0f warmth %.0f tint %.0f", a.blacks, a.whites, a.brightness,
	     a.contrast, a.shadows, a.highlights, a.warmth, a.tint);
	CHECK(a.blacks < -5 && a.whites > 5, "levels stretched");
	CHECK(a.warmth > 5, "blue cast corrected towards warm (%.0f)", a.warmth);
	CHECK(a.contrast >= 0 && a.contrast <= 30, "gentle contrast");
	double m0, s0, m1, s1;
	stats(src, 0, 0, 300, 200, &m0, &s0);
	IvImage *o = iv_image_copy(src);
	iv_adjust_apply(o, &a);
	stats(o, 0, 0, 300, 200, &m1, &s1);
	CHECK(s1 > s0 * 1.4, "enhanced image has more spread (sd %.1f -> %.1f)", s0, s1);
	double rb0 = 0, rb1 = 0;
	for (int i = 0; i < 300 * 200; i++) {
		rb0 += (double)src->px[i * 4 + 2] - src->px[i * 4];
		rb1 += (double)o->px[i * 4 + 2] - o->px[i * 4];
	}
	CHECK(fabs(rb1) < fabs(rb0) * 0.8, "blue-red imbalance reduced (%.1f -> %.1f)",
	      rb0 / 60000, rb1 / 60000);
	save_png("enhance-before.png", src);
	save_png("enhance-after.png", o);
	iv_image_free(o);
	iv_image_free(src);

	/* Under-exposed: brightness / shadows lifted. */
	IvImage *dark = gen_photo(300, 200, 32);
	for (int i = 0; i < 300 * 200; i++)
		for (int c = 0; c < 3; c++)
			dark->px[i * 4 + c] = (uint8_t)(dark->px[i * 4 + c] * 0.3);
	iv_auto_enhance(dark, &a);
	CHECK(a.brightness > 5 || a.shadows > 5, "dark image lifted (bright %.0f shadows %.0f)",
	      a.brightness, a.shadows);
	iv_image_free(dark);

	/* A well exposed neutral image needs little. */
	IvImage *ok = iv_image_new(100, 100);
	for (int y = 0; y < 100; y++)
		for (int x = 0; x < 100; x++) {
			uint8_t *p = PX(ok, x, y);
			p[0] = p[1] = p[2] = (uint8_t)(x * 255 / 99);
			p[3] = 255;
		}
	iv_auto_enhance(ok, &a);
	CHECK(fabs(a.warmth) < 3 && fabs(a.tint) < 3 && fabs(a.blacks) < 3 && fabs(a.whites) < 3,
	      "neutral full-range ramp left alone (w %.0f t %.0f b %.0f wh %.0f)",
	      a.warmth, a.tint, a.blacks, a.whites);
	iv_image_free(ok);
}

/* ------------------------------------------------------------------------
 * geometry
 * --------------------------------------------------------------------- */

/* Brute force: the largest area axis-aligned rectangle inside the turned
 * one, by scanning widths (for a centred rectangle). */
static void brute_inner(int w, int h, double deg, double *bw, double *bh)
{
	double a = deg * 3.14159265358979323846 / 180.0;
	double s = fabs(sin(a)), c = fabs(cos(a));
	double best = 0;
	*bw = *bh = 0;
	double maxW = w * c + h * s;
	for (int i = 1; i <= 20000; i++) {
		double W = maxW * i / 20000.0;
		/* Both containment constraints solved for H. */
		double H1 = s > 1e-12 ? (w - W * c) / s : 1e300;
		double H2 = c > 1e-12 ? (h - W * s) / c : 1e300;
		double H = H1 < H2 ? H1 : H2;
		if (H <= 0)
			continue;
		if (W * H > best) {
			best = W * H;
			*bw = W;
			*bh = H;
		}
	}
}

static int fits(int w, int h, double deg, double W, double H)
{
	double a = deg * 3.14159265358979323846 / 180.0;
	double s = fabs(sin(a)), c = fabs(cos(a));
	return W * c + H * s <= w + 1e-6 && W * s + H * c <= h + 1e-6;
}

static void test_geometry(void)
{
	IvImage *src = gen_photo(37, 23, 41);
	PX(src, 0, 0)[0] = 1;
	PX(src, 36, 0)[0] = 2;
	IvImage *r = iv_image_copy(src);
	for (int i = 0; i < 4; i++) {
		IvImage *n = iv_rotate90(r, 1);
		iv_image_free(r);
		r = n;
	}
	CHECK(max_diff(r, src) == 0, "rotate90 x4 = identity");
	iv_image_free(r);
	r = iv_rotate90(src, 1);
	CHECK(r->w == 23 && r->h == 37, "rotate90 swaps size");
	CHECK(PX(r, 22, 0)[0] == 1 && PX(r, 22, 36)[0] == 2, "rotate90 cw: top-left -> top-right");
	IvImage *r2 = iv_rotate90(r, -1);
	CHECK(max_diff(r2, src) == 0, "cw then ccw = identity");
	iv_image_free(r2);
	r2 = iv_rotate90(src, 2);
	CHECK(PX(r2, 36, 22)[0] == 1, "rotate 180");
	iv_image_free(r2);
	r2 = iv_rotate90(src, 7);	/* = 3 */
	IvImage *r3 = iv_rotate90(src, -1);
	CHECK(max_diff(r2, r3) == 0, "turns taken modulo 4");
	iv_image_free(r2);
	iv_image_free(r3);
	iv_image_free(r);

	IvImage *f = iv_image_copy(src);
	iv_flip(f, 1);
	CHECK(PX(f, 36, 0)[0] == 1 && PX(f, 0, 0)[0] == 2, "flip horizontal");
	iv_flip(f, 1);
	CHECK(max_diff(f, src) == 0, "flip h twice = identity");
	iv_flip(f, 0);
	CHECK(PX(f, 0, 22)[0] == 1, "flip vertical");
	iv_flip(f, 0);
	CHECK(max_diff(f, src) == 0, "flip v twice = identity");
	iv_image_free(f);

	/* Inner rectangle: closed form vs brute force. */
	double worst = 0, worst_a = 0;
	int bad = 0, bad_aspect = 0;
	const int sizes[][2] = { { 4000, 3000 }, { 3000, 4000 }, { 1920, 1080 }, { 1000, 1000 },
				 { 2000, 300 }, { 300, 2000 }, { 640, 480 } };
	for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
		for (double deg = -45; deg <= 45.0; deg += 3.7) {
			int w = sizes[si][0], h = sizes[si][1];
			double cw, ch, bw, bh;
			iv_straighten_inner_rect(w, h, deg, &cw, &ch);
			brute_inner(w, h, deg, &bw, &bh);
			double rel = fabs(cw * ch - bw * bh) / (bw * bh);
			if (rel > worst)
				worst = rel;
			if (!fits(w, h, deg, cw, ch) || cw * ch < bw * bh * (1 - 1e-3))
				bad++;
			double aw, ah;
			iv_straighten_inner_rect_aspect(w, h, deg, &aw, &ah);
			if (!fits(w, h, deg, aw, ah) || fabs(aw / ah - (double)w / h) > 1e-9 ||
			    fits(w, h, deg, aw * 1.001, ah * 1.001))
				bad_aspect++;
			if (fabs(aw * ah - bw * bh) / (bw * bh) > worst_a)
				worst_a = fabs(aw * ah - bw * bh) / (bw * bh);
		}
	}
	CHECK(bad == 0, "inner rect fits and is maximal (%d bad, worst rel. area diff %.2e)", bad, worst);
	CHECK(bad_aspect == 0, "aspect inner rect fits, keeps aspect, is maximal (%d bad)", bad_aspect);
	info("inner rect: worst relative area difference to brute force %.2e", worst);
	double iw, ih;
	iv_straighten_inner_rect(400, 300, 0.0, &iw, &ih);
	CHECK(iw == 400 && ih == 300, "0 degrees = full size");

	/* Free rotation. */
	IvImage *p = gen_photo(120, 80, 43);
	IvImage *z = iv_rotate_free(p, 0.0, 0);
	CHECK(max_diff(z, p) == 0, "rotate_free 0 = copy");
	iv_image_free(z);
	z = iv_rotate_free(p, 90.0, 0);
	IvImage *q = iv_rotate90(p, 1);
	CHECK(z->w == q->w && z->h == q->h, "rotate_free 90 size %dx%d", z->w, z->h);
	int md = 0;
	for (int y = 2; y < q->h - 2; y++)
		for (int x = 2; x < q->w - 2; x++)
			for (int k = 0; k < 3; k++) {
				int d = abs(PX(z, x, y)[k] - PX(q, x, y)[k]);
				if (d > md)
					md = d;
			}
	CHECK(md <= 2, "rotate_free 90 = rotate90 inside (max diff %d)", md);
	iv_image_free(z);
	iv_image_free(q);
	z = iv_rotate_free(p, 12.5, 0);
	CHECK(z->w > p->w && PX(z, 0, 0)[3] == 0, "bounding box with transparent corners");
	iv_image_free(z);
	z = iv_rotate_free(p, 12.5, 1);
	iv_straighten_inner_rect(120, 80, 12.5, &iw, &ih);
	CHECK(z->w == (int)floor(iw + 1e-6) && z->h == (int)floor(ih + 1e-6) && iv_image_is_opaque(z),
	      "crop to fit: %dx%d opaque", z->w, z->h);
	iv_image_free(z);
	z = iv_rotate_free(p, -7.0, 2);
	CHECK(fabs((double)z->w / z->h - 1.5) < 0.04, "crop to fit keeping aspect (%dx%d)", z->w, z->h);
	iv_image_free(z);
	/* Content check: rotating a centred dot keeps it centred. */
	IvImage *dot = iv_image_new(101, 101);
	for (int i = 0; i < 101 * 101; i++)
		dot->px[i * 4 + 3] = 255;
	for (int y = 48; y <= 52; y++)
		for (int x = 48; x <= 52; x++)
			PX(dot, x, y)[0] = 255;
	z = iv_rotate_free(dot, 33.0, 1);
	CHECK(PX(z, z->w / 2, z->h / 2)[0] > 200, "centre stays centre");
	iv_image_free(z);
	iv_image_free(dot);

	IvImage *c = iv_crop(p, 10, 5, 30, 20);
	CHECK(c && c->w == 30 && c->h == 20 && memcmp(PX(c, 0, 0), PX(p, 10, 5), 4) == 0, "crop");
	iv_image_free(c);
	c = iv_crop(p, -10, -10, 30, 30);
	CHECK(c && c->w == 20 && c->h == 20, "crop clipped at the origin");
	iv_image_free(c);
	c = iv_crop(p, 200, 0, 10, 10);
	CHECK(c == NULL, "crop outside = NULL");
	iv_image_free(p);
	iv_image_free(src);
}

/* ------------------------------------------------------------------------
 * blur
 * --------------------------------------------------------------------- */

static void test_blur(void)
{
	const double sigmas[] = { 0.8, 1.5, 3.0, 6.0, 15.0 };
	for (size_t i = 0; i < sizeof(sigmas) / sizeof(sigmas[0]); i++) {
		double s = sigmas[i];
		int n = (int)(s * 12) + 21;
		IvImage *img = iv_image_new(n, n);
		for (int k = 0; k < n * n; k++)
			img->px[k * 4 + 3] = 255;
		/* A bright square in the middle: an impulse with enough mass
		 * that 8-bit rounding of the spread-out result stays small
		 * (wider for the wide blurs). */
		int c = n / 2, q = s >= 6.0 ? 10 : 2;
		for (int y = c - q; y <= c + q; y++)
			for (int x = c - q; x <= c + q; x++)
				PX(img, x, y)[0] = 255;
		double side = 2 * q + 1, mass = side * side * 255;
		iv_gaussian_blur(img, s);
		double sum = 0, var = 0;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				double v = PX(img, x, y)[0];
				sum += v;
				var += v * (x - c) * (x - c);
			}
		var /= sum;
		/* A square of side k has variance (k^2 - 1) / 12 per axis. */
		double est = sqrt(fmax(0.0, var - (side * side - 1) / 12.0));
		CHECK(fabs(sum - mass) / mass < 0.03, "sigma %.1f: mass kept (%.0f of %.0f)", s, sum, mass);
		CHECK(fabs(est - s) / s < 0.12, "sigma %.1f: measured %.2f", s, est);
		iv_image_free(img);
	}
	IvImage *flat = iv_image_new(50, 50);
	for (int k = 0; k < 2500; k++) {
		flat->px[k * 4] = 77;
		flat->px[k * 4 + 3] = 255;
	}
	iv_gaussian_blur(flat, 9.0);
	int ok = 1;
	for (int k = 0; k < 2500; k++)
		ok &= flat->px[k * 4] == 77;
	CHECK(ok, "flat image unchanged by blur");
	iv_image_free(flat);

	IvImage *e = iv_image_new(40, 10);
	for (int y = 0; y < 10; y++)
		for (int x = 0; x < 40; x++) {
			uint8_t *p = PX(e, x, y);
			p[0] = p[1] = p[2] = x < 20 ? 80 : 160;
			p[3] = 255;
		}
	iv_unsharp(e, 1.5, 1.0);
	CHECK(PX(e, 19, 5)[0] < 80 && PX(e, 20, 5)[0] > 160, "unsharp overshoots at the edge (%d, %d)",
	      PX(e, 19, 5)[0], PX(e, 20, 5)[0]);
	CHECK(PX(e, 2, 5)[0] == 80 && PX(e, 37, 5)[0] == 160, "unsharp leaves flat areas");
	iv_image_free(e);

	IvImage *big = gen_photo(2048, 1536, 51);
	double t0 = now_ms();
	iv_gaussian_blur(big, 20.0);
	info("gaussian sigma 20 on 2048x1536: %.0f ms", now_ms() - t0);
	iv_image_free(big);
}

/* ------------------------------------------------------------------------
 * mask
 * --------------------------------------------------------------------- */

static void test_mask(void)
{
	uint8_t *m = calloc(100 * 100, 1);
	iv_mask_dab(m, 100, 100, 50, 50, 20, 255, 0.5);
	CHECK(m[50 * 100 + 50] == 255 && m[50 * 100 + 58] == 255, "hard core painted");
	CHECK(m[50 * 100 + 66] > 0 && m[50 * 100 + 66] < 255, "soft falloff (%d)", m[50 * 100 + 66]);
	CHECK(m[50 * 100 + 71] == 0 && m[5 * 100 + 5] == 0, "nothing outside the radius");
	iv_mask_dab(m, 100, 100, 50, 50, 10, 0, 1.0);
	CHECK(m[50 * 100 + 50] == 0 && m[50 * 100 + 58] == 0 && m[50 * 100 + 62] > 150, "erase dab");
	iv_mask_dab(m, 100, 100, -500, 30, 10, 255, 1.0);	/* off image */
	iv_mask_dab(m, 100, 100, 99.5, 99.5, 3, 255, 1.0);
	CHECK(m[99 * 100 + 99] == 255, "dab at the corner");
	int bx, by, bw, bh;
	/* Hardness 0.5: coverage crosses one half at 15 px, so the >= 128
	 * box starts at 35; the corner dab extends it to 99. */
	CHECK(iv_mask_bbox(m, 100, 100, 128, &bx, &by, &bw, &bh) && bx == 35 && by == 35 &&
	      bw == 65 && bh == 65,
	      "bbox %d,%d %dx%d", bx, by, bw, bh);
	uint8_t *f = malloc(100 * 100);
	memset(f, 0, 100 * 100);
	for (int y = 0; y < 100; y++)
		for (int x = 50; x < 100; x++)
			f[y * 100 + x] = 255;
	iv_mask_feather(f, 100, 100, 8);
	CHECK(f[50 * 100 + 40] > 0 && f[50 * 100 + 40] < 60 && f[50 * 100 + 60] > 195 &&
	      abs(f[50 * 100 + 50] - 128) < 30, "feather ramp (%d %d %d)", f[50 * 100 + 40],
	      f[50 * 100 + 50], f[50 * 100 + 60]);
	uint8_t *s = malloc(25 * 25);
	iv_mask_scale(f, 100, 100, s, 25, 25);
	CHECK(s[12 * 25 + 2] < 10 && s[12 * 25 + 22] > 245, "mask shrink");
	uint8_t *u = malloc(200 * 200);
	iv_mask_scale(s, 25, 25, u, 200, 200);
	CHECK(u[100 * 200 + 10] < 10 && u[100 * 200 + 190] > 245, "mask enlarge");
	free(m);
	free(f);
	free(s);
	free(u);
}

/* ------------------------------------------------------------------------
 * histogram
 * --------------------------------------------------------------------- */

static void test_histogram(void)
{
	IvImage *img = gen_photo(64, 48, 61);
	PX(img, 0, 0)[3] = 0;
	uint32_t h[4][256];
	iv_histogram(img, h);
	for (int c = 0; c < 4; c++) {
		uint64_t s = 0;
		for (int i = 0; i < 256; i++)
			s += h[c][i];
		CHECK(s == 64 * 48 - 1, "channel %d counts every visible pixel (%llu)", c,
		      (unsigned long long)s);
	}
	IvImage *w = iv_image_new(4, 4);
	memset(w->px, 255, 64);
	iv_histogram(w, h);
	CHECK(h[3][255] == 16 && h[0][255] == 16, "white maps to 255");
	iv_image_free(w);
	iv_image_free(img);
}

/* ------------------------------------------------------------------------
 * inpaint
 * --------------------------------------------------------------------- */

typedef struct {
	int calls;
	int cancel_after;
	double last;
	int monotone;
} ProgState;

static int prog_cb(double f, void *user)
{
	ProgState *p = user;
	if (f + 1e-9 < p->last)
		p->monotone = 0;
	p->last = f;
	p->calls++;
	return p->cancel_after > 0 && p->calls >= p->cancel_after;
}

/* Mean absolute error inside the mask against the clean image, the mean
 * colour bias, and how much of the blob colour survived. */
static void hole_error(const IvImage *res, const IvImage *truth, const uint8_t *mask,
		       const uint8_t blob[3], double *mae, double *bias, double *blob_left)
{
	double e = 0, b[3] = { 0, 0, 0 };
	long n = 0, nb = 0;
	for (int y = 0; y < res->h; y++)
		for (int x = 0; x < res->w; x++) {
			if (mask[(size_t)y * res->w + x] < 128)
				continue;
			const uint8_t *p = PX(res, x, y), *t = PX(truth, x, y);
			for (int c = 0; c < 3; c++) {
				e += abs(p[c] - t[c]);
				b[c] += p[c] - t[c];
			}
			n++;
			if (abs(p[0] - blob[0]) + abs(p[1] - blob[1]) + abs(p[2] - blob[2]) < 60)
				nb++;
		}
	*mae = e / (3.0 * n);
	*bias = (fabs(b[0]) + fabs(b[1]) + fabs(b[2])) / (3.0 * n);
	*blob_left = (double)nb / n;
}

/* Standard deviation of the high-pass (pixel minus its 3x3 mean) of luma
 * over pixels at distance [r0, r1) from (cx, cy): how much fine texture
 * an area carries. */
static double texture_sd(const IvImage *img, double cx, double cy, double r0, double r1)
{
	double s = 0, s2 = 0;
	long n = 0;
	for (int y = 1; y < img->h - 1; y++)
		for (int x = 1; x < img->w - 1; x++) {
			double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
			if (d < r0 || d >= r1)
				continue;
			double m = 0;
			for (int j = -1; j <= 1; j++)
				for (int i = -1; i <= 1; i++)
					m += luma_of(PX(img, x + i, y + j));
			double hp = luma_of(PX(img, x, y)) - m / 9.0;
			s += hp;
			s2 += hp * hp;
			n++;
		}
	if (n == 0)
		return 0;
	double mean = s / n;
	return sqrt(fmax(0.0, s2 / n - mean * mean));
}

static void inpaint_case(const char *name, IvImage *truth, double cx, double cy,
			 double r, double max_mae, double max_bias)
{
	const uint8_t blob[3] = { 235, 30, 40 };
	int w = truth->w, h = truth->h;
	IvImage *img = iv_image_copy(truth);
	uint8_t *mask = calloc((size_t)w * h, 1);
	draw_disc(img, cx, cy, r, blob, mask);
	char fn[256];
	snprintf(fn, sizeof(fn), "inpaint-%s-before.png", name);
	save_png(fn, img);
	ProgState ps = { 0, 0, 0.0, 1 };
	double t0 = now_ms();
	int rc = iv_inpaint(img, mask, prog_cb, &ps);
	double t1 = now_ms();
	CHECK(rc == 0, "%s: inpaint ok", name);
	double mae, bias, left;
	hole_error(img, truth, mask, blob, &mae, &bias, &left);
	/* The texture is random, so the fill cannot equal the hidden truth;
	 * what matters is that it matches its statistics. */
	info("%s: hole r=%.0f in %dx%d: MAE %.1f, bias %.1f, blob left %.3f, %.0f ms, %d progress calls",
	     name, r, w, h, mae, bias, left, t1 - t0, ps.calls);
	/* Texture inside the fill compared with the ring around the hole:
	 * a fill that is merely smooth scores far below 1. */
	double t_in = texture_sd(img, cx, cy, 0, r + 1);
	double t_out = texture_sd(img, cx, cy, r + 8, r + 24);
	double tex = t_out > 0 ? t_in / t_out : 1.0;
	info("%s: texture inside / around = %.2f", name, tex);
	CHECK(tex > 0.6 && tex < 1.6, "%s: texture kept (%.2f)", name, tex);
	CHECK(left < 0.002, "%s: blob removed (%.3f left)", name, left);
	CHECK(mae < max_mae, "%s: MAE %.1f < %.1f", name, mae, max_mae);
	CHECK(bias < max_bias, "%s: colour bias %.1f < %.1f", name, bias, max_bias);
	CHECK(ps.monotone && ps.calls > 0 && fabs(ps.last - 1.0) < 1e-9, "%s: progress", name);
	/* Outside the (dilated) mask nothing may change. */
	int changed = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
			if (d > r + 7 && memcmp(PX(img, x, y), PX(truth, x, y), 4) != 0)
				changed++;
		}
	CHECK(changed == 0, "%s: pixels outside the mask untouched (%d)", name, changed);
	snprintf(fn, sizeof(fn), "inpaint-%s-after.png", name);
	save_png(fn, img);
	free(mask);
	iv_image_free(img);
}

static void test_inpaint(void)
{
	/* Small dust spot on a photo-like gradient. */
	IvImage *a = gen_photo(240, 180, 71);
	add_texture(a, 12, 3.0, 72);
	inpaint_case("dust", a, 120, 90, 6, 14, 6);
	iv_image_free(a);
	/* Medium object on texture. */
	IvImage *b = gen_photo(320, 240, 73);
	add_texture(b, 25, 4.0, 74);
	inpaint_case("object", b, 150, 120, 22, 22, 8);
	iv_image_free(b);
	/* Stripes: structure must continue through the hole. */
	IvImage *c = iv_image_new(256, 256);
	for (int y = 0; y < 256; y++)
		for (int x = 0; x < 256; x++) {
			double v = 0.5 + 0.5 * sin((x + y * 0.5) * 2 * 3.14159265 / 16.0);
			uint8_t *p = PX(c, x, y);
			p[0] = (uint8_t)(60 + 120 * v);
			p[1] = (uint8_t)(70 + 100 * v);
			p[2] = (uint8_t)(90 + 60 * v);
			p[3] = 255;
		}
	inpaint_case("stripes", c, 128, 128, 20, 20, 8);
	iv_image_free(c);
	/* A straight boundary between two textured regions running through
	 * a large hole: the line must continue, not blur or bend. */
	IvImage *ed = iv_image_new(400, 300);
	for (int y = 0; y < 300; y++)
		for (int x = 0; x < 400; x++) {
			uint8_t *p = PX(ed, x, y);
			int above = y < 0.35 * x + 80;
			p[0] = above ? 90 : 200;
			p[1] = above ? 140 : 170;
			p[2] = above ? 210 : 110;
			p[3] = 255;
		}
	add_texture(ed, 16, 3.0, 80);
	inpaint_case("edge", ed, 200, 150, 45, 14, 6);
	iv_image_free(ed);
	/* Big hole: goes through the pyramid. */
	IvImage *d = gen_photo(640, 480, 75);
	add_texture(d, 18, 5.0, 76);
	inpaint_case("big", d, 330, 230, 70, 24, 10);
	iv_image_free(d);

	/* Two separate spots, one at the image border. */
	IvImage *e = gen_photo(200, 150, 77);
	add_texture(e, 10, 3.0, 78);
	IvImage *ei = iv_image_copy(e);
	uint8_t *mask = calloc(200 * 150, 1);
	const uint8_t col[3] = { 10, 250, 10 };
	draw_disc(ei, 40, 40, 5, col, mask);
	draw_disc(ei, 197, 100, 6, col, mask);
	int rc = iv_inpaint(ei, mask, NULL, NULL);
	double mae, bias, left;
	hole_error(ei, e, mask, col, &mae, &bias, &left);
	CHECK(rc == 0 && left < 0.01 && mae < 16, "two spots incl. border (MAE %.1f, left %.3f)", mae, left);
	iv_image_free(ei);

	/* Cancel leaves the image untouched. */
	IvImage *f = gen_photo(400, 300, 79);
	IvImage *fc = iv_image_copy(f);
	memset(mask, 0, 200 * 150);
	uint8_t *m2 = calloc(400 * 300, 1);
	draw_disc(f, 200, 150, 60, col, m2);
	IvImage *fbefore = iv_image_copy(f);
	ProgState ps = { 0, 2, 0.0, 1 };
	rc = iv_inpaint(f, m2, prog_cb, &ps);
	CHECK(rc == 1 && max_diff(f, fbefore) == 0, "cancel returns 1 and keeps the image (rc %d)", rc);
	/* Empty mask: nothing to do. */
	memset(m2, 0, 400 * 300);
	rc = iv_inpaint(fc, m2, NULL, NULL);
	CHECK(rc == 0, "empty mask ok");
	CHECK(iv_inpaint(NULL, m2, NULL, NULL) == -1, "NULL image rejected");
	free(m2);
	free(mask);
	iv_image_free(f);
	iv_image_free(fc);
	iv_image_free(fbefore);
	iv_image_free(e);
}

/* ------------------------------------------------------------------------
 * background
 * --------------------------------------------------------------------- */

/* A subject: a textured, multi-coloured blob shape (head and shoulders
 * reaching the bottom edge). */
static void draw_subject(IvImage *img, uint8_t *truth)
{
	int w = img->w, h = img->h;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double nx = (x - w * 0.5) / (w * 0.16), ny = (y - h * 0.40) / (h * 0.2);
			int head = nx * nx + ny * ny <= 1.0;
			double bx = (x - w * 0.5) / (w * 0.32), by = (y - h * 1.0) / (h * 0.38);
			int body = bx * bx + by * by <= 1.0;
			if (!head && !body)
				continue;
			truth[(size_t)y * w + x] = 1;
			uint8_t *p = PX(img, x, y);
			double t = sin(x * 0.21) * cos(y * 0.17);
			if (head) {
				p[0] = clamp8(205 + 25 * t);
				p[1] = clamp8(160 + 20 * t);
				p[2] = clamp8(130 + 15 * t);
			} else {
				p[0] = clamp8(40 + 30 * t);
				p[1] = clamp8(60 + 30 * t);
				p[2] = clamp8(140 + 40 * t);
			}
		}
}

static double iou_bg(const uint8_t *mask, const uint8_t *truth_fg, int n)
{
	long inter = 0, uni = 0;
	for (int i = 0; i < n; i++) {
		int a = mask[i] >= 128, b = !truth_fg[i];
		inter += a && b;
		uni += a || b;
	}
	return uni ? (double)inter / uni : 0.0;
}

static void bg_case(const char *name, IvImage *img, uint8_t *truth, double min_iou)
{
	int w = img->w, h = img->h;
	uint8_t *m = malloc((size_t)w * h);
	double t0 = now_ms();
	int rc = iv_bg_estimate(img, m, NULL, NULL);
	double t1 = now_ms();
	double iou = iou_bg(m, truth, w * h);
	info("%s: rc %d, background IoU %.3f, %.0f ms", name, rc, iou, t1 - t0);
	CHECK(rc == 1, "%s: clear background found", name);
	CHECK(iou >= min_iou, "%s: IoU %.3f >= %.2f", name, iou, min_iou);
	char fn[256];
	snprintf(fn, sizeof(fn), "bg-%s-input.png", name);
	save_png(fn, img);
	snprintf(fn, sizeof(fn), "bg-%s-mask.png", name);
	save_mask(fn, m, w, h);
	IvImage *r = iv_image_copy(img);
	iv_bg_apply(r, m, IV_BG_REPLACE, 0, 0x3F74F2FFu);
	snprintf(fn, sizeof(fn), "bg-%s-replaced.png", name);
	save_png(fn, r);
	iv_image_free(r);
	free(m);
}

static void test_background(void)
{
	const int w = 480, h = 360;
	uint8_t *truth = calloc(w * h, 1);

	/* Plain background with a little noise. */
	IvImage *a = iv_image_new(w, h);
	rng_state = 5;
	for (int i = 0; i < w * h; i++) {
		double n = (rndf() - 0.5) * 6;
		a->px[i * 4 + 0] = clamp8(230 + n);
		a->px[i * 4 + 1] = clamp8(232 + n);
		a->px[i * 4 + 2] = clamp8(228 + n);
		a->px[i * 4 + 3] = 255;
	}
	draw_subject(a, truth);
	bg_case("plain", a, truth, 0.95);
	iv_image_free(a);

	/* Gradient background (studio sweep). */
	memset(truth, 0, w * h);
	IvImage *b = iv_image_new(w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *p = PX(b, x, y);
			p[0] = clamp8(60 + 150.0 * y / h + 20.0 * x / w);
			p[1] = clamp8(90 + 120.0 * y / h);
			p[2] = clamp8(120 + 90.0 * y / h);
			p[3] = 255;
		}
	draw_subject(b, truth);
	bg_case("gradient", b, truth, 0.93);
	iv_image_free(b);

	/* Textured background (foliage-like noise, green). */
	memset(truth, 0, w * h);
	IvImage *c = iv_image_new(w, h);
	for (int i = 0; i < w * h; i++) {
		c->px[i * 4 + 0] = 70;
		c->px[i * 4 + 1] = 130;
		c->px[i * 4 + 2] = 60;
		c->px[i * 4 + 3] = 255;
	}
	add_texture(c, 30, 3.0, 9);
	draw_subject(c, truth);
	bg_case("textured", c, truth, 0.88);
	iv_image_free(c);

	/* No subject at all / pure noise: no clear background. */
	uint8_t *m = malloc(w * h);
	IvImage *u = iv_image_new(w, h);
	for (int i = 0; i < w * h; i++) {
		u->px[i * 4] = u->px[i * 4 + 1] = u->px[i * 4 + 2] = 128;
		u->px[i * 4 + 3] = 255;
	}
	int rc = iv_bg_estimate(u, m, NULL, NULL);
	CHECK(rc == 0, "uniform image: no clear background (rc %d)", rc);
	rng_state = 77;
	for (int i = 0; i < w * h * 4; i++)
		if ((i & 3) != 3)
			u->px[i] = (uint8_t)(rnd() & 0xFF);
	rc = iv_bg_estimate(u, m, NULL, NULL);
	CHECK(rc == 0, "random noise: no clear background (rc %d)", rc);
	/* A photo full of detail without a separable subject. */
	IvImage *ph = gen_photo(w, h, 81);
	add_texture(ph, 40, 2.0, 82);
	rc = iv_bg_estimate(ph, m, NULL, NULL);
	info("busy photo without subject: rc %d", rc);
	iv_image_free(ph);
	iv_image_free(u);

	/* Real picture: the photo area of a screenshot. */
	const char *av = getenv("IVPROC_AVATAR");
	if (av != NULL) {
		IvImage *shot = tp_read_png(av);
		if (shot == NULL) {
			info("IVPROC_AVATAR given but cannot be read (libpng missing?) - skipped");
		} else {
			int rx = 600, ry = 284, rw = 762, rh = 752;
			const char *rect = getenv("IVPROC_AVATAR_RECT");
			if (rect)
				sscanf(rect, "%d,%d,%d,%d", &rx, &ry, &rw, &rh);
			IvImage *pic = iv_crop(shot, rx, ry, rw, rh);
			uint8_t *pm = malloc((size_t)pic->w * pic->h);
			double t0 = now_ms();
			rc = iv_bg_estimate(pic, pm, NULL, NULL);
			double t1 = now_ms();
			long nbg = 0;
			for (int i = 0; i < pic->w * pic->h; i++)
				nbg += pm[i] >= 128;
			/* Ground truth is not known; sanity: the four corners are
			 * background, the centre (the face) is not. */
			int cx = pic->w / 2, cy = pic->h / 2;
			info("avatar %dx%d: rc %d, background %.1f %%, %.0f ms", pic->w, pic->h, rc,
			     100.0 * nbg / (pic->w * pic->h), t1 - t0);
			CHECK(rc == 1, "avatar: clear background found");
			CHECK(pm[5] > 200 && pm[(size_t)5 * pic->w + pic->w - 6] > 200, "avatar: top corners background");
			CHECK(pm[(size_t)cy * pic->w + cx] < 50, "avatar: centre is subject");
			save_png("bg-avatar-input.png", pic);
			save_mask("bg-avatar-mask.png", pm, pic->w, pic->h);
			IvImage *r = iv_image_copy(pic);
			iv_bg_apply(r, pm, IV_BG_REPLACE, 0, 0xEEF2FAFFu);
			save_png("bg-avatar-replaced.png", r);
			iv_image_free(r);
			r = iv_image_copy(pic);
			iv_bg_apply(r, pm, IV_BG_BLUR, 60, 0);
			save_png("bg-avatar-blurred.png", r);
			iv_image_free(r);
			free(pm);
			iv_image_free(pic);
			iv_image_free(shot);
		}
	}

	/* Operations. */
	IvImage *op = iv_image_new(100, 60);
	uint8_t *om = malloc(100 * 60);
	for (int y = 0; y < 60; y++)
		for (int x = 0; x < 100; x++) {
			uint8_t *p = PX(op, x, y);
			int subj = x >= 40 && x < 60;
			p[0] = subj ? 250 : 0;
			p[1] = 0;
			p[2] = subj ? 0 : 250;
			p[3] = 255;
			om[y * 100 + x] = subj ? 0 : 255;
		}
	IvImage *r = iv_image_copy(op);
	iv_bg_apply(r, om, IV_BG_BLUR, 100, 0);
	CHECK(PX(r, 38, 30)[0] < 6 && PX(r, 38, 30)[2] > 244, "blur does not bleed the subject (%d,%d,%d)",
	      PX(r, 38, 30)[0], PX(r, 38, 30)[1], PX(r, 38, 30)[2]);
	CHECK(PX(r, 50, 30)[0] == 250, "blur leaves the subject");
	iv_image_free(r);
	r = iv_image_copy(op);
	iv_bg_apply(r, om, IV_BG_REMOVE, 0, 0);
	CHECK(PX(r, 10, 10)[3] == 0 && PX(r, 50, 10)[3] == 255, "remove");
	iv_image_free(r);
	r = iv_image_copy(op);
	iv_bg_apply(r, om, IV_BG_REPLACE, 0, 0x11223344u);
	uint8_t *q = PX(r, 10, 10);
	CHECK(q[0] == 0x11 && q[1] == 0x22 && q[2] == 0x33 && q[3] == 0x44 && PX(r, 50, 10)[0] == 250,
	      "replace (%02x%02x%02x%02x)", q[0], q[1], q[2], q[3]);
	iv_image_free(r);
	iv_image_free(op);
	free(om);
	free(m);
	free(truth);
}

/* ------------------------------------------------------------------------
 * EXIF blobs
 * --------------------------------------------------------------------- */

typedef struct {
	uint8_t *d;
	size_t n, cap;
	int le;
} Buf;

static void b_put(Buf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2 + 64;
		b->d = realloc(b->d, b->cap);
	}
	memcpy(b->d + b->n, p, n);
	b->n += n;
}

static void b_u8(Buf *b, unsigned v)
{
	uint8_t c = (uint8_t)v;
	b_put(b, &c, 1);
}

static void b_u16(Buf *b, unsigned v)
{
	if (b->le) {
		b_u8(b, v & 0xFF);
		b_u8(b, v >> 8);
	} else {
		b_u8(b, v >> 8);
		b_u8(b, v & 0xFF);
	}
}

static void b_u32(Buf *b, uint32_t v)
{
	if (b->le) {
		b_u16(b, v & 0xFFFF);
		b_u16(b, v >> 16);
	} else {
		b_u16(b, v >> 16);
		b_u16(b, v & 0xFFFF);
	}
}

/* An IFD under construction: entries with inline or deferred data. */
typedef struct {
	unsigned tag, type;
	uint32_t count;
	uint8_t data[64];
	size_t len;
	int ptr_to;	/* >= 0: value is the offset of IFD #ptr_to */
} Ent;

static Ent e_ascii(unsigned tag, const char *s)
{
	Ent e = { tag, 2, (uint32_t)strlen(s) + 1, { 0 }, strlen(s) + 1, -1 };
	memcpy(e.data, s, e.len);
	return e;
}

static Ent e_short(unsigned tag, unsigned v, int le)
{
	Ent e = { tag, 3, 1, { 0 }, 2, -1 };
	e.data[0] = (uint8_t)(le ? v : v >> 8);
	e.data[1] = (uint8_t)(le ? v >> 8 : v);
	return e;
}

static void put32raw(uint8_t *p, uint32_t v, int le)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(le ? v >> (8 * i) : v >> (8 * (3 - i)));
}

static Ent e_long(unsigned tag, uint32_t v, int le)
{
	Ent e = { tag, 4, 1, { 0 }, 4, -1 };
	put32raw(e.data, v, le);
	return e;
}

static Ent e_rats(unsigned tag, int type, const uint32_t *nd, int n, int le)
{
	Ent e = { tag, (unsigned)type, (uint32_t)n, { 0 }, (size_t)n * 8, -1 };
	for (int i = 0; i < 2 * n; i++)
		put32raw(e.data + 4 * i, nd[i], le);
	return e;
}

static Ent e_ptr(unsigned tag, int ifd)
{
	Ent e = { tag, 4, 1, { 0 }, 4, ifd };
	return e;
}

/* Serialise up to 4 IFDs (0 = IFD0) into a TIFF block. */
static Buf build_tiff(int le, Ent *ifds[], const int *counts, int nifd)
{
	Buf b = { NULL, 0, 0, le };
	b_put(&b, le ? "II" : "MM", 2);
	b_u16(&b, 42);
	b_u32(&b, 8);
	uint32_t ifd_off[4];
	/* Layout: IFDs one after the other, each followed by its data. */
	size_t pos = 8;
	for (int i = 0; i < nifd; i++) {
		ifd_off[i] = (uint32_t)pos;
		size_t dsz = 0;
		for (int k = 0; k < counts[i]; k++)
			if (ifds[i][k].len > 4)
				dsz += (ifds[i][k].len + 1) & ~(size_t)1;
		pos += 2 + 12 * counts[i] + 4 + dsz;
	}
	for (int i = 0; i < nifd; i++) {
		b_u16(&b, (unsigned)counts[i]);
		size_t data = b.n + 12 * counts[i] + 4;
		size_t dpos = data;
		for (int k = 0; k < counts[i]; k++) {
			Ent *e = &ifds[i][k];
			b_u16(&b, e->tag);
			b_u16(&b, e->type);
			b_u32(&b, e->count);
			if (e->ptr_to >= 0) {
				b_u32(&b, ifd_off[e->ptr_to]);
			} else if (e->len <= 4) {
				uint8_t v[4] = { 0, 0, 0, 0 };
				memcpy(v, e->data, e->len);
				b_put(&b, v, 4);
			} else {
				b_u32(&b, (uint32_t)dpos);
				dpos += (e->len + 1) & ~(size_t)1;
			}
		}
		b_u32(&b, 0);	/* next IFD */
		for (int k = 0; k < counts[i]; k++) {
			Ent *e = &ifds[i][k];
			if (e->len > 4 && e->ptr_to < 0) {
				b_put(&b, e->data, e->len);
				if (e->len & 1)
					b_u8(&b, 0);
			}
		}
	}
	return b;
}

static Buf sample_tiff(int le)
{
	static Ent ifd0[9], exif[10], gps[6];
	int n0 = 0, ne = 0, ng = 0;
	ifd0[n0++] = e_ascii(0x010F, "LikeCam");
	ifd0[n0++] = e_ascii(0x0110, "Model One");
	ifd0[n0++] = e_short(0x0112, 6, le);
	const uint32_t r300[] = { 300, 1 };
	ifd0[n0++] = e_rats(0x011A, 5, r300, 1, le);
	ifd0[n0++] = e_rats(0x011B, 5, r300, 1, le);
	ifd0[n0++] = e_short(0x0128, 2, le);
	ifd0[n0++] = e_ascii(0x0132, "2026:11:03 10:05:39");
	ifd0[n0++] = e_ptr(0x8769, 1);
	ifd0[n0++] = e_ptr(0x8825, 2);
	const uint32_t et[] = { 1, 250 }, fn[] = { 28, 10 }, fl[] = { 50, 1 }, eb[] = { (uint32_t)-1, 3 };
	exif[ne++] = e_rats(0x829A, 5, et, 1, le);
	exif[ne++] = e_rats(0x829D, 5, fn, 1, le);
	exif[ne++] = e_short(0x8827, 400, le);
	exif[ne++] = e_ascii(0x9003, "2026:11:01 09:00:00");
	exif[ne++] = e_rats(0x9204, 10, eb, 1, le);
	exif[ne++] = e_short(0x9209, 0x19, le);
	exif[ne++] = e_rats(0x920A, 5, fl, 1, le);
	exif[ne++] = e_short(0xA001, 1, le);
	exif[ne++] = e_long(0xA002, 4000, le);
	exif[ne++] = e_ascii(0xA434, "Prime 50mm F2.8");
	const uint32_t lat[] = { 48, 1, 51, 1, 2960, 100 }, lon[] = { 2, 1, 17, 1, 402, 10 },
		       alt[] = { 355, 10 };
	gps[ng++] = e_ascii(0x0001, "N");
	gps[ng++] = e_rats(0x0002, 5, lat, 3, le);
	gps[ng++] = e_ascii(0x0003, "W");
	gps[ng++] = e_rats(0x0004, 5, lon, 3, le);
	Ent ar = { 0x0005, 1, 1, { 1 }, 1, -1 };
	gps[ng++] = ar;
	gps[ng++] = e_rats(0x0006, 5, alt, 1, le);
	Ent *ifds[3] = { ifd0, exif, gps };
	int counts[3] = { n0, ne, ng };
	return build_tiff(le, ifds, counts, 3);
}

static void check_sample(const IvExif *x, const char *what)
{
	CHECK(strcmp(x->make, "LikeCam") == 0 && strcmp(x->model, "Model One") == 0,
	      "%s: make/model '%s' '%s'", what, x->make, x->model);
	CHECK(x->orientation == 6, "%s: orientation %d", what, x->orientation);
	CHECK(fabs(x->x_dpi - 300) < 1e-9 && fabs(x->y_dpi - 300) < 1e-9, "%s: dpi %.1f", what, x->x_dpi);
	CHECK(strcmp(x->datetime, "2026:11:03 10:05:39") == 0, "%s: datetime", what);
	CHECK(strcmp(x->datetime_original, "2026:11:01 09:00:00") == 0, "%s: datetime original", what);
	CHECK(fabs(x->exposure_time - 1.0 / 250) < 1e-12, "%s: exposure %.6f", what, x->exposure_time);
	CHECK(fabs(x->fnumber - 2.8) < 1e-9, "%s: f-number %.2f", what, x->fnumber);
	CHECK(x->iso == 400, "%s: iso %d", what, x->iso);
	CHECK(x->flash == 0x19, "%s: flash %d", what, x->flash);
	CHECK(fabs(x->focal_length - 50) < 1e-9, "%s: focal %.1f", what, x->focal_length);
	CHECK(x->color_space == 1, "%s: color space", what);
	CHECK(strcmp(x->lens, "Prime 50mm F2.8") == 0, "%s: lens '%s'", what, x->lens);
	CHECK(fabs(x->exposure_bias + 1.0 / 3) < 1e-9, "%s: bias %.3f", what, x->exposure_bias);
	CHECK(x->has_gps && fabs(x->gps_lat - (48 + 51 / 60.0 + 29.6 / 3600)) < 1e-9 &&
	      fabs(x->gps_lon + (2 + 17 / 60.0 + 40.2 / 3600)) < 1e-9,
	      "%s: gps %.6f %.6f", what, x->gps_lat, x->gps_lon);
	CHECK(x->has_alt && fabs(x->gps_alt + 35.5) < 1e-9, "%s: altitude %.1f", what, x->gps_alt);
	CHECK(x->has_exif, "%s: has_exif", what);
}

/* A JPEG header with optional APP1 and a fake scan. */
static Buf build_jpeg(const Buf *tiff, int w, int h, int jfif_dpi)
{
	Buf j = { NULL, 0, 0, 0 };
	b_u8(&j, 0xFF);
	b_u8(&j, 0xD8);
	if (jfif_dpi) {
		const uint8_t app0[] = { 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 2, 1,
					 (uint8_t)(jfif_dpi >> 8), (uint8_t)jfif_dpi,
					 (uint8_t)(jfif_dpi >> 8), (uint8_t)jfif_dpi, 0, 0 };
		b_put(&j, app0, sizeof(app0));
	}
	if (tiff) {
		size_t len = tiff->n + 8;
		b_u8(&j, 0xFF);
		b_u8(&j, 0xE1);
		b_u8(&j, (unsigned)(len >> 8));
		b_u8(&j, (unsigned)(len & 0xFF));
		b_put(&j, "Exif\0\0", 6);
		b_put(&j, tiff->d, tiff->n);
	}
	/* DQT stub, SOF0 (8-bit, 3 components), SOS and some entropy data. */
	const uint8_t dqt[] = { 0xFF, 0xDB, 0, 4, 0, 1 };
	b_put(&j, dqt, sizeof(dqt));
	const uint8_t sof[] = { 0xFF, 0xC0, 0, 17, 8, (uint8_t)(h >> 8), (uint8_t)h,
				(uint8_t)(w >> 8), (uint8_t)w, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
	b_put(&j, sof, sizeof(sof));
	const uint8_t sos[] = { 0xFF, 0xDA, 0, 12, 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0 };
	b_put(&j, sos, sizeof(sos));
	for (int i = 0; i < 300; i++)
		b_u8(&j, (unsigned)((i * 37) & 0xFE));	/* never 0xFF */
	b_u8(&j, 0xFF);
	b_u8(&j, 0xD9);
	return j;
}

static void png_chunk(Buf *p, const char *type, const uint8_t *d, uint32_t n)
{
	uint8_t hdr[8] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
	memcpy(hdr + 4, type, 4);
	b_put(p, hdr, 8);
	if (n)
		b_put(p, d, n);
	uint32_t crc = tp_crc32((const uint8_t *)type, 4, 0);
	crc = tp_crc32(d, n, crc);
	uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
	b_put(p, c, 4);
}

static Buf build_png(const Buf *tiff, int phys_dpi)
{
	Buf p = { NULL, 0, 0, 0 };
	b_put(&p, "\x89PNG\r\n\x1a\n", 8);
	const uint8_t ihdr[13] = { 0, 0, 0x01, 0x40, 0, 0, 0, 0xF0, 8, 6, 0, 0, 0 };
	png_chunk(&p, "IHDR", ihdr, 13);
	if (phys_dpi) {
		uint32_t ppm = (uint32_t)lround(phys_dpi / 0.0254);
		uint8_t ph[9] = { (uint8_t)(ppm >> 24), (uint8_t)(ppm >> 16), (uint8_t)(ppm >> 8),
				  (uint8_t)ppm, (uint8_t)(ppm >> 24), (uint8_t)(ppm >> 16),
				  (uint8_t)(ppm >> 8), (uint8_t)ppm, 1 };
		png_chunk(&p, "pHYs", ph, 9);
	}
	const uint8_t idat[2] = { 0x78, 0x01 };
	png_chunk(&p, "IDAT", idat, 2);
	if (tiff)
		png_chunk(&p, "eXIf", tiff->d, (uint32_t)tiff->n);
	png_chunk(&p, "IEND", NULL, 0);
	return p;
}

static void le32put(Buf *b, uint32_t v)
{
	uint8_t c[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	b_put(b, c, 4);
}

static Buf build_webp(const Buf *tiff, int with_prefix)
{
	Buf r = { NULL, 0, 0, 1 };
	b_put(&r, "RIFF", 4);
	le32put(&r, 0);	/* patched below */
	b_put(&r, "WEBP", 4);
	b_put(&r, "VP8X", 4);
	le32put(&r, 10);
	uint8_t x[10] = { 0x18, 0, 0, 0, 0x3F, 0x01, 0, 0xEF, 0, 0 };	/* alpha + EXIF; 320x240 */
	b_put(&r, x, 10);
	b_put(&r, "VP8L", 4);
	le32put(&r, 5);
	uint8_t l[6] = { 0x2F, 0x3F, 0xC0, 0x3B, 0x10, 0 };
	b_put(&r, l, 6);	/* 5 + pad */
	b_put(&r, "EXIF", 4);
	uint32_t n = (uint32_t)tiff->n + (with_prefix ? 6 : 0);
	le32put(&r, n);
	if (with_prefix)
		b_put(&r, "Exif\0\0", 6);
	b_put(&r, tiff->d, tiff->n);
	if (n & 1)
		b_u8(&r, 0);
	uint32_t riff = (uint32_t)r.n - 8;
	r.d[4] = (uint8_t)riff;
	r.d[5] = (uint8_t)(riff >> 8);
	r.d[6] = (uint8_t)(riff >> 16);
	r.d[7] = (uint8_t)(riff >> 24);
	return r;
}

static int write_file(const char *path, const Buf *b)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fwrite(b->d, 1, b->n, f);
	return fclose(f);
}

static char *tmp_path(const char *name)
{
	static char buf[4][1024];
	static int k;
	const char *dir = g_outdir ? g_outdir : ".";
	char *p = buf[k++ & 3];
	snprintf(p, 1024, "%s/ivtest-%s", dir, name);
	return p;
}

static void test_exif(void)
{
	for (int le = 0; le <= 1; le++) {
		Buf t = sample_tiff(le);
		IvExif x;
		int rc = iv_exif_parse_tiff(t.d, t.n, &x);
		CHECK(rc == 1, "tiff %s parsed", le ? "LE" : "BE");
		check_sample(&x, le ? "tiff LE" : "tiff BE");

		Buf j = build_jpeg(&t, 1125, 1038, 0);
		rc = iv_exif_read_mem(j.d, j.n, &x);
		CHECK(rc == 1 && x.width == 1125 && x.height == 1038 && x.bits == 24 &&
		      strcmp(x.format, "JPEG") == 0 && strcmp(x.color_type, "RGB") == 0,
		      "jpeg: rc %d %dx%d bits %d", rc, x.width, x.height, x.bits);
		check_sample(&x, "jpeg");
		/* From a file through the block cache. */
		char *jp = tmp_path(le ? "le.jpg" : "be.jpg");
		write_file(jp, &j);
		rc = iv_exif_read_file(jp, &x);
		CHECK(rc == 1, "jpeg file read");
		check_sample(&x, "jpeg file");

		Buf p = build_png(&t, 0);
		rc = iv_exif_read_mem(p.d, p.n, &x);
		CHECK(rc == 1 && x.width == 320 && x.height == 240 && x.bits == 32 &&
		      strcmp(x.color_type, "RGBA") == 0, "png eXIf: %dx%d bits %d", x.width, x.height, x.bits);
		check_sample(&x, "png");

		for (int pre = 0; pre <= 1; pre++) {
			Buf wb = build_webp(&t, pre);
			rc = iv_exif_read_mem(wb.d, wb.n, &x);
			CHECK(rc == 1 && x.width == 320 && x.height == 240 && x.channels == 4,
			      "webp EXIF (prefix %d): %dx%d ch %d", pre, x.width, x.height, x.channels);
			check_sample(&x, "webp");
			free(wb.d);
		}
		/* A TIFF file itself. */
		char *tp = tmp_path(le ? "le.tif" : "be.tif");
		write_file(tp, &t);
		rc = iv_exif_read_file(tp, &x);
		CHECK(rc == 1 && strcmp(x.format, "TIFF") == 0, "tiff file");
		check_sample(&x, "tiff file");
		remove(jp);
		remove(tp);
		free(t.d);
		free(j.d);
		free(p.d);
	}
	/* Fallbacks without EXIF. */
	IvExif x;
	Buf j = build_jpeg(NULL, 640, 480, 96);
	int rc = iv_exif_read_mem(j.d, j.n, &x);
	CHECK(rc == 0 && fabs(x.x_dpi - 96) < 1e-9 && x.orientation == 1 && x.width == 640,
	      "jpeg JFIF dpi fallback (rc %d, dpi %.1f)", rc, x.x_dpi);
	free(j.d);
	Buf p = build_png(NULL, 150);
	rc = iv_exif_read_mem(p.d, p.n, &x);
	CHECK(rc == 0 && fabs(x.x_dpi - 150) < 0.05, "png pHYs fallback (%.2f)", x.x_dpi);
	free(p.d);
	uint8_t gif[] = { 'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xF0, 0, 0xF7, 0, 0 };
	rc = iv_exif_read_mem(gif, sizeof(gif), &x);
	CHECK(rc == 0 && x.width == 320 && x.height == 240 && x.bits == 8, "gif header");
	uint8_t junk[] = "hello world, not an image";
	CHECK(iv_exif_read_mem(junk, sizeof(junk), &x) == -1, "unknown format = -1");
	CHECK(iv_exif_read_file("/nonexistent/file.jpg", &x) == -1, "missing file = -1");

	/* Texts. */
	char buf[64];
	iv_exif_format_dms(48.858222, 1, buf, sizeof(buf));
	CHECK(strcmp(buf, "48° 51′ 29.6″ N") == 0, "dms '%s'", buf);
	iv_exif_format_dms(-2.2945, 0, buf, sizeof(buf));
	CHECK(strstr(buf, "W") != NULL, "dms west '%s'", buf);
	iv_exif_format_exposure(1.0 / 250, buf, sizeof(buf));
	CHECK(strcmp(buf, "1/250 s") == 0, "exposure '%s'", buf);
	iv_exif_format_exposure(2.5, buf, sizeof(buf));
	CHECK(strcmp(buf, "2.5 s") == 0, "exposure '%s'", buf);
	CHECK(strcmp(iv_exif_flash_string(0x19), "Flash fired, auto mode") == 0, "flash text");
	CHECK(*iv_exif_orientation_string(6) && *iv_exif_exposure_program_string(3) &&
	      *iv_exif_metering_mode_string(5) && strcmp(iv_exif_color_space_string(1), "sRGB") == 0,
	      "texts");
}

/* ------------------------------------------------------------------------
 * jpegmeta
 * --------------------------------------------------------------------- */

/* Whether `text' occurs anywhere in the n bytes at d. */
static int has_bytes(const uint8_t *d, size_t n, const char *text)
{
	size_t k = strlen(text);

	for (size_t i = 0; k <= n && i <= n - k; i++)
		if (memcmp(d + i, text, k) == 0)
			return 1;
	return 0;
}

static void test_jpegmeta(void)
{
	Buf t = sample_tiff(1);
	Buf orig = build_jpeg(&t, 4000, 3000, 72);
	char *op = tmp_path("orig.jpg");
	char *np = tmp_path("new.jpg");
	CHECK(write_file(op, &orig) == 0, "scratch directory writable (%s)", op);
	uint8_t *app1 = NULL;
	size_t len = 0;
	int rc = iv_jpeg_get_app1(op, &app1, &len);
	CHECK(rc == 0 && len == t.n + 6 && memcmp(app1, "Exif\0\0", 6) == 0 &&
	      memcmp(app1 + 6, t.d, t.n) == 0, "get_app1 returns the payload");

	/* A freshly written JPEG (no EXIF, different size). */
	Buf fresh = build_jpeg(NULL, 3000, 4000, 72);
	write_file(np, &fresh);
	rc = iv_jpeg_write_with_app1(np, app1, len, 1);
	CHECK(rc == 0, "write_with_app1");
	IvExif x;
	rc = iv_exif_read_file(np, &x);
	CHECK(rc == 1 && x.orientation == 1 && strcmp(x.make, "LikeCam") == 0 && x.width == 3000,
	      "re-inserted EXIF readable, orientation reset (rc %d, o %d)", rc, x.orientation);
	uint8_t *again = NULL;
	size_t alen = 0;
	CHECK(iv_jpeg_get_app1(np, &again, &alen) == 0 && alen == len, "APP1 present once");
	/* The segment order: SOI, APP0, APP1. */
	size_t n = 0;
	FILE *f = fopen(np, "rb");
	uint8_t head[24];
	if (f != NULL) {
		n = fread(head, 1, sizeof(head), f);
		fclose(f);
	}
	CHECK(n == 24 && head[2] == 0xFF && head[3] == 0xE0 && head[20] == 0xFF && head[21] == 0xE1,
	      "APP1 directly after JFIF APP0");
	/* PixelXDimension rewritten to the new width. */
	int found = 0;
	for (size_t i = 0; i + 12 < alen; i++)
		if (again[i] == 0x02 && again[i + 1] == 0xA0 && again[i + 2] == 4) {
			uint32_t v = again[i + 8] | (again[i + 9] << 8) | (again[i + 10] << 16) |
				     ((uint32_t)again[i + 11] << 24);
			found = v == 3000;
		}
	CHECK(found, "PixelXDimension patched to 3000");
	/* Writing twice replaces, never duplicates. */
	rc = iv_jpeg_write_with_app1(np, app1, len, 0);
	long sz2 = -1;
	f = fopen(np, "rb");
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		sz2 = ftell(f);
		fclose(f);
	}
	CHECK(rc == 0 && (size_t)sz2 == fresh.n + len + 4, "second write replaces (size %ld)", sz2);
	rc = iv_exif_read_file(np, &x);
	CHECK(x.orientation == 6, "reset_orientation 0 keeps orientation");
	/* Without APP0 in the target. */
	Buf noapp0 = build_jpeg(NULL, 10, 10, 0);
	write_file(np, &noapp0);
	CHECK(iv_jpeg_write_with_app1(np, app1, len, 1) == 0 && iv_exif_read_file(np, &x) == 1,
	      "insert without APP0");
	/* Refusals. */
	CHECK(iv_jpeg_write_with_app1(np, (const uint8_t *)"garbage-not-exif", 16, 1) == -1,
	      "non-EXIF payload refused");
	Buf notjpeg = build_png(NULL, 0);
	write_file(np, &notjpeg);
	CHECK(iv_jpeg_write_with_app1(np, app1, len, 1) == -1, "non-JPEG target refused");
	free(again);
	again = NULL;
	CHECK(iv_jpeg_get_app1(np, &again, &alen) == -1 && again == NULL, "get_app1 on a PNG = -1");

	/* A source with a thumbnail (IFD1 with a JPEG thumbnail after it):
	 * the copy keeps no trace of the old pixels, neither the link nor
	 * the bytes. */
	{
		static const char old_pixels[] = "\xFF\xD8LIKEOS-OLD-PIXELS\xFF\xD9";
		Buf th = sample_tiff(1);
		uint32_t ifd1, thumb;
		uint8_t *got = NULL;
		size_t glen = 0;
		IvExif tx;

		if (th.n & 1)
			b_u8(&th, 0);
		ifd1 = (uint32_t)th.n;
		thumb = ifd1 + 2 + 2 * 12 + 4;
		b_u16(&th, 2);
		b_u16(&th, 0x0201);
		b_u16(&th, 4);
		b_u32(&th, 1);
		b_u32(&th, thumb);
		b_u16(&th, 0x0202);
		b_u16(&th, 4);
		b_u32(&th, 1);
		b_u32(&th, sizeof old_pixels - 1);
		b_u32(&th, 0);
		b_put(&th, old_pixels, sizeof old_pixels - 1);
		/* IFD0 (9 entries at 8) now links to it. */
		put32raw(th.d + 8 + 2 + 9 * 12, ifd1, 1);
		Buf withthumb = build_jpeg(&th, 4000, 3000, 72);
		write_file(op, &withthumb);
		free(app1);
		app1 = NULL;
		CHECK(iv_jpeg_get_app1(op, &app1, &len) == 0 &&
			      has_bytes(app1, len, "LIKEOS-OLD-PIXELS"),
		      "source carries the thumbnail");
		write_file(np, &fresh);
		CHECK(iv_jpeg_write_with_app1(np, app1, len, 1) == 0 &&
			      iv_jpeg_get_app1(np, &got, &glen) == 0,
		      "thumbnail source copied");
		CHECK(got && !has_bytes(got, glen, "LIKEOS-OLD-PIXELS"),
		      "old thumbnail bytes wiped");
		CHECK(got && glen > 6 + 8 + 2 + 9 * 12 + 4 &&
			      got[6 + 8 + 2 + 9 * 12] == 0 && got[6 + 8 + 2 + 9 * 12 + 1] == 0 &&
			      got[6 + 8 + 2 + 9 * 12 + 2] == 0 && got[6 + 8 + 2 + 9 * 12 + 3] == 0,
		      "IFD1 unlinked");
		CHECK(iv_exif_read_file(np, &tx) == 1 && strcmp(tx.make, "LikeCam") == 0,
		      "camera data kept beside the wiped thumbnail");
		free(got);
		free(th.d);
		free(withthumb.d);
	}
	free(app1);
	remove(op);
	remove(np);
	free(t.d);
	free(orig.d);
	free(fresh.d);
	free(noapp0.d);
	free(notjpeg.d);
}

/* ------------------------------------------------------------------------
 * fuzz
 * --------------------------------------------------------------------- */

static void mutate(uint8_t *d, size_t *n, size_t cap)
{
	int ops = 1 + (int)(rnd() % 8);
	for (int k = 0; k < ops && *n > 0; k++) {
		size_t i = rnd() % *n;
		switch (rnd() % 7) {
		case 0:
			d[i] ^= (uint8_t)(1u << (rnd() % 8));
			break;
		case 1:
			d[i] = (uint8_t)rnd();
			break;
		case 2:
			d[i] = (rnd() & 1) ? 0xFF : 0x00;
			break;
		case 3:	/* truncate */
			*n = i + 1;
			break;
		case 4:	/* large integer */
			if (i + 4 <= *n)
				memset(d + i, 0xFF, 4);
			break;
		case 5:	/* duplicate a range */
			if (*n + 16 <= cap) {
				memmove(d + i + 16, d + i, *n - i);
				*n += 16;
			}
			break;
		default:	/* small number */
			d[i] = (uint8_t)(rnd() % 4);
			break;
		}
	}
}

static void test_fuzz(void)
{
	int iters = 20000;
	const char *e = getenv("IVPROC_FUZZ");
	if (e)
		iters = atoi(e);
	Buf seeds[8];
	int ns = 0;
	for (int le = 0; le <= 1; le++) {
		Buf t = sample_tiff(le);
		seeds[ns++] = build_jpeg(&t, 100, 100, 72);
		seeds[ns++] = build_png(&t, 72);
		seeds[ns++] = build_webp(&t, le);
		seeds[ns++] = t;
	}
	rng_state = 2026;
	long total = 0, parsed = 0;
	double t0 = now_ms();
	for (int s = 0; s < ns; s++) {
		size_t cap = seeds[s].n + 4096;
		uint8_t *buf = malloc(cap);
		for (int it = 0; it < iters; it++) {
			size_t n = seeds[s].n;
			memcpy(buf, seeds[s].d, n);
			mutate(buf, &n, cap);
			/* Exactly sized copy so ASan catches any overread. */
			uint8_t *exact = malloc(n ? n : 1);
			memcpy(exact, buf, n);
			IvExif x;
			int rc = iv_exif_read_mem(exact, n, &x);
			parsed += rc == 1;
			iv_exif_parse_tiff(exact, n, &x);
			total++;
			free(exact);
		}
		free(buf);
	}
	info("fuzz: %ld mutated inputs, %ld still parsed as EXIF, %.0f ms", total, parsed, now_ms() - t0);
	CHECK(total == (long)ns * iters, "fuzz loop completed");

	/* The JPEG re-insertion with mutated payloads and targets. */
	char *np = tmp_path("fuzz.jpg");
	Buf t = sample_tiff(1);
	Buf good = build_jpeg(NULL, 64, 64, 72);
	int runs = iters / 10 > 50 ? iters / 10 : 50;
	for (int it = 0; it < runs; it++) {
		size_t n = t.n + 6;
		uint8_t *app = malloc(n + 4096);
		memcpy(app, "Exif\0\0", 6);
		memcpy(app + 6, t.d, t.n);
		mutate(app + 6, &n, n + 4090);
		n = n < 6 ? 6 : n;
		uint8_t *exact = malloc(n);
		memcpy(exact, app, n);
		Buf tgt = { malloc(good.n + 4096), good.n, good.n + 4096, 0 };
		memcpy(tgt.d, good.d, good.n);
		if (it & 1)
			mutate(tgt.d, &tgt.n, tgt.cap);
		write_file(np, &tgt);
		iv_jpeg_write_with_app1(np, exact, n, it & 2);
		IvExif x;
		iv_exif_read_file(np, &x);
		uint8_t *g = NULL;
		size_t gl = 0;
		if (iv_jpeg_get_app1(np, &g, &gl) == 0)
			free(g);
		free(exact);
		free(app);
		free(tgt.d);
	}
	CHECK(1, "jpeg re-insertion fuzz completed (%d runs)", runs);

	/* The pixel functions on odd sizes and random parameters: tiny and
	 * thin images, masks that cover almost everything, extreme slider
	 * values.  Nothing may crash or read out of bounds. */
	int pruns = iters / 100 > 60 ? iters / 100 : 60;
	for (int it = 0; it < pruns; it++) {
		int w = 1 + (int)(rnd() % 48), h = 1 + (int)(rnd() % 48);
		if (it % 7 == 0)
			w = 1 + (int)(rnd() % 3);
		IvImage *img = iv_image_new(w, h);
		for (int i = 0; i < w * h * 4; i++)
			img->px[i] = (uint8_t)rnd();
		uint8_t *m = malloc((size_t)w * h);
		int dens = (int)(rnd() % 100);
		for (int i = 0; i < w * h; i++)
			m[i] = (int)(rnd() % 100) < dens ? 255 : 0;
		IvImage *c = iv_image_copy(img);
		iv_inpaint(c, m, NULL, NULL);
		iv_bg_estimate(img, m, NULL, NULL);
		iv_bg_apply(c, m, (IvBgOp)(rnd() % 3), rnd() % 101, rnd());
		IvAdjust a;
		double *f = (double *)&a;
		for (size_t k = 0; k < sizeof(a) / sizeof(double); k++)
			f[k] = (double)((int)(rnd() % 201) - 100);
		iv_adjust_apply_ex(c, &a, 0.1 + rndf() * 2);
		iv_style_apply(c, (IvStyle)(rnd() % IV_STYLE_COUNT), rndf());
		IvImage *r = iv_rotate_free(c, rndf() * 720 - 360, (int)(rnd() % 3));
		iv_image_free(r);
		r = iv_image_scale(c, 1 + (int)(rnd() % 70), 1 + (int)(rnd() % 70), (int)(rnd() % 3));
		iv_image_free(r);
		iv_gaussian_blur(c, rndf() * 30);
		iv_unsharp(c, rndf() * 5, rndf() * 3);
		iv_mask_dab(m, w, h, rndf() * w * 2 - w / 2, rndf() * h * 2 - h / 2, rndf() * 30,
			    (int)(rnd() % 256), rndf());
		iv_mask_feather(m, w, h, rndf() * 20);
		IvAdjust e;
		iv_auto_enhance(c, &e);
		iv_image_free(c);
		iv_image_free(img);
		free(m);
	}
	CHECK(1, "pixel function robustness loop completed (%d runs)", pruns);
	remove(np);
	free(t.d);
	free(good.d);
	for (int s = 0; s < ns; s++)
		free(seeds[s].d);
}

/* ------------------------------------------------------------------------
 * webp
 * --------------------------------------------------------------------- */

static void test_webp(void)
{
	IvImage *img = gen_photo(160, 100, 91);
	for (int y = 0; y < 100; y++)
		for (int x = 0; x < 40; x++)
			PX(img, x, y)[3] = (uint8_t)(x * 6);
	uint8_t *enc = NULL;
	size_t n = 0;
	CHECK(iv_webp_encode_mem(img, 101, &enc, &n) == 0 && n > 0, "lossless encode");
	int w = 0, h = 0, al = 0, an = 1;
	CHECK(iv_webp_probe(enc, n, &w, &h, &al, &an) == 0 && w == 160 && h == 100 && al && !an,
	      "probe %dx%d alpha %d anim %d", w, h, al, an);
	int has_alpha = 0;
	IvImage *d = iv_webp_decode_mem(enc, n, &has_alpha);
	/* Lossless keeps every visible pixel exactly (RGB under alpha 0 may
	 * be dropped by the encoder, which is fine). */
	int diff = 0;
	for (int y = 0; d && y < 100; y++)
		for (int x = 0; x < 160; x++) {
			const uint8_t *a = PX(img, x, y), *b = PX(d, x, y);
			if (a[3] != b[3])
				diff = 999;
			else if (a[3] > 0)
				for (int c = 0; c < 3; c++)
					if (abs(a[c] - b[c]) > diff)
						diff = abs(a[c] - b[c]);
		}
	CHECK(d && has_alpha && diff == 0, "lossless round trip exact (%d)", diff);
	free(enc);
	iv_image_free(d);

	CHECK(iv_webp_encode_mem(img, 90, &enc, &n) == 0, "lossy encode");
	d = iv_webp_decode_mem(enc, n, NULL);
	double se = 0;
	long cnt = 0;
	for (int y = 0; d && y < 100; y++)
		for (int x = 40; x < 160; x++)
			for (int c = 0; c < 3; c++) {
				double dd = PX(img, x, y)[c] - PX(d, x, y)[c];
				se += dd * dd;
				cnt++;
			}
	double psnr = cnt ? 10 * log10(255.0 * 255.0 / (se / cnt + 1e-9)) : 0;
	CHECK(d && psnr > 30, "lossy q90 PSNR %.1f dB", psnr);
	info("webp: lossless exact, lossy q90 %zu bytes PSNR %.1f dB", n, psnr);
	free(enc);
	iv_image_free(d);

	char *p = tmp_path("rt.webp");
	CHECK(iv_webp_save(img, p, 101) == 0, "save");
	d = iv_webp_load(p, &has_alpha);
	CHECK(d && d->w == 160 && has_alpha, "load");
	IvExif x;
	CHECK(iv_exif_read_file(p, &x) == 0 && x.width == 160 && x.height == 100 &&
	      strcmp(x.format, "WebP") == 0, "webp info without EXIF (%dx%d)", x.width, x.height);
	iv_image_free(d);
	/* Opaque image: no alpha in the file. */
	IvImage *op = gen_photo(64, 64, 92);
	CHECK(iv_webp_save(op, p, 80) == 0, "save opaque");
	d = iv_webp_load(p, &has_alpha);
	CHECK(d && !has_alpha, "opaque file has no alpha");
	iv_image_free(d);
	iv_image_free(op);
	remove(p);
	uint8_t bad[32] = "RIFF\x10\0\0\0WEBPVP8 garbage...";
	CHECK(iv_webp_decode_mem(bad, sizeof(bad), NULL) == NULL, "garbage rejected");
	CHECK(iv_webp_load("/nonexistent.webp", NULL) == NULL, "missing file");
	iv_image_free(img);
}

/* ------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

static const struct {
	const char *name;
	void (*fn)(void);
} suites[] = {
	{ "image", test_image },	   { "scale", test_scale },
	{ "adjust", test_adjust },	   { "styles", test_styles },
	{ "enhance", test_enhance },	   { "geometry", test_geometry },
	{ "blur", test_blur },		   { "mask", test_mask },
	{ "histogram", test_histogram },   { "inpaint", test_inpaint },
	{ "background", test_background }, { "exif", test_exif },
	{ "jpegmeta", test_jpegmeta },	   { "fuzz", test_fuzz },
	{ "webp", test_webp },
};

int main(int argc, char **argv)
{
	g_outdir = getenv("IVPROC_OUTDIR");
	int any = 0;
	for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		int run = argc < 2;
		for (int a = 1; a < argc; a++)
			if (strcmp(argv[a], "all") == 0 || strcmp(argv[a], suites[i].name) == 0)
				run = 1;
		if (!run)
			continue;
		any = 1;
		int before = g_fail;
		printf("[%s]\n", suites[i].name);
		fflush(stdout);
		suites[i].fn();
		printf("[%s] %s\n", suites[i].name, g_fail == before ? "ok" : "FAILED");
		fflush(stdout);
	}
	if (!any) {
		fprintf(stderr, "unknown suite\n");
		return 2;
	}
	printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
