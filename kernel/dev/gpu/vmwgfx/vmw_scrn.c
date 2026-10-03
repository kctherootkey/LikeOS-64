// LikeOS -- vmwgfx: scan-out through screen objects.
//
// A screen object is a screen the host composes on its own: defined with a
// size and a place in the host's desktop (DEFINE_SCREEN), kept in a backing
// store, and drawn onto with blits.  This is the display path of a device
// with guest memory regions (GMRs) and SVGA_FIFO_CAP_SCREEN_OBJECT_2 but no
// guest-backed objects -- VMware with 3D off, VirtualBox and QEMU where they
// offer it.
//
// Two kinds of image reach a screen:
//   - a buffer object bound to a GMR: the region is named once with
//     DEFINE_GMRFB and each dirty rectangle is one BLIT_GMRFB_TO_SCREEN;
//   - a rendered surface, which lives in host memory: one
//     BLIT_SURFACE_TO_SCREEN with a bounding box and the list of
//     rectangles inside it.
// The screen can also be read back into a buffer object
// (BLIT_SCREEN_TO_GMRFB), which is what PRESENT_READBACK asks for.
//
// Every display unit (vmw_kms.h) can carry one screen object, its screen id
// the unit number; unit 0 is the primary screen.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2011-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* Send DESTROY_SCREEN for the primary screen object (unit 0) when its crtc
 * is switched off.  Further units are always destroyed: a screen nothing
 * drives any more would stay in the host's desktop.  The primary has only
 * ever been forgotten here, never destroyed -- the console's fallback and a
 * display server's shutdown both turn the crtc off just before putting the
 * aperture back -- and 0 keeps it that way. */
#define VMW_SOU_DESTROY_PRIMARY_ON_DISABLE 0

/* Rectangles per command.  A command is built on the stack and handed to
 * the channel whole, so these bound its size: 20 + 8 * 32 bytes for a
 * buffer-object blit run, 56 + 16 * 16 for a surface blit. */
#define VMW_SOU_BLITS_PER_CMD 8
#define VMW_SOU_SURFACE_CLIPS_PER_CMD 16

/*
 * SVGA commands that are used by this code.  See the device headers for
 * what each one means.
 */
struct vmw_kms_sou_define_gmrfb {
	uint32_t header;
	SVGAFifoCmdDefineGMRFB body;
} __attribute__((packed));

struct vmw_kms_sou_bo_blit {
	uint32_t header;
	SVGAFifoCmdBlitGMRFBToScreen body;
} __attribute__((packed));

struct vmw_kms_sou_readback_blit {
	uint32_t header;
	SVGAFifoCmdBlitScreenToGMRFB body;
} __attribute__((packed));

struct vmw_kms_sou_dirty_cmd {
	SVGA3dCmdHeader header;
	SVGA3dCmdBlitSurfaceToScreen body;
} __attribute__((packed));

/* The region named, then the blits from it, as one submission: the
 * GMRFB is device state, and a definition from anyone else landing between
 * the two would send these blits from the wrong region. */
struct vmw_sou_bo_batch {
	struct vmw_kms_sou_define_gmrfb gmrfb;
	struct vmw_kms_sou_bo_blit blit[VMW_SOU_BLITS_PER_CMD];
} __attribute__((packed));

struct vmw_sou_readback_batch {
	struct vmw_kms_sou_define_gmrfb gmrfb;
	struct vmw_kms_sou_readback_blit blit[VMW_SOU_BLITS_PER_CMD];
} __attribute__((packed));

struct vmw_sou_surface_batch {
	struct vmw_kms_sou_dirty_cmd cmd;
	SVGASignedRect rect[VMW_SOU_SURFACE_CLIPS_PER_CMD];
} __attribute__((packed));

/* ---- unit state ---------------------------------------------------------- */

static int fb_is_surface(struct drm_framebuffer *fb)
{
	return fb->obj && fb->obj->kind == DRM_GEM_SURFACE;
}

/* A buffer object the host can scan out of directly: bound to a guest
 * memory region, and the image starts where the region does. */
