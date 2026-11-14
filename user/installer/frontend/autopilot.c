/* LikeOS installer -- the autopilot (see autopilot.h).
 *
 * A timer looks at the wizard a moment after every change: an open
 * question is answered with its main button; a waiting page (the start
 * screen, the look at the medium, the copy) is waited for; any other page
 * gets its answers through Page.autofill, and a moment later -- after a
 * picture, when pictures are wanted -- Next.  A page that stays where it is
 * three times running has refused the answers, and the run fails.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "autopilot.h"
#include "backend-bridge.h"
#include "sheet.h"
#include "wizard.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define AP_TRIES        3      /* attempts on one page before giving up */
#define AP_WAIT_LIMIT_S 180    /* a waiting page, the copy step excepted */
#define AP_POLL_MS      300

struct Autopilot {
	App *app;
	struct il_answers a;
	char *path;
	int delay_ms;
	char *shots;           /* directory for pictures, or NULL */
	int shot_seq;
	guint timer;
	int page;              /* the page and step last seen */
	int substep;
	int tries;
	gint64 since;          /* when they were first seen (monotonic, us) */
	gboolean pending_next; /* filled in: Next on the following tick */
	int retries;           /* Try again presses left on the error page */
	gboolean finished;
};

static gboolean tick(gpointer data);

static void schedule(Autopilot *ap, int ms)
{
	if (ap->finished)
		return;
	if (ap->timer)
		g_source_remove(ap->timer);
	ap->timer = g_timeout_add(ms, tick, ap);
}

/* After a failure during a job: the end of the job, then the end. */
static gboolean quit_when_idle(gpointer data)
{
	Autopilot *ap = data;

	if (bridge_busy(ap->app->bridge))
		return G_SOURCE_CONTINUE;
	ap->timer = 0;
	app_quit(ap->app);
	return G_SOURCE_REMOVE;
}

