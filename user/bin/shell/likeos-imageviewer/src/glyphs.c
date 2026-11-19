// LikeOS -- LikeOS Image Viewer: the program's own line glyphs.
//
// Every glyph is drawn here as vector geometry of LikeOS's own, so it is
// sharp at any size and takes whatever colour the caller has set as the
// cairo source.  The drawings sit on a 16-unit box with strokes 1.5 units
// wide, round caps and round joins; solid shapes only where a shape reads
// better filled (the play triangle, the pause bars, the dots of "more").
// The window frame glyphs use the 10-unit box and the 1.25-pixel stroke
// that every LikeOS title bar draws (the window manager's and Explorer's).
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "glyphs.h"

typedef void (*Painter)(cairo_t *cr);

#define PI G_PI

/* ---- helpers, in glyph units ------------------------------------------ */

static void line(cairo_t *cr, double x0, double y0, double x1, double y1)
{
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
	cairo_stroke(cr);
}

/* An open polyline of `n' points. */
static void poly(cairo_t *cr, const double *p, int n)
{
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < n; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_stroke(cr);
}

/* A closed outline of `n' points. */
static void closed(cairo_t *cr, const double *p, int n)
{
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < n; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

/* A closed shape filled and outlined with the round join, which rounds
 * its corners by half a stroke. */
static void solid(cairo_t *cr, const double *p, int n)
{
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < n; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_close_path(cr);
	cairo_fill_preserve(cr);
	cairo_stroke(cr);
}

static void circle(cairo_t *cr, double cx, double cy, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * PI);
	cairo_stroke(cr);
}

static void dot(cairo_t *cr, double cx, double cy, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * PI);
	cairo_fill(cr);
}

static void rrect_path(cairo_t *cr, double x0, double y0, double x1,
		       double y1, double r)
{
	r = MIN(r, MIN((x1 - x0) / 2, (y1 - y0) / 2));
	cairo_new_sub_path(cr);
	cairo_arc(cr, x1 - r, y0 + r, r, -0.5 * PI, 0);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, 0.5 * PI);
	cairo_arc(cr, x0 + r, y1 - r, r, 0.5 * PI, PI);
	cairo_arc(cr, x0 + r, y0 + r, r, PI, 1.5 * PI);
	cairo_close_path(cr);
}

static void rrect(cairo_t *cr, double x0, double y0, double x1, double y1,
		  double r)
{
	rrect_path(cr, x0, y0, x1, y1, r);
	cairo_stroke(cr);
}

static void rrect_fill(cairo_t *cr, double x0, double y0, double x1,
		       double y1, double r)
{
	rrect_path(cr, x0, y0, x1, y1, r);
	cairo_fill(cr);
}

/* An open arrow head with its tip at (x, y), pointing along (dx, dy):
 * two wings `len' long, 45 degrees either side of the way back. */
static void arrow_head(cairo_t *cr, double x, double y, double dx, double dy,
		       double len)
{
	double l = hypot(dx, dy);

	if (l <= 0)
		return;
	dx /= l;
	dy /= l;
	const double c = cos(PI / 4), s = sin(PI / 4);
	double ux = -dx, uy = -dy;
	double ax = ux * c - uy * s, ay = ux * s + uy * c;
	double bx = ux * c + uy * s, by = -ux * s + uy * c;

	cairo_move_to(cr, x + ax * len, y + ay * len);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x + bx * len, y + by * len);
	cairo_stroke(cr);
}

/* An arc from angle a0 to a1 (clockwise on screen when a1 > a0) with an
 * arrow head at its end, pointing the way the arc runs. */
static void arc_arrow(cairo_t *cr, double cx, double cy, double r, double a0,
		      double a1, double head)
{
	cairo_new_sub_path(cr);
	if (a1 >= a0)
		cairo_arc(cr, cx, cy, r, a0, a1);
	else
		cairo_arc_negative(cr, cx, cy, r, a0, a1);
	cairo_stroke(cr);
	double ex = cx + r * cos(a1), ey = cy + r * sin(a1);
	/* The tangent: clockwise (-sin, cos), the other way (sin, -cos). */
	double tx = -sin(a1), ty = cos(a1);

	if (a1 < a0) {
		tx = -tx;
		ty = -ty;
	}
	arrow_head(cr, ex, ey, tx, ty, head);
}

/* Strokes the outline `back' with whatever lies within `gap' of the
 * rounded rectangle `front' (and inside it) cut away, so a clear band
 * separates a shape that sits behind another one. */
static void behind(cairo_t *cr, void (*back)(cairo_t *cr), double fx0,
		   double fy0, double fx1, double fy1, double fr, double gap)
{
	double lw = cairo_get_line_width(cr);

	cairo_push_group(cr);
	back(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	rrect_path(cr, fx0, fy0, fx1, fy1, fr);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, lw + 2 * gap);
	cairo_stroke(cr);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
}

/* ---- the window frame (10-unit box) ------------------------------------ */

static void fr_minimize(cairo_t *cr)
{
	line(cr, 1, 5.5, 9, 5.5);
}

static void fr_maximize(cairo_t *cr)
{
	rrect(cr, 0.5, 0.5, 9.5, 9.5, 2.5);
}

static void fr_restore_back(cairo_t *cr)
{
	rrect(cr, 2.5, 0.5, 9.5, 7.5, 2);
}

static void fr_restore(cairo_t *cr)
{
	cairo_pattern_t *src = cairo_get_source(cr);

	cairo_pattern_reference(src);
	/* Cut 1.625 units round the front square's outline, as on every
	 * LikeOS title bar: a clear gap between the two. */
	behind(cr, fr_restore_back, 0.5, 2.5, 7.5, 9.5, 2, 1.0);
	cairo_set_source(cr, src);
	cairo_pattern_destroy(src);
	rrect(cr, 0.5, 2.5, 7.5, 9.5, 2);
}

static void fr_close(cairo_t *cr)
{
	line(cr, 1, 1, 9, 9);
	line(cr, 1, 9, 9, 1);
}

/* ---- viewer commands --------------------------------------------------- */

static void gl_edit(cairo_t *cr)
{
	/* A pencil at 45 degrees over a short base line. */
	static const double body[] = { 2.75, 13.25, 3.5, 10.25, 10.5, 3.25,
				       12.75, 5.5, 5.75, 12.5 };
	closed(cr, body, 5);
	line(cr, 9, 4.75, 11.25, 7);
	line(cr, 9.25, 13.5, 13.5, 13.5);
}

