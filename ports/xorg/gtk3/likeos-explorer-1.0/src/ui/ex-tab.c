// LikeOS -- File Explorer: one tab.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-tab.h"
#include "ui/ex-commandbar.h"
#include "ui/ex-context-menu.h"
#include "ui/ex-file-view.h"
#include "ui/ex-folder-model.h"
#include "ui/ex-home-page.h"
#include "ui/ex-preview-pane.h"
#include "ui/ex-sidebar.h"
#include "ui/ex-style.h"
#include "ui/widgets/lx-breadcrumb.h"
#include "ui/widgets/lx-segmented.h"
#include "backend/ex-dir-loader.h"
#include "backend/ex-monitor.h"
#include "backend/ex-recent.h"
#include "backend/ex-search.h"
#include "backend/ex-thumbs.h"
#include "backend/ex-trash.h"
#include "backend/ex-util.h"
#include "services/ex-settings.h"
#include "services/ex-undo.h"

#include <string.h>

/* The view switch at the right of the status bar. */
enum { SEG_DETAILS, SEG_LARGE };

struct _ExTab {
	ExWindow *win;
	GtkWidget *root, *label, *label_icon, *label_text;
	ExNavigator *nav;
	ExSelection *sel;
	ExFolderModel *model;
	ExMonitor *mon;
	ExSearch *search;
	GCancellable *cancel; /* everything the current place started */
	ExFile *folder_info; /* the folder shown, once known */
	GPtrArray *crumbs; /* ExLocation */
	GPtrArray *reselect; /* GFile to select after a reload */
	ExLocation *shown; /* the place last loaded */

	GtkWidget *back, *fwd, *up, *crumb, *search_entry;
	GtkWidget *cmdbar, *outer, *sidebar, *inner, *stack;
	GtkWidget *home, *thispc, *view, *view_overlay, *empty_label;
	GtkWidget *msg_box, *msg_label;
	GtkWidget *right;
	GtkWidget *st_count, *st_sel, *spinner, *seg;

	char *mode;
	gboolean searching_replaces; /* the search entry edits the search */
	gboolean loading;
	gboolean dead;
};

/* ---- small things -------------------------------------------------------- */

ExLocation *ex_tab_location(ExTab *t)
{
	return ex_navigator_current(t->nav);
}

ExNavigator *ex_tab_navigator(ExTab *t)
{
	return t->nav;
}

ExSelection *ex_tab_selection(ExTab *t)
{
	return t->sel;
}

GtkWidget *ex_tab_widget(ExTab *t)
{
	return t->root;
}

ExTab *ex_tab_from_widget(GtkWidget *w)
{
	return w ? g_object_get_data(G_OBJECT(w), "ex-tab") : NULL;
}

GtkWidget *ex_tab_label(ExTab *t)
{
	return t->label;
}

GFile *ex_tab_folder(ExTab *t)
{
	ExLocation *l = ex_tab_location(t);

	return ex_location_kind(l) == EX_LOC_FOLDER ? ex_location_file(l) : NULL;
}

gboolean ex_tab_folder_writable(ExTab *t)
{
	return ex_tab_folder(t) && t->folder_info &&
	       (ex_file_get_caps(t->folder_info) & EX_CAP_WRITE);
}

char *ex_tab_title(ExTab *t)
{
	return ex_location_display_name(ex_tab_location(t));
}

static gboolean is_current(ExTab *t)
{
	return !t->dead && ex_window_current_tab(t->win) == t;
}

static void actions_changed(ExTab *t)
{
	if (is_current(t))
		ex_window_update_actions(t->win);
}

/* ---- status bar -------------------------------------------------------------- */

static void update_status(ExTab *t)
{
	ExLocationKind k = ex_location_kind(ex_tab_location(t));
	guint n = t->model ? ex_folder_model_count(t->model) : 0;
	guint s = ex_selection_count(t->sel);
	char *a, *b = NULL;

	if (k == EX_LOC_HOME || k == EX_LOC_THISPC)
		a = g_strdup("");
	else
		a = g_strdup_printf(n == 1 ? "%u item" : "%u items", n);
	if (s) {
		guint64 sz = ex_selection_total_size(t->sel);
		char *size = sz ? ex_format_size(sz) : NULL;

		b = g_strdup_printf("%u item%s selected%s%s", s, s == 1 ? "" : "s",
				    size ? "   " : "", size ? size : "");
		g_free(size);
	}
	gtk_label_set_text(GTK_LABEL(t->st_count), a);
	gtk_label_set_text(GTK_LABEL(t->st_sel), b ? b : "");
	g_free(a);
	g_free(b);

	gboolean empty = t->model && !t->loading && n == 0 &&
			 (k == EX_LOC_FOLDER || k == EX_LOC_TRASH || k == EX_LOC_SEARCH);
	gtk_label_set_text(GTK_LABEL(t->empty_label),
			   k == EX_LOC_SEARCH ? "No items match your search." :
			   k == EX_LOC_TRASH  ? "The Trash is empty." :
						"This folder is empty.");
	gtk_widget_set_visible(t->empty_label, empty);
}

static void set_loading(ExTab *t, gboolean on)
{
	t->loading = on;
	gtk_widget_set_visible(t->spinner, on);
	if (on)
		gtk_spinner_start(GTK_SPINNER(t->spinner));
	else
		gtk_spinner_stop(GTK_SPINNER(t->spinner));
	update_status(t);
}

static void model_count(ExFolderModel *m, ExTab *t)
{
	update_status(t);
}

static void sel_changed(ExSelection *s, ExTab *t)
{
	update_status(t);
	actions_changed(t);
}

/* ---- view mode, sort, panes ---------------------------------------------------- */

static gboolean is_icon_mode(const char *m)
{
	return !strcmp(m, "icons") || !strcmp(m, "large-icons") ||
	       !strcmp(m, "tiles");
}

const char *ex_tab_view_mode(ExTab *t)
{
	return t->mode;
}

void ex_tab_set_view_mode(ExTab *t, const char *mode)
{
	g_free(t->mode);
	t->mode = g_strdup(mode);
	ex_file_view_set_mode(EX_FILE_VIEW(t->view), mode);
	lx_segmented_set_active(LX_SEGMENTED(t->seg),
				is_icon_mode(mode) ? SEG_LARGE : SEG_DETAILS);
	actions_changed(t);
}

static ExSortColumn sort_col(const char *name)
{
	if (!g_strcmp0(name, "modified"))
		return EX_SORT_MODIFIED;
	if (!g_strcmp0(name, "type"))
		return EX_SORT_TYPE;
	if (!g_strcmp0(name, "size"))
		return EX_SORT_SIZE;
	return EX_SORT_NAME;
}

static const char *sort_name(ExSortColumn c)
{
	switch (c) {
	case EX_SORT_MODIFIED:
	case EX_SORT_DELETED:
		return "modified";
	case EX_SORT_TYPE:
		return "type";
	case EX_SORT_SIZE:
		return "size";
	default:
		return "name";
	}
}

