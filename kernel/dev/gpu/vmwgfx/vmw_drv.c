// LikeOS -- display-manager backend for the VMware SVGA II / SVGA3D
// device ("vmwgfx": the name userspace's driver stack looks for).
//
// Sits on the hardware primitives kernel/dev/video/vmsvga2.c exports.  Two
// display paths, chosen by what the host offers:
//   - screen objects: the scan-out buffer is a guest memory region (a
//     buffer object bound to a GMR) and the host reads it directly;
//     a dirty rectangle is one BLIT_GMRFB_TO_SCREEN command;
//   - legacy: the host scans out VRAM; dirty rectangles are copied from
//     the buffer object into VRAM and announced with SVGA_CMD_UPDATE.
// Fences come from the FIFO fence register; the fence interrupt signals
// them where the host has one, a short timer polls where it has not.
//
// This file: the probe and initialisation, the buffer-object hooks and the
// driver table.  The display paths are in vmw_kms.c and vmw_stdu.c, the
// cursor in vmw_cursor.c, fences in vmw_fence.c, the ioctls in vmw_ioctl.c,
// command submission in vmw_cmdbuf.c.
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
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/vmw_pm.h>
#include <kernel/dev/gpu/vmwgfx/vmw_connector.h>

static struct vmw_device g_vmw;

/* ---- buffer objects --------------------------------------------------- */

/* A buffer object's id and pages are accounted (vmw_gmrid.c) before the
 * device hears of it, so a buffer the device's pool cannot hold is refused
 * here rather than by the device, which would halt the command-buffer
 * context over it.  Both ends use the object's page count, which does not
 * change while it lives. */
int vmw_gem_init(struct drm_gem_object *o)
{
	struct vmw_bo *b = kalloc(sizeof(*b));
	uint32_t npages;
	int rc;

	if (!b)
		return -ENOMEM;
	mm_memset(b, 0, sizeof(*b));
	b->gmr_id = -1;
	b->mob.id = SVGA3D_INVALID_ID;
	o->priv = b;
	if (!o->pages)
		return 0;
	npages = o->npages;
	/* Guest-backed hosts: every buffer is a MOB (what surfaces bind to
	 * and what DX commands name), so a buffer without one is refused,
	 * not handed out.  It used to be returned anyway with no MOB behind
	 * it -- and the first command naming it then carried an invalid MOB
	 * id to the device.  A GMR as well where the host has them, for the
	 * 2D scan-out and legacy DMA paths. */
	if (g_vmw.has_gb && g_vmw.otables_ready) {
		uint32_t id = SVGA3D_INVALID_ID;

		rc = vmw_gmrid_man_get_node(&g_vmw, VMW_PL_MOB, npages, &id);
		if (rc == 0) {
			b->mob.id = id;
			/* On failure the bind leaves nothing behind but the
			 * id, which goes back with its pages. */
			rc = vmw_mob_bind(&g_vmw, &b->mob, o->pages, npages,
					  (uint32_t)o->size);
			if (rc) {
				vmw_gmrid_man_put_node(&g_vmw, VMW_PL_MOB, id,
						       npages);
				b->mob.id = SVGA3D_INVALID_ID;
			}
		}
		if (rc) {
			o->priv = NULL;
			kfree(b);
			return rc;
		}
		o->backend_id = id;
	}
	if (g_vmw.has_gmr && (b->mob.id == SVGA3D_INVALID_ID || o->scanout)) {
		uint32_t id = SVGA3D_INVALID_ID;

		/* A region is optional, as it always was: without one the
		 * buffer keeps whatever else it has. */
		if (vmw_gmrid_man_get_node(&g_vmw, VMW_PL_GMR, npages, &id) == 0) {
			if (vmsvga2_gmr_bind((int)id, o->pages, npages) == 0) {
				b->gmr_id = (int)id;
				if (b->mob.id == SVGA3D_INVALID_ID)
					o->backend_id = id;
			} else {
				vmw_gmrid_man_put_node(&g_vmw, VMW_PL_GMR, id,
						       npages);
			}
		}
	}
	return 0;
}

