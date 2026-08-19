// LikeOS-64 -- shared drawing for the desktop panel and its widgets.
//
// See panel.h for what this is and why it exists: the clock and the load
// monitor are drawn by these functions whether they are running as their own
// programs or as areas inside the taskbar, so the two cannot drift apart.

#include "panel.h"

#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>

void panel_fill_face(cairo_t *cr, double x, double y, double w, double h)
{
	cairo_set_source_rgb(cr, PANEL_FACE_R, PANEL_FACE_G, PANEL_FACE_B);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

/* One L-shaped band of a bevel: along the top and down the left, or along the
 * bottom and up the right, `n' pixels thick.
 *
 * This is the geometry of ctwm's Draw3DBorder() (drawing.c), which draws each
 * band as n nested L-shapes with line i inset by i and shortened by i at the
 * far end.  That shortening is what mitres the top-right and bottom-left
 * corners at 45 degrees, and the mitre is most of what makes an X toolkit's
 * bevel look different from a Windows one.
 *
 * Filled rectangles rather than stroked lines: a stroke is centred on its
 * path, so a one-pixel line on an integer coordinate straddles two pixel
 * columns and comes out grey and two wide.  These edges have to be crisp.
 */
static void bevel_band(cairo_t *cr, double x, double y, double w, double h,
		       int n, gboolean topleft)
{
	for (int i = 0; i < n; i++) {
		if (topleft) {
			/* top edge from the left corner, stopping i short of
			 * the right; left edge likewise, i short of the
			 * bottom */
			cairo_rectangle(cr, x, y + i, w - i, 1);
			cairo_rectangle(cr, x + i, y, 1, h - i);
		} else {
			cairo_rectangle(cr, x + w - i - 1, y + i, 1, h - i);
			cairo_rectangle(cr, x + i, y + h - i - 1, w - i, 1);
		}
	}
	cairo_fill(cr);
}

void panel_bevel_n(cairo_t *cr, double x, double y, double w, double h,
		   int n, gboolean raised)
{
	const double tl_r = raised ? PANEL_TOP_R : PANEL_BOT_R;
	const double tl_g = raised ? PANEL_TOP_G : PANEL_BOT_G;
	const double tl_b = raised ? PANEL_TOP_B : PANEL_BOT_B;
	const double br_r = raised ? PANEL_BOT_R : PANEL_TOP_R;
	const double br_g = raised ? PANEL_BOT_G : PANEL_TOP_G;
	const double br_b = raised ? PANEL_BOT_B : PANEL_TOP_B;

	cairo_set_source_rgb(cr, tl_r, tl_g, tl_b);
	bevel_band(cr, x, y, w, h, n, TRUE);
	cairo_set_source_rgb(cr, br_r, br_g, br_b);
	bevel_band(cr, x, y, w, h, n, FALSE);
}

void panel_bevel(cairo_t *cr, double x, double y, double w, double h,
		 gboolean raised, gboolean thin)
{
	panel_bevel_n(cr, x, y, w, h, thin ? 1 : 2, raised);
}

static int draw_text(cairo_t *cr, double x, double y, double w,
		     const char *text, const char *font, int align)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);

	/* Ellipsise rather than clip.  A task button narrowed by a dozen open
	 * windows shows "Mozilla Fire..." instead of a title cut mid-glyph,
	 * which is what the shell this imitates did. */
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

	cairo_move_to(cr, tx, y);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	return tw;
}

int panel_text(cairo_t *cr, double x, double y, double w, const char *text,
	       gboolean bold, int align)
{
	return draw_text(cr, x, y, w, text,
			 bold ? PANEL_FONT_BOLD : PANEL_FONT, align);
}

