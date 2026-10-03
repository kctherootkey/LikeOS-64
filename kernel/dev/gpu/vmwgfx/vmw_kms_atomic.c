// LikeOS -- vmwgfx: atomic mode setting.
//
// The DRM core hands the driver a whole new display state -- every crtc's
// mode and whether it runs, every plane's framebuffer, place and damage --
// which the driver first checks (vmw_atomic_check()) and then applies in one
// go (vmw_atomic_commit()).  The legacy calls (SETCRTC, PAGE_FLIP, SETPLANE,
// CURSOR, DPMS, the console's mode sets) arrive the same way, translated by
// the core.
//
// Each crtc is one display unit (vmw_kms.h), and the commit is done with the
// per-unit code the legacy hooks use (vmw_kms.c): vmw_du_disable(),
// vmw_du_mode_set(), vmw_du_flip(), vmw_du_dirty(), and the cursor plane in
// vmw_cursor.c.  The order is the one the device needs:
//   1. video overlays paused if any unit goes off or changes its mode (the
//      host composes them onto the screens being redefined);
//   2. units that go off switched off -- a crtc without a mode, or one that
//      keeps its mode with its pipe stopped (DPMS off), whose planes stay in
//      the state but show nothing;
//   3. modes set: a requested mode set, a unit switched on again, a unit
//      whose screen the device no longer holds (the display handed back),
//      a unit whose framebuffer origin moved;
//   4. a new framebuffer on a running unit shown whole (a page flip), the
//      damage of an unchanged one shown rectangle by rectangle;
//   5. cursor planes: image when it changed, position from the request;
//   6. overlays resumed.
// From step 4 on nothing fails the commit: an update that could not be
// shown is reported and the next one tried, because a client that has one
// update refused may stop sending them (see vmw_dirty_not_shown()).  A mode
// set that fails is returned when it is the only one the request asked for
// -- what the legacy SETCRTC and the console's mode set have always
// returned, and what the console falls back on -- and reported otherwise.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2014-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2011-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2024-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_damage_helper.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/dev/video/vmsvga2.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* Damage rectangles handed to vmw_du_dirty() per call: each call is one
 * announcement to the device, and these live on the stack. */
#define VMW_ATOMIC_DAMAGE_BATCH 16

/* The device is blanked by a DPMS off of the first unit
 * (VMW_ATOMIC_DPMS_BLANK).  Written under the display lock. */
static int vmw_atomic_blanked;

/* A state refused, said the first few times: the client only sees the
 * errno, and a display server that gives up on it names nothing. */
static int vmw_atomic_refuse(const char *why)
{
	static unsigned said;

	if (said < 8) {
		said++;
		kprintf("[drm] vmwgfx: atomic check refused: %s\n", why);
	}
	return -EINVAL;
}

/* Something the commit could not do, said the first few times and never
 * returned (see the top of this file). */
static void vmw_atomic_not_shown(const char *what, int unit, int rc)
{
	static unsigned said;

	if (said < 12) {
		said++;
		kprintf("[drm] vmwgfx: %s on unit %d not shown (%d)\n", what,
			unit, rc);
	}
}

static int fb_is_surface(const struct drm_framebuffer *fb)
{
	return fb->obj && fb->obj->kind == DRM_GEM_SURFACE;
}

/* The state of crtc `crtc''s primary plane in `st', NULL without one. */
static struct drm_plane_state *vmw_primary_state(struct drm_atomic_state *st,
						 int crtc)
{
	struct drm_plane *p = drm_crtc_primary(st->dev, crtc);

	return p ? &st->planes[p->index] : NULL;
}

static uint32_t vmw_ncrtc(const struct drm_device *dev)
{
	return dev->ncrtc < DRM_MAX_CRTCS ? dev->ncrtc : DRM_MAX_CRTCS;
}

/* ---- the check ------------------------------------------------------------ */

