// LikeOS -- File Explorer: the look.
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

/* Once per process. */
void ex_style_init(void);
/* A dialog keeps the size its contents give it: not resizable, and no
 * maximise or minimise -- the window manager is told so in both ways it
 * understands (fixed size hints, and Motif functions of move and close). */
void ex_style_fixed_dialog(GtkWidget *dialog);
void ex_style_add(GtkWidget *w, const char *klass);

G_END_DECLS

#endif
