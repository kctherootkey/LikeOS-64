// LikeOS -- File Explorer: the navigation pane.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-sidebar.h"
#include "ui/ex-file-view.h"
#include "ui/ex-style.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-monitor.h"
#include "backend/ex-trash.h"
#include "backend/ex-util.h"
#include "backend/ex-volumes.h"

#include <string.h>

enum { C_ICON, C_LABEL, C_ID, C_KIND, C_TIP, N_C };
enum { K_ITEM, K_HEADER, K_SEPARATOR };

struct _ExSidebar {
	GtkScrolledWindow parent;
	GtkTreeStore *store;
	GtkWidget *tree;
	char *current;
	ExMonitor *trash_mon;
	gboolean rebuilding;
	gboolean pinned_open, pc_open;
};

G_DEFINE_TYPE(ExSidebar, ex_sidebar, GTK_TYPE_SCROLLED_WINDOW)

enum { SIG_OPEN, SIG_DROP, SIG_POPUP, N_SIGS };
static guint sigs[N_SIGS];

static const GtkTargetEntry drop_targets[] = {
	{ (char *)"text/uri-list", 0, 0 },
};

static void add_row(ExSidebar *s, GtkTreeIter *parent, GtkTreeIter *out,
		    const char *icon, const char *label, const char *id,
		    int kind, const char *tip)
{
	GtkTreeIter it;

	gtk_tree_store_append(s->store, out ? out : &it, parent);
	gtk_tree_store_set(s->store, out ? out : &it, C_ICON, icon, C_LABEL,
			   label, C_ID, id, C_KIND, kind, C_TIP, tip, -1);
}

static void add_folder(ExSidebar *s, GtkTreeIter *parent, GFile *f,
		       const char *label)
{
	ExLocation *l = ex_location_new_folder(f);
	char *name = label ? g_strdup(label) : ex_location_display_name(l);
	char *tip = g_file_get_parse_name(f);

	add_row(s, parent, NULL, ex_location_icon_name(l), name,
		ex_location_id(l), K_ITEM, tip);
	g_free(tip);
	g_free(name);
	ex_location_unref(l);
}

static void rebuild(ExSidebar *s)
{
	GtkTreeIter it, grp;
	static const GUserDirectory personal[] = {
		G_USER_DIRECTORY_DESKTOP,   G_USER_DIRECTORY_DOWNLOAD,
		G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_PICTURES,
		G_USER_DIRECTORY_MUSIC,	    G_USER_DIRECTORY_VIDEOS,
	};
	ExVolumes *vols = ex_volumes_get_default();

	s->rebuilding = TRUE;
	gtk_tree_store_clear(s->store);

	add_row(s, NULL, NULL, "go-home", "Home", "likeos:home", K_ITEM,
		"Pinned folders, favorites and recent files");
	add_row(s, NULL, NULL, NULL, NULL, NULL, K_SEPARATOR, NULL);

	/* The personal folders that exist, in Windows' order. */
	for (gsize i = 0; i < G_N_ELEMENTS(personal); i++) {
		const char *p = ex_user_dir(personal[i]);

		if (!p || !g_file_test(p, G_FILE_TEST_IS_DIR))
			continue;
		GFile *f = g_file_new_for_path(p);
		add_folder(s, NULL, f, NULL);
		g_object_unref(f);
	}

	GPtrArray *bm = ex_bookmarks_list(ex_bookmarks_get_default());
	if (bm->len) {
		add_row(s, NULL, &grp, "view-pin-symbolic", "Pinned", "", K_HEADER,
			NULL);
		for (guint i = 0; i < bm->len; i++) {
			ExBookmark *b = g_ptr_array_index(bm, i);

			add_folder(s, &grp, b->file, b->label);
		}
	}

	add_row(s, NULL, NULL, NULL, NULL, NULL, K_SEPARATOR, NULL);
	add_row(s, NULL, &grp, "computer", "This PC", "likeos:thispc", K_ITEM,
		"Drives and personal folders");
	GPtrArray *vl = ex_volumes_list(vols);
	for (guint i = 0; vl && i < vl->len; i++) {
		ExVolume *v = g_ptr_array_index(vl, i);
		ExLocation *l = ex_location_new_folder(v->root);
		char *tip = g_file_get_parse_name(v->root);

		if (!v->network)
			add_row(s, &grp, NULL, v->icon_name, v->name,
				ex_location_id(l), K_ITEM, tip);
		g_free(tip);
		ex_location_unref(l);
	}
	if (ex_volumes_have_network(vols)) {
		add_row(s, NULL, &grp, "network-workgroup", "Network",
			"network:///", K_ITEM, "Computers and shares on the network");
		for (guint i = 0; vl && i < vl->len; i++) {
			ExVolume *v = g_ptr_array_index(vl, i);

			if (v->network)
				add_folder(s, &grp, v->root, v->name);
		}
	}
	add_row(s, NULL, NULL, NULL, NULL, NULL, K_SEPARATOR, NULL);
	add_row(s, NULL, &it, !ex_trash_is_empty() ? "user-trash-full" : "user-trash",
		"Trash", "likeos:trash", K_ITEM, "Deleted items");

	gtk_tree_view_expand_all(GTK_TREE_VIEW(s->tree));
	s->rebuilding = FALSE;
	if (s->current) {
		char *cur = g_strdup(s->current);
		ExLocation *l = ex_location_parse(cur);

		if (l) {
			ex_sidebar_set_location(s, l);
			ex_location_unref(l);
		}
		g_free(cur);
	}
}

