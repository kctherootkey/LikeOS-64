// LikeOS -- display-manager core: mode setting.
//
// The mode objects (connectors, encoders, crtcs, planes, properties,
// blobs, framebuffers), the legacy and atomic mode-setting ioctls, dumb
// buffers, cursors, page flips, dirty rectangles, and the vblank counter
// with its events.  What touches hardware goes through drm_driver.
//
// This file sets the device's mode-setting state up (drm_kms_init),
// releases what a closing file held, and routes the mode-setting ioctls,
// with the rights each one needs, to the files that implement them:
// drm_mode_object.c, drm_property.c, drm_crtc.c, drm_plane.c,
// drm_connector.c, drm_framebuffer.c, drm_vblank.c, drm_dumb_buffers.c and
// drm_lease.c.  The ATOMIC replay for drivers without atomic entry points
// stays here.
//
// drm_kms_init creates the core's properties with drm_property.c, and the
// functions below attach them to every crtc, plane and connector (see
// drm_mode_object.h); a closing file's blobs are released with its other
// mode-setting objects.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* ---- init ----------------------------------------------------------- */

static const struct drm_prop_enum_list drm_dpms_enum_list[] = {
	{ 0, "On" },
	{ 1, "Standby" },
	{ 2, "Suspend" },
	{ 3, "Off" },
};

static const struct drm_prop_enum_list drm_link_status_enum_list[] = {
	{ 0, "Good" },
	{ 1, "Bad" },
};

static const struct drm_prop_enum_list drm_plane_type_enum_list[] = {
	{ DRM_PLANE_TYPE_OVERLAY, "Overlay" },
	{ DRM_PLANE_TYPE_PRIMARY, "Primary" },
	{ DRM_PLANE_TYPE_CURSOR, "Cursor" },
};

/* The property's id, 0 when it could not be made (the device's table is
 * full: a driver bug, already warned about). */
static uint32_t prop_id_of(const struct drm_prop *p)
{
	return p ? p->id : 0;
}

void drm_kms_init(struct drm_device *dev)
{
	struct drm_prop *p;

	if (!dev->refresh_hz)
		dev->refresh_hz = 60;
	/* Once, here, and never again: the arming paths must not re-initialise
	 * a timer that may be queued.  See the vblank lifecycle comment. */
	for (int i = 0; i < DRM_MAX_CONNECTORS; i++)
		hrtimer_init(&dev->vbl[i].timer, drm_vbl_timer_fire, dev);
	/* The core's properties.  Clients cache property ids, and every one
	 * here takes the next mode-object id, so the order of creation is
	 * part of what clients see: keep it. */
	p = drm_property_create_enum(dev, 0, "DPMS", drm_dpms_enum_list,
				     ARRAY_SIZE(drm_dpms_enum_list));
	dev->prop_dpms = prop_id_of(p);
	p = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE,
				"EDID", 0);
	dev->prop_edid = prop_id_of(p);
	p = drm_property_create_object(dev, DRM_MODE_PROP_ATOMIC, "CRTC_ID",
				       DRM_MODE_OBJECT_CRTC);
	dev->prop_crtc_id = prop_id_of(p);
	p = drm_property_create_enum(dev, 0, "link-status",
				     drm_link_status_enum_list,
				     ARRAY_SIZE(drm_link_status_enum_list));
	dev->prop_link_status = prop_id_of(p);
	p = drm_property_create_bool(dev, DRM_MODE_PROP_IMMUTABLE, "non-desktop");
	dev->prop_non_desktop = prop_id_of(p);
	p = drm_property_create_enum(dev, DRM_MODE_PROP_IMMUTABLE, "type",
				     drm_plane_type_enum_list,
				     ARRAY_SIZE(drm_plane_type_enum_list));
	dev->prop_type = prop_id_of(p);
	p = drm_property_create_object(dev, DRM_MODE_PROP_ATOMIC, "FB_ID",
				       DRM_MODE_OBJECT_FB);
	dev->prop_fb_id = prop_id_of(p);
	p = drm_property_create_bool(dev, DRM_MODE_PROP_ATOMIC, "ACTIVE");
	dev->prop_active = prop_id_of(p);
	p = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC,
				"MODE_ID", 0);
	dev->prop_mode_id = prop_id_of(p);
	static const char *const srcn[] = { "SRC_X", "SRC_Y", "SRC_W", "SRC_H" };
	uint32_t *srcp[] = { &dev->prop_src_x, &dev->prop_src_y, &dev->prop_src_w,
			     &dev->prop_src_h };
	for (int i = 0; i < 4; i++) {
		p = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC, srcn[i],
					      0, 0xFFFFFFFFULL);
		*srcp[i] = prop_id_of(p);
	}
	static const char *const crtn[] = { "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H" };
	uint32_t *crtp[] = { &dev->prop_crtc_x, &dev->prop_crtc_y, &dev->prop_crtc_w,
			     &dev->prop_crtc_h };
	for (int i = 0; i < 4; i++) {
		if (i < 2)
			p = drm_property_create_signed_range(dev, DRM_MODE_PROP_ATOMIC,
							     crtn[i], -2147483648LL,
							     2147483647LL);
		else
			p = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
						      crtn[i], 0, 2147483647ULL);
		*crtp[i] = prop_id_of(p);
	}
	/* GAMMA_LUT and its size only where a driver takes the table through
	 * the atomic state.  Advertising the size is a promise: a display
	 * server that reads it switches to that path and asserts on the
	 * property.  A driver without the pair is asked through SETGAMMA. */
	if (dev->drv->atomic_commit && dev->drv->gamma_size) {
		p = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC,
					"GAMMA_LUT", 0);
		dev->prop_gamma_lut = prop_id_of(p);
		/* The size is each crtc's value of the property; the range
		 * only says what a size can be, so that two pipes of one
		 * device may differ (drm_crtc_enable_color_mgmt). */
		p = drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE |
						       DRM_MODE_PROP_ATOMIC,
					      "GAMMA_LUT_SIZE",
					      DRM_COLOR_LEGACY_GAMMA_SPLIT ? 0 : dev->drv->gamma_size,
					      DRM_COLOR_LEGACY_GAMMA_SPLIT ? 0xffffffffULL :
									     dev->drv->gamma_size);
		dev->prop_gamma_lut_size = prop_id_of(p);
	}
	p = drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE |
				     DRM_MODE_PROP_ATOMIC, "IN_FORMATS", 0);
	dev->prop_in_formats = prop_id_of(p);

	/* IN_FORMATS blobs: the driver's scanout formats, each usable with
	 * every one of its layout modifiers (the core's defaults when the
	 * driver lists none), and the cursor's. */
	{
		const uint32_t *formats = dev->drv->fb_formats ? dev->drv->fb_formats : drm_default_formats;
		uint32_t nformats = dev->drv->fb_formats ? dev->drv->nfb_formats : 3;
		const uint64_t *mods = dev->drv->fb_modifiers ? dev->drv->fb_modifiers : drm_linear_modifiers;
		uint32_t nmods = dev->drv->fb_modifiers ? dev->drv->nfb_modifiers : 1;
		dev->in_formats_blob = drm_in_formats_blob(dev, formats, nformats, mods, nmods);
		dev->in_formats_cursor_blob = drm_in_formats_blob(dev, drm_cursor_formats, 1,
								  drm_linear_modifiers, 1);
	}
	/* The optional properties, for a driver with atomic entry points
	 * that opted into them -- after everything above, so that every id
	 * a driver without them has seen stays what it was. */
	if (dev->drv->atomic_commit) {
		uint32_t features = dev->drv->features;

		if (features & DRM_FEATURE_ATOMIC_FENCES) {
			p = drm_property_create_signed_range(dev, DRM_MODE_PROP_ATOMIC,
							     "IN_FENCE_FD", -1,
							     2147483647LL);
			dev->prop_in_fence_fd = prop_id_of(p);
			p = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
						      "OUT_FENCE_PTR", 0, ~0ULL);
			dev->prop_out_fence_ptr = prop_id_of(p);
		}
		if (features & DRM_FEATURE_DAMAGE_CLIPS) {
			p = drm_property_create(dev, DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_BLOB,
						"FB_DAMAGE_CLIPS", 0);
			dev->prop_fb_damage_clips = prop_id_of(p);
		}
		if (features & DRM_FEATURE_COLOR_MGMT) {
			p = drm_property_create(dev, DRM_MODE_PROP_BLOB, "DEGAMMA_LUT", 0);
			dev->prop_degamma_lut = prop_id_of(p);
			p = drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE,
						      "DEGAMMA_LUT_SIZE", 0, 0xffffffffULL);
			dev->prop_degamma_lut_size = prop_id_of(p);
			p = drm_property_create(dev, DRM_MODE_PROP_BLOB, "CTM", 0);
			dev->prop_ctm = prop_id_of(p);
		}
		if (features & DRM_FEATURE_VRR) {
			p = drm_property_create_bool(dev, 0, "VRR_ENABLED");
			dev->prop_vrr_enabled = prop_id_of(p);
		}
	}
	dev->min_width = 64;
	dev->min_height = 64;
	if (!dev->max_width)
		dev->max_width = 8192;
	if (!dev->max_height)
		dev->max_height = 8192;
	/* Objects are normally added after this (their planes need the
	 * IN_FORMATS blobs above); any that exist already get the core's
	 * properties now, the rest on their first lookup. */
	for (uint32_t i = 0; i < dev->nconn; i++)
		drm_connector_attach_core_properties(dev, &dev->conn[i]);
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		drm_crtc_attach_core_properties(dev, &dev->crtc[i]);
	for (uint32_t i = 0; i < dev->nplanes; i++)
		drm_plane_attach_core_properties(dev, &dev->planes[i]);
	/* The connector properties every device may carry (PATH, TILE,
	 * HDR_OUTPUT_METADATA); created here only when the connector code is
	 * built to create them up front, otherwise on first attach. */
	drm_connector_create_standard_properties(dev);
}

