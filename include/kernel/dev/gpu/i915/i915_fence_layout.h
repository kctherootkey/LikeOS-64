// LikeOS -- the fence registers' encoding and the tiled address arithmetic,
// pure so the build host can check them.
//
// A fence register names a range of the global address space and how it is
// tiled; the processor's accesses through the aperture that fall inside a
// fenced range are translated by the hardware from the linear layout the
// program sees to the tiled one in memory.  Gen6 and later have 32 of them,
// 64 bits each: the range start in the low word with the valid bit and the
// tiling mode, the range's last page in the high word with the stride (in
// 128-byte units, minus one) in its low twelve bits.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_FENCE_LAYOUT_H
#define KERNEL_DEV_GPU_I915_FENCE_LAYOUT_H

#include <kernel/uapi/types.h>

#define I915_FENCE_REG_GEN6_LO(i) (0x100000 + (i) * 8)
#define I915_FENCE_REG_GEN6_HI(i) (0x100000 + (i) * 8 + 4)
#define I915_FENCE_REG_VALID (1u << 0)
#define I915_FENCE_TILING_Y (1u << 1)
#define I915_FENCE_MAX_STRIDE (4096u * 128u)
#define I915_NUM_FENCES_GEN6 32

/* The 64-bit register value for a tiled object at `start' of `size' bytes
 * (both page aligned) with the given stride; 0 (an invalid register) when
 * the parameters cannot be expressed. */
static inline uint64_t i915_fence_value(uint64_t start, uint64_t size,
					uint32_t stride, int tiling_y)
{
	if (!size || (start & 0xfff) || (size & 0xfff) || start + size > 0xffffffffULL)
		return 0;
	if (stride < 128 || (stride % 128) || stride > I915_FENCE_MAX_STRIDE)
		return 0;
	uint64_t last = (start + size - 4096) & 0xfffff000ULL;
	uint64_t pitch = (uint64_t)(stride / 128 - 1) & 0xfff;
	uint64_t val = (last << 32) | (pitch << 32) | (start & 0xfffff000ULL);
	val |= I915_FENCE_REG_VALID;
	if (tiling_y)
		val |= I915_FENCE_TILING_Y;
	return val;
}

/* Where byte (x, y) of a linear image with `stride' bytes per row lands
 * in memory when the object is tiled: X tiles are 512 bytes by 8 rows,
 * Y tiles 128 bytes by 32 rows with 16-byte columns.  What the aperture
 * translates for the processor, and what a test can check against a
 * plain read of the object's pages. */
static inline uint64_t i915_tiled_offset(uint32_t x, uint32_t y, uint32_t stride,
					 int tiling_y)
{
	if (tiling_y) {
		uint32_t tiles_per_row = stride / 128;
		uint64_t tile = (uint64_t)(y / 32) * tiles_per_row + x / 128;
		uint32_t tx = x % 128, ty = y % 32;
		return tile * 4096 + (tx / 16) * 512 + ty * 16 + (tx % 16);
	}
	uint32_t tiles_per_row = stride / 512;
	uint64_t tile = (uint64_t)(y / 8) * tiles_per_row + x / 512;
	return tile * 4096 + (y % 8) * 512 + (x % 512);
}

#endif
