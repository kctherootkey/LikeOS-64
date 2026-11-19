/* LikeOS installer -- the LikeOS mark and the globe of the region page.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

void art_paint_logo(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;

	(void)phase;
	/* A faint shadow under the mark. */
	cairo_save(cr);
	cairo_translate(cr, 0.01, 0.025);
	cairo_set_line_width(cr, 0.15);
	cairo_set_source_rgba(cr, 0.10, 0.18, 0.45, 0.10);
	cairo_move_to(cr, 0.205, 0.19);
	cairo_line_to(cr, 0.205, 0.74);
	cairo_line_to(cr, 0.78, 0.74);
	cairo_stroke(cr);
	cairo_arc(cr, 0.60, 0.385, 0.19, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_restore(cr);

	/* The L: one thick stroke with round ends. */
	p = art_linear(0.15, 0.15, 0.75, 0.80, 0x4373ec, 0x2b50cf);
	cairo_set_source(cr, p);
	cairo_set_line_width(cr, 0.13);
	cairo_move_to(cr, 0.205, 0.18);
	cairo_line_to(cr, 0.205, 0.735);
	cairo_line_to(cr, 0.78, 0.735);
	cairo_stroke(cr);
	cairo_pattern_destroy(p);

	/* The O: a ring, lighter towards its top right. */
	p = art_linear(0.78, 0.20, 0.44, 0.56, 0x6cc9fb, 0x2a7cf0);
	cairo_set_source(cr, p);
	cairo_set_line_width(cr, 0.115);
	cairo_arc(cr, 0.60, 0.375, 0.185, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_pattern_destroy(p);
}

/* An irregular island from a list of points, smoothed into curves. */
static void island(cairo_t *cr, const double *pts, int n)
{
	double mx, my;

	mx = (pts[0] + pts[2 * (n - 1)]) / 2;
	my = (pts[1] + pts[2 * (n - 1) + 1]) / 2;
	cairo_move_to(cr, mx, my);
	for (int i = 0; i < n; i++) {
		double x = pts[2 * i], y = pts[2 * i + 1];
		double nx = pts[2 * ((i + 1) % n)], ny = pts[2 * ((i + 1) % n) + 1];

		cairo_curve_to(cr, x, y, x, y, (x + nx) / 2, (y + ny) / 2);
	}
	cairo_close_path(cr);
}

static void orbit_point(double angle, double *x, double *y)
{
	const double cx = 0.46, cy = 0.50, rx = 0.43, ry = 0.12;
	const double rot = -0.32;
	double ex = cos(angle) * rx, ey = sin(angle) * ry;

	*x = cx + ex * cos(rot) - ey * sin(rot);
	*y = cy + ex * sin(rot) + ey * cos(rot);
}

static void orbit_half(cairo_t *cr, gboolean front)
{
	cairo_save(cr);
	cairo_translate(cr, 0.46, 0.50);
	cairo_rotate(cr, -0.32);
	cairo_scale(cr, 0.43, 0.12);
	if (front)
		cairo_arc(cr, 0, 0, 1, 0, M_PI);
	else
		cairo_arc(cr, 0, 0, 1, M_PI, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_line_width(cr, 0.012);
	art_set_rgb(cr, ART_ACCENT_LIGHT, front ? 0.85 : 0.35);
	cairo_stroke(cr);
}

static void satellite(cairo_t *cr, double phase, gboolean front)
{
	double a = phase * 2 * M_PI + 0.6, x, y;

	if ((sin(a) >= 0) != front)
		return;
	orbit_point(a, &x, &y);
	art_shadow_circle(cr, x, y, 0.025, 0.015, 0.15);
	art_set_rgb(cr, ART_WHITE, 1);
	cairo_arc(cr, x, y, 0.026, 0, 2 * M_PI);
	cairo_fill(cr);
	art_set_rgb(cr, ART_TEAL, 1);
	cairo_arc(cr, x, y, 0.014, 0, 2 * M_PI);
	cairo_fill(cr);
}

void art_paint_globe(cairo_t *cr, double phase)
{
	const double cx = 0.46, cy = 0.50, r = 0.28;
	double bob = art_bob(phase, 0.008, 0);
	cairo_pattern_t *p;
	static const double land1[] = {
		0.30, 0.34, 0.38, 0.28, 0.47, 0.31, 0.50, 0.38, 0.45, 0.44,
		0.40, 0.47, 0.37, 0.55, 0.31, 0.52, 0.27, 0.43,
	};
	static const double land2[] = {
		0.52, 0.50, 0.60, 0.47, 0.68, 0.52, 0.70, 0.60, 0.64, 0.68,
		0.57, 0.71, 0.53, 0.63,
	};
	static const double land3[] = {
		0.56, 0.27, 0.62, 0.25, 0.68, 0.30, 0.64, 0.36, 0.58, 0.34,
	};
	static const double land4[] = {
		0.33, 0.64, 0.40, 0.62, 0.45, 0.68, 0.42, 0.75, 0.36, 0.74,
	};

	cairo_translate(cr, 0, bob);

	art_shadow_circle(cr, cx, cy + 0.02, r, 0.06, 0.18);
	orbit_half(cr, FALSE);
	satellite(cr, phase, FALSE);

	/* The sphere. */
	p = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
	cairo_pattern_add_color_stop_rgb(p, 0.0, 0x8f / 255.0, 0xc8 / 255.0, 1.0);
	cairo_pattern_add_color_stop_rgb(p, 0.45, 0x3b / 255.0, 0x74 / 255.0,
					 0xf0 / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1.0, 0x55 / 255.0, 0x45 / 255.0,
					 0xd8 / 255.0);
	cairo_set_source(cr, p);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	cairo_save(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_clip(cr);

	/* Land, drifting slowly eastwards. */
	cairo_save(cr);
	cairo_translate(cr, art_bob(phase, 0.015, 0.25), 0);
	art_set_rgb(cr, ART_WHITE, 0.30);
	island(cr, land1, 9);
	cairo_fill(cr);
	island(cr, land2, 7);
	cairo_fill(cr);
	art_set_rgb(cr, ART_WHITE, 0.22);
	island(cr, land3, 5);
	cairo_fill(cr);
	island(cr, land4, 5);
	cairo_fill(cr);
	cairo_restore(cr);

	/* Meridians and parallels. */
	cairo_set_line_width(cr, 0.004);
	art_set_rgb(cr, ART_WHITE, 0.22);
	for (int i = 1; i <= 3; i++) {
		double rx = r * i / 3.5;

		cairo_save(cr);
		cairo_translate(cr, cx, cy);
		cairo_scale(cr, rx, r);
		cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
	}
	for (int i = -2; i <= 2; i++) {
		double y = cy + r * i / 3.0;
		double half = sqrt(MAX(0, r * r - (y - cy) * (y - cy)));

		cairo_move_to(cr, cx - half, y);
		cairo_line_to(cr, cx + half, y);
		cairo_stroke(cr);
	}

	/* Light from the upper left, shade at the lower right. */
	p = cairo_pattern_create_radial(cx - r * 0.45, cy - r * 0.5, 0, cx - r * 0.3,
					cy - r * 0.3, r * 1.2);
	cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.38);
	cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.05);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.05, 0.05, 0.2, 0.18);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	cairo_restore(cr);

	/* A rim of light. */
	cairo_set_line_width(cr, 0.006);
	cairo_arc(cr, cx, cy, r - 0.003, 0, 2 * M_PI);
	art_set_rgb(cr, ART_WHITE, 0.35);
	cairo_stroke(cr);

	/* A location pin on the land. */
	{
		double px = 0.405, py = 0.405;

		art_shadow_circle(cr, px, py + 0.02, 0.03, 0.02, 0.2);
		cairo_move_to(cr, px, py + 0.07);
		cairo_curve_to(cr, px - 0.02, py + 0.04, px - 0.045, py + 0.02,
			       px - 0.045, py - 0.005);
		cairo_arc(cr, px, py - 0.005, 0.045, M_PI, 2 * M_PI);
		cairo_curve_to(cr, px + 0.045, py + 0.02, px + 0.02, py + 0.04, px,
			       py + 0.07);
		cairo_close_path(cr);
		p = art_linear(px, py - 0.05, px, py + 0.07, ART_TEAL_LIGHT, ART_TEAL);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		art_set_rgb(cr, ART_WHITE, 1);
		cairo_arc(cr, px, py - 0.005, 0.017, 0, 2 * M_PI);
		cairo_fill(cr);
	}

	orbit_half(cr, TRUE);
	satellite(cr, phase, TRUE);

	/* The clock badge. */
	{
		double bx = 0.745, by = 0.715, br = 0.125;
		double sec = phase * 2 * M_PI;

		art_shadow_circle(cr, bx, by, br, 0.05, 0.22);
		p = art_linear(bx, by - br, bx, by + br, ART_WHITE, 0xeef3fb);
		cairo_set_source(cr, p);
		cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		cairo_set_line_width(cr, 0.012);
		art_set_rgb(cr, ART_ACCENT_LIGHT, 0.9);
		cairo_arc(cr, bx, by, br - 0.012, 0, 2 * M_PI);
		cairo_stroke(cr);
		/* hour marks */
		cairo_set_line_width(cr, 0.008);
		art_set_rgb(cr, ART_INK, 0.45);
		for (int i = 0; i < 12; i++) {
			double a = i * M_PI / 6;
			double r0 = br * (i % 3 == 0 ? 0.62 : 0.70), r1 = br * 0.76;

			cairo_move_to(cr, bx + cos(a) * r0, by + sin(a) * r0);
			cairo_line_to(cr, bx + cos(a) * r1, by + sin(a) * r1);
			cairo_stroke(cr);
		}
		/* hands at ten past ten, the second hand sweeping */
		art_set_rgb(cr, ART_INK, 0.85);
		cairo_set_line_width(cr, 0.014);
		cairo_move_to(cr, bx, by);
		cairo_line_to(cr, bx + cos(-M_PI * 0.83) * br * 0.42,
			      by + sin(-M_PI * 0.83) * br * 0.42);
		cairo_stroke(cr);
		cairo_set_line_width(cr, 0.010);
		cairo_move_to(cr, bx, by);
		cairo_line_to(cr, bx + cos(-M_PI * 0.17) * br * 0.58,
			      by + sin(-M_PI * 0.17) * br * 0.58);
		cairo_stroke(cr);
		art_set_rgb(cr, ART_ACCENT, 1);
		cairo_set_line_width(cr, 0.005);
		cairo_move_to(cr, bx, by);
		cairo_line_to(cr, bx + cos(sec - M_PI / 2) * br * 0.66,
			      by + sin(sec - M_PI / 2) * br * 0.66);
		cairo_stroke(cr);
		cairo_arc(cr, bx, by, 0.012, 0, 2 * M_PI);
		cairo_fill(cr);
	}
}
