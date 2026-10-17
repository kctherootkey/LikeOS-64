// LikeOS -- Media Player: walking folders for media files.
//
// Runs in a GTask worker thread, because a folder of music on a slow disk
// takes seconds to list and the window must keep painting meanwhile.  Only
// GLib calls (and GStreamer's thread-safe URI helpers) are made here; the
// play queue is touched by the callback on the main loop.
//
// A folder tree can contain symbolic links back to its own ancestors.  Each
// directory's device and inode are remembered, so a loop is walked once and
// not forever; depth and the total entry count are capped as well.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#include "playlist.h"
#include "scan.h"
#include "util.h"

#define SCAN_MAX_DEPTH 24

typedef struct {
	gchar **inputs;
	MpScanDone done;
	gpointer user_data;
} ScanJob;

typedef struct {
	MpScanResult *r;
	GHashTable *visited;	/* "dev:ino" of directories entered */
	GCancellable *cancel;
} ScanCtx;

static void result_free(MpScanResult *r)
{
	if (r == NULL)
		return;
	g_ptr_array_free(r->uris, TRUE);
	g_ptr_array_free(r->titles, TRUE);
	g_ptr_array_free(r->problems, TRUE);
	g_free(r);
}

static gboolean full(ScanCtx *c)
{
	if (c->r->uris->len >= MP_PLAYLIST_MAX) {
		c->r->truncated = TRUE;
		return TRUE;
	}
	return g_cancellable_is_cancelled(c->cancel);
}

static void add(ScanCtx *c, gchar *uri, gchar *title)
{
	if (full(c)) {
		g_free(uri);
		g_free(title);
		return;
	}
	g_ptr_array_add(c->r->uris, uri);
	g_ptr_array_add(c->r->titles, title);
}

static void problem(ScanCtx *c, const char *what, const char *why)
{
	if (c->r->problems->len < 50)
		g_ptr_array_add(c->r->problems,
				g_strdup_printf("%s: %s", what, why));
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
	return mp_compare_filenames(*(const char *const *)a,
				    *(const char *const *)b);
}

static void walk_dir(ScanCtx *c, const char *dir, int depth)
{
	if (full(c))
		return;
	if (depth > SCAN_MAX_DEPTH) {
		problem(c, dir, "folders nested too deeply; not entered");
		return;
	}

	GStatBuf st;
	if (g_stat(dir, &st) != 0)
		return;
	gchar *key = g_strdup_printf("%llu:%llu",
				     (unsigned long long)st.st_dev,
				     (unsigned long long)st.st_ino);
	if (g_hash_table_contains(c->visited, key)) {
		g_free(key);
		return;
	}
	g_hash_table_add(c->visited, key);

	GError *err = NULL;
	GDir *d = g_dir_open(dir, 0, &err);
	if (d == NULL) {
		gchar *disp = g_filename_display_name(dir);
		problem(c, disp, err->message);
		g_free(disp);
		g_clear_error(&err);
		return;
	}

	GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
	const char *name;
	while ((name = g_dir_read_name(d)) != NULL) {
		if (name[0] == '.')	/* hidden: thumbnails, trash, VCS */
			continue;
		gchar *full_path = g_build_filename(dir, name, NULL);
		if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
			g_ptr_array_add(dirs, full_path);
		} else if (mp_is_media_name(name) &&
			   g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
			g_ptr_array_add(files, full_path);
		} else {
			g_free(full_path);
		}
		if (files->len + dirs->len > 4 * MP_PLAYLIST_MAX)
			break;
	}
	g_dir_close(d);

	/* Files of a folder in name order, then its sub-folders in name
	 * order: an album's tracks before its "Bonus" folder. */
	g_ptr_array_sort(files, cmp_names);
	g_ptr_array_sort(dirs, cmp_names);

	for (guint i = 0; i < files->len && !full(c); i++) {
		GFile *f = g_file_new_for_path(g_ptr_array_index(files, i));
		add(c, g_file_get_uri(f), NULL);
		g_object_unref(f);
	}
	for (guint i = 0; i < dirs->len && !full(c); i++)
		walk_dir(c, g_ptr_array_index(dirs, i), depth + 1);

	g_ptr_array_free(files, TRUE);
	g_ptr_array_free(dirs, TRUE);
}

static void scan_one(ScanCtx *c, const char *uri)
{
	gchar *path = mp_uri_to_path(uri);
	if (path == NULL) {
		/* A stream: GStreamer decides at play time. */
		if (mp_uri_is_playable_scheme(uri))
			add(c, g_strdup(uri), NULL);
		else
			problem(c, uri, "no source element for this kind "
					"of address is installed");
		return;
	}

	gchar *disp = g_filename_display_name(path);
	GStatBuf st;
	if (g_stat(path, &st) != 0) {
		problem(c, disp, g_strerror(errno));
	} else if (S_ISDIR(st.st_mode)) {
		walk_dir(c, path, 0);
	} else if (mp_is_playlist_name(path)) {
		GPtrArray *uris = NULL, *titles = NULL;
		GError *err = NULL;
		if (mp_playlist_file_load(path, &uris, &titles, &err)) {
			for (guint i = 0; i < uris->len; i++) {
				add(c, g_strdup(g_ptr_array_index(uris, i)),
				    g_strdup(g_ptr_array_index(titles, i)));
			}
			g_ptr_array_free(uris, TRUE);
			g_ptr_array_free(titles, TRUE);
		} else {
			problem(c, disp, err->message);
			g_clear_error(&err);
		}
	} else if (S_ISREG(st.st_mode)) {
		/* Taken even with an unknown extension: the user picked it,
		 * and GStreamer's type detection reads content, not names. */
		add(c, g_strdup(uri), NULL);
	} else {
		problem(c, disp, "not a regular file");
	}
	g_free(disp);
	g_free(path);
}

static void scan_thread(GTask *task, gpointer source, gpointer task_data,
			GCancellable *cancel)
{
	ScanJob *job = task_data;
	(void)source;

	MpScanResult *r = g_new0(MpScanResult, 1);
	r->uris = g_ptr_array_new_with_free_func(g_free);
	r->titles = g_ptr_array_new_with_free_func(g_free);
	r->problems = g_ptr_array_new_with_free_func(g_free);

	ScanCtx c = { r, g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					       NULL),
		      cancel };
	for (gchar **in = job->inputs; *in != NULL && !full(&c); in++)
		scan_one(&c, *in);
	g_hash_table_destroy(c.visited);

	g_task_return_pointer(task, r, (GDestroyNotify)result_free);
}

static void job_free(gpointer p)
{
	ScanJob *job = p;
	g_strfreev(job->inputs);
	g_free(job);
}

static void scan_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	(void)source;
	(void)data;
	GTask *task = G_TASK(res);
	ScanJob *job = g_task_get_task_data(task);
	MpScanResult *r = g_task_propagate_pointer(task, NULL);
	if (r != NULL) {
		job->done(r, job->user_data);
		result_free(r);
	}
}

void mp_scan_async(gchar **inputs, GCancellable *cancel, MpScanDone done,
		   gpointer user_data)
{
	ScanJob *job = g_new0(ScanJob, 1);
	job->inputs = g_strdupv(inputs);
	job->done = done;
	job->user_data = user_data;

	GTask *task = g_task_new(NULL, cancel, scan_finished, NULL);
	g_task_set_task_data(task, job, job_free);
	g_task_run_in_thread(task, scan_thread);
	g_object_unref(task);
}
