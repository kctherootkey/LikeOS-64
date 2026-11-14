/* LikeOS installer -- the error page.
 *
 * What failed (the step and the backend's sentence), whether the disk was
 * already changed, where the log is, and the last lines of the log behind
 * a "details" expander.  Try again runs the installation once more with
 * the same choices; Back to the summary lets them be changed first.
 *
 * A plan describes changes to the disk as it was scanned.  Once the
 * partition table is written that disk is gone, so a second attempt first
 * looks at the disks again: after a failure in a later step the new table
 * is the planned one, and the new plan simply installs on the same
 * partition (formatting it, and the EFI system partition when the first
 * plan did, again).  When the partition step itself failed nobody knows
 * how far it got: the disk page shows the disk as it is now.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <errno.h>

typedef struct {
	GtkWidget *step;
	GtkWidget *notice;
	GtkWidget *disk_state;
	GtkWidget *log_path;
	GtkWidget *log_view;
	GtkWidget *expander;
	gboolean retrying;
	/* what the failed plan did, for the next attempt */
	char dev[64];
	int root;
	int esp;
	gboolean esp_formatted;
} ErrorPage;

static gboolean scroll_to_end(gpointer data)
{
	ErrorPage *e = data;
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->log_view));
	GtkTextIter end;

	gtk_text_buffer_get_end_iter(buf, &end);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(e->log_view), &end, 0, FALSE,
				     0, 1.0);
	return G_SOURCE_REMOVE;
}

/* The newest lines are the interesting ones: show the end of the log. */
static void on_expanded(GObject *o, GParamSpec *ps, gpointer data)
{
	(void)ps;
	if (gtk_expander_get_expanded(GTK_EXPANDER(o)))
		g_timeout_add(80, scroll_to_end, data);
}

static GtkWidget *error_build(Page *p)
{
	ErrorPage *e = p->priv;
	GtkWidget *body, *root, *sw;

	root = card_page_new("Something went wrong",
			     "LikeOS could not be installed.", &body);
	e->step = ui_label("", "il-text");
	gtk_box_pack_start(GTK_BOX(body), e->step, FALSE, FALSE, 0);
	e->notice = notice_new();
	gtk_box_pack_start(GTK_BOX(body), e->notice, FALSE, FALSE, 0);
	e->disk_state = ui_check_row("", ICON_DISK);
	gtk_box_pack_start(GTK_BOX(body), e->disk_state, FALSE, FALSE, 0);
	e->log_path = ui_check_row("", ICON_INFO);
	gtk_box_pack_start(GTK_BOX(body), e->log_path, FALSE, FALSE, 0);

	e->expander = gtk_expander_new_with_mnemonic("Show the _details");
	ui_add_class(e->expander, "il-expander");
	e->log_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(e->log_view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(e->log_view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(e->log_view), GTK_WRAP_CHAR);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(e->log_view), FALSE);
	ui_add_class(e->log_view, "il-log");
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 160);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), e->log_view);
	ui_add_class(sw, "il-list-frame");
	gtk_container_add(GTK_CONTAINER(e->expander), sw);
	g_signal_connect(e->expander, "notify::expanded",
			 G_CALLBACK(on_expanded), e);
	gtk_box_pack_start(GTK_BOX(body), e->expander, FALSE, FALSE, 0);
	return root;
}

static void set_row_text(GtkWidget *row, const char *markup)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(row));
	GtkWidget *label = g_list_last(kids)->data;

	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_list_free(kids);
}

