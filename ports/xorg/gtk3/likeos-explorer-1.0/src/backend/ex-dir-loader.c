// LikeOS -- File Explorer: reading a folder.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-dir-loader.h"

#define EX_DIR_BATCH 200

typedef struct {
	GFile *dir;
	GFileEnumerator *en;
	ExDirBatchFunc batch;
	gpointer user_data;
} LoadState;

static void load_state_free(LoadState *s)
{
	g_clear_object(&s->dir);
	g_clear_object(&s->en);
	g_free(s);
}

static void next_batch(GTask *task);

static void got_files(GObject *src, GAsyncResult *res, gpointer data)
{
	GTask *task = data;
	LoadState *s = g_task_get_task_data(task);
	GError *err = NULL;
	GList *infos = g_file_enumerator_next_files_finish(
		G_FILE_ENUMERATOR(src), res, &err);

	if (err) {
		g_task_return_error(task, err);
		g_object_unref(task);
		return;
	}
	if (!infos) {
		g_file_enumerator_close_async(s->en, G_PRIORITY_LOW, NULL, NULL,
					      NULL);
		g_task_return_boolean(task, TRUE);
		g_object_unref(task);
		return;
	}
	GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
	for (GList *l = infos; l; l = l->next) {
		GFileInfo *info = l->data;
		GFile *child = g_file_enumerator_get_child(s->en, info);

		g_ptr_array_add(files, ex_file_new(child, info));
		g_object_unref(child);
	}
	g_list_free_full(infos, g_object_unref);
	if (!g_task_return_error_if_cancelled(task)) {
		s->batch(files, s->user_data);
		next_batch(task);
	} else {
		g_object_unref(task);
	}
	g_ptr_array_unref(files);
}

static void next_batch(GTask *task)
{
	LoadState *s = g_task_get_task_data(task);

	g_file_enumerator_next_files_async(s->en, EX_DIR_BATCH,
					   G_PRIORITY_DEFAULT,
					   g_task_get_cancellable(task),
					   got_files, task);
}

static void enumerated(GObject *src, GAsyncResult *res, gpointer data)
{
	GTask *task = data;
	LoadState *s = g_task_get_task_data(task);
	GError *err = NULL;

	s->en = g_file_enumerate_children_finish(G_FILE(src), res, &err);
	if (!s->en) {
		g_task_return_error(task, err);
		g_object_unref(task);
		return;
	}
	next_batch(task);
}

void ex_dir_load_async(GFile *dir, GCancellable *cancellable,
		       ExDirBatchFunc batch, GAsyncReadyCallback done,
		       gpointer user_data)
{
	GTask *task = g_task_new(dir, cancellable, done, user_data);
	LoadState *s = g_new0(LoadState, 1);

	s->dir = g_object_ref(dir);
	s->batch = batch;
	s->user_data = user_data;
	g_task_set_task_data(task, s, (GDestroyNotify)load_state_free);
	g_task_set_source_tag(task, ex_dir_load_async);
	/* Links are followed: a link to a folder lists, sorts and opens as a
	 * folder.  GIO still reports it as a link (standard::is-symlink), and
	 * a broken one comes back with no type of its own. */
	g_file_enumerate_children_async(dir, EX_FILE_ATTRS,
					G_FILE_QUERY_INFO_NONE,
					G_PRIORITY_DEFAULT, cancellable,
					enumerated, task);
}

gboolean ex_dir_load_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_boolean(G_TASK(res), error);
}
