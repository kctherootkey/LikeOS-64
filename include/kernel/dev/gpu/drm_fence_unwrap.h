// LikeOS -- display-manager core: flattening fence containers.
//
// Looking through chains and arrays to the plain fences they are made of,
// and merging several fences into one -- an array without duplicates,
// keeping only the latest fence of each timeline.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Advanced Micro
// Devices' code: GPL-2.0-only
// Portions Copyright (C) 2022 Advanced Micro Devices, Inc.

#ifndef KERNEL_DEV_GPU_DRM_FENCE_UNWRAP_H
#define KERNEL_DEV_GPU_DRM_FENCE_UNWRAP_H

#include <kernel/dev/gpu/drm.h>

struct drm_fence_unwrap {
	struct drm_fence *chain; /* the link being looked through, referenced */
	struct drm_fence *array; /* what that link holds (maybe an array) */
	unsigned index; /* last part of `array' returned */
};

struct drm_fence *drm_fence_unwrap_first(struct drm_fence *head,
					 struct drm_fence_unwrap *cursor);
struct drm_fence *drm_fence_unwrap_next(struct drm_fence_unwrap *cursor);
/* Leaving a drm_fence_unwrap_for_each() loop before its end: drop what
 * the cursor still holds. */
void drm_fence_unwrap_end(struct drm_fence_unwrap *cursor);

/* Every plain fence inside `head' (`head' itself when it is plain).  The
 * fences are valid only inside the loop; take a reference to keep one. */
#define drm_fence_unwrap_for_each(fence, cursor, head)			\
	for (fence = drm_fence_unwrap_first(head, cursor); fence;	\
	     fence = drm_fence_unwrap_next(cursor))

/* Sort `fences' (each referenced) by timeline and keep only the latest
 * of each, dropping the others; returns how many are left. */
size_t drm_fence_dedup_array(struct drm_fence **fences, size_t num_fences);

/* One fence for all of `fences': NULL without memory; a signalled
 * private fence (timestamped with the latest part) when nothing is
 * pending; the one pending fence when there is one; otherwise a new
 * array of every pending part, de-duplicated by timeline.  Referenced. */
struct drm_fence *__drm_fence_unwrap_merge(size_t num_fences,
					   struct drm_fence **fences,
					   struct drm_fence_unwrap *cursors);
struct drm_fence *drm_fence_unwrap_merge(size_t num_fences,
					 struct drm_fence **fences);

#endif
