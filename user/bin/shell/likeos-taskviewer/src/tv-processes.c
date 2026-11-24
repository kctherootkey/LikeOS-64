// LikeOS Task Viewer -- the Processes page.
//
// Everything running, in three groups:
//
//   Apps                  the programs with a window on the desktop.  An
//                         app's row adds up its own process and every
//                         process it started that has no window of its
//                         own (a browser's web processes, a terminal's
//                         shell); opened, it lists its windows and, when
//                         there is more than one, those processes.
//   Background processes  every other process of the users.
//   System                the tasks of the system itself.
//
// With Group by type off the same rows stand in one list.  The column
// headers carry the totals: the share of the processors in use and of the
// memory.  The CPU and memory cells are tinted with the accent colour, the
// stronger the more a process takes.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-app.h"
#include "tv-glyphs.h"
#include "tv-tree.h"

static struct {
	TvTree *tree;
	GtkWidget *end_button;
	char *needle; /* case-folded search, or NULL */
	guint serial; /* the snapshot shown */
	/* What a context menu acts on. */
	TvRowKind kind;
	int pid;
	gulong xid;
} P;

static gboolean grouped(void)
{
	return tv_settings_get_bool("group-by-type", TRUE);
}

/* ---- the rows ----------------------------------------------------------------- */

static TvNode *proc_node(const TvProc *p, const char *name)
{
	TvNode *n = tv_node_new(TV_ROW_PROC, g_strdup_printf("p%d", p->pid),
				name ? name : p->name, p->icon);

	n->status = tv_state_status(p->state);
	n->cpu = p->cpu;
	n->mem = p->mem;
	n->pid = p->pid;
	return n;
}

static gboolean proc_matches(const TvProc *p)
{
	return tv_search_match(P.needle, p->name, p->pid) ||
	       (p->comm && tv_search_match(P.needle, p->comm, 0));
}

static TvNode *app_node(const TvSnapshot *s, TvProc *main)
{
	GPtrArray *members = g_ptr_array_new();
	gboolean match = proc_matches(main);
	TvNode *n;
	char *name;

	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *q = s->procs->pdata[i];

		if (q->owner_pid != main->pid)
			continue;
		g_ptr_array_add(members, q);
		if (!match && proc_matches(q))
			match = TRUE;
	}
	for (guint i = 0; !match && main->wins && i < main->wins->len; i++)
		if (tv_search_match(P.needle,
				    ((TvWin *)main->wins->pdata[i])->title, 0))
			match = TRUE;
	if (!match) {
		g_ptr_array_unref(members);
		return NULL;
	}

	name = members->len > 1 ?
		       g_strdup_printf("%s (%u)", main->name, members->len) :
		       g_strdup(main->name);
	n = tv_node_new(TV_ROW_APP, g_strdup_printf("a%d", main->pid), name,
			main->icon);
	g_free(name);
	n->pid = main->pid;
	n->status = tv_state_status(main->state);
	for (guint i = 0; i < members->len; i++) {
		TvProc *q = members->pdata[i];

		n->cpu += q->cpu;
		n->mem += q->mem;
	}
	n->cpu = MIN(n->cpu, 100.0);

	for (guint i = 0; main->wins && i < main->wins->len; i++) {
		TvWin *w = main->wins->pdata[i];
		TvNode *k = tv_node_new(TV_ROW_WIN,
					g_strdup_printf("w%lu", w->xid),
					w->title[0] ? w->title : main->name,
					main->icon);

		k->cpu = -1;
		k->pid = main->pid;
		k->xid = w->xid;
		tv_node_add(n, k);
	}
	if (members->len > 1)
		for (guint i = 0; i < members->len; i++) {
			TvProc *q = members->pdata[i];

			tv_node_add(n, proc_node(q, q->comm[0] ? q->comm :
								   q->name));
		}
	g_ptr_array_unref(members);
	return n;
}

