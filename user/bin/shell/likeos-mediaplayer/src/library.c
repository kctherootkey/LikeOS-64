// LikeOS -- LikeOS Media Player: the music and video library.
//
// Folders are walked by scan.c in a worker thread; what it finds is merged
// into the store on the main loop: new files are added under their file
// name and handed to the discoverer for their tags, files whose
// modification time changed are read again, and files that are gone are
// removed.  Album covers come from a picture file in the album's folder or,
// failing that, from a picture embedded in a track's tags; the embedded
// ones are kept in ~/.cache/mediaplayer/covers so they need not be read
// again on the next start.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gst/gst.h>

#include "discover.h"
#include "library.h"
#include "mediaplayer.h"
#include "scan.h"
#include "settings.h"
#include "thumbs.h"
#include "util.h"

#define CACHE_MAGIC "# LikeOS Media Player library 1"
#define RECENT_MAX 24
#define COVER_EDGE 300

typedef struct {
	guint id;
	GtkTreeIter iter;
} Row;

struct MpLibrary {
	GtkListStore *store;
	GHashTable *by_uri;	/* URI -> Row* (owned) */
	GHashTable *by_id;	/* id -> Row* (borrowed) */
	guint next_id;
	MpDiscover *disc;
	MpThumbs *thumbs;
	GCancellable *cancel;
	guint scanning;
	guint generation;

	GHashTable *albums;	/* key -> MpAlbum* */
	GHashTable *artists;	/* key -> MpArtist* */
	GPtrArray *album_list;
	GPtrArray *artist_list;
	GHashTable *covers;	/* album key -> GdkPixbuf*, NULL: none */
	GHashTable *uri_covers;	/* URI outside the library -> GdkPixbuf* */

	GPtrArray *recent;	/* MpRecent* */

	MpLibraryChanged changed;
	MpLibraryPicture picture;
	gpointer data;
	guint changed_timer;
	guint save_timer;
	gboolean loading;	/* reading the cache file */
};

typedef struct {
	MpLibrary *lib;
	GCancellable *cancel;	/* the library's, referenced */
	guint generation;
	gboolean retag;
} ScanReq;

static void mark_changed(MpLibrary *lib);

/* ---- small helpers ---------------------------------------------------- */

static gchar *collate_key(const char *s)
{
	gchar *f = g_utf8_casefold(s != NULL ? s : "", -1);
	gchar *k = g_utf8_collate_key(f, -1);
	g_free(f);
	return k;
}

static gchar *group_key(const char *album, const char *artist)
{
	gchar *al = g_utf8_casefold(album != NULL ? album : "", -1);
	gchar *ar = g_utf8_casefold(artist != NULL ? artist : "", -1);
	gchar *k = g_strconcat(al, "\x1f", ar, NULL);
	g_free(al);
	g_free(ar);
	return k;
}

static void unref_maybe(gpointer p)
{
	if (p != NULL)
		g_object_unref(p);
}

static gchar *cover_cache_path(const char *key)
{
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, key, -1);
	gchar *name = g_strconcat(sum, ".png", NULL);
	gchar *path = g_build_filename(g_get_user_cache_dir(), "mediaplayer",
				       "covers", name, NULL);
	g_free(sum);
	g_free(name);
	return path;
}

static GdkPixbuf *shrink(GdkPixbuf *pb)
{
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	if (w <= COVER_EDGE && h <= COVER_EDGE)
		return pb;
	double s = (double)COVER_EDGE / MAX(w, h);
	GdkPixbuf *small = gdk_pixbuf_scale_simple(
		pb, MAX(1, (int)(w * s)), MAX(1, (int)(h * s)),
		GDK_INTERP_BILINEAR);
	g_object_unref(pb);
	return small;
}

/* ---- rows ------------------------------------------------------------- */

static void set_text_cols(MpLibrary *lib, GtkTreeIter *it, const char *title,
			  const char *artist, const char *album)
{
	gchar *tk = collate_key(title);
	gchar *ark = collate_key(artist);
	gchar *alk = collate_key(album);
	gchar *g = group_key(album, artist);
	gtk_list_store_set(lib->store, it, MP_LIB_TITLE, title, MP_LIB_ARTIST,
			   artist, MP_LIB_ALBUM, album, MP_LIB_TITLE_KEY, tk,
			   MP_LIB_ARTIST_KEY, ark, MP_LIB_ALBUM_KEY, alk,
			   MP_LIB_GROUP, g, -1);
	g_free(tk);
	g_free(ark);
	g_free(alk);
	g_free(g);
}

