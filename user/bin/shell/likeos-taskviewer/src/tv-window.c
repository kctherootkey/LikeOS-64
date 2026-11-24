// LikeOS Task Viewer -- the window and the program.
//
//   likeos-taskviewer [--page=ID]
//
// One window per session: the program is a GtkApplication, so starting it
// again (the panel's load monitor, Ctrl+Shift+Esc, the menu) brings the
// running window to the front instead of opening a second one.  Without a
// session bus every start is a window of its own, rather than no window.
//
// The window: a navigation rail at the left (a glyph and a name per page,
// narrowed to the glyphs by the button at its top), and to its right a
// header -- the page's title, the search field and the command bar -- over
// the pages, which sit in a stack.  Every page is created once; one timer at
// the chosen update speed ticks all of them, the shown one with
// visible = TRUE (see tv-pages.h).
//
// The size, place and state of the window, the last page, the update
// speed and the other choices are kept in
// $XDG_CONFIG_HOME/likeos-taskviewer/settings.ini.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-app.h"
#include "tv-glyphs.h"

#define APP_ID "org.likeos.TaskViewer"
#define GROUP "Task Viewer"

static const TvPageDesc *const pages[] = {
	&tv_page_processes, &tv_page_performance, &tv_page_users,
	&tv_page_details,   &tv_page_services,	  &tv_page_startup,
};
#define N_PAGES G_N_ELEMENTS(pages)

static const struct {
	const char *id;
	const char *label;
	guint ms;
} speeds[] = {
	{ "high", "_High", 500 },
	{ "normal", "_Normal", 1000 },
	{ "low", "_Low", 4000 },
	{ "paused", "_Paused", 0 },
};

static struct {
	GtkApplication *app;
	GtkWindow *win;
	GtkWidget *rail, *rail_list, *rail_labels[N_PAGES];
	GtkWidget *title, *search, *bar, *stack;
	GtkWidget *page_widgets[N_PAGES];
	int current;
	guint timer;
	guint interval;
	gboolean iconified;
	gboolean maximized;
	int width, height, x, y;
	gboolean have_pos;
	char *start_page;
} W = { .current = -1, .interval = 1000 };

/* ---- settings ------------------------------------------------------------- */

static GKeyFile *kf;

static char *settings_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "likeos-taskviewer",
				"settings.ini", NULL);
}

static GKeyFile *settings(void)
{
	if (!kf) {
		char *path = settings_path();

		kf = g_key_file_new();
		g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
		g_free(path);
	}
	return kf;
}

gboolean tv_settings_get_bool(const char *key, gboolean fallback)
{
	GError *err = NULL;
	gboolean v = g_key_file_get_boolean(settings(), GROUP, key, &err);

	if (err) {
		g_error_free(err);
		return fallback;
	}
	return v;
}

void tv_settings_set_bool(const char *key, gboolean value)
{
	g_key_file_set_boolean(settings(), GROUP, key, value);
}

int tv_settings_get_int(const char *key, int fallback)
{
	GError *err = NULL;
	int v = g_key_file_get_integer(settings(), GROUP, key, &err);

	if (err) {
		g_error_free(err);
		return fallback;
	}
	return v;
}

void tv_settings_set_int(const char *key, int value)
{
	g_key_file_set_integer(settings(), GROUP, key, value);
}

char **tv_settings_get_strv(const char *key)
{
	char **v = g_key_file_get_string_list(settings(), GROUP, key, NULL,
					      NULL);

	return v ? v : g_new0(char *, 1);
}

void tv_settings_set_strv(const char *key, const char *const *value)
{
	g_key_file_set_string_list(settings(), GROUP, key, value,
				   value ? g_strv_length((char **)value) : 0);
}

void tv_settings_save(void)
{
	char *path = settings_path();
	char *dir = g_path_get_dirname(path);
	GError *err = NULL;

	g_mkdir_with_parents(dir, 0700);
	if (!g_key_file_save_to_file(settings(), path, &err)) {
		g_warning("cannot save %s: %s", path, err->message);
		g_error_free(err);
	}
	g_free(dir);
	g_free(path);
}

