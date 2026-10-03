// LikeOS -- vmwgfx: suspend and resume.
//
// Down, in this order: the screens (overlay streams paused, the cursor
// hidden, every screen target and screen object taken off the device --
// the DRM core keeps what was shown and replays it after resume); the
// command stream emptied and every fence waited for (or given up on), the
// fence timer stopped; for a reset also the object tables and the
// command-buffer channel; then the hardware layer (vmsvga2_hw_suspend()).
//
// Up: the hardware layer first (vmsvga2_hw_resume(), seeded with the last
// fence handed out).  If the device came back reset -- always after a reset
// suspend, and after a light one whose device lost its state anyway -- the
// driver's half is rebuilt: the guest driver id, the command-buffer channel,
// fences (everything issued before reads as passed), fresh object tables,
// and every live buffer object's MOB and region defined again from what the
// driver kept, which is all of it: id, pages and page tables never moved.
// Then the overlay streams run again, and the DRM core replays the display.
//
// Nothing may submit or wait on a fence in between: the DRM core's power
// gate holds every ioctl, and the object reaper waits behind it too.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's and VMware's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright 2012-2023 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright 2015-2023 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/vmw_pm.h>

/* One device; the DRM core runs one transition at a time on it. */
static struct {
	int suspended;	/* vmw_pm_suspend() succeeded, no resume since */
	int reset;	/* ...and the device was reset by it */
	/* The device lost its objects: they are rebuilt before a resume may
	 * return success.  Kept across a failed resume, so a retry -- for
	 * which the hardware layer reports "not suspended" -- rebuilds. */
	int rebuild;
	uint32_t fence_seed; /* the last fence sequence issued before suspend */
} g_vmw_pm;

/* ---- what a reset would lose ------------------------------------------- */

/* Ids in use in an id bitmap of `nbits', not counting id 0, which is held
 * back for good (vmw_gb_init()). */
static uint32_t vmw_pm_ids_in_use(struct vmw_device *v, const uint8_t *bm,
				  uint32_t nbits)
{
	uint32_t n = 0;
	uint64_t fl;

	if (!bm)
		return 0;
	spin_lock_irqsave(&v->id_lock, &fl);
	for (uint32_t i = 0; i < nbits / 8; i++)
		n += gpu_hweight32(bm[i]);
	if (bm[0] & 1)
		n--;
	spin_unlock_irqrestore(&v->id_lock, fl);
	return n;
}

/* Surfaces clients hold.  Counted by object rather than by id: the ids also
 * cover the driver's own screen-target display surfaces, which the screen
 * teardown destroys anyway, and surfaces whose last reference is gone and
 * which only wait for the device to finish with them. */
static uint32_t vmw_pm_client_surfaces(struct vmw_device *v)
{
	struct drm_device *dev = &v->drm;
	uint32_t n = 0;
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	for (struct drm_gem_object *o = dev->objects; o; o = o->next)
		if (o->kind == DRM_GEM_SURFACE && o->priv &&
		    __atomic_load_n(&o->refs, __ATOMIC_RELAXED) > 0)
			n++;
	spin_unlock_irqrestore(&dev->lock, fl);
	return n;
}

/* A reset takes every surface, context and shader with it, and there is no
 * way to save them first (nothing here can evict a resource to guest
 * memory): refuse while clients hold any.  0 or -EBUSY. */
static int vmw_pm_reset_allowed(struct vmw_device *v)
{
	uint32_t srf = vmw_pm_client_surfaces(v);
	uint32_t ctx = vmw_pm_ids_in_use(v, v->context_ids, VMW_NUM_CONTEXTS);
	uint32_t shd = vmw_pm_ids_in_use(v, v->shader_ids, VMW_NUM_SHADERS);

	if (srf == 0 && ctx == 0 && shd == 0)
		return 0;
	kprintf("[drm] vmwgfx: can't suspend with a device reset while 3D resources are active (%u surfaces, %u contexts, %u shaders)\n",
		srf, ctx, shd);
	return -EBUSY;
}

