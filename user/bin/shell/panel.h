/* LikeOS -- shared drawing for the desktop panel and its widgets.
 *
 * The clock and the load monitor exist twice over: as the standalone programs
 * datetime(1) and load(1), and as areas inside taskbar(1).  This header is what
 * makes that one implementation rather than two -- the taskbar draws the same
 * pixels by calling the same functions, so the widgets cannot drift apart.
 *
 * Everything here is Cairo drawing into a caller-supplied context at a
 * caller-supplied rectangle.  Nothing owns a widget, a window or a timer; the
 * programs do that.
 *
 * The look is the LikeOS shell's: apps are light, the shell is dark.  The
 * panel is the shell's deep blue-grey, tinted a little by the wallpaper under
 * it when taskbar(1) can sample the desktop's root pixmap (see tint_* there);
 * PANEL_BG is the solid colour used when it cannot, and by the widgets
 * running as their own windows.  Controls are invisible at rest and show as a
 * soft light tile under the pointer; the focused window's button keeps a
 * brighter one, and small dots under a button count its program's windows --
 * in the shell accent for the one that has the focus.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_PANEL_H
#define _LIKEOS_PANEL_H

#include <gtk/gtk.h>

/* The dark shell palette, as 0xRRGGBB (the same values as the GTK theme's
 * $shell-* tokens, res/xorg/gtk3/theme/src/_colors.scss). */
#define PANEL_BG            0x161c2a /* the panel and every flyout */
#define PANEL_RAISED        0x1f2636 /* cards, fields and tiles on it */
#define PANEL_BORDER        0x2c3446 /* the frame of a flyout */
#define PANEL_TEXT          0xeef2fa
#define PANEL_TEXT_DIM      0xa4adbf /* secondary text */
#define PANEL_TEXT_DISABLED 0x6b7486
#define PANEL_ACCENT        0x5b8ff9 /* indicators, focus, links */
#define PANEL_ACCENT_FILL   0x3f74f2 /* switches on, slider fill, today */

/* The panel is 48 pixels tall; every control on it is a 40-pixel tile. */
#define PANEL_HEIGHT 48

/* Fonts: Selawik, the system's interface face, at 9 points. */
#define PANEL_FONT "Selawik 9"
#define PANEL_FONT_BOLD "Selawik Semibold 9"

/* Set the Cairo source to a 0xRRGGBB colour. */
void panel_color(cairo_t *cr, unsigned int rgb);

/* Set the Cairo source to a 0xRRGGBB colour at alpha `a'. */
void panel_color_a(cairo_t *cr, unsigned int rgb, double a);

/* Fill a rectangle with the panel's colour. */
void panel_fill_face(cairo_t *cr, double x, double y, double w, double h);

/* A rounded rectangle path (radius r). */
void panel_round_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r);

/* Tile states: how a control is lit. */
enum panel_tile {
	PANEL_TILE_NONE,    /* at rest: nothing drawn */
	PANEL_TILE_HOVER,   /* under the pointer */
	PANEL_TILE_LIT,     /* the focused window's button, or an open flyout's */
	PANEL_TILE_DOWN,    /* held down */
};

/* Radius of every tile on the panel. */
#define PANEL_TILE_RADIUS 8

/* The rounded tile behind a control in the given state: white at a low
 * alpha over the panel, so it reads the same over the solid colour and over
 * the wallpaper tint. */
void panel_tile(cairo_t *cr, double x, double y, double w, double h,
		enum panel_tile state);

/* The window count under a program's button: one 4-pixel dot per window, up
 * to three, centred on cx with their centres on cy -- in the secondary text
 * colour, or the accent when the program has the focus. */
void panel_dots(cairo_t *cr, double cx, double cy, int count,
		gboolean active);

/* A plain glass orb of radius `r' centred on (cx, cy): a blue body lit from
 * below with a gloss along its top.  `glow' (0..1) brightens it and lays a
 * soft halo round it; `pressed' darkens it and shades it in from the top.
 * The start button's artwork is the icon theme's likeos-start-orb; this is
 * what the panel draws when the theme has none. */
void panel_orb(cairo_t *cr, double cx, double cy, double r, double glow,
	       gboolean pressed);

/* The magnifier glyph, `s' pixels square (16 or 20), in the current source. */
void panel_glyph_search(cairo_t *cr, double x, double y, double s);

/* Text, clipped and ellipsised to `w', in PANEL_TEXT.
 *
 * `align' is 0 for left, 1 for centre, 2 for right.  Returns the width the
 * text actually took, which the callers that pack things next to it need.
 * The text is centred vertically on `cy'. */
int panel_text(cairo_t *cr, double x, double cy, double w, const char *text,
	       gboolean bold, int align);

/* As panel_text(), in any font and in any colour, alpha included. */
int panel_text_full(cairo_t *cr, double x, double cy, double w,
		    const char *text, const char *font, const double rgba[4],
		    int align);

/* The width `text' takes in `font'. */
int panel_text_width(cairo_t *cr, const char *text, const char *font);

/* ---------------------------------------------------------------------------
 * The load and memory monitor.
 * ------------------------------------------------------------------------ */

/* Samples in the load history, and so the points of the curve. */
#define PANEL_LOAD_HISTORY 14

/* Seconds between samples.  The load average is itself a decaying mean over a
 * minute, so sampling faster shows the same curve with more points. */
#define PANEL_LOAD_SECS 2

struct panel_load {
	double load[PANEL_LOAD_HISTORY]; /* 0..1, share of the whole machine */
	int nload;			 /* samples held, capped at HISTORY */
	double mem;			 /* 0..1, share of RAM in use */
	int ncpu;
};

/* Set up the monitor: zero the history and read the processor count. */
void panel_load_init(struct panel_load *m);

/* Take one sample.  Call every PANEL_LOAD_SECS seconds. */
void panel_load_sample(struct panel_load *m);

/* Draw the monitor in the given rectangle: the load history as a small
 * curve over a soft accent fill on a raised card, and beside it two lines,
 * the processor load and the memory in use.  This is the whole of what
 * load(1) shows. */
void panel_load_draw(cairo_t *cr, struct panel_load *m, double x, double y,
		     double w, double h);

/* The width panel_load_draw needs. */
#define PANEL_LOAD_W 116

/* ---------------------------------------------------------------------------
 * The clock.
 * ------------------------------------------------------------------------ */

/* Draw the clock inside the given rectangle.  On the panel it is one line,
 * the date in the secondary colour and the time after it: "Mon 2 Nov  14:32",
 * right-aligned.  `stacked' puts the time over the date instead, centred --
 * the form datetime(1) uses in a window of its own. */
void panel_clock_draw(cairo_t *cr, double x, double y, double w, double h,
		      gboolean stacked);

/* The width the one-line clock needs, so a caller can size a tray area to
 * it.  Measured with the widest digits, so it changes only with the date. */
int panel_clock_width(cairo_t *cr);

#endif /* _LIKEOS_PANEL_H */
