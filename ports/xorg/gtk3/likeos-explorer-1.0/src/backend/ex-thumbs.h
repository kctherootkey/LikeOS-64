// LikeOS -- File Explorer: thumbnails.
//
// The freedesktop.org thumbnail cache (~/.cache/thumbnails/{normal,large,
// x-large}/<md5 of the URI>.png, stamped with the source's URI and
// modification time), which the PCManFM desktop reads and writes too.  A
// missing or stale thumbnail is made on one of two worker threads: images
// through gdk-pixbuf, videos by taking a frame a tenth of the way in through
// GStreamer.  What could not be made is remembered in fail/likeos-explorer/
// and not tried again until the file changes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_THUMBS_H
#define EX_THUMBS_H

#include "backend/ex-file.h"

G_BEGIN_DECLS

typedef enum {
	EX_THUMB_NORMAL = 128,
	EX_THUMB_LARGE = 256,
	EX_THUMB_XLARGE = 512,
} ExThumbSize;

/* Whether a thumbnail can be made for `f' at all (its type). */
gboolean ex_thumbs_supported(ExFile *f);

/* Ask for `f''s thumbnail.  When it is ready, ex_file_set_thumbnail() is
 * called on the main loop (and the file emits "changed"); when it cannot be
 * made, ex_file_set_thumbnail_failed().  A request for a file already
 * waiting moves it to the front: the most recently shown items first. */
void ex_thumbs_request(ExFile *f, ExThumbSize size);

/* Forget every waiting request (the folder changed). */
void ex_thumbs_cancel_all(void);

/* The cache path a thumbnail of `uri' has at `size' (for tests). */
char *ex_thumbs_cache_path(const char *uri, ExThumbSize size);

G_END_DECLS

#endif
