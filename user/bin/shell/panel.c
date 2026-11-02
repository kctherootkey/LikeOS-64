// LikeOS -- shared drawing for the desktop panel and its widgets.
//
// See panel.h for what this is and why it exists: the clock and the load
// monitor are drawn by these functions whether they are running as their own
// programs or as areas inside the taskbar, so the two cannot drift apart.
//
// Copyright (C) 2026 The LikeOS Project

#include "panel.h"

#include <math.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>

void panel_color(cairo_t *cr, unsigned int rgb)
{
	cairo_set_source_rgb(cr, ((rgb >> 16) & 0xff) / 255.0,
			     ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
}

void panel_color_a(cairo_t *cr, unsigned int rgb, double a)
{
	cairo_set_source_rgba(cr, ((rgb >> 16) & 0xff) / 255.0,
			      ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0,
			      a);
}

void panel_fill_face(cairo_t *cr, double x, double y, double w, double h)
{
	panel_color(cr, PANEL_BG);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

void panel_round_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r)
{
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* The shell's hover and pressed fills: white at 7% under the pointer, 12%
 * for a lit control, and in between while held -- flat, no outline, so a
 * control is a shape of light on the panel and nothing more. */
void panel_tile(cairo_t *cr, double x, double y, double w, double h,
		enum panel_tile state)
{
	double a;

	switch (state) {
	case PANEL_TILE_HOVER:
		a = 0.07;
		break;
	case PANEL_TILE_LIT:
		a = 0.12;
		break;
	case PANEL_TILE_DOWN:
		a = 0.10;
		break;
	default:
		return;
	}
	cairo_save(cr);
	panel_round_rect(cr, x, y, w, h, PANEL_TILE_RADIUS);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_fill(cr);
	cairo_restore(cr);
}

void panel_dots(cairo_t *cr, double cx, double cy, int count,
		gboolean active)
{
	const double d = 4, gap = 3;

	if (count <= 0)
		return;
	if (count > 3)
		count = 3;

	double x0 = cx - (count * d + (count - 1) * gap) / 2 + d / 2;

	cairo_save(cr);
	panel_color(cr, active ? PANEL_ACCENT : PANEL_TEXT_DIM);
	for (int i = 0; i < count; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, x0 + i * (d + gap), cy, d / 2, 0, 2 * M_PI);
	}
	cairo_fill(cr);
	cairo_restore(cr);
}

void panel_orb(cairo_t *cr, double cx, double cy, double r, double glow,
	       gboolean pressed)
{
	cairo_pattern_t *p;
	double k = pressed ? 0.7 : 1.0 + 0.18 * glow;

	cairo_save(cr);

	/* The halo, just outside the rim. */
	if (glow > 0) {
		p = cairo_pattern_create_radial(cx, cy, r * 0.9, cx, cy, r + 3);
		cairo_pattern_add_color_stop_rgba(p, 0, 0.49, 0.77, 1, 0.6 * glow);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.37, 0.66, 1, 0);
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, r + 3, 0, 2 * M_PI);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}

	/* The rim, and the glass inside it: light pooled at the bottom,
	 * deep blue at the edge. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 0.03, 0.09, 0.22);
	cairo_fill(cr);
	p = cairo_pattern_create_radial(cx, cy + r * 0.9, 0, cx, cy + r * 0.6,
					r * 1.3);
	cairo_pattern_add_color_stop_rgb(p, 0, MIN(1, 0.5 * k), MIN(1, 0.88 * k),
					 1);
	cairo_pattern_add_color_stop_rgb(p, 0.32, 0.23 * k, 0.53 * k,
					 MIN(1, 0.94 * k));
	cairo_pattern_add_color_stop_rgb(p, 0.66, 0.14 * k, 0.31 * k, 0.78 * k);
	cairo_pattern_add_color_stop_rgb(p, 1, 0.05 * k, 0.13 * k, 0.42 * k);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r - 1, 0, 2 * M_PI);
	cairo_set_source(cr, p);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	cairo_clip(cr);

	/* Pressed in: a shadow along the top inside the rim. */
	if (pressed) {
		p = cairo_pattern_create_linear(0, cy - r, 0, cy);
		cairo_pattern_add_color_stop_rgba(p, 0, 0.01, 0.03, 0.14, 0.55);
		cairo_pattern_add_color_stop_rgba(p, 1, 0.01, 0.03, 0.14, 0);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
	}

	/* The gloss: a band of light over the top half, fading down. */
	double g = pressed ? 0.35 : 0.7 + 0.08 * glow;

	p = cairo_pattern_create_linear(0, cy - r, 0, cy);
	cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, g);
	cairo_pattern_add_color_stop_rgba(p, 0.6, 1, 1, 1, g * 0.35);
	cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.05);
	cairo_save(cr);
	cairo_translate(cr, cx, cy - r * 0.42);
	cairo_scale(cr, r * 0.86, r * 0.56);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source(cr, p);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	cairo_restore(cr);
}

