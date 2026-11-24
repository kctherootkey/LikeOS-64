// LikeOS Task Viewer -- the process backend of the Processes, Details and
// Users pages.
//
// One snapshot of every process, taken from the kernel's process listing
// (getprocinfo(), threads folded into their processes) together with the
// desktop's windows (_NET_CLIENT_LIST and _NET_WM_PID), the desktop entries
// the apps come from, the user names and the processor share each process
// had since the snapshot before.  The pages share it: whichever asks first
// after an update tick takes the new one.
//
// A snapshot lives until the next one is taken.  Nothing may keep a TvProc
// or TvWin pointer across a return to the main loop; keep the PID or the
// window id and look it up again.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_PROCS_H
#define TV_PROCS_H

#include <gio/gio.h>

#include "tv-model.h"

G_BEGIN_DECLS

/* A top-level window the desktop shows. */
typedef struct {
	gulong xid;
	int pid;
	char *title;
	char *res_name, *res_class; /* WM_CLASS, as set */
	gboolean minimized;
	gboolean maximized;
} TvWin;

typedef struct TvProc {
	int pid, ppid, uid;
	int state;
	int nice;
	int threads;
	gboolean kernel;
	guint64 ticks; /* user + system, in clock ticks */
	double cpu;    /* share of all processors, percent */
	guint64 mem;   /* resident bytes */
	gint64 start;  /* wall-clock seconds since the epoch, 0 unknown */
	char *comm;    /* program name */
	char *cmdline; /* "" when not readable */
	char *user;    /* user name (or the number) */

	TvGroup group;
	int owner_pid;	  /* the app's main process (itself for it), or 0 */
	GPtrArray *wins;  /* TvWin *, the windows this process owns */
	GAppInfo *app;	  /* its desktop entry, or NULL */
	GIcon *icon;	  /* never NULL */
	char *name;	  /* the entry's name for an app, else comm */
} TvProc;

typedef struct {
	GPtrArray *procs;   /* TvProc *, in PID order */
	GHashTable *by_pid; /* pid -> TvProc * */
	GPtrArray *wins;    /* TvWin *, in stacking-list order */
	double cpu_total;   /* percent of all processors in use */
	guint64 mem_total;  /* bytes of memory */
	guint64 mem_used;   /* bytes in use (total minus available) */
	long ncpu, hz;
	gint64 taken;	    /* monotonic microseconds */
	guint serial;	    /* grows with every snapshot */
} TvSnapshot;

/* The current snapshot; takes the first one if there is none yet. */
const TvSnapshot *tv_procs_snapshot(void);
/* Takes a new snapshot. */
void tv_procs_sample(void);
/* Takes a new one unless the current one is younger than half the update
 * interval (so the pages ticking one after the other share one).  When
 * updates are paused it keeps the current one. */
void tv_procs_refresh(void);

TvProc *tv_procs_find(int pid);
/* The window's process, and the window itself. */
TvWin *tv_procs_find_win(gulong xid);

/* The processes below `pid' (all generations, not itself), deepest first
 * -- the order to end a tree in.  g_array_unref(). */
GArray *tv_procs_descendants(int pid);

/* The program file a process runs: /proc/<pid>/exe, looked up on PATH
 * when that is a bare name.  NULL when unknown. */
char *tv_proc_exe_path(int pid);

/* The memory and processor counts. */
long tv_procs_ncpu(void);

/* Actions.  Errors come back in plain words ("You do not have permission
 * to end this process."). */
gboolean tv_proc_signal(int pid, int sig, GError **error);
gboolean tv_proc_get_affinity(int pid, guint64 *mask, GError **error);
/* Applies to every thread of the process. */
gboolean tv_proc_set_affinity(int pid, guint64 mask, GError **error);

void tv_win_activate(gulong xid);
void tv_win_raise(gulong xid);
void tv_win_minimize(gulong xid);
void tv_win_maximize(gulong xid);
/* Asks the window to close (the window manager's polite close). */
void tv_win_close(gulong xid);

G_END_DECLS

#endif
