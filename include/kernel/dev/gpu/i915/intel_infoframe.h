// LikeOS -- HDMI infoframes (pure; see intel_infoframe.c).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_INFOFRAME_H
#define KERNEL_DEV_GPU_I915_INTEL_INFOFRAME_H

#include <kernel/uapi/drm/drm_mode.h>

#define INTEL_AVI_INFOFRAME_SIZE 17 /* header + 13 payload bytes */

/* The AVI infoframe for a mode: RGB, 8 bpc, the mode's picture aspect,
 * the CEA video code `vic' (0 for a non-CEA timing).  Returns the byte
 * count or -1 when `out' is too small. */
int intel_hdmi_avi_infoframe(const struct drm_mode_modeinfo *m, uint8_t vic,
			     uint8_t *out, unsigned outlen);
/* The CEA video code of a mode, 0 when it is not a CEA timing. */
uint8_t intel_hdmi_vic_for_mode(const struct drm_mode_modeinfo *m);

#endif
