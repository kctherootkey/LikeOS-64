// LikeOS Explorer: the window's commands.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-actions.h"

#include <gio/gdesktopappinfo.h>
#include "ui/ex-tab.h"
#include "ui/ex-style.h"
#include "ui/ex-properties.h"
#include "ui/ex-share.h"
#include "ui/ex-operation-center.h"
#include "backend/ex-apps.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-image.h"
#include "backend/ex-recent.h"
#include "backend/ex-trash.h"
#include "backend/ex-volumes.h"
#include "backend/ex-index.h"
#include "services/ex-clipboard.h"
#include "services/ex-settings.h"
#include "services/ex-undo.h"

#include <string.h>

/* ---- helpers --------------------------------------------------------------- */

static ExTab *tab(gpointer w)
{
	return ex_window_current_tab(EX_WINDOW(w));
}

static GPtrArray *sel_files(ExTab *t)
{
	return ex_selection_files(ex_tab_selection(t));
}

static gboolean confirm(ExWindow *w, const char *primary, const char *secondary,
			const char *yes)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w), GTK_DIALOG_MODAL,
					      GTK_MESSAGE_QUESTION,
					      GTK_BUTTONS_NONE, "%s", primary);
	char *title = g_strdup(yes), *o = title;
	int r;

	/* Named after what it asks to do ("Empty Trash"): the button's
	 * label without its mnemonic mark. */
	for (const char *i = yes; *i; i++)
		if (*i != '_')
			*o++ = *i;
	*o = '\0';
	gtk_window_set_title(GTK_WINDOW(d), title);
	g_free(title);
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, yes,
			       GTK_RESPONSE_ACCEPT, NULL);
	ex_style_fixed_dialog(d);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_ACCEPT;
}

static char *count_label(guint n, ExSelection *s)
{
	ExFile *one = ex_selection_single(s);

	if (one)
		return g_strdup_printf("“%s”", ex_file_get_name(one));
	return g_strdup_printf("these %u items", n);
}

/* ---- navigation ------------------------------------------------------------ */

static void a_back(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_navigator_back(ex_tab_navigator(tab(w)));
}

static void a_forward(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_navigator_forward(ex_tab_navigator(tab(w)));
}

static void a_up(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_navigator_up(ex_tab_navigator(tab(w)));
}

static void a_refresh(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_refresh(tab(w));
}

static void a_home(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_new_special(EX_LOC_HOME);

	ex_tab_go(tab(w), l);
	ex_location_unref(l);
}

static void a_address(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_focus_address(tab(w));
}

static void a_search(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_focus_search(tab(w));
}

static void a_new_tab(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_new_special(EX_LOC_HOME);

	ex_window_add_tab(EX_WINDOW(w), l, TRUE);
	ex_location_unref(l);
}

static void a_close_tab(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_window_close_tab(EX_WINDOW(w), tab(w));
}

static void a_new_window(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_ref(ex_tab_location(tab(w)));
	ExWindow *nw = ex_window_new(gtk_window_get_application(GTK_WINDOW(w)), l);

	gtk_window_present(GTK_WINDOW(nw));
	ex_location_unref(l);
}

static void a_open_location(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));

	if (l) {
		ex_tab_go(tab(w), l);
		ex_location_unref(l);
	}
}

static void a_open_location_tab(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));

	if (l) {
		ex_window_add_tab(EX_WINDOW(w), l, FALSE);
		ex_location_unref(l);
	}
}

/* ---- opening ------------------------------------------------------------------ */

static void a_open(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *items = ex_selection_get(ex_tab_selection(tab(w)));

	if (items->len)
		ex_window_open(EX_WINDOW(w), items, FALSE);
}

static void a_open_new_tab(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *items = ex_selection_get(ex_tab_selection(tab(w)));

	ex_window_open(EX_WINDOW(w), items, TRUE);
}

static void a_open_with(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *id = g_variant_get_string(p, NULL);
	GDesktopAppInfo *app = g_desktop_app_info_new(id);
	GPtrArray *files = sel_files(tab(w));

	if (app) {
		ex_window_open_with(EX_WINDOW(w), files, G_APP_INFO(app));
		g_object_unref(app);
	}
	g_ptr_array_unref(files);
}

static void chooser_response(GtkDialog *d, int r, gpointer data)
{
	ExWindow *w = EX_WINDOW(gtk_window_get_transient_for(GTK_WINDOW(d)));
	GPtrArray *files = g_object_get_data(G_OBJECT(d), "ex-files");

	if (r == GTK_RESPONSE_OK) {
		GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));

		if (app) {
			ex_window_open_with(w, files, app);
			g_object_unref(app);
		}
	}
	gtk_widget_destroy(GTK_WIDGET(d));
}

static void a_open_with_other(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	ExFile *f = ex_selection_get(ex_tab_selection(t))->len ?
			    g_ptr_array_index(ex_selection_get(ex_tab_selection(t)), 0) :
			    NULL;
	GtkWidget *d;

	if (!f)
		return;
	d = gtk_app_chooser_dialog_new(GTK_WINDOW(w), GTK_DIALOG_MODAL,
				       ex_file_get_file(f));
	g_object_set_data_full(G_OBJECT(d), "ex-files", sel_files(t),
			       (GDestroyNotify)g_ptr_array_unref);
	g_signal_connect(d, "response", G_CALLBACK(chooser_response), NULL);
	gtk_widget_show(d);
}