/* Can the unit's path carry framebuffer `fb'?  The same choice the mode set
 * makes (vmw_du_show()): screen targets take any framebuffer; screen
 * objects take a buffer object bound to a region, and a surface only on a
 * device without guest-backed objects (there BLIT_SURFACE_TO_SCREEN takes
 * the command and shows nothing); the legacy display copies a buffer object
 * into the aperture, on the first unit only. */
static int vmw_check_primary_fb(struct vmw_device *v, int unit,
				const struct drm_framebuffer *fb)
{
	int stdu = vmw_stdu_available(v);

	if (fb_is_surface(fb)) {
		if (stdu)
			return 0;
		if (v->has_gb)
			return vmw_atomic_refuse("a guest-backed surface needs a screen target");
		if (!vmw_sou_can_carry(v, (struct drm_framebuffer *)fb))
			return vmw_atomic_refuse("a surface needs screen objects with 3D");
		return 0;
	}
	if (!fb->obj)
		return vmw_atomic_refuse("a framebuffer without an object");
	/* 32 bits per pixel everywhere.  16 only where something converts
	 * it: a screen target shows it through a display surface of its own
	 * format, a screen-object blit names the buffer's format (GMRFB).
	 * The legacy display scans the aperture out at the 32 bits per pixel
	 * the mode registers are programmed with. */
	if (fb->bpp != 32 &&
	    !(fb->bpp == 16 &&
	      (stdu || vmw_sou_can_carry(v, (struct drm_framebuffer *)fb))))
		return vmw_atomic_refuse("the scan-out takes 32 bits per pixel");
	if (unit > 0 && !stdu && !vmw_sou_can_carry(v, (struct drm_framebuffer *)fb))
		return vmw_atomic_refuse("a further unit's buffer needs a screen object");
	return 0;
}

/* One crtc of the request. */
static int vmw_check_crtc(struct vmw_device *v, struct drm_atomic_state *st,
			  struct drm_crtc_state *cs)
{
	int i = cs->crtc->index;
	struct vmw_display_unit *du = vmw_du(v, i);
	struct drm_plane_state *ps = vmw_primary_state(st, i);
	int modeset = drm_crtc_state_needs_modeset(cs);
	const struct drm_mode_modeinfo *m = &cs->mode;

	if (ps && ps->fb) {
		/* Uploads are done per framebuffer: a new one is shown whole,
		 * whatever damage the client gave. */
		if (ps->fb_changed)
			ps->ignore_damage_clips = 1;
	}
	if (!cs->mode_valid)
		return 0; /* off: nothing to show */
	if (!du)
		return vmw_atomic_refuse("a crtc without a display unit");
	if (modeset) {
		/* Each unit drives its own connector and no other. */
		if (cs->connector_mask != (1u << i) && cs->connector_mask != 0)
			return vmw_atomic_refuse("invalid connectors configuration");
		/* The legacy display is one screen: further units exist only
		 * as screen targets or screen objects. */
		if (i > 0 && !vmw_stdu_available(v) && !v->has_screen_object)
			return vmw_atomic_refuse("further display units need screen targets or screen objects");
		/* A unit the host's layout leaves out of its desktop. */
		if (!du->pref_active)
			return vmw_atomic_refuse("enabling a disabled display unit");
		if (m->hdisplay > v->scanout_max_width ||
		    m->vdisplay > v->scanout_max_height ||
		    (uint64_t)m->hdisplay * m->vdisplay * 4 > v->scanout_max_mem)
			return vmw_atomic_refuse("the mode is beyond the scan-out's limits");
	}
	if (ps && ps->fb && (modeset || ps->changed))
		return vmw_check_primary_fb(v, i, ps->fb);
	return 0;
}

/* The legacy display places its units itself (implicit placement): every
 * enabled unit has to show the same framebuffer. */
