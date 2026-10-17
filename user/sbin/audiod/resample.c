// audiod sample-rate conversion: rational polyphase windowed-sinc filter.
//
// Conversion from in_rate to out_rate is treated as the exact fraction
// L/M = out_rate/in_rate reduced by their greatest common divisor
// (44100 -> 48000 is 160/147).  Output frame k sits at input position
// k * M / L, which is tracked as an integer frame index plus a numerator
// in [0, L).  Every step adds exactly M/L, so no error accumulates no
// matter how long a stream plays: after any number of outputs the read
// position is exact.
//
// The filter for the fractional offset p/L is one row ("phase") of a
// table of Kaiser-windowed sinc kernels.  When L is small enough the table
// has exactly L rows and every output uses a precomputed row; for odd ratios
// with a huge L (47999 -> 48000) the table instead holds P+1 evenly spaced
// rows and the coefficients for the exact offset are interpolated linearly
// between neighbours.  Either way the timing stays exact; only the kernel
// is approximated, far below the stop-band level.
//
// When downsampling, the cut-off moves down to the output Nyquist and the
// kernel is stretched by M/L so the transition band stays as narrow in
// output terms.  Tables are shared between streams with the same ratio and
// quality.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RS_MAX_TAPS 512 /* longest kernel (heavy downsampling) */
#define RS_EXACT_MAX_ENTRIES (128u * 1024u) /* floats in an exact table */
#define RS_EXACT_MAX_L 4096u
#define RS_HIST_EXTRA 4096 /* input frames buffered beyond the kernel */
#define RS_CACHE_SLOTS 12

struct rs_table {
	uint32_t L, M;
	int quality;
	int taps;
	int exact; /* rows == L, else rows == phases + 1 (interpolated) */
	uint32_t phases;
	float *coef; /* rows x taps */
	int refs;
};

static struct rs_table *rs_cache[RS_CACHE_SLOTS];

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

/* Modified Bessel function of the first kind, order 0, by its power
 * series; converges quickly for the beta values used here (<= 12). */
static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0, q = x * x / 4.0;
	for (int k = 1; k < 64; k++) {
		term *= q / ((double)k * (double)k);
		sum += term;
		if (term < sum * 1e-17)
			break;
	}
	return sum;
}

static double sinc(double x)
{
	if (fabs(x) < 1e-12)
		return 1.0;
	double px = M_PI * x;
	return sin(px) / px;
}

/* Kernel parameters per quality: zero crossings each side, Kaiser beta
 * and where the pass band ends (fraction of the lower Nyquist). */
static void quality_params(int quality, int *half, double *beta,
			   double *rolloff, uint32_t *phases)
{
	if (quality >= RS_QUALITY_BEST) {
		*half = 32;
		*beta = 10.06; /* about 100 dB stop band */
		*rolloff = 0.945;
		*phases = 512;
	} else {
		*half = 16;
		*beta = 8.0; /* about 80 dB */
		*rolloff = 0.91;
		*phases = 256;
	}
}

/* Fill one table row: the kernel sampled at k - (taps/2 - 1) - frac for
 * k = 0..taps-1, normalised to unity DC gain. */
static void make_row(float *row, int taps, double frac, double fc,
		     double beta, double i0b)
{
	double half = taps / 2.0;
	double sum = 0.0;
	double tmp[RS_MAX_TAPS];

	for (int k = 0; k < taps; k++) {
		double t = (double)k - (half - 1.0) - frac;
		double r = t / half;
		double w = 0.0;
		if (r > -1.0 && r < 1.0)
			w = bessel_i0(beta * sqrt(1.0 - r * r)) / i0b;
		tmp[k] = fc * sinc(fc * t) * w;
		sum += tmp[k];
	}
	if (fabs(sum) < 1e-9)
		sum = 1.0;
	for (int k = 0; k < taps; k++)
		row[k] = (float)(tmp[k] / sum);
}

static struct rs_table *table_build(uint32_t L, uint32_t M, int quality)
{
	int half;
	double beta, rolloff;
	uint32_t phases;
	quality_params(quality, &half, &beta, &rolloff, &phases);

	double fc = rolloff; /* relative to the input Nyquist */
	int taps = 2 * half;
	if (M > L) {
		/* downsampling: cut off below the output Nyquist and widen
		 * the kernel to keep the same transition in output terms */
		fc = rolloff * (double)L / (double)M;
		double t = (double)taps * (double)M / (double)L;
		taps = (int)ceil(t);
		if (taps & 1)
			taps++;
		if (taps > RS_MAX_TAPS)
			taps = RS_MAX_TAPS;
	}

	struct rs_table *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->L = L;
	t->M = M;
	t->quality = quality;
	t->taps = taps;

	size_t rows;
	if (L <= RS_EXACT_MAX_L &&
	    (size_t)L * (size_t)taps <= RS_EXACT_MAX_ENTRIES) {
		t->exact = 1;
		t->phases = L;
		rows = L;
	} else {
		t->exact = 0;
		t->phases = phases;
		rows = (size_t)phases + 1;
	}

