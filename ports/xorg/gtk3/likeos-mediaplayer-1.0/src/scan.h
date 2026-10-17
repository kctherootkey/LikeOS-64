// LikeOS -- Media Player: turning files, folders and playlists into
// queue entries, off the GTK thread.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_SCAN_H
#define MP_SCAN_H

#include <gio/gio.h>

typedef struct {
	GPtrArray *uris;	/* gchar*, in play order */
	GPtrArray *titles;	/* gchar* or NULL, parallel to uris */
	GPtrArray *problems;	/* gchar*, one line per input that failed */
	gboolean truncated;	/* the entry limit was reached */
} MpScanResult;

typedef void (*MpScanDone)(MpScanResult *result, gpointer user_data);

/* `inputs' are URIs (file:// or any stream) as the user gave them.  Folders
 * are walked recursively for media files, playlist files are read, other
 * files and streams are taken as they are.  `done' runs on the main loop
 * and owns nothing: the result is freed after it returns. */
void mp_scan_async(gchar **inputs, GCancellable *cancel, MpScanDone done,
		   gpointer user_data);

#endif
