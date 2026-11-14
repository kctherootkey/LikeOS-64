/* libinstaller -- how far the installation is, and how long it still takes.
 *
 * The weights say what share of the whole each step is.  Inside the
 * unpacking, the share done is not the share of the bytes: the image holds
 * a few big files and thousands of small ones, and on a real disk a small
 * file costs far more than its bytes (it is created, written, given its
 * owner, mode, times and attributes).  So the time of the unpacking is
 * modelled as
 *
 *	seconds = a * MiB + b * files
 *
 * and a and b are fitted while it runs: every IL_TRACK_WINDOW_MS one
 * sample (seconds, MiB, files) goes into the normal equations of a least
 * squares fit whose older samples fade (time constant IL_TRACK_FORGET_S).
 * Where the samples cannot tell bytes and files apart (the big files come
 * first, then only small ones), a weak pull towards b = a * 256 KiB keeps
 * the fit defined; the data outweighs it as soon as it says something.
 * The share done is then (a * MiB done + b * files done) / (the same of
 * the totals), the time left a * MiB left + b * files left.
 *
 * The steps after the unpacking (configure, boot loader, finalize with its
 * sync) get the unpacking's time per unit of weight; configure and the boot
 * loader are scaled by how the finished ones measured against their
 * estimate, the finalize step is not (its sync writes what the unpacking
 * left in the cache, which says nothing about how fast configure was).  The
 * step running counts with what its own time and fraction say.  The time left a front end sees
 * runs down with the clock, follows the model with a lag (faster up than
 * down, so it does not jump about), and never falls below 80 % of what the
 * model says, so a short time is never shown while much work is left.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <string.h>

#include "il_internal.h"

#define MIB                  (1024.0 * 1024.0)
#define IL_TRACK_FORGET_S    60.0   /* fading of old samples */
#define IL_TRACK_PRIOR       25.0   /* weight of the default file cost */
#define IL_TRACK_ETA_UP_S    4.0    /* lag of the time left, growing */
#define IL_TRACK_ETA_DOWN_S  12.0   /* ... and shrinking */
#define IL_TRACK_ETA_FLOOR   0.8    /* never below this share of the model */
#define IL_TRACK_STEP_MIN_S  2.0    /* the least a later step is given */
#define IL_TRACK_GLIDE_S     2.0    /* the unpacking's share catching up */

static const double g_weights[IL_STEP_COUNT] = {
	0.01, /* prepare */
	0.02, /* partition */
	0.05, /* format */
	0.01, /* mount */
	0.75, /* extract */
	0.06, /* configure */
	0.03, /* bootloader */
	0.07, /* finalize */
};

double il_step_weight(int step)
{
	if (step < 0 || step >= IL_STEP_COUNT)
		return 0.0;
	return g_weights[step];
}

void il_track_init(struct il_track *t, uint64_t now_ms)
{
	memset(t, 0, sizeof(*t));
	t->eta = -1.0;
	t->step_t0[0] = now_ms ? now_ms : 1;
}

void il_track_totals(struct il_track *t, uint64_t bytes, uint64_t files,
		     uint64_t compressed)
{
	t->bytes_total = bytes;
	t->files_total = files;
	t->comp_total = compressed;
}

void il_track_step(struct il_track *t, int step, uint64_t now_ms)
{
	if (step < 0 || step >= IL_STEP_COUNT)
		return;
	if (now_ms == 0)
		now_ms = 1;
	for (int i = 0; i < step; i++)
		if (t->step_t0[i] && !t->step_ms[i])
			t->step_ms[i] = now_ms > t->step_t0[i] ?
					now_ms - t->step_t0[i] : 1;
	if (step != t->step || !t->step_t0[step]) {
		t->step = step;
		t->frac = 0.0;
		t->step_t0[step] = now_ms;
		t->step_ms[step] = 0;
	}
}

void il_track_fraction(struct il_track *t, double fraction)
{
	if (fraction > 1.0)
		fraction = 1.0;
	if (fraction > t->frac)
		t->frac = fraction;
}

/* ---- the fit ----------------------------------------------------------- */

/* The coefficients in use: fitted, or the default (only their ratio
 * matters then). */
static void il_track_costs(const struct il_track *t, double *a, double *b)
{
	if (t->samples > 0 && t->cost_mib + t->cost_file > 0.0) {
		*a = t->cost_mib;
		*b = t->cost_file;
	} else {
		*a = 1.0;
		*b = IL_TRACK_FILE_KIB / 1024.0;
	}
}

