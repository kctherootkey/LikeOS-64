// LikeOS Task Viewer -- the parts the process pages share: ending
// processes and windows, the properties and affinity dialogs, opening a
// program's folder, the tinted load cells and the column headers with
// their totals.
//
// Ending is polite first.  An app's windows are asked to close the way the
// window manager's close button asks, a process gets SIGTERM; either may
// take a moment, or refuse.  A few seconds later, if it is still there,
// the user is asked whether to end it at once (SIGKILL) -- the program had
// its chance to save, and the choice is the user's, not a timer's.
//
// Copyright (C) 2026 The LikeOS Project

#include <signal.h>
#include <string.h>
#include <time.h>

#include "tv-app.h"
#include "tv-glyphs.h"

#define FOLLOW_UP_MS 5000

/* ---- settings shared by the pages ----------------------------------------- */

gboolean tv_mem_percents(void)
{
	return tv_settings_get_bool("memory-percents", FALSE);
}

void tv_set_mem_percents(gboolean on)
{
	tv_settings_set_bool("memory-percents", on);
	tv_settings_save();
	tv_window_refresh_now();
}

/* ---- messages ------------------------------------------------------------- */

void tv_show_error(const char *primary, const GError *error)
{
	GtkWidget *d = gtk_message_dialog_new(
		tv_main_window(), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", primary);

	if (error && error->message)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", error->message);
	gtk_window_set_title(GTK_WINDOW(d), "Task Viewer");
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static gboolean ask(const char *primary, const char *secondary,
		    const char *yes)
{
	GtkWidget *d = gtk_message_dialog_new(
		tv_main_window(), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", primary);
	GtkWidget *b;
	int r;

	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	gtk_window_set_title(GTK_WINDOW(d), "Task Viewer");
	gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
	b = gtk_dialog_add_button(GTK_DIALOG(d), yes, GTK_RESPONSE_ACCEPT);
	gtk_style_context_add_class(gtk_widget_get_style_context(b),
				    "destructive-action");
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_ACCEPT;
}

/* ---- ending --------------------------------------------------------------- */

typedef struct {
	int pid;
	gint64 start;
	gulong xid; /* a window to watch rather than the process, or 0 */
	char *name;
} FollowUp;

static GHashTable *pending; /* "p<pid>" / "w<xid>" -> source id */

static char *pending_key(const FollowUp *f)
{
	return f->xid ? g_strdup_printf("w%lu", f->xid) :
			g_strdup_printf("p%d", f->pid);
}

static void follow_up_free(gpointer data)
{
	FollowUp *f = data;

	g_free(f->name);
	g_free(f);
}

static gboolean still_there(const FollowUp *f)
{
	TvProc *p;

	tv_procs_sample();
	if (f->xid)
		return tv_procs_find_win(f->xid) != NULL;
	p = tv_procs_find(f->pid);
	return p && p->state != TV_STATE_ZOMBIE &&
	       (!f->start || !p->start || ABS(p->start - f->start) <= 1);
}

static gboolean follow_up(gpointer data)
{
	FollowUp *f = data;
	char *key = pending_key(f);
	char *primary;
	GError *err = NULL;
	TvProc *p;

	g_hash_table_remove(pending, key);
	g_free(key);
	if (!still_there(f))
		return G_SOURCE_REMOVE;

	primary = g_strdup_printf("“%s” did not close", f->name);
	if (ask(primary,
		"It was asked to close but is still running.  If you end it "
		"now, any unsaved work in it is lost.",
		"_End now")) {
		/* The process could have gone while the question was up, and
		 * its number been reused: only the same process is ended. */
		if (still_there(f) && (p = tv_procs_find(f->pid)) &&
		    p->state != TV_STATE_ZOMBIE &&
		    !tv_proc_signal(f->pid, SIGKILL, &err)) {
			tv_show_error("The process could not be ended.", err);
			g_clear_error(&err);
		}
		tv_window_refresh_now();
	}
	g_free(primary);
	return G_SOURCE_REMOVE;
}

static void watch(int pid, gulong xid)
{
	TvProc *p = tv_procs_find(pid);
	FollowUp *f = g_new0(FollowUp, 1);
	char *key;
	guint id;

	f->pid = pid;
	f->xid = xid;
	f->start = p ? p->start : 0;
	if (xid) {
		TvWin *w = tv_procs_find_win(xid);

		f->name = g_strdup(w && w->title[0] ? w->title :
				   p		     ? p->name :
							"The window");
	} else {
		f->name = g_strdup(p ? p->name : "The process");
	}
	if (!pending)
		pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						NULL);
	key = pending_key(f);
	if (g_hash_table_contains(pending, key)) {
		g_free(key);
		follow_up_free(f);
		return;
	}
	id = g_timeout_add_full(G_PRIORITY_DEFAULT, FOLLOW_UP_MS, follow_up, f,
				follow_up_free);
	g_hash_table_insert(pending, key, GUINT_TO_POINTER(id));
}

void tv_end_process(int pid)
{
	GError *err = NULL;

	if (!tv_proc_signal(pid, SIGTERM, &err)) {
		tv_show_error("The process could not be ended.", err);
		g_clear_error(&err);
		return;
	}
	watch(pid, 0);
	tv_window_refresh_now();
}

void tv_end_app(int pid)
{
	TvProc *p = tv_procs_find(pid);

	if (!p || !p->wins || !p->wins->len) {
		tv_end_process(pid);
		return;
	}
	for (guint i = 0; i < p->wins->len; i++)
		tv_win_close(((TvWin *)p->wins->pdata[i])->xid);
	watch(pid, 0);
}

void tv_end_window(gulong xid)
{
	TvWin *w = tv_procs_find_win(xid);

	if (!w)
		return;
	tv_win_close(xid);
	watch(w->pid, xid);
}

void tv_force_end(int pid)
{
	TvProc *p = tv_procs_find(pid);
	char *primary;
	GError *err = NULL;

	if (!p)
		return;
	primary = g_strdup_printf("End “%s” (%d) now?", p->name, pid);
	if (ask(primary,
		"The process is ended at once, without the chance to save "
		"its work.  Ending a part of the system can make it "
		"unstable.",
		"_End process")) {
		if (!tv_proc_signal(pid, SIGKILL, &err)) {
			tv_show_error("The process could not be ended.", err);
			g_clear_error(&err);
		}
		tv_window_refresh_now();
	}
	g_free(primary);
}

void tv_end_tree(int pid)
{
	TvProc *p = tv_procs_find(pid);
	GArray *tree;
	char *primary;
	GError *first = NULL;

	if (!p)
		return;
	primary = g_strdup_printf(
		"End “%s” (%d) and every process it started?",
		p->name, pid);
	if (!ask(primary,
		 "The processes are ended at once, without the chance to save "
		 "their work.",
		 "_End process tree")) {
		g_free(primary);
		return;
	}
	g_free(primary);
	tree = tv_procs_descendants(pid);
	g_array_append_val(tree, pid);
	for (guint i = 0; i < tree->len; i++) {
		GError *err = NULL;

		if (!tv_proc_signal(g_array_index(tree, int, i), SIGKILL,
				    &err)) {
			if (!first)
				first = err;
			else
				g_error_free(err);
		}
	}
	g_array_unref(tree);
	if (first) {
		tv_show_error("Not every process of the tree could be ended.",
			      first);
		g_error_free(first);
	}
	tv_window_refresh_now();
}

/* ---- the program's folder --------------------------------------------------- */

void tv_open_file_location(int pid)
{
	char *path = tv_proc_exe_path(pid);
	char *argv[3] = { "likeos-explorer", NULL, NULL };
	GError *err = NULL;

	if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
		GError e = { G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			     (char *)"The process does not say which program "
				     "file it runs, or the file is gone." };

		tv_show_error("The file location is not known.", &e);
		g_free(path);
		return;
	}
	/* Given a file, Explorer opens its folder with the file selected. */
	argv[1] = path;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			   NULL, &err)) {
		tv_show_error("Explorer could not be started.", err);
		g_clear_error(&err);
	}
	g_free(path);
}

