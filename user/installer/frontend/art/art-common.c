/* LikeOS installer -- shared drawing helpers and the illustration widget.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

void art_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r)
{
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	if (r < 0)
		r = 0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void art_set_rgb(cairo_t *cr, guint32 rgb, double alpha)
{
	cairo_set_source_rgba(cr, ((rgb >> 16) & 0xff) / 255.0,
			      ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0,
			      alpha);
}

static void add_stop(cairo_pattern_t *p, double off, guint32 rgb, double a)
{
	cairo_pattern_add_color_stop_rgba(p, off, ((rgb >> 16) & 0xff) / 255.0,
					  ((rgb >> 8) & 0xff) / 255.0,
					  (rgb & 0xff) / 255.0, a);
}

cairo_pattern_t *art_linear(double x0, double y0, double x1, double y1,
			    guint32 c0, guint32 c1)
{
	cairo_pattern_t *p = cairo_pattern_create_linear(x0, y0, x1, y1);

	add_stop(p, 0, c0, 1);
	add_stop(p, 1, c1, 1);
	return p;
}

void art_shadow(cairo_t *cr, double x, double y, double w, double h,
		double r, double blur, double alpha)
{
	const int layers = 14;

	cairo_save(cr);
	for (int i = layers; i >= 1; i--) {
		double g = blur * i / layers;

		art_rounded_rect(cr, x - g, y - g + blur * 0.35, w + 2 * g,
				 h + 2 * g, r + g);
		cairo_set_source_rgba(cr, 0.08, 0.12, 0.25,
				      alpha / layers * (1.0 - (double)i / (layers + 1)) * 2.0);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

void art_shadow_circle(cairo_t *cr, double cx, double cy, double radius,
		       double blur, double alpha)
{
	cairo_pattern_t *p;

	cairo_save(cr);
	p = cairo_pattern_create_radial(cx, cy + blur * 0.35, radius * 0.6, cx,
					cy + blur * 0.35, radius + blur);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.08, 0.12, 0.25, alpha);
	cairo_pattern_add_color_stop_rgba(p, (radius - radius * 0.6) /
					  (radius + blur - radius * 0.6),
					  0.08, 0.12, 0.25, alpha * 0.7);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.08, 0.12, 0.25, 0);
	cairo_set_source(cr, p);
	cairo_arc(cr, cx, cy + blur * 0.35, radius + blur, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	cairo_restore(cr);
}

static int g_anim = -1;

gboolean art_animations_enabled(void)
{
	if (g_anim < 0) {
		const char *e = g_getenv("LIKEOS_INSTALLER_NO_ANIMATION");

		g_anim = !(e && *e && *e != '0');
	}
	return g_anim;
}

void art_set_animations(gboolean on)
{
	g_anim = on ? 1 : 0;
}

double art_ease(double t)
{
	if (t <= 0)
		return 0;
	if (t >= 1)
		return 1;
	return t * t * (3 - 2 * t);
}

double art_bob(double phase, double amount, double shift)
{
	return sin((phase + shift) * 2 * M_PI) * amount;
}

void art_person(cairo_t *cr, double cx, double cy, double r, guint32 fg,
		double alpha)
{
	cairo_save(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_clip(cr);
	art_set_rgb(cr, fg, alpha);
	/* head */
	cairo_arc(cr, cx, cy - r * 0.18, r * 0.30, 0, 2 * M_PI);
	cairo_fill(cr);
	/* shoulders: a wide rounded body */
	art_rounded_rect(cr, cx - r * 0.56, cy + r * 0.22, r * 1.12, r * 0.80,
			 r * 0.34);
	cairo_fill(cr);
	cairo_restore(cr);
}

void art_draw(cairo_t *cr, ArtKind kind, double x, double y, double size,
	      double phase)
{
	if (size <= 1)
		return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, size, size);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (kind) {
	case ART_LOGO:          art_paint_logo(cr, phase); break;
	case ART_GLOBE:         art_paint_globe(cr, phase); break;
	case ART_KEYBOARD:      art_paint_keyboard(cr, phase); break;
	case ART_KEYBOARD_PLUS: art_paint_keyboard_plus(cr, phase); break;
	case ART_COMPUTER:      art_paint_computer(cr, phase); break;
	case ART_SHIELD:        art_paint_shield(cr, phase); break;
	case ART_AVATAR:        art_paint_avatar(cr, phase); break;
	case ART_AVATAR_KEY:    art_paint_avatar_key(cr, phase); break;
	case ART_DISK:          art_paint_disk(cr, phase); break;
	case ART_SUCCESS:       art_paint_success(cr, phase); break;
	case ART_WARNING:       art_paint_warning(cr, phase); break;
	case ART_SHAPES:        art_paint_shapes(cr, phase); break;
	default: break;
	}
	cairo_restore(cr);
}

