// LikeOS -- LikeOS Image Viewer: the canvas.
//
// The view is a zoom and the image point that sits in the middle of the
// viewport (the canvas less the room the floating chrome keeps clear).
// Fit-to-window recomputes the zoom whenever the canvas changes size; any
// other zoom keeps its scale and only keeps the picture from being dragged
// out of sight.  Zooming keeps the image point under the pointer where it
// is, and is animated over a few frames (geometrically, so a step from 25 %
// to 50 % feels like one from 200 % to 400 %).
//
// Drawing picks the prescaled level of the picture closest above the zoom
// and lets cairo do the last step, with its good filter when the view is
// still and the fast one while it moves; zoomed in far enough to see the
// pixels, it draws them as squares.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "canvas.h"

#define IV_FADE_MS 320
#define IV_ZOOM_ANIM_MS 160
#define IV_SPINNER_DELAY_MS 260
#define IV_PIXEL_ZOOM 3.0	/* from here on pixels are drawn as squares */
#define IV_WHEEL_FACTOR 1.25	/* one wheel notch */

enum {
	SIG_VIEW_CHANGED,
	SIG_IMAGE_ACTIVATED,
	SIG_NAVIGATE,
	SIG_CONTEXT_MENU,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _IvCanvas {
	GtkDrawingArea parent;

	/* What is shown. */
	IvPicture *pic;
	cairo_surface_t *surface;	/* the editor's display */
	int lw, lh;			/* logical image size, 0: nothing */
	gboolean alpha;

	/* Animation (GIF). */
	GdkPixbufAnimationIter *iter;
	cairo_surface_t *frame;
	guint frame_timer;
	gboolean playing;

	/* The view. */
	gboolean fit;
	double zoom;
	double cx, cy;			/* image point in the viewport middle */
	int inset_t, inset_r, inset_b, inset_l;

	/* An animated zoom. */
	guint zoom_tick;
	gint64 zoom_t0;
	double z_from, z_to;
	double anchor_wx, anchor_wy, anchor_ix, anchor_iy;
	gboolean zoom_ends_fit;

	/* Dragging. */
	gboolean drag_pan;
	gboolean pressed, dragging;
	double press_x, press_y, last_x, last_y;
	guint settle_timer;
	gboolean moving;		/* fast filter while the view moves */
	double wheel_acc;

	/* The cross-fade from the previous picture. */
	cairo_surface_t *fade_from;
	guint fade_tick;
	gint64 fade_t0;
	double fade;

	/* The spinner. */
	gboolean loading;
	guint loading_delay;
	gboolean spinner_on;
	guint spin_tick;
	double spin_phase;

	/* The message when there is nothing to show. */
	IvGlyph msg_glyph;
	char *msg_title, *msg_detail;

	IvBackdrop backdrop;
	cairo_pattern_t *checker;

	guint view_idle;		/* "view-changed" after an allocation */

	IvCanvasOverlay overlay_hook;
	IvCanvasEvent event_hook;
	gpointer hook_data;
	char *cursor_name;
	const char *cursor_shown;
};

G_DEFINE_TYPE(IvCanvas, iv_canvas, GTK_TYPE_DRAWING_AREA)

static void update_cursor(IvCanvas *c);
static void clamp_view(IvCanvas *c);

/* ---- geometry ------------------------------------------------------------- */

static void viewport(IvCanvas *c, double *x, double *y, double *w, double *h)
{
	GtkWidget *wid = GTK_WIDGET(c);
	double aw = gtk_widget_get_allocated_width(wid);
	double ah = gtk_widget_get_allocated_height(wid);

	*x = c->inset_l;
	*y = c->inset_t;
	*w = MAX(1, aw - c->inset_l - c->inset_r);
	*h = MAX(1, ah - c->inset_t - c->inset_b);
}

double iv_canvas_fit_zoom(IvCanvas *c)
{
	double vx, vy, vw, vh;

	if (c->lw <= 0 || c->lh <= 0)
		return 1.0;
	viewport(c, &vx, &vy, &vw, &vh);
	return MIN(1.0, MIN(vw / c->lw, vh / c->lh));
}

static void view_middle(IvCanvas *c, double *mx, double *my)
{
	double vx, vy, vw, vh;

	viewport(c, &vx, &vy, &vw, &vh);
	*mx = vx + vw / 2;
	*my = vy + vh / 2;
}

void iv_canvas_widget_to_image(IvCanvas *c, double wx, double wy, double *ix,
			       double *iy)
{
	double mx, my;

	view_middle(c, &mx, &my);
	*ix = c->cx + (wx - mx) / c->zoom;
	*iy = c->cy + (wy - my) / c->zoom;
}

void iv_canvas_image_to_widget(IvCanvas *c, double ix, double iy, double *wx,
			       double *wy)
{
	double mx, my;

	view_middle(c, &mx, &my);
	*wx = mx + (ix - c->cx) * c->zoom;
	*wy = my + (iy - c->cy) * c->zoom;
}

void iv_canvas_image_rect_d(IvCanvas *c, double *x, double *y, double *w,
			    double *h)
{
	double x0, y0;

	iv_canvas_image_to_widget(c, 0, 0, &x0, &y0);
	/* On whole pixels when the picture is smaller than the view, so a
	 * fitted picture has crisp edges. */
	if (c->zoom <= 1.0) {
		x0 = round(x0);
		y0 = round(y0);
	}
	*x = x0;
	*y = y0;
	*w = c->lw * c->zoom;
	*h = c->lh * c->zoom;
}

void iv_canvas_image_rect(IvCanvas *c, GdkRectangle *r)
{
	double x, y, w, h;

	iv_canvas_image_rect_d(c, &x, &y, &w, &h);
	r->x = (int)floor(x);
	r->y = (int)floor(y);
	r->width = (int)ceil(w);
	r->height = (int)ceil(h);
}

/* The picture may not be dragged out of the view: an axis on which it is
 * smaller than the view stays centred, on the other its edges stay at or
 * beyond the view's. */
static void clamp_view(IvCanvas *c)
{
	double vx, vy, vw, vh;

	if (c->lw <= 0)
		return;
	viewport(c, &vx, &vy, &vw, &vh);
	double hw = vw / (2 * c->zoom), hh = vh / (2 * c->zoom);

	if (c->lw <= 2 * hw)
		c->cx = c->lw / 2.0;
	else
		c->cx = CLAMP(c->cx, hw, c->lw - hw);
	if (c->lh <= 2 * hh)
		c->cy = c->lh / 2.0;
	else
		c->cy = CLAMP(c->cy, hh, c->lh - hh);
}

static void view_changed(IvCanvas *c)
{
	clamp_view(c);
	update_cursor(c);
	gtk_widget_queue_draw(GTK_WIDGET(c));
	g_signal_emit(c, signals[SIG_VIEW_CHANGED], 0);
}

static void apply_fit(IvCanvas *c)
{
	c->zoom = iv_canvas_fit_zoom(c);
	c->cx = c->lw / 2.0;
	c->cy = c->lh / 2.0;
}

/* The view moves: the fast filter until it has been still a moment. */
static gboolean settled(gpointer data)
{
	IvCanvas *c = data;

	c->settle_timer = 0;
	c->moving = FALSE;
	gtk_widget_queue_draw(GTK_WIDGET(c));
	return G_SOURCE_REMOVE;
}

static void moving(IvCanvas *c)
{
	c->moving = TRUE;
	if (c->settle_timer)
		g_source_remove(c->settle_timer);
	c->settle_timer = g_timeout_add(140, settled, c);
}

/* ---- zoom --------------------------------------------------------------------- */

double iv_canvas_get_zoom(IvCanvas *c)
{
	return c->zoom;
}

gboolean iv_canvas_is_fit(IvCanvas *c)
{
	return c->fit;
}

static void set_zoom_anchored(IvCanvas *c, double z, double ax, double ay,
			      double aix, double aiy)
{
	double mx, my;

	view_middle(c, &mx, &my);
	c->zoom = CLAMP(z, IV_ZOOM_MIN, IV_ZOOM_MAX);
	c->cx = aix - (ax - mx) / c->zoom;
	c->cy = aiy - (ay - my) / c->zoom;
}

static gboolean zoom_frame(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	IvCanvas *c = IV_CANVAS(w);
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	double t = (now - c->zoom_t0) / (IV_ZOOM_ANIM_MS * 1000.0);
	double e, z;

	if (t >= 1.0) {
		t = 1.0;
	}
	/* Ease out, on the logarithm of the zoom. */
	e = 1 - (1 - t) * (1 - t) * (1 - t);
	z = exp(log(c->z_from) + (log(c->z_to) - log(c->z_from)) * e);
	if (c->zoom_ends_fit) {
		/* Towards fit the middle of the picture glides to the middle
		 * of the view while the zoom changes. */
		double mx, my;

		view_middle(c, &mx, &my);
		set_zoom_anchored(c, z, c->anchor_wx + (mx - c->anchor_wx) * e,
				  c->anchor_wy + (my - c->anchor_wy) * e,
				  c->anchor_ix, c->anchor_iy);
	} else {
		set_zoom_anchored(c, z, c->anchor_wx, c->anchor_wy, c->anchor_ix,
				  c->anchor_iy);
	}
	moving(c);
	if (t >= 1.0) {
		c->zoom_tick = 0;
		if (c->zoom_ends_fit) {
			c->fit = TRUE;
			apply_fit(c);
		}
		view_changed(c);
		return G_SOURCE_REMOVE;
	}
	view_changed(c);
	return G_SOURCE_CONTINUE;
}

static void stop_zoom_animation(IvCanvas *c)
{
	if (c->zoom_tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(c), c->zoom_tick);
		c->zoom_tick = 0;
	}
}

