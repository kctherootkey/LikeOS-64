// LikeOS Explorer: search.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-search.h"
#include "backend/ex-file.h"
#include "backend/ex-index.h"
#include "backend/ex-util.h"

#include <string.h>

/* At most this many hits are kept: a query matching half the disk is not
 * a search any more. */
#define EX_SEARCH_MAX 5000
/* A batch goes to the main loop at this many hits or this often. */
#define EX_SEARCH_BATCH 64
#define EX_SEARCH_BATCH_US (150 * 1000)

struct _ExQuery {
	GPtrArray *words; /* casefolded */
	GPatternSpec *glob; /* on the casefolded name */
	char *kind;
	char *ext; /* casefolded, without the dot */
};

ExQuery *ex_query_parse(const char *text)
{
	ExQuery *q = g_new0(ExQuery, 1);
	char **tok = g_strsplit_set(text ? text : "", " \t", -1);

	q->words = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; tok[i]; i++) {
		char *t = tok[i];

		if (!*t)
			continue;
		if (g_ascii_strncasecmp(t, "kind:", 5) == 0 && t[5]) {
			g_free(q->kind);
			q->kind = g_ascii_strdown(t + 5, -1);
		} else if (g_ascii_strncasecmp(t, "ext:", 4) == 0 && t[4]) {
			g_free(q->ext);
			q->ext = g_utf8_casefold(t[4] == '.' ? t + 5 : t + 4, -1);
		} else if (strpbrk(t, "*?")) {
			char *f = g_utf8_casefold(t, -1);

			if (q->glob)
				g_pattern_spec_free(q->glob);
			q->glob = g_pattern_spec_new(f);
			g_free(f);
		} else {
			g_ptr_array_add(q->words, g_utf8_casefold(t, -1));
		}
	}
	g_strfreev(tok);
	return q;
}

void ex_query_free(ExQuery *q)
{
	if (!q)
		return;
	g_ptr_array_unref(q->words);
	if (q->glob)
		g_pattern_spec_free(q->glob);
	g_free(q->kind);
	g_free(q->ext);
	g_free(q);
}

gboolean ex_query_is_empty(const ExQuery *q)
{
	return q->words->len == 0 && !q->glob && !q->kind && !q->ext;
}

const char *ex_query_kind_of_name(const char *name, gboolean is_dir)
{
	char *ct;
	const char *k = "other";

	if (is_dir)
		return "folder";
	ct = g_content_type_guess(name, NULL, 0, NULL);
	if (g_str_has_prefix(ct, "image/"))
		k = "image";
	else if (g_str_has_prefix(ct, "video/"))
		k = "video";
	else if (g_str_has_prefix(ct, "audio/"))
		k = "audio";
	else if (g_str_has_prefix(ct, "text/") || strstr(ct, "pdf") ||
		 strstr(ct, "document") || strstr(ct, "spreadsheet") ||
		 strstr(ct, "presentation") || strstr(ct, "msword"))
		k = "document";
	else if (strstr(ct, "zip") || strstr(ct, "tar") ||
		 strstr(ct, "compressed") || strstr(ct, "archive") ||
		 strstr(ct, "x-7z") || strstr(ct, "rar"))
		k = "archive";
	else if (strstr(ct, "executable"))
		k = "program";
	g_free(ct);
	return k;
}

gboolean ex_query_matches(const ExQuery *q, const char *name,
			  const char *fold, gboolean is_dir, const char *kind)
{
	for (guint i = 0; i < q->words->len; i++)
		if (!strstr(fold, g_ptr_array_index(q->words, i)))
			return FALSE;
	if (q->glob && !g_pattern_spec_match_string(q->glob, fold))
		return FALSE;
	if (q->ext) {
		const char *dot = strrchr(fold, '.');

		if (is_dir || !dot || strcmp(dot + 1, q->ext) != 0)
			return FALSE;
	}
	if (q->kind) {
		if (!kind)
			kind = ex_query_kind_of_name(name, is_dir);
		if (strcmp(kind, q->kind) != 0)
			return FALSE;
	}
	return TRUE;
}

/* ---- ExSearch ------------------------------------------------------------ */

struct _ExSearch {
	GObject parent;
	GFile *scope;
	char *text;
	gboolean show_hidden;
	GCancellable *cancel;
	gboolean running;
	GMainContext *ctx;
};

G_DEFINE_TYPE(ExSearch, ex_search, G_TYPE_OBJECT)

enum { SIG_HITS, SIG_FINISHED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_search_finalize(GObject *o)
{
	ExSearch *s = EX_SEARCH(o);

