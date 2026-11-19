/* LikeOS installer -- running the backend without freezing the window.
 *
 * Everything that can take more than a moment (scanning the disks, probing
 * and checking the system image, the installation itself) runs on a
 * worker thread.  Progress comes back to the main loop through g_idle_add,
 * coalesced so a fast backend cannot flood the window with redraws, and
 * the result is delivered with the job's done callback.  One job runs at a
 * time, which is also what the backend's context requires.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_BRIDGE_H
#define LIKEOS_INSTALLER_BRIDGE_H

#include "app.h"

/* A copy of one progress report: every string is copied out of the
 * backend's callback on the worker thread, so nothing here points into
 * the backend's buffers when the main loop reads it. */
typedef struct {
	int step;
	double step_fraction;
	double overall;
	int percent;             /* the one number shown (il_progress) */
	char detail[512];
	guint64 bytes_done, bytes_total;
	guint64 files_done, files_total;
	int eta_seconds;
	gboolean can_cancel;
} BridgeProgress;

typedef void (*BridgeDoneCb)(App *app, int rc, gpointer data);
typedef void (*BridgeProgressCb)(App *app, const BridgeProgress *p,
				 gpointer data);

Bridge *bridge_new(App *app);
/* Stops a running job (il_cancel, then waits for it) and writes the held
 * back log lines.  The context is freed after this and before
 * bridge_free(): its log still reaches the bridge's hook until then. */
void bridge_shutdown(Bridge *b);
void bridge_free(Bridge *b);
gboolean bridge_busy(Bridge *b);

/* il_ctx_new with the program's options (image, target, log, image files
 * to install on) and the bridge's log hook; called once at start. */
il_ctx *bridge_create_context(Bridge *b);

/* Probes the system image into app->src. */
gboolean bridge_probe_source(Bridge *b, BridgeDoneCb done, gpointer data);
/* Scans the disks into app->disks; the plans of the old scan are freed. */
gboolean bridge_scan_disks(Bridge *b, BridgeDoneCb done, gpointer data);
/* Reads the whole image and checks its checksum. */
gboolean bridge_verify_source(Bridge *b, BridgeProgressCb progress,
			      BridgeDoneCb done, gpointer data);
/* Installs with app->cfg on the target plan. */
gboolean bridge_install(Bridge *b, BridgeProgressCb progress,
			BridgeDoneCb done, gpointer data);
void bridge_cancel(Bridge *b);
gboolean bridge_can_cancel(Bridge *b);

/* The last lines of the backend's log, oldest first (for the error page);
 * free with g_free. */
char *bridge_log_tail(Bridge *b, int max_lines);

/* A line for the backend's log from the main thread while a job runs: it
 * is written (il_log) on the main thread as soon as the job has ended. */
void bridge_defer_log(Bridge *b, int level, const char *line);

#endif
