/* LikeOS installer -- the keyboard layouts on offer and trying them out.
 *
 * The catalogue is the system's own (layouts.tsv, read through
 * keyboard-conf.c, the code the Keyboard settings window uses).  Choosing a
 * layout switches the running X session to it at once with setxkbmap, so
 * the person types every following answer -- the passwords above all --
 * with the layout the installed system will have.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_KEYBOARD_H
#define LIKEOS_INSTALLER_KEYBOARD_H

#include "app.h"
#include "keyboard-conf.h"

/* Loads the catalogue into app->kbd_cat (an empty array when missing;
 * then a small built-in list is used). */
void keyboard_load(App *app);
int keyboard_count(App *app);
const struct kbd_layout *keyboard_get(App *app, int i);
const struct kbd_layout *keyboard_find(App *app, const char *layout,
				       const char *variant);
/* "de" or "de:nodeadkeys" -> catalogue entry. */
const struct kbd_layout *keyboard_find_key(App *app, const char *key);
/* The key used in lists: "layout" or "layout:variant"; g_free. */
char *keyboard_key(const struct kbd_layout *l);

/* A sensible first layout for a locale (en_GB -> gb). */
const char *keyboard_default_for_locale(const char *locale);

/* Applies app->cfg's layouts to the X session (asynchronously). */
void keyboard_apply(App *app);
/* Applies just one layout (trying a layout before adding it). */
void keyboard_apply_one(App *app, const char *layout, const char *variant);

/* "US", "DE": the short code for the status tray. */
char *keyboard_short_code(const char *layout);

#endif
