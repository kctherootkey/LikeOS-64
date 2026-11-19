// LikeOS Explorer: the item views.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-file-view.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "services/ex-clipboard.h"
#include "backend/ex-util.h"

#include <math.h>
#include <string.h>

/* How long a request to select or rename something not shown yet waits for
 * it to appear. */
#define EX_PENDING_US (5 * G_USEC_PER_SEC)
/* Type-to-select: keys within this time extend the prefix. */
#define EX_TYPEAHEAD_US (900 * 1000)
/* How long the item after deleted ones waits to be selected (the deletion
 * runs in the background, and a big folder takes a while). */
#define EX_ANCHOR_US (60 * G_USEC_PER_SEC)
/* Row heights of the Details and List views, and of Compact view, in
 * pixels. */
#define EX_ROW_HEIGHT 28
#define EX_ROW_HEIGHT_COMPACT 22
/* The style class of the item views (the theme's look for them, apart
 * from the navigation pane's tree). */
#define EX_CLASS_ITEMS "ex-items"

static const GtkTargetEntry drag_targets[] = {
	{ (char *)"text/uri-list", 0, 0 },
};

struct _ExFileView {
	GtkStack parent;
	ExSelection *sel;
	ExFolderModel *model;
	char *mode;
	ExColumnSet cols;
	gboolean show_ext;
	gboolean syncing; /* selection being pushed into a view */
	int removing; /* inside a model row deletion */

	GtkWidget *tree_sw, *tree;
	GtkTreeViewColumn *c_name, *c_2, *c_3, *c_size;
	GtkCellRenderer *r_icon, *r_name;
	GtkWidget *icon_sw, *icons;
	GtkCellRenderer *ri_pix, *ri_text;
	int icon_size;

	GPtrArray *pending_select; /* GFile */
	gint64 pending_until;
	/* None of them was there yet: the first to come replaces the
	 * selection, the others join it. */
	gboolean pending_fresh;
	GFile *pending_rename;
	gint64 rename_until;

	/* Selected after the selected items are deleted: the item that takes
	 * their place. */
	GFile *anchor;
	gint64 anchor_until;

	/* A press on a selected item, held back: it either starts a drag of
	 * the whole selection or, released, selects only that item (the GTK
	 * views themselves drop the rest of the selection at the press, so
	 * several items could never be dragged). */
	GtkTreePath *held;
	int held_then; /* HELD_*: what a release without a drag does */
	double held_x, held_y; /* root coordinates */
	gboolean held_last; /* the last press was held back */

	/* The folder shown -- where a drop on no item goes -- and its name. */
	GFile *folder;
	char *folder_name;
	GdkDragAction folder_action;

	/* A drag from here, while it lasts: what its picture shows. */
	GdkDragContext *drag;
	GPtrArray *drag_files; /* GFile */
	GdkPixbuf *drag_icon;
	char *drag_hint;
	GFile *hint_dest; /* the answer below is for this destination */
	GdkDragAction hint_action;

	GString *typeahead;
	gint64 typeahead_at;
};

G_DEFINE_TYPE(ExFileView, ex_file_view, GTK_TYPE_STACK)

enum { HELD_SELECT_ONLY, HELD_KEEP, HELD_UNSELECT };

enum { SIG_ACTIVATE, SIG_POPUP, SIG_RENAME, SIG_DROP, SIG_SORT, SIG_ZOOM,
       N_SIGS };
static guint sigs[N_SIGS];

static gboolean is_tree_mode(const char *m)
{
	return !strcmp(m, "details") || !strcmp(m, "list") ||
	       !strcmp(m, "compact");
}

static gboolean tree_active(ExFileView *v)
{
	return is_tree_mode(v->mode);
}

static gboolean path_selected(ExFileView *v, GtkTreePath *p);
static void activate_selection(ExFileView *v);
static void pending_hit(ExFileView *v, GFile *gf, GtkTreePath *p);

/* ---- selection, both ways -------------------------------------------------- */

static void collect_tree(GtkTreeModel *tm, GtkTreePath *p, GtkTreeIter *it,
			 gpointer data)
{
	g_ptr_array_add(data, ex_folder_model_get_file(EX_FOLDER_MODEL(tm), it));
}

static GPtrArray *view_selected(ExFileView *v)
{
	GPtrArray *a = g_ptr_array_new();

	if (!v->model)
		return a;
	if (tree_active(v)) {
		gtk_tree_selection_selected_foreach(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)),
			collect_tree, a);
	} else {
		GList *paths = gtk_icon_view_get_selected_items(
			GTK_ICON_VIEW(v->icons));

		paths = g_list_reverse(paths);
		for (GList *l = paths; l; l = l->next) {
			GtkTreeIter it;

			if (gtk_tree_model_get_iter(GTK_TREE_MODEL(v->model), &it,
						    l->data))
				g_ptr_array_add(a, ex_folder_model_get_file(
							   v->model, &it));
		}
		g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
	}
	return a;
}

static void view_selection_changed(ExFileView *v)
{
	GPtrArray *a;

	if (v->syncing || v->removing)
		return;
	/* The user chose something else: the deleted items' successor is
	 * no longer wanted. */
	g_clear_object(&v->anchor);
	a = view_selected(v);
	ex_selection_set(v->sel, a);
	g_ptr_array_unref(a);
}

static void tree_sel_changed(GtkTreeSelection *s, ExFileView *v)
{
	view_selection_changed(v);
}

static void icon_sel_changed(GtkIconView *iv, ExFileView *v)
{
	view_selection_changed(v);
}

/* Show `items' (ExFile) as the selection of the visible view.  `focus':
 * the first of them also gets the keyboard cursor and is scrolled to, so
 * the arrow keys go on from it. */
static void apply_selection(ExFileView *v, GPtrArray *items, gboolean focus)
{
	GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree));
	GtkTreePath *first = NULL;

	if (!v->model)
		return;
	v->syncing = TRUE;
	gtk_tree_selection_unselect_all(ts);
	gtk_icon_view_unselect_all(GTK_ICON_VIEW(v->icons));
	for (guint i = 0; i < items->len; i++) {
		ExFile *f = g_ptr_array_index(items, i);
		GtkTreeIter it;

		if (!ex_folder_model_get_iter_for(v->model, ex_file_get_file(f),
						  &it))
			continue;
		GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(v->model),
							 &it);
		if (!first && focus) {
			first = gtk_tree_path_copy(p);
			/* The tree's cursor selects its row: set it before
			 * selecting the rest. */
			if (tree_active(v))
				gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->tree), p,
							 NULL, FALSE);
		}
		if (tree_active(v))
			gtk_tree_selection_select_path(ts, p);
		else
			gtk_icon_view_select_path(GTK_ICON_VIEW(v->icons), p);
		gtk_tree_path_free(p);
	}
	if (first) {
		if (tree_active(v)) {
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree), first,
						     NULL, FALSE, 0, 0);
		} else {
			gtk_icon_view_set_cursor(GTK_ICON_VIEW(v->icons), first,
						 NULL, FALSE);
			gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(v->icons), first,
						     FALSE, 0, 0);
		}
		gtk_tree_path_free(first);
	}
	v->syncing = FALSE;
}

/* Push ExSelection into the visible view (after a mode switch, or when the
 * selection was set from outside). */
static void push_selection(ExFileView *v)
{
	GPtrArray *items = ex_objects_copy(ex_selection_get(v->sel));

	apply_selection(v, items, TRUE);
	g_ptr_array_unref(items);
	if (!v->removing) {
		GPtrArray *a = view_selected(v);

		ex_selection_set(v->sel, a);
		g_ptr_array_unref(a);
	}
}

/* ---- rows going away ------------------------------------------------------- */

/* Before the views hear of a removed row: they react to losing their
 * cursor row by selecting the row next to it, which is not a choice the
 * user made -- the selection is put right afterwards. */
static void model_row_deleting(GtkTreeModel *tm, GtkTreePath *p,
			       ExFileView *v)
{
	v->removing++;
}

static gboolean view_shows(ExFileView *v, GPtrArray *items)
{
	guint n;

	if (tree_active(v))
		n = (guint)gtk_tree_selection_count_selected_rows(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)));
	else {
		GList *l = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(v->icons));

		n = g_list_length(l);
		g_list_free_full(l, (GDestroyNotify)gtk_tree_path_free);
	}
	if (n != items->len)
		return FALSE;
	for (guint i = 0; i < items->len; i++) {
		GtkTreeIter it;
		GtkTreePath *p;
		gboolean on;

		if (!ex_folder_model_get_iter_for(
			    v->model, ex_file_get_file(g_ptr_array_index(items, i)),
			    &it))
			return FALSE;
		p = gtk_tree_model_get_path(GTK_TREE_MODEL(v->model), &it);
		on = path_selected(v, p);
		gtk_tree_path_free(p);
		if (!on)
			return FALSE;
	}
	return TRUE;
}

/* A folder with no items (or none yet, while it loads) draws no keyboard
 * ring round the whole list: the theme leaves it out for EX_CLASS_EMPTY. */
static void note_empty(ExFileView *v)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(v->tree);
	gboolean empty =
		!v->model ||
		gtk_tree_model_iter_n_children(GTK_TREE_MODEL(v->model), NULL) == 0;

	if (empty == gtk_style_context_has_class(sc, EX_CLASS_EMPTY))
		return;
	if (empty)
		gtk_style_context_add_class(sc, EX_CLASS_EMPTY);
	else
		gtk_style_context_remove_class(sc, EX_CLASS_EMPTY);
}

