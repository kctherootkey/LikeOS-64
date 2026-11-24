// LikeOS Task Viewer -- the program's own line glyphs: the navigation rail
// and the command bar.
//
// Drawn as vector geometry of LikeOS's own (tv-glyphs.c) on a 16-unit box,
// strokes 1.5 units wide with round caps and joins, in the CSS colour of
// wherever the glyph sits -- so they follow hover, selection and the
// insensitive state like text does.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_GLYPHS_H
#define TV_GLYPHS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
	TV_GLYPH_NONE = 0,
	TV_GLYPH_PROCESSES,
	TV_GLYPH_PERFORMANCE,
	TV_GLYPH_USERS,
	TV_GLYPH_DETAILS,
	TV_GLYPH_SERVICES,
	TV_GLYPH_STARTUP,
	TV_GLYPH_RUN,
	TV_GLYPH_END_TASK,
	TV_GLYPH_MORE,
	TV_GLYPH_MENU,
	TV_GLYPH_COUNT
} TvGlyph;

/* The rail glyph of a page id ("processes", ...), TV_GLYPH_NONE for an id
 * without one (the page's themed icon is shown then). */
TvGlyph tv_glyph_for_page(const char *id);

/* Paints `g' centred on (cx, cy), `size' pixels square, in the cairo
 * source colour. */
void tv_glyph_paint(cairo_t *cr, TvGlyph g, double cx, double cy, double size);

/* A glyph widget, drawn in its CSS colour. */
GtkWidget *tv_glyph_new(TvGlyph g, int size);

G_END_DECLS

#endif
