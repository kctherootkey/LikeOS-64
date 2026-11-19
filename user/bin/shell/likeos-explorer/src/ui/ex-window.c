// LikeOS Explorer: a window.
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
	/* The title row: the tabs, New tab, and the caption buttons. */
	GtkWidget *titlebar, *tabs, *plus, *max_btn, *max_glyph;
	/* A press on the title row or a tab, until it turns into a drag. */
	GtkWidget *press_tab;
	double press_x, press_y;
	gboolean press_pending, tab_dragging;
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

/* The tab row mirrors the notebook: the same order, and the current
 * page's tab marked checked (the theme raises it). */
static void sync_tabs(ExWindow *w)
{
	int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->nb));

	for (int i = 0; i < n; i++) {
		GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), i);
		GtkWidget *label = g_object_get_data(G_OBJECT(page), "ex-tab-label");

		if (!label)
			continue;
		gtk_box_reorder_child(GTK_BOX(w->tabs), label, i);
		if (i == cur)
			gtk_widget_set_state_flags(label, GTK_STATE_FLAG_CHECKED,
						   FALSE);
		else
			gtk_widget_unset_state_flags(label, GTK_STATE_FLAG_CHECKED);
		gtk_widget_queue_draw(label);
	}
}

static int tab_index(ExWindow *w, GtkWidget *label)
{
	int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));

	for (int i = 0; i < n; i++) {
		GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), i);

		if (g_object_get_data(G_OBJECT(page), "ex-tab-label") == label)
			return i;
	}
	return -1;
}

/* A press on a tab shows it; dragging it sideways moves it along the
 * row. */
static gboolean tab_press(GtkWidget *label, GdkEventButton *ev, ExWindow *w)
{
	int i;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != 1)
		return FALSE;
	i = tab_index(w, label);
	if (i >= 0)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), i);
	w->press_tab = label;
	w->press_x = ev->x_root;
	w->press_y = ev->y_root;
	w->tab_dragging = FALSE;
	return TRUE;
}

static gboolean tab_motion(GtkWidget *label, GdkEventMotion *ev, ExWindow *w)
{
	GtkAllocation a;
	int x, y, n, from, to;

	if (w->press_tab != label || !(ev->state & GDK_BUTTON1_MASK))
		return FALSE;
	if (!w->tab_dragging &&
	    !gtk_drag_check_threshold(label, (int)w->press_x, (int)w->press_y,
				      (int)ev->x_root, (int)ev->y_root))
		return TRUE;
	w->tab_dragging = TRUE;
	/* Find the tab under the pointer and take its place. */
	gtk_widget_translate_coordinates(label, w->tabs, (int)ev->x, (int)ev->y,
					 &x, &y);
	from = tab_index(w, label);
	n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));
	to = from;
	for (int i = 0; i < n; i++) {
		GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), i);
		GtkWidget *other = g_object_get_data(G_OBJECT(page), "ex-tab-label");

		if (!other)
			continue;
		gtk_widget_get_allocation(other, &a);
		gtk_widget_translate_coordinates(other, w->tabs, 0, 0, &a.x, &a.y);
		if (x >= a.x && x < a.x + a.width)
			to = i;
	}
	if (from >= 0 && to != from) {
		gtk_notebook_reorder_child(
			GTK_NOTEBOOK(w->nb),
			gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), from), to);
		sync_tabs(w);
	}
	return TRUE;
}

static gboolean tab_release(GtkWidget *label, GdkEventButton *ev, ExWindow *w)
{
	if (ev->button == 1)
		w->press_tab = NULL;
	return FALSE;
}

/* A closing window takes the title row, and the tabs with it, down before
 * the notebook's pages: the page then forgets its tab, which page_removed()
 * would otherwise destroy a second time. */
static void tab_label_destroyed(GtkWidget *label, GtkWidget *page)
{
	if (g_object_get_data(G_OBJECT(page), "ex-tab-label") == label)
		g_object_set_data(G_OBJECT(page), "ex-tab-label", NULL);
}

