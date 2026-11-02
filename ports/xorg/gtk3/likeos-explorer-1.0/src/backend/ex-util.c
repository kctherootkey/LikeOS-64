// LikeOS Explorer: small helpers the whole backend shares.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-util.h"

#include <stdlib.h>
#include <string.h>

char *ex_format_size(guint64 bytes)
{
	if (bytes == 1)
		return g_strdup("1 byte");
	if (bytes < 1000)
		return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", bytes);
	return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

char *ex_format_time(gint64 unix_time)
{
	GDateTime *t, *now;
	char *out;
	int days;

	if (unix_time <= 0)
		return g_strdup("");
	t = g_date_time_new_from_unix_local(unix_time);
	now = g_date_time_new_now_local();
	if (!t || !now) {
		if (t)
			g_date_time_unref(t);
		if (now)
			g_date_time_unref(now);
		return g_strdup("");
	}
	GDateTime *t0 = g_date_time_new_local(g_date_time_get_year(t),
					      g_date_time_get_month(t),
					      g_date_time_get_day_of_month(t),
					      0, 0, 0);
	GDateTime *n0 = g_date_time_new_local(g_date_time_get_year(now),
					      g_date_time_get_month(now),
					      g_date_time_get_day_of_month(now),
					      0, 0, 0);
	days = (int)(g_date_time_difference(n0, t0) / G_TIME_SPAN_DAY);
	if (days == 0)
		out = g_date_time_format(t, "Today, %H:%M");
	else if (days == 1)
		out = g_date_time_format(t, "Yesterday, %H:%M");
	else
		out = g_date_time_format(t, "%d.%m.%Y %H:%M");
	g_date_time_unref(t0);
	g_date_time_unref(n0);
	g_date_time_unref(t);
	g_date_time_unref(now);
	return out ? out : g_strdup("");
}

const char *ex_name_extension(const char *name)
{
	static const char *const doubles[] = { ".tar.gz", ".tar.bz2",
					       ".tar.xz", ".tar.zst", NULL };
	size_t len = strlen(name);

	for (int i = 0; doubles[i]; i++) {
		size_t dl = strlen(doubles[i]);

		if (len > dl && g_ascii_strcasecmp(name + len - dl, doubles[i]) == 0)
			return name + len - dl;
	}
	const char *dot = strrchr(name, '.');
	/* A leading dot is a hidden name, not an extension. */
	if (!dot || dot == name || dot[1] == '\0')
		return name + len;
	return dot;
}

char *ex_unique_name(GFile *dir, const char *base, gboolean keep_ext)
{
	const char *ext = keep_ext ? ex_name_extension(base) : base + strlen(base);
	char *stem = g_strndup(base, (gsize)(ext - base));
	int start = 1;

	/* "a (2).txt" taken goes on to "a (3).txt", not "a (2) (2).txt". */
	size_t sl = strlen(stem);
	if (sl > 4 && stem[sl - 1] == ')') {
		char *open = strrchr(stem, '(');

		if (open && open > stem && open[-1] == ' ') {
			char *end = NULL;
			long num = strtol(open + 1, &end, 10);

			if (end == stem + sl - 1 && num >= 2 && num < 10000) {
				open[-1] = '\0';
				start = (int)num;
			}
		}
	}
	for (int n = start; n < 10000; n++) {
		char *cand = n == 1 ? g_strdup(base) :
			     n == start ? g_strdup(base) :
					  g_strdup_printf("%s (%d)%s", stem, n, ext);
		GFile *f = g_file_get_child(dir, cand);
		gboolean taken = g_file_query_exists(f, NULL);

		g_object_unref(f);
		if (!taken) {
			g_free(stem);
			return cand;
		}
		g_free(cand);
	}
	g_free(stem);
	return g_strdup_printf("%s (%" G_GINT64_FORMAT ")", base,
			       g_get_real_time());
}

char *ex_collate_key(const char *name)
{
	return g_utf8_collate_key_for_filename(name, -1);
}

gboolean ex_name_is_hidden(const char *name)
{
	size_t len = name ? strlen(name) : 0;

	return len > 0 && (name[0] == '.' || name[len - 1] == '~');
}

const char *ex_name_problem(const char *name)
{
	if (!name || !*name)
		return "A name cannot be empty.";
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return "\".\" and \"..\" are reserved names.";
	if (strchr(name, '/'))
		return "A name cannot contain \"/\".";
	if (strlen(name) > 255)
		return "The name is too long.";
	if (!g_utf8_validate(name, -1, NULL))
		return "The name is not valid text.";
	return NULL;
}

GFile *ex_home_dir(void)
{
	return g_file_new_for_path(g_get_home_dir());
}

const char *ex_user_dir(GUserDirectory which)
{
	static char *fallback[G_USER_N_DIRECTORIES];
	static const char *const names[G_USER_N_DIRECTORIES] = {
		[G_USER_DIRECTORY_DESKTOP] = "Desktop",
		[G_USER_DIRECTORY_DOCUMENTS] = "Documents",
		[G_USER_DIRECTORY_DOWNLOAD] = "Downloads",
		[G_USER_DIRECTORY_MUSIC] = "Music",
		[G_USER_DIRECTORY_PICTURES] = "Pictures",
		[G_USER_DIRECTORY_PUBLIC_SHARE] = "Public",
		[G_USER_DIRECTORY_TEMPLATES] = "Templates",
		[G_USER_DIRECTORY_VIDEOS] = "Videos",
	};
	const char *p = g_get_user_special_dir(which);

	if (which < 0 || which >= G_USER_N_DIRECTORIES)
		return NULL;
	if (p && strcmp(p, g_get_home_dir()) != 0)
		return p;
	if (!fallback[which] && names[which])
		fallback[which] = g_build_filename(g_get_home_dir(), names[which],
						   NULL);
	if (fallback[which] && g_file_test(fallback[which], G_FILE_TEST_IS_DIR))
		return fallback[which];
	return NULL;
}

void ex_run_on(GMainContext *ctx, GSourceFunc fn, gpointer data,
	       GDestroyNotify destroy)
{
	GSource *src = g_idle_source_new();

	g_source_set_priority(src, G_PRIORITY_DEFAULT);
	g_source_set_callback(src, fn, data, destroy);
	g_source_attach(src, ctx);
	g_source_unref(src);
}

static gpointer ref_copy(gconstpointer obj, gpointer data)
{
	return g_object_ref((gpointer)obj);
}

GPtrArray *ex_objects_copy(GPtrArray *a)
{
	GPtrArray *c = g_ptr_array_copy(a, ref_copy, NULL);

	g_ptr_array_set_free_func(c, g_object_unref);
	return c;
}