void ex_tab_set_sort(ExTab *t, const char *column, gboolean ascending)
{
	ExSettings *s = ex_settings_get();

	ex_settings_set_string(s, "sort-column", column);
	ex_settings_set_bool(s, "sort-ascending", ascending);
	if (t->model)
		ex_folder_model_set_sort(t->model, sort_col(column), ascending);
	ex_file_view_set_columns(EX_FILE_VIEW(t->view),
				 ex_location_kind(ex_tab_location(t)) == EX_LOC_TRASH ?
					 EX_COLUMNS_TRASH :
				 ex_location_kind(ex_tab_location(t)) == EX_LOC_SEARCH ?
					 EX_COLUMNS_SEARCH :
					 EX_COLUMNS_FOLDER);
	actions_changed(t);
}

void ex_tab_get_sort(ExTab *t, const char **column, gboolean *ascending)
{
	ExSortColumn c = EX_SORT_NAME;
	gboolean a = TRUE;

	if (t->model)
		ex_folder_model_get_sort(t->model, &c, &a);
	*column = sort_name(c);
	*ascending = a;
}

void ex_tab_set_right_pane(ExTab *t, const char *pane)
{
	gboolean on = g_strcmp0(pane, "none") != 0;

	gtk_widget_set_visible(t->right, on);
	if (on)
		ex_preview_pane_set_mode(t->right, pane);
}

void ex_tab_set_navigation_pane(ExTab *t, gboolean shown)
{
	gtk_widget_set_visible(t->sidebar, shown);
}

void ex_tab_apply_settings(ExTab *t)
{
	ExSettings *s = ex_settings_get();
	char *rp = ex_settings_get_string(s, "right-pane");

	if (t->model)
		ex_folder_model_set_show_hidden(t->model,
						ex_settings_get_bool(s, "show-hidden"));
	ex_file_view_set_show_extensions(EX_FILE_VIEW(t->view),
					 ex_settings_get_bool(s, "show-extensions"));
	ex_file_view_set_single_click(EX_FILE_VIEW(t->view),
				      ex_settings_get_bool(s, "single-click"));
	ex_tab_set_right_pane(t, rp);
	ex_tab_set_navigation_pane(t, ex_settings_get_bool(s, "show-navigation-pane"));
	g_free(rp);
	update_status(t);
}

/* ---- selection commands --------------------------------------------------------- */

void ex_tab_select_all(ExTab *t)
{
	ex_file_view_select_all(EX_FILE_VIEW(t->view));
}

void ex_tab_select_none(ExTab *t)
{
	ex_file_view_select_none(EX_FILE_VIEW(t->view));
}

void ex_tab_invert_selection(ExTab *t)
{
	ex_file_view_invert_selection(EX_FILE_VIEW(t->view));
}

void ex_tab_select_files(ExTab *t, GPtrArray *files)
{
	/* While the place loads, this is what the load ends by selecting
	 * (instead of what it was going to). */
	if (t->loading) {
		g_clear_pointer(&t->reselect, g_ptr_array_unref);
		t->reselect = ex_objects_copy(files);
	}
	ex_file_view_select_files(EX_FILE_VIEW(t->view), files);
}

void ex_tab_start_rename(ExTab *t)
{
	ExFile *f = ex_selection_single(t->sel);

	if (f)
		ex_file_view_start_rename(EX_FILE_VIEW(t->view), f);
}

void ex_tab_note_removal(ExTab *t)
{
	ex_file_view_note_removal(EX_FILE_VIEW(t->view));
}

void ex_tab_rename_when_shown(ExTab *t, GFile *f)
{
	ex_file_view_rename_when_shown(EX_FILE_VIEW(t->view), f);
}

GPtrArray *ex_tab_listed(ExTab *t)
{
	if (!t->model)
		return g_ptr_array_new_with_free_func(g_object_unref);
	return ex_folder_model_list(t->model);
}

void ex_tab_focus_address(ExTab *t)
{
	lx_breadcrumb_edit(LX_BREADCRUMB(t->crumb));
}

void ex_tab_focus_search(ExTab *t)
{
	gtk_widget_grab_focus(t->search_entry);
}

void ex_tab_focus_view(ExTab *t)
{
	ex_file_view_focus(EX_FILE_VIEW(t->view));
}

/* ---- loading a place ---------------------------------------------------------------- */

static void new_model(ExTab *t)
{
	ExSettings *s = ex_settings_get();
	char *col = ex_settings_get_string(s, "sort-column");

	if (t->model)
		g_signal_handlers_disconnect_by_data(t->model, t);
	g_clear_object(&t->model);
	t->model = ex_folder_model_new();
	ex_folder_model_set_show_hidden(t->model,
					ex_settings_get_bool(s, "show-hidden"));
	ex_folder_model_set_sort(t->model, sort_col(col),
				 ex_settings_get_bool(s, "sort-ascending"));
	g_signal_connect(t->model, "count-changed", G_CALLBACK(model_count), t);
	ex_file_view_set_model(EX_FILE_VIEW(t->view), t->model);
	g_free(col);
}

static void stop_everything(ExTab *t)
{
	if (t->cancel) {
		g_cancellable_cancel(t->cancel);
		g_clear_object(&t->cancel);
	}
	t->cancel = g_cancellable_new();
	if (t->mon)
		g_signal_handlers_disconnect_by_data(t->mon, t);
	g_clear_object(&t->mon);
	if (t->search) {
		g_signal_handlers_disconnect_by_data(t->search, t);
		ex_search_cancel(t->search);
		g_clear_object(&t->search);
	}
	g_clear_object(&t->folder_info);
}

static void show_message(ExTab *t, const char *text)
{
	gtk_label_set_text(GTK_LABEL(t->msg_label), text);
	gtk_stack_set_visible_child(GTK_STACK(t->stack), t->msg_box);
	set_loading(t, FALSE);
}

static void reselect(ExTab *t)
{
	if (t->reselect) {
		ex_file_view_select_files(EX_FILE_VIEW(t->view), t->reselect);
		g_clear_pointer(&t->reselect, g_ptr_array_unref);
	}
}

/* Folder reading. */

static void batch(GPtrArray *files, gpointer data)
{
	ExTab *t = data;

	ex_folder_model_add(t->model, files);
}

static void loaded(GObject *src, GAsyncResult *res, gpointer data)
{
	ExTab *t = data;
	GError *e = NULL;

	if (!ex_dir_load_finish(res, &e)) {
		if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			char *m = g_strdup_printf("This folder cannot be opened.\n\n%s",
						  e->message);
			show_message(t, m);
			g_free(m);
		}
		g_error_free(e);
		return;
	}
	set_loading(t, FALSE);
	reselect(t);
}

/* An item the monitor reported, once its information is in. */
typedef struct {
	ExTab *t;
	GCancellable *cancel;
} Ask;

static void got_info(GObject *src, GAsyncResult *res, gpointer data)
{
	Ask *a = data;
	GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);

	if (info && !g_cancellable_is_cancelled(a->cancel)) {
		ExFile *known = ex_folder_model_lookup(a->t->model, G_FILE(src));

		if (known) {
			ex_file_set_info(known, info);
		} else {
			ExFile *f = ex_file_new(G_FILE(src), info);

			ex_folder_model_add_one(a->t->model, f);
			g_object_unref(f);
		}
	}
	g_clear_object(&info);
	g_object_unref(a->cancel);
	g_free(a);
}

