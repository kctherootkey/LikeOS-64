// LikeOS -- vmwgfx: DX views and state objects.
//
// A DX view (shader-resource, render-target, depth-stencil, unordered
// access) is a device object a command stream defines inside a DX context:
// an id in one of the context's object tables, describing a window onto a
// surface.  The device keeps that id pointing at the surface's id and
// nothing else, so the driver has to remember the relation itself: when the
// surface is destroyed, every view of it must be destroyed first -- with the
// view's context and before DESTROY_GB_SURFACE reaches the device -- or the
// device refuses the surface destroy and the surface id is reused while the
// device still holds it.
//
// Views live in the context's command-buffer resource manager (keyed by the
// client's view id) and on the surface's view_list; a view holds a
// reference to its surface and to its context.  State objects (blend,
// depth-stencil, rasterizer, sampler, element layout, stream output) are
// only mapped to their object tables here.
//
// struct vmw_view is private to vmw_so.c.
//
// Locking: the surface and object-table lists are protected by
// vmw_device.binding_lock (sleeping, taken inside execbuf_lock).  The
// staging entry points (vmw_view_add/remove/lookup) run under execbuf_lock
// like the rest of the command-buffer resource manager.  A view's release
// takes binding_lock, so the last reference to a view must not be dropped
// with binding_lock held -- vmw_cmdbuf_res_commit()/_revert()/
// _man_destroy() are called without it.  The state change of a commit
// (vmw_cmdbuf_res_commit_notify()) is made under it, in the same hold as
// the submission of the stream.
//
// A view the device no longer has (its surface or its context was
// destroyed underneath it) keeps its client id in the manager but has
// res.id == -1: a command that names it -- the client's own destroy above
// all -- has to be dropped (turned into a no-op) by the submission code
// rather than handed to the device, which would refuse it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2014-2015 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_SO_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_SO_H

#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>

struct vmw_device;
struct vmw_resource;
struct vmw_cmdbuf_res_manager;

/*
 * VMW_TRACK_VIEWS - destroy-time view and binding scrubbing.
 *
 * 1: destroying a surface first destroys every view of it (each in its own
 *    DX context) and unbinds it from every context that binds it, then
 *    sends the surface destroy; destroying a context unbinds everything it
 *    has bound and destroys its views before the context destroy.
 * 0: no such command is emitted -- the device sees exactly what it saw
 *    before views were tracked.  The records are still dropped quietly
 *    (vmw_binding_res_list_forget()), since they must never outlive the
 *    objects they name.
 */
#ifndef VMW_TRACK_VIEWS
#define VMW_TRACK_VIEWS 1
#endif

enum vmw_view_type {
	vmw_view_sr,
	vmw_view_rt,
	vmw_view_ds,
	vmw_view_ua,
	vmw_view_max,
};

enum vmw_so_type {
	vmw_so_el,
	vmw_so_bs,
	vmw_so_ds,
	vmw_so_rs,
	vmw_so_ss,
	vmw_so_so,
	vmw_so_max,
};

/**
 * union vmw_view_destroy - view destruction command body
 *
 * @rtv: RenderTarget view destruction command body
 * @srv: ShaderResource view destruction command body
 * @dsv: DepthStencil view destruction command body
 * @uav: UnorderedAccess view destruction command body
 * @view_id: A single u32 view id.
 *
 * Every member is a single u32 in the command stream; vmw_so.c checks that
 * at compile time, since the destroy path relies on it.
 */
union vmw_view_destroy {
	SVGA3dCmdDXDestroyRenderTargetView rtv;
	SVGA3dCmdDXDestroyShaderResourceView srv;
	SVGA3dCmdDXDestroyDepthStencilView dsv;
	SVGA3dCmdDXDestroyUAView uav;
	u32 view_id;
};

/* Map enum vmw_view_type to view destroy command ids */
extern const u32 vmw_view_destroy_cmds[];

/* Map enum vmw_view_type to SVGACOTableType */
extern const SVGACOTableType vmw_view_cotables[];

/* Map enum vmw_so_type to SVGACOTableType */
extern const SVGACOTableType vmw_so_cotables[];

