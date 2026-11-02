// LikeOS -- File Explorer: what is selected.
//
// Every view of a tab reports into one selection, so the command bar, the
// context menu, the status bar and the panes all describe the same items,
// and switching from Details to Icons keeps them selected.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_SELECTION_H
#define EX_SELECTION_H

#include "backend/ex-file.h"

G_BEGIN_DECLS

#define EX_TYPE_SELECTION (ex_selection_get_type())
G_DECLARE_FINAL_TYPE(ExSelection, ex_selection, EX, SELECTION, GObject)

/* "changed" () */
ExSelection *ex_selection_new(void);

/* `files' of ExFile (the selection keeps its own references). */
void ex_selection_set(ExSelection *s, GPtrArray *files);
void ex_selection_clear(ExSelection *s);
GPtrArray *ex_selection_get(ExSelection *s); /* not a new reference */
guint ex_selection_count(ExSelection *s);
/* The single selected item, or NULL. */
ExFile *ex_selection_single(ExSelection *s);
/* What every selected item allows (0 for an empty selection). */
ExFileCaps ex_selection_caps(ExSelection *s);
gboolean ex_selection_all_dirs(ExSelection *s);
guint64 ex_selection_total_size(ExSelection *s);
/* GFile of every item, new array. */
GPtrArray *ex_selection_files(ExSelection *s);

G_END_DECLS

#endif
