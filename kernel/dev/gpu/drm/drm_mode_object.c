// LikeOS -- display-manager core: mode objects.
//
// Every mode object a client can name -- crtc, connector, encoder, plane,
// property, blob, framebuffer -- carries an id from one device-wide counter.
// Clients cache those ids, so they are handed out strictly in creation order
// and never reused.  Here too: finding an object by id and type, the
// property list each crtc, plane and connector carries, the per-object
// property listing (OBJ_GETPROPERTIES), the property write (OBJ_SETPROPERTY),
// and the id-array copy-out every enumerating ioctl shares.
//
// A property list holds ids and the value each property was attached
// with.  The core's own properties are attached by drm_kms.c the first time
// an object's list is looked at, ahead of whatever a driver attached; their
// current value is read from the object (the committed state), so the
// listing a client sees is the same before and after any change.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* OBJ_GETPROPERTIES lists every property of the object to every client.
 * Set to 1 to leave the properties only atomic clients use (CRTC_ID,
 * FB_ID, MODE_ID, the plane rectangle, ...) out for a client without the
 * atomic capability.  0 keeps the listing display servers have always
 * seen here. */
#define DRM_OBJ_GETPROPERTIES_HIDE_ATOMIC 0

/* A connector without an EDID lists no EDID property (rather than one
 * whose value is 0).  Set to 1 to always list it. */
#define DRM_CONNECTOR_LIST_EMPTY_EDID 0

/* ---- objects ------------------------------------------------------------- */

uint32_t drm_mode_id_alloc(struct drm_device *dev)
{
	return dev->next_mode_id++;
}

void *drm_mode_object_find(struct drm_device *dev, uint32_t id, uint32_t type,
			   uint32_t *type_out)
{
	void *obj = NULL;
	uint32_t t = 0;

	if (!id)
		return NULL;
	/* Ids are unique over every kind of object, so DRM_MODE_OBJECT_ANY
	 * finds at most one. */
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_CRTC) &&
	    (obj = drm_crtc_find(dev, id)))
		t = DRM_MODE_OBJECT_CRTC;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_CONNECTOR) &&
	    (obj = drm_conn_find(dev, id)))
		t = DRM_MODE_OBJECT_CONNECTOR;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_ENCODER) &&
	    (obj = drm_enc_find(dev, id)))
		t = DRM_MODE_OBJECT_ENCODER;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_PLANE) &&
	    (obj = drm_plane_find(dev, id)))
		t = DRM_MODE_OBJECT_PLANE;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_FB) &&
	    (obj = drm_fb_lookup(dev, id)))
		t = DRM_MODE_OBJECT_FB;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_PROPERTY) &&
	    (obj = drm_prop_find(dev, id)))
		t = DRM_MODE_OBJECT_PROPERTY;
	if (!obj && (type == DRM_MODE_OBJECT_ANY || type == DRM_MODE_OBJECT_BLOB) &&
	    (obj = drm_blob_find(dev, id)))
		t = DRM_MODE_OBJECT_BLOB;
	if (obj && type_out)
		*type_out = t;
	return obj;
}

struct drm_object_properties *drm_mode_object_props_of(struct drm_device *dev,
						       uint32_t type, void *obj)
{
	if (!obj)
		return NULL;
	switch (type) {
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *c = obj;
		drm_crtc_attach_core_properties(dev, c);
		return &c->properties;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *p = obj;
		drm_plane_attach_core_properties(dev, p);
		return &p->properties;
	}
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *c = obj;
		drm_connector_attach_core_properties(dev, c);
		return &c->properties;
	}
	default:
		/* framebuffers, encoders, properties and blobs carry none */
		return NULL;
	}
}

struct drm_object_properties *drm_mode_object_properties(struct drm_device *dev,
							 uint32_t id,
							 uint32_t type)
{
	uint32_t t = 0;
	void *obj = drm_mode_object_find(dev, id, type, &t);

	return drm_mode_object_props_of(dev, t, obj);
}

/* ---- property lists -------------------------------------------------------- */

void drm_object_attach_property(struct drm_object_properties *props,
				struct drm_prop *property, uint64_t init_val)
{
	int count;

