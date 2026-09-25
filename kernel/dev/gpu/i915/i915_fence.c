// LikeOS -- mappings of objects through the graphics aperture, with fences.
//
// A client that asks for the "GTT" kind of mapping expects to see an
// object through the device: the pages behind the aperture BAR, which the
// hardware translates through the global address space and, for a tiled
// object, through a fence register that turns the linear addresses the
// program uses into the tiled layout the engines use.  That is how a
// program reads a decoded video frame (a tiled surface) row by row.
//
// The mapping is lazy: nothing is set up until a page is touched.  At the
// first fault the object is bound in the part of the global space the
// aperture covers and, if tiled, given a fence; both are kept until the
// last mapping of the object goes away.  Fences are not taken from a
// mapping in use to serve another: a mapping that finds none free fails
// its access, and the kernel says so once.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_fence_layout.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* One mapping (one mmap call) of an object through the aperture. */
struct i915_gtt_map {
	struct drm_gem_object *obj; /* referenced for the mapping's life */
	uint32_t first_page;
	int refs; /* region records */
};

static void fence_write(struct i915_device *i915, int id, uint64_t val)
{
	i915_write32(i915, I915_FENCE_REG_GEN6_LO(id), 0);
	(void)i915_read32(i915, I915_FENCE_REG_GEN6_LO(id));
	i915_write32(i915, I915_FENCE_REG_GEN6_HI(id), (uint32_t)(val >> 32));
	i915_write32(i915, I915_FENCE_REG_GEN6_LO(id), (uint32_t)val);
	(void)i915_read32(i915, I915_FENCE_REG_GEN6_LO(id));
}

int i915_fences_init(struct i915_device *i915)
{
	i915->num_fences = i915->info->gen >= 6 ? I915_NUM_FENCES_GEN6 : 0;
	spinlock_init(&i915->fence_lock, "i915_fence");
	for (int i = 0; i < I915_NUM_FENCES_GEN6; i++)
		i915->fence_obj[i] = NULL;
	/* every register invalid until an object needs one */
	if (i915->mmio_virt)
		for (int i = 0; i < i915->num_fences; i++)
			fence_write(i915, i, 0);
	return 0;
}

/* After a reset or resume the registers are written again from the
 * objects that hold them. */
void i915_fences_restore(struct i915_device *i915)
{
	uint64_t fl;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	for (int i = 0; i < i915->num_fences; i++) {
		struct drm_gem_object *o = i915->fence_obj[i];
		struct i915_bo *bo = o ? o->priv : NULL;
		uint64_t val = 0;
		if (bo && bo->gtt_map_bound)
			val = i915_fence_value(bo->ggtt, (uint64_t)o->npages * 4096,
					       bo->stride, bo->tiling == I915_TILING_Y);
		fence_write(i915, i, val);
	}
	spin_unlock_irqrestore(&i915->fence_lock, fl);
}

/* With fence_lock held: a fence for the object, or -1. */
static int fence_take(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (bo->fence_id >= 0)
		return bo->fence_id;
	for (int i = 0; i < i915->num_fences; i++) {
		if (i915->fence_obj[i])
			continue;
		uint64_t val = i915_fence_value(bo->ggtt, (uint64_t)o->npages * 4096,
						bo->stride, bo->tiling == I915_TILING_Y);
		if (!val)
			return -1;
		i915->fence_obj[i] = o;
		bo->fence_id = i;
		fence_write(i915, i, val);
		return i;
	}
	return -1;
}

static void fence_drop(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (bo->fence_id < 0)
		return;
	i915->fence_obj[bo->fence_id] = NULL;
	fence_write(i915, bo->fence_id, 0);
	bo->fence_id = -1;
}

/* The object behind the aperture: bound where the aperture reaches, fenced
 * if tiled.  With fence_lock held. */
