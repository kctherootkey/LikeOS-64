// LikeOS -- vmwgfx: processor copies between buffer objects.
//
// The screen-target path sometimes has to move pixels with the processor:
// a client's buffer object into the driver's display surface, or the screen
// back into a buffer.  vmw_bo_cpu_blit() copies a rectangle between two
// buffer objects page by page; with the diff copier it also compares as it
// copies and reports the bounding box of what actually changed, so the
// update the device is then asked to make covers only that.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright 2017 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_BLIT_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_BLIT_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>

/**
 * struct vmw_diff_cpy - CPU blit information structure
 *
 * @rect: The output bounding box rectangle.
 * @line: The current line of the blit.
 * @line_offset: Offset of the current line segment.
 * @cpp: Bytes per pixel (granularity information).
 * @do_cpy: Which memcpy function to use.
 */
struct vmw_diff_cpy {
	struct drm_mode_rect_k rect;
	size_t line;
	size_t line_offset;
	int cpp;
	void (*do_cpy)(struct vmw_diff_cpy *diff, u8 *dest, const u8 *src,
		       size_t n);
};

#define VMW_CPU_BLIT_INITIALIZER {	\
	.do_cpy = vmw_memcpy,		\
}

#define VMW_CPU_BLIT_DIFF_INITIALIZER(_cpp) {	  \
	.rect = { .x1 = INT_MAX/2,		  \
		  .y1 = INT_MAX/2,		  \
		  .x2 = INT_MIN/2,		  \
		  .y2 = INT_MIN/2		  \
	},					  \
	.line = 0,				  \
	.line_offset = 0,			  \
	.cpp = _cpp,				  \
	.do_cpy = vmw_diff_memcpy,		  \
}

void vmw_diff_memcpy(struct vmw_diff_cpy *diff, u8 *dest, const u8 *src,
		     size_t n);

void vmw_memcpy(struct vmw_diff_cpy *diff, u8 *dest, const u8 *src, size_t n);

/* Copy a w x h byte rectangle (w in BYTES) from `src' at src_offset with
 * src_stride to `dst' at dst_offset with dst_stride.  Both objects must be
 * page-backed.  Returns 0 or -errno. */
int vmw_bo_cpu_blit(struct drm_gem_object *dst, u32 dst_offset, u32 dst_stride,
		    struct drm_gem_object *src, u32 src_offset, u32 src_stride,
		    u32 w, u32 h, struct vmw_diff_cpy *diff);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_BLIT_H */