static void ask_info(ExTab *t, GFile *f)
{
	Ask *a = g_new0(Ask, 1);

	a->t = t;
	a->cancel = g_object_ref(t->cancel);
	g_file_query_info_async(f, EX_FILE_ATTRS, G_FILE_QUERY_INFO_NONE,
				G_PRIORITY_DEFAULT, t->cancel, got_info, a);
}

static void m_created(ExMonitor *m, GFile *f, ExTab *t)
{
	ask_info(t, f);
}

static void m_changed(ExMonitor *m, GFile *f, ExTab *t)
{
	if (ex_folder_model_lookup(t->model, f))
		ask_info(t, f);
}

static void m_deleted(ExMonitor *m, GFile *f, ExTab *t)
{
	ex_folder_model_remove(t->model, f);
}

/* Renamed within the folder: once the new name's information is in, the
 * row becomes the new item where it is (selected if it was). */
typedef struct {
	ExTab *t;
	GCancellable *cancel;
	GFile *from;
} Renamed;

static void got_renamed_info(GObject *src, GAsyncResult *res, gpointer data)
{
	Renamed *r = data;
	GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);

	if (!g_cancellable_is_cancelled(r->cancel)) {
		if (info) {
			ExFile *f = ex_file_new(G_FILE(src), info);

			ex_folder_model_replace(r->t->model, r->from, f);
			g_object_unref(f);
		} else {
			/* Gone again already. */
			ex_folder_model_remove(r->t->model, r->from);
		}
	}
	g_clear_object(&info);
	g_object_unref(r->cancel);
	g_object_unref(r->from);
	g_free(r);
}

static void m_renamed(ExMonitor *m, GFile *from, GFile *to, ExTab *t)
{
	Renamed *r = g_new0(Renamed, 1);

	r->t = t;
	r->cancel = g_object_ref(t->cancel);
	r->from = g_object_ref(from);
	g_file_query_info_async(to, EX_FILE_ATTRS, G_FILE_QUERY_INFO_NONE,
				G_PRIORITY_DEFAULT, t->cancel, got_renamed_info, r);
}

static void m_gone(ExMonitor *m, ExTab *t)
{
	/* The folder itself went: show the nearest one still there. */
	ExLocation *p = ex_location_parent(ex_tab_location(t));

	if (p) {
		ex_navigator_replace(t->nav, p);
		ex_location_unref(p);
	}
}

static void load_location(ExTab *t);

static void m_rescan(ExMonitor *m, ExTab *t)
{
	ex_tab_refresh(t);
}

static void got_folder_info(GObject *src, GAsyncResult *res, gpointer data)
{
	Ask *a = data;
	GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);

	if (info && !g_cancellable_is_cancelled(a->cancel)) {
		g_clear_object(&a->t->folder_info);
		a->t->folder_info = ex_file_new(G_FILE(src), info);
		actions_changed(a->t);
	}
	g_clear_object(&info);
	g_object_unref(a->cancel);
	g_free(a);
}

static void watch(ExTab *t, GFile *dir)
{
	t->mon = ex_monitor_new(dir);
	g_signal_connect(t->mon, "created", G_CALLBACK(m_created), t);
	g_signal_connect(t->mon, "deleted", G_CALLBACK(m_deleted), t);
	g_signal_connect(t->mon, "changed", G_CALLBACK(m_changed), t);
	g_signal_connect(t->mon, "renamed", G_CALLBACK(m_renamed), t);
	g_signal_connect(t->mon, "rescan", G_CALLBACK(m_rescan), t);
}

static void load_folder(ExTab *t, GFile *dir)
{
	Ask *a = g_new0(Ask, 1);

	new_model(t);
	ex_file_view_set_columns(EX_FILE_VIEW(t->view), EX_COLUMNS_FOLDER);
	gtk_stack_set_visible_child(GTK_STACK(t->stack), t->view_overlay);
	set_loading(t, TRUE);
	ex_dir_load_async(dir, t->cancel, batch, loaded, t);
	watch(t, dir);
	g_signal_connect(t->mon, "gone", G_CALLBACK(m_gone), t);
	a->t = t;
	a->cancel = g_object_ref(t->cancel);
	g_file_query_info_async(dir, EX_FILE_ATTRS, G_FILE_QUERY_INFO_NONE,
				G_PRIORITY_DEFAULT, t->cancel, got_folder_info, a);
}

/* The Trash: its records are read with the items. */

static void trash_listed(GObject *src, GAsyncResult *res, gpointer data)
{
	Ask *a = data;
	GPtrArray *items = ex_trash_list_finish(res, NULL);

	if (items && !g_cancellable_is_cancelled(a->cancel)) {
		ExTab *t = a->t;
		GPtrArray *had = ex_folder_model_list(t->model);
		GHashTable *now = g_hash_table_new((GHashFunc)g_file_hash,
						   (GEqualFunc)g_file_equal);

		/* The listing is brought up to date in place, so the rows
		 * that stay keep their selection. */
		for (guint i = 0; i < items->len; i++)
			g_hash_table_add(now, ex_file_get_file(
						      g_ptr_array_index(items, i)));
		for (guint i = 0; i < had->len; i++) {
			GFile *f = ex_file_get_file(g_ptr_array_index(had, i));

			if (!g_hash_table_contains(now, f))
				ex_folder_model_remove(t->model, f);
		}
		ex_folder_model_add(t->model, items);
		g_hash_table_unref(now);
		g_ptr_array_unref(had);
		set_loading(t, FALSE);
		reselect(t);
		actions_changed(t);
	}
	if (items)
		g_ptr_array_unref(items);
	g_object_unref(a->cancel);
	g_free(a);
}

static void list_trash(ExTab *t)
{
	Ask *a = g_new0(Ask, 1);

	a->t = t;
	a->cancel = g_object_ref(t->cancel);
	set_loading(t, TRUE);
	ex_trash_list_async(t->cancel, trash_listed, a);
}

static void trash_event(ExMonitor *m, GFile *f, ExTab *t)
{
	list_trash(t);
}

static void trash_renamed(ExMonitor *m, GFile *from, GFile *to, ExTab *t)
{
	list_trash(t);
}

static void trash_rescan(ExMonitor *m, ExTab *t)
{
	list_trash(t);
}

static void load_trash(ExTab *t)
{
	GFile *dir = ex_trash_files_dir();

	new_model(t);
	ex_file_view_set_columns(EX_FILE_VIEW(t->view), EX_COLUMNS_TRASH);
	gtk_stack_set_visible_child(GTK_STACK(t->stack), t->view_overlay);
	list_trash(t);
	t->mon = ex_monitor_new(dir);
	g_signal_connect(t->mon, "created", G_CALLBACK(trash_event), t);
	g_signal_connect(t->mon, "deleted", G_CALLBACK(trash_event), t);
	g_signal_connect(t->mon, "renamed", G_CALLBACK(trash_renamed), t);
	g_signal_connect(t->mon, "rescan", G_CALLBACK(trash_rescan), t);
	g_object_unref(dir);
}

/* Search. */

