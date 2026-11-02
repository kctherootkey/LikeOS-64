// LikeOS -- File Explorer: the operation center.
//
// One window for every transfer of the session: what runs and what waits,
// each with its progress, speed, the item being worked on and a Cancel
// button; finished ones with their outcome, and for those that failed, the
// errors one by one.  It opens on its own when an operation takes longer
// than a moment and closes once everything finished without errors; the
// status bar's operations button opens it at any time.
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

G_END_DECLS

#endif
