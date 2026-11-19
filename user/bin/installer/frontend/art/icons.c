/* LikeOS installer -- the small line icons of toolbars, rows and entries.
 *
 * Drawn in the unit square with a stroke about a twelfth of the size, in
 * one colour, so they read like the system's own symbolic icons and follow
 * the text colour of whatever holds them.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

static void arrow_head(cairo_t *cr, double x, double y, double angle, double len)
{
	cairo_move_to(cr, x + cos(angle + 2.5) * len, y + sin(angle + 2.5) * len);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x + cos(angle - 2.5) * len, y + sin(angle - 2.5) * len);
}

static void drive_outline(cairo_t *cr, double y, double h)
{
	art_rounded_rect(cr, 0.12, y, 0.76, h, 0.08);
}

static void paint_icon(cairo_t *cr, IconKind kind)
{
	double a;

	switch (kind) {
	case ICON_REFRESH:
	case ICON_RESTART:
		a = kind == ICON_REFRESH ? -0.25 : -1.3;
		cairo_arc(cr, 0.5, 0.5, 0.31, a, a + 5.0);
		cairo_stroke(cr);
		{
			double ex = 0.5 + cos(a + 5.0) * 0.31;
			double ey = 0.5 + sin(a + 5.0) * 0.31;

			arrow_head(cr, ex, ey, a + 5.0 + M_PI / 2, 0.17);
			cairo_stroke(cr);
		}
		break;
	case ICON_DELETE:
	case ICON_CLOSE:
		a = kind == ICON_DELETE ? 0.24 : 0.27;
		cairo_move_to(cr, a, a);
		cairo_line_to(cr, 1 - a, 1 - a);
		cairo_move_to(cr, 1 - a, a);
		cairo_line_to(cr, a, 1 - a);
		cairo_stroke(cr);
		break;
	case ICON_FORMAT:
		drive_outline(cr, 0.60, 0.26);
		cairo_stroke(cr);
		/* a brush sweeping the drive clean */
		cairo_move_to(cr, 0.70, 0.10);
		cairo_line_to(cr, 0.52, 0.36);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.40, 0.33);
		cairo_line_to(cr, 0.60, 0.46);
		cairo_line_to(cr, 0.52, 0.56);
		cairo_line_to(cr, 0.30, 0.50);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_arc(cr, 0.76, 0.73, 0.03, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_NEW:
		cairo_move_to(cr, 0.5, 0.18);
		cairo_line_to(cr, 0.5, 0.82);
		cairo_move_to(cr, 0.18, 0.5);
		cairo_line_to(cr, 0.82, 0.5);
		cairo_stroke(cr);
		break;
	case ICON_EXTEND:
		drive_outline(cr, 0.56, 0.28);
		cairo_stroke(cr);
		cairo_rectangle(cr, 0.20, 0.64, 0.30, 0.12);
		cairo_fill(cr);
		cairo_move_to(cr, 0.16, 0.30);
		cairo_line_to(cr, 0.84, 0.30);
		cairo_stroke(cr);
		arrow_head(cr, 0.86, 0.30, 0, 0.14);
		cairo_stroke(cr);
		arrow_head(cr, 0.14, 0.30, M_PI, 0.14);
		cairo_stroke(cr);
		break;
	case ICON_WHOLE_DISK:
		drive_outline(cr, 0.30, 0.40);
		cairo_stroke(cr);
		art_rounded_rect(cr, 0.21, 0.39, 0.58, 0.14, 0.03);
		cairo_fill(cr);
		cairo_arc(cr, 0.74, 0.61, 0.035, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_DETAILS:
	case ICON_INFO:
		cairo_arc(cr, 0.5, 0.5, 0.36, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.5, 0.46);
		cairo_line_to(cr, 0.5, 0.70);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.33, 0.045, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_UNDO:
		cairo_move_to(cr, 0.22, 0.40);
		cairo_line_to(cr, 0.62, 0.40);
		cairo_curve_to(cr, 0.92, 0.40, 0.92, 0.80, 0.62, 0.80);
		cairo_line_to(cr, 0.36, 0.80);
		cairo_stroke(cr);
		arrow_head(cr, 0.20, 0.40, M_PI, 0.16);
		cairo_stroke(cr);
		break;
	case ICON_EYE:
	case ICON_EYE_OFF:
		cairo_move_to(cr, 0.08, 0.5);
		cairo_curve_to(cr, 0.30, 0.16, 0.70, 0.16, 0.92, 0.5);
		cairo_curve_to(cr, 0.70, 0.84, 0.30, 0.84, 0.08, 0.5);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.5, 0.13, 0, 2 * M_PI);
		cairo_fill(cr);
		if (kind == ICON_EYE_OFF) {
			cairo_move_to(cr, 0.16, 0.14);
			cairo_line_to(cr, 0.84, 0.86);
			cairo_stroke(cr);
		}
		break;
	case ICON_BACK:
		cairo_move_to(cr, 0.84, 0.5);
		cairo_line_to(cr, 0.16, 0.5);
		cairo_stroke(cr);
		arrow_head(cr, 0.15, 0.5, M_PI, 0.30);
		cairo_stroke(cr);
		break;
	case ICON_POWER:
		cairo_arc(cr, 0.5, 0.54, 0.32, -M_PI / 2 + 0.75, 3 * M_PI / 2 - 0.75);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.5, 0.12);
		cairo_line_to(cr, 0.5, 0.48);
		cairo_stroke(cr);
		break;
	case ICON_KEYBOARD:
		art_rounded_rect(cr, 0.08, 0.24, 0.84, 0.52, 0.08);
		cairo_stroke(cr);
		for (int r = 0; r < 2; r++)
			for (int c = 0; c < 5; c++) {
				cairo_rectangle(cr, 0.20 + c * 0.13, 0.35 + r * 0.13,
						0.07, 0.07);
				cairo_fill(cr);
			}
		cairo_move_to(cr, 0.32, 0.64);
		cairo_line_to(cr, 0.68, 0.64);
		cairo_stroke(cr);
		break;
	case ICON_CHECK:
		cairo_move_to(cr, 0.18, 0.52);
		cairo_line_to(cr, 0.40, 0.74);
		cairo_line_to(cr, 0.82, 0.28);
		cairo_stroke(cr);
		break;
	case ICON_HELP:
		cairo_arc(cr, 0.5, 0.5, 0.36, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.40, 0.11, M_PI * 1.05, M_PI * 2.35);
		cairo_line_to(cr, 0.5, 0.58);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.70, 0.045, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_WARNING:
		cairo_move_to(cr, 0.5, 0.12);
		cairo_line_to(cr, 0.90, 0.84);
		cairo_line_to(cr, 0.10, 0.84);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.5, 0.38);
		cairo_line_to(cr, 0.5, 0.60);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.72, 0.045, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_ERROR:
		cairo_arc(cr, 0.5, 0.5, 0.36, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.37, 0.37);
		cairo_line_to(cr, 0.63, 0.63);
		cairo_move_to(cr, 0.63, 0.37);
		cairo_line_to(cr, 0.37, 0.63);
		cairo_stroke(cr);
		break;
	case ICON_DISK:
		drive_outline(cr, 0.28, 0.44);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.24, 0.56);
		cairo_line_to(cr, 0.76, 0.56);
		cairo_stroke(cr);
		cairo_arc(cr, 0.72, 0.65, 0.04, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_USB:
		art_rounded_rect(cr, 0.26, 0.36, 0.48, 0.52, 0.08);
		cairo_stroke(cr);
		cairo_rectangle(cr, 0.35, 0.12, 0.30, 0.24);
		cairo_stroke(cr);
		cairo_rectangle(cr, 0.41, 0.19, 0.06, 0.07);
		cairo_rectangle(cr, 0.53, 0.19, 0.06, 0.07);
		cairo_fill(cr);
		break;
	case ICON_PARTITION:
		art_rounded_rect(cr, 0.10, 0.30, 0.80, 0.40, 0.06);
		cairo_stroke(cr);
		cairo_rectangle(cr, 0.10, 0.30, 0.34, 0.40);
		cairo_fill(cr);
		cairo_move_to(cr, 0.64, 0.30);
		cairo_line_to(cr, 0.64, 0.70);
		cairo_stroke(cr);
		break;
	case ICON_FREE: {
		static const double dash[] = { 0.10, 0.07 };

		cairo_set_dash(cr, dash, 2, 0);
		art_rounded_rect(cr, 0.10, 0.30, 0.80, 0.40, 0.06);
		cairo_stroke(cr);
		cairo_set_dash(cr, NULL, 0, 0);
		break;
	}
	case ICON_SEARCH:
		cairo_arc(cr, 0.43, 0.43, 0.25, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.62, 0.62);
		cairo_line_to(cr, 0.86, 0.86);
		cairo_stroke(cr);
		break;
	case ICON_GLOBE:
		cairo_arc(cr, 0.5, 0.5, 0.37, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_save(cr);
		cairo_translate(cr, 0.5, 0.5);
		cairo_scale(cr, 0.16, 0.37);
		cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.14, 0.5);
		cairo_line_to(cr, 0.86, 0.5);
		cairo_stroke(cr);
		break;
	case ICON_USER:
		cairo_arc(cr, 0.5, 0.34, 0.17, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.16, 0.88);
		cairo_curve_to(cr, 0.18, 0.62, 0.34, 0.58, 0.5, 0.58);
		cairo_curve_to(cr, 0.66, 0.58, 0.82, 0.62, 0.84, 0.88);
		cairo_stroke(cr);
		break;
	case ICON_LOCK:
		art_rounded_rect(cr, 0.20, 0.44, 0.60, 0.44, 0.08);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.32, 0.44);
		cairo_line_to(cr, 0.32, 0.32);
		cairo_arc(cr, 0.5, 0.32, 0.18, M_PI, 2 * M_PI);
		cairo_line_to(cr, 0.68, 0.44);
		cairo_stroke(cr);
		cairo_arc(cr, 0.5, 0.64, 0.05, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_COMPUTER:
		art_rounded_rect(cr, 0.10, 0.16, 0.80, 0.52, 0.06);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.5, 0.68);
		cairo_line_to(cr, 0.5, 0.82);
		cairo_move_to(cr, 0.30, 0.84);
		cairo_line_to(cr, 0.70, 0.84);
		cairo_stroke(cr);
		break;
	case ICON_CLOCK:
		cairo_arc(cr, 0.5, 0.5, 0.36, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.5, 0.28);
		cairo_line_to(cr, 0.5, 0.5);
		cairo_line_to(cr, 0.66, 0.60);
		cairo_stroke(cr);
		break;
	case ICON_LANGUAGE:
		art_rounded_rect(cr, 0.10, 0.14, 0.80, 0.56, 0.12);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.30, 0.70);
		cairo_line_to(cr, 0.24, 0.88);
		cairo_line_to(cr, 0.46, 0.70);
		cairo_stroke(cr);
		cairo_move_to(cr, 0.34, 0.58);
		cairo_line_to(cr, 0.5, 0.26);
		cairo_line_to(cr, 0.66, 0.58);
		cairo_move_to(cr, 0.40, 0.47);
		cairo_line_to(cr, 0.60, 0.47);
		cairo_stroke(cr);
		break;
	case ICON_DOT:
		cairo_arc(cr, 0.5, 0.5, 0.18, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	default:
		break;
	}
}

