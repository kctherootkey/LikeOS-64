// LikeOS -- LikeOS Image Viewer: the straighten dial of the Crop tool.
//
// See tool-crop-dial.h for what it does.  The dial is a drawing area that
// paints itself:
//
//        +12.4°                     the angle, accent while not level
//   ...|....|....|....▼....|....|...  the ruler slides, the mark stays
//      -15       0    |    15        numbers every fifteen degrees
//   ===============(=======)          the travel from 0 to the angle
//
// The ruler fades out towards both ends, so it reads as a wheel seen from
// the front rather than as a scale with ends; the real ends (+-45) are
// drawn as taller ticks.  The colours come from the theme: the widget's own
// CSS colour for the ticks and the text, the shell accent for the mark.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>

#include "tool-crop-dial.h"

/* Pixels per degree on the ruler: the panel is about 300 px wide, so a
 * little more than +-20 degrees are in view at once. */
#define PX_PER_DEG 7.0
/* Height of the widget and where its parts sit. */
#define DIAL_H 78
#define VALUE_Y 14.0	/* centre line of the angle text */
#define BASE_Y 50.0	/* the line the ticks stand on */
#define LABEL_Y 63.0	/* centre line of the numbers */
#define TRAVEL_Y 74.0	/* the travel bar */
/* Close to 0 while dragging snaps to 0, within this many degrees. */
#define SNAP_DEG 0.6
/* Keys and the wheel end their gesture after this rest (ms). */
#define REST_MS 550

typedef struct {
	IvDialChanged changed;
	gpointer data;
	double value;

	/* the drag */
	gboolean pressed, moved, fine;
	double press_x, press_value, last_x;
	/* keys and the wheel */
	guint rest;
	double smooth_acc;
	/* the pointer is over the widget */
	gboolean hover;
} Dial;

static Dial *dial_of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "iv-dial");
}

static double quantise(double v)
{
	v = CLAMP(v, -IV_DIAL_LIMIT, IV_DIAL_LIMIT);
	v = round(v * 10.0) / 10.0;
	/* No "-0.0" on the label. */
	return fabs(v) < 0.05 ? 0.0 : v;
}

/* ---- colours ---------------------------------------------------------------- */

static void theme_color(GtkWidget *w, const char *name, double r, double g,
			double b, GdkRGBA *out)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);

	if (!gtk_style_context_lookup_color(sc, name, out)) {
		out->red = r;
		out->green = g;
		out->blue = b;
		out->alpha = 1;
	}
}

static void colors(GtkWidget *w, GdkRGBA *fg, GdkRGBA *dim, GdkRGBA *accent)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);

	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), fg);
	theme_color(w, "likeos_shell_text_secondary", 0.643, 0.678, 0.749, dim);
	theme_color(w, "likeos_shell_accent", 0.357, 0.561, 0.976, accent);
}

/* ---- drawing ----------------------------------------------------------------- */

/* How strongly a tick at `x' shows: full in the middle, fading out to
 * the ends of the widget (a wheel seen from the front). */
static double fade(double x, double cx, double half)
{
	double t = fabs(x - cx) / MAX(1.0, half);

	t = CLAMP(t, 0, 1);
	return 1.0 - t * t * t * 0.92;
}

