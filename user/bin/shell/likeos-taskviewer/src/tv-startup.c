// LikeOS Task Viewer -- the Startup apps page.
//
// A LikeOS desktop session is the X session script: xinit runs the user's
// ~/.xinitrc when there is one, /etc/X11/xinit/xinitrc otherwise, and every
// program the session starts is a line of that script -- the helpers run
// once before the desktop appears, the desktop's own programs in the
// background, and the window manager last, whose end is the session's.
// There is no autostart directory or other list the session consults, so
// nothing here can switch a program off: the page lists what the script
// starts, whether each one runs now, and says where to change it.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>
#include <unistd.h>

#include "tv-pages.h"
#include "tv-sysinfo.h"

#define SYSTEM_SESSION "/etc/X11/xinit/xinitrc"

enum {
	COL_NAME,
	COL_PROGRAM,
	COL_DESCRIPTION,
	COL_COMMAND,
	COL_STATUS,
	COL_KIND,
	COL_RUNNING,
	COL_WATCH, /* whether it keeps running (and is looked for) */
	COL_ORDER,
	N_COLS
};

typedef struct {
	GtkListStore *store;
	GtkTreeModel *filter;
	GtkWidget *view;
	GtkWidget *note;
	char *path;
	char *text;
	char *needle;
} Startup;

/* The desktop's own programs in plain words. */
static const struct {
	const char *program, *name;
} known[] = {
	{ "taskbar", "Desktop bar" },
	{ "pcmanfm", "Desktop icons" },
	{ "ctwm", "Window manager" },
	{ "likeos-volume-agent", "USB drive helper" },
	{ "likeos-xkb-apply", "Keyboard layout" },
	{ "likeos-display-settings", "Display resolution" },
	{ "xsetroot", "Desktop background" },
	{ "dbus-launch", "Session message bus" },
};

static Startup *startup_of(GtkWidget *page)
{
	return g_object_get_data(G_OBJECT(page), "tv-startup");
}

static const char *display_name(const char *program)
{
	for (guint i = 0; i < G_N_ELEMENTS(known); i++)
		if (!strcmp(known[i].program, program))
			return known[i].name;
	return program;
}

/* The script xinit runs for this user. */
static char *session_path(void)
{
	char *own = g_build_filename(g_get_home_dir(), ".xinitrc", NULL);

	if (g_file_test(own, G_FILE_TEST_EXISTS))
		return own;
	g_free(own);
	return g_strdup(SYSTEM_SESSION);
}

static const char *kind_words(const TvSessionEntry *e)
{
	if (e->session)
		return "Runs the session";
	if (e->background)
		return "Keeps running";
	return "Runs once at start";
}

static void set_running(GtkListStore *store, GtkTreeIter *it, int pid,
			gboolean watch)
{
	char *t;

	if (!watch)
		t = g_strdup("Done at start");
	else if (pid)
		t = g_strdup_printf("Running (PID %d)", pid);
	else
		t = g_strdup("Not running");
	gtk_list_store_set(store, it, COL_RUNNING, t, -1);
	g_free(t);
}

static void reload(Startup *s)
{
	char *path = session_path();
	char *text = NULL;
	GPtrArray *list;
	GArray *procs;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		text = g_strdup("");
	if (s->text && !strcmp(s->text, text) && !g_strcmp0(s->path, path)) {
		g_free(text);
		g_free(path);
		return;
	}
	g_free(s->text);
	g_free(s->path);
	s->text = text;
	s->path = path;

	list = tv_parse_session_script(text);
	procs = tv_proc_snapshot();
	gtk_list_store_clear(s->store);
	for (guint i = 0; i < list->len; i++) {
		const TvSessionEntry *e = list->pdata[i];
		gboolean watch = e->background || e->session;
		const char *name = display_name(e->name);
		const char *about = e->description ? e->description : "";
		GtkTreeIter it;

		/* A description that only says the name again says
		 * nothing. */
		if (!g_ascii_strcasecmp(about, name))
			about = "";

		gtk_list_store_insert_with_values(
			s->store, &it, -1, COL_NAME, name, COL_PROGRAM,
			e->name, COL_DESCRIPTION, about, COL_COMMAND,
			e->command, COL_STATUS, "Enabled", COL_KIND,
			kind_words(e), COL_WATCH, watch, COL_ORDER, (int)i, -1);
		set_running(s->store, &it,
			    watch ? tv_proc_find(procs, e->name, (int)getuid(),
						 FALSE) :
				    0,
			    watch);
	}
	g_array_unref(procs);
	g_ptr_array_unref(list);

	{
		char *note = g_strdup_printf(
			"Every desktop session starts these programs, in this "
			"order, from %s.  The session has no list of programs "
			"to leave out, so they are changed in that script%s; "
			"changes apply from the next sign-in.",
			s->path,
			strcmp(s->path, SYSTEM_SESSION) ?
				"" :
				" (a copy of it as ~/.xinitrc applies to you "
				"alone)");
		gtk_label_set_text(GTK_LABEL(s->note), note);
		g_free(note);
	}
}

