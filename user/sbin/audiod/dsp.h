// audiod signal processing: sample formats, conversion, channel mapping,
// resampling, per-stream pipelines and the mixing bus.
//
// Everything declared here is plain C with no system dependencies, so the
// host test harness (tests/) compiles the very same sources with the
// sanitizers.  Samples move through the daemon as 32-bit float in the range
// [-1, 1]; the client's format is converted on the way in and the device's
// format on the way out.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef AUDIOD_DSP_H
#define AUDIOD_DSP_H

#include <stddef.h>
#include <stdint.h>

/* ---- sample formats ------------------------------------------------------ */

/* The daemon's own enumeration, independent of both the wire protocol
 * (AUD_FMT_*) and the device interface (AUDIO_FMT_*), which number the
 * formats differently.  Each side maps into this one. */
enum sfmt {
	SF_NONE = 0,
	SF_U8,
	SF_S16LE,
	SF_S16BE,
	SF_S24LE, /* 24 significant bits in the low bits of 32 */
	SF_S24_3LE, /* packed, 3 bytes */
	SF_S20LE, /* 20 significant bits in the low bits of 32 */
	SF_S32LE,
	SF_F32LE,
	SF_F64LE,
	SF_COUNT
};

#define DSP_MAX_CH 8

size_t sf_bytes(int fmt);
const char *sf_name(int fmt);
int sf_from_name(const char *name); /* "s16" ... ; SF_NONE if unknown */
int sf_from_proto(uint32_t aud_fmt); /* AUD_FMT_* -> SF_*, SF_NONE if bad */
uint32_t sf_to_proto(int fmt); /* SF_* -> AUD_FMT_* (0 if none) */

/* ---- conversion (convert.c) --------------------------------------------- */

/* Decode `samples' interleaved samples (frames * channels) from `src' into
 * floats.  `src' needs no particular alignment.  Floating-point input is
 * sanitised: NaN becomes 0 and anything beyond +-4 is clamped, so one bad
 * client cannot poison the shared bus. */
void cv_to_float(int fmt, const void *src, float *dst, size_t samples);

/* Triangular-PDF dither state for the integer outputs of 16 bits or less. */
struct dither {
	uint32_t seed;
	int enabled;
};
void dither_init(struct dither *d, uint32_t seed, int enabled);

/* Encode floats (clamped to [-1, 1]) into a device format. */
void cv_from_float(int fmt, const float *src, void *dst, size_t samples,
		   struct dither *d);

/* Speaker positions used for the channel layouts. */
enum spk_pos {
	SPK_MONO = 0,
	SPK_FL,
	SPK_FR,
	SPK_FC,
	SPK_LFE,
	SPK_RL,
	SPK_RR,
	SPK_SL,
	SPK_SR,
	SPK_RC,
	SPK_COUNT
};

/* Position of channel `idx' in the standard layout of `channels' channels
 * (1 mono, 2 stereo, 3 L/R/C, 4 quad, 5 5.0, 6 5.1, 7 6.1, 8 7.1). */
int spk_layout_pos(int channels, int idx);
/* -1 left side, +1 right side, 0 centre (gets the mean of both volumes). */
int spk_side(int pos);

/* An out x in mixing matrix. */
struct chmap {
	int in_ch, out_ch;
	int identity; /* in_ch == out_ch and the matrix is the unit matrix */
	float m[DSP_MAX_CH][DSP_MAX_CH]; /* m[out][in] */
};

/* Standard up/down-mix between the layouts above.  Downmixes fold centre
 * and surround channels in at -3 dB and normalise each output row so the
 * sum of its coefficients never exceeds unity (no clipping by the matrix
 * itself).  LFE is dropped when the target has no LFE channel. */
int chmap_init(struct chmap *cm, int in_ch, int out_ch);
void chmap_apply(const struct chmap *cm, const float *in, float *out,
		 size_t frames);

/* ---- resampling (resample.c) -------------------------------------------- */

#define RS_QUALITY_FAST 0 /* linear interpolation */
#define RS_QUALITY_GOOD 1 /* 32-tap Kaiser-windowed sinc, ~80 dB */
#define RS_QUALITY_BEST 2 /* 64-tap Kaiser-windowed sinc, ~100 dB */

struct rs_table; /* shared, reference counted polyphase filter bank */