	t->coef = malloc(rows * (size_t)taps * sizeof(float));
	if (!t->coef) {
		free(t);
		return NULL;
	}

	double i0b = bessel_i0(beta);
	for (size_t r = 0; r < rows; r++) {
		double frac = t->exact ? (double)r / (double)L
				       : (double)r / (double)phases;
		make_row(t->coef + r * (size_t)taps, taps, frac, fc, beta,
			 i0b);
	}
	return t;
}

static struct rs_table *table_get(uint32_t L, uint32_t M, int quality)
{
	int free_slot = -1;
	for (int i = 0; i < RS_CACHE_SLOTS; i++) {
		struct rs_table *t = rs_cache[i];
		if (!t) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (t->L == L && t->M == M && t->quality == quality) {
			t->refs++;
			return t;
		}
	}
	struct rs_table *t = table_build(L, M, quality);
	if (!t)
		return NULL;
	t->refs = 1;
	if (free_slot < 0) {
		/* evict an unused table */
		for (int i = 0; i < RS_CACHE_SLOTS; i++) {
			if (rs_cache[i] && rs_cache[i]->refs == 0) {
				free(rs_cache[i]->coef);
				free(rs_cache[i]);
				rs_cache[i] = NULL;
				free_slot = i;
				break;
			}
		}
	}
	if (free_slot >= 0)
		rs_cache[free_slot] = t;
	else
		t->refs |= 0x40000000; /* uncached: freed at last put */
	return t;
}

static void table_put(struct rs_table *t)
{
	if (!t)
		return;
	if (t->refs & 0x40000000) {
		int r = (t->refs & ~0x40000000) - 1;
		if (r <= 0) {
			free(t->coef);
			free(t);
		} else {
			t->refs = r | 0x40000000;
		}
		return;
	}
	if (t->refs > 0)
		t->refs--;
	/* kept in the cache for the next stream at this ratio */
}

void rs_cache_trim(void)
{
	for (int i = 0; i < RS_CACHE_SLOTS; i++) {
		if (rs_cache[i] && rs_cache[i]->refs == 0) {
			free(rs_cache[i]->coef);
			free(rs_cache[i]);
			rs_cache[i] = NULL;
		}
	}
}

/* ---- the resampler ------------------------------------------------------- */

int rs_init(struct resampler *rs, uint32_t in_rate, uint32_t out_rate,
	    int channels, int quality)
{
	memset(rs, 0, sizeof(*rs));
	if (in_rate == 0 || out_rate == 0 || channels < 1 ||
	    channels > DSP_MAX_CH)
		return -1;

	rs->in_rate = in_rate;
	rs->out_rate = out_rate;
	rs->ch = channels;
	rs->quality = quality;

	if (in_rate == out_rate) {
		rs->bypass = 1;
		rs->L = rs->M = 1;
		return 0;
	}

	uint32_t g = gcd_u32(in_rate, out_rate);
	rs->L = out_rate / g;
	rs->M = in_rate / g;
	rs->step_int = rs->M / rs->L;
	rs->step_frac = rs->M % rs->L;

	if (quality <= RS_QUALITY_FAST) {
		rs->taps = 2;
		rs->tab = NULL;
	} else {
		rs->tab = table_get(rs->L, rs->M, quality);
		if (!rs->tab)
			return -1;
		rs->taps = rs->tab->taps;
		if (!rs->tab->exact) {
			rs->wbuf = malloc((size_t)rs->taps * sizeof(float));
			if (!rs->wbuf) {
				table_put(rs->tab);
				rs->tab = NULL;
				return -1;
			}
		}
	}

	/* room for the kernel, one step of the largest stride and a decent
	 * chunk of input */
	rs->hist_cap = (size_t)rs->taps + RS_HIST_EXTRA + 2 * (size_t)rs->step_int + 2;
	rs->hist = calloc(rs->hist_cap * (size_t)channels, sizeof(float));
	if (!rs->hist) {
		free(rs->wbuf);
		rs->wbuf = NULL;
		table_put(rs->tab);
		rs->tab = NULL;
		return -1;
	}
	rs_reset(rs);
	return 0;
}

void rs_free(struct resampler *rs)
{
	free(rs->hist);
	free(rs->wbuf);
	table_put(rs->tab);
	memset(rs, 0, sizeof(*rs));
}

void rs_reset(struct resampler *rs)
{
	if (rs->bypass || !rs->hist)
		return;
	/* The kernel is centred: output 0 lies under input frame 0, which
	 * sits taps/2 - 1 frames into the history.  Those leading frames
	 * are silence. */
	size_t lead = (size_t)(rs->taps / 2 - 1);
	memset(rs->hist, 0, rs->hist_cap * (size_t)rs->ch * sizeof(float));
	rs->hist_len = lead;
	rs->base = 0;
	rs->frac = 0;
}

