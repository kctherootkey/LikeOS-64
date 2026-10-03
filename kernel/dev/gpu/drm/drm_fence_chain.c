// LikeOS -- display-manager core: fence chains.
//
// A timeline built of fences: each link carries a sequence number and the
// link before it, so "the fence for point N" is found by walking the
// chain, and links whose fences have signalled are dropped as it goes.
//
// A link's `prev' pointer is replaced while others may be reading it (a
// walk cuts signalled links out from under concurrent walkers).  Readers
// take a reference to what it points at, and writers swap it, under one
// small lock; references are dropped only after the lock is let go, since
// dropping one may free a link and that frees further links.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Advanced Micro
// Devices' code: GPL-2.0-only
// Portions Copyright (C) 2018 Advanced Micro Devices, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/dev/gpu/gpu_util.h>

/* Guards every link's `prev' pointer: held only to read-and-reference or
 * to swap it, never across anything else (interrupt-safe: walks happen
 * in fence callbacks). */
static spinlock_t g_chain_lock = SPINLOCK_INIT("drm_fence_chain");

static bool drm_fence_chain_enable_signaling(struct drm_fence *fence);

/* The link before `chain', referenced (NULL at the start of the chain). */
static struct drm_fence *drm_fence_chain_get_prev(struct drm_fence_chain *chain)
{
	struct drm_fence *prev;
	uint64_t fl;

	spin_lock_irqsave(&g_chain_lock, &fl);
	prev = chain->prev;
	if (prev)
		drm_fence_get(prev);
	spin_unlock_irqrestore(&g_chain_lock, fl);
	return prev;
}

/* Swap chain->prev from `old' to `new' if it still is `old'; returns
 * what it was. */
static struct drm_fence *drm_fence_chain_cmpxchg_prev(struct drm_fence_chain *chain,
						      struct drm_fence *old,
						      struct drm_fence *new)
{
	struct drm_fence *cur;
	uint64_t fl;

	spin_lock_irqsave(&g_chain_lock, &fl);
	cur = chain->prev;
	if (cur == old)
		chain->prev = new;
	spin_unlock_irqrestore(&g_chain_lock, fl);
	return cur;
}

struct drm_fence *drm_fence_chain_walk(struct drm_fence *fence)
{
	struct drm_fence_chain *chain, *prev_chain;
	struct drm_fence *prev, *replacement, *tmp;

	chain = to_drm_fence_chain(fence);
	if (!chain) {
		drm_fence_put(fence);
		return NULL;
	}

	while ((prev = drm_fence_chain_get_prev(chain))) {

		prev_chain = to_drm_fence_chain(prev);
		if (prev_chain) {
			if (!drm_fence_is_signaled(prev_chain->fence))
				break;

			replacement = drm_fence_chain_get_prev(prev_chain);
		} else {
			if (!drm_fence_is_signaled(prev))
				break;

			replacement = NULL;
		}

		/* Cut the signalled link out.  Whoever wins the swap hands
		 * the chain's reference to `prev' over to us to drop; the
		 * loser drops the replacement it took. */
		tmp = drm_fence_chain_cmpxchg_prev(chain, prev, replacement);
		if (tmp == prev)
			drm_fence_put(tmp);
		else
			drm_fence_put(replacement);
		drm_fence_put(prev);
	}

	drm_fence_put(fence);
	return prev;
}

int drm_fence_chain_find_seqno(struct drm_fence **pfence, uint64_t seqno)
{
	struct drm_fence_chain *chain;

	if (!seqno)
		return 0;

	chain = to_drm_fence_chain(*pfence);
	if (!chain || chain->base.seqno64 < seqno)
		return -EINVAL;

	drm_fence_chain_for_each(*pfence, &chain->base) {
		if ((*pfence)->context != chain->base.context ||
		    to_drm_fence_chain(*pfence)->prev_seqno < seqno)
			break;
	}
	drm_fence_put(&chain->base);

	return 0;
}

static const char *drm_fence_chain_get_driver_name(struct drm_fence *fence)
{
	(void)fence;
	return "drm_fence_chain";
}

static const char *drm_fence_chain_get_timeline_name(struct drm_fence *fence)
{
	(void)fence;
	return "unbound";
}

/* The fence the link was watching signalled.  Watch the next one still
 * pending, or, when there is none, signal the link.  Runs with no fence
 * lock held (see drm_fence_func_t), so it re-arms right here; the core
 * has already taken `cb' off the signalled fence's list. */
