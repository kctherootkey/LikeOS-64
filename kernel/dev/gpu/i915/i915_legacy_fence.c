// LikeOS -- fence registers, tiling and swizzling before Broadwell.
//
// A fence register names a range of the global address space and how it
// is tiled, and everything that reaches memory through that range -- the
// processor through the aperture, and on gen2/3 also the engines and the
// display -- sees the tiled layout as if it were linear.  Gen2 has eight
// of them, describing power-of-two regions from 512 KB aligned to their
// size; gen3 the same from 1 MB (and eight more on the i945, G33 and
// Pineview); gen4/5 sixteen 64-bit registers with page-granular start
// and end; Gen6/7 sixteen (Sandy Bridge, Valleyview) or thirty-two at
// another offset, with the pitch moved into the high word.
//
// The fences are few, so they are lent: a mapping or a scanout pins its
// fence while it lasts; a submission on gen2/3 takes one for the batch's
// tiled objects and leaves it, to be taken back once the object is idle.
//
// How memory is interleaved between channels decides whether the
// engines' tiled accesses swizzle bit 6 of the address, which a client
// writing a tiled surface through the processor must repeat; the memory
// controller's registers say, chipset by chipset.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define DISPPLANE_TILED (1 << 10)

static int ilog2_u32(uint32_t v)
{
	int n = -1;
	while (v) {
		v >>= 1;
		n++;
	}
	return n;
}

static int ffs_u32(uint32_t v)
{
	return v ? ilog2_u32(v & -v) + 1 : 0;
}

static int is_pow2(uint32_t v)
{
	return v && !(v & (v - 1));
}

int i915_legacy_num_fences(struct i915_device *i915)
{
	int p = i915->info->platform;

	if (!i915->bar_aperture.size)
		return 0;
	if (leg_gen(i915) >= 7 && p != I915_PLATFORM_VALLEYVIEW)
		return 32;
	if (leg_gen(i915) >= 4 || p == I915_PLATFORM_I945G || p == I915_PLATFORM_I945GM || g_leg.is_g33)
		return 16;
	return 8;
}

/* ---- swizzling ---------------------------------------------------------------- */

