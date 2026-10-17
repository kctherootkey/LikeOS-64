// audiod host unit tests: conversion, channel mapping, resampling, mixing,
// the per-stream pipeline, the wire protocol parser, the WAV reader and the
// configuration and state parsers - including hostile input.
//
// Built for the build machine with -fsanitize=address,undefined (see the
// Makefile next to this file).  It only computes; it opens no files and no
// sockets.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../audiod.h"

static int failures, checks;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

/* ---- conversion ---------------------------------------------------------------- */

static void test_convert(void)
{
	static const int fmts[] = { SF_U8,    SF_S16LE, SF_S16BE, SF_S24LE,
				    SF_S24_3LE, SF_S20LE, SF_S32LE, SF_F32LE,
				    SF_F64LE };
	static const float tol[] = { 1.0f / 100, 1.0f / 30000, 1.0f / 30000,
				     1e-6f,	 1e-6f,	       1e-5f,
				     1e-6f,	 0.0f,	       0.0f };
	float in[257], back[257];
	uint8_t raw[257 * 8];

	for (int i = 0; i < 257; i++)
		in[i] = -1.0f + 2.0f * (float)i / 256.0f;

	for (size_t f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
		struct dither d;
		dither_init(&d, 1, 0);
		memset(raw, 0xa5, sizeof(raw));
		cv_from_float(fmts[f], in, raw, 257, &d);
		cv_to_float(fmts[f], raw, back, 257);
		float worst = 0.0f;
		for (int i = 0; i < 257; i++) {
			float e = fabsf(back[i] - in[i]);
			if (e > worst)
				worst = e;
		}
		CHECK(worst <= tol[f] + 1e-7f, "format %s round trip error %g",
		      sf_name(fmts[f]), (double)worst);
	}

	/* hostile floats are sanitised */
	float nasty[4] = { NAN, INFINITY, -INFINITY, 1e30f };
	float out[4];
	cv_to_float(SF_F32LE, nasty, out, 4);
	CHECK(out[0] == 0.0f && out[1] == 4.0f && out[2] == -4.0f &&
		      out[3] == 4.0f,
	      "float sanitising: %g %g %g %g", (double)out[0], (double)out[1],
	      (double)out[2], (double)out[3]);
	double dn[2] = { NAN, -1e300 };
	cv_to_float(SF_F64LE, dn, out, 2);
	CHECK(out[0] == 0.0f && out[1] == -4.0f, "f64 sanitising");

	/* extremes encode without overflow */
	float ext[3] = { 1.0f, -1.0f, 2.0f };
	int32_t s32[3];
	cv_from_float(SF_S32LE, ext, s32, 3, NULL);
	CHECK(s32[0] == 2147483647 && s32[1] == -2147483647 &&
		      s32[2] == 2147483647,
	      "s32 extremes %d %d %d", s32[0], s32[1], s32[2]);
	int16_t s16[3];
	cv_from_float(SF_S16LE, ext, s16, 3, NULL);
	CHECK(s16[0] == 32767 && s16[1] == -32767 && s16[2] == 32767,
	      "s16 extremes");
	/* the top byte of S24-in-32 is ignored on input */
	uint32_t s24 = 0xff7fffffu;
	cv_to_float(SF_S24LE, &s24, out, 1);
	CHECK(fabsf(out[0] - 8388607.0f / 8388608.0f) < 1e-6f, "s24 top byte");

	/* dither keeps silence silent */
	float zero[64] = { 0 };
	int16_t z16[64];
	struct dither d;
	dither_init(&d, 99, 1);
	cv_from_float(SF_S16LE, zero, z16, 64, &d);
	int nz = 0;
	for (int i = 0; i < 64; i++)
		nz |= z16[i];
	CHECK(nz == 0, "dithered silence is not silent");

	/* names and protocol mapping */
	CHECK(sf_from_name("s16") == SF_S16LE, "sf_from_name s16");
	CHECK(sf_from_name("bogus") == SF_NONE, "sf_from_name bogus");
	for (uint32_t a = 0; a <= AUD_FMT_MAX + 3; a++) {
		int sf = sf_from_proto(a);
		if (a >= 1 && a <= AUD_FMT_MAX)
			CHECK(sf != SF_NONE && sf_to_proto(sf) == a,
			      "proto format %u", a);
		else
			CHECK(sf == SF_NONE, "bad proto format %u accepted", a);
	}
}

/* ---- channel maps ---------------------------------------------------------------- */

