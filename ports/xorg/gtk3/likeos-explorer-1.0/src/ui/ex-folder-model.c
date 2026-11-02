// LikeOS -- File Explorer: the items of one folder, as GTK sees them.
//
// The shown rows are a GSequence kept in sort order; a GtkTreeIter carries
// the GSequenceIter, which stays valid as rows come and go around it, so the
// model can promise persistent iterators.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-folder-model.h"

#include <string.h>

typedef struct {
	ExFile *file;
	GSequenceIter *iter; /* NULL while filtered out */
	gulong changed_id;
	gint oldpos; /* scratch for re-sorting */
} Entry;

struct _ExFolderModel {
	GObject parent;
	gint stamp;
	GSequence *rows; /* Entry*, shown, sorted */
	GHashTable *all; /* GFile -> Entry */
	ExSortColumn col;
	gboolean asc;
	gboolean show_hidden;
};

static void tree_model_init(GtkTreeModelIface *iface);
static void drag_source_init(GtkTreeDragSourceIface *iface);

G_DEFINE_TYPE_WITH_CODE(ExFolderModel, ex_folder_model, G_TYPE_OBJECT,
			G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL,
					      tree_model_init)
			G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_DRAG_SOURCE,
					      drag_source_init))

enum { SIG_COUNT, SIG_REPLACED, N_SIGS };
static guint sigs[N_SIGS];

static void entry_free(Entry *e)
{
	if (e->changed_id)
		g_signal_handler_disconnect(e->file, e->changed_id);
	g_object_unref(e->file);
	g_free(e);
}

static void ex_folder_model_finalize(GObject *o)
{
	ExFolderModel *m = EX_FOLDER_MODEL(o);

	g_sequence_free(m->rows);
	g_hash_table_unref(m->all);
	G_OBJECT_CLASS(ex_folder_model_parent_class)->finalize(o);
}

static void ex_folder_model_class_init(ExFolderModelClass *k)
{
	G_OBJECT_CLASS(k)->finalize = ex_folder_model_finalize;
	sigs[SIG_COUNT] = g_signal_new("count-changed", G_TYPE_FROM_CLASS(k),
				       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
				       G_TYPE_NONE, 0);
	sigs[SIG_REPLACED] = g_signal_new("file-replaced", G_TYPE_FROM_CLASS(k),
					  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
					  G_TYPE_NONE, 2, EX_TYPE_FILE,
					  EX_TYPE_FILE);
}

static void ex_folder_model_init(ExFolderModel *m)
{
	m->stamp = g_random_int();
	m->rows = g_sequence_new(NULL);
	m->all = g_hash_table_new_full(g_file_hash, (GEqualFunc)g_file_equal,
				       NULL, (GDestroyNotify)entry_free);
	m->col = EX_SORT_NAME;
	m->asc = TRUE;
}

ExFolderModel *ex_folder_model_new(void)
{
	return g_object_new(EX_TYPE_FOLDER_MODEL, NULL);
}

/* ---- ordering ------------------------------------------------------------ */

static int cmp_i64(gint64 a, gint64 b)
{
	return a < b ? -1 : a > b ? 1 : 0;
}

static int compare(gconstpointer pa, gconstpointer pb, gpointer data)
{
	const Entry *ea = pa, *eb = pb;
	ExFolderModel *m = data;
	ExFile *a = ea->file, *b = eb->file;
	gboolean da = ex_file_is_dir(a), db = ex_file_is_dir(b);
	int r = 0;

	/* Folders first, whichever way the column is sorted. */
	if (da != db)
		return da ? -1 : 1;
	switch (m->col) {
	case EX_SORT_MODIFIED:
		r = cmp_i64(ex_file_get_mtime(a), ex_file_get_mtime(b));
		break;
	case EX_SORT_SIZE:
		r = cmp_i64((gint64)ex_file_get_size(a),
			    (gint64)ex_file_get_size(b));
		break;
	case EX_SORT_TYPE: {
		char *ka = ex_file_get_kind_label(a);
		char *kb = ex_file_get_kind_label(b);

		r = g_utf8_collate(ka, kb);
		g_free(ka);
		g_free(kb);
		break;
	}
	case EX_SORT_ORIGIN:
		r = g_strcmp0(ex_file_get_trash_orig_path(a),
			      ex_file_get_trash_orig_path(b));
		break;
	case EX_SORT_DELETED:
		r = cmp_i64(ex_file_get_trash_deleted(a),
			    ex_file_get_trash_deleted(b));
		break;
	case EX_SORT_NAME:
	default:
		break;
	}
	if (r == 0)
		r = strcmp(ex_file_get_collate_key(a), ex_file_get_collate_key(b));
	return m->asc ? r : -r;
}

