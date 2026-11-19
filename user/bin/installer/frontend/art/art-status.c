/* LikeOS installer -- the check, the warning and the floating tiles.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

void art_paint_success(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double pulse = 0.5 + 0.5 * sin(phase * 2 * M_PI);

	/* a halo that breathes */
	art_set_rgb(cr, ART_SUCCESS_COL, 0.06 + 0.04 * pulse);
	cairo_arc(cr, 0.5, 0.5, 0.40 + 0.02 * pulse, 0, 2 * M_PI);
	cairo_fill(cr);
	art_set_rgb(cr, ART_SUCCESS_COL, 0.10);
	cairo_arc(cr, 0.5, 0.5, 0.345, 0, 2 * M_PI);
	cairo_fill(cr);

	art_shadow_circle(cr, 0.5, 0.5, 0.29, 0.07, 0.25);
	p = art_linear(0.25, 0.22, 0.75, 0.80, 0x5fe0a8, ART_SUCCESS_COL);
	cairo_set_source(cr, p);
	cairo_arc(cr, 0.5, 0.5, 0.29, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	cairo_set_line_width(cr, 0.006);
	art_set_rgb(cr, ART_WHITE, 0.45);
	cairo_arc(cr, 0.5, 0.5, 0.283, M_PI * 1.05, M_PI * 1.55);
	cairo_stroke(cr);

	art_set_rgb(cr, ART_WHITE, 1);
	cairo_set_line_width(cr, 0.055);
	cairo_move_to(cr, 0.385, 0.51);
	cairo_line_to(cr, 0.465, 0.59);
	cairo_line_to(cr, 0.625, 0.415);
	cairo_stroke(cr);
}

void art_paint_warning(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double b = art_bob(phase, 0.006, 0);

	cairo_translate(cr, 0, b);
	art_shadow(cr, 0.22, 0.30, 0.56, 0.46, 0.08, 0.06, 0.18);

	cairo_move_to(cr, 0.5, 0.20);
	cairo_line_to(cr, 0.82, 0.76);
	cairo_line_to(cr, 0.18, 0.76);
	cairo_close_path(cr);
	p = art_linear(0.5, 0.2, 0.5, 0.8, 0xffc457, 0xe08600);
	cairo_set_source(cr, p);
	cairo_set_line_width(cr, 0.09);
	cairo_fill_preserve(cr);
	cairo_stroke(cr);
	cairo_pattern_destroy(p);

	art_set_rgb(cr, ART_WHITE, 1);
	cairo_set_line_width(cr, 0.06);
	cairo_move_to(cr, 0.5, 0.40);
	cairo_line_to(cr, 0.5, 0.58);
	cairo_stroke(cr);
	cairo_arc(cr, 0.5, 0.685, 0.033, 0, 2 * M_PI);
	cairo_fill(cr);
}

void art_paint_shapes(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	double b1 = art_bob(phase, 0.014, 0.0);
	double b2 = art_bob(phase, 0.014, 0.33);
	double b3 = art_bob(phase, 0.014, 0.66);

	/* a violet chip */
	art_shadow(cr, 0.16, 0.66 + b3, 0.11, 0.11, 0.03, 0.03, 0.18);
	p = art_linear(0.16, 0.66, 0.27, 0.77, 0x9a8cff, ART_VIOLET);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, 0.16, 0.66 + b3, 0.11, 0.11, 0.03);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	/* the big accent tile, tilted */
	cairo_save(cr);
	cairo_translate(cr, 0.42, 0.46 + b1);
	cairo_rotate(cr, -0.18);
	art_shadow(cr, -0.18, -0.18, 0.36, 0.36, 0.07, 0.06, 0.28);
	p = art_linear(-0.18, -0.18, 0.18, 0.18, ART_ACCENT_LIGHT, ART_ACCENT_DEEP);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, -0.18, -0.18, 0.36, 0.36, 0.07);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	/* the orbit of the mark, on the tile */
	art_set_rgb(cr, ART_WHITE, 0.9);
	cairo_set_line_width(cr, 0.04);
	cairo_arc(cr, 0.03, -0.03, 0.075, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, -0.10, -0.10);
	cairo_line_to(cr, -0.10, 0.10);
	cairo_line_to(cr, 0.10, 0.10);
	cairo_stroke(cr);
	cairo_restore(cr);

	/* a teal disc */
	art_shadow_circle(cr, 0.66, 0.66 + b2, 0.13, 0.05, 0.22);
	p = art_linear(0.53, 0.53, 0.79, 0.79, ART_TEAL_LIGHT, ART_TEAL);
	cairo_set_source(cr, p);
	cairo_arc(cr, 0.66, 0.66 + b2, 0.13, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	/* a white card with two lines */
	art_shadow(cr, 0.58, 0.14 + b3, 0.26, 0.19, 0.03, 0.04, 0.2);
	art_set_rgb(cr, ART_WHITE, 1);
	art_rounded_rect(cr, 0.58, 0.14 + b3, 0.26, 0.19, 0.03);
	cairo_fill(cr);
	art_set_rgb(cr, ART_ACCENT, 0.8);
	art_rounded_rect(cr, 0.61, 0.19 + b3, 0.16, 0.024, 0.012);
	cairo_fill(cr);
	art_set_rgb(cr, 0xc9d1de, 1);
	art_rounded_rect(cr, 0.61, 0.235 + b3, 0.11, 0.02, 0.01);
	cairo_fill(cr);
	art_rounded_rect(cr, 0.61, 0.272 + b3, 0.18, 0.02, 0.01);
	cairo_fill(cr);
}
