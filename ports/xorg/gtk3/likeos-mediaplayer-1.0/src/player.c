// LikeOS -- Media Player: the GStreamer engine.
//
// playbin does the real work: it finds a source for the URI, a demuxer for
// the container, decoders for the streams and sinks for the output.  This
// file chooses the video sink, keeps track of playbin's asynchronous state
// and turns its bus messages into the callbacks in player.h.
//
// VIDEO OUTPUT.  Three ways to get pictures into the window, tried in this
// order when the setting is "auto":
//
//   xvimagesink  The X video extension: the server (or the GPU behind it)
//                converts and scales.  Cheapest by far, and what the vmwgfx
//                overlay and the i915 sprite planes provide.  Rendered into
//                a GtkDrawingArea's own X window through GstVideoOverlay.
//                A server without an Xv adaptor fails the READY transition,
//                which is how its absence is detected.
//   gtksink      Frames painted by GTK with cairo.  Works everywhere GTK
//                does, scales in software.
//   ximagesink   Plain X images through GstVideoOverlay; does not scale,
//                so it is the last resort.
//
// "gl" (gtkglsink inside glsinkbin) is available on request only: it needs
// a GL context GDK can share, which not every machine here provides.
//
// THREADS.  The bus watch runs on the default main context, so every bus
// message -- and so every callback -- arrives on the GTK thread.  playbin's
// own signals ("video-changed", "about-to-finish") come from streaming
// threads; the change signals post an application message onto the bus so
// they are handled on the GTK thread too, and about-to-finish only touches
// the next-URI slot, under a mutex.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <gst/audio/streamvolume.h>
#include <gst/pbutils/pbutils.h>
#include <gst/tag/tag.h>
#include <gst/video/videooverlay.h>
#include <gst/video/video.h>

#include "player.h"

/* After player.h: GDK_WINDOWING_X11 comes from gtk.h's gdkconfig.h. */
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

/* playbin's "flags" bits (GstPlayFlags is private to the plug-in). */
enum {
	PLAY_FLAG_VIDEO = 1 << 0,
	PLAY_FLAG_AUDIO = 1 << 1,
	PLAY_FLAG_TEXT = 1 << 2,
	PLAY_FLAG_VIS = 1 << 3,
	PLAY_FLAG_SOFT_VOLUME = 1 << 4,
	PLAY_FLAG_DEINTERLACE = 1 << 9,
};

#define MSG_STREAMS_CHANGED "mediaplayer-streams-changed"

/* A flushing seek ends with an ASYNC_DONE.  Should that never come (an
 * element that drops the seek half-way), the next one waits this long at
 * most. */
#define SEEK_GUARD_MS 1500

/* After a gapless switch the new entry's duration and seekability are asked
 * for again this often, this many times, until both are known: there is no
 * ASYNC_DONE to wait for, and not every demuxer posts DURATION_CHANGED. */
#define TIMING_POLL_MS 250
#define TIMING_POLL_TRIES 20

struct MpPlayer {
	GstElement *playbin;
	GstElement *video_sink;	/* the element given to playbin */
	GstElement *overlay_sink; /* the GstVideoOverlay inside it, if any */
	GtkWidget *video_widget;
	gchar *video_sink_name;
	gboolean no_video;
	gpointer window_handle;	/* guintptr, read from streaming threads */

	MpPlayerCallbacks cb;
	gpointer data;
	guint bus_watch;

	gchar *uri;
	GstState target;	/* NULL, PAUSED or PLAYING */
	MpPlayerState state;
	gboolean is_live;
	gboolean buffering;
	gboolean prerolled;
	gboolean seekable;
	gint64 duration;
	gint64 pending_seek;	/* -1 for none: issued at the next ASYNC_DONE */
	gboolean pending_seek_accurate;
	gboolean seek_busy;	/* a flushing seek has not completed yet */
	gint64 seek_target;	/* where the latest seek goes, -1 for none */
	guint seek_guard;	/* SEEK_GUARD_MS timeout while seek_busy */
	guint timing_poll;	/* TIMING_POLL_MS timeout, see above */
	guint timing_tries;
	double rate;
	gboolean rate_pending;
	gboolean subtitles;
	double volume;
	gboolean mute;

	GstTagList *tags;
	GPtrArray *missing;	/* gchar* descriptions, NULL-terminated */

	int n_video, n_audio, n_text;

	GMutex lock;		/* guards the three fields below */
	gchar *next_uri;
	gchar *switched_uri;
	gboolean gapless_pending;
	gboolean gapless;
};

/* ---- sinks ------------------------------------------------------------ */

static gboolean sink_reaches_ready(GstElement *sink)
{
	/* READY is where the X sinks open the display and look for what they
	 * need (xvimagesink: an adaptor that takes a format it can use).
	 * Going there and back costs a connection, nothing more. */
	GstStateChangeReturn r = gst_element_set_state(sink, GST_STATE_READY);
	if (r == GST_STATE_CHANGE_ASYNC)
		r = gst_element_get_state(sink, NULL, NULL, 2 * GST_SECOND);
	gst_element_set_state(sink, GST_STATE_NULL);
	return r != GST_STATE_CHANGE_FAILURE;
}

static void set_if_has(GstElement *e, const char *prop, gboolean v)
{
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop) != NULL)
		g_object_set(e, prop, v, NULL);
}

/* ---- decoders and the video sink -------------------------------------- */

/* How many threads a software video decoder may use.
 *
 * The libav decoders default to one thread per processor, and with frame
 * threading every one of them holds a frame of its own: the picture is
 * handed from thread to thread and comes out as many frames late as there
 * are threads.  On eight processors that is eight frames in flight and
 * eight wakeups per picture, each of which has to find a processor that is
 * free; on a virtual machine whose processors are not all scheduled at
 * once, a wakeup that waits makes the whole pipeline wait, the video queue
 * fills, the demuxer stops feeding the sound, and picture and sound stutter
 * together.  The web browser's engine plays the same files through the same
 * decoders smoothly because it fixes the count at two; this does the same.
 * Two threads keep a 1080p stream decoding faster than real time on any
 * machine this runs on. */
#define MP_VIDEO_DECODER_THREADS 2

/* How late a frame may be and still be shown.  Later ones are dropped, and
 * the quality messages (qos) tell the decoder to skip work it would only
 * throw away -- which is how a machine that falls behind for a moment
 * catches up instead of drifting from the sound. */
#define MP_VIDEO_MAX_LATENESS (100 * GST_MSECOND)

static gboolean has_prop(GstElement *e, const char *prop)
{
	return g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop) != NULL;
}

/* playbin's "element-setup": every element it creates passes through here
 * before it leaves the NULL state.  Runs on whichever thread is building
 * the pipeline; it only sets properties on the new element. */