static void zoom_common(IvCanvas *c, double z, double ax, double ay,
			gboolean animate, gboolean ends_fit)
{
	double mx, my, aix, aiy;

	if (c->lw <= 0)
		return;
	z = CLAMP(z, IV_ZOOM_MIN, IV_ZOOM_MAX);
	view_middle(c, &mx, &my);
	if (ax < 0) {
		ax = mx;
		ay = my;
	}
	/* Pointing beside the picture zooms about the nearest point of it. */
	iv_canvas_widget_to_image(c, ax, ay, &aix, &aiy);
	aix = CLAMP(aix, 0, c->lw);
	aiy = CLAMP(aiy, 0, c->lh);
	if (ends_fit) {
		/* Towards fit, the picture glides to the middle as well. */
		aix = c->lw / 2.0;
		aiy = c->lh / 2.0;
		iv_canvas_image_to_widget(c, aix, aiy, &ax, &ay);
	}
	stop_zoom_animation(c);
	c->fit = FALSE;
	if (!animate || !iv_settings_get()->smooth_zoom ||
	    !gtk_widget_get_mapped(GTK_WIDGET(c)) || fabs(z - c->zoom) < 1e-6) {
		set_zoom_anchored(c, z, ax, ay, aix, aiy);
		if (ends_fit) {
			c->fit = TRUE;
			apply_fit(c);
		}
		moving(c);
		view_changed(c);
		return;
	}
	c->z_from = c->zoom;
	c->z_to = z;
	c->anchor_wx = ax;
	c->anchor_wy = ay;
	c->anchor_ix = aix;
	c->anchor_iy = aiy;
	c->zoom_ends_fit = ends_fit;
	c->zoom_t0 = gdk_frame_clock_get_frame_time(
		gtk_widget_get_frame_clock(GTK_WIDGET(c)));
	c->zoom_tick = gtk_widget_add_tick_callback(GTK_WIDGET(c), zoom_frame,
						    NULL, NULL);
}

