// LikeOS -- display-manager core: framebuffers.
//
// A framebuffer is one to three buffer objects described as an image:
// size, format, and per plane of the format a pitch, an offset and the
// object it lives in, plus the layout modifier the planes share.  A
// request is checked against the format table (drm_fourcc.c) -- every
// plane's pitch at least a line of its pixels, every plane inside its
// object, the modifier rules -- and against the driver's scanout formats
// and layouts.  Client framebuffers are owned by the file that made them
// and go with it; the kernel console and the cursor wrapper make their
// own, owned by nobody.  Removing one takes it off whatever shows it;
// closing one only lets go of it, and it goes once nothing shows it any
// more.  The framebuffer ioctls: ADDFB, ADDFB2, RMFB, CLOSEFB, GETFB,
// GETFB2 and DIRTYFB.  Also the format lookups that ask the device.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's and Laurent Pinchart's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation
// Portions Copyright (C) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_fourcc.h>
#include <kernel/dev/gpu/drm_internal.h>
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

/*
 * RMFB removes only a framebuffer the calling file created; anything else
 * -- another client's, the console's, the cursor wrapper's -- is "no such
 * framebuffer" to it.  Without the check a display server could take the
 * console's framebuffer away under it, and the object reference the
 * console's framebuffer holds would be dropped behind its back.  0 goes
 * back to removing any framebuffer by id.
 */
#define DRM_FB_RMFB_OWNER_ONLY 1

/* The largest value the 32-bit pitch / offset arithmetic of a plane may
 * reach. */
#define DRM_FB_U32_MAX 0xffffffffull

/* DIRTYFB rectangles per driver call when no buffer for all of them can be
 * had (on the stack: 16 clips and 16 rectangles, 384 bytes). */
#define DRM_FB_DIRTY_CHUNK 16u

/* Framebuffers a file closed (CLOSEFB) while they were still shown, over
 * all devices.  While it is 0 nothing has to be swept for them. */
static uint32_t g_fb_closed;

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id)
{
	if (!id)
		return NULL;
	for (int i = 0; i < DRM_MAX_FBS; i++)
		if (dev->fbs[i].id == id)
			return &dev->fbs[i];
	return NULL;
}

/* ---- formats, as this device sees them ---------------------------------- */

/* The driver's scanout formats bound what a framebuffer may carry; the
 * default set is XR24/AR24/RG16. */
static bool drv_has_format(struct drm_device *dev, uint32_t format)
{
	const uint32_t *formats = dev->drv->fb_formats ? dev->drv->fb_formats : drm_default_formats;
	uint32_t n = dev->drv->fb_formats ? dev->drv->nfb_formats : (uint32_t)ARRAY_SIZE(drm_default_formats);

	for (uint32_t i = 0; i < n; i++)
		if (formats[i] == format)
			return true;
	return false;
}

/* The same for layout modifiers; the default is the linear layout. */
static bool drv_has_modifier(struct drm_device *dev, uint64_t modifier)
{
	const uint64_t *mods = dev->drv->fb_modifiers ? dev->drv->fb_modifiers : drm_linear_modifiers;
	uint32_t n = dev->drv->fb_modifiers ? dev->drv->nfb_modifiers : (uint32_t)ARRAY_SIZE(drm_linear_modifiers);

	for (uint32_t i = 0; i < n; i++)
		if (mods[i] == modifier)
			return true;
	return false;
}

/* The description a framebuffer of this format and layout gets on this
 * device: the driver's own, for a layout it describes differently (a
 * compressed one with an extra plane, say), else the generic table's. */
const struct drm_format_info *drm_get_format_info(struct drm_device *dev,
						  uint32_t pixel_format,
						  uint64_t modifier)
{
	const struct drm_format_info *info = NULL;

	if (dev->drv->get_format_info)
		info = dev->drv->get_format_info(pixel_format, modifier);

	if (!info)
		info = drm_format_info(pixel_format);

	return info;
}

/* The format the legacy ADDFB means by bpp/depth on this device.  The
 * processor is little endian, so the host-order formats are the plain
 * ones.  A device that scans out 10 bits per channel only in BGR order
 * gets that order for the 30-bit depth -- an old client cannot name the
 * order, and RGB would be refused. */
