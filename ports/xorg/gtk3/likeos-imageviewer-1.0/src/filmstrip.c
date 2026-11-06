// LikeOS -- LikeOS Image Viewer: the filmstrip.
//
// A row of thumbnails of the folder above the bottom strip.  It is one
// drawing area as wide as all the cells together inside a scrolled window,
// and draws only the cells in view, so a folder of ten thousand pictures
// costs no more than one of ten; the thumbnails come from the shared
// thumbnail cache through a worker thread (thumbs.c) and appear as they
// arrive.  The current picture is outlined in the accent colour and kept
// in view: the row glides to it whenever it changes.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "imageviewer.h"

#define CELL_W 80
#define CELL_H 60
#define GAP 6
#define PAD 8
#define THUMB_PX 160	/* thumbnails are kept at twice the cell size */
#define GLIDE_MS 180

struct IvFilmstrip {
	IvWindow *w;
	GtkWidget *root, *scroller, *area;
	IvThumbs *thumbs;
	int current, hover;
	guint glide_tick;
	gint64 glide_t0;
	double glide_from, glide_to;
	double press_x;
	gboolean pressed;
};

static int count(IvFilmstrip *f)
{
	return iv_folder_count(f->w->folder);
}

static double cell_x(int i)
{
	return PAD + i * (double)(CELL_W + GAP);
}

static int cell_at(IvFilmstrip *f, double x, double y)
{
	int i = (int)floor((x - PAD) / (CELL_W + GAP));

	if (i < 0 || i >= count(f) || x - cell_x(i) > CELL_W || y < PAD ||
	    y > PAD + CELL_H)
		return -1;
	return i;
}

static void resize_area(IvFilmstrip *f)
{
	int n = count(f);
	int width = n > 0 ? (int)cell_x(n) - GAP + PAD : 0;

	gtk_widget_set_size_request(f->area, width, CELL_H + 2 * PAD);
	gtk_widget_queue_draw(f->area);
}

static void style_color(GtkWidget *w, const char *cls, GdkRGBA *out)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);

	gtk_style_context_save(sc);
	gtk_style_context_add_class(sc, cls);
	gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), out);
	gtk_style_context_restore(sc);
}

static gboolean draw(GtkWidget *wid, cairo_t *cr, IvFilmstrip *f)
{
	GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(f->scroller));
	double x0 = gtk_adjustment_get_value(adj);
	double x1 = x0 + gtk_adjustment_get_page_size(adj);
	int n = count(f);
	int first = MAX(0, (int)floor((x0 - PAD) / (CELL_W + GAP)) - 1);
	int last = MIN(n - 1, (int)ceil((x1 - PAD) / (CELL_W + GAP)) + 1);
	GdkRGBA accent, place, glyph, hover;

	style_color(wid, "iv-film-accent", &accent);
	style_color(wid, "iv-film-placeholder", &place);
	style_color(wid, "iv-film-glyph", &glyph);
	style_color(wid, "iv-film-hover", &hover);
	for (int i = first; i <= last; i++) {
		const char *path = iv_folder_path(f->w->folder, i);
		GdkPixbuf *pb = path ? iv_thumbs_get(f->thumbs, path) : NULL;
		double x = cell_x(i), y = PAD;

		cairo_save(cr);
		iv_rounded_rect(cr, x, y, CELL_W, CELL_H, 6);
		cairo_clip(cr);
		gdk_cairo_set_source_rgba(cr, &place);
		cairo_paint(cr);
		if (pb) {
			/* Filling the cell, cut to it. */
			double pw = gdk_pixbuf_get_width(pb);
			double ph = gdk_pixbuf_get_height(pb);
			double k = MAX(CELL_W / pw, CELL_H / ph);

			cairo_translate(cr, x + (CELL_W - pw * k) / 2,
					y + (CELL_H - ph * k) / 2);
			cairo_scale(cr, k, k);
			gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr),
						 CAIRO_FILTER_GOOD);
			cairo_paint(cr);
		} else {
			gdk_cairo_set_source_rgba(cr, &glyph);
			iv_glyph_paint(cr, IV_GLYPH_IMAGE, x + CELL_W / 2.0,
				       y + CELL_H / 2.0, 20);
		}
		cairo_restore(cr);
		if (i == f->hover && i != f->current) {
			iv_rounded_rect(cr, x, y, CELL_W, CELL_H, 6);
			gdk_cairo_set_source_rgba(cr, &hover);
			cairo_fill(cr);
		}
		if (i == f->current) {
			iv_rounded_rect(cr, x - 2.5, y - 2.5, CELL_W + 5, CELL_H + 5,
					8);
			gdk_cairo_set_source_rgba(cr, &accent);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
	}
	return TRUE;
}

