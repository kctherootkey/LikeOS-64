// LikeOS: speed graph.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/widgets/lx-speed-graph.h"

#include <math.h>

/* Faint rules across the chart, between the baseline and the top. */
#define GRID_ROWS 4
/* Samples closer together than this share a place on the graph: the chart
 * is a few hundred pixels wide, so a thousand places are more than every
 * pixel column needs. */
#define SAMPLE_STEP 0.001
/* The highest speed seen sits at two thirds of the height, which leaves
 * room above it for the label and for the speed to grow. */
#define HEADROOM 1.5
/* The card's corners, and how far the text and the line keep from its
 * edges. */
#define RADIUS 6
#define INSET 8
#define LINE_WIDTH 1.5

typedef struct {
	double f, rate;
} Sample;

struct _LxSpeedGraph {
	GtkDrawingArea parent;
	GArray *samples; /* Sample, in order of progress */
	double fraction;
	double current;
	double peak;
	char *label;
};

G_DEFINE_TYPE(LxSpeedGraph, lx_speed_graph, GTK_TYPE_DRAWING_AREA)

static void lx_speed_graph_finalize(GObject *o)
{
	LxSpeedGraph *g = LX_SPEED_GRAPH(o);

	g_array_unref(g->samples);
	g_free(g->label);
	G_OBJECT_CLASS(lx_speed_graph_parent_class)->finalize(o);
}

/* A colour the theme names, or the LikeOS palette's. */
static void color(GtkWidget *w, const char *name, const char *fallback,
		  GdkRGBA *out)
{
	if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w),
					    name, out))
		gdk_rgba_parse(out, fallback);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h,
		    double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -0.5 * G_PI, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * G_PI);
	cairo_arc(cr, x + r, y + h - r, r, 0.5 * G_PI, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
}

/* A quadratic Bézier from the current point through control (cx, cy) to
 * (x, y), as cairo's cubic. */
static void quad_to(cairo_t *cr, double cx, double cy, double x, double y)
{
	double x0, y0;

	cairo_get_current_point(cr, &x0, &y0);
	cairo_curve_to(cr, x0 + 2.0 / 3 * (cx - x0), y0 + 2.0 / 3 * (cy - y0),
		       x + 2.0 / 3 * (cx - x), y + 2.0 / 3 * (cy - y), x, y);
}

/* The line through the samples, smoothed: it runs from one midpoint
 * between two samples to the next, each sample the control point between.
 * It starts at the left edge level with the first sample -- the first
 * speed is known only after a moment -- and ends level with the last one
 * at `xe', where the transfer is; `*ye' is that height. */
static void trace(cairo_t *cr, LxSpeedGraph *g, double x0, double w,
		  double bottom, double h, double top, double xe, double *ye)
{
	Sample *first = &g_array_index(g->samples, Sample, 0);
	double px = x0, py = bottom - h * MIN(first->rate / top, 1.0);

	cairo_move_to(cr, px, py);
	for (guint i = 0; i < g->samples->len; i++) {
		Sample *s = &g_array_index(g->samples, Sample, i);
		double x = x0 + w * s->f;
		double y = bottom - h * MIN(s->rate / top, 1.0);

		quad_to(cr, px, py, (px + x) / 2, (py + y) / 2);
		px = x;
		py = y;
	}
	quad_to(cr, px, py, MAX(xe, px), py);
	*ye = py;
}