void leg_detect_swizzle(struct i915_device *i915)
{
	uint32_t sx = I915_BIT_6_SWIZZLE_UNKNOWN, sy = I915_BIT_6_SWIZZLE_UNKNOWN;
	int gen = leg_gen(i915);
	int p = i915->info->platform;

	if (p == I915_PLATFORM_VALLEYVIEW) {
		/* no GPU swizzling there */
		sx = sy = I915_BIT_6_SWIZZLE_NONE;
	} else if (gen >= 6) {
		/* What the firmware's tiled framebuffer was laid out with
		 * stays; otherwise swizzle when both channels hold DIMMs of
		 * the same size (the third channel never does on a part
		 * with graphics). */
		if (i915->boot_scanout.pipe >= 0 && (i915->boot_scanout.plane_ctl & DISPPLANE_TILED)) {
			if (i915_read32(i915, DISP_ARB_CTL) & DISP_TILE_SURFACE_SWIZZLING) {
				sx = I915_BIT_6_SWIZZLE_9_10;
				sy = I915_BIT_6_SWIZZLE_9;
			} else {
				sx = sy = I915_BIT_6_SWIZZLE_NONE;
			}
		} else {
			uint32_t c0 = i915_read32(i915, MAD_DIMM_C0) &
				      (MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK);
			uint32_t c1 = i915_read32(i915, MAD_DIMM_C1) &
				      (MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK);
			if (c0 == c1) {
				sx = I915_BIT_6_SWIZZLE_9_10;
				sy = I915_BIT_6_SWIZZLE_9;
			} else {
				sx = sy = I915_BIT_6_SWIZZLE_NONE;
			}
		}
	} else if (gen == 5) {
		/* Ironlake swizzles the same whatever the DRAM */
		sx = I915_BIT_6_SWIZZLE_9_10;
		sy = I915_BIT_6_SWIZZLE_9;
	} else if (gen == 2) {
		sx = sy = I915_BIT_6_SWIZZLE_NONE;
	} else if (p == I915_PLATFORM_G45 || p == I915_PLATFORM_I965G || p == I915_PLATFORM_G33) {
		/* The 965 and G33 interleave as much memory as they can
		 * and swizzle tiled accesses where the ranks pair up; where
		 * they do not, it varies with the page, which is unknown. */
		if (leg_read16(i915, C0DRB3_BW) == leg_read16(i915, C1DRB3_BW)) {
			sx = I915_BIT_6_SWIZZLE_9_10;
			sy = I915_BIT_6_SWIZZLE_9;
		}
	} else {
		uint32_t dcc = i915_read32(i915, DCC);

		/* 9xx: single channel or asymmetric, nobody swizzles;
		 * interleaved, the engines swizzle on bits 9 and 10 (X) or
		 * 9 (Y), and the processor may xor bit 11 or 17 in too */
		switch (dcc & DCC_ADDRESSING_MODE_MASK) {
		case DCC_ADDRESSING_MODE_SINGLE_CHANNEL:
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_ASYMMETRIC:
			sx = sy = I915_BIT_6_SWIZZLE_NONE;
			break;
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_INTERLEAVED:
			if (dcc & DCC_CHANNEL_XOR_DISABLE) {
				sx = I915_BIT_6_SWIZZLE_9_10;
				sy = I915_BIT_6_SWIZZLE_9;
			} else if (!(dcc & DCC_CHANNEL_XOR_BIT_17)) {
				sx = I915_BIT_6_SWIZZLE_9_10_11;
				sy = I915_BIT_6_SWIZZLE_9_11;
			} else {
				sx = I915_BIT_6_SWIZZLE_9_10_17;
				sy = I915_BIT_6_SWIZZLE_9_17;
			}
			break;
		}
		/* L-shaped memory (modified enhanced addressing) */
		if (gen == 4 && !(i915_read32(i915, DCC2) & DCC2_MODIFIED_ENHANCED_DISABLE))
			sx = sy = I915_BIT_6_SWIZZLE_UNKNOWN;
		if (dcc == 0xffffffffu) {
			kprintf("[drm] i915: cannot read the memory controller; tiling swizzle unknown\n");
			sx = sy = I915_BIT_6_SWIZZLE_UNKNOWN;
		}
	}
	/* A client that sees "unknown" refuses to tile at all; say none and
	 * report the physical mode as unknown, as the interface expects. */
	g_leg.swizzle_unknown = sx == I915_BIT_6_SWIZZLE_UNKNOWN || sy == I915_BIT_6_SWIZZLE_UNKNOWN;
	if (g_leg.swizzle_unknown)
		sx = sy = I915_BIT_6_SWIZZLE_NONE;
	g_leg.swizzle_x = sx;
	g_leg.swizzle_y = sy;
}

void i915_legacy_swizzle(struct i915_device *i915, uint32_t tiling, uint32_t *swizzle,
			 uint32_t *phys_swizzle)
{
	uint32_t s;

	(void)i915;
	switch (tiling) {
	case I915_TILING_X:
		s = g_leg.swizzle_x;
		break;
	case I915_TILING_Y:
		s = g_leg.swizzle_y;
		break;
	default:
		s = I915_BIT_6_SWIZZLE_NONE;
		break;
	}
	if (phys_swizzle)
		*phys_swizzle = g_leg.swizzle_unknown ? I915_BIT_6_SWIZZLE_UNKNOWN : s;
	/* bit 17 depends on where each page landed; pages here never move,
	 * so the client only ever needs the bit-6 part */
	if (s == I915_BIT_6_SWIZZLE_9_17)
		s = I915_BIT_6_SWIZZLE_9;
	if (s == I915_BIT_6_SWIZZLE_9_10_17)
		s = I915_BIT_6_SWIZZLE_9_10;
	if (swizzle)
		*swizzle = s;
}

static int has_128_byte_y_tiling(struct i915_device *i915)
{
	return !leg_is(i915, I915_PLATFORM_I915G) && !leg_is(i915, I915_PLATFORM_I915GM);
}

int i915_legacy_tiling_ok(struct i915_device *i915, uint32_t tiling, uint32_t stride)
{
	uint32_t tile_width;

	if (tiling == I915_TILING_NONE)
		return 1;
	if (tiling > I915_TILING_Y)
		return 0;
	if (leg_gen(i915) >= 7) {
		if (stride / 128 > GEN7_FENCE_MAX_PITCH_VAL)
			return 0;
	} else if (leg_gen(i915) >= 4) {
		if (stride / 128 > I965_FENCE_MAX_PITCH_VAL)
			return 0;
	} else {
		if (stride > 8192 || !is_pow2(stride))
			return 0;
	}
	if (tiling == I915_TILING_Y && has_128_byte_y_tiling(i915))
		tile_width = 128;
	else if (leg_gen(i915) == 2)
		tile_width = 128;
	else
		tile_width = 512;
	return stride && !(stride % tile_width);
}