/* After: the selection is what it was, less what went.  When the last of
 * the items deleted from here went, the item that took their place is
 * selected instead. */
static void model_row_deleted(GtkTreeModel *tm, GtkTreePath *p, ExFileView *v)
{
	GPtrArray *was = ex_selection_get(v->sel);
	GPtrArray *keep = g_ptr_array_new_with_free_func(g_object_unref);
	gboolean focus = FALSE;
	ExFile *a;

	note_empty(v);
	if (--v->removing > 0) {
		g_ptr_array_unref(keep);
		return;
	}
	for (guint i = 0; i < was->len; i++) {
		ExFile *f = g_ptr_array_index(was, i);
		GtkTreeIter it;

		if (ex_folder_model_get_iter_for(v->model, ex_file_get_file(f),
						 &it))
			g_ptr_array_add(keep, g_object_ref(f));
	}
	if (v->anchor && g_get_monotonic_time() > v->anchor_until)
		g_clear_object(&v->anchor);
	if (!keep->len && v->anchor &&
	    (a = ex_folder_model_lookup(v->model, v->anchor)) != NULL) {
		g_ptr_array_add(keep, g_object_ref(a));
		g_clear_object(&v->anchor);
		focus = TRUE;
	}
	if (focus || !view_shows(v, keep))
		apply_selection(v, keep, focus);
	if (keep->len != was->len || focus)
		ex_selection_set(v->sel, keep);
	g_ptr_array_unref(keep);
}

/* An item renamed in place: same row, new ExFile. */
static void model_file_replaced(ExFolderModel *m, ExFile *old, ExFile *nw,
				ExFileView *v)
{
	GPtrArray *was = ex_selection_get(v->sel);
	gboolean selected = g_ptr_array_find(was, old, NULL);
	GtkTreeIter it;

	if (selected)
		view_selection_changed(v);
	if (ex_folder_model_get_iter_for(m, ex_file_get_file(nw), &it)) {
		GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(m), &it);

		/* Renamed here: the rename's result is waiting for it. */
		pending_hit(v, ex_file_get_file(nw), p);
		/* It moved to its new place in the order: keep it in sight. */
		if (path_selected(v, p)) {
			if (tree_active(v))
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree),
							     p, NULL, FALSE, 0, 0);
			else
				gtk_icon_view_scroll_to_path(
					GTK_ICON_VIEW(v->icons), p, FALSE, 0, 0);
		}
		gtk_tree_path_free(p);
	}
}

/* ---- drawing cells ----------------------------------------------------------- */

static ExFile *file_at(GtkTreeModel *tm, GtkTreeIter *it)
{
	return ex_folder_model_get_file(EX_FOLDER_MODEL(tm), it);
}

/* Whether the name is shown without its extension: when extensions are
 * hidden, and for a shortcut always (its name is what it starts). */
static gboolean ext_hidden(ExFileView *v, ExFile *f)
{
	return !ex_file_is_dir(f) && (!v->show_ext || ex_icons_is_shortcut(f));
}

static char *shown_name(ExFileView *v, ExFile *f)
{
	const char *n = ex_file_get_name(f);

	if (!ext_hidden(v, f))
		return g_strdup(n);
	const char *ext = ex_name_extension(n);
	return g_strndup(n, (gsize)(ext - n));
}

static gboolean dimmed(ExFile *f)
{
	return ex_clipboard_is_cut(ex_clipboard_get(), ex_file_get_file(f)) ||
	       ex_file_is_hidden(f);
}

static void cell_icon(GtkCellLayout *l, GtkCellRenderer *r, GtkTreeModel *tm,
		      GtkTreeIter *it, gpointer data)
{
	ExFileView *v = data;
	ExFile *f = file_at(tm, it);
	/* The size is the size of the view DRAWING, not of the one on screen:
	 * GTK measures the hidden view too (rows are validated in the
	 * background), and answering the Details column with a 128-pixel icon
	 * while Large icons was showing left that width cached in it -- back
	 * in Details, each small icon sat in the middle of a wide cell with the
	 * name far to the right of it. */
	gboolean in_tree = GTK_IS_TREE_VIEW_COLUMN(l);
	int size = in_tree ? 16 : v->icon_size;
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(v));
	GdkPixbuf *pb = ex_icons_for_file(f, size, scale, dimmed(f), !in_tree);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(
					  pb, scale, NULL) :
				  NULL;

	g_object_set(r, "surface", s, NULL);
	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
}

static void cell_name(GtkCellLayout *l, GtkCellRenderer *r, GtkTreeModel *tm,
		      GtkTreeIter *it, gpointer data)
{
	ExFileView *v = data;
	ExFile *f = file_at(tm, it);
	char *n = shown_name(v, f);

	/* Three lines only in the Tiles icon view itself (see cell_icon()). */
	if (!GTK_IS_TREE_VIEW_COLUMN(l) && !strcmp(v->mode, "tiles")) {
		char *kind = ex_icons_kind_label(f);
		char *size = ex_file_is_dir(f) ? g_strdup("") :
						 ex_format_size(ex_file_get_size(f));
		char *m = g_markup_printf_escaped(
			"%s\n<span size='small' alpha='60%%'>%s\n%s</span>", n, kind,
			size);

		g_object_set(r, "markup", m, NULL);
		g_free(m);
		g_free(kind);
		g_free(size);
	} else {
		g_object_set(r, "text", n, NULL);
	}
	g_free(n);
}

static void cell_mtime(GtkTreeViewColumn *c, GtkCellRenderer *r,
		       GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	char *t = ex_format_time(ex_file_get_mtime(file_at(tm, it)));

	g_object_set(r, "text", t, NULL);
	g_free(t);
}

static void cell_type(GtkTreeViewColumn *c, GtkCellRenderer *r,
		      GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	char *t = ex_icons_kind_label(file_at(tm, it));

	g_object_set(r, "text", t, NULL);
	g_free(t);
}

static void cell_size(GtkTreeViewColumn *c, GtkCellRenderer *r,
		      GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	ExFile *f = file_at(tm, it);
	/* The size in the unit that suits it ("4.2 MB"), as every other
	 * place in the system writes sizes. */
	char *t = ex_file_is_dir(f) ? g_strdup("") :
				      ex_format_size(ex_file_get_size(f));

	g_object_set(r, "text", t, NULL);
	g_free(t);
}

static void cell_origin(GtkTreeViewColumn *c, GtkCellRenderer *r,
			GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	const char *o = ex_file_get_trash_orig_path(file_at(tm, it));
	char *d = o && *o ? g_path_get_dirname(o) : g_strdup("");

	g_object_set(r, "text", d, NULL);
	g_free(d);
}

static void cell_deleted(GtkTreeViewColumn *c, GtkCellRenderer *r,
			 GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	char *t = ex_format_time(ex_file_get_trash_deleted(file_at(tm, it)));

	g_object_set(r, "text", t, NULL);
	g_free(t);
}

static void cell_folder(GtkTreeViewColumn *c, GtkCellRenderer *r,
			GtkTreeModel *tm, GtkTreeIter *it, gpointer data)
{
	GFile *p = g_file_get_parent(ex_file_get_file(file_at(tm, it)));
	char *s = p ? g_file_get_parse_name(p) : g_strdup("");

	g_object_set(r, "text", s, NULL);
	g_free(s);
	g_clear_object(&p);
}

/* ---- columns ----------------------------------------------------------------- */

static void header_clicked(GtkTreeViewColumn *c, ExFileView *v)
{
	int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "ex-sort"));
	ExSortColumn cur;
	gboolean asc;

	ex_folder_model_get_sort(v->model, &cur, &asc);
	asc = (int)cur == col ? !asc : TRUE;
	g_signal_emit(v, sigs[SIG_SORT], 0, col, asc);
}

/* A column's header: the title at the left, and for the column the items
 * are sorted by a small chevron at the right end, centred on the title's
 * line -- pointing up for ascending -- instead of GTK's arrow.  Drawn over
 * the header button, in the accent colour. */
static gboolean header_draw(GtkWidget *b, cairo_t *cr, GtkTreeViewColumn *c)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(b);
	gboolean up = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "ex-up"));
	double cx = gtk_widget_get_allocated_width(b) - 14.5;
	double mid = gtk_widget_get_allocated_height(b) / 2.0;
	double top = floor(mid) - 1.5, bottom = floor(mid) + 2.5;
	GdkRGBA col;

	if (!g_object_get_data(G_OBJECT(c), "ex-sorted"))
		return FALSE;
	if (!gtk_style_context_lookup_color(sc, "likeos_accent_fill", &col))
		gtk_style_context_get_color(sc, gtk_style_context_get_state(sc),
					    &col);
	cairo_save(cr);
	cairo_set_line_width(cr, 1.25);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, cx - 4, up ? bottom : top);
	cairo_line_to(cr, cx, up ? top : bottom);
	cairo_line_to(cr, cx + 4, up ? bottom : top);
	gdk_cairo_set_source_rgba(cr, &col);
	cairo_stroke(cr);
	cairo_restore(cr);
	return FALSE;
}

/* After the column joined the tree: its header button exists then. */
static void column_header(GtkTreeViewColumn *c)
{
	GtkWidget *b = gtk_tree_view_column_get_button(c);

	if (b)
		g_signal_connect_after(b, "draw", G_CALLBACK(header_draw), c);
}

static void column_title(GtkTreeViewColumn *c, const char *title)
{
	gtk_tree_view_column_set_title(c, title);
}

