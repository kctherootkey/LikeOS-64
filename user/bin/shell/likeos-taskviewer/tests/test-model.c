// LikeOS Task Viewer -- tests of the pure part of the process pages
// (src/tv-model.c): rates, formatting, names, matching and the grouping of
// processes into apps, background processes and system tasks.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "../src/tv-model.h"

static void test_cpu(void)
{
	/* 50 ticks of 100 Hz in one second on one processor: half of it. */
	g_assert_cmpfloat_with_epsilon(tv_cpu_percent(100, 150, 1.0, 100, 1),
				       50.0, 1e-9);
	/* The same on four processors: an eighth of all of them. */
	g_assert_cmpfloat_with_epsilon(tv_cpu_percent(100, 150, 1.0, 100, 4),
				       12.5, 1e-9);
	/* Measured against the real interval, not a nominal one. */
	g_assert_cmpfloat_with_epsilon(tv_cpu_percent(0, 100, 2.0, 100, 1),
				       50.0, 1e-9);
	/* Too short to tell. */
	g_assert_cmpfloat(tv_cpu_percent(0, 10, 0.1, 100, 1), <, 0.0);
	/* Ticks never go back; a smaller figure is no use. */
	g_assert_cmpfloat(tv_cpu_percent(200, 100, 1.0, 100, 1), ==, 0.0);
	/* Clamped: rounding can overshoot. */
	g_assert_cmpfloat(tv_cpu_percent(0, 250, 1.0, 100, 2), ==, 100.0);
	g_assert_cmpfloat(tv_cpu_percent(0, 10, 1.0, 100, 0), ==, 10.0);
}

static void check_str(char *got, const char *want)
{
	g_assert_cmpstr(got, ==, want);
	g_free(got);
}

static void test_format(void)
{
	check_str(tv_format_mem(0), "0 MB");
	check_str(tv_format_mem(4096), "0.1 MB");
	check_str(tv_format_mem(12 * 1024 * 1024 + 300 * 1024), "12.3 MB");
	check_str(tv_format_mem((guint64)1234 * 1024 * 1024 +
				512 * 1024),
		  "1,234.5 MB");
	check_str(tv_format_mem((guint64)1234567 * 1024 * 1024),
		  "1,234,567.0 MB");
	check_str(tv_format_percent(0), "0%");
	check_str(tv_format_percent(0.01), "0.1%");
	check_str(tv_format_percent(12.46), "12.5%");
	check_str(tv_format_percent(100), "100%");
	check_str(tv_format_percent(-3), "0%");
	check_str(tv_format_mem_cell(512 * 1024 * 1024,
				     (guint64)2048 * 1024 * 1024, TRUE),
		  "25.0%");
	check_str(tv_format_mem_cell(512 * 1024 * 1024,
				     (guint64)2048 * 1024 * 1024, FALSE),
		  "512.0 MB");
	check_str(tv_format_mem_cell(1, 0, TRUE), "0%");
	check_str(tv_format_duration(59), "00:00:59");
	check_str(tv_format_duration(3600 + 61), "01:01:01");
	check_str(tv_format_duration(86400 + 7200), "1 day 02:00:00");
	check_str(tv_format_duration(3 * 86400), "3 days 00:00:00");
	check_str(tv_format_duration(-5), "00:00:00");
}

