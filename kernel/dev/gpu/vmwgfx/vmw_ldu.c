// LikeOS -- vmwgfx: the legacy display unit.
//
// The oldest of the device's scan-out paths, and the one every SVGA II
// device has: the host scans out the framebuffer aperture (VRAM) at the
// geometry of the mode registers, and the driver tells it which rectangles
// changed with SVGA_CMD_UPDATE.  Buffer objects here are guest pages, never
// placed in VRAM, so showing one is a copy of its dirty rectangles into the
// aperture followed by that announcement.
//
// There is one legacy display.  The mode registers describe a single
// screen, and the single-display topology that goes with them is written
// by the console driver's mode set (kernel/dev/video/vmsvga2*.c); units
// beyond the first are only carried by screen objects or screen targets.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* The legacy display path, retired before a screen target takes the screen.
 *
 * Until here the boot console showed itself the old way: pixels in the
 * framebuffer aperture, an SVGA_CMD_UPDATE through the FIFO for every
 * changed rectangle, and SVGA_REG_TRACES on so the host also noticed writes
 * that announced nothing.  The screen target that follows is a different
 * mechanism on the host, fed from a different queue, and the two were left
 * to race: updates of the legacy framebuffer still sitting in the FIFO
 * could be consumed AFTER the target had been defined and painted, and the
 * host then showed that framebuffer -- black, the console having moved out
 * of it -- rather than the target.  Which of the two won depended on how
 * far the host had got with the FIFO at that moment, so the screen came up
 * black on some boots and not on others.
 *
 * So: no more tracing (it belongs to the legacy path; a device driven
 * through command buffers has no use for it), and the FIFO drained --
 * SVGA_REG_SYNC until
 * SVGA_REG_BUSY clears -- so that everything the legacy path ever said has
 * been heard before the target says anything.  Free when nothing is queued,
 * which is every mode set after the first. */
void vmw_legacy_display_retire(struct vmw_device *v)
{
	(void)v;
	vmsvga2_set_traces(0);
	vmsvga2_fifo_flush();
}

/* Legacy: copy rows of the object into VRAM, then announce them.
 *
 * The rectangle is in framebuffer coordinates; the aperture shows the
 * framebuffer from the unit's (crtc_x, crtc_y), so the pixel at (x, y)
 * lands at (x - crtc_x, y - crtc_y) of the screen.  Clipped to both the
 * screen the registers describe and the framebuffer. */
int vmw_ldu_copy(struct vmw_device *v, struct vmw_display_unit *du,
		 struct drm_framebuffer *fb, int x1, int y1, int x2, int y2)
{
	struct drm_gem_object *o = fb->obj;
	uint32_t bpp = fb->bpp / 8;
	int cx = du ? du->crtc_x : 0;
	int cy = du ? du->crtc_y : 0;

	if (!o || o->kind != DRM_GEM_BO)
		return -ENODEV;

	/* Into screen coordinates, and clipped to the screen... */
	x1 -= cx;
	x2 -= cx;
	y1 -= cy;
	y2 -= cy;
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > (int)v->hw.width)
		x2 = (int)v->hw.width;
	if (y2 > (int)v->hw.height)
		y2 = (int)v->hw.height;
	/* ...and to the framebuffer. */
	if (x1 + cx < 0)
		x1 = -cx;
	if (y1 + cy < 0)
		y1 = -cy;
	if (x2 + cx > (int)fb->width)
		x2 = (int)fb->width - cx;
	if (y2 + cy > (int)fb->height)
		y2 = (int)fb->height - cy;
	if (x1 >= x2 || y1 >= y2)
		return 0;
	uint8_t *vram = v->hw.fb_virt + v->hw.fb_offset;
	for (int y = y1; y < y2; y++) {
		uint64_t src_off = fb->offset + (uint64_t)(y + cy) * fb->pitch +
				   (uint64_t)(x1 + cx) * bpp;
		uint64_t bytes = (uint64_t)(x2 - x1) * bpp;
		uint8_t *dst = vram + (uint64_t)y * v->hw.pitch + (uint64_t)x1 * bpp;
		/* The object is page-backed; a row may cross pages. */
		while (bytes) {
			uint32_t page = (uint32_t)(src_off / PAGE_SIZE);
			uint32_t in = (uint32_t)(src_off % PAGE_SIZE);
			uint32_t chunk = (uint32_t)(PAGE_SIZE - in);
			if (chunk > bytes)
				chunk = (uint32_t)bytes;
			uint8_t *src = drm_gem_page_virt(o, page);
			if (!src)
				return -EIO;
			kmemcpy(dst, src + in, chunk);
			dst += chunk;
			src_off += chunk;
			bytes -= chunk;
		}
	}
	vmsvga2_update_rect((uint32_t)x1, (uint32_t)y1, (uint32_t)(x2 - x1),
			    (uint32_t)(y2 - y1));
	return 0;
}

/* Show `fb' on the legacy display.  The mode registers have already been
 * programmed by the caller where they could be (vmw_mode_set()). */
int vmw_ldu_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		     const struct drm_mode_modeinfo *mode,
		     struct drm_framebuffer *fb)
{
	/* One legacy display: the registers describe a single screen. */
	if (du->unit != 0)
		return -EINVAL;
	/* The last path copies into the framebuffer aperture, which is still
	 * whatever geometry the mode registers accepted.  If they would not
	 * take this mode and no screen carried it either, copying anyway
	 * would put the top-left corner of the image on the screen and call
	 * it success.  Refuse instead, and the caller keeps the display it
	 * already had. */
	if (v->hw.width != mode->hdisplay || v->hw.height != mode->vdisplay)
		return -EINVAL;
	/* Legacy: the host reads VRAM; disable its own dirty tracking of
	 * VRAM writes, the copies below announce themselves. */
	vmsvga2_set_traces(0);
	return vmw_ldu_copy(v, du, fb, du->crtc_x, du->crtc_y,
			    du->crtc_x + (int)mode->hdisplay,
			    du->crtc_y + (int)mode->vdisplay);
}