/* ---- properties ----------------------------------------------------------- */

static void prop_row(GtkGrid *g, int row, const char *name, const char *value)
{
	GtkWidget *k = gtk_label_new(name);
	GtkWidget *v = gtk_label_new(value && *value ? value : "—");

	gtk_widget_set_halign(k, GTK_ALIGN_START);
	gtk_widget_set_valign(k, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(k),
				    "dim-label");
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 60);
	gtk_widget_set_hexpand(v, TRUE);
	gtk_grid_attach(g, k, 0, row, 1, 1);
	gtk_grid_attach(g, v, 1, row, 1, 1);
}

void tv_show_properties(int pid)
{
	TvProc *p = tv_procs_find(pid);
	GtkWidget *d, *grid, *head, *icon, *title, *box;
	char *path, *num, *started = NULL, *age = NULL, *ppid, *threads;
	int row = 0;

	if (!p)
		return;
	d = gtk_dialog_new_with_buttons("Properties", tv_main_window(),
					GTK_DIALOG_DESTROY_WITH_PARENT,
					"_Close", GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_resizable(GTK_WINDOW(d), TRUE);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 12);

	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	icon = gtk_image_new_from_gicon(p->icon, GTK_ICON_SIZE_DND);
	title = gtk_label_new(p->name);
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class(gtk_widget_get_style_context(title),
				    "tv-properties-title");
	gtk_box_pack_start(GTK_BOX(head), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(head), title, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
			   FALSE, 0);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);

	path = p->kernel ? NULL : tv_proc_exe_path(pid);
	num = g_strdup_printf("%d", p->pid);
	ppid = g_strdup_printf("%d", p->ppid);
	threads = g_strdup_printf("%d", p->threads);
	if (p->start > 0) {
		GDateTime *dt = g_date_time_new_from_unix_local(p->start);
		GDateTime *now = g_date_time_new_now_local();

		if (dt) {
			started = g_date_time_format(dt, "%x %X");
			age = tv_format_duration(
				g_date_time_difference(now, dt) /
				G_TIME_SPAN_SECOND);
			g_date_time_unref(dt);
		}
		g_date_time_unref(now);
	}
	prop_row(GTK_GRID(grid), row++, "Program name",
		 p->comm[0] ? p->comm : p->name);
	prop_row(GTK_GRID(grid), row++, "Location",
		 p->kernel ? "A task of the system itself" : path);
	prop_row(GTK_GRID(grid), row++, "Process ID", num);
	prop_row(GTK_GRID(grid), row++, "Parent process ID", ppid);
	prop_row(GTK_GRID(grid), row++, "User", p->user);
	if (started) {
		char *s = g_strdup_printf("%s  (running for %s)", started, age);

		prop_row(GTK_GRID(grid), row++, "Started", s);
		g_free(s);
	} else {
		prop_row(GTK_GRID(grid), row++, "Started", NULL);
	}
	prop_row(GTK_GRID(grid), row++, "Threads", threads);
	prop_row(GTK_GRID(grid), row++, "Command line",
		 p->cmdline[0] ? p->cmdline :
		 p->kernel     ? NULL :
				 "(only readable by its owner)");

	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show_all(d);
	g_free(path);
	g_free(num);
	g_free(ppid);
	g_free(threads);
	g_free(started);
	g_free(age);
}

