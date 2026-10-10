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

void panel_fill_face(cairo_t *cr, double x, double y, double w, double h)
{
	panel_color(cr, PANEL_BG);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

void panel_round_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* A lit tile is the lighter fill with a slightly darker lower edge, which is
 * all the depth Windows 11 gives a taskbar button.  The edge is a second,
 * one-pixel-lower copy of the shape drawn first, so only its bottom shows. */
void panel_tile(cairo_t *cr, double x, double y, double w, double h,
		enum panel_tile state)
{
	if (state == PANEL_TILE_NONE)
		return;
	if (state == PANEL_TILE_LIT) {
		panel_color(cr, PANEL_TILE_EDGE);
		panel_round_rect(cr, x, y + 1, w, h, 4);
		cairo_fill(cr);
	}
	panel_color(cr, state == PANEL_TILE_DOWN ? PANEL_TILE_PRESSED :
						   PANEL_TILE_HOVER);
	panel_round_rect(cr, x, y, w, h, 4);
	cairo_fill(cr);
}

void panel_indicator(cairo_t *cr, double cx, double y, gboolean active)
{
	double w = active ? 16 : 6;

	panel_color(cr, active ? PANEL_ACCENT : PANEL_INDICATOR);
	panel_round_rect(cr, floor(cx - w / 2), y - 3, w, 3, 1.5);
	cairo_fill(cr);
}

/* The LikeOS emblem: a 22-pixel rounded square in a blue gradient carrying a
 * white "L" -- the place and the size of the Windows logo on its start
 * button, but this system's own mark. */
void panel_start_button(cairo_t *cr, double x, double y, double w, double h,
			enum panel_tile state)
{
	panel_tile(cr, x, y, w, h, state);

	double s = 22;
	double ex = floor(x + (w - s) / 2);
	double ey = floor(y + (h - s) / 2) + (state == PANEL_TILE_DOWN ? 1 : 0);
	cairo_pattern_t *g = cairo_pattern_create_linear(ex, ey, ex + s, ey + s);

	cairo_pattern_add_color_stop_rgb(g, 0, 0x29 / 255.0, 0xa8 / 255.0,
					 0xff / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0x00 / 255.0, 0x5f / 255.0,
					 0xb8 / 255.0);
	cairo_set_source(cr, g);
	panel_round_rect(cr, ex, ey, s, s, 5);
	cairo_fill(cr);
	cairo_pattern_destroy(g);

	/* The L, built from two rounded bars rather than set in type: a glyph
	 * would follow whatever the font does at this size. */
	cairo_set_source_rgb(cr, 1, 1, 1);
	panel_round_rect(cr, ex + 7, ey + 5, 3.5, 12, 1.2);
	cairo_fill(cr);
	panel_round_rect(cr, ex + 7, ey + 13.5, 9, 3.5, 1.2);
	cairo_fill(cr);
}

static int draw_text(cairo_t *cr, double x, double cy, double w,
		     const char *text, const char *font, int align,
		     unsigned int color)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);

	/* Ellipsise rather than clip: a button narrowed by many open windows
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

	panel_color(cr, color);
	cairo_move_to(cr, floor(tx), floor(cy - th / 2.0));
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	return tw;
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
	/* The picture: the load history as accent-coloured bars on a pale
	 * card, 26 pixels square, where Windows shows the weather icon. */
	double gs = 26;
	double gx = x + 8, gy = floor(y + (h - gs) / 2);

	cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
	panel_round_rect(cr, gx, gy, gs, gs, 4);
	cairo_fill(cr);

	if (m->nload > 0) {
		double bw = (gs - 6) / (double)PANEL_LOAD_HISTORY;
		double bh_max = gs - 8;

		panel_color(cr, PANEL_ACCENT);
		for (int i = 0; i < m->nload; i++) {
			int slot = PANEL_LOAD_HISTORY - m->nload + i;
			double bh = m->load[i] * bh_max;

			/* Anything non-zero gets a pixel: a load of 0.01 is
			 * not nothing, and a blank graph looks broken. */
			if (bh < 1.0 && m->load[i] > 0.0)
				bh = 1.0;
			if (bh <= 0.0)
				continue;
			cairo_rectangle(cr, gx + 3 + slot * bw,
					gy + gs - 4 - bh, bw - 0.4, bh);
		}
		cairo_fill(cr);
	}

	/* The two lines: processor load over memory in use, as the weather
	 * widget sets its temperature over its forecast. */
	char l1[32], l2[32];
	int cpu = m->nload ? (int)(m->load[m->nload - 1] * 100.0 + 0.5) : 0;

	snprintf(l1, sizeof(l1), "CPU %d %%", cpu);
	snprintf(l2, sizeof(l2), "RAM %d %%", (int)(m->mem * 100.0 + 0.5));

	double tx = gx + gs + 8;
	double tw = x + w - tx;

	draw_text(cr, tx, y + h / 2 - 8, tw, l1, PANEL_FONT, 0, PANEL_TEXT);
	draw_text(cr, tx, y + h / 2 + 8, tw, l2, PANEL_FONT, 0,
		  PANEL_TEXT_DIM);
}

/* ------------------------------------------------------------------------ */
/* Clock                                                                     */
/* ------------------------------------------------------------------------ */

/* Both strings, formatted.  %H:%M and dd.mm.yyyy are written out rather than
 * taken from the locale: this is a fixed-width slot in a panel and a locale
 * that chose a longer form would simply be clipped. */
static void clock_strings(char *timebuf, size_t tsz, char *datebuf, size_t dsz)
{
	time_t now = time(NULL);
	struct tm tmv;

	timebuf[0] = datebuf[0] = '\0';
	if (!localtime_r(&now, &tmv))
		return;

	snprintf(timebuf, tsz, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
	snprintf(datebuf, dsz, "%02d.%02d.%04d", tmv.tm_mday, tmv.tm_mon + 1,
		 tmv.tm_year + 1900);
}

void panel_clock_draw(cairo_t *cr, double x, double y, double w, double h,
		      gboolean stacked)
{
	char timebuf[16], datebuf[16], both[40];

	clock_strings(timebuf, sizeof(timebuf), datebuf, sizeof(datebuf));

	if (!stacked) {
		snprintf(both, sizeof(both), "%s  %s", timebuf, datebuf);
		panel_text(cr, x, y + h / 2, w, both, FALSE, 1);
		return;
	}

	/* Time over date, both right-aligned and the same size and weight, as
	 * the Windows 11 tray sets them. */
	panel_text(cr, x + 8, y + h / 2 - 8, w - 16, timebuf, FALSE, 2);
	panel_text(cr, x + 8, y + h / 2 + 8, w - 16, datebuf, FALSE, 2);
}

int panel_clock_width(cairo_t *cr)
{
	/* Measured against the widest digits rather than the current time: the
	 * digits are not all the same width in a proportional face. */
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d =
		pango_font_description_from_string(PANEL_FONT);
	int w, h;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, "88.88.8888", -1);
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	return w + 16;
}
