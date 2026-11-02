// LikeOS -- File Explorer: one item in a listing.
//
// An ExFile is a GFile together with what was last learned about it (a
// GFileInfo), and what the item allows -- its capabilities -- worked out from
// that information once, so a command bar and a context menu ask the same
// question and get the same answer.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_FILE_H
#define EX_FILE_H

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

G_BEGIN_DECLS

/* What a listing asks for.  The cheap content type: the one guessed from the
 * name, as sniffing every file of a large folder would read every file. */
#define EX_FILE_ATTRS                                                        \
	G_FILE_ATTRIBUTE_STANDARD_NAME                                       \
	"," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME                           \
	"," G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME                              \
	"," G_FILE_ATTRIBUTE_STANDARD_TYPE                                   \
	"," G_FILE_ATTRIBUTE_STANDARD_SIZE                                   \
	"," G_FILE_ATTRIBUTE_STANDARD_ICON                                   \
	"," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE                      \
	"," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN                              \
	"," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP                              \
	"," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK                             \
	"," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET                         \
	"," G_FILE_ATTRIBUTE_STANDARD_TARGET_URI                             \
	"," G_FILE_ATTRIBUTE_TIME_MODIFIED                                   \
	"," G_FILE_ATTRIBUTE_TIME_ACCESS                                     \
	"," G_FILE_ATTRIBUTE_TIME_CREATED                                    \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_READ                                 \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE                                \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE                              \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE                               \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH                                \
	"," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME                               \
	"," G_FILE_ATTRIBUTE_UNIX_MODE                                       \
	"," G_FILE_ATTRIBUTE_UNIX_UID                                        \
	"," G_FILE_ATTRIBUTE_UNIX_GID                                        \
	"," G_FILE_ATTRIBUTE_UNIX_NLINK                                      \
	"," G_FILE_ATTRIBUTE_OWNER_USER                                      \
	"," G_FILE_ATTRIBUTE_OWNER_GROUP

typedef enum {
	EX_CAP_READ = 1 << 0,
	EX_CAP_WRITE = 1 << 1, /* contents (a folder: may add names) */
	EX_CAP_RENAME = 1 << 2,
	EX_CAP_DELETE = 1 << 3,
	EX_CAP_TRASH = 1 << 4,
	EX_CAP_EXECUTE = 1 << 5,
	EX_CAP_OPEN = 1 << 6, /* there is something to open it with */
	EX_CAP_RESTORE = 1 << 7, /* an item in the Trash */
} ExFileCaps;

#define EX_TYPE_FILE (ex_file_get_type())
G_DECLARE_FINAL_TYPE(ExFile, ex_file, EX, FILE, GObject)

ExFile *ex_file_new(GFile *file, GFileInfo *info);
/* Query synchronously: for one item, off the main loop. */
ExFile *ex_file_new_query(GFile *file, GCancellable *cancellable,
			  GError **error);

GFile *ex_file_get_file(ExFile *f);
GFileInfo *ex_file_get_info(ExFile *f);
/* Replace what is known about the item; emits "changed". */
void ex_file_set_info(ExFile *f, GFileInfo *info);

const char *ex_file_get_name(ExFile *f); /* display name */
const char *ex_file_get_edit_name(ExFile *f);
const char *ex_file_get_collate_key(ExFile *f);
const char *ex_file_get_content_type(ExFile *f);
char *ex_file_get_type_description(ExFile *f);
GIcon *ex_file_get_icon(ExFile *f);
guint64 ex_file_get_size(ExFile *f);
gint64 ex_file_get_mtime(ExFile *f);
gboolean ex_file_is_dir(ExFile *f);
gboolean ex_file_is_hidden(ExFile *f);
gboolean ex_file_is_symlink(ExFile *f);
ExFileCaps ex_file_get_caps(ExFile *f);
/* "Folder", or the extension in capitals ("PNG file") -- the Type column. */
char *ex_file_get_kind_label(ExFile *f);
/* One of "folder", "image", "video", "audio", "document", "archive",
 * "program", "other": what search's kind: filter and the Gallery go by. */
const char *ex_file_get_kind(ExFile *f);

/* The Trash records where an item came from and when it went. */
void ex_file_set_trash_info(ExFile *f, const char *orig_path,
			    gint64 deleted);
const char *ex_file_get_trash_orig_path(ExFile *f);
gint64 ex_file_get_trash_deleted(ExFile *f);

/* A thumbnail once one is known (NULL until then) and whether asking for
 * one is worth it. */
GdkPixbuf *ex_file_get_thumbnail(ExFile *f);
void ex_file_set_thumbnail(ExFile *f, GdkPixbuf *thumb);
gboolean ex_file_thumbnail_failed(ExFile *f);
void ex_file_set_thumbnail_failed(ExFile *f);

G_END_DECLS

#endif
