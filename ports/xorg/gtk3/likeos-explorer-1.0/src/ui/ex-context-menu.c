// LikeOS -- File Explorer: context menus.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-context-menu.h"
#include "backend/ex-apps.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-volumes.h"
#include "backend/ex-util.h"
#include "services/ex-settings.h"
#include "ui/ex-share.h"

#include <string.h>

static void item(GMenu *m, const char *label, const char *action,
		 const char *icon)
{
	GMenuItem *it = g_menu_item_new(label, action);

	if (icon) {
		GIcon *gi = g_themed_icon_new(icon);

		g_menu_item_set_icon(it, gi);
		g_object_unref(gi);
	}
	g_menu_append_item(m, it);
	g_object_unref(it);
}

static GMenu *section(GMenu *m)
{
	GMenu *s = g_menu_new();

	g_menu_append_section(m, NULL, G_MENU_MODEL(s));
	g_object_unref(s);
	return s;
}

GMenuModel *ex_context_menu_new_items(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);
	const char *tdir = ex_user_dir(G_USER_DIRECTORY_TEMPLATES);

	item(s, "_Folder", "win.new-folder", "folder-new");
	s = section(m);
	item(s, "_Text document", "win.new-file::text", "text-x-generic");
	item(s, "_Empty file", "win.new-file::empty", "document-new");
	/* The Templates folder's files, each a kind of new document. */
	if (tdir) {
		GDir *d = g_dir_open(tdir, 0, NULL);
		const char *name;
		GMenu *ts = NULL;

		while (d && (name = g_dir_read_name(d))) {
			char *p = g_build_filename(tdir, name, NULL);

			if (name[0] != '.' && g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
				GMenuItem *it = g_menu_item_new(name, NULL);

				if (!ts)
					ts = section(m);
				g_menu_item_set_action_and_target(it, "win.new-file",
								  "s", p);
				g_menu_append_item(ts, it);
				g_object_unref(it);
			}
			g_free(p);
		}
		if (d)
			g_dir_close(d);
	}
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_view(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	item(s, "_Large icons", "win.view-mode::large-icons", NULL);
	item(s, "_Icons", "win.view-mode::icons", NULL);
	item(s, "_Tiles", "win.view-mode::tiles", NULL);
	item(s, "Li_st", "win.view-mode::list", NULL);
	item(s, "_Details", "win.view-mode::details", NULL);
	item(s, "_Compact", "win.view-mode::compact", NULL);
	s = section(m);
	item(s, "_Navigation pane", "win.navigation-pane", NULL);
	item(s, "D_etails pane", "win.right-pane::details", NULL);
	item(s, "_Preview pane", "win.right-pane::preview", NULL);
	s = section(m);
	item(s, "_Hidden items", "win.show-hidden", NULL);
	item(s, "File name e_xtensions", "win.show-extensions", NULL);
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_sort(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	item(s, "_Name", "win.sort-by::name", NULL);
	item(s, "Date _modified", "win.sort-by::modified", NULL);
	item(s, "_Type", "win.sort-by::type", NULL);
	item(s, "_Size", "win.sort-by::size", NULL);
	s = section(m);
	item(s, "_Ascending", "win.sort-order::ascending", NULL);
	item(s, "_Descending", "win.sort-order::descending", NULL);
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_more(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	item(s, "_Undo", "win.undo", "edit-undo");
	item(s, "_Redo", "win.redo", "edit-redo");
	s = section(m);
	item(s, "Select _all", "win.select-all", "edit-select-all");
	item(s, "Select _none", "win.select-none", NULL);
	item(s, "_Invert selection", "win.invert-selection", NULL);
	s = section(m);
	item(s, "_Pin to Quick access", "win.pin", "view-pin-symbolic");
	item(s, "Copy as _path", "win.copy-path", NULL);
	item(s, "Open in _Terminal", "win.open-terminal", "utilities-terminal");
	s = section(m);
	item(s, "_Operations", "win.operations", NULL);
	item(s, "Pr_operties", "win.properties", "document-properties");
	item(s, "_Options", "win.options", "preferences-system");
	return G_MENU_MODEL(m);
}

/* "Open with" for the selection's common type. */
static GMenuModel *open_with(ExTab *t)
{
	GMenu *m = g_menu_new();
	GPtrArray *items = ex_selection_get(ex_tab_selection(t));
	GPtrArray *types = g_ptr_array_new();
	const char *common;

	for (guint i = 0; i < items->len; i++)
		g_ptr_array_add(types, (gpointer)ex_file_get_content_type(
					       g_ptr_array_index(items, i)));
	common = ex_apps_common_type(types);
	g_ptr_array_unref(types);
	if (common) {
		GPtrArray *apps = ex_apps_for(common);
		GMenu *s = section(m);

		for (guint i = 0; i < apps->len; i++) {
			GAppInfo *a = g_ptr_array_index(apps, i);
			const char *id = g_app_info_get_id(a);
			GMenuItem *it;

			if (!id)
				continue;
			it = g_menu_item_new(g_app_info_get_display_name(a), NULL);
			g_menu_item_set_action_and_target(it, "win.open-with", "s",
							  id);
			if (g_app_info_get_icon(a))
				g_menu_item_set_icon(it, g_app_info_get_icon(a));
			g_menu_append_item(s, it);
			g_object_unref(it);
		}
		g_ptr_array_unref(apps);
	}
	item(section(m), "_Choose another app…", "win.open-with-other", NULL);
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_items(ExTab *t)
{
	GMenu *m = g_menu_new();
	ExSelection *sel = ex_tab_selection(t);
	gboolean trash = ex_location_kind(ex_tab_location(t)) == EX_LOC_TRASH;
	gboolean dirs = ex_selection_all_dirs(sel);
	GMenu *s;

	if (trash) {
		s = section(m);
		item(s, "_Restore", "win.restore", "edit-undo");
		item(s, "_Delete permanently", "win.delete", "edit-delete");
		s = section(m);
		item(s, "Pr_operties", "win.properties", "document-properties");
		return G_MENU_MODEL(m);
	}
	s = section(m);
	item(s, "_Open", "win.open", "document-open");
	if (dirs) {
		item(s, "Open in new _tab", "win.open-new-tab", "tab-new");
	} else {
		GMenuModel *ow = open_with(t);

		g_menu_append_submenu(s, "Open _with", ow);
		g_object_unref(ow);
	}
	s = section(m);
	item(s, "Cu_t", "win.cut", "edit-cut");
	item(s, "_Copy", "win.copy", "edit-copy");
	if (dirs && ex_selection_count(sel) == 1)
		g_menu_append(s, "_Paste into folder", "win.paste-into(true)");
	s = section(m);
	item(s, "Create _shortcut", "win.create-shortcut", NULL);
	item(s, "Rena_me", "win.rename", NULL);
	item(s, "_Delete", "win.trash", "user-trash");
	s = section(m);
	if (dirs) {
		ExFile *one = ex_selection_single(sel);
		gboolean pinned = one && ex_bookmarks_contains(
						 ex_bookmarks_get_default(),
						 ex_file_get_file(one));

		item(s, pinned ? "Unpin from _Quick access" : "Pin to _Quick access",
		     "win.pin", "view-pin-symbolic");
	} else {
		ExFile *one = ex_selection_single(sel);
		gboolean fav = one && ex_settings_is_favorite(ex_settings_get(),
							       ex_file_get_file(one));

		item(s, fav ? "Remove from _Favorites" : "Add to _Favorites",
		     "win.favorite", "starred");
	}
	item(s, "Copy as _path", "win.copy-path", NULL);
	item(s, "S_hare", "win.share", "emblem-shared");
	s = section(m);
	item(s, "Pr_operties", "win.properties", "document-properties");
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_background(ExTab *t)
{
	GMenu *m = g_menu_new();
	ExLocationKind kind = ex_location_kind(ex_tab_location(t));
	GMenuModel *sub;
	GMenu *s = section(m);

	sub = ex_context_menu_view();
	g_menu_append_submenu(s, "_View", sub);
	g_object_unref(sub);
	sub = ex_context_menu_sort();
	g_menu_append_submenu(s, "_Sort by", sub);
	g_object_unref(sub);
	item(s, "_Refresh", "win.refresh", "view-refresh");
	if (kind == EX_LOC_TRASH) {
		s = section(m);
		item(s, "_Empty Trash", "win.empty-trash", "user-trash");
		item(s, "Restore _all items", "win.restore-all", NULL);
		return G_MENU_MODEL(m);
	}
	if (kind == EX_LOC_FOLDER) {
		s = section(m);
		item(s, "_Paste", "win.paste", "edit-paste");
		item(s, "_Undo", "win.undo", "edit-undo");
		s = section(m);
		sub = ex_context_menu_new_items();
		g_menu_append_submenu(s, "_New", sub);
		g_object_unref(sub);
		s = section(m);
		item(s, "Open in _Terminal", "win.open-terminal",
		     "utilities-terminal");
		item(s, "Pin to _Quick access", "win.pin", "view-pin-symbolic");
		item(s, "Pr_operties", "win.properties", "document-properties");
	}
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_location(ExLocation *loc)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);
	const char *id = ex_location_id(loc);
	GFile *f = ex_location_file(loc);
	GMenuItem *it;

	it = g_menu_item_new("_Open", NULL);
	g_menu_item_set_action_and_target(it, "win.open-location", "s", id);
	g_menu_append_item(s, it);
	g_object_unref(it);
	it = g_menu_item_new("Open in new _tab", NULL);
	g_menu_item_set_action_and_target(it, "win.open-location-tab", "s", id);
	g_menu_append_item(s, it);
	g_object_unref(it);
	if (ex_location_kind(loc) == EX_LOC_TRASH) {
		item(section(m), "_Empty Trash", "win.empty-trash", "user-trash");
		return G_MENU_MODEL(m);
	}
	if (f && ex_bookmarks_contains(ex_bookmarks_get_default(), f)) {
		it = g_menu_item_new("Unpin from _Quick access", NULL);
		g_menu_item_set_action_and_target(it, "win.unpin", "s", id);
		g_menu_append_item(section(m), it);
		g_object_unref(it);
	} else if (f && ex_location_kind(loc) == EX_LOC_FOLDER) {
		it = g_menu_item_new("Pin to _Quick access", NULL);
		g_menu_item_set_action_and_target(it, "win.pin-location", "s", id);
		g_menu_append_item(section(m), it);
		g_object_unref(it);
	}
	const ExVolume *v = f ? ex_volumes_find(ex_volumes_get_default(), f) : NULL;
	if (v && v->mount && v->removable && g_file_equal(v->root, f)) {
		it = g_menu_item_new("_Eject", NULL);
		g_menu_item_set_action_and_target(it, "win.eject", "s", id);
		g_menu_append_item(section(m), it);
		g_object_unref(it);
	}
	if (f) {
		it = g_menu_item_new("Pr_operties", NULL);
		g_menu_item_set_action_and_target(it, "win.location-properties",
						  "s", id);
		g_menu_append_item(section(m), it);
		g_object_unref(it);
	}
	return G_MENU_MODEL(m);
}

static gboolean destroy_later(gpointer menu)
{
	gtk_widget_destroy(menu);
	return G_SOURCE_REMOVE;
}

static void deactivated(GtkMenu *menu, gpointer data)
{
	/* After the chosen item's action ran: "deactivate" comes first. */
	g_idle_add(destroy_later, menu);
}

void ex_context_menu_popup(GtkWidget *widget, GMenuModel *model,
			   GdkEventButton *ev)
{
	GtkWidget *menu = gtk_menu_new_from_model(model);

	gtk_menu_attach_to_widget(GTK_MENU(menu), widget, NULL);
	g_signal_connect(menu, "deactivate", G_CALLBACK(deactivated), NULL);
	if (ev)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	else
		gtk_menu_popup_at_widget(GTK_MENU(menu), widget, GDK_GRAVITY_CENTER,
					 GDK_GRAVITY_NORTH_WEST, NULL);
}
