// LikeOS -- File Explorer: what a song or a video is.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-media.h"

#ifdef EX_HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#endif

void ex_media_info_free(ExMediaInfo *m)
{
	if (!m)
		return;
	g_free(m->title);
	g_free(m->artist);
	g_free(m->album);
	g_free(m);
}

char *ex_media_format_duration(gint64 ms)
{
	gint64 s = ms / 1000;

	if (s >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT ":%02d:%02d", s / 3600,
				       (int)(s / 60 % 60), (int)(s % 60));
	return g_strdup_printf("%d:%02d", (int)(s / 60), (int)(s % 60));
}

#ifdef EX_HAVE_GSTREAMER
static char *tag_string(const GstTagList *tags, const char *tag)
{
	char *v = NULL;

	if (tags && gst_tag_list_get_string(tags, tag, &v))
		return v;
	return NULL;
}

static void describe_thread(GTask *task, gpointer src, gpointer data,
			    GCancellable *c)
{
	static gsize inited;
	GFile *file = data;
	GError *err = NULL;
	char *uri = g_file_get_uri(file);

	if (g_once_init_enter(&inited)) {
		gst_init_check(NULL, NULL, NULL);
		g_once_init_leave(&inited, 1);
	}
	GstDiscoverer *d = gst_discoverer_new(5 * GST_SECOND, &err);
	GstDiscovererInfo *info = d ? gst_discoverer_discover_uri(d, uri, &err) :
				      NULL;

	g_free(uri);
	if (!info) {
		g_task_return_error(task, err ? err :
						g_error_new_literal(
							G_IO_ERROR,
							G_IO_ERROR_FAILED,
							"Not a media file"));
		g_clear_object(&d);
		return;
	}
	g_clear_error(&err);
	ExMediaInfo *m = g_new0(ExMediaInfo, 1);
	GList *videos = gst_discoverer_info_get_video_streams(info);
	const GstTagList *tags = gst_discoverer_info_get_tags(info);

	m->duration_ms = (gint64)(gst_discoverer_info_get_duration(info) /
				  GST_MSECOND);
	if (videos) {
		GstDiscovererVideoInfo *v = videos->data;

		m->width = (int)gst_discoverer_video_info_get_width(v);
		m->height = (int)gst_discoverer_video_info_get_height(v);
	}
	gst_discoverer_stream_info_list_free(videos);
	m->title = tag_string(tags, GST_TAG_TITLE);
	m->artist = tag_string(tags, GST_TAG_ARTIST);
	m->album = tag_string(tags, GST_TAG_ALBUM);
	g_object_unref(info);
	g_object_unref(d);
	g_task_return_pointer(task, m, (GDestroyNotify)ex_media_info_free);
}
#endif

void ex_media_describe_async(GFile *file, GCancellable *cancellable,
			     GAsyncReadyCallback done, gpointer user_data)
{
	GTask *task = g_task_new(NULL, cancellable, done, user_data);

#ifdef EX_HAVE_GSTREAMER
	g_task_set_task_data(task, g_object_ref(file), g_object_unref);
	g_task_run_in_thread(task, describe_thread);
#else
	g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"Media details are not available");
#endif
	g_object_unref(task);
}

ExMediaInfo *ex_media_describe_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_pointer(G_TASK(res), error);
}
