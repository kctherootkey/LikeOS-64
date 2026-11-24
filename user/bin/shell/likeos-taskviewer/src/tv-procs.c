// LikeOS Task Viewer -- the process backend (see tv-procs.h).
//
// The processor share of a process is the growth of its user and system
// ticks between two snapshots over the ticks every processor had in the
// real time between them (the monotonic clock, not the nominal update
// interval: a busy main loop or a resumed pause makes the two differ).  A
// PID seen with a different start tick is a new process and starts from
// nothing.
//
// The windows come from the window manager's client list.  A window counts
// when the desktop would give it a taskbar button: not the panel or the
// desktop, not a splash or a toolbar, not one asking to be skipped.  Its
// process is the one its _NET_WM_PID names.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procinfo.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdkx.h>
#endif

#include "tv-pages.h"
#include "tv-procs.h"

/* ---- previous figures, for the rates ---------------------------------- */

typedef struct {
	guint64 start_tick;
	guint64 ticks;
	double cpu;
} Prev;

static TvSnapshot *cur;
static GHashTable *prev;     /* pid -> Prev * */
static double prev_time;     /* monotonic seconds of the snapshot before */
static long sys_hz, sys_ncpu, page_size;

static procinfo_t *raw;
static int raw_cap;

static GHashTable *users; /* uid -> name */

static GIcon *generic_icon;

/* ---- desktop entries ---------------------------------------------------- */

static GHashTable *by_wmclass, *by_id, *by_exec; /* key -> GAppInfo * */
static GAppInfoMonitor *app_monitor;

static void index_clear(void)
{
	g_clear_pointer(&by_wmclass, g_hash_table_destroy);
	g_clear_pointer(&by_id, g_hash_table_destroy);
	g_clear_pointer(&by_exec, g_hash_table_destroy);
}

static void index_put(GHashTable *t, const char *key, GAppInfo *app)
{
	if (!key || !*key || g_hash_table_contains(t, key))
		return;
	g_hash_table_insert(t, g_ascii_strdown(key, -1), g_object_ref(app));
}

static void index_add(GAppInfo *app)
{
	const char *wmclass = NULL;
	char **keys;
	char *bin;

	if (G_IS_DESKTOP_APP_INFO(app))
		wmclass = g_desktop_app_info_get_startup_wm_class(
			G_DESKTOP_APP_INFO(app));
	index_put(by_wmclass, wmclass, app);
	keys = tv_desktop_id_keys(g_app_info_get_id(app));
	for (char **k = keys; *k; k++)
		index_put(by_id, *k, app);
	g_strfreev(keys);
	bin = tv_exec_basename(g_app_info_get_commandline(app));
	index_put(by_exec, bin, app);
	g_free(bin);
}

static void index_build(void)
{
	GList *all;

	if (by_wmclass)
		return;
	by_wmclass = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					   g_object_unref);
	by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
				      g_object_unref);
	by_exec = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					g_object_unref);
	all = g_app_info_get_all();
	/* The entries the menus show first: a hidden helper entry for the
	 * same program must not win over the real one. */
	for (int pass = 0; pass < 2; pass++)
		for (GList *l = all; l; l = l->next)
			if (g_app_info_should_show(l->data) == (pass == 0))
				index_add(l->data);
	g_list_free_full(all, g_object_unref);
}

static void apps_changed(GAppInfoMonitor *m, gpointer data)
{
	index_clear();
}

static GAppInfo *lookup(GHashTable *t, const char *key)
{
	GAppInfo *a;
	char *k;

	if (!key || !*key)
		return NULL;
	k = g_ascii_strdown(key, -1);
	a = g_hash_table_lookup(t, k);
	g_free(k);
	return a;
}

