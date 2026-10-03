// LikeOS -- display-manager core: plane rotation, stacking and blending.
//
// The basic plane model is a source rectangle of a framebuffer (16.16
// fixed point) scaled onto a destination rectangle of a crtc.  A driver
// whose hardware can do more says so with properties on each plane:
//
// alpha:  the plane-wide opacity, 0 transparent .. 0xffff opaque, combined
//         with the pixels' own alpha.  The pixels are not expected to be
//         pre-multiplied by it.
// rotation:  a reflection along x and/or y, then a rotation counter-
//         clockwise by 0/90/180/270 degrees, applied to the image sampled
//         from the source rectangle before it is scaled onto the
//         destination.  rotate-180 is the same as reflecting along both
//         axes; drm_rotation_simplify() turns one into the other for
//         hardware that has only one of them.
// zpos:   the stacking order: higher values above lower ones, equal values
//         in no particular order (drm_atomic_normalize_zpos breaks ties by
//         plane id).  Immutable where the hardware's order is fixed.  When
//         one plane of a device has it, all of them should.
// pixel blend mode:  how a pixel is combined with what is below it --
//         "None":           out = plane_alpha * fg + (1 - plane_alpha) * bg
//         "Pre-multiplied": out = plane_alpha * fg +
//                                 (1 - plane_alpha * fg.alpha) * bg
//         "Coverage":       out = plane_alpha * fg.alpha * fg +
//                                 (1 - plane_alpha * fg.alpha) * bg
//         For a format without alpha the three are the same.
//
// Creating a property here attaches it to the plane, points the plane's
// own pointer for that kind at it and sets the plane's committed value; the
// atomic core routes writes into the plane state and commits them back (see
// drm_blend.h).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Samsung's and Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Samsung Electronics Co.Ltd
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>

static bool name_is(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

bool drm_blend_device_ready(struct drm_device *dev)
{
	/* CRTC_ID is among the first properties drm_kms_init makes, and
	 * drm_kms_init runs inside drm_dev_register.  Before that the core's
	 * properties do not exist yet, and an object whose list was looked at
	 * would never get them. */
	return !WARN_ON(!dev || !dev->prop_crtc_id);
}

/* ---- sharing identical properties ----------------------------------------- */

static bool prop_identical(const struct drm_prop *a, const struct drm_prop *b)
{
	if (a->flags != b->flags || !name_is(a->name, b->name))
		return false;
	if (drm_property_type_is(a, DRM_MODE_PROP_RANGE) ||
	    drm_property_type_is(a, DRM_MODE_PROP_SIGNED_RANGE))
		return a->values[0] == b->values[0] && a->values[1] == b->values[1];
	if (drm_property_type_is(a, DRM_MODE_PROP_OBJECT))
		return a->values[0] == b->values[0];
	if (drm_property_type_is(a, DRM_MODE_PROP_ENUM) ||
	    drm_property_type_is(a, DRM_MODE_PROP_BITMASK)) {
		const struct drm_mode_property_enum *ea = drm_property_enums(a);
		const struct drm_mode_property_enum *eb = drm_property_enums(b);

		if (a->nenums != b->nenums)
			return false;
		for (uint32_t i = 0; i < a->nenums; i++)
			if (ea[i].value != eb[i].value || !name_is(ea[i].name, eb[i].name))
				return false;
	}
	return true;
}

/* Every plane of a device asks for its own rotation, alpha, blend mode and
 * zpos, and the device's table has room for DRM_MAX_PROPS properties all
 * told: one per plane would fill it on a device with many planes.  A
 * client looks a plane's property up by name in that plane's list, and the
 * value is per plane in that list, so planes with the same capabilities can
 * share one property the way every plane shares CRTC_ID. */
struct drm_prop *drm_blend_share_property(struct drm_device *dev,
					  struct drm_prop *fresh)
{
	if (!fresh)
		return NULL;
	for (uint32_t i = 0; i < dev->nprops; i++) {
		struct drm_prop *p = &dev->props[i];

		if (p == fresh || !p->id)
			continue;
		if (prop_identical(p, fresh)) {
			/* `fresh' is the newest entry: destroying it gives its
			 * slot back to the table. */
			drm_property_destroy(dev, fresh);
			return p;
		}
	}
	return fresh;
}

struct drm_prop *drm_blend_plane_attach(struct drm_device *dev,
					struct drm_plane *plane,
					struct drm_prop *prop,
					uint64_t init_val)
{
	uint64_t check;

	prop = drm_blend_share_property(dev, prop);
	if (!prop)
		return NULL;
	drm_object_attach_property(&plane->properties, prop, init_val);
	/* not attached: the list was full (warned about) */
	if (drm_object_property_get_value(&plane->properties, prop, &check))
		return NULL;
	return prop;
}

/* ---- creating them ------------------------------------------------------- */

/* A mutable "alpha" range, 0 (transparent) .. 0xffff (opaque), starting
 * opaque. */
int drm_plane_create_alpha_property(struct drm_device *dev,
				    struct drm_plane *plane)
{
	struct drm_prop *prop;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->alpha_property))
		return -EINVAL;

	prop = drm_property_create_range(dev, 0, "alpha",
					 0, DRM_BLEND_ALPHA_OPAQUE);
	prop = drm_blend_plane_attach(dev, plane, prop, DRM_BLEND_ALPHA_OPAQUE);
	if (!prop)
		return -ENOMEM;

	plane->alpha_property = prop;
	plane->alpha = DRM_BLEND_ALPHA_OPAQUE;

	return 0;
}

