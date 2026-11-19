// LikeOS Explorer: turning pictures.
//
// Rotate left and Rotate right for a picture, without losing anything: a JPEG keeps its compressed data untouched and
// only the Orientation tag of its Exif data changes (added to the Exif data
// when it has none, or a minimal Exif segment inserted when the picture has
// no Exif data yet), which every viewer and the thumbnails honour; a PNG,
// BMP or TIFF -- lossless formats -- is turned and written again.  Other
// formats are not offered.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_IMAGE_H
#define EX_IMAGE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Whether a picture of `content_type' can be turned. */
gboolean ex_image_can_rotate(const char *content_type);

/* Turn `file' a quarter turn clockwise (`clockwise') or anticlockwise, in a
 * worker thread. */
void ex_image_rotate_async(GFile *file, gboolean clockwise,
			   GCancellable *cancellable, GAsyncReadyCallback done,
			   gpointer user_data);
gboolean ex_image_rotate_finish(GAsyncResult *res, GError **error);
/* The same, synchronously (tests, worker threads). */
gboolean ex_image_rotate(GFile *file, gboolean clockwise,
			 GCancellable *cancellable, GError **error);

/* The Exif Orientation of a JPEG file (1 to 8), 1 when it has none or is
 * no JPEG.  5 to 8 mean the picture is shown turned a quarter, so its width
 * and height swap. */
int ex_image_jpeg_orientation(const char *path);

/* The orientation after a quarter turn of one with `orientation'. */
int ex_image_orientation_turn(int orientation, gboolean clockwise);

G_END_DECLS

#endif
