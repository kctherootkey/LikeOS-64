// LikeOS Task Viewer -- the grouped process tree (see tv-tree.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-app.h"
#include "tv-tree.h"

enum { SORT_NAME, SORT_STATUS, SORT_CPU, SORT_MEM };

/* ---- nodes ------------------------------------------------------------------- */

static void node_free(gpointer data)
{
	TvNode *n = data;

	g_free(n->key);
	g_free(n->name);
	g_clear_object(&n->icon);
	if (n->kids)
		g_ptr_array_unref(n->kids);
	g_free(n);
}

GPtrArray *tv_nodes_new(void)
{
	return g_ptr_array_new_with_free_func(node_free);
}

TvNode *tv_node_new(TvRowKind kind, char *key, const char *name, GIcon *icon)
{
	TvNode *n = g_new0(TvNode, 1);

	n->kind = kind;
	n->key = key;
	n->name = g_strdup(name ? name : "");
	n->icon = icon ? g_object_ref(icon) : NULL;
	n->status = "";
	return n;
}

void tv_node_add(TvNode *parent, TvNode *kid)
{
	if (!parent->kids)
		parent->kids = tv_nodes_new();
	g_ptr_array_add(parent->kids, kid);
}

/* ---- syncing ------------------------------------------------------------------ */

static void set_row(TvTree *t, GtkTreeIter *it, const TvNode *n, gboolean fresh)
{
	if (!fresh) {
		char *name = NULL, *status = NULL;
		GIcon *icon = NULL;
		double cpu;
		guint64 mem;
		gboolean same;

		gtk_tree_model_get(GTK_TREE_MODEL(t->store), it, TV_COL_NAME,
				   &name, TV_COL_STATUS, &status, TV_COL_ICON,
				   &icon, TV_COL_CPU, &cpu, TV_COL_MEM, &mem, -1);
		same = !g_strcmp0(name, n->name) &&
		       !g_strcmp0(status, n->status) && cpu == n->cpu &&
		       mem == n->mem &&
		       (icon == n->icon ||
			(icon && n->icon && g_icon_equal(icon, n->icon)));
		g_free(name);
		g_free(status);
		g_clear_object(&icon);
		/* An unchanged row is left alone: every set is a re-sort and
		 * a redraw. */
		if (same)
			return;
	}
	{
		char *fold = g_utf8_casefold(n->name, -1);
		char *sort = g_utf8_collate_key(fold, -1);

		gtk_tree_store_set(t->store, it, TV_COL_KEY, n->key, TV_COL_KIND,
				   n->kind, TV_COL_NAME, n->name, TV_COL_ICON,
				   n->icon, TV_COL_STATUS, n->status,
				   TV_COL_CPU, n->cpu, TV_COL_MEM, n->mem,
				   TV_COL_PID, n->pid, TV_COL_XID, n->xid,
				   TV_COL_ORDER, n->order, TV_COL_SORT, sort,
				   -1);
		g_free(sort);
		g_free(fold);
	}
}

static void sync_level(TvTree *t, GtkTreeIter *parent, GPtrArray *nodes,
		       GPtrArray *expand)
{
	GHashTable *have = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, g_free);
	GtkTreeModel *m = GTK_TREE_MODEL(t->store);
	GtkTreeIter it;
	GHashTableIter hi;
	gpointer v;

	if (gtk_tree_model_iter_children(m, &it, parent)) {
		do {
			char *key = NULL;

			gtk_tree_model_get(m, &it, TV_COL_KEY, &key, -1);
			if (key && !g_hash_table_contains(have, key))
				g_hash_table_insert(have, key,
						    g_memdup2(&it, sizeof(it)));
			else
				g_free(key);
		} while (gtk_tree_model_iter_next(m, &it));
	}

	for (guint i = 0; nodes && i < nodes->len; i++) {
		TvNode *n = nodes->pdata[i];
		GtkTreeIter *old = g_hash_table_lookup(have, n->key);
		GtkTreeIter row;

		if (old) {
			row = *old;
			g_hash_table_remove(have, n->key);
			set_row(t, &row, n, FALSE);
		} else {
			gtk_tree_store_append(t->store, &row, parent);
			set_row(t, &row, n, TRUE);
			if (n->expand && n->kids && n->kids->len)
				g_ptr_array_add(expand,
						gtk_tree_model_get_path(m, &row));
		}
		sync_level(t, &row, n->kids, expand);
	}

	g_hash_table_iter_init(&hi, have);
	while (g_hash_table_iter_next(&hi, NULL, &v))
		gtk_tree_store_remove(t->store, v);
	g_hash_table_destroy(have);
}

