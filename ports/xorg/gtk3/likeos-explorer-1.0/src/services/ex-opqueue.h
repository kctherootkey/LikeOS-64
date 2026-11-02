// LikeOS Explorer: the operation queue.
//
// Every operation of every window goes through here.  At most two run at a
// time, and two that write into the same folder never run at once -- a copy
// and a move into one destination would ask about each other's names.  The
// queue keeps the session's history, which the operation center shows.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_OPQUEUE_H
#define EX_OPQUEUE_H

#include "backend/ex-fileops.h"

G_BEGIN_DECLS

#define EX_TYPE_OPQUEUE (ex_opqueue_get_type())
G_DECLARE_FINAL_TYPE(ExOpQueue, ex_opqueue, EX, OPQUEUE, GObject)

/* Signals:
 *   "added"    (ExOperation *)
 *   "started"  (ExOperation *)
 *   "finished" (ExOperation *)
 */
ExOpQueue *ex_opqueue_get(void);

/* Queue `op' (the queue takes a reference).  `undoable': record it for
 * Undo when it finishes. */
void ex_opqueue_add(ExOpQueue *q, ExOperation *op, gboolean undoable);
/* Operations not finished yet, oldest first (not new references). */
GPtrArray *ex_opqueue_active(ExOpQueue *q);
/* Everything of this session, newest first. */
GPtrArray *ex_opqueue_history(ExOpQueue *q);
void ex_opqueue_clear_finished(ExOpQueue *q);
gboolean ex_opqueue_is_undoable(ExOpQueue *q, ExOperation *op);

G_END_DECLS

#endif