/* ---- keeping the current one in view ----------------------------------------- */

static gboolean glide(GtkWidget *wid, GdkFrameClock *clock, gpointer data)
{
	IvFilmstrip *f = data;
	GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(f->scroller));
	double t = (gdk_frame_clock_get_frame_time(clock) - f->glide_t0) /
		   (GLIDE_MS * 1000.0);
	double e;

	t = CLAMP(t, 0, 1);
	e = 1 - (1 - t) * (1 - t);
	gtk_adjustment_set_value(adj, f->glide_from + (f->glide_to - f->glide_from) * e);
	if (t >= 1) {
		f->glide_tick = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void bring_into_view(IvFilmstrip *f, gboolean animate)
{
	GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(f->scroller));
	double page = gtk_adjustment_get_page_size(adj);
	double upper = gtk_adjustment_get_upper(adj);
	double target;

	if (f->current < 0 || page <= 0)
		return;
	target = cell_x(f->current) + CELL_W / 2.0 - page / 2;
	target = CLAMP(target, 0, MAX(0, upper - page));
	if (f->glide_tick) {
		gtk_widget_remove_tick_callback(f->area, f->glide_tick);
		f->glide_tick = 0;
	}
	if (!animate || !gtk_widget_get_mapped(f->area)) {
		gtk_adjustment_set_value(adj, target);
		return;
	}
	f->glide_from = gtk_adjustment_get_value(adj);
	f->glide_to = target;
	f->glide_t0 = gdk_frame_clock_get_frame_time(
		gtk_widget_get_frame_clock(f->area));
	f->glide_tick = gtk_widget_add_tick_callback(f->area, glide, f, NULL);
}

static void mapped(GtkWidget *wid, IvFilmstrip *f)
{
	bring_into_view(f, FALSE);
}

static void scrolled(GtkAdjustment *adj, IvFilmstrip *f)
{
	gtk_widget_queue_draw(f->area);
}

/* ---- input ------------------------------------------------------------------------ */

static gboolean press(GtkWidget *wid, GdkEventButton *ev, IvFilmstrip *f)
{
	if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS) {
		f->pressed = TRUE;
		f->press_x = ev->x;
	}
	return TRUE;
}

static gboolean release(GtkWidget *wid, GdkEventButton *ev, IvFilmstrip *f)
{
	int i;

	if (ev->button != 1 || !f->pressed)
		return FALSE;
	f->pressed = FALSE;
	i = cell_at(f, ev->x, ev->y);
	if (i >= 0 && fabs(ev->x - f->press_x) < 8)
		iv_viewer_go_to(f->w, i);
	return TRUE;
}

static gboolean motion(GtkWidget *wid, GdkEventMotion *ev, IvFilmstrip *f)
{
	int i = cell_at(f, ev->x, ev->y);

	if (i != f->hover) {
		f->hover = i;
		gtk_widget_queue_draw(wid);
		if (i >= 0) {
			char *name = g_filename_display_basename(
				iv_folder_path(f->w->folder, i));

			gtk_widget_set_tooltip_text(wid, name);
			g_free(name);
		} else {
			gtk_widget_set_tooltip_text(wid, NULL);
		}
	}
	return FALSE;
}

static gboolean leave(GtkWidget *wid, GdkEventCrossing *ev, IvFilmstrip *f)
{
	f->hover = -1;
	gtk_widget_queue_draw(wid);
	return FALSE;
}

