// LikeOS-64 -- datetime, the standalone clock.
//
// Two lines, right-aligned, in the arrangement the Windows 11 taskbar uses:
//
//         13:37
//    19.08.2026
//
// The drawing is in panel.c, shared with the taskbar's tray clock -- which
// draws the one-line form of the same thing, because that is what fits beside
// the workspace buttons.
//
// Since taskbar(1) took over the desktop bar this is no longer started by
// xinitrc.  It is kept because the stacked form is the nicer one when it has a
// window to itself.

#include "panel.h"

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	(void)data;
	panel_fill_face(cr, 0, 0, gtk_widget_get_allocated_width(widget),
			gtk_widget_get_allocated_height(widget));
	panel_clock_draw(cr, 0, 0, gtk_widget_get_allocated_width(widget),
			 gtk_widget_get_allocated_height(widget), TRUE);
	return TRUE;
}

/* Redraw once a second.
 *
 * A minute-aligned timer would be tidier for a clock showing only minutes, but
 * it has to be re-armed against the wall clock every time it fires or it
 * drifts; at one redraw a second the difference is not measurable and this
 * cannot drift at all. */
static gboolean on_tick(gpointer widget)
{
	gtk_widget_queue_draw(GTK_WIDGET(widget));
	return G_SOURCE_CONTINUE;
}

/* See load.c for why this is gtk_window_parse_geometry and not
 * gtk_window_move: only parse_geometry sets the USPosition hint, and ctwm
 * places any window without it interactively. */
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
	g_set_prgname("datetime");
	gdk_set_program_class("datetime");

	gtk_init(&argc, &argv);

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_window_set_title(GTK_WINDOW(win), "datetime");
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);

	GtkWidget *area = gtk_drawing_area_new();

	gtk_container_add(GTK_CONTAINER(win), area);
	g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	g_timeout_add_seconds(1, on_tick, area);

	gtk_widget_set_size_request(area, 88, PANEL_HEIGHT);
	apply_geometry(GTK_WINDOW(win), argc, argv);

	gtk_widget_show_all(win);
	gtk_main();
	return 0;
}