/* ---- fence geometry ------------------------------------------------------------- */

/* The global range a fence over the object needs: the object itself from
 * gen4 on, a power of two from 512 KB (gen2) or 1 MB (gen3) before. */
static uint32_t fence_size(struct i915_device *i915, uint64_t size, uint32_t tiling)
{
	uint32_t s;

	if (tiling == I915_TILING_NONE || leg_gen(i915) >= 4)
		return (uint32_t)((size + 4095) & ~4095ULL);
	s = leg_gen(i915) == 3 ? (1u << 20) : (512u << 10);
	while (s < size)
		s <<= 1;
	return s;
}

static uint32_t fence_alignment(struct i915_device *i915, uint64_t size, uint32_t tiling)
{
	if (tiling == I915_TILING_NONE || leg_gen(i915) >= 4)
		return 4096;
	return fence_size(i915, size, tiling);
}

static int unfenced_needs_alignment(struct i915_device *i915)
{
	return leg_gen(i915) <= 3 && !g_leg.is_g33;
}

void i915_legacy_exec_layout(struct i915_device *i915, struct drm_gem_object *o,
			     uint64_t ex_flags, uint32_t *npages, uint64_t *align, int *mappable)
{
	struct i915_bo *bo = o->priv;

	*npages = o->npages;
	*mappable = 0;
	if (leg_gen(i915) >= 4 || !bo || bo->tiling == I915_TILING_NONE)
		return;
	if (!(ex_flags & EXEC_OBJECT_NEEDS_FENCE) && !unfenced_needs_alignment(i915))
		return;
	/* a tiled object the engine reaches through a fence: the fence's
	 * whole region, at its alignment, behind the aperture */
	uint32_t fs = fence_size(i915, o->size, bo->tiling);
	*npages = fs / 4096;
	if (*align < fs)
		*align = fs;
	*mappable = 1;
}

int i915_legacy_exec_misplaced(struct i915_device *i915, struct drm_gem_object *o,
			       const struct i915_vma *v, uint64_t ex_flags)
{
	struct i915_bo *bo = o->priv;
	uint32_t npages;
	uint64_t align = 4096;
	int mappable;

	if (!v->attached)
		return 1;
	/* a binding an aperture mapping is using stays where it is */
	if (bo && bo->gtt_map_bound && !bo->gtt_map_own && bo->gtt_map_ggtt == v->addr)
		return 0;
	i915_legacy_exec_layout(i915, o, ex_flags, &npages, &align, &mappable);
	if (!mappable)
		return 0;
	return v->npages < npages || (v->addr & (align - 1)) ||
	       v->addr + (uint64_t)npages * 4096 > g_leg.mappable_end;
}

/* ---- the registers ----------------------------------------------------------------- */

static void fence_write_hw(struct i915_device *i915, int id)
{
	struct leg_fence *f = &g_leg.fences[id];
	int gen = leg_gen(i915);

	if (gen >= 4) {
		uint32_t lo_reg = gen >= 6 ? FENCE_REG_GEN6_LO(id) : FENCE_REG_965_LO(id);
		uint32_t hi_reg = gen >= 6 ? FENCE_REG_GEN6_HI(id) : FENCE_REG_965_HI(id);
		int pitch_shift = gen >= 6 ? GEN6_FENCE_PITCH_SHIFT : I965_FENCE_PITCH_SHIFT;
		uint64_t val = 0;

		if (f->tiling) {
			val = (uint64_t)(f->start + f->size - I965_FENCE_PAGE) << 32;
			val |= f->start;
			val |= (uint64_t)(f->stride / 128 - 1) << pitch_shift;
			if (f->tiling == I915_TILING_Y)
				val |= 1u << I965_FENCE_TILING_Y_SHIFT;
			val |= I965_FENCE_REG_VALID;
		}
		/* Two 32-bit writes are not atomic: switch the register off,
		 * write the high word, then the low word with the valid
		 * bit -- each landing before the next. */
		i915_write32(i915, lo_reg, 0);
		(void)i915_read32(i915, lo_reg);
		i915_write32(i915, hi_reg, (uint32_t)(val >> 32));
		i915_write32(i915, lo_reg, (uint32_t)val);
		(void)i915_read32(i915, lo_reg);
		return;
	}
	uint32_t val = 0;
	if (f->tiling) {
		uint32_t stride = f->stride;
		int y = f->tiling == I915_TILING_Y;

		val = f->start;
		if (y)
			val |= 1u << I830_FENCE_TILING_Y_SHIFT;
		if (gen == 3) {
			stride /= (y && has_128_byte_y_tiling(i915)) ? 128 : 512;
			val |= (uint32_t)(ffs_u32(f->size >> 20) - 1) << 8;
			val |= (uint32_t)ilog2_u32(stride) << I830_FENCE_PITCH_SHIFT;
		} else {
			val |= (uint32_t)(ffs_u32(f->size >> 19) - 1) << 8;
			val |= (uint32_t)ilog2_u32(stride / 128) << I830_FENCE_PITCH_SHIFT;
		}
		val |= I830_FENCE_REG_VALID;
	}
	i915_write32(i915, FENCE_REG(id), val);
	(void)i915_read32(i915, FENCE_REG(id));
}

