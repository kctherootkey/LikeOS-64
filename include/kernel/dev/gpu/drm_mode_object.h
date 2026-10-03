// LikeOS -- display-manager core: the properties a mode object carries.
//
// Every crtc, plane and connector has a list of the properties attached to
// it, in attach order, with the value of each.  OBJ_GETPROPERTIES reports
// that list; a property write goes through the atomic state for the values
// a state keeps, and lands in the list for the others.  Framebuffers carry
// no properties (their list pointer is NULL), and neither do encoders.
//
// The core's own properties (DPMS, CRTC_ID, FB_ID, ACTIVE, MODE_ID, the
// plane rectangle, IN_FORMATS, ...) come first in every list, in the order
// drm_kms.c attaches them; a driver's own follow.  Their values are read
// from the object itself (drm_mode_object_property_value), the list keeps
// only the value they were attached with.
//
// The object itself keeps its id in its own `id' field (one device-wide
// counter, ids never reused -- see drm_mode_object.c); there is no common
// base structure.  The functions below take the object's property list,
// or the object as a (type, pointer) pair.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_MODE_OBJECT_H
#define KERNEL_DEV_GPU_DRM_MODE_OBJECT_H

#include <kernel/uapi/types.h>

struct drm_device;
struct drm_prop;
struct drm_crtc;
struct drm_plane;
struct drm_connector;

/* The most properties one object can carry. */
#define DRM_OBJECT_MAX_PROPERTY 64

struct drm_object_properties {
	/* How many entries below are in use. */
	int count;
	/* Ids of the attached properties (dev->props[]), in attach order:
	 * the order OBJ_GETPROPERTIES reports them in. */
	uint32_t ids[DRM_OBJECT_MAX_PROPERTY];
	/* Each one's value.  For a property an atomic state keeps, the state
	 * is authoritative and this is the value it was attached with. */
	uint64_t values[DRM_OBJECT_MAX_PROPERTY];
	/* The core's properties are on the list (drm_kms.c).  An object is
	 * zeroed when it is added, so this starts false; the first look at
	 * the list attaches them, ahead of anything a driver attached. */
	bool core_attached;
};

/* ---- drm_mode_object.c ------------------------------------------------- */

/* Attach a property with its initial value.  Attaching the same property
 * twice, or more than DRM_OBJECT_MAX_PROPERTY, is a driver bug (warned
 * about, ignored). */
void drm_object_attach_property(struct drm_object_properties *props,
				struct drm_prop *property, uint64_t init_val);
/* Set / read the value kept in the list: 0, or -EINVAL when the property
 * is not attached. */
int drm_object_property_set_value(struct drm_object_properties *props,
				  struct drm_prop *property, uint64_t val);
int drm_object_property_get_value(const struct drm_object_properties *props,
				  struct drm_prop *property, uint64_t *val);
/* The attached property with this id, NULL when the object has none. */
struct drm_prop *drm_mode_obj_find_prop_id(struct drm_device *dev,
					   const struct drm_object_properties *props,
					   uint32_t prop_id);

/* The object `id' of type `type' (DRM_MODE_OBJECT_*, or DRM_MODE_OBJECT_ANY
 * for whatever it is), NULL when there is none; *type_out (may be NULL)
 * says which type it was.  Crtcs, connectors, encoders, planes,
 * framebuffers, properties and blobs.  Nothing is referenced: a blob a
 * caller keeps must come from drm_property_lookup_blob(). */
void *drm_mode_object_find(struct drm_device *dev, uint32_t id, uint32_t type,
			   uint32_t *type_out);
/* The property list of a crtc, plane or connector given as (type,
 * object), with the core's properties attached; NULL for other types. */
struct drm_object_properties *drm_mode_object_props_of(struct drm_device *dev,
						       uint32_t type, void *obj);
/* The property list of the object `id' of type `type' (DRM_MODE_OBJECT_*,
 * or DRM_MODE_OBJECT_ANY), NULL when there is no such object or it
 * carries no properties. */
struct drm_object_properties *drm_mode_object_properties(struct drm_device *dev,
							 uint32_t id,
							 uint32_t type);
/* The current value of an attached property: the core's own read from the
 * object (the committed state), the rest from the list.  0, or -EINVAL when
 * the property is not attached. */
int drm_mode_object_property_value(struct drm_device *dev, uint32_t type,
				   void *obj, struct drm_prop *property,
				   uint64_t *val);
/* Copy the object's property ids and values out to the user arrays (each
 * may be 0), at most *count of them, and set *count to how many there are.
 * `atomic': the client set the atomic capability -- without it the
 * properties only atomic clients use are left out (the OBJ_GETPROPERTIES
 * listing keeps them; see DRM_OBJ_GETPROPERTIES_HIDE_ATOMIC).  0 or
 * -EFAULT. */
int drm_mode_object_get_properties(struct drm_device *dev, uint32_t type,
				   void *obj, bool atomic, uint64_t props_ptr,
				   uint64_t prop_values_ptr, uint32_t *count);

/* ---- drm_kms.c ----------------------------------------------------------- */

/* Attach the core's properties to a crtc, plane or connector, ahead of any
 * a driver attached already.  Once per object; later calls do nothing.
 * Called for every object on its first property lookup, so an object added
 * at any time gets them; a file that adds objects may call these right
 * after adding one. */
void drm_crtc_attach_core_properties(struct drm_device *dev,
				     struct drm_crtc *crtc);
void drm_plane_attach_core_properties(struct drm_device *dev,
				      struct drm_plane *plane);
void drm_connector_attach_core_properties(struct drm_device *dev,
					  struct drm_connector *conn);

#endif
