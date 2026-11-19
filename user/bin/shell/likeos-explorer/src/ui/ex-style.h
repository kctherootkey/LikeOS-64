// LikeOS Explorer: the look.
//
// Explorer's styling is part of the LikeOS GTK theme (res/xorg/gtk3/theme,
// the .likeos-explorer and lx-* rules, built from the same design tokens as
// the rest of the theme), so a theme change restyles Explorer with
// everything else.  This only names the style classes the widgets carry and,
// under any other theme, adds a small stylesheet made of that theme's own
// standard colours so the layout still reads right.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_STYLE_H
#define EX_STYLE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Style classes. */
#define EX_CLASS_WINDOW "likeos-explorer"
#define EX_CLASS_NAVBAR "ex-navbar"
#define EX_CLASS_COMMANDBAR "ex-commandbar"
#define EX_CLASS_SIDEBAR "ex-sidebar"
#define EX_CLASS_STATUSBAR "ex-statusbar"
#define EX_CLASS_HOME "ex-home"
#define EX_CLASS_SECTION_TITLE "ex-section-title"
#define EX_CLASS_TILE "ex-tile"
#define EX_CLASS_PANE "ex-pane"
#define EX_CLASS_DIM "ex-dim"
#define EX_CLASS_COMPACT "ex-compact"
#define EX_CLASS_PLACEHOLDER "ex-placeholder"
#define EX_CLASS_EMPTY "ex-empty"
#define EX_CLASS_TITLEBAR "ex-titlebar"
#define EX_CLASS_CAPTION "ex-caption"
#define EX_CLASS_SUBTLE "ex-subtle"

/* Glyphs drawn as lines rather than taken from the icon theme: the window's
 * own caption buttons and the tab row's small marks, in the same geometry
 * as the window manager's title bars, whatever the theme. */
typedef enum {
	EX_GLYPH_MINIMIZE,
	EX_GLYPH_MAXIMIZE,
	EX_GLYPH_RESTORE,
	EX_GLYPH_CLOSE,
	EX_GLYPH_PLUS,
} ExGlyph;

/* Once per process. */
void ex_style_init(void);
/* A dialog keeps the size its contents give it: not resizable, and no
 * maximise or minimise -- the window manager is told so in both ways it
 * understands (fixed size hints, and Motif functions of move and close). */
void ex_style_fixed_dialog(GtkWidget *dialog);
void ex_style_add(GtkWidget *w, const char *klass);

/* A picture from the icon theme at `px' pixels: `name' (one of the LikeOS
 * line icons, "likeos-cut" and so on), or `fallback' under a theme that has
 * no such icon. */
GtkWidget *ex_style_icon(const char *name, const char *fallback, int px);
GIcon *ex_style_gicon(const char *name, const char *fallback);
void ex_style_set_icon(GtkImage *img, const char *name, const char *fallback,
		       int px);
/* A `size'-pixel glyph in the text colour of whatever holds it (so a
 * button's hover and pressed colours apply), changed with
 * ex_style_set_glyph(). */
GtkWidget *ex_style_glyph(ExGlyph glyph, int size);
void ex_style_set_glyph(GtkWidget *glyph, ExGlyph kind);
/* A flat button holding only `child', named `tip' for the tooltip and for
 * assistive technology, triggering `action' (detailed name, or NULL). */
GtkWidget *ex_style_icon_button(GtkWidget *child, const char *tip,
				const char *action);

G_END_DECLS

#endif