static void hits(ExSearch *s, GPtrArray *files, ExTab *t)
{
	ex_folder_model_add(t->model, files);
}

static void search_done(ExSearch *s, gboolean from_index, ExTab *t)
{
	set_loading(t, FALSE);
}

static void load_search(ExTab *t, ExLocation *loc)
{
	GFile *scope = ex_location_file(loc);

	new_model(t);
	ex_file_view_set_columns(EX_FILE_VIEW(t->view), EX_COLUMNS_SEARCH);
	gtk_stack_set_visible_child(GTK_STACK(t->stack), t->view_overlay);
	if (!scope) {
		show_message(t, "Search a folder: open it, then type in the search box.");
		return;
	}
	set_loading(t, TRUE);
	t->search = ex_search_new(scope, ex_location_query(loc),
				  ex_settings_get_bool(ex_settings_get(),
						       "show-hidden"));
	g_signal_connect(t->search, "hits", G_CALLBACK(hits), t);
	g_signal_connect(t->search, "finished", G_CALLBACK(search_done), t);
	ex_search_start(t->search);
}

/* The bar above. */

static void set_crumbs(ExTab *t, ExLocation *loc)
{
	GPtrArray *c = ex_location_crumbs(loc);
	const char **labels = g_new0(const char *, c->len + 1);
	const char **icons = g_new0(const char *, c->len + 1);
	gboolean *expand = g_new0(gboolean, c->len + 1);
	char **names = g_new0(char *, c->len + 1);
	char *edit = ex_location_edit_text(loc);

	for (guint i = 0; i < c->len; i++) {
		ExLocation *l = g_ptr_array_index(c, i);

		names[i] = ex_location_kind(l) == EX_LOC_SEARCH ?
				   g_strdup_printf("Search “%s”", ex_location_query(l)) :
				   ex_location_display_name(l);
		labels[i] = names[i];
		icons[i] = ex_location_icon_name(l);
		/* Only a folder has folders inside to list. */
		expand[i] = ex_location_kind(l) == EX_LOC_FOLDER;
	}
	lx_breadcrumb_set_crumbs(LX_BREADCRUMB(t->crumb), labels, icons, expand,
				 c->len);
	lx_breadcrumb_set_edit_text(LX_BREADCRUMB(t->crumb), edit);
	g_clear_pointer(&t->crumbs, g_ptr_array_unref);
	t->crumbs = c;
	g_strfreev(names);
	g_free(labels);
	g_free(icons);
	g_free(expand);
	g_free(edit);
}

static void update_title(ExTab *t)
{
	char *title = ex_tab_title(t);

	gtk_label_set_text(GTK_LABEL(t->label_text), title);
	gtk_widget_set_tooltip_text(t->label, title);
	gtk_image_set_from_icon_name(GTK_IMAGE(t->label_icon),
				     ex_location_icon_name(ex_tab_location(t)),
				     GTK_ICON_SIZE_MENU);
	if (is_current(t)) {
		char *wt = g_strdup_printf("%s - File Explorer", title);

		gtk_window_set_title(GTK_WINDOW(t->win), wt);
		g_free(wt);
	}
	g_free(title);
}

static void set_search_hint(ExTab *t, ExLocation *loc)
{
	ExLocation *scope = ex_location_kind(loc) == EX_LOC_SEARCH ?
				    ex_location_scope(loc) :
				    loc;
	char *name = ex_location_display_name(scope);
	char *hint = g_strdup_printf("Search %s", name);

	gtk_entry_set_placeholder_text(GTK_ENTRY(t->search_entry), hint);
	/* Leaving a search empties the box; entering one shows its words. */
	t->searching_replaces = TRUE;
	gtk_entry_set_text(GTK_ENTRY(t->search_entry),
			   ex_location_kind(loc) == EX_LOC_SEARCH ?
				   ex_location_query(loc) :
				   "");
	t->searching_replaces = FALSE;
	gtk_widget_set_sensitive(t->search_entry,
				 ex_location_file(scope) != NULL);
	g_free(hint);
	g_free(name);
}

static void load_location(ExTab *t)
{
	ExLocation *loc = ex_tab_location(t);
	ExLocationKind k = ex_location_kind(loc);
	char *mode;

	stop_everything(t);
	ex_thumbs_cancel_all();
	ex_selection_clear(t->sel);
	if (t->shown != loc) {
		g_clear_pointer(&t->shown, ex_location_unref);
		t->shown = ex_location_ref(loc);
	}
	set_crumbs(t, loc);
	update_title(t);
	set_search_hint(t, loc);
	ex_sidebar_set_location(EX_SIDEBAR(t->sidebar), loc);
	ex_commandbar_set_kind(t->cmdbar, k);
	ex_preview_pane_set_location(t->right, loc);

	mode = ex_settings_view_for(ex_settings_get(), ex_location_id(loc));
	g_free(t->mode);
	t->mode = mode;
	ex_file_view_set_mode(EX_FILE_VIEW(t->view), mode);
	lx_segmented_set_active(LX_SEGMENTED(t->seg),
				is_icon_mode(mode) ? SEG_LARGE : SEG_DETAILS);

	/* Where a drop on no item goes, by name for the drag's caption. */
	{
		GFile *dir = k == EX_LOC_FOLDER || k == EX_LOC_TRASH ?
				     ex_location_file(loc) :
				     NULL;
		char *name = dir ? ex_location_display_name(loc) : NULL;

		ex_file_view_set_folder(EX_FILE_VIEW(t->view), dir, name,
					k == EX_LOC_TRASH ? GDK_ACTION_MOVE : 0);
		g_free(name);
	}
	switch (k) {
	case EX_LOC_HOME:
		g_clear_object(&t->model);
		ex_file_view_set_model(EX_FILE_VIEW(t->view), NULL);
		ex_home_page_reload(EX_HOME_PAGE(t->home));
		gtk_stack_set_visible_child(GTK_STACK(t->stack), t->home);
		set_loading(t, FALSE);
		break;
	case EX_LOC_THISPC:
		g_clear_object(&t->model);
		ex_file_view_set_model(EX_FILE_VIEW(t->view), NULL);
		ex_home_page_reload(EX_HOME_PAGE(t->thispc));
		gtk_stack_set_visible_child(GTK_STACK(t->stack), t->thispc);
		set_loading(t, FALSE);
		break;
	case EX_LOC_TRASH:
		load_trash(t);
		break;
	case EX_LOC_SEARCH:
		load_search(t, loc);
		break;
	case EX_LOC_FOLDER:
	default:
		load_folder(t, ex_location_file(loc));
		break;
	}
	gtk_widget_set_sensitive(t->back, ex_navigator_can_back(t->nav));
	gtk_widget_set_sensitive(t->fwd, ex_navigator_can_forward(t->nav));
	gtk_widget_set_sensitive(t->up, ex_navigator_can_up(t->nav));
	actions_changed(t);
	if (k != EX_LOC_HOME && k != EX_LOC_THISPC && is_current(t) &&
	    !lx_breadcrumb_is_editing(LX_BREADCRUMB(t->crumb)) &&
	    !gtk_widget_has_focus(t->search_entry))
		ex_file_view_focus(EX_FILE_VIEW(t->view));
}

