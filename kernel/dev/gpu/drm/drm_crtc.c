// LikeOS -- display-manager core: crtcs.
//
// A crtc scans a framebuffer out with a mode.  The legacy mode set
// (drm_crtc_set) goes to the driver's mode_set / crtc_disable, or through an
// atomic state for a driver with atomic entry points; the kernel console
// uses the same path without a file.  The crtc ioctls: GETRESOURCES,
// GETCRTC, SETCRTC, CURSOR / CURSOR2, GETGAMMA, SETGAMMA and PAGE_FLIP.
//
// A crtc with a mode is enabled whether or not DPMS lets its pipe run, and
// GETCRTC reports its mode either way; the legacy gamma table is the crtc's
// own size (256), whatever its atomic GAMMA_LUT takes.  After a resume a
// driver without atomic entry points is given back what the legacy calls
// had set (drm_legacy_replay).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's, Dave Airlie's and Red Hat's code: MIT
// Portions Copyright (c) 2006-2008 Intel Corporation
// Portions Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
// Portions Copyright (c) 2008 Red Hat Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		if (dev->crtc[i].id == id)
			return &dev->crtc[i];
	return NULL;
}

struct drm_gem_object *drm_crtc_scanout(struct drm_device *dev, int crtc)
{
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return NULL;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, dev->crtc[crtc].fb_id);
	return fb ? fb->obj : NULL;
}

int drm_kms_crtc_set_kernel(struct drm_device *dev, uint32_t crtc_index,
			    const struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
	if (crtc_index >= dev->ncrtc)
		return -ENODEV;
	return drm_crtc_set(dev, NULL, &dev->crtc[crtc_index], mode, fb_id, 0, 0);
}

int drm_crtc_set(struct drm_device *dev, struct drm_file *fp,
		 struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode,
		 uint32_t fb_id, int x, int y)
{
	if (dev->drv->atomic_commit)
		return drm_atomic_legacy_crtc_set(dev, fp, crtc, mode, fb_id, x, y,
						  NULL, 0);
	if (!fb_id || !mode) {
		crtc->fb_id = 0;
		crtc->active = 0;
		crtc->enabled = 0;
		if (dev->drv->crtc_disable)
			dev->drv->crtc_disable(dev, crtc);
		drm_kms_vbl_sync(dev, crtc->index);
		return 0;
	}
	struct drm_framebuffer *fb = drm_fb_lookup(dev, fb_id);
	if (!fb)
		return -ENOENT;
	if (x < 0 || y < 0 || (uint32_t)(mode->hdisplay + x) > fb->width ||
	    (uint32_t)(mode->vdisplay + y) > fb->height)
		return -ENOSPC;
	int rc = dev->drv->mode_set ? dev->drv->mode_set(dev, crtc, mode, fb, x, y) : -ENODEV;
	if (rc)
		return rc;
	crtc->mode = *mode;
	crtc->fb_id = fb_id;
	crtc->x = x;
	crtc->y = y;
	crtc->active = 1;
	crtc->enabled = 1;
	dev->conn[crtc->index].crtc_id = crtc->id;
	dev->enc[crtc->index].crtc_id = crtc->id;
	drm_kms_vbl_sync(dev, crtc->index);
	return 0;
}

/*
 * After a resume, for a driver without atomic entry points: what the legacy
 * calls had set is set again in the order they set it.  Every running crtc
 * gets its mode, framebuffer and position, then its cursor image and
 * place; last each connector that a client had switched off with DPMS is
 * switched off again.  The first error is returned, and the rest is still
 * tried.
 */
