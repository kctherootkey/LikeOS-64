// LikeOS -- display-manager core: fence arrays.
//
// A fence that stands for several others: signalled when all of them are
// (or, on request, when any one is), with the first error among them.
//
// Nothing is watched until someone asks (drm_fence_enable_signaling(): a
// callback added, a wait, a poll): then a callback goes on every part,
// each holding a reference to the array, and the part whose callback
// brings the pending count to zero signals the array.  Asked without
// watching (drm_fence_is_signaled()), the array counts its signalled parts
// itself.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Collabora's and
// Advanced Micro Devices' code: GPL-2.0-only
// Portions Copyright (C) 2016 Collabora Ltd
// Portions Copyright (C) 2016 Advanced Micro Devices, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_fence_array.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/dev/gpu/gpu_util.h>

/* base.error while the array has not signalled: "no error yet".  The
 * first part that fails replaces it; signalling clears what is left. */
#define PENDING_ERROR 1

static const char *drm_fence_array_get_driver_name(struct drm_fence *fence)
{
	(void)fence;
	return "drm_fence_array";
}

static const char *drm_fence_array_get_timeline_name(struct drm_fence *fence)
{
	(void)fence;
	return "unbound";
}

static void drm_fence_array_set_pending_error(struct drm_fence_array *array,
					      int error)
{
	int expected = PENDING_ERROR;

	/* The first error reported by any part, and only before the array
	 * itself has signalled. */
	if (error)
		__atomic_compare_exchange_n(&array->base.error, &expected, error,
					    false, __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE);
}

