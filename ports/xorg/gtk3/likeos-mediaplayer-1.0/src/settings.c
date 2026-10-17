// LikeOS -- Media Player: loading and saving ~/.config/mediaplayer/settings.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#include "playlist.h"
#include "settings.h"

#define GROUP "Player"

static void set_defaults(MpSettings *s)
{
	s->volume = 0.8;
	s->muted = FALSE;
	s->last_dir = NULL;
	s->shuffle = FALSE;
	s->repeat = MP_REPEAT_NONE;
	s->show_playlist = TRUE;
	s->width = 960;
	s->height = 600;
	s->queue_width = 300;
	s->video_sink = g_strdup("auto");
	s->gapless = TRUE;
	s->discover = TRUE;
	s->show_remaining = FALSE;
	s->subtitles = TRUE;
}

gchar *mp_settings_dir(void)
{
	return g_build_filename(g_get_user_config_dir(), "mediaplayer", NULL);
}

static gchar *settings_path(void)
{
	gchar *dir = mp_settings_dir();
	gchar *path = g_build_filename(dir, "settings", NULL);
	g_free(dir);
	return path;
}

/* Each getter leaves the default in place when the key is missing, has the
 * wrong type or is out of range. */
static void get_bool(GKeyFile *kf, const char *key, gboolean *out)
{
	GError *err = NULL;
	gboolean v = g_key_file_get_boolean(kf, GROUP, key, &err);
	if (err == NULL)
		*out = v;
	g_clear_error(&err);
}

static void get_int(GKeyFile *kf, const char *key, int lo, int hi, int *out)
{
	GError *err = NULL;
	int v = g_key_file_get_integer(kf, GROUP, key, &err);
	if (err == NULL && v >= lo && v <= hi)
		*out = v;
	g_clear_error(&err);
}

static void get_double(GKeyFile *kf, const char *key, double lo, double hi,
		       double *out)
{
	GError *err = NULL;
	double v = g_key_file_get_double(kf, GROUP, key, &err);
	/* v == v rejects a NaN written by hand. */
	if (err == NULL && v == v && v >= lo && v <= hi)
		*out = v;
	g_clear_error(&err);
}

static const char *const sink_names[] = { "auto", "xv", "gtk", "x11", "gl",
					  NULL };

void mp_settings_load(MpSettings *s)
{
	set_defaults(s);

	gchar *path = settings_path();
	GKeyFile *kf = g_key_file_new();
	GError *err = NULL;

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
		if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_warning("%s: %s; using the defaults", path,
				  err->message);
		g_clear_error(&err);
		g_key_file_free(kf);
		g_free(path);
		return;
	}

	get_double(kf, "volume", 0.0, 1.0, &s->volume);
	get_bool(kf, "muted", &s->muted);
	get_bool(kf, "shuffle", &s->shuffle);
	get_int(kf, "repeat", MP_REPEAT_NONE, MP_REPEAT_ONE, &s->repeat);
	get_bool(kf, "show-playlist", &s->show_playlist);
	get_int(kf, "width", 320, 16384, &s->width);
	get_int(kf, "height", 240, 16384, &s->height);
	get_int(kf, "queue-width", 160, 4096, &s->queue_width);
	get_bool(kf, "gapless", &s->gapless);
	get_bool(kf, "discover-metadata", &s->discover);
	get_bool(kf, "show-remaining", &s->show_remaining);
	get_bool(kf, "subtitles", &s->subtitles);

	gchar *dir = g_key_file_get_string(kf, GROUP, "last-directory", NULL);
	if (dir != NULL && g_path_is_absolute(dir) &&
	    g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		s->last_dir = dir;
	} else {
		g_free(dir);
	}

	gchar *sink = g_key_file_get_string(kf, GROUP, "video-sink", NULL);
	if (sink != NULL && g_strv_contains(sink_names, sink)) {
		g_free(s->video_sink);
		s->video_sink = sink;
	} else {
		g_free(sink);
	}

	g_key_file_free(kf);
	g_free(path);
}

gboolean mp_settings_save(const MpSettings *s)
{
	gchar *dir = mp_settings_dir();
	/* 0700: the folder names what the user has been watching. */
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_warning("cannot create %s: %s", dir, g_strerror(errno));
		g_free(dir);
		return FALSE;
	}
	g_free(dir);

	GKeyFile *kf = g_key_file_new();
	gchar *path = settings_path();

	/* Keep keys this version does not know (written by a newer one, or
	 * by hand) rather than dropping them on every save. */
	g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

	g_key_file_set_double(kf, GROUP, "volume", s->volume);
	g_key_file_set_boolean(kf, GROUP, "muted", s->muted);
	g_key_file_set_boolean(kf, GROUP, "shuffle", s->shuffle);
	g_key_file_set_integer(kf, GROUP, "repeat", s->repeat);
	g_key_file_set_boolean(kf, GROUP, "show-playlist", s->show_playlist);
	g_key_file_set_integer(kf, GROUP, "width", s->width);
	g_key_file_set_integer(kf, GROUP, "height", s->height);
	g_key_file_set_integer(kf, GROUP, "queue-width", s->queue_width);
	g_key_file_set_string(kf, GROUP, "video-sink",
			      s->video_sink ? s->video_sink : "auto");
	g_key_file_set_boolean(kf, GROUP, "gapless", s->gapless);
	g_key_file_set_boolean(kf, GROUP, "discover-metadata", s->discover);
	g_key_file_set_boolean(kf, GROUP, "show-remaining", s->show_remaining);
	g_key_file_set_boolean(kf, GROUP, "subtitles", s->subtitles);
	if (s->last_dir != NULL)
		g_key_file_set_string(kf, GROUP, "last-directory", s->last_dir);

	/* Written whole to a temporary file and renamed over the old one, so
	 * a crash mid-save never leaves half a file; created 0600 from the
	 * start rather than tightened afterwards. */
	GError *err = NULL;
	gsize len = 0;
	gchar *data = g_key_file_to_data(kf, &len, NULL);
	gboolean ok = g_file_set_contents_full(path, data, (gssize)len,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, &err);
	if (!ok) {
		g_warning("cannot save %s: %s", path, err->message);
		g_clear_error(&err);
	}
	g_free(data);
	g_key_file_free(kf);
	g_free(path);
	return ok;
}

void mp_settings_clear(MpSettings *s)
{
	g_clear_pointer(&s->last_dir, g_free);
	g_clear_pointer(&s->video_sink, g_free);
}
