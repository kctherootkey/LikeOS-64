// LikeOS-64 -- taskbar, the desktop shell's panel.
//
// A Windows 2000-style bar across the bottom of the screen:
//
//   [LikeOS] [Load|Mem] [ task buttons .......... ] [1][2][3][4] [date time]
//
// It replaces ctwm's icon manager (which was the task list) and ctwm's
// workspace manager (which was the workspace switcher).  Both are switched off
// in system.ctwmrc -- ctwm still manages windows, decorations, placement and
// focus, and this program only observes and asks.
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
// Drawn as one GtkDrawingArea with manual hit-testing rather than as a box of
// GtkButtons.  The look wanted here is a specific one -- two-pixel bevels,
// exact greys, no rounding, no animation -- and every one of those is a fight
// with a GTK theme that has its own ideas.  Drawing it directly is less code
// than overriding all of that, and the hit-testing is a handful of rectangle
// comparisons because the layout is computed in one place (layout()).

#include "panel.h"

#include <gdk/gdkx.h>
#include <signal.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* --- layout constants ---------------------------------------------------- */

#define START_W 78	/* the LikeOS button */
#define LOADMON_W 176	/* the load/memory tray */
#define WS_BTN_W 22	/* one workspace button */
#define TASK_MAX_W 160	/* a task button never grows past this */
#define TASK_MIN_W 40	/* ... nor shrinks below it; past that they scroll */
#define GAP 3		/* between the panel's groups */

/* The atoms this program uses.  Interned once at start-up; X round-trips are
 * the expensive part of talking to the server and these are used on every
 * window on every update. */
static struct {
	Atom client_list, active_window, current_desktop, number_of_desktops;
	Atom wm_desktop, wm_name, wm_visible_name, wm_state, wm_change_state;
	Atom net_wm_state, skip_taskbar, close_window;
	Atom window_type, type_dock, type_desktop, type_toolbar, type_splash;
	Atom utf8, strut_partial, wm_icon;
} A;

struct task {
	Window win;
	char *title;
	gboolean iconic;
	long desktop;
	GdkPixbuf *icon; /* from _NET_WM_ICON, or NULL */
	/* Where layout() put it, so the click handler can find it again. */
	double x, w;
};

static GtkWidget *g_area;
static GdkWindow *g_gdkwin;
static Display *g_dpy;
static Window g_root;

static GArray *g_tasks;	     /* struct task */
static Window g_active;      /* _NET_ACTIVE_WINDOW */
static long g_desktop;	     /* _NET_CURRENT_DESKTOP */
static long g_ndesktops = 4; /* _NET_NUMBER_OF_DESKTOPS */
static struct panel_load g_load;
static int g_panel_w = 800;

/* Which control the pointer is over and which is held down, for the hover and
 * pressed looks.  -1 is none; task buttons are 0..n-1, the specials are the
 * negative constants below. */
#define HIT_NONE -1
#define HIT_START -2
#define HIT_WS_BASE -100 /* HIT_WS_BASE - n is workspace n */
static int g_hover = HIT_NONE;
static int g_pressed = HIT_NONE;
static gboolean g_menu_up;

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

/* The smallest _NET_WM_ICON that is still at least 16x16, scaled to 16.
 *
 * The property is a list of (width, height, ARGB pixels...) runs.  Picking the
 * smallest adequate one rather than the first keeps a 256x256 icon from being
 * downsampled to 16 pixels, which turns detailed artwork into mud. */
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
		if (iw >= 16 && (best_w == 0 || iw < best_w)) {
			best_w = iw;
			best_h = ih;
			best_off = i + 2;
		}
		i += 2 + iw * ih;
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

	GdkPixbuf *small = gdk_pixbuf_scale_simple(pb, 16, 16,
						   GDK_INTERP_BILINEAR);

	g_object_unref(pb);
	return small;
}

/* Should this window get a task button?
 *
 * Skipped: the panel and the desktop (by window type), anything asking to be
 * skipped (GTK sets that for its own utility windows), and anything on another
 * workspace -- the taskbar shows the current desktop, which is what the shell
 * being imitated did.  Windows with _NET_WM_DESKTOP of 0xffffffff are on all
 * desktops and always shown. */
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

/* --- the task list --------------------------------------------------------- */

