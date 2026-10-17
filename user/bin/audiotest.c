/* audiotest -- test signals through the system audio service.
 *
 * Generates the signals one needs to check a sound setup and plays them
 * with libaudio:
 *
 *   sine       a steady tone (default 440 Hz)
 *   channels   speaker identification: each channel in turn says its name
 *              with a short voice-like call of its own (different pitch,
 *              vowel and melody per position), so left/right swaps and
 *              surround mix-ups are heard, not just seen
 *   sweep      logarithmic (or linear) frequency sweep, 20 Hz - 20 kHz
 *   white      white noise
 *   pink       pink (1/f) noise, equal energy per octave
 *   square, saw, triangle   band-limited classic waveforms
 *   silence    digital silence (keeps a stream open)
 *   latency    measures the service's latency, the write-to-heard time and
 *              the real clock rate of the device against the system clock
 *
 * Userland may use floating point, so everything is synthesised in float
 * and sent as F32 (or S16 with triangular dither, --s16).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <stdint.h>

#include <audio/audio.h>

#define BLOCK_FRAMES 1024
#define MAX_CHANNELS 8
#define TWO_PI 6.283185307179586
#define FADE_MS 15

static const char *prog = "audiotest";
static volatile sig_atomic_t interrupted;

enum mode {
	M_SINE, M_CHANNELS, M_SWEEP, M_WHITE, M_PINK, M_SQUARE, M_SAW,
	M_TRIANGLE, M_SILENCE, M_LATENCY
};

static const struct {
	const char *name;
	enum mode m;
	double default_secs; /* 0 = mode decides */
	const char *text;
} modes[] = {
	{ "sine", M_SINE, 5, "sine tone" },
	{ "channels", M_CHANNELS, 0, "speaker identification" },
	{ "sweep", M_SWEEP, 10, "frequency sweep" },
	{ "white", M_WHITE, 5, "white noise" },
	{ "pink", M_PINK, 5, "pink noise" },
	{ "square", M_SQUARE, 5, "square wave" },
	{ "saw", M_SAW, 5, "sawtooth wave" },
	{ "triangle", M_TRIANGLE, 5, "triangle wave" },
	{ "silence", M_SILENCE, 5, "silence" },
	{ "latency", M_LATENCY, 5, "latency measurement" },
};

static struct {
	enum mode mode;
	double freq, freq_to;
	int freq_given, freq_to_given;
	double amp; /* linear peak */
	double secs; /* < 0: default, 0: forever */
	int channel; /* 0 = all, else 1-based */
	uint32_t channels, rate;
	uint32_t volume;
	int have_volume;
	uint32_t role;
	uint32_t buffer_ms;
	uint32_t rounds; /* channels mode, 0 = forever */
	int linear;
	int s16;
	int quiet;
} opt = {
	.mode = M_SINE,
	.freq = 440,
	.freq_to = 20000,
	.amp = 0.25, /* -12 dBFS: loud enough, never painful */
	.secs = -1,
	.channels = 2,
	.rate = 48000,
	.role = AUD_ROLE_OTHER,
	.rounds = 1,
};

/* ---- output --------------------------------------------------------------- */

static la_stream *st;
static float *fbuf; /* BLOCK_FRAMES * channels */
static int16_t *sbuf;
static uint64_t frames_out;

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static inline uint64_t rng_next(void)
{
	/* xorshift64*: fast, good enough for noise and dither */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545f4914f6cdd1dull;
}

static inline float rng_uniform(void) /* [-1, 1) */
{
	return (float)((int64_t)(rng_next() >> 11) - (1ll << 52)) /
	       (float)(1ll << 52);
}

static void on_signal(int sig)
{
	if (interrupted)
		_exit(128 + sig);
	interrupted = sig;
}

/* Write `frames' frames from fbuf, converting when S16 was asked for. */
static int emit(uint32_t frames)
{
	const void *p = fbuf;
	size_t bytes, off = 0;
	uint32_t i, n = frames * opt.channels;

	if (opt.s16) {
		for (i = 0; i < n; i++) {
			/* TPDF dither of one LSB, then round and clip. */
			float v = fbuf[i] * 32767.0f +
				  (rng_uniform() + rng_uniform()) * 0.5f;
			if (v > 32767.0f)
				v = 32767.0f;
			if (v < -32768.0f)
				v = -32768.0f;
			sbuf[i] = (int16_t)lrintf(v);
		}
		p = sbuf;
		bytes = (size_t)n * 2;
	} else {
		bytes = (size_t)n * 4;
	}
	while (off < bytes && !interrupted) {
		ssize_t w = la_write(st, (const uint8_t *)p + off, bytes - off);
		if (w == -EINTR || w == -EAGAIN)
			continue;
		if (w < 0) {
			fprintf(stderr, "\n%s: playback failed: %s\n", prog,
				la_strerror((int)w));
			return -1;
		}
		off += (size_t)w;
	}
	frames_out += frames;
	return interrupted ? -1 : 0;
}

