// LikeOS -- vmwgfx: display.
//
// What each display unit does -- set a mode, go off, flip, show dirty
// rectangles -- and the DRM core's per-call display hooks built on it, taken
// to the display unit of the crtc they name (connector i, encoder i and crtc
// i are unit i).  The atomic commit (vmw_kms_atomic.c) uses the same per-unit
// functions.  The scan-out path that carries a unit is chosen by what the
// host offers:
//   - screen targets (vmw_stdu.c): the image is a guest-backed surface the
//     host scans out directly;
//   - screen objects (vmw_scrn.c): the scan-out buffer is a guest memory
//     region (a buffer object bound to a GMR) and the host reads it
//     directly; a dirty rectangle is one BLIT_GMRFB_TO_SCREEN command;
//   - legacy (vmw_ldu.c): the host scans out VRAM; dirty rectangles are
//     copied from the buffer object into VRAM and announced with
//     SVGA_CMD_UPDATE.
// Also here: the host-driven layout (DRM_VMW_UPDATE_LAYOUT), the client
// presents (PRESENT / PRESENT_READBACK) and the walk over clip rectangles
// and units the paths share (vmw_kms_helper_dirty()).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/video/vmsvga2_hw.h>
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
#include <kernel/dev/gpu/vmwgfx/vmw_connector.h>

/* ---- display units ------------------------------------------------------ */

struct vmw_display_unit *vmw_du(struct vmw_device *v, int unit)
{
	BUILD_BUG_ON(VMW_MAX_HEADS < 1);
	BUILD_BUG_ON(VMW_MAX_HEADS > DRM_MAX_CONNECTORS);

	if (unit < 0 || unit >= VMW_MAX_HEADS)
		return NULL;
	v->du[unit].unit = unit;
	return &v->du[unit];
}

int vmw_du_count(struct vmw_device *v)
{
	int n = (int)v->drm.nconn;

	if (n > VMW_MAX_HEADS)
		n = VMW_MAX_HEADS;
	return n < 1 ? 1 : n;
}

/* The display lock (see vmw_kms_lock() in vmw_kms.h).  One per driver:
 * the device is a single PCI function, so there is never a second
 * vmw_device to keep apart. */
static mm_rwsem_t vmw_kms_lock_sem = MM_RWSEM_INIT("vmw_kms");

void vmw_kms_lock(struct vmw_device *v)
{
	(void)v;
	mm_write_lock(&vmw_kms_lock_sem);
}

void vmw_kms_unlock(struct vmw_device *v)
{
	(void)v;
	mm_write_unlock(&vmw_kms_lock_sem);
}

int vmw_kms_is_atomic(struct vmw_device *v)
{
	return v->drm.drv && v->drm.drv->atomic_commit != NULL;
}

/* The unit of a crtc: its index. */
static struct vmw_display_unit *vmw_crtc_du(struct vmw_device *v,
					    const struct drm_crtc *crtc)
{
	return vmw_du(v, crtc ? crtc->index : 0);
}

void vmw_kms_initial_size(struct vmw_device *v, uint32_t *w, uint32_t *h)
{
	uint32_t iw = 0, ih = 0;

	if (vmsvga2_get_initial_size(&iw, &ih) == 0 && iw && ih &&
	    iw <= v->scanout_max_width && ih <= v->scanout_max_height &&
	    (uint64_t)iw * ih * 4 <= v->scanout_max_mem) {
		*w = iw;
		*h = ih;
		return;
	}
	*w = v->hw.width;
	*h = v->hw.height;
}

#if VMW_MAX_HEADS > 1
/* Sizes offered on a further unit's connector besides its preferred one,
 * where the scan-out can carry them. */
static const uint16_t vmw_du_builtin_modes[][2] = {
	{ 3840, 2160 }, { 2560, 1600 }, { 2560, 1440 }, { 2048, 1152 },
	{ 1920, 1440 }, { 1920, 1200 }, { 1920, 1080 }, { 1856, 1392 },
	{ 1792, 1344 }, { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 },
	{ 1440, 900 },  { 1400, 1050 }, { 1366, 768 },  { 1360, 768 },
	{ 1280, 1024 }, { 1280, 960 },  { 1280, 800 },  { 1280, 768 },
	{ 1280, 720 },  { 1152, 864 },  { 1024, 768 },  { 800, 600 },
	{ 640, 480 },
};

static int vmw_du_mode_fits(const struct vmw_device *v, uint32_t w, uint32_t h)
{
	return w && h && w <= v->scanout_max_width &&
	       h <= v->scanout_max_height &&
	       (uint64_t)w * h * 4 <= v->scanout_max_mem;
}

