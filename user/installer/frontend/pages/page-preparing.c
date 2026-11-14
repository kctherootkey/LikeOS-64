/* LikeOS installer -- page 3: getting things ready.
 *
 * Looks at the installation medium (the system image and its size) and
 * at the disks of the computer, then moves on to the disk page by itself.
 * When the image is missing there is nothing to install: the page says so
 * and offers to look again.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#define PREPARE_MIN_MS 1400

typedef struct {
	GtkWidget *spinner;
	GtkWidget *status;
	GtkWidget *detail;
	GtkWidget *notice;
	GtkWidget *retry;
	GtkWidget *steps;
	gboolean scan_done;
	gboolean timer_done;
	gboolean failed;
	gboolean active;
	guint timer;
	guint wait;
} Preparing;

static void preparing_scan(Page *p);

static void preparing_finish(Page *p)
{
	Preparing *s = p->priv;
	App *app = p->app;

	if (!s->active || !s->scan_done || !s->timer_done)
		return;
	ring_spinner_set_active(s->spinner, FALSE);
	if (!app->src_ok) {
		char *w = app_sentence(app->src_error[0] ? app->src_error :
				       app->src.path[0] ? app->src.path :
				       IL_DEFAULT_SOURCE);
		char *m = pages_escape(w);
		char *t = g_strdup_printf("<b>The LikeOS system image was not "
					  "found on this medium.</b>\n%s\nMake "
					  "sure you started the computer from a "
					  "complete LikeOS installation medium.",
					  m);

		app_log(app, IL_LOG_INFO, "ui: setup cannot continue: no system "
			"image (%s)", w);
		g_free(w);

		notice_set(s->notice, NOTICE_ERROR, t);
		g_free(t);
		g_free(m);
		gtk_label_set_text(GTK_LABEL(s->status), "Setup cannot continue");
		gtk_widget_show(s->retry);
		step_list_set_state(s->steps, 0, STEP_FAILED);
		step_list_set_state(s->steps, 2, STEP_PENDING);
		s->failed = TRUE;
		return;
	}
	if (!app->disks_ok) {
		char *w = app_sentence(app->disks_error);
		char *m = pages_escape(w);
		char *t = g_strdup_printf("<b>The disks could not be read.</b>\n%s", m);

		app_log(app, IL_LOG_INFO, "ui: setup cannot continue: %s", w);
		g_free(w);

		notice_set(s->notice, NOTICE_ERROR, t);
		g_free(t);
		g_free(m);
		gtk_label_set_text(GTK_LABEL(s->status), "Setup cannot continue");
		gtk_widget_show(s->retry);
		step_list_set_state(s->steps, 1, STEP_FAILED);
		step_list_set_state(s->steps, 2, STEP_PENDING);
		s->failed = TRUE;
		return;
	}
	s->active = FALSE;
	wizard_go(pages_wizard(), PAGE_DISK, +1);
}

static gboolean preparing_timer(gpointer data)
{
	Page *p = data;
	Preparing *s = p->priv;

	s->timer = 0;
	s->timer_done = TRUE;
	preparing_finish(p);
	return G_SOURCE_REMOVE;
}

static void scan_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	Preparing *s = p->priv;

	(void)app;
	s->scan_done = TRUE;
	step_list_set_state(s->steps, 1, rc == 0 ? STEP_DONE : STEP_FAILED);
	if (rc == 0) {
		int n = 0;

		for (int i = 0; app->disks && i < app->disks->n; i++)
			if (!app->disks->disks[i].is_boot_medium)
				n++;
		{
			char *t = g_strdup_printf(n == 1 ? "Found %d disk" :
						  "Found %d disks", n);

			gtk_label_set_text(GTK_LABEL(s->detail), t);
			g_free(t);
		}
	}
	step_list_set_state(s->steps, 2, STEP_RUNNING);
	preparing_finish(p);
}

static void probe_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	Preparing *s = p->priv;

	step_list_set_state(s->steps, 0, rc == 0 && app->src_ok ? STEP_DONE :
			    STEP_FAILED);
	if (rc == 0 && app->src_ok) {
		char *size = app_size_text(app->src.uncompressed_bytes);
		char *t = g_strdup_printf("LikeOS %s \xc2\xb7 %s", app->src.version, size);

		app_log(app, IL_LOG_INFO, "ui: system image %s: LikeOS %s, %s, "
			"needs %" G_GUINT64_FORMAT " bytes", app->src.path,
			app->src.version, size, app->src.required_bytes);

		gtk_label_set_text(GTK_LABEL(s->detail), t);
		g_free(t);
		g_free(size);
	}
	step_list_set_state(s->steps, 1, STEP_RUNNING);
	gtk_label_set_text(GTK_LABEL(s->status), "Looking for disks\xe2\x80\xa6");
	if (!bridge_scan_disks(app->bridge, scan_done, p))
		scan_done(app, -1, p);
}

static gboolean preparing_wait(gpointer data)
{
	Page *p = data;
	Preparing *s = p->priv;

	s->wait = 0;
	if (s->active)
		preparing_scan(p);
	return G_SOURCE_REMOVE;
}

static void preparing_scan(Page *p)
{
	Preparing *s = p->priv;

	/* a scan from an earlier visit may still run: wait for its end */
	if (bridge_busy(p->app->bridge)) {
		s->active = TRUE;
		if (!s->wait)
			s->wait = g_timeout_add(150, preparing_wait, p);
		return;
	}
	s->scan_done = FALSE;
	s->timer_done = FALSE;
	s->failed = FALSE;
	s->active = TRUE;
	notice_hide(s->notice);
	gtk_widget_hide(s->retry);
	step_list_reset(s->steps);
	step_list_set_state(s->steps, 0, STEP_RUNNING);
	ring_spinner_set_active(s->spinner, TRUE);
	gtk_label_set_text(GTK_LABEL(s->status),
			   "Checking the installation medium\xe2\x80\xa6");
	gtk_label_set_text(GTK_LABEL(s->detail), "");
	if (s->timer)
		g_source_remove(s->timer);
	s->timer = g_timeout_add(p->app->no_splash_delay ? 100 : PREPARE_MIN_MS,
				 preparing_timer, p);
	if (!bridge_probe_source(p->app->bridge, probe_done, p))
		probe_done(p->app, p->app->src_ok ? 0 : -1, p);
}

