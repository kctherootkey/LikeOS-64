// LikeOS Explorer: context menus.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-context-menu.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-apps.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-volumes.h"
#include "backend/ex-util.h"
#include "services/ex-settings.h"
#include "ui/ex-share.h"

#include <math.h>
#include <string.h>

/* Item attributes the LikeOS menu reads (a GtkMenu ignores them):
 * "ex-hint" is the key named at its right when it has no accelerator of
 * its own (Enter opens), and "ex-default" marks the command a double click
 * runs, shown in the strong weight. */
#define EX_ATTR_HINT "ex-hint"
#define EX_ATTR_DEFAULT "ex-default"

/* ---- the menu models --------------------------------------------------------- */

static void item_full(GMenu *m, const char *label, const char *action,
		      const char *icon, const char *fallback, gboolean dflt,
		      const char *hint)
{
	GMenuItem *it = g_menu_item_new(label, action);

	if (icon) {
		const char *names[] = { icon, fallback, NULL };
		GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);

		g_menu_item_set_icon(it, gi);
		g_object_unref(gi);
	}
	if (dflt)
		g_menu_item_set_attribute(it, EX_ATTR_DEFAULT, "b", TRUE);
	if (hint)
		g_menu_item_set_attribute(it, EX_ATTR_HINT, "s", hint);
	g_menu_append_item(m, it);
	g_object_unref(it);
}

static void item(GMenu *m, const char *label, const char *action,
		 const char *icon, const char *fallback)
{
	item_full(m, label, action, icon, fallback, FALSE, NULL);
}

