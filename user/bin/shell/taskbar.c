// LikeOS -- taskbar, the desktop shell's panel.
//
// A Windows 11 style bar across the bottom of the screen:
//
//   [load]          [LikeOS] [task] [task] [task] ...      [1][2][3][4] [vol] [time]
//                                                                             [date]
//
// The start button and the window buttons sit together in the middle, the
// load monitor where Windows keeps its widgets, the workspace switcher, the
// speaker of the volume control and the clock at the right.  Each window keeps a button with its icon and title
// (Windows 11's "never combine" arrangement), lit for the window that has the
// focus and marked with the short indicator bars Windows draws under the
// buttons of running programs.
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
// GtkButtons.  The look wanted here is a specific one -- exact colours, tiles
// that only appear under the pointer, indicator bars -- and every one of those
// is a fight with a GTK theme that has its own ideas.  Drawing it directly is
// less code than overriding all of that, and the hit-testing is a handful of
// rectangle comparisons because the layout is computed in one place
// (layout()).
//
// Copyright (C) 2026 The LikeOS Project

#include "panel.h"

#include <audio/audio.h>
#include <errno.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* --- layout constants ---------------------------------------------------- */

#define TILE_H 40	/* every button: Windows 11's 40-pixel taskbar tile */
#define START_W 40	/* the LikeOS button, square */
#define TASK_MAX_W 160	/* a window button never grows past this */
#define TASK_MIN_W 40	/* ... nor shrinks below it; past that they drop off */
#define TASK_LABEL_MIN 96 /* narrower than this, a button shows only its icon */
#define TASK_GAP 4	/* between buttons */
#define WS_BTN_W 32	/* one workspace button */
#define ICON_SIZE 24	/* window icons, as on the Windows 11 taskbar */
#define VOL_W 34	/* the speaker's button in the notification area */

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
} A;

struct task {
	Window win;
	char *title;
	gboolean iconic;
	long desktop;
	GdkPixbuf *icon; /* from _NET_WM_ICON at ICON_SIZE, or NULL */
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
#define HIT_VOLUME -3
#define HIT_WS_BASE -100 /* HIT_WS_BASE - n is workspace n */
static int g_hover = HIT_NONE;
static int g_pressed = HIT_NONE;
static gboolean g_menu_up;
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

/* The flyout's state; the flyout itself is further down, with the menus.
 *
 * Every size here was measured off Windows 11's Quick Settings at 100% scale,
 * keeping only the volume row. */
#define FLY_W 360	  /* the panel, its 1-pixel border included */
#define FLY_BODY_H 72	  /* the volume row's area, inside the border */
#define FLY_ROW_CY 37	  /* the slider's centre line, from the area's top */
#define FLY_RADIUS 8
#define FLY_GAP 12	  /* to the bar and to the screen's right edge */
#define FLY_SHADOW 24	  /* margin for the shadow, when there is a compositor */
#define TIP_SHADOW 10

/* The volume row, as offsets from the panel's left edge. */
#define FLY_SPK_X 25	  /* the speaker glyph */
#define FLY_TRACK_X 57	  /* the slider track... */
#define FLY_TRACK_W 278	  /* ... and its length, ending FLY_SPK_X from the right */

#define TIP_FONT "Selawik 9"

/* Hit codes inside the flyout. */
#define FH_OUTSIDE -1
#define FH_NONE 0
#define FH_MUTE 1
#define FH_SLIDER 2

static struct {
	GtkWidget *win, *tip;
	gboolean up, dark, composited;
	int margin, tip_margin;
	int hover, pressed;
	gboolean dragging;
	double knob;	 /* the accent dot's radius now, animated toward the */
	guint tick;	 /* ... target fly_knob_target() names */
	gint64 tick_last;
	int tip_w, tip_h;
	int grab_tries;
} F = { .knob = 5 };

/* What audiod last told us, as the main thread sees it. */
struct vol_state {
	gboolean online;	 /* connected to audiod */
	int vol;		 /* 0..100: the louder channel, as Windows shows it */
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
	gboolean set_vol, set_mute;
	int vol, mute;
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
	gboolean set_vol, set_mute;
	int vol, mute, nev = 0;
	char ev[AUDIO_EVENTS][AUD_EVENT_NAME_MAX];
	gint64 now = g_get_monotonic_time();
	gboolean waiting = FALSE;

