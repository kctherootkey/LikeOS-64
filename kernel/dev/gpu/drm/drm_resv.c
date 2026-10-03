// LikeOS -- display-manager core: reservation objects.
//
// The fences attached to a buffer object, each with what it is for
// (kernel work, a writer, a reader, bookkeeping), so that implicit
// synchronisation can wait for exactly the right ones.
//
// The set is a vector of fence pointers with the usage in the two low bits
// of each entry.  Writers hold the object's (sleeping) lock and only ever
// store into slots reserved beforehand, so adding never allocates; the
// vector is reallocated by drm_resv_reserve_fences() alone, which also
// leaves the fences that have signalled behind.  Readers without the lock
// take a reference to an entry under the object's spinlock and check the
// generation number to notice a reallocation.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Canonical's and
// VMware's code: MIT
// Portions Copyright (C) 2012-2014 Canonical Ltd (Maarten Lankhorst)
// Portions Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_resv.h>
#include <kernel/dev/gpu/drm_fence_array.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/dev/gpu/gpu_util.h>

/* Mask for the lower fence pointer bits */
#define DRM_RESV_LIST_MASK 0x3UL

struct drm_resv_list {
	uint32_t num_fences, max_fences;
	uintptr_t table[];
};

/* Extract the fence and usage flags from an entry in the list. */
static void drm_resv_list_entry(struct drm_resv_list *list, unsigned int index,
				struct drm_fence **fence,
				enum drm_resv_usage *usage)
{
	uintptr_t tmp = __atomic_load_n(&list->table[index], __ATOMIC_ACQUIRE);

	*fence = (struct drm_fence *)(tmp & ~DRM_RESV_LIST_MASK);
	if (usage)
		*usage = (enum drm_resv_usage)(tmp & DRM_RESV_LIST_MASK);
}

/* Set the fence and usage flags at the specific index in the list. */
static void drm_resv_list_set(struct drm_resv_list *list, unsigned int index,
			      struct drm_fence *fence, enum drm_resv_usage usage)
{
	uintptr_t tmp = (uintptr_t)fence | (uintptr_t)usage;

	__atomic_store_n(&list->table[index], tmp, __ATOMIC_RELEASE);
}

static unsigned int roundup_pow_of_two_u32(unsigned int n)
{
	unsigned int p = 1;

	while (p < n && p < 0x80000000u)
		p <<= 1;
	return p;
}

/* Allocate a new list with room for at least `max_fences'. */
static struct drm_resv_list *drm_resv_list_alloc(unsigned int max_fences)
{
	struct drm_resv_list *list;

	list = kalloc(sizeof(*list) + (size_t)max_fences * sizeof(list->table[0]));
	if (!list)
		return NULL;

	list->num_fences = 0;
	list->max_fences = max_fences;
	return list;
}

/* Free a list and drop every reference in it. */
static void drm_resv_list_free(struct drm_resv_list *list)
{
	unsigned int i;

	if (!list)
		return;

	for (i = 0; i < list->num_fences; ++i) {
		struct drm_fence *fence;

		drm_resv_list_entry(list, i, &fence, NULL);
		drm_fence_put(fence);
	}
	kfree(list);
}

void drm_resv_init(struct drm_resv *obj)
{
	mm_rwsem_init(&obj->lock, "drm_resv");
	spinlock_init(&obj->slock, "drm_resv_list");
	obj->fences = NULL;
	obj->gen = 0;
}

void drm_resv_fini(struct drm_resv *obj)
{
	/* The object is dead: nobody else looks at it any more. */
	drm_resv_list_free(obj->fences);
	obj->fences = NULL;
}

/* Swap in a new list for the readers, under the spinlock; returns the
 * old one. */
static struct drm_resv_list *drm_resv_publish(struct drm_resv *obj,
					      struct drm_resv_list *list)
{
	struct drm_resv_list *old;
	uint64_t fl;

	spin_lock_irqsave(&obj->slock, &fl);
	old = obj->fences;
	obj->fences = list;
	obj->gen++;
	spin_unlock_irqrestore(&obj->slock, fl);
	return old;
}