static void a_open_terminal(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *dir = ex_tab_folder(tab(w));
	char *path = dir ? g_file_get_path(dir) : NULL;
	GList *all = g_app_info_get_all();
	GAppInfo *term = NULL;

	/* Whichever terminal is installed, by its desktop entry's category --
	 * no program named here. */
	for (GList *l = all; l && !term; l = l->next) {
		const char *cats = G_IS_DESKTOP_APP_INFO(l->data) ?
					   g_desktop_app_info_get_categories(
						   l->data) :
					   NULL;

		if (cats && strstr(cats, "TerminalEmulator") &&
		    g_app_info_should_show(l->data))
			term = g_object_ref(l->data);
	}
	g_list_free_full(all, g_object_unref);
	if (term && path) {
		char *argv[] = { (char *)g_app_info_get_executable(term), NULL };
		GError *e = NULL;

		if (!g_spawn_async(path, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				   NULL, NULL, &e)) {
			ex_window_show_error(EX_WINDOW(w),
					     "The terminal could not be started",
					     e->message);
			g_error_free(e);
		}
	} else if (!term) {
		ex_window_show_error(EX_WINDOW(w), "No terminal is installed", NULL);
	}
	g_clear_object(&term);
	g_free(path);
}

/* ---- clipboard ---------------------------------------------------------------- */

static void a_cut(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *f = sel_files(tab(w));

	ex_clipboard_cut(ex_clipboard_get(), f);
	g_ptr_array_unref(f);
	gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void a_copy(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *f = sel_files(tab(w));

	ex_clipboard_copy(ex_clipboard_get(), f);
	g_ptr_array_unref(f);
}

static void a_copy_path(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *f = sel_files(tab(w));

	if (!f->len) {
		GFile *dir = ex_tab_folder(tab(w));

		if (dir)
			g_ptr_array_add(f, g_object_ref(dir));
	}
	ex_clipboard_copy_paths(ex_clipboard_get(), f);
	g_ptr_array_unref(f);
}

typedef struct {
	ExWindow *w;
	GFile *dest;
} PasteReq;

static void pasted(GPtrArray *files, gboolean cut, gpointer data)
{
	PasteReq *r = data;

	if (files && files->len) {
		ExOperation *op = cut ? ex_operation_new_move(files, r->dest) :
					ex_operation_new_copy(files, r->dest);

		ex_window_run(r->w, op, TRUE, TRUE);
		g_object_unref(op);
		if (cut)
			ex_clipboard_clear_cut(ex_clipboard_get());
	}
	g_object_unref(r->w);
	g_object_unref(r->dest);
	g_free(r);
}

static void a_paste(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	GFile *dest = ex_tab_folder(t);
	ExFile *one = ex_selection_single(ex_tab_selection(t));
	PasteReq *r;

	/* A single selected folder is where a paste goes (the item menu's
	 * "Paste into folder"). */
	if (p && g_variant_get_boolean(p) && one && ex_file_is_dir(one))
		dest = ex_file_get_file(one);
	if (!dest)
		return;
	r = g_new0(PasteReq, 1);
	r->w = g_object_ref(w);
	r->dest = g_object_ref(dest);
	ex_clipboard_request(ex_clipboard_get(), pasted, r);
}

/* ---- changing things ------------------------------------------------------------ */

static void a_rename(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_start_rename(tab(w));
}

static void a_trash(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	ExSelection *s = ex_tab_selection(t);
	GPtrArray *f;
	ExOperation *op;

	if (!ex_selection_count(s))
		return;
	/* In the Trash, Delete is for good. */
	if (ex_location_kind(ex_tab_location(t)) == EX_LOC_TRASH) {
		g_action_group_activate_action(G_ACTION_GROUP(w), "delete", NULL);
		return;
	}
	if (ex_settings_get_bool(ex_settings_get(), "confirm-trash")) {
		char *what = count_label(ex_selection_count(s), s);
		char *q = g_strdup_printf("Move %s to the Trash?", what);
		gboolean yes = confirm(EX_WINDOW(w), q, NULL, "_Move to Trash");

		g_free(q);
		g_free(what);
		if (!yes)
			return;
	}
	f = sel_files(t);
	op = ex_operation_new_trash(f);
	ex_tab_note_removal(t);
	ex_window_run(EX_WINDOW(w), op, TRUE, FALSE);
	g_object_unref(op);
	g_ptr_array_unref(f);
}

static void a_delete(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	ExSelection *s = ex_tab_selection(t);
	char *what, *q;
	gboolean yes;

	if (!ex_selection_count(s))
		return;
	what = count_label(ex_selection_count(s), s);
	q = g_strdup_printf("Permanently delete %s?", what);
	yes = confirm(EX_WINDOW(w), q,
		      "This cannot be undone: the items do not go to the Trash.",
		      "_Delete");
	g_free(q);
	g_free(what);
	if (yes) {
		GPtrArray *f = sel_files(t);
		ExOperation *op = ex_operation_new_delete(f);

		ex_tab_note_removal(t);
		ex_window_run(EX_WINDOW(w), op, FALSE, FALSE);
		g_object_unref(op);
		g_ptr_array_unref(f);
	}
}

static void created(ExOperation *op, gpointer data)
{
	ExWindow *w = data;
	GPtrArray *res = ex_operation_get_results(op);
	ExTab *t = ex_window_current_tab(w);

	if (t && res->len && ex_operation_get_state(op) == EX_OP_DONE)
		ex_tab_rename_when_shown(t,
					 ((ExOpResult *)g_ptr_array_index(res, 0))->dest);
}

static void create_and_rename(ExWindow *w, ExOperation *op)
{
	g_signal_connect_object(op, "finished", G_CALLBACK(created), w, 0);
	ex_window_run(w, op, TRUE, TRUE);
	g_object_unref(op);
}

static void a_new_folder(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *dir = ex_tab_folder(tab(w));

	if (dir && ex_tab_folder_writable(tab(w)))
		create_and_rename(EX_WINDOW(w), ex_operation_new_mkdir(dir, NULL));
}

static void a_new_file(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *dir = ex_tab_folder(tab(w));
	const char *kind = g_variant_get_string(p, NULL);
	ExOperation *op;

	if (!dir || !ex_tab_folder_writable(tab(w)))
		return;
	if (!strcmp(kind, "text")) {
		op = ex_operation_new_newfile(dir, "New Text Document.txt", NULL);
	} else if (!strcmp(kind, "empty")) {
		op = ex_operation_new_newfile(dir, "New file", NULL);
	} else {
		/* A template from the Templates folder. */
		GFile *tpl = g_file_new_for_path(kind);
		char *name = g_file_get_basename(tpl);

		op = ex_operation_new_newfile(dir, name, tpl);
		g_free(name);
		g_object_unref(tpl);
	}
	create_and_rename(EX_WINDOW(w), op);
}

static void a_shortcut(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *dir = ex_tab_folder(tab(w));
	GPtrArray *f = sel_files(tab(w));

	if (dir && f->len) {
		ExOperation *op = ex_operation_new_link(f, dir);

		ex_window_run(EX_WINDOW(w), op, TRUE, TRUE);
		g_object_unref(op);
	}
	g_ptr_array_unref(f);
}

static void a_properties(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	GPtrArray *items = ex_selection_get(ex_tab_selection(t));

	if (items->len) {
		ex_properties_show(EX_WINDOW(w), items);
		return;
	}
	GFile *dir = ex_tab_folder(t);
	if (!dir)
		dir = ex_location_file(ex_tab_location(t));
	if (dir) {
		ExFile *f = ex_file_new_query(dir, NULL, NULL);

		if (f) {
			GPtrArray *one = g_ptr_array_new_with_free_func(g_object_unref);

			g_ptr_array_add(one, f);
			ex_properties_show(EX_WINDOW(w), one);
			g_ptr_array_unref(one);
		}
	}
}

static void a_location_properties(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));
	GFile *f = l ? ex_location_file(l) : NULL;
	ExFile *ef = f ? ex_file_new_query(f, NULL, NULL) : NULL;

	if (ef) {
		GPtrArray *one = g_ptr_array_new_with_free_func(g_object_unref);

		g_ptr_array_add(one, ef);
		ex_properties_show(EX_WINDOW(w), one);
		g_ptr_array_unref(one);
	}
	if (l)
		ex_location_unref(l);
}

