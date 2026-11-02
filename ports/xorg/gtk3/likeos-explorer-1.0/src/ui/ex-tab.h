// LikeOS Explorer: one tab.
//
// What a tab shows and how: its navigation bar (Back, Forward, Up, the
// address bar, search), the command bar, the navigation pane, the content
// -- Home, Computer, or the items of a folder, the Trash or a search -- the
// optional details or preview pane, and the status bar.  The tab owns the
// navigator (where it is and has been), the selection, the folder model and
// the monitor that keeps the model current.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_TAB_H
#define EX_TAB_H

#include "ui/ex-window.h"
#include "services/ex-navigator.h"
#include "services/ex-selection.h"

G_BEGIN_DECLS

/* The tab's top widget is a GtkBox; ExTab is reached from it with
 * ex_tab_from_widget(). */
ExTab *ex_tab_new(ExWindow *win, ExLocation *loc);
GtkWidget *ex_tab_widget(ExTab *t);
ExTab *ex_tab_from_widget(GtkWidget *w);
/* The tab label (icon + name + close button). */
GtkWidget *ex_tab_label(ExTab *t);

ExLocation *ex_tab_location(ExTab *t);
ExNavigator *ex_tab_navigator(ExTab *t);
ExSelection *ex_tab_selection(ExTab *t);
/* The folder new items and pastes go into: the folder shown, or NULL
 * (Home, Computer, the Trash, a search). */
GFile *ex_tab_folder(ExTab *t);
/* Whether the folder shown accepts new items. */
gboolean ex_tab_folder_writable(ExTab *t);
char *ex_tab_title(ExTab *t);

void ex_tab_go(ExTab *t, ExLocation *loc);
void ex_tab_refresh(ExTab *t);
void ex_tab_focus_address(ExTab *t);
void ex_tab_focus_search(ExTab *t);
void ex_tab_focus_view(ExTab *t);

const char *ex_tab_view_mode(ExTab *t);
void ex_tab_set_view_mode(ExTab *t, const char *mode);
void ex_tab_set_sort(ExTab *t, const char *column, gboolean ascending);
void ex_tab_get_sort(ExTab *t, const char **column, gboolean *ascending);
void ex_tab_set_right_pane(ExTab *t, const char *pane);
void ex_tab_set_navigation_pane(ExTab *t, gboolean shown);
void ex_tab_apply_settings(ExTab *t); /* hidden items, extensions, click */

/* Every item shown (ExFile, new array of new references). */
GPtrArray *ex_tab_listed(ExTab *t);

void ex_tab_select_all(ExTab *t);
void ex_tab_select_none(ExTab *t);
void ex_tab_invert_selection(ExTab *t);
void ex_tab_select_files(ExTab *t, GPtrArray *files);
/* Whether everything selected is a picture (the picture commands). */
gboolean ex_tab_selection_is_images(ExTab *t);
/* Home's Filter: the kind of file Recent and Favorites show ("all", or a
 * kind of ex_file_get_kind()). */
void ex_tab_set_home_filter(ExTab *t, const char *kind);
const char *ex_tab_home_filter(ExTab *t);
void ex_tab_start_rename(ExTab *t);
void ex_tab_rename_when_shown(ExTab *t, GFile *f);
/* The selection is about to be deleted (see ex_file_view_note_removal()). */
void ex_tab_note_removal(ExTab *t);

/* Context menu for the selection (on_item) or the background, at the
 * pointer (ev) or at the focus (ev NULL). */
void ex_tab_popup(ExTab *t, GdkEventButton *ev, gboolean on_item);

void ex_tab_destroy(ExTab *t);

G_END_DECLS

#endif
