// LikeOS -- LikeOS Image Viewer: colour-space helpers shared by the
// processing modules (sRGB transfer curve, CIE Lab, hue, gamut fitting and
// the monotone tone-curve spline).
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>

#include "ivpriv.h"

/* The IEC 61966-2-1 sRGB transfer function, both directions. */
float ivp_srgb_to_linear(float v)
{
	if (v <= 0.04045f)
		return v / 12.92f;
	return powf((v + 0.055f) / 1.055f, 2.4f);
}

float ivp_linear_to_srgb(float v)
{
	if (v <= 0.0f)
		return 0.0f;
	if (v <= 0.0031308f)
		return v * 12.92f;
	return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

void ivp_build_srgb_lut(float lut[256])
{
	for (int i = 0; i < 256; i++)
		lut[i] = ivp_srgb_to_linear((float)i / 255.0f);
}

/* The CIE f(t) used by XYZ -> Lab, with its linear toe. */
static float lab_f(float t)
{
	const float d = 6.0f / 29.0f;
	if (t > d * d * d)
		return cbrtf(t);
	return t / (3.0f * d * d) + 4.0f / 29.0f;
}

void ivp_rgb8_to_lab(const float lut[256], uint8_t r8, uint8_t g8, uint8_t b8,
		     float lab[3])
{
	float r = lut[r8], g = lut[g8], b = lut[b8];
	/* sRGB primaries, D65 white, normalised so white has X = Y = Z = 1. */
	float x = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / 0.95047f;
	float y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
	float z = (0.0193339f * r + 0.1191920f * g + 0.9503041f * b) / 1.08883f;
	float fx = lab_f(x), fy = lab_f(y), fz = lab_f(z);
	lab[0] = 116.0f * fy - 16.0f;
	lab[1] = 500.0f * (fx - fy);
	lab[2] = 200.0f * (fy - fz);
}

void ivp_hue_sat(float r, float g, float b, float *hue, float *sat)
{
	float mx = fmaxf(r, fmaxf(g, b));
	float mn = fminf(r, fminf(g, b));
	float d = mx - mn;
	float h = 0.0f;
	if (d > 1e-6f) {
		if (mx == r)
			h = 60.0f * fmodf((g - b) / d, 6.0f);
		else if (mx == g)
			h = 60.0f * ((b - r) / d + 2.0f);
		else
			h = 60.0f * ((r - g) / d + 4.0f);
		if (h < 0.0f)
			h += 360.0f;
	}
	if (hue)
		*hue = h;
	if (sat)
		*sat = mx > 1e-6f ? d / mx : 0.0f;
}

void ivp_gamut_fit(float *r, float *g, float *b)
{
	float y = ivp_luma(*r, *g, *b);
	/* A grey that is itself out of range can only be clamped. */
	if (y <= 0.0f) {
		*r = *g = *b = 0.0f;
		return;
	}
	if (y >= 1.0f) {
		*r = *g = *b = 1.0f;
		return;
	}
	/* Largest t <= 1 with y + t * (c - y) inside 0..1 for every channel. */
	float t = 1.0f;
	float c[3] = { *r, *g, *b };
	for (int i = 0; i < 3; i++) {
		float d = c[i] - y;
		if (c[i] > 1.0f && d > 0.0f)
			t = fminf(t, (1.0f - y) / d);
		else if (c[i] < 0.0f && d < 0.0f)
			t = fminf(t, -y / d);
	}
	if (t < 1.0f) {
		*r = y + t * (c[0] - y);
		*g = y + t * (c[1] - y);
		*b = y + t * (c[2] - y);
	}
	*r = ivp_clampf(*r, 0.0f, 1.0f);
	*g = ivp_clampf(*g, 0.0f, 1.0f);
	*b = ivp_clampf(*b, 0.0f, 1.0f);
}

/*
 * Monotone piecewise cubic Hermite interpolation (Fritsch-Carlson).  Tone
 * curves must never fold back on themselves -- a curve that rises and
 * dips again posterises -- so the tangents of an ordinary cubic spline are
 * limited until every segment is monotone.
 */
void ivp_curve_lut(const float *xs, const float *ys, int n, float *lut,
		   int size)
{
	if (n < 2 || size < 2) {
		for (int i = 0; i < size; i++)
			lut[i] = size > 1 ? (float)i / (float)(size - 1) : 0.0f;
		return;
	}
	enum { MAXP = 16 };
	if (n > MAXP)
		n = MAXP;
	float dx[MAXP], sl[MAXP], m[MAXP];
	for (int i = 0; i < n - 1; i++) {
		dx[i] = xs[i + 1] - xs[i];
		if (dx[i] < 1e-6f)
			dx[i] = 1e-6f;
		sl[i] = (ys[i + 1] - ys[i]) / dx[i];
	}
	m[0] = sl[0];
	m[n - 1] = sl[n - 2];
	for (int i = 1; i < n - 1; i++) {
		if (sl[i - 1] * sl[i] <= 0.0f)
			m[i] = 0.0f;
		else
			m[i] = 0.5f * (sl[i - 1] + sl[i]);
	}
	for (int i = 0; i < n - 1; i++) {
		if (fabsf(sl[i]) < 1e-9f) {
			m[i] = m[i + 1] = 0.0f;
			continue;
		}
		float a = m[i] / sl[i], b = m[i + 1] / sl[i];
		float s = a * a + b * b;
		if (s > 9.0f) {
			float t = 3.0f / sqrtf(s);
			m[i] = t * a * sl[i];
			m[i + 1] = t * b * sl[i];
		}
	}
	int seg = 0;
	for (int i = 0; i < size; i++) {
		float x = (float)i / (float)(size - 1);
		if (x <= xs[0]) {
			lut[i] = ys[0];
			continue;
		}
		if (x >= xs[n - 1]) {
			lut[i] = ys[n - 1];
			continue;
		}
		while (seg < n - 2 && x > xs[seg + 1])
			seg++;
		float h = dx[seg];
		float t = (x - xs[seg]) / h;
		float t2 = t * t, t3 = t2 * t;
		float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
		float h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
		lut[i] = h00 * ys[seg] + h10 * h * m[seg] + h01 * ys[seg + 1] +
			 h11 * h * m[seg + 1];
	}
}
