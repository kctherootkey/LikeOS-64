// LikeOS -- File Explorer: a window.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-window.h"
#include "ui/ex-actions.h"
#include "ui/ex-conflict-dialog.h"
#include "ui/ex-style.h"
#include "ui/ex-tab.h"
#include "backend/ex-apps.h"
#include "services/ex-clipboard.h"
#include "services/ex-opqueue.h"
#include "services/ex-settings.h"
#include "services/ex-undo.h"

#include <string.h>

struct _ExWindow {
	GtkApplicationWindow parent;
	GtkWidget *nb;
	gboolean closing;
};

G_DEFINE_TYPE(ExWindow, ex_window, GTK_TYPE_APPLICATION_WINDOW)

/* ---- tabs ----------------------------------------------------------------- */

ExTab *ex_window_current_tab(ExWindow *w)
{
	int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->nb));

	if (n < 0)
		return NULL;
	return ex_tab_from_widget(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), n));
}

ExTab *ex_window_add_tab(ExWindow *w, ExLocation *loc, gboolean focus)
{
	ExTab *t = ex_tab_new(w, loc);
	GtkWidget *page = ex_tab_widget(t);
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->nb));
	int n = gtk_notebook_insert_page(GTK_NOTEBOOK(w->nb), page,
					 ex_tab_label(t), cur + 1);

	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(w->nb), page, TRUE);
	gtk_container_child_set(GTK_CONTAINER(w->nb), page, "tab-expand", FALSE,
				NULL);
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->nb), TRUE);
	if (focus) {
		gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), n);
		/* The keyboard goes to the items: typing selects, keys act. */
		ex_tab_focus_view(t);
	}
	return t;
}

void ex_window_close_tab(ExWindow *w, ExTab *t)
{
	if (!t)
		return;
	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb)) <= 1) {
		gtk_window_close(GTK_WINDOW(w));
		return;
	}
	ex_tab_destroy(t);
}

static gboolean update_later(gpointer data)
{
	ex_window_update_actions(data);
	return G_SOURCE_REMOVE;
}

static void switched(GtkNotebook *nb, GtkWidget *page, guint n, ExWindow *w)
{
	ExTab *t = ex_tab_from_widget(page);
	char *title, *wt;

	if (!t)
		return;
	title = ex_tab_title(t);
	wt = g_strdup_printf("%s - File Explorer", title);
	gtk_window_set_title(GTK_WINDOW(w), wt);
	g_free(wt);
	g_free(title);
	ex_tab_focus_view(t);
	/* After the switch is complete: the actions ask for the new tab. */
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, update_later, g_object_ref(w),
			g_object_unref);
}

static void page_removed(GtkNotebook *nb, GtkWidget *page, guint n, ExWindow *w)
{
	if (!w->closing && gtk_notebook_get_n_pages(nb) == 0)
		gtk_window_close(GTK_WINDOW(w));
}

void ex_window_update_actions(ExWindow *w)
{
	if (!w->closing)
		ex_actions_update(w);
}

/* ---- running operations ---------------------------------------------------- */

static void op_conflict(ExOperation *op, ExConflict *c, ExWindow *w)
{
	ex_conflict_dialog_show(GTK_WINDOW(w), op, c);
}

static void op_issue(ExOperation *op, ExIssue *i, ExWindow *w)
{
	ex_issue_dialog_show(GTK_WINDOW(w), op, i);
}

static void op_select(ExOperation *op, ExWindow *w)
{
	ExTab *t = ex_window_current_tab(w);
	GPtrArray *res = ex_operation_get_results(op);
	GPtrArray *sel;
	GFile *dir;

	if (!t || !res->len)
		return;
	dir = ex_tab_folder(t);
	sel = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint i = 0; i < res->len; i++) {
		ExOpResult *r = g_ptr_array_index(res, i);
		GFile *parent = r->dest ? g_file_get_parent(r->dest) : NULL;

		if (parent && dir && g_file_equal(parent, dir))
			g_ptr_array_add(sel, g_object_ref(r->dest));
		g_clear_object(&parent);
	}
	if (sel->len)
		ex_tab_select_files(t, sel);
	g_ptr_array_unref(sel);
}