static int vmw_check_implicit(struct vmw_device *v, struct drm_atomic_state *st)
{
	const struct drm_framebuffer *implicit_fb = NULL;

	if (vmw_active_display_unit(v) != vmw_du_legacy)
		return 0;
	for (uint32_t i = 0; i < vmw_ncrtc(&v->drm); i++) {
		struct drm_plane_state *ps = vmw_primary_state(st, (int)i);

		if (!st->crtcs[i].mode_valid || !ps || ps->crtc != (int)i)
			continue;
		if (!implicit_fb)
			implicit_fb = ps->fb;
		else if (implicit_fb != ps->fb)
			return vmw_atomic_refuse("implicitly placed units show different framebuffers");
	}
	return 0;
}

/* The desktop the request leaves: every enabled unit at its place in the
 * host's layout, at its mode's size, within the display memory. */
static int vmw_check_topology(struct vmw_device *v, struct drm_atomic_state *st)
{
	struct drm_vmw_rect rects[DRM_MAX_CRTCS];
	uint32_t n = 0;

	for (uint32_t i = 0; i < vmw_ncrtc(&v->drm); i++) {
		const struct drm_crtc_state *cs = &st->crtcs[i];
		struct vmw_display_unit *du = vmw_du(v, (int)i);

		if (!cs->mode_valid || !du)
			continue;
		rects[n].x = du->gui_x > 0 ? du->gui_x : 0;
		rects[n].y = du->gui_y > 0 ? du->gui_y : 0;
		rects[n].w = cs->mode.hdisplay;
		rects[n].h = cs->mode.vdisplay;
		n++;
	}
	/* One screen was checked against the scan-out's own limits above
	 * (vmw_check_crtc()), which are what the connectors offer modes by
	 * (vmw_scanout_limits()); the display memory bounds the layout of
	 * several, as the host's layout ioctl does. */
	if (n < 2)
		return 0;
	if (vmw_kms_check_display_memory(v, n, rects))
		return vmw_atomic_refuse("the layout is beyond the display memory");
	return 0;
}

int vmw_atomic_check(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct vmw_device *v = dev->priv;
	int need_modeset = 0;
	int rc = 0;

	vmw_kms_lock(v);
	for (uint32_t i = 0; i < vmw_ncrtc(dev) && !rc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];

		if (drm_crtc_state_needs_modeset(cs))
			need_modeset = 1;
		rc = vmw_check_crtc(v, st, cs);
	}
	/* Cursor planes whose image or place is part of the request. */
	for (uint32_t i = 0; i < dev->nplanes && i < DRM_MAX_PLANES && !rc; i++) {
		struct drm_plane_state *ps = &st->planes[i];

		if (!ps->plane || ps->plane->type != DRM_PLANE_TYPE_CURSOR ||
		    !ps->changed || !ps->fb)
			continue;
		rc = vmw_cursor_plane_check(v, ps);
	}
	if (!rc)
		rc = vmw_check_implicit(v, st);
	if (!rc && need_modeset)
		rc = vmw_check_topology(v, st);
	vmw_kms_unlock(v);
	return rc;
}

/* ---- the commit ------------------------------------------------------------ */

/* The damage of an unchanged framebuffer on a running unit, shown the way
 * a DIRTYFB of the same rectangles is: in framebuffer coordinates, clipped
 * to the plane's source, the whole source when the request gave none. */
static void vmw_commit_damage(struct vmw_device *v,
			      struct vmw_display_unit *du,
			      struct drm_plane_state *ps)
{
	struct drm_plane_state old;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_mode_rect_k rects[VMW_ATOMIC_DAMAGE_BATCH];
	struct drm_rect r;
	uint32_t n = 0;

	drm_atomic_helper_old_plane_state(&v->drm, ps->plane, &old);
	drm_atomic_helper_damage_iter_init(&iter, &old, ps);
	drm_atomic_for_each_plane_damage(&iter, &r) {
		rects[n].x1 = r.x1;
		rects[n].y1 = r.y1;
		rects[n].x2 = r.x2;
		rects[n].y2 = r.y2;
		if (++n == VMW_ATOMIC_DAMAGE_BATCH) {
			vmw_du_dirty(v, du, ps->fb, rects, n);
			n = 0;
		}
	}
	if (n)
		vmw_du_dirty(v, du, ps->fb, rects, n);
}

