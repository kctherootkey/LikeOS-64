// LikeOS Explorer: formatting a USB drive.
//
// A LikeOS properties-style window: the drive's picture and "Format <Name>
// (<device>)" in the header; then on a card the drive's capacity, the file
// system (every one the device service can format, its default first), the
// allocation unit size (that file system's sizes, its default first), the
// volume label (as long as that file system allows) and "Restore device
// defaults"; under "Format options" Quick format; a progress bar with a
// line of status; Start and Close.  Start asks first -- formatting erases
// everything on the drive -- then the device service formats the drive and
// reports its progress; Close waits until it is done.  Its errors are shown
// with the service's own message.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_FORMAT_DIALOG_H
#define EX_FORMAT_DIALOG_H

#include "ui/ex-window.h"

G_BEGIN_DECLS

/* One window per drive: a second call brings the first one forward. */
void ex_format_dialog_show(ExWindow *w, const char *device);

G_END_DECLS

#endif
