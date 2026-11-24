// LikeOS Task Viewer -- the pure part of the process pages (see tv-model.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tv-model.h"

const char *tv_group_title(TvGroup g)
{
	switch (g) {
	case TV_GROUP_APPS:
		return "Apps";
	case TV_GROUP_BACKGROUND:
		return "Background processes";
	case TV_GROUP_SYSTEM:
		return "System";
	default:
		return "";
	}
}

const char *tv_state_name(int state)
{
	switch (state) {
	case TV_STATE_READY:
	case TV_STATE_RUNNING:
		return "Running";
	case TV_STATE_BLOCKED:
		return "Sleeping";
	case TV_STATE_STOPPED:
		return "Stopped";
	case TV_STATE_ZOMBIE:
		return "Zombie";
	default:
		return "Unknown";
	}
}

const char *tv_state_status(int state)
{
	switch (state) {
	case TV_STATE_STOPPED:
		return "Stopped";
	case TV_STATE_ZOMBIE:
		return "Ending";
	default:
		return "";
	}
}

static const struct {
	const char *name;
	int nice;
} prio[TV_PRIO_COUNT] = {
	[TV_PRIO_REALTIME] = { "Realtime", -20 },
	[TV_PRIO_HIGH] = { "High", -10 },
	[TV_PRIO_ABOVE_NORMAL] = { "Above normal", -5 },
	[TV_PRIO_NORMAL] = { "Normal", 0 },
	[TV_PRIO_BELOW_NORMAL] = { "Below normal", 5 },
	[TV_PRIO_LOW] = { "Low", 19 },
};

TvPriority tv_priority_of_nice(int nice)
{
	if (nice <= -15)
		return TV_PRIO_REALTIME;
	if (nice <= -8)
		return TV_PRIO_HIGH;
	if (nice < 0)
		return TV_PRIO_ABOVE_NORMAL;
	if (nice == 0)
		return TV_PRIO_NORMAL;
	if (nice < 12)
		return TV_PRIO_BELOW_NORMAL;
	return TV_PRIO_LOW;
}

int tv_priority_nice(TvPriority p)
{
	return (int)p >= 0 && p < TV_PRIO_COUNT ? prio[p].nice : 0;
}

const char *tv_priority_name(TvPriority p)
{
	return (int)p >= 0 && p < TV_PRIO_COUNT ? prio[p].name : "";
}

double tv_cpu_percent(guint64 prev_ticks, guint64 cur_ticks, double elapsed,
		      long hz, long ncpu)
{
	double p;

	if (elapsed < 0.2 || hz <= 0)
		return -1.0;
	if (ncpu < 1)
		ncpu = 1;
	if (cur_ticks <= prev_ticks)
		return 0.0;
	p = (double)(cur_ticks - prev_ticks) * 100.0 /
	    (elapsed * (double)hz * (double)ncpu);
	return CLAMP(p, 0.0, 100.0);
}

/* `v' with thousands separators and one decimal. */
static char *group_digits(double v, int decimals)
{
	char *raw = g_strdup_printf("%.*f", decimals, v);
	char *dot = strchr(raw, '.');
	size_t intlen = dot ? (size_t)(dot - raw) : strlen(raw);
	GString *s = g_string_new(NULL);

	for (size_t i = 0; i < intlen; i++) {
		if (i > 0 && (intlen - i) % 3 == 0)
			g_string_append_c(s, ',');
		g_string_append_c(s, raw[i]);
	}
	if (dot)
		g_string_append(s, dot);
	g_free(raw);
	return g_string_free(s, FALSE);
}

char *tv_format_mem(guint64 bytes)
{
	double mb = (double)bytes / (1024.0 * 1024.0);
	char *num, *out;

	if (bytes == 0)
		return g_strdup("0 MB");
	/* One decimal; a tiny resident set still reads as non-zero. */
	if (mb < 0.1)
		mb = 0.1;
	num = group_digits(mb, 1);
	out = g_strdup_printf("%s MB", num);
	g_free(num);
	return out;
}

char *tv_format_percent(double percent)
{
	if (percent <= 0.0 || isnan(percent))
		return g_strdup("0%");
	if (percent >= 99.95)
		return g_strdup("100%");
	if (percent < 0.05)
		percent = 0.1;
	return g_strdup_printf("%.1f%%", percent);
}

char *tv_format_mem_cell(guint64 bytes, guint64 total, gboolean percents)
{
	if (!percents)
		return tv_format_mem(bytes);
	if (total == 0)
		return g_strdup("0%");
	return tv_format_percent((double)bytes * 100.0 / (double)total);
}

char *tv_format_duration(gint64 seconds)
{
	gint64 d, h, m, s;

	if (seconds < 0)
		seconds = 0;
	d = seconds / 86400;
	h = (seconds / 3600) % 24;
	m = (seconds / 60) % 60;
	s = seconds % 60;
	if (d > 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s %02d:%02d:%02d",
				       d, d == 1 ? "day" : "days", (int)h,
				       (int)m, (int)s);
	return g_strdup_printf("%02d:%02d:%02d", (int)h, (int)m, (int)s);
}

double tv_heat_alpha(double fraction)
{
	if (!(fraction > 0.0))
		return 0.0;
	if (fraction > 1.0)
		fraction = 1.0;
	return 0.08 + 0.42 * sqrt(fraction);
}

