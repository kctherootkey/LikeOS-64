// LikeOS -- vmwgfx: the hardware cursor.
//
// The DRM core's cursor hooks, the cursor plane of an atomic request
// (vmw_cursor_plane_check() / vmw_cursor_plane_update(), called by
// vmw_kms_atomic.c), the CURSOR_BYPASS ioctl and the snooping of legacy
// cursor surfaces.
//
// One image at a time reaches the host, by one of three routes chosen per
// image (vmw_cursor_update_type()):
//   - a snooped legacy surface: a 64x64 A8R8G8B8 scanout surface the client
//     fills with SURFACE_DMA.  The verifier copies every such DMA into a
//     kernel image (vmw_cursor_cmd_dma_snoop()), and that image is defined
//     whenever it changes -- at the cursor set and after every submission
//     (vmw_kms_cursor_post_execbuf());
//   - a cursor MOB, on a host with SVGA_CAP2_CURSOR_MOB: a header and the
//     ARGB pixels in a MOB the device is pointed at through
//     SVGA_REG_CURSOR_MOBID.  Images up to SVGA_REG_CURSOR_MAX_DIMENSION
//     on a side.  A few MOBs are kept for reuse, so that a pointer changing
//     shape does not create and destroy device objects every time;
//   - a define command through the hardware layer: an alpha cursor, or on
//     a host without alpha cursors a two-colour AND/XOR approximation.
//     64x64 at most.  This is also the fallback when a MOB cannot be had.
// The image comes from a buffer object (rows of width * 4 bytes for the
// legacy hooks, the framebuffer's pitch for a cursor plane) or from a
// guest-backed surface, which is read back from the device first so that
// what the host rendered into it is what the pointer shows.
//
// The position goes through the hardware layer too
// (vmsvga2_hw_cursor_update_position(), which picks the CURSOR4 registers,
// the FIFO bypass or the legacy registers).  The device places the image's
// HOTSPOT at the position it is given, so the position written is the
// image's top-left corner from the client plus the hotspot: the image's own
// (CURSOR2, or a cursor plane's HOTSPOT_X / HOTSPOT_Y) and the one a display
// server sets with DRM_VMW_CURSOR_BYPASS.  The position of a cursor plane is
// the one in the request, not the crtc's committed cursor position, which
// the core updates only after the commit.
//
// Locking: one sleeping lock, vmw_cursor_lock, over the cursor state and
// every hardware access made on its behalf.  It is taken inside execbuf_lock
// (vmw_kms_cursor_post_execbuf()) and never held while taking execbuf_lock
// or binding_lock; references to surfaces are dropped after it is released.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2024-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/drm/drm.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* Cursor images on a host with SVGA_CAP2_CURSOR_MOB go to the device as a
 * MOB named through SVGA_REG_CURSOR_MOBID, which is what lets an image be
 * larger than 64x64.  Off: every image is a define command (64x64 at most),
 * the only route before. */
#define VMW_CURSOR_MOB 1

/* The position registers name where the device puts the image's hotspot,
 * while the DRM cursor position is the image's top-left corner: the hotspot
 * is added.  Off: the top-left corner is written as is, which puts the
 * pointer's tip `hotspot' pixels up and to the left of where the client
 * meant it. */
#define VMW_CURSOR_POSITION_HOTSPOT 1

/* A guest-backed surface used as a cursor is read back from the device
 * (READBACK_GB_SURFACE, then a fence wait) before its backing is copied,
 * so that what was rendered into it on the host is what the pointer shows.
 * Off: the backing is used as it stands. */
#define VMW_CURSOR_SURFACE_READBACK 1

#define VMW_CURSOR_SNOOP_FORMAT SVGA3D_A8R8G8B8
#define VMW_CURSOR_SNOOP_WIDTH 64
#define VMW_CURSOR_SNOOP_HEIGHT 64

/* The largest image a define command carries (the hardware layer's limit). */
#define VMW_CURSOR_DEFINE_MAX 64

/* Cursor MOBs kept for reuse besides the one the device shows. */
#define VMW_CURSOR_MOB_CACHE 3

enum vmw_cursor_update_type {
	VMW_CURSOR_UPDATE_NONE = 0,
	VMW_CURSOR_UPDATE_LEGACY,	/* a snooped legacy surface */
	VMW_CURSOR_UPDATE_DEFINE,	/* a define command */
	VMW_CURSOR_UPDATE_MOB,		/* a cursor MOB */
};

/* The cursor of one crtc. */
struct vmw_cursor_unit {
	/* Set by DRM_VMW_CURSOR_BYPASS, added to the image's own hotspot. */
	int32_t legacy_hot_x, legacy_hot_y;
	/* The image's own hotspot (CURSOR2), as last set. */
	int32_t core_hot_x, core_hot_y;
	/* The hotspot the device was given with the image (the two above
	 * added up when it was defined): a CURSOR_BYPASS since then means the
	 * image has to be defined again. */
	uint32_t def_hot_x, def_hot_y;
	uint32_t width, height;
	/* An image is defined and the client wants it seen. */
	bool shown;
	/* A cursor plane's image changed while its crtc was off and was not
	 * given to the device: define it when the crtc comes back. */
	bool stale;
	/* The position last written, so that a hide leaves it in place. */
	int32_t pos_x, pos_y;
	/* A snooped legacy surface being shown: one reference to its object,
	 * and the snooper id of the image the device last got. */
	struct drm_gem_object *snoop_obj;
	size_t snoop_id;
};

struct vmw_cursor_state {
	struct vmw_cursor_unit unit[DRM_MAX_CRTCS];
	/* Units with a snoop_obj; read without the lock as a hint. */
	int snoop_units;
	/* The MOB the device was last pointed at, and spares. */
	struct drm_gem_object *mob_cur;
	struct drm_gem_object *mob_cache[VMW_CURSOR_MOB_CACHE];
	/* The image on the device is mob_cur's (not a define command's):
	 * what a device that lost its state has to be pointed at again. */
	bool mob_shown;
	/* Between vmw_cursor_suspend() and vmw_cursor_resume(): the cursor is
	 * off the screen whatever the units say. */
	bool suspended;
	/* Staging for define commands. */
	uint32_t image[VMW_CURSOR_DEFINE_MAX * VMW_CURSOR_DEFINE_MAX];
	uint32_t and_mask[VMW_CURSOR_DEFINE_MAX * 2];
	uint32_t xor_mask[VMW_CURSOR_DEFINE_MAX * 2];
};

/* An image a client asks for: `w' x `h' pixels of object `obj'.  A buffer
 * object holds it as rows `pitch' bytes apart (0: w * 4) from byte `offset'
 * on; a surface holds it from its pixel (x, y), in its own layout. */