static void il_track_solve(struct il_track *t)
{
	const double k = IL_TRACK_FILE_KIB / 1024.0;   /* MiB per file */
	const double l = IL_TRACK_PRIOR;
	double m11 = t->fit_xx + l * k * k;
	double m12 = t->fit_xy - l * k;
	double m22 = t->fit_yy + l;
	double det = m11 * m22 - m12 * m12;
	double a, b;

	if (det <= 1e-12 * (m11 * m22 + 1e-30))
		return;
	a = (t->fit_xt * m22 - m12 * t->fit_yt) / det;
	b = (m11 * t->fit_yt - m12 * t->fit_xt) / det;
	/* Costs are not negative: fit the other one alone then. */
	if (b < 0.0) {
		b = 0.0;
		a = t->fit_xx > 0.0 ? t->fit_xt / t->fit_xx : 0.0;
	} else if (a < 0.0) {
		a = 0.0;
		b = t->fit_yy > 0.0 ? t->fit_yt / t->fit_yy : 0.0;
	}
	if (a < 0.0)
		a = 0.0;
	if (b < 0.0)
		b = 0.0;
	if (a + b <= 0.0)
		return;
	t->cost_mib = a;
	t->cost_file = b;
	t->samples++;
}

void il_track_extract(struct il_track *t, uint64_t now_ms, uint64_t bytes,
		      uint64_t files, uint64_t compressed)
{
	double x, y, s, keep;

	if (bytes > t->bytes_done)
		t->bytes_done = bytes;
	if (files > t->files_done)
		t->files_done = files;
	if (compressed > t->comp_done)
		t->comp_done = compressed;
	if (!t->win_ms) {
		t->win_ms = now_ms ? now_ms : 1;
		t->win_bytes = t->bytes_done;
		t->win_files = t->files_done;
		return;
	}
	if (now_ms < t->win_ms + IL_TRACK_WINDOW_MS)
		return;
	s = (double)(now_ms - t->win_ms) / 1000.0;
	x = (double)(t->bytes_done - t->win_bytes) / MIB;
	y = (double)(t->files_done - t->win_files);
	/* Nothing moved (a stall): that time still belongs to the work, so
	 * the window stays open and the next sample carries it. */
	if (x <= 0.0 && y <= 0.0)
		return;
	t->win_ms = now_ms;
	t->win_bytes = t->bytes_done;
	t->win_files = t->files_done;
	keep = IL_TRACK_FORGET_S / (IL_TRACK_FORGET_S + s);
	t->fit_xx = t->fit_xx * keep + x * x;
	t->fit_xy = t->fit_xy * keep + x * y;
	t->fit_yy = t->fit_yy * keep + y * y;
	t->fit_xt = t->fit_xt * keep + x * s;
	t->fit_yt = t->fit_yt * keep + y * s;
	il_track_solve(t);
}

/* The share of the unpacking done, by the model (not yet monotonic). */
static double il_track_extract_share(const struct il_track *t)
{
	double a, b, done, total;
	uint64_t bd = t->bytes_done, fd = t->files_done;

	if (!t->bytes_total && !t->files_total) {
		if (!t->comp_total)
			return 0.0;
		return (double)t->comp_done / (double)t->comp_total;
	}
	if (bd > t->bytes_total)
		bd = t->bytes_total;
	if (fd > t->files_total)
		fd = t->files_total;
	il_track_costs(t, &a, &b);
	done = a * (double)bd / MIB + b * (double)fd;
	total = a * (double)t->bytes_total / MIB + b * (double)t->files_total;
	if (total <= 0.0)
		return 0.0;
	return done / total;
}

/* ---- the time left ------------------------------------------------------ */

/* Seconds the unpacking still needs, el_s seconds into it; -1 while the
 * fit knows nothing. */
static double il_track_extract_left(const struct il_track *t, double el_s)
{
	double mib, files;

	if (!t->bytes_total && !t->files_total) {
		/* An image without its sizes: the archive's own pace. */
		if (!t->comp_total || !t->comp_done)
			return -1.0;
		return el_s * (double)(t->comp_total > t->comp_done ?
				       t->comp_total - t->comp_done : 0) /
		       (double)t->comp_done;
	}
	if (t->samples < 2)
		return -1.0;
	mib = t->bytes_total > t->bytes_done ?
	      (double)(t->bytes_total - t->bytes_done) / MIB : 0.0;
	files = t->files_total > t->files_done ?
		(double)(t->files_total - t->files_done) : 0.0;
	return t->cost_mib * mib + t->cost_file * files;
}