static gboolean shown(ExFolderModel *m, ExFile *f)
{
	return m->show_hidden || !ex_file_is_hidden(f);
}

/* ---- rows coming and going ------------------------------------------------ */

static void fill_iter(ExFolderModel *m, GtkTreeIter *it, GSequenceIter *si)
{
	it->stamp = m->stamp;
	it->user_data = si;
	it->user_data2 = NULL;
	it->user_data3 = NULL;
}

static void show_entry(ExFolderModel *m, Entry *e)
{
	GtkTreeIter it;
	GtkTreePath *p;

	e->iter = g_sequence_insert_sorted(m->rows, e, compare, m);
	fill_iter(m, &it, e->iter);
	p = gtk_tree_path_new_from_indices(g_sequence_iter_get_position(e->iter),
					   -1);
	gtk_tree_model_row_inserted(GTK_TREE_MODEL(m), p, &it);
	gtk_tree_path_free(p);
}

static void hide_entry(ExFolderModel *m, Entry *e)
{
	GtkTreePath *p = gtk_tree_path_new_from_indices(
		g_sequence_iter_get_position(e->iter), -1);

	g_sequence_remove(e->iter);
	e->iter = NULL;
	gtk_tree_model_row_deleted(GTK_TREE_MODEL(m), p);
	gtk_tree_path_free(p);
}

/* Put a shown entry where the order now wants it.  The row moves -- the
 * views are told by a reordering, not by a removal and an insertion -- so
 * it keeps its selection and the keyboard focus. */
static void move_entry(ExFolderModel *m, Entry *e)
{
	int from = g_sequence_iter_get_position(e->iter);
	int to, n = g_sequence_get_length(m->rows);
	GtkTreeIter it;
	GtkTreePath *p;

	g_sequence_sort_changed(e->iter, compare, m);
	to = g_sequence_iter_get_position(e->iter);
	if (to != from) {
		/* order[new position] = old position */
		int *order = g_new(int, n);
		GtkTreePath *root = gtk_tree_path_new();

		for (int i = 0; i < n; i++)
			order[i] = i;
		if (from < to)
			for (int i = from; i < to; i++)
				order[i] = i + 1;
		else
			for (int i = to + 1; i <= from; i++)
				order[i] = i - 1;
		order[to] = from;
		gtk_tree_model_rows_reordered(GTK_TREE_MODEL(m), root, NULL,
					      order);
		gtk_tree_path_free(root);
		g_free(order);
	}
	fill_iter(m, &it, e->iter);
	p = gtk_tree_path_new_from_indices(to, -1);
	gtk_tree_model_row_changed(GTK_TREE_MODEL(m), p, &it);
	gtk_tree_path_free(p);
}

static void file_changed(ExFile *f, ExFolderModel *m)
{
	Entry *e = g_hash_table_lookup(m->all, ex_file_get_file(f));
	gboolean want;

	if (!e)
		return;
	want = shown(m, f);
	if (e->iter && !want) {
		hide_entry(m, e);
		g_signal_emit(m, sigs[SIG_COUNT], 0);
		return;
	}
	if (!e->iter && want) {
		show_entry(m, e);
		g_signal_emit(m, sigs[SIG_COUNT], 0);
		return;
	}
	if (e->iter)
		move_entry(m, e);
}