uint32_t drm_driver_legacy_fb_format(struct drm_device *dev, uint32_t bpp,
				     uint32_t depth)
{
	uint32_t fmt = drm_mode_legacy_fb_format(bpp, depth);

	if (fmt == DRM_FORMAT_XRGB2101010 && !drv_has_format(dev, fmt) &&
	    drv_has_format(dev, DRM_FORMAT_XBGR2101010))
		fmt = DRM_FORMAT_XBGR2101010;

	return fmt;
}

/* The format a colour mode (bits per pixel and depth in one number, as a
 * command line gives it) means: 15 is 16 bits with depth 15, 32 is depth
 * 24; everything else has bpp == depth. */
uint32_t drm_driver_color_mode_format(struct drm_device *dev,
				      unsigned int color_mode)
{
	switch (color_mode) {
	case 15:
		return drm_driver_legacy_fb_format(dev, 16, 15);
	case 32:
		return drm_driver_legacy_fb_format(dev, 32, 24);
	default:
		return drm_driver_legacy_fb_format(dev, color_mode, color_mode);
	}
}

/* The bits per pixel GETFB reports: plane 0's, or 0 for a block format
 * whose block holds no whole number of bits per pixel (asking
 * drm_format_info_bpp() for that would warn). */
static uint32_t fb_legacy_bpp(const struct drm_format_info *info)
{
	unsigned int block = drm_format_info_block_width(info, 0) *
			     drm_format_info_block_height(info, 0);

	if (!block || info->char_per_block[0] * 8 % block)
		return 0;
	return drm_format_info_bpp(info, 0);
}

/* ---- framebuffers ------------------------------------------------------ */

/*
 * The request against the format's description, plane by plane: a handle
 * for every plane the format has, a pitch that holds a line of that
 * plane's pixels, offsets and sizes that fit the 32-bit fields, one
 * modifier for all planes and only with DRM_MODE_FB_MODIFIERS, and
 * nothing named for planes the format does not have.
 */
static int framebuffer_check(struct drm_device *dev,
			     const struct drm_format_info *info,
			     const struct drm_mode_fb_cmd2 *r)
{
	int i;

	(void)dev;
	if (r->width == 0)
		return -EINVAL;

	if (r->height == 0)
		return -EINVAL;

	for (i = 0; i < info->num_planes; i++) {
		unsigned int width = drm_format_info_plane_width(info, r->width, i);
		unsigned int height = drm_format_info_plane_height(info, r->height, i);
		unsigned int block_size = info->char_per_block[i];
		uint64_t min_pitch = drm_format_info_min_pitch(info, i, width);

		/* A format with no bytes per block in the table exists only
		 * in layouts the table cannot measure. */
		if (!block_size && (r->modifier[i] == DRM_FORMAT_MOD_LINEAR))
			return -EINVAL;

		if (!r->handles[i])
			return -EINVAL;

		if (min_pitch > DRM_FB_U32_MAX)
			return -ERANGE;

		if ((uint64_t)height * r->pitches[i] + r->offsets[i] > DRM_FB_U32_MAX)
			return -ERANGE;

		if (block_size && r->pitches[i] < min_pitch)
			return -EINVAL;

		if (r->modifier[i] && !(r->flags & DRM_MODE_FB_MODIFIERS))
			return -EINVAL;

		if (r->flags & DRM_MODE_FB_MODIFIERS &&
		    r->modifier[i] != r->modifier[0])
			return -EINVAL;

		/* modifier specific checks: */
		switch (r->modifier[i]) {
		case DRM_FORMAT_MOD_SAMSUNG_64_32_TILE:
			/* 64x32 tiles of NV12, whole tiles per line and per
			 * column, and a pitch of whole 128-byte units */
			if (r->pixel_format != DRM_FORMAT_NV12 ||
			    width % 128 || height % 32 ||
			    r->pitches[i] % 128)
				return -EINVAL;
			break;

		default:
			break;
		}
	}

	for (i = info->num_planes; i < 4; i++) {
		if (r->modifier[i])
			return -EINVAL;

		/* Clients that predate DRM_MODE_FB_MODIFIERS did not clear
		 * the unused entries; only a client that sets the flag is
		 * held to them being zero. */
		if (!(r->flags & DRM_MODE_FB_MODIFIERS))
			continue;

		if (r->handles[i])
			return -EINVAL;

		if (r->pitches[i])
			return -EINVAL;

		if (r->offsets[i])
			return -EINVAL;
	}

	return 0;
}