void iv_canvas_zoom_to(IvCanvas *c, double zoom, double ax, double ay,
		       gboolean animate)
{
	zoom_common(c, zoom, ax, ay, animate, FALSE);
}

static const double zoom_steps[] = {
	0.02, 0.03, 0.05, 0.08, 0.10, 0.125, 0.167, 0.25, 0.333, 0.5,
	0.667, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0,
	24.0, 32.0,
};

void iv_canvas_zoom_step(IvCanvas *c, int direction, double ax, double ay)
{
	double z = c->zoom_tick ? c->z_to : c->zoom;
	double target = z;

	if (direction > 0) {
		for (gsize i = 0; i < G_N_ELEMENTS(zoom_steps); i++)
			if (zoom_steps[i] > z * 1.01) {
				target = zoom_steps[i];
				break;
			}
	} else {
		for (gsize i = G_N_ELEMENTS(zoom_steps); i > 0; i--)
			if (zoom_steps[i - 1] < z / 1.01) {
				target = zoom_steps[i - 1];
				break;
			}
	}
	/* Out past fit lands on fit, so the picture never gets lost in a
	 * small view by accident. */
	double fz = iv_canvas_fit_zoom(c);

	if (direction < 0 && target < fz && z > fz * 1.01)
		target = fz;
	iv_canvas_zoom_to(c, target, ax, ay, TRUE);
}

void iv_canvas_zoom_fit(IvCanvas *c, gboolean animate)
{
	if (c->lw <= 0) {
		c->fit = TRUE;
		return;
	}
	if (c->fit && !c->zoom_tick)
		return;
	zoom_common(c, iv_canvas_fit_zoom(c), -1, -1, animate, TRUE);
}

void iv_canvas_zoom_actual(IvCanvas *c, double ax, double ay)
{
	iv_canvas_zoom_to(c, 1.0, ax, ay, TRUE);
}

void iv_canvas_toggle_fit(IvCanvas *c, double ax, double ay)
{
	double fz = iv_canvas_fit_zoom(c);

	if (c->fit && fabs(fz - 1.0) > 1e-3)
		iv_canvas_zoom_actual(c, ax, ay);
	else if (!c->fit)
		iv_canvas_zoom_fit(c, TRUE);
	else
		/* Fit already is 100 %: double the size instead. */
		iv_canvas_zoom_to(c, 2.0, ax, ay, TRUE);
}

void iv_canvas_pan(IvCanvas *c, double dx, double dy)
{
	if (c->lw <= 0)
		return;
	stop_zoom_animation(c);
	c->fit = FALSE;
	c->cx -= dx / c->zoom;
	c->cy -= dy / c->zoom;
	moving(c);
	view_changed(c);
}

void iv_canvas_set_drag_pan(IvCanvas *c, gboolean on)
{
	c->drag_pan = on;
	update_cursor(c);
}

void iv_canvas_set_insets(IvCanvas *c, int top, int right, int bottom,
			  int left)
{
	if (c->inset_t == top && c->inset_r == right && c->inset_b == bottom &&
	    c->inset_l == left)
		return;
	c->inset_t = top;
	c->inset_r = right;
	c->inset_b = bottom;
	c->inset_l = left;
	if (c->fit)
		apply_fit(c);
	view_changed(c);
}

/* ---- the spinner and the cross-fade ---------------------------------------- */

static gboolean spin_frame(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	IvCanvas *c = IV_CANVAS(w);

	c->spin_phase = gdk_frame_clock_get_frame_time(clock) / 1e6 * 1.4;
	gtk_widget_queue_draw(w);
	return G_SOURCE_CONTINUE;
}

static gboolean show_spinner(gpointer data)
{
	IvCanvas *c = data;

	c->loading_delay = 0;
	c->spinner_on = TRUE;
	if (!c->spin_tick)
		c->spin_tick = gtk_widget_add_tick_callback(GTK_WIDGET(c),
							    spin_frame, NULL, NULL);
	gtk_widget_queue_draw(GTK_WIDGET(c));
	return G_SOURCE_REMOVE;
}