static void vmw_gem_free(struct drm_gem_object *o)
{
	if (o->kind == DRM_GEM_SURFACE) {
		vmw_surface_gem_free(&g_vmw, o);
		return;
	}
	struct vmw_bo *b = o->priv;

	if (!b)
		return;
	if (b->mob.id != SVGA3D_INVALID_ID) {
		vmw_mob_unbind(&g_vmw, &b->mob);
		vmw_gmrid_man_put_node(&g_vmw, VMW_PL_MOB, b->mob.id, o->npages);
		b->mob.id = SVGA3D_INVALID_ID;
	}
	/* Unbinds and frees the region as well. */
	if (b->gmr_id >= 0)
		vmw_gmrid_man_put_node(&g_vmw, VMW_PL_GMR, (uint32_t)b->gmr_id,
				       o->npages);
	kfree(b);
	o->priv = NULL;
}

/* The pages of every buffer object here are reachable by the host: they are
 * what a MOB page table names.  Hold them until it has finished with them. */
static void vmw_gem_release_pages(struct drm_gem_object *o)
{
	if (!o->pages)
		return;
	vmw_defer_free_pages(&g_vmw, o->pages, (uint32_t)o->npages);
}

static uint64_t vmw_gem_page_phys(struct drm_gem_object *o, uint64_t index)
{
	if (!o->pages || index >= o->npages)
		return 0;
	return o->pages[index];
}

/* The file's own state, there from the open on: DRM_VMW_GET_PARAM records
 * in it what kind of client this is before anything else is created, and
 * having it from the start means two threads of a fresh client cannot
 * both find it missing and each allocate one. */
static int vmw_driver_open(struct drm_device *dev, struct drm_file *fp)
{
	struct vmw_file *f;

	(void)dev;
	f = kalloc(sizeof(*f));
	if (!f)
		return -ENOMEM;
	mm_memset(f, 0, sizeof(*f));
	fp->priv = f;
	return 0;
}

static void vmw_postclose(struct drm_device *dev, struct drm_file *fp)
{
	/* Anything this file asked to be told about is dropped here: the
	 * event would be queued onto a file that no longer exists. */
	vmw_fence_file_release(dev->priv, fp);
	vmw_overlay_file_release(dev->priv, fp);
	vmw_file_release(dev->priv, fp);
}

/* ---- probe ---------------------------------------------------------- */

/* The hooks of the per-call display interface; vmw_driver_table() derives
 * the table that is registered from it. */
/* The core's reaper tick: pages held for the device go back once it has
 * caught up (vmw_cmdbuf_idle_reclaim()). */
static void vmw_drm_idle_reclaim(struct drm_device *dev)
{
	vmw_cmdbuf_idle_reclaim(dev->priv);
}

static const struct drm_driver vmw_driver = {
	.name = "vmwgfx",
	.desc = "VMware SVGA II display-manager driver",
	.date = "20260827",
	.major = 2,
	.minor = 20,
	.patch = 0,
	.cursor_w = 64,
	.cursor_h = 64,
	/* Surface ids cross processes by value (DRI2: X server to client, then
	 * GB_SURFACE_REF on the client's own file), so a handle must name one
	 * object device-wide.  See struct drm_driver. */
	.global_handles = 1,
	.open = vmw_driver_open,
	.postclose = vmw_postclose,
#if VMW_PM
	.suspend = vmw_pm_suspend,
	.resume = vmw_pm_resume,
#endif
	.master_set = vmw_master_set,
	.master_drop = vmw_master_drop,
	.gem_init = vmw_gem_init,
	.gem_free = vmw_gem_free,
	.fence_poll = vmw_drm_fence_poll,
	.idle_reclaim = vmw_drm_idle_reclaim,
	.gem_release_pages = vmw_gem_release_pages,
	.gem_page_phys = vmw_gem_page_phys,
	.gem_mmap_pte_extra = 0,
	.mode_set = vmw_mode_set,
	.crtc_disable = vmw_crtc_disable,
	.fb_dirty = vmw_fb_dirty,
	.page_flip = vmw_page_flip,
	.cursor_set = vmw_cursor_set,
	.cursor_move = vmw_cursor_move,
	.dpms = vmw_dpms,
	.display_verify = vmw_display_verify,
	.display_fallback = vmw_display_fallback,
	.hw_vblank = 0,
	.ioctl = vmw_ioctl,
	.render_allowed = vmw_render_allowed,
};