static void on_element_setup(GstElement *pb, GstElement *e, gpointer data)
{
	(void)pb;
	(void)data;

	GstElementFactory *f = gst_element_get_factory(e);
	if (f == NULL)
		return;
	const char *klass = gst_element_factory_get_metadata(
		f, GST_ELEMENT_METADATA_KLASS);
	const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));

	if (klass == NULL || name == NULL || strstr(klass, "Decoder") == NULL ||
	    strstr(klass, "Video") == NULL)
		return;
	if (g_str_has_prefix(name, "avdec_") && has_prop(e, "max-threads"))
		g_object_set(e, "max-threads", MP_VIDEO_DECODER_THREADS, NULL);
	/* Frames decoded with reference pictures missing are dropped, not
	 * shown.  Right after a seek to a keyframe the pictures that follow it
	 * on screen may still refer to the group before it, and shown they
	 * are the grey blocks that smear the picture for a second or two; the
	 * last good frame stays up instead until clean ones arrive. */
	if (g_str_has_prefix(name, "avdec_") && has_prop(e, "output-corrupt"))
		g_object_set(e, "output-corrupt", FALSE, NULL);
	/* "max-errors" stays at its default.  Zero would make the FIRST
	 * undecodable packet end playback -- and right after a seek the
	 * decoder routinely gets a packet or two it cannot decode before the
	 * next keyframe, which the default tolerates. */
}

/* The element that really renders: inside a bin (glsinkbin) it is the
 * bin's sink. */
static GstElement *innermost_sink(GstElement *sink)
{
	GstElement *e = gst_object_ref(sink);

	while (GST_IS_BIN(e)) {
		GstIterator *it = gst_bin_iterate_sinks(GST_BIN(e));
		GValue v = G_VALUE_INIT;
		GstElement *inner = NULL;

		if (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
			inner = gst_object_ref(g_value_get_object(&v));
			g_value_unset(&v);
		}
		gst_iterator_free(it);
		if (inner == NULL)
			break;
		gst_object_unref(e);
		e = inner;
	}
	return e;
}

static void configure_video_sink(GstElement *sink)
{
	GstElement *e = innermost_sink(sink);

	if (has_prop(e, "qos"))
		g_object_set(e, "qos", TRUE, NULL);
	if (has_prop(e, "max-lateness"))
		g_object_set(e, "max-lateness", (gint64)MP_VIDEO_MAX_LATENESS,
			     NULL);
	gst_object_unref(e);
}

static gboolean on_overlay_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	MpPlayer *p = data;

	/* While a frame is up, the sink owns these pixels: ask it to redraw
	 * the last frame (and pick up a new window size) instead of painting
	 * over it.  Otherwise the area is plain black. */
	if (p->overlay_sink != NULL && p->n_video > 0 &&
	    (p->state == MP_PLAYER_PAUSED || p->state == MP_PLAYER_PLAYING ||
	     (p->state == MP_PLAYER_LOADING && p->prerolled))) {
		gst_video_overlay_expose(GST_VIDEO_OVERLAY(p->overlay_sink));
		return FALSE;
	}
	GtkAllocation a;
	gtk_widget_get_allocation(w, &a);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, 0, a.width, a.height);
	cairo_fill(cr);
	return FALSE;
}

static void on_overlay_realize(GtkWidget *w, gpointer data)
{
	MpPlayer *p = data;
	GdkWindow *win = gtk_widget_get_window(w);

	/* The sink draws with X calls into an X window, so the area needs
	 * one of its own rather than a region of its toplevel's. */
	if (!gdk_window_ensure_native(win)) {
		g_warning("cannot give the video area a native window");
		return;
	}
#ifdef GDK_WINDOWING_X11
	if (GDK_IS_X11_WINDOW(win)) {
		guintptr xid = (guintptr)GDK_WINDOW_XID(win);
		g_atomic_pointer_set(&p->window_handle, (gpointer)xid);
		if (p->overlay_sink != NULL)
			gst_video_overlay_set_window_handle(
				GST_VIDEO_OVERLAY(p->overlay_sink), xid);
	}
#endif
}

static void on_overlay_unrealize(GtkWidget *w, gpointer data)
{
	MpPlayer *p = data;
	(void)w;
	/* The X window is about to go: the sink must not draw into it. */
	g_atomic_pointer_set(&p->window_handle, NULL);
	if (p->overlay_sink != NULL)
		gst_video_overlay_set_window_handle(
			GST_VIDEO_OVERLAY(p->overlay_sink), 0);
}

static void on_overlay_size_allocate(GtkWidget *w, GdkRectangle *a,
				     gpointer data)
{
	MpPlayer *p = data;
	(void)a;
	(void)w;
	if (p->overlay_sink != NULL && p->prerolled && p->n_video > 0)
		gst_video_overlay_expose(GST_VIDEO_OVERLAY(p->overlay_sink));
}

static GtkWidget *make_overlay_area(MpPlayer *p)
{
	GtkWidget *area = gtk_drawing_area_new();
	gtk_widget_set_app_paintable(area, TRUE);
	/* GTK must not paint this window through an offscreen buffer: the
	 * buffer would be copied over the sink's frame on every redraw. */
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_widget_set_double_buffered(area, FALSE);
	G_GNUC_END_IGNORE_DEPRECATIONS
	gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK |
					    GDK_BUTTON_RELEASE_MASK |
					    GDK_POINTER_MOTION_MASK |
					    GDK_SCROLL_MASK |
					    GDK_SMOOTH_SCROLL_MASK);
	g_signal_connect(area, "draw", G_CALLBACK(on_overlay_draw), p);
	g_signal_connect(area, "realize", G_CALLBACK(on_overlay_realize), p);
	g_signal_connect(area, "unrealize", G_CALLBACK(on_overlay_unrealize),
			 p);
	g_signal_connect_after(area, "size-allocate",
			       G_CALLBACK(on_overlay_size_allocate), p);
	return area;
}

static gboolean try_overlay_sink(MpPlayer *p, const char *factory)
{
	GstElement *sink = gst_element_factory_make(factory, NULL);
	if (sink == NULL)
		return FALSE;
	gst_object_ref_sink(sink);
	if (!GST_IS_VIDEO_OVERLAY(sink) || !sink_reaches_ready(sink)) {
		gst_object_unref(sink);
		return FALSE;
	}
	set_if_has(sink, "force-aspect-ratio", TRUE);
	/* GTK handles the window's input; the sink selecting button presses
	 * on the same window would take them away from it. */
	gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), FALSE);

	p->video_sink = sink;
	p->overlay_sink = gst_object_ref(sink);
	p->video_widget = g_object_ref_sink(make_overlay_area(p));
	p->video_sink_name = g_strdup(factory);
	return TRUE;
}