static void error_enter(Page *p, int direction)
{
	ErrorPage *e = p->priv;
	App *app = p->app;
	char *msg, *t, *log;
	int step = app->failed_step;

	(void)direction;
	if (step >= 0 && step < IL_STEP_COUNT)
		t = g_strdup_printf("The step that failed: %s (step %d of %d).",
				    il_step_title(step), step + 1, IL_STEP_COUNT);
	else
		t = g_strdup("The installation could not start.");
	gtk_label_set_text(GTK_LABEL(e->step), t);
	g_free(t);

	{
		char *w = app_sentence(app->install_error[0] ? app->install_error :
				       "The reason is not known; see the log.");

		msg = pages_escape(w);
		g_free(w);
	}
	notice_set(e->notice, NOTICE_ERROR, msg);
	g_free(msg);

	if (step > IL_STEP_PREPARE)
		set_row_text(e->disk_state,
			     "<b>The disk was already changed.</b> Its old "
			     "partitions may be gone. Try again, or choose another "
			     "place to install.");
	else
		set_row_text(e->disk_state, "Nothing was written to the disk.");

	{
		char *path = pages_escape(app_log_path(app));

		t = g_strdup_printf("The full record is in <tt>%s</tt>. Copy it "
				    "to a USB stick to ask for help.", path);
		g_free(path);
	}
	set_row_text(e->log_path, t);
	g_free(t);

	log = bridge_log_tail(app->bridge, 120);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(e->log_view)),
				 log, -1);
	g_free(log);
	gtk_expander_set_expanded(GTK_EXPANDER(e->expander), FALSE);
	p->substep = 0;
	e->retrying = FALSE;
	p->next_ok = TRUE;
	p->back_ok = FALSE;
	/* after the partition table was written, the plan is of a disk that
	 * is gone: changes start from the disk as it is now */
	p->secondary_label = step >= IL_STEP_PARTITION ?
		"Choose the _disk again" : "Back to the _summary";
	page_update(p);
}

/* To the disk page, which looks at the disks again first. */
static void to_disk_page(Page *p, const char *markup)
{
	App *app = p->app;
	Wizard *w = pages_wizard();

	g_free(app->disk_message);
	app->disk_message = g_strdup(markup);
	app->disk_rescan = TRUE;
	app->target_disk = -1;
	wizard_history_truncate(w, PAGE_DISK);
	wizard_go(w, PAGE_DISK, -1);
}

static void retry_failed(Page *p, const char *why)
{
	ErrorPage *e = p->priv;
	App *app = p->app;
	Wizard *w = pages_wizard();
	char *s = app_sentence(why), *m = pages_escape(s);

	char *t;

	app_log(app, IL_LOG_INFO, "ui: the same installation cannot be repeated "
		"(%s); back to the disk page", why);
	e->retrying = FALSE;
	t = g_strdup_printf("<b>The disk was changed by the attempt.</b> %s "
			    "Choose again where LikeOS goes.", m);
	/* the disks were just scanned */
	g_free(app->disk_message);
	app->disk_message = t;
	app->target_disk = -1;
	g_free(m);
	g_free(s);
	wizard_history_truncate(w, PAGE_DISK);
	wizard_go(w, PAGE_DISK, -1);
}

/* The disks were scanned again: the same installation on the new table. */
static void retry_scanned(App *app, int rc, gpointer data)
{
	Page *p = data;
	ErrorPage *e = p->priv;
	const struct il_disk *d;
	char why[IL_ERROR_MAX];
	int di, n;

	if (rc != 0 || !app->disks) {
		retry_failed(p, app->disks_error[0] ? app->disks_error :
			     "The disks could not be read");
		return;
	}
	d = il_disks_find(app->disks, e->dev);
	if (!d) {
		g_snprintf(why, sizeof(why), "The disk %s is not there any more",
			   e->dev);
		retry_failed(p, why);
		return;
	}
	di = (int)(d - app->disks->disks);
	if (!plans_get(app, di)) {
		retry_failed(p, "The disk cannot be used");
		return;
	}
	if (e->esp_formatted) {
		PlanStep st = { PLAN_STEP_FORMAT, e->esp, 0, 0 };

		if (plans_apply(app, di, &st, NULL) != 0) {
			retry_failed(p, il_last_error(app->ctx));
			return;
		}
	}
	if (il_plan_select_root(plans_get(app, di), e->root) != 0) {
		retry_failed(p, il_last_error(app->ctx));
		return;
	}
	n = il_plan_validate(plans_get(app, di), app->src_ok ? &app->src : NULL,
			     why, sizeof(why));
	if (n != 0) {
		retry_failed(p, why);
		return;
	}
	app->target_disk = di;
	e->retrying = FALSE;
	app_log(app, IL_LOG_INFO, "ui: trying again on %s, partition %d",
		e->dev, e->root);
	wizard_go(pages_wizard(), PAGE_INSTALLING, +1);
}

