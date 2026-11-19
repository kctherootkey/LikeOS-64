// LikeOS Explorer: the search index of the home folder.
//
// The stored form is text, one entry per line after a header:
//
//     likeos-explorer-index 1	<home>	<built, seconds since the epoch>
//     d|f	<mtime>	<size>	<path relative to home>
//
// Paths contain no tab or newline in practice; a name that does is left
// out rather than escaped.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-index.h"
#include "backend/ex-util.h"

#include <glib/gstdio.h>
#include <string.h>

/* A stored index older than this is rebuilt when a search needs it. */
#define EX_INDEX_MAX_AGE (10 * 60)
/* After changes were seen, rebuild at most this often. */
#define EX_INDEX_DIRTY_DELAY (2 * 60)
#define EX_INDEX_MAX_ENTRIES 500000

typedef struct {
	char *rel; /* relative to home */
	char *fold; /* casefolded last component */
	const char *name; /* last component inside rel */
	gboolean is_dir;
} Entry;

struct _ExIndex {
	GMutex lock;
	GPtrArray *entries; /* Entry, NULL until loaded or built */
	gint64 built;
	gboolean enabled;
	gboolean building;
	gboolean loaded_once;
	gboolean dirty;
	gint64 dirty_since;
	char *home;
	char *store;
};

static void entry_free(Entry *e)
{
	g_free(e->rel);
	g_free(e->fold);
	g_free(e);
}

static Entry *entry_new(const char *rel, gboolean is_dir)
{
	Entry *e = g_new0(Entry, 1);
	const char *slash;

	e->rel = g_strdup(rel);
	slash = strrchr(e->rel, '/');
	e->name = slash ? slash + 1 : e->rel;
	e->fold = g_utf8_casefold(e->name, -1);
	e->is_dir = is_dir;
	return e;
}

ExIndex *ex_index_get_default(void)
{
	static ExIndex *the;
	static gsize once;

	if (g_once_init_enter(&once)) {
		the = g_new0(ExIndex, 1);
		g_mutex_init(&the->lock);
		the->enabled = TRUE;
		the->home = g_strdup(g_get_home_dir());
		the->store = g_build_filename(g_get_user_cache_dir(),
					      "likeos-explorer", "index-v1",
					      NULL);
		g_once_init_leave(&once, 1);
	}
	return the;
}

void ex_index_set_enabled(ExIndex *ix, gboolean enabled)
{
	g_mutex_lock(&ix->lock);
	ix->enabled = enabled;
	if (!enabled) {
		g_clear_pointer(&ix->entries, g_ptr_array_unref);
		g_unlink(ix->store);
	}
	g_mutex_unlock(&ix->lock);
}

gboolean ex_index_get_enabled(ExIndex *ix)
{
	return ix->enabled;
}

/* Whether `path' is the home folder or below it, and not inside a hidden
 * folder or the cache (which the index leaves out). */
static gboolean indexable_scope(ExIndex *ix, const char *path)
{
	size_t hl = strlen(ix->home);

	if (strncmp(path, ix->home, hl) != 0 ||
	    (path[hl] != '\0' && path[hl] != '/'))
		return FALSE;
	for (const char *s = path + hl; (s = strchr(s, '/')); s++)
		if (s[1] == '.')
			return FALSE;
	return TRUE;
}

gboolean ex_index_covers(ExIndex *ix, GFile *scope, gboolean show_hidden)
{
	char *p = g_file_get_path(scope);
	gboolean ok;

	if (!p)
		return FALSE;
	g_mutex_lock(&ix->lock);
	ok = ix->enabled && ix->entries && !show_hidden &&
	     indexable_scope(ix, p);
	g_mutex_unlock(&ix->lock);
	g_free(p);
	return ok;
}