static void tasks_clear(void)
{
	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);

		g_free(t->title);
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
		g_array_append_val(g_tasks, t);

		/* Watch this window for title and state changes.  Without it
		 * a window that is minimised, renamed or restored would keep
		 * its old button until something else happened to trigger a
		 * refresh. */
		gdk_x11_display_error_trap_push(gdk_display_get_default());
		XSelectInput(g_dpy, wins[i], PropertyChangeMask);
		gdk_x11_display_error_trap_pop_ignored(
			gdk_display_get_default());
	}
	XFree(d);
out:
	g_active = prop_window(g_root, A.active_window);
	if (g_area)
		gtk_widget_queue_draw(g_area);
}

/* --- layout ---------------------------------------------------------------- */

/* Where everything goes.  Called by both the drawing code and the click
 * handler, so the two cannot disagree about what is where -- which is the only
 * real hazard in hit-testing a hand-drawn widget. */
struct layout {
	double start_x, start_w;
	double load_x, load_w;
	double task_x, task_w;
	double ws_x, ws_w, ws_btn_w;
	double clock_x, clock_w;
};

/* The measured width of the clock text, cached from the last draw.
 *
 * layout() has to give the same answer to the drawing code and to the click
 * handler, and only the drawing code has a cairo_t to measure text with.
 * Without the cache the two disagreed by however far the real clock width was
 * from the guess, and every control to the right of the task area -- the
 * workspace buttons above all -- responded to clicks a few pixels away from
 * where it was drawn. */
static double g_clock_w;

static void layout(cairo_t *cr, int width, struct layout *L)
{
	if (cr)
		g_clock_w = panel_clock_width(cr) + 14;

	double clock_w = g_clock_w;

	if (clock_w < 100)
		clock_w = 100;

	double ws_btn = WS_BTN_W;
	double ws_w = g_ndesktops * ws_btn + 6;

	/* Inset from the frame on both sides, the same as the top and bottom. */
	L->start_x = PANEL_BORDER + GAP;
	L->start_w = START_W;
	L->load_x = L->start_x + L->start_w + GAP;
	L->load_w = LOADMON_W;
	L->clock_w = clock_w;
	L->clock_x = width - PANEL_BORDER - GAP - clock_w;
	L->ws_w = ws_w;
	L->ws_btn_w = ws_btn;
	L->ws_x = L->clock_x - GAP - ws_w;
	L->task_x = L->load_x + L->load_w + GAP;
	L->task_w = L->ws_x - GAP - L->task_x;

	/* On a narrow screen the tray groups can eat the whole bar.  The task
	 * area is the one that gives: it is the only part whose contents can
	 * scroll, and a negative width would otherwise be handed to Cairo. */
	if (L->task_w < 0)
		L->task_w = 0;
}

/* --- drawing ---------------------------------------------------------------- */

static void draw_start(cairo_t *cr, struct layout *L, double y, double h)
{
	gboolean pressed = (g_pressed == HIT_START) || g_menu_up;

	/* Drawn by panel.c: the outline, the bevel and the engraved lettering
	 * are one look and belong together.  It shifts a pixel down and right
	 * while held, which is how every button in this style showed it. */
	panel_start_button(cr, L->start_x, y, L->start_w, h, pressed);
}