static GAppInfo *match_app(const TvProc *p)
{
	GAppInfo *a = NULL;
	char *prog, *base = NULL;

	index_build();
	for (guint i = 0; p->wins && i < p->wins->len && !a; i++) {
		TvWin *w = p->wins->pdata[i];

		if (!a)
			a = lookup(by_wmclass, w->res_class);
		if (!a)
			a = lookup(by_wmclass, w->res_name);
		if (!a)
			a = lookup(by_id, w->res_class);
		if (!a)
			a = lookup(by_id, w->res_name);
		if (!a)
			a = lookup(by_exec, w->res_name);
	}
	if (!a)
		a = lookup(by_exec, p->comm);
	if (!a && (prog = tv_cmdline_program(p->cmdline))) {
		base = g_path_get_basename(prog);
		a = lookup(by_exec, base);
		g_free(prog);
	}
	if (!a && p->wins && p->wins->len)
		a = lookup(by_id, p->comm);
	g_free(base);
	return a;
}

/* ---- users -------------------------------------------------------------- */

static const char *user_name(int uid)
{
	const char *n;
	struct passwd *pw;

	if (!users)
		users = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					      NULL, g_free);
	n = g_hash_table_lookup(users, GINT_TO_POINTER(uid + 1));
	if (n)
		return n;
	pw = getpwuid((uid_t)uid);
	if (pw && pw->pw_name && *pw->pw_name)
		n = g_strdup(pw->pw_name);
	else
		n = g_strdup_printf("%d", uid);
	g_hash_table_insert(users, GINT_TO_POINTER(uid + 1), (gpointer)n);
	return n;
}

/* ---- windows ------------------------------------------------------------ */

static void win_free(gpointer data)
{
	TvWin *w = data;

	g_free(w->title);
	g_free(w->res_name);
	g_free(w->res_class);
	g_free(w);
}

#ifdef GDK_WINDOWING_X11
static struct {
	gboolean done;
	Atom client_list, wm_pid, net_wm_name, utf8, window_type, type_dock,
		type_desktop, type_toolbar, type_splash, type_menu,
		type_utility, net_wm_state, skip_taskbar, hidden, max_vert,
		max_horz, active_window, close_window, wm_change_state;
} A;

static Display *xdpy(void)
{
	GdkDisplay *d = gdk_display_get_default();

	if (!d || !GDK_IS_X11_DISPLAY(d))
		return NULL;
	return GDK_DISPLAY_XDISPLAY(d);
}

static void atoms_init(Display *dpy)
{
	static const char *names[] = {
		"_NET_CLIENT_LIST",
		"_NET_WM_PID",
		"_NET_WM_NAME",
		"UTF8_STRING",
		"_NET_WM_WINDOW_TYPE",
		"_NET_WM_WINDOW_TYPE_DOCK",
		"_NET_WM_WINDOW_TYPE_DESKTOP",
		"_NET_WM_WINDOW_TYPE_TOOLBAR",
		"_NET_WM_WINDOW_TYPE_SPLASH",
		"_NET_WM_WINDOW_TYPE_MENU",
		"_NET_WM_WINDOW_TYPE_UTILITY",
		"_NET_WM_STATE",
		"_NET_WM_STATE_SKIP_TASKBAR",
		"_NET_WM_STATE_HIDDEN",
		"_NET_WM_STATE_MAXIMIZED_VERT",
		"_NET_WM_STATE_MAXIMIZED_HORZ",
		"_NET_ACTIVE_WINDOW",
		"_NET_CLOSE_WINDOW",
		"WM_CHANGE_STATE",
	};
	Atom out[G_N_ELEMENTS(names)];

	if (A.done)
		return;
	XInternAtoms(dpy, (char **)names, G_N_ELEMENTS(names), False, out);
	A.client_list = out[0];
	A.wm_pid = out[1];
	A.net_wm_name = out[2];
	A.utf8 = out[3];
	A.window_type = out[4];
	A.type_dock = out[5];
	A.type_desktop = out[6];
	A.type_toolbar = out[7];
	A.type_splash = out[8];
	A.type_menu = out[9];
	A.type_utility = out[10];
	A.net_wm_state = out[11];
	A.skip_taskbar = out[12];
	A.hidden = out[13];
	A.max_vert = out[14];
	A.max_horz = out[15];
	A.active_window = out[16];
	A.close_window = out[17];
	A.wm_change_state = out[18];
	A.done = TRUE;
}

