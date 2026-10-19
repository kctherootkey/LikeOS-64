// LikeOS -- display-manager core: what a pixel format looks like in memory.
//
// One struct drm_format_info per four-character format code: how many
// planes it has, how many bytes a pixel (or a block of pixels) takes in
// each, the chroma subsampling, whether it has alpha, whether it is YUV.
// The framebuffer checks use it to validate pitches and sizes plane by
// plane.  The table and the pure lookups are drm_fourcc.c's; the lookups
// that ask the device (drm_get_format_info, the driver variants of the
// legacy format) are drm_framebuffer.c's.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Laurent Pinchart's code: HPND-sell-variant
// Portions Copyright (C) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>

#ifndef KERNEL_DEV_GPU_DRM_FOURCC_H
#define KERNEL_DEV_GPU_DRM_FOURCC_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_fourcc.h>

/* The most planes a format has. */
#define DRM_FORMAT_MAX_PLANES 4u

/* The formats in the processor's byte order (little endian here). */
#define DRM_FORMAT_HOST_XRGB1555 DRM_FORMAT_XRGB1555
#define DRM_FORMAT_HOST_RGB565 DRM_FORMAT_RGB565
#define DRM_FORMAT_HOST_XRGB8888 DRM_FORMAT_XRGB8888
#define DRM_FORMAT_HOST_ARGB8888 DRM_FORMAT_ARGB8888

struct drm_device;

struct drm_format_info {
	/* the 4CC code, DRM_FORMAT_* */
	uint32_t format;
	/* colour depth (bits excluding padding) for the formats the legacy
	 * ADDFB can name, 0 otherwise */
	uint8_t depth;
	uint8_t num_planes;
	union {
		/* bytes per pixel, per plane (formats with 1x1 blocks) */
		uint8_t cpp[DRM_FORMAT_MAX_PLANES];
		/* bytes per block, per plane (block formats; cpp is then 0
		 * for code that does not know blocks) */
		uint8_t char_per_block[DRM_FORMAT_MAX_PLANES];
	};
	/* block size in pixels, per plane (0 means 1) */
	uint8_t block_w[DRM_FORMAT_MAX_PLANES];
	uint8_t block_h[DRM_FORMAT_MAX_PLANES];
	/* horizontal / vertical chroma subsampling factor */
	uint8_t hsub;
	uint8_t vsub;
	bool has_alpha;
	bool is_yuv;
	/* palette-indexed (C1, C2, C4, C8) */
	bool is_color_indexed;
};

static inline bool drm_format_info_is_yuv_packed(const struct drm_format_info *info)
{
	return info->is_yuv && info->num_planes == 1;
}

static inline bool drm_format_info_is_yuv_semiplanar(const struct drm_format_info *info)
{
	return info->is_yuv && info->num_planes == 2;
}

static inline bool drm_format_info_is_yuv_planar(const struct drm_format_info *info)
{
	return info->is_yuv && info->num_planes == 3;
}

static inline bool drm_format_info_is_yuv_sampling_410(const struct drm_format_info *info)
{
	return info->is_yuv && info->hsub == 4 && info->vsub == 4;
}

static inline bool drm_format_info_is_yuv_sampling_411(const struct drm_format_info *info)
{
	return info->is_yuv && info->hsub == 4 && info->vsub == 1;
}

static inline bool drm_format_info_is_yuv_sampling_420(const struct drm_format_info *info)
{
	return info->is_yuv && info->hsub == 2 && info->vsub == 2;
}

static inline bool drm_format_info_is_yuv_sampling_422(const struct drm_format_info *info)
{
	return info->is_yuv && info->hsub == 2 && info->vsub == 1;
}

static inline bool drm_format_info_is_yuv_sampling_444(const struct drm_format_info *info)
{
	return info->is_yuv && info->hsub == 1 && info->vsub == 1;
}

/* The width of `plane' for a framebuffer `width' pixels wide (chroma
 * planes are subsampled); 0 for a plane the format does not have. */
static inline int drm_format_info_plane_width(const struct drm_format_info *info,
					      int width, int plane)
{
	if (!info || plane >= info->num_planes)
		return 0;
	if (plane == 0)
		return width;
	return (width + info->hsub - 1) / info->hsub;
}

static inline int drm_format_info_plane_height(const struct drm_format_info *info,
					       int height, int plane)
{
	if (!info || plane >= info->num_planes)
		return 0;
	if (plane == 0)
		return height;
	return (height + info->vsub - 1) / info->vsub;
}

/* ---- drm_fourcc.c -------------------------------------------------------- */

/* The format's description, NULL for a format the table does not know. */
const struct drm_format_info *__drm_format_info(uint32_t format);
/* The same, warning about an unknown format (a caller bug). */
const struct drm_format_info *drm_format_info(uint32_t format);
/* The description a framebuffer of this format and layout modifier gets on
 * this device (a driver may describe its compressed layouts its own way,
 * drm_driver.get_format_info).  In drm_framebuffer.c, as are the two
 * drm_driver_* format lookups below. */
const struct drm_format_info *drm_get_format_info(struct drm_device *dev,
						  uint32_t pixel_format,
						  uint64_t modifier);
/* The format the legacy ADDFB means by bpp/depth, DRM_FORMAT_INVALID for
 * none; the driver variant prefers what the device can scan out. */
uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth);
uint32_t drm_driver_legacy_fb_format(struct drm_device *dev, uint32_t bpp,
				     uint32_t depth);
uint32_t drm_driver_color_mode_format(struct drm_device *dev,
				      unsigned int color_mode);
unsigned int drm_format_info_block_width(const struct drm_format_info *info,
					 int plane);
unsigned int drm_format_info_block_height(const struct drm_format_info *info,
					  int plane);
/* Bits per pixel of `plane' (block formats: averaged over the block). */
unsigned int drm_format_info_bpp(const struct drm_format_info *info, int plane);
/* The smallest pitch a line of buffer_width pixels needs in `plane'. */
uint64_t drm_format_info_min_pitch(const struct drm_format_info *info,
				   int plane, unsigned int buffer_width);

#endif
