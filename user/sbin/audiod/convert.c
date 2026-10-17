// audiod sample format conversion and channel up/down-mixing.
//
// Client formats are decoded into float in [-1, 1]; the mixed bus is
// encoded into whatever the device accepts.  Unaligned input is read with
// memcpy, so a DATA message may start at any byte.  The channel matrices
// follow the usual broadcast downmix rules (centre and surrounds at -3 dB,
// LFE dropped when the target has none) with every output row normalised so
// the matrix alone can never clip.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include <audio/protocol.h>

#include "dsp.h"

/* ---- format tables -------------------------------------------------------- */

static const struct {
	const char *name;
	size_t bytes;
} sf_tab[SF_COUNT] = {
	[SF_NONE] = { "none", 0 },
	[SF_U8] = { "u8", 1 },
	[SF_S16LE] = { "s16", 2 },
	[SF_S16BE] = { "s16be", 2 },
	[SF_S24LE] = { "s24", 4 },
	[SF_S24_3LE] = { "s24_3", 3 },
	[SF_S20LE] = { "s20", 4 },
	[SF_S32LE] = { "s32", 4 },
	[SF_F32LE] = { "float", 4 },
	[SF_F64LE] = { "f64", 8 },
};

size_t sf_bytes(int fmt)
{
	if (fmt <= SF_NONE || fmt >= SF_COUNT)
		return 0;
	return sf_tab[fmt].bytes;
}

const char *sf_name(int fmt)
{
	if (fmt < SF_NONE || fmt >= SF_COUNT)
		return "?";
	return sf_tab[fmt].name;
}

int sf_from_name(const char *name)
{
	if (!name)
		return SF_NONE;
	for (int i = SF_U8; i < SF_COUNT; i++)
		if (strcmp(name, sf_tab[i].name) == 0)
			return i;
	/* a few common spellings */
	if (strcmp(name, "s16le") == 0 || strcmp(name, "s16_le") == 0)
		return SF_S16LE;
	if (strcmp(name, "s24le") == 0 || strcmp(name, "s24_le") == 0)
		return SF_S24LE;
	if (strcmp(name, "s32le") == 0 || strcmp(name, "s32_le") == 0)
		return SF_S32LE;
	if (strcmp(name, "s24_3le") == 0)
		return SF_S24_3LE;
	if (strcmp(name, "f32") == 0 || strcmp(name, "f32le") == 0)
		return SF_F32LE;
	return SF_NONE;
}

int sf_from_proto(uint32_t aud_fmt)
{
	switch (aud_fmt) {
	case AUD_FMT_U8:
		return SF_U8;
	case AUD_FMT_S16_LE:
		return SF_S16LE;
	case AUD_FMT_S24_LE:
		return SF_S24LE;
	case AUD_FMT_S24_3LE:
		return SF_S24_3LE;
	case AUD_FMT_S32_LE:
		return SF_S32LE;
	case AUD_FMT_F32_LE:
		return SF_F32LE;
	case AUD_FMT_S16_BE:
		return SF_S16BE;
	case AUD_FMT_F64_LE:
		return SF_F64LE;
	default:
		return SF_NONE;
	}
}

uint32_t sf_to_proto(int fmt)
{
	switch (fmt) {
	case SF_U8:
		return AUD_FMT_U8;
	case SF_S16LE:
		return AUD_FMT_S16_LE;
	case SF_S16BE:
		return AUD_FMT_S16_BE;
	case SF_S24LE:
	case SF_S20LE: /* closest wire format */
		return AUD_FMT_S24_LE;
	case SF_S24_3LE:
		return AUD_FMT_S24_3LE;
	case SF_S32LE:
		return AUD_FMT_S32_LE;
	case SF_F32LE:
		return AUD_FMT_F32_LE;
	case SF_F64LE:
		return AUD_FMT_F64_LE;
	default:
		return 0;
	}
}

/* ---- decoding --------------------------------------------------------------- */

static inline float sanitize(float x)
{
	/* NaN compares unequal to itself */
	if (x != x)
		return 0.0f;
	if (x > 4.0f)
		return 4.0f;
	if (x < -4.0f)
		return -4.0f;
	return x;
}

