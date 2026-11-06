// LikeOS -- LikeOS Image Viewer: the straighten dial of the Crop tool.
//
// A horizontal ruler that slides under a fixed centre mark: a minor tick
// every degree, a major one every five, numbers every fifteen, the angle
// above the mark.  The value runs from -45 to +45 degrees in tenths.
//
//   drag      slides the ruler (Shift: ten times finer, no snapping);
//             the value snaps to 0 when it passes close by
//   click     brings the tick under the pointer to the mark
//   wheel     one degree a notch (Shift: a tenth)
//   keys      Left/Right (Down/Up) one degree, Shift a tenth,
//             Page Up/Down five, Home back to 0
//   double-click  back to 0
//
// Every change calls the changed callback with final FALSE; the end of a
// gesture (release, or a short rest after keys and the wheel) calls it once
// more with final TRUE, so the caller can make one undo step of it.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IV_TOOL_CROP_DIAL_H
#define IV_TOOL_CROP_DIAL_H

#include <gtk/gtk.h>

#define IV_DIAL_LIMIT 45.0

typedef void (*IvDialChanged)(GtkWidget *dial, double value, gboolean final,
			      gpointer data);

GtkWidget *iv_dial_new(IvDialChanged changed, gpointer data);
/* Shows `value' without calling the callback (for undo and redo). */
void iv_dial_set_value(GtkWidget *dial, double value);
double iv_dial_get_value(GtkWidget *dial);
/* TRUE while a drag, or keys/wheel without their final call yet. */
gboolean iv_dial_busy(GtkWidget *dial);
/* Ends a pending key/wheel gesture now (its final call happens). */
void iv_dial_flush(GtkWidget *dial);
/* The dial's own keys, also when the event reached the window first. */
gboolean iv_dial_key(GtkWidget *dial, GdkEventKey *ev);

#endif
