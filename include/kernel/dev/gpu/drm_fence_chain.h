// LikeOS -- display-manager core: fence chains.
//
// A timeline built of fences: each link holds one fence, the point
// (sequence number) it stands for and the link before it.  A link
// signals once its own fence and every earlier one have.  "The fence for
// point N" is found by walking back from the newest link, and links
// whose fences have all signalled are cut out of the chain as it is
// walked, so a long-lived timeline does not keep its whole history.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Advanced Micro
// Devices' code: GPL-2.0-only
// Portions Copyright (C) 2018 Advanced Micro Devices, Inc.

#ifndef KERNEL_DEV_GPU_DRM_FENCE_CHAIN_H
#define KERNEL_DEV_GPU_DRM_FENCE_CHAIN_H

#include <kernel/dev/gpu/drm.h>

struct drm_fence_chain {
	struct drm_fence base; /* first: the link IS a fence */
	/* The link (or plain fence) before this one, referenced; changed
	 * only under the chain lock in drm_fence_chain.c. */
	struct drm_fence *prev;
	uint64_t prev_seqno; /* prev's point, when on the same timeline */
	struct drm_fence *fence; /* this link's own fence, referenced */
	/* Armed on the first fence of the chain that has not signalled;
	 * re-armed from its own callback until none is left. */
	struct drm_fence_cb cb;
};

extern const struct drm_fence_ops drm_fence_chain_ops;

static inline bool drm_fence_is_chain(const struct drm_fence *f)
{
	return f && f->ops == &drm_fence_chain_ops;
}

static inline struct drm_fence_chain *to_drm_fence_chain(struct drm_fence *f)
{
	if (!drm_fence_is_chain(f))
		return NULL;
	return (struct drm_fence_chain *)((char *)f -
		__builtin_offsetof(struct drm_fence_chain, base));
}

/* The fence a link holds, or `f' itself when it is no link. */
static inline struct drm_fence *drm_fence_chain_contained(struct drm_fence *f)
{
	struct drm_fence_chain *chain = to_drm_fence_chain(f);

	return chain ? chain->fence : f;
}

/* Allocate a link (uninitialised); free one that was never initialised
 * with drm_fence_chain_free(), an initialised one only by dropping it. */
struct drm_fence_chain *drm_fence_chain_alloc(void);
void drm_fence_chain_free(struct drm_fence_chain *chain);

/* Step from `fence' (a reference the call consumes) to the link before
 * it, referenced, dropping links whose fences have signalled on the way;
 * NULL at the end or when `fence' is no link. */
struct drm_fence *drm_fence_chain_walk(struct drm_fence *fence);
/* Replace *pfence (a link, referenced) by the link that stands for point
 * `seqno' -- the oldest link still covering it -- or NULL when that point
 * has signalled.  0, or -EINVAL when `seqno' is beyond the chain. */
int drm_fence_chain_find_seqno(struct drm_fence **pfence, uint64_t seqno);
/* Make `chain' the link for point `seqno', holding `fence' after `prev'
 * (either may be a plain fence; prev may be NULL).  Takes over both
 * references.  `fence' must not itself be a link. */
void drm_fence_chain_init(struct drm_fence_chain *chain, struct drm_fence *prev,
			  struct drm_fence *fence, uint64_t seqno);

/* Every link from `head' back, each referenced while it is `iter'.
 * Leaving the loop early leaves `iter' referenced: drop it. */
#define drm_fence_chain_for_each(iter, head)			\
	for (iter = drm_fence_ref(head); iter;			\
	     iter = drm_fence_chain_walk(iter))

#endif