/* Is the framebuffer on screen: a plane's, a crtc's or a crtc's cursor? */
static bool fb_in_use(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nplanes; i++)
		if (dev->planes[i].fb_id == id)
			return true;
	for (uint32_t c = 0; c < dev->ncrtc; c++)
		if (dev->crtc[c].fb_id == id || dev->crtc[c].cursor_fb_id == id)
			return true;
	return false;
}

/* The slot goes back: the plane-0 object's reference, and one each for
 * the objects of planes 1-3 (taken per plane even where a plane shares
 * plane 0's object). */
static void fb_free(struct drm_framebuffer *fb)
{
	if (fb->obj)
		drm_gem_put(fb->obj);
	fb->obj = NULL;
	fb->objs[0] = NULL;
	for (unsigned int p = 1; p < DRM_FORMAT_MAX_PLANES; p++) {
		if (fb->objs[p])
			drm_gem_put(fb->objs[p]);
		fb->objs[p] = NULL;
	}
	if (fb->closed) {
		fb->closed = 0;
		g_fb_closed--;
	}
	fb->id = 0;
}

/* Framebuffers their files closed while shown, and that nothing shows any
 * more, go now. */
static void fb_reap_closed(struct drm_device *dev)
{
	if (!g_fb_closed)
		return;
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		struct drm_framebuffer *fb = &dev->fbs[i];

		if (fb->id && fb->closed && !fb_in_use(dev, fb->id))
			fb_free(fb);
	}
}

int drm_fb_create(struct drm_device *dev, struct drm_file *fp,
		  struct drm_mode_fb_cmd2 *r, uint32_t *id_out)
{
	struct drm_gem_object *objs[DRM_FORMAT_MAX_PLANES] = { NULL, NULL, NULL, NULL };
	const struct drm_format_info *info;
	uint64_t modifier;
	unsigned int p;
	int rc;

	if (r->flags & ~(DRM_MODE_FB_INTERLACED | DRM_MODE_FB_MODIFIERS))
		return -EINVAL;
	if (r->width < dev->min_width || r->height < dev->min_height ||
	    r->width > dev->max_width || r->height > dev->max_height)
		return -EINVAL;
	/* check if the format is known at all */
	if (!__drm_format_info(r->pixel_format))
		return -EINVAL;
	/* now let the driver pick its own description */
	info = drm_get_format_info(dev, r->pixel_format, r->modifier[0]);
	rc = framebuffer_check(dev, info, r);
	if (rc)
		return rc;

	/* The device: one of its scanout formats, and with
	 * DRM_MODE_FB_MODIFIERS one of its layouts (the check above made
	 * every plane's modifier the same). */
	if (!drv_has_format(dev, r->pixel_format))
		return -EINVAL;
	modifier = DRM_FORMAT_MOD_INVALID;
	if (r->flags & DRM_MODE_FB_MODIFIERS) {
		modifier = r->modifier[0];
		if (!drv_has_modifier(dev, modifier))
			return -EINVAL;
	}

	/* The objects, one lookup reference per plane, and every plane
	 * inside its object: offset plus a full pitch for every line of the
	 * plane. */
	for (p = 0; p < info->num_planes; p++) {
		uint32_t height = (uint32_t)drm_format_info_plane_height(info, (int)r->height, (int)p);

		objs[p] = drm_gem_lookup(fp, r->handles[p]);
		if (!objs[p]) {
			rc = -ENOENT;
			goto err;
		}
		if ((uint64_t)r->offsets[p] + (uint64_t)r->pitches[p] * height > objs[p]->size) {
			rc = -EINVAL;
			goto err;
		}
	}
	if (dev->drv->fb_check) {
		rc = dev->drv->fb_check(dev, objs[0], r, &modifier);
		if (rc)
			goto err;
	} else {
		modifier = DRM_FORMAT_MOD_LINEAR;
	}

	fb_reap_closed(dev);
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id)
			continue;
		struct drm_framebuffer *fb = &dev->fbs[i];
		/* a reused slot starts clean: nothing of the previous
		 * framebuffer's planes may show through */
		mm_memset(fb, 0, sizeof(*fb));
		fb->id = drm_mode_id_alloc(dev);
		fb->width = r->width;
		fb->height = r->height;
		fb->pitch = r->pitches[0];
		fb->offset = r->offsets[0];
		fb->format = r->pixel_format;
		fb->bpp = fb_legacy_bpp(info);
		fb->depth = info->depth;
		fb->modifier = modifier;
		fb->obj = objs[0]; /* keeps the lookup reference */
		fb->owner = fp;
		fb->format_info = info;
		for (p = 0; p < info->num_planes; p++) {
			fb->pitches[p] = r->pitches[p];
			fb->offsets[p] = r->offsets[p];
			/* objs[0] is `obj' itself; planes 1-3 keep their own
			 * lookup references */
			fb->objs[p] = objs[p];
		}
		*id_out = fb->id;
		return 0;
	}
	rc = -ENOSPC;
