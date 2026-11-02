// LikeOS -- LikeOS Media Player: still pictures of videos for the library
// tiles.
//
// A picture is a frame from a little way into the video (the first frame
// is so often black), taken by a playbin of its own that only decodes the
// picture and never plays: paused, sought, and asked for the frame it shows.
// That happens in a worker thread, one video at a time, so a folder of
// films costs one decoder at a time and the window keeps painting.
//
// Pictures are kept in ~/.cache/mediaplayer/thumbnails as PNG files named
// after a checksum of the URI, with the video's modification time written
// into the file: a video that changes gets a new picture.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <glib/gstdio.h>
#include <gst/gst.h>

#include "thumbs.h"
#include "util.h"

#define THUMB_WIDTH 320
#define THUMB_WAIT (8 * GST_SECOND)
/* playbin's "flags": decode the video stream only. */
#define PLAY_FLAG_VIDEO 0x1

struct MpThumbs {
	MpThumbReady ready;
	gpointer data;
	GHashTable *pictures;	/* URI -> GdkPixbuf*, or NULL: failed */
	GQueue waiting;		/* URIs */
	GHashTable *queued;	/* URIs waiting or being made */
	GCancellable *cancel;
	gboolean busy;
	struct Job *job;	/* the one being made, while busy */
	guint idle;
	gchar *dir;
};

typedef struct Job {
	MpThumbs *t;		/* NULL once the owner is gone */
	gchar *uri;
} Job;

static void pump(MpThumbs *t);

static gchar *cache_path(MpThumbs *t, const char *uri)
{
	gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	gchar *name = g_strconcat(sum, ".png", NULL);
	gchar *path = g_build_filename(t->dir, name, NULL);
	g_free(sum);
	g_free(name);
	return path;
}

static gchar *file_mtime(const char *uri)
{
	gchar *path = mp_uri_to_path(uri);
	GStatBuf st;
	gchar *m = NULL;
	if (path != NULL && g_stat(path, &st) == 0)
		m = g_strdup_printf("%lld", (long long)st.st_mtime);
	g_free(path);
	return m;
}

static GdkPixbuf *load_cached(MpThumbs *t, const char *uri)
{
	gchar *path = cache_path(t, uri);
	GdkPixbuf *pb = NULL;
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		pb = gdk_pixbuf_new_from_file(path, NULL);
		gchar *m = file_mtime(uri);
		const char *saved = pb ? gdk_pixbuf_get_option(
						 pb, "tEXt::Thumb::MTime")
				       : NULL;
		if (pb != NULL && (m == NULL || g_strcmp0(saved, m) != 0))
			g_clear_object(&pb);
		g_free(m);
	}
	g_free(path);
	return pb;
}

static void save_cached(MpThumbs *t, const char *uri, GdkPixbuf *pb)
{
	if (g_mkdir_with_parents(t->dir, 0700) != 0)
		return;
	gchar *path = cache_path(t, uri);
	gchar *m = file_mtime(uri);
	gchar *tmp = g_strconcat(path, ".part", NULL);
	if (m != NULL &&
	    gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri,
			    "tEXt::Thumb::MTime", m, NULL))
		g_rename(tmp, path);
	else
		g_unlink(tmp);
	g_free(tmp);
	g_free(m);
	g_free(path);
}

/* ---- the worker ------------------------------------------------------- */

static GdkPixbuf *pixbuf_from_sample(GstSample *s)
{
	GstCaps *caps = gst_sample_get_caps(s);
	GstBuffer *buf = gst_sample_get_buffer(s);
	if (caps == NULL || buf == NULL)
		return NULL;
	GstStructure *st = gst_caps_get_structure(caps, 0);
	int w = 0, h = 0, par_n = 1, par_d = 1;
	if (!gst_structure_get_int(st, "width", &w) ||
	    !gst_structure_get_int(st, "height", &h) || w <= 0 || h <= 0 ||
	    w > 8192 || h > 8192)
		return NULL;
	gst_structure_get_fraction(st, "pixel-aspect-ratio", &par_n, &par_d);
	if (par_n <= 0 || par_d <= 0)
		par_n = par_d = 1;

	GstMapInfo map;
	if (!gst_buffer_map(buf, &map, GST_MAP_READ))
		return NULL;
	gsize stride = GST_ROUND_UP_4((gsize)w * 3);
	GdkPixbuf *full = NULL;
	if (map.size >= stride * (gsize)h) {
		full = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
		guchar *dst = gdk_pixbuf_get_pixels(full);
		int dstride = gdk_pixbuf_get_rowstride(full);
		for (int y = 0; y < h; y++)
			memcpy(dst + (gsize)y * dstride, map.data + y * stride,
			       (gsize)w * 3);
	}
	gst_buffer_unmap(buf, &map);
	if (full == NULL)
		return NULL;

	/* Square pixels, THUMB_WIDTH wide. */
	double dw = (double)w * par_n / par_d;
	int tw = THUMB_WIDTH, th = (int)(THUMB_WIDTH * h / dw + 0.5);
	if (th < 1)
		th = 1;
	GdkPixbuf *small = gdk_pixbuf_scale_simple(full, tw, th,
						   GDK_INTERP_BILINEAR);
	g_object_unref(full);
	return small;
}