void tv_tree_sync(TvTree *t, GPtrArray *top)
{
	GPtrArray *expand =
		g_ptr_array_new_with_free_func((GDestroyNotify)gtk_tree_path_free);

	sync_level(t, NULL, top, expand);
	for (guint i = 0; i < expand->len; i++) {
		GtkTreePath *p = gtk_tree_model_sort_convert_child_path_to_path(
			GTK_TREE_MODEL_SORT(t->sorted), expand->pdata[i]);

		if (p) {
			gtk_tree_view_expand_row(GTK_TREE_VIEW(t->view), p, FALSE);
			gtk_tree_path_free(p);
		}
	}
	g_ptr_array_unref(expand);
}

void tv_tree_clear(TvTree *t)
{
	gtk_tree_store_clear(t->store);
}

void tv_tree_expand_groups(TvTree *t)
{
	GtkTreeModel *m = t->sorted;
	GtkTreeIter it;

	if (!gtk_tree_model_get_iter_first(m, &it))
		return;
	do {
		int kind;

		gtk_tree_model_get(m, &it, TV_COL_KIND, &kind, -1);
		if (kind == TV_ROW_GROUP) {
			GtkTreePath *p = gtk_tree_model_get_path(m, &it);

			gtk_tree_view_expand_row(GTK_TREE_VIEW(t->view), p,
						 FALSE);
			gtk_tree_path_free(p);
		}
	} while (gtk_tree_model_iter_next(m, &it));
}

/* ---- totals ------------------------------------------------------------------- */

void tv_tree_set_totals(TvTree *t, const TvSnapshot *s)
{
	char buf[32];

	g_snprintf(buf, sizeof(buf), "%.0f%%", s->cpu_total);
	gtk_label_set_text(GTK_LABEL(t->cpu_total), buf);
	g_snprintf(buf, sizeof(buf), "%.0f%%",
		   s->mem_total ? (double)s->mem_used * 100.0 /
					  (double)s->mem_total :
				  0.0);
	gtk_label_set_text(GTK_LABEL(t->mem_total), buf);
}

/* ---- sorting ------------------------------------------------------------------ */

static int compare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
		   gpointer data)
{
	TvTree *t = g_object_get_data(G_OBJECT(m), "tv-tree");
	int col = GPOINTER_TO_INT(data);
	int ka, kb, oa, ob, pa, pb, sort_col, r = 0;
	double ca, cb;
	guint64 ma, mb;
	char *sa = NULL, *sb = NULL, *ta = NULL, *tb = NULL;
	GtkSortType order = GTK_SORT_ASCENDING;
	int dir;

	if (t && t->sorted)
		gtk_tree_sortable_get_sort_column_id(
			GTK_TREE_SORTABLE(t->sorted), &sort_col, &order);
	/* The view reverses whatever this returns for a descending sort;
	 * what must not turn round is pre-multiplied by `dir'. */
	dir = order == GTK_SORT_DESCENDING ? -1 : 1;

	gtk_tree_model_get(m, a, TV_COL_KIND, &ka, TV_COL_ORDER, &oa,
			   TV_COL_CPU, &ca, TV_COL_MEM, &ma, TV_COL_PID, &pa,
			   TV_COL_SORT, &sa, TV_COL_STATUS, &ta, -1);
	gtk_tree_model_get(m, b, TV_COL_KIND, &kb, TV_COL_ORDER, &ob,
			   TV_COL_CPU, &cb, TV_COL_MEM, &mb, TV_COL_PID, &pb,
			   TV_COL_SORT, &sb, TV_COL_STATUS, &tb, -1);

	if (ka == TV_ROW_GROUP || kb == TV_ROW_GROUP) {
		r = dir * (oa < ob ? -1 : oa > ob);
		goto out;
	}
	if ((ka == TV_ROW_WIN) != (kb == TV_ROW_WIN)) {
		r = dir * (ka == TV_ROW_WIN ? -1 : 1);
		goto out;
	}
	switch (col) {
	case SORT_STATUS:
		r = g_strcmp0(ta, tb);
		break;
	case SORT_CPU:
		r = ca < cb ? -1 : ca > cb;
		break;
	case SORT_MEM:
		r = ma < mb ? -1 : ma > mb;
		break;
	default:
		break;
	}
	if (!r)
		r = g_strcmp0(sa, sb);
	if (!r)
		r = pa < pb ? -1 : pa > pb;
out:
	g_free(sa);
	g_free(sb);
	g_free(ta);
	g_free(tb);
	return r;
}