/* ---- the contract's side ---------------------------------------------------- */

guint tv_update_interval_ms(void)
{
	return W.interval;
}

GtkWindow *tv_main_window(void)
{
	return W.win;
}

/* ---- ticking ---------------------------------------------------------------- */

static gboolean page_on_screen(int i)
{
	return W.win && i == W.current && !W.iconified &&
	       gtk_widget_get_mapped(GTK_WIDGET(W.win));
}

static void tick_all(void)
{
	for (guint i = 0; i < N_PAGES; i++)
		if (pages[i]->tick && W.page_widgets[i])
			pages[i]->tick(W.page_widgets[i], page_on_screen(i));
}

static gboolean on_timer(gpointer data)
{
	tick_all();
	return G_SOURCE_CONTINUE;
}

static void set_interval(guint ms)
{
	if (W.timer) {
		g_source_remove(W.timer);
		W.timer = 0;
	}
	W.interval = ms;
	if (ms)
		W.timer = g_timeout_add(ms, on_timer, NULL);
}

/* Only the page on screen: an extra tick of the others would put an
 * uneven step into the histories they keep. */
void tv_window_refresh_now(void)
{
	tv_procs_sample();
	if (W.current >= 0 && pages[W.current]->tick &&
	    page_on_screen(W.current))
		pages[W.current]->tick(W.page_widgets[W.current], TRUE);
}

/* ---- pages ------------------------------------------------------------------ */

static void bar_clear(void)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(W.bar));

	for (GList *l = kids; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
}

static void show_page(int i)
{
	const TvPageDesc *d;

	if (i < 0 || i >= (int)N_PAGES || i == W.current)
		return;
	/* The page left keeps no filter the field no longer shows. */
	if (W.current >= 0 && pages[W.current]->filter &&
	    *gtk_entry_get_text(GTK_ENTRY(W.search)))
		pages[W.current]->filter(W.page_widgets[W.current], "");
	W.current = i;
	d = pages[i];

	g_signal_handlers_block_matched(W.search, G_SIGNAL_MATCH_DATA, 0, 0,
					NULL, NULL, &W);
	gtk_entry_set_text(GTK_ENTRY(W.search), "");
	g_signal_handlers_unblock_matched(W.search, G_SIGNAL_MATCH_DATA, 0, 0,
					  NULL, NULL, &W);
	gtk_widget_set_visible(W.search, d->filter != NULL);

	gtk_label_set_text(GTK_LABEL(W.title), d->title);
	gtk_stack_set_visible_child_name(GTK_STACK(W.stack), d->id);
	bar_clear();
	if (d->fill_commands)
		d->fill_commands(W.page_widgets[i], W.bar);
	gtk_widget_show_all(W.bar);

	GtkListBoxRow *row =
		gtk_list_box_get_row_at_index(GTK_LIST_BOX(W.rail_list), i);
	if (row && gtk_list_box_get_selected_row(GTK_LIST_BOX(W.rail_list)) !=
			   row)
		gtk_list_box_select_row(GTK_LIST_BOX(W.rail_list), row);

	tv_settings_set_int("page", i);
	g_free(W.start_page);
	W.start_page = NULL;
}

void tv_window_show_page(const char *id)
{
	for (guint i = 0; i < N_PAGES; i++)
		if (!g_strcmp0(pages[i]->id, id))
			show_page((int)i);
}

static void on_rail_selected(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
	if (row)
		show_page(gtk_list_box_row_get_index(row));
}

static void on_search(GtkSearchEntry *e, gpointer data)
{
	if (W.current >= 0 && pages[W.current]->filter)
		pages[W.current]->filter(W.page_widgets[W.current],
					 gtk_entry_get_text(GTK_ENTRY(e)));
}

/* ---- the rail --------------------------------------------------------------- */