static void submenu(GMenu *m, const char *label, GMenuModel *sub,
		    const char *icon, const char *fallback)
{
	GMenuItem *it = g_menu_item_new_submenu(label, sub);

	if (icon) {
		const char *names[] = { icon, fallback, NULL };
		GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);

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

	item(s, "_Folder", "win.new-folder", "likeos-new-folder", "folder-new");
	s = section(m);
	item(s, "_Text document", "win.new-file::text", "text-x-generic", NULL);
	item(s, "_Empty file", "win.new-file::empty", "likeos-new-file",
	     "document-new");
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
	GMenu *show = g_menu_new();

	item(s, "E_xtra large icons", "win.view-mode::extra-large-icons",
	     "likeos-large-icons-view", NULL);
	item(s, "_Large icons", "win.view-mode::large-icons",
	     "likeos-large-icons-view", NULL);
	item(s, "_Medium icons", "win.view-mode::icons", "likeos-icons-view",
	     NULL);
	item(s, "Small _icons", "win.view-mode::small-icons", "likeos-icons-view",
	     NULL);
	item(s, "Li_st", "win.view-mode::list", "likeos-list-view", NULL);
	item(s, "_Details", "win.view-mode::details", "likeos-details-view", NULL);
	item(s, "_Tiles", "win.view-mode::tiles", "likeos-tiles-view", NULL);
	s = section(m);
	item(s, "C_ompact view", "win.view-mode::compact", "likeos-list-view",
	     NULL);
	item(show, "_Navigation pane", "win.navigation-pane", NULL, NULL);
	item(show, "D_etails pane", "win.right-pane::details", NULL, NULL);
	item(show, "_Preview pane", "win.right-pane::preview", NULL, NULL);
	item(show, "File name e_xtensions", "win.show-extensions", NULL, NULL);
	item(show, "_Hidden items", "win.show-hidden", NULL, NULL);
	submenu(section(m), "_Show", G_MENU_MODEL(show), "likeos-view",
		"view-reveal-symbolic");
	g_object_unref(show);
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_sort(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	item(s, "_Name", "win.sort-by::name", NULL, NULL);
	item(s, "Date _modified", "win.sort-by::modified", NULL, NULL);
	item(s, "_Type", "win.sort-by::type", NULL, NULL);
	item(s, "_Size", "win.sort-by::size", NULL, NULL);
	s = section(m);
	item(s, "_Ascending", "win.sort-order::ascending", "likeos-chevron-up",
	     "pan-up-symbolic");
	item(s, "_Descending", "win.sort-order::descending", "likeos-chevron-down",
	     "pan-down-symbolic");
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_more(void)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);

	item(s, "_Undo", "win.undo", "likeos-undo", "edit-undo");
	item(s, "_Redo", "win.redo", "likeos-redo", "edit-redo");
	s = section(m);
	item(s, "Select _all", "win.select-all", "likeos-select-all",
	     "edit-select-all");
	item(s, "Select _none", "win.select-none", "likeos-select-none", NULL);
	item(s, "_Invert selection", "win.invert-selection",
	     "likeos-invert-selection", NULL);
	s = section(m);
	item(s, "_Pin", "win.pin", "likeos-pin", "view-pin-symbolic");
	item(s, "Copy as _path", "win.copy-path", "likeos-copy-path", NULL);
	item(s, "Open in _Terminal", "win.open-terminal", "likeos-terminal",
	     "utilities-terminal");
	s = section(m);
	item(s, "_Operations", "win.operations", NULL, NULL);
	item(s, "Pr_operties", "win.properties", "likeos-properties",
	     "document-properties");
	item(s, "Explorer _settings", "win.options", "likeos-settings",
	     "preferences-system");
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
	item(section(m), "_Choose another app…", "win.open-with-other",
	     "likeos-open-with", NULL);
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_items(ExTab *t)
{
	GMenu *m = g_menu_new();
	ExSelection *sel = ex_tab_selection(t);
	gboolean trash = ex_location_kind(ex_tab_location(t)) == EX_LOC_TRASH;
	gboolean dirs = ex_selection_all_dirs(sel);
	ExFile *one = ex_selection_single(sel);
	GMenu *s;

	if (trash) {
		s = section(m);
		item(s, "_Restore", "win.restore", "likeos-restore", "edit-undo");
		item(s, "_Delete permanently", "win.delete",
		     "likeos-delete-permanently", "edit-delete");
		s = section(m);
		item(s, "Pr_operties", "win.properties", "likeos-properties",
		     "document-properties");
		return G_MENU_MODEL(m);
	}
	/* Opening first, the command a double click runs in the strong
	 * weight; then the clipboard; then what changes the item; the
	 * picture commands; keeping it at hand; and its properties. */
	s = section(m);
	item_full(s, "_Open", "win.open", "likeos-open", "document-open", TRUE,
		  "Enter");
	if (dirs) {
		item(s, "Open in new _tab", "win.open-new-tab", "likeos-new-tab",
		     "tab-new");
	} else {
		GMenuModel *ow = open_with(t);

		submenu(s, "Open _with", ow, "likeos-open-with", NULL);
		g_object_unref(ow);
	}
	s = section(m);
	item(s, "Cu_t", "win.cut", "likeos-cut", "edit-cut");
	item(s, "_Copy", "win.copy", "likeos-copy", "edit-copy");
	if (dirs && ex_selection_count(sel) == 1)
		item(s, "_Paste into folder", "win.paste-into(true)", "likeos-paste",
		     "edit-paste");
	item(s, "Copy as _path", "win.copy-path", "likeos-copy-path", NULL);
	s = section(m);
	item(s, "Rena_me", "win.rename", "likeos-rename", NULL);
	item(s, "_Delete", "win.trash", "likeos-delete", "user-trash");
	item(s, "Create _shortcut", "win.create-shortcut",
	     "likeos-create-shortcut", NULL);
	if (ex_tab_selection_is_images(t)) {
		s = section(m);
		if (one)
			item(s, "Set as desktop _background", "win.set-wallpaper",
			     "likeos-wallpaper", NULL);
		item(s, "Rotate _right", "win.rotate-right", "likeos-rotate-right",
		     "object-rotate-right");
		item(s, "Rotate _left", "win.rotate-left", "likeos-rotate-left",
		     "object-rotate-left");
	}
	s = section(m);
	if (dirs) {
		gboolean pinned = one && ex_bookmarks_contains(
						 ex_bookmarks_get_default(),
						 ex_file_get_file(one));

		if (pinned)
			item(s, "Un_pin", "win.pin", "likeos-unpin",
			     "view-pin-symbolic");
		else
			item(s, "_Pin", "win.pin", "likeos-pin",
			     "view-pin-symbolic");
	} else {
		gboolean fav = one && ex_settings_is_favorite(ex_settings_get(),
							       ex_file_get_file(one));

		item(s, fav ? "Remove from _Favorites" : "Add to _Favorites",
		     "win.favorite", "likeos-favorite", "starred");
	}
	item(s, "S_hare", "win.share", "likeos-share", "emblem-shared");
	s = section(m);
	item(s, "Pr_operties", "win.properties", "likeos-properties",
	     "document-properties");
	return G_MENU_MODEL(m);
}

GMenuModel *ex_context_menu_background(ExTab *t)
{
	GMenu *m = g_menu_new();
	ExLocationKind kind = ex_location_kind(ex_tab_location(t));
	GMenuModel *sub;
	GMenu *s = section(m);

	sub = ex_context_menu_view();
	submenu(s, "_View", sub, "likeos-view", NULL);
	g_object_unref(sub);
	sub = ex_context_menu_sort();
	submenu(s, "_Sort by", sub, "likeos-sort", NULL);
	g_object_unref(sub);
	item(s, "_Refresh", "win.refresh", "likeos-refresh", "view-refresh");
	if (kind == EX_LOC_TRASH) {
		s = section(m);
		item(s, "_Empty Trash", "win.empty-trash", "likeos-empty-trash",
		     "user-trash");
		item(s, "Restore _all items", "win.restore-all", "likeos-restore",
		     NULL);
		return G_MENU_MODEL(m);
	}
	if (kind == EX_LOC_FOLDER) {
		s = section(m);
		item(s, "_Paste", "win.paste", "likeos-paste", "edit-paste");
		item(s, "_Undo", "win.undo", "likeos-undo", "edit-undo");
		s = section(m);
		sub = ex_context_menu_new_items();
		submenu(s, "_New", sub, "likeos-new", "list-add");
		g_object_unref(sub);
		s = section(m);
		item(s, "_Pin", "win.pin", "likeos-pin", "view-pin-symbolic");
		item(s, "Open in _Terminal", "win.open-terminal",
		     "likeos-terminal", "utilities-terminal");
		item(s, "Pr_operties", "win.properties", "likeos-properties",
		     "document-properties");
	}
	return G_MENU_MODEL(m);
}

static void target_item(GMenu *m, const char *label, const char *action,
			const char *id, const char *icon, const char *fallback)
{
	GMenuItem *it = g_menu_item_new(label, NULL);

	g_menu_item_set_action_and_target(it, action, "s", id);
	if (icon) {
		const char *names[] = { icon, fallback, NULL };
		GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);

		g_menu_item_set_icon(it, gi);
		g_object_unref(gi);
	}
	g_menu_append_item(m, it);
	g_object_unref(it);
}