/* A ring and a short handle at forty-five degrees, on the 16-unit grid with
 * the shell's 1.5-unit stroke. */
void panel_glyph_search(cairo_t *cr, double x, double y, double s)
{
	double k = s / 16.0;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	cairo_set_line_width(cr, 1.5);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 7, 7, 4.75, 0, 2 * M_PI);
	cairo_move_to(cr, 10.6, 10.6);
	cairo_line_to(cr, 14, 14);
	cairo_stroke(cr);
	cairo_restore(cr);
}

int panel_text_full(cairo_t *cr, double x, double cy, double w,
		    const char *text, const char *font, const double rgba[4],
		    int align)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);

	/* Ellipsise rather than clip: a label squeezed by its neighbours
	 * shows "Mozilla Fire..." instead of a title cut mid-glyph. */
	if (w > 0) {
		pango_layout_set_width(l, (int)w * PANGO_SCALE);
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	}
	pango_layout_get_pixel_size(l, &tw, &th);

	double tx = x;

	if (align == 1)
		tx = x + (w - tw) / 2;
	else if (align == 2)
		tx = x + w - tw;
	if (tx < x)
		tx = x;

	cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
	cairo_move_to(cr, floor(tx), floor(cy - th / 2.0));
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	return tw;
}

int panel_text_width(cairo_t *cr, const char *text, const char *font)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	int w, h;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	return w;
}

static int draw_text(cairo_t *cr, double x, double cy, double w,
		     const char *text, const char *font, int align,
		     unsigned int color)
{
	double c[4] = { ((color >> 16) & 0xff) / 255.0,
			((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0,
			1 };

	return panel_text_full(cr, x, cy, w, text, font, c, align);
}

int panel_text(cairo_t *cr, double x, double cy, double w, const char *text,
	       gboolean bold, int align)
{
	return draw_text(cr, x, cy, w, text, bold ? PANEL_FONT_BOLD : PANEL_FONT,
			 align, PANEL_TEXT);
}

/* ------------------------------------------------------------------------ */
/* Load and memory                                                           */
/* ------------------------------------------------------------------------ */

void panel_load_init(struct panel_load *m)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);

	memset(m, 0, sizeof(*m));
	m->ncpu = (n > 0) ? (int)n : 1;
}

/* Read one sample.
 *
 * loads[0] is the 1-minute average in 16.16 fixed point, which is exactly what
 * top(1) divides by 65536.0 to print; dividing again by the processor count
 * turns "runnable tasks" into "share of the machine".  Full height therefore
 * means every core busy, not one -- on a four-core machine the latter reads
 * alarmingly, filling most of the graph while the CPUs are nearly idle.
 *
 * Memory in use is total minus available rather than total minus free: free
 * counts only untouched pages, so a system with a warm page cache reports
 * itself nearly full when almost all of that is reclaimable.  `available' is
 * the kernel's own estimate of what a new allocation could get, and is the
 * number top's used figure is built from. */
void panel_load_sample(struct panel_load *m)
{
	struct sysinfo si;

	if (sysinfo(&si) != 0)
		return;

	double la = (double)si.loads[0] / 65536.0;
	double frac = la / (double)m->ncpu;

	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;

	if (m->nload < PANEL_LOAD_HISTORY) {
		m->load[m->nload++] = frac;
	} else {
		memmove(&m->load[0], &m->load[1],
			sizeof(m->load[0]) * (PANEL_LOAD_HISTORY - 1));
		m->load[PANEL_LOAD_HISTORY - 1] = frac;
	}

	if (si.totalram > 0) {
		unsigned long avail = si.available ? si.available : si.freeram;
		unsigned long used = si.totalram > avail ? si.totalram - avail :
							   0;

		m->mem = (double)used / (double)si.totalram;
	}
}