static void text_at(GtkWidget *w, cairo_t *cr, const char *markup, double cx,
		    double cy)
{
	PangoLayout *l = gtk_widget_create_pango_layout(w, NULL);
	int tw, th;

	pango_layout_set_markup(l, markup, -1);
	pango_layout_get_pixel_size(l, &tw, &th);
	cairo_move_to(cr, round(cx - tw / 2.0), round(cy - th / 2.0));
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

static gboolean dial_draw(GtkWidget *w, cairo_t *cr, gpointer unused)
{
	Dial *d = dial_of(w);
	double W = gtk_widget_get_allocated_width(w);
	double cx = floor(W / 2.0) + 0.5, half = W / 2.0;
	GdkRGBA fg, dim, accent;
	char buf[64];
	int first, last;
	gboolean level = fabs(d->value) < 0.05;
	gboolean sensitive = gtk_widget_is_sensitive(w);

	colors(w, &fg, &dim, &accent);
	if (!sensitive)
		accent = dim;

	/* The angle above the mark. */
	if (level)
		g_snprintf(buf, sizeof buf, "<span weight='600'>0.0°</span>");
	else
		g_snprintf(buf, sizeof buf, "<span weight='600'>%s%.1f°</span>",
			   d->value > 0 ? "+" : "−", fabs(d->value));
	if (level)
		gdk_cairo_set_source_rgba(cr, &fg);
	else
		gdk_cairo_set_source_rgba(cr, &accent);
	text_at(w, cr, buf, cx, VALUE_Y);

	/* The ticks of every whole degree in view.  The ruler's 0 sits at
	 * cx - value * PX_PER_DEG: sliding the ruler to the left brings the
	 * positive degrees under the mark. */
	first = (int)floor(d->value - half / PX_PER_DEG) - 1;
	last = (int)ceil(d->value + half / PX_PER_DEG) + 1;
	first = MAX(first, -(int)IV_DIAL_LIMIT);
	last = MIN(last, (int)IV_DIAL_LIMIT);
	cairo_set_line_width(cr, 1.0);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	for (int deg = first; deg <= last; deg++) {
		double x = floor(cx + (deg - d->value) * PX_PER_DEG) + 0.5;
		double a = fade(x, cx, half);
		gboolean major = deg % 5 == 0;
		gboolean end = abs(deg) == (int)IV_DIAL_LIMIT;
		double h = end ? 18 : major ? 12 : 6;

		if (x < -1 || x > W + 1)
			continue;
		cairo_set_source_rgba(cr, major ? fg.red : dim.red,
				      major ? fg.green : dim.green,
				      major ? fg.blue : dim.blue,
				      a * (major ? (d->hover ? 0.95 : 0.8) :
						   (d->hover ? 0.75 : 0.55)));
		cairo_move_to(cr, x, BASE_Y - h);
		cairo_line_to(cr, x, BASE_Y);
		cairo_stroke(cr);
		if (deg % 15 == 0) {
			char num[16];

			if (deg == 0)
				g_snprintf(num, sizeof num, "0");
			else
				g_snprintf(num, sizeof num, "%s%d",
					   deg > 0 ? "" : "−", abs(deg));
			cairo_set_source_rgba(cr, dim.red, dim.green, dim.blue,
					      a * 0.9);
			g_snprintf(buf, sizeof buf, "<span size='small'>%s</span>",
				   num);
			text_at(w, cr, buf, x, LABEL_Y);
		}
	}

	/* The mark: a line through the ruler with a small wedge on top. */
	gdk_cairo_set_source_rgba(cr, &accent);
	cairo_set_line_width(cr, 2.0);
	cairo_move_to(cr, cx, BASE_Y - 22);
	cairo_line_to(cr, cx, BASE_Y + 3);
	cairo_stroke(cr);
	cairo_move_to(cr, cx - 5, BASE_Y - 27);
	cairo_line_to(cr, cx + 5, BASE_Y - 27);
	cairo_line_to(cr, cx, BASE_Y - 21);
	cairo_close_path(cr);
	cairo_fill(cr);

	/* The travel bar: a hairline track across the whole range, the part
	 * from 0 to the angle in the accent.  It is a fixed scale (the
	 * whole -45..45 fits), so it shows at a glance how far off level
	 * the picture is turned. */
	{
		double x0 = 12, x1 = W - 12, mid = (x0 + x1) / 2;
		double xv = mid + (x1 - x0) / 2 * d->value / IV_DIAL_LIMIT;

		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_width(cr, 3.0);
		cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.16);
		cairo_move_to(cr, x0, TRAVEL_Y);
		cairo_line_to(cr, x1, TRAVEL_Y);
		cairo_stroke(cr);
		cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.4);
		cairo_set_line_width(cr, 1.0);
		cairo_move_to(cr, floor(mid) + 0.5, TRAVEL_Y - 3);
		cairo_line_to(cr, floor(mid) + 0.5, TRAVEL_Y + 3);
		cairo_stroke(cr);
		if (!level) {
			cairo_set_line_width(cr, 3.0);
			gdk_cairo_set_source_rgba(cr, &accent);
			cairo_move_to(cr, mid, TRAVEL_Y);
			cairo_line_to(cr, xv, TRAVEL_Y);
			cairo_stroke(cr);
			cairo_arc(cr, xv, TRAVEL_Y, 3.5, 0, 2 * G_PI);
			cairo_fill(cr);
		}
	}

	/* The keyboard focus. */
	if (gtk_widget_has_visible_focus(w)) {
		double r = 8, x = 1, y = 1, ww = W - 2, hh = DIAL_H - 2;

		cairo_new_sub_path(cr);
		cairo_arc(cr, x + ww - r, y + r, r, -G_PI / 2, 0);
		cairo_arc(cr, x + ww - r, y + hh - r, r, 0, G_PI / 2);
		cairo_arc(cr, x + r, y + hh - r, r, G_PI / 2, G_PI);
		cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
		cairo_close_path(cr);
		cairo_set_line_width(cr, 2.0);
		cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue,
				      0.9);
		cairo_stroke(cr);
	}
	return TRUE;
}