GMenuModel *ex_context_menu_location(ExLocation *loc)
{
	GMenu *m = g_menu_new();
	GMenu *s = section(m);
	const char *id = ex_location_id(loc);
	GFile *f = ex_location_file(loc);

	target_item(s, "_Open", "win.open-location", id, "likeos-open",
		    "document-open");
	target_item(s, "Open in new _tab", "win.open-location-tab", id,
		    "likeos-new-tab", "tab-new");
	if (ex_location_kind(loc) == EX_LOC_TRASH) {
		item(section(m), "_Empty Trash", "win.empty-trash",
		     "likeos-empty-trash", "user-trash");
		return G_MENU_MODEL(m);
	}
	if (f && ex_bookmarks_contains(ex_bookmarks_get_default(), f))
		target_item(section(m), "Un_pin", "win.unpin", id,
			    "likeos-unpin", "view-pin-symbolic");
	else if (f && ex_location_kind(loc) == EX_LOC_FOLDER)
		target_item(section(m), "_Pin", "win.pin-location", id,
			    "likeos-pin", "view-pin-symbolic");
	const ExVolume *v = f ? ex_volumes_find(ex_volumes_get_default(), f) : NULL;
	if (v && v->mount && v->removable && g_file_equal(v->root, f))
		target_item(section(m), "_Eject", "win.eject", id, "likeos-eject",
			    "media-eject");
	if (f)
		target_item(section(m), "Pr_operties", "win.location-properties",
			    id, "likeos-properties", "document-properties");
	return G_MENU_MODEL(m);
}

/* ---- the GtkMenu fallback --------------------------------------------------------- */

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

static GtkWidget *fallback_menu(GtkWidget *widget, GMenuModel *model)
{
	GtkWidget *menu = gtk_menu_new_from_model(model);

	gtk_menu_attach_to_widget(GTK_MENU(menu), widget, NULL);
	g_signal_connect(menu, "deactivate", G_CALLBACK(deactivated), NULL);
	return menu;
}

/* ---- the LikeOS menu -----------------------------------------------------------------
 *
 * One conventional menu: a white panel with rounded corners and a hairline
 * edge, the commands 30 pixels tall, each with its picture at the left, its
 * name, its key at the right in the secondary colour and a chevron where a
 * submenu opens, in groups between hairlines.  The panel is a window of its
 * own so that its corners can be cut round without a compositor and its
 * rows drawn as the theme's menu rows are.
 *
 * While it is open it holds the pointer and the keyboard (one grab, on the
 * first panel): every event comes to that panel, and the panel decides
 * from the pointer's place on the screen which panel and which command it
 * is over -- the panels of submenus get no events of their own.  The
 * command under the pointer, or chosen with the arrow keys, is drawn as
 * hovered.  A click outside every panel closes them all. */

/* How long the pointer rests on a command before its submenu opens (or
 * another one closes), and how long after opening a release still belongs
 * to the click that opened the menu. */
#define EX_SUBMENU_MS 300
#define EX_RELEASE_GRACE_US (400 * 1000)
/* The panel's corner radius, in pixels (the theme draws the same). */
#define EX_MENU_RADIUS 10

typedef struct _Ctx Ctx;
typedef struct _Panel Panel;

typedef struct {
	Panel *panel;
	GtkWidget *w;
	char *action; /* "win.copy" */
	GVariant *target;
	GMenuModel *submenu;
	gunichar mnemonic;
} Row;

struct _Panel {
	Ctx *ctx;
	GtkWidget *win;
	GPtrArray *rows; /* Row: the commands, top to bottom */
	int cur; /* the hovered or chosen row, -1 for none */
	Panel *parent, *child;
	Row *opener; /* the parent's row this submenu belongs to */
	guint timer;
	int x, y, w, h; /* on the screen */
};

struct _Ctx {
	GtkWidget *attach;
	GtkWidget *toplevel;
	GMenuModel *model;
	Panel *root;
	GdkSeat *seat;
	gboolean pressed; /* a button went down while the menu was open */
	gboolean closed;
	gint64 opened_us;
};

static void panel_free(Panel *p);

static void row_free(Row *r)
{
	g_free(r->action);
	if (r->target)
		g_variant_unref(r->target);
	g_clear_object(&r->submenu);
	g_free(r);
}

/* The action group an item's action lives in ("win." the window's, "app."
 * the application's) and its name there. */
static GActionGroup *group_for(Ctx *c, const char *action, const char **name)
{
	if (!action)
		return NULL;
	if (g_str_has_prefix(action, "win.") && G_IS_ACTION_GROUP(c->toplevel)) {
		*name = action + 4;
		return G_ACTION_GROUP(c->toplevel);
	}
	if (g_str_has_prefix(action, "app.") && GTK_IS_WINDOW(c->toplevel)) {
		GtkApplication *app = gtk_window_get_application(
			GTK_WINDOW(c->toplevel));

		*name = action + 4;
		return app ? G_ACTION_GROUP(app) : NULL;
	}
	return NULL;
}

static gboolean action_enabled(Ctx *c, const char *action)
{
	const char *name = NULL;
	GActionGroup *g = group_for(c, action, &name);

	return g && g_action_group_has_action(g, name) &&
	       g_action_group_get_action_enabled(g, name);
}