double tv_mem_heat_fraction(guint64 bytes, guint64 total)
{
	if (total == 0)
		return 0.0;
	return MIN(1.0, (double)bytes * 4.0 / (double)total);
}

gint64 tv_start_time(guint64 start_tick, long hz, double now_mono,
		     gint64 now_real)
{
	double since_boot;

	if (hz <= 0 || now_real <= 0)
		return 0;
	since_boot = (double)start_tick / (double)hz;
	if (since_boot > now_mono)
		since_boot = now_mono;
	return now_real - (gint64)floor(now_mono - since_boot + 0.5);
}

char *tv_exec_basename(const char *exec)
{
	char **argv = NULL;
	char *bin = NULL;
	int argc = 0;

	if (!exec || !g_shell_parse_argv(exec, &argc, &argv, NULL))
		return NULL;
	for (int i = 0; i < argc && !bin; i++) {
		char *base;

		/* Field codes are not the program. */
		if (argv[i][0] == '%')
			continue;
		base = g_path_get_basename(argv[i]);
		/* "env" and the assignments after it are not the program
		 * either. */
		if (!(i == 0 && strcmp(base, "env") == 0) &&
		    !(strchr(argv[i], '=') && argv[i][0] != '/'))
			bin = g_ascii_strdown(base, -1);
		g_free(base);
	}
	g_strfreev(argv);
	return bin;
}

char *tv_cmdline_program(const char *cmdline)
{
	const char *p, *e;

	if (!cmdline)
		return NULL;
	p = cmdline;
	while (*p == ' ' || *p == '\t')
		p++;
	e = p;
	while (*e && *e != ' ' && *e != '\t')
		e++;
	if (e == p)
		return NULL;
	return g_strndup(p, (gsize)(e - p));
}

char *tv_proc_display_name(const char *comm, const char *cmdline, int pid)
{
	char *prog;

	if (comm && *comm)
		return g_strdup(comm);
	prog = tv_cmdline_program(cmdline);
	if (prog) {
		char *base = g_path_get_basename(prog);

		g_free(prog);
		/* "/" alone has no name in it. */
		if (strcmp(base, G_DIR_SEPARATOR_S) != 0 &&
		    strcmp(base, ".") != 0)
			return base;
		g_free(base);
	}
	return g_strdup_printf("Process %d", pid);
}

char **tv_desktop_id_keys(const char *id)
{
	GPtrArray *keys = g_ptr_array_new();
	char *stem, *dot;

	if (!id || !*id) {
		g_ptr_array_add(keys, NULL);
		return (char **)g_ptr_array_free(keys, FALSE);
	}
	stem = g_ascii_strdown(id, -1);
	if (g_str_has_suffix(stem, ".desktop"))
		stem[strlen(stem) - strlen(".desktop")] = '\0';
	g_ptr_array_add(keys, g_strdup(stem));
	/* Reverse-domain ids: the last part is what the program is called. */
	dot = strrchr(stem, '.');
	if (dot && dot[1])
		g_ptr_array_add(keys, g_strdup(dot + 1));
	g_free(stem);
	g_ptr_array_add(keys, NULL);
	return (char **)g_ptr_array_free(keys, FALSE);
}

gboolean tv_search_match(const char *needle, const char *name, int pid)
{
	gboolean digits = TRUE;
	gboolean found = FALSE;

	if (!needle || !*needle)
		return TRUE;
	if (name) {
		char *hay = g_utf8_casefold(name, -1);

		found = strstr(hay, needle) != NULL;
		g_free(hay);
		if (found)
			return TRUE;
	}
	for (const char *p = needle; *p; p++)
		if (!g_ascii_isdigit(*p))
			digits = FALSE;
	if (digits && pid > 0) {
		char buf[16];

		g_snprintf(buf, sizeof(buf), "%d", pid);
		return g_str_has_prefix(buf, needle);
	}
	return FALSE;
}

void tv_group_assign(const TvGroupIn *in, int n, TvGroup *group, int *owner)
{
	GHashTable *index = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (int i = 0; i < n; i++)
		g_hash_table_insert(index, GINT_TO_POINTER(in[i].pid),
				    GINT_TO_POINTER(i + 1));

	for (int i = 0; i < n; i++) {
		int cur = i;

		owner[i] = -1;
		if (in[i].kernel) {
			group[i] = TV_GROUP_SYSTEM;
			continue;
		}
		group[i] = TV_GROUP_BACKGROUND;
		/* Up the parent chain to the first process with a window;
		 * at most n steps, so a broken chain cannot loop. */
		for (int steps = 0; cur >= 0 && steps <= n; steps++) {
			int next;

			if (in[cur].kernel)
				break;
			if (in[cur].has_window) {
				group[i] = TV_GROUP_APPS;
				owner[i] = cur;
				break;
			}
			if (in[cur].ppid <= 0 || in[cur].ppid == in[cur].pid)
				break;
			next = GPOINTER_TO_INT(g_hash_table_lookup(
				       index, GINT_TO_POINTER(in[cur].ppid))) -
			       1;
			cur = next;
		}
	}
	g_hash_table_destroy(index);
}