/* How many units the display path can carry: as many screen targets as
 * the driver drives, every head with screen objects, one legacy display. */
static int vmw_du_possible(struct vmw_device *v)
{
	int n = 1;

	if (vmw_stdu_available(v)) {
		n = vmw_stdu_num_units(v);
		if (n > VMW_MAX_HEADS)
			n = VMW_MAX_HEADS;
	} else if (v->has_screen_object) {
		n = VMW_MAX_HEADS;
	}
	return n;
}
#endif

int vmw_kms_init_display(struct vmw_device *v)
{
	uint32_t iw, ih;

	vmw_kms_initial_size(v, &iw, &ih);
	for (int i = 0; i < VMW_MAX_HEADS; i++) {
		struct vmw_display_unit *du = vmw_du(v, i);

		du->pref_active = i == 0;
		du->pref_w = iw;
		du->pref_h = ih;
	}
#if VMW_MAX_HEADS > 1
	int n = vmw_du_possible(v);

	while ((int)v->drm.nconn < n) {
		uint32_t mm_w = iw * 254 / 960, mm_h = ih * 254 / 960;
		int c = drm_connector_add(&v->drm, DRM_MODE_CONNECTOR_VIRTUAL,
					  mm_w, mm_h);
		struct drm_mode_modeinfo m;

		if (c < 0)
			break;
		/* Not part of the host's desktop until its layout says so. */
		v->drm.conn[c].connected = 0;
		drm_mode_fill(&m, iw, ih, 60, 1);
		drm_connector_add_mode(&v->drm, c, &m);
		for (unsigned k = 0; k < ARRAY_SIZE(vmw_du_builtin_modes); k++) {
			uint32_t sw = vmw_du_builtin_modes[k][0];
			uint32_t sh = vmw_du_builtin_modes[k][1];

			if ((sw == iw && sh == ih) || !vmw_du_mode_fits(v, sw, sh))
				continue;
			drm_mode_fill(&m, sw, sh, 60, 0);
			drm_connector_add_mode(&v->drm, c, &m);
		}
	}
	if (v->drm.nconn > 1)
		kprintf("[drm] vmwgfx: %u display units\n", v->drm.nconn);
#endif
	return 0;
}

/* ---- scan-out ----------------------------------------------------------- */

static int fb_is_surface(struct drm_framebuffer *fb)
{
	return fb->obj && fb->obj->kind == DRM_GEM_SURFACE;
}

/* A rectangle that could not be shown.  Report it, but never hand it back.
 * The X modesetting driver answers ONE failed DIRTYFB by unregistering its
 * damage tracking for the rest of the session ("Disabling kernel dirty
 * updates, not required." -- X_INFO, easy to miss), after which nothing
 * asks for an update again and the screen is frozen whatever the driver
 * does.  A rectangle that could not be shown is worth far less than the
 * next one that could. */
static void vmw_dirty_not_shown(const struct drm_mode_rect_k *r, int rc)
{
	static int budget = 12;
	static int last_rc;

	if (rc != last_rc && budget > 0) {
		budget--;
		last_rc = rc;
		kprintf("[drm] vmwgfx: dirty rectangle %d,%d-%d,%d not shown (%d)\n",
			r->x1, r->y1, r->x2, r->y2, rc);
	}
}

/* Show `fb' on unit `du' at the geometry already recorded in it, through
 * the first path that carries it. */
static int vmw_du_show(struct vmw_device *v, struct vmw_display_unit *du,
		       const struct drm_mode_modeinfo *mode,
		       struct drm_framebuffer *fb)
{
	int rc;

	/* Screen targets first: on a device with guest-backed objects this is
	 * the path the host expects, and the two below may be refused. */
	if (vmw_stdu_available(v)) {
		vmw_legacy_display_retire(v);
		if (vmw_stdu_unit_set_mode(v, du->unit, mode->hdisplay,
					   mode->vdisplay, du->gui_x, du->gui_y,
					   du->crtc_x, du->crtc_y, fb) == 0) {
			vmw_sou_forget(v, du);
			return 0;
		}
		/* Not fatal: fall through to the older paths, which is what a
		 * device that reports screen targets but refuses to define one
		 * leaves as the only way to show anything. */
		vmw_stdu_unit_teardown(v, du->unit);
	}
	/* Screen objects (vmw_scrn.c, which also says why a guest-backed
	 * surface is refused there). */
	rc = vmw_sou_mode_set(v, du, mode, fb);
	if (rc <= 0)
		return rc;
	if (fb_is_surface(fb))
		return -ENODEV; /* no way to show a surface without screen objects */
	vmw_sou_forget(v, du);
	return vmw_ldu_mode_set(v, du, mode, fb);
}

