// LikeOS Explorer: Share.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-share.h"
#include "services/ex-clipboard.h"
#include "ui/ex-style.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

/* The mail program's way of taking attachments, from its desktop entry:
 * an Exec-like line in which %F stands for the files. */
#define EX_ATTACH_KEY "X-LikeOS-Attach"

static GDesktopAppInfo *mailer(void)
{
	GAppInfo *a = g_app_info_get_default_for_uri_scheme("mailto");

	if (a && G_IS_DESKTOP_APP_INFO(a) &&
	    g_desktop_app_info_has_key(G_DESKTOP_APP_INFO(a), EX_ATTACH_KEY))
		return G_DESKTOP_APP_INFO(a);
	g_clear_object(&a);
	return NULL;
}

gboolean ex_share_can_mail(void)
{
	GDesktopAppInfo *m = mailer();
	gboolean ok = m != NULL;

	g_clear_object(&m);
	return ok;
}

static gboolean mail(GPtrArray *files, GError **error)
{
	GDesktopAppInfo *m = mailer();
	char *line, **argv = NULL;
	GPtrArray *args;
	gboolean ok;

	if (!m) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "No mail program that takes attachments is set");
		return FALSE;
	}
	line = g_desktop_app_info_get_string(m, EX_ATTACH_KEY);
	if (!g_shell_parse_argv(line, NULL, &argv, error)) {
		g_free(line);
		g_object_unref(m);
		return FALSE;
	}
	args = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; argv[i]; i++) {
		if (!strcmp(argv[i], "%F")) {
			for (guint k = 0; k < files->len; k++) {
				char *p = g_file_get_path(g_ptr_array_index(files, k));

				if (p)
					g_ptr_array_add(args, p);
			}
		} else {
			g_ptr_array_add(args, g_strdup(argv[i]));
		}
	}
	g_ptr_array_add(args, NULL);
	ok = g_spawn_async(NULL, (char **)args->pdata, NULL, G_SPAWN_SEARCH_PATH,
			   NULL, NULL, NULL, error);
	g_ptr_array_unref(args);
	g_strfreev(argv);
	g_free(line);
	g_object_unref(m);
	return ok;
}

enum { R_MAIL = 1, R_PATH, R_OTHER };

typedef struct {
	ExWindow *w;
	GPtrArray *files;
} Share;

static void share_free(Share *s)
{
	g_object_unref(s->w);
	g_ptr_array_unref(s->files);
	g_free(s);
}

static void other_chosen(GtkDialog *d, int r, Share *s)
{
	if (r == GTK_RESPONSE_OK) {
		GAppInfo *a = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));

		if (a) {
			ex_window_open_with(s->w, s->files, a);
			g_object_unref(a);
		}
	}
	gtk_widget_destroy(GTK_WIDGET(d));
	share_free(s);
}

static void response(GtkDialog *d, int r, Share *s)
{
	GError *e = NULL;

	gtk_widget_destroy(GTK_WIDGET(d));
	switch (r) {
	case R_MAIL:
		if (!mail(s->files, &e)) {
			ex_window_show_error(s->w, "The mail program could not be started",
					     e->message);
			g_error_free(e);
		}
		break;
	case R_PATH:
		ex_clipboard_copy_paths(ex_clipboard_get(), s->files);
		break;
	case R_OTHER: {
		GtkWidget *c = gtk_app_chooser_dialog_new(GTK_WINDOW(s->w),
							  GTK_DIALOG_MODAL,
							  g_ptr_array_index(s->files,
									    0));
		g_signal_connect(c, "response", G_CALLBACK(other_chosen), s);
		gtk_widget_show(c);
		return; /* `s' goes with the chooser */
	}
	default:
		break;
	}
	share_free(s);
}

static GtkWidget *target(GtkWidget *d, int response_id, const char *icon,
			 const char *label)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

	gtk_box_pack_start(GTK_BOX(box),
			   gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	atk_object_set_name(gtk_widget_get_accessible(b), label);
	gtk_dialog_add_action_widget(GTK_DIALOG(d), b, response_id);
	return b;
}

void ex_share_show(ExWindow *w, GPtrArray *files)
{
	Share *s = g_new0(Share, 1);
	GtkWidget *d = gtk_dialog_new_with_buttons("Share", GTK_WINDOW(w),
						   GTK_DIALOG_MODAL |
							   GTK_DIALOG_DESTROY_WITH_PARENT,
						   NULL, NULL);
	GtkWidget *l;
	char *what = files->len == 1 ?
			     g_file_get_basename(g_ptr_array_index(files, 0)) :
			     g_strdup_printf("%u items", files->len);

	s->w = g_object_ref(w);
	s->files = g_ptr_array_ref(files);
	l = gtk_label_new(what);
	gtk_container_set_border_width(GTK_CONTAINER(l), 12);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  l);
	if (ex_share_can_mail())
		target(d, R_MAIL, "mail-send", "Email");
	target(d, R_PATH, "edit-copy", "Copy path");
	if (files->len == 1)
		target(d, R_OTHER, "system-run", "Another app…");
	g_signal_connect(d, "response", G_CALLBACK(response), s);
	ex_style_fixed_dialog(d);
	gtk_widget_show_all(d);
	g_free(what);
}
