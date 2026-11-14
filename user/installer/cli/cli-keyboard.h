/* likeos-install -- the keyboard layout catalogue.
 *
 * /usr/share/likeos/keyboard/layouts.tsv lists every layout LikeOS has, one
 * per line, five TAB-separated fields:
 *
 *   id  name  language  xkb_layout  xkb_variant
 *
 * (id is the console keymap loadkeys takes).  Read here directly, without
 * any toolkit, so the text-mode installer works on a bare console.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALL_CLI_KEYBOARD_H
#define LIKEOS_INSTALL_CLI_KEYBOARD_H

#include <stddef.h>

#define KBD_CATALOGUE "/usr/share/likeos/keyboard/layouts.tsv"

struct kbd_layout {
	char id[48];
	char name[96];
	char language[96];
	char layout[32];
	char variant[32];
};

struct kbd_catalogue {
	struct kbd_layout *l;
	int n;
};

/* Reads `path' (NULL: KBD_CATALOGUE; the environment variable
 * LIKEOS_KEYBOARD_CATALOGUE overrides it for tests).  0 or -errno. */
int kbd_load(struct kbd_catalogue *c, const char *path);
void kbd_free(struct kbd_catalogue *c);

/* The layout with XKB layout/variant, or console id `id'; -1 when none. */
int kbd_find_xkb(const struct kbd_catalogue *c, const char *layout,
		 const char *variant);
int kbd_find_id(const struct kbd_catalogue *c, const char *id);

/* Indexes of the layouts whose name, language, id or XKB name contain
 * `text' (any case), at most `max' of them, in catalogue order; exact id
 * matches come first.  Returns how many matched in all (may exceed max). */
int kbd_search(const struct kbd_catalogue *c, const char *text, int *out,
	       int max);

/* "German (no dead keys)" for layout/variant, or "de(nodeadkeys)" when the
 * catalogue does not know it. */
void kbd_describe(const struct kbd_catalogue *c, const char *layout,
		  const char *variant, char *buf, size_t len);

#endif /* LIKEOS_INSTALL_CLI_KEYBOARD_H */
