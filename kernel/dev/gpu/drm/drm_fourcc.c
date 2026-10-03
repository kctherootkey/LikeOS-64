// LikeOS -- display-manager core: the pixel format table.
//
// The description of every four-character format code (planes, bytes per
// pixel or per block, subsampling, alpha, YUV) and the lookups the
// framebuffer checks use: a format by code, the legacy ADDFB's bpp/depth
// to a format, block sizes, bits per pixel and the minimum pitch of a
// plane.
//
// Pure: no device, no allocation, no locks -- the host test
// (host/test-fourcc.sh) compiles this file as it is.  The lookups that
// ask the device (drm_get_format_info, drm_driver_legacy_fb_format,
// drm_driver_color_mode_format) live with the framebuffer code in
// drm_framebuffer.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Laurent Pinchart's code: HPND-sell-variant
// Portions Copyright (C) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>

#include <kernel/dev/gpu/drm_fourcc.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>

/* The format code the legacy ADDFB means by bits per pixel and colour
 * depth; DRM_FORMAT_INVALID for a pair that names none. */
uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth)
{
	uint32_t fmt = DRM_FORMAT_INVALID;

	switch (bpp) {
	case 1:
		if (depth == 1)
			fmt = DRM_FORMAT_C1;
		break;

	case 2:
		if (depth == 2)
			fmt = DRM_FORMAT_C2;
		break;

	case 4:
		if (depth == 4)
			fmt = DRM_FORMAT_C4;
		break;

	case 8:
		if (depth == 8)
			fmt = DRM_FORMAT_C8;
		break;

	case 16:
		switch (depth) {
		case 15:
			fmt = DRM_FORMAT_XRGB1555;
			break;
		case 16:
			fmt = DRM_FORMAT_RGB565;
			break;
		default:
			break;
		}
		break;

	case 24:
		if (depth == 24)
			fmt = DRM_FORMAT_RGB888;
		break;

	case 32:
		switch (depth) {
		case 24:
			fmt = DRM_FORMAT_XRGB8888;
			break;
		case 30:
			fmt = DRM_FORMAT_XRGB2101010;
			break;
		case 32:
			fmt = DRM_FORMAT_ARGB8888;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}

	return fmt;
}

/*
 * Every format the core knows.  cpp and char_per_block share storage:
 * a plain format gives bytes per pixel (block 1x1), a block format bytes
 * per block_w x block_h block.  A format that only exists in non-linear
 * layouts has 0 bytes here; its pitch cannot be checked from the table.
 * The processor is little endian, so the DRM_FORMAT_BIG_ENDIAN variants
 * have no entries.
 */
static const struct drm_format_info drm_formats[] = {
	{ .format = DRM_FORMAT_C1,		.depth = 1,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
	{ .format = DRM_FORMAT_C2,		.depth = 2,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
	{ .format = DRM_FORMAT_C4,		.depth = 4,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
	{ .format = DRM_FORMAT_C8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
	{ .format = DRM_FORMAT_D1,		.depth = 1,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_D2,		.depth = 2,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_D4,		.depth = 4,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_D8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R1,		.depth = 1,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R2,		.depth = 2,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R4,		.depth = 4,  .num_planes = 1,
	  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R10,		.depth = 10, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_R12,		.depth = 12, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGB332,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGR233,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XRGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGBX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGRX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_ARGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGBA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGRA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XRGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGBX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGRX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_ARGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGBA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGRA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGB565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGR565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGB888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGR888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XRGB8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGBX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGRX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGB565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGR565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XRGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_RGBX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_BGRX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_ARGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGBA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGRA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGB161616,	.depth = 0,
	  .num_planes = 1, .char_per_block = { 6, 0, 0 },
	  .block_w = { 1, 0, 0 }, .block_h = { 1, 0, 0 },
	  .hsub = 1, .vsub = 1, .has_alpha = false },
	{ .format = DRM_FORMAT_BGR161616,	.depth = 0,
	  .num_planes = 1, .char_per_block = { 6, 0, 0 },
	  .block_w = { 1, 0, 0 }, .block_h = { 1, 0, 0 },
	  .hsub = 1, .vsub = 1, .has_alpha = false },
	{ .format = DRM_FORMAT_ARGB8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGBA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGRA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XRGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_ARGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_AXBXGXRX106106106106, .depth = 0, .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XRGB16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_XBGR16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
	{ .format = DRM_FORMAT_ARGB16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_ABGR16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGB888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGR888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XRGB8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_XBGR8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_RGBX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_BGRX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
	{ .format = DRM_FORMAT_YUV410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
	{ .format = DRM_FORMAT_YVU410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
	{ .format = DRM_FORMAT_YUV411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YVU411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YUV420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_YVU420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_YUV422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YVU422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YUV444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YVU444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Y8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_NV12,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_NV21,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_NV16,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_NV61,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_NV24,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_NV42,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YUYV,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_YVYU,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_UYVY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_VYUY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_XYUV8888,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_VUY888,          .depth = 0,  .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_XVUY2101010,     .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_AYUV,		.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_Y210,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Y212,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Y216,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Y410,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_Y412,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_Y416,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_XVYU2101010,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_XVYU12_16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_XVYU16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Y0L0,		.depth = 0,  .num_planes = 1,
	  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
	  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_X0L0,		.depth = 0,  .num_planes = 1,
	  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
	  .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_Y0L2,		.depth = 0,  .num_planes = 1,
	  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
	  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
	{ .format = DRM_FORMAT_X0L2,		.depth = 0,  .num_planes = 1,
	  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
	  .hsub = 2, .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_P010,            .depth = 0,  .num_planes = 2,
	  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_P012,		.depth = 0,  .num_planes = 2,
	  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
	   .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_P016,		.depth = 0,  .num_planes = 2,
	  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_P210,		.depth = 0,
	  .num_planes = 2, .char_per_block = { 2, 4, 0 },
	  .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
	  .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_VUY101010,	.depth = 0,
	  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 1, .vsub = 1,
	  .is_yuv = true },
	{ .format = DRM_FORMAT_YUV420_8BIT,     .depth = 0,
	  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
	  .is_yuv = true },
	{ .format = DRM_FORMAT_YUV420_10BIT,    .depth = 0,
	  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
	  .is_yuv = true },
	{ .format = DRM_FORMAT_NV15,		.depth = 0,
	  .num_planes = 2, .char_per_block = { 5, 5, 0 },
	  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
	  .vsub = 2, .is_yuv = true },
	{ .format = DRM_FORMAT_NV20,		.depth = 0,
	  .num_planes = 2, .char_per_block = { 5, 5, 0 },
	  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
	  .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_NV30,		.depth = 0,
	  .num_planes = 2, .char_per_block = { 5, 5, 0 },
	  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 1,
	  .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Q410,		.depth = 0,
	  .num_planes = 3, .char_per_block = { 2, 2, 2 },
	  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 1,
	  .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_Q401,		.depth = 0,
	  .num_planes = 3, .char_per_block = { 2, 2, 2 },
	  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 1,
	  .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_P030,            .depth = 0,  .num_planes = 2,
	  .char_per_block = { 4, 8, 0 }, .block_w = { 3, 3, 0 }, .block_h = { 1, 1, 0 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_P230,		.depth = 0,  .num_planes = 2,
	  .char_per_block = { 4, 8, 0 }, .block_w = { 3, 3, 0 }, .block_h = { 1, 1, 0 },
	  .hsub = 2, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_S010,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_S210,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_S410,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 1, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_S012,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_S212,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_S412,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 1, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_S016,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 2, .is_yuv = true},
	{ .format = DRM_FORMAT_S216,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 2, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_S416,            .depth = 0,  .num_planes = 3,
	  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
	  .hsub = 1, .vsub = 1, .is_yuv = true},
	{ .format = DRM_FORMAT_XYYY2101010,	.depth = 0,  .num_planes = 1,
	  .char_per_block = { 4, 0, 0 }, .block_w = { 3, 0, 0 }, .block_h = { 1, 0, 0 },
	  .hsub = 1, .vsub = 1, .is_yuv = true },
	{ .format = DRM_FORMAT_T430,		.depth = 0,  .num_planes = 3,
	  .char_per_block = { 4, 4, 4 }, .block_w = { 3, 3, 3 }, .block_h = { 1, 1, 1 },
	  .hsub = 1, .vsub = 1, .is_yuv = true },
};

/* The description of `format', NULL for a code the table does not know.
 * See drm_format_info() for the variant that warns. */
const struct drm_format_info *__drm_format_info(uint32_t format)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(drm_formats); ++i) {
		if (drm_formats[i].format == format)
			return &drm_formats[i];
	}

	return NULL;
}

/* The same, for callers that only pass formats they know exist: an unknown
 * one is a bug in the caller and is reported. */
const struct drm_format_info *drm_format_info(uint32_t format)
{
	const struct drm_format_info *info;

	info = __drm_format_info(format);
	WARN_ON(!info);
	return info;
}

/* Width in pixels of a block of `plane'; 0 for a plane the format does
 * not have. */
unsigned int drm_format_info_block_width(const struct drm_format_info *info,
					 int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_w[plane])
		return 1;
	return info->block_w[plane];
}

/* Height in pixels of a block of `plane'; 0 for a plane the format does
 * not have. */
unsigned int drm_format_info_block_height(const struct drm_format_info *info,
					  int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_h[plane])
		return 1;
	return info->block_h[plane];
}

/* Bits per pixel of `plane'.  A block format whose block does not divide
 * into whole bits per pixel has no answer: 0, and a warning, since the
 * caller wanted a per-pixel figure for a format that has none. */
unsigned int drm_format_info_bpp(const struct drm_format_info *info, int plane)
{
	unsigned int block_size;

	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	block_size = drm_format_info_block_width(info, plane) *
		     drm_format_info_block_height(info, plane);

	if (info->char_per_block[plane] * 8 % block_size) {
		WARN_RATELIMIT(1, "format %08x plane %d: no whole number of bits per pixel",
			       info->format, plane);
		return 0;
	}

	return info->char_per_block[plane] * 8 / block_size;
}

/* The fewest bytes a line of `buffer_width' pixels of `plane' takes: the
 * blocks it covers, rounded up, over the block's height (a block spans
 * that many lines). */
uint64_t drm_format_info_min_pitch(const struct drm_format_info *info,
				   int plane, unsigned int buffer_width)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	return DIV_ROUND_UP_ULL((uint64_t)buffer_width * info->char_per_block[plane],
				drm_format_info_block_width(info, plane) *
				drm_format_info_block_height(info, plane));
}
