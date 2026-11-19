// LikeOS -- LikeOS Image Viewer: internal helpers shared by the ivproc
// sources (not installed, not part of the public API).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IVPRIV_H
#define IVPRIV_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "ivproc.h"

/* ------------------------------------------------------------------------
 * Allocation (alloc.c)
 *
 * Every allocation of the library goes through these, so a failure is
 * counted for the thread that saw it (iv_alloc_failures(), ivproc.h).
 * <stdlib.h> comes first above: its own declarations are not renamed.
 * --------------------------------------------------------------------- */

void ivp_note_alloc_failure(void);
void *ivp_malloc_counted(size_t n);
void *ivp_calloc_counted(size_t n, size_t size);
void *ivp_realloc_counted(void *p, size_t n);
#define malloc(n) ivp_malloc_counted(n)
#define calloc(n, size) ivp_calloc_counted((n), (size))
#define realloc(p, n) ivp_realloc_counted((p), (n))

/* ------------------------------------------------------------------------
 * Small numeric helpers
 * --------------------------------------------------------------------- */

static inline float ivp_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline double ivp_clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline int ivp_clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Round a 0..1 float to a byte. */
static inline uint8_t ivp_to_u8(float v)
{
	if (!(v > 0.0f))	/* also catches NaN */
		return 0;
	if (v >= 1.0f)
		return 255;
	return (uint8_t)(v * 255.0f + 0.5f);
}

/* Round a 0..255 float to a byte. */
static inline uint8_t ivp_u8(float v)
{
	if (!(v > 0.0f))
		return 0;
	if (v >= 255.0f)
		return 255;
	return (uint8_t)(v + 0.5f);
}

