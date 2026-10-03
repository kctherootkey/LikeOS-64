// LikeOS -- display-manager core: damage rectangles of a plane update.
//
// A client may say which parts of a plane's framebuffer changed since the
// last commit (the FB_DAMAGE_CLIPS property: a blob of struct
// drm_mode_rect in framebuffer coordinates).  A driver that uploads or
// flushes only what changed iterates over those rectangles, clipped to the
// plane's source; when the update cannot be partial (a different
// framebuffer, a moved source, a full mode set, no clips given) the
// iterator yields the whole source once instead.
//
// The iterator reads the plane state as the plane check left it
// (drm_atomic_helper_check_plane_state: visible, the clipped src) and the
// old state's clipped src; drm_atomic_helper_old_plane_state() makes the
// committed plane into such an old state.  The clips come from the plane
// state's damage_clips / num_damage_clips, which the atomic core fills
// from the FB_DAMAGE_CLIPS blob (a kernel caller may point them at its own
// array that lives as long as the request).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2018 VMware, Inc., Palo Alto, CA., USA
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (c) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_DAMAGE_HELPER_H
#define KERNEL_DEV_GPU_DRM_DAMAGE_HELPER_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_rect.h>

/* Every damage rectangle of the update in turn, clipped to the plane
 * source, in whole framebuffer pixels:
 *
 *	drm_atomic_helper_damage_iter_init(&iter, old_state, new_state);
 *	drm_atomic_for_each_plane_damage(&iter, &rect) { ... }
 */
#define drm_atomic_for_each_plane_damage(iter, rect) \
	while (drm_atomic_helper_damage_iter_next(iter, rect))

struct drm_atomic_helper_damage_iter {
	/* private: the plane source in whole numbers */
	struct drm_rect plane_src;
	/* private: the rectangles of the damage blob */
	const struct drm_rect *clips;
	/* private: how many */
	uint32_t num_clips;
	/* private: the one the iterator is at */
	uint32_t curr_clip;
	/* private: the whole source is one damage rectangle */
	bool full_update;
};

/* ---- drm_damage_helper.c ------------------------------------------------- */

/* Drop the plane's damage clips when its crtc does a full mode set in
 * request `state' (drm_crtc_state_needs_modeset): then everything is
 * redrawn, whatever the client said.  Call it from the atomic check, after
 * the core has computed mode_changed / active_changed /
 * connectors_changed. */
void drm_atomic_helper_check_plane_damage(struct drm_atomic_state *state,
					  struct drm_plane_state *plane_state);
/* Start iterating over the damage of new_state.  Nothing (the first
 * iter_next is false) for a plane that is off, has no framebuffer or is
 * not visible; the whole source once when there are no clips, when
 * ignore_damage_clips is set, when there is no old_state (NULL), or when
 * the clipped source differs from old_state's. */
void drm_atomic_helper_damage_iter_init(struct drm_atomic_helper_damage_iter *iter,
					const struct drm_plane_state *old_state,
					const struct drm_plane_state *new_state);
/* The next rectangle, in framebuffer pixels, clipped to the source rounded
 * out to whole pixels; clips wholly outside it are skipped.  False when
 * there is none left. */
bool drm_atomic_helper_damage_iter_next(struct drm_atomic_helper_damage_iter *iter,
					struct drm_rect *rect);
/* The bounding box of all damage of the update; false when there is
 * none. */
bool drm_atomic_helper_damage_merged(const struct drm_plane_state *old_state,
				     const struct drm_plane_state *state,
				     struct drm_rect *rect);
/* The damage clips the state carries (resolved from its FB_DAMAGE_CLIPS
 * blob by the atomic core): how many (0 when damage_clips is NULL), and
 * the array (NULL when none).  A non-NULL array with a count of 0 is an
 * update that damaged nothing. */
unsigned int drm_plane_get_damage_clips_count(const struct drm_plane_state *state);
struct drm_mode_rect *drm_plane_get_damage_clips(const struct drm_plane_state *state);

#endif