static int fb_is_gmr_scanout(struct drm_framebuffer *fb)
{
	return !fb_is_surface(fb) && fb->obj && fb->obj->priv &&
	       ((struct vmw_bo *)fb->obj->priv)->gmr_id >= 0 &&
	       fb->offset == 0;
}

/* The region of a buffer-object framebuffer, -1 when it has none. */
static int fb_gmr(struct drm_framebuffer *fb)
{
	if (fb_is_surface(fb) || !fb->obj || !fb->obj->priv)
		return -1;
	return ((struct vmw_bo *)fb->obj->priv)->gmr_id;
}

/* Unit 0 as the rest of the driver sees it: screen_defined, screen_w,
 * screen_h and scan_gmr of the device.  Called after every change to a
 * unit's screen object; only unit 0 is published. */
static void sou_publish(struct vmw_device *v, const struct vmw_display_unit *du)
{
	if (du->unit != 0)
		return;
	v->screen_defined = du->sou_defined;
	v->screen_w = du->sou_w;
	v->screen_h = du->sou_h;
	v->scan_gmr = du->sou_scan_gmr;
}

void vmw_sou_forget(struct vmw_device *v, struct vmw_display_unit *du)
{
	du->sou_defined = 0;
	du->sou_backing_size = 0;
	sou_publish(v, du);
}

int vmw_sou_can_carry(struct vmw_device *v, struct drm_framebuffer *fb)
{
	return v->has_screen_object &&
	       (fb_is_surface(fb) ? v->has_3d : fb_is_gmr_scanout(fb));
}

/* Where in the aperture unit `du''s backing store of `size' bytes goes:
 * the lowest offset that overlaps no other unit's.  Unit 0 alone -- the
 * single-head case -- always gets offset 0. */
static int sou_backing_place(struct vmw_device *v, struct vmw_display_unit *du,
			     uint64_t size, uint32_t *offset)
{
	int n = vmw_du_count(v);
	uint64_t cand[VMW_MAX_HEADS + 1];
	int ncand = 0;

	cand[ncand++] = 0;
	for (int i = 0; i < n; i++) {
		struct vmw_display_unit *o = vmw_du(v, i);

		if (o && o != du && o->sou_defined && o->sou_backing_size)
			cand[ncand++] = (uint64_t)o->sou_backing_offset +
					o->sou_backing_size;
	}
	uint64_t best = (~0ULL);

	for (int c = 0; c < ncand; c++) {
		uint64_t start = cand[c];
		int clash = 0;

		if (start + size > v->hw.vram_size || start >= best)
			continue;
		for (int i = 0; i < n && !clash; i++) {
			struct vmw_display_unit *o = vmw_du(v, i);

			if (!o || o == du || !o->sou_defined ||
			    !o->sou_backing_size)
				continue;
			if (start < (uint64_t)o->sou_backing_offset +
					    o->sou_backing_size &&
			    o->sou_backing_offset < start + size)
				clash = 1;
		}
		if (!clash)
			best = start;
	}
	if (best == (~0ULL))
		return -ENOSPC;
	*offset = (uint32_t)best;
	return 0;
}

/* ---- defining and destroying screens ------------------------------------ */

/* Define the unit's screen object.
 *
 * With a buffer object the screen's backing store -- the host's copy of
 * what is on it -- is the framebuffer aperture, and the image is carried
 * onto it with BLIT_GMRFB_TO_SCREEN from the object's guest memory region.
 * That is the arrangement the device documents for a screen object (a
 * backing buffer in VRAM, named SVGA_GMR_FRAMEBUFFER here), and the one
 * the host is known to take.  Naming the object's own region as the backing
 * store instead was never seen accepted: VMware with 3D off refused the
 * definition outright.  It also means the aperture has to hold the screen,
 * which is what bounds the mode -- see vmw_scanout_limits().  With several
 * screens, each has its own part of the aperture (sou_backing_place()).
 *
 * With a surface (a rendered image, which lives in host memory) the screen
 * has no backing store and the image is blitted onto it with
 * BLIT_SURFACE_TO_SCREEN.
 *
 * The screen sits at the unit's place in the host's desktop (gui_x/gui_y,
 * from the host's layout); unit 0 is the primary screen. */
