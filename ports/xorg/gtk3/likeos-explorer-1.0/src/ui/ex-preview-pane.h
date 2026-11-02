// LikeOS Explorer: the details and preview panes.
//
// The pane at the right of a tab.  Details: the selected item's picture,
// name, type, size, dates and place -- or for several, how many and their
// total size -- or for none, the folder itself.  Preview: the item's
// contents -- an image, the beginning of a text, a video's frame with its
// length and size, a song's title and length -- loaded in the background
// and replaced when the selection moves on.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_PREVIEW_PANE_H
#define EX_PREVIEW_PANE_H

#include <gtk/gtk.h>
#include "services/ex-selection.h"
#include "backend/ex-location.h"

G_BEGIN_DECLS

GtkWidget *ex_preview_pane_new(ExSelection *sel);
/* "details" or "preview". */
void ex_preview_pane_set_mode(GtkWidget *pane, const char *mode);
void ex_preview_pane_set_location(GtkWidget *pane, ExLocation *loc);

G_END_DECLS

#endif
