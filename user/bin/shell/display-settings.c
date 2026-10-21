// LikeOS -- likeos-display-settings, the Display settings dialog.
//
// Lists the sizes the screen can be set to and sets the one picked:
//
//  - With a display driver behind the X server (modesetting on i915 or
//    vmwgfx) RandR offers the output's modes.  The size is switched at once
//    and saved, and the user is asked whether to keep it; switching back
//    restores and saves the previous size.  Nobody answering in
//    REVERT_SECONDS counts as switching back, so a size the monitor cannot
//    show does not stay on a screen nobody can read.
//  - On the boot framebuffer (the fbdev X driver) there is one mode and no
//    way to change it while running.  The sizes listed are the boot
//    firmware's (/sys/firmware/efi/gop_modes); the size picked is saved and
//    the bootloader sets it at the next start.
//
// The setting lives in /etc/likeos/display.conf, written through the setuid
// helper likeos-display-save.  The bootloader reads it, the kernel's display
// drivers come up in it, and `likeos-display-settings --restore', run by
// xinitrc before the desktop starts, sets it in the X server where the
// driver offers it but came up in another (the panel's own mode on i915).
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF_PATH "/etc/likeos/display.conf"
#define SAVE_HELPER "/usr/libexec/likeos-display-save"
#define GOP_MODES "/sys/firmware/efi/gop_modes"
#define REVERT_SECONDS 20
#define SWITCH_ATOM "_LIKEOS_DISPLAY_SWITCH"
#define SWITCH_WAIT_MS 500
#define WM_RESTART_WAIT_MS 3000
#define MAX_SIZES 128

struct size {
	unsigned w, h;
	RRMode mode; /* None on the framebuffer */
};

/* What RandR says about the output the desktop is on. */
struct output {
	Display *dpy;
	Window root;
	XRRScreenResources *res;
	RROutput id;
	RRCrtc crtc;
	unsigned cur_w, cur_h;
	struct size sizes[MAX_SIZES];
	int nsizes;
};

/* ---- the saved setting ---- */

static int saved_size(unsigned *w, unsigned *h)
{
	FILE *f = fopen(CONF_PATH, "r");
	char line[128];
	int ok = 0;

	if (!f)
		return 0;
	while (!ok && fgets(line, sizeof(line), f))
		ok = sscanf(line, "resolution=%ux%u", w, h) == 2;
	fclose(f);
	return ok;
}

static gboolean save_size(unsigned w, unsigned h, GError **err)
{
	char arg[32];
	char *argv[] = { (char *)SAVE_HELPER, arg, NULL };
	gint status = 0;

	snprintf(arg, sizeof(arg), "%ux%u", w, h);
	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			  NULL, &status, err))
		return FALSE;
	return g_spawn_check_wait_status(status, err);
}

/* ---- RandR ---- */

static int size_cmp(const void *a, const void *b)
{
	const struct size *x = a, *y = b;

	if (x->w != y->w)
		return x->w < y->w ? 1 : -1;
	if (x->h != y->h)
		return x->h < y->h ? 1 : -1;
	return 0;
}

static double mode_refresh(const XRRModeInfo *m)
{
	if (!m->hTotal || !m->vTotal)
		return 0;
	return (double)m->dotClock / ((double)m->hTotal * m->vTotal);
}

static const XRRModeInfo *mode_info(const XRRScreenResources *res, RRMode id)
{
	for (int i = 0; i < res->nmode; i++)
		if (res->modes[i].id == id)
			return &res->modes[i];
	return NULL;
}

/* The primary output, else the first connected one with a CRTC, and its
 * sizes: one entry per size, the output's preferred mode of that size if it
 * has one, else the fastest. */