static void on_sort_changed(GtkTreeSortable *s, gpointer data)
{
	TvTree *t = data;
	int col;
	GtkSortType order;
	char *k;

	if (!gtk_tree_sortable_get_sort_column_id(s, &col, &order))
		return;
	k = g_strconcat(t->sort_setting, "-sort", NULL);
	tv_settings_set_int(k, col * 2 + (order == GTK_SORT_DESCENDING));
	g_free(k);
}

/* ---- cells -------------------------------------------------------------------- */

static void name_icon_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
			   GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	int kind;
	GIcon *icon = NULL;

	gtk_tree_model_get(m, it, TV_COL_KIND, &kind, TV_COL_ICON, &icon, -1);
	g_object_set(r, "visible", kind != TV_ROW_GROUP && icon != NULL,
		     "gicon", icon, NULL);
	g_clear_object(&icon);
}

static void name_text_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
			   GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	int kind;
	char *name = NULL;

	gtk_tree_model_get(m, it, TV_COL_KIND, &kind, TV_COL_NAME, &name, -1);
	g_object_set(r, "text", name, "weight",
		     kind == TV_ROW_GROUP ? PANGO_WEIGHT_BOLD :
					    PANGO_WEIGHT_NORMAL,
		     NULL);
	g_free(name);
}

static void cpu_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
		     GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	TvTree *t = data;
	double cpu;
	char *s;

	gtk_tree_model_get(m, it, TV_COL_CPU, &cpu, -1);
	if (cpu < 0) {
		g_object_set(r, "text", "", NULL);
		tv_cell_heat(r, t->view, 0);
		return;
	}
	s = tv_format_percent(cpu);
	g_object_set(r, "text", s, NULL);
	g_free(s);
	tv_cell_heat(r, t->view, cpu / 100.0);
}

static void mem_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
		     GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	TvTree *t = data;
	const TvSnapshot *s = tv_procs_snapshot();
	double cpu;
	guint64 mem;
	char *txt;

	gtk_tree_model_get(m, it, TV_COL_CPU, &cpu, TV_COL_MEM, &mem, -1);
	if (cpu < 0) {
		g_object_set(r, "text", "", NULL);
		tv_cell_heat(r, t->view, 0);
		return;
	}
	txt = tv_format_mem_cell(mem, s->mem_total, tv_mem_percents());
	g_object_set(r, "text", txt, NULL);
	g_free(txt);
	tv_cell_heat(r, t->view, tv_mem_heat_fraction(mem, s->mem_total));
}

static GtkTreeViewColumn *add_column(TvTree *t, GtkWidget *header, int sort,
				     GtkCellRenderer *r,
				     GtkTreeCellDataFunc func, int width)
{
	GtkTreeViewColumn *c = gtk_tree_view_column_new();

	gtk_tree_view_column_set_widget(c, header);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_set_cell_data_func(c, r, func, t, NULL);
	gtk_tree_view_column_set_sort_column_id(c, sort);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_fixed_width(c, width);
	gtk_tree_view_column_set_alignment(c, 1.0f);
	gtk_tree_view_append_column(GTK_TREE_VIEW(t->view), c);
	return c;
}

