/* LikeOS installer -- people, the key and the shield.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

static void sparkle(cairo_t *cr, double x, double y, double r, guint32 col,
		    double alpha)
{
	art_set_rgb(cr, col, alpha);
	cairo_move_to(cr, x, y - r);
	cairo_curve_to(cr, x + r * 0.15, y - r * 0.15, x + r * 0.15, y - r * 0.15,
		       x + r, y);
	cairo_curve_to(cr, x + r * 0.15, y + r * 0.15, x + r * 0.15, y + r * 0.15,
		       x, y + r);
	cairo_curve_to(cr, x - r * 0.15, y + r * 0.15, x - r * 0.15, y + r * 0.15,
		       x - r, y);
	cairo_curve_to(cr, x - r * 0.15, y - r * 0.15, x - r * 0.15, y - r * 0.15,
		       x, y - r);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void badge_disc(cairo_t *cr, double cx, double cy, double r, guint32 c0,
		       guint32 c1)
{
	cairo_pattern_t *p;

	art_shadow_circle(cr, cx, cy, r, r * 0.45, 0.22);
	p = art_linear(cx - r, cy - r, cx + r, cy + r, c0, c1);
	cairo_set_source(cr, p);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
}

void art_paint_avatar(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double b1 = art_bob(phase, 0.008, 0), b2 = art_bob(phase, 0.008, 0.5);

	/* small accent dots around the group */
	art_set_rgb(cr, ART_ACCENT_LIGHT, 0.35);
	cairo_arc(cr, 0.22, 0.22 + b2, 0.035, 0, 2 * M_PI);
	cairo_fill(cr);
	art_set_rgb(cr, ART_TEAL_LIGHT, 0.55);
	cairo_arc(cr, 0.86, 0.62 + b1, 0.025, 0, 2 * M_PI);
	cairo_fill(cr);

	/* the second person, behind */
	badge_disc(cr, 0.64, 0.38 + b2, 0.19, 0xeef1f6, 0xd2d8e3);
	art_person(cr, 0.64, 0.40 + b2, 0.19, ART_WHITE, 0.95);

	/* the first person */
	art_shadow_circle(cr, 0.42, 0.57 + b1, 0.25, 0.07, 0.25);
	p = cairo_pattern_create_linear(0.17, 0.32, 0.67, 0.82);
	cairo_pattern_add_color_stop_rgb(p, 0, 0x6c / 255.0, 0xa6 / 255.0, 1);
	cairo_pattern_add_color_stop_rgb(p, 0.5, 0x2a / 255.0, 0x5b / 255.0,
					 0xd7 / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1, 0x6a / 255.0, 0x5a / 255.0,
					 0xe0 / 255.0);
	cairo_set_source(cr, p);
	cairo_arc(cr, 0.42, 0.57 + b1, 0.25, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	art_person(cr, 0.42, 0.59 + b1, 0.25, ART_WHITE, 0.55);
	cairo_set_line_width(cr, 0.006);
	art_set_rgb(cr, ART_WHITE, 0.4);
	cairo_arc(cr, 0.42, 0.57 + b1, 0.245, M_PI * 1.05, M_PI * 1.6);
	cairo_stroke(cr);

	/* a plus: someone is being added */
	badge_disc(cr, 0.62, 0.76 + b1, 0.075, ART_TEAL_LIGHT, ART_TEAL);
	art_set_rgb(cr, ART_WHITE, 1);
	cairo_set_line_width(cr, 0.018);
	cairo_move_to(cr, 0.62 - 0.032, 0.76 + b1);
	cairo_line_to(cr, 0.62 + 0.032, 0.76 + b1);
	cairo_move_to(cr, 0.62, 0.76 - 0.032 + b1);
	cairo_line_to(cr, 0.62, 0.76 + 0.032 + b1);
	cairo_stroke(cr);
}

static void key_shape(cairo_t *cr, double x, double y, double len, double angle)
{
	cairo_pattern_t *p;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_rotate(cr, angle);
	p = art_linear(-len * 0.3, -len * 0.2, len, len * 0.2, 0xffd36b, 0xe39410);
	cairo_set_source(cr, p);
	/* bow */
	cairo_arc(cr, 0, 0, len * 0.22, 0, 2 * M_PI);
	cairo_new_sub_path(cr);
	cairo_arc_negative(cr, 0, 0, len * 0.09, 2 * M_PI, 0);
	cairo_fill(cr);
	/* shaft and bits */
	art_rounded_rect(cr, len * 0.18, -len * 0.045, len * 0.82, len * 0.09,
			 len * 0.03);
	cairo_fill(cr);
	cairo_rectangle(cr, len * 0.72, 0, len * 0.08, len * 0.16);
	cairo_rectangle(cr, len * 0.86, 0, len * 0.08, len * 0.12);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	cairo_restore(cr);
}