void ex_window_run(ExWindow *w, ExOperation *op, gboolean undoable,
		   gboolean select_results)
{
	/* Connected before the queue can start it. */
	g_signal_connect_object(op, "conflict", G_CALLBACK(op_conflict), w, 0);
	g_signal_connect_object(op, "issue", G_CALLBACK(op_issue), w, 0);
	if (select_results)
		g_signal_connect_object(op, "finished", G_CALLBACK(op_select), w, 0);
	ex_opqueue_add(ex_opqueue_get(), op, undoable);
}

/* ---- opening ------------------------------------------------------------------ */

void ex_window_show_error(ExWindow *w, const char *primary,
			  const char *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w),
					      GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
					      "%s", primary);

	gtk_window_set_title(GTK_WINDOW(d), "File Explorer");
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	ex_style_fixed_dialog(d);
	gtk_widget_show(d);
}

static GAppLaunchContext *launch_context(ExWindow *w)
{
	GdkAppLaunchContext *c = gdk_display_get_app_launch_context(
		gtk_widget_get_display(GTK_WIDGET(w)));

	gdk_app_launch_context_set_timestamp(c, gtk_get_current_event_time());
	return G_APP_LAUNCH_CONTEXT(c);
}

/* What was opened goes into the shared recent list, as GTK programs do. */
static void remember(GPtrArray *files)
{
	GtkRecentManager *rm = gtk_recent_manager_get_default();

	for (guint i = 0; i < files->len; i++) {
		char *uri = g_file_get_uri(g_ptr_array_index(files, i));

		gtk_recent_manager_add_item(rm, uri);
		g_free(uri);
	}
}

void ex_window_open_with(ExWindow *w, GPtrArray *files, GAppInfo *app)
{
	GAppLaunchContext *ctx = launch_context(w);
	GError *e = NULL;

	if (!ex_apps_launch(app, files, ctx, &e)) {
		ex_window_show_error(w, "The app could not be started",
				     e ? e->message : NULL);
		g_clear_error(&e);
	} else {
		remember(files);
	}
	g_object_unref(ctx);
}

/* A part of a command line, quoted for it: in double quotes, with what
 * is special there escaped, and % doubled (a field code otherwise). */
static void add_quoted(GString *cmd, const char *arg)
{
	g_string_append(cmd, " \"");
	for (const char *c = arg; *c; c++) {
		if (strchr("\"`$\\", *c))
			g_string_append_c(cmd, '\\');
		if (*c == '%')
			g_string_append_c(cmd, '%');
		g_string_append_c(cmd, *c);
	}
	g_string_append_c(cmd, '"');
}

/* A program starts in its own folder, as on Windows.  A console program
 * gets a terminal window, which stays open once the program has ended so
 * that what it printed can be read. */
static void run_program(ExWindow *w, GFile *f, ExProgramKind kind)
{
	char *path = g_file_get_path(f);
	char *dir = g_path_get_dirname(path);
	GError *e = NULL;
	gboolean ok;

	if (kind == EX_PROGRAM_CONSOLE) {
		GString *cmd = g_string_new("/bin/sh -c");
		char *name = g_file_get_basename(f);
		GAppLaunchContext *ctx = launch_context(w);
		GAppInfo *app;

		add_quoted(cmd, "cd -- \"$1\" 2>/dev/null; \"$0\"; s=$?; echo; "
				"echo \"[Process exited with code $s]\"; "
				"printf 'Press Enter to close this window. '; "
				"read -r _");
		add_quoted(cmd, path);
		add_quoted(cmd, dir);
		app = g_app_info_create_from_commandline(
			cmd->str, name, G_APP_INFO_CREATE_NEEDS_TERMINAL, &e);
		ok = app && g_app_info_launch(app, NULL, ctx, &e);
		g_clear_object(&app);
		g_object_unref(ctx);
		g_free(name);
		g_string_free(cmd, TRUE);
	} else {
		char *argv[] = { path, NULL };

		ok = g_spawn_async(dir, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
				   NULL, &e);
	}
	if (!ok) {
		ex_window_show_error(w, "The program could not be started",
				     e ? e->message : NULL);
		g_clear_error(&e);
	}
	g_free(dir);
	g_free(path);
}

