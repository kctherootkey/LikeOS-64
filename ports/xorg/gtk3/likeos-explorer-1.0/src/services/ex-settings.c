// LikeOS Explorer: settings.
//
// Copyright (C) 2026 The LikeOS Project

#include "services/ex-settings.h"

#include <string.h>

#define EX_SCHEMA "org.likeos.Explorer"

struct _ExSettings {
	GObject parent;
	GSettings *gs; /* NULL: the in-memory fallback */
	GHashTable *mem; /* key -> GVariant */
};

G_DEFINE_TYPE(ExSettings, ex_settings, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

/* The defaults, for a run without the schema.  The same as
 * data/org.likeos.Explorer.gschema.xml. */
static const struct {
	const char *key;
	const char *def; /* GVariant text form */
} defaults[] = {
	{ "view-mode", "'details'" },
	{ "folder-view-modes", "@a{ss} {}" },
	{ "sort-column", "'name'" },
	{ "sort-ascending", "true" },
	{ "show-hidden", "false" },
	{ "show-extensions", "true" },
	{ "show-navigation-pane", "true" },
	{ "right-pane", "'none'" },
	{ "favorites", "@as []" },
	{ "show-recent", "true" },
	{ "show-frequent", "true" },
	{ "index-home", "true" },
	{ "single-click", "false" },
	{ "confirm-trash", "false" },
	{ "window-size", "(1100, 680)" },
	{ "window-maximized", "false" },
	{ "sidebar-width", "220" },
	{ "start-location", "'likeos:home'" },
	{ "gallery-folders", "['pictures', 'videos']" },
};

static void ex_settings_class_init(ExSettingsClass *k)
{
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
					 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
					 G_TYPE_STRING);
}

static void ex_settings_init(ExSettings *s)
{
}

static void gs_changed(GSettings *gs, const char *key, ExSettings *s)
{
	g_signal_emit(s, sigs[SIG_CHANGED], g_quark_from_string(key), key);
}

ExSettings *ex_settings_get(void)
{
	static ExSettings *the;

	if (the)
		return the;
	the = g_object_new(EX_TYPE_SETTINGS, NULL);

	GSettingsSchemaSource *src = g_settings_schema_source_get_default();
	GSettingsSchema *schema = src ? g_settings_schema_source_lookup(
						src, EX_SCHEMA, TRUE) :
					NULL;
	/* A run from the build tree may point at its compiled schema. */
	const char *dir = g_getenv("EXPLORER_SCHEMA_DIR");
	if (!schema && dir) {
		GSettingsSchemaSource *own =
			g_settings_schema_source_new_from_directory(dir, src,
								    FALSE, NULL);
		if (own) {
			schema = g_settings_schema_source_lookup(own, EX_SCHEMA,
								 FALSE);
			g_settings_schema_source_unref(own);
		}
	}
	if (schema) {
		the->gs = g_settings_new_full(schema, NULL, NULL);
		g_settings_schema_unref(schema);
		g_signal_connect(the->gs, "changed", G_CALLBACK(gs_changed),
				 the);
		return the;
	}
	g_warning("settings schema " EX_SCHEMA
		  " is not installed: settings are not kept");
	the->mem = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					 (GDestroyNotify)g_variant_unref);
	for (gsize i = 0; i < G_N_ELEMENTS(defaults); i++) {
		GVariant *v = g_variant_parse(NULL, defaults[i].def, NULL, NULL,
					      NULL);

		g_hash_table_insert(the->mem, (gpointer)defaults[i].key,
				    g_variant_ref_sink(v));
	}
	return the;
}

static GVariant *get(ExSettings *s, const char *key)
{
	if (s->gs)
		return g_settings_get_value(s->gs, key);
	GVariant *v = g_hash_table_lookup(s->mem, key);

	g_return_val_if_fail(v != NULL, NULL);
	return g_variant_ref(v);
}

static void set(ExSettings *s, const char *key, GVariant *v)
{
	g_variant_ref_sink(v);
	if (s->gs) {
		g_settings_set_value(s->gs, key, v);
	} else {
		const char *k = NULL;

		for (gsize i = 0; i < G_N_ELEMENTS(defaults); i++)
			if (strcmp(defaults[i].key, key) == 0)
				k = defaults[i].key;
		g_return_if_fail(k != NULL);
		g_hash_table_insert(s->mem, (gpointer)k, g_variant_ref(v));
		g_signal_emit(s, sigs[SIG_CHANGED], g_quark_from_string(k), k);
	}
	g_variant_unref(v);
}

