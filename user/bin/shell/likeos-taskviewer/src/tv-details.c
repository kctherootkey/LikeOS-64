// LikeOS Task Viewer -- the Details page.
//
// Every process in one flat list, with what the kernel reports about it:
// name, PID, state, user, processor share, resident memory, threads,
// priority class and command line.  Sorted by any column.  This is the
// place for the blunt tools: ending a process at once, ending a process
// with everything it started, and the processors it may run on.
//
// The priority class is shown as reported; it is not offered for change,
// because the system has no call that sets the nice level of a process.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-app.h"
#include "tv-glyphs.h"

enum {
	D_PID,
	D_NAME,
	D_ICON,
	D_STATUS,
	D_USER,
	D_CPU,
	D_MEM,
	D_THREADS,
	D_PRIO,
	D_CMD,
	D_N
};

static struct {
	GtkWidget *scroll, *view;
	GtkListStore *store;
	GtkWidget *end_button;
	char *needle;
	guint serial;
	int pid; /* what the context menu acts on */
} D;

static gboolean matches(const TvProc *p)
{
	return tv_search_match(D.needle, p->comm, p->pid) ||
	       tv_search_match(D.needle, p->name, 0) ||
	       tv_search_match(D.needle, p->user, 0);
}

static void set_row(GtkTreeIter *it, const TvProc *p, gboolean fresh)
{
	const char *status = tv_state_name(p->state);
	const char *prio = tv_priority_name(tv_priority_of_nice(p->nice));
	const char *cmd = p->cmdline[0] ? p->cmdline : "";

	if (!fresh) {
		double cpu;
		guint64 mem;
		int threads;
		char *st = NULL, *pr = NULL, *c = NULL;
		gboolean same;

		gtk_tree_model_get(GTK_TREE_MODEL(D.store), it, D_CPU, &cpu,
				   D_MEM, &mem, D_THREADS, &threads, D_STATUS,
				   &st, D_PRIO, &pr, D_CMD, &c, -1);
		same = cpu == p->cpu && mem == p->mem &&
		       threads == p->threads && !g_strcmp0(st, status) &&
		       !g_strcmp0(pr, prio) && !g_strcmp0(c, cmd);
		g_free(st);
		g_free(pr);
		g_free(c);
		if (same)
			return;
		gtk_list_store_set(D.store, it, D_STATUS, status, D_CPU,
				   p->cpu, D_MEM, p->mem, D_THREADS, p->threads,
				   D_PRIO, prio, D_CMD, cmd, -1);
		return;
	}
	gtk_list_store_insert_with_values(
		D.store, it, -1, D_PID, p->pid, D_NAME,
		p->comm[0] ? p->comm : p->name, D_ICON, p->icon, D_STATUS,
		status, D_USER, p->user, D_CPU, p->cpu, D_MEM, p->mem,
		D_THREADS, p->threads, D_PRIO, prio, D_CMD, cmd, -1);
}

static void update_buttons(void)
{
	GtkTreeModel *m;
	GtkTreeIter it;

	if (D.end_button)
		gtk_widget_set_sensitive(
			D.end_button,
			gtk_tree_selection_get_selected(
				gtk_tree_view_get_selection(
					GTK_TREE_VIEW(D.view)),
				&m, &it));
}

