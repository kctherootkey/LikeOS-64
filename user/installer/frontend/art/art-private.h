/* LikeOS installer -- the painters behind art_draw().
 *
 * Every painter draws into the unit square (0,0)-(1,1); art_draw() has
 * already translated and scaled the context, so all lengths here are
 * fractions of the illustration's size.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_ART_PRIVATE_H
#define LIKEOS_INSTALLER_ART_PRIVATE_H

#include "art.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void art_paint_logo(cairo_t *cr, double phase);
void art_paint_globe(cairo_t *cr, double phase);
void art_paint_keyboard(cairo_t *cr, double phase);
void art_paint_keyboard_plus(cairo_t *cr, double phase);
void art_paint_computer(cairo_t *cr, double phase);
void art_paint_shield(cairo_t *cr, double phase);
void art_paint_avatar(cairo_t *cr, double phase);
void art_paint_avatar_key(cairo_t *cr, double phase);
void art_paint_disk(cairo_t *cr, double phase);
void art_paint_success(cairo_t *cr, double phase);
void art_paint_warning(cairo_t *cr, double phase);
void art_paint_shapes(cairo_t *cr, double phase);

/* A person: head and shoulders inside a circle of radius r at (cx, cy). */
void art_person(cairo_t *cr, double cx, double cy, double r, guint32 fg,
		double alpha);
/* A smooth ease: 0..1 -> 0..1 */
double art_ease(double t);
/* A float offset from the phase (a slow bob). */
double art_bob(double phase, double amount, double shift);

#endif