int vmw_du_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		    const struct drm_mode_modeinfo *mode,
		    struct drm_framebuffer *fb, int x, int y)
{
	/* For vmw_display_verify(): refusals from before this mode set are
	 * somebody else's -- a display server's, typically. */
	v->cb_errors_seen = v->cb_errors;

	/* Will a screen object or a screen target carry this mode?  Then its
	 * geometry comes from the command that defines it, and the mode
	 * registers below have no say in it -- and must not be given a veto,
	 * because the limits the device enforces on them are derived from the
	 * framebuffer aperture that this scan-out never reads. */
	int by_screen = vmw_stdu_available(v) || vmw_sou_can_carry(v, fb);

	if (du->unit == 0) {
		/* On a device driven through command buffers the mode
		 * registers are not merely redundant beside a screen target
		 * -- programming them cycles SVGA_REG_ENABLE, and disabling
		 * the device resets it, which stops the command-buffer
		 * contexts: the very next screen-target command comes back
		 * SVGA_CB_STATUS_CB_HEADER_ERROR.  These registers are never
		 * touched again after bring-up on such a device.
		 * (cb_submit_raw() can restart a stopped context now, but not
		 * resetting the device at all is strictly better than
		 * recovering from it.)  The aperture path cannot need them
		 * either: it is only reached when the screen paths are
		 * refused, and it refuses a geometry the registers do not
		 * already carry. */
		if (!(by_screen && v->cb_ready) &&
		    (v->hw.width != mode->hdisplay ||
		     v->hw.height != mode->vdisplay || v->hw.bpp != 32)) {
			/* Where the console scans out of a buffer object of
			 * its own, this mode belongs to the client that asked
			 * for it and the console keeps its framebuffer and its
			 * geometry; it gets its own mode back when that client
			 * drops the display. */
			int rc = drm_console_active(&v->drm) ?
					 vmsvga2_hw_set_mode_device(mode->hdisplay,
								    mode->vdisplay) :
					 vmsvga2_hw_set_mode(mode->hdisplay,
							     mode->vdisplay);
			if (rc != 0 && !by_screen)
				return -EINVAL;
		}
	} else if (!by_screen) {
		/* The mode registers describe one screen: a further unit
		 * exists only as a screen target or a screen object. */
		return -EINVAL;
	}
	vmsvga2_hw_geometry(&v->hw);

	/* The unit's new geometry, which the paths below read; kept only if
	 * one of them shows the framebuffer. */
	uint32_t old_w = du->w, old_h = du->h;
	int32_t old_x = du->crtc_x, old_y = du->crtc_y;

	du->w = mode->hdisplay;
	du->h = mode->vdisplay;
	du->crtc_x = x;
	du->crtc_y = y;
	int rc = vmw_du_show(v, du, mode, fb);

	if (rc) {
		/* A screen object the device now holds at the new size keeps
		 * the geometry it was defined with: later updates to it are
		 * clipped against that. */
		if (du->sou_defined && du->sou_w == du->w && du->sou_h == du->h)
			return rc;
		du->w = old_w;
		du->h = old_h;
		du->crtc_x = old_x;
		du->crtc_y = old_y;
		return rc;
	}
	du->active = 1;
	du->fb_id = fb->id;
	return 0;
}

int vmw_mode_set(struct drm_device *dev, struct drm_crtc *crtc,
		 const struct drm_mode_modeinfo *mode,
		 struct drm_framebuffer *fb, int x, int y)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du = vmw_crtc_du(v, crtc);
	int rc;

	if (!du)
		return -EINVAL;
	/* Video overlay streams are composed onto the screen by the host;
	 * stop them while the screen is redefined under them and start them
	 * again on whatever it became.  Both calls take the overlay lock and
	 * then the command-reservation lock (both sleeping); neither is held
	 * here. */
	vmw_overlay_pause_all(v);
	rc = vmw_du_mode_set(v, du, mode, fb, x, y);
	vmw_overlay_resume_all(v);
	return rc;
}

void vmw_du_disable(struct vmw_device *v, struct vmw_display_unit *du)
{
	if (vmw_stdu_unit_defined(v, du->unit, NULL, NULL))
		vmw_stdu_unit_teardown(v, du->unit);
	vmw_sou_disable(v, du);
	du->active = 0;
	du->fb_id = 0;
}

