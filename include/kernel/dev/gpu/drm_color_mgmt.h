// LikeOS -- display-manager core: colour management.
//
// A crtc's pixels pass, after the planes are blended, through a degamma
// lookup table, a 3x3 colour transformation matrix and a gamma lookup
// table, each optional:
//   "DEGAMMA_LUT" / "GAMMA_LUT"  blobs of struct drm_color_lut (0: a
//                                linear table, which is how a crtc starts)
//   "DEGAMMA_LUT_SIZE" / "GAMMA_LUT_SIZE"  immutable: the most entries the
//                                hardware's table takes
//   "CTM"                        a blob of one struct drm_color_ctm (0: the
//                                identity)
// and a plane showing a YCbCr framebuffer may say how to convert it:
//   "COLOR_ENCODING"  BT.601 / BT.709 / BT.2020
//   "COLOR_RANGE"     limited / full range
//
// Who does what.  The device-wide properties are made by drm_kms_init
// (dev->prop_degamma_lut, prop_degamma_lut_size, prop_ctm for a driver with
// DRM_FEATURE_COLOR_MGMT; prop_gamma_lut and prop_gamma_lut_size for an
// atomic driver with a gamma table); drm_crtc_enable_color_mgmt() attaches
// the ones a crtc has.  The atomic core keeps the tables as referenced
// blobs in struct drm_crtc_state (degamma_lut, ctm, gamma_lut) and the
// committed ones in struct drm_crtc, and checks a table's length against
// the crtc's *_LUT_SIZE value.  A driver's check uses drm_color_lut_check()
// for what its hardware cannot take; its commit converts entries with
// drm_color_lut_extract() and the matrix with
// drm_color_ctm_s31_32_to_qm_n().  The plane properties are made per plane
// by drm_plane_create_color_properties(), which leaves the plane's
// color_encoding_property / color_range_property pointing at them and its
// committed color_encoding / color_range at the defaults.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_COLOR_MGMT_H
#define KERNEL_DEV_GPU_DRM_COLOR_MGMT_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/gpu_util.h>

/* A table entry (16 bits, 0xffff = 1.0) rounded to the hardware's
 * `bit_precision' bits, the way an integer colour is converted to and from
 * a normalised one: value * (2^bits - 1) / (2^16 - 1), to nearest. */
static inline u32 drm_color_lut_extract(u32 user_input, int bit_precision)
{
	if (bit_precision > 16)
		return (u32)DIV_ROUND_CLOSEST_ULL((u64)user_input *
						  (((u64)1 << bit_precision) - 1),
						  (1 << 16) - 1);
	else
		return DIV_ROUND_CLOSEST(user_input * ((1u << bit_precision) - 1),
					 (1u << 16) - 1);
}

/* The same for a 32-bit entry (0xffffffff = 1.0): U0.bit_precision from
 * U0.32. */
static inline u32 drm_color_lut32_extract(u32 user_input, int bit_precision)
{
	u64 max = (bit_precision >= 64) ? ~0ULL : (1ULL << bit_precision) - 1;

	return (u32)DIV_ROUND_CLOSEST_ULL((u64)user_input * max,
					  (1ULL << 32) - 1);
}

/* A CTM coefficient -- S31.32 sign-magnitude, as clients write it -- as a
 * two's complement Qm.n number (m integer bits including the sign, m <= 32;
 * n fraction bits, n <= 32), clamped to [-2^(m-1), 2^(m-1) - 2^-n].  m may
 * be 0 when every bit is a fraction bit (Q0.32). */
u64 drm_color_ctm_s31_32_to_qm_n(u64 user_input, u32 m, u32 n);

/* Entries in a lookup table blob. */
static inline int drm_color_lut_size(const struct drm_blob *blob)
{
	return (int)(blob->length / sizeof(struct drm_color_lut));
}

static inline const struct drm_color_lut *drm_color_lut_data(const struct drm_blob *blob)
{
	return (const struct drm_color_lut *)blob->data;
}

enum drm_color_encoding {
	DRM_COLOR_YCBCR_BT601,
	DRM_COLOR_YCBCR_BT709,
	DRM_COLOR_YCBCR_BT2020,
	DRM_COLOR_ENCODING_MAX,
};

enum drm_color_range {
	DRM_COLOR_YCBCR_LIMITED_RANGE,
	DRM_COLOR_YCBCR_FULL_RANGE,
	DRM_COLOR_RANGE_MAX,
};

/* What drm_color_lut_check() checks, as a mask. */
enum drm_color_lut_tests {
	/* Every entry has red == green == blue: for hardware with one value
	 * per entry that applies to all three channels. */
	DRM_COLOR_LUT_EQUAL_CHANNELS = BIT(0),
	/* No channel ever decreases from one entry to the next. */
	DRM_COLOR_LUT_NON_DECREASING = BIT(1),
};

/* 0 when the table (NULL: none) passes the tests in `tests', else
 * -EINVAL. */
