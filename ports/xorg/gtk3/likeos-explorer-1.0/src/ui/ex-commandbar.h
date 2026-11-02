// LikeOS -- File Explorer: the command bar.
//
// The row of commands under the address bar -- New, Cut, Copy, Paste,
// Rename, Share, Delete, Sort, View and "more" -- every button an action,
// so it greys out what the selection does not allow.  Its contents follow
// the place: the Trash shows Empty and Restore, a search drops New and
// Paste, Home and This PC keep only what applies there.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_COMMANDBAR_H
#define EX_COMMANDBAR_H

#include <gtk/gtk.h>
#include "backend/ex-location.h"

G_BEGIN_DECLS

GtkWidget *ex_commandbar_new(void);
void ex_commandbar_set_kind(GtkWidget *bar, ExLocationKind kind);

G_END_DECLS

#endif