/* The "rotation" bitmask with the entries the plane supports:
 *   DRM_MODE_ROTATE_0 "rotate-0", DRM_MODE_ROTATE_90 "rotate-90",
 *   DRM_MODE_ROTATE_180 "rotate-180", DRM_MODE_ROTATE_270 "rotate-270",
 *   DRM_MODE_REFLECT_X "reflect-x", DRM_MODE_REFLECT_Y "reflect-y".
 * The axes are those of the source rectangle, before rotation. */
int drm_plane_create_rotation_property(struct drm_device *dev,
				       struct drm_plane *plane,
				       unsigned int rotation,
				       unsigned int supported_rotations)
{
	static const struct drm_prop_enum_list props[] = {
		{ __builtin_ffs(DRM_MODE_ROTATE_0) - 1, "rotate-0" },
		{ __builtin_ffs(DRM_MODE_ROTATE_90) - 1, "rotate-90" },
		{ __builtin_ffs(DRM_MODE_ROTATE_180) - 1, "rotate-180" },
		{ __builtin_ffs(DRM_MODE_ROTATE_270) - 1, "rotate-270" },
		{ __builtin_ffs(DRM_MODE_REFLECT_X) - 1, "reflect-x" },
		{ __builtin_ffs(DRM_MODE_REFLECT_Y) - 1, "reflect-y" },
	};
	struct drm_prop *prop;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->rotation_property))
		return -EINVAL;

	WARN_ON((supported_rotations & DRM_MODE_ROTATE_MASK) == 0);
	WARN_ON(!is_power_of_2(rotation & DRM_MODE_ROTATE_MASK));
	WARN_ON(rotation & ~supported_rotations);

	prop = drm_property_create_bitmask(dev, 0, "rotation",
					   props, ARRAY_SIZE(props),
					   supported_rotations);
	prop = drm_blend_plane_attach(dev, plane, prop, rotation);
	if (!prop)
		return -ENOMEM;

	plane->rotation = rotation;
	plane->rotation_property = prop;

	return 0;
}

/* For hardware that can do everything but, say, DRM_MODE_REFLECT_X:
 *
 *   drm_rotation_simplify(rotation, DRM_MODE_ROTATE_0 |
 *                         DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_180 |
 *                         DRM_MODE_ROTATE_270 | DRM_MODE_REFLECT_Y);
 *
 * gets rid of the x reflection where an equivalent without it exists:
 * reflecting along both axes is a rotation by 180 degrees, so flipping
 * both reflection bits and turning 180 degrees further shows the same
 * picture. */
