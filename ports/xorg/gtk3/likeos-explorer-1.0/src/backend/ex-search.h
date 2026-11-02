// LikeOS -- File Explorer: search.
//
// A query is words that must all appear in a name (case does not matter),
// or a pattern with * and ?, plus filters:
//
//     kind:image   kind:video  kind:audio  kind:document  kind:folder
//     kind:archive kind:program
//     ext:png      (the extension, without the dot)
//
// so "holiday kind:image" finds the pictures with "holiday" in their names.
//
// Two engines answer it.  Below the home folder the index (ex-index.h) does,
// at once; anywhere else -- or while the index is not built yet -- a walk of
// the folder tree in a worker thread, whose results arrive in batches as
// they are found.  The caller sees the same signals either way.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_SEARCH_H
#define EX_SEARCH_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _ExQuery ExQuery;

ExQuery *ex_query_parse(const char *text);
void ex_query_free(ExQuery *q);
/* Whether the query asks for anything at all. */
gboolean ex_query_is_empty(const ExQuery *q);
/* Whether a name passes.  `fold' is g_utf8_casefold() of the name; `kind'
 * one of ex_file_get_kind()'s answers, or NULL to have it guessed from the
 * name when the query filters on it. */
gboolean ex_query_matches(const ExQuery *q, const char *name,
			  const char *fold, gboolean is_dir, const char *kind);
/* The kind of a name, from its extension alone. */
const char *ex_query_kind_of_name(const char *name, gboolean is_dir);

#define EX_TYPE_SEARCH (ex_search_get_type())
G_DECLARE_FINAL_TYPE(ExSearch, ex_search, EX, SEARCH, GObject)

/* Signals:
 *   "hits"     (GPtrArray *files)  -- ExFile items found since the last
 *   "finished" (gboolean from_index)
 */
ExSearch *ex_search_new(GFile *scope, const char *query,
			gboolean show_hidden);
void ex_search_start(ExSearch *s);
void ex_search_cancel(ExSearch *s);
gboolean ex_search_is_running(ExSearch *s);

G_END_DECLS

#endif
