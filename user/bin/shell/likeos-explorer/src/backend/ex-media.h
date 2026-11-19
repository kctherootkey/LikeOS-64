// LikeOS Explorer: what a song or a video is.
//
// Length, picture size and the title, artist and album tags, read with
// GStreamer's discoverer in a worker thread -- for the preview and details
// panes.  Without GStreamer every request fails as not supported.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_MEDIA_H
#define EX_MEDIA_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
	gint64 duration_ms; /* 0 when not known */
	int width, height; /* 0 for audio */
	char *title, *artist, *album;
} ExMediaInfo;

void ex_media_info_free(ExMediaInfo *m);

void ex_media_describe_async(GFile *file, GCancellable *cancellable,
			     GAsyncReadyCallback done, gpointer user_data);
ExMediaInfo *ex_media_describe_finish(GAsyncResult *res, GError **error);

/* "3:07", "1:02:45". */
char *ex_media_format_duration(gint64 ms);

G_END_DECLS

#endif
