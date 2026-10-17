// audiod mixing bus: accumulation, master volume, limiter, soft clipper
// and the final conversion into the device format.
//
// Streams add their (already gain-scaled) samples into a float accumulator.
// Float keeps 24 bits of mantissa at any level, so summing many streams
// neither overflows nor loses the quiet ones; the headroom problem moves to
// the end of the chain, where a peak limiter pulls the block gain down when
// the sum exceeds full scale and a soft clipper rounds off whatever the
// limiter's attack lets through, so overload sounds like compression
// instead of the hard edges of integer wrap or clipping.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

#define CLIP_KNEE 0.891f /* -1 dBFS: soft clipping starts here */
#define LIMIT_CEIL 0.98f /* the limiter aims a little below full scale */
#define LIMIT_RELEASE_MS 250.0f

float soft_clip(float x)
{
	float a = x < 0.0f ? -x : x;
	if (!(a > CLIP_KNEE)) /* also catches NaN -> returned as is below */
		return x != x ? 0.0f : x;
	/* tanh shoulder from the knee to full scale: continuous in value
	 * and slope at the knee, asymptotic to 1.0 */
	float span = 1.0f - CLIP_KNEE;
	float y = CLIP_KNEE + span * tanhf((a - CLIP_KNEE) / span);
	return x < 0.0f ? -y : y;
}

int mixer_init(struct mixer *m, uint32_t rate, int channels, int out_fmt,
	       size_t max_frames)
{
	memset(m, 0, sizeof(*m));
	if (rate == 0 || channels < 1 || channels > DSP_MAX_CH ||
	    sf_bytes(out_fmt) == 0 || max_frames == 0)
		return -1;
	m->rate = rate;
	m->channels = channels;
	m->out_fmt = out_fmt;
	m->cap_frames = max_frames;
	m->acc = calloc(max_frames * (size_t)channels, sizeof(float));
	if (!m->acc)
		return -1;
	for (int c = 0; c < DSP_MAX_CH; c++)
		ramp_init(&m->master[c], 1.0f);
	ramp_init(&m->fade, 1.0f);
	m->master_l = m->master_r = 1.0f;
	m->sw_master = 1;
	m->lim_gain = 1.0f;
	/* one-pole release: the gain recovers with this time constant */
	m->lim_release = 1.0f - expf(-1.0f / (LIMIT_RELEASE_MS * 0.001f *
					      (float)rate));
	/* dither matters for the 16-bit and 8-bit outputs only */
	dither_init(&m->dither, 0x13579bdfu ^ rate,
		    out_fmt == SF_S16LE || out_fmt == SF_S16BE ||
			    out_fmt == SF_U8);
	return 0;
}

void mixer_free(struct mixer *m)
{
	free(m->acc);
	memset(m, 0, sizeof(*m));
}

void mixer_set_master(struct mixer *m, uint32_t left, uint32_t right,
		      int mute, int sw)
{
	uint32_t frames = m->rate / 1000u * DSP_RAMP_MS;
	m->sw_master = sw;
	m->master_mute = mute ? 1 : 0;
	m->master_l = sw ? vol_to_gain(left) : 1.0f;
	m->master_r = sw ? vol_to_gain(right) : 1.0f;

	for (int c = 0; c < m->channels; c++) {
		int side = m->channels == 1
				   ? 0
				   : spk_side(spk_layout_pos(m->channels, c));
		float g = side < 0 ? m->master_l
			  : side > 0 ? m->master_r
				     : 0.5f * (m->master_l + m->master_r);
		/* Mute always ramps here, in software, even when the
		 * hardware has a mute switch too: a hardware mute that acts
		 * mid-waveform clicks, a ramp to zero does not. */
		if (m->master_mute)
			g = 0.0f;
		ramp_set(&m->master[c], g, frames);
	}
}

float *mixer_begin(struct mixer *m, size_t frames)
{
	if (frames > m->cap_frames)
		frames = m->cap_frames;
	memset(m->acc, 0, frames * (size_t)m->channels * sizeof(float));
	return m->acc;
}

void mixer_fade_out(struct mixer *m, uint32_t ms)
{
	uint32_t frames = (uint32_t)((uint64_t)m->rate * ms / 1000u);
	ramp_set(&m->fade, 0.0f, frames ? frames : 1);
}

int mixer_fade_done(const struct mixer *m)
{
	return ramp_done(&m->fade) && m->fade.cur == 0.0f;
}

void mixer_finish(struct mixer *m, size_t frames, void *out)
{
	const int ch = m->channels;
	float *a = m->acc;

	if (frames > m->cap_frames)
		frames = m->cap_frames;
	size_t n = frames * (size_t)ch;

	/* 1. master volume, mute and the shutdown fade, all ramped */
	int steady = ramp_done(&m->fade);
	for (int c = 0; c < ch && steady; c++)
		steady = ramp_done(&m->master[c]);
	if (steady) {
		float g[DSP_MAX_CH];
		int unity = 1;
		for (int c = 0; c < ch; c++) {
			g[c] = m->master[c].cur * m->fade.cur;
			if (g[c] != 1.0f)
				unity = 0;
		}
		if (!unity)
			for (size_t f = 0; f < frames; f++)
				for (int c = 0; c < ch; c++)
					a[f * ch + c] *= g[c];
	} else {
		for (size_t f = 0; f < frames; f++) {
			float fd = ramp_next(&m->fade);
			for (int c = 0; c < ch; c++)
				a[f * ch + c] *= ramp_next(&m->master[c]) * fd;
		}
	}

	/* 2. peak limiter over the block */
	float peak = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float v = a[i] < 0.0f ? -a[i] : a[i];
		if (v > peak)
			peak = v;
	}
	float target = 1.0f;
	if (peak * m->lim_gain > LIMIT_CEIL && peak > 0.0f)
		target = LIMIT_CEIL / peak;
	if (target < m->lim_gain) {
		/* attack: reach the target within the block, linearly */
		float g = m->lim_gain;
		float step = (target - g) / (float)(frames ? frames : 1);
		for (size_t f = 0; f < frames; f++) {
			g += step;
			for (int c = 0; c < ch; c++)
				a[f * ch + c] *= g;
		}
		m->lim_gain = target;
		m->clipped++;
	} else if (m->lim_gain < 1.0f) {
		/* release: recover exponentially, never past what the
		 * current block allows */
		float g = m->lim_gain;
		for (size_t f = 0; f < frames; f++) {
			g += (1.0f - g) * m->lim_release;
			if (g > target)
				g = target;
			for (int c = 0; c < ch; c++)
				a[f * ch + c] *= g;
		}
		if (g > 0.99999f)
			g = 1.0f;
		m->lim_gain = g;
	}

	/* 3. soft clip what is left above the knee (the attack ramp lets
	 * the first samples of a sudden peak through) */
	for (size_t i = 0; i < n; i++) {
		float v = a[i];
		if (v > CLIP_KNEE || v < -CLIP_KNEE || v != v)
			a[i] = soft_clip(v);
	}

	/* 4. into the device format */
	cv_from_float(m->out_fmt, a, out, n, &m->dither);
}
