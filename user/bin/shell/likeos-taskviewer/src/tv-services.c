// LikeOS Task Viewer -- the Services page.
//
// The services are what init starts from /etc/inittab (/etc/inittab.install
// on the installation medium's setup session): every entry with its
// action, whether it runs now and under which PID, and the entries that
// are commented out, as switched off.  init takes no requests while it
// runs -- it reads the file once at start and answers only the signals that
// shut the machine down -- so the page shows the services and changes
// nothing; the file is where they are changed, for the next start.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-pages.h"
#include "tv-sysinfo.h"

#define INITTAB "/etc/inittab"
#define INITTAB_INSTALL "/etc/inittab.install"

enum {
	COL_NAME,
	COL_PID, /* int, 0 when not running */
	COL_PID_TEXT,
	COL_DESCRIPTION,
	COL_STATUS,
	COL_START, /* the inittab action, in words */
	COL_COMMAND,
	COL_DISABLED, /* commented out */
	N_COLS
};

typedef struct {
	GtkListStore *store;
	GtkTreeModel *filter;
	GtkWidget *view;
	GtkWidget *note;
	char *path; /* the inittab read */
	char *text; /* its contents when last read */
	char *needle; /* the search, folded; NULL for none */
} Services;

static Services *services_of(GtkWidget *page)
{
	return g_object_get_data(G_OBJECT(page), "tv-services");
}

/* The inittab init reads on this boot: the setup session's when the
 * system was started for installing (init then says so to every program
 * in LIKEOS_MODE) and that file is there. */
static const char *inittab_path(void)
{
	if (!g_strcmp0(g_getenv("LIKEOS_MODE"), "install") &&
	    g_file_test(INITTAB_INSTALL, G_FILE_TEST_EXISTS))
		return INITTAB_INSTALL;
	return INITTAB;
}

static const char *start_words(const TvInitEntry *e)
{
	if (e->disabled)
		return "Off (commented out)";
	switch (e->action) {
	case TV_INIT_ONCE:
		return "Once at start";
	case TV_INIT_WAIT:
		return "At start, waited for";
	case TV_INIT_RESPAWN:
		break;
	}
	return "Always (restarted)";
}

static const char *status_words(const TvInitEntry *e, int pid)
{
	if (e->disabled)
		return "Disabled";
	if (pid)
		return "Running";
	return "Stopped";
}

static void set_pid(GtkListStore *store, GtkTreeIter *it, int pid,
		    const char *status)
{
	char buf[16] = "";

	if (pid)
		g_snprintf(buf, sizeof(buf), "%d", pid);
	gtk_list_store_set(store, it, COL_PID, pid, COL_PID_TEXT, buf,
			   COL_STATUS, status, -1);
}

/* Reads the inittab again and rebuilds the list when it changed. */
static void reload(Services *s)
{
	char *text = NULL;
	GPtrArray *list;
	GArray *procs;

	g_free(s->path);
	s->path = g_strdup(inittab_path());
	if (!g_file_get_contents(s->path, &text, NULL, NULL))
		text = g_strdup("");
	if (s->text && !strcmp(s->text, text)) {
		g_free(text);
		return;
	}
	g_free(s->text);
	s->text = text;

	list = tv_parse_inittab(text);
	procs = tv_proc_snapshot();
	gtk_list_store_clear(s->store);
	for (guint i = 0; i < list->len; i++) {
		const TvInitEntry *e = list->pdata[i];
		int pid = e->disabled ? 0 :
					tv_proc_find(procs, e->name, -1, TRUE);
		GtkTreeIter it;

		gtk_list_store_insert_with_values(
			s->store, &it, -1, COL_NAME, e->name, COL_DESCRIPTION,
			e->description ? e->description : "", COL_START,
			start_words(e), COL_COMMAND, e->command,
			COL_DISABLED, e->disabled, -1);
		set_pid(s->store, &it, pid, status_words(e, pid));
	}
	g_array_unref(procs);
	g_ptr_array_unref(list);

	{
		char *note = g_strdup_printf(
			"The programs init starts from %s.  init takes no "
			"requests while the system runs, so services are "
			"started, stopped and switched off in that file; "
			"changes apply from the next start.",
			s->path);
		gtk_label_set_text(GTK_LABEL(s->note), note);
		g_free(note);
	}
}

