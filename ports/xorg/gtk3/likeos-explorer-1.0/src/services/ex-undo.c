// LikeOS -- File Explorer: undo and redo.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-undo.h"
#include "services/ex-opqueue.h"

#define EX_UNDO_MAX 50

typedef struct {
	ExOpKind kind;
	GPtrArray *src; /* GFile, may hold NULL */
	GPtrArray *dst;
	char *what; /* "Copy", "Move", ... */
} Record;

struct _ExUndo {
	GObject parent;
	GPtrArray *undo; /* Record, newest last */
	GPtrArray *redo;
	/* Operations started by undo/redo, and which stack their record goes
	 * to when they finish. */
	GHashTable *pending; /* ExOperation* -> GINT(1 = redo stack, 2 = undo) */
};

G_DEFINE_TYPE(ExUndo, ex_undo, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void record_free(Record *r)
{
	g_ptr_array_unref(r->src);
	g_ptr_array_unref(r->dst);
	g_free(r->what);
	g_free(r);
}

static void ex_undo_class_init(ExUndoClass *k)
{
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_undo_init(ExUndo *u)
{
	u->undo = g_ptr_array_new_with_free_func((GDestroyNotify)record_free);
	u->redo = g_ptr_array_new_with_free_func((GDestroyNotify)record_free);
	u->pending = g_hash_table_new(NULL, NULL);
}

static const char *kind_word(ExOpKind k)
{
	switch (k) {
	case EX_OP_COPY:
		return "Copy";
	case EX_OP_MOVE:
		return "Move";
	case EX_OP_LINK:
		return "Create shortcut";
	case EX_OP_TRASH:
		return "Delete";
	case EX_OP_RENAME:
		return "Rename";
	case EX_OP_MKDIR:
		return "New folder";
	case EX_OP_NEWFILE:
		return "New item";
	case EX_OP_RESTORE:
		return "Restore";
	default:
		return NULL;
	}
}

static Record *record_of(ExOperation *op, const char *what)
{
	ExOpKind k = ex_operation_get_kind(op);
	GPtrArray *res = ex_operation_get_results(op);
	Record *r;

	if (!kind_word(k) || res->len == 0)
		return NULL;
	r = g_new0(Record, 1);
	r->kind = k;
	r->src = g_ptr_array_new_with_free_func(g_object_unref);
	r->dst = g_ptr_array_new_with_free_func(g_object_unref);
	r->what = g_strdup(what ? what : kind_word(k));
	for (guint i = 0; i < res->len; i++) {
		ExOpResult *x = g_ptr_array_index(res, i);

		/* A trash that ended as a permanent delete cannot come back. */
		if (k == EX_OP_TRASH && !x->dest)
			continue;
		g_ptr_array_add(r->src, x->source ? g_object_ref(x->source) :
						    NULL);
		g_ptr_array_add(r->dst, x->dest ? g_object_ref(x->dest) : NULL);
	}
	if (r->dst->len == 0) {
		record_free(r);
		return NULL;
	}
	return r;
}

/* The operation that takes `r' back. */
static ExOperation *inverse(Record *r)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);
	GPtrArray *b = g_ptr_array_new_with_free_func(g_object_unref);
	ExOperation *op = NULL;

	switch (r->kind) {
	case EX_OP_COPY:
	case EX_OP_LINK:
	case EX_OP_MKDIR:
	case EX_OP_NEWFILE:
	case EX_OP_RESTORE:
		for (guint i = 0; i < r->dst->len; i++)
			if (g_ptr_array_index(r->dst, i))
				g_ptr_array_add(a, g_object_ref(
							   g_ptr_array_index(r->dst, i)));
		if (a->len)
			op = ex_operation_new_trash(a);
		break;
	case EX_OP_MOVE:
	case EX_OP_RENAME:
		for (guint i = 0; i < r->dst->len; i++) {
			GFile *d = g_ptr_array_index(r->dst, i);
			GFile *s = g_ptr_array_index(r->src, i);

			if (d && s) {
				g_ptr_array_add(a, g_object_ref(d));
				g_ptr_array_add(b, g_object_ref(s));
			}
		}
		if (a->len)
			op = ex_operation_new_move_exact(a, b);
		break;
	case EX_OP_TRASH:
		for (guint i = 0; i < r->dst->len; i++)
			if (g_ptr_array_index(r->dst, i))
				g_ptr_array_add(a, g_object_ref(
							   g_ptr_array_index(r->dst, i)));
		if (a->len)
			op = ex_operation_new_restore(a);
		break;
	default:
		break;
	}
	g_ptr_array_unref(a);
	g_ptr_array_unref(b);
	return op;
}