static void rebuild(gboolean force)
{
	const TvSnapshot *s = tv_procs_snapshot();
	GtkTreeModel *m = GTK_TREE_MODEL(D.store);
	GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	GtkTreeIter it;
	gboolean more;

	if (!force && s->serial == D.serial) {
		g_hash_table_destroy(seen);
		return;
	}
	D.serial = s->serial;

	/* The rows there are: updated, or gone. */
	more = gtk_tree_model_get_iter_first(m, &it);
	while (more) {
		int pid;
		TvProc *p;

		gtk_tree_model_get(m, &it, D_PID, &pid, -1);
		p = tv_procs_find(pid);
		/* A reused PID is another process: a row keeps the name and
		 * the user it was made with, and is made again when they
		 * change. */
		if (p && matches(p) && !g_hash_table_contains(seen,
							      GINT_TO_POINTER(pid))) {
			char *user = NULL, *name = NULL;
			gboolean same;

			gtk_tree_model_get(m, &it, D_USER, &user, D_NAME, &name,
					   -1);
			same = !g_strcmp0(user, p->user) &&
			       !g_strcmp0(name, p->comm[0] ? p->comm : p->name);
			g_free(user);
			g_free(name);
			if (same) {
				set_row(&it, p, FALSE);
				g_hash_table_add(seen, GINT_TO_POINTER(pid));
				more = gtk_tree_model_iter_next(m, &it);
				continue;
			}
		}
		more = gtk_list_store_remove(D.store, &it);
	}
	/* And the new ones. */
	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *p = s->procs->pdata[i];

		if (g_hash_table_contains(seen, GINT_TO_POINTER(p->pid)) ||
		    !matches(p))
			continue;
		set_row(&it, p, TRUE);
	}
	g_hash_table_destroy(seen);
	gtk_widget_queue_draw(D.view);
	update_buttons();
}

/* ---- cells -------------------------------------------------------------------- */

static void cpu_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
		     GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	double cpu;
	char *s;

	gtk_tree_model_get(m, it, D_CPU, &cpu, -1);
	s = tv_format_percent(cpu);
	g_object_set(r, "text", s, NULL);
	g_free(s);
	tv_cell_heat(r, D.view, cpu / 100.0);
}

static void mem_cell(GtkTreeViewColumn *c, GtkCellRenderer *r,
		     GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	const TvSnapshot *s = tv_procs_snapshot();
	guint64 mem;
	char *t;

	gtk_tree_model_get(m, it, D_MEM, &mem, -1);
	t = tv_format_mem(mem);
	g_object_set(r, "text", t, NULL);
	g_free(t);
	tv_cell_heat(r, D.view, tv_mem_heat_fraction(mem, s->mem_total));
}

static GtkTreeViewColumn *column(const char *title, int sort, int width,
				 gboolean numeric)
{
	GtkTreeViewColumn *c = gtk_tree_view_column_new();

	gtk_tree_view_column_set_title(c, title);
	gtk_tree_view_column_set_sort_column_id(c, sort);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_reorderable(c, TRUE);
	gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
	gtk_tree_view_column_set_fixed_width(c, width);
	if (numeric)
		gtk_tree_view_column_set_alignment(c, 1.0f);
	gtk_tree_view_append_column(GTK_TREE_VIEW(D.view), c);
	return c;
}

static void text_column(const char *title, int col, int width,
			gboolean numeric)
{
	GtkTreeViewColumn *c = column(title, col, width, numeric);
	GtkCellRenderer *r = gtk_cell_renderer_text_new();

	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 6, NULL);
	if (numeric)
		g_object_set(r, "xalign", 1.0f, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_add_attribute(c, r, "text", col);
}

