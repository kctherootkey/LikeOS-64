// LikeOS -- likeos-settings, the Settings window.
//
// The way into every part of the system a user can set: a card per area,
// each with its picture, its name, a line on what it covers and, in the
// accent, how it is set now ("1600 x 1000", "German, +1 more").  A click
// opens the area's own window; this one stays open beside it and follows
// what was changed there.  A search field above the cards narrows them to
// the areas whose names or key words match.
//
// The areas are the table `areas' below: adding one is a line there (and,
// when it has a current state worth showing, a function that says it).
//
// Started by the launcher's Settings entry and the Control Center's
// Settings button (taskbar.c).
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>
#include <string.h>

#include "keyboard-conf.h"

#define TITLE "Settings"

/* ---- what each area says it is set to now ---- */

/* The size of the screen the desktop is on, in its own pixels. */
static char *display_state(void)
{
	GdkDisplay *d = gdk_display_get_default();
	GdkMonitor *m = gdk_display_get_primary_monitor(d);
	GdkRectangle r;
	int scale;

	if (!m)
		m = gdk_display_get_monitor(d, 0);
	if (!m)
		return NULL;
	gdk_monitor_get_geometry(m, &r);
	scale = gdk_monitor_get_scale_factor(m);
	return g_strdup_printf("%d \xc3\x97 %d", r.width * scale,
			       r.height * scale);
}

static char *keyboard_state(void)
{
	GPtrArray *cat = kbd_catalogue_load();
	struct kbd_conf c;
	char *s;

	kbd_conf_read(&c);
	s = kbd_conf_summary(cat, &c);
	kbd_conf_clear(&c);
	g_ptr_array_unref(cat);
	return s;
}

static const struct area {
	const char *group;	/* the heading it is listed under */
	const char *icon, *title, *description;
	const char *keywords;	/* more words the search finds it by */
	const char *exec;	/* its window */
	char *(*state)(void);	/* how it is set now, or NULL */
} areas[] = {
	{ "Devices", "preferences-desktop-display", "Display",
	  "Resolution and layout of the screen",
	  "screen monitor resolution size scale",
	  "/usr/bin/likeos-display-settings", display_state },
	{ "Devices", "preferences-desktop-keyboard", "Keyboard",
	  "Layouts and input languages",
	  "keyboard layout language input typing keymap xkb console",
	  "/usr/bin/likeos-keyboard-settings", keyboard_state },
};

#define NAREAS G_N_ELEMENTS(areas)

static struct {
	GtkWidget *win, *search, *empty;
	GtkWidget *state[NAREAS];
	/* One heading and grid of cards per group, in the table's order. */
	struct group {
		const char *name;
		GtkWidget *heading, *flow;
	} group[NAREAS];
	guint ngroups;
	char *needle;
	GFileMonitor *monitor;
} S;

static void add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static GtkWidget *label_new(const char *text, const char *cls)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	add_class(l, cls);
	return l;
}

static void refresh_states(void)
{
	for (guint i = 0; i < NAREAS; i++) {
		char *s;

		if (!areas[i].state)
			continue;
		s = areas[i].state();
		gtk_label_set_text(GTK_LABEL(S.state[i]), s ? s : "");
		g_free(s);
	}
}

/* ---- opening an area ---- */

static void on_card_clicked(GtkButton *b, gpointer data)
{
	const struct area *a = data;
	char *argv[] = { (char *)a->exec, NULL };
	GError *err = NULL;

	(void)b;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			   &err)) {
		GtkWidget *m = gtk_message_dialog_new(
			GTK_WINDOW(S.win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
			GTK_BUTTONS_OK, "%s settings could not be opened.",
			a->title);

		gtk_message_dialog_format_secondary_text(
			GTK_MESSAGE_DIALOG(m), "%s", err->message);
		gtk_window_set_title(GTK_WINDOW(m), TITLE);
		gtk_dialog_run(GTK_DIALOG(m));
		gtk_widget_destroy(m);
		g_clear_error(&err);
	}
}

/* ---- the search ---- */

static gboolean area_matches(const struct area *a)
{
	char *hay, *f;
	gboolean hit;

	if (!S.needle || !*S.needle)
		return TRUE;
	hay = g_strjoin(" ", a->title, a->description, a->keywords, NULL);
	f = g_utf8_casefold(hay, -1);
	hit = strstr(f, S.needle) != NULL;
	g_free(f);
	g_free(hay);
	return hit;
}