GdkPixbuf *art_pixbuf(ArtKind kind, int size)
{
	cairo_surface_t *s;
	cairo_t *cr;
	GdkPixbuf *pb;

	if (size < 1)
		size = 1;
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(s);
	art_draw(cr, kind, 0, 0, size, 0.25);
	cairo_destroy(cr);
	pb = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
	cairo_surface_destroy(s);
	return pb;
}

/* ---- the illustration widget --------------------------------------- */

typedef struct {
	ArtKind kind;
	ArtKind old_kind;
	gint64 fade_start;     /* monotonic us, 0 = no fade */
	gint64 epoch;
	gint64 last_frame;
	guint tick;
	gboolean animated;
	double phase;
} ArtView;

#define ART_FADE_US   320000
#define ART_PERIOD_US 6000000.0

static ArtView *view_data(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "art-view");
}

static gboolean view_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	ArtView *v = data;
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	double size = MIN(aw, ah);
	double x = (aw - size) / 2.0, y = (ah - size) / 2.0;
	double f = 1.0;

	if (v->fade_start) {
		gint64 now = g_get_monotonic_time();

		f = (double)(now - v->fade_start) / ART_FADE_US;
		if (f >= 1.0) {
			f = 1.0;
			v->fade_start = 0;
		}
		f = art_ease(f);
	}
	if (f < 1.0 && v->old_kind != ART_NONE) {
		cairo_push_group(cr);
		art_draw(cr, v->old_kind, x, y - 12 * f, size, v->phase);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, 1.0 - f);
	}
	if (v->kind != ART_NONE) {
		cairo_push_group(cr);
		art_draw(cr, v->kind, x, y + 12 * (1.0 - f), size, v->phase);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, f);
	}
	return FALSE;
}

static gboolean view_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	ArtView *v = data;
	gint64 now = gdk_frame_clock_get_frame_time(clock);

	if (!v->epoch)
		v->epoch = now;
	if (v->fade_start) {
		gtk_widget_queue_draw(w);
		return G_SOURCE_CONTINUE;
	}
	/* The float is slow; 20 frames a second are plenty and spare a
	 * machine that paints in software. */
	if (v->animated && art_animations_enabled() &&
	    now - v->last_frame >= 50000) {
		v->last_frame = now;
		v->phase = fmod((double)(now - v->epoch) / ART_PERIOD_US, 1.0);
		gtk_widget_queue_draw(w);
	}
	return G_SOURCE_CONTINUE;
}

static void view_map(GtkWidget *w, gpointer data)
{
	ArtView *v = data;

	if (!v->tick)
		v->tick = gtk_widget_add_tick_callback(w, view_tick, v, NULL);
}

static void view_unmap(GtkWidget *w, gpointer data)
{
	ArtView *v = data;

	if (v->tick) {
		gtk_widget_remove_tick_callback(w, v->tick);
		v->tick = 0;
	}
}

GtkWidget *art_view_new(ArtKind kind)
{
	GtkWidget *w = gtk_drawing_area_new();
	ArtView *v = g_new0(ArtView, 1);

	v->kind = kind;
	v->animated = TRUE;
	g_object_set_data_full(G_OBJECT(w), "art-view", v, g_free);
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "il-art");
	g_signal_connect(w, "draw", G_CALLBACK(view_draw), v);
	g_signal_connect(w, "map", G_CALLBACK(view_map), v);
	g_signal_connect(w, "unmap", G_CALLBACK(view_unmap), v);
	return w;
}

void art_view_set(GtkWidget *w, ArtKind kind)
{
	ArtView *v = view_data(w);

	if (!v || v->kind == kind)
		return;
	v->old_kind = v->kind;
	v->kind = kind;
	v->fade_start = gtk_widget_get_mapped(w) ? g_get_monotonic_time() : 0;
	gtk_widget_queue_draw(w);
}

ArtKind art_view_get(GtkWidget *w)
{
	ArtView *v = view_data(w);

	return v ? v->kind : ART_NONE;
}

void art_view_set_animated(GtkWidget *w, gboolean animated)
{
	ArtView *v = view_data(w);

	if (v)
		v->animated = animated;
}