static gboolean row_sep(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	int kind;

	gtk_tree_model_get(m, it, C_KIND, &kind, -1);
	return kind == K_SEPARATOR;
}

static gboolean can_select(GtkTreeSelection *sel, GtkTreeModel *m,
			   GtkTreePath *p, gboolean was, gpointer data)
{
	GtkTreeIter it;
	int kind = K_ITEM;

	if (gtk_tree_model_get_iter(m, &it, p))
		gtk_tree_model_get(m, &it, C_KIND, &kind, -1);
	return kind == K_ITEM;
}

static void cell_label(GtkTreeViewColumn *c, GtkCellRenderer *r,
		       GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	int kind;
	char *label;

	gtk_tree_model_get(m, it, C_KIND, &kind, C_LABEL, &label, -1);
	g_object_set(r, "text", label, "weight",
		     kind == K_HEADER ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL,
		     NULL);
	g_free(label);
}

static ExLocation *loc_at(ExSidebar *s, GtkTreePath *p)
{
	GtkTreeIter it;
	char *id = NULL;
	int kind = K_SEPARATOR;
	ExLocation *l = NULL;

	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s->store), &it, p))
		return NULL;
	gtk_tree_model_get(GTK_TREE_MODEL(s->store), &it, C_ID, &id, C_KIND,
			   &kind, -1);
	if (kind == K_ITEM && id && *id)
		l = ex_location_parse(id);
	g_free(id);
	return l;
}

static void sel_changed(GtkTreeSelection *sel, ExSidebar *s)
{
	GtkTreeModel *m;
	GtkTreeIter it;

	if (s->rebuilding)
		return;
	if (!gtk_tree_selection_get_selected(sel, &m, &it))
		return;
	GtkTreePath *p = gtk_tree_model_get_path(m, &it);
	ExLocation *l = loc_at(s, p);

	gtk_tree_path_free(p);
	if (l && (!s->current || strcmp(s->current, ex_location_id(l)) != 0))
		g_signal_emit(s, sigs[SIG_OPEN], 0, l, FALSE);
	if (l)
		ex_location_unref(l);
}

static gboolean button(GtkWidget *w, GdkEventButton *ev, ExSidebar *s)
{
	GtkTreePath *p = NULL;
	ExLocation *l;

	if (ev->type != GDK_BUTTON_PRESS || (ev->button != 2 && ev->button != 3))
		return FALSE;
	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(s->tree), (int)ev->x,
					   (int)ev->y, &p, NULL, NULL, NULL))
		return FALSE;
	l = loc_at(s, p);
	gtk_tree_path_free(p);
	if (!l)
		return TRUE;
	if (ev->button == 2)
		g_signal_emit(s, sigs[SIG_OPEN], 0, l, TRUE);
	else
		g_signal_emit(s, sigs[SIG_POPUP], 0, l, ev);
	ex_location_unref(l);
	return TRUE;
}

