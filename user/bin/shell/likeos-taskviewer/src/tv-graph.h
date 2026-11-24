// LikeOS Task Viewer -- the history graph.
//
// A drawing area showing the last 60 seconds of one or two series: the
// first as a line over a filled area, the second as a dashed line over it
// (read and write, send and receive), both in the resource's colour, over a
// light grid that moves along with the time.  The scale is either fixed
// (percentages) or follows the largest value shown, rounded up to a 1-2-5
// step, and a label may be given the graph keeps in step with it.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_GRAPH_H
#define TV_GRAPH_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
	TV_GRAPH_LARGE, /* the main graph: full grid */
	TV_GRAPH_SMALL, /* one of many (a logical processor): coarse grid */
	TV_GRAPH_COMPACT, /* the side list: no grid */
} TvGraphStyle;

#define TV_GRAPH_SECONDS 60

GtkWidget *tv_graph_new(TvGraphStyle style);

/* The series colour (0..1 each). */
void tv_graph_set_color(GtkWidget *graph, double r, double g, double b);

/* A fixed scale (100 for percentages), or 0 for one that follows the
 * values and never drops below `min_scale'. */
void tv_graph_set_scale(GtkWidget *graph, double fixed, double min_scale);

/* Rounds an automatic scale to 1-2-5 steps of the powers of 1024 (bytes)
 * rather than of 1000 (bits, percentages). */
void tv_graph_set_binary(GtkWidget *graph, gboolean binary);

/* Whether the second, dashed series is drawn. */
void tv_graph_set_dual(GtkWidget *graph, gboolean dual);

/* A label the graph keeps showing its scale in, formatted by `format'
 * (returns a new string). */
void tv_graph_set_scale_label(GtkWidget *graph, GtkLabel *label,
			      char *(*format)(double scale));

/* Adds a sample of both series (the second is ignored unless dual), taken
 * now. */
void tv_graph_add(GtkWidget *graph, double v0, double v1);

/* The scale the graph is drawn at now. */
double tv_graph_get_scale(GtkWidget *graph);

G_END_DECLS

#endif