/* The running state of every row, from a new process list. */
static void refresh_status(Services *s)
{
	GtkTreeModel *m = GTK_TREE_MODEL(s->store);
	GArray *procs = tv_proc_snapshot();
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);

	for (; ok; ok = gtk_tree_model_iter_next(m, &it)) {
		char *name = NULL;
		int old = 0, pid;
		gboolean disabled = FALSE;

		gtk_tree_model_get(m, &it, COL_NAME, &name, COL_DISABLED,
				   &disabled, COL_PID, &old, -1);
		pid = disabled ? 0 : tv_proc_find(procs, name, -1, TRUE);
		if (pid != old)
			set_pid(s->store, &it, pid,
				disabled ? "Disabled" :
				pid	 ? "Running" :
					   "Stopped");
		g_free(name);
	}
	g_array_unref(procs);
}

static gboolean row_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	Services *s = data;
	char *name = NULL, *desc = NULL, *hay;
	gboolean show;

	if (!s->needle)
		return TRUE;
	gtk_tree_model_get(m, it, COL_NAME, &name, COL_DESCRIPTION, &desc, -1);
	hay = g_utf8_casefold(name ? name : "", -1);
	show = strstr(hay, s->needle) != NULL;
	g_free(hay);
	if (!show && desc) {
		hay = g_utf8_casefold(desc, -1);
		show = strstr(hay, s->needle) != NULL;
		g_free(hay);
	}
	g_free(name);
	g_free(desc);
	return show;
}

static void add_column(GtkWidget *view, const char *title, int col,
		       int sort_col, gboolean expand, float xalign)
{
	GtkCellRenderer *cell = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *c;

	g_object_set(cell, "xalign", xalign, NULL);
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

static void services_free(gpointer data)
{
	Services *s = data;

	g_object_unref(s->store);
	g_object_unref(s->filter);
	g_free(s->path);
	g_free(s->text);
	g_free(s->needle);
	g_free(s);
}

static GtkWidget *create(void)
{
	Services *s = g_new0(Services, 1);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *scroll;
	GtkTreeModel *sorted;

	g_object_set_data_full(G_OBJECT(page), "tv-services", s,
			       services_free);
	s->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT,
				      G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_STRING,
				      G_TYPE_STRING, G_TYPE_BOOLEAN);
	s->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(s->store), NULL);
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(s->filter),
					       row_visible, s, NULL);
	sorted = gtk_tree_model_sort_new_with_model(s->filter);

	s->view = gtk_tree_view_new_with_model(sorted);
	g_object_unref(sorted);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(s->view), COL_NAME);
	add_column(s->view, "Name", COL_NAME, COL_NAME, FALSE, 0);
	add_column(s->view, "PID", COL_PID_TEXT, COL_PID, FALSE, 1);
	add_column(s->view, "Description", COL_DESCRIPTION, COL_DESCRIPTION,
		   TRUE, 0);
	add_column(s->view, "Status", COL_STATUS, COL_STATUS, FALSE, 0);
	add_column(s->view, "Starts", COL_START, COL_START, FALSE, 0);
	add_column(s->view, "Command", COL_COMMAND, COL_COMMAND, TRUE, 0);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), s->view);
	gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

	s->note = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(s->note), 0);
	gtk_label_set_line_wrap(GTK_LABEL(s->note), TRUE);
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
	Services *s = services_of(page);
	char *old;

	if (!visible)
		return;
	old = g_strdup(s->text);
	reload(s);
	/* A rebuilt list has its statuses already. */
	if (!g_strcmp0(old, s->text))
		refresh_status(s);
	g_free(old);
}

static void filter(GtkWidget *page, const char *text)
{
	Services *s = services_of(page);

	g_free(s->needle);
	s->needle = text && *text ? g_utf8_casefold(text, -1) : NULL;
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(s->filter));
}

const TvPageDesc tv_page_services = {
	.id = "services",
	.title = "Services",
	.icon = "emblem-system-symbolic",
	.create = create,
	.tick = tick,
	.fill_commands = NULL,
	.filter = filter,
};