static gboolean error_next(Page *p)
{
	ErrorPage *e = p->priv;
	App *app = p->app;
	il_plan *plan = app_target_plan(app);
	const struct il_disk *d = app_target_disk(app);

	if (e->retrying || bridge_busy(app->bridge))
		return FALSE;
	/* before the partition table: the same plan, unchanged */
	if (app->failed_step <= IL_STEP_PREPARE || !plan || !d) {
		app_log(app, IL_LOG_INFO, "ui: trying the installation again");
		wizard_go(pages_wizard(), PAGE_INSTALLING, +1);
		return FALSE;
	}
	g_strlcpy(e->dev, d->dev, sizeof(e->dev));
	e->root = il_plan_root(plan);
	e->esp = il_plan_esp(plan);
	e->esp_formatted = FALSE;
	for (int i = 0; i < il_plan_nops(plan); i++) {
		const struct il_op *op = il_plan_op(plan, i);

		if (op->kind == IL_OP_FORMAT && op->partno == e->esp)
			e->esp_formatted = TRUE;
	}
	if (app->failed_step == IL_STEP_PARTITION) {
		/* how far did it get?  The disk page shows it. */
		app_log(app, IL_LOG_INFO, "ui: trying again after a failed "
			"partition step: back to the disk page");
		to_disk_page(p, "<b>The partition table may have been changed."
			     "</b> This is the disk as it is now. Choose again "
			     "where LikeOS goes.");
		return FALSE;
	}
	app_log(app, IL_LOG_INFO, "ui: trying again: looking at the disks "
		"first (the partition table was written)");
	e->retrying = TRUE;
	p->next_ok = FALSE;
	p->secondary_label = NULL;
	page_update(p);
	notice_set(e->notice, NOTICE_INFO, "Looking at the disk again\xe2\x80\xa6");
	if (!bridge_scan_disks(app->bridge, retry_scanned, p)) {
		e->retrying = FALSE;
		p->next_ok = TRUE;
		page_update(p);
	}
	return FALSE;
}

static void error_secondary(Page *p)
{
	Wizard *w = pages_wizard();
	ErrorPage *e = p->priv;

	if (e->retrying || bridge_busy(p->app->bridge))
		return;
	if (p->app->failed_step >= IL_STEP_PARTITION) {
		app_log(p->app, IL_LOG_INFO, "ui: back to the disk page after "
			"the failure");
		to_disk_page(p, "<b>The disk was changed by the attempt.</b> "
			     "This is the disk as it is now. Choose where "
			     "LikeOS goes.");
		return;
	}
	app_log(p->app, IL_LOG_INFO, "ui: back to the summary after the failure");
	wizard_history_truncate(w, PAGE_SUMMARY);
	wizard_go(w, PAGE_SUMMARY, -1);
}

static int error_autofill(Page *p, const struct il_answers *a)
{
	(void)p;
	(void)a;
	/* an autopilot run ends here (autopilot.c) */
	return AUTOFILL_FAIL;
}

void page_error_init(Page *p)
{
	p->id = "error";
	p->scene = SCENE_CARD;
	p->build = error_build;
	p->enter = error_enter;
	p->go_next = error_next;
	p->secondary = error_secondary;
	p->autofill = error_autofill;
	p->next_label = "_Try again";
	p->secondary_label = "Back to the _summary";
	p->back_ok = FALSE;
	p->art = ART_WARNING;
	p->priv = g_new0(ErrorPage, 1);
}