struct vmw_cursor_image {
	struct drm_gem_object *obj;
	uint64_t offset;
	uint32_t pitch;
	uint32_t x, y;
	uint32_t w, h;
};

/* Where an image comes from: rows `pitch' bytes apart, starting `offset'
 * bytes into a page-backed object, or in a kernel buffer. */
struct vmw_cursor_src {
	struct drm_gem_object *obj;
	uint64_t offset;
	uint32_t pitch;
	const uint32_t *image;
};

static mm_rwsem_t vmw_cursor_lock = MM_RWSEM_INIT("vmw_cursor");

/* A client error, said a few times per call site and then no more: a
 * display server that gets it wrong once gets it wrong on every pointer
 * move. */
#define vmw_cursor_log(fmt, ...)                                       \
	do {                                                           \
		static int __vmw_cursor_said;                          \
		if (__vmw_cursor_said < 10) {                          \
			__vmw_cursor_said++;                           \
			kprintf("[drm] vmwgfx: " fmt, ##__VA_ARGS__);  \
		}                                                      \
	} while (0)

/* ---- page-backed object access -------------------------------------------
 *
 * Objects are arrays of pages that are not contiguous in the direct map, so
 * every copy goes a page at a time.  The callers have checked the range
 * against the object's size; a missing page is still answered with -EIO
 * rather than a fault. */

static uint32_t vmw_cursor_chunk(uint64_t off, uint32_t bytes)
{
	uint32_t room = (uint32_t)(PAGE_SIZE - (off % PAGE_SIZE));

	return room < bytes ? room : bytes;
}

static int vmw_cursor_obj_read(struct drm_gem_object *o, uint64_t off,
			       void *dst, uint32_t bytes)
{
	uint8_t *d = dst;

	while (bytes) {
		uint8_t *page = drm_gem_page_virt(o, (uint32_t)(off / PAGE_SIZE));
		uint32_t chunk = vmw_cursor_chunk(off, bytes);

		if (!page)
			return -EIO;
		kmemcpy(d, page + off % PAGE_SIZE, chunk);
		d += chunk;
		off += chunk;
		bytes -= chunk;
	}
	return 0;
}

static int vmw_cursor_obj_write(struct drm_gem_object *o, uint64_t off,
				const void *src, uint32_t bytes)
{
	const uint8_t *s = src;

	while (bytes) {
		uint8_t *page = drm_gem_page_virt(o, (uint32_t)(off / PAGE_SIZE));
		uint32_t chunk = vmw_cursor_chunk(off, bytes);

		if (!page)
			return -EIO;
		kmemcpy(page + off % PAGE_SIZE, s, chunk);
		s += chunk;
		off += chunk;
		bytes -= chunk;
	}
	return 0;
}

static int vmw_cursor_obj_copy(struct drm_gem_object *dst, uint64_t doff,
			       struct drm_gem_object *src, uint64_t soff,
			       uint32_t bytes)
{
	while (bytes) {
		uint8_t *sp = drm_gem_page_virt(src, (uint32_t)(soff / PAGE_SIZE));
		uint8_t *dp = drm_gem_page_virt(dst, (uint32_t)(doff / PAGE_SIZE));
		uint32_t chunk = vmw_cursor_chunk(soff, bytes);

		chunk = vmw_cursor_chunk(doff, chunk);
		if (!sp || !dp)
			return -EIO;
		kmemcpy(dp + doff % PAGE_SIZE, sp + soff % PAGE_SIZE, chunk);
		soff += chunk;
		doff += chunk;
		bytes -= chunk;
	}
	return 0;
}

/* Row `y' of an image, `bytes' long, into `dst'. */
static int vmw_cursor_src_row(const struct vmw_cursor_src *src, uint32_t y,
			      void *dst, uint32_t bytes)
{
	if (src->image) {
		kmemcpy(dst, (const uint8_t *)src->image + (size_t)y * src->pitch,
			bytes);
		return 0;
	}
	return vmw_cursor_obj_read(src->obj, src->offset + (uint64_t)y * src->pitch,
				   dst, bytes);
}

/* The image, packed (rows of w * 4 bytes), into `out'. */
static int vmw_cursor_src_gather(const struct vmw_cursor_src *src, uint32_t w,
				 uint32_t h, uint32_t *out)
{
	for (uint32_t y = 0; y < h; y++) {
		int rc = vmw_cursor_src_row(src, y, out + (size_t)y * w, w * 4);

		if (rc)
			return rc;
	}
	return 0;
}

/* The image, packed, into object `dst' from byte `doff' on. */
static int vmw_cursor_src_copy_to_obj(const struct vmw_cursor_src *src,
				      uint32_t w, uint32_t h,
				      struct drm_gem_object *dst, uint64_t doff)
{
	const uint32_t row = w * 4;

	for (uint32_t y = 0; y < h; y++) {
		uint64_t to = doff + (uint64_t)y * row;
		int rc;

		if (src->image)
			rc = vmw_cursor_obj_write(dst, to,
						  (const uint8_t *)src->image +
							  (size_t)y * src->pitch,
						  row);
		else
			rc = vmw_cursor_obj_copy(dst, to, src->obj,
						 src->offset +
							 (uint64_t)y * src->pitch,
						 row);
		if (rc)
			return rc;
	}
	return 0;
}

/* ---- state ---------------------------------------------------------------- */

static bool vmw_cursor_has_mob(const struct vmw_device *v)
{
	return VMW_CURSOR_MOB && v->has_gb && v->otables_ready &&
	       (v->cap2 & SVGA_CAP2_CURSOR_MOB) != 0;
}

/* The state, set up on first use.  Caller holds vmw_cursor_lock. */
static struct vmw_cursor_state *vmw_cursor_state_get(struct vmw_device *v)
{
	struct vmw_cursor_state *st = v->cursor;

	if (st)
		return st;
	st = kalloc(sizeof(*st));
	if (!st)
		return NULL;
	mm_memset(st, 0, sizeof(*st));
	v->cursor = st;
	return st;
}

static struct vmw_cursor_unit *vmw_cursor_unit(struct vmw_cursor_state *st,
					       struct drm_crtc *crtc)
{
	if (!crtc || crtc->index < 0 || crtc->index >= DRM_MAX_CRTCS)
		return NULL;
	return &st->unit[crtc->index];
}

/* Where the crtc's scan-out sits on the virtual desktop the cursor
 * registers address: its display unit's place in the host's layout (the
 * desktop's origin for the first unit and on a single head). */
static void vmw_cursor_unit_origin(struct vmw_device *v, struct drm_crtc *crtc,
				   int32_t *x, int32_t *y)
{
	struct vmw_display_unit *du = crtc ? vmw_du(v, crtc->index) : NULL;

	*x = du ? du->gui_x : 0;
	*y = du ? du->gui_y : 0;
}

/* The hotspot of the image as defined: the image's own plus the display
 * server's, never negative (the device takes it unsigned). */
static uint32_t vmw_cursor_hot(int32_t legacy, int32_t core)
{
	int64_t hot = (int64_t)legacy + core;

	if (hot < 0)
		return 0;
	if (hot > INT32_MAX)
		return INT32_MAX;
	return (uint32_t)hot;
}

/* Take the cursor off the screen, leaving its position. */
static int vmw_cursor_hw_hide(int32_t x, int32_t y)
{
	if (vmsvga2_hw_cursor_update_position(false, x, y) == 0)
		return 0;
	return vmsvga2_hw_cursor_hide();
}

/* Write the unit's position: the client's top-left corner (x, y) on the
 * crtc, placed on the desktop, plus the hotspot. */
static int vmw_cursor_unit_position(struct vmw_device *v,
				    struct vmw_cursor_unit *u,
				    struct drm_crtc *crtc, int x, int y)
{
	int32_t ox, oy;

	vmw_cursor_unit_origin(v, crtc, &ox, &oy);
	u->pos_x = x + ox;
	u->pos_y = y + oy;
#if VMW_CURSOR_POSITION_HOTSPOT
	u->pos_x += (int32_t)vmw_cursor_hot(u->legacy_hot_x, u->core_hot_x);
	u->pos_y += (int32_t)vmw_cursor_hot(u->legacy_hot_y, u->core_hot_y);
#endif
	if (!u->shown)
		return vmw_cursor_hw_hide(u->pos_x, u->pos_y);
	return vmsvga2_hw_cursor_update_position(true, u->pos_x, u->pos_y);
}

/* ---- define commands -------------------------------------------------- */

/* A two-colour approximation of an ARGB image, for a host without alpha
 * cursors: transparent = AND 1 / XOR 0, black = AND 0 / XOR 0, anything
 * else = AND 0 / XOR 1 (white).  Rows are byte-oriented, most significant
 * bit first, padded to 32 bits; the width itself is padded to a multiple
 * of 32 with transparent columns, which is where the decoders of the
 * various hosts agree on the row stride. */
static int vmw_cursor_define_mono(struct vmw_cursor_state *st,
				  const uint32_t *argb, uint32_t w, uint32_t h,
				  uint32_t hot_x, uint32_t hot_y)
{
	uint8_t *andb = (uint8_t *)st->and_mask;
	uint8_t *xorb = (uint8_t *)st->xor_mask;
	uint32_t pw = (w + 31) & ~31u;
	uint32_t row_bytes = pw / 8;

	for (uint32_t i = 0; i < ARRAY_SIZE(st->and_mask); i++) {
		st->and_mask[i] = 0xFFFFFFFFu;
		st->xor_mask[i] = 0;
	}
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = argb[y * w + x];
			uint32_t i = y * row_bytes + (x >> 3);
			uint8_t bit = (uint8_t)(0x80u >> (x & 7));

			if ((p >> 24) >= 0x80) {
				andb[i] &= (uint8_t)~bit;
				if (p & 0xFFFFFF)
					xorb[i] |= bit;
			}
		}
	}
	return vmsvga2_hw_cursor_define_mono(pw, h, hot_x, hot_y, st->and_mask,
					     st->xor_mask);
}