static gboolean drag_motion(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			    guint time, ExSidebar *s)
{
	GtkTreePath *p = NULL;
	GtkTreeViewDropPosition pos;
	ExLocation *l = NULL;

	if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(s->tree), x, y, &p,
					      &pos))
		l = loc_at(s, p);
	if (l && (ex_location_kind(l) == EX_LOC_FOLDER ||
		  ex_location_kind(l) == EX_LOC_TRASH)) {
		gboolean trash = ex_location_kind(l) == EX_LOC_TRASH;
		char *name = ex_location_display_name(l);
		/* The Trash always takes by moving. */
		GdkDragAction a = ex_file_view_drag_hint(
			ctx, ex_location_file(l), name,
			trash ? GDK_ACTION_MOVE : 0);

		if (!a && !trash && !ex_file_view_dragging(ctx))
			a = gdk_drag_context_get_suggested_action(ctx);
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(s->tree),
						a ? p : NULL,
						GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
		gdk_drag_status(ctx, a, time);
		g_free(name);
	} else {
		(void)ex_file_view_drag_hint(ctx, NULL, NULL, 0);
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(s->tree), NULL, 0);
		gdk_drag_status(ctx, 0, time);
	}
	if (l)
		ex_location_unref(l);
	if (p)
		gtk_tree_path_free(p);
	return TRUE;
}

static void drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time,
		       ExSidebar *s)
{
	(void)ex_file_view_drag_hint(ctx, NULL, NULL, 0);
	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(s->tree), NULL, 0);
}

static gboolean drag_drop(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			  guint time, ExSidebar *s)
{
	gtk_drag_get_data(w, ctx, gdk_atom_intern_static_string("text/uri-list"),
			  time);
	return TRUE;
}

static void drag_received(GtkWidget *w, GdkDragContext *ctx, int x, int y,
			  GtkSelectionData *sd, guint info, guint time,
			  ExSidebar *s)
{
	GtkTreePath *p = NULL;
	GtkTreeViewDropPosition pos;
	char **uris = gtk_selection_data_get_uris(sd);
	ExLocation *l = NULL;

	if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(s->tree), x, y, &p,
					      &pos))
		l = loc_at(s, p);
	if (l && uris && ex_location_file(l)) {
		GPtrArray *a = g_ptr_array_new_with_free_func(g_free);

		for (int i = 0; uris[i]; i++)
			g_ptr_array_add(a, g_strdup(uris[i]));
		g_signal_emit(s, sigs[SIG_DROP], 0, a, ex_location_file(l));
		g_ptr_array_unref(a);
	}
	gtk_drag_finish(ctx, l != NULL, FALSE, time);
	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(s->tree), NULL, 0);
	if (l)
		ex_location_unref(l);
	if (p)
		gtk_tree_path_free(p);
	g_strfreev(uris);
}

static void changed_cb(gpointer src, ExSidebar *s)
{
	rebuild(s);
}

/* "created" / "deleted" carry the item; "rescan" carries nothing. */
static void trash_item(ExMonitor *m, GFile *f, ExSidebar *s)
{
	rebuild(s);
}

static void trash_rescan(ExMonitor *m, ExSidebar *s)
{
	rebuild(s);
}

static void ex_sidebar_dispose(GObject *o)
{
	ExSidebar *s = EX_SIDEBAR(o);

	g_signal_handlers_disconnect_by_data(ex_bookmarks_get_default(), s);
	g_signal_handlers_disconnect_by_data(ex_volumes_get_default(), s);
	g_clear_object(&s->trash_mon);
	G_OBJECT_CLASS(ex_sidebar_parent_class)->dispose(o);
}

static void ex_sidebar_finalize(GObject *o)
{
	ExSidebar *s = EX_SIDEBAR(o);

	g_clear_object(&s->store);
	g_free(s->current);
	G_OBJECT_CLASS(ex_sidebar_parent_class)->finalize(o);
}

static void ex_sidebar_class_init(ExSidebarClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_sidebar_dispose;
	G_OBJECT_CLASS(k)->finalize = ex_sidebar_finalize;
	sigs[SIG_OPEN] = g_signal_new("open", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 2,
				      EX_TYPE_LOCATION, G_TYPE_BOOLEAN);
	sigs[SIG_DROP] = g_signal_new("drop", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 2,
				      G_TYPE_PTR_ARRAY, G_TYPE_FILE);
	sigs[SIG_POPUP] = g_signal_new("popup", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 2,
				       EX_TYPE_LOCATION, G_TYPE_POINTER);
}