/* The table registered with the display-manager core.
 *
 * With VMW_ATOMIC (vmw_kms.h) the display is driven through atomic
 * requests: the core checks a request, vmw_atomic_check() checks it against
 * the display units, vmw_atomic_commit() carries it out, and the per-call
 * hooks are not used -- the core turns the per-call ioctls into requests.
 * The features are what an atomic SVGA driver offers: the generic plane
 * check, damage clips on the planes, in- and out-fences and the cursor
 * hotspot; no blending, colour management or variable refresh.  The
 * connectors probe through the core's probe helper either way
 * (vmw_connector_driver_setup()).  Without VMW_ATOMIC the per-call table is
 * registered as it is. */
static struct drm_driver vmw_registered_driver;

static const struct drm_driver *vmw_driver_table(void)
{
	struct drm_driver *d = &vmw_registered_driver;

	*d = vmw_driver;
#if VMW_ATOMIC
	d->atomic_check = vmw_atomic_check;
	d->atomic_commit = vmw_atomic_commit;
	d->cursor_obj_check = vmw_cursor_obj_check;
	d->features |= DRM_FEATURE_ATOMIC_PLANE_CHECK | DRM_FEATURE_DAMAGE_CLIPS |
		       DRM_FEATURE_ATOMIC_FENCES | DRM_FEATURE_CURSOR_HOTSPOT;
	d->gamma_size = 0;
	d->degamma_size = 0;
	d->color_has_ctm = 0;
	d->mode_set = NULL;
	d->crtc_disable = NULL;
	d->page_flip = NULL;
	d->cursor_set = NULL;
	d->cursor_move = NULL;
	d->dpms = NULL;
#endif
	vmw_connector_driver_setup(d);
	return d;
}

/* Does the host offer 3D?
 *
 * Two places carry the answer and which one is live depends on the device.
 *
 * A device with guest-backed objects answers through the device-capability
 * register: write the capability's index to SVGA_REG_DEV_CAP, read the
 * value back.  This is the only reliable source on such a device -- the
 * FIFO's own version fields are the older interface and the host is not
 * obliged to keep them filled in.
 *
 * An older device answers in the FIFO, in one of TWO slots.  The version
 * moved to SVGA_FIFO_3D_HWVERSION_REVISED (17) because the original slot
 * (7) sits inside the region a guest may write, so a guest that reset the
 * FIFO could destroy the value the host had put there; the host advertises
 * the move with SVGA_FIFO_CAP_3D_HWVERSION_REVISED.  Reading the original
 * slot on a device that has moved the field yields 0.
 *
 * Reading only slot 7 is what this did before, and this machine -- VMware
 * with 3D switched on, caps 0xfdffc3e2 (SVGA_CAP_3D and SVGA_CAP_GBOBJECTS
 * both set), fifo caps 0x77f (the REVISED bit set) -- reported "3d no",
 * with has_gb and has_dx switched off behind it and DRM_VMW_PARAM_3D
 * answering 0, so Mesa's svga driver would not bind and GL fell back to
 * llvmpipe.  DRM_VMW_PARAM_FIFO_HW_VERSION below already read the right
 * slot; this is the same knowledge applied where the decision is made. */
static int vmw_probe_3d(const struct vmw_device *v)
{
	if (!(v->hw.caps & SVGA_CAP_3D))
		return 0;
	if (v->hw.caps & SVGA_CAP_GBOBJECTS) {
		vmsvga2_hw_write_reg(SVGA_REG_DEV_CAP, SVGA3D_DEVCAP_3D);
		return vmsvga2_hw_read_reg(SVGA_REG_DEV_CAP) != 0;
	}
	if (!(v->hw.caps & SVGA_CAP_EXTENDED_FIFO))
		return 0;
	if (vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_3D_HWVERSION_REVISED) &&
	    vmsvga2_hw_has_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED))
		return vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED) != 0;
	return vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION) != 0;
}

/* ---- memory limits, driver identity, shader model ------------------------ */

/* What the device lets the driver use beyond the guest-backed pool (that
 * one is read with the capability table, below):
 *   - max_primary_mem: how large a primary surface may be.  A device with
 *     guest-backed objects states it in SVGA_REG_MAX_PRIMARY_MEM; anything
 *     older shows its primary from VRAM, which bounds it.  A guest-backed
 *     device that answers 0 is left at 0 ("does not say"); the scan-out
 *     limits and DRM_VMW_PARAM_MAX_FB_SIZE fall back to VRAM themselves.
 *   - memory_size: the surface memory a client without guest-backed
 *     objects may use -- what SVGA_REG_MEMORY_SIZE counts beyond VRAM on
 *     a device with second-generation regions, and an arbitrary 512 MB on
 *     one without (every 3D-capable device has them anyway) or where that
 *     register leaves nothing. */