/* A property, inside an error trap: the window may be gone by now. */
static unsigned char *prop_get(Display *dpy, Window w, Atom prop, Atom type,
			       unsigned long *nitems)
{
	Atom actual;
	int fmt;
	unsigned long after;
	unsigned char *data = NULL;
	GdkDisplay *disp = gdk_display_get_default();
	int r;

	*nitems = 0;
	gdk_x11_display_error_trap_push(disp);
	r = XGetWindowProperty(dpy, w, prop, 0, 1024 * 1024, False, type,
			       &actual, &fmt, nitems, &after, &data);
	gdk_x11_display_error_trap_pop_ignored(disp);
	if (r != Success || actual == None) {
		if (data)
			XFree(data);
		*nitems = 0;
		return NULL;
	}
	return data;
}

static gboolean has_atom(Display *dpy, Window w, Atom prop, Atom value)
{
	unsigned long n;
	unsigned char *d = prop_get(dpy, w, prop, XA_ATOM, &n);
	gboolean found = FALSE;

	if (d) {
		for (unsigned long i = 0; i < n; i++)
			if (((Atom *)d)[i] == value)
				found = TRUE;
		XFree(d);
	}
	return found;
}

static char *win_title(Display *dpy, Window w)
{
	unsigned long n;
	unsigned char *d = prop_get(dpy, w, A.net_wm_name, A.utf8, &n);
	char *s = NULL;

	if (d && *d && g_utf8_validate((char *)d, -1, NULL))
		s = g_strdup((char *)d);
	if (d)
		XFree(d);
	if (s)
		return s;
	d = prop_get(dpy, w, XA_WM_NAME, AnyPropertyType, &n);
	if (d && *d)
		s = g_convert((char *)d, -1, "UTF-8", "ISO-8859-1", NULL, NULL,
			      NULL);
	if (d)
		XFree(d);
	return s ? s : g_strdup("");
}

static void read_windows(GPtrArray *out)
{
	Display *dpy = xdpy();
	unsigned long n;
	unsigned char *d;
	GdkDisplay *disp = gdk_display_get_default();

	if (!dpy)
		return;
	atoms_init(dpy);
	d = prop_get(dpy, DefaultRootWindow(dpy), A.client_list, XA_WINDOW, &n);
	if (!d)
		return;
	for (unsigned long i = 0; i < n; i++) {
		Window w = ((Window *)d)[i];
		unsigned long k;
		unsigned char *pid;
		XClassHint ch = { NULL, NULL };
		TvWin *tw;

		if (has_atom(dpy, w, A.window_type, A.type_dock) ||
		    has_atom(dpy, w, A.window_type, A.type_desktop) ||
		    has_atom(dpy, w, A.window_type, A.type_toolbar) ||
		    has_atom(dpy, w, A.window_type, A.type_splash) ||
		    has_atom(dpy, w, A.window_type, A.type_menu) ||
		    has_atom(dpy, w, A.net_wm_state, A.skip_taskbar))
			continue;
		pid = prop_get(dpy, w, A.wm_pid, XA_CARDINAL, &k);
		if (!pid)
			continue;
		tw = g_new0(TvWin, 1);
		tw->xid = (gulong)w;
		tw->pid = k >= 1 ? (int)*(unsigned long *)pid : 0;
		XFree(pid);
		if (tw->pid <= 0) {
			g_free(tw);
			continue;
		}
		tw->title = win_title(dpy, w);
		gdk_x11_display_error_trap_push(disp);
		if (XGetClassHint(dpy, w, &ch)) {
			tw->res_name = g_strdup(ch.res_name);
			tw->res_class = g_strdup(ch.res_class);
			if (ch.res_name)
				XFree(ch.res_name);
			if (ch.res_class)
				XFree(ch.res_class);
		}
		gdk_x11_display_error_trap_pop_ignored(disp);
		tw->minimized = has_atom(dpy, w, A.net_wm_state, A.hidden);
		tw->maximized = has_atom(dpy, w, A.net_wm_state, A.max_vert) &&
				has_atom(dpy, w, A.net_wm_state, A.max_horz);
		g_ptr_array_add(out, tw);
	}
	XFree(d);
}

