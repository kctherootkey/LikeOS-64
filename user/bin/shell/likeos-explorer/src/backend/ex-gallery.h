// LikeOS Explorer: the Gallery's collection.
//
// The Gallery shows every picture and video of a few folders (its
// "collection", Pictures by default) as one timeline, newest first.  This
// gathers them: the folders are walked in a worker thread, down to eight
// levels, hidden folders and files left out, and each picture's proportions
// are read from its header (turned when its Exif data says it is shown a
// quarter turned), so the Gallery can lay its rows out before any
// thumbnail exists.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_GALLERY_H
#define EX_GALLERY_H

#include "backend/ex-file.h"

G_BEGIN_DECLS

/* `folders' of GFile.  The result is an array of ExFile, newest first. */
void ex_gallery_collect_async(GPtrArray *folders, GCancellable *cancellable,
			      GAsyncReadyCallback done, gpointer user_data);
GPtrArray *ex_gallery_collect_finish(GAsyncResult *res, GError **error);

/* Width over height of an item the collection returned (4:3 when it could
 * not be read, 16:9 for a video). */
double ex_gallery_aspect(ExFile *f);

G_END_DECLS

#endif