static void rail_set_collapsed(gboolean collapsed)
{
	for (guint i = 0; i < N_PAGES; i++)
		gtk_widget_set_visible(W.rail_labels[i], !collapsed);
	tv_settings_set_bool("rail-collapsed", collapsed);
}

static void on_rail_toggle(GtkButton *b, gpointer data)
{
	rail_set_collapsed(gtk_widget_get_visible(W.rail_labels[0]));
}

static GtkWidget *rail_row(guint i)
{
	const TvPageDesc *d = pages[i];
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	TvGlyph g = tv_glyph_for_page(d->id);
	GtkWidget *icon = g ? tv_glyph_new(g, 18) :
			      gtk_image_new_from_icon_name(d->icon,
							   GTK_ICON_SIZE_MENU);
	GtkWidget *label = gtk_label_new(d->title);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(row), box);
	gtk_widget_set_tooltip_text(row, d->title);
	gtk_style_context_add_class(gtk_widget_get_style_context(row),
				    "tv-rail-row");
	W.rail_labels[i] = label;
	return row;
}

static GtkWidget *build_rail(void)
{
	GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *toggle = gtk_button_new();

	gtk_container_add(GTK_CONTAINER(toggle),
			  tv_glyph_new(TV_GLYPH_MENU, 18));
	gtk_button_set_relief(GTK_BUTTON(toggle), GTK_RELIEF_NONE);
	gtk_widget_set_halign(toggle, GTK_ALIGN_START);
	gtk_widget_set_tooltip_text(toggle, "Show or hide the page names");
	gtk_style_context_add_class(gtk_widget_get_style_context(toggle),
				    "tv-rail-toggle");
	g_signal_connect(toggle, "clicked", G_CALLBACK(on_rail_toggle), NULL);
	gtk_box_pack_start(GTK_BOX(rail), toggle, FALSE, FALSE, 0);

	W.rail_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(W.rail_list),
					GTK_SELECTION_BROWSE);
	for (guint i = 0; i < N_PAGES; i++)
		gtk_container_add(GTK_CONTAINER(W.rail_list), rail_row(i));
	g_signal_connect(W.rail_list, "row-selected",
			 G_CALLBACK(on_rail_selected), NULL);
	gtk_box_pack_start(GTK_BOX(rail), W.rail_list, FALSE, FALSE, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(rail),
				    "tv-rail");
	return rail;
}

/* ---- the "..." menu --------------------------------------------------------- */

static void on_speed(GtkCheckMenuItem *item, gpointer data)
{
	guint i = GPOINTER_TO_UINT(data);

	if (!gtk_check_menu_item_get_active(item))
		return;
	set_interval(speeds[i].ms);
	tv_settings_set_int("update-ms", (int)speeds[i].ms);
	tv_settings_save();
}

static void on_on_top(GtkCheckMenuItem *item, gpointer data)
{
	gboolean on = gtk_check_menu_item_get_active(item);

	gtk_window_set_keep_above(W.win, on);
	tv_settings_set_bool("always-on-top", on);
	tv_settings_save();
}

static void on_refresh(gpointer data)
{
	tv_window_refresh_now();
}

static void on_about(gpointer data)
{
	static const char *authors[] = { "The LikeOS Project", NULL };

	gtk_show_about_dialog(
		W.win, "program-name", "Task Viewer", "version", TV_VERSION,
		"comments",
		"The programs and processes running on this computer, "
		"and how much of it they use.",
		"copyright", "Copyright © 2026 The LikeOS Project",
		"license-type", GTK_LICENSE_GPL_2_0_ONLY, "logo-icon-name",
		"likeos-taskviewer", "authors", authors, NULL);
}