void cv_to_float(int fmt, const void *src, float *dst, size_t samples)
{
	const uint8_t *p = src;
	size_t i;

	switch (fmt) {
	case SF_U8:
		for (i = 0; i < samples; i++)
			dst[i] = ((int)p[i] - 128) * (1.0f / 128.0f);
		break;
	case SF_S16LE:
		for (i = 0; i < samples; i++) {
			int16_t v = (int16_t)((uint16_t)p[2 * i] |
					      ((uint16_t)p[2 * i + 1] << 8));
			dst[i] = v * (1.0f / 32768.0f);
		}
		break;
	case SF_S16BE:
		for (i = 0; i < samples; i++) {
			int16_t v = (int16_t)(((uint16_t)p[2 * i] << 8) |
					      (uint16_t)p[2 * i + 1]);
			dst[i] = v * (1.0f / 32768.0f);
		}
		break;
	case SF_S24LE:
		for (i = 0; i < samples; i++) {
			uint32_t u;
			memcpy(&u, p + 4 * i, 4);
			/* sign-extend the low 24 bits, ignore the top byte */
			int32_t v = (int32_t)(u << 8) >> 8;
			dst[i] = v * (1.0f / 8388608.0f);
		}
		break;
	case SF_S20LE:
		for (i = 0; i < samples; i++) {
			uint32_t u;
			memcpy(&u, p + 4 * i, 4);
			int32_t v = (int32_t)(u << 12) >> 12;
			dst[i] = v * (1.0f / 524288.0f);
		}
		break;
	case SF_S24_3LE:
		for (i = 0; i < samples; i++) {
			uint32_t u = (uint32_t)p[3 * i] |
				     ((uint32_t)p[3 * i + 1] << 8) |
				     ((uint32_t)p[3 * i + 2] << 16);
			int32_t v = (int32_t)(u << 8) >> 8;
			dst[i] = v * (1.0f / 8388608.0f);
		}
		break;
	case SF_S32LE:
		for (i = 0; i < samples; i++) {
			int32_t v;
			memcpy(&v, p + 4 * i, 4);
			dst[i] = (float)((double)v * (1.0 / 2147483648.0));
		}
		break;
	case SF_F32LE:
		for (i = 0; i < samples; i++) {
			float v;
			memcpy(&v, p + 4 * i, 4);
			dst[i] = sanitize(v);
		}
		break;
	case SF_F64LE:
		for (i = 0; i < samples; i++) {
			double v;
			memcpy(&v, p + 8 * i, 8);
			if (v != v)
				v = 0.0;
			else if (v > 4.0)
				v = 4.0;
			else if (v < -4.0)
				v = -4.0;
			dst[i] = (float)v;
		}
		break;
	default:
		memset(dst, 0, samples * sizeof(float));
		break;
	}
}

/* ---- encoding ---------------------------------------------------------------- */

void dither_init(struct dither *d, uint32_t seed, int enabled)
{
	d->seed = seed ? seed : 0x9e3779b9u;
	d->enabled = enabled;
}

/* xorshift32: cheap, good enough for dither noise */
static inline uint32_t dither_rand(struct dither *d)
{
	uint32_t x = d->seed;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	d->seed = x;
	return x;
}

/* Triangular noise in (-1, 1) LSB: the difference of two uniforms. */
static inline float dither_tpdf(struct dither *d)
{
	float a = (float)(dither_rand(d) >> 8) * (1.0f / 16777216.0f);
	float b = (float)(dither_rand(d) >> 8) * (1.0f / 16777216.0f);
	return a - b;
}

static inline float clamp1(float x)
{
	if (x != x)
		return 0.0f;
	if (x > 1.0f)
		return 1.0f;
	if (x < -1.0f)
		return -1.0f;
	return x;
}

/* Round to nearest, ties away from zero, inside [lo, hi]. */
static inline int32_t quant(float v, int32_t lo, int32_t hi)
{
	float r = v >= 0.0f ? v + 0.5f : v - 0.5f;
	if (r >= (float)hi)
		return hi;
	if (r <= (float)lo)
		return lo;
	return (int32_t)r;
}

void cv_from_float(int fmt, const float *src, void *dst, size_t samples,
		   struct dither *d)
{
	uint8_t *p = dst;
	size_t i;
	int dith = d && d->enabled;

