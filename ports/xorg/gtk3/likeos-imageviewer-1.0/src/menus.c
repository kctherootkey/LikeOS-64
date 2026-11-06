// LikeOS -- LikeOS Image Viewer: the menus.
//
// Open with (the applications that take the picture's type, the default
// first), the "more" menu of the title row with everything else that can
// be done with the picture, and the menu a right click on the picture
// opens.  All of them in the dark palette of the viewer, with LikeOS's
// own glyphs and the keys that do the same at the right.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <gio/gdesktopappinfo.h>

#include "canvas.h"

/* ---- Open with ------------------------------------------------------------------ */

static void launch_app(GtkMenuItem *mi, IvWindow *w)
{
	GAppInfo *app = g_object_get_data(G_OBJECT(mi), "iv-app");

	if (app)
		iv_cmd_open_with(w, app);
}

static GtkWidget *app_item(GtkWidget *menu, GAppInfo *app, gboolean is_default,
			   IvWindow *w)
{
	GtkWidget *mi = gtk_menu_item_new();
	/* The theme gives a menu item's image its own 10-pixel gap. */
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GIcon *icon = g_app_info_get_icon(app);
	GtkWidget *img = icon ? gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_MENU) :
				iv_glyph_new(IV_GLYPH_APP, 16);
	GtkWidget *l = gtk_label_new(g_app_info_get_display_name(app));

	if (icon)
		gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
	gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
	if (is_default) {
		GtkWidget *d = gtk_label_new("Default");

		iv_add_class(d, "iv-accel");
		gtk_widget_set_margin_start(d, 24);
		gtk_box_pack_end(GTK_BOX(box), d, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(mi), box);
	iv_add_class(mi, "iv-menu-item");
	g_object_set_data_full(G_OBJECT(mi), "iv-app", g_object_ref(app),
			       g_object_unref);
	g_signal_connect(mi, "activate", G_CALLBACK(launch_app), w);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
	gtk_widget_show_all(mi);
	return mi;
}

static gboolean is_self(GAppInfo *app)
{
	return g_strcmp0(g_app_info_get_id(app), IV_DESKTOP_ID) == 0;
}

static gboolean listed(GPtrArray *a, GAppInfo *app)
{
	for (guint i = 0; i < a->len; i++)
		if (g_app_info_equal(g_ptr_array_index(a, i), app))
			return TRUE;
	return FALSE;
}

/* The applications for the picture's type: the default, the recommended
 * ones, then the rest that claim it -- never the viewer itself. */
static GPtrArray *apps_for(const char *mime, GAppInfo **def_out)
{
	char *ct = g_content_type_from_mime_type(mime);
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);
	GAppInfo *def = g_app_info_get_default_for_type(ct ? ct : mime, FALSE);
	GList *rec = g_app_info_get_recommended_for_type(ct ? ct : mime);
	GList *all = g_app_info_get_all_for_type(ct ? ct : mime);

	if (def && !is_self(def) && g_app_info_should_show(def))
		g_ptr_array_add(a, g_object_ref(def));
	for (GList *l = rec; l; l = l->next)
		if (!is_self(l->data) && !listed(a, l->data) &&
		    g_app_info_should_show(l->data))
			g_ptr_array_add(a, g_object_ref(l->data));
	for (GList *l = all; l; l = l->next)
		if (!is_self(l->data) && !listed(a, l->data) &&
		    g_app_info_should_show(l->data))
			g_ptr_array_add(a, g_object_ref(l->data));
	g_list_free_full(rec, g_object_unref);
	g_list_free_full(all, g_object_unref);
	*def_out = def;
	g_free(ct);
	return a;
}

void iv_menu_open_with(IvWindow *w, GtkWidget *button)
{
	GtkWidget *m = iv_menu_new();
	char *mime;
	GAppInfo *def = NULL;
	GPtrArray *apps;

	if (!w->path)
		return;
	mime = w->pic && w->pic->mime ? g_strdup(w->pic->mime) :
					iv_mime_for_path(w->path);
	apps = apps_for(mime, &def);
	for (guint i = 0; i < apps->len; i++) {
		GAppInfo *a = g_ptr_array_index(apps, i);

		app_item(m, a, def && g_app_info_equal(a, def), w);
	}
	if (apps->len == 0) {
		GtkWidget *none = iv_menu_item(m, IV_GLYPH_APP,
					       "No other app opens this type", NULL,
					       NULL, NULL);

		gtk_widget_set_sensitive(none, FALSE);
	}
	iv_menu_separator(m);
	iv_menu_item(m, IV_GLYPH_OPEN_WITH, "_Choose another app…", NULL,
		     G_CALLBACK(iv_cmd_choose_app), w);
	iv_menu_item(m, IV_GLYPH_FOLDER_OPEN, "Show in LikeOS _Explorer", NULL,
		     G_CALLBACK(iv_cmd_show_in_explorer), w);
	g_ptr_array_unref(apps);
	g_clear_object(&def);
	g_free(mime);
	iv_menu_popup_below_end(m, button);
}