/* ---- the core's properties on each object ------------------------------
 *
 * Attached in the order OBJ_GETPROPERTIES has always listed them; the
 * values here are only the initial ones -- the listing reads the core's
 * properties from the object itself (drm_mode_object_property_value). */

static void attach_id(struct drm_device *dev, struct drm_object_properties *props,
		      uint32_t prop_id, uint64_t init_val)
{
	struct drm_prop *p = drm_prop_find(dev, prop_id);

	if (p)
		drm_object_attach_property(props, p, init_val);
}

static void props_swap(struct drm_object_properties *props, int a, int b)
{
	uint32_t id = props->ids[a];
	uint64_t v = props->values[a];

	props->ids[a] = props->ids[b];
	props->values[a] = props->values[b];
	props->ids[b] = id;
	props->values[b] = v;
}

static void props_reverse(struct drm_object_properties *props, int from, int to)
{
	while (from < to)
		props_swap(props, from++, to--);
}

/* The entries [0, first_core) are a driver's, attached before the core's
 * got there; turn [driver..., core...] into [core..., driver...] keeping
 * each group's order. */
static void core_first(struct drm_object_properties *props, int first_core)
{
	if (first_core <= 0 || first_core >= props->count)
		return;
	props_reverse(props, 0, first_core - 1);
	props_reverse(props, first_core, props->count - 1);
	props_reverse(props, 0, props->count - 1);
}

void drm_connector_attach_core_properties(struct drm_device *dev,
					  struct drm_connector *conn)
{
	struct drm_object_properties *props = &conn->properties;
	int had = props->count;

	if (props->core_attached)
		return;
	props->core_attached = true;
	attach_id(dev, props, dev->prop_dpms, 0);
	attach_id(dev, props, dev->prop_crtc_id, 0);
	/* a link the driver marked bad before the list was made reads bad */
	attach_id(dev, props, dev->prop_link_status, conn->link_status);
	attach_id(dev, props, dev->prop_non_desktop, 0);
	/* listed only while the connector has an EDID (drm_mode_object.c) */
	attach_id(dev, props, dev->prop_edid, 0);
	core_first(props, had);
}

void drm_crtc_attach_core_properties(struct drm_device *dev,
				     struct drm_crtc *crtc)
{
	struct drm_object_properties *props = &crtc->properties;
	int had = props->count;

