// LikeOS -- taskbar, the desktop shell's panel.
//
// The dark strip along the bottom of the screen:
//
//   (orb)[ws] | [app][app][app] ...      [load] [net vol] [Mon 2 Nov  14:32]
//
// At the left are the start orb, which opens the launcher, and the Workspaces
// button, then, after a hairline, one icon-only button per application --
// the pinned ones first, then whatever else is running -- packed from the
// left.  A button stands for every window of its application: small dots
// under its icon count the windows, and the focused application's tile stays
// lit with its dots in the accent colour.  At the right are the load
// monitor, the network and volume icons (one tile for the two, which opens
// the Control Center) and the one-line clock, which opens the calendar.  A
// right-click on the empty panel offers Show desktop and the rest of the
// panel's own menu.
//
// The orb opens the launcher: the user and the session buttons along its
// top, the search field, a switch between Favorites, All apps and Recent
// files, and the places along its foot.  The Super key opens it too.
//
// It replaces ctwm's icon manager (which was the task list) and ctwm's
// workspace manager (which was the workspace switcher -- now the Workspaces
// flyout).  Both are switched off in system.ctwmrc -- ctwm still manages
// windows, decorations, placement and focus, and this program only observes
// and asks.
//
// EVERYTHING IT KNOWS COMES FROM EWMH, not from ctwm.  The window list is
// _NET_CLIENT_LIST, the focused window is _NET_ACTIVE_WINDOW, the workspace is
// _NET_CURRENT_DESKTOP, and it acts by sending the client messages the spec
// defines.  There is no ctwm-specific code here at all, which is the point:
// the panel is a shell component, not a window manager plugin, and it would
// work against any EWMH window manager.
//
// The one thing EWMH cannot tell us is whether a window is MINIMISED.  ctwm
// does not advertise _NET_WM_STATE_HIDDEN, so iconic state is read from the
// ICCCM property every window manager has set since X11R4: WM_STATE, whose
// first word is IconicState when the window is minimised.  Minimising is sent
// the ICCCM way too, as a WM_CHANGE_STATE client message; there is no EWMH
// message for it, which is a gap in the spec rather than in ctwm.
//
// The panel is drawn as one GtkDrawingArea with manual hit-testing rather
// than as a box of GtkButtons.  The look wanted here is a specific one --
// exact colours, tiles that only appear under the pointer, window dots -- and
// every one of those is a fight with a GTK theme that has its own ideas.
// Drawing it directly is less code than overriding all of that, and the
// hit-testing is a handful of rectangle comparisons because the layout is
// computed in one place (layout()).  The flyouts are drawn the same way.  The
// launcher is the exception: it has a text field and scrolling lists, which
// are what GTK widgets are for, and is styled by the theme (_shell.scss).
//
// Copyright (C) 2026 The LikeOS Project

#include "panel.h"

#include <audio/audio.h>
#include <cairo-xlib.h>
#include <errno.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* --- layout constants ---------------------------------------------------- */

#define TILE_H 40	/* every control is a 40-pixel tile */
#define EDGE_PAD 4	/* between the screen's edges and the outermost tiles */
#define ORB_SLOT 48	/* the start orb's square, at the panel's left edge */
#define WS_W 40		/* the Workspaces button, square */
#define SEP_PAD 6	/* either side of the hairline after the Workspaces button */
#define APP_W 44	/* an application's button, icon only */
#define APP_GAP 2	/* between application buttons */
#define ICON_SIZE 24	/* window icons on the panel */
#define CHEVRON_W 28	/* the overflow button, when the load monitor gives way */
#define TRAY_W 60	/* the tray's tile: the network and the speaker */
#define TRAY_GLYPH_GAP 10 /* between the two glyphs on it */
#define TRAY_GAP 2	/* between the tray's tile and the clock's */
#define CLOCK_PAD 10	/* the clock's tile around its text */

/* The configuration this program keeps for itself: the buttons pinned to the
 * panel and the launcher's favourites, one desktop entry name a line.  (The
 * favourites' file keeps the name it has always had, so a user's list
 * survives.) */
#define PIN_DIR "likeos"
#define PIN_TASKBAR "taskbar-pinned"
#define PIN_FAVORITES "start-pinned"

/* The atoms this program uses.  Interned once at start-up; X round-trips are
 * the expensive part of talking to the server and these are used on every
 * window on every update. */
static struct {
	Atom client_list, active_window, current_desktop, number_of_desktops;
	Atom wm_desktop, wm_name, wm_visible_name, wm_state, wm_change_state;
	Atom net_wm_state, skip_taskbar, close_window;
	Atom window_type, type_dock, type_desktop, type_toolbar, type_splash;
	Atom utf8, strut_partial, wm_icon;
	Atom supporting_wm_check, startup_sound, display_switch;
	Atom showing_desktop, rootpmap, desktop_names;
	Atom max_vert, max_horz, frame_extents, workarea, moveresize_window;
	Atom net_supported;
} A;

/* One window on the panel. */
struct task {
	Window win;
	char *title;
	gboolean iconic;
	long desktop;
	GdkPixbuf *icon; /* from _NET_WM_ICON at ICON_SIZE, or NULL */
	char *res_name;	 /* WM_CLASS, lower-cased, or NULL */
	char *res_class;
};

/* An application, from its desktop entry.  The launcher lists these, and the
 * panel's buttons are made of them. */
struct app {
	char *id;	/* "likeos-explorer.desktop" */
	char *name;
	char *comment;
	char *icon;	/* an icon name or an absolute path, or NULL */
	char *exec;	/* field codes stripped */
	char *wmclass;	/* StartupWMClass, lower-cased, or NULL */
	char *bin;	/* the program's file name, lower-cased: how a window
			   without StartupWMClass is matched to it */
	char *keywords; /* lower-cased, for the search */
	gboolean hidden; /* NoDisplay: findable by its windows, never listed */
	gboolean terminal;
};

/* One application button: an application, pinned or running, and the
 * windows it has open.  Windows that match no known application get a button
 * of their own class. */
struct button {
	struct app *app;  /* or NULL */
	gboolean pinned;
	GArray *wins;	  /* indices into g_tasks */
	GdkPixbuf *icon;  /* a reference, not owned */
	char *key;	  /* what grouped the windows, when there is no app */
	double x;	  /* where layout() put it, or -1 when it did not fit */
};

static GtkWidget *g_area;
static GdkWindow *g_gdkwin;
/* The panel's window and the monitor geometry it was laid out for. */
static GtkWidget *g_win;
static GdkRectangle g_layout_geo;
static Display *g_dpy;
static Window g_root;

static GArray *g_tasks;	     /* struct task */
static GPtrArray *g_buttons; /* struct button * */
static Window g_active;      /* _NET_ACTIVE_WINDOW */
static long g_desktop;	     /* _NET_CURRENT_DESKTOP */
static long g_ndesktops = 4; /* _NET_NUMBER_OF_DESKTOPS */
static gboolean g_showing_desktop; /* _NET_SHOWING_DESKTOP */
static struct panel_load g_load;
static int g_panel_w = 800;

/* Which control the pointer is over and which is held down, for the hover and
 * pressed looks.  -1 is none; buttons are 0..n-1, the specials are the
 * negative constants below. */
#define HIT_NONE -1
#define HIT_LAUNCHER -2
#define HIT_WORKSPACES -3
#define HIT_CHEVRON -4	/* the overflow button */
#define HIT_LOAD -5	/* the load monitor */
#define HIT_TRAY -6	/* the network and the speaker */
#define HIT_CLOCK -8
static int g_hover = HIT_NONE;
static int g_pressed = HIT_NONE;
static gboolean g_bar_drawn; /* painted at least once: see startup_poll() */

/* --- X property helpers --------------------------------------------------- */

/* Every one of these runs inside a GDK error trap.  A window can be destroyed
 * between appearing in _NET_CLIENT_LIST and being asked about, and the X error
 * that follows would abort the program by default -- a taskbar that dies when
 * a window closes while it is drawing is worse than no taskbar. */
static unsigned char *prop_get(Window w, Atom prop, Atom type, int *fmt,
			       unsigned long *nitems)
{
	Atom actual_type;
	int actual_fmt;
	unsigned long after;
	unsigned char *data = NULL;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	int r = XGetWindowProperty(g_dpy, w, prop, 0, 1024 * 1024, False, type,
				   &actual_type, &actual_fmt, nitems, &after,
				   &data);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());

	if (r != Success || actual_type == None) {
		if (data)
			XFree(data);
		return NULL;
	}
	if (fmt)
		*fmt = actual_fmt;
	return data;
}

static long prop_card(Window w, Atom prop, long fallback)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, prop, XA_CARDINAL, NULL, &n);
	long v = fallback;

	if (d) {
		if (n >= 1)
			v = (long)*(unsigned long *)d;
		XFree(d);
	}
	return v;
}

static Window prop_window(Window w, Atom prop)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, prop, XA_WINDOW, NULL, &n);
	Window v = None;

	if (d) {
		if (n >= 1)
			v = *(Window *)d;
		XFree(d);
	}
	return v;
}

static gboolean prop_has_atom(Window w, Atom prop, Atom value)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, prop, XA_ATOM, NULL, &n);
	gboolean found = FALSE;

	if (d) {
		Atom *a = (Atom *)d;

		for (unsigned long i = 0; i < n; i++)
			if (a[i] == value)
				found = TRUE;
		XFree(d);
	}
	return found;
}

/* The window's title: _NET_WM_NAME (UTF-8) first, WM_NAME as the fallback for
 * clients too old to set it.  xterm is one of those. */
static char *win_title(Window w)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, A.wm_name, A.utf8, NULL, &n);

	if (d && *d) {
		char *s = g_strdup((char *)d);

		XFree(d);
		return s;
	}
	if (d)
		XFree(d);

	d = prop_get(w, XA_WM_NAME, AnyPropertyType, NULL, &n);
	if (d && *d) {
		/* Latin-1 by the letter of ICCCM; converted rather than
		 * handed to Pango raw, which would drop the whole string on
		 * one high byte. */
		char *s = g_convert((char *)d, -1, "UTF-8", "ISO-8859-1", NULL,
				    NULL, NULL);

		XFree(d);
		if (s)
			return s;
	} else if (d) {
		XFree(d);
	}
	return g_strdup("(untitled)");
}

/* WM_STATE's first word.  WithdrawnState(0), NormalState(1), IconicState(3). */
static gboolean win_iconic(Window w)
{
	unsigned long n = 0;
	int fmt = 0;
	unsigned char *d = prop_get(w, A.wm_state, A.wm_state, &fmt, &n);
	gboolean ic = FALSE;

	if (d) {
		if (n >= 1)
			ic = (*(unsigned long *)d == IconicState);
		XFree(d);
	}
	return ic;
}

/* The smallest _NET_WM_ICON that is still at least ICON_SIZE square, scaled
 * to ICON_SIZE.
 *
 * The property is a list of (width, height, ARGB pixels...) runs.  Picking the
 * smallest adequate one rather than the first keeps a 256x256 icon from being
 * downsampled to 24 pixels, which turns detailed artwork into mud. */
static GdkPixbuf *win_icon(Window w)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, A.wm_icon, XA_CARDINAL, NULL, &n);

	if (!d)
		return NULL;

	unsigned long *p = (unsigned long *)d;
	unsigned long i = 0;
	unsigned long best_off = 0, best_w = 0, best_h = 0;

	while (i + 2 <= n) {
		unsigned long iw = p[i], ih = p[i + 1];

		if (iw == 0 || ih == 0 || i + 2 + iw * ih > n)
			break;
		if (iw >= ICON_SIZE && (best_w == 0 || iw < best_w)) {
			best_w = iw;
			best_h = ih;
			best_off = i + 2;
		}
		i += 2 + iw * ih;
	}
	/* Nothing big enough: take the largest there is, rather than no
	 * icon at all. */
	if (best_w == 0) {
		i = 0;
		while (i + 2 <= n) {
			unsigned long iw = p[i], ih = p[i + 1];

			if (iw == 0 || ih == 0 || i + 2 + iw * ih > n)
				break;
			if (iw > best_w) {
				best_w = iw;
				best_h = ih;
				best_off = i + 2;
			}
			i += 2 + iw * ih;
		}
	}
	if (best_w == 0) {
		XFree(d);
		return NULL;
	}

	GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
				       (int)best_w, (int)best_h);

	if (!pb) {
		XFree(d);
		return NULL;
	}

	guchar *px = gdk_pixbuf_get_pixels(pb);
	int stride = gdk_pixbuf_get_rowstride(pb);

	/* The property is ARGB packed into 32 bits per element (each element
	 * is a long, so 64 bits wide on this machine and the top half is
	 * padding); GdkPixbuf wants RGBA bytes. */
	for (unsigned long yy = 0; yy < best_h; yy++) {
		for (unsigned long xx = 0; xx < best_w; xx++) {
			unsigned long v = p[best_off + yy * best_w + xx];
			guchar *o = px + yy * stride + xx * 4;

			o[0] = (v >> 16) & 0xff;
			o[1] = (v >> 8) & 0xff;
			o[2] = v & 0xff;
			o[3] = (v >> 24) & 0xff;
		}
	}
	XFree(d);

	GdkPixbuf *small = gdk_pixbuf_scale_simple(pb, ICON_SIZE, ICON_SIZE,
						   GDK_INTERP_BILINEAR);

	g_object_unref(pb);
	return small;
}

/* WM_CLASS, both halves lower-cased: what a window is matched to its
 * application by, and what groups the windows of an application that has no
 * desktop entry. */
static void win_class(Window w, char **name, char **cls)
{
	XClassHint ch = { NULL, NULL };

	*name = *cls = NULL;
	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XGetClassHint(g_dpy, w, &ch)) {
		if (ch.res_name && *ch.res_name)
			*name = g_ascii_strdown(ch.res_name, -1);
		if (ch.res_class && *ch.res_class)
			*cls = g_ascii_strdown(ch.res_class, -1);
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	if (ch.res_name)
		XFree(ch.res_name);
	if (ch.res_class)
		XFree(ch.res_class);
}

/* Should this window get a task button?
 *
 * Skipped: the panel and the desktop (by window type), anything asking to be
 * skipped (GTK sets that for its own utility windows), and anything on another
 * workspace -- the panel shows the current workspace's windows only, so a
 * workspace switch is a change of task list too.  Windows with
 * _NET_WM_DESKTOP of 0xffffffff are on all desktops and always shown. */
static gboolean want_task(Window w, long *out_desktop)
{
	if (prop_has_atom(w, A.window_type, A.type_dock) ||
	    prop_has_atom(w, A.window_type, A.type_desktop) ||
	    prop_has_atom(w, A.window_type, A.type_toolbar) ||
	    prop_has_atom(w, A.window_type, A.type_splash))
		return FALSE;
	if (prop_has_atom(w, A.net_wm_state, A.skip_taskbar))
		return FALSE;

	long d = prop_card(w, A.wm_desktop, -1);

	*out_desktop = d;
	if (d >= 0 && d != 0x7fffffff && (unsigned long)d != 0xffffffffUL &&
	    d != g_desktop)
		return FALSE;
	return TRUE;
}

/* --- acting on windows ---------------------------------------------------- */

static void send_root_message(Window w, Atom type, long d0, long d1, long d2)
{
	XClientMessageEvent ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.window = w;
	ev.message_type = type;
	ev.format = 32;
	ev.data.l[0] = d0;
	ev.data.l[1] = d1;
	ev.data.l[2] = d2;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	XSendEvent(g_dpy, g_root, False,
		   SubstructureNotifyMask | SubstructureRedirectMask,
		   (XEvent *)&ev);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
}

/* Bring a window to the front: deiconify it if need be, raise it, focus it.
 *
 * One _NET_ACTIVE_WINDOW message does all three.  ctwm's handler deiconifies a
 * window that is not mapped, raises it when RaiseOnWarp is set (its default),
 * and sets the focus -- which is exactly what clicking a task button means. */
static void task_activate(Window w)
{
	send_root_message(w, A.active_window, 2 /* source: pager */,
			  CurrentTime, 0);
}

/* Minimise.  There is no EWMH message for this, so it goes the ICCCM way:
 * WM_CHANGE_STATE carrying IconicState, which every window manager has
 * understood since X11R4 and which ctwm turns into its own iconify. */
static void task_minimise(Window w)
{
	send_root_message(w, A.wm_change_state, IconicState, 0, 0);
}

static void task_close(Window w)
{
	send_root_message(w, A.close_window, CurrentTime, 2, 0);
}

static void goto_desktop(long n)
{
	send_root_message(g_root, A.current_desktop, n, CurrentTime, 0);
}

/* Show desktop, from the panel's menu or Super+D: every window out of the
 * way, or back.  EWMH has the message for it, and ctwm answers it. */
static void toggle_show_desktop(void)
{
	g_showing_desktop = prop_card(g_root, A.showing_desktop, 0) != 0;
	send_root_message(g_root, A.showing_desktop, !g_showing_desktop, 0, 0);
}

/* --- the task list --------------------------------------------------------- */

static void tasks_clear(void)
{
	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);

		g_free(t->title);
		g_free(t->res_name);
		g_free(t->res_class);
		if (t->icon)
			g_object_unref(t->icon);
	}
	g_array_set_size(g_tasks, 0);
}

/* Rebuild the list from _NET_CLIENT_LIST.
 *
 * Rebuilt wholesale on every change rather than diffed.  The list is a handful
 * of windows and each one costs four properties; the alternative is tracking
 * additions and removals against a cache, which is more code and one more
 * thing that can disagree with the server. */
/* Add `mask' to the events this program's X connection receives from `w'.
 *
 * Added, never set: an X client has ONE event mask per window, and the
 * connection used here is GDK's own.  On the root and, above all, on this
 * program's own windows -- its confirmation dialogs are managed windows on
 * _NET_CLIENT_LIST like any other -- GDK has selected what it lives on, and
 * XSelectInput() with a bare PropertyChangeMask threw that away.  For a new
 * dialog that meant no MapNotify: GDK keeps a new toplevel's frame clock
 * frozen until the window is mapped, so the dialog was never drawn (a white
 * rectangle), and being modal it took every click on the bar with it.
 * Whether it happened depended on whether this program or the window
 * manager's map reached the server first. */
static void select_more_input(Window w, long mask)
{
	GdkDisplay *disp = gdk_display_get_default();
	XWindowAttributes wa;

	gdk_x11_display_error_trap_push(disp);
	if (XGetWindowAttributes(g_dpy, w, &wa) &&
	    (wa.your_event_mask & mask) != mask)
		XSelectInput(g_dpy, w, wa.your_event_mask | mask);
	gdk_x11_display_error_trap_pop_ignored(disp);
}

static void buttons_rebuild(void);

static void tasks_refresh(void)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.client_list, XA_WINDOW, NULL, &n);

	tasks_clear();
	if (!d)
		goto out;

	Window *wins = (Window *)d;

	for (unsigned long i = 0; i < n; i++) {
		long desk = -1;

		if (!want_task(wins[i], &desk))
			continue;

		struct task t;

		memset(&t, 0, sizeof(t));
		t.win = wins[i];
		t.title = win_title(wins[i]);
		t.iconic = win_iconic(wins[i]);
		t.desktop = desk;
		t.icon = win_icon(wins[i]);
		win_class(wins[i], &t.res_name, &t.res_class);
		g_array_append_val(g_tasks, t);

		/* Watch this window for title and state changes.  Without it
		 * a window that is minimised, renamed or restored would keep
		 * its old button until something else happened to trigger a
		 * refresh. */
		select_more_input(wins[i], PropertyChangeMask);
	}
	XFree(d);
out:
	g_active = prop_window(g_root, A.active_window);
	buttons_rebuild();
	if (g_area)
		gtk_widget_queue_draw(g_area);
}

/* --- applications ---------------------------------------------------------- */

/* Every application the desktop entries describe, sorted by name, and the
 * same keyed by entry name.  Read at start-up and again each time the
 * launcher opens -- a score of small files, and a program installed meanwhile
 * shows up without anything having to watch for it. */
static GPtrArray *g_apps;
static GHashTable *g_app_by_id;

/* Icons at the sizes asked for, keyed by "name@size".  Dropped with the
 * catalogue, and when the icon theme changes. */
static GHashTable *g_icon_cache;

static void app_free(gpointer p)
{
	struct app *a = p;

	g_free(a->id);
	g_free(a->name);
	g_free(a->comment);
	g_free(a->icon);
	g_free(a->exec);
	g_free(a->wmclass);
	g_free(a->bin);
	g_free(a->keywords);
	g_free(a);
}

/* Strip the field codes a desktop Exec line may carry (%f, %U, ...).
 *
 * They name files to open, and nothing is being opened here.  Left in, they
 * would be passed to the program as literal arguments.  "%%" is a literal
 * percent sign. */
static char *clean_exec(const char *exec)
{
	GString *s = g_string_new(NULL);

	for (const char *p = exec; *p; p++) {
		if (p[0] == '%' && p[1]) {
			p++;
			if (*p == '%')
				g_string_append_c(s, '%');
			continue;
		}
		g_string_append_c(s, *p);
	}
	return g_strstrip(g_string_free(s, FALSE));
}

/* The file name of the program an Exec line runs, lower-cased: "env A=b
 * /usr/bin/xterm -ls" is "xterm".  A window whose WM_CLASS says the same
 * belongs to this entry. */
static char *exec_bin(const char *exec)
{
	char **argv = NULL;
	char *bin = NULL;
	int argc = 0;

	if (!g_shell_parse_argv(exec, &argc, &argv, NULL))
		return NULL;
	for (int i = 0; i < argc && !bin; i++) {
		char *base = g_path_get_basename(argv[i]);

		/* "env" and the assignments after it are not the program. */
		if (!(i == 0 && strcmp(base, "env") == 0) &&
		    !(strchr(argv[i], '=') && argv[i][0] != '/'))
			bin = g_ascii_strdown(base, -1);
		g_free(base);
	}
	g_strfreev(argv);
	return bin;
}

/* Read one desktop entry.  NULL for anything that is not an application this
 * session can run: another type, Hidden, a TryExec that is not installed, or
 * no Exec line at all. */
static struct app *app_load(const char *path, const char *id)
{
	GKeyFile *kf = g_key_file_new();
	struct app *a = NULL;
	const char *G = G_KEY_FILE_DESKTOP_GROUP;
	char *type = NULL, *exec = NULL, *tryexec = NULL;

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
		goto out;
	type = g_key_file_get_string(kf, G, "Type", NULL);
	exec = g_key_file_get_string(kf, G, "Exec", NULL);
	if (g_strcmp0(type, "Application") != 0 || !exec || !*exec ||
	    g_key_file_get_boolean(kf, G, "Hidden", NULL))
		goto out;
	tryexec = g_key_file_get_string(kf, G, "TryExec", NULL);
	if (tryexec && *tryexec) {
		char *found = g_find_program_in_path(tryexec);

		if (!found)
			goto out;
		g_free(found);
	}

	a = g_new0(struct app, 1);
	a->id = g_strdup(id);
	a->name = g_key_file_get_locale_string(kf, G, "Name", NULL, NULL);
	if (!a->name)
		a->name = g_strdup(id);
	a->comment = g_key_file_get_locale_string(kf, G, "Comment", NULL, NULL);
	a->icon = g_key_file_get_string(kf, G, "Icon", NULL);
	a->exec = clean_exec(exec);
	a->bin = exec_bin(a->exec);
	a->hidden = g_key_file_get_boolean(kf, G, "NoDisplay", NULL);
	a->terminal = g_key_file_get_boolean(kf, G, "Terminal", NULL);

	char *wm = g_key_file_get_string(kf, G, "StartupWMClass", NULL);

	if (wm && *wm)
		a->wmclass = g_ascii_strdown(wm, -1);
	g_free(wm);

	char *kw = g_key_file_get_locale_string(kf, G, "Keywords", NULL, NULL);
	char *gn = g_key_file_get_locale_string(kf, G, "GenericName", NULL,
						NULL);
	char *all = g_strjoin(" ", kw ? kw : "", gn ? gn : "",
			      a->comment ? a->comment : "", a->bin ? a->bin : "",
			      NULL);

	a->keywords = g_utf8_strdown(all, -1);
	g_free(all);
	g_free(kw);
	g_free(gn);
out:
	g_free(type);
	g_free(exec);
	g_free(tryexec);
	g_key_file_free(kf);
	return a;
}

static void apps_scan_dir(const char *base)
{
	char *dir = g_build_filename(base, "applications", NULL);
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *fn;

	if (d) {
		while ((fn = g_dir_read_name(d))) {
			if (!g_str_has_suffix(fn, ".desktop") ||
			    g_hash_table_contains(g_app_by_id, fn))
				continue;

			char *path = g_build_filename(dir, fn, NULL);
			struct app *a = app_load(path, fn);

			/* A user's entry that is Hidden still shadows the
			 * system's one of the same name: that is how an
			 * application is removed from the menus. */
			if (a)
				g_ptr_array_add(g_apps, a);
			g_hash_table_insert(g_app_by_id, g_strdup(fn), a);
			g_free(path);
		}
		g_dir_close(d);
	}
	g_free(dir);
}

static gint app_cmp(gconstpointer pa, gconstpointer pb)
{
	const struct app *a = *(struct app *const *)pa;
	const struct app *b = *(struct app *const *)pb;
	char *ka = g_utf8_casefold(a->name, -1), *kb = g_utf8_casefold(b->name, -1);
	gint r = g_utf8_collate(ka, kb);

	g_free(ka);
	g_free(kb);
	return r;
}

/* The Display settings dialog has no desktop entry of its own -- it is part of
 * this shell -- but it belongs in the launcher like any other program. */
static void apps_add_builtin(void)
{
	if (g_hash_table_contains(g_app_by_id, "likeos-display-settings.desktop"))
		return;

	struct app *a = g_new0(struct app, 1);

	a->id = g_strdup("likeos-display-settings.desktop");
	a->name = g_strdup("Settings");
	a->comment = g_strdup("Display resolution and layout");
	a->icon = g_strdup("preferences-desktop-display");
	a->exec = g_strdup("/usr/bin/likeos-display-settings");
	a->bin = g_strdup("likeos-display-settings");
	a->keywords = g_strdup("display screen resolution monitor settings "
			       "control panel");
	g_ptr_array_add(g_apps, a);
	g_hash_table_insert(g_app_by_id, g_strdup(a->id), a);
}

static void apps_load(void)
{
	if (g_apps)
		g_ptr_array_unref(g_apps);
	if (g_app_by_id)
		g_hash_table_unref(g_app_by_id);
	g_apps = g_ptr_array_new_with_free_func(app_free);
	g_app_by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					    NULL);

	/* The user's own entries first: the first of a name wins. */
	apps_scan_dir(g_get_user_data_dir());
	for (const char *const *d = g_get_system_data_dirs(); *d; d++)
		apps_scan_dir(*d);
	apps_add_builtin();
	g_ptr_array_sort(g_apps, app_cmp);
	if (g_icon_cache)
		g_hash_table_remove_all(g_icon_cache);
}

static struct app *app_by_id(const char *id)
{
	return id && g_app_by_id ? g_hash_table_lookup(g_app_by_id, id) : NULL;
}

/* An icon by name or path at `size' pixels, from the cache.  The reference is
 * the cache's. */
static GdkPixbuf *icon_load(const char *name, int size)
{
	char key[256];
	GdkPixbuf *pb;

	if (!name || !*name)
		return NULL;
	if (!g_icon_cache)
		g_icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
						     g_free, g_object_unref);
	g_snprintf(key, sizeof(key), "%s@%d", name, size);
	pb = g_hash_table_lookup(g_icon_cache, key);
	if (pb)
		return pb;

	if (g_path_is_absolute(name)) {
		pb = gdk_pixbuf_new_from_file_at_size(name, size, size, NULL);
	} else {
		/* "foo.png" in an Icon line is a mistake the specification
		 * tolerates: the name is the part before the extension. */
		char *bare = g_strdup(name);
		char *dot = strrchr(bare, '.');

		if (dot && (!strcmp(dot, ".png") || !strcmp(dot, ".svg") ||
			    !strcmp(dot, ".xpm")))
			*dot = '\0';
		pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), bare,
					      size, GTK_ICON_LOOKUP_FORCE_SIZE,
					      NULL);
		g_free(bare);
	}
	if (pb)
		g_hash_table_insert(g_icon_cache, g_strdup(key), pb);
	return pb;
}

/* The icon for an application, falling back to the generic program icon so
 * that no button or tile is ever blank. */
static GdkPixbuf *app_icon(const struct app *a, int size)
{
	GdkPixbuf *pb = a ? icon_load(a->icon, size) : NULL;

	return pb ? pb : icon_load("application-x-executable", size);
}

/* Start a program the way its entry says.  A Terminal=true entry runs in the
 * terminal this desktop uses for that. */
static void spawn_command(const char *cmd)
{
	GError *err = NULL;

	if (!g_spawn_command_line_async(cmd, &err)) {
		g_warning("cannot start %s: %s", cmd, err ? err->message : "?");
		g_clear_error(&err);
	}
}

static void app_launch(const struct app *a)
{
	if (!a)
		return;
	if (a->terminal) {
		char *cmd = g_strdup_printf("xterm -e %s", a->exec);

		spawn_command(cmd);
		g_free(cmd);
	} else {
		spawn_command(a->exec);
	}
}

/* Which application a window belongs to: its StartupWMClass first, then the
 * program's file name, then the entry's own name ("org.xfce.mousepad" matches
 * a "mousepad" window).  A listed entry wins over a NoDisplay one that matches
 * the same way -- a terminal's preferences dialog has the terminal's program
 * name too. */
static gboolean key_is(const char *k, const struct task *t)
{
	return k && *k && (g_strcmp0(k, t->res_class) == 0 ||
			   g_strcmp0(k, t->res_name) == 0);
}

static struct app *app_for_task(const struct task *t)
{
	if (!g_apps || (!t->res_name && !t->res_class))
		return NULL;
	for (int pass = 0; pass < 6; pass++) {
		gboolean hidden = pass & 1;

		for (guint i = 0; i < g_apps->len; i++) {
			struct app *a = g_ptr_array_index(g_apps, i);
			gboolean hit = FALSE;

			if (a->hidden != hidden)
				continue;
			if (pass < 2) {
				hit = key_is(a->wmclass, t);
			} else if (pass < 4) {
				hit = !a->wmclass && key_is(a->bin, t);
			} else {
				char *stem = g_ascii_strdown(a->id, -1);
				char *dot = g_strrstr(stem, ".desktop");

				if (dot)
					*dot = '\0';
				dot = strrchr(stem, '.');
				hit = key_is(dot ? dot + 1 : stem, t);
				g_free(stem);
			}
			if (hit)
				return a;
		}
	}
	return NULL;
}

/* --- pinned applications --------------------------------------------------- */

/* The buttons pinned to the panel and the launcher's favourites, as lists of
 * entry names in ~/.config/likeos.  Until the user changes them, these are
 * the defaults; an entry that is not installed is skipped. */
static const char *const g_default_bar_pins[] = {
	"likeos-explorer.desktop", "luakit.desktop", "xfce4-terminal.desktop",
	"mediaplayer.desktop", NULL
};

static const char *const g_default_favorites[] = {
	"likeos-explorer.desktop", "luakit.desktop", "xfce4-terminal.desktop",
	"org.xfce.mousepad.desktop", "mediaplayer.desktop",
	"claws-mail.desktop", "io.github.Hexchat.desktop",
	"galculator.desktop", "xterm.desktop",
	"likeos-display-settings.desktop", NULL
};

static GPtrArray *g_pin_bar, *g_favorites; /* char * */

static char *pin_path(const char *file)
{
	return g_build_filename(g_get_user_config_dir(), PIN_DIR, file, NULL);
}

static GPtrArray *pins_read(const char *file, const char *const *defaults)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
	char *path = pin_path(file);
	char *text = NULL;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (char **l = lines; *l; l++) {
			g_strstrip(*l);
			if (**l && **l != '#')
				g_ptr_array_add(list, g_strdup(*l));
		}
		g_strfreev(lines);
		g_free(text);
	} else {
		for (const char *const *d = defaults; *d; d++)
			g_ptr_array_add(list, g_strdup(*d));
	}
	g_free(path);
	return list;
}

static void pins_write(GPtrArray *list, const char *file)
{
	char *path = pin_path(file);
	char *dir = g_path_get_dirname(path);
	GString *s = g_string_new("# Pinned applications, one desktop entry a "
				  "line.  Written by taskbar(1).\n");

	for (guint i = 0; i < list->len; i++)
		g_string_append_printf(s, "%s\n",
				       (char *)g_ptr_array_index(list, i));
	g_mkdir_with_parents(dir, 0755);
	if (!g_file_set_contents(path, s->str, -1, NULL))
		g_warning("cannot save %s", path);
	g_string_free(s, TRUE);
	g_free(dir);
	g_free(path);
}

static int pin_index(GPtrArray *list, const char *id)
{
	for (guint i = 0; id && i < list->len; i++)
		if (g_strcmp0(g_ptr_array_index(list, i), id) == 0)
			return (int)i;
	return -1;
}

static void pin_toggle(GPtrArray *list, const char *file, const char *id)
{
	int i = pin_index(list, id);

	if (i >= 0)
		g_ptr_array_remove_index(list, i);
	else
		g_ptr_array_add(list, g_strdup(id));
	pins_write(list, file);
}

/* --- the buttons ----------------------------------------------------------- */

static void button_free(gpointer p)
{
	struct button *b = p;

	g_array_unref(b->wins);
	g_free(b->key);
	g_free(b);
}

static struct button *button_add(struct app *a, gboolean pinned,
				 const char *key)
{
	struct button *b = g_new0(struct button, 1);

	b->app = a;
	b->pinned = pinned;
	b->wins = g_array_new(FALSE, FALSE, sizeof(guint));
	b->key = g_strdup(key);
	b->x = -1;
	g_ptr_array_add(g_buttons, b);
	return b;
}

/* The icon for a window that has none of its own. */
static GdkPixbuf *fallback_icon(void)
{
	return icon_load("application-x-executable", ICON_SIZE);
}

/* Lay the windows out as buttons: the pinned applications in their order,
 * running or not, then every other application that has a window, in the
 * order its first window appeared.  Windows of no known application are
 * grouped by their WM_CLASS. */
static void buttons_rebuild(void)
{
	if (!g_buttons)
		g_buttons = g_ptr_array_new_with_free_func(button_free);
	g_ptr_array_set_size(g_buttons, 0);

	for (guint i = 0; g_pin_bar && i < g_pin_bar->len; i++) {
		struct app *a = app_by_id(g_ptr_array_index(g_pin_bar, i));

		if (a)
			button_add(a, TRUE, NULL);
	}

	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);
		struct app *a = app_for_task(t);
		const char *key = t->res_class ? t->res_class : t->res_name;
		struct button *b = NULL;

		for (guint j = 0; j < g_buttons->len && !b; j++) {
			struct button *c = g_ptr_array_index(g_buttons, j);

			if (a ? c->app == a :
				(!c->app && key && g_strcmp0(c->key, key) == 0))
				b = c;
		}
		if (!b)
			b = button_add(a, FALSE, key);
		g_array_append_val(b->wins, i);
	}

	for (guint j = 0; j < g_buttons->len; j++) {
		struct button *b = g_ptr_array_index(g_buttons, j);

		b->icon = NULL;
		if (b->app)
			b->icon = icon_load(b->app->icon, ICON_SIZE);
		if (!b->icon && b->wins->len)
			b->icon = g_array_index(g_tasks, struct task,
						g_array_index(b->wins, guint, 0))
					  .icon;
		if (!b->icon)
			b->icon = fallback_icon();
	}
}

static struct task *button_task(const struct button *b, guint k)
{
	return &g_array_index(g_tasks, struct task,
			      g_array_index(b->wins, guint, k));
}

/* Has the button the focused window?  Its tile stays lit and its pill is the
 * long one. */
