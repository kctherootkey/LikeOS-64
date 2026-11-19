// LikeOS Explorer: Computer.
//
// The drives under "Drives", each a card with its picture, its name, a
// slim bar of how full it is -- the accent colour, red when it is nearly
// full -- and "X GB free of Y GB" under it; network shares, when there are
// any, under "Network shares".  Each group folds away under its heading.
//
// Signals:
//   "open"           (ExLocation *loc, gboolean new_tab)
//   "popup-location" (ExLocation *loc, GdkEventButton *ev)  -- ev NULL for
//                    the menu key
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_COMPUTER_PAGE_H
#define EX_COMPUTER_PAGE_H

#include <gtk/gtk.h>
#include "backend/ex-location.h"

G_BEGIN_DECLS

#define EX_TYPE_COMPUTER_PAGE (ex_computer_page_get_type())
G_DECLARE_FINAL_TYPE(ExComputerPage, ex_computer_page, EX, COMPUTER_PAGE,
		     GtkScrolledWindow)

GtkWidget *ex_computer_page_new(void);
void ex_computer_page_reload(ExComputerPage *p);

G_END_DECLS

#endif