static void refresh_running(Startup *s)
{
	GtkTreeModel *m = GTK_TREE_MODEL(s->store);
	GArray *procs = tv_proc_snapshot();
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);

	for (; ok; ok = gtk_tree_model_iter_next(m, &it)) {
		char *program = NULL;
		gboolean watch = FALSE;

		gtk_tree_model_get(m, &it, COL_PROGRAM, &program, COL_WATCH,
				   &watch, -1);
		if (watch)
			set_running(s->store, &it,
				    tv_proc_find(procs, program, (int)getuid(),
						 FALSE),
				    TRUE);
		g_free(program);
	}
	g_array_unref(procs);
}

static gboolean row_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	Startup *s = data;
	gboolean show = FALSE;
	int cols[] = { COL_NAME, COL_PROGRAM, COL_DESCRIPTION };

	if (!s->needle)
		return TRUE;
	for (guint i = 0; i < G_N_ELEMENTS(cols) && !show; i++) {
		char *v = NULL, *hay;

		gtk_tree_model_get(m, it, cols[i], &v, -1);
		hay = g_utf8_casefold(v ? v : "", -1);
		show = strstr(hay, s->needle) != NULL;
		g_free(hay);
		g_free(v);
	}
	return show;
}

static void add_column(GtkWidget *view, const char *title, int col,
		       int sort_col, gboolean expand)
{
	GtkCellRenderer *cell = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *c;

	/* Only the wide columns give way when the window is narrow. */
	if (expand)
		g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	c = gtk_tree_view_column_new_with_attributes(title, cell, "text", col,
						     NULL);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_sort_column_id(c, sort_col);
	gtk_tree_view_column_set_expand(c, expand);
	gtk_tree_view_column_set_min_width(c, 60);
	gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}

static void startup_free(gpointer data)
{
	Startup *s = data;

	g_object_unref(s->store);
	g_object_unref(s->filter);
	g_free(s->path);
	g_free(s->text);
	g_free(s->needle);
	g_free(s);
}

static GtkWidget *create(void)
{
	Startup *s = g_new0(Startup, 1);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *scroll;
	GtkTreeModel *sorted;

	g_object_set_data_full(G_OBJECT(page), "tv-startup", s, startup_free);
	s->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INT);
	s->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(s->store), NULL);
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(s->filter),
					       row_visible, s, NULL);
	sorted = gtk_tree_model_sort_new_with_model(s->filter);
	/* In the order the session starts them, until a column is
	 * chosen. */
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sorted),
					     COL_ORDER, GTK_SORT_ASCENDING);

	s->view = gtk_tree_view_new_with_model(sorted);
	g_object_unref(sorted);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(s->view), COL_NAME);
	add_column(s->view, "Name", COL_NAME, COL_NAME, FALSE);
	add_column(s->view, "Description", COL_DESCRIPTION, COL_DESCRIPTION,
		   TRUE);
	add_column(s->view, "Status", COL_STATUS, COL_STATUS, FALSE);
	add_column(s->view, "Starts as", COL_KIND, COL_KIND, FALSE);
	add_column(s->view, "Now", COL_RUNNING, COL_RUNNING, FALSE);
	add_column(s->view, "Command", COL_COMMAND, COL_COMMAND, TRUE);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), s->view);
	gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

	s->note = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(s->note), 0);
	gtk_label_set_line_wrap(GTK_LABEL(s->note), TRUE);
	gtk_label_set_selectable(GTK_LABEL(s->note), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(s->note),
				    GTK_STYLE_CLASS_DIM_LABEL);
	gtk_widget_set_margin_start(s->note, 12);
	gtk_widget_set_margin_end(s->note, 12);
	gtk_widget_set_margin_top(s->note, 8);
	gtk_widget_set_margin_bottom(s->note, 8);
	gtk_box_pack_start(GTK_BOX(page), s->note, FALSE, FALSE, 0);

	reload(s);
	gtk_widget_show_all(page);
	return page;
}

static void tick(GtkWidget *page, gboolean visible)
{
	Startup *s = startup_of(page);
	char *old;

	if (!visible)
		return;
	old = g_strdup(s->text);
	reload(s);
	if (!g_strcmp0(old, s->text))
		refresh_running(s);
	g_free(old);
}

static void filter(GtkWidget *page, const char *text)
{
	Startup *s = startup_of(page);

	g_free(s->needle);
	s->needle = text && *text ? g_utf8_casefold(text, -1) : NULL;
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(s->filter));
}

const TvPageDesc tv_page_startup = {
	.id = "startup",
	.title = "Startup apps",
	.icon = "media-playback-start-symbolic",
	.create = create,
	.tick = tick,
	.fill_commands = NULL,
	.filter = filter,
};
