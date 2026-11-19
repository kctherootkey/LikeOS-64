/* libinstaller -- the install languages and formats on offer.
 *
 * LikeOS speaks English for now; the formats differ in how dates and times
 * are written (LC_TIME).  The list is data so that a language added later
 * shows up in every front end at once.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <string.h>
#include <strings.h>

#include "il_internal.h"


static const struct {
	const char *name;
	const char *label;
} g_locales[] = {
	{ "en_US.UTF-8", "English (United States)" },
	{ "en_GB.UTF-8", "English (United Kingdom)" },
};

#define IL_NLOCALES ((int)(sizeof(g_locales) / sizeof(g_locales[0])))

int il_locale_count(void)
{
	return IL_NLOCALES;
}

const char *il_locale_name(int i)
{
	if (i < 0 || i >= IL_NLOCALES)
		return NULL;
	return g_locales[i].name;
}

const char *il_locale_label(int i)
{
	if (i < 0 || i >= IL_NLOCALES)
		return NULL;
	return g_locales[i].label;
}

int il_locale_find(const char *name)
{
	if (!name)
		return -1;
	for (int i = 0; i < IL_NLOCALES; i++)
		if (!strcmp(g_locales[i].name, name))
			return i;
	/* "en_US.utf8" and "en_US" name the same thing. */
	for (int i = 0; i < IL_NLOCALES; i++) {
		size_t n = strcspn(g_locales[i].name, ".");

		if (!strncmp(g_locales[i].name, name, n) &&
		    (name[n] == '\0' || !strcasecmp(name + n, ".utf8") ||
		     !strcasecmp(name + n, ".UTF-8")))
			return i;
	}
	return -1;
}
