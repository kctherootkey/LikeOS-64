// LikeOS -- vmwgfx: the device-object base every surface, context, view and
// command-buffer-managed object embeds.
//
// A device object (a surface, a context, a DX view, a shader) is named by an
// id the device knows and is referenced from more places than its owner:
// a context's binding tracker points at the surfaces and views bound to it,
// a view keeps its surface alive, the per-context command-buffer resource
// manager holds the views and shaders a command stream defined.  The base
// gives all of them one reference count, one destructor protocol and one
// list of the context bindings that point at the object, so that tearing an
// object down can find and scrub every binding to it first.
//
// What is deliberately NOT here: eviction.  Every guest-backed object in
// this driver is resident from creation to destruction (its MOB is defined
// up front), so there is no LRU, no validation pass and no backup switching.
//
// Rules of use:
//   - vmw_resource_init() at creation: count 1, the creator's reference.
//   - vmw_resource_reference()/vmw_resource_unreference() pair up; the
//     final unreference runs the release below.
//   - release: if `hw_destroy' is set, every binding on `binding_head' is
//     killed (under vmw_device.binding_lock) and then `hw_destroy' tells the
//     device; `guest_memory_bo', when set, is unbound (func->unbind) and its
//     reference dropped; finally `res_free' frees the embedding object
//     (kfree(res) when NULL).
//   - `binding_head' is protected by vmw_device.binding_lock, a sleeping
//     lock: never hold it across drm_gem_put(), which can re-enter surface
//     destruction.
//   - Surfaces and contexts keep their own device ids (sid, cid) and their
//     own backing pointers; `id' mirrors the device id for code that only
//     sees the base.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright 2012-2014 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_RESOURCE_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_RESOURCE_H

#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>

struct vmw_device;
struct vmw_resource;
struct drm_gem_object;

/* Resources that are managed using ioctls. */
enum vmw_res_type {
	vmw_res_context,
	vmw_res_surface,
	vmw_res_stream,
	vmw_res_shader,
	vmw_res_dx_context,
	vmw_res_cotable,
	vmw_res_view,
	vmw_res_streamoutput,
	vmw_res_max
};

/* Resources that are managed using command streams. */
enum vmw_cmdbuf_res_type {
	vmw_cmdbuf_res_shader,
	vmw_cmdbuf_res_view,
	vmw_cmdbuf_res_streamoutput
};

/* What a submission does to an object's contents: nothing, makes the device
 * copy newer than guest memory (it rendered into it), or the reverse.
 * Returned by vmw_binding_dirtying() and vmw_view_dirtying(). */
#define VMW_RES_DIRTY_NONE 0
#define VMW_RES_DIRTY_SET BIT(0)
#define VMW_RES_DIRTY_CLEAR BIT(1)

/* Staging state of a command-buffer-managed resource: defined or removed by
 * a command stream that has not been committed yet, or committed. */
enum vmw_cmdbuf_res_state {
	VMW_CMDBUF_RES_COMMITTED,
	VMW_CMDBUF_RES_ADD,
	VMW_CMDBUF_RES_DEL
};

/* Members and functions common for a resource type.
 *
 * @res_type:           which kind of object this is.
 * @needs_guest_memory: the object is guest-backed and needs persistent
 *                      buffer storage.
 * @type_name:          for messages.
 * @create:             create the device object.
 * @destroy:            destroy the device object.
 * @bind:               bind the device object to its guest memory.
 * @unbind:             unbind it; `readback' asks the device to write the
 *                      object's contents back into guest memory first.
 * @commit_notify:      a command-buffer-managed resource's define or remove
 *                      command has been committed to the device; called
 *                      with binding_lock held.
 * Any hook may be NULL. */
struct vmw_res_func {
	enum vmw_res_type res_type;
	bool needs_guest_memory;
	const char *type_name;

	int (*create)(struct vmw_resource *res);
	int (*destroy)(struct vmw_resource *res);
	int (*bind)(struct vmw_resource *res);
	int (*unbind)(struct vmw_resource *res, bool readback);
	void (*commit_notify)(struct vmw_resource *res,
			      enum vmw_cmdbuf_res_state state);
};

/* The base.
 *
 * @refcount:            references; the object is released at zero.
 * @dev:                 the device.  Immutable.
 * @id:                  device id, -1 when the object has none (yet).
 * @func:                method table.  Immutable.
 * @guest_memory_bo:     guest memory the object is bound to, NULL for none.
 *                       Holds one reference, dropped at release.
 * @guest_memory_offset: where in @guest_memory_bo the object starts.
 * @guest_memory_size:   bytes of guest memory the object needs.
 * @binding_head:        the context bindings pointing at this object
 *                       (struct vmw_ctx_bindinfo.res_list); protected by
 *                       vmw_device.binding_lock.
 * @res_free:            frees the embedding object; NULL means kfree(res).
 * @hw_destroy:          destroys the device object as part of release;
 *                       NULL when the owner does that itself. */
struct vmw_resource {
	refcount_t refcount;
	struct vmw_device *dev;
	int id;
	const struct vmw_res_func *func;
	struct drm_gem_object *guest_memory_bo;
	uint64_t guest_memory_offset;
	uint64_t guest_memory_size;
	struct list_head binding_head;
	void (*res_free)(struct vmw_resource *res);
	void (*hw_destroy)(struct vmw_resource *res);
};

/* Initialise an embedded base: one reference (the caller's), no bindings,
 * no guest memory, no hw_destroy.  `id' is the device id or -1.  Returns 0. */
int vmw_resource_init(struct vmw_device *v, struct vmw_resource *res, int id,
		      void (*res_free)(struct vmw_resource *res),
		      const struct vmw_res_func *func);
struct vmw_resource *vmw_resource_reference(struct vmw_resource *res);
/* A reference, unless the object is already on its way out (count zero):
 * then NULL. */
struct vmw_resource *
vmw_resource_reference_unless_doomed(struct vmw_resource *res);
/* Drop the reference *p_res holds and clear the pointer.  NULL is fine. */
void vmw_resource_unreference(struct vmw_resource **p_res);
enum vmw_res_type vmw_res_type(const struct vmw_resource *res);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_RESOURCE_H */
