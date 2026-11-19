// LikeOS Explorer: the item views.
//
// Six layouts over one ExFolderModel: Details (columns with headers), List
// and Compact (rows), Icons, Large icons (thumbnails) and Tiles.  The first
// three are a GtkTreeView, the others a GtkIconView -- GTK's own views, with
// their keyboard handling, rubber-band selection and accessibility -- and
// switching keeps the selection, because the selection lives in the tab's
// ExSelection rather than in either view.
//
// What a view cannot decide itself it hands up as signals:
//
//   "activate"  (GPtrArray *files)    open these (ExFile)
//   "popup"     (GdkEventButton *ev, gboolean on_item)   context menu
//   "rename"    (ExFile *file, const char *new_name)
//   "drop"      (GPtrArray *uris, GFile *dest_dir, guint action)
//                                     a drag ended here; dest_dir NULL
//                                     means the folder the tab shows
//   "sort"      (int column, gboolean ascending)  a column header
//   "zoom"      (int direction)       Ctrl + wheel
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_FILE_VIEW_H
#define EX_FILE_VIEW_H

#include <gtk/gtk.h>
#include "ui/ex-folder-model.h"
#include "services/ex-selection.h"

G_BEGIN_DECLS

typedef enum {
	EX_COLUMNS_FOLDER, /* Name, Date modified, Type, Size */
	EX_COLUMNS_TRASH, /* Name, Original location, Date deleted, Size */
	EX_COLUMNS_SEARCH, /* Name, Folder, Date modified, Size */
} ExColumnSet;

#define EX_TYPE_FILE_VIEW (ex_file_view_get_type())
G_DECLARE_FINAL_TYPE(ExFileView, ex_file_view, EX, FILE_VIEW, GtkStack)

GtkWidget *ex_file_view_new(ExSelection *selection);

void ex_file_view_set_model(ExFileView *v, ExFolderModel *m);
/* "details", "list", "compact", "icons", "large-icons", "tiles". */
void ex_file_view_set_mode(ExFileView *v, const char *mode);
const char *ex_file_view_get_mode(ExFileView *v);
void ex_file_view_set_columns(ExFileView *v, ExColumnSet cols);
void ex_file_view_set_show_extensions(ExFileView *v, gboolean show);
void ex_file_view_set_single_click(ExFileView *v, gboolean single);

/* Select the items for these GFiles (those not shown yet are selected when
 * they appear, for a while: a paste or a new folder selects its result). */
void ex_file_view_select_files(ExFileView *v, GPtrArray *files);
void ex_file_view_select_all(ExFileView *v);
void ex_file_view_select_none(ExFileView *v);
void ex_file_view_invert_selection(ExFileView *v);
/* Start editing `f''s name in place. */
void ex_file_view_start_rename(ExFileView *v, ExFile *f);
/* The folder shown: where a drop on no item goes, and its name for the
 * drag's "Move to …" (NULL where nothing can be dropped); `always' the
 * action a drop there always is (the Trash moves), or 0. */
void ex_file_view_set_folder(ExFileView *v, GFile *folder, const char *name,
			     GdkDragAction always);
/* A drag from one of the views is over a place that takes it (`dest',
 * called `place'; NULL: over nothing that does).  Shows on the drag what
 * dropping there does and returns that action: `forced' if not 0 (keys
 * held, or a place that always moves, as the Trash), else move within a
 * filesystem and copy across -- 0 where it would do nothing (onto itself,
 * or moved to where it already is).  For any other drag, returns `forced'. */
GdkDragAction ex_file_view_drag_hint(GdkDragContext *ctx, GFile *dest,
				     const char *place, GdkDragAction forced);
/* Whether `ctx' is a drag from one of the views. */
gboolean ex_file_view_dragging(GdkDragContext *ctx);
/* The selected items are about to be deleted from here: when they have
 * gone, the item that took their place is selected. */
void ex_file_view_note_removal(ExFileView *v);
/* The next item matching `f' to appear is renamed in place (New folder). */
void ex_file_view_rename_when_shown(ExFileView *v, GFile *f);
void ex_file_view_focus(ExFileView *v);

G_END_DECLS

#endif