int drm_legacy_replay(struct drm_device *dev)
{
	const struct drm_driver *drv = dev->drv;
	int rc = 0;

	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc *c = &dev->crtc[i];
		struct drm_framebuffer *fb;
		int r;

		if (!c->active || !c->fb_id)
			continue;
		fb = drm_fb_lookup(dev, c->fb_id);
		if (!fb)
			continue;
		r = drv->mode_set ? drv->mode_set(dev, c, &c->mode, fb, c->x, c->y) : -ENODEV;
		if (r) {
			if (!rc)
				rc = r;
			continue;
		}
		drm_kms_vbl_sync(dev, (int)i);
		if (c->cursor_obj && drv->cursor_set) {
			r = drv->cursor_set(dev, c, c->cursor_obj, c->cursor_handle_w,
					    c->cursor_handle_h, c->cursor_hot_x,
					    c->cursor_hot_y);
			if (r && !rc)
				rc = r;
			if (r == 0 && drv->cursor_move)
				(void)drv->cursor_move(dev, c, c->cursor_x, c->cursor_y);
		}
	}
	if (drv->dpms) {
		for (uint32_t i = 0; i < dev->nconn; i++) {
			struct drm_connector *cn = &dev->conn[i];

			if (cn->crtc_id && cn->dpms != 0)
				(void)drv->dpms(dev, cn, cn->dpms);
		}
	}
	return rc;
}

/* GETRESOURCES */
int drm_mode_getresources(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_card_res *r = kb;
	uint32_t ids[DRM_MAX_CONNECTORS];
	uint32_t fbids[DRM_MAX_FBS];
	uint32_t nfb = 0;
	int rc;
	for (int i = 0; i < DRM_MAX_FBS; i++)
		if (dev->fbs[i].id && dev->fbs[i].owner == fp)
			fbids[nfb++] = dev->fbs[i].id;
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		ids[i] = dev->crtc[i].id;
	if ((rc = drm_copy_ids(r->crtc_id_ptr, r->count_crtcs, ids, dev->ncrtc)))
		return rc;
	for (uint32_t i = 0; i < dev->nconn; i++)
		ids[i] = dev->conn[i].id;
	if ((rc = drm_copy_ids(r->connector_id_ptr, r->count_connectors, ids, dev->nconn)))
		return rc;
	for (uint32_t i = 0; i < dev->nenc; i++)
		ids[i] = dev->enc[i].id;
	if ((rc = drm_copy_ids(r->encoder_id_ptr, r->count_encoders, ids, dev->nenc)))
		return rc;
	if ((rc = drm_copy_ids(r->fb_id_ptr, r->count_fbs, fbids, nfb)))
		return rc;
	r->count_crtcs = dev->ncrtc;
	r->count_connectors = dev->nconn;
	r->count_encoders = dev->nenc;
	r->count_fbs = nfb;
	r->min_width = dev->min_width;
	r->min_height = dev->min_height;
	r->max_width = dev->max_width;
	r->max_height = dev->max_height;
	return 0;
}

/* GETCRTC */
int drm_mode_getcrtc(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_crtc *c = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, c->crtc_id);
	if (!cr)
		return -ENOENT;
	c->fb_id = cr->fb_id;
	c->x = (uint32_t)cr->x;
	c->y = (uint32_t)cr->y;
	c->gamma_size = drm_crtc_legacy_gamma_size(dev, cr);
	/* the mode is reported while the crtc has one, also while DPMS keeps
	 * its pipe off */
	c->mode_valid = DRM_ATOMIC_ENABLE_SEMANTICS ? (cr->enabled || cr->active) : cr->active;
	c->mode = cr->mode;
	/* a client may have set the timing without its rate */
	if (c->mode_valid && !c->mode.vrefresh && c->mode.htotal && c->mode.vtotal) {
		uint64_t den = (uint64_t)c->mode.htotal * c->mode.vtotal;
		uint64_t num = (uint64_t)c->mode.clock * 1000;
		if (c->mode.flags & DRM_MODE_FLAG_INTERLACE)
			num *= 2;
		if (c->mode.flags & DRM_MODE_FLAG_DBLSCAN)
			den *= 2;
		c->mode.vrefresh = (uint32_t)((num + den / 2) / den);
	}
	c->count_connectors = 0;
	return 0;
}