void ex_window_open(ExWindow *w, GPtrArray *files, gboolean new_tab)
{
	GPtrArray *docs = g_ptr_array_new_with_free_func(g_object_unref);
	gboolean first_dir = TRUE;
	ExProgramKind kind;

	for (guint i = 0; i < files->len; i++) {
		ExFile *f = g_ptr_array_index(files, i);
		GFile *gf = ex_file_get_file(f);

		if (ex_file_is_dir(f)) {
			ExLocation *l = ex_location_new_folder(gf);

			/* The first folder replaces the view (unless a new tab
			 * was asked for); further ones open next to it. */
			if (first_dir && !new_tab)
				ex_tab_go(ex_window_current_tab(w), l);
			else
				ex_window_add_tab(w, l, FALSE);
			first_dir = FALSE;
			ex_location_unref(l);
		} else if ((kind = ex_apps_program_kind(
				    gf, (ex_file_get_caps(f) & EX_CAP_EXECUTE) != 0)) !=
			   EX_PROGRAM_NONE) {
			run_program(w, gf, kind);
		} else {
			g_ptr_array_add(docs, g_object_ref(gf));
		}
	}
	if (docs->len) {
		GAppLaunchContext *ctx = launch_context(w);
		GError *e = NULL;

		if (!ex_apps_launch(NULL, docs, ctx, &e)) {
			/* Nothing is set to open it: let the user pick. */
			if (docs->len == 1 &&
			    g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
				g_action_group_activate_action(G_ACTION_GROUP(w),
							       "open-with-other",
							       NULL);
			else
				ex_window_show_error(w, "The item could not be opened",
						     e ? e->message : NULL);
			g_clear_error(&e);
		} else {
			remember(docs);
		}
		g_object_unref(ctx);
	}
	g_ptr_array_unref(docs);
}

/* ---- keys -------------------------------------------------------------------- */

static gboolean ex_window_key_press(GtkWidget *wid, GdkEventKey *ev)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(wid));

	/* While typing -- the address bar, the search box, a name being
	 * edited -- the text field gets the key first: Delete deletes a
	 * character, not the selected files, and Ctrl+C copies text. */
	if (focus && gtk_widget_get_mapped(focus) &&
	    (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus)) &&
	    gtk_window_propagate_key_event(GTK_WINDOW(wid), ev))
		return TRUE;
	return GTK_WIDGET_CLASS(ex_window_parent_class)->key_press_event(wid, ev);
}

/* ---- settings and life cycle ------------------------------------------------- */

static void each_tab(ExWindow *w, void (*fn)(ExTab *))
{
	int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));

	for (int i = 0; i < n; i++) {
		ExTab *t = ex_tab_from_widget(
			gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), i));

		if (t)
			fn(t);
	}
}

static void setting_changed(ExSettings *s, const char *key, ExWindow *w)
{
	if (!strcmp(key, "show-hidden") || !strcmp(key, "show-extensions") ||
	    !strcmp(key, "single-click") || !strcmp(key, "right-pane") ||
	    !strcmp(key, "show-navigation-pane"))
		each_tab(w, ex_tab_apply_settings);
	ex_window_update_actions(w);
}

static void state_changed(gpointer src, ExWindow *w)
{
	ex_window_update_actions(w);
	gtk_widget_queue_draw(GTK_WIDGET(w)); /* cut items dim, or stop dimming */
}