int vmw_crtc_disable(struct drm_device *dev, struct drm_crtc *crtc)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du = vmw_crtc_du(v, crtc);

	if (!du)
		return 0;
	vmw_du_disable(v, du);
	return 0;
}

/* Did the console's screen come up?  See drm_driver.display_verify.
 *
 * Everything on the screen-target path is queued and executed later, and
 * a buffer the device refuses is repaired by dropping the one command it
 * did not like -- so a define, a bind or an update can vanish without any
 * caller hearing of it, and the first anyone knows is a screen that stays
 * black.  Waiting for the device to finish and then asking whether it
 * refused anything since the mode set is the only test that sees that.  A
 * device that has stopped answering shows up the same way: the wait gives
 * up on it and the buffers it never took are counted as refused.
 *
 * The console is on unit 0. */
int vmw_display_verify(struct drm_device *dev)
{
	struct vmw_device *v = dev->priv;

	if (vmw_stdu_available(v)) {
		vmw_cmd_drain(v);
		if (!vmw_stdu_unit_defined(v, 0, NULL, NULL)) {
			kprintf("[drm] vmwgfx: no screen target after the console's mode set\n");
			return -ENODEV;
		}
	} else if (vmw_du(v, 0)->sou_defined && v->cb_ready) {
		/* A screen object over the command-buffer channel is just as
		 * silent: the define and the blits are queued, and a refused
		 * one is dropped.  Ask the same question. */
		vmw_cmd_drain(v);
	} else {
		return 0; /* the legacy path answers as it goes */
	}
	if (v->cb_errors != v->cb_errors_seen) {
		kprintf("[drm] vmwgfx: the device refused %u command buffer(s) while the console's screen was set up\n",
			v->cb_errors - v->cb_errors_seen);
		return -EIO;
	}
	return 0;
}

/* The screen-target path cannot be trusted: put the display back the way
 * the boot console had it.  See drm_driver.display_fallback.  The console
 * has already stopped drawing into its buffer object and has its old flush
 * hook back, so everything painted from here on goes into the aperture and
 * is announced through the FIFO, exactly as before this driver initialised.
 *
 * The device reset before the legacy mode set is what makes it certain.
 * Disabling the device (SVGA_REG_ENABLE to 0) resets it: whatever screen
 * target the host still holds -- an empty one, if the refused command was
 * the update that should have filled it -- goes with the reset, and the
 * host is back to showing the framebuffer aperture once the mode set
 * enables it again, which no queued command can take away.  The reset also
 * drops the guest-backed state (object tables, contexts), so the driver
 * stops offering it: screen-target scan-out is off for good and 3D with it,
 * and a display server finds the device as it was before 3D was detected
 * -- the framebuffer and screen-object paths, which need nothing of what
 * was lost.  Every unit goes: the reset takes all their screens. */
void vmw_display_fallback(struct drm_device *dev)
{
	struct vmw_device *v = dev->priv;

	for (int i = 0; i < vmw_stdu_num_units(v); i++)
		if (vmw_stdu_unit_defined(v, i, NULL, NULL))
			vmw_stdu_unit_teardown(v, i);
	if (vmw_stdu_available(v)) {
		v->st_refused = 1;
		v->has_gb = 0;
		v->has_dx = 0;
		v->has_3d = 0;
		v->has_sm41 = 0;
		v->has_sm5 = 0;
		v->has_gl43 = 0;
		v->otables_ready = 0;
		v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM41] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM5] = 0;
		v->devcaps[SVGA3D_DEVCAP_GL43] = 0;
		kprintf("[drm] vmwgfx: screen-target display refused; console and scan-out back on the framebuffer, 3D off\n");
	} else {
		/* The screen object was refused: not offered again, so a
		 * display server's mode set takes the aperture path the
		 * console is going back to, rather than repeating this. */
		v->has_screen_object = 0;
		kprintf("[drm] vmwgfx: screen-object display refused; console and scan-out back on the framebuffer\n");
	}
	for (int i = 0; i < vmw_du_count(v); i++) {
		struct vmw_display_unit *du = vmw_du(v, i);

		vmw_sou_forget(v, du);
		du->active = 0;
		du->fb_id = 0;
	}
	vmsvga2_set_traces(1);
	/* The reset, asked for explicitly: the mode set below does not
	 * have to do it. */
	vmsvga2_hw_svga_disable(false);
	if (vmsvga2_hw_set_mode(v->hw.width, v->hw.height) != 0)
		kprintf("[drm] vmwgfx: legacy mode set %ux%u failed as well\n",
			v->hw.width, v->hw.height);
	vmsvga2_hw_geometry(&v->hw);
	vmsvga2_update_rect(0, 0, v->hw.width, v->hw.height);
}