static void draw_tasks(cairo_t *cr, struct layout *L, double y, double h)
{
	panel_fill_face(cr, L->task_x, y, L->task_w, h);
	panel_bevel(cr, L->task_x, y, L->task_w, h, FALSE, FALSE);

	if (g_tasks->len == 0 || L->task_w < TASK_MIN_W)
		return;

	double avail = L->task_w - 6;
	double bw = avail / g_tasks->len;

	if (bw > TASK_MAX_W)
		bw = TASK_MAX_W;
	if (bw < TASK_MIN_W)
		bw = TASK_MIN_W;

	double bx = L->task_x + 3;
	double by = y + 3;
	double bh = h - 6;

	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);

		t->x = bx;
		t->w = bw - 2;
		if (bx + t->w > L->task_x + L->task_w - 3) {
			/* Ran out of bar.  Mark the rest as unplaced so the
			 * click handler does not match them. */
			t->w = 0;
			bx += bw;
			continue;
		}

		gboolean active = (t->win == g_active) && !t->iconic;
		gboolean down = (g_pressed == (int)i) || active;

		panel_fill_face(cr, t->x, by, t->w, bh);
		panel_bevel(cr, t->x, by, t->w, bh, !down, FALSE);

		/* The active window's button is not merely sunken: it also
		 * gets a dotted fill, which is how the classic shell told a
		 * pressed button apart from the focused one. */
		if (active) {
			cairo_set_source_rgb(cr, PANEL_LIGHT_R, PANEL_LIGHT_G,
					     PANEL_LIGHT_B);
			for (int yy = (int)by + 3; yy < by + bh - 2; yy++)
				for (int xx = (int)t->x + 3 + (yy & 1);
				     xx < t->x + t->w - 2; xx += 2)
					cairo_rectangle(cr, xx, yy, 1, 1);
			cairo_fill(cr);
		}

		double tx = t->x + 5 + (down ? 1 : 0);
		double tw = t->w - 10;

		if (t->icon && tw > 22) {
			gdk_cairo_set_source_pixbuf(cr, t->icon, tx,
						    by + (bh - 16) / 2);
			cairo_paint(cr);
			tx += 19;
			tw -= 19;
		}
		cairo_set_source_rgb(cr, 0, 0, 0);
		if (tw > 0) {
			/* A minimised window's title is bracketed, the way
			 * iconified titles have always been shown on X. */
			char *label = t->iconic ?
					      g_strdup_printf("[%s]", t->title) :
					      g_strdup(t->title);

			panel_text(cr, tx, by + (bh - 15) / 2 + (down ? 1 : 0),
				   tw, label, FALSE, 0);
			g_free(label);
		}
		bx += bw;
	}
}

static void draw_workspaces(cairo_t *cr, struct layout *L, double y, double h)
{
	panel_fill_face(cr, L->ws_x, y, L->ws_w, h);
	panel_bevel(cr, L->ws_x, y, L->ws_w, h, FALSE, FALSE);

	double by = y + 3;
	double bh = h - 6;

	for (long i = 0; i < g_ndesktops; i++) {
		double bx = L->ws_x + 3 + i * L->ws_btn_w;
		double bw = L->ws_btn_w - 2;
		gboolean cur = (i == g_desktop);
		gboolean down = cur || (g_pressed == HIT_WS_BASE - (int)i);
		char num[8];

		panel_fill_face(cr, bx, by, bw, bh);
		panel_bevel(cr, bx, by, bw, bh, !down, FALSE);
		snprintf(num, sizeof(num), "%ld", i + 1);
		cairo_set_source_rgb(cr, 0, 0, 0);
		panel_text(cr, bx + (down ? 1 : 0), by + (bh - 15) / 2 +
				       (down ? 1 : 0), bw, num, cur, 1);
	}
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	(void)data;
	int width = gtk_widget_get_allocated_width(widget);
	int height = gtk_widget_get_allocated_height(widget);
	struct layout L;

	g_panel_w = width;
	layout(cr, width, &L);

	panel_fill_face(cr, 0, 0, width, height);

	/* The bar's own frame: a raised band all the way round, the same shape
	 * and the same shading ctwm gives every window it manages.  The panel
	 * is a window on this desktop and should look like one.
	 *
	 * Drawn round the whole rectangle rather than just the top edge even
	 * though three of its sides are against the screen edge: a band that
	 * stopped short would leave the corners unmitred, and the mitre is
	 * half of what makes it read as a ctwm frame. */
	panel_bevel_n(cr, 0, 0, width, height, PANEL_BORDER, TRUE);

	/* Everything inside the frame.  CY/CH are the content band -- the row
	 * the controls share -- so the frame width is named once here rather
	 * than being spelled out at each of the five call sites. */
	double cy = PANEL_BORDER;
	double ch = height - PANEL_BORDER * 2;

	draw_start(cr, &L, cy, ch);
	panel_load_draw(cr, &g_load, L.load_x, cy, L.load_w, ch);
	draw_tasks(cr, &L, cy, ch);
	draw_workspaces(cr, &L, cy, ch);

	panel_fill_face(cr, L.clock_x, cy, L.clock_w, ch);
	panel_bevel(cr, L.clock_x, cy, L.clock_w, ch, FALSE, FALSE);
	panel_clock_draw(cr, L.clock_x, cy, L.clock_w, ch, FALSE);
	return TRUE;
}

/* --- the start menu ---------------------------------------------------------- */

