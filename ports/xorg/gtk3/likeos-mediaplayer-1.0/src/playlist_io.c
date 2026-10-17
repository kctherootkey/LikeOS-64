// LikeOS -- Media Player: reading and writing playlist files.
//
// M3U (plain and extended, in UTF-8 or Latin-1), M3U8 (always UTF-8) and
// PLS.  These are the formats other players save and web radio stations
// hand out.  A playlist is untrusted input -- it may come off a web page --
// so the file size, the line length and the entry count are all bounded, and
// an entry is only ever turned into a URI for GStreamer to open: nothing in
// a playlist is executed or followed recursively.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>

#include "playlist.h"
#include "util.h"

#define PLAYLIST_MAX_BYTES (8 * 1024 * 1024)
#define PLAYLIST_MAX_LINE 8192

/* Text of the file in UTF-8.  An M3U without the "8" predates any encoding
 * convention; what is not valid UTF-8 is read as Latin-1, which is what
 * the players that wrote them used. */
static gchar *read_text(const char *path, GError **error)
{
	GStatBuf st;
	if (g_stat(path, &st) != 0) {
		int e = errno;
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
			    "%s", g_strerror(e));
		return NULL;
	}
	if (!S_ISREG(st.st_mode)) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			    "Not a regular file");
		return NULL;
	}
	if (st.st_size > PLAYLIST_MAX_BYTES) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
			    "The playlist is larger than %d MB",
			    PLAYLIST_MAX_BYTES / (1024 * 1024));
		return NULL;
	}

	gchar *data = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &data, &len, error))
		return NULL;
	if (len > PLAYLIST_MAX_BYTES) {	/* grew between stat and read */
		g_free(data);
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
			    "The playlist is too large");
		return NULL;
	}
	/* A NUL inside the data would silently end every string operation
	 * early; a playlist with one is not a text file. */
	if (memchr(data, '\0', len) != NULL) {
		g_free(data);
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			    "The playlist is not a text file");
		return NULL;
	}

	const gchar *start = data;
	/* UTF-8 byte order mark. */
	if (len >= 3 && (guchar)data[0] == 0xef && (guchar)data[1] == 0xbb &&
	    (guchar)data[2] == 0xbf)
		start += 3;

	gchar *text;
	if (g_utf8_validate(start, -1, NULL)) {
		text = g_strdup(start);
	} else {
		text = g_convert(start, -1, "UTF-8", "ISO-8859-1", NULL, NULL,
				 NULL);
		if (text == NULL)
			text = g_utf8_make_valid(start, -1);
	}
	g_free(data);
	return text;
}

/* Turn one entry line into a URI.  Windows-written playlists use
 * backslashes, and a relative "Music\track.mp3" is meant as a path. */
static gchar *entry_to_uri(const char *line, const char *base_dir)
{
	gchar *s = g_strdup(line);
	if (strstr(s, "://") == NULL && strchr(s, '\\') != NULL &&
	    strchr(s, '/') == NULL)
		g_strdelimit(s, "\\", '/');
	gchar *uri = mp_uri_from_arg(s, base_dir);
	g_free(s);
	if (uri != NULL && !mp_uri_is_playable_scheme(uri) &&
	    !mp_uri_is_local(uri)) {
		g_free(uri);
		return NULL;
	}
	return uri;
}

static void load_m3u(gchar *text, const char *base_dir, GPtrArray *uris,
		     GPtrArray *titles)
{
	gchar *pending_title = NULL;
	gchar **lines = g_strsplit(text, "\n", -1);

	for (gchar **l = lines; *l != NULL; l++) {
		if (uris->len >= MP_PLAYLIST_MAX)
			break;
		gchar *line = g_strstrip(*l);
		if (*line == '\0' || strlen(line) > PLAYLIST_MAX_LINE)
			continue;
		if (line[0] == '#') {
			/* "#EXTINF:<seconds>,<display title>" names the entry
			 * on the next line. */
			if (g_ascii_strncasecmp(line, "#EXTINF:", 8) == 0) {
				const char *comma = strchr(line + 8, ',');
				g_free(pending_title);
				pending_title = comma && comma[1]
					? mp_sanitize_text(comma + 1, 512)
					: NULL;
			}
			continue;
		}
		gchar *uri = entry_to_uri(line, base_dir);
		if (uri == NULL) {
			g_clear_pointer(&pending_title, g_free);
			continue;
		}
		g_ptr_array_add(uris, uri);
		g_ptr_array_add(titles, pending_title);
		pending_title = NULL;
	}
	g_free(pending_title);
	g_strfreev(lines);
}

typedef struct {
	long n;
	gchar *file;
	gchar *title;
} PlsEntry;

static gint pls_cmp(gconstpointer a, gconstpointer b)
{
	const PlsEntry *x = *(PlsEntry *const *)a;
	const PlsEntry *y = *(PlsEntry *const *)b;
	return x->n < y->n ? -1 : x->n > y->n;
}

static void pls_entry_free(gpointer p)
{
	PlsEntry *e = p;
	g_free(e->file);
	g_free(e->title);
	g_free(e);
}

/* "FileN=", "TitleN=" and "LengthN=", in any order and any case.  N is
 * what orders the entries, not their position in the file. */
