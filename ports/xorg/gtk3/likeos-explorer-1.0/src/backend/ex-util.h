// LikeOS Explorer: small helpers the whole backend shares.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_UTIL_H
#define EX_UTIL_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* "1.4 MB", "512 bytes" -- decimal units, as the rest of the system shows
 * sizes (libfm.conf si_unit=1). */
char *ex_format_size(guint64 bytes);

/* "Today, 14:02", "Yesterday, 09:10", "12.03.2026 18:44" for a time in
 * seconds since the epoch, local time. */
char *ex_format_time(gint64 unix_time);

/* A name for a new item in `dir' that does not exist yet: `base' itself,
 * then "base (2)", "base (3)" ... with the extension kept after the number
 * when `keep_ext' ("photo (2).jpg"). */
char *ex_unique_name(GFile *dir, const char *base, gboolean keep_ext);

/* The extension of `name' including the dot, or "" (".tar.gz" counts as
 * one). */
const char *ex_name_extension(const char *name);

/* Case-insensitive, accent-preserving key for comparing names the way a
 * person would ("File10" after "File9"). */
char *ex_collate_key(const char *name);

/* TRUE when `name' would be hidden in a listing (leading dot or trailing
 * tilde). */
gboolean ex_name_is_hidden(const char *name);

/* A name the user typed for a new or renamed item: NULL when acceptable,
 * otherwise a sentence saying why not. */
const char *ex_name_problem(const char *name);

/* Run `fn' on `ctx''s own thread, from any thread.  Unlike
 * g_main_context_invoke() this never runs `fn' in the calling thread: a
 * worker holding its own locks must not find itself inside a signal
 * handler that takes them. */
void ex_run_on(GMainContext *ctx, GSourceFunc fn, gpointer data,
	       GDestroyNotify destroy);

/* The home directory, never NULL. */
GFile *ex_home_dir(void);

/* A personal folder: the XDG user directory when one is configured
 * (~/.config/user-dirs.dirs), else ~/Documents, ~/Downloads, ... when that
 * folder exists -- an account without xdg-user-dirs still has its folders
 * found.  NULL when there is none.  Not to be freed. */
const char *ex_user_dir(GUserDirectory which);

/* A new array holding new references to the objects of `a'. */
GPtrArray *ex_objects_copy(GPtrArray *a);

G_END_DECLS

#endif