/* Fill one frame's channels: all of them, or only the chosen one. */
static inline void put(uint32_t frame, float v)
{
	uint32_t c;
	float *f = fbuf + (size_t)frame * opt.channels;

	if (opt.channel) {
		for (c = 0; c < opt.channels; c++)
			f[c] = 0.0f;
		f[opt.channel - 1] = v;
	} else {
		for (c = 0; c < opt.channels; c++)
			f[c] = v;
	}
}

/* Gain of a raised-cosine fade at both ends of a finite signal. */
static inline float fade_gain(uint64_t i, uint64_t total, uint64_t fade)
{
	double x;

	if (fade == 0)
		return 1.0f;
	if (i < fade) {
		x = (double)i / (double)fade;
		return (float)(0.5 - 0.5 * cos(x * M_PI));
	}
	if (total && i + fade >= total) {
		x = (double)(total - i) / (double)fade;
		if (x < 0)
			x = 0;
		return (float)(0.5 - 0.5 * cos(x * M_PI));
	}
	return 1.0f;
}

/* ---- band-limited oscillators ------------------------------------------------ */

/* polyBLEP: a two-sample polynomial correction around each discontinuity
 * of a naive sawtooth/square removes most of the aliasing. */
static inline double poly_blep(double t, double dt)
{
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.0;
	}
	if (t > 1.0 - dt) {
		t = (t - 1.0) / dt;
		return t * t + t + t + 1.0;
	}
	return 0.0;
}

struct osc {
	double phase; /* 0..1 */
};

static double osc_sample(struct osc *o, enum mode m, double freq)
{
	double dt = freq / opt.rate, v;

	switch (m) {
	case M_SAW:
		v = 2.0 * o->phase - 1.0;
		v -= poly_blep(o->phase, dt);
		break;
	case M_SQUARE: {
		double t2 = o->phase + 0.5;
		if (t2 >= 1.0)
			t2 -= 1.0;
		v = o->phase < 0.5 ? 1.0 : -1.0;
		v += poly_blep(o->phase, dt);
		v -= poly_blep(t2, dt);
		break;
	}
	case M_TRIANGLE:
		/* The triangle is continuous and its harmonics fall with
		 * 1/k^2, so the plain shape aliases very little.  Starts at
		 * zero going up, like the sine. */
		v = o->phase < 0.25 ? 4.0 * o->phase :
		    o->phase < 0.75 ? 2.0 - 4.0 * o->phase :
				      4.0 * o->phase - 4.0;
		break;
	default:
		v = sin(TWO_PI * o->phase);
	}
	o->phase += dt;
	if (o->phase >= 1.0)
		o->phase -= floor(o->phase);
	return v;
}

/* ---- pink noise (Voss-McCartney) --------------------------------------------- */

#define PINK_ROWS 16

static struct {
	float rows[PINK_ROWS];
	float sum;
	uint32_t counter;
} pink;

static float pink_sample(void)
{
	float w;

	/* Row k changes every 2^k samples: the row is picked by the count of
	 * trailing zeros of a running counter, so each sample updates one
	 * row and the sum keeps 1/f weighting. */
	pink.counter++;
	if (pink.counter) {
		int k = __builtin_ctz(pink.counter);
		if (k < PINK_ROWS) {
			float nv = rng_uniform();
			pink.sum += nv - pink.rows[k];
			pink.rows[k] = nv;
		}
	}
	w = rng_uniform();
	return (pink.sum + w) / (float)(PINK_ROWS + 1) * 3.0f;
}

/* ---- continuous signals ------------------------------------------------------- */

static int run_continuous(void)
{
	uint64_t total = opt.secs > 0 ? (uint64_t)(opt.secs * opt.rate) : 0;
	uint64_t fade = (uint64_t)opt.rate * FADE_MS / 1000, i = 0;
	struct osc o = { 0 };
	double f0 = opt.freq, f1 = opt.freq_to, freq = f0;
	double ratio = 0;
	time_t last_print = 0;

	if (opt.mode == M_SWEEP) {
		if (!opt.freq_given)
			f0 = 20;
		freq = f0;
		if (!total) {
			fprintf(stderr, "%s: a sweep needs a duration\n", prog);
			return 2;
		}
		ratio = log(f1 / f0);
	}

	while (!interrupted && (total == 0 || i < total)) {
		uint32_t n = BLOCK_FRAMES, k;
		if (total && total - i < n)
			n = (uint32_t)(total - i);
		for (k = 0; k < n; k++, i++) {
			double v;
			switch (opt.mode) {
			case M_WHITE:
				v = rng_uniform();
				break;
			case M_PINK:
				v = pink_sample();
				if (v > 1.0) v = 1.0;
				if (v < -1.0) v = -1.0;
				break;
			case M_SILENCE:
				v = 0;
				break;
			case M_SWEEP: {
				double x = (double)i / (double)total;
				freq = opt.linear ? f0 + (f1 - f0) * x : f0 * exp(ratio * x);
				v = osc_sample(&o, M_SINE, freq);
				break;
			}
			default:
				v = osc_sample(&o, opt.mode, freq);
			}
			put(k, (float)(v * opt.amp) * fade_gain(i, total, fade));
		}
		if (emit(n))
			break;
		if (opt.mode == M_SWEEP && !opt.quiet) {
			time_t now = time(NULL);
			if (now != last_print) {
				last_print = now;
				fprintf(stderr, "\r  %7.0f Hz  ", freq);
			}
		}
	}
	if (opt.mode == M_SWEEP && !opt.quiet)
		fputc('\n', stderr);
	return 0;
}

