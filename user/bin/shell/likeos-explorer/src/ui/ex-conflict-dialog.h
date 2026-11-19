// LikeOS Explorer: questions an operation asks.
//
// LikeOS message boxes: an icon badge beside the question in a larger
// type, what the operation is doing under it, the details, and a footer
// band holding "apply to all" at the left and the answers at the right,
// the default one in the accent.
//
// A name taken at the destination: the item coming in and the one already
// there side by side, each with its size and date, and Replace, Skip or
// Keep both -- for two folders Merge or Skip.  A failure: what went wrong,
// with Try again, Skip, Cancel and, when the Trash cannot take an item
// there, Delete permanently.  Both answer the operation through
// ex_conflict_resolve() / ex_issue_resolve() and do not block the window.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_CONFLICT_DIALOG_H
#define EX_CONFLICT_DIALOG_H

#include <gtk/gtk.h>
#include "backend/ex-fileops.h"

G_BEGIN_DECLS

void ex_conflict_dialog_show(GtkWindow *parent, ExOperation *op,
			     const ExConflict *c);
void ex_issue_dialog_show(GtkWindow *parent, ExOperation *op,
			  const ExIssue *i);

G_END_DECLS

#endif
