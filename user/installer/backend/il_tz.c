/* libinstaller -- time zones.
 *
 * The time zones are exactly the ones the C library knows: LikeOS carries
 * no zone database, its libc maps a name to a POSIX TZ rule from a built-in
 * table (user/lib/libc/src/time/time.c).  That table is repeated here, rule
 * for rule, so the list a person picks from holds nothing the installed
 * system would not understand; keep the two in step when a zone is added.
 *
 * The list is sorted the way people look for their zone: by the offset
 * from UTC, then by city.  Labels read "(UTC+01:00) Berlin"; the offset is
 * the standard (winter) one of the rule.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_internal.h"

static const struct {
	const char *name;
	const char *rule;
} g_tz_table[] = {
	{ "UTC", "UTC0" },
	{ "Etc/UTC", "UTC0" },
	{ "GMT", "UTC0" },
	{ "Etc/GMT", "UTC0" },
	/* central Europe */
	{ "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Luxembourg", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zagreb", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Bratislava", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Ljubljana", "CET-1CEST,M3.5.0,M10.5.0/3" },
	/* western Europe */
	{ "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
	/* eastern Europe */
	{ "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kiev", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Istanbul", "<+03>-3" },
	{ "Europe/Moscow", "MSK-3" },
	/* the Americas */
	{ "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Phoenix", "MST7" },
	{ "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "America/Sao_Paulo", "<-03>3" },
	{ "America/Mexico_City", "CST6" },
	{ "America/Bogota", "<-05>5" },
	{ "America/Buenos_Aires", "<-03>3" },
	{ "America/Argentina/Buenos_Aires", "<-03>3" },
	/* Asia and Oceania */
	{ "Asia/Tokyo", "JST-9" },
	{ "Asia/Seoul", "KST-9" },
	{ "Asia/Shanghai", "CST-8" },
	{ "Asia/Hong_Kong", "HKT-8" },
	{ "Asia/Singapore", "<+08>-8" },
	{ "Asia/Kolkata", "IST-5:30" },
	{ "Asia/Calcutta", "IST-5:30" },
	{ "Asia/Dubai", "<+04>-4" },
	{ "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Brisbane", "AEST-10" },
	{ "Australia/Perth", "AWST-8" },
	{ "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Africa/Johannesburg", "SAST-2" },
	{ "Africa/Lagos", "WAT-1" },
};

#define IL_NZONES ((int)(sizeof(g_tz_table) / sizeof(g_tz_table[0])))

struct il_zone {
	int index;          /* into g_tz_table */
	int east_minutes;   /* standard offset east of UTC */
	char label[96];
};

static struct il_zone g_zones[IL_NZONES];
static volatile int g_zones_ready;

/* The standard offset of a POSIX rule ("CET-1CEST,...", "<+03>-3",
 * "IST-5:30"), east of UTC in minutes: POSIX gives the value to add to
 * local time to get UTC, so the sign is flipped. */
static int il_rule_offset(const char *rule)
{
	const char *p = rule;
	int sign = 1, h = 0, m = 0, s = 0;

	if (*p == '<') {
		while (*p && *p != '>')
			p++;
		if (*p == '>')
			p++;
	} else {
		while (isalpha((unsigned char)*p))
			p++;
	}
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		sign = -1;
		p++;
	}
	while (isdigit((unsigned char)*p))
		h = h * 10 + (*p++ - '0');
	if (*p == ':') {
		p++;
		while (isdigit((unsigned char)*p))
			m = m * 10 + (*p++ - '0');
		if (*p == ':') {
			p++;
			while (isdigit((unsigned char)*p))
				s = s * 10 + (*p++ - '0');
		}
	}
	return -sign * (h * 60 + m + s / 60);
}

/* "Europe/Berlin" -> "Berlin", "America/Argentina/Buenos_Aires" ->
 * "Buenos Aires". */
static void il_city(const char *name, char *out, size_t len)
{
	const char *base = strrchr(name, '/');
	size_t n = 0;

	base = base ? base + 1 : name;
	for (const char *p = base; *p && n + 1 < len; p++)
		out[n++] = *p == '_' ? ' ' : *p;
	out[n] = '\0';
}

static int il_zone_cmp(const void *a, const void *b)
{
	const struct il_zone *x = a, *y = b;
	int c;

	if (x->east_minutes != y->east_minutes)
		return x->east_minutes < y->east_minutes ? -1 : 1;
	c = strcmp(x->label, y->label);
	if (c)
		return c;
	return x->index - y->index;
}

static void il_zones_init(void)
{
	if (g_zones_ready)
		return;
	for (int i = 0; i < IL_NZONES; i++) {
		struct il_zone *z = &g_zones[i];
		const char *name = g_tz_table[i].name;
		char city[64], off[16];
		int e = il_rule_offset(g_tz_table[i].rule);
		int a = e < 0 ? -e : e;

		z->index = i;
		z->east_minutes = e;
		snprintf(off, sizeof(off), "UTC%c%02d:%02d", e < 0 ? '-' : '+',
			 a / 60, a % 60);
		if (!strcmp(name, "UTC")) {
			snprintf(z->label, sizeof(z->label),
				 "(UTC) Coordinated Universal Time");
		} else if (!strchr(name, '/')) {
			snprintf(z->label, sizeof(z->label), "(UTC) %s", name);
		} else if (il_starts_with(name, "Etc/")) {
			snprintf(z->label, sizeof(z->label), "(UTC) %s", name);
		} else {
			il_city(name, city, sizeof(city));
			snprintf(z->label, sizeof(z->label), "(%s) %s", off, city);
		}
	}
	/* Old spellings of a zone (Europe/Kiev, Asia/Calcutta) and cities in
	 * two places of the database get their full name added, so no two
	 * labels are alike. */
	for (int i = 0; i < IL_NZONES; i++)
		for (int j = 0; j < i; j++)
			if (!strcmp(g_zones[i].label, g_zones[j].label)) {
				char tmp[96];

				snprintf(tmp, sizeof(tmp), "%.60s (%s)",
					 g_zones[i].label,
					 g_tz_table[g_zones[i].index].name);
				il_strlcpy(g_zones[i].label, tmp,
					   sizeof(g_zones[i].label));
				break;
			}
	qsort(g_zones, IL_NZONES, sizeof(g_zones[0]), il_zone_cmp);
	g_zones_ready = 1;
}

int il_timezone_count(void)
{
	return IL_NZONES;
}

const char *il_timezone_name(int i)
{
	il_zones_init();
	if (i < 0 || i >= IL_NZONES)
		return NULL;
	return g_tz_table[g_zones[i].index].name;
}

const char *il_timezone_label(int i)
{
	il_zones_init();
	if (i < 0 || i >= IL_NZONES)
		return NULL;
	return g_zones[i].label;
}

int il_timezone_offset_minutes(int i)
{
	il_zones_init();
	if (i < 0 || i >= IL_NZONES)
		return 0;
	return g_zones[i].east_minutes;
}

int il_timezone_find(const char *name)
{
	if (!name || !name[0])
		return -1;
	il_zones_init();
	for (int i = 0; i < IL_NZONES; i++)
		if (!strcmp(g_tz_table[g_zones[i].index].name, name))
			return i;
	return -1;
}

