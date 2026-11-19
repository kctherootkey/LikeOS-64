// LikeOS -- flyout.
//
// A reusable LikeOS widget: a popover with a title and a body, styled as one
// of the system's flyouts (rounded, elevated, the same padding everywhere),
// for transient panels such as the operation list or a share sheet.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LX_FLYOUT_H
#define LX_FLYOUT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A popover pointing at `relative_to', holding `title' above `body'. */
GtkWidget *lx_flyout_new(GtkWidget *relative_to, const char *title,
			 GtkWidget *body);

G_END_DECLS

#endif
