// LikeOS -- File Explorer: reading a folder.
//
// Asynchronous and in batches: the first names of a large folder are on
// screen while the rest is still being read, and the main loop never waits
// for the disk.  Cancelling stops the reading at the next batch.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_DIR_LOADER_H
#define EX_DIR_LOADER_H

#include "backend/ex-file.h"

G_BEGIN_DECLS

/* Called on the main loop with each batch of new ExFile items (the array
 * and the items stay the caller's to keep a reference on). */
typedef void (*ExDirBatchFunc)(GPtrArray *files, gpointer user_data);

/* Read `dir'.  `batch' runs for every batch; `done' when the whole folder
 * has been read, failed, or was cancelled -- ex_dir_load_finish() tells. */
void ex_dir_load_async(GFile *dir, GCancellable *cancellable,
		       ExDirBatchFunc batch, GAsyncReadyCallback done,
		       gpointer user_data);
gboolean ex_dir_load_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif
