// LikeOS Explorer: recently used files.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-recent.h"
#include "backend/ex-file.h"
#include "backend/ex-util.h"

#include <string.h>

GFile *ex_recent_file(void)
{
	char *p = g_build_filename(g_get_user_data_dir(), "recently-used.xbel",
				   NULL);
	GFile *f = g_file_new_for_path(p);

	g_free(p);
	return f;
}

static char *recent_path(void)
{
	return g_build_filename(g_get_user_data_dir(), "recently-used.xbel",
				NULL);
}

typedef struct {
	char *uri;
	gint64 when;
} Entry;

static int by_when(gconstpointer a, gconstpointer b)
{
	const Entry *x = *(const Entry *const *)a, *y = *(const Entry *const *)b;

	return x->when < y->when ? 1 : x->when > y->when ? -1 : 0;
}

static gboolean below_hidden(const char *path)
{
	const char *home = g_get_home_dir();
	const char *p = path;

	if (g_str_has_prefix(path, home))
		p = path + strlen(home);
	for (const char *s = p; (s = strchr(s, '/')); s++)
		if (s[1] == '.')
			return TRUE;
	return FALSE;
}

typedef struct {
	guint max;
	gboolean include_hidden;
} ListArgs;

static void list_thread(GTask *task, gpointer src, gpointer data,
			GCancellable *cancel)
{
	ListArgs *a = data;
	GBookmarkFile *bf = g_bookmark_file_new();
	char *path = recent_path();
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GError *err = NULL;

	if (!g_bookmark_file_load_from_file(bf, path, &err)) {
		g_free(path);
		g_bookmark_file_free(bf);
		/* No list yet is an empty list. */
		g_clear_error(&err);
		g_task_return_pointer(task, out,
				      (GDestroyNotify)g_ptr_array_unref);
		return;
	}
	g_free(path);

	gsize n = 0;
	char **uris = g_bookmark_file_get_uris(bf, &n);
	GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);

	for (gsize i = 0; i < n; i++) {
		Entry *e;
		GDateTime *dt;

		if (!g_str_has_prefix(uris[i], "file://"))
			continue;
		e = g_new0(Entry, 1);
		e->uri = uris[i];
		dt = g_bookmark_file_get_modified_date_time(bf, uris[i], NULL);
		e->when = dt ? g_date_time_to_unix(dt) : 0;
		g_ptr_array_add(entries, e);
	}
	g_ptr_array_sort(entries, by_when);
	for (guint i = 0; i < entries->len && out->len < a->max; i++) {
		Entry *e = g_ptr_array_index(entries, i);
		GFile *f = g_file_new_for_uri(e->uri);
		char *p = g_file_get_path(f);

		if (p && (a->include_hidden || !below_hidden(p))) {
			ExFile *ef = ex_file_new_query(f, cancel, NULL);

			/* Gone, or not this user's to read: not listed. */
			if (ef && (ex_file_get_caps(ef) & EX_CAP_READ) &&
			    !ex_file_is_dir(ef))
				g_ptr_array_add(out, ef);
			else if (ef)
				g_object_unref(ef);
		}
		g_free(p);
		g_object_unref(f);
	}
	g_ptr_array_unref(entries);
	g_strfreev(uris);
	g_bookmark_file_free(bf);
	if (!g_task_return_error_if_cancelled(task))
		g_task_return_pointer(task, out,
				      (GDestroyNotify)g_ptr_array_unref);
	else
		g_ptr_array_unref(out);
}

void ex_recent_list_async(guint max, gboolean include_hidden,
			  GCancellable *cancellable, GAsyncReadyCallback done,
			  gpointer user_data)
{
	GTask *task = g_task_new(NULL, cancellable, done, user_data);
	ListArgs *a = g_new0(ListArgs, 1);

	a->max = max;
	a->include_hidden = include_hidden;
	g_task_set_task_data(task, a, g_free);
	g_task_run_in_thread(task, list_thread);
	g_object_unref(task);
}

GPtrArray *ex_recent_list_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}

static const char empty_xbel[] =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<xbel version=\"1.0\"\n"
	"      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
	"      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n"
	">\n</xbel>\n";

gboolean ex_recent_clear(GError **error)
{
	char *path = recent_path();
	gboolean ok = g_file_set_contents(path, empty_xbel, -1, error);

	g_free(path);
	return ok;
}

gboolean ex_recent_forget(GFile *file, GError **error)
{
	GBookmarkFile *bf = g_bookmark_file_new();
	char *path = recent_path();
	char *uri = g_file_get_uri(file);
	gboolean ok = g_bookmark_file_load_from_file(bf, path, error);

	if (ok && g_bookmark_file_remove_item(bf, uri, NULL))
		ok = g_bookmark_file_to_file(bf, path, error);
	g_free(uri);
	g_free(path);
	g_bookmark_file_free(bf);
	return ok;
}