static void set_duration(MpLibrary *lib, GtkTreeIter *it, gint64 d)
{
	gchar *t = d > 0 ? mp_format_time(d) : g_strdup("");
	gtk_list_store_set(lib->store, it, MP_LIB_DURATION, d,
			   MP_LIB_DURATION_TEXT, t, -1);
	g_free(t);
}

static Row *add_row(MpLibrary *lib, const char *uri, const char *title,
		    const char *artist, const char *album, gint64 duration,
		    gboolean video, gint64 mtime, gint64 added, guint track)
{
	Row *row = g_new0(Row, 1);
	row->id = ++lib->next_id;
	gtk_list_store_insert_with_values(lib->store, &row->iter, -1,
					  MP_LIB_ID, row->id, MP_LIB_URI, uri,
					  MP_LIB_VIDEO, video, MP_LIB_MTIME,
					  mtime, MP_LIB_ADDED, added,
					  MP_LIB_TRACK, track, -1);
	set_text_cols(lib, &row->iter, title, artist, album);
	set_duration(lib, &row->iter, duration);
	g_hash_table_insert(lib->by_uri, g_strdup(uri), row);
	g_hash_table_insert(lib->by_id, GUINT_TO_POINTER(row->id), row);
	return row;
}

static void remove_row(MpLibrary *lib, const char *uri)
{
	Row *row = g_hash_table_lookup(lib->by_uri, uri);
	if (row == NULL)
		return;
	mp_discover_forget(lib->disc, row->id);
	gtk_list_store_remove(lib->store, &row->iter);
	g_hash_table_remove(lib->by_id, GUINT_TO_POINTER(row->id));
	g_hash_table_remove(lib->by_uri, uri);
}

/* ---- the cache file --------------------------------------------------- */

static gchar *cache_file(void)
{
	return g_build_filename(g_get_user_cache_dir(), "mediaplayer",
				"library", NULL);
}

static void load_cache(MpLibrary *lib)
{
	gchar *path = cache_file();
	gchar *text = NULL;
	gsize len = 0;
	if (!g_file_get_contents(path, &text, &len, NULL)) {
		g_free(path);
		return;
	}
	g_free(path);
	gchar **lines = g_strsplit(text, "\n", -1);
	g_free(text);
	if (lines[0] == NULL || strcmp(lines[0], CACHE_MAGIC) != 0) {
		g_strfreev(lines);
		return;
	}
	lib->loading = TRUE;
	for (gchar **l = lines + 1; *l != NULL; l++) {
		/* uri, mtime, added, video, duration, track, title, artist,
		 * album */
		gchar **f = g_strsplit(*l, "\t", 9);
		if (g_strv_length(f) == 9 && g_utf8_validate(*l, -1, NULL) &&
		    strstr(f[0], "://") != NULL &&
		    !g_hash_table_contains(lib->by_uri, f[0]))
			add_row(lib, f[0], f[6], f[7], f[8],
				g_ascii_strtoll(f[4], NULL, 10),
				f[3][0] == '1', g_ascii_strtoll(f[1], NULL, 10),
				g_ascii_strtoll(f[2], NULL, 10),
				(guint)g_ascii_strtoull(f[5], NULL, 10));
		g_strfreev(f);
	}
	lib->loading = FALSE;
	g_strfreev(lines);
}

