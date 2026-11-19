// LikeOS Explorer: icons for items.
//
// One place that turns an item into the picture a view shows: its
// thumbnail when there is one and the view is large enough for it, else its
// themed icon at the view's size, dimmed while the item waits on the
// clipboard to be cut.  Lookups are cached per icon and size, and the cache
// is dropped when the icon theme changes.
//
// A thumbnail of a picture or a video is framed by a hairline round the
// picture, so a light photo still reads as an item on the white of the
// view.  And the name of an item's type is made here too, in plain words
// ("Folder", "PNG file", "Text document", "Shortcut"), so the Type column,
// the sort by type, the tiles and the panes all say the same.
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

/* Whether `f' is a shortcut (a .desktop file): it shows its program's
 * picture with the shortcut arrow, and its name without the extension. */
gboolean ex_icons_is_shortcut(ExFile *f);

/* The name of `f''s type, as the views show it (newly allocated). */
char *ex_icons_kind_label(ExFile *f);

G_END_DECLS

#endif