static void fail(Autopilot *ap, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void fail(Autopilot *ap, const char *fmt, ...)
{
	App *app = ap->app;
	va_list va;
	char *msg;

	va_start(va, fmt);
	msg = g_strdup_vprintf(fmt, va);
	va_end(va);
	g_printerr("likeos-installer: autopilot: %s\n", msg);
	app_log(app, IL_LOG_WARN, "ui: autopilot failed: %s", msg);
	g_free(msg);
	ap->finished = TRUE;
	app->exit_code = APP_EXIT_AUTOPILOT;
	/* never in the middle of a job: the program waits for it */
	if (ap->timer) {
		g_source_remove(ap->timer);
		ap->timer = 0;
	}
	if (!bridge_busy(app->bridge))
		app_quit(app);
	else
		ap->timer = g_timeout_add(AP_POLL_MS, quit_when_idle, ap);
}

/* A picture of the window, named after the page and its step. */
static void shot(Autopilot *ap, const char *id, int substep)
{
	GdkWindow *win;
	GdkPixbuf *pb;
	GError *err = NULL;
	char *name;

	if (!ap->shots || !(win = gtk_widget_get_window(ap->app->window)))
		return;
	pb = gdk_pixbuf_get_from_window(win, 0, 0, gdk_window_get_width(win),
					gdk_window_get_height(win));
	if (!pb)
		return;
	if (substep > 0)
		name = g_strdup_printf("%s/%02d-%s-%d.png", ap->shots,
				       ++ap->shot_seq, id, substep);
	else
		name = g_strdup_printf("%s/%02d-%s.png", ap->shots, ++ap->shot_seq,
				       id);
	if (!gdk_pixbuf_save(pb, name, "png", &err, NULL)) {
		g_printerr("likeos-installer: autopilot: %s: %s\n", name,
			   err ? err->message : "not saved");
		g_clear_error(&err);
	}
	g_free(name);
	g_object_unref(pb);
}

static gboolean tick(gpointer data)
{
	Autopilot *ap = data;
	App *app = ap->app;
	Page *p = wizard_current(app->wiz);
	gint64 now = g_get_monotonic_time();
	int res;

	ap->timer = 0;
	if (ap->finished || !p)
		return G_SOURCE_REMOVE;

	/* a question ("Erase and install?"): its main button */
	if (sheet_is_open(app)) {
		shot(ap, "question", 0);
		app_log(app, IL_LOG_INFO, "ui: autopilot answers the question "
			"with its main button");
		sheet_respond(app, SHEET_OK);
		schedule(ap, ap->delay_ms);
		return G_SOURCE_REMOVE;
	}

	if (p->index != ap->page || p->substep != ap->substep) {
		ap->page = p->index;
		ap->substep = p->substep;
		ap->tries = 0;
		ap->since = now;
		ap->pending_next = FALSE;
	}
	if (ap->pending_next) {
		ap->pending_next = FALSE;
		shot(ap, p->id, p->substep);
		wizard_next(app->wiz);
		schedule(ap, ap->delay_ms);
		return G_SOURCE_REMOVE;
	}

	switch (p->index) {
	case PAGE_SPLASH:
	case PAGE_PREPARING:
	case PAGE_INSTALLING:
		/* these move on by themselves */
		if (p->index != PAGE_INSTALLING &&
		    now - ap->since > (gint64)AP_WAIT_LIMIT_S * G_USEC_PER_SEC) {
			shot(ap, p->id, 0);
			fail(ap, "page \"%s\" did not move on in %d s", p->id,
			     AP_WAIT_LIMIT_S);
			return G_SOURCE_REMOVE;
		}
		/* a picture once it shows something; the copy once more
		 * when it is well under way */
		ap->tries++;
		if (ap->tries == 3)
			shot(ap, p->id, 0);
		else if (p->index == PAGE_INSTALLING && ap->tries == 40)
			shot(ap, p->id, 1);
		schedule(ap, AP_POLL_MS);
		return G_SOURCE_REMOVE;
	case PAGE_ERROR:
		if (ap->retries > 0) {
			/* the error page's own way on (a test of it) */
			if (!p->next_ok) {
				schedule(ap, AP_POLL_MS);
				return G_SOURCE_REMOVE;
			}
			ap->retries--;
			shot(ap, p->id, 0);
			app_log(app, IL_LOG_INFO, "ui: autopilot tries again");
			wizard_next(app->wiz);
			schedule(ap, ap->delay_ms);
			return G_SOURCE_REMOVE;
		}
		shot(ap, p->id, 0);
		fail(ap, "the installation failed in step %d: %s",
		     app->failed_step + 1, app->install_error);
		return G_SOURCE_REMOVE;
	default:
		break;
	}

	if (ap->tries++ >= AP_TRIES) {
		shot(ap, p->id, p->substep);
		fail(ap, "page \"%s\" (step %d) did not move on", p->id,
		     p->substep);
		return G_SOURCE_REMOVE;
	}
	res = p->autofill ? p->autofill(p, &ap->a) : AUTOFILL_NEXT;
	switch (res) {
	case AUTOFILL_NEXT:
		ap->pending_next = TRUE;
		break;
	case AUTOFILL_WAIT:
		ap->tries--;
		if (now - ap->since > (gint64)AP_WAIT_LIMIT_S * G_USEC_PER_SEC) {
			fail(ap, "page \"%s\" was not ready in %d s", p->id,
			     AP_WAIT_LIMIT_S);
			return G_SOURCE_REMOVE;
		}
		schedule(ap, AP_POLL_MS);
		return G_SOURCE_REMOVE;
	case AUTOFILL_FAIL:
		shot(ap, p->id, p->substep);
		fail(ap, "page \"%s\" cannot take the answers of %s", p->id,
		     ap->path);
		return G_SOURCE_REMOVE;
	default:
		break;
	}
	schedule(ap, ap->delay_ms);
	return G_SOURCE_REMOVE;
}

gboolean autopilot_start(App *app, const char *answers_path)
{
	Autopilot *ap = g_new0(Autopilot, 1);
	const char *e;
	char why[IL_ERROR_MAX];

	ap->app = app;
	ap->path = g_strdup(answers_path);
	ap->page = -1;
	if (il_answers_load(app->ctx, answers_path, &ap->a) != 0) {
		g_printerr("likeos-installer: autopilot: %s\n",
			   il_last_error(app->ctx));
		explicit_bzero(&ap->a, sizeof(ap->a));
		g_free(ap->path);
		g_free(ap);
		return FALSE;
	}
	if (il_config_validate(&ap->a.cfg, why, sizeof(why)) != 0)
		/* the pages will refuse it; say why up front */
		g_printerr("likeos-installer: autopilot: %s: %s\n", answers_path,
			   why);
	e = g_getenv("LIKEOS_INSTALLER_AUTOPILOT_DELAY");
	ap->delay_ms = e && *e ? atoi(e) : 400;
	if (ap->delay_ms < 50)
		ap->delay_ms = 50;
	e = g_getenv("LIKEOS_INSTALLER_AUTOPILOT_RETRIES");
	ap->retries = e && *e ? atoi(e) : 0;
	e = g_getenv("LIKEOS_INSTALLER_AUTOPILOT_SHOTS");
	if (e && *e) {
		ap->shots = g_strdup(e);
		g_mkdir_with_parents(ap->shots, 0755);
	}
	app->autopilot = ap;
	app_log(app, IL_LOG_INFO, "ui: autopilot from %s", answers_path);
	return TRUE;
}

void autopilot_free(App *app)
{
	Autopilot *ap = app->autopilot;

	if (!ap)
		return;
	if (ap->timer)
		g_source_remove(ap->timer);
	/* the answers may hold passwords (a plain memset before the free
	 * may be dropped by the compiler) */
	explicit_bzero(&ap->a, sizeof(ap->a));
	g_free(ap->path);
	g_free(ap->shots);
	g_free(ap);
	app->autopilot = NULL;
}

void autopilot_page_entered(App *app, int page)
{
	Autopilot *ap = app->autopilot;

	(void)page;
	if (!ap || ap->finished)
		return;
	/* a moment for the page to appear and settle (a change of scene
	 * cross-fades for 380 ms) */
	schedule(ap, MAX(ap->delay_ms, 600));
}
