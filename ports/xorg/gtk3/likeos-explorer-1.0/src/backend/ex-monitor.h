// LikeOS -- File Explorer: watching a folder for changes made elsewhere.
//
// GFileMonitor where the location supports one -- on LikeOS's own disks the
// kernel's inotify -- and, where it does not, a slow poll of the folder's
// modification time that asks for a full re-read when it moves.  The view
// above does not need to know which: both end in the same signals.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_MONITOR_H
#define EX_MONITOR_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define EX_TYPE_MONITOR (ex_monitor_get_type())
G_DECLARE_FINAL_TYPE(ExMonitor, ex_monitor, EX, MONITOR, GObject)

/* Signals:
 *   "created" (GFile *child)     -- a name appeared
 *   "deleted" (GFile *child)     -- a name went away
 *   "changed" (GFile *child)     -- contents or attributes changed
 *   "renamed" (GFile *from, GFile *to) -- within the folder
 *   "gone"    ()                 -- the folder itself was deleted or moved
 *   "rescan"  ()                 -- something changed; read it all again
 */
ExMonitor *ex_monitor_new(GFile *dir);

/* TRUE when changes arrive as they happen, FALSE when polled. */
gboolean ex_monitor_is_live(ExMonitor *m);

G_END_DECLS

#endif
