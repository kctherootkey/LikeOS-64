// LikeOS -- File Explorer: context menus.
//
// Built from the same actions as the command bar, so a right click offers
// exactly what the selection allows: on items -- Open, Open with, Open in
// new tab, Cut, Copy, Create shortcut, Rename, Delete, Pin, Favorite, Copy
// as path, Share, Properties (Restore and Delete permanently in the Trash);
// on the background -- View, Sort by, Refresh, Paste, Undo, New, Open in
// Terminal, Properties.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_CONTEXT_MENU_H
#define EX_CONTEXT_MENU_H

#include "ui/ex-tab.h"

G_BEGIN_DECLS

GMenuModel *ex_context_menu_items(ExTab *t);
GMenuModel *ex_context_menu_background(ExTab *t);
/* A navigation-pane row. */
GMenuModel *ex_context_menu_location(ExLocation *loc);
/* "New" submenu (shared with the command bar). */
GMenuModel *ex_context_menu_new_items(void);
GMenuModel *ex_context_menu_view(void);
GMenuModel *ex_context_menu_sort(void);
GMenuModel *ex_context_menu_more(void);

/* Show `model' as a menu over `widget'. */
void ex_context_menu_popup(GtkWidget *widget, GMenuModel *model,
			   GdkEventButton *ev);

G_END_DECLS

#endif