void panel_start_button(cairo_t *cr, double x, double y, double w, double h,
			gboolean pressed)
{
	/* A hard outline all the way round, which the tray wells do not have.
	 * Motif draws this as the "highlight" rectangle a button gets when it
	 * can take the keyboard focus, and it is what separates the button
	 * from the bar it sits on rather than letting the light band bleed
	 * into the face behind it. */
	cairo_set_source_rgb(cr, PANEL_DARK_R, PANEL_DARK_G, PANEL_DARK_B);
	cairo_rectangle(cr, x, y, w, 1);
	cairo_rectangle(cr, x, y + h - 1, w, 1);
	cairo_rectangle(cr, x, y, 1, h);
	cairo_rectangle(cr, x + w - 1, y, 1, h);
	cairo_fill(cr);

	panel_fill_face(cr, x + 1, y + 1, w - 2, h - 2);
	panel_bevel(cr, x + 1, y + 1, w - 2, h - 2, !pressed, FALSE);

	/* The lettering, cut into the face.
	 *
	 * Two passes: the label in the top shadow colour a pixel down and
	 * right, then in black on top.  The light copy peeking out along the
	 * lower-right of every stroke is what makes the text look engraved
	 * rather than printed -- it is the same trick Motif uses for an
	 * insensitive label, borrowed here because the panel's one branded
	 * control should not look like a plain string.
	 *
	 * Bold and a size up from the rest of the panel: this is the button
	 * everything else is arranged around. */
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d =
		pango_font_description_from_string("DejaVu Sans Bold 9");
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, "LikeOS", -1);
	pango_layout_get_pixel_size(l, &tw, &th);

	double tx = x + (w - tw) / 2 + (pressed ? 1 : 0);
	double ty = y + (h - th) / 2 + (pressed ? 1 : 0);

	cairo_set_source_rgb(cr, PANEL_TOP_R, PANEL_TOP_G, PANEL_TOP_B);
	cairo_move_to(cr, tx + 1, ty + 1);
	pango_cairo_show_layout(cr, l);

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_move_to(cr, tx, ty);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

