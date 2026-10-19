// LikeOS -- the null render state (pure: fixed-width types only, so
// the host tests compile it).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_RENDERSTATE_H
#define KERNEL_DEV_GPU_I915_RENDERSTATE_H

#include <kernel/uapi/types.h>

/* The size of the Gen9 batch in bytes, commands and the state they
 * point at together (one page holds it). */
uint32_t i915_renderstate_gen9_bytes(void);
/* Write the Gen9 batch into `page', relocated to run at `base' (page
 * aligned, in the address space it will run in).  0 on success. */
int i915_renderstate_gen9_build(uint32_t *page, uint32_t page_bytes, uint64_t base);

#endif