err:
	for (p = 0; p < DRM_FORMAT_MAX_PLANES; p++)
		if (objs[p])
			drm_gem_put(objs[p]);
	return rc;
}

/* ---- framebuffers and mode sets owned by the KERNEL ------------------- */
/*
 * The in-kernel console client (drm_console.c) needs both, and neither can
 * go through the ioctl paths: it has no drm_file to own a handle, its
 * framebuffer has to outlive every client that comes and goes, and it must
 * not show up in any client's resource list -- GETRESOURCES filters
 * framebuffers on the owning file, and a NULL owner matches none of them.
 */
int drm_kms_fb_add_internal(struct drm_device *dev, struct drm_gem_object *o,
			    uint32_t w, uint32_t h, uint32_t pitch,
			    uint32_t format, uint32_t *id_out)
{
	const struct drm_format_info *info = __drm_format_info(format);

	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id)
			continue;
		struct drm_framebuffer *fb = &dev->fbs[i];
		mm_memset(fb, 0, sizeof(*fb));
		fb->id = drm_mode_id_alloc(dev);
		fb->width = w;
		fb->height = h;
		fb->pitch = pitch;
		fb->format = format;
		if (info) {
			fb->bpp = fb_legacy_bpp(info);
			fb->depth = info->depth;
		} else {
			fb->bpp = format == DRM_FORMAT_RGB565 ? 16 : 32;
			fb->depth = format == DRM_FORMAT_ARGB8888 ? 32 :
				    format == DRM_FORMAT_RGB565 ? 16 : 24;
		}
		fb->modifier = DRM_FORMAT_MOD_LINEAR;
		fb->obj = o;
		fb->owner = NULL;
		fb->format_info = info;
		fb->pitches[0] = pitch;
		fb->objs[0] = o;
		drm_gem_get(o);
		*id_out = fb->id;
		return 0;
	}
	return -ENOSPC;
}

int drm_kms_fb_add_kernel(struct drm_device *dev, struct drm_gem_object *o,
			  uint32_t w, uint32_t h, uint32_t pitch,
			  uint32_t *id_out)
{
	return drm_kms_fb_add_internal(dev, o, w, h, pitch, DRM_FORMAT_XRGB8888, id_out);
}

/* Drop a kernel-owned framebuffer nobody scans out any more.  (A client's
 * closed framebuffer has no owner either, but is not the kernel's.) */
void drm_kms_fb_remove_internal(struct drm_device *dev, uint32_t id)
{
	struct drm_framebuffer *fb = drm_fb_lookup(dev, id);
	if (!fb || fb->owner || fb->closed)
		return;
	fb_free(fb);
}

/* Whatever shows the framebuffer stops showing it: the crtc it is the
 * primary of goes off, any other plane on it is switched off. */
void drm_fb_unshow(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nplanes; i++) {
		struct drm_plane *p = &dev->planes[i];
		if (p->fb_id != id || p->crtc < 0)
			continue;
		if (p->type == DRM_PLANE_TYPE_PRIMARY || !dev->drv->atomic_commit) {
			drm_crtc_set(dev, NULL, &dev->crtc[p->crtc], NULL, 0, 0, 0);
			continue;
		}
		drm_atomic_legacy_set_plane(dev, NULL, p, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	for (uint32_t c = 0; c < dev->ncrtc; c++)
		if (dev->crtc[c].fb_id == id)
			drm_crtc_set(dev, NULL, &dev->crtc[c], NULL, 0, 0, 0);
}

int drm_fb_remove(struct drm_device *dev, uint32_t id)
{
	struct drm_framebuffer *fb = drm_fb_lookup(dev, id);
	if (!fb)
		return -ENOENT;
	drm_fb_unshow(dev, id);
	fb_free(fb);
	return 0;
}

/* Framebuffers a file created go when it does (drm_kms_file_release). */
void drm_framebuffer_file_release(struct drm_device *dev, struct drm_file *fp)
{
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id && dev->fbs[i].owner == fp) {
			drm_fb_unshow(dev, dev->fbs[i].id);
			fb_free(&dev->fbs[i]);
		}
	}
	/* taking them off may have taken a closed one off as well */
	fb_reap_closed(dev);
}