double il_track_remaining(const struct il_track *t, uint64_t now_ms)
{
	double x_secs, per_weight, scale = 1.0, left = 0.0;
	double est_sum = 0.0, real_sum = 0.0;

	if (t->finished)
		return 0.0;
	if (t->step < IL_STEP_EXTRACT || !t->step_t0[IL_STEP_EXTRACT])
		return -1.0;
	if (t->step == IL_STEP_EXTRACT) {
		uint64_t el = now_ms > t->step_t0[IL_STEP_EXTRACT] ?
			      now_ms - t->step_t0[IL_STEP_EXTRACT] : 0;
		double xl = il_track_extract_left(t, (double)el / 1000.0);

		if (xl < 0.0 || el < IL_TRACK_WARMUP_MS)
			return -1.0;
		x_secs = (double)el / 1000.0 + xl;
		left = xl;
	} else {
		x_secs = (double)t->step_ms[IL_STEP_EXTRACT] / 1000.0;
	}
	per_weight = x_secs / g_weights[IL_STEP_EXTRACT];

	/* How the finished later steps compared with their estimate. */
	for (int s = IL_STEP_EXTRACT + 1; s < t->step; s++) {
		double est = per_weight * g_weights[s];

		if (est < IL_TRACK_STEP_MIN_S)
			est = IL_TRACK_STEP_MIN_S;
		est_sum += est;
		real_sum += (double)t->step_ms[s] / 1000.0;
	}
	if (est_sum > 0.0) {
		scale = real_sum / est_sum;
		if (scale < 0.25)
			scale = 0.25;
		if (scale > 4.0)
			scale = 4.0;
	}

	for (int s = IL_STEP_EXTRACT + 1; s < IL_STEP_COUNT; s++) {
		double est = per_weight * g_weights[s] *
			     (s == IL_STEP_FINALIZE ? 1.0 : scale);

		if (est < IL_TRACK_STEP_MIN_S)
			est = IL_TRACK_STEP_MIN_S;
		if (s < t->step)
			continue;
		if (s == t->step) {
			double el = now_ms > t->step_t0[s] ?
				    (double)(now_ms - t->step_t0[s]) / 1000.0 : 0;
			double by_frac = est * (1.0 - t->frac);
			double by_clock = est - el;

			left += by_frac > by_clock ? by_frac : by_clock;
		} else {
			left += est;
		}
	}
	return left;
}

static void il_track_update_eta(struct il_track *t, uint64_t now_ms)
{
	double raw = il_track_remaining(t, now_ms);
	double dt, pred, tau;

	if (raw < 0.0) {
		t->eta = -1.0;
		t->eta_ms = now_ms;
		return;
	}
	if (t->eta < 0.0 || t->finished) {
		t->eta = raw;
		t->eta_ms = now_ms;
		return;
	}
	dt = now_ms > t->eta_ms ? (double)(now_ms - t->eta_ms) / 1000.0 : 0.0;
	t->eta_ms = now_ms;
	pred = t->eta - dt;
	if (pred < 0.0)
		pred = 0.0;
	tau = raw > pred ? IL_TRACK_ETA_UP_S : IL_TRACK_ETA_DOWN_S;
	t->eta = pred + (raw - pred) * (dt / (tau + dt));
	if (t->eta < raw * IL_TRACK_ETA_FLOOR)
		t->eta = raw * IL_TRACK_ETA_FLOOR;
}

/* ---- the report ---------------------------------------------------------- */

void il_track_finish(struct il_track *t, uint64_t now_ms)
{
	il_track_step(t, IL_STEP_COUNT - 1, now_ms);
	t->step_ms[IL_STEP_COUNT - 1] = now_ms > t->step_t0[IL_STEP_COUNT - 1] ?
		now_ms - t->step_t0[IL_STEP_COUNT - 1] : 1;
	t->frac = 1.0;
	t->overall = 1.0;
	t->finished = 1;
}

void il_track_report(struct il_track *t, uint64_t now_ms,
		     struct il_progress *p)
{
	double o = 0.0;
	int pct;

	if (t->step == IL_STEP_EXTRACT && !t->finished) {
		/* The model's share, never back; a jump forward (the fit
		 * learning that files are cheaper than assumed) is spread
		 * over a few seconds instead of shown at once. */
		double want = il_track_extract_share(t);

		if (want > 1.0)
			want = 1.0;
		if (want > t->frac + 0.005 && t->frac_ms) {
			double dt = now_ms > t->frac_ms ?
				    (double)(now_ms - t->frac_ms) / 1000.0 : 0.0;

			want = t->frac + (want - t->frac) *
			       (dt / (IL_TRACK_GLIDE_S + dt));
		}
		il_track_fraction(t, want);
		t->frac_ms = now_ms;
	}
	for (int i = 0; i < t->step && i < IL_STEP_COUNT; i++)
		o += g_weights[i];
	o += g_weights[t->step] * t->frac;
	if (o > 1.0)
		o = 1.0;
	if (t->finished)
		o = 1.0;
	if (o > t->overall)
		t->overall = o;
	il_track_update_eta(t, now_ms);

	pct = (int)(t->overall * 100.0);
	if (pct < 0)
		pct = 0;
	if (!t->finished && pct > 99)
		pct = 99;
	p->step = t->step;
	p->step_fraction = t->frac;
	p->overall = t->overall;
	p->percent = pct;
	p->bytes_done = t->bytes_done;
	p->bytes_total = t->bytes_total;
	p->files_done = t->files_done;
	p->files_total = t->files_total;
	p->eta_seconds = t->finished ? 0 : t->eta < 0.0 ? -1 :
			 (int)(t->eta + 0.5);
}
