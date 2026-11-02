// LikeOS -- File Explorer: settings.
//
// GSettings (schema org.likeos.Explorer) when the schema is installed -- on
// LikeOS through the keyfile backend, ~/.config/glib-2.0/settings/keyfile.
// Without it -- a run from the build tree -- the same keys live in memory
// with the same defaults, rather than GSettings aborting the program.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_SETTINGS_H
#define EX_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define EX_TYPE_SETTINGS (ex_settings_get_type())
G_DECLARE_FINAL_TYPE(ExSettings, ex_settings, EX, SETTINGS, GObject)

/* "changed::<key>" (const char *key) when a key changes, from here or from
 * another Explorer process. */
ExSettings *ex_settings_get(void);

gboolean ex_settings_get_bool(ExSettings *s, const char *key);
void ex_settings_set_bool(ExSettings *s, const char *key, gboolean v);
int ex_settings_get_int(ExSettings *s, const char *key);
void ex_settings_set_int(ExSettings *s, const char *key, int v);
char *ex_settings_get_string(ExSettings *s, const char *key);
void ex_settings_set_string(ExSettings *s, const char *key, const char *v);
char **ex_settings_get_strv(ExSettings *s, const char *key);
void ex_settings_set_strv(ExSettings *s, const char *key,
			  const char *const *v);
void ex_settings_get_pair(ExSettings *s, const char *key, int *a, int *b);
void ex_settings_set_pair(ExSettings *s, const char *key, int a, int b);

/* folder-view-modes: the view for a location id, or the default. */
char *ex_settings_view_for(ExSettings *s, const char *location_id);
void ex_settings_set_view_for(ExSettings *s, const char *location_id,
			      const char *mode);

/* favorites */
gboolean ex_settings_is_favorite(ExSettings *s, GFile *f);
void ex_settings_set_favorite(ExSettings *s, GFile *f, gboolean on);

G_END_DECLS

#endif
