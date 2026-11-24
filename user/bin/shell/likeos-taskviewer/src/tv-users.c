// LikeOS Task Viewer -- the Users page.
//
// One row per user with processes running, with what they take together;
// opened, the user's processes.  The tasks of the system itself are not a
// user's and are left out (the Processes page lists them).
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-app.h"
#include "tv-tree.h"

static struct {
	TvTree *tree;
	char *needle;
	guint serial;
	int pid;
	GIcon *user_icon;
} U;

static gboolean matches(const TvProc *p)
{
	return tv_search_match(U.needle, p->name, p->pid) ||
	       tv_search_match(U.needle, p->user, 0);
}

static GPtrArray *build(const TvSnapshot *s)
{
	GPtrArray *top = tv_nodes_new();
	GHashTable *users = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *p = s->procs->pdata[i];
		TvNode *u, *n;

		if (p->kernel || !matches(p))
			continue;
		u = g_hash_table_lookup(users, GINT_TO_POINTER(p->uid + 1));
		if (!u) {
			u = tv_node_new(TV_ROW_USER,
					g_strdup_printf("u%d", p->uid),
					p->user, U.user_icon);
			u->pid = -1;
			g_hash_table_insert(users, GINT_TO_POINTER(p->uid + 1),
					    u);
			g_ptr_array_add(top, u);
		}
		n = tv_node_new(TV_ROW_PROC, g_strdup_printf("p%d", p->pid),
				p->name, p->icon);
		n->status = tv_state_status(p->state);
		n->cpu = p->cpu;
		n->mem = p->mem;
		n->pid = p->pid;
		tv_node_add(u, n);
		u->cpu += p->cpu;
		u->mem += p->mem;
	}
	for (guint i = 0; i < top->len; i++) {
		TvNode *u = top->pdata[i];
		char *name = g_strdup_printf("%s (%u)", u->name, u->kids->len);

		g_free(u->name);
		u->name = name;
		u->cpu = MIN(u->cpu, 100.0);
	}
	g_hash_table_destroy(users);
	return top;
}

static void rebuild(gboolean force)
{
	const TvSnapshot *s = tv_procs_snapshot();
	GPtrArray *top;

	if (!force && s->serial == U.serial)
		return;
	U.serial = s->serial;
	top = build(s);
	tv_tree_sync(U.tree, top);
	g_ptr_array_unref(top);
	tv_tree_set_totals(U.tree, s);
	gtk_widget_queue_draw(U.tree->view);
}

/* ---- actions ------------------------------------------------------------------ */

static void act_expand(gpointer d)
{
	GtkTreePath *p = tv_tree_selected_path(U.tree);
	GtkTreeView *v = GTK_TREE_VIEW(U.tree->view);

	if (!p)
		return;
	if (gtk_tree_view_row_expanded(v, p))
		gtk_tree_view_collapse_row(v, p);
	else
		gtk_tree_view_expand_row(v, p, FALSE);
	gtk_tree_path_free(p);
}

static void act_end(gpointer d)
{
	TvProc *p = tv_procs_find(U.pid);

	if (!p)
		return;
	if (p->wins && p->wins->len)
		tv_end_app(U.pid);
	else
		tv_end_process(U.pid);
}

static void act_details(gpointer d)
{
	tv_details_select_pid(U.pid);
}

static void act_location(gpointer d)
{
	tv_open_file_location(U.pid);
}

static void act_properties(gpointer d)
{
	tv_show_properties(U.pid);
}

static void popup_menu(GdkEvent *event)
{
	GtkWidget *menu;
	TvRowKind kind;
	TvProc *p;

	if (!tv_tree_selected(U.tree, &kind, &U.pid, NULL))
		return;
	menu = gtk_menu_new();
	if (kind == TV_ROW_USER) {
		GtkTreePath *path = tv_tree_selected_path(U.tree);
		gboolean open = path && gtk_tree_view_row_expanded(
						GTK_TREE_VIEW(U.tree->view),
						path);

		gtk_tree_path_free(path);
		tv_menu_add(menu, open ? "_Collapse" : "E_xpand",
			    G_CALLBACK(act_expand), NULL, TRUE);
	} else {
		p = tv_procs_find(U.pid);
		tv_menu_add(menu, "_End task", G_CALLBACK(act_end), NULL,
			    p != NULL);
		tv_menu_separator(menu);
		tv_menu_add(menu, "_Go to details", G_CALLBACK(act_details),
			    NULL, p != NULL);
		tv_menu_add(menu, "_Open file location",
			    G_CALLBACK(act_location), NULL, p != NULL);
		tv_menu_add(menu, "P_roperties", G_CALLBACK(act_properties),
			    NULL, p != NULL);
	}
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), U.tree->view, NULL);
	tv_menu_free_when_closed(menu);
	if (event)
		gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
	else
		gtk_menu_popup_at_widget(GTK_MENU(menu), U.tree->view,
					 GDK_GRAVITY_CENTER,
					 GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
	if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	if (tv_tree_select_at(U.tree, e->x, e->y))
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
	TvRowKind kind;

	if ((e->keyval == GDK_KEY_Delete || e->keyval == GDK_KEY_KP_Delete) &&
	    tv_tree_selected(U.tree, &kind, &U.pid, NULL) &&
	    kind == TV_ROW_PROC) {
		act_end(NULL);
		return TRUE;
	}
	return FALSE;
}

static void on_activated(GtkTreeView *v, GtkTreePath *path,
			 GtkTreeViewColumn *c, gpointer d)
{
	if (gtk_tree_view_row_expanded(v, path))
		gtk_tree_view_collapse_row(v, path);
	else
		gtk_tree_view_expand_row(v, path, FALSE);
}

/* ---- the page ----------------------------------------------------------------- */

static void on_map(GtkWidget *w, gpointer d)
{
	tv_procs_refresh();
	rebuild(TRUE);
}

static GtkWidget *create(void)
{
	U.user_icon = g_themed_icon_new_with_default_fallbacks("avatar-default");
	U.tree = tv_tree_new("users", "User", FALSE);
	g_signal_connect(U.tree->view, "button-press-event",
			 G_CALLBACK(on_button), NULL);
	g_signal_connect(U.tree->view, "popup-menu", G_CALLBACK(on_popup),
			 NULL);
	g_signal_connect(U.tree->view, "key-press-event", G_CALLBACK(on_key),
			 NULL);
	g_signal_connect(U.tree->view, "row-activated",
			 G_CALLBACK(on_activated), NULL);
	g_signal_connect(U.tree->scroll, "map", G_CALLBACK(on_map), NULL);
	return U.tree->scroll;
}

static void tick(GtkWidget *page, gboolean visible)
{
	if (!visible)
		return;
	tv_procs_refresh();
	rebuild(FALSE);
}

static void filter(GtkWidget *page, const char *text)
{
	g_free(U.needle);
	U.needle = text && *text ? g_utf8_casefold(text, -1) : NULL;
	rebuild(TRUE);
}

const TvPageDesc tv_page_users = {
	.id = "users",
	.title = "Users",
	.icon = "system-users",
	.create = create,
	.tick = tick,
	.filter = filter,
};