/* Define a packed ARGB image of at most VMW_CURSOR_DEFINE_MAX on a side. */
static int vmw_cursor_define(struct vmw_cursor_state *st, const uint32_t *argb,
			     uint32_t w, uint32_t h, uint32_t hot_x,
			     uint32_t hot_y)
{
	int rc = vmsvga2_hw_cursor_define_alpha(w, h, hot_x, hot_y, argb);

	if (rc == -ENODEV)
		rc = vmw_cursor_define_mono(st, argb, w, h, hot_x, hot_y);
	if (rc == 0)
		st->mob_shown = false;
	return rc;
}

/* ---- cursor MOBs ------------------------------------------------------ */

static uint32_t vmw_cursor_mob_size(uint32_t w, uint32_t h)
{
	return w * h * (uint32_t)sizeof(uint32_t) +
	       (uint32_t)sizeof(SVGAGBCursorHeader);
}

/* Can a w x h image go into a cursor MOB on this device? */
static bool vmw_cursor_mob_fits(struct vmw_device *v, uint32_t w, uint32_t h)
{
	uint32_t max_dim = vmsvga2_hw_read_reg(SVGA_REG_CURSOR_MAX_DIMENSION);
	uint32_t max_bytes = vmsvga2_hw_read_reg(SVGA_REG_CURSOR_MAX_BYTE_SIZE);
	uint64_t size = (uint64_t)w * h * sizeof(uint32_t) +
			sizeof(SVGAGBCursorHeader);

	if (w > max_dim || h > max_dim)
		return false;
	if (size > v->max_mob_size)
		return false;
	/* Zero: the device does not say, and the MOB limit above is all. */
	if (max_bytes && size > max_bytes)
		return false;
	return true;
}

static void vmw_cursor_mob_destroy(struct drm_gem_object **mob)
{
	if (!*mob)
		return;
	drm_gem_put(*mob);
	*mob = NULL;
}

/* A MOB no longer shown goes back to the spares: into a free slot, else in
 * place of a smaller spare, else it is destroyed. */
static void vmw_cursor_mob_put(struct vmw_cursor_state *st,
			       struct drm_gem_object **mob)
{
	uint32_t i;

	if (!*mob)
		return;
	for (i = 0; i < VMW_CURSOR_MOB_CACHE; i++) {
		if (!st->mob_cache[i]) {
			st->mob_cache[i] = *mob;
			*mob = NULL;
			return;
		}
	}
	for (i = 0; i < VMW_CURSOR_MOB_CACHE; i++) {
		if (st->mob_cache[i]->size < (*mob)->size) {
			vmw_cursor_mob_destroy(&st->mob_cache[i]);
			st->mob_cache[i] = *mob;
			*mob = NULL;
			return;
		}
	}
	vmw_cursor_mob_destroy(mob);
}

/* A MOB of at least `size' bytes that the device is not showing: a spare,
 * or a new one.  A new MOB is fenced before it is used, so the device has
 * processed its DEFINE_GB_MOB64 by the time CURSOR_MOBID names it. */
static int vmw_cursor_mob_get(struct vmw_device *v, struct vmw_cursor_state *st,
			      uint32_t size, struct drm_gem_object **out)
{
	struct drm_gem_object *bo;
	struct drm_fence *fence;
	struct vmw_bo *b;

	for (uint32_t i = 0; i < VMW_CURSOR_MOB_CACHE; i++) {
		if (st->mob_cache[i] && st->mob_cache[i]->size >= size) {
			*out = st->mob_cache[i];
			st->mob_cache[i] = NULL;
			return 0;
		}
	}

	bo = drm_gem_alloc(&v->drm, DRM_GEM_BO, size);
	if (!bo)
		return -ENOMEM;
	if (drm_gem_alloc_pages(bo) || v->drm.drv->gem_init(bo)) {
		drm_gem_put(bo);
		return -ENOMEM;
	}
	b = bo->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID) {
		drm_gem_put(bo);
		return -ENOMEM;
	}

	fence = vmw_fence_emit(v, 0);
	if (!fence) {
		drm_gem_put(bo);
		return -ENOMEM;
	}
	(void)vmw_fence_wait(v, fence, VMW_FENCE_TIMEOUT_NS);
	drm_fence_put(fence);

	*out = bo;
	return 0;
}

/* Show a w x h image through a cursor MOB. */
static int vmw_cursor_update_mob(struct vmw_device *v,
				 struct vmw_cursor_state *st,
				 const struct vmw_cursor_src *src, uint32_t w,
				 uint32_t h, uint32_t hot_x, uint32_t hot_y)
{
	struct drm_gem_object *mob = NULL;
	struct drm_gem_object *old;
	SVGAGBCursorHeader header;
	SVGAGBAlphaCursorHeader *alpha_header = &header.header.alphaHeader;
	int rc;

	rc = vmw_cursor_mob_get(v, st, vmw_cursor_mob_size(w, h), &mob);
	if (rc)
		return rc;

	mm_memset(&header, 0, sizeof(header));
	header.type = SVGA_ALPHA_CURSOR;
	header.sizeInBytes = w * h * (uint32_t)sizeof(uint32_t);
	alpha_header->hotspotX = hot_x;
	alpha_header->hotspotY = hot_y;
	alpha_header->width = w;
	alpha_header->height = h;

	rc = vmw_cursor_obj_write(mob, 0, &header, sizeof(header));
	if (!rc)
		rc = vmw_cursor_src_copy_to_obj(src, w, h, mob, sizeof(header));
	if (rc) {
		vmw_cursor_mob_put(st, &mob);
		return rc;
	}

	vmsvga2_hw_write_reg(SVGA_REG_CURSOR_MOBID,
			     ((struct vmw_bo *)mob->priv)->mob.id);
	/* The image is the MOB's now: a device reset must not define the
	 * hardware layer's cached image over it. */
	vmsvga2_hw_cursor_forget();

	old = st->mob_cur;
	st->mob_cur = mob;
	st->mob_shown = true;
	vmw_cursor_mob_put(st, &old);
	return 0;
}

/* ---- sources ---------------------------------------------------------- */

/* Where a surface keeps the image: the byte of its backing where the
 * image's first row starts, and the row pitch.  -EINVAL for a surface
 * that cannot be a cursor this way. */
static int vmw_cursor_surface_layout(const struct vmw_surface *s,
				     const struct vmw_cursor_image *img,
				     uint64_t *start, uint32_t *pitch_out)
{
	uint32_t pitch;
	uint64_t first, end;

	/* A legacy surface without a snooper keeps its storage on the host,
	 * with nothing in the guest to read. */
	if (s->legacy || !s->backup || !s->defined || !s->backup->pages)
		return -EINVAL;
	/* Four bytes per pixel, blue first, alpha last: what the define
	 * command and the cursor MOB take. */
	if (s->format != SVGA3D_A8R8G8B8 && s->format != SVGA3D_B8G8R8A8_UNORM)
		return -EINVAL;
	if (img->w > s->base_size.width || img->h > s->base_size.height ||
	    img->x > s->base_size.width - img->w ||
	    img->y > s->base_size.height - img->h)
		return -EINVAL;
	pitch = s->base_size.width * 4;
	first = s->backup_offset + (uint64_t)img->y * pitch +
		(uint64_t)img->x * 4;
	end = first + (uint64_t)(img->h - 1) * pitch + (uint64_t)img->w * 4;
	if (end > s->backup->size)
		return -EINVAL;
	if (start)
		*start = first;
	if (pitch_out)
		*pitch_out = pitch;
	return 0;
}

/* The same for a buffer object. */
static int vmw_cursor_bo_layout(const struct drm_gem_object *o,
				const struct vmw_cursor_image *img,
				uint32_t *pitch_out)
{
	uint32_t pitch = img->pitch ? img->pitch : img->w * 4;
	uint64_t end;

	if (!o->pages || pitch < img->w * 4)
		return -EINVAL;
	end = img->offset + (uint64_t)(img->h - 1) * pitch +
	      (uint64_t)img->w * 4;
	if (end > o->size)
		return -EINVAL;
	if (pitch_out)
		*pitch_out = pitch;
	return 0;
}

/* A guest-backed surface as a cursor: its backing, made current first. */
static int vmw_cursor_surface_src(struct vmw_device *v, struct vmw_surface *s,
				  const struct vmw_cursor_image *img,
				  struct vmw_cursor_src *src)
{
	uint64_t first;
	uint32_t pitch;
	int rc = vmw_cursor_surface_layout(s, img, &first, &pitch);

	if (rc)
		return rc;

#if VMW_CURSOR_SURFACE_READBACK
	/* Not over writes the device has not been given yet: a coherent
	 * surface with dirty boxes holds newer pixels in the guest than the
	 * host has, and a read-back would put the older ones back. */
	if (s->bound && !(s->coherent && vmw_surface_dirty_count(s) != 0)) {
		SVGA3dCmdReadbackGBSurface body = { .sid = s->sid };
		struct drm_fence *fence;

		if (vmw_cmd_one(v, SVGA_3D_CMD_READBACK_GB_SURFACE, &body,
				sizeof(body)) == 0) {
			fence = vmw_fence_emit(v, 0);
			if (fence) {
				if (vmw_fence_wait(v, fence, VMW_FENCE_TIMEOUT_NS))
					vmw_cursor_log("cursor surface read-back did not complete\n");
				drm_fence_put(fence);
			}
		}
	}
#else
	(void)v;
#endif

	src->obj = s->backup;
	src->offset = first;
	src->pitch = pitch;
	src->image = NULL;
	return 0;
}