unsigned int drm_rotation_simplify(unsigned int rotation,
				   unsigned int supported_rotations)
{
	if (rotation & ~supported_rotations) {
		rotation ^= DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
		rotation = (rotation & DRM_MODE_REFLECT_MASK) |
			   (unsigned int)BIT((gpu_ffs(rotation & DRM_MODE_ROTATE_MASK) + 1)
					     % 4);
	}

	return rotation;
}

/* The plane's zpos property made: attach it, and the committed position
 * starts there (normalised as it is until the first check says
 * otherwise). */
static int plane_set_zpos_property(struct drm_device *dev,
				   struct drm_plane *plane,
				   struct drm_prop *prop, unsigned int zpos)
{
	prop = drm_blend_plane_attach(dev, plane, prop, zpos);
	if (!prop)
		return -ENOMEM;

	plane->zpos_property = prop;
	plane->zpos = zpos;
	plane->normalized_zpos = zpos;

	return 0;
}

/* A mutable "zpos" range [min, max], usually 0 .. planes per crtc - 1.
 * Planes whose position the hardware fixes (a background, a cursor that is
 * always on top) get an immutable zpos outside [min, max] instead, and
 * every plane starts at a zpos that sorts it where it is shown.  A driver
 * with a mutable zpos calls drm_atomic_normalize_zpos() from its check. */
int drm_plane_create_zpos_property(struct drm_device *dev,
				   struct drm_plane *plane, unsigned int zpos,
				   unsigned int min, unsigned int max)
{
	struct drm_prop *prop;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->zpos_property))
		return -EINVAL;

	prop = drm_property_create_range(dev, 0, "zpos", min, max);

	return plane_set_zpos_property(dev, plane, prop, zpos);
}

/* An immutable "zpos": tells clients how the planes are stacked, and that
 * the order cannot be changed. */
int drm_plane_create_zpos_immutable_property(struct drm_device *dev,
					     struct drm_plane *plane,
					     unsigned int zpos)
{
	struct drm_prop *prop;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->zpos_property))
		return -EINVAL;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE,
					 "zpos", zpos, zpos);

	return plane_set_zpos_property(dev, plane, prop, zpos);
}

/* The "pixel blend mode" enum with the modes in `supported_modes'
 * (BIT(DRM_MODE_BLEND_*)): "None", "Pre-multiplied", "Coverage". */
int drm_plane_create_blend_mode_property(struct drm_device *dev,
					 struct drm_plane *plane,
					 unsigned int supported_modes)
{
	struct drm_prop *prop;
	static const struct drm_prop_enum_list props[] = {
		{ DRM_MODE_BLEND_PIXEL_NONE, "None" },
		{ DRM_MODE_BLEND_PREMULTI, "Pre-multiplied" },
		{ DRM_MODE_BLEND_COVERAGE, "Coverage" },
	};
	unsigned int default_mode;
	unsigned int valid_mode_mask = BIT(DRM_MODE_BLEND_PIXEL_NONE) |
				       BIT(DRM_MODE_BLEND_PREMULTI) |
				       BIT(DRM_MODE_BLEND_COVERAGE);
	unsigned int i;

	if (!drm_blend_device_ready(dev) || WARN_ON(!plane))
		return -EINVAL;
	if (WARN_ON(plane->blend_mode_property))
		return -EINVAL;

	if (WARN_ON((supported_modes & ~valid_mode_mask) ||
		    (supported_modes == 0)))
		return -EINVAL;

	prop = drm_property_create(dev, DRM_MODE_PROP_ENUM,
				   "pixel blend mode",
				   (int)gpu_hweight32(supported_modes));
	if (!prop)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(props); i++) {
		int ret;

		if (!(BIT(props[i].type) & supported_modes))
			continue;

		ret = drm_property_add_enum(prop, (uint64_t)props[i].type,
					    props[i].name);

		if (ret) {
			drm_property_destroy(dev, prop);

			return ret;
		}
	}

	if (supported_modes & BIT(DRM_MODE_BLEND_PREMULTI))
		default_mode = DRM_MODE_BLEND_PREMULTI;
	else if (supported_modes & BIT(DRM_MODE_BLEND_COVERAGE))
		default_mode = DRM_MODE_BLEND_COVERAGE;
	else
		default_mode = DRM_MODE_BLEND_PIXEL_NONE;

	prop = drm_blend_plane_attach(dev, plane, prop, default_mode);
	if (!prop)
		return -ENOMEM;

	plane->blend_mode_property = prop;
	plane->pixel_blend_mode = (uint16_t)default_mode;

	return 0;
}