/* A stateful action: 1 when on (the current choice of a radio group, or a
 * toggle that is on), 0 when off, -1 when it has no state. */
static int action_checked(Ctx *c, const char *action, GVariant *target,
			  gboolean *radio)
{
	const char *name = NULL;
	GActionGroup *g = group_for(c, action, &name);
	GVariant *st;
	int on = -1;

	*radio = FALSE;
	if (!g || !g_action_group_has_action(g, name))
		return -1;
	st = g_action_group_get_action_state(g, name);
	if (!st)
		return -1;
	if (target && g_variant_is_of_type(st, g_variant_get_type(target))) {
		*radio = TRUE;
		on = g_variant_equal(st, target);
	} else if (g_variant_is_of_type(st, G_VARIANT_TYPE_BOOLEAN)) {
		on = g_variant_get_boolean(st);
	}
	g_variant_unref(st);
	return on;
}

/* The label without its mnemonic underscore, and the mnemonic. */
static char *strip_mnemonic(const char *label, gunichar *mnemonic)
{
	GString *out = g_string_new(NULL);

	*mnemonic = 0;
	for (const char *p = label; *p; p = g_utf8_next_char(p)) {
		if (*p == '_' && p[1] == '_') {
			g_string_append_c(out, '_');
			p++;
		} else if (*p == '_' && p[1]) {
			if (!*mnemonic)
				*mnemonic = g_unichar_tolower(
					g_utf8_get_char(p + 1));
		} else {
			g_string_append_unichar(out, g_utf8_get_char(p));
		}
	}
	return g_string_free(out, FALSE);
}

/* A key as the menu names it: "Ctrl+Shift+C", "Alt+Enter", "F2". */
static char *key_text(const char *accel)
{
	guint key = 0;
	GdkModifierType mods = 0;
	GString *out;
	char *k;

	gtk_accelerator_parse(accel, &key, &mods);
	if (!key)
		return NULL;
	out = g_string_new(NULL);
	if (mods & GDK_CONTROL_MASK)
		g_string_append(out, "Ctrl+");
	if (mods & GDK_SHIFT_MASK)
		g_string_append(out, "Shift+");
	if (mods & GDK_MOD1_MASK)
		g_string_append(out, "Alt+");
	if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
		g_string_append(out, "Enter");
	} else {
		k = gtk_accelerator_get_label(gdk_keyval_to_upper(key), 0);
		g_string_append(out, k);
		g_free(k);
	}
	return g_string_free(out, FALSE);
}

/* The key named at an item's right: its own hint, else the first
 * accelerator the application has for its action. */
static char *item_hint(Ctx *c, GMenuModel *m, int i, const char *action,
		       GVariant *target)
{
	char *hint = NULL;
	GtkApplication *app;
	char *detailed, **accels;

	if (g_menu_model_get_item_attribute(m, i, EX_ATTR_HINT, "s", &hint))
		return hint;
	if (!action || !GTK_IS_WINDOW(c->toplevel))
		return NULL;
	app = gtk_window_get_application(GTK_WINDOW(c->toplevel));
	if (!app)
		return NULL;
	detailed = g_action_print_detailed_name(action, target);
	accels = gtk_application_get_accels_for_action(app, detailed);
	if (accels && accels[0])
		hint = key_text(accels[0]);
	g_strfreev(accels);
	g_free(detailed);
	return hint;
}

static GtkWidget *themed_image(const char *name, const char *fallback,
			       int size)
{
	const char *names[] = { name, fallback, NULL };
	GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);
	GtkWidget *img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_MENU);

	gtk_image_set_pixel_size(GTK_IMAGE(img), size);
	g_object_unref(gi);
	return img;
}

