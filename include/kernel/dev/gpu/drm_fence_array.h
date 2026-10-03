// LikeOS -- display-manager core: fence arrays.
//
// A fence that stands for several others: signalled when all of them have
// (or, when made so, as soon as any one has), carrying the first error
// among them.  An array never contains another container: flatten first
// (drm_fence_unwrap.h).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Collabora's and
// Advanced Micro Devices' code: GPL-2.0-only
// Portions Copyright (C) 2016 Collabora Ltd
// Portions Copyright (C) 2016 Advanced Micro Devices, Inc.

#ifndef KERNEL_DEV_GPU_DRM_FENCE_ARRAY_H
#define KERNEL_DEV_GPU_DRM_FENCE_ARRAY_H

#include <kernel/dev/gpu/drm.h>

struct drm_fence_array;

/* One per part: the callback that counts the part off. */
struct drm_fence_array_cb {
	struct drm_fence_cb cb;
	struct drm_fence_array *array;
};

struct drm_fence_array {
	struct drm_fence base; /* first: the array IS a fence */
	unsigned num_fences;
	int num_pending; /* parts still to signal (atomic) */
	struct drm_fence **fences; /* owned, with a reference to each */
	struct drm_fence_array_cb callbacks[];
};

extern const struct drm_fence_ops drm_fence_array_ops;

static inline bool drm_fence_is_array(const struct drm_fence *f)
{
	return f && f->ops == &drm_fence_array_ops;
}

static inline struct drm_fence_array *to_drm_fence_array(struct drm_fence *f)
{
	if (!drm_fence_is_array(f))
		return NULL;
	return (struct drm_fence_array *)((char *)f -
		__builtin_offsetof(struct drm_fence_array, base));
}

/* An array of `num_fences' parts, unset (zeroed); NULL without memory. */
struct drm_fence_array *drm_fence_array_alloc(int num_fences);
/* Set up an allocated array over `fences' (a kalloc'd vector of
 * `num_fences' >= 1 referenced fences, both of which the array takes
 * over), on stream `context' at `seqno'. */
void drm_fence_array_init(struct drm_fence_array *array, int num_fences,
			  struct drm_fence **fences, uint64_t context,
			  unsigned seqno, bool signal_on_any);
/* alloc + init; NULL without memory (the caller still owns `fences'). */
struct drm_fence_array *drm_fence_array_create(int num_fences,
					       struct drm_fence **fences,
					       uint64_t context, unsigned seqno,
					       bool signal_on_any);
/* Is every fence that `f' stands for on stream `context'? */
bool drm_fence_match_context(struct drm_fence *f, uint64_t context);
/* Iterating: the first part (or `head' itself when it is no array), and
 * the part at `index' (NULL past the end or when `head' is no array). */
struct drm_fence *drm_fence_array_first(struct drm_fence *head);
struct drm_fence *drm_fence_array_next(struct drm_fence *head, unsigned index);

#define drm_fence_array_for_each(fence, index, head)			\
	for (index = 0, fence = drm_fence_array_first(head); fence;	\
	     ++(index), fence = drm_fence_array_next(head, index))

#endif
