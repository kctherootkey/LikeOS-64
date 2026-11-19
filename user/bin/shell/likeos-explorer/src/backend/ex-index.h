// LikeOS Explorer: the search index of the home folder.
//
// Every name below the home folder -- hidden folders and caches excepted --
// with its kind, size and modification time, in one file
// (~/.cache/likeos-explorer/index-v1) and in memory once loaded.  It is
// rebuilt in the background when it is older than a few minutes and a search
// needs it, or when the folders Explorer is showing report changes; a search
// is answered from the copy in memory meanwhile.  The index can be switched
// off (privacy): it is then deleted and every search walks.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_INDEX_H
#define EX_INDEX_H

#include "backend/ex-search.h"

G_BEGIN_DECLS

typedef struct _ExIndex ExIndex;

ExIndex *ex_index_get_default(void);

/* Switch indexing on or off; off deletes the stored index. */
void ex_index_set_enabled(ExIndex *ix, gboolean enabled);
gboolean ex_index_get_enabled(ExIndex *ix);

/* Whether the index can answer a search below `scope' now. */
gboolean ex_index_covers(ExIndex *ix, GFile *scope, gboolean show_hidden);

/* Paths (absolute, newly allocated) matching `q' below `scope', at most
 * `max'.  Thread-safe. */
GPtrArray *ex_index_query(ExIndex *ix, GFile *scope, const ExQuery *q,
			  guint max);

/* Something changed below the home folder: rebuild soon. */
void ex_index_mark_dirty(ExIndex *ix);
/* Load the stored index and rebuild it if it is stale (background). */
void ex_index_ensure(ExIndex *ix);
/* Rebuild now, waiting for it (tests). */
void ex_index_rebuild_sync(ExIndex *ix);
guint ex_index_size(ExIndex *ix);

G_END_DECLS

#endif
