// LikeOS Explorer: one operation in the progress window.
//
// Each transfer is a card:
//
//     [⧉]  Copying 1 item from Downloads to Desktop          [‖] [✕]
//          33% complete
//     ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//     ╭ Speed: 257 MB/s ───────────────────────────────────────────╮
//     │          ⌒‿⌒‿⌒●  (the speed sparkline)                      │
//     ╰─────────────────────────────────────────────────────────────╯
//     FILE                 TIME LEFT          REMAINING
//     ubuntu-24.04.iso     About 2 minutes    1 (3.92 GB)
//
// a badge for the kind of operation, the folder names as links that open
// them, Pause / Resume and Cancel as icon buttons, the progress bar, and
// with more details the sparkline and the three facts under it.  Waiting
// operations say so; finished ones say how they ended and list what could
// not be done.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_PROGRESS_ITEM_H
#define EX_PROGRESS_ITEM_H

#include <gtk/gtk.h>
#include "backend/ex-fileops.h"

G_BEGIN_DECLS

/* The panel for `op' (it keeps a reference).  It follows the operation's
 * "progress" signal on its own. */
GtkWidget *ex_progress_item_new(ExOperation *op);
ExOperation *ex_progress_item_get_operation(GtkWidget *item);
/* More details (the sparkline, the file, the time and what remains) or
 * fewer (the line, the status and the progress bar). */
void ex_progress_item_set_details(GtkWidget *item, gboolean more);
/* Once the operation finished: its outcome and its errors. */
void ex_progress_item_finish(GtkWidget *item);

/* "33% complete", "Paused – 33% complete", "Calculating…" -- the status
 * line, which the window also takes as its title. */
char *ex_progress_status_text(ExOperation *op);
/* "Copying 1 item from Downloads to Desktop": a label whose folder names
 * are links opening them in a new window. */
GtkWidget *ex_progress_header_new(ExOperation *op);
void ex_progress_header_update(GtkWidget *label, ExOperation *op);

G_END_DECLS

#endif
