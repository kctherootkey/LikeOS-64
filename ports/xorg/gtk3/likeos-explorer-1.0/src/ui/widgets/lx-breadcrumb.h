// LikeOS -- breadcrumb address bar.
//
// A reusable LikeOS widget: the path as a row of buttons, one per place
// ("Computer › System disk › usr › share"), each chevron opening a list of the
// folders inside that place; a click on the empty part of the bar -- or
// lx_breadcrumb_edit() -- turns it into a text field holding the path, and
// Escape or leaving it turns it back.  It knows nothing about files: the
// owner supplies the crumbs, and for a chevron the names below a crumb.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LX_BREADCRUMB_H
#define LX_BREADCRUMB_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define LX_TYPE_BREADCRUMB (lx_breadcrumb_get_type())
G_DECLARE_FINAL_TYPE(LxBreadcrumb, lx_breadcrumb, LX, BREADCRUMB, GtkBin)

/* Signals:
 *   "crumb-activated" (guint index)       -- a crumb was clicked
 *   "text-activated"  (const char *text)  -- Enter in the text field
 *   "children-wanted" (guint index, GtkWidget *popover_box)
 *       -- a chevron was opened: fill the box (a vertical GtkBox) with
 *          buttons for what is below crumb `index'
 */
GtkWidget *lx_breadcrumb_new(void);

/* The crumbs, as label + icon name pairs (icon may be NULL), outermost
 * first.  `expandable' says per crumb whether it gets a chevron listing what
 * is inside it (NULL: every one does). */
void lx_breadcrumb_set_crumbs(LxBreadcrumb *b, const char *const *labels,
			      const char *const *icons,
			      const gboolean *expandable, guint n);
/* What the text field holds when editing starts. */
void lx_breadcrumb_set_edit_text(LxBreadcrumb *b, const char *text);
void lx_breadcrumb_edit(LxBreadcrumb *b);
void lx_breadcrumb_stop_edit(LxBreadcrumb *b);
gboolean lx_breadcrumb_is_editing(LxBreadcrumb *b);
/* Completion for the text field (a GtkEntryCompletion the owner fills). */
GtkEntry *lx_breadcrumb_get_entry(LxBreadcrumb *b);

G_END_DECLS

#endif