static gboolean lx_speed_graph_draw(GtkWidget *w, cairo_t *cr)
{
	LxSpeedGraph *g = LX_SPEED_GRAPH(w);
	int W = gtk_widget_get_allocated_width(w);
	int H = gtk_widget_get_allocated_height(w);
	GdkRGBA bg, line, fill, grid, border, text;
	/* The plot: inside the card, under the text's line. */
	double x0 = INSET, iw = W - 2 * INSET;
	double y0 = 1 + INSET + 14, bottom = H - 1 - INSET / 2;
	double ih = bottom - y0;
	double xd, top, ye = bottom;

	if (iw <= 0 || ih <= 0)
		return FALSE;
	color(w, "lx_graph_bg", "#ffffff", &bg);
	color(w, "lx_graph_line", "#3b74f0", &line);
	color(w, "lx_graph_fill", "#3b74f0", &fill);
	color(w, "lx_graph_grid", "#e6eaf1", &grid);
	color(w, "lx_graph_border", "#e1e6ef", &border);
	gtk_style_context_get_color(gtk_widget_get_style_context(w),
				    gtk_widget_get_state_flags(w), &text);

	/* The card. */
	rounded(cr, 0.5, 0.5, W - 1, H - 1, RADIUS);
	gdk_cairo_set_source_rgba(cr, &bg);
	cairo_fill_preserve(cr);
	gdk_cairo_set_source_rgba(cr, &border);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	/* The rules, dashed, on whole pixels. */
	cairo_save(cr);
	gdk_cairo_set_source_rgba(cr, &grid);
	cairo_set_dash(cr, (double[]){ 2, 3 }, 2, 0);
	for (int j = 1; j < GRID_ROWS; j++) {
		double y = floor(y0 + ih * j / GRID_ROWS) + 0.5;

		cairo_move_to(cr, x0, y);
		cairo_line_to(cr, x0 + iw, y);
	}
	cairo_stroke(cr);
	cairo_restore(cr);

	xd = x0 + iw * CLAMP(g->fraction, 0.0, 1.0);
	top = g->peak * HEADROOM;
	if (top > 0 && g->samples->len) {
		cairo_pattern_t *wash;

		cairo_save(cr);
		rounded(cr, 0.5, 0.5, W - 1, H - 1, RADIUS);
		cairo_clip(cr);
		/* The wash under the line: the accent, fading downwards. */
		trace(cr, g, x0, iw, bottom, ih, top, xd, &ye);
		cairo_line_to(cr, xd, H);
		cairo_line_to(cr, x0, H);
		cairo_close_path(cr);
		wash = cairo_pattern_create_linear(0, y0, 0, H);
		cairo_pattern_add_color_stop_rgba(wash, 0, fill.red, fill.green,
						  fill.blue, 0.30);
		cairo_pattern_add_color_stop_rgba(wash, 1, fill.red, fill.green,
						  fill.blue, 0.02);
		cairo_set_source(cr, wash);
		cairo_fill(cr);
		cairo_pattern_destroy(wash);
		/* The line itself. */
		trace(cr, g, x0, iw, bottom, ih, top, xd, &ye);
		gdk_cairo_set_source_rgba(cr, &line);
		cairo_set_line_width(cr, LINE_WIDTH);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
		cairo_restore(cr);
		/* Where the transfer is now: a dot on a white ring. */
		if (g->current > 0) {
			cairo_arc(cr, xd, ye, 4, 0, 2 * G_PI);
			gdk_cairo_set_source_rgba(cr, &bg);
			cairo_fill(cr);
			cairo_arc(cr, xd, ye, 2.75, 0, 2 * G_PI);
			gdk_cairo_set_source_rgba(cr, &line);
			cairo_fill(cr);
		}
	}

	/* The current speed (or why there is none), at the top left. */
	if (g->label) {
		PangoLayout *pl = gtk_widget_create_pango_layout(w, g->label);

		gdk_cairo_set_source_rgba(cr, &text);
		cairo_move_to(cr, x0 + 2, 1 + INSET - 3);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
	}
	return FALSE;
}

/* 84 pixels tall, as wide as the window lets it be. */
static void lx_speed_graph_get_preferred_width(GtkWidget *w, int *min,
					       int *nat)
{
	*min = 120;
	*nat = 395;
}

static void lx_speed_graph_get_preferred_height(GtkWidget *w, int *min,
						int *nat)
{
	*min = *nat = 84;
}

static void lx_speed_graph_class_init(LxSpeedGraphClass *k)
{
	GtkWidgetClass *wk = GTK_WIDGET_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = lx_speed_graph_finalize;
	wk->draw = lx_speed_graph_draw;
	wk->get_preferred_width = lx_speed_graph_get_preferred_width;
	wk->get_preferred_height = lx_speed_graph_get_preferred_height;
	gtk_widget_class_set_css_name(wk, "speedgraph");
	gtk_widget_class_set_accessible_role(wk, ATK_ROLE_CHART);
}

static void lx_speed_graph_init(LxSpeedGraph *g)
{
	g->samples = g_array_new(FALSE, FALSE, sizeof(Sample));
}

GtkWidget *lx_speed_graph_new(void)
{
	return g_object_new(LX_TYPE_SPEED_GRAPH, NULL);
}

void lx_speed_graph_add_sample(LxSpeedGraph *g, double fraction, double rate)
{
	Sample s = { CLAMP(fraction, 0.0, 1.0), MAX(rate, 0.0) };

	if (g->samples->len) {
		Sample *last = &g_array_index(g->samples, Sample,
					      g->samples->len - 1);

		if (s.f < last->f + SAMPLE_STEP) {
			/* The same place: the two measurements meet. */
			last->rate = (last->rate + s.rate) / 2;
			s.rate = last->rate;
			goto peak;
		}
	}
	g_array_append_val(g->samples, s);
peak:
	if (s.rate > g->peak)
		g->peak = s.rate;
	gtk_widget_queue_draw(GTK_WIDGET(g));
}

void lx_speed_graph_set_fraction(LxSpeedGraph *g, double fraction)
{
	fraction = CLAMP(fraction, 0.0, 1.0);
	if (fraction == g->fraction)
		return;
	g->fraction = fraction;
	gtk_widget_queue_draw(GTK_WIDGET(g));
}

void lx_speed_graph_set_current(LxSpeedGraph *g, double rate,
				const char *label)
{
	g->current = MAX(rate, 0.0);
	if (g->current > g->peak)
		g->peak = g->current;
	if (g_strcmp0(label, g->label)) {
		g_free(g->label);
		g->label = g_strdup(label);
		atk_object_set_name(gtk_widget_get_accessible(GTK_WIDGET(g)),
				    label ? label : "");
	}
	gtk_widget_queue_draw(GTK_WIDGET(g));
}

void lx_speed_graph_clear(LxSpeedGraph *g)
{
	g_array_set_size(g->samples, 0);
	g->fraction = 0;
	g->current = 0;
	g->peak = 0;
	gtk_widget_queue_draw(GTK_WIDGET(g));
}
