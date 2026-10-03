// LikeOS -- DSC rate-control QP tables and the parameters derived from them.
//
// The display engine's DSC encoder takes its per-range minimum and
// maximum quantisation parameters from tables indexed by component depth,
// RC range and compressed bpp (in steps of half a bit per pixel), and
// interpolates the range BPG offsets between the recommended sets; see
// intel_qp_tables.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2021 Intel Corporation
// Portions Copyright (C) 2018 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_QP_TABLES_H
#define KERNEL_DEV_GPU_I915_INTEL_QP_TABLES_H

#include <kernel/dev/gpu/drm_dsc.h>

u8 intel_lookup_range_min_qp(int bpc, int buf_i, int bpp_i, bool is_420);
u8 intel_lookup_range_max_qp(int bpc, int buf_i, int bpp_i, bool is_420);

/*
 * Fill the rate-control parameters of @vdsc_cfg from the QP tables and the
 * DSC 1.2a model's formulas: first line / second line BPG offsets,
 * initial offset and transmit delay, flatness and quantisation limits,
 * and per range the min/max QP and the interpolated BPG offset.
 * bits_per_pixel (x16; doubled for native 4:2:0), bits_per_component,
 * slice_height, convert_rgb and native_420 must be set.  This is the
 * path display version 13 and later use for every compressed bpp.
 */
void intel_qp_calculate_rc_params(struct drm_dsc_config *vdsc_cfg);

#endif /* KERNEL_DEV_GPU_I915_INTEL_QP_TABLES_H */