static void drm_fence_array_clear_pending_error(struct drm_fence_array *array)
{
	int expected = PENDING_ERROR;

	/* No part failed: no error. */
	__atomic_compare_exchange_n(&array->base.error, &expected, 0, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* A part signalled.  Runs with no fence lock held (see drm_fence_func_t),
 * so the array is signalled right here. */
static void drm_fence_array_cb_func(struct drm_fence *f, struct drm_fence_cb *cb)
{
	struct drm_fence_array_cb *array_cb = (struct drm_fence_array_cb *)cb;
	struct drm_fence_array *array = array_cb->array;

	drm_fence_array_set_pending_error(array, f->error);

	if (__atomic_sub_fetch(&array->num_pending, 1, __ATOMIC_ACQ_REL) == 0) {
		drm_fence_array_clear_pending_error(array);
		drm_fence_signal(&array->base);
	}
	/* this callback's hold on the array */
	drm_fence_put(&array->base);
}

static bool drm_fence_array_enable_signaling(struct drm_fence *fence)
{
	struct drm_fence_array *array = to_drm_fence_array(fence);
	struct drm_fence_array_cb *cb = array->callbacks;
	unsigned i;

	for (i = 0; i < array->num_fences; ++i) {
		cb[i].array = array;
		/* The array may be reported signalled before all of its
		 * callbacks have run (signal-on-any, or the last one racing
		 * with the core), so each callback holds a reference of its
		 * own: the array is not freed under one still to come. */
		drm_fence_get(&array->base);
		if (drm_fence_add_callback(array->fences[i], &cb[i].cb,
					   drm_fence_array_cb_func)) {
			int error = array->fences[i]->error;

			drm_fence_array_set_pending_error(array, error);
			drm_fence_put(&array->base);
			if (__atomic_sub_fetch(&array->num_pending, 1,
					       __ATOMIC_ACQ_REL) == 0) {
				drm_fence_array_clear_pending_error(array);
				return false;
			}
		}
	}

	return true;
}

static bool drm_fence_array_signaled(struct drm_fence *fence)
{
	struct drm_fence_array *array = to_drm_fence_array(fence);
	int num_pending;
	unsigned int i;

	/* The pending count is read before the watching bit, so as not to
	 * race enable_signaling() decrementing it into a half-done count.
	 * Not watching: count the signalled parts here (the `!--' is what
	 * makes signal-on-any work, its count being one). */
	num_pending = __atomic_load_n(&array->num_pending, __ATOMIC_ACQUIRE);
	if (__atomic_load_n(&array->base.sflags, __ATOMIC_ACQUIRE) &
	    DRM_FENCE_SF_ENABLE_SIGNAL) {
		if (num_pending <= 0)
			goto signal;
		return false;
	}

	for (i = 0; i < array->num_fences; ++i) {
		if (drm_fence_is_signaled(array->fences[i]) && !--num_pending)
			goto signal;
	}
	return false;

signal:
	drm_fence_array_clear_pending_error(array);
	return true;
}

static void drm_fence_array_release(struct drm_fence *fence)
{
	struct drm_fence_array *array = to_drm_fence_array(fence);
	unsigned i;

	for (i = 0; i < array->num_fences; ++i)
		drm_fence_put(array->fences[i]);

	kfree(array->fences);
	kfree(array);
}

static void drm_fence_array_set_deadline(struct drm_fence *fence,
					 uint64_t deadline)
{
	struct drm_fence_array *array = to_drm_fence_array(fence);
	unsigned i;

	for (i = 0; i < array->num_fences; ++i)
		drm_fence_set_deadline(array->fences[i], deadline);
}

const struct drm_fence_ops drm_fence_array_ops = {
	.get_driver_name = drm_fence_array_get_driver_name,
	.get_timeline_name = drm_fence_array_get_timeline_name,
	.enable_signaling = drm_fence_array_enable_signaling,
	.signaled = drm_fence_array_signaled,
	.release = drm_fence_array_release,
	.set_deadline = drm_fence_array_set_deadline,
};

struct drm_fence_array *drm_fence_array_alloc(int num_fences)
{
	if (num_fences <= 0)
		return NULL;
	return kcalloc(1, sizeof(struct drm_fence_array) +
			  (size_t)num_fences * sizeof(struct drm_fence_array_cb));
}

void drm_fence_array_init(struct drm_fence_array *array, int num_fences,
			  struct drm_fence **fences, uint64_t context,
			  unsigned seqno, bool signal_on_any)
{
	/* The array borrows the lock of the device of its first part. */
	if (WARN_ON(num_fences <= 0 || !fences || !fences[0]))
		return;

	array->num_fences = (unsigned)num_fences;

	drm_fence_init_unlisted(&array->base, fences[0]->dev,
				&drm_fence_array_ops, context, seqno);

	array->num_pending = signal_on_any ? 1 : num_fences;
	array->fences = fences;

	array->base.error = PENDING_ERROR;

	/* An array never holds another container: operations on it would
	 * recurse, without bound, on the kernel stack.  The caller flattens
	 * instead (drm_fence_unwrap_merge()). */
	while (num_fences--)
		WARN_ON(drm_fence_is_container(fences[num_fences]));
}

struct drm_fence_array *drm_fence_array_create(int num_fences,
					       struct drm_fence **fences,
					       uint64_t context, unsigned seqno,
					       bool signal_on_any)
{
	struct drm_fence_array *array;

	if (WARN_ON(num_fences <= 0 || !fences || !fences[0]))
		return NULL;

	array = drm_fence_array_alloc(num_fences);
	if (!array)
		return NULL;

	drm_fence_array_init(array, num_fences, fences, context, seqno,
			     signal_on_any);

	return array;
}

bool drm_fence_match_context(struct drm_fence *fence, uint64_t context)
{
	struct drm_fence_array *array = to_drm_fence_array(fence);
	unsigned i;

	if (!array)
		return fence->context == context;

	for (i = 0; i < array->num_fences; i++) {
		if (array->fences[i]->context != context)
			return false;
	}

	return true;
}

struct drm_fence *drm_fence_array_first(struct drm_fence *head)
{
	struct drm_fence_array *array;

	if (!head)
		return NULL;

	array = to_drm_fence_array(head);
	if (!array)
		return head;

	if (!array->num_fences)
		return NULL;

	return array->fences[0];
}

struct drm_fence *drm_fence_array_next(struct drm_fence *head, unsigned index)
{
	struct drm_fence_array *array = to_drm_fence_array(head);

	if (!array || index >= array->num_fences)
		return NULL;

	return array->fences[index];
}

bool drm_fence_is_container(struct drm_fence *f)
{
	return drm_fence_is_array(f) || drm_fence_is_chain(f);
}