static void on_more(GtkButton *button, gpointer data)
{
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *speed_menu = gtk_menu_new();
	GtkWidget *item;
	GSList *radio = NULL;

	tv_menu_add(menu, "_Refresh now", G_CALLBACK(on_refresh), NULL, TRUE);

	item = gtk_menu_item_new_with_mnemonic("_Update speed");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), speed_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	for (guint i = 0; i < G_N_ELEMENTS(speeds); i++) {
		GtkWidget *r = gtk_radio_menu_item_new_with_mnemonic(
			radio, speeds[i].label);

		radio = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(r));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(r),
					       speeds[i].ms == W.interval);
		g_signal_connect(r, "toggled", G_CALLBACK(on_speed),
				 GUINT_TO_POINTER(i));
		gtk_menu_shell_append(GTK_MENU_SHELL(speed_menu), r);
	}

	item = gtk_check_menu_item_new_with_mnemonic("_Always on top");
	gtk_check_menu_item_set_active(
		GTK_CHECK_MENU_ITEM(item),
		tv_settings_get_bool("always-on-top", FALSE));
	g_signal_connect(item, "toggled", G_CALLBACK(on_on_top), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	if (W.current >= 0 && pages[W.current] == &tv_page_processes) {
		tv_menu_separator(menu);
		tv_processes_fill_menu(W.page_widgets[W.current],
				       GTK_MENU_SHELL(menu));
	}

	tv_menu_separator(menu);
	tv_menu_add(menu, "A_bout Task Viewer", G_CALLBACK(on_about), NULL,
		    TRUE);

	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(button), NULL);
	tv_menu_free_when_closed(menu);
	gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(button),
				 GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST,
				 NULL);
}

static void on_run(GtkButton *b, gpointer data)
{
	tv_run_dialog(W.win);
}

/* ---- the header ------------------------------------------------------------- */

static GtkWidget *build_header(void)
{
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *run = tv_command_button(TV_GLYPH_RUN, "_Start new task");
	GtkWidget *more = tv_command_button(TV_GLYPH_MORE, NULL);

	W.title = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(W.title), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(W.title),
				    "tv-title");
	gtk_box_pack_start(GTK_BOX(h), W.title, FALSE, FALSE, 0);

	W.search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(W.search),
				       "Search by name or PID");
	gtk_entry_set_width_chars(GTK_ENTRY(W.search), 28);
	gtk_widget_set_halign(W.search, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(W.search, GTK_ALIGN_CENTER);
	gtk_widget_set_no_show_all(W.search, TRUE);
	g_signal_connect(W.search, "search-changed", G_CALLBACK(on_search),
			 &W);
	gtk_box_set_center_widget(GTK_BOX(h), W.search);

	W.bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_style_context_add_class(gtk_widget_get_style_context(W.bar),
				    "tv-commandbar");

	gtk_widget_set_tooltip_text(run, "Start a program, or open a folder, "
					 "a document or a web address");
	g_signal_connect(run, "clicked", G_CALLBACK(on_run), NULL);
	gtk_widget_set_tooltip_text(more, "More options");
	g_signal_connect(more, "clicked", G_CALLBACK(on_more), NULL);

	gtk_box_pack_end(GTK_BOX(h), more, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(h), W.bar, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(h), run, FALSE, FALSE, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(h),
				    "tv-header");
	return h;
}

/* ---- the look ---------------------------------------------------------------- */

static const char css[] =
	".tv-rail { padding: 6px 4px; min-width: 48px; }\n"
	".tv-rail list { background: transparent; }\n"
	".tv-rail-row { padding: 9px 12px; border-radius: 6px; "
	"  margin: 1px 0; }\n"
	".tv-rail-toggle { padding: 9px 12px; margin-bottom: 4px; }\n"
	".tv-header { padding: 10px 16px 10px 20px; }\n"
	".tv-title { font-size: 130%; font-weight: 600; margin-right: 12px; }\n"
	".tv-command { padding: 4px 10px; }\n"
	".tv-total { font-size: 120%; font-weight: 600; }\n"
	".tv-column-name { font-size: 90%; }\n"
	".tv-properties-title { font-size: 120%; font-weight: 600; }\n"
	".tv-content { border-top-left-radius: 8px; }\n";

