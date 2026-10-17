// LikeOS -- Media Player: start-up.
//
//   mediaplayer [--fullscreen] [--no-video] [--shuffle] [--repeat=MODE]
//               [--video-sink=NAME] [FILE|FOLDER|PLAYLIST|URL ...]
//
// Plain GTK initialisation rather than GtkApplication: GtkApplication
// wants a session bus to register on, and a media player started from a
// file manager or a terminal must not depend on one.  Every invocation is
// its own window.
//
// Copyright (C) 2026 The LikeOS Project

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include <gst/pbutils/pbutils.h>

#include "mediaplayer.h"

static gboolean show_version;

static gboolean start_playback(gpointer data)
{
	MpApp *app = data;
	if (app->options->files != NULL && app->options->files[0] != NULL)
		mp_app_add_paths_async(app, app->options->files, TRUE);
	if (app->options->start_fullscreen)
		mp_app_set_fullscreen(app, TRUE);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	MpOptions opt = { 0 };

	const GOptionEntry entries[] = {
		{ "fullscreen", 'f', 0, G_OPTION_ARG_NONE,
		  &opt.start_fullscreen, "Start in full screen", NULL },
		{ "no-video", 'a', 0, G_OPTION_ARG_NONE, &opt.no_video,
		  "Play the sound only; do not decode video", NULL },
		{ "no-audio", 0, 0, G_OPTION_ARG_NONE, &opt.no_audio,
		  "Play the picture only; do not decode sound", NULL },
		{ "shuffle", 's', 0, G_OPTION_ARG_NONE, &opt.shuffle,
		  "Turn shuffle on", NULL },
		{ "repeat", 'r', 0, G_OPTION_ARG_STRING, &opt.repeat,
		  "Repeat mode: none, all or one", "MODE" },
		{ "video-sink", 0, 0, G_OPTION_ARG_STRING, &opt.video_sink,
		  "Video output: auto, xv, gtk, x11 or gl", "NAME" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &show_version,
		  "Print the version and exit", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY,
		  &opt.files, NULL, "[FILE|FOLDER|PLAYLIST|URL...]" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};

	setlocale(LC_ALL, "");
	g_set_prgname(MP_APP_ID);
	g_set_application_name(MP_APP_NAME);

	GOptionContext *ctx = g_option_context_new(NULL);
	g_option_context_set_summary(ctx,
				     "Play music and videos, files and "
				     "streams, from a play queue.");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_add_group(ctx, gtk_get_option_group(FALSE));
	g_option_context_add_group(ctx, gst_init_get_option_group());

	GError *err = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		fprintf(stderr, "mediaplayer: %s\n", err->message);
		fprintf(stderr, "Try \"mediaplayer --help\".\n");
		g_clear_error(&err);
		g_option_context_free(ctx);
		return 2;
	}
	g_option_context_free(ctx);

	if (show_version) {
		printf("mediaplayer %s (GStreamer %u.%u.%u)\n", MP_VERSION,
		       GST_VERSION_MAJOR, GST_VERSION_MINOR,
		       GST_VERSION_MICRO);
		return 0;
	}
	if (opt.repeat != NULL &&
	    g_ascii_strcasecmp(opt.repeat, "none") != 0 &&
	    g_ascii_strcasecmp(opt.repeat, "off") != 0 &&
	    g_ascii_strcasecmp(opt.repeat, "all") != 0 &&
	    g_ascii_strcasecmp(opt.repeat, "one") != 0) {
		fprintf(stderr, "mediaplayer: --repeat takes none, all or "
				"one\n");
		return 2;
	}
	if (opt.video_sink != NULL) {
		static const char *const sinks[] = { "auto", "xv", "gtk",
						     "x11", "gl", NULL };
		if (!g_strv_contains(sinks, opt.video_sink)) {
			fprintf(stderr, "mediaplayer: --video-sink takes auto, "
					"xv, gtk, x11 or gl\n");
			return 2;
		}
	}

	/* Option parsing above initialised GTK and GStreamer through their
	 * groups; these make sure of it (and report a missing display). */
	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "mediaplayer: cannot open the display "
				"(is DISPLAY set?)\n");
		return 1;
	}
	if (!gst_init_check(&argc, &argv, &err)) {
		fprintf(stderr, "mediaplayer: GStreamer: %s\n",
			err ? err->message : "initialisation failed");
		g_clear_error(&err);
		return 1;
	}
	gst_pb_utils_init();

	gtk_window_set_default_icon_name(MP_ICON_NAME);
	mp_style_install();

	MpApp *app = mp_window_create(&opt);
	if (app == NULL)
		return 1;

	g_idle_add(start_playback, app);
	gtk_main();

	mp_window_destroy(app);
	g_strfreev(opt.files);
	g_free(opt.repeat);
	g_free(opt.video_sink);
	return 0;
}
