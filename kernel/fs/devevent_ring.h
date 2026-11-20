// LikeOS -- device events: the record ring behind /dev/devevents.
//
// Pure bookkeeping, no locking and no kernel services, so the host test
// (host/test-devevent.sh) runs the same code the kernel does.  The caller
// (kernel/fs/devevent.c) holds its ring lock around every call.
//
// Records are numbered 1, 2, 3 ... in publication order (`seq').  The ring
// keeps the last DEVEVENT_RING_SIZE of them: record `s' lives in slot
// (s - 1) % DEVEVENT_RING_SIZE.  A reader is nothing but a cursor -- the seq
// of the next record it wants.  A cursor older than the oldest kept record
// has lost records: the reader gets one DEVICE_EVENT_OVERFLOW record whose
// seq is the first one it missed and is moved to the oldest kept record.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_DEVEVENT_RING_H
#define KERNEL_FS_DEVEVENT_RING_H

#include <kernel/uapi/devevent.h>

/* A power of two. */
#define DEVEVENT_RING_SIZE 512u

struct devevent_ring {
	/* The seq the next record gets; 1 before the first.  Written under
	 * the caller's lock, read without it (a reader's "anything new?"). */
	uint64_t next_seq;
	struct device_event rec[DEVEVENT_RING_SIZE];
};

_Static_assert((DEVEVENT_RING_SIZE & (DEVEVENT_RING_SIZE - 1)) == 0,
	       "DEVEVENT_RING_SIZE must be a power of two");

/* The seq of the oldest record still kept (== next_seq when none is). */
static inline uint64_t devevent_ring_oldest(const struct devevent_ring *r)
{
	uint64_t next = r->next_seq ? r->next_seq : 1;

	return next > DEVEVENT_RING_SIZE ? next - DEVEVENT_RING_SIZE : 1;
}

/* The cursor of a reader that starts now: the next record published. */
static inline uint64_t devevent_ring_tail(const struct devevent_ring *r)
{
	return r->next_seq ? r->next_seq : 1;
}

/* Append `ev' (fully initialised by the caller, seq ignored); returns the
 * seq it got.  The oldest record is overwritten once the ring is full. */
static inline uint64_t devevent_ring_put(struct devevent_ring *r,
					 const struct device_event *ev)
{
	uint64_t s = devevent_ring_tail(r);
	struct device_event *d = &r->rec[(s - 1) & (DEVEVENT_RING_SIZE - 1)];

	*d = *ev;
	d->seq = s;
	__atomic_store_n(&r->next_seq, s + 1, __ATOMIC_RELEASE);
	return s;
}

/* Has the reader at `cursor' anything to read (a record or an overflow)?
 * May be called without the lock. */
static inline int devevent_ring_pending(const struct devevent_ring *r,
					uint64_t cursor)
{
	uint64_t next = __atomic_load_n(&r->next_seq, __ATOMIC_ACQUIRE);

	return cursor < (next ? next : 1);
}

/* Copy up to `max' records for the reader at `cursor' into `out' and
 * return how many; *next_cursor is where the reader continues once they
 * have been delivered.  A reader that has lost records gets the
 * DEVICE_EVENT_OVERFLOW record first (timestamp `now_ns'). */
static inline unsigned devevent_ring_take(const struct devevent_ring *r,
					  uint64_t cursor,
					  struct device_event *out,
					  unsigned max, uint64_t now_ns,
					  uint64_t *next_cursor)
{
	uint64_t oldest = devevent_ring_oldest(r);
	uint64_t next = devevent_ring_tail(r);
	unsigned n = 0;

	if (cursor < 1)
		cursor = 1;
	if (max && cursor < oldest) {
		struct device_event *o = &out[n++];
		unsigned char *p = (unsigned char *)o;

		for (unsigned i = 0; i < sizeof(*o); i++)
			p[i] = 0;
		o->size = sizeof(*o);
		o->version = DEVEVENT_VERSION;
		o->type = DEVICE_EVENT_OVERFLOW;
		o->seq = cursor;
		o->timestamp_ns = now_ns;
		cursor = oldest;
	}
	while (n < max && cursor < next) {
		out[n++] = r->rec[(cursor - 1) & (DEVEVENT_RING_SIZE - 1)];
		cursor++;
	}
	*next_cursor = cursor;
	return n;
}

#endif /* KERNEL_FS_DEVEVENT_RING_H */
