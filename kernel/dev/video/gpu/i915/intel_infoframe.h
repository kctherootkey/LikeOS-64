// LikeOS -- HDMI infoframes (pure; see intel_infoframe.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2006-2009 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_INFOFRAME_H
#define KERNEL_DEV_GPU_I915_INTEL_INFOFRAME_H

#include <kernel/uapi/drm/drm_mode.h>

struct drm_connector;
union hdmi_infoframe;

#define INTEL_AVI_INFOFRAME_SIZE 17 /* header + 13 payload bytes */

/* The AVI infoframe for a mode: RGB, 8 bpc, the mode's picture aspect,
 * the CEA video code `vic' (0 for a non-CEA timing).  Returns the byte
 * count or -1 when `out' is too small.  (The driver's own packer, used
 * when the infoframe library is switched off.) */
int intel_hdmi_avi_infoframe(const struct drm_mode_modeinfo *m, uint8_t vic,
			     uint8_t *out, unsigned outlen);
/* The CEA video code of a mode, 0 when it is not a CEA timing. */
uint8_t intel_hdmi_vic_for_mode(const struct drm_mode_modeinfo *m);

/* ---- the transmitter's form, built by the infoframe library ------------- */

/* A data island packet buffer of the transmitter (the DIP data
 * registers): 32 bytes, the header's three bytes, then a byte the
 * hardware fills in (ECC, or a DisplayPort header byte), then the
 * payload from the checksum on. */
#define INTEL_DIP_DATA_SIZE 32

/* The packets an HDMI sink is sent with a mode: each in the transmitter's
 * form with its length (0: not sent), and the general control packet's
 * register value. */
struct intel_hdmi_infoframes {
	uint8_t avi[INTEL_DIP_DATA_SIZE];
	int avi_len;
	uint8_t spd[INTEL_DIP_DATA_SIZE];
	int spd_len;
	uint8_t vendor[INTEL_DIP_DATA_SIZE];
	int vendor_len;
	int gcp_enable; /* send general control packets */
	uint32_t gcp; /* HSW_TVIDEO_DIP_GCP value */
};

/* Pack infoframe `f' into the transmitter's form: the header bytes, the
 * hole at byte 3, the rest.  The byte count, or a negative errno. */
int intel_hdmi_infoframe_to_dip(const union hdmi_infoframe *f, uint8_t *dip,
				unsigned size);

/* The AVI (RGB, full range, the mode's codes and aspect), source product
 * description ("Intel", integrated or discrete graphics) and, for a sink
 * with the HDMI vendor block, HDMI vendor infoframes of mode `m' for the
 * sink on connector `c' (its EDID's display info; may be NULL), and the
 * general control packet at 8 bits per component.  0, or a negative
 * errno when the AVI infoframe cannot describe the mode. */
int intel_hdmi_infoframes_compute(const struct drm_connector *c,
				  const struct drm_mode_modeinfo *m, int discrete,
				  struct intel_hdmi_infoframes *out);

#endif
