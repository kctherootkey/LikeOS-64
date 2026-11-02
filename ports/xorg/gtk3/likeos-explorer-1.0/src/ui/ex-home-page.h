// LikeOS -- File Explorer: Home and This PC.
//
// Home is where a new window starts: Quick access (the pinned folders, and
// when wanted the folders used most), Favorites, Recent files (what the
// desktop's shared list holds, filtered to what still exists and this user
// may open, hidden folders left out), and the drives.  This PC lists the
// drives with how full each is, and the personal folders.
//
// Signals of both:
//   "open"      (ExLocation *loc, gboolean new_tab)
//   "open-file" (ExFile *file)
//   "popup"     (ExFile *file, GdkEventButton *ev)
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

GtkWidget *ex_home_page_new(gboolean this_pc);
void ex_home_page_reload(ExHomePage *p);

G_END_DECLS

#endif