/* Store an entry where the readers may be looking. */
static void drm_resv_store(struct drm_resv *obj, struct drm_resv_list *list,
			   unsigned int index, struct drm_fence *fence,
			   enum drm_resv_usage usage)
{
	uint64_t fl;

	spin_lock_irqsave(&obj->slock, &fl);
	drm_resv_list_set(list, index, fence, usage);
	spin_unlock_irqrestore(&obj->slock, fl);
}

int drm_resv_reserve_fences(struct drm_resv *obj, unsigned int num_fences)
{
	struct drm_resv_list *old, *new;
	unsigned int i, j, k, max;

	WARN_ON(!drm_resv_held(obj));

	/* Asking for no room at all is a miscount in the caller. */
	if (WARN_ON(!num_fences))
		return -EINVAL;

	old = obj->fences;
	if (old && old->max_fences) {
		if ((old->num_fences + num_fences) <= old->max_fences)
			return 0;
		max = max(old->num_fences + num_fences, old->max_fences * 2);
	} else {
		max = max(4u, roundup_pow_of_two_u32(num_fences));
	}

	new = drm_resv_list_alloc(max);
	if (!new)
		return -ENOMEM;

	/* Each entry brings its hold on its fence along.  Signalled fences go
	 * to the end of the new vector, outside its count, to be dropped
	 * once the new list is in place. */
	for (i = 0, j = 0, k = max; i < (old ? old->num_fences : 0); ++i) {
		enum drm_resv_usage usage;
		struct drm_fence *fence;

		drm_resv_list_entry(old, i, &fence, &usage);
		if (drm_fence_is_signaled(fence))
			new->table[--k] = (uintptr_t)fence;
		else
			drm_resv_list_set(new, j++, fence, usage);
	}
	new->num_fences = j;

	/* The set of unsignalled fences is the same in both: readers on
	 * either see the same thing. */
	old = drm_resv_publish(obj, new);

	if (!old)
		return 0;

	/* Let go of the signalled fences */
	for (i = k; i < max; ++i)
		drm_fence_put((struct drm_fence *)new->table[i]);
	kfree(old);

	return 0;
}

void drm_resv_add_fence(struct drm_resv *obj, struct drm_fence *fence,
			enum drm_resv_usage usage)
{
	struct drm_resv_list *fobj;
	struct drm_fence *old;
	unsigned int i, count;

	WARN_ON(!drm_resv_held(obj));

	/* Containers are added part by part, never whole. */
	WARN_ON(drm_fence_is_container(fence));
	if (WARN_ON((uintptr_t)fence & DRM_RESV_LIST_MASK))
		return;

	fobj = obj->fences;
	if (WARN_ON(!fobj))
		return;
	drm_fence_get(fence);
	count = fobj->num_fences;

	for (i = 0; i < count; ++i) {
		enum drm_resv_usage old_usage;

		drm_resv_list_entry(fobj, i, &old, &old_usage);
		if ((drm_fence_timeline(old) == drm_fence_timeline(fence) &&
		     old_usage >= usage &&
		     drm_fence_is_later_or_same(fence, old)) ||
		    drm_fence_is_signaled(old)) {
			drm_resv_store(obj, fobj, i, fence, usage);
			drm_fence_put(old);
			return;
		}
	}

	/* No slot was reserved: the caller's bug, and storing past the end
	 * would corrupt memory.  Keep the set as it was. */
	if (WARN_ON(fobj->num_fences >= fobj->max_fences)) {
		drm_fence_put(fence);
		return;
	}
	count++;

	/* the entry is in place before the count says it exists */
	{
		uint64_t fl;

		spin_lock_irqsave(&obj->slock, &fl);
		drm_resv_list_set(fobj, i, fence, usage);
		fobj->num_fences = count;
		spin_unlock_irqrestore(&obj->slock, fl);
	}
}

void drm_resv_replace_fences(struct drm_resv *obj, uint64_t context,
			     struct drm_fence *replacement,
			     enum drm_resv_usage usage)
{
	struct drm_resv_list *list;
	unsigned int i;

	WARN_ON(!drm_resv_held(obj));

