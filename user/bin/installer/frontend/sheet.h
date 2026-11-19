/* LikeOS installer -- dialogs drawn inside the installer's own window.
 *
 * The installer usually runs without a window manager, where a separate
 * dialog window would appear undecorated in a corner.  A sheet instead dims
 * the wizard and shows a rounded card in its middle: a title, a body that
 * scrolls, and a row of buttons.  Esc answers SHEET_CANCEL.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_SHEET_H
#define LIKEOS_INSTALLER_SHEET_H

#include "app.h"
#include "art/art.h"

enum {
	SHEET_CANCEL = 0,
	SHEET_OK = 1,
	SHEET_OTHER = 2,
	SHEET_THIRD = 3,
};

typedef struct Sheet Sheet;
typedef void (*SheetCb)(Sheet *s, int response, gpointer data);

Sheet *sheet_new(App *app, const char *title, ArtKind art);
void sheet_set_body(Sheet *s, GtkWidget *body);
void sheet_set_text(Sheet *s, const char *markup);
/* style: "primary", "danger" or NULL */
GtkWidget *sheet_add_button(Sheet *s, const char *label, int response,
			    const char *style);
/* A wide sheet (licence text, disk details). */
void sheet_set_wide(Sheet *s, gboolean wide);
void sheet_present(Sheet *s, SheetCb cb, gpointer data);
void sheet_close(Sheet *s, int response);
/* The sheet's response buttons can be disabled while it works. */
void sheet_set_busy(Sheet *s, gboolean busy);

gboolean sheet_is_open(App *app);
/* Esc: closes the open sheet; TRUE when there was one. */
gboolean sheet_escape(App *app);

/* Convenience: a question with two buttons. */
void sheet_confirm(App *app, const char *title, const char *markup,
		   const char *ok_label, gboolean danger, ArtKind art,
		   SheetCb cb, gpointer data);
/* A text to read: plain monospace (licence) or Pango markup (help). */
void sheet_show_text(App *app, const char *title, const char *text,
		     gboolean monospace);
/* Leaving setup: restart, shut down, or end setup (the installer session
 * then offers the text-mode installer, a login and more). */
void sheet_power(App *app);
/* Answers the open sheet as its button would (the autopilot); FALSE when
 * none is open. */
gboolean sheet_respond(App *app, int response);

#endif