static int output_probe(struct output *o, Display *dpy)
{
	int ev, er, major = 0, minor = 0;
	XRROutputInfo *oi = NULL;

	memset(o, 0, sizeof(*o));
	o->dpy = dpy;
	o->root = DefaultRootWindow(dpy);
	o->cur_w = DisplayWidth(dpy, DefaultScreen(dpy));
	o->cur_h = DisplayHeight(dpy, DefaultScreen(dpy));
	if (!XRRQueryExtension(dpy, &ev, &er) ||
	    !XRRQueryVersion(dpy, &major, &minor) ||
	    (major == 1 && minor < 2))
		return -1;
	o->res = XRRGetScreenResourcesCurrent(dpy, o->root);
	if (!o->res)
		return -1;

	RROutput primary = XRRGetOutputPrimary(dpy, o->root);
	for (int pass = 0; pass < 2 && !o->id; pass++) {
		for (int i = 0; i < o->res->noutput && !o->id; i++) {
			RROutput id = o->res->outputs[i];
			XRROutputInfo *t;

			if (pass == 0 && id != primary)
				continue;
			t = XRRGetOutputInfo(dpy, o->res, id);
			if (t && t->connection == RR_Connected && t->crtc) {
				o->id = id;
				oi = t;
			} else if (t) {
				XRRFreeOutputInfo(t);
			}
		}
	}
	if (!o->id)
		return -1;
	o->crtc = oi->crtc;

	XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, o->res, o->crtc);
	if (ci) {
		o->cur_w = ci->width;
		o->cur_h = ci->height;
		XRRFreeCrtcInfo(ci);
	}

	for (int i = 0; i < oi->nmode; i++) {
		const XRRModeInfo *m = mode_info(o->res, oi->modes[i]);
		int k;

		if (!m)
			continue;
		for (k = 0; k < o->nsizes; k++)
			if (o->sizes[k].w == m->width &&
			    o->sizes[k].h == m->height)
				break;
		if (k == o->nsizes) {
			if (o->nsizes == MAX_SIZES)
				continue;
			o->sizes[k].w = m->width;
			o->sizes[k].h = m->height;
			o->sizes[k].mode = m->id;
			o->nsizes++;
		} else if (i >= oi->npreferred) {
			const XRRModeInfo *had = mode_info(o->res, o->sizes[k].mode);
			int had_preferred = 0;

			for (int p = 0; p < oi->npreferred; p++)
				if (oi->modes[p] == o->sizes[k].mode)
					had_preferred = 1;
			if (!had_preferred && had &&
			    mode_refresh(m) > mode_refresh(had))
				o->sizes[k].mode = m->id;
		}
	}
	XRRFreeOutputInfo(oi);
	qsort(o->sizes, o->nsizes, sizeof(o->sizes[0]), size_cmp);
	return 0;
}

static const struct size *output_find(const struct output *o, unsigned w,
				      unsigned h)
{
	for (int i = 0; i < o->nsizes; i++)
		if (o->sizes[i].w == w && o->sizes[i].h == h)
			return &o->sizes[i];
	return NULL;
}

static int x_error;

static int on_x_error(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	(void)ev;
	x_error = 1;
	return 0;
}

/* Put the output's CRTC on `s' and make the screen that size.  Every CRTC
 * that is on goes off first, so the screen can shrink below where any of
 * them reached; the others stay off -- this desktop runs one output. */
static int output_set(struct output *o, const struct size *s)
{
	Display *dpy = o->dpy;
	int scr = DefaultScreen(dpy);
	int (*old)(Display *, XErrorEvent *);
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, o->root);
	XRRCrtcInfo *ci;
	Rotation rot;
	int mm_w, mm_h, failed = 0;

	/* Fresh resources: their timestamps are what a set is checked
	 * against. */
	if (!res)
		return -1;
	XRRFreeScreenResources(o->res);
	o->res = res;
	ci = XRRGetCrtcInfo(dpy, o->res, o->crtc);
	rot = ci ? ci->rotation : RR_Rotate_0;
	if (ci)
		XRRFreeCrtcInfo(ci);
	/* Keep the screen's dots per millimetre. */
	mm_w = (int)((double)s->w * DisplayWidthMM(dpy, scr) /
		     DisplayWidth(dpy, scr));
	mm_h = (int)((double)s->h * DisplayHeightMM(dpy, scr) /
		     DisplayHeight(dpy, scr));

	x_error = 0;
	old = XSetErrorHandler(on_x_error);
	XGrabServer(dpy);
	for (int i = 0; i < o->res->ncrtc; i++) {
		XRRCrtcInfo *c = XRRGetCrtcInfo(dpy, o->res, o->res->crtcs[i]);

		if (c && c->mode != None)
			XRRSetCrtcConfig(dpy, o->res, o->res->crtcs[i],
					 CurrentTime, 0, 0, None, RR_Rotate_0,
					 NULL, 0);
		if (c)
			XRRFreeCrtcInfo(c);
	}
	XRRSetScreenSize(dpy, o->root, (int)s->w, (int)s->h, mm_w, mm_h);
	if (XRRSetCrtcConfig(dpy, o->res, o->crtc, CurrentTime, 0, 0, s->mode,
			     rot, &o->id, 1) != RRSetConfigSuccess)
		failed = 1;
	XUngrabServer(dpy);
	XSync(dpy, False);
	XSetErrorHandler(old);
	if (x_error || failed)
		return -1;
	o->cur_w = s->w;
	o->cur_h = s->h;
	return 0;
}

