// LikeOS Explorer: the items of one folder, as GTK sees them.
//
// One list model behind every view of a tab -- the Details, List and
// Compact tree views and the Icons, Large icons and Tiles icon views -- so a
// view switch shows the same rows in the same order.  It holds one column,
// the ExFile; views draw from it with cell data functions rather than
// copying names, sizes and dates into columns of their own, and only the
// rows on screen are ever drawn: a folder of fifty thousand items is fifty
// thousand references, not fifty thousand widgets.
//
// The model keeps every item it was given, hidden ones included, and shows
// those the filter lets through, sorted by the chosen column with folders
// first.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_FOLDER_MODEL_H
#define EX_FOLDER_MODEL_H

#include <gtk/gtk.h>
#include "backend/ex-file.h"

G_BEGIN_DECLS

enum { EX_COL_FILE, EX_N_COLS };

typedef enum {
	EX_SORT_NAME,
	EX_SORT_MODIFIED,
	EX_SORT_TYPE,
	EX_SORT_SIZE,
	EX_SORT_ORIGIN, /* Trash: where it came from */
	EX_SORT_DELETED, /* Trash: when it went */
} ExSortColumn;

#define EX_TYPE_FOLDER_MODEL (ex_folder_model_get_type())
G_DECLARE_FINAL_TYPE(ExFolderModel, ex_folder_model, EX, FOLDER_MODEL,
		     GObject)

/* Signals:
 *   "count-changed" ()                  -- the number of shown items changed
 *   "file-replaced" (ExFile *old, ExFile *new) -- an item was renamed in
 *       place: same row, new item (see ex_folder_model_replace())
 */
ExFolderModel *ex_folder_model_new(void);

void ex_folder_model_clear(ExFolderModel *m);
/* Add items (ExFile).  One already present -- same GFile -- is updated
 * instead. */
void ex_folder_model_add(ExFolderModel *m, GPtrArray *files);
void ex_folder_model_add_one(ExFolderModel *m, ExFile *f);
void ex_folder_model_remove(ExFolderModel *m, GFile *file);
/* The item known as `from' is now `to' (renamed): the row stays the same
 * row -- it moves to where the order wants it, keeping its selection --
 * instead of going and coming back. */
void ex_folder_model_replace(ExFolderModel *m, GFile *from, ExFile *to);
/* The item for `file', shown or not (not a new reference). */
ExFile *ex_folder_model_lookup(ExFolderModel *m, GFile *file);
gboolean ex_folder_model_get_iter_for(ExFolderModel *m, GFile *file,
				      GtkTreeIter *iter);
ExFile *ex_folder_model_get_file(ExFolderModel *m, GtkTreeIter *iter);

void ex_folder_model_set_sort(ExFolderModel *m, ExSortColumn col,
			      gboolean ascending);
void ex_folder_model_get_sort(ExFolderModel *m, ExSortColumn *col,
			      gboolean *ascending);
void ex_folder_model_set_show_hidden(ExFolderModel *m, gboolean show);

/* Items shown / all items. */
guint ex_folder_model_count(ExFolderModel *m);
guint ex_folder_model_count_all(ExFolderModel *m);
/* Every shown item, in order (new array of new references). */
GPtrArray *ex_folder_model_list(ExFolderModel *m);

G_END_DECLS

#endif