static void test_chmap(void)
{
	struct chmap cm;

	for (int i = 1; i <= DSP_MAX_CH; i++)
		for (int o = 1; o <= DSP_MAX_CH; o++) {
			CHECK(chmap_init(&cm, i, o) == 0, "chmap %d->%d", i, o);
			for (int r = 0; r < o; r++) {
				float sum = 0.0f;
				for (int c = 0; c < i; c++) {
					CHECK(cm.m[r][c] >= 0.0f,
					      "negative coefficient");
					sum += cm.m[r][c];
				}
				CHECK(sum <= 2.0f + 1e-5f,
				      "row %d of %d->%d sums to %g", r, i, o,
				      (double)sum);
				if (i > o)
					CHECK(sum <= 1.0f + 1e-5f,
					      "downmix row %d of %d->%d sums to %g",
					      r, i, o, (double)sum);
			}
		}
	CHECK(chmap_init(&cm, 0, 2) < 0 && chmap_init(&cm, 2, 9) < 0,
	      "bad channel counts accepted");

	/* mono -> stereo: unity on both */
	chmap_init(&cm, 1, 2);
	float mono[3] = { 0.5f, -0.25f, 1.0f }, st[6];
	chmap_apply(&cm, mono, st, 3);
	CHECK(st[0] == 0.5f && st[1] == 0.5f && st[4] == 1.0f && st[5] == 1.0f,
	      "mono->stereo");

	/* stereo -> mono: average */
	chmap_init(&cm, 2, 1);
	float s2[2] = { 1.0f, 0.0f }, m1;
	chmap_apply(&cm, s2, &m1, 1);
	CHECK(fabsf(m1 - 0.5f) < 1e-6f, "stereo->mono %g", (double)m1);

	/* 5.1 -> stereo: centre reaches both sides, LFE is dropped, a
	 * left-only signal stays left */
	chmap_init(&cm, 6, 2);
	CHECK(cm.m[0][2] > 0.0f && cm.m[1][2] > 0.0f, "centre folded");
	CHECK(cm.m[0][3] == 0.0f && cm.m[1][3] == 0.0f, "LFE dropped");
	CHECK(cm.m[1][0] == 0.0f && cm.m[0][1] == 0.0f, "no crosstalk");
	CHECK(cm.m[0][4] > 0.0f && cm.m[1][5] > 0.0f, "rears folded");

	/* 7.1 -> 5.1: sides onto rears */
	chmap_init(&cm, 8, 6);
	CHECK(cm.m[4][6] > 0.0f && cm.m[5][7] > 0.0f, "7.1 sides -> 5.1 rears");

	/* in-place shrink */
	chmap_init(&cm, 6, 2);
	float buf[12];
	for (int k = 0; k < 12; k++)
		buf[k] = 0.1f * (float)k;
	chmap_apply(&cm, buf, buf, 2);
	CHECK(isfinite(buf[0]) && isfinite(buf[3]), "in-place downmix");
}

/* ---- resampling ------------------------------------------------------------------ */

/* Feed a sine through rs in random chunk sizes, compare against the ideal
 * output; returns the SNR in dB and the output count. */
static double resample_snr(uint32_t in_rate, uint32_t out_rate, int quality,
			   double freq, size_t in_frames, size_t *out_count)
{
	struct resampler rs;
	if (rs_init(&rs, in_rate, out_rate, 2, quality) < 0)
		return -1.0;
	size_t out_cap = (size_t)((double)in_frames * out_rate / in_rate) + 4096;
	float *in = malloc(in_frames * 2 * sizeof(float));
	float *out = malloc(out_cap * 2 * sizeof(float));
	for (size_t i = 0; i < in_frames; i++) {
		float v = (float)(0.5 * sin(2.0 * M_PI * freq * (double)i /
					    in_rate));
		in[2 * i] = v;
		in[2 * i + 1] = -v;
	}
	size_t fed = 0, got = 0;
	while (fed < in_frames) {
		size_t chunk = 1 + rnd() % 3000;
		if (chunk > in_frames - fed)
			chunk = in_frames - fed;
		size_t took = rs_push(&rs, in + 2 * fed, chunk);
		fed += took;
		got += rs_pull(&rs, out + 2 * got, out_cap - got);
	}
	/* the tail */
	rs_push_silence(&rs, rs_tail_frames(&rs));
	got += rs_pull(&rs, out + 2 * got, out_cap - got);

	/* the kernel is centred, so output k is at input time k*M/L */
	double sig = 0.0, err = 0.0;
	size_t edge = 2000; /* skip start-up and the tail */
	size_t ideal_n = (size_t)((double)in_frames * out_rate / in_rate);
	for (size_t k = edge; k + edge < ideal_n && k < got; k++) {
		double t = (double)k / out_rate;
		double ideal = 0.5 * sin(2.0 * M_PI * freq * t);
		double e = out[2 * k] - ideal;
		double e2 = out[2 * k + 1] + ideal;
		sig += ideal * ideal * 2;
		err += e * e + e2 * e2;
	}
	*out_count = got;
	free(in);
	free(out);
	rs_free(&rs);
	if (err <= 0.0)
		return 200.0;
	return 10.0 * log10(sig / err);
}