int panel_text_small(cairo_t *cr, double x, double y, double w,
		     const char *text, int align)
{
	return draw_text(cr, x, y, w, text, PANEL_FONT_SMALL, align);
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
	panel_fill_face(cr, x, y, w, h);
	panel_bevel(cr, x, y, w, h, FALSE, FALSE);

	double ix = x + 2, iy = y + 2;
	double iw = w - 4, ih = h - 4;

	/* The load pane takes the larger share: it carries the history and
	 * needs the width, while the memory pane holds a label and ten
	 * segments. */
	double lw = (iw * 3.0) / 5.0;

	cairo_set_source_rgb(cr, 0, 0, 0);

	/* --- load ---------------------------------------------------------- */
	/* The label row and the graph row are sized against the 22 pixels a
	 * 28-pixel panel leaves inside the tray's bevel: 10 for the text at
	 * PANEL_FONT_SMALL and the rest for the graph.  These are deliberately
	 * not fractions of the height -- at this size a rounding error is a
	 * visible pixel. */
	panel_text_small(cr, ix + 3, iy - 1, lw - 6, "Load", 0);

	double gx = ix + 3;
	double gy = iy + 10;
	double gw = lw - 6;
	double gh = ih - 10;

	if (gh > 0 && m->nload > 0) {
		double bw = gw / (double)PANEL_LOAD_HISTORY;
		/* No gap once the bars are thin: a gap wider than the bar
		 * leaves a row of dots rather than a graph. */
		double gap = bw > 2.5 ? 1.0 : 0.0;

		for (int i = 0; i < m->nload; i++) {
			/* Newest at the right.  While the history is still
			 * filling it is short, so it is pushed over rather
			 * than drawn from the left edge. */
			int slot = PANEL_LOAD_HISTORY - m->nload + i;
			double bh = m->load[i] * gh;

			/* Anything non-zero gets at least one pixel: a load of
			 * 0.01 is not nothing, and a graph that renders it as
			 * blank looks broken rather than quiet. */
			if (bh < 1.0 && m->load[i] > 0.0)
				bh = 1.0;
			if (bh <= 0.0)
				continue;
			cairo_rectangle(cr, gx + slot * bw, gy + gh - bh,
					bw - gap, bh);
		}
		cairo_fill(cr);
	}

	/* The divider between the panes, drawn as a sunken pair so it matches
	 * the frame around them. */
	double dx = ix + lw;

	cairo_set_source_rgb(cr, PANEL_SHADOW_R, PANEL_SHADOW_G,
			     PANEL_SHADOW_B);
	cairo_rectangle(cr, dx, iy, 1, ih);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, PANEL_LIGHT_R, PANEL_LIGHT_G, PANEL_LIGHT_B);
	cairo_rectangle(cr, dx + 1, iy, 1, ih);
	cairo_fill(cr);

	/* --- memory --------------------------------------------------------- */
	double mx = dx + 5;
	double mw = ix + iw - mx - 2;
	char pct[8];

	if (mw < 8)
		return;

	snprintf(pct, sizeof(pct), "%d%%", (int)(m->mem * 100.0 + 0.5));
	cairo_set_source_rgb(cr, 0, 0, 0);
	panel_text_small(cr, mx, iy - 1, mw / 2, "Mem", 0);
	panel_text_small(cr, mx + mw / 2, iy - 1, mw / 2, pct, 2);

	double sy = iy + 10;
	double sh = ih - 10;

	if (sh > 0) {
		double sw = mw / (double)PANEL_MEM_SEGS;
		/* Rounded, not truncated, so a bar reading 42% is not drawn as
		 * four tenths: the number and the bar have to agree. */
		int filled = (int)(m->mem * PANEL_MEM_SEGS + 0.5);

		for (int i = 0; i < PANEL_MEM_SEGS; i++) {
			double sx = mx + i * sw;

			cairo_set_source_rgb(cr, 0, 0, 0);
			if (i < filled) {
				cairo_rectangle(cr, sx, sy, sw - 1, sh);
				cairo_fill(cr);
			} else {
				cairo_set_line_width(cr, 1.0);
				cairo_rectangle(cr, sx + 0.5, sy + 0.5,
						sw - 2, sh - 1);
				cairo_stroke(cr);
			}
		}
	}
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
	cairo_set_source_rgb(cr, 0, 0, 0);

	if (!stacked) {
		/* One centred line, which is what the tray shows: the date and
		 * the time separated by a double space so the pair reads as
		 * two fields rather than one long number. */
		snprintf(both, sizeof(both), "%s  %s", datebuf, timebuf);

		PangoLayout *l = pango_cairo_create_layout(cr);
		PangoFontDescription *d =
			pango_font_description_from_string(PANEL_FONT);
		int tw, th;

		pango_layout_set_font_description(l, d);
		pango_font_description_free(d);
		pango_layout_set_text(l, both, -1);
		pango_layout_get_pixel_size(l, &tw, &th);
		cairo_move_to(cr, x + (w - tw) / 2, y + (h - th) / 2);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
		return;
	}

	/* Stacked: the time carries the weight because it is the thing read at
	 * a glance; the date underneath is a size smaller and regular.  Both
	 * are right-aligned to the same column, which is what makes the pair
	 * read as one block rather than two strings. */
	PangoLayout *tl = pango_cairo_create_layout(cr);
	PangoLayout *dl = pango_cairo_create_layout(cr);
	PangoFontDescription *d;
	int tw, th, dw, dh;

	d = pango_font_description_from_string("DejaVu Sans Bold 11");
	pango_layout_set_font_description(tl, d);
	pango_font_description_free(d);
	pango_layout_set_text(tl, timebuf, -1);
	pango_layout_get_pixel_size(tl, &tw, &th);

	d = pango_font_description_from_string("DejaVu Sans 9");
	pango_layout_set_font_description(dl, d);
	pango_font_description_free(d);
	pango_layout_set_text(dl, datebuf, -1);
	pango_layout_get_pixel_size(dl, &dw, &dh);

	int gap = 1;
	int total = th + gap + dh;
	double ty = y + (h - total) / 2;

	if (ty < y)
		ty = y;

	cairo_move_to(cr, x + w - 8 - tw, ty);
	pango_cairo_show_layout(cr, tl);
	cairo_move_to(cr, x + w - 8 - dw, ty + th + gap);
	pango_cairo_show_layout(cr, dl);

	g_object_unref(tl);
	g_object_unref(dl);
}

int panel_clock_width(cairo_t *cr)
{
	/* Measured against a worst-case string rather than the current time:
	 * the digits are not all the same width in a proportional face, so a
	 * tray sized to 01:11 would clip 28.12.2026. */
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d =
		pango_font_description_from_string(PANEL_FONT);
	int w, h;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, "88.88.8888  88:88", -1);
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	return w;
}