	if (WARN_ON(!props || !property))
		return;
	count = props->count;
	for (int i = 0; i < count; i++)
		if (WARN_ON(props->ids[i] == property->id))
			return;

	if (count == DRM_OBJECT_MAX_PROPERTY) {
		WARN(1, "cannot attach property %s: the object has %d already",
		     property->name, count);
		return;
	}

	props->ids[count] = property->id;
	props->values[count] = init_val;
	props->count++;
}

int drm_object_property_set_value(struct drm_object_properties *props,
				  struct drm_prop *property, uint64_t val)
{
	if (!props || !property)
		return -EINVAL;

	/* On a driver with atomic entry points the state holds the value of
	 * a property a client can change; the list keeps only immutable
	 * ones up to date. */
	WARN_ON_ONCE(property->dev && property->dev->drv &&
		     property->dev->drv->atomic_commit &&
		     !(property->flags & DRM_MODE_PROP_IMMUTABLE));

	for (int i = 0; i < props->count; i++) {
		if (props->ids[i] == property->id) {
			props->values[i] = val;
			return 0;
		}
	}

	return -EINVAL;
}

int drm_object_property_get_value(const struct drm_object_properties *props,
				  struct drm_prop *property, uint64_t *val)
{
	if (!props || !property)
		return -EINVAL;
	for (int i = 0; i < props->count; i++) {
		if (props->ids[i] == property->id) {
			*val = props->values[i];
			return 0;
		}
	}

	return -EINVAL;
}

struct drm_prop *drm_mode_obj_find_prop_id(struct drm_device *dev,
					   const struct drm_object_properties *props,
					   uint32_t prop_id)
{
	if (!props || !prop_id)
		return NULL;
	for (int i = 0; i < props->count; i++)
		if (props->ids[i] == prop_id)
			return drm_prop_find(dev, prop_id);

	return NULL;
}

/* ---- current values ---------------------------------------------------------
 *
 * The core's properties report what the object shows now.  A driver with
 * atomic entry points keeps every plane's committed state in the plane; a
 * driver without them knows only crtcs, so its primary plane reports the
 * crtc's framebuffer and the mode's full size, and its cursor plane the
 * crtc it is on with no framebuffer of its own. */

static int connector_value(struct drm_device *dev, struct drm_connector *c,
			   const struct drm_prop *prop, uint64_t *val)
{
	if (prop->id == dev->prop_dpms)
		*val = (uint64_t)c->dpms;
	else if (prop->id == dev->prop_crtc_id)
		*val = c->crtc_id;
	else if (prop->id == dev->prop_edid)
		*val = c->edid_blob_id;
	return 0;
}

static int crtc_value(struct drm_device *dev, struct drm_crtc *cr,
		      const struct drm_prop *prop, uint64_t *val)
{
	if (prop->id == dev->prop_active)
		*val = (uint64_t)cr->active;
	else if (prop->id == dev->prop_mode_id)
		*val = cr->mode_blob;
	return 0;
}