void ex_folder_model_add_one(ExFolderModel *m, ExFile *f)
{
	Entry *e = g_hash_table_lookup(m->all, ex_file_get_file(f));

	if (e) {
		/* Known: take the new information. */
		if (e->file != f)
			ex_file_set_info(e->file, ex_file_get_info(f));
		return;
	}
	e = g_new0(Entry, 1);
	e->file = g_object_ref(f);
	e->changed_id = g_signal_connect(f, "changed", G_CALLBACK(file_changed),
					 m);
	g_hash_table_insert(m->all, ex_file_get_file(f), e);
	if (shown(m, f))
		show_entry(m, e);
}

void ex_folder_model_add(ExFolderModel *m, GPtrArray *files)
{
	guint before = (guint)g_sequence_get_length(m->rows);

	for (guint i = 0; i < files->len; i++)
		ex_folder_model_add_one(m, g_ptr_array_index(files, i));
	if ((guint)g_sequence_get_length(m->rows) != before)
		g_signal_emit(m, sigs[SIG_COUNT], 0);
}

void ex_folder_model_replace(ExFolderModel *m, GFile *from, ExFile *to)
{
	Entry *e = g_hash_table_lookup(m->all, from);
	ExFile *old;

	if (!e || g_hash_table_contains(m->all, ex_file_get_file(to))) {
		/* Not known under the old name, or the new name is already
		 * listed (it replaced something): the plain way. */
		if (e)
			ex_folder_model_remove(m, from);
		ex_folder_model_add_one(m, to);
		return;
	}
	old = e->file;
	g_signal_handler_disconnect(old, e->changed_id);
	g_hash_table_steal(m->all, from);
	e->file = g_object_ref(to);
	e->changed_id = g_signal_connect(to, "changed",
					 G_CALLBACK(file_changed), m);
	g_hash_table_insert(m->all, ex_file_get_file(to), e);
	if (e->iter && !shown(m, to)) {
		hide_entry(m, e);
		g_signal_emit(m, sigs[SIG_COUNT], 0);
	} else if (!e->iter && shown(m, to)) {
		show_entry(m, e);
		g_signal_emit(m, sigs[SIG_COUNT], 0);
	} else if (e->iter) {
		move_entry(m, e);
	}
	g_signal_emit(m, sigs[SIG_REPLACED], 0, old, to);
	g_object_unref(old);
}

void ex_folder_model_remove(ExFolderModel *m, GFile *file)
{
	Entry *e = g_hash_table_lookup(m->all, file);

	if (!e)
		return;
	if (e->iter)
		hide_entry(m, e);
	g_hash_table_remove(m->all, file);
	g_signal_emit(m, sigs[SIG_COUNT], 0);
}

void ex_folder_model_clear(ExFolderModel *m)
{
	/* From the end, so each deletion is of the last row. */
	while (g_sequence_get_length(m->rows) > 0) {
		GSequenceIter *last = g_sequence_iter_prev(
			g_sequence_get_end_iter(m->rows));
		Entry *e = g_sequence_get(last);

		hide_entry(m, e);
	}
	g_hash_table_remove_all(m->all);
	g_signal_emit(m, sigs[SIG_COUNT], 0);
}

ExFile *ex_folder_model_lookup(ExFolderModel *m, GFile *file)
{
	Entry *e = g_hash_table_lookup(m->all, file);

	return e ? e->file : NULL;
}

gboolean ex_folder_model_get_iter_for(ExFolderModel *m, GFile *file,
				      GtkTreeIter *iter)
{
	Entry *e = g_hash_table_lookup(m->all, file);

	if (!e || !e->iter)
		return FALSE;
	fill_iter(m, iter, e->iter);
	return TRUE;
}

ExFile *ex_folder_model_get_file(ExFolderModel *m, GtkTreeIter *iter)
{
	g_return_val_if_fail(iter->stamp == m->stamp, NULL);
	return ((Entry *)g_sequence_get(iter->user_data))->file;
}

