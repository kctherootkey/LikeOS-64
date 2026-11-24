// LikeOS Task Viewer -- the program's own line glyphs (see tv-glyphs.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tv-glyphs.h"

#define PI G_PI

static void line(cairo_t *cr, double x0, double y0, double x1, double y1)
{
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
	cairo_stroke(cr);
}

static void dot(cairo_t *cr, double cx, double cy, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * PI);
	cairo_fill(cr);
}

static void rrect(cairo_t *cr, double x0, double y0, double x1, double y1,
		  double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x1 - r, y0 + r, r, -0.5 * PI, 0);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, 0.5 * PI);
	cairo_arc(cr, x0 + r, y1 - r, r, 0.5 * PI, PI);
	cairo_arc(cr, x0 + r, y0 + r, r, PI, 1.5 * PI);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

/* A frame with three bars of different heights standing in it: the
 * processes, each taking its share. */
static void gl_processes(cairo_t *cr)
{
	rrect(cr, 1.75, 1.75, 14.25, 14.25, 2.5);
	line(cr, 5, 11.25, 5, 8);
	line(cr, 8, 11.25, 8, 4.75);
	line(cr, 11, 11.25, 11, 9.5);
}

/* A trace over a baseline. */
static void gl_performance(cairo_t *cr)
{
	cairo_move_to(cr, 1.5, 9);
	cairo_line_to(cr, 4.5, 9);
	cairo_line_to(cr, 6.5, 3.5);
	cairo_line_to(cr, 9.5, 12.5);
	cairo_line_to(cr, 11.5, 7);
	cairo_line_to(cr, 14.5, 7);
	cairo_stroke(cr);
	line(cr, 1.5, 14.25, 14.5, 14.25);
}

/* Two people, one in front of the other. */
static void gl_users(cairo_t *cr)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, 6, 5, 2.5, 0, 2 * PI);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 6, 15, 5, 1.15 * PI, 1.85 * PI);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 11.25, 5.75, 2, -0.6 * PI, 0.6 * PI);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 11, 15, 4.5, 1.55 * PI, 1.9 * PI);
	cairo_stroke(cr);
}

/* A list: a mark and a line per row. */
static void gl_details(cairo_t *cr)
{
	for (int i = 0; i < 3; i++) {
		double y = 3.5 + i * 4.5;

		dot(cr, 2.75, y, 1.1);
		line(cr, 6, y, 14.25, y);
	}
}

/* A wheel with eight cogs. */
static void gl_services(cairo_t *cr)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 8, 2, 0, 2 * PI);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 8, 4.75, 0, 2 * PI);
	cairo_stroke(cr);
	for (int i = 0; i < 8; i++) {
		double a = i * PI / 4;

		line(cr, 8 + cos(a) * 5.25, 8 + sin(a) * 5.25,
		     8 + cos(a) * 6.75, 8 + sin(a) * 6.75);
	}
}

/* A dial with its needle well up: what starts with the session and how
 * hard it weighs on the start. */
static void gl_startup(cairo_t *cr)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 9.5, 6.25, 0.8 * PI, 2.2 * PI);
	cairo_stroke(cr);
	line(cr, 8, 9.5, 11.25, 5.5);
	dot(cr, 8, 9.5, 1.4);
	line(cr, 5, 14, 11, 14);
}

/* A window with an arrow leaving it at the top right. */
static void gl_run(cairo_t *cr)
{
	cairo_move_to(cr, 7.5, 2.5);
	cairo_line_to(cr, 4, 2.5);
	cairo_curve_to(cr, 2.75, 2.5, 2, 3.25, 2, 4.5);
	cairo_line_to(cr, 2, 12);
	cairo_curve_to(cr, 2, 13.25, 2.75, 14, 4, 14);
	cairo_line_to(cr, 11.5, 14);
	cairo_curve_to(cr, 12.75, 14, 13.5, 13.25, 13.5, 12);
	cairo_line_to(cr, 13.5, 8.5);
	cairo_stroke(cr);
	line(cr, 7.5, 8.5, 14, 2);
	cairo_move_to(cr, 10, 2);
	cairo_line_to(cr, 14, 2);
	cairo_line_to(cr, 14, 6);
	cairo_stroke(cr);
}

/* A ring struck through. */
static void gl_end_task(cairo_t *cr)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 8, 6, 0, 2 * PI);
	cairo_stroke(cr);
	line(cr, 3.9, 12.1, 12.1, 3.9);
}

static void gl_more(cairo_t *cr)
{
	dot(cr, 3, 8, 1.25);
	dot(cr, 8, 8, 1.25);
	dot(cr, 13, 8, 1.25);
}

static void gl_menu(cairo_t *cr)
{
	line(cr, 2, 4, 14, 4);
	line(cr, 2, 8, 14, 8);
	line(cr, 2, 12, 14, 12);
}

static void (*const painters[TV_GLYPH_COUNT])(cairo_t *) = {
	[TV_GLYPH_PROCESSES] = gl_processes,
	[TV_GLYPH_PERFORMANCE] = gl_performance,
	[TV_GLYPH_USERS] = gl_users,
	[TV_GLYPH_DETAILS] = gl_details,
	[TV_GLYPH_SERVICES] = gl_services,
	[TV_GLYPH_STARTUP] = gl_startup,
	[TV_GLYPH_RUN] = gl_run,
	[TV_GLYPH_END_TASK] = gl_end_task,
	[TV_GLYPH_MORE] = gl_more,
	[TV_GLYPH_MENU] = gl_menu,
};

TvGlyph tv_glyph_for_page(const char *id)
{
	static const struct {
		const char *id;
		TvGlyph g;
	} map[] = {
		{ "processes", TV_GLYPH_PROCESSES },
		{ "performance", TV_GLYPH_PERFORMANCE },
		{ "users", TV_GLYPH_USERS },
		{ "details", TV_GLYPH_DETAILS },
		{ "services", TV_GLYPH_SERVICES },
		{ "startup", TV_GLYPH_STARTUP },
	};

	for (guint i = 0; id && i < G_N_ELEMENTS(map); i++)
		if (!strcmp(map[i].id, id))
			return map[i].g;
	return TV_GLYPH_NONE;
}

void tv_glyph_paint(cairo_t *cr, TvGlyph g, double cx, double cy, double size)
{
	double s = size / 16.0;
	double px;

	if ((int)g <= 0 || g >= TV_GLYPH_COUNT || !painters[g] || size < 4)
		return;
	px = size <= 24 ? 1.5 * s : MAX(2.25, 1.5 + size / 32.0);
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_translate(cr, round(cx - size / 2.0), round(cy - size / 2.0));
	cairo_scale(cr, s, s);
	cairo_set_line_width(cr, px / s);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	painters[g](cr);
	cairo_restore(cr);
}

static gboolean glyph_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	TvGlyph g = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "tv-glyph"));
	int size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "tv-size"));
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	GdkRGBA c;

	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), &c);
	gdk_cairo_set_source_rgba(cr, &c);
	tv_glyph_paint(cr, g, gtk_widget_get_allocated_width(w) / 2.0,
		       gtk_widget_get_allocated_height(w) / 2.0, size);
	return TRUE;
}

GtkWidget *tv_glyph_new(TvGlyph g, int size)
{
	GtkWidget *w = gtk_drawing_area_new();

	g_object_set_data(G_OBJECT(w), "tv-glyph", GINT_TO_POINTER(g));
	g_object_set_data(G_OBJECT(w), "tv-size", GINT_TO_POINTER(size));
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	g_signal_connect(w, "draw", G_CALLBACK(glyph_draw), NULL);
	return w;
}
