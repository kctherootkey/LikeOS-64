// LikeOS -- rotated views of objects in the global GTT: the page order
// of a view, and the views each object keeps (i915_ggtt_view.h says what
// they are for).
//
// The order: for every tile column of the plane, left to right, the
// tiles of that column from the bottom tile row up, then padding up to
// the view's column stride.  The display, scanning the view row by row
// with its rotation bits set, sees the surface turned by 90 degrees.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2016 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2021 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_ggtt_view.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the layout ----------------------------------------------------------------- */

uint64_t i915_rotation_info_size(const struct i915_rotation_info *r)
{
	uint64_t size = 0;

	for (unsigned i = 0; i < I915_ROTATION_PLANES; i++)
		size += (uint64_t)r->plane[i].dst_stride * r->plane[i].width;
	return size;
}

/* Page `idx' of the view: the planes follow each other, each plane is
 * `width' columns of `dst_stride' pages, and page `row' of column
 * `column' is the object's tile at tile row height - 1 - row of that
 * column -- the first page of a column being the bottom tile. */
uint64_t i915_rotation_view_page(const struct i915_rotation_info *r, uint64_t idx)
{
	for (unsigned i = 0; i < I915_ROTATION_PLANES; i++) {
		const struct i915_rotation_plane *p = &r->plane[i];
		uint64_t size = (uint64_t)p->dst_stride * p->width;
		uint64_t column, row;

		if (idx >= size) {
			idx -= size;
			continue;
		}
		column = idx / p->dst_stride;
		row = idx % p->dst_stride;
		if (row >= p->height)
			return I915_ROTATION_PADDING;
		return (uint64_t)p->offset + (uint64_t)p->src_stride * (p->height - 1 - row) + column;
	}
	return I915_ROTATION_PADDING;
}

int i915_rotation_info_valid(const struct i915_rotation_info *r, uint64_t npages)
{
	uint64_t size = i915_rotation_info_size(r);

	/* the global GTT is below 4 GB */
	if (!size || size > 0xfffffu)
		return 0;
	for (unsigned i = 0; i < I915_ROTATION_PLANES; i++) {
		const struct i915_rotation_plane *p = &r->plane[i];
		uint64_t last;

		if (!p->width && !p->height)
			continue;
		if (!p->width || !p->height || p->dst_stride < p->height)
			return 0;
		/* the top right tile is the last one the plane reads */
		last = (uint64_t)p->offset + (uint64_t)p->src_stride * (p->height - 1) + p->width - 1;
		if (last >= npages)
			return 0;
	}
	return 1;
}

/* ---- views of objects ------------------------------------------------------------- */

/* The views of every object and their use stamps.  Taken around the list
 * walks only: the entries are written and unbound outside it. */
static spinlock_t g_view_lock = SPINLOCK_INIT("i915_ggtt_view");
static uint64_t g_view_seq;

static int rotation_info_same(const struct i915_rotation_info *a,
			      const struct i915_rotation_info *b)
{
	for (unsigned i = 0; i < I915_ROTATION_PLANES; i++) {
		const struct i915_rotation_plane *x = &a->plane[i], *y = &b->plane[i];
		if (x->offset != y->offset || x->width != y->width || x->height != y->height ||
		    x->src_stride != y->src_stride || x->dst_stride != y->dst_stride)
			return 0;
	}
	return 1;
}

static int view_holds(const struct i915_ggtt_view *v, const uint32_t *keep, unsigned nkeep)
{
	uint64_t end = (uint64_t)v->ggtt + (uint64_t)v->npages * 4096;

	for (unsigned i = 0; i < nkeep; i++)
		if (keep[i] && keep[i] >= v->ggtt && keep[i] < end)
			return 1;
	return 0;
}

/* The view of `bo' made like `info', its use stamp renewed; NULL when it
 * has none.  Called with g_view_lock held. */
static struct i915_ggtt_view *view_find_locked(struct i915_bo *bo,
					       const struct i915_rotation_info *info)
{
	for (struct i915_ggtt_view *v = bo->views; v; v = v->next) {
		if (rotation_info_same(&v->info, info)) {
			v->used = ++g_view_seq;
			return v;
		}
	}
	return NULL;
}

struct view_fill {
	const struct drm_gem_object *o;
	const struct i915_rotation_info *info;
};

static uint64_t view_page_of(void *ctx, uint32_t i)
{
	const struct view_fill *f = ctx;
	uint64_t src = i915_rotation_view_page(f->info, i);

	/* i915_rotation_info_valid() kept every tile inside the object */
	if (src == I915_ROTATION_PADDING || src >= f->o->npages)
		return I915_ROTATION_PADDING;
	return f->o->pages[src];
}