static gboolean button_active(const struct button *b)
{
	for (guint k = 0; k < b->wins->len; k++) {
		struct task *t = button_task(b, k);

		if (t->win == g_active && !t->iconic)
			return TRUE;
	}
	return FALSE;
}

/* What the tooltip says: the window's title, or the application's name when
 * it has no window or several. */
static const char *button_title(const struct button *b)
{
	if (b->wins->len == 1)
		return button_task(b, 0)->title;
	if (b->app)
		return b->app->name;
	if (b->wins->len)
		return button_task(b, 0)->title;
	return "";
}

/* --- the volume control: state and the audio thread ------------------------- */

/* Output kinds, numbered as <sys/audio.h> numbers them.
 * That header describes the kernel's audio devices and is not part of the X
 * sysroot; audiod passes the numbers through unchanged, so they are restated
 * here rather than the header being dragged in for a few constants. */
#ifndef AUDIO_OUT_SPEAKER
#define AUDIO_OUT_SPEAKER 1
#define AUDIO_OUT_HEADPHONE 2
#define AUDIO_OUT_LINE 3
#define AUDIO_OUT_HDMI 4
#define AUDIO_OUT_DP 5
#define AUDIO_OUT_SPDIF 6
#define AUDIO_OUT_HEADSET 7
#endif
#ifndef AUDIO_OUTSEL_AUTO
#define AUDIO_OUTSEL_AUTO 0
#define AUDIO_OUTSEL_ID 1
#endif

/* The Control Center's state; the flyout itself is further down, with the
 * menus.
 *
 * It is a vertical list: the volume row at the top, then one row per thing
 * this system can set from here (the network's details, the display, the
 * workspaces), and a footer with Settings and the system monitor.  The
 * session's own buttons are the launcher's, not this.  The sizes are the shell's: 380 pixels wide, rows of 56, corners of 14. */
#define FLY_W 380	  /* the panel, its 1-pixel border included */
#define FLY_PAD 8	  /* rows' inset from the border */
#define FLY_VOL_H 60	  /* the volume row */
#define FLY_ROW_H 56	  /* every other row */
#define FLY_INFO_H 44	  /* the network's details, when shown */
#define FLY_FOOT_H 56	  /* the footer strip */
#define FLY_LIST_HEAD 52  /* the outputs page's heading */
#define FLY_LIST_ROW 44	  /* one output */
#define FLY_NROWS 3	  /* network, display, workspaces */
#define FLY_BTN 36	  /* a round-cornered icon button */
#define FLY_FOOT_SETTINGS_W 104 /* the footer's two buttons */
#define FLY_FOOT_MONITOR_W 144
#define FLY_RADIUS 14
#define FLY_GAP 8	  /* to the panel and to the screen's right edge */
#define FLY_SHADOW 24	  /* margin for the shadow, when there is a compositor */
#define TIP_SHADOW 10

/* The volume row, as offsets from the panel's left edge. */
#define FLY_SPK_X 16	  /* the speaker button, in line with the rows' glyphs */
#define FLY_TRACK_X 62	  /* the slider track... */
#define FLY_TRACK_R 60	  /* ... ends this far from the right edge, before the
			     percentage */

#define TIP_FONT "Selawik 9"
#define FLY_FONT "Selawik 9"
#define FLY_FONT_BOLD "Selawik Semibold 9"

/* Hit codes inside the flyout. */
#define FH_OUTSIDE -1
#define FH_NONE 0
#define FH_MUTE 1
#define FH_SLIDER 2
#define FH_SETTINGS 3	  /* the footer's Settings button */
#define FH_ROW 4	  /* 4..6: network, display, workspaces */
#define FH_OUTPUTS 8	  /* the chevron that lists the outputs */
#define FH_BACK 9	  /* back from that list */
#define FH_MONITOR 10	  /* the footer's System monitor button */
#define FH_OUT 20	  /* 20..: a row of the outputs page; 20 is "Automatic" */

static struct {
	GtkWidget *win, *tip;
	gboolean up, composited;
	int margin, tip_margin;
	int hover, pressed;
	gboolean dragging;
	double knob;	 /* the halo round the thumb now, animated toward */
	guint tick;	 /* ... the radius fly_knob_target() names */
	gint64 tick_last;
	int tip_w, tip_h;
	int grab_tries;
	int page;	 /* 0 the list, 1 the list of outputs */
	gboolean net_info; /* the network row's details are shown */
	int placed_h;	 /* the height fly_place() last gave it */
	int owner;	 /* the panel control it belongs to: HIT_TRAY */
} F = { .knob = 9, .owner = HIT_TRAY };

/* What audiod last told us, as the main thread sees it. */
struct vol_state {
	gboolean online;	 /* connected to audiod */
	int vol;		 /* 0..100: the louder of the two channels */
	gboolean muted;
	gboolean have_outs;	 /* outs is valid */
	struct aud_outputs outs;
	guint seq;		 /* the newest request already applied */
};

static struct vol_state V;

/* The newest volume or mute change the user asked for.  A report from the
 * audio thread that has not applied it yet still carries the old level, and
 * taking that would make the slider jump back under the pointer. */
static guint g_vol_seq;

/* A level set by the user and not yet sent (see vol_send()), or -1. */
static int g_vol_unsent = -1;

/* Every exchange with audiod runs on a thread of its own.
 *
 * libaudio's requests are synchronous: each waits for its reply, for up to
 * five seconds when the service is stuck, and connecting retries for a few
 * hundred milliseconds when the service is not there at all.  On the GTK
 * thread any of that freezes the whole bar -- clock, task buttons, menus --
 * so the bar only ever posts what it wants here and gets reports back through
 * the main loop.  The thread owns the control handle outright, which is the
 * one-thread-per-handle rule libaudio asks for.
 *
 * Requests are coalesced rather than queued: while a slider is dragged only
 * the newest level matters, and a queue would replay every step of the drag
 * to a service that might have been slow for a moment. */
#define AUDIO_EVENTS 4
#define AUDIO_RETRY_US (3 * G_USEC_PER_SEC)

static struct {
	gboolean running;  /* the thread exists; nothing below is used before */
	GMutex lock;
	int wake[2];	   /* the main thread writes a byte to wake the worker */
	guint seq;
	gboolean set_vol, set_mute, set_out;
	int vol, mute;
	uint32_t out_mode, out_id;
	int nev;
	char ev[AUDIO_EVENTS][AUD_EVENT_NAME_MAX];
	gint64 ev_until[AUDIO_EVENTS]; /* monotonic time it stops being worth playing */
} Q;

static void vol_state_changed(void);

static void audio_kick(void)
{
	char c = 1;

	if (!Q.running)
		return;

	ssize_t r = write(Q.wake[1], &c, 1);

	(void)r; /* a full pipe is already a pending wake-up */
}

static void audio_request_volume(int v)
{
	g_mutex_lock(&Q.lock);
	Q.set_vol = TRUE;
	Q.vol = v;
	Q.seq = ++g_vol_seq;
	g_mutex_unlock(&Q.lock);
	audio_kick();
}

/* Choose the output: AUDIO_OUTSEL_AUTO, or AUDIO_OUTSEL_ID and its id. */
static void audio_request_output(uint32_t mode, uint32_t id)
{
	g_mutex_lock(&Q.lock);
	Q.set_out = TRUE;
	Q.out_mode = mode;
	Q.out_id = id;
	g_mutex_unlock(&Q.lock);
	audio_kick();
}

static void audio_request_mute(gboolean mute)
{
	g_mutex_lock(&Q.lock);
	Q.set_mute = TRUE;
	Q.mute = mute ? 1 : 0;
	Q.seq = ++g_vol_seq;
	g_mutex_unlock(&Q.lock);
	audio_kick();
}

/* Play one of the system event sounds, if that can happen within `ms'.
 * A sound that could not be played in time is dropped: a volume ding or a
 * start-up chime arriving seconds late is worse than none. */
static void audio_play(const char *name, int ms)
{
	g_mutex_lock(&Q.lock);
	if (Q.nev < AUDIO_EVENTS) {
		g_strlcpy(Q.ev[Q.nev], name, AUD_EVENT_NAME_MAX);
		Q.ev_until[Q.nev] = g_get_monotonic_time() + (gint64)ms * 1000;
		Q.nev++;
	}
	g_mutex_unlock(&Q.lock);
	audio_kick();
}

/* A report, handed from the worker to the main loop. */
static gboolean audio_report(gpointer data)
{
	struct vol_state *r = data;
	V.online = r->online;
	if (r->online) {
		/* Not while the user is moving the level, either: a change
		 * still waiting for the throttle has not even been numbered
		 * yet. */
		if (r->seq >= g_vol_seq && !F.dragging && g_vol_unsent < 0) {
			V.vol = r->vol;
			V.muted = r->muted;
		}
		V.have_outs = r->have_outs;
		V.outs = r->outs;
	} else {
		V.have_outs = FALSE;
	}
	g_free(r);
	vol_state_changed();
	return G_SOURCE_REMOVE;
}

static void audio_post(const struct vol_state *st)
{
	struct vol_state *copy = g_new(struct vol_state, 1);

	*copy = *st;
	g_idle_add(audio_report, copy);
}

static void audio_read_outputs(la_ctl *c, struct vol_state *st)
{
	st->have_outs = la_get_outputs(c, &st->outs) == 0;
}

/* Connect, subscribe, and read the whole state.  Subscribed first, so that a
 * change landing between the reads and the subscription is not lost. */
static la_ctl *audio_connect(struct vol_state *st)
{
	uint32_t l = 0, r = 0;
	int err = 0, mute = 0;
	la_ctl *c = la_ctl_open("taskbar", &err);

	if (!c)
		return NULL;
	if (la_subscribe(c, AUD_SUB_VOLUME | AUD_SUB_OUTPUTS) < 0 ||
	    la_get_volume(c, &l, &r) < 0 || la_get_mute(c, &mute) < 0) {
		la_ctl_close(c);
		return NULL;
	}
	st->online = TRUE;
	st->vol = (int)(l > r ? l : r);
	st->muted = mute != 0;
	audio_read_outputs(c, st);
	return c;
}

/* Take what the main thread asked for.  With `c' NULL (audiod is not there)
 * the settings are forgotten and only the event sounds that still have time
 * left wait for the next attempt.  Returns TRUE when an event is waiting. */
static gboolean audio_apply(la_ctl *c, struct vol_state *st)
{
	gboolean set_vol, set_mute, set_out;
	int vol, mute, nev = 0;
	uint32_t out_mode, out_id;
	char ev[AUDIO_EVENTS][AUD_EVENT_NAME_MAX];
	gint64 now = g_get_monotonic_time();
	gboolean waiting = FALSE;

	g_mutex_lock(&Q.lock);
	set_vol = Q.set_vol;
	set_mute = Q.set_mute;
	vol = Q.vol;
	mute = Q.mute;
	set_out = Q.set_out;
	out_mode = Q.out_mode;
	out_id = Q.out_id;
	Q.set_vol = Q.set_mute = Q.set_out = FALSE;
	st->seq = Q.seq;

	int keep = 0;

	for (int i = 0; i < Q.nev; i++) {
		if (Q.ev_until[i] < now)
			continue;
		if (c) {
			memcpy(ev[nev++], Q.ev[i], AUD_EVENT_NAME_MAX);
		} else {
			if (keep != i) {
				memcpy(Q.ev[keep], Q.ev[i], AUD_EVENT_NAME_MAX);
				Q.ev_until[keep] = Q.ev_until[i];
			}
			keep++;
		}
	}
	Q.nev = keep;
	waiting = keep > 0;
	g_mutex_unlock(&Q.lock);

	if (!c)
		return waiting;

	/* Volume before the sounds, so the ding that follows a change is
	 * heard at the new level. */
	if (set_vol && la_set_volume_master(c, (uint32_t)vol, (uint32_t)vol) == 0)
		st->vol = vol;
	if (set_mute && la_set_mute_master(c, mute) == 0)
		st->muted = mute != 0;
	if (set_out && la_set_output(c, out_mode, out_id) == 0)
		audio_read_outputs(c, st);
	for (int i = 0; i < nev; i++)
		la_play_event(c, ev[i], 100, 0);
	return FALSE;
}

/* Read every notification waiting.  Returns 1 when the state changed, 0 when
 * not, and a negative errno when the connection is gone. */
static int audio_drain(la_ctl *c, struct vol_state *st)
{
	struct aud_notify n;
	gboolean outs = FALSE;
	int changed = 0, r;

	while ((r = la_next_notify(c, &n)) > 0) {
		if (n.what == AUD_NOTIFY_VOLUME) {
			st->vol = (int)(n.a > n.b ? n.a : n.b);
			st->muted = n.c != 0;
			changed = 1;
		} else if (n.what == AUD_NOTIFY_OUTPUT) {
			outs = TRUE;
		}
	}
	if (r < 0)
		return r;
	if (outs) {
		audio_read_outputs(c, st);
		changed = 1;
	}
	return changed;
}

static gpointer audio_thread(gpointer data)
{
	struct vol_state st;
	la_ctl *c = NULL;
	gint64 next_try = 0;
	gboolean kicked = FALSE;

	(void)data;
	memset(&st, 0, sizeof(st));

	for (;;) {
		gint64 now = g_get_monotonic_time();

		/* Not connected: try again every few seconds, and at once
		 * when there is something to do -- audiod may have come up
		 * since the last attempt and a sound is waiting. */
		if (!c && (now >= next_try || kicked)) {
			c = audio_connect(&st);
			if (c) {
				audio_post(&st);
			} else {
				next_try = now + AUDIO_RETRY_US;
				if (st.online) {
					st.online = FALSE;
					audio_post(&st);
				}
			}
		}

		gboolean waiting = audio_apply(c, &st);

		if (c) {
			int r = audio_drain(c, &st);

			if (r < 0) {
				/* audiod went away (or restarted).  Reconnect
				 * soon: a restart brings it straight back. */
				la_ctl_close(c);
				c = NULL;
				st.online = FALSE;
				next_try = now + G_USEC_PER_SEC;
			}
			audio_post(&st);
		}

		struct pollfd pfd[2];
		int nfd = 1, timeout = -1;

		pfd[0].fd = Q.wake[0];
		pfd[0].events = POLLIN;
		if (c) {
			pfd[1].fd = la_ctl_fd(c);
			pfd[1].events = POLLIN;
			nfd = 2;
		} else {
			gint64 left = next_try - g_get_monotonic_time();

			timeout = left > 0 ? (int)(left / 1000) + 1 : 0;
			/* A sound waiting for audiod is worth a quicker look
			 * than the idle retry. */
			if (waiting && timeout > 500)
				timeout = 500;
		}
		if (poll(pfd, nfd, timeout) < 0 && errno != EINTR)
			g_usleep(100000);

		kicked = FALSE;
		if (pfd[0].revents & POLLIN) {
			char buf[64];

			while (read(Q.wake[0], buf, sizeof(buf)) > 0)
				;
			kicked = TRUE;
		}
		if (!c && waiting && !kicked)
			kicked = TRUE;
	}
	return NULL;
}

static void audio_start(void)
{
	GError *err = NULL;

	g_mutex_init(&Q.lock);
	if (!g_unix_open_pipe(Q.wake, FD_CLOEXEC, &err) ||
	    !g_unix_set_fd_nonblocking(Q.wake[0], TRUE, NULL) ||
	    !g_unix_set_fd_nonblocking(Q.wake[1], TRUE, NULL)) {
		g_warning("volume control: %s", err ? err->message : "no pipe");
		g_clear_error(&err);
		return;
	}
	Q.running = TRUE;
	g_thread_unref(g_thread_new("audio", audio_thread, NULL));
}

/* --- the shell's glyphs ------------------------------------------------------- */

/* The glyphs are drawn rather than taken from the icon theme, so that the
 * shell looks the same whatever theme is installed and every line in it has
 * the same weight: each is LikeOS geometry on a 16-unit grid with round caps
 * and joins, drawn with a 1.5-pixel stroke at whatever size it is asked for
 * (16 on the panel and in the flyouts, 20 where a button is larger).  The
 * set: the tray's speaker and network, the session's power, restart and
 * sign-out, the settings sliders, the user, places (home, documents,
 * downloads, pictures, a folder), the display, Workspaces' two panes, and
 * the small marks (add, close, check, chevrons). */

/* Start a glyph: translated to (x, y) and scaled from the 16-unit grid to
 * `size' pixels, with the stroke kept at 1.5 pixels. */
static void glyph_begin(cairo_t *cr, double x, double y, double size)
{
	double k = size / 16.0;

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	cairo_set_line_width(cr, 1.5 / k);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
}

/* The speaker: a short body and its cone, then up to three waves, or with
 * `waves' negative a small cross for muted.  With `ghost' set the waves the
 * level does not reach are drawn too, faintly -- the panel's version; the
 * Control Center's shows only the lit ones.  16 pixels. */
static void glyph_speaker(cairo_t *cr, double x, double y, int waves,
			  gboolean ghost)
{
	static const double rad[] = { 2.75, 4.75, 6.75 };
	double r, g, b, a = 1;

	cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a);
	glyph_begin(cr, x, y, 16);
	cairo_move_to(cr, 1.75, 6.25);
	cairo_line_to(cr, 4.25, 6.25);
	cairo_line_to(cr, 7.75, 3);
	cairo_line_to(cr, 7.75, 13);
	cairo_line_to(cr, 4.25, 9.75);
	cairo_line_to(cr, 1.75, 9.75);
	cairo_close_path(cr);
	cairo_stroke(cr);

	if (waves < 0) {
		cairo_move_to(cr, 10.5, 6.25);
		cairo_line_to(cr, 14, 9.75);
		cairo_move_to(cr, 14, 6.25);
		cairo_line_to(cr, 10.5, 9.75);
		cairo_stroke(cr);
	}
	for (int i = 0; i < 3; i++) {
		double t = 42 * M_PI / 180;

		if (i >= waves) {
			if (!ghost || waves < 0)
				break;
			cairo_set_source_rgba(cr, r, g, b, a * 0.3);
		}
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 8, rad[i], -t, t);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

/* How many waves the speaker shows: one per third of the range, none at
 * zero, and the cross when muted. */
static int vol_waves(void)
{
	if (V.muted)
		return -1;
	if (V.vol == 0)
		return 0;
	if (V.vol <= 33)
		return 1;
	if (V.vol <= 66)
		return 2;
	return 3;
}

/* The active output's name, for the tooltip. */
static const char *vol_output_name(void)
{
	if (V.have_outs) {
		for (uint32_t i = 0; i < V.outs.count && i < AUD_MAX_OUTPUTS; i++) {
			const struct aud_output *o = &V.outs.out[i];

			if (!o->active)
				continue;
			if (o->name[0])
				return o->name;
			switch (o->type) {
			case AUDIO_OUT_HEADPHONE:
			case AUDIO_OUT_HEADSET:
				return "Headphones";
			case AUDIO_OUT_HDMI:
			case AUDIO_OUT_DP:
				return "Display audio";
			default:
				break;
			}
		}
	}
	return "Speakers";
}

/* "Speakers: 52%". */
static char *vol_tooltip(void)
{
	if (!V.online)
		return g_strdup("No audio device");
	if (V.muted)
		return g_strdup_printf("%s: Muted", vol_output_name());
	return g_strdup_printf("%s: %d%%", vol_output_name(), V.vol);
}

/* --- the other glyphs ------------------------------------------------------- */

enum glyph {
	GL_CHEVRON_UP,
	GL_CHEVRON_DOWN,
	GL_CHEVRON_LEFT,
	GL_CHEVRON_RIGHT,
	GL_NET,		/* a wired connection: three linked nodes */
	GL_NO_NET,	/* the same, struck through */
	GL_WORKSPACES,	/* two panes side by side */
	GL_POWER,
	GL_RESTART,
	GL_SIGNOUT,
	GL_SETTINGS,	/* two sliders */
	GL_USER,
	GL_FOLDER,
	GL_HOME,
	GL_DOCUMENT,
	GL_DOWNLOAD,
	GL_PICTURE,
	GL_DISPLAY,
	GL_CLOSE,
	GL_CHECK,
	GL_PIN,		/* a map pin: the panel's pinned buttons */
	GL_STAR,	/* the launcher's favourites */
	GL_ACTIVITY,	/* a pulse: the system monitor */
};

/* The network: a node at the top joined by a stem and a crossbar to two
 * below it. */
static void glyph_net_path(cairo_t *cr)
{
	panel_round_rect(cr, 5.75, 1.75, 4.5, 3.5, 1);
	panel_round_rect(cr, 1.75, 10.75, 3.75, 3.5, 1);
	panel_round_rect(cr, 10.5, 10.75, 3.75, 3.5, 1);
	cairo_move_to(cr, 8, 5.25);
	cairo_line_to(cr, 8, 8);
	cairo_move_to(cr, 3.625, 10.75);
	cairo_line_to(cr, 3.625, 8);
	cairo_line_to(cr, 12.375, 8);
	cairo_line_to(cr, 12.375, 10.75);
}

static void glyph_draw(cairo_t *cr, enum glyph g, double x, double y,
		       double size)
{
	glyph_begin(cr, x, y, size);

	switch (g) {
	case GL_CHEVRON_UP:
		cairo_move_to(cr, 4, 10);
		cairo_line_to(cr, 8, 6);
		cairo_line_to(cr, 12, 10);
		cairo_stroke(cr);
		break;
	case GL_CHEVRON_DOWN:
		cairo_move_to(cr, 4, 6);
		cairo_line_to(cr, 8, 10);
		cairo_line_to(cr, 12, 6);
		cairo_stroke(cr);
		break;
	case GL_CHEVRON_LEFT:
		cairo_move_to(cr, 10, 4);
		cairo_line_to(cr, 6, 8);
		cairo_line_to(cr, 10, 12);
		cairo_stroke(cr);
		break;
	case GL_CHEVRON_RIGHT:
		cairo_move_to(cr, 6, 4);
		cairo_line_to(cr, 10, 8);
		cairo_line_to(cr, 6, 12);
		cairo_stroke(cr);
		break;
	case GL_NET:
		glyph_net_path(cr);
		cairo_stroke(cr);
		break;
	case GL_NO_NET: {
		double r, gg, b, a;

		cairo_pattern_get_rgba(cairo_get_source(cr), &r, &gg, &b, &a);
		cairo_set_source_rgba(cr, r, gg, b, a * 0.45);
		glyph_net_path(cr);
		cairo_stroke(cr);
		cairo_set_source_rgba(cr, r, gg, b, a);
		cairo_move_to(cr, 2, 2);
		cairo_line_to(cr, 14, 14);
		cairo_stroke(cr);
		break;
	}
	case GL_WORKSPACES:
		panel_round_rect(cr, 1.75, 3.75, 5.5, 8.5, 1.5);
		panel_round_rect(cr, 8.75, 3.75, 5.5, 8.5, 1.5);
		cairo_stroke(cr);
		break;
	case GL_POWER:
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 8.75, 5.5, -M_PI / 2 + 0.68,
			  3 * M_PI / 2 - 0.68);
		cairo_move_to(cr, 8, 1.75);
		cairo_line_to(cr, 8, 7.25);
		cairo_stroke(cr);
		break;
	case GL_RESTART:
		/* A ring, open at the top, its end turned into an arrow. */
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 8.5, 5.25, -M_PI / 2 + 0.75,
			  3 * M_PI / 2 - 0.12);
		cairo_stroke(cr);
		cairo_move_to(cr, 5.4, 1.4);
		cairo_line_to(cr, 7.6, 3.3);
		cairo_line_to(cr, 5.4, 5.3);
		cairo_stroke(cr);
		break;
	case GL_SIGNOUT:
		cairo_move_to(cr, 7.25, 2.5);
		cairo_line_to(cr, 3, 2.5);
		cairo_line_to(cr, 3, 13.5);
		cairo_line_to(cr, 7.25, 13.5);
		cairo_move_to(cr, 6.5, 8);
		cairo_line_to(cr, 14, 8);
		cairo_move_to(cr, 11.25, 5.25);
		cairo_line_to(cr, 14, 8);
		cairo_line_to(cr, 11.25, 10.75);
		cairo_stroke(cr);
		break;
	case GL_SETTINGS:
		/* Two sliders: a line with a knob, broken where the knob
		 * sits so nothing shows through it. */
		cairo_move_to(cr, 2, 5);
		cairo_line_to(cr, 8, 5);
		cairo_move_to(cr, 12, 5);
		cairo_line_to(cr, 14, 5);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 10, 5, 2, 0, 2 * M_PI);
		cairo_move_to(cr, 2, 11);
		cairo_line_to(cr, 4, 11);
		cairo_move_to(cr, 8, 11);
		cairo_line_to(cr, 14, 11);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 6, 11, 2, 0, 2 * M_PI);
		cairo_stroke(cr);
		break;
	case GL_USER:
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 5.25, 2.75, 0, 2 * M_PI);
		cairo_move_to(cr, 2.75, 14);
		cairo_curve_to(cr, 2.75, 10.75, 5, 9.25, 8, 9.25);
		cairo_curve_to(cr, 11, 9.25, 13.25, 10.75, 13.25, 14);
		cairo_stroke(cr);
		break;
	case GL_FOLDER:
		cairo_move_to(cr, 2, 12.75);
		cairo_line_to(cr, 2, 3.5);
		cairo_line_to(cr, 6, 3.5);
		cairo_line_to(cr, 7.75, 5.25);
		cairo_line_to(cr, 14, 5.25);
		cairo_line_to(cr, 14, 12.75);
		cairo_close_path(cr);
		cairo_stroke(cr);
		break;
	case GL_HOME:
		cairo_move_to(cr, 2.25, 7.5);
		cairo_line_to(cr, 8, 2.5);
		cairo_line_to(cr, 13.75, 7.5);
		cairo_move_to(cr, 4, 6.25);
		cairo_line_to(cr, 4, 13.5);
		cairo_line_to(cr, 12, 13.5);
		cairo_line_to(cr, 12, 6.25);
		cairo_move_to(cr, 6.75, 13.5);
		cairo_line_to(cr, 6.75, 10);
		cairo_line_to(cr, 9.25, 10);
		cairo_line_to(cr, 9.25, 13.5);
		cairo_stroke(cr);
		break;
	case GL_DOCUMENT:
		cairo_move_to(cr, 3.5, 1.75);
		cairo_line_to(cr, 9.25, 1.75);
		cairo_line_to(cr, 12.5, 5);
		cairo_line_to(cr, 12.5, 14.25);
		cairo_line_to(cr, 3.5, 14.25);
		cairo_close_path(cr);
		cairo_move_to(cr, 9.25, 1.75);
		cairo_line_to(cr, 9.25, 5);
		cairo_line_to(cr, 12.5, 5);
		cairo_move_to(cr, 6, 8.5);
		cairo_line_to(cr, 10, 8.5);
		cairo_move_to(cr, 6, 11.25);
		cairo_line_to(cr, 10, 11.25);
		cairo_stroke(cr);
		break;
	case GL_DOWNLOAD:
		cairo_move_to(cr, 8, 2);
		cairo_line_to(cr, 8, 10);
		cairo_move_to(cr, 4.75, 6.75);
		cairo_line_to(cr, 8, 10);
		cairo_line_to(cr, 11.25, 6.75);
		cairo_move_to(cr, 2.5, 10.5);
		cairo_line_to(cr, 2.5, 13.5);
		cairo_line_to(cr, 13.5, 13.5);
		cairo_line_to(cr, 13.5, 10.5);
		cairo_stroke(cr);
		break;
	case GL_PICTURE:
		panel_round_rect(cr, 1.75, 2.75, 12.5, 10.5, 1.5);
		cairo_move_to(cr, 1.75, 11.5);
		cairo_line_to(cr, 6, 7.5);
		cairo_line_to(cr, 9, 10.5);
		cairo_line_to(cr, 10.75, 9);
		cairo_line_to(cr, 14.25, 12);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 10.75, 5.75, 1.25, 0, 2 * M_PI);
		cairo_stroke(cr);
		break;
	case GL_DISPLAY:
		panel_round_rect(cr, 1.75, 2.25, 12.5, 8.75, 1.5);
		cairo_move_to(cr, 8, 11);
		cairo_line_to(cr, 8, 13.5);
		cairo_move_to(cr, 5, 13.5);
		cairo_line_to(cr, 11, 13.5);
		cairo_stroke(cr);
		break;
	case GL_CLOSE:
		cairo_move_to(cr, 4, 4);
		cairo_line_to(cr, 12, 12);
		cairo_move_to(cr, 12, 4);
		cairo_line_to(cr, 4, 12);
		cairo_stroke(cr);
		break;
	case GL_CHECK:
		cairo_move_to(cr, 3, 8.5);
		cairo_line_to(cr, 6.5, 12);
		cairo_line_to(cr, 13, 4.5);
		cairo_stroke(cr);
		break;
	case GL_PIN:
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 6.25, 4.25, M_PI * 0.82, M_PI * 2.18);
		cairo_line_to(cr, 8, 14);
		cairo_close_path(cr);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8, 6.25, 1.5, 0, 2 * M_PI);
		cairo_stroke(cr);
		break;
	case GL_STAR:
		for (int i = 0; i < 10; i++) {
			double t = -M_PI / 2 + i * M_PI / 5;
			double r = (i & 1) ? 2.9 : 6.25;
			double px = 8 + r * cos(t), py = 8.6 + r * sin(t);

			if (i == 0)
				cairo_move_to(cr, px, py);
			else
				cairo_line_to(cr, px, py);
		}
		cairo_close_path(cr);
		cairo_stroke(cr);
		break;
	case GL_ACTIVITY:
		cairo_move_to(cr, 1.5, 8.5);
		cairo_line_to(cr, 4.5, 8.5);
		cairo_line_to(cr, 6.5, 3.5);
		cairo_line_to(cr, 9.5, 12.5);
		cairo_line_to(cr, 11.5, 8.5);
		cairo_line_to(cr, 14.5, 8.5);
		cairo_stroke(cr);
		break;
	}
	cairo_restore(cr);
}

/* A glyph as an image widget, for the launcher: drawn once at `size' in
 * `rgb'. */
static GtkWidget *glyph_image(enum glyph g, int size, unsigned int rgb)
{
	cairo_surface_t *s =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);

	panel_color(cr, rgb);
	glyph_draw(cr, g, 0, 0, size);
	cairo_destroy(cr);

	GtkWidget *img = gtk_image_new_from_surface(s);

	cairo_surface_destroy(s);
	return img;
}

/* --- the network icon --------------------------------------------------------- */

/* Whether there is a connection: an interface other than loopback that is
 * up, running and has an address, and a default route to leave the machine
 * by.  The address alone is not enough -- an interface the network has not
 * configured yet, or one kept on a fixed address with no gateway, reaches
 * nothing beyond its own segment -- and the interface flags alone are less:
 * a network driver reports its interface up and running whether or not a
 * cable is in.  What a lost link does change is the address: the driver
 * drops the lease, and the address, the gateway and the default route go
 * with it.
 *
 * The routes are the system's own table (net_getinfo()); where that cannot
 * be read, the address decides alone.  Polled every few seconds -- there is
 * no notification to wait on -- and when the Control Center opens, so the
 * icon, the tooltip and the Control Center's row follow a change on their
 * own. */
#define NET_POLL_SECS 3

static struct {
	gboolean up;
	char name[IFNAMSIZ];
	char addr[INET_ADDRSTRLEN];
} g_net;

/* Is there a default route out through interface `dev'?  1 or 0; -1 when the
 * routes cannot be read. */
static int net_default_route(const char *dev)
{
#ifdef NET_GET_ROUTE_TABLE
	net_route_info_t r[32];
	int n = net_getinfo(NET_GET_ROUTE_TABLE, r, G_N_ELEMENTS(r));

	if (n < 0)
		return -1;
	for (int i = 0; i < n && i < (int)G_N_ELEMENTS(r); i++) {
		r[i].dev_name[sizeof(r[i].dev_name) - 1] = 0;
		if (r[i].dst_net != 0 || r[i].netmask != 0)
			continue;
		/* A route bound to no interface ("*") goes out of any. */
		if (strcmp(r[i].dev_name, dev) == 0 ||
		    strcmp(r[i].dev_name, "*") == 0)
			return 1;
	}
	return 0;
#else
	(void)dev;
	return -1;
#endif
}

static char *tray_tooltip(void);

static gboolean net_poll(gpointer data)
{
	struct ifaddrs *ifs = NULL;
	gboolean up = FALSE;
	char name[IFNAMSIZ] = "", addr[INET_ADDRSTRLEN] = "";

	(void)data;
	if (getifaddrs(&ifs) == 0) {
		for (struct ifaddrs *i = ifs; i && !up; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
			    !i->ifa_name || (i->ifa_flags & IFF_LOOPBACK) ||
			    !(i->ifa_flags & IFF_UP) ||
			    !(i->ifa_flags & IFF_RUNNING))
				continue;

			struct sockaddr_in *sin = (struct sockaddr_in *)i->ifa_addr;

			if (sin->sin_addr.s_addr == 0 ||
			    net_default_route(i->ifa_name) == 0)
				continue;
			up = TRUE;
			g_strlcpy(name, i->ifa_name, sizeof(name));
			inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));
		}
		freeifaddrs(ifs);
	}
	if (up != g_net.up || strcmp(addr, g_net.addr) != 0 ||
	    strcmp(name, g_net.name) != 0) {
		g_net.up = up;
		g_strlcpy(g_net.name, name, sizeof(g_net.name));
		g_strlcpy(g_net.addr, addr, sizeof(g_net.addr));
		if (g_area) {
			gtk_widget_queue_draw(g_area);
			if (g_hover == HIT_TRAY) {
				char *tip = tray_tooltip();

				gtk_widget_set_tooltip_text(g_area, tip);
				g_free(tip);
			}
		}
		if (F.up)
			gtk_widget_queue_draw(F.win);
	}
	return G_SOURCE_CONTINUE;
}

/* The tray's tooltip: the network's state over the sound's. */
static char *tray_tooltip(void)
{
	char *vol = vol_tooltip();
	char *tip;

	if (g_net.up)
		tip = g_strdup_printf("Network: connected (%s, %s)\n%s",
				      g_net.name, g_net.addr, vol);
	else
		tip = g_strdup_printf("Network: not connected\n%s", vol);
	g_free(vol);
	return tip;
}

/* --- the wallpaper tint ---------------------------------------------------------- */

/* The panel is the shell's dark colour with a little of the wallpaper under
 * it showing through -- 12%, blurred to a wash -- so that it sits on the
 * desktop rather than being cut out of it.  There is no compositor here to
 * blend real windows, but the wallpaper is the only thing under the panel
 * anyway, and the desktop publishes it: PCManFM sets the root window's
 * _XROOTPMAP_ID to the pixmap it draws its background from, the convention
 * every pseudo-transparent panel has used.  The same copy draws the small
 * pictures of the workspaces.
 *
 * So the wallpaper is copied once, shrunk to a fortieth (which is the blur:
 * shrinking averages, and the bilinear filter that blows a region back up
 * spreads each sample smoothly over forty pixels, leaving only the
 * wallpaper's colours), and the panel paints the part of it that lies
 * behind it under its own colour.  Without a published pixmap it is the
 * solid colour alone. */
#define WALL_SHRINK 40
#define TINT_AMOUNT 0.88	/* the panel's own colour over the wallpaper */

static cairo_surface_t *g_wall_small; /* the wallpaper, shrunk; or NULL */