ExTab *ex_window_add_tab(ExWindow *w, ExLocation *loc, gboolean focus)
{
	ExTab *t = ex_tab_new(w, loc);
	GtkWidget *page = ex_tab_widget(t);
	GtkWidget *label = ex_tab_label(t);
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->nb));
	int n;

	/* The notebook only switches pages; the tabs live in the title row. */
	g_object_set_data(G_OBJECT(page), "ex-tab-label", label);
	g_signal_connect_object(label, "destroy", G_CALLBACK(tab_label_destroyed),
				page, 0);
	gtk_widget_add_events(label, GDK_BUTTON_PRESS_MASK |
					     GDK_BUTTON_RELEASE_MASK |
					     GDK_BUTTON_MOTION_MASK);
	g_signal_connect_after(label, "button-press-event", G_CALLBACK(tab_press),
			       w);
	g_signal_connect(label, "motion-notify-event", G_CALLBACK(tab_motion), w);
	g_signal_connect(label, "button-release-event", G_CALLBACK(tab_release),
			 w);
	gtk_box_pack_start(GTK_BOX(w->tabs), label, FALSE, FALSE, 0);
	n = gtk_notebook_insert_page(GTK_NOTEBOOK(w->nb), page, NULL, cur + 1);
	sync_tabs(w);
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
	/* The current page is not yet the new one while this runs. */
	title = ex_tab_title(t);
	wt = g_strdup_printf(EX_WINDOW_TITLE_FMT, title);
	gtk_window_set_title(GTK_WINDOW(w), wt);
	g_free(wt);
	g_free(title);
	ex_tab_focus_view(t);
	/* After the switch is complete: the actions ask for the new tab. */
	g_idle_add_full(G_PRIORITY_HIGH_IDLE, update_later, g_object_ref(w),
			g_object_unref);
}

static void switched_after(GtkNotebook *nb, GtkWidget *page, guint n,
			   ExWindow *w)
{
	sync_tabs(w);
}

