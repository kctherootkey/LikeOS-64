// LikeOS -- vmwgfx: the driver's ioctls.
//
// The DRM_VMW_* requests: parameters and capabilities, buffer objects and
// CPU access to them, fence waits, and the dispatch to the surface,
// context, shader, command-submission, display, overlay and host-message
// code.  Which of them a render node may use is decided here too.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2022 VMware, Inc., Palo Alto, CA., USA

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

/* ---- driver ioctls ----------------------------------------------------- */

int vmw_render_allowed(unsigned nr)
{
	switch (nr) {
	case DRM_VMW_GET_PARAM:
	case DRM_VMW_ALLOC_BO:
	case DRM_VMW_UNREF_DMABUF:
	case DRM_VMW_CREATE_CONTEXT:
	case DRM_VMW_UNREF_CONTEXT:
	case DRM_VMW_CREATE_SURFACE:
	case DRM_VMW_UNREF_SURFACE:
	case DRM_VMW_REF_SURFACE:
	case DRM_VMW_EXECBUF:
	case DRM_VMW_GET_3D_CAP:
	case DRM_VMW_FENCE_WAIT:
	case DRM_VMW_FENCE_SIGNALED:
	case DRM_VMW_FENCE_UNREF:
	case DRM_VMW_CREATE_SHADER:
	case DRM_VMW_UNREF_SHADER:
	case DRM_VMW_GB_SURFACE_CREATE:
	case DRM_VMW_GB_SURFACE_REF:
	case DRM_VMW_SYNCCPU:
	case DRM_VMW_CREATE_EXTENDED_CONTEXT:
	case DRM_VMW_GB_SURFACE_CREATE_EXT:
	case DRM_VMW_GB_SURFACE_REF_EXT:
	case DRM_VMW_MSG:
	/* Waiting for a fence through an event rather than an ioctl is the
	 * same operation with a different way of being told, so a render
	 * node may do it too.  UPDATE_LAYOUT is not here: it changes what
	 * the display advertises, which belongs to whoever holds the
	 * display. */
	case DRM_VMW_FENCE_EVENT:
		return 1;
	default:
		return 0;
	}
}

/* The client's view of the device.
 *
 * Asking for DRM_VMW_PARAM_MAX_MOB_MEMORY is how a client that knows about
 * guest-backed objects identifies itself (the GL stack asks for it first
 * thing on such a device); from then on this file is answered in the
 * device's own formats -- DRM_VMW_PARAM_3D_CAPS_SIZE and
 * DRM_VMW_GET_3D_CAP give the flat capability array rather than the
 * older record format, and DRM_VMW_PARAM_MAX_SURF_MEMORY the surface
 * memory rather than half the guest-backed pool. */
static long vmw_ioctl_get_param(struct vmw_device *v, struct drm_file *fp,
				struct drm_vmw_getparam_arg *a)
{
	struct vmw_file *vfp = fp->priv;
	bool gb_aware = vfp && vfp->gb_aware;

