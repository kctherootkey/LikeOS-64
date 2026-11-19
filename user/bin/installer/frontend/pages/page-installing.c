/* LikeOS installer -- page 11: installing.
 *
 * The whole screen in the accent colour, a big title, how far it is, the
 * list of steps with their marks, what is being done right now and how
 * long it will still take.  Every number comes from the backend's report as
 * it is: the one percentage (il_progress.percent) in the line and in the
 * ring, the time left already smoothed by the backend's model, and the
 * files and bytes as two separate counts (the bytes run ahead: the big
 * files come first).  Cancel is offered only while the backend can
 * still stop without having touched the disk (il_can_cancel); from the
 * partition table on, the installation runs to its end.  The computer
 * does not restart in between.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <errno.h>
#include <string.h>

typedef struct {
	GtkWidget *title;
	GtkWidget *percent;
	GtkWidget *spinner;
	GtkWidget *steps;
	GtkWidget *step_label;
	GtkWidget *detail;
	GtkWidget *stats;
	GtkWidget *eta;
	GtkWidget *ring;
	GtkWidget *ring_col;
	GtkWidget *bar;
	GtkWidget *cancel;
	gboolean cancelling;
	gboolean running;
	Sheet *cancel_sheet;     /* the open "Stop the installation?" */
	int last_step;
	guint finish_timer;
} Installing;

/* 148213 -> "148,213" */
static char *group_digits(guint64 v)
{
	char raw[32];
	GString *out = g_string_new(NULL);
	int len;

	g_snprintf(raw, sizeof(raw), "%" G_GUINT64_FORMAT, v);
	len = (int)strlen(raw);
	for (int i = 0; i < len; i++) {
		if (i && (len - i) % 3 == 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, raw[i]);
	}
	return g_string_free(out, FALSE);
}

/* Nothing while the backend does not know yet (the first seconds of
 * copying the files). */
static char *eta_text(int seconds)
{
	if (seconds < 0)
		return g_strdup("");
	if (seconds < 60)
		return g_strdup("Less than a minute left");
	if (seconds < 90)
		return g_strdup("About a minute left");
	if (seconds < 90 * 60)
		return g_strdup_printf("About %d minutes left", (seconds + 30) / 60);
	return g_strdup_printf("About %d hours left", (seconds + 1800) / 3600);
}

/* "5,396 of 16,624 files · 1.3 GB copied" */
static char *stats_text(const BridgeProgress *bp)
{
	char *fd = group_digits(bp->files_done);
	char *size = app_size_text(bp->bytes_done);
	char *s;

	if (bp->files_total) {
		char *ft = group_digits(bp->files_total);

		s = g_strdup_printf("%s of %s files \xc2\xb7 %s copied", fd, ft,
				    size);
		g_free(ft);
	} else {
		s = g_strdup_printf("%s files \xc2\xb7 %s copied", fd, size);
	}
	g_free(fd);
	g_free(size);
	return s;
}

static void on_progress(App *app, const BridgeProgress *bp, gpointer data)
{
	Page *p = data;
	Installing *in = p->priv;
	char t[64], *s;
	int pct = CLAMP(bp->percent, 0, 100);

	(void)app;
	if (!in->running)
		return;
	/* one number, the same in the line and in the ring */
	g_snprintf(t, sizeof(t), "%d%% complete", pct);
	gtk_label_set_text(GTK_LABEL(in->percent), t);
	progress_ring_set_progress(in->ring, bp->overall, pct);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(in->bar), bp->overall);

	if (bp->step != in->last_step) {
		for (int i = 0; i < IL_STEP_COUNT; i++)
			step_list_set_state(in->steps, i, i < bp->step ? STEP_DONE :
					    i == bp->step ? STEP_RUNNING :
					    STEP_PENDING);
		s = g_strdup_printf("Step %d of %d: %s", bp->step + 1, IL_STEP_COUNT,
				    il_step_title(bp->step));
		gtk_label_set_text(GTK_LABEL(in->step_label), s);
		g_free(s);
		in->last_step = bp->step;
	}
	gtk_label_set_text(GTK_LABEL(in->detail), bp->detail);

	if (bp->step == IL_STEP_EXTRACT && (bp->files_total || bp->bytes_total)) {
		s = stats_text(bp);
		gtk_label_set_text(GTK_LABEL(in->stats), s);
		g_free(s);
	} else if (bp->step > IL_STEP_EXTRACT) {
		gtk_label_set_text(GTK_LABEL(in->stats), "All files are copied.");
	} else {
		gtk_label_set_text(GTK_LABEL(in->stats), "");
	}
	s = eta_text(bp->eta_seconds);
	gtk_label_set_text(GTK_LABEL(in->eta), s);
	g_free(s);

	gtk_widget_set_visible(in->cancel, bp->can_cancel || in->cancelling);
	/* too late to stop: the question is moot */
	if (!bp->can_cancel && in->cancel_sheet) {
		Sheet *sh = in->cancel_sheet;

		in->cancel_sheet = NULL;
		sheet_close(sh, SHEET_CANCEL);
	}
}

