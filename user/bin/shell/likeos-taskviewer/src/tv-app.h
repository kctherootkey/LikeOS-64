// LikeOS Task Viewer -- what the window and the process pages (Processes,
// Details, Users) share beyond the page contract of tv-pages.h: the
// settings file, moving between pages, the Start new task dialog and the
// pieces of the process views (actions on a process, the tinted load
// cells, the column headers with their totals).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_APP_H
#define TV_APP_H

#include <gtk/gtk.h>

#include "tv-pages.h"
#include "tv-procs.h"

G_BEGIN_DECLS

/* ---- settings (tv-window.c) ----------------------------------------------
 * One key file, $XDG_CONFIG_HOME/likeos-taskviewer/settings.ini, group
 * [Task Viewer]; written when the window closes and when a setting the
 * pages own changes. */
gboolean tv_settings_get_bool(const char *key, gboolean fallback);
void tv_settings_set_bool(const char *key, gboolean value);
int tv_settings_get_int(const char *key, int fallback);
void tv_settings_set_int(const char *key, int value);
/* NULL-terminated, g_strfreev(); never NULL. */
char **tv_settings_get_strv(const char *key);
void tv_settings_set_strv(const char *key, const char *const *value);
void tv_settings_save(void);

/* ---- the window (tv-window.c) --------------------------------------------- */

/* Shows the page with this id. */
void tv_window_show_page(const char *id);
/* Takes a new process snapshot and ticks every page now. */
void tv_window_refresh_now(void);

/* ---- pages with entries of their own in the "..." menu --------------------- */

/* The Processes page's view options: Group by type, Expand all, Collapse
 * all (tv-processes.c). */
void tv_processes_fill_menu(GtkWidget *page, GtkMenuShell *menu);
/* Shows the Details page with this process selected (tv-details.c). */
void tv_details_select_pid(int pid);

/* ---- Start new task (tv-run.c) ---------------------------------------------- */

void tv_run_dialog(GtkWindow *parent);

/* ---- the process views' common parts (tv-procui.c) ------------------------ */

/* Memory as values or as percents of all memory (Processes and Users). */
gboolean tv_mem_percents(void);
void tv_set_mem_percents(gboolean on);

/* An error in a message dialog over the main window. */
void tv_show_error(const char *primary, const GError *error);

/* Ends a process politely: SIGTERM; when it is still there a few seconds
 * later, offers to force it. */
void tv_end_process(int pid);
/* Closes the windows of an app (the window manager's close), with the same
 * offer when the app is still running a few seconds later. */
void tv_end_app(int pid);
/* Closes one window (and offers to end its process when the window is
 * still there a few seconds later). */
void tv_end_window(gulong xid);
/* Ends a process at once (SIGKILL), after asking. */
void tv_force_end(int pid);
/* Ends a process and everything it started, after asking. */
void tv_end_tree(int pid);
/* Opens the folder of the program file in Explorer, the file selected. */
void tv_open_file_location(int pid);
/* The properties dialog: name, path, PID, user, start, command line. */
void tv_show_properties(int pid);
/* The processor affinity dialog. */
void tv_show_affinity(int pid);

/* Load shading: tints a numeric cell with the accent colour for `fraction'
 * (0..1) of load; 0 clears it. */
void tv_cell_heat(GtkCellRenderer *cell, GtkWidget *view, double fraction);

/* A column header with a total above the column's name ("17%" over
 * "CPU"); `*total' is the label to update. */
GtkWidget *tv_total_header(const char *title, gboolean numeric,
			   GtkWidget **total);

/* Puts a menu item into `menu'.  `cb' gets `data'. */
GtkWidget *tv_menu_add(GtkWidget *menu, const char *label, GCallback cb,
		       gpointer data, gboolean sensitive);
GtkWidget *tv_menu_separator(GtkWidget *menu);
/* Frees a popup menu once it has closed.  A menu is taken down
 * ("deactivate") before the chosen item is activated, so freeing it in
 * "deactivate" would disconnect the item's handler before it runs: the
 * menu goes from the main loop, after the item has done its work. */
void tv_menu_free_when_closed(GtkWidget *menu);

/* A command bar button: glyph (TvGlyph, 0 for none) and label. */
GtkWidget *tv_command_button(int glyph, const char *label);

G_END_DECLS

#endif