static int vmw_sou_define(struct vmw_device *v, struct vmw_display_unit *du,
			  uint32_t w, uint32_t h, struct drm_gem_object *o)
{
	struct vmw_bo *b = (o && o->kind == DRM_GEM_BO) ? o->priv : NULL;
	struct {
		uint32_t cmdType;
		SVGAScreenObject obj;
	} __attribute__((packed)) cmd;
	int backed = b && b->gmr_id >= 0;
	uint64_t size = (uint64_t)w * h * 4;
	uint32_t offset = 0;

	if (!backed && !(o && o->kind == DRM_GEM_SURFACE))
		return -ENODEV;
	if (backed && sou_backing_place(v, du, size, &offset) != 0)
		return -ENOSPC; /* the aperture cannot hold the backing store */
	mm_memset(&cmd, 0, sizeof(cmd));
	cmd.cmdType = SVGA_CMD_DEFINE_SCREEN;
	cmd.obj.structSize = sizeof(SVGAScreenObject);
	cmd.obj.id = (uint32_t)du->unit;
	cmd.obj.flags = SVGA_SCREEN_MUST_BE_SET | SVGA_SCREEN_HAS_ROOT |
			(du->unit == 0 ? SVGA_SCREEN_IS_PRIMARY : 0);
	cmd.obj.size.width = w;
	cmd.obj.size.height = h;
	cmd.obj.root.x = du->gui_x;
	cmd.obj.root.y = du->gui_y;
	cmd.obj.backingStore.ptr.gmrId = backed ? SVGA_GMR_FRAMEBUFFER : SVGA_GMR_NULL;
	cmd.obj.backingStore.ptr.offset = backed ? offset : 0;
	cmd.obj.backingStore.pitch = backed ? w * 4 : 0;
	cmd.obj.cloneCount = 0;
	if (vmw_cmd_raw(v, &cmd, sizeof(cmd), 1) != 0)
		return -EIO;
	du->sou_defined = 1;
	du->sou_w = w;
	du->sou_h = h;
	du->sou_root_x = du->gui_x;
	du->sou_root_y = du->gui_y;
	du->sou_scan_gmr = backed ? (uint32_t)b->gmr_id : SVGA_GMR_NULL;
	du->sou_backing_offset = offset;
	du->sou_backing_size = backed ? (uint32_t)size : 0;
	sou_publish(v, du);
	return 0;
}

/* Take the unit's screen out of the host's desktop.  The backing store is
 * the aperture, which nothing frees, so there is nothing to wait for. */
static int vmw_sou_destroy(struct vmw_device *v, struct vmw_display_unit *du)
{
	struct {
		uint32_t cmdType;
		SVGAFifoCmdDestroyScreen body;
	} __attribute__((packed)) cmd;
	int rc;

	/* no need to do anything */
	if (!du->sou_defined)
		return 0;
	cmd.cmdType = SVGA_CMD_DESTROY_SCREEN;
	cmd.body.screenId = (uint32_t)du->unit;
	rc = vmw_cmd_raw(v, &cmd, sizeof(cmd), 1);
	vmw_sou_forget(v, du);
	return rc ? -EIO : 0;
}

void vmw_sou_disable(struct vmw_device *v, struct vmw_display_unit *du)
{
	if (du->sou_defined &&
	    (du->unit != 0 || VMW_SOU_DESTROY_PRIMARY_ON_DISABLE)) {
		if (vmw_sou_destroy(v, du) != 0)
			kprintf("[drm] vmwgfx: screen object %d not destroyed\n",
				du->unit);
		return;
	}
	vmw_sou_forget(v, du);
}

/* ---- buffer objects onto the screen ------------------------------------- */

struct vmw_kms_sou_bo_dirty {
	struct vmw_kms_dirty base;
	struct vmw_sou_bo_batch batch;
	int queued;
};

