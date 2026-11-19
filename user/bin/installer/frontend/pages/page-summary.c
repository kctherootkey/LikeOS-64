/* LikeOS installer -- page 10: ready to install.
 *
 * Everything that was chosen, the list of the changes the disk is going to
 * get, and a clear warning when something is erased.  Two extras: checking
 * the installation medium (reads the whole image and compares its
 * checksum), and saving the choices as an answer file for the text-mode
 * installer.  Install starts the one and only pass; the computer restarts
 * only at the very end.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <errno.h>
#include <string.h>

#define ANSWERS_PATH "/tmp/likeos-install.conf"

typedef struct {
	GtkWidget *recap;
	GtkWidget *ops_title;
	GtkWidget *ops;
	GtkWidget *warning;
	GtkWidget *status;
	GtkWidget *problem;      /* the choices do not fit together */
	gboolean destructive;
	gboolean config_ok;
	gboolean verifying;
	gboolean answers_partial;
	char *erase_text;
	/* the medium check */
	Sheet *verify_sheet;
	GtkWidget *verify_bar;
	GtkWidget *verify_label;
} Summary;

static void clear_box(GtkWidget *box)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(box));

	for (GList *k = kids; k; k = k->next)
		gtk_widget_destroy(k->data);
	g_list_free(kids);
}

static void add_recap(Summary *s, IconKind icon, const char *title,
		      const char *value)
{
	char *t = pages_escape(title), *v = pages_escape(value);
	char *m = g_strdup_printf("<span weight=\"600\">%s</span>\n%s", t, v);
	GtkWidget *row = ui_check_row(m, icon);

	gtk_widget_show_all(row);
	gtk_box_pack_start(GTK_BOX(s->recap), row, FALSE, FALSE, 0);
	g_free(m);
	g_free(t);
	g_free(v);
}

