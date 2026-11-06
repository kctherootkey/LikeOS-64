// LikeOS -- LikeOS Image Viewer: the commands.
//
// What the menus, the buttons and the keys do with the picture: saving a
// copy in another format, the clipboard, the wallpaper, mail, renaming,
// the file date, opening it elsewhere, and the dialogs.  Each command
// checks for itself that it has something to work on, so any of them may
// be called at any time.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#include <glib/gstdio.h>

#include "canvas.h"

static gboolean have_picture(IvWindow *w)
{
	return w->pic && !w->pic->error && w->pic->pixbuf;
}

/* ---- Save as -------------------------------------------------------------------- */

typedef struct {
	IvWindow *w;
	char *path;
} Saved;

static void saved(GObject *o, GAsyncResult *res, gpointer data)
{
	Saved *s = data;
	GError *e = NULL;

	iv_toast_hide(s->w->toast);
	if (!iv_save_finish(res, &e)) {
		iv_window_error(s->w, "The picture could not be saved", e);
		g_clear_error(&e);
	} else {
		char *base = g_filename_display_basename(s->path);
		char *msg = g_strdup_printf("Saved as “%s”", base);
		char *dir = g_path_get_dirname(s->path);

		/* A copy in the same folder belongs to the list now. */
		if (s->w->folder && !g_strcmp0(dir, iv_folder_dir(s->w->folder))) {
			iv_viewer_mark_self_written(s->w, s->path);
			iv_folder_add(s->w->folder, s->path);
			iv_filmstrip_reload(s->w->film);
			iv_window_set_title_text(s->w);
		}
		iv_window_toast(s->w, msg);
		g_free(dir);
		g_free(msg);
		g_free(base);
	}
	g_free(s->path);
	g_free(s);
}

static void save_chosen(const char *path, const IvSaveOptions *o, gpointer data)
{
	IvWindow *w = data;
	Saved *s;
	IvSaveOptions opt;

	if (!path || !have_picture(w))
		return;
	opt = *o;
	opt.exif_from = g_strdup(w->path);
	s = g_new0(Saved, 1);
	s->w = w;
	s->path = g_strdup(path);
	iv_toast_progress(w->toast, "Saving", -1);
	iv_save_pixbuf_async(w->pic->pixbuf, path, &opt, NULL, saved, s);
	iv_save_options_clear(&opt);
}

void iv_cmd_save_as(IvWindow *w)
{
	IvFormat f;
	char *suggest;

	if (!have_picture(w))
		return;
	f = iv_format_for_mime(w->pic->mime);
	if (f == IV_FORMAT_NONE)
		f = w->pic->has_alpha ? IV_FORMAT_PNG : IV_FORMAT_JPEG;
	suggest = iv_unique_sibling(w->path, " (copy)", iv_format_extension(f));
	iv_dialog_save_as(GTK_WINDOW(w->window), suggest, f, w->pic->has_alpha,
			  save_chosen, w);
	g_free(suggest);
}

/* ---- the clipboard ----------------------------------------------------------------- */

void iv_cmd_copy_image(IvWindow *w)
{
	GtkClipboard *cb;

	if (!have_picture(w))
		return;
	cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_image(cb, w->pic->pixbuf);
	/* The picture stays on the clipboard after the viewer closes. */
	gtk_clipboard_set_can_store(cb, NULL, 0);
	iv_window_toast(w, "Picture copied");
}

void iv_cmd_copy_path(IvWindow *w)
{
	GtkClipboard *cb;

	if (!w->path)
		return;
	cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text(cb, w->path, -1);
	gtk_clipboard_set_can_store(cb, NULL, 0);
	/* The primary selection too, for the middle button. */
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), w->path,
			       -1);
	iv_window_toast(w, "Path copied");
}

/* ---- the desktop and other programs --------------------------------------------- */

void iv_cmd_wallpaper(IvWindow *w)
{
	GError *e = NULL;

	if (!have_picture(w))
		return;
	iv_viewer_flush_rotation(w);
	if (!iv_set_wallpaper(w->path, &e)) {
		iv_window_error(w, "The desktop wallpaper could not be set", e);
		g_clear_error(&e);
		return;
	}
	iv_window_toast(w, "Set as the desktop wallpaper");
}

void iv_cmd_mail(IvWindow *w)
{
	const char *paths[2] = { w->path, NULL };
	GError *e = NULL;

	if (!w->path)
		return;
	iv_viewer_flush_rotation(w);
	if (!iv_mail_files(paths, &e)) {
		iv_window_error(w, "The mail program could not be started", e);
		g_clear_error(&e);
	}
}

void iv_cmd_show_in_explorer(IvWindow *w)
{
	GError *e = NULL;

	if (!w->path)
		return;
	if (!iv_show_in_explorer(w->path, &e)) {
		iv_window_error(w, "LikeOS Explorer could not be started", e);
		g_clear_error(&e);
	}
}

void iv_cmd_open_folder(IvWindow *w)
{
	char *dir;
	GError *e = NULL;

	if (!w->path)
		return;
	dir = g_path_get_dirname(w->path);
	if (!iv_open_folder(dir, &e)) {
		iv_window_error(w, "The folder could not be opened", e);
		g_clear_error(&e);
	}
	g_free(dir);
}

void iv_cmd_open_with(IvWindow *w, GAppInfo *app)
{
	GFile *f;
	GList *files;
	GError *e = NULL;
	GdkAppLaunchContext *ctx;

	if (!w->path || !app)
		return;
	iv_viewer_flush_rotation_now(w);
	f = g_file_new_for_path(w->path);
	files = g_list_prepend(NULL, f);
	ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(w->window));
	if (!g_app_info_launch(app, files, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
		iv_window_error(w, "The app could not be started", e);
		g_clear_error(&e);
	} else {
		char *uri = g_file_get_uri(f);

		gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
		g_free(uri);
	}
	g_object_unref(ctx);
	g_list_free(files);
	g_object_unref(f);
}

