/* LikeOS installer -- fitting the wizard to any screen.
 *
 * The installer runs full screen on whatever the machine offers: a laptop
 * panel, a large monitor, or the 800x600 a virtual machine starts with.
 * The Setup panel and the card take a share of the screen up to a largest
 * size (FrameBin does that), the card drops its illustration column on a
 * narrow screen, every page body scrolls, and the buttons stay pinned to
 * the bottom.  Three tiers set the type sizes and spacing through CSS
 * classes on the window: il-compact, (regular), il-large.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_LAYOUT_H
#define LIKEOS_INSTALLER_LAYOUT_H

#include "app.h"

/* Below this width the card shows one column. */
#define LAYOUT_NARROW_WIDTH   1000
/* Below this height spacing shrinks. */
#define LAYOUT_SHORT_HEIGHT   700
#define LAYOUT_LARGE_WIDTH    1600
#define LAYOUT_LARGE_HEIGHT   1000

/* A container with one child, given a share of its own size: frac_w and
 * frac_h of the space (minus the margins), never more than max_w x max_h,
 * never less than the child needs, centred. */
#define FRAME_TYPE_BIN (frame_bin_get_type())
G_DECLARE_FINAL_TYPE(FrameBin, frame_bin, FRAME, BIN, GtkBin)

GtkWidget *frame_bin_new(double frac_w, double frac_h, int max_w, int max_h);
void frame_bin_set_share(FrameBin *bin, double frac_w, double frac_h,
			 int max_w, int max_h);
/* Space kept free around the child; bottom is added below. */
void frame_bin_set_margins(FrameBin *bin, int margin, int extra_bottom);
/* Never taller than the child's natural height (dialogs). */
void frame_bin_set_fit_height(FrameBin *bin, gboolean fit);
/* Compact sizes: used while the window is in the compact tier. */
void frame_bin_set_compact(FrameBin *bin, gboolean compact);

/* Window geometry -> tier; updates the CSS classes of the window and
 * returns TRUE when anything changed. */
gboolean layout_update(App *app, int width, int height);

/* The screen the installer covers. */
void layout_screen_size(GtkWidget *window, int *w, int *h);

/* Fit a toplevel to the screen (no window manager may be running). */
void layout_cover_screen(App *app);

#endif