/* A three-quarter turn with its arrow head, round a small picture tile. */
static void rotate_common(cairo_t *cr, gboolean right)
{
	rrect(cr, 5.75, 6.25, 10.25, 10.75, 1.1);
	if (right)
		arc_arrow(cr, 8, 8.5, 5.75, 0.72 * PI, 1.98 * PI, 2.6);
	else
		arc_arrow(cr, 8, 8.5, 5.75, 2.28 * PI, 1.02 * PI, 2.6);
}

static void gl_rotate_right(cairo_t *cr)
{
	rotate_common(cr, TRUE);
}

static void gl_rotate_left(cairo_t *cr)
{
	rotate_common(cr, FALSE);
}

static void gl_delete(cairo_t *cr)
{
	static const double handle[] = { 6, 4.25, 6.5, 2.25, 9.5, 2.25,
					 10, 4.25 };
	line(cr, 2.25, 4.25, 13.75, 4.25);
	poly(cr, handle, 4);
	cairo_move_to(cr, 3.75, 4.25);
	cairo_line_to(cr, 4.55, 12.4);
	cairo_curve_to(cr, 4.65, 13.2, 5.1, 13.75, 5.9, 13.75);
	cairo_line_to(cr, 10.1, 13.75);
	cairo_curve_to(cr, 10.9, 13.75, 11.35, 13.2, 11.45, 12.4);
	cairo_line_to(cr, 12.25, 4.25);
	cairo_stroke(cr);
	line(cr, 6.75, 7, 6.75, 11);
	line(cr, 9.25, 7, 9.25, 11);
}

static void gl_more(cairo_t *cr)
{
	dot(cr, 3.25, 8, 1.2);
	dot(cr, 8, 8, 1.2);
	dot(cr, 12.75, 8, 1.2);
}

static void gl_open_with(cairo_t *cr)
{
	/* A box with its top-right corner open and an arrow leaving it. */
	static const double head[] = { 9.25, 2.5, 13.5, 2.5, 13.5, 6.75 };
	cairo_move_to(cr, 7, 3);
	cairo_line_to(cr, 4.5, 3);
	cairo_arc_negative(cr, 4.5, 5, 2, 1.5 * PI, PI);
	cairo_line_to(cr, 2.5, 11.5);
	cairo_arc_negative(cr, 4.5, 11.5, 2, PI, 0.5 * PI);
	cairo_line_to(cr, 11, 13.5);
	cairo_arc_negative(cr, 11, 11.5, 2, 0.5 * PI, 0);
	cairo_line_to(cr, 13, 9);
	cairo_stroke(cr);
	line(cr, 7.75, 8.25, 13.25, 2.75);
	poly(cr, head, 3);
}

static void gl_chevron_down(cairo_t *cr)
{
	static const double p[] = { 4, 6, 8, 10, 12, 6 };
	poly(cr, p, 3);
}

static void gl_chevron_up(cairo_t *cr)
{
	static const double p[] = { 4, 10, 8, 6, 12, 10 };
	poly(cr, p, 3);
}

static void gl_chevron_left(cairo_t *cr)
{
	static const double p[] = { 10, 4, 6, 8, 10, 12 };
	poly(cr, p, 3);
}

static void gl_chevron_right(cairo_t *cr)
{
	static const double p[] = { 6, 4, 10, 8, 6, 12 };
	poly(cr, p, 3);
}

/* Previous and next in the strip: tall, open chevrons. */
static void gl_previous(cairo_t *cr)
{
	static const double p[] = { 10.25, 2.5, 4.75, 8, 10.25, 13.5 };
	poly(cr, p, 3);
}

static void gl_next(cairo_t *cr)
{
	static const double p[] = { 5.75, 2.5, 11.25, 8, 5.75, 13.5 };
	poly(cr, p, 3);
}

static void gl_save_as(cairo_t *cr)
{
	/* A tray and an arrow into it, with a small pencil mark beside. */
	static const double tray[] = { 2.25, 9.75, 2.25, 13.5, 13.75, 13.5,
				       13.75, 9.75 };
	static const double head[] = { 4.75, 6.75, 8, 10, 11.25, 6.75 };
	poly(cr, tray, 4);
	line(cr, 8, 2.25, 8, 10);
	poly(cr, head, 3);
}

static void copy_back(cairo_t *cr)
{
	rrect(cr, 5.5, 2.25, 13.75, 10.5, 2);
}

static void gl_copy(cairo_t *cr)
{
	cairo_pattern_t *src = cairo_get_source(cr);

	cairo_pattern_reference(src);
	behind(cr, copy_back, 2.25, 5.5, 10.5, 13.75, 2, 1.25);
	cairo_set_source(cr, src);
	cairo_pattern_destroy(src);
	rrect(cr, 2.25, 5.5, 10.5, 13.75, 2);
}

static void gl_copy_path(cairo_t *cr)
{
	/* Two links of a chain, at 45 degrees. */
	cairo_save(cr);
	cairo_translate(cr, 8, 8);
	cairo_rotate(cr, -PI / 4);
	rrect(cr, -7, -2.4, 1.25, 2.4, 2.4);
	rrect(cr, -1.25, -2.4, 7, 2.4, 2.4);
	cairo_restore(cr);
}

static void gl_resize(cairo_t *cr)
{
	static const double frame_a[] = { 2.25, 6, 2.25, 2.25, 6, 2.25 };
	static const double frame_b[] = { 10, 13.75, 13.75, 13.75, 13.75,
					  10 };
	static const double head[] = { 9.25, 2.75, 13.25, 2.75, 13.25, 6.75 };
	rrect(cr, 2.25, 9, 7, 13.75, 1.5);
	poly(cr, frame_a, 3);
	poly(cr, frame_b, 3);
	line(cr, 8.5, 7.5, 13, 3);
	poly(cr, head, 3);
}