/* SETCRTC */
int drm_mode_setcrtc(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_crtc *c = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, c->crtc_id);
	if (!cr)
		return -ENOENT;
	uint32_t cids[DRM_MAX_CONNECTORS];
	uint32_t ncid = 0;
	if (c->count_connectors > 0 && c->set_connectors_ptr) {
		ncid = c->count_connectors;
		if (ncid > DRM_MAX_CONNECTORS)
			return -EINVAL;
		if (!validate_user_ptr(c->set_connectors_ptr, ncid * 4) ||
		    copy_from_user(cids, (void *)(uintptr_t)c->set_connectors_ptr, ncid * 4) != 0)
			return -EFAULT;
		for (uint32_t k = 0; k < ncid; k++)
			if (!drm_conn_find(dev, cids[k]))
				return -ENOENT;
	}
	if (!c->mode_valid || !c->fb_id)
		return drm_crtc_set(dev, fp, cr, NULL, 0, 0, 0);
	if (dev->drv->atomic_commit)
		return drm_atomic_legacy_crtc_set(dev, fp, cr, &c->mode, c->fb_id,
						  (int)c->x, (int)c->y, cids, ncid);
	return drm_crtc_set(dev, fp, cr, &c->mode, c->fb_id, (int)c->x, (int)c->y);
}

/* CURSOR and CURSOR2 (`nr' says which: CURSOR2 carries a hot spot) */
int drm_mode_cursor_common(struct drm_device *dev, void *kb,
			   struct drm_file *fp, unsigned nr)
{
	struct drm_mode_cursor2 *c = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, c->crtc_id);
	if (!cr)
		return -ENOENT;
	if (dev->drv->atomic_commit) {
		struct drm_gem_object *o = NULL;
		if ((c->flags & DRM_MODE_CURSOR_BO) && c->handle) {
			o = drm_gem_lookup(fp, c->handle);
			if (!o)
				return -ENOENT;
		}
		int rc = drm_atomic_legacy_cursor(dev, fp, cr, c->flags, o,
						  c->width, c->height,
						  nr == 0xBB ? c->hot_x : 0,
						  nr == 0xBB ? c->hot_y : 0,
						  c->x, c->y);
		if (o)
			drm_gem_put(o); /* the wrapper framebuffer holds it */
		return rc;
	}
	if (c->flags & DRM_MODE_CURSOR_BO) {
		struct drm_gem_object *o = NULL;
		if (c->handle) {
			o = drm_gem_lookup(fp, c->handle);
			if (!o)
				return -ENOENT;
		}
		int32_t hx = nr == 0xBB ? c->hot_x : 0;
		int32_t hy = nr == 0xBB ? c->hot_y : 0;
		int rc = dev->drv->cursor_set ?
				 dev->drv->cursor_set(dev, cr, o, c->width, c->height, hx, hy) :
				 -ENODEV;
		if (cr->cursor_obj)
			drm_gem_put(cr->cursor_obj);
		cr->cursor_obj = o; /* keeps the reference */
		cr->cursor_handle_w = c->width;
		cr->cursor_handle_h = c->height;
		cr->cursor_hot_x = hx;
		cr->cursor_hot_y = hy;
		if (rc)
			return rc;
	}
	if (c->flags & DRM_MODE_CURSOR_MOVE) {
		cr->cursor_x = c->x;
		cr->cursor_y = c->y;
		if (dev->drv->cursor_move)
			return dev->drv->cursor_move(dev, cr, c->x, c->y);
	}
	return 0;
}

