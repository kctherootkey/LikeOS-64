/*
 * eventpoll.h - the poll table and the interface between poll.c (poll,
 * select, the per-object readiness questions) and eventpoll.c (epoll).
 *
 * A poller asks an object two things at once: "are you ready?" and "wake me
 * when you might be".  The second half is poll_wait(pt, owner, head): the
 * object names each wait queue a readiness change of it is announced on, and
 * the poll table decides what registering there means.  For poll() and
 * select() it is a wait-queue entry of the calling task whose wake callback
 * sets `triggered' and wakes the task.  For epoll it is an entry of the epoll
 * ITEM, permanent until the item goes, whose callback puts the item on its
 * set's ready list (`qproc').
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _KERNEL_NET_EVENTPOLL_H_
#define _KERNEL_NET_EVENTPOLL_H_

#include <kernel/ke/waitq.h>

struct poll_table;
struct poll_table_entry;

/* What poll_wait() does with a registration when the table is not the plain
 * poll()/select() one. */
typedef void (*poll_queue_proc)(struct poll_table *pt, void *owner,
				struct wait_queue_head *head);

struct poll_table {
	/* NULL: the poll()/select() registration (below).  Set: epoll's. */
	poll_queue_proc qproc;
	/* Set by the readiness question when the object is a plain file that
	 * is always ready and has nothing to wait on (epoll refuses those,
	 * EPERM). */
	int nopoll;
	/* The events the current question is about: a registration's wake
	 * key filter (poll/select), the item's events (epoll). */
	unsigned key;

	/* poll() / select() ---------------------------------------------- */
	struct poll_table_entry *ents;
	int n;   /* entries in use */
	int cap; /* 0 when the allocation failed -- see poll_wait() */
	/* A registration was refused (table full or missing, object gone):
	 * the sleep falls back to a one-tick re-scan for that iteration. */
	int unregistered;
	/* Set by a wake of any queue this poller is on, cleared before each
	 * scan.  The poller checks it after publishing its sleep. */
	volatile int triggered;
};

/* Register on `head', a queue of the object `owner' (poll.c). */
void poll_wait(struct poll_table *pt, void *owner, struct wait_queue_head *h);

/* The readiness of one descriptor-table entry, as poll() reports it.
 * `fd' is only used for the console's empty slot (entry NULL).  `pt' may be
 * NULL (no registration).  `depth' counts epoll sets nested inside each other
 * while their readiness is asked (0 at the top). */
short poll_entry(void *entry, int fd, short events, struct poll_table *pt,
		 int depth);

/* Lifetime of the object behind an fd-table entry, for epoll (poll.c).
 *
 * tryhold: take a reference if the object is still live (its last reference
 * not yet dropped); never resurrects one whose release has begun.  Must be
 * called only while the object's memory is known to exist.  put: drop it
 * (may run the object's final release).
 *
 * item_hold/item_put: what an epoll item keeps for as long as it exists.
 * Nothing (item_hold returns 1), for objects whose final release removes the
 * items watching them (eventpoll_release()).  A memory-only reference (2)
 * for the ones that cannot, which are then checked with poll_obj_closed().
 * 0: the object is already gone. */
int poll_obj_tryhold(void *entry);
void poll_obj_put(void *entry);
int poll_obj_item_hold(void *entry);
void poll_obj_item_put(void *entry);
int poll_obj_closed(void *entry);

/* epoll (eventpoll.c) ------------------------------------------------------ */

/* An epoll set's own readiness: POLLIN when an event is waiting.  `pt' is
 * registered on the set's poll queue. */
short ep_eventpoll_poll(int idx, short events, struct poll_table *pt,
			int depth);

/* The object an fd-table entry names is being released for good: every
 * epoll item watching it goes.  Called once per object, before it is freed,
 * from its last reference drop.  Must be called with no lock held. */
void eventpoll_release(void *entry);

/* A wait queue is about to be freed while epoll items may still be on it
 * (one that belongs to something other than the object polled): they are
 * taken off it first.  No lock held. */
void eventpoll_wq_free(struct wait_queue_head *h);

/* Instance references (descriptors and lookups). */
int epoll_tryget(int idx);

/* ppoll/pselect/epoll_pwait: install the wait's signal mask (poll.c).
 * 1 installed, 0 none given, or -errno. */
int poll_sigmask_install(uint64_t umask_ptr, kernel_sigset_t *saved);

#endif /* _KERNEL_NET_EVENTPOLL_H_ */