static void page_removed(GtkNotebook *nb, GtkWidget *page, guint n, ExWindow *w)
{
	GtkWidget *label = g_object_get_data(G_OBJECT(page), "ex-tab-label");

	g_object_set_data(G_OBJECT(page), "ex-tab-label", NULL);
	if (label) {
		if (w->press_tab == label)
			w->press_tab = NULL;
		gtk_widget_destroy(label);
	}
	if (!w->closing && gtk_notebook_get_n_pages(nb) == 0)
		gtk_window_close(GTK_WINDOW(w));
	else
		sync_tabs(w);
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

/* Gallery watches no folder: what an operation changed there (deleted,
 * moved away) is read again once it ends. */
static void op_finished_gallery(ExOperation *op, ExWindow *w)
{
	ExTab *t = ex_window_current_tab(w);

	if (t && ex_location_kind(ex_tab_location(t)) == EX_LOC_GALLERY)
		ex_tab_refresh(t);
}

void ex_window_run(ExWindow *w, ExOperation *op, gboolean undoable,
		   gboolean select_results)
{
	g_signal_connect_object(op, "finished", G_CALLBACK(op_finished_gallery),
				w, 0);
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

	gtk_window_set_title(GTK_WINDOW(d), EX_APP_NAME);
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

/* A program starts in its own folder.  A console program
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

/* Ctrl+Tab and Ctrl+Page Down go to the next tab, with Shift or Page Up to
 * the one before, round the ends; Ctrl+1 to Ctrl+8 pick a tab by position
 * and Ctrl+9 the last one. */
static gboolean tab_key(ExWindow *w, GdkEventKey *ev)
{
	GtkNotebook *nb = GTK_NOTEBOOK(w->nb);
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	int n = gtk_notebook_get_n_pages(nb);
	int cur = gtk_notebook_get_current_page(nb);
	int to = -1;

	if (!(mods & GDK_CONTROL_MASK) || (mods & (GDK_MOD1_MASK | GDK_SUPER_MASK)))
		return FALSE;
	switch (ev->keyval) {
	case GDK_KEY_Tab:
	case GDK_KEY_KP_Tab:
	case GDK_KEY_ISO_Left_Tab:
		to = (mods & GDK_SHIFT_MASK) || ev->keyval == GDK_KEY_ISO_Left_Tab ?
			     cur - 1 :
			     cur + 1;
		break;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		if (mods & GDK_SHIFT_MASK)
			return FALSE;
		to = cur + 1;
		break;
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		if (mods & GDK_SHIFT_MASK)
			return FALSE;
		to = cur - 1;
		break;
	default:
		if (mods & GDK_SHIFT_MASK)
			return FALSE;
		if (ev->keyval >= GDK_KEY_1 && ev->keyval <= GDK_KEY_8)
			to = MIN((int)(ev->keyval - GDK_KEY_1), n - 1);
		else if (ev->keyval == GDK_KEY_9)
			to = n - 1;
		else
			return FALSE;
		break;
	}
	if (n < 1)
		return FALSE;
	gtk_notebook_set_current_page(nb, (to + n) % n);
	return TRUE;
}

/* Ctrl+Shift+1 to 8 pick the layouts in the View menu's order.  Taken by
 * the key's place rather than its symbol: with Shift held the key gives
 * "!", "@" and so on (which symbols depends on the keyboard layout), so the
 * accelerators alone never match. */
static gboolean layout_key(ExWindow *w, GdkEventKey *ev)
{
	static const char *const modes[] = {
		"extra-large-icons", "large-icons", "icons", "small-icons",
		"list",		     "details",	    "tiles", "compact",
	};
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	GdkKeymap *km = gdk_keymap_get_for_display(
		gtk_widget_get_display(GTK_WIDGET(w)));
	guint base = 0;

	if (mods != (GDK_CONTROL_MASK | GDK_SHIFT_MASK) ||
	    !gdk_keymap_translate_keyboard_state(km, ev->hardware_keycode, 0,
						 ev->group, &base, NULL, NULL,
						 NULL))
		return FALSE;
	if (base >= GDK_KEY_KP_1 && base <= GDK_KEY_KP_8)
		base = GDK_KEY_1 + (base - GDK_KEY_KP_1);
	if (base < GDK_KEY_1 || base > GDK_KEY_8)
		return FALSE;
	if (!g_action_group_get_action_enabled(G_ACTION_GROUP(w), "view-mode"))
		return TRUE;
	g_action_group_activate_action(G_ACTION_GROUP(w), "view-mode",
				       g_variant_new_string(modes[base - GDK_KEY_1]));
	return TRUE;
}

static gboolean ex_window_key_press(GtkWidget *wid, GdkEventKey *ev)
{
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(wid));

	if (tab_key(EX_WINDOW(wid), ev) || layout_key(EX_WINDOW(wid), ev))
		return TRUE;

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

/* ---- the title row -------------------------------------------------------- */

static void toggle_maximized(ExWindow *w)
{
	if (gtk_window_is_maximized(GTK_WINDOW(w)))
		gtk_window_unmaximize(GTK_WINDOW(w));
	else
		gtk_window_maximize(GTK_WINDOW(w));
}

static void minimize_clicked(GtkButton *b, ExWindow *w)
{
	gtk_window_iconify(GTK_WINDOW(w));
}

static void maximize_clicked(GtkButton *b, ExWindow *w)
{
	toggle_maximized(w);
}

static void close_clicked(GtkButton *b, ExWindow *w)
{
	gtk_window_close(GTK_WINDOW(w));
}

static void menu_restore(GtkMenuItem *mi, ExWindow *w)
{
	gtk_window_unmaximize(GTK_WINDOW(w));
}

static void menu_minimize(GtkMenuItem *mi, ExWindow *w)
{
	gtk_window_iconify(GTK_WINDOW(w));
}

static void menu_maximize(GtkMenuItem *mi, ExWindow *w)
{
	gtk_window_maximize(GTK_WINDOW(w));
}

static void menu_close(GtkMenuItem *mi, ExWindow *w)
{
	gtk_window_close(GTK_WINDOW(w));
}

static void window_menu_item(GtkWidget *menu, const char *label,
			     const char *accel, gboolean sensitive, GCallback cb,
			     ExWindow *w)
{
	GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);

	if (accel) {
		GtkWidget *al = gtk_bin_get_child(GTK_BIN(mi));
		guint key;
		GdkModifierType mods;

		gtk_accelerator_parse(accel, &key, &mods);
		gtk_accel_label_set_accel(GTK_ACCEL_LABEL(al), key, mods);
	}
	gtk_widget_set_sensitive(mi, sensitive);
	g_signal_connect(mi, "activate", cb, w);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* The window menu a right click on the title row opens, as on the title
 * bar of any window. */
static void window_menu(ExWindow *w, GdkEventButton *ev)
{
	GtkWidget *menu = gtk_menu_new();
	gboolean max = gtk_window_is_maximized(GTK_WINDOW(w));

	window_menu_item(menu, "_Restore", NULL, max, G_CALLBACK(menu_restore), w);
	window_menu_item(menu, "Mi_nimize", NULL, TRUE, G_CALLBACK(menu_minimize),
			 w);
	window_menu_item(menu, "Ma_ximize", NULL, !max,
			 G_CALLBACK(menu_maximize), w);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			      gtk_separator_menu_item_new());
	window_menu_item(menu, "_Close", "<Alt>F4", TRUE, G_CALLBACK(menu_close),
			 w);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), w->titlebar, NULL);
	g_signal_connect(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* The empty part of the title row is the window's handle: dragging it moves
 * the window (once the pointer has moved far enough to be a drag, so that a
 * double click still reaches here), a double click maximises or restores. */
static gboolean title_press(GtkWidget *wid, GdkEventButton *ev, ExWindow *w)
{
	if (ev->button == 1 && ev->type == GDK_2BUTTON_PRESS) {
		w->press_pending = FALSE;
		toggle_maximized(w);
		return TRUE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (ev->button == 3) {
		window_menu(w, ev);
		return TRUE;
	}
	if (ev->button == 1) {
		w->press_pending = TRUE;
		w->press_x = ev->x_root;
		w->press_y = ev->y_root;
		return TRUE;
	}
	return FALSE;
}

static gboolean title_motion(GtkWidget *wid, GdkEventMotion *ev, ExWindow *w)
{
	if (!w->press_pending || !(ev->state & GDK_BUTTON1_MASK))
		return FALSE;
	if (!gtk_drag_check_threshold(wid, (int)w->press_x, (int)w->press_y,
				      (int)ev->x_root, (int)ev->y_root))
		return TRUE;
	w->press_pending = FALSE;
	gtk_window_begin_move_drag(GTK_WINDOW(w), 1, (int)w->press_x,
				   (int)w->press_y, ev->time);
	return TRUE;
}

static gboolean title_release(GtkWidget *wid, GdkEventButton *ev, ExWindow *w)
{
	if (ev->button == 1)
		w->press_pending = FALSE;
	return FALSE;
}

/* The caption buttons: inset rounded buttons 36 by 26 pixels, 4 apart and
 * 6 from the right edge, centred in the title row -- the window manager's
 * title bars have the same ones (sizes and colours in the theme). */
static GtkWidget *caption_button(ExGlyph glyph, const char *tip,
				 const char *klass, GCallback cb, ExWindow *w,
				 GtkWidget **glyph_out)
{
	GtkWidget *g = ex_style_glyph(glyph, 10);
	GtkWidget *b = ex_style_icon_button(g, tip, NULL);

	ex_style_add(b, EX_CLASS_CAPTION);
	ex_style_add(b, klass);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_signal_connect(b, "clicked", cb, w);
	if (glyph_out)
		*glyph_out = g;
	return b;
}

static GtkWidget *build_titlebar(ExWindow *w)
{
	GtkWidget *eb = gtk_event_box_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *grip = gtk_label_new(NULL);

	w->titlebar = eb;
	ex_style_add(row, EX_CLASS_TITLEBAR);
	gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
					  GDK_BUTTON_MOTION_MASK);
	g_signal_connect(eb, "button-press-event", G_CALLBACK(title_press), w);
	g_signal_connect(eb, "motion-notify-event", G_CALLBACK(title_motion), w);
	g_signal_connect(eb, "button-release-event", G_CALLBACK(title_release), w);

	w->tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	ex_style_add(w->tabs, "ex-tabs");
	atk_object_set_role(gtk_widget_get_accessible(w->tabs),
			    ATK_ROLE_PAGE_TAB_LIST);
	atk_object_set_name(gtk_widget_get_accessible(w->tabs), "Tabs");
	gtk_box_pack_start(GTK_BOX(row), w->tabs, FALSE, TRUE, 0);

	w->plus = ex_style_icon_button(ex_style_glyph(EX_GLYPH_PLUS, 10),
				       "New tab (Ctrl+T)", "win.new-tab");
	ex_style_add(w->plus, "ex-new-tab");
	gtk_widget_set_valign(w->plus, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), w->plus, FALSE, FALSE, 0);

	/* Room left to grab the window by, however many tabs are open. */
	gtk_widget_set_size_request(grip, 48, -1);
	gtk_box_pack_start(GTK_BOX(row), grip, TRUE, TRUE, 0);

	gtk_box_pack_end(GTK_BOX(row),
			 caption_button(EX_GLYPH_CLOSE, "Close", "close",
					G_CALLBACK(close_clicked), w, NULL),
			 FALSE, FALSE, 0);
	w->max_btn = caption_button(EX_GLYPH_MAXIMIZE, "Maximize", "maximize",
				    G_CALLBACK(maximize_clicked), w,
				    &w->max_glyph);
	gtk_box_pack_end(GTK_BOX(row), w->max_btn, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row),
			 caption_button(EX_GLYPH_MINIMIZE, "Minimize", "minimize",
					G_CALLBACK(minimize_clicked), w, NULL),
			 FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(eb), row);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	gtk_widget_show_all(eb);
	return eb;
}

