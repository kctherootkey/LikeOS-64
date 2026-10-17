// LikeOS -- Media Player: time formatting, URI and file-name helpers.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <gio/gio.h>

#include "util.h"

gchar *mp_format_time(gint64 ns)
{
	if (ns < 0 || !GST_CLOCK_TIME_IS_VALID((GstClockTime)ns))
		return g_strdup("--:--");

	guint64 total = (guint64)ns / GST_SECOND;
	guint64 h = total / 3600;
	guint64 m = (total / 60) % 60;
	guint64 s = total % 60;

	if (h > 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT ":%02u:%02u", h,
				       (unsigned)m, (unsigned)s);
	return g_strdup_printf("%u:%02u", (unsigned)m, (unsigned)s);
}

gchar *mp_format_remaining(gint64 position, gint64 duration)
{
	if (duration < 0 || position < 0)
		return g_strdup("--:--");
	gint64 left = duration > position ? duration - position : 0;
	gchar *t = mp_format_time(left);
	gchar *r = g_strconcat("-", t, NULL);
	g_free(t);
	return r;
}

gchar *mp_uri_from_arg(const char *arg, const char *base_dir)
{
	if (arg == NULL)
		return NULL;

	/* Playlist lines and dropped text carry stray white space and the
	 * carriage returns of files written elsewhere. */
	gchar *s = g_strstrip(g_strdup(arg));
	if (*s == '\0') {
		g_free(s);
		return NULL;
	}

	gchar *uri = NULL;
	if (gst_uri_is_valid(s) && strstr(s, "://") != NULL) {
		/* Already a URI.  A file:// one is normalised through GFile so
		 * that "file:///a/./b" and "file:///a/b" compare equal in the
		 * playlist. */
		if (g_ascii_strncasecmp(s, "file:", 5) == 0) {
			GFile *f = g_file_new_for_uri(s);
			uri = g_file_get_uri(f);
			g_object_unref(f);
		} else {
			uri = g_strdup(s);
		}
	} else {
		gchar *path;
		if (g_path_is_absolute(s)) {
			path = g_strdup(s);
		} else if (base_dir != NULL) {
			path = g_build_filename(base_dir, s, NULL);
		} else {
			gchar *cwd = g_get_current_dir();
			path = g_build_filename(cwd, s, NULL);
			g_free(cwd);
		}
		GFile *f = g_file_new_for_path(path);
		uri = g_file_get_uri(f);
		g_object_unref(f);
		g_free(path);
	}
	g_free(s);
	return uri;
}

gchar *mp_uri_to_path(const char *uri)
{
	if (uri == NULL || g_ascii_strncasecmp(uri, "file:", 5) != 0)
		return NULL;
	return g_filename_from_uri(uri, NULL, NULL);
}

gboolean mp_uri_is_local(const char *uri)
{
	return uri != NULL && g_ascii_strncasecmp(uri, "file:", 5) == 0;
}

gchar *mp_uri_display_name(const char *uri)
{
	if (uri == NULL)
		return g_strdup("");

	gchar *path = mp_uri_to_path(uri);
	if (path != NULL) {
		gchar *base = g_path_get_basename(path);
		gchar *disp = g_filename_display_name(base);
		g_free(base);
		g_free(path);
		/* Drop the extension, but only a plausible one: "Mr. Blue Sky"
		 * has a dot that is not one. */
		char *dot = strrchr(disp, '.');
		if (dot != NULL && dot != disp && strlen(dot) <= 6 &&
		    strchr(dot, ' ') == NULL)
			*dot = '\0';
		return disp;
	}

	/* A stream: the last non-empty path component, unescaped, or the host
	 * when the path is "/" (an internet radio station's root). */
	gchar *result = NULL;
	GstUri *gu = gst_uri_from_string(uri);
	if (gu != NULL) {
		const gchar *host = gst_uri_get_host(gu);
		GList *segs = gst_uri_get_path_segments(gu);
		const gchar *last = NULL;
		for (GList *l = segs; l != NULL; l = l->next) {
			if (l->data != NULL && *(const gchar *)l->data)
				last = l->data;
		}
		if (last != NULL)
			result = g_strdup(last);
		else if (host != NULL)
			result = g_strdup(host);
		g_list_free_full(segs, g_free);
		gst_uri_unref(gu);
	}
	if (result == NULL)
		result = g_strdup(uri);
	gchar *clean = mp_sanitize_text(result, 256);
	g_free(result);
	return clean;
}