static GPtrArray *build(const TvSnapshot *s)
{
	GPtrArray *top = tv_nodes_new();
	GPtrArray *in[TV_GROUP_COUNT];

	for (int g = 0; g < TV_GROUP_COUNT; g++)
		in[g] = g_ptr_array_new();

	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *p = s->procs->pdata[i];
		TvNode *n = NULL;

		if (p->group == TV_GROUP_APPS) {
			if (p->owner_pid == p->pid)
				n = app_node(s, p);
		} else if (proc_matches(p)) {
			n = proc_node(p, NULL);
		}
		if (n)
			g_ptr_array_add(in[p->group], n);
	}

	for (int g = 0; g < TV_GROUP_COUNT; g++) {
		if (!grouped()) {
			for (guint i = 0; i < in[g]->len; i++)
				g_ptr_array_add(top, in[g]->pdata[i]);
		} else if (in[g]->len) {
			char *title = g_strdup_printf("%s (%u)",
						      tv_group_title(g),
						      in[g]->len);
			TvNode *grp = tv_node_new(TV_ROW_GROUP,
						  g_strdup_printf("g%d", g),
						  title, NULL);

			g_free(title);
			grp->cpu = -1;
			grp->order = g;
			grp->expand = TRUE;
			for (guint i = 0; i < in[g]->len; i++)
				tv_node_add(grp, in[g]->pdata[i]);
			g_ptr_array_add(top, grp);
		} else {
			/* An empty group shows no heading. */
		}
		g_ptr_array_unref(in[g]);
	}
	return top;
}

static void update_buttons(void)
{
	TvRowKind kind;

	if (P.end_button)
		gtk_widget_set_sensitive(
			P.end_button,
			tv_tree_selected(P.tree, &kind, NULL, NULL) &&
				kind != TV_ROW_GROUP);
}

static void rebuild(gboolean force)
{
	const TvSnapshot *s = tv_procs_snapshot();
	GPtrArray *top;

	if (!force && s->serial == P.serial)
		return;
	P.serial = s->serial;
	top = build(s);
	tv_tree_sync(P.tree, top);
	g_ptr_array_unref(top);
	tv_tree_set_totals(P.tree, s);
	/* The formatting depends on settings the rows do not carry. */
	gtk_widget_queue_draw(P.tree->view);
	update_buttons();
}

/* ---- actions ------------------------------------------------------------------ */

static void end_selected(void)
{
	TvRowKind kind;
	int pid;
	gulong xid;

	if (!tv_tree_selected(P.tree, &kind, &pid, &xid))
		return;
	switch (kind) {
	case TV_ROW_APP:
		tv_end_app(pid);
		break;
	case TV_ROW_WIN:
		tv_end_window(xid);
		break;
	case TV_ROW_PROC:
		tv_end_process(pid);
		break;
	default:
		break;
	}
}

static void act_end(gpointer d)
{
	end_selected();
}

static gulong target_window(void)
{
	TvProc *p;

	if (P.xid)
		return P.xid;
	p = tv_procs_find(P.pid);
	return p && p->wins && p->wins->len ?
		       ((TvWin *)p->wins->pdata[0])->xid :
		       0;
}

static void act_switch(gpointer d)
{
	gulong x = target_window();

	if (x)
		tv_win_activate(x);
}

static void act_raise(gpointer d)
{
	gulong x = target_window();

	if (x)
		tv_win_raise(x);
}

static void act_minimize(gpointer d)
{
	gulong x = target_window();

	if (x)
		tv_win_minimize(x);
}

static void act_maximize(gpointer d)
{
	gulong x = target_window();

	if (x)
		tv_win_maximize(x);
}

static void act_expand(gpointer d)
{
	GtkTreePath *p = tv_tree_selected_path(P.tree);
	GtkTreeView *v = GTK_TREE_VIEW(P.tree->view);

	if (!p)
		return;
	if (gtk_tree_view_row_expanded(v, p))
		gtk_tree_view_collapse_row(v, p);
	else
		gtk_tree_view_expand_row(v, p, FALSE);
	gtk_tree_path_free(p);
}