/* The wheel's up and down move the row sideways. */
static gboolean scroll(GtkWidget *wid, GdkEventScroll *ev, IvFilmstrip *f)
{
	GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(
		GTK_SCROLLED_WINDOW(f->scroller));
	double dx = 0, dy = 0, step = CELL_W + GAP;

	switch (ev->direction) {
	case GDK_SCROLL_UP:
	case GDK_SCROLL_LEFT:
		dy = -1;
		break;
	case GDK_SCROLL_DOWN:
	case GDK_SCROLL_RIGHT:
		dy = 1;
		break;
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &dy);
		dy += dx;
		step = 40;
		break;
	}
	gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + dy * step);
	return TRUE;
}

static void thumb_ready(const char *path, GdkPixbuf *thumb, gpointer data)
{
	IvFilmstrip *f = data;

	gtk_widget_queue_draw(f->area);
}

/* ---- the interface ------------------------------------------------------------------ */

IvFilmstrip *iv_filmstrip_new(IvWindow *w)
{
	IvFilmstrip *f = g_new0(IvFilmstrip, 1);
	GtkAdjustment *adj;

	f->w = w;
	f->current = -1;
	f->hover = -1;
	f->thumbs = iv_thumbs_new(THUMB_PX, thumb_ready, f);
	f->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	iv_add_class(f->root, "iv-filmstrip");
	f->scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(f->scroller),
				       GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(f->scroller),
						  TRUE);
	f->area = gtk_drawing_area_new();
	iv_add_class(f->area, "iv-film-area");
	gtk_widget_add_events(f->area, GDK_BUTTON_PRESS_MASK |
					       GDK_BUTTON_RELEASE_MASK |
					       GDK_POINTER_MOTION_MASK |
					       GDK_LEAVE_NOTIFY_MASK |
					       GDK_SCROLL_MASK |
					       GDK_SMOOTH_SCROLL_MASK);
	g_signal_connect(f->area, "draw", G_CALLBACK(draw), f);
	g_signal_connect(f->area, "button-press-event", G_CALLBACK(press), f);
	g_signal_connect(f->area, "button-release-event", G_CALLBACK(release), f);
	g_signal_connect(f->area, "motion-notify-event", G_CALLBACK(motion), f);
	g_signal_connect(f->area, "leave-notify-event", G_CALLBACK(leave), f);
	g_signal_connect(f->area, "scroll-event", G_CALLBACK(scroll), f);
	g_signal_connect(f->area, "map", G_CALLBACK(mapped), f);
	gtk_container_add(GTK_CONTAINER(f->scroller), f->area);
	adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(f->scroller));
	g_signal_connect(adj, "value-changed", G_CALLBACK(scrolled), f);
	g_signal_connect(adj, "changed", G_CALLBACK(scrolled), f);
	gtk_box_pack_start(GTK_BOX(f->root), f->scroller, TRUE, TRUE, 0);
	gtk_widget_set_size_request(f->scroller, -1, CELL_H + 2 * PAD);
	gtk_widget_show_all(f->root);
	return f;
}

void iv_filmstrip_free(IvFilmstrip *f)
{
	if (!f)
		return;
	iv_thumbs_free(f->thumbs);
	g_free(f);
}

GtkWidget *iv_filmstrip_widget(IvFilmstrip *f)
{
	return f->root;
}

void iv_filmstrip_reload(IvFilmstrip *f)
{
	if (!f)
		return;
	/* What was queued for the old list is not needed any more. */
	iv_thumbs_clear_queue(f->thumbs);
	if (f->current >= count(f))
		f->current = count(f) - 1;
	resize_area(f);
}

void iv_filmstrip_set_current(IvFilmstrip *f, int index, gboolean animate)
{
	if (!f)
		return;
	f->current = index;
	gtk_widget_queue_draw(f->area);
	bring_into_view(f, animate);
}

void iv_filmstrip_forget(IvFilmstrip *f, const char *path)
{
	if (!f)
		return;
	iv_thumbs_forget(f->thumbs, path);
	gtk_widget_queue_draw(f->area);
}
