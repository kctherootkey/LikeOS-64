// LikeOS -- display-manager core: flattening fence containers.
//
// Looking through chains and arrays to the fences they are made of, and
// merging several fences into one array without duplicates, keeping only
// the latest fence of each timeline.  SYNC_IOC_MERGE and the sync_file
// listing are built on this.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Advanced Micro
// Devices' code: GPL-2.0-only
// Portions Copyright (C) 2022 Advanced Micro Devices, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_fence_array.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/dev/gpu/drm_fence_unwrap.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/gpu/gpu_util.h>

/* Start on the link the cursor stands at: what it holds, from the top. */
static struct drm_fence *unwrap_array(struct drm_fence_unwrap *cursor)
{
	cursor->array = drm_fence_chain_contained(cursor->chain);
	cursor->index = 0;
	return drm_fence_array_first(cursor->array);
}

struct drm_fence *drm_fence_unwrap_first(struct drm_fence *head,
					 struct drm_fence_unwrap *cursor)
{
	cursor->chain = drm_fence_ref(head);
	return unwrap_array(cursor);
}

struct drm_fence *drm_fence_unwrap_next(struct drm_fence_unwrap *cursor)
{
	struct drm_fence *tmp;

	++cursor->index;
	tmp = drm_fence_array_next(cursor->array, cursor->index);
	if (tmp)
		return tmp;

	cursor->chain = drm_fence_chain_walk(cursor->chain);
	return unwrap_array(cursor);
}

void drm_fence_unwrap_end(struct drm_fence_unwrap *cursor)
{
	drm_fence_put(cursor->chain);
	cursor->chain = NULL;
	cursor->array = NULL;
}

/* By timeline, and within one the latest first. */
static int fence_cmp(struct drm_fence *a, struct drm_fence *b)
{
	uint64_t ta = drm_fence_timeline(a), tb = drm_fence_timeline(b);

	if (ta < tb)
		return -1;
	if (ta > tb)
		return 1;
	if (drm_fence_is_later(b, a))
		return 1;
	if (drm_fence_is_later(a, b))
		return -1;
	return 0;
}

/* Heap sort: in place, no recursion and no stack that grows with the
 * count. */
static void fence_sift_down(struct drm_fence **v, size_t root, size_t n)
{
	for (;;) {
		size_t child = 2 * root + 1;

		if (child >= n)
			return;
		if (child + 1 < n && fence_cmp(v[child], v[child + 1]) < 0)
			child++;
		if (fence_cmp(v[root], v[child]) >= 0)
			return;
		struct drm_fence *t = v[root];
		v[root] = v[child];
		v[child] = t;
		root = child;
	}
}

static void fence_sort(struct drm_fence **v, size_t n)
{
	if (n < 2)
		return;
	for (size_t i = n / 2; i-- > 0;)
		fence_sift_down(v, i, n);
	for (size_t end = n - 1; end > 0; end--) {
		struct drm_fence *t = v[0];
		v[0] = v[end];
		v[end] = t;
		fence_sift_down(v, 0, end);
	}
}

size_t drm_fence_dedup_array(struct drm_fence **fences, size_t num_fences)
{
	size_t i, j;

	if (!num_fences)
		return 0;

	fence_sort(fences, num_fences);

	/* Only the most recent fence of each timeline is kept: the sort put
	 * it first among its own. */
	j = 0;
	for (i = 1; i < num_fences; i++) {
		if (drm_fence_timeline(fences[i]) == drm_fence_timeline(fences[j]))
			drm_fence_put(fences[i]);
		else
			fences[++j] = fences[i];
	}

	return ++j;
}

struct drm_fence *__drm_fence_unwrap_merge(size_t num_fences,
					   struct drm_fence **fences,
					   struct drm_fence_unwrap *iter)
{
	struct drm_fence *tmp, *unsignaled = NULL, **array;
	struct drm_fence_array *result;
	struct drm_device *dev = NULL;
	uint64_t timestamp;
	size_t i, count;

	for (i = 0; i < num_fences && !dev; ++i)
		if (fences[i])
			dev = fences[i]->dev;
	if (WARN_ON(!dev))
		return NULL;

	count = 0;
	timestamp = 0;
	for (i = 0; i < num_fences; ++i) {
		drm_fence_unwrap_for_each(tmp, &iter[i], fences[i]) {
			if (!drm_fence_is_signaled(tmp)) {
				drm_fence_put(unsignaled);
				unsignaled = drm_fence_ref(tmp);
				++count;
			} else {
				uint64_t t = drm_fence_timestamp(tmp);

				if (t > timestamp)
					timestamp = t;
			}
		}
	}

	/* Nothing pending: a private signalled fence, with the time the
	 * last part signalled.  One pending: that one -- by far the most
	 * common case, and no allocation. */
	if (count == 0)
		return drm_fence_signalled_at(dev, timestamp);
	else if (count == 1)
		return unsignaled;

	drm_fence_put(unsignaled);

	size_t room = count;

	array = kcalloc(room, sizeof(*array));
	if (!array)
		return NULL;

	/* The second pass may find fewer pending (they signal meanwhile),
	 * never more than were counted: a fence does not become unsignalled
	 * and no parts appear.  Bounded all the same. */
	count = 0;
	for (i = 0; i < num_fences; ++i) {
		drm_fence_unwrap_for_each(tmp, &iter[i], fences[i]) {
			if (!drm_fence_is_signaled(tmp)) {
				if (!WARN_ON_ONCE(count >= room))
					array[count++] = drm_fence_ref(tmp);
			} else {
				uint64_t t = drm_fence_timestamp(tmp);

				if (t > timestamp)
					timestamp = t;
			}
		}
	}

	if (count == 0 || count == 1)
		goto return_fastpath;

	count = drm_fence_dedup_array(array, count);

	if (count > 1) {
		result = drm_fence_array_create(count, array,
						drm_fence_context_alloc(1), 1,
						false);
		if (!result) {
			for (i = 0; i < count; i++)
				drm_fence_put(array[i]);
			tmp = NULL;
			goto return_tmp;
		}
		return &result->base;
	}

return_fastpath:
	if (count == 0)
		tmp = drm_fence_signalled_at(dev, timestamp);
	else
		tmp = array[0];

return_tmp:
	kfree(array);
	return tmp;
}

struct drm_fence *drm_fence_unwrap_merge(size_t num_fences,
					 struct drm_fence **fences)
{
	struct drm_fence_unwrap small[4], *cursors = small;
	struct drm_fence *f;

	if (num_fences > ARRAY_SIZE(small)) {
		cursors = kcalloc(num_fences, sizeof(*cursors));
		if (!cursors)
			return NULL;
	}
	f = __drm_fence_unwrap_merge(num_fences, fences, cursors);
	if (cursors != small)
		kfree(cursors);
	return f;
}