	g_mutex_lock(&Q.lock);
	set_vol = Q.set_vol;
	set_mute = Q.set_mute;
	vol = Q.vol;
	mute = Q.mute;
	Q.set_vol = Q.set_mute = FALSE;
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

/* --- the volume control: glyphs ----------------------------------------------- */

/* The glyphs are drawn rather than taken from the icon theme.  Windows 11's
 * are Segoe Fluent outlines -- one-pixel strokes, hinted onto the pixel grid
 * -- and Adwaita's symbolic speakers are filled shapes of a different weight;
 * next to a clock set in Selawik they looked borrowed.  Each is drawn at 16
 * pixels, its coordinates copied off Windows' own at 100% scale, from the
 * top-left corner of its 16-pixel box. */

static void glyph_begin(cairo_t *cr, double x, double y)
{
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_set_line_width(cr, 1.0);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* The speaker: a box and its cone, then up to three waves, or with `waves'
 * negative the cross of a muted speaker.  With `ghost' set the waves the
 * level does not reach are drawn too, faintly -- the taskbar's version; the
 * flyout's shows only the lit ones. */
static void glyph_speaker(cairo_t *cr, double x, double y, int waves,
			  gboolean ghost)
{
	static const double rad[] = { 3.5, 6.5, 9.5 };
	static const double deg[] = { 45.6, 43.8, 43.2 };
	double r, g, b, a = 1;

	cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a);
	glyph_begin(cr, x, y);
	cairo_move_to(cr, 0.5, 5.5);
	cairo_line_to(cr, 3.5, 5.5);
	cairo_line_to(cr, 6.5, 2.5);
	cairo_line_to(cr, 6.5, 13.5);
	cairo_line_to(cr, 3.5, 10.5);
	cairo_line_to(cr, 0.5, 10.5);
	cairo_close_path(cr);
	cairo_stroke(cr);

	if (waves < 0) {
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		cairo_move_to(cr, 9.5, 5.5);
		cairo_line_to(cr, 14.5, 10.5);
		cairo_move_to(cr, 14.5, 5.5);
		cairo_line_to(cr, 9.5, 10.5);
		cairo_stroke(cr);
	}
	/* A hair heavier than the outline: a curve one pixel wide spreads
	 * over two columns, and Windows' hinted waves read darker. */
	cairo_set_line_width(cr, 1.15);
	for (int i = 0; i < 3; i++) {
		double t = deg[i] * M_PI / 180;

		if (i >= waves) {
			if (!ghost || waves < 0)
				break;
			cairo_set_source_rgba(cr, r, g, b, a * 0.3);
		}
		cairo_new_sub_path(cr);
		cairo_arc(cr, 6.0, 8.0, rad[i], -t, t);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

/* How many waves the speaker shows for the current state: Windows' four
 * steps, with the cross for muted. */
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

/* "Speakers: 52%", as the Windows 11 tray says it. */
static char *vol_tooltip(void)
{
	if (!V.online)
		return g_strdup("No audio device");
	if (V.muted)
		return g_strdup_printf("%s: Muted", vol_output_name());
	return g_strdup_printf("%s: %d%%", vol_output_name(), V.vol);
}

/* --- layout ---------------------------------------------------------------- */

/* Where everything goes.  Called by both the drawing code and the click
 * handler, so the two cannot disagree about what is where -- which is the only
 * real hazard in hit-testing a hand-drawn widget. */
struct layout {
	double load_x, load_w;
	double start_x;
	double task_x, task_bw, task_end;
	double ws_x, ws_w;
	double vol_x;
	double clock_x, clock_w;
	double tile_y;
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
		g_clock_w = panel_clock_width(cr);

	double clock_w = g_clock_w < 80 ? 80 : g_clock_w;

	L->tile_y = (PANEL_HEIGHT - TILE_H) / 2;

	/* The two ends: the load monitor at the left, where Windows keeps its
	 * widgets; the clock at the right, with the speaker of the volume
	 * control beside it where Windows has its own, and the workspace
	 * buttons beyond that. */
	L->load_x = 4;
	L->load_w = PANEL_LOAD_W;
	L->clock_w = clock_w;
	L->clock_x = width - 8 - clock_w;
	L->vol_x = L->clock_x - 4 - VOL_W;
	L->ws_w = g_ndesktops * WS_BTN_W;
	L->ws_x = L->vol_x - 4 - L->ws_w;

	/* The middle: the start button and the window buttons, centred on the
	 * screen as one group.  When there are too many windows for that, the
	 * buttons narrow, and the group slides left until it meets the load
	 * monitor. */
	double left = L->load_x + L->load_w + 8;
	double right = L->ws_x - 8;
	int n = (int)g_tasks->len;
	double bw = TASK_MAX_W;
	double group = START_W + n * (TASK_GAP + bw);

	if (n > 0 && group > right - left) {
		bw = (right - left - START_W) / n - TASK_GAP;
		if (bw < TASK_MIN_W)
			bw = TASK_MIN_W;
		group = START_W + n * (TASK_GAP + bw);
	}

	double gx = floor((width - group) / 2);

	if (gx + group > right)
		gx = right - group;
	if (gx < left)
		gx = left;

	L->start_x = gx;
	L->task_x = gx + START_W + TASK_GAP;
	L->task_bw = floor(bw);
	L->task_end = right;
}

/* --- drawing ---------------------------------------------------------------- */

/* How a control is lit: held down, pointed at, or neither. */
static enum panel_tile tile_state(int hit, gboolean lit)
{
	if (g_pressed == hit && g_hover == hit)
		return PANEL_TILE_DOWN;
	if (lit || g_hover == hit)
		return PANEL_TILE_LIT;
	return PANEL_TILE_NONE;
}

/* The icon for a window that has none of its own. */
static GdkPixbuf *fallback_icon(void)
{
	static GdkPixbuf *pb;

	if (!pb) {
		GtkIconTheme *th = gtk_icon_theme_get_default();

		pb = gtk_icon_theme_load_icon(th, "application-x-executable",
					      ICON_SIZE,
					      GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	}
	return pb;
}

static void draw_tasks(cairo_t *cr, struct layout *L)
{
	double bw = L->task_bw;
	double ty = L->tile_y;
	gboolean labels = bw >= TASK_LABEL_MIN;

	for (guint i = 0; i < g_tasks->len; i++) {
		struct task *t = &g_array_index(g_tasks, struct task, i);

		t->x = L->task_x + i * (bw + TASK_GAP);
		t->w = bw;
		if (t->x + t->w > L->task_end) {
			/* Ran out of bar.  Mark the rest as unplaced so the
			 * click handler does not match them. */
			t->w = 0;
			continue;
		}

		gboolean active = (t->win == g_active) && !t->iconic;

		panel_tile(cr, t->x, ty, t->w, TILE_H,
			   tile_state((int)i, active));

		GdkPixbuf *icon = t->icon ? t->icon : fallback_icon();
		double ix = labels ? t->x + 10 : t->x + (t->w - ICON_SIZE) / 2;
		double iy = ty + (TILE_H - ICON_SIZE) / 2 - 1;

		if (g_pressed == (int)i && g_hover == (int)i)
			iy += 1;
		if (icon) {
			gdk_cairo_set_source_pixbuf(cr, icon, ix, iy);
			cairo_paint(cr);
		}
		if (labels) {
			double lx = ix + ICON_SIZE + 8;

			panel_text(cr, lx, ty + TILE_H / 2 - 1,
				   t->x + t->w - 8 - lx, t->title, FALSE, 0);
		}

		/* Every window that exists gets an indicator; the focused
		 * one's is long and blue. */
		panel_indicator(cr, t->x + t->w / 2, ty + TILE_H - 1, active);
	}
}

static void draw_workspaces(cairo_t *cr, struct layout *L)
{
	for (long i = 0; i < g_ndesktops; i++) {
		double bx = L->ws_x + i * WS_BTN_W;
		gboolean cur = (i == g_desktop);
		char num[24];

		panel_tile(cr, bx + 2, L->tile_y, WS_BTN_W - 4, TILE_H,
			   tile_state(HIT_WS_BASE - (int)i, cur));
		snprintf(num, sizeof(num), "%ld", i + 1);
		panel_text(cr, bx, PANEL_HEIGHT / 2 - 1, WS_BTN_W, num, cur, 1);
		if (cur)
			panel_indicator(cr, bx + WS_BTN_W / 2,
					L->tile_y + TILE_H - 1, TRUE);
	}
}

/* The speaker in the notification area: lit while its flyout is open, and
 * greyed while there is no audiod to talk to. */
static void draw_volume(cairo_t *cr, struct layout *L)
{
	double gx = L->vol_x + 9, gy = L->tile_y + (TILE_H - 16) / 2;

	panel_tile(cr, L->vol_x, L->tile_y, VOL_W, TILE_H,
		   tile_state(HIT_VOLUME, F.up));
	if (V.online) {
		panel_color(cr, PANEL_TEXT);
		glyph_speaker(cr, gx, gy, vol_waves(), TRUE);
	} else {
		cairo_set_source_rgba(cr, 0x1b / 255.0, 0x1b / 255.0,
				      0x1b / 255.0, 0.4);
		glyph_speaker(cr, gx, gy, -1, FALSE);
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

	/* The bar: one flat colour, and a hairline along the top where it
	 * meets the desktop. */
	panel_fill_face(cr, 0, 0, width, height);
	panel_color(cr, PANEL_EDGE);
	cairo_rectangle(cr, 0, 0, width, 1);
	cairo_fill(cr);

	panel_load_draw(cr, &g_load, L.load_x, 0, L.load_w, height);
	panel_start_button(cr, L.start_x, L.tile_y, START_W, TILE_H,
			   tile_state(HIT_START, g_menu_up));
	draw_tasks(cr, &L);
	draw_workspaces(cr, &L);
	draw_volume(cr, &L);
	panel_clock_draw(cr, L.clock_x, 0, L.clock_w, height, TRUE);
	g_bar_drawn = TRUE;
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

static void on_restart_wm(GtkMenuItem *item, gpointer data)
{
	(void)item;
	(void)data;
	signal_wm(SIGHUP, "restart");
}

/* Ends the whole session, not just the decorations: when ctwm goes, xinitrc
 * finishes and the X server follows.
 *
 * Through likeos-logout rather than a signal from here: it starts the log-out
 * sound while the desktop is still on the screen and then tells ctwm to go,
 * so every way of leaving the session ends the same way.  Started asynchronously: the bar
 * keeps drawing until the server takes it away.  If the helper cannot be
 * started at all, ctwm is signalled from here instead. */
static void on_exit_wm(GtkMenuItem *item, gpointer data)
{
	char *argv[] = { (char *)"/usr/bin/likeos-logout", NULL };
	GError *err = NULL;

	(void)item;
	(void)data;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			   &err)) {
		g_warning("exit: cannot start %s: %s", argv[0],
			  err ? err->message : "?");
		g_clear_error(&err);
		signal_wm(SIGTERM, "exit");
	}
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

	GtkWidget *restart = gtk_menu_item_new_with_label("Restart session");
	GtkWidget *quit = gtk_menu_item_new_with_label("Exit session");

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

	GdkRectangle r = { (int)L.start_x, 0, START_W, PANEL_HEIGHT };

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

/* --- the volume flyout ----------------------------------------------------------- */

/* Windows 11's Quick Settings panel with only the volume row left in it: the
 * speaker (mute) and the slider.
 *
 * Drawn by hand like the bar, and for the same reason: the slider wanted is a
 * specific one -- a four-pixel track, a white disc with an accent dot that
 * grows under the pointer and shrinks while held, a value bubble above it
 * while dragging -- and GtkScale under a theme is a long way from that.
 *
 * The flyout is an override-redirect popup that grabs the pointer and the
 * keyboard while it is open, which is how Windows' flyouts behave: keys go to
 * it, and a click anywhere else closes it (and is spent doing so).  Without a
 * compositor -- ctwm does not composite, and nothing else runs -- a window
 * cannot be partly transparent, so the rounded corners are cut with a shape
 * and the soft shadow is left out; with one, both are drawn properly.
 * Its constants and state are declared above, beside the volume state. */

/* The colours.  The light ones were measured off the real panel (its acrylic
 * settles on these greys); the rest are Windows 11's theme resources. */
struct fly_colors {
	double bg[4], border[4], text[4];
	double hover[4], press[4], accent[4], track[4], thumb[4];
	double thumb_top[4], thumb_bot[4], tip_bg[4], tip_border[4];
	double disabled[4];
};

static const struct fly_colors FC_LIGHT = {
	.bg = { 0xe4 / 255.0, 0xe4 / 255.0, 0xe4 / 255.0, 1 },
	.border = { 0, 0, 0, 0.1 },
	.text = { 0, 0, 0, 0.894 },
	.hover = { 0, 0, 0, 0.037 },
	.press = { 0, 0, 0, 0.024 },
	.accent = { 0x00 / 255.0, 0x67 / 255.0, 0xc0 / 255.0, 1 },
	.track = { 0, 0, 0, 0.4458 },
	.thumb = { 1, 1, 1, 1 },
	.thumb_top = { 0, 0, 0, 0.058 },
	.thumb_bot = { 0, 0, 0, 0.162 },
	.tip_bg = { 0xf9 / 255.0, 0xf9 / 255.0, 0xf9 / 255.0, 1 },
	.tip_border = { 0, 0, 0, 0.058 },
	.disabled = { 0, 0, 0, 0.217 },
};

static const struct fly_colors FC_DARK = {
	.bg = { 0x24 / 255.0, 0x24 / 255.0, 0x24 / 255.0, 1 },
	.border = { 0, 0, 0, 0.3 },
	.text = { 1, 1, 1, 1 },
	.hover = { 1, 1, 1, 0.061 },
	.press = { 1, 1, 1, 0.042 },
	.accent = { 0x4c / 255.0, 0xc2 / 255.0, 0xff / 255.0, 1 },
	.track = { 1, 1, 1, 0.5442 },
	.thumb = { 0x45 / 255.0, 0x45 / 255.0, 0x45 / 255.0, 1 },
	.thumb_top = { 1, 1, 1, 0.093 },
	.thumb_bot = { 1, 1, 1, 0.07 },
	.tip_bg = { 0x2c / 255.0, 0x2c / 255.0, 0x2c / 255.0, 1 },
	.tip_border = { 0, 0, 0, 0.2 },
	.disabled = { 1, 1, 1, 0.16 },
};

static const struct fly_colors *w11_colors(gboolean dark)
{
	return dark ? &FC_DARK : &FC_LIGHT;
}

static const struct fly_colors *fly_colors(void)
{
	return w11_colors(F.dark);
}

static void rgba(cairo_t *cr, const double c[4])
{
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

/* The outline around a panel.  Windows draws a faint one and lets the shadow
 * do the separating; without a shadow that hairline vanishes against a white
 * window, so it is drawn firmer then. */
static void fly_outline(cairo_t *cr, const double c[4], double x, double y,
			double w, double h, double r)
{
	if (F.composited)
		rgba(cr, c);
	else if (F.dark)
		cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
	else
		cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
	cairo_set_line_width(cr, 1);
	panel_round_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
	cairo_stroke(cr);
}

/* A soft shadow under a rounded rectangle: stacked translucent outlines,
 * densest at the edge, with the falloff squared the way a blur's is. */
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

static int fly_text(cairo_t *cr, double x, double cy, double w,
		    const char *text, const char *font, const double c[4],
		    int align)
{
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *d = pango_font_description_from_string(font);
	int tw, th;

	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);
	if (w > 0) {
		pango_layout_set_width(l, (int)w * PANGO_SCALE);
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	}
	pango_layout_get_pixel_size(l, &tw, &th);

	double tx = x;

	if (align == 1)
		tx = x + (w - tw) / 2;
	rgba(cr, c);
	cairo_move_to(cr, floor(tx), floor(cy - th / 2.0));
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
	return tw;
}

/* Where everything in the flyout goes, in window coordinates.  Like layout()
 * for the bar: the drawing and the hit-testing both read it. */
struct fly_layout {
	double px, py, pw, ph;	/* the panel; the rest is shadow */
	double cy;		/* the volume row's centre line */
	double sl_x, sl_w;	/* the slider track */
	int win_w, win_h;
};

static void fly_layout(struct fly_layout *L)
{
	int m = F.margin;

	L->pw = FLY_W;
	L->ph = 1 + FLY_BODY_H + 1;
	L->px = m;
	L->py = m;
	L->cy = L->py + 1 + FLY_ROW_CY;
	L->sl_x = L->px + FLY_TRACK_X;
	L->sl_w = FLY_TRACK_W;
	L->win_w = (int)L->pw + 2 * m;
	L->win_h = (int)L->ph + 2 * m;
}

/* The thumb's travel: its centre stays half a thumb inside the track's ends,
 * as WinUI's Slider keeps it. */
static double fly_thumb_x(const struct fly_layout *L, int v)
{
	return L->sl_x + 10 + (L->sl_w - 20) * v / 100.0;
}

static int fly_value_at(const struct fly_layout *L, double x)
{
	double v = (x - L->sl_x - 10) / (L->sl_w - 20) * 100.0;

	return v < 0 ? 0 : v > 100 ? 100 : (int)floor(v + 0.5);
}

/* The mute button's hover tile, which is also its hit area. */
static void fly_button_rect(const struct fly_layout *L, int hit,
			    GdkRectangle *r)
{
	switch (hit) {
	case FH_MUTE:	/* centred on the speaker glyph */
		*r = (GdkRectangle){ (int)L->px + FLY_SPK_X - 8,
				     (int)L->cy - 15, 32, 32 };
		break;
	default:
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
	struct fly_layout L;
	GdkRectangle r;

	fly_layout(&L);
	if (x < L.px || x >= L.px + L.pw || y < L.py || y >= L.py + L.ph)
		return FH_OUTSIDE;

	fly_button_rect(&L, FH_MUTE, &r);
	if (in_rect(&r, x, y))
		return FH_MUTE;
	if (x >= L.sl_x - 4 && x < L.sl_x + L.sl_w + 4 &&
	    y >= L.cy - 16 && y < L.cy + 16)
		return FH_SLIDER;
	return FH_NONE;
}

/* A button's tile: Windows' subtle fill under the pointer, a fainter one
 * while held, nothing at rest. */
static void fly_button_tile(cairo_t *cr, const struct fly_layout *L, int hit)
{
	const struct fly_colors *c = fly_colors();
	GdkRectangle r;

	if (F.pressed == hit && F.hover == hit)
		rgba(cr, c->press);
	else if (F.hover == hit && F.pressed == FH_NONE)
		rgba(cr, c->hover);
	else
		return;
	fly_button_rect(L, hit, &r);
	panel_round_rect(cr, r.x, r.y, r.width, r.height, 4);
	cairo_fill(cr);
}

/* The slider, WinUI's own: a 4-pixel track in two colours either side of a
 * 20-pixel disc carrying an accent dot of radius `knob'.  The track runs from
 * x for w pixels, centred on cy. */
static void w11_slider_paint(cairo_t *cr, const struct fly_colors *c,
			     double x, double w, double cy, int value,
			     double knob, gboolean enabled)
{
	double tx = x + 10 + (w - 20) * value / 100.0;

	rgba(cr, enabled ? c->track : c->disabled);
	panel_round_rect(cr, x, cy - 2, w, 4, 2);
	cairo_fill(cr);
	if (enabled) {
		rgba(cr, c->accent);
		panel_round_rect(cr, x, cy - 2, tx - x, 4, 2);
		cairo_fill(cr);
	}

	/* The disc, with WinUI's elevation border: a hair darker at the bottom
	 * than at the top, which is all that lifts it off the panel. */
	cairo_pattern_t *g = cairo_pattern_create_linear(0, cy - 10, 0, cy + 10);

	cairo_pattern_add_color_stop_rgba(g, 0, c->thumb_top[0], c->thumb_top[1],
					  c->thumb_top[2], c->thumb_top[3]);
	cairo_pattern_add_color_stop_rgba(g, 0.85, c->thumb_top[0],
					  c->thumb_top[1], c->thumb_top[2],
					  c->thumb_top[3]);
	cairo_pattern_add_color_stop_rgba(g, 1, c->thumb_bot[0], c->thumb_bot[1],
					  c->thumb_bot[2], c->thumb_bot[3]);
	cairo_new_sub_path(cr);
	cairo_arc(cr, tx, cy, 10, 0, 2 * M_PI);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_new_sub_path(cr);
	cairo_arc(cr, tx, cy, 9, 0, 2 * M_PI);
	rgba(cr, c->thumb);
	cairo_fill(cr);

	cairo_new_sub_path(cr);
	cairo_arc(cr, tx, cy, knob, 0, 2 * M_PI);
	rgba(cr, enabled ? c->accent : c->disabled);
	cairo_fill(cr);
}

static void fly_draw_slider(cairo_t *cr, const struct fly_layout *L)
{
	w11_slider_paint(cr, fly_colors(), L->sl_x, L->sl_w, L->cy, V.vol,
			 F.knob, V.online);
}

/* The volume row: the speaker (mute) and the slider. */
static void fly_draw_volume(cairo_t *cr, const struct fly_layout *L)
{
	const struct fly_colors *c = fly_colors();

	/* The speaker as it stands now.  Clicking it mutes, and the glyph
	 * turns into the crossed one. */
	fly_button_tile(cr, L, FH_MUTE);
	rgba(cr, V.online ? c->text : c->disabled);
	glyph_speaker(cr, L->px + FLY_SPK_X, L->cy - 7,
		      V.online ? vol_waves() : -1, FALSE);

	fly_draw_slider(cr, L);
}

/* The whole flyout, into a cairo context the size of the window. */
static void fly_paint(cairo_t *cr)
{
	const struct fly_colors *c = fly_colors();
	struct fly_layout L;

	fly_layout(&L);

	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);

	if (F.margin > 0)
		fly_shadow(cr, L.px, L.py, L.pw, L.ph, FLY_RADIUS,
			   F.margin - 4, 8, F.dark ? 0.45 : 0.26);

	panel_round_rect(cr, L.px, L.py, L.pw, L.ph, FLY_RADIUS);
	rgba(cr, c->bg);
	cairo_fill(cr);
	fly_outline(cr, c->border, L.px, L.py, L.pw, L.ph, FLY_RADIUS);
	fly_draw_volume(cr, &L);
}

/* The value bubble shown above the thumb while it is dragged. */
static void tip_paint(cairo_t *cr, int w, int h)
{
	const struct fly_colors *c = fly_colors();
	int m = F.tip_margin;
	char num[8];

	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);

	if (m > 0)
		fly_shadow(cr, m, m, w - 2 * m, h - 2 * m, 4, m - 2, 2,
			   F.dark ? 0.4 : 0.16);
	rgba(cr, c->tip_bg);
	panel_round_rect(cr, m, m, w - 2 * m, h - 2 * m, 4);
	cairo_fill(cr);
	fly_outline(cr, c->tip_border, m, m, w - 2 * m, h - 2 * m, 4);
	g_snprintf(num, sizeof(num), "%d", V.vol);
	fly_text(cr, m, h / 2.0, w - 2 * m, num, TIP_FONT, c->text, 1);
}

/* --- the volume flyout: behaviour ------------------------------------------------- */

/* Is the desktop's GTK theme a dark one?  Asked each time the flyout opens,
 * so a theme switch shows on the next click. */
static gboolean theme_is_dark(void)
{
	GtkSettings *s = gtk_settings_get_default();
	gboolean dark = FALSE;
	char *name = NULL;

	if (!s)
		return FALSE;
	g_object_get(s, "gtk-application-prefer-dark-theme", &dark,
		     "gtk-theme-name", &name, NULL);
	if (name) {
		char *low = g_ascii_strdown(name, -1);

		if (strstr(low, "dark"))
			dark = TRUE;
		g_free(low);
		g_free(name);
	}
	return dark;
}

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

static GtkWidget *popup_new(GCallback draw)
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

/* The "ding" Windows plays when the volume has been changed, so the new level
 * can be heard.  Played once the change is over -- the slider let go, the
 * wheel stopped -- rather than for every step, and never closer together than
 * a few hundred milliseconds. */
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
 * a level unmutes, as it does in Windows: nobody drags a slider expecting to
 * stay silent. */
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
		F.tip = popup_new(G_CALLBACK(on_tip_draw));
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
	int y = oy + (int)L.cy - 10 - 6 - (h - m);

	if (w != F.tip_w || h != F.tip_h) {
		F.tip_w = w;
		F.tip_h = h;
		gtk_widget_set_size_request(F.tip, w, h);
		gtk_window_resize(GTK_WINDOW(F.tip), w, h);
		popup_shape(F.tip, w, h, 4);
	}
	gtk_window_move(GTK_WINDOW(F.tip), x, y);
	gtk_widget_show(F.tip);
	gtk_widget_queue_draw(F.tip);
}

/* --- animation ------------------------------------------------------------------ */

/* The dot is 10 pixels across at rest -- measured off Windows' own Quick
 * Settings slider -- and WinUI scales it by 7/6 under the pointer and by 5/6
 * while held, eased between in about 80 ms. */
static double fly_knob_target(void)
{
	if (F.dragging)
		return 5.0 * 5 / 6;
	if (F.hover == FH_SLIDER && V.online)
		return 5.0 * 7 / 6;
	return 5;
}

/* One frame of the dot's animation. */
static gboolean fly_tick(GtkWidget *w, GdkFrameClock *clock, gpointer d)
{
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	double target = fly_knob_target();
	double dt = F.tick_last ? (now - F.tick_last) / 1e6 : 1.0 / 60;
	double step = dt * 2 / 0.083;
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

/* Size the flyout and pin it where Windows puts Quick Settings: FLY_GAP
 * above the bar and FLY_GAP in from the right edge of the screen. */
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
 * pressing a button under it -- Windows spends that click the same way. */
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
	/* The bar saw no motion while the grab was held; what it last
	 * thought was under the pointer is stale. */
	g_hover = HIT_NONE;
	g_pressed = HIT_NONE;
	gtk_widget_queue_draw(g_area);
}

static void fly_press(int hit, double x)
{
	struct fly_layout L;

	F.pressed = hit;
	if (hit == FH_SLIDER && V.online) {
		/* WinUI's slider moves the thumb to the point pressed and
		 * drags from there. */
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

/* The slider's keys, WinUI's: arrows by one, Page Up and Page Down by ten,
 * Home and End to the ends.  Esc closes, as it closes every flyout. */
static gboolean on_fly_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
	int v = V.vol;

	(void)w;
	(void)d;
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		fly_close();
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
	if (!F.win) {
		F.composited = gdk_screen_is_composited(gdk_screen_get_default());
		F.margin = F.composited ? FLY_SHADOW : 0;
		F.win = popup_new(G_CALLBACK(on_fly_draw));
		gtk_window_set_title(GTK_WINDOW(F.win), "Volume");
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
	F.dark = theme_is_dark();
	F.hover = F.pressed = FH_NONE;
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
		if (g_hover == HIT_VOLUME) {
			char *tip = vol_tooltip();

			gtk_widget_set_tooltip_text(g_area, tip);
			g_free(tip);
		}
	}
	if (!F.up)
		return;
	if (!V.online)
		fly_end_drag();
	gtk_widget_queue_draw(F.win);
}

/* --- input ------------------------------------------------------------------- */

/* Which control is at (x, y)?  See struct layout: this and the drawing code
 * are the only two readers of it, so they agree by construction. */
static int hit_test(double x, double y)
{
	struct layout L;

	(void)y;
	layout(NULL, g_panel_w, &L);

	if (x >= L.start_x && x < L.start_x + START_W)
		return HIT_START;
	if (x >= L.vol_x && x < L.vol_x + VOL_W)
		return HIT_VOLUME;

	for (long i = 0; i < g_ndesktops; i++) {
		double bx = L.ws_x + i * WS_BTN_W;

		if (x >= bx && x < bx + WS_BTN_W)
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
	} else if (ev->button == 2 && hit == HIT_VOLUME) {
		/* A middle click on the speaker mutes, without opening
		 * anything. */
		vol_toggle_mute();
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
	} else if (hit == HIT_VOLUME) {
		/* Normally the flyout's grab takes this click and closes it
		 * before the bar sees it; this is for when the grab could not
		 * be had. */
		if (F.up)
			fly_close();
		else
			fly_open();
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
		/* The tile under the pointer lights up. */
		gtk_widget_queue_draw(g_area);
		/* Tooltip carries the full title, which a narrow button will
		 * have ellipsised. */
		if (hit >= 0 && (guint)hit < g_tasks->len) {
			gtk_widget_set_tooltip_text(
				g_area,
				g_array_index(g_tasks, struct task, hit).title);
		} else if (hit == HIT_VOLUME) {
			char *tip = vol_tooltip();

			gtk_widget_set_tooltip_text(g_area, tip);
			g_free(tip);
		} else {
			gtk_widget_set_tooltip_text(g_area, NULL);
		}
	}
	return TRUE;
}

/* The wheel over the speaker changes the volume, two steps a notch as in
 * Windows, and the ding plays once the wheel has stopped. */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
	(void)w;
	(void)d;
	if (hit_test(ev->x, ev->y) != HIT_VOLUME || !V.online)
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

/* --- X event plumbing ---------------------------------------------------------- */

/* Root and per-window PropertyNotify.
 *
 * Everything the panel shows is a property, so one filter covers the lot: the
 * window list, the focus, the workspace, and each window's title and iconic
 * state.  GDK hands us the raw event before it decides it has no use for it,
 * which is what makes this possible without an X connection of our own. */
static void display_switch(int state); /* below, with the panel's layout */

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
		} else if (a == A.display_switch) {
			display_switch(e->xproperty.state);
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
#undef IA
}

/* The panel's window and the monitor geometry it was laid out for. */
static GtkWidget *g_win;
static GdkRectangle g_layout_geo;

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

int main(int argc, char **argv)
{
	g_set_prgname("taskbar");
	gdk_set_program_class("taskbar");

	gtk_init(&argc, &argv);

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