/* GETGAMMA: the crtc's legacy table, exactly its size of entries. */
int drm_mode_gamma_get_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_crtc_lut *l = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, l->crtc_id);
	if (!cr)
		return -ENOENT;
	uint32_t n = drm_crtc_legacy_gamma_size(dev, cr);
	/* the table size must match */
	if (l->gamma_size != n || n > 256)
		return -EINVAL;
	uint32_t bytes = n * (uint32_t)sizeof(uint16_t);
	if (dev->drv->atomic_commit && n != 256) {
		/* An atomic driver keeps the table resampled onto 256 entries
		 * (drm_atomic_legacy_gamma): read back at the points the
		 * client's entries went to. */
		uint16_t *t = kalloc(3 * 512);
		int rc = 0;

		if (!t)
			return -ENOMEM;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t k = (i * 255 + (n - 1) / 2) / (n - 1);
			t[i] = cr->gamma[0][k];
			t[256 + i] = cr->gamma[1][k];
			t[512 + i] = cr->gamma[2][k];
		}
		if (copy_to_user((void *)(uintptr_t)l->red, t, bytes) != 0 ||
		    copy_to_user((void *)(uintptr_t)l->green, t + 256, bytes) != 0 ||
		    copy_to_user((void *)(uintptr_t)l->blue, t + 512, bytes) != 0)
			rc = -EFAULT;
		kfree(t);
		return rc;
	}
	if (copy_to_user((void *)(uintptr_t)l->red, cr->gamma[0], bytes) != 0 ||
	    copy_to_user((void *)(uintptr_t)l->green, cr->gamma[1], bytes) != 0 ||
	    copy_to_user((void *)(uintptr_t)l->blue, cr->gamma[2], bytes) != 0)
		return -EFAULT;
	return 0;
}

/* SETGAMMA: the crtc's legacy table, exactly its size of entries. */
int drm_mode_gamma_set_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp)
{
	struct drm_mode_crtc_lut *l = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, l->crtc_id);
	if (!cr)
		return -ENOENT;
	uint32_t n = drm_crtc_legacy_gamma_size(dev, cr);
	if (l->gamma_size != n || n > 256)
		return -EINVAL;
	uint32_t bytes = n * (uint32_t)sizeof(uint16_t);
	if (dev->drv->atomic_commit) {
		uint16_t *t = kalloc(3 * 512);
		if (!t)
			return -ENOMEM;
		if (copy_from_user(t, (void *)(uintptr_t)l->red, bytes) != 0 ||
		    copy_from_user(t + 256, (void *)(uintptr_t)l->green, bytes) != 0 ||
		    copy_from_user(t + 512, (void *)(uintptr_t)l->blue, bytes) != 0) {
			kfree(t);
			return -EFAULT;
		}
		int rc = drm_atomic_legacy_gamma(dev, fp, cr, t, t + 256, t + 512, n);
		kfree(t);
		return rc;
	}
	if (copy_from_user(cr->gamma[0], (void *)(uintptr_t)l->red, bytes) != 0 ||
	    copy_from_user(cr->gamma[1], (void *)(uintptr_t)l->green, bytes) != 0 ||
	    copy_from_user(cr->gamma[2], (void *)(uintptr_t)l->blue, bytes) != 0)
		return -EFAULT;
	return 0; /* the virtual display has no LUT: accepted */
}

/* PAGE_FLIP */
int drm_mode_page_flip_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp)
{
	struct drm_mode_crtc_page_flip_target *f = kb;
	struct drm_crtc *cr = drm_crtc_find(dev, f->crtc_id);
	if (!cr)
		return -ENOENT;
	if (f->flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC))
		return -EINVAL;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, f->fb_id);
	if (!fb)
		return -ENOENT;
	if (!cr->active)
		return -EINVAL;
	if (dev->drv->atomic_commit)
		return drm_atomic_legacy_page_flip(dev, fp, cr, fb,
						   !!(f->flags & DRM_MODE_PAGE_FLIP_EVENT),
						   f->user_data);
	int rc = dev->drv->page_flip ? dev->drv->page_flip(dev, cr, fb) : -EINVAL;
	if (rc)
		return rc;
	cr->fb_id = fb->id;
	if (f->flags & DRM_MODE_PAGE_FLIP_EVENT)
		return drm_kms_queue_flip_event(dev, fp, cr->index, f->user_data);
	return 0;
}