/* An item's picture, 16 pixels: a full-colour icon of the theme. */
static GtkWidget *item_image(GtkWidget *ref, GIcon *icon)
{
	int scale = gtk_widget_get_scale_factor(ref);
	GdkPixbuf *pb = ex_icons_gicon(icon, 16, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	return img;
}

static Row *add_row(Panel *p, GtkWidget *w, const char *action,
		    GVariant *target, GMenuModel *sub, gunichar mnemonic)
{
	Row *r = g_new0(Row, 1);

	r->panel = p;
	r->w = w;
	r->action = g_strdup(action);
	r->target = target ? g_variant_ref(target) : NULL;
	r->submenu = sub ? g_object_ref(sub) : NULL;
	r->mnemonic = mnemonic;
	g_ptr_array_add(p->rows, r);
	return r;
}

/* A command: picture (or the mark of a choice), name, key, chevron. */
static GtkWidget *command_widget(Panel *p, const char *text, GIcon *icon,
				 int checked, gboolean radio, const char *hint,
				 gboolean has_sub, gboolean dflt)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *l = gtk_label_new(text);

	gtk_widget_set_size_request(slot, 16, 16);
	gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
	if (checked == 1 && radio) {
		GtkWidget *dot = gtk_label_new("●");

		ex_style_add(dot, "ex-menu-dot");
		gtk_box_pack_start(GTK_BOX(slot), dot, TRUE, TRUE, 0);
	} else if (checked == 1) {
		gtk_box_pack_start(GTK_BOX(slot),
				   themed_image("likeos-check",
						"object-select-symbolic", 16),
				   TRUE, TRUE, 0);
	} else if (icon) {
		/* The choices not taken show their pictures. */
		gtk_box_pack_start(GTK_BOX(slot), item_image(p->win, icon), TRUE,
				   TRUE, 0);
	}
	gtk_box_pack_start(GTK_BOX(row), slot, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_hexpand(l, TRUE);
	ex_style_add(l, "ex-menu-label");
	if (dflt)
		ex_style_add(l, "ex-menu-default");
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	if (hint) {
		GtkWidget *h = gtk_label_new(hint);

		ex_style_add(h, "ex-menu-hint");
		gtk_box_pack_start(GTK_BOX(row), h, FALSE, FALSE, 0);
	}
	if (has_sub) {
		GtkWidget *chev = themed_image("likeos-chevron-right",
					       "pan-end-symbolic", 12);

		ex_style_add(chev, "ex-menu-chevron");
		gtk_box_pack_start(GTK_BOX(row), chev, FALSE, FALSE, 0);
	}
	ex_style_add(row, "ex-menu-item");
	atk_object_set_role(gtk_widget_get_accessible(row), ATK_ROLE_MENU_ITEM);
	atk_object_set_name(gtk_widget_get_accessible(row), text);
	return row;
}

static GtkWidget *separator(void)
{
	GtkWidget *s = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	ex_style_add(s, "ex-menu-separator");
	return s;
}

typedef struct {
	Panel *p;
	GtkWidget *list;
	gboolean want_sep; /* a new group starts: a hairline before it */
} Build;

static void build_items(Build *b, GMenuModel *m)
{
	int n = g_menu_model_get_n_items(m);

	for (int i = 0; i < n; i++) {
		GMenuModel *sec = g_menu_model_get_item_link(m, i,
							     G_MENU_LINK_SECTION);
		GMenuModel *sub;
		char *label = NULL, *action = NULL, *text, *hint;
		GVariant *target, *iv;
		GIcon *icon = NULL;
		gunichar mn;
		gboolean enabled, radio, dflt = FALSE;
		int checked;
		GtkWidget *w;

		if (sec) {
			b->want_sep = TRUE;
			build_items(b, sec);
			b->want_sep = TRUE;
			g_object_unref(sec);
			continue;
		}
		if (!g_menu_model_get_item_attribute(m, i, G_MENU_ATTRIBUTE_LABEL,
						     "s", &label))
			continue;
		g_menu_model_get_item_attribute(m, i, G_MENU_ATTRIBUTE_ACTION, "s",
						&action);
		g_menu_model_get_item_attribute(m, i, EX_ATTR_DEFAULT, "b", &dflt);
		target = g_menu_model_get_item_attribute_value(
			m, i, G_MENU_ATTRIBUTE_TARGET, NULL);
		iv = g_menu_model_get_item_attribute_value(m, i,
							   G_MENU_ATTRIBUTE_ICON,
							   NULL);
		if (iv) {
			icon = g_icon_deserialize(iv);
			g_variant_unref(iv);
		}
		sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
		text = strip_mnemonic(label, &mn);
		enabled = sub ? g_menu_model_get_n_items(sub) > 0 :
				action_enabled(b->p->ctx, action);
		if (b->want_sep && b->p->rows->len)
			gtk_box_pack_start(GTK_BOX(b->list), separator(), FALSE,
					   FALSE, 0);
		b->want_sep = FALSE;
		checked = action_checked(b->p->ctx, action, target, &radio);
		hint = sub ? NULL : item_hint(b->p->ctx, m, i, action, target);
		w = command_widget(b->p, text, icon, checked, radio, hint,
				   sub != NULL, dflt);
		gtk_widget_set_sensitive(w, enabled);
		gtk_box_pack_start(GTK_BOX(b->list), w, FALSE, FALSE, 0);
		add_row(b->p, w, action, target, sub, mn);
		g_free(hint);
		g_free(text);
		g_free(label);
		g_free(action);
		if (target)
			g_variant_unref(target);
		g_clear_object(&icon);
		g_clear_object(&sub);
	}
}

/* Round corners without a compositor: the window's shape is a rounded
 * rectangle, row by row in the corners. */
static void shape_rounded(GtkWidget *w, GdkRectangle *a, gpointer data)
{
	cairo_region_t *r = cairo_region_create();
	int R = MIN(EX_MENU_RADIUS, MIN(a->width, a->height) / 2);
	cairo_rectangle_int_t mid = { 0, R, a->width, a->height - 2 * R };

	for (int y = 0; y < R; y++) {
		double dy = R - y - 0.5;
		int in = R - (int)floor(sqrt((double)R * R - dy * dy) + 0.5);
		cairo_rectangle_int_t top = { in, y, a->width - 2 * in, 1 };
		cairo_rectangle_int_t bot = { in, a->height - 1 - y,
					      a->width - 2 * in, 1 };

		cairo_region_union_rectangle(r, &top);
		cairo_region_union_rectangle(r, &bot);
	}
	cairo_region_union_rectangle(r, &mid);
	gtk_widget_shape_combine_region(w, r);
	cairo_region_destroy(r);
}

static Panel *panel_new(Ctx *c, GMenuModel *model)
{
	Panel *p = g_new0(Panel, 1);
	Build b = { p, NULL, FALSE };

	p->ctx = c;
	p->cur = -1;
	p->rows = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	p->win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_type_hint(GTK_WINDOW(p->win),
				 GDK_WINDOW_TYPE_HINT_POPUP_MENU);
	gtk_window_set_screen(GTK_WINDOW(p->win),
			      gtk_widget_get_screen(c->attach));
	if (GTK_IS_WINDOW(c->toplevel))
		gtk_window_set_transient_for(GTK_WINDOW(p->win),
					     GTK_WINDOW(c->toplevel));
	gtk_window_set_resizable(GTK_WINDOW(p->win), FALSE);
	ex_style_add(p->win, "ex-menu");
	atk_object_set_role(gtk_widget_get_accessible(p->win),
			    ATK_ROLE_POPUP_MENU);
	b.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	build_items(&b, model);
	gtk_container_add(GTK_CONTAINER(p->win), b.list);
	gtk_widget_show_all(b.list);
	g_signal_connect(p->win, "size-allocate", G_CALLBACK(shape_rounded),
			 NULL);
	return p;
}

/* Where a panel goes: at (x, y) -- for a submenu beside its row, `alt_x'
 * the place on the other side -- kept on the monitor's work area, flipped
 * to the left or upwards where it does not fit. */
static void panel_place(Panel *p, int x, int y, int alt_x, int alt_y)
{
	GdkDisplay *d = gtk_widget_get_display(p->win);
	GdkMonitor *mon = gdk_display_get_monitor_at_point(d, x, y);
	GdkRectangle wa = { 0, 0, 1 << 15, 1 << 15 };
	GtkRequisition req;

	if (mon)
		gdk_monitor_get_workarea(mon, &wa);
	gtk_widget_get_preferred_size(p->win, NULL, &req);
	p->w = req.width;
	p->h = req.height;
	if (x + p->w > wa.x + wa.width)
		x = alt_x - p->w;
	if (y + p->h > wa.y + wa.height)
		y = alt_y >= 0 ? alt_y - p->h : wa.y + wa.height - p->h;
	x = MAX(wa.x, MIN(x, wa.x + wa.width - p->w));
	y = MAX(wa.y, MIN(y, wa.y + wa.height - p->h));
	p->x = x;
	p->y = y;
	gtk_window_move(GTK_WINDOW(p->win), x, y);
}

/* ---- hovering and choosing ----------------------------------------------------- */

static Row *row_at(Panel *p, int i)
{
	return i >= 0 && i < (int)p->rows->len ? g_ptr_array_index(p->rows, i) :
						 NULL;
}

static void set_current(Panel *p, int i)
{
	Row *was = row_at(p, p->cur), *now = row_at(p, i);

	if (was == now)
		return;
	if (was)
		gtk_widget_unset_state_flags(was->w, GTK_STATE_FLAG_PRELIGHT);
	if (now)
		gtk_widget_set_state_flags(now->w, GTK_STATE_FLAG_PRELIGHT, FALSE);
	p->cur = now ? i : -1;
}

static void close_child(Panel *p)
{
	if (!p->child)
		return;
	panel_free(p->child);
	p->child = NULL;
}

static void open_child(Panel *p, Row *r, gboolean select_first);
static void move(Panel *p, int dir);

static gboolean hover_timer(gpointer data)
{
	Panel *p = data;
	Row *r = row_at(p, p->cur);

	p->timer = 0;
	if (p->child && (!r || p->child->opener != r))
		close_child(p);
	if (r && r->submenu && !p->child && gtk_widget_is_sensitive(r->w))
		open_child(p, r, FALSE);
	return G_SOURCE_REMOVE;
}

static void hover(Panel *p, int i)
{
	if (p->cur == i)
		return;
	set_current(p, i);
	if (p->timer)
		g_source_remove(p->timer);
	p->timer = g_timeout_add(EX_SUBMENU_MS, hover_timer, p);
}

static Panel *deepest(Ctx *c)
{
	Panel *p = c->root;

	while (p && p->child)
		p = p->child;
	return p;
}

/* The panel and row at a point of the screen. */
static Panel *hit(Ctx *c, int x, int y, int *row)
{
	*row = -1;
	for (Panel *p = deepest(c); p; p = p->parent) {
		if (x < p->x || y < p->y || x >= p->x + p->w || y >= p->y + p->h)
			continue;
		for (guint i = 0; i < p->rows->len; i++) {
			Row *r = g_ptr_array_index(p->rows, i);
			GtkAllocation a;
			int rx, ry;

			if (!gtk_widget_translate_coordinates(r->w, p->win, 0, 0,
							      &rx, &ry))
				continue;
			gtk_widget_get_allocation(r->w, &a);
			if (x >= p->x + rx && x < p->x + rx + a.width &&
			    y >= p->y + ry && y < p->y + ry + a.height) {
				*row = (int)i;
				break;
			}
		}
		return p;
	}
	return NULL;
}

static void close_all(Ctx *c)
{
	if (c->closed)
		return;
	c->closed = TRUE;
	gdk_seat_ungrab(c->seat);
	/* The root panel's window owns the context: freed with it. */
	gtk_widget_destroy(c->root->win);
}

static void activate(Ctx *c, Panel *p, Row *r)
{
	if (!gtk_widget_is_sensitive(r->w))
		return;
	if (r->submenu) {
		if (!p->child || p->child->opener != r) {
			close_child(p);
			open_child(p, r, TRUE);
		} else if (p->child->cur < 0) {
			/* Opened by the pointer resting on it: the keyboard
			 * goes on in it from its first command. */
			move(p->child, 1);
		}
		return;
	}
	const char *name = NULL;
	GActionGroup *g = group_for(c, r->action, &name);
	char *action = g_strdup(name);
	GVariant *target = r->target ? g_variant_ref(r->target) : NULL;

	if (g)
		g_object_ref(g);
	/* Closed before the action runs: it may open a window or a dialog,
	 * which must not find the pointer held. */
	close_all(c);
	if (g && action)
		g_action_group_activate_action(g, action, target);
	if (target)
		g_variant_unref(target);
	g_free(action);
	g_clear_object(&g);
}

static void open_child(Panel *p, Row *r, gboolean select_first)
{
	Panel *ch = panel_new(p->ctx, r->submenu);
	GtkAllocation a;
	int rx = 0, ry = 0;

	gtk_widget_translate_coordinates(r->w, p->win, 0, 0, &rx, &ry);
	gtk_widget_get_allocation(r->w, &a);
	ch->parent = p;
	ch->opener = r;
	p->child = ch;
	/* Beside the row, its first command level with it; on the left of
	 * the panel where the right has no room. */
	panel_place(ch, p->x + p->w - 4, p->y + ry - 5, p->x + 4, -1);
	gtk_widget_show(ch->win);
	if (select_first) {
		for (guint i = 0; i < ch->rows->len; i++)
			if (gtk_widget_is_sensitive(
				    ((Row *)g_ptr_array_index(ch->rows, i))->w)) {
				set_current(ch, (int)i);
				break;
			}
	}
}

/* ---- the keyboard ------------------------------------------------------------------- */

static gboolean selectable(Panel *p, int i)
{
	Row *r = row_at(p, i);

	return r && gtk_widget_is_sensitive(r->w);
}

/* Up and Down, over the commands that apply, round the ends. */
static void move(Panel *p, int dir)
{
	int n = (int)p->rows->len;
	int i = p->cur;

	if (!n)
		return;
	if (i < 0)
		i = dir > 0 ? -1 : n;
	for (int k = 0; k < n; k++) {
		i += dir;
		if (i < 0)
			i = n - 1;
		else if (i >= n)
			i = 0;
		if (selectable(p, i)) {
			set_current(p, i);
			return;
		}
	}
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, Ctx *c)
{
	Panel *p = deepest(c);
	Row *r;

	if (c->closed || !p)
		return TRUE;
	r = row_at(p, p->cur);
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		if (p->parent)
			close_child(p->parent);
		else
			close_all(c);
		return TRUE;
	case GDK_KEY_Menu:
		close_all(c);
		return TRUE;
	case GDK_KEY_F10:
		if (ev->state & GDK_SHIFT_MASK)
			close_all(c);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		move(p, -1);
		return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
	case GDK_KEY_Tab:
		move(p, 1);
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
		if (p->parent)
			close_child(p->parent);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
		if (r && r->submenu)
			activate(c, p, r);
		return TRUE;
	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		set_current(p, -1);
		move(p, 1);
		return TRUE;
	case GDK_KEY_End:
	case GDK_KEY_KP_End:
		set_current(p, -1);
		move(p, -1);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_ISO_Enter:
	case GDK_KEY_space:
	case GDK_KEY_KP_Space:
		if (r)
			activate(c, p, r);
		return TRUE;
	default:
		break;
	}
	/* A letter: the command it is the access key of. */
	gunichar ch = g_unichar_tolower(gdk_keyval_to_unicode(ev->keyval));

	if (ch && !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
		for (guint i = 0; i < p->rows->len; i++) {
			Row *x = g_ptr_array_index(p->rows, i);

			if (x->mnemonic == ch && gtk_widget_is_sensitive(x->w)) {
				set_current(p, (int)i);
				activate(c, p, x);
				break;
			}
		}
	}
	return TRUE;
}