static gboolean flow_filter(GtkFlowBoxChild *child, gpointer data)
{
	(void)data;
	return area_matches(g_object_get_data(G_OBJECT(child), "area"));
}

/* A group's heading shows while any of its cards does. */
static void on_search(GtkSearchEntry *e, gpointer data)
{
	int shown = 0;

	(void)data;
	g_free(S.needle);
	S.needle = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(e)), -1);
	for (guint g = 0; g < S.ngroups; g++) {
		int in_group = 0;

		gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(S.group[g].flow));
		for (guint i = 0; i < NAREAS; i++)
			if (strcmp(areas[i].group, S.group[g].name) == 0)
				in_group += area_matches(&areas[i]);
		gtk_widget_set_visible(S.group[g].heading, in_group > 0);
		gtk_widget_set_visible(S.group[g].flow, in_group > 0);
		shown += in_group;
	}
	gtk_widget_set_visible(S.empty, shown == 0);
}

/* Enter in the search opens the first area it shows. */
static void on_search_activate(GtkEntry *e, gpointer data)
{
	(void)e;
	(void)data;
	for (guint i = 0; i < NAREAS; i++)
		if (area_matches(&areas[i])) {
			on_card_clicked(NULL, (gpointer)&areas[i]);
			return;
		}
}

/* Typing anywhere in the window goes to the search. */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	(void)w;
	(void)data;
	if (gtk_widget_has_focus(S.search))
		return FALSE;
	if (ev->keyval == GDK_KEY_Escape)
		return FALSE;
	return gtk_search_entry_handle_event(GTK_SEARCH_ENTRY(S.search),
					     (GdkEvent *)ev);
}

/* ---- following changes ---- */

/* The window comes back to the front -- likely from an area's window:
 * what it shows may have changed there. */
static void on_active(GObject *o, GParamSpec *p, gpointer data)
{
	(void)p;
	(void)data;
	if (gtk_window_is_active(GTK_WINDOW(o)))
		refresh_states();
}

/* A setting in /etc/likeos was replaced (the helpers rename a new file
 * over the old one). */
static void on_conf_changed(GFileMonitor *m, GFile *f, GFile *other,
			    GFileMonitorEvent ev, gpointer data)
{
	(void)m;
	(void)f;
	(void)other;
	(void)data;
	if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
	    ev == G_FILE_MONITOR_EVENT_CREATED ||
	    ev == G_FILE_MONITOR_EVENT_RENAMED ||
	    ev == G_FILE_MONITOR_EVENT_MOVED_IN)
		refresh_states();
}

/* ---- the window ---- */

/* One area's card: the picture, then the name, the line on what it covers
 * and its state, and a chevron saying it opens something. */
static GtkWidget *area_card(guint i)
{
	const struct area *a = &areas[i];
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *icon = gtk_image_new_from_icon_name(a->icon,
						       GTK_ICON_SIZE_DIALOG);
	GtkWidget *chev = gtk_image_new_from_icon_name("pan-end-symbolic",
						       GTK_ICON_SIZE_MENU);
	GtkWidget *desc;

	add_class(b, "st-hub-card");
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
	gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(col), label_new(a->title, "st-card-title"),
			   FALSE, FALSE, 0);
	/* Lines of a bounded length: the grid puts as many cards on a line
	 * as fit their natural width. */
	desc = label_new(a->description, "st-card-desc");
	gtk_label_set_ellipsize(GTK_LABEL(desc), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(desc), 30);
	gtk_box_pack_start(GTK_BOX(col), desc, FALSE, FALSE, 0);
	S.state[i] = label_new("", "st-card-state");
	gtk_label_set_ellipsize(GTK_LABEL(S.state[i]), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(S.state[i]), 30);
	gtk_box_pack_start(GTK_BOX(col), S.state[i], FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);

	add_class(chev, "st-card-chevron");
	gtk_widget_set_valign(chev, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(row), chev, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_widget_set_tooltip_text(b, a->description);
	g_signal_connect(b, "clicked", G_CALLBACK(on_card_clicked),
			 (gpointer)a);
	return b;
}