GPtrArray *ex_index_query(ExIndex *ix, GFile *scope, const ExQuery *q,
			  guint max)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	char *sp = g_file_get_path(scope);
	size_t hl = strlen(ix->home);
	const char *rel_scope = "";
	size_t rl;

	if (!sp)
		return out;
	if (strlen(sp) > hl)
		rel_scope = sp + hl + 1;
	rl = strlen(rel_scope);
	g_mutex_lock(&ix->lock);
	for (guint i = 0; ix->entries && i < ix->entries->len && out->len < max;
	     i++) {
		Entry *e = g_ptr_array_index(ix->entries, i);

		if (rl && (strncmp(e->rel, rel_scope, rl) != 0 ||
			   e->rel[rl] != '/'))
			continue;
		if (ex_query_matches(q, e->name, e->fold, e->is_dir, NULL))
			g_ptr_array_add(out, g_build_filename(ix->home, e->rel,
							      NULL));
	}
	g_mutex_unlock(&ix->lock);
	g_free(sp);
	return out;
}

/* ---- building ------------------------------------------------------------ */

static void walk(ExIndex *ix, GFile *dir, const char *rel, GPtrArray *out,
		 GString *text, int depth, const char *cache)
{
	GFileEnumerator *en;
	GFileInfo *fi;

	if (depth > 64 || out->len >= EX_INDEX_MAX_ENTRIES)
		return;
	en = g_file_enumerate_children(
		dir,
		G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
		"," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED
		"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	if (!en)
		return;
	while (out->len < EX_INDEX_MAX_ENTRIES &&
	       (fi = g_file_enumerator_next_file(en, NULL, NULL))) {
		const char *name = g_file_info_get_name(fi);
		gboolean is_dir = g_file_info_get_file_type(fi) ==
				  G_FILE_TYPE_DIRECTORY;
		char *crel;

		if (ex_name_is_hidden(name) || strpbrk(name, "\t\n")) {
			g_object_unref(fi);
			continue;
		}
		crel = *rel ? g_strconcat(rel, "/", name, NULL) : g_strdup(name);
		if (is_dir && cache && strcmp(crel, cache) == 0) {
			g_free(crel);
			g_object_unref(fi);
			continue;
		}
		g_ptr_array_add(out, entry_new(crel, is_dir));
		g_string_append_printf(
			text, "%c\t%" G_GUINT64_FORMAT "\t%" G_GOFFSET_FORMAT "\t%s\n",
			is_dir ? 'd' : 'f',
			g_file_info_get_attribute_uint64(
				fi, G_FILE_ATTRIBUTE_TIME_MODIFIED),
			g_file_info_get_size(fi), crel);
		if (is_dir) {
			GFile *child = g_file_enumerator_get_child(en, fi);

			walk(ix, child, crel, out, text, depth + 1, cache);
			g_object_unref(child);
		}
		g_free(crel);
		g_object_unref(fi);
	}
	g_object_unref(en);
}

static void build(ExIndex *ix)
{
	GFile *home = g_file_new_for_path(ix->home);
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
	GString *text = g_string_new(NULL);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	const char *cachedir = g_get_user_cache_dir();
	const char *cache_rel = NULL;
	size_t hl = strlen(ix->home);

	/* The cache folder is usually hidden (~/.cache) and skipped as such;
	 * one configured elsewhere below home is skipped by name. */
	if (strncmp(cachedir, ix->home, hl) == 0 && cachedir[hl] == '/')
		cache_rel = cachedir + hl + 1;
	g_string_append_printf(text, "likeos-explorer-index 1\t%s\t%" G_GINT64_FORMAT
				     "\n",
			       ix->home, now);
	walk(ix, home, "", out, text, 0, cache_rel);

	char *dir = g_path_get_dirname(ix->store);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);

	g_mutex_lock(&ix->lock);
	if (ix->enabled) {
		(void)g_file_set_contents_full(ix->store, text->str,
					       (gssize)text->len,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, NULL);
		g_clear_pointer(&ix->entries, g_ptr_array_unref);
		ix->entries = out;
		ix->built = now;
		out = NULL;
	}
	ix->building = FALSE;
	ix->dirty = FALSE;
	g_mutex_unlock(&ix->lock);
	if (out)
		g_ptr_array_unref(out);
	g_string_free(text, TRUE);
	g_object_unref(home);
}