/* ---- going down ---------------------------------------------------------- */

/* Take the screens off the device.  What is shown is the DRM core's to
 * remember and to set again; the units' own records of a screen target or
 * screen object are cleared with it, so that the replay defines them
 * afresh. */
static void vmw_pm_kms_suspend(struct vmw_device *v)
{
	vmw_overlay_pause_all(v);
#if VMW_PM_CURSOR_HOOKS
	vmw_cursor_suspend(v);
#else
	(void)vmsvga2_hw_cursor_hide();
#endif
	for (int i = 0; i < vmw_stdu_num_units(v); i++)
		if (vmw_stdu_unit_defined(v, i, NULL, NULL))
			vmw_stdu_unit_teardown(v, i);
	for (int i = 0; i < vmw_du_count(v); i++) {
		struct vmw_display_unit *du = vmw_du(v, i);

		if (du)
			vmw_sou_disable(v, du);
	}
}

/* Empty the command stream and settle every fence.  Once every fence has
 * passed, the objects that were only waiting for the device are finished
 * here, while the device can still take their destroy commands, and those
 * are waited for as well. */
static void vmw_pm_quiesce(struct vmw_device *v)
{
	vmw_cmd_drain(v);
	vmw_fence_suspend(v);
	drm_gem_reap(&v->drm);
	vmw_cmd_drain(v);
}

/* ---- coming back ---------------------------------------------------------- */

/* Objects whose last reference has gone and that only wait for the device
 * (the DRM core's dead list): the device lost their MOBs and surfaces with
 * the reset, so their destroy commands must not be sent -- an id the device
 * does not have is a command error.  Only flags are written, under the
 * lock that keeps them on the list; nothing is sent. */