	if (props->core_attached)
		return;
	props->core_attached = true;
	attach_id(dev, props, dev->prop_active, 0);
	attach_id(dev, props, dev->prop_mode_id, 0);
	if (dev->prop_gamma_lut) {
		attach_id(dev, props, dev->prop_gamma_lut, 0);
		attach_id(dev, props, dev->prop_gamma_lut_size, dev->drv->gamma_size);
	}
	/* The optional ones, where drm_kms_init made them (opted in). */
	attach_id(dev, props, dev->prop_out_fence_ptr, 0);
	attach_id(dev, props, dev->prop_vrr_enabled, 0);
	/* DEGAMMA_LUT where the driver has a degamma table, CTM where its
	 * crtcs have a matrix (hardware without one -- the oldest display
	 * controllers -- lists none). */
	if (dev->prop_ctm)
		(void)drm_crtc_enable_color_mgmt(dev, crtc, dev->drv->degamma_size,
						 DRM_COLOR_CTM_BY_DRIVER ?
							 dev->drv->color_has_ctm != 0 : true,
						 0);
	core_first(props, had);
}

void drm_plane_attach_core_properties(struct drm_device *dev,
				      struct drm_plane *plane)
{
	struct drm_object_properties *props = &plane->properties;
	int had = props->count;

	if (props->core_attached)
		return;
	props->core_attached = true;
	attach_id(dev, props, dev->prop_type, plane->type);
	attach_id(dev, props, dev->prop_fb_id, 0);
	attach_id(dev, props, dev->prop_crtc_id, 0);
	attach_id(dev, props, dev->prop_src_x, 0);
	attach_id(dev, props, dev->prop_src_y, 0);
	attach_id(dev, props, dev->prop_src_w, 0);
	attach_id(dev, props, dev->prop_src_h, 0);
	attach_id(dev, props, dev->prop_crtc_x, 0);
	attach_id(dev, props, dev->prop_crtc_y, 0);
	attach_id(dev, props, dev->prop_crtc_w, 0);
	attach_id(dev, props, dev->prop_crtc_h, 0);
	attach_id(dev, props, dev->prop_in_formats, plane->in_formats_blob);
	/* The optional ones, where drm_kms_init made them (opted in).  An
	 * IN_FENCE_FD reads -1: there is never a fence to report. */
	attach_id(dev, props, dev->prop_in_fence_fd, (uint64_t)-1LL);
	attach_id(dev, props, dev->prop_fb_damage_clips, 0);
	core_first(props, had);
}

void drm_kms_file_release(struct drm_device *dev, struct drm_file *fp)
{
	/* Events this file was waiting for. */
	drm_vblank_file_release(dev, fp);
	/* Framebuffers it created (a master's are dropped with it). */
	drm_framebuffer_file_release(dev, fp);
	/* Blobs it created: its reference on each goes, and with it every
	 * blob no state or object still uses. */
	drm_property_destroy_user_blobs(dev, fp);
}

/* ---- ioctls ---------------------------------------------------------- */

/* ATOMIC: through drm_atomic.c for a driver with atomic entry points, else
 * replayed through the legacy paths */