static void a_pin(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	ExBookmarks *b = ex_bookmarks_get_default();
	GPtrArray *items = ex_selection_get(ex_tab_selection(t));
	gboolean any = FALSE;

	for (guint i = 0; i < items->len; i++) {
		ExFile *f = g_ptr_array_index(items, i);

		if (!ex_file_is_dir(f))
			continue;
		any = TRUE;
		if (ex_bookmarks_contains(b, ex_file_get_file(f)))
			ex_bookmarks_remove(b, ex_file_get_file(f));
		else
			ex_bookmarks_add(b, ex_file_get_file(f), NULL);
	}
	if (!any && ex_tab_folder(t)) {
		GFile *dir = ex_tab_folder(t);

		if (ex_bookmarks_contains(b, dir))
			ex_bookmarks_remove(b, dir);
		else
			ex_bookmarks_add(b, dir, NULL);
	}
}

static void a_pin_location(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));

	if (l && ex_location_file(l))
		ex_bookmarks_add(ex_bookmarks_get_default(), ex_location_file(l),
				 NULL);
	if (l)
		ex_location_unref(l);
}

static void a_unpin(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));

	if (l && ex_location_file(l))
		ex_bookmarks_remove(ex_bookmarks_get_default(), ex_location_file(l));
	if (l)
		ex_location_unref(l);
}

static void a_favorite(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *items = ex_selection_get(ex_tab_selection(tab(w)));
	ExSettings *s = ex_settings_get();

	for (guint i = 0; i < items->len; i++) {
		GFile *f = ex_file_get_file(g_ptr_array_index(items, i));

		ex_settings_set_favorite(s, f, !ex_settings_is_favorite(s, f));
	}
}

static void a_share(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *f = sel_files(tab(w));

	if (f->len)
		ex_share_show(EX_WINDOW(w), f);
	g_ptr_array_unref(f);
}

/* ---- selection ----------------------------------------------------------------- */

static void a_select_all(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_select_all(tab(w));
}

static void a_select_none(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_select_none(tab(w));
}

static void a_invert(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_invert_selection(tab(w));
}

/* ---- undo ------------------------------------------------------------------------- */

static void a_undo(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExOperation *op = ex_undo_undo(ex_undo_get());

	if (op)
		ex_window_run(EX_WINDOW(w), op, FALSE, TRUE);
}

static void a_redo(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExOperation *op = ex_undo_redo(ex_undo_get());

	if (op)
		ex_window_run(EX_WINDOW(w), op, FALSE, TRUE);
}

/* ---- the Trash and volumes ------------------------------------------------------------- */

static void a_empty_trash(GSimpleAction *a, GVariant *p, gpointer w)
{
	if (confirm(EX_WINDOW(w), "Permanently delete everything in the Trash?",
		    "This cannot be undone.", "_Empty Trash")) {
		ExOperation *op = ex_operation_new_empty_trash();

		ex_window_run(EX_WINDOW(w), op, FALSE, FALSE);
		g_object_unref(op);
	}
}

