/* LikeOS installer -- a determinate ring.
 *
 * The fraction is drawn as an arc from the top, clockwise, over a faint
 * track; the shown value glides towards the set one, so the coarse steps of
 * a progress report still look smooth.  The number inside is the
 * percentage set with progress_ring_set_progress() (so that it is the same
 * as every other place showing it), else the arc's own.
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
	double target;
	double shown;
	int percent;             /* -1: from the arc */
	gboolean show_text;
	guint tick;
	gint64 last;
} Ring;

static gboolean ring_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	Ring *r = data;
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	double size = MIN(MIN(aw, ah), r->size);
	double cx = aw / 2.0, cy = ah / 2.0;
	double lw = MAX(3.0, size / 14.0);
	double rad = size / 2.0 - lw / 2 - 1;
	GdkRGBA c;

	gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, lw);
	cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * 0.18);
	cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
	cairo_stroke(cr);

	if (r->shown > 0.001) {
		cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
		cairo_arc(cr, cx, cy, rad, -M_PI / 2,
			  -M_PI / 2 + 2 * M_PI * MIN(1.0, r->shown));
		cairo_stroke(cr);
	}

	if (r->show_text) {
		PangoLayout *layout;
		PangoFontDescription *fd;
		char text[16];
		int tw, th;

		g_snprintf(text, sizeof(text), "%d%%", r->percent >= 0 ? r->percent :
			   (int)floor(r->shown * 100 + 0.5));
		layout = gtk_widget_create_pango_layout(w, text);
		fd = pango_font_description_copy(
			pango_context_get_font_description(
				gtk_widget_get_pango_context(w)));
		pango_font_description_set_absolute_size(fd, size * 0.22 * PANGO_SCALE);
		pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
		pango_layout_set_font_description(layout, fd);
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
		cairo_move_to(cr, cx - tw / 2.0, cy - th / 2.0);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(fd);
		g_object_unref(layout);
	}
	return FALSE;
}

static gboolean ring_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	Ring *r = data;
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	double dt = r->last ? (now - r->last) / 1e6 : 0.016;

	r->last = now;
	if (fabs(r->target - r->shown) < 0.0005) {
		if (r->shown != r->target) {
			r->shown = r->target;
			gtk_widget_queue_draw(w);
		}
		r->tick = 0;
		r->last = 0;
		return G_SOURCE_REMOVE;
	}
	/* close a quarter of the gap every 60th of a second */
	r->shown += (r->target - r->shown) * MIN(1.0, dt * 15.0);
	gtk_widget_queue_draw(w);
	return G_SOURCE_CONTINUE;
}

GtkWidget *progress_ring_new(int size)
{
	GtkWidget *w = gtk_drawing_area_new();
	Ring *r = g_new0(Ring, 1);

	r->size = size;
	r->percent = -1;
	r->show_text = TRUE;
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "il-progress-ring");
	g_object_set_data_full(G_OBJECT(w), "ring", r, g_free);
	g_signal_connect(w, "draw", G_CALLBACK(ring_draw), r);
	return w;
}

void progress_ring_set_fraction(GtkWidget *w, double fraction)
{
	Ring *r = g_object_get_data(G_OBJECT(w), "ring");

	if (!r)
		return;
	r->target = CLAMP(fraction, 0.0, 1.0);
	if (fraction < r->shown - 0.2 || !gtk_widget_get_mapped(w) ||
	    !art_animations_enabled()) {
		r->shown = r->target;
		gtk_widget_queue_draw(w);
		return;
	}
	if (!r->tick)
		r->tick = gtk_widget_add_tick_callback(w, ring_tick, r, NULL);
}

void progress_ring_set_progress(GtkWidget *w, double fraction, int percent)
{
	Ring *r = g_object_get_data(G_OBJECT(w), "ring");

	if (!r)
		return;
	if (r->percent != percent) {
		r->percent = CLAMP(percent, 0, 100);
		gtk_widget_queue_draw(w);
	}
	progress_ring_set_fraction(w, fraction);
}

double progress_ring_get_fraction(GtkWidget *w)
{
	Ring *r = g_object_get_data(G_OBJECT(w), "ring");

	return r ? r->target : 0.0;
}

void progress_ring_set_size(GtkWidget *w, int size)
{
	Ring *r = g_object_get_data(G_OBJECT(w), "ring");

	if (r && r->size != size) {
		r->size = size;
		gtk_widget_set_size_request(w, size, size);
		gtk_widget_queue_resize(w);
	}
}

void progress_ring_set_show_text(GtkWidget *w, gboolean show)
{
	Ring *r = g_object_get_data(G_OBJECT(w), "ring");

	if (r) {
		r->show_text = show;
		gtk_widget_queue_draw(w);
	}
}