static inline float ivp_smoothstep(float e0, float e1, float x)
{
	float t = (x - e0) / (e1 - e0);
	t = ivp_clampf(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

static inline float ivp_mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

/* White balance model shared by Adjust (warmth / tint sliders) and auto
 * enhance: gains on linear light, at slider +-100:
 *   warmth  red x (1 + W), blue x (1 - W), green x (1 + W / 10)
 *   tint    green x (1 - T), red and blue x (1 + T_RB)
 * then normalised so grey keeps its luminance. */
#define IVP_WB_WARM 0.45f
#define IVP_WB_TINT 0.35f
#define IVP_WB_TINT_RB 0.12f

/* Rec. 709 luma weights, used on gamma-encoded values (perceptual luma). */
#define IVP_LR 0.2126f
#define IVP_LG 0.7152f
#define IVP_LB 0.0722f

static inline float ivp_luma(float r, float g, float b)
{
	return IVP_LR * r + IVP_LG * g + IVP_LB * b;
}

static inline uint8_t *ivp_px(const IvImage *img, int x, int y)
{
	return img->px + (size_t)y * (size_t)img->stride + (size_t)x * 4;
}

/* Overflow-checked w * h * elem for allocations; 0 on overflow. */
size_t ivp_size3(size_t a, size_t b, size_t c);
void *ivp_alloc(size_t a, size_t b, size_t c);	/* malloc */
void *ivp_zalloc(size_t a, size_t b, size_t c);	/* calloc */

/* Deterministic integer hash (for grain and seeded sampling). */
static inline uint32_t ivp_hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static inline uint32_t ivp_hash2(int x, int y, uint32_t seed)
{
	return ivp_hash32((uint32_t)x * 0x9E3779B1U ^
			  ivp_hash32((uint32_t)y + seed * 0x85EBCA77U));
}

/* Uniform -1..1 from a hash. */
static inline float ivp_hash_unit(uint32_t h)
{
	return (float)(h & 0xFFFFFF) * (2.0f / 16777215.0f) - 1.0f;
}

/* ------------------------------------------------------------------------
 * Colour (color.c)
 * --------------------------------------------------------------------- */

float ivp_srgb_to_linear(float v);	/* 0..1 -> 0..1 */
float ivp_linear_to_srgb(float v);	/* 0..1 -> 0..1 */
/* lut[i] = linear value of sRGB byte i. */
void ivp_build_srgb_lut(float lut[256]);

/* CIE L*a*b* (D65) from an sRGB triplet; lut from ivp_build_srgb_lut. */
void ivp_rgb8_to_lab(const float lut[256], uint8_t r, uint8_t g, uint8_t b,
		     float lab[3]);
/* Hue in degrees 0..360 of gamma-encoded RGB 0..1 and its HSV-style
 * saturation (max - min) / max. */
void ivp_hue_sat(float r, float g, float b, float *hue, float *sat);
/* Scale the chroma of (r,g,b) around its luma so every channel is within
 * 0..1 (keeps the hue; used instead of hard clipping). */
void ivp_gamut_fit(float *r, float *g, float *b);

/* Monotone cubic (Fritsch-Carlson) curve through n points (x ascending,
 * 0..1), sampled into lut[0..size-1] at x = i / (size - 1). */
void ivp_curve_lut(const float *xs, const float *ys, int n, float *lut,
		   int size);

/* ------------------------------------------------------------------------
 * Blur (blur.c)
 * --------------------------------------------------------------------- */

enum { IVP_U8 = 0, IVP_U16 = 1, IVP_F32 = 2 };

/* Separable Gaussian over interleaved data of nc channels (row_stride in
 * BYTES), in place.  Small sigma: true kernel; large sigma: 3 box passes. */
void ivp_blur_generic(void *data, int type, int w, int h, int nc,
		      size_t row_stride, double sigma_x, double sigma_y);
/* Single float plane, tightly packed. */
void ivp_blur_plane(float *p, int w, int h, double sigma);
/* Exact box mean of radius r (window 2r+1, edges clamped), float plane. */
void ivp_box_plane(float *p, int w, int h, int r);

/* Self-guided filter coefficients: for a float plane I (w*h), radius r
 * and regularisation eps, computes a and b (w*h each) so that the
 * edge-preserving smooth result is a * I + b.  Returns 0 / -1 (memory). */
int ivp_guided_self(const float *I, int w, int h, int r, float eps, float *a,
		    float *b);
/* Guided filter of p with guide I (both w*h); result in q.  0 / -1. */
int ivp_guided(const float *I, const float *p, int w, int h, int r,
	       float eps, float *q);

/* Premultiplied 16-bit RGBA copy of an image (channel value c*a, alpha
 * a*255, both 0..65025), and back. */
uint16_t *ivp_premul16(const IvImage *img);
void ivp_unpremul16(const uint16_t *src, IvImage *img);

/* ------------------------------------------------------------------------
 * Downscaled luminance helpers (adjust.c / enhance.c / background.c)
 * --------------------------------------------------------------------- */

/* Box-average an image into a small float plane of luma (gamma-encoded,
 * 0..1), integer factor f (>= 1).  *ow / *oh receive the size. */
float *ivp_luma_small(const IvImage *img, int f, int *ow, int *oh);
/* Bilinear sample of a small plane at full-resolution position
 * ((x + 0.5) / f - 0.5, ...). */
float ivp_plane_sample(const float *p, int w, int h, float x, float y);

/* Shared pieces used by Adjust and Styles. */
void ivp_vignette_tables(int w, int h, float **nx2, float **ny2);
float ivp_vignette_falloff(float nx2, float ny2);
float ivp_grain_sample(int x, int y, double cell, uint32_t seed);

/* Whole-file read (refuses files above max bytes; NULL on error) and a
 * write through "<path>.ivproc-tmp" + rename(). (image.c) */
uint8_t *ivp_read_file(const char *path, size_t max, size_t *len);
int ivp_write_file_atomic(const char *path, const uint8_t *data, size_t len);

/* Inpainting and background internals are file-local. */

#endif /* IVPRIV_H */
