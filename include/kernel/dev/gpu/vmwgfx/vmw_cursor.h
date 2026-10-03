// LikeOS -- vmwgfx: the hardware cursor.
//
// The DRM core hands the driver a cursor image (an ARGB buffer object, or
// with atomic mode setting a cursor plane's framebuffer) and a position.  The image reaches the host as a define command -- or, on a
// device with CAP2_CURSOR_MOB, as a MOB the device reads by id -- and the
// position through the cursor registers.  The register and FIFO primitives
// belong to the hardware layer (vmsvga2_hw_cursor_* / vmsvga2_cursor_*);
// this driver decides which mechanism to use and owns the MOB cursors, the
// surface cursors (read back from the device first) and the cursor snooping
// of legacy surface DMA into a cursor surface.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2024-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_CURSOR_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_CURSOR_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>

struct vmw_device;
struct vmw_surface;
/* Cursor state beyond the hooks (MOB cursors and their recycling); private
 * to vmw_cursor.c. */
struct vmw_cursor_state;

/* The image of a legacy cursor surface, kept up to date by snooping the
 * SURFACE_DMA commands a client writes into it.  `image' is NULL for every
 * surface that is not a cursor. */
struct vmw_cursor_snooper {
	size_t id;
	uint32_t *image;
};

/* drm_driver.cursor_set / cursor_move */
int vmw_cursor_set(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_gem_object *o, uint32_t w, uint32_t h,
		   int32_t hot_x, int32_t hot_y);
int vmw_cursor_move(struct drm_device *dev, struct drm_crtc *crtc, int x,
		    int y);

/* drm_driver.cursor_obj_check: may `o' be shown as a w x h cursor by the
 * legacy CURSOR / CURSOR2 call on an atomic driver?  A buffer object must
 * hold the image packed (w * h * 4 bytes); a surface must be a snooped
 * 64x64 cursor surface, or a guest-backed four-byte BGRA surface of at least
 * that size with its backing in the guest; anything beyond 64x64 needs a
 * device with cursor MOBs that takes the size.  0 or -EINVAL. */
int vmw_cursor_obj_check(struct drm_device *dev, struct drm_gem_object *o,
			 uint32_t w, uint32_t h);

/* ---- cursor planes (atomic mode setting, vmw_kms_atomic.c) ---- */

/* A cursor plane's new state. */
struct vmw_cursor_plane_args {
	struct drm_framebuffer *fb;	/* NULL: the plane shows nothing */
	uint32_t src_x, src_y;		/* the image's top-left in fb, pixels */
	uint32_t w, h;			/* its size */
	int32_t x, y;			/* its top-left on the crtc */
	int32_t hot_x, hot_y;		/* its hotspot (HOTSPOT_X/_Y, CURSOR2) */
	bool on;			/* the crtc runs: show it */
	bool image_changed;		/* the image may not be the one defined */
};

/* Atomic check: can the device show the cursor plane state `ps' (an
 * ARGB8888 framebuffer, an image the device takes at that size)?  0 for a
 * plane without a framebuffer, else 0 or -EINVAL. */
int vmw_cursor_plane_check(struct vmw_device *v,
			   const struct drm_plane_state *ps);
/* Atomic commit: show crtc `crtc''s cursor as `a' says -- the image
 * defined again when it changed (or the hotspot did, or it could not be
 * defined while the crtc was off), the position written, hidden when the
 * plane has no image or the crtc is off.  Never fails: what cannot be shown
 * is reported and the previous image stays.  Returns 0. */
int vmw_cursor_plane_update(struct vmw_device *v, struct drm_crtc *crtc,
			    const struct vmw_cursor_plane_args *a);

/* ---- power management (vmw_pm.c) ----
 *
 * suspend: the cursor off the screen, its image, hotspot and position
 * kept.  resume: back on the screen where it was shown; with `reinit' (the
 * device lost its state) the device is pointed at the cursor MOB again
 * when that is what it showed (the MOB itself was defined again by the
 * rebuild), and every image is marked to be defined again by the next
 * cursor update -- the display replay -- whether or not its framebuffer
 * changed. */
void vmw_cursor_suspend(struct vmw_device *v);
void vmw_cursor_resume(struct vmw_device *v, int reinit);

/* Set up and tear down the cursor state (vmw_device.cursor).  Init returns
 * 0 or -errno; a failure leaves the register/FIFO cursor working. */
int vmw_cursor_init(struct vmw_device *v);
void vmw_cursor_takedown(struct vmw_device *v);

/* DRM_VMW_CURSOR_BYPASS */
long vmw_ioctl_cursor_bypass(struct vmw_device *v, struct drm_file *fp,
			     struct drm_vmw_cursor_bypass_arg *arg);

/* A legacy surface was just created (its format, flags, size, mip levels
 * and scanout flag filled in): give it a snooper image if it can be a
 * cursor -- a single-image 64x64 A8R8G8B8 scanout surface of a client that
 * did not ask for atomic mode setting (srf->snooper.image, freed with the
 * surface).  0, or -ENOMEM. */
int vmw_cursor_snooper_create(struct drm_file *fp, struct vmw_surface *srf);
/* A verified SURFACE_DMA into `srf' from buffer `bo': update the snooped
 * image.  Called by the command verifier.  `header' is the command's
 * SVGA3dCmdHeader, followed in memory by its body (SVGA3dCmdSurfaceDMA,
 * the copy boxes, the suffix) as the client wrote it; `bo' is the buffer
 * the guest pointer names (NULL for none: nothing is copied). */
void vmw_cursor_cmd_dma_snoop(SVGA3dCmdHeader *header, struct vmw_surface *srf,
			      struct drm_gem_object *bo);
/* A submission has been handed over: a crtc whose cursor is a snooped
 * surface that the submission wrote into gets the new image defined.
 * Cheap when no crtc shows a snooped cursor.  Process context; may be
 * called with or without execbuf_lock held (it takes only the cursor
 * lock, which nothing holds while taking execbuf_lock). */
void vmw_kms_cursor_post_execbuf(struct vmw_device *v);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_CURSOR_H */
