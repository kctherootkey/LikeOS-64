/* LikeOS installer -- illustrations and icons, drawn at run time.
 *
 * Nothing here is a picture file: every illustration of the wizard (the
 * globe, the keyboard, the computer with its name tag, the people, the
 * disk, the shield, the check mark) and every small icon of the toolbar is
 * painted with cairo, so it is sharp at any size and on any screen.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_ART_H
#define LIKEOS_INSTALLER_ART_H

#include <gtk/gtk.h>

typedef enum {
	ART_NONE = 0,
	ART_LOGO,          /* the LikeOS mark: a rounded tile with an orbit */
	ART_GLOBE,         /* globe with a compass needle and a clock badge */
	ART_KEYBOARD,      /* keyboard with a highlighted key */
	ART_KEYBOARD_PLUS, /* two keyboards, a plus badge */
	ART_COMPUTER,      /* monitor showing a name tag */
	ART_SHIELD,        /* shield with a padlock: the root password */
	ART_AVATAR,        /* two people */
	ART_AVATAR_KEY,    /* a person with a key: the user's password */
	ART_DISK,          /* a drive with partitions */
	ART_SUCCESS,       /* a big check in a ring */
	ART_WARNING,       /* a triangle with an exclamation mark */
	ART_SHAPES,        /* floating tiles, for the "one moment" card */
	ART_COUNT
} ArtKind;

/* Paints an illustration into the square (x, y, size).  phase runs from 0
 * to 1 and back over a few seconds; illustrations float gently with it. */
void art_draw(cairo_t *cr, ArtKind kind, double x, double y, double size,
	      double phase);

/* A pixbuf of an illustration (for list rows and dialogs). */
GdkPixbuf *art_pixbuf(ArtKind kind, int size);

/* A drawing area that shows one illustration, cross-fades to a new one when
 * the kind changes and floats slowly while mapped. */
GtkWidget *art_view_new(ArtKind kind);
void art_view_set(GtkWidget *view, ArtKind kind);
ArtKind art_view_get(GtkWidget *view);
void art_view_set_animated(GtkWidget *view, gboolean animated);

/* Whether illustrations and spinners move (off with the environment
 * variable LIKEOS_INSTALLER_NO_ANIMATION=1, or --no-animation). */
gboolean art_animations_enabled(void);
void art_set_animations(gboolean on);

/* ---- small icons --------------------------------------------------- */

typedef enum {
	ICON_NONE = 0,
	ICON_REFRESH,
	ICON_DELETE,
	ICON_FORMAT,
	ICON_NEW,
	ICON_EXTEND,
	ICON_WHOLE_DISK,
	ICON_DETAILS,
	ICON_UNDO,
	ICON_EYE,
	ICON_EYE_OFF,
	ICON_BACK,
	ICON_CLOSE,
	ICON_POWER,
	ICON_KEYBOARD,
	ICON_CHECK,
	ICON_HELP,
	ICON_WARNING,
	ICON_INFO,
	ICON_ERROR,
	ICON_DISK,
	ICON_USB,
	ICON_PARTITION,
	ICON_FREE,
	ICON_SEARCH,
	ICON_RESTART,
	ICON_GLOBE,
	ICON_USER,
	ICON_LOCK,
	ICON_COMPUTER,
	ICON_CLOCK,
	ICON_LANGUAGE,
	ICON_DOT,
	ICON_COUNT
} IconKind;

/* Paints an icon in the square (x, y, size) with the given colour. */
void icon_draw(cairo_t *cr, IconKind kind, double x, double y, double size,
	       const GdkRGBA *color);

/* A widget showing an icon in its style context's text colour (so it
 * greys out with its parent). */
GtkWidget *icon_new(IconKind kind, int size);
void icon_set(GtkWidget *icon, IconKind kind);

/* A pixbuf of an icon in one colour (for entries and tree views). */
GdkPixbuf *icon_pixbuf(IconKind kind, int size, const GdkRGBA *color);

/* ---- helpers shared by the painters --------------------------------- */

void art_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r);
void art_set_rgb(cairo_t *cr, guint32 rgb, double alpha);
/* A soft shadow under a rounded rectangle. */
void art_shadow(cairo_t *cr, double x, double y, double w, double h,
		double r, double blur, double alpha);
/* A soft shadow under a circle. */
void art_shadow_circle(cairo_t *cr, double cx, double cy, double radius,
		       double blur, double alpha);
cairo_pattern_t *art_linear(double x0, double y0, double x1, double y1,
			    guint32 c0, guint32 c1);

/* The installer palette. */
#define ART_ACCENT        0x2a5bd7
#define ART_ACCENT_LIGHT  0x5b8ff9
#define ART_ACCENT_DEEP   0x1a3f9e
#define ART_TEAL          0x17b8a6
#define ART_TEAL_LIGHT    0x6fe0cf
#define ART_VIOLET        0x6a5ae0
#define ART_SKY           0x8fc3ff
#define ART_INK           0x141a24
#define ART_GREY          0xc9d1de
#define ART_GREY_LIGHT    0xeef2f8
#define ART_WHITE         0xffffff
#define ART_SUCCESS_COL   0x1f9d55
#define ART_WARNING_COL   0xd98a00
#define ART_ERROR_COL     0xd63a3a

#endif