int drm_color_lut_check(const struct drm_blob *lut, u32 tests);

/* The names the COLOR_ENCODING / COLOR_RANGE entries carry; "unknown" (and
 * a warning) out of range. */
const char *drm_get_color_encoding_name(enum drm_color_encoding encoding);
const char *drm_get_color_range_name(enum drm_color_range range);

/* ---- setting objects up (drm_color_mgmt.c) -------------------------------- */

/* Attach the colour management properties the crtc's hardware has:
 * DEGAMMA_LUT and DEGAMMA_LUT_SIZE (= degamma_lut_size) when that is not 0,
 * CTM when has_ctm, GAMMA_LUT and GAMMA_LUT_SIZE (= gamma_lut_size) when
 * that is not 0.  After drm_dev_register(), once per crtc.  The core's own
 * properties are attached to the crtc first; a property already attached
 * (GAMMA_LUT and its size, which the core attaches for an atomic driver
 * with drv->gamma_size) stays attached, a size taking this crtc's value
 * (a pipe whose table is shorter than the device's default).
 * 0; -EINVAL (warned about) when the device lacks a property asked for --
 * DEGAMMA_LUT and CTM need DRM_FEATURE_COLOR_MGMT in drv->features; -ENOMEM
 * when the crtc's list is full.  What could be attached is attached either
 * way.  (With DRM_COLOR_LEGACY_GAMMA_SPLIT 0 a size that disagrees is
 * refused with -EINVAL instead.) */
int drm_crtc_enable_color_mgmt(struct drm_device *dev, struct drm_crtc *crtc,
			       uint32_t degamma_lut_size, bool has_ctm,
			       uint32_t gamma_lut_size);

/* The size of the crtc's legacy gamma table (SETGAMMA / GETGAMMA and
 * GETCRTC's gamma_size): 2..256 entries, reset to a linear ramp.  The core
 * takes 256 for a crtc nobody set it on.  0, or -EINVAL (warned about). */
int drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc, int gamma_size);
/* What the legacy calls see as that size. */
uint32_t drm_crtc_legacy_gamma_size(const struct drm_device *dev,
				    const struct drm_crtc *crtc);

/* COLOR_ENCODING and COLOR_RANGE on the plane, with the entries whose bit
 * (BIT(DRM_COLOR_*)) is in the masks; each default must be one of them.
 * 0, -EINVAL (warned about), -ENOMEM. */
int drm_plane_create_color_properties(struct drm_device *dev,
				      struct drm_plane *plane,
				      u32 supported_encodings,
				      u32 supported_ranges,
				      enum drm_color_encoding default_encoding,
				      enum drm_color_range default_range);

/* ---- the older gamma call on an atomic crtc -------------------------------
 *
 * SETGAMMA's three tables of `size' entries, as a request makes them: one
 * new kernel blob of struct drm_color_lut in the state's GAMMA_LUT (or, on
 * a crtc with only a degamma table, DEGAMMA_LUT), the other table and the
 * CTM reset to none, color_mgmt_changed set when that changed anything.
 * The state holds the only reference to the new blob.  0, -ENODEV for a
 * crtc with neither table, -EINVAL, -ENOMEM. */
int drm_crtc_legacy_gamma_set_state(struct drm_device *dev,
				    struct drm_crtc_state *cs,
				    const uint16_t *red, const uint16_t *green,
				    const uint16_t *blue, uint32_t size);

/* ---- programming tables into hardware -------------------------------------
 *
 * For a driver that loads its palette or gamma table entry by entry:
 * set_lut(crtc, index, r, g, b) with 16-bit values. */

typedef void (*drm_crtc_set_lut_func)(struct drm_crtc *, unsigned int, u16, u16, u16);

/* A 256-entry ramp into a table of 256 (8 bits per channel), 32/64/32
 * (5-6-5) or 32 (5-5-5) entries. */
void drm_crtc_load_gamma_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
			     drm_crtc_set_lut_func set_gamma);
void drm_crtc_load_gamma_565_from_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
				      drm_crtc_set_lut_func set_gamma);
void drm_crtc_load_gamma_555_from_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
				      drm_crtc_set_lut_func set_gamma);
/* The linear ramp for the same layouts. */
void drm_crtc_fill_gamma_888(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma);
void drm_crtc_fill_gamma_565(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma);
void drm_crtc_fill_gamma_555(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma);
/* A 256-entry palette for C8 formats; the RGB332 palette; a grey ramp. */
void drm_crtc_load_palette_8(struct drm_crtc *crtc, const struct drm_color_lut *lut,
			     drm_crtc_set_lut_func set_palette);
void drm_crtc_fill_palette_332(struct drm_crtc *crtc, drm_crtc_set_lut_func set_palette);
void drm_crtc_fill_palette_8(struct drm_crtc *crtc, drm_crtc_set_lut_func set_palette);

#endif