static gboolean try_gtk_sink(MpPlayer *p, gboolean gl)
{
	GstElement *sink;
	GstElement *inner;

	if (gl) {
		inner = gst_element_factory_make("gtkglsink", NULL);
		if (inner == NULL)
			return FALSE;
		sink = gst_element_factory_make("glsinkbin", NULL);
		if (sink == NULL) {
			gst_object_unref(gst_object_ref_sink(inner));
			return FALSE;
		}
		g_object_set(sink, "sink", inner, NULL);
	} else {
		sink = gst_element_factory_make("gtksink", NULL);
		if (sink == NULL)
			return FALSE;
		inner = sink;
	}
	gst_object_ref_sink(sink);

	GtkWidget *widget = NULL;
	g_object_get(inner, "widget", &widget, NULL);
	if (widget == NULL) {
		gst_object_unref(sink);
		return FALSE;
	}
	if (gl && !sink_reaches_ready(sink)) {
		g_object_unref(widget);
		gst_object_unref(sink);
		return FALSE;
	}
	set_if_has(inner, "force-aspect-ratio", TRUE);
	gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK |
					      GDK_POINTER_MOTION_MASK |
					      GDK_SCROLL_MASK |
					      GDK_SMOOTH_SCROLL_MASK);
	p->video_sink = sink;
	p->video_widget = widget;	/* g_object_get gave us a reference */
	p->video_sink_name = g_strdup(gl ? "gtkglsink" : "gtksink");
	return TRUE;
}

static void choose_video_sink(MpPlayer *p, const char *pref)
{
	const char *order_auto[] = { "xv", "gtk", "x11", NULL };
	const char *one[] = { pref, NULL };
	const char *const *order = order_auto;

	if (pref != NULL && strcmp(pref, "auto") != 0)
		order = one;

	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && order == order_auto)
			break;
		if (pass == 1)
			order = order_auto;
		for (int i = 0; order[i] != NULL; i++) {
			const char *k = order[i];
			gboolean ok = FALSE;
			if (strcmp(k, "xv") == 0)
				ok = try_overlay_sink(p, "xvimagesink");
			else if (strcmp(k, "x11") == 0)
				ok = try_overlay_sink(p, "ximagesink");
			else if (strcmp(k, "gtk") == 0)
				ok = try_gtk_sink(p, FALSE);
			else if (strcmp(k, "gl") == 0)
				ok = try_gtk_sink(p, TRUE);
			if (ok)
				return;
			g_message("video output \"%s\" is not available", k);
		}
		/* An explicit choice that failed falls back to the automatic
		 * order (the second pass) rather than to no picture at all. */
	}

	/* Nothing could show pictures.  A fakesink keeps playbin from making
	 * a window of its own (autovideosink would), and the area stays
	 * black; the sound still plays. */
	g_warning("no video output is available; playing sound only");
	GstElement *fake = gst_element_factory_make("fakesink", NULL);
	if (fake != NULL) {
		g_object_set(fake, "sync", TRUE, NULL);
		p->video_sink = gst_object_ref_sink(fake);
	}
	p->video_widget = g_object_ref_sink(gtk_drawing_area_new());
	p->video_sink_name = g_strdup("none");
}

/* ---- state ------------------------------------------------------------ */

static void update_state(MpPlayer *p)
{
	GstState cur = GST_STATE_NULL;
	gst_element_get_state(p->playbin, &cur, NULL, 0);

	MpPlayerState s;
	if (p->target == GST_STATE_NULL || p->uri == NULL)
		s = MP_PLAYER_STOPPED;
	else if (p->buffering)
		s = MP_PLAYER_LOADING;
	else if (cur == GST_STATE_PLAYING)
		s = MP_PLAYER_PLAYING;
	else if (p->target == GST_STATE_PAUSED && cur == GST_STATE_PAUSED)
		s = MP_PLAYER_PAUSED;
	else
		s = MP_PLAYER_LOADING;

	if (s != p->state) {
		p->state = s;
		if (p->cb.state_changed != NULL)
			p->cb.state_changed(p, s, p->data);
		/* Paint the area black again when the picture goes away. */
		if (p->video_widget != NULL && s == MP_PLAYER_STOPPED)
			gtk_widget_queue_draw(p->video_widget);
	}
}

/* Ask for the duration and seekability, and tell the application when
 * either changed.  Both before the callback: a seek bar enabled from the
 * callback must see the seekability that goes with the duration.  (Asking
 * for the duration first and reporting it at once left the bar disabled
 * for good, since the seekability that arrived a moment later changed no
 * duration and so was never reported.) */
static void update_timing(MpPlayer *p)
{
	GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);
	gboolean seekable = FALSE;
	if (gst_element_query(p->playbin, q))
		gst_query_parse_seeking(q, NULL, &seekable, NULL, NULL);
	gst_query_unref(q);
	seekable = seekable && !p->is_live;

	gint64 d = -1;
	if (!gst_element_query_duration(p->playbin, GST_FORMAT_TIME, &d) ||
	    d <= 0)
		d = -1;

	if (d != p->duration || seekable != p->seekable) {
		p->duration = d;
		p->seekable = seekable;
		if (p->cb.duration != NULL)
			p->cb.duration(p, d, p->data);
	}
}

