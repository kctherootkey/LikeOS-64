// LikeOS -- vmwgfx: per-context binding tracker.
//
// A context's state names other objects: the render targets and depth
// buffer it draws into, the shader-resource views it samples, the shaders,
// constant buffers, vertex and index buffers and stream-output targets it
// uses.  The device keeps those bindings by id and does not care whether
// the object behind an id still exists -- a binding left pointing at a
// destroyed surface or view is a device error the moment a draw uses it,
// and with ids recycled lowest-first it is very often a binding to the
// WRONG object instead.
//
// So every binding a command stream establishes is recorded twice: on the
// context (what it has bound, per slot) and on the bound object (who binds
// it, vmw_resource.binding_head).  Destroying an object then scrubs every
// binding to it from every context first ("scrub": emit the command that
// sets the slot to nothing), and destroying a context kills its records.
// A staged copy of the state is built while a command stream is verified
// and committed to the context only when the stream is submitted, so a
// refused stream leaves the tracked state as it was.
//
// The tracker's state (struct vmw_ctx_binding_state) is private to
// vmw_binding.c; everything else goes through the functions below.
//
// Locking: a context's persistent state and every object's binding_head
// are protected by vmw_device.binding_lock, a sleeping lock taken for
// writing by the caller of every function below that touches them (commit,
// kill, scrub, rebind, reset, res_list_*).  Process context only; the
// scrubs emit device commands, which may sleep.  Never hold the lock
// across drm_gem_put().  A staged state built by one execbuf is private to
// it and needs no lock for vmw_binding_add() / _cb_offset_update() /
// _add_uav_index() / _state_alloc() / _state_free().
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2015 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_BINDING_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_BINDING_H

#include <kernel/dev/gpu/vmwgfx/vmw_compat.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_reg.h>

#define VMW_MAX_VIEW_BINDINGS 128

#define VMW_MAX_UAV_BIND_TYPE 2

struct vmw_device;
struct vmw_resource;
struct vmw_ctx_binding_state;

/*
 * enum vmw_ctx_binding_type - abstract resource to context binding types
 */
enum vmw_ctx_binding_type {
	vmw_ctx_binding_shader,
	vmw_ctx_binding_rt,
	vmw_ctx_binding_tex,
	vmw_ctx_binding_cb,
	vmw_ctx_binding_dx_shader,
	vmw_ctx_binding_dx_rt,
	vmw_ctx_binding_sr,
	vmw_ctx_binding_ds,
	vmw_ctx_binding_so_target,
	vmw_ctx_binding_vb,
	vmw_ctx_binding_ib,
	vmw_ctx_binding_uav,
	vmw_ctx_binding_cs_uav,
	vmw_ctx_binding_so,
	vmw_ctx_binding_max
};

/**
 * struct vmw_ctx_bindinfo - single binding metadata
 *
 * @ctx_list: List head for the context's list of bindings.
 * @res_list: List head for a resource's list of bindings.
 * @ctx: Non-refcounted pointer to the context that owns the binding. NULL
 * indicates no binding present.
 * @res: Non-refcounted pointer to the resource the binding points to. This
 * is typically a surface or a view.
 * @bt: Binding type.
 * @scrubbed: Whether the binding has been scrubbed from the context.
 */
struct vmw_ctx_bindinfo {
	struct list_head ctx_list;
	struct list_head res_list;
	struct vmw_resource *ctx;
	struct vmw_resource *res;
	enum vmw_ctx_binding_type bt;
	bool scrubbed;
};