static int vmw_cursor_src_of(struct vmw_device *v,
			     const struct vmw_cursor_image *img,
			     struct vmw_surface *srf, struct vmw_cursor_src *src)
{
	uint32_t pitch;
	int rc;

	mm_memset(src, 0, sizeof(*src));
	if (srf)
		return vmw_cursor_surface_src(v, srf, img, src);
	rc = vmw_cursor_bo_layout(img->obj, img, &pitch);
	if (rc)
		return rc;
	src->obj = img->obj;
	src->offset = img->offset;
	src->pitch = pitch;
	return 0;
}

static enum vmw_cursor_update_type
vmw_cursor_update_type(struct vmw_device *v, struct vmw_surface *srf)
{
	if (srf && srf->snooper.image)
		return VMW_CURSOR_UPDATE_LEGACY;
	if (vmw_cursor_has_mob(v))
		return VMW_CURSOR_UPDATE_MOB;
	return VMW_CURSOR_UPDATE_DEFINE;
}

/* Hand image `img' to the device.  *legacy says whether it is a snooped
 * surface (whose snooper id is then in *snoop_id). */
static int vmw_cursor_set_image(struct vmw_device *v,
				struct vmw_cursor_state *st,
				const struct vmw_cursor_image *img,
				uint32_t hot_x, uint32_t hot_y, bool *legacy,
				size_t *snoop_id)
{
	struct drm_gem_object *o = img->obj;
	uint32_t w = img->w, h = img->h;
	struct vmw_surface *srf = NULL;
	struct vmw_cursor_src src;
	int rc = 0;

	*legacy = false;
	if (o->kind == DRM_GEM_SURFACE) {
		srf = o->priv;
		if (!srf)
			return -EINVAL;
	}
	if (w == 0 || h == 0)
		return -EINVAL;

	switch (vmw_cursor_update_type(v, srf)) {
	case VMW_CURSOR_UPDATE_LEGACY:
		if (w != VMW_CURSOR_SNOOP_WIDTH || h != VMW_CURSOR_SNOOP_HEIGHT) {
			vmw_cursor_log("invalid cursor dimensions (%u, %u)\n", w, h);
			return -EINVAL;
		}
		*snoop_id = srf->snooper.id;
		rc = vmw_cursor_define(st, srf->snooper.image, w, h, hot_x, hot_y);
		if (rc == 0)
			*legacy = true;
		return rc;
	case VMW_CURSOR_UPDATE_MOB:
		rc = vmw_cursor_src_of(v, img, srf, &src);
		if (rc)
			return rc;
		if (vmw_cursor_mob_fits(v, w, h)) {
			rc = vmw_cursor_update_mob(v, st, &src, w, h, hot_x, hot_y);
			if (rc == 0)
				return 0;
		}
		/* No MOB to be had: a define, if the image is small enough. */
		if (w > VMW_CURSOR_DEFINE_MAX || h > VMW_CURSOR_DEFINE_MAX) {
			if (!rc)
				vmw_cursor_log("cursor dimensions (%u, %u) exceed the device's maximum\n",
					       w, h);
			return rc ? rc : -EINVAL;
		}
		break;
	case VMW_CURSOR_UPDATE_DEFINE:
		if (w > VMW_CURSOR_DEFINE_MAX || h > VMW_CURSOR_DEFINE_MAX)
			return -EINVAL;
		rc = vmw_cursor_src_of(v, img, srf, &src);
		if (rc)
			return rc;
		break;
	case VMW_CURSOR_UPDATE_NONE:
	default:
		return -EINVAL;
	}

	rc = vmw_cursor_src_gather(&src, w, h, st->image);
	if (rc)
		return rc;
	return vmw_cursor_define(st, st->image, w, h, hot_x, hot_y);
}

/* ---- drm_driver hooks ------------------------------------------------- */

int vmw_cursor_set(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_gem_object *o, uint32_t w, uint32_t h,
		   int32_t hot_x, int32_t hot_y)
{
	struct vmw_device *v = dev->priv;
	struct vmw_cursor_state *st;
	struct vmw_cursor_unit *u;
	struct drm_gem_object *drop = NULL;
	bool legacy = false;
	size_t snoop_id = 0;
	int rc;

	mm_write_lock(&vmw_cursor_lock);
	st = vmw_cursor_state_get(v);
	if (!st) {
		rc = -ENOMEM;
		goto out;
	}
	u = vmw_cursor_unit(st, crtc);
	if (!u) {
		rc = -EINVAL;
		goto out;
	}

	if (!o) {
		/* No image: off the screen until the next one. */
		u->shown = false;
		if (u->snoop_obj) {
			drop = u->snoop_obj;
			u->snoop_obj = NULL;
			st->snoop_units--;
		}
		(void)vmw_cursor_hw_hide(u->pos_x, u->pos_y);
		rc = 0;
		goto out;
	}

	/* The legacy call's object holds the image packed from its start. */
	struct vmw_cursor_image img = {
		.obj = o, .offset = 0, .pitch = w * 4, .x = 0, .y = 0,
		.w = w, .h = h,
	};
	uint32_t def_hot_x = vmw_cursor_hot(u->legacy_hot_x, hot_x);
	uint32_t def_hot_y = vmw_cursor_hot(u->legacy_hot_y, hot_y);

	rc = vmw_cursor_set_image(v, st, &img, def_hot_x, def_hot_y, &legacy,
				  &snoop_id);
	if (rc)
		goto out; /* what was shown stays shown */
	u->def_hot_x = def_hot_x;
	u->def_hot_y = def_hot_y;
	u->stale = false;

	/* A snooped surface is followed after every submission, which needs
	 * the surface to stay alive while it is the cursor. */
	if (u->snoop_obj) {
		drop = u->snoop_obj;
		u->snoop_obj = NULL;
		st->snoop_units--;
	}
	if (legacy) {
		drm_gem_get(o);
		u->snoop_obj = o;
		u->snoop_id = snoop_id;
		st->snoop_units++;
	}
	u->core_hot_x = hot_x;
	u->core_hot_y = hot_y;
	u->width = w;
	u->height = h;
	u->shown = true;
	/* The hotspot may have changed with the image, and the position
	 * written depends on it.  A device without a position mechanism
	 * still has the image defined; that is not an error here. */
	(void)vmw_cursor_unit_position(v, u, crtc, crtc->cursor_x,
				       crtc->cursor_y);
out:
	mm_write_unlock(&vmw_cursor_lock);
	if (drop)
		drm_gem_put(drop);
	return rc;
}

