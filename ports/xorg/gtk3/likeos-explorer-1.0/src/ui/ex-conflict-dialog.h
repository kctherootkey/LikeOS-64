// LikeOS -- File Explorer: questions an operation asks.
//
// "Replace or skip files": the destination already has an item of that
// name -- replace it, skip this one, keep both, or for two folders merge
// them, with the two items' sizes and dates side by side and "do this for
// every conflict".  And the failure dialog: what went wrong, with Retry,
// Skip, Cancel and, when trashing is not possible in that place, Delete
// permanently.  Both answer the operation through ex_conflict_resolve() /
// ex_issue_resolve() and do not block the window.
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