/* Up, or Back to a folder that holds the one left: the folder come from
 * is selected, as in Windows. */
static GFile *came_from(ExLocation *from, ExLocation *to)
{
	GFile *f = from ? ex_location_file(from) : NULL;
	GFile *dir = ex_location_file(to);
	GFile *c;

	if (!f || !dir || ex_location_kind(from) != EX_LOC_FOLDER ||
	    ex_location_kind(to) != EX_LOC_FOLDER || !g_file_has_prefix(f, dir))
		return NULL;
	c = g_object_ref(f);
	for (;;) {
		GFile *p = g_file_get_parent(c);

		if (!p || g_file_equal(p, dir)) {
			g_clear_object(&p);
			return c;
		}
		g_object_unref(c);
		c = p;
	}
}

static void nav_changed(ExNavigator *n, ExTab *t)
{
	GFile *c = came_from(t->shown, ex_tab_location(t));

	g_clear_pointer(&t->reselect, g_ptr_array_unref);
	if (c) {
		t->reselect = g_ptr_array_new_with_free_func(g_object_unref);
		g_ptr_array_add(t->reselect, c);
	}
	load_location(t);
}

void ex_tab_go(ExTab *t, ExLocation *loc)
{
	ex_navigator_go(t->nav, loc);
}

void ex_tab_refresh(ExTab *t)
{
	g_clear_pointer(&t->reselect, g_ptr_array_unref);
	t->reselect = ex_selection_files(t->sel);
	load_location(t);
}

/* ---- the address bar ----------------------------------------------------------------- */

static void crumb_activated(LxBreadcrumb *b, guint idx, ExTab *t)
{
	if (t->crumbs && idx < t->crumbs->len)
		ex_tab_go(t, g_ptr_array_index(t->crumbs, idx));
}

typedef struct {
	ExTab *t;
	char *text;
	GCancellable *cancel;
} Typed;

static void typed_info(GObject *src, GAsyncResult *res, gpointer data)
{
	Typed *ty = data;
	GError *e = NULL;
	GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, &e);

	if (!g_cancellable_is_cancelled(ty->cancel)) {
		if (!info) {
			char *m = g_strdup_printf("Cannot find “%s”.", ty->text);

			ex_window_show_error(ty->t->win, m,
					     "Check the spelling and try again.");
			g_free(m);
		} else if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
			ExLocation *l = ex_location_new_folder(G_FILE(src));

			ex_tab_go(ty->t, l);
			ex_location_unref(l);
		} else {
			/* A file's path opens the file, as in Windows. */
			ExFile *f = ex_file_new(G_FILE(src), info);
			GPtrArray *one = g_ptr_array_new_with_free_func(g_object_unref);

			g_ptr_array_add(one, f);
			ex_window_open(ty->t->win, one, FALSE);
			g_ptr_array_unref(one);
		}
	}
	g_clear_error(&e);
	g_clear_object(&info);
	g_object_unref(ty->cancel);
	g_free(ty->text);
	g_free(ty);
}

static void text_activated(LxBreadcrumb *b, const char *text, ExTab *t)
{
	ExLocation *l = ex_location_parse(g_strstrip((char *)text));

	/* The address field closed: the keyboard goes back to the items
	 * until the new place has loaded and takes it. */
	ex_tab_focus_view(t);
	if (!l)
		return;
	if (ex_location_kind(l) != EX_LOC_FOLDER) {
		ex_tab_go(t, l);
	} else {
		Typed *ty = g_new0(Typed, 1);

		ty->t = t;
		ty->text = g_strdup(text);
		ty->cancel = g_object_ref(t->cancel);
		g_file_query_info_async(ex_location_file(l), EX_FILE_ATTRS,
					G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
					t->cancel, typed_info, ty);
	}
	ex_location_unref(l);
}

/* A chevron's list of subfolders, read in the background. */
typedef struct {
	ExTab *t;
	GtkWidget *box;
	GPtrArray *found; /* ExFile */
} Kids;

static void kid_clicked(GtkButton *b, ExTab *t)
{
	ExLocation *l = g_object_get_data(G_OBJECT(b), "ex-location");
	GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);

	if (pop)
		gtk_popover_popdown(GTK_POPOVER(pop));
	ex_tab_go(t, l);
}

static void kids_batch(GPtrArray *files, gpointer data)
{
	Kids *k = data;

	for (guint i = 0; i < files->len; i++) {
		ExFile *f = g_ptr_array_index(files, i);

		if (ex_file_is_dir(f) && !ex_file_is_hidden(f))
			g_ptr_array_add(k->found, g_object_ref(f));
	}
}

static int by_name(gconstpointer a, gconstpointer b)
{
	return strcmp(ex_file_get_collate_key(*(ExFile *const *)a),
		      ex_file_get_collate_key(*(ExFile *const *)b));
}

static void kids_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Kids *k = data;

	if (ex_dir_load_finish(res, NULL)) {
		g_ptr_array_sort(k->found, by_name);
		for (guint i = 0; i < k->found->len && i < 200; i++) {
			ExFile *f = g_ptr_array_index(k->found, i);
			GtkWidget *b = gtk_button_new_with_label(ex_file_get_name(f));
			ExLocation *l = ex_location_new_folder(ex_file_get_file(f));

			gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
			gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)),
					      GTK_ALIGN_START);
			g_object_set_data_full(G_OBJECT(b), "ex-location", l,
					       (GDestroyNotify)ex_location_unref);
			g_signal_connect(b, "clicked", G_CALLBACK(kid_clicked), k->t);
			gtk_box_pack_start(GTK_BOX(k->box), b, FALSE, FALSE, 0);
			gtk_widget_show(b);
		}
		if (!k->found->len) {
			GtkWidget *l = gtk_label_new("No folders inside");

			ex_style_add(l, EX_CLASS_PLACEHOLDER);
			gtk_box_pack_start(GTK_BOX(k->box), l, FALSE, FALSE, 6);
			gtk_widget_show(l);
		}
	}
	g_object_unref(k->box);
	g_ptr_array_unref(k->found);
	g_free(k);
}

static void children_wanted(LxBreadcrumb *b, guint idx, GtkWidget *box, ExTab *t)
{
	ExLocation *l = t->crumbs && idx < t->crumbs->len ?
				g_ptr_array_index(t->crumbs, idx) :
				NULL;
	Kids *k;

	if (!l || ex_location_kind(l) != EX_LOC_FOLDER)
		return;
	k = g_new0(Kids, 1);
	k->t = t;
	k->box = g_object_ref(box);
	k->found = g_ptr_array_new_with_free_func(g_object_unref);
	ex_dir_load_async(ex_location_file(l), t->cancel, kids_batch, kids_done, k);
}

/* ---- search box ------------------------------------------------------------------------ */

