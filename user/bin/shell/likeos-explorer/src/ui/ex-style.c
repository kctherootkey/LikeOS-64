// LikeOS Explorer: the look.
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

/* Keyboard focus rings belong to keyboard use: moving the focus with the
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
 * title bar shows Close alone (ctwm patch 0005). */
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

/* ---- pictures ------------------------------------------------------------- */

GIcon *ex_style_gicon(const char *name, const char *fallback)
{
	const char *names[] = { name, fallback, NULL };

	return g_themed_icon_new_from_names((char **)names, fallback ? 2 : 1);
}

void ex_style_set_icon(GtkImage *img, const char *name, const char *fallback,
		       int px)
{
	GIcon *icon = ex_style_gicon(name, fallback);

	gtk_image_set_from_gicon(img, icon, GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(img, px);
	g_object_unref(icon);
}

GtkWidget *ex_style_icon(const char *name, const char *fallback, int px)
{
	GtkWidget *img = gtk_image_new();

	ex_style_set_icon(GTK_IMAGE(img), name, fallback, px);
	return img;
}

/* The glyphs are LikeOS's own line drawings on a 10-unit box, the same
 * geometry ctwm draws on its title bars: strokes 1.25 pixels wide with
 * round ends, the horizontal and vertical ones along pixel centres so each
 * covers one row or column fully.
 *
 *   minimise  a line across the middle, (1, 5.5) to (9, 5.5);
 *   maximise  a square from 0.5 to 9.5 with corners of radius 2.5;
 *   restore   a front square 0.5..7.5 across and 2.5..9.5 down, and a back
 *             one 2.5..9.5 across and 0.5..7.5 down, both of radius 2, the
 *             back one cut away within 1.625 of the front one's outline
 *             (and inside it), so a clear gap separates the two;
 *   close     the two diagonals of 1..9;
 *   plus      a cross through (5.5, 5.5), for New tab. */
#define EX_GLYPH_STROKE 1.25

static void glyph_rrect(cairo_t *cr, double x0, double y0, double x1,
			double y1, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x1 - r, y0 + r, r, -0.5 * G_PI, 0);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, 0.5 * G_PI);
	cairo_arc(cr, x0 + r, y1 - r, r, 0.5 * G_PI, G_PI);
	cairo_arc(cr, x0 + r, y0 + r, r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
}

static void glyph_line(cairo_t *cr, double x0, double y0, double x1, double y1)
{
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
}

static gboolean draw_glyph(GtkWidget *w, cairo_t *cr, gpointer data)
{
	ExGlyph g = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "ex-glyph"));
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	int width = gtk_widget_get_allocated_width(w);
	int height = gtk_widget_get_allocated_height(w);
	int size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "ex-size"));
	double k = size / 10.0;
	GdkRGBA c;

	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), &c);
	/* The box on whole pixels, so the half-unit lines stay crisp. */
	cairo_translate(cr, (width - size) / 2, (height - size) / 2);
	cairo_scale(cr, k, k);
	cairo_set_line_width(cr, EX_GLYPH_STROKE / k);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	gdk_cairo_set_source_rgba(cr, &c);
	switch (g) {
	case EX_GLYPH_MINIMIZE:
		glyph_line(cr, 1, 5.5, 9, 5.5);
		break;
	case EX_GLYPH_MAXIMIZE:
		glyph_rrect(cr, 0.5, 0.5, 9.5, 9.5, 2.5);
		break;
	case EX_GLYPH_RESTORE:
		/* The back square in a group of its own, the band round the
		 * front one cleared out of it, then the front square. */
		cairo_push_group(cr);
		glyph_rrect(cr, 2.5, 0.5, 9.5, 7.5, 2);
		cairo_stroke(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		glyph_rrect(cr, 0.5, 2.5, 7.5, 9.5, 2);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 2 * (EX_GLYPH_STROKE / 2 + 1) / k);
		cairo_stroke(cr);
		cairo_pop_group_to_source(cr);
		cairo_paint(cr);
		gdk_cairo_set_source_rgba(cr, &c);
		glyph_rrect(cr, 0.5, 2.5, 7.5, 9.5, 2);
		break;
	case EX_GLYPH_CLOSE:
		glyph_line(cr, 1, 1, 9, 9);
		glyph_line(cr, 1, 9, 9, 1);
		break;
	case EX_GLYPH_PLUS:
		glyph_line(cr, 1.5, 5.5, 9.5, 5.5);
		glyph_line(cr, 5.5, 1.5, 5.5, 9.5);
		break;
	}
	cairo_stroke(cr);
	return TRUE;
}

GtkWidget *ex_style_glyph(ExGlyph glyph, int size)
{
	GtkWidget *w = gtk_drawing_area_new();

	g_object_set_data(G_OBJECT(w), "ex-glyph", GINT_TO_POINTER(glyph));
	g_object_set_data(G_OBJECT(w), "ex-size", GINT_TO_POINTER(size));
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	g_signal_connect(w, "draw", G_CALLBACK(draw_glyph), NULL);
	return w;
}

void ex_style_set_glyph(GtkWidget *glyph, ExGlyph kind)
{
	g_object_set_data(G_OBJECT(glyph), "ex-glyph", GINT_TO_POINTER(kind));
	gtk_widget_queue_draw(glyph);
}

GtkWidget *ex_style_icon_button(GtkWidget *child, const char *tip,
				const char *action)
{
	GtkWidget *b = gtk_button_new();

	gtk_container_add(GTK_CONTAINER(b), child);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click(b, FALSE);
	ex_style_add(b, EX_CLASS_SUBTLE);
	if (tip) {
		gtk_widget_set_tooltip_text(b, tip);
		atk_object_set_name(gtk_widget_get_accessible(b), tip);
	}
	if (action)
		gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b),
							action);
	return b;
}