/**
 * struct vmw_ctx_bindinfo_tex - texture stage binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @texture_stage: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_tex {
	struct vmw_ctx_bindinfo bi;
	uint32 texture_stage;
};

/**
 * struct vmw_ctx_bindinfo_shader - Shader binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @shader_slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_shader {
	struct vmw_ctx_bindinfo bi;
	SVGA3dShaderType shader_slot;
};

/**
 * struct vmw_ctx_bindinfo_cb - Constant buffer binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @shader_slot: Device data used to reconstruct binding command.
 * @offset: Device data used to reconstruct binding command.
 * @size: Device data used to reconstruct binding command.
 * @slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_cb {
	struct vmw_ctx_bindinfo bi;
	SVGA3dShaderType shader_slot;
	uint32 offset;
	uint32 size;
	uint32 slot;
};

/**
 * struct vmw_ctx_bindinfo_view - View binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @shader_slot: Device data used to reconstruct binding command.
 * @slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_view {
	struct vmw_ctx_bindinfo bi;
	SVGA3dShaderType shader_slot;
	uint32 slot;
};

/**
 * struct vmw_ctx_bindinfo_so_target - StreamOutput binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @offset: Device data used to reconstruct binding command.
 * @size: Device data used to reconstruct binding command.
 * @slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_so_target {
	struct vmw_ctx_bindinfo bi;
	uint32 offset;
	uint32 size;
	uint32 slot;
};

/**
 * struct vmw_ctx_bindinfo_vb - Vertex buffer binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @offset: Device data used to reconstruct binding command.
 * @stride: Device data used to reconstruct binding command.
 * @slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_vb {
	struct vmw_ctx_bindinfo bi;
	uint32 offset;
	uint32 stride;
	uint32 slot;
};

/**
 * struct vmw_ctx_bindinfo_ib - Index buffer binding metadata
 *
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @offset: Device data used to reconstruct binding command.
 * @format: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_ib {
	struct vmw_ctx_bindinfo bi;
	uint32 offset;
	uint32 format;
};

/**
 * struct vmw_dx_shader_bindings - per shader type context binding state
 *
 * @shader: The shader binding for this shader type
 * @const_buffer: Const buffer bindings for this shader type.
 * @shader_res: Shader resource view bindings for this shader type.
 * @dirty_sr: Bitmap tracking individual shader resource bindings changes
 * that have not yet been emitted to the device.
 * @dirty: Bitmap tracking per-binding type binding changes that have not
 * yet been emitted to the device.
 */
struct vmw_dx_shader_bindings {
	struct vmw_ctx_bindinfo_shader shader;
	struct vmw_ctx_bindinfo_cb const_buffers[SVGA3D_DX_MAX_CONSTBUFFERS];
	struct vmw_ctx_bindinfo_view shader_res[SVGA3D_DX_MAX_SRVIEWS];
	DECLARE_BITMAP(dirty_sr, SVGA3D_DX_MAX_SRVIEWS);
	unsigned long dirty;
};

/**
 * struct vmw_ctx_bindinfo_uav - UAV context binding state.
 * @views: UAV view bindings.
 * @index: The device splice index set by user-space.
 */
struct vmw_ctx_bindinfo_uav {
	struct vmw_ctx_bindinfo_view views[SVGA3D_DX11_1_MAX_UAVIEWS];
	uint32 index;
};

/**
 * struct vmw_ctx_bindinfo_so - Stream output binding metadata.
 * @bi: struct vmw_ctx_bindinfo we derive from.
 * @slot: Device data used to reconstruct binding command.
 */
struct vmw_ctx_bindinfo_so {
	struct vmw_ctx_bindinfo bi;
	uint32 slot;
};

/* Record binding `ci' (its embedding vmw_ctx_bindinfo_* is copied) in
 * `cbs' at (shader_slot, slot). */
void vmw_binding_add(struct vmw_ctx_binding_state *cbs,
		     const struct vmw_ctx_bindinfo *ci, u32 shader_slot,
		     u32 slot);
void vmw_binding_cb_offset_update(struct vmw_ctx_binding_state *cbs,
				  u32 shader_slot, u32 slot, u32 offsetInBytes);
void vmw_binding_add_uav_index(struct vmw_ctx_binding_state *cbs, uint32 slot,
			       uint32 splice_index);
/* Move the staged bindings `from' into the context's tracker `to'. */
void vmw_binding_state_commit(struct vmw_ctx_binding_state *to,
			      struct vmw_ctx_binding_state *from);
/* An object is going away: forget every binding on its `binding_head'
 * (without telling the device). */
void vmw_binding_res_list_kill(struct list_head *head);
/* An object is going away: emit the commands that unbind it from every
 * context that binds it, and mark those bindings scrubbed. */
void vmw_binding_res_list_scrub(struct list_head *head);
/* Re-emit every scrubbed binding of a context. */
int vmw_binding_rebind_all(struct vmw_ctx_binding_state *cbs);
void vmw_binding_state_kill(struct vmw_ctx_binding_state *cbs);
void vmw_binding_state_scrub(struct vmw_ctx_binding_state *cbs);
/* A fresh, empty tracker; ERR_PTR(-ENOMEM) on failure. */
struct vmw_ctx_binding_state *vmw_binding_state_alloc(struct vmw_device *v);
void vmw_binding_state_free(struct vmw_ctx_binding_state *cbs);
struct list_head *vmw_binding_state_list(struct vmw_ctx_binding_state *cbs);
void vmw_binding_state_reset(struct vmw_ctx_binding_state *cbs);
/* Whether a binding of this type lets the device write the bound object
 * (render target, depth, UAV, stream output): the object's guest memory
 * must then be treated as dirty after the submission. */
u32 vmw_binding_dirtying(enum vmw_ctx_binding_type binding_type);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_BINDING_H */