static int plane_value(struct drm_device *dev, struct drm_plane *p,
		       const struct drm_prop *prop, uint64_t *val)
{
	uint32_t id = prop->id;

	if (id == dev->prop_type) {
		*val = p->type;
		return 0;
	}
	if (id == dev->prop_in_formats) {
		*val = p->in_formats_blob;
		return 0;
	}
	if (dev->drv->atomic_commit) {
		struct drm_crtc *cr = p->crtc >= 0 ? &dev->crtc[p->crtc] : NULL;

		if (id == dev->prop_fb_id)
			*val = p->fb_id;
		else if (id == dev->prop_crtc_id)
			*val = cr ? cr->id : 0;
		else if (id == dev->prop_src_x)
			*val = p->src_x;
		else if (id == dev->prop_src_y)
			*val = p->src_y;
		else if (id == dev->prop_src_w)
			*val = p->src_w;
		else if (id == dev->prop_src_h)
			*val = p->src_h;
		else if (id == dev->prop_crtc_x)
			*val = (uint64_t)(int64_t)p->crtc_x;
		else if (id == dev->prop_crtc_y)
			*val = (uint64_t)(int64_t)p->crtc_y;
		else if (id == dev->prop_crtc_w)
			*val = p->crtc_w;
		else if (id == dev->prop_crtc_h)
			*val = p->crtc_h;
		return 0;
	}

	/* Without atomic entry points only the primary and cursor planes
	 * exist for the core, as views of their crtc. */
	int cursor;
	struct drm_crtc *cr = drm_plane_crtc(dev, p->id, &cursor);
	if (!cr)
		return -ENOENT;
	if (id == dev->prop_fb_id)
		*val = cursor ? 0 : cr->fb_id;
	else if (id == dev->prop_crtc_id)
		*val = cr->active ? cr->id : 0;
	else if (id == dev->prop_src_x)
		*val = (uint64_t)cr->x << 16;
	else if (id == dev->prop_src_y)
		*val = (uint64_t)cr->y << 16;
	else if (id == dev->prop_src_w)
		*val = (uint64_t)cr->mode.hdisplay << 16;
	else if (id == dev->prop_src_h)
		*val = (uint64_t)cr->mode.vdisplay << 16;
	else if (id == dev->prop_crtc_x)
		*val = 0;
	else if (id == dev->prop_crtc_y)
		*val = 0;
	else if (id == dev->prop_crtc_w)
		*val = cr->mode.hdisplay;
	else if (id == dev->prop_crtc_h)
		*val = cr->mode.vdisplay;
	return 0;
}

int drm_mode_object_property_value(struct drm_device *dev, uint32_t type,
				   void *obj, struct drm_prop *property,
				   uint64_t *val)
{
	struct drm_object_properties *props = drm_mode_object_props_of(dev, type, obj);
	int rc;

	if (!props || !property)
		return -EINVAL;
	/* the value it was attached with, or set to since */
	rc = drm_object_property_get_value(props, property, val);
	if (rc)
		return rc;
	switch (type) {
	case DRM_MODE_OBJECT_CONNECTOR:
		return connector_value(dev, obj, property, val);
	case DRM_MODE_OBJECT_CRTC:
		return crtc_value(dev, obj, property, val);
	case DRM_MODE_OBJECT_PLANE:
		return plane_value(dev, obj, property, val);
	default:
		return 0;
	}
}

int drm_mode_object_get_properties(struct drm_device *dev, uint32_t type,
				   void *obj, bool atomic, uint64_t props_ptr,
				   uint64_t prop_values_ptr, uint32_t *count)
{
	struct drm_object_properties *props = drm_mode_object_props_of(dev, type, obj);
	uint32_t n = 0;

	if (!props)
		return -EINVAL;
	for (int i = 0; i < props->count; i++) {
		struct drm_prop *prop = drm_prop_find(dev, props->ids[i]);
		uint64_t val;
		int ret;

		if (!prop)
			continue;
		if ((prop->flags & DRM_MODE_PROP_ATOMIC) && !atomic)
			continue;

		ret = drm_mode_object_property_value(dev, type, obj, prop, &val);
		if (ret)
			return ret;

		if (!DRM_CONNECTOR_LIST_EMPTY_EDID && type == DRM_MODE_OBJECT_CONNECTOR &&
		    prop->id == dev->prop_edid && val == 0)
			continue;

		/* Filled as far as the caller made room; a null array is
		 * room for nothing. */
		if (*count > n) {
			uint64_t dst;

			if (props_ptr) {
				dst = props_ptr + (uint64_t)n * sizeof(uint32_t);
				if (!validate_user_ptr(dst, sizeof(uint32_t)) ||
				    copy_to_user((void *)(uintptr_t)dst, &prop->id,
						 sizeof(uint32_t)) != 0)
					return -EFAULT;
			}
			if (prop_values_ptr) {
				dst = prop_values_ptr + (uint64_t)n * sizeof(uint64_t);
				if (!validate_user_ptr(dst, sizeof(uint64_t)) ||
				    copy_to_user((void *)(uintptr_t)dst, &val,
						 sizeof(uint64_t)) != 0)
					return -EFAULT;
			}
		}

		n++;
	}
	*count = n;

	return 0;
}

/* ---- ioctls ---------------------------------------------------------- */

