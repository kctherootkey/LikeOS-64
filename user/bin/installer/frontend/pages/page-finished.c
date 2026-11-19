/* LikeOS installer -- page 12: done.
 *
 * LikeOS is on the disk (il_install() has unmounted it again).  The
 * medium should come out, then the computer restarts -- by itself after
 * fifteen seconds, or at once with Restart now; Shut down switches it off
 * instead, and "Stop the countdown" waits for a decision.  The program
 * restarts the computer itself (il_reboot) where it may; otherwise it ends
 * with the exit status that asks the installer session to do it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#define COUNTDOWN_S 15

typedef struct {
	GtkWidget *where;
	GtkWidget *signin;
	GtkWidget *countdown;
	GtkWidget *bar;
	GtkWidget *stop;
	guint timer;
	int left;
} Finished;

static void stop_timer(Finished *f)
{
	if (f->timer) {
		g_source_remove(f->timer);
		f->timer = 0;
	}
}

static void show_left(Page *p)
{
	Finished *f = p->priv;
	char *t;

	if (f->timer) {
		t = g_strdup_printf(f->left == 1 ? "Restarting in %d second" :
				    "Restarting in %d seconds", f->left);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar),
					      (double)f->left / COUNTDOWN_S);
		gtk_widget_show(f->bar);
	} else {
		t = g_strdup("The countdown is stopped. Restart or shut down when "
			     "you are ready.");
		gtk_widget_hide(f->bar);
	}
	gtk_widget_set_visible(f->stop, f->timer != 0);
	gtk_label_set_text(GTK_LABEL(f->countdown), t);
	g_free(t);
}

static gboolean tick(gpointer data)
{
	Page *p = data;
	Finished *f = p->priv;

	if (--f->left <= 0) {
		f->timer = 0;
		app_log(p->app, IL_LOG_INFO, "ui: the countdown ran out");
		app_power_action(p->app, TRUE);
		return G_SOURCE_REMOVE;
	}
	show_left(p);
	return G_SOURCE_CONTINUE;
}

static void on_stop(GtkButton *b, gpointer data)
{
	Page *p = data;
	Finished *f = p->priv;

	(void)b;
	stop_timer(f);
	app_log(p->app, IL_LOG_INFO, "ui: the countdown was stopped");
	show_left(p);
}

static GtkWidget *finished_build(Page *p)
{
	Finished *f = p->priv;
	GtkWidget *body, *root, *steps;

	root = card_page_new("LikeOS is installed",
			     "Everything is in place. One restart, and your new "
			     "system starts.", &body);
	f->where = ui_label("", "il-text");
	gtk_box_pack_start(GTK_BOX(body), f->where, FALSE, FALSE, 0);

	steps = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	ui_add_class(steps, "il-next-steps");
	gtk_box_pack_start(GTK_BOX(steps),
			   ui_check_row("<b>Remove the installation medium</b> (the "
					"USB stick setup was started from). The "
					"computer then starts LikeOS from its "
					"disk.", ICON_USB), FALSE, FALSE, 0);
	f->signin = ui_check_row("", ICON_USER);
	gtk_box_pack_start(GTK_BOX(steps), f->signin, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(steps),
			   ui_check_row("A record of the installation is kept in "
					"<tt>" IL_TARGET_LOG "</tt> on the new "
					"system.", ICON_INFO), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), steps, FALSE, FALSE, 0);

	f->countdown = ui_label("", "il-countdown");
	gtk_box_pack_start(GTK_BOX(body), f->countdown, FALSE, FALSE, 0);
	f->bar = gtk_progress_bar_new();
	ui_add_class(f->bar, "il-progress");
	gtk_widget_set_no_show_all(f->bar, TRUE);
	gtk_box_pack_start(GTK_BOX(body), f->bar, FALSE, FALSE, 0);
	f->stop = link_button_new("S_top the countdown", ICON_NONE);
	gtk_widget_set_halign(f->stop, GTK_ALIGN_START);
	/* shown and hidden with the countdown (show_left) */
	g_signal_connect(f->stop, "clicked", G_CALLBACK(on_stop), p);
	gtk_box_pack_start(GTK_BOX(body), f->stop, FALSE, FALSE, 0);
	return root;
}

static void finished_enter(Page *p, int direction)
{
	Finished *f = p->priv;
	App *app = p->app;
	const struct il_disk *d = app_target_disk(app);
	GtkWidget *label;
	char *t, *u;

	(void)direction;
	if (d)
		t = g_strdup_printf("LikeOS %s now lives on Disk %d (%s).",
				    app->src_ok ? app->src.version : "",
				    app->target_disk, d->model[0] ? d->model : d->dev);
	else
		t = g_strdup_printf("LikeOS %s is now installed on this computer.",
				    app->src_ok ? app->src.version : "");
	gtk_label_set_text(GTK_LABEL(f->where), t);
	g_free(t);

	u = pages_escape(app->cfg.user.username[0] ? app->cfg.user.username : "root");
	t = g_strdup_printf("At the <b>%s login:</b> prompt, sign in as <b>%s</b> "
			    "with the password you chose, then type <tt>startx"
			    "</tt> to start the desktop.", app->cfg.hostname, u);
	{
		GList *kids = gtk_container_get_children(GTK_CONTAINER(f->signin));

		label = g_list_last(kids)->data;
		g_list_free(kids);
	}
	gtk_label_set_markup(GTK_LABEL(label), t);
	g_free(t);
	g_free(u);

	wizard_clear_history(pages_wizard());
	stop_timer(f);
	f->left = COUNTDOWN_S;
	f->timer = g_timeout_add_seconds(1, tick, p);
	show_left(p);
}

static void finished_leave(Page *p)
{
	stop_timer(p->priv);
}

static gboolean finished_next(Page *p)
{
	stop_timer(p->priv);
	app_power_action(p->app, TRUE);
	return FALSE;
}

static void finished_secondary(Page *p)
{
	stop_timer(p->priv);
	app_power_action(p->app, FALSE);
}

static int finished_autofill(Page *p, const struct il_answers *a)
{
	if (a->reboot)
		return AUTOFILL_NEXT;
	stop_timer(p->priv);
	app_exit_setup(p->app);
	return AUTOFILL_DONE;
}

void page_finished_init(Page *p)
{
	p->id = "finished";
	p->scene = SCENE_CARD;
	p->build = finished_build;
	p->enter = finished_enter;
	p->leave = finished_leave;
	p->go_next = finished_next;
	p->secondary = finished_secondary;
	p->autofill = finished_autofill;
	p->next_label = "_Restart now";
	p->secondary_label = "Shut _down";
	p->back_ok = FALSE;
	p->art = ART_SUCCESS;
	p->priv = g_new0(Finished, 1);
}