static GtkTreeViewColumn *text_column(ExFileView *v, const char *title,
				      int width)
{
	GtkTreeViewColumn *c = gtk_tree_view_column_new();
	GtkCellRenderer *r = gtk_cell_renderer_text_new();

	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 6, NULL);
	gtk_tree_view_column_set_title(c, title);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_fixed_width(c, width);
	gtk_tree_view_column_set_clickable(c, TRUE);
	g_signal_connect(c, "clicked", G_CALLBACK(header_clicked), v);
	g_object_set_data(G_OBJECT(c), "ex-renderer", r);
	return c;
}

static void set_col_func(GtkTreeViewColumn *c, GtkTreeCellDataFunc fn,
			 int sort, ExFileView *v)
{
	GtkCellRenderer *r = g_object_get_data(G_OBJECT(c), "ex-renderer");

	gtk_tree_view_column_set_cell_data_func(c, r, fn, v, NULL);
	g_object_set_data(G_OBJECT(c), "ex-sort", GINT_TO_POINTER(sort));
}

static void apply_columns(ExFileView *v)
{
	switch (v->cols) {
	case EX_COLUMNS_TRASH:
		column_title(v->c_2, "Original location");
		set_col_func(v->c_2, cell_origin, EX_SORT_ORIGIN, v);
		column_title(v->c_3, "Date deleted");
		set_col_func(v->c_3, cell_deleted, EX_SORT_DELETED, v);
		break;
	case EX_COLUMNS_SEARCH:
		column_title(v->c_2, "Folder");
		set_col_func(v->c_2, cell_folder, EX_SORT_NAME, v);
		column_title(v->c_3, "Date modified");
		set_col_func(v->c_3, cell_mtime, EX_SORT_MODIFIED, v);
		break;
	case EX_COLUMNS_FOLDER:
	default:
		column_title(v->c_2, "Date modified");
		set_col_func(v->c_2, cell_mtime, EX_SORT_MODIFIED, v);
		column_title(v->c_3, "Type");
		set_col_func(v->c_3, cell_type, EX_SORT_TYPE, v);
		break;
	}
	set_col_func(v->c_size, cell_size, EX_SORT_SIZE, v);
	gtk_widget_queue_draw(v->tree);
}

static void show_sort_indicator(ExFileView *v)
{
	ExSortColumn col;
	gboolean asc, shown = FALSE;
	GtkTreeViewColumn *all[] = { v->c_name, v->c_2, v->c_3, v->c_size };

	if (!v->model)
		return;
	ex_folder_model_get_sort(v->model, &col, &asc);
	for (gsize i = 0; i < G_N_ELEMENTS(all); i++) {
		int c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(all[i]),
							  "ex-sort"));
		GtkWidget *b = gtk_tree_view_column_get_button(all[i]);
		/* One chevron: a search's Folder column sorts by name too. */
		gboolean on = c == (int)col && !shown;

		shown |= on;
		/* GTK's own indicator stays off (header_draw() draws the
		 * chevron); the order is still told to the column. */
		gtk_tree_view_column_set_sort_indicator(all[i], FALSE);
		gtk_tree_view_column_set_sort_order(
			all[i], asc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
		g_object_set_data(G_OBJECT(all[i]), "ex-sorted",
				  GINT_TO_POINTER(on));
		g_object_set_data(G_OBJECT(all[i]), "ex-up", GINT_TO_POINTER(asc));
		if (b)
			gtk_widget_queue_draw(b);
	}
}

/* The columns after the name in the secondary text colour: the name reads
 * first.  Looked up again when the theme changes. */
static void tree_style_updated(GtkWidget *w, ExFileView *v)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	GtkTreeViewColumn *cols[] = { v->c_2, v->c_3, v->c_size };
	GdkRGBA c;

	if (!gtk_style_context_lookup_color(sc, "likeos_text_secondary", &c)) {
		gtk_style_context_get_color(sc, GTK_STATE_FLAG_NORMAL, &c);
		c.alpha *= 0.7;
	}
	for (gsize i = 0; i < G_N_ELEMENTS(cols); i++)
		g_object_set(g_object_get_data(G_OBJECT(cols[i]), "ex-renderer"),
			     "foreground-rgba", &c, NULL);
}

/* ---- events -------------------------------------------------------------------- */

static void activate_selection(ExFileView *v)
{
	GPtrArray *a = view_selected(v);

	if (a->len)
		g_signal_emit(v, sigs[SIG_ACTIVATE], 0, a);
	g_ptr_array_unref(a);
}

static void tree_row_activated(GtkTreeView *tv, GtkTreePath *p,
			       GtkTreeViewColumn *c, ExFileView *v)
{
	activate_selection(v);
}

static void icon_item_activated(GtkIconView *iv, GtkTreePath *p, ExFileView *v)
{
	activate_selection(v);
}

static GtkTreePath *path_at(ExFileView *v, double x, double y)
{
	GtkTreePath *p = NULL;

	if (tree_active(v)) {
		if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(v->tree), (int)x,
						   (int)y, &p, NULL, NULL, NULL))
			p = NULL;
	} else {
		p = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(v->icons), (int)x,
						  (int)y);
	}
	return p;
}

static gboolean path_selected(ExFileView *v, GtkTreePath *p)
{
	if (tree_active(v))
		return gtk_tree_selection_path_is_selected(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)), p);
	return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(v->icons), p);
}

static void select_only(ExFileView *v, GtkTreePath *p)
{
	if (tree_active(v)) {
		GtkTreeSelection *s =
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree));

		gtk_tree_selection_unselect_all(s);
		gtk_tree_selection_select_path(s, p);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->tree), p, NULL, FALSE);
	} else {
		gtk_icon_view_unselect_all(GTK_ICON_VIEW(v->icons));
		gtk_icon_view_select_path(GTK_ICON_VIEW(v->icons), p);
		gtk_icon_view_set_cursor(GTK_ICON_VIEW(v->icons), p, NULL, FALSE);
	}
}

/* `p' joins the selection and gets the keyboard cursor. */
static void add_to_selection(ExFileView *v, GtkTreePath *p)
{
	if (tree_active(v)) {
		GtkTreeSelection *s =
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree));
		GList *rows = gtk_tree_selection_get_selected_rows(s, NULL);

		/* The tree's cursor selects only its row: put the rest back. */
		v->syncing = TRUE;
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->tree), p, NULL, FALSE);
		for (GList *l = rows; l; l = l->next)
			gtk_tree_selection_select_path(s, l->data);
		v->syncing = FALSE;
		g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	} else {
		v->syncing = TRUE;
		gtk_icon_view_select_path(GTK_ICON_VIEW(v->icons), p);
		gtk_icon_view_set_cursor(GTK_ICON_VIEW(v->icons), p, NULL, FALSE);
		v->syncing = FALSE;
	}
	view_selection_changed(v);
}

static gboolean single_click(ExFileView *v)
{
	return gtk_tree_view_get_activate_on_single_click(GTK_TREE_VIEW(v->tree));
}

static void drop_held(ExFileView *v)
{
	g_clear_pointer(&v->held, gtk_tree_path_free);
}

static gboolean button_press(GtkWidget *w, GdkEventButton *ev, ExFileView *v)
{
	GtkTreePath *p;
	gboolean held_before = v->held_last;

	if (ev->type == GDK_BUTTON_PRESS)
		v->held_last = FALSE;
	/* The tree view's coordinates are of its bin window. */
	if (tree_active(v) && ev->window != gtk_tree_view_get_bin_window(
						    GTK_TREE_VIEW(v->tree)))
		return FALSE;
	if (ev->type == GDK_2BUTTON_PRESS) {
		/* The tree never saw the second press of this double click
		 * (it was held back below), so it cannot count it: open
		 * here. */
		if (held_before && ev->button == 1 && !single_click(v)) {
			drop_held(v);
			activate_selection(v);
			return TRUE;
		}
		return FALSE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	p = path_at(v, ev->x, ev->y);
	/* A press on an item: it is selected at once (Ctrl: added), and
	 * moving on drags the selection -- GTK's tree would start a
	 * selection rectangle from an unselected row instead.  Releasing
	 * without moving settles the click.  Shift keeps GTK's range
	 * selection; a press on no item, the rectangle. */
	if (ev->button == 1 && p && !(ev->state & GDK_SHIFT_MASK)) {
		gboolean on = path_selected(v, p);

		drop_held(v);
		gtk_widget_grab_focus(w);
		if (ev->state & GDK_CONTROL_MASK) {
			if (!on)
				add_to_selection(v, p);
			v->held_then = on ? HELD_UNSELECT : HELD_KEEP;
		} else {
			if (!on)
				select_only(v, p);
			v->held_then = HELD_SELECT_ONLY;
		}
		v->held = p;
		v->held_x = ev->x_root;
		v->held_y = ev->y_root;
		v->held_last = TRUE;
		return TRUE;
	}
	if (ev->button == 3) {
		gtk_widget_grab_focus(w);
		if (p && !path_selected(v, p))
			select_only(v, p);
		else if (!p)
			ex_file_view_select_none(v);
		g_signal_emit(v, sigs[SIG_POPUP], 0, ev, p != NULL);
		if (p)
			gtk_tree_path_free(p);
		return TRUE;
	}
	if (ev->button == 1 && !p && !(ev->state & (GDK_CONTROL_MASK |
						    GDK_SHIFT_MASK)))
		ex_file_view_select_none(v); /* a click on nothing */
	if (ev->button == 8 || ev->button == 9) {
		/* Mouse back/forward buttons: the window's actions. */
		GtkWidget *top = gtk_widget_get_toplevel(w);

		if (GTK_IS_APPLICATION_WINDOW(top))
			g_action_group_activate_action(
				G_ACTION_GROUP(top),
				ev->button == 8 ? "back" : "forward", NULL);
		if (p)
			gtk_tree_path_free(p);
		return TRUE;
	}
	if (p)
		gtk_tree_path_free(p);
	return FALSE;
}

static gboolean held_motion(GtkWidget *w, GdkEventMotion *ev, ExFileView *v)
{
	GtkTargetList *tl;

	if (!v->held)
		return FALSE;
	if (!(ev->state & GDK_BUTTON1_MASK)) {
		drop_held(v);
		return FALSE;
	}
	if (!gtk_drag_check_threshold(w, (int)v->held_x, (int)v->held_y,
				      (int)ev->x_root, (int)ev->y_root))
		return TRUE;
	drop_held(v);
	tl = gtk_target_list_new(drag_targets, G_N_ELEMENTS(drag_targets));
	gtk_drag_begin_with_coordinates(w, tl,
					GDK_ACTION_COPY | GDK_ACTION_MOVE |
						GDK_ACTION_LINK,
					1, (GdkEvent *)ev, -1, -1);
	gtk_target_list_unref(tl);
	return TRUE;
}

/* Released without a drag: a plain click selects just the item (and
 * opens it, with single-click opening); Ctrl+click on a selected item
 * takes it out of the selection. */
static gboolean held_release(GtkWidget *w, GdkEventButton *ev, ExFileView *v)
{
	GtkTreePath *p = v->held;

	if (!p || ev->button != 1)
		return FALSE;
	v->held = NULL;
	switch (v->held_then) {
	case HELD_SELECT_ONLY:
		select_only(v, p);
		if (single_click(v))
			activate_selection(v);
		break;
	case HELD_UNSELECT:
		if (tree_active(v))
			gtk_tree_selection_unselect_path(
				gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)),
				p);
		else
			gtk_icon_view_unselect_path(GTK_ICON_VIEW(v->icons), p);
		break;
	default:
		break;
	}
	gtk_tree_path_free(p);
	return TRUE;
}

