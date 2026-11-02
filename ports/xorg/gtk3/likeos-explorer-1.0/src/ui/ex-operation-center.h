// LikeOS Explorer: the operation center.
//
// One window for every transfer of the session: each running or waiting
// operation a white card on the window colour -- its line ("Copying 1 item
// from Downloads to Desktop"), its status, Pause and Cancel, a progress bar
// and, with more details, the speed sparkline and what remains -- one under
// the other, the title saying the percentage (or how many run).  Finished
// ones follow with their outcome, and for those that failed, the errors one
// by one, until cleared.  It opens on its own when an operation takes longer
// than a moment and closes once everything finished without errors; the
// status bar's operations button opens it at any time.  It opens with the
// speed graph each time; fewer details lasts while it stays open.
//
// While an operation is not finished, or the window shows, the program
// keeps running with no Explorer window open: an operation handed over on
// the command line (likeos-explorer --copy-to and the others, which the
// desktop uses) runs and shows here like any other.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_OPERATION_CENTER_H
#define EX_OPERATION_CENTER_H

#include <gtk/gtk.h>
#include "backend/ex-fileops.h"

G_BEGIN_DECLS

/* Called once: follow the queue from now on. */
void ex_operation_center_init(void);
void ex_operation_center_present(GtkWindow *parent);
/* Queue `op', which no Explorer window started, its questions asked over
 * the Explorer window in front or this window. */
void ex_operation_center_run(ExOperation *op, gboolean undoable);

G_END_DECLS

#endif