static void root_message(gulong xid, Atom type, long d0, long d1, long d2)
{
	Display *dpy = xdpy();
	XClientMessageEvent ev;
	GdkDisplay *disp = gdk_display_get_default();

	if (!dpy)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.window = (Window)xid;
	ev.message_type = type;
	ev.format = 32;
	ev.data.l[0] = d0;
	ev.data.l[1] = d1;
	ev.data.l[2] = d2;
	gdk_x11_display_error_trap_push(disp);
	XSendEvent(dpy, DefaultRootWindow(dpy), False,
		   SubstructureNotifyMask | SubstructureRedirectMask,
		   (XEvent *)&ev);
	XFlush(dpy);
	gdk_x11_display_error_trap_pop_ignored(disp);
}

void tv_win_activate(gulong xid)
{
	if (!xdpy())
		return;
	atoms_init(xdpy());
	/* Source 2: a pager-like tool acting for the user. */
	root_message(xid, A.active_window, 2, CurrentTime, 0);
}

void tv_win_raise(gulong xid)
{
	Display *dpy = xdpy();
	TvWin *w = tv_procs_find_win(xid);
	GdkDisplay *disp = gdk_display_get_default();

	if (!dpy)
		return;
	/* A minimised window has to come back first, which is what the
	 * activation does; otherwise a stacking request alone, which the
	 * window manager answers by raising the frame without moving the
	 * focus. */
	if (w && w->minimized) {
		tv_win_activate(xid);
		return;
	}
	gdk_x11_display_error_trap_push(disp);
	XRaiseWindow(dpy, (Window)xid);
	XFlush(dpy);
	gdk_x11_display_error_trap_pop_ignored(disp);
}

void tv_win_minimize(gulong xid)
{
	if (!xdpy())
		return;
	atoms_init(xdpy());
	root_message(xid, A.wm_change_state, IconicState, 0, 0);
}

void tv_win_maximize(gulong xid)
{
	TvWin *w = tv_procs_find_win(xid);

	if (!xdpy())
		return;
	atoms_init(xdpy());
	if (w && w->minimized)
		tv_win_activate(xid);
	root_message(xid, A.net_wm_state, 1 /* add */, (long)A.max_vert,
		     (long)A.max_horz);
}

void tv_win_close(gulong xid)
{
	if (!xdpy())
		return;
	atoms_init(xdpy());
	root_message(xid, A.close_window, CurrentTime, 2, 0);
}
#else
static void read_windows(GPtrArray *out)
{
}

void tv_win_activate(gulong xid)
{
}

void tv_win_raise(gulong xid)
{
}

void tv_win_minimize(gulong xid)
{
}

void tv_win_maximize(gulong xid)
{
}

void tv_win_close(gulong xid)
{
}
#endif

/* ---- the snapshot ------------------------------------------------------- */

static void proc_free(gpointer data)
{
	TvProc *p = data;

	g_free(p->comm);
	g_free(p->cmdline);
	if (p->wins)
		g_ptr_array_unref(p->wins);
	g_clear_object(&p->app);
	g_clear_object(&p->icon);
	g_free(p->name);
	g_free(p);
}

static void snapshot_free(TvSnapshot *s)
{
	if (!s)
		return;
	g_hash_table_destroy(s->by_pid);
	g_ptr_array_unref(s->procs);
	g_ptr_array_unref(s->wins);
	g_free(s);
}

static double mono_seconds(void)
{
	return (double)g_get_monotonic_time() / 1e6;
}

/* The listing, threads folded; grows the buffer until everything fits. */
static int read_listing(gboolean fold)
{
	int n;

	for (;;) {
		if (!raw) {
			raw_cap = 512;
			raw = g_new(procinfo_t, raw_cap);
		}
		n = getprocinfo(raw, raw_cap);
		if (n < 0)
			return 0;
		if (n < raw_cap || raw_cap >= 16384)
			break;
		raw_cap *= 2;
		raw = g_renew(procinfo_t, raw, raw_cap);
	}
	return fold ? procinfo_fold_threads(raw, n) : n;
}