/* ---- the pointer ---------------------------------------------------------------------- */

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, Ctx *c)
{
	int row;
	Panel *p;

	if (c->closed)
		return TRUE;
	p = hit(c, (int)ev->x_root, (int)ev->y_root, &row);
	if (p) {
		/* Back in a parent panel: the submenu stays while the pointer
		 * is on its row. */
		hover(p, row);
	} else {
		Panel *d = deepest(c);

		/* Outside every panel: nothing hovered, but a submenu's
		 * row stays marked while it is open. */
		if (d && !d->child)
			set_current(d, -1);
	}
	return TRUE;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, Ctx *c)
{
	int row;

	if (c->closed || ev->type != GDK_BUTTON_PRESS)
		return TRUE;
	if (!hit(c, (int)ev->x_root, (int)ev->y_root, &row)) {
		close_all(c);
		return TRUE;
	}
	c->pressed = TRUE;
	return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, Ctx *c)
{
	int row;
	Panel *p;
	Row *r;

	if (c->closed)
		return TRUE;
	p = hit(c, (int)ev->x_root, (int)ev->y_root, &row);
	r = p ? row_at(p, row) : NULL;
	/* The release of the click that opened the menu is not a choice,
	 * unless the button was held and moved onto a command. */
	if (!c->pressed &&
	    g_get_monotonic_time() - c->opened_us < EX_RELEASE_GRACE_US)
		return TRUE;
	c->pressed = FALSE;
	if (r)
		activate(c, p, r);
	return TRUE;
}