static void fill(Page *p)
{
	Summary *s = p->priv;
	App *app = p->app;
	il_plan *plan = app_target_plan(app);
	const struct il_disk *disk = app_target_disk(app);
	struct il_disk *view = g_new0(struct il_disk, 1);
	const struct il_partition *root = NULL;
	char *t, *size;
	int nops = plan ? il_plan_nops(plan) : 0;
	gboolean erases_all = FALSE;
	GString *erased = g_string_new(NULL);

	clear_box(s->recap);
	clear_box(s->ops);

	if (plan) {
		il_plan_view(plan, view);
		for (int i = 0; i < view->nparts; i++)
			if (view->parts[i].number == il_plan_root(plan))
				root = &view->parts[i];
	}
	if (disk && root) {
		size = app_size_text(root->size_bytes);
		t = g_strdup_printf("LikeOS %s on Disk %d, partition %d (%s) of %s",
				    app->src_ok ? app->src.version : "",
				    app->target_disk, root->number, size,
				    disk->model[0] ? disk->model : disk->dev);
		add_recap(s, ICON_DISK, "Install", t);
		g_free(t);
		g_free(size);
	} else {
		add_recap(s, ICON_WARNING, "Install", "No disk was chosen yet. Go "
			  "back to the disk page.");
	}
	t = g_strdup_printf("%s \xc2\xb7 formats: %s", app_locale_label(app->cfg.locale),
			    app_locale_label(app->cfg.time_format));
	add_recap(s, ICON_LANGUAGE, "Language", t);
	g_free(t);
	t = app_layouts_summary(app);
	add_recap(s, ICON_KEYBOARD, app->cfg.nlayouts > 1 ? "Keyboard layouts" :
		  "Keyboard layout", t);
	g_free(t);
	add_recap(s, ICON_CLOCK, "Time zone", app_timezone_label(app->cfg.timezone));
	add_recap(s, ICON_COMPUTER, "Computer name", app->cfg.hostname);
	if (app->cfg.user.fullname[0])
		t = g_strdup_printf("root and %s (%s)", app->cfg.user.fullname,
				    app->cfg.user.username);
	else
		t = g_strdup_printf("root and %s", app->cfg.user.username[0] ?
				    app->cfg.user.username : "\xe2\x80\x94");
	add_recap(s, ICON_USER, "Accounts", t);
	g_free(t);

	/* the changes to the disk */
	s->destructive = FALSE;
	for (int i = 0; i < nops; i++) {
		const struct il_op *op = il_plan_op(plan, i);
		char line[200];
		GtkWidget *row;
		char *e;
		IconKind icon = ICON_DOT;

		if (!op)
			continue;
		il_op_describe(op, line, sizeof(line));
		switch (op->kind) {
		case IL_OP_NEW_TABLE:
			erases_all = TRUE;
			s->destructive = TRUE;
			icon = ICON_WARNING;
			break;
		case IL_OP_DELETE:
			s->destructive = TRUE;
			icon = ICON_DELETE;
			g_string_append_printf(erased, "%spartition %d",
					       erased->len ? ", " : "", op->partno);
			break;
		case IL_OP_CREATE:
			icon = ICON_NEW;
			break;
		case IL_OP_EXTEND:
			icon = ICON_EXTEND;
			break;
		case IL_OP_FORMAT: {
			const struct il_disk *orig = il_plan_disk(plan);
			gboolean existed = FALSE;

			/* a partition that is on the disk now, whatever it
			 * holds: an unknown file system is not an empty one */
			for (int k = 0; orig && k < orig->nparts; k++)
				if (orig->parts[k].number == op->partno)
					existed = TRUE;
			for (int k = 0; k < nops; k++) {
				const struct il_op *o = il_plan_op(plan, k);

				if (o && o->kind == IL_OP_CREATE &&
				    o->partno == op->partno)
					existed = FALSE;
			}
			if (existed && !erases_all) {
				s->destructive = TRUE;
				g_string_append_printf(erased, "%spartition %d",
						       erased->len ? ", " : "",
						       op->partno);
			}
			icon = ICON_FORMAT;
			break;
		}
		default:
			icon = ICON_PARTITION;
			break;
		}
		e = pages_escape(line);
		row = ui_check_row(e, icon);
		ui_add_class(row, "il-op-row");
		gtk_widget_show_all(row);
		gtk_box_pack_start(GTK_BOX(s->ops), row, FALSE, FALSE, 0);
		g_free(e);
	}
	gtk_widget_set_visible(s->ops_title, nops > 0);

	g_free(s->erase_text);
	s->erase_text = NULL;
	if (s->destructive && disk) {
		char *dsize = app_size_text(disk->size_bytes);

		if (erases_all)
			s->erase_text = g_strdup_printf(
				"<b>Everything on Disk %d (%s, %s) will be erased.</b> "
				"Make sure you have a copy of anything on it you want "
				"to keep.", app->target_disk,
				disk->model[0] ? disk->model : disk->dev, dsize);
		else
			s->erase_text = g_strdup_printf(
				"<b>Everything on %s of Disk %d will be erased.</b> "
				"Make sure you have a copy of anything on it you want "
				"to keep.", erased->str, app->target_disk);
		notice_set(s->warning, NOTICE_WARNING, s->erase_text);
		g_free(dsize);
	} else {
		notice_set(s->warning, NOTICE_INFO,
			   "Nothing that is on the disk now is erased: LikeOS goes "
			   "into new or empty space.");
	}
	g_string_free(erased, TRUE);
	g_free(view);

	/* everything chosen, checked once more as a whole */
	{
		char why[IL_ERROR_MAX];

		s->config_ok = il_config_validate(&app->cfg, why, sizeof(why)) == 0;
		if (!s->config_ok) {
			char *w = app_sentence(why), *e = pages_escape(w);
			char *t = g_strdup_printf("<b>Something you chose cannot be "
						  "used.</b>\n%s Go back and change "
						  "it.", e);

			notice_set(s->problem, NOTICE_ERROR, t);
			app_log(app, IL_LOG_INFO, "ui: the choices are not valid: %s",
				why);
			g_free(t);
			g_free(e);
			g_free(w);
		} else {
			notice_hide(s->problem);
		}
	}
	p->next_ok = plan && root && s->config_ok && !s->verifying;
	page_update(p);
}

/* ---- checking the medium ------------------------------------------- */

static void verify_progress(App *app, const BridgeProgress *bp, gpointer data)
{
	Page *p = data;
	Summary *s = p->priv;
	char t[64];

	(void)app;
	if (!s->verify_sheet)
		return;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->verify_bar), bp->overall);
	g_snprintf(t, sizeof(t), "%d%% checked", (int)(bp->overall * 100 + 0.5));
	gtk_label_set_text(GTK_LABEL(s->verify_label), t);
}

static void verify_busy(Page *p, gboolean busy)
{
	Summary *s = p->priv;

	/* no other step while the image is read: the backend does one thing
	 * at a time */
	s->verifying = busy;
	p->back_ok = !busy;
	if (busy) {
		/* the plan is not asked while the worker has the context */
		p->next_ok = FALSE;
		page_update(p);
	} else {
		fill(p);
	}
}

static void verify_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	Summary *s = p->priv;

	if (s->verify_sheet)
		sheet_close(s->verify_sheet, SHEET_OK);
	s->verify_sheet = NULL;
	verify_busy(p, FALSE);
	app_log(app, IL_LOG_INFO, "ui: the check of the installation medium %s",
		rc == 0 ? "passed" : "failed");
	if (rc == 0) {
		notice_set(s->status, NOTICE_SUCCESS,
			   "The installation medium is fine: the system image "
			   "matches its checksum.");
	} else {
		char *w = app_sentence(il_last_error(app->ctx));
		char *m = pages_escape(w);
		char *t = g_strdup_printf("<b>The installation medium is damaged.</b> "
					  "%s Write the medium again before you "
					  "install.", m);

		g_free(w);

		notice_set(s->status, NOTICE_ERROR, t);
		g_free(t);
		g_free(m);
	}
}