static void search_changed(GtkSearchEntry *e, ExTab *t)
{
	const char *q = gtk_entry_get_text(GTK_ENTRY(e));
	ExLocation *cur = ex_tab_location(t);
	gboolean in_search = ex_location_kind(cur) == EX_LOC_SEARCH;

	if (t->searching_replaces)
		return;
	if (!*q) {
		if (in_search)
			ex_navigator_replace(t->nav, ex_location_scope(cur));
		return;
	}
	ExLocation *s = ex_location_new_search(cur, q);

	/* Typing more refines the search; it is one history entry. */
	if (in_search)
		ex_navigator_replace(t->nav, s);
	else
		ex_navigator_go(t->nav, s);
	ex_location_unref(s);
	gtk_widget_grab_focus(GTK_WIDGET(e));
	gtk_editable_set_position(GTK_EDITABLE(e), -1);
}

static void search_stop(GtkSearchEntry *e, ExTab *t)
{
	gtk_entry_set_text(GTK_ENTRY(e), "");
	ex_tab_focus_view(t);
}

static void search_activate(GtkSearchEntry *e, ExTab *t)
{
	ex_tab_focus_view(t);
}

/* ---- the views' requests ----------------------------------------------------------------- */

static void view_activate(ExFileView *v, GPtrArray *files, ExTab *t)
{
	if (ex_location_kind(ex_tab_location(t)) == EX_LOC_TRASH)
		return; /* the Trash's items are not opened from it */
	ex_window_open(t->win, files, FALSE);
}

static void view_popup(ExFileView *v, GdkEventButton *ev, gboolean on_item,
		       ExTab *t)
{
	ex_tab_popup(t, ev, on_item);
}

static void view_rename(ExFileView *v, ExFile *f, const char *name, ExTab *t)
{
	const char *why = ex_name_problem(name);
	ExOperation *op;

	if (why) {
		ex_window_show_error(t->win, "The name cannot be used", why);
		return;
	}
	op = ex_operation_new_rename(ex_file_get_file(f), name);
	ex_window_run(t->win, op, TRUE, TRUE);
	g_object_unref(op);
}

/* Move when source and destination share a filesystem, copy otherwise. */
static guint default_action(GFile *src, GFile *dest)
{
	const char *attr = G_FILE_ATTRIBUTE_ID_FILESYSTEM;
	GFileInfo *a = g_file_query_info(src, attr, G_FILE_QUERY_INFO_NONE, NULL,
					 NULL);
	GFileInfo *b = g_file_query_info(dest, attr, G_FILE_QUERY_INFO_NONE, NULL,
					 NULL);
	gboolean same = a && b &&
			!g_strcmp0(g_file_info_get_attribute_string(a, attr),
				   g_file_info_get_attribute_string(b, attr));

	g_clear_object(&a);
	g_clear_object(&b);
	return same ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static void drop_files(ExTab *t, GPtrArray *uris, GFile *dest, guint action)
{
	GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
	ExLocationKind k = ex_location_kind(ex_tab_location(t));
	ExOperation *op = NULL;

	for (guint i = 0; i < uris->len; i++)
		g_ptr_array_add(files, g_file_new_for_uri(g_ptr_array_index(uris, i)));
	if (!dest && k == EX_LOC_TRASH) {
		op = ex_operation_new_trash(files);
	} else {
		if (!dest)
			dest = ex_tab_folder(t);
		if (!dest || !files->len) {
			g_ptr_array_unref(files);
			return;
		}
		/* Dropped where it already is: nothing to do. */
		GFile *parent = g_file_get_parent(g_ptr_array_index(files, 0));
		gboolean same_place = parent && g_file_equal(parent, dest);

		g_clear_object(&parent);
		if (!action)
			action = default_action(g_ptr_array_index(files, 0), dest);
		if (same_place && action == GDK_ACTION_MOVE) {
			g_ptr_array_unref(files);
			return;
		}
		if (action == GDK_ACTION_LINK)
			op = ex_operation_new_link(files, dest);
		else if (action == GDK_ACTION_MOVE)
			op = ex_operation_new_move(files, dest);
		else
			op = ex_operation_new_copy(files, dest);
	}
	ex_window_run(t->win, op, TRUE, TRUE);
	g_object_unref(op);
	g_ptr_array_unref(files);
}

static void view_drop(ExFileView *v, GPtrArray *uris, GFile *dest, guint action,
		      ExTab *t)
{
	drop_files(t, uris, dest, action);
}

static void view_sort(ExFileView *v, int col, gboolean asc, ExTab *t)
{
	ex_tab_set_sort(t, sort_name((ExSortColumn)col), asc);
}

static void view_zoom(ExFileView *v, int dir, ExTab *t)
{
	static const char *const order[] = { "details", "list", "tiles", "icons",
					     "large-icons" };
	int cur = 0;

	for (gsize i = 0; i < G_N_ELEMENTS(order); i++)
		if (!strcmp(order[i], t->mode))
			cur = (int)i;
	cur = CLAMP(cur + dir, 0, (int)G_N_ELEMENTS(order) - 1);
	g_action_group_activate_action(G_ACTION_GROUP(t->win), "view-mode",
				       g_variant_new_string(order[cur]));
}

static void seg_changed(LxSegmented *s, int idx, ExTab *t)
{
	g_action_group_activate_action(
		G_ACTION_GROUP(t->win), "view-mode",
		g_variant_new_string(idx == SEG_LARGE ? "large-icons" : "details"));
}

/* ---- the navigation pane, Home ------------------------------------------------------------ */

static void sidebar_open(ExSidebar *s, ExLocation *l, gboolean new_tab, ExTab *t)
{
	if (new_tab)
		ex_window_add_tab(t->win, l, FALSE);
	else
		ex_tab_go(t, l);
}

static void sidebar_drop(ExSidebar *s, GPtrArray *uris, GFile *dest, ExTab *t)
{
	GFile *tf = ex_trash_files_dir();

	if (g_file_equal(dest, tf)) {
		GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
		ExOperation *op;

		for (guint i = 0; i < uris->len; i++)
			g_ptr_array_add(files,
					g_file_new_for_uri(g_ptr_array_index(uris, i)));
		op = ex_operation_new_trash(files);
		ex_window_run(t->win, op, TRUE, FALSE);
		g_object_unref(op);
		g_ptr_array_unref(files);
	} else {
		drop_files(t, uris, dest, 0);
	}
	g_object_unref(tf);
}

static void sidebar_popup(ExSidebar *s, ExLocation *l, GdkEventButton *ev,
			  ExTab *t)
{
	GMenuModel *m = ex_context_menu_location(l);

	ex_context_menu_popup(t->sidebar, m, ev);
	g_object_unref(m);
}

/* A tile of Home or This PC: the same menu as the place's row in the
 * navigation pane. */
static void home_popup_location(GtkWidget *home, ExLocation *l,
				GdkEventButton *ev, ExTab *t)
{
	GMenuModel *m = ex_context_menu_location(l);

	ex_window_update_actions(t->win);
	ex_context_menu_popup(home, m, ev);
	g_object_unref(m);
}

static void home_open(GtkWidget *h, ExLocation *l, gboolean new_tab, ExTab *t)
{
	sidebar_open(NULL, l, new_tab, t);
}

static void home_open_file(GtkWidget *h, ExFile *f, ExTab *t)
{
	GPtrArray *one = g_ptr_array_new_with_free_func(g_object_unref);

	g_ptr_array_add(one, g_object_ref(f));
	ex_window_open(t->win, one, FALSE);
	g_ptr_array_unref(one);
}

/* Home's own little menu: these items are not a selection of the tab. */
typedef struct {
	ExTab *t;
	ExFile *f;
} HomeItem;

static void home_item_free(gpointer data, GClosure *c)
{
	HomeItem *h = data;

	g_object_unref(h->f);
	g_free(h);
}

static void hi_open(GtkMenuItem *mi, HomeItem *h)
{
	home_open_file(NULL, h->f, h->t);
}

static void hi_location(GtkMenuItem *mi, HomeItem *h)
{
	GFile *p = g_file_get_parent(ex_file_get_file(h->f));

	if (p) {
		ExLocation *l = ex_location_new_folder(p);
		GPtrArray *sel = g_ptr_array_new_with_free_func(g_object_unref);

		ex_tab_go(h->t, l);
		g_ptr_array_add(sel, g_object_ref(ex_file_get_file(h->f)));
		ex_tab_select_files(h->t, sel);
		g_ptr_array_unref(sel);
		ex_location_unref(l);
		g_object_unref(p);
	}
}

static void hi_unfavorite(GtkMenuItem *mi, HomeItem *h)
{
	ex_settings_set_favorite(ex_settings_get(), ex_file_get_file(h->f), FALSE);
}

static void hi_forget(GtkMenuItem *mi, HomeItem *h)
{
	(void)ex_recent_forget(ex_file_get_file(h->f), NULL);
}

static void menu_item(GtkWidget *menu, const char *label, GCallback cb,
		      ExTab *t, ExFile *f)
{
	GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
	HomeItem *h = g_new0(HomeItem, 1);

	h->t = t;
	h->f = g_object_ref(f);
	g_signal_connect_data(mi, "activate", cb, h, home_item_free, 0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void home_popup(GtkWidget *home, ExFile *f, GdkEventButton *ev, ExTab *t)
{
	GtkWidget *menu = gtk_menu_new();

	menu_item(menu, "_Open", G_CALLBACK(hi_open), t, f);
	menu_item(menu, "Open file _location", G_CALLBACK(hi_location), t, f);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	if (ex_settings_is_favorite(ex_settings_get(), ex_file_get_file(f)))
		menu_item(menu, "Remove from _Favorites", G_CALLBACK(hi_unfavorite),
			  t, f);
	menu_item(menu, "Remove from _Recent", G_CALLBACK(hi_forget), t, f);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), home, NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

void ex_tab_popup(ExTab *t, GdkEventButton *ev, gboolean on_item)
{
	GMenuModel *m = on_item ? ex_context_menu_items(t) :
				  ex_context_menu_background(t);

	ex_window_update_actions(t->win);
	ex_context_menu_popup(t->view, m, ev);
	g_object_unref(m);
}

/* ---- construction ------------------------------------------------------------------------ */

static GtkWidget *nav_button(const char *icon, const char *tip,
			     const char *action)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);

	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), tip);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
	return b;
}

