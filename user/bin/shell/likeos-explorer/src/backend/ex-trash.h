// LikeOS Explorer: the Trash.
//
// The freedesktop.org trash layout in the home folder: files/ holds the
// trashed items, info/<name>.trashinfo records for each where it came from
// and when it was trashed.  Putting things in is GIO's g_file_trash(); this
// is the rest -- listing, reading the records, finding what a trash
// operation produced, and the records' upkeep on restore and delete.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_TRASH_H
#define EX_TRASH_H

#include <gio/gio.h>

G_BEGIN_DECLS

GFile *ex_trash_info_dir(void);
/* info/<name>.trashinfo for an item of files/. */
GFile *ex_trash_info_file_for(GFile *trash_item);

/* Where an item came from (an absolute path) and when it went (seconds
 * since the epoch, local time as the record keeps it). */
gboolean ex_trash_read_info(GFile *trash_item, char **orig_path,
			    gint64 *deleted, GError **error);

/* The item g_file_trash() just made of `orig_path': the newest record that
 * names it.  NULL when there is none. */
GFile *ex_trash_find_item(const char *orig_path);

/* Every item, as ExFile objects carrying their trash records.  `done' runs
 * on the main loop; ex_trash_list_finish() hands over the array. */
void ex_trash_list_async(GCancellable *cancellable, GAsyncReadyCallback done,
			 gpointer user_data);
GPtrArray *ex_trash_list_finish(GAsyncResult *res, GError **error);

/* How many items the Trash holds (a directory count, no records read). */
guint ex_trash_count(void);
/* Whether it holds anything: one directory entry read, so cheap enough to
 * ask on the main loop (the Trash icon). */
gboolean ex_trash_is_empty(void);

G_END_DECLS

#endif
