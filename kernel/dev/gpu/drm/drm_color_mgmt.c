// LikeOS -- display-manager core: colour management.
//
// The crtc's colour pipeline -- DEGAMMA_LUT, CTM, GAMMA_LUT with the sizes
// of the two tables -- and the YCbCr conversion properties of a plane
// (COLOR_ENCODING, COLOR_RANGE); see drm_color_mgmt.h for what each means
// and who keeps which part.  Here: attaching them to the objects that have
// them, checking a table against what the hardware can take, the integer
// conversions a driver programs the hardware with, the older gamma call
// turned into a table blob, and the helpers that load a table into
// hardware entry by entry.
//
// Hardware may use fewer bits than an entry has, or fewer entries than a
// table has (interpolating between, say, entry 0 and entry 4).  A driver
// that supports several table sizes advertises the largest and resamples
// smaller ones.  For mostly historical reasons GAMMA_LUT is also the colour
// map of indexed formats such as DRM_FORMAT_C8.
//
// The table the older calls (SETGAMMA, GETGAMMA, GETCRTC's gamma_size) speak
// of is a separate thing with a size of its own, the crtc's gamma_size: 256
// entries on every display controller this core drives, whatever the
// GAMMA_LUT_SIZE of its atomic table is (1024, or 262145 for a
// multi-segment table).  Each crtc carries its own GAMMA_LUT_SIZE and
// DEGAMMA_LUT_SIZE value, since pipes of one device can differ.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>

/* ---- conversions ------------------------------------------------------------ */

/* Example: a Q3.12 number takes 3 + 12 = 15 bits and covers
 * [-2^2, 2^2 - 2^-12].  The bits from m + n - 1 up are copies of the sign
 * (0 positive, 1 negative). */
u64 drm_color_ctm_s31_32_to_qm_n(u64 user_input, u32 m, u32 n)
{
	u64 mag = (user_input & ~BIT_ULL(63)) >> (32 - n);
	bool negative = !!(user_input & BIT_ULL(63));
	s64 val;

	WARN_ON(m > 32 || n > 32);

	val = (s64)clamp_val(mag, 0, negative ?
				     BIT_ULL(n + m - 1) : BIT_ULL(n + m - 1) - 1);

	return negative ? (u64)-val : (u64)val;
}

/* ---- the crtc's pipeline ------------------------------------------------------ */

/* Attach the device's property `prop_id' to the crtc with `val', unless it
 * is attached already -- then with that same value. */
static int crtc_attach_color(struct drm_device *dev, struct drm_crtc *crtc,
			     uint32_t prop_id, uint64_t val)
{
	struct drm_prop *prop = drm_prop_find(dev, prop_id);
	uint64_t cur;

	/* Not made by drm_kms_init: the driver did not ask for colour
	 * management, or has no gamma table. */
	if (WARN_ON(!prop))
		return -EINVAL;

	if (!drm_object_property_get_value(&crtc->properties, prop, &cur)) {
		/* the core's GAMMA_LUT pair, sized by drv->gamma_size */
		if (cur == val)
			return 0;
#if DRM_COLOR_LEGACY_GAMMA_SPLIT
		/* a crtc whose table differs from the device's default (a
		 * second pipe with a shorter one): this crtc's own size */
		return drm_object_property_set_value(&crtc->properties, prop, val);
#else
		WARN_ON(1);
		return -EINVAL;
#endif
	}

	drm_object_attach_property(&crtc->properties, prop, val);
	/* not attached: the list was full (warned about) */
	if (drm_object_property_get_value(&crtc->properties, prop, &cur))
		return -ENOMEM;
	return 0;
}

static void first_error(int *ret, int rc)
{
	if (rc && !*ret)
		*ret = rc;
}

int drm_crtc_enable_color_mgmt(struct drm_device *dev, struct drm_crtc *crtc,
			       uint32_t degamma_lut_size, bool has_ctm,
			       uint32_t gamma_lut_size)
{
	int ret = 0;

	if (!drm_blend_device_ready(dev) || WARN_ON(!crtc))
		return -EINVAL;

	/* The core's own come first in the crtc's list, and the core
	 * attaches GAMMA_LUT itself where the driver has a gamma table:
	 * attach them now, so that what follows sees them. */
	drm_crtc_attach_core_properties(dev, crtc);

	if (degamma_lut_size) {
		first_error(&ret, crtc_attach_color(dev, crtc, dev->prop_degamma_lut, 0));
		first_error(&ret, crtc_attach_color(dev, crtc, dev->prop_degamma_lut_size,
						    degamma_lut_size));
	}

	if (has_ctm)
		first_error(&ret, crtc_attach_color(dev, crtc, dev->prop_ctm, 0));

	if (gamma_lut_size) {
		first_error(&ret, crtc_attach_color(dev, crtc, dev->prop_gamma_lut, 0));
		first_error(&ret, crtc_attach_color(dev, crtc, dev->prop_gamma_lut_size,
						    gamma_lut_size));
	}

	return ret;
}

int drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc, int gamma_size)
{
	uint32_t n = (uint32_t)gamma_size;

	if (WARN_ON(!crtc || gamma_size <= 1 || gamma_size > 256))
		return -EINVAL;

	/* A linear ramp until a client loads one: entry i of n is
	 * i / (n - 1) of full scale, which for 256 entries is i << 8 | i. */
	crtc->gamma_size = n;
	for (uint32_t i = 0; i < 256; i++) {
		uint16_t v = i < n ? (uint16_t)(((uint32_t)i * 0xffffu) / (n - 1)) : 0xffff;

		crtc->gamma[0][i] = v;
		crtc->gamma[1][i] = v;
		crtc->gamma[2][i] = v;
	}
	crtc->gamma_identity = 1;
	return 0;
}

uint32_t drm_crtc_legacy_gamma_size(const struct drm_device *dev,
				    const struct drm_crtc *crtc)
{
#if DRM_COLOR_LEGACY_GAMMA_SPLIT
	(void)dev;
	return crtc->gamma_size ? crtc->gamma_size : 256;
#else
	(void)crtc;
	return dev->drv->gamma_size ? dev->drv->gamma_size : 256;
#endif
}

int drm_crtc_legacy_gamma_set_state(struct drm_device *dev,
				    struct drm_crtc_state *cs,
				    const uint16_t *red, const uint16_t *green,
				    const uint16_t *blue, uint32_t size)
{
	struct drm_crtc *crtc = cs->crtc;
	struct drm_blob *blob;
	struct drm_color_lut *blob_data;
	bool use_gamma_lut;
	bool replaced;
	uint32_t i;

	if (!crtc || !size ||
	    size > DRM_PROPERTY_BLOB_MAX_LENGTH / sizeof(struct drm_color_lut))
		return -EINVAL;

	if (drm_mode_obj_find_prop_id(dev, &crtc->properties, dev->prop_gamma_lut))
		use_gamma_lut = true;
	else if (drm_mode_obj_find_prop_id(dev, &crtc->properties, dev->prop_degamma_lut))
		use_gamma_lut = false;
	else
		return -ENODEV;

	blob = drm_property_create_blob(dev, sizeof(struct drm_color_lut) * size,
					NULL);
	if (!blob)
		return -ENOMEM;

	/* GAMMA_LUT with the older call's values. */
	blob_data = blob->data;
	for (i = 0; i < size; i++) {
		blob_data[i].red = red[i];
		blob_data[i].green = green[i];
		blob_data[i].blue = blue[i];
	}

	/* GAMMA_LUT set, DEGAMMA_LUT and CTM reset */
	replaced = drm_property_replace_blob(&cs->degamma_lut,
					     use_gamma_lut ? NULL : blob);
	replaced |= drm_property_replace_blob(&cs->ctm, NULL);
	replaced |= drm_property_replace_blob(&cs->gamma_lut,
					      use_gamma_lut ? blob : NULL);
	cs->color_mgmt_changed |= replaced;

	/* the creator's reference: the state has its own */
	drm_property_blob_put(blob);
	return 0;
}

/* ---- a plane's YCbCr conversion ----------------------------------------------- */

static const char * const color_encoding_name[] = {
	[DRM_COLOR_YCBCR_BT601] = "ITU-R BT.601 YCbCr",
	[DRM_COLOR_YCBCR_BT709] = "ITU-R BT.709 YCbCr",
	[DRM_COLOR_YCBCR_BT2020] = "ITU-R BT.2020 YCbCr",
};

static const char * const color_range_name[] = {
	[DRM_COLOR_YCBCR_FULL_RANGE] = "YCbCr full range",
	[DRM_COLOR_YCBCR_LIMITED_RANGE] = "YCbCr limited range",
};

/* Constant strings, so safe from any context. */
const char *drm_get_color_encoding_name(enum drm_color_encoding encoding)
{
	if (WARN_ON((unsigned int)encoding >= ARRAY_SIZE(color_encoding_name)))
		return "unknown";

	return color_encoding_name[encoding];
}

const char *drm_get_color_range_name(enum drm_color_range range)
{
	if (WARN_ON((unsigned int)range >= ARRAY_SIZE(color_range_name)))
		return "unknown";

	return color_range_name[range];
}