static void gl_wallpaper(cairo_t *cr)
{
	/* A screen on a foot, a landscape on it. */
	static const double hills[] = { 3.75, 9.75, 6.5, 6.75, 8.5, 8.75,
					9.75, 7.5, 12.25, 9.75 };
	rrect(cr, 1.5, 2.25, 14.5, 11.5, 2);
	poly(cr, hills, 5);
	dot(cr, 11.25, 5, 0.95);
	line(cr, 8, 11.5, 8, 13.75);
	line(cr, 5, 13.75, 11, 13.75);
}

static void gl_mail(cairo_t *cr)
{
	static const double flap[] = { 2.5, 4.75, 8, 8.75, 13.5, 4.75 };
	rrect(cr, 1.5, 3.25, 14.5, 12.75, 2);
	poly(cr, flap, 3);
}

static void rename_field(cairo_t *cr)
{
	rrect(cr, 1.25, 4.5, 14.75, 11.5, 2);
}

static void gl_rename(cairo_t *cr)
{
	/* A text field with the text cursor standing through it. */
	cairo_pattern_t *src = cairo_get_source(cr);
	double lw = cairo_get_line_width(cr);

	cairo_pattern_reference(src);
	cairo_push_group(cr);
	rename_field(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_set_line_width(cr, lw * 3);
	line(cr, 10.5, 2, 10.5, 14);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	cairo_set_source(cr, src);
	cairo_pattern_destroy(src);
	cairo_set_line_width(cr, lw);
	line(cr, 10.5, 2.25, 10.5, 13.75);
	line(cr, 9, 2.25, 12, 2.25);
	line(cr, 9, 13.75, 12, 13.75);
	line(cr, 4, 8, 7.25, 8);
}

static void gl_settings(cairo_t *cr)
{
	/* A wheel of eight flat teeth round a ring. */
	const int teeth = 8;
	const double ro = 6.5, ri = 4.85;

	for (int k = 0; k < teeth; k++) {
		double a = k * 2 * PI / teeth;
		double t0 = a - 0.2, t1 = a + 0.2;
		double v0 = a + 0.36, v1 = a + 2 * PI / teeth - 0.36;

		if (k == 0)
			cairo_move_to(cr, 8 + ro * cos(t0), 8 + ro * sin(t0));
		else
			cairo_line_to(cr, 8 + ro * cos(t0), 8 + ro * sin(t0));
		cairo_line_to(cr, 8 + ro * cos(t1), 8 + ro * sin(t1));
		cairo_line_to(cr, 8 + ri * cos(v0), 8 + ri * sin(v0));
		cairo_arc(cr, 8, 8, ri, v0, v1);
	}
	cairo_close_path(cr);
	cairo_stroke(cr);
	circle(cr, 8, 8, 2.1);
}

static void gl_keyboard(cairo_t *cr)
{
	rrect(cr, 1.25, 3.75, 14.75, 12.25, 2);
	for (int i = 0; i < 4; i++)
		dot(cr, 4.25 + 2.5 * i, 6.5, 0.72);
	for (int i = 0; i < 3; i++)
		dot(cr, 5.5 + 2.5 * i, 8.6, 0.72);
	line(cr, 5.25, 10.25, 10.75, 10.25);
}

static void gl_about(cairo_t *cr)
{
	rrect(cr, 2, 2, 14, 14, 3.5);
	line(cr, 8, 7.25, 8, 11);
	dot(cr, 8, 5, 0.95);
}

static void gl_folder(cairo_t *cr)
{
	static const double p[] = { 1.5, 12.5, 1.5, 3.5, 6, 3.5, 7.5, 5,
				    14.5, 5, 14.5, 12.5 };
	closed(cr, p, 6);
	line(cr, 1.5, 7.25, 14.5, 7.25);
}

static void gl_folder_open(cairo_t *cr)
{
	static const double back[] = { 1.5, 12.5, 1.5, 3.5, 6, 3.5, 7.5, 5,
				       12.25, 5, 12.25, 7.25 };
	static const double front[] = { 1.5, 12.5, 4, 7.25, 14.75, 7.25,
					12.25, 12.5 };
	poly(cr, back, 6);
	closed(cr, front, 4);
}

static void gl_app(cairo_t *cr)
{
	rrect(cr, 2.25, 2.25, 13.75, 13.75, 3.25);
	circle(cr, 8, 8, 2.25);
}

/* ---- the bottom strip --------------------------------------------------- */

static void gl_filmstrip(cairo_t *cr)
{
	/* The current frame large in the middle, its neighbours peeking. */
	rrect(cr, 4.75, 3.25, 11.25, 12.75, 1.75);
	rrect(cr, 0.85, 5.25, 2.6, 10.75, 0.8);
	rrect(cr, 13.4, 5.25, 15.15, 10.75, 0.8);
}

static void gl_slideshow(cairo_t *cr)
{
	static const double tri[] = { 6.6, 4.75, 10.4, 7, 6.6, 9.25 };
	rrect(cr, 1.5, 2, 14.5, 11.75, 2);
	solid(cr, tri, 3);
	line(cr, 4.75, 14, 11.25, 14);
}

static void gl_play(cairo_t *cr)
{
	static const double p[] = { 5, 3.5, 12.75, 8, 5, 12.5 };
	solid(cr, p, 3);
}

static void gl_pause(cairo_t *cr)
{
	rrect_fill(cr, 4, 3, 6.75, 13, 1.1);
	rrect_fill(cr, 9.25, 3, 12, 13, 1.1);
}

static void gl_info(cairo_t *cr)
{
	circle(cr, 8, 8, 6.25);
	line(cr, 8, 7.25, 8, 11.25);
	dot(cr, 8, 4.85, 0.95);
}

static void gl_fit(cairo_t *cr)
{
	static const double a[] = { 2.25, 5.75, 2.25, 2.25, 5.75, 2.25 };
	static const double b[] = { 10.25, 2.25, 13.75, 2.25, 13.75, 5.75 };
	static const double c[] = { 13.75, 10.25, 13.75, 13.75, 10.25,
				    13.75 };
	static const double d[] = { 5.75, 13.75, 2.25, 13.75, 2.25, 10.25 };
	poly(cr, a, 3);
	poly(cr, b, 3);
	poly(cr, c, 3);
	poly(cr, d, 3);
	rrect(cr, 5.5, 5.5, 10.5, 10.5, 1.25);
}

static void gl_actual_size(cairo_t *cr)
{
	/* "1:1" in a frame. */
	static const double one_a[] = { 3.9, 6.6, 5, 5.75, 5, 10.5 };
	static const double one_b[] = { 9.9, 6.6, 11, 5.75, 11, 10.5 };
	rrect(cr, 1.25, 2.25, 14.75, 13.75, 2);
	poly(cr, one_a, 3);
	poly(cr, one_b, 3);
	dot(cr, 8, 6.9, 0.8);
	dot(cr, 8, 9.6, 0.8);
}

static void magnifier(cairo_t *cr)
{
	circle(cr, 7, 7, 4.85);
	line(cr, 10.55, 10.55, 13.75, 13.75);
}

static void gl_zoom_in(cairo_t *cr)
{
	magnifier(cr);
	line(cr, 4.75, 7, 9.25, 7);
	line(cr, 7, 4.75, 7, 9.25);
}

static void gl_zoom_out(cairo_t *cr)
{
	magnifier(cr);
	line(cr, 4.75, 7, 9.25, 7);
}

static void gl_fullscreen(cairo_t *cr)
{
	/* Four arrows pointing out of the corners. */
	static const double a[] = { 2.25, 6, 2.25, 2.25, 6, 2.25 };
	static const double b[] = { 10, 2.25, 13.75, 2.25, 13.75, 6 };
	static const double c[] = { 13.75, 10, 13.75, 13.75, 10, 13.75 };
	static const double d[] = { 6, 13.75, 2.25, 13.75, 2.25, 10 };
	poly(cr, a, 3);
	poly(cr, b, 3);
	poly(cr, c, 3);
	poly(cr, d, 3);
	line(cr, 2.5, 2.5, 6, 6);
	line(cr, 13.5, 2.5, 10, 6);
	line(cr, 13.5, 13.5, 10, 10);
	line(cr, 2.5, 13.5, 6, 10);
}

static void gl_leave_fullscreen(cairo_t *cr)
{
	/* The same four arrows, pointing in. */
	static const double a[] = { 6, 2.25, 6, 6, 2.25, 6 };
	static const double b[] = { 13.75, 6, 10, 6, 10, 2.25 };
	static const double c[] = { 10, 13.75, 10, 10, 13.75, 10 };
	static const double d[] = { 2.25, 10, 6, 10, 6, 13.75 };
	poly(cr, a, 3);
	poly(cr, b, 3);
	poly(cr, c, 3);
	poly(cr, d, 3);
	line(cr, 2.25, 2.25, 5.75, 5.75);
	line(cr, 13.75, 2.25, 10.25, 5.75);
	line(cr, 13.75, 13.75, 10.25, 10.25);
	line(cr, 2.25, 13.75, 5.75, 10.25);
}

/* ---- the info panel ---------------------------------------------------- */

static void gl_image(cairo_t *cr)
{
	static const double hills[] = { 3.5, 11.25, 6.5, 7.75, 8.75, 10,
					 10.25, 8.5, 12.5, 11.25 };
	rrect(cr, 1.5, 2.25, 14.5, 13.75, 2);
	poly(cr, hills, 5);
	circle(cr, 10.75, 5.5, 1.2);
}

static void gl_calendar(cairo_t *cr)
{
	rrect(cr, 2, 3.25, 14, 13.75, 2);
	line(cr, 2, 6.5, 14, 6.5);
	line(cr, 5.5, 1.75, 5.5, 4.25);
	line(cr, 10.5, 1.75, 10.5, 4.25);
	for (int r = 0; r < 2; r++)
		for (int c = 0; c < 3; c++)
			dot(cr, 5 + 3 * c, 9 + 2.5 * r, 0.72);
}

static void gl_camera(cairo_t *cr)
{
	static const double bump[] = { 5, 4.5, 6.25, 2.75, 9.75, 2.75, 11,
				       4.5 };
	rrect(cr, 1.5, 4.5, 14.5, 13, 2);
	poly(cr, bump, 4);
	circle(cr, 8, 8.75, 2.6);
	dot(cr, 12, 6.75, 0.6);
}

static void gl_location(cairo_t *cr)
{
	/* A pin: a round head running to a point. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 6.5, 4.75, 0.82 * PI, 2.18 * PI);
	cairo_line_to(cr, 8, 14.25);
	cairo_close_path(cr);
	cairo_stroke(cr);
	circle(cr, 8, 6.5, 1.6);
}

static void gl_file(cairo_t *cr)
{
	static const double page[] = { 3.25, 1.75, 9.5, 1.75, 12.75, 5,
				       12.75, 14.25, 3.25, 14.25 };
	static const double fold[] = { 9.5, 1.75, 9.5, 5, 12.75, 5 };
	closed(cr, page, 5);
	poly(cr, fold, 3);
}

static void gl_check(cairo_t *cr)
{
	static const double p[] = { 3, 8.5, 6.5, 12, 13, 4.5 };
	poly(cr, p, 3);
}

static void gl_warning(cairo_t *cr)
{
	static const double p[] = { 8, 2, 14.5, 13.5, 1.5, 13.5 };
	closed(cr, p, 3);
	line(cr, 8, 6.5, 8, 9.25);
	dot(cr, 8, 11.4, 0.85);
}

/* ---- the editor shell -------------------------------------------------- */

static void gl_back(cairo_t *cr)
{
	static const double head[] = { 7.5, 3.5, 3, 8, 7.5, 12.5 };
	line(cr, 3.25, 8, 13.5, 8);
	poly(cr, head, 3);
}

static void gl_undo(cairo_t *cr)
{
	static const double head[] = { 6.25, 3.25, 3.25, 6.25, 6.25, 9.25 };
	cairo_move_to(cr, 3.5, 6.25);
	cairo_line_to(cr, 9.75, 6.25);
	cairo_arc(cr, 9.75, 9.5, 3.25, -0.5 * PI, 0.5 * PI);
	cairo_line_to(cr, 6, 12.75);
	cairo_stroke(cr);
	poly(cr, head, 3);
}

static void gl_redo(cairo_t *cr)
{
	static const double head[] = { 9.75, 3.25, 12.75, 6.25, 9.75, 9.25 };
	cairo_move_to(cr, 12.5, 6.25);
	cairo_line_to(cr, 6.25, 6.25);
	cairo_arc_negative(cr, 6.25, 9.5, 3.25, -0.5 * PI, 0.5 * PI);
	cairo_line_to(cr, 10, 12.75);
	cairo_stroke(cr);
	poly(cr, head, 3);
}

static void gl_revert(cairo_t *cr)
{
	/* An arc running back round a clock face. */
	arc_arrow(cr, 8, 8, 5.75, 2.9 * PI, 1.15 * PI, 2.5);
	static const double hands[] = { 8, 5, 8, 8, 10.25, 9.5 };
	poly(cr, hands, 3);
}

static void gl_compare(cairo_t *cr)
{
	/* A frame split down the middle, the left half solid. */
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, 8, 16);
	cairo_clip(cr);
	rrect_fill(cr, 1.5, 2.5, 14.5, 13.5, 2);
	cairo_restore(cr);
	rrect(cr, 1.5, 2.5, 14.5, 13.5, 2);
	line(cr, 8, 1, 8, 15);
}

static void gl_plus(cairo_t *cr)
{
	line(cr, 8, 3, 8, 13);
	line(cr, 3, 8, 13, 8);
}

static void gl_minus(cairo_t *cr)
{
	line(cr, 3, 8, 13, 8);
}

/* ---- the editor tools ---------------------------------------------------- */

static void gl_crop(cairo_t *cr)
{
	static const double a[] = { 4.5, 1.5, 4.5, 11.5, 14.5, 11.5 };
	static const double b[] = { 1.5, 4.5, 11.5, 4.5, 11.5, 14.5 };
	poly(cr, a, 3);
	poly(cr, b, 3);
}

static void gl_adjust(cairo_t *cr)
{
	/* Three sliders with their knobs at different places. */
	static const struct {
		double y, k;
	} rows[] = { { 3.75, 10.25 }, { 8, 5.25 }, { 12.25, 9 } };

	for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
		double y = rows[i].y, k = rows[i].k;

		if (k - 2.5 > 1.75)
			line(cr, 1.75, y, k - 2.5, y);
		if (k + 2.5 < 14.25)
			line(cr, k + 2.5, y, 14.25, y);
		circle(cr, k, y, 1.75);
	}
}

