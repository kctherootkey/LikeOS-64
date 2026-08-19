// LikeOS-64 -- load, the standalone load and memory monitor.
//
// The same two panes the taskbar shows in its tray, in a window of their own.
// All the drawing and sampling is in panel.c, so this and the panel cannot
// disagree about what the machine is doing; what is here is the window, the
// timer and the geometry.
//
// Since taskbar(1) took over the desktop bar this is no longer started by
// xinitrc.  It is kept because it is useful on its own -- run it from a
// terminal to watch the load while something is building.

#include "panel.h"

static struct panel_load g_mon;

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	(void)data;
	panel_load_draw(cr, &g_mon, 0, 0,
			gtk_widget_get_allocated_width(widget),
			gtk_widget_get_allocated_height(widget));
	return TRUE;
}

static gboolean on_tick(gpointer widget)
{
	panel_load_sample(&g_mon);
	gtk_widget_queue_draw(GTK_WIDGET(widget));
	return G_SOURCE_CONTINUE;
}

/* Window size and position, from a -geometry argument in the X style.
 *
 * gtk_window_parse_geometry() rather than gtk_window_move()/set_default_size(),
 * and the difference is load-bearing under this window manager.  ctwm places a
 * window interactively -- it hands the pointer a rubber-band outline and waits
 * for a click -- unless the window says its position was chosen by the USER.
 * That is the USPosition hint, and parse_geometry is the one call that sets it;
 * gtk_window_move() sets PPosition instead, which ctwm ignores by default.
 *
 * Deprecated since GTK 3.20 with no replacement that sets USPosition, so the
 * warning is silenced here rather than worked around: the alternative is
 * setting the hint through raw Xlib after realisation, which is more code doing
 * the same thing less clearly. */
static void apply_geometry(GtkWindow *win, int argc, char **argv)
{
	const char *geom = NULL;

	for (int i = 1; i < argc; i++) {
		if (g_strcmp0(argv[i], "-geometry") == 0 && i + 1 < argc)
			geom = argv[++i];
	}
	if (!geom)
		return;

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_window_parse_geometry(win, geom);
	G_GNUC_END_IGNORE_DEPRECATIONS
}

int main(int argc, char **argv)
{
	/* WM_CLASS, set through the program name and program class rather than
	 * gtk_window_set_wmclass(), which is deprecated: GTK builds WM_CLASS
	 * from these two when the window is realised. */
	g_set_prgname("load");
	gdk_set_program_class("load");

	gtk_init(&argc, &argv);

	panel_load_init(&g_mon);
	/* One sample before the window is mapped, so the first frame drawn has
	 * a memory reading rather than an empty pane. */
	panel_load_sample(&g_mon);

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_window_set_title(GTK_WINDOW(win), "load");
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);

	GtkWidget *area = gtk_drawing_area_new();

	gtk_container_add(GTK_CONTAINER(win), area);
	g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	g_timeout_add_seconds(PANEL_LOAD_SECS, on_tick, area);

	gtk_widget_set_size_request(area, 176, PANEL_HEIGHT);
	apply_geometry(GTK_WINDOW(win), argc, argv);

	gtk_widget_show_all(win);
	gtk_main();
	return 0;
}
