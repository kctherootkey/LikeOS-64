// LikeOS -- File Explorer: undo and redo.
//
// Each finished operation queued as undoable leaves a record of what it did
// (its results).  Undo runs the inverse as an ordinary operation:
//
//     copy, link, new folder/file   -> the created items go to the Trash
//     move, rename                  -> moved back to the exact old names
//     to the Trash                  -> restored from the Trash
//     restore                       -> back to the Trash
//
// and that inverse's own results become the record Redo inverts again -- so
// redoing a copy restores the copies from the Trash instead of copying
// anew.  Deleting for good, emptying the Trash and permission changes cannot
// be undone and leave no record.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_UNDO_H
#define EX_UNDO_H

#include "backend/ex-fileops.h"

G_BEGIN_DECLS

#define EX_TYPE_UNDO (ex_undo_get_type())
G_DECLARE_FINAL_TYPE(ExUndo, ex_undo, EX, UNDO, GObject)

/* "changed" () */
ExUndo *ex_undo_get(void);

gboolean ex_undo_can_undo(ExUndo *u);
gboolean ex_undo_can_redo(ExUndo *u);
/* "Undo Copy", "Redo Rename" -- for menus and tooltips. */
char *ex_undo_undo_label(ExUndo *u);
char *ex_undo_redo_label(ExUndo *u);
/* Queue the inverse of the newest record.  The operation (or NULL). */
ExOperation *ex_undo_undo(ExUndo *u);
ExOperation *ex_undo_redo(ExUndo *u);

G_END_DECLS

#endif