static gboolean timing_poll_cb(gpointer data)
{
	MpPlayer *p = data;
	update_timing(p);
	if ((p->duration > 0 && p->seekable) || p->is_live ||
	    p->target == GST_STATE_NULL || ++p->timing_tries >= TIMING_POLL_TRIES) {
		p->timing_poll = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void start_timing_poll(MpPlayer *p)
{
	p->timing_tries = 0;
	if (p->timing_poll == 0)
		p->timing_poll = g_timeout_add(TIMING_POLL_MS, timing_poll_cb, p);
}

static void stop_timing_poll(MpPlayer *p)
{
	if (p->timing_poll != 0) {
		g_source_remove(p->timing_poll);
		p->timing_poll = 0;
	}
}

static void count_streams(MpPlayer *p)
{
	gint nv = 0, na = 0, nt = 0;
	g_object_get(p->playbin, "n-video", &nv, "n-audio", &na, "n-text",
		     &nt, NULL);
	p->n_video = p->no_video ? 0 : nv;
	p->n_audio = na;
	p->n_text = nt;
}

static void reset_seek_state(MpPlayer *p)
{
	p->pending_seek = -1;
	p->seek_busy = FALSE;
	p->seek_target = -1;
	if (p->seek_guard != 0) {
		g_source_remove(p->seek_guard);
		p->seek_guard = 0;
	}
}

static void reset_stream_state(MpPlayer *p)
{
	p->prerolled = FALSE;
	p->seekable = FALSE;
	p->is_live = FALSE;
	p->buffering = FALSE;
	p->duration = -1;
	reset_seek_state(p);
	stop_timing_poll(p);
	p->n_video = p->n_audio = p->n_text = 0;
	g_clear_pointer(&p->tags, gst_tag_list_unref);
	g_ptr_array_set_size(p->missing, 0);
	g_ptr_array_add(p->missing, NULL);
	if (p->rate != 1.0)
		p->rate_pending = TRUE;
}

static const char *const *missing_list(MpPlayer *p)
{
	return (const char *const *)p->missing->pdata;
}

static void add_missing(MpPlayer *p, const char *desc)
{
	/* The array keeps a trailing NULL so it can be passed as a strv. */
	for (guint i = 0; i + 1 < p->missing->len; i++) {
		if (g_strcmp0(g_ptr_array_index(p->missing, i), desc) == 0)
			return;
	}
	if (p->missing->len > 32)
		return;
	g_ptr_array_index(p->missing, p->missing->len - 1) = g_strdup(desc);
	g_ptr_array_add(p->missing, NULL);
}

static void apply_pending(MpPlayer *p);

static gboolean seek_guard_cb(gpointer data)
{
	MpPlayer *p = data;
	p->seek_guard = 0;
	g_message("seek not completed after %d ms; going on", SEEK_GUARD_MS);
	p->seek_busy = FALSE;
	apply_pending(p);
	if (!p->seek_busy)
		p->seek_target = -1;
	return G_SOURCE_REMOVE;
}

/* Send one flushing seek (at the current rate) and note that it is under
 * way until its ASYNC_DONE. */
static gboolean issue_seek(MpPlayer *p, gint64 pos, gboolean accurate)
{
	GstSeekFlags fl = GST_SEEK_FLAG_FLUSH;
	fl |= accurate ? GST_SEEK_FLAG_ACCURATE
		       : GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST;
	if (!gst_element_seek(p->playbin, p->rate, GST_FORMAT_TIME, fl,
			      GST_SEEK_TYPE_SET, pos, GST_SEEK_TYPE_NONE,
			      GST_CLOCK_TIME_NONE)) {
		p->seek_target = -1;
		return FALSE;
	}
	p->rate_pending = FALSE;
	p->seek_busy = TRUE;
	p->seek_target = pos;
	if (p->seek_guard != 0)
		g_source_remove(p->seek_guard);
	p->seek_guard = g_timeout_add(SEEK_GUARD_MS, seek_guard_cb, p);
	return TRUE;
}

/* Issue what waited for the pipeline: a seek asked for before preroll or
 * during another seek, or a rate change. */
static void apply_pending(MpPlayer *p)
{
	if (p->pending_seek < 0 && !p->rate_pending)
		return;
	gint64 pos = p->pending_seek;
	gboolean accurate = p->pending_seek_accurate || p->rate_pending;
	if (pos < 0 && !gst_element_query_position(p->playbin, GST_FORMAT_TIME,
						   &pos))
		pos = 0;
	p->pending_seek = -1;
	if (p->seekable)
		issue_seek(p, pos, accurate);
	else
		p->seek_target = -1;
	p->rate_pending = FALSE;
}

static void seek_done(MpPlayer *p)
{
	p->seek_busy = FALSE;
	if (p->seek_guard != 0) {
		g_source_remove(p->seek_guard);
		p->seek_guard = 0;
	}
	apply_pending(p);
	if (!p->seek_busy)
		p->seek_target = -1;
}

/* ---- bus -------------------------------------------------------------- */

static void handle_error(MpPlayer *p, GstMessage *msg)
{
	GError *err = NULL;
	gchar *dbg = NULL;
	gst_message_parse_error(msg, &err, &dbg);

	gchar *src = gst_object_get_path_string(GST_MESSAGE_SRC(msg));
	gchar *details = g_strdup_printf("%s\n\n%s%s%s", src,
					 err ? err->message : "",
					 dbg ? "\n\n" : "", dbg ? dbg : "");
	g_warning("playback error from %s: %s", src,
		  err ? err->message : "?");

	/* Stop where we are: a pipeline in error stays in error. */
	gst_element_set_state(p->playbin, GST_STATE_NULL);
	p->target = GST_STATE_NULL;
	p->buffering = FALSE;
	reset_seek_state(p);
	stop_timing_poll(p);
	update_state(p);

	if (p->cb.error != NULL)
		p->cb.error(p, err, details, missing_list(p), p->data);
	g_free(details);
	g_free(src);
	g_free(dbg);
	g_clear_error(&err);
}

static void handle_warning(MpPlayer *p, GstMessage *msg)
{
	GError *err = NULL;
	gchar *dbg = NULL;
	gst_message_parse_warning(msg, &err, &dbg);
	g_message("playback warning: %s (%s)", err ? err->message : "?",
		  dbg ? dbg : "");
	if (p->cb.warning != NULL && err != NULL)
		p->cb.warning(p, err->message, missing_list(p), p->data);
	g_free(dbg);
	g_clear_error(&err);
}

static void handle_tags(MpPlayer *p, GstMessage *msg)
{
	GstTagList *tl = NULL;
	gst_message_parse_tag(msg, &tl);
	if (tl == NULL)
		return;
	if (p->tags == NULL) {
		p->tags = gst_tag_list_copy(tl);
	} else {
		GstTagList *merged = gst_tag_list_merge(
			p->tags, tl, GST_TAG_MERGE_REPLACE);
		gst_tag_list_unref(p->tags);
		p->tags = merged;
	}
	gst_tag_list_unref(tl);
	if (p->cb.tags != NULL)
		p->cb.tags(p, p->data);
}

static void handle_buffering(MpPlayer *p, GstMessage *msg)
{
	if (p->is_live)
		return;	/* live sources buffer as they please */
	gint pct = 100;
	gst_message_parse_buffering(msg, &pct);

	if (pct < 100) {
		if (!p->buffering) {
			p->buffering = TRUE;
			if (p->target == GST_STATE_PLAYING)
				gst_element_set_state(p->playbin,
						      GST_STATE_PAUSED);
		}
	} else if (p->buffering) {
		p->buffering = FALSE;
		if (p->target == GST_STATE_PLAYING)
			gst_element_set_state(p->playbin, GST_STATE_PLAYING);
	}
	update_state(p);
	if (p->cb.buffering != NULL)
		p->cb.buffering(p, pct, p->data);
}

static void handle_stream_start(MpPlayer *p)
{
	gchar *uri = NULL;
	g_mutex_lock(&p->lock);
	if (p->gapless_pending) {
		p->gapless_pending = FALSE;
		uri = p->switched_uri;
		p->switched_uri = NULL;
		/* The slot is consumed; the application names the one after
		 * this when it hears about the switch. */
		g_clear_pointer(&p->next_uri, g_free);
	}
	g_mutex_unlock(&p->lock);

	if (uri == NULL)
		return;

	g_free(p->uri);
	p->uri = uri;
	g_clear_pointer(&p->tags, gst_tag_list_unref);
	g_ptr_array_set_size(p->missing, 0);
	g_ptr_array_add(p->missing, NULL);
	p->duration = -1;
	p->seekable = FALSE;
	count_streams(p);
	/* The application moves to the new entry first, so the timing
	 * reported next belongs to it and not to the one that ended. */
	if (p->cb.track_advanced != NULL)
		p->cb.track_advanced(p, uri, p->data);
	if (p->cb.streams != NULL)
		p->cb.streams(p, p->data);
	update_timing(p);
	if (p->duration <= 0 || !p->seekable)
		start_timing_poll(p);
}

static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
	MpPlayer *p = data;
	(void)bus;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR:
		handle_error(p, msg);
		break;
	case GST_MESSAGE_WARNING:
		handle_warning(p, msg);
		break;
	case GST_MESSAGE_EOS:
		/* Leave the pipeline where it is (the last video frame stays
		 * up); the application decides what comes next. */
		if (p->cb.eos != NULL)
			p->cb.eos(p, p->data);
		break;
	case GST_MESSAGE_STATE_CHANGED:
		if (GST_MESSAGE_SRC(msg) == GST_OBJECT(p->playbin))
			update_state(p);
		break;
	case GST_MESSAGE_ASYNC_DONE:
		/* Preroll is complete: after loading, or after a flushing
		 * seek, which this ends. */
		p->prerolled = TRUE;
		update_timing(p);
		seek_done(p);
		update_state(p);
		break;
	case GST_MESSAGE_DURATION_CHANGED:
		update_timing(p);
		break;
	case GST_MESSAGE_TAG:
		handle_tags(p, msg);
		break;
	case GST_MESSAGE_BUFFERING:
		handle_buffering(p, msg);
		break;
	case GST_MESSAGE_CLOCK_LOST:
		/* The clock provider went away (an audio sink was swapped):
		 * a pause and resume makes the pipeline pick a new one. */
		if (p->target == GST_STATE_PLAYING) {
			gst_element_set_state(p->playbin, GST_STATE_PAUSED);
			gst_element_set_state(p->playbin, GST_STATE_PLAYING);
		}
		break;
	case GST_MESSAGE_LATENCY:
		gst_bin_recalculate_latency(GST_BIN(p->playbin));
		break;
	case GST_MESSAGE_REQUEST_STATE: {
		GstState st;
		gst_message_parse_request_state(msg, &st);
		gst_element_set_state(p->playbin, st);
		break;
	}
	case GST_MESSAGE_STREAM_START:
		handle_stream_start(p);
		break;
	case GST_MESSAGE_ELEMENT:
		if (gst_is_missing_plugin_message(msg)) {
			gchar *d = gst_missing_plugin_message_get_description(
				msg);
			if (d != NULL)
				add_missing(p, d);
			g_free(d);
		}
		break;
	case GST_MESSAGE_APPLICATION: {
		const GstStructure *s = gst_message_get_structure(msg);
		if (s != NULL &&
		    gst_structure_has_name(s, MSG_STREAMS_CHANGED)) {
			count_streams(p);
			if (p->cb.streams != NULL)
				p->cb.streams(p, p->data);
			if (p->video_widget != NULL)
				gtk_widget_queue_draw(p->video_widget);
		}
		break;
	}
	default:
		break;
	}
	return G_SOURCE_CONTINUE;
}