static gboolean popup_menu_key(GtkWidget *w, ExFileView *v)
{
	GPtrArray *a = view_selected(v);
	gboolean on_item = a->len > 0;

	g_ptr_array_unref(a);
	g_signal_emit(v, sigs[SIG_POPUP], 0, NULL, on_item);
	return TRUE;
}

/* Type-to-select: the first item whose name starts with what was typed. */
static gboolean typeahead(ExFileView *v, GdkEventKey *ev)
{
	gunichar c = gdk_keyval_to_unicode(ev->keyval);
	gint64 now = g_get_monotonic_time();
	char buf[8];
	int len;

	if (!c || !g_unichar_isprint(c) ||
	    (ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) || !v->model)
		return FALSE;
	if (now - v->typeahead_at > EX_TYPEAHEAD_US)
		g_string_truncate(v->typeahead, 0);
	v->typeahead_at = now;
	if (c == ' ' && v->typeahead->len == 0)
		return FALSE; /* space toggles the selection */
	len = g_unichar_to_utf8(c, buf);
	g_string_append_len(v->typeahead, buf, len);

	char *want = g_utf8_casefold(v->typeahead->str, -1);
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(v->model), &it);

	while (ok) {
		char *fold = g_utf8_casefold(
			ex_file_get_name(ex_folder_model_get_file(v->model, &it)),
			-1);
		gboolean hit = g_str_has_prefix(fold, want);

		g_free(fold);
		if (hit) {
			GtkTreePath *p = gtk_tree_model_get_path(
				GTK_TREE_MODEL(v->model), &it);

			select_only(v, p);
			if (tree_active(v))
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree),
							     p, NULL, FALSE, 0, 0);
			else
				gtk_icon_view_scroll_to_path(
					GTK_ICON_VIEW(v->icons), p, FALSE, 0, 0);
			gtk_tree_path_free(p);
			break;
		}
		ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(v->model), &it);
	}
	g_free(want);
	return TRUE;
}

static gboolean key_press(GtkWidget *w, GdkEventKey *ev, ExFileView *v)
{
	if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) &&
	    !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
		activate_selection(v);
		return TRUE;
	}
	return typeahead(v, ev);
}

static gboolean scroll_zoom(GtkWidget *w, GdkEventScroll *ev, ExFileView *v)
{
	int dir = 0;

	if (!(ev->state & GDK_CONTROL_MASK))
		return FALSE;
	if (ev->direction == GDK_SCROLL_UP)
		dir = 1;
	else if (ev->direction == GDK_SCROLL_DOWN)
		dir = -1;
	else if (ev->direction == GDK_SCROLL_SMOOTH)
		dir = ev->delta_y < 0 ? 1 : ev->delta_y > 0 ? -1 : 0;
	if (dir)
		g_signal_emit(v, sigs[SIG_ZOOM], 0, dir);
	return TRUE;
}

/* ---- rename in place ------------------------------------------------------------ */

static void name_edited(GtkCellRendererText *r, const char *path,
			const char *text, ExFileView *v)
{
	GtkTreeIter it;
	GtkTreePath *p = gtk_tree_path_new_from_string(path);

	g_object_set(r, "editable", FALSE, NULL);
	if (gtk_tree_model_get_iter(GTK_TREE_MODEL(v->model), &it, p)) {
		ExFile *f = ex_folder_model_get_file(v->model, &it);
		char *shown = shown_name(v, f);

		if (strcmp(text, shown) != 0) {
			/* With extensions hidden, the edit is of the stem. */
			char *name = ext_hidden(v, f) ?
					     g_strconcat(text,
							 ex_name_extension(
								 ex_file_get_name(f)),
							 NULL) :
					     g_strdup(text);
			g_signal_emit(v, sigs[SIG_RENAME], 0, f, name);
			g_free(name);
		}
		g_free(shown);
	}
	gtk_tree_path_free(p);
}

static void edit_canceled(GtkCellRenderer *r, ExFileView *v)
{
	g_object_set(r, "editable", FALSE, NULL);
}

/* The part of the name to select: all but the extension. */
static gboolean select_stem(gpointer data)
{
	GtkEditable *ed = data;
	int end = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ed), "ex-stem-end"));

	gtk_editable_select_region(ed, 0, end);
	g_object_unref(ed);
	return G_SOURCE_REMOVE;
}

static void editing_started(GtkCellRenderer *r, GtkCellEditable *ed,
			    const char *path, ExFileView *v)
{
	GtkTreeIter it;
	GtkTreePath *p = gtk_tree_path_new_from_string(path);

	if (GTK_IS_ENTRY(ed) &&
	    gtk_tree_model_get_iter(GTK_TREE_MODEL(v->model), &it, p)) {
		ExFile *f = ex_folder_model_get_file(v->model, &it);
		char *n = shown_name(v, f);
		int end = -1;

		gtk_entry_set_text(GTK_ENTRY(ed), n);
		/* The name without its extension is selected, so typing
		 * replaces the name and keeps the type. */
		if (!ex_file_is_dir(f) && !ext_hidden(v, f)) {
			const char *ext = ex_name_extension(n);

			end = (int)g_utf8_strlen(n, (gssize)(ext - n));
		}
		/* After the entry has taken the focus: focusing an entry
		 * selects all of it, which would undo this. */
		g_object_set_data(G_OBJECT(ed), "ex-stem-end", GINT_TO_POINTER(end));
		g_idle_add(select_stem, g_object_ref(ed));
		g_free(n);
	}
	gtk_tree_path_free(p);
}

void ex_file_view_start_rename(ExFileView *v, ExFile *f)
{
	GtkTreeIter it;
	GtkTreePath *p;

	if (!v->model ||
	    !ex_folder_model_get_iter_for(v->model, ex_file_get_file(f), &it))
		return;
	p = gtk_tree_model_get_path(GTK_TREE_MODEL(v->model), &it);
	select_only(v, p);
	if (tree_active(v)) {
		g_object_set(v->r_name, "editable", TRUE, NULL);
		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree), p, NULL,
					     FALSE, 0, 0);
		gtk_widget_grab_focus(v->tree);
		gtk_tree_view_set_cursor_on_cell(GTK_TREE_VIEW(v->tree), p,
						 v->c_name, v->r_name, TRUE);
	} else {
		g_object_set(v->ri_text, "editable", TRUE, NULL);
		gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(v->icons), p, FALSE, 0,
					     0);
		gtk_widget_grab_focus(v->icons);
		gtk_icon_view_set_cursor(GTK_ICON_VIEW(v->icons), p, v->ri_text,
					 TRUE);
	}
	gtk_tree_path_free(p);
}

/* ---- drag and drop ------------------------------------------------------------- */

static void drag_data_get(GtkWidget *w, GdkDragContext *ctx,
			  GtkSelectionData *sd, guint info, guint time,
			  ExFileView *v)
{
	GPtrArray *a = view_selected(v);
	char **uris = g_new0(char *, a->len + 1);

	for (guint i = 0; i < a->len; i++)
		uris[i] = g_file_get_uri(ex_file_get_file(g_ptr_array_index(a, i)));
	gtk_selection_data_set_uris(sd, uris);
	g_strfreev(uris);
	g_ptr_array_unref(a);
}

/* ---- the drag's picture ------------------------------------------------- */

/* Picture sizes, in pixels. */
#define EX_DRAG_ICON 48
#define EX_DRAG_PAD 6
/* Each picture stacked behind the first, when several items go, sits this
 * much further right and up. */
#define EX_DRAG_STACK 3

