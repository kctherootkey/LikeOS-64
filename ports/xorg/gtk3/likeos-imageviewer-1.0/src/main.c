// LikeOS -- LikeOS Image Viewer: start-up.
//
//   imageviewer [--fullscreen] [--slideshow] [--edit] [--sort=ORDER]
//               [FILE|FOLDER ...]
//
// Plain GTK initialisation rather than GtkApplication: GtkApplication
// wants a session bus to register on, and a viewer started from a file
// manager or a terminal must not depend on one.  Every invocation is its
// own window.  One file opens with the rest of its folder to browse;
// several files make a list of just those; a folder opens its first
// picture.
//
// Copyright (C) 2026 The LikeOS Project

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include "imageviewer.h"

static gboolean show_version;

typedef struct {
	IvWindow *w;
	IvOptions *opt;
} Start;

static gboolean start(gpointer data)
{
	Start *s = data;

	if (s->opt->files && s->opt->files[0])
		iv_viewer_open_list(s->w, s->opt->files);
	if (s->opt->slideshow)
		iv_viewer_slideshow(s->w, TRUE);
	else if (s->opt->fullscreen)
		iv_window_set_fullscreen(s->w, TRUE);
	g_free(s);
	return G_SOURCE_REMOVE;
}

/* --edit waits for the picture: the editor opens what is shown. */
static gboolean start_editing(gpointer data)
{
	IvWindow *w = data;
	static int tries;

	if (w->pic && !w->pic->error) {
		iv_window_enter_editor(w);
		return G_SOURCE_REMOVE;
	}
	return ++tries < 100 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	IvOptions opt = { 0 };
	GOptionContext *ctx;
	GError *err = NULL;
	gboolean sort_ok = TRUE;
	IvWindow *w;
	Start *s;

	const GOptionEntry entries[] = {
		{ "fullscreen", 'f', 0, G_OPTION_ARG_NONE, &opt.fullscreen,
		  "Start in full screen", NULL },
		{ "slideshow", 's', 0, G_OPTION_ARG_NONE, &opt.slideshow,
		  "Start a slideshow of the folder", NULL },
		{ "edit", 'e', 0, G_OPTION_ARG_NONE, &opt.edit,
		  "Open the picture in the editor", NULL },
		{ "sort", 0, 0, G_OPTION_ARG_STRING, &opt.sort,
		  "The order of the folder: name, name-desc, date, date-desc, "
		  "size, size-desc or type", "ORDER" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &show_version,
		  "Print the version and exit", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt.files,
		  NULL, "[FILE|FOLDER...]" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};

	setlocale(LC_ALL, "");
	g_set_prgname(IV_APP_ID);
	g_set_application_name(IV_APP_NAME);

	ctx = g_option_context_new(NULL);
	g_option_context_set_summary(ctx, "View, browse and edit pictures.");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_add_group(ctx, gtk_get_option_group(FALSE));
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		fprintf(stderr, "imageviewer: %s\n", err->message);
		fprintf(stderr, "Try \"imageviewer --help\".\n");
		g_clear_error(&err);
		g_option_context_free(ctx);
		return 2;
	}
	g_option_context_free(ctx);

	if (show_version) {
		printf("imageviewer %s\n", IV_VERSION);
		return 0;
	}
	iv_sort_parse(opt.sort, &sort_ok);
	if (!sort_ok) {
		fprintf(stderr, "imageviewer: --sort takes name, name-desc, date, "
				"date-desc, size, size-desc or type\n");
		return 2;
	}
	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "imageviewer: cannot open the display "
				"(is DISPLAY set?)\n");
		return 1;
	}

	gtk_window_set_default_icon_name(IV_ICON_NAME);
	iv_style_install();

	w = iv_window_new(&opt);
	gtk_widget_show(w->window);
	s = g_new0(Start, 1);
	s->w = w;
	s->opt = &opt;
	g_idle_add(start, s);
	if (opt.edit)
		g_timeout_add(100, start_editing, w);
	gtk_main();

	iv_window_destroy(w);
	iv_settings_flush();
	g_strfreev(opt.files);
	g_free(opt.sort);
	return 0;
}