static void app_chosen(GtkDialog *d, int r, IvWindow *w)
{
	if (r == GTK_RESPONSE_OK) {
		GAppInfo *a = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));

		if (a) {
			iv_cmd_open_with(w, a);
			g_object_unref(a);
		}
	}
	gtk_widget_destroy(GTK_WIDGET(d));
}

void iv_cmd_choose_app(IvWindow *w)
{
	GFile *f;
	GtkWidget *d;

	if (!w->path)
		return;
	f = g_file_new_for_path(w->path);
	d = gtk_app_chooser_dialog_new(GTK_WINDOW(w->window),
				       GTK_DIALOG_MODAL |
					       GTK_DIALOG_DESTROY_WITH_PARENT,
				       f);
	g_object_unref(f);
	g_signal_connect(d, "response", G_CALLBACK(app_chosen), w);
	gtk_widget_show(d);
}

/* ---- the name and the date --------------------------------------------------------- */

void iv_cmd_rename(IvWindow *w)
{
	if (!w->path || w->editing)
		return;
	iv_viewer_set_info(w, TRUE);
	iv_info_start_rename(w->info);
}

gboolean iv_cmd_rename_to(IvWindow *w, const char *new_name)
{
	char *dir, *to, *name;
	const char *bad = NULL;

	if (!w->path || !new_name)
		return FALSE;
	name = g_strstrip(g_strdup(new_name));
	if (!*name)
		bad = "A name cannot be empty";
	else if (strchr(name, '/'))
		bad = "A name cannot contain “/”";
	else if (!strcmp(name, ".") || !strcmp(name, ".."))
		bad = "That name is reserved";
	else if (strlen(name) > 255)
		bad = "The name is too long";
	if (bad) {
		iv_window_toast(w, bad);
		g_free(name);
		return FALSE;
	}
	dir = g_path_get_dirname(w->path);
	to = g_build_filename(dir, name, NULL);
	if (!strcmp(to, w->path)) {
		g_free(to);
		g_free(dir);
		g_free(name);
		return TRUE;
	}
	if (g_file_test(to, G_FILE_TEST_EXISTS)) {
		char *msg = g_strdup_printf("There already is a file called “%s”",
					    name);

		iv_window_toast(w, msg);
		g_free(msg);
		g_free(to);
		g_free(dir);
		g_free(name);
		return FALSE;
	}
	iv_viewer_flush_rotation_now(w);
	if (g_rename(w->path, to) != 0) {
		int err = errno;
		char *msg = g_strdup_printf("The file could not be renamed: %s",
					    g_strerror(err));

		iv_window_toast(w, msg);
		g_free(msg);
		g_free(to);
		g_free(dir);
		g_free(name);
		return FALSE;
	}
	{
		char *from = g_strdup(w->path);

		iv_viewer_renamed(w, from, to);
		g_free(from);
	}
	iv_window_toast(w, "Renamed");
	g_free(to);
	g_free(dir);
	g_free(name);
	return TRUE;
}

gboolean iv_cmd_set_mtime(IvWindow *w, gint64 when)
{
	struct stat st;
	struct utimbuf t;

	if (!w->path || stat(w->path, &st) != 0)
		return FALSE;
	t.actime = st.st_atime;
	t.modtime = (time_t)when;
	if (utime(w->path, &t) != 0) {
		int err = errno;
		char *msg = g_strdup_printf("The date could not be changed: %s",
					    g_strerror(err));

		iv_window_toast(w, msg);
		g_free(msg);
		return FALSE;
	}
	/* Keep what is shown; only its record of the time changes. */
	iv_viewer_mark_self_written(w, w->path);
	if (w->pic) {
		w->pic->mtime = when;
		iv_cache_put(w->cache, w->pic);
		iv_info_set_picture(w->info, w->pic);
	}
	return TRUE;
}

/* ---- opening another picture --------------------------------------------------- */

static void open_chosen(GtkDialog *d, int r, IvWindow *w)
{
	if (r == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));

		if (path)
			iv_viewer_open(w, path);
		g_free(path);
	}
	gtk_widget_destroy(GTK_WIDGET(d));
}

void iv_cmd_open_file(IvWindow *w)
{
	GtkWidget *d;
	GtkFileFilter *pics = gtk_file_filter_new();
	GtkFileFilter *all = gtk_file_filter_new();

	if (w->editing)
		return;
	d = gtk_file_chooser_dialog_new("Open a picture", GTK_WINDOW(w->window),
					GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
					GTK_RESPONSE_CANCEL, "_Open",
					GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_filter_set_name(pics, "Pictures");
	gtk_file_filter_add_pixbuf_formats(pics);
	gtk_file_filter_add_mime_type(pics, "image/webp");
	gtk_file_filter_add_pattern(pics, "*.webp");
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), pics);
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
	if (w->folder && iv_folder_dir(w->folder))
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),
						    iv_folder_dir(w->folder));
	g_signal_connect(d, "response", G_CALLBACK(open_chosen), w);
	gtk_widget_show(d);
}

/* ---- the dialogs ----------------------------------------------------------------- */

void iv_cmd_resize(IvWindow *w)
{
	if (have_picture(w))
		iv_dialog_resize(w);
}

void iv_cmd_settings(IvWindow *w)
{
	iv_dialog_settings(w);
}

void iv_cmd_shortcuts(IvWindow *w)
{
	iv_dialog_shortcuts(w);
}

void iv_cmd_about(IvWindow *w)
{
	iv_dialog_about(w);
}