static void func_column(const char *title, int col, int width,
			GtkTreeCellDataFunc f)
{
	GtkTreeViewColumn *c = column(title, col, width, TRUE);
	GtkCellRenderer *r = gtk_cell_renderer_text_new();

	g_object_set(r, "xalign", 1.0f, "xpad", 6, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_set_cell_data_func(c, r, f, NULL, NULL);
}

/* ---- actions ------------------------------------------------------------------ */

static gboolean selected_pid(int *pid)
{
	GtkTreeModel *m;
	GtkTreeIter it;

	if (!gtk_tree_selection_get_selected(
		    gtk_tree_view_get_selection(GTK_TREE_VIEW(D.view)), &m, &it))
		return FALSE;
	gtk_tree_model_get(m, &it, D_PID, pid, -1);
	return TRUE;
}

static void end_selected(void)
{
	int pid;

	if (selected_pid(&pid))
		tv_end_process(pid);
}

static void act_end(gpointer d)
{
	tv_end_process(D.pid);
}

static void act_tree(gpointer d)
{
	tv_end_tree(D.pid);
}

static void act_force(gpointer d)
{
	tv_force_end(D.pid);
}

static void act_affinity(gpointer d)
{
	tv_show_affinity(D.pid);
}

static void act_location(gpointer d)
{
	tv_open_file_location(D.pid);
}

static void act_properties(gpointer d)
{
	tv_show_properties(D.pid);
}

static void act_window(gpointer d)
{
	TvProc *p = tv_procs_find(D.pid);

	if (p && p->wins && p->wins->len)
		tv_win_activate(((TvWin *)p->wins->pdata[0])->xid);
}

static void popup_menu(GdkEvent *event)
{
	GtkWidget *menu;
	TvProc *p;
	gboolean user;

	if (!selected_pid(&D.pid))
		return;
	p = tv_procs_find(D.pid);
	user = p && !p->kernel;
	menu = gtk_menu_new();
	tv_menu_add(menu, "_End task", G_CALLBACK(act_end), NULL, user);
	tv_menu_add(menu, "End process _tree", G_CALLBACK(act_tree), NULL,
		    user);
	tv_menu_add(menu, "_Force end", G_CALLBACK(act_force), NULL, user);
	tv_menu_separator(menu);
	tv_menu_add(menu, "Set _affinity…", G_CALLBACK(act_affinity),
		    NULL, user);
	if (p && p->wins && p->wins->len)
		tv_menu_add(menu, "_Switch to window", G_CALLBACK(act_window),
			    NULL, TRUE);
	tv_menu_separator(menu);
	tv_menu_add(menu, "_Open file location", G_CALLBACK(act_location),
		    NULL, user);
	tv_menu_add(menu, "P_roperties", G_CALLBACK(act_properties), NULL,
		    p != NULL);
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), D.view, NULL);
	tv_menu_free_when_closed(menu);
	if (event)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
	else
		gtk_menu_popup_at_widget(GTK_MENU(menu), D.view,
					 GDK_GRAVITY_CENTER,
					 GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
	GtkTreePath *p = NULL;

	if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(D.view), (int)e->x,
					  (int)e->y, &p, NULL, NULL, NULL)) {
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(D.view), p, NULL, FALSE);
		gtk_tree_path_free(p);
		popup_menu((GdkEvent *)e);
	}
	return TRUE;
}

static gboolean on_popup(GtkWidget *w, gpointer d)
{
	popup_menu(NULL);
	return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
	if (e->keyval == GDK_KEY_Delete || e->keyval == GDK_KEY_KP_Delete) {
		end_selected();
		return TRUE;
	}
	return FALSE;
}

static void on_activated(GtkTreeView *v, GtkTreePath *path,
			 GtkTreeViewColumn *c, gpointer d)
{
	if (selected_pid(&D.pid))
		tv_show_properties(D.pid);
}

static void on_selection(GtkTreeSelection *s, gpointer d)
{
	update_buttons();
}

static void on_sort_changed(GtkTreeSortable *s, gpointer d)
{
	int col;
	GtkSortType order;

	if (gtk_tree_sortable_get_sort_column_id(s, &col, &order))
		tv_settings_set_int("details-sort",
				    col * 2 + (order == GTK_SORT_DESCENDING));
}

void tv_details_select_pid(int pid)
{
	GtkTreeModel *m = GTK_TREE_MODEL(D.store);
	GtkTreeIter it;
	gboolean more;

	tv_window_show_page("details");
	rebuild(TRUE);
	for (more = gtk_tree_model_get_iter_first(m, &it); more;
	     more = gtk_tree_model_iter_next(m, &it)) {
		int p;

		gtk_tree_model_get(m, &it, D_PID, &p, -1);
		if (p == pid) {
			GtkTreePath *path = gtk_tree_model_get_path(m, &it);

			gtk_tree_view_set_cursor(GTK_TREE_VIEW(D.view), path,
						 NULL, FALSE);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(D.view),
						     path, NULL, TRUE, 0.5f,
						     0.0f);
			gtk_tree_path_free(path);
			gtk_widget_grab_focus(D.view);
			break;
		}
	}
}