static void test_resample(void)
{
	size_t n;
	double snr;

	snr = resample_snr(44100, 48000, RS_QUALITY_BEST, 1000.0, 44100, &n);
	printf("resample 44100->48000 best: SNR %.1f dB, %zu frames\n", snr, n);
	CHECK(snr > 90.0, "44.1->48 best: SNR %.1f dB", snr);
	/* 1 s in -> 1 s out, plus at most the flushed tail */
	CHECK(n >= 48000 && n <= 48000 + 64, "44.1->48 length %zu", n);

	snr = resample_snr(44100, 48000, RS_QUALITY_GOOD, 1000.0, 44100, &n);
	printf("resample 44100->48000 good: SNR %.1f dB\n", snr);
	CHECK(snr > 70.0, "44.1->48 good: SNR %.1f dB", snr);

	snr = resample_snr(44100, 48000, RS_QUALITY_FAST, 440.0, 44100, &n);
	printf("resample 44100->48000 fast: SNR %.1f dB\n", snr);
	CHECK(snr > 30.0, "44.1->48 fast: SNR %.1f dB", snr);

	snr = resample_snr(48000, 44100, RS_QUALITY_BEST, 1000.0, 48000, &n);
	CHECK(snr > 85.0, "48->44.1 best: SNR %.1f dB", snr);

	snr = resample_snr(8000, 48000, RS_QUALITY_GOOD, 440.0, 8000, &n);
	CHECK(snr > 60.0, "8->48 good: SNR %.1f dB", snr);
	CHECK(n >= 48000 && n <= 48000 + 64, "8->48 length %zu", n);

	snr = resample_snr(22050, 48000, RS_QUALITY_GOOD, 1000.0, 22050, &n);
	CHECK(snr > 70.0, "22.05->48 good: SNR %.1f dB", snr);

	snr = resample_snr(192000, 48000, RS_QUALITY_GOOD, 1000.0, 192000, &n);
	CHECK(snr > 60.0, "192->48 good: SNR %.1f dB", snr);

	/* a ratio with a huge L: interpolated phases, exact timing */
	snr = resample_snr(47999, 48000, RS_QUALITY_BEST, 1000.0, 47999, &n);
	printf("resample 47999->48000 best (interpolated phases): SNR %.1f dB\n",
	       snr);
	CHECK(snr > 70.0, "47999->48000 best: SNR %.1f dB", snr);

	/* drift: 10 minutes of 44.1 kHz in odd chunks must give exactly the
	 * right number of 48 kHz frames before the tail */
	struct resampler rs;
	CHECK(rs_init(&rs, 44100, 48000, 1, RS_QUALITY_FAST) == 0, "init");
	float *buf = calloc(8192, sizeof(float));
	float *out = calloc(16384, sizeof(float));
	uint64_t fed = 0, got = 0, total = 44100ull * 600;
	while (fed < total) {
		size_t chunk = 1 + rnd() % 4096;
		if (chunk > total - fed)
			chunk = (size_t)(total - fed);
		size_t took = rs_push(&rs, buf, chunk);
		fed += took;
		size_t g;
		while ((g = rs_pull(&rs, out, 16384)) > 0)
			got += g;
	}
	/* outputs whose position k*147/160 < total - (taps-1) are complete:
	 * for the linear kernel that is exactly ceil((total-1)*160/147) */
	uint64_t expect = ((total - 1) * 160 + 146) / 147;
	CHECK(got == expect, "drift: got %llu frames, expected %llu",
	      (unsigned long long)got, (unsigned long long)expect);
	rs_free(&rs);

	/* identity rate is a bypass */
	CHECK(rs_init(&rs, 48000, 48000, 2, RS_QUALITY_BEST) == 0 && rs.bypass,
	      "bypass");
	rs_free(&rs);
	CHECK(rs_init(&rs, 0, 48000, 2, RS_QUALITY_BEST) < 0, "rate 0");
	CHECK(rs_init(&rs, 48000, 44100, 9, RS_QUALITY_BEST) < 0, "9 channels");

	/* every ratio between common rates initialises and runs */
	static const uint32_t rates[] = { 4000,  8000,	 11025,	 16000,	 22050,
					  32000, 44100,	 48000,	 88200,	 96000,
					  176400, 192000, 384000 };
	for (size_t a = 0; a < sizeof(rates) / sizeof(rates[0]); a++)
		for (size_t b = 0; b < sizeof(rates) / sizeof(rates[0]); b++) {
			for (int q = 0; q <= 2; q++) {
				int rc = rs_init(&rs, rates[a], rates[b], 2, q);
				CHECK(rc == 0, "init %u->%u q%d", rates[a],
				      rates[b], q);
				if (rc)
					continue;
				size_t t = rs_push(&rs, buf, 4096 / 2);
				(void)t;
				size_t g = rs_pull(&rs, out, 8192 / 2);
				for (size_t k = 0; k < g * 2; k++)
					CHECK(isfinite(out[k]), "non-finite");
				rs_free(&rs);
			}
		}
	rs_cache_trim();
	free(buf);
	free(out);
}

/* ---- mixer ------------------------------------------------------------------------ */

static void test_mixer(void)
{
	/* soft clip: identity below the knee, bounded, monotonic */
	float prev = -2.0f;
	for (int i = -4000; i <= 4000; i++) {
		float x = (float)i / 1000.0f;
		float y = soft_clip(x);
		CHECK(y >= prev - 1e-6f, "soft clip not monotonic at %g",
		      (double)x);
		CHECK(y <= 1.0f && y >= -1.0f, "soft clip unbounded at %g",
		      (double)x);
		if (fabsf(x) < 0.89f)
			CHECK(y == x, "soft clip changes %g", (double)x);
		prev = y;
	}
	CHECK(soft_clip(NAN) == 0.0f, "soft clip NaN");

	struct mixer m;
	CHECK(mixer_init(&m, 48000, 2, SF_S16LE, 960) == 0, "mixer init");
	mixer_set_master(&m, 100, 100, 0, 1);

	/* two full-scale streams: the sum is limited, never wraps */
	int16_t out[960 * 2];
	for (int block = 0; block < 20; block++) {
		float *acc = mixer_begin(&m, 960);
		for (int i = 0; i < 960 * 2; i++)
			acc[i] = 2.0f * (float)sin(2 * M_PI * 1000.0 *
						   (block * 960 + i / 2) / 48000.0);
		mixer_finish(&m, 960, out);
		/* integer wrap would flip the sign at the peaks */
		int ok = 1;
		for (int i = 0; i < 960 * 2; i++) {
			double x = sin(2 * M_PI * 1000.0 * (block * 960 + i / 2) /
				       48000.0);
			if ((x > 0.2 && out[i] <= 0) || (x < -0.2 && out[i] >= 0))
				ok = 0;
		}
		CHECK(ok, "mixer output wrapped");
	}
	CHECK(m.lim_gain < 0.6f, "limiter did not engage (gain %g)",
	      (double)m.lim_gain);

	/* volume change ramps: no step bigger than the ramp allows */
	mixer_set_master(&m, 100, 100, 0, 1);
	for (int b = 0; b < 30; b++) {
		float *acc = mixer_begin(&m, 960);
		for (int i = 0; i < 960 * 2; i++)
			acc[i] = 0.5f;
		mixer_finish(&m, 960, out);
	}
	mixer_set_master(&m, 0, 0, 0, 1);
	float *acc = mixer_begin(&m, 960);
	for (int i = 0; i < 960 * 2; i++)
		acc[i] = 0.5f;
	mixer_finish(&m, 960, out);
	int maxstep = 0;
	for (int i = 2; i < 960 * 2; i += 2) {
		int d = abs(out[i] - out[i - 2]);
		if (d > maxstep)
			maxstep = d;
	}
	CHECK(maxstep < 200, "volume change clicks (step %d)", maxstep);
	CHECK(out[2 * 959] == 0, "volume 0 not reached after the ramp");

	/* mute and the shutdown fade */
	mixer_set_master(&m, 100, 100, 1, 1);
	for (int b = 0; b < 2; b++) {
		acc = mixer_begin(&m, 960);
		for (int i = 0; i < 960 * 2; i++)
			acc[i] = 0.5f;
		mixer_finish(&m, 960, out);
	}
	CHECK(out[0] == 0 && out[1919] == 0, "mute");
	mixer_set_master(&m, 100, 100, 0, 1);
	mixer_fade_out(&m, 10);
	acc = mixer_begin(&m, 960);
	mixer_finish(&m, 960, out);
	CHECK(mixer_fade_done(&m), "fade not done");
	mixer_free(&m);

	CHECK(vol_to_gain(0) == 0.0f && vol_to_gain(100) == 1.0f &&
		      vol_to_gain(50) > 0.1f && vol_to_gain(50) < 0.15f,
	      "volume curve");

	/* every device format */
	static const int f[] = { SF_U8,	   SF_S16LE, SF_S24LE, SF_S24_3LE,
				 SF_S20LE, SF_S32LE, SF_F32LE };
	for (size_t k = 0; k < sizeof(f) / sizeof(f[0]); k++) {
		CHECK(mixer_init(&m, 44100, 6, f[k], 512) == 0, "init fmt");
		uint8_t raw[512 * 6 * 4];
		acc = mixer_begin(&m, 512);
		for (int i = 0; i < 512 * 6; i++)
			acc[i] = (float)((int)(rnd() % 2001) - 1000) / 300.0f;
		mixer_finish(&m, 512, raw);
		mixer_free(&m);
	}
}