static void gl_styles(cairo_t *cr)
{
	circle(cr, 8, 5.6, 3.35);
	circle(cr, 5.3, 10.3, 3.35);
	circle(cr, 10.7, 10.3, 3.35);
}

static void gl_draw(cairo_t *cr)
{
	/* A marker drawing a wave. */
	static const double body[] = { 6.5, 9.5, 12, 4, 14, 6, 8.5, 11.5 };
	static const double tip[] = { 6.5, 9.5, 5.25, 12.75, 8.5, 11.5 };
	closed(cr, body, 4);
	poly(cr, tip, 3);
	cairo_move_to(cr, 1.5, 14.25);
	cairo_curve_to(cr, 2.5, 12.75, 3.5, 12.75, 4.25, 14.25);
	cairo_stroke(cr);
}

static void gl_retouch(cairo_t *cr)
{
	/* A dotted spot and the swab that clears it. */
	double lw = cairo_get_line_width(cr);
	static const double dash[] = { 0.01, 2.35 };

	cairo_save(cr);
	cairo_set_dash(cr, dash, 2, 0);
	cairo_set_line_width(cr, lw * 1.15);
	circle(cr, 5.75, 10.25, 3.75);
	cairo_restore(cr);
	cairo_save(cr);
	cairo_translate(cr, 11.25, 4.75);
	cairo_rotate(cr, -PI / 4);
	rrect(cr, -4, -1.9, 4, 1.9, 1.9);
	line(cr, -0.75, -1.9, -0.75, 1.9);
	cairo_restore(cr);
}