/* Read one .desktop file into a name, an icon name and a command.
 *
 * The shortcuts on the desktop are Type=Link entries pointing at the real
 * application entry under /usr/share/applications, so a Link is followed once
 * to find the Exec line.  See res/etc/skel/Desktop for why they are Links.
 *
 * Returns FALSE for anything without a usable command. */
static gboolean read_entry(const char *path, char **name, char **icon,
			   char **exec)
{
	GKeyFile *kf = g_key_file_new();
	gboolean ok = FALSE;

	*name = *icon = *exec = NULL;
	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
		goto out;

	char *type = g_key_file_get_string(kf, "Desktop Entry", "Type", NULL);

	*name = g_key_file_get_locale_string(kf, "Desktop Entry", "Name", NULL,
					     NULL);
	*icon = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);

	if (g_strcmp0(type, "Link") == 0) {
		char *url = g_key_file_get_string(kf, "Desktop Entry", "URL",
						  NULL);

		if (url) {
			GKeyFile *t = g_key_file_new();

			if (g_key_file_load_from_file(t, url, G_KEY_FILE_NONE,
						      NULL)) {
				*exec = g_key_file_get_string(
					t, "Desktop Entry", "Exec", NULL);
				if (!*icon)
					*icon = g_key_file_get_string(
						t, "Desktop Entry", "Icon",
						NULL);
				if (!*name)
					*name = g_key_file_get_locale_string(
						t, "Desktop Entry", "Name",
						NULL, NULL);
			}
			g_key_file_free(t);
			g_free(url);
		}
	} else {
		*exec = g_key_file_get_string(kf, "Desktop Entry", "Exec",
					      NULL);
	}
	g_free(type);
	ok = (*exec != NULL);
out:
	g_key_file_free(kf);
	if (!ok) {
		g_clear_pointer(name, g_free);
		g_clear_pointer(icon, g_free);
		g_clear_pointer(exec, g_free);
	}
	return ok;
}

/* Strip the field codes a desktop Exec line may carry (%f, %U, ...).
 *
 * They name files to open, and nothing is being opened here.  Left in, they
 * would be passed to the program as literal arguments. */
static char *clean_exec(const char *exec)
{
	GString *s = g_string_new(NULL);

	for (const char *p = exec; *p; p++) {
		if (p[0] == '%' && p[1]) {
			p++;
			continue;
		}
		g_string_append_c(s, *p);
	}
	return g_string_free(s, FALSE);
}

static void on_launch(GtkMenuItem *item, gpointer data)
{
	(void)data;
	const char *cmd = g_object_get_data(G_OBJECT(item), "exec");
	GError *err = NULL;

	if (!cmd)
		return;

	if (!g_spawn_command_line_async(cmd, &err)) {
		g_warning("cannot start %s: %s", cmd,
			  err ? err->message : "?");
		g_clear_error(&err);
	}
}

/* Signal the window manager.
 *
 * There is no EWMH message for "restart yourself" or "quit" -- neither is
 * something the spec contemplates a client asking for.  ctwm restarts on
 * SIGHUP and shuts down on SIGTERM, and xinitrc writes its pid to
 * ~/.ctwm.pid before exec'ing it precisely so that something outside ctwm can
 * reach it: it advertises _NET_SUPPORTING_WM_CHECK but no _NET_WM_PID, so the
 * file is the only route.
 *
 * The pid is read fresh each time rather than cached: a restart replaces the
 * process, and xinitrc's exec means the file still holds the right number
 * afterwards only because exec keeps the pid. */
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

static void on_restart_wm(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	signal_wm(SIGHUP, "restart");
}

static void on_exit_wm(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	/* Ends the whole session, not just the decorations: xinitrc's last
	 * command is `exec ctwm', so when it goes the X server follows. */
	signal_wm(SIGTERM, "exit");
}

/* The icon for a menu entry, from the icon theme, at 16 pixels.
 *
 * Falls back to the generic executable icon: xnedit, for one, names an icon
 * the theme on this image does not carry, and a menu with one blank row looks
 * like a bug rather than a missing artwork file. */