static void theme_color(GtkWidget *w, const char *name, const char *fallback,
			GdkRGBA *c)
{
	if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w), name,
					    c) &&
	    !gtk_style_context_lookup_color(gtk_widget_get_style_context(w),
					    fallback, c))
		gdk_rgba_parse(c, "gray");
}

static void rounded(cairo_t *cr, double x, double y, double w, double h,
		    double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

/* What a drag shows under the pointer: the item's icon alone -- with two
 * more pictures stacked behind it and the number of items on a badge when
 * there are several -- and, over a place that takes them, what dropping
 * does there ("Move to Documents") on a card below it.  Nothing is drawn
 * around them and every part is opaque: on a screen without a compositing
 * manager the drag's window takes the shape of the picture, so what is
 * under the pointer shows around the icon. */
static void drag_paint(ExFileView *v)
{
	GtkWidget *w = GTK_WIDGET(v);
	int scale = gtk_widget_get_scale_factor(w);
	PangoLayout *hint = v->drag_hint ?
				    gtk_widget_create_pango_layout(w, v->drag_hint) :
				    NULL;
	PangoLayout *count = NULL;
	guint n = v->drag_files ? v->drag_files->len : 0;
	int stack = n > 2 ? 2 : n > 1 ? 1 : 0;
	GdkRGBA accent, card, text, stroke;
	int hw = 0, hh = 0, cw = 0, ch = 0, width, height;
	double half, r = 0, ix, iy, cx = 0, cy = 0;
	cairo_surface_t *s;
	cairo_t *cr;

	theme_color(w, "likeos_accent", "theme_selected_bg_color", &accent);
	theme_color(w, "likeos_card", "theme_base_color", &card);
	theme_color(w, "likeos_text", "theme_fg_color", &text);
	theme_color(w, "likeos_stroke", "borders", &stroke);
	if (hint)
		pango_layout_get_pixel_size(hint, &hw, &hh);
	if (n > 1) {
		char *num = g_strdup_printf("%u", n);
		PangoAttrList *at = pango_attr_list_new();

		count = gtk_widget_create_pango_layout(w, num);
		pango_attr_list_insert(at, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
		pango_layout_set_attributes(count, at);
		pango_layout_get_pixel_size(count, &cw, &ch);
		r = MAX(cw, ch) / 2.0 + 3;
		pango_attr_list_unref(at);
		g_free(num);
	}

	/* The icons centred, the badge on the front one's top right corner,
	 * the hint card under them. */
	half = EX_DRAG_ICON / 2.0 + EX_DRAG_STACK * stack + 1;
	if (count)
		half = MAX(half, EX_DRAG_ICON / 2.0 + r + 2);
	if (hint)
		half = MAX(half, (hw + 4 * EX_DRAG_PAD) / 2.0 + 1);
	width = (int)ceil(2 * half);
	iy = MAX(EX_DRAG_STACK * stack, count ? r - 2 : 0);
	height = (int)ceil(iy) + EX_DRAG_ICON + 1 +
		 (hint ? 4 + hh + 2 * EX_DRAG_PAD + 1 : 0);
	ix = (width - EX_DRAG_ICON) / 2.0;
	s = gdk_window_create_similar_image_surface(
		gtk_widget_get_window(w), CAIRO_FORMAT_ARGB32, width * scale,
		height * scale, scale);
	if (!s) {
		g_clear_object(&hint);
		g_clear_object(&count);
		return;
	}
	cr = cairo_create(s);

	if (v->drag_icon) {
		/* The pictures behind first, the front one over them. */
		for (int k = stack; k >= 0; k--) {
			cairo_save(cr);
			cairo_translate(cr, ix + EX_DRAG_STACK * k,
					iy - EX_DRAG_STACK * k);
			cairo_scale(cr, (double)EX_DRAG_ICON /
						gdk_pixbuf_get_width(v->drag_icon),
				    (double)EX_DRAG_ICON /
					    gdk_pixbuf_get_height(v->drag_icon));
			gdk_cairo_set_source_pixbuf(cr, v->drag_icon, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	if (count) {
		/* An accent disc on a white ring, so it stands off the icon. */
		cx = ix + EX_DRAG_ICON - 4;
		cy = iy + 4;
		cairo_arc(cr, cx, cy, r + 1.5, 0, 2 * G_PI);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
		gdk_cairo_set_source_rgba(cr, &accent);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_move_to(cr, cx - cw / 2.0, cy - ch / 2.0);
		pango_cairo_show_layout(cr, count);
	}
	if (hint) {
		double hx = (width - hw) / 2.0 - 2 * EX_DRAG_PAD;
		double hy = iy + EX_DRAG_ICON + 4;

		rounded(cr, hx + 0.5, hy + 0.5, hw + 4 * EX_DRAG_PAD,
			hh + 2 * EX_DRAG_PAD, 4);
		gdk_cairo_set_source_rgba(cr, &card);
		cairo_fill_preserve(cr);
		gdk_cairo_set_source_rgba(cr, &stroke);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		gdk_cairo_set_source_rgba(cr, &text);
		cairo_move_to(cr, hx + 2 * EX_DRAG_PAD, hy + EX_DRAG_PAD);
		pango_cairo_show_layout(cr, hint);
	}
	cairo_destroy(cr);
	/* The pointer on the middle of the front icon. */
	cairo_surface_set_device_offset(s, -(width / 2.0) * scale,
					-(iy + EX_DRAG_ICON / 2.0) * scale);
	gtk_drag_set_icon_surface(v->drag, s);
	cairo_surface_destroy(s);
	g_clear_object(&hint);
	g_clear_object(&count);
}

static void drag_clear(ExFileView *v)
{
	v->drag = NULL;
	g_clear_pointer(&v->drag_files, g_ptr_array_unref);
	g_clear_object(&v->drag_icon);
	g_clear_pointer(&v->drag_hint, g_free);
	g_clear_object(&v->hint_dest);
}

/* After the views' own handler, which pictures only the row under the
 * press. */
static void drag_begin(GtkWidget *w, GdkDragContext *ctx, ExFileView *v)
{
	GPtrArray *a = view_selected(v);

	drag_clear(v);
	drop_held(v);
	if (a->len) {
		ExFile *first = g_ptr_array_index(a, 0);

		v->drag = ctx;
		v->drag_files = g_ptr_array_new_with_free_func(g_object_unref);
		for (guint i = 0; i < a->len; i++)
			g_ptr_array_add(v->drag_files,
					g_object_ref(ex_file_get_file(
						g_ptr_array_index(a, i))));
		v->drag_icon = ex_icons_for_file(first, EX_DRAG_ICON,
						 gtk_widget_get_scale_factor(w),
						 FALSE, TRUE);
		drag_paint(v);
	}
	g_ptr_array_unref(a);
}

static void drag_end(GtkWidget *w, GdkDragContext *ctx, ExFileView *v)
{
	drag_clear(v);
}

/* The view a drag in this process started from, if one of ours. */
static ExFileView *drag_source(GdkDragContext *ctx)
{
	GtkWidget *src = gtk_drag_get_source_widget(ctx);
	ExFileView *v = src ? g_object_get_data(G_OBJECT(src), "ex-file-view") :
			      NULL;

	return v && v->drag ? v : NULL;
}

/* Move within one filesystem, copy across. */
static GdkDragAction same_fs_action(GFile *src, GFile *dest)
{
	const char *attr = G_FILE_ATTRIBUTE_ID_FILESYSTEM;
	GFileInfo *a = g_file_query_info(src, attr, G_FILE_QUERY_INFO_NONE, NULL,
					 NULL);
	GFileInfo *b = g_file_query_info(dest, attr, G_FILE_QUERY_INFO_NONE, NULL,
					 NULL);
	gboolean same = a && b &&
			!g_strcmp0(g_file_info_get_attribute_string(a, attr),
				   g_file_info_get_attribute_string(b, attr));

	g_clear_object(&a);
	g_clear_object(&b);
	return same ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

gboolean ex_file_view_dragging(GdkDragContext *ctx)
{
	return drag_source(ctx) != NULL;
}

GdkDragAction ex_file_view_drag_hint(GdkDragContext *ctx, GFile *dest,
				     const char *place, GdkDragAction forced)
{
	ExFileView *v = drag_source(ctx);
	GdkDragAction a;
	char *hint = NULL;

	if (!v)
		return forced;
	if (!dest) {
		a = 0;
	} else if (forced) {
		a = forced;
	} else {
		/* Asked of the disk once per destination, not per motion. */
		if (!v->hint_dest || !g_file_equal(v->hint_dest, dest)) {
			g_set_object(&v->hint_dest, dest);
			v->hint_action = same_fs_action(
				g_ptr_array_index(v->drag_files, 0), dest);
		}
		a = v->hint_action;
	}
	/* Onto itself, or moved to where it already is: nothing to do. */
	for (guint i = 0; a && i < v->drag_files->len; i++) {
		GFile *f = g_ptr_array_index(v->drag_files, i);
		GFile *parent = g_file_get_parent(f);

		if (g_file_equal(f, dest) || g_file_has_prefix(dest, f) ||
		    (a == GDK_ACTION_MOVE && parent && g_file_equal(parent, dest)))
			a = 0;
		g_clear_object(&parent);
	}
	if (a == GDK_ACTION_MOVE)
		hint = g_strdup_printf("\u2192 Move to %s", place);
	else if (a == GDK_ACTION_COPY)
		hint = g_strdup_printf("+ Copy to %s", place);
	else if (a == GDK_ACTION_LINK)
		hint = g_strdup_printf("\u2197 Create link in %s", place);
	if (g_strcmp0(hint, v->drag_hint) != 0) {
		g_free(v->drag_hint);
		v->drag_hint = hint;
		drag_paint(v);
	} else {
		g_free(hint);
	}
	return a;
}

/* The folder under the pointer, if the pointer is over one. */
static ExFile *folder_at(ExFileView *v, int x, int y, GtkTreePath **out)
{
	GtkTreePath *p = NULL;
	ExFile *f = NULL;
	GtkTreeIter it;

	if (tree_active(v)) {
		int bx, by;

		gtk_tree_view_convert_widget_to_bin_window_coords(
			GTK_TREE_VIEW(v->tree), x, y, &bx, &by);
		if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(v->tree), bx,
						   by, &p, NULL, NULL, NULL))
			p = NULL;
	} else {
		p = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(v->icons), x, y);
	}
	if (p && gtk_tree_model_get_iter(GTK_TREE_MODEL(v->model), &it, p)) {
		f = ex_folder_model_get_file(v->model, &it);
		if (!ex_file_is_dir(f) || !(ex_file_get_caps(f) & EX_CAP_WRITE))
			f = NULL;
	}
	if (out && f)
		*out = p;
	else if (p)
		gtk_tree_path_free(p);
	return f;
}

/* Move within one filesystem, copy across -- unless keys say otherwise:
 * Ctrl copies, Shift moves, Ctrl+Shift or Alt links. */
static GdkDragAction pick_action(GtkWidget *w, GdkDragContext *ctx)
{
	GdkModifierType mods = 0;
	GdkDragAction allowed = gdk_drag_context_get_actions(ctx);
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(w));

	gdk_window_get_device_position(gtk_widget_get_window(w),
				       gdk_seat_get_pointer(seat), NULL, NULL,
				       &mods);
	if ((mods & GDK_MOD1_MASK) ||
	    ((mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)))
		return allowed & GDK_ACTION_LINK ? GDK_ACTION_LINK : 0;
	if (mods & GDK_CONTROL_MASK)
		return allowed & GDK_ACTION_COPY;
	if (mods & GDK_SHIFT_MASK)
		return allowed & GDK_ACTION_MOVE;
	/* Decided for real on drop, where the files are known; while moving
	 * over the view, "move" when it is allowed. */
	return allowed & GDK_ACTION_MOVE ? GDK_ACTION_MOVE :
					   (allowed & GDK_ACTION_COPY);
}

static gboolean drag_motion(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			    guint time, ExFileView *v)
{
	GtkTreePath *p = NULL;
	ExFile *f = v->model ? folder_at(v, x, y, &p) : NULL;
	GdkDragAction a = pick_action(w, ctx);
	GdkModifierType mods = 0;
	char *name = f ? g_strdup(ex_file_get_name(f)) : g_strdup(v->folder_name);

	gdk_window_get_device_position(
		gtk_widget_get_window(w),
		gdk_seat_get_pointer(gdk_display_get_default_seat(
			gtk_widget_get_display(w))),
		NULL, NULL, &mods);
	/* From one of our views: what dropping here does, shown on the drag
	 * (and nothing at all where it would do nothing). */
	if (drag_source(ctx))
		a = ex_file_view_drag_hint(
			ctx, f ? ex_file_get_file(f) : v->folder, name,
			(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)) ?
				a :
			f ? 0 :
				v->folder_action);
	else if (!f && !v->folder)
		a = 0;
	g_free(name);

	if (tree_active(v))
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(v->tree), f ? p : NULL,
						GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
	else
		gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(v->icons),
						 f ? p : NULL,
						 GTK_ICON_VIEW_DROP_INTO);
	if (p)
		gtk_tree_path_free(p);
	gdk_drag_status(ctx, a, time);
	return TRUE;
}