static void vmw_sou_fill_gmrfb(struct vmw_kms_sou_define_gmrfb *gmr,
			       struct drm_framebuffer *fb)
{
	uint32_t depth = fb->depth ? fb->depth : 24;
	uint32_t bpp = fb->bpp ? fb->bpp : 32;

	/* Emulate RGBA support, contrary to svga_reg.h this is not
	 * supported by hosts. This is only a problem if we are reading
	 * this value later and expecting what we uploaded back.
	 */
	if (depth == 32)
		depth = 24;
	mm_memset(gmr, 0, sizeof(*gmr));
	gmr->header = SVGA_CMD_DEFINE_GMRFB;
	gmr->body.format.bitsPerPixel = bpp;
	gmr->body.format.colorDepth = depth;
	gmr->body.format.reserved = 0;
	gmr->body.bytesPerLine = fb->pitch;
	gmr->body.ptr.gmrId = (uint32_t)fb_gmr(fb);
	gmr->body.ptr.offset = fb->offset;
}

static void vmw_sou_bo_clip(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_bo_dirty *bdirty =
		container_of(dirty, struct vmw_kms_sou_bo_dirty, base);
	struct vmw_kms_sou_bo_blit *blit = &bdirty->batch.blit[dirty->num_hits];

	blit->header = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
	blit->body.destScreenId = (uint32_t)dirty->unit->unit;
	blit->body.srcOrigin.x = dirty->fb_x;
	blit->body.srcOrigin.y = dirty->fb_y;
	blit->body.destRect.left = dirty->unit_x1;
	blit->body.destRect.top = dirty->unit_y1;
	blit->body.destRect.right = dirty->unit_x2;
	blit->body.destRect.bottom = dirty->unit_y2;
	dirty->num_hits++;
}

/* Queued, not announced: a window drag arrives here as a run of
 * rectangles, and ringing the doorbell for each one traps out of the
 * virtual machine that many times.  One ring covers the whole run -- see
 * vmw_kms_sou_do_bo_dirty().  Down the driver's own channel, not straight
 * into the FIFO: these blits show a region the command-buffer channel has
 * been filling, and two streams have no order between them.  See
 * vmw_cmd_raw(). */
static void vmw_sou_bo_fifo_commit(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_bo_dirty *bdirty =
		container_of(dirty, struct vmw_kms_sou_bo_dirty, base);
	uint32_t bytes;
	int rc;

	if (!dirty->num_hits)
		return;
	bytes = (uint32_t)(sizeof(struct vmw_kms_sou_define_gmrfb) +
			   sizeof(struct vmw_kms_sou_bo_blit) * dirty->num_hits);
	rc = vmw_cmd_raw(dirty->v, &bdirty->batch, bytes, 0);
	if (rc == 0)
		bdirty->queued = 1;
	else if (!dirty->rc)
		dirty->rc = rc;
}

static int sou_unit_defined(const struct vmw_display_unit *du)
{
	return du->sou_defined;
}

int vmw_kms_sou_do_bo_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			    const struct drm_mode_rect_k *clips,
			    const struct drm_vmw_rect *vclips,
			    uint32_t num_clips, int increment,
			    struct drm_fence **out_fence, int unit)
{
	struct vmw_kms_sou_bo_dirty bdirty;
	int ret;

	if (out_fence)
		*out_fence = NULL;
	if (!fb || fb_gmr(fb) < 0)
		return -ENODEV;
	if (increment < 1 || (num_clips && !clips && !vclips))
		return -EINVAL;