static void verify_sheet_closed(Sheet *sh, int response, gpointer data)
{
	Page *p = data;
	Summary *s = p->priv;

	(void)sh;
	(void)response;
	s->verify_sheet = NULL;
}

static void on_verify(GtkButton *b, gpointer data)
{
	Page *p = data;
	Summary *s = p->priv;
	GtkWidget *box;

	(void)b;
	if (bridge_busy(p->app->bridge))
		return;
	s->verify_sheet = sheet_new(p->app, "Checking the installation medium",
				    ART_DISK);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_box_pack_start(GTK_BOX(box),
			   ui_label("Setup reads the whole system image and "
				    "compares it with its checksum. This takes a "
				    "few minutes.", "il-sheet-text"), FALSE, FALSE, 0);
	s->verify_bar = gtk_progress_bar_new();
	ui_add_class(s->verify_bar, "il-progress");
	gtk_box_pack_start(GTK_BOX(box), s->verify_bar, FALSE, FALSE, 0);
	s->verify_label = ui_label("Starting\xe2\x80\xa6", "il-caption");
	gtk_box_pack_start(GTK_BOX(box), s->verify_label, FALSE, FALSE, 0);
	sheet_set_body(s->verify_sheet, box);
	sheet_add_button(s->verify_sheet, "_Hide", SHEET_CANCEL, NULL);
	sheet_present(s->verify_sheet, verify_sheet_closed, p);
	app_log(p->app, IL_LOG_INFO, "ui: checking the installation medium");
	if (!bridge_verify_source(p->app->bridge, verify_progress, verify_done, p))
		sheet_close(s->verify_sheet, SHEET_CANCEL);
	else
		verify_busy(p, TRUE);
}

/* ---- saving the answers -------------------------------------------- */

static void on_save(GtkButton *b, gpointer data)
{
	Page *p = data;
	Summary *s = p->priv;
	App *app = p->app;
	struct il_answers a;
	const struct il_disk *d = app_target_disk(app);
	il_plan *plan = app_target_plan(app);
	int rc;

	(void)b;
	if (bridge_busy(app->bridge))
		return;
	memset(&a, 0, sizeof(a));
	g_strlcpy(a.disk, d ? d->dev : "auto", sizeof(a.disk));
	a.root_partno = plan ? il_plan_root(plan) : -1;
	a.reboot = 1;
	a.cfg = app->cfg;
	/* An answer file knows two layouts: the whole disk, or LikeOS on an
	 * existing partition.  Other changes to the disk are not in it. */
	{
		int n = plan ? il_plan_nops(plan) : 0;
		gboolean whole = n > 0 && il_plan_op(plan, 0)->kind == IL_OP_NEW_TABLE;
		gboolean only_root = TRUE;

		for (int i = 0; i < n; i++) {
			const struct il_op *op = il_plan_op(plan, i);

			if (op->partno != a.root_partno ||
			    (op->kind != IL_OP_SET_TYPE && op->kind != IL_OP_FORMAT))
				only_root = FALSE;
		}
		a.whole_disk = whole;
		s->answers_partial = !whole && !only_root;
	}
	rc = il_answers_save(app->ctx, ANSWERS_PATH, &a, 0);
	/* the copy of the choices holds the passwords */
	explicit_bzero(&a, sizeof(a));
	app_log(app, IL_LOG_INFO, "ui: choices saved to %s: %s", ANSWERS_PATH,
		rc == 0 ? "done" : il_last_error(app->ctx));
	if (rc == 0) {
		notice_set(s->status, s->answers_partial ? NOTICE_WARNING :
			   NOTICE_SUCCESS, s->answers_partial ?
			   "Your choices were saved to <tt>" ANSWERS_PATH "</tt> "
			   "(without the passwords). An answer file can only use "
			   "a whole disk or an existing partition, so the "
			   "partitions you create or delete here are not in it." :
			   "Your choices were saved to <tt>" ANSWERS_PATH "</tt> "
			   "(without the passwords). <tt>likeos-install --answers"
			   "</tt> can repeat this installation.");
	} else {
		char *w = app_sentence(il_last_error(app->ctx));
		char *m = pages_escape(w);

		notice_set(s->status, NOTICE_ERROR, m);
		g_free(m);
		g_free(w);
	}
}

/* ---- the page ------------------------------------------------------ */