TvTree *tv_tree_new(const char *setting, const char *name_title,
		    gboolean status_column)
{
	TvTree *t = g_new0(TvTree, 1);
	GtkTreeViewColumn *c;
	GtkCellRenderer *r;
	GtkTreeSortable *st;
	char *k;
	int saved;

	t->sort_setting = g_strdup(setting);
	t->store = gtk_tree_store_new(TV_N_COLS, G_TYPE_STRING, G_TYPE_INT,
				      G_TYPE_STRING, G_TYPE_ICON, G_TYPE_STRING,
				      G_TYPE_DOUBLE, G_TYPE_UINT64, G_TYPE_INT,
				      G_TYPE_ULONG, G_TYPE_INT, G_TYPE_STRING);
	g_object_set_data(G_OBJECT(t->store), "tv-tree", t);
	t->sorted = gtk_tree_model_sort_new_with_model(GTK_TREE_MODEL(t->store));
	st = GTK_TREE_SORTABLE(t->sorted);
	for (int i = SORT_NAME; i <= SORT_MEM; i++)
		gtk_tree_sortable_set_sort_func(st, i, compare,
						GINT_TO_POINTER(i), NULL);

	t->view = gtk_tree_view_new_with_model(t->sorted);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(t->view), FALSE);
	gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(t->view), FALSE);
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(t->view)),
		GTK_SELECTION_BROWSE);

	/* Name: the icon and the name, the expander in front. */
	c = gtk_tree_view_column_new();
	gtk_tree_view_column_set_widget(c, tv_total_header(name_title, FALSE,
							   NULL));
	r = gtk_cell_renderer_pixbuf_new();
	g_object_set(r, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 2, NULL);
	gtk_tree_view_column_pack_start(c, r, FALSE);
	gtk_tree_view_column_set_cell_data_func(c, r, name_icon_cell, t, NULL);
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 4, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_set_cell_data_func(c, r, name_text_cell, t, NULL);
	gtk_tree_view_column_set_sort_column_id(c, SORT_NAME);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_expand(c, TRUE);
	gtk_tree_view_column_set_min_width(c, 220);
	gtk_tree_view_append_column(GTK_TREE_VIEW(t->view), c);
	gtk_tree_view_set_expander_column(GTK_TREE_VIEW(t->view), c);

	if (status_column) {
		r = gtk_cell_renderer_text_new();
		g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
		c = gtk_tree_view_column_new_with_attributes(
			NULL, r, "text", TV_COL_STATUS, NULL);
		gtk_tree_view_column_set_widget(
			c, tv_total_header("Status", FALSE, NULL));
		gtk_tree_view_column_set_sort_column_id(c, SORT_STATUS);
		gtk_tree_view_column_set_resizable(c, TRUE);
		gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(c, 110);
		gtk_tree_view_append_column(GTK_TREE_VIEW(t->view), c);
	}

	r = gtk_cell_renderer_text_new();
	g_object_set(r, "xalign", 1.0f, "xpad", 8, NULL);
	add_column(t, tv_total_header("CPU", TRUE, &t->cpu_total), SORT_CPU, r,
		   cpu_cell, 90);
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "xalign", 1.0f, "xpad", 8, NULL);
	add_column(t, tv_total_header("Memory", TRUE, &t->mem_total), SORT_MEM,
		   r, mem_cell, 120);

	k = g_strconcat(setting, "-sort", NULL);
	saved = tv_settings_get_int(k, SORT_NAME * 2);
	g_free(k);
	if (saved < 0 || saved / 2 > SORT_MEM)
		saved = 0;
	gtk_tree_sortable_set_sort_column_id(st, saved / 2,
					     saved % 2 ? GTK_SORT_DESCENDING :
							 GTK_SORT_ASCENDING);
	g_signal_connect(st, "sort-column-changed",
			 G_CALLBACK(on_sort_changed), t);

	t->scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(t->scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(t->scroll), t->view);
	gtk_widget_show_all(t->scroll);
	return t;
}

/* ---- selection ---------------------------------------------------------------- */

gboolean tv_tree_selected(TvTree *t, TvRowKind *kind, int *pid, gulong *xid)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(t->view));
	GtkTreeModel *m;
	GtkTreeIter it;
	int k, p;
	gulong x;

	if (!gtk_tree_selection_get_selected(sel, &m, &it))
		return FALSE;
	gtk_tree_model_get(m, &it, TV_COL_KIND, &k, TV_COL_PID, &p, TV_COL_XID,
			   &x, -1);
	if (kind)
		*kind = k;
	if (pid)
		*pid = p;
	if (xid)
		*xid = x;
	return TRUE;
}

gboolean tv_tree_select_at(TvTree *t, double x, double y)
{
	GtkTreePath *p = NULL;

	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(t->view), (int)x,
					   (int)y, &p, NULL, NULL, NULL))
		return FALSE;
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(t->view), p, NULL, FALSE);
	gtk_tree_path_free(p);
	return TRUE;
}

GtkTreePath *tv_tree_selected_path(TvTree *t)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(t->view));
	GtkTreeModel *m;
	GtkTreeIter it;

	if (!gtk_tree_selection_get_selected(sel, &m, &it))
		return NULL;
	return gtk_tree_model_get_path(m, &it);
}

gboolean tv_tree_selected_has_kids(TvTree *t)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(t->view));
	GtkTreeModel *m;
	GtkTreeIter it;

	return gtk_tree_selection_get_selected(sel, &m, &it) &&
	       gtk_tree_model_iter_has_child(m, &it);
}