static void on_retry(GtkButton *b, gpointer data)
{
	Page *p = data;

	(void)b;
	app_log(p->app, IL_LOG_INFO, "ui: looking at the medium again");
	preparing_scan(p);
}

static GtkWidget *preparing_build(Page *p)
{
	Preparing *s = p->priv;
	GtkWidget *body, *root, *row, *texts;

	root = setup_page_new("Getting things ready", &body);
	gtk_box_pack_start(GTK_BOX(body),
			   ui_label("Setup is looking at this computer. Nothing is "
				    "changed yet.", "il-text"), FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	ui_add_class(row, "il-busy-row");
	s->spinner = ring_spinner_new(32);
	ui_add_class(s->spinner, "il-accent-spinner");
	gtk_box_pack_start(GTK_BOX(row), s->spinner, FALSE, FALSE, 0);
	texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	s->status = ui_label("", "il-status");
	s->detail = ui_label("", "il-caption");
	gtk_box_pack_start(GTK_BOX(texts), s->status, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(texts), s->detail, FALSE, FALSE, 0);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), texts, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);

	s->steps = step_list_new();
	ui_add_class(s->steps, "il-steps-light");
	step_list_add(s->steps, "Checking the installation medium");
	step_list_add(s->steps, "Looking for disks");
	step_list_add(s->steps, "Preparing the disk choices");
	gtk_box_pack_start(GTK_BOX(body), s->steps, FALSE, FALSE, 0);

	s->notice = notice_new();
	gtk_box_pack_start(GTK_BOX(body), s->notice, FALSE, FALSE, 0);
	s->retry = ui_button("_Try again", NULL);
	gtk_widget_set_halign(s->retry, GTK_ALIGN_START);
	gtk_widget_set_no_show_all(s->retry, TRUE);
	g_signal_connect(s->retry, "clicked", G_CALLBACK(on_retry), p);
	gtk_box_pack_start(GTK_BOX(body), s->retry, FALSE, FALSE, 0);
	return root;
}

static void preparing_enter(Page *p, int direction)
{
	(void)direction;
	preparing_scan(p);
}

static void preparing_leave(Page *p)
{
	Preparing *s = p->priv;

	s->active = FALSE;
	if (s->timer) {
		g_source_remove(s->timer);
		s->timer = 0;
	}
	if (s->wait) {
		g_source_remove(s->wait);
		s->wait = 0;
	}
}

void page_preparing_init(Page *p)
{
	p->id = "preparing";
	p->scene = SCENE_SETUP;
	p->transient = TRUE;
	p->build = preparing_build;
	p->enter = preparing_enter;
	p->leave = preparing_leave;
	p->next_visible = FALSE;
	p->priv = g_new0(Preparing, 1);
}