void iv_canvas_set_loading(IvCanvas *c, gboolean loading)
{
	if (c->loading == loading)
		return;
	c->loading = loading;
	if (loading) {
		/* A spinner that flashes up for a fast load is noise. */
		c->loading_delay = g_timeout_add(IV_SPINNER_DELAY_MS, show_spinner,
						 c);
		return;
	}
	if (c->loading_delay) {
		g_source_remove(c->loading_delay);
		c->loading_delay = 0;
	}
	if (c->spin_tick) {
		gtk_widget_remove_tick_callback(GTK_WIDGET(c), c->spin_tick);
		c->spin_tick = 0;
	}
	c->spinner_on = FALSE;
	gtk_widget_queue_draw(GTK_WIDGET(c));
}

static gboolean fade_frame(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
	IvCanvas *c = IV_CANVAS(w);
	double t = (gdk_frame_clock_get_frame_time(clock) - c->fade_t0) /
		   (IV_FADE_MS * 1000.0);

	c->fade = CLAMP(t, 0, 1);
	gtk_widget_queue_draw(w);
	if (c->fade >= 1) {
		g_clear_pointer(&c->fade_from, cairo_surface_destroy);
		c->fade_tick = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void draw_content(IvCanvas *c, cairo_t *cr, gboolean final);

/* What is on the canvas now, as a picture to fade out from. */
static void begin_fade(IvCanvas *c)
{
	GtkWidget *w = GTK_WIDGET(c);
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	cairo_t *cr;

	if (!gtk_widget_get_mapped(w) || aw < 2 || ah < 2 || c->lw <= 0)
		return;
	g_clear_pointer(&c->fade_from, cairo_surface_destroy);
	c->fade_from = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, aw, ah);
	cr = cairo_create(c->fade_from);
	draw_content(c, cr, FALSE);
	cairo_destroy(cr);
	c->fade = 0;
	c->fade_t0 = gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(w));
	if (!c->fade_tick)
		c->fade_tick = gtk_widget_add_tick_callback(w, fade_frame, NULL,
							    NULL);
}

/* ---- animation --------------------------------------------------------------- */

static void stop_animation(IvCanvas *c)
{
	if (c->frame_timer) {
		g_source_remove(c->frame_timer);
		c->frame_timer = 0;
	}
	g_clear_object(&c->iter);
	g_clear_pointer(&c->frame, cairo_surface_destroy);
}

static gboolean next_frame(gpointer data);

static void schedule_frame(IvCanvas *c)
{
	int delay;

	if (c->frame_timer) {
		g_source_remove(c->frame_timer);
		c->frame_timer = 0;
	}
	if (!c->iter || !c->playing)
		return;
	IV_BEGIN_ANIMATION_API
	delay = gdk_pixbuf_animation_iter_get_delay_time(c->iter);
	IV_END_ANIMATION_API
	if (delay < 0)
		return; /* the last frame stays */
	/* Very short delays are what old encoders wrote for "as fast as
	 * possible"; browsers draw them at 10 frames a second. */
	if (delay < 20)
		delay = 100;
	c->frame_timer = g_timeout_add((guint)delay, next_frame, c);
}

static void take_frame(IvCanvas *c)
{
	GdkPixbuf *pb;

	IV_BEGIN_ANIMATION_API
	pb = gdk_pixbuf_animation_iter_get_pixbuf(c->iter);
	IV_END_ANIMATION_API

	g_clear_pointer(&c->frame, cairo_surface_destroy);
	if (pb)
		c->frame = iv_surface_from_pixbuf(pb);
}

static gboolean next_frame(gpointer data)
{
	IvCanvas *c = data;
	gboolean advanced = FALSE;

	c->frame_timer = 0;
	IV_BEGIN_ANIMATION_API
	if (c->iter)
		advanced = gdk_pixbuf_animation_iter_advance(c->iter, NULL);
	IV_END_ANIMATION_API
	if (advanced) {
		take_frame(c);
		gtk_widget_queue_draw(GTK_WIDGET(c));
	}
	schedule_frame(c);
	return G_SOURCE_REMOVE;
}

void iv_canvas_set_playing(IvCanvas *c, gboolean playing)
{
	c->playing = playing;
	schedule_frame(c);
}

/* ---- what is shown ------------------------------------------------------------- */

static void clear_message(IvCanvas *c)
{
	g_clear_pointer(&c->msg_title, g_free);
	g_clear_pointer(&c->msg_detail, g_free);
	c->msg_glyph = IV_GLYPH_NONE;
}

void iv_canvas_set_picture(IvCanvas *c, IvPicture *p, gboolean crossfade,
			   gboolean keep_view)
{
	if (crossfade && p != c->pic)
		begin_fade(c);
	stop_animation(c);
	clear_message(c);
	g_clear_pointer(&c->surface, cairo_surface_destroy);
	if (p != c->pic) {
		if (c->pic)
			iv_picture_unref(c->pic);
		c->pic = p ? iv_picture_ref(p) : NULL;
	}
	if (p && !p->error) {
		gboolean same = c->lw == p->width && c->lh == p->height;

		c->lw = p->width;
		c->lh = p->height;
		c->alpha = p->has_alpha;
		if (!keep_view || !same) {
			stop_zoom_animation(c);
			c->fit = TRUE;
			apply_fit(c);
		}
		if (p->anim) {
			IV_BEGIN_ANIMATION_API
			c->iter = gdk_pixbuf_animation_get_iter(p->anim, NULL);
			IV_END_ANIMATION_API
			take_frame(c);
			c->playing = TRUE;
			schedule_frame(c);
		}
	} else {
		c->lw = c->lh = 0;
		if (p && p->error) {
			c->msg_glyph = IV_GLYPH_WARNING;
			c->msg_title = g_strdup_printf("“%s” cannot be shown",
						       p->name ? p->name : "");
			c->msg_detail = g_strdup(p->error);
		}
	}
	view_changed(c);
}