static gboolean save_cache(gpointer data)
{
	MpLibrary *lib = data;
	lib->save_timer = 0;
	GString *s = g_string_new(CACHE_MAGIC "\n");
	GtkTreeModel *m = GTK_TREE_MODEL(lib->store);
	GtkTreeIter it;
	for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
	     ok = gtk_tree_model_iter_next(m, &it)) {
		gchar *uri, *title, *artist, *album;
		gint64 mtime, added, dur;
		gboolean video;
		guint track;
		gtk_tree_model_get(m, &it, MP_LIB_URI, &uri, MP_LIB_TITLE,
				   &title, MP_LIB_ARTIST, &artist, MP_LIB_ALBUM,
				   &album, MP_LIB_MTIME, &mtime, MP_LIB_ADDED,
				   &added, MP_LIB_DURATION, &dur, MP_LIB_VIDEO,
				   &video, MP_LIB_TRACK, &track, -1);
		/* The texts went through mp_sanitize_text(): no tabs or
		 * line breaks in them. */
		g_string_append_printf(s, "%s\t%lld\t%lld\t%d\t%lld\t%u\t%s\t%s\t%s\n",
				       uri, (long long)mtime,
				       (long long)added, video ? 1 : 0,
				       (long long)dur, track, title, artist,
				       album);
		g_free(uri);
		g_free(title);
		g_free(artist);
		g_free(album);
	}
	gchar *path = cache_file();
	gchar *dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) == 0)
		g_file_set_contents_full(path, s->str, (gssize)s->len,
					 G_FILE_SET_CONTENTS_CONSISTENT, 0600,
					 NULL);
	g_free(dir);
	g_free(path);
	g_string_free(s, TRUE);
	return G_SOURCE_REMOVE;
}

static void schedule_save(MpLibrary *lib)
{
	if (lib->save_timer != 0)
		g_source_remove(lib->save_timer);
	lib->save_timer = g_timeout_add_seconds(3, save_cache, lib);
}

/* ---- albums and artists ----------------------------------------------- */

static void album_free(gpointer p)
{
	MpAlbum *a = p;
	g_free(a->key);
	g_free(a->title);
	g_free(a->artist);
	g_free(a->first_uri);
	g_free(a);
}

static void artist_free(gpointer p)
{
	MpArtist *a = p;
	g_free(a->key);
	g_free(a->name);
	g_free(a->album);
	g_free(a);
}

static gint cmp_album(gconstpointer pa, gconstpointer pb)
{
	const MpAlbum *a = *(MpAlbum *const *)pa, *b = *(MpAlbum *const *)pb;
	return g_utf8_collate(a->title, b->title);
}

static gint cmp_artist(gconstpointer pa, gconstpointer pb)
{
	const MpArtist *a = *(MpArtist *const *)pa;
	const MpArtist *b = *(MpArtist *const *)pb;
	return strcmp(a->key, b->key);
}

static void rebuild_groups(MpLibrary *lib)
{
	g_hash_table_remove_all(lib->albums);
	g_hash_table_remove_all(lib->artists);
	g_ptr_array_set_size(lib->album_list, 0);
	g_ptr_array_set_size(lib->artist_list, 0);

	GtkTreeModel *m = GTK_TREE_MODEL(lib->store);
	GtkTreeIter it;
	for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
	     ok = gtk_tree_model_iter_next(m, &it)) {
		gboolean video;
		gchar *uri, *artist, *album, *group, *akey;
		gint64 dur, added;
		gtk_tree_model_get(m, &it, MP_LIB_VIDEO, &video, MP_LIB_URI,
				   &uri, MP_LIB_ARTIST, &artist, MP_LIB_ALBUM,
				   &album, MP_LIB_GROUP, &group,
				   MP_LIB_ARTIST_KEY, &akey, MP_LIB_DURATION,
				   &dur, MP_LIB_ADDED, &added, -1);
		if (!video) {
			MpAlbum *a = g_hash_table_lookup(lib->albums, group);
			if (a == NULL) {
				a = g_new0(MpAlbum, 1);
				a->key = g_strdup(group);
				a->title = g_strdup(*album ? album
							   : "Unknown album");
				a->artist = g_strdup(*artist ? artist
							     : "Unknown artist");
				a->first_uri = g_strdup(uri);
				g_hash_table_insert(lib->albums, a->key, a);
				g_ptr_array_add(lib->album_list, a);
			}
			a->tracks++;
			if (dur > 0)
				a->total += dur;
			if (added > a->added)
				a->added = added;

			MpArtist *ar = g_hash_table_lookup(lib->artists, akey);
			if (ar == NULL) {
				ar = g_new0(MpArtist, 1);
				ar->key = g_strdup(akey);
				ar->name = g_strdup(*artist ? artist
							    : "Unknown artist");
				ar->album = g_strdup(group);
				g_hash_table_insert(lib->artists, ar->key, ar);
				g_ptr_array_add(lib->artist_list, ar);
			}
			ar->tracks++;
		}
		g_free(uri);
		g_free(artist);
		g_free(album);
		g_free(group);
		g_free(akey);
	}
	g_ptr_array_sort(lib->album_list, cmp_album);
	g_ptr_array_sort(lib->artist_list, cmp_artist);
}