static gboolean go_finished(gpointer data)
{
	Page *p = data;
	Installing *in = p->priv;

	in->finish_timer = 0;
	wizard_go(pages_wizard(), PAGE_FINISHED, +1);
	return G_SOURCE_REMOVE;
}

static void on_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	Installing *in = p->priv;
	Wizard *w = pages_wizard();

	in->running = FALSE;
	ring_spinner_set_active(in->spinner, FALSE);
	gtk_widget_hide(in->cancel);
	/* "Stop the installation?" has no meaning any more */
	if (in->cancel_sheet) {
		Sheet *sh = in->cancel_sheet;

		in->cancel_sheet = NULL;
		sheet_close(sh, SHEET_CANCEL);
	}
	if (rc == 0) {
		/* the passwords are on the new system; nothing needs them
		 * again */
		explicit_bzero(app->cfg.root_password,
			       sizeof(app->cfg.root_password));
		explicit_bzero(app->cfg.user.password,
			       sizeof(app->cfg.user.password));
	}
	if (rc == 0)
		app_log(app, IL_LOG_INFO, "ui: the installation finished");
	else if (rc == -ECANCELED)
		app_log(app, IL_LOG_INFO, "ui: the installation was cancelled");
	else
		app_log(app, IL_LOG_INFO, "ui: the installation failed in step %d "
			"(%s): %s", app->failed_step + 1,
			il_step_title(app->failed_step), app->install_error);
	if (rc == 0) {
		for (int i = 0; i < IL_STEP_COUNT; i++)
			step_list_set_state(in->steps, i, STEP_DONE);
		gtk_label_set_text(GTK_LABEL(in->percent), "100% complete");
		progress_ring_set_progress(in->ring, 1.0, 100);
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(in->bar), 1.0);
		gtk_label_set_text(GTK_LABEL(in->detail), "LikeOS is installed.");
		gtk_label_set_text(GTK_LABEL(in->eta), "");
		wizard_clear_history(w);
		in->finish_timer = g_timeout_add(app->no_splash_delay ? 50 : 900,
						 go_finished, p);
		return;
	}
	if (rc == -ECANCELED) {
		app->install_rc = -ECANCELED;
		wizard_history_truncate(w, PAGE_SUMMARY);
		wizard_go(w, PAGE_SUMMARY, -1);
		return;
	}
	if (in->last_step >= 0)
		step_list_set_state(in->steps, in->last_step, STEP_FAILED);
	wizard_go(w, PAGE_ERROR, +1);
}

static void cancel_confirmed(Sheet *s, int response, gpointer data)
{
	Page *p = data;
	Installing *in = p->priv;

	(void)s;
	in->cancel_sheet = NULL;
	if (response != SHEET_OK || !in->running)
		return;
	if (!bridge_can_cancel(p->app->bridge)) {
		gtk_widget_hide(in->cancel);
		return;
	}
	in->cancelling = TRUE;
	app_log(p->app, IL_LOG_INFO, "ui: cancel requested");
	gtk_button_set_label(GTK_BUTTON(in->cancel), "Cancelling\xe2\x80\xa6");
	gtk_widget_set_sensitive(in->cancel, FALSE);
	bridge_cancel(p->app->bridge);
}

static void on_cancel(GtkButton *b, gpointer data)
{
	Page *p = data;

	Installing *in = p->priv;
	Sheet *s;

	(void)b;
	s = sheet_new(p->app, "Stop the installation?", ART_WARNING);
	sheet_set_text(s, "Nothing has been written to the disk yet. You go back "
		       "to the summary and can change your choices.");
	sheet_add_button(s, "_Continue", SHEET_CANCEL, NULL);
	sheet_add_button(s, "_Stop", SHEET_OK, "danger");
	in->cancel_sheet = s;
	sheet_present(s, cancel_confirmed, p);
}

