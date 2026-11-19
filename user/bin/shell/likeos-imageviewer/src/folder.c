// LikeOS -- LikeOS Image Viewer: the folder being browsed.
//
// The pictures next to the one opened, in the order LikeOS Explorer lists
// them by default: by name, in natural order (digits by value, case folded,
// the GLib file-name collation Explorer uses as well), or in the order
// named on the command line with --sort.  Hidden files are left out, unless
// one of them is what was opened.  The folder is watched, and a change
// (another program saving, copying or deleting) refreshes the list a
// moment later, in one go however many events arrived.
//
// Several files named on the command line make a fixed list instead: those
// files, in the same order, not watched.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>
#include <sys/stat.h>

#include "imageviewer.h"

typedef struct {
	char *path;
	char *key;	/* natural-order key of the name */
	char *ext;	/* lower case, for the type order */
	gint64 mtime;
	goffset size;
} Item;

struct IvFolder {
	char *dir;		/* NULL for a fixed list */
	char *keep;		/* the file opened, kept even if hidden */
	GPtrArray *items;	/* Item* in order */
	IvSort sort;
	gboolean fixed;
	GFileMonitor *mon;
	guint rescan_source;
	IvFolderChanged changed;
	gpointer changed_data;
};

static void item_free(gpointer p)
{
	Item *it = p;

	g_free(it->path);
	g_free(it->key);
	g_free(it->ext);
	g_free(it);
}

static Item *item_new(const char *path)
{
	Item *it = g_new0(Item, 1);
	char *base = g_path_get_basename(path);
	const char *dot = strrchr(base, '.');
	struct stat st;

	it->path = g_strdup(path);
	it->key = iv_name_sort_key(base);
	it->ext = g_ascii_strdown(dot ? dot + 1 : "", -1);
	if (stat(path, &st) == 0) {
		it->mtime = st.st_mtime;
		it->size = st.st_size;
	}
	g_free(base);
	return it;
}

static int cmp_name(const Item *a, const Item *b)
{
	int c = strcmp(a->key, b->key);

	return c ? c : strcmp(a->path, b->path);
}

static int cmp_items(gconstpointer pa, gconstpointer pb, gpointer data)
{
	const Item *a = *(Item *const *)pa, *b = *(Item *const *)pb;
	IvSort sort = GPOINTER_TO_INT(data);
	int c = 0;

	switch (sort) {
	case IV_SORT_NAME:
		return cmp_name(a, b);
	case IV_SORT_NAME_DESC:
		return -cmp_name(a, b);
	case IV_SORT_MTIME:
	case IV_SORT_MTIME_DESC:
		c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime;
		if (sort == IV_SORT_MTIME_DESC)
			c = -c;
		break;
	case IV_SORT_SIZE:
	case IV_SORT_SIZE_DESC:
		c = a->size < b->size ? -1 : a->size > b->size;
		if (sort == IV_SORT_SIZE_DESC)
			c = -c;
		break;
	case IV_SORT_TYPE:
		c = strcmp(a->ext, b->ext);
		break;
	}
	/* Ties in name order, so the order never depends on the listing. */
	return c ? c : cmp_name(a, b);
}

static void sort_items(IvFolder *f)
{
	g_ptr_array_sort_with_data(f->items, cmp_items, GINT_TO_POINTER(f->sort));
}

static void scan(IvFolder *f)
{
	GDir *d;
	const char *name;

	g_ptr_array_set_size(f->items, 0);
	d = g_dir_open(f->dir, 0, NULL);
	if (!d) {
		/* Unreadable folder: at least the picture itself. */
		if (f->keep && g_file_test(f->keep, G_FILE_TEST_IS_REGULAR))
			g_ptr_array_add(f->items, item_new(f->keep));
		return;
	}
	while ((name = g_dir_read_name(d))) {
		char *p = g_build_filename(f->dir, name, NULL);

		if (name[0] == '.' && g_strcmp0(p, f->keep) != 0) {
			g_free(p);
			continue;
		}
		if (iv_path_is_image(p) && g_file_test(p, G_FILE_TEST_IS_REGULAR))
			g_ptr_array_add(f->items, item_new(p));
		g_free(p);
	}
	g_dir_close(d);
	/* The file opened, even when its name does not say it is a picture
	 * (a JPEG called "photo", say). */
	if (f->keep && iv_folder_index(f, f->keep) < 0 &&
	    g_file_test(f->keep, G_FILE_TEST_IS_REGULAR))
		g_ptr_array_add(f->items, item_new(f->keep));
	sort_items(f);
}

static gboolean rescan_now(gpointer data)
{
	IvFolder *f = data;

	f->rescan_source = 0;
	scan(f);
	if (f->changed)
		f->changed(f, f->changed_data);
	return G_SOURCE_REMOVE;
}