	switch (a->param) {
	case DRM_VMW_PARAM_NUM_STREAMS:
		a->value = vmw_overlay_num_streams(v);
		return 0;
	case DRM_VMW_PARAM_NUM_FREE_STREAMS:
		a->value = vmw_overlay_num_free_streams(v);
		return 0;
	case DRM_VMW_PARAM_3D:
		a->value = v->has_3d ? 1 : 0;
		return 0;
	case DRM_VMW_PARAM_HW_CAPS:
		a->value = v->hw.caps;
		return 0;
	case DRM_VMW_PARAM_HW_CAPS2:
		a->value = v->cap2;
		return 0;
	case DRM_VMW_PARAM_FIFO_CAPS:
		a->value = v->hw.fifo_caps;
		return 0;
	case DRM_VMW_PARAM_MAX_FB_SIZE:
		/* The largest primary surface; a device that does not state
		 * one shows its primary from VRAM. */
		a->value = v->max_primary_mem ? v->max_primary_mem :
						v->hw.vram_size;
		return 0;
	case DRM_VMW_PARAM_FIFO_HW_VERSION:
		/* A guest-backed device is at least the hardware version
		 * that introduced guest-backed objects, and that is all a
		 * client learns from this: the FIFO's version slots are the
		 * older interface, which such a device need not fill in --
		 * and the GL stack refuses 3D below that version.  An older
		 * device answers in the FIFO, in the slot its capabilities
		 * say is live (see vmw_probe_3d()). */
		if (v->hw.caps & SVGA_CAP_GBOBJECTS)
			a->value = SVGA3D_HWVERSION_WS8_B1;
		else if (vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_3D_HWVERSION_REVISED) &&
			 vmsvga2_hw_has_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED))
			a->value = vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED);
		else
			a->value = vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION);
		return 0;
	case DRM_VMW_PARAM_MAX_SURF_MEMORY:
		/* A client unaware of guest-backed objects budgets its
		 * surfaces against this, while on a guest-backed device they
		 * all land in the pool: half of it leaves the other half for
		 * everything else.  (A guest-backed client does not ask; it
		 * accounts the pool itself.) */
		if ((v->hw.caps & SVGA_CAP_GBOBJECTS) && !gb_aware)
			a->value = v->max_mob_memory / 2;
		else
			a->value = v->memory_size;
		return 0;
	case DRM_VMW_PARAM_3D_CAPS_SIZE:
		a->value = vmw_devcaps_size(v, gb_aware);
		return 0;
	case DRM_VMW_PARAM_MAX_MOB_MEMORY:
		if (vfp)
			vfp->gb_aware = 1;
		a->value = v->max_mob_memory;
		return 0;
	case DRM_VMW_PARAM_MAX_MOB_SIZE:
		a->value = v->max_mob_size;
		return 0;
	case DRM_VMW_PARAM_SCREEN_TARGET:
		/* Whether the screen target is the display unit in use, not
		 * whether a size register answered. */
		a->value = vmw_active_display_unit(v) == vmw_du_screen_target;
		return 0;
	case DRM_VMW_PARAM_DX:
		a->value = has_sm4_context(v);
		return 0;
	case DRM_VMW_PARAM_SM4_1:
		a->value = has_sm4_1_context(v);
		return 0;
	case DRM_VMW_PARAM_SM5:
		a->value = has_sm5_context(v);
		return 0;
	case DRM_VMW_PARAM_GL43:
		a->value = has_gl43_context(v);
		return 0;
	case DRM_VMW_PARAM_DEVICE_ID:
		a->value = v->drm.pci ? v->drm.pci->device_id : 0x0405;
		return 0;
	case DRM_VMW_PARAM_USER_SRF:
		/* Surfaces are always the kernel's here. */
		a->value = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

/* DRM_VMW_GET_3D_CAP: the capability table, in the format this client
 * reads (vmw_devcaps.c), at most max_size bytes of it. */
static long vmw_ioctl_get_3d_cap(struct vmw_device *v, struct drm_file *fp,
				 struct drm_vmw_get_3d_cap_arg *a)
{
	struct vmw_file *vfp = fp->priv;
	bool gb_aware = vfp && vfp->gb_aware;
	uint32_t size;
	void *bounce;
	int ret;

	if (a->pad64 != 0 || a->max_size == 0) {
		VMW_DEBUG_USER("Illegal GET_3D_CAP argument.\n");
		return -EINVAL;
	}
	size = vmw_devcaps_size(v, gb_aware);
	if (size == 0)
		return -ENODEV;	/* no 3D, no table */
	if (a->max_size < size)
		size = a->max_size;

	bounce = kalloc(size);
	if (!bounce)
		return -ENOMEM;
	mm_memset(bounce, 0, size);
	ret = vmw_devcaps_copy(v, gb_aware, bounce, size);
	if (ret == 0 &&
	    (!validate_user_ptr(a->buffer, size) ||
	     copy_to_user((void *)(uintptr_t)a->buffer, bounce, size) != 0))
		ret = -EFAULT;
	kfree(bounce);
	return ret;
}

/* ---- presents -----------------------------------------------------------
 *
 * The client names rectangles of a surface (PRESENT) or of the screen
 * (PRESENT_READBACK); the display unit in use does the rest (vmw_kms.c).
 * Both belong to whoever holds the display. */

/* The most rectangles one call may name: far beyond any real damage list,
 * and it keeps the copy of them a bounded allocation. */
#define VMW_PRESENT_MAX_CLIPS (256u * 1024u)

/* Copy the client's rectangles in.  NULL with *ret set on failure. */
static struct drm_vmw_rect *vmw_present_clips(uint64_t clips_ptr,
					      uint32_t num_clips, int *ret)
{
	struct drm_vmw_rect *clips;
	size_t bytes;

	if (clips_ptr == 0) {
		VMW_DEBUG_USER("Variable clips_ptr must be specified.\n");
		*ret = -EINVAL;
		return NULL;
	}
	if (num_clips > VMW_PRESENT_MAX_CLIPS) {
		*ret = -ENOMEM;
		return NULL;
	}
	bytes = (size_t)num_clips * sizeof(*clips);
	clips = kalloc(bytes);
	if (!clips) {
		*ret = -ENOMEM;
		return NULL;
	}
	mm_memset(clips, 0, bytes);
	if (!validate_user_ptr(clips_ptr, bytes) ||
	    copy_from_user(clips, (const void *)(uintptr_t)clips_ptr, bytes) != 0) {
		kfree(clips);
		*ret = -EFAULT;
		return NULL;
	}
	*ret = 0;
	return clips;
}