	mm_memset(&bdirty, 0, sizeof(bdirty));
	vmw_sou_fill_gmrfb(&bdirty.batch.gmrfb, fb);
	bdirty.base.fifo_commit = vmw_sou_bo_fifo_commit;
	bdirty.base.clip = vmw_sou_bo_clip;
	bdirty.base.unit_ok = sou_unit_defined;
	bdirty.base.cmd = &bdirty.batch;
	bdirty.base.max_hits = VMW_SOU_BLITS_PER_CMD;
	ret = vmw_kms_helper_dirty(v, fb, clips, vclips, 0, 0, num_clips,
				   increment, unit, &bdirty.base);
	if (ret == 0)
		ret = bdirty.base.rc;
	if (bdirty.queued) {
		/* Announce the run.  With the command-buffer channel up the
		 * blits were gathered rather than written to the FIFO, so
		 * handing the gathered batch over IS the announcement. */
		if (v->cb_ready)
			vmw_cmd_flush(v);
		else
			vmsvga2_hw_doorbell();
		if (out_fence)
			*out_fence = vmw_fence_emit(v, DRM_VMW_FENCE_FLAG_EXEC);
	}
	return ret;
}

/* ---- surfaces onto the screen ------------------------------------------- */

/**
 * struct vmw_kms_sou_surface_dirty - Closure structure for
 * blit surface to screen command.
 * @base: The base type we derive from. Used by vmw_kms_helper_dirty().
 * @left: Left side of bounding box.
 * @right: Right side of bounding box.
 * @top: Top side of bounding box.
 * @bottom: Bottom side of bounding box.
 * @dst_x: Difference between source clip rects and framebuffer coordinates.
 * @dst_y: Difference between source clip rects and framebuffer coordinates.
 * @sid: Surface id of surface to copy from.
 * @emitted: A command went to the device.
 * @batch: The command being built.
 */
struct vmw_kms_sou_surface_dirty {
	struct vmw_kms_dirty base;
	s32 left, right, top, bottom;
	s32 dst_x, dst_y;
	u32 sid;
	int emitted;
	struct vmw_sou_surface_batch batch;
};

static void vmw_sou_surface_fifo_commit(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_surface_dirty *sdirty =
		container_of(dirty, struct vmw_kms_sou_surface_dirty, base);
	struct vmw_kms_sou_dirty_cmd *cmd = &sdirty->batch.cmd;
	s32 trans_x = dirty->unit->crtc_x - sdirty->dst_x;
	s32 trans_y = dirty->unit->crtc_y - sdirty->dst_y;
	SVGASignedRect *blit = sdirty->batch.rect;
	uint32_t region_size;
	int rc;

	if (!dirty->num_hits)
		return;

	/* A single rectangle IS its bounding box: the command goes without
	 * a list, which is the form a one-rectangle update always took. */
	region_size = dirty->num_hits > 1 ?
			      dirty->num_hits * sizeof(SVGASignedRect) : 0;
	cmd->header.id = SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN;
	cmd->header.size = sizeof(cmd->body) + region_size;

	/*
	 * Use the destination bounding box to specify destination - and
	 * source bounding regions.
	 */
	cmd->body.destRect.left = sdirty->left;
	cmd->body.destRect.right = sdirty->right;
	cmd->body.destRect.top = sdirty->top;
	cmd->body.destRect.bottom = sdirty->bottom;

	cmd->body.srcRect.left = sdirty->left + trans_x;
	cmd->body.srcRect.right = sdirty->right + trans_x;
	cmd->body.srcRect.top = sdirty->top + trans_y;
	cmd->body.srcRect.bottom = sdirty->bottom + trans_y;

	cmd->body.srcImage.sid = sdirty->sid;
	cmd->body.srcImage.face = 0;
	cmd->body.srcImage.mipmap = 0;
	cmd->body.destScreenId = (uint32_t)dirty->unit->unit;

	/* Blits are relative to the destination rect. Translate. */
	for (u32 i = 0; i < dirty->num_hits; ++i, ++blit) {
		blit->left -= sdirty->left;
		blit->right -= sdirty->left;
		blit->top -= sdirty->top;
		blit->bottom -= sdirty->top;
	}

	rc = vmw_cmd_submit(dirty->v, &sdirty->batch,
			    (uint32_t)(sizeof(*cmd) + region_size),
			    SVGA3D_INVALID_ID);
	if (rc == 0)
		sdirty->emitted = 1;
	else if (!dirty->rc)
		dirty->rc = rc;

	sdirty->left = sdirty->top = S32_MAX;
	sdirty->right = sdirty->bottom = INT_MIN;
}