int drm_copy_ids(uint64_t uptr, uint32_t cap, const uint32_t *ids, uint32_t n)
{
	if (!uptr || !cap)
		return 0;
	uint32_t c = n < cap ? n : cap;
	if (!validate_user_ptr(uptr, c * 4))
		return -EFAULT;
	return copy_to_user((void *)(uintptr_t)uptr, ids, c * 4) == 0 ? 0 : -EFAULT;
}

/* OBJ_GETPROPERTIES: the object's properties in attach order, each with
 * its current value. */
int drm_mode_obj_get_properties_ioctl(struct drm_device *dev, void *kb,
				      struct drm_file *fp)
{
	struct drm_mode_obj_get_properties *arg = kb;
	uint32_t type = 0;
	void *obj;
	bool atomic = true;

	obj = drm_mode_object_find(dev, arg->obj_id, arg->obj_type, &type);
	if (!obj)
		return -ENOENT;
	if (!drm_mode_object_props_of(dev, type, obj))
		return -EINVAL;
	if (DRM_OBJ_GETPROPERTIES_HIDE_ATOMIC)
		atomic = ((fp->client_caps >> DRM_CLIENT_CAP_ATOMIC) & 1) != 0;

	return drm_mode_object_get_properties(dev, type, obj, atomic,
					      arg->props_ptr, arg->prop_values_ptr,
					      &arg->count_props);
}

/* A driver without atomic entry points: what its older calls can do. */
static int set_property_legacy(struct drm_device *dev, uint32_t type, void *obj,
			       struct drm_prop *prop, uint64_t prop_value)
{
	void *ref;
	int ret = -EINVAL;

	if (!drm_property_change_valid_get(prop, prop_value, &ref))
		return -EINVAL;

	switch (type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *c = obj;
		if (prop->id == dev->prop_dpms) {
			c->dpms = (int)prop_value;
			if (dev->drv->dpms)
				dev->drv->dpms(dev, c, (int)prop_value);
			ret = 0;
		}
		break;
	}
	default:
		/* nothing on a crtc or plane is settable this way */
		break;
	}
	drm_property_change_valid_put(prop, ref);
	return ret;
}

/* A driver with atomic entry points: a state with this one change,
 * checked and committed. */
static int set_property_atomic(struct drm_device *dev, struct drm_file *fp,
			       uint32_t type, uint32_t obj_id,
			       struct drm_prop *prop, uint64_t prop_value)
{
	void *ref;
	int ret;

	/* DPMS is the older on/off knob: any value other than on is off,
	 * and the state's crtc follows (drm_atomic_conn_set). */
	if (prop->id == dev->prop_dpms) {
		if (type != DRM_MODE_OBJECT_CONNECTOR)
			return -EINVAL;
		return drm_atomic_legacy_set_property(dev, fp, type, obj_id,
						      prop->id, prop_value);
	}

	if (!drm_property_change_valid_get(prop, prop_value, &ref))
		return -EINVAL;
	ret = drm_atomic_legacy_set_property(dev, fp, type, obj_id, prop->id,
					     prop_value);
	drm_property_change_valid_put(prop, ref);
	return ret;
}

/* OBJ_SETPROPERTY: the object must carry the property, and the value must
 * be one the property allows. */
int drm_mode_obj_set_property_ioctl(struct drm_device *dev, void *kb,
				    struct drm_file *fp)
{
	struct drm_mode_obj_set_property *arg = kb;
	struct drm_object_properties *props;
	struct drm_prop *property;
	uint32_t type = 0;
	void *obj;

	obj = drm_mode_object_find(dev, arg->obj_id, arg->obj_type, &type);
	if (!obj)
		return -ENOENT;

	props = drm_mode_object_props_of(dev, type, obj);
	if (!props)
		return -EINVAL;

	property = drm_mode_obj_find_prop_id(dev, props, arg->prop_id);
	if (!property)
		return -EINVAL;

	if (dev->drv->atomic_commit)
		return set_property_atomic(dev, fp, type, arg->obj_id, property,
					   arg->value);
	return set_property_legacy(dev, type, obj, property, arg->value);
}
