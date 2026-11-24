// LikeOS Task Viewer -- the pure part of the process pages: rates, sizes,
// names, matching and the grouping of processes into apps, background
// processes and system tasks.
//
// Nothing here touches the system, X or GTK: every function takes what it
// works on as arguments, so the whole file is tested on the build host
// (tests/test-model.c).  The process backend (tv-procs.c) feeds it.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_MODEL_H
#define TV_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* Process states as the process listing reports them. */
enum {
	TV_STATE_READY = 0,
	TV_STATE_RUNNING = 1,
	TV_STATE_BLOCKED = 2,
	TV_STATE_STOPPED = 3,
	TV_STATE_ZOMBIE = 4,
};

/* The groups of the Processes page, in the order they are shown. */
typedef enum {
	TV_GROUP_APPS = 0,
	TV_GROUP_BACKGROUND = 1,
	TV_GROUP_SYSTEM = 2,
	TV_GROUP_COUNT
} TvGroup;

const char *tv_group_title(TvGroup g);

/* "Running", "Sleeping", "Stopped", "Zombie" (the Details page). */
const char *tv_state_name(int state);
/* The Processes page's status: empty for a process that is simply alive,
 * "Stopped" or "Ending" otherwise. */
const char *tv_state_status(int state);

/* The priority class of a nice value, and the nice value a class sets. */
typedef enum {
	TV_PRIO_REALTIME,
	TV_PRIO_HIGH,
	TV_PRIO_ABOVE_NORMAL,
	TV_PRIO_NORMAL,
	TV_PRIO_BELOW_NORMAL,
	TV_PRIO_LOW,
	TV_PRIO_COUNT
} TvPriority;

TvPriority tv_priority_of_nice(int nice);
int tv_priority_nice(TvPriority p);
const char *tv_priority_name(TvPriority p);

/* The share of all processors a process used between two samples: the
 * growth of its user+system ticks over the ticks all processors had in
 * `elapsed' seconds.  Negative when the interval is too short to measure
 * (under a fifth of a second the tick granularity is all one would see);
 * the caller then keeps the previous figure.  Clamped to 0..100. */
double tv_cpu_percent(guint64 prev_ticks, guint64 cur_ticks, double elapsed,
		      long hz, long ncpu);

/* "0 MB", "0.4 MB", "12.3 MB", "1,234.5 MB" -- memory is shown in
 * megabytes with one decimal, as a column of them compares at a glance. */
char *tv_format_mem(guint64 bytes);
/* "0%", "0.3%", "12.5%", "100%". */
char *tv_format_percent(double percent);
/* A memory cell in the chosen mode: tv_format_mem() or the share of
 * `total' as tv_format_percent(). */
char *tv_format_mem_cell(guint64 bytes, guint64 total, gboolean percents);
/* "4 days 03:12:09"-style age, for the properties dialog. */
char *tv_format_duration(gint64 seconds);

/* How strongly a cell is tinted for a load of `fraction' (0..1, of a
 * processor share or of the memory): 0 for none, else up to 0.5.  Grows
 * with the square root, so a light load is still visible. */
double tv_heat_alpha(double fraction);
/* The fraction of memory a cell is tinted for: a quarter of all memory is
 * the full tint. */
double tv_mem_heat_fraction(guint64 bytes, guint64 total);

/* The wall-clock start of a process, in seconds since the epoch, from its
 * start tick (ticks of `hz' since boot) and the current monotonic and
 * real-time clocks (seconds).  0 when it cannot be known. */
gint64 tv_start_time(guint64 start_tick, long hz, double now_mono,
		     gint64 now_real);

/* The program a desktop entry's Exec line runs, lower-cased and without
 * its directory: "env A=b /usr/bin/xterm -ls" is "xterm".  NULL when the
 * line has no program. */
char *tv_exec_basename(const char *exec);
/* The first word of a command line ("/usr/bin/foo -x" -> "/usr/bin/foo"). */
char *tv_cmdline_program(const char *cmdline);
/* What a process is called when nothing better (its desktop entry, its
 * window) names it: its program name as the kernel keeps it (comm); a task
 * without one by the program of its command line, without the directory;
 * one with neither "Process <pid>". */
char *tv_proc_display_name(const char *comm, const char *cmdline, int pid);
/* The keys a desktop entry id is found under: "org.xfce.mousepad.desktop"
 * gives "org.xfce.mousepad" and "mousepad" (lower-case).  NULL-terminated,
 * g_strfreev(). */
char **tv_desktop_id_keys(const char *id);

/* Whether a search matches a row: `needle' (already case-folded with
 * g_utf8_casefold) is a part of `name' (any case), or all digits and a
 * prefix of the PID.  An empty needle matches everything. */
gboolean tv_search_match(const char *needle, const char *name, int pid);

/* Grouping.  One record per process (threads folded): */
typedef struct {
	int pid;
	int ppid;
	gboolean kernel;
	gboolean has_window; /* owns a window the desktop shows */
} TvGroupIn;

/* For every record: its group, and for an app the index of the record that
 * owns the app's window (the process itself for the app's main process,
 * an ancestor for its helpers; -1 outside the apps).  A process with a
 * window of its own is an app; a user process below one, with none of its
 * own, belongs to the nearest such ancestor; any other user process is a
 * background process, a kernel task a system task. */
void tv_group_assign(const TvGroupIn *in, int n, TvGroup *group, int *owner);

G_END_DECLS

#endif