/* ADDFB: the legacy request, bpp/depth for a format and one plane. */
int drm_mode_addfb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_fb_cmd *f = kb;
	struct drm_mode_fb_cmd2 r;
	mm_memset(&r, 0, sizeof(r));
	r.pixel_format = drm_driver_legacy_fb_format(dev, f->bpp, f->depth);
	if (r.pixel_format == DRM_FORMAT_INVALID)
		return -EINVAL;
	r.width = f->width;
	r.height = f->height;
	r.pitches[0] = f->pitch;
	r.handles[0] = f->handle;
	return drm_fb_create(dev, fp, &r, &f->fb_id);
}

/* ADDFB2 */
int drm_mode_addfb2_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_fb_cmd2 *r = kb;
	return drm_fb_create(dev, fp, r, &r->fb_id);
}

/* RMFB: the file's framebuffer goes, off the screen first. */
int drm_mode_rmfb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	uint32_t id = *(uint32_t *)kb;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, id);
	if (!fb)
		return -ENOENT;
#if DRM_FB_RMFB_OWNER_ONLY
	if (fb->owner != fp)
		return -ENOENT;
#else
	(void)fp;
#endif
	return drm_fb_remove(dev, id);
}

/* CLOSEFB: the file lets go of its framebuffer but whatever shows it keeps
 * showing it -- a client can hand the screen over without a blank frame.
 * Not shown, it goes at once; shown, it becomes nobody's and goes when it
 * is no longer shown (fb_reap_closed). */
int drm_mode_closefb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_closefb *r = kb;
	struct drm_framebuffer *fb;

	if (r->pad)
		return -EINVAL;
	fb = drm_fb_lookup(dev, r->fb_id);
	if (!fb || fb->owner != fp)
		return -ENOENT;
	if (!fb_in_use(dev, fb->id)) {
		fb_free(fb);
		return 0;
	}
	fb->owner = NULL;
	fb->closed = 1;
	g_fb_closed++;
	return 0;
}

/* GETFB: a single-plane framebuffer's size, pitch, bpp/depth, and a new
 * handle to its object.  A multi-planar one needs GETFB2. */
int drm_mode_getfb(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_fb_cmd *f = kb;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, f->fb_id);
	if (!fb)
		return -ENOENT;
	if (fb->format_info && fb->format_info->num_planes > 1)
		return -EINVAL;
	f->width = fb->width;
	f->height = fb->height;
	f->pitch = fb->pitch;
	f->bpp = fb->bpp;
	f->depth = fb->depth;
	uint32_t h;
	if (drm_gem_handle_create(fp, fb->obj, &h) != 0)
		return -ENOMEM;
	f->handle = h;
	return 0;
}

/* GETFB2: every plane's pitch, offset, modifier and a handle to its
 * object -- the same handle for planes that share an object. */
int drm_mode_getfb2_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_fb_cmd2 *r = kb;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, r->fb_id);
	unsigned int nplanes, i;
	int rc = 0;

	if (!fb)
		return -ENOENT;
	nplanes = fb->format_info ? fb->format_info->num_planes : 1;
	r->width = fb->width;
	r->height = fb->height;
	r->pixel_format = fb->format;
	r->flags = DRM_MODE_FB_MODIFIERS;
	for (i = 0; i < 4; i++) {
		r->handles[i] = 0;
		r->pitches[i] = 0;
		r->offsets[i] = 0;
		r->modifier[i] = 0;
	}
	for (i = 0; i < nplanes; i++) {
		r->pitches[i] = i ? fb->pitches[i] : fb->pitch;
		r->offsets[i] = i ? fb->offsets[i] : fb->offset;
		r->modifier[i] = fb->modifier;
	}

	for (i = 0; i < nplanes; i++) {
		struct drm_gem_object *o = i ? fb->objs[i] : fb->obj;
		unsigned int j;

		/* If the same object backs several planes, it gets the same
		 * handle. */
		for (j = 0; j < i; j++) {
			struct drm_gem_object *oj = j ? fb->objs[j] : fb->obj;
			if (o == oj) {
				r->handles[i] = r->handles[j];
				break;
			}
		}
		if (r->handles[i])
			continue;
		if (!o) {
			rc = -ENODEV;
			break;
		}
		if (drm_gem_handle_create(fp, o, &r->handles[i]) != 0) {
			rc = -ENOMEM;
			break;
		}
	}

	if (rc != 0) {
		/* Delete any handles created so far. */
		for (i = 0; i < 4; i++) {
			unsigned int j;

			if (r->handles[i])
				drm_gem_handle_delete(fp, r->handles[i]);
			/* and forget the copies of the one just deleted */
			for (j = i + 1; j < 4; j++)
				if (r->handles[j] == r->handles[i])
					r->handles[j] = 0;
			r->handles[i] = 0;
		}
	}
	return rc;
}