/* ---- life cycle ------------------------------------------------------------------------- */

static void panel_free(Panel *p)
{
	close_child(p);
	if (p->timer)
		g_source_remove(p->timer);
	p->timer = 0;
	if (p->parent) {
		/* A submenu: its window goes with it.  The root panel's
		 * window is destroyed by close_all(), which frees this. */
		gtk_widget_destroy(p->win);
	}
	g_ptr_array_unref(p->rows);
	g_free(p);
}

static void root_destroyed(GtkWidget *w, Ctx *c)
{
	c->closed = TRUE;
	panel_free(c->root);
	g_object_unref(c->model);
	g_object_unref(c->attach);
	g_free(c);
}

/* The widget's place on the screen. */
static void widget_origin(GtkWidget *widget, int *x, int *y, int *w, int *h)
{
	GdkWindow *gw = gtk_widget_get_window(widget);
	GtkAllocation a;
	int ox = 0, oy = 0;

	gtk_widget_get_allocation(widget, &a);
	if (gw)
		gdk_window_get_origin(gw, &ox, &oy);
	if (!gtk_widget_get_has_window(widget)) {
		ox += a.x;
		oy += a.y;
	}
	*x = ox;
	*y = oy;
	*w = a.width;
	*h = a.height;
}

/* `below': a toolbar button's menu, hanging from the button's bottom left
 * corner (above it where there is no room below). */
