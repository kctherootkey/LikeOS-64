// LikeOS Task Viewer -- the history graph.
//
// The samples are kept with the time they were taken, and the graph is
// drawn against time rather than against the sample count: the right edge
// is the newest sample, the left edge TV_GRAPH_SECONDS before it, whatever
// the update speed is and however it was changed on the way.  The vertical
// grid lines sit at fixed points of that time, every 6 seconds, so they
// travel left with the curve.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tv-graph.h"

#define MAX_SAMPLES 1024 /* 60 s at the fastest update speed, with room */
#define GRID_SECONDS 6

typedef struct {
	gint64 t; /* monotonic, microseconds */
	double v[2];
} Sample;

typedef struct {
	TvGraphStyle style;
	double r, g, b;
	double fixed; /* 0: automatic */
	double min_scale;
	double scale; /* the one drawn at */
	gboolean dual;
	gboolean binary;
	Sample s[MAX_SAMPLES]; /* ring */
	int head, n;
	GtkLabel *scale_label;
	char *(*format)(double);
} Graph;

static Graph *graph_of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "tv-graph");
}

static const Sample *sample_at(const Graph *g, int i)
{
	return &g->s[(g->head + i) % MAX_SAMPLES];
}

/* 1, 2 or 5 times a power of ten, at least `v'. */
static double nice_ceiling(double v)
{
	double p, m;

	if (v <= 0)
		return 1;
	p = pow(10.0, floor(log10(v)));
	m = v / p;
	if (m <= 1.0 + 1e-9)
		return p;
	if (m <= 2.0 + 1e-9)
		return 2 * p;
	if (m <= 5.0 + 1e-9)
		return 5 * p;
	return 10 * p;
}

static void update_label(Graph *g)
{
	char *t;

	if (!g->scale_label || !g->format)
		return;
	t = g->format(g->scale);
	gtk_label_set_text(g->scale_label, t);
	g_free(t);
}

static void rescale(Graph *g)
{
	double max = 0, scale;
	gint64 newest, from;

	if (g->fixed > 0) {
		scale = g->fixed;
	} else {
		newest = g->n ? sample_at(g, g->n - 1)->t : 0;
		from = newest - (gint64)TV_GRAPH_SECONDS * G_USEC_PER_SEC;
		for (int i = 0; i < g->n; i++) {
			const Sample *s = sample_at(g, i);
			if (s->t < from)
				continue;
			max = MAX(max, s->v[0]);
			if (g->dual)
				max = MAX(max, s->v[1]);
		}
		max = MAX(max, g->min_scale);
		if (g->binary) {
			/* In the unit the value is shown in: KB, MB, ... */
			double unit = 1;

			while (max / unit >= 1000.0)
				unit *= 1024.0;
			scale = nice_ceiling(max / unit);
			scale = scale >= 1000.0 ? 1024.0 * unit : scale * unit;
		} else {
			scale = nice_ceiling(max);
		}
	}
	if (scale != g->scale) {
		g->scale = scale;
		update_label(g);
	}
}

/* x of time `t' in a plot `w' wide whose right edge is `newest'. */
static double time_x(gint64 t, gint64 newest, double w)
{
	return w - (double)(newest - t) * w /
			   ((double)TV_GRAPH_SECONDS * G_USEC_PER_SEC);
}

static void trace(cairo_t *cr, const Graph *g, int k, gint64 newest, double w,
		  double h)
{
	gboolean first = TRUE;

	for (int i = 0; i < g->n; i++) {
		const Sample *s = sample_at(g, i);
		double v = s->v[k] / g->scale;
		double x = time_x(s->t, newest, w);

		if (i + 1 < g->n &&
		    time_x(sample_at(g, i + 1)->t, newest, w) < 0)
			continue; /* wholly left of the plot */
		v = CLAMP(v, 0.0, 1.0);
		if (first)
			cairo_move_to(cr, x, h - v * h);
		else
			cairo_line_to(cr, x, h - v * h);
		first = FALSE;
	}
}

