/* LikeOS -- the keyboard setting and the layout catalogue, as the desktop's
 * programs read them (see keyboard-conf.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "keyboard-conf.h"

#include <string.h>

static void layout_free(gpointer p)
{
	struct kbd_layout *l = p;

	g_free(l->id);
	g_free(l->name);
	g_free(l->language);
	g_free(l->layout);
	g_free(l->variant);
	g_free(l);
}

GPtrArray *kbd_catalogue_load(void)
{
	GPtrArray *cat = g_ptr_array_new_with_free_func(layout_free);
	char *text = NULL;

	if (!g_file_get_contents(KBD_CATALOGUE, &text, NULL, NULL))
		return cat;

	char **lines = g_strsplit(text, "\n", -1);

	for (char **ln = lines; *ln; ln++) {
		char *line = *ln;
		size_t len = strlen(line);

		if (len && line[len - 1] == '\r')
			line[len - 1] = '\0';
		if (!line[0] || line[0] == '#')
			continue;

		/* id, name, language and the XKB layout are needed; the
		 * variant may be empty, and its tab may even be missing. */
		char **f = g_strsplit(line, "\t", 6);

		if (g_strv_length(f) >= 4 && f[0][0] && f[1][0] && f[3][0]) {
			struct kbd_layout *l = g_new0(struct kbd_layout, 1);

			l->id = g_strdup(f[0]);
			l->name = g_strdup(f[1]);
			l->language = g_strdup(f[2][0] ? f[2] : f[1]);
			l->layout = g_strdup(f[3]);
			l->variant = g_strdup(f[4] ? f[4] : "");
			g_ptr_array_add(cat, l);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(text);
	return cat;
}

const struct kbd_layout *kbd_catalogue_find(const GPtrArray *cat,
					    const char *layout,
					    const char *variant)
{
	if (!variant)
		variant = "";
	for (guint i = 0; cat && i < cat->len; i++) {
		const struct kbd_layout *l = g_ptr_array_index(cat, i);

		if (strcmp(l->layout, layout) == 0 &&
		    strcmp(l->variant, variant) == 0)
			return l;
	}
	return NULL;
}

gboolean kbd_layout_nonlatin(const GPtrArray *cat, const char *layout,
			     const char *variant)
{
	const struct kbd_layout *l = kbd_catalogue_find(cat, layout, variant);
	char *text = NULL;
	gboolean hit = FALSE;

	if (!l || !g_file_get_contents(KBD_NONLATIN, &text, NULL, NULL))
		return FALSE;

	char **lines = g_strsplit(text, "\n", -1);

	for (char **ln = lines; *ln && !hit; ln++)
		hit = strcmp(g_strstrip(*ln), l->id) == 0;
	g_strfreev(lines);
	g_free(text);
	return hit;
}

/* The value of a shell assignment's right-hand side: quoted in "" or '', or
 * a bare word, which ends at white space (a comment may follow). */
static char *shell_value(const char *v)
{
	if (*v == '"' || *v == '\'') {
		const char *end = strchr(v + 1, *v);

		return end ? g_strndup(v + 1, end - v - 1) : g_strdup(v + 1);
	}

	size_t n = strcspn(v, " \t#");

	return g_strndup(v, n);
}

/* A comma list split into at most KBD_MAX_LAYOUTS entries, each trimmed. */
static int split_list(const char *s, char **out)
{
	char **parts = g_strsplit(s ? s : "", ",", -1);
	int n = 0;

	for (char **p = parts; *p && n < KBD_MAX_LAYOUTS; p++)
		out[n++] = g_strstrip(g_strdup(*p));
	g_strfreev(parts);
	return n;
}

void kbd_conf_read(struct kbd_conf *c)
{
	char *path = g_build_filename(KBD_CONF_DIR, KBD_CONF_NAME, NULL);
	char *text = NULL, *layout = NULL, *variant = NULL, *options = NULL;

	memset(c, 0, sizeof(*c));
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (char **ln = lines; *ln; ln++) {
			const char *s = *ln;
			char **slot = NULL;

			while (*s == ' ' || *s == '\t')
				s++;
			if (g_str_has_prefix(s, "export "))
				s += 7;
			if (g_str_has_prefix(s, "LAYOUT="))
				slot = &layout, s += 7;
			else if (g_str_has_prefix(s, "VARIANT="))
				slot = &variant, s += 8;
			else if (g_str_has_prefix(s, "OPTIONS="))
				slot = &options, s += 8;
			if (slot) {
				g_free(*slot);
				*slot = shell_value(s);
			}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);

	char *v[KBD_MAX_LAYOUTS] = { 0 };
	int nv;

	c->n = split_list(layout, c->layout);
	nv = split_list(variant, v);
	/* Empty names are not layouts: drop them with their variants. */
	int k = 0;

	for (int i = 0; i < c->n; i++) {
		if (!c->layout[i][0]) {
			g_free(c->layout[i]);
			g_free(i < nv ? v[i] : NULL);
			continue;
		}
		c->layout[k] = c->layout[i];
		c->variant[k] = i < nv ? v[i] : g_strdup("");
		k++;
	}
	for (int i = c->n; i < nv; i++)
		g_free(v[i]);
	c->n = k;
	if (c->n == 0) {
		c->layout[0] = g_strdup("us");
		c->variant[0] = g_strdup("");
		c->n = 1;
	}
	c->options = options ? options : g_strdup("");
	g_free(layout);
	g_free(variant);
}

void kbd_conf_clear(struct kbd_conf *c)
{
	for (int i = 0; i < c->n; i++) {
		g_free(c->layout[i]);
		g_free(c->variant[i]);
	}
	g_free(c->options);
	memset(c, 0, sizeof(*c));
}

char *kbd_layout_label(const GPtrArray *cat, const char *layout,
		       const char *variant)
{
	const struct kbd_layout *l = kbd_catalogue_find(cat, layout, variant);

	if (l)
		return g_strdup(l->name);
	if (variant && *variant)
		return g_strdup_printf("%s (%s)", layout, variant);
	return g_strdup(layout);
}

char *kbd_conf_summary(const GPtrArray *cat, const struct kbd_conf *c)
{
	char *first = kbd_layout_label(cat, c->layout[0], c->variant[0]);
	char *s;

	if (c->n <= 1)
		return first;
	s = g_strdup_printf("%s, +%d more", first, c->n - 1);
	g_free(first);
	return s;
}