/* ---- affinity ------------------------------------------------------------- */

typedef struct {
	GtkWidget *all;
	GPtrArray *boxes;
	gboolean syncing;
} Affinity;

static void affinity_sync(Affinity *a, GtkWidget *dialog)
{
	guint on = 0;

	for (guint i = 0; i < a->boxes->len; i++)
		if (gtk_toggle_button_get_active(a->boxes->pdata[i]))
			on++;
	a->syncing = TRUE;
	gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(a->all),
					   on > 0 && on < a->boxes->len);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->all),
				     on == a->boxes->len);
	a->syncing = FALSE;
	/* A process allowed on no processor at all cannot run. */
	gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_OK,
					  on > 0);
}

static void affinity_one(GtkToggleButton *b, gpointer data)
{
	Affinity *a = data;

	if (!a->syncing)
		affinity_sync(a, gtk_widget_get_toplevel(GTK_WIDGET(b)));
}

static void affinity_all(GtkToggleButton *b, gpointer data)
{
	Affinity *a = data;
	gboolean on = gtk_toggle_button_get_active(b);

	if (a->syncing)
		return;
	a->syncing = TRUE;
	for (guint i = 0; i < a->boxes->len; i++)
		gtk_toggle_button_set_active(a->boxes->pdata[i], on);
	a->syncing = FALSE;
	affinity_sync(a, gtk_widget_get_toplevel(GTK_WIDGET(b)));
}