/* ---- changing the value ------------------------------------------------------- */

static void set_and_tell(GtkWidget *w, double v, gboolean final)
{
	Dial *d = dial_of(w);

	v = quantise(v);
	if (v != d->value) {
		d->value = v;
		gtk_widget_queue_draw(w);
		if (d->changed)
			d->changed(w, v, FALSE, d->data);
	}
	if (final && d->changed)
		d->changed(w, d->value, TRUE, d->data);
}

static gboolean rest_over(gpointer data)
{
	GtkWidget *w = data;
	Dial *d = dial_of(w);

	d->rest = 0;
	if (d->changed)
		d->changed(w, d->value, TRUE, d->data);
	return G_SOURCE_REMOVE;
}

/* A key or wheel step: shown at once, final after a rest. */
static void step(GtkWidget *w, double delta)
{
	Dial *d = dial_of(w);
	double before = d->value;

	set_and_tell(w, d->value + delta, FALSE);
	if (d->value == before && !d->rest)
		return;
	if (d->rest)
		g_source_remove(d->rest);
	d->rest = g_timeout_add(REST_MS, rest_over, w);
}

void iv_dial_flush(GtkWidget *w)
{
	Dial *d = dial_of(w);

	if (!d || !d->rest)
		return;
	g_source_remove(d->rest);
	rest_over(w);
}

gboolean iv_dial_busy(GtkWidget *w)
{
	Dial *d = dial_of(w);

	return d && (d->pressed || d->rest);
}

void iv_dial_set_value(GtkWidget *w, double v)
{
	Dial *d = dial_of(w);

	v = quantise(v);
	if (v == d->value)
		return;
	d->value = v;
	gtk_widget_queue_draw(w);
}

double iv_dial_get_value(GtkWidget *w)
{
	return dial_of(w)->value;
}

/* ---- the pointer ---------------------------------------------------------------- */