/* Maximised, the middle caption button restores, and shows so. */
static gboolean window_state(GtkWidget *wid, GdkEventWindowState *ev)
{
	ExWindow *w = EX_WINDOW(wid);

	if (ev->changed_mask & (GDK_WINDOW_STATE_MAXIMIZED |
				GDK_WINDOW_STATE_FULLSCREEN)) {
		gboolean max = (ev->new_window_state &
				(GDK_WINDOW_STATE_MAXIMIZED |
				 GDK_WINDOW_STATE_FULLSCREEN)) != 0;
		const char *tip = max ? "Restore Down" : "Maximize";

		ex_style_set_glyph(w->max_glyph,
				   max ? EX_GLYPH_RESTORE : EX_GLYPH_MAXIMIZE);
		gtk_widget_set_tooltip_text(w->max_btn, tip);
		atk_object_set_name(gtk_widget_get_accessible(w->max_btn), tip);
	}
	if (GTK_WIDGET_CLASS(ex_window_parent_class)->window_state_event)
		return GTK_WIDGET_CLASS(ex_window_parent_class)
			->window_state_event(wid, ev);
	return FALSE;
}

/* The window draws its own title row, so the window manager is asked for
 * the border alone: it keeps the frame, its corners and its resize edges,
 * and draws no title bar of its own (the Motif decoration hint). */