static GtkWidget *installing_build(Page *p)
{
	Installing *in = p->priv;
	GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 48);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *row, *scroller;

	ui_add_class(root, "il-installing");

	in->title = ui_label("Installing LikeOS", "il-install-title");
	gtk_box_pack_start(GTK_BOX(left), in->title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(left),
			   ui_label("This will take a while. Your computer will not "
				    "restart until setup is finished.",
				    "il-install-subtitle"), FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	ui_add_class(row, "il-install-progress-row");
	in->spinner = ring_spinner_new(22);
	gtk_box_pack_start(GTK_BOX(row), in->spinner, FALSE, FALSE, 0);
	in->percent = ui_label("0% complete", "il-install-percent");
	gtk_widget_set_valign(in->percent, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), in->percent, FALSE, FALSE, 0);
	in->eta = ui_label("", "il-install-eta");
	gtk_widget_set_valign(in->eta, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start(in->eta, 14);
	gtk_box_pack_start(GTK_BOX(row), in->eta, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(left), row, FALSE, FALSE, 0);

	in->bar = gtk_progress_bar_new();
	ui_add_class(in->bar, "il-install-bar");
	gtk_box_pack_start(GTK_BOX(left), in->bar, FALSE, FALSE, 0);

	in->steps = step_list_new();
	ui_add_class(in->steps, "il-steps-dark");
	for (int i = 0; i < IL_STEP_COUNT; i++)
		step_list_add(in->steps, il_step_title(i));
	gtk_box_pack_start(GTK_BOX(left), in->steps, FALSE, FALSE, 0);

	in->step_label = ui_label("", "il-install-step");
	gtk_box_pack_start(GTK_BOX(left), in->step_label, FALSE, FALSE, 0);
	in->detail = ui_label("", "il-install-detail");
	gtk_label_set_line_wrap(GTK_LABEL(in->detail), FALSE);
	gtk_label_set_ellipsize(GTK_LABEL(in->detail), PANGO_ELLIPSIZE_MIDDLE);
	gtk_box_pack_start(GTK_BOX(left), in->detail, FALSE, FALSE, 0);
	in->stats = ui_label("", "il-install-stats");
	gtk_box_pack_start(GTK_BOX(left), in->stats, FALSE, FALSE, 0);

	in->cancel = ui_button("_Cancel", "il-button-light");
	gtk_widget_set_halign(in->cancel, GTK_ALIGN_START);
	gtk_widget_set_no_show_all(in->cancel, TRUE);
	g_signal_connect(in->cancel, "clicked", G_CALLBACK(on_cancel), p);
	gtk_box_pack_start(GTK_BOX(left), in->cancel, FALSE, FALSE, 0);

	gtk_widget_set_size_request(left, 420, -1);
	gtk_box_pack_start(GTK_BOX(root), left, FALSE, FALSE, 0);

	in->ring_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	gtk_widget_set_valign(in->ring_col, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(in->ring_col, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(in->ring_col, TRUE);
	in->ring = progress_ring_new(220);
	ui_add_class(in->ring, "il-install-ring");
	gtk_box_pack_start(GTK_BOX(in->ring_col), in->ring, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), in->ring_col, FALSE, FALSE, 0);

	scroller = ui_scroller(root);
	ui_add_class(scroller, "il-install-scroller");
	return scroller;
}

static void installing_enter(Page *p, int direction)
{
	Installing *in = p->priv;
	App *app = p->app;

	(void)direction;
	in->cancelling = FALSE;
	in->last_step = -1;
	gtk_button_set_label(GTK_BUTTON(in->cancel), "_Cancel");
	gtk_widget_set_sensitive(in->cancel, TRUE);
	gtk_widget_show(in->cancel);
	step_list_reset(in->steps);
	progress_ring_set_progress(in->ring, 0, 0);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(in->bar), 0);
	gtk_label_set_text(GTK_LABEL(in->percent), "0% complete");
	gtk_label_set_text(GTK_LABEL(in->detail), "Starting\xe2\x80\xa6");
	gtk_label_set_text(GTK_LABEL(in->stats), "");
	gtk_label_set_text(GTK_LABEL(in->eta), "");
	gtk_label_set_text(GTK_LABEL(in->step_label), "");
	ring_spinner_set_active(in->spinner, TRUE);
	app->install_rc = 0;
	app->install_error[0] = '\0';
	in->running = bridge_install(app->bridge, on_progress, on_done, p);
	if (!in->running) {
		g_strlcpy(app->install_error, "Setup is still busy with another task.",
			  sizeof(app->install_error));
		app->install_rc = -EBUSY;
		wizard_go(pages_wizard(), PAGE_ERROR, +1);
	}
}

static void installing_leave(Page *p)
{
	Installing *in = p->priv;

	if (in->finish_timer) {
		g_source_remove(in->finish_timer);
		in->finish_timer = 0;
	}
}

static void installing_relayout(Page *p)
{
	Installing *in = p->priv;

	GtkWidget *left = gtk_widget_get_parent(in->steps);

	gtk_widget_set_visible(in->ring_col, !p->app->narrow);
	gtk_widget_set_visible(in->bar, p->app->narrow);
	/* without the ring the column takes the whole width */
	gtk_box_set_child_packing(GTK_BOX(gtk_widget_get_parent(left)), left,
				  p->app->narrow, p->app->narrow, 0, GTK_PACK_START);
	gtk_widget_set_size_request(left, p->app->narrow ? -1 :
				    MIN(720, p->app->screen_w / 2), -1);
	/* on a big screen the ring grows, and the whole sits in the middle
	 * of the height instead of under the top edge */
	progress_ring_set_size(in->ring, p->app->tier == TIER_LARGE ? 300 : 220);
	gtk_widget_set_valign(gtk_widget_get_parent(left),
			      p->app->tier == TIER_COMPACT ? GTK_ALIGN_START :
			      GTK_ALIGN_CENTER);
}

void page_installing_init(Page *p)
{
	p->id = "installing";
	p->scene = SCENE_ACCENT;
	p->transient = TRUE;
	p->build = installing_build;
	p->enter = installing_enter;
	p->leave = installing_leave;
	p->relayout = installing_relayout;
	p->next_visible = FALSE;
	p->back_ok = FALSE;
	p->priv = g_new0(Installing, 1);
}