static void install_css(void)
{
	GtkCssProvider *p = gtk_css_provider_new();

	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(
		gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

/* ---- the window --------------------------------------------------------------- */

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d)
{
	if (!W.maximized) {
		gtk_window_get_size(W.win, &W.width, &W.height);
		gtk_window_get_position(W.win, &W.x, &W.y);
		W.have_pos = TRUE;
	}
	return FALSE;
}

static gboolean on_state(GtkWidget *w, GdkEventWindowState *e, gpointer d)
{
	W.iconified = (e->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0;
	W.maximized = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
	return FALSE;
}

static void save_window(void)
{
	if (W.width > 0 && W.height > 0) {
		tv_settings_set_int("width", W.width);
		tv_settings_set_int("height", W.height);
	}
	if (W.have_pos) {
		tv_settings_set_int("x", W.x);
		tv_settings_set_int("y", W.y);
	}
	tv_settings_set_bool("maximized", W.maximized);
	tv_settings_save();
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
	save_window();
	return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
	if (W.timer) {
		g_source_remove(W.timer);
		W.timer = 0;
	}
	W.win = NULL;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
	guint mods = e->state & gtk_accelerator_get_default_mod_mask();

	if (e->keyval == GDK_KEY_F5 && !mods) {
		tv_window_refresh_now();
		return TRUE;
	}
	if ((e->keyval == GDK_KEY_f || e->keyval == GDK_KEY_F) &&
	    mods == GDK_CONTROL_MASK && gtk_widget_get_visible(W.search)) {
		gtk_widget_grab_focus(W.search);
		return TRUE;
	}
	if ((e->keyval == GDK_KEY_n || e->keyval == GDK_KEY_N) &&
	    mods == GDK_CONTROL_MASK) {
		tv_run_dialog(W.win);
		return TRUE;
	}
	if (e->keyval == GDK_KEY_Escape && gtk_widget_has_focus(W.search)) {
		gtk_entry_set_text(GTK_ENTRY(W.search), "");
		return TRUE;
	}
	/* Ctrl+1 .. Ctrl+6: the pages. */
	if (mods == GDK_CONTROL_MASK && e->keyval >= GDK_KEY_1 &&
	    e->keyval < GDK_KEY_1 + (int)N_PAGES) {
		show_page((int)(e->keyval - GDK_KEY_1));
		return TRUE;
	}
	return FALSE;
}

static void restore_geometry(void)
{
	int w = tv_settings_get_int("width", 980);
	int h = tv_settings_get_int("height", 680);
	GdkDisplay *disp = gdk_display_get_default();
	GdkMonitor *mon = disp ? gdk_display_get_primary_monitor(disp) : NULL;
	GdkRectangle area = { 0, 0, 0, 0 };
	int sw, sh;

	if (!mon && disp && gdk_display_get_n_monitors(disp) > 0)
		mon = gdk_display_get_monitor(disp, 0);
	if (mon)
		gdk_monitor_get_workarea(mon, &area);
	sw = area.x + area.width;
	sh = area.y + area.height;

	if (sw > 0 && sh > 0) {
		w = CLAMP(w, 480, sw);
		h = CLAMP(h, 360, sh);
	}
	gtk_window_set_default_size(W.win, w, h);
	W.width = w;
	W.height = h;
	if (g_key_file_has_key(settings(), GROUP, "x", NULL)) {
		int x = tv_settings_get_int("x", 0);
		int y = tv_settings_get_int("y", 0);

		/* Only a place on the screen as it is now. */
		if (sw <= 0 || (x > -w / 2 && x < sw - 48 && y >= 0 &&
				y < sh - 48)) {
			gtk_window_move(W.win, x, y);
			W.x = x;
			W.y = y;
			W.have_pos = TRUE;
		}
	}
	if (tv_settings_get_bool("maximized", FALSE))
		gtk_window_maximize(W.win);
}

static int start_page_index(void)
{
	if (W.start_page)
		for (guint i = 0; i < N_PAGES; i++)
			if (!g_strcmp0(pages[i]->id, W.start_page))
				return (int)i;
	return CLAMP(tv_settings_get_int("page", 0), 0, (int)N_PAGES - 1);
}

static void build_window(GtkApplication *app)
{
	GtkWidget *outer, *body, *content;
	int ms;

	install_css();
	W.win = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(W.win, "Task Viewer");
	gtk_window_set_icon_name(W.win, "likeos-taskviewer");
	gtk_window_set_role(W.win, "taskviewer");
	restore_geometry();

	outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	W.rail = build_rail();
	gtk_box_pack_start(GTK_BOX(outer), W.rail, FALSE, FALSE, 0);

	body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(body), build_header(), FALSE, FALSE, 0);
	W.stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(W.stack),
				      GTK_STACK_TRANSITION_TYPE_NONE);
	for (guint i = 0; i < N_PAGES; i++) {
		W.page_widgets[i] = pages[i]->create();
		gtk_stack_add_named(GTK_STACK(W.stack), W.page_widgets[i],
				    pages[i]->id);
	}
	content = W.stack;
	gtk_style_context_add_class(gtk_widget_get_style_context(content),
				    "tv-content");
	gtk_box_pack_start(GTK_BOX(body), content, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(W.win), outer);

	g_signal_connect(W.win, "configure-event", G_CALLBACK(on_configure),
			 NULL);
	g_signal_connect(W.win, "window-state-event", G_CALLBACK(on_state),
			 NULL);
	g_signal_connect(W.win, "delete-event", G_CALLBACK(on_delete), NULL);
	g_signal_connect(W.win, "destroy", G_CALLBACK(on_destroy), NULL);
	g_signal_connect(W.win, "key-press-event", G_CALLBACK(on_key), NULL);

	gtk_widget_show_all(outer);
	rail_set_collapsed(tv_settings_get_bool("rail-collapsed", FALSE));
	if (tv_settings_get_bool("always-on-top", FALSE))
		gtk_window_set_keep_above(W.win, TRUE);

	ms = tv_settings_get_int("update-ms", 1000);
	W.interval = 1000;
	for (guint i = 0; i < G_N_ELEMENTS(speeds); i++)
		if ((int)speeds[i].ms == ms)
			W.interval = speeds[i].ms;
	show_page(start_page_index());
	set_interval(W.interval);
	gtk_window_present(W.win);
}

