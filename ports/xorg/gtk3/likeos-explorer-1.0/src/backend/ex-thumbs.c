// LikeOS Explorer: thumbnails.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-thumbs.h"
#include "backend/ex-util.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#ifdef EX_HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#endif

/* Larger images are not worth decoding for a thumbnail. */
#define EX_THUMB_MAX_IMAGE (64u * 1024 * 1024)
/* How long a video gets to show a frame. */
#define EX_THUMB_VIDEO_TIMEOUT (5 * GST_SECOND)

typedef struct {
	ExFile *file;
	ExThumbSize size;
	char *uri;
	gint64 mtime;
	char *ctype;
} Job;

static GMutex g_lock;
static GQueue g_jobs = G_QUEUE_INIT; /* Job*, front first */
static GHashTable *g_waiting; /* uri -> GList link in g_jobs */
static GThreadPool *g_pool;

static void job_free(Job *j)
{
	g_clear_object(&j->file);
	g_free(j->uri);
	g_free(j->ctype);
	g_free(j);
}

static const char *size_dir(ExThumbSize size)
{
	switch (size) {
	case EX_THUMB_XLARGE:
		return "x-large";
	case EX_THUMB_LARGE:
		return "large";
	case EX_THUMB_NORMAL:
	default:
		return "normal";
	}
}

char *ex_thumbs_cache_path(const char *uri, ExThumbSize size)
{
	char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	char *name = g_strconcat(md5, ".png", NULL);
	char *p = g_build_filename(g_get_user_cache_dir(), "thumbnails",
				   size_dir(size), name, NULL);

	g_free(name);
	g_free(md5);
	return p;
}

static char *fail_path(const char *uri)
{
	char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	char *name = g_strconcat(md5, ".png", NULL);
	char *p = g_build_filename(g_get_user_cache_dir(), "thumbnails",
				   "fail", "likeos-explorer", name, NULL);

	g_free(name);
	g_free(md5);
	return p;
}

/* Whether gdk-pixbuf has a loader for this type.  Read once. */
static gboolean pixbuf_handles(const char *ctype)
{
	static GHashTable *types;
	static gsize once;

	if (g_once_init_enter(&once)) {
		GSList *fmts = gdk_pixbuf_get_formats();

		types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					      NULL);
		for (GSList *l = fmts; l; l = l->next) {
			char **m = gdk_pixbuf_format_get_mime_types(l->data);

			for (int i = 0; m && m[i]; i++)
				g_hash_table_add(types, g_strdup(m[i]));
			g_strfreev(m);
		}
		g_slist_free(fmts);
		g_once_init_leave(&once, 1);
	}
	return g_hash_table_contains(types, ctype);
}

gboolean ex_thumbs_supported(ExFile *f)
{
	const char *ct;

	if (ex_file_is_dir(f) || !(ex_file_get_caps(f) & EX_CAP_READ))
		return FALSE;
	ct = ex_file_get_content_type(f);
	if (g_str_has_prefix(ct, "image/"))
		return pixbuf_handles(ct) &&
		       ex_file_get_size(f) <= EX_THUMB_MAX_IMAGE;
#ifdef EX_HAVE_GSTREAMER
	if (g_str_has_prefix(ct, "video/"))
		return TRUE;
#endif
	return FALSE;
}

/* A cached thumbnail that still matches the file, or NULL. */
static GdkPixbuf *load_cached(const char *path, const char *uri, gint64 mtime)
{
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
	const char *u, *m;

	if (!pb)
		return NULL;
	u = gdk_pixbuf_get_option(pb, "tEXt::Thumb::URI");
	m = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
	if (!u || strcmp(u, uri) != 0 || !m ||
	    g_ascii_strtoll(m, NULL, 10) != mtime) {
		g_object_unref(pb);
		return NULL;
	}
	return pb;
}

static gboolean failed_before(const char *uri, gint64 mtime)
{
	char *p = fail_path(uri);
	GdkPixbuf *pb = load_cached(p, uri, mtime);

	g_free(p);
	if (pb) {
		g_object_unref(pb);
		return TRUE;
	}
	return FALSE;
}

/* Write `pb' as the thumbnail at `path' -- to a temporary name and renamed,
 * so a reader never sees half a PNG. */