static void act_details(gpointer d)
{
	tv_details_select_pid(P.pid);
}

static void act_location(gpointer d)
{
	tv_open_file_location(P.pid);
}

static void act_properties(gpointer d)
{
	tv_show_properties(P.pid);
}

static void act_mem_mode(GtkCheckMenuItem *item, gpointer data)
{
	if (gtk_check_menu_item_get_active(item))
		tv_set_mem_percents(GPOINTER_TO_INT(data));
}

/* Resource values > Memory > Values / Percents. */
static void add_resource_values(GtkWidget *menu)
{
	GtkWidget *rv = gtk_menu_item_new_with_mnemonic("_Resource values");
	GtkWidget *rv_menu = gtk_menu_new();
	GtkWidget *mem = gtk_menu_item_new_with_mnemonic("_Memory");
	GtkWidget *mem_menu = gtk_menu_new();
	GtkWidget *values, *percents;

	values = gtk_radio_menu_item_new_with_mnemonic(NULL, "_Values");
	percents = gtk_radio_menu_item_new_with_mnemonic_from_widget(
		GTK_RADIO_MENU_ITEM(values), "_Percents");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(percents),
				       tv_mem_percents());
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(values),
				       !tv_mem_percents());
	g_signal_connect(values, "toggled", G_CALLBACK(act_mem_mode),
			 GINT_TO_POINTER(FALSE));
	g_signal_connect(percents, "toggled", G_CALLBACK(act_mem_mode),
			 GINT_TO_POINTER(TRUE));
	gtk_menu_shell_append(GTK_MENU_SHELL(mem_menu), values);
	gtk_menu_shell_append(GTK_MENU_SHELL(mem_menu), percents);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(mem), mem_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(rv_menu), mem);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(rv), rv_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), rv);
	gtk_widget_show_all(rv);
}

static void popup_menu(GdkEvent *event)
{
	GtkWidget *menu;
	TvProc *p;
	gboolean has_win, kids;

	if (!tv_tree_selected(P.tree, &P.kind, &P.pid, &P.xid))
		return;
	p = tv_procs_find(P.pid);
	has_win = P.kind == TV_ROW_WIN ||
		  (P.kind == TV_ROW_APP && p && p->wins && p->wins->len);
	kids = tv_tree_selected_has_kids(P.tree);
	menu = gtk_menu_new();

	if (kids) {
		GtkTreePath *path = tv_tree_selected_path(P.tree);
		gboolean open = path && gtk_tree_view_row_expanded(
						GTK_TREE_VIEW(P.tree->view),
						path);

		gtk_tree_path_free(path);
		tv_menu_add(menu, open ? "_Collapse" : "E_xpand",
			    G_CALLBACK(act_expand), NULL, TRUE);
	}
	if (P.kind == TV_ROW_GROUP)
		goto show;
	if (has_win) {
		if (kids)
			tv_menu_separator(menu);
		tv_menu_add(menu, "_Switch to", G_CALLBACK(act_switch), NULL,
			    TRUE);
		tv_menu_add(menu, "_Bring to front", G_CALLBACK(act_raise),
			    NULL, TRUE);
		tv_menu_add(menu, "Mi_nimize", G_CALLBACK(act_minimize), NULL,
			    TRUE);
		tv_menu_add(menu, "Ma_ximize", G_CALLBACK(act_maximize), NULL,
			    TRUE);
	}
	if (kids || has_win)
		tv_menu_separator(menu);
	tv_menu_add(menu, "_End task", G_CALLBACK(act_end), NULL,
		    !(p && p->kernel));
	tv_menu_separator(menu);
	add_resource_values(menu);
	tv_menu_separator(menu);
	tv_menu_add(menu, "_Go to details", G_CALLBACK(act_details), NULL,
		    p != NULL);
	tv_menu_add(menu, "_Open file location", G_CALLBACK(act_location),
		    NULL, p && !p->kernel);
	tv_menu_add(menu, "P_roperties", G_CALLBACK(act_properties), NULL,
		    p != NULL);
show:
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), P.tree->view, NULL);
	tv_menu_free_when_closed(menu);
	if (event)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
	else
		gtk_menu_popup_at_widget(GTK_MENU(menu), P.tree->view,
					 GDK_GRAVITY_CENTER,
					 GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
	if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (!tv_tree_select_at(P.tree, e->x, e->y))
		return TRUE;
	popup_menu((GdkEvent *)e);
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
	if (!tv_tree_selected(P.tree, &P.kind, &P.pid, &P.xid))
		return;
	if (P.kind == TV_ROW_WIN)
		act_switch(NULL);
	else if (gtk_tree_view_row_expanded(v, path))
		gtk_tree_view_collapse_row(v, path);
	else
		gtk_tree_view_expand_row(v, path, FALSE);
}

