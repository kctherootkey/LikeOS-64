/* libinstaller host test: whole installations in simulate mode.
 *
 *   test_install MODE IMAGE SOURCE TARGET LOG [ANSWERS]
 *
 * MODE
 *   install   erase IMAGE and install SOURCE (a .tar.zst with .sha256 and
 *             .info beside it); TARGET is the directory standing for the new
 *             root.  Prints RESULT=, ERROR= and the progress summary; the
 *             script checks the disk image and TARGET with the host's tools.
 *   answers   the same, driven by the answer file ANSWERS
 *   dryrun    validate only; nothing may be written
 *   cancel    cancel during the first step; nothing may be written
 *   fail      expect a failure (a damaged image); prints the error
 *
 * The progress reports are checked on the way: steps in order, the overall
 * fraction and the percentage never going back, the percentage the floor of
 * the fraction and 100 only in the last report, every detail line readable
 * text (a name read from freed memory is not), and no more than about ten
 * reports a second.  RATE= gives the reports per second of the unpacking
 * and the most in any one second.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "il_internal.h"

struct watch {
	il_ctx *ctx;
	int cancel_on_first;
	int cancel_sent;
	int last_step;
	double last_overall;
	int reports;
	int backwards;
	int eta_seen;
	double final_overall;
	int last_percent;
	int final_percent;
	int bad_percent;
	int bad_detail;
	/* the rate of the reports */
	double t_first_extract, t_last_extract;
	int extract_reports;
	double sec_start;
	int sec_count, sec_max;
};

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* UTF-8 text without control characters. */
static int readable(const char *s)
{
	const unsigned char *u = (const unsigned char *)s;

	while (*u) {
		int n = *u < 0x80 ? 0 : (*u & 0xe0) == 0xc0 ? 1 :
			(*u & 0xf0) == 0xe0 ? 2 : (*u & 0xf8) == 0xf0 ? 3 : -1;

		if (*u < 0x20 || *u == 0x7f || n < 0)
			return 0;
		u++;
		for (; n > 0; n--, u++)
			if ((*u & 0xc0) != 0x80)
				return 0;
	}
	return 1;
}

static void on_progress(const struct il_progress *p, void *user)
{
	struct watch *w = user;

	double t = now_s();

	w->reports++;
	if (p->step == IL_STEP_EXTRACT) {
		if (!w->extract_reports)
			w->t_first_extract = t;
		w->t_last_extract = t;
		w->extract_reports++;
		if (t - w->sec_start >= 1.0) {
			w->sec_start = t;
			w->sec_count = 0;
		}
		if (++w->sec_count > w->sec_max)
			w->sec_max = w->sec_count;
	}
	if (!p->detail || !readable(p->detail)) {
		if (w->bad_detail++ < 5)
			fprintf(stderr, "unreadable detail line in step %d\n",
				p->step);
	}
	if (p->percent < w->last_percent || p->percent > 100 ||
	    (p->percent != (int)(p->overall * 100.0) &&
	     !(p->percent == 99 && p->overall >= 0.99))) {
		if (w->bad_percent++ < 5)
			fprintf(stderr, "percent %d with overall %.4f (last %d)\n",
				p->percent, p->overall, w->last_percent);
	}
	if (w->final_percent == 100)
		w->bad_percent++;      /* nothing may follow the 100 */
	w->last_percent = p->percent;
	w->final_percent = p->percent;
	if (p->step < w->last_step || p->overall + 1e-9 < w->last_overall) {
		w->backwards++;
		fprintf(stderr, "progress went back: step %d->%d, %.4f->%.4f\n",
			w->last_step, p->step, w->last_overall, p->overall);
	}
	w->last_step = p->step;
	w->last_overall = p->overall;
	w->final_overall = p->overall;
	if (p->eta_seconds >= 0)
		w->eta_seen = 1;
	if (w->cancel_on_first && !w->cancel_sent) {
		if (!il_can_cancel(w->ctx))
			fprintf(stderr, "il_can_cancel() is 0 during prepare\n");
		il_cancel(w->ctx);
		w->cancel_sent = 1;
	}
}