/* ---- speaker identification ------------------------------------------------ */

struct formant {
	float f, bw, gain;
};

/* Vowel colours by their first three formants (average adult voice). */
static const struct formant vowels[][3] = {
	/* a */ { { 750, 90, 1.0f }, { 1200, 110, 0.6f }, { 2600, 160, 0.25f } },
	/* e */ { { 450, 70, 1.0f }, { 1950, 110, 0.5f }, { 2600, 160, 0.3f } },
	/* i */ { { 300, 60, 1.0f }, { 2300, 120, 0.45f }, { 3000, 180, 0.3f } },
	/* o */ { { 500, 80, 1.0f }, { 850, 90, 0.55f }, { 2500, 160, 0.2f } },
	/* u */ { { 320, 60, 1.0f }, { 800, 80, 0.4f }, { 2400, 160, 0.15f } },
};
enum { V_A, V_E, V_I, V_O, V_U };

struct syllable {
	uint8_t vowel;
	float f0_from, f0_to; /* intonation of the syllable */
	float secs;
};

struct callsign {
	const char *code, *name;
	int lfe;
	int nsyl;
	struct syllable syl[3];
};

/* One call per speaker position.  Left positions use a falling melody,
 * right ones a rising one, centres a level one; front/side/back differ in
 * vowel and register, so each is recognisable without looking. */
static const struct callsign calls[] = {
	{ "FL", "Front Left", 0, 2, { { V_O, 200, 190, 0.22f }, { V_A, 190, 160, 0.26f } } },
	{ "FR", "Front Right", 0, 2, { { V_O, 200, 210, 0.22f }, { V_I, 220, 260, 0.26f } } },
	{ "FC", "Front Center", 0, 2, { { V_E, 210, 210, 0.22f }, { V_E, 210, 205, 0.26f } } },
	{ "LFE", "Subwoofer", 1, 0, { { 0, 0, 0, 0 } } },
	{ "BL", "Rear Left", 0, 3, { { V_U, 160, 150, 0.18f }, { V_A, 150, 140, 0.18f }, { V_O, 140, 120, 0.24f } } },
	{ "BR", "Rear Right", 0, 3, { { V_U, 160, 170, 0.18f }, { V_A, 170, 185, 0.18f }, { V_I, 185, 220, 0.24f } } },
	{ "SL", "Side Left", 0, 2, { { V_A, 260, 240, 0.2f }, { V_U, 240, 200, 0.26f } } },
	{ "SR", "Side Right", 0, 2, { { V_A, 260, 280, 0.2f }, { V_I, 280, 330, 0.26f } } },
	{ "BC", "Rear Center", 0, 2, { { V_U, 170, 170, 0.22f }, { V_O, 170, 165, 0.26f } } },
	{ "M", "Mono", 0, 2, { { V_O, 200, 200, 0.22f }, { V_O, 200, 180, 0.26f } } },
};

static const struct callsign *call_by_code(const char *code)
{
	size_t i;

	for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
		if (!strcmp(calls[i].code, code))
			return &calls[i];
	return &calls[sizeof(calls) / sizeof(calls[0]) - 1];
}

/* The conventional channel order for each count. */
static const char *const layouts[MAX_CHANNELS + 1][MAX_CHANNELS] = {
	{ NULL },
	{ "M" },
	{ "FL", "FR" },
	{ "FL", "FR", "FC" },
	{ "FL", "FR", "BL", "BR" },
	{ "FL", "FR", "FC", "BL", "BR" },
	{ "FL", "FR", "FC", "LFE", "BL", "BR" },
	{ "FL", "FR", "FC", "LFE", "BC", "SL", "SR" },
	{ "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR" },
};