static void person(cairo_t *cr, double cx, double top)
{
	circle(cr, cx, top + 2.25, 2.25);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, top + 10.5, 4.25, PI, 2 * PI);
	cairo_stroke(cr);
}

static void gl_background(cairo_t *cr)
{
	/* A person in front of a hatched backdrop. */
	cairo_save(cr);
	rrect_path(cr, 1.5, 2, 14.5, 14, 2);
	cairo_clip_preserve(cr);
	cairo_stroke(cr);
	for (double x = -12; x < 16; x += 3.5)
		line(cr, x, 14, x + 12, 2);
	cairo_restore(cr);
	/* The figure cleared out of the hatching, then drawn. */
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 6.25, 3.25, 0, 2 * PI);
	cairo_fill(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 14.5, 5.25, PI, 2 * PI);
	cairo_fill(cr);
	cairo_restore(cr);
	person(cr, 8, 4);
}

/* ---- inside the tool panels ---------------------------------------------- */

static void flip_common(cairo_t *cr)
{
	static const double left[] = { 6.25, 3.5, 6.25, 12.5, 1.5, 12.5 };
	static const double right[] = { 9.75, 3.5, 9.75, 12.5, 14.5, 12.5 };
	static const double dash[] = { 1.2, 1.8 };

	closed(cr, left, 3);
	solid(cr, right, 3);
	cairo_save(cr);
	cairo_set_dash(cr, dash, 2, 0);
	line(cr, 8, 1, 8, 15);
	cairo_restore(cr);
}

static void gl_flip_h(cairo_t *cr)
{
	flip_common(cr);
}

static void gl_flip_v(cairo_t *cr)
{
	cairo_save(cr);
	cairo_translate(cr, 8, 8);
	cairo_rotate(cr, PI / 2);
	cairo_translate(cr, -8, -8);
	flip_common(cr);
	cairo_restore(cr);
}

