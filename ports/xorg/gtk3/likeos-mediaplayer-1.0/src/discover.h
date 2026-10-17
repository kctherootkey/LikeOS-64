// LikeOS -- Media Player: reading titles and lengths of queued files ahead
// of playing them.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_DISCOVER_H
#define MP_DISCOVER_H

#include <glib.h>

typedef struct MpDiscover MpDiscover;

/* Any of the strings may be NULL and `duration' -1 when not found. */
typedef void (*MpDiscoverFunc)(guint id, const char *title,
			       const char *artist, const char *album,
			       gint64 duration, gpointer user_data);

/* NULL when GStreamer's discoverer cannot be created. */
MpDiscover *mp_discover_new(MpDiscoverFunc func, gpointer user_data);
void mp_discover_free(MpDiscover *d);
/* Queue one local file.  Streams are never probed: that would open a
 * network connection per entry. */
void mp_discover_queue(MpDiscover *d, guint id, const char *uri);
void mp_discover_forget(MpDiscover *d, guint id);
void mp_discover_clear(MpDiscover *d);

#endif