static int parse_channel(const char *s, int *out)
{
	char *end;
	long v;
	uint32_t i;

	v = strtol(s, &end, 10);
	if (!*end && end != s) {
		if (v < 1 || v > (long)opt.channels)
			return -1;
		*out = (int)v;
		return 0;
	}
	for (i = 0; i < opt.channels; i++) {
		const struct callsign *c = call_by_code(layouts[opt.channels][i]);
		if (!strcasecmp(s, c->code) || !strcasecmp(s, c->name)) {
			*out = (int)i + 1;
			return 0;
		}
	}
	if (!strcasecmp(s, "left") && opt.channels >= 2) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(s, "right") && opt.channels >= 2) {
		*out = 2;
		return 0;
	}
	return -1;
}

/* Resonance of one formant at frequency f (a simple Lorentzian). */
static inline double formant_gain(const struct formant *fm, double f)
{
	double x = (f - fm->f) / fm->bw;
	return fm->gain / (1.0 + x * x);
}

/* Play one call on channel `ch' (0-based); all others silent. */
static int play_call(uint32_t ch, const struct callsign *cs)
{
	uint32_t r = opt.rate, k, c;
	int s;

	/* The subwoofer gets two low thumps instead of a voice. */
	if (cs->lfe) {
		int b;
		for (b = 0; b < 2 && !interrupted; b++) {
			uint64_t n = r * 35 / 100, i;
			double ph = 0;
			for (i = 0; i < n && !interrupted;) {
				uint32_t m = BLOCK_FRAMES;
				if (n - i < m)
					m = (uint32_t)(n - i);
				for (k = 0; k < m; k++, i++) {
					double t = (double)i / r;
					double f = 45.0 + 20.0 * (1.0 - t / 0.35);
					double env = fade_gain(i, n, r / 25) * exp(-t * 3.0);
					float *fr = fbuf + (size_t)k * opt.channels;
					ph += TWO_PI * f / r;
					for (c = 0; c < opt.channels; c++)
						fr[c] = 0;
					fr[ch] = (float)(sin(ph) * env * opt.amp * 2.5);
				}
				if (emit(m))
					return -1;
			}
			/* gap between thumps */
			memset(fbuf, 0, sizeof(float) * BLOCK_FRAMES * opt.channels);
			for (i = 0; i < r / 8; i += BLOCK_FRAMES)
				if (emit(BLOCK_FRAMES))
					return -1;
		}
		return 0;
	}

	for (s = 0; s < cs->nsyl && !interrupted; s++) {
		const struct syllable *sy = &cs->syl[s];
		const struct formant *fm = vowels[sy->vowel];
		uint64_t n = (uint64_t)(sy->secs * r), i;
		uint64_t att = r / 50, rel = r / 12;
		double phase = 0;

		for (i = 0; i < n && !interrupted;) {
			uint32_t m = BLOCK_FRAMES;
			if (n - i < m)
				m = (uint32_t)(n - i);
			for (k = 0; k < m; k++, i++) {
				double t = (double)i / (double)n;
				/* intonation glide plus a little vibrato */
				double f0 = sy->f0_from + (sy->f0_to - sy->f0_from) * t;
				double v = 0, env;
				int h;
				float *fr = fbuf + (size_t)k * opt.channels;

				f0 *= 1.0 + 0.012 * sin(TWO_PI * 5.5 * i / r);
				phase += f0 / r;
				if (phase >= 1.0)
					phase -= floor(phase);
				/* Harmonics of a glottal-like source (1/h) shaped
				 * by the vowel's formants. */
				for (h = 1; h <= 40; h++) {
					double fh = f0 * h, g;
					if (fh > 4500.0 || fh > r * 0.45)
						break;
					g = formant_gain(&fm[0], fh) + formant_gain(&fm[1], fh) +
					    formant_gain(&fm[2], fh);
					v += g / h * sin(TWO_PI * phase * h);
				}
				if (i < att)
					env = (double)i / att;
				else if (n - i < rel)
					env = (double)(n - i) / rel;
				else
					env = 1.0;
				for (c = 0; c < opt.channels; c++)
					fr[c] = 0;
				fr[ch] = (float)(v * env * opt.amp * 1.6);
				if (fr[ch] > 1.0f)
					fr[ch] = 1.0f;
				if (fr[ch] < -1.0f)
					fr[ch] = -1.0f;
			}
			if (emit(m))
				return -1;
		}
		/* short pause between syllables */
		memset(fbuf, 0, sizeof(float) * BLOCK_FRAMES * opt.channels);
		if (emit((uint32_t)(r * 6 / 100 < BLOCK_FRAMES ? r * 6 / 100 : BLOCK_FRAMES)))
			return -1;
	}
	return 0;
}