static void tint_update(void)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.rootpmap, XA_PIXMAP, NULL, &n);
	Pixmap pm = None;

	if (g_wall_small) {
		cairo_surface_destroy(g_wall_small);
		g_wall_small = NULL;
	}
	if (d) {
		if (n >= 1)
			pm = *(Pixmap *)d;
		XFree(d);
	}
	if (pm == None)
		return;

	Window root_ret;
	int px, py;
	unsigned int pw = 0, ph = 0, bw, depth = 0;
	GdkDisplay *disp = gdk_display_get_default();
	Screen *scr = DefaultScreenOfDisplay(g_dpy);

	/* The pixmap is the desktop's, not ours, and can be freed under us
	 * when the wallpaper changes: every request on it is trapped, and it
	 * is copied at once rather than kept. */
	gdk_x11_display_error_trap_push(disp);
	Status ok = XGetGeometry(g_dpy, pm, &root_ret, &px, &py, &pw, &ph, &bw,
				 &depth);

	if (!ok || pw == 0 || ph == 0 || (int)depth != DefaultDepthOfScreen(scr)) {
		gdk_x11_display_error_trap_pop_ignored(disp);
		return;
	}

	cairo_surface_t *src = cairo_xlib_surface_create(
		g_dpy, pm, DefaultVisualOfScreen(scr), (int)pw, (int)ph);
	int sw = ((int)pw + WALL_SHRINK - 1) / WALL_SHRINK;
	int sh = ((int)ph + WALL_SHRINK - 1) / WALL_SHRINK;
	cairo_surface_t *small =
		cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
	cairo_t *cr = cairo_create(small);

	cairo_scale(cr, 1.0 / WALL_SHRINK, 1.0 / WALL_SHRINK);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(small);
	cairo_surface_destroy(src);
	XSync(g_dpy, False);

	if (gdk_x11_display_error_trap_pop(disp) == 0 &&
	    cairo_surface_status(small) == CAIRO_STATUS_SUCCESS) {
		g_wall_small = small;
	} else {
		cairo_surface_destroy(small);
	}
}

/* Paint the surface at root position (rx, ry), w x h: the blurred wallpaper
 * behind it washed with `tint' at `amount', or the flat `tint' alone when
 * there is no wallpaper to sample.  (0, 0) in `cr' is the surface's top-left
 * corner; the caller clips it to its shape. */
static void tint_paint(cairo_t *cr, int rx, int ry, int w, int h,
		       unsigned int tint, double amount)
{
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_clip(cr);
	if (g_wall_small) {
		cairo_save(cr);
		cairo_scale(cr, WALL_SHRINK, WALL_SHRINK);
		cairo_set_source_surface(cr, g_wall_small,
					 -(double)rx / WALL_SHRINK,
					 -(double)ry / WALL_SHRINK);
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr),
					 CAIRO_EXTEND_PAD);
		cairo_paint(cr);
		cairo_restore(cr);
		panel_color(cr, tint);
		cairo_paint_with_alpha(cr, amount);
	} else {
		panel_color(cr, tint);
		cairo_paint(cr);
	}
	cairo_restore(cr);
}

/* --- layout ---------------------------------------------------------------- */

/* Where everything goes.  Called by both the drawing code and the click
 * handler, so the two cannot disagree about what is where -- which is the only
 * real hazard in hit-testing a hand-drawn widget. */
struct layout {
	double orb_x, ws_x, sep_x;
	double app_x;		   /* the first application button */
	int napps;		   /* the buttons that fit */
	double chev_x;		   /* the overflow button, or -1 */
	double load_x, load_w;	   /* load_w is 0 when it gave way */
	double tray_x, clock_x, clock_w;
	double tile_y;
};

/* The measured width of the clock text, cached from the last draw.
 *
 * layout() has to give the same answer to the drawing code and to the click
 * handler, and only the drawing code has a cairo_t to measure text with.
 * Without the cache the two disagreed by however far the real clock width was
 * from the guess, and every control to the right of the task area responded
 * to clicks a few pixels away from where it was drawn. */
static double g_clock_w;

static void layout(cairo_t *cr, int width, struct layout *L)
{
	if (cr)
		g_clock_w = panel_clock_width(cr);

	double clock_text = g_clock_w < 90 ? 90 : g_clock_w;

	L->tile_y = (PANEL_HEIGHT - TILE_H) / 2;

	/* The left end: the start orb in its square at the very edge,
	 * Workspaces, and the hairline that starts the applications. */
	L->orb_x = 0;
	L->ws_x = L->orb_x + ORB_SLOT + APP_GAP;
	L->sep_x = L->ws_x + WS_W + SEP_PAD;
	L->app_x = L->sep_x + 1 + SEP_PAD;

	/* The right end, from the edge in: the clock, the tray (the network
	 * and the speaker on one tile) and the load monitor. */
	L->clock_w = clock_text + 2 * CLOCK_PAD;
	L->clock_x = width - EDGE_PAD - L->clock_w;
	L->tray_x = L->clock_x - TRAY_GAP - TRAY_W;
	L->load_w = PANEL_LOAD_W;
	L->load_x = L->tray_x - 6 - L->load_w;
	L->chev_x = -1;

	/* The applications fill what is left between.  When they do not fit,
	 * the load monitor gives way to the overflow button (which holds it),
	 * and past that the last buttons drop off. */
	int n = g_buttons ? (int)g_buttons->len : 0;
	double right = L->load_x - 12;

	if (L->app_x + n * (APP_W + APP_GAP) > right) {
		L->load_w = 0;
		L->chev_x = L->tray_x - 6 - CHEVRON_W;
		right = L->chev_x - 8;
	}
	L->napps = (int)floor((right - L->app_x + APP_GAP) / (APP_W + APP_GAP));
	if (L->napps > n)
		L->napps = n;
	if (L->napps < 0)
		L->napps = 0;

	for (int i = 0; i < n; i++) {
		struct button *b = g_ptr_array_index(g_buttons, i);

		b->x = i < L->napps ? L->app_x + i * (APP_W + APP_GAP) : -1;
	}
}

/* --- drawing ---------------------------------------------------------------- */

static gboolean popup_owner_up(int hit); /* below, with the flyouts */
static int hit_test(double x, double y); /* below, with the input */

/* After a flyout or the launcher has closed: the bar saw no motion while
 * their grab was held, so what it last thought was under the pointer is
 * stale.  Asked of the server instead -- the orb under a pointer that has
 * just closed the launcher stays lit rather than going out until the pointer
 * next moves. */
static void bar_rehover(void)
{
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
	GdkDevice *ptr = seat ? gdk_seat_get_pointer(seat) : NULL;
	int x = -1, y = -1;

	g_hover = HIT_NONE;
	if (!g_gdkwin || !ptr)
		return;
	gdk_window_get_device_position(g_gdkwin, ptr, &x, &y, NULL);
	if (x >= 0 && y >= 0 && x < g_panel_w && y < PANEL_HEIGHT)
		g_hover = hit_test(x, y);
}

/* How a control is lit: held down, pointed at, lit for a reason of its own
 * (the focused window, an open flyout), or not at all. */
static enum panel_tile tile_state(int hit, gboolean lit)
{
	if (g_pressed == hit && g_hover == hit)
		return PANEL_TILE_DOWN;
	if (lit || popup_owner_up(hit))
		return PANEL_TILE_LIT;
	if (g_hover == hit)
		return PANEL_TILE_HOVER;
	return PANEL_TILE_NONE;
}

static void draw_pixbuf(cairo_t *cr, GdkPixbuf *pb, double x, double y,
			double size)
{
	int w = gdk_pixbuf_get_width(pb);

	cairo_save(cr);
	cairo_translate(cr, x, y);
	if (w != (int)size)
		cairo_scale(cr, size / w, size / w);
	gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* The start orb: the icon theme's likeos-start-orb at rest, -hover while the
 * pointer is over it or the launcher is open, and -pressed while it is held
 * down.  Between rest and lit it cross-fades, over 180 ms on the way in and
 * 250 ms on the way out, easing out; a fade turned round half-way runs back
 * from where it got to.  The frame clock drives it, and only while a fade is
 * running.  Without the artwork, panel_orb() draws a plain glass orb in its
 * place. */
#define ORB_FADE_IN_US 180000
#define ORB_FADE_OUT_US 250000

static struct {
	GdkPixbuf *img[3];	/* at rest, lit, pressed; NULL without the theme's */
	gboolean loaded;
	double from, to;	/* the fade: from this glow to that, 0..1 */
	gint64 t0, dur;		/* starting then, for so long (microseconds) */
	guint tick;
} O;

static void orb_load(void)
{
	static const char *const names[3] = { "likeos-start-orb",
					      "likeos-start-orb-hover",
					      "likeos-start-orb-pressed" };
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	int scale = gtk_widget_get_scale_factor(g_area);

	if (O.loaded)
		return;
	O.loaded = TRUE;
	for (int i = 0; i < 3; i++)
		O.img[i] = gtk_icon_theme_load_icon_for_scale(
			theme, names[i], ORB_SLOT, scale, 0, NULL);
	/* All three or none: a half-themed orb would jump between looks. */
	if (!O.img[0] || !O.img[1] || !O.img[2])
		for (int i = 0; i < 3; i++)
			g_clear_object(&O.img[i]);
}

static void orb_forget(void)
{
	for (int i = 0; i < 3; i++)
		g_clear_object(&O.img[i]);
	O.loaded = FALSE;
}

static gint64 orb_now(void)
{
	GdkFrameClock *fc = gtk_widget_get_frame_clock(g_area);

	return fc ? gdk_frame_clock_get_frame_time(fc) : g_get_monotonic_time();
}

/* How lit the orb is at `now'. */
static double orb_glow_at(gint64 now)
{
	if (O.dur <= 0 || now >= O.t0 + O.dur)
		return O.to;

	double t = now <= O.t0 ? 0 : (double)(now - O.t0) / O.dur;
	double e = 1 - (1 - t) * (1 - t) * (1 - t);

	return O.from + (O.to - O.from) * e;
}

static gboolean orb_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
	gint64 now = gdk_frame_clock_get_frame_time(clock);

	(void)d;
	gtk_widget_queue_draw_area(w, 0, 0, ORB_SLOT, PANEL_HEIGHT);
	if (now >= O.t0 + O.dur) {
		O.tick = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* Start a fade when the orb's state has changed since it was last drawn. */
static void orb_retarget(gint64 now)
{
	double want = g_hover == HIT_LAUNCHER || popup_owner_up(HIT_LAUNCHER) ?
			      1 : 0;

	if (want == O.to)
		return;
	O.from = orb_glow_at(now);
	O.to = want;
	O.t0 = now;
	O.dur = (gint64)(fabs(want - O.from) *
			 (want > O.from ? ORB_FADE_IN_US : ORB_FADE_OUT_US));
	if (!O.tick)
		O.tick = gtk_widget_add_tick_callback(g_area, orb_tick, NULL,
						      NULL);
}

static void orb_paint(cairo_t *cr, GdkPixbuf *pb, double x, double y,
		      double alpha)
{
	double k = (double)ORB_SLOT / gdk_pixbuf_get_width(pb);

	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

static void draw_orb(cairo_t *cr, struct layout *L)
{
	gint64 now = orb_now();
	gboolean down = g_pressed == HIT_LAUNCHER && g_hover == HIT_LAUNCHER;
	double x = L->orb_x, y = (PANEL_HEIGHT - ORB_SLOT) / 2;
	double glow;

	orb_retarget(now);
	glow = orb_glow_at(now);
	orb_load();
	if (!O.img[0]) {
		panel_orb(cr, x + ORB_SLOT / 2.0, y + ORB_SLOT / 2.0,
			  ORB_SLOT / 2.0 - 2, down ? 0 : glow, down);
		return;
	}
	if (down) {
		orb_paint(cr, O.img[2], x, y, 1);
		return;
	}
	/* The lit orb over the one at rest, as lit as the fade has got. */
	if (glow < 1)
		orb_paint(cr, O.img[0], x, y, 1);
	if (glow > 0)
		orb_paint(cr, O.img[1], x, y, glow);
}

static void draw_buttons(cairo_t *cr, struct layout *L)
{
	double ty = L->tile_y;

	for (int i = 0; i < L->napps; i++) {
		struct button *b = g_ptr_array_index(g_buttons, i);
		gboolean active = button_active(b);
		enum panel_tile st = tile_state(i, active);

		panel_tile(cr, b->x, ty, APP_W, TILE_H, st);

		/* The icon sits a little above the tile's centre, leaving the
		 * foot of the tile for the window dots.  Held down, it draws
		 * in a little toward its centre. */
		double sz = st == PANEL_TILE_DOWN ? ICON_SIZE - 2 : ICON_SIZE;

		if (b->icon)
			draw_pixbuf(cr, b->icon,
				    floor(b->x + (APP_W - sz) / 2),
				    floor(ty + 6 + (ICON_SIZE - sz) / 2), sz);
		panel_dots(cr, b->x + APP_W / 2.0, ty + TILE_H - 5,
			   (int)b->wins->len, active);
	}
}

/* The tray: the network and the speaker side by side on one tile, which
 * opens the Control Center -- it holds more than the sound, so the two are
 * one control rather than two. */
static void draw_tray(cairo_t *cr, struct layout *L)
{
	double gy = L->tile_y + (TILE_H - 16) / 2;
	double gx = L->tray_x + (TRAY_W - 2 * 16 - TRAY_GLYPH_GAP) / 2;

	panel_tile(cr, L->tray_x, L->tile_y, TRAY_W, TILE_H,
		   tile_state(HIT_TRAY, FALSE));
	panel_color(cr, PANEL_TEXT);
	glyph_draw(cr, g_net.up ? GL_NET : GL_NO_NET, gx, gy, 16);

	gx += 16 + TRAY_GLYPH_GAP;
	if (V.online) {
		panel_color(cr, PANEL_TEXT);
		glyph_speaker(cr, gx, gy, vol_waves(), TRUE);
	} else {
		panel_color(cr, PANEL_TEXT_DISABLED);
		glyph_speaker(cr, gx, gy, -1, FALSE);
	}
}

/* The panel's own background: the shell colour over the wallpaper tint, and
 * a hairline along the top where it meets the desktop. */
static void draw_face(cairo_t *cr, int width, int height)
{
	int ox = 0, oy = 0;

	if (g_gdkwin)
		gdk_window_get_origin(g_gdkwin, &ox, &oy);
	tint_paint(cr, ox, oy, width, height, PANEL_BG, TINT_AMOUNT);
	panel_color(cr, PANEL_BORDER);
	cairo_rectangle(cr, 0, 0, width, 1);
	cairo_fill(cr);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	(void)data;
	int width = gtk_widget_get_allocated_width(widget);
	int height = gtk_widget_get_allocated_height(widget);
	struct layout L;

	g_panel_w = width;
	layout(cr, width, &L);
	draw_face(cr, width, height);

	draw_orb(cr, &L);

	enum panel_tile ws = tile_state(HIT_WORKSPACES, FALSE);

	panel_tile(cr, L.ws_x, L.tile_y, WS_W, TILE_H, ws);
	panel_color(cr, popup_owner_up(HIT_WORKSPACES) ? PANEL_ACCENT :
							 PANEL_TEXT);
	glyph_draw(cr, GL_WORKSPACES, L.ws_x + (WS_W - 20) / 2,
		   L.tile_y + (TILE_H - 20) / 2, 20);

	panel_color_a(cr, PANEL_TEXT, 0.14);
	cairo_rectangle(cr, L.sep_x, (PANEL_HEIGHT - 24) / 2, 1, 24);
	cairo_fill(cr);

	draw_buttons(cr, &L);

	if (L.chev_x >= 0) {
		panel_tile(cr, L.chev_x, L.tile_y, CHEVRON_W, TILE_H,
			   tile_state(HIT_CHEVRON, FALSE));
		panel_color(cr, PANEL_TEXT);
		glyph_draw(cr, popup_owner_up(HIT_CHEVRON) ? GL_CHEVRON_DOWN :
							     GL_CHEVRON_UP,
			   L.chev_x + (CHEVRON_W - 16) / 2,
			   L.tile_y + (TILE_H - 16) / 2, 16);
	}
	if (L.load_w > 0) {
		panel_tile(cr, L.load_x, L.tile_y, L.load_w, TILE_H,
			   tile_state(HIT_LOAD, FALSE));
		panel_load_draw(cr, &g_load, L.load_x, 0, L.load_w, height);
	}

	draw_tray(cr, &L);

	panel_tile(cr, L.clock_x, L.tile_y, L.clock_w, TILE_H,
		   tile_state(HIT_CLOCK, FALSE));
	panel_clock_draw(cr, L.clock_x + CLOCK_PAD, 0,
			 L.clock_w - 2 * CLOCK_PAD, height, FALSE);
	g_bar_drawn = TRUE;
	return TRUE;
}

/* --- session and power commands ---------------------------------------------- */

/* Signal the window manager.
 *
 * There is no EWMH message for "restart yourself" or "quit" -- neither is
 * something the spec contemplates a client asking for.  ctwm restarts on
 * SIGHUP and shuts down on SIGTERM, and xinitrc writes its pid to
 * ~/.ctwm.pid precisely so that something outside ctwm can reach it: it
 * advertises _NET_SUPPORTING_WM_CHECK but no _NET_WM_PID, so the file is the
 * only route.
 *
 * The pid is read fresh each time rather than cached: xinitrc removes the
 * file once ctwm has gone.  (A restart re-executes ctwm in the same process,
 * so the pid stays valid across it.) */
static void signal_wm(int sig, const char *what)
{
	char *path = g_build_filename(g_get_home_dir(), ".ctwm.pid", NULL);
	char *text = NULL;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		long pid = strtol(text, NULL, 10);

		if (pid > 1)
			kill((pid_t)pid, sig);
		else
			g_warning("%s: no pid in %s", what, path);
	} else {
		g_warning("%s: cannot read %s", what, path);
	}
	g_free(text);
	g_free(path);
}

static void session_restart(void)
{
	signal_wm(SIGHUP, "restart");
}

/* Ends the whole session, not just the decorations: when ctwm goes, xinitrc
 * finishes and the X server follows.
 *
 * Through likeos-logout rather than a signal from here: it starts the log-out
 * sound while the desktop is still on the screen and then tells ctwm to go,
 * so every way of leaving the session ends the same way.  Started
 * asynchronously: the bar keeps drawing until the server takes it away.  If
 * the helper cannot be started at all, ctwm is signalled from here instead. */
static void session_logout(void)
{
	char *argv[] = { (char *)"/usr/bin/likeos-logout", NULL };
	GError *err = NULL;

	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			   &err)) {
		g_warning("exit: cannot start %s: %s", argv[0],
			  err ? err->message : "?");
		g_clear_error(&err);
		signal_wm(SIGTERM, "exit");
	}
}

/* Restart and Shut down run /bin/reboot and /bin/poweroff.  They decide
 * themselves whether this user may (root, or the user logged in at the
 * console -- which the desktop's user is), and the bar says so when the
 * command refuses.  Neither, nor signing out, happens on one click: each
 * asks first (confirm_open(), with the launcher), since a misplaced click
 * must not close every program the user has open. */
struct power_req {
	const char *cmd;   /* /bin/reboot or /bin/poweroff */
	const char *what;  /* "restarted" / "shut down", for the message */
};

static const struct power_req g_reboot_req = { "/bin/reboot", "restarted" };
static const struct power_req g_poweroff_req = { "/bin/poweroff", "shut down" };

/* What a confirmation asks about. */
enum confirm_kind { CONFIRM_SIGN_OUT, CONFIRM_RESTART, CONFIRM_SHUT_DOWN };

static void confirm_open(enum confirm_kind kind);

static void power_failed(const struct power_req *req)
{
	GtkWidget *d = gtk_message_dialog_new(
		NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
		"The computer could not be %s.", req->what);

	gtk_message_dialog_format_secondary_text(
		GTK_MESSAGE_DIALOG(d),
		"Only root and the user logged in at the console may do this.");
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static void on_power_exit(GPid pid, gint status, gpointer data)
{
	const struct power_req *req = data;

	g_spawn_close_pid(pid);
	/* A command that succeeded does not come back: the machine goes
	 * down.  One that exits has refused or failed. */
	if (!g_spawn_check_wait_status(status, NULL))
		power_failed(req);
}

static void power_run(const struct power_req *req)
{
	char *argv[] = { (char *)req->cmd, NULL };
	GError *err = NULL;
	GPid pid;

	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
			   NULL, &pid, &err)) {
		g_warning("cannot start %s: %s", req->cmd,
			  err ? err->message : "?");
		g_clear_error(&err);
		power_failed(req);
		return;
	}
	g_child_watch_add(pid, on_power_exit, (gpointer)req);
}

/* --- the panel's menus ---------------------------------------------------------- */

/* The icon for a menu entry, from the icon theme, at 16 pixels.
 *
 * Falls back to the generic executable icon: an entry may name an icon the
 * theme on this image does not carry, and a menu with one blank row looks
 * like a bug rather than a missing artwork file. */
static GtkWidget *menu_icon(const char *name)
{
	GdkPixbuf *pb = icon_load(name, 16);

	if (!pb)
		pb = icon_load("application-x-executable", 16);
	return pb ? gtk_image_new_from_pixbuf(pb) : NULL;
}

/* One menu row: icon on the left, label beside it.
 *
 * Built by hand out of a box rather than with GtkImageMenuItem, which is
 * deprecated and which recent GTK ignores the image of unless a setting is
 * turned on.  `img' may be NULL for a row with no icon, which still keeps the
 * icon's column so the labels line up. */
static GtkWidget *menu_row(GtkWidget *img, const char *label)
{
	GtkWidget *item = gtk_menu_item_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *lbl = gtk_label_new(label);

	if (!img) {
		img = gtk_image_new();
		gtk_widget_set_size_request(img, 16, 16);
	}
	gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(item), box);
	return item;
}

/* Tear a menu down once it closes.
 *
 * A menu popped up with gtk_menu_popup_at_rect() is not owned by anything --
 * it is built floating and stays alive until somebody destroys it -- so
 * without this every right-click would leak a menu and all its icons.
 * Destroyed from an idle rather than inline because this runs from inside the
 * menu's own signal emission. */
static gboolean destroy_menu(gpointer menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu));
	return G_SOURCE_REMOVE;
}

static int g_menu_owner = HIT_NONE; /* the button whose menu is open */

static void on_menu_done(GtkWidget *w, gpointer data)
{
	(void)data;
	g_menu_owner = HIT_NONE;
	gtk_widget_queue_draw(g_area);
	g_idle_add(destroy_menu, w);
}

/* Open `menu' upward from the panel, lined up with the control it belongs to.
 * The panel is at the bottom of the screen, so the menu grows away from it.
 *
 * The panel's menus are shell surfaces and take the dark shell look: the
 * class below is what _shell.scss styles them by, on the menu and on the
 * popup window GTK puts it in. */
static void menu_popup_above(GtkWidget *menu, int owner, double x, double w)
{
	GdkRectangle r = { (int)x, 0, (int)w, PANEL_HEIGHT };

	gtk_style_context_add_class(gtk_widget_get_style_context(menu),
				    "likeos-shell-menu");
	g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_done), NULL);
	gtk_widget_show_all(menu);
	gtk_style_context_add_class(
		gtk_widget_get_style_context(gtk_widget_get_toplevel(menu)),
		"likeos-shell-menu");
	g_menu_owner = owner;
	gtk_widget_queue_draw(g_area);
	gtk_menu_popup_at_rect(GTK_MENU(menu), g_gdkwin, &r,
			       GDK_GRAVITY_NORTH, GDK_GRAVITY_SOUTH, NULL);
}

/* What a menu row acts on.  Rows carry their window or application by value
 * (an XID, an entry name) rather than by pointer: the task list can be rebuilt
 * while the menu is open. */
static void on_row_activate_window(GtkMenuItem *item, gpointer data)
{
	(void)data;
	Window w = (Window)(uintptr_t)g_object_get_data(G_OBJECT(item), "win");

	if (w != None)
		task_activate(w);
}

static void on_row_close_windows(GtkMenuItem *item, gpointer data)
{
	GArray *wins = g_object_get_data(G_OBJECT(item), "wins");

	(void)data;
	for (guint i = 0; wins && i < wins->len; i++)
		task_close(g_array_index(wins, Window, i));
}

static void on_row_launch(GtkMenuItem *item, gpointer data)
{
	(void)data;
	app_launch(app_by_id(g_object_get_data(G_OBJECT(item), "app")));
}

static void on_row_pin(GtkMenuItem *item, gpointer data)
{
	(void)data;
	pin_toggle(g_pin_bar, PIN_TASKBAR,
		   g_object_get_data(G_OBJECT(item), "app"));
	buttons_rebuild();
	gtk_widget_queue_draw(g_area);
}

/* A button's menu, on a right-click: the application (to open another
 * window of it), pinning, and closing -- the last nearest the panel, where
 * the pointer already is. */
