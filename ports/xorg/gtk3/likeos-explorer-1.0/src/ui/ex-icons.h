// LikeOS -- File Explorer: icons for items.
//
// One place that turns an item into the picture a view shows: its
// thumbnail when there is one and the view is large enough for it, else its
// themed icon at the view's size, dimmed while the item waits on the
// clipboard to be cut.  Lookups are cached per icon and size, and the cache
// is dropped when the icon theme changes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_ICONS_H
#define EX_ICONS_H

#include <gtk/gtk.h>
#include "backend/ex-file.h"

G_BEGIN_DECLS

/* `size' in pixels; `scale' the widget's scale factor. */
GdkPixbuf *ex_icons_for_file(ExFile *f, int size, int scale, gboolean dimmed,
			     gboolean thumbnails);
GdkPixbuf *ex_icons_named(const char *name, int size, int scale);
GdkPixbuf *ex_icons_gicon(GIcon *icon, int size, int scale);

G_END_DECLS

#endif
