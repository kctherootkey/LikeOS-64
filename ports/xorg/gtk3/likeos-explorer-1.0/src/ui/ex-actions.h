// LikeOS Explorer: the window's commands.
//
// One GAction per command ("win.copy", "win.view-mode", ...), installed on
// each window.  The command bar's buttons, the context menus and the
// keyboard shortcuts all trigger these, so a command behaves the same from
// wherever it is reached, and it is enabled from one place: the current
// tab's location, its selection and what the selected items allow.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_ACTIONS_H
#define EX_ACTIONS_H

#include "ui/ex-window.h"

G_BEGIN_DECLS

void ex_actions_install(ExWindow *w);
void ex_actions_update(ExWindow *w);
/* Keyboard shortcuts, once per application. */
void ex_actions_set_accels(GtkApplication *app);

G_END_DECLS

#endif
