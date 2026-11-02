// LikeOS -- File Explorer: the Properties window.
//
// General (name, type, what opens it, where it is, size -- counted in the
// background for folders -- and its dates), Permissions (owner, group and
// the read/write/execute bits, editable by the owner) and, for a file, the
// application it opens with, which can be changed for its whole type.
// Several items get a summary instead.
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
