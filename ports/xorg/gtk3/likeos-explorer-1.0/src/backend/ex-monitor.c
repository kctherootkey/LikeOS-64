// LikeOS -- File Explorer: watching a folder for changes made elsewhere.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-monitor.h"

/* How often a folder without a live monitor is looked at. */
#define EX_POLL_SECONDS 3

struct _ExMonitor {
	GObject parent;
	GFile *dir;
	GFileMonitor *mon;
	guint poll_id;
	guint64 last_mtime;
	guint64 last_mtime_usec;
	gboolean poll_busy;
	GCancellable *cancel;
};

G_DEFINE_TYPE(ExMonitor, ex_monitor, G_TYPE_OBJECT)

enum { SIG_CREATED, SIG_DELETED, SIG_CHANGED, SIG_RENAMED, SIG_GONE,
       SIG_RESCAN, N_SIGS };
static guint sigs[N_SIGS];

static void ex_monitor_dispose(GObject *o)
{
	ExMonitor *m = EX_MONITOR(o);

	if (m->poll_id) {
		g_source_remove(m->poll_id);
		m->poll_id = 0;
	}
	if (m->cancel) {
		g_cancellable_cancel(m->cancel);
		g_clear_object(&m->cancel);
	}
	if (m->mon) {
		g_signal_handlers_disconnect_by_data(m->mon, m);
		g_file_monitor_cancel(m->mon);
		g_clear_object(&m->mon);
	}
	g_clear_object(&m->dir);
	G_OBJECT_CLASS(ex_monitor_parent_class)->dispose(o);
}

static void ex_monitor_class_init(ExMonitorClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_monitor_dispose;
	sigs[SIG_CREATED] = g_signal_new("created", t, G_SIGNAL_RUN_LAST, 0,
					 NULL, NULL, NULL, G_TYPE_NONE, 1,
					 G_TYPE_FILE);
	sigs[SIG_DELETED] = g_signal_new("deleted", t, G_SIGNAL_RUN_LAST, 0,
					 NULL, NULL, NULL, G_TYPE_NONE, 1,
					 G_TYPE_FILE);
	sigs[SIG_CHANGED] = g_signal_new("changed", t, G_SIGNAL_RUN_LAST, 0,
					 NULL, NULL, NULL, G_TYPE_NONE, 1,
					 G_TYPE_FILE);
	sigs[SIG_RENAMED] = g_signal_new("renamed", t, G_SIGNAL_RUN_LAST, 0,
					 NULL, NULL, NULL, G_TYPE_NONE, 2,
					 G_TYPE_FILE, G_TYPE_FILE);
	sigs[SIG_GONE] = g_signal_new("gone", t, G_SIGNAL_RUN_LAST, 0, NULL,
				      NULL, NULL, G_TYPE_NONE, 0);
	sigs[SIG_RESCAN] = g_signal_new("rescan", t, G_SIGNAL_RUN_LAST, 0,
					NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void ex_monitor_init(ExMonitor *m)
{
}

static void on_event(GFileMonitor *mon, GFile *file, GFile *other,
		     GFileMonitorEvent ev, gpointer data)
{
	ExMonitor *m = data;
	gboolean self = g_file_equal(file, m->dir);

	switch (ev) {
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_MOVED_IN:
		if (!self)
			g_signal_emit(m, sigs[SIG_CREATED], 0, file);
		break;
	case G_FILE_MONITOR_EVENT_DELETED:
	case G_FILE_MONITOR_EVENT_MOVED_OUT:
		if (self)
			g_signal_emit(m, sigs[SIG_GONE], 0);
		else
			g_signal_emit(m, sigs[SIG_DELETED], 0, file);
		break;
	case G_FILE_MONITOR_EVENT_RENAMED:
		if (self)
			g_signal_emit(m, sigs[SIG_GONE], 0);
		else if (other)
			g_signal_emit(m, sigs[SIG_RENAMED], 0, file, other);
		break;
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
	case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
		if (!self)
			g_signal_emit(m, sigs[SIG_CHANGED], 0, file);
		break;
	case G_FILE_MONITOR_EVENT_MOVED:
		/* Only reported without WATCH_MOVES; asked for, so a full
		 * re-read is the safe answer. */
		g_signal_emit(m, sigs[SIG_RESCAN], 0);
		break;
	case G_FILE_MONITOR_EVENT_UNMOUNTED:
		g_signal_emit(m, sigs[SIG_GONE], 0);
		break;
	default:
		break;
	}
}

static void polled(GObject *src, GAsyncResult *res, gpointer data)
{
	ExMonitor *m = data;
	GError *err = NULL;
	GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, &err);

	m->poll_busy = FALSE;
	if (!info || !m->dir) {
		/* !m->dir: disposed while the query was out. */
		if (m->dir &&
		    g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
			g_signal_emit(m, sigs[SIG_GONE], 0);
		g_clear_error(&err);
		g_clear_object(&info);
		g_object_unref(m);
		return;
	}
	guint64 t = g_file_info_get_attribute_uint64(
		info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	guint64 us = g_file_info_get_attribute_uint32(
		info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
	if (t != m->last_mtime || us != m->last_mtime_usec) {
		gboolean first = m->last_mtime == 0 && m->last_mtime_usec == 0;

		m->last_mtime = t;
		m->last_mtime_usec = us;
		if (!first)
			g_signal_emit(m, sigs[SIG_RESCAN], 0);
	}
	g_object_unref(info);
	g_object_unref(m);
}

static gboolean poll_tick(gpointer data)
{
	ExMonitor *m = data;

	if (!m->poll_busy) {
		m->poll_busy = TRUE;
		g_file_query_info_async(
			m->dir,
			G_FILE_ATTRIBUTE_TIME_MODIFIED
			"," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
			G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW, m->cancel,
			polled, g_object_ref(m));
	}
	return G_SOURCE_CONTINUE;
}

ExMonitor *ex_monitor_new(GFile *dir)
{
	ExMonitor *m = g_object_new(EX_TYPE_MONITOR, NULL);
	GError *err = NULL;

	m->dir = g_object_ref(dir);
	m->cancel = g_cancellable_new();
	m->mon = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES,
					  NULL, &err);
	if (m->mon) {
		/* Coalesce a stream of writes to one file into one update. */
		g_file_monitor_set_rate_limit(m->mon, 500);
		g_signal_connect(m->mon, "changed", G_CALLBACK(on_event), m);
		return m;
	}
	g_debug("no live monitor for %s (%s); polling",
		g_file_peek_path(dir) ? g_file_peek_path(dir) : "?",
		err ? err->message : "unknown");
	g_clear_error(&err);
	poll_tick(m); /* the time later polls compare with */
	m->poll_id = g_timeout_add_seconds(EX_POLL_SECONDS, poll_tick, m);
	return m;
}

gboolean ex_monitor_is_live(ExMonitor *m)
{
	return m->mon != NULL;
}