static gboolean dial_press(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	Dial *d = dial_of(w);

	if (ev->button != 1)
		return FALSE;
	if (!gtk_widget_has_focus(w))
		gtk_widget_grab_focus(w);
	if (ev->type == GDK_2BUTTON_PRESS) {
		d->pressed = FALSE;
		iv_dial_flush(w);
		set_and_tell(w, 0, TRUE);
		return TRUE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;
	iv_dial_flush(w);
	d->pressed = TRUE;
	d->moved = FALSE;
	d->press_x = d->last_x = ev->x;
	d->press_value = d->value;
	d->fine = (ev->state & GDK_SHIFT_MASK) != 0;
	return TRUE;
}

static gboolean dial_motion(GtkWidget *w, GdkEventMotion *ev, gpointer unused)
{
	Dial *d = dial_of(w);
	gboolean fine = (ev->state & GDK_SHIFT_MASK) != 0;
	double v;

	if (!d->pressed)
		return FALSE;
	if (!d->moved && fabs(ev->x - d->press_x) < 3)
		return TRUE;
	d->moved = TRUE;
	/* Shift pressed or let go in the middle of a drag: continue from
	 * where the value is, at the new speed, so nothing jumps. */
	if (fine != d->fine) {
		d->fine = fine;
		d->press_x = d->last_x;
		d->press_value = d->value;
	}
	d->last_x = ev->x;
	/* The ruler follows the pointer: dragging to the left brings the
	 * higher degrees under the mark. */
	v = d->press_value - (ev->x - d->press_x) / PX_PER_DEG * (fine ? 0.1 : 1.0);
	if (!fine && fabs(v) < SNAP_DEG)
		v = 0;
	set_and_tell(w, v, FALSE);
	return TRUE;
}

static gboolean dial_release(GtkWidget *w, GdkEventButton *ev, gpointer unused)
{
	Dial *d = dial_of(w);
	double W = gtk_widget_get_allocated_width(w);

	if (ev->button != 1 || !d->pressed)
		return FALSE;
	d->pressed = FALSE;
	if (!d->moved && ev->y > BASE_Y - 30) {
		/* A click on the ruler: that tick comes to the mark. */
		double v = d->value + (ev->x - W / 2.0) / PX_PER_DEG;

		set_and_tell(w, (ev->state & GDK_SHIFT_MASK) ? v : round(v), TRUE);
		return TRUE;
	}
	set_and_tell(w, d->value, TRUE);
	return TRUE;
}

static gboolean dial_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer unused)
{
	Dial *d = dial_of(w);
	double unit = (ev->state & GDK_SHIFT_MASK) ? 0.1 : 1.0;
	double dx, dy;

	switch (ev->direction) {
	case GDK_SCROLL_UP:
	case GDK_SCROLL_RIGHT:
		step(w, unit);
		return TRUE;
	case GDK_SCROLL_DOWN:
	case GDK_SCROLL_LEFT:
		step(w, -unit);
		return TRUE;
	case GDK_SCROLL_SMOOTH:
		if (!gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &dy))
			return FALSE;
		/* Touchpads: a step for each whole unit of travel. */
		d->smooth_acc += fabs(dx) > fabs(dy) ? dx : -dy;
		while (d->smooth_acc >= 1.0) {
			d->smooth_acc -= 1.0;
			step(w, unit);
		}
		while (d->smooth_acc <= -1.0) {
			d->smooth_acc += 1.0;
			step(w, -unit);
		}
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean dial_crossing(GtkWidget *w, GdkEventCrossing *ev, gpointer unused)
{
	Dial *d = dial_of(w);

	if (ev->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;
	d->hover = ev->type == GDK_ENTER_NOTIFY;
	gtk_widget_queue_draw(w);
	return FALSE;
}

/* ---- keys ---------------------------------------------------------------------- */

gboolean iv_dial_key(GtkWidget *w, GdkEventKey *ev)
{
	Dial *d = dial_of(w);
	double unit = (ev->state & GDK_SHIFT_MASK) ? 0.1 : 1.0;

	if (!d || !gtk_widget_is_sensitive(w))
		return FALSE;
	if (ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))
		return FALSE;
	switch (ev->keyval) {
	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		step(w, -unit);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		step(w, unit);
		return TRUE;
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		step(w, 5);
		return TRUE;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		step(w, -5);
		return TRUE;
	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		step(w, -d->value);
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean dial_key_press(GtkWidget *w, GdkEventKey *ev, gpointer unused)
{
	return iv_dial_key(w, ev);
}

static gboolean dial_focus_change(GtkWidget *w, GdkEventFocus *ev, gpointer unused)
{
	if (!ev->in)
		iv_dial_flush(w);
	gtk_widget_queue_draw(w);
	return FALSE;
}

/* ---- the widget ---------------------------------------------------------------- */

static void dial_free(gpointer p)
{
	Dial *d = p;

	if (d->rest)
		g_source_remove(d->rest);
	g_free(d);
}

static void dial_unrealize(GtkWidget *w, gpointer unused)
{
	Dial *d = dial_of(w);

	/* No timer may outlive the widget's window. */
	if (d && d->rest) {
		g_source_remove(d->rest);
		d->rest = 0;
	}
}

static void dial_get_accessible_name(GtkWidget *w)
{
	AtkObject *a = gtk_widget_get_accessible(w);

	if (a)
		atk_object_set_name(a, "Straighten angle");
}

GtkWidget *iv_dial_new(IvDialChanged changed, gpointer data)
{
	GtkWidget *w = gtk_drawing_area_new();
	Dial *d = g_new0(Dial, 1);

	d->changed = changed;
	d->data = data;
	g_object_set_data_full(G_OBJECT(w), "iv-dial", d, dial_free);
	gtk_widget_set_size_request(w, 220, DIAL_H);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_widget_set_can_focus(w, TRUE);
	gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
				 GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
				 GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK |
				 GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
				 GDK_FOCUS_CHANGE_MASK);
	gtk_style_context_add_class(gtk_widget_get_style_context(w), "iv-dial");
	gtk_widget_set_tooltip_text(w, "Drag, scroll or use the arrow keys to "
				       "straighten; Shift for tenths of a "
				       "degree, double-click for level");
	g_signal_connect(w, "draw", G_CALLBACK(dial_draw), NULL);
	g_signal_connect(w, "button-press-event", G_CALLBACK(dial_press), NULL);
	g_signal_connect(w, "button-release-event", G_CALLBACK(dial_release), NULL);
	g_signal_connect(w, "motion-notify-event", G_CALLBACK(dial_motion), NULL);
	g_signal_connect(w, "scroll-event", G_CALLBACK(dial_scroll), NULL);
	g_signal_connect(w, "key-press-event", G_CALLBACK(dial_key_press), NULL);
	g_signal_connect(w, "enter-notify-event", G_CALLBACK(dial_crossing), NULL);
	g_signal_connect(w, "leave-notify-event", G_CALLBACK(dial_crossing), NULL);
	g_signal_connect(w, "focus-in-event", G_CALLBACK(dial_focus_change), NULL);
	g_signal_connect(w, "focus-out-event", G_CALLBACK(dial_focus_change), NULL);
	g_signal_connect(w, "unrealize", G_CALLBACK(dial_unrealize), NULL);
	dial_get_accessible_name(w);
	return w;
}
