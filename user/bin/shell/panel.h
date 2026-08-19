/* LikeOS-64 -- shared drawing for the desktop panel and its widgets.
 *
 * The clock and the load monitor exist twice over: as the standalone programs
 * datetime(1) and load(1), and as areas inside taskbar(1).  This header is what
 * makes that one implementation rather than two -- the taskbar draws the same
 * pixels by calling the same functions, so the widgets cannot drift apart.
 *
 * Everything here is Cairo drawing into a caller-supplied context at a
 * caller-supplied rectangle.  Nothing owns a widget, a window or a timer; the
 * programs do that.
 */
#ifndef _LIKEOS_PANEL_H
#define _LIKEOS_PANEL_H

#include <gtk/gtk.h>

/* The 3D palette: Motif's construction, in Windows 2000's colours.
 *
 * The two toolkits differ less in their greys than in the SHAPE of an edge.
 * Windows draws four one-pixel lines -- white then face going in on the
 * top-left, near-black then mid-grey going in on the bottom-right -- which is
 * what gives it that hard, high-contrast chisel.  Motif draws two solid
 * two-pixel bands instead, a topShadow and a bottomShadow, each computed from
 * the background rather than picked from a fixed palette: roughly 150% of it
 * for the light side and 55% for the dark.  The result is softer and reads as
 * moulded rather than stamped, which is the Unix look asked for here.
 *
 * So the face stays #d4d0c8 -- the panel is still of that era -- and the edges
 * are Motif's: no pure white, no near-black, and no four-tone stack.
 *
 * TOP and BOT are the two shadow bands.  LIGHT and DARK remain for the few
 * places that genuinely want an extreme: the panel's own outer edge and the
 * dotted focus fill. */
#define PANEL_FACE_R 0.831 /* #d4d0c8, the background everything sits on */
#define PANEL_FACE_G 0.816
#define PANEL_FACE_B 0.784

/* ctwm's own shadow colours, from its own formulas.
 *
 * GetShadeColors() in util.c derives both from the background:
 *
 *     clear = c + (65535 - c) * (ClearShadowContrast / 100)
 *     dark  = c * ((100 - DarkShadowContrast) / 100)
 *
 * with the defaults this build uses, ClearShadowContrast 50 and
 * DarkShadowContrast 40.  Run over the panel face that gives #e9e7e3 and
 * #7f7c78 -- so the bar's edges are shaded by exactly the rule ctwm shades its
 * window frames by, and the two look like parts of one desktop rather than two
 * toolkits sharing a screen. */
#define PANEL_TOP_R 0.914 /* #e9e7e3, ctwm's shadc for this face */
#define PANEL_TOP_G 0.906
#define PANEL_TOP_B 0.890

#define PANEL_BOT_R 0.498 /* #7f7c78, ctwm's shadd for this face */
#define PANEL_BOT_G 0.486
#define PANEL_BOT_B 0.471

#define PANEL_LIGHT_R 1.000 /* #ffffff */
#define PANEL_LIGHT_G 1.000
#define PANEL_LIGHT_B 1.000

#define PANEL_SHADOW_R 0.502 /* #808080 */
#define PANEL_SHADOW_G 0.502
#define PANEL_SHADOW_B 0.502

#define PANEL_DARK_R 0.251 /* #404040 */
#define PANEL_DARK_G 0.251
#define PANEL_DARK_B 0.251

/* Panel height, and the width of the frame around it.
 *
 * 28 was the Windows 2000 taskbar exactly, and at the bottom of a framebuffer
 * screen it read as a sliver.  34 gives the bar the ctwm frame below and still
 * leaves a 24-pixel button, which is within a pixel of what that shell used.
 *
 * PANEL_BORDER is ctwm's window border, not a Windows one: ctwm draws a raised
 * 3D band of ThreeDBorderWidth around every frame it manages, and the panel
 * wearing the same band is what makes it look like part of this desktop.
 * Three rather than ctwm's default six -- six around a 34-pixel bar leaves no
 * room for anything inside it. */
#define PANEL_HEIGHT 34
#define PANEL_BORDER 3

