// audiod per-stream signal pipeline: byte FIFO, decode, channel map,
// resample, gain ramps and accumulation into the mixing bus.
//
// A stream's client data lands in its ring as raw bytes in the client's
// format.  Each mixer cycle asks every stream for exactly as many bus
// frames as the device wants; the pipeline pulls whole source frames out of
// the ring on demand, so the ring alone carries the stream's buffering and
// its fill level is what flow control and latency reporting look at.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

/* ---- byte FIFO ---------------------------------------------------------- */

int ring_init(struct bytering *r, size_t size)
{
	memset(r, 0, sizeof(*r));
	if (size == 0)
		return -1;
	r->buf = malloc(size);
	if (!r->buf)
		return -1;
	r->size = size;
	return 0;
}

void ring_init_static(struct bytering *r, const uint8_t *data, size_t len)
{
	memset(r, 0, sizeof(*r));
	r->buf = (uint8_t *)(uintptr_t)data;
	r->size = len;
	r->used = len;
	r->external = 1;
}

void ring_free(struct bytering *r)
{
	if (!r->external)
		free(r->buf);
	memset(r, 0, sizeof(*r));
}

size_t ring_space(const struct bytering *r)
{
	if (r->external)
		return 0;
	return r->size - r->used;
}

size_t ring_write(struct bytering *r, const void *src, size_t len)
{
	const uint8_t *s = src;
	size_t space = ring_space(r);
	if (len > space)
		len = space;
	if (len == 0)
		return 0;
	size_t wr = r->rd + r->used;
	if (wr >= r->size)
		wr -= r->size;
	size_t first = r->size - wr;
	if (first > len)
		first = len;
	memcpy(r->buf + wr, s, first);
	if (len > first)
		memcpy(r->buf, s + first, len - first);
	r->used += len;
	return len;
}

size_t ring_read(struct bytering *r, void *dst, size_t len)
{
	uint8_t *d = dst;
	if (len > r->used)
		len = r->used;
	if (len == 0)
		return 0;
	size_t first = r->size - r->rd;
	if (first > len)
		first = len;
	memcpy(d, r->buf + r->rd, first);
	if (len > first)
		memcpy(d + first, r->buf, len - first);
	r->rd += len;
	if (r->rd >= r->size)
		r->rd -= r->size;
	r->used -= len;
	if (r->used == 0 && !r->external)
		r->rd = 0; /* keep later writes contiguous */
	return len;
}

void ring_clear(struct bytering *r)
{
	if (r->external) {
		/* a static source is "cleared" by consuming it */
		r->rd = r->size;
		r->used = 0;
		return;
	}
	r->rd = 0;
	r->used = 0;
}

/* ---- ramps and volume curve ---------------------------------------------- */

void ramp_init(struct ramp *r, float value)
{
	r->cur = r->target = value;
	r->step = 0.0f;
	r->left = 0;
}

void ramp_set(struct ramp *r, float target, uint32_t frames)
{
	if (frames == 0 || target == r->cur) {
		r->cur = r->target = target;
		r->left = 0;
		r->step = 0.0f;
		return;
	}
	r->target = target;
	r->left = frames;
	r->step = (target - r->cur) / (float)frames;
}

float vol_to_gain(uint32_t v)
{
	if (v == 0)
		return 0.0f;
	if (v >= 100)
		return 1.0f;
	float x = (float)v / 100.0f;
	return x * x * x;
}

/* ---- pipeline ------------------------------------------------------------ */

static uint32_t ramp_frames(uint32_t rate)
{
	uint32_t n = rate / 1000u * DSP_RAMP_MS;
	return n ? n : 1;
}

static int pipeline_setup(struct dsp_stream *s)
{
	int in = s->channels, out = s->out_ch;
	s->rs_ch = in < out ? in : out;
	if (chmap_init(&s->pre, in, s->rs_ch) < 0)
		return -1;
	if (chmap_init(&s->post, s->rs_ch, out) < 0)
		return -1;
	if (rs_init(&s->rs, s->rate, s->out_rate, s->rs_ch, s->quality) < 0)
		return -1;
	return 0;
}

static int alloc_scratch(struct dsp_stream *s)
{
	size_t fl = (size_t)DSP_CHUNK * DSP_MAX_CH;
	s->raw = malloc((size_t)DSP_CHUNK * s->frame_bytes);
	s->tmp_a = malloc(fl * sizeof(float));
	s->tmp_b = malloc(fl * sizeof(float));
	s->tmp_c = malloc(fl * sizeof(float));
	if (!s->raw || !s->tmp_a || !s->tmp_b || !s->tmp_c)
		return -1;
	return 0;
}