static GdkPixbuf *grab_frame(const char *uri, GCancellable *cancel)
{
	GstElement *pb = gst_element_factory_make("playbin", NULL);
	if (pb == NULL)
		return NULL;
	GstElement *vs = gst_element_factory_make("fakesink", NULL);
	GstElement *as = gst_element_factory_make("fakesink", NULL);
	if (vs != NULL)
		g_object_set(pb, "video-sink", vs, NULL);
	if (as != NULL)
		g_object_set(pb, "audio-sink", as, NULL);
	g_object_set(pb, "uri", uri, "flags", PLAY_FLAG_VIDEO, NULL);

	GdkPixbuf *out = NULL;
	if (gst_element_set_state(pb, GST_STATE_PAUSED) ==
	    GST_STATE_CHANGE_FAILURE)
		goto done;
	if (gst_element_get_state(pb, NULL, NULL, THUMB_WAIT) !=
	    GST_STATE_CHANGE_SUCCESS)
		goto done;
	if (g_cancellable_is_cancelled(cancel))
		goto done;

	gint64 dur = -1;
	if (gst_element_query_duration(pb, GST_FORMAT_TIME, &dur) && dur > 0) {
		/* A sixth of the way in, but not more than two minutes: past
		 * the opening titles, before the plot. */
		gint64 at = MIN(dur / 6, 120 * GST_SECOND);
		if (gst_element_seek_simple(pb, GST_FORMAT_TIME,
					    GST_SEEK_FLAG_FLUSH |
						    GST_SEEK_FLAG_KEY_UNIT,
					    at))
			gst_element_get_state(pb, NULL, NULL, THUMB_WAIT);
	}

	GstCaps *caps = gst_caps_new_simple("video/x-raw", "format",
					    G_TYPE_STRING, "RGB", NULL);
	GstSample *s = NULL;
	g_signal_emit_by_name(pb, "convert-sample", caps, &s);
	gst_caps_unref(caps);
	if (s != NULL) {
		out = pixbuf_from_sample(s);
		gst_sample_unref(s);
	}
done:
	gst_element_set_state(pb, GST_STATE_NULL);
	gst_object_unref(pb);
	return out;
}

static void job_thread(GTask *task, gpointer source, gpointer task_data,
		       GCancellable *cancel)
{
	Job *job = task_data;
	(void)source;
	GdkPixbuf *pb = grab_frame(job->uri, cancel);
	g_task_return_pointer(task, pb, pb ? g_object_unref : NULL);
}

static void job_free(gpointer p)
{
	Job *job = p;
	g_free(job->uri);
	g_free(job);
}

static void job_done(GObject *source, GAsyncResult *res, gpointer data)
{
	(void)source;
	(void)data;
	GTask *task = G_TASK(res);
	Job *job = g_task_get_task_data(task);
	GdkPixbuf *pb = g_task_propagate_pointer(task, NULL);
	MpThumbs *t = job->t;
	if (t == NULL) {
		/* The owner went while this one was being made. */
		if (pb != NULL)
			g_object_unref(pb);
		return;
	}
	t->busy = FALSE;
	t->job = NULL;
	g_hash_table_remove(t->queued, job->uri);
	if (pb != NULL)
		save_cached(t, job->uri, pb);
	g_hash_table_replace(t->pictures, g_strdup(job->uri), pb);
	t->ready(job->uri, pb, t->data);
	pump(t);
}

static void pump(MpThumbs *t)
{
	while (!t->busy && !g_queue_is_empty(&t->waiting)) {
		gchar *uri = g_queue_pop_head(&t->waiting);
		GdkPixbuf *pb = load_cached(t, uri);
		if (pb != NULL) {
			g_hash_table_remove(t->queued, uri);
			g_hash_table_replace(t->pictures, g_strdup(uri), pb);
			t->ready(uri, pb, t->data);
			g_free(uri);
			continue;
		}
		Job *job = g_new0(Job, 1);
		job->t = t;
		job->uri = uri;
		t->job = job;
		t->busy = TRUE;
		GTask *task = g_task_new(NULL, t->cancel, job_done, NULL);
		g_task_set_task_data(task, job, job_free);
		g_task_run_in_thread(task, job_thread);
		g_object_unref(task);
	}
}

static gboolean pump_idle(gpointer data)
{
	MpThumbs *t = data;
	t->idle = 0;
	pump(t);
	return G_SOURCE_REMOVE;
}

static void unref_picture(gpointer p)
{
	if (p != NULL)
		g_object_unref(p);
}

MpThumbs *mp_thumbs_new(MpThumbReady ready, gpointer data)
{
	MpThumbs *t = g_new0(MpThumbs, 1);
	t->ready = ready;
	t->data = data;
	t->pictures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					    unref_picture);
	t->queued = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					  NULL);
	g_queue_init(&t->waiting);
	t->cancel = g_cancellable_new();
	t->dir = g_build_filename(g_get_user_cache_dir(), "mediaplayer",
				  "thumbnails", NULL);
	return t;
}

void mp_thumbs_free(MpThumbs *t)
{
	if (t == NULL)
		return;
	g_cancellable_cancel(t->cancel);
	if (t->busy && t->job != NULL)
		t->job->t = NULL;
	if (t->idle != 0)
		g_source_remove(t->idle);
	g_object_unref(t->cancel);
	g_queue_clear_full(&t->waiting, g_free);
	g_hash_table_destroy(t->queued);
	g_hash_table_destroy(t->pictures);
	g_free(t->dir);
	g_free(t);
}

GdkPixbuf *mp_thumbs_get(MpThumbs *t, const char *uri)
{
	gpointer pb = NULL;
	if (t == NULL || uri == NULL)
		return NULL;
	if (g_hash_table_lookup_extended(t->pictures, uri, NULL, &pb))
		return pb;
	if (!mp_uri_is_local(uri) || g_hash_table_contains(t->queued, uri))
		return NULL;
	g_hash_table_add(t->queued, g_strdup(uri));
	g_queue_push_tail(&t->waiting, g_strdup(uri));
	/* Not from inside a draw handler: the cache lookup may call back. */
	if (!t->busy && t->idle == 0)
		t->idle = g_idle_add(pump_idle, t);
	return NULL;
}