static gboolean changed_timeout(gpointer data)
{
	MpLibrary *lib = data;
	lib->changed_timer = 0;
	rebuild_groups(lib);
	if (lib->changed != NULL)
		lib->changed(lib, lib->data);
	return G_SOURCE_REMOVE;
}

static void mark_changed(MpLibrary *lib)
{
	if (lib->changed_timer == 0)
		lib->changed_timer = g_timeout_add(300, changed_timeout, lib);
	if (!lib->loading)
		schedule_save(lib);
}

/* ---- tags ------------------------------------------------------------- */

static void cover_from_tags(MpLibrary *lib, const char *group,
			    const GstTagList *tags)
{
	if (tags == NULL || g_hash_table_lookup(lib->covers, group) != NULL)
		return;
	if (gst_tag_list_get_tag_size(tags, GST_TAG_IMAGE) == 0 &&
	    gst_tag_list_get_tag_size(tags, GST_TAG_PREVIEW_IMAGE) == 0)
		return;
	GdkPixbuf *pb = mp_cover_from_tags(tags);
	if (pb == NULL)
		return;
	pb = shrink(pb);
	gchar *path = cover_cache_path(group);
	gchar *dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) == 0)
		gdk_pixbuf_save(pb, path, "png", NULL, NULL);
	g_free(dir);
	g_free(path);
	g_hash_table_replace(lib->covers, g_strdup(group), pb);
	if (lib->picture != NULL)
		lib->picture(lib, NULL, lib->data);
}

static void on_discovered(guint id, const char *title, const char *artist,
			  const char *album, gint64 duration,
			  gboolean has_video, const GstTagList *tags,
			  gpointer data)
{
	MpLibrary *lib = data;
	Row *row = g_hash_table_lookup(lib->by_id, GUINT_TO_POINTER(id));
	if (row == NULL)
		return;
	GtkTreeModel *m = GTK_TREE_MODEL(lib->store);
	gchar *old_title = NULL;
	gtk_tree_model_get(m, &row->iter, MP_LIB_TITLE, &old_title, -1);

	gchar *t = mp_sanitize_text(title, 300);
	gchar *ar = mp_sanitize_text(artist, 300);
	gchar *al = mp_sanitize_text(album, 300);
	set_text_cols(lib, &row->iter, t != NULL && *t ? t : old_title,
		      ar != NULL ? ar : "", al != NULL ? al : "");
	set_duration(lib, &row->iter, duration);
	guint track = 0;
	if (tags != NULL)
		gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &track);
	gtk_list_store_set(lib->store, &row->iter, MP_LIB_VIDEO, has_video,
			   MP_LIB_TRACK, track, -1);
	if (!has_video) {
		gchar *g = group_key(al, ar);
		cover_from_tags(lib, g, tags);
		g_free(g);
	}
	g_free(t);
	g_free(ar);
	g_free(al);
	g_free(old_title);
	mark_changed(lib);
}

/* ---- scanning --------------------------------------------------------- */

static gint64 mtime_of(const char *uri)
{
	gchar *path = mp_uri_to_path(uri);
	GStatBuf st;
	gint64 m = -1;
	if (path != NULL && g_stat(path, &st) == 0)
		m = (gint64)st.st_mtime;
	g_free(path);
	return m;
}