static void test_heat(void)
{
	g_assert_cmpfloat(tv_heat_alpha(0), ==, 0);
	g_assert_cmpfloat(tv_heat_alpha(-1), ==, 0);
	g_assert_cmpfloat(tv_heat_alpha(0.01), >, 0.08);
	g_assert_cmpfloat(tv_heat_alpha(0.25), <, tv_heat_alpha(0.5));
	g_assert_cmpfloat_with_epsilon(tv_heat_alpha(1), 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(tv_heat_alpha(7), 0.5, 1e-9);
	g_assert_cmpfloat_with_epsilon(tv_mem_heat_fraction(25, 100), 1.0,
				       1e-9);
	g_assert_cmpfloat_with_epsilon(tv_mem_heat_fraction(5, 100), 0.2,
				       1e-9);
	g_assert_cmpfloat(tv_mem_heat_fraction(5, 0), ==, 0);
}

static void test_names(void)
{
	g_assert_cmpstr(tv_state_name(TV_STATE_READY), ==, "Running");
	g_assert_cmpstr(tv_state_name(TV_STATE_RUNNING), ==, "Running");
	g_assert_cmpstr(tv_state_name(TV_STATE_BLOCKED), ==, "Sleeping");
	g_assert_cmpstr(tv_state_name(TV_STATE_STOPPED), ==, "Stopped");
	g_assert_cmpstr(tv_state_name(TV_STATE_ZOMBIE), ==, "Zombie");
	g_assert_cmpstr(tv_state_name(42), ==, "Unknown");
	g_assert_cmpstr(tv_state_status(TV_STATE_BLOCKED), ==, "");
	g_assert_cmpstr(tv_state_status(TV_STATE_STOPPED), ==, "Stopped");
	g_assert_cmpstr(tv_state_status(TV_STATE_ZOMBIE), ==, "Ending");
	g_assert_cmpstr(tv_group_title(TV_GROUP_APPS), ==, "Apps");

	/* Every class maps back to itself through its nice value. */
	for (int p = 0; p < TV_PRIO_COUNT; p++)
		g_assert_cmpint(tv_priority_of_nice(tv_priority_nice(p)), ==, p);
	g_assert_cmpint(tv_priority_nice(TV_PRIO_REALTIME), ==, -20);
	g_assert_cmpint(tv_priority_nice(TV_PRIO_LOW), ==, 19);
	g_assert_cmpstr(tv_priority_name(tv_priority_of_nice(0)), ==,
			"Normal");
	g_assert_cmpstr(tv_priority_name(tv_priority_of_nice(10)), ==,
			"Below normal");
	g_assert_cmpstr(tv_priority_name(tv_priority_of_nice(-1)), ==,
			"Above normal");
}

static void test_start(void)
{
	/* Started 10 s after boot, 100 s of uptime now: 90 s ago. */
	g_assert_cmpint(tv_start_time(1000, 100, 100.0, 1000000), ==, 999910);
	/* A start tick past the clock (rounding) is now. */
	g_assert_cmpint(tv_start_time(20000, 100, 100.0, 1000000), ==, 1000000);
	g_assert_cmpint(tv_start_time(1000, 0, 100.0, 1000000), ==, 0);
}

static void test_exec(void)
{
	check_str(tv_exec_basename("/usr/bin/xterm -ls"), "xterm");
	check_str(tv_exec_basename("env A=b B=c /usr/bin/Foo %U"), "foo");
	check_str(tv_exec_basename("likeos-explorer %F"), "likeos-explorer");
	check_str(tv_exec_basename("\"/opt/my app/run\" --x"), "run");
	g_assert_null(tv_exec_basename(""));
	g_assert_null(tv_exec_basename(NULL));
	g_assert_null(tv_exec_basename("\"unterminated"));
	check_str(tv_cmdline_program("  /usr/bin/foo -x y"), "/usr/bin/foo");
	check_str(tv_cmdline_program("bash"), "bash");
	g_assert_null(tv_cmdline_program("   "));
	g_assert_null(tv_cmdline_program(NULL));
	/* A process's name: comm, else its command line's program, else its
	 * number. */
	check_str(tv_proc_display_name("top", "/usr/bin/top -d 1", 7), "top");
	check_str(tv_proc_display_name("", "/usr/bin/top -d 1", 7), "top");
	check_str(tv_proc_display_name(NULL, "  bash -l", 8), "bash");
	check_str(tv_proc_display_name("", "", 9), "Process 9");
	check_str(tv_proc_display_name(NULL, NULL, 10), "Process 10");
	check_str(tv_proc_display_name("", "/ x", 11), "Process 11");

	char **k = tv_desktop_id_keys("org.xfce.Mousepad.desktop");

	g_assert_cmpuint(g_strv_length(k), ==, 2);
	g_assert_cmpstr(k[0], ==, "org.xfce.mousepad");
	g_assert_cmpstr(k[1], ==, "mousepad");
	g_strfreev(k);
	k = tv_desktop_id_keys("luakit.desktop");
	g_assert_cmpuint(g_strv_length(k), ==, 1);
	g_assert_cmpstr(k[0], ==, "luakit");
	g_strfreev(k);
	k = tv_desktop_id_keys(NULL);
	g_assert_cmpuint(g_strv_length(k), ==, 0);
	g_strfreev(k);
}

static void test_search(void)
{
	char *n = g_utf8_casefold("Lua", -1);

	g_assert_true(tv_search_match(NULL, "x", 1));
	g_assert_true(tv_search_match("", "x", 1));
	g_assert_true(tv_search_match(n, "luakit", 0));
	g_assert_true(tv_search_match(n, "LUAKIT", 0));
	g_assert_false(tv_search_match(n, "xterm", 12));
	g_assert_true(tv_search_match("12", "xterm", 123));
	g_assert_false(tv_search_match("23", "xterm", 123));
	g_assert_false(tv_search_match("12", "xterm", 0));
	g_assert_true(tv_search_match("12", "app12", 7));
	g_assert_false(tv_search_match("x1", NULL, 1));
	g_free(n);
}

static void test_groups(void)
{
	/* init(1) - xinit(10) - taskbar(11)
	 *                     - xterm(20, window) - bash(21) - top(22)
	 *                     - luakit(30, window) - web(31) - net(32)
	 *                                          - dialog(33, window)
	 *                                              - helper(34)
	 * kthread(2), and an orphan(40) whose parent is gone, and a cycle
	 * 50 <-> 51 that must not hang. */
	static const TvGroupIn in[] = {
		{ 1, 0, FALSE, FALSE },	   { 10, 1, FALSE, FALSE },
		{ 11, 10, FALSE, FALSE },  { 20, 10, FALSE, TRUE },
		{ 21, 20, FALSE, FALSE },  { 22, 21, FALSE, FALSE },
		{ 30, 10, FALSE, TRUE },   { 31, 30, FALSE, FALSE },
		{ 32, 31, FALSE, FALSE },  { 33, 30, FALSE, TRUE },
		{ 34, 33, FALSE, FALSE },  { 2, 0, TRUE, FALSE },
		{ 40, 999, FALSE, FALSE }, { 50, 51, FALSE, FALSE },
		{ 51, 50, FALSE, FALSE },  { 60, 2, FALSE, FALSE },
	};
	enum { N = G_N_ELEMENTS(in) };
	TvGroup g[N];
	int o[N];
	struct {
		int pid;
		TvGroup group;
		int owner_pid;
	} want[] = {
		{ 1, TV_GROUP_BACKGROUND, 0 },	{ 10, TV_GROUP_BACKGROUND, 0 },
		{ 11, TV_GROUP_BACKGROUND, 0 }, { 20, TV_GROUP_APPS, 20 },
		{ 21, TV_GROUP_APPS, 20 },	{ 22, TV_GROUP_APPS, 20 },
		{ 30, TV_GROUP_APPS, 30 },	{ 31, TV_GROUP_APPS, 30 },
		{ 32, TV_GROUP_APPS, 30 },	{ 33, TV_GROUP_APPS, 33 },
		{ 34, TV_GROUP_APPS, 33 },	{ 2, TV_GROUP_SYSTEM, 0 },
		{ 40, TV_GROUP_BACKGROUND, 0 }, { 50, TV_GROUP_BACKGROUND, 0 },
		{ 51, TV_GROUP_BACKGROUND, 0 }, { 60, TV_GROUP_BACKGROUND, 0 },
	};

	tv_group_assign(in, N, g, o);
	for (int i = 0; i < N; i++) {
		g_assert_cmpint(in[i].pid, ==, want[i].pid);
		g_assert_cmpint(g[i], ==, want[i].group);
		g_assert_cmpint(o[i] >= 0 ? in[o[i]].pid : 0, ==,
				want[i].owner_pid);
	}
	tv_group_assign(in, 0, g, o);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/model/cpu", test_cpu);
	g_test_add_func("/model/format", test_format);
	g_test_add_func("/model/heat", test_heat);
	g_test_add_func("/model/names", test_names);
	g_test_add_func("/model/start", test_start);
	g_test_add_func("/model/exec", test_exec);
	g_test_add_func("/model/search", test_search);
	g_test_add_func("/model/groups", test_groups);
	return g_test_run();
}
