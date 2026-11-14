/* LikeOS installer -- keyboards, the computer and the disk.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "art-private.h"

/* ---- keyboard ------------------------------------------------------ */

/* One keyboard in the rectangle (x, y, w): the height follows the width.
 * hl is the key lit in the accent colour (-1: none); tint washes the whole
 * board in the accent (for the one behind). */
static void keyboard(cairo_t *cr, double x, double y, double w, int hl,
		     double tint)
{
	const double h = w * 0.46;
	const double pad = w * 0.045, gap = w * 0.012;
	const int cols = 12;
	double key = (w - 2 * pad - (cols - 1) * gap) / cols;
	double kh = (h - 2 * pad - 3 * gap) / 4;
	cairo_pattern_t *p;
	int n = 0;

	art_shadow(cr, x, y, w, h, w * 0.05, w * 0.06, 0.22);
	p = art_linear(x, y, x, y + h, 0xfdfeff, 0xe4e9f2);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, x, y, w, h, w * 0.05);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	cairo_set_line_width(cr, w * 0.004);
	art_set_rgb(cr, 0xc6cfdd, 1);
	cairo_stroke(cr);

	for (int row = 0; row < 4; row++) {
		double ky = y + pad + row * (kh + gap);
		double kx = x + pad;
		/* key widths of the row, in key units */
		static const double r0[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
		static const double r1[] = { 1.5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.5 };
		static const double r2[] = { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.3 };
		static const double r3[] = { 1.3, 1.3, 1.3, 6.2, 1.3, 1.3 };
		const double *widths;
		int nk;

		switch (row) {
		case 0: widths = r0; nk = 12; break;
		case 1: widths = r1; nk = 11; break;
		case 2: widths = r2; nk = 11; break;
		default: widths = r3; nk = 6; break;
		}
		/* scale the row so it ends at the right edge */
		double units = 0;

		for (int i = 0; i < nk; i++)
			units += widths[i];
		double unit = (w - 2 * pad - (nk - 1) * gap) / units;

		for (int i = 0; i < nk; i++, n++) {
			double kw = widths[i] * unit;
			gboolean lit = n == hl;

			(void)key;
			/* the key's lower edge */
			art_set_rgb(cr, lit ? ART_ACCENT_DEEP : 0xcbd3e0, 1);
			art_rounded_rect(cr, kx, ky + kh * 0.08, kw, kh, kh * 0.22);
			cairo_fill(cr);
			if (lit) {
				p = art_linear(kx, ky, kx, ky + kh, ART_ACCENT_LIGHT,
					       ART_ACCENT);
				cairo_set_source(cr, p);
			} else {
				p = NULL;
				art_set_rgb(cr, 0xf7f9fc, 1);
			}
			art_rounded_rect(cr, kx, ky, kw, kh * 0.92, kh * 0.22);
			cairo_fill(cr);
			if (p)
				cairo_pattern_destroy(p);
			if (lit) {
				/* a small glyph on the lit key */
				art_set_rgb(cr, ART_WHITE, 1);
				cairo_set_line_width(cr, w * 0.006);
				cairo_move_to(cr, kx + kw * 0.32, ky + kh * 0.66);
				cairo_line_to(cr, kx + kw * 0.5, ky + kh * 0.24);
				cairo_line_to(cr, kx + kw * 0.68, ky + kh * 0.66);
				cairo_move_to(cr, kx + kw * 0.39, ky + kh * 0.5);
				cairo_line_to(cr, kx + kw * 0.61, ky + kh * 0.5);
				cairo_stroke(cr);
			}
			kx += kw + gap;
		}
	}
	if (tint > 0) {
		art_set_rgb(cr, ART_ACCENT_LIGHT, tint);
		cairo_set_operator(cr, CAIRO_OPERATOR_ATOP);
		art_rounded_rect(cr, x - 1, y - 1, w + 2, h + 2, w * 0.05);
		cairo_fill(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
}

void art_paint_keyboard(cairo_t *cr, double phase)
{
	/* The lit key wanders over the home row and back. */
	static const int path[] = { 13, 14, 15, 16, 17, 18, 19, 20 };
	int step = (int)(phase * 8) % 8;

	cairo_translate(cr, 0, art_bob(phase, 0.006, 0));
	keyboard(cr, 0.08, 0.32, 0.84, path[step], 0);
}

void art_paint_keyboard_plus(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	const double bx = 0.80, by = 0.70, br = 0.085;

	cairo_save(cr);
	cairo_translate(cr, 0, art_bob(phase, 0.006, 0.4));
	cairo_push_group(cr);
	keyboard(cr, 0.22, 0.18, 0.66, -1, 0.25);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, 0.75);
	cairo_restore(cr);

	cairo_save(cr);
	cairo_translate(cr, 0, art_bob(phase, 0.006, 0));
	keyboard(cr, 0.08, 0.40, 0.70, 15, 0);
	cairo_restore(cr);

	art_shadow_circle(cr, bx, by, br, 0.04, 0.25);
	p = art_linear(bx - br, by - br, bx + br, by + br, ART_TEAL_LIGHT, ART_TEAL);
	cairo_set_source(cr, p);
	cairo_arc(cr, bx, by, br, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	art_set_rgb(cr, ART_WHITE, 1);
	cairo_set_line_width(cr, 0.022);
	cairo_move_to(cr, bx - br * 0.45, by);
	cairo_line_to(cr, bx + br * 0.45, by);
	cairo_move_to(cr, bx, by - br * 0.45);
	cairo_line_to(cr, bx, by + br * 0.45);
	cairo_stroke(cr);
}

/* ---- computer ------------------------------------------------------ */

void art_paint_computer(cairo_t *cr, double phase)
{
	cairo_pattern_t *p;
	const double mx = 0.10, my = 0.20, mw = 0.74, mh = 0.48;
	const double inset = 0.022;
	gboolean cursor_on = fmod(phase * 12, 2.0) < 1.2;

	cairo_translate(cr, 0, art_bob(phase, 0.005, 0));

	/* stand */
	p = art_linear(0.40, 0.66, 0.40, 0.80, 0xb7c1d3, 0x8e9ab0);
	cairo_set_source(cr, p);
	cairo_move_to(cr, 0.42, 0.66);
	cairo_line_to(cr, 0.52, 0.66);
	cairo_line_to(cr, 0.545, 0.785);
	cairo_line_to(cr, 0.395, 0.785);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	art_shadow(cr, 0.29, 0.78, 0.36, 0.035, 0.017, 0.03, 0.18);
	p = art_linear(0.29, 0.78, 0.29, 0.815, 0xd9dfe9, 0xaeb8c9);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, 0.29, 0.78, 0.36, 0.035, 0.017);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	/* monitor */
	art_shadow(cr, mx, my, mw, mh, 0.035, 0.05, 0.28);
	p = art_linear(mx, my, mx, my + mh, 0x323c52, 0x1c2333);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, mx, my, mw, mh, 0.035);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	p = cairo_pattern_create_linear(mx, my, mx + mw, my + mh);
	cairo_pattern_add_color_stop_rgb(p, 0, 0xe9 / 255.0, 0xf1 / 255.0, 1);
	cairo_pattern_add_color_stop_rgb(p, 0.6, 0xc9 / 255.0, 0xdb / 255.0,
					 0xfd / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1, 0xb5 / 255.0, 0xc6 / 255.0,
					 0xf6 / 255.0);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, mx + inset, my + inset, mw - 2 * inset,
			 mh - 2 * inset, 0.018);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	/* soft waves across the screen */
	cairo_save(cr);
	art_rounded_rect(cr, mx + inset, my + inset, mw - 2 * inset,
			 mh - 2 * inset, 0.018);
	cairo_clip(cr);
	art_set_rgb(cr, ART_WHITE, 0.35);
	cairo_move_to(cr, mx, my + mh * 0.78);
	cairo_curve_to(cr, mx + mw * 0.3, my + mh * 0.6, mx + mw * 0.6,
		       my + mh * 0.98, mx + mw, my + mh * 0.70);
	cairo_line_to(cr, mx + mw, my + mh);
	cairo_line_to(cr, mx, my + mh);
	cairo_close_path(cr);
	cairo_fill(cr);
	art_set_rgb(cr, ART_ACCENT_LIGHT, 0.18);
	cairo_move_to(cr, mx, my + mh * 0.88);
	cairo_curve_to(cr, mx + mw * 0.35, my + mh * 0.74, mx + mw * 0.7,
		       my + mh * 1.02, mx + mw, my + mh * 0.82);
	cairo_line_to(cr, mx + mw, my + mh);
	cairo_line_to(cr, mx, my + mh);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_restore(cr);

	/* the name plate */
	{
		double nx = 0.20, ny = 0.36, nw = 0.46, nh = 0.15;

		art_shadow(cr, nx, ny, nw, nh, 0.025, 0.03, 0.20);
		art_set_rgb(cr, ART_WHITE, 1);
		art_rounded_rect(cr, nx, ny, nw, nh, 0.025);
		cairo_fill(cr);
		p = art_linear(nx + 0.03, ny, nx + 0.11, ny + nh, ART_ACCENT_LIGHT,
			       ART_ACCENT);
		cairo_set_source(cr, p);
		cairo_arc(cr, nx + 0.075, ny + nh / 2, 0.042, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		art_set_rgb(cr, ART_WHITE, 1);
		cairo_set_font_size(cr, 0.05);
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
				       CAIRO_FONT_WEIGHT_BOLD);
		cairo_move_to(cr, nx + 0.058, ny + nh / 2 + 0.018);
		cairo_show_text(cr, "#");
		art_set_rgb(cr, ART_ACCENT, 0.85);
		art_rounded_rect(cr, nx + 0.135, ny + 0.042, 0.19, 0.026, 0.013);
		cairo_fill(cr);
		art_set_rgb(cr, 0xc4cede, 1);
		art_rounded_rect(cr, nx + 0.135, ny + 0.085, 0.12, 0.020, 0.010);
		cairo_fill(cr);
		if (cursor_on) {
			art_set_rgb(cr, ART_ACCENT, 1);
			cairo_set_line_width(cr, 0.006);
			cairo_move_to(cr, nx + 0.338, ny + 0.035);
			cairo_line_to(cr, nx + 0.338, ny + 0.075);
			cairo_stroke(cr);
		}
	}

	/* a tag on a string at the monitor's corner */
	{
		double tx = 0.80, ty = 0.50;

		cairo_set_line_width(cr, 0.006);
		art_set_rgb(cr, 0x8e9ab0, 1);
		cairo_move_to(cr, mx + mw - 0.03, my + 0.06);
		cairo_curve_to(cr, 0.92, 0.28, 0.86, 0.40, tx + 0.02, ty + 0.03);
		cairo_stroke(cr);

		cairo_save(cr);
		cairo_translate(cr, tx, ty);
		cairo_rotate(cr, 0.32 + art_bob(phase, 0.04, 0.1));
		art_shadow(cr, -0.06, 0.0, 0.13, 0.18, 0.02, 0.03, 0.22);
		p = art_linear(-0.06, 0, 0.07, 0.18, ART_TEAL_LIGHT, ART_TEAL);
		cairo_set_source(cr, p);
		cairo_move_to(cr, 0.005, -0.02);
		cairo_line_to(cr, 0.07, 0.03);
		cairo_line_to(cr, 0.07, 0.165);
		cairo_arc(cr, 0.05, 0.165, 0.02, 0, M_PI / 2);
		cairo_line_to(cr, -0.04, 0.185);
		cairo_arc(cr, -0.04, 0.165, 0.02, M_PI / 2, M_PI);
		cairo_line_to(cr, -0.06, 0.03);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		art_set_rgb(cr, ART_WHITE, 1);
		cairo_arc(cr, 0.005, 0.03, 0.014, 0, 2 * M_PI);
		cairo_fill(cr);
		art_set_rgb(cr, ART_WHITE, 0.85);
		art_rounded_rect(cr, -0.035, 0.08, 0.08, 0.016, 0.008);
		cairo_fill(cr);
		art_rounded_rect(cr, -0.035, 0.115, 0.05, 0.016, 0.008);
		cairo_fill(cr);
		cairo_restore(cr);
	}
}

