/* LikeOS installer -- running the backend on a worker thread
 * (see backend-bridge.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "backend-bridge.h"

#include <errno.h>
#include <string.h>

#define LOG_KEEP 400

typedef enum {
	JOB_PROBE,
	JOB_SCAN,
	JOB_VERIFY,
	JOB_INSTALL,
} JobKind;

typedef struct {
	Bridge *bridge;
	JobKind kind;
	BridgeDoneCb done;
	BridgeProgressCb progress;
	gpointer data;
	int rc;

	/* results, handed to App on the main thread */
	struct il_source_info src;
	struct il_disk_list *disks;
	char error[IL_ERROR_MAX];

	/* the newest progress, guarded by the bridge's lock */
	BridgeProgress latest;
	gboolean progress_pending;
} Job;

typedef struct {
	int level;
	char *line;
} DeferredLine;

struct Bridge {
	App *app;
	GMutex lock;
	Job *job;              /* the running job, NULL when idle */
	GThread *thread;
	GQueue *log;           /* char *, newest last */
	GQueue *deferred;      /* DeferredLine *, main thread only */
};

Bridge *bridge_new(App *app)
{
	Bridge *b = g_new0(Bridge, 1);

	b->app = app;
	g_mutex_init(&b->lock);
	b->log = g_queue_new();
	b->deferred = g_queue_new();
	return b;
}

static void deferred_free(gpointer data)
{
	DeferredLine *d = data;

	g_free(d->line);
	g_free(d);
}

void bridge_defer_log(Bridge *b, int level, const char *line)
{
	DeferredLine *d = g_new0(DeferredLine, 1);

	d->level = level;
	d->line = g_strdup(line);
	g_queue_push_tail(b->deferred, d);
}

static void flush_deferred(Bridge *b)
{
	DeferredLine *d;

	while ((d = g_queue_pop_head(b->deferred)) != NULL) {
		if (b->app->ctx)
			il_be->log(b->app->ctx, d->level, "%s", d->line);
		deferred_free(d);
	}
}

void bridge_shutdown(Bridge *b)
{
	if (!b)
		return;
	if (b->thread) {
		if (b->app->ctx)
			il_cancel(b->app->ctx);
		g_thread_join(b->thread);
		b->thread = NULL;
		b->job = NULL;
	}
	flush_deferred(b);
}

void bridge_free(Bridge *b)
{
	if (!b)
		return;
	g_queue_free_full(b->deferred, deferred_free);
	g_queue_free_full(b->log, g_free);
	g_mutex_clear(&b->lock);
	g_free(b);
}

gboolean bridge_busy(Bridge *b)
{
	return b->job != NULL;
}

static void log_hook(int level, const char *line, void *user)
{
	Bridge *b = user;

	(void)level;
	g_mutex_lock(&b->lock);
	g_queue_push_tail(b->log, g_strdup(line));
	while (g_queue_get_length(b->log) > LOG_KEEP)
		g_free(g_queue_pop_head(b->log));
	g_mutex_unlock(&b->lock);
}

il_ctx *bridge_create_context(Bridge *b)
{
	App *app = b->app;
	struct il_options opt;
	const char *src = g_getenv("LIKEOS_INSTALL_SOURCE");

	memset(&opt, 0, sizeof(opt));
	opt.source = app->source_path ? app->source_path :
		src && *src ? src : NULL;
	opt.target = app->target_path;
	opt.log_path = app->log_path;
	opt.verbose = app->verbose ||
		g_getenv("LIKEOS_INSTALLER_VERBOSE") != NULL;
	opt.simulate = app->simulate;
	opt.simulate_disks = (const char *const *)app->sim_disks;
	opt.log_cb = log_hook;
	opt.log_user = b;
	return il_ctx_new(&opt);
}

char *bridge_log_tail(Bridge *b, int max_lines)
{
	GString *s = g_string_new(NULL);
	GList *l;
	int n;

	g_mutex_lock(&b->lock);
	n = (int)g_queue_get_length(b->log);
	l = b->log->head;
	for (int i = 0; l && i < n - max_lines; i++)
		l = l->next;
	for (; l; l = l->next) {
		g_string_append(s, l->data);
		g_string_append_c(s, '\n');
	}
	g_mutex_unlock(&b->lock);
	return g_string_free(s, FALSE);
}

/* ---- delivering to the main loop ----------------------------------- */

static gboolean progress_idle(gpointer data)
{
	Job *job = data;
	Bridge *b = job->bridge;
	BridgeProgress p;

	g_mutex_lock(&b->lock);
	p = job->latest;
	job->progress_pending = FALSE;
	g_mutex_unlock(&b->lock);
	if (job->progress && b->job == job)
		job->progress(b->app, &p, job->data);
	return G_SOURCE_REMOVE;
}

static void post_progress(Job *job, const BridgeProgress *p)
{
	Bridge *b = job->bridge;
	gboolean schedule;

	g_mutex_lock(&b->lock);
	job->latest = *p;
	schedule = !job->progress_pending;
	job->progress_pending = TRUE;
	g_mutex_unlock(&b->lock);
	if (schedule)
		g_idle_add_full(G_PRIORITY_DEFAULT, progress_idle, job, NULL);
}