static int common_init(struct dsp_stream *s, int fmt, uint32_t rate,
		       int channels, uint32_t out_rate, int out_ch,
		       int quality)
{
	size_t sb = sf_bytes(fmt);
	if (sb == 0 || channels < 1 || channels > DSP_MAX_CH || rate == 0 ||
	    out_rate == 0 || out_ch < 1 || out_ch > DSP_MAX_CH)
		return -1;
	s->fmt = fmt;
	s->rate = rate;
	s->channels = channels;
	s->frame_bytes = sb * (size_t)channels;
	s->out_rate = out_rate;
	s->out_ch = out_ch;
	s->quality = quality;
	if (alloc_scratch(s) < 0)
		return -1;
	if (pipeline_setup(s) < 0)
		return -1;
	ramp_init(&s->fade, 1.0f);
	s->vol_l = s->vol_r = 1.0f;
	for (int c = 0; c < DSP_MAX_CH; c++)
		ramp_init(&s->gain[c], 1.0f);
	return 0;
}

int dsp_stream_init(struct dsp_stream *s, int fmt, uint32_t rate,
		    int channels, size_t ring_bytes, uint32_t out_rate,
		    int out_ch, int quality)
{
	memset(s, 0, sizeof(*s));
	if (common_init(s, fmt, rate, channels, out_rate, out_ch, quality) <
		    0 ||
	    ring_init(&s->ring, ring_bytes) < 0) {
		dsp_stream_free(s);
		return -1;
	}
	return 0;
}

int dsp_stream_init_static(struct dsp_stream *s, int fmt, uint32_t rate,
			   int channels, const uint8_t *data, size_t len,
			   uint32_t out_rate, int out_ch, int quality)
{
	memset(s, 0, sizeof(*s));
	if (common_init(s, fmt, rate, channels, out_rate, out_ch, quality) <
	    0) {
		dsp_stream_free(s);
		return -1;
	}
	ring_init_static(&s->ring, data, len);
	/* all the data is there from the start */
	s->eof = 1;
	return 0;
}

void dsp_stream_free(struct dsp_stream *s)
{
	ring_free(&s->ring);
	rs_free(&s->rs);
	free(s->raw);
	free(s->tmp_a);
	free(s->tmp_b);
	free(s->tmp_c);
	memset(s, 0, sizeof(*s));
}

static void apply_volume(struct dsp_stream *s, uint32_t frames)
{
	for (int c = 0; c < s->out_ch; c++) {
		int side = s->out_ch == 1 ? 0
					  : spk_side(spk_layout_pos(s->out_ch, c));
		float g = side < 0 ? s->vol_l
			  : side > 0 ? s->vol_r
				     : 0.5f * (s->vol_l + s->vol_r);
		if (s->mute)
			g = 0.0f;
		ramp_set(&s->gain[c], g, frames);
	}
}

int dsp_stream_retarget(struct dsp_stream *s, uint32_t out_rate, int out_ch)
{
	if (out_rate == 0 || out_ch < 1 || out_ch > DSP_MAX_CH)
		return -1;
	if (out_rate == s->out_rate && out_ch == s->out_ch)
		return 0;
	rs_free(&s->rs);
	s->out_rate = out_rate;
	s->out_ch = out_ch;
	if (pipeline_setup(s) < 0)
		return -1;
	apply_volume(s, 0);
	return 0;
}

void dsp_stream_set_volume(struct dsp_stream *s, uint32_t left,
			   uint32_t right, int mute)
{
	s->vol_l = vol_to_gain(left);
	s->vol_r = vol_to_gain(right);
	s->mute = mute ? 1 : 0;
	apply_volume(s, ramp_frames(s->out_rate));
}

void dsp_stream_flush(struct dsp_stream *s)
{
	ring_clear(&s->ring);
	rs_reset(&s->rs);
	s->tail_pushed = 0;
	s->tail_done = 0;
}

void dsp_stream_set_eof(struct dsp_stream *s, int on)
{
	if (on) {
		s->eof = 1;
		return;
	}
	s->eof = 0;
	s->tail_pushed = 0;
	s->tail_done = 0;
}

size_t dsp_stream_queued_frames(const struct dsp_stream *s)
{
	if (!s->frame_bytes)
		return 0;
	return s->ring.used / s->frame_bytes;
}

double dsp_stream_pending_frames(const struct dsp_stream *s)
{
	double p = (double)dsp_stream_queued_frames(s);
	if (!s->rs.bypass) {
		/* the filter holds input frames at the source rate after the
		 * pre-map, which does not change the frame count */
		p += rs_pending_in(&s->rs);
	}
	return p;
}

int dsp_stream_finished(const struct dsp_stream *s)
{
	return s->eof && dsp_stream_queued_frames(s) == 0 && s->tail_done;
}

