/*
 * gen-sounds -- synthesize the LikeOS system sounds.
 *
 * Writes the event sounds that audiod plays by name (startup, shutdown,
 * login, logout, notify, error, volume, device-added, device-removed,
 * message) as 48 kHz, 16-bit, stereo PCM WAV files.
 *
 * Why GENERATED rather than recorded: every sound here is a handful of notes
 * on two or three synthetic instruments, so the code that plays them is far
 * shorter, diffable and editable than a directory of binary recordings, and
 * nothing in it is anybody else's sample.  Same reasoning as
 * host/gen-cursors.c.  The output is deterministic: all randomness (detune
 * drift, noise, dither) comes from a seeded generator, so two runs on the
 * same machine produce byte-identical files.
 *
 * The palette is one key, D-flat major, and one family of timbres, so the
 * sounds belong together:
 *
 *   epiano   two-operator FM tone, carrier and modulator at the same
 *            frequency (a warm, round electric-piano body whose brightness
 *            fades as the note rings), plus a quiet "tine" operator pair at
 *            14:1 that gives the strike its glassy edge for a few
 *            milliseconds.  Two slightly detuned copies are panned apart.
 *   bars     additive models of struck bars: a vibraphone (partials near
 *            1, 4 and 10, long decay, gentle tremolo), a marimba (the same
 *            series with a short decay and a woody mallet), and a small
 *            glass bell (near-harmonic with two faint inharmonic partials).
 *            Each partial has its own decay -- the higher, the faster --
 *            and a band-passed noise burst stands in for the mallet.
 *   pad      a stack of three detuned band-limited sawtooth oscillators per
 *            note with slow random pitch drift, spread across the stereo
 *            field and shaped by a 4-pole low-pass whose cutoff and level
 *            follow smooth automation curves.
 *   air      band-passed stereo noise, very low, for sheen.
 *
 * Everything is mixed into a dry bus and a reverb send.  The reverb is a
 * stereo network of early-reflection taps, eight damped feedback combs per
 * channel and four series allpasses; the comb feedback is computed from the
 * requested decay time, and the comb inputs are scaled so the reverb's
 * impulse response carries about the same energy as its input whatever the
 * decay time, which keeps "send" and "wet" meaningful.
 *
 * Mastering, per sound: rumble high-pass, a slight high-shelf cut for
 * warmth, raised-cosine fades at both ends, gain chosen from BOTH a peak
 * ceiling and a loudness target (BS.1770 K-weighted, gated) so the event
 * sounds sit quieter than the start-up chime, a look-ahead peak limiter that
 * never needs more than a dB or two, a DC correction shaped like a Hann
 * window (it cannot move the faded-out ends), then TPDF dither to 16 bit.
 * The program checks its own output for clipping and DC before writing it.
 *
 * Musical pitch is equal temperament, A4 = 440 Hz; notes are MIDI numbers.
 *
 * Usage: gen-sounds <output-directory>
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SR 48000
#define SRD 48000.0

static const double PI = 3.14159265358979323846;
static const double TWO_PI = 6.28318530717958647692;
static const double SQRT2 = 1.41421356237309504880;

/* Pitches used below, as MIDI note numbers (middle C = 60). */
enum {
	DB2 = 37,
	DB3 = 49,
	AB3 = 56,
	C4 = 60,
	DB4 = 61,
	EB4 = 63,
	F4 = 65,
	AB4 = 68,
	C5 = 72,
	DB5 = 73,
	EB5 = 75,
	F5 = 77,
	AB5 = 80,
	BB5 = 82,
	C6 = 84,
};

/* ---- Utilities ---------------------------------------------------------- */

