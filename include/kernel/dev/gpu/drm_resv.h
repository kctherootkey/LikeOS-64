// LikeOS -- display-manager core: reservation objects.
//
// The fences attached to a buffer object, each tagged with what it is for:
//
//   KERNEL    memory management by the kernel itself (a clear, a move):
//             everybody waits for these before touching the buffer;
//   WRITE     a submission that writes the buffer (implicit sync);
//   READ      a submission that reads it (implicit sync);
//   BOOKKEEP  work that takes no part in implicit sync (explicitly
//             synchronised submissions, page-table updates).
//
// Every usage includes the ones before it: asking for READ fences yields
// KERNEL, WRITE and READ ones.  A new writer waits for READ (that is, for
// everybody), a new reader for WRITE -- drm_resv_usage_rw().
//
// Changing the set (reserve, add, replace) takes the object's lock, which
// sleeps; looking at it does not need the lock: the "unlocked" iterator
// takes a reference to each fence it returns and starts over when the
// set was reallocated under it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Canonical's and
// VMware's code: MIT
// Portions Copyright (C) 2012-2014 Canonical Ltd (Maarten Lankhorst)
// Portions Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_DRM_RESV_H
#define KERNEL_DEV_GPU_DRM_RESV_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/rwsem.h>

enum drm_resv_usage {
	DRM_RESV_USAGE_KERNEL,
	DRM_RESV_USAGE_WRITE,
	DRM_RESV_USAGE_READ,
	DRM_RESV_USAGE_BOOKKEEP
};

/* Implicit sync: what a new access waits for.  A new write waits for
 * every read and write (READ includes WRITE); a new read only for the
 * writes. */
static inline enum drm_resv_usage drm_resv_usage_rw(bool write)
{
	return write ? DRM_RESV_USAGE_READ : DRM_RESV_USAGE_WRITE;
}

/* The stricter of two usages. */
static inline enum drm_resv_usage drm_resv_usage_max(enum drm_resv_usage a,
						     enum drm_resv_usage b)
{
	return a > b ? a : b;
}

struct drm_resv_list;

struct drm_resv {
	/* Held to change the set; a sleeping lock (process context). */
	mm_rwsem_t lock;
	/* Held for an instant by whoever swaps `fences' or stores an entry,
	 * and by the unlocked readers to take a reference to one. */
	spinlock_t slock;
	struct drm_resv_list *fences;
	/* Bumped whenever `fences' is replaced: a reader's cursor that
	 * still names the old list starts over. */
	uint32_t gen;
};

struct drm_resv_iter {
	struct drm_resv *obj;
	enum drm_resv_usage usage; /* the strictest usage returned */
	struct drm_fence *fence; /* current, referenced when unlocked */
	enum drm_resv_usage fence_usage; /* its usage */
	unsigned index;
	struct drm_resv_list *fences; /* the list being walked */
	uint32_t gen; /* obj->gen when it was taken */
	unsigned num_fences; /* its length then */
	bool is_restarted; /* the walk started over at this fence */
};

void drm_resv_init(struct drm_resv *obj);
void drm_resv_fini(struct drm_resv *obj);

static inline void drm_resv_lock(struct drm_resv *obj)
{
	mm_write_lock(&obj->lock);
}

static inline void drm_resv_unlock(struct drm_resv *obj)
{
	mm_write_unlock(&obj->lock);
}

static inline bool drm_resv_held(struct drm_resv *obj)
{
	return mm_rwsem_is_write_locked(&obj->lock);
}

/* Room for `num_fences' more fences, so that drm_resv_add_fence() need
 * not allocate.  Locked; 0 or -ENOMEM. */
int drm_resv_reserve_fences(struct drm_resv *obj, unsigned int num_fences);
/* Add `fence' (referenced by the call) with `usage', replacing a fence of
 * the same timeline it supersedes, or a signalled one.  Locked, with a
 * slot reserved. */
void drm_resv_add_fence(struct drm_resv *obj, struct drm_fence *fence,
			enum drm_resv_usage usage);
/* Replace every fence on stream `context' by `replacement' with `usage'
 * (e.g. once preemption fences have done their job).  Locked. */
void drm_resv_replace_fences(struct drm_resv *obj, uint64_t context,
			     struct drm_fence *replacement,
			     enum drm_resv_usage usage);

/* Unlocked iteration: each fence returned is referenced until the next
 * step or drm_resv_iter_end(). */
struct drm_fence *drm_resv_iter_first_unlocked(struct drm_resv_iter *cursor);
struct drm_fence *drm_resv_iter_next_unlocked(struct drm_resv_iter *cursor);
/* Locked iteration (no references taken). */
struct drm_fence *drm_resv_iter_first(struct drm_resv_iter *cursor);
struct drm_fence *drm_resv_iter_next(struct drm_resv_iter *cursor);

static inline void drm_resv_iter_begin(struct drm_resv_iter *cursor,
				       struct drm_resv *obj,
				       enum drm_resv_usage usage)
{
	cursor->obj = obj;
	cursor->usage = usage;
	cursor->fence = NULL;
	cursor->fences = NULL;
	cursor->gen = 0;
	cursor->index = 0;
	cursor->num_fences = 0;
	cursor->is_restarted = false;
}

static inline void drm_resv_iter_end(struct drm_resv_iter *cursor)
{
	drm_fence_put(cursor->fence);
	cursor->fence = NULL;
}

static inline enum drm_resv_usage drm_resv_iter_usage(struct drm_resv_iter *cursor)
{
	return cursor->fence_usage;
}

static inline bool drm_resv_iter_is_restarted(struct drm_resv_iter *cursor)
{
	return cursor->is_restarted;
}

#define drm_resv_for_each_fence_unlocked(cursor, fence)			\
	for (fence = drm_resv_iter_first_unlocked(cursor); fence;	\
	     fence = drm_resv_iter_next_unlocked(cursor))

#define drm_resv_for_each_fence(cursor, obj, usage, fence)		\
	for (drm_resv_iter_begin(cursor, obj, usage),			\
	     fence = drm_resv_iter_first(cursor); fence;		\
	     fence = drm_resv_iter_next(cursor))

/* Make `dst''s set a copy of `src''s.  `dst' locked. */
int drm_resv_copy_fences(struct drm_resv *dst, struct drm_resv *src);
/* Every unsignalled fence up to `usage', referenced, in a kalloc'd
 * vector (NULL when there are none). */
int drm_resv_get_fences(struct drm_resv *obj, enum drm_resv_usage usage,
			unsigned int *num_fences, struct drm_fence ***fences);
/* All of them as one fence (an array when several), or NULL. */
int drm_resv_get_singleton(struct drm_resv *obj, enum drm_resv_usage usage,
			   struct drm_fence **fence);
/* Wait for every fence up to `usage': 0, -ETIMEDOUT or -ERESTARTSYS.  A
 * zero timeout only checks. */
int drm_resv_wait_timeout(struct drm_resv *obj, enum drm_resv_usage usage,
			  int intr, uint64_t timeout_ns);
void drm_resv_set_deadline(struct drm_resv *obj, enum drm_resv_usage usage,
			   uint64_t deadline_ns);
/* Has every fence up to `usage' signalled? */
bool drm_resv_test_signaled(struct drm_resv *obj, enum drm_resv_usage usage);

#endif
