// LikeOS -- Media Player: background metadata for the play queue.
//
// GstDiscoverer opens a file far enough to know its streams, length and
// tags, without playing it.  Files are probed one at a time with a short
// pause in between, so filling in a queue of a few thousand tracks costs
// one decoder setup at a time on an otherwise idle machine instead of a
// burst that would stutter whatever is playing.
//
// Copyright (C) 2026 The LikeOS Project

#include <gst/pbutils/pbutils.h>

#include "discover.h"
#include "util.h"

#define DISCOVER_TIMEOUT (5 * GST_SECOND)
#define DISCOVER_GAP_MS 40
#define DISCOVER_QUEUE_MAX 10000

typedef struct {
	guint id;
	gchar *uri;
} Item;

struct MpDiscover {
	GstDiscoverer *disc;
	MpDiscoverFunc func;
	gpointer user_data;
	GQueue queue;		/* Item* waiting */
	Item *busy;		/* the one being probed, or NULL */
	guint pump_source;
};

static void item_free(gpointer p)
{
	Item *it = p;
	if (it == NULL)
		return;
	g_free(it->uri);
	g_free(it);
}

static void pump(MpDiscover *d);

static gboolean pump_cb(gpointer data)
{
	MpDiscover *d = data;
	d->pump_source = 0;
	pump(d);
	return G_SOURCE_REMOVE;
}

static void schedule_pump(MpDiscover *d)
{
	if (d->pump_source == 0 && d->busy == NULL &&
	    !g_queue_is_empty(&d->queue))
		d->pump_source = g_timeout_add(DISCOVER_GAP_MS, pump_cb, d);
}

static void pump(MpDiscover *d)
{
	while (d->busy == NULL && !g_queue_is_empty(&d->queue)) {
		Item *it = g_queue_pop_head(&d->queue);
		if (gst_discoverer_discover_uri_async(d->disc, it->uri)) {
			d->busy = it;
			return;
		}
		item_free(it);
	}
}

static gchar *tag_str(const GstTagList *tl, const char *tag)
{
	gchar *s = NULL;
	if (tl != NULL && gst_tag_list_get_string(tl, tag, &s))
		return s;
	return NULL;
}

static void on_discovered(GstDiscoverer *disc, GstDiscovererInfo *info,
			  GError *err, gpointer data)
{
	MpDiscover *d = data;
	(void)disc;
	(void)err;

	Item *it = d->busy;
	d->busy = NULL;
	if (it == NULL)
		goto next;	/* forgotten while being probed */

	GstDiscovererResult res = gst_discoverer_info_get_result(info);
	if (it->id != 0 && (res == GST_DISCOVERER_OK ||
			    res == GST_DISCOVERER_MISSING_PLUGINS)) {
		const GstTagList *tl = gst_discoverer_info_get_tags(info);
		gchar *title = tag_str(tl, GST_TAG_TITLE);
		gchar *artist = tag_str(tl, GST_TAG_ARTIST);
		gchar *album = tag_str(tl, GST_TAG_ALBUM);
		GstClockTime dur = gst_discoverer_info_get_duration(info);
		gint64 ns = GST_CLOCK_TIME_IS_VALID(dur) && dur > 0
				    ? (gint64)dur
				    : -1;
		d->func(it->id, title, artist, album, ns, d->user_data);
		g_free(title);
		g_free(artist);
		g_free(album);
	}
	item_free(it);
next:
	schedule_pump(d);
}

MpDiscover *mp_discover_new(MpDiscoverFunc func, gpointer user_data)
{
	GError *err = NULL;
	GstDiscoverer *disc = gst_discoverer_new(DISCOVER_TIMEOUT, &err);
	if (disc == NULL) {
		g_message("metadata discovery unavailable: %s",
			  err ? err->message : "?");
		g_clear_error(&err);
		return NULL;
	}
	MpDiscover *d = g_new0(MpDiscover, 1);
	d->disc = disc;
	d->func = func;
	d->user_data = user_data;
	g_queue_init(&d->queue);
	g_signal_connect(disc, "discovered", G_CALLBACK(on_discovered), d);
	/* Asynchronous mode: results arrive on the main loop this is called
	 * from. */
	gst_discoverer_start(disc);
	return d;
}

void mp_discover_free(MpDiscover *d)
{
	if (d == NULL)
		return;
	if (d->pump_source != 0)
		g_source_remove(d->pump_source);
	g_signal_handlers_disconnect_by_data(d->disc, d);
	gst_discoverer_stop(d->disc);
	g_object_unref(d->disc);
	g_queue_clear_full(&d->queue, item_free);
	item_free(d->busy);
	g_free(d);
}

void mp_discover_queue(MpDiscover *d, guint id, const char *uri)
{
	if (d == NULL || uri == NULL || !mp_uri_is_local(uri))
		return;
	if (g_queue_get_length(&d->queue) >= DISCOVER_QUEUE_MAX)
		return;
	Item *it = g_new0(Item, 1);
	it->id = id;
	it->uri = g_strdup(uri);
	g_queue_push_tail(&d->queue, it);
	schedule_pump(d);
}

void mp_discover_forget(MpDiscover *d, guint id)
{
	if (d == NULL)
		return;
	for (GList *l = d->queue.head; l != NULL;) {
		GList *n = l->next;
		Item *it = l->data;
		if (it->id == id) {
			item_free(it);
			g_queue_delete_link(&d->queue, l);
		}
		l = n;
	}
	/* The probe in flight cannot be called back; its result is simply
	 * dropped when it arrives. */
	if (d->busy != NULL && d->busy->id == id)
		d->busy->id = 0;
}

void mp_discover_clear(MpDiscover *d)
{
	if (d == NULL)
		return;
	g_queue_clear_full(&d->queue, item_free);
	g_queue_init(&d->queue);
	if (d->busy != NULL)
		d->busy->id = 0;
}
