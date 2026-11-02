// LikeOS Explorer: where a tab is and where it has been.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-navigator.h"

/* History kept behind a tab. */
#define EX_HISTORY_MAX 100

struct _ExNavigator {
	GObject parent;
	ExLocation *cur;
	GPtrArray *back; /* nearest last */
	GPtrArray *fwd; /* nearest last */
};

G_DEFINE_TYPE(ExNavigator, ex_navigator, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_navigator_finalize(GObject *o)
{
	ExNavigator *n = EX_NAVIGATOR(o);

	ex_location_unref(n->cur);
	g_ptr_array_unref(n->back);
	g_ptr_array_unref(n->fwd);
	G_OBJECT_CLASS(ex_navigator_parent_class)->finalize(o);
}

static void ex_navigator_class_init(ExNavigatorClass *k)
{
	G_OBJECT_CLASS(k)->finalize = ex_navigator_finalize;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_navigator_init(ExNavigator *n)
{
	n->back = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ex_location_unref);
	n->fwd = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ex_location_unref);
}

ExNavigator *ex_navigator_new(ExLocation *start)
{
	ExNavigator *n = g_object_new(EX_TYPE_NAVIGATOR, NULL);

	n->cur = ex_location_ref(start);
	return n;
}

ExLocation *ex_navigator_current(ExNavigator *n)
{
	return n->cur;
}

void ex_navigator_go(ExNavigator *n, ExLocation *loc)
{
	if (ex_location_equal(loc, n->cur))
		return;
	g_ptr_array_add(n->back, n->cur);
	if (n->back->len > EX_HISTORY_MAX)
		g_ptr_array_remove_index(n->back, 0);
	n->cur = ex_location_ref(loc);
	g_ptr_array_set_size(n->fwd, 0);
	g_signal_emit(n, sigs[SIG_CHANGED], 0);
}

void ex_navigator_replace(ExNavigator *n, ExLocation *loc)
{
	ExLocation *old = n->cur;

	n->cur = ex_location_ref(loc);
	ex_location_unref(old);
	g_signal_emit(n, sigs[SIG_CHANGED], 0);
}

gboolean ex_navigator_can_back(ExNavigator *n)
{
	return n->back->len > 0;
}

gboolean ex_navigator_can_forward(ExNavigator *n)
{
	return n->fwd->len > 0;
}

gboolean ex_navigator_can_up(ExNavigator *n)
{
	ExLocation *p = ex_location_parent(n->cur);
	gboolean ok = p != NULL;

	if (p)
		ex_location_unref(p);
	return ok;
}

void ex_navigator_jump(ExNavigator *n, int steps)
{
	gboolean moved = FALSE;

	while (steps < 0 && n->back->len) {
		g_ptr_array_add(n->fwd, n->cur);
		n->cur = g_ptr_array_steal_index(n->back, n->back->len - 1);
		steps++;
		moved = TRUE;
	}
	while (steps > 0 && n->fwd->len) {
		g_ptr_array_add(n->back, n->cur);
		n->cur = g_ptr_array_steal_index(n->fwd, n->fwd->len - 1);
		steps--;
		moved = TRUE;
	}
	if (moved)
		g_signal_emit(n, sigs[SIG_CHANGED], 0);
}

void ex_navigator_back(ExNavigator *n)
{
	ex_navigator_jump(n, -1);
}

void ex_navigator_forward(ExNavigator *n)
{
	ex_navigator_jump(n, 1);
}

void ex_navigator_up(ExNavigator *n)
{
	ExLocation *p = ex_location_parent(n->cur);

	if (p) {
		ex_navigator_go(n, p);
		ex_location_unref(p);
	}
}

static GPtrArray *reversed(GPtrArray *a)
{
	GPtrArray *out = g_ptr_array_new();

	for (guint i = a->len; i > 0; i--)
		g_ptr_array_add(out, g_ptr_array_index(a, i - 1));
	return out;
}

GPtrArray *ex_navigator_back_list(ExNavigator *n)
{
	return reversed(n->back);
}

GPtrArray *ex_navigator_forward_list(ExNavigator *n)
{
	return reversed(n->fwd);
}
