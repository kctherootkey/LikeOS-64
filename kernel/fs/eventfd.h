// LikeOS -- eventfd: the in-kernel side.
//
// A kernel user that wants to post an event to a client's eventfd (a
// synchronisation object telling an event loop that a fence signalled)
// takes a reference to the counter behind the descriptor, adds to it from
// whatever context it runs in, and drops the reference when done.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_EVENTFD_H
#define KERNEL_FS_EVENTFD_H

struct eventfd_ctx;

/* The counter behind descriptor `fd' of the caller, referenced: 0, or
 * -EBADF (no such descriptor) / -EINVAL (not an eventfd). */
int eventfd_ctx_fdget(int fd, struct eventfd_ctx **out);
/* Add one to the counter (saturating) and wake its readers; any context,
 * interrupts off included. */
void eventfd_signal(struct eventfd_ctx *ctx);
void eventfd_ctx_put(struct eventfd_ctx *ctx);

#endif
