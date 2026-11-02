// LikeOS Explorer: the Trash.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-trash.h"
#include "backend/ex-file.h"
#include "backend/ex-location.h"

#include <string.h>

GFile *ex_trash_info_dir(void)
{
	char *p = g_build_filename(g_get_user_data_dir(), "Trash", "info",
				   NULL);
	GFile *f = g_file_new_for_path(p);

	g_free(p);
	return f;
}

GFile *ex_trash_info_file_for(GFile *trash_item)
{
	char *base = g_file_get_basename(trash_item);
	char *name = g_strconcat(base, ".trashinfo", NULL);
	GFile *dir = ex_trash_info_dir();
	GFile *f = g_file_get_child(dir, name);

	g_object_unref(dir);
	g_free(name);
	g_free(base);
	return f;
}

/* The record's Path= is relative to the trash's top directory when it does
 * not start with "/"; for the home trash that is the root. */
static gboolean parse_info(const char *data, gsize len, char **orig,
			   gint64 *deleted)
{
	GKeyFile *kf = g_key_file_new();
	gboolean ok = FALSE;

	if (g_key_file_load_from_data(kf, data, len, G_KEY_FILE_NONE, NULL)) {
		char *path = g_key_file_get_string(kf, "Trash Info", "Path",
						   NULL);
		char *date = g_key_file_get_string(kf, "Trash Info",
						   "DeletionDate", NULL);
		if (path) {
			char *un = g_uri_unescape_string(path, NULL);

			if (!un)
				un = g_strdup(path);
			if (orig && un[0] == '/') {
				*orig = un;
			} else if (orig) {
				*orig = g_build_filename("/", un, NULL);
				g_free(un);
			} else {
				g_free(un);
			}
			ok = TRUE;
		}
		if (deleted) {
			*deleted = 0;
			if (date) {
				GTimeZone *tz = g_time_zone_new_local();
				GDateTime *dt = g_date_time_new_from_iso8601(
					date, tz);

				if (dt) {
					*deleted = g_date_time_to_unix(dt);
					g_date_time_unref(dt);
				}
				g_time_zone_unref(tz);
			}
		}
		g_free(path);
		g_free(date);
	}
	g_key_file_unref(kf);
	return ok;
}

gboolean ex_trash_read_info(GFile *trash_item, char **orig_path,
			    gint64 *deleted, GError **error)
{
	GFile *info = ex_trash_info_file_for(trash_item);
	char *data = NULL;
	gsize len = 0;
	gboolean ok = g_file_load_contents(info, NULL, &data, &len, NULL,
					   error);

	g_object_unref(info);
	if (!ok)
		return FALSE;
	ok = parse_info(data, len, orig_path, deleted);
	g_free(data);
	if (!ok)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			    "The trash record is damaged");
	return ok;
}

GFile *ex_trash_find_item(const char *orig_path)
{
	GFile *dir = ex_trash_info_dir();
	GFileEnumerator *en = g_file_enumerate_children(
		dir, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
		NULL, NULL);
	GFile *best = NULL;
	gint64 best_time = G_MININT64;
	GFileInfo *fi;

	if (!en) {
		g_object_unref(dir);
		return NULL;
	}
	while ((fi = g_file_enumerator_next_file(en, NULL, NULL))) {
		const char *name = g_file_info_get_name(fi);

		if (g_str_has_suffix(name, ".trashinfo")) {
			GFile *rec = g_file_get_child(dir, name);
			char *data = NULL, *orig = NULL;
			gsize len;
			gint64 when = 0;

			if (g_file_load_contents(rec, NULL, &data, &len, NULL,
						 NULL) &&
			    parse_info(data, len, &orig, &when) &&
			    strcmp(orig, orig_path) == 0 && when >= best_time) {
				char *item = g_strndup(
					name, strlen(name) - strlen(".trashinfo"));
				GFile *files = ex_trash_files_dir();

				g_clear_object(&best);
				best = g_file_get_child(files, item);
				best_time = when;
				g_object_unref(files);
				g_free(item);
			}
			g_free(orig);
			g_free(data);
			g_object_unref(rec);
		}
		g_object_unref(fi);
	}
	g_object_unref(en);
	g_object_unref(dir);
	return best;
}

static void list_thread(GTask *task, gpointer src, gpointer data,
			GCancellable *cancel)
{
	GFile *files = ex_trash_files_dir();
	GError *err = NULL;
	GFileEnumerator *en = g_file_enumerate_children(
		files, EX_FILE_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		cancel, &err);
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GFileInfo *fi;

	if (!en) {
		/* No trash yet is an empty trash. */
		if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(&err);
			g_task_return_pointer(task, out,
					      (GDestroyNotify)g_ptr_array_unref);
		} else {
			g_ptr_array_unref(out);
			g_task_return_error(task, err);
		}
		g_object_unref(files);
		return;
	}
	while ((fi = g_file_enumerator_next_file(en, cancel, NULL))) {
		GFile *child = g_file_enumerator_get_child(en, fi);
		ExFile *ef = ex_file_new(child, fi);
		char *orig = NULL;
		gint64 when = 0;

		if (ex_trash_read_info(child, &orig, &when, NULL))
			ex_file_set_trash_info(ef, orig, when);
		else
			ex_file_set_trash_info(ef, "", 0);
		g_ptr_array_add(out, ef);
		g_free(orig);
		g_object_unref(child);
		g_object_unref(fi);
	}
	g_object_unref(en);
	g_object_unref(files);
	if (!g_task_return_error_if_cancelled(task))
		g_task_return_pointer(task, out,
				      (GDestroyNotify)g_ptr_array_unref);
	else
		g_ptr_array_unref(out);
}

void ex_trash_list_async(GCancellable *cancellable, GAsyncReadyCallback done,
			 gpointer user_data)
{
	GTask *task = g_task_new(NULL, cancellable, done, user_data);

	g_task_set_source_tag(task, ex_trash_list_async);
	g_task_run_in_thread(task, list_thread);
	g_object_unref(task);
}

GPtrArray *ex_trash_list_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}

guint ex_trash_count(void)
{
	GFile *files = ex_trash_files_dir();
	GFileEnumerator *en = g_file_enumerate_children(
		files, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
		NULL, NULL);
	guint n = 0;
	GFileInfo *fi;

	if (en) {
		while ((fi = g_file_enumerator_next_file(en, NULL, NULL))) {
			n++;
			g_object_unref(fi);
		}
		g_object_unref(en);
	}
	g_object_unref(files);
	return n;
}

gboolean ex_trash_is_empty(void)
{
	GFile *files = ex_trash_files_dir();
	GFileEnumerator *en = g_file_enumerate_children(
		files, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NONE,
		NULL, NULL);
	GFileInfo *fi = en ? g_file_enumerator_next_file(en, NULL, NULL) : NULL;
	gboolean empty = fi == NULL;

	g_clear_object(&fi);
	g_clear_object(&en);
	g_object_unref(files);
	return empty;
}