static void vmw_sou_surface_clip(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_surface_dirty *sdirty =
		container_of(dirty, struct vmw_kms_sou_surface_dirty, base);
	SVGASignedRect *blit = &sdirty->batch.rect[dirty->num_hits];

	/* Destination rect. */
	blit->left = dirty->unit_x1;
	blit->top = dirty->unit_y1;
	blit->right = dirty->unit_x2;
	blit->bottom = dirty->unit_y2;

	/* Destination bounding box */
	sdirty->left = min_t(s32, sdirty->left, dirty->unit_x1);
	sdirty->top = min_t(s32, sdirty->top, dirty->unit_y1);
	sdirty->right = max_t(s32, sdirty->right, dirty->unit_x2);
	sdirty->bottom = max_t(s32, sdirty->bottom, dirty->unit_y2);

	dirty->num_hits++;
}

int vmw_kms_sou_do_surface_dirty(struct vmw_device *v,
				 struct drm_framebuffer *fb,
				 const struct drm_mode_rect_k *clips,
				 const struct drm_vmw_rect *vclips,
				 struct vmw_surface *srf, s32 dest_x, s32 dest_y,
				 uint32_t num_clips, int inc,
				 struct drm_fence **out_fence, int unit)
{
	struct vmw_kms_sou_surface_dirty *sdirty;
	int ret;

	if (out_fence)
		*out_fence = NULL;
	if (!srf && fb && fb_is_surface(fb))
		srf = fb->obj->priv;
	if (!srf)
		return -ENODEV;
	if (inc < 1 || (num_clips && !clips && !vclips))
		return -EINVAL;
	/* Screen objects carry a surface only on a device WITHOUT
	 * guest-backed objects.  Where there are GB surfaces,
	 * BLIT_SURFACE_TO_SCREEN takes the command and shows nothing -- a
	 * silent success that reads as a black screen -- so the caller must
	 * hear that this path cannot show it. */
	if (v->has_gb)
		return -ENODEV;

	/* Over 300 bytes of command: not on the stack of a caller that may
	 * already be deep in a display server's ioctl. */
	sdirty = kalloc(sizeof(*sdirty));
	if (!sdirty)
		return -ENOMEM;
	mm_memset(sdirty, 0, sizeof(*sdirty));
	sdirty->base.fifo_commit = vmw_sou_surface_fifo_commit;
	sdirty->base.clip = vmw_sou_surface_clip;
	sdirty->base.unit_ok = sou_unit_defined;
	sdirty->base.cmd = &sdirty->batch;
	sdirty->base.max_hits = VMW_SOU_SURFACE_CLIPS_PER_CMD;
	sdirty->sid = srf->sid;
	sdirty->left = sdirty->top = S32_MAX;
	sdirty->right = sdirty->bottom = INT_MIN;
	sdirty->dst_x = dest_x;
	sdirty->dst_y = dest_y;

	ret = vmw_kms_helper_dirty(v, fb, clips, vclips, dest_x, dest_y,
				   num_clips, inc, unit, &sdirty->base);
	if (ret == 0)
		ret = sdirty->base.rc;
	if (sdirty->emitted) {
		/* Let the host consume it promptly. */
		struct drm_fence *f =
			vmw_fence_emit(v, out_fence ? DRM_VMW_FENCE_FLAG_EXEC : 0);

		if (out_fence)
			*out_fence = f;
		else if (f)
			drm_fence_put(f);
	}
	kfree(sdirty);
	return ret;
}

/* ---- the display hooks' screen-object halves ----------------------------- */

/* The whole of what the unit shows: its mode's rectangle of the
 * framebuffer. */
static void sou_unit_rect(const struct vmw_display_unit *du,
			  struct drm_mode_rect_k *r)
{
	r->x1 = du->crtc_x;
	r->y1 = du->crtc_y;
	r->x2 = du->crtc_x + (int32_t)du->w;
	r->y2 = du->crtc_y + (int32_t)du->h;
}