static void drm_fence_chain_cb(struct drm_fence *f, struct drm_fence_cb *cb)
{
	struct drm_fence_chain *chain =
		(struct drm_fence_chain *)((char *)cb -
			__builtin_offsetof(struct drm_fence_chain, cb));

	/* drop what arming took on `f' */
	drm_fence_put(f);
	if (!drm_fence_chain_enable_signaling(&chain->base))
		/* nothing left pending */
		drm_fence_signal(&chain->base);
	/* and what the armed callback held on the link */
	drm_fence_put(&chain->base);
}

static bool drm_fence_chain_enable_signaling(struct drm_fence *fence)
{
	struct drm_fence_chain *head = to_drm_fence_chain(fence);

	drm_fence_get(&head->base);
	drm_fence_chain_for_each(fence, &head->base) {
		struct drm_fence *f = drm_fence_chain_contained(fence);

		drm_fence_get(f);
		if (!drm_fence_add_callback(f, &head->cb, drm_fence_chain_cb)) {
			drm_fence_put(fence);
			return true;
		}
		drm_fence_put(f);
	}
	drm_fence_put(&head->base);
	return false;
}

static bool drm_fence_chain_signaled(struct drm_fence *fence)
{
	drm_fence_chain_for_each(fence, fence) {
		struct drm_fence *f = drm_fence_chain_contained(fence);

		if (!drm_fence_is_signaled(f)) {
			drm_fence_put(fence);
			return false;
		}
	}

	return true;
}

static void drm_fence_chain_release(struct drm_fence *fence)
{
	struct drm_fence_chain *chain = to_drm_fence_chain(fence);
	struct drm_fence *prev;

	/* Unlink as much of the chain as only this link keeps alive, here
	 * in a loop: letting each link's release free the next would recurse
	 * once per link, and a timeline can be thousands of links long. */
	while ((prev = chain->prev)) {
		struct drm_fence_chain *prev_chain;

		if (__atomic_load_n(&prev->refs, __ATOMIC_ACQUIRE) > 1)
			break;

		prev_chain = to_drm_fence_chain(prev);
		if (!prev_chain)
			break;

		/* We hold the last reference to prev_chain: nobody else
		 * can be looking at its prev pointer. */
		chain->prev = prev_chain->prev;
		prev_chain->prev = NULL;
		drm_fence_put(prev);
	}
	drm_fence_put(prev);

	drm_fence_put(chain->fence);
	kfree(chain);
}

static void drm_fence_chain_set_deadline(struct drm_fence *fence,
					 uint64_t deadline)
{
	drm_fence_chain_for_each(fence, fence) {
		struct drm_fence *f = drm_fence_chain_contained(fence);

		drm_fence_set_deadline(f, deadline);
	}
}

const struct drm_fence_ops drm_fence_chain_ops = {
	.get_driver_name = drm_fence_chain_get_driver_name,
	.get_timeline_name = drm_fence_chain_get_timeline_name,
	.enable_signaling = drm_fence_chain_enable_signaling,
	.signaled = drm_fence_chain_signaled,
	.release = drm_fence_chain_release,
	.set_deadline = drm_fence_chain_set_deadline,
};

struct drm_fence_chain *drm_fence_chain_alloc(void)
{
	return kcalloc(1, sizeof(struct drm_fence_chain));
}

void drm_fence_chain_free(struct drm_fence_chain *chain)
{
	kfree(chain);
}

void drm_fence_chain_init(struct drm_fence_chain *chain, struct drm_fence *prev,
			  struct drm_fence *fence, uint64_t seqno)
{
	struct drm_fence_chain *prev_chain = to_drm_fence_chain(prev);
	uint64_t context;

	chain->prev = prev;
	chain->fence = fence;
	chain->prev_seqno = 0;

	/* Try to reuse the context of the previous chain node. */
	if (prev_chain && seqno > prev->seqno64) {
		context = prev->context;
		chain->prev_seqno = prev->seqno64;
	} else {
		context = drm_fence_context_alloc(1);
		/* Make sure that we always have a valid sequence number. */
		if (prev_chain && prev->seqno64 > seqno)
			seqno = prev->seqno64;
	}

	/* The link borrows the lock of its fence's device.  A link always
	 * holds a fence. */
	WARN_ON(!fence);
	drm_fence_init_unlisted(&chain->base, fence ? fence->dev :
				(prev ? prev->dev : NULL),
				&drm_fence_chain_ops, context, seqno);

	/* Links are joined only through `prev', never by holding one as
	 * the fence: flatten into an array instead. */
	WARN_ON(drm_fence_is_chain(fence));
}
