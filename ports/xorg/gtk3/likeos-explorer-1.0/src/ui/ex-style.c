// LikeOS -- File Explorer: the look.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-style.h"

#include <string.h>

/* Only used under a theme other than LikeOS: spacing and structure, in
 * that theme's own colours.  The real rules are in the LikeOS theme. */
static const char fallback_css[] =
	".likeos-explorer .ex-navbar { padding: 4px 8px; }\n"
	".likeos-explorer .ex-commandbar { padding: 2px 8px;"
	"  border-bottom: 1px solid @borders; }\n"
	".likeos-explorer .ex-statusbar { padding: 2px 10px; }\n"
	".likeos-explorer .ex-section-title { font-weight: bold;"
	"  padding: 12px 0 4px 0; }\n"
	".likeos-explorer .ex-tile { padding: 8px; border-radius: 4px; }\n"
	".likeos-explorer .ex-placeholder { opacity: 0.6; }\n"
	".likeos-explorer .ex-pane { border-left: 1px solid @borders; }\n";

static GtkCssProvider *g_fallback;

static void theme_changed(GtkSettings *s, GParamSpec *ps, gpointer data)
{
	char *name = NULL;
	GdkScreen *scr = gdk_screen_get_default();

	g_object_get(s, "gtk-theme-name", &name, NULL);
	gboolean ours = name && strcmp(name, "LikeOS") == 0;

	if (!ours && !g_fallback) {
		g_fallback = gtk_css_provider_new();
		gtk_css_provider_load_from_data(g_fallback, fallback_css, -1,
						NULL);
		gtk_style_context_add_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(g_fallback),
			GTK_STYLE_PROVIDER_PRIORITY_FALLBACK + 1);
	} else if (ours && g_fallback) {
		gtk_style_context_remove_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(g_fallback));
		g_clear_object(&g_fallback);
	}
	g_free(name);
}

/* Keyboard focus rings, as Windows shows them: moving the focus with the
 * keyboard (Tab, the arrow keys, Home, End, Page Up and Down) shows them, a
 * mouse press hides them again, and any other key -- a shortcut, typing a
 * name, Space on a box -- leaves them as they were.  GTK on its own shows
 * them for good at the first key of any kind. */
static gboolean moves_focus(guint keyval)
{
	switch (keyval) {
	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
	case GDK_KEY_KP_Tab:
	case GDK_KEY_Up:
	case GDK_KEY_Down:
	case GDK_KEY_Left:
	case GDK_KEY_Right:
	case GDK_KEY_KP_Up:
	case GDK_KEY_KP_Down:
	case GDK_KEY_KP_Left:
	case GDK_KEY_KP_Right:
	case GDK_KEY_Home:
	case GDK_KEY_End:
	case GDK_KEY_KP_Home:
	case GDK_KEY_KP_End:
	case GDK_KEY_Page_Up:
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Up:
	case GDK_KEY_KP_Page_Down:
		return TRUE;
	}
	return FALSE;
}

static GtkWindow *event_window(GdkEvent *ev)
{
	GtkWidget *w = gtk_get_event_widget(ev);

	if (!w)
		return NULL;
	w = gtk_widget_get_toplevel(w);
	return GTK_IS_WINDOW(w) ? g_object_ref(GTK_WINDOW(w)) : NULL;
}

static void focus_rings(GdkEvent *ev, gpointer data)
{
	GtkWindow *win = NULL;
	gboolean was = FALSE;

	switch (ev->type) {
	case GDK_KEY_PRESS:
	case GDK_KEY_RELEASE:
		if (moves_focus(ev->key.keyval))
			break;
		win = event_window(ev);
		if (win)
			was = gtk_window_get_focus_visible(win);
		break;
	case GDK_BUTTON_PRESS:
		/* Before the press is handled too: a window it opens takes
		 * the setting from this one. */
		win = event_window(ev);
		if (win)
			gtk_window_set_focus_visible(win, FALSE);
		break;
	default:
		break;
	}
	gtk_main_do_event(ev);
	if (win) {
		if (gtk_window_get_focus_visible(win) != was)
			gtk_window_set_focus_visible(win, was);
		g_object_unref(win);
	}
}

void ex_style_init(void)
{
	GtkSettings *s = gtk_settings_get_default();

	if (!s)
		return;
	gdk_event_handler_set(focus_rings, NULL, NULL);
	g_signal_connect(s, "notify::gtk-theme-name", G_CALLBACK(theme_changed),
			 NULL);
	theme_changed(s, NULL, NULL);
}

void ex_style_add(GtkWidget *w, const char *klass)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), klass);
}

/* Move and close only: a dialog has no Minimize or Maximize, and its
 * title bar shows Close alone (ctwm patch 0005), as on Windows. */
static void fixed_realized(GtkWidget *w, gpointer data)
{
	gdk_window_set_functions(gtk_widget_get_window(w),
				 GDK_FUNC_MOVE | GDK_FUNC_CLOSE);
}

void ex_style_fixed_dialog(GtkWidget *dialog)
{
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	if (gtk_widget_get_realized(dialog))
		fixed_realized(dialog, NULL);
	else
		g_signal_connect(dialog, "realize", G_CALLBACK(fixed_realized),
				 NULL);
}
