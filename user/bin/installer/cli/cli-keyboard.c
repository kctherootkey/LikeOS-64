/* likeos-install -- the keyboard layout catalogue.  See cli-keyboard.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "cli-keyboard.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Field `idx' of a TAB-separated line into out; 0 when the line has no
 * such field or it does not fit. */
static int kbd_field(const char *line, int idx, char *out, size_t cap)
{
	const char *p = line;
	size_t n;

	for (int i = 0; i < idx; i++) {
		p = strchr(p, '\t');
		if (!p)
			return 0;
		p++;
	}
	n = strcspn(p, "\t\r\n");
	if (n >= cap)
		return 0;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

int kbd_load(struct kbd_catalogue *c, const char *path)
{
	char line[512];
	const char *env = getenv("LIKEOS_KEYBOARD_CATALOGUE");
	FILE *f;
	int cap = 0;

	memset(c, 0, sizeof(*c));
	if (!path)
		path = env && env[0] ? env : KBD_CATALOGUE;
	f = fopen(path, "r");
	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		struct kbd_layout k;

		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		memset(&k, 0, sizeof(k));
		if (!kbd_field(line, 0, k.id, sizeof(k.id)) ||
		    !kbd_field(line, 1, k.name, sizeof(k.name)) ||
		    !kbd_field(line, 2, k.language, sizeof(k.language)) ||
		    !kbd_field(line, 3, k.layout, sizeof(k.layout)))
			continue;
		/* An empty variant may lack its field altogether. */
		if (!kbd_field(line, 4, k.variant, sizeof(k.variant)))
			k.variant[0] = '\0';
		if (!k.id[0] || !k.layout[0])
			continue;
		if (c->n == cap) {
			int nc = cap ? cap * 2 : 128;
			struct kbd_layout *nl =
				realloc(c->l, (size_t)nc * sizeof(*nl));

			if (!nl) {
				fclose(f);
				kbd_free(c);
				return -ENOMEM;
			}
			c->l = nl;
			cap = nc;
		}
		c->l[c->n++] = k;
	}
	fclose(f);
	return c->n ? 0 : -ENOENT;
}

void kbd_free(struct kbd_catalogue *c)
{
	free(c->l);
	memset(c, 0, sizeof(*c));
}

int kbd_find_xkb(const struct kbd_catalogue *c, const char *layout,
		 const char *variant)
{
	for (int i = 0; i < c->n; i++)
		if (!strcmp(c->l[i].layout, layout) &&
		    !strcmp(c->l[i].variant, variant ? variant : ""))
			return i;
	return -1;
}

int kbd_find_id(const struct kbd_catalogue *c, const char *id)
{
	for (int i = 0; i < c->n; i++)
		if (!strcasecmp(c->l[i].id, id))
			return i;
	return -1;
}

static int kbd_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return 1;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

int kbd_search(const struct kbd_catalogue *c, const char *text, int *out,
	       int max)
{
	int total = 0, k = 0;
	int exact = kbd_find_id(c, text);

	if (exact >= 0) {
		if (k < max)
			out[k++] = exact;
		total++;
	}
	for (int i = 0; i < c->n; i++) {
		const struct kbd_layout *l = &c->l[i];

		if (i == exact)
			continue;
		if (kbd_contains(l->name, text) ||
		    kbd_contains(l->language, text) ||
		    kbd_contains(l->id, text) || kbd_contains(l->layout, text)) {
			if (k < max)
				out[k++] = i;
			total++;
		}
	}
	return total;
}

void kbd_describe(const struct kbd_catalogue *c, const char *layout,
		  const char *variant, char *buf, size_t len)
{
	int i = kbd_find_xkb(c, layout, variant);

	if (i >= 0)
		snprintf(buf, len, "%s", c->l[i].name);
	else if (variant && variant[0])
		snprintf(buf, len, "%s(%s)", layout, variant);
	else
		snprintf(buf, len, "%s", layout);
}