int vmw_cursor_move(struct drm_device *dev, struct drm_crtc *crtc, int x,
		    int y)
{
	struct vmw_device *v = dev->priv;
	struct vmw_cursor_state *st;
	struct vmw_cursor_unit *u;
	int rc;

	mm_write_lock(&vmw_cursor_lock);
	st = vmw_cursor_state_get(v);
	u = st ? vmw_cursor_unit(st, crtc) : NULL;
	if (!u) {
		rc = st ? -EINVAL : -ENOMEM;
		goto out;
	}
	rc = vmw_cursor_unit_position(v, u, crtc, x, y);
	/* Moving a cursor that is not shown is not an error. */
	if (!u->shown)
		rc = 0;
	else if (rc)
		rc = -ENODEV;
out:
	mm_write_unlock(&vmw_cursor_lock);
	return rc;
}

/* ---- cursor planes (atomic mode setting) ---------------------------------- */

/* Could the device show this image at all?  The check of an atomic request
 * and of the legacy CURSOR call; the commit that follows may still fall
 * short (no MOB to be had), which is reported there and not returned. */
static int vmw_cursor_image_check(struct vmw_device *v,
				  const struct vmw_cursor_image *img)
{
	struct vmw_surface *srf = NULL;
	int rc;

	if (!img->obj || img->w == 0 || img->h == 0)
		return -EINVAL;
	if (img->obj->kind == DRM_GEM_SURFACE) {
		srf = img->obj->priv;
		if (!srf)
			return -EINVAL;
	}
	if (srf && srf->snooper.image) {
		/* A snooped legacy surface is shown whole, from the image
		 * the snooper keeps. */
		if (img->w != VMW_CURSOR_SNOOP_WIDTH ||
		    img->h != VMW_CURSOR_SNOOP_HEIGHT || img->x || img->y) {
			vmw_cursor_log("invalid cursor dimensions (%u, %u)\n",
				       img->w, img->h);
			return -EINVAL;
		}
		return 0;
	}
	rc = srf ? vmw_cursor_surface_layout(srf, img, NULL, NULL) :
		   vmw_cursor_bo_layout(img->obj, img, NULL);
	if (rc) {
		vmw_cursor_log("%s not suitable for a %ux%u cursor\n",
			       srf ? "surface" : "buffer", img->w, img->h);
		return rc;
	}
	/* A define command takes up to 64x64; beyond that only a cursor MOB
	 * does, within the device's dimension and size limits. */
	if (img->w <= VMW_CURSOR_DEFINE_MAX && img->h <= VMW_CURSOR_DEFINE_MAX)
		return 0;
	if (vmw_cursor_has_mob(v) && vmw_cursor_mob_fits(v, img->w, img->h))
		return 0;
	vmw_cursor_log("cursor dimensions (%u, %u) exceed the device's maximum\n",
		       img->w, img->h);
	return -EINVAL;
}

int vmw_cursor_obj_check(struct drm_device *dev, struct drm_gem_object *o,
			 uint32_t w, uint32_t h)
{
	/* The legacy call's object holds the image packed from its start. */
	struct vmw_cursor_image img = {
		.obj = o, .offset = 0, .pitch = w * 4, .x = 0, .y = 0,
		.w = w, .h = h,
	};

	return vmw_cursor_image_check(dev->priv, &img);
}

/* The image a cursor plane state names: its source rectangle of the
 * framebuffer, at the size it is shown (planes do not scale). */
static void vmw_cursor_plane_image(struct drm_framebuffer *fb, uint32_t src_x,
				   uint32_t src_y, uint32_t w, uint32_t h,
				   struct vmw_cursor_image *img)
{
	img->obj = fb->obj;
	img->x = src_x;
	img->y = src_y;
	img->w = w;
	img->h = h;
	img->pitch = fb->pitch;
	img->offset = (uint64_t)fb->offset + (uint64_t)src_y * fb->pitch +
		      (uint64_t)src_x * 4;
}

int vmw_cursor_plane_check(struct vmw_device *v,
			   const struct drm_plane_state *ps)
{
	struct vmw_cursor_image img;

	if (!ps->fb)
		return 0; /* switched off */
	/* What the define command and the cursor MOB carry. */
	if (ps->fb->format != DRM_FORMAT_ARGB8888) {
		vmw_cursor_log("cursor framebuffer format %08x is not ARGB8888\n",
			       ps->fb->format);
		return -EINVAL;
	}
	if (!ps->fb->obj)
		return -EINVAL;
	vmw_cursor_plane_image(ps->fb, ps->src_x >> 16, ps->src_y >> 16,
			       ps->crtc_w, ps->crtc_h, &img);
	return vmw_cursor_image_check(v, &img);
}

int vmw_cursor_plane_update(struct vmw_device *v, struct drm_crtc *crtc,
			    const struct vmw_cursor_plane_args *a)
{
	struct vmw_cursor_state *st;
	struct vmw_cursor_unit *u;
	struct drm_gem_object *drop = NULL;
	bool legacy = false;
	size_t snoop_id = 0;
	int rc;

	mm_write_lock(&vmw_cursor_lock);
	st = vmw_cursor_state_get(v);
	u = st ? vmw_cursor_unit(st, crtc) : NULL;
	if (!u)
		goto out;

	if (!a->fb) {
		/* No image: off the screen until the next one. */
		u->shown = false;
		u->stale = false;
		u->width = 0;
		u->height = 0;
		if (u->snoop_obj) {
			drop = u->snoop_obj;
			u->snoop_obj = NULL;
			st->snoop_units--;
		}
		(void)vmw_cursor_hw_hide(u->pos_x, u->pos_y);
		goto out;
	}

	uint32_t def_hot_x = vmw_cursor_hot(u->legacy_hot_x, a->hot_x);
	uint32_t def_hot_y = vmw_cursor_hot(u->legacy_hot_y, a->hot_y);
	bool need_image = a->image_changed || u->stale || !u->width ||
			  u->width != a->w || u->height != a->h ||
			  u->def_hot_x != def_hot_x || u->def_hot_y != def_hot_y;

	if (!a->on || st->suspended) {
		/* The crtc is off: nothing on the screen.  An image that
		 * changed meanwhile is defined when it comes back. */
		u->shown = false;
		if (need_image)
			u->stale = true;
		(void)vmw_cursor_hw_hide(u->pos_x, u->pos_y);
		goto out;
	}

	if (need_image) {
		struct vmw_cursor_image img;

		vmw_cursor_plane_image(a->fb, a->src_x, a->src_y, a->w, a->h,
				       &img);
		rc = img.obj ? vmw_cursor_set_image(v, st, &img, def_hot_x,
						    def_hot_y, &legacy,
						    &snoop_id) :
			       -EINVAL;
		if (rc) {
			/* Reported, never returned: the request was checked,
			 * and what was shown stays shown. */
			vmw_cursor_log("cursor image %ux%u not shown (%d)\n",
				       a->w, a->h, rc);
			if (!u->width) {
				u->shown = false;
				(void)vmw_cursor_hw_hide(u->pos_x, u->pos_y);
				goto out;
			}
		} else {
			/* A snooped surface is followed after every
			 * submission, which needs it alive while it is the
			 * cursor. */
			if (u->snoop_obj) {
				drop = u->snoop_obj;
				u->snoop_obj = NULL;
				st->snoop_units--;
			}
			if (legacy) {
				drm_gem_get(img.obj);
				u->snoop_obj = img.obj;
				u->snoop_id = snoop_id;
				st->snoop_units++;
			}
			u->core_hot_x = a->hot_x;
			u->core_hot_y = a->hot_y;
			u->def_hot_x = def_hot_x;
			u->def_hot_y = def_hot_y;
			u->width = a->w;
			u->height = a->h;
			u->stale = false;
		}
	}
	u->shown = true;
	/* The position in the request: the crtc's own cursor position is
	 * only brought up to date after the commit. */
	(void)vmw_cursor_unit_position(v, u, crtc, a->x, a->y);
out:
	mm_write_unlock(&vmw_cursor_lock);
	if (drop)
		drm_gem_put(drop);
	return 0;
}