/* ---- sorting and filtering ------------------------------------------------ */

static void resort(ExFolderModel *m)
{
	int n = g_sequence_get_length(m->rows);
	int pos = 0;

	if (n == 0)
		return;
	for (GSequenceIter *si = g_sequence_get_begin_iter(m->rows);
	     !g_sequence_iter_is_end(si); si = g_sequence_iter_next(si))
		((Entry *)g_sequence_get(si))->oldpos = pos++;
	g_sequence_sort(m->rows, compare, m);
	int *order = g_new(int, n);
	pos = 0;
	for (GSequenceIter *si = g_sequence_get_begin_iter(m->rows);
	     !g_sequence_iter_is_end(si); si = g_sequence_iter_next(si))
		order[pos++] = ((Entry *)g_sequence_get(si))->oldpos;
	GtkTreePath *root = gtk_tree_path_new();
	gtk_tree_model_rows_reordered(GTK_TREE_MODEL(m), root, NULL, order);
	gtk_tree_path_free(root);
	g_free(order);
}

void ex_folder_model_set_sort(ExFolderModel *m, ExSortColumn col,
			      gboolean ascending)
{
	if (m->col == col && m->asc == ascending)
		return;
	m->col = col;
	m->asc = ascending;
	resort(m);
}

void ex_folder_model_get_sort(ExFolderModel *m, ExSortColumn *col,
			      gboolean *ascending)
{
	*col = m->col;
	*ascending = m->asc;
}

void ex_folder_model_set_show_hidden(ExFolderModel *m, gboolean show)
{
	GHashTableIter it;
	gpointer k, v;

	if (m->show_hidden == show)
		return;
	m->show_hidden = show;
	g_hash_table_iter_init(&it, m->all);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Entry *e = v;
		gboolean want = shown(m, e->file);

		if (want && !e->iter)
			show_entry(m, e);
		else if (!want && e->iter)
			hide_entry(m, e);
	}
	g_signal_emit(m, sigs[SIG_COUNT], 0);
}

guint ex_folder_model_count(ExFolderModel *m)
{
	return (guint)g_sequence_get_length(m->rows);
}

guint ex_folder_model_count_all(ExFolderModel *m)
{
	return g_hash_table_size(m->all);
}

GPtrArray *ex_folder_model_list(ExFolderModel *m)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);

	for (GSequenceIter *si = g_sequence_get_begin_iter(m->rows);
	     !g_sequence_iter_is_end(si); si = g_sequence_iter_next(si))
		g_ptr_array_add(a,
				g_object_ref(((Entry *)g_sequence_get(si))->file));
	return a;
}

/* ---- GtkTreeModel ---------------------------------------------------------- */

static GtkTreeModelFlags tm_get_flags(GtkTreeModel *tm)
{
	return GTK_TREE_MODEL_LIST_ONLY | GTK_TREE_MODEL_ITERS_PERSIST;
}

static gint tm_get_n_columns(GtkTreeModel *tm)
{
	return EX_N_COLS;
}

static GType tm_get_column_type(GtkTreeModel *tm, gint col)
{
	return EX_TYPE_FILE;
}

static gboolean tm_get_iter(GtkTreeModel *tm, GtkTreeIter *it,
			    GtkTreePath *path)
{
	ExFolderModel *m = EX_FOLDER_MODEL(tm);
	int depth = gtk_tree_path_get_depth(path);
	int idx;

	if (depth != 1)
		return FALSE;
	idx = gtk_tree_path_get_indices(path)[0];
	if (idx < 0 || idx >= g_sequence_get_length(m->rows))
		return FALSE;
	fill_iter(m, it, g_sequence_get_iter_at_pos(m->rows, idx));
	return TRUE;
}

static GtkTreePath *tm_get_path(GtkTreeModel *tm, GtkTreeIter *it)
{
	return gtk_tree_path_new_from_indices(
		g_sequence_iter_get_position(it->user_data), -1);
}