static void restore(ExWindow *w, GPtrArray *items)
{
	GPtrArray *f = g_ptr_array_new_with_free_func(g_object_unref);
	ExOperation *op;

	for (guint i = 0; i < items->len; i++)
		g_ptr_array_add(f, g_object_ref(ex_file_get_file(
					   g_ptr_array_index(items, i))));
	if (f->len) {
		op = ex_operation_new_restore(f);
		ex_window_run(w, op, TRUE, FALSE);
		g_object_unref(op);
	}
	g_ptr_array_unref(f);
}

static void a_restore(GSimpleAction *a, GVariant *p, gpointer w)
{
	restore(EX_WINDOW(w), ex_selection_get(ex_tab_selection(tab(w))));
}

static void a_restore_all(GSimpleAction *a, GVariant *p, gpointer w)
{
	GPtrArray *all = ex_tab_listed(tab(w));

	restore(EX_WINDOW(w), all);
	g_ptr_array_unref(all);
}

static void ejected(GObject *src, GAsyncResult *res, gpointer w)
{
	GError *e = NULL;

	if (!ex_volume_eject_finish(res, &e)) {
		ex_window_show_error(EX_WINDOW(w), "The drive could not be ejected",
				     e->message);
		g_error_free(e);
	}
	g_object_unref(w);
}

static void a_eject(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExLocation *l = ex_location_parse(g_variant_get_string(p, NULL));
	const ExVolume *v = l && ex_location_file(l) ?
				    ex_volumes_find(ex_volumes_get_default(),
						    ex_location_file(l)) :
				    NULL;

	if (v && v->mount) {
		GMountOperation *mop = gtk_mount_operation_new(GTK_WINDOW(w));

		ex_volume_eject_async(v, mop, NULL, ejected, g_object_ref(w));
		g_object_unref(mop);
	}
	if (l)
		ex_location_unref(l);
}

/* ---- view ---------------------------------------------------------------------------- */

static void a_view_mode(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExTab *t = tab(w);
	const char *mode = g_variant_get_string(p, NULL);

	ex_tab_set_view_mode(t, mode);
	ex_settings_set_view_for(ex_settings_get(),
				 ex_location_id(ex_tab_location(t)), mode);
	g_simple_action_set_state(a, p);
}

static void a_sort_by(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *col;
	gboolean asc;

	ex_tab_get_sort(tab(w), &col, &asc);
	ex_tab_set_sort(tab(w), g_variant_get_string(p, NULL), asc);
	g_simple_action_set_state(a, p);
}

static void a_sort_order(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *col;
	gboolean asc;

	ex_tab_get_sort(tab(w), &col, &asc);
	ex_tab_set_sort(tab(w), col,
			!strcmp(g_variant_get_string(p, NULL), "ascending"));
	g_simple_action_set_state(a, p);
}

static void a_setting_toggle(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *key = g_object_get_data(G_OBJECT(a), "ex-key");
	GVariant *st = g_action_get_state(G_ACTION(a));
	gboolean v = !g_variant_get_boolean(st);

	g_variant_unref(st);
	ex_settings_set_bool(ex_settings_get(), key, v);
	g_simple_action_set_state(a, g_variant_new_boolean(v));
}

static void a_right_pane(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *want = g_variant_get_string(p, NULL);
	char *cur = ex_settings_get_string(ex_settings_get(), "right-pane");
	/* Choosing the pane already shown closes it. */
	const char *now = !strcmp(cur, want) ? "none" : want;

	ex_settings_set_string(ex_settings_get(), "right-pane", now);
	g_simple_action_set_state(a, g_variant_new_string(now));
	g_free(cur);
}

static void a_operations(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_operation_center_present(GTK_WINDOW(w));
}

/* ---- pictures ------------------------------------------------------------------------- */

/* The desktop is PCManFM's: it is asked to show the picture, filling the
 * screen and cut to fit (its "crop" mode). */
static void a_set_wallpaper(GSimpleAction *a, GVariant *p, gpointer w)
{
	ExFile *f = ex_selection_single(ex_tab_selection(tab(w)));
	char *path = f ? g_file_get_path(ex_file_get_file(f)) : NULL;
	GError *e = NULL;

	if (!path)
		return;
	char *arg = g_strdup_printf("--set-wallpaper=%s", path);
	char *argv[] = { (char *)"pcmanfm", arg, (char *)"--wallpaper-mode=crop",
			 NULL };

	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
			   &e)) {
		ex_window_show_error(EX_WINDOW(w),
				     "The desktop background could not be set",
				     e->message);
		g_error_free(e);
	}
	g_free(arg);
	g_free(path);
}

typedef struct {
	ExWindow *w;
	guint left;
	GString *failed;
} Turn;

static void turned(GObject *src, GAsyncResult *res, gpointer data)
{
	Turn *tn = data;
	GError *e = NULL;

	if (!ex_image_rotate_finish(res, &e)) {
		char *name = g_file_get_basename(G_FILE(src));

		g_string_append_printf(tn->failed, "%s%s: %s",
				       tn->failed->len ? "\n" : "", name,
				       e->message);
		g_free(name);
		g_error_free(e);
	}
	if (--tn->left)
		return;
	if (tn->failed->len)
		ex_window_show_error(tn->w, "The picture could not be rotated",
				     tn->failed->str);
	/* A folder's monitor shows the change; Gallery is read again. */
	if (ex_location_kind(ex_tab_location(ex_window_current_tab(tn->w))) ==
	    EX_LOC_GALLERY)
		ex_tab_refresh(ex_window_current_tab(tn->w));
	g_string_free(tn->failed, TRUE);
	g_object_unref(tn->w);
	g_free(tn);
}