/*
 * vmw_view_cmd_to_type - Return the view type for a create or destroy command
 *
 * @id: The SVGA3D command id.
 *
 * For a given view create or destroy command id, return the corresponding
 * enum vmw_view_type. If the command is unknown, return vmw_view_max.
 * The define and destroy commands of the first three view types are
 * numbered in pairs, which is what the arithmetic relies on.
 */
static inline enum vmw_view_type vmw_view_cmd_to_type(u32 id)
{
	u32 tmp = (id - SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW) / 2;

	if (id == SVGA_3D_CMD_DX_DEFINE_UA_VIEW ||
	    id == SVGA_3D_CMD_DX_DESTROY_UA_VIEW)
		return vmw_view_ua;

	if (id == SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2)
		return vmw_view_ds;

	if (tmp > (u32)vmw_view_ds)
		return vmw_view_max;

	return (enum vmw_view_type)tmp;
}

/*
 * vmw_so_cmd_to_type - Return the state object type for a
 * create or destroy command
 *
 * @id: The SVGA3D command id.
 *
 * For a given state object create or destroy command id,
 * return the corresponding enum vmw_so_type. If the command is unknown,
 * return vmw_so_max.
 */
static inline enum vmw_so_type vmw_so_cmd_to_type(u32 id)
{
	switch (id) {
	case SVGA_3D_CMD_DX_DEFINE_ELEMENTLAYOUT:
	case SVGA_3D_CMD_DX_DESTROY_ELEMENTLAYOUT:
		return vmw_so_el;
	case SVGA_3D_CMD_DX_DEFINE_BLEND_STATE:
	case SVGA_3D_CMD_DX_DESTROY_BLEND_STATE:
		return vmw_so_bs;
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_STATE:
	case SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_STATE:
		return vmw_so_ds;
	case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE:
	case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE_V2:
	case SVGA_3D_CMD_DX_DESTROY_RASTERIZER_STATE:
		return vmw_so_rs;
	case SVGA_3D_CMD_DX_DEFINE_SAMPLER_STATE:
	case SVGA_3D_CMD_DX_DESTROY_SAMPLER_STATE:
		return vmw_so_ss;
	case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT:
	case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT_WITH_MOB:
	case SVGA_3D_CMD_DX_DESTROY_STREAMOUTPUT:
		return vmw_so_so;
	default:
		break;
	}
	return vmw_so_max;
}

/*
 * View management - vmw_so.c
 */

/* Stage a view the command stream defines: `cmd' (cmd_size bytes, header
 * included) is kept to re-emit the define, `srf' is referenced, the view is
 * added to `man' under `user_key' and to the staging `list'. */
int vmw_view_add(struct vmw_cmdbuf_res_manager *man, struct vmw_resource *ctx,
		 struct vmw_resource *srf, enum vmw_view_type view_type,
		 u32 user_key, const void *cmd, size_t cmd_size,
		 struct list_head *list);

/* Stage the removal of a view.  On success *res_p is the view whose destroy
 * is now staged (not referenced), or NULL when the view had itself only been
 * staged and was simply dropped. */
int vmw_view_remove(struct vmw_cmdbuf_res_manager *man, u32 user_key,
		    enum vmw_view_type view_type, struct list_head *list,
		    struct vmw_resource **res_p);

/* A surface is being destroyed: destroy every view on its view_list (the
 * device commands go out with each view's own context). */
void vmw_view_surface_list_destroy(struct vmw_device *v,
				   struct list_head *view_list);
/* A context's object table is going away: destroy the views on `list';
 * `readback' as for the table itself. */
void vmw_view_cotable_list_destroy(struct vmw_device *v, struct list_head *list,
				   bool readback);
/* Both of the above: the caller holds binding_lock. */
struct vmw_resource *vmw_view_srf(struct vmw_resource *res);
/* The view under `user_key', or ERR_PTR(-EINVAL). Not referenced. */
struct vmw_resource *vmw_view_lookup(struct vmw_cmdbuf_res_manager *man,
				     enum vmw_view_type view_type, u32 user_key);
u32 vmw_view_dirtying(struct vmw_resource *res);

/* Forget every context binding on an object's binding list without telling
 * the device (marks them scrubbed, then kills them).  For VMW_TRACK_VIEWS 0;
 * the caller holds binding_lock. */
void vmw_binding_res_list_forget(struct list_head *head);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_SO_H */
