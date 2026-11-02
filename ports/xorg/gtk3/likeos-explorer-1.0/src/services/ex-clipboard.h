// LikeOS -- File Explorer: cut, copy and paste of files.
//
// The X clipboard, in the formats the rest of the desktop speaks: GNOME's
// x-special/gnome-copied-files ("copy" or "cut", then URIs) -- what the
// PCManFM desktop reads and writes, so files copied on the desktop paste in
// Explorer and the other way round -- text/uri-list for other programs, and
// plain text (the paths) for a terminal or an editor.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_CLIPBOARD_H
#define EX_CLIPBOARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define EX_TYPE_CLIPBOARD (ex_clipboard_get_type())
G_DECLARE_FINAL_TYPE(ExClipboard, ex_clipboard, EX, CLIPBOARD, GObject)

/* "changed" () when whether something can be pasted may have changed. */
ExClipboard *ex_clipboard_get(void);

/* `files' of GFile. */
void ex_clipboard_copy(ExClipboard *c, GPtrArray *files);
void ex_clipboard_cut(ExClipboard *c, GPtrArray *files);
/* Paths as text (Copy path). */
void ex_clipboard_copy_paths(ExClipboard *c, GPtrArray *files);

/* Whether files are on the clipboard (as last seen; kept current from the
 * clipboard's owner-change notifications, and by ex_clipboard_refresh()). */
gboolean ex_clipboard_can_paste(ExClipboard *c);
/* Look at what the clipboard holds again (a window became active: another
 * program may have copied something meanwhile). */
void ex_clipboard_refresh(ExClipboard *c);
/* Whether `f' is on the clipboard to be cut (shown dimmed). */
gboolean ex_clipboard_is_cut(ExClipboard *c, GFile *f);

/* What is on it: `done' gets the files (GPtrArray of GFile, or NULL) and
 * whether they were cut. */
typedef void (*ExPasteFunc)(GPtrArray *files, gboolean cut,
			    gpointer user_data);
void ex_clipboard_request(ExClipboard *c, ExPasteFunc done,
			  gpointer user_data);
/* After a cut was pasted: the clipboard is emptied, as the files moved. */
void ex_clipboard_clear_cut(ExClipboard *c);

G_END_DECLS

#endif