static void rotate(gpointer w, gboolean clockwise)
{
	GPtrArray *items = ex_selection_get(ex_tab_selection(tab(w)));
	Turn *tn = g_new0(Turn, 1);

	tn->w = g_object_ref(w);
	tn->failed = g_string_new(NULL);
	tn->left = 1; /* held until every request is out */
	for (guint i = 0; i < items->len; i++) {
		tn->left++;
		ex_image_rotate_async(ex_file_get_file(g_ptr_array_index(items, i)),
				      clockwise, NULL, turned, tn);
	}
	if (--tn->left == 0) {
		g_string_free(tn->failed, TRUE);
		g_object_unref(tn->w);
		g_free(tn);
	}
}

static void a_rotate_right(GSimpleAction *a, GVariant *p, gpointer w)
{
	rotate(w, TRUE);
}

static void a_rotate_left(GSimpleAction *a, GVariant *p, gpointer w)
{
	rotate(w, FALSE);
}

/* ---- Home and Gallery ---------------------------------------------------------------- */

static void a_home_filter(GSimpleAction *a, GVariant *p, gpointer w)
{
	ex_tab_set_home_filter(tab(w), g_variant_get_string(p, NULL));
	g_simple_action_set_state(a, p);
}

/* A file of Home's lists, named by its URI: these are not a selection of
 * the tab, so their menu's commands carry the file along. */
static void a_open_uri(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *f = g_file_new_for_uri(g_variant_get_string(p, NULL));
	ExFile *ef = ex_file_new_query(f, NULL, NULL);

	if (ef) {
		GPtrArray *one = g_ptr_array_new_with_free_func(g_object_unref);

		g_ptr_array_add(one, ef);
		ex_window_open(EX_WINDOW(w), one, FALSE);
		g_ptr_array_unref(one);
	}
	g_object_unref(f);
}

static void a_open_file_location(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *f = g_file_new_for_uri(g_variant_get_string(p, NULL));
	GFile *dir = g_file_get_parent(f);

	if (dir) {
		ExLocation *l = ex_location_new_folder(dir);
		GPtrArray *sel = g_ptr_array_new_with_free_func(g_object_unref);

		ex_tab_go(tab(w), l);
		g_ptr_array_add(sel, g_object_ref(f));
		ex_tab_select_files(tab(w), sel);
		g_ptr_array_unref(sel);
		ex_location_unref(l);
		g_object_unref(dir);
	}
	g_object_unref(f);
}

static void a_favorite_uri(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *f = g_file_new_for_uri(g_variant_get_string(p, NULL));
	ExSettings *s = ex_settings_get();

	ex_settings_set_favorite(s, f, !ex_settings_is_favorite(s, f));
	g_object_unref(f);
}

static void a_forget_recent(GSimpleAction *a, GVariant *p, gpointer w)
{
	GFile *f = g_file_new_for_uri(g_variant_get_string(p, NULL));

	(void)ex_recent_forget(f, NULL);
	g_object_unref(f);
}

/* Gallery's collection: which personal folders it shows, each a switch. */
static const struct {
	const char *action, *name;
	GUserDirectory dir;
} collection[] = {
	{ "gallery-pictures", "pictures", G_USER_DIRECTORY_PICTURES },
	{ "gallery-videos", "videos", G_USER_DIRECTORY_VIDEOS },
	{ "gallery-desktop", "desktop", G_USER_DIRECTORY_DESKTOP },
	{ "gallery-downloads", "download", G_USER_DIRECTORY_DOWNLOAD },
	{ "gallery-documents", "documents", G_USER_DIRECTORY_DOCUMENTS },
};

static gboolean in_collection(const char *name)
{
	char **v = ex_settings_get_strv(ex_settings_get(), "gallery-folders");
	gboolean in = v && g_strv_contains((const char *const *)v, name);

	g_strfreev(v);
	return in;
}

static void a_collection(GSimpleAction *a, GVariant *p, gpointer w)
{
	const char *name = g_object_get_data(G_OBJECT(a), "ex-name");
	char **v = ex_settings_get_strv(ex_settings_get(), "gallery-folders");
	GPtrArray *out = g_ptr_array_new();
	gboolean was = FALSE;

	for (int i = 0; v && v[i]; i++) {
		if (!strcmp(v[i], name))
			was = TRUE;
		else
			g_ptr_array_add(out, v[i]);
	}
	if (!was)
		g_ptr_array_add(out, (gpointer)name);
	g_ptr_array_add(out, NULL);
	ex_settings_set_strv(ex_settings_get(), "gallery-folders",
			     (const char *const *)out->pdata);
	g_simple_action_set_state(a, g_variant_new_boolean(!was));
	g_ptr_array_unref(out);
	g_strfreev(v);
}

/* ---- Explorer settings --------------------------------------------------------------- */

static void opt_toggled(GtkSwitch *sw, GParamSpec *ps, gpointer data)
{
	gboolean on = gtk_switch_get_active(sw);

	ex_settings_set_bool(ex_settings_get(), data, on);
	if (!strcmp(data, "index-home"))
		ex_index_set_enabled(ex_index_get_default(), on);
}

static void clear_history(GtkButton *b, gpointer w)
{
	GError *e = NULL;

	if (!ex_recent_clear(&e)) {
		ex_window_show_error(EX_WINDOW(w), "History could not be cleared",
				     e->message);
		g_error_free(e);
	} else {
		gtk_button_set_label(b, "Cleared");
		gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
	}
}

