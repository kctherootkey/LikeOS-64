// LikeOS -- display-manager core: dumb buffers.
//
// The driver-independent way to get a buffer a client can draw into with
// the processor and scan out: CREATE_DUMB allocates a linear, page-backed
// object sized and pitched for the request, MAP_DUMB gives the offset to
// mmap it at, DESTROY_DUMB drops the handle.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* CREATE_DUMB */
int drm_mode_create_dumb_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp)
{
	struct drm_mode_create_dumb *c = kb;
	if (c->bpp != 32 && c->bpp != 16)
		return -EINVAL;
	if (c->width == 0 || c->height == 0 || c->width > 16384 || c->height > 16384)
		return -EINVAL;
	uint32_t pitch = (c->width * (c->bpp / 8) + 63) & ~63u;
	uint64_t size = (uint64_t)pitch * c->height;
	struct drm_gem_object *o = drm_gem_alloc(dev, DRM_GEM_BO, size);
	if (!o)
		return -ENOMEM;
	o->scanout = 1;
	o->width = c->width;
	o->height = c->height;
	o->pitch = pitch;
	o->format = c->bpp == 32 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_RGB565;
	int rc = drm_gem_alloc_pages(o);
	if (rc == 0 && dev->drv->gem_init)
		rc = dev->drv->gem_init(o);
	if (rc) {
		drm_gem_put(o);
		return rc;
	}
	uint32_t h;
	rc = drm_gem_handle_create(fp, o, &h);
	drm_gem_put(o);
	if (rc)
		return rc;
	c->handle = h;
	c->pitch = pitch;
	c->size = size;
	return 0;
}

/* MAP_DUMB */
int drm_mode_mmap_dumb_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp)
{
	(void)dev;
	struct drm_mode_map_dumb *m = kb;
	struct drm_gem_object *o = drm_gem_lookup(fp, m->handle);
	if (!o)
		return -ENOENT;
	m->offset = drm_gem_mmap_offset(o);
	drm_gem_put(o);
	return 0;
}

/* DESTROY_DUMB */
int drm_mode_destroy_dumb_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp)
{
	(void)dev;
	struct drm_mode_destroy_dumb *d = kb;
	return drm_gem_handle_delete(fp, d->handle);
}
