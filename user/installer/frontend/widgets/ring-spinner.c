/* LikeOS installer -- the busy indicator.
 *
 * A faint track, and on it an arc that stretches and shrinks while it runs
 * round, led by a bright dot.  Painted in the widget's text colour, so the
 * same spinner works on the black splash, the white panel and the accent
 * screen.  Animated by the frame clock, only while mapped.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
	int size;
	gboolean active;
	guint tick;
	gint64 epoch;
	double t;          /* seconds since start */
} Spinner;

#define SPIN_PERIOD 1.6    /* one revolution of the head */
#define SPIN_STRETCH 2.4   /* one stretch-and-shrink of the arc */

static double ease_in_out(double x)
{
	return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
}

static gboolean spinner_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	Spinner *s = data;
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	double size = MIN(MIN(aw, ah), s->size);
	double cx = aw / 2.0, cy = ah / 2.0;
	double lw = MAX(2.0, size / 11.0);
	double r = size / 2.0 - lw;
	double head, len, st;
	GdkRGBA c;

	gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

	/* track */
	cairo_set_line_width(cr, lw);
	cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * 0.16);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_stroke(cr);

	/* the running arc */
	st = fmod(s->t / SPIN_STRETCH, 1.0);
	len = 0.35 + 3.2 * (st < 0.5 ? ease_in_out(st * 2) : ease_in_out((1 - st) * 2));
	head = fmod(s->t / SPIN_PERIOD, 1.0) * 2 * M_PI + st * 2 * M_PI - M_PI / 2;

	{
		cairo_pattern_t *p;
		/* fade the tail by drawing it in short pieces */
		int pieces = 14;

		for (int i = 0; i < pieces; i++) {
			double a0 = head - len * (i + 1) / pieces;
			double a1 = head - len * i / pieces;
			double alpha = 1.0 - (double)i / pieces * 0.75;

			cairo_set_source_rgba(cr, c.red, c.green, c.blue,
					      c.alpha * alpha);
			cairo_arc(cr, cx, cy, r, a0, a1 + 0.01);
			cairo_stroke(cr);
		}
		/* the head: a dot a little wider than the arc */
		p = cairo_pattern_create_radial(cx + cos(head) * r, cy + sin(head) * r,
						0, cx + cos(head) * r,
						cy + sin(head) * r, lw * 1.4);
		cairo_pattern_add_color_stop_rgba(p, 0, c.red, c.green, c.blue,
						  c.alpha);
		cairo_pattern_add_color_stop_rgba(p, 0.6, c.red, c.green, c.blue,
						  c.alpha);
		cairo_pattern_add_color_stop_rgba(p, 1, c.red, c.green, c.blue, 0);
		cairo_set_source(cr, p);
		cairo_arc(cr, cx + cos(head) * r, cy + sin(head) * r, lw * 1.4, 0,
			  2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	return FALSE;
}

static gboolean spinner_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	Spinner *s = data;
	gint64 now = gdk_frame_clock_get_frame_time(clock);

	if (!s->epoch)
		s->epoch = now;
	if (s->active && art_animations_enabled()) {
		s->t = (now - s->epoch) / 1e6;
		gtk_widget_queue_draw(w);
	}
	return G_SOURCE_CONTINUE;
}

static void spinner_map(GtkWidget *w, gpointer data)
{
	Spinner *s = data;

	if (!s->tick)
		s->tick = gtk_widget_add_tick_callback(w, spinner_tick, s, NULL);
}

static void spinner_unmap(GtkWidget *w, gpointer data)
{
	Spinner *s = data;

	if (s->tick) {
		gtk_widget_remove_tick_callback(w, s->tick);
		s->tick = 0;
	}
}

GtkWidget *ring_spinner_new(int size)
{
	GtkWidget *w = gtk_drawing_area_new();
	Spinner *s = g_new0(Spinner, 1);

	s->size = size;
	s->active = TRUE;
	s->t = 0.35;   /* a pleasant still frame when animations are off */
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "il-spinner");
	g_object_set_data_full(G_OBJECT(w), "spinner", s, g_free);
	g_signal_connect(w, "draw", G_CALLBACK(spinner_draw), s);
	g_signal_connect(w, "map", G_CALLBACK(spinner_map), s);
	g_signal_connect(w, "unmap", G_CALLBACK(spinner_unmap), s);
	return w;
}

void ring_spinner_set_active(GtkWidget *w, gboolean active)
{
	Spinner *s = g_object_get_data(G_OBJECT(w), "spinner");

	if (!s)
		return;
	s->active = active;
	gtk_widget_set_opacity(w, active ? 1.0 : 0.0);
}
