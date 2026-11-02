// LikeOS -- File Explorer: recently used files.
//
// The desktop's shared list, ~/.local/share/recently-used.xbel, which GTK
// programs add to when they open or save a file.  What is handed out has
// been checked: only local files that still exist and that this user may
// read -- a list never shows a name the user could not open, nor one inside
// a hidden folder unless asked.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_RECENT_H
#define EX_RECENT_H

#include <gio/gio.h>

G_BEGIN_DECLS

GFile *ex_recent_file(void);

/* Up to `max' items, newest first, as ExFile.  `include_hidden': also items
 * below a hidden folder. */
void ex_recent_list_async(guint max, gboolean include_hidden,
			  GCancellable *cancellable, GAsyncReadyCallback done,
			  gpointer user_data);
GPtrArray *ex_recent_list_finish(GAsyncResult *res, GError **error);

/* Remove everything (for every program: the list is shared). */
gboolean ex_recent_clear(GError **error);
/* Remove one entry. */
gboolean ex_recent_forget(GFile *file, GError **error);

G_END_DECLS

#endif