void vmw_du_dirty(struct vmw_device *v, struct vmw_display_unit *du,
		  struct drm_framebuffer *fb,
		  const struct drm_mode_rect_k *rects, uint32_t n)
{
	int unit = du->unit;
	int rc;

	/* A guest-backed surface is only displayable through a screen target;
	 * if the mode set did not leave one up, bring it up now. */
	if (!vmw_stdu_unit_defined(v, unit, NULL, NULL) && fb_is_surface(fb) &&
	    vmw_stdu_available(v))
		vmw_stdu_unit_ensure(v, unit, du->active ? du->w : fb->width,
				     du->active ? du->h : fb->height);

	/* Whatever fails below is reported (vmw_dirty_not_shown()), never
	 * returned. */
	if (vmw_stdu_unit_defined(v, unit, NULL, NULL)) {
		for (uint32_t i = 0; i < n; i++) {
			rc = vmw_stdu_unit_present(v, unit, fb, rects[i].x1,
						   rects[i].y1, rects[i].x2,
						   rects[i].y2, 0);
			if (rc)
				vmw_dirty_not_shown(&rects[i], rc);
		}
		return;
	}
	if (n == 0)
		return;
	rc = vmw_sou_fb_dirty(v, du, fb, rects, n);
	if (rc < 0)
		vmw_dirty_not_shown(&rects[0], rc);
	if (rc <= 0)
		return;
	for (uint32_t i = 0; i < n; i++) {
		rc = unit == 0 ? vmw_ldu_copy(v, du, fb, rects[i].x1,
					      rects[i].y1, rects[i].x2,
					      rects[i].y2) :
				 -ENODEV;
		if (rc)
			vmw_dirty_not_shown(&rects[i], rc);
	}
}

int vmw_fb_dirty(struct drm_device *dev, struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 const struct drm_mode_rect_k *rects, uint32_t n)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du = vmw_crtc_du(v, crtc);

	if (!du || !fb)
		return 0;
	/* With atomic mode setting a crtc can keep its framebuffer while its
	 * unit is off (DPMS), and a unit is off between the display being
	 * handed back and the console's next mode set.  Nothing is shown
	 * there: bringing a screen target up for a rectangle (below) would
	 * switch the unit back on behind the state's back.  The mode set
	 * that switches it on shows the whole framebuffer. */
	if (vmw_kms_is_atomic(v) && !du->active)
		return 0;
	vmw_du_dirty(v, du, fb, rects, n);
	return 0;
}

int vmw_du_flip(struct vmw_device *v, struct vmw_display_unit *du,
		struct drm_framebuffer *fb, uint32_t w, uint32_t h)
{
	int unit = du->unit;
	int rc;

	if (!vmw_stdu_unit_defined(v, unit, NULL, NULL) && fb_is_surface(fb) &&
	    vmw_stdu_available(v))
		vmw_stdu_unit_ensure(v, unit, w, h);
	if (vmw_stdu_unit_defined(v, unit, NULL, NULL)) {
		rc = vmw_stdu_unit_present(v, unit, fb, du->crtc_x, du->crtc_y,
					   du->crtc_x + (int)w,
					   du->crtc_y + (int)h, 1);
	} else {
		rc = vmw_sou_page_flip(v, du, fb);
		if (rc > 0) {
			if (fb_is_surface(fb))
				rc = -ENODEV;
			else if (unit == 0)
				rc = vmw_ldu_copy(v, du, fb, 0, 0,
						  (int)fb->width,
						  (int)fb->height);
			else
				rc = -EINVAL;
		}
	}
	if (rc == 0)
		du->fb_id = fb->id;
	return rc;
}

int vmw_page_flip(struct drm_device *dev, struct drm_crtc *crtc,
		  struct drm_framebuffer *fb)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du = vmw_crtc_du(v, crtc);

	if (!du || !fb)
		return -EINVAL;
	return vmw_du_flip(v, du, fb, crtc->mode.hdisplay,
			   crtc->mode.vdisplay);
}

/* Blanking is the whole device's (SVGA_REG_ENABLE): it follows the first
 * connector, whose screen is the one the legacy registers describe. */