static void show_jump_list(int idx)
{
	struct button *b = g_ptr_array_index(g_buttons, idx);
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *item;

	if (b->app) {
		item = menu_row(menu_icon(b->app->icon), b->app->name);
		g_object_set_data_full(G_OBJECT(item), "app",
				       g_strdup(b->app->id), g_free);
		g_signal_connect(item, "activate", G_CALLBACK(on_row_launch),
				 NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

		gboolean pinned = pin_index(g_pin_bar, b->app->id) >= 0;

		item = menu_row(glyph_image(GL_PIN, 16, PANEL_TEXT),
				pinned ? "Unpin from panel" : "Pin to panel");
		g_object_set_data_full(G_OBJECT(item), "app",
				       g_strdup(b->app->id), g_free);
		g_signal_connect(item, "activate", G_CALLBACK(on_row_pin), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	if (b->wins->len) {
		GArray *wins = g_array_new(FALSE, FALSE, sizeof(Window));

		for (guint k = 0; k < b->wins->len; k++)
			g_array_append_val(wins, button_task(b, k)->win);
		item = menu_row(glyph_image(GL_CLOSE, 16, PANEL_TEXT),
				b->wins->len > 1 ? "Close all windows" :
						   "Close window");
		g_object_set_data_full(G_OBJECT(item), "wins", wins,
				       (GDestroyNotify)g_array_unref);
		g_signal_connect(item, "activate",
				 G_CALLBACK(on_row_close_windows), NULL);
		if (b->app)
			gtk_menu_shell_append(GTK_MENU_SHELL(menu),
					      gtk_separator_menu_item_new());
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	menu_popup_above(menu, idx, b->x, APP_W);
}

/* The windows of a button that has several: a click on it lists them by
 * title, to pick one. */
static void show_window_list(int idx)
{
	struct button *b = g_ptr_array_index(g_buttons, idx);
	GtkWidget *menu = gtk_menu_new();

	for (guint k = 0; k < b->wins->len; k++) {
		struct task *t = button_task(b, k);
		GtkWidget *img = NULL;

		if (t->icon) {
			GdkPixbuf *small = gdk_pixbuf_scale_simple(
				t->icon, 16, 16, GDK_INTERP_BILINEAR);

			img = gtk_image_new_from_pixbuf(small);
			g_object_unref(small);
		} else if (b->app) {
			img = menu_icon(b->app->icon);
		}

		GtkWidget *item = menu_row(img, t->title);

		g_object_set_data(G_OBJECT(item), "win",
				  (gpointer)(uintptr_t)t->win);
		g_signal_connect(item, "activate",
				 G_CALLBACK(on_row_activate_window), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
	menu_popup_above(menu, idx, b->x, APP_W);
}

/* --- the panel's own menu --------------------------------------------------------- */

static void on_row_show_desktop(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	toggle_show_desktop();
}

static void tv_open(void);

static void on_row_workspaces(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	tv_open();
}

static void on_row_command(GtkMenuItem *item, gpointer data)
{
	(void)data;
	spawn_command(g_object_get_data(G_OBJECT(item), "cmd"));
}

/* A menu row with its key at the right, in the secondary colour. */
static GtkWidget *menu_row_key(GtkWidget *img, const char *label,
			       const char *key)
{
	GtkWidget *item = menu_row(img, label);
	GtkWidget *box = gtk_bin_get_child(GTK_BIN(item));
	GtkWidget *k = gtk_label_new(key);

	gtk_style_context_add_class(gtk_widget_get_style_context(k),
				    "likeos-menu-key");
	gtk_box_pack_end(GTK_BOX(box), k, FALSE, FALSE, 0);
	return item;
}

/* A right-click on the panel itself, away from its buttons: the desktop and
 * the workspaces, the system monitor and the display settings. */
static void show_panel_menu(double x)
{
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *item;

	g_showing_desktop = prop_card(g_root, A.showing_desktop, 0) != 0;
	item = menu_row_key(glyph_image(GL_DISPLAY, 16, PANEL_TEXT),
			    g_showing_desktop ? "Show windows" : "Show desktop",
			    "Super+D");
	g_signal_connect(item, "activate", G_CALLBACK(on_row_show_desktop),
			 NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = menu_row_key(glyph_image(GL_WORKSPACES, 16, PANEL_TEXT),
			    "Workspaces", "Super+Tab");
	g_signal_connect(item, "activate", G_CALLBACK(on_row_workspaces), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			      gtk_separator_menu_item_new());

	item = menu_row(glyph_image(GL_ACTIVITY, 16, PANEL_TEXT),
			"System monitor");
	g_object_set_data(G_OBJECT(item), "cmd", (gpointer) "xterm -e top");
	g_signal_connect(item, "activate", G_CALLBACK(on_row_command), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = menu_row(glyph_image(GL_SETTINGS, 16, PANEL_TEXT),
			"Display settings");
	g_object_set_data(G_OBJECT(item), "cmd",
			  (gpointer) "/usr/bin/likeos-display-settings");
	g_signal_connect(item, "activate", G_CALLBACK(on_row_command), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	menu_popup_above(menu, HIT_NONE, x, 1);
}

/* --- the Control Center ------------------------------------------------------------ */

/* The shell's settings flyout, above the tray: a list of rows on the dark
 * shell surface.  The volume row comes first -- the speaker (mute), the
 * slider, the level, and a chevron to the list of outputs when there is a
 * choice -- then the network (its state, and its details when the row is
 * opened), the display and the workspaces, and a footer with Settings and
 * the system monitor.
 *
 * Drawn by hand like the panel, and for the same reason: the slider wanted is
 * a specific one -- a six-pixel track, a light thumb that gathers a soft
 * accent halo under the pointer and while held, a value bubble above it
 * while dragging -- and GtkScale under a theme is a long way from that.
 *
 * The flyout is an override-redirect popup that grabs the pointer and the
 * keyboard while it is open: keys go to it, and a click anywhere else closes
 * it (and is spent doing so).  Without a compositor -- ctwm does not
 * composite, and nothing else runs -- a window cannot be partly transparent,
 * so the rounded corners are cut with a shape and the soft shadow is left
 * out; with one, both are drawn properly.  Its constants and state are
 * declared above, beside the volume state. */

static int fly_text(cairo_t *cr, double x, double cy, double w,
		    const char *text, const char *font, unsigned int rgb,
		    int align)
{
	double c[4] = { ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
			(rgb & 0xff) / 255.0, 1 };

	return panel_text_full(cr, x, cy, w, text, font, c, align);
}

/* A soft shadow under a rounded rectangle: stacked translucent outlines,
 * densest at the edge, with the falloff squared the way a blur's is.  Only
 * drawn when a compositor gives the window an alpha channel. */
static void fly_shadow(cairo_t *cr, double x, double y, double w, double h,
		       double r, int spread, double dy, double alpha)
{
	for (int i = spread; i >= 1; i--) {
		double c0 = 1.0 - (double)(i - 1) / spread;
		double c1 = 1.0 - (double)i / spread;
		double a = alpha * (c0 * c0 - c1 * c1);

		panel_round_rect(cr, x - i, y - i + dy, w + 2 * i, h + 2 * i,
				 r + i);
		cairo_set_source_rgba(cr, 0, 0, 0, a);
		cairo_fill(cr);
	}
}

/* A shell surface: the solid shell colour inside a one-pixel frame in the
 * shell's border colour. */
static void shell_surface(cairo_t *cr, double x, double y, double w, double h,
			  double r)
{
	panel_round_rect(cr, x, y, w, h, r);
	panel_color(cr, PANEL_BG);
	cairo_fill(cr);
	panel_round_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
	panel_color(cr, PANEL_BORDER);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* Where everything in the flyout goes, in window coordinates.  Like layout()
 * for the panel: the drawing and the hit-testing both read it. */
struct fly_layout {
	double px, py, pw, ph;	/* the panel; the rest is shadow */
	double cy;		/* the volume row's centre line */
	double sl_x, sl_w;	/* the slider track */
	double pct_x;		/* the level, after the slider */
	double row_y[FLY_NROWS], row_h[FLY_NROWS];
	double foot_y;		/* the footer's top edge */
	int nout;		/* rows of the outputs page */
	int win_w, win_h;
};

/* Is there a choice of output?  The chevron that lists them is there only
 * then: one output, or none known, is nothing to choose between. */
static gboolean fly_can_choose_output(void)
{
	return V.online && V.have_outs && V.outs.count >= 2;
}

static void fly_layout(struct fly_layout *L)
{
	int m = F.margin;
	double y;

	L->pw = FLY_W;
	L->px = m;
	L->py = m;
	L->nout = 0;
	y = L->py + 1;
	if (F.page == 1) {
		L->nout = 1 + (int)MIN(V.outs.count, AUD_MAX_OUTPUTS);
		y += FLY_LIST_HEAD + L->nout * FLY_LIST_ROW + FLY_PAD;
		L->cy = -100;
		for (int i = 0; i < FLY_NROWS; i++)
			L->row_y[i] = L->row_h[i] = -100;
	} else {
		y += FLY_PAD;
		L->cy = y + FLY_VOL_H / 2.0;
		y += FLY_VOL_H + 1 + FLY_PAD / 2;
		for (int i = 0; i < FLY_NROWS; i++) {
			L->row_y[i] = y;
			L->row_h[i] = FLY_ROW_H +
				      (i == 0 && F.net_info ? FLY_INFO_H : 0);
			y += L->row_h[i];
		}
		y += FLY_PAD;
	}
	L->foot_y = y;
	L->ph = (y - L->py) + FLY_FOOT_H;

	double right = L->px + L->pw - FLY_PAD - 8;

	if (fly_can_choose_output())
		right -= 36;
	L->pct_x = right - 38;
	L->sl_x = L->px + FLY_TRACK_X;
	L->sl_w = L->pct_x - 12 - L->sl_x;
	L->win_w = (int)L->pw + 2 * m;
	L->win_h = (int)L->ph + 2 * m;
}

/* The thumb's travel: its centre stays a thumb's radius inside the track's
 * ends. */
static double fly_thumb_x(const struct fly_layout *L, int v)
{
	return L->sl_x + 8 + (L->sl_w - 16) * v / 100.0;
}

static int fly_value_at(const struct fly_layout *L, double x)
{
	double v = (x - L->sl_x - 8) / (L->sl_w - 16) * 100.0;

	return v < 0 ? 0 : v > 100 ? 100 : (int)floor(v + 0.5);
}

/* A control's hover tile, which is also its hit area. */
static void fly_button_rect(const struct fly_layout *L, int hit,
			    GdkRectangle *r)
{
	int right = (int)(L->px + L->pw) - FLY_PAD;
	int fy = (int)L->foot_y + (FLY_FOOT_H - FLY_BTN) / 2;

	switch (hit) {
	case FH_MUTE:
		*r = (GdkRectangle){ (int)L->px + FLY_SPK_X,
				     (int)L->cy - FLY_BTN / 2, FLY_BTN, FLY_BTN };
		break;
	case FH_OUTPUTS: /* the chevron at the volume row's right end */
		*r = (GdkRectangle){ right - 4 - 32, (int)L->cy - 16, 32, 32 };
		break;
	case FH_BACK:
		*r = (GdkRectangle){ (int)L->px + FLY_PAD + 4,
				     (int)L->py + 1 + (FLY_LIST_HEAD - 32) / 2,
				     32, 32 };
		break;
	case FH_SETTINGS: /* the footer's left end */
		*r = (GdkRectangle){ (int)L->px + FLY_PAD + 4, fy,
				     FLY_FOOT_SETTINGS_W, FLY_BTN };
		break;
	case FH_MONITOR: /* and its right end */
		*r = (GdkRectangle){ right - 4 - FLY_FOOT_MONITOR_W, fy,
				     FLY_FOOT_MONITOR_W, FLY_BTN };
		break;
	default:
		if (hit >= FH_ROW && hit < FH_ROW + FLY_NROWS) {
			int i = hit - FH_ROW;

			*r = (GdkRectangle){ (int)L->px + FLY_PAD,
					     (int)L->row_y[i] + 2,
					     (int)L->pw - 2 * FLY_PAD,
					     (int)L->row_h[i] - 4 };
			break;
		}
		if (hit >= FH_OUT && hit < FH_OUT + L->nout) {
			*r = (GdkRectangle){
				(int)L->px + FLY_PAD,
				(int)L->py + 1 + FLY_LIST_HEAD +
					(hit - FH_OUT) * FLY_LIST_ROW,
				(int)L->pw - 2 * FLY_PAD, FLY_LIST_ROW - 2
			};
			break;
		}
		*r = (GdkRectangle){ 0, 0, 0, 0 };
	}
}

static gboolean in_rect(const GdkRectangle *r, double x, double y)
{
	return x >= r->x && x < r->x + r->width && y >= r->y &&
	       y < r->y + r->height;
}

static int fly_hit(double x, double y)
{
	static const int foot[] = { FH_SETTINGS, FH_MONITOR };
	struct fly_layout L;
	GdkRectangle r;

	fly_layout(&L);
	if (x < L.px || x >= L.px + L.pw || y < L.py || y >= L.py + L.ph)
		return FH_OUTSIDE;

	for (guint i = 0; i < G_N_ELEMENTS(foot); i++) {
		fly_button_rect(&L, foot[i], &r);
		if (in_rect(&r, x, y))
			return foot[i];
	}
	if (F.page == 1) {
		fly_button_rect(&L, FH_BACK, &r);
		if (in_rect(&r, x, y))
			return FH_BACK;
		for (int i = 0; i < L.nout; i++) {
			fly_button_rect(&L, FH_OUT + i, &r);
			if (in_rect(&r, x, y))
				return FH_OUT + i;
		}
		return FH_NONE;
	}
	for (int i = 0; i < FLY_NROWS; i++) {
		fly_button_rect(&L, FH_ROW + i, &r);
		if (in_rect(&r, x, y))
			return FH_ROW + i;
	}
	fly_button_rect(&L, FH_MUTE, &r);
	if (in_rect(&r, x, y))
		return FH_MUTE;
	if (fly_can_choose_output()) {
		fly_button_rect(&L, FH_OUTPUTS, &r);
		if (in_rect(&r, x, y))
			return FH_OUTPUTS;
	}
	if (x >= L.sl_x - 4 && x < L.sl_x + L.sl_w + 4 &&
	    y >= L.cy - 16 && y < L.cy + 16)
		return FH_SLIDER;
	return FH_NONE;
}

/* A control's tile: the shell's hover fill under the pointer, a firmer one
 * while held, nothing at rest. */
static void fly_button_tile(cairo_t *cr, const struct fly_layout *L, int hit)
{
	GdkRectangle r;
	double a;

	if (F.pressed == hit && F.hover == hit)
		a = 0.12;
	else if (F.hover == hit && F.pressed == FH_NONE)
		a = 0.07;
	else
		return;
	fly_button_rect(L, hit, &r);
	panel_round_rect(cr, r.x, r.y, r.width, r.height, 10);
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	cairo_fill(cr);
}

/* The slider: a six-pixel track, filled in the accent up to the thumb, and
 * a light round thumb sixteen pixels across.  A soft accent halo of radius
 * `knob' sits behind the thumb; at rest it is the thumb's own size and does
 * not show.  The track runs from x for w pixels, centred on cy. */
static void slider_paint(cairo_t *cr, double x, double w, double cy, int value,
			 double knob, gboolean enabled)
{
	double tx = x + 8 + (w - 16) * value / 100.0;

	panel_round_rect(cr, x, cy - 3, w, 6, 3);
	cairo_set_source_rgba(cr, 1, 1, 1, enabled ? 0.14 : 0.07);
	cairo_fill(cr);
	if (enabled) {
		panel_round_rect(cr, x, cy - 3, tx - x, 6, 3);
		panel_color(cr, PANEL_ACCENT_FILL);
		cairo_fill(cr);
		if (knob > 8.5) {
			cairo_new_sub_path(cr);
			cairo_arc(cr, tx, cy, knob, 0, 2 * M_PI);
			panel_color_a(cr, PANEL_ACCENT, 0.28);
			cairo_fill(cr);
		}
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, tx, cy, 8, 0, 2 * M_PI);
	panel_color(cr, enabled ? PANEL_TEXT : PANEL_TEXT_DISABLED);
	cairo_fill(cr);
}

/* The volume row: the speaker (mute), the slider, the level, and the
 * chevron to the outputs. */
static void fly_draw_volume(cairo_t *cr, const struct fly_layout *L)
{
	GdkRectangle r;
	char pct[16];

	/* The speaker on a tile like the rows' glyphs: in the accent while
	 * the sound is on, raised when muted. */
	gboolean on = V.online && !V.muted;

	fly_button_rect(L, FH_MUTE, &r);
	panel_round_rect(cr, r.x, r.y, r.width, r.height, 10);
	panel_color(cr, on ? PANEL_ACCENT_FILL : PANEL_RAISED);
	cairo_fill(cr);
	fly_button_tile(cr, L, FH_MUTE);
	panel_color(cr, on ? 0xffffff :
		    V.online ? PANEL_TEXT : PANEL_TEXT_DISABLED);
	glyph_speaker(cr, r.x + (r.width - 16) / 2, r.y + (r.height - 16) / 2,
		      V.online ? vol_waves() : -1, FALSE);

	slider_paint(cr, L->sl_x, L->sl_w, L->cy, V.vol, F.knob, V.online);

	if (!V.online)
		g_strlcpy(pct, "Off", sizeof(pct));
	else if (V.muted)
		g_strlcpy(pct, "Muted", sizeof(pct));
	else
		g_snprintf(pct, sizeof(pct), "%d%%", V.vol);
	fly_text(cr, L->pct_x, L->cy, 40, pct, FLY_FONT, PANEL_TEXT_DIM, 0);

	if (fly_can_choose_output()) {
		fly_button_tile(cr, L, FH_OUTPUTS);
		fly_button_rect(L, FH_OUTPUTS, &r);
		panel_color(cr, PANEL_TEXT);
		glyph_draw(cr, GL_CHEVRON_RIGHT, r.x + 8, r.y + 8, 16);
	}
}

/* One row of the list: a glyph on a raised tile, a title over its state,
 * and a chevron at the right end.  `on' fills the tile with the accent. */
static void fly_draw_row(cairo_t *cr, const struct fly_layout *L, int i,
			 enum glyph g, gboolean on, const char *title,
			 const char *state, enum glyph chev)
{
	GdkRectangle r;
	double cy = L->row_y[i] + FLY_ROW_H / 2.0;
	double gx = L->px + FLY_PAD + 8;

	fly_button_tile(cr, L, FH_ROW + i);
	fly_button_rect(L, FH_ROW + i, &r);

	panel_round_rect(cr, gx, cy - 18, 36, 36, 10);
	panel_color(cr, on ? PANEL_ACCENT_FILL : PANEL_RAISED);
	cairo_fill(cr);
	panel_color(cr, on ? 0xffffff : PANEL_TEXT);
	glyph_draw(cr, g, gx + 10, cy - 8, 16);

	fly_text(cr, gx + 48, cy - 8, r.width - 110, title, FLY_FONT_BOLD,
		 PANEL_TEXT, 0);
	fly_text(cr, gx + 48, cy + 9, r.width - 110, state, FLY_FONT,
		 PANEL_TEXT_DIM, 0);
	panel_color(cr, PANEL_TEXT_DIM);
	glyph_draw(cr, chev, r.x + r.width - 30, cy - 8, 16);
}

static void fly_draw_rows(cairo_t *cr, const struct fly_layout *L)
{
	char state[96], ws[64];

	/* The hairline between the volume row and the list. */
	panel_color(cr, PANEL_BORDER);
	cairo_rectangle(cr, L->px + 16, L->row_y[0] - FLY_PAD / 2 - 1,
			L->pw - 32, 1);
	cairo_fill(cr);

	if (g_net.up)
		g_snprintf(state, sizeof(state), "Connected \xc2\xb7 %s",
			   g_net.addr);
	else
		g_strlcpy(state, "Not connected", sizeof(state));
	fly_draw_row(cr, L, 0, g_net.up ? GL_NET : GL_NO_NET, g_net.up,
		     "Network", state,
		     F.net_info ? GL_CHEVRON_UP : GL_CHEVRON_DOWN);

	/* The network row's details, opened by a click on it. */
	if (F.net_info) {
		double x = L->px + FLY_PAD + 56;
		double y = L->row_y[0] + FLY_ROW_H - 4;

		fly_text(cr, x, y + 10, 100, "Interface", FLY_FONT,
			 PANEL_TEXT_DIM, 0);
		fly_text(cr, x + 100, y + 10, 180,
			 g_net.up ? g_net.name : "none", FLY_FONT, PANEL_TEXT,
			 0);
		fly_text(cr, x, y + 28, 100, "IPv4 address", FLY_FONT,
			 PANEL_TEXT_DIM, 0);
		fly_text(cr, x + 100, y + 28, 180,
			 g_net.up ? g_net.addr : "none", FLY_FONT, PANEL_TEXT,
			 0);
	}

	fly_draw_row(cr, L, 1, GL_DISPLAY, FALSE, "Display",
		     "Resolution and layout", GL_CHEVRON_RIGHT);
	g_snprintf(ws, sizeof(ws), "Workspace %ld of %ld", g_desktop + 1,
		   g_ndesktops < 1 ? 1 : g_ndesktops);
	fly_draw_row(cr, L, 2, GL_WORKSPACES, FALSE, "Workspaces", ws,
		     GL_CHEVRON_RIGHT);
}

/* The output for row `i' of the outputs page: -1 for "Automatic". */
static int fly_out_index(int i)
{
	return i - 1;
}

static const char *fly_out_name(const struct aud_output *o)
{
	if (o->name[0])
		return o->name;
	switch (o->type) {
	case AUDIO_OUT_HEADPHONE:
	case AUDIO_OUT_HEADSET:
		return "Headphones";
	case AUDIO_OUT_HDMI:
	case AUDIO_OUT_DP:
		return "Display audio";
	default:
		return "Speakers";
	}
}

/* The outputs page: a back chevron and the heading, then "Automatic" and
 * every output, the one in use ticked. */
static void fly_draw_outputs(cairo_t *cr, const struct fly_layout *L)
{
	GdkRectangle r;

	fly_button_tile(cr, L, FH_BACK);
	fly_button_rect(L, FH_BACK, &r);
	panel_color(cr, PANEL_TEXT);
	glyph_draw(cr, GL_CHEVRON_LEFT, r.x + 8, r.y + 8, 16);
	fly_text(cr, r.x + 42, r.y + 16, 200, "Sound output",
		 "Selawik Semibold 10.5", PANEL_TEXT, 0);

	for (int i = 0; i < L->nout; i++) {
		int k = fly_out_index(i);
		const struct aud_output *o = k >= 0 ? &V.outs.out[k] : NULL;
		gboolean cur = o ? (V.outs.mode == AUDIO_OUTSEL_ID && o->active) :
				   V.outs.mode == AUDIO_OUTSEL_AUTO;
		gboolean absent = o && !o->present;
		double cy;

		fly_button_tile(cr, L, FH_OUT + i);
		fly_button_rect(L, FH_OUT + i, &r);
		cy = r.y + r.height / 2.0;
		panel_color(cr, absent ? PANEL_TEXT_DISABLED : PANEL_TEXT);
		if (o)
			glyph_speaker(cr, r.x + 14, cy - 8, 2, FALSE);
		else
			glyph_draw(cr, GL_RESTART, r.x + 14, cy - 8, 16);
		fly_text(cr, r.x + 44, absent ? cy - 8 : cy, r.width - 90,
			 o ? fly_out_name(o) : "Automatic",
			 cur ? FLY_FONT_BOLD : FLY_FONT,
			 absent ? PANEL_TEXT_DISABLED : PANEL_TEXT, 0);
		if (absent)
			fly_text(cr, r.x + 44, cy + 9, r.width - 90,
				 "Not connected", FLY_FONT, PANEL_TEXT_DISABLED,
				 0);
		if (cur) {
			panel_color(cr, PANEL_ACCENT);
			glyph_draw(cr, GL_CHECK, r.x + r.width - 30, cy - 8, 16);
		}
	}
}

/* The footer: a strip a shade lighter than the body, with Settings at its
 * left and the system monitor at its right, each a glyph and its name. */
static void fly_draw_footer(cairo_t *cr, const struct fly_layout *L)
{
	static const struct {
		int hit;
		enum glyph g;
		const char *name;
	} btn[] = {
		{ FH_SETTINGS, GL_SETTINGS, "Settings" },
		{ FH_MONITOR, GL_ACTIVITY, "System monitor" },
	};
	GdkRectangle r;

	cairo_save(cr);
	panel_round_rect(cr, L->px + 1, L->py + 1, L->pw - 2, L->ph - 2,
			 FLY_RADIUS - 1);
	cairo_clip(cr);
	cairo_rectangle(cr, L->px, L->foot_y, L->pw, FLY_FOOT_H);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.03);
	cairo_fill(cr);
	panel_color(cr, PANEL_BORDER);
	cairo_rectangle(cr, L->px, L->foot_y, L->pw, 1);
	cairo_fill(cr);
	cairo_restore(cr);

	for (guint i = 0; i < G_N_ELEMENTS(btn); i++) {
		fly_button_tile(cr, L, btn[i].hit);
		fly_button_rect(L, btn[i].hit, &r);
		panel_color(cr, PANEL_TEXT);
		glyph_draw(cr, btn[i].g, r.x + 10, r.y + 10, 16);
		fly_text(cr, r.x + 36, r.y + r.height / 2.0, r.width - 44,
			 btn[i].name, FLY_FONT, PANEL_TEXT, 0);
	}
}

/* The whole flyout, into a cairo context the size of the window. */
static void fly_paint(cairo_t *cr)
{
	struct fly_layout L;

	fly_layout(&L);

	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);

	if (F.margin > 0)
		fly_shadow(cr, L.px, L.py, L.pw, L.ph, FLY_RADIUS,
			   F.margin - 4, 8, 0.45);
	shell_surface(cr, L.px, L.py, L.pw, L.ph, FLY_RADIUS);
	fly_draw_footer(cr, &L);
	if (F.page == 1) {
		fly_draw_outputs(cr, &L);
	} else {
		fly_draw_volume(cr, &L);
		fly_draw_rows(cr, &L);
	}
}

/* The value bubble shown above the thumb while it is dragged. */
static void tip_paint(cairo_t *cr, int w, int h)
{
	int m = F.tip_margin;
	char num[8];

	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);

	if (m > 0)
		fly_shadow(cr, m, m, w - 2 * m, h - 2 * m, 10, m - 2, 2, 0.4);
	panel_round_rect(cr, m, m, w - 2 * m, h - 2 * m, 10);
	panel_color(cr, PANEL_RAISED);
	cairo_fill(cr);
	panel_round_rect(cr, m + 0.5, m + 0.5, w - 2 * m - 1, h - 2 * m - 1,
			 9.5);
	panel_color(cr, PANEL_BORDER);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	g_snprintf(num, sizeof(num), "%d", V.vol);
	fly_text(cr, m, h / 2.0, w - 2 * m, num, FLY_FONT_BOLD, PANEL_TEXT, 1);
}

/* --- the flyouts' windows ----------------------------------------------------------- */

/* Rounded corners for a popup.  With a compositor the window has an alpha
 * channel and the corners are simply not painted; without one they would come
 * out black, so the window is shaped to the rounded outline instead. */
static void popup_shape(GtkWidget *w, int width, int height, double radius)
{
	if (F.composited) {
		gtk_widget_shape_combine_region(w, NULL);
		return;
	}

	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A1, width,
							height);
	cairo_t *cr = cairo_create(s);

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	panel_round_rect(cr, 0, 0, width, height, radius);
	cairo_fill(cr);
	cairo_destroy(cr);

	cairo_region_t *rg = gdk_cairo_region_create_from_surface(s);

	gtk_widget_shape_combine_region(w, rg);
	cairo_region_destroy(rg);
	cairo_surface_destroy(s);
}

/* The colour the X server fills a window with before the program has drawn
 * it -- when it is first mapped, and wherever it is exposed later.  The X
 * window of a GTK toplevel has none of its own (or the theme's light window
 * colour, see the GTK patch "new windows filled with their background"), so
 * a shell surface would flash light or show what was under it until its
 * first frame: each one is given its own dark colour instead, kept for its
 * lifetime.  Not for a window with an alpha channel (with a compositor):
 * its shadow margin must start out clear. */
static void window_set_x_background(GtkWidget *w, unsigned int rgb)
{
	GdkWindow *gw = gtk_widget_get_window(w);
	GdkRGBA c = { ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
		      (rgb & 0xff) / 255.0, 1 };

	if (!gw || gdk_window_get_visual(gw) !=
			   gdk_screen_get_system_visual(gtk_widget_get_screen(w)))
		return;
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gdk_window_set_background_rgba(gw, &c);
	G_GNUC_END_IGNORE_DEPRECATIONS
}

static void on_shell_realize(GtkWidget *w, gpointer data)
{
	window_set_x_background(w, GPOINTER_TO_UINT(data));
}

/* The panel's own X background: its colour as it will be drawn -- the shell
 * colour, washed over the average of the wallpaper behind it when there is
 * one to sample (see tint_paint()) -- so that from the moment it is mapped,
 * and before its first frame, the bar is already the colour it ends up.
 * (rx, ry, w, h) is where the panel is on the root window. */
static void panel_x_background(GtkWidget *win, int rx, int ry, int w, int h)
{
	unsigned int rgb = PANEL_BG;

	if (g_wall_small) {
		int sw = cairo_image_surface_get_width(g_wall_small);
		int sh = cairo_image_surface_get_height(g_wall_small);
		int stride = cairo_image_surface_get_stride(g_wall_small);
		const unsigned char *px = cairo_image_surface_get_data(g_wall_small);
		int x0 = CLAMP(rx / WALL_SHRINK, 0, sw - 1);
		int x1 = CLAMP((rx + w - 1) / WALL_SHRINK, x0, sw - 1);
		int y0 = CLAMP(ry / WALL_SHRINK, 0, sh - 1);
		int y1 = CLAMP((ry + h - 1) / WALL_SHRINK, y0, sh - 1);
		double sum[3] = { 0, 0, 0 };
		int n = 0;

		cairo_surface_flush(g_wall_small);
		for (int y = y0; px && y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				guint32 v = *(const guint32 *)(px + y * stride +
							       4 * x);

				sum[0] += (v >> 16) & 0xff;
				sum[1] += (v >> 8) & 0xff;
				sum[2] += v & 0xff;
				n++;
			}
		}
		if (n) {
			rgb = 0;
			for (int i = 0; i < 3; i++) {
				double own = (PANEL_BG >> (16 - 8 * i)) & 0xff;
				double c = own * TINT_AMOUNT +
					   sum[i] / n * (1 - TINT_AMOUNT);

				rgb |= (unsigned int)CLAMP(c + 0.5, 0, 255)
				       << (16 - 8 * i);
			}
		}
	}
	window_set_x_background(win, rgb);
}

/* A hand-drawn popup whose surface is `bg'. */
static GtkWidget *popup_new(GCallback draw, unsigned int bg)
{
	GtkWidget *w = gtk_window_new(GTK_WINDOW_POPUP);
	GdkScreen *scr = gtk_widget_get_screen(w);

	gtk_widget_set_app_paintable(w, TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(w), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
	if (F.composited) {
		GdkVisual *v = gdk_screen_get_rgba_visual(scr);

		if (v)
			gtk_widget_set_visual(w, v);
	}
	g_signal_connect_after(w, "realize", G_CALLBACK(on_shell_realize),
			       GUINT_TO_POINTER(bg));
	g_signal_connect(w, "draw", draw, NULL);
	return w;
}

/* --- sending changes ---------------------------------------------------------- */

/* Dragging sends at most thirty levels a second.  The pointer reports far
 * more often than that, and each level is a request audiod answers; the
 * newest one always goes out, the ones in between do not matter. */
#define VOL_SEND_US (G_USEC_PER_SEC / 30)

static gint64 g_vol_sent_at;
static guint g_vol_timer;

static gboolean vol_flush(gpointer data)
{
	(void)data;
	g_vol_timer = 0;
	if (g_vol_unsent >= 0) {
		audio_request_volume(g_vol_unsent);
		g_vol_unsent = -1;
		g_vol_sent_at = g_get_monotonic_time();
	}
	return G_SOURCE_REMOVE;
}

static void vol_flush_now(void)
{
	if (g_vol_timer) {
		g_source_remove(g_vol_timer);
		g_vol_timer = 0;
	}
	vol_flush(NULL);
}

static void vol_send(int v)
{
	g_vol_unsent = v;
	if (g_vol_timer)
		return;

	gint64 since = g_get_monotonic_time() - g_vol_sent_at;

	if (since >= VOL_SEND_US)
		vol_flush(NULL);
	else
		g_vol_timer = g_timeout_add(
			(guint)((VOL_SEND_US - since) / 1000) + 1, vol_flush,
			NULL);
}

/* The "volume" event sound, played when the level has been changed so the
 * new level can be heard.  Played once the change is over -- the slider let
 * go, the wheel stopped -- rather than for every step, and never closer
 * together than a few hundred milliseconds. */
#define DING_GAP_US (G_USEC_PER_SEC / 4)

static guint g_ding_timer;
static gint64 g_ding_at;

static gboolean ding_now(gpointer data)
{
	gint64 now = g_get_monotonic_time();

	(void)data;
	g_ding_timer = 0;
	if (V.online && !V.muted && V.vol > 0 && now - g_ding_at >= DING_GAP_US) {
		g_ding_at = now;
		audio_play("volume", 1000);
	}
	return G_SOURCE_REMOVE;
}

static void ding_later(guint ms)
{
	if (g_ding_timer)
		g_source_remove(g_ding_timer);
	g_ding_timer = g_timeout_add(ms, ding_now, NULL);
}

static void tip_update(void);

/* The user set a level.  Shown at once, sent at the throttled rate.  Setting
 * a level unmutes: nobody drags a slider expecting to stay silent. */
static void vol_set_user(int v)
{
	if (!V.online)
		return;
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if (V.muted) {
		V.muted = FALSE;
		audio_request_mute(FALSE);
	}
	if (v != V.vol || g_vol_unsent >= 0) {
		V.vol = v;
		vol_send(v);
	}
	vol_state_changed();
	tip_update();
}

static void vol_toggle_mute(void)
{
	if (!V.online)
		return;
	vol_flush_now();
	V.muted = !V.muted;
	audio_request_mute(V.muted);
	vol_state_changed();
}

/* --- the value bubble ----------------------------------------------------------- */

static gboolean on_tip_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	(void)data;
	tip_paint(cr, gtk_widget_get_allocated_width(w),
		  gtk_widget_get_allocated_height(w));
	return TRUE;
}

/* Place the bubble over the thumb (shown only while it is dragged) and redraw
 * it with the value. */
static void tip_update(void)
{
	if (!F.up || !F.dragging) {
		if (F.tip)
			gtk_widget_hide(F.tip);
		return;
	}
	if (!F.tip) {
		F.tip = popup_new(G_CALLBACK(on_tip_draw), PANEL_RAISED);
		F.tip_margin = F.composited ? TIP_SHADOW : 0;
	}

	/* Sized for "100", so it does not change width as the value does. */
	PangoLayout *l = gtk_widget_create_pango_layout(F.tip, "100");
	PangoFontDescription *d = pango_font_description_from_string(TIP_FONT);
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_get_pixel_size(l, &tw, &th);
	g_object_unref(l);

	int m = F.tip_margin;
	int w = tw + 18 + 2 * m, h = th + 12 + 2 * m;
	struct fly_layout L;
	int ox = 0, oy = 0;

	fly_layout(&L);
	gdk_window_get_origin(gtk_widget_get_window(F.win), &ox, &oy);

	int x = ox + (int)floor(fly_thumb_x(&L, V.vol)) - w / 2;
	int y = oy + (int)L.cy - 8 - 8 - (h - m);

	if (w != F.tip_w || h != F.tip_h) {
		F.tip_w = w;
		F.tip_h = h;
		gtk_widget_set_size_request(F.tip, w, h);
		gtk_window_resize(GTK_WINDOW(F.tip), w, h);
		popup_shape(F.tip, w, h, 10);
	}
	gtk_window_move(GTK_WINDOW(F.tip), x, y);
	gtk_widget_show(F.tip);
	gtk_widget_queue_draw(F.tip);
}

/* --- animation ------------------------------------------------------------------ */

/* The halo round the thumb: the thumb's own radius at rest, so it does not
 * show, 12 under the pointer and 13 while held, eased between in about
 * 80 ms. */
static double fly_knob_target(void)
{
	if (F.dragging)
		return 13;
	if (F.hover == FH_SLIDER && V.online)
		return 12;
	return 8;
}

/* One frame of the halo's animation. */
static gboolean fly_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	double target = fly_knob_target();
	double dt = F.tick_last ? (now - F.tick_last) / 1e6 : 1.0 / 60;
	double step = dt * 5 / 0.083;
	gboolean more = FALSE;

	(void)d;
	F.tick_last = now;
	if (fabs(target - F.knob) <= step) {
		F.knob = target;
	} else {
		F.knob += target > F.knob ? step : -step;
		more = TRUE;
	}
	gtk_widget_queue_draw(w);
	if (!more)
		F.tick = 0;
	return more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void fly_animate(void)
{
	if (!F.win || F.tick || F.knob == fly_knob_target())
		return;
	F.tick_last = 0;
	F.tick = gtk_widget_add_tick_callback(F.win, fly_tick, NULL, NULL);
}

/* --- opening, closing, sizing ------------------------------------------------------ */

static void fly_close(void);
static void tv_open(void);
static void bar_click_through(double x_root, double y_root, int owner);

/* Size the flyout and pin it over the tray: FLY_GAP above the panel and
 * FLY_GAP in from the right edge of the screen. */
static void fly_place(void)
{
	struct fly_layout L;
	int ox = 0, oy = 0;

	fly_layout(&L);
	gdk_window_get_origin(g_gdkwin, &ox, &oy);

	GdkMonitor *mon = gdk_display_get_monitor_at_window(
		gdk_display_get_default(), g_gdkwin);
	GdkRectangle geo = { ox, oy - 768, g_panel_w, 768 };

	if (mon)
		gdk_monitor_get_geometry(mon, &geo);

	int m = F.margin;
	int x = geo.x + geo.width - FLY_GAP - (int)L.pw - m;
	int y = oy - FLY_GAP - (int)L.ph - m;

	/* A bar narrower than the monitor: stay over the bar's own end. */
	if (x + m + (int)L.pw > ox + g_panel_w - FLY_GAP)
		x = ox + g_panel_w - FLY_GAP - (int)L.pw - m;

	F.placed_h = L.win_h;
	gtk_widget_set_size_request(F.win, L.win_w, L.win_h);
	gtk_window_resize(GTK_WINDOW(F.win), L.win_w, L.win_h);
	gtk_window_move(GTK_WINDOW(F.win), x, y);
	/* And straight to the X window as well.  GTK alone was not enough: a
	 * popup shown again after being hidden came back at the position it
	 * had when it was hidden, whatever was asked in between.  The window is override-redirect, so this
	 * is final: no window manager gets a say in where it goes. */
	GdkWindow *gw = gtk_widget_get_window(F.win);

	if (gw)
		gdk_window_move_resize(gw, x, y, L.win_w, L.win_h);
	popup_shape(F.win, L.win_w, L.win_h, FLY_RADIUS);
	gtk_widget_queue_draw(F.win);
}

/* The grab that makes it a flyout: every key and every click comes here,
 * wherever it lands.  owner_events is off, so even a click on the bar is
 * reported to the flyout (outside its bounds) and closes it rather than
 * pressing a button under it: the click is spent closing it. */
static gboolean fly_grab(gpointer data)
{
	GdkWindow *w = gtk_widget_get_window(F.win);
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());

	(void)data;
	if (!F.up || !w)
		return G_SOURCE_REMOVE;
	if (gdk_seat_grab(seat, w, GDK_SEAT_CAPABILITY_ALL, FALSE, NULL, NULL,
			  NULL, NULL) == GDK_GRAB_SUCCESS)
		return G_SOURCE_REMOVE;
	/* Someone else holds a grab -- a menu closing, a client mid-drag.
	 * Retry for a moment, as GTK's own menus do; past that the flyout stays
	 * open without one and closes on the next click on the speaker. */
	if (++F.grab_tries < 25)
		return G_SOURCE_CONTINUE;
	return G_SOURCE_REMOVE;
}

static gboolean on_fly_map(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	F.grab_tries = 0;
	if (fly_grab(NULL) == G_SOURCE_CONTINUE)
		g_timeout_add(20, fly_grab, NULL);
	return FALSE;
}

static void fly_end_drag(void)
{
	if (!F.dragging)
		return;
	F.dragging = FALSE;
	vol_flush_now();
	ding_later(0);
	tip_update();
	fly_animate();
}

static void fly_close(void)
{
	if (!F.up)
		return;
	fly_end_drag();
	F.up = FALSE;
	F.hover = F.pressed = FH_NONE;
	gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
	gtk_widget_hide(F.win);
	tip_update();
	g_pressed = HIT_NONE;
	bar_rehover();
	gtk_widget_queue_draw(g_area);
}

static void fly_press(int hit, double x)
{
	struct fly_layout L;

	F.pressed = hit;
	if (hit == FH_SLIDER && V.online) {
		/* A press on the track moves the thumb to the point pressed
		 * and drags from there. */
		fly_layout(&L);
		F.dragging = TRUE;
		vol_set_user(fly_value_at(&L, x));
		fly_animate();
	}
	gtk_widget_queue_draw(F.win);
}

static gboolean on_fly_button_press(GtkWidget *w, GdkEventButton *ev,
				    gpointer d)
{
	(void)w;
	(void)d;
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;

	int hit = fly_hit(ev->x, ev->y);

	if (hit == FH_OUTSIDE) {
		fly_close();
		bar_click_through(ev->x_root, ev->y_root, F.owner);
		return TRUE;
	}
	if (ev->button == 1)
		fly_press(hit, ev->x);
	else if (ev->button == 2 && (hit == FH_MUTE || hit == FH_SLIDER))
		vol_toggle_mute();
	return TRUE;
}

static gboolean on_fly_button_release(GtkWidget *w, GdkEventButton *ev,
				      gpointer d)
{
	(void)w;
	(void)d;
	if (ev->button != 1)
		return TRUE;

	int hit = fly_hit(ev->x, ev->y);
	int was = F.pressed;

	F.pressed = FH_NONE;
	if (F.dragging)
		fly_end_drag();
	else if (hit == was && hit == FH_MUTE)
		vol_toggle_mute();
	else if (hit == was && (hit == FH_SETTINGS || hit == FH_ROW + 1)) {
		fly_close();
		app_launch(app_by_id("likeos-display-settings.desktop"));
		return TRUE;
	} else if (hit == was && hit == FH_ROW) {
		F.net_info = !F.net_info;
		fly_place();
	} else if (hit == was && hit == FH_ROW + 2) {
		fly_close();
		tv_open();
		return TRUE;
	} else if (hit == was && hit == FH_MONITOR) {
		fly_close();
		spawn_command("xterm -e top");
		return TRUE;
	} else if (hit == was && hit == FH_OUTPUTS) {
		F.page = 1;
		F.hover = FH_NONE;
		fly_place();
	} else if (hit == was && hit == FH_BACK) {
		F.page = 0;
		F.hover = FH_NONE;
		fly_place();
	} else if (hit == was && hit >= FH_OUT) {
		int k = fly_out_index(hit - FH_OUT);

		if (k < 0)
			audio_request_output(AUDIO_OUTSEL_AUTO, 0);
		else
			audio_request_output(AUDIO_OUTSEL_ID, V.outs.out[k].id);
		F.page = 0;
		F.hover = FH_NONE;
		fly_place();
	}
	if (F.win)
		gtk_widget_queue_draw(F.win);
	return TRUE;
}

static gboolean on_fly_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (F.dragging) {
		struct fly_layout L;

		fly_layout(&L);
		vol_set_user(fly_value_at(&L, ev->x));
		return TRUE;
	}

	int hit = fly_hit(ev->x, ev->y);

	if (hit == FH_OUTSIDE)
		hit = FH_NONE;
	if (hit != F.hover) {
		F.hover = hit;
		fly_animate();
		gtk_widget_queue_draw(F.win);
	}
	return TRUE;
}

static gboolean on_fly_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_RIGHT)
		vol_set_user(V.vol + 2);
	else if (ev->direction == GDK_SCROLL_DOWN ||
		 ev->direction == GDK_SCROLL_LEFT)
		vol_set_user(V.vol - 2);
	else
		return TRUE;
	ding_later(150);
	return TRUE;
}

/* The slider's keys: arrows by one, Page Up and Page Down by ten, Home and
 * End to the ends.  Esc closes, as it closes every flyout. */
static gboolean on_fly_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
	int v = V.vol;

	(void)w;
	(void)d;
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		/* Back from the outputs first, then away. */
		if (F.page == 1) {
			F.page = 0;
			fly_place();
		} else {
			fly_close();
		}
		return TRUE;
	default:
		break;
	}
	switch (ev->keyval) {
	case GDK_KEY_Left:
	case GDK_KEY_Down:
	case GDK_KEY_KP_Left:
	case GDK_KEY_KP_Down:
		v -= 1;
		break;
	case GDK_KEY_Right:
	case GDK_KEY_Up:
	case GDK_KEY_KP_Right:
	case GDK_KEY_KP_Up:
		v += 1;
		break;
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		v += 10;
		break;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		v -= 10;
		break;
	case GDK_KEY_Home:
	case GDK_KEY_KP_Home:
		v = 0;
		break;
	case GDK_KEY_End:
	case GDK_KEY_KP_End:
		v = 100;
		break;
	default:
		return TRUE;
	}
	vol_set_user(v);
	ding_later(300);
	return TRUE;
}

static gboolean on_fly_grab_broken(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	fly_close();
	return TRUE;
}

static gboolean on_fly_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	(void)w;
	(void)data;
	fly_paint(cr);
	return TRUE;
}

static void fly_open(void)
{
	if (F.up)
		return;
	F.owner = HIT_TRAY;
	net_poll(NULL);
	if (!F.win) {
		F.composited = gdk_screen_is_composited(gdk_screen_get_default());
		F.margin = F.composited ? FLY_SHADOW : 0;
		F.win = popup_new(G_CALLBACK(on_fly_draw), PANEL_BG);
		gtk_window_set_title(GTK_WINDOW(F.win), "Control Center");
		gtk_widget_add_events(F.win, GDK_BUTTON_PRESS_MASK |
						     GDK_BUTTON_RELEASE_MASK |
						     GDK_POINTER_MOTION_MASK |
						     GDK_SCROLL_MASK |
						     GDK_KEY_PRESS_MASK);
		g_signal_connect(F.win, "map-event", G_CALLBACK(on_fly_map),
				 NULL);
		g_signal_connect(F.win, "button-press-event",
				 G_CALLBACK(on_fly_button_press), NULL);
		g_signal_connect(F.win, "button-release-event",
				 G_CALLBACK(on_fly_button_release), NULL);
		g_signal_connect(F.win, "motion-notify-event",
				 G_CALLBACK(on_fly_motion), NULL);
		g_signal_connect(F.win, "scroll-event",
				 G_CALLBACK(on_fly_scroll), NULL);
		g_signal_connect(F.win, "key-press-event",
				 G_CALLBACK(on_fly_key), NULL);
		g_signal_connect(F.win, "grab-broken-event",
				 G_CALLBACK(on_fly_grab_broken), NULL);
	}
	F.hover = F.pressed = FH_NONE;
	F.page = 0;
	F.net_info = FALSE;
	F.knob = fly_knob_target();
	F.up = TRUE;
	fly_place();
	gtk_widget_show(F.win);
	fly_place(); /* again now that it is mapped; see fly_place() */
	gtk_widget_queue_draw(g_area);
}

