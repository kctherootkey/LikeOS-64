// LikeOS Explorer: the navigation pane.
//
// Three groups under small headings: Favorites -- Home, Gallery, the
// personal folders and the folders pinned (the shared GTK bookmarks), each
// with its pin, and the Trash, full or empty; Devices -- Computer and its
// drives; Network, only when some backend provides network places.  Each
// row is a location id, not a path, so a row follows its place.
//
// Signals:
//   "open"   (ExLocation *loc, gboolean new_tab)
//   "drop"   (GPtrArray *uris, GFile *dest)   files dropped on a row
//   "popup"  (ExLocation *loc, GdkEventButton *ev)
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_SIDEBAR_H
#define EX_SIDEBAR_H

#include <gtk/gtk.h>
#include "backend/ex-location.h"

G_BEGIN_DECLS

#define EX_TYPE_SIDEBAR (ex_sidebar_get_type())
G_DECLARE_FINAL_TYPE(ExSidebar, ex_sidebar, EX, SIDEBAR, GtkScrolledWindow)

GtkWidget *ex_sidebar_new(void);
/* Highlight the row for `loc' (or nothing when there is none). */
void ex_sidebar_set_location(ExSidebar *s, ExLocation *loc);

G_END_DECLS

#endif