/* Fonts.  DejaVu Sans is the only proportional face on the image; the classic
 * shell used an 8pt UI font, and 8 here lands at about the same size.
 *
 * The tray gets a size smaller.  At 28 pixels the load monitor has to fit a
 * label and a graph one above the other inside 22 pixels of usable height, and
 * 8pt leaves the graph three pixels tall. */
#define PANEL_FONT "DejaVu Sans 8"
#define PANEL_FONT_BOLD "DejaVu Sans Bold 8"
#define PANEL_FONT_SMALL "DejaVu Sans 6.5"

/* Fill a rectangle with the face colour. */
void panel_fill_face(cairo_t *cr, double x, double y, double w, double h);

/* A 3D edge in ctwm's shape: two bands, topShadow along the top and left and
 * bottomShadow along the bottom and right, mitred on the diagonal at the other
 * two corners.
 *
 * This is Draw3DBorder() from ctwm's drawing.c, in Cairo.  Each band is drawn
 * as `n' nested L-shapes, every one a pixel shorter at the far end than the
 * last, which is what produces the 45-degree join at the top-right and
 * bottom-left; squaring those off is the giveaway of a hand-rolled bevel and
 * is the main thing that made the earlier version look like a Windows control
 * rather than an X one.
 *
 * `raised' is the button look; clearing it swaps the bands, which is the whole
 * of what makes a hole rather than a button. */
void panel_bevel_n(cairo_t *cr, double x, double y, double w, double h,
		   int n, gboolean raised);

/* The common cases: a two-pixel bevel, or a one-pixel one for the small
 * controls -- two pixels around a 24-pixel workspace button leaves almost no
 * face showing. */
void panel_bevel(cairo_t *cr, double x, double y, double w, double h,
		 gboolean raised, gboolean thin);

/* The LikeOS button.
 *
 * Its own function rather than a bevel and a label, because it is the one
 * control with a look of its own: a hard outline around the Motif bevel, and a
 * label cut into the face with a highlight below it.  See the implementation
 * for how the lettering is built. */
void panel_start_button(cairo_t *cr, double x, double y, double w, double h,
			gboolean pressed);

/* Text, clipped and ellipsised to `w'.
 *
 * `align' is 0 for left, 1 for centre, 2 for right.  Returns the width the
 * text actually took, which the callers that pack things next to it need.
 * panel_text() uses the panel font; panel_text_small() the tray one. */
int panel_text(cairo_t *cr, double x, double y, double w, const char *text,
	       gboolean bold, int align);
int panel_text_small(cairo_t *cr, double x, double y, double w,
		     const char *text, int align);

/* ---------------------------------------------------------------------------
 * The load and memory monitor.
 * ------------------------------------------------------------------------ */

/* Samples in the load history, and so the number of bars drawn.  Chosen
 * against the pane width rather than a span of time: at the two-second sample
 * below it is just under a minute of history, and it leaves each bar wide
 * enough to be a bar rather than a hairline. */
#define PANEL_LOAD_HISTORY 28

/* Seconds between samples.  The load average is itself a decaying mean over a
 * minute, so sampling faster shows the same curve with more points. */
#define PANEL_LOAD_SECS 2

/* Segments in the memory bar.  Ten reads as tenths without needing a scale. */
#define PANEL_MEM_SEGS 10

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

/* Draw the two panes -- load history left, memory right -- inside the given
 * rectangle, framed sunken.  This is the whole of what load(1) shows. */
void panel_load_draw(cairo_t *cr, struct panel_load *m, double x, double y,
		     double w, double h);

/* ---------------------------------------------------------------------------
 * The clock.
 * ------------------------------------------------------------------------ */

/* Draw the date and time inside the given rectangle.
 *
 * `stacked' picks the arrangement: set, the time sits above the date, both
 * right-aligned, which is what datetime(1) shows on its own; clear, they are
 * one centred line "19.05.2026  14:32", which is what fits the panel's tray
 * and is what the classic shell's clock looked like. */
void panel_clock_draw(cairo_t *cr, double x, double y, double w, double h,
		      gboolean stacked);

/* The width the one-line form needs, so a caller can size a tray area to it
 * rather than guessing.  Depends on the font, so it is measured, not counted. */
int panel_clock_width(cairo_t *cr);

#endif /* _LIKEOS_PANEL_H */