/* ---- stream pipeline --------------------------------------------------------------- */

static void test_stream(void)
{
	struct dsp_stream s;
	/* 44.1 kHz S16 stereo into a 48 kHz stereo bus */
	CHECK(dsp_stream_init(&s, SF_S16LE, 44100, 2, 44100 * 4, 48000, 2,
			      RS_QUALITY_GOOD) == 0,
	      "stream init");
	dsp_stream_set_volume(&s, 100, 100, 0);
	int16_t pcm[4410 * 2];
	for (int i = 0; i < 4410; i++) {
		int16_t v = (int16_t)(16000 * sin(2 * M_PI * 440.0 * i / 44100.0));
		pcm[2 * i] = v;
		pcm[2 * i + 1] = v;
	}
	size_t total_out = 0;
	float acc[480 * 2];
	for (int rep = 0; rep < 10; rep++) {
		CHECK(ring_write(&s.ring, pcm, sizeof(pcm)) == sizeof(pcm),
		      "ring write");
		/* 100 ms in -> 4800 frames out per round, in 10 ms blocks */
		for (int b = 0; b < 10; b++) {
			memset(acc, 0, sizeof(acc));
			total_out += dsp_stream_mix(&s, acc, 480);
		}
	}
	CHECK(total_out >= 48000 - 64 && total_out <= 48000,
	      "1 s of 44.1k gave %zu bus frames", total_out);
	/* drain: the rest comes out, then the stream is finished */
	dsp_stream_set_eof(&s, 1);
	for (int b = 0; b < 20 && !dsp_stream_finished(&s); b++) {
		memset(acc, 0, sizeof(acc));
		total_out += dsp_stream_mix(&s, acc, 480);
	}
	CHECK(dsp_stream_finished(&s), "drain did not finish");
	CHECK(total_out >= 48000 && total_out <= 48000 + 64,
	      "total after drain %zu", total_out);
	dsp_stream_free(&s);

	/* every client format and channel count into a 6-channel bus */
	static const int fm[] = { SF_U8,      SF_S16LE, SF_S16BE, SF_S24LE,
				  SF_S24_3LE, SF_S32LE, SF_F32LE, SF_F64LE };
	for (size_t k = 0; k < sizeof(fm) / sizeof(fm[0]); k++)
		for (int ch = 1; ch <= 8; ch++) {
			int rc = dsp_stream_init(&s, fm[k], 22050, ch, 65536, 48000,
						 6, RS_QUALITY_FAST);
			CHECK(rc == 0, "init %s %d ch", sf_name(fm[k]), ch);
			if (rc)
				continue;
			uint8_t junk[4096];
			for (size_t i = 0; i < sizeof(junk); i++)
				junk[i] = (uint8_t)rnd();
			size_t fb = s.frame_bytes;
			ring_write(&s.ring, junk, sizeof(junk) / fb * fb);
			float a6[256 * 6] = { 0 };
			dsp_stream_mix(&s, a6, 256);
			for (int i = 0; i < 256 * 6; i++)
				CHECK(isfinite(a6[i]) && fabsf(a6[i]) < 64.0f,
				      "junk produced %g", (double)a6[i]);
			CHECK(dsp_stream_retarget(&s, 44100, 2) == 0, "retarget");
			dsp_stream_mix(&s, a6, 256);
			dsp_stream_free(&s);
		}

	/* static (event) source */
	int16_t ev[1000];
	for (int i = 0; i < 1000; i++)
		ev[i] = (int16_t)(i * 30);
	CHECK(dsp_stream_init_static(&s, SF_S16LE, 48000, 1, (uint8_t *)ev,
				     sizeof(ev), 48000, 2, RS_QUALITY_GOOD) == 0,
	      "static init");
	float a2[2048 * 2] = { 0 };
	size_t g = dsp_stream_mix(&s, a2, 2048);
	CHECK(g == 1000, "static source gave %zu frames", g);
	CHECK(dsp_stream_finished(&s), "static source not finished");
	CHECK(ring_space(&s.ring) == 0, "static ring writable");
	dsp_stream_free(&s);

	/* the ring wraps correctly */
	struct bytering r;
	ring_init(&r, 10);
	uint8_t a[7] = { 1, 2, 3, 4, 5, 6, 7 }, b[10];
	CHECK(ring_write(&r, a, 7) == 7, "w1");
	CHECK(ring_read(&r, b, 5) == 5 && b[4] == 5, "r1");
	CHECK(ring_write(&r, a, 7) == 7, "w2 (wraps)");
	CHECK(ring_write(&r, a, 7) == 1, "overflow refused");
	/* 2 left + 7 + the 1 that fitted = 10 */
	CHECK(ring_read(&r, b, 10) == 10 && b[0] == 6 && b[2] == 1 &&
		      b[8] == 7 && b[9] == 1,
	      "r2 %u %u %u %u", b[0], b[2], b[8], b[9]);
	ring_free(&r);
}