int vmw_sou_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		     const struct drm_mode_modeinfo *mode,
		     struct drm_framebuffer *fb)
{
	struct drm_mode_rect_k all;

	sou_unit_rect(du, &all);
	/* Screen objects carry a surface only on a device WITHOUT guest-backed
	 * objects.  Where there are GB surfaces, BLIT_SURFACE_TO_SCREEN takes
	 * the command and shows nothing -- a silent success that reads as a
	 * black screen -- so the screen target is the only path, and if it
	 * could not carry the mode the caller must hear about it. */
	if (v->has_screen_object && fb_is_surface(fb) && v->has_3d && !v->has_gb) {
		vmw_legacy_display_retire(v);
		if (vmw_sou_define(v, du, mode->hdisplay, mode->vdisplay, fb->obj) == 0)
			return vmw_kms_sou_do_surface_dirty(v, fb, &all, NULL,
							    NULL, 0, 0, 1, 1,
							    NULL, du->unit);
	}
	if (v->has_screen_object && fb_is_gmr_scanout(fb)) {
		/* The same two queues as for a screen target: the legacy
		 * console's updates are still in the FIFO, the screen goes
		 * down the command-buffer channel.  Drain the one before the
		 * other says anything. */
		vmw_legacy_display_retire(v);
		if (vmw_sou_define(v, du, mode->hdisplay, mode->vdisplay, fb->obj) == 0)
			return vmw_kms_sou_do_bo_dirty(v, fb, &all, NULL, 1, 1,
						       NULL, du->unit);
	}
	return 1;
}

int vmw_sou_fb_dirty(struct vmw_device *v, struct vmw_display_unit *du,
		     struct drm_framebuffer *fb,
		     const struct drm_mode_rect_k *rects, uint32_t n)
{
	if (!du->sou_defined)
		return 1;
	if (fb_is_surface(fb)) {
		if (v->has_gb)
			return 1;
		return vmw_kms_sou_do_surface_dirty(v, fb, rects, NULL, NULL,
						    0, 0, n, 1, NULL, du->unit);
	}
	/* The region on the screen: anything else is not what it shows.  (A
	 * buffer with no region, while the screen shows a surface, compares
	 * equal here and is refused by the blit: it has nothing to blit
	 * from, and the aperture is no backing store of that screen.) */
	if (!fb->obj || !fb->obj->priv ||
	    ((struct vmw_bo *)fb->obj->priv)->gmr_id != (int)du->sou_scan_gmr)
		return 1;
	return vmw_kms_sou_do_bo_dirty(v, fb, rects, NULL, n, 1, NULL,
				       du->unit);
}

int vmw_sou_page_flip(struct vmw_device *v, struct vmw_display_unit *du,
		      struct drm_framebuffer *fb)
{
	struct drm_mode_rect_k all;

	if (!du->sou_defined)
		return 1;
	sou_unit_rect(du, &all);
	if (fb_is_surface(fb)) {
		if (v->has_gb)
			return 1;
		/* A surface needs a screen without a backing store. */
		if (du->sou_scan_gmr != SVGA_GMR_NULL)
			vmw_sou_define(v, du, du->w, du->h, fb->obj);
		return vmw_kms_sou_do_surface_dirty(v, fb, &all, NULL, NULL,
						    0, 0, 1, 1, NULL, du->unit);
	}
	if (fb_gmr(fb) >= 0 && fb->offset == 0) {
		/* The screen's backing store is the aperture, not the buffer,
		 * so a flip is one blit from the new buffer's region; the
		 * screen stays as defined.  Remember which region is on it
		 * for vmw_fb_dirty(). */
		du->sou_scan_gmr = (uint32_t)fb_gmr(fb);
		sou_publish(v, du);
		return vmw_kms_sou_do_bo_dirty(v, fb, &all, NULL, 1, 1, NULL,
					       du->unit);
	}
	return 1;
}

/* ---- the screen back into a buffer object ------------------------------- */

struct vmw_kms_sou_readback_dirty {
	struct vmw_kms_dirty base;
	struct vmw_sou_readback_batch batch;
	int queued;
};

