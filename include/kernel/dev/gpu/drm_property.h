// LikeOS -- display-manager core: creating properties and blobs.
//
// A property is an entry of the device's table (struct drm_prop in drm.h):
// a name, a type -- range, signed range, enum, bitmask, object, blob, or a
// boolean (a 0..1 range) -- and the values or names its type needs.
// Properties are created once, while the driver sets the device up, and
// attached to the objects that carry them (drm_mode_object.h).
//
// A blob (struct drm_blob in drm.h) is an immutable byte array named by an
// id: an EDID, a mode, a format list, a lookup table, damage rectangles.
// Blobs are reference counted; one a client created belongs to that file
// and goes when it destroys it or closes, while states that still use it
// keep it alive through their references.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_PROPERTY_H
#define KERNEL_DEV_GPU_DRM_PROPERTY_H

#include <kernel/dev/gpu/drm.h>

/* One enum or bitmask entry to create a property from: the value (for a
 * bitmask, the bit number) and its name. */
struct drm_prop_enum_list {
	int type;
	const char *name;
};

/* Is the property of this type?  Extended types (object, signed range)
 * compare as a value, the original ones as a bit. */
static inline bool drm_property_type_is(const struct drm_prop *property,
					uint32_t type)
{
	if (property->flags & DRM_MODE_PROP_EXTENDED_TYPE)
		return (property->flags & DRM_MODE_PROP_EXTENDED_TYPE) == type;
	return (property->flags & type) != 0;
}

/* ---- drm_property.c ---------------------------------------------------- */

/* A new property of the type in `flags' (DRM_MODE_PROP_* with any of
 * IMMUTABLE / ATOMIC), room for `num_values' enum entries; NULL when the
 * table is full or the flags make no sense. */
struct drm_prop *drm_property_create(struct drm_device *dev, uint32_t flags,
				     const char *name, int num_values);
struct drm_prop *drm_property_create_enum(struct drm_device *dev,
					  uint32_t flags, const char *name,
					  const struct drm_prop_enum_list *props,
					  int num_values);
/* Only the entries whose bit is in supported_bits are added. */
struct drm_prop *drm_property_create_bitmask(struct drm_device *dev,
					     uint32_t flags, const char *name,
					     const struct drm_prop_enum_list *props,
					     int num_props,
					     uint64_t supported_bits);
struct drm_prop *drm_property_create_range(struct drm_device *dev,
					   uint32_t flags, const char *name,
					   uint64_t min, uint64_t max);
struct drm_prop *drm_property_create_signed_range(struct drm_device *dev,
						  uint32_t flags,
						  const char *name,
						  int64_t min, int64_t max);
/* An object property: its value names an object of `type'
 * (DRM_MODE_OBJECT_*). */
struct drm_prop *drm_property_create_object(struct drm_device *dev,
					    uint32_t flags, const char *name,
					    uint32_t type);
struct drm_prop *drm_property_create_bool(struct drm_device *dev,
					  uint32_t flags, const char *name);
/* One more entry on an enum or bitmask property (no fixed limit): 0, or
 * -EINVAL for a value a bitmask cannot hold / a duplicate, -ENOMEM. */
int drm_property_add_enum(struct drm_prop *property, uint64_t value,
			  const char *name);
/* The property with this id, NULL when there is none. */
struct drm_prop *drm_prop_find(struct drm_device *dev, uint32_t id);
static inline struct drm_prop *drm_property_find(struct drm_device *dev,
						 uint32_t id)
{
	return drm_prop_find(dev, id);
}

/* A new blob holding a copy of `data' (zeroed when NULL), owned by the
 * kernel, with one reference; NULL when there is no room. */
struct drm_blob *drm_property_create_blob(struct drm_device *dev,
					  size_t length, const void *data);
/* The blob with this id, referenced; NULL when there is none. */
struct drm_blob *drm_property_lookup_blob(struct drm_device *dev,
					  uint32_t id);
struct drm_blob *drm_property_blob_get(struct drm_blob *blob);
/* Drop a reference; the last one frees the blob and its id. */
void drm_property_blob_put(struct drm_blob *blob);
/* Make *blob point at new_blob (references moved accordingly); true when
 * that changed anything. */
bool drm_property_replace_blob(struct drm_blob **blob,
			       struct drm_blob *new_blob);
/* The same from a blob id a client gave (0 = none), checking the blob's
 * size: at most max_size, exactly expected_size, a multiple of
 * expected_elem_size (each <= 0 when it does not matter).  *replaced is
 * set (never cleared) when *blob changed.  0, or -EINVAL for an unknown
 * blob or one of the wrong size. */
int drm_property_replace_blob_from_id(struct drm_device *dev,
				      struct drm_blob **blob,
				      uint64_t blob_id,
				      int64_t max_size,
				      int64_t expected_size,
				      int64_t expected_elem_size,
				      bool *replaced);
/* Replace a kernel-owned blob an object property refers to (an EDID,
 * a connector path) with one made of `data' (none when length is 0), and
 * update the property's value in the object's list. */
int drm_property_replace_global_blob(struct drm_device *dev,
				     struct drm_blob **replace,
				     size_t length, const void *data,
				     struct drm_object_properties *obj_holds_id,
				     struct drm_prop *prop_holds_id);
/* A closing file's blobs: each loses the file's reference. */
void drm_property_destroy_user_blobs(struct drm_device *dev,
				     struct drm_file *fp);

/* Undo a property made by drm_property_create*() while the driver is still
 * setting the device up (its id is not handed out again). */
void drm_property_destroy(struct drm_device *dev, struct drm_prop *property);

/* Is `value' one a client may write into `property'?  A range or signed
 * range: inside it; an enum: one of its entries; a bitmask: only its bits;
 * a blob: 0 or an existing blob; an object: 0 or an existing object of the
 * property's type.  Never for an immutable property.  *ref is set to what
 * the value keeps alive for the duration of the write (a referenced
 * struct drm_blob, else NULL); hand it back to drm_property_change_valid_put
 * once the write is done. */
bool drm_property_change_valid_get(struct drm_prop *property, uint64_t value,
				   void **ref);
void drm_property_change_valid_put(struct drm_prop *property, void *ref);

/* The entries of an enum or bitmask property (nenums of them). */
static inline const struct drm_mode_property_enum *
drm_property_enums(const struct drm_prop *property)
{
	return property->enum_list ? property->enum_list : property->enums;
}

/* 1: blobs as large as the biggest lookup table a crtc takes -- a
 * multi-segment gamma table of 262145 entries of 8 bytes, just over 2 MiB --
 * with every file's own blobs bounded in total.  0: 64 KiB per blob and no
 * bound per file. */
#ifndef DRM_PROPERTY_BLOB_LARGE
#define DRM_PROPERTY_BLOB_LARGE 1
#endif

#if DRM_PROPERTY_BLOB_LARGE
/* The largest blob a client may create: 2 MiB and 64 KiB of slack above
 * the 2 MiB + 8 bytes of the biggest gamma table. */
#define DRM_PROPERTY_BLOB_MAX_LENGTH (2u * 1024u * 1024u + 64u * 1024u)
/* The most bytes of blobs one file may own at once.  A request names a
 * handful of blobs; this is room for several full sets of tables per crtc
 * and keeps one client from tying up the device's blob slots' worth of
 * kernel memory (DRM_MAX_BLOBS of the size above). */
#define DRM_PROPERTY_BLOB_FILE_MAX (32ull * 1024u * 1024u)
#else
/* The largest blob a client may create. */
#define DRM_PROPERTY_BLOB_MAX_LENGTH (64u * 1024u)
#endif

#endif