static int run_channels(void)
{
	uint32_t round = 0, c;

	if (!opt.quiet)
		fprintf(stderr, "Speaker test: %u channel%s, each one calls in turn%s\n",
			opt.channels, opt.channels == 1 ? "" : "s",
			opt.rounds == 0 ? " (Ctrl-C to stop)" : "");
	while (!interrupted && (opt.rounds == 0 || round < opt.rounds)) {
		for (c = 0; c < opt.channels && !interrupted; c++) {
			const struct callsign *cs;
			uint64_t lat = 0, played = 0;
			uint32_t i;

			if (opt.channel && (int)c + 1 != opt.channel)
				continue;
			cs = call_by_code(layouts[opt.channels][c]);
			/* Announce when the call is actually heard: wait out
			 * what is still queued first. */
			if (la_latency(st, &lat, &played) >= 0 && lat > 0)
				(void)la_drain(st);
			if (!opt.quiet) {
				fprintf(stderr, "  %u  %-4s %s\n", c + 1, cs->code, cs->name);
				fflush(stderr);
			}
			if (play_call(c, cs))
				return interrupted ? 0 : 1;
			/* pause between speakers */
			memset(fbuf, 0, sizeof(float) * BLOCK_FRAMES * opt.channels);
			for (i = 0; i < opt.rate / 2; i += BLOCK_FRAMES)
				if (emit(BLOCK_FRAMES))
					return interrupted ? 0 : 1;
		}
		round++;
	}
	return 0;
}

/* ---- latency ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int run_latency(void)
{
	double secs = opt.secs > 0 ? opt.secs : 5;
	uint64_t total = (uint64_t)(secs * opt.rate), i = 0;
	uint64_t t0 = 0, p0 = 0, t1 = 0, p1 = 0;
	uint64_t lat_min = UINT64_MAX, lat_max = 0, lat_sum = 0, nlat = 0;
	uint64_t next_sample = 0;
	struct osc o = { 0 };
	la_ctl *ctl;
	int err, trial;
	uint64_t rt_min = UINT64_MAX, rt_max = 0, rt_sum = 0, nrt = 0;

	ctl = la_ctl_open(prog, &err);
	if (ctl) {
		struct aud_server_info si;
		if (la_get_server_info(ctl, &si) >= 0 && !opt.quiet && si.rate) {
			uint64_t dev_us = (uint64_t)si.period_frames * si.periods *
					  1000000ull / si.rate;
			printf("Mixer:          %u Hz, %u ch, period %u frames x %u "
			       "(%llu.%llu ms device buffer)\n",
			       si.rate, si.channels, si.period_frames, si.periods,
			       (unsigned long long)(dev_us / 1000),
			       (unsigned long long)(dev_us % 1000 / 100));
		}
		la_ctl_close(ctl);
	}
	if (!opt.quiet)
		printf("Stream:         %u Hz, %u ch, buffer %s%u ms\n", opt.rate,
		       opt.channels, opt.buffer_ms ? "" : "default ~",
		       opt.buffer_ms ? opt.buffer_ms : 100);

	/* Phase 1: steady playback of a quiet tone; sample the reported
	 * latency every 50 ms and the played-frames counter against the
	 * monotonic clock. */
	if (!opt.quiet) {
		printf("Measuring steady-state latency for %.1f s...\n", secs);
		fflush(stdout);
	}
	while (!interrupted && i < total) {
		uint32_t n = BLOCK_FRAMES, k;
		if (total - i < n)
			n = (uint32_t)(total - i);
		for (k = 0; k < n; k++, i++)
			put(k, (float)(osc_sample(&o, M_SINE, 1000) * opt.amp * 0.25) *
				       fade_gain(i, total, opt.rate / 50));
		if (emit(n))
			break;
		if (i >= next_sample) {
			uint64_t lat, played;
			next_sample = i + opt.rate / 20;
			if (la_latency(st, &lat, &played) >= 0) {
				uint64_t t = now_ns();
				if (lat < lat_min) lat_min = lat;
				if (lat > lat_max) lat_max = lat;
				lat_sum += lat;
				nlat++;
				/* skip the first half second: start-up */
				if (i > opt.rate / 2) {
					if (!t0 && played) {
						t0 = t;
						p0 = played;
					} else if (t0) {
						t1 = t;
						p1 = played;
					}
				}
			}
		}
	}
	if (interrupted)
		return 0;

	/* Phase 2: from an empty stream, write a 10 ms click and time how
	 * long until the service says it was heard (la_drain). */
	if (!opt.quiet) {
		printf("Measuring write-to-heard time...\n");
		fflush(stdout);
	}
	(void)la_drain(st);
	for (trial = 0; trial < 8 && !interrupted; trial++) {
		uint32_t n = opt.rate / 100, k;
		uint64_t t, dt;
		if (n > BLOCK_FRAMES)
			n = BLOCK_FRAMES;
		for (k = 0; k < n; k++)
			put(k, (float)(sin(TWO_PI * 2000.0 * k / opt.rate) * opt.amp *
				       fade_gain(k, n, n / 4)));
		t = now_ns();
		if (emit(n))
			break;
		if (la_drain(st) < 0)
			break;
		dt = (now_ns() - t) / 1000;
		if (trial == 0)
			continue; /* the first one wakes things up */
		if (dt < rt_min) rt_min = dt;
		if (dt > rt_max) rt_max = dt;
		rt_sum += dt;
		nrt++;
		{
			struct timespec ts = { 0, 150000000 };
			nanosleep(&ts, NULL);
		}
	}
	if (interrupted)
		return 0;

	printf("\nResults\n");
	if (nlat)
		printf("  reported latency    min %llu.%llu ms, avg %llu.%llu ms, max %llu.%llu ms (%llu samples)\n",
		       (unsigned long long)(lat_min / 1000), (unsigned long long)(lat_min % 1000 / 100),
		       (unsigned long long)(lat_sum / nlat / 1000),
		       (unsigned long long)(lat_sum / nlat % 1000 / 100),
		       (unsigned long long)(lat_max / 1000), (unsigned long long)(lat_max % 1000 / 100),
		       (unsigned long long)nlat);
	else
		printf("  reported latency    not available\n");
	if (nrt)
		printf("  write-to-heard      min %llu.%llu ms, avg %llu.%llu ms, max %llu.%llu ms (%llu clicks)\n",
		       (unsigned long long)(rt_min / 1000), (unsigned long long)(rt_min % 1000 / 100),
		       (unsigned long long)(rt_sum / nrt / 1000),
		       (unsigned long long)(rt_sum / nrt % 1000 / 100),
		       (unsigned long long)(rt_max / 1000), (unsigned long long)(rt_max % 1000 / 100),
		       (unsigned long long)nrt);
	if (t0 && t1 > t0 + 1000000000ull && p1 > p0) {
		/* frames per second actually played, by the system clock */
		double rate = (double)(p1 - p0) * 1e9 / (double)(t1 - t0);
		double ppm = (rate / opt.rate - 1.0) * 1e6;
		printf("  measured rate       %.1f Hz (%+.0f ppm against the system clock)\n",
		       rate, ppm);
		if (ppm > 2000 || ppm < -2000)
			printf("  note: more than 0.2%% off -- the reported position or the "
			       "device clock is unreliable\n");
	} else {
		printf("  measured rate       not enough data (play longer with -t)\n");
	}
	return 0;
}