/* ---- protocol --------------------------------------------------------------------- */

static size_t put_msg(uint8_t *buf, uint32_t type, const void *p, uint32_t len)
{
	struct aud_header h = { type, len };
	memcpy(buf, &h, sizeof(h));
	if (len)
		memcpy(buf + sizeof(h), p, len);
	return sizeof(h) + len;
}

static int feed_all(struct msg_reader *mr, const uint8_t *data, size_t len,
		    int bytewise, int *messages)
{
	size_t off = 0;
	*messages = 0;
	while (off < len) {
		size_t used = 0;
		size_t avail = bytewise ? 1 : len - off;
		int r = mr_feed(mr, data + off, avail, &used);
		off += used;
		if (r == MR_ERROR)
			return -1;
		if (r == MR_MESSAGE) {
			(*messages)++;
			mr_next(mr);
		}
		if (used == 0 && r != MR_MESSAGE)
			return -2; /* no progress */
	}
	return 0;
}

static void test_protocol(void)
{
	struct msg_reader mr;
	uint8_t buf[200000];
	int msgs;

	/* a valid conversation, whole and byte by byte */
	struct aud_hello h;
	memset(&h, 0, sizeof(h));
	h.version = AUDIOD_PROTOCOL_VERSION;
	h.kind = AUD_CLIENT_STREAM;
	strcpy(h.name, "test");
	struct aud_stream_open o;
	memset(&o, 0, sizeof(o));
	o.format = AUD_FMT_S16_LE;
	o.rate = 44100;
	o.channels = 2;
	o.role = AUD_ROLE_MUSIC;
	o.volume = 100;
	strcpy(o.name, "song");
	uint8_t pcm[4096] = { 0 };
	size_t n = 0;
	n += put_msg(buf + n, AUD_MSG_HELLO, &h, sizeof(h));
	n += put_msg(buf + n, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	n += put_msg(buf + n, AUD_MSG_DATA, pcm, sizeof(pcm));
	n += put_msg(buf + n, AUD_MSG_DATA, pcm, 0);
	n += put_msg(buf + n, AUD_MSG_GET_LATENCY, NULL, 0);
	n += put_msg(buf + n, AUD_MSG_DRAIN, NULL, 0);
	for (int bw = 0; bw <= 1; bw++) {
		mr_init(&mr);
		CHECK(feed_all(&mr, buf, n, bw, &msgs) == 0 && msgs == 6,
		      "valid sequence (bytewise %d): %d messages", bw, msgs);
		CHECK(mr_partial(&mr) == 0, "leftover bytes");
		mr_free(&mr);
	}

	/* oversized, unknown, server-only, wrong length */
	struct {
		uint32_t type, len;
		const char *what;
	} bad[] = {
		{ AUD_MSG_DATA, AUDIOD_MAX_PAYLOAD + 1, "oversized DATA" },
		{ AUD_MSG_DATA, 0xffffffffu, "huge length" },
		{ 0, 0, "type 0" },
		{ 9999, 0, "unknown type" },
		{ AUD_MSG_OK, 0, "server-only OK" },
		{ AUD_MSG_REQUEST, 8, "server-only REQUEST" },
		{ AUD_MSG_NOTIFY, 16, "server-only NOTIFY" },
		{ AUD_MSG_UNDERRUN, 0, "server-only UNDERRUN" },
		{ AUD_MSG_HELLO, sizeof(struct aud_hello) - 1, "short HELLO" },
		{ AUD_MSG_HELLO, sizeof(struct aud_hello) + 1, "long HELLO" },
		{ AUD_MSG_DRAIN, 4, "DRAIN with payload" },
		{ AUD_MSG_SET_MASTER_VOLUME, 4, "short volume" },
		{ AUD_MSG_LIST_STREAMS, 1, "LIST with payload" },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct aud_header hh = { bad[i].type, bad[i].len };
		mr_init(&mr);
		size_t used;
		int r = mr_feed(&mr, (uint8_t *)&hh, sizeof(hh), &used);
		CHECK(r == MR_ERROR, "%s accepted", bad[i].what);
		CHECK(mr.cap <= AUDIOD_MAX_PAYLOAD, "%s allocated %zu",
		      bad[i].what, mr.cap);
		mr_free(&mr);
	}

	/* fuzz: random streams never crash, never over-allocate */
	for (int iter = 0; iter < 20000; iter++) {
		size_t len = rnd() % 512;
		for (size_t i = 0; i < len; i++)
			buf[i] = (uint8_t)rnd();
		if (rnd() % 3 == 0 && len >= 8) {
			/* plausible header */
			uint32_t t = 1 + rnd() % 60;
			uint32_t l = rnd() % 300;
			memcpy(buf, &t, 4);
			memcpy(buf + 4, &l, 4);
		}
		mr_init(&mr);
		int fr = feed_all(&mr, buf, len, (int)(rnd() & 1), &msgs);
		CHECK(fr != -2, "fuzz: reader stopped making progress");
		CHECK(mr.cap <= AUDIOD_MAX_PAYLOAD, "fuzz over-allocated");
		if (fr == 0 && mr.have_header) {
			/* whatever was completed is internally consistent */
			uint32_t lo, hi;
			CHECK(proto_len_range(mr.h.type, &lo, &hi) == 0 &&
				      mr.h.len >= lo && mr.h.len <= hi,
			      "header passed with a bad length");
		}
		mr_free(&mr);
	}

	/* payload checks */
	char name[AUDIOD_NAME_MAX];
	CHECK(proto_check_hello(&h, name) == 0 && !strcmp(name, "test"),
	      "hello ok");
	struct aud_hello h2 = h;
	h2.version = 2;
	CHECK(proto_check_hello(&h2, name) == -EPROTONOSUPPORT, "version");
	h2 = h;
	h2.kind = 7;
	CHECK(proto_check_hello(&h2, name) == -EINVAL, "kind");
	h2 = h;
	memset(h2.name, 'A', sizeof(h2.name));
	CHECK(proto_check_hello(&h2, name) == -EBADMSG, "unterminated name");
	h2 = h;
	strcpy(h2.name, "a\033[2Jb\n");
	CHECK(proto_check_hello(&h2, name) == 0 && !strchr(name, '\033') &&
		      !strchr(name, '\n'),
	      "control characters kept: %s", name);

	struct stream_req rq;
	CHECK(proto_check_stream_open(&o, &rq) == 0 && rq.fmt == SF_S16LE,
	      "open ok");
	struct aud_stream_open o2;
#define BADOPEN(field, val, want)                                              \
	do {                                                                   \
		o2 = o;                                                        \
		o2.field = val;                                                \
		CHECK(proto_check_stream_open(&o2, &rq) == want,               \
		      "stream open " #field "=" #val);                         \
	} while (0)
	BADOPEN(format, 0, -EINVAL);
	BADOPEN(format, 99, -EINVAL);
	BADOPEN(rate, 3999, -EINVAL);
	BADOPEN(rate, 384001, -EINVAL);
	BADOPEN(rate, 0xffffffffu, -EINVAL);
	BADOPEN(channels, 0, -EINVAL);
	BADOPEN(channels, 9, -EINVAL);
	BADOPEN(role, 8, -EINVAL);
	BADOPEN(buffer_ms, 10001, -EINVAL);
	BADOPEN(volume, 101, -EINVAL);
	BADOPEN(flags, 0x80000000u, -EINVAL);
	BADOPEN(role, 0, 0);
	o2 = o;
	memset(o2.name, 'x', sizeof(o2.name));
	CHECK(proto_check_stream_open(&o2, &rq) == -EBADMSG, "open name");

	struct aud_volume v = { 101, 0, { 0, 0 } };
	CHECK(proto_check_volume(&v) < 0, "volume 101");
	v.left = 100;
	v.right = 100;
	CHECK(proto_check_volume(&v) == 0, "volume 100");
	struct aud_mute mu = { 2, 0 };
	CHECK(proto_check_mute(&mu) < 0, "mute 2");
	struct aud_set_output so = { 3, 0 };
	CHECK(proto_check_set_output(&so) < 0, "outsel 3");
	so.mode = OUTSEL_ID;
	so.id = 0x100; /* an HDMI port */
	CHECK(proto_check_set_output(&so) == 0, "HDMI output id refused");
	so.id = 0x10000;
	CHECK(proto_check_set_output(&so) < 0, "output id");
	struct aud_subscribe sb = { 1u << 9, 0 };
	CHECK(proto_check_subscribe(&sb) < 0, "subscribe bits");

	/* event names */
	static const char *good[] = { "startup", "notify", "volume", "a",
				      "error_2", "log-in" };
	static const char *badn[] = { "",	   "../etc/passwd", "a/b", "..",
				      "Startup", "x.wav", "-rf",	   " x",
				      "abcdefghijklmnopqrstuvwxyz012345" };
	for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
		CHECK(event_name_valid(good[i]), "good name %s refused", good[i]);
	for (size_t i = 0; i < sizeof(badn) / sizeof(badn[0]); i++)
		CHECK(!event_name_valid(badn[i]), "bad name '%s' accepted",
		      badn[i]);
	struct aud_play_event pe;
	char en[AUD_EVENT_NAME_MAX];
	memset(&pe, 0, sizeof(pe));
	strcpy(pe.name, "notify");
	pe.volume = 50;
	CHECK(proto_check_play_event(&pe, en) == 0, "event ok");
	memset(pe.name, 'a', sizeof(pe.name));
	CHECK(proto_check_play_event(&pe, en) == -EBADMSG, "event unterminated");
	strcpy(pe.name, "notify");
	pe.wait = 2;
	CHECK(proto_check_play_event(&pe, en) == -EINVAL, "event wait 2");
}

/* ---- WAV -------------------------------------------------------------------------- */

static size_t make_wav(uint8_t *b, uint16_t tag, uint16_t ch, uint32_t rate,
		       uint16_t bits, uint32_t frames, int extensible)
{
	size_t n = 0;
	uint32_t fmt_len = extensible ? 40 : 16;
	uint32_t block = (uint32_t)ch * bits / 8;
	uint32_t data_len = frames * block;
#define P32(v)                                                                 \
	do {                                                                   \
		uint32_t _v = (v);                                             \
		memcpy(b + n, &_v, 4);                                         \
		n += 4;                                                        \
	} while (0)
#define P16(v)                                                                 \
	do {                                                                   \
		uint16_t _v = (v);                                             \
		memcpy(b + n, &_v, 2);                                         \
		n += 2;                                                        \
	} while (0)
	memcpy(b + n, "RIFF", 4);
	n += 4;
	P32(4 + 8 + fmt_len + 8 + data_len);
	memcpy(b + n, "WAVE", 4);
	n += 4;
	memcpy(b + n, "fmt ", 4);
	n += 4;
	P32(fmt_len);
	P16(extensible ? 0xFFFE : tag);
	P16(ch);
	P32(rate);
	P32(rate * block);
	P16((uint16_t)block);
	P16(bits);
	if (extensible) {
		static const uint8_t tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10,
						  0x00, 0x80, 0x00, 0x00, 0xaa,
						  0x00, 0x38, 0x9b, 0x71 };
		P16(22);
		P16(bits);
		P32(3);
		P16(tag);
		memcpy(b + n, tail, 14);
		n += 14;
	}
	/* an unknown chunk, odd-sized, to exercise padding */
	memcpy(b + n, "LIST", 4);
	n += 4;
	P32(3);
	b[n++] = 'a';
	b[n++] = 'b';
	b[n++] = 'c';
	b[n++] = 0;
	memcpy(b + n, "data", 4);
	n += 4;
	P32(data_len);
	for (uint32_t i = 0; i < data_len; i++)
		b[n++] = (uint8_t)(i * 7);
	/* fix the RIFF size for the LIST chunk */
	uint32_t riff = (uint32_t)n - 8;
	memcpy(b + 4, &riff, 4);
	return n;
}

static void test_wav(void)
{
	static uint8_t b[300000];
	struct wav_info wi;
	size_t n;

	struct {
		uint16_t tag, ch, bits;
		int ext;
		int fmt;
	} ok[] = {
		{ 1, 2, 16, 0, SF_S16LE }, { 1, 1, 8, 0, SF_U8 },
		{ 1, 2, 24, 0, SF_S24_3LE }, { 1, 2, 32, 0, SF_S32LE },
		{ 3, 2, 32, 0, SF_F32LE }, { 3, 1, 64, 0, SF_F64LE },
		{ 1, 6, 16, 1, SF_S16LE }, { 3, 8, 32, 1, SF_F32LE },
	};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		n = make_wav(b, ok[i].tag, ok[i].ch, 44100, ok[i].bits, 1000,
			     ok[i].ext);
		int rc = wav_parse(b, n, &wi);
		CHECK(rc == 0 && wi.fmt == ok[i].fmt && wi.frames == 1000 &&
			      wi.channels == ok[i].ch && wi.rate == 44100,
		      "wav case %zu: rc %d fmt %d frames %zu", i, rc, wi.fmt,
		      wi.frames);
		if (rc == 0)
			CHECK(wi.data >= b && wi.data + wi.data_len <= b + n,
			      "data outside the buffer");
	}

	/* every truncation of a valid file is handled */
	n = make_wav(b, 1, 2, 48000, 16, 500, 0);
	for (size_t cut = 0; cut < n; cut++) {
		int rc = wav_parse(b, cut, &wi);
		if (rc == 0)
			CHECK(wi.data + wi.data_len <= b + cut &&
				      wi.data_len % wi.block_align == 0,
			      "truncated %zu: data past the end", cut);
	}

	/* broken headers */
	n = make_wav(b, 1, 2, 48000, 16, 100, 0);
	uint8_t c[4096];
	memcpy(c, b, n);
	c[22] = 0; /* channels 0 */
	CHECK(wav_parse(c, n, &wi) < 0, "0 channels");
	memcpy(c, b, n);
	c[22] = 9;
	CHECK(wav_parse(c, n, &wi) < 0, "9 channels");
	memcpy(c, b, n);
	c[32] = 3; /* block align */
	CHECK(wav_parse(c, n, &wi) < 0, "bad block align");
	memcpy(c, b, n);
	c[34] = 12; /* bits */
	CHECK(wav_parse(c, n, &wi) < 0, "12 bits");
	memcpy(c, b, n);
	memset(c + 24, 0, 4); /* rate 0 */
	CHECK(wav_parse(c, n, &wi) < 0, "rate 0");
	memcpy(c, b, n);
	c[20] = 2; /* ADPCM */
	CHECK(wav_parse(c, n, &wi) < 0, "ADPCM");
	memcpy(c, b, n);
	memcpy(c + 8, "AVI ", 4);
	CHECK(wav_parse(c, n, &wi) < 0, "not WAVE");
	memcpy(c, b, n);
	uint32_t huge = 0xfffffff0u;
	memcpy(c + 16, &huge, 4); /* fmt chunk size */
	CHECK(wav_parse(c, n, &wi) < 0, "huge fmt chunk");
	memcpy(c, b, n);
	memcpy(c + 4, &huge, 4); /* RIFF size */
	CHECK(wav_parse(c, n, &wi) == 0, "huge RIFF size clipped");
	CHECK(wav_parse(NULL, 100, &wi) < 0, "NULL");
	CHECK(wav_parse(b, WAV_MAX_BYTES + 1, &wi) < 0, "too big");

	/* random mutations */
	for (int iter = 0; iter < 30000; iter++) {
		n = make_wav(b, (rnd() & 1) ? 1 : 3, 1 + rnd() % 8, 8000 + rnd() % 90000,
			     (uint16_t)(8 * (1 + rnd() % 8)), rnd() % 64,
			     (int)(rnd() & 1));
		int muts = 1 + rnd() % 8;
		for (int k = 0; k < muts; k++)
			b[rnd() % n] = (uint8_t)rnd();
		size_t len = (rnd() % 4 == 0) ? rnd() % (n + 1) : n;
		int rc = wav_parse(b, len, &wi);
		if (rc == 0) {
			CHECK(wi.data >= b && wi.data + wi.data_len <= b + len,
			      "mutated: data outside");
			CHECK(wi.channels >= 1 && wi.channels <= 8 &&
				      wi.rate >= 4000 && wi.rate <= 384000 &&
				      wi.fmt != SF_NONE,
			      "mutated: bad values accepted");
			/* decoding it must be safe */
			float tmp[512];
			size_t frames = wi.frames > 512 / wi.channels
						? 512 / wi.channels
						: wi.frames;
			cv_to_float(wi.fmt, wi.data, tmp, frames * wi.channels);
		}
	}
}