static void fill_config(struct il_config *cfg)
{
	il_config_defaults(cfg);
	strcpy(cfg->hostname, "simbox");
	strcpy(cfg->timezone, "Europe/Berlin");
	strcpy(cfg->time_format, "en_GB.UTF-8");
	cfg->nlayouts = 2;
	strcpy(cfg->layouts[0], "de");
	strcpy(cfg->variants[0], "nodeadkeys");
	strcpy(cfg->layouts[1], "us");
	strcpy(cfg->root_password, "toor-sim");
	strcpy(cfg->user.fullname, "Ada Lovelace");
	strcpy(cfg->user.username, "ada");
	strcpy(cfg->user.password, "ada-sim");
}

int main(int argc, char **argv)
{
	struct il_options o;
	struct il_disk_list disks;
	struct il_config cfg;
	struct watch w;
	const char *mode, *image, *source, *target, *log;
	const char *sim[2];
	il_ctx *ctx;
	il_plan *plan = NULL;
	int rc;

	if (argc < 6) {
		fprintf(stderr, "usage: test_install MODE IMAGE SOURCE TARGET LOG "
			"[ANSWERS]\n");
		return 2;
	}
	mode = argv[1];
	image = argv[2];
	source = argv[3];
	target = argv[4];
	log = argv[5];
	sim[0] = image;
	sim[1] = NULL;
	memset(&o, 0, sizeof(o));
	o.source = source;
	o.target = target;
	o.log_path = log;
	o.simulate = 1;
	o.simulate_disks = sim;
	o.dry_run = !strcmp(mode, "dryrun");
	o.verbose = getenv("IL_TEST_VERBOSE") != NULL;
	ctx = il_ctx_new(&o);
	if (!ctx) {
		fprintf(stderr, "no context\n");
		return 1;
	}
	rc = il_disks_scan(ctx, &disks);
	if (rc || disks.n != 1) {
		fprintf(stderr, "scan: %d disks, %s\n", disks.n, il_last_error(ctx));
		return 1;
	}
	fill_config(&cfg);
	if (!strcmp(mode, "answers")) {
		struct il_answers a;

		if (argc < 7) {
			fprintf(stderr, "answers mode needs ANSWERS\n");
			return 2;
		}
		rc = il_answers_load(ctx, argv[6], &a);
		if (!rc)
			rc = il_answers_plan(ctx, &a, &disks, &plan);
		if (rc) {
			fprintf(stderr, "answers: %s\n", il_last_error(ctx));
			return 1;
		}
		cfg = a.cfg;
	} else {
		plan = il_plan_new(ctx, &disks.disks[0]);
		rc = il_plan_use_whole_disk(plan);
		if (rc) {
			fprintf(stderr, "plan: %s\n", il_last_error(ctx));
			return 1;
		}
	}
	memset(&w, 0, sizeof(w));
	w.ctx = ctx;
	w.cancel_on_first = !strcmp(mode, "cancel");
	rc = il_install(ctx, plan, &cfg, on_progress, &w);
	printf("RESULT=%d\n", rc);
	printf("ERROR=%s\n", rc ? il_last_error(ctx) : "");
	printf("REPORTS=%d BACKWARDS=%d FINAL=%.4f PERCENT=%d ETA_SEEN=%d "
	       "CAN_CANCEL_AFTER=%d BAD_PERCENT=%d BAD_DETAIL=%d\n",
	       w.reports, w.backwards, w.final_overall, w.final_percent,
	       w.eta_seen, il_can_cancel(ctx), w.bad_percent, w.bad_detail);
	{
		double xs = w.t_last_extract - w.t_first_extract;

		printf("RATE=%.1f/s over %.1f s of unpacking (%d reports), "
		       "at most %d in one second\n",
		       xs > 0.0 ? (w.extract_reports - 1) / xs : 0.0, xs,
		       w.extract_reports, w.sec_max);
	}
	if (w.bad_detail || w.bad_percent)
		rc = rc ? rc : -EIO;
	if (!strcmp(mode, "install") || !strcmp(mode, "answers")) {
		if (rc != 0 || w.backwards || w.final_overall < 0.9999 ||
		    w.final_percent != 100 || w.sec_max > 15)
			rc = rc ? rc : -EIO;
	} else if (!strcmp(mode, "dryrun")) {
		rc = rc == 0 ? 0 : rc;
	} else if (!strcmp(mode, "cancel")) {
		rc = rc == -ECANCELED ? 0 : (rc ? rc : -EIO);
	} else if (!strcmp(mode, "fail")) {
		rc = rc != 0 ? 0 : -EIO;
	}
	il_plan_free(plan);
	il_ctx_free(ctx);
	return rc ? 1 : 0;
}