/* Runs on whichever thread posted the message. */
static GstBusSyncReply bus_sync_cb(GstBus *bus, GstMessage *msg,
				   gpointer data)
{
	MpPlayer *p = data;
	(void)bus;

	if (!gst_is_video_overlay_prepare_window_handle_message(msg))
		return GST_BUS_PASS;
	gpointer h = g_atomic_pointer_get(&p->window_handle);
	if (h != NULL)
		gst_video_overlay_set_window_handle(
			GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)), (guintptr)h);
	gst_message_unref(msg);
	return GST_BUS_DROP;
}

/* Streaming thread: hand the change over to the bus. */
static void on_streams_changed(GstElement *pb, gpointer data)
{
	(void)data;
	gst_element_post_message(
		pb, gst_message_new_application(
			    GST_OBJECT(pb),
			    gst_structure_new_empty(MSG_STREAMS_CHANGED)));
}

static void on_streams_changed_idx(GstElement *pb, gint idx, gpointer data)
{
	(void)idx;
	on_streams_changed(pb, data);
}

/* Streaming thread: the current stream's data has all been read.  Naming
 * the next URI now makes playbin continue into it without a gap. */
static void on_about_to_finish(GstElement *pb, gpointer data)
{
	MpPlayer *p = data;
	g_mutex_lock(&p->lock);
	if (p->gapless && p->next_uri != NULL && !p->gapless_pending) {
		g_object_set(pb, "uri", p->next_uri, NULL);
		g_free(p->switched_uri);
		p->switched_uri = g_strdup(p->next_uri);
		p->gapless_pending = TRUE;
	}
	g_mutex_unlock(&p->lock);
}

/* ---- construction ----------------------------------------------------- */