/* Something about the volume changed -- a report from audiod, or the user. */
static void vol_state_changed(void)
{
	if (g_area) {
		gtk_widget_queue_draw(g_area);
		if (g_hover == HIT_TRAY) {
			char *tip = tray_tooltip();

			gtk_widget_set_tooltip_text(g_area, tip);
			g_free(tip);
		}
	}
	if (!F.up)
		return;
	if (!V.online)
		fly_end_drag();

	/* The output list or the chevron can come and go with a report: the
	 * flyout keeps its size only while its content does. */
	struct fly_layout L;

	fly_layout(&L);
	if (L.win_h != F.placed_h)
		fly_place();
	gtk_widget_queue_draw(F.win);
}

/* --- the other flyouts ------------------------------------------------------------ */

/* The calendar, the Workspaces strip and the overflow button's icons are
 * flyouts of one kind: a hand-drawn shell surface above the panel, with a
 * grab so that keys go to it and a click anywhere else closes it, as the
 * Control Center above works.  They share one window, since only one is ever
 * open, and differ in the handful of functions below. */
#define POP_GAP 8    /* to the panel, and to the screen's edge */
#define POP_RADIUS 14

struct pop_ops {
	void (*size)(int *w, int *h);
	void (*paint)(cairo_t *cr, int w, int h);
	int (*hit)(double x, double y);	/* a control, or -1 */
	void (*click)(int hit);
	gboolean (*key)(guint keyval);
	void (*scroll)(int dir);	/* -1 up, +1 down */
	const char *(*tooltip)(int hit);
};

enum pop_place { POP_RIGHT, POP_CENTRE, POP_OVER_OWNER };

static struct {
	GtkWidget *win;
	const struct pop_ops *ops;
	int owner;		/* the bar control it opened from */
	enum pop_place place;
	gboolean up;
	int w, h;
	int hover, pressed;
	int grab_tries;
} P = { .owner = HIT_NONE, .hover = -1, .pressed = -1 };

static void bar_click_through(double x_root, double y_root, int owner);
static void launcher_close(void);
static gboolean launcher_is_up(void);

static void pop_close(void)
{
	if (!P.up)
		return;
	P.up = FALSE;
	P.owner = HIT_NONE;
	P.hover = P.pressed = -1;
	gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
	gtk_widget_hide(P.win);
	g_pressed = HIT_NONE;
	bar_rehover();
	gtk_widget_queue_draw(g_area);
}

/* Close whatever is open: a flyout, the launcher. */
static void popups_close(void)
{
	pop_close();
	fly_close();
	launcher_close();
}

static gboolean popup_owner_up(int hit)
{
	if (hit == HIT_NONE)
		return FALSE;
	return (P.up && P.owner == hit) || (F.up && hit == F.owner) ||
	       (launcher_is_up() && hit == HIT_LAUNCHER) || g_menu_owner == hit;
}

static gboolean pop_grab(gpointer data)
{
	GdkWindow *w = P.win ? gtk_widget_get_window(P.win) : NULL;
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());

	(void)data;
	if (!P.up || !w)
		return G_SOURCE_REMOVE;
	if (gdk_seat_grab(seat, w, GDK_SEAT_CAPABILITY_ALL, FALSE, NULL, NULL,
			  NULL, NULL) == GDK_GRAB_SUCCESS)
		return G_SOURCE_REMOVE;
	return ++P.grab_tries < 25 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean on_pop_map(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	P.grab_tries = 0;
	if (pop_grab(NULL) == G_SOURCE_CONTINUE)
		g_timeout_add(20, pop_grab, NULL);
	return FALSE;
}

static gboolean on_pop_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	(void)d;
	(void)w;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
	shell_surface(cr, 0, 0, P.w, P.h, POP_RADIUS);
	P.ops->paint(cr, P.w, P.h);
	return TRUE;
}

static int pop_hit(double x, double y)
{
	if (x < 0 || y < 0 || x >= P.w || y >= P.h)
		return -2; /* outside */
	return P.ops->hit(x, y);
}

static gboolean on_pop_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;

	int hit = pop_hit(ev->x, ev->y);

	if (hit == -2) {
		int owner = P.owner;

		pop_close();
		bar_click_through(ev->x_root, ev->y_root, owner);
		return TRUE;
	}
	if (ev->button == 1) {
		P.pressed = hit;
		gtk_widget_queue_draw(P.win);
	}
	return TRUE;
}

static gboolean on_pop_release(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->button != 1 || !P.up)
		return TRUE;

	int hit = pop_hit(ev->x, ev->y);
	int was = P.pressed;

	P.pressed = -1;
	if (hit >= 0 && hit == was)
		P.ops->click(hit);
	if (P.up)
		gtk_widget_queue_draw(P.win);
	return TRUE;
}

static gboolean on_pop_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
	(void)w;
	(void)d;

	int hit = pop_hit(ev->x, ev->y);

	if (hit < 0)
		hit = -1;
	if (hit != P.hover) {
		P.hover = hit;
		gtk_widget_set_tooltip_text(
			P.win, hit >= 0 && P.ops->tooltip ? P.ops->tooltip(hit) :
							    NULL);
		gtk_widget_queue_draw(P.win);
	}
	return TRUE;
}

static gboolean on_pop_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (!P.ops->scroll)
		return TRUE;
	if (ev->direction == GDK_SCROLL_UP)
		P.ops->scroll(-1);
	else if (ev->direction == GDK_SCROLL_DOWN)
		P.ops->scroll(1);
	gtk_widget_queue_draw(P.win);
	return TRUE;
}

static void launcher_toggle(gboolean search);

static gboolean on_pop_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->keyval == GDK_KEY_Escape) {
		pop_close();
		return TRUE;
	}
	if (ev->keyval == GDK_KEY_Super_L || ev->keyval == GDK_KEY_Super_R) {
		pop_close();
		launcher_toggle(FALSE);
		return TRUE;
	}
	if (P.ops->key && P.ops->key(ev->keyval))
		gtk_widget_queue_draw(P.win);
	return TRUE;
}

static gboolean on_pop_grab_broken(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	pop_close();
	return TRUE;
}

static void pop_place(void)
{
	int ox = 0, oy = 0, x;
	struct layout L;

	gdk_window_get_origin(g_gdkwin, &ox, &oy);
	layout(NULL, g_panel_w, &L);

	switch (P.place) {
	case POP_RIGHT:
		x = ox + g_panel_w - POP_GAP - P.w;
		break;
	case POP_CENTRE:
		x = ox + (g_panel_w - P.w) / 2;
		break;
	default: {
		double cx = P.owner == HIT_CHEVRON ? L.chev_x + CHEVRON_W / 2.0 :
			    P.owner == HIT_WORKSPACES ? L.ws_x + WS_W / 2.0 :
							g_panel_w / 2.0;

		x = ox + (int)cx - P.w / 2;
		break;
	}
	}
	if (x + P.w > ox + g_panel_w - POP_GAP)
		x = ox + g_panel_w - POP_GAP - P.w;
	if (x < ox + POP_GAP)
		x = ox + POP_GAP;

	int y = oy - POP_GAP - P.h;
	GdkWindow *gw = gtk_widget_get_window(P.win);

	gtk_widget_set_size_request(P.win, P.w, P.h);
	gtk_window_resize(GTK_WINDOW(P.win), P.w, P.h);
	gtk_window_move(GTK_WINDOW(P.win), x, y);
	if (gw)
		gdk_window_move_resize(gw, x, y, P.w, P.h);
	popup_shape(P.win, P.w, P.h, POP_RADIUS);
	gtk_widget_queue_draw(P.win);
}

static void pop_open(const struct pop_ops *ops, int owner, enum pop_place place)
{
	popups_close();
	if (!P.win) {
		P.win = popup_new(G_CALLBACK(on_pop_draw), PANEL_BG);
		gtk_widget_add_events(P.win, GDK_BUTTON_PRESS_MASK |
						     GDK_BUTTON_RELEASE_MASK |
						     GDK_POINTER_MOTION_MASK |
						     GDK_SCROLL_MASK |
						     GDK_KEY_PRESS_MASK);
		g_signal_connect(P.win, "map-event", G_CALLBACK(on_pop_map), NULL);
		g_signal_connect(P.win, "button-press-event",
				 G_CALLBACK(on_pop_press), NULL);
		g_signal_connect(P.win, "button-release-event",
				 G_CALLBACK(on_pop_release), NULL);
		g_signal_connect(P.win, "motion-notify-event",
				 G_CALLBACK(on_pop_motion), NULL);
		g_signal_connect(P.win, "scroll-event", G_CALLBACK(on_pop_scroll),
				 NULL);
		g_signal_connect(P.win, "key-press-event", G_CALLBACK(on_pop_key),
				 NULL);
		g_signal_connect(P.win, "grab-broken-event",
				 G_CALLBACK(on_pop_grab_broken), NULL);
	}
	P.ops = ops;
	P.owner = owner;
	P.place = place;
	P.hover = P.pressed = -1;
	ops->size(&P.w, &P.h);
	P.up = TRUE;
	pop_place();
	gtk_widget_show(P.win);
	pop_place(); /* again now that it is mapped; see fly_place() */
	gtk_widget_queue_draw(g_area);
}

/* A control's hover tile inside a flyout: the shell's hover fill, firmer
 * while held. */
static void pop_tile(cairo_t *cr, int hit, double x, double y, double w,
		     double h, double r)
{
	double a;

	if (P.pressed == hit && P.hover == hit)
		a = 0.12;
	else if (P.hover == hit && P.pressed < 0)
		a = 0.07;
	else
		return;
	cairo_set_source_rgba(cr, 1, 1, 1, a);
	panel_round_rect(cr, x, y, w, h, r);
	cairo_fill(cr);
}

/* --- the calendar ----------------------------------------------------------------- */

/* The clock's flyout: the time large at the top with the date written out
 * under it, then the month with arrows to page through it, and the days as
 * a grid of rounded cells -- today's filled in the accent, the ISO week
 * numbers down the left.  Weeks start on Monday. */
#define CAL_PAD 16
#define CAL_WK 28	/* the week numbers' column */
#define CAL_CELL 40
#define CAL_ROW 36
#define CAL_W (2 * CAL_PAD + CAL_WK + 7 * CAL_CELL)
#define CAL_HEAD_H 80
#define CAL_MONTH_H 48
#define CAL_WDAY_H 24
#define CAL_X0 (CAL_PAD + CAL_WK)
#define CAL_GRID_Y (CAL_HEAD_H + 1 + CAL_MONTH_H + CAL_WDAY_H)

#define CH_TODAY 100	/* the time and date: back to this month */
#define CH_PREV 101
#define CH_NEXT 102
				/* 0..41: the day cells */

static struct {
	int year, month;	/* the month shown, 1..12 */
	int sel_y, sel_m, sel_d; /* a day clicked on, or 0 */
} C;

static void cal_today(int *y, int *m, int *d)
{
	GDateTime *now = g_date_time_new_now_local();

	*y = g_date_time_get_year(now);
	*m = g_date_time_get_month(now);
	*d = g_date_time_get_day_of_month(now);
	g_date_time_unref(now);
}

static void cal_size(int *w, int *h)
{
	*w = CAL_W;
	*h = CAL_GRID_Y + 6 * CAL_ROW + 14;
}

/* The date in cell `i': its year, month and day. */
static void cal_cell_date(int i, int *y, int *m, int *d)
{
	GDate *first = g_date_new_dmy(1, C.month, C.year);
	int lead = g_date_get_weekday(first) - G_DATE_MONDAY; /* 0..6 */

	g_date_subtract_days(first, lead);
	g_date_add_days(first, i);
	*y = g_date_get_year(first);
	*m = g_date_get_month(first);
	*d = g_date_get_day(first);
	g_date_free(first);
}

static void cal_paint(cairo_t *cr, int w, int h)
{
	static const double text[4] = { 0xee / 255.0, 0xf2 / 255.0,
					0xfa / 255.0, 1 };
	static const double dim[4] = { 0xa4 / 255.0, 0xad / 255.0,
				       0xbf / 255.0, 1 };
	static const double off[4] = { 0x6b / 255.0, 0x74 / 255.0,
				       0x86 / 255.0, 1 };
	static const double white[4] = { 1, 1, 1, 1 };
	int ty, tm, td;
	char buf[128];

	(void)h;
	cal_today(&ty, &tm, &td);

	/* The time, large, over the date written out. */
	GDateTime *now = g_date_time_new_now_local();
	char *time = g_date_time_format(now, "%H:%M");
	char *date = g_date_time_format(now, "%A, %-d %B %Y");

	g_date_time_unref(now);
	pop_tile(cr, CH_TODAY, 8, 8, w - 16, CAL_HEAD_H - 16, 10);
	panel_text_full(cr, CAL_PAD + 4, 32, w - 2 * CAL_PAD, time,
			"Selawik Semibold 20", text, 0);
	panel_text_full(cr, CAL_PAD + 4, 58, w - 2 * CAL_PAD, date,
			"Selawik 9", dim, 0);
	g_free(time);
	g_free(date);

	panel_color(cr, PANEL_BORDER);
	cairo_rectangle(cr, 0, CAL_HEAD_H, w, 1);
	cairo_fill(cr);

	/* The month, and the arrows that page through the months. */
	double my = CAL_HEAD_H + 1 + CAL_MONTH_H / 2.0;
	GDateTime *first = g_date_time_new_local(C.year, C.month, 1, 0, 0, 0);
	char *month = g_date_time_format(first, "%B %Y");

	g_date_time_unref(first);
	panel_text_full(cr, CAL_PAD + 4, my, 200, month,
			"Selawik Semibold 10.5", text, 0);
	g_free(month);

	double ax = w - CAL_PAD - 2 * 32 - 4;

	pop_tile(cr, CH_PREV, ax, my - 16, 32, 32, 8);
	pop_tile(cr, CH_NEXT, ax + 36, my - 16, 32, 32, 8);
	panel_color(cr, PANEL_TEXT);
	glyph_draw(cr, GL_CHEVRON_LEFT, ax + 8, my - 8, 16);
	glyph_draw(cr, GL_CHEVRON_RIGHT, ax + 36 + 8, my - 8, 16);

	/* The weekday names, Monday first, over the week column's heading. */
	GDate *mon = g_date_new_dmy(5, 1, 2026); /* a Monday */
	double wy = CAL_HEAD_H + 1 + CAL_MONTH_H + CAL_WDAY_H / 2.0;

	panel_text_full(cr, CAL_PAD, wy, CAL_WK, "Wk", "Selawik 8", off, 1);
	for (int i = 0; i < 7; i++) {
		char wd[16];

		g_date_strftime(wd, sizeof(wd), "%a", mon);
		g_date_add_days(mon, 1);
		panel_text_full(cr, CAL_X0 + i * CAL_CELL, wy, CAL_CELL, wd,
				"Selawik 8", dim, 1);
	}
	g_date_free(mon);

	for (int i = 0; i < 42; i++) {
		int y, m, d;
		double cx = CAL_X0 + (i % 7) * CAL_CELL;
		double cy = CAL_GRID_Y + (i / 7) * CAL_ROW;
		double mid = cy + CAL_ROW / 2.0;
		gboolean today, sel, weekend = i % 7 >= 5;

		cal_cell_date(i, &y, &m, &d);
		today = y == ty && m == tm && d == td;
		sel = y == C.sel_y && m == C.sel_m && d == C.sel_d;

		/* The row's ISO week number, at its Monday. */
		if (i % 7 == 0) {
			GDate *gd = g_date_new_dmy(d, m, y);

			g_snprintf(buf, sizeof(buf), "%u",
				   g_date_get_iso8601_week_of_year(gd));
			g_date_free(gd);
			panel_text_full(cr, CAL_PAD, mid, CAL_WK, buf,
					"Selawik 8", off, 1);
		}

		panel_round_rect(cr, cx + 3, cy + 2, CAL_CELL - 6, CAL_ROW - 4, 8);
		if (today) {
			panel_color(cr, PANEL_ACCENT_FILL);
			cairo_fill_preserve(cr);
		}
		if (P.hover == i) {
			cairo_set_source_rgba(cr, 1, 1, 1,
					      P.pressed == i ? 0.12 : 0.08);
			cairo_fill_preserve(cr);
		}
		cairo_new_path(cr);
		if (sel && !today) {
			panel_color(cr, PANEL_ACCENT);
			cairo_set_line_width(cr, 1.5);
			panel_round_rect(cr, cx + 3.75, cy + 2.75, CAL_CELL - 7.5,
					 CAL_ROW - 5.5, 7.25);
			cairo_stroke(cr);
		}
		g_snprintf(buf, sizeof(buf), "%d", d);
		panel_text_full(cr, cx, mid, CAL_CELL, buf,
				today ? "Selawik Semibold 9" : "Selawik 9",
				today	       ? white :
				m != C.month ? off :
				weekend      ? dim :
					       text,
				1);
	}
}

static int cal_hit(double x, double y)
{
	double my = CAL_HEAD_H + 1 + CAL_MONTH_H / 2.0;
	double ax = CAL_W - CAL_PAD - 2 * 32 - 4;

	if (y >= 8 && y < CAL_HEAD_H - 8 && x >= 8 && x < CAL_W - 8)
		return CH_TODAY;
	if (y >= my - 16 && y < my + 16) {
		if (x >= ax && x < ax + 32)
			return CH_PREV;
		if (x >= ax + 36 && x < ax + 68)
			return CH_NEXT;
	}
	if (x >= CAL_X0 && x < CAL_X0 + 7 * CAL_CELL && y >= CAL_GRID_Y &&
	    y < CAL_GRID_Y + 6 * CAL_ROW)
		return (int)((y - CAL_GRID_Y) / CAL_ROW) * 7 +
		       (int)((x - CAL_X0) / CAL_CELL);
	return -1;
}

static void cal_step(int dir)
{
	C.month += dir;
	if (C.month < 1) {
		C.month = 12;
		C.year--;
	} else if (C.month > 12) {
		C.month = 1;
		C.year++;
	}
}

static void cal_click(int hit)
{
	int d;

	if (hit == CH_PREV) {
		cal_step(-1);
	} else if (hit == CH_NEXT) {
		cal_step(1);
	} else if (hit == CH_TODAY) {
		cal_today(&C.year, &C.month, &d);
		C.sel_y = C.sel_m = C.sel_d = 0;
	} else if (hit >= 0 && hit < 42) {
		int y, m;

		cal_cell_date(hit, &y, &m, &d);
		C.sel_y = y;
		C.sel_m = m;
		C.sel_d = d;
		/* A day of the month before or after pages to it. */
		if (m != C.month) {
			C.year = y;
			C.month = m;
		}
	}
}

static gboolean cal_key(guint keyval)
{
	switch (keyval) {
	case GDK_KEY_Page_Up:
	case GDK_KEY_Up:
	case GDK_KEY_Left:
		cal_step(-1);
		return TRUE;
	case GDK_KEY_Page_Down:
	case GDK_KEY_Down:
	case GDK_KEY_Right:
		cal_step(1);
		return TRUE;
	case GDK_KEY_Home:
		cal_click(CH_TODAY);
		return TRUE;
	default:
		return FALSE;
	}
}

static void cal_scroll(int dir)
{
	cal_step(dir);
}

static const struct pop_ops g_cal_ops = {
	.size = cal_size,
	.paint = cal_paint,
	.hit = cal_hit,
	.click = cal_click,
	.key = cal_key,
	.scroll = cal_scroll,
};

static void cal_open(void)
{
	int d;

	cal_today(&C.year, &C.month, &d);
	C.sel_y = C.sel_m = C.sel_d = 0;
	pop_open(&g_cal_ops, HIT_CLOCK, POP_RIGHT);
}

/* --- Workspaces ----------------------------------------------------------------------- */

/* The workspaces, as a strip of cards above the Workspaces button: each a
 * small picture of the workspace -- the wallpaper with its windows on it,
 * each with its icon -- and its number and name under it, the current one
 * outlined in the accent.  Clicking a card switches to it.  The windows are
 * read when the flyout opens. */
#define TV_THUMB_W 168
#define TV_PAD 12
#define TV_GAP 10
#define TV_LABEL_H 36
#define TV_MAX 8

struct tv_win {
	long desktop;
	int x, y, w, h;		/* on the root window */
	GdkPixbuf *icon;	/* owned */
	gboolean active;
};

static struct {
	GArray *wins;		/* struct tv_win */
	int scr_w, scr_h;
	double thumb_h;
	char **names;		/* _NET_DESKTOP_NAMES, or NULL */
} T;

static void tv_free(void)
{
	if (T.wins) {
		for (guint i = 0; i < T.wins->len; i++) {
			struct tv_win *v = &g_array_index(T.wins, struct tv_win, i);

			if (v->icon)
				g_object_unref(v->icon);
		}
		g_array_set_size(T.wins, 0);
	}
	g_strfreev(T.names);
	T.names = NULL;
}

/* Every managed window on every desktop, with where it is.  The geometry is
 * the client's own, which leaves out the title bar ctwm puts on it; for a
 * picture this size that is close enough. */
static void tv_scan(void)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.client_list, XA_WINDOW, NULL, &n);
	GdkDisplay *disp = gdk_display_get_default();

	tv_free();
	if (!T.wins)
		T.wins = g_array_new(FALSE, TRUE, sizeof(struct tv_win));
	T.scr_w = DisplayWidth(g_dpy, DefaultScreen(g_dpy));
	T.scr_h = DisplayHeight(g_dpy, DefaultScreen(g_dpy));
	if (!d)
		return;

	Window *w = (Window *)d;

	for (unsigned long i = 0; i < n; i++) {
		XWindowAttributes wa;
		Window child;
		int rx, ry;

		if (prop_has_atom(w[i], A.window_type, A.type_dock) ||
		    prop_has_atom(w[i], A.window_type, A.type_desktop) ||
		    prop_has_atom(w[i], A.net_wm_state, A.skip_taskbar) ||
		    win_iconic(w[i]))
			continue;
		gdk_x11_display_error_trap_push(disp);
		Status ok = XGetWindowAttributes(g_dpy, w[i], &wa) &&
			    XTranslateCoordinates(g_dpy, w[i], g_root, 0, 0, &rx,
						  &ry, &child);
		gdk_x11_display_error_trap_pop_ignored(disp);
		if (!ok)
			continue;

		struct tv_win v;

		v.desktop = prop_card(w[i], A.wm_desktop, -1);
		v.x = rx;
		v.y = ry - 36;
		v.w = wa.width;
		v.h = wa.height + 36;
		v.icon = win_icon(w[i]);
		if (!v.icon) {
			/* No icon of its own: its application's. */
			struct task t = { 0 };
			struct app *a;

			win_class(w[i], &t.res_name, &t.res_class);
			a = app_for_task(&t);
			if (a && app_icon(a, 16))
				v.icon = g_object_ref(app_icon(a, 16));
			g_free(t.res_name);
			g_free(t.res_class);
		}
		v.active = w[i] == g_active;
		g_array_append_val(T.wins, v);
	}
	XFree(d);

	/* The desktops' names, if the window manager publishes any worth
	 * showing; "1", "2", ... are not. */
	unsigned long nn = 0;
	unsigned char *names = prop_get(g_root, A.desktop_names, A.utf8, NULL,
					&nn);

	if (names) {
		GPtrArray *list = g_ptr_array_new();

		for (unsigned long off = 0; off < nn;) {
			const char *s = (const char *)names + off;

			g_ptr_array_add(list, g_strdup(s));
			off += strlen(s) + 1;
		}
		g_ptr_array_add(list, NULL);
		T.names = (char **)g_ptr_array_free(list, FALSE);
		XFree(names);
	}
}

static int tv_count(void)
{
	return g_ndesktops < 1 ? 1 : g_ndesktops > TV_MAX ? TV_MAX :
							    (int)g_ndesktops;
}

static void tv_size(int *w, int *h)
{
	int n = tv_count();

	T.thumb_h = floor(TV_THUMB_W * (double)(T.scr_h ? T.scr_h : 3) /
			  (T.scr_w ? T.scr_w : 4));
	*w = 2 * TV_PAD + n * TV_THUMB_W + (n - 1) * TV_GAP;
	*h = 2 * TV_PAD + (int)T.thumb_h + TV_LABEL_H;
}

static char *tv_name(int i)
{
	if (T.names) {
		int k = 0;

		for (char **p = T.names; *p; p++, k++) {
			if (k != i)
				continue;
			gboolean digits = TRUE;

			for (const char *c = *p; *c; c++)
				if (!g_ascii_isdigit(*c))
					digits = FALSE;
			if (**p && !digits)
				return g_strdup(*p);
		}
	}
	return g_strdup_printf("Workspace %d", i + 1);
}

/* One workspace's picture: the wallpaper, or the shell's colour, with its
 * windows drawn on it in the light colours of the programs. */
static void tv_thumb(cairo_t *cr, int i, double x, double y)
{
	double s = TV_THUMB_W / (double)(T.scr_w ? T.scr_w : 1);

	cairo_save(cr);
	panel_round_rect(cr, x, y, TV_THUMB_W, T.thumb_h, 8);
	cairo_clip(cr);
	if (g_wall_small) {
		cairo_save(cr);
		cairo_translate(cr, x, y);
		cairo_scale(cr, s * WALL_SHRINK, s * WALL_SHRINK);
		cairo_set_source_surface(cr, g_wall_small, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		panel_color(cr, PANEL_RAISED);
		cairo_paint(cr);
	}

	/* Its windows, bottom of the stack first. */
	for (guint k = 0; T.wins && k < T.wins->len; k++) {
		struct tv_win *v = &g_array_index(T.wins, struct tv_win, k);

		if (v->desktop != i && v->desktop >= 0 &&
		    v->desktop != 0x7fffffff &&
		    (unsigned long)v->desktop != 0xffffffffUL)
			continue;

		double wx = x + v->x * s, wy = y + v->y * s;
		double ww = v->w * s, wh = v->h * s;

		panel_round_rect(cr, wx, wy, ww, wh, 2);
		cairo_set_source_rgb(cr, 0xfb / 255.0, 0xfc / 255.0, 0xfe / 255.0);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 0x14 / 255.0, 0x1a / 255.0,
				      0x24 / 255.0, 0.35);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		cairo_rectangle(cr, wx, wy, ww, MIN(5, wh));
		cairo_set_source_rgb(cr, 0xee / 255.0, 0xf2 / 255.0, 0xf9 / 255.0);
		cairo_fill(cr);
		if (v->icon && ww > 20 && wh > 20) {
			double is = MIN(16, MIN(ww, wh) - 4);

			draw_pixbuf(cr, v->icon, floor(wx + (ww - is) / 2),
				    floor(wy + (wh - is) / 2), is);
		}
	}
	cairo_restore(cr);
}

static void tv_paint(cairo_t *cr, int w, int h)
{
	static const double text[4] = { 0xee / 255.0, 0xf2 / 255.0,
					0xfa / 255.0, 1 };
	static const double dim[4] = { 0xa4 / 255.0, 0xad / 255.0,
				       0xbf / 255.0, 1 };
	static const double white[4] = { 1, 1, 1, 1 };

	(void)w;
	(void)h;
	for (int i = 0; i < tv_count(); i++) {
		double x = TV_PAD + i * (TV_THUMB_W + TV_GAP);
		double y = TV_PAD;
		double ly = y + T.thumb_h + TV_LABEL_H / 2.0 + 1;
		gboolean cur = i == g_desktop;
		char num[8];

		pop_tile(cr, i, x - 4, y - 4, TV_THUMB_W + 8,
			 T.thumb_h + TV_LABEL_H + 4, 10);
		tv_thumb(cr, i, x, y);

		/* The current workspace is outlined in the accent colour,
		 * the others by a faint frame. */
		if (cur) {
			panel_color(cr, PANEL_ACCENT);
			cairo_set_line_width(cr, 2);
			panel_round_rect(cr, x - 1, y - 1, TV_THUMB_W + 2,
					 T.thumb_h + 2, 9);
		} else {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
			cairo_set_line_width(cr, 1);
			panel_round_rect(cr, x + 0.5, y + 0.5, TV_THUMB_W - 1,
					 T.thumb_h - 1, 7.5);
		}
		cairo_stroke(cr);

		/* Its number on a small disc, and its name. */
		g_snprintf(num, sizeof(num), "%d", i + 1);
		cairo_new_sub_path(cr);
		cairo_arc(cr, x + 10, ly, 10, 0, 2 * M_PI);
		panel_color(cr, cur ? PANEL_ACCENT_FILL : PANEL_RAISED);
		cairo_fill(cr);
		panel_text_full(cr, x, ly, 20, num, "Selawik Semibold 8",
				cur ? white : dim, 1);

		char *name = tv_name(i);

		panel_text_full(cr, x + 28, ly, TV_THUMB_W - 28, name,
				cur ? "Selawik Semibold 9" : "Selawik 9",
				cur ? text : dim, 0);
		g_free(name);
	}
}

static int tv_hit(double x, double y)
{
	if (y < TV_PAD - 4 || y >= TV_PAD + T.thumb_h + TV_LABEL_H)
		return -1;
	for (int i = 0; i < tv_count(); i++) {
		double cx = TV_PAD + i * (TV_THUMB_W + TV_GAP);

		if (x >= cx - 4 && x < cx + TV_THUMB_W + 4)
			return i;
	}
	return -1;
}

static void tv_click(int hit)
{
	pop_close();
	if (hit != g_desktop)
		goto_desktop(hit);
}

static gboolean tv_key(guint keyval)
{
	int n = tv_count();
	int i = P.hover >= 0 && P.hover < n ? P.hover : (int)g_desktop;

	switch (keyval) {
	case GDK_KEY_Left:
		P.hover = (i + n - 1) % n;
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_Tab:
		P.hover = (i + 1) % n;
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_space:
		tv_click(i);
		return FALSE;
	default:
		return FALSE;
	}
}

static const char *tv_tooltip(int hit)
{
	static char buf[64];
	int n = 0;

	for (guint k = 0; T.wins && k < T.wins->len; k++)
		if (g_array_index(T.wins, struct tv_win, k).desktop == hit)
			n++;
	g_snprintf(buf, sizeof(buf), n == 1 ? "%d window" : "%d windows", n);
	return buf;
}

static const struct pop_ops g_tv_ops = {
	.size = tv_size,
	.paint = tv_paint,
	.hit = tv_hit,
	.click = tv_click,
	.key = tv_key,
	.tooltip = tv_tooltip,
};

static void tv_open(void)
{
	tv_scan();
	pop_open(&g_tv_ops, HIT_WORKSPACES, POP_OVER_OWNER);
}

/* --- the overflow button ---------------------------------------------------------- */

/* When the application buttons need the room, the load monitor leaves the
 * panel and this button takes its place.  Its flyout holds the tray's
 * extras: the processor and memory monitor (whose numbers are its tooltip,
 * and which opens top), the display settings, and the sound -- the Control
 * Center, as the tray on the panel opens it. */
#define OV_CELL 40
#define OV_PAD 6
#define OV_N 3

static void ov_size(int *w, int *h)
{
	*w = 2 * OV_PAD + OV_N * OV_CELL;
	*h = 2 * OV_PAD + OV_CELL;
}

static void ov_paint(cairo_t *cr, int w, int h)
{
	(void)w;
	(void)h;
	for (int i = 0; i < OV_N; i++) {
		double x = OV_PAD + i * OV_CELL, y = OV_PAD;

		pop_tile(cr, i, x + 2, y + 2, OV_CELL - 4, OV_CELL - 4, 8);
		if (i == 0) {
			/* A miniature of the load curve. */
			double gx = x + 12, gy = y + 12;

			panel_color(cr, PANEL_TEXT);
			cairo_set_line_width(cr, 1.5);
			panel_round_rect(cr, gx + 0.75, gy + 0.75, 14.5, 14.5, 3);
			cairo_stroke(cr);
			panel_color(cr, PANEL_ACCENT);
			cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			for (int k = 0; k < 4; k++) {
				int idx = g_load.nload - 4 + k;
				double v = idx >= 0 ? g_load.load[idx] : 0;
				double px = gx + 3.5 + k * 3, py = gy + 12 - v * 8;

				if (k == 0)
					cairo_move_to(cr, px, py);
				else
					cairo_line_to(cr, px, py);
			}
			cairo_stroke(cr);
		} else if (i == 1) {
			panel_color(cr, PANEL_TEXT);
			glyph_draw(cr, GL_DISPLAY, x + 12, y + 12, 16);
		} else {
			panel_color(cr, PANEL_TEXT);
			glyph_speaker(cr, x + 12, y + 12,
				      V.online ? vol_waves() : -1, TRUE);
		}
	}
}

static int ov_hit(double x, double y)
{
	if (y < OV_PAD || y >= OV_PAD + OV_CELL || x < OV_PAD)
		return -1;

	int i = (int)((x - OV_PAD) / OV_CELL);

	return i < OV_N ? i : -1;
}

static void ov_click(int hit)
{
	pop_close();
	if (hit == 0)
		spawn_command("xterm -e top");
	else if (hit == 1)
		app_launch(app_by_id("likeos-display-settings.desktop"));
	else
		fly_open();
}

/* The load monitor's numbers, as its tooltip on the panel and here. */
static char *load_tooltip(void)
{
	return g_strdup_printf(
		"Processor %d%%, memory %d%%",
		g_load.nload ? (int)(g_load.load[g_load.nload - 1] * 100 + 0.5) :
			       0,
		(int)(g_load.mem * 100 + 0.5));
}

static const char *ov_tooltip(int hit)
{
	static char buf[96];
	char *t;

	if (hit == 1)
		return "Display settings";
	t = hit == 2 ? vol_tooltip() : load_tooltip();
	g_strlcpy(buf, t, sizeof(buf));
	g_free(t);
	return buf;
}

static const struct pop_ops g_ov_ops = {
	.size = ov_size,
	.paint = ov_paint,
	.hit = ov_hit,
	.click = ov_click,
	.tooltip = ov_tooltip,
};

/* --- the launcher ---------------------------------------------------------------------- */

/* The launcher, anchored over the panel's bottom-left corner: the user and
 * the session's buttons across the top, the search field, a switch between
 * Favorites, All apps and Recent files over the page it picks, and chips for
 * the places along the foot.  Typing searches; Esc steps back and then
 * closes; the Super key opens and closes it.
 *
 * This one is built of GTK widgets -- it has a text field and lists that
 * scroll -- styled by the theme's _shell.scss through the class names set
 * here.  The window paints its own dark surface and is shaped to its rounded
 * outline.  It is built afresh each time it opens, so it always shows the
 * applications and files as they are now, and destroyed when it closes.
 *
 * It holds the pointer and the keyboard the way a GTK menu does: a seat grab
 * with owner_events set, so clicks inside it reach its widgets normally, and
 * a GTK grab, so a click on the panel comes to it too rather than to the
 * panel.  Anything outside is a click away from it and closes it. */
#define LN_W 640
#define LN_H 600
#define LN_COLS 5
#define LN_TILE_W 112
#define LN_TILE_H 84
#define LN_HEAD_H 64
#define LN_FOOT_H 56
#define LN_RADIUS 14
#define LN_CHROME 216	/* everything but the page: header, search, switch, footer */

static struct {
	GtkWidget *win, *overlay, *stack, *entry;
	GtkWidget *results, *detail;	/* the search page's two halves */
	GtkWidget *hint;		/* the prompt in the empty search box */
	GtkWidget *switcher;		/* Favorites / All apps / Recent files */
	GtkWidget *seg[3];
	GtkWidget *card, *catcher;	/* an open context menu, or NULL */
	gboolean up;
	gboolean seg_busy;		/* setting the switch from code */
	int grab_tries;
	int rows;			/* of the favourites grid */
	int w;				/* the window's width */
} S;