static void store(GdkPixbuf *pb, const char *path, const char *uri,
		  gint64 mtime, guint64 size)
{
	char *dir = g_path_get_dirname(path);
	char *tmp = g_strdup_printf("%s.%d.tmp", path, (int)getpid());
	char *mt = g_strdup_printf("%" G_GINT64_FORMAT, mtime);
	char *sz = g_strdup_printf("%" G_GUINT64_FORMAT, size);

	g_mkdir_with_parents(dir, 0700);
	if (gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri,
			    "tEXt::Thumb::MTime", mt, "tEXt::Thumb::Size", sz,
			    "tEXt::Software", "LikeOS Explorer", NULL)) {
		g_chmod(tmp, 0600);
		if (g_rename(tmp, path) != 0)
			g_unlink(tmp);
	} else {
		g_unlink(tmp);
	}
	g_free(sz);
	g_free(mt);
	g_free(tmp);
	g_free(dir);
}

static GdkPixbuf *scale_to(GdkPixbuf *src, int box)
{
	int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);

	if (w <= box && h <= box)
		return g_object_ref(src);
	double s = (double)box / (w > h ? w : h);
	int nw = MAX(1, (int)(w * s)), nh = MAX(1, (int)(h * s));

	return gdk_pixbuf_scale_simple(src, nw, nh, GDK_INTERP_BILINEAR);
}

static GdkPixbuf *make_image(const char *uri, int box)
{
	char *path = g_filename_from_uri(uri, NULL, NULL);
	GdkPixbuf *pb = NULL, *rot = NULL;

	if (!path)
		return NULL;
	pb = gdk_pixbuf_new_from_file_at_size(path, box, box, NULL);
	g_free(path);
	if (!pb)
		return NULL;
	/* A photo taken on its side is shown upright. */
	rot = gdk_pixbuf_apply_embedded_orientation(pb);
	g_object_unref(pb);
	return rot;
}

#ifdef EX_HAVE_GSTREAMER
static gboolean wait_preroll(GstElement *pipe)
{
	GstBus *bus = gst_element_get_bus(pipe);
	gboolean ok = FALSE;

	for (;;) {
		GstMessage *m = gst_bus_timed_pop_filtered(
			bus, EX_THUMB_VIDEO_TIMEOUT,
			GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR |
				GST_MESSAGE_EOS);

		if (!m)
			break;
		ok = GST_MESSAGE_TYPE(m) == GST_MESSAGE_ASYNC_DONE;
		gst_message_unref(m);
		break;
	}
	gst_object_unref(bus);
	return ok;
}

static void free_pixels(guchar *pixels, gpointer data)
{
	g_free(pixels);
}

static GdkPixbuf *make_video(const char *uri, int box)
{
	static gsize inited;
	GError *err = NULL;
	GdkPixbuf *out = NULL;

	if (g_once_init_enter(&inited)) {
		gst_init_check(NULL, NULL, NULL);
		g_once_init_leave(&inited, 1);
	}
	char *desc = g_strdup_printf(
		"uridecodebin uri=\"%s\" ! videoconvert ! videoscale ! "
		"appsink name=sink sync=false "
		"caps=video/x-raw,format=RGB,pixel-aspect-ratio=1/1",
		uri);
	GstElement *pipe = gst_parse_launch(desc, &err);

	g_free(desc);
	if (!pipe) {
		g_clear_error(&err);
		return NULL;
	}
	g_clear_error(&err);
	GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");

	gst_element_set_state(pipe, GST_STATE_PAUSED);
	if (wait_preroll(pipe)) {
		gint64 dur = 0;

		/* A tenth of the way in: past black leaders and logos. */
		if (gst_element_query_duration(pipe, GST_FORMAT_TIME, &dur) &&
		    dur > 0 &&
		    gst_element_seek_simple(pipe, GST_FORMAT_TIME,
					    GST_SEEK_FLAG_FLUSH |
						    GST_SEEK_FLAG_KEY_UNIT,
					    dur / 10))
			(void)wait_preroll(pipe);
		GstSample *s = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink),
							     GST_SECOND);
		if (s) {
			GstVideoInfo vi;
			GstBuffer *buf = gst_sample_get_buffer(s);
			GstMapInfo map;

			if (gst_video_info_from_caps(&vi, gst_sample_get_caps(s)) &&
			    buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
				int w = GST_VIDEO_INFO_WIDTH(&vi);
				int h = GST_VIDEO_INFO_HEIGHT(&vi);
				int stride = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0);
				guchar *px = g_memdup2(map.data,
						       (gsize)stride * (gsize)h);
				GdkPixbuf *full = gdk_pixbuf_new_from_data(
					px, GDK_COLORSPACE_RGB, FALSE, 8, w, h,
					stride, free_pixels, NULL);

				out = scale_to(full, box);
				g_object_unref(full);
				gst_buffer_unmap(buf, &map);
			}
			gst_sample_unref(s);
		}
	}
	gst_element_set_state(pipe, GST_STATE_NULL);
	gst_object_unref(sink);
	gst_object_unref(pipe);
	return out;
}
#endif