static int drm_kms_atomic_legacy(struct drm_device *dev, void *kb,
				 struct drm_file *fp)
{
	struct drm_mode_atomic *a = kb;
	if (!((fp->client_caps >> DRM_CLIENT_CAP_ATOMIC) & 1))
		return -EINVAL;
	if (dev->drv->atomic_commit)
		return drm_atomic_ioctl(dev, fp, a);
	/* A driver without atomic entry points: the properties the
	 * older calls know are replayed through them. */
	if (a->flags & ~(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK |
			 DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT))
		return -EINVAL;
	/* Gather the per-object property lists and apply what the
	 * legacy paths know: FB_ID/CRTC_ID on a plane, ACTIVE and
	 * MODE_ID on a crtc, DPMS on a connector. */
	uint32_t nobj = a->count_objs;
	if (nobj > 16)
		return -EINVAL;
	uint32_t objs[16], objcnt[16];
	if (copy_from_user(objs, (void *)(uintptr_t)a->objs_ptr, nobj * 4) != 0 ||
	    copy_from_user(objcnt, (void *)(uintptr_t)a->count_props_ptr, nobj * 4) != 0)
		return -EFAULT;
	uint32_t pidx = 0;
	struct drm_crtc *flip_crtc = NULL;
	int rc = 0;
	for (uint32_t i = 0; i < nobj && rc == 0; i++) {
		for (uint32_t j = 0; j < objcnt[i]; j++, pidx++) {
			uint32_t pid;
			uint64_t val;
			if (copy_from_user(&pid, (void *)(uintptr_t)(a->props_ptr + pidx * 4), 4) != 0 ||
			    copy_from_user(&val, (void *)(uintptr_t)(a->prop_values_ptr + pidx * 8), 8) != 0)
				return -EFAULT;
			if (a->flags & DRM_MODE_ATOMIC_TEST_ONLY)
				continue;
			int cursor;
			struct drm_crtc *cr = drm_plane_crtc(dev, objs[i], &cursor);
			if (cr && !cursor && pid == dev->prop_fb_id) {
				if (!val) {
					rc = drm_crtc_set(dev, fp, cr, NULL, 0, 0, 0);
				} else {
					struct drm_framebuffer *fb = drm_fb_lookup(dev, (uint32_t)val);
					if (!fb) {
						rc = -ENOENT;
						break;
					}
					if (cr->active && dev->drv->page_flip) {
						rc = dev->drv->page_flip(dev, cr, fb);
						if (rc == 0)
							cr->fb_id = fb->id;
					} else {
						rc = drm_crtc_set(dev, fp, cr, &cr->mode, fb->id, 0, 0);
					}
				}
				flip_crtc = cr;
				continue;
			}
			struct drm_crtc *c2 = drm_crtc_find(dev, objs[i]);
			if (c2 && pid == dev->prop_mode_id && val) {
				/* referenced while the mode is copied out, so
				 * a destroy meanwhile cannot free it under us */
				struct drm_blob *b = val <= 0xffffffffULL ?
					drm_property_lookup_blob(dev, (uint32_t)val) : NULL;
				if (!b || b->length < sizeof(struct drm_mode_modeinfo)) {
					drm_property_blob_put(b);
					rc = -EINVAL;
					break;
				}
				mm_memcpy(&c2->mode, b->data, sizeof(c2->mode));
				drm_property_blob_put(b);
				if (c2->fb_id)
					rc = drm_crtc_set(dev, fp, c2, &c2->mode, c2->fb_id, c2->x, c2->y);
				continue;
			}
			if (c2 && pid == dev->prop_active) {
				if (!val)
					rc = drm_crtc_set(dev, fp, c2, NULL, 0, 0, 0);
				continue;
			}
			struct drm_connector *cn = drm_conn_find(dev, objs[i]);
			if (cn && pid == dev->prop_dpms) {
				cn->dpms = (int)val;
				if (dev->drv->dpms)
					dev->drv->dpms(dev, cn, (int)val);
			}
		}
	}
	if (rc == 0 && (a->flags & DRM_MODE_PAGE_FLIP_EVENT) &&
	    !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		struct drm_crtc *cr = flip_crtc ? flip_crtc : &dev->crtc[0];
		rc = drm_kms_queue_flip_event(dev, fp, cr->index, a->user_data);
	}
	return rc;
}