/* The desktop bar off the screen for a size change, so it is not seen laid
 * out for the old size on the new screen: _LIKEOS_DISPLAY_SWITCH on the root
 * set to 1, and the bar answers 2 once it has gone (taskbar.c).  Waits for
 * the answer at most SWITCH_WAIT_MS -- no bar running means no answer.  On a
 * connection of its own, so that selecting the root's property changes does
 * not change what GDK's connection receives.  Returns that connection, for
 * panel_show(), or NULL. */
static Display *panel_hide(void)
{
	Display *d = XOpenDisplay(NULL);
	Window root;
	Atom a;
	long one = 1;
	gint64 end;

	if (!d)
		return NULL;
	root = DefaultRootWindow(d);
	a = XInternAtom(d, SWITCH_ATOM, False);
	XSelectInput(d, root, PropertyChangeMask);
	XChangeProperty(d, root, a, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&one, 1);
	XFlush(d);
	end = g_get_monotonic_time() + SWITCH_WAIT_MS * 1000;
	for (;;) {
		while (XPending(d)) {
			XEvent ev;
			Atom type;
			int format;
			unsigned long n, after;
			unsigned char *data = NULL;
			long v = 0;

			XNextEvent(d, &ev);
			if (ev.type != PropertyNotify || ev.xproperty.atom != a ||
			    ev.xproperty.state != PropertyNewValue)
				continue;
			if (XGetWindowProperty(d, root, a, 0, 1, False,
					       XA_CARDINAL, &type, &format, &n,
					       &after, &data) == Success &&
			    data && n == 1 && format == 32)
				v = *(long *)data;
			if (data)
				XFree(data);
			if (v == 2)
				return d;
		}

		gint64 left = end - g_get_monotonic_time();
		struct pollfd pfd = { ConnectionNumber(d), POLLIN, 0 };

		if (left <= 0)
			return d;
		poll(&pfd, 1, (int)(left / 1000) + 1);
	}
}

/* The size has changed: the bar lays itself out for it and comes back. */
static void panel_show(Display *d)
{
	if (!d)
		return;
	XDeleteProperty(d, DefaultRootWindow(d),
			XInternAtom(d, SWITCH_ATOM, False));
	XCloseDisplay(d);
}

/* ctwm lays out its screen only when it starts: have it restart, as the
 * desktop menu's Restart session does.  The desktop follows the new size on
 * its own, the taskbar through panel_hide()/panel_show().  True when the
 * signal went out. */
static gboolean restart_wm(void)
{
	char *path = g_build_filename(g_get_home_dir(), ".ctwm.pid", NULL);
	char *buf = NULL;
	gboolean sent = FALSE;

	if (g_file_get_contents(path, &buf, NULL, NULL)) {
		pid_t pid = (pid_t)strtol(buf, NULL, 10);

		if (pid > 1)
			sent = kill(pid, SIGHUP) == 0;
		g_free(buf);
	}
	g_free(path);
	return sent;
}

/* Until the restarted window manager has the screen again -- it sets
 * _NET_SUPPORTING_WM_CHECK on the root once it has taken it over -- at most
 * WM_RESTART_WAIT_MS.  A window mapped while it restarts asks the manager
 * that is going away to map it, and that request is lost with it: the
 * window stays unmapped, which is how the desktop bar went missing.  `d' is
 * panel_hide()'s connection, which already receives the root's property
 * changes. */
static void wait_for_wm(Display *d)
{
	Atom check = XInternAtom(d, "_NET_SUPPORTING_WM_CHECK", False);
	gint64 end = g_get_monotonic_time() + WM_RESTART_WAIT_MS * 1000;

	for (;;) {
		while (XPending(d)) {
			XEvent ev;

			XNextEvent(d, &ev);
			if (ev.type == PropertyNotify &&
			    ev.xproperty.atom == check &&
			    ev.xproperty.state == PropertyNewValue)
				return;
		}

		gint64 left = end - g_get_monotonic_time();
		struct pollfd pfd = { ConnectionNumber(d), POLLIN, 0 };

		if (left <= 0)
			return;
		poll(&pfd, 1, (int)(left / 1000) + 1);
	}
}

