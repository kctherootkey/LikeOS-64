// LikeOS Explorer: what is selected.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-selection.h"

struct _ExSelection {
	GObject parent;
	GPtrArray *items;
};

G_DEFINE_TYPE(ExSelection, ex_selection, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_selection_finalize(GObject *o)
{
	g_ptr_array_unref(EX_SELECTION(o)->items);
	G_OBJECT_CLASS(ex_selection_parent_class)->finalize(o);
}

static void ex_selection_class_init(ExSelectionClass *k)
{
	G_OBJECT_CLASS(k)->finalize = ex_selection_finalize;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_selection_init(ExSelection *s)
{
	s->items = g_ptr_array_new_with_free_func(g_object_unref);
}

ExSelection *ex_selection_new(void)
{
	return g_object_new(EX_TYPE_SELECTION, NULL);
}

static gboolean same(GPtrArray *a, GPtrArray *b)
{
	if (a->len != b->len)
		return FALSE;
	for (guint i = 0; i < a->len; i++)
		if (g_ptr_array_index(a, i) != g_ptr_array_index(b, i))
			return FALSE;
	return TRUE;
}

void ex_selection_set(ExSelection *s, GPtrArray *files)
{
	if (files && same(s->items, files))
		return;
	g_ptr_array_set_size(s->items, 0);
	for (guint i = 0; files && i < files->len; i++)
		g_ptr_array_add(s->items,
				g_object_ref(g_ptr_array_index(files, i)));
	g_signal_emit(s, sigs[SIG_CHANGED], 0);
}

void ex_selection_clear(ExSelection *s)
{
	if (s->items->len == 0)
		return;
	g_ptr_array_set_size(s->items, 0);
	g_signal_emit(s, sigs[SIG_CHANGED], 0);
}

GPtrArray *ex_selection_get(ExSelection *s)
{
	return s->items;
}

guint ex_selection_count(ExSelection *s)
{
	return s->items->len;
}

ExFile *ex_selection_single(ExSelection *s)
{
	return s->items->len == 1 ? g_ptr_array_index(s->items, 0) : NULL;
}

ExFileCaps ex_selection_caps(ExSelection *s)
{
	ExFileCaps c = ~(ExFileCaps)0;

	if (s->items->len == 0)
		return 0;
	for (guint i = 0; i < s->items->len; i++)
		c &= ex_file_get_caps(g_ptr_array_index(s->items, i));
	return c;
}

gboolean ex_selection_all_dirs(ExSelection *s)
{
	if (s->items->len == 0)
		return FALSE;
	for (guint i = 0; i < s->items->len; i++)
		if (!ex_file_is_dir(g_ptr_array_index(s->items, i)))
			return FALSE;
	return TRUE;
}

guint64 ex_selection_total_size(ExSelection *s)
{
	guint64 t = 0;

	for (guint i = 0; i < s->items->len; i++) {
		ExFile *f = g_ptr_array_index(s->items, i);

		if (!ex_file_is_dir(f))
			t += ex_file_get_size(f);
	}
	return t;
}

GPtrArray *ex_selection_files(ExSelection *s)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);

	for (guint i = 0; i < s->items->len; i++)
		g_ptr_array_add(a, g_object_ref(ex_file_get_file(
					   g_ptr_array_index(s->items, i))));
	return a;
}