int i915_legacy_fences_init(struct i915_device *i915)
{
	spinlock_init(&i915->fence_lock, "i915_fence");
	i915->num_fences = i915_legacy_num_fences(i915);
	for (int i = 0; i < 32; i++) {
		i915->fence_obj[i] = NULL;
		mm_memset(&g_leg.fences[i], 0, sizeof(g_leg.fences[i]));
	}
	for (int i = 0; i < i915->num_fences; i++)
		fence_write_hw(i915, i);
	leg_detect_swizzle(i915);
	i915_dbg("[drm] i915: %d fence registers, bit-6 swizzle X %u Y %u\n", i915->num_fences,
		 g_leg.swizzle_x, g_leg.swizzle_y);
	return 0;
}

void i915_legacy_fences_restore(struct i915_device *i915)
{
	uint64_t fl;

	spin_lock_irqsave(&i915->fence_lock, &fl);
	for (int i = 0; i < i915->num_fences; i++)
		fence_write_hw(i915, i);
	spin_unlock_irqrestore(&i915->fence_lock, fl);
}

/* Nothing of the engines' still uses the object. */
static int object_idle(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	uint64_t fl;
	int idle = 1;

	if (!bo)
		return 1;
	spin_lock_irqsave(&i915->sync_lock, &fl);
	if (bo->write_fence && !bo->write_fence->signaled)
		idle = 0;
	for (int c = 0; c < I915_ENGINE_CLASSES; c++)
		if (bo->read_fence[c] && !bo->read_fence[c]->signaled)
			idle = 0;
	spin_unlock_irqrestore(&i915->sync_lock, fl);
	return idle;
}

/* A fence for the object over [start, start + size), with fence_lock
 * held: the one it has (rewritten if its geometry moved), a free one,
 * or the least recently used one whose object is idle and unpinned. */
static int fence_get(struct i915_device *i915, struct drm_gem_object *o, uint32_t start,
		     uint32_t size, int pin)
{
	struct i915_bo *bo = o->priv;
	int id = bo->fence_id;

	if (id >= 0 && id < i915->num_fences && i915->fence_obj[id] == o) {
		struct leg_fence *f = &g_leg.fences[id];
		if (f->start != start || f->size != size || f->stride != bo->stride ||
		    f->tiling != bo->tiling) {
			if (f->pin)
				return -EBUSY;
			if (!object_idle(i915, o))
				return -EAGAIN;
			f->start = start;
			f->size = size;
			f->stride = bo->stride;
			f->tiling = bo->tiling;
			fence_write_hw(i915, id);
		}
		if (pin)
			f->pin++;
		f->lru = ++g_leg.fence_clock;
		return id;
	}
	id = -1;
	for (int i = 0; i < i915->num_fences; i++) {
		if (!i915->fence_obj[i]) {
			id = i;
			break;
		}
	}
	if (id < 0) {
		uint64_t best = ~0ULL;
		for (int i = 0; i < i915->num_fences; i++) {
			struct leg_fence *f = &g_leg.fences[i];
			if (f->pin || f->lru >= best || !object_idle(i915, i915->fence_obj[i]))
				continue;
			best = f->lru;
			id = i;
		}
		if (id < 0)
			return -EAGAIN;
		((struct i915_bo *)i915->fence_obj[id]->priv)->fence_id = -1;
	}
	struct leg_fence *f = &g_leg.fences[id];
	f->start = start;
	f->size = size;
	f->stride = bo->stride;
	f->tiling = bo->tiling;
	f->pin = pin ? 1 : 0;
	f->lru = ++g_leg.fence_clock;
	i915->fence_obj[id] = o;
	bo->fence_id = id;
	fence_write_hw(i915, id);
	return id;
}