static void sys_init(void)
{
	long v;

	if (sys_hz)
		return;
	v = sysconf(_SC_CLK_TCK);
	sys_hz = v > 0 ? v : 100;
	v = sysconf(_SC_NPROCESSORS_ONLN);
	sys_ncpu = v > 0 ? v : 1;
	v = sysconf(_SC_PAGESIZE);
	page_size = v > 0 ? v : 4096;
	generic_icon = g_themed_icon_new_with_default_fallbacks(
		"application-x-executable");
	app_monitor = g_app_info_monitor_get();
	g_signal_connect(app_monitor, "changed", G_CALLBACK(apps_changed),
			 NULL);
}

long tv_procs_ncpu(void)
{
	sys_init();
	return sys_ncpu;
}

static char *field_dup(const char *s, gsize max)
{
	gsize len = strnlen(s, max);
	char *out = g_strndup(s, len);

	/* The kernel's strings are whatever the program passed; the view
	 * needs UTF-8. */
	if (!g_utf8_validate(out, -1, NULL)) {
		char *fixed = g_utf8_make_valid(out, -1);

		g_free(out);
		out = fixed;
	}
	return out;
}

void tv_procs_sample(void)
{
	static guint serial;
	TvSnapshot *s = g_new0(TvSnapshot, 1);
	GHashTable *next = g_hash_table_new_full(g_direct_hash, g_direct_equal,
						 NULL, g_free);
	struct sysinfo si;
	double now = mono_seconds(), elapsed;
	gint64 real = g_get_real_time() / G_USEC_PER_SEC;
	int n;
	TvGroupIn *gin;
	TvGroup *group;
	int *owner;

	sys_init();
	s->procs = g_ptr_array_new_with_free_func(proc_free);
	s->by_pid = g_hash_table_new(g_direct_hash, g_direct_equal);
	s->wins = g_ptr_array_new_with_free_func(win_free);
	s->ncpu = sys_ncpu;
	s->hz = sys_hz;
	s->taken = g_get_monotonic_time();
	s->serial = ++serial;
	elapsed = prev_time > 0 ? now - prev_time : 0;

	memset(&si, 0, sizeof(si));
	if (sysinfo(&si) == 0) {
		guint64 unit = si.mem_unit ? si.mem_unit : 1;
		guint64 avail = si.available ? si.available : si.freeram;

		s->mem_total = (guint64)si.totalram * unit;
		avail *= unit;
		s->mem_used = s->mem_total > avail ? s->mem_total - avail : 0;
	}

	read_windows(s->wins);

	n = read_listing(TRUE);
	for (int i = 0; i < n; i++) {
		const procinfo_t *r = &raw[i];
		TvProc *p = g_new0(TvProc, 1);
		Prev *was = prev ? g_hash_table_lookup(prev,
						       GINT_TO_POINTER(r->pid)) :
				   NULL;
		Prev *now_p = g_new0(Prev, 1);

		p->pid = r->pid;
		p->ppid = r->ppid;
		p->uid = r->uid;
		p->state = r->state;
		p->nice = r->nice;
		p->threads = r->nr_threads > 0 ? r->nr_threads : 1;
		p->kernel = r->is_kernel != 0;
		p->ticks = r->utime_ticks + r->stime_ticks;
		p->mem = p->kernel ? 0 : r->rss * (guint64)page_size;
		p->start = tv_start_time(r->start_tick, sys_hz, now, real);
		p->comm = field_dup(r->comm, sizeof(r->comm));
		p->cmdline = field_dup(r->cmdline, sizeof(r->cmdline));
		p->user = g_strdup(user_name(r->uid));
		p->cpu = 0;
		if (was && was->start_tick == r->start_tick) {
			double c = tv_cpu_percent(was->ticks, p->ticks, elapsed,
						  sys_hz, sys_ncpu);

			p->cpu = c < 0 ? was->cpu : c;
		}
		now_p->start_tick = r->start_tick;
		now_p->ticks = p->ticks;
		now_p->cpu = p->cpu;
		g_hash_table_replace(next, GINT_TO_POINTER(p->pid), now_p);

		s->cpu_total += p->cpu;
		g_ptr_array_add(s->procs, p);
		g_hash_table_insert(s->by_pid, GINT_TO_POINTER(p->pid), p);
	}
	s->cpu_total = MIN(s->cpu_total, 100.0);
	/* A too-short interval kept the old figures; it must not become the
	 * base the next rate is measured from either. */
	if (elapsed >= 0.2 || prev_time <= 0) {
		if (prev)
			g_hash_table_destroy(prev);
		prev = next;
		prev_time = now;
	} else {
		g_hash_table_destroy(next);
	}

	/* Each window to its process. */
	for (guint i = 0; i < s->wins->len; i++) {
		TvWin *w = s->wins->pdata[i];
		TvProc *p = g_hash_table_lookup(s->by_pid,
						GINT_TO_POINTER(w->pid));

		if (!p || p->kernel)
			continue;
		if (!p->wins)
			p->wins = g_ptr_array_new();
		g_ptr_array_add(p->wins, w);
	}

	/* Groups. */
	gin = g_new0(TvGroupIn, s->procs->len + 1);
	group = g_new0(TvGroup, s->procs->len + 1);
	owner = g_new0(int, s->procs->len + 1);
	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *p = s->procs->pdata[i];

		gin[i].pid = p->pid;
		gin[i].ppid = p->ppid;
		gin[i].kernel = p->kernel;
		gin[i].has_window = p->wins && p->wins->len;
	}
	tv_group_assign(gin, (int)s->procs->len, group, owner);
	for (guint i = 0; i < s->procs->len; i++) {
		TvProc *p = s->procs->pdata[i];
		GAppInfo *a;

		p->group = group[i];
		p->owner_pid = owner[i] >= 0 ?
				       ((TvProc *)s->procs->pdata[owner[i]])->pid :
				       0;
		a = p->kernel ? NULL : match_app(p);
		p->app = a ? g_object_ref(a) : NULL;
		p->icon = a && g_app_info_get_icon(a) ?
				  g_object_ref(g_app_info_get_icon(a)) :
				  g_object_ref(generic_icon);
		/* An app is called what its entry calls it; a process
		 * running on its own by its program name. */
		if (a && p->wins && p->wins->len)
			p->name = g_strdup(g_app_info_get_display_name(a));
		else if (p->wins && p->wins->len && !p->comm[0] &&
			 ((TvWin *)p->wins->pdata[0])->res_class)
			p->name = g_strdup(
				((TvWin *)p->wins->pdata[0])->res_class);
		else
			p->name = tv_proc_display_name(p->comm, p->cmdline,
						       p->pid);
	}
	g_free(gin);
	g_free(group);
	g_free(owner);

	snapshot_free(cur);
	cur = s;
}