static void vmw_memory_limits(struct vmw_device *v)
{
	if (v->hw.caps & SVGA_CAP_GMR2) {
		uint64_t mem = vmsvga2_hw_read_reg(SVGA_REG_MEMORY_SIZE);

		/* A host answering no more than VRAM leaves nothing rather
		 * than wrapping -- and nothing is not an answer a client can
		 * budget against (the GL stack would flush after every
		 * surface), so it gets the fixed size instead. */
		v->memory_size = mem > v->hw.vram_size ? mem - v->hw.vram_size :
							 0;
	}
	if (!v->memory_size)
		v->memory_size = 512ULL * 1024 * 1024;
	if (v->hw.caps & SVGA_CAP_GBOBJECTS)
		v->max_primary_mem = vmsvga2_hw_read_reg(SVGA_REG_MAX_PRIMARY_MEM);
	else
		v->max_primary_mem = v->hw.vram_size;
}

/* Which guest driver the host is talking to, for the host's own logs and
 * for any behaviour it keys on the driver: the registers exist on devices
 * with CAP2_DX2.  The id is written first, then the three version words,
 * and SUBMIT latches them.
 *
 * The id says "unknown": the defined ids name other drivers, and a host
 * that keys behaviour on one of them would apply it to this driver, with
 * version numbers that mean nothing in that driver's history.  Version 2 is
 * this driver's own version (as DRM_IOCTL_VERSION reports it); there is no
 * kernel version to put in version 1. */
#define VMW_GUEST_DRIVER_ID SVGA_REG_GUEST_DRIVER_ID_UNKNOWN

void vmw_write_driver_id(const struct vmw_device *v,
				const struct drm_driver *drv)
{
	if (!(v->cap2 & SVGA_CAP2_DX2))
		return;
	vmsvga2_hw_write_reg(SVGA_REG_GUEST_DRIVER_ID, VMW_GUEST_DRIVER_ID);
	vmsvga2_hw_write_reg(SVGA_REG_GUEST_DRIVER_VERSION1, 0);
	vmsvga2_hw_write_reg(SVGA_REG_GUEST_DRIVER_VERSION2,
			     (uint32_t)drv->major << 24 |
			     (uint32_t)drv->minor << 16 |
			     (uint32_t)drv->patch);
	vmsvga2_hw_write_reg(SVGA_REG_GUEST_DRIVER_VERSION3, 0);
	vmsvga2_hw_write_reg(SVGA_REG_GUEST_DRIVER_ID,
			     SVGA_REG_GUEST_DRIVER_ID_SUBMIT);
}

/* The device's capability bits are logged by the hardware layer at probe;
 * what is this driver's own conclusion is the shader model it offers. */
static void vmw_print_sm_type(const struct vmw_device *v)
{
	static const char *const names[] = {
		[VMW_SM_LEGACY] = "Legacy",
		[VMW_SM_4] = "SM4",
		[VMW_SM_4_1] = "SM4_1",
		[VMW_SM_5] = "SM_5",
		[VMW_SM_5_1X] = "SM_5_1X",
		[VMW_SM_MAX] = "Invalid"
	};

	BUILD_BUG_ON(ARRAY_SIZE(names) != (VMW_SM_MAX + 1));
	kprintf("[drm] vmwgfx: available shader model: %s\n",
		names[vmw_sm_type(v)]);
}

/* ---- scan-out limits ---------------------------------------------------- */

/* The standard mode list, largest first.  Shared with the boot console
 * (kernel/dev/video/vmsvga2.c), so the console and a display server agree
 * about what this device can show. */
static const uint32_t vmw_builtin_modes[][2] = {
	{ 2560, 1600 }, { 2560, 1440 }, { 1920, 1440 }, { 1920, 1200 },
	{ 1920, 1080 }, { 1856, 1392 }, { 1792, 1344 }, { 1680, 1050 },
	{ 1600, 1200 }, { 1600, 900 },	{ 1440, 900 },	{ 1400, 1050 },
	{ 1366, 768 },	{ 1360, 768 },	{ 1280, 1024 }, { 1280, 960 },
	{ 1280, 800 },	{ 1280, 768 },	{ 1280, 720 },	{ 1152, 864 },
	{ 1024, 768 },	{ 800, 600 },	{ 640, 480 },
};