/* Coming back to a window, Paste must reflect what was copied elsewhere
 * meanwhile, even if no owner-change notification arrived. */
static void became_active(GtkWindow *win, GParamSpec *ps, gpointer data)
{
	if (gtk_window_is_active(win))
		ex_clipboard_refresh(ex_clipboard_get());
}

static gboolean delete_event(GtkWidget *wid, GdkEventAny *ev)
{
	ExWindow *w = EX_WINDOW(wid);
	ExSettings *s = ex_settings_get();
	gboolean max = gtk_window_is_maximized(GTK_WINDOW(w));
	int width, height;

	if (!max) {
		gtk_window_get_size(GTK_WINDOW(w), &width, &height);
		ex_settings_set_pair(s, "window-size", width, height);
	}
	ex_settings_set_bool(s, "window-maximized", max);
	w->closing = TRUE;
	return FALSE;
}

static void ex_window_dispose(GObject *o)
{
	ExWindow *w = EX_WINDOW(o);

	w->closing = TRUE;
	g_signal_handlers_disconnect_by_data(ex_settings_get(), w);
	g_signal_handlers_disconnect_by_data(ex_clipboard_get(), w);
	g_signal_handlers_disconnect_by_data(ex_undo_get(), w);
	G_OBJECT_CLASS(ex_window_parent_class)->dispose(o);
}

static void ex_window_class_init(ExWindowClass *k)
{
	G_OBJECT_CLASS(k)->dispose = ex_window_dispose;
	GTK_WIDGET_CLASS(k)->key_press_event = ex_window_key_press;
	GTK_WIDGET_CLASS(k)->delete_event = delete_event;
}

static void ex_window_init(ExWindow *w)
{
	ExSettings *s = ex_settings_get();
	GtkWidget *plus;
	int width, height;

	ex_style_add(GTK_WIDGET(w), EX_CLASS_WINDOW);
	gtk_window_set_icon_name(GTK_WINDOW(w), "system-file-manager");
	ex_settings_get_pair(s, "window-size", &width, &height);
	gtk_window_set_default_size(GTK_WINDOW(w), MAX(width, 640),
				    MAX(height, 420));
	if (ex_settings_get_bool(s, "window-maximized"))
		gtk_window_maximize(GTK_WINDOW(w));

	w->nb = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(w->nb), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(w->nb), FALSE);
	gtk_notebook_set_group_name(GTK_NOTEBOOK(w->nb), "likeos-explorer");
	plus = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(plus), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(plus, "New tab (Ctrl+T)");
	atk_object_set_name(gtk_widget_get_accessible(plus), "New tab");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(plus), "win.new-tab");
	gtk_widget_show(plus);
	gtk_notebook_set_action_widget(GTK_NOTEBOOK(w->nb), plus, GTK_PACK_END);
	g_signal_connect(w->nb, "switch-page", G_CALLBACK(switched), w);
	g_signal_connect(w->nb, "page-removed", G_CALLBACK(page_removed), w);
	gtk_container_add(GTK_CONTAINER(w), w->nb);
	gtk_widget_show(w->nb);

	ex_actions_install(w);
	g_signal_connect(s, "changed", G_CALLBACK(setting_changed), w);
	g_signal_connect(ex_clipboard_get(), "changed", G_CALLBACK(state_changed), w);
	g_signal_connect(ex_undo_get(), "changed", G_CALLBACK(state_changed), w);
	g_signal_connect(w, "notify::is-active", G_CALLBACK(became_active), NULL);
}

ExWindow *ex_window_new(GtkApplication *app, ExLocation *loc)
{
	ExWindow *w = g_object_new(EX_TYPE_WINDOW, "application", app, NULL);

	ex_window_add_tab(w, loc, TRUE);
	ex_window_update_actions(w);
	return w;
}