	list = obj->fences;
	for (i = 0; list && i < list->num_fences; ++i) {
		struct drm_fence *old;

		drm_resv_list_entry(list, i, &old, NULL);
		if (old->context != context)
			continue;

		drm_resv_store(obj, list, i, drm_fence_ref(replacement), usage);
		drm_fence_put(old);
	}
}

/* Restart the unlocked iteration on the current list.  Spinlock held. */
static void drm_resv_iter_restart_unlocked(struct drm_resv_iter *cursor)
{
	cursor->index = 0;
	cursor->num_fences = 0;
	cursor->fences = cursor->obj->fences;
	cursor->gen = cursor->obj->gen;
	if (cursor->fences)
		cursor->num_fences = cursor->fences->num_fences;
	cursor->is_restarted = true;
}

/* Walk to the next not signalled fence and grab a reference to it.  The
 * spinlock is held only to take a hold on the fence; whether it has
 * signalled is asked without it, since asking may signal it and run its
 * callbacks. */
static void drm_resv_iter_walk_unlocked(struct drm_resv_iter *cursor, bool restart)
{
	for (;;) {
		struct drm_fence *fence;
		enum drm_resv_usage usage;
		uint64_t fl;

		/* Let go of the previous round's fence */
		drm_fence_put(cursor->fence);
		cursor->fence = NULL;

		spin_lock_irqsave(&cursor->obj->slock, &fl);
		if (restart || cursor->gen != cursor->obj->gen ||
		    cursor->fences != cursor->obj->fences)
			drm_resv_iter_restart_unlocked(cursor);
		restart = false;
		if (!cursor->fences || cursor->index >= cursor->num_fences) {
			spin_unlock_irqrestore(&cursor->obj->slock, fl);
			return;
		}
		drm_resv_list_entry(cursor->fences, cursor->index++, &fence, &usage);
		drm_fence_get(fence);
		spin_unlock_irqrestore(&cursor->obj->slock, fl);

		cursor->fence = fence;
		cursor->fence_usage = usage;
		if (!drm_fence_is_signaled(fence) && cursor->usage >= usage)
			return;
	}
}

struct drm_fence *drm_resv_iter_first_unlocked(struct drm_resv_iter *cursor)
{
	cursor->is_restarted = false;
	drm_resv_iter_walk_unlocked(cursor, true);
	return cursor->fence;
}

struct drm_fence *drm_resv_iter_next_unlocked(struct drm_resv_iter *cursor)
{
	cursor->is_restarted = false;
	drm_resv_iter_walk_unlocked(cursor, false);
	return cursor->fence;
}

struct drm_fence *drm_resv_iter_first(struct drm_resv_iter *cursor)
{
	struct drm_fence *fence;

	WARN_ON(!drm_resv_held(cursor->obj));

	cursor->index = 0;
	cursor->fences = cursor->obj->fences;

	fence = drm_resv_iter_next(cursor);
	cursor->is_restarted = true;
	return fence;
}

struct drm_fence *drm_resv_iter_next(struct drm_resv_iter *cursor)
{
	struct drm_fence *fence;

	cursor->is_restarted = false;

	do {
		if (!cursor->fences ||
		    cursor->index >= cursor->fences->num_fences)
			return NULL;

		drm_resv_list_entry(cursor->fences, cursor->index++,
				    &fence, &cursor->fence_usage);
	} while (cursor->fence_usage > cursor->usage);

	return fence;
}

int drm_resv_copy_fences(struct drm_resv *dst, struct drm_resv *src)
{
	struct drm_resv_iter cursor;
	struct drm_resv_list *list;
	struct drm_fence *f;

	WARN_ON(!drm_resv_held(dst));

	list = NULL;

	drm_resv_iter_begin(&cursor, src, DRM_RESV_USAGE_BOOKKEEP);
	drm_resv_for_each_fence_unlocked(&cursor, f) {

		if (drm_resv_iter_is_restarted(&cursor)) {
			drm_resv_list_free(list);

			list = drm_resv_list_alloc(cursor.num_fences ?
						   cursor.num_fences : 1);
			if (!list) {
				drm_resv_iter_end(&cursor);
				return -ENOMEM;
			}
			list->num_fences = 0;
		}

		if (WARN_ON_ONCE(list->num_fences >= list->max_fences))
			continue;
		drm_fence_get(f);
		drm_resv_list_set(list, list->num_fences++, f,
				  drm_resv_iter_usage(&cursor));
	}
	drm_resv_iter_end(&cursor);

	list = drm_resv_publish(dst, list);
	drm_resv_list_free(list);
	return 0;
}