static void drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time,
		       ExFileView *v)
{
	(void)ex_file_view_drag_hint(ctx, NULL, NULL, 0);
	if (tree_active(v))
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(v->tree), NULL, 0);
	else
		gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(v->icons), NULL, 0);
}

static gboolean drag_drop(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			  guint time, ExFileView *v)
{
	GdkAtom t = gtk_drag_dest_find_target(w, ctx, NULL);

	/* The views' own drop handling expects a model that takes rows;
	 * dropped files are a file operation, handled here alone. */
	g_signal_stop_emission_by_name(w, "drag-drop");
	if (t == GDK_NONE)
		return FALSE;
	gtk_drag_get_data(w, ctx, t, time);
	return TRUE;
}

static void drag_received(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			  GtkSelectionData *sd, guint info, guint time,
			  ExFileView *v)
{
	char **uris = gtk_selection_data_get_uris(sd);
	ExFile *dest = v->model ? folder_at(v, x, y, NULL) : NULL;
	GdkModifierType mods = 0;
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(w));
	guint action;

	g_signal_stop_emission_by_name(w, "drag-data-received");
	gdk_window_get_device_position(gtk_widget_get_window(w),
				       gdk_seat_get_pointer(seat), NULL, NULL,
				       &mods);
	/* 0: no key held -- the receiver chooses move or copy by filesystem. */
	action = (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)) ?
			 (guint)pick_action(w, ctx) :
			 0;
	if (uris && uris[0]) {
		GPtrArray *a = g_ptr_array_new_with_free_func(g_free);

		for (int i = 0; uris[i]; i++)
			g_ptr_array_add(a, g_strdup(uris[i]));
		g_signal_emit(v, sigs[SIG_DROP], 0, a,
			      dest ? ex_file_get_file(dest) : NULL, action);
		g_ptr_array_unref(a);
	}
	gtk_drag_finish(ctx, uris != NULL, FALSE, time);
	g_strfreev(uris);
	drag_leave(w, ctx, time, v);
}

static void setup_dnd(ExFileView *v, GtkWidget *w)
{
	GdkDragAction acts = GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK;

	if (GTK_IS_TREE_VIEW(w))
		gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(w),
						       GDK_BUTTON1_MASK,
						       drag_targets, 1, acts);
	else
		gtk_icon_view_enable_model_drag_source(GTK_ICON_VIEW(w),
						       GDK_BUTTON1_MASK,
						       drag_targets, 1, acts);
	gtk_drag_dest_set(w, 0, drag_targets, 1, acts);
	g_signal_connect(w, "drag-data-get", G_CALLBACK(drag_data_get), v);
	g_signal_connect_after(w, "drag-begin", G_CALLBACK(drag_begin), v);
	g_signal_connect(w, "drag-end", G_CALLBACK(drag_end), v);
	g_object_set_data(G_OBJECT(w), "ex-file-view", v);
	g_signal_connect(w, "drag-motion", G_CALLBACK(drag_motion), v);
	g_signal_connect(w, "drag-leave", G_CALLBACK(drag_leave), v);
	g_signal_connect(w, "drag-drop", G_CALLBACK(drag_drop), v);
	g_signal_connect(w, "drag-data-received", G_CALLBACK(drag_received), v);
}

/* ---- pending selections ------------------------------------------------------- */

/* `gf', at `p', just appeared (or got its new name): select it if a
 * selection is waiting for it. */
static void pending_hit(ExFileView *v, GFile *gf, GtkTreePath *p)
{
	if (!v->pending_select || g_get_monotonic_time() >= v->pending_until)
		return;
	for (guint i = 0; i < v->pending_select->len; i++) {
		if (!g_file_equal(g_ptr_array_index(v->pending_select, i), gf))
			continue;
		v->syncing = TRUE;
		if (v->pending_fresh) {
			v->pending_fresh = FALSE;
			gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(
				GTK_TREE_VIEW(v->tree)));
			gtk_icon_view_unselect_all(GTK_ICON_VIEW(v->icons));
			if (tree_active(v))
				gtk_tree_view_set_cursor(GTK_TREE_VIEW(v->tree), p,
							 NULL, FALSE);
			else
				gtk_icon_view_set_cursor(GTK_ICON_VIEW(v->icons), p,
							 NULL, FALSE);
		}
		if (tree_active(v))
			gtk_tree_selection_select_path(
				gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)),
				p);
		else
			gtk_icon_view_select_path(GTK_ICON_VIEW(v->icons), p);
		v->syncing = FALSE;
		view_selection_changed(v);
		return;
	}
}

static void row_inserted(GtkTreeModel *tm, GtkTreePath *p, GtkTreeIter *it,
			 ExFileView *v)
{
	gint64 now = g_get_monotonic_time();
	ExFile *f = ex_folder_model_get_file(v->model, it);
	GFile *gf = ex_file_get_file(f);

	if (v->pending_rename && now < v->rename_until &&
	    g_file_equal(gf, v->pending_rename)) {
		g_clear_object(&v->pending_rename);
		/* Started by model_row_inserted() once this insertion has
		 * settled in the view. */
		g_object_set_data_full(G_OBJECT(v), "ex-rename-file",
				       g_object_ref(f), g_object_unref);
	}
	pending_hit(v, gf, p);
}

/* ---- construction --------------------------------------------------------------- */

static void ex_file_view_finalize(GObject *o)
{
	ExFileView *v = EX_FILE_VIEW(o);

	g_clear_object(&v->sel);
	if (v->model)
		g_signal_handlers_disconnect_by_data(v->model, v);
	g_clear_object(&v->model);
	g_clear_object(&v->anchor);
	g_clear_pointer(&v->held, gtk_tree_path_free);
	drag_clear(v);
	g_clear_object(&v->folder);
	g_free(v->folder_name);
	g_free(v->mode);
	g_clear_pointer(&v->pending_select, g_ptr_array_unref);
	g_clear_object(&v->pending_rename);
	g_string_free(v->typeahead, TRUE);
	G_OBJECT_CLASS(ex_file_view_parent_class)->finalize(o);
}