static GtkWidget *menu_icon(const char *name)
{
	GtkIconTheme *theme = gtk_icon_theme_get_default();
	GdkPixbuf *pb = NULL;

	if (name)
		pb = gtk_icon_theme_load_icon(theme, name, 16,
					      GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	if (!pb)
		pb = gtk_icon_theme_load_icon(theme, "application-x-executable",
					      16, GTK_ICON_LOOKUP_FORCE_SIZE,
					      NULL);
	if (!pb)
		return NULL;

	GtkWidget *img = gtk_image_new_from_pixbuf(pb);

	g_object_unref(pb);
	return img;
}

/* One menu row: icon on the left, label beside it.
 *
 * Built by hand out of a box rather than with GtkImageMenuItem, which is
 * deprecated and which recent GTK ignores the image of unless a setting is
 * turned on -- the icons are the point of both the menus built here. */
static GtkWidget *icon_menu_item(const char *icon, const char *label)
{
	GtkWidget *item = gtk_menu_item_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *img = menu_icon(icon);
	GtkWidget *lbl = gtk_label_new(label);

	if (img)
		gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(item), box);
	return item;
}

/* One row of the Programs submenu, which is the above plus what to run. */
static GtkWidget *program_item(const char *name, const char *icon,
			       const char *exec)
{
	GtkWidget *item = icon_menu_item(icon, name && *name ? name : exec);

	g_object_set_data_full(G_OBJECT(item), "exec", g_strdup(exec), g_free);
	g_signal_connect(item, "activate", G_CALLBACK(on_launch), NULL);
	return item;
}

/* The Programs submenu, built from the shortcuts on the desktop.
 *
 * The desktop is the list: whatever the user put there is what the menu
 * offers, which is the arrangement the shell this imitates used and means
 * there is one place to add a program rather than two. */
static GtkWidget *programs_menu(void)
{
	GtkWidget *menu = gtk_menu_new();
	const char *home = g_get_home_dir();
	char *dir = g_build_filename(home, "Desktop", NULL);
	GDir *d = g_dir_open(dir, 0, NULL);
	int count = 0;

	if (!d) {
		/* No Desktop directory yet -- a fresh account before pcmanfm
		 * has run.  The skeleton is the same list. */
		g_free(dir);
		dir = g_strdup("/etc/skel/Desktop");
		d = g_dir_open(dir, 0, NULL);
	}

	if (d) {
		GList *names = NULL;
		const char *fn;

		while ((fn = g_dir_read_name(d)))
			if (g_str_has_suffix(fn, ".desktop"))
				names = g_list_prepend(names, g_strdup(fn));
		g_dir_close(d);
		names = g_list_sort(names, (GCompareFunc)g_strcmp0);

		for (GList *it = names; it; it = it->next) {
			char *path = g_build_filename(dir, it->data, NULL);
			char *name = NULL, *icon = NULL, *exec = NULL;

			if (read_entry(path, &name, &icon, &exec)) {
				char *cmd = clean_exec(exec);

				gtk_menu_shell_append(
					GTK_MENU_SHELL(menu),
					program_item(name, icon, cmd));
				g_free(cmd);
				count++;
			}
			g_free(name);
			g_free(icon);
			g_free(exec);
			g_free(path);
		}
		g_list_free_full(names, g_free);
	}
	g_free(dir);

	if (count == 0) {
		GtkWidget *empty = gtk_menu_item_new_with_label(
			"(no programs on the desktop)");

		gtk_widget_set_sensitive(empty, FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty);
	}
	gtk_widget_show_all(menu);
	return menu;
}

/* Tear the menu down once it closes.
 *
 * A menu popped up with gtk_menu_popup_at_rect() is not owned by anything --
 * it is built floating and stays alive until somebody destroys it -- so
 * without this every click on the LikeOS button would leak a menu and all its
 * icons.  Destroyed from an idle rather than inline because this runs from
 * inside the menu's own signal emission. */
static gboolean destroy_menu(gpointer menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu));
	return G_SOURCE_REMOVE;
}

static void on_task_close(GtkMenuItem *item, gpointer data)
{
	(void)data;
	Window w = (Window)(uintptr_t)g_object_get_data(G_OBJECT(item), "win");

	if (w != None)
		task_close(w);
}

static void on_menu_done(GtkWidget *w, gpointer data)
{
	(void)data;
	g_menu_up = FALSE;
	gtk_widget_queue_draw(g_area);
	g_idle_add(destroy_menu, w);
}