static void ex_sidebar_init(ExSidebar *s)
{
	GtkTreeViewColumn *col = gtk_tree_view_column_new();
	GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
	GtkCellRenderer *rt = gtk_cell_renderer_text_new();
	GFile *tf;

	s->store = gtk_tree_store_new(N_C, G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
	s->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(s->tree), FALSE);
	gtk_tree_view_set_row_separator_func(GTK_TREE_VIEW(s->tree), row_sep,
					     NULL, NULL);
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(s->tree), C_TIP);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(s->tree), FALSE);
	gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(s->tree), 0);
	atk_object_set_name(gtk_widget_get_accessible(s->tree),
			    "Navigation pane");
	gtk_tree_view_column_pack_start(col, ri, FALSE);
	gtk_tree_view_column_add_attribute(col, ri, "icon-name", C_ICON);
	g_object_set(ri, "xpad", 4, NULL);
	gtk_tree_view_column_pack_start(col, rt, TRUE);
	gtk_tree_view_column_set_cell_data_func(col, rt, cell_label, NULL, NULL);
	g_object_set(rt, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(s->tree), col);
	gtk_tree_selection_set_select_function(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree)), can_select,
		NULL, NULL);
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree)),
			 "changed", G_CALLBACK(sel_changed), s);
	g_signal_connect(s->tree, "button-press-event", G_CALLBACK(button), s);
	gtk_drag_dest_set(s->tree, 0, drop_targets, 1,
			  GDK_ACTION_COPY | GDK_ACTION_MOVE);
	g_signal_connect(s->tree, "drag-motion", G_CALLBACK(drag_motion), s);
	g_signal_connect(s->tree, "drag-drop", G_CALLBACK(drag_drop), s);
	g_signal_connect(s->tree, "drag-leave", G_CALLBACK(drag_leave), s);
	g_signal_connect(s->tree, "drag-data-received",
			 G_CALLBACK(drag_received), s);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(s), s->tree);
	ex_style_add(GTK_WIDGET(s), EX_CLASS_SIDEBAR);

	g_signal_connect(ex_bookmarks_get_default(), "changed",
			 G_CALLBACK(changed_cb), s);
	g_signal_connect(ex_volumes_get_default(), "changed",
			 G_CALLBACK(changed_cb), s);
	tf = ex_trash_files_dir();
	s->trash_mon = ex_monitor_new(tf);
	g_object_unref(tf);
	g_signal_connect(s->trash_mon, "created", G_CALLBACK(trash_item), s);
	g_signal_connect(s->trash_mon, "deleted", G_CALLBACK(trash_item), s);
	g_signal_connect(s->trash_mon, "rescan", G_CALLBACK(trash_rescan), s);
	rebuild(s);
	gtk_widget_show_all(s->tree);
}

GtkWidget *ex_sidebar_new(void)
{
	return g_object_new(EX_TYPE_SIDEBAR, NULL);
}

typedef struct {
	const char *id;
	GtkTreePath *found;
} Find;

static gboolean find_row(GtkTreeModel *m, GtkTreePath *p, GtkTreeIter *it,
			 gpointer data)
{
	Find *f = data;
	char *id = NULL;

	gtk_tree_model_get(m, it, C_ID, &id, -1);
	if (id && !strcmp(id, f->id))
		f->found = gtk_tree_path_copy(p);
	g_free(id);
	return f->found != NULL;
}

void ex_sidebar_set_location(ExSidebar *s, ExLocation *loc)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree));
	Find f = { ex_location_id(loc), NULL };

	g_free(s->current);
	s->current = g_strdup(ex_location_id(loc));
	gtk_tree_model_foreach(GTK_TREE_MODEL(s->store), find_row, &f);
	s->rebuilding = TRUE;
	if (f.found) {
		gtk_tree_selection_select_path(sel, f.found);
		gtk_tree_path_free(f.found);
	} else {
		gtk_tree_selection_unselect_all(sel);
	}
	s->rebuilding = FALSE;
}