/* ---- power management --------------------------------------------------- */

void vmw_cursor_suspend(struct vmw_device *v)
{
	struct vmw_cursor_state *st;
	int32_t x = 0, y = 0;

	mm_write_lock(&vmw_cursor_lock);
	st = v->cursor;
	if (st) {
		st->suspended = true;
		for (uint32_t i = 0; i < DRM_MAX_CRTCS; i++) {
			if (st->unit[i].shown) {
				x = st->unit[i].pos_x;
				y = st->unit[i].pos_y;
			}
		}
	}
	/* Off the screen, the image and every unit's state kept. */
	(void)vmw_cursor_hw_hide(x, y);
	mm_write_unlock(&vmw_cursor_lock);
}

void vmw_cursor_resume(struct vmw_device *v, int reinit)
{
	struct vmw_cursor_state *st;

	mm_write_lock(&vmw_cursor_lock);
	st = v->cursor;
	if (!st)
		goto out;
	st->suspended = false;
	if (reinit) {
		/* The device lost its state.  The hardware layer defines the
		 * last define-command image again itself; a MOB image needs
		 * the device pointed at the MOB once more (redefined by the
		 * rebuild under the same id).  Every unit's image is also
		 * marked stale, so the display replay that follows defines
		 * it again even where the request leaves the framebuffer as
		 * it was. */
		struct vmw_bo *b = st->mob_cur ? st->mob_cur->priv : NULL;

		if (st->mob_shown && b && b->mob.id != SVGA3D_INVALID_ID &&
		    vmw_cursor_has_mob(v)) {
			vmsvga2_hw_write_reg(SVGA_REG_CURSOR_MOBID, b->mob.id);
			vmsvga2_hw_cursor_forget();
		}
		for (uint32_t i = 0; i < DRM_MAX_CRTCS; i++)
			if (st->unit[i].width)
				st->unit[i].stale = true;
	}
	/* Back where it was, on the screen again where it was shown. */
	for (uint32_t i = 0; i < DRM_MAX_CRTCS; i++) {
		struct vmw_cursor_unit *u = &st->unit[i];

		if (u->shown)
			(void)vmsvga2_hw_cursor_update_position(true, u->pos_x,
								u->pos_y);
	}
out:
	mm_write_unlock(&vmw_cursor_lock);
}

/* ---- set-up ------------------------------------------------------------- */

int vmw_cursor_init(struct vmw_device *v)
{
	struct vmw_cursor_state *st;

	mm_write_lock(&vmw_cursor_lock);
	st = vmw_cursor_state_get(v);
	mm_write_unlock(&vmw_cursor_lock);
	return st ? 0 : -ENOMEM;
}

void vmw_cursor_takedown(struct vmw_device *v)
{
	struct drm_gem_object *drop[DRM_MAX_CRTCS + VMW_CURSOR_MOB_CACHE + 1];
	struct vmw_cursor_state *st;
	uint32_t n = 0;

	mm_write_lock(&vmw_cursor_lock);
	st = v->cursor;
	if (!st) {
		mm_write_unlock(&vmw_cursor_lock);
		return;
	}
	v->cursor = NULL;
	(void)vmw_cursor_hw_hide(0, 0);
	for (uint32_t i = 0; i < DRM_MAX_CRTCS; i++)
		if (st->unit[i].snoop_obj)
			drop[n++] = st->unit[i].snoop_obj;
	for (uint32_t i = 0; i < VMW_CURSOR_MOB_CACHE; i++)
		if (st->mob_cache[i])
			drop[n++] = st->mob_cache[i];
	if (st->mob_cur)
		drop[n++] = st->mob_cur;
	mm_write_unlock(&vmw_cursor_lock);

	for (uint32_t i = 0; i < n; i++)
		drm_gem_put(drop[i]);
	kfree(st);
}

/* ---- DRM_VMW_CURSOR_BYPASS -------------------------------------------- */

/* A display server's hotspot for one crtc or for all of them, added to the
 * hotspot of every image from then on.  Takes effect with the next image
 * or move. */
long vmw_ioctl_cursor_bypass(struct vmw_device *v, struct drm_file *fp,
			     struct drm_vmw_cursor_bypass_arg *arg)
{
	struct vmw_cursor_state *st;
	struct drm_crtc *crtc;
	long ret = 0;

	/* The display's owner only. */
	if (!fp->is_master)
		return -EACCES;

	mm_write_lock(&vmw_cursor_lock);
	st = vmw_cursor_state_get(v);
	if (!st) {
		ret = -ENOMEM;
		goto out;
	}

	if (arg->flags & DRM_VMW_CURSOR_BYPASS_ALL) {
		for (uint32_t i = 0; i < v->drm.ncrtc && i < DRM_MAX_CRTCS; i++) {
			st->unit[i].legacy_hot_x = arg->xhot;
			st->unit[i].legacy_hot_y = arg->yhot;
		}
		goto out;
	}

	crtc = drm_crtc_find(&v->drm, arg->crtc_id);
	if (!crtc || !vmw_cursor_unit(st, crtc)) {
		ret = -ENOENT;
		goto out;
	}
	st->unit[crtc->index].legacy_hot_x = arg->xhot;
	st->unit[crtc->index].legacy_hot_y = arg->yhot;
out:
	mm_write_unlock(&vmw_cursor_lock);
	return ret;
}