static void fence_release(struct i915_device *i915, struct drm_gem_object *o, int unpin)
{
	struct i915_bo *bo = o->priv;
	int id = bo->fence_id;

	if (id < 0 || id >= i915->num_fences || i915->fence_obj[id] != o)
		return;
	if (unpin && g_leg.fences[id].pin)
		g_leg.fences[id].pin--;
	if (g_leg.fences[id].pin)
		return;
	mm_memset(&g_leg.fences[id], 0, sizeof(g_leg.fences[id]));
	i915->fence_obj[id] = NULL;
	bo->fence_id = -1;
	fence_write_hw(i915, id);
}

int i915_legacy_exec_fence(struct i915_device *i915, struct drm_gem_object *o, uint64_t addr)
{
	struct i915_bo *bo = o->priv;
	uint64_t fl;
	int rc = 0;

	if (leg_gen(i915) >= 4 || !bo)
		return 0;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	if (bo->tiling == I915_TILING_NONE) {
		/* an untiled object reached through a stale fence would be
		 * detiled by it: the fence goes */
		if (bo->fence_id >= 0 && !g_leg.fences[bo->fence_id].pin)
			fence_release(i915, o, 0);
	} else {
		int id = fence_get(i915, o, (uint32_t)addr, fence_size(i915, o->size, bo->tiling), 0);
		if (id < 0)
			rc = id;
	}
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	if (rc == -EBUSY) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: a tiled object is mapped at another address than the engine needs it\n");
		}
	}
	return rc;
}

/* ---- mappings through the aperture --------------------------------------------- */

static int usable_binding(struct i915_device *i915, struct drm_gem_object *o, uint32_t *addr);

int i915_legacy_exec_shared_binding(struct i915_device *i915, struct drm_gem_object *o,
				    uint64_t ex_flags, uint64_t *addr)
{
	struct i915_bo *bo = o->priv;
	uint64_t fl;
	uint32_t a = 0;
	int found = 0;

	if (leg_gen(i915) >= 4 || !bo || bo->tiling == I915_TILING_NONE ||
	    !(ex_flags & EXEC_OBJECT_NEEDS_FENCE))
		return 0;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	int id = bo->fence_id;
	if (id >= 0 && id < i915->num_fences && i915->fence_obj[id] == o && g_leg.fences[id].pin) {
		/* the fence is held where the plane or a mapping reads */
		a = g_leg.fences[id].start;
		found = 1;
	} else if (bo->gtt_map_bound) {
		a = bo->gtt_map_ggtt;
		found = 1;
	} else if (bo->bound && bo->ggtt_size && usable_binding(i915, o, &a) && a == bo->ggtt) {
		/* a scanout surface bound fenced, between flips */
		found = 1;
	}
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	if (found)
		*addr = a;
	return found;
}

/* Where the object already is in the global space, if that binding
 * suits an aperture mapping (and on gen2/3 a fence over it). */
static int usable_binding(struct i915_device *i915, struct drm_gem_object *o, uint32_t *addr)
{
	struct i915_bo *bo = o->priv;
	uint32_t fs = fence_size(i915, o->size, bo->tiling);
	uint32_t fa = fence_alignment(i915, o->size, bo->tiling);

	if (leg_gen(i915) <= 5) {
		for (struct i915_vma *v = bo->vmas; v; v = v->next) {
			if (v->vm != g_leg.vm || !v->attached)
				continue;
			if ((uint64_t)v->npages * 4096 >= fs && !(v->addr & (fa - 1)) &&
			    v->addr + fs <= g_leg.mappable_end) {
				*addr = (uint32_t)v->addr;
				return 1;
			}
		}
	}
	/* the global binding, where it reserves the fence's whole region
	 * (an object of exactly that size, or a tiled scanout surface the
	 * display bound fenced) */
	uint64_t reserved = bo->ggtt_size ? bo->ggtt_size : ((o->size + 4095) & ~4095ULL);
	if (bo->bound && (uint64_t)bo->ggtt + fs <= g_leg.mappable_end && !(bo->ggtt & (fa - 1)) &&
	    fs <= reserved) {
		*addr = bo->ggtt;
		return 1;
	}
	return 0;
}

/* A binding of its own behind the aperture: fence-aligned and fence-sized
 * on gen2/3 (the tail past the object leads to scratch). */