/* One setting: its name and a line saying what it does at the left, a
 * switch at the right; the whole row a line of its card. */
static GtkWidget *setting_row(const char *key, const char *label,
			      const char *detail)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *l = gtk_label_new_with_mnemonic(label);
	GtkWidget *d = gtk_label_new(detail);
	GtkWidget *sw = gtk_switch_new();

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_xalign(GTK_LABEL(d), 0);
	gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(d), 46);
	ex_style_add(d, EX_CLASS_DIM);
	ex_style_add(d, "ex-settings-detail");
	gtk_label_set_mnemonic_widget(GTK_LABEL(l), sw);
	gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), d, FALSE, FALSE, 0);
	gtk_switch_set_active(GTK_SWITCH(sw),
			      ex_settings_get_bool(ex_settings_get(), key));
	gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
	atk_object_set_name(gtk_widget_get_accessible(sw),
			    gtk_label_get_text(GTK_LABEL(l)));
	g_signal_connect(sw, "notify::active", G_CALLBACK(opt_toggled),
			 (gpointer)key);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), sw, FALSE, FALSE, 0);
	ex_style_add(row, "ex-settings-row");
	return row;
}

/* A titled card of settings rows. */
static GtkWidget *settings_group(GtkWidget *box, const char *title)
{
	GtkWidget *h = gtk_label_new(title);
	GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_label_set_xalign(GTK_LABEL(h), 0);
	ex_style_add(h, "ex-settings-heading");
	ex_style_add(c, "ex-props-card");
	ex_style_add(c, "ex-settings-card");
	gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
	return c;
}

static void group_add(GtkWidget *group, GtkWidget *w)
{
	/* A hairline between two rows. */
	if (g_object_get_data(G_OBJECT(group), "ex-rows")) {
		GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

		ex_style_add(sep, "ex-settings-separator");
		gtk_box_pack_start(GTK_BOX(group), sep, FALSE, FALSE, 0);
	}
	g_object_set_data(G_OBJECT(group), "ex-rows", GINT_TO_POINTER(1));
	gtk_box_pack_start(GTK_BOX(group), w, FALSE, FALSE, 0);
}

static void a_options(GSimpleAction *a, GVariant *p, gpointer w)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(
		"Explorer Settings", GTK_WINDOW(w),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
		GTK_RESPONSE_CLOSE, NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *g, *clr, *row, *col, *l, *dl;

	ex_style_add(d, "ex-properties");
	ex_style_add(box, "ex-props-page");
	g = settings_group(box, "Browsing");
	group_add(g, setting_row("single-click", "Open items with a _single click",
				 "Point to select, click once to open"));
	group_add(g, setting_row("show-hidden", "Show _hidden items",
				 "Names starting with a dot are listed too"));
	group_add(g, setting_row("show-extensions", "Show file name _extensions",
				 "\"report.pdf\" rather than \"report\""));
	group_add(g, setting_row("confirm-trash", "Ask before moving to the _Trash",
				 "A question before Delete takes effect"));

	g = settings_group(box, "Home");
	group_add(g, setting_row("show-recent", "Show _recent files",
				 "The files opened last, in any program"));
	group_add(g, setting_row("show-frequent", "Show _frequent folders",
				 "Folders used often join the pinned ones"));
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	l = gtk_label_new("Recent files history");
	dl = gtk_label_new("Empties the list every program shares");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_xalign(GTK_LABEL(dl), 0);
	ex_style_add(dl, EX_CLASS_DIM);
	ex_style_add(dl, "ex-settings-detail");
	gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), dl, FALSE, FALSE, 0);
	clr = gtk_button_new_with_mnemonic("_Clear");
	gtk_widget_set_valign(clr, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(clr, "Empties the list every program shares");
	g_signal_connect(clr, "clicked", G_CALLBACK(clear_history), w);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), clr, FALSE, FALSE, 0);
	ex_style_add(row, "ex-settings-row");
	group_add(g, row);

	g = settings_group(box, "Search");
	group_add(g, setting_row("index-home", "Keep a search _index of the home folder",
				 "Searches below the home folder answer at once"));

	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  box);
	ex_style_fixed_dialog(d);
	gtk_widget_show_all(d);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* ---- table ------------------------------------------------------------------------------ */

static const GActionEntry entries[] = {
	{ "back", a_back },
	{ "forward", a_forward },
	{ "up", a_up },
	{ "refresh", a_refresh },
	{ "go-home", a_home },
	{ "focus-address", a_address },
	{ "focus-search", a_search },
	{ "new-tab", a_new_tab },
	{ "close-tab", a_close_tab },
	{ "new-window", a_new_window },
	{ "open-location", a_open_location, "s" },
	{ "open-location-tab", a_open_location_tab, "s" },
	{ "location-properties", a_location_properties, "s" },
	{ "open", a_open },
	{ "open-new-tab", a_open_new_tab },
	{ "open-with", a_open_with, "s" },
	{ "open-with-other", a_open_with_other },
	{ "open-terminal", a_open_terminal },
	{ "cut", a_cut },
	{ "copy", a_copy },
	{ "copy-path", a_copy_path },
	{ "paste", a_paste },
	{ "paste-into", a_paste, "b" },
	{ "rename", a_rename },
	{ "trash", a_trash },
	{ "delete", a_delete },
	{ "new-folder", a_new_folder },
	{ "new-file", a_new_file, "s" },
	{ "create-shortcut", a_shortcut },
	{ "properties", a_properties },
	{ "pin", a_pin },
	{ "unpin", a_unpin, "s" },
	{ "pin-location", a_pin_location, "s" },
	{ "favorite", a_favorite },
	{ "share", a_share },
	{ "select-all", a_select_all },
	{ "select-none", a_select_none },
	{ "invert-selection", a_invert },
	{ "undo", a_undo },
	{ "redo", a_redo },
	{ "empty-trash", a_empty_trash },
	{ "restore", a_restore },
	{ "restore-all", a_restore_all },
	{ "eject", a_eject, "s" },
	{ "operations", a_operations },
	{ "options", a_options },
	{ "view-mode", a_view_mode, "s", "'details'" },
	{ "sort-by", a_sort_by, "s", "'name'" },
	{ "sort-order", a_sort_order, "s", "'ascending'" },
	{ "right-pane", a_right_pane, "s", "'none'" },
	{ "set-wallpaper", a_set_wallpaper },
	{ "rotate-right", a_rotate_right },
	{ "rotate-left", a_rotate_left },
	{ "home-filter", a_home_filter, "s", "'all'" },
	{ "open-uri", a_open_uri, "s" },
	{ "open-file-location", a_open_file_location, "s" },
	{ "favorite-uri", a_favorite_uri, "s" },
	{ "forget-recent", a_forget_recent, "s" },
};