/* Streaming rational resampler.  The conversion ratio is kept as the
 * reduced fraction in_rate/out_rate and the read position as an integer
 * plus a numerator over out_rate/gcd, so an hour of 44.1 kHz becomes
 * exactly the right number of 48 kHz frames: there is no rounding to drift
 * on. */
struct resampler {
	uint32_t in_rate, out_rate;
	uint32_t L, M; /* out/g, in/g: L outputs per M inputs */
	uint32_t step_int, step_frac; /* M / L, M % L */
	int ch;
	int quality;
	int taps; /* filter length in input frames (even) */
	int bypass; /* in_rate == out_rate */
	struct rs_table *tab; /* NULL in linear mode */
	float *hist; /* interleaved input history */
	size_t hist_cap; /* frames */
	size_t hist_len; /* valid frames */
	size_t base; /* first tap of the next output */
	uint32_t frac; /* 0..L-1: fractional position, in 1/L input frames */
	float *wbuf; /* interpolated coefficient row (non-exact tables) */
};

int rs_init(struct resampler *rs, uint32_t in_rate, uint32_t out_rate,
	    int channels, int quality);
void rs_free(struct resampler *rs);
/* Forget all history (flush/seek): back to the initial state. */
void rs_reset(struct resampler *rs);
/* How many input frames the history can take right now. */
size_t rs_space(struct resampler *rs);
/* Append input frames; returns how many were taken (<= rs_space()). */
size_t rs_push(struct resampler *rs, const float *in, size_t frames);
/* Append `frames' frames of silence (used to flush the filter tail). */
size_t rs_push_silence(struct resampler *rs, size_t frames);
/* Produce up to `frames' output frames from the history. */
size_t rs_pull(struct resampler *rs, float *out, size_t frames);
/* Further input frames needed before `out_frames' outputs can be pulled. */
size_t rs_needed(const struct resampler *rs, size_t out_frames);
/* Input frames that are in the history but not yet reflected in output,
 * i.e. the resampler's contribution to latency, in input frames. */
double rs_pending_in(const struct resampler *rs);
/* Silence needed after the last real input for its tail to come out. */
size_t rs_tail_frames(const struct resampler *rs);
/* Release cached filter tables nobody uses (config reload). */
void rs_cache_trim(void);

/* ---- byte FIFO ------------------------------------------------------------ */

struct bytering {
	uint8_t *buf;
	size_t size; /* capacity */
	size_t rd; /* read offset */
	size_t used; /* bytes stored */
	int external; /* buf is not ours (event sound data) */
};

int ring_init(struct bytering *r, size_t size);
/* A read-only ring over existing data (full from the start). */
void ring_init_static(struct bytering *r, const uint8_t *data, size_t len);
void ring_free(struct bytering *r);
size_t ring_space(const struct bytering *r);
size_t ring_write(struct bytering *r, const void *src, size_t len);
size_t ring_read(struct bytering *r, void *dst, size_t len);
void ring_clear(struct bytering *r);

/* ---- gain ramps ----------------------------------------------------------- */

/* Linear gain change spread over a number of frames so that volume
 * steps, mutes and pauses never produce a click. */
struct ramp {
	float cur, target, step;
	uint32_t left;
};

void ramp_init(struct ramp *r, float value);
void ramp_set(struct ramp *r, float target, uint32_t frames);
static inline float ramp_next(struct ramp *r)
{
	if (r->left) {
		r->cur += r->step;
		if (--r->left == 0)
			r->cur = r->target;
	}
	return r->cur;
}
static inline int ramp_done(const struct ramp *r)
{
	return r->left == 0;
}

/* 0..100 -> linear amplitude on a cubic curve (perceptually even steps:
 * 50 is about -18 dB, 10 about -60 dB, 0 is silence). */
float vol_to_gain(uint32_t v);

/* ---- per-stream pipeline (stream.c) --------------------------------------- */

/* Source frames -> float -> (downmix) -> resample -> (upmix) -> gain ->
 * added to the bus.  Downmixing before the resampler and upmixing after it
 * keeps the resampler working on the smaller channel count. */
struct dsp_stream {
	/* source */
	int fmt; /* SF_* */
	uint32_t rate;
	int channels;
	size_t frame_bytes;
	struct bytering ring;

	/* bus */
	uint32_t out_rate;
	int out_ch;