static int bind_fenced(struct i915_device *i915, struct drm_gem_object *o, uint32_t *ggtt)
{
	struct i915_bo *bo = o->priv;
	uint32_t fs = fence_size(i915, o->size, bo->tiling);
	uint32_t fa = fence_alignment(i915, o->size, bo->tiling);
	uint32_t first;
	int rc;

	if (fs == ((o->size + 4095) & ~4095ULL) && fa == 4096)
		return i915_legacy_ggtt_bind_obj_mappable(i915, o, ggtt);
	rc = leg_ggtt_alloc(i915, fs / 4096, fa / 4096, 0, (uint32_t)(g_leg.mappable_end / 4096), 0,
			    &first);
	if (rc)
		return rc;
	for (uint32_t i = 0; i < fs / 4096; i++)
		leg_ggtt_write(i915, first + i,
			       i < o->npages ? leg_pte_encode(i915, o->pages[i], LEG_CACHE_NONE, 0) :
					       g_leg.scratch_pte);
	leg_ggtt_invalidate(i915);
	*ggtt = first * 4096;
	return 0;
}

int i915_legacy_ggtt_bind_fenced(struct i915_device *i915, struct drm_gem_object *o, uint32_t *ggtt)
{
	return bind_fenced(i915, o, ggtt);
}

uint32_t i915_legacy_fence_size(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	return fence_size(i915, o->size, bo ? bo->tiling : I915_TILING_NONE);
}

/* Whether the object's fence is held by its aperture mapping. */
static int map_pinned(struct i915_device *i915, struct drm_gem_object *o)
{
	int id = ((struct i915_bo *)o->priv)->fence_id;

	return id >= 0 && id < i915->num_fences && i915->fence_obj[id] == o &&
	       g_leg.fences[id].map_pinned;
}

int i915_legacy_gtt_map_in(struct i915_device *i915, struct drm_gem_object *o, uint32_t *base)
{
	struct i915_bo *bo = o->priv;

	if (!bo->gtt_map_bound) {
		uint32_t addr;
		if (usable_binding(i915, o, &addr)) {
			bo->gtt_map_ggtt = addr;
			bo->gtt_map_own = 0;
		} else {
			int rc = bind_fenced(i915, o, &addr);
			if (rc)
				return rc;
			bo->gtt_map_ggtt = addr;
			bo->gtt_map_own = 1;
		}
		bo->gtt_map_bound = 1;
	}
	if (bo->tiling != I915_TILING_NONE && !map_pinned(i915, o)) {
		int id = fence_get(i915, o, bo->gtt_map_ggtt, fence_size(i915, o->size, bo->tiling),
				   1);
		if (id < 0) {
			static int said;
			if (said < 4) {
				said++;
				kprintf("[drm] i915: no fence register free for a tiled object of %u pages (%d in use)\n",
					o->npages, i915->num_fences);
			}
			return -ENOSPC;
		}
		g_leg.fences[id].map_pinned = 1;
	}
	*base = bo->gtt_map_ggtt;
	return 0;
}

void i915_legacy_gtt_map_out(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;

	if (map_pinned(i915, o)) {
		g_leg.fences[bo->fence_id].map_pinned = 0;
		fence_release(i915, o, 1);
	} else if (bo->fence_id >= 0 && bo->fence_id < i915->num_fences &&
		   !g_leg.fences[bo->fence_id].pin && object_idle(i915, o)) {
		/* an engine fence of an idle object goes with the mapping
		 * (and certainly with the object, which is idle when it is
		 * freed) */
		fence_release(i915, o, 0);
	}
	if (bo->gtt_map_bound && bo->gtt_map_own)
		i915_legacy_ggtt_unbind(i915, bo->gtt_map_ggtt,
					fence_size(i915, o->size, bo->tiling));
	bo->gtt_map_own = 0;
	bo->gtt_map_bound = 0;
}

int i915_legacy_fence_pin(struct i915_device *i915, struct drm_gem_object *o, uint32_t ggtt)
{
	struct i915_bo *bo = o->priv;
	uint64_t fl;
	int id;

	if (!bo || bo->tiling == I915_TILING_NONE)
		return 0;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	id = fence_get(i915, o, ggtt, fence_size(i915, o->size, bo->tiling), 1);
	spin_unlock_irqrestore(&i915->fence_lock, fl);
	return id < 0 ? id : 0;
}

void i915_legacy_fence_unpin(struct i915_device *i915, struct drm_gem_object *o)
{
	uint64_t fl;

	if (!o->priv)
		return;
	spin_lock_irqsave(&i915->fence_lock, &fl);
	fence_release(i915, o, 1);
	spin_unlock_irqrestore(&i915->fence_lock, fl);
}
