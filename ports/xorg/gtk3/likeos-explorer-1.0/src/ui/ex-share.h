// LikeOS -- File Explorer: Share.
//
// Where items can go from here, as a short list of targets: an e-mail
// attachment through the mail program the system uses for mailto: links
// (when its desktop entry says how to attach files, X-LikeOS-Attach), the
// items' paths on the clipboard, or opening them in another program.  No
// program is named here; what is offered depends on what is installed.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_SHARE_H
#define EX_SHARE_H

#include "ui/ex-window.h"

G_BEGIN_DECLS

/* `files' of GFile. */
void ex_share_show(ExWindow *w, GPtrArray *files);
/* Whether a mail program can take attachments. */
gboolean ex_share_can_mail(void);

G_END_DECLS

#endif