static void monitor_event(GFileMonitor *m, GFile *file, GFile *other,
			  GFileMonitorEvent ev, gpointer data)
{
	IvFolder *f = data;

	switch (ev) {
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
	case G_FILE_MONITOR_EVENT_DELETED:
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_MOVED_IN:
	case G_FILE_MONITOR_EVENT_MOVED_OUT:
	case G_FILE_MONITOR_EVENT_RENAMED:
	case G_FILE_MONITOR_EVENT_CHANGED:
		break;
	default:
		return;
	}
	/* A burst of events (a copy of a hundred files) makes one rescan. */
	if (f->rescan_source)
		g_source_remove(f->rescan_source);
	f->rescan_source = g_timeout_add(400, rescan_now, f);
}

IvFolder *iv_folder_new(const char *path, IvSort sort)
{
	IvFolder *f = g_new0(IvFolder, 1);
	GFile *dir;

	f->items = g_ptr_array_new_with_free_func(item_free);
	f->sort = sort;
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		f->dir = g_canonicalize_filename(path, NULL);
	} else {
		char *abs = g_canonicalize_filename(path, NULL);

		f->dir = g_path_get_dirname(abs);
		f->keep = abs;
	}
	scan(f);
	dir = g_file_new_for_path(f->dir);
	f->mon = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL,
					  NULL);
	if (f->mon)
		g_signal_connect(f->mon, "changed", G_CALLBACK(monitor_event), f);
	g_object_unref(dir);
	return f;
}

IvFolder *iv_folder_new_for_list(char **paths, IvSort sort)
{
	IvFolder *f = g_new0(IvFolder, 1);

	f->items = g_ptr_array_new_with_free_func(item_free);
	f->sort = sort;
	f->fixed = TRUE;
	for (int i = 0; paths && paths[i]; i++) {
		char *abs = g_canonicalize_filename(paths[i], NULL);

		if (g_file_test(abs, G_FILE_TEST_IS_REGULAR) &&
		    iv_folder_index(f, abs) < 0)
			g_ptr_array_add(f->items, item_new(abs));
		g_free(abs);
	}
	if (f->items->len) {
		Item *first = g_ptr_array_index(f->items, 0);

		f->dir = g_path_get_dirname(first->path);
	}
	/* Named files keep the order they were named in unless a sort was
	 * asked for explicitly. */
	if (sort != IV_SORT_NAME)
		sort_items(f);
	return f;
}

void iv_folder_free(IvFolder *f)
{
	if (!f)
		return;
	if (f->rescan_source)
		g_source_remove(f->rescan_source);
	if (f->mon) {
		g_signal_handlers_disconnect_by_data(f->mon, f);
		g_file_monitor_cancel(f->mon);
		g_object_unref(f->mon);
	}
	g_ptr_array_unref(f->items);
	g_free(f->dir);
	g_free(f->keep);
	g_free(f);
}

void iv_folder_set_changed(IvFolder *f, IvFolderChanged cb, gpointer data)
{
	f->changed = cb;
	f->changed_data = data;
}

int iv_folder_count(IvFolder *f)
{
	return f ? (int)f->items->len : 0;
}

const char *iv_folder_path(IvFolder *f, int i)
{
	if (!f || i < 0 || i >= (int)f->items->len)
		return NULL;
	return ((Item *)g_ptr_array_index(f->items, i))->path;
}

int iv_folder_index(IvFolder *f, const char *path)
{
	if (!f || !path)
		return -1;
	for (guint i = 0; i < f->items->len; i++)
		if (!strcmp(((Item *)g_ptr_array_index(f->items, i))->path, path))
			return (int)i;
	return -1;
}

const char *iv_folder_dir(IvFolder *f)
{
	return f ? f->dir : NULL;
}

void iv_folder_set_sort(IvFolder *f, IvSort sort)
{
	f->sort = sort;
	sort_items(f);
}

void iv_folder_remove(IvFolder *f, const char *path)
{
	int i = iv_folder_index(f, path);

	if (i >= 0)
		g_ptr_array_remove_index(f->items, (guint)i);
}

void iv_folder_add(IvFolder *f, const char *path)
{
	if (iv_folder_index(f, path) >= 0)
		return;
	g_ptr_array_add(f->items, item_new(path));
	if (!f->fixed || f->sort != IV_SORT_NAME)
		sort_items(f);
}

void iv_folder_renamed(IvFolder *f, const char *from, const char *to)
{
	int i = iv_folder_index(f, from);

	if (g_strcmp0(f->keep, from) == 0) {
		g_free(f->keep);
		f->keep = g_strdup(to);
	}
	if (i < 0) {
		iv_folder_add(f, to);
		return;
	}
	Item *it = item_new(to);

	item_free(g_ptr_array_index(f->items, i));
	g_ptr_array_index(f->items, i) = it;
	if (!f->fixed || f->sort != IV_SORT_NAME)
		sort_items(f);
}
