// LikeOS -- display-manager core: the layout of EDID-Like Data.
//
// The ELD is the summary of a sink an HDMI or DisplayPort audio codec is
// handed: a 4-byte header, then a baseline block with the CEA revision,
// the monitor name, the connection type, the speaker allocation, the
// manufacturer and product codes and up to 15 short audio descriptors.
// drm_edid_to_eld() (drm_eld.c) builds it from the EDID; the accessors
// here read it back.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_ELD_H
#define KERNEL_DEV_GPU_DRM_ELD_H

#include <kernel/uapi/types.h>

struct cea_sad;

/* ELD Header Block */
#define DRM_ELD_HEADER_BLOCK_SIZE 4

#define DRM_ELD_VER 0
#define DRM_ELD_VER_SHIFT 3
#define DRM_ELD_VER_MASK (0x1f << 3)
#define DRM_ELD_VER_CEA861D (2 << 3) /* supports 861D or below */
#define DRM_ELD_VER_CANNED (0x1f << 3)

#define DRM_ELD_BASELINE_ELD_LEN 2 /* in dwords! */

/* ELD Baseline Block for ELD_Ver == 2 */
#define DRM_ELD_CEA_EDID_VER_MNL 4
#define DRM_ELD_CEA_EDID_VER_SHIFT 5
#define DRM_ELD_CEA_EDID_VER_MASK (7 << 5)
#define DRM_ELD_CEA_EDID_VER_NONE (0 << 5)
#define DRM_ELD_CEA_EDID_VER_CEA861 (1 << 5)
#define DRM_ELD_CEA_EDID_VER_CEA861A (2 << 5)
#define DRM_ELD_CEA_EDID_VER_CEA861BCD (3 << 5)
#define DRM_ELD_MNL_SHIFT 0
#define DRM_ELD_MNL_MASK (0x1f << 0)

#define DRM_ELD_SAD_COUNT_CONN_TYPE 5
#define DRM_ELD_SAD_COUNT_SHIFT 4
#define DRM_ELD_SAD_COUNT_MASK (0xf << 4)
#define DRM_ELD_CONN_TYPE_SHIFT 2
#define DRM_ELD_CONN_TYPE_MASK (3 << 2)
#define DRM_ELD_CONN_TYPE_HDMI (0 << 2)
#define DRM_ELD_CONN_TYPE_DP (1 << 2)
#define DRM_ELD_SUPPORTS_AI (1 << 1)
#define DRM_ELD_SUPPORTS_HDCP (1 << 0)

#define DRM_ELD_AUD_SYNCH_DELAY 6 /* in units of 2 ms */
#define DRM_ELD_AUD_SYNCH_DELAY_MAX 0xfa /* 500 ms */

#define DRM_ELD_SPEAKER 7
#define DRM_ELD_SPEAKER_MASK 0x7f
#define DRM_ELD_SPEAKER_RLRC (1 << 6)
#define DRM_ELD_SPEAKER_FLRC (1 << 5)
#define DRM_ELD_SPEAKER_RC (1 << 4)
#define DRM_ELD_SPEAKER_RLR (1 << 3)
#define DRM_ELD_SPEAKER_FC (1 << 2)
#define DRM_ELD_SPEAKER_LFE (1 << 1)
#define DRM_ELD_SPEAKER_FLR (1 << 0)

#define DRM_ELD_PORT_ID 8 /* offsets 8..15 inclusive */
#define DRM_ELD_PORT_ID_LEN 8

#define DRM_ELD_MANUFACTURER_NAME0 16
#define DRM_ELD_MANUFACTURER_NAME1 17

#define DRM_ELD_PRODUCT_CODE0 18
#define DRM_ELD_PRODUCT_CODE1 19

#define DRM_ELD_MONITOR_NAME_STRING 20 /* offsets 20..(20+mnl-1) inclusive */

#define DRM_ELD_CEA_SAD(mnl, sad) (20 + (mnl) + 3 * (sad))

/* The monitor name length in bytes. */
static inline int drm_eld_mnl(const uint8_t *eld)
{
	return (eld[DRM_ELD_CEA_EDID_VER_MNL] & DRM_ELD_MNL_MASK) >> DRM_ELD_MNL_SHIFT;
}

/* SAD `sad_index' to / from the ELD; 0, or -EINVAL past the SAD count. */
int drm_eld_sad_get(const uint8_t *eld, int sad_index, struct cea_sad *cta_sad);
int drm_eld_sad_set(uint8_t *eld, int sad_index, const struct cea_sad *cta_sad);

/* The first short audio descriptor, NULL for an ELD version or name
 * length this layout does not describe. */
static inline const uint8_t *drm_eld_sad(const uint8_t *eld)
{
	unsigned int ver, mnl;

	ver = (eld[DRM_ELD_VER] & DRM_ELD_VER_MASK) >> DRM_ELD_VER_SHIFT;
	if (ver != 2 && ver != 31)
		return NULL;

	mnl = (unsigned int)drm_eld_mnl(eld);
	if (mnl > 16)
		return NULL;

	return eld + DRM_ELD_CEA_SAD(mnl, 0);
}

/* The number of short audio descriptors. */
static inline int drm_eld_sad_count(const uint8_t *eld)
{
	return (eld[DRM_ELD_SAD_COUNT_CONN_TYPE] & DRM_ELD_SAD_COUNT_MASK) >>
	       DRM_ELD_SAD_COUNT_SHIFT;
}

/* The baseline block's payload size in bytes (for Baseline_ELD_Len). */
static inline int drm_eld_calc_baseline_block_size(const uint8_t *eld)
{
	return DRM_ELD_MONITOR_NAME_STRING - DRM_ELD_HEADER_BLOCK_SIZE +
	       drm_eld_mnl(eld) + drm_eld_sad_count(eld) * 3;
}

/* Header plus baseline block, a multiple of 4 (the vendor block, if any,
 * follows and is not counted). */
static inline int drm_eld_size(const uint8_t *eld)
{
	return DRM_ELD_HEADER_BLOCK_SIZE + eld[DRM_ELD_BASELINE_ELD_LEN] * 4;
}

/* The speaker mask (DRM_ELD_SPEAKER_*). */
static inline uint8_t drm_eld_get_spk_alloc(const uint8_t *eld)
{
	return eld[DRM_ELD_SPEAKER] & DRM_ELD_SPEAKER_MASK;
}

/* DRM_ELD_CONN_TYPE_HDMI or DRM_ELD_CONN_TYPE_DP. */
static inline uint8_t drm_eld_get_conn_type(const uint8_t *eld)
{
	return eld[DRM_ELD_SAD_COUNT_CONN_TYPE] & DRM_ELD_CONN_TYPE_MASK;
}

#endif
