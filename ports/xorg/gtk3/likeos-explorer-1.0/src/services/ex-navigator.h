// LikeOS -- File Explorer: where a tab is and where it has been.
//
// Back, Forward and Up over location ids: the same folder renamed in another
// window is still found, and a search or the Trash is a place in the history
// like any folder.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_NAVIGATOR_H
#define EX_NAVIGATOR_H

#include "backend/ex-location.h"

G_BEGIN_DECLS

#define EX_TYPE_NAVIGATOR (ex_navigator_get_type())
G_DECLARE_FINAL_TYPE(ExNavigator, ex_navigator, EX, NAVIGATOR, GObject)

/* "changed" () after every move. */
ExNavigator *ex_navigator_new(ExLocation *start);

ExLocation *ex_navigator_current(ExNavigator *n);
/* Go somewhere new: the forward history is dropped. */
void ex_navigator_go(ExNavigator *n, ExLocation *loc);
/* Replace the current place without a history entry (a search refined, a
 * folder that was renamed under us). */
void ex_navigator_replace(ExNavigator *n, ExLocation *loc);
gboolean ex_navigator_can_back(ExNavigator *n);
gboolean ex_navigator_can_forward(ExNavigator *n);
gboolean ex_navigator_can_up(ExNavigator *n);
void ex_navigator_back(ExNavigator *n);
void ex_navigator_forward(ExNavigator *n);
void ex_navigator_up(ExNavigator *n);
/* Jump `steps' back (negative) or forward (positive). */
void ex_navigator_jump(ExNavigator *n, int steps);
/* Recent places behind / ahead, nearest first (not new references). */
GPtrArray *ex_navigator_back_list(ExNavigator *n);
GPtrArray *ex_navigator_forward_list(ExNavigator *n);

G_END_DECLS

#endif