/* ---- arguments --------------------------------------------------------------- */

static int parse_double(const char *s, double lo, double hi, double *out)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (end == s || errno)
		return -1;
	if ((*end == 'k' || *end == 'K') && end[1] == 0) {
		v *= 1000.0;
		end++;
	}
	if (*end || !(v >= lo && v <= hi))
		return -1;
	*out = v;
	return 0;
}

/* Amplitude: "0.5", "50%", "-6dB" / "-6 dBFS". */
static int parse_amp(const char *s, double *out)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (end == s || errno)
		return -1;
	while (*end == ' ')
		end++;
	if (!strcasecmp(end, "db") || !strcasecmp(end, "dbfs")) {
		if (v > 0 || v < -120)
			return -1;
		*out = pow(10.0, v / 20.0);
		return 0;
	}
	if (!strcmp(end, "%")) {
		if (v < 0 || v > 100)
			return -1;
		*out = v / 100.0;
		return 0;
	}
	if (*end || v < 0 || v > 1)
		return -1;
	*out = v;
	return 0;
}

static int parse_uint(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (!isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v < lo || v > hi)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static int parse_mode(const char *s, enum mode *m)
{
	size_t i;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (!strcasecmp(s, modes[i].name)) {
			*m = modes[i].m;
			return 0;
		}
	if (!strcasecmp(s, "speakers") || !strcasecmp(s, "speaker") ||
	    !strcasecmp(s, "ident")) {
		*m = M_CHANNELS;
		return 0;
	}
	if (!strcasecmp(s, "noise")) {
		*m = M_WHITE;
		return 0;
	}
	if (!strcasecmp(s, "tone")) {
		*m = M_SINE;
		return 0;
	}
	return -1;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: %s [MODE] [options]\n"
		"\n"
		"Modes: sine (default), channels, sweep, white, pink, square, saw,\n"
		"       triangle, silence, latency\n"
		"\n"
		"  -m, --mode MODE        the signal (or give it as the first word)\n"
		"  -f, --freq HZ          frequency (sine/square/saw/triangle: 440;\n"
		"                         sweep start: 20)\n"
		"  -F, --to HZ            sweep end frequency (20000)\n"
		"      --linear           linear instead of logarithmic sweep\n"
		"  -a, --amplitude A      peak level: 0..1, N%%, or -N dB (default -12 dB)\n"
		"  -t, --time SECONDS     duration, 0 = until Ctrl-C (default 5, sweep 10)\n"
		"  -c, --channel N|NAME   play on one channel only (1-based, or FL, FR, ...)\n"
		"  -n, --channels N       stream channels 1..8 (default 2)\n"
		"  -r, --rate HZ          sample rate (default 48000)\n"
		"  -L, --rounds N         channels mode: rounds, 0 = forever (default 1)\n"
		"  -v, --volume N         stream volume 0..100\n"
		"  -b, --buffer MS        buffering wanted from the service\n"
		"      --s16              send 16-bit dithered samples instead of float\n"
		"  -q, --quiet            no messages\n"
		"  -h, --help             this text\n",
		prog);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "freq", required_argument, NULL, 'f' },
		{ "frequency", required_argument, NULL, 'f' },
		{ "to", required_argument, NULL, 'F' },
		{ "linear", no_argument, NULL, 1 },
		{ "amplitude", required_argument, NULL, 'a' },
		{ "time", required_argument, NULL, 't' },
		{ "channel", required_argument, NULL, 'c' },
		{ "channels", required_argument, NULL, 'n' },
		{ "rate", required_argument, NULL, 'r' },
		{ "rounds", required_argument, NULL, 'L' },
		{ "volume", required_argument, NULL, 'v' },
		{ "buffer", required_argument, NULL, 'b' },
		{ "s16", no_argument, NULL, 2 },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const char *channel_arg = NULL;
	la_format fmt;
	la_stream_options so;
	struct sigaction sa;
	char name[AUDIOD_NAME_MAX];
	int c, err, rc = 0;
	size_t i;

	if (argv[0] && argv[0][0]) {
		const char *s = strrchr(argv[0], '/');
		prog = s ? s + 1 : argv[0];
	}

	while (optind < argc) {
		if (argv[optind][0] != '-' || argv[optind][1] == 0) {
			if (parse_mode(argv[optind], &opt.mode)) {
				fprintf(stderr, "%s: unknown mode '%s'\n", prog, argv[optind]);
				return 2;
			}
			optind++;
			continue;
		}
		if (!strcmp(argv[optind], "--")) {
			optind++;
			continue;
		}
		c = getopt_long(argc, argv, "m:f:F:a:t:c:n:r:L:v:b:qh", longopts, NULL);
		if (c == -1) {
			optind++;
			continue;
		}
		switch (c) {
		case 'm':
			if (parse_mode(optarg, &opt.mode)) {
				fprintf(stderr, "%s: unknown mode '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'f':
			if (parse_double(optarg, 1, 192000, &opt.freq)) {
				fprintf(stderr, "%s: bad frequency '%s'\n", prog, optarg);
				return 2;
			}
			opt.freq_given = 1;
			break;
		case 'F':
			if (parse_double(optarg, 1, 192000, &opt.freq_to)) {
				fprintf(stderr, "%s: bad frequency '%s'\n", prog, optarg);
				return 2;
			}
			opt.freq_to_given = 1;
			break;
		case 1: opt.linear = 1; break;
		case 2: opt.s16 = 1; break;
		case 'a':
			if (parse_amp(optarg, &opt.amp)) {
				fprintf(stderr, "%s: bad amplitude '%s' (0..1, N%%, -N dB)\n",
					prog, optarg);
				return 2;
			}
			break;
		case 't':
			if (parse_double(optarg, 0, 86400, &opt.secs)) {
				fprintf(stderr, "%s: bad duration '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'c': channel_arg = optarg; break;
		case 'n':
			if (parse_uint(optarg, 1, MAX_CHANNELS, &opt.channels)) {
				fprintf(stderr, "%s: bad channel count '%s' (1..%d)\n", prog,
					optarg, MAX_CHANNELS);
				return 2;
			}
			break;
		case 'r':
			if (parse_uint(optarg, 8000, 384000, &opt.rate)) {
				fprintf(stderr, "%s: bad rate '%s' (8000..384000)\n", prog,
					optarg);
				return 2;
			}
			break;
		case 'L':
			if (parse_uint(optarg, 0, 100000, &opt.rounds)) {
				fprintf(stderr, "%s: bad round count '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'v':
			if (parse_uint(optarg, 0, 100, &opt.volume)) {
				fprintf(stderr, "%s: bad volume '%s'\n", prog, optarg);
				return 2;
			}
			opt.have_volume = 1;
			break;
		case 'b':
			if (parse_uint(optarg, 1, 10000, &opt.buffer_ms)) {
				fprintf(stderr, "%s: bad buffer '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'q': opt.quiet = 1; break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	/* Checks that need every option. */
	if (channel_arg && parse_channel(channel_arg, &opt.channel)) {
		fprintf(stderr, "%s: no channel '%s' in a %u-channel stream\n", prog,
			channel_arg, opt.channels);
		return 2;
	}
	if (opt.secs < 0) {
		for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
			if (modes[i].m == opt.mode)
				opt.secs = modes[i].default_secs;
	}
	if (opt.mode == M_SWEEP) {
		double from = opt.freq_given ? opt.freq : 20;
		if (opt.freq_to > opt.rate / 2.0)
			opt.freq_to = opt.rate / 2.0 * 0.95;
		if (from >= opt.freq_to) {
			fprintf(stderr, "%s: the sweep must rise (%.0f -> %.0f Hz)\n", prog,
				from, opt.freq_to);
			return 2;
		}
		if (opt.secs == 0) {
			fprintf(stderr, "%s: a sweep needs a duration\n", prog);
			return 2;
		}
	} else if (opt.freq >= opt.rate / 2.0 &&
		   (opt.mode == M_SINE || opt.mode == M_SQUARE || opt.mode == M_SAW ||
		    opt.mode == M_TRIANGLE)) {
		fprintf(stderr, "%s: %.0f Hz cannot be played at %u Hz (limit %u Hz)\n",
			prog, opt.freq, opt.rate, opt.rate / 2);
		return 2;
	}
	(void)opt.freq_to_given;

	fbuf = calloc((size_t)BLOCK_FRAMES * opt.channels, sizeof(float));
	sbuf = calloc((size_t)BLOCK_FRAMES * opt.channels, sizeof(int16_t));
	if (!fbuf || !sbuf) {
		fprintf(stderr, "%s: out of memory\n", prog);
		return 1;
	}
	rng_state ^= (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ull ^ (uint64_t)getpid();
	if (!rng_state)
		rng_state = 1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.format = opt.s16 ? AUD_FMT_S16_LE : AUD_FMT_F32_LE;
	fmt.rate = opt.rate;
	fmt.channels = opt.channels;
	memset(&so, 0, sizeof(so));
	so.role = opt.role;
	so.buffer_ms = opt.buffer_ms;
	if (opt.have_volume) {
		so.volume = opt.volume;
		if (!opt.volume)
			so.flags |= LA_OPT_VOLUME_ZERO;
	}
	so.app_name = prog;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (modes[i].m == opt.mode)
			snprintf(name, sizeof(name), "%s", modes[i].text);
	so.stream_name = name;

	st = la_open(&fmt, &so, &err);
	if (!st) {
		fprintf(stderr, "%s: cannot open an audio stream: %s\n", prog,
			la_strerror(err));
		if (err == -ENOENT || err == -ECONNREFUSED)
			fprintf(stderr, "%s: the audio service (audiod) is not running\n",
				prog);
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (!opt.quiet && opt.mode != M_CHANNELS && opt.mode != M_LATENCY) {
		char lvl[32], dur[32], where[48] = "";
		if (opt.amp > 0)
			snprintf(lvl, sizeof(lvl), "%.1f dBFS", 20.0 * log10(opt.amp));
		else
			snprintf(lvl, sizeof(lvl), "silent");
		if (opt.secs > 0)
			snprintf(dur, sizeof(dur), "%.1f s", opt.secs);
		else
			snprintf(dur, sizeof(dur), "until Ctrl-C");
		if (opt.channel)
			snprintf(where, sizeof(where), ", channel %d only", opt.channel);
		if (opt.mode == M_SWEEP)
			fprintf(stderr, "%s sweep %.0f -> %.0f Hz, %s, %u ch at %u Hz, %s%s\n",
				opt.linear ? "Linear" : "Logarithmic",
				opt.freq_given ? opt.freq : 20.0, opt.freq_to, lvl,
				opt.channels, opt.rate, dur, where);
		else if (opt.mode == M_SINE || opt.mode == M_SQUARE ||
			 opt.mode == M_SAW || opt.mode == M_TRIANGLE)
			fprintf(stderr, "%s %.1f Hz, %s, %u ch at %u Hz, %s%s\n",
				name, opt.freq, lvl, opt.channels, opt.rate, dur, where);
		else
			fprintf(stderr, "%s, %s, %u ch at %u Hz, %s%s\n", name, lvl,
				opt.channels, opt.rate, dur, where);
	}

	switch (opt.mode) {
	case M_CHANNELS:
		rc = run_channels();
		break;
	case M_LATENCY:
		rc = run_latency();
		break;
	default:
		rc = run_continuous();
	}

	if (interrupted) {
		(void)la_flush(st);
		la_close(st);
		if (!opt.quiet)
			fprintf(stderr, "\n%s: stopped\n", prog);
		return 130;
	}
	(void)la_drain(st);
	la_close(st);
	free(fbuf);
	free(sbuf);
	return rc;
}
