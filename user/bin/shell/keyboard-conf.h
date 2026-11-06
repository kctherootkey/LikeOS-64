/* LikeOS -- the keyboard setting and the layout catalogue, as the desktop's
 * programs read them.
 *
 * Three programs show the keyboard setting: the Keyboard settings window
 * (likeos-keyboard-settings), which edits it, the Settings window
 * (likeos-settings), whose Keyboard card names the layout in use, and the
 * taskbar, whose Control Center has a Keyboard row.  This is the one reading
 * of the two files they share:
 *
 *  - /etc/likeos/keyboard.conf, the setting: shell assignments of LAYOUT,
 *    VARIANT and OPTIONS, each a comma list in XKB's terms (LAYOUT=de,us
 *    VARIANT=nodeadkeys, OPTIONS=grp:alt_shift_toggle).  The first layout is
 *    the primary one, which the console uses too.  Written only by the
 *    setuid helper likeos-keyboard-save, always by renaming a new file over
 *    the old one.
 *  - /usr/share/likeos/keyboard/layouts.tsv, the catalogue of the layouts
 *    the system offers: one line per layout, tab-separated
 *    id, name, language, XKB layout, XKB variant (empty when none), sorted
 *    by name; # starts a comment.
 *  - /usr/share/likeos/keyboard/nonlatin.list, the ids of the catalogue's
 *    layouts that have no Latin letters, one per line.  Such a layout, when
 *    it is the only one set, is given English (US) as a second one in the X
 *    session (likeos-xkb-apply) and on the console, so that a web address or
 *    a command can still be typed.
 *
 * The paths are compile-time defaults, so a test build can point them at
 * files of its own.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_KEYBOARD_CONF_H
#define _LIKEOS_KEYBOARD_CONF_H

#include <glib.h>

#ifndef KBD_CONF_DIR
#define KBD_CONF_DIR "/etc/likeos"
#endif
#define KBD_CONF_NAME "keyboard.conf"
#ifndef KBD_CATALOGUE
#define KBD_CATALOGUE "/usr/share/likeos/keyboard/layouts.tsv"
#endif
#ifndef KBD_NONLATIN
#define KBD_NONLATIN "/usr/share/likeos/keyboard/nonlatin.list"
#endif

/* XKB switches between at most four layouts (its groups). */
#define KBD_MAX_LAYOUTS 4

/* One line of the catalogue. */
struct kbd_layout {
	char *id;	/* the console keymap's name: de, de-nodeadkeys */
	char *name;	/* what a person reads: German (no dead keys) */
	char *language;	/* German */
	char *layout;	/* XKB's layout and variant: de, nodeadkeys */
	char *variant;	/* "" when none */
};

/* The setting: n layouts, each with its variant ("" for none), and the XKB
 * options as one comma list. */
struct kbd_conf {
	int n;
	char *layout[KBD_MAX_LAYOUTS];
	char *variant[KBD_MAX_LAYOUTS];
	char *options;
};

/* The catalogue as an array of struct kbd_layout *, in the file's order; an
 * empty array when it cannot be read.  Unreferenced with
 * g_ptr_array_unref(). */
GPtrArray *kbd_catalogue_load(void);

/* The catalogue's line for an XKB layout and variant, or NULL. */
const struct kbd_layout *kbd_catalogue_find(const GPtrArray *cat,
					    const char *layout,
					    const char *variant);

/* Whether a layout has no Latin letters (nonlatin.list); FALSE for one the
 * catalogue does not list. */
gboolean kbd_layout_nonlatin(const GPtrArray *cat, const char *layout,
			     const char *variant);

/* Read the setting.  A file that is missing or names no layout reads as
 * the default, English (US): LAYOUT=us and nothing else. */
void kbd_conf_read(struct kbd_conf *c);
void kbd_conf_clear(struct kbd_conf *c);

/* What to call a layout: its catalogue name, or for one the catalogue does
 * not list, its XKB name ("xx (variant)").  Freed with g_free(). */
char *kbd_layout_label(const GPtrArray *cat, const char *layout,
		       const char *variant);

/* The setting in a few words, for a status line: the primary layout's name,
 * and how many more there are ("German, +1 more").  Freed with g_free(). */
char *kbd_conf_summary(const GPtrArray *cat, const struct kbd_conf *c);

#endif