static void on_selection(GtkTreeSelection *s, gpointer d)
{
	update_buttons();
}

/* ---- the "..." menu's view options -------------------------------------------- */

static void act_group(GtkCheckMenuItem *item, gpointer d)
{
	tv_settings_set_bool("group-by-type",
			     gtk_check_menu_item_get_active(item));
	tv_settings_save();
	tv_tree_clear(P.tree);
	rebuild(TRUE);
}

static void act_expand_all(gpointer d)
{
	gtk_tree_view_expand_all(GTK_TREE_VIEW(P.tree->view));
}

static void act_collapse_all(gpointer d)
{
	gtk_tree_view_collapse_all(GTK_TREE_VIEW(P.tree->view));
	/* The headings stay open: collapsed, the page would be empty. */
	tv_tree_expand_groups(P.tree);
}

void tv_processes_fill_menu(GtkWidget *page, GtkMenuShell *menu)
{
	GtkWidget *item = gtk_check_menu_item_new_with_mnemonic("_Group by type");

	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), grouped());
	g_signal_connect(item, "toggled", G_CALLBACK(act_group), NULL);
	gtk_menu_shell_append(menu, item);
	tv_menu_add(GTK_WIDGET(menu), "E_xpand all", G_CALLBACK(act_expand_all),
		    NULL, TRUE);
	tv_menu_add(GTK_WIDGET(menu), "_Collapse all",
		    G_CALLBACK(act_collapse_all), NULL, TRUE);
}

/* ---- the page ----------------------------------------------------------------- */

static void on_map(GtkWidget *w, gpointer d)
{
	tv_procs_refresh();
	rebuild(TRUE);
}

static GtkWidget *create(void)
{
	P.tree = tv_tree_new("processes", "Name", TRUE);
	g_signal_connect(P.tree->view, "button-press-event",
			 G_CALLBACK(on_button), NULL);
	g_signal_connect(P.tree->view, "popup-menu", G_CALLBACK(on_popup),
			 NULL);
	g_signal_connect(P.tree->view, "key-press-event", G_CALLBACK(on_key),
			 NULL);
	g_signal_connect(P.tree->view, "row-activated",
			 G_CALLBACK(on_activated), NULL);
	g_signal_connect(gtk_tree_view_get_selection(
				 GTK_TREE_VIEW(P.tree->view)),
			 "changed", G_CALLBACK(on_selection), NULL);
	g_signal_connect(P.tree->scroll, "map", G_CALLBACK(on_map), NULL);
	return P.tree->scroll;
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

	gtk_widget_set_tooltip_text(b, "End the selected app or process");
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(end_selected), NULL);
	gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
	P.end_button = b;
	g_object_add_weak_pointer(G_OBJECT(b), (gpointer *)&P.end_button);
	update_buttons();
}

static void filter(GtkWidget *page, const char *text)
{
	g_free(P.needle);
	P.needle = text && *text ? g_utf8_casefold(text, -1) : NULL;
	rebuild(TRUE);
}

const TvPageDesc tv_page_processes = {
	.id = "processes",
	.title = "Processes",
	.icon = "utilities-system-monitor",
	.create = create,
	.tick = tick,
	.fill_commands = fill_commands,
	.filter = filter,
};
