// LikeOS Explorer: a drive's properties.
//
// For every drive -- the system disk, the RAM disk, a USB drive -- in the
// look of the Properties window: the drive's picture and name in the
// header; on a card its type (USB drive, Local disk, RAM disk) and file
// system; on a second card the used space, the free space and the
// capacity, each in exact bytes and in MB or GB, the used and free rows
// marked with the colours of a ring chart of the two beside them, the
// drive's name under the ring; and OK.  The figures of a mounted drive come
// from its filesystem, asked in the background; of a USB drive that is not
// mounted only the capacity is known.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_DRIVE_PROPERTIES_H
#define EX_DRIVE_PROPERTIES_H

#include "ui/ex-window.h"
#include "backend/ex-volumes.h"

G_BEGIN_DECLS

void ex_drive_properties_show(ExWindow *w, const ExVolume *v);

G_END_DECLS

#endif