static void gl_straighten(cairo_t *cr)
{
	/* A dial with its ticks and a tilted needle. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 12.5, 6.25, 1.08 * PI, 1.92 * PI);
	cairo_stroke(cr);
	for (int i = -2; i <= 2; i++) {
		double a = 1.5 * PI + i * 0.18 * PI;

		line(cr, 8 + 6.25 * cos(a), 12.5 + 6.25 * sin(a),
		     8 + 4.9 * cos(a), 12.5 + 4.9 * sin(a));
	}
	line(cr, 8, 12.5, 10.75, 7.25);
	dot(cr, 8, 12.5, 1.1);
}

static void gl_aspect(cairo_t *cr)
{
	static const double a[] = { 4, 8.25, 4, 5.75, 6.5, 5.75 };
	static const double b[] = { 12, 7.75, 12, 10.25, 9.5, 10.25 };
	rrect(cr, 1.5, 3, 14.5, 13, 2);
	poly(cr, a, 3);
	poly(cr, b, 3);
}

static void gl_swap(cairo_t *cr)
{
	line(cr, 2.75, 5.25, 12.75, 5.25);
	arrow_head(cr, 13, 5.25, 1, 0, 3);
	line(cr, 13.25, 10.75, 3.25, 10.75);
	arrow_head(cr, 3, 10.75, -1, 0, 3);
}

static void gl_reset(cairo_t *cr)
{
	arc_arrow(cr, 8, 8, 5.5, 0.6 * PI, 2.35 * PI, 2.6);
	dot(cr, 8, 8, 1.1);
}

static void gl_light(cairo_t *cr)
{
	circle(cr, 8, 8, 2.75);
	for (int i = 0; i < 8; i++) {
		double a = i * PI / 4;

		line(cr, 8 + 4.75 * cos(a), 8 + 4.75 * sin(a),
		     8 + 6.5 * cos(a), 8 + 6.5 * sin(a));
	}
}

static void gl_color(cairo_t *cr)
{
	/* A drop. */
	cairo_move_to(cr, 8, 1.75);
	cairo_curve_to(cr, 9.5, 4.5, 13, 7.25, 13, 10);
	cairo_arc(cr, 8, 10, 5, 0, PI);
	cairo_curve_to(cr, 3, 7.25, 6.5, 4.5, 8, 1.75);
	cairo_close_path(cr);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 10, 2.75, 0.55 * PI, 0.95 * PI);
	cairo_stroke(cr);
}

static void gl_detail(cairo_t *cr)
{
	/* A diamond, its left half solid. */
	static const double d[] = { 8, 1.75, 14.25, 8, 8, 14.25, 1.75, 8 };
	static const double half[] = { 8, 1.75, 8, 14.25, 1.75, 8 };
	solid(cr, half, 3);
	closed(cr, d, 4);
}

static void gl_wand(cairo_t *cr)
{
	/* A wand with short rays at its tip. */
	line(cr, 2.25, 13.75, 10, 6);
	line(cr, 12.25, 1.5, 12.25, 2.9);
	line(cr, 12.25, 5.6, 12.25, 7);
	line(cr, 9.5, 4.25, 10.9, 4.25);
	line(cr, 13.6, 4.25, 15, 4.25);
	dot(cr, 12.25, 4.25, 0.9);
}

static void gl_pen(cairo_t *cr)
{
	static const double body[] = { 2.5, 13.5, 3.25, 10.75, 11, 3, 13,
				       5, 5.25, 12.75 };
	closed(cr, body, 5);
	line(cr, 3.25, 10.75, 5.25, 12.75);
}

static void gl_marker(cairo_t *cr)
{
	/* A broad marker with a chisel tip over its stroke. */
	cairo_save(cr);
	cairo_translate(cr, 9.25, 6.75);
	cairo_rotate(cr, PI / 4);
	rrect(cr, -2.75, -6, 2.75, 2.5, 1.25);
	static const double tip[] = { -1.75, 2.5, -1, 5, 1.75, 5, 1.75, 2.5 };
	poly(cr, tip, 4);
	cairo_restore(cr);
	line(cr, 1.75, 14.25, 8, 14.25);
}

static void gl_calligraphy(cairo_t *cr)
{
	/* A broad nib, point down. */
	static const double nib[] = { 4.25, 2, 11.75, 2, 12.75, 7.25, 8,
				      14.5, 3.25, 7.25 };
	closed(cr, nib, 5);
	line(cr, 8, 9.25, 8, 14.25);
	dot(cr, 8, 7.75, 1.1);
}

static void gl_eraser(cairo_t *cr)
{
	cairo_save(cr);
	cairo_translate(cr, 8.5, 7.5);
	cairo_rotate(cr, -PI / 4);
	rrect(cr, -6, -3, 6, 3, 1.5);
	line(cr, -1.25, -3, -1.25, 3);
	cairo_restore(cr);
	line(cr, 6, 14.25, 14, 14.25);
}

static void gl_rect(cairo_t *cr)
{
	/* A solid bar laid over the middle of three lines of text. */
	static const double bar[] = { 2.75, 6.25, 13.25, 6.25, 13.25, 9.75,
				      2.75, 9.75 };
	line(cr, 2, 2.75, 14, 2.75);
	solid(cr, bar, 4);
	line(cr, 2, 13.25, 10, 13.25);
}

static void gl_clear(cairo_t *cr)
{
	rrect(cr, 2.25, 2.25, 13.75, 13.75, 2.5);
	line(cr, 5.5, 5.5, 10.5, 10.5);
	line(cr, 10.5, 5.5, 5.5, 10.5);
}

static void gl_brush(cairo_t *cr)
{
	static const double bristles[] = { 7.75, 6.75, 9.25, 8.25, 7.25,
					   11.5, 2.75, 13.25, 4.5, 8.75 };
	line(cr, 14, 2, 9.75, 6.25);
	solid(cr, bristles, 5);
}

static void gl_blur(cairo_t *cr)
{
	/* Dots that shrink away from the middle. */
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			int d = abs(x) + abs(y);
			double r = d == 0 ? 1.5 : d == 1 ? 1.0 : 0.6;

			dot(cr, 8 + 4.5 * x, 8 + 4.5 * y, r);
		}
}

static void gl_remove_bg(cairo_t *cr)
{
	static const double dash[] = { 1.5, 1.75 };

	cairo_save(cr);
	cairo_set_dash(cr, dash, 2, 0);
	rrect(cr, 1.5, 2, 14.5, 14, 2);
	cairo_restore(cr);
	person(cr, 8, 4);
}

static void gl_fill(cairo_t *cr)
{
	/* A tipped pail and a drop. */
	cairo_save(cr);
	cairo_translate(cr, 7, 8.5);
	cairo_rotate(cr, PI / 4);
	rrect(cr, -4, -4, 4, 4, 1);
	cairo_restore(cr);
	line(cr, 4.25, 2.5, 7, 5.5);
	dot(cr, 13.25, 12.25, 1.4);
}

