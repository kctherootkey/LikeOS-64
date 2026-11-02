// LikeOS Explorer: the Gallery's collection.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-gallery.h"
#include "backend/ex-image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#define MAX_DEPTH 8
#define MAX_ITEMS 20000

static void walk(GFile *dir, int depth, GPtrArray *out, GHashTable *seen,
		 GCancellable *c)
{
	GFileEnumerator *en;
	GFileInfo *info;
	char *key;

	if (depth > MAX_DEPTH || out->len >= MAX_ITEMS ||
	    g_cancellable_is_cancelled(c))
		return;
	/* Each folder once, however it is reached (a folder of the
	 * collection inside another, a link back up). */
	key = g_file_get_uri(dir);
	if (g_hash_table_contains(seen, key)) {
		g_free(key);
		return;
	}
	g_hash_table_add(seen, key);
	en = g_file_enumerate_children(dir, EX_FILE_ATTRS, G_FILE_QUERY_INFO_NONE,
				       c, NULL);
	if (!en)
		return;
	while ((info = g_file_enumerator_next_file(en, c, NULL))) {
		GFile *child = g_file_enumerator_get_child(en, info);

		if (g_file_info_get_is_hidden(info) ||
		    g_file_info_get_is_backup(info)) {
			/* Not shown in Explorer, not in the Gallery either. */
		} else if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
			walk(child, depth + 1, out, seen, c);
		} else if (out->len < MAX_ITEMS) {
			ExFile *f = ex_file_new(child, info);
			const char *kind = ex_file_get_kind(f);

			if (!g_strcmp0(kind, "image") || !g_strcmp0(kind, "video")) {
				double a = !g_strcmp0(kind, "video") ? 16.0 / 9.0 :
								      4.0 / 3.0;
				char *path = g_file_get_path(child);
				int w = 0, h = 0;

				if (path && !g_strcmp0(kind, "image") &&
				    gdk_pixbuf_get_file_info(path, &w, &h) && w > 0 &&
				    h > 0) {
					a = (double)w / h;
					if (ex_image_jpeg_orientation(path) >= 5)
						a = 1.0 / a;
				}
				g_free(path);
				g_object_set_data_full(G_OBJECT(f), "ex-aspect",
						       g_memdup2(&a, sizeof(a)),
						       g_free);
				g_ptr_array_add(out, f);
			} else {
				g_object_unref(f);
			}
		}
		g_object_unref(child);
		g_object_unref(info);
	}
	g_object_unref(en);
}

static int newest_first(gconstpointer a, gconstpointer b)
{
	gint64 ta = ex_file_get_mtime(*(ExFile **)a);
	gint64 tb = ex_file_get_mtime(*(ExFile **)b);

	return ta < tb ? 1 : ta > tb ? -1 : 0;
}

static void collect_thread(GTask *task, gpointer src, gpointer data,
			   GCancellable *c)
{
	GPtrArray *folders = data;
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						 NULL);

	for (guint i = 0; i < folders->len; i++)
		walk(g_ptr_array_index(folders, i), 0, out, seen, c);
	g_hash_table_unref(seen);
	g_ptr_array_sort(out, newest_first);
	if (g_cancellable_is_cancelled(c)) {
		g_ptr_array_unref(out);
		g_task_return_error_if_cancelled(task);
		return;
	}
	g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

void ex_gallery_collect_async(GPtrArray *folders, GCancellable *c,
			      GAsyncReadyCallback done, gpointer user_data)
{
	GTask *task = g_task_new(NULL, c, done, user_data);
	GPtrArray *copy = g_ptr_array_new_with_free_func(g_object_unref);

	for (guint i = 0; i < folders->len; i++)
		g_ptr_array_add(copy, g_object_ref(g_ptr_array_index(folders, i)));
	g_task_set_task_data(task, copy, (GDestroyNotify)g_ptr_array_unref);
	g_task_run_in_thread(task, collect_thread);
	g_object_unref(task);
}

GPtrArray *ex_gallery_collect_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}

double ex_gallery_aspect(ExFile *f)
{
	double *a = g_object_get_data(G_OBJECT(f), "ex-aspect");

	return a && *a > 0.05 && *a < 20 ? *a : 4.0 / 3.0;
}