int i915_ggtt_rotated_view_get(struct i915_device *i915, struct drm_gem_object *o,
			       const struct i915_rotation_info *info, uint32_t align,
			       const uint32_t *keep, unsigned nkeep, uint32_t *ggtt)
{
	struct i915_bo *bo = o ? o->priv : NULL;
	struct i915_ggtt_view *v, *dup, *drop = NULL;
	struct view_fill fill;
	uint32_t align_pages;
	uint64_t fl;
	int rc;

	*ggtt = 0;
	if (!bo || !o->pages || !o->npages)
		return -EINVAL;
	if (i915_is_legacy(i915))
		return -EOPNOTSUPP;

	spin_lock_irqsave(&g_view_lock, &fl);
	v = view_find_locked(bo, info);
	if (v)
		*ggtt = v->ggtt;
	spin_unlock_irqrestore(&g_view_lock, fl);
	if (v)
		return 0;

	if (!i915_rotation_info_valid(info, o->npages)) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: rotated view of %ux%u tiles at tile %u does not fit an object of %u pages\n",
				info->plane[0].width, info->plane[0].height, info->plane[0].offset,
				o->npages);
		}
		return -EINVAL;
	}
	v = kalloc(sizeof(*v));
	if (!v)
		return -ENOMEM;
	mm_memset(v, 0, sizeof(*v));
	v->info = *info;
	v->npages = (uint32_t)i915_rotation_info_size(info);
	align_pages = align >= 4096 ? align / 4096 : 1;
	rc = i915_ggtt_reserve(i915, v->npages, align_pages, &v->ggtt);
	if (rc) {
		kfree(v);
		return rc;
	}
	/* the display reads the view uncached, as it does the object */
	fill.o = o;
	fill.info = info;
	i915_ggtt_fill(i915, v->ggtt, v->npages, view_page_of, &fill, 1);

	spin_lock_irqsave(&g_view_lock, &fl);
	dup = view_find_locked(bo, info);
	if (!dup) {
		unsigned n = 0;

		v->used = ++g_view_seq;
		v->next = bo->views;
		bo->views = v;
		for (struct i915_ggtt_view *w = bo->views; w; w = w->next)
			n++;
		/* Too many: the one asked for longest ago goes, unless a pipe
		 * scans it out (it then stays until a later view is made). */
		if (n > I915_GGTT_VIEWS_PER_OBJECT) {
			struct i915_ggtt_view **victim = NULL;
			for (struct i915_ggtt_view **pp = &bo->views; *pp; pp = &(*pp)->next) {
				struct i915_ggtt_view *w = *pp;
				if (w == v || view_holds(w, keep, nkeep))
					continue;
				if (!victim || w->used < (*victim)->used)
					victim = pp;
			}
			if (victim) {
				drop = *victim;
				*victim = drop->next;
				drop->next = NULL;
			}
		}
		*ggtt = v->ggtt;
	} else {
		/* made meanwhile by another commit: that one is the view */
		*ggtt = dup->ggtt;
		drop = v;
	}
	spin_unlock_irqrestore(&g_view_lock, fl);
	if (drop) {
		i915_ggtt_unbind(i915, drop->ggtt, drop->npages * 4096u);
		kfree(drop);
	}
	i915_dbg("[drm] i915: rotated view %s at %08x (%ux%u tiles)\n", dup ? "found" : "made",
		 *ggtt, info->plane[0].width, info->plane[0].height);
	return 0;
}

void i915_ggtt_views_release(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o ? o->priv : NULL;
	struct i915_ggtt_view *list;
	uint64_t fl;

	if (!bo)
		return;
	spin_lock_irqsave(&g_view_lock, &fl);
	list = bo->views;
	bo->views = NULL;
	spin_unlock_irqrestore(&g_view_lock, fl);
	while (list) {
		struct i915_ggtt_view *v = list;
		list = v->next;
		i915_ggtt_unbind(i915, v->ggtt, v->npages * 4096u);
		kfree(v);
	}
}

unsigned i915_ggtt_views_rewrite(struct i915_device *i915)
{
	struct drm_device *dev = &i915->drm;
	unsigned n = 0;

	/* resume: nothing else runs, the object list holds still */
	for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
		struct i915_bo *bo = o->priv;
		if (!bo || !o->pages)
			continue;
		for (struct i915_ggtt_view *v = bo->views; v; v = v->next) {
			struct view_fill fill = { o, &v->info };
			i915_ggtt_fill(i915, v->ggtt, v->npages, view_page_of, &fill, 1);
			n++;
		}
	}
	return n;
}
