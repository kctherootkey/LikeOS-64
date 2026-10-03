// LikeOS -- the registers of the variable refresh timing generator of
// the Intel display transcoders (display version 11 and later).
//
// Each transcoder has its own block at the transcoder's base: the
// control word, the range (minimum, maximum and flip line, each one less
// than the number of lines it stands for), the live status, and the
// push register a flip uses to end the frame early.  Only what the
// driver programs is named here.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2024 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_VRR_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_VRR_REGS_H

#include <kernel/dev/gpu/i915/i915_reg.h>

#ifndef TRANS_VRR_CTL
#define TRANS_VRR_CTL(t) (TRANS_BASE(t) + 0x420)
#endif
#define VRR_CTL_VRR_ENABLE (1u << 31)
#define VRR_CTL_IGN_MAX_SHIFT (1u << 30)
#define VRR_CTL_FLIP_LINE_EN (1u << 29)
#define VRR_CTL_PIPELINE_FULL_MASK (0xffu << 3)
#define VRR_CTL_PIPELINE_FULL(x) (((uint32_t)(x) << 3) & VRR_CTL_PIPELINE_FULL_MASK)
#define VRR_CTL_PIPELINE_FULL_MAX 0xff
#define VRR_CTL_PIPELINE_FULL_OVERRIDE (1u << 0)
#define XELPD_VRR_CTL_VRR_GUARDBAND_MASK 0xffffu
#define XELPD_VRR_CTL_VRR_GUARDBAND(x) ((uint32_t)(x) & XELPD_VRR_CTL_VRR_GUARDBAND_MASK)
#define XELPD_VRR_CTL_VRR_GUARDBAND_MAX 0xffff

#ifndef TRANS_VRR_VMAX
#define TRANS_VRR_VMAX(t) (TRANS_BASE(t) + 0x424)
#endif
#define VRR_VMAX_MASK 0xfffffu

#ifndef TRANS_VRR_STATUS
#define TRANS_VRR_STATUS(t) (TRANS_BASE(t) + 0x42c)
#endif
#define VRR_STATUS_VRR_EN_LIVE (1u << 27)

#ifndef TRANS_VRR_VMIN
#define TRANS_VRR_VMIN(t) (TRANS_BASE(t) + 0x434)
#endif
#define VRR_VMIN_MASK 0xffffu

#ifndef TRANS_VRR_FLIPLINE
#define TRANS_VRR_FLIPLINE(t) (TRANS_BASE(t) + 0x438)
#endif
#define VRR_FLIPLINE_MASK 0xfffffu

#ifndef TRANS_PUSH
#define TRANS_PUSH(t) (TRANS_BASE(t) + 0xa70)
#endif
#define TRANS_PUSH_EN (1u << 31)
#define TRANS_PUSH_SEND (1u << 30)

/* The transcoder chicken bits of display versions 12 and 13 (Tiger Lake
 * to Alder Lake / DG2; those have no separate eDP transcoder, so index 3
 * is transcoder D).  PIPE_VBLANK_WITH_DELAY: Tiger Lake generates the
 * variable-refresh "safe window"; Alder Lake makes the set context
 * latency count with variable refresh. */
#define VRR_CHICKEN_TRANS(t) ((t) == 3 ? 0x420d8u : 0x420c0u + (uint32_t)(t) * 4)
#define VRR_PIPE_VBLANK_WITH_DELAY (1u << 31)

#endif