int vmw_dpms(struct drm_device *dev, struct drm_connector *c, int mode)
{
	if (c != &dev->conn[0])
		return 0;
	vmsvga2_display_enable(mode == 0);
	return 0;
}

void vmw_master_set(struct drm_device *dev, struct drm_file *fp)
{
	(void)dev;
	(void)fp;
	/* The console stops painting here rather than at the first mode set.
	 * It is a client of this device itself now (drm_console.c), and its
	 * own mode set is a mode set like any other -- taking the display
	 * away there would silence the console on behalf of the console. */
	vmsvga2_hw_display_take();
}

void vmw_master_drop(struct drm_device *dev, struct drm_file *fp)
{
	struct vmw_device *v = dev->priv;
	(void)fp;
	for (int i = 0; i < vmw_du_count(v); i++) {
		struct vmw_display_unit *du = vmw_du(v, i);

		vmw_sou_forget(v, du);
		/* With atomic mode setting the console's next request may
		 * keep the mode and only change the framebuffer -- a flip,
		 * which would go to a screen object this has just forgotten.
		 * A unit marked off is set up again by that request instead,
		 * as the legacy mode set always did. */
		if (vmw_kms_is_atomic(v))
			du->active = 0;
	}
	vmsvga2_cursor_show(0);
	vmsvga2_hw_display_release();
	vmsvga2_hw_geometry(&v->hw);
}

/* ---- host-driven layout ------------------------------------------------
 *
 * The hypervisor resizes its window and tells the guest what geometry it
 * would like; a client that owns the display (an X server, or the tool that
 * follows the window size) passes those rectangles here, one per screen.
 * They are recorded in the display units -- whether each is part of the
 * host's desktop, at what size and where -- and the connectors are told
 * (vmw_connector_layout_changed()): rectangle i becomes the PREFERRED mode
 * of unit i's connector, so that the next time anything asks the connector
 * what it wants -- which is what a mode-setting client does on a hot-plug
 * event -- it is told the size the host is actually showing; a unit that
 * joins or leaves the desktop is reported as a hot-plug.
 *
 * Rectangles beyond the units the driver has are read past and ignored
 * rather than refused, and never change the units that exist. */

/* The display memory a topology needs.  `rects' are the screens, placed in
 * the host's desktop.  The bound for the screens together is
 * max_primary_mem (0: the device states none); for screen targets each
 * screen is also bounded by the largest screen target, and the bounding box
 * of the desktop is free of the bound only where the device says so
 * (SVGA_CAP_NO_BB_RESTRICTION, screen targets only). */
int vmw_kms_check_display_memory(struct vmw_device *v, uint32_t n,
				 const struct drm_vmw_rect *rects)
{
	int stdu = vmw_active_display_unit(v) == vmw_du_screen_target;
	uint64_t total_pixels = 0, pixel_mem, bb_mem;
	uint64_t bb_x2 = 0, bb_y2 = 0;

	for (uint32_t i = 0; i < n; i++) {
		/* For screen targets only each screen on its own is limited
		 * by SCREENTARGET_MAX_WIDTH/HEIGHT. */
		if (stdu && (rects[i].w > v->scanout_max_width ||
			     rects[i].h > v->scanout_max_height)) {
			VMW_DEBUG_KMS("Screen size not supported.\n");
			return -EINVAL;
		}
		/* Bounding box upper left is at (0,0).  (Places are never
		 * negative: the layout ioctl refuses them, the atomic check
		 * passes the units' own.) */
		if ((uint64_t)rects[i].x + rects[i].w > bb_x2)
			bb_x2 = (uint64_t)rects[i].x + rects[i].w;
		if ((uint64_t)rects[i].y + rects[i].h > bb_y2)
			bb_y2 = (uint64_t)rects[i].y + rects[i].h;
		total_pixels += (uint64_t)rects[i].w * rects[i].h;
	}
	if (!v->max_primary_mem)
		return 0; /* the device states no bound */
	/* Virtual svga device primary limits are always in 32-bpp. */
	pixel_mem = total_pixels * 4;
	if (pixel_mem > v->max_primary_mem) {
		VMW_DEBUG_KMS("Combined output size too large.\n");
		return -EINVAL;
	}
	/* SVGA_CAP_NO_BB_RESTRICTION is available for screen targets only. */
	if (!stdu || !(v->hw.caps & SVGA_CAP_NO_BB_RESTRICTION)) {
		bb_mem = bb_x2 * bb_y2 * 4;
		if (bb_mem > v->max_primary_mem) {
			VMW_DEBUG_KMS("Topology is beyond supported limits.\n");
			return -EINVAL;
		}
	}
	return 0;
}