/* ---- More --------------------------------------------------------------------- */

static GtkWidget *item(GtkWidget *m, IvGlyph g, const char *label,
		       const char *accel, void (*cmd)(IvWindow *), IvWindow *w,
		       gboolean sensitive)
{
	GtkWidget *mi = iv_menu_item(m, g, label, accel, G_CALLBACK(cmd), w);

	gtk_widget_set_sensitive(mi, sensitive);
	return mi;
}

void iv_menu_more(IvWindow *w, GtkWidget *button)
{
	GtkWidget *m = iv_menu_new();
	gboolean pic = w->pic && !w->pic->error;
	gboolean file = w->path != NULL;

	item(m, IV_GLYPH_SAVE_AS, "_Save as…", "Ctrl+S", iv_cmd_save_as, w, pic);
	item(m, IV_GLYPH_COPY, "_Copy", "Ctrl+C", iv_cmd_copy_image, w, pic);
	item(m, IV_GLYPH_COPY_PATH, "Copy _path", "Ctrl+Shift+C", iv_cmd_copy_path,
	     w, file);
	iv_menu_separator(m);
	item(m, IV_GLYPH_RESIZE, "Resi_ze picture…", NULL, iv_cmd_resize, w, pic);
	item(m, IV_GLYPH_WALLPAPER, "Set as desktop _wallpaper", NULL,
	     iv_cmd_wallpaper, w, pic);
	item(m, IV_GLYPH_MAIL, "Send by _mail", NULL, iv_cmd_mail, w,
	     file && iv_mail_available());
	item(m, IV_GLYPH_RENAME, "_Rename", "F2", iv_cmd_rename, w, file);
	iv_menu_separator(m);
	item(m, IV_GLYPH_SETTINGS, "Se_ttings…", "Ctrl+,", iv_cmd_settings, w, TRUE);
	item(m, IV_GLYPH_KEYBOARD, "_Keyboard shortcuts", "F1", iv_cmd_shortcuts, w,
	     TRUE);
	item(m, IV_GLYPH_ABOUT, "_About", NULL, iv_cmd_about, w, TRUE);
	iv_menu_popup_below_end(m, button);
}

/* ---- the picture's own menu -------------------------------------------------- */

static void rotate_right(IvWindow *w)
{
	iv_viewer_rotate(w, 1);
}

static void rotate_left(IvWindow *w)
{
	iv_viewer_rotate(w, -1);
}

static void delete_picture(IvWindow *w)
{
	iv_viewer_delete(w);
}

static void toggle_info(IvWindow *w)
{
	iv_viewer_toggle_info(w);
}

static void edit(IvWindow *w)
{
	iv_window_enter_editor(w);
}

void iv_menu_context(IvWindow *w, GdkEvent *ev)
{
	GtkWidget *m = iv_menu_new();
	gboolean pic = w->pic && !w->pic->error;
	gboolean file = w->path != NULL;

	if (!file)
		return;
	item(m, IV_GLYPH_EDIT, "_Edit", "E", edit, w, pic);
	item(m, IV_GLYPH_ROTATE_RIGHT, "Rotate _right", "R", rotate_right, w,
	     pic && iv_picture_writable(w->pic));
	item(m, IV_GLYPH_ROTATE_LEFT, "Rotate _left", "Shift+R", rotate_left, w,
	     pic && iv_picture_writable(w->pic));
	iv_menu_separator(m);
	item(m, IV_GLYPH_COPY, "_Copy", "Ctrl+C", iv_cmd_copy_image, w, pic);
	item(m, IV_GLYPH_COPY_PATH, "Copy _path", "Ctrl+Shift+C", iv_cmd_copy_path,
	     w, file);
	item(m, IV_GLYPH_WALLPAPER, "Set as desktop _wallpaper", NULL,
	     iv_cmd_wallpaper, w, pic);
	item(m, IV_GLYPH_FOLDER_OPEN, "Show in LikeOS E_xplorer", NULL,
	     iv_cmd_show_in_explorer, w, file);
	iv_menu_separator(m);
	item(m, IV_GLYPH_INFO, "_Info", "I", toggle_info, w, file);
	item(m, IV_GLYPH_FULLSCREEN, w->fullscreen ? "Leave _full screen" :
						     "_Full screen",
	     "F11", iv_window_toggle_fullscreen, w, TRUE);
	iv_menu_separator(m);
	item(m, IV_GLYPH_DELETE, "Move to _Trash", "Delete", delete_picture, w,
	     file);
	iv_menu_popup_at_pointer(m, w->window, ev);
}