/* ---- the page ----------------------------------------------------------------- */

static void on_map(GtkWidget *w, gpointer d)
{
	tv_procs_refresh();
	rebuild(TRUE);
}

static GtkWidget *create(void)
{
	GtkTreeViewColumn *c;
	GtkCellRenderer *r;
	int saved;

	D.store = gtk_list_store_new(D_N, G_TYPE_INT, G_TYPE_STRING,
				     G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
				     G_TYPE_DOUBLE, G_TYPE_UINT64, G_TYPE_INT,
				     G_TYPE_STRING, G_TYPE_STRING);
	D.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(D.store));
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(D.view), FALSE);
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(D.view)),
		GTK_SELECTION_BROWSE);

	c = column("Name", D_NAME, 200, FALSE);
	r = gtk_cell_renderer_pixbuf_new();
	g_object_set(r, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 4, NULL);
	gtk_tree_view_column_pack_start(c, r, FALSE);
	gtk_tree_view_column_add_attribute(c, r, "gicon", D_ICON);
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_add_attribute(c, r, "text", D_NAME);

	text_column("PID", D_PID, 70, TRUE);
	text_column("Status", D_STATUS, 90, FALSE);
	text_column("User name", D_USER, 100, FALSE);
	func_column("CPU", D_CPU, 70, cpu_cell);
	func_column("Memory", D_MEM, 110, mem_cell);
	text_column("Threads", D_THREADS, 70, TRUE);
	text_column("Priority", D_PRIO, 100, FALSE);
	text_column("Command line", D_CMD, 400, FALSE);

	saved = tv_settings_get_int("details-sort", D_NAME * 2);
	if (saved < 0 || saved / 2 >= D_N || saved / 2 == D_ICON)
		saved = D_NAME * 2;
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(D.store),
					     saved / 2,
					     saved % 2 ? GTK_SORT_DESCENDING :
							 GTK_SORT_ASCENDING);
	g_signal_connect(D.store, "sort-column-changed",
			 G_CALLBACK(on_sort_changed), NULL);

	g_signal_connect(D.view, "button-press-event", G_CALLBACK(on_button),
			 NULL);
	g_signal_connect(D.view, "popup-menu", G_CALLBACK(on_popup), NULL);
	g_signal_connect(D.view, "key-press-event", G_CALLBACK(on_key), NULL);
	g_signal_connect(D.view, "row-activated", G_CALLBACK(on_activated),
			 NULL);
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(D.view)),
			 "changed", G_CALLBACK(on_selection), NULL);

	D.scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(D.scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(D.scroll), D.view);
	g_signal_connect(D.scroll, "map", G_CALLBACK(on_map), NULL);
	gtk_widget_show_all(D.scroll);
	return D.scroll;
}

static void tick(GtkWidget *page, gboolean visible)
{
	if (!visible)
		return;
	tv_procs_refresh();
	rebuild(FALSE);
}

static void fill_commands(GtkWidget *page, GtkWidget *bar)
{
	GtkWidget *b = tv_command_button(TV_GLYPH_END_TASK, "_End task");

	gtk_widget_set_tooltip_text(b, "End the selected process");
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(end_selected), NULL);
	gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
	D.end_button = b;
	g_object_add_weak_pointer(G_OBJECT(b), (gpointer *)&D.end_button);
	update_buttons();
}

static void filter(GtkWidget *page, const char *text)
{
	g_free(D.needle);
	D.needle = text && *text ? g_utf8_casefold(text, -1) : NULL;
	rebuild(TRUE);
}

const TvPageDesc tv_page_details = {
	.id = "details",
	.title = "Details",
	.icon = "view-list-details",
	.create = create,
	.tick = tick,
	.fill_commands = fill_commands,
	.filter = filter,
};