long vmw_ioctl_update_layout(struct vmw_device *v,
			     struct drm_vmw_update_layout_arg *a)
{
	struct drm_vmw_rect rects[VMW_MAX_HEADS];
	uint32_t nunits = (uint32_t)vmw_du_count(v);
	uint32_t n;
	uint32_t iw, ih;
	int rc = 0;

	if (a->num_outputs == 0 || !a->rects)
		return -EINVAL;
	n = a->num_outputs < nunits ? a->num_outputs : nunits;
	if (drm_copy_from_user(rects, (const void *)(uintptr_t)a->rects,
			       n * sizeof(rects[0])) != 0)
		return -EFAULT;
	for (uint32_t i = 0; i < n; i++) {
		const struct drm_vmw_rect *r = &rects[i];

		if (!r->w || !r->h || r->w > v->drm.max_width ||
		    r->h > v->drm.max_height)
			return -EINVAL;
#if VMW_MAX_HEADS > 1
		/* Verify user-space for overflow as kernel use drm_rect */
		if ((int64_t)r->x + r->w > INT_MAX ||
		    (int64_t)r->y + r->h > INT_MAX)
			return -ERANGE;
		/* The desktop is limited to what the device can scan out in
		 * one piece: a window manager may create one framebuffer for
		 * the whole of it. */
		if (r->x < 0 || r->y < 0 ||
		    (uint64_t)r->x + r->w > v->drm.max_width ||
		    (uint64_t)r->y + r->h > v->drm.max_height)
			return -EINVAL;
#endif
	}
#if VMW_MAX_HEADS > 1
	rc = vmw_kms_check_display_memory(v, n, rects);
	if (rc)
		return rc;
#endif

	vmw_kms_initial_size(v, &iw, &ih);
	/* What the layout says of every unit, written under the display
	 * lock: an atomic check reads the places and the desktop membership
	 * (pref_active) of the units it is asked to switch on. */
	vmw_kms_lock(v);
	for (uint32_t i = 0; i < nunits; i++) {
		struct vmw_display_unit *du = vmw_du(v, (int)i);
		int active = i < n;

		if (active) {
			du->pref_w = rects[i].w;
			du->pref_h = rects[i].h;
#if VMW_MAX_HEADS > 1
			du->gui_x = rects[i].x;
			du->gui_y = rects[i].y;
#endif
		} else {
			du->pref_w = iw;
			du->pref_h = ih;
			du->gui_x = 0;
			du->gui_y = 0;
		}
		du->pref_active = active;
	}
	vmw_kms_unlock(v);
	/* The connectors follow: preferred modes, status, suggested offsets,
	 * hot-plug.  Outside the lock -- the probe this may run goes through
	 * the core, which can call back into the driver's hooks. */
	rc = vmw_connector_layout_changed(v);
	return rc;
}

/* ---- client-driven presents --------------------------------------------- */

enum vmw_display_unit_type vmw_active_display_unit(struct vmw_device *v)
{
	if (vmw_stdu_available(v))
		return vmw_du_screen_target;
	if (v->has_screen_object)
		return vmw_du_screen_object;
	return vmw_du_legacy;
}

int vmw_kms_present(struct vmw_device *v, struct drm_file *fp,
		    struct drm_framebuffer *fb, struct vmw_surface *srf,
		    uint32_t sid, int32_t dest_x, int32_t dest_y,
		    struct drm_vmw_rect *clips, uint32_t num_clips)
{
	int ret;

	(void)fp;
	(void)sid;
	switch (vmw_active_display_unit(v)) {
	case vmw_du_screen_target:
		/* Every screen target showing `fb'. */
		return vmw_kms_stdu_surface_dirty(v, fb, clips, srf, dest_x,
						  dest_y, num_clips, 1, NULL,
						  VMW_STDU_ALL_UNITS);
	case vmw_du_screen_object:
		/* Every screen object showing `fb'. */
		ret = vmw_kms_sou_do_surface_dirty(v, fb, NULL, clips, srf,
						   dest_x, dest_y, num_clips, 1,
						   NULL, VMW_DU_ALL_UNITS);
		break;
	default:
		return -ENOSYS;
	}
	if (ret)
		return ret;
	vmw_cmd_flush(v);
	return 0;
}

