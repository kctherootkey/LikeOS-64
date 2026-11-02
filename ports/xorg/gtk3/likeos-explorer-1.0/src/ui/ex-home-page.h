// LikeOS Explorer: Home.
//
// Where a new window starts.  Pinned: a grid of folder cards -- the pinned
// folders (or, with none pinned, the personal ones) and, when wanted, the
// folders used most.  Under it Recent files and Favorites, two lists behind
// a two-way switch, each in a card: the files the desktop's shared list
// holds (filtered to what still exists and this user may open, hidden
// folders left out) and the files marked as favorites, each with its folder
// and when it was last opened.  Both groups fold away under their headings.
// Computer has a page of its own (ex-computer-page.h).
//
// Signals:
//   "open"           (ExLocation *loc, gboolean new_tab)
//   "open-file"      (ExFile *file)
//   "popup"          (ExFile *file, GdkEventButton *ev)
//   "popup-location" (ExLocation *loc, GdkEventButton *ev)
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_HOME_PAGE_H
#define EX_HOME_PAGE_H

#include <gtk/gtk.h>
#include "backend/ex-file.h"
#include "backend/ex-location.h"

G_BEGIN_DECLS

#define EX_TYPE_HOME_PAGE (ex_home_page_get_type())
G_DECLARE_FINAL_TYPE(ExHomePage, ex_home_page, EX, HOME_PAGE,
		     GtkScrolledWindow)

GtkWidget *ex_home_page_new(void);
void ex_home_page_reload(ExHomePage *p);
/* The command bar's Filter: Recent and Favorites list only the files of one
 * kind (as ex_file_get_kind() names them: "document", "image", "video",
 * "audio", "folder", "archive"), or of all kinds ("all"). */
void ex_home_page_set_filter(ExHomePage *p, const char *kind);
const char *ex_home_page_get_filter(ExHomePage *p);

G_END_DECLS

#endif
