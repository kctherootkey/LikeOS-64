// LikeOS -- rotated views of objects in the global GTT.
//
// The display's planes scan a surface out row by row.  To show a Y-tiled
// surface turned by 90 or 270 degrees, the plane is pointed at a second
// mapping of the object in the global GTT -- a view -- whose pages are
// the object's tiles in another order: column by column, each column read
// from the bottom tile row up.  Read as an ordinary tiled surface, the
// view is the picture turned on its side; the plane's own rotation bits
// turn each tile.  A view maps the object's pages, it copies nothing:
// what a client draws into the object is in the view at once.
//
// A view is made the first time a plane needs it, kept with the object
// (like the object's ordinary scanout binding) and found again for every
// later flip to the same layout, and unbound when the object is freed.
// An object keeps a few views at most; making one more drops the one
// used longest ago that no pipe is scanning out.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2016 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2021 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_GGTT_VIEW_H
#define KERNEL_DEV_GPU_I915_GGTT_VIEW_H

#include <kernel/uapi/types.h>

struct i915_device;
struct drm_gem_object;

/* One colour plane of a rotated view, in tiles (= GTT pages): the tile
 * of the object the plane starts at, the tile columns and rows the view
 * covers, the object's tiles per tile row, and the view's pages per
 * column (at least `height'; the pages past `height' are padding the
 * display never reads). */
struct i915_rotation_plane {
	uint32_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t src_stride;
	uint32_t dst_stride;
};

/* The planes of a view, laid out one after the other; a plane of zero
 * width takes no room.  Every format the planes take today has one. */
#define I915_ROTATION_PLANES 2
struct i915_rotation_info {
	struct i915_rotation_plane plane[I915_ROTATION_PLANES];
};

/* What a page of the view maps: an index into the object's pages, or
 * this for a padding page. */
#define I915_ROTATION_PADDING ((uint64_t)-1)

/* ---- the layout (pure) ------------------------------------------------------- */

/* Pages of the view: dst_stride * width summed over the planes. */
uint64_t i915_rotation_info_size(const struct i915_rotation_info *r);
/* The object page that page `idx' of the view maps, or
 * I915_ROTATION_PADDING (also for an index past the view). */
uint64_t i915_rotation_view_page(const struct i915_rotation_info *r, uint64_t idx);
/* Whether the view can be made of an object of `npages' pages: every
 * tile it reads lies inside the object, no plane is taller than its
 * column, and it is not empty.  1 or 0. */
int i915_rotation_info_valid(const struct i915_rotation_info *r, uint64_t npages);

/* ---- views of objects ----------------------------------------------------------- */

/* A view kept with its object (struct i915_bo's `views'). */
struct i915_ggtt_view {
	struct i915_ggtt_view *next;
	struct i915_rotation_info info;
	uint32_t ggtt; /* where the view starts in the global GTT */
	uint32_t npages;
	uint64_t used; /* when a plane last asked for it (a sequence) */
};

/* How many views an object keeps before the oldest unused one goes. */
#define I915_GGTT_VIEWS_PER_OBJECT 4

/* The global GTT address of the rotated view `info' of object `o': the
 * one it already has, or one made now -- aligned to `align' bytes (a
 * power of two, at least a page), uncached as scanout is.  `keep' lists
 * `nkeep' addresses being scanned out: a view holding any of them is not
 * dropped to make room.  0, -EINVAL for a view that does not fit the
 * object, -EOPNOTSUPP where the global GTT makes no views (the parts
 * before Broadwell), -ENOSPC, -ENOMEM. */
int i915_ggtt_rotated_view_get(struct i915_device *i915, struct drm_gem_object *o,
			       const struct i915_rotation_info *info, uint32_t align,
			       const uint32_t *keep, unsigned nkeep, uint32_t *ggtt);
/* Every view of `o' unbound and freed (from the object's free). */
void i915_ggtt_views_release(struct i915_device *i915, struct drm_gem_object *o);
/* Every view's entries written again (after a resume lost the GTT's
 * contents).  Returns how many views there were. */
unsigned i915_ggtt_views_rewrite(struct i915_device *i915);

/* ---- global GTT primitives for views (i915_gtt.c) ------------------------------- */

/* A free run of `npages' pages aligned to `align_pages' pages, taken;
 * *ggtt gets its address.  0, -EINVAL, -ENOMEM, -ENOSPC.  Given back
 * with i915_ggtt_unbind(). */
int i915_ggtt_reserve(struct i915_device *i915, uint32_t npages, uint32_t align_pages,
		      uint32_t *ggtt);
/* Write the `npages' entries from `ggtt' on: entry i maps page_of(ctx, i)
 * (an object's pages[] entry), or the scratch page for
 * I915_ROTATION_PADDING; uncached or through the cache.  Flushed. */
typedef uint64_t (*i915_ggtt_page_fn)(void *ctx, uint32_t i);
void i915_ggtt_fill(struct i915_device *i915, uint32_t ggtt, uint32_t npages,
		    i915_ggtt_page_fn page_of, void *ctx, int uncached);

#endif