IvPicture *iv_canvas_get_picture(IvCanvas *c)
{
	return c->pic;
}

void iv_canvas_set_surface(IvCanvas *c, cairo_surface_t *s, int logical_w,
			   int logical_h, gboolean keep_view)
{
	gboolean same = c->lw == logical_w && c->lh == logical_h;

	stop_animation(c);
	clear_message(c);
	if (c->pic) {
		iv_picture_unref(c->pic);
		c->pic = NULL;
	}
	if (s != c->surface) {
		g_clear_pointer(&c->surface, cairo_surface_destroy);
		c->surface = s ? cairo_surface_reference(s) : NULL;
	}
	c->lw = s ? logical_w : 0;
	c->lh = s ? logical_h : 0;
	c->alpha = s && cairo_surface_get_content(s) != CAIRO_CONTENT_COLOR;
	if (!same && !keep_view) {
		stop_zoom_animation(c);
		c->fit = TRUE;
		apply_fit(c);
	} else if (!same && c->fit) {
		/* A new size kept in view: fit follows it; a fixed zoom
		 * stays and is only clamped. */
		apply_fit(c);
	}
	view_changed(c);
}

void iv_canvas_set_message(IvCanvas *c, IvGlyph g, const char *title,
			   const char *detail)
{
	stop_animation(c);
	if (c->pic) {
		iv_picture_unref(c->pic);
		c->pic = NULL;
	}
	g_clear_pointer(&c->surface, cairo_surface_destroy);
	clear_message(c);
	c->lw = c->lh = 0;
	c->msg_glyph = g;
	c->msg_title = g_strdup(title);
	c->msg_detail = g_strdup(detail);
	view_changed(c);
}

void iv_canvas_get_image_size(IvCanvas *c, int *w, int *h)
{
	if (w)
		*w = c->lw;
	if (h)
		*h = c->lh;
}

gboolean iv_canvas_has_image(IvCanvas *c)
{
	return c->lw > 0;
}

void iv_canvas_set_backdrop(IvCanvas *c, IvBackdrop b)
{
	GtkWidget *w = GTK_WIDGET(c);

	c->backdrop = b;
	iv_set_class(w, "darker", b == IV_BACKDROP_DARKER);
	iv_set_class(w, "checker", b == IV_BACKDROP_CHECKER);
	gtk_widget_queue_draw(w);
}

void iv_canvas_set_hooks(IvCanvas *c, IvCanvasOverlay overlay,
			 IvCanvasEvent event, gpointer data)
{
	c->overlay_hook = overlay;
	c->event_hook = event;
	c->hook_data = data;
	gtk_widget_queue_draw(GTK_WIDGET(c));
}

void iv_canvas_set_cursor_name(IvCanvas *c, const char *name)
{
	if (g_strcmp0(name, c->cursor_name) == 0)
		return;
	/* cursor_shown may point at the old name, which is about to go:
	 * make it a value no cursor has, so the next update sets one. */
	if (c->cursor_shown && c->cursor_shown == c->cursor_name)
		c->cursor_shown = "";
	g_free(c->cursor_name);
	c->cursor_name = g_strdup(name);
	update_cursor(c);
}

/* ---- drawing ------------------------------------------------------------------- */

static void style_color(IvCanvas *c, const char *cls, GdkRGBA *out)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(c));

	gtk_style_context_save(sc);
	if (cls)
		gtk_style_context_add_class(sc, cls);
	gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), out);
	gtk_style_context_restore(sc);
}

static void draw_backdrop(IvCanvas *c, cairo_t *cr, int aw, int ah)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(c));

	gtk_render_background(sc, cr, 0, 0, aw, ah);
	if (c->backdrop == IV_BACKDROP_CHECKER) {
		cairo_save(cr);
		cairo_set_source(cr, c->checker);
		cairo_paint(cr);
		cairo_restore(cr);
	}
}

/* The surface to draw from and how many of its pixels make one logical
 * image pixel. */
static cairo_surface_t *source_for(IvCanvas *c, double *k)
{
	cairo_surface_t *s = NULL;

	if (c->frame) {
		s = c->frame;
	} else if (c->surface) {
		s = c->surface;
	} else if (c->pic) {
		int level = iv_picture_level_for(c->pic, c->zoom);

		s = iv_picture_surface(c->pic, level);
		/* A level not made yet (the full size of a large picture
		 * before anyone zoomed in): the next one down meanwhile. */
		while (!s && ++level < c->pic->n_levels)
			s = iv_picture_surface(c->pic, level);
	}
	if (!s || c->lw <= 0) {
		*k = 1;
		return NULL;
	}
	*k = cairo_image_surface_get_width(s) / (double)c->lw;
	return s;
}