/* The switch's pages, in its order. */
static const char *const g_pages[3] = { "favorites", "all", "recent" };

static gboolean launcher_is_up(void)
{
	return S.up;
}

static gboolean launcher_destroy(gpointer w)
{
	gtk_widget_destroy(GTK_WIDGET(w));
	return G_SOURCE_REMOVE;
}

static void launcher_close(void)
{
	if (!S.up)
		return;
	S.up = FALSE;
	gtk_grab_remove(S.win);
	gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
	gtk_widget_hide(S.win);
	/* Destroyed from an idle: this can run inside one of its own
	 * widgets' signal handlers. */
	g_idle_add(launcher_destroy, S.win);
	memset(&S, 0, sizeof(S));
	g_pressed = HIT_NONE;
	bar_rehover();
	gtk_widget_queue_draw(g_area);
}

/* Launch from the launcher: it closes behind what it started. */
static void launcher_launch_app(const char *id)
{
	struct app *a = app_by_id(id);

	launcher_close();
	app_launch(a);
}

/* Open a folder in LikeOS Explorer. */
static void open_folder(const char *dir)
{
	char *q = g_shell_quote(dir);
	char *cmd = g_strdup_printf("likeos-explorer %s", q);

	spawn_command(cmd);
	g_free(cmd);
	g_free(q);
}

static void launcher_open_uri(const char *uri)
{
	GError *err = NULL;

	launcher_close();
	if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
		/* No handler registered for the type: the file manager
		 * still shows it. */
		char *dir = NULL;
		GFile *f = g_file_new_for_uri(uri);
		GFile *parent = g_file_get_parent(f);

		g_clear_error(&err);
		if (parent)
			dir = g_file_get_path(parent);
		if (dir)
			open_folder(dir);
		g_free(dir);
		if (parent)
			g_object_unref(parent);
		g_object_unref(f);
	}
}

/* --- the context card ---------------------------------------------------- */

/* The launcher's own menus -- the user's, a tile's right-click -- are drawn
 * inside its window as a card over the content, rather than as GTK menus.
 * A GTK menu takes the pointer grab for itself and the launcher would lose
 * it; a card is just more widgets, and the transparent catcher under it
 * closes it on a click anywhere else. */
typedef void (*card_fn)(const char *arg);

static void card_close(void)
{
	if (S.card) {
		gtk_widget_destroy(S.card);
		S.card = NULL;
	}
	if (S.catcher) {
		gtk_widget_destroy(S.catcher);
		S.catcher = NULL;
	}
}

static gboolean on_catcher_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	card_close();
	return TRUE;
}

static void on_card_item(GtkButton *b, gpointer d)
{
	card_fn fn = (card_fn)d;
	char *arg = g_strdup(g_object_get_data(G_OBJECT(b), "arg"));

	card_close();
	fn(arg);
	g_free(arg);
}

static GtkWidget *card_new(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_style_context_add_class(gtk_widget_get_style_context(box),
				    "launcher-card");
	return box;
}

static void card_add(GtkWidget *card, GtkWidget *img, const char *label,
		     card_fn fn, const char *arg)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *l = gtk_label_new(label);

	if (!img) {
		img = gtk_image_new();
		gtk_widget_set_size_request(img, 16, 16);
	}
	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(b),
				    "launcher-card-item");
	g_object_set_data_full(G_OBJECT(b), "arg", g_strdup(arg), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_card_item), (gpointer)fn);
	gtk_box_pack_start(GTK_BOX(card), b, FALSE, FALSE, 0);
}

/* Show `card' with its corner at (x, y) in the launcher's coordinates: above
 * and to the left of that point when `up'/`left' is set. */
static void card_show(GtkWidget *card, int x, int y, gboolean up,
		      gboolean left)
{
	GtkRequisition nat;

	card_close();
	S.catcher = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(S.catcher), FALSE);
	g_signal_connect(S.catcher, "button-press-event",
			 G_CALLBACK(on_catcher_press), NULL);
	gtk_overlay_add_overlay(GTK_OVERLAY(S.overlay), S.catcher);
	gtk_widget_show(S.catcher);

	/* Measured once it is in the launcher: before that it has none of
	 * its styles, and its padding and minimum widths are not yet
	 * counted. */
	S.card = card;
	gtk_widget_set_halign(card, GTK_ALIGN_START);
	gtk_widget_set_valign(card, GTK_ALIGN_START);
	gtk_overlay_add_overlay(GTK_OVERLAY(S.overlay), card);
	gtk_widget_show_all(card);
	gtk_widget_get_preferred_size(card, NULL, &nat);
	if (up)
		y -= nat.height;
	if (left)
		x -= nat.width;
	if (x + nat.width > S.w - 8)
		x = S.w - 8 - nat.width;
	if (x < 8)
		x = 8;
	if (y < 8)
		y = 8;
	gtk_widget_set_margin_start(card, x);
	gtk_widget_set_margin_top(card, y);

	/* Opened from the keyboard, the focus moves into the card so the
	 * arrows walk its items; opened with the pointer it stays put, and
	 * no focus ring appears on an item nobody went to. */
	GdkEvent *cur = gtk_get_current_event();
	gboolean keyboard = cur && (cur->type == GDK_KEY_PRESS ||
				    cur->type == GDK_KEY_RELEASE);
	GList *kids = gtk_container_get_children(GTK_CONTAINER(card));

	if (cur)
		gdk_event_free(cur);
	if (kids && keyboard)
		gtk_widget_grab_focus(kids->data);
	g_list_free(kids);
}

/* --- the card actions --------------------------------------------------- */

static void act_power_off(const char *arg)
{
	(void)arg;
	launcher_close();
	confirm_open(CONFIRM_SHUT_DOWN);
}

static void act_restart(const char *arg)
{
	(void)arg;
	launcher_close();
	confirm_open(CONFIRM_RESTART);
}

static void act_sign_out(const char *arg)
{
	(void)arg;
	launcher_close();
	confirm_open(CONFIRM_SIGN_OUT);
}

static void act_restart_desktop(const char *arg)
{
	(void)arg;
	launcher_close();
	session_restart();
}

static void act_launch(const char *id)
{
	launcher_launch_app(id);
}

static void launcher_rebuild_favorites(void);

static void act_favorite(const char *id)
{
	pin_toggle(g_favorites, PIN_FAVORITES, id);
	launcher_rebuild_favorites();
}

static void act_pin_bar(const char *id)
{
	pin_toggle(g_pin_bar, PIN_TASKBAR, id);
	buttons_rebuild();
	gtk_widget_queue_draw(g_area);
}

static void act_open_uri(const char *uri)
{
	launcher_open_uri(uri);
}

static void act_open_location(const char *uri)
{
	GFile *f = g_file_new_for_uri(uri);
	GFile *parent = g_file_get_parent(f);
	char *dir = parent ? g_file_get_path(parent) : NULL;

	launcher_close();
	if (dir)
		open_folder(dir);
	g_free(dir);
	if (parent)
		g_object_unref(parent);
	g_object_unref(f);
}

static const char *favorite_label(const char *id)
{
	return pin_index(g_favorites, id) >= 0 ? "Remove from Favorites" :
						 "Add to Favorites";
}

static const char *pin_label(const char *id)
{
	return pin_index(g_pin_bar, id) >= 0 ? "Unpin from panel" :
					       "Pin to panel";
}

/* The menu for an application: open it, make it a favourite or not, and pin
 * it to the panel or not. */
static void app_card(const char *id, int x, int y)
{
	GtkWidget *card = card_new();

	card_add(card, NULL, "Open", act_launch, id);
	card_add(card, glyph_image(GL_STAR, 16, PANEL_TEXT), favorite_label(id),
		 act_favorite, id);
	card_add(card, glyph_image(GL_PIN, 16, PANEL_TEXT), pin_label(id),
		 act_pin_bar, id);
	card_show(card, x, y, FALSE, FALSE);
}

static void file_card(const char *uri, int x, int y)
{
	GtkWidget *card = card_new();

	card_add(card, NULL, "Open", act_open_uri, uri);
	card_add(card, glyph_image(GL_FOLDER, 16, PANEL_TEXT),
		 "Open file location", act_open_location, uri);
	card_show(card, x, y, FALSE, FALSE);
}

/* Where a right-click inside a widget landed, in the launcher's
 * coordinates. */
static void menu_coords(GtkWidget *w, GdkEventButton *ev, int *x, int *y)
{
	int wx = 0, wy = 0;

	gtk_widget_translate_coordinates(w, S.overlay, (int)ev->x, (int)ev->y,
					 &wx, &wy);
	*x = wx;
	*y = wy;
}

/* --- building blocks ----------------------------------------------------- */

/* Every button of the launcher leaves the keyboard focus where it was when it
 * is clicked -- in the search box, ready for typing -- rather than taking it
 * and drawing a focus ring nobody asked for. */
static void add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
	if (GTK_IS_BUTTON(w))
		gtk_widget_set_focus_on_click(w, FALSE);
}

static GtkWidget *label_new(const char *text, const char *cls, float xalign)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), xalign);
	if (cls)
		add_class(l, cls);
	return l;
}

static GtkWidget *image_from_pixbuf(GdkPixbuf *pb, int size)
{
	GtkWidget *img = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new();

	gtk_widget_set_size_request(img, size, size);
	return img;
}

static void on_app_clicked(GtkButton *b, gpointer d)
{
	(void)d;
	launcher_launch_app(g_object_get_data(G_OBJECT(b), "app"));
}

static gboolean on_app_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	int x, y;

	(void)d;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	menu_coords(w, ev, &x, &y);
	app_card(g_object_get_data(G_OBJECT(w), "app"), x, y);
	return TRUE;
}

/* The menu key and Shift+F10 open the same card, at the widget. */
static gboolean on_app_popup_menu(GtkWidget *w, gpointer d)
{
	int x = 0, y = 0;
	GtkAllocation a;

	(void)d;
	gtk_widget_get_allocation(w, &a);
	gtk_widget_translate_coordinates(w, S.overlay, a.width / 2,
					 a.height / 2, &x, &y);
	app_card(g_object_get_data(G_OBJECT(w), "app"), x, y);
	return TRUE;
}

static void app_button_wire(GtkWidget *b, const struct app *a)
{
	g_object_set_data_full(G_OBJECT(b), "app", g_strdup(a->id), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_app_clicked), NULL);
	g_signal_connect(b, "button-press-event", G_CALLBACK(on_app_press),
			 NULL);
	g_signal_connect(b, "popup-menu", G_CALLBACK(on_app_popup_menu), NULL);
	if (a->comment && *a->comment)
		gtk_widget_set_tooltip_text(b, a->comment);
}

/* A favourite's tile: the icon at 40 pixels over the name on one line. */
static GtkWidget *app_tile(const struct app *a)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *l = label_new(a->name, "launcher-tile-label", 0.5);

	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(box),
			   image_from_pixbuf(app_icon(a, 40), 40), FALSE, FALSE,
			   0);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_size_request(b, LN_TILE_W, LN_TILE_H);
	add_class(b, "launcher-tile");
	app_button_wire(b, a);
	return b;
}

/* A row of a list: icon, name, and a second line in the secondary colour. */
static GtkWidget *list_row_box(GtkWidget *img, const char *name,
			       const char *sub)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	GtkWidget *l = label_new(name, "launcher-item-name", 0);

	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	if (sub) {
		GtkWidget *s = label_new(sub, "launcher-item-sub", 0);

		gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(text), s, FALSE, FALSE, 0);
	}
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	return row;
}

/* A scrolled page that only ever scrolls up and down. */
static GtkWidget *scroller(void)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	return sw;
}

/* --- recently used files -------------------------------------------------- */

/* When, short: "Just now", "17 min ago", "3 h ago", "Yesterday, 21:13",
 * "30 Oct". */
static char *when_text(time_t t)
{
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *then = g_date_time_new_from_unix_local(t);
	gint64 secs = g_date_time_to_unix(now) - t;
	char *s;

	if (!then) {
		s = g_strdup("");
	} else if (secs < 60) {
		s = g_strdup("Just now");
	} else if (secs < 3600) {
		s = g_strdup_printf("%d min ago", (int)(secs / 60));
	} else if (secs < 86400 &&
		   g_date_time_get_day_of_year(now) ==
			   g_date_time_get_day_of_year(then)) {
		s = g_strdup_printf("%d h ago", (int)(secs / 3600));
	} else {
		GDateTime *yest = g_date_time_add_days(now, -1);

		if (g_date_time_get_year(yest) == g_date_time_get_year(then) &&
		    g_date_time_get_day_of_year(yest) ==
			    g_date_time_get_day_of_year(then))
			s = g_date_time_format(then, "Yesterday, %H:%M");
		else if (g_date_time_get_year(now) == g_date_time_get_year(then))
			s = g_date_time_format(then, "%-d %b");
		else
			s = g_date_time_format(then, "%-d %b %Y");
		g_date_time_unref(yest);
	}
	if (then)
		g_date_time_unref(then);
	g_date_time_unref(now);
	return s;
}

static gint recent_cmp(gconstpointer a, gconstpointer b)
{
	time_t ta = gtk_recent_info_get_modified((GtkRecentInfo *)a);
	time_t tb = gtk_recent_info_get_modified((GtkRecentInfo *)b);

	return ta < tb ? 1 : ta > tb ? -1 : 0;
}

/* The recently used files that still exist, newest first, at most `max'
 * (all of them for -1).  The caller frees the list with
 * g_list_free_full(..., gtk_recent_info_unref). */
static GList *recent_files(int max)
{
	GList *all = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
	GList *out = NULL;
	int n = 0;

	all = g_list_sort(all, recent_cmp);
	for (GList *l = all; l; l = l->next) {
		GtkRecentInfo *ri = l->data;

		if ((max < 0 || n < max) && gtk_recent_info_is_local(ri) &&
		    gtk_recent_info_exists(ri)) {
			out = g_list_prepend(out, gtk_recent_info_ref(ri));
			n++;
		}
	}
	g_list_free_full(all, (GDestroyNotify)gtk_recent_info_unref);
	return g_list_reverse(out);
}

static GtkWidget *recent_icon(GtkRecentInfo *ri, int size)
{
	GIcon *gi = gtk_recent_info_get_gicon(ri);
	GtkWidget *img;

	if (gi) {
		img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DND);
		gtk_image_set_pixel_size(GTK_IMAGE(img), size);
		g_object_unref(gi);
	} else {
		img = image_from_pixbuf(icon_load("text-x-generic", size), size);
	}
	gtk_widget_set_size_request(img, size, size);
	return img;
}

static void on_uri_clicked(GtkButton *b, gpointer d)
{
	(void)d;
	launcher_open_uri(g_object_get_data(G_OBJECT(b), "uri"));
}

static gboolean on_uri_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	int x, y;

	(void)d;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	menu_coords(w, ev, &x, &y);
	file_card(g_object_get_data(G_OBJECT(w), "uri"), x, y);
	return TRUE;
}

/* The folder a recent file is in, by its name: "Documents". */
static char *recent_folder(GtkRecentInfo *ri)
{
	GFile *f = g_file_new_for_uri(gtk_recent_info_get_uri(ri));
	GFile *parent = g_file_get_parent(f);
	char *name = NULL;

	if (parent) {
		char *path = g_file_get_path(parent);

		if (path && g_strcmp0(path, g_get_home_dir()) == 0)
			name = g_strdup("Home");
		else if (path)
			name = g_path_get_basename(path);
		g_free(path);
		g_object_unref(parent);
	}
	g_object_unref(f);
	return name ? name : g_strdup("");
}

/* One recent file: its icon, its name over its folder, and when. */
static GtkWidget *recent_item(GtkRecentInfo *ri)
{
	GtkWidget *b = gtk_button_new();
	char *when = when_text(gtk_recent_info_get_modified(ri));
	char *folder = recent_folder(ri);
	GtkWidget *row = list_row_box(recent_icon(ri, 24),
				      gtk_recent_info_get_display_name(ri),
				      folder);

	gtk_box_pack_end(GTK_BOX(row), label_new(when, "launcher-item-sub", 1),
			 FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	g_free(when);
	g_free(folder);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "launcher-row");
	g_object_set_data_full(G_OBJECT(b), "uri",
			       g_strdup(gtk_recent_info_get_uri(ri)), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_uri_clicked), NULL);
	g_signal_connect(b, "button-press-event", G_CALLBACK(on_uri_press),
			 NULL);
	gtk_widget_set_tooltip_text(b, gtk_recent_info_get_uri_display(ri));
	return b;
}

/* --- the pages ---------------------------------------------------------------- */

/* Show one of the switch's pages, and light its segment. */
static void launcher_show_page(const char *name)
{
	card_close();
	gtk_stack_set_visible_child_name(GTK_STACK(S.stack), name);
	S.seg_busy = TRUE;
	for (int i = 0; i < 3; i++)
		if (S.seg[i])
			gtk_toggle_button_set_active(
				GTK_TOGGLE_BUTTON(S.seg[i]),
				g_strcmp0(name, g_pages[i]) == 0);
	S.seg_busy = FALSE;
}

static void on_segment(GtkToggleButton *b, gpointer d)
{
	if (S.seg_busy)
		return;
	/* A segment is a radio button: clicking the lit one keeps it lit. */
	if (!gtk_toggle_button_get_active(b)) {
		S.seg_busy = TRUE;
		gtk_toggle_button_set_active(b, TRUE);
		S.seg_busy = FALSE;
		return;
	}
	launcher_show_page(g_pages[GPOINTER_TO_INT(d)]);
}

/* Favorites: a grid of tiles, five to a row. */
static GtkWidget *page_favorites(void)
{
	GtkWidget *sw = scroller();
	GtkWidget *grid = gtk_grid_new();
	int n = 0;

	gtk_widget_set_size_request(sw, -1, S.rows * LN_TILE_H);
	add_class(grid, "launcher-grid");
	gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(grid, GTK_ALIGN_START);
	for (guint i = 0; i < g_favorites->len; i++) {
		struct app *a = app_by_id(g_ptr_array_index(g_favorites, i));

		if (!a)
			continue;
		gtk_grid_attach(GTK_GRID(grid), app_tile(a), n % LN_COLS,
				n / LN_COLS, 1, 1);
		n++;
	}
	if (n == 0)
		gtk_grid_attach(GTK_GRID(grid),
				label_new("Add programs here from All apps: "
					  "right-click one and choose Add to "
					  "Favorites.",
					  "launcher-empty", 0.5),
				0, 0, 1, 1);
	gtk_container_add(GTK_CONTAINER(sw), grid);
	return sw;
}

static void launcher_rebuild_favorites(void)
{
	GtkWidget *old = gtk_stack_get_child_by_name(GTK_STACK(S.stack),
						     "favorites");
	gboolean shown = old && gtk_stack_get_visible_child(GTK_STACK(S.stack)) == old;
	GtkWidget *page = page_favorites();

	if (old)
		gtk_widget_destroy(old);
	gtk_widget_show_all(page);
	gtk_stack_add_named(GTK_STACK(S.stack), page, "favorites");
	gtk_container_child_set(GTK_CONTAINER(S.stack), page, "position", 0,
				NULL);
	if (shown)
		gtk_stack_set_visible_child(GTK_STACK(S.stack), page);
}

/* A row of the All apps list. */
static GtkWidget *app_row(const struct app *a, const char *sub)
{
	GtkWidget *b = gtk_button_new();

	gtk_container_add(GTK_CONTAINER(b),
			  list_row_box(image_from_pixbuf(app_icon(a, 24), 24),
				       a->name, sub));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "launcher-row");
	app_button_wire(b, a);
	return b;
}

/* All apps: the whole catalogue, A to Z, under its initials. */
static GtkWidget *page_all(void)
{
	GtkWidget *sw = scroller();
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gunichar last = 0;

	add_class(list, "launcher-list");
	for (guint i = 0; i < g_apps->len; i++) {
		struct app *a = g_ptr_array_index(g_apps, i);
		gunichar c = g_unichar_toupper(g_utf8_get_char(a->name));

		if (a->hidden)
			continue;
		if (!g_unichar_isalpha(c))
			c = '#';
		if (c != last) {
			char buf[8] = { 0 };

			g_unichar_to_utf8(c, buf);
			gtk_box_pack_start(GTK_BOX(list),
					   label_new(buf, "launcher-letter", 0),
					   FALSE, FALSE, 0);
			last = c;
		}
		gtk_box_pack_start(GTK_BOX(list), app_row(a, NULL), FALSE,
				   FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(sw), list);
	return sw;
}

static void on_explorer(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	/* LikeOS Explorer's home lists the recent files in full. */
	launcher_close();
	spawn_command("likeos-explorer");
}

/* Recent files: every recently used file that still exists, newest first,
 * and a way on to LikeOS Explorer, which has the full history. */
static GtkWidget *page_recent(void)
{
	GtkWidget *sw = scroller();
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GList *files = recent_files(-1);

	add_class(list, "launcher-list");
	for (GList *l = files; l; l = l->next)
		gtk_box_pack_start(GTK_BOX(list), recent_item(l->data), FALSE,
				   FALSE, 0);
	if (!files)
		gtk_box_pack_start(GTK_BOX(list),
				   label_new("The files you open will show up "
					     "here, newest first.",
					     "launcher-empty", 0),
				   FALSE, FALSE, 0);
	g_list_free_full(files, (GDestroyNotify)gtk_recent_info_unref);

	GtkWidget *more = gtk_button_new_with_label("Open LikeOS Explorer");

	add_class(more, "launcher-link");
	gtk_widget_set_halign(more, GTK_ALIGN_START);
	g_signal_connect(more, "clicked", G_CALLBACK(on_explorer), NULL);
	gtk_box_pack_start(GTK_BOX(list), more, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(sw), list);
	return sw;
}

/* --- search ------------------------------------------------------------------- */

/* How well an application matches the search: its name starting with the
 * text beats a word of its name starting with it, which beats the text
 * anywhere in the name, which beats a keyword or the description. */
static int match_score(const struct app *a, const char *q)
{
	char *name = g_utf8_casefold(a->name, -1);
	int score = 0;
	const char *at = strstr(name, q);

	if (at == name)
		score = 40;
	else if (at && (at[-1] == ' ' || at[-1] == '-'))
		score = 30;
	else if (at)
		score = 20;
	else if (a->keywords && strstr(a->keywords, q))
		score = 10;
	g_free(name);
	return score;
}

struct hit {
	const struct app *app;
	int score;
};

static gint hit_cmp(gconstpointer pa, gconstpointer pb)
{
	const struct hit *a = pa, *b = pb;

	if (a->score != b->score)
		return b->score - a->score;
	return app_cmp(&a->app, &b->app);
}

/* The strip under the search results: the selected result, and what can be
 * done with it as a row of buttons. */
static void detail_add_action(GtkWidget *box, enum glyph g, const char *text,
			      card_fn fn, const char *arg)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	gtk_box_pack_start(GTK_BOX(row), glyph_image(g, 16, PANEL_TEXT), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), gtk_label_new(text), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "launcher-action");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_object_set_data_full(G_OBJECT(b), "arg", g_strdup(arg), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_card_item), (gpointer)fn);
	gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
}

static void detail_fill(GtkWidget *row)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(S.detail));

	for (GList *l = kids; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
	gtk_widget_set_visible(S.detail, row != NULL);
	if (!row)
		return;

	const char *id = g_object_get_data(G_OBJECT(row), "app");
	const char *uri = g_object_get_data(G_OBJECT(row), "uri");
	const struct app *a = app_by_id(id);
	GtkWidget *img;
	const char *name, *kind;
	char *base = NULL;

	if (a) {
		img = image_from_pixbuf(app_icon(a, 32), 32);
		name = a->name;
		kind = "Application";
	} else if (uri) {
		GFile *f = g_file_new_for_uri(uri);
		GFileInfo *fi = g_file_query_info(
			f, G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
			0, NULL, NULL);

		if (fi && g_file_info_get_icon(fi)) {
			img = gtk_image_new_from_gicon(g_file_info_get_icon(fi),
						       GTK_ICON_SIZE_DND);
			gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
		} else {
			img = image_from_pixbuf(icon_load("text-x-generic", 32),
						32);
		}
		base = fi ? g_strdup(g_file_info_get_display_name(fi)) :
			    g_file_get_basename(f);
		name = base;
		kind = "File";
		if (fi)
			g_object_unref(fi);
		g_object_unref(f);
	} else {
		return;
	}

	GtkWidget *head = list_row_box(img, name, kind);
	GList *labels = gtk_container_get_children(GTK_CONTAINER(head));

	/* The name in the strip is the selection's, set a little larger. */
	add_class(head, "launcher-detail-head");
	for (GList *k = labels; k; k = k->next)
		if (GTK_IS_BOX(k->data)) {
			GList *t = gtk_container_get_children(k->data);

			if (t)
				add_class(t->data, "launcher-detail-name");
			g_list_free(t);
		}
	g_list_free(labels);
	gtk_box_pack_start(GTK_BOX(S.detail), head, TRUE, TRUE, 0);

	GtkWidget *acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	add_class(acts, "launcher-detail-actions");
	if (a) {
		detail_add_action(acts, GL_CHEVRON_RIGHT, "Open", act_launch,
				  a->id);
		detail_add_action(acts, GL_STAR, favorite_label(a->id),
				  act_favorite, a->id);
		detail_add_action(acts, GL_PIN, pin_label(a->id), act_pin_bar,
				  a->id);
	} else {
		detail_add_action(acts, GL_CHEVRON_RIGHT, "Open", act_open_uri,
				  uri);
		detail_add_action(acts, GL_FOLDER, "Open file location",
				  act_open_location, uri);
	}
	gtk_box_pack_end(GTK_BOX(S.detail), acts, FALSE, FALSE, 0);
	gtk_widget_show_all(S.detail);
	g_free(base);
}

static void on_result_selected(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
	(void)lb;
	(void)d;
	if (S.detail)
		detail_fill(row ? GTK_WIDGET(row) : NULL);
}

static void on_result_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
	const char *id = g_object_get_data(G_OBJECT(row), "app");
	const char *uri = g_object_get_data(G_OBJECT(row), "uri");

	(void)lb;
	(void)d;
	if (id)
		launcher_launch_app(id);
	else if (uri)
		launcher_open_uri(uri);
}

static void results_heading(GtkWidget *lb, const char *text)
{
	GtkWidget *row = gtk_list_box_row_new();

	gtk_container_add(GTK_CONTAINER(row),
			  label_new(text, "launcher-results-heading", 0));
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_set_can_focus(row, FALSE);
	gtk_container_add(GTK_CONTAINER(lb), row);
}

static GtkWidget *results_row(GtkWidget *lb, GtkWidget *content,
			      gboolean best)
{
	GtkWidget *row = gtk_list_box_row_new();

	gtk_container_add(GTK_CONTAINER(row), content);
	add_class(row, best ? "launcher-best" : "launcher-result");
	gtk_container_add(GTK_CONTAINER(lb), row);
	return row;
}

static void launcher_search(const char *text)
{
	char *q = g_utf8_casefold(text, -1);
	GArray *hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	GtkWidget *lb = S.results;
	GtkWidget *first = NULL;

	g_strstrip(q);
	GList *kids = gtk_container_get_children(GTK_CONTAINER(lb));

	for (GList *l = kids; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);

	for (guint i = 0; i < g_apps->len; i++) {
		struct app *a = g_ptr_array_index(g_apps, i);
		struct hit h = { a, match_score(a, q) };

		if (!a->hidden && h.score > 0)
			g_array_append_val(hits, h);
	}
	g_array_sort(hits, hit_cmp);

	/* Files: the recently used ones whose names contain the text. */
	GList *files = recent_files(-1), *found = NULL;
	int nfound = 0;

	for (GList *l = files; l && nfound < 6; l = l->next) {
		char *dn = g_utf8_casefold(
			gtk_recent_info_get_display_name(l->data), -1);

		if (strstr(dn, q)) {
			found = g_list_append(found, l->data);
			nfound++;
		}
		g_free(dn);
	}

	if (hits->len || found) {
		results_heading(lb, "Top result");

		if (hits->len) {
			const struct app *a = g_array_index(hits, struct hit, 0).app;

			first = results_row(
				lb,
				list_row_box(image_from_pixbuf(app_icon(a, 32), 32),
					     a->name, "Application"),
				TRUE);
			g_object_set_data_full(G_OBJECT(first), "app",
					       g_strdup(a->id), g_free);
		} else {
			GtkRecentInfo *ri = found->data;

			first = results_row(
				lb,
				list_row_box(recent_icon(ri, 32),
					     gtk_recent_info_get_display_name(ri),
					     "File"),
				TRUE);
			g_object_set_data_full(G_OBJECT(first), "uri",
					       g_strdup(gtk_recent_info_get_uri(ri)),
					       g_free);
			found = g_list_delete_link(found, found);
		}
		if (hits->len > 1)
			results_heading(lb, "Applications");
		for (guint i = 1; i < hits->len && i < 9; i++) {
			const struct app *a = g_array_index(hits, struct hit, i).app;
			GtkWidget *row = results_row(
				lb,
				list_row_box(image_from_pixbuf(app_icon(a, 24), 24),
					     a->name, NULL),
				FALSE);

			g_object_set_data_full(G_OBJECT(row), "app",
					       g_strdup(a->id), g_free);
		}
		if (found)
			results_heading(lb, "Files");
		for (GList *l = found; l; l = l->next) {
			GtkRecentInfo *ri = l->data;
			GtkWidget *row = results_row(
				lb,
				list_row_box(recent_icon(ri, 24),
					     gtk_recent_info_get_display_name(ri),
					     NULL),
				FALSE);

			g_object_set_data_full(G_OBJECT(row), "uri",
					       g_strdup(gtk_recent_info_get_uri(ri)),
					       g_free);
		}
	} else {
		char *msg = g_strdup_printf(
			"Nothing matches \xe2\x80\x9c%s\xe2\x80\x9d", text);
		GtkWidget *row = gtk_list_box_row_new();

		gtk_container_add(GTK_CONTAINER(row),
				  label_new(msg, "launcher-empty", 0));
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
		gtk_container_add(GTK_CONTAINER(lb), row);
		g_free(msg);
	}
	gtk_widget_show_all(lb);
	if (first)
		gtk_list_box_select_row(GTK_LIST_BOX(lb), GTK_LIST_BOX_ROW(first));
	else
		detail_fill(NULL);

	g_list_free(found);
	g_list_free_full(files, (GDestroyNotify)gtk_recent_info_unref);
	g_array_unref(hits);
	g_free(q);
}

/* The search page: the results down the page, and the strip for the selected
 * one under them. */
static GtkWidget *page_search(void)
{
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *sw = scroller();

	S.results = gtk_list_box_new();
	add_class(S.results, "launcher-results");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(S.results),
					GTK_SELECTION_BROWSE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(S.results),
						  TRUE);
	g_signal_connect(S.results, "row-selected",
			 G_CALLBACK(on_result_selected), NULL);
	g_signal_connect(S.results, "row-activated",
			 G_CALLBACK(on_result_activated), NULL);
	gtk_container_add(GTK_CONTAINER(sw), S.results);
	gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);

	S.detail = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	add_class(S.detail, "launcher-detail");
	gtk_box_pack_end(GTK_BOX(page), S.detail, FALSE, FALSE, 0);
	return page;
}

/* The page the switch shows, by name. */
static const char *launcher_switch_page(void)
{
	for (int i = 0; i < 3; i++)
		if (S.seg[i] &&
		    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(S.seg[i])))
			return g_pages[i];
	return g_pages[0];
}

/* While there is text in the search box the results take the switch's
 * place; emptied, the box gives the page the switch was on back. */
static void on_search_changed(GtkEditable *e, gpointer d)
{
	const char *text = gtk_entry_get_text(GTK_ENTRY(e));

	(void)d;
	card_close();
	gtk_widget_set_child_visible(S.hint, !*text);
	gtk_widget_set_visible(S.switcher, !*text);
	if (!*text) {
		launcher_show_page(launcher_switch_page());
		return;
	}
	launcher_search(text);
	gtk_stack_set_visible_child_name(GTK_STACK(S.stack), "search");
}

/* Enter in the search box opens the selected result -- the top one unless
 * the arrows moved off it. */
static void on_search_activate(GtkEntry *e, gpointer d)
{
	GtkListBoxRow *row;

	(void)d;
	if (!*gtk_entry_get_text(e))
		return;
	row = gtk_list_box_get_selected_row(GTK_LIST_BOX(S.results));
	if (row)
		on_result_activated(GTK_LIST_BOX(S.results), row, NULL);
}

/* Up and Down in the search box move through the results without leaving
 * it, so the user can keep typing. */
static void results_move(int dir)
{
	GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(S.results));
	int i = cur ? gtk_list_box_row_get_index(cur) : -1;

	for (int k = i + dir; k >= 0; k += dir) {
		GtkListBoxRow *r = gtk_list_box_get_row_at_index(
			GTK_LIST_BOX(S.results), k);

		if (!r)
			break;
		if (gtk_list_box_row_get_selectable(r)) {
			gtk_list_box_select_row(GTK_LIST_BOX(S.results), r);
			return;
		}
	}
}

/* --- the header and the footer ---------------------------------------------- */

/* Up to two initials of the user's name: "Ada Lovelace" is "AL". */
static void user_initials(const char *name, char *out, size_t sz)
{
	GString *s = g_string_new(NULL);
	gboolean start = TRUE;

	for (const char *p = name; *p && s->len < 8; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isspace(c) || c == '-' || c == '.') {
			start = TRUE;
			continue;
		}
		if (start && g_unichar_isalnum(c) && g_utf8_strlen(s->str, -1) < 2)
			g_string_append_unichar(s, g_unichar_toupper(c));
		start = FALSE;
	}
	g_strlcpy(out, s->len ? s->str : "?", sz);
	g_string_free(s, TRUE);
}

/* The name to greet the user by: the full name from the password database,
 * else the login name, with its first letter up. */
static char *user_display_name(void)
{
	const char *real = g_get_real_name();

	if (real && *real && strcmp(real, "Unknown") != 0)
		return g_strdup(real);

	const char *login = g_get_user_name();
	char *s = g_strdup(login ? login : "User");

	if (*s)
		*s = g_ascii_toupper(*s);
	return s;
}

/* The user's picture: ~/.face if there is one, else the initials on a disc
 * in the accent colour. */
static gboolean on_avatar_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	int s = gtk_widget_get_allocated_width(w);
	char *face = g_build_filename(g_get_home_dir(), ".face", NULL);
	GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(face, s, s, FALSE,
							  NULL);
	static const double white[4] = { 1, 1, 1, 1 };

	(void)d;
	g_free(face);
	cairo_new_sub_path(cr);
	cairo_arc(cr, s / 2.0, s / 2.0, s / 2.0, 0, 2 * M_PI);
	cairo_clip(cr);
	if (pb) {
		gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
		cairo_paint(cr);
		g_object_unref(pb);
		return TRUE;
	}

	char *name = user_display_name();
	char ini[16];

	user_initials(name, ini, sizeof(ini));
	g_free(name);
	panel_color(cr, PANEL_ACCENT_FILL);
	cairo_paint(cr);
	panel_text_full(cr, 0, s / 2.0, s, ini, "Selawik Semibold 12", white, 1);
	return TRUE;
}

/* The user's own menu, under the name. */
static void on_user_clicked(GtkButton *b, gpointer d)
{
	GtkWidget *card = card_new();
	GtkAllocation a;
	int x = 0, y = 0;

	(void)d;
	card_add(card, glyph_image(GL_RESTART, 16, PANEL_TEXT),
		 "Restart the desktop", act_restart_desktop, NULL);
	card_add(card, glyph_image(GL_SIGNOUT, 16, PANEL_TEXT), "Sign out",
		 act_sign_out, NULL);
	gtk_widget_get_allocation(GTK_WIDGET(b), &a);
	gtk_widget_translate_coordinates(GTK_WIDGET(b), S.overlay, 0, a.height,
					 &x, &y);
	card_show(card, x, y + 4, FALSE, FALSE);
}