long drm_kms_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		   void *kb, unsigned size, int *handled)
{
	(void)size;
	*handled = 1;

	/* Everything below needs mode-setting rights except the pure
	 * queries, which any file on the primary node may use. */
	int query = (nr == 0xA0 || nr == 0xA1 || nr == 0xA6 || nr == 0xA7 ||
		     nr == 0xAA || nr == 0xAC || nr == 0xB5 || nr == 0xB9 ||
		     nr == 0xB6 || nr == 0x3a || nr == 0x3b || nr == 0xB2 ||
		     nr == 0xB3 || nr == 0xB4 || nr == 0xCE);
	if (!query && !fp->is_master && nr != 0x3c)
		return -EACCES;

	switch (nr) {
	case 0xA0: /* GETRESOURCES */
		return drm_mode_getresources(dev, kb, fp);
	case 0xA1: /* GETCRTC */
		return drm_mode_getcrtc(dev, kb, fp);
	case 0xA2: /* SETCRTC */
		return drm_mode_setcrtc(dev, kb, fp);
	case 0xA3: /* CURSOR */
	case 0xBB: /* CURSOR2 */
		return drm_mode_cursor_common(dev, kb, fp, nr);
	case 0xA4: /* GETGAMMA */
		return drm_mode_gamma_get_ioctl(dev, kb, fp);
	case 0xA5: /* SETGAMMA */
		return drm_mode_gamma_set_ioctl(dev, kb, fp);
	case 0xA6: /* GETENCODER */
		return drm_mode_getencoder(dev, kb, fp);
	case 0xA7: /* GETCONNECTOR */
		return drm_mode_getconnector(dev, kb, fp);
	case 0xAA: /* GETPROPERTY */
		return drm_mode_getproperty_ioctl(dev, kb, fp);
	case 0xAB: /* SETPROPERTY (connector) */
		return drm_connector_property_set_ioctl(dev, kb, fp);
	case 0xAC: /* GETPROPBLOB */
		return drm_mode_getblob_ioctl(dev, kb, fp);
	case 0xAE: /* ADDFB */
		return drm_mode_addfb_ioctl(dev, kb, fp);
	case 0xB8: /* ADDFB2 */
		return drm_mode_addfb2_ioctl(dev, kb, fp);
	case 0xAF: /* RMFB */
		return drm_mode_rmfb_ioctl(dev, kb, fp);
	case 0xD0: /* CLOSEFB: rights as RMFB (not a query) */
		return drm_mode_closefb_ioctl(dev, kb, fp);
	case 0xAD: /* GETFB */
		return drm_mode_getfb(dev, kb, fp);
	case 0xCE: /* GETFB2 */
		return drm_mode_getfb2_ioctl(dev, kb, fp);
	case 0xB0: /* PAGE_FLIP */
		return drm_mode_page_flip_ioctl(dev, kb, fp);
	case 0xB1: /* DIRTYFB */
		return drm_mode_dirtyfb_ioctl(dev, kb, fp);
	case 0xB2: /* CREATE_DUMB */
		return drm_mode_create_dumb_ioctl(dev, kb, fp);
	case 0xB3: /* MAP_DUMB */
		return drm_mode_mmap_dumb_ioctl(dev, kb, fp);
	case 0xB4: /* DESTROY_DUMB */
		return drm_mode_destroy_dumb_ioctl(dev, kb, fp);
	case 0xB5: /* GETPLANERESOURCES */
		return drm_mode_getplane_res(dev, kb, fp);
	case 0xB6: /* GETPLANE */
		return drm_mode_getplane(dev, kb, fp);
	case 0xB7: /* SETPLANE */
		return drm_mode_setplane(dev, kb, fp);
	case 0xB9: /* OBJ_GETPROPERTIES */
		return drm_mode_obj_get_properties_ioctl(dev, kb, fp);
	case 0xBA: /* OBJ_SETPROPERTY */
		return drm_mode_obj_set_property_ioctl(dev, kb, fp);
	case 0xBD: /* CREATEPROPBLOB */
		return drm_mode_createblob_ioctl(dev, kb, fp);
	case 0xBE: /* DESTROYPROPBLOB */
		return drm_mode_destroyblob_ioctl(dev, kb, fp);
	case 0xBC: /* ATOMIC */
		return drm_kms_atomic_legacy(dev, kb, fp);
	case 0x3a: /* WAIT_VBLANK */
		return drm_wait_vblank_ioctl(dev, kb, fp);
	case 0x3b: /* CRTC_GET_SEQUENCE */
		return drm_crtc_get_sequence_ioctl(dev, kb, fp);
	case 0x3c: /* CRTC_QUEUE_SEQUENCE */
		return drm_crtc_queue_sequence_ioctl(dev, kb, fp);
	case 0xC6: /* CREATE_LEASE */
		return drm_mode_create_lease_ioctl(dev, kb, fp);
	case 0xC7: /* LIST_LESSEES */
		return drm_mode_list_lessees_ioctl(dev, kb, fp);
	case 0xC8: /* GET_LEASE */
		return drm_mode_get_lease_ioctl(dev, kb, fp);
	case 0xC9: /* REVOKE_LEASE */
		return drm_mode_revoke_lease_ioctl(dev, kb, fp);
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