void tv_show_affinity(int pid)
{
	TvProc *p = tv_procs_find(pid);
	long ncpu = MIN(tv_procs_ncpu(), 64);
	guint64 mask = 0;
	GError *err = NULL;
	GtkWidget *d, *box, *label, *scroll, *flow;
	Affinity a = { NULL, NULL, FALSE };
	char *text;

	if (!p)
		return;
	if (!tv_proc_get_affinity(pid, &mask, &err)) {
		tv_show_error("The processor affinity could not be read.", err);
		g_clear_error(&err);
		return;
	}
	d = gtk_dialog_new_with_buttons("Processor affinity", tv_main_window(),
					GTK_DIALOG_MODAL |
						GTK_DIALOG_DESTROY_WITH_PARENT,
					"_Cancel", GTK_RESPONSE_CANCEL, "_OK",
					GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	text = g_strdup_printf("Which processors may run “%s” (%d)?",
			       p->name, pid);
	label = gtk_label_new(text);
	g_free(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

	a.all = gtk_check_button_new_with_mnemonic("_All processors");
	gtk_box_pack_start(GTK_BOX(box), a.all, FALSE, FALSE, 0);
	a.boxes = g_ptr_array_new();
	flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
	for (long i = 0; i < ncpu; i++) {
		char *n = g_strdup_printf("CPU %ld", i);
		GtkWidget *cb = gtk_check_button_new_with_label(n);

		g_free(n);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb),
					     (mask >> i) & 1);
		g_signal_connect(cb, "toggled", G_CALLBACK(affinity_one), &a);
		g_ptr_array_add(a.boxes, cb);
		gtk_container_add(GTK_CONTAINER(flow), cb);
	}
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
						   320);
	gtk_container_add(GTK_CONTAINER(scroll), flow);
	gtk_widget_set_margin_start(scroll, 20);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	g_signal_connect(a.all, "toggled", G_CALLBACK(affinity_all), &a);
	gtk_widget_show_all(d);
	affinity_sync(&a, d);

	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		guint64 m = 0;

		for (guint i = 0; i < a.boxes->len; i++)
			if (gtk_toggle_button_get_active(a.boxes->pdata[i]))
				m |= (guint64)1 << i;
		if (m && !tv_proc_set_affinity(pid, m, &err)) {
			tv_show_error("The processor affinity could not be set.",
				      err);
			g_clear_error(&err);
		}
	}
	gtk_widget_destroy(d);
	g_ptr_array_unref(a.boxes);
}

/* ---- cells and headers ------------------------------------------------------ */

/* The load tint: Task Viewer's own teal (the processor graph's colour),
 * the same under every theme. */
static void accent_of(GtkWidget *w, GdkRGBA *c)
{
	(void)w;
	c->red = 0.00;
	c->green = 0.58;
	c->blue = 0.56;
	c->alpha = 1.0;
}

void tv_cell_heat(GtkCellRenderer *cell, GtkWidget *view, double fraction)
{
	double alpha = tv_heat_alpha(fraction);
	GdkRGBA c;

	if (alpha <= 0) {
		g_object_set(cell, "cell-background-set", FALSE, NULL);
		return;
	}
	accent_of(view, &c);
	c.alpha = alpha;
	g_object_set(cell, "cell-background-rgba", &c, NULL);
}

GtkWidget *tv_total_header(const char *title, gboolean numeric,
			   GtkWidget **total)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *t = gtk_label_new(numeric ? "" : " ");
	GtkWidget *n = gtk_label_new(title);
	float x = numeric ? 1.0f : 0.0f;

	gtk_label_set_xalign(GTK_LABEL(t), x);
	gtk_label_set_xalign(GTK_LABEL(n), x);
	gtk_style_context_add_class(gtk_widget_get_style_context(t),
				    "tv-total");
	gtk_style_context_add_class(gtk_widget_get_style_context(n),
				    "tv-column-name");
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), n, FALSE, FALSE, 0);
	gtk_widget_show_all(box);
	if (total)
		*total = t;
	return box;
}

/* ---- menus and buttons ------------------------------------------------------ */

GtkWidget *tv_menu_add(GtkWidget *menu, const char *label, GCallback cb,
		       gpointer data, gboolean sensitive)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);

	if (cb)
		g_signal_connect_swapped(item, "activate", cb, data);
	gtk_widget_set_sensitive(item, sensitive);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_widget_show(item);
	return item;
}

GtkWidget *tv_menu_separator(GtkWidget *menu)
{
	GtkWidget *sep = gtk_separator_menu_item_new();

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
	gtk_widget_show(sep);
	return sep;
}

static gboolean menu_free(gpointer menu)
{
	gtk_widget_destroy(menu);
	g_object_unref(menu);
	return G_SOURCE_REMOVE;
}

static void on_menu_closed(GtkMenuShell *menu, gpointer data)
{
	g_idle_add(menu_free, g_object_ref(menu));
}

void tv_menu_free_when_closed(GtkWidget *menu)
{
	g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_closed), NULL);
}

GtkWidget *tv_command_button(int glyph, const char *label)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	if (glyph)
		gtk_box_pack_start(GTK_BOX(box), tv_glyph_new(glyph, 16), FALSE,
				   FALSE, 0);
	if (label)
		gtk_box_pack_start(GTK_BOX(box),
				   gtk_label_new_with_mnemonic(label), FALSE,
				   FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(b),
				    "tv-command");
	gtk_widget_show_all(b);
	return b;
}
