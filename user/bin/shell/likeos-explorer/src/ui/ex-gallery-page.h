// LikeOS Explorer: Gallery.
//
// Every picture and video of the collection (the
// Pictures and Videos folders by default, chosen under Collection in the
// command bar) as one timeline, newest first, in rows of thumbnails that
// keep each picture's proportions and fill the width exactly, a heading
// over each month, and the years down the right side to jump between.  A
// click selects (Ctrl adds, Shift extends), a double click or Enter opens,
// a right click opens the item menu -- the selection is the tab's, so the
// command bar, the panes and the status bar follow it.
//
// Signals:
//   "activate" (GPtrArray *files)                 ExFile to open
//   "popup"    (GdkEventButton *ev, gboolean on_item)   ev NULL: menu key
//   "loaded"   ()                                  the collection was read
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_GALLERY_PAGE_H
#define EX_GALLERY_PAGE_H

#include <gtk/gtk.h>
#include "services/ex-selection.h"

G_BEGIN_DECLS

#define EX_TYPE_GALLERY_PAGE (ex_gallery_page_get_type())
G_DECLARE_FINAL_TYPE(ExGalleryPage, ex_gallery_page, EX, GALLERY_PAGE, GtkBox)

GtkWidget *ex_gallery_page_new(ExSelection *sel);
/* Read the collection again. */
void ex_gallery_page_reload(ExGalleryPage *p);
/* Forget it (the tab shows something else). */
void ex_gallery_page_clear(ExGalleryPage *p);
gboolean ex_gallery_page_loading(ExGalleryPage *p);
guint ex_gallery_page_count(ExGalleryPage *p);
/* Every item shown (ExFile, new array of new references). */
GPtrArray *ex_gallery_page_list(ExGalleryPage *p);

void ex_gallery_page_select_all(ExGalleryPage *p);
void ex_gallery_page_select_none(ExGalleryPage *p);
void ex_gallery_page_invert_selection(ExGalleryPage *p);
void ex_gallery_page_focus(ExGalleryPage *p);

G_END_DECLS

#endif