MpPlayer *mp_player_new(const MpPlayerCallbacks *cb, gpointer data,
			const char *video_sink, gboolean no_video,
			gboolean no_audio, GError **error)
{
	GstElement *pb = gst_element_factory_make("playbin", "player");
	if (pb == NULL) {
		g_set_error(error, GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN,
			    "The GStreamer element \"playbin\" is missing. "
			    "Check that gst-plugins-base is installed.");
		return NULL;
	}

	MpPlayer *p = g_new0(MpPlayer, 1);
	p->playbin = gst_object_ref_sink(pb);
	p->cb = *cb;
	p->data = data;
	p->no_video = no_video;
	p->target = GST_STATE_NULL;
	p->state = MP_PLAYER_STOPPED;
	p->duration = -1;
	p->pending_seek = -1;
	p->seek_target = -1;
	p->rate = 1.0;
	p->volume = 1.0;
	p->subtitles = TRUE;
	p->gapless = TRUE;
	p->missing = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(p->missing, NULL);
	g_mutex_init(&p->lock);

	guint flags = 0;
	g_object_get(pb, "flags", &flags, NULL);
	flags |= PLAY_FLAG_AUDIO | PLAY_FLAG_TEXT | PLAY_FLAG_SOFT_VOLUME |
		 PLAY_FLAG_DEINTERLACE;
	flags &= ~(guint)PLAY_FLAG_VIS;
	if (no_video)
		flags &= ~(guint)(PLAY_FLAG_VIDEO | PLAY_FLAG_TEXT);
	else
		flags |= PLAY_FLAG_VIDEO;
	/* No sound at all: no audio decoder, no sink, and the pipeline runs
	 * on the system clock instead of the sound card's. */
	if (no_audio)
		flags &= ~(guint)(PLAY_FLAG_AUDIO | PLAY_FLAG_SOFT_VOLUME);
	g_object_set(pb, "flags", flags, NULL);

	/* Speeds other than 1x keep their pitch through scaletempo, which
	 * passes the samples through untouched at normal speed. */
	g_signal_connect(pb, "element-setup", G_CALLBACK(on_element_setup),
			 NULL);

	GstElement *tempo = gst_element_factory_make("scaletempo", NULL);
	if (tempo != NULL)
		g_object_set(pb, "audio-filter", tempo, NULL);

	if (!no_video) {
		choose_video_sink(p, video_sink);
		if (p->video_sink != NULL) {
			configure_video_sink(p->video_sink);
			g_object_set(pb, "video-sink", p->video_sink, NULL);
		}
		g_message("video output: %s", p->video_sink_name);
	}

	GstBus *bus = gst_element_get_bus(pb);
	gst_bus_set_sync_handler(bus, bus_sync_cb, p, NULL);
	p->bus_watch = gst_bus_add_watch(bus, bus_cb, p);
	gst_object_unref(bus);

	g_signal_connect(pb, "video-changed", G_CALLBACK(on_streams_changed),
			 p);
	g_signal_connect(pb, "audio-changed", G_CALLBACK(on_streams_changed),
			 p);
	g_signal_connect(pb, "text-changed", G_CALLBACK(on_streams_changed),
			 p);
	g_signal_connect(pb, "video-tags-changed",
			 G_CALLBACK(on_streams_changed_idx), p);
	g_signal_connect(pb, "audio-tags-changed",
			 G_CALLBACK(on_streams_changed_idx), p);
	g_signal_connect(pb, "about-to-finish",
			 G_CALLBACK(on_about_to_finish), p);
	return p;
}

void mp_player_free(MpPlayer *p)
{
	if (p == NULL)
		return;
	gst_element_set_state(p->playbin, GST_STATE_NULL);
	reset_seek_state(p);
	stop_timing_poll(p);
	if (p->bus_watch != 0)
		g_source_remove(p->bus_watch);
	GstBus *bus = gst_element_get_bus(p->playbin);
	gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
	gst_object_unref(bus);
	gst_object_unref(p->playbin);
	if (p->video_sink != NULL)
		gst_object_unref(p->video_sink);
	if (p->overlay_sink != NULL)
		gst_object_unref(p->overlay_sink);
	if (p->video_widget != NULL)
		g_object_unref(p->video_widget);
	g_clear_pointer(&p->tags, gst_tag_list_unref);
	g_ptr_array_free(p->missing, TRUE);
	g_mutex_clear(&p->lock);
	g_free(p->next_uri);
	g_free(p->switched_uri);
	g_free(p->video_sink_name);
	g_free(p->uri);
	g_free(p);
}

GtkWidget *mp_player_get_video_widget(MpPlayer *p)
{
	return p->video_widget;
}

const char *mp_player_get_video_sink_name(MpPlayer *p)
{
	return p->video_sink_name ? p->video_sink_name : "none";
}

/* ---- transport -------------------------------------------------------- */

static void set_target(MpPlayer *p, GstState target)
{
	p->target = target;
	GstStateChangeReturn r = gst_element_set_state(
		p->playbin, p->buffering && target == GST_STATE_PLAYING
				    ? GST_STATE_PAUSED
				    : target);
	if (r == GST_STATE_CHANGE_NO_PREROLL)
		p->is_live = TRUE;
	/* A FAILURE is followed by an ERROR message on the bus, which is
	 * where it is reported. */
	update_state(p);
}