static void scan_done(MpScanResult *r, gpointer data)
{
	ScanReq *req = data;
	if (g_cancellable_is_cancelled(req->cancel)) {
		g_object_unref(req->cancel);
		g_free(req);
		return;
	}
	MpLibrary *lib = req->lib;
	lib->scanning--;
	if (req->generation != lib->generation) {
		g_object_unref(req->cancel);
		g_free(req);
		return;
	}

	GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	for (guint i = 0; i < r->uris->len; i++) {
		const char *uri = g_ptr_array_index(r->uris, i);
		g_hash_table_add(seen, (gpointer)uri);
		gint64 mtime = mtime_of(uri);
		Row *row = g_hash_table_lookup(lib->by_uri, uri);
		if (row != NULL) {
			gint64 old = 0;
			gtk_tree_model_get(GTK_TREE_MODEL(lib->store),
					   &row->iter, MP_LIB_MTIME, &old, -1);
			if (req->retag || old != mtime) {
				gtk_list_store_set(lib->store, &row->iter,
						   MP_LIB_MTIME, mtime, -1);
				mp_discover_queue(lib->disc, row->id, uri);
			}
			continue;
		}
		gchar *name = mp_uri_display_name(uri);
		gchar *path = mp_uri_to_path(uri);
		row = add_row(lib, uri, name, "", "", -1,
			      mp_is_video_name(path), mtime, now, 0);
		mp_discover_queue(lib->disc, row->id, uri);
		g_free(path);
		g_free(name);
	}

	GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);
	GHashTableIter hi;
	gpointer key;
	g_hash_table_iter_init(&hi, lib->by_uri);
	while (g_hash_table_iter_next(&hi, &key, NULL)) {
		if (!g_hash_table_contains(seen, key))
			g_ptr_array_add(gone, g_strdup(key));
	}
	for (guint i = 0; i < gone->len; i++)
		remove_row(lib, g_ptr_array_index(gone, i));
	g_ptr_array_free(gone, TRUE);
	g_hash_table_destroy(seen);

	g_object_unref(req->cancel);
	g_free(req);
	mark_changed(lib);
}

void mp_library_scan(MpLibrary *lib, gchar **music_dirs, gchar **video_dirs,
		     gboolean retag)
{
	GPtrArray *in = g_ptr_array_new_with_free_func(g_free);
	gchar **lists[] = { music_dirs, video_dirs };
	for (int l = 0; l < 2; l++) {
		for (gchar **d = lists[l]; d != NULL && *d != NULL; d++) {
			if (!g_file_test(*d, G_FILE_TEST_IS_DIR))
				continue;
			GFile *f = g_file_new_for_path(*d);
			g_ptr_array_add(in, g_file_get_uri(f));
			g_object_unref(f);
		}
	}
	lib->generation++;
	if (in->len == 0) {
		/* No folder at all: the library is empty. */
		gtk_list_store_clear(lib->store);
		g_hash_table_remove_all(lib->by_id);
		g_hash_table_remove_all(lib->by_uri);
		mp_discover_clear(lib->disc);
		g_ptr_array_free(in, TRUE);
		mark_changed(lib);
		return;
	}
	g_ptr_array_add(in, NULL);
	ScanReq *req = g_new0(ScanReq, 1);
	req->lib = lib;
	req->cancel = g_object_ref(lib->cancel);
	req->generation = lib->generation;
	req->retag = retag;
	lib->scanning++;
	mp_scan_async((gchar **)in->pdata, lib->cancel, scan_done, req);
	g_ptr_array_free(in, TRUE);
	if (lib->changed != NULL)
		lib->changed(lib, lib->data);
}

gboolean mp_library_busy(MpLibrary *lib)
{
	return lib->scanning > 0;
}

/* ---- recent ----------------------------------------------------------- */

static void recent_free(gpointer p)
{
	MpRecent *r = p;
	g_free(r->uri);
	g_free(r->title);
	g_free(r->artist);
	g_free(r);
}

static gchar *recent_file(void)
{
	gchar *dir = mp_settings_dir();
	gchar *path = g_build_filename(dir, "recent", NULL);
	g_free(dir);
	return path;
}