static long vmw_ioctl_present(struct vmw_device *v, struct drm_file *fp,
			      struct drm_vmw_present_arg *a)
{
	struct drm_vmw_rect *clips;
	struct drm_framebuffer *fb;
	struct drm_gem_object *so;
	int ret;

	if (!fp->is_master)
		return -EACCES;
	if (a->num_clips == 0)
		return 0;
	clips = vmw_present_clips(a->clips_ptr, a->num_clips, &ret);
	if (!clips)
		return ret;

	fb = drm_fb_lookup(&v->drm, a->fb_id);
	if (!fb) {
		VMW_DEBUG_USER("Invalid framebuffer id.\n");
		ret = -ENOENT;
		goto out_clips;
	}
	/* The surface by the client's handle; the lookup's reference keeps
	 * it alive across the present. */
	so = drm_gem_lookup(fp, a->sid);
	if (!so || so->kind != DRM_GEM_SURFACE || !so->priv) {
		ret = -EINVAL;
		goto out_surface;
	}
	struct vmw_surface *srf = so->priv;

	ret = vmw_kms_present(v, fp, fb, srf, srf->sid, a->dest_x, a->dest_y,
			      clips, a->num_clips);
out_surface:
	if (so)
		drm_gem_put(so);
out_clips:
	kfree(clips);
	return ret;
}

static long vmw_ioctl_present_readback(struct vmw_device *v,
				       struct drm_file *fp,
				       struct drm_vmw_present_readback_arg *a)
{
	struct drm_vmw_rect *clips;
	struct drm_framebuffer *fb;
	int ret;

	if (!fp->is_master)
		return -EACCES;
	if (a->num_clips == 0)
		return 0;
	clips = vmw_present_clips(a->clips_ptr, a->num_clips, &ret);
	if (!clips)
		return ret;

	fb = drm_fb_lookup(&v->drm, a->fb_id);
	if (!fb) {
		VMW_DEBUG_USER("Invalid framebuffer id.\n");
		ret = -ENOENT;
		goto out_clips;
	}
	/* Read back into what? -- only a framebuffer with a buffer object
	 * behind it has somewhere for the pixels to go. */
	if (!fb->obj || fb->obj->kind != DRM_GEM_BO) {
		VMW_DEBUG_USER("Framebuffer not buffer backed.\n");
		ret = -EINVAL;
		goto out_clips;
	}
	ret = vmw_kms_readback(v, fp, fb, a->fence_rep, clips, a->num_clips);
out_clips:
	kfree(clips);
	return ret;
}

