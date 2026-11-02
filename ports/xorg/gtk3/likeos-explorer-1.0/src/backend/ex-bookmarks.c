// LikeOS -- File Explorer: bookmarks.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-bookmarks.h"

#include <string.h>

struct _ExBookmarks {
	GObject parent;
	GFile *path;
	GFileMonitor *mon;
	GPtrArray *list;
};

G_DEFINE_TYPE(ExBookmarks, ex_bookmarks, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void bm_free(ExBookmark *b)
{
	g_clear_object(&b->file);
	g_free(b->label);
	g_free(b);
}

static void ex_bookmarks_class_init(ExBookmarksClass *k)
{
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_bookmarks_init(ExBookmarks *b)
{
	b->list = g_ptr_array_new_with_free_func((GDestroyNotify)bm_free);
}

static void load(ExBookmarks *b)
{
	char *data = NULL;

	g_ptr_array_set_size(b->list, 0);
	if (!g_file_load_contents(b->path, NULL, &data, NULL, NULL, NULL))
		return;
	char **lines = g_strsplit(data, "\n", -1);

	for (int i = 0; lines[i]; i++) {
		char *line = g_strstrip(lines[i]);
		char *sp;
		ExBookmark *bm;

		if (!*line)
			continue;
		sp = strchr(line, ' ');
		if (sp)
			*sp++ = '\0';
		bm = g_new0(ExBookmark, 1);
		bm->file = g_file_new_for_uri(line);
		bm->label = sp && *sp ? g_strdup(sp) : NULL;
		g_ptr_array_add(b->list, bm);
	}
	g_strfreev(lines);
	g_free(data);
}

static void save(ExBookmarks *b)
{
	GString *s = g_string_new(NULL);
	GFile *dir = g_file_get_parent(b->path);

	for (guint i = 0; i < b->list->len; i++) {
		ExBookmark *bm = g_ptr_array_index(b->list, i);
		char *uri = g_file_get_uri(bm->file);

		g_string_append(s, uri);
		if (bm->label)
			g_string_append_printf(s, " %s", bm->label);
		g_string_append_c(s, '\n');
		g_free(uri);
	}
	(void)g_file_make_directory_with_parents(dir, NULL, NULL);
	(void)g_file_replace_contents(b->path, s->str, s->len, NULL, FALSE,
				      G_FILE_CREATE_NONE, NULL, NULL, NULL);
	g_object_unref(dir);
	g_string_free(s, TRUE);
}

static void file_changed(GFileMonitor *m, GFile *f, GFile *o,
			 GFileMonitorEvent ev, gpointer data)
{
	ExBookmarks *b = data;

	if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
	    ev == G_FILE_MONITOR_EVENT_CREATED ||
	    ev == G_FILE_MONITOR_EVENT_DELETED ||
	    ev == G_FILE_MONITOR_EVENT_RENAMED ||
	    ev == G_FILE_MONITOR_EVENT_MOVED_IN) {
		load(b);
		g_signal_emit(b, sigs[SIG_CHANGED], 0);
	}
}

ExBookmarks *ex_bookmarks_get_default(void)
{
	static ExBookmarks *the;

	if (!the) {
		char *p = g_build_filename(g_get_user_config_dir(), "gtk-3.0",
					   "bookmarks", NULL);

		the = g_object_new(EX_TYPE_BOOKMARKS, NULL);
		the->path = g_file_new_for_path(p);
		g_free(p);
		load(the);
		the->mon = g_file_monitor_file(the->path,
					       G_FILE_MONITOR_WATCH_MOVES, NULL,
					       NULL);
		if (the->mon)
			g_signal_connect(the->mon, "changed",
					 G_CALLBACK(file_changed), the);
	}
	return the;
}

GPtrArray *ex_bookmarks_list(ExBookmarks *b)
{
	return b->list;
}

static int find(ExBookmarks *b, GFile *f)
{
	for (guint i = 0; i < b->list->len; i++)
		if (g_file_equal(((ExBookmark *)g_ptr_array_index(b->list, i))
					 ->file,
				 f))
			return (int)i;
	return -1;
}

gboolean ex_bookmarks_contains(ExBookmarks *b, GFile *f)
{
	return find(b, f) >= 0;
}

void ex_bookmarks_add(ExBookmarks *b, GFile *f, const char *label)
{
	ExBookmark *bm;

	if (find(b, f) >= 0)
		return;
	bm = g_new0(ExBookmark, 1);
	bm->file = g_object_ref(f);
	bm->label = label ? g_strdup(label) : NULL;
	g_ptr_array_add(b->list, bm);
	save(b);
	g_signal_emit(b, sigs[SIG_CHANGED], 0);
}

void ex_bookmarks_remove(ExBookmarks *b, GFile *f)
{
	int i = find(b, f);

	if (i < 0)
		return;
	g_ptr_array_remove_index(b->list, (guint)i);
	save(b);
	g_signal_emit(b, sigs[SIG_CHANGED], 0);
}

void ex_bookmarks_move(ExBookmarks *b, guint from, guint to)
{
	if (from >= b->list->len || to >= b->list->len || from == to)
		return;
	gpointer item = g_ptr_array_steal_index(b->list, from);

	g_ptr_array_insert(b->list, (gint)to, item);
	save(b);
	g_signal_emit(b, sigs[SIG_CHANGED], 0);
}