/* ---- snooping of legacy cursor surfaces ---------------------------------- */

int vmw_cursor_snooper_create(struct drm_file *fp, struct vmw_surface *srf)
{
	const SVGA3dSurfaceDesc *desc;
	uint32_t cursor_size_bytes;
	uint32_t *image;

	srf->snooper.image = NULL;
	srf->snooper.id = 0;
	/* An atomic client sets its cursor through planes, never by DMA
	 * into a surface. */
	if (fp && (fp->client_caps & (1ULL << DRM_CLIENT_CAP_ATOMIC)))
		return 0;
	if (!srf->scanout || srf->mip_levels != 1 || srf->array_size > 1 ||
	    (srf->flags & SVGA3D_SURFACE_CUBEMAP) ||
	    srf->base_size.width != VMW_CURSOR_SNOOP_WIDTH ||
	    srf->base_size.height != VMW_CURSOR_SNOOP_HEIGHT ||
	    srf->format != VMW_CURSOR_SNOOP_FORMAT)
		return 0;

	desc = vmw_surface_get_desc(VMW_CURSOR_SNOOP_FORMAT);
	cursor_size_bytes = VMW_CURSOR_SNOOP_WIDTH * VMW_CURSOR_SNOOP_HEIGHT *
			    desc->pitchBytesPerBlock;
	image = kalloc(cursor_size_bytes);
	if (!image) {
		vmw_cursor_log("failed to allocate a cursor image\n");
		return -ENOMEM;
	}
	mm_memset(image, 0, cursor_size_bytes);
	srf->snooper.image = image;
	return 0;
}

void vmw_cursor_cmd_dma_snoop(SVGA3dCmdHeader *header, struct vmw_surface *srf,
			      struct drm_gem_object *bo)
{
	struct vmw_dma_cmd {
		SVGA3dCmdHeader header;
		SVGA3dCmdSurfaceDMA dma;
	} *cmd;
	const SVGA3dSurfaceDesc *desc =
		vmw_surface_get_desc(VMW_CURSOR_SNOOP_FORMAT);
	const uint32_t image_pitch =
		VMW_CURSOR_SNOOP_WIDTH * desc->pitchBytesPerBlock;
	uint8_t *image;
	SVGA3dCopyBox *box;
	uint32_t box_count, row_bytes;
	uint64_t last;

	/* No snooper installed, nothing to copy. */
	if (!srf || !srf->snooper.image)
		return;
	cmd = container_of(header, struct vmw_dma_cmd, header);

	if (cmd->dma.host.face != 0 || cmd->dma.host.mipmap != 0) {
		vmw_cursor_log("face and mipmap for cursors should never != 0\n");
		return;
	}
	/* The body, one copy box and the suffix. */
	if (cmd->header.size < 64) {
		vmw_cursor_log("at least one full copy box must be given\n");
		return;
	}

	box = (SVGA3dCopyBox *)&cmd[1];
	box_count = (cmd->header.size - sizeof(SVGA3dCmdSurfaceDMA)) /
		    sizeof(SVGA3dCopyBox);

	if (cmd->dma.guest.ptr.offset % PAGE_SIZE ||
	    box->x != 0 || box->y != 0 || box->z != 0 ||
	    box->srcx != 0 || box->srcy != 0 || box->srcz != 0 ||
	    box->d != 1 || box_count != 1 ||
	    box->w > VMW_CURSOR_SNOOP_WIDTH || box->h > VMW_CURSOR_SNOOP_HEIGHT) {
		/* Only a page-aligned single box at the origin is followed. */
		vmw_cursor_log("can't snoop dma request for cursor: (%u, %u, %u) (%u, %u, %u) (%ux%ux%u) %u %u\n",
			       box->srcx, box->srcy, box->srcz, box->x, box->y,
			       box->z, box->w, box->h, box->d, box_count,
			       cmd->dma.guest.ptr.offset);
		return;
	}
	/* A read from the host leaves the surface as it was. */
	if (cmd->dma.transfer != SVGA3D_WRITE_HOST_VRAM)
		return;
	if (!bo || !bo->pages || box->w == 0 || box->h == 0)
		return;

	row_bytes = box->w * desc->pitchBytesPerBlock;
	last = (uint64_t)cmd->dma.guest.ptr.offset +
	       (uint64_t)(box->h - 1) * cmd->dma.guest.pitch + row_bytes;
	if (last > bo->size) {
		vmw_cursor_log("cursor dma reads past the end of its buffer\n");
		return;
	}

	image = (uint8_t *)srf->snooper.image;
	if (box->w == VMW_CURSOR_SNOOP_WIDTH &&
	    cmd->dma.guest.pitch == image_pitch) {
		if (vmw_cursor_obj_read(bo, cmd->dma.guest.ptr.offset, image,
					box->h * image_pitch))
			return;
	} else {
		/* Row by row; `image' is addressed in bytes. */
		for (uint32_t i = 0; i < box->h; i++) {
			if (vmw_cursor_obj_read(bo,
						(uint64_t)cmd->dma.guest.ptr.offset +
							(uint64_t)i * cmd->dma.guest.pitch,
						image + (size_t)i * image_pitch,
						row_bytes))
				return;
		}
	}
	srf->snooper.id++;
}

void vmw_kms_cursor_post_execbuf(struct vmw_device *v)
{
	struct vmw_cursor_state *st = v->cursor;

	if (!st || !__atomic_load_n(&st->snoop_units, __ATOMIC_ACQUIRE))
		return;

	mm_write_lock(&vmw_cursor_lock);
	st = v->cursor;
	for (uint32_t i = 0; st && i < DRM_MAX_CRTCS; i++) {
		struct vmw_cursor_unit *u = &st->unit[i];
		struct vmw_surface *srf;

		if (!u->snoop_obj || !u->shown)
			continue;
		srf = u->snoop_obj->priv;
		if (!srf || !srf->snooper.image || u->snoop_id == srf->snooper.id)
			continue;
		u->snoop_id = srf->snooper.id;
		(void)vmw_cursor_define(st, srf->snooper.image,
					VMW_CURSOR_SNOOP_WIDTH,
					VMW_CURSOR_SNOOP_HEIGHT,
					vmw_cursor_hot(u->legacy_hot_x,
						       u->core_hot_x),
					vmw_cursor_hot(u->legacy_hot_y,
						       u->core_hot_y));
	}
	mm_write_unlock(&vmw_cursor_lock);
}