#define VMW_SCANOUT_MAX_DIM 8192U

/* The build's screen-size preference (Makefile: SCREEN_SIZE,
 * MAX_SCREEN_SIZE), the same ceilings the boot console applies.  They bound
 * the mode this comes up in, not the mode list: a device that can scan out
 * more than the build asked for still offers those modes to a display
 * server that explicitly asks for one. */
#ifndef SCREEN_MAX_WIDTH
#define SCREEN_MAX_WIDTH 0xFFFFFFFFU
#endif
#ifndef SCREEN_MAX_HEIGHT
#define SCREEN_MAX_HEIGHT 0xFFFFFFFFU
#endif
#if defined(SCREEN_LARGE)
#define VMW_PREF_MAX_WIDTH 1920U
#define VMW_PREF_MAX_HEIGHT 1200U
#else
#define VMW_PREF_MAX_WIDTH 1280U
#define VMW_PREF_MAX_HEIGHT 800U
#endif

/* How large a screen this device can really show -- which is not what the
 * framebuffer aperture suggests.
 *
 * The console this driver takes over from puts its pixels in the aperture,
 * so its mode is bounded by the graphics memory the virtual machine was
 * configured with, and the device derives SVGA_REG_MAX_WIDTH/MAX_HEIGHT from
 * exactly that: 4 MB of it comes back as 1176x885, which is a statement
 * about the aperture and not about any display.
 *
 * A screen target never reads that aperture.  The image stays in guest
 * memory and the host is told where it is, so the bounds that apply are the
 * device's own for such a scan-out: the primary surface memory the device
 * reports and the largest image it can sample.  Under those the screen can
 * be far larger than the aperture would ever have allowed.
 *
 * A screen object is half way: its image is in a guest memory region too,
 * but its backing store is the aperture (see vmw_define_screen()), so the
 * aperture's memory still bounds it -- only its geometry registers do not.
 *
 * Every widening is conditional on the device reporting the register that
 * justifies it; a device that reports nothing keeps the aperture's bounds,
 * which is what this driver used for every mode before.
 */
static void vmw_scanout_limits(struct vmw_device *v)
{
	uint64_t mem = v->hw.vram_size;
	uint32_t w = v->hw.max_width ? v->hw.max_width : VMW_SCANOUT_MAX_DIM;
	uint32_t h = v->hw.max_height ? v->hw.max_height : VMW_SCANOUT_MAX_DIM;

	v->scanout_in_guest_memory = v->has_screen_object || v->has_screentarget;
	if (v->has_screentarget) {
		uint32_t reg;

		/* A screen target reads guest memory only; the memory that
		 * bounds it is the primary-surface memory the device states
		 * (vmw_device.max_primary_mem, read at init). */
		if (v->max_primary_mem > mem)
			mem = v->max_primary_mem;
		reg = v->devcaps[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH];
		if (reg > w)
			w = reg;
		reg = v->devcaps[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT];
		if (reg > h)
			h = reg;
		/* Only a bound, and only where the device states one: a zero
		 * here means "unstated", not "zero-sized". */
		reg = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_WIDTH);
		if (reg && reg < w)
			w = reg;
		reg = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_HEIGHT);
		if (reg && reg < h)
			h = reg;
	} else if (v->has_screen_object) {
		uint32_t reg;

		/* A screen object's image is in guest memory, but its backing
		 * store is in the aperture (vmw_define_screen()), so the
		 * memory bound stays the aperture's: screen-object modes are
		 * bounded by VRAM.  The geometry bound does not: MAX_WIDTH/
		 * MAX_HEIGHT are the device's statement about scanning the
		 * aperture out directly, and a screen object is not that; it
		 * is allowed 8192 either way and the memory decides.  On 4 MB that
		 * is the difference between 1152x864 and 1280x800. */
		w = VMW_SCANOUT_MAX_DIM;
		h = VMW_SCANOUT_MAX_DIM;
		if (v->hw.caps & SVGA_CAP_GMR2) {
			/* The image's region: the register is only meaningful
			 * on a device with the second-generation regions. */
			reg = vmsvga2_hw_read_reg(SVGA_REG_GMRS_MAX_PAGES);
			if (reg && (uint64_t)reg * PAGE_SIZE < mem)
				mem = (uint64_t)reg * PAGE_SIZE;
		}
	}
	if (w > VMW_SCANOUT_MAX_DIM)
		w = VMW_SCANOUT_MAX_DIM;
	if (h > VMW_SCANOUT_MAX_DIM)
		h = VMW_SCANOUT_MAX_DIM;
	v->scanout_max_mem = mem;
	v->scanout_max_width = w;
	v->scanout_max_height = h;
}

