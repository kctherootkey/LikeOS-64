// LikeOS -- the bits of the universal plane registers (Skylake and later)
// that the plane code programs beyond the surface itself: rotation and
// horizontal flip, the alpha mode, the YCbCr conversion selectors, the
// constant-alpha fields of the colour-key registers and the field
// layouts of the position, size, offset and stride registers.  Also the
// two bits of the older primary plane (DSPCNTR, Haswell and Broadwell)
// and the cursor that rotate them.
//
// The register offsets themselves (PLANE_CTL, PLANE_COLOR_CTL,
// PLANE_KEY*, CUR_CTL, DSPCNTR, ...) are in i915_reg.h; include that
// first.  Every name here is guarded, so a definition that i915_reg.h
// already carries (with the same value) wins without a clash.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2024 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_SKL_UNIVERSAL_PLANE_REGS_H
#define KERNEL_DEV_GPU_I915_SKL_UNIVERSAL_PLANE_REGS_H

/* ---- PLANE_CTL -------------------------------------------------------------- */

#ifndef PLANE_CTL_YUV_RANGE_CORRECTION_DISABLE
#define PLANE_CTL_YUV_RANGE_CORRECTION_DISABLE (1u << 28)
#endif
#ifndef PLANE_CTL_KEY_ENABLE_SOURCE
#define PLANE_CTL_KEY_ENABLE_SOURCE (1u << 21)
#endif
#ifndef PLANE_CTL_KEY_ENABLE_DESTINATION
#define PLANE_CTL_KEY_ENABLE_DESTINATION (2u << 21)
#endif
/* BT.709 instead of BT.601 for the YCbCr to RGB conversion (before
 * display version 10; PLANE_COLOR_CTL has the selector from then on) */
#ifndef PLANE_CTL_YUV_TO_RGB_CSC_FORMAT_BT709
#define PLANE_CTL_YUV_TO_RGB_CSC_FORMAT_BT709 (1u << 18)
#endif
/* Tile4 reuses the old Yf encoding */
#ifndef PLANE_CTL_TILED_4
#define PLANE_CTL_TILED_4 (5u << 10)
#endif
#ifndef PLANE_CTL_TILED_YF
#define PLANE_CTL_TILED_YF (5u << 10)
#endif
/* display version 11 on: mirror the surface left to right */
#ifndef PLANE_CTL_FLIP_HORIZONTAL
#define PLANE_CTL_FLIP_HORIZONTAL (1u << 8)
#endif
/* The rotation field counts clockwise; the plane property's rotations
 * count counter-clockwise (skl_plane_ctl_rotate() swaps 90 and 270). */
#ifndef PLANE_CTL_ROTATE_90
#define PLANE_CTL_ROTATE_90 0x1u
#endif
#ifndef PLANE_CTL_ROTATE_270
#define PLANE_CTL_ROTATE_270 0x3u
#endif

/* ---- PLANE_COLOR_CTL (display version 10 on) ---------------------------------- */

#ifndef PLANE_COLOR_YUV_RANGE_CORRECTION_DISABLE
#define PLANE_COLOR_YUV_RANGE_CORRECTION_DISABLE (1u << 28)
#endif
#ifndef PLANE_COLOR_PLANE_CSC_ENABLE
#define PLANE_COLOR_PLANE_CSC_ENABLE (1u << 21) /* version 11 on */
#endif
#ifndef PLANE_COLOR_INPUT_CSC_ENABLE
#define PLANE_COLOR_INPUT_CSC_ENABLE (1u << 20) /* version 11 on */
#endif
#ifndef PLANE_COLOR_CSC_MODE_MASK
#define PLANE_COLOR_CSC_MODE_MASK (7u << 17)
#endif
#ifndef PLANE_COLOR_CSC_MODE_BYPASS
#define PLANE_COLOR_CSC_MODE_BYPASS (0u << 17)
#endif
#ifndef PLANE_COLOR_CSC_MODE_YUV601_TO_RGB601
#define PLANE_COLOR_CSC_MODE_YUV601_TO_RGB601 (1u << 17)
#endif
#ifndef PLANE_COLOR_CSC_MODE_YUV709_TO_RGB709
#define PLANE_COLOR_CSC_MODE_YUV709_TO_RGB709 (2u << 17)
#endif
#ifndef PLANE_COLOR_CSC_MODE_YUV2020_TO_RGB2020
#define PLANE_COLOR_CSC_MODE_YUV2020_TO_RGB2020 (3u << 17)
#endif
#ifndef PLANE_COLOR_ALPHA_MASK
#define PLANE_COLOR_ALPHA_MASK (3u << 4)
#endif
#ifndef PLANE_COLOR_ALPHA_SW_PREMULTIPLY
#define PLANE_COLOR_ALPHA_SW_PREMULTIPLY (2u << 4)
#endif
#ifndef PLANE_COLOR_ALPHA_HW_PREMULTIPLY
#define PLANE_COLOR_ALPHA_HW_PREMULTIPLY (3u << 4)
#endif

/* ---- the colour key registers, used for the plane's constant alpha ------------ */

/* PLANE_KEYMSK: blend with the constant alpha of PLANE_KEYMAX */
#ifndef PLANE_KEYMSK_ALPHA_ENABLE
#define PLANE_KEYMSK_ALPHA_ENABLE (1u << 31)
#endif
#ifndef PLANE_KEYMAX_ALPHA
#define PLANE_KEYMAX_ALPHA(a) (((uint32_t)(a) & 0xffu) << 24)
#endif

/* ---- position, size, offset, stride ------------------------------------------- */

#ifndef PLANE_POS_Y
#define PLANE_POS_Y(y) (((uint32_t)(y) & 0xffffu) << 16)
#endif
#ifndef PLANE_POS_X
#define PLANE_POS_X(x) ((uint32_t)(x) & 0xffffu)
#endif
#ifndef PLANE_HEIGHT
#define PLANE_HEIGHT(h) (((uint32_t)(h) & 0xffffu) << 16)
#endif
#ifndef PLANE_WIDTH
#define PLANE_WIDTH(w) ((uint32_t)(w) & 0xffffu)
#endif
#ifndef PLANE_OFFSET_Y
#define PLANE_OFFSET_Y(y) (((uint32_t)(y) & 0xffffu) << 16)
#endif
#ifndef PLANE_OFFSET_X
#define PLANE_OFFSET_X(x) ((uint32_t)(x) & 0xffffu)
#endif
#ifndef PLANE_STRIDE_
#define PLANE_STRIDE_(s) ((uint32_t)(s) & 0xfffu)
#endif

/* ---- the older primary plane and the cursor ---------------------------------------- */

#ifndef DISP_ROTATE_180
#define DISP_ROTATE_180 (1u << 15)
#endif
/* Cherryview pipe B only; never set on the parts this code drives */
#ifndef DISP_MIRROR
#define DISP_MIRROR (1u << 8)
#endif
#ifndef MCURSOR_ROTATE_180
#define MCURSOR_ROTATE_180 (1u << 15)
#endif

#endif