static gboolean likeos_popup(GtkWidget *widget, GMenuModel *model,
			     GdkEventButton *ev, gboolean below)
{
	Ctx *c = g_new0(Ctx, 1);
	GdkDisplay *d = gtk_widget_get_display(widget);
	GdkEvent *trigger = ev ? gdk_event_copy((GdkEvent *)ev) :
				 gtk_get_current_event();
	/* Opened from the keyboard: the first command is chosen. */
	gboolean keyboard = !ev && (!trigger ||
				    (trigger->type != GDK_BUTTON_PRESS &&
				     trigger->type != GDK_BUTTON_RELEASE));
	GdkGrabStatus st;
	int x, y, alt_y;

	c->attach = g_object_ref(widget);
	c->toplevel = gtk_widget_get_toplevel(widget);
	c->model = g_object_ref(model);
	c->seat = gdk_display_get_default_seat(d);
	c->root = panel_new(c, model);
	if (below) {
		int wx, wy, ww, wh;

		widget_origin(widget, &wx, &wy, &ww, &wh);
		x = wx;
		y = wy + wh + 2;
		alt_y = wy - 2;
	} else if (ev) {
		x = (int)ev->x_root + 1;
		y = alt_y = (int)ev->y_root + 1;
	} else {
		/* The menu key: over the middle of the view. */
		int wx, wy, ww, wh;

		widget_origin(widget, &wx, &wy, &ww, &wh);
		x = wx + ww / 3;
		y = alt_y = wy + wh / 4;
	}
	panel_place(c->root, x, y, x, alt_y);
	gtk_widget_add_events(c->root->win, GDK_POINTER_MOTION_MASK |
						    GDK_BUTTON_PRESS_MASK |
						    GDK_BUTTON_RELEASE_MASK |
						    GDK_KEY_PRESS_MASK);
	g_signal_connect(c->root->win, "motion-notify-event",
			 G_CALLBACK(on_motion), c);
	g_signal_connect(c->root->win, "button-press-event", G_CALLBACK(on_press),
			 c);
	g_signal_connect(c->root->win, "button-release-event",
			 G_CALLBACK(on_release), c);
	g_signal_connect(c->root->win, "key-press-event", G_CALLBACK(on_key), c);
	g_signal_connect(c->root->win, "destroy", G_CALLBACK(root_destroyed), c);
	gtk_widget_show(c->root->win);
	/* Everything to the first panel; owner_events FALSE: the events of
	 * the submenus' windows too, sorted out by hit(). */
	st = gdk_seat_grab(c->seat, gtk_widget_get_window(c->root->win),
			   GDK_SEAT_CAPABILITY_ALL, FALSE, NULL, trigger, NULL,
			   NULL);
	if (trigger)
		gdk_event_free(trigger);
	if (st != GDK_GRAB_SUCCESS) {
		c->closed = TRUE;
		gtk_widget_destroy(c->root->win);
		return FALSE;
	}
	c->opened_us = g_get_monotonic_time();
	/* Opened from the keyboard: the first command is chosen, so the
	 * arrows go on from there. */
	if (keyboard)
		move(c->root, 1);
	return TRUE;
}

void ex_context_menu_popup(GtkWidget *widget, GMenuModel *model,
			   GdkEventButton *ev)
{
	GtkWidget *menu;

	/* A GtkMenu only when the pointer cannot be held for the panel. */
	if (likeos_popup(widget, model, ev, FALSE))
		return;
	menu = fallback_menu(widget, model);
	if (ev)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	else
		gtk_menu_popup_at_widget(GTK_MENU(menu), widget, GDK_GRAVITY_CENTER,
					 GDK_GRAVITY_NORTH_WEST, NULL);
}

void ex_context_menu_popup_below(GtkWidget *button, GMenuModel *model)
{
	GtkWidget *menu;

	if (likeos_popup(button, model, NULL, TRUE))
		return;
	menu = fallback_menu(button, model);
	gtk_menu_popup_at_widget(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST,
				 GDK_GRAVITY_NORTH_WEST, NULL);
}