gboolean ex_settings_get_bool(ExSettings *s, const char *key)
{
	GVariant *v = get(s, key);
	gboolean b = v ? g_variant_get_boolean(v) : FALSE;

	if (v)
		g_variant_unref(v);
	return b;
}

void ex_settings_set_bool(ExSettings *s, const char *key, gboolean v)
{
	set(s, key, g_variant_new_boolean(v));
}

int ex_settings_get_int(ExSettings *s, const char *key)
{
	GVariant *v = get(s, key);
	int i = v ? g_variant_get_int32(v) : 0;

	if (v)
		g_variant_unref(v);
	return i;
}

void ex_settings_set_int(ExSettings *s, const char *key, int v)
{
	set(s, key, g_variant_new_int32(v));
}

char *ex_settings_get_string(ExSettings *s, const char *key)
{
	GVariant *v = get(s, key);
	char *str = v ? g_variant_dup_string(v, NULL) : g_strdup("");

	if (v)
		g_variant_unref(v);
	return str;
}

void ex_settings_set_string(ExSettings *s, const char *key, const char *v)
{
	set(s, key, g_variant_new_string(v ? v : ""));
}

char **ex_settings_get_strv(ExSettings *s, const char *key)
{
	GVariant *v = get(s, key);
	char **sv = v ? g_variant_dup_strv(v, NULL) : g_new0(char *, 1);

	if (v)
		g_variant_unref(v);
	return sv;
}

void ex_settings_set_strv(ExSettings *s, const char *key,
			  const char *const *v)
{
	set(s, key, g_variant_new_strv(v, -1));
}

void ex_settings_get_pair(ExSettings *s, const char *key, int *a, int *b)
{
	GVariant *v = get(s, key);

	*a = *b = 0;
	if (v) {
		g_variant_get(v, "(ii)", a, b);
		g_variant_unref(v);
	}
}

void ex_settings_set_pair(ExSettings *s, const char *key, int a, int b)
{
	set(s, key, g_variant_new("(ii)", a, b));
}

char *ex_settings_view_for(ExSettings *s, const char *location_id)
{
	GVariant *v = get(s, "folder-view-modes");
	const char *mode = NULL;
	char *out;

	if (v && location_id)
		g_variant_lookup(v, location_id, "&s", &mode);
	out = mode ? g_strdup(mode) : ex_settings_get_string(s, "view-mode");
	if (v)
		g_variant_unref(v);
	return out;
}

void ex_settings_set_view_for(ExSettings *s, const char *location_id,
			      const char *mode)
{
	GVariant *v = get(s, "folder-view-modes");
	GVariantBuilder b;
	GVariantIter it;
	const char *k, *m;
	int n = 0;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
	/* The newest choices are kept; a long-lived account does not grow the
	 * map without end. */
	g_variant_iter_init(&it, v);
	while (g_variant_iter_next(&it, "{&s&s}", &k, &m))
		if (strcmp(k, location_id) != 0 && n++ < 499)
			g_variant_builder_add(&b, "{ss}", k, m);
	g_variant_builder_add(&b, "{ss}", location_id, mode);
	g_variant_unref(v);
	set(s, "folder-view-modes", g_variant_builder_end(&b));
}

gboolean ex_settings_is_favorite(ExSettings *s, GFile *f)
{
	char **fav = ex_settings_get_strv(s, "favorites");
	char *uri = g_file_get_uri(f);
	gboolean on = g_strv_contains((const char *const *)fav, uri);

	g_free(uri);
	g_strfreev(fav);
	return on;
}

void ex_settings_set_favorite(ExSettings *s, GFile *f, gboolean on)
{
	char **fav = ex_settings_get_strv(s, "favorites");
	char *uri = g_file_get_uri(f);
	GPtrArray *out = g_ptr_array_new();

	for (int i = 0; fav[i]; i++)
		if (strcmp(fav[i], uri) != 0)
			g_ptr_array_add(out, fav[i]);
	if (on)
		g_ptr_array_insert(out, 0, uri);
	g_ptr_array_add(out, NULL);
	ex_settings_set_strv(s, "favorites", (const char *const *)out->pdata);
	g_ptr_array_unref(out);
	g_free(uri);
	g_strfreev(fav);
}