int vmw_kms_readback(struct vmw_device *v, struct drm_file *fp,
		     struct drm_framebuffer *fb, uint64_t user_fence_rep,
		     struct drm_vmw_rect *clips, uint32_t num_clips)
{
	switch (vmw_active_display_unit(v)) {
	case vmw_du_screen_target:
		/* Every screen target showing `fb'. */
		return vmw_kms_stdu_readback(v, fp, fb, user_fence_rep, clips,
					     num_clips, 1, VMW_STDU_ALL_UNITS);
	case vmw_du_screen_object:
		/* Every screen object showing `fb'. */
		return vmw_kms_sou_readback(v, fp, fb, user_fence_rep, clips,
					    num_clips, VMW_DU_ALL_UNITS);
	default:
		return -ENOSYS;
	}
}

/* ---- clip rectangles over units ----------------------------------------- */

int vmw_kms_helper_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			 const struct drm_mode_rect_k *clips,
			 const struct drm_vmw_rect *vclips, s32 dest_x,
			 s32 dest_y, uint32_t num_clips, int increment,
			 int unit, struct vmw_kms_dirty *dirty)
{
	struct vmw_display_unit *units[VMW_MAX_HEADS];
	u32 num_units = 0;
	u32 i, k;

	dirty->v = v;
	if (dirty->max_hits == 0 || (num_clips && !clips && !vclips) ||
	    increment < 1)
		return -EINVAL;

	/* A unit named by the caller is the only one; otherwise every unit
	 * that is showing this framebuffer. */
	if (unit != VMW_DU_ALL_UNITS) {
		struct vmw_display_unit *du = vmw_du(v, unit);

		if (du && du->w && du->h &&
		    (!dirty->unit_ok || dirty->unit_ok(du)))
			units[num_units++] = du;
	} else if (fb) {
		for (int j = 0; j < vmw_du_count(v); j++) {
			struct vmw_display_unit *du = vmw_du(v, j);

			if (du->active && du->fb_id == fb->id && du->w &&
			    du->h && (!dirty->unit_ok || dirty->unit_ok(du)))
				units[num_units++] = du;
		}
	}

	for (k = 0; k < num_units; k++) {
		struct vmw_display_unit *du = units[k];
		s32 crtc_x = du->crtc_x;
		s32 crtc_y = du->crtc_y;
		s32 crtc_width = (s32)du->w;
		s32 crtc_height = (s32)du->h;

		dirty->unit = du;
		dirty->num_hits = 0;
		for (i = 0; i < num_clips; i++) {
			s32 clip_left;
			s32 clip_top;

			/* Select clip array type. */
			if (clips) {
				const struct drm_mode_rect_k *c =
					&clips[(size_t)i * increment];

				dirty->fb_x = c->x1;
				dirty->fb_y = c->y1;
				dirty->unit_x2 = c->x2 + dest_x - crtc_x;
				dirty->unit_y2 = c->y2 + dest_y - crtc_y;
			} else {
				const struct drm_vmw_rect *c =
					&vclips[(size_t)i * increment];

				dirty->fb_x = c->x;
				dirty->fb_y = c->y;
				dirty->unit_x2 = dirty->fb_x + (s32)c->w +
						 dest_x - crtc_x;
				dirty->unit_y2 = dirty->fb_y + (s32)c->h +
						 dest_y - crtc_y;
			}

			dirty->unit_x1 = dirty->fb_x + dest_x - crtc_x;
			dirty->unit_y1 = dirty->fb_y + dest_y - crtc_y;

			/* Skip this clip if it's outside the unit */
			if (dirty->unit_x1 >= crtc_width ||
			    dirty->unit_y1 >= crtc_height ||
			    dirty->unit_x2 <= 0 || dirty->unit_y2 <= 0)
				continue;

			/* Clip right and bottom to the unit */
			dirty->unit_x2 = min_t(s32, dirty->unit_x2,
					       crtc_width);
			dirty->unit_y2 = min_t(s32, dirty->unit_y2,
					       crtc_height);

			/* Clip left and top to the unit */
			clip_left = min_t(s32, dirty->unit_x1, 0);
			clip_top = min_t(s32, dirty->unit_y1, 0);
			dirty->unit_x1 -= clip_left;
			dirty->unit_y1 -= clip_top;
			dirty->fb_x -= clip_left;
			dirty->fb_y -= clip_top;

			dirty->clip(dirty);
			/* A full command goes now; the unit carries on in
			 * the next. */
			if (dirty->num_hits >= dirty->max_hits) {
				dirty->fifo_commit(dirty);
				dirty->num_hits = 0;
			}
		}

		dirty->fifo_commit(dirty);
	}

	return 0;
}