void icon_draw(cairo_t *cr, IconKind kind, double x, double y, double size,
	       const GdkRGBA *color)
{
	if (size <= 1 || kind == ICON_NONE)
		return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size, size);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	/* about 1.5 px at 16 px, never thinner than one pixel */
	cairo_set_line_width(cr, MAX(1.0 / size, size < 24 ? 0.095 : 0.075));
	if (color)
		gdk_cairo_set_source_rgba(cr, color);
	paint_icon(cr, kind);
	cairo_restore(cr);
}

static gboolean icon_widget_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	IconKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "icon-kind"));
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	int size = GPOINTER_TO_INT(data);
	GdkRGBA col;

	gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &col);
	if (size > aw)
		size = aw;
	if (size > ah)
		size = ah;
	icon_draw(cr, kind, floor((aw - size) / 2.0), floor((ah - size) / 2.0),
		  size, &col);
	return FALSE;
}

GtkWidget *icon_new(IconKind kind, int size)
{
	GtkWidget *w = gtk_drawing_area_new();

	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "il-icon");
	g_object_set_data(G_OBJECT(w), "icon-kind", GINT_TO_POINTER(kind));
	g_signal_connect(w, "draw", G_CALLBACK(icon_widget_draw),
			 GINT_TO_POINTER(size));
	return w;
}

void icon_set(GtkWidget *w, IconKind kind)
{
	g_object_set_data(G_OBJECT(w), "icon-kind", GINT_TO_POINTER(kind));
	gtk_widget_queue_draw(w);
}

GdkPixbuf *icon_pixbuf(IconKind kind, int size, const GdkRGBA *color)
{
	cairo_surface_t *s;
	cairo_t *cr;
	GdkPixbuf *pb;

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(s);
	icon_draw(cr, kind, 0, 0, size, color);
	cairo_destroy(cr);
	pb = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
	cairo_surface_destroy(s);
	return pb;
}