static gboolean draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	Graph *g = data;
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	double width = gtk_widget_get_allocated_width(w);
	double height = gtk_widget_get_allocated_height(w);
	double pw, ph, lw;
	gint64 newest;

	gtk_render_background(sc, cr, 0, 0, width, height);
	if (width < 4 || height < 4)
		return FALSE;

	/* The plot inside a one-pixel frame, on pixel centres. */
	cairo_save(cr);
	cairo_translate(cr, 1, 1);
	pw = width - 2;
	ph = height - 2;
	cairo_rectangle(cr, 0, 0, pw, ph);
	cairo_clip(cr);

	newest = g->n ? sample_at(g, g->n - 1)->t : g_get_monotonic_time();

	if (g->style != TV_GRAPH_COMPACT) {
		int rows = g->style == TV_GRAPH_LARGE ? 10 : 4;
		gint64 step = (gint64)GRID_SECONDS * G_USEC_PER_SEC *
			      (g->style == TV_GRAPH_LARGE ? 1 : 2);
		gint64 t = newest - newest % step;

		cairo_set_source_rgba(cr, g->r, g->g, g->b, 0.16);
		cairo_set_line_width(cr, 1);
		for (int i = 1; i < rows; i++) {
			double y = floor(ph * i / rows) + 0.5;
			cairo_move_to(cr, 0, y);
			cairo_line_to(cr, pw, y);
		}
		for (; time_x(t, newest, pw) > 0; t -= step) {
			double x = floor(time_x(t, newest, pw)) + 0.5;
			cairo_move_to(cr, x, 0);
			cairo_line_to(cr, x, ph);
		}
		cairo_stroke(cr);
	}

	if (g->n > 0) {
		lw = g->style == TV_GRAPH_LARGE ? 1.5 : 1.0;
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

		/* The first series: the area, then its line. */
		trace(cr, g, 0, newest, pw, ph);
		cairo_line_to(cr, pw, ph);
		cairo_line_to(cr, MAX(time_x(sample_at(g, 0)->t, newest, pw), 0),
			      ph);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, g->r, g->g, g->b,
				      g->style == TV_GRAPH_COMPACT ? 0.28 : 0.18);
		cairo_fill(cr);
		trace(cr, g, 0, newest, pw, ph);
		cairo_set_source_rgb(cr, g->r, g->g, g->b);
		cairo_set_line_width(cr, lw);
		cairo_stroke(cr);

		if (g->dual) {
			static const double dash[] = { 4, 3 };
			trace(cr, g, 1, newest, pw, ph);
			cairo_set_dash(cr, dash, 2, 0);
			cairo_stroke(cr);
			cairo_set_dash(cr, NULL, 0, 0);
		}
	}
	cairo_restore(cr);

	/* The frame. */
	cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
	cairo_set_source_rgba(cr, g->r, g->g, g->b, 0.85);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	return FALSE;
}

GtkWidget *tv_graph_new(TvGraphStyle style)
{
	GtkWidget *w = gtk_drawing_area_new();
	Graph *g = g_new0(Graph, 1);

	g->style = style;
	g->r = 0.2;
	g->g = 0.45;
	g->b = 0.85;
	g->fixed = 100;
	g->scale = 100;
	g_object_set_data_full(G_OBJECT(w), "tv-graph", g, g_free);
	/* The view's own background: the theme's content colour, light or
	 * dark. */
	gtk_style_context_add_class(gtk_widget_get_style_context(w),
				    GTK_STYLE_CLASS_VIEW);
	gtk_style_context_add_class(gtk_widget_get_style_context(w),
				    "tv-graph");
	switch (style) {
	case TV_GRAPH_LARGE:
		gtk_widget_set_size_request(w, 240, 140);
		gtk_widget_set_hexpand(w, TRUE);
		gtk_widget_set_vexpand(w, TRUE);
		break;
	case TV_GRAPH_SMALL:
		gtk_widget_set_size_request(w, 60, 40);
		gtk_widget_set_hexpand(w, TRUE);
		gtk_widget_set_vexpand(w, TRUE);
		break;
	case TV_GRAPH_COMPACT:
		gtk_widget_set_size_request(w, 64, 42);
		break;
	}
	g_signal_connect(w, "draw", G_CALLBACK(draw), g);
	return w;
}

void tv_graph_set_color(GtkWidget *graph, double r, double g, double b)
{
	Graph *gr = graph_of(graph);

	gr->r = r;
	gr->g = g;
	gr->b = b;
	gtk_widget_queue_draw(graph);
}

void tv_graph_set_scale(GtkWidget *graph, double fixed, double min_scale)
{
	Graph *g = graph_of(graph);

	g->fixed = fixed;
	g->min_scale = min_scale;
	g->scale = -1; /* relabelled by rescale */
	rescale(g);
	gtk_widget_queue_draw(graph);
}

void tv_graph_set_binary(GtkWidget *graph, gboolean binary)
{
	Graph *g = graph_of(graph);

	g->binary = binary;
	rescale(g);
	gtk_widget_queue_draw(graph);
}

void tv_graph_set_dual(GtkWidget *graph, gboolean dual)
{
	Graph *g = graph_of(graph);

	g->dual = dual;
	rescale(g);
	gtk_widget_queue_draw(graph);
}

void tv_graph_set_scale_label(GtkWidget *graph, GtkLabel *label,
			      char *(*format)(double scale))
{
	Graph *g = graph_of(graph);

	g->scale_label = label;
	g->format = format;
	update_label(g);
}

void tv_graph_add(GtkWidget *graph, double v0, double v1)
{
	Graph *g = graph_of(graph);
	gint64 now = g_get_monotonic_time();
	Sample *s;

	/* Drop what fell off the left edge, keeping one sample beyond it so
	 * the curve reaches the edge. */
	while (g->n > 2 &&
	       sample_at(g, 1)->t <
		       now - (gint64)TV_GRAPH_SECONDS * G_USEC_PER_SEC) {
		g->head = (g->head + 1) % MAX_SAMPLES;
		g->n--;
	}
	if (g->n == MAX_SAMPLES) {
		g->head = (g->head + 1) % MAX_SAMPLES;
		g->n--;
	}
	s = &g->s[(g->head + g->n) % MAX_SAMPLES];
	s->t = now;
	s->v[0] = isfinite(v0) ? MAX(v0, 0) : 0;
	s->v[1] = isfinite(v1) ? MAX(v1, 0) : 0;
	g->n++;
	rescale(g);
	gtk_widget_queue_draw(graph);
}

double tv_graph_get_scale(GtkWidget *graph)
{
	return graph_of(graph)->scale;
}
