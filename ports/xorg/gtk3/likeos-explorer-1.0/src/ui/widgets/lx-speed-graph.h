// LikeOS: speed graph.
//
// A reusable LikeOS widget: the throughput of a running transfer as a
// sparkline.  The width stands for the whole transfer from start to end;
// a smooth accent line, 1.5 pixels wide, traces how fast each stretch went,
// over a soft accent wash that fades towards the baseline, and ends in a
// dot where the transfer is now.  A few faint rules lie under it, and the
// current speed is written at the top ("Speed: 257 MB/s").  The chart is a
// rounded white card with a hairline edge.
//
// The colours are the theme's named colours lx_graph_bg, lx_graph_line,
// lx_graph_fill, lx_graph_grid and lx_graph_border (@define-color), with
// the LikeOS palette as defaults.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LX_SPEED_GRAPH_H
#define LX_SPEED_GRAPH_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define LX_TYPE_SPEED_GRAPH (lx_speed_graph_get_type())
G_DECLARE_FINAL_TYPE(LxSpeedGraph, lx_speed_graph, LX, SPEED_GRAPH,
		     GtkDrawingArea)

GtkWidget *lx_speed_graph_new(void);
/* The speed (bytes per second) measured when `fraction' (0 to 1) of the
 * transfer was done.  Samples come in order of progress; one that would go
 * back is taken as belonging to where the graph already is. */
void lx_speed_graph_add_sample(LxSpeedGraph *g, double fraction, double rate);
/* How much is done: where the line ends. */
void lx_speed_graph_set_fraction(LxSpeedGraph *g, double fraction);
/* The current speed and the text written for it.  A rate of 0 hides the
 * end dot; `label' NULL hides the text. */
void lx_speed_graph_set_current(LxSpeedGraph *g, double rate,
				const char *label);
void lx_speed_graph_clear(LxSpeedGraph *g);

G_END_DECLS

#endif