/* --restore: the saved size, where the driver offers it and the server is
 * not already in it.  Quietly does nothing otherwise. */
static int restore(void)
{
	struct output o;
	unsigned w, h;
	const struct size *s;
	Display *dpy;
	int rc = 0;

	if (!saved_size(&w, &h))
		return 0;
	dpy = XOpenDisplay(NULL);
	if (!dpy)
		return 1;
	if (output_probe(&o, dpy) == 0 && (o.cur_w != w || o.cur_h != h) &&
	    (s = output_find(&o, w, h)) != NULL)
		rc = output_set(&o, s) == 0 ? 0 : 1;
	if (o.res)
		XRRFreeScreenResources(o.res);
	XCloseDisplay(dpy);
	return rc;
}

/* A size change in the running session: the desktop bar out of sight while
 * the screen changes and the window manager restarts on the new size, and
 * back only once the manager is there to map it. */
static int switch_size(struct output *o, const struct size *s)
{
	Display *bar = panel_hide();
	int rc = output_set(o, s);

	if (rc == 0 && restart_wm() && bar)
		wait_for_wm(bar);
	panel_show(bar);
	return rc;
}

/* ---- the dialog ---- */

enum { COL_LABEL, COL_W, COL_H, NCOLS };

static struct {
	struct output o;
	gboolean switchable; /* RandR can set the size; else the next boot */
	GtkWidget *win, *view;
	GtkWidget *confirm;
	guint countdown;
	int seconds;
	struct size prev;
} D;

static void message(GtkMessageType type, const char *primary,
		    const char *secondary)
{
	GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(D.win),
					      GTK_DIALOG_MODAL, type,
					      GTK_BUTTONS_OK, "%s", primary);

	if (secondary)
		gtk_message_dialog_format_secondary_text(
			GTK_MESSAGE_DIALOG(m), "%s", secondary);
	gtk_window_set_title(GTK_WINDOW(m), "Display settings");
	gtk_dialog_run(GTK_DIALOG(m));
	gtk_widget_destroy(m);
}

static void save_or_complain(unsigned w, unsigned h)
{
	GError *err = NULL;

	if (!save_size(w, h, &err)) {
		message(GTK_MESSAGE_ERROR,
			"The resolution could not be saved for the next start.",
			err ? err->message : NULL);
		g_clear_error(&err);
	}
}

static void confirm_text(void)
{
	gtk_message_dialog_format_secondary_text(
		GTK_MESSAGE_DIALOG(D.confirm),
		"The previous resolution, %ux%u, is restored in %d second%s.",
		D.prev.w, D.prev.h, D.seconds, D.seconds == 1 ? "" : "s");
}

static gboolean on_countdown(gpointer data)
{
	(void)data;
	if (--D.seconds <= 0) {
		D.countdown = 0;
		gtk_dialog_response(GTK_DIALOG(D.confirm), GTK_RESPONSE_REJECT);
		return G_SOURCE_REMOVE;
	}
	confirm_text();
	return G_SOURCE_CONTINUE;
}

/* Switched to `s' and saved it: keep it, or go back to D.prev. */
static void ask_keep(const struct size *s)
{
	gint r;

	D.confirm = gtk_message_dialog_new(GTK_WINDOW(D.win), GTK_DIALOG_MODAL,
					   GTK_MESSAGE_QUESTION,
					   GTK_BUTTONS_NONE,
					   "Keep this resolution (%ux%u)?",
					   s->w, s->h);
	gtk_window_set_title(GTK_WINDOW(D.confirm), "Display settings");
	gtk_dialog_add_buttons(GTK_DIALOG(D.confirm),
			       "Switch back to previous resolution",
			       GTK_RESPONSE_REJECT, "Keep resolution",
			       GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(D.confirm),
					GTK_RESPONSE_REJECT);
	gtk_window_set_position(GTK_WINDOW(D.confirm), GTK_WIN_POS_CENTER);
	D.seconds = REVERT_SECONDS;
	confirm_text();
	D.countdown = g_timeout_add_seconds(1, on_countdown, NULL);
	r = gtk_dialog_run(GTK_DIALOG(D.confirm));
	if (D.countdown) {
		g_source_remove(D.countdown);
		D.countdown = 0;
	}
	gtk_widget_destroy(D.confirm);
	D.confirm = NULL;
	if (r == GTK_RESPONSE_ACCEPT)
		return;

	if (D.prev.mode != None && switch_size(&D.o, &D.prev) == 0) {
		save_or_complain(D.prev.w, D.prev.h);
	} else {
		message(GTK_MESSAGE_ERROR,
			"The previous resolution could not be restored.", NULL);
	}
}