static const struct {
	const char *action, *key;
} toggles[] = {
	{ "show-hidden", "show-hidden" },
	{ "show-extensions", "show-extensions" },
	{ "navigation-pane", "show-navigation-pane" },
};

void ex_actions_install(ExWindow *w)
{
	ExSettings *s = ex_settings_get();
	char *rp;

	g_action_map_add_action_entries(G_ACTION_MAP(w), entries,
					G_N_ELEMENTS(entries), w);
	for (gsize i = 0; i < G_N_ELEMENTS(toggles); i++) {
		GSimpleAction *a = g_simple_action_new_stateful(
			toggles[i].action, NULL,
			g_variant_new_boolean(ex_settings_get_bool(s, toggles[i].key)));

		g_object_set_data(G_OBJECT(a), "ex-key", (gpointer)toggles[i].key);
		g_signal_connect(a, "activate", G_CALLBACK(a_setting_toggle), w);
		g_action_map_add_action(G_ACTION_MAP(w), G_ACTION(a));
		g_object_unref(a);
	}
	for (gsize i = 0; i < G_N_ELEMENTS(collection); i++) {
		GSimpleAction *a = g_simple_action_new_stateful(
			collection[i].action, NULL,
			g_variant_new_boolean(in_collection(collection[i].name)));

		g_object_set_data(G_OBJECT(a), "ex-name",
				  (gpointer)collection[i].name);
		g_signal_connect(a, "activate", G_CALLBACK(a_collection), w);
		g_action_map_add_action(G_ACTION_MAP(w), G_ACTION(a));
		g_object_unref(a);
	}
	rp = ex_settings_get_string(s, "right-pane");
	g_simple_action_set_state(
		G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(w),
							   "right-pane")),
		g_variant_new_string(rp));
	g_free(rp);
}

static void enable(ExWindow *w, const char *name, gboolean on)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w), name);

	if (a)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void set_state(ExWindow *w, const char *name, GVariant *v)
{
	GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w), name);

	if (a)
		g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
	else
		g_variant_unref(g_variant_ref_sink(v));
}