static void draw_image(IvCanvas *c, cairo_t *cr, gboolean final)
{
	double x, y, w, h, k;
	cairo_surface_t *s = source_for(c, &k);
	cairo_filter_t filter;

	if (!s)
		return;
	iv_canvas_image_rect_d(c, &x, &y, &w, &h);
	if (c->alpha && c->backdrop != IV_BACKDROP_CHECKER) {
		cairo_save(cr);
		cairo_rectangle(cr, x, y, w, h);
		cairo_clip(cr);
		cairo_translate(cr, x, y);
		cairo_set_source(cr, c->checker);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	if (c->zoom >= IV_PIXEL_ZOOM)
		filter = CAIRO_FILTER_NEAREST;
	else if (c->moving || !final)
		filter = CAIRO_FILTER_BILINEAR;
	else
		filter = CAIRO_FILTER_GOOD;
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, c->zoom / k, c->zoom / k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), filter);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void draw_message(IvCanvas *c, cairo_t *cr, int aw, int ah)
{
	GdkRGBA fg, dim;
	PangoLayout *l;
	int tw, th, y;
	double mx, my;

	if (!c->msg_title && c->msg_glyph == IV_GLYPH_NONE)
		return;
	style_color(c, NULL, &fg);
	style_color(c, "iv-canvas-secondary", &dim);
	view_middle(c, &mx, &my);
	y = (int)my - 60;
	if (c->msg_glyph != IV_GLYPH_NONE) {
		gdk_cairo_set_source_rgba(cr, &dim);
		iv_glyph_paint(cr, c->msg_glyph, mx, y, 48);
		y += 44;
	}
	l = gtk_widget_create_pango_layout(GTK_WIDGET(c), NULL);
	if (c->msg_title) {
		char *m = g_markup_printf_escaped("<span size='large' weight='600'>%s</span>",
						  c->msg_title);

		pango_layout_set_markup(l, m, -1);
		g_free(m);
		pango_layout_set_width(l, PANGO_SCALE * MIN(560, aw - 48));
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_size(l, &tw, &th);
		gdk_cairo_set_source_rgba(cr, &fg);
		cairo_move_to(cr, mx - MIN(560, aw - 48) / 2.0, y);
		pango_cairo_show_layout(cr, l);
		y += th + 6;
	}
	if (c->msg_detail) {
		pango_layout_set_attributes(l, NULL);
		pango_layout_set_text(l, c->msg_detail, -1);
		pango_layout_set_width(l, PANGO_SCALE * MIN(560, aw - 48));
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
		gdk_cairo_set_source_rgba(cr, &dim);
		cairo_move_to(cr, mx - MIN(560, aw - 48) / 2.0, y);
		pango_cairo_show_layout(cr, l);
	}
	g_object_unref(l);
}

static void draw_spinner(IvCanvas *c, cairo_t *cr)
{
	GdkRGBA accent, track;
	double mx, my;
	const double r = 18;

	view_middle(c, &mx, &my);
	style_color(c, "iv-canvas-accent", &accent);
	style_color(c, "iv-canvas-secondary", &track);
	track.alpha *= 0.35;
	/* A dark disc behind, for pictures as light as the ring. */
	cairo_set_source_rgba(cr, 0.086, 0.11, 0.165, 0.72);
	cairo_arc(cr, mx, my, r + 12, 0, 2 * G_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, 3);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	gdk_cairo_set_source_rgba(cr, &track);
	cairo_arc(cr, mx, my, r, 0, 2 * G_PI);
	cairo_stroke(cr);
	gdk_cairo_set_source_rgba(cr, &accent);
	double a0 = c->spin_phase * 2 * G_PI;
	double len = G_PI * (0.6 + 0.4 * sin(c->spin_phase * 2.1));

	cairo_arc(cr, mx, my, r, a0, a0 + len);
	cairo_stroke(cr);
}

static void draw_content(IvCanvas *c, cairo_t *cr, gboolean final)
{
	GtkWidget *w = GTK_WIDGET(c);
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);

	draw_backdrop(c, cr, aw, ah);
	if (c->lw > 0)
		draw_image(c, cr, final);
	else
		draw_message(c, cr, aw, ah);
}

static gboolean iv_canvas_draw(GtkWidget *w, cairo_t *cr)
{
	IvCanvas *c = IV_CANVAS(w);

	draw_content(c, cr, TRUE);
	if (c->fade_from && c->fade < 1) {
		cairo_set_source_surface(cr, c->fade_from, 0, 0);
		cairo_paint_with_alpha(cr, 1 - c->fade);
	}
	if (c->overlay_hook && c->lw > 0) {
		cairo_save(cr);
		c->overlay_hook(c, cr, c->hook_data);
		cairo_restore(cr);
	}
	if (c->spinner_on)
		draw_spinner(c, cr);
	return TRUE;
}

/* ---- input ----------------------------------------------------------------------- */

static gboolean pannable(IvCanvas *c)
{
	double vx, vy, vw, vh;

	if (c->lw <= 0)
		return FALSE;
	viewport(c, &vx, &vy, &vw, &vh);
	return c->lw * c->zoom > vw + 0.5 || c->lh * c->zoom > vh + 0.5;
}