/* The unit's framebuffer origin no longer is the one it was set up with
 * (SETCRTC at another x/y, a plane source moved): the screen is set up
 * again from there, as the legacy mode set did. */
static int vmw_commit_origin_moved(const struct vmw_display_unit *du,
				   const struct drm_plane_state *ps)
{
	return (int32_t)(ps->src_x >> 16) != du->crtc_x ||
	       (int32_t)(ps->src_y >> 16) != du->crtc_y;
}

/* The device-wide blank that goes with a DPMS off of the first unit. */
static void vmw_commit_blank(int blank)
{
	if (!VMW_ATOMIC_DPMS_BLANK || blank == vmw_atomic_blanked)
		return;
	vmsvga2_display_enable(!blank);
	vmw_atomic_blanked = blank;
}

/* Every cursor plane the request names, or whose crtc went off or was set
 * up again. */
static void vmw_commit_cursors(struct vmw_device *v,
			       struct drm_atomic_state *st, uint32_t touched)
{
	struct drm_device *dev = &v->drm;
	uint32_t ncrtc = vmw_ncrtc(dev);

	for (uint32_t i = 0; i < dev->nplanes && i < DRM_MAX_PLANES; i++) {
		struct drm_plane *p = &dev->planes[i];
		struct drm_plane_state *ps = &st->planes[i];
		struct vmw_cursor_plane_args a;
		int c;

		if (p->type != DRM_PLANE_TYPE_CURSOR)
			continue;
		c = ps->crtc >= 0 ? ps->crtc : p->crtc;
		if (c < 0 || (uint32_t)c >= ncrtc)
			continue;
		if (!ps->changed && !(touched & (1u << c)))
			continue;
		/* Moved to another crtc: off the one it was on first. */
		if (p->crtc >= 0 && (uint32_t)p->crtc < ncrtc && ps->crtc >= 0 &&
		    p->crtc != ps->crtc) {
			mm_memset(&a, 0, sizeof(a));
			(void)vmw_cursor_plane_update(v, &dev->crtc[p->crtc], &a);
		}

		struct vmw_display_unit *du = vmw_du(v, c);
		const struct drm_crtc_state *cs = &st->crtcs[c];

		mm_memset(&a, 0, sizeof(a));
		a.fb = ps->crtc >= 0 ? ps->fb : NULL;
		a.src_x = ps->src_x >> 16;
		a.src_y = ps->src_y >> 16;
		a.w = ps->crtc_w;
		a.h = ps->crtc_h;
		a.x = ps->crtc_x;
		a.y = ps->crtc_y;
		a.hot_x = ps->hot_x;
		a.hot_y = ps->hot_y;
		a.on = du && du->active && cs->active && cs->mode_valid;
		a.image_changed = ps->fb_changed || ps->crtc_changed ||
				  ps->src_x != p->src_x || ps->src_y != p->src_y ||
				  ps->crtc_w != p->crtc_w ||
				  ps->crtc_h != p->crtc_h ||
				  ps->hot_x != p->hot_x || ps->hot_y != p->hot_y;
		(void)vmw_cursor_plane_update(v, &dev->crtc[c], &a);
	}
}