	struct chmap pre; /* in_ch -> rs_ch (identity when not downmixing) */
	struct chmap post; /* rs_ch -> out_ch */
	int rs_ch;
	struct resampler rs;
	int quality;

	/* scratch, sized for DSP_CHUNK frames */
	uint8_t *raw;
	float *tmp_a, *tmp_b, *tmp_c;

	/* volume: per bus channel gain ramps, and a fade used by pause,
	 * resume, kill and shutdown */
	struct ramp gain[DSP_MAX_CH];
	struct ramp fade;
	float vol_l, vol_r; /* linear */
	int mute;

	/* end of data: once the ring is empty the filter tail is flushed */
	int eof;
	int tail_pushed;
	int tail_done;

	/* counters */
	uint64_t in_frames; /* source frames taken from the ring */
	uint64_t out_frames; /* bus frames produced */
};

#define DSP_CHUNK 1024 /* frames processed per internal step */
#define DSP_RAMP_MS 12

int dsp_stream_init(struct dsp_stream *s, int fmt, uint32_t rate,
		    int channels, size_t ring_bytes, uint32_t out_rate,
		    int out_ch, int quality);
/* Same, with a read-only static source (decoded event sound). */
int dsp_stream_init_static(struct dsp_stream *s, int fmt, uint32_t rate,
			   int channels, const uint8_t *data, size_t len,
			   uint32_t out_rate, int out_ch, int quality);
void dsp_stream_free(struct dsp_stream *s);
/* The bus format changed (device reopened at another rate/channels). */
int dsp_stream_retarget(struct dsp_stream *s, uint32_t out_rate, int out_ch);
/* Set the stream volume (0..100 each side) and mute, ramping smoothly. */
void dsp_stream_set_volume(struct dsp_stream *s, uint32_t left,
			   uint32_t right, int mute);
/* Drop buffered source data and filter history. */
void dsp_stream_flush(struct dsp_stream *s);
/* The client announced the end of its data (drain), or sent more after
 * all (on = 0). */
void dsp_stream_set_eof(struct dsp_stream *s, int on);
/* Whole source frames waiting in the ring. */
size_t dsp_stream_queued_frames(const struct dsp_stream *s);
/* Has every sample, including the resampler tail, been rendered? */
int dsp_stream_finished(const struct dsp_stream *s);
/* Latency inside the pipeline, in source frames (ring + filter). */
double dsp_stream_pending_frames(const struct dsp_stream *s);

/* Render `frames' bus frames and ADD them into `acc' (interleaved,
 * out_ch channels).  Returns the number of frames that carried stream data;
 * when it is less than `frames' the stream ran dry for the rest. */
size_t dsp_stream_mix(struct dsp_stream *s, float *acc, size_t frames);

/* ---- the bus (mixer.c) -------------------------------------------------- */

struct mixer {
	uint32_t rate;
	int channels;
	int out_fmt; /* SF_* of the device */
	float *acc; /* frames * channels accumulator */
	size_t cap_frames;

	/* software master volume; 1.0 when the hardware does it */
	struct ramp master[DSP_MAX_CH];
	float master_l, master_r;
	int master_mute;
	int sw_master; /* apply master volume in software */
	struct ramp fade; /* shutdown fade */

	/* peak limiter + soft clipper */
	float lim_gain;
	float lim_release; /* per-frame recovery coefficient */
	uint32_t clipped; /* blocks that needed limiting (statistics) */

	struct dither dither;
};

int mixer_init(struct mixer *m, uint32_t rate, int channels, int out_fmt,
	       size_t max_frames);
void mixer_free(struct mixer *m);
/* Volumes 0..100.  `sw' says whether the mixer applies them (no usable
 * hardware control) or leaves the signal at unity. */
void mixer_set_master(struct mixer *m, uint32_t left, uint32_t right,
		      int mute, int sw);
/* Zero the accumulator for `frames' frames (<= cap_frames). */
float *mixer_begin(struct mixer *m, size_t frames);
/* Apply master gain, limiter, soft clip, convert into `out'. */
void mixer_finish(struct mixer *m, size_t frames, void *out);
/* Start the shutdown fade (ms) and tell when it is complete. */
void mixer_fade_out(struct mixer *m, uint32_t ms);
int mixer_fade_done(const struct mixer *m);

/* Soft clipper on its own (exported for the tests). */
float soft_clip(float x);

#endif /* AUDIOD_DSP_H */