void mp_player_load(MpPlayer *p, const char *uri, gboolean play)
{
	gst_element_set_state(p->playbin, GST_STATE_NULL);

	g_mutex_lock(&p->lock);
	p->gapless_pending = FALSE;
	g_clear_pointer(&p->switched_uri, g_free);
	g_clear_pointer(&p->next_uri, g_free);
	g_mutex_unlock(&p->lock);

	g_free(p->uri);
	p->uri = g_strdup(uri);
	reset_stream_state(p);
	if (p->video_widget != NULL)
		gtk_widget_queue_draw(p->video_widget);
	if (uri == NULL) {
		p->target = GST_STATE_NULL;
		update_state(p);
		return;
	}
	g_object_set(p->playbin, "uri", uri, NULL);
	/* Re-applied per stream: the volume element is per pipeline run. */
	gst_stream_volume_set_volume(GST_STREAM_VOLUME(p->playbin),
				     GST_STREAM_VOLUME_FORMAT_CUBIC,
				     p->volume);
	gst_stream_volume_set_mute(GST_STREAM_VOLUME(p->playbin), p->mute);
	set_target(p, play ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

const char *mp_player_get_uri(MpPlayer *p)
{
	return p->uri;
}

void mp_player_play(MpPlayer *p)
{
	if (p->uri == NULL)
		return;
	set_target(p, GST_STATE_PLAYING);
}

void mp_player_pause(MpPlayer *p)
{
	if (p->uri == NULL)
		return;
	set_target(p, GST_STATE_PAUSED);
}

void mp_player_stop(MpPlayer *p)
{
	/* READY rather than NULL keeps the sinks open (no new X connection
	 * per track), but drops the stream: Play starts over. */
	gst_element_set_state(p->playbin, GST_STATE_READY);
	p->target = GST_STATE_NULL;
	p->prerolled = FALSE;
	p->seekable = FALSE;
	p->buffering = FALSE;
	reset_seek_state(p);
	stop_timing_poll(p);
	update_state(p);
	if (p->video_widget != NULL)
		gtk_widget_queue_draw(p->video_widget);
}

MpPlayerState mp_player_get_state(MpPlayer *p)
{
	return p->state;
}

gboolean mp_player_wants_playing(MpPlayer *p)
{
	return p->target == GST_STATE_PLAYING;
}

void mp_player_set_next_uri(MpPlayer *p, const char *uri)
{
	g_mutex_lock(&p->lock);
	g_free(p->next_uri);
	p->next_uri = g_strdup(uri);
	g_mutex_unlock(&p->lock);
}

void mp_player_set_gapless(MpPlayer *p, gboolean gapless)
{
	g_mutex_lock(&p->lock);
	p->gapless = gapless;
	g_mutex_unlock(&p->lock);
}

/* ---- position --------------------------------------------------------- */

gboolean mp_player_seek(MpPlayer *p, gint64 position, gboolean accurate)
{
	if (p->uri == NULL)
		return FALSE;
	if (position < 0)
		position = 0;
	if (p->duration > 0 && position > p->duration)
		position = p->duration;

	if (!p->prerolled || p->seek_busy) {
		/* Not prerolled yet, or another seek is under way: remember
		 * this one (it replaces any other waiting) and issue it at the
		 * ASYNC_DONE.  Flushing seeks sent back to back would each
		 * restart the decoders and the sinks' preroll; a dragged seek
		 * bar or a held arrow key would bury the pipeline in them. */
		if (p->prerolled && !p->seekable)
			return FALSE;
		p->pending_seek = position;
		p->pending_seek_accurate = accurate;
		p->seek_target = position;
		if (p->target == GST_STATE_NULL)
			set_target(p, GST_STATE_PAUSED);
		return TRUE;
	}
	if (!p->seekable)
		return FALSE;
	return issue_seek(p, position, accurate);
}

gint64 mp_player_get_position(MpPlayer *p)
{
	gint64 pos = -1;
	if (p->uri == NULL || p->target == GST_STATE_NULL)
		return -1;
	/* A seek under way: the pipeline still reports where it was (or
	 * nothing, mid-flush), but the user asked for this. */
	if (p->seek_target >= 0)
		return p->seek_target;
	if (!gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos))
		return -1;
	return pos;
}

gint64 mp_player_get_duration(MpPlayer *p)
{
	return p->duration;
}

gboolean mp_player_is_seekable(MpPlayer *p)
{
	return p->seekable;
}

gboolean mp_player_is_live(MpPlayer *p)
{
	return p->is_live;
}

void mp_player_set_rate(MpPlayer *p, double rate)
{
	if (!(rate >= 0.25 && rate <= 4.0))
		return;
	p->rate = rate;
	if (p->prerolled && p->seekable && !p->seek_busy) {
		gint64 pos = 0;
		if (!gst_element_query_position(p->playbin, GST_FORMAT_TIME,
						&pos))
			pos = 0;
		issue_seek(p, pos, TRUE);
	} else if (p->seek_busy) {
		/* Goes out with the waiting seek, or from where the running
		 * one lands. */
		p->rate_pending = TRUE;
	} else {
		p->rate_pending = rate != 1.0;
	}
}

double mp_player_get_rate(MpPlayer *p)
{
	return p->rate;
}

/* ---- volume ----------------------------------------------------------- */

void mp_player_set_volume(MpPlayer *p, double volume)
{
	if (!(volume >= 0.0))
		volume = 0.0;
	if (volume > 1.0)
		volume = 1.0;
	p->volume = volume;
	/* The cubic scale is what people hear as even steps; playbin wants
	 * a linear factor, and the conversion is GStreamer's own. */
	gst_stream_volume_set_volume(GST_STREAM_VOLUME(p->playbin),
				     GST_STREAM_VOLUME_FORMAT_CUBIC, volume);
}

double mp_player_get_volume(MpPlayer *p)
{
	return p->volume;
}

void mp_player_set_mute(MpPlayer *p, gboolean mute)
{
	p->mute = !!mute;
	gst_stream_volume_set_mute(GST_STREAM_VOLUME(p->playbin), p->mute);
}

gboolean mp_player_get_mute(MpPlayer *p)
{
	return p->mute;
}

/* ---- what is playing -------------------------------------------------- */

gboolean mp_player_has_video(MpPlayer *p)
{
	return p->n_video > 0;
}

gboolean mp_player_has_audio(MpPlayer *p)
{
	return p->n_audio > 0;
}

const GstTagList *mp_player_get_tags(MpPlayer *p)
{
	return p->tags;
}

static gchar *tag_string(const GstTagList *tl, const char *tag)
{
	gchar *s = NULL;
	if (tl != NULL && gst_tag_list_get_string(tl, tag, &s))
		return s;
	return NULL;
}

static GstTagList *stream_tags(MpPlayer *p, MpTrackType type, int idx)
{
	const char *sig = type == MP_TRACK_AUDIO   ? "get-audio-tags"
			  : type == MP_TRACK_VIDEO ? "get-video-tags"
						   : "get-text-tags";
	GstTagList *tl = NULL;
	if (idx >= 0)
		g_signal_emit_by_name(p->playbin, sig, idx, &tl);
	return tl;
}

static GstPad *stream_pad(MpPlayer *p, MpTrackType type, int idx)
{
	const char *sig = type == MP_TRACK_AUDIO ? "get-audio-pad"
						 : "get-video-pad";
	GstPad *pad = NULL;
	if (idx >= 0)
		g_signal_emit_by_name(p->playbin, sig, idx, &pad);
	return pad;
}

void mp_player_get_stream_info(MpPlayer *p, MpStreamInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->video_sink = g_strdup(mp_player_get_video_sink_name(p));

	/* The audio sink autoaudiosink actually chose, by its factory. */
	GstElement *as = NULL;
	g_object_get(p->playbin, "audio-sink", &as, NULL);
	if (as != NULL) {
		GstElement *real = as;
		if (GST_IS_BIN(as)) {
			GstIterator *it = gst_bin_iterate_sinks(GST_BIN(as));
			GValue v = G_VALUE_INIT;
			if (gst_iterator_next(it, &v) == GST_ITERATOR_OK)
				real = g_value_get_object(&v);
			GstElementFactory *f = gst_element_get_factory(real);
			info->audio_sink = g_strdup(
				f ? gst_plugin_feature_get_name(
					    GST_PLUGIN_FEATURE(f))
				  : GST_OBJECT_NAME(real));
			g_value_unset(&v);
			gst_iterator_free(it);
		} else {
			GstElementFactory *f = gst_element_get_factory(as);
			info->audio_sink = g_strdup(
				f ? gst_plugin_feature_get_name(
					    GST_PLUGIN_FEATURE(f))
				  : GST_OBJECT_NAME(as));
		}
		gst_object_unref(as);
	}

	gint cv = -1, ca = -1, ct = -1;
	g_object_get(p->playbin, "current-video", &cv, "current-audio", &ca,
		     "current-text", &ct, NULL);

	if (p->n_video > 0) {
		GstPad *pad = stream_pad(p, MP_TRACK_VIDEO, cv);
		if (pad != NULL) {
			GstCaps *caps = gst_pad_get_current_caps(pad);
			GstVideoInfo vi;
			if (caps != NULL && gst_video_info_from_caps(&vi, caps)) {
				info->width = GST_VIDEO_INFO_WIDTH(&vi);
				info->height = GST_VIDEO_INFO_HEIGHT(&vi);
				info->fps_n = GST_VIDEO_INFO_FPS_N(&vi);
				info->fps_d = GST_VIDEO_INFO_FPS_D(&vi);
				info->par_n = GST_VIDEO_INFO_PAR_N(&vi);
				info->par_d = GST_VIDEO_INFO_PAR_D(&vi);
			}
			if (caps != NULL)
				gst_caps_unref(caps);
			gst_object_unref(pad);
		}
		GstTagList *tl = stream_tags(p, MP_TRACK_VIDEO, cv);
		info->video_codec = tag_string(tl, GST_TAG_VIDEO_CODEC);
		if (tl != NULL)
			gst_tag_list_unref(tl);
	}

	if (p->n_audio > 0) {
		GstPad *pad = stream_pad(p, MP_TRACK_AUDIO, ca);
		if (pad != NULL) {
			GstCaps *caps = gst_pad_get_current_caps(pad);
			if (caps != NULL && gst_caps_get_size(caps) > 0) {
				const GstStructure *s =
					gst_caps_get_structure(caps, 0);
				gst_structure_get_int(s, "rate",
						      &info->audio_rate);
				gst_structure_get_int(s, "channels",
						      &info->audio_channels);
			}
			if (caps != NULL)
				gst_caps_unref(caps);
			gst_object_unref(pad);
		}
		GstTagList *tl = stream_tags(p, MP_TRACK_AUDIO, ca);
		info->audio_codec = tag_string(tl, GST_TAG_AUDIO_CODEC);
		if (tl != NULL) {
			guint br = 0;
			if (gst_tag_list_get_uint(tl, GST_TAG_BITRATE, &br) ||
			    gst_tag_list_get_uint(tl, GST_TAG_NOMINAL_BITRATE,
						  &br))
				info->bitrate = br;
			gst_tag_list_unref(tl);
		}
	}

	if (p->n_text > 0 && ct >= 0) {
		GstTagList *tl = stream_tags(p, MP_TRACK_TEXT, ct);
		info->subtitle_codec = tag_string(tl, GST_TAG_SUBTITLE_CODEC);
		if (tl != NULL)
			gst_tag_list_unref(tl);
	}

	/* What the per-stream lists lacked, from the merged tags. */
	if (info->video_codec == NULL && p->n_video > 0)
		info->video_codec = tag_string(p->tags, GST_TAG_VIDEO_CODEC);
	if (info->audio_codec == NULL && p->n_audio > 0)
		info->audio_codec = tag_string(p->tags, GST_TAG_AUDIO_CODEC);
	info->container = tag_string(p->tags, GST_TAG_CONTAINER_FORMAT);
	if (info->bitrate == 0 && p->tags != NULL) {
		guint br = 0;
		if (gst_tag_list_get_uint(p->tags, GST_TAG_BITRATE, &br) ||
		    gst_tag_list_get_uint(p->tags, GST_TAG_NOMINAL_BITRATE,
					  &br))
			info->bitrate = br;
	}
}

void mp_stream_info_clear(MpStreamInfo *info)
{
	g_free(info->video_codec);
	g_free(info->audio_codec);
	g_free(info->container);
	g_free(info->subtitle_codec);
	g_free(info->video_sink);
	g_free(info->audio_sink);
	memset(info, 0, sizeof(*info));
}

/* ---- track selection -------------------------------------------------- */

int mp_player_get_n_tracks(MpPlayer *p, MpTrackType type)
{
	switch (type) {
	case MP_TRACK_AUDIO:
		return p->n_audio;
	case MP_TRACK_VIDEO:
		return p->n_video;
	case MP_TRACK_TEXT:
		return p->n_text;
	}
	return 0;
}

static const char *current_prop(MpTrackType type)
{
	return type == MP_TRACK_AUDIO	? "current-audio"
	       : type == MP_TRACK_VIDEO ? "current-video"
					: "current-text";
}

int mp_player_get_current_track(MpPlayer *p, MpTrackType type)
{
	gint cur = -1;
	g_object_get(p->playbin, current_prop(type), &cur, NULL);
	return cur;
}

gchar *mp_player_get_track_label(MpPlayer *p, MpTrackType type, int index)
{
	GstTagList *tl = stream_tags(p, type, index);
	gchar *lang = tag_string(tl, GST_TAG_LANGUAGE_NAME);
	if (lang == NULL) {
		gchar *code = tag_string(tl, GST_TAG_LANGUAGE_CODE);
		if (code != NULL) {
			const gchar *name = gst_tag_get_language_name(code);
			lang = g_strdup(name ? name : code);
			g_free(code);
		}
	}
	gchar *title = tag_string(tl, GST_TAG_TITLE);
	gchar *codec = tag_string(tl, type == MP_TRACK_AUDIO
					      ? GST_TAG_AUDIO_CODEC
				      : type == MP_TRACK_VIDEO
					      ? GST_TAG_VIDEO_CODEC
					      : GST_TAG_SUBTITLE_CODEC);
	if (tl != NULL)
		gst_tag_list_unref(tl);

	GString *s = g_string_new(NULL);
	g_string_append_printf(s, "%d", index + 1);
	if (title != NULL)
		g_string_append_printf(s, " — %s", title);
	if (lang != NULL)
		g_string_append_printf(s, " — %s", lang);
	if (codec != NULL)
		g_string_append_printf(s, " (%s)", codec);
	g_free(title);
	g_free(lang);
	g_free(codec);
	return g_string_free(s, FALSE);
}

void mp_player_set_current_track(MpPlayer *p, MpTrackType type, int index)
{
	if (index < 0 || index >= mp_player_get_n_tracks(p, type))
		return;
	g_object_set(p->playbin, current_prop(type), index, NULL);
}

void mp_player_set_subtitles(MpPlayer *p, gboolean on)
{
	p->subtitles = !!on;
	if (p->no_video)
		return;
	guint flags = 0;
	g_object_get(p->playbin, "flags", &flags, NULL);
	if (on)
		flags |= PLAY_FLAG_TEXT;
	else
		flags &= ~(guint)PLAY_FLAG_TEXT;
	g_object_set(p->playbin, "flags", flags, NULL);
}

gboolean mp_player_get_subtitles(MpPlayer *p)
{
	return p->subtitles;
}

void mp_player_expose(MpPlayer *p)
{
	if (p->overlay_sink != NULL && p->prerolled && p->n_video > 0)
		gst_video_overlay_expose(GST_VIDEO_OVERLAY(p->overlay_sink));
	else if (p->video_widget != NULL)
		gtk_widget_queue_draw(p->video_widget);
}