static uint32_t vmw_pm_forget_dead(struct vmw_device *v)
{
	struct drm_device *dev = &v->drm;
	uint32_t n = 0;
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	for (struct drm_gem_object *o = dev->dead; o; o = o->dead_next) {
		if (!o->priv)
			continue;
		if (o->kind == DRM_GEM_SURFACE) {
			struct vmw_surface *s = o->priv;

			s->defined = 0;
			s->bound = 0;
		} else {
			struct vmw_bo *b = o->priv;

			vmw_mob_forget(&b->mob);
		}
		n++;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	return n;
}

/* A reference to every live object of the device that has driver state
 * (buffer objects and surfaces), for working on them without the device's
 * lock.  NULL with *out_n = 0 when there are none; NULL with *out_n set to
 * UINT32_MAX when there was no memory for the array. */
static struct drm_gem_object **vmw_pm_grab_objects(struct vmw_device *v,
						   uint32_t *out_n)
{
	struct drm_device *dev = &v->drm;

	for (;;) {
		struct drm_gem_object **objs;
		uint32_t count = 0, n = 0, cap;
		int overflow = 0;
		uint64_t fl;

		spin_lock_irqsave(&dev->lock, &fl);
		for (struct drm_gem_object *o = dev->objects; o; o = o->next)
			if (o->priv)
				count++;
		spin_unlock_irqrestore(&dev->lock, fl);
		*out_n = 0;
		if (!count)
			return NULL;
		/* Room for a few more: nothing should be created meanwhile,
		 * but a miss only costs another round. */
		cap = count + 16;
		objs = kalloc((size_t)cap * sizeof(*objs));
		if (!objs) {
			*out_n = UINT32_MAX;
			return NULL;
		}
		spin_lock_irqsave(&dev->lock, &fl);
		for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
			if (!o->priv)
				continue;
			if (n == cap) {
				overflow = 1;
				break;
			}
			if (drm_gem_get_unless_zero(o))
				objs[n++] = o;
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		if (!overflow) {
			*out_n = n;
			return objs;
		}
		for (uint32_t i = 0; i < n; i++)
			drm_gem_put(objs[i]);
		kfree(objs);
	}
}

/* Give a reset device back everything the driver kept: fresh object tables,
 * then every live buffer object's MOB (same id, same page tables) and
 * second-generation region (same id, same pages), then the surfaces that
 * were defined on it -- defined and bound again; what the device held of
 * their contents beyond the backing is gone.  Legacy surfaces, contexts and
 * shaders cannot come back (a reset suspend refused while there were any):
 * they are counted and reported.  0, or the error of the object tables (the
 * caller stays suspended and may try again). */
static int vmw_pm_rebuild_objects(struct vmw_device *v)
{
	uint32_t errors_before = v->cb_errors;
	uint32_t nmob = 0, ngmr = 0, nsrf = 0, nfail = 0, nlost = 0;
	int gmr2 = (v->hw.caps & SVGA_CAP_GMR2) != 0;
	struct drm_gem_object **objs;
	uint32_t n;
	int rc;

	if (v->has_gb) {
		/* Tables the device may or may not still hold: told to stop
		 * using them (harmless on a reset device), and new zeroed
		 * ones given -- stale entries would describe objects the
		 * device no longer has. */
		if (v->otable_batch)
			vmw_otables_takedown(v);
		rc = vmw_otables_setup(v);
		if (rc) {
			kprintf("[drm] vmwgfx: object tables could not be set up again (%d)\n",
				rc);
			return rc;
		}
	}

	objs = vmw_pm_grab_objects(v, &n);
	if (n == UINT32_MAX)
		return -ENOMEM;

	/* Buffer objects first: the surfaces below bind to their MOBs. */
	for (uint32_t i = 0; i < n; i++) {
		struct drm_gem_object *o = objs[i];
		struct vmw_bo *b;

		if (o->kind != DRM_GEM_BO)
			continue;
		b = o->priv;
		if (b->mob.id != SVGA3D_INVALID_ID && b->mob.defined) {
			if (vmw_mob_redefine(v, &b->mob) == 0)
				nmob++;
			else
				nfail++;
		}
		if (gmr2 && b->gmr_id >= 0 && o->pages) {
			if (vmsvga2_gmr_bind(b->gmr_id, o->pages, o->npages) == 0)
				ngmr++;
			else
				nfail++;
		}
	}

	for (uint32_t i = 0; i < n; i++) {
		struct drm_gem_object *o = objs[i];
		struct vmw_surface *s;
		int was_bound;

		if (o->kind != DRM_GEM_SURFACE)
			continue;
		s = o->priv;
		if (!s->defined)
			continue;
		if (s->legacy || !v->has_gb) {
			s->defined = 0;
			nlost++;
			continue;
		}
		was_bound = s->bound;
		s->defined = 0;
		s->bound = 0;
		rc = vmw_surface_define(v, s);
		if (rc == 0 && was_bound)
			rc = vmw_surface_bind(v, s);
		if (rc == 0)
			nsrf++;
		else
			nfail++;
	}

	/* Everything above is queued: let the device take it, and hear about
	 * anything it refused before clients are let back in. */
	vmw_cmd_drain(v);
	for (uint32_t i = 0; i < n; i++)
		drm_gem_put(objs[i]);
	kfree(objs);

	{
		uint32_t ctx = vmw_pm_ids_in_use(v, v->context_ids,
						 VMW_NUM_CONTEXTS);
		uint32_t shd = vmw_pm_ids_in_use(v, v->shader_ids,
						 VMW_NUM_SHADERS);

		if (nlost || ctx || shd)
			kprintf("[drm] vmwgfx: the device lost %u legacy surface(s), %u context(s) and %u shader(s) with its state; their clients will see errors\n",
				nlost, ctx, shd);
	}
	if (nfail || v->cb_errors != errors_before)
		kprintf("[drm] vmwgfx: %u object(s) not restored, %u command buffer(s) refused while restoring\n",
			nfail, v->cb_errors - errors_before);
	kprintf("[drm] vmwgfx: device state restored: %u MOB(s), %u region(s), %u surface(s)\n",
		nmob, ngmr, nsrf);
	return 0;
}

/* Bring the driver's half back on a running device.  `rebuild': the device
 * lost its objects (or, for an unwound reset suspend, its object tables) and
 * gets them back from what the driver kept. */
static int vmw_pm_restore(struct vmw_device *v, int rebuild)
{
	int rc;

	if (rebuild) {
		vmw_write_driver_id(v, v->drm.drv);
		(void)vmw_pm_forget_dead(v);
		/* A context that will not start leaves the device on the
		 * FIFO; that is reported and carries on, as at init. */
		(void)vmw_cmdbuf_resume(v);
	}
	vmw_fence_resume(v, g_vmw_pm.fence_seed);
	if (rebuild) {
		rc = vmw_pm_rebuild_objects(v);
		if (rc)
			return rc;
	}
	vmw_overlay_resume_all(v);
#if VMW_PM_CURSOR_HOOKS
	vmw_cursor_resume(v, rebuild);
#endif
	return 0;
}

/* ---- the hooks ------------------------------------------------------------ */

int vmw_pm_suspend(struct drm_device *dev)
{
	struct vmw_device *v = dev->priv;
	const int reset = VMW_PM_RESET;
	int rc;

	if (!v)
		return -ENODEV;
	if (g_vmw_pm.suspended)
		return 0;
	if (reset) {
		rc = vmw_pm_reset_allowed(v);
		if (rc)
			return rc;
	}

	vmw_pm_kms_suspend(v);
	vmw_pm_quiesce(v);
	g_vmw_pm.fence_seed = v->drm.fence_seq;
	if (reset) {
		/* The tables go before the reset that would take them anyway,
		 * so that the device is told and their memory is released
		 * once it has stopped reading it; the MOB ids stay accounted
		 * to their objects.  Then the channel: the hardware layer
		 * resets only with no command channel registered. */
		vmw_otables_takedown(v);
		(void)vmw_cmdbuf_suspend(v);
	}

	rc = vmsvga2_hw_suspend(reset != 0);
	if (rc) {
		int r;

		kprintf("[drm] vmwgfx: the device would not suspend (%d); running on\n",
			rc);
		r = vmw_pm_restore(v, reset);
		if (r)
			kprintf("[drm] vmwgfx: device state not fully restored (%d)\n",
				r);
		/* Nothing will replay the screens for a suspend that did not
		 * happen: show them again here. */
		r = dev->drv->atomic_commit ? drm_atomic_replay(dev) :
					      drm_legacy_replay(dev);
		if (r)
			kprintf("[drm] vmwgfx: the display did not come back (%d)\n",
				r);
		return rc;
	}
	g_vmw_pm.suspended = 1;
	g_vmw_pm.reset = reset;
	g_vmw_pm.rebuild = reset;
	return 0;
}

int vmw_pm_resume(struct drm_device *dev)
{
	struct vmw_device *v = dev->priv;
	int rc;

	if (!v)
		return -ENODEV;
	if (!g_vmw_pm.suspended)
		return 0;

	rc = vmsvga2_hw_resume(g_vmw_pm.fence_seed);
	if (rc < 0) {
		kprintf("[drm] vmwgfx: the device did not come back (%d)\n", rc);
		return rc;
	}
	if (rc == 1 && !g_vmw_pm.rebuild) {
		kprintf("[drm] vmwgfx: the device lost its state while suspended; rebuilding\n");
		g_vmw_pm.rebuild = 1;
	}

	rc = vmw_pm_restore(v, g_vmw_pm.rebuild);
	if (rc)
		return rc;
	g_vmw_pm.rebuild = 0;
	g_vmw_pm.reset = 0;
	g_vmw_pm.suspended = 0;
	return 0;
}