static void update_cursor(IvCanvas *c)
{
	GtkWidget *w = GTK_WIDGET(c);
	GdkWindow *win = gtk_widget_get_window(w);
	const char *want = NULL;

	if (!win)
		return;
	if (c->cursor_name)
		want = c->cursor_name;
	else if (c->dragging)
		want = "grabbing";
	else if (c->drag_pan && pannable(c))
		want = "grab";
	if (g_strcmp0(want, c->cursor_shown) == 0)
		return;
	c->cursor_shown = want;
	if (want) {
		GdkCursor *cur = gdk_cursor_new_from_name(gtk_widget_get_display(w),
							  want);

		if (!cur && !strcmp(want, "grabbing"))
			cur = gdk_cursor_new_from_name(gtk_widget_get_display(w),
						       "move");
		gdk_window_set_cursor(win, cur);
		g_clear_object(&cur);
	} else {
		gdk_window_set_cursor(win, NULL);
	}
}

static gboolean hook(IvCanvas *c, GdkEvent *ev, double wx, double wy)
{
	double ix, iy;

	if (!c->event_hook || c->lw <= 0)
		return FALSE;
	iv_canvas_widget_to_image(c, wx, wy, &ix, &iy);
	return c->event_hook(c, ev, ix, iy, c->hook_data);
}

static gboolean iv_canvas_button_press(GtkWidget *w, GdkEventButton *ev)
{
	IvCanvas *c = IV_CANVAS(w);

	if (!gtk_widget_has_focus(w) && gtk_widget_get_can_focus(w))
		gtk_widget_grab_focus(w);
	if (hook(c, (GdkEvent *)ev, ev->x, ev->y))
		return TRUE;
	if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) {
		g_signal_emit(c, signals[SIG_CONTEXT_MENU], 0, ev);
		return TRUE;
	}
	if (ev->button != 1)
		return FALSE;
	if (ev->type == GDK_2BUTTON_PRESS) {
		c->pressed = FALSE;
		if (c->lw > 0)
			iv_canvas_toggle_fit(c, ev->x, ev->y);
		return TRUE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;
	c->pressed = TRUE;
	c->dragging = FALSE;
	c->press_x = c->last_x = ev->x;
	c->press_y = c->last_y = ev->y;
	return TRUE;
}

static gboolean iv_canvas_motion(GtkWidget *w, GdkEventMotion *ev)
{
	IvCanvas *c = IV_CANVAS(w);

	if (hook(c, (GdkEvent *)ev, ev->x, ev->y))
		return TRUE;
	if (!c->pressed || !(ev->state & GDK_BUTTON1_MASK))
		return FALSE;
	if (!c->dragging) {
		if (!gtk_drag_check_threshold(w, (int)c->press_x, (int)c->press_y,
					      (int)ev->x, (int)ev->y))
			return TRUE;
		c->dragging = c->drag_pan;
		update_cursor(c);
		if (!c->dragging)
			return TRUE;
	}
	if (pannable(c))
		iv_canvas_pan(c, ev->x - c->last_x, ev->y - c->last_y);
	c->last_x = ev->x;
	c->last_y = ev->y;
	return TRUE;
}

static gboolean iv_canvas_button_release(GtkWidget *w, GdkEventButton *ev)
{
	IvCanvas *c = IV_CANVAS(w);
	gboolean was_drag = c->dragging;
	gboolean moved;

	if (hook(c, (GdkEvent *)ev, ev->x, ev->y)) {
		c->pressed = c->dragging = FALSE;
		return TRUE;
	}
	if (ev->button != 1 || !c->pressed)
		return FALSE;
	moved = gtk_drag_check_threshold(w, (int)c->press_x, (int)c->press_y,
					 (int)ev->x, (int)ev->y);
	c->pressed = FALSE;
	c->dragging = FALSE;
	update_cursor(c);
	if (!was_drag && !moved)
		g_signal_emit(c, signals[SIG_IMAGE_ACTIVATED], 0);
	return TRUE;
}

static gboolean iv_canvas_scroll(GtkWidget *w, GdkEventScroll *ev)
{
	IvCanvas *c = IV_CANVAS(w);
	IvSettings *s = iv_settings_get();
	double dx = 0, dy = 0;
	gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
	gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;

	if (hook(c, (GdkEvent *)ev, ev->x, ev->y))
		return TRUE;
	switch (ev->direction) {
	case GDK_SCROLL_UP:
		dy = -1;
		break;
	case GDK_SCROLL_DOWN:
		dy = 1;
		break;
	case GDK_SCROLL_LEFT:
		dx = -1;
		break;
	case GDK_SCROLL_RIGHT:
		dx = 1;
		break;
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &dy);
		break;
	}
	if (c->lw <= 0) {
		if (!s->wheel_zooms && dy != 0 && ev->direction != GDK_SCROLL_SMOOTH)
			g_signal_emit(c, signals[SIG_NAVIGATE], 0, dy > 0 ? 1 : -1);
		return TRUE;
	}
	/* Shift: across; the horizontal wheel always pans. */
	if (shift && dy != 0 && dx == 0) {
		dx = dy;
		dy = 0;
	}
	if (dx != 0 && dy == 0) {
		iv_canvas_pan(c, -dx * 60, 0);
		return TRUE;
	}
	if (ctrl || s->wheel_zooms) {
		double z = c->zoom_tick ? c->z_to : c->zoom;

		if (ev->direction == GDK_SCROLL_SMOOTH) {
			/* Touchpads deliver fractions of a notch. */
			iv_canvas_zoom_to(c, z * pow(IV_WHEEL_FACTOR, -dy), ev->x,
					  ev->y, FALSE);
		} else {
			double target = z * (dy < 0 ? IV_WHEEL_FACTOR :
						     1 / IV_WHEEL_FACTOR);
			double fz = iv_canvas_fit_zoom(c);

			/* Zooming out stops at fit on the way, once. */
			if (dy > 0 && z > fz * 1.01 && target < fz)
				iv_canvas_zoom_fit(c, TRUE);
			else
				iv_canvas_zoom_to(c, target, ev->x, ev->y, TRUE);
		}
		return TRUE;
	}
	/* The wheel browses; zoomed in, it scrolls the picture first. */
	if (pannable(c)) {
		iv_canvas_pan(c, 0, -dy * 80);
		return TRUE;
	}
	c->wheel_acc += dy;
	if (fabs(c->wheel_acc) >= 1) {
		g_signal_emit(c, signals[SIG_NAVIGATE], 0, c->wheel_acc > 0 ? 1 : -1);
		c->wheel_acc = 0;
	}
	return TRUE;
}