static void die(const char *fmt, ...)
{
	va_list ap;

	fputs("gen-sounds: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *xcalloc(size_t n, size_t size)
{
	void *p;

	if (n == 0)
		n = 1;
	p = calloc(n, size);
	if (!p)
		die("out of memory (%zu x %zu bytes)", n, size);
	return p;
}

static double clampd(double x, double lo, double hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static double db_to_lin(double db)
{
	return pow(10.0, db / 20.0);
}

static double lin_to_db(double x)
{
	return x > 1e-12 ? 20.0 * log10(x) : -240.0;
}

static double midi_hz(double m)
{
	return 440.0 * pow(2.0, (m - 69.0) / 12.0);
}

static double cents_ratio(double cents)
{
	return pow(2.0, cents / 1200.0);
}

/* Fractional part, so a phase stays in [0, 1) however long the note. */
static double frac(double x)
{
	return x - floor(x);
}

static size_t sec_to_samples(double s)
{
	if (s <= 0.0)
		return 0;
	return (size_t)llround(s * SRD);
}

/* ---- Deterministic random numbers --------------------------------------- */

/*
 * splitmix64: tiny, statistically good enough for audio noise, and -- the
 * point -- identical on every machine, unlike rand().
 */
typedef struct {
	uint64_t s;
} rng_t;

static uint64_t rng_u64(rng_t *r)
{
	uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* Uniform in [0, 1) with 53 bits. */
static double rng_unit(rng_t *r)
{
	return (double)(rng_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* Uniform in [-1, 1). */
static double rng_bi(rng_t *r)
{
	return 2.0 * rng_unit(r) - 1.0;
}

/* ---- Envelopes and automation curves ------------------------------------ */

/* Raised-cosine rise from 0 to 1 over `a' seconds: no corner, no click. */
static double env_attack(double t, double a)
{
	if (t <= 0.0)
		return 0.0;
	if (a <= 0.0 || t >= a)
		return 1.0;
	return 0.5 - 0.5 * cos(PI * t / a);
}

/* Raised-cosine fall from 1 to 0, starting at `start', lasting `len'. */
static double env_release(double t, double start, double len)
{
	if (t <= start)
		return 1.0;
	if (len <= 0.0 || t >= start + len)
		return 0.0;
	return 0.5 + 0.5 * cos(PI * (t - start) / len);
}

/*
 * A breakpoint curve.  Between points the value moves along a half cosine,
 * so the slope is zero at every breakpoint: a level or a filter cutoff that
 * follows it never kinks audibly.  Frequencies are interpolated on a log
 * scale (curve_at_log), which is how pitch and brightness are heard.
 */
#define CURVE_MAX 12

typedef struct {
	int n;
	double t[CURVE_MAX];
	double v[CURVE_MAX];
} curve_t;

static void curve_check(const curve_t *c, const char *what, int positive)
{
	int i;

	if (c->n < 1 || c->n > CURVE_MAX)
		die("curve %s: %d points", what, c->n);
	for (i = 0; i < c->n; i++) {
		if (i > 0 && !(c->t[i] > c->t[i - 1]))
			die("curve %s: times not increasing at point %d", what, i);
		if (positive && !(c->v[i] > 0.0))
			die("curve %s: value %d must be positive", what, i);
	}
}

static double curve_interp(const curve_t *c, double t, int logscale)
{
	int i;

	if (t <= c->t[0])
		return c->v[0];
	for (i = 1; i < c->n; i++) {
		if (t < c->t[i]) {
			double u = (t - c->t[i - 1]) / (c->t[i] - c->t[i - 1]);
			double a = c->v[i - 1], b = c->v[i];

			u = 0.5 - 0.5 * cos(PI * u);
			if (logscale)
				return exp(log(a) + (log(b) - log(a)) * u);
			return a + (b - a) * u;
		}
	}
	return c->v[c->n - 1];
}

static double curve_at(const curve_t *c, double t)
{
	return curve_interp(c, t, 0);
}

static double curve_at_log(const curve_t *c, double t)
{
	return curve_interp(c, t, 1);
}

/* ---- Stereo buffers and the scene --------------------------------------- */

typedef struct {
	size_t n;
	double *l, *r;
} sbuf_t;

static void sbuf_init(sbuf_t *b, size_t n)
{
	b->n = n;
	b->l = xcalloc(n, sizeof(double));
	b->r = xcalloc(n, sizeof(double));
}

static void sbuf_free(sbuf_t *b)
{
	free(b->l);
	free(b->r);
	b->l = b->r = NULL;
	b->n = 0;
}

/*
 * Equal-power pan.  Centre is unity in both channels (so a centred voice is
 * as loud per channel as its own amplitude); hard left is sqrt(2) on the
 * left alone.  Nothing below pans further than about +-0.7.
 */
static void pan_law(double pan, double *gl, double *gr)
{
	double a = (clampd(pan, -1.0, 1.0) + 1.0) * 0.25 * PI;

	*gl = cos(a) * SQRT2;
	*gr = sin(a) * SQRT2;
}

typedef struct {
	double rt60;     /* late decay to -60 dB, seconds */
	double predelay; /* before the late reverb starts, seconds */
	double damp;     /* 0..1, high-frequency loss per comb pass */
	double size;     /* scales the comb and allpass delays */
	double width;    /* 0 = mono reverb, 1 = fully decorrelated */
	double hp_hz;    /* reverb input high-pass (keeps the tail clean) */
	double lp_hz;    /* reverb input low-pass (keeps it soft) */
	double er_level; /* early reflections */
	double wet;      /* reverb return level */
} reverb_cfg_t;

typedef struct {
	size_t n;     /* length in frames */
	sbuf_t dry;   /* direct sound */
	sbuf_t send;  /* reverb input */
	rng_t rng;    /* this sound's own random stream */
	reverb_cfg_t rv;
	int has_reverb;
} scene_t;

static void scene_add(scene_t *sc, size_t i, double v, double gl, double gr,
		      double send)
{
	if (i >= sc->n)
		return;
	sc->dry.l[i] += v * gl;
	sc->dry.r[i] += v * gr;
	if (send > 0.0) {
		sc->send.l[i] += v * gl * send;
		sc->send.r[i] += v * gr * send;
	}
}

/* ---- Filters ------------------------------------------------------------ */

/*
 * State-variable filter in the trapezoidal-integrator form.  It stays stable
 * and free of zipper noise while its cutoff moves every sample, which is
 * what the pad and the noise sweeps need.  `bpn' is the band-pass scaled to
 * unity gain at the centre frequency.
 */
typedef struct {
	double ic1, ic2;
} svf_t;

typedef struct {
	double lp, bp, bpn, hp;
} svf_out_t;

static svf_out_t svf_tick(svf_t *s, double x, double fc, double q)
{
	svf_out_t o;
	double g, k, a1, a2, a3, v1, v2, v3;

	fc = clampd(fc, 5.0, 0.45 * SRD);
	g = tan(PI * fc / SRD);
	k = 1.0 / q;
	a1 = 1.0 / (1.0 + g * (g + k));
	a2 = g * a1;
	a3 = g * a2;
	v3 = x - s->ic2;
	v1 = a1 * s->ic1 + a2 * v3;
	v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
	s->ic1 = 2.0 * v1 - s->ic1;
	s->ic2 = 2.0 * v2 - s->ic2;
	o.lp = v2;
	o.bp = v1;
	o.bpn = k * v1;
	o.hp = x - k * v1 - v2;
	return o;
}

/* Biquad, transposed direct form II.  Coefficients normalised by a0. */
typedef struct {
	double b0, b1, b2, a1, a2;
	double z1, z2;
} biquad_t;

static double bq_tick(biquad_t *f, double x)
{
	double y = f->b0 * x + f->z1;

	f->z1 = f->b1 * x - f->a1 * y + f->z2;
	f->z2 = f->b2 * x - f->a2 * y;
	return y;
}

static void bq_set(biquad_t *f, double b0, double b1, double b2, double a0,
		   double a1, double a2)
{
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
	f->z1 = f->z2 = 0.0;
}

static void bq_lowpass(biquad_t *f, double fc, double q)
{
	double w = TWO_PI * fc / SRD, c = cos(w), al = sin(w) / (2.0 * q);

	bq_set(f, (1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + al,
	       -2.0 * c, 1.0 - al);
}

static void bq_highpass(biquad_t *f, double fc, double q)
{
	double w = TWO_PI * fc / SRD, c = cos(w), al = sin(w) / (2.0 * q);

	bq_set(f, (1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + al,
	       -2.0 * c, 1.0 - al);
}

/* High shelf with slope S (1 = steepest without overshoot). */
static void bq_highshelf(biquad_t *f, double fc, double gain_db, double s)
{
	double a = pow(10.0, gain_db / 40.0);
	double w = TWO_PI * fc / SRD, c = cos(w), sn = sin(w);
	double al = sn / 2.0 * sqrt((a + 1.0 / a) * (1.0 / s - 1.0) + 2.0);
	double sa = 2.0 * sqrt(a) * al;

	bq_set(f,
	       a * ((a + 1.0) + (a - 1.0) * c + sa),
	       -2.0 * a * ((a - 1.0) + (a + 1.0) * c),
	       a * ((a + 1.0) + (a - 1.0) * c - sa),
	       (a + 1.0) - (a - 1.0) * c + sa,
	       2.0 * ((a - 1.0) - (a + 1.0) * c),
	       (a + 1.0) - (a - 1.0) * c - sa);
}

/* ---- Delay-line building blocks ----------------------------------------- */

typedef struct {
	double *buf;
	int len, pos;
} dline_t;

static void dl_init(dline_t *d, int len)
{
	if (len < 1)
		len = 1;
	d->buf = xcalloc((size_t)len, sizeof(double));
	d->len = len;
	d->pos = 0;
}

static void dl_free(dline_t *d)
{
	free(d->buf);
	d->buf = NULL;
}

/* The sample written `delay' writes ago, 1 <= delay <= len.  Call before
 * dl_write for the current sample. */
static double dl_read(const dline_t *d, int delay)
{
	int i = d->pos - delay;

	if (i < 0)
		i += d->len;
	return d->buf[i];
}

static void dl_write(dline_t *d, double x)
{
	d->buf[d->pos] = x;
	if (++d->pos == d->len)
		d->pos = 0;
}

/*
 * Feedback comb with a one-pole low-pass in the loop: every trip round the
 * loop loses a little more treble than bass, which is what makes a tail
 * sound like a room instead of a spring.
 */
typedef struct {
	dline_t d;
	double fb, damp, store, in_gain;
} comb_t;

static double comb_tick(comb_t *c, double x)
{
	double out = dl_read(&c->d, c->d.len);

	c->store = out * (1.0 - c->damp) + c->store * c->damp;
	dl_write(&c->d, x * c->in_gain + c->store * c->fb);
	return out;
}

/*
 * Schroeder allpass, H(z) = (z^-d - g) / (1 - g z^-d): flat magnitude, it
 * only smears phase, turning the combs' periodic echoes into a dense wash.
 */
typedef struct {
	dline_t d;
	double g;
} allpass_t;

static double ap_tick(allpass_t *a, double x)
{
	double wd = dl_read(&a->d, a->d.len);
	double w = x + a->g * wd;

	dl_write(&a->d, w);
	return wd - a->g * w;
}

static int is_prime(int n)
{
	int k;

	if (n < 2)
		return 0;
	for (k = 2; k * k <= n; k++)
		if (n % k == 0)
			return 0;
	return 1;
}

/* Mutually prime delay lengths keep the comb echoes from lining up into a
 * pitched buzz. */
static int next_prime(int n)
{
	while (!is_prime(n))
		n++;
	return n;
}

/* ---- Reverb ------------------------------------------------------------- */

#define RV_COMBS 8
#define RV_APS 4
#define RV_ER 6

/* Comb loop times at size 1.0: 25-37 ms, a medium hall. */
static const double rv_comb_ms[RV_COMBS] = {
	25.31, 26.94, 28.96, 30.75, 32.24, 33.81, 35.31, 36.67,
};
static const double rv_ap_ms[RV_APS] = { 12.61, 10.00, 7.73, 5.10 };
/* The right channel's loops are this much longer: decorrelates L and R. */
static const double rv_spread_ms = 0.52;

/* Early reflections: tap times (ms after the dry sound) and gains.  The
 * two channels hear different walls. */
static const double rv_er_ms[2][RV_ER] = {
	{ 7.1, 11.9, 17.3, 23.5, 31.1, 39.7 },
	{ 8.3, 13.7, 19.1, 26.3, 33.9, 42.1 },
};
static const double rv_er_gain[RV_ER] = { 0.55, -0.45, 0.38, -0.30, 0.24, -0.18 };

/*
 * Run the send bus through the reverb and ADD the result to `out'.
 */
static void reverb_process(const reverb_cfg_t *cfg, const sbuf_t *in, sbuf_t *out)
{
	comb_t comb[2][RV_COMBS];
	allpass_t ap[2][RV_APS];
	biquad_t hp[2], lp[2];
	dline_t pre[2];
	int er_tap[2][RV_ER];
	int pre_samp, pre_len, ch, k;
	double w1, w2, comb_norm = 1.0 / sqrt((double)RV_COMBS);
	size_t i;

	if (cfg->rt60 <= 0.05)
		die("reverb rt60 %.3f too short", cfg->rt60);

	pre_samp = (int)lround(cfg->predelay * SRD);
	if (pre_samp < 1)
		pre_samp = 1;
	pre_len = pre_samp;
	for (ch = 0; ch < 2; ch++) {
		for (k = 0; k < RV_ER; k++) {
			er_tap[ch][k] = (int)lround(rv_er_ms[ch][k] * SRD / 1000.0);
			if (er_tap[ch][k] > pre_len)
				pre_len = er_tap[ch][k];
		}
	}

	for (ch = 0; ch < 2; ch++) {
		bq_highpass(&hp[ch], cfg->hp_hz, 0.7071);
		bq_lowpass(&lp[ch], cfg->lp_hz, 0.7071);
		dl_init(&pre[ch], pre_len);
		for (k = 0; k < RV_COMBS; k++) {
			double ms = rv_comb_ms[k] * cfg->size + (ch ? rv_spread_ms : 0.0);
			int len = next_prime((int)lround(ms * SRD / 1000.0));
			comb_t *c = &comb[ch][k];

			dl_init(&c->d, len);
			/* Loop gain for the requested decay: after rt60
			 * seconds the signal has made rt60*SR/len trips and
			 * must be 60 dB down. */
			c->fb = pow(10.0, -3.0 * (double)len / (cfg->rt60 * SRD));
			c->damp = clampd(cfg->damp, 0.0, 0.95);
			c->store = 0.0;
			/* Unit-energy impulse response per comb: the
			 * response of y = x + g*y[n-d] has energy
			 * 1/(1-g^2). */
			c->in_gain = sqrt(1.0 - c->fb * c->fb);
		}
		for (k = 0; k < RV_APS; k++) {
			double ms = rv_ap_ms[k] * cfg->size + (ch ? rv_spread_ms : 0.0);

			dl_init(&ap[ch][k].d, next_prime((int)lround(ms * SRD / 1000.0)));
			ap[ch][k].g = 0.5;
		}
	}

	w1 = (1.0 + cfg->width) * 0.5;
	w2 = (1.0 - cfg->width) * 0.5;

	for (i = 0; i < in->n; i++) {
		double x[2], er[2], late[2];

		x[0] = bq_tick(&lp[0], bq_tick(&hp[0], in->l[i]));
		x[1] = bq_tick(&lp[1], bq_tick(&hp[1], in->r[i]));

		for (ch = 0; ch < 2; ch++) {
			double p_own, p_other, sum = 0.0, e = 0.0;

			/* Read before writing this sample: taps reach back
			 * 1..pre_len samples. */
			p_own = dl_read(&pre[ch], pre_samp);
			p_other = dl_read(&pre[ch ^ 1], pre_samp);
			for (k = 0; k < RV_ER; k++) {
				double own = dl_read(&pre[ch], er_tap[ch][k]);
				double oth = dl_read(&pre[ch ^ 1], er_tap[ch][k]);

				e += rv_er_gain[k] * (0.7 * own + 0.3 * oth);
			}
			er[ch] = e;
			{
				double lin = 0.75 * p_own + 0.25 * p_other;

				for (k = 0; k < RV_COMBS; k++)
					sum += comb_tick(&comb[ch][k], lin);
			}
			sum *= comb_norm;
			for (k = 0; k < RV_APS; k++)
				sum = ap_tick(&ap[ch][k], sum);
			late[ch] = sum;
		}
		dl_write(&pre[0], x[0]);
		dl_write(&pre[1], x[1]);

		out->l[i] += cfg->wet * (w1 * late[0] + w2 * late[1] + cfg->er_level * er[0]);
		out->r[i] += cfg->wet * (w1 * late[1] + w2 * late[0] + cfg->er_level * er[1]);
	}

	for (ch = 0; ch < 2; ch++) {
		dl_free(&pre[ch]);
		for (k = 0; k < RV_COMBS; k++)
			dl_free(&comb[ch][k].d);
		for (k = 0; k < RV_APS; k++)
			dl_free(&ap[ch][k].d);
	}
}

/* ---- Notes -------------------------------------------------------------- */

typedef struct {
	double t0;     /* onset, seconds from the start of the sound */
	double midi;   /* pitch */
	double vel;    /* strike strength 0..1: level AND brightness */
	double pan;    /* -1 left .. +1 right */
	double dur;    /* seconds until the release begins */
	double rel;    /* release length, seconds */
	double bright; /* timbre 0 (dark) .. 1 (bright) */
	double send;   /* reverb send */
	double decay;  /* multiplies the instrument's natural decay times */
	double trem;   /* tremolo depth (bars only), 0..0.3 */
} note_t;

static note_t note(double t0, double midi, double vel, double pan, double dur,
		   double rel, double bright, double send)
{
	note_t n;

	memset(&n, 0, sizeof n);
	n.t0 = t0;
	n.midi = midi;
	n.vel = vel;
	n.pan = pan;
	n.dur = dur;
	n.rel = rel;
	n.bright = bright;
	n.send = send;
	n.decay = 1.0;
	n.trem = 0.0;
	return n;
}

static void note_check(const note_t *n)
{
	if (n->t0 < 0.0 || n->dur < 0.0 || n->rel < 0.0)
		die("note %.1f: negative time", n->midi);
	if (n->midi < 12.0 || n->midi > 120.0)
		die("note %.1f: out of range", n->midi);
	if (n->decay <= 0.0)
		die("note %.1f: decay must be positive", n->midi);
}

/* The range of frames a note touches, clipped to the scene. */
static size_t note_frames(const scene_t *sc, const note_t *n, size_t *start)
{
	size_t i0 = sec_to_samples(n->t0);
	size_t len = sec_to_samples(n->dur + n->rel) + 1;

	*start = i0;
	if (i0 >= sc->n)
		return 0;
	if (len > sc->n - i0)
		len = sc->n - i0;
	return len;
}

/* ---- Instrument: FM electric piano -------------------------------------- */

/*
 * Two operator pairs.  The BODY is phase modulation with the modulator at the
 * carrier's own frequency: sin(w t + I(t) sin(w t)).  That spectrum is
 * harmonic (sidebands at multiples of f, Bessel-weighted by I) and has no DC
 * term, because the zero-frequency sideband is sin(0).  The index starts high
 * -- the bright "bloom" of a struck tone -- and decays towards a small floor
 * so the ringing tail is nearly a sine.
 *
 * The TINE is a carrier at f modulated at 14 f with an index that dies within
 * ~20 ms: a brief cluster of partials around 13 f and 15 f, heard as the
 * glassy edge of the strike rather than as pitch.  It is kept quiet, and
 * scaled down for high notes so it never gets near Nyquist or turns shrill.
 *
 * Low notes ring longer than high ones (k), like real tines and strings.
 */
static void render_epiano(scene_t *sc, const note_t *nt, double gain)
{
	const double f = midi_hz(nt->midi);
	const double vel = clampd(nt->vel, 0.0, 1.0);
	const double bright = clampd(nt->bright, 0.0, 1.0);
	const double k = clampd(pow(261.63 / f, 0.45), 0.45, 1.7) * nt->decay;
	const double tau_fast = 0.30 * k, tau_slow = 2.1 * k;
	const double idx_peak = (0.50 + 1.50 * bright) * (0.55 + 0.45 * vel);
	const double idx_floor = 0.18 + 0.45 * bright;
	const double tau_idx = 0.20 * k;
	const double tine_scale = clampd(pow(440.0 / f, 0.6), 0.25, 1.3);
	double tine_idx = (0.4 + 1.6 * bright) * vel * tine_scale;
	double tine_amp = 0.18 * (0.3 + 0.7 * bright) * vel * tine_scale;
	const double tau_tine_idx = 0.018, tau_tine = 0.10 * k;
	const double att = 0.0025 + 0.004 * (1.0 - vel);
	const double amp = gain * (0.2 + 0.8 * vel);
	/* Two copies of the body, a few cents apart, panned apart: a slow
	 * beating chorus and a little width. */
	const double fa = f * cents_ratio(-1.8), fb = f * cents_ratio(2.6);
	double gal, gar, gbl, gbr;
	size_t i0, len, j;

	note_check(nt);
	/* Highest tine sideband that matters (~2 orders around 14 f). */
	if (f * 16.0 > 0.42 * SRD) {
		tine_idx = 0.0;
		tine_amp = 0.0;
	}
	pan_law(nt->pan - 0.12, &gal, &gar);
	pan_law(nt->pan + 0.12, &gbl, &gbr);

	len = note_frames(sc, nt, &i0);
	for (j = 0; j < len; j++) {
		double t = (double)j / SRD;
		double env = env_attack(t, att) *
			     (0.62 * exp(-t / tau_fast) + 0.38 * exp(-t / tau_slow)) *
			     env_release(t, nt->dur, nt->rel);
		double ix = idx_peak * exp(-t / tau_idx) + idx_floor;
		double pa = TWO_PI * frac(fa * t), pb = TWO_PI * frac(fb * t);
		double body_a = sin(pa + ix * sin(pa));
		double body_b = sin(pb + ix * sin(pb));
		double tine = 0.0;

		if (env == 0.0 && t > att)
			break;
		if (tine_amp > 0.0) {
			double pc = TWO_PI * frac(f * t);
			double pm = TWO_PI * frac(14.0 * f * t);

			tine = tine_amp * exp(-t / tau_tine) *
			       sin(pc + tine_idx * exp(-t / tau_tine_idx) * sin(pm));
		}
		scene_add(sc, i0 + j, env * amp * (body_a + tine), gal, gar, nt->send);
		scene_add(sc, i0 + j, env * amp * 0.55 * body_b, gbl, gbr, nt->send);
	}
}

/* ---- Instrument: struck bars (vibraphone, marimba, glass) --------------- */

typedef struct {
	double ratio;      /* partial frequency / fundamental */
	double amp;        /* level at full brightness */
	double tau;        /* decay, relative to the fundamental's */
	double bright_exp; /* how strongly brightness controls this partial */
} partial_t;

#define BAR_PARTIALS 8

typedef struct {
	const char *name;
	int np;
	partial_t p[BAR_PARTIALS];
	double tau_base;     /* fundamental decay at middle C, seconds */
	double tau_pow;      /* decay ~ (261.63/f)^tau_pow */
	double attack;       /* seconds */
	double mallet_amp;   /* noise burst level */
	double mallet_q;     /* its band-pass Q */
	double mallet_ratio; /* its centre / fundamental */
	double mallet_tau;   /* its decay, seconds */
	double trem_rate;    /* Hz, for notes that ask for tremolo */
} bar_model_t;

/*
 * Tuned bars are cut so the first overtone lands two octaves up (4:1) and
 * the next near 10:1; that is why a vibraphone sounds so consonant in a
 * chord.  The ratios are set a hair off the exact integers, as on a real
 * instrument, which keeps the overtones from fusing completely.  The faint
 * 2:1 partial is not a bar mode; it stands in for the resonator tube and
 * adds body.
 */
static const bar_model_t bar_vibes = {
	.name = "vibraphone",
	.np = 4,
	.p = {
		{ 1.000, 1.00, 1.00, 0.0 },
		{ 3.990, 0.32, 0.30, 1.0 },
		{ 9.930, 0.07, 0.12, 2.0 },
		{ 2.000, 0.05, 0.55, 0.5 },
	},
	.tau_base = 1.70,
	.tau_pow = 0.50,
	.attack = 0.0018,
	.mallet_amp = 0.035,
	.mallet_q = 1.2,
	.mallet_ratio = 3.0,
	.mallet_tau = 0.004,
	.trem_rate = 4.7,
};

/* Same modal series, short decay (wood, not aluminium), harder mallet. */
static const bar_model_t bar_marimba = {
	.name = "marimba",
	.np = 3,
	.p = {
		{ 1.000, 1.00, 1.00, 0.0 },
		{ 3.985, 0.20, 0.22, 1.0 },
		{ 9.800, 0.05, 0.09, 2.0 },
	},
	.tau_base = 0.55,
	.tau_pow = 0.60,
	.attack = 0.0012,
	.mallet_amp = 0.060,
	.mallet_q = 0.9,
	.mallet_ratio = 2.2,
	.mallet_tau = 0.005,
	.trem_rate = 0.0,
};

/*
 * A small glass bell: mostly harmonic (so it agrees with the chord), with
 * two faint inharmonic partials near 4.17 and 5.43 that make it ring like
 * glass and decay first.
 */
static const bar_model_t bar_glass = {
	.name = "glass",
	.np = 6,
	.p = {
		{ 1.000, 1.00, 1.00, 0.0 },
		{ 2.000, 0.38, 0.60, 0.7 },
		{ 3.000, 0.10, 0.38, 1.2 },
		{ 4.170, 0.05, 0.22, 1.6 },
		{ 5.430, 0.035, 0.14, 2.0 },
		{ 6.000, 0.02, 0.10, 2.5 },
	},
	.tau_base = 1.25,
	.tau_pow = 0.50,
	.attack = 0.0015,
	.mallet_amp = 0.020,
	.mallet_q = 2.0,
	.mallet_ratio = 5.0,
	.mallet_tau = 0.003,
	.trem_rate = 0.0,
};

static void render_bar(scene_t *sc, const bar_model_t *m, const note_t *nt,
		       double gain)
{
	const double f = midi_hz(nt->midi);
	const double vel = clampd(nt->vel, 0.0, 1.0);
	const double bright = clampd(nt->bright, 0.0, 1.0);
	const double k = clampd(pow(261.63 / f, m->tau_pow), 0.35, 2.0) * nt->decay;
	const double amp = gain * (0.2 + 0.8 * vel);
	double pamp[BAR_PARTIALS], ptau[BAR_PARTIALS], pf[BAR_PARTIALS];
	double mallet_fc, mallet_amp, trem_ph, norm = 0.0;
	double gl, gr;
	svf_t mallet = { 0.0, 0.0 };
	size_t i0, len, j;
	int p;

	note_check(nt);
	if (m->np > BAR_PARTIALS)
		die("bar model %s: too many partials", m->name);

	/* Harder strikes excite the upper modes more: brightness and velocity
	 * both feed the partial weights. */
	for (p = 0; p < m->np; p++) {
		double b = (0.2 + 0.8 * bright) * (0.7 + 0.3 * vel);

		pf[p] = f * m->p[p].ratio;
		pamp[p] = m->p[p].amp * pow(b, m->p[p].bright_exp);
		ptau[p] = m->tau_base * k * m->p[p].tau;
		/* Drop partials that would sit near or above Nyquist, and
		 * fade out the ones above 14 kHz, where they only sizzle. */
		if (pf[p] > 0.40 * SRD)
			pamp[p] = 0.0;
		else if (pf[p] > 14000.0)
			pamp[p] *= clampd((0.40 * SRD - pf[p]) / (0.40 * SRD - 14000.0), 0.0, 1.0);
		norm += pamp[p];
	}
	/* Keep the strike peak independent of how many partials sound. */
	norm = norm > 0.0 ? 1.0 / sqrt(norm) : 0.0;

	mallet_fc = clampd(f * m->mallet_ratio, 400.0, 8000.0);
	mallet_amp = m->mallet_amp * vel * (0.3 + 0.7 * bright);
	trem_ph = rng_unit(&sc->rng);
	pan_law(nt->pan, &gl, &gr);

	len = note_frames(sc, nt, &i0);
	for (j = 0; j < len; j++) {
		double t = (double)j / SRD;
		double rel = env_release(t, nt->dur, nt->rel);
		double att = env_attack(t, m->attack);
		double v = 0.0, trem = 1.0, noise;

		for (p = 0; p < m->np; p++)
			if (pamp[p] > 0.0)
				v += pamp[p] * exp(-t / ptau[p]) * sin(TWO_PI * frac(pf[p] * t));
		v *= norm;

		if (nt->trem > 0.0 && m->trem_rate > 0.0) {
			/* The rotating discs in a vibraphone's tubes: a slow
			 * amplitude wobble, depth rising over the first 0.3 s
			 * as the motor's effect becomes audible over the
			 * strike. */
			double d = clampd(nt->trem, 0.0, 0.3) * env_attack(t, 0.3);

			trem = 1.0 - d * (0.5 - 0.5 * cos(TWO_PI * (m->trem_rate * t + trem_ph)));
		}

		/* Mallet: band-passed noise, gone within ~20 ms. */
		noise = 0.0;
		if (t < 12.0 * m->mallet_tau) {
			svf_out_t o = svf_tick(&mallet, rng_bi(&sc->rng), mallet_fc, m->mallet_q);

			noise = mallet_amp * env_attack(t, 0.0005) * exp(-t / m->mallet_tau) * o.bpn;
		}

		scene_add(sc, i0 + j, amp * rel * (att * trem * v + noise), gl, gr, nt->send);
	}
}

/* ---- Instrument: pad ---------------------------------------------------- */

enum { PAD_SAW, PAD_SINE };

#define PAD_NOTES 8

typedef struct {
	int n;
	double midi[PAD_NOTES];
	double lvl[PAD_NOTES];
	int wave[PAD_NOTES];
	curve_t amp;    /* overall level against scene time */
	curve_t cutoff; /* low-pass cutoff in Hz against scene time */
	double gain;
	double send;
	double width;   /* pan of the outer detuned voices */
	double detune;  /* cents between the outer voices and the centre */
	double drift;   /* cents of slow random pitch wander */
} pad_t;

static void pad_note(pad_t *p, double midi, double lvl, int wave)
{
	if (p->n >= PAD_NOTES)
		die("pad: too many notes");
	p->midi[p->n] = midi;
	p->lvl[p->n] = lvl;
	p->wave[p->n] = wave;
	p->n++;
}

/*
 * PolyBLEP correction: subtracting this two-sample polynomial at each wrap
 * of the sawtooth removes most of the aliasing a naive ramp produces.
 */
static double polyblep(double t, double dt)
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

/*
 * One oscillator of the pad.  `drift' wanders the pitch with a slow sine of
 * random rate and phase, so the voices of a note beat against each other
 * irregularly -- the motion that makes a pad sound alive rather than
 * like an organ.
 */
static void pad_voice(scene_t *sc, sbuf_t *bus, double f, double lvl, int wave,
		      double pan, double drift)
{
	double ph = rng_unit(&sc->rng);
	double lfo_rate = 0.11 + 0.23 * rng_unit(&sc->rng);
	double lfo_ph = rng_unit(&sc->rng);
	double gl, gr;
	size_t i;

	if (wave == PAD_SINE)
		ph = 0.0; /* a sine starting at zero needs no help to start clean */
	pan_law(pan, &gl, &gr);
	for (i = 0; i < bus->n; i++) {
		double t = (double)i / SRD;
		double fi = f * cents_ratio(drift * sin(TWO_PI * (lfo_rate * t + lfo_ph)));
		double dt = fi / SRD, v;

		if (wave == PAD_SINE) {
			v = sin(TWO_PI * ph);
		} else {
			v = 2.0 * ph - 1.0 - polyblep(ph, dt);
		}
		ph += dt;
		if (ph >= 1.0)
			ph -= 1.0;
		bus->l[i] += v * lvl * gl;
		bus->r[i] += v * lvl * gr;
	}
}

static void render_pad(scene_t *sc, const pad_t *p)
{
	/* 4-pole Butterworth low-pass as two cascaded 2-pole sections. */
	static const double q_stage[2] = { 0.5412, 1.3066 };
	svf_t flt[2][2];
	sbuf_t bus;
	size_t i;
	int k;

	curve_check(&p->amp, "pad amp", 0);
	curve_check(&p->cutoff, "pad cutoff", 1);
	sbuf_init(&bus, sc->n);
	memset(flt, 0, sizeof flt);

	for (k = 0; k < p->n; k++) {
		double f = midi_hz(p->midi[k]);
		/* Alternate which side the flat voice goes to, note by note,
		 * so the chord spreads evenly. */
		double side = (k & 1) ? -1.0 : 1.0;

		if (p->wave[k] == PAD_SINE) {
			pad_voice(sc, &bus, f, p->lvl[k], PAD_SINE, 0.0, p->drift * 0.5);
			continue;
		}
		/* Three saws per note, -d / ~0 / +d cents; the outer two at
		 * 0.8 level so the centre carries the pitch. */
		pad_voice(sc, &bus, f * cents_ratio(-p->detune), p->lvl[k] * 0.8,
			  PAD_SAW, -side * p->width, p->drift);
		pad_voice(sc, &bus, f * cents_ratio(0.3), p->lvl[k], PAD_SAW,
			  0.0, p->drift);
		pad_voice(sc, &bus, f * cents_ratio(p->detune * 0.95), p->lvl[k] * 0.8,
			  PAD_SAW, side * p->width, p->drift);
	}

	for (i = 0; i < sc->n; i++) {
		double t = (double)i / SRD;
		double fc = curve_at_log(&p->cutoff, t);
		double a = curve_at(&p->amp, t) * p->gain;
		double l = bus.l[i], r = bus.r[i];

		for (k = 0; k < 2; k++) {
			l = svf_tick(&flt[0][k], l, fc, q_stage[k]).lp;
			r = svf_tick(&flt[1][k], r, fc, q_stage[k]).lp;
		}
		if (a <= 0.0)
			continue;
		sc->dry.l[i] += a * l;
		sc->dry.r[i] += a * r;
		sc->send.l[i] += a * l * p->send;
		sc->send.r[i] += a * r * p->send;
	}
	sbuf_free(&bus);
}

/* ---- Instrument: band-passed noise -------------------------------------- */

typedef struct {
	curve_t amp;    /* level against scene time */
	curve_t center; /* band centre in Hz against scene time */
	double q;
	double gain;
	double send;
	double width;   /* 0 = same noise both sides, 1 = independent */
} noise_t;

static void render_noise(scene_t *sc, const noise_t *nz)
{
	svf_t fl = { 0.0, 0.0 }, fr = { 0.0, 0.0 };
	size_t i;

	curve_check(&nz->amp, "noise amp", 0);
	curve_check(&nz->center, "noise center", 1);
	for (i = 0; i < sc->n; i++) {
		double t = (double)i / SRD;
		double fc = curve_at_log(&nz->center, t);
		double a = curve_at(&nz->amp, t) * nz->gain;
		double common = rng_bi(&sc->rng);
		double nl = rng_bi(&sc->rng), nr = rng_bi(&sc->rng);
		double w = clampd(nz->width, 0.0, 1.0);
		double xl = (1.0 - w) * common + w * nl;
		double xr = (1.0 - w) * common + w * nr;
		double yl = svf_tick(&fl, xl, fc, nz->q).bpn;
		double yr = svf_tick(&fr, xr, fc, nz->q).bpn;

		if (a <= 0.0)
			continue;
		sc->dry.l[i] += a * yl;
		sc->dry.r[i] += a * yr;
		sc->send.l[i] += a * yl * nz->send;
		sc->send.r[i] += a * yr * nz->send;
	}
}

/* ---- Instrument: the volume tick ---------------------------------------- */

/*
 * A small rounded "tock": a sine whose pitch drops ~18% in the first few
 * milliseconds (that drop is what the ear hears as a soft pop rather than a
 * beep), a faint 2.76:1 partial for a wooden edge, and a tiny high noise
 * click for definition.  Everything is gone in ~50 ms.
 */
static void render_tick(scene_t *sc, double t0, double midi, double gain,
			double send)
{
	const double f0 = midi_hz(midi);
	const double tau = 0.011, tau2 = 0.005, tau_click = 0.0012;
	svf_t click = { 0.0, 0.0 };
	double ph = 0.0, ph2 = 0.0;
	size_t i0 = sec_to_samples(t0), j;

	for (j = 0; i0 + j < sc->n; j++) {
		double t = (double)j / SRD;
		double glide = 1.0 + 0.18 * exp(-t / 0.004);
		double f = f0 * glide;
		double att = env_attack(t, 0.0012);
		double v, c;

		v = sin(TWO_PI * ph) * exp(-t / tau) +
		    0.18 * sin(TWO_PI * ph2) * exp(-t / tau2);
		ph = frac(ph + f / SRD);
		ph2 = frac(ph2 + 2.76 * f / SRD);
		c = svf_tick(&click, rng_bi(&sc->rng), 4200.0, 1.0).bpn *
		    0.05 * exp(-t / tau_click);
		v = att * (v + c) * gain;
		scene_add(sc, i0 + j, v, 1.0, 1.0, send);
		if (t > 0.07)
			break;
	}
}

/* ---- Mastering ---------------------------------------------------------- */

/*
 * Integrated loudness after ITU-R BS.1770: K-weighting (a high-frequency
 * shelf and a low cut, coefficients for 48 kHz), mean square per 400 ms
 * block with 75% overlap, an absolute gate at -70 LUFS and a relative gate
 * 10 LU below the ungated level.  Sounds shorter than one block are
 * measured as one block.
 */
static double measure_lufs(const double *l, const double *r, size_t n)
{
	const size_t block = (size_t)(0.4 * SRD), hop = block / 4;
	double *kl = xcalloc(n, sizeof(double)), *kr = xcalloc(n, sizeof(double));
	double *z, sum, thr, lufs;
	size_t nb, b, i, cnt;
	biquad_t s1[2], s2[2];
	int ch;

	for (ch = 0; ch < 2; ch++) {
		s1[ch].b0 = 1.53512485958697;
		s1[ch].b1 = -2.69169618940638;
		s1[ch].b2 = 1.19839281085285;
		s1[ch].a1 = -1.69065929318241;
		s1[ch].a2 = 0.73248077421585;
		s1[ch].z1 = s1[ch].z2 = 0.0;
		s2[ch].b0 = 1.0;
		s2[ch].b1 = -2.0;
		s2[ch].b2 = 1.0;
		s2[ch].a1 = -1.99004745483398;
		s2[ch].a2 = 0.99007225036621;
		s2[ch].z1 = s2[ch].z2 = 0.0;
	}
	for (i = 0; i < n; i++) {
		kl[i] = bq_tick(&s2[0], bq_tick(&s1[0], l[i]));
		kr[i] = bq_tick(&s2[1], bq_tick(&s1[1], r[i]));
	}

	if (n < block) {
		double zl = 0.0, zr = 0.0;

		for (i = 0; i < n; i++) {
			zl += kl[i] * kl[i];
			zr += kr[i] * kr[i];
		}
		free(kl);
		free(kr);
		sum = (zl + zr) / (double)(n ? n : 1);
		return sum > 0.0 ? -0.691 + 10.0 * log10(sum) : -120.0;
	}

	nb = (n - block) / hop + 1;
	z = xcalloc(nb, sizeof(double));
	for (b = 0; b < nb; b++) {
		double zl = 0.0, zr = 0.0;

		for (i = b * hop; i < b * hop + block; i++) {
			zl += kl[i] * kl[i];
			zr += kr[i] * kr[i];
		}
		z[b] = (zl + zr) / (double)block;
	}

	/* Absolute gate. */
	sum = 0.0;
	cnt = 0;
	for (b = 0; b < nb; b++) {
		if (z[b] > 0.0 && -0.691 + 10.0 * log10(z[b]) > -70.0) {
			sum += z[b];
			cnt++;
		}
	}
	if (!cnt) {
		free(z);
		free(kl);
		free(kr);
		return -120.0;
	}
	/* Relative gate. */
	thr = -0.691 + 10.0 * log10(sum / (double)cnt) - 10.0;
	sum = 0.0;
	cnt = 0;
	for (b = 0; b < nb; b++) {
		if (z[b] > 0.0 && -0.691 + 10.0 * log10(z[b]) > thr) {
			sum += z[b];
			cnt++;
		}
	}
	lufs = cnt ? -0.691 + 10.0 * log10(sum / (double)cnt) : -120.0;
	free(z);
	free(kl);
	free(kr);
	return lufs;
}

static double peak_of(const sbuf_t *b)
{
	double p = 0.0;
	size_t i;

	for (i = 0; i < b->n; i++) {
		if (fabs(b->l[i]) > p)
			p = fabs(b->l[i]);
		if (fabs(b->r[i]) > p)
			p = fabs(b->r[i]);
	}
	return p;
}

/*
 * Look-ahead peak limiter, offline.  req[i] is the gain sample i needs to
 * stay under the ceiling.  hold[i] is the minimum of req over the next
 * `look' samples, so the gain is already down when a peak arrives; a moving
 * average of hold over `look'+1 samples smooths the gain without ever
 * exceeding req at the peak (every value averaged there is <= req[peak]).
 * Recovery is a 60 ms exponential, taken as a min so it cannot undo that
 * guarantee.  Stereo-linked, so the image does not shift.  Returns the
 * deepest gain reduction in dB (0 when nothing was touched).
 */
static double limiter(sbuf_t *b, double ceiling)
{
	const size_t look = (size_t)(0.002 * SRD);
	const double rel = 1.0 - exp(-1.0 / (0.060 * SRD));
	double *req = xcalloc(b->n, sizeof(double));
	double *hold = xcalloc(b->n, sizeof(double));
	/* Window sum of hold[i-look .. i]; frames before the start count as
	 * 1.0 (no reduction), so the sum starts at look+1. */
	double acc = (double)(look + 1), g = 1.0, gmin = 1.0;
	size_t i, k;

	for (i = 0; i < b->n; i++) {
		double a = fmax(fabs(b->l[i]), fabs(b->r[i]));

		req[i] = a > ceiling ? ceiling / a : 1.0;
	}
	for (i = 0; i < b->n; i++) {
		double m = 1.0;

		for (k = i; k <= i + look && k < b->n; k++)
			if (req[k] < m)
				m = req[k];
		hold[i] = m;
	}
	for (i = 0; i < b->n; i++) {
		double s;

		acc += hold[i];
		acc -= i >= look + 1 ? hold[i - look - 1] : 1.0;
		s = acc / (double)(look + 1);
		if (s > 1.0) /* rounding */
			s = 1.0;
		g = fmin(s, g + (1.0 - g) * rel);
		if (g < gmin)
			gmin = g;
		b->l[i] *= g;
		b->r[i] *= g;
	}
	free(req);
	free(hold);
	return lin_to_db(gmin);
}

/*
 * Remove DC without touching the ends: subtract m * w(i), w a Hann window
 * (zero at both ends), with m chosen so the channel's sum becomes zero.
 */
static void remove_dc(double *x, size_t n)
{
	double sx = 0.0, sw = 0.0, m;
	size_t i;

	if (n < 3)
		return;
	for (i = 0; i < n; i++) {
		double w = sin(PI * (double)i / (double)(n - 1));

		sx += x[i];
		sw += w * w;
	}
	m = sx / sw;
	for (i = 0; i < n; i++) {
		double w = sin(PI * (double)i / (double)(n - 1));

		x[i] -= m * w * w;
	}
}

/* Raised-cosine fades: the first and the last sample are exactly zero. */
static void apply_fades(sbuf_t *b, double fade_in, double fade_out)
{
	size_t mi = sec_to_samples(fade_in), mo = sec_to_samples(fade_out), i;

	if (mi > b->n)
		mi = b->n;
	if (mo > b->n)
		mo = b->n;
	for (i = 0; i < mi; i++) {
		double w = 0.5 - 0.5 * cos(PI * (double)i / (double)mi);

		b->l[i] *= w;
		b->r[i] *= w;
	}
	for (i = 0; i < mo; i++) {
		size_t idx = b->n - mo + i;
		double w = mo > 1 ? 0.5 + 0.5 * cos(PI * (double)i / (double)(mo - 1)) : 0.0;

		b->l[idx] *= w;
		b->r[idx] *= w;
	}
}

typedef struct {
	double peak_raw_db;   /* before gain */
	double lufs_raw;
	double gain_db;
	const char *bound;    /* which target set the gain */
	double limit_db;      /* deepest limiter reduction */
	double lufs_out;
	double peak_out_db;   /* after quantisation */
	double dc_lsb[2];     /* mean per channel, in LSB */
	double rms_db;
} master_report_t;

typedef struct sound_def sound_def_t;

struct sound_def {
	const char *name;
	const char *desc;
	double seconds;
	double fade_in, fade_out;
	double peak_db; /* ceiling */
	double lufs;    /* loudness target; the quieter of the two wins */
	uint64_t seed;
	void (*build)(scene_t *sc);
};

/* The headroom the gain stage may leave to the limiter, in dB. */
#define LIMITER_ALLOWANCE_DB 1.5

static void master(const sound_def_t *def, sbuf_t *mix, master_report_t *rep)
{
	biquad_t hp[2][2], shelf[2];
	double g_peak, g_loud, g, peak;
	size_t i;
	int ch;

	/* Rumble high-pass (4th order: two 2nd-order sections) and a gentle
	 * treble shelf so nothing reads as hissy or sharp on small
	 * speakers. */
	for (ch = 0; ch < 2; ch++) {
		bq_highpass(&hp[ch][0], 32.0, 0.5412);
		bq_highpass(&hp[ch][1], 32.0, 1.3066);
		bq_highshelf(&shelf[ch], 8500.0, -2.0, 1.0);
	}
	for (i = 0; i < mix->n; i++) {
		mix->l[i] = bq_tick(&shelf[0], bq_tick(&hp[0][1], bq_tick(&hp[0][0], mix->l[i])));
		mix->r[i] = bq_tick(&shelf[1], bq_tick(&hp[1][1], bq_tick(&hp[1][0], mix->r[i])));
	}

	apply_fades(mix, def->fade_in, def->fade_out);

	peak = peak_of(mix);
	if (peak <= 0.0)
		die("%s: silent", def->name);
	rep->peak_raw_db = lin_to_db(peak);
	rep->lufs_raw = measure_lufs(mix->l, mix->r, mix->n);

	g_peak = db_to_lin(def->peak_db + LIMITER_ALLOWANCE_DB) / peak;
	g_loud = db_to_lin(def->lufs - rep->lufs_raw);
	if (g_loud < g_peak) {
		g = g_loud;
		rep->bound = "loudness";
	} else {
		g = g_peak;
		rep->bound = "peak";
	}
	rep->gain_db = lin_to_db(g);
	for (i = 0; i < mix->n; i++) {
		mix->l[i] *= g;
		mix->r[i] *= g;
	}

	rep->limit_db = limiter(mix, db_to_lin(def->peak_db));
	remove_dc(mix->l, mix->n);
	remove_dc(mix->r, mix->n);
	rep->lufs_out = measure_lufs(mix->l, mix->r, mix->n);
}

/*
 * TPDF dither: the difference of two uniform variables is triangular over
 * +-1 LSB, which makes the quantisation error independent of the signal
 * (no distortion on quiet tails, just a steady noise floor ~-96 dBFS).  The
 * dither is ramped in and out over the first and last 512 frames, where the
 * fades have brought the signal to zero, so the files begin and end on
 * exact digital silence.
 */
static int16_t *quantise(const sbuf_t *b, rng_t *rng, master_report_t *rep)
{
	const size_t edge = 512;
	int16_t *out = xcalloc(b->n * 2, sizeof(int16_t));
	double sum[2] = { 0.0, 0.0 }, sq = 0.0;
	int peak = 0;
	size_t i;

	for (i = 0; i < b->n; i++) {
		size_t dist = i < b->n - 1 - i ? i : b->n - 1 - i;
		double dw = dist >= edge ? 1.0 : (double)dist / (double)edge;
		const double x[2] = { b->l[i], b->r[i] };
		int ch;

		for (ch = 0; ch < 2; ch++) {
			double v = x[ch] * 32767.0 + dw * (rng_unit(rng) - rng_unit(rng));
			long s = lrint(v);

			if (s > 32767 || s < -32768)
				die("clipping at frame %zu (%ld)", i, s);
			out[i * 2 + ch] = (int16_t)s;
			sum[ch] += (double)s;
			sq += (double)s * (double)s;
			if (labs(s) > peak)
				peak = (int)labs(s);
		}
	}
	rep->peak_out_db = lin_to_db((double)peak / 32767.0);
	rep->dc_lsb[0] = sum[0] / (double)b->n;
	rep->dc_lsb[1] = sum[1] / (double)b->n;
	rep->rms_db = lin_to_db(sqrt(sq / (double)(2 * b->n)) / 32767.0);
	if (peak >= 32767)
		die("output reaches full scale");
	if (fabs(rep->dc_lsb[0]) > 2.0 || fabs(rep->dc_lsb[1]) > 2.0)
		die("DC offset %.2f / %.2f LSB", rep->dc_lsb[0], rep->dc_lsb[1]);
	return out;
}

/* ---- WAV output --------------------------------------------------------- */

static void put_le16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put_le32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
}

/*
 * The canonical 44-byte header (RIFF, one "fmt " chunk of 16 bytes, PCM
 * format 1, then "data"), with no optional chunks, so even the simplest
 * reader finds the samples at offset 44.  Written to a temporary name and
 * renamed, so an interrupted run never leaves a truncated file behind.
 */
static void write_wav(const char *path, const int16_t *pcm, size_t frames)
{
	const unsigned channels = 2, bits = 16;
	const uint32_t data_bytes = (uint32_t)(frames * channels * (bits / 8));
	unsigned char hdr[44];
	unsigned char *bytes;
	char tmp[1100];
	FILE *f;
	size_t i;

	if (frames > 0x3fffffff / 4)
		die("%s: too long", path);
	memcpy(hdr + 0, "RIFF", 4);
	put_le32(hdr + 4, 36u + data_bytes);
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	put_le32(hdr + 16, 16);
	put_le16(hdr + 20, 1);
	put_le16(hdr + 22, channels);
	put_le32(hdr + 24, SR);
	put_le32(hdr + 28, SR * channels * (bits / 8));
	put_le16(hdr + 32, channels * (bits / 8));
	put_le16(hdr + 34, bits);
	memcpy(hdr + 36, "data", 4);
	put_le32(hdr + 40, data_bytes);

	bytes = xcalloc(data_bytes, 1);
	for (i = 0; i < frames * channels; i++)
		put_le16(bytes + i * 2, (unsigned)(uint16_t)pcm[i]);

	if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
		die("path too long: %s", path);
	f = fopen(tmp, "wb");
	if (!f)
		die("%s: %s", tmp, strerror(errno));
	if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
	    fwrite(bytes, 1, data_bytes, f) != data_bytes)
		die("%s: write failed: %s", tmp, strerror(errno));
	if (fclose(f) != 0)
		die("%s: close failed: %s", tmp, strerror(errno));
	if (rename(tmp, path) != 0)
		die("rename %s -> %s: %s", tmp, path, strerror(errno));
	free(bytes);
}

/* ---- The sounds --------------------------------------------------------- */

/*
 * A shared hall for the two long sounds (decay set per sound), a little pre-delay so
 * the strikes stay clear of their own reflections, filtered input so the tail
 * is neither muddy nor bright.
 */
static reverb_cfg_t hall(double rt60, double wet)
{
	reverb_cfg_t r;

	r.rt60 = rt60;
	r.predelay = 0.024;
	r.damp = 0.38;
	r.size = 1.18;
	r.width = 0.92;
	r.hp_hz = 170.0;
	r.lp_hz = 7200.0;
	r.er_level = 0.30;
	r.wet = wet;
	return r;
}

/* A small, bright room for the short event sounds. */
static reverb_cfg_t room(double rt60, double wet)
{
	reverb_cfg_t r;

	r.rt60 = rt60;
	r.predelay = 0.012;
	r.damp = 0.30;
	r.size = 0.85;
	r.width = 0.85;
	r.hp_hz = 220.0;
	r.lp_hz = 8000.0;
	r.er_level = 0.25;
	r.wet = wet;
	return r;
}

/* Both layers of the chime voice: FM body plus vibraphone bar. */
static void chime(scene_t *sc, const note_t *n, double ep_gain, double bar_gain)
{
	render_epiano(sc, n, ep_gain);
	render_bar(sc, &bar_vibes, n, bar_gain);
}

/*
 * startup: D-flat major 9 (Db F Ab C Eb), rolled upward on the chime voice
 * from Ab3 to Eb5, crowned by F5 a beat later -- the third on top, open and
 * resolved.  The notes walk from left to right as they rise.  Underneath, a
 * pad voiced Db2 (sine sub) / Db3 Ab3 Eb4 F4 C5 swells in as its filter
 * opens, peaking just after the arpeggio lands, then settles and closes as
 * everything fades into the hall.
 */
static void build_startup(scene_t *sc)
{
	static const double arp_midi[] = { AB3, DB4, F4, AB4, C5, EB5, F5 };
	static const double arp_t[] = { 0.050, 0.165, 0.275, 0.380, 0.480, 0.580, 0.800 };
	static const double arp_vel[] = { 0.70, 0.73, 0.77, 0.80, 0.78, 0.74, 0.66 };
	const int n_arp = (int)(sizeof arp_midi / sizeof arp_midi[0]);
	pad_t pad;
	noise_t air;
	note_t n;
	int k;

	sc->rv = hall(2.6, 1.15);
	sc->has_reverb = 1;

	memset(&pad, 0, sizeof pad);
	pad_note(&pad, DB2, 0.22, PAD_SINE);
	pad_note(&pad, DB3, 0.40, PAD_SAW);
	pad_note(&pad, AB3, 0.40, PAD_SAW);
	pad_note(&pad, EB4, 0.24, PAD_SAW);
	pad_note(&pad, F4, 0.34, PAD_SAW);
	pad_note(&pad, C5, 0.20, PAD_SAW);
	pad.amp = (curve_t){ 5, { 0.00, 0.20, 1.30, 2.20, 3.50 },
			     { 0.00, 0.10, 1.00, 0.80, 0.00 } };
	pad.cutoff = (curve_t){ 5, { 0.00, 0.60, 1.35, 2.30, 3.50 },
				{ 240.0, 650.0, 2000.0, 1250.0, 420.0 } };
	pad.gain = 0.17;
	pad.send = 0.32;
	pad.width = 0.65;
	pad.detune = 7.0;
	pad.drift = 2.5;
	render_pad(sc, &pad);

	/* The root, struck softly with the first arpeggio note; shorter
	 * than its natural ring so the tail belongs to the chord, not to a
	 * low hum. */
	n = note(0.050, DB3, 0.56, 0.0, 3.0, 0.8, 0.30, 0.35);
	n.decay = 0.65;
	render_epiano(sc, &n, 0.15);

	for (k = 0; k < n_arp; k++) {
		double pan = k < n_arp - 1 ? -0.45 + 0.90 * (double)k / (double)(n_arp - 2) : 0.15;
		double t0 = arp_t[k];

		n = note(t0, arp_midi[k], arp_vel[k], pan, 3.05 - t0, 0.75,
			 0.50 + 0.03 * (double)k, 0.42);
		n.trem = 0.10;
		chime(sc, &n, 0.20, 0.15);
	}

	/* Air: a breath of high noise that rises with the pad. */
	memset(&air, 0, sizeof air);
	air.amp = (curve_t){ 4, { 0.0, 1.40, 2.40, 3.40 }, { 0.0, 1.0, 0.6, 0.0 } };
	air.center = (curve_t){ 3, { 0.0, 1.50, 3.40 }, { 5200.0, 8800.0, 6500.0 } };
	air.q = 0.8;
	air.gain = 0.020;
	air.send = 0.6;
	air.width = 1.0;
	render_noise(sc, &air);
}

/*
 * shutdown: the same voice, a calm descent through the chord -- Eb5 C5 Ab4
 * F4 -- quick steps, each a little later than the last, landing on the tonic Db4
 * with Ab3 and Db3 beneath.  The pad (Db add9) is there from the start and
 * fades as its filter closes, so the sound darkens as it ends.  Moves right
 * to left, the mirror of start-up.
 */
static void build_shutdown(scene_t *sc)
{
	static const double fig_midi[] = { EB5, C5, AB4, F4 };
	static const double fig_t[] = { 0.030, 0.120, 0.215, 0.315 };
	static const double fig_vel[] = { 0.66, 0.64, 0.62, 0.60 };
	static const double fig_pan[] = { 0.40, 0.18, -0.05, -0.25 };
	pad_t pad;
	note_t n;
	int k;

	sc->rv = hall(1.1, 1.00);
	sc->has_reverb = 1;

	memset(&pad, 0, sizeof pad);
	pad_note(&pad, DB2, 0.22, PAD_SINE);
	pad_note(&pad, DB3, 0.48, PAD_SAW);
	pad_note(&pad, AB3, 0.40, PAD_SAW);
	pad_note(&pad, EB4, 0.22, PAD_SAW);
	pad_note(&pad, F4, 0.28, PAD_SAW);
	pad.amp = (curve_t){ 4, { 0.00, 0.18, 0.55, 1.35 },
			     { 0.00, 0.75, 0.65, 0.00 } };
	pad.cutoff = (curve_t){ 4, { 0.00, 0.30, 0.70, 1.35 },
				{ 1500.0, 1700.0, 900.0, 260.0 } };
	pad.gain = 0.12;
	pad.send = 0.32;
	pad.width = 0.65;
	pad.detune = 7.0;
	pad.drift = 2.5;
	render_pad(sc, &pad);

	for (k = 0; k < 4; k++) {
		n = note(fig_t[k], fig_midi[k], fig_vel[k], fig_pan[k],
			 0.85 - fig_t[k], 0.35, 0.56 - 0.04 * (double)k, 0.42);
		n.trem = 0.08;
		chime(sc, &n, 0.20, 0.15);
	}

	/* The resolution: tonic on top, fifth and root below. */
	n = note(0.440, DB4, 0.58, -0.10, 0.55, 0.40, 0.40, 0.42);
	n.trem = 0.08;
	chime(sc, &n, 0.20, 0.15);
	n = note(0.445, AB3, 0.42, 0.10, 0.55, 0.40, 0.32, 0.40);
	chime(sc, &n, 0.16, 0.12);
	n = note(0.450, DB3, 0.45, 0.0, 0.55, 0.40, 0.28, 0.38);
	n.decay = 0.8;
	render_epiano(sc, &n, 0.16);
}

/*
 * login: a bright rising fourth, Ab4 to Db5 -- the dominant lifting to the
 * tonic, "welcome" -- with F5 sounding softly above the second note.
 */
static void build_login(scene_t *sc)
{
	note_t n;

	sc->rv = room(1.3, 1.0);
	sc->has_reverb = 1;

	n = note(0.020, AB4, 0.70, -0.15, 0.75, 0.30, 0.65, 0.30);
	chime(sc, &n, 0.20, 0.15);
	n = note(0.160, DB5, 0.78, 0.15, 0.70, 0.30, 0.68, 0.30);
	chime(sc, &n, 0.20, 0.15);
	n = note(0.163, F5, 0.45, 0.25, 0.65, 0.30, 0.60, 0.30);
	render_bar(sc, &bar_glass, &n, 0.12);
}

/* logout: the mirror, Db5 falling to Ab4 with F4 beneath; darker. */
static void build_logout(scene_t *sc)
{
	note_t n;

	sc->rv = room(0.8, 1.0);
	sc->has_reverb = 1;

	n = note(0.015, DB5, 0.70, 0.15, 0.35, 0.20, 0.45, 0.30);
	chime(sc, &n, 0.20, 0.15);
	n = note(0.115, AB4, 0.68, -0.15, 0.35, 0.20, 0.40, 0.30);
	chime(sc, &n, 0.20, 0.15);
	n = note(0.118, F4, 0.45, -0.25, 0.32, 0.20, 0.35, 0.30);
	render_epiano(sc, &n, 0.16);
}

/*
 * notify: a gentle "ding-dong", C6 falling a major third to Ab5, on the
 * glass bell with a little FM body underneath for warmth.
 */
static void build_notify(scene_t *sc)
{
	note_t n;

	sc->rv = room(0.9, 0.9);
	sc->has_reverb = 1;

	n = note(0.010, C6, 0.66, 0.10, 0.35, 0.20, 0.45, 0.30);
	render_bar(sc, &bar_glass, &n, 0.18);
	n.vel = 0.45;
	render_epiano(sc, &n, 0.10);

	n = note(0.130, AB5, 0.60, -0.10, 0.25, 0.20, 0.42, 0.30);
	render_bar(sc, &bar_glass, &n, 0.18);
	n.vel = 0.42;
	render_epiano(sc, &n, 0.10);
}

/*
 * error: soft and low, not an alarm -- the falling minor third Eb4 to C4,
 * the gentle "uh-oh", on a muted marimba (dark, short decay) with the FM
 * body under it for roundness and a quiet lower octave for weight.  Low for
 * this palette, yet high enough that a laptop speaker still plays it: a
 * fundamental much under 250 Hz would mostly vanish there.
 */
static void build_error(scene_t *sc)
{
	static const double m[] = { EB4, C4 };
	static const double t[] = { 0.010, 0.135 };
	static const double v[] = { 0.74, 0.70 };
	static const double p[] = { -0.06, 0.06 };
	note_t n;
	int k;

	sc->rv = room(0.7, 0.8);
	sc->has_reverb = 1;

	for (k = 0; k < 2; k++) {
		n = note(t[k], m[k], v[k], p[k], 0.30 - 0.08 * k, 0.10, 0.25, 0.20);
		n.decay = 0.55;
		render_bar(sc, &bar_marimba, &n, 0.24);
		n.bright = 0.15;
		render_epiano(sc, &n, 0.13);
		n.midi -= 12.0;
		n.vel = 0.5;
		n.decay = 0.45;
		render_epiano(sc, &n, 0.08);
	}
}

/* volume: one soft tock at Bb5, centred, almost dry. */
static void build_volume(scene_t *sc)
{
	sc->rv = room(0.25, 0.5);
	sc->has_reverb = 1;
	render_tick(sc, 0.001, BB5, 0.5, 0.08);
}

/*
 * device-added: a quick upward pluck, Db5 to Ab5, with a breath of noise
 * sweeping up behind it.
 */
static void build_device_added(scene_t *sc)
{
	noise_t sw;
	note_t n;

	sc->rv = room(0.7, 0.9);
	sc->has_reverb = 1;

	n = note(0.010, DB5, 0.70, -0.12, 0.12, 0.15, 0.70, 0.25);
	n.decay = 0.45;
	render_epiano(sc, &n, 0.20);
	render_bar(sc, &bar_glass, &n, 0.10);
	n = note(0.085, AB5, 0.74, 0.12, 0.12, 0.15, 0.72, 0.25);
	n.decay = 0.45;
	render_epiano(sc, &n, 0.20);
	render_bar(sc, &bar_glass, &n, 0.10);

	memset(&sw, 0, sizeof sw);
	sw.amp = (curve_t){ 3, { 0.0, 0.06, 0.24 }, { 0.0, 1.0, 0.0 } };
	sw.center = (curve_t){ 2, { 0.0, 0.20 }, { 1500.0, 6000.0 } };
	sw.q = 1.4;
	sw.gain = 0.030;
	sw.send = 0.3;
	sw.width = 0.6;
	render_noise(sc, &sw);
}

/* device-removed: the mirror, Ab5 down to Db5, the sweep falling. */
static void build_device_removed(scene_t *sc)
{
	noise_t sw;
	note_t n;

	sc->rv = room(0.7, 0.9);
	sc->has_reverb = 1;

	n = note(0.010, AB5, 0.70, 0.12, 0.12, 0.15, 0.55, 0.25);
	n.decay = 0.45;
	render_epiano(sc, &n, 0.20);
	render_bar(sc, &bar_glass, &n, 0.10);
	n = note(0.085, DB5, 0.70, -0.12, 0.12, 0.15, 0.50, 0.25);
	n.decay = 0.45;
	render_epiano(sc, &n, 0.20);
	render_bar(sc, &bar_glass, &n, 0.10);

	memset(&sw, 0, sizeof sw);
	sw.amp = (curve_t){ 3, { 0.0, 0.06, 0.24 }, { 0.0, 1.0, 0.0 } };
	sw.center = (curve_t){ 2, { 0.0, 0.20 }, { 6000.0, 1500.0 } };
	sw.q = 1.4;
	sw.gain = 0.030;
	sw.send = 0.3;
	sw.width = 0.6;
	render_noise(sc, &sw);
}

/*
 * message: a warm wooden triplet up the tonic triad, Db5 F5 Ab5, on the
 * marimba, the last note doubled softly by the glass bell so it hangs.
 */
static void build_message(scene_t *sc)
{
	static const double m[] = { DB5, F5, AB5 };
	static const double t[] = { 0.010, 0.078, 0.146 };
	static const double v[] = { 0.62, 0.66, 0.72 };
	static const double p[] = { -0.20, 0.0, 0.20 };
	note_t n;
	int k;

	sc->rv = room(1.0, 1.0);
	sc->has_reverb = 1;

	for (k = 0; k < 3; k++) {
		n = note(t[k], m[k], v[k], p[k], 0.45 - t[k], 0.15, 0.62, 0.30);
		render_bar(sc, &bar_marimba, &n, 0.22);
	}
	n = note(t[2] + 0.003, AB5, 0.40, 0.25, 0.35, 0.15, 0.40, 0.35);
	render_bar(sc, &bar_glass, &n, 0.10);
}

/*
 * Targets.  The start-up chime peaks at -3 dBFS and is the loudest sound;
 * the others are 3-11 dB quieter so they sit under whatever is playing.
 * Each sound's gain is the lower of what its peak ceiling and its loudness
 * target allow, so a sound with an unusually soft attack does not get pushed
 * up to its ceiling.
 */
static const sound_def_t sounds[] = {
	{ "startup", "start-up chime: Db major 9 rolled over a swelling pad",
	  4.00, 0.002, 0.60, -3.0, -15.6, 0x5354415254555031ull, build_startup },
	{ "shutdown", "shut-down: descending figure resolving to the tonic",
	  1.40, 0.002, 0.30, -4.0, -17.4, 0x53485554444f574eull, build_shutdown },
	{ "login", "log-in: rising fourth Ab4-Db5",
	  1.20, 0.002, 0.35, -6.0, -20.0, 0x4c4f47494e000001ull, build_login },
	{ "logout", "log-out: falling fourth Db5-Ab4",
	  0.75, 0.002, 0.20, -7.0, -21.0, 0x4c4f474f55540001ull, build_logout },
	{ "notify", "notification: glass ding-dong C6-Ab5",
	  0.60, 0.002, 0.18, -8.0, -21.0, 0x4e4f544946590001ull, build_notify },
	{ "error", "error: soft falling minor third Eb4-C4",
	  0.50, 0.002, 0.12, -8.0, -21.0, 0x4552524f52000001ull, build_error },
	{ "volume", "volume-change tick",
	  0.08, 0.0005, 0.020, -14.0, -26.0, 0x564f4c554d450001ull, build_volume },
	{ "device-added", "device plugged in: upward pluck and sweep",
	  0.40, 0.002, 0.10, -9.0, -22.0, 0x4445564144440001ull, build_device_added },
	{ "device-removed", "device removed: downward pluck and sweep",
	  0.40, 0.002, 0.10, -9.0, -22.0, 0x44455652454d0001ull, build_device_removed },
	{ "message", "message: marimba triad Db5-F5-Ab5",
	  0.70, 0.002, 0.20, -8.0, -21.0, 0x4d45535341474501ull, build_message },
};

static void make_sound(const char *dir, const sound_def_t *def)
{
	char path[1024];
	master_report_t rep;
	scene_t sc;
	sbuf_t mix;
	int16_t *pcm;
	rng_t dither;
	size_t i;

	memset(&sc, 0, sizeof sc);
	memset(&rep, 0, sizeof rep);
	sc.n = sec_to_samples(def->seconds);
	if (sc.n < 64)
		die("%s: too short", def->name);
	sbuf_init(&sc.dry, sc.n);
	sbuf_init(&sc.send, sc.n);
	sc.rng.s = def->seed;

	def->build(&sc);

	/* mix = dry + reverb(send) */
	sbuf_init(&mix, sc.n);
	for (i = 0; i < sc.n; i++) {
		mix.l[i] = sc.dry.l[i];
		mix.r[i] = sc.dry.r[i];
	}
	if (sc.has_reverb)
		reverb_process(&sc.rv, &sc.send, &mix);

	master(def, &mix, &rep);
	dither.s = def->seed ^ 0xD17E7D17E7D17E7Dull;
	pcm = quantise(&mix, &dither, &rep);

	if ((size_t)snprintf(path, sizeof path, "%s/%s.wav", dir, def->name) >= sizeof path)
		die("path too long");
	write_wav(path, pcm, sc.n);

	printf("%-15s %5.2f s  raw %6.1f dBFS %6.1f LUFS  gain %+6.1f dB (%-8s) "
	       "lim %5.2f dB  out: peak %5.1f dBFS  %6.1f LUFS  rms %6.1f dBFS  "
	       "dc %+.2f/%+.2f LSB\n",
	       def->name, (double)sc.n / SRD, rep.peak_raw_db, rep.lufs_raw,
	       rep.gain_db, rep.bound, rep.limit_db, rep.peak_out_db,
	       rep.lufs_out, rep.rms_db, rep.dc_lsb[0], rep.dc_lsb[1]);

	free(pcm);
	sbuf_free(&mix);
	sbuf_free(&sc.dry);
	sbuf_free(&sc.send);
}

/* mkdir -p */
static void make_dirs(const char *dir)
{
	char buf[1024];
	size_t i, len = strlen(dir);

	if (len == 0 || len >= sizeof buf)
		die("bad output directory");
	memcpy(buf, dir, len + 1);
	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST)
			die("%s: %s", buf, strerror(errno));
		if (i < len)
			buf[i] = '/';
	}
}

int main(int argc, char **argv)
{
	const char *dir;
	size_t k;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}
	dir = argv[1];
	make_dirs(dir);

	for (k = 0; k < sizeof sounds / sizeof sounds[0]; k++)
		make_sound(dir, &sounds[k]);
	return 0;
}