static void ex_file_view_class_init(ExFileViewClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = ex_file_view_finalize;
	sigs[SIG_ACTIVATE] = g_signal_new("activate", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 1,
					  G_TYPE_PTR_ARRAY);
	sigs[SIG_POPUP] = g_signal_new("popup", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 2,
				       G_TYPE_POINTER, G_TYPE_BOOLEAN);
	sigs[SIG_RENAME] = g_signal_new("rename", t, G_SIGNAL_RUN_LAST, 0, NULL,
					NULL, NULL, G_TYPE_NONE, 2, EX_TYPE_FILE,
					G_TYPE_STRING);
	sigs[SIG_DROP] = g_signal_new("drop", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 3,
				      G_TYPE_PTR_ARRAY, G_TYPE_FILE, G_TYPE_UINT);
	sigs[SIG_SORT] = g_signal_new("sort", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT,
				      G_TYPE_BOOLEAN);
	sigs[SIG_ZOOM] = g_signal_new("zoom", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void build_tree(ExFileView *v)
{
	GtkCellRenderer *ri;

	v->tree = gtk_tree_view_new();
	gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v->tree), TRUE);
	gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(v->tree), TRUE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(v->tree), FALSE);
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)),
		GTK_SELECTION_MULTIPLE);
	atk_object_set_name(gtk_widget_get_accessible(v->tree), "Items");

	v->c_name = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(v->c_name, "Name");
	ri = v->r_icon = gtk_cell_renderer_pixbuf_new();
	/* A fixed 16-pixel icon cell: its width never depends on what was
	 * measured before.  Its height is the row's (set_row_height()). */
	g_object_set(ri, "xpad", 4, "xalign", 0.0, NULL);
	gtk_cell_renderer_set_fixed_size(ri, 16 + 2 * 4, EX_ROW_HEIGHT);
	gtk_tree_view_column_pack_start(v->c_name, ri, FALSE);
	gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v->c_name), ri,
					   cell_icon, v, NULL);
	v->r_name = gtk_cell_renderer_text_new();
	g_object_set(v->r_name, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 4,
		     NULL);
	gtk_tree_view_column_pack_start(v->c_name, v->r_name, TRUE);
	gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v->c_name), v->r_name,
					   cell_name, v, NULL);
	g_signal_connect(v->r_name, "edited", G_CALLBACK(name_edited), v);
	g_signal_connect(v->r_name, "editing-canceled", G_CALLBACK(edit_canceled),
			 v);
	g_signal_connect(v->r_name, "editing-started",
			 G_CALLBACK(editing_started), v);
	gtk_tree_view_column_set_sizing(v->c_name, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_fixed_width(v->c_name, 320);
	gtk_tree_view_column_set_resizable(v->c_name, TRUE);
	gtk_tree_view_column_set_expand(v->c_name, TRUE);
	gtk_tree_view_column_set_clickable(v->c_name, TRUE);
	g_object_set_data(G_OBJECT(v->c_name), "ex-sort",
			  GINT_TO_POINTER(EX_SORT_NAME));
	g_signal_connect(v->c_name, "clicked", G_CALLBACK(header_clicked), v);
	gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), v->c_name);

	v->c_2 = text_column(v, "Date modified", 160);
	v->c_3 = text_column(v, "Type", 150);
	v->c_size = text_column(v, "Size", 90);
	g_object_set(g_object_get_data(G_OBJECT(v->c_size), "ex-renderer"),
		     "xalign", 1.0, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), v->c_2);
	gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), v->c_3);
	gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), v->c_size);
	column_header(v->c_name);
	column_header(v->c_2);
	column_header(v->c_3);
	column_header(v->c_size);
	apply_columns(v);
	ex_style_add(v->tree, EX_CLASS_ITEMS);
	g_signal_connect(v->tree, "style-updated", G_CALLBACK(tree_style_updated),
			 v);

	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)),
			 "changed", G_CALLBACK(tree_sel_changed), v);
	g_signal_connect(v->tree, "row-activated", G_CALLBACK(tree_row_activated),
			 v);
	g_signal_connect(v->tree, "button-press-event", G_CALLBACK(button_press),
			 v);
	g_signal_connect(v->tree, "popup-menu", G_CALLBACK(popup_menu_key), v);
	g_signal_connect(v->tree, "key-press-event", G_CALLBACK(key_press), v);
	g_signal_connect(v->tree, "motion-notify-event", G_CALLBACK(held_motion),
			 v);
	g_signal_connect(v->tree, "button-release-event",
			 G_CALLBACK(held_release), v);
	setup_dnd(v, v->tree);

	v->tree_sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(v->tree_sw), v->tree);
}

static void build_icons(ExFileView *v)
{
	v->icons = gtk_icon_view_new();
	gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(v->icons),
					 GTK_SELECTION_MULTIPLE);
	gtk_icon_view_set_item_padding(GTK_ICON_VIEW(v->icons), 4);
	gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(v->icons), 2);
	gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(v->icons), 2);
	gtk_icon_view_set_margin(GTK_ICON_VIEW(v->icons), 8);
	atk_object_set_name(gtk_widget_get_accessible(v->icons), "Items");
	ex_style_add(v->icons, EX_CLASS_ITEMS);

	v->ri_pix = gtk_cell_renderer_pixbuf_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v->icons), v->ri_pix, FALSE);
	gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v->icons), v->ri_pix,
					   cell_icon, v, NULL);
	v->ri_text = gtk_cell_renderer_text_new();
	g_object_set(v->ri_text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "ellipsize",
		     PANGO_ELLIPSIZE_END, "alignment", PANGO_ALIGN_CENTER,
		     "xalign", 0.5, "yalign", 0.0, NULL);
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v->icons), v->ri_text, FALSE);
	gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v->icons), v->ri_text,
					   cell_name, v, NULL);
	g_signal_connect(v->ri_text, "edited", G_CALLBACK(name_edited), v);
	g_signal_connect(v->ri_text, "editing-canceled",
			 G_CALLBACK(edit_canceled), v);
	g_signal_connect(v->ri_text, "editing-started",
			 G_CALLBACK(editing_started), v);

	g_signal_connect(v->icons, "selection-changed",
			 G_CALLBACK(icon_sel_changed), v);
	g_signal_connect(v->icons, "item-activated",
			 G_CALLBACK(icon_item_activated), v);
	g_signal_connect(v->icons, "button-press-event",
			 G_CALLBACK(button_press), v);
	g_signal_connect(v->icons, "popup-menu", G_CALLBACK(popup_menu_key), v);
	g_signal_connect(v->icons, "key-press-event", G_CALLBACK(key_press), v);
	g_signal_connect(v->icons, "scroll-event", G_CALLBACK(scroll_zoom), v);
	g_signal_connect(v->icons, "motion-notify-event", G_CALLBACK(held_motion),
			 v);
	g_signal_connect(v->icons, "button-release-event",
			 G_CALLBACK(held_release), v);
	setup_dnd(v, v->icons);

	v->icon_sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_container_add(GTK_CONTAINER(v->icon_sw), v->icons);
}

static void ex_file_view_init(ExFileView *v)
{
	v->mode = g_strdup("details");
	v->show_ext = TRUE;
	v->icon_size = 48;
	v->typeahead = g_string_new(NULL);
	build_tree(v);
	build_icons(v);
	note_empty(v);
	g_signal_connect(v->tree, "scroll-event", G_CALLBACK(scroll_zoom), v);
	gtk_stack_add_named(GTK_STACK(v), v->tree_sw, "tree");
	gtk_stack_add_named(GTK_STACK(v), v->icon_sw, "icons");
	gtk_widget_show_all(v->tree_sw);
	gtk_widget_show_all(v->icon_sw);
}

GtkWidget *ex_file_view_new(ExSelection *selection)
{
	ExFileView *v = g_object_new(EX_TYPE_FILE_VIEW, NULL);

	v->sel = g_object_ref(selection);
	return GTK_WIDGET(v);
}

/* ---- public ----------------------------------------------------------------------- */

static gboolean rename_soon(gpointer data)
{
	ExFileView *v = data;
	ExFile *f = g_object_get_data(G_OBJECT(v), "ex-rename-file");

	if (f)
		ex_file_view_start_rename(v, f);
	g_object_set_data(G_OBJECT(v), "ex-rename-file", NULL);
	g_object_unref(v);
	return G_SOURCE_REMOVE;
}

static void model_row_inserted(GtkTreeModel *tm, GtkTreePath *p,
			       GtkTreeIter *it, ExFileView *v)
{
	gpointer had = g_object_get_data(G_OBJECT(v), "ex-rename-file");

	note_empty(v);
	row_inserted(tm, p, it, v);
	if (!had && g_object_get_data(G_OBJECT(v), "ex-rename-file"))
		g_idle_add(rename_soon, g_object_ref(v));
}