static gboolean iv_canvas_leave(GtkWidget *w, GdkEventCrossing *ev)
{
	hook(IV_CANVAS(w), (GdkEvent *)ev, ev->x, ev->y);
	return FALSE;
}

/* Whoever listens may change widgets (the zoom label): not in the middle
 * of the window's allocation, but just after it. */
static gboolean view_changed_later(gpointer data)
{
	IvCanvas *c = data;

	c->view_idle = 0;
	g_signal_emit(c, signals[SIG_VIEW_CHANGED], 0);
	return G_SOURCE_REMOVE;
}

static void iv_canvas_size_allocate(GtkWidget *w, GtkAllocation *a)
{
	IvCanvas *c = IV_CANVAS(w);

	GTK_WIDGET_CLASS(iv_canvas_parent_class)->size_allocate(w, a);
	if (c->fit && !c->zoom_tick)
		apply_fit(c);
	clamp_view(c);
	update_cursor(c);
	if (!c->view_idle)
		c->view_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 15,
					       view_changed_later, c, NULL);
}

static void iv_canvas_realize(GtkWidget *w)
{
	GTK_WIDGET_CLASS(iv_canvas_parent_class)->realize(w);
	IV_CANVAS(w)->cursor_shown = NULL;
	update_cursor(IV_CANVAS(w));
}

static void iv_canvas_finalize(GObject *o)
{
	IvCanvas *c = IV_CANVAS(o);

	stop_animation(c);
	if (c->view_idle)
		g_source_remove(c->view_idle);
	if (c->settle_timer)
		g_source_remove(c->settle_timer);
	if (c->loading_delay)
		g_source_remove(c->loading_delay);
	if (c->pic)
		iv_picture_unref(c->pic);
	g_clear_pointer(&c->surface, cairo_surface_destroy);
	g_clear_pointer(&c->fade_from, cairo_surface_destroy);
	cairo_pattern_destroy(c->checker);
	clear_message(c);
	g_free(c->cursor_name);
	G_OBJECT_CLASS(iv_canvas_parent_class)->finalize(o);
}

static void iv_canvas_class_init(IvCanvasClass *k)
{
	GtkWidgetClass *wk = GTK_WIDGET_CLASS(k);

	G_OBJECT_CLASS(k)->finalize = iv_canvas_finalize;
	wk->draw = iv_canvas_draw;
	wk->button_press_event = iv_canvas_button_press;
	wk->button_release_event = iv_canvas_button_release;
	wk->motion_notify_event = iv_canvas_motion;
	wk->scroll_event = iv_canvas_scroll;
	wk->leave_notify_event = iv_canvas_leave;
	wk->size_allocate = iv_canvas_size_allocate;
	wk->realize = iv_canvas_realize;

	signals[SIG_VIEW_CHANGED] = g_signal_new("view-changed",
						 G_TYPE_FROM_CLASS(k),
						 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
						 NULL, G_TYPE_NONE, 0);
	signals[SIG_IMAGE_ACTIVATED] = g_signal_new("image-activated",
						    G_TYPE_FROM_CLASS(k),
						    G_SIGNAL_RUN_LAST, 0, NULL,
						    NULL, NULL, G_TYPE_NONE, 0);
	signals[SIG_NAVIGATE] = g_signal_new("navigate", G_TYPE_FROM_CLASS(k),
					     G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					     NULL, G_TYPE_NONE, 1, G_TYPE_INT);
	signals[SIG_CONTEXT_MENU] = g_signal_new("context-menu",
						 G_TYPE_FROM_CLASS(k),
						 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
						 NULL, G_TYPE_NONE, 1,
						 G_TYPE_POINTER);
}

static void iv_canvas_init(IvCanvas *c)
{
	GtkWidget *w = GTK_WIDGET(c);

	c->zoom = 1.0;
	c->fit = TRUE;
	c->drag_pan = TRUE;
	c->playing = TRUE;
	c->checker = iv_checker_pattern(8);
	gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
					 GDK_POINTER_MOTION_MASK |
					 GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
					 GDK_LEAVE_NOTIFY_MASK);
	gtk_widget_set_can_focus(w, TRUE);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_widget_set_vexpand(w, TRUE);
	iv_add_class(w, "iv-canvas");
	iv_canvas_set_backdrop(c, iv_settings_get()->backdrop);
}

IvCanvas *iv_canvas_new(void)
{
	return g_object_new(IV_TYPE_CANVAS, NULL);
}