static int object_map_in(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	uint64_t aperture = i915->bar_aperture.size;

	if (!bo->gtt_map_bound) {
		/* an existing binding inside the aperture serves (a scanout
		 * buffer); otherwise one is made low in the global space */
		if (bo->bound && (uint64_t)bo->ggtt + (uint64_t)o->npages * 4096 <= aperture) {
			bo->gtt_map_bound = 1;
			bo->gtt_map_own = 0;
		} else {
			uint32_t ggtt = 0;
			int rc = i915_ggtt_bind_obj_mappable(i915, o, &ggtt);
			if (rc)
				return rc;
			bo->gtt_map_ggtt = ggtt;
			bo->gtt_map_bound = 1;
			bo->gtt_map_own = 1;
		}
	}
	if (bo->tiling != I915_TILING_NONE && bo->fence_id < 0) {
		/* a fence describes the object's whole binding */
		uint32_t start = bo->gtt_map_own ? bo->gtt_map_ggtt : bo->ggtt;
		uint32_t saved = bo->ggtt;
		bo->ggtt = start;
		int id = fence_take(i915, o);
		bo->ggtt = saved;
		if (id < 0) {
			static int said;
			if (said < 4) {
				said++;
				kprintf("[drm] i915: no fence register free for a tiled object of %u pages (%d in use)\n",
					o->npages, i915->num_fences);
			}
			return -ENOSPC;
		}
	}
	return 0;
}

static void object_map_out(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	fence_drop(i915, o);
	if (bo->gtt_map_bound && bo->gtt_map_own) {
		i915_ggtt_unbind(i915, bo->gtt_map_ggtt, (uint32_t)(o->npages * 4096));
		bo->gtt_map_own = 0;
	}
	bo->gtt_map_bound = 0;
}

/* The page behind the aperture for index `index' of the mapping. */
static uint64_t gtt_map_fault(void *ctx, uint64_t index)
{
	struct i915_gtt_map *m = ctx;
	struct drm_gem_object *o = m->obj;
	struct i915_bo *bo = o->priv;
	struct i915_device *i915 = &g_i915;
	uint64_t page = m->first_page + index;
	uint64_t fl;

	if (!bo || page >= o->npages)
		return 0;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	int rc = object_map_in(i915, o);
	uint32_t base = bo->gtt_map_own ? bo->gtt_map_ggtt : bo->ggtt;
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	if (rc)
		return 0;
	return i915->bar_aperture.base + (uint64_t)base + page * 4096;
}

static void gtt_map_get(void *ctx)
{
	struct i915_gtt_map *m = ctx;
	__atomic_fetch_add(&m->refs, 1, __ATOMIC_ACQ_REL);
}

static void gtt_map_put(void *ctx)
{
	struct i915_gtt_map *m = ctx;
	if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	struct drm_gem_object *o = m->obj;
	struct i915_bo *bo = o->priv;
	struct i915_device *i915 = &g_i915;
	uint64_t fl;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	if (bo && --bo->gtt_map_refs == 0)
		object_map_out(i915, o);
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	kfree(m);
	drm_gem_put(o);
}

/* The driver's mapping kinds: 4 is the aperture. */
int i915_gem_mmap_kind(struct drm_gem_object *o, unsigned kind, uint64_t first_page,
		       struct device_mmap *m)
{
	struct i915_device *i915 = &g_i915;
	struct i915_bo *bo = o->priv;

	if (kind != I915_MMAP_KIND_GTT)
		return 0;
	if (!bo || !o->pages || !i915->bar_aperture.size || !i915->gtt_virt)
		return -ENODEV;
	if (bo->userptr)
		return -EINVAL;
	struct i915_gtt_map *gm = kalloc(sizeof(*gm));
	if (!gm)
		return -ENOMEM;
	gm->obj = o;
	gm->first_page = (uint32_t)first_page;
	gm->refs = 1;
	drm_gem_get(o);
	uint64_t fl;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	bo->gtt_map_refs++;
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	m->lazy = 1;
	m->fault = gtt_map_fault;
	m->page_phys = NULL;
	m->obj = gm;
	m->get = gtt_map_get;
	m->put = gtt_map_put;
	m->pte_extra = PAGE_WRITE_THROUGH; /* write-combining, like the device */
	m->dirty_ops = NULL;
	return 1;
}

/* The object is going: nothing maps it any more, but a binding the map
 * path made is released here for safety. */
void i915_fence_object_free(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	uint64_t fl;
	if (!bo)
		return;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	if (bo->gtt_map_bound || bo->fence_id >= 0)
		object_map_out(i915, o);
	spin_unlock_irqrestore(&i915->fence_lock, fl);
}