	switch (fmt) {
	case SF_U8:
		for (i = 0; i < samples; i++) {
			float x = clamp1(src[i]) * 127.0f;
			/* exact digital silence stays silent */
			if (dith && x != 0.0f)
				x += dither_tpdf(d);
			p[i] = (uint8_t)(quant(x, -128, 127) + 128);
		}
		break;
	case SF_S16LE:
	case SF_S16BE:
		for (i = 0; i < samples; i++) {
			float x = clamp1(src[i]) * 32767.0f;
			if (dith && x != 0.0f)
				x += dither_tpdf(d);
			uint16_t u = (uint16_t)(int16_t)quant(x, -32768, 32767);
			if (fmt == SF_S16LE) {
				p[2 * i] = (uint8_t)u;
				p[2 * i + 1] = (uint8_t)(u >> 8);
			} else {
				p[2 * i] = (uint8_t)(u >> 8);
				p[2 * i + 1] = (uint8_t)u;
			}
		}
		break;
	case SF_S24LE:
		for (i = 0; i < samples; i++) {
			int32_t v = quant(clamp1(src[i]) * 8388607.0f,
					  -8388608, 8388607);
			memcpy(p + 4 * i, &v, 4);
		}
		break;
	case SF_S20LE:
		for (i = 0; i < samples; i++) {
			int32_t v = quant(clamp1(src[i]) * 524287.0f, -524288,
					  524287);
			memcpy(p + 4 * i, &v, 4);
		}
		break;
	case SF_S24_3LE:
		for (i = 0; i < samples; i++) {
			int32_t v = quant(clamp1(src[i]) * 8388607.0f,
					  -8388608, 8388607);
			uint32_t u = (uint32_t)v;
			p[3 * i] = (uint8_t)u;
			p[3 * i + 1] = (uint8_t)(u >> 8);
			p[3 * i + 2] = (uint8_t)(u >> 16);
		}
		break;
	case SF_S32LE:
		for (i = 0; i < samples; i++) {
			/* float cannot hold 2^31-1; go through double */
			double x = (double)clamp1(src[i]) * 2147483647.0;
			int32_t v;
			if (x >= 2147483647.0)
				v = 2147483647;
			else if (x <= -2147483648.0)
				v = (int32_t)(-2147483647 - 1);
			else
				v = (int32_t)(x >= 0 ? x + 0.5 : x - 0.5);
			memcpy(p + 4 * i, &v, 4);
		}
		break;
	case SF_F32LE:
		for (i = 0; i < samples; i++) {
			float v = clamp1(src[i]);
			memcpy(p + 4 * i, &v, 4);
		}
		break;
	case SF_F64LE:
		for (i = 0; i < samples; i++) {
			double v = clamp1(src[i]);
			memcpy(p + 8 * i, &v, 8);
		}
		break;
	default:
		break;
	}
}

/* ---- channel layouts -------------------------------------------------------- */

static const uint8_t layouts[DSP_MAX_CH + 1][DSP_MAX_CH] = {
	[1] = { SPK_MONO },
	[2] = { SPK_FL, SPK_FR },
	[3] = { SPK_FL, SPK_FR, SPK_FC },
	[4] = { SPK_FL, SPK_FR, SPK_RL, SPK_RR },
	[5] = { SPK_FL, SPK_FR, SPK_FC, SPK_RL, SPK_RR },
	[6] = { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RL, SPK_RR },
	[7] = { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RC, SPK_SL, SPK_SR },
	[8] = { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RL, SPK_RR, SPK_SL,
		SPK_SR },
};

int spk_layout_pos(int channels, int idx)
{
	if (channels < 1 || channels > DSP_MAX_CH || idx < 0 ||
	    idx >= channels)
		return SPK_MONO;
	return layouts[channels][idx];
}

int spk_side(int pos)
{
	switch (pos) {
	case SPK_FL:
	case SPK_RL:
	case SPK_SL:
		return -1;
	case SPK_FR:
	case SPK_RR:
	case SPK_SR:
		return 1;
	default:
		return 0;
	}
}

#define M3DB 0.70710678f /* -3 dB */

/* index of position `pos' in the layout, or -1 */
static int find_pos(int channels, int pos)
{
	for (int i = 0; i < channels; i++)
		if (layouts[channels][i] == pos)
			return i;
	return -1;
}

/* Add input channel `in_idx' at position `pos' into the matrix for a
 * target layout with at least two channels. */