static void show_start_menu(void)
{
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *programs = gtk_menu_item_new_with_label("Programs");

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(programs), programs_menu());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), programs);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			      gtk_separator_menu_item_new());

	GtkWidget *restart = gtk_menu_item_new_with_label("Restart ctwm");
	GtkWidget *quit = gtk_menu_item_new_with_label("Exit ctwm");

	g_signal_connect(restart, "activate", G_CALLBACK(on_restart_wm), NULL);
	g_signal_connect(quit, "activate", G_CALLBACK(on_exit_wm), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), restart);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);

	g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_done), NULL);
	gtk_widget_show_all(menu);

	/* Anchored to the start button and opening UPWARD: the panel is at the
	 * bottom of the screen, so the menu grows away from it.  The rect's
	 * NORTH_WEST corner meets the menu's SOUTH_WEST one, which puts the
	 * bottom-left of the menu on the top-left of the button. */
	struct layout L;

	layout(NULL, g_panel_w, &L);

	GdkRectangle r = { (int)L.start_x, 0, (int)L.start_w, PANEL_HEIGHT };

	g_menu_up = TRUE;
	gtk_widget_queue_draw(g_area);
	gtk_menu_popup_at_rect(GTK_MENU(menu), g_gdkwin, &r,
			       GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST,
			       NULL);
}

/* The window menu, on a right-click on a task button.
 *
 * Closing outright on right-click is what this did first, and it is too easy
 * to do by accident: the button is small, the pointer is already over it, and
 * there is no way back from a window that has gone.  One menu entry is barely
 * a menu, but it is a confirmation step and it is where the rest of the
 * window commands would go.
 *
 * Anchored to the button rather than to the pointer, so it lines up with the
 * thing it acts on, and opening upward for the same reason the start menu
 * does -- the panel is at the bottom of the screen. */
static void show_task_menu(int idx)
{
	struct task *t = &g_array_index(g_tasks, struct task, idx);
	GtkWidget *menu = gtk_menu_new();
	GtkWidget *close = icon_menu_item("window-close", "Close");

	g_object_set_data(G_OBJECT(close), "win",
			  (gpointer)(uintptr_t)t->win);
	g_signal_connect(close, "activate", G_CALLBACK(on_task_close), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), close);

	g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_done), NULL);
	gtk_widget_show_all(menu);

	GdkRectangle r = { (int)t->x, 0, (int)t->w, PANEL_HEIGHT };

	gtk_menu_popup_at_rect(GTK_MENU(menu), g_gdkwin, &r,
			       GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST,
			       NULL);
}

/* --- input ------------------------------------------------------------------- */

/* Which control is at (x, y)?  See struct layout: this and the drawing code
 * are the only two readers of it, so they agree by construction. */