static gboolean selected_size(unsigned *w, unsigned *h)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(D.view));
	GtkTreeModel *model;
	GtkTreeIter it;

	if (!gtk_tree_selection_get_selected(sel, &model, &it))
		return FALSE;
	gtk_tree_model_get(model, &it, COL_W, w, COL_H, h, -1);
	return TRUE;
}

static void on_set(void)
{
	unsigned w, h;

	if (!selected_size(&w, &h))
		return;

	if (!D.switchable) {
		GError *err = NULL;

		if (!save_size(w, h, &err)) {
			message(GTK_MESSAGE_ERROR,
				"The resolution could not be saved.",
				err ? err->message : NULL);
			g_clear_error(&err);
			return;
		}
		char text[128];

		snprintf(text, sizeof(text),
			 "The resolution %ux%u is set when the system starts "
			 "the next time.",
			 w, h);
		message(GTK_MESSAGE_INFO,
			"Restart the system for the new resolution to take "
			"effect.",
			text);
		gtk_widget_destroy(D.win);
		return;
	}

	const struct size *s = output_find(&D.o, w, h);
	const struct size *cur = output_find(&D.o, D.o.cur_w, D.o.cur_h);

	if (!s)
		return;
	if (w == D.o.cur_w && h == D.o.cur_h) {
		/* Already on the screen; only remember it. */
		save_or_complain(w, h);
		gtk_widget_destroy(D.win);
		return;
	}
	D.prev = cur ? *cur : (struct size){ D.o.cur_w, D.o.cur_h, None };
	gtk_widget_hide(D.win);
	if (switch_size(&D.o, s) != 0) {
		gtk_widget_show(D.win);
		message(GTK_MESSAGE_ERROR,
			"The display driver did not accept this resolution.",
			NULL);
		return;
	}
	save_or_complain(w, h);
	ask_keep(s);
	gtk_widget_destroy(D.win);
}

static void on_response(GtkDialog *dlg, gint r, gpointer data)
{
	(void)dlg;
	(void)data;
	if (r == GTK_RESPONSE_OK)
		on_set();
	else
		gtk_widget_destroy(D.win);
}

static void on_row_activated(GtkTreeView *v, GtkTreePath *p,
			     GtkTreeViewColumn *c, gpointer data)
{
	(void)v;
	(void)p;
	(void)c;
	(void)data;
	on_set();
}

/* The framebuffer's sizes: what the boot firmware offers, largest first;
 * at least the one on the screen now. */
static void framebuffer_sizes(void)
{
	char *text = NULL;

	D.o.nsizes = 0;
	if (g_file_get_contents(GOP_MODES, &text, NULL, NULL)) {
		char *line = text, *next;

		for (; line && *line && D.o.nsizes < MAX_SIZES; line = next) {
			unsigned w, h;

			next = strchr(line, '\n');
			if (next)
				*next++ = '\0';
			if (sscanf(line, "%ux%u", &w, &h) == 2 &&
			    w >= 640 && h >= 480 && !output_find(&D.o, w, h)) {
				D.o.sizes[D.o.nsizes].w = w;
				D.o.sizes[D.o.nsizes].h = h;
				D.o.sizes[D.o.nsizes].mode = None;
				D.o.nsizes++;
			}
		}
		g_free(text);
	}
	if (!output_find(&D.o, D.o.cur_w, D.o.cur_h) &&
	    D.o.nsizes < MAX_SIZES) {
		D.o.sizes[D.o.nsizes].w = D.o.cur_w;
		D.o.sizes[D.o.nsizes].h = D.o.cur_h;
		D.o.sizes[D.o.nsizes].mode = None;
		D.o.nsizes++;
	}
	qsort(D.o.sizes, D.o.nsizes, sizeof(D.o.sizes[0]), size_cmp);
}