const TvSnapshot *tv_procs_snapshot(void)
{
	if (!cur)
		tv_procs_sample();
	return cur;
}

void tv_procs_refresh(void)
{
	guint ms = tv_update_interval_ms();

	if (!cur) {
		tv_procs_sample();
		return;
	}
	if (ms == 0)
		return;
	if (g_get_monotonic_time() - cur->taken >= (gint64)ms * 1000 / 2)
		tv_procs_sample();
}

TvProc *tv_procs_find(int pid)
{
	if (!cur)
		return NULL;
	return g_hash_table_lookup(cur->by_pid, GINT_TO_POINTER(pid));
}

TvWin *tv_procs_find_win(gulong xid)
{
	if (!cur)
		return NULL;
	for (guint i = 0; i < cur->wins->len; i++) {
		TvWin *w = cur->wins->pdata[i];

		if (w->xid == xid)
			return w;
	}
	return NULL;
}

static void collect(GHashTable *children, int pid, GArray *out, int depth)
{
	GArray *kids = g_hash_table_lookup(children, GINT_TO_POINTER(pid));

	if (!kids || depth > 64)
		return;
	for (guint i = 0; i < kids->len; i++) {
		int c = g_array_index(kids, int, i);

		collect(children, c, out, depth + 1);
		g_array_append_val(out, c);
	}
}

GArray *tv_procs_descendants(int pid)
{
	GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
	GHashTable *children;

	if (!cur)
		return out;
	children = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
					 (GDestroyNotify)g_array_unref);
	for (guint i = 0; i < cur->procs->len; i++) {
		TvProc *p = cur->procs->pdata[i];
		GArray *k;

		if (p->ppid <= 0 || p->ppid == p->pid)
			continue;
		k = g_hash_table_lookup(children, GINT_TO_POINTER(p->ppid));
		if (!k) {
			k = g_array_new(FALSE, FALSE, sizeof(int));
			g_hash_table_insert(children, GINT_TO_POINTER(p->ppid),
					    k);
		}
		g_array_append_val(k, p->pid);
	}
	collect(children, pid, out, 0);
	g_hash_table_destroy(children);
	return out;
}