/* ---- disk ---------------------------------------------------------- */

static void drive(cairo_t *cr, double x, double y, double w, double h,
		  gboolean lit, double fill)
{
	cairo_pattern_t *p;
	double bx = x + w * 0.09, by = y + h * 0.40, bw = w * 0.82, bh = h * 0.22;
	static const guint32 seg_col[] = { ART_ACCENT, ART_TEAL, ART_VIOLET };
	static const double seg_w[] = { 0.40, 0.26, 0.14 };
	double sx = bx;

	art_shadow(cr, x, y, w, h, h * 0.18, h * 0.22, 0.25);
	p = art_linear(x, y, x, y + h, 0x3a4560, 0x1d2435);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, x, y, w, h, h * 0.18);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	/* top bevel */
	p = art_linear(x, y, x, y + h * 0.3, 0x5a6684, 0x3a4560);
	cairo_set_source(cr, p);
	art_rounded_rect(cr, x + h * 0.04, y + h * 0.04, w - h * 0.08, h * 0.22,
			 h * 0.12);
	cairo_fill(cr);
	cairo_pattern_destroy(p);

	/* the partition bar */
	art_set_rgb(cr, 0x11161f, 1);
	art_rounded_rect(cr, bx - 0.004, by - 0.004, bw + 0.008, bh + 0.008,
			 bh * 0.3);
	cairo_fill(cr);
	cairo_save(cr);
	art_rounded_rect(cr, bx, by, bw, bh, bh * 0.28);
	cairo_clip(cr);
	art_set_rgb(cr, 0x55607a, 1);
	cairo_paint(cr);
	for (int i = 0; i < 3; i++) {
		double sw = bw * seg_w[i] * fill;

		p = art_linear(sx, by, sx, by + bh, seg_col[i] | 0x101010,
			       seg_col[i]);
		cairo_set_source(cr, p);
		cairo_rectangle(cr, sx, by, MAX(0, sw - 0.004), bh);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		sx += sw;
	}
	cairo_restore(cr);

	/* status light and screws */
	art_set_rgb(cr, lit ? 0x3ce07a : 0x2a8f50, 1);
	cairo_arc(cr, x + w * 0.88, y + h * 0.78, h * 0.05, 0, 2 * M_PI);
	cairo_fill(cr);
	art_set_rgb(cr, 0x6b7896, 1);
	cairo_arc(cr, x + w * 0.06, y + h * 0.78, h * 0.035, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_arc(cr, x + w * 0.12, y + h * 0.78, h * 0.035, 0, 2 * M_PI);
	cairo_fill(cr);
}

void art_paint_disk(cairo_t *cr, double phase)
{
	double f = 0.55 + 0.45 * art_ease(fmod(phase * 2, 1.0));
	gboolean lit = fmod(phase * 10, 1.0) < 0.5;

	cairo_save(cr);
	cairo_translate(cr, 0, art_bob(phase, 0.006, 0.3));
	cairo_push_group(cr);
	drive(cr, 0.18, 0.24, 0.68, 0.22, FALSE, 1.0);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, 0.55);
	cairo_restore(cr);

	cairo_translate(cr, 0, art_bob(phase, 0.006, 0));
	drive(cr, 0.10, 0.44, 0.80, 0.26, lit, f);
}