/* A square button with a glyph and a tooltip that calls `fn'. */
static GtkWidget *icon_button(enum glyph g, const char *tip, card_fn fn,
			      const char *arg)
{
	GtkWidget *b = gtk_button_new();

	gtk_container_add(GTK_CONTAINER(b), glyph_image(g, 16, PANEL_TEXT));
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "launcher-icon-button");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(b, tip);
	g_object_set_data_full(G_OBJECT(b), "arg", g_strdup(arg), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_card_item), (gpointer)fn);
	return b;
}

/* The header: the user at the left -- picture, name, and the machine under
 * it -- and the session's buttons at the right. */
static GtkWidget *header(void)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *user = gtk_button_new();
	GtkWidget *ubox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *avatar = gtk_drawing_area_new();
	char *name = user_display_name();
	const char *host = g_get_host_name();

	if (!host || !*host || strcmp(host, "localhost") == 0)
		host = "LikeOS";
	add_class(bar, "launcher-header");
	gtk_widget_set_size_request(bar, -1, LN_HEAD_H);
	gtk_widget_set_size_request(avatar, 40, 40);
	gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
	g_signal_connect(avatar, "draw", G_CALLBACK(on_avatar_draw), NULL);
	gtk_box_pack_start(GTK_BOX(ubox), avatar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text),
			   label_new(name, "launcher-user-name", 0), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), label_new(host, "launcher-item-sub", 0),
			   FALSE, FALSE, 0);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(ubox), text, FALSE, FALSE, 0);
	g_free(name);
	gtk_container_add(GTK_CONTAINER(user), ubox);
	gtk_button_set_relief(GTK_BUTTON(user), GTK_RELIEF_NONE);
	add_class(user, "launcher-user");
	gtk_widget_set_valign(user, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(user, "Account");
	g_signal_connect(user, "clicked", G_CALLBACK(on_user_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(bar), user, FALSE, FALSE, 0);

	/* Packed from the right: shut down at the very end. */
	gtk_box_pack_end(GTK_BOX(bar),
			 icon_button(GL_POWER, "Shut down", act_power_off, NULL),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar),
			 icon_button(GL_RESTART, "Restart", act_restart, NULL),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar),
			 icon_button(GL_SIGNOUT, "Sign out", act_sign_out, NULL),
			 FALSE, FALSE, 0);
	return bar;
}

static void act_open_folder(const char *dir)
{
	launcher_close();
	open_folder(dir);
}

/* A chip along the foot: a glyph and a name. */
static GtkWidget *chip(enum glyph g, const char *text, card_fn fn,
		       const char *arg, const char *tip)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	gtk_box_pack_start(GTK_BOX(box), glyph_image(g, 16, PANEL_TEXT), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "launcher-chip");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	if (tip)
		gtk_widget_set_tooltip_text(b, tip);
	g_object_set_data_full(G_OBJECT(b), "arg", g_strdup(arg), g_free);
	g_signal_connect(b, "clicked", G_CALLBACK(on_card_item), (gpointer)fn);
	return b;
}

/* The footer: the places -- home and the usual folders in it, the ones that
 * exist -- opened in LikeOS Explorer, and Settings at the right end. */
static GtkWidget *footer(void)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	static const struct {
		GUserDirectory dir;
		const char *name, *fallback;
		enum glyph g;
	} places[] = {
		{ G_USER_N_DIRECTORIES, "Home", NULL, GL_HOME },
		{ G_USER_DIRECTORY_DOCUMENTS, "Documents", "Documents",
		  GL_DOCUMENT },
		{ G_USER_DIRECTORY_DOWNLOAD, "Downloads", "Downloads",
		  GL_DOWNLOAD },
		{ G_USER_DIRECTORY_PICTURES, "Pictures", "Pictures",
		  GL_PICTURE },
	};

	add_class(bar, "launcher-footer");
	gtk_widget_set_size_request(bar, -1, LN_FOOT_H);
	for (guint i = 0; i < G_N_ELEMENTS(places); i++) {
		const char *home = g_get_home_dir();
		const char *dir = places[i].dir == G_USER_N_DIRECTORIES ?
					  home :
					  g_get_user_special_dir(places[i].dir);
		char *path = NULL;

		/* No user-dirs configuration names the folder: the usual
		 * name in the home directory, if it is there. */
		if (!dir || g_strcmp0(dir, home) == 0)
			path = places[i].fallback ?
				       g_build_filename(home, places[i].fallback,
							NULL) :
				       g_strdup(home);
		else
			path = g_strdup(dir);
		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			gtk_box_pack_start(GTK_BOX(bar),
					   chip(places[i].g, places[i].name,
						act_open_folder, path, path),
					   FALSE, FALSE, 0);
		g_free(path);
	}
	if (app_by_id("likeos-display-settings.desktop"))
		gtk_box_pack_end(GTK_BOX(bar),
				 chip(GL_SETTINGS, "Settings", act_launch,
				      "likeos-display-settings.desktop", NULL),
				 FALSE, FALSE, 0);
	return bar;
}

/* --- the window ------------------------------------------------------------------- */

static gboolean on_launcher_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	int ww = gtk_widget_get_allocated_width(w);
	int hh = gtk_widget_get_allocated_height(w);

	(void)d;
	panel_round_rect(cr, 0, 0, ww, hh, LN_RADIUS);
	panel_color(cr, PANEL_BG);
	cairo_fill(cr);
	return FALSE; /* the children draw over it */
}

/* The frame, over the children: the footer's strip reaches the edges. */
static gboolean on_launcher_draw_after(GtkWidget *w, cairo_t *cr, gpointer d)
{
	int ww = gtk_widget_get_allocated_width(w);
	int hh = gtk_widget_get_allocated_height(w);

	(void)d;
	panel_color(cr, PANEL_BORDER);
	cairo_set_line_width(cr, 1);
	panel_round_rect(cr, 0.5, 0.5, ww - 1, hh - 1, LN_RADIUS - 0.5);
	cairo_stroke(cr);
	return FALSE;
}

static gboolean launcher_grab(gpointer data)
{
	GdkWindow *w = S.win ? gtk_widget_get_window(S.win) : NULL;
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());

	(void)data;
	if (!S.up || !w)
		return G_SOURCE_REMOVE;
	if (gdk_seat_grab(seat, w, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL,
			  NULL, NULL) == GDK_GRAB_SUCCESS)
		return G_SOURCE_REMOVE;
	return ++S.grab_tries < 25 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Give a popup the keyboard focus in GTK's eyes.  It is override-redirect
 * and the window manager never focuses it; the keyboard grab already sends
 * every key here, but without a focus-in GTK thinks the window inactive and
 * the launcher's search box would draw no caret. */
static void popup_focus_in(GtkWidget *win)
{
	GdkEvent *fe = gdk_event_new(GDK_FOCUS_CHANGE);

	fe->focus_change.type = GDK_FOCUS_CHANGE;
	fe->focus_change.window = g_object_ref(gtk_widget_get_window(win));
	fe->focus_change.in = TRUE;
	gtk_widget_send_focus_change(win, fe);
	gdk_event_free(fe);
}

static gboolean on_launcher_map(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	S.grab_tries = 0;
	if (launcher_grab(NULL) == G_SOURCE_CONTINUE)
		g_timeout_add(20, launcher_grab, NULL);
	popup_focus_in(S.win);
	gtk_widget_grab_focus(S.entry);
	return FALSE;
}

/* A click that is not on the launcher's own widgets -- on the panel, or
 * outside this program altogether -- closes it.  On the panel it also does
 * what it would have done, unless it was on the orb, which is what toggles
 * the launcher. */
static gboolean on_launcher_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	GdkWindow *top = ev->window ? gdk_window_get_toplevel(ev->window) : NULL;

	(void)d;
	if (top == gtk_widget_get_window(w)) {
		int ww = gtk_widget_get_allocated_width(w);
		int hh = gtk_widget_get_allocated_height(w);
		int x = 0, y = 0;

		gdk_window_get_origin(top, &x, &y);
		if (ev->x_root >= x && ev->x_root < x + ww && ev->y_root >= y &&
		    ev->y_root < y + hh)
			return FALSE;
	}
	launcher_close();
	bar_click_through(ev->x_root, ev->y_root, HIT_LAUNCHER);
	return TRUE;
}

static gboolean on_launcher_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
	(void)d;
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		/* One step back at a time: the card, the search, the page,
		 * and then away. */
		if (S.card) {
			card_close();
		} else if (*gtk_entry_get_text(GTK_ENTRY(S.entry))) {
			gtk_entry_set_text(GTK_ENTRY(S.entry), "");
			gtk_widget_grab_focus(S.entry);
		} else if (g_strcmp0(gtk_stack_get_visible_child_name(
					     GTK_STACK(S.stack)),
				     g_pages[0]) != 0) {
			launcher_show_page(g_pages[0]);
		} else {
			launcher_close();
		}
		return TRUE;
	case GDK_KEY_Super_L:
	case GDK_KEY_Super_R:
		launcher_close();
		return TRUE;
	default:
		break;
	}

	gboolean in_entry = gtk_widget_has_focus(S.entry);

	if (in_entry && !S.card &&
	    g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(S.stack)),
		      "search") == 0) {
		if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_KP_Down) {
			results_move(1);
			return TRUE;
		}
		if (ev->keyval == GDK_KEY_Up || ev->keyval == GDK_KEY_KP_Up) {
			results_move(-1);
			return TRUE;
		}
	}

	/* Typing anywhere in the launcher goes into the search box: the
	 * first printable key moves the focus there. */
	gunichar c = gdk_keyval_to_unicode(ev->keyval);

	if (!in_entry && !S.card && c && g_unichar_isprint(c) &&
	    !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
		gtk_widget_grab_focus(S.entry);
		gtk_editable_set_position(GTK_EDITABLE(S.entry), -1);
		return gtk_widget_event(S.entry, (GdkEvent *)ev);
	}
	(void)w;
	return FALSE;
}

static gboolean on_launcher_grab_broken(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)d;
	/* Taken by one of the launcher's own popups (an entry's context
	 * menu): take it back when that is done rather than closing. */
	if (ev->grab_broken.grab_window &&
	    gdk_window_get_toplevel(ev->grab_broken.grab_window) !=
		    gtk_widget_get_window(S.win)) {
		g_timeout_add(100, launcher_grab, NULL);
		return TRUE;
	}
	return TRUE;
}

static GdkPixbuf *search_pixbuf(int size, unsigned int rgb)
{
	cairo_surface_t *s =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	GdkPixbuf *pb;

	panel_color(cr, rgb);
	panel_glyph_search(cr, 0, 0, size);
	cairo_destroy(cr);
	pb = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
	cairo_surface_destroy(s);
	return pb;
}

/* The switch: three segments in a raised track, one of them lit. */
static GtkWidget *switcher(void)
{
	static const char *const names[3] = { "Favorites", "All apps",
					      "Recent files" };
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	add_class(box, "launcher-switch");
	gtk_widget_set_halign(box, GTK_ALIGN_START);
	for (int i = 0; i < 3; i++) {
		S.seg[i] = gtk_toggle_button_new_with_label(names[i]);
		add_class(S.seg[i], "launcher-segment");
		g_signal_connect(S.seg[i], "toggled", G_CALLBACK(on_segment),
				 GINT_TO_POINTER(i));
		gtk_box_pack_start(GTK_BOX(box), S.seg[i], FALSE, FALSE, 0);
	}
	return box;
}

static void launcher_open(gboolean search)
{
	GdkRectangle geo = { 0, 0, g_panel_w, 768 };
	int ox = 0, oy = 0;

	popups_close();
	apps_load();
	buttons_rebuild();

	GdkMonitor *mon = gdk_display_get_monitor_at_window(
		gdk_display_get_default(), g_gdkwin);

	if (mon)
		gdk_monitor_get_geometry(mon, &geo);
	gdk_window_get_origin(g_gdkwin, &ox, &oy);

	/* Made smaller to fit a small screen: fewer rows of favourites rather
	 * than a launcher that runs off the top. */
	int h = MIN(LN_H, oy - geo.y - 2 * POP_GAP);
	int w = MIN(LN_W, geo.width - 2 * POP_GAP);

	S.rows = MAX(1, (h - LN_CHROME) / LN_TILE_H);
	S.w = w;

	S.win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_type_hint(GTK_WINDOW(S.win),
				 GDK_WINDOW_TYPE_HINT_POPUP_MENU);
	gtk_window_set_title(GTK_WINDOW(S.win), "Launcher");
	gtk_widget_set_app_paintable(S.win, TRUE);
	if (F.composited) {
		GdkVisual *v = gdk_screen_get_rgba_visual(
			gtk_widget_get_screen(S.win));

		if (v)
			gtk_widget_set_visual(S.win, v);
	}
	add_class(S.win, "likeos-launcher");
	g_signal_connect_after(S.win, "realize", G_CALLBACK(on_shell_realize),
			       GUINT_TO_POINTER(PANEL_BG));
	gtk_widget_add_events(S.win, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
	g_signal_connect(S.win, "draw", G_CALLBACK(on_launcher_draw), NULL);
	g_signal_connect_after(S.win, "draw", G_CALLBACK(on_launcher_draw_after),
			       NULL);
	g_signal_connect(S.win, "map-event", G_CALLBACK(on_launcher_map), NULL);
	g_signal_connect(S.win, "button-press-event",
			 G_CALLBACK(on_launcher_press), NULL);
	g_signal_connect(S.win, "key-press-event", G_CALLBACK(on_launcher_key),
			 NULL);
	g_signal_connect(S.win, "grab-broken-event",
			 G_CALLBACK(on_launcher_grab_broken), NULL);

	S.overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(S.win), S.overlay);

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_container_add(GTK_CONTAINER(S.overlay), body);
	gtk_box_pack_start(GTK_BOX(body), header(), FALSE, FALSE, 0);
	add_class(main, "launcher-main");
	gtk_box_pack_start(GTK_BOX(body), main, TRUE, TRUE, 0);

	S.entry = gtk_entry_new();
	add_class(S.entry, "launcher-search");
	GdkPixbuf *mag = search_pixbuf(16, PANEL_TEXT_DIM);

	gtk_entry_set_icon_from_pixbuf(GTK_ENTRY(S.entry),
				       GTK_ENTRY_ICON_PRIMARY, mag);
	g_object_unref(mag);
	g_signal_connect(S.entry, "changed", G_CALLBACK(on_search_changed),
			 NULL);
	g_signal_connect(S.entry, "activate", G_CALLBACK(on_search_activate),
			 NULL);

	/* The prompt, as a label over the empty field rather than the entry's
	 * placeholder: GTK hides a placeholder while the field has the focus,
	 * and this one always has it -- the prompt stays beside the caret
	 * until the first key. */
	GtkWidget *eo = gtk_overlay_new();

	S.hint = label_new("Search applications and files",
			   "launcher-search-hint", 0);
	gtk_widget_set_halign(S.hint, GTK_ALIGN_START);
	gtk_widget_set_valign(S.hint, GTK_ALIGN_START);
	gtk_container_add(GTK_CONTAINER(eo), S.entry);
	gtk_overlay_add_overlay(GTK_OVERLAY(eo), S.hint);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(eo), S.hint, TRUE);
	gtk_box_pack_start(GTK_BOX(main), eo, FALSE, FALSE, 0);

	S.switcher = switcher();
	gtk_box_pack_start(GTK_BOX(main), S.switcher, FALSE, FALSE, 0);

	S.stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(S.stack),
				      GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_stack_add_named(GTK_STACK(S.stack), page_favorites(), "favorites");
	gtk_stack_add_named(GTK_STACK(S.stack), page_all(), "all");
	gtk_stack_add_named(GTK_STACK(S.stack), page_recent(), "recent");
	gtk_stack_add_named(GTK_STACK(S.stack), page_search(), "search");
	gtk_box_pack_start(GTK_BOX(main), S.stack, TRUE, TRUE, 0);

	gtk_box_pack_end(GTK_BOX(body), footer(), FALSE, FALSE, 0);

	/* Over the panel's left end, POP_GAP clear of the panel and of the
	 * screen's edge. */
	int x = geo.x + POP_GAP;
	int y = oy - POP_GAP - h;

	gtk_widget_set_size_request(S.win, w, h);
	gtk_window_set_default_size(GTK_WINDOW(S.win), w, h);
	gtk_window_move(GTK_WINDOW(S.win), x, y);
	gtk_widget_show_all(S.overlay);
	launcher_show_page(g_pages[0]);
	gtk_widget_realize(S.win);
	gdk_window_move_resize(gtk_widget_get_window(S.win), x, y, w, h);
	popup_shape(S.win, w, h, LN_RADIUS);

	S.up = TRUE;
	gtk_widget_show(S.win);
	gtk_grab_add(S.win);
	(void)search; /* the search box has the focus either way */
	gtk_widget_queue_draw(g_area);
}

static void launcher_toggle(gboolean search)
{
	if (S.up)
		launcher_close();
	else
		launcher_open(search);
}

/* --- the confirmation ------------------------------------------------------- */

/* Signing out, restarting and shutting down each ask first, on a card of the
 * shell's in the middle of the screen: a glyph for what is about to happen,
 * the question, a line saying what it means for the open programs, and
 * Cancel beside the action itself in the accent.  The action has the
 * keyboard focus, so Enter goes ahead and Esc does not.
 *
 * It is an override-redirect popup like the flyouts, so no window can come
 * up over it, and it holds the pointer and the keyboard while it is open: a
 * click elsewhere does nothing, and the panel does not answer until it is
 * gone.  There is only ever one.  Without a compositor the screen behind it
 * stays as it is; the card is shaped to its rounded outline. */
#define CF_W 420
#define CF_RADIUS 14

static struct {
	GtkWidget *win, *cancel, *ok;
	enum confirm_kind kind;
	gboolean up;
	int grab_tries;
	int w, h;
} CF;

static const struct {
	enum glyph g;
	const char *title, *action;
} g_confirm[] = {
	[CONFIRM_SIGN_OUT] = { GL_SIGNOUT, "Sign out of LikeOS?", "Sign out" },
	[CONFIRM_RESTART] = { GL_RESTART, "Restart this computer?", "Restart" },
	[CONFIRM_SHUT_DOWN] = { GL_POWER, "Shut down this computer?",
				"Shut down" },
};

static gboolean confirm_destroy(gpointer w)
{
	gtk_widget_destroy(GTK_WIDGET(w));
	return G_SOURCE_REMOVE;
}

static void confirm_close(void)
{
	if (!CF.up)
		return;
	CF.up = FALSE;
	gtk_grab_remove(CF.win);
	gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
	gtk_widget_hide(CF.win);
	/* From an idle: this runs inside one of its buttons' handlers. */
	g_idle_add(confirm_destroy, CF.win);
	CF.win = CF.cancel = CF.ok = NULL;
	gtk_widget_queue_draw(g_area);
}

static void confirm_accept(void)
{
	enum confirm_kind kind = CF.kind;

	confirm_close();
	switch (kind) {
	case CONFIRM_SIGN_OUT:
		session_logout();
		break;
	case CONFIRM_RESTART:
		power_run(&g_reboot_req);
		break;
	case CONFIRM_SHUT_DOWN:
		power_run(&g_poweroff_req);
		break;
	}
}

static void on_confirm_ok(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	confirm_accept();
}

static void on_confirm_cancel(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	confirm_close();
}

static gboolean confirm_grab(gpointer data)
{
	GdkWindow *w = CF.win ? gtk_widget_get_window(CF.win) : NULL;
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());

	(void)data;
	if (!CF.up || !w)
		return G_SOURCE_REMOVE;
	if (gdk_seat_grab(seat, w, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL,
			  NULL, NULL) == GDK_GRAB_SUCCESS)
		return G_SOURCE_REMOVE;
	/* The launcher's grab can still be on its way out. */
	return ++CF.grab_tries < 25 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean on_confirm_map(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)ev;
	(void)d;
	CF.grab_tries = 0;
	if (confirm_grab(NULL) == G_SOURCE_CONTINUE)
		g_timeout_add(20, confirm_grab, NULL);
	popup_focus_in(w);
	gtk_widget_grab_focus(CF.ok);
	return FALSE;
}

/* Esc cancels; Enter goes ahead -- or, with the focus moved to Cancel,
 * cancels, as a focused button is what Enter presses. */
static gboolean on_confirm_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
	(void)w;
	(void)d;
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		confirm_close();
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_ISO_Enter:
		if (CF.cancel && gtk_widget_has_focus(CF.cancel))
			confirm_close();
		else
			confirm_accept();
		return TRUE;
	case GDK_KEY_Super_L:
	case GDK_KEY_Super_R:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Presses outside the card -- on the panel, or on another program, which
 * the grab reports here -- are swallowed. */
static gboolean on_confirm_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	GdkWindow *top = ev->window ? gdk_window_get_toplevel(ev->window) : NULL;

	(void)d;
	if (top == gtk_widget_get_window(w) && ev->x >= 0 && ev->y >= 0 &&
	    ev->x < CF.w && ev->y < CF.h)
		return FALSE;
	return TRUE;
}

static gboolean on_confirm_grab_broken(GtkWidget *w, GdkEvent *ev, gpointer d)
{
	(void)w;
	(void)d;
	/* Taken by a popup of its own (a button's tooltip does not, but be
	 * safe): take it back a moment later. */
	if (CF.up && ev->grab_broken.grab_window &&
	    gdk_window_get_toplevel(ev->grab_broken.grab_window) !=
		    gtk_widget_get_window(CF.win))
		g_timeout_add(100, confirm_grab, NULL);
	return TRUE;
}

static gboolean on_confirm_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	(void)d;
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
	shell_surface(cr, 0, 0, gtk_widget_get_allocated_width(w),
		      gtk_widget_get_allocated_height(w), CF_RADIUS);
	return FALSE; /* the children draw over it */
}

/* The glyph on a raised disc. */
static gboolean on_confirm_icon_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	int s = gtk_widget_get_allocated_width(w);

	(void)d;
	cairo_new_sub_path(cr);
	cairo_arc(cr, s / 2.0, s / 2.0, s / 2.0, 0, 2 * M_PI);
	panel_color(cr, PANEL_RAISED);
	cairo_fill(cr);
	panel_color(cr, PANEL_ACCENT);
	glyph_draw(cr, g_confirm[CF.kind].g, (s - 20) / 2.0, (s - 20) / 2.0, 20);
	return TRUE;
}

static void confirm_open(enum confirm_kind kind)
{
	GdkRectangle geo = { 0, 0, g_panel_w, 768 };
	GtkRequisition nat;

	if (CF.up)
		return;
	popups_close();

	CF.kind = kind;
	CF.win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_type_hint(GTK_WINDOW(CF.win),
				 GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_title(GTK_WINDOW(CF.win), g_confirm[kind].action);
	gtk_widget_set_app_paintable(CF.win, TRUE);
	if (F.composited) {
		GdkVisual *v = gdk_screen_get_rgba_visual(
			gtk_widget_get_screen(CF.win));

		if (v)
			gtk_widget_set_visual(CF.win, v);
	}
	add_class(CF.win, "likeos-confirm");
	g_signal_connect_after(CF.win, "realize", G_CALLBACK(on_shell_realize),
			       GUINT_TO_POINTER(PANEL_BG));
	gtk_widget_add_events(CF.win, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
	g_signal_connect(CF.win, "draw", G_CALLBACK(on_confirm_draw), NULL);
	g_signal_connect(CF.win, "map-event", G_CALLBACK(on_confirm_map), NULL);
	g_signal_connect(CF.win, "key-press-event", G_CALLBACK(on_confirm_key),
			 NULL);
	g_signal_connect(CF.win, "button-press-event",
			 G_CALLBACK(on_confirm_press), NULL);
	g_signal_connect(CF.win, "grab-broken-event",
			 G_CALLBACK(on_confirm_grab_broken), NULL);

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *icon = gtk_drawing_area_new();
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *line;

	add_class(body, "confirm-body");
	gtk_widget_set_size_request(icon, 44, 44);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	g_signal_connect(icon, "draw", G_CALLBACK(on_confirm_icon_draw), NULL);
	gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text),
			   label_new(g_confirm[kind].title, "confirm-title", 0),
			   FALSE, FALSE, 0);
	line = label_new("Open apps will close, and any unsaved work may be "
			 "lost.", "confirm-text", 0);
	gtk_label_set_line_wrap(GTK_LABEL(line), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(line), 40);
	gtk_box_pack_start(GTK_BOX(text), line, FALSE, FALSE, 0);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(top), text, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(body), top, FALSE, FALSE, 0);

	CF.cancel = gtk_button_new_with_label("Cancel");
	add_class(CF.cancel, "confirm-cancel");
	g_signal_connect(CF.cancel, "clicked", G_CALLBACK(on_confirm_cancel),
			 NULL);
	CF.ok = gtk_button_new_with_label(g_confirm[kind].action);
	add_class(CF.ok, "confirm-primary");
	gtk_widget_set_can_default(CF.ok, TRUE);
	g_signal_connect(CF.ok, "clicked", G_CALLBACK(on_confirm_ok), NULL);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
	add_class(buttons, "confirm-buttons");
	gtk_box_pack_start(GTK_BOX(buttons), CF.cancel, FALSE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(buttons), CF.ok, FALSE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(body), buttons, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(CF.win), body);
	gtk_window_set_default(GTK_WINDOW(CF.win), CF.ok);
	gtk_widget_show_all(body);

	/* Measured once its children are in it and styled. */
	gtk_widget_set_size_request(body, CF_W, -1);
	gtk_widget_get_preferred_size(CF.win, NULL, &nat);
	CF.w = MAX(CF_W, nat.width);
	CF.h = nat.height;

	GdkMonitor *mon = g_gdkwin ? gdk_display_get_monitor_at_window(
					     gdk_display_get_default(), g_gdkwin) :
				     NULL;

	if (mon)
		gdk_monitor_get_geometry(mon, &geo);

	int x = geo.x + (geo.width - CF.w) / 2;
	int y = geo.y + (geo.height - CF.h) / 2;

	gtk_widget_set_size_request(CF.win, CF.w, CF.h);
	gtk_window_move(GTK_WINDOW(CF.win), x, y);
	gtk_widget_realize(CF.win);
	gdk_window_move_resize(gtk_widget_get_window(CF.win), x, y, CF.w, CF.h);
	popup_shape(CF.win, CF.w, CF.h, CF_RADIUS);

	CF.up = TRUE;
	gtk_widget_show(CF.win);
	gtk_grab_add(CF.win);
}

/* --- input ------------------------------------------------------------------- */

/* Which control is at (x, y)?  See struct layout: this and the drawing code
 * are the only two readers of it, so they agree by construction. */
static int hit_test(double x, double y)
{
	struct layout L;

	(void)y;
	layout(NULL, g_panel_w, &L);

	if (x >= L.orb_x && x < L.orb_x + ORB_SLOT)
		return HIT_LAUNCHER;
	if (x >= L.ws_x && x < L.ws_x + WS_W)
		return HIT_WORKSPACES;
	if (L.chev_x >= 0 && x >= L.chev_x && x < L.chev_x + CHEVRON_W)
		return HIT_CHEVRON;
	if (L.load_w > 0 && x >= L.load_x && x < L.load_x + L.load_w)
		return HIT_LOAD;
	if (x >= L.tray_x && x < L.tray_x + TRAY_W)
		return HIT_TRAY;
	if (x >= L.clock_x && x < L.clock_x + L.clock_w)
		return HIT_CLOCK;

	for (int i = 0; i < L.napps; i++) {
		struct button *b = g_ptr_array_index(g_buttons, i);

		if (x >= b->x && x < b->x + APP_W)
			return i;
	}
	return HIT_NONE;
}

/* A left click on a control, released where it was pressed. */
static void bar_activate(int hit)
{
	switch (hit) {
	case HIT_LAUNCHER:
		launcher_toggle(FALSE);
		return;
	case HIT_WORKSPACES:
		if (P.up && P.owner == HIT_WORKSPACES)
			pop_close();
		else
			tv_open();
		return;
	case HIT_CHEVRON:
		if (P.up && P.owner == HIT_CHEVRON)
			pop_close();
		else
			pop_open(&g_ov_ops, HIT_CHEVRON, POP_OVER_OWNER);
		return;
	case HIT_LOAD:
		popups_close();
		spawn_command("xterm -e top");
		return;
	case HIT_TRAY:
		/* Normally the flyout's grab takes this click and closes it
		 * before the panel sees it; this is for when the grab could
		 * not be had. */
		if (F.up) {
			fly_close();
		} else {
			popups_close();
			fly_open();
		}
		return;
	case HIT_CLOCK:
		if (P.up && P.owner == HIT_CLOCK)
			pop_close();
		else
			cal_open();
		return;
	default:
		break;
	}
	if (hit < 0 || (guint)hit >= g_buttons->len)
		return;

	struct button *b = g_ptr_array_index(g_buttons, hit);

	popups_close();
	if (b->wins->len == 0) {
		app_launch(b->app);
	} else if (b->wins->len > 1) {
		show_window_list(hit);
	} else {
		struct task *t = button_task(b, 0);

		/* The classic behaviour, and the one people expect: clicking
		 * the button of the window you are already in minimises it,
		 * clicking any other brings it up. */
		if (t->win == g_active && !t->iconic)
			task_minimise(t->win);
		else
			task_activate(t->win);
	}
}

/* A press that closed a flyout or the launcher, at root position (x, y).
 * The click is spent closing it -- except on another control of the panel,
 * which opens at once: clicking the clock with the Control Center up goes
 * straight to the calendar.  The control the flyout belongs to only closes
 * it. */
static void bar_click_through(double x_root, double y_root, int owner)
{
	int ox = 0, oy = 0;

	if (!g_gdkwin)
		return;
	gdk_window_get_origin(g_gdkwin, &ox, &oy);

	double x = x_root - ox, y = y_root - oy;

	if (x < 0 || x >= g_panel_w || y < 0 || y >= PANEL_HEIGHT)
		return;

	int hit = hit_test(x, y);

	if (hit == owner || hit == HIT_NONE)
		return;
	bar_activate(hit);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;

	int hit = hit_test(ev->x, ev->y);

	/* A press puts the tooltip away: whatever the click opens would
	 * otherwise appear under it. */
	gtk_widget_set_tooltip_text(g_area, NULL);
	if (ev->button == 1) {
		g_pressed = hit;
		gtk_widget_queue_draw(g_area);
	} else if (ev->button == 2 && hit == HIT_TRAY) {
		/* A middle click on the tray mutes, without opening
		 * anything. */
		vol_toggle_mute();
	} else if (ev->button == 2 && hit >= 0 &&
		   (guint)hit < g_buttons->len) {
		/* ... and on an application, opens another window of it. */
		app_launch(((struct button *)g_ptr_array_index(g_buttons, hit))
				   ->app);
	} else if (ev->button == 3 && hit >= 0 &&
		   (guint)hit < g_buttons->len) {
		popups_close();
		show_jump_list(hit);
	} else if (ev->button == 3 && hit == HIT_NONE) {
		/* The panel's own menu, from its empty stretch. */
		popups_close();
		show_panel_menu(ev->x);
	}
	return TRUE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)d;
	int hit = hit_test(ev->x, ev->y);
	int was = g_pressed;

	g_pressed = HIT_NONE;
	gtk_widget_queue_draw(g_area);

	if (ev->button != 1 || hit != was || hit == HIT_NONE)
		return TRUE;
	bar_activate(hit);
	return TRUE;
}

/* The tooltip for a control: the window's title on an application's button,
 * which the icon-only button cannot show; the state of the network, the
 * volume or the machine over their icons. */
static void bar_tooltip(int hit)
{
	char *tip = NULL;

	if (hit >= 0 && (guint)hit < g_buttons->len)
		tip = g_strdup(button_title(g_ptr_array_index(g_buttons, hit)));
	else if (hit == HIT_TRAY)
		tip = tray_tooltip();
	else if (hit == HIT_LOAD)
		tip = load_tooltip();
	else if (hit == HIT_LAUNCHER)
		tip = g_strdup("Launcher");
	else if (hit == HIT_WORKSPACES)
		tip = g_strdup_printf("Workspaces \xe2\x80\x94 workspace %ld of %ld",
				      g_desktop + 1,
				      g_ndesktops < 1 ? 1 : g_ndesktops);
	else if (hit == HIT_CHEVRON)
		tip = g_strdup("More");
	else if (hit == HIT_CLOCK) {
		GDateTime *now = g_date_time_new_now_local();

		tip = g_date_time_format(now, "%A, %-d %B %Y");
		g_date_time_unref(now);
	}
	gtk_widget_set_tooltip_text(g_area, tip);
	g_free(tip);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
	(void)w;
	(void)d;
	int hit = hit_test(ev->x, ev->y);

	if (hit != g_hover) {
		g_hover = hit;
		/* The tile under the pointer lights up. */
		gtk_widget_queue_draw(g_area);
		bar_tooltip(hit);
	}
	return TRUE;
}

/* The wheel over the tray changes the volume, two steps a notch, and the
 * volume sound plays once the wheel has stopped. */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (hit_test(ev->x, ev->y) != HIT_TRAY || !V.online)
		return TRUE;
	if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_RIGHT)
		vol_set_user(V.vol + 2);
	else if (ev->direction == GDK_SCROLL_DOWN ||
		 ev->direction == GDK_SCROLL_LEFT)
		vol_set_user(V.vol - 2);
	else
		return TRUE;
	ding_later(150);
	return TRUE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer d)
{
	(void)w;
	(void)ev;
	(void)d;
	g_hover = HIT_NONE;
	g_pressed = HIT_NONE;
	gtk_widget_queue_draw(g_area);
	return TRUE;
}

/* --- snapping with the keyboard ------------------------------------------------- */

/* Super with the arrow keys, on the focused window:
 *
 *   Super+Up      maximise;
 *   Super+Down    restore a maximised window, put a snapped one back where
 *                 it was, and minimise any other;
 *   Super+Left    snap to the left half of the work area -- or, on a window
 *   Super+Right   snapped to the other half, put it back where it was.
 *
 * Maximising is EWMH's _NET_WM_STATE, which the window manager undoes to the
 * window's own earlier geometry.  A snap is a plain move and resize, so the
 * geometry before it is remembered here, per window, for the way back.  The
 * move is _NET_MOVERESIZE_WINDOW when the window manager lists it in
 * _NET_SUPPORTED, and otherwise an ordinary configure request, which every
 * window manager honours -- ctwm among them, which does not implement the
 * message.  Positions here are the frame's top-left corner, sizes the
 * client's, with the frame measured by _NET_FRAME_EXTENTS. */
static gboolean monitor_geometry(GdkRectangle *geo); /* with the layout */

struct snap {
	int side;	/* -1 left, 1 right */
	int x, y, w, h; /* the frame's position and the client's size before */
};

static GHashTable *g_snaps; /* Window -> struct snap */

static gboolean win_maximised(Window w)
{
	return prop_has_atom(w, A.net_wm_state, A.max_vert) &&
	       prop_has_atom(w, A.net_wm_state, A.max_horz);
}

static void win_set_maximised(Window w, gboolean on)
{
	send_root_message(w, A.net_wm_state, on ? 1 : 0, (long)A.max_vert,
			  (long)A.max_horz);
}

/* The frame's left, right, top and bottom widths. */
static void win_extents(Window w, long ext[4])
{
	unsigned long n = 0;
	unsigned char *d = prop_get(w, A.frame_extents, XA_CARDINAL, NULL, &n);

	ext[0] = ext[1] = ext[2] = ext[3] = 0;
	if (d) {
		for (unsigned long i = 0; i < n && i < 4; i++)
			ext[i] = ((long *)d)[i];
		XFree(d);
	}
}

