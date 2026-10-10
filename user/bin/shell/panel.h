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
 * The look is the Windows 11 taskbar's, light theme: a flat band of the pale
 * blue-grey Windows' Mica material takes on over a blue desktop, controls
 * that are invisible at rest and show as a lighter rounded tile under the
 * pointer, and short indicator bars under the buttons of running programs --
 * a blue one for the window that has the focus.  The colours were measured
 * from the real taskbar.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_PANEL_H
#define _LIKEOS_PANEL_H

#include <gtk/gtk.h>

/* Colours, as 0xRRGGBB. */
#define PANEL_BG            0xd6e0f6 /* the bar itself */
#define PANEL_EDGE          0xaeb4c1 /* the hairline along its top */
#define PANEL_TILE_HOVER    0xebf0fb /* a control under the pointer, and the
					focused window's button */
#define PANEL_TILE_PRESSED  0xe2e9f9 /* a control held down */
#define PANEL_TILE_EDGE     0xc2cbdf /* the lower edge of a lit tile */
#define PANEL_ACCENT        0x0078d4 /* the focused window's indicator */
#define PANEL_INDICATOR     0x767c88 /* a running window's indicator */
#define PANEL_TEXT          0x1b1b1b
#define PANEL_TEXT_DIM      0x5c5f66

/* Panel height: Windows 11's taskbar, 48 pixels at 100% scale. */
#define PANEL_HEIGHT 48

/* Fonts: Selawik (the open substitute for Segoe UI) at Windows' 9 points. */
#define PANEL_FONT "Selawik 9"
#define PANEL_FONT_BOLD "Selawik Semibold 9"

/* Set the Cairo source to a 0xRRGGBB colour. */
void panel_color(cairo_t *cr, unsigned int rgb);

/* Fill a rectangle with the bar's colour. */
void panel_fill_face(cairo_t *cr, double x, double y, double w, double h);

/* A rounded rectangle path (radius r). */
void panel_round_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r);

/* Tile states: how a control is lit. */
enum panel_tile {
	PANEL_TILE_NONE,    /* at rest: nothing drawn */
	PANEL_TILE_LIT,     /* under the pointer, or the focused window */
	PANEL_TILE_DOWN,    /* held down */
};

/* The rounded tile behind a control in the given state. */
void panel_tile(cairo_t *cr, double x, double y, double w, double h,
		enum panel_tile state);

/* The indicator bar under a running program's button, centred on cx with
 * its bottom at y: 16 pixels of accent for the focused window, 6 of grey
 * for the others. */
void panel_indicator(cairo_t *cr, double cx, double y, gboolean active);

/* The start button: a tile and the LikeOS emblem. */
void panel_start_button(cairo_t *cr, double x, double y, double w, double h,
			enum panel_tile state);

/* Text, clipped and ellipsised to `w', in PANEL_TEXT.
 *
 * `align' is 0 for left, 1 for centre, 2 for right.  Returns the width the
 * text actually took, which the callers that pack things next to it need.
 * The text is centred vertically on `cy'. */
int panel_text(cairo_t *cr, double x, double cy, double w, const char *text,
	       gboolean bold, int align);

/* ---------------------------------------------------------------------------
 * The load and memory monitor.
 * ------------------------------------------------------------------------ */

/* Samples in the load history, and so the number of bars drawn. */
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

/* Draw the monitor in the given rectangle, laid out the way Windows 11 lays
 * out the widget at the left of its taskbar: a small picture -- here the load
 * history as a bar graph -- and two lines of text beside it, the processor
 * load and the memory in use.  This is the whole of what load(1) shows. */
void panel_load_draw(cairo_t *cr, struct panel_load *m, double x, double y,
		     double w, double h);

/* The width panel_load_draw needs. */
#define PANEL_LOAD_W 128

/* ---------------------------------------------------------------------------
 * The clock.
 * ------------------------------------------------------------------------ */

/* Draw the time over the date, both right-aligned, as the Windows 11 tray
 * shows them, inside the given rectangle.  `stacked' clear puts them on one
 * line instead. */
void panel_clock_draw(cairo_t *cr, double x, double y, double w, double h,
		      gboolean stacked);

/* The width the clock needs, measured against the widest date and time in
 * the panel font, so a caller can size a tray area to it. */
int panel_clock_width(cairo_t *cr);

#endif /* _LIKEOS_PANEL_H */
