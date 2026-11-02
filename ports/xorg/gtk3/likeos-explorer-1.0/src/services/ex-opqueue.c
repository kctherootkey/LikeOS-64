// LikeOS Explorer: the operation queue.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-opqueue.h"
#include "backend/ex-index.h"

#define EX_QUEUE_PARALLEL 2
#define EX_HISTORY_KEEP 50

struct _ExOpQueue {
	GObject parent;
	GPtrArray *waiting; /* ExOperation, oldest first */
	GPtrArray *running;
	GPtrArray *history; /* newest last */
	GHashTable *undoable; /* ExOperation* set */
};

G_DEFINE_TYPE(ExOpQueue, ex_opqueue, G_TYPE_OBJECT)

enum { SIG_ADDED, SIG_STARTED, SIG_FINISHED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_opqueue_class_init(ExOpQueueClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	sigs[SIG_ADDED] = g_signal_new("added", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 1,
				       EX_TYPE_OPERATION);
	sigs[SIG_STARTED] = g_signal_new("started", t, G_SIGNAL_RUN_LAST, 0,
					 NULL, NULL, NULL, G_TYPE_NONE, 1,
					 EX_TYPE_OPERATION);
	sigs[SIG_FINISHED] = g_signal_new("finished", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 1,
					  EX_TYPE_OPERATION);
}

static void ex_opqueue_init(ExOpQueue *q)
{
	q->waiting = g_ptr_array_new_with_free_func(g_object_unref);
	q->running = g_ptr_array_new_with_free_func(g_object_unref);
	q->history = g_ptr_array_new_with_free_func(g_object_unref);
	q->undoable = g_hash_table_new(NULL, NULL);
}

ExOpQueue *ex_opqueue_get(void)
{
	static ExOpQueue *the;

	if (!the)
		the = g_object_new(EX_TYPE_OPQUEUE, NULL);
	return the;
}

/* Whether `op' writes where a running operation writes. */
static gboolean collides(ExOpQueue *q, ExOperation *op)
{
	GFile *d = ex_operation_get_dest_dir(op);

	if (!d)
		return FALSE;
	for (guint i = 0; i < q->running->len; i++) {
		GFile *rd = ex_operation_get_dest_dir(
			g_ptr_array_index(q->running, i));

		if (rd && g_file_equal(rd, d))
			return TRUE;
	}
	return FALSE;
}

static void pump(ExOpQueue *q);

static void op_finished(ExOperation *op, ExOpQueue *q)
{
	g_signal_handlers_disconnect_by_func(op, op_finished, q);
	g_object_ref(op);
	g_ptr_array_remove(q->running, op);
	g_ptr_array_remove(q->waiting, op); /* cancelled while queued */
	/* What changed below home makes the search index stale. */
	ex_index_mark_dirty(ex_index_get_default());
	g_signal_emit(q, sigs[SIG_FINISHED], 0, op);
	g_object_unref(op);
	pump(q);
}

static void pump(ExOpQueue *q)
{
	for (guint i = 0;
	     i < q->waiting->len && q->running->len < EX_QUEUE_PARALLEL;) {
		ExOperation *op = g_ptr_array_index(q->waiting, i);

		if (ex_operation_get_state(op) != EX_OP_QUEUED) {
			g_ptr_array_remove_index(q->waiting, i);
			continue;
		}
		if (collides(q, op)) {
			i++;
			continue;
		}
		op = g_ptr_array_steal_index(q->waiting, i);
		g_ptr_array_add(q->running, op);
		ex_operation_start(op);
		g_signal_emit(q, sigs[SIG_STARTED], 0, op);
	}
}

void ex_opqueue_add(ExOpQueue *q, ExOperation *op, gboolean undoable)
{
	g_ptr_array_add(q->waiting, g_object_ref(op));
	g_ptr_array_add(q->history, g_object_ref(op));
	while (q->history->len > EX_HISTORY_KEEP) {
		ExOperation *old = g_ptr_array_index(q->history, 0);
		ExOpState st = ex_operation_get_state(old);

		if (st == EX_OP_QUEUED || st == EX_OP_RUNNING)
			break;
		g_hash_table_remove(q->undoable, old);
		g_ptr_array_remove_index(q->history, 0);
	}
	if (undoable)
		g_hash_table_add(q->undoable, op);
	g_signal_connect(op, "finished", G_CALLBACK(op_finished), q);
	g_signal_emit(q, sigs[SIG_ADDED], 0, op);
	pump(q);
}

GPtrArray *ex_opqueue_active(ExOpQueue *q)
{
	GPtrArray *a = g_ptr_array_new();

	for (guint i = 0; i < q->running->len; i++)
		g_ptr_array_add(a, g_ptr_array_index(q->running, i));
	for (guint i = 0; i < q->waiting->len; i++)
		g_ptr_array_add(a, g_ptr_array_index(q->waiting, i));
	return a;
}

GPtrArray *ex_opqueue_history(ExOpQueue *q)
{
	GPtrArray *a = g_ptr_array_new();

	for (guint i = q->history->len; i > 0; i--)
		g_ptr_array_add(a, g_ptr_array_index(q->history, i - 1));
	return a;
}

void ex_opqueue_clear_finished(ExOpQueue *q)
{
	for (guint i = 0; i < q->history->len;) {
		ExOperation *op = g_ptr_array_index(q->history, i);
		ExOpState st = ex_operation_get_state(op);

		if (st != EX_OP_QUEUED && st != EX_OP_RUNNING) {
			g_hash_table_remove(q->undoable, op);
			g_ptr_array_remove_index(q->history, i);
		} else {
			i++;
		}
	}
}

gboolean ex_opqueue_is_undoable(ExOpQueue *q, ExOperation *op)
{
	return g_hash_table_contains(q->undoable, op);
}
