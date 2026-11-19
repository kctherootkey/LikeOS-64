// LikeOS -- split button.
//
// A reusable LikeOS widget: a command with its most likely choice on the
// main part and the others behind an arrow -- "New" creates a folder at a
// click and offers the other kinds in its menu.  Both halves work with
// actions, so they enable and disable with the command they stand for.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LX_SPLIT_BUTTON_H
#define LX_SPLIT_BUTTON_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A linked box: [icon label][▾].  `action' is the main part's detailed
 * action name; `menu' what the arrow opens. */
GtkWidget *lx_split_button_new(const char *icon_name, const char *label,
			       const char *action, GMenuModel *menu);
/* A button with a dropdown only ("Sort ▾", "View ▾"). */
GtkWidget *lx_menu_button_new(const char *icon_name, const char *label,
			      GMenuModel *menu);

G_END_DECLS

#endif