/* Right click on Back or Forward: the places in that direction. */
static void history_jump(GtkMenuItem *mi, ExTab *t)
{
	ex_navigator_jump(t->nav, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi),
								   "ex-steps")));
}

static gboolean history_menu(GtkWidget *b, GdkEventButton *ev, ExTab *t)
{
	gboolean back = b == t->back;
	GPtrArray *list;
	GtkWidget *menu;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	list = back ? ex_navigator_back_list(t->nav) :
		      ex_navigator_forward_list(t->nav);
	menu = gtk_menu_new();
	for (guint i = 0; i < list->len && i < 15; i++) {
		char *name = ex_location_display_name(g_ptr_array_index(list, i));
		GtkWidget *mi = gtk_menu_item_new_with_label(name);

		g_object_set_data(G_OBJECT(mi), "ex-steps",
				  GINT_TO_POINTER(back ? -(int)(i + 1) : (int)(i + 1)));
		g_signal_connect(mi, "activate", G_CALLBACK(history_jump), t);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
		g_free(name);
	}
	g_ptr_array_unref(list);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), b, NULL);
	gtk_menu_popup_at_widget(GTK_MENU(menu), b, GDK_GRAVITY_SOUTH_WEST,
				 GDK_GRAVITY_NORTH_WEST, (GdkEvent *)ev);
	return TRUE;
}

static void close_clicked(GtkButton *b, ExTab *t)
{
	ex_window_close_tab(t->win, t);
}

static gboolean label_button(GtkWidget *w, GdkEventButton *ev, ExTab *t)
{
	if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
		ex_window_close_tab(t->win, t);
		return TRUE;
	}
	return FALSE;
}

static GtkWidget *make_label(ExTab *t)
{
	GtkWidget *eb = gtk_event_box_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic",
							 GTK_ICON_SIZE_MENU);

	t->label_icon = gtk_image_new();
	t->label_text = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(t->label_text), PANGO_ELLIPSIZE_END);
	gtk_label_set_width_chars(GTK_LABEL(t->label_text), 12);
	gtk_label_set_max_width_chars(GTK_LABEL(t->label_text), 24);
	gtk_label_set_xalign(GTK_LABEL(t->label_text), 0);
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(close, "Close tab (Ctrl+W)");
	atk_object_set_name(gtk_widget_get_accessible(close), "Close tab");
	g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), t);
	gtk_box_pack_start(GTK_BOX(box), t->label_icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), t->label_text, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(box), close, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(eb), box);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	g_signal_connect(eb, "button-press-event", G_CALLBACK(label_button), t);
	gtk_widget_show_all(eb);
	return eb;
}

static GtkWidget *build_navbar(ExTab *t)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	ex_style_add(bar, EX_CLASS_NAVBAR);
	t->back = nav_button("go-previous", "Back (Alt+Left)", "win.back");
	t->fwd = nav_button("go-next", "Forward (Alt+Right)", "win.forward");
	t->up = nav_button("go-up", "Up (Alt+Up)", "win.up");
	g_signal_connect(t->back, "button-press-event", G_CALLBACK(history_menu), t);
	g_signal_connect(t->fwd, "button-press-event", G_CALLBACK(history_menu), t);
	gtk_box_pack_start(GTK_BOX(bar), t->back, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), t->fwd, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), t->up, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar),
			   nav_button("view-refresh", "Refresh (F5)", "win.refresh"),
			   FALSE, FALSE, 0);

	t->crumb = lx_breadcrumb_new();
	gtk_widget_set_hexpand(t->crumb, TRUE);
	g_signal_connect(t->crumb, "crumb-activated", G_CALLBACK(crumb_activated), t);
	g_signal_connect(t->crumb, "text-activated", G_CALLBACK(text_activated), t);
	g_signal_connect(t->crumb, "children-wanted", G_CALLBACK(children_wanted), t);
	gtk_box_pack_start(GTK_BOX(bar), t->crumb, TRUE, TRUE, 4);

	t->search_entry = gtk_search_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(t->search_entry), 26);
	atk_object_set_name(gtk_widget_get_accessible(t->search_entry), "Search");
	g_signal_connect(t->search_entry, "search-changed", G_CALLBACK(search_changed),
			 t);
	g_signal_connect(t->search_entry, "stop-search", G_CALLBACK(search_stop), t);
	g_signal_connect(t->search_entry, "activate", G_CALLBACK(search_activate), t);
	gtk_box_pack_end(GTK_BOX(bar), t->search_entry, FALSE, FALSE, 0);
	return bar;
}