void ex_actions_update(ExWindow *w)
{
	ExTab *t = ex_window_current_tab(w);
	ExSelection *s;
	ExNavigator *n;
	ExLocationKind kind;
	ExFileCaps caps;
	guint count;
	gboolean writable, folder, trash, dirs, has;
	ExUndo *u = ex_undo_get();
	ExSettings *st = ex_settings_get();

	if (!t)
		return;
	s = ex_tab_selection(t);
	n = ex_tab_navigator(t);
	kind = ex_location_kind(ex_tab_location(t));
	caps = ex_selection_caps(s);
	count = ex_selection_count(s);
	has = count > 0;
	writable = ex_tab_folder_writable(t);
	folder = ex_tab_folder(t) != NULL;
	trash = kind == EX_LOC_TRASH;
	dirs = ex_selection_all_dirs(s);

	enable(w, "back", ex_navigator_can_back(n));
	enable(w, "forward", ex_navigator_can_forward(n));
	enable(w, "up", ex_navigator_can_up(n));
	enable(w, "open", has && !trash);
	enable(w, "open-new-tab", has && dirs && !trash);
	enable(w, "open-with", has && !trash);
	enable(w, "open-with-other", has && !trash && !dirs);
	enable(w, "open-terminal", folder && g_file_is_native(ex_tab_folder(t)));
	enable(w, "cut", has && !trash && (caps & EX_CAP_DELETE));
	enable(w, "copy", has && !trash);
	enable(w, "copy-path", (has || folder) && !trash);
	enable(w, "paste", writable && ex_clipboard_can_paste(ex_clipboard_get()));
	enable(w, "paste-into", ex_clipboard_can_paste(ex_clipboard_get()) &&
					((count == 1 && dirs &&
					  (caps & EX_CAP_WRITE)) ||
					 writable));
	enable(w, "rename", count == 1 && (caps & EX_CAP_RENAME) && !trash &&
				    kind != EX_LOC_GALLERY);
	enable(w, "trash", has && (caps & EX_CAP_DELETE));
	enable(w, "delete", has && (caps & EX_CAP_DELETE));
	enable(w, "new-folder", writable);
	enable(w, "new-file", writable);
	enable(w, "create-shortcut", has && writable && kind == EX_LOC_FOLDER);
	enable(w, "properties", has || folder);
	enable(w, "pin", (has && dirs) || (!has && kind == EX_LOC_FOLDER));
	enable(w, "favorite", has && !trash);
	enable(w, "share", has && !trash);
	enable(w, "select-all", kind != EX_LOC_HOME && kind != EX_LOC_COMPUTER);
	enable(w, "select-none", has);
	enable(w, "invert-selection", kind != EX_LOC_HOME && kind != EX_LOC_COMPUTER);
	enable(w, "undo", ex_undo_can_undo(u));
	enable(w, "redo", ex_undo_can_redo(u));
	enable(w, "empty-trash", trash && !ex_trash_is_empty());
	enable(w, "restore", trash && has);
	enable(w, "restore-all", trash && !ex_trash_is_empty());
	enable(w, "view-mode", kind != EX_LOC_HOME && kind != EX_LOC_COMPUTER &&
				       kind != EX_LOC_GALLERY);
	enable(w, "sort-by", kind != EX_LOC_HOME && kind != EX_LOC_COMPUTER &&
				     kind != EX_LOC_GALLERY);
	enable(w, "sort-order", kind != EX_LOC_HOME && kind != EX_LOC_COMPUTER &&
					kind != EX_LOC_GALLERY);
	/* Pictures: one for the desktop background, any number turned. */
	{
		gboolean images = has && !trash && ex_tab_selection_is_images(t);
		gboolean turnable = images && (caps & EX_CAP_WRITE);
		GPtrArray *items = ex_selection_get(s);

		for (guint i = 0; turnable && i < items->len; i++)
			turnable = ex_image_can_rotate(ex_file_get_content_type(
				g_ptr_array_index(items, i)));
		enable(w, "set-wallpaper",
		       images && count == 1 &&
			       g_file_is_native(ex_file_get_file(
				       ex_selection_single(s))));
		enable(w, "rotate-right", turnable);
		enable(w, "rotate-left", turnable);
	}
	enable(w, "home-filter", kind == EX_LOC_HOME);
	set_state(w, "home-filter",
		  g_variant_new_string(ex_tab_home_filter(t)));
	for (gsize i = 0; i < G_N_ELEMENTS(collection); i++)
		set_state(w, collection[i].action,
			  g_variant_new_boolean(in_collection(collection[i].name)));

	const char *col;
	gboolean asc;
	ex_tab_get_sort(t, &col, &asc);
	set_state(w, "view-mode", g_variant_new_string(ex_tab_view_mode(t)));
	set_state(w, "sort-by", g_variant_new_string(col));
	set_state(w, "sort-order",
		  g_variant_new_string(asc ? "ascending" : "descending"));
	for (gsize i = 0; i < G_N_ELEMENTS(toggles); i++)
		set_state(w, toggles[i].action,
			  g_variant_new_boolean(
				  ex_settings_get_bool(st, toggles[i].key)));
}

void ex_actions_set_accels(GtkApplication *app)
{
	static const struct {
		const char *action;
		const char *keys[4];
	} accels[] = {
		{ "win.back", { "<Alt>Left", "BackSpace", "Back", NULL } },
		{ "win.forward", { "<Alt>Right", "Forward", NULL } },
		{ "win.up", { "<Alt>Up", NULL } },
		{ "win.refresh", { "F5", "<Control>r", NULL } },
		{ "win.focus-address", { "<Control>l", "<Alt>d", "F4", NULL } },
		{ "win.focus-search", { "<Control>f", "F3", "<Control>e", NULL } },
		{ "win.new-tab", { "<Control>t", NULL } },
		{ "win.close-tab", { "<Control>w", NULL } },
		{ "win.new-window", { "<Control>n", NULL } },
		{ "win.cut", { "<Control>x", NULL } },
		{ "win.copy", { "<Control>c", NULL } },
		{ "win.copy-path", { "<Control><Shift>c", NULL } },
		{ "win.paste", { "<Control>v", NULL } },
		{ "win.rename", { "F2", NULL } },
		{ "win.trash", { "Delete", "<Control>d", NULL } },
		{ "win.delete", { "<Shift>Delete", NULL } },
		{ "win.new-folder", { "<Control><Shift>n", NULL } },
		{ "win.properties", { "<Alt>Return", NULL } },
		{ "win.select-all", { "<Control>a", NULL } },
		{ "win.undo", { "<Control>z", NULL } },
		{ "win.redo", { "<Control>y", "<Control><Shift>z", NULL } },
		{ "win.show-hidden", { "<Control>h", NULL } },
		{ "win.right-pane::preview", { "<Alt>p", NULL } },
		{ "win.right-pane::details", { "<Alt><Shift>p", NULL } },
		/* In the View menu's order: 1 extra large, 2 large, 3 medium,
		 * 4 small icons, 5 list, 6 details, 7 tiles, 8 the compact
		 * list. */
		{ "win.view-mode::extra-large-icons", { "<Control><Shift>1", NULL } },
		{ "win.view-mode::large-icons", { "<Control><Shift>2", NULL } },
		{ "win.view-mode::icons", { "<Control><Shift>3", NULL } },
		{ "win.view-mode::small-icons", { "<Control><Shift>4", NULL } },
		{ "win.view-mode::list", { "<Control><Shift>5", NULL } },
		{ "win.view-mode::details", { "<Control><Shift>6", NULL } },
		{ "win.view-mode::tiles", { "<Control><Shift>7", NULL } },
		{ "win.view-mode::compact", { "<Control><Shift>8", NULL } },
	};

	for (gsize i = 0; i < G_N_ELEMENTS(accels); i++)
		gtk_application_set_accels_for_action(app, accels[i].action,
						      accels[i].keys);
}