static void ex_window_realize(GtkWidget *wid)
{
	GTK_WIDGET_CLASS(ex_window_parent_class)->realize(wid);
	gdk_window_set_decorations(gtk_widget_get_window(wid), GDK_DECOR_BORDER);
}

static void ex_window_class_init(ExWindowClass *k)
{
	GTK_WIDGET_CLASS(k)->realize = ex_window_realize;
	GTK_WIDGET_CLASS(k)->window_state_event = window_state;
	G_OBJECT_CLASS(k)->dispose = ex_window_dispose;
	GTK_WIDGET_CLASS(k)->key_press_event = ex_window_key_press;
	GTK_WIDGET_CLASS(k)->delete_event = delete_event;
}

static void ex_window_init(ExWindow *w)
{
	ExSettings *s = ex_settings_get();
	GtkWidget *box;
	int width, height;

	ex_style_add(GTK_WIDGET(w), EX_CLASS_WINDOW);
	gtk_window_set_icon_name(GTK_WINDOW(w), "system-file-manager");
	ex_settings_get_pair(s, "window-size", &width, &height);
	gtk_window_set_default_size(GTK_WINDOW(w), MAX(width, 640),
				    MAX(height, 420));
	if (ex_settings_get_bool(s, "window-maximized"))
		gtk_window_maximize(GTK_WINDOW(w));

	/* The smallest window in which the bars stay whole. */
	gtk_widget_set_size_request(GTK_WIDGET(w), 500, 360);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(box), build_titlebar(w), FALSE, FALSE, 0);
	w->nb = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->nb), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(w->nb), FALSE);
	g_signal_connect(w->nb, "switch-page", G_CALLBACK(switched), w);
	g_signal_connect_after(w->nb, "switch-page", G_CALLBACK(switched_after), w);
	g_signal_connect(w->nb, "page-removed", G_CALLBACK(page_removed), w);
	g_signal_connect_swapped(w->nb, "page-reordered", G_CALLBACK(sync_tabs), w);
	gtk_box_pack_start(GTK_BOX(box), w->nb, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(w), box);
	gtk_widget_show_all(box);

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