static gpointer build_thread(gpointer data)
{
	build(data);
	return NULL;
}

static void load_stored(ExIndex *ix)
{
	char *data = NULL;
	gsize len = 0;

	if (!g_file_get_contents(ix->store, &data, &len, NULL))
		return;
	char *nl = strchr(data, '\n');
	if (!nl || !g_str_has_prefix(data, "likeos-explorer-index 1\t")) {
		g_free(data);
		return;
	}
	*nl = '\0';
	char **hdr = g_strsplit(data, "\t", 3);
	gboolean same_home = g_strv_length(hdr) == 3 &&
			     strcmp(hdr[1], ix->home) == 0;
	gint64 built = same_home ? g_ascii_strtoll(hdr[2], NULL, 10) : 0;

	g_strfreev(hdr);
	if (!same_home) {
		g_free(data);
		return;
	}
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
	char *line = nl + 1;

	while (*line) {
		char *end = strchr(line, '\n');
		char **f;

		if (end)
			*end = '\0';
		f = g_strsplit(line, "\t", 4);
		if (g_strv_length(f) == 4 && *f[3])
			g_ptr_array_add(out, entry_new(f[3], f[0][0] == 'd'));
		g_strfreev(f);
		if (!end)
			break;
		line = end + 1;
	}
	g_free(data);
	g_mutex_lock(&ix->lock);
	if (!ix->entries && ix->enabled) {
		ix->entries = out;
		ix->built = built;
		out = NULL;
	}
	g_mutex_unlock(&ix->lock);
	if (out)
		g_ptr_array_unref(out);
}

/* Under ix->lock: whether the index wants rebuilding now. */
static gboolean stale_locked(ExIndex *ix)
{
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	return ix->enabled &&
	       (!ix->entries || now - ix->built > EX_INDEX_MAX_AGE ||
		(ix->dirty && now - ix->dirty_since > EX_INDEX_DIRTY_DELAY));
}

/* The first look: read the stored index, then rebuild it if it is stale --
 * both off the main loop, as the stored index can be megabytes. */
static gpointer load_thread(gpointer data)
{
	ExIndex *ix = data;
	gboolean rebuild;

	load_stored(ix);
	g_mutex_lock(&ix->lock);
	rebuild = stale_locked(ix);
	if (!rebuild)
		ix->building = FALSE;
	g_mutex_unlock(&ix->lock);
	if (rebuild)
		build(ix);
	return NULL;
}

void ex_index_ensure(ExIndex *ix)
{
	GThreadFunc fn = NULL;

	g_mutex_lock(&ix->lock);
	if (ix->enabled && !ix->building) {
		if (!ix->loaded_once) {
			ix->loaded_once = TRUE;
			fn = load_thread;
		} else if (stale_locked(ix)) {
			fn = build_thread;
		}
		if (fn)
			ix->building = TRUE;
	}
	g_mutex_unlock(&ix->lock);
	if (fn)
		g_thread_unref(g_thread_new("explorer-index", fn, ix));
}

void ex_index_mark_dirty(ExIndex *ix)
{
	g_mutex_lock(&ix->lock);
	if (!ix->dirty) {
		ix->dirty = TRUE;
		ix->dirty_since = g_get_real_time() / G_USEC_PER_SEC;
	}
	g_mutex_unlock(&ix->lock);
}

void ex_index_rebuild_sync(ExIndex *ix)
{
	g_mutex_lock(&ix->lock);
	ix->building = TRUE;
	g_mutex_unlock(&ix->lock);
	build(ix);
}

guint ex_index_size(ExIndex *ix)
{
	guint n;

	g_mutex_lock(&ix->lock);
	n = ix->entries ? ix->entries->len : 0;
	g_mutex_unlock(&ix->lock);
	return n;
}