static void vmw_sou_readback_clip(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_readback_dirty *rdirty =
		container_of(dirty, struct vmw_kms_sou_readback_dirty, base);
	struct vmw_kms_sou_readback_blit *blit =
		&rdirty->batch.blit[dirty->num_hits];

	blit->header = SVGA_CMD_BLIT_SCREEN_TO_GMRFB;
	blit->body.srcScreenId = (uint32_t)dirty->unit->unit;
	blit->body.destOrigin.x = dirty->fb_x;
	blit->body.destOrigin.y = dirty->fb_y;
	blit->body.srcRect.left = dirty->unit_x1;
	blit->body.srcRect.top = dirty->unit_y1;
	blit->body.srcRect.right = dirty->unit_x2;
	blit->body.srcRect.bottom = dirty->unit_y2;
	dirty->num_hits++;
}

static void vmw_sou_readback_fifo_commit(struct vmw_kms_dirty *dirty)
{
	struct vmw_kms_sou_readback_dirty *rdirty =
		container_of(dirty, struct vmw_kms_sou_readback_dirty, base);
	uint32_t bytes;
	int rc;

	if (!dirty->num_hits)
		return;
	bytes = (uint32_t)(sizeof(struct vmw_kms_sou_define_gmrfb) +
			   sizeof(struct vmw_kms_sou_readback_blit) *
				   dirty->num_hits);
	rc = vmw_cmd_raw(dirty->v, &rdirty->batch, bytes, 0);
	if (rc == 0)
		rdirty->queued = 1;
	else if (!dirty->rc)
		dirty->rc = rc;
}

int vmw_kms_sou_readback(struct vmw_device *v, struct drm_file *fp,
			 struct drm_framebuffer *fb, uint64_t user_fence_rep,
			 const struct drm_vmw_rect *vclips, uint32_t num_clips,
			 int unit)
{
	struct vmw_kms_sou_readback_dirty *rdirty;
	struct drm_fence *fence = NULL;
	uint32_t handle = 0;
	int ret;

	/* The screen is copied into the buffer's guest memory region: a
	 * framebuffer without one has nowhere for the pixels to go. */
	if (!fb || fb_gmr(fb) < 0)
		return -EINVAL;
	if (num_clips && !vclips)
		return -EINVAL;

	rdirty = kalloc(sizeof(*rdirty));
	if (!rdirty)
		return -ENOMEM;
	mm_memset(rdirty, 0, sizeof(*rdirty));
	vmw_sou_fill_gmrfb(&rdirty->batch.gmrfb, fb);
	rdirty->base.fifo_commit = vmw_sou_readback_fifo_commit;
	rdirty->base.clip = vmw_sou_readback_clip;
	rdirty->base.unit_ok = sou_unit_defined;
	rdirty->base.cmd = &rdirty->batch;
	rdirty->base.max_hits = VMW_SOU_BLITS_PER_CMD;
	ret = vmw_kms_helper_dirty(v, fb, NULL, vclips, 0, 0, num_clips, 1,
				   unit, &rdirty->base);
	if (ret == 0)
		ret = rdirty->base.rc;
	if (rdirty->queued && !v->cb_ready)
		vmsvga2_hw_doorbell();
	kfree(rdirty);

	/* The copy is the device's: the client learns when it is done from
	 * the fence.  A device without fences is waited for here instead,
	 * since the client then has nothing to wait on. */
	fence = vmw_fence_emit(v, DRM_VMW_FENCE_FLAG_EXEC);
	if (!fence) {
		vmw_cmd_flush(v);
		if (v->cb_ready)
			vmw_cmd_drain(v);
		else
			vmsvga2_fifo_flush();
	}
	if (fence && fp && user_fence_rep &&
	    drm_fence_handle_create(fp, fence, &handle) != 0)
		handle = 0;
	vmw_fence_copy_user_rep(v, fp, ret ? ret : (fence ? 0 : -ENOMEM),
				user_fence_rep, fence, handle, -1);
	if (fence)
		drm_fence_put(fence);
	return ret;
}