static void build_window(void)
{
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *htext = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *logo;

	S.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(S.win), TITLE);
	gtk_window_set_icon_name(GTK_WINDOW(S.win), "preferences-system");
	gtk_window_set_position(GTK_WINDOW(S.win), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(S.win), 820, 440);
	add_class(S.win, "st-window");
	add_class(S.win, "st-hub");

	/* The header: the picture, "Settings" and the search. */
	add_class(header, "st-hub-header");
	logo = gtk_image_new_from_icon_name("preferences-system",
					    GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(logo), 48);
	gtk_widget_set_valign(logo, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(header), logo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(htext), label_new("Settings", "st-hub-title"),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(htext),
			   label_new("Set up LikeOS the way you work",
				     "st-subtitle"),
			   FALSE, FALSE, 0);
	gtk_widget_set_valign(htext, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(header), htext, TRUE, TRUE, 0);
	S.search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(S.search), "Find a setting");
	add_class(S.search, "st-search");
	gtk_widget_set_size_request(S.search, 260, -1);
	gtk_widget_set_valign(S.search, GTK_ALIGN_CENTER);
	g_signal_connect(S.search, "search-changed", G_CALLBACK(on_search),
			 NULL);
	g_signal_connect(S.search, "activate", G_CALLBACK(on_search_activate),
			 NULL);
	gtk_box_pack_end(GTK_BOX(header), S.search, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

	/* The cards under their groups' headings, two to a line where the
	 * window is wide enough. */
	add_class(page, "st-hub-page");
	for (guint i = 0; i < NAREAS; i++) {
		struct group *g = NULL;
		GtkWidget *child = gtk_flow_box_child_new();

		for (guint k = 0; k < S.ngroups && !g; k++)
			if (strcmp(S.group[k].name, areas[i].group) == 0)
				g = &S.group[k];
		if (!g) {
			g = &S.group[S.ngroups++];
			g->name = areas[i].group;
			g->heading = label_new(g->name, "st-heading");
			g->flow = gtk_flow_box_new();
			add_class(g->flow, "st-hub-grid");
			gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(g->flow),
							GTK_SELECTION_NONE);
			gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(g->flow),
						     TRUE);
			gtk_flow_box_set_min_children_per_line(
				GTK_FLOW_BOX(g->flow), 1);
			gtk_flow_box_set_max_children_per_line(
				GTK_FLOW_BOX(g->flow), 2);
			gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(g->flow),
							12);
			gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(g->flow), 12);
			gtk_flow_box_set_filter_func(GTK_FLOW_BOX(g->flow),
						     flow_filter, NULL, NULL);
			gtk_box_pack_start(GTK_BOX(page), g->heading, FALSE,
					   FALSE, 0);
			gtk_box_pack_start(GTK_BOX(page), g->flow, FALSE,
					   FALSE, 0);
		}
		gtk_widget_set_can_focus(child, FALSE);
		g_object_set_data(G_OBJECT(child), "area", (gpointer)&areas[i]);
		gtk_container_add(GTK_CONTAINER(child), area_card(i));
		gtk_container_add(GTK_CONTAINER(g->flow), child);
	}
	S.empty = label_new("No setting matches the search", "st-empty");
	gtk_label_set_xalign(GTK_LABEL(S.empty), 0.5);
	gtk_widget_set_no_show_all(S.empty, TRUE);
	gtk_box_pack_start(GTK_BOX(page), S.empty, FALSE, FALSE, 0);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), page);
	gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(S.win), outer);

	g_signal_connect(S.win, "key-press-event", G_CALLBACK(on_key), NULL);
	g_signal_connect(S.win, "notify::is-active", G_CALLBACK(on_active),
			 NULL);
	g_signal_connect(S.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

int main(int argc, char **argv)
{
	GFile *dir;

	gtk_init(&argc, &argv);
	build_window();
	refresh_states();

	dir = g_file_new_for_path(KBD_CONF_DIR);
	S.monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
					     NULL, NULL);
	if (S.monitor)
		g_signal_connect(S.monitor, "changed",
				 G_CALLBACK(on_conf_changed), NULL);
	g_object_unref(dir);

	gtk_widget_show_all(S.win);
	gtk_window_set_focus(GTK_WINDOW(S.win), NULL);
	gtk_main();
	return 0;
}