void ex_file_view_set_model(ExFileView *v, ExFolderModel *m)
{
	if (m == v->model)
		return;
	if (v->model)
		g_signal_handlers_disconnect_by_data(v->model, v);
	v->removing = 0;
	drop_held(v);
	g_clear_object(&v->anchor);
	g_set_object(&v->model, m);
	/* Before the views connect theirs: see model_row_deleting(). */
	if (m)
		g_signal_connect(m, "row-deleted", G_CALLBACK(model_row_deleting),
				 v);
	gtk_tree_view_set_model(GTK_TREE_VIEW(v->tree),
				m ? GTK_TREE_MODEL(m) : NULL);
	gtk_icon_view_set_model(GTK_ICON_VIEW(v->icons),
				m ? GTK_TREE_MODEL(m) : NULL);
	if (m) {
		g_signal_connect_after(m, "row-inserted",
				       G_CALLBACK(model_row_inserted), v);
		g_signal_connect_after(m, "row-deleted",
				       G_CALLBACK(model_row_deleted), v);
		g_signal_connect(m, "file-replaced",
				 G_CALLBACK(model_file_replaced), v);
	}
	note_empty(v);
	show_sort_indicator(v);
}

/* The icon views' layouts: the picture's size, the
 * width of an item, whether the name is beside the picture or under it,
 * and how wide the name may be. */
static const struct {
	const char *mode;
	int icon, width, wrap;
	gboolean beside;
} icon_modes[] = {
	{ "extra-large-icons", 256, 264, 256, FALSE },
	{ "large-icons", 96, 104, 100, FALSE },
	{ "icons", 48, 76, 72, FALSE },
	{ "small-icons", 16, 200, 170, TRUE },
	{ "tiles", 48, 250, 186, TRUE },
};

/* The tree's rows: their height lives in the icon cell, and the tree
 * measures it once (fixed-height mode) -- switched off and on, with the
 * rows marked dirty, the tree measures again. */
static void set_row_height(ExFileView *v, int height)
{
	int h;

	gtk_cell_renderer_get_fixed_size(v->r_icon, NULL, &h);
	if (h == height)
		return;
	gtk_cell_renderer_set_fixed_size(v->r_icon, 16 + 2 * 4, height);
	gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v->tree), FALSE);
	gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v->tree), TRUE);
	/* The rows measured before keep their heights until marked dirty. */
	gtk_tree_view_columns_autosize(GTK_TREE_VIEW(v->tree));
}

void ex_file_view_set_mode(ExFileView *v, const char *mode)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(v->tree);

	g_free(v->mode);
	v->mode = g_strdup(mode);
	if (is_tree_mode(mode)) {
		gboolean details = !strcmp(mode, "details");
		gboolean compact = !strcmp(mode, "compact");

		gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(v->tree), details);
		gtk_tree_view_column_set_visible(v->c_2, details);
		gtk_tree_view_column_set_visible(v->c_3, details);
		gtk_tree_view_column_set_visible(v->c_size, details);
		if (compact)
			gtk_style_context_add_class(sc, EX_CLASS_COMPACT);
		else
			gtk_style_context_remove_class(sc, EX_CLASS_COMPACT);
		set_row_height(v, compact ? EX_ROW_HEIGHT_COMPACT : EX_ROW_HEIGHT);
		gtk_stack_set_visible_child_name(GTK_STACK(v), "tree");
	} else {
		GtkIconView *iv = GTK_ICON_VIEW(v->icons);
		gsize k = 2; /* Medium icons for a mode not known here */

		for (gsize i = 0; i < G_N_ELEMENTS(icon_modes); i++)
			if (!strcmp(mode, icon_modes[i].mode))
				k = i;
		v->icon_size = icon_modes[k].icon;
		gtk_icon_view_set_item_orientation(
			iv, icon_modes[k].beside ? GTK_ORIENTATION_HORIZONTAL :
						   GTK_ORIENTATION_VERTICAL);
		gtk_icon_view_set_item_width(iv, icon_modes[k].width);
		/* Pictures of one size in a cell of that size: a thumbnail
		 * wider than tall sits on the cell's bottom edge, so the names
		 * under a row of them line up. */
		gtk_cell_renderer_set_fixed_size(v->ri_pix, v->icon_size,
						 v->icon_size);
		g_object_set(v->ri_pix, "xalign", 0.5, "yalign",
			     icon_modes[k].beside ? 0.5 : 1.0, NULL);
		g_object_set(v->ri_text, "xalign", icon_modes[k].beside ? 0.0 : 0.5,
			     "yalign", icon_modes[k].beside ? 0.5 : 0.0,
			     "alignment",
			     icon_modes[k].beside ? PANGO_ALIGN_LEFT :
						    PANGO_ALIGN_CENTER,
			     "wrap-width", icon_modes[k].wrap, NULL);
		gtk_icon_view_set_spacing(iv, icon_modes[k].beside ? 8 : 2);
		gtk_stack_set_visible_child_name(GTK_STACK(v), "icons");
		/* The layout depends on the item sizes just changed. */
		gtk_icon_view_set_model(iv, NULL);
		gtk_icon_view_set_model(iv, v->model ? GTK_TREE_MODEL(v->model) :
						       NULL);
	}
	push_selection(v);
}

const char *ex_file_view_get_mode(ExFileView *v)
{
	return v->mode;
}

void ex_file_view_set_columns(ExFileView *v, ExColumnSet cols)
{
	v->cols = cols;
	apply_columns(v);
	show_sort_indicator(v);
}

void ex_file_view_set_show_extensions(ExFileView *v, gboolean show)
{
	v->show_ext = show;
	gtk_widget_queue_draw(GTK_WIDGET(v));
}

void ex_file_view_set_single_click(ExFileView *v, gboolean single)
{
	gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(v->tree), single);
	gtk_icon_view_set_activate_on_single_click(GTK_ICON_VIEW(v->icons),
						   single);
}

void ex_file_view_select_files(ExFileView *v, GPtrArray *files)
{
	GPtrArray *found = g_ptr_array_new();

	g_clear_pointer(&v->pending_select, g_ptr_array_unref);
	v->pending_select = g_ptr_array_new_with_free_func(g_object_unref);
	v->pending_until = g_get_monotonic_time() + EX_PENDING_US;
	for (guint i = 0; i < files->len; i++) {
		GFile *f = g_ptr_array_index(files, i);
		ExFile *ef = v->model ? ex_folder_model_lookup(v->model, f) : NULL;

		g_ptr_array_add(v->pending_select, g_object_ref(f));
		if (ef)
			g_ptr_array_add(found, ef);
	}
	/* Nothing there yet (a rename or paste whose result the folder has
	 * not reported): the selection stays until the first one comes. */
	v->pending_fresh = !found->len && files->len;
	if (!v->pending_fresh) {
		ex_selection_set(v->sel, found);
		push_selection(v);
	}
	g_ptr_array_unref(found);
	ex_file_view_focus(v);
}

void ex_file_view_select_all(ExFileView *v)
{
	if (tree_active(v))
		gtk_tree_selection_select_all(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)));
	else
		gtk_icon_view_select_all(GTK_ICON_VIEW(v->icons));
}

void ex_file_view_select_none(ExFileView *v)
{
	if (tree_active(v))
		gtk_tree_selection_unselect_all(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)));
	else
		gtk_icon_view_unselect_all(GTK_ICON_VIEW(v->icons));
}

void ex_file_view_invert_selection(ExFileView *v)
{
	GtkTreeIter it;
	gboolean ok;

	if (!v->model)
		return;
	v->syncing = TRUE;
	ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(v->model), &it);
	while (ok) {
		GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(v->model),
							 &it);
		gboolean on = path_selected(v, p);

		if (tree_active(v)) {
			GtkTreeSelection *s = gtk_tree_view_get_selection(
				GTK_TREE_VIEW(v->tree));

			if (on)
				gtk_tree_selection_unselect_path(s, p);
			else
				gtk_tree_selection_select_path(s, p);
		} else {
			if (on)
				gtk_icon_view_unselect_path(GTK_ICON_VIEW(v->icons), p);
			else
				gtk_icon_view_select_path(GTK_ICON_VIEW(v->icons), p);
		}
		gtk_tree_path_free(p);
		ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(v->model), &it);
	}
	v->syncing = FALSE;
	view_selection_changed(v);
}

void ex_file_view_set_folder(ExFileView *v, GFile *folder, const char *name,
			     GdkDragAction always)
{
	v->folder_action = always;
	g_set_object(&v->folder, folder);
	g_free(v->folder_name);
	v->folder_name = g_strdup(name);
}

void ex_file_view_note_removal(ExFileView *v)
{
	GtkTreeIter it;
	gboolean ok, seen = FALSE;
	ExFile *before = NULL, *after = NULL;

	g_clear_object(&v->anchor);
	if (!v->model)
		return;
	/* The first unselected item after the last selected one; at the end
	 * of the list, the last unselected one before the first. */
	ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(v->model), &it);
	while (ok) {
		GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(v->model),
							 &it);
		gboolean on = path_selected(v, p);

		gtk_tree_path_free(p);
		if (on) {
			seen = TRUE;
			after = NULL;
		} else if (!seen) {
			before = ex_folder_model_get_file(v->model, &it);
		} else if (!after) {
			after = ex_folder_model_get_file(v->model, &it);
		}
		ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(v->model), &it);
	}
	if (!seen)
		return;
	if (after || before) {
		v->anchor = g_object_ref(ex_file_get_file(after ? after : before));
		v->anchor_until = g_get_monotonic_time() + EX_ANCHOR_US;
	}
}

void ex_file_view_rename_when_shown(ExFileView *v, GFile *f)
{
	ExFile *ef = v->model ? ex_folder_model_lookup(v->model, f) : NULL;

	if (ef) {
		ex_file_view_start_rename(v, ef);
		return;
	}
	g_set_object(&v->pending_rename, f);
	v->rename_until = g_get_monotonic_time() + EX_PENDING_US;
}

void ex_file_view_focus(ExFileView *v)
{
	gtk_widget_grab_focus(tree_active(v) ? v->tree : v->icons);
}
