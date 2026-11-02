// LikeOS Explorer: the Properties window.
//
// A header with the item's picture large and its name in a field to edit,
// the type under it; a two-way switch between General and Access; and the
// page under it, its facts in labelled rows on white cards.  General: type,
// the app it opens with (chosen for the whole type with "Choose…"), where
// it is, its size -- counted in the background for folders -- and its
// dates.  Access: owner, group and who may read, change and run it,
// editable by the owner.  Several items get a summary instead.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_PROPERTIES_H
#define EX_PROPERTIES_H

#include "ui/ex-window.h"
#include "backend/ex-file.h"

G_BEGIN_DECLS

/* `files' of ExFile. */
void ex_properties_show(ExWindow *w, GPtrArray *files);

G_END_DECLS

#endif