/* DIRTYFB: the client changed these rectangles of the framebuffer (none:
 * all of it); every crtc showing it is told. */
int drm_mode_dirtyfb_ioctl(struct drm_device *dev, void *kb,
			   struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_fb_dirty_cmd *d = kb;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, d->fb_id);
	uint32_t n, flags, c;
	int rc = 0;

	if (!fb)
		return -ENOENT;
	n = d->num_clips;
	/* a count without rectangles, or rectangles without a count */
	if (!n != !d->clips_ptr)
		return -EINVAL;
	flags = DRM_MODE_FB_DIRTY_FLAGS & d->flags;
	/* copies come as source/destination pairs */
	if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY && (n % 2))
		return -EINVAL;
	if (n > DRM_MODE_FB_DIRTY_MAX_CLIPS)
		return -EINVAL;
	/* A driver that needs no telling says so; a client stops asking. */
	if (!dev->drv->fb_dirty)
		return -ENOSYS;

	/* Nothing to do for a framebuffer no crtc shows. */
	for (c = 0; c < dev->ncrtc; c++)
		if (dev->crtc[c].fb_id == fb->id)
			break;
	if (c == dev->ncrtc)
		return 0;

	if (n == 0) {
		struct drm_mode_rect_k all = { 0, 0, (int32_t)fb->width, (int32_t)fb->height };
		for (c = 0; c < dev->ncrtc; c++)
			if (dev->crtc[c].fb_id == fb->id)
				rc = dev->drv->fb_dirty(dev, &dev->crtc[c], fb, &all, 1);
		return rc;
	}

	/* The rectangles go to the driver in as few calls as possible: each
	 * call is one announcement to the device (vmwgfx rings once per
	 * call).  At most DRM_MODE_FB_DIRTY_MAX_CLIPS, 2 KB of clips and
	 * 4 KB of rectangles -- too much for the stack, so allocated; when
	 * memory is short, a few at a time from a small buffer instead.
	 * Failing the call is not an option there: the X modesetting
	 * driver stops sending damage for good after one failed DIRTYFB. */
	struct drm_clip_rect small_cl[DRM_FB_DIRTY_CHUNK];
	struct drm_mode_rect_k small_rects[DRM_FB_DIRTY_CHUNK];
	struct drm_clip_rect *cl = kalloc((size_t)n * sizeof(*cl));
	struct drm_mode_rect_k *rects = kalloc((size_t)n * sizeof(*rects));
	uint32_t chunk = n;
	if (!cl || !rects) {
		if (cl)
			kfree(cl);
		if (rects)
			kfree(rects);
		cl = small_cl;
		rects = small_rects;
		chunk = DRM_FB_DIRTY_CHUNK;
	}
	for (uint32_t done = 0; done < n && rc == 0; done += chunk) {
		uint32_t k = n - done < chunk ? n - done : chunk;

		if (copy_from_user(cl, (void *)(uintptr_t)(d->clips_ptr + (uint64_t)done * sizeof(*cl)),
				   (size_t)k * sizeof(*cl)) != 0) {
			rc = -EFAULT;
			break;
		}
		for (uint32_t i = 0; i < k; i++) {
			rects[i].x1 = cl[i].x1;
			rects[i].y1 = cl[i].y1;
			rects[i].x2 = cl[i].x2;
			rects[i].y2 = cl[i].y2;
		}
		for (c = 0; c < dev->ncrtc; c++)
			if (dev->crtc[c].fb_id == fb->id)
				rc = dev->drv->fb_dirty(dev, &dev->crtc[c], fb, rects, k);
	}
	if (cl != small_cl)
		kfree(cl);
	if (rects != small_rects)
		kfree(rects);
	return rc;
}
