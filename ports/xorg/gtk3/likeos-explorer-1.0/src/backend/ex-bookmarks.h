// LikeOS -- File Explorer: bookmarks.
//
// The GTK bookmarks file (~/.config/gtk-3.0/bookmarks): one "URI [label]"
// per line, the list the GTK file chooser and the PCManFM desktop show as
// well.  Explorer's sidebar shows it under Pinned; pinning a folder in
// Explorer adds to it, so the folder is pinned in every file dialog too.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_BOOKMARKS_H
#define EX_BOOKMARKS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
	GFile *file;
	char *label; /* NULL: the folder's own name */
} ExBookmark;

#define EX_TYPE_BOOKMARKS (ex_bookmarks_get_type())
G_DECLARE_FINAL_TYPE(ExBookmarks, ex_bookmarks, EX, BOOKMARKS, GObject)

/* The one list; "changed" when the file changes, whoever changed it. */
ExBookmarks *ex_bookmarks_get_default(void);
GPtrArray *ex_bookmarks_list(ExBookmarks *b); /* ExBookmark*, owned */
gboolean ex_bookmarks_contains(ExBookmarks *b, GFile *f);
void ex_bookmarks_add(ExBookmarks *b, GFile *f, const char *label);
void ex_bookmarks_remove(ExBookmarks *b, GFile *f);
/* Move entry `from' to position `to'. */
void ex_bookmarks_move(ExBookmarks *b, guint from, guint to);

G_END_DECLS

#endif
