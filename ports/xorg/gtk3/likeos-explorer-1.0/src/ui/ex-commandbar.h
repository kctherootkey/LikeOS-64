// LikeOS Explorer: the command bar.
//
// The toolbar under the address row: New, a labelled button outlined in
// the accent; the clipboard group (Cut, Copy, Paste) and the item group
// (Rename, Share, Delete) as icon buttons named by their tooltips; Sort and
// View (with Filter or Collection where they apply) as menu buttons; "more"
// behind the three dots; and at the far end the switch between the details
// and the preview pane.  The groups stand apart by hairlines.  Every button
// is an action, so it greys out what the selection does not allow.  Its
// contents follow the place: the Trash shows Empty and Restore, a search
// drops New and Paste, Home and Computer keep the whole bar with what does
// not apply there greyed, Home adds Filter and Gallery its Collection, and
// a selection of pictures brings the picture commands.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_COMMANDBAR_H
#define EX_COMMANDBAR_H

#include <gtk/gtk.h>
#include "backend/ex-location.h"

G_BEGIN_DECLS

GtkWidget *ex_commandbar_new(void);
void ex_commandbar_set_kind(GtkWidget *bar, ExLocationKind kind);
/* Pictures are selected: Set as background, Rotate left, Rotate right. */
void ex_commandbar_set_images(GtkWidget *bar, gboolean images);

G_END_DECLS

#endif
