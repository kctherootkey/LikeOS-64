// LikeOS -- VESA Display Stream Compression helpers.
//
// The pure half of DSC support: the DP PPS packet header, the PPS payload
// packing, the rate-control buffer thresholds and the recommended RC
// parameter sets of the DSC 1.1 and 1.2 specifications, and the derived
// rate-control values (chunk size, offsets, scale intervals, decoder
// delay) every encoder computes the same way.  Integer arithmetic only;
// nothing here touches hardware, allocates or sleeps, so the host tests
// compile it as is.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2018 Intel Corp.

#ifndef KERNEL_DEV_GPU_DRM_DSC_HELPER_H
#define KERNEL_DEV_GPU_DRM_DSC_HELPER_H

#include <kernel/dev/gpu/drm_dsc.h>

enum drm_dsc_params_type {
	DRM_DSC_1_2_444,
	DRM_DSC_1_1_PRE_SCR, /* legacy params from DSC 1.1 */
	DRM_DSC_1_2_422,
	DRM_DSC_1_2_420,
};

void drm_dsc_dp_pps_header_init(struct dp_sdp_header *pps_header);
int drm_dsc_dp_rc_buffer_size(u8 rc_buffer_block_size, u8 rc_buffer_size);
void drm_dsc_pps_payload_pack(struct drm_dsc_picture_parameter_set *pps_sdp,
			      const struct drm_dsc_config *dsc_cfg);
void drm_dsc_set_const_params(struct drm_dsc_config *vdsc_cfg);
void drm_dsc_set_rc_buf_thresh(struct drm_dsc_config *vdsc_cfg);
int drm_dsc_setup_rc_params(struct drm_dsc_config *vdsc_cfg, enum drm_dsc_params_type type);
int drm_dsc_compute_rc_parameters(struct drm_dsc_config *vdsc_cfg);
u8 drm_dsc_initial_scale_value(const struct drm_dsc_config *dsc);
u32 drm_dsc_flatness_det_thresh(const struct drm_dsc_config *dsc);
u32 drm_dsc_get_bpp_int(const struct drm_dsc_config *vdsc_cfg);

/*
 * Log a configuration through kprintf, every line starting with @tag
 * (a connector or pipe name).  For bring-up and the modeset log only.
 */
void drm_dsc_dump_config(const char *tag, const struct drm_dsc_config *cfg);

#endif /* KERNEL_DEV_GPU_DRM_DSC_HELPER_H */