void art_paint_avatar_key(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double b = art_bob(phase, 0.008, 0);

	art_shadow_circle(cr, 0.42, 0.46 + b, 0.27, 0.07, 0.25);
	p = art_linear(0.15, 0.2, 0.69, 0.73, ART_TEAL_LIGHT, ART_ACCENT);
	cairo_set_source(cr, p);
	cairo_arc(cr, 0.42, 0.46 + b, 0.27, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	art_person(cr, 0.42, 0.48 + b, 0.27, ART_WHITE, 0.6);

	badge_disc(cr, 0.70, 0.72 + b * 0.5, 0.14, ART_WHITE, 0xe9eef7);
	key_shape(cr, 0.64, 0.68 + b * 0.5, 0.17, 0.75);

	sparkle(cr, 0.80, 0.24 + b, 0.04, ART_ACCENT_LIGHT, 0.7);
	sparkle(cr, 0.15, 0.78 - b, 0.028, ART_TEAL, 0.6);
}

void art_paint_shield(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double b = art_bob(phase, 0.008, 0);

	cairo_translate(cr, 0, b);

	/* the shield's outline */
	cairo_new_path(cr);
	cairo_move_to(cr, 0.5, 0.13);
	cairo_curve_to(cr, 0.60, 0.20, 0.70, 0.22, 0.79, 0.22);
	cairo_line_to(cr, 0.79, 0.47);
	cairo_curve_to(cr, 0.79, 0.67, 0.66, 0.80, 0.5, 0.88);
	cairo_curve_to(cr, 0.34, 0.80, 0.21, 0.67, 0.21, 0.47);
	cairo_line_to(cr, 0.21, 0.22);
	cairo_curve_to(cr, 0.30, 0.22, 0.40, 0.20, 0.5, 0.13);
	cairo_close_path(cr);
	{
		cairo_path_t *shape = cairo_copy_path(cr);

		cairo_new_path(cr);
		/* shadow: the shape, offset and faded */
		for (int i = 6; i >= 1; i--) {
			cairo_save(cr);
			cairo_translate(cr, 0.5, 0.52 + 0.012 * i);
			cairo_scale(cr, 1 + 0.012 * i, 1 + 0.012 * i);
			cairo_translate(cr, -0.5, -0.52);
			cairo_append_path(cr, shape);
			cairo_set_source_rgba(cr, 0.08, 0.12, 0.25, 0.035);
			cairo_fill(cr);
			cairo_restore(cr);
		}
		p = cairo_pattern_create_linear(0.21, 0.13, 0.79, 0.88);
		cairo_pattern_add_color_stop_rgb(p, 0, 0x6c / 255.0, 0xa6 / 255.0, 1);
		cairo_pattern_add_color_stop_rgb(p, 0.55, 0x2a / 255.0,
						 0x5b / 255.0, 0xd7 / 255.0);
		cairo_pattern_add_color_stop_rgb(p, 1, 0x1a / 255.0, 0x3f / 255.0,
						 0x9e / 255.0);
		cairo_append_path(cr, shape);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);

		/* the lit left half */
		cairo_save(cr);
		cairo_append_path(cr, shape);
		cairo_clip(cr);
		art_set_rgb(cr, ART_WHITE, 0.12);
		cairo_rectangle(cr, 0.0, 0.0, 0.5, 1.0);
		cairo_fill(cr);
		cairo_restore(cr);

		cairo_append_path(cr, shape);
		cairo_set_line_width(cr, 0.008);
		art_set_rgb(cr, ART_WHITE, 0.35);
		cairo_stroke(cr);
		cairo_path_destroy(shape);
	}

	/* the padlock */
	art_set_rgb(cr, ART_WHITE, 1);
	cairo_set_line_width(cr, 0.04);
	cairo_move_to(cr, 0.43, 0.50);
	cairo_line_to(cr, 0.43, 0.43);
	cairo_arc(cr, 0.5, 0.43, 0.07, M_PI, 2 * M_PI);
	cairo_line_to(cr, 0.57, 0.50);
	cairo_stroke(cr);
	art_rounded_rect(cr, 0.375, 0.48, 0.25, 0.19, 0.035);
	cairo_fill(cr);
	art_set_rgb(cr, ART_ACCENT_DEEP, 1);
	cairo_arc(cr, 0.5, 0.555, 0.026, 0, 2 * M_PI);
	cairo_fill(cr);
	art_rounded_rect(cr, 0.488, 0.565, 0.024, 0.06, 0.01);
	cairo_fill(cr);

	sparkle(cr, 0.86, 0.17, 0.045, ART_TEAL, 0.8);
	sparkle(cr, 0.13, 0.70, 0.03, ART_ACCENT_LIGHT, 0.7);
	sparkle(cr, 0.84, 0.74, 0.022, ART_VIOLET, 0.5);
}
