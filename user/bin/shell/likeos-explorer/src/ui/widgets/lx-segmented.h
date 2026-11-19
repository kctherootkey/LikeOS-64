// LikeOS -- segmented control.
//
// A reusable LikeOS widget: a row of linked toggle buttons of which exactly
// one is active -- the status bar's view switch, a settings choice.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LX_SEGMENTED_H
#define LX_SEGMENTED_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define LX_TYPE_SEGMENTED (lx_segmented_get_type())
G_DECLARE_FINAL_TYPE(LxSegmented, lx_segmented, LX, SEGMENTED, GtkBox)

/* "changed" (int index) when the user picks another segment. */
GtkWidget *lx_segmented_new(void);
/* A segment with an icon (and a tooltip, which is also its accessible
 * name).  Returns its index. */
int lx_segmented_append(LxSegmented *s, const char *icon_name,
			const char *tooltip);
/* The same with any GIcon (a themed icon with fallbacks, say). */
int lx_segmented_append_gicon(LxSegmented *s, GIcon *icon, const char *tooltip);
/* A segment named by its text. */
int lx_segmented_append_label(LxSegmented *s, const char *label);
void lx_segmented_set_active(LxSegmented *s, int index);
int lx_segmented_get_active(LxSegmented *s);

G_END_DECLS

#endif