static int hit_test(double x, double y)
{
	struct layout L;

	(void)y;
	layout(NULL, g_panel_w, &L);

	if (x >= L.start_x && x < L.start_x + L.start_w)
		return HIT_START;

	for (long i = 0; i < g_ndesktops; i++) {
		double bx = L.ws_x + 3 + i * L.ws_btn_w;

		if (x >= bx && x < bx + L.ws_btn_w - 2)
			return HIT_WS_BASE - (int)i;
	}

	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);

		if (t->w > 0 && x >= t->x && x < t->x + t->w)
			return (int)i;
	}
	return HIT_NONE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;

	int hit = hit_test(ev->x, ev->y);

	if (ev->button == 1) {
		g_pressed = hit;
		gtk_widget_queue_draw(g_area);
	} else if (ev->button == 3 && hit >= 0 && (guint)hit < g_tasks->len) {
		show_task_menu(hit);
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

	if (hit == HIT_START) {
		show_start_menu();
	} else if (hit <= HIT_WS_BASE) {
		goto_desktop(HIT_WS_BASE - hit);
	} else if (hit >= 0 && (guint)hit < g_tasks->len) {
		struct task *t = &g_array_index(g_tasks, struct task, hit);

		/* The classic behaviour, and the one people expect: clicking
		 * the button of the window you are already in minimises it,
		 * clicking any other brings it up. */
		if (t->win == g_active && !t->iconic)
			task_minimise(t->win);
		else
			task_activate(t->win);
	}
	return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d)
{
	(void)w;
	(void)d;
	int hit = hit_test(ev->x, ev->y);

	if (hit != g_hover) {
		g_hover = hit;
		/* Tooltip carries the full title, which a narrow button will
		 * have ellipsised. */
		if (hit >= 0 && (guint)hit < g_tasks->len)
			gtk_widget_set_tooltip_text(
				g_area,
				g_array_index(g_tasks, struct task, hit).title);
		else
			gtk_widget_set_tooltip_text(g_area, NULL);
	}
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

/* --- X event plumbing ---------------------------------------------------------- */

/* Root and per-window PropertyNotify.
 *
 * Everything the panel shows is a property, so one filter covers the lot: the
 * window list, the focus, the workspace, and each window's title and iconic
 * state.  GDK hands us the raw event before it decides it has no use for it,
 * which is what makes this possible without an X connection of our own. */
static GdkFilterReturn event_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
	XEvent *e = (XEvent *)xev;

	(void)ev;
	(void)data;

	if (e->type != PropertyNotify)
		return GDK_FILTER_CONTINUE;

	Atom a = e->xproperty.atom;

	if (e->xproperty.window == g_root) {
		if (a == A.client_list) {
			tasks_refresh();
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
#undef IA
}

/* Classic colours for the menus.
 *
 * The menu is the one part of this that is real GTK widgets rather than
 * Cairo -- submenus, keyboard navigation and icon rows are a great deal of
 * machinery to reimplement -- so it is styled instead of drawn.  Without this
 * the panel is 2000 and its menu is 2014. */
static void apply_menu_css(void)
{
	static const char *css =
		"menu, .menu {"
		"  background-color: #d4d0c8;"
		"  border: 1px solid #808080;"
		"  padding: 2px;"
		"}"
		"menu menuitem, .menu menuitem {"
		"  color: #000000;"
		"  padding: 3px 8px;"
		"  font-family: 'DejaVu Sans';"
		"  font-size: 8pt;"
		"}"
		"menu menuitem:hover, .menu menuitem:hover {"
		"  background-color: #0a246a;"
		"  color: #ffffff;"
		"}"
		"menu menuitem:disabled, .menu menuitem:disabled {"
		"  color: #808080;"
		"}"
		"menu separator, .menu separator {"
		"  background-color: #808080;"
		"  margin: 2px 1px;"
		"  min-height: 1px;"
		"}"
		"tooltip, tooltip.background {"
		"  background-color: #ffffe1;"
		"  color: #000000;"
		"  border: 1px solid #000000;"
		"}";
	GtkCssProvider *p = gtk_css_provider_new();

	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(
		gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

int main(int argc, char **argv)
{
	g_set_prgname("taskbar");
	gdk_set_program_class("taskbar");

	gtk_init(&argc, &argv);
	apply_menu_css();

	GdkDisplay *disp = gdk_display_get_default();

	g_dpy = gdk_x11_display_get_xdisplay(disp);
	g_root = gdk_x11_get_default_root_xwindow();
	intern_atoms();

	g_tasks = g_array_new(FALSE, TRUE, sizeof(struct task));
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

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

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
	 * geo.y matters as well as geo.height: on a second monitor stacked
	 * below the first, the bottom of the screen is not the bottom of the
	 * desktop. */
	char geom[32];

	g_snprintf(geom, sizeof(geom), "+%d+%d", geo.x,
		   geo.y + geo.height - PANEL_HEIGHT);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_window_parse_geometry(GTK_WINDOW(win), geom);
	G_GNUC_END_IGNORE_DEPRECATIONS

	g_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(g_area, sw, PANEL_HEIGHT);
	gtk_widget_add_events(g_area, GDK_BUTTON_PRESS_MASK |
					      GDK_BUTTON_RELEASE_MASK |
					      GDK_POINTER_MOTION_MASK |
					      GDK_LEAVE_NOTIFY_MASK);
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
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_show_all(win);
	g_gdkwin = gtk_widget_get_window(win);

	Window xwin = GDK_WINDOW_XID(g_gdkwin);

	set_strut(xwin, PANEL_HEIGHT, geo.x, sw);

	/* Watch the root for the four properties that describe the desktop. */
	gdk_x11_display_error_trap_push(disp);
	XSelectInput(g_dpy, g_root, PropertyChangeMask | SubstructureNotifyMask);
	gdk_x11_display_error_trap_pop_ignored(disp);
	gdk_window_add_filter(NULL, event_filter, NULL);

	g_ndesktops = prop_card(g_root, A.number_of_desktops, 4);
	g_desktop = prop_card(g_root, A.current_desktop, 0);
	tasks_refresh();

	g_timeout_add_seconds(PANEL_LOAD_SECS, on_tick, NULL);
	g_timeout_add_seconds(3, on_reconcile, NULL);

	gtk_main();
	return 0;
}
