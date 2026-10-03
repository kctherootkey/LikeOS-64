// LikeOS -- display-manager core: damage rectangles of a plane update.
//
// Iterating over the FB_DAMAGE_CLIPS rectangles of a plane update, clipped
// to the plane source, falling back to the whole source when the update
// cannot be partial; their bounding box; dropping them on a full mode
// set.
//
// The iterator works on the plane state after the plane check
// (drm_atomic_helper_check_plane_state): it reads the state's visible
// flag and its clipped src, and compares that src with the old state's
// (drm_atomic_helper_old_plane_state gives the committed plane as one).
// A plane state that never went through the check is not visible, and
// yields no damage at all.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2018 VMware, Inc., Palo Alto, CA., USA
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_damage_helper.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>

/* struct drm_mode_rect (the blob's element) is read as struct drm_rect. */
_Static_assert(sizeof(struct drm_mode_rect) == sizeof(struct drm_rect) &&
	       __builtin_offsetof(struct drm_mode_rect, x1) == __builtin_offsetof(struct drm_rect, x1) &&
	       __builtin_offsetof(struct drm_mode_rect, y1) == __builtin_offsetof(struct drm_rect, y1) &&
	       __builtin_offsetof(struct drm_mode_rect, x2) == __builtin_offsetof(struct drm_rect, x2) &&
	       __builtin_offsetof(struct drm_mode_rect, y2) == __builtin_offsetof(struct drm_rect, y2),
	       "damage clips must have the layout of struct drm_rect");

/*
 * Damage the client gave is meaningless once the crtc does a full mode
 * set: everything is redrawn.  Clearing the clips makes the iterator
 * yield the whole plane source.  If there are more reasons a driver must
 * redraw the whole plane, they belong here too (or the driver sets
 * ignore_damage_clips).
 *
 * The plane state holds no reference through fb_damage_clips / damage_clips
 * of its own (they point into a blob the request keeps alive), so there is
 * nothing to drop here.
 */
void drm_atomic_helper_check_plane_damage(struct drm_atomic_state *state,
					  struct drm_plane_state *plane_state)
{
	struct drm_crtc_state *crtc_state;

	if (plane_state->crtc >= 0) {
		if (WARN_ON(plane_state->crtc >= DRM_MAX_CRTCS))
			return;
		crtc_state = &state->crtcs[plane_state->crtc];

		if (WARN_ON(!crtc_state->crtc))
			return;

		if (drm_crtc_state_needs_modeset(crtc_state)) {
			plane_state->fb_damage_clips = 0;
			plane_state->damage_clips = NULL;
			plane_state->num_damage_clips = 0;
		}
	}
}

unsigned int drm_plane_get_damage_clips_count(const struct drm_plane_state *state)
{
	return (state && state->damage_clips) ? state->num_damage_clips : 0;
}

struct drm_mode_rect *drm_plane_get_damage_clips(const struct drm_plane_state *state)
{
	return state ? state->damage_clips : NULL;
}

void drm_atomic_helper_damage_iter_init(struct drm_atomic_helper_damage_iter *iter,
					const struct drm_plane_state *old_state,
					const struct drm_plane_state *state)
{
	struct drm_rect src;

	iter->plane_src.x1 = iter->plane_src.y1 = 0;
	iter->plane_src.x2 = iter->plane_src.y2 = 0;
	iter->clips = NULL;
	iter->num_clips = 0;
	iter->curr_clip = 0;
	iter->full_update = false;

	if (!state || state->crtc < 0 || !state->fb || !state->visible)
		return;

	iter->clips = (const struct drm_rect *)drm_plane_get_damage_clips(state);
	iter->num_clips = drm_plane_get_damage_clips_count(state);

	/* Round x1/y1 down and x2/y2 up, to catch every pixel the 16.16
	 * source touches. */
	src = drm_plane_state_src(state);

	iter->plane_src.x1 = src.x1 >> 16;
	iter->plane_src.y1 = src.y1 >> 16;
	iter->plane_src.x2 = (src.x2 >> 16) + !!(src.x2 & 0xFFFF);
	iter->plane_src.y2 = (src.y2 >> 16) + !!(src.y2 & 0xFFFF);

	/*
	 * The whole source, once, when there are no clips, when the driver
	 * redraws everything anyway, or when the source moved: the damage is
	 * relative to what was shown, and that is no longer where it was.
	 */
	if (!iter->clips || state->ignore_damage_clips || !old_state ||
	    !drm_rect_equals(&state->src, &old_state->src)) {
		iter->clips = NULL;
		iter->num_clips = 0;
		iter->full_update = true;
	}
}

/*
 * Clips lying wholly outside the source are skipped; the others come back
 * intersected with the source as rounded out in iter_init.  A first call
 * that returns false means the plane needs no update at all.
 */
bool drm_atomic_helper_damage_iter_next(struct drm_atomic_helper_damage_iter *iter,
					struct drm_rect *rect)
{
	bool ret = false;

	if (iter->full_update) {
		*rect = iter->plane_src;
		iter->full_update = false;
		return true;
	}

	while (iter->curr_clip < iter->num_clips) {
		*rect = iter->clips[iter->curr_clip];
		iter->curr_clip++;

		if (drm_rect_intersect(rect, &iter->plane_src)) {
			ret = true;
			break;
		}
	}

	return ret;
}

bool drm_atomic_helper_damage_merged(const struct drm_plane_state *old_state,
				     const struct drm_plane_state *state,
				     struct drm_rect *rect)
{
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	bool valid = false;

	rect->x1 = 0x7fffffff;
	rect->y1 = 0x7fffffff;
	rect->x2 = 0;
	rect->y2 = 0;

	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		rect->x1 = min(rect->x1, clip.x1);
		rect->y1 = min(rect->y1, clip.y1);
		rect->x2 = max(rect->x2, clip.x2);
		rect->y2 = max(rect->y2, clip.y2);
		valid = true;
	}

	return valid;
}
