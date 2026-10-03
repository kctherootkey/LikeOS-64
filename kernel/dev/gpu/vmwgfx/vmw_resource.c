// LikeOS -- vmwgfx: the device-object base.
//
// Reference counting and release for the objects that embed a
// struct vmw_resource (surfaces, contexts, views, command-buffer-managed
// shaders).  See vmw_resource.h for the rules.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>

struct vmw_resource *vmw_resource_reference(struct vmw_resource *res)
{
	refcount_inc(&res->refcount);
	return res;
}

struct vmw_resource *
vmw_resource_reference_unless_doomed(struct vmw_resource *res)
{
	return refcount_inc_not_zero(&res->refcount) ? res : NULL;
}

/* The last reference is gone.
 *
 * Order matters.  The guest memory is unbound before anything else, while
 * the device object still exists to be unbound.  Then every binding that
 * points at the object is forgotten -- under the binding lock, because the
 * contexts' trackers link into the same lists -- BEFORE the device is told
 * to destroy it: a binding record that outlived the object would be
 * re-emitted later, naming an id the device no longer has (or that by then
 * belongs to something else).  Only then is the memory freed. */
static void vmw_resource_release(struct vmw_resource *res)
{
	struct vmw_device *v = res->dev;

	if (res->guest_memory_bo) {
		if (res->func && res->func->unbind)
			res->func->unbind(res, false);
		drm_gem_put(res->guest_memory_bo);
		res->guest_memory_bo = NULL;
	}

	if (res->hw_destroy != NULL) {
		mm_write_lock(&v->binding_lock);
		vmw_binding_res_list_kill(&res->binding_head);
		mm_write_unlock(&v->binding_lock);
		res->hw_destroy(res);
	}

	if (res->res_free != NULL)
		res->res_free(res);
	else
		kfree(res);
}

void vmw_resource_unreference(struct vmw_resource **p_res)
{
	struct vmw_resource *res = *p_res;

	*p_res = NULL;
	if (!res)
		return;
	if (refcount_dec_and_test(&res->refcount))
		vmw_resource_release(res);
}

int vmw_resource_init(struct vmw_device *v, struct vmw_resource *res, int id,
		      void (*res_free)(struct vmw_resource *res),
		      const struct vmw_res_func *func)
{
	refcount_set(&res->refcount, 1);
	res->hw_destroy = NULL;
	res->res_free = res_free;
	res->dev = v;
	res->func = func;
	INIT_LIST_HEAD(&res->binding_head);
	res->id = id;
	res->guest_memory_bo = NULL;
	res->guest_memory_offset = 0;
	res->guest_memory_size = 0;
	return 0;
}

enum vmw_res_type vmw_res_type(const struct vmw_resource *res)
{
	return res->func->res_type;
}