static void tm_get_value(GtkTreeModel *tm, GtkTreeIter *it, gint col,
			 GValue *v)
{
	Entry *e = g_sequence_get(it->user_data);

	g_value_init(v, EX_TYPE_FILE);
	g_value_set_object(v, e->file);
}

static gboolean tm_iter_next(GtkTreeModel *tm, GtkTreeIter *it)
{
	GSequenceIter *n = g_sequence_iter_next(it->user_data);

	if (g_sequence_iter_is_end(n)) {
		it->stamp = 0;
		return FALSE;
	}
	it->user_data = n;
	return TRUE;
}

static gboolean tm_iter_previous(GtkTreeModel *tm, GtkTreeIter *it)
{
	if (g_sequence_iter_is_begin(it->user_data)) {
		it->stamp = 0;
		return FALSE;
	}
	it->user_data = g_sequence_iter_prev(it->user_data);
	return TRUE;
}

static gboolean tm_iter_children(GtkTreeModel *tm, GtkTreeIter *it,
				 GtkTreeIter *parent)
{
	ExFolderModel *m = EX_FOLDER_MODEL(tm);

	if (parent || g_sequence_get_length(m->rows) == 0)
		return FALSE;
	fill_iter(m, it, g_sequence_get_begin_iter(m->rows));
	return TRUE;
}

static gboolean tm_iter_has_child(GtkTreeModel *tm, GtkTreeIter *it)
{
	return FALSE;
}

static gint tm_iter_n_children(GtkTreeModel *tm, GtkTreeIter *it)
{
	return it ? 0 : g_sequence_get_length(EX_FOLDER_MODEL(tm)->rows);
}

static gboolean tm_iter_nth_child(GtkTreeModel *tm, GtkTreeIter *it,
				  GtkTreeIter *parent, gint n)
{
	ExFolderModel *m = EX_FOLDER_MODEL(tm);

	if (parent || n < 0 || n >= g_sequence_get_length(m->rows))
		return FALSE;
	fill_iter(m, it, g_sequence_get_iter_at_pos(m->rows, n));
	return TRUE;
}

static gboolean tm_iter_parent(GtkTreeModel *tm, GtkTreeIter *it,
			       GtkTreeIter *child)
{
	return FALSE;
}

static void tree_model_init(GtkTreeModelIface *iface)
{
	iface->get_flags = tm_get_flags;
	iface->get_n_columns = tm_get_n_columns;
	iface->get_column_type = tm_get_column_type;
	iface->get_iter = tm_get_iter;
	iface->get_path = tm_get_path;
	iface->get_value = tm_get_value;
	iface->iter_next = tm_iter_next;
	iface->iter_previous = tm_iter_previous;
	iface->iter_children = tm_iter_children;
	iface->iter_has_child = tm_iter_has_child;
	iface->iter_n_children = tm_iter_n_children;
	iface->iter_nth_child = tm_iter_nth_child;
	iface->iter_parent = tm_iter_parent;
}

/* ---- GtkTreeDragSource ------------------------------------------------------ */

/* Every row may be dragged; the views put the selected items' URIs into the
 * drag themselves, and a move is carried out by a file operation, never by
 * deleting rows. */
static gboolean ds_row_draggable(GtkTreeDragSource *ds, GtkTreePath *p)
{
	return TRUE;
}

static gboolean ds_drag_data_get(GtkTreeDragSource *ds, GtkTreePath *p,
				 GtkSelectionData *sd)
{
	return FALSE;
}

static gboolean ds_drag_data_delete(GtkTreeDragSource *ds, GtkTreePath *p)
{
	return FALSE;
}

static void drag_source_init(GtkTreeDragSourceIface *iface)
{
	iface->row_draggable = ds_row_draggable;
	iface->drag_data_get = ds_drag_data_get;
	iface->drag_data_delete = ds_drag_data_delete;
}
