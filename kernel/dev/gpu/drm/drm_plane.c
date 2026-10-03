// LikeOS -- display-manager core: planes.
//
// A plane shows a source rectangle of a framebuffer on a rectangle of a
// crtc.  Every crtc gets a primary and a cursor plane when its connector is
// added; a driver may add overlays.  Each plane carries the formats and
// layout modifiers it can scan out, published to clients as its IN_FORMATS
// blob.  The plane ioctls: GETPLANERESOURCES, GETPLANE and SETPLANE; and
// the check that a plane's source rectangle lies inside its framebuffer.
// A new plane starts with the optional plane properties at their defaults,
// and with the blending properties for a driver that opted into them.
//
// A display that draws the pointer itself -- the host of a virtual machine --
// needs the cursor's hot spot as well as its position, since that is the
// point it tracks the host's pointer by.  A driver with
// DRM_FEATURE_CURSOR_HOTSPOT gives its cursor planes HOTSPOT_X / HOTSPOT_Y,
// and an atomic client that has not said it sets them
// (DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) is not shown the cursor planes: it
// would move the image without the hot spot and the two pointers would part.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

struct drm_plane *drm_plane_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nplanes; i++)
		if (dev->planes[i].id == id)
			return &dev->planes[i];
	return NULL;
}

/* The crtc a primary or cursor plane belongs to (the older paths, which
 * know only those two, ask this way). */
struct drm_crtc *drm_plane_crtc(struct drm_device *dev, uint32_t plane_id,
				int *is_cursor)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		if (dev->crtc[i].primary_plane_id == plane_id) {
			*is_cursor = 0;
			return &dev->crtc[i];
		}
		if (dev->crtc[i].cursor_plane_id == plane_id) {
			*is_cursor = 1;
			return &dev->crtc[i];
		}
	}
	return NULL;
}

const uint32_t drm_cursor_formats[1] = { DRM_FORMAT_ARGB8888 };
const uint64_t drm_linear_modifiers[1] = { DRM_FORMAT_MOD_LINEAR };
const uint32_t drm_default_formats[3] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888,
					     DRM_FORMAT_RGB565 };

/* The IN_FORMATS blob for a format/modifier list: every format usable
 * with every modifier. */
uint32_t drm_in_formats_blob(struct drm_device *dev, const uint32_t *formats,
			     uint32_t nformats, const uint64_t *mods,
			     uint32_t nmods)
{
	if (nformats > 64)
		nformats = 64;
	uint32_t fo = sizeof(struct drm_format_modifier_blob);
	uint32_t mo = (fo + nformats * 4 + 7) & ~7u;
	uint32_t len = mo + nmods * sizeof(struct drm_format_modifier);
	uint8_t *b = kalloc(len);
	uint32_t id;
	if (!b)
		return 0;
	/* The modifier array starts 8-byte aligned: with an odd number of
	 * formats there are 4 bytes of padding in front of it, and they go
	 * to every client that reads the blob. */
	mm_memset(b, 0, len);
	struct drm_format_modifier_blob hdr;
	mm_memset(&hdr, 0, sizeof(hdr));
	hdr.version = FORMAT_BLOB_CURRENT;
	hdr.count_formats = nformats;
	hdr.formats_offset = fo;
	hdr.count_modifiers = nmods;
	hdr.modifiers_offset = mo;
	mm_memcpy(b, &hdr, sizeof(hdr));
	mm_memcpy(b + fo, formats, nformats * 4);
	for (uint32_t m = 0; m < nmods; m++) {
		struct drm_format_modifier fm;
		mm_memset(&fm, 0, sizeof(fm));
		fm.formats = nformats >= 64 ? ~0ULL : ((1ULL << nformats) - 1);
		fm.offset = 0;
		fm.modifier = mods[m];
		mm_memcpy(b + mo + m * sizeof(fm), &fm, sizeof(fm));
	}
	id = drm_blob_create(dev, b, len);
	kfree(b);
	return id;
}

/*
 * A driver that opted into DRM_FEATURE_BLEND gets the blending properties
 * on every plane it adds: alpha, the three blend modes (pre-multiplied by
 * default), rotation (only the unrotated one -- the core cannot know what
 * the hardware rotates) and a fixed stacking position by plane type,
 * primary at the bottom and the cursor on top.  A driver that can do more
 * creates its own instead (drm_blend.h) and leaves the bit off.
 */