static void gl_lock(cairo_t *cr)
{
	rrect(cr, 3, 7, 13, 14, 2);
	cairo_move_to(cr, 5, 7);
	cairo_line_to(cr, 5, 5.25);
	cairo_arc(cr, 8, 5.25, 3, PI, 2 * PI);
	cairo_line_to(cr, 11, 7);
	cairo_stroke(cr);
	dot(cr, 8, 10.5, 1);
}

static void gl_unlock(cairo_t *cr)
{
	rrect(cr, 3, 7, 13, 14, 2);
	cairo_move_to(cr, 5, 7);
	cairo_line_to(cr, 5, 3.75);
	cairo_arc(cr, 8, 3.75, 3, PI, 2 * PI);
	cairo_line_to(cr, 11, 4.75);
	cairo_stroke(cr);
	dot(cr, 8, 10.5, 1);
}

static void gl_eyedropper(cairo_t *cr)
{
	line(cr, 2.5, 13.5, 9.25, 6.75);
	line(cr, 7.75, 5.25, 10.75, 8.25);
	cairo_save(cr);
	cairo_translate(cr, 11.5, 4.5);
	cairo_rotate(cr, -PI / 4);
	rrect(cr, -1.6, -3.25, 1.6, 1.5, 1.6);
	cairo_restore(cr);
}

/* ---- the table ------------------------------------------------------------ */

static const struct {
	const char *name;
	Painter paint;
	gboolean frame; /* the 10-unit window frame box */
} painters[IV_GLYPH_COUNT] = {
	[IV_GLYPH_NONE] = { "none", NULL, FALSE },
	[IV_GLYPH_MINIMIZE] = { "minimize", fr_minimize, TRUE },
	[IV_GLYPH_MAXIMIZE] = { "maximize", fr_maximize, TRUE },
	[IV_GLYPH_RESTORE] = { "restore", fr_restore, TRUE },
	[IV_GLYPH_CLOSE] = { "close", fr_close, TRUE },
	[IV_GLYPH_EDIT] = { "edit", gl_edit, FALSE },
	[IV_GLYPH_ROTATE_RIGHT] = { "rotate-right", gl_rotate_right, FALSE },
	[IV_GLYPH_ROTATE_LEFT] = { "rotate-left", gl_rotate_left, FALSE },
	[IV_GLYPH_DELETE] = { "delete", gl_delete, FALSE },
	[IV_GLYPH_MORE] = { "more", gl_more, FALSE },
	[IV_GLYPH_OPEN_WITH] = { "open-with", gl_open_with, FALSE },
	[IV_GLYPH_CHEVRON_DOWN] = { "chevron-down", gl_chevron_down, FALSE },
	[IV_GLYPH_CHEVRON_UP] = { "chevron-up", gl_chevron_up, FALSE },
	[IV_GLYPH_CHEVRON_LEFT] = { "chevron-left", gl_chevron_left, FALSE },
	[IV_GLYPH_CHEVRON_RIGHT] = { "chevron-right", gl_chevron_right,
				     FALSE },
	[IV_GLYPH_PREVIOUS] = { "previous", gl_previous, FALSE },
	[IV_GLYPH_NEXT] = { "next", gl_next, FALSE },
	[IV_GLYPH_SAVE_AS] = { "save-as", gl_save_as, FALSE },
	[IV_GLYPH_COPY] = { "copy", gl_copy, FALSE },
	[IV_GLYPH_COPY_PATH] = { "copy-path", gl_copy_path, FALSE },
	[IV_GLYPH_RESIZE] = { "resize", gl_resize, FALSE },
	[IV_GLYPH_WALLPAPER] = { "wallpaper", gl_wallpaper, FALSE },
	[IV_GLYPH_MAIL] = { "mail", gl_mail, FALSE },
	[IV_GLYPH_RENAME] = { "rename", gl_rename, FALSE },
	[IV_GLYPH_SETTINGS] = { "settings", gl_settings, FALSE },
	[IV_GLYPH_KEYBOARD] = { "keyboard", gl_keyboard, FALSE },
	[IV_GLYPH_ABOUT] = { "about", gl_about, FALSE },
	[IV_GLYPH_FOLDER] = { "folder", gl_folder, FALSE },
	[IV_GLYPH_FOLDER_OPEN] = { "folder-open", gl_folder_open, FALSE },
	[IV_GLYPH_APP] = { "app", gl_app, FALSE },
	[IV_GLYPH_FILMSTRIP] = { "filmstrip", gl_filmstrip, FALSE },
	[IV_GLYPH_SLIDESHOW] = { "slideshow", gl_slideshow, FALSE },
	[IV_GLYPH_PLAY] = { "play", gl_play, FALSE },
	[IV_GLYPH_PAUSE] = { "pause", gl_pause, FALSE },
	[IV_GLYPH_INFO] = { "info", gl_info, FALSE },
	[IV_GLYPH_FIT] = { "fit", gl_fit, FALSE },
	[IV_GLYPH_ACTUAL_SIZE] = { "actual-size", gl_actual_size, FALSE },
	[IV_GLYPH_ZOOM_IN] = { "zoom-in", gl_zoom_in, FALSE },
	[IV_GLYPH_ZOOM_OUT] = { "zoom-out", gl_zoom_out, FALSE },
	[IV_GLYPH_FULLSCREEN] = { "fullscreen", gl_fullscreen, FALSE },
	[IV_GLYPH_LEAVE_FULLSCREEN] = { "leave-fullscreen",
					gl_leave_fullscreen, FALSE },
	[IV_GLYPH_IMAGE] = { "image", gl_image, FALSE },
	[IV_GLYPH_CALENDAR] = { "calendar", gl_calendar, FALSE },
	[IV_GLYPH_CAMERA] = { "camera", gl_camera, FALSE },
	[IV_GLYPH_LOCATION] = { "location", gl_location, FALSE },
	[IV_GLYPH_FILE] = { "file", gl_file, FALSE },
	[IV_GLYPH_CHECK] = { "check", gl_check, FALSE },
	[IV_GLYPH_WARNING] = { "warning", gl_warning, FALSE },
	[IV_GLYPH_BACK] = { "back", gl_back, FALSE },
	[IV_GLYPH_UNDO] = { "undo", gl_undo, FALSE },
	[IV_GLYPH_REDO] = { "redo", gl_redo, FALSE },
	[IV_GLYPH_REVERT] = { "revert", gl_revert, FALSE },
	[IV_GLYPH_COMPARE] = { "compare", gl_compare, FALSE },
	[IV_GLYPH_PLUS] = { "plus", gl_plus, FALSE },
	[IV_GLYPH_MINUS] = { "minus", gl_minus, FALSE },
	[IV_GLYPH_CROP] = { "crop", gl_crop, FALSE },
	[IV_GLYPH_ADJUST] = { "adjust", gl_adjust, FALSE },
	[IV_GLYPH_STYLES] = { "styles", gl_styles, FALSE },
	[IV_GLYPH_DRAW] = { "draw", gl_draw, FALSE },
	[IV_GLYPH_RETOUCH] = { "retouch", gl_retouch, FALSE },
	[IV_GLYPH_BACKGROUND] = { "background", gl_background, FALSE },
	[IV_GLYPH_FLIP_H] = { "flip-horizontal", gl_flip_h, FALSE },
	[IV_GLYPH_FLIP_V] = { "flip-vertical", gl_flip_v, FALSE },
	[IV_GLYPH_STRAIGHTEN] = { "straighten", gl_straighten, FALSE },
	[IV_GLYPH_ASPECT] = { "aspect", gl_aspect, FALSE },
	[IV_GLYPH_SWAP] = { "swap", gl_swap, FALSE },
	[IV_GLYPH_RESET] = { "reset", gl_reset, FALSE },
	[IV_GLYPH_LIGHT] = { "light", gl_light, FALSE },
	[IV_GLYPH_COLOR] = { "color", gl_color, FALSE },
	[IV_GLYPH_DETAIL] = { "detail", gl_detail, FALSE },
	[IV_GLYPH_WAND] = { "wand", gl_wand, FALSE },
	[IV_GLYPH_PEN] = { "pen", gl_pen, FALSE },
	[IV_GLYPH_MARKER] = { "marker", gl_marker, FALSE },
	[IV_GLYPH_CALLIGRAPHY] = { "calligraphy", gl_calligraphy, FALSE },
	[IV_GLYPH_ERASER] = { "eraser", gl_eraser, FALSE },
	[IV_GLYPH_RECT] = { "rectangle", gl_rect, FALSE },
	[IV_GLYPH_CLEAR] = { "clear", gl_clear, FALSE },
	[IV_GLYPH_BRUSH] = { "brush", gl_brush, FALSE },
	[IV_GLYPH_BLUR] = { "blur", gl_blur, FALSE },
	[IV_GLYPH_REMOVE_BG] = { "remove-background", gl_remove_bg, FALSE },
	[IV_GLYPH_FILL] = { "fill", gl_fill, FALSE },
	[IV_GLYPH_LOCK] = { "lock", gl_lock, FALSE },
	[IV_GLYPH_UNLOCK] = { "unlock", gl_unlock, FALSE },
	[IV_GLYPH_EYEDROPPER] = { "eyedropper", gl_eyedropper, FALSE },
};