/* Each set bit of the masks is an enum value the plane supports. */
int drm_plane_create_color_properties(struct drm_device *dev,
				      struct drm_plane *plane,
				      u32 supported_encodings,
				      u32 supported_ranges,
				      enum drm_color_encoding default_encoding,
				      enum drm_color_range default_range)
{
	struct drm_prop *prop;
	struct drm_prop_enum_list enum_list[max_t(int, DRM_COLOR_ENCODING_MAX,
						  DRM_COLOR_RANGE_MAX)];
	int i, len;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->color_encoding_property || plane->color_range_property))
		return -EINVAL;

	if (WARN_ON(supported_encodings == 0 ||
		    (supported_encodings & -BIT(DRM_COLOR_ENCODING_MAX)) != 0 ||
		    (supported_encodings & BIT(default_encoding)) == 0))
		return -EINVAL;

	if (WARN_ON(supported_ranges == 0 ||
		    (supported_ranges & -BIT(DRM_COLOR_RANGE_MAX)) != 0 ||
		    (supported_ranges & BIT(default_range)) == 0))
		return -EINVAL;

	len = 0;
	for (i = 0; i < DRM_COLOR_ENCODING_MAX; i++) {
		if ((supported_encodings & BIT(i)) == 0)
			continue;

		enum_list[len].type = i;
		enum_list[len].name = color_encoding_name[i];
		len++;
	}

	prop = drm_property_create_enum(dev, 0, "COLOR_ENCODING",
					enum_list, len);
	prop = drm_blend_plane_attach(dev, plane, prop, default_encoding);
	if (!prop)
		return -ENOMEM;
	plane->color_encoding_property = prop;
	plane->color_encoding = default_encoding;

	len = 0;
	for (i = 0; i < DRM_COLOR_RANGE_MAX; i++) {
		if ((supported_ranges & BIT(i)) == 0)
			continue;

		enum_list[len].type = i;
		enum_list[len].name = color_range_name[i];
		len++;
	}

	prop = drm_property_create_enum(dev, 0, "COLOR_RANGE",
					enum_list, len);
	prop = drm_blend_plane_attach(dev, plane, prop, default_range);
	if (!prop)
		return -ENOMEM;
	plane->color_range_property = prop;
	plane->color_range = default_range;

	return 0;
}

/* ---- checking a table ----------------------------------------------------------- */

/* For a driver's check: refuse a client's table its hardware cannot take.
 * Quiet: the client sees the -EINVAL, and a refused request is not a
 * kernel event worth a log line. */
int drm_color_lut_check(const struct drm_blob *lut, u32 tests)
{
	const struct drm_color_lut *entry;
	int i;

	if (!lut || !tests)
		return 0;

	entry = lut->data;
	for (i = 0; i < drm_color_lut_size(lut); i++) {
		if (tests & DRM_COLOR_LUT_EQUAL_CHANNELS) {
			if (entry[i].red != entry[i].blue ||
			    entry[i].red != entry[i].green)
				return -EINVAL; /* r/g/b must be equal */
		}

		if (i > 0 && tests & DRM_COLOR_LUT_NON_DECREASING) {
			if (entry[i].red < entry[i - 1].red ||
			    entry[i].green < entry[i - 1].green ||
			    entry[i].blue < entry[i - 1].blue)
				return -EINVAL; /* entries must never decrease */
		}
	}

	return 0;
}

/* ---- gamma tables -------------------------------------------------------------- */

/* The 256 entries of `lut' as they are. */
void drm_crtc_load_gamma_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
			     drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;

	for (i = 0; i < 256; ++i)
		set_gamma(crtc, i, lut[i].red, lut[i].green, lut[i].blue);
}

/* The 256 entries of `lut' picked down to 5/6/5 bits per channel. */
void drm_crtc_load_gamma_565_from_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
				      drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;
	u16 r, g, b;

	for (i = 0; i < 32; ++i) {
		r = lut[i * 8 + i / 4].red;
		g = lut[i * 4 + i / 16].green;
		b = lut[i * 8 + i / 4].blue;
		set_gamma(crtc, i, r, g, b);
	}
	/* Green has one more bit, so add padding with 0 for red and blue. */
	for (i = 32; i < 64; ++i) {
		g = lut[i * 4 + i / 16].green;
		set_gamma(crtc, i, 0, g, 0);
	}
}

/* The 256 entries of `lut' picked down to 5/5/5 bits per channel. */
void drm_crtc_load_gamma_555_from_888(struct drm_crtc *crtc, const struct drm_color_lut *lut,
				      drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;
	u16 r, g, b;

	for (i = 0; i < 32; ++i) {
		r = lut[i * 8 + i / 4].red;
		g = lut[i * 8 + i / 4].green;
		b = lut[i * 8 + i / 4].blue;
		set_gamma(crtc, i, r, g, b);
	}
}