int drm_resv_get_fences(struct drm_resv *obj, enum drm_resv_usage usage,
			unsigned int *num_fences, struct drm_fence ***fences)
{
	struct drm_resv_iter cursor;
	struct drm_fence *fence;
	unsigned int room = 0;

	*num_fences = 0;
	*fences = NULL;

	drm_resv_iter_begin(&cursor, obj, usage);
	drm_resv_for_each_fence_unlocked(&cursor, fence) {

		if (drm_resv_iter_is_restarted(&cursor)) {
			struct drm_fence **new_fences;
			unsigned int count;

			while (*num_fences)
				drm_fence_put((*fences)[--(*num_fences)]);

			count = cursor.num_fences + 1;

			/* Eventually re-allocate the array */
			new_fences = krealloc(*fences, count * sizeof(void *));
			if (!new_fences) {
				kfree(*fences);
				*fences = NULL;
				*num_fences = 0;
				drm_resv_iter_end(&cursor);
				return -ENOMEM;
			}
			*fences = new_fences;
			room = count;
		}

		if (WARN_ON_ONCE(*num_fences >= room))
			continue;
		(*fences)[(*num_fences)++] = drm_fence_ref(fence);
	}
	drm_resv_iter_end(&cursor);

	return 0;
}

int drm_resv_get_singleton(struct drm_resv *obj, enum drm_resv_usage usage,
			   struct drm_fence **fence)
{
	struct drm_fence_array *array;
	struct drm_fence **fences;
	unsigned count;
	int r;

	r = drm_resv_get_fences(obj, usage, &count, &fences);
	if (r)
		return r;

	if (count == 0) {
		*fence = NULL;
		kfree(fences);
		return 0;
	}

	if (count == 1) {
		*fence = fences[0];
		kfree(fences);
		return 0;
	}

	array = drm_fence_array_create((int)count, fences,
				       drm_fence_context_alloc(1), 1, false);
	if (!array) {
		while (count--)
			drm_fence_put(fences[count]);
		kfree(fences);
		return -ENOMEM;
	}

	*fence = &array->base;
	return 0;
}

int drm_resv_wait_timeout(struct drm_resv *obj, enum drm_resv_usage usage,
			  int intr, uint64_t timeout_ns)
{
	uint64_t deadline = hrtimer_now_ns() + timeout_ns;
	struct drm_resv_iter cursor;
	struct drm_fence *fence;
	int ret = 0;

	drm_resv_iter_begin(&cursor, obj, usage);
	drm_resv_for_each_fence_unlocked(&cursor, fence) {
		uint64_t now = hrtimer_now_ns();
		uint64_t left = timeout_ns && now < deadline ? deadline - now : 0;

		ret = drm_fence_wait_flags(fence, left, intr);
		if (ret)
			break;
	}
	drm_resv_iter_end(&cursor);

	return ret;
}

void drm_resv_set_deadline(struct drm_resv *obj, enum drm_resv_usage usage,
			   uint64_t deadline_ns)
{
	struct drm_resv_iter cursor;
	struct drm_fence *fence;

	drm_resv_iter_begin(&cursor, obj, usage);
	drm_resv_for_each_fence_unlocked(&cursor, fence) {
		drm_fence_set_deadline(fence, deadline_ns);
	}
	drm_resv_iter_end(&cursor);
}

bool drm_resv_test_signaled(struct drm_resv *obj, enum drm_resv_usage usage)
{
	struct drm_resv_iter cursor;
	struct drm_fence *fence;

	drm_resv_iter_begin(&cursor, obj, usage);
	drm_resv_for_each_fence_unlocked(&cursor, fence) {
		drm_resv_iter_end(&cursor);
		return false;
	}
	drm_resv_iter_end(&cursor);
	return true;
}