static void plane_add_blend_properties(struct drm_device *dev, struct drm_plane *p)
{
	unsigned int zpos = p->type == DRM_PLANE_TYPE_PRIMARY ? 0 :
			    p->type == DRM_PLANE_TYPE_CURSOR ? 2 : 1;

	(void)drm_plane_create_alpha_property(dev, p);
	(void)drm_plane_create_blend_mode_property(dev, p,
						   BIT(DRM_MODE_BLEND_PIXEL_NONE) |
						   BIT(DRM_MODE_BLEND_PREMULTI) |
						   BIT(DRM_MODE_BLEND_COVERAGE));
	(void)drm_plane_create_rotation_property(dev, p, DRM_MODE_ROTATE_0,
						 DRM_MODE_ROTATE_0);
	(void)drm_plane_create_zpos_immutable_property(dev, p, zpos);
}

/* HOTSPOT_X and HOTSPOT_Y on a cursor plane, both starting at 0.  Shared
 * between the cursor planes of a device (the value is per plane). */
static int plane_create_hotspot_properties(struct drm_device *dev, struct drm_plane *p)
{
	struct drm_prop *prop_x;
	struct drm_prop *prop_y;

	WARN_ON(!(dev->drv->features & DRM_FEATURE_CURSOR_HOTSPOT));

	prop_x = drm_property_create_signed_range(dev, 0, "HOTSPOT_X",
						  -2147483648LL, 2147483647LL);
	prop_x = drm_blend_plane_attach(dev, p, prop_x, 0);
	if (!prop_x)
		return -ENOMEM;

	prop_y = drm_property_create_signed_range(dev, 0, "HOTSPOT_Y",
						  -2147483648LL, 2147483647LL);
	prop_y = drm_blend_plane_attach(dev, p, prop_y, 0);
	if (!prop_y)
		return -ENOMEM;

	p->hotspot_x_property = prop_x;
	p->hotspot_y_property = prop_y;

	return 0;
}

int drm_plane_add(struct drm_device *dev, uint32_t type, uint32_t possible_crtcs,
		  const uint32_t *formats, uint32_t nformats,
		  const uint64_t *modifiers, uint32_t nmodifiers)
{
	if (dev->nplanes >= DRM_MAX_PLANES)
		return -1;
	int i = (int)dev->nplanes++;
	struct drm_plane *p = &dev->planes[i];
	mm_memset(p, 0, sizeof(*p));
	p->id = drm_mode_id_alloc(dev);
	p->index = i;
	p->type = type;
	p->possible_crtcs = possible_crtcs;
	p->crtc = -1;
	/* what the optional plane properties read until a client sets
	 * them: unrotated, opaque, pre-multiplied (drm_blend.h) */
	p->rotation = DRM_MODE_ROTATE_0;
	p->alpha = DRM_BLEND_ALPHA_OPAQUE;
	p->pixel_blend_mode = DRM_MODE_BLEND_PREMULTI;
	if (type == DRM_PLANE_TYPE_CURSOR) {
		p->formats = drm_cursor_formats;
		p->nformats = 1;
		p->modifiers = drm_linear_modifiers;
		p->nmodifiers = 1;
		p->in_formats_blob = dev->in_formats_cursor_blob;
	} else if (formats) {
		p->formats = formats;
		p->nformats = nformats;
		p->modifiers = modifiers ? modifiers : drm_linear_modifiers;
		p->nmodifiers = modifiers ? nmodifiers : 1;
		p->in_formats_blob = drm_in_formats_blob(dev, p->formats, p->nformats,
						    p->modifiers, p->nmodifiers);
	} else {
		p->formats = dev->drv->fb_formats ? dev->drv->fb_formats : drm_default_formats;
		p->nformats = dev->drv->fb_formats ? dev->drv->nfb_formats : 3;
		p->modifiers = dev->drv->fb_modifiers ? dev->drv->fb_modifiers : drm_linear_modifiers;
		p->nmodifiers = dev->drv->fb_modifiers ? dev->drv->nfb_modifiers : 1;
		p->in_formats_blob = dev->in_formats_blob;
	}
	if ((dev->drv->features & DRM_FEATURE_BLEND) && dev->drv->atomic_commit &&
	    dev->prop_crtc_id)
		plane_add_blend_properties(dev, p);
	if (DRM_CURSOR_HOTSPOT && type == DRM_PLANE_TYPE_CURSOR &&
	    (dev->drv->features & DRM_FEATURE_CURSOR_HOTSPOT) &&
	    dev->drv->atomic_commit && dev->prop_crtc_id)
		(void)plane_create_hotspot_properties(dev, p);
	return i;
}

