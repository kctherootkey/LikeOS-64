// LikeOS -- LikeOS Media Player: the small building blocks of the
// interface.
//
// Glyphs (line drawings in the text colour of wherever they sit), the
// buttons made of them, page headers, the rounded rows of the lists and the
// tile cards of the library grids.  Everything here takes its colours
// from the theme through style classes; nothing hard-codes a colour.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_UI_H
#define MP_UI_H

#include <gtk/gtk.h>

/* The name of the first of the two icons the theme has: the LikeOS line
 * icon when the LikeOS icon theme is installed, the fallback otherwise. */
const char *mp_icon(const char *likeos_name, const char *fallback);

/* glyphs.c: draws the glyph `name' (a likeos-* name) centred on (cx, cy),
 * `size' pixels square, in the cairo source colour.  FALSE when there is no
 * drawing of that name. */
gboolean mp_glyph_paint(cairo_t *cr, const char *name, double cx, double cy,
			double size);
gboolean mp_glyph_known(const char *name);
/* The glyph as a picture `size' pixels square, for a cell renderer. */
cairo_surface_t *mp_glyph_surface(GtkWidget *w, const char *likeos_name,
				  int size, const GdkRGBA *color);

/* A glyph: drawn in the widget's CSS colour, so that it follows hover,
 * pressed, checked and dark-strip colours like text does. */
GtkWidget *mp_glyph_new(const char *likeos_name, const char *fallback,
			int size);
void mp_glyph_set(GtkWidget *glyph, const char *likeos_name,
		  const char *fallback);

/* A button showing just a glyph.  It never takes the keyboard focus: the
 * window's own key handler is what the keys are for. */
GtkWidget *mp_glyph_button(const char *likeos_name, const char *fallback,
			   int size, const char *tooltip,
			   const char *css_class, gboolean toggle);
void mp_glyph_button_set(GtkWidget *button, const char *likeos_name,
			 const char *fallback);

/* A command button: glyph and label.  `accent' makes it the accent-filled
 * one a page has at most one of. */
GtkWidget *mp_text_button(const char *likeos_name, const char *fallback,
			  const char *label, gboolean accent);

void mp_add_class(GtkWidget *w, const char *cls);

/* A page's title row: the title (with a status line under it, empty until
 * mp_page_set_subtitle()) at the left and, at the right, a row the page
 * puts its command buttons into (returned in `actions_out'). */
GtkWidget *mp_page_header(const char *title, GtkWidget **actions_out);
void mp_page_set_subtitle(GtkWidget *header, const char *text);
/* A section title inside a page ("Recent media"). */
GtkWidget *mp_section_label(const char *text);

/* The lists: plain rows, with the hovered and selected ones filled as
 * rounded rows in the theme's colours (style classes mp-row-hover and
 * mp-row-selected on the tree view).
 * `current' (may be NULL) says which row is the one playing, which gets
 * mp-row-current. */
typedef gboolean (*MpRowIsCurrent)(GtkTreeModel *m, GtkTreeIter *it,
				   gpointer data);
void mp_list_style(GtkTreeView *tv, MpRowIsCurrent current, gpointer data);

/* Sets a text cell's text in a colour, semibold when `strong'. */
void mp_cell_set_text(GtkCellRenderer *cell, const char *text,
		      const GdkRGBA *color, gboolean strong);

/* ---- tiles ------------------------------------------------------------ */

typedef enum {
	MP_TILE_SQUARE,	/* album, recent music */
	MP_TILE_WIDE,	/* video, 16:9 */
	MP_TILE_ROUND,	/* artist */
} MpTileShape;

typedef struct MpTile MpTile;

/* Returns the picture for a tile (a borrowed reference), or NULL for the
 * placeholder.  May be called on every redraw, so it must be cheap once
 * the picture has been loaded. */
typedef GdkPixbuf *(*MpTileArt)(MpTile *tile, gpointer data);
/* The tile was opened (clicked, or Enter) or its play button clicked. */
typedef void (*MpTileAction)(MpTile *tile, gboolean play, gpointer data);

struct MpTile {
	MpTileShape shape;
	gchar *key;		/* what the tile stands for: album key, URI */
	gchar *title;
	gchar *subtitle;
	gchar *placeholder;	/* glyph on the placeholder: likeos name */
	gchar *placeholder_fb;	/* ... and its fallback */
	gchar *badge;		/* small text over the picture ("3:21") */
	MpTileArt art;
	MpTileAction action;
	gpointer data;
	GtkWidget *child;	/* the GtkFlowBoxChild */
	GtkWidget *area;	/* the picture */
	int width, height;	/* of the picture */
};

/* A tile, already wrapped in its GtkFlowBoxChild; owns copies of the
 * strings. */
GtkWidget *mp_tile_new(MpTileShape shape, int width, const char *key,
		       const char *title, const char *subtitle,
		       const char *placeholder, const char *placeholder_fb,
		       MpTileArt art, MpTileAction action, gpointer data);
MpTile *mp_tile_of(GtkWidget *child);
void mp_tile_set_badge(GtkWidget *child, const char *badge);
/* A grid of tiles: a GtkFlowBox set up the way the pages use it. */
GtkWidget *mp_tile_grid_new(void);
void mp_tile_grid_clear(GtkWidget *grid);

/* Draws a picture to fill (cover) the rectangle, clipped to a rounded
 * rectangle or a circle -- the way tiles and the now-playing art show
 * covers and video frames. */
void mp_draw_picture(cairo_t *cr, GdkPixbuf *pb, double x, double y,
		     double w, double h, double radius);
void mp_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		     double r);

/* ---- settings cards --------------------------------------------------- */

/* One row of a settings card: glyph, title and description at the left,
 * `control' at the right. */
GtkWidget *mp_setting_row(const char *likeos_name, const char *fallback,
			  const char *title, const char *description,
			  GtkWidget *control);

#endif