/* Take up to `n' whole source frames from the ring and decode them into
 * `dst' with rs_ch channels.  Returns the frames taken. */
static size_t take_source(struct dsp_stream *s, float *dst, size_t n)
{
	size_t avail = dsp_stream_queued_frames(s);
	if (n > avail)
		n = avail;
	if (n > DSP_CHUNK)
		n = DSP_CHUNK;
	if (n == 0)
		return 0;
	ring_read(&s->ring, s->raw, n * s->frame_bytes);
	if (s->pre.identity) {
		cv_to_float(s->fmt, s->raw, dst, n * (size_t)s->channels);
	} else {
		cv_to_float(s->fmt, s->raw, s->tmp_a,
			    n * (size_t)s->channels);
		chmap_apply(&s->pre, s->tmp_a, dst, n);
	}
	s->in_frames += n;
	return n;
}

/* Produce up to `want' (<= DSP_CHUNK) frames with out_ch channels in
 * s->tmp_c.  Returns how many were produced. */
static size_t produce(struct dsp_stream *s, size_t want)
{
	float *mid = s->post.identity ? s->tmp_c : s->tmp_b;
	size_t produced = 0;

	if (s->rs.bypass) {
		produced = take_source(s, mid, want);
		if (produced < want && s->eof)
			s->tail_done = 1;
	} else {
		/* pull what the history allows; refill it from the ring;
		 * repeat until satisfied or the source is dry */
		for (;;) {
			produced += rs_pull(&s->rs,
					    mid + produced * (size_t)s->rs_ch,
					    want - produced);
			if (produced >= want)
				break;
			size_t need = rs_needed(&s->rs, want - produced);
			size_t space = rs_space(&s->rs);
			if (need == 0)
				need = 1;
			if (need > space)
				need = space;
			if (need > DSP_CHUNK)
				need = DSP_CHUNK;
			if (need == 0)
				break; /* cannot happen: the history drains */
			size_t got = take_source(s, s->tmp_a, need);
			if (got) {
				/* take_source wrote into tmp_a when the pre-map
				 * is identity; otherwise it mapped tmp_a into
				 * tmp_a's own front (shrinking, safe) */
				rs_push(&s->rs, s->tmp_a, got);
				continue;
			}
			if (s->eof && !s->tail_done) {
				/* the source has ended: push the silence that
				 * carries the kernel past the last sample,
				 * once, and pull the rest out */
				if (!s->tail_pushed) {
					size_t t = rs_tail_frames(&s->rs);
					if (t > space)
						t = space;
					rs_push_silence(&s->rs, t);
					s->tail_pushed = 1;
				}
				produced += rs_pull(
					&s->rs,
					mid + produced * (size_t)s->rs_ch,
					want - produced);
				if (produced < want) {
					/* the tail is out; whatever remains
					 * in the history is padding */
					s->tail_done = 1;
					rs_reset(&s->rs);
				}
				break;
			}
			break;
		}
	}

	if (produced && !s->post.identity)
		chmap_apply(&s->post, mid, s->tmp_c, produced);
	return produced;
}

size_t dsp_stream_mix(struct dsp_stream *s, float *acc, size_t frames)
{
	const int oc = s->out_ch;
	size_t done = 0;

	while (done < frames) {
		size_t want = frames - done;
		if (want > DSP_CHUNK)
			want = DSP_CHUNK;
		size_t got = produce(s, want);
		if (got == 0)
			break;

		float *a = acc + done * (size_t)oc;
		const float *x = s->tmp_c;
		int steady = ramp_done(&s->fade);
		for (int c = 0; c < oc && steady; c++)
			steady = ramp_done(&s->gain[c]);

		if (steady) {
			float g[DSP_MAX_CH];
			int silent = 1;
			for (int c = 0; c < oc; c++) {
				g[c] = s->gain[c].cur * s->fade.cur;
				if (g[c] != 0.0f)
					silent = 0;
			}
			if (!silent) {
				if (oc == 2) {
					for (size_t f = 0; f < got; f++) {
						a[2 * f] += x[2 * f] * g[0];
						a[2 * f + 1] += x[2 * f + 1] * g[1];
					}
				} else {
					for (size_t f = 0; f < got; f++)
						for (int c = 0; c < oc; c++)
							a[f * oc + c] +=
								x[f * oc + c] * g[c];
				}
			}
		} else {
			for (size_t f = 0; f < got; f++) {
				float fd = ramp_next(&s->fade);
				for (int c = 0; c < oc; c++)
					a[f * oc + c] += x[f * oc + c] *
							 ramp_next(&s->gain[c]) *
							 fd;
			}
		}
		done += got;
		if (got < want)
			break;
	}
	s->out_frames += done;
	return done;
}