static void load_recent(MpLibrary *lib)
{
	gchar *path = recent_file();
	gchar *text = NULL;
	if (g_file_get_contents(path, &text, NULL, NULL) &&
	    g_utf8_validate(text, -1, NULL)) {
		gchar **lines = g_strsplit(text, "\n", -1);
		for (gchar **l = lines; *l != NULL; l++) {
			gchar **f = g_strsplit(*l, "\t", 4);
			if (g_strv_length(f) == 4 &&
			    strstr(f[0], "://") != NULL &&
			    lib->recent->len < RECENT_MAX) {
				MpRecent *r = g_new0(MpRecent, 1);
				r->uri = g_strdup(f[0]);
				r->video = f[1][0] == '1';
				r->title = g_strdup(f[2]);
				r->artist = g_strdup(f[3]);
				g_ptr_array_add(lib->recent, r);
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
	}
	g_free(text);
	g_free(path);
}

static void save_recent(MpLibrary *lib)
{
	GString *s = g_string_new(NULL);
	for (guint i = 0; i < lib->recent->len; i++) {
		MpRecent *r = g_ptr_array_index(lib->recent, i);
		g_string_append_printf(s, "%s\t%d\t%s\t%s\n", r->uri,
				       r->video ? 1 : 0, r->title,
				       r->artist ? r->artist : "");
	}
	gchar *dir = mp_settings_dir();
	gchar *path = recent_file();
	if (g_mkdir_with_parents(dir, 0700) == 0)
		g_file_set_contents_full(path, s->str, (gssize)s->len,
					 G_FILE_SET_CONTENTS_CONSISTENT, 0600,
					 NULL);
	g_free(path);
	g_free(dir);
	g_string_free(s, TRUE);
}

GPtrArray *mp_library_recent(MpLibrary *lib)
{
	return lib->recent;
}

void mp_library_add_recent(MpLibrary *lib, const char *uri,
			   const char *title, const char *artist,
			   gboolean video)
{
	if (uri == NULL)
		return;
	for (guint i = 0; i < lib->recent->len; i++) {
		MpRecent *r = g_ptr_array_index(lib->recent, i);
		if (strcmp(r->uri, uri) == 0) {
			g_ptr_array_remove_index(lib->recent, i);
			break;
		}
	}
	MpRecent *r = g_new0(MpRecent, 1);
	r->uri = g_strdup(uri);
	r->title = mp_sanitize_text(title != NULL ? title : "", 300);
	r->artist = mp_sanitize_text(artist != NULL ? artist : "", 300);
	r->video = video;
	g_ptr_array_insert(lib->recent, 0, r);
	if (lib->recent->len > RECENT_MAX)
		g_ptr_array_set_size(lib->recent, RECENT_MAX);
	save_recent(lib);
}

void mp_library_clear_recent(MpLibrary *lib)
{
	g_ptr_array_set_size(lib->recent, 0);
	save_recent(lib);
}

/* ---- pictures --------------------------------------------------------- */

static void on_thumb(const char *uri, GdkPixbuf *pb, gpointer data)
{
	MpLibrary *lib = data;
	if (pb != NULL && lib->picture != NULL)
		lib->picture(lib, uri, lib->data);
}

GdkPixbuf *mp_library_album_cover(MpLibrary *lib, MpAlbum *album)
{
	gpointer pb = NULL;
	if (album == NULL)
		return NULL;
	if (g_hash_table_lookup_extended(lib->covers, album->key, NULL, &pb))
		return pb;
	/* First time: the folder's picture, else one taken from the tags
	 * on an earlier run. */
	pb = mp_cover_from_folder(album->first_uri, NULL);
	if (pb == NULL) {
		gchar *path = cover_cache_path(album->key);
		pb = gdk_pixbuf_new_from_file(path, NULL);
		g_free(path);
	}
	if (pb != NULL)
		pb = shrink(pb);
	g_hash_table_insert(lib->covers, g_strdup(album->key), pb);
	return pb;
}

GdkPixbuf *mp_library_video_picture(MpLibrary *lib, const char *uri)
{
	return mp_thumbs_get(lib->thumbs, uri);
}

GdkPixbuf *mp_library_picture_for(MpLibrary *lib, const char *uri)
{
	if (uri == NULL)
		return NULL;
	Row *row = g_hash_table_lookup(lib->by_uri, uri);
	if (row != NULL) {
		gboolean video;
		gchar *group;
		gtk_tree_model_get(GTK_TREE_MODEL(lib->store), &row->iter,
				   MP_LIB_VIDEO, &video, MP_LIB_GROUP, &group,
				   -1);
		GdkPixbuf *pb = NULL;
		if (video)
			pb = mp_thumbs_get(lib->thumbs, uri);
		else
			pb = mp_library_album_cover(
				lib, g_hash_table_lookup(lib->albums, group));
		g_free(group);
		return pb;
	}
	/* Played from somewhere outside the library. */
	gchar *path = mp_uri_to_path(uri);
	gboolean video = path != NULL && mp_is_video_name(path);
	g_free(path);
	if (video)
		return mp_thumbs_get(lib->thumbs, uri);
	gpointer pb = NULL;
	if (g_hash_table_lookup_extended(lib->uri_covers, uri, NULL, &pb))
		return pb;
	pb = mp_cover_from_folder(uri, NULL);
	if (pb != NULL)
		pb = shrink(pb);
	g_hash_table_insert(lib->uri_covers, g_strdup(uri), pb);
	return pb;
}

/* ---- queries ---------------------------------------------------------- */

GtkListStore *mp_library_store(MpLibrary *lib)
{
	return lib->store;
}

guint mp_library_count(MpLibrary *lib, gboolean video)
{
	guint n = 0;
	GtkTreeModel *m = GTK_TREE_MODEL(lib->store);
	GtkTreeIter it;
	for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
	     ok = gtk_tree_model_iter_next(m, &it)) {
		gboolean v;
		gtk_tree_model_get(m, &it, MP_LIB_VIDEO, &v, -1);
		if (!v == !video)
			n++;
	}
	return n;
}

GPtrArray *mp_library_albums(MpLibrary *lib)
{
	return lib->album_list;
}

GPtrArray *mp_library_artists(MpLibrary *lib)
{
	return lib->artist_list;
}

MpAlbum *mp_library_album(MpLibrary *lib, const char *key)
{
	return key != NULL ? g_hash_table_lookup(lib->albums, key) : NULL;
}

MpArtist *mp_library_artist(MpLibrary *lib, const char *key)
{
	return key != NULL ? g_hash_table_lookup(lib->artists, key) : NULL;
}

gboolean mp_library_lookup(MpLibrary *lib, const char *uri, gchar **title,
			   gchar **artist, gchar **album, gint64 *duration)
{
	Row *row = uri != NULL ? g_hash_table_lookup(lib->by_uri, uri) : NULL;
	if (row == NULL)
		return FALSE;
	gchar *t, *ar, *al;
	gint64 d;
	gtk_tree_model_get(GTK_TREE_MODEL(lib->store), &row->iter,
			   MP_LIB_TITLE, &t, MP_LIB_ARTIST, &ar, MP_LIB_ALBUM,
			   &al, MP_LIB_DURATION, &d, -1);
	if (title != NULL)
		*title = t;
	else
		g_free(t);
	if (artist != NULL)
		*artist = ar;
	else
		g_free(ar);
	if (album != NULL)
		*album = al;
	else
		g_free(al);
	if (duration != NULL)
		*duration = d;
	return TRUE;
}

/* ---- life ------------------------------------------------------------- */

MpLibrary *mp_library_new(MpLibraryChanged changed,
			  MpLibraryPicture picture, gpointer data)
{
	MpLibrary *lib = g_new0(MpLibrary, 1);
	lib->changed = changed;
	lib->picture = picture;
	lib->data = data;
	lib->store = gtk_list_store_new(
		MP_LIB_N_COLS, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_STRING,
		G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_UINT);
	lib->by_uri = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					    g_free);
	lib->by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
	lib->albums = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					    album_free);
	lib->artists = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					     artist_free);
	lib->album_list = g_ptr_array_new();
	lib->artist_list = g_ptr_array_new();
	lib->covers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					    unref_maybe);
	lib->uri_covers = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, unref_maybe);
	lib->recent = g_ptr_array_new_with_free_func(recent_free);
	lib->cancel = g_cancellable_new();
	lib->disc = mp_discover_new(on_discovered, lib);
	lib->thumbs = mp_thumbs_new(on_thumb, lib);
	load_cache(lib);
	load_recent(lib);
	rebuild_groups(lib);
	return lib;
}

void mp_library_free(MpLibrary *lib)
{
	if (lib == NULL)
		return;
	g_cancellable_cancel(lib->cancel);
	g_object_unref(lib->cancel);
	if (lib->changed_timer != 0)
		g_source_remove(lib->changed_timer);
	if (lib->save_timer != 0) {
		g_source_remove(lib->save_timer);
		save_cache(lib);
	}
	mp_discover_free(lib->disc);
	mp_thumbs_free(lib->thumbs);
	g_hash_table_destroy(lib->albums);
	g_hash_table_destroy(lib->artists);
	g_ptr_array_free(lib->album_list, TRUE);
	g_ptr_array_free(lib->artist_list, TRUE);
	g_hash_table_destroy(lib->covers);
	g_hash_table_destroy(lib->uri_covers);
	g_ptr_array_free(lib->recent, TRUE);
	g_hash_table_destroy(lib->by_id);
	g_hash_table_destroy(lib->by_uri);
	g_object_unref(lib->store);
	g_free(lib);
}