static void rs_compact(struct resampler *rs)
{
	if (rs->base == 0)
		return;
	size_t keep = rs->hist_len > rs->base ? rs->hist_len - rs->base : 0;
	if (keep)
		memmove(rs->hist, rs->hist + rs->base * (size_t)rs->ch,
			keep * (size_t)rs->ch * sizeof(float));
	rs->hist_len = keep;
	rs->base = 0;
}

size_t rs_space(struct resampler *rs)
{
	if (rs->bypass)
		return (size_t)-1;
	if (rs->hist_len >= rs->hist_cap ||
	    rs->base > rs->hist_cap / 2)
		rs_compact(rs);
	return rs->hist_cap - rs->hist_len;
}

size_t rs_push(struct resampler *rs, const float *in, size_t frames)
{
	size_t space = rs_space(rs);
	if (rs->bypass)
		return 0;
	if (frames > space)
		frames = space;
	memcpy(rs->hist + rs->hist_len * (size_t)rs->ch, in,
	       frames * (size_t)rs->ch * sizeof(float));
	rs->hist_len += frames;
	return frames;
}

size_t rs_push_silence(struct resampler *rs, size_t frames)
{
	size_t space = rs_space(rs);
	if (rs->bypass)
		return 0;
	if (frames > space)
		frames = space;
	memset(rs->hist + rs->hist_len * (size_t)rs->ch, 0,
	       frames * (size_t)rs->ch * sizeof(float));
	rs->hist_len += frames;
	return frames;
}

size_t rs_needed(const struct resampler *rs, size_t out_frames)
{
	if (rs->bypass || out_frames == 0)
		return 0;
	/* the last of the outputs starts at base + floor((frac + (n-1)M)/L) */
	uint64_t adv = ((uint64_t)rs->frac +
			(uint64_t)(out_frames - 1) * rs->M) / rs->L;
	uint64_t need_end = (uint64_t)rs->base + adv + (uint64_t)rs->taps;
	if (need_end <= rs->hist_len)
		return 0;
	return (size_t)(need_end - rs->hist_len);
}

double rs_pending_in(const struct resampler *rs)
{
	if (rs->bypass)
		return 0.0;
	double centre = (double)rs->base + (double)(rs->taps / 2 - 1) +
			(double)rs->frac / (double)rs->L;
	double p = (double)rs->hist_len - centre;
	return p > 0.0 ? p : 0.0;
}

size_t rs_tail_frames(const struct resampler *rs)
{
	if (rs->bypass)
		return 0;
	return (size_t)(rs->taps / 2) + 1;
}

/* Advance the read position by exactly M/L input frames. */
static inline void rs_advance(struct resampler *rs)
{
	rs->base += rs->step_int;
	rs->frac += rs->step_frac;
	if (rs->frac >= rs->L) {
		rs->frac -= rs->L;
		rs->base++;
	}
}

size_t rs_pull(struct resampler *rs, float *out, size_t frames)
{
	const int ch = rs->ch;
	const int taps = rs->taps;
	size_t n = 0;

	if (rs->bypass)
		return 0;

	while (n < frames && rs->base + (size_t)taps <= rs->hist_len) {
		const float *h = rs->hist + rs->base * (size_t)ch;
		float *o = out + n * (size_t)ch;
		const float *row;

		if (!rs->tab) {
			/* linear: two taps, weights from the exact fraction */
			float f = (float)rs->frac / (float)rs->L;
			for (int c = 0; c < ch; c++)
				o[c] = h[c] + (h[ch + c] - h[c]) * f;
			rs_advance(rs);
			n++;
			continue;
		}

		if (rs->tab->exact) {
			row = rs->tab->coef + (size_t)rs->frac * (size_t)taps;
		} else {
			/* interpolate between the two nearest stored phases;
			 * the position itself is still exact */
			uint64_t num = (uint64_t)rs->frac * rs->tab->phases;
			uint32_t r0 = (uint32_t)(num / rs->L);
			float a = (float)(num % rs->L) / (float)rs->L;
			const float *c0 = rs->tab->coef + (size_t)r0 * (size_t)taps;
			const float *c1 = c0 + taps;
			for (int k = 0; k < taps; k++)
				rs->wbuf[k] = c0[k] + (c1[k] - c0[k]) * a;
			row = rs->wbuf;
		}

		if (ch == 2) {
			float a0 = 0.0f, a1 = 0.0f;
			for (int k = 0; k < taps; k++) {
				a0 += h[2 * k] * row[k];
				a1 += h[2 * k + 1] * row[k];
			}
			o[0] = a0;
			o[1] = a1;
		} else if (ch == 1) {
			float a0 = 0.0f;
			for (int k = 0; k < taps; k++)
				a0 += h[k] * row[k];
			o[0] = a0;
		} else {
			float acc[DSP_MAX_CH] = { 0 };
			for (int k = 0; k < taps; k++) {
				const float *hk = h + (size_t)k * (size_t)ch;
				float w = row[k];
				for (int c = 0; c < ch; c++)
					acc[c] += hk[c] * w;
			}
			for (int c = 0; c < ch; c++)
				o[c] = acc[c];
		}
		rs_advance(rs);
		n++;
	}
	return n;
}