static void route(struct chmap *cm, int in_idx, int pos)
{
	int oc = cm->out_ch;
	int fl = find_pos(oc, SPK_FL), fr = find_pos(oc, SPK_FR);
	int direct = find_pos(oc, pos);

	if (direct >= 0) {
		cm->m[direct][in_idx] += 1.0f;
		return;
	}

	switch (pos) {
	case SPK_MONO:
		/* mono into stereo and beyond: same signal on both fronts,
		 * at unity, as every player does */
		cm->m[fl][in_idx] += 1.0f;
		cm->m[fr][in_idx] += 1.0f;
		break;
	case SPK_FC:
		cm->m[fl][in_idx] += M3DB;
		cm->m[fr][in_idx] += M3DB;
		break;
	case SPK_LFE:
		/* dropped: the low-frequency effects channel is optional
		 * content, and folding it into small speakers only adds
		 * distortion */
		break;
	case SPK_RL:
	case SPK_RR: {
		int side = find_pos(oc, pos == SPK_RL ? SPK_SL : SPK_SR);
		int rc = find_pos(oc, SPK_RC);
		if (side >= 0)
			cm->m[side][in_idx] += 1.0f;
		else if (rc >= 0)
			cm->m[rc][in_idx] += M3DB;
		else
			cm->m[pos == SPK_RL ? fl : fr][in_idx] += M3DB;
		break;
	}
	case SPK_SL:
	case SPK_SR: {
		int rear = find_pos(oc, pos == SPK_SL ? SPK_RL : SPK_RR);
		if (rear >= 0)
			cm->m[rear][in_idx] += 1.0f;
		else
			cm->m[pos == SPK_SL ? fl : fr][in_idx] += M3DB;
		break;
	}
	case SPK_RC: {
		int rl = find_pos(oc, SPK_RL), rr = find_pos(oc, SPK_RR);
		int sl = find_pos(oc, SPK_SL), sr = find_pos(oc, SPK_SR);
		if (rl >= 0 && rr >= 0) {
			cm->m[rl][in_idx] += M3DB;
			cm->m[rr][in_idx] += M3DB;
		} else if (sl >= 0 && sr >= 0) {
			cm->m[sl][in_idx] += M3DB;
			cm->m[sr][in_idx] += M3DB;
		} else {
			cm->m[fl][in_idx] += 0.5f;
			cm->m[fr][in_idx] += 0.5f;
		}
		break;
	}
	case SPK_FL:
	case SPK_FR:
		/* every layout of two or more channels has both fronts */
		break;
	default:
		break;
	}
}

int chmap_init(struct chmap *cm, int in_ch, int out_ch)
{
	memset(cm, 0, sizeof(*cm));
	if (in_ch < 1 || in_ch > DSP_MAX_CH || out_ch < 1 ||
	    out_ch > DSP_MAX_CH)
		return -1;
	cm->in_ch = in_ch;
	cm->out_ch = out_ch;

	if (in_ch == out_ch) {
		for (int i = 0; i < in_ch; i++)
			cm->m[i][i] = 1.0f;
		cm->identity = 1;
		return 0;
	}

	if (out_ch == 1) {
		/* fold to stereo first, then average the two */
		struct chmap st;
		if (chmap_init(&st, in_ch, 2) < 0)
			return -1;
		for (int i = 0; i < in_ch; i++)
			cm->m[0][i] = 0.5f * (st.m[0][i] + st.m[1][i]);
		/* a mono source through stereo comes back at unity */
		return 0;
	}

	for (int i = 0; i < in_ch; i++)
		route(cm, i, layouts[in_ch][i]);

	/* Normalise rows whose gains add up to more than unity, so that a
	 * full-scale signal on every input stays within full scale. */
	for (int o = 0; o < out_ch; o++) {
		float sum = 0.0f;
		for (int i = 0; i < in_ch; i++)
			sum += fabsf(cm->m[o][i]);
		if (sum > 1.0f + 1e-6f)
			for (int i = 0; i < in_ch; i++)
				cm->m[o][i] /= sum;
	}
	return 0;
}

void chmap_apply(const struct chmap *cm, const float *in, float *out,
		 size_t frames)
{
	int ic = cm->in_ch, oc = cm->out_ch;

	if (cm->identity) {
		if (in != out)
			memmove(out, in, frames * (size_t)ic * sizeof(float));
		return;
	}

	/* fast paths for the two common cases */
	if (ic == 1 && oc == 2) {
		/* walk backwards so in == out works */
		for (size_t f = frames; f-- > 0;) {
			float v = in[f];
			out[2 * f] = v * cm->m[0][0];
			out[2 * f + 1] = v * cm->m[1][0];
		}
		return;
	}
	if (ic == 2 && oc == 1) {
		for (size_t f = 0; f < frames; f++)
			out[f] = in[2 * f] * cm->m[0][0] +
				 in[2 * f + 1] * cm->m[0][1];
		return;
	}

	/* General case, through a frame-sized temporary.  In place
	 * (in == out) is safe only when shrinking, walking forwards; the
	 * pipeline always gives an upmix its own output buffer. */
	for (size_t f = 0; f < frames; f++) {
		float tmpi[DSP_MAX_CH];
		const float *src = in + f * (size_t)ic;
		for (int i = 0; i < ic; i++)
			tmpi[i] = src[i];
		float *dstp = out + f * (size_t)oc;
		for (int o = 0; o < oc; o++) {
			float acc = 0.0f;
			for (int i = 0; i < ic; i++)
				acc += cm->m[o][i] * tmpi[i];
			dstp[o] = acc;
		}
	}
}