/* Extensions of the containers and elementary streams GStreamer's demuxers
 * and parsers handle.  Lower case; compared case-insensitively. */
static const char *const media_exts[] = {
	/* audio */
	"wav", "wave", "aif", "aiff", "aifc", "au", "snd", "mp3", "mp2",
	"mpa", "m4a", "m4b", "aac", "adts", "ogg", "oga", "opus", "flac",
	"wma", "ape", "wv", "mka", "spx", "ac3", "eac3", "dts", "amr",
	"awb", "caf", "tta", "mid", "midi", "voc", "w64", "rf64", "mpc",
	"dsf", "dff",
	/* video */
	"mp4", "m4v", "mov", "qt", "mkv", "webm", "avi", "ogv", "ogm",
	"mpg", "mpeg", "mpe", "m2v", "vob", "ts", "m2ts", "mts", "m2t",
	"flv", "f4v", "wmv", "asf", "3gp", "3g2", "divx", "xvid", "h264",
	"264", "h265", "265", "hevc", "ivf", "y4m", "dv", "mxf", "nut",
	"rm", "rmvb", "mjpeg", "mjpg",
	NULL
};

static const char *const playlist_exts[] = {
	"m3u", "m3u8", "pls", NULL
};

const char *const *mp_media_extensions(void)
{
	return media_exts;
}

static gboolean has_ext(const char *name, const char *const *list)
{
	const char *dot = strrchr(name, '.');
	if (dot == NULL || dot[1] == '\0')
		return FALSE;
	for (int i = 0; list[i] != NULL; i++) {
		if (g_ascii_strcasecmp(dot + 1, list[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean mp_is_playlist_name(const char *name)
{
	return name != NULL && has_ext(name, playlist_exts);
}

gboolean mp_is_media_name(const char *name)
{
	if (name == NULL || *name == '\0' || *name == '.')
		return FALSE;
	if (has_ext(name, media_exts))
		return TRUE;
	if (mp_is_playlist_name(name))
		return FALSE;

	/* Anything the shared MIME database calls audio or video by its name
	 * alone, for the extensions the table above does not know. */
	gboolean uncertain = TRUE;
	gchar *ct = g_content_type_guess(name, NULL, 0, &uncertain);
	gboolean media = FALSE;
	if (ct != NULL && !uncertain) {
		gchar *mime = g_content_type_get_mime_type(ct);
		if (mime != NULL &&
		    (g_str_has_prefix(mime, "audio/") ||
		     g_str_has_prefix(mime, "video/")) &&
		    strstr(mime, "mpegurl") == NULL &&
		    strstr(mime, "scpls") == NULL)
			media = TRUE;
		g_free(mime);
	}
	g_free(ct);
	return media;
}

gboolean mp_uri_is_playable_scheme(const char *uri)
{
	if (uri == NULL)
		return FALSE;
	gchar *proto = gst_uri_get_protocol(uri);
	if (proto == NULL)
		return FALSE;
	gboolean ok = gst_uri_protocol_is_supported(GST_URI_SRC, proto);
	g_free(proto);
	return ok;
}

gint mp_compare_filenames(const char *a, const char *b)
{
	gchar *ka = g_utf8_collate_key_for_filename(a, -1);
	gchar *kb = g_utf8_collate_key_for_filename(b, -1);
	gint r = strcmp(ka, kb);
	g_free(ka);
	g_free(kb);
	return r;
}

gchar *mp_sanitize_text(const char *s, gsize max_chars)
{
	if (s == NULL)
		return NULL;

	gchar *valid = g_utf8_make_valid(s, -1);
	GString *out = g_string_sized_new(strlen(valid) + 1);
	gsize n = 0;
	for (const gchar *p = valid; *p != '\0' && n < max_chars;
	     p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		/* Tabs and line breaks become spaces; other control and
		 * format characters (including bidi overrides, which can make
		 * a label lie about its own text) are dropped. */
		if (c == '\t' || c == '\n' || c == '\r') {
			g_string_append_c(out, ' ');
		} else if (g_unichar_iscntrl(c) ||
			   g_unichar_type(c) == G_UNICODE_FORMAT) {
			continue;
		} else {
			g_string_append_unichar(out, c);
		}
		n++;
	}
	g_free(valid);
	g_strstrip(out->str);
	out->len = strlen(out->str);
	return g_string_free(out, FALSE);
}