static int vmw_mode_fits(const struct vmw_device *v, uint32_t w, uint32_t h)
{
	return w != 0 && h != 0 && w <= v->scanout_max_width &&
	       h <= v->scanout_max_height &&
	       (uint64_t)w * h * 4 <= v->scanout_max_mem;
}

/* ...and is one the build allows the machine to come up in.
 * MAX_SCREEN_SIZE overrules everyone, the host included. */
static int vmw_mode_allowed(const struct vmw_device *v, uint32_t w, uint32_t h)
{
	return vmw_mode_fits(v, w, h) && w <= SCREEN_MAX_WIDTH &&
	       h <= SCREEN_MAX_HEIGHT;
}

/* ...and is one the build would pick on its own.  SCREEN_SIZE bounds what
 * the mode list may offer; it does not overrule the host. */
static int vmw_mode_preferable(const struct vmw_device *v, uint32_t w,
			       uint32_t h)
{
	return vmw_mode_allowed(v, w, h) && w <= VMW_PREF_MAX_WIDTH &&
	       h <= VMW_PREF_MAX_HEIGHT;
}

void vmwgfx_init(void)
{
	struct vmw_device *v = &g_vmw;

	if (!vmsvga2_hw_present())
		return;
	mm_memset(v, 0, sizeof(*v));
	vmsvga2_hw_geometry(&v->hw);
	v->has_gmr = (v->hw.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) != 0;
	v->has_screen_object = v->has_gmr &&
			       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_SCREEN_OBJECT_2);
	v->has_3d = vmw_probe_3d(v);
	vmw_fence_manager_init(v);
	mm_rwsem_init(&v->binding_lock, "vmw_binding");
	vmw_cmd_reserve_init(v);

	/* Guest-backed objects and the DX (VGPU10) command set. */
	v->has_gb = v->has_3d && (v->hw.caps & SVGA_CAP_GBOBJECTS) != 0;
	v->has_cmdbuf = (v->hw.caps & SVGA_CAP_COMMAND_BUFFERS) != 0;
	v->has_dx = v->has_gb && v->has_cmdbuf && (v->hw.caps & SVGA_CAP_DX) != 0;
	if (v->hw.caps & SVGA_CAP_CAP2_REGISTER)
		v->cap2 = vmsvga2_hw_read_reg(SVGA_REG_CAP2);
	vmw_memory_limits(v);
	if (v->has_gb) {
		vmw_devcaps_create(v);
		/* Which register carries the guest-backed memory pool is
		 * decided by a capability, not by trying one and seeing
		 * whether it answers: on a device without CAP2_GB_MEMSIZE_2
		 * the 64-bit-capable register is simply not implemented and
		 * reads back whatever the register file has there -- a
		 * plausible-looking wrong number, not a zero the fallback
		 * would catch.
		 *
		 * The value matters well beyond bookkeeping: the client's GL
		 * stack flushes preemptively once a batch has referenced
		 * half of it, so reporting a pool far smaller than the real
		 * one turns every frame into a stream of tiny submissions. */
		if (v->cap2 & SVGA_CAP2_GB_MEMSIZE_2)
			v->max_mob_memory = (uint64_t)vmsvga2_hw_read_reg(
						    SVGA_REG_GBOBJECT_MEM_SIZE_KB) *
					    1024ULL;
		else
			v->max_mob_memory = (uint64_t)vmsvga2_hw_read_reg(
						    SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB) *
					    1024ULL;
		if (!v->max_mob_memory)
			v->max_mob_memory = 512ULL << 20;
		v->max_mob_size = vmsvga2_hw_read_reg(SVGA_REG_MOB_MAX_SIZE);
		if (!v->max_mob_size)
			v->max_mob_size = 128ULL << 20;
		v->has_screentarget = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_WIDTH) != 0;
		/* The message channel is not part of the device at all -- it
		 * is a port-I/O protocol the hypervisor answers -- so it is
		 * probed separately and its absence changes nothing else. */
		v->has_msg = vmw_msg_probe();
		if (v->has_dx) {
			v->has_dx = v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] != 0;
			/* A shader model takes both halves of the answer: the
			 * device-capability register says the host can run
			 * it, and CAP2 says this device speaks the command
			 * set that comes with it -- SM4.1 rides on CAP2_DX2,
			 * SM5 on CAP2_DX3.  The devcap alone is not enough,
			 * and believing it means sending a command the device
			 * does not have, which halts the command-buffer
			 * context. */
			v->has_sm41 = v->has_dx && (v->cap2 & SVGA_CAP2_DX2) &&
				      v->devcaps[SVGA3D_DEVCAP_SM41] != 0;
			v->has_sm5 = v->has_sm41 && (v->cap2 & SVGA_CAP2_DX3) &&
				     v->devcaps[SVGA3D_DEVCAP_SM5] != 0;
			v->has_gl43 = v->has_sm5 && v->devcaps[SVGA3D_DEVCAP_GL43] != 0;
		}
		/* Report only what the validator carries. */
		if (!v->has_dx) {
			v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] = 0;
			v->devcaps[SVGA3D_DEVCAP_SM41] = 0;
			v->devcaps[SVGA3D_DEVCAP_SM5] = 0;
			v->devcaps[SVGA3D_DEVCAP_GL43] = 0;
		}
	}
	vmw_scanout_limits(v);
	v->drm.max_width = v->scanout_max_width;
	v->drm.max_height = v->scanout_max_height;
	v->drm.refresh_hz = 60;
	if (v->has_gb)
		kprintf("[drm] vmwgfx: guest-backed memory pool %uMB, largest object %uMB\n",
			(uint32_t)(v->max_mob_memory >> 20),
			(uint32_t)(v->max_mob_size >> 20));
	kprintf("[drm] vmwgfx: scan-out from %s, up to %ux%u within %uKB\n",
		v->scanout_in_guest_memory ? "guest memory" :
					     "the framebuffer aperture",
		v->scanout_max_width, v->scanout_max_height,
		(uint32_t)(v->scanout_max_mem / 1024));
	if (drm_dev_register(&v->drm, vmw_driver_table(), vmsvga2_hw_pci(), v) != 0)
		return;

	/* Guest-backed objects come AFTER the device is registered: the object
	 * tables are GEM objects on this same drm_device, and the GEM layer
	 * reaches the backend through dev->drv, which registration is what
	 * fills in.  Setting them up first faulted on a NULL dev->drv the
	 * moment anything failed.
	 *
	 * Doing it here, at init, is what lets the console come up on KMS
	 * immediately below: the display moves to the guest-backed model once
	 * and the console is a client of it from the start, rather than the
	 * scan-out changing under a console that is already painting. */
	if (vmw_gb_init(v) != 0) {
		kprintf("[drm] vmwgfx: guest-backed object setup failed; 3D off\n");
		v->has_gb = 0;
		v->has_dx = 0;
		v->has_3d = 0;
	}
	/* vmw_cmdbuf_init() turns DX off if the command-buffer context would
	 * not start; the capability array has to say the same thing, because
	 * that is where Mesa reads DXCONTEXT from. */
	if (!v->has_dx) {
		v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM41] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM5] = 0;
		v->devcaps[SVGA3D_DEVCAP_GL43] = 0;
		v->has_sm41 = 0;
		v->has_sm5 = 0;
		v->has_gl43 = 0;
	}
	/* The cursor state (MOB cursors where the device has them); a
	 * failure leaves the register/FIFO cursor working. */
	(void)vmw_cursor_init(v);
	/* The device is up: say what it runs, introduce the driver in the
	 * host's log and tell the device which driver it is talking to. */
	vmw_print_sm_type(v);
	vmw_msg_init(v);
	vmw_write_driver_id(v, v->drm.drv);

	/* One virtual connector.  The mode list is the standard set bounded by
	 * what this device can actually scan out, largest first, and the
	 * largest is the preferred one -- the console takes the first mode on
	 * the list and a display server asks for the preferred one, so this
	 * is where the resolution the machine comes up in is decided.  The
	 * mode the boot console left behind is kept on the list too, so
	 * falling back to it needs no modeset the device could refuse. */
	uint32_t pw = 0, ph = 0;
	/* What the host recommends, if it said and this scan-out can carry
	 * it.  The console driver asked at probe; the answer applies here
	 * rather than there, because here is where the aperture's limits
	 * stop applying and the recommendation usually becomes reachable. */
	if (vmsvga2_get_host_preferred(&pw, &ph) != 0 ||
	    !vmw_mode_allowed(v, pw, ph))
		pw = 0;
	for (unsigned i = 0;
	     i < sizeof(vmw_builtin_modes) / sizeof(vmw_builtin_modes[0]) && !pw;
	     i++) {
		if (vmw_mode_preferable(v, vmw_builtin_modes[i][0],
					vmw_builtin_modes[i][1])) {
			pw = vmw_builtin_modes[i][0];
			ph = vmw_builtin_modes[i][1];
		}
	}
	if (!pw)
		vmw_kms_initial_size(v, &pw, &ph);
	/* A size in millimetres at 96 dpi for the preferred mode. */
	uint32_t mm_w = pw * 254 / 960, mm_h = ph * 254 / 960;
	int c = drm_connector_add(&v->drm, DRM_MODE_CONNECTOR_VIRTUAL, mm_w, mm_h);
	if (c >= 0) {
		struct drm_mode_modeinfo m;
		drm_mode_fill(&m, pw, ph, 60, 1);
		drm_connector_add_mode(&v->drm, c, &m);
		if (v->hw.width != pw || v->hw.height != ph) {
			drm_mode_fill(&m, v->hw.width, v->hw.height, 60, 0);
			drm_connector_add_mode(&v->drm, c, &m);
		}
		for (unsigned i = 0;
		     i < sizeof(vmw_builtin_modes) / sizeof(vmw_builtin_modes[0]);
		     i++) {
			uint32_t sw = vmw_builtin_modes[i][0];
			uint32_t sh = vmw_builtin_modes[i][1];
			if (sw == pw && sh == ph)
				continue;
			if (sw == v->hw.width && sh == v->hw.height)
				continue;
			if (!vmw_mode_fits(v, sw, sh))
				continue;
			drm_mode_fill(&m, sw, sh, 60, 0);
			drm_connector_add_mode(&v->drm, c, &m);
		}
	}
	/* The display units: their initial state and, with more than one
	 * head, the connectors of the further units. */
	vmw_kms_init_display(v);
	/* The connectors' probe hooks and their properties; before the console
	 * takes the display, whose mode set changes what the device reports. */
	vmw_connector_init_all(v);
	mm_rwsem_init(&v->execbuf_lock, "vmw_execbuf");
	vmsvga2_hw_set_irq_callback(vmw_irq_cb);

	/* Put the console on this device's KMS path.
	 *
	 * Here, and not earlier: it needs the connector and its mode list,
	 * which is what the block above builds.  From this point the console
	 * draws into a buffer object that gets scanned out the same way an X
	 * server's does, instead of into the framebuffer in VRAM that the
	 * device no longer reads once it is in guest-backed mode.
	 *
	 * A failure is not one: the console keeps whatever it was already
	 * drawing into, which is the boot framebuffer, and the machine looks
	 * exactly as it did before this driver existed. */
	if (drm_console_takeover(&v->drm) != 0)
		kprintf("[drm] vmwgfx: console stays on the framebuffer\n");
	/* What is on the screen -- which with guest-memory scan-out is not
	 * what the mode registers say.  Those keep describing the framebuffer
	 * aperture the display no longer comes from: on a machine whose
	 * graphics memory cannot hold the mode, they stay at whatever the
	 * boot console could fit while the screen object shows the real one,
	 * and printing them made this line contradict the console's own. */
	uint32_t sw = v->hw.width, sh = v->hw.height;
	if (v->st_defined) {
		sw = v->st_w;
		sh = v->st_h;
	} else if (v->screen_defined) {
		sw = v->screen_w;
		sh = v->screen_h;
	}
	vmw_overlay_init(v);
	kprintf("[drm] vmwgfx: %ux%u, %s scan-out, gmr %s, 3d %s, gb %s, dx %s (sm4.1 %s, sm5 %s), cmdbuf %s, irq %s, overlay streams %d\n",
		sw, sh,
		vmw_stdu_available(v)   ? "screen-target" :
		v->has_screen_object	? "screen-object" : "legacy",
		v->has_gmr ? "yes" : "no", v->has_3d ? "yes" : "no",
		v->has_gb ? "yes" : "no", v->has_dx ? "yes" : "no",
		v->has_sm41 ? "yes" : "no", v->has_sm5 ? "yes" : "no",
		v->has_cmdbuf ? "yes" : "no",
		v->hw.irq_enabled ? "yes" : "polled",
		vmw_overlay_num_streams(v));
}