static void load_pls(gchar *text, const char *base_dir, GPtrArray *uris,
		     GPtrArray *titles)
{
	GHashTable *by_n = g_hash_table_new(g_direct_hash, g_direct_equal);
	GPtrArray *all = g_ptr_array_new_with_free_func(pls_entry_free);
	gchar **lines = g_strsplit(text, "\n", -1);

	for (gchar **l = lines; *l != NULL; l++) {
		gchar *line = g_strstrip(*l);
		if (strlen(line) > PLAYLIST_MAX_LINE)
			continue;
		gchar *eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;
		gboolean is_file = g_ascii_strncasecmp(key, "File", 4) == 0;
		gboolean is_title = g_ascii_strncasecmp(key, "Title", 5) == 0;
		if (!is_file && !is_title)
			continue;
		const char *num = key + (is_file ? 4 : 5);
		char *end = NULL;
		long n = strtol(num, &end, 10);
		if (end == num || *end != '\0' || n < 0 ||
		    n > MP_PLAYLIST_MAX * 2L)
			continue;

		PlsEntry *e = g_hash_table_lookup(by_n, GINT_TO_POINTER(n));
		if (e == NULL) {
			if (all->len >= MP_PLAYLIST_MAX)
				continue;
			e = g_new0(PlsEntry, 1);
			e->n = n;
			g_hash_table_insert(by_n, GINT_TO_POINTER(n), e);
			g_ptr_array_add(all, e);
		}
		if (is_file) {
			g_free(e->file);
			e->file = g_strdup(val);
		} else {
			g_free(e->title);
			e->title = *val ? mp_sanitize_text(val, 512) : NULL;
		}
	}
	g_strfreev(lines);

	g_ptr_array_sort(all, pls_cmp);
	for (guint i = 0; i < all->len; i++) {
		PlsEntry *e = g_ptr_array_index(all, i);
		if (e->file == NULL)
			continue;
		gchar *uri = entry_to_uri(e->file, base_dir);
		if (uri == NULL)
			continue;
		g_ptr_array_add(uris, uri);
		g_ptr_array_add(titles, e->title);
		e->title = NULL;
	}
	g_ptr_array_free(all, TRUE);
	g_hash_table_destroy(by_n);
}

static gboolean looks_like_pls(const char *path, const char *text)
{
	const char *dot = strrchr(path, '.');
	if (dot != NULL && g_ascii_strcasecmp(dot, ".pls") == 0)
		return TRUE;
	while (*text == ' ' || *text == '\t' || *text == '\r' ||
	       *text == '\n')
		text++;
	return g_ascii_strncasecmp(text, "[playlist]", 10) == 0;
}

gboolean mp_playlist_file_load(const char *path, GPtrArray **uris_out,
			       GPtrArray **titles_out, GError **error)
{
	gchar *text = read_text(path, error);
	if (text == NULL)
		return FALSE;

	gchar *base_dir = g_path_get_dirname(path);
	GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *titles = g_ptr_array_new_with_free_func(g_free);

	if (looks_like_pls(path, text))
		load_pls(text, base_dir, uris, titles);
	else
		load_m3u(text, base_dir, uris, titles);

	g_free(base_dir);
	g_free(text);

	if (uris->len == 0) {
		g_ptr_array_free(uris, TRUE);
		g_ptr_array_free(titles, TRUE);
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			    "The playlist has no entries this player can use");
		return FALSE;
	}
	*uris_out = uris;
	*titles_out = titles;
	return TRUE;
}

gboolean mp_playlist_file_save(MpPlaylist *pl, const char *path,
			       GError **error)
{
	gchar **titles = NULL;
	gint64 *durations = NULL;
	gchar **uris = mp_playlist_get_uris(pl, &titles, &durations);

	GString *out = g_string_new("#EXTM3U\n");
	for (guint i = 0; uris[i] != NULL; i++) {
		/* A title must stay on its line: one with a line break in it
		 * would turn the rest into a bogus entry. */
		gchar *title = mp_sanitize_text(titles[i] ? titles[i] : "",
						512);
		gint64 secs = durations[i] >= 0
				      ? (durations[i] + GST_SECOND / 2) /
						GST_SECOND
				      : -1;
		g_string_append_printf(out, "#EXTINF:%" G_GINT64_FORMAT ",%s\n",
				       secs, title);
		g_free(title);

		/* Local files as plain absolute paths (what every player
		 * reads), everything else as its URI. */
		gchar *p = mp_uri_to_path(uris[i]);
		if (p != NULL) {
			gchar *u = g_filename_to_utf8(p, -1, NULL, NULL, NULL);
			if (u != NULL && strchr(u, '\n') == NULL)
				g_string_append_printf(out, "%s\n", u);
			else
				g_string_append_printf(out, "%s\n", uris[i]);
			g_free(u);
			g_free(p);
		} else {
			g_string_append_printf(out, "%s\n", uris[i]);
		}
	}

	gboolean ok = g_file_set_contents(path, out->str, (gssize)out->len,
					  error);
	g_string_free(out, TRUE);
	g_strfreev(uris);
	g_strfreev(titles);
	g_free(durations);
	return ok;
}