typedef struct {
	ExFile *file;
	GdkPixbuf *pb;
} Done;

static gboolean deliver(gpointer data)
{
	Done *d = data;

	if (d->pb)
		ex_file_set_thumbnail(d->file, d->pb);
	else
		ex_file_set_thumbnail_failed(d->file);
	g_clear_object(&d->pb);
	g_object_unref(d->file);
	g_free(d);
	return G_SOURCE_REMOVE;
}

static void run_job(gpointer unused, gpointer pool_data)
{
	Job *j;

	g_mutex_lock(&g_lock);
	j = g_queue_pop_head(&g_jobs);
	if (j)
		g_hash_table_remove(g_waiting, j->uri);
	g_mutex_unlock(&g_lock);
	if (!j)
		return;

	char *path = ex_thumbs_cache_path(j->uri, j->size);
	GdkPixbuf *pb = load_cached(path, j->uri, j->mtime);

	if (!pb && !failed_before(j->uri, j->mtime)) {
		GdkPixbuf *raw = NULL;

		if (g_str_has_prefix(j->ctype, "image/"))
			raw = make_image(j->uri, (int)j->size);
#ifdef EX_HAVE_GSTREAMER
		else if (g_str_has_prefix(j->ctype, "video/"))
			raw = make_video(j->uri, (int)j->size);
#endif
		if (raw) {
			pb = scale_to(raw, (int)j->size);
			g_object_unref(raw);
			store(pb, path, j->uri, j->mtime,
			      ex_file_get_size(j->file));
		} else {
			/* A 1x1 marker, so the next look is a cache hit. */
			GdkPixbuf *mark = gdk_pixbuf_new(GDK_COLORSPACE_RGB,
							 TRUE, 8, 1, 1);
			char *fp = fail_path(j->uri);

			gdk_pixbuf_fill(mark, 0);
			store(mark, fp, j->uri, j->mtime, 0);
			g_free(fp);
			g_object_unref(mark);
		}
	}
	g_free(path);

	Done *d = g_new0(Done, 1);
	d->file = g_object_ref(j->file);
	d->pb = pb;
	ex_run_on(NULL, deliver, d, NULL);
	job_free(j);
}

void ex_thumbs_request(ExFile *f, ExThumbSize size)
{
	Job *j;
	GList *link;

	if (ex_file_get_thumbnail(f) || ex_file_thumbnail_failed(f) ||
	    !ex_thumbs_supported(f))
		return;
	char *uri = g_file_get_uri(ex_file_get_file(f));

	g_mutex_lock(&g_lock);
	if (!g_waiting)
		g_waiting = g_hash_table_new(g_str_hash, g_str_equal);
	if (!g_pool)
		g_pool = g_thread_pool_new(run_job, NULL, 2, FALSE, NULL);
	link = g_hash_table_lookup(g_waiting, uri);
	if (link) {
		/* Already waiting: to the front. */
		g_queue_unlink(&g_jobs, link);
		g_queue_push_head_link(&g_jobs, link);
		g_mutex_unlock(&g_lock);
		g_free(uri);
		return;
	}
	j = g_new0(Job, 1);
	j->file = g_object_ref(f);
	j->size = size;
	j->uri = uri;
	j->mtime = ex_file_get_mtime(f);
	j->ctype = g_strdup(ex_file_get_content_type(f));
	g_queue_push_head(&g_jobs, j);
	g_hash_table_insert(g_waiting, j->uri, g_queue_peek_head_link(&g_jobs));
	g_mutex_unlock(&g_lock);
	/* One pool task per job; each takes whatever is at the front. */
	g_thread_pool_push(g_pool, GINT_TO_POINTER(1), NULL);
}

void ex_thumbs_cancel_all(void)
{
	Job *j;

	g_mutex_lock(&g_lock);
	while ((j = g_queue_pop_head(&g_jobs)))
		job_free(j);
	if (g_waiting)
		g_hash_table_remove_all(g_waiting);
	g_mutex_unlock(&g_lock);
}