static void trim(GPtrArray *stack)
{
	while (stack->len > EX_UNDO_MAX)
		g_ptr_array_remove_index(stack, 0);
}

static void finished(ExOpQueue *q, ExOperation *op, ExUndo *u)
{
	gpointer where = NULL;
	ExOpState st = ex_operation_get_state(op);
	gboolean from_undo = g_hash_table_lookup_extended(u->pending, op, NULL,
							  &where);

	if (from_undo)
		g_hash_table_remove(u->pending, op);
	else if (!ex_opqueue_is_undoable(q, op))
		return;
	if (st == EX_OP_QUEUED || st == EX_OP_RUNNING)
		return;

	Record *r;
	if (from_undo) {
		/* The inverse's record keeps the original's name ("Redo
		 * Copy" after undoing a copy). */
		char *what = g_object_get_data(G_OBJECT(op), "ex-undo-what");

		r = record_of(op, what);
		if (r) {
			g_ptr_array_add(GPOINTER_TO_INT(where) == 1 ? u->redo :
								      u->undo,
					r);
			trim(GPOINTER_TO_INT(where) == 1 ? u->redo : u->undo);
		}
	} else {
		r = record_of(op, NULL);
		if (r) {
			g_ptr_array_add(u->undo, r);
			trim(u->undo);
			/* Something new was done: what was undone stays
			 * undone. */
			g_ptr_array_set_size(u->redo, 0);
		}
	}
	g_signal_emit(u, sigs[SIG_CHANGED], 0);
}

ExUndo *ex_undo_get(void)
{
	static ExUndo *the;

	if (!the) {
		the = g_object_new(EX_TYPE_UNDO, NULL);
		g_signal_connect(ex_opqueue_get(), "finished",
				 G_CALLBACK(finished), the);
	}
	return the;
}

gboolean ex_undo_can_undo(ExUndo *u)
{
	return u->undo->len > 0;
}

gboolean ex_undo_can_redo(ExUndo *u)
{
	return u->redo->len > 0;
}

static char *label(GPtrArray *stack, const char *verb)
{
	if (!stack->len)
		return g_strdup(verb);
	Record *r = g_ptr_array_index(stack, stack->len - 1);

	return g_strdup_printf("%s %s", verb, r->what);
}

char *ex_undo_undo_label(ExUndo *u)
{
	return label(u->undo, "Undo");
}

char *ex_undo_redo_label(ExUndo *u)
{
	return label(u->redo, "Redo");
}

static ExOperation *run_top(ExUndo *u, GPtrArray *stack, int dest_stack)
{
	if (!stack->len)
		return NULL;
	Record *r = g_ptr_array_steal_index(stack, stack->len - 1);
	ExOperation *op = inverse(r);

	if (op) {
		g_object_set_data_full(G_OBJECT(op), "ex-undo-what",
				       g_strdup(r->what), g_free);
		g_hash_table_insert(u->pending, op,
				    GINT_TO_POINTER(dest_stack));
		ex_opqueue_add(ex_opqueue_get(), op, FALSE);
		g_object_unref(op);
	}
	record_free(r);
	g_signal_emit(u, sigs[SIG_CHANGED], 0);
	return op;
}

ExOperation *ex_undo_undo(ExUndo *u)
{
	return run_top(u, u->undo, 1);
}

ExOperation *ex_undo_redo(ExUndo *u)
{
	return run_top(u, u->redo, 2);
}
