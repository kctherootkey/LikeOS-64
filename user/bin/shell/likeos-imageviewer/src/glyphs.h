// LikeOS -- LikeOS Image Viewer: the program's own line glyphs.
//
// Every glyph of the title row, the strips, the menus, the info panel and
// the editor is a drawing of LikeOS's own (glyphs.c): a 16-unit box,
// strokes 1.5 units wide, round caps and joins, painted in the colour the
// caller has set -- or, as a widget, in the CSS colour of wherever it sits,
// so it follows hover, pressed, checked and disabled states like text.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_GLYPHS_H
#define IV_GLYPHS_H

#include <gtk/gtk.h>

typedef enum {
	IV_GLYPH_NONE = 0,

	/* window frame (drawn on a 10-unit box, see glyphs.c) */
	IV_GLYPH_MINIMIZE,
	IV_GLYPH_MAXIMIZE,
	IV_GLYPH_RESTORE,
	IV_GLYPH_CLOSE,

	/* viewer commands */
	IV_GLYPH_EDIT,
	IV_GLYPH_ROTATE_RIGHT,
	IV_GLYPH_ROTATE_LEFT,
	IV_GLYPH_DELETE,
	IV_GLYPH_MORE,
	IV_GLYPH_OPEN_WITH,
	IV_GLYPH_CHEVRON_DOWN,
	IV_GLYPH_CHEVRON_UP,
	IV_GLYPH_CHEVRON_LEFT,
	IV_GLYPH_CHEVRON_RIGHT,
	IV_GLYPH_PREVIOUS,
	IV_GLYPH_NEXT,
	IV_GLYPH_SAVE_AS,
	IV_GLYPH_COPY,
	IV_GLYPH_COPY_PATH,
	IV_GLYPH_RESIZE,
	IV_GLYPH_WALLPAPER,
	IV_GLYPH_MAIL,
	IV_GLYPH_RENAME,
	IV_GLYPH_SETTINGS,
	IV_GLYPH_KEYBOARD,
	IV_GLYPH_ABOUT,
	IV_GLYPH_FOLDER,
	IV_GLYPH_FOLDER_OPEN,
	IV_GLYPH_APP,

	/* the bottom strip */
	IV_GLYPH_FILMSTRIP,
	IV_GLYPH_SLIDESHOW,
	IV_GLYPH_PLAY,
	IV_GLYPH_PAUSE,
	IV_GLYPH_INFO,
	IV_GLYPH_FIT,
	IV_GLYPH_ACTUAL_SIZE,
	IV_GLYPH_ZOOM_IN,
	IV_GLYPH_ZOOM_OUT,
	IV_GLYPH_FULLSCREEN,
	IV_GLYPH_LEAVE_FULLSCREEN,

	/* the info panel */
	IV_GLYPH_IMAGE,
	IV_GLYPH_CALENDAR,
	IV_GLYPH_CAMERA,
	IV_GLYPH_LOCATION,
	IV_GLYPH_FILE,
	IV_GLYPH_CHECK,
	IV_GLYPH_WARNING,

	/* the editor shell */
	IV_GLYPH_BACK,
	IV_GLYPH_UNDO,
	IV_GLYPH_REDO,
	IV_GLYPH_REVERT,
	IV_GLYPH_COMPARE,
	IV_GLYPH_PLUS,
	IV_GLYPH_MINUS,

	/* the editor tools */
	IV_GLYPH_CROP,
	IV_GLYPH_ADJUST,
	IV_GLYPH_STYLES,
	IV_GLYPH_DRAW,
	IV_GLYPH_RETOUCH,
	IV_GLYPH_BACKGROUND,

	/* inside the tool panels */
	IV_GLYPH_FLIP_H,
	IV_GLYPH_FLIP_V,
	IV_GLYPH_STRAIGHTEN,
	IV_GLYPH_ASPECT,
	IV_GLYPH_SWAP,
	IV_GLYPH_RESET,
	IV_GLYPH_LIGHT,
	IV_GLYPH_COLOR,
	IV_GLYPH_DETAIL,
	IV_GLYPH_WAND,
	IV_GLYPH_PEN,
	IV_GLYPH_MARKER,
	IV_GLYPH_CALLIGRAPHY,
	IV_GLYPH_ERASER,
	IV_GLYPH_RECT,
	IV_GLYPH_CLEAR,
	IV_GLYPH_BRUSH,
	IV_GLYPH_BLUR,
	IV_GLYPH_REMOVE_BG,
	IV_GLYPH_FILL,
	IV_GLYPH_LOCK,
	IV_GLYPH_UNLOCK,
	IV_GLYPH_EYEDROPPER,

	IV_GLYPH_COUNT
} IvGlyph;

/* Paints `g' centred on (cx, cy), `size' pixels square, in the cairo
 * source colour.  The window frame glyphs use the 10-unit box and the
 * 1.25-pixel stroke of every LikeOS title bar; the rest the 16-unit box. */
void iv_glyph_paint(cairo_t *cr, IvGlyph g, double cx, double cy,
		    double size);

/* A short, stable name for a glyph ("rotate-right"), for tests and for
 * the accessible description of glyph-only buttons. */
const char *iv_glyph_name(IvGlyph g);

/* A glyph widget: drawn in its own CSS colour (style class iv-glyph). */
GtkWidget *iv_glyph_new(IvGlyph g, int size);
void iv_glyph_set(GtkWidget *glyph, IvGlyph g);
IvGlyph iv_glyph_get(GtkWidget *glyph);

/* The glyph as a picture `size' pixels square in `color', for places that
 * take a surface (cell renderers, drag icons). */
cairo_surface_t *iv_glyph_surface(IvGlyph g, int size, const GdkRGBA *color);

#endif