	g_cancellable_cancel(s->cancel);
	g_clear_object(&s->cancel);
	g_clear_object(&s->scope);
	g_free(s->text);
	g_main_context_unref(s->ctx);
	G_OBJECT_CLASS(ex_search_parent_class)->finalize(o);
}

static void ex_search_class_init(ExSearchClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = ex_search_finalize;
	sigs[SIG_HITS] = g_signal_new("hits", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 1,
				      G_TYPE_PTR_ARRAY);
	sigs[SIG_FINISHED] = g_signal_new("finished", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 1,
					  G_TYPE_BOOLEAN);
}

static void ex_search_init(ExSearch *s)
{
	s->cancel = g_cancellable_new();
	s->ctx = g_main_context_ref_thread_default();
}

ExSearch *ex_search_new(GFile *scope, const char *query, gboolean show_hidden)
{
	ExSearch *s = g_object_new(EX_TYPE_SEARCH, NULL);

	s->scope = g_object_ref(scope);
	s->text = g_strdup(query);
	s->show_hidden = show_hidden;
	return s;
}

typedef struct {
	ExSearch *s;
	GPtrArray *hits;
} HitBatch;

static gboolean deliver_hits(gpointer data)
{
	HitBatch *b = data;

	if (!g_cancellable_is_cancelled(b->s->cancel))
		g_signal_emit(b->s, sigs[SIG_HITS], 0, b->hits);
	return G_SOURCE_REMOVE;
}

static void hit_batch_free(gpointer data)
{
	HitBatch *b = data;

	g_ptr_array_unref(b->hits);
	g_object_unref(b->s);
	g_free(b);
}

typedef struct {
	ExSearch *s;
	ExQuery *q;
	GPtrArray *pending;
	gint64 last_flush;
	guint total;
	gboolean from_index;
} Walk;

static void flush_hits(Walk *w, gboolean force)
{
	gint64 now = g_get_monotonic_time();

	if (w->pending->len == 0)
		return;
	if (!force && w->pending->len < EX_SEARCH_BATCH &&
	    now - w->last_flush < EX_SEARCH_BATCH_US)
		return;
	HitBatch *b = g_new0(HitBatch, 1);

	b->s = g_object_ref(w->s);
	b->hits = w->pending;
	w->pending = g_ptr_array_new_with_free_func(g_object_unref);
	w->last_flush = now;
	ex_run_on(w->s->ctx, deliver_hits, b, hit_batch_free);
}

static void walk_dir(Walk *w, GFile *dir, int depth)
{
	GCancellable *c = w->s->cancel;
	GFileEnumerator *en;
	GFileInfo *fi;
	GPtrArray *subdirs;

	if (depth > 64 || g_cancellable_is_cancelled(c) ||
	    w->total >= EX_SEARCH_MAX)
		return;
	en = g_file_enumerate_children(dir, EX_FILE_ATTRS,
				       G_FILE_QUERY_INFO_NONE, c, NULL);
	if (!en)
		return;
	subdirs = g_ptr_array_new_with_free_func(g_object_unref);
	while (w->total < EX_SEARCH_MAX &&
	       (fi = g_file_enumerator_next_file(en, c, NULL))) {
		const char *name = g_file_info_get_name(fi);
		gboolean hidden = ex_name_is_hidden(name) ||
				  g_file_info_get_is_hidden(fi);
		gboolean is_dir = g_file_info_get_file_type(fi) ==
				  G_FILE_TYPE_DIRECTORY;

		if (hidden && !w->s->show_hidden) {
			g_object_unref(fi);
			continue;
		}
		GFile *child = g_file_enumerator_get_child(en, fi);
		const char *disp = g_file_info_get_display_name(fi);
		char *fold = g_utf8_casefold(disp ? disp : name, -1);

		if (ex_query_matches(w->q, disp ? disp : name, fold, is_dir,
				     NULL)) {
			g_ptr_array_add(w->pending, ex_file_new(child, fi));
			w->total++;
			flush_hits(w, FALSE);
		}
		g_free(fold);
		/* Links are not descended into: a link to a parent would
		 * never end. */
		if (is_dir && !g_file_info_get_is_symlink(fi))
			g_ptr_array_add(subdirs, g_object_ref(child));
		g_object_unref(child);
		g_object_unref(fi);
	}
	g_object_unref(en);
	for (guint i = 0; i < subdirs->len && !g_cancellable_is_cancelled(c);
	     i++)
		walk_dir(w, g_ptr_array_index(subdirs, i), depth + 1);
	g_ptr_array_unref(subdirs);
}

static void search_thread(GTask *task, gpointer src, gpointer data,
			  GCancellable *cancel)
{
	ExSearch *s = src;
	Walk w = { 0 };
	ExIndex *ix = ex_index_get_default();

	w.s = s;
	w.q = ex_query_parse(s->text);
	w.pending = g_ptr_array_new_with_free_func(g_object_unref);
	w.last_flush = g_get_monotonic_time();
	if (!ex_query_is_empty(w.q)) {
		if (ex_index_covers(ix, s->scope, s->show_hidden)) {
			GPtrArray *paths = ex_index_query(ix, s->scope, w.q,
							  EX_SEARCH_MAX);

			w.from_index = TRUE;
			for (guint i = 0; i < paths->len &&
					  !g_cancellable_is_cancelled(s->cancel);
			     i++) {
				GFile *f = g_file_new_for_path(
					g_ptr_array_index(paths, i));
				/* The index may be a little behind: what is
				 * gone is not reported. */
				ExFile *ef = ex_file_new_query(f, s->cancel, NULL);

				if (ef) {
					g_ptr_array_add(w.pending, ef);
					flush_hits(&w, FALSE);
				}
				g_object_unref(f);
			}
			g_ptr_array_unref(paths);
		} else {
			walk_dir(&w, s->scope, 0);
		}
	}
	flush_hits(&w, TRUE);
	g_ptr_array_unref(w.pending);
	ex_query_free(w.q);
	g_task_return_boolean(task, w.from_index);
}

static void search_done(GObject *src, GAsyncResult *res, gpointer data)
{
	ExSearch *s = EX_SEARCH(src);
	gboolean from_index = g_task_propagate_boolean(G_TASK(res), NULL);

	s->running = FALSE;
	if (!g_cancellable_is_cancelled(s->cancel))
		g_signal_emit(s, sigs[SIG_FINISHED], 0, from_index);
}

void ex_search_start(ExSearch *s)
{
	GTask *task;

	g_return_if_fail(!s->running);
	s->running = TRUE;
	ex_index_ensure(ex_index_get_default());
	task = g_task_new(s, NULL, search_done, NULL);
	g_task_run_in_thread(task, search_thread);
	g_object_unref(task);
}

void ex_search_cancel(ExSearch *s)
{
	g_cancellable_cancel(s->cancel);
}

gboolean ex_search_is_running(ExSearch *s)
{
	return s->running;
}