static GtkWidget *build_statusbar(ExTab *t)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	ex_style_add(bar, EX_CLASS_STATUSBAR);
	t->st_count = gtk_label_new("");
	t->st_sel = gtk_label_new("");
	t->spinner = gtk_spinner_new();
	gtk_widget_set_tooltip_text(t->spinner, "Working…");
	t->seg = lx_segmented_new();
	lx_segmented_append(LX_SEGMENTED(t->seg), "view-list", "Details");
	lx_segmented_append(LX_SEGMENTED(t->seg), "view-grid", "Large icons");
	g_signal_connect(t->seg, "changed", G_CALLBACK(seg_changed), t);
	gtk_box_pack_start(GTK_BOX(bar), t->st_count, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), t->st_sel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), t->spinner, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar), t->seg, FALSE, FALSE, 0);
	return bar;
}

static void root_destroyed(GtkWidget *w, ExTab *t)
{
	t->dead = TRUE;
	stop_everything(t);
	g_clear_object(&t->cancel);
	g_signal_handlers_disconnect_by_data(t->nav, t);
	g_signal_handlers_disconnect_by_data(t->sel, t);
	if (t->model)
		g_signal_handlers_disconnect_by_data(t->model, t);
	g_clear_object(&t->model);
	g_clear_object(&t->nav);
	g_clear_object(&t->sel);
	g_clear_pointer(&t->crumbs, g_ptr_array_unref);
	g_clear_pointer(&t->reselect, g_ptr_array_unref);
	g_clear_pointer(&t->shown, ex_location_unref);
	g_free(t->mode);
	g_free(t);
}

ExTab *ex_tab_new(ExWindow *win, ExLocation *loc)
{
	ExTab *t = g_new0(ExTab, 1);
	ExSettings *s = ex_settings_get();

	t->win = win;
	t->nav = ex_navigator_new(loc);
	t->sel = ex_selection_new();
	t->mode = g_strdup("details");
	t->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_object_set_data(G_OBJECT(t->root), "ex-tab", t);
	t->label = make_label(t);

	gtk_box_pack_start(GTK_BOX(t->root), build_navbar(t), FALSE, FALSE, 0);
	t->cmdbar = ex_commandbar_new();
	gtk_box_pack_start(GTK_BOX(t->root), t->cmdbar, FALSE, FALSE, 0);

	/* Navigation pane | content | right pane. */
	t->outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	t->sidebar = ex_sidebar_new();
	gtk_widget_set_size_request(t->sidebar, 160, -1);
	gtk_paned_pack1(GTK_PANED(t->outer), t->sidebar, FALSE, FALSE);
	gtk_paned_set_position(GTK_PANED(t->outer),
			       ex_settings_get_int(s, "sidebar-width"));
	t->inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	t->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(t->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(t->stack), 90);

	t->view = ex_file_view_new(t->sel);
	t->view_overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(t->view_overlay), t->view);
	t->empty_label = gtk_label_new("");
	ex_style_add(t->empty_label, EX_CLASS_PLACEHOLDER);
	gtk_widget_set_halign(t->empty_label, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(t->empty_label, GTK_ALIGN_START);
	gtk_widget_set_margin_top(t->empty_label, 48);
	gtk_widget_set_no_show_all(t->empty_label, TRUE);
	gtk_overlay_add_overlay(GTK_OVERLAY(t->view_overlay), t->empty_label);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(t->view_overlay),
					     t->empty_label, TRUE);
	g_signal_connect(t->view, "activate", G_CALLBACK(view_activate), t);
	g_signal_connect(t->view, "popup", G_CALLBACK(view_popup), t);
	g_signal_connect(t->view, "rename", G_CALLBACK(view_rename), t);
	g_signal_connect(t->view, "drop", G_CALLBACK(view_drop), t);
	g_signal_connect(t->view, "sort", G_CALLBACK(view_sort), t);
	g_signal_connect(t->view, "zoom", G_CALLBACK(view_zoom), t);

	t->home = ex_home_page_new(FALSE);
	t->thispc = ex_home_page_new(TRUE);
	g_signal_connect(t->home, "open", G_CALLBACK(home_open), t);
	g_signal_connect(t->thispc, "open", G_CALLBACK(home_open), t);
	g_signal_connect(t->home, "open-file", G_CALLBACK(home_open_file), t);
	g_signal_connect(t->home, "popup", G_CALLBACK(home_popup), t);
	g_signal_connect(t->home, "popup-location",
			 G_CALLBACK(home_popup_location), t);
	g_signal_connect(t->thispc, "popup-location",
			 G_CALLBACK(home_popup_location), t);

	t->msg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	t->msg_label = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(t->msg_label), TRUE);
	gtk_label_set_justify(GTK_LABEL(t->msg_label), GTK_JUSTIFY_CENTER);
	gtk_widget_set_valign(t->msg_box, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(t->msg_box),
			   gtk_image_new_from_icon_name("dialog-warning",
							GTK_ICON_SIZE_DIALOG),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(t->msg_box), t->msg_label, FALSE, FALSE, 0);

	gtk_stack_add_named(GTK_STACK(t->stack), t->view_overlay, "files");
	gtk_stack_add_named(GTK_STACK(t->stack), t->home, "home");
	gtk_stack_add_named(GTK_STACK(t->stack), t->thispc, "thispc");
	gtk_stack_add_named(GTK_STACK(t->stack), t->msg_box, "message");
	gtk_box_pack_start(GTK_BOX(t->inner), t->stack, TRUE, TRUE, 0);
	t->right = ex_preview_pane_new(t->sel);
	gtk_widget_set_no_show_all(t->right, TRUE);
	gtk_box_pack_end(GTK_BOX(t->inner), t->right, FALSE, FALSE, 0);
	gtk_paned_pack2(GTK_PANED(t->outer), t->inner, TRUE, FALSE);
	gtk_box_pack_start(GTK_BOX(t->root), t->outer, TRUE, TRUE, 0);

	g_signal_connect(t->sidebar, "open", G_CALLBACK(sidebar_open), t);
	g_signal_connect(t->sidebar, "drop", G_CALLBACK(sidebar_drop), t);
	g_signal_connect(t->sidebar, "popup", G_CALLBACK(sidebar_popup), t);

	gtk_box_pack_end(GTK_BOX(t->root), build_statusbar(t), FALSE, FALSE, 0);
	g_signal_connect(t->nav, "changed", G_CALLBACK(nav_changed), t);
	g_signal_connect(t->sel, "changed", G_CALLBACK(sel_changed), t);
	g_signal_connect(t->root, "destroy", G_CALLBACK(root_destroyed), t);

	gtk_widget_show_all(t->root);
	gtk_widget_set_no_show_all(t->spinner, TRUE);
	gtk_widget_hide(t->spinner);
	ex_tab_apply_settings(t);
	load_location(t);
	return t;
}

void ex_tab_destroy(ExTab *t)
{
	gtk_widget_destroy(t->root);
}