static void fill_gamma_888(struct drm_crtc *crtc, unsigned int i, u16 r, u16 g, u16 b,
			   drm_crtc_set_lut_func set_gamma)
{
	r = (u16)((r << 8) | r);
	g = (u16)((g << 8) | g);
	b = (u16)((b << 8) | b);

	set_gamma(crtc, i, r, g, b);
}

void drm_crtc_fill_gamma_888(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;

	for (i = 0; i < 256; ++i)
		fill_gamma_888(crtc, i, (u16)i, (u16)i, (u16)i, set_gamma);
}

static void fill_gamma_565(struct drm_crtc *crtc, unsigned int i, u16 r, u16 g, u16 b,
			   drm_crtc_set_lut_func set_gamma)
{
	r = (u16)((r << 11) | (r << 6) | (r << 1) | (r >> 4));
	g = (u16)((g << 10) | (g << 4) | (g >> 2));
	b = (u16)((b << 11) | (b << 6) | (b << 1) | (b >> 4));

	set_gamma(crtc, i, r, g, b);
}

void drm_crtc_fill_gamma_565(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;

	for (i = 0; i < 32; ++i)
		fill_gamma_565(crtc, i, (u16)i, (u16)i, (u16)i, set_gamma);
	/* Green has one more bit, so add padding with 0 for red and blue. */
	for (i = 32; i < 64; ++i)
		fill_gamma_565(crtc, i, 0, (u16)i, 0, set_gamma);
}

static void fill_gamma_555(struct drm_crtc *crtc, unsigned int i, u16 r, u16 g, u16 b,
			   drm_crtc_set_lut_func set_gamma)
{
	r = (u16)((r << 11) | (r << 6) | (r << 1) | (r >> 4));
	g = (u16)((g << 11) | (g << 6) | (g << 1) | (g >> 4));
	b = (u16)((b << 11) | (b << 6) | (b << 1) | (b >> 4));

	set_gamma(crtc, i, r, g, b);
}

void drm_crtc_fill_gamma_555(struct drm_crtc *crtc, drm_crtc_set_lut_func set_gamma)
{
	unsigned int i;

	for (i = 0; i < 32; ++i)
		fill_gamma_555(crtc, i, (u16)i, (u16)i, (u16)i, set_gamma);
}

/* ---- palettes ------------------------------------------------------------------ */

void drm_crtc_load_palette_8(struct drm_crtc *crtc, const struct drm_color_lut *lut,
			     drm_crtc_set_lut_func set_palette)
{
	unsigned int i;

	for (i = 0; i < 256; ++i)
		set_palette(crtc, i, lut[i].red, lut[i].green, lut[i].blue);
}

static void fill_palette_332(struct drm_crtc *crtc, u16 r, u16 g, u16 b,
			     drm_crtc_set_lut_func set_palette)
{
	unsigned int i = (unsigned int)((r << 5) | (g << 2) | b); /* 8-bit palette index */

	/* Expand R (3-bit) G (3-bit) and B (2-bit) values to 16-bit values */
	r = (u16)((r << 13) | (r << 10) | (r << 7) | (r << 4) | (r << 1) | (r >> 2));
	g = (u16)((g << 13) | (g << 10) | (g << 7) | (g << 4) | (g << 1) | (g >> 2));
	b = (u16)((b << 14) | (b << 12) | (b << 10) | (b << 8) | (b << 6) | (b << 4) |
		  (b << 2) | b);

	set_palette(crtc, i, r, g, b);
}

void drm_crtc_fill_palette_332(struct drm_crtc *crtc, drm_crtc_set_lut_func set_palette)
{
	unsigned int r, g, b;

	/* Limits of 8-8-4 are the maximum number of values for each channel. */
	for (r = 0; r < 8; ++r) {
		for (g = 0; g < 8; ++g) {
			for (b = 0; b < 4; ++b)
				fill_palette_332(crtc, (u16)r, (u16)g, (u16)b, set_palette);
		}
	}
}

static void fill_palette_8(struct drm_crtc *crtc, unsigned int i,
			   drm_crtc_set_lut_func set_palette)
{
	u16 y = (u16)((i << 8) | i); /* relative luminance */

	set_palette(crtc, i, y, y, y);
}

void drm_crtc_fill_palette_8(struct drm_crtc *crtc, drm_crtc_set_lut_func set_palette)
{
	unsigned int i;

	for (i = 0; i < 256; ++i)
		fill_palette_8(crtc, i, set_palette);
}