const char *iv_glyph_name(IvGlyph g)
{
	if ((int)g < 0 || g >= IV_GLYPH_COUNT || !painters[g].name)
		return "none";
	return painters[g].name;
}

void iv_glyph_paint(cairo_t *cr, IvGlyph g, double cx, double cy,
		    double size)
{
	if ((int)g <= 0 || g >= IV_GLYPH_COUNT || !painters[g].paint ||
	    size < 4)
		return;
	cairo_save(cr);
	cairo_new_path(cr);
	if (painters[g].frame) {
		/* The title-bar box: 10 units, 1.25-pixel strokes, the box
		 * on whole pixels so the half-unit lines stay crisp. */
		double k = size / 10.0;

		cairo_translate(cr, floor(cx - size / 2.0),
				floor(cy - size / 2.0));
		cairo_scale(cr, k, k);
		cairo_set_line_width(cr, 1.25 / k);
	} else {
		double s = size / 16.0;
		/* 1.5 units at the small sizes; on a large placeholder the
		 * stroke grows more slowly than the glyph. */
		double px = size <= 24 ? 1.5 * s : MAX(2.25, 1.5 + size / 32.0);

		cairo_translate(cr, round(cx - size / 2.0),
				round(cy - size / 2.0));
		cairo_scale(cr, s, s);
		cairo_set_line_width(cr, px / s);
	}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	painters[g].paint(cr);
	cairo_restore(cr);
}

/* ---- the widget ---------------------------------------------------------- */

static gboolean glyph_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	IvGlyph g = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "iv-glyph"));
	int size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "iv-size"));
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	GdkRGBA c;

	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), &c);
	gdk_cairo_set_source_rgba(cr, &c);
	iv_glyph_paint(cr, g, gtk_widget_get_allocated_width(w) / 2.0,
		       gtk_widget_get_allocated_height(w) / 2.0, size);
	return TRUE;
}

GtkWidget *iv_glyph_new(IvGlyph g, int size)
{
	GtkWidget *w = gtk_drawing_area_new();

	g_object_set_data(G_OBJECT(w), "iv-glyph", GINT_TO_POINTER(g));
	g_object_set_data(G_OBJECT(w), "iv-size", GINT_TO_POINTER(size));
	gtk_widget_set_size_request(w, size, size);
	gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
	gtk_style_context_add_class(gtk_widget_get_style_context(w),
				    "iv-glyph");
	g_signal_connect(w, "draw", G_CALLBACK(glyph_draw), NULL);
	return w;
}

void iv_glyph_set(GtkWidget *glyph, IvGlyph g)
{
	if (!glyph)
		return;
	if (iv_glyph_get(glyph) == g)
		return;
	g_object_set_data(G_OBJECT(glyph), "iv-glyph", GINT_TO_POINTER(g));
	gtk_widget_queue_draw(glyph);
}

IvGlyph iv_glyph_get(GtkWidget *glyph)
{
	return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(glyph), "iv-glyph"));
}

cairo_surface_t *iv_glyph_surface(IvGlyph g, int size, const GdkRGBA *color)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							size, size);
	cairo_t *cr = cairo_create(s);

	gdk_cairo_set_source_rgba(cr, color);
	iv_glyph_paint(cr, g, size / 2.0, size / 2.0, size);
	cairo_destroy(cr);
	return s;
}
