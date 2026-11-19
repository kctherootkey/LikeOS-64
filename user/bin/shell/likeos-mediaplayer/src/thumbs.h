// LikeOS -- LikeOS Media Player: still pictures of videos for the library
// tiles.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_THUMBS_H
#define MP_THUMBS_H

#include <gtk/gtk.h>

typedef struct MpThumbs MpThumbs;

/* A picture was made (or could not be: `pb' NULL).  Runs on the main
 * loop. */
typedef void (*MpThumbReady)(const char *uri, GdkPixbuf *pb, gpointer data);

MpThumbs *mp_thumbs_new(MpThumbReady ready, gpointer data);
void mp_thumbs_free(MpThumbs *t);

/* The picture for a local video, a borrowed reference; NULL while it is
 * not there yet, in which case it is looked for in the cache on disk or
 * made in the background, and `ready' is called when it is done.  A file
 * that has no picture to give is not tried twice. */
GdkPixbuf *mp_thumbs_get(MpThumbs *t, const char *uri);

#endif