long vmw_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		      unsigned dir, void *kb, unsigned size, int *handled)
{
	struct vmw_device *v = dev->priv;
	(void)dir;
	*handled = 1;

	switch (nr) {
	case DRM_VMW_GET_PARAM:
		return vmw_ioctl_get_param(v, fp, kb);
	case DRM_VMW_ALLOC_BO: {
		union drm_vmw_alloc_bo_arg *a = kb;
		uint32_t req_size = a->req.size;
		if (req_size == 0 || req_size > (512u << 20))
			return -EINVAL;
		struct drm_gem_object *o = drm_gem_alloc(dev, DRM_GEM_BO, req_size);
		if (!o)
			return -ENOMEM;
		int rc = drm_gem_alloc_pages(o);
		if (rc == 0)
			rc = vmw_gem_init(o);
		if (rc) {
			drm_gem_put(o);
			return rc;
		}
		uint32_t h;
		rc = drm_gem_handle_create(fp, o, &h);
		if (rc) {
			drm_gem_put(o);
			return rc;
		}
		struct vmw_bo *b = o->priv;
		a->rep.handle = h;
		a->rep.map_handle = drm_gem_mmap_offset(o);
		a->rep.cur_gmr_id = b->gmr_id >= 0 ? (uint32_t)b->gmr_id : 0;
		a->rep.cur_gmr_offset = 0;
		drm_gem_put(o);
		return 0;
	}
	case DRM_VMW_UNREF_DMABUF: {
		struct drm_vmw_handle_close_arg *a = kb;
		return drm_gem_handle_delete(fp, a->handle);
	}
	case DRM_VMW_SYNCCPU: {
		struct drm_vmw_synccpu_arg *a = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
		if (!o)
			return -ENOENT;
		int rc = 0;
		/* Ask the device first, exactly as DRM_VMW_FENCE_WAIT does.
		 * Without it this tests a flag only an interrupt or an
		 * unrelated thread would have set, so a fence that passed long
		 * ago still looks pending and the map sleeps for a tick.  This
		 * call is on the display path: gbm_bo_map() reaches it once
		 * per frame. */
		vmw_fence_check(v);
		if (a->op == drm_vmw_synccpu_grab && o->fence && !o->fence->signaled) {
			if (a->flags & drm_vmw_synccpu_dontblock)
				rc = -EBUSY;
			else {
				/* Arms the fence interrupt for the wait where
				 * the device needs it; a wait that runs out
				 * is reported as it always was. */
				rc = vmw_fence_obj_wait(v, o->fence, false, true,
							VMW_FENCE_TIMEOUT_NS);
				if (rc == -EBUSY)
					rc = -ETIMEDOUT;
			}
		}
		drm_gem_put(o);
		return rc;
	}
	case DRM_VMW_FENCE_WAIT:
		return vmw_fence_obj_wait_ioctl(v, fp, kb);
	case DRM_VMW_FENCE_SIGNALED:
		return vmw_fence_obj_signaled_ioctl(v, fp, kb);
	case DRM_VMW_FENCE_UNREF:
		return vmw_fence_obj_unref_ioctl(v, fp, kb);
	case DRM_VMW_GET_3D_CAP:
		return vmw_ioctl_get_3d_cap(v, fp, kb);
	/* The legacy surface pair: what a host without guest-backed objects
	 * offers, and the only 3D path there. */
	case DRM_VMW_CREATE_SURFACE:
		return vmw_ioctl_create_surface(v, fp, kb);
	case DRM_VMW_REF_SURFACE:
		return vmw_ioctl_ref_surface(v, fp, kb);
	case DRM_VMW_GB_SURFACE_CREATE:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_create(v, fp, kb, 0);
	case DRM_VMW_GB_SURFACE_CREATE_EXT:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_create(v, fp, kb, 1);
	case DRM_VMW_GB_SURFACE_REF:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_ref(v, fp, kb, 0);
	case DRM_VMW_GB_SURFACE_REF_EXT:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_ref(v, fp, kb, 1);
	case DRM_VMW_UNREF_SURFACE:
		return vmw_ioctl_unref_surface(v, fp, kb);
	case DRM_VMW_CREATE_CONTEXT: {
		struct drm_vmw_context_arg *a = kb;
		if (!v->has_3d)
			return -ENODEV;
		return vmw_ioctl_create_context(v, fp, 0, &a->cid);
	}
	case DRM_VMW_CREATE_EXTENDED_CONTEXT: {
		union drm_vmw_extended_context_arg *a = kb;
		int dx = a->req == drm_vmw_context_dx;
		if (!v->has_3d || (dx && !v->has_dx))
			return -ENODEV;
		return vmw_ioctl_create_context(v, fp, dx, &a->rep.cid);
	}
	case DRM_VMW_UNREF_CONTEXT: {
		struct drm_vmw_context_arg *a = kb;
		return vmw_ioctl_unref_context(v, fp, (uint32_t)a->cid);
	}
	case DRM_VMW_CREATE_SHADER:
		return vmw_ioctl_create_shader(v, fp, kb);
	case DRM_VMW_UNREF_SHADER: {
		struct drm_vmw_shader_arg *a = kb;
		return vmw_ioctl_unref_shader(v, fp, a->handle);
	}
	case DRM_VMW_EXECBUF: {
		struct drm_vmw_execbuf_arg arg;
		mm_memset(&arg, 0, sizeof(arg));
		/* Version 1 callers pass 24 bytes: no context, no fence fd. */
		mm_memcpy(&arg, kb, size < sizeof(arg) ? size : sizeof(arg));
		if (size < sizeof(arg)) {
			arg.context_handle = SVGA3D_INVALID_ID;
			arg.imported_fence_fd = -1;
			arg.flags = 0;
		}
		return vmw_execbuf(v, fp, &arg);
	}
	case DRM_VMW_FENCE_EVENT:
		return vmw_ioctl_fence_event(v, fp, kb);
	case DRM_VMW_UPDATE_LAYOUT:
		return vmw_ioctl_update_layout(v, kb);
	case DRM_VMW_MSG:
		return vmw_ioctl_msg(v, kb);
	/* the video overlay streams: the display's owner only */
	case DRM_VMW_CONTROL_STREAM:
		if (size < sizeof(struct drm_vmw_control_stream_arg))
			return -EINVAL;
		return vmw_ioctl_control_stream(v, fp, kb);
	case DRM_VMW_CLAIM_STREAM:
		return vmw_ioctl_claim_stream(v, fp, kb);
	case DRM_VMW_UNREF_STREAM:
		return vmw_ioctl_unref_stream(v, fp, kb);
	case DRM_VMW_CURSOR_BYPASS:
		return vmw_ioctl_cursor_bypass(v, fp, kb);
	case DRM_VMW_PRESENT:
		return vmw_ioctl_present(v, fp, kb);
	case DRM_VMW_PRESENT_READBACK:
		return vmw_ioctl_present_readback(v, fp, kb);
	default:
		return v->has_3d ? -ENOSYS : -ENODEV;
	}
}
