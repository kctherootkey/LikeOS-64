// LikeOS -- File Explorer: a window.
//
// A window is a row of tabs, each an ExTab, and the actions every part of
// it uses: the command bar's buttons, the context menus and the keyboard
// shortcuts all trigger the same "win.*" actions, whose enabled state the
// window keeps in step with the current tab's location and selection.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_WINDOW_H
#define EX_WINDOW_H

#include <gtk/gtk.h>
#include "backend/ex-location.h"
#include "backend/ex-fileops.h"

G_BEGIN_DECLS

typedef struct _ExTab ExTab;

#define EX_TYPE_WINDOW (ex_window_get_type())
G_DECLARE_FINAL_TYPE(ExWindow, ex_window, EX, WINDOW, GtkApplicationWindow)

ExWindow *ex_window_new(GtkApplication *app, ExLocation *loc);

ExTab *ex_window_current_tab(ExWindow *w);
ExTab *ex_window_add_tab(ExWindow *w, ExLocation *loc, gboolean focus);
void ex_window_close_tab(ExWindow *w, ExTab *tab);
/* Recompute which actions are enabled (selection, location, clipboard,
 * undo changed). */
void ex_window_update_actions(ExWindow *w);

/* Queue an operation started from this window: its questions (name
 * conflicts, failures) are asked over this window; `select_results' selects
 * what it made when it ends in the folder the tab shows. */
void ex_window_run(ExWindow *w, ExOperation *op, gboolean undoable,
		   gboolean select_results);

/* Open items: folders in this tab (or a new one), files with their
 * applications. */
void ex_window_open(ExWindow *w, GPtrArray *files, gboolean new_tab);
void ex_window_open_with(ExWindow *w, GPtrArray *files, GAppInfo *app);

void ex_window_show_error(ExWindow *w, const char *primary,
			  const char *secondary);

G_END_DECLS

#endif