/* ---- the program ------------------------------------------------------------- */

static void on_activate(GtkApplication *app, gpointer data)
{
	if (W.win) {
		/* Started again: the running window comes forward. */
		if (W.start_page)
			tv_window_show_page(W.start_page);
		gtk_window_deiconify(W.win);
		gtk_window_present_with_time(W.win, GDK_CURRENT_TIME);
		return;
	}
	build_window(app);
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl,
			   gpointer data)
{
	GVariantDict *opts = g_application_command_line_get_options_dict(cl);
	const char *page = NULL;

	if (g_variant_dict_lookup(opts, "page", "&s", &page)) {
		g_free(W.start_page);
		W.start_page = g_strdup(page);
	}
	g_application_activate(app);
	return 0;
}

static int on_local_options(GApplication *app, GVariantDict *opts,
			    gpointer data)
{
	if (g_variant_dict_contains(opts, "version")) {
		g_print("likeos-taskviewer %s\n", TV_VERSION);
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
	const GOptionEntry options[] = {
		{ "page", 'p', 0, G_OPTION_ARG_STRING, NULL,
		  "The page to show: processes, performance, users, details, "
		  "services or startup",
		  "ID" },
		{ "version", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Print the version and exit", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	int status;

	/* No session bus: no single instance, rather than no program. */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;
	g_set_prgname("likeos-taskviewer");
	g_set_application_name("Task Viewer");
	W.app = gtk_application_new(APP_ID, flags);
	g_application_add_main_option_entries(G_APPLICATION(W.app), options);
	g_signal_connect(W.app, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(W.app, "command-line", G_CALLBACK(on_command_line),
			 NULL);
	g_signal_connect(W.app, "handle-local-options",
			 G_CALLBACK(on_local_options), NULL);
	status = g_application_run(G_APPLICATION(W.app), argc, argv);
	g_object_unref(W.app);
	return status;
}