int vmw_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct vmw_device *v = dev->priv;
	uint32_t ncrtc = vmw_ncrtc(dev);
	uint32_t off_mask = 0, set_mask = 0, asked_mask = 0;
	uint32_t nset = 0;
	int rc = 0;

	vmw_kms_lock(v);

	/* What each unit has to do. */
	for (uint32_t i = 0; i < ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct vmw_display_unit *du = vmw_du(v, (int)i);
		struct drm_plane_state *ps = vmw_primary_state(st, (int)i);
		int want = cs->mode_valid && cs->active && ps && ps->fb;

		if (!du)
			continue;
		if (!want) {
			/* Off, or enabled with its pipe stopped: whatever
			 * the device still holds for the unit goes. */
			if (du->active || du->sou_defined ||
			    vmw_stdu_unit_defined(v, (int)i, NULL, NULL))
				off_mask |= 1u << i;
			continue;
		}
		/* A mode set asked for; a framebuffer origin that moved; or a
		 * new primary state for a unit that shows nothing although
		 * its crtc runs -- the display was handed back
		 * (vmw_master_drop()) and the console's mode set keeps the
		 * mode, or an earlier mode set of it failed.  A cursor move
		 * that finds the unit so does not try again. */
		if (drm_crtc_state_needs_modeset(cs) ||
		    (du->active && vmw_commit_origin_moved(du, ps)) ||
		    (!du->active && ps->changed))
			set_mask |= 1u << i;
		if (drm_crtc_state_needs_modeset(cs) || ps->changed)
			asked_mask |= 1u << i;
	}
	for (uint32_t i = 0; i < ncrtc; i++)
		if (set_mask & (1u << i))
			nset++;

	if (off_mask || set_mask)
		vmw_overlay_pause_all(v);

	/* Units that go off. */
	for (uint32_t i = 0; i < ncrtc; i++) {
		if (!(off_mask & (1u << i)))
			continue;
		vmw_du_disable(v, vmw_du(v, (int)i));
		if (i == 0 && st->crtcs[i].mode_valid && !st->crtcs[i].active)
			vmw_commit_blank(1);
	}
	/* Several units set up at once is a new layout: all of them go off
	 * first, so that none is defined over the place or the backing
	 * store another one still holds.  A single unit is set up over what
	 * it shows, which on failure leaves it as it was -- the legacy mode
	 * set's behaviour. */
	if (nset > 1) {
		for (uint32_t i = 0; i < ncrtc; i++) {
			struct vmw_display_unit *du = vmw_du(v, (int)i);

			if ((set_mask & (1u << i)) && du->active)
				vmw_du_disable(v, du);
		}
	}
	/* Modes. */
	for (uint32_t i = 0; i < ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct vmw_display_unit *du = vmw_du(v, (int)i);
		struct drm_plane_state *ps = vmw_primary_state(st, (int)i);
		int r;

		if (!(set_mask & (1u << i)))
			continue;
		if (i == 0)
			vmw_commit_blank(0);
		r = vmw_du_mode_set(v, du, &cs->mode, ps->fb,
				    (int)(ps->src_x >> 16),
				    (int)(ps->src_y >> 16));
		if (!r)
			continue;
		if ((asked_mask & (1u << i)) && nset == 1 && !off_mask) {
			/* The only mode set asked for: the request fails as
			 * the legacy call would, and nothing else of it has
			 * been applied. */
			rc = r;
			goto out;
		}
		/* The state is recorded all the same: the unit shows
		 * nothing of it until a later request sets it up. */
		vmw_atomic_not_shown("mode set", (int)i, r);
		du->active = 0;
	}
	/* New framebuffers and damage on units that keep running. */
	for (uint32_t i = 0; i < ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct vmw_display_unit *du = vmw_du(v, (int)i);
		struct drm_plane_state *ps = vmw_primary_state(st, (int)i);
		int r;

		if ((set_mask & (1u << i)) || !du || !du->active ||
		    !cs->active || !cs->mode_valid || !ps || !ps->fb ||
		    !ps->changed)
			continue;
		if (ps->fb_changed || ps->fb->id != du->fb_id) {
			r = vmw_du_flip(v, du, ps->fb, cs->mode.hdisplay,
					cs->mode.vdisplay);
			if (r)
				vmw_atomic_not_shown("page flip", (int)i, r);
			continue;
		}
		vmw_commit_damage(v, du, ps);
	}
	vmw_commit_cursors(v, st, off_mask | set_mask);
out:
	if (off_mask || set_mask)
		vmw_overlay_resume_all(v);
	vmw_kms_unlock(v);
	return rc;
}