char *tv_proc_exe_path(int pid)
{
	char *link = g_strdup_printf("/proc/%d/exe", pid);
	char *target = g_file_read_link(link, NULL);
	char *path = NULL;

	g_free(link);
	if (!target || !*target) {
		TvProc *p = tv_procs_find(pid);

		g_free(target);
		target = p ? tv_cmdline_program(p->cmdline) : NULL;
		if (!target)
			return NULL;
	}
	if (g_path_is_absolute(target)) {
		path = g_strdup(target);
	} else if (!strchr(target, '/')) {
		path = g_find_program_in_path(target);
	} else {
		/* Relative to where the process started; its working
		 * directory is the best guess there is. */
		char *cwdlink = g_strdup_printf("/proc/%d/cwd", pid);
		char *cwd = g_file_read_link(cwdlink, NULL);

		if (cwd)
			path = g_canonicalize_filename(target, cwd);
		g_free(cwd);
		g_free(cwdlink);
	}
	g_free(target);
	return path;
}

static void set_errno_error(GError **error, int err, const char *what,
			    const char *verb)
{
	const char *msg;
	char *perm = NULL;

	switch (err) {
	case EPERM:
	case EACCES:
		msg = perm = g_strdup_printf(
			"You do not have permission to %s this process.", verb);
		break;
	case ESRCH:
		msg = "The process no longer exists.";
		break;
	case EINVAL:
		msg = "The system refused the request as invalid.";
		break;
	default:
		msg = g_strerror(err);
		break;
	}
	g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "%s %s",
		    what, msg);
	g_free(perm);
}

gboolean tv_proc_signal(int pid, int sig, GError **error)
{
	TvProc *p = tv_procs_find(pid);

	if (pid <= 1) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			    "The system's first process cannot be ended.");
		return FALSE;
	}
	if (p && p->kernel) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			    "%s is a part of the system and cannot be ended.",
			    p->name);
		return FALSE;
	}
	if (kill((pid_t)pid, sig) != 0) {
		set_errno_error(error, errno, "The process could not be ended.", "end");
		return FALSE;
	}
	return TRUE;
}

gboolean tv_proc_get_affinity(int pid, guint64 *mask, GError **error)
{
	cpu_set_t set;

	memset(&set, 0, sizeof(set));
	if (sched_getaffinity((pid_t)pid, sizeof(set), &set) != 0) {
		set_errno_error(error, errno,
				"The processor affinity could not be read.",
				"read");
		return FALSE;
	}
	*mask = (guint64)set.bits[0];
	if (*mask == 0) /* no restriction recorded */
		*mask = sys_ncpu >= 64 ? G_MAXUINT64 :
					 (((guint64)1 << sys_ncpu) - 1);
	return TRUE;
}

gboolean tv_proc_set_affinity(int pid, guint64 mask, GError **error)
{
	cpu_set_t set;
	int n, done = 0, failed = 0, err = 0;

	sys_init();
	memset(&set, 0, sizeof(set));
	set.bits[0] = (unsigned long)mask;
	/* The setting is per thread: every thread of the process, from the
	 * unfolded listing. */
	n = read_listing(FALSE);
	for (int i = 0; i < n; i++) {
		if (raw[i].tgid != pid && raw[i].pid != pid)
			continue;
		if (sched_setaffinity((pid_t)raw[i].pid, sizeof(set), &set) ==
		    0) {
			done++;
		} else {
			failed++;
			err = errno;
		}
	}
	if (!done && !failed) {
		if (sched_setaffinity((pid_t)pid, sizeof(set), &set) == 0)
			return TRUE;
		err = errno;
		failed = 1;
	}
	if (failed && !done) {
		set_errno_error(error, err,
				"The processor affinity could not be set.",
				"change");
		return FALSE;
	}
	return TRUE;
}