/* ---- stacking ------------------------------------------------------------- */

/* Lower zpos first; equal zpos by plane id.  The states are elements of
 * st->planes[], so their index is the plane's. */
static int zpos_cmp(const struct drm_device *dev,
		    const struct drm_atomic_state *st,
		    const struct drm_plane_state *sa,
		    const struct drm_plane_state *sb)
{
	uint32_t ida = dev->planes[sa - st->planes].id;
	uint32_t idb = dev->planes[sb - st->planes].id;

	if (sa->zpos != sb->zpos)
		return sa->zpos < sb->zpos ? -1 : 1;
	if (ida != idb)
		return ida < idb ? -1 : 1;
	return 0;
}

/* normalized_zpos 0..n-1 for the n plane states of one crtc.  The keys are
 * unique (plane ids are), so any sort gives the same order; an insertion
 * sort over at most DRM_MAX_PLANES pointers needs no allocation. */
static void crtc_normalize_zpos(const struct drm_device *dev,
				const struct drm_atomic_state *st,
				struct drm_plane_state **states, int n)
{
	for (int i = 1; i < n; i++) {
		struct drm_plane_state *s = states[i];
		int j = i - 1;

		while (j >= 0 && zpos_cmp(dev, st, states[j], s) > 0) {
			states[j + 1] = states[j];
			j--;
		}
		states[j + 1] = s;
	}

	for (int i = 0; i < n; i++)
		states[i]->normalized_zpos = (uint32_t)i;
}

int drm_atomic_normalize_zpos_crtcs(struct drm_device *dev,
				    struct drm_atomic_state *st,
				    uint32_t *changed_crtcs)
{
	struct drm_plane_state *states[DRM_MAX_PLANES];
	uint32_t old_mask[DRM_MAX_CRTCS], new_mask[DRM_MAX_CRTCS];
	uint32_t zpos_changed = 0;
	uint32_t i;

	for (i = 0; i < DRM_MAX_CRTCS; i++)
		old_mask[i] = new_mask[i] = 0;

	for (i = 0; i < dev->nplanes; i++) {
		const struct drm_plane *plane = &dev->planes[i];
		struct drm_plane_state *ps = &st->planes[i];

		if (ps->crtc >= (int)dev->ncrtc)
			return -EINVAL;
		if (ps->crtc >= 0) {
			new_mask[ps->crtc] |= 1u << i;
			if (plane->zpos != ps->zpos)
				zpos_changed |= 1u << ps->crtc;
		}
		if (plane->crtc >= 0 && plane->crtc < (int)dev->ncrtc)
			old_mask[plane->crtc] |= 1u << i;
	}

	for (i = 0; i < dev->ncrtc; i++) {
		int n = 0;

		if (old_mask[i] != new_mask[i])
			zpos_changed |= 1u << i;
		for (uint32_t p = 0; p < dev->nplanes; p++)
			if (new_mask[i] & (1u << p))
				states[n++] = &st->planes[p];
		crtc_normalize_zpos(dev, st, states, n);
	}

	if (changed_crtcs)
		*changed_crtcs = zpos_changed;
	return 0;
}