void panel_load_draw(cairo_t *cr, struct panel_load *m, double x, double y,
		     double w, double h)
{
	/* The picture: a raised card 32 x 24 with the load history as a
	 * curve, the newest sample at the right edge, over a fill that fades
	 * from the accent to nothing. */
	const double gw = 32, gh = 24;
	double gx = x + 8, gy = floor(y + (h - gh) / 2);
	double base = gy + gh - 4, top = gy + 4;

	cairo_save(cr);
	panel_round_rect(cr, gx, gy, gw, gh, 6);
	panel_color(cr, PANEL_RAISED);
	cairo_fill_preserve(cr);
	cairo_clip(cr);

	if (m->nload > 0) {
		double step = (gw - 6) / (double)(PANEL_LOAD_HISTORY - 1);
		double x0 = gx + 3 + (PANEL_LOAD_HISTORY - m->nload) * step;

		/* Anything non-zero gets a pixel of height: a load of 0.01 is
		 * not nothing, and a flat line looks like a broken graph. */
		for (int i = 0; i < m->nload; i++) {
			double v = m->load[i];
			double py = base - v * (base - top);

			if (v > 0 && base - py < 1)
				py = base - 1;
			if (i == 0)
				cairo_move_to(cr, x0, py);
			else
				cairo_line_to(cr, x0 + i * step, py);
		}

		cairo_path_t *line = cairo_copy_path(cr);
		cairo_pattern_t *g = cairo_pattern_create_linear(0, top, 0, base);

		cairo_line_to(cr, x0 + (m->nload - 1) * step, gy + gh);
		cairo_line_to(cr, x0, gy + gh);
		cairo_close_path(cr);
		cairo_pattern_add_color_stop_rgba(g, 0, 0x5b / 255.0,
						  0x8f / 255.0, 0xf9 / 255.0,
						  0.45);
		cairo_pattern_add_color_stop_rgba(g, 1, 0x5b / 255.0,
						  0x8f / 255.0, 0xf9 / 255.0,
						  0.05);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);

		cairo_append_path(cr, line);
		cairo_path_destroy(line);
		panel_color(cr, PANEL_ACCENT);
		cairo_set_line_width(cr, 1.5);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
	}
	cairo_restore(cr);

	/* The two lines beside it: processor load over memory in use. */
	char l1[32], l2[32];
	int cpu = m->nload ? (int)(m->load[m->nload - 1] * 100.0 + 0.5) : 0;

	snprintf(l1, sizeof(l1), "CPU %d%%", cpu);
	snprintf(l2, sizeof(l2), "RAM %d%%", (int)(m->mem * 100.0 + 0.5));

	double tx = gx + gw + 9;
	double tw = x + w - tx;

	draw_text(cr, tx, y + h / 2 - 8, tw, l1, PANEL_FONT, 0, PANEL_TEXT);
	draw_text(cr, tx, y + h / 2 + 8, tw, l2, PANEL_FONT, 0,
		  PANEL_TEXT_DIM);
}

/* ------------------------------------------------------------------------ */
/* Clock                                                                     */
/* ------------------------------------------------------------------------ */

/* The date as the panel shows it, "Mon 2 Nov", and the time, "14:32".  The
 * time is written out as %H:%M rather than taken from the locale: it sits in
 * a slot sized for five characters. */
static void clock_strings(char *timebuf, size_t tsz, char *datebuf, size_t dsz)
{
	GDateTime *now = g_date_time_new_now_local();
	char *date;

	timebuf[0] = datebuf[0] = '\0';
	if (!now)
		return;
	snprintf(timebuf, tsz, "%02d:%02d", g_date_time_get_hour(now),
		 g_date_time_get_minute(now));
	date = g_date_time_format(now, "%a %-d %b");
	if (date)
		g_strlcpy(datebuf, date, dsz);
	g_free(date);
	g_date_time_unref(now);
}

/* The gap between the date and the time on the one-line clock. */
#define CLOCK_GAP 10

void panel_clock_draw(cairo_t *cr, double x, double y, double w, double h,
		      gboolean stacked)
{
	static const double dim[4] = { 0xa4 / 255.0, 0xad / 255.0,
				       0xbf / 255.0, 1 };
	static const double text[4] = { 0xee / 255.0, 0xf2 / 255.0,
					 0xfa / 255.0, 1 };
	char timebuf[32], datebuf[64];

	clock_strings(timebuf, sizeof(timebuf), datebuf, sizeof(datebuf));

	if (stacked) {
		panel_text_full(cr, x, y + h / 2 - 9, w, timebuf,
				"Selawik Semibold 11", text, 1);
		panel_text_full(cr, x, y + h / 2 + 10, w, datebuf, PANEL_FONT,
				dim, 1);
		return;
	}

	/* One line, right-aligned in the rectangle: the time keeps its place
	 * at the right end, and the date stands to the left of it. */
	int tw = panel_text_width(cr, "88:88", PANEL_FONT_BOLD);
	double tx = x + w - tw;

	panel_text_full(cr, tx, y + h / 2, tw, timebuf, PANEL_FONT_BOLD, text,
			2);
	panel_text_full(cr, x, y + h / 2, tx - CLOCK_GAP - x, datebuf,
			PANEL_FONT, dim, 2);
}

int panel_clock_width(cairo_t *cr)
{
	char timebuf[32], datebuf[64];

	clock_strings(timebuf, sizeof(timebuf), datebuf, sizeof(datebuf));
	/* The date as it is today, the time at its widest digits: the digits
	 * are not all the same width in a proportional face, and a clock that
	 * changed width every minute would nudge the tray about. */
	return panel_text_width(cr, datebuf, PANEL_FONT) + CLOCK_GAP +
	       panel_text_width(cr, "88:88", PANEL_FONT_BOLD);
}
