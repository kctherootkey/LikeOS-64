// LikeOS Task Viewer -- the contract between the window and its pages.
//
// The window (tv-window.c) shows a navigation rail on the left and, to its
// right, a header with the page title, a command bar and the current page.
// Every page is described by one TvPageDesc; the window creates each page
// once, keeps it in a GtkStack and drives all of them from one timer at the
// chosen update speed, so a page that keeps a history (the performance
// graphs) keeps sampling while it is not shown.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_PAGES_H
#define TV_PAGES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
	const char *id; /* "processes", "performance", ... (stack child name) */
	const char *title; /* shown in the rail and as the page heading */
	const char *icon; /* themed icon name for the rail */

	/* Builds the page.  Called once, before the window is shown. */
	GtkWidget *(*create)(void);

	/* Called at every update tick for every page (visible tells whether
	 * the page is on screen; a page that only lists things may skip its
	 * work when it is not).  May be NULL. */
	void (*tick)(GtkWidget *page, gboolean visible);

	/* Fills the command bar (a horizontal GtkBox at the right of the
	 * header) with this page's own buttons; the window adds "Run new
	 * task" itself and clears the bar when the page changes.  May be
	 * NULL. */
	void (*fill_commands)(GtkWidget *page, GtkWidget *bar);

	/* The search field's text changed while this page is shown ("" when
	 * cleared).  NULL: the page has nothing to search and the window
	 * hides the field. */
	void (*filter)(GtkWidget *page, const char *text);
} TvPageDesc;

/* The pages, in rail order.  Each is defined in its own source file. */
extern const TvPageDesc tv_page_processes;	/* tv-processes.c  (agent A) */
extern const TvPageDesc tv_page_performance;	/* tv-perf.c       (agent B) */
extern const TvPageDesc tv_page_users;		/* tv-users.c      (agent A) */
extern const TvPageDesc tv_page_details;	/* tv-details.c    (agent A) */
extern const TvPageDesc tv_page_services;	/* tv-services.c   (agent B) */
extern const TvPageDesc tv_page_startup;	/* tv-startup.c    (agent B) */

/* The update interval in milliseconds the window ticks at (0: paused).
 * Pages that compute rates use it rather than assuming one second. */
guint tv_update_interval_ms(void);

/* The window, for dialogs that need a parent. */
GtkWindow *tv_main_window(void);

/* Run a command line the way the "Start new task" dialog does (no shell
 * interpretation beyond splitting the words; errors shown in a dialog). */
gboolean tv_run_command(const char *cmdline, GError **error);

G_END_DECLS

#endif
