// LikeOS Explorer: which application opens what.
//
// The system's association database -- the .desktop files and
// mimeapps.list, through GAppInfo -- and nothing of Explorer's own: no
// program is named here, and changing a default here changes it for every
// program that asks the same question.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_APPS_H
#define EX_APPS_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* The default for a content type, or NULL. */
GAppInfo *ex_apps_default_for(const char *content_type);
/* Applications to offer under "Open with": the default first, then the
 * recommended ones, no duplicates, never the file manager itself for a
 * folder.  Array of GAppInfo. */
GPtrArray *ex_apps_for(const char *content_type);
/* The content type every file of the list shares, or NULL when they
 * differ (then only "Open" per file makes sense). */
const char *ex_apps_common_type(GPtrArray *content_types);

/* Make `app' the default for `content_type' (the user's mimeapps.list). */
gboolean ex_apps_set_default(GAppInfo *app, const char *content_type,
			     GError **error);

/* Open `files' (GFile) with `app' -- or each with its default when `app' is
 * NULL.  `ctx' is the display's launch context (startup notification, the
 * screen); may be NULL. */
gboolean ex_apps_launch(GAppInfo *app, GPtrArray *files,
			GAppLaunchContext *ctx, GError **error);

/* What opening `f' does when it is a program the user may run: nothing of
 * the kind (a document, opened with its app); a program that makes its own
 * windows, started as it is; or a console program -- one linked against no
 * windowing library, or a script (#!) -- which runs in a terminal window,
 * so that what it prints can be read. */
typedef enum {
	EX_PROGRAM_NONE,
	EX_PROGRAM_WINDOWED,
	EX_PROGRAM_CONSOLE,
} ExProgramKind;

ExProgramKind ex_apps_program_kind(GFile *f, gboolean can_execute);

G_END_DECLS

#endif