static void build_window(void)
{
	unsigned sw = 0, sh = 0, mark_w, mark_h;
	gboolean have_saved = saved_size(&sw, &sh);
	GtkListStore *store = gtk_list_store_new(NCOLS, G_TYPE_STRING,
						 G_TYPE_UINT, G_TYPE_UINT);
	GtkTreeIter it, select_it;
	gboolean have_select = FALSE;

	/* The row selected at first: the size on the screen -- on the
	 * framebuffer the saved size, which is what the next start sets. */
	mark_w = D.o.cur_w;
	mark_h = D.o.cur_h;
	if (!D.switchable && have_saved && output_find(&D.o, sw, sh)) {
		mark_w = sw;
		mark_h = sh;
	}
	for (int i = 0; i < D.o.nsizes; i++) {
		char label[64];
		const struct size *s = &D.o.sizes[i];

		if (s->w == D.o.cur_w && s->h == D.o.cur_h)
			snprintf(label, sizeof(label), "%u \xc3\x97 %u (current)",
				 s->w, s->h);
		else if (!D.switchable && have_saved && s->w == sw &&
			 s->h == sh)
			snprintf(label, sizeof(label),
				 "%u \xc3\x97 %u (after restart)", s->w, s->h);
		else
			snprintf(label, sizeof(label), "%u \xc3\x97 %u", s->w,
				 s->h);
		gtk_list_store_append(store, &it);
		gtk_list_store_set(store, &it, COL_LABEL, label, COL_W, s->w,
				   COL_H, s->h, -1);
		if (s->w == mark_w && s->h == mark_h) {
			select_it = it;
			have_select = TRUE;
		}
	}

	D.win = gtk_dialog_new_with_buttons("Display settings", NULL, 0,
					    "Cancel", GTK_RESPONSE_CANCEL,
					    "Set resolution", GTK_RESPONSE_OK,
					    NULL);
	gtk_window_set_icon_name(GTK_WINDOW(D.win),
				 "preferences-desktop-display");
	gtk_window_set_position(GTK_WINDOW(D.win), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(D.win), 340, 420);
	gtk_dialog_set_default_response(GTK_DIALOG(D.win), GTK_RESPONSE_OK);

	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(D.win));
	GtkWidget *label = gtk_label_new("Resolution:");
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *note = gtk_label_new(
		D.switchable ?
			"The resolution changes at once; you are asked "
			"whether to keep it." :
			"The display runs on the boot framebuffer: a new "
			"resolution takes effect after a restart.");

	gtk_container_set_border_width(GTK_CONTAINER(box), 10);
	gtk_box_set_spacing(GTK_BOX(box), 6);
	gtk_widget_set_halign(label, GTK_ALIGN_START);
	gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);

	D.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(D.view), FALSE);
	gtk_tree_view_append_column(
		GTK_TREE_VIEW(D.view),
		gtk_tree_view_column_new_with_attributes(
			"Resolution", gtk_cell_renderer_text_new(), "text",
			COL_LABEL, NULL));
	g_signal_connect(D.view, "row-activated",
			 G_CALLBACK(on_row_activated), NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
					    GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), D.view);

	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

	if (have_select) {
		GtkTreePath *p = gtk_tree_model_get_path(
			gtk_tree_view_get_model(GTK_TREE_VIEW(D.view)),
			&select_it);

		gtk_tree_selection_select_iter(
			gtk_tree_view_get_selection(GTK_TREE_VIEW(D.view)),
			&select_it);
		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(D.view), p, NULL,
					     TRUE, 0.5, 0);
		gtk_tree_path_free(p);
	}

	g_signal_connect(D.win, "response", G_CALLBACK(on_response), NULL);
	g_signal_connect(D.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(D.win);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--restore") == 0)
		return restore();

	gtk_init(&argc, &argv);

	Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());

	/* A driver that can switch offers more than one size; the fbdev
	 * driver's RandR has the one mode it runs. */
	D.switchable = output_probe(&D.o, dpy) == 0 && D.o.nsizes > 1;
	if (!D.switchable) {
		D.o.cur_w = DisplayWidth(dpy, DefaultScreen(dpy));
		D.o.cur_h = DisplayHeight(dpy, DefaultScreen(dpy));
		framebuffer_sizes();
	}
	build_window();
	gtk_main();
	return 0;
}