int drm_framebuffer_check_src_coords(uint32_t src_x, uint32_t src_y,
				     uint32_t src_w, uint32_t src_h,
				     const struct drm_framebuffer *fb)
{
	uint32_t fb_width = fb->width << 16;
	uint32_t fb_height = fb->height << 16;

	/* Make sure source coordinates are inside the fb. */
	if (src_w > fb_width ||
	    src_x > fb_width - src_w ||
	    src_h > fb_height ||
	    src_y > fb_height - src_h)
		return -ENOSPC;

	return 0;
}

/* GETPLANERESOURCES */
int drm_mode_getplane_res(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_get_plane_res *r = kb;
	uint32_t ids[DRM_MAX_PLANES];
	uint32_t n = 0;
	/* Without the universal-planes capability a client sees only
	 * overlays: the primary and cursor planes are what its older
	 * calls drive. */
	int universal = (fp->client_caps >> DRM_CLIENT_CAP_UNIVERSAL_PLANES) & 1;
	/* A display that draws the pointer itself needs the hot spot: an
	 * atomic client that has not said it sets HOTSPOT_X / HOTSPOT_Y gets
	 * no cursor plane, and draws its pointer some other way. */
	int hide_cursors = DRM_CURSOR_HOTSPOT &&
			   (dev->drv->features & DRM_FEATURE_CURSOR_HOTSPOT) &&
			   ((fp->client_caps >> DRM_CLIENT_CAP_ATOMIC) & 1) &&
			   !((fp->client_caps >> DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) & 1);
	for (uint32_t i = 0; i < dev->nplanes; i++) {
		if (!universal && dev->planes[i].type != DRM_PLANE_TYPE_OVERLAY)
			continue;
		if (hide_cursors && dev->planes[i].type == DRM_PLANE_TYPE_CURSOR)
			continue;
		ids[n++] = dev->planes[i].id;
	}
	int rc = drm_copy_ids(r->plane_id_ptr, r->count_planes, ids, n);
	if (rc)
		return rc;
	r->count_planes = n;
	return 0;
}

/* GETPLANE */
int drm_mode_getplane(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_get_plane *g = kb;
	struct drm_plane *p = drm_plane_find(dev, g->plane_id);
	if (!p)
		return -ENOENT;
	int rc = drm_copy_ids(g->format_type_ptr, g->count_format_types, p->formats,
			  p->nformats);
	if (rc)
		return rc;
	g->count_format_types = p->nformats;
	if (dev->drv->atomic_commit) {
		g->crtc_id = p->crtc >= 0 ? dev->crtc[p->crtc].id : 0;
		g->fb_id = p->fb_id;
	} else {
		int cursor;
		struct drm_crtc *cr = drm_plane_crtc(dev, g->plane_id, &cursor);
		g->crtc_id = cr && cr->active ? cr->id : 0;
		g->fb_id = (cr && !cursor) ? cr->fb_id : 0;
	}
	g->possible_crtcs = p->possible_crtcs;
	g->gamma_size = 0;
	return 0;
}

/* SETPLANE */
int drm_mode_setplane(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_set_plane *s = kb;
	struct drm_plane *p = drm_plane_find(dev, s->plane_id);
	if (!p)
		return -ENOENT;
	if (dev->drv->atomic_commit)
		return drm_atomic_legacy_set_plane(dev, fp, p, s->crtc_id, s->fb_id,
						   s->crtc_x, s->crtc_y, s->crtc_w,
						   s->crtc_h, s->src_x, s->src_y,
						   s->src_w, s->src_h);
	int cursor;
	struct drm_crtc *cr = drm_plane_crtc(dev, s->plane_id, &cursor);
	if (!cr)
		return -ENOENT;
	if (cursor)
		return -EINVAL;
	if (!s->fb_id)
		return drm_crtc_set(dev, fp, cr, NULL, 0, 0, 0);
	struct drm_framebuffer *fb = drm_fb_lookup(dev, s->fb_id);
	if (!fb)
		return -ENOENT;
	if (cr->active && cr->fb_id != fb->id && dev->drv->page_flip) {
		int rc = dev->drv->page_flip(dev, cr, fb);
		if (rc)
			return rc;
		cr->fb_id = fb->id;
		return 0;
	}
	return drm_crtc_set(dev, fp, cr, &cr->mode, fb->id, (int)(s->src_x >> 16), (int)(s->src_y >> 16));
}