/* Where the window is now: its frame's top-left corner and its own size. */
static gboolean win_geometry(Window w, int *x, int *y, int *cw, int *ch)
{
	XWindowAttributes wa;
	Window child;
	int rx = 0, ry = 0;
	long ext[4];
	GdkDisplay *disp = gdk_display_get_default();

	gdk_x11_display_error_trap_push(disp);
	Status ok = XGetWindowAttributes(g_dpy, w, &wa) &&
		    XTranslateCoordinates(g_dpy, w, g_root, 0, 0, &rx, &ry,
					  &child);
	gdk_x11_display_error_trap_pop_ignored(disp);
	if (!ok)
		return FALSE;
	win_extents(w, ext);
	*x = rx - (int)ext[0];
	*y = ry - (int)ext[2];
	*cw = wa.width;
	*ch = wa.height;
	return TRUE;
}

static void win_move_resize(Window w, int x, int y, int cw, int ch)
{
	XClientMessageEvent ev;
	GdkDisplay *disp = gdk_display_get_default();

	if (cw < 1 || ch < 1)
		return;
	if (!prop_has_atom(g_root, A.net_supported, A.moveresize_window)) {
		/* A configure request names where the window's own border
		 * would be without a window manager; the manager puts its
		 * frame around that, so the frame's outer edge lands one
		 * frame border further out.  The left extent is that
		 * border's width (the top one has the title bar in it). */
		long ext[4];

		win_extents(w, ext);
		gdk_x11_display_error_trap_push(disp);
		XMoveResizeWindow(g_dpy, w, x + (int)ext[0], y + (int)ext[0],
				  (unsigned)cw, (unsigned)ch);
		XFlush(g_dpy);
		gdk_x11_display_error_trap_pop_ignored(disp);
		return;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.window = w;
	ev.message_type = A.moveresize_window;
	ev.format = 32;
	/* North-west gravity, x, y, width and height given, from a pager. */
	ev.data.l[0] = NorthWestGravity | (0xf << 8) | (2 << 12);
	ev.data.l[1] = x;
	ev.data.l[2] = y;
	ev.data.l[3] = cw;
	ev.data.l[4] = ch;

	gdk_x11_display_error_trap_push(disp);
	XSendEvent(g_dpy, g_root, False,
		   SubstructureNotifyMask | SubstructureRedirectMask,
		   (XEvent *)&ev);
	XFlush(g_dpy);
	gdk_x11_display_error_trap_pop_ignored(disp);
}

/* The current desktop's work area: the screen less the bar. */
static void work_area(GdkRectangle *r)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.workarea, XA_CARDINAL, NULL, &n);
	unsigned long k = (unsigned long)(g_desktop < 0 ? 0 : g_desktop) * 4;

	if (d && n >= 4) {
		long *v = (long *)d;

		if (k + 4 > n)
			k = 0;
		*r = (GdkRectangle){ (int)v[k], (int)v[k + 1], (int)v[k + 2],
				     (int)v[k + 3] };
	} else {
		monitor_geometry(r);
		r->height -= PANEL_HEIGHT;
	}
	if (d)
		XFree(d);
}

static void snap_put_back(Window w, struct snap *sn)
{
	win_move_resize(w, sn->x, sn->y, sn->w, sn->h);
	g_hash_table_remove(g_snaps, GSIZE_TO_POINTER(w));
}

/* Snap `w' to half `side' of the work area, remembering where it was unless
 * it was already snapped (to the other half, it keeps the first place). */
static void snap_to(Window w, int side)
{
	struct snap *sn = g_hash_table_lookup(g_snaps, GSIZE_TO_POINTER(w));
	GdkRectangle wa;
	long ext[4];

	if (!sn) {
		sn = g_new0(struct snap, 1);
		if (!win_geometry(w, &sn->x, &sn->y, &sn->w, &sn->h)) {
			g_free(sn);
			return;
		}
		g_hash_table_insert(g_snaps, GSIZE_TO_POINTER(w), sn);
	}
	sn->side = side;
	work_area(&wa);
	win_extents(w, ext);

	int half = wa.width / 2;
	int fx = side < 0 ? wa.x : wa.x + half;
	int fw = side < 0 ? half : wa.width - half;

	win_move_resize(w, fx, wa.y, fw - (int)(ext[0] + ext[1]),
			wa.height - (int)(ext[2] + ext[3]));
}

/* A maximised window is restored first, and snapped once the window manager
 * has put it back: snapping it at once would race the restore, and what
 * would be remembered as its place is the maximised geometry. */
struct snap_later {
	Window w;
	int side;
};

static gboolean snap_later_cb(gpointer data)
{
	struct snap_later *sl = data;

	snap_to(sl->w, sl->side);
	g_free(sl);
	return G_SOURCE_REMOVE;
}

static void snap_key(KeySym sym)
{
	Window w = g_active;

	if (w == None || w == g_root ||
	    prop_has_atom(w, A.window_type, A.type_dock) ||
	    prop_has_atom(w, A.window_type, A.type_desktop))
		return;
	if (!g_snaps)
		g_snaps = g_hash_table_new_full(NULL, NULL, NULL, g_free);

	struct snap *sn = g_hash_table_lookup(g_snaps, GSIZE_TO_POINTER(w));
	gboolean maxed = win_maximised(w);

	switch (sym) {
	case XK_Up:
		if (!maxed)
			win_set_maximised(w, TRUE);
		break;
	case XK_Down:
		if (maxed)
			win_set_maximised(w, FALSE);
		else if (sn)
			snap_put_back(w, sn);
		else
			task_minimise(w);
		break;
	case XK_Left:
	case XK_Right: {
		int side = sym == XK_Left ? -1 : 1;

		if (maxed) {
			struct snap_later *sl = g_new(struct snap_later, 1);

			sl->w = w;
			sl->side = side;
			win_set_maximised(w, FALSE);
			g_timeout_add(150, snap_later_cb, sl);
		} else if (sn && sn->side == -side) {
			snap_put_back(w, sn);
		} else if (!sn || sn->side != side) {
			snap_to(w, side);
		}
		break;
	}
	default:
		break;
	}
}

/* Forget the windows that have gone: an XID is reused, and a new window must
 * not inherit an old one's place. */
static void snaps_prune(void)
{
	unsigned long n = 0;
	unsigned char *d;
	GHashTableIter it;
	gpointer key;

	if (!g_snaps || g_hash_table_size(g_snaps) == 0)
		return;
	d = prop_get(g_root, A.client_list, XA_WINDOW, NULL, &n);
	g_hash_table_iter_init(&it, g_snaps);
	while (g_hash_table_iter_next(&it, &key, NULL)) {
		gboolean alive = FALSE;

		for (unsigned long i = 0; d && i < n && !alive; i++)
			alive = ((Window *)d)[i] == (Window)GPOINTER_TO_SIZE(key);
		if (!alive)
			g_hash_table_iter_remove(&it);
	}
	if (d)
		XFree(d);
}

/* --- X event plumbing ---------------------------------------------------------- */

/* Root and per-window PropertyNotify.
 *
 * Everything the panel shows is a property, so one filter covers the lot: the
 * window list, the focus, the workspace, and each window's title and iconic
 * state.  GDK hands us the raw event before it decides it has no use for it,
 * which is what makes this possible without an X connection of our own. */
static void display_switch(int state); /* below, with the panel's layout */

/* The Super key.
 *
 * Grabbed on the root window, so it reaches this program whatever has the
 * focus.  Pressed and released on its own, it opens and closes the
 * launcher; with another key in between it was the start of a combination,
 * and the launcher stays shut.  The shell's own combinations are answered
 * here too: Super+E opens LikeOS Explorer, Super+D shows the desktop,
 * Super+R and Super+S the launcher's search, Super+Tab the Workspaces strip,
 * and Super with the arrows maximises, restores, minimises and snaps
 * (snap_key()).
 *
 * Grabbed with every combination of the lock modifiers, since a grab is for
 * an exact modifier state and Caps Lock or Num Lock being on would otherwise
 * make it miss. */
static KeyCode g_super_l, g_super_r;
static gboolean g_super_alone;

static void super_grab(void)
{
	GdkDisplay *disp = gdk_display_get_default();
	unsigned int locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

	g_super_l = XKeysymToKeycode(g_dpy, XK_Super_L);
	g_super_r = XKeysymToKeycode(g_dpy, XK_Super_R);
	gdk_x11_display_error_trap_push(disp);
	for (guint i = 0; i < G_N_ELEMENTS(locks); i++) {
		if (g_super_l)
			XGrabKey(g_dpy, g_super_l, locks[i], g_root, False,
				 GrabModeAsync, GrabModeAsync);
		if (g_super_r)
			XGrabKey(g_dpy, g_super_r, locks[i], g_root, False,
				 GrabModeAsync, GrabModeAsync);
	}
	gdk_x11_display_error_trap_pop_ignored(disp);
}

static gboolean super_key(XKeyEvent *k, gboolean press)
{
	gboolean super = k->keycode && (k->keycode == g_super_l ||
					k->keycode == g_super_r);

	if (press) {
		if (super) {
			g_super_alone = TRUE;
			return TRUE;
		}
		if (!g_super_alone && !(k->state & Mod4Mask))
			return FALSE;
		g_super_alone = FALSE;

		KeySym sym = XLookupKeysym(k, 0);

		if (sym == XK_e) {
			popups_close();
			spawn_command("likeos-explorer");
		} else if (sym == XK_d) {
			popups_close();
			toggle_show_desktop();
		} else if (sym == XK_r || sym == XK_s) {
			if (!launcher_is_up())
				launcher_toggle(TRUE);
		} else if (sym == XK_Tab) {
			tv_open();
		} else if (sym == XK_Up || sym == XK_Down || sym == XK_Left ||
			   sym == XK_Right) {
			popups_close();
			snap_key(sym);
		}
		return TRUE;
	}
	if (super && g_super_alone) {
		g_super_alone = FALSE;
		/* An open flyout has the keyboard and handles the key
		 * itself; this is the launcher's toggle from anywhere
		 * else. */
		launcher_toggle(FALSE);
		return TRUE;
	}
	return super;
}

static GdkFilterReturn event_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
	XEvent *e = (XEvent *)xev;

	(void)ev;
	(void)data;

	if ((e->type == KeyPress || e->type == KeyRelease) &&
	    e->xkey.window == g_root)
		return super_key(&e->xkey, e->type == KeyPress) ?
			       GDK_FILTER_REMOVE :
			       GDK_FILTER_CONTINUE;

	if (e->type != PropertyNotify)
		return GDK_FILTER_CONTINUE;

	Atom a = e->xproperty.atom;

	if (e->xproperty.window == g_root) {
		if (a == A.client_list) {
			tasks_refresh();
			snaps_prune();
		} else if (a == A.active_window) {
			g_active = prop_window(g_root, A.active_window);
			gtk_widget_queue_draw(g_area);
		} else if (a == A.current_desktop) {
			g_desktop = prop_card(g_root, A.current_desktop, 0);
			/* The task list is per-desktop, so a workspace switch
			 * changes it as surely as opening a window does. */
			tasks_refresh();
		} else if (a == A.number_of_desktops) {
			g_ndesktops = prop_card(g_root,
						A.number_of_desktops, 4);
			gtk_widget_queue_draw(g_area);
		} else if (a == A.display_switch) {
			display_switch(e->xproperty.state);
		} else if (a == A.rootpmap) {
			/* A new wallpaper: the tint behind the panel changes
			 * with it. */
			tint_update();
			panel_x_background(g_win, g_layout_geo.x,
					   g_layout_geo.y + g_layout_geo.height -
						   PANEL_HEIGHT,
					   g_panel_w, PANEL_HEIGHT);
			gtk_widget_queue_draw(g_area);
		} else if (a == A.showing_desktop) {
			g_showing_desktop =
				prop_card(g_root, A.showing_desktop, 0) != 0;
			gtk_widget_queue_draw(g_area);
		}
		return GDK_FILTER_CONTINUE;
	}

	/* A client window: a title change, or a minimise/restore. */
	if (a == A.wm_name || a == XA_WM_NAME || a == A.wm_state ||
	    a == A.wm_desktop || a == A.net_wm_state) {
		tasks_refresh();
	}
	return GDK_FILTER_CONTINUE;
}

static gboolean on_tick(gpointer data)
{
	(void)data;
	panel_load_sample(&g_load);
	gtk_widget_queue_draw(g_area);
	return G_SOURCE_CONTINUE;
}

/* A slow reconcile against the server.
 *
 * Everything here is event-driven and should not need this.  It is insurance
 * against the one case events cannot cover: a window that is destroyed between
 * appearing in the client list and having PropertyChangeMask selected on it
 * leaves no event behind, and its button would sit there until something else
 * happened.  Three seconds is slow enough to cost nothing and fast enough that
 * nobody notices the gap. */
static gboolean on_reconcile(gpointer data)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.client_list, XA_WINDOW, NULL, &n);
	gboolean changed = FALSE;

	(void)data;

	/* Compare the list before rebuilding.  A full refresh re-reads four
	 * properties per window and decodes every icon, which is not something
	 * to do three times a second for no reason -- and the redraw it
	 * queues would repaint the panel each time as well. */
	if (!d) {
		changed = (g_tasks->len != 0);
	} else {
		Window *w = (Window *)d;
		guint seen = 0;

		for (unsigned long i = 0; i < n; i++) {
			long desk;

			if (!want_task(w[i], &desk))
				continue;
			if (seen >= g_tasks->len ||
			    g_array_index(g_tasks, struct task, seen).win !=
				    w[i]) {
				changed = TRUE;
				break;
			}
			seen++;
		}
		if (!changed && seen != g_tasks->len)
			changed = TRUE;
		XFree(d);
	}

	if (changed)
		tasks_refresh();
	return G_SOURCE_CONTINUE;
}

/* --- the start-up sound ------------------------------------------------------------ */

/* Played once the desktop is really there, not when X starts: at that point
 * the screen is still black, and the chime used to finish before anything
 * appeared.  "There" is three things -- this bar drawn, a window manager
 * managing the screen, and the desktop window (pcmanfm's icons) mapped --
 * checked five times a second, which costs a property read or two.  If one of
 * them never comes (no pcmanfm, another window manager) it plays after 15
 * seconds anyway.
 *
 * Once per X session, not once per taskbar: the decision is marked with a
 * property on the root window, which lives exactly as long as the session,
 * so a taskbar that crashed and was restarted -- or a second one -- stays
 * quiet. */
#define STARTUP_POLL_MS 200
#define STARTUP_GIVE_UP_US (15 * G_USEC_PER_SEC)

static gint64 g_startup_since;
static guint g_startup_polls;

/* EWMH's test for a running window manager: the root names a window, and that
 * window names itself.  A stale property left by a window manager that died
 * names a window that is gone, or one that does not point back. */
static gboolean startup_wm_up(void)
{
	Window w = prop_window(g_root, A.supporting_wm_check);

	return w != None && prop_window(w, A.supporting_wm_check) == w;
}

static gboolean win_viewable(Window w)
{
	XWindowAttributes wa;
	Status ok;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	ok = XGetWindowAttributes(g_dpy, w, &wa);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	return ok && wa.map_state == IsViewable;
}

static gboolean is_desktop_win(Window w)
{
	return prop_has_atom(w, A.window_type, A.type_desktop) &&
	       win_viewable(w);
}

/* A desktop window, mapped.  The window manager's client list is the cheap
 * place to look; once a second the tree under the root is searched as well
 * (two levels: the frames, and the clients inside them), for a desktop
 * window the window manager does not list. */
static gboolean startup_desktop_up(gboolean deep)
{
	unsigned long n = 0;
	unsigned char *d = prop_get(g_root, A.client_list, XA_WINDOW, NULL, &n);
	gboolean found = FALSE;

	if (d) {
		Window *w = (Window *)d;

		for (unsigned long i = 0; i < n && !found; i++)
			found = is_desktop_win(w[i]);
		XFree(d);
	}
	if (found || !deep)
		return found;

	Window root_ret, parent, *kids = NULL;
	unsigned int nkids = 0;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	if (XQueryTree(g_dpy, g_root, &root_ret, &parent, &kids, &nkids)) {
		for (unsigned int i = 0; i < nkids && !found; i++) {
			Window *sub = NULL;
			unsigned int nsub = 0;

			if (!win_viewable(kids[i]))
				continue;
			found = is_desktop_win(kids[i]);
			if (!found && XQueryTree(g_dpy, kids[i], &root_ret,
						 &parent, &sub, &nsub)) {
				for (unsigned int j = 0; j < nsub && !found; j++)
					found = is_desktop_win(sub[j]);
			}
			if (sub)
				XFree(sub);
		}
		if (kids)
			XFree(kids);
	}
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
	return found;
}

static gboolean startup_poll(gpointer data)
{
	(void)data;
	g_startup_polls++;

	gboolean late = g_get_monotonic_time() - g_startup_since >=
			STARTUP_GIVE_UP_US;

	if (!late && !(g_bar_drawn && startup_wm_up() &&
		       startup_desktop_up(g_startup_polls % 5 == 0)))
		return G_SOURCE_CONTINUE;

	/* Marked before it is played: played twice is the failure this
	 * guards against, not played at all is merely quiet. */
	long one = 1;

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	XChangeProperty(g_dpy, g_root, A.startup_sound, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)&one, 1);
	XFlush(g_dpy);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());

	/* audiod normally runs long before X does; ten seconds is for the
	 * boot where it is still coming up. */
	audio_play("startup", 10000);
	return G_SOURCE_REMOVE;
}

static void startup_sound_arm(void)
{
	if (prop_card(g_root, A.startup_sound, 0) != 0)
		return;
	g_startup_since = g_get_monotonic_time();
	g_timeout_add(STARTUP_POLL_MS, startup_poll, NULL);
}

/* Reserve the strip so maximised windows and the desktop icons stay clear of
 * it.  ctwm reads _NET_WM_STRUT_PARTIAL and narrows _NET_WORKAREA, which is
 * what every well-behaved client consults before placing itself.
 *
 * The BOTTOM edge, since that is where the panel is.  The twelve cardinals are
 * left, right, top, bottom, then the start/end pairs in that same order, so
 * bottom is index 3 and its span is 10 and 11. */
static void set_strut(Window w, int height, int x, int width)
{
	long strut[12] = { 0 };

	strut[3] = height;	   /* bottom */
	strut[10] = x;		   /* bottom_start_x */
	strut[11] = x + width - 1; /* bottom_end_x */

	gdk_x11_display_error_trap_push(gdk_display_get_default());
	XChangeProperty(g_dpy, w, A.strut_partial, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)strut, 12);
	gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
}

static void intern_atoms(void)
{
#define IA(f, n) A.f = XInternAtom(g_dpy, n, False)
	IA(client_list, "_NET_CLIENT_LIST");
	IA(active_window, "_NET_ACTIVE_WINDOW");
	IA(current_desktop, "_NET_CURRENT_DESKTOP");
	IA(number_of_desktops, "_NET_NUMBER_OF_DESKTOPS");
	IA(wm_desktop, "_NET_WM_DESKTOP");
	IA(wm_name, "_NET_WM_NAME");
	IA(wm_visible_name, "_NET_WM_VISIBLE_NAME");
	IA(wm_state, "WM_STATE");
	IA(wm_change_state, "WM_CHANGE_STATE");
	IA(net_wm_state, "_NET_WM_STATE");
	IA(skip_taskbar, "_NET_WM_STATE_SKIP_TASKBAR");
	IA(close_window, "_NET_CLOSE_WINDOW");
	IA(window_type, "_NET_WM_WINDOW_TYPE");
	IA(type_dock, "_NET_WM_WINDOW_TYPE_DOCK");
	IA(type_desktop, "_NET_WM_WINDOW_TYPE_DESKTOP");
	IA(type_toolbar, "_NET_WM_WINDOW_TYPE_TOOLBAR");
	IA(type_splash, "_NET_WM_WINDOW_TYPE_SPLASH");
	IA(utf8, "UTF8_STRING");
	IA(strut_partial, "_NET_WM_STRUT_PARTIAL");
	IA(wm_icon, "_NET_WM_ICON");
	IA(supporting_wm_check, "_NET_SUPPORTING_WM_CHECK");
	IA(startup_sound, "_LIKEOS_STARTUP_SOUND");
	IA(display_switch, "_LIKEOS_DISPLAY_SWITCH");
	IA(showing_desktop, "_NET_SHOWING_DESKTOP");
	IA(rootpmap, "_XROOTPMAP_ID");
	IA(desktop_names, "_NET_DESKTOP_NAMES");
	IA(max_vert, "_NET_WM_STATE_MAXIMIZED_VERT");
	IA(max_horz, "_NET_WM_STATE_MAXIMIZED_HORZ");
	IA(frame_extents, "_NET_FRAME_EXTENTS");
	IA(workarea, "_NET_WORKAREA");
	IA(moveresize_window, "_NET_MOVERESIZE_WINDOW");
	IA(net_supported, "_NET_SUPPORTED");
#undef IA
}

/* The monitor the panel sits on: the primary one, else the first. */
static gboolean monitor_geometry(GdkRectangle *geo)
{
	GdkDisplay *disp = gdk_display_get_default();
	GdkMonitor *mon = gdk_display_get_primary_monitor(disp);

	if (!mon)
		mon = gdk_display_get_monitor(disp, 0);
	if (!mon)
		return FALSE;
	gdk_monitor_get_geometry(mon, geo);
	return TRUE;
}

/* Lay the panel out for `geo': as wide as the monitor, at its bottom edge,
 * with the strut that keeps maximised windows clear of it.  The position is
 * stated the way main() explains, as a user position. */
static void panel_place(const GdkRectangle *geo)
{
	char geom[32];

	g_layout_geo = *geo;
	g_panel_w = geo->width;
	gtk_widget_set_size_request(g_area, geo->width, PANEL_HEIGHT);
	gtk_window_resize(GTK_WINDOW(g_win), geo->width, PANEL_HEIGHT);
	g_snprintf(geom, sizeof(geom), "+%d+%d", geo->x,
		   geo->y + geo->height - PANEL_HEIGHT);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_window_parse_geometry(GTK_WINDOW(g_win), geom);
	G_GNUC_END_IGNORE_DEPRECATIONS
	if (g_gdkwin)
		set_strut(GDK_WINDOW_XID(g_gdkwin), PANEL_HEIGHT, geo->x,
			  geo->width);
	panel_x_background(g_win, geo->x, geo->y + geo->height - PANEL_HEIGHT,
			   geo->width, PANEL_HEIGHT);
	popups_close();
	gtk_widget_queue_draw(g_area);
}

/* Display settings changing the screen size.
 *
 * Followed as it happens, the change would show: the panel laid out for the
 * old size on the new screen until it has caught up.  So the dialog asks
 * first, through _LIKEOS_DISPLAY_SWITCH on the root window: 1 means "about to
 * change the size", and the panel takes itself off the screen and answers 2
 * once it is off; the dialog changes the size and deletes the property, and
 * the panel lays itself out for the new size and comes back.  A dialog that
 * never deletes it does not keep the panel away: it comes back after
 * SWITCH_HIDE_MS regardless. */
#define SWITCH_HIDE_MS 10000

static gboolean g_switch_hidden;
static guint g_switch_timeout;

static gboolean display_switch_done(gpointer data)
{
	GdkRectangle geo;

	(void)data;
	if (g_switch_timeout) {
		g_source_remove(g_switch_timeout);
		g_switch_timeout = 0;
	}
	if (!g_switch_hidden)
		return G_SOURCE_REMOVE;
	g_switch_hidden = FALSE;
	if (monitor_geometry(&geo))
		panel_place(&geo);
	gtk_widget_show(g_win);
	return G_SOURCE_REMOVE;
}

static gboolean display_switch_expired(gpointer data)
{
	g_switch_timeout = 0;
	return display_switch_done(data);
}

static void display_switch(int state)
{
	if (state == PropertyDelete) {
		display_switch_done(NULL);
		return;
	}
	if (prop_card(g_root, A.display_switch, 0) != 1)
		return; /* our own answer, or nothing we know */
	if (!g_switch_hidden) {
		g_switch_hidden = TRUE;
		gtk_widget_hide(g_win);
	}
	/* Off the screen before the answer goes out. */
	gdk_display_sync(gdk_display_get_default());

	long answer = 2;

	XChangeProperty(g_dpy, g_root, A.display_switch, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)&answer, 1);
	XFlush(g_dpy);
	if (g_switch_timeout)
		g_source_remove(g_switch_timeout);
	g_switch_timeout =
		g_timeout_add(SWITCH_HIDE_MS, display_switch_expired, NULL);
}

/* A screen size change made by some other program (xrandr): follow it. */
static void on_monitors_changed(GdkScreen *screen, gpointer data)
{
	GdkRectangle geo;

	(void)screen;
	(void)data;
	/* Hidden for Display settings: laid out when it comes back. */
	if (g_switch_hidden || !monitor_geometry(&geo) ||
	    gdk_rectangle_equal(&geo, &g_layout_geo))
		return;
	panel_place(&geo);
}

static void on_icon_theme_changed(GtkIconTheme *t, gpointer d)
{
	(void)t;
	(void)d;
	if (g_icon_cache)
		g_hash_table_remove_all(g_icon_cache);
	orb_forget();
	buttons_rebuild();
	gtk_widget_queue_draw(g_area);
}

/* The launcher's look lives in the LikeOS theme (_shell.scss).  Under any
 * other theme these few rules keep it usable and close to the intended look:
 * light text and flat buttons on its dark surface, the field, the switch,
 * the footer and the cards -- and the same for the confirmation card. */
static const char g_fallback_css[] =
	".likeos-launcher { color: #eef2fa; }\n"
	".likeos-launcher label { color: #eef2fa; }\n"
	".likeos-launcher button { background: none; border: 1px solid transparent;"
	" box-shadow: none; border-radius: 6px; color: #eef2fa; }\n"
	".likeos-launcher button:hover { background-color: rgba(255,255,255,0.07); }\n"
	".likeos-launcher button:checked { background-color: rgba(255,255,255,0.12); }\n"
	".likeos-launcher .launcher-header { padding: 0 12px 0 14px; }\n"
	".likeos-launcher .launcher-main { padding: 0 20px; }\n"
	".likeos-launcher entry.launcher-search { border-radius: 8px; min-height: 36px;"
	" background: #1f2636; color: #eef2fa; border: 1px solid #2c3446;"
	" margin-bottom: 12px; }\n"
	".likeos-launcher .launcher-search-hint { color: #a4adbf;"
	" margin: 10px 0 0 40px; }\n"
	".likeos-launcher .launcher-switch { background: #1f2636; border-radius: 8px;"
	" padding: 2px; margin-bottom: 10px; }\n"
	".likeos-launcher .launcher-item-sub { color: #a4adbf; }\n"
	".likeos-launcher .launcher-footer { background-color: rgba(255,255,255,0.03);"
	" border-top: 1px solid #2c3446; padding: 0 12px;"
	" border-radius: 0 0 14px 14px; }\n"
	".likeos-launcher .launcher-card { background: #1f2636; border-radius: 10px;"
	" border: 1px solid #2c3446; padding: 4px; }\n"
	".likeos-shell-menu, .likeos-shell-menu menu { background: #1f2636;"
	" color: #eef2fa; }\n"
	".likeos-confirm, .likeos-confirm label { color: #eef2fa; }\n"
	".likeos-confirm .confirm-body { padding: 24px 24px 20px 24px; }\n"
	".likeos-confirm .confirm-title { font-weight: 600; font-size: 10.5pt; }\n"
	".likeos-confirm .confirm-text { color: #a4adbf; }\n"
	".likeos-confirm .confirm-buttons { margin-top: 24px; }\n"
	".likeos-confirm button { min-height: 30px; min-width: 96px;"
	" border-radius: 6px; box-shadow: none; background: #1f2636;"
	" border: 1px solid #2c3446; color: #eef2fa; }\n"
	".likeos-confirm button.confirm-primary { background: #3f74f2;"
	" border-color: #3f74f2; }\n"
	".likeos-confirm button.confirm-primary label { color: #ffffff; }\n";

static void load_fallback_css(void)
{
	char *theme = NULL;

	g_object_get(gtk_settings_get_default(), "gtk-theme-name", &theme, NULL);
	if (g_strcmp0(theme, "LikeOS") != 0 &&
	    g_strcmp0(g_getenv("GTK_THEME"), "LikeOS") != 0) {
		GtkCssProvider *p = gtk_css_provider_new();

		gtk_css_provider_load_from_data(p, g_fallback_css, -1, NULL);
		gtk_style_context_add_provider_for_screen(
			gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		g_object_unref(p);
	}
	g_free(theme);
}

int main(int argc, char **argv)
{
	g_set_prgname("taskbar");
	gdk_set_program_class("taskbar");

	gtk_init(&argc, &argv);

	GdkDisplay *disp = gdk_display_get_default();

	g_dpy = gdk_x11_display_get_xdisplay(disp);
	g_root = gdk_x11_get_default_root_xwindow();
	intern_atoms();
	F.composited = gdk_screen_is_composited(gdk_screen_get_default());

	g_tasks = g_array_new(FALSE, TRUE, sizeof(struct task));
	apps_load();
	g_pin_bar = pins_read(PIN_TASKBAR, g_default_bar_pins);
	g_favorites = pins_read(PIN_FAVORITES, g_default_favorites);
	load_fallback_css();
	panel_load_init(&g_load);
	panel_load_sample(&g_load);

	/* The width of the monitor the panel sits on.
	 *
	 * gdk_screen_get_width() would be shorter and is deprecated for a good
	 * reason: with more than one output it returns the width of the whole
	 * bounding box, so the panel would stretch across every monitor and
	 * its right-hand tray would land on the wrong one. */
	GdkMonitor *mon = gdk_display_get_primary_monitor(disp);
	GdkRectangle geo;

	if (!mon)
		mon = gdk_display_get_monitor(disp, 0);
	if (mon) {
		gdk_monitor_get_geometry(mon, &geo);
	} else {
		geo.x = geo.y = 0;
		geo.width = 1024;
		geo.height = 768;
	}

	int sw = geo.width;

	g_panel_w = sw;
	g_layout_geo = geo;

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	g_win = win;

	/* A dock, which is what this is: ctwm reads the type and stacks it
	 * accordingly, and no sane window manager offers to decorate one. */
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
	/* And a dock that never takes the keyboard focus.
	 *
	 * This desktop runs ctwm with ClickToFocus, which focuses whatever
	 * window a button was pressed in -- so every click on the panel moved
	 * the focus off the window the user was working in, and the task
	 * button for it stopped being drawn active.  Clicking a button hid
	 * that, because activating the task set the focus straight back; the
	 * right-click menu had nothing to set it back with, and the button
	 * visibly let go the moment the menu opened.
	 *
	 * accept_focus clears the input flag in WM_HINTS, which is the ICCCM
	 * way of saying the window does not want the focus, and ctwm checks it
	 * before every one of its ClickToFocus calls.  It replays the pointer
	 * either way, so the panel still gets the click it declined the focus
	 * for.  The menus keep working because a GTK menu grabs the seat
	 * outright rather than relying on the focus. */
	gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
	gtk_window_set_title(GTK_WINDOW(win), "taskbar");
	gtk_window_set_default_size(GTK_WINDOW(win), sw, PANEL_HEIGHT);

	g_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(g_area, sw, PANEL_HEIGHT);
	gtk_widget_add_events(g_area, GDK_BUTTON_PRESS_MASK |
					      GDK_BUTTON_RELEASE_MASK |
					      GDK_POINTER_MOTION_MASK |
					      GDK_LEAVE_NOTIFY_MASK |
					      GDK_SCROLL_MASK);
	gtk_container_add(GTK_CONTAINER(win), g_area);

	g_signal_connect(g_area, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(g_area, "button-press-event",
			 G_CALLBACK(on_button_press), NULL);
	g_signal_connect(g_area, "button-release-event",
			 G_CALLBACK(on_button_release), NULL);
	g_signal_connect(g_area, "motion-notify-event", G_CALLBACK(on_motion),
			 NULL);
	g_signal_connect(g_area, "leave-notify-event", G_CALLBACK(on_leave),
			 NULL);
	g_signal_connect(g_area, "scroll-event", G_CALLBACK(on_scroll), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show(g_area);

	/* Pinned to the bottom edge of the monitor -- and pinned in the way
	 * ctwm actually honours.
	 *
	 * gtk_window_move() states a position as PPosition, "the program
	 * chose this", and ctwm ignores PPosition unless UsePPosition is
	 * turned on, which this desktop does not turn on.  So ctwm considered
	 * itself free to place the bar wherever it liked, and the bar only
	 * reached the bottom because GTK re-issued the move after the window
	 * was mapped and ctwm honoured that as an ordinary ConfigureRequest.
	 * That is a race being won, not a position being set, and it stopped
	 * being won.
	 *
	 * parse_geometry states it as USPosition instead, "the user chose
	 * this", which ctwm takes as final before it places the window at all.
	 * It is the only call in GTK 3 that sets that hint -- deprecated with
	 * no replacement, which is why load(1) and datetime(1) use it too.
	 *
	 * It comes after the drawing area is added and shown: GTK sizes the
	 * geometry from the window's visible children, and warns when it has
	 * none yet.
	 *
	 * geo.y matters as well as geo.height: on a second monitor stacked
	 * below the first, the bottom of the screen is not the bottom of the
	 * desktop. */
	char geom[32];

	g_snprintf(geom, sizeof(geom), "+%d+%d", geo.x,
		   geo.y + geo.height - PANEL_HEIGHT);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_window_parse_geometry(GTK_WINDOW(win), geom);
	G_GNUC_END_IGNORE_DEPRECATIONS

	/* The panel's colour from the very first moment it is on the screen:
	 * set on the X window between realizing it and mapping it.  The
	 * window paints itself (the drawing area covers all of it), so GTK
	 * does not lay the theme's window colour under it, and the X
	 * background stays the panel's for good rather than until the first
	 * frame. */
	gtk_widget_set_app_paintable(win, TRUE);
	tint_update();
	gtk_widget_realize(win);
	panel_x_background(win, geo.x, geo.y + geo.height - PANEL_HEIGHT, sw,
			   PANEL_HEIGHT);
	gtk_widget_show_all(win);
	g_gdkwin = gtk_widget_get_window(win);

	Window xwin = GDK_WINDOW_XID(g_gdkwin);

	set_strut(xwin, PANEL_HEIGHT, geo.x, sw);

	/* Watch the root for the four properties that describe the desktop
	 * (on top of what GDK selects there for itself: see
	 * select_more_input()). */
	select_more_input(g_root, PropertyChangeMask | SubstructureNotifyMask);
	gdk_window_add_filter(NULL, event_filter, NULL);

	g_ndesktops = prop_card(g_root, A.number_of_desktops, 4);
	g_desktop = prop_card(g_root, A.current_desktop, 0);
	g_showing_desktop = prop_card(g_root, A.showing_desktop, 0) != 0;
	tasks_refresh();
	super_grab();
	net_poll(NULL);
	g_timeout_add_seconds(NET_POLL_SECS, net_poll, NULL);

	/* A new icon theme: the cached icons are the old theme's. */
	g_signal_connect(gtk_icon_theme_get_default(), "changed",
			 G_CALLBACK(on_icon_theme_changed), NULL);

	g_timeout_add_seconds(PANEL_LOAD_SECS, on_tick, NULL);
	g_timeout_add_seconds(3, on_reconcile, NULL);

	/* The volume control's connection to audiod, and the start-up sound
	 * that waits for the desktop. */
	audio_start();
	startup_sound_arm();

	g_signal_connect(gdk_display_get_default_screen(disp), "monitors-changed",
			 G_CALLBACK(on_monitors_changed), NULL);
	g_signal_connect(gdk_display_get_default_screen(disp), "size-changed",
			 G_CALLBACK(on_monitors_changed), NULL);

	gtk_main();
	return 0;
}