static GtkWidget *summary_build(Page *p)
{
	Summary *s = p->priv;
	GtkWidget *body, *root, *links, *l;

	root = setup_page_new("Ready to install", &body);
	gtk_box_pack_start(GTK_BOX(body),
			   ui_label("LikeOS is installed in one pass. Your computer "
				    "does not restart until setup is finished. "
				    "Here is what you chose:", "il-text"),
			   FALSE, FALSE, 0);

	/* results of the extras and a cancelled run: seen first */
	s->status = notice_new();
	gtk_box_pack_start(GTK_BOX(body), s->status, FALSE, FALSE, 0);
	s->problem = notice_new();
	gtk_box_pack_start(GTK_BOX(body), s->problem, FALSE, FALSE, 0);

	s->recap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	ui_add_class(s->recap, "il-recap");
	gtk_box_pack_start(GTK_BOX(body), s->recap, FALSE, FALSE, 0);

	s->ops_title = ui_label("Changes to the disk", "il-section");
	gtk_widget_set_no_show_all(s->ops_title, TRUE);
	gtk_box_pack_start(GTK_BOX(body), s->ops_title, FALSE, FALSE, 0);
	s->ops = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	ui_add_class(s->ops, "il-ops");
	gtk_box_pack_start(GTK_BOX(body), s->ops, FALSE, FALSE, 0);

	s->warning = notice_new();
	gtk_box_pack_start(GTK_BOX(body), s->warning, FALSE, FALSE, 0);

	links = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	gtk_widget_set_halign(links, GTK_ALIGN_START);
	l = link_button_new("Check the installation _medium", ICON_CHECK);
	g_signal_connect(l, "clicked", G_CALLBACK(on_verify), p);
	gtk_box_pack_start(GTK_BOX(links), l, FALSE, FALSE, 0);
	l = link_button_new("Save these _choices to a file", ICON_DOT);
	icon_set(g_object_get_data(G_OBJECT(l), "button-icon"), ICON_FORMAT);
	g_signal_connect(l, "clicked", G_CALLBACK(on_save), p);
	gtk_box_pack_start(GTK_BOX(links), l, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(root), "summary-links", links);
	gtk_box_pack_start(GTK_BOX(body), links, FALSE, FALSE, 0);
	return root;
}

static void summary_enter(Page *p, int direction)
{
	Summary *s = p->priv;
	App *app = p->app;

	(void)direction;
	fill(p);
	if (app->install_rc == -ECANCELED) {
		notice_set(s->status, NOTICE_INFO,
			   "The installation was cancelled before anything was "
			   "written. You can change your choices or start again.");
		app->install_rc = 0;
	} else if (direction > 0) {
		notice_hide(s->status);
	}
}

/* What is about to happen, for the log (and the serial line). */
static void log_start(Page *p)
{
	App *app = p->app;
	il_plan *plan = app_target_plan(app);
	char *sum = g_malloc(8192);

	il_plan_summary(plan, sum, 8192);
	app_log(app, IL_LOG_INFO, "ui: install confirmed; the plan:\n%s", sum);
	g_free(sum);
}

static void install_confirmed(Sheet *sh, int response, gpointer data)
{
	Page *p = data;

	(void)sh;
	if (response == SHEET_OK) {
		log_start(p);
		wizard_go(pages_wizard(), PAGE_INSTALLING, +1);
	} else {
		app_log(p->app, IL_LOG_INFO, "ui: erasing was not confirmed");
	}
}

static gboolean summary_next(Page *p)
{
	Summary *s = p->priv;

	if (bridge_busy(p->app->bridge) || !s->config_ok)
		return FALSE;
	if (s->destructive) {
		sheet_confirm(p->app, "Erase and install LikeOS?", s->erase_text,
			      "_Erase and install", TRUE, ART_WARNING,
			      install_confirmed, p);
		return FALSE;
	}
	log_start(p);
	return TRUE;
}

static int summary_autofill(Page *p, const struct il_answers *a)
{
	(void)a;
	(void)p;
	/* Install; the autopilot confirms the question that may follow */
	return AUTOFILL_NEXT;
}

static void summary_relayout(Page *p)
{
	GtkWidget *links = g_object_get_data(G_OBJECT(p->root), "summary-links");

	if (links)
		gtk_orientable_set_orientation(GTK_ORIENTABLE(links), p->app->narrow ?
			GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
}

void page_summary_init(Page *p)
{
	p->id = "summary";
	p->scene = SCENE_SETUP;
	p->build = summary_build;
	p->enter = summary_enter;
	p->go_next = summary_next;
	p->relayout = summary_relayout;
	p->autofill = summary_autofill;
	p->next_label = "_Install";
	p->priv = g_new0(Summary, 1);
}