static gboolean done_idle(gpointer data)
{
	Job *job = data;
	Bridge *b = job->bridge;
	App *app = b->app;

	/* progress posted after the end must not outlive the job */
	g_mutex_lock(&b->lock);
	if (job->progress_pending) {
		g_mutex_unlock(&b->lock);
		return G_SOURCE_CONTINUE;
	}
	g_mutex_unlock(&b->lock);

	if (b->thread) {
		g_thread_join(b->thread);
		b->thread = NULL;
	}
	/* the context is the main thread's again */
	b->job = NULL;
	flush_deferred(b);
	switch (job->kind) {
	case JOB_PROBE:
		app->src = job->src;
		app->src_ok = job->rc == 0 && job->src.present;
		g_strlcpy(app->src_error, job->error, sizeof(app->src_error));
		break;
	case JOB_SCAN:
		plans_free_all(app);
		g_free(app->disks);
		app->disks = job->disks;
		job->disks = NULL;
		app->disks_ok = job->rc == 0;
		g_strlcpy(app->disks_error, job->error, sizeof(app->disks_error));
		/* the plans are gone, and an index of the old list may name
		 * another disk in the new one */
		app->target_disk = -1;
		break;
	case JOB_INSTALL:
		app->install_rc = job->rc;
		g_strlcpy(app->install_error, job->error,
			  sizeof(app->install_error));
		app->failed_step = job->rc ? job->latest.step : -1;
		app->installed = job->rc == 0;
		break;
	default:
		break;
	}
	if (job->done)
		job->done(app, job->rc, job->data);
	g_free(job->disks);
	g_free(job);
	return G_SOURCE_REMOVE;
}

/* ---- the worker ---------------------------------------------------- */

static void install_progress(const struct il_progress *ip, void *user)
{
	Job *job = user;
	BridgeProgress p;

	memset(&p, 0, sizeof(p));
	p.step = ip->step;
	p.step_fraction = ip->step_fraction;
	p.overall = ip->overall;
	p.percent = ip->percent;
	/* the detail is the backend's only during the callback */
	g_strlcpy(p.detail, ip->detail ? ip->detail : "", sizeof(p.detail));
	p.bytes_done = ip->bytes_done;
	p.bytes_total = ip->bytes_total;
	p.files_done = ip->files_done;
	p.files_total = ip->files_total;
	p.eta_seconds = ip->eta_seconds;
	p.can_cancel = il_can_cancel(job->bridge->app->ctx);
	post_progress(job, &p);
}

static void verify_progress(double fraction, void *user)
{
	Job *job = user;
	BridgeProgress p;

	memset(&p, 0, sizeof(p));
	p.overall = fraction;
	p.step_fraction = fraction;
	p.percent = (int)(CLAMP(fraction, 0.0, 1.0) * 100.0);
	p.eta_seconds = -1;
	post_progress(job, &p);
}

static gpointer worker(gpointer data)
{
	Job *job = data;
	App *app = job->bridge->app;
	il_ctx *ctx = app->ctx;

	switch (job->kind) {
	case JOB_PROBE:
		job->rc = il_source_probe(ctx, &job->src);
		break;
	case JOB_SCAN:
		job->disks = g_new0(struct il_disk_list, 1);
		job->rc = il_disks_scan(ctx, job->disks);
		break;
	case JOB_VERIFY:
		job->rc = il_source_verify(ctx, verify_progress, job);
		break;
	case JOB_INSTALL: {
		il_plan *plan = app_target_plan(app);

		if (!plan) {
			job->rc = -EINVAL;
			g_strlcpy(job->error, "No disk was chosen.",
				  sizeof(job->error));
			break;
		}
		job->rc = il_install(ctx, plan, &app->cfg, install_progress, job);
		break;
	}
	}
	if (job->rc && !job->error[0])
		g_strlcpy(job->error, il_last_error(ctx), sizeof(job->error));
	g_idle_add_full(G_PRIORITY_DEFAULT, done_idle, job, NULL);
	return NULL;
}

static gboolean start(Bridge *b, JobKind kind, BridgeProgressCb progress,
		      BridgeDoneCb done, gpointer data)
{
	Job *job;

	if (b->job || !b->app->ctx)
		return FALSE;
	job = g_new0(Job, 1);
	job->bridge = b;
	job->kind = kind;
	job->progress = progress;
	job->done = done;
	job->data = data;
	job->latest.step = 0;
	job->latest.eta_seconds = -1;
	job->latest.can_cancel = kind == JOB_INSTALL;
	b->job = job;
	b->thread = g_thread_new("installer-worker", worker, job);
	return TRUE;
}

gboolean bridge_probe_source(Bridge *b, BridgeDoneCb done, gpointer data)
{
	return start(b, JOB_PROBE, NULL, done, data);
}

gboolean bridge_scan_disks(Bridge *b, BridgeDoneCb done, gpointer data)
{
	return start(b, JOB_SCAN, NULL, done, data);
}

gboolean bridge_verify_source(Bridge *b, BridgeProgressCb progress,
			      BridgeDoneCb done, gpointer data)
{
	return start(b, JOB_VERIFY, progress, done, data);
}

gboolean bridge_install(Bridge *b, BridgeProgressCb progress,
			BridgeDoneCb done, gpointer data)
{
	return start(b, JOB_INSTALL, progress, done, data);
}

void bridge_cancel(Bridge *b)
{
	if (b->job && b->app->ctx)
		il_cancel(b->app->ctx);
}

/* From the last progress report: the context belongs to the worker while
 * a job runs, and only il_cancel() may be called from here. */
gboolean bridge_can_cancel(Bridge *b)
{
	gboolean can;

	if (!b->job || b->job->kind != JOB_INSTALL)
		return FALSE;
	g_mutex_lock(&b->lock);
	can = b->job->latest.can_cancel;
	g_mutex_unlock(&b->lock);
	return can;
}