/* ---- configuration and state ---------------------------------------------------------- */

static void test_config(void)
{
	struct audiod_config c;
	char err[160];
	const char *good = "# comment\n"
			   "device = auto\n"
			   "rate=44100\n"
			   "format = s24 ; trailing comment\n"
			   "channels = 2\n"
			   "period_ms = 5\n"
			   "periods = 6\n"
			   "default_volume = 80\n"
			   "resample_quality = best\n"
			   "event_sounds = no\n"
			   "max_streams_per_user = 4\n"
			   "max_clients = 32\n"
			   "log_level = debug\n"
			   "\n\n";
	CHECK(config_parse(good, strlen(good), &c, err, sizeof(err)) == 0,
	      "good config: %s", err);
	CHECK(c.rate == 44100 && c.format == SF_S24LE && c.periods == 6 &&
		      c.period_ms == 5 && c.default_volume == 80 &&
		      c.resample_quality == RS_QUALITY_BEST && !c.event_sounds &&
		      c.max_streams_per_user == 4 && c.max_clients == 32 &&
		      c.log_level == AL_DEBUG,
	      "values not applied");

	static const char *bad[] = {
		"rate = 44100x\n",
		"rate = 1000\n",
		"rate = -1\n",
		"rate = 99999999999999999999\n",
		"unknown = 1\n",
		"rate = 48000\nrate = 44100\n",
		"device = /etc/passwd\n",
		"device = /dev/audio0/../../etc\n",
		"device = /dev/audio9\n",
		"channels = 0\n",
		"periods = 1\n",
		"default_volume = 101\n",
		"resample_quality = ultra\n",
		"event_sounds = maybe\n",
		"max_clients = 0\n",
		"max_clients = 1000\n",
		"log_level = loud\n",
		"just words\n",
		"= value\n",
		"key =\n",
		"format = s16be\n",
		"device = a b\n",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct audiod_config before;
		config_defaults(&before);
		c = before;
		CHECK(config_parse(bad[i], strlen(bad[i]), &c, err, sizeof(err)) <
				      0,
		      "bad config accepted: %s", bad[i]);
		CHECK(memcmp(&c, &before, sizeof(c)) == 0,
		      "rejected config changed the output");
	}
	/* long line and NUL */
	char lng[2000];
	memset(lng, 'a', sizeof(lng));
	CHECK(config_parse(lng, sizeof(lng), &c, err, sizeof(err)) < 0,
	      "long line");
	const char nul[] = "rate = 48000\0\n";
	CHECK(config_parse(nul, sizeof(nul) - 1, &c, err, sizeof(err)) < 0,
	      "NUL byte");
	CHECK(config_device_ok("/dev/audio3") && config_device_ok("/dev/dsp") &&
		      config_device_ok("2") && !config_device_ok("/dev/audio") &&
		      !config_device_ok("/dev/audio12") && !config_device_ok("8"),
	      "device names");

	/* fuzz the parser */
	for (int iter = 0; iter < 5000; iter++) {
		char t[300];
		size_t len = rnd() % sizeof(t);
		for (size_t i = 0; i < len; i++) {
			static const char alpha[] = "abcdefghijklmnop_rsy= \n#;0123456789/.";
			t[i] = alpha[rnd() % (sizeof(alpha) - 1)];
		}
		config_parse(t, len, &c, err, sizeof(err));
	}

	struct audiod_state st;
	memset(&st, 0, sizeof(st));
	st.card = -1;
	const char *sf = "# x\nvolume_left=40\nvolume_right=50\nmute=1\n"
			 "output_mode=id\noutput_id=3\npcm_output=256\ncard=1\n"
			 "future_key=7\n";
	CHECK(state_parse(sf, strlen(sf), &st) == 0 && st.vol_l == 40 &&
		      st.vol_r == 50 && st.mute == 1 && st.out_mode == OUTSEL_ID &&
		      st.out_id == 3 && st.pcm_output == 256 && st.card == 1,
	      "state parse");
	const char *sb = "volume_left=400\nmute=7\ncard=99\noutput_id=99999\n";
	memset(&st, 0, sizeof(st));
	CHECK(state_parse(sb, strlen(sb), &st) < 0 && st.vol_l == 0 &&
		      st.mute == 0,
	      "bad state values accepted");
}

int main(void)
{
	log_set_level(AL_ERR);
	test_convert();
	test_chmap();
	test_resample();
	test_mixer();
	test_stream();
	test_protocol();
	test_wav();
	test_config();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
