// LikeOS -- vmwgfx: per-context binding tracker.
//
// A guest-backed context can be saved to its backing MOB and restored by
// the device at any time, and on restore the device validates every
// binding the context holds against the objects that exist at that moment
// -- outside of any draw call, whenever it likes.  A binding left pointing
// at a destroyed surface, view or shader is therefore an error even if
// nothing ever draws with it again, and with ids recycled lowest-first it
// is very often a binding to the WRONG object instead.
//
// So every binding is tracked twice (see vmw_binding.h): per context, slot
// by slot, and per bound object on its vmw_resource.binding_head.  Before
// an object goes away every binding to it is scrubbed (the device is told
// to bind nothing in that slot) and forgotten; before a context goes away
// all of its bindings are scrubbed and forgotten.
//
// A binding may point at an object from several contexts, so a per-context
// lock cannot protect the two sets of lists.  One device-wide lock,
// vmw_device.binding_lock (a sleeping lock), protects all tracker state that
// is linked into object lists: every function here that walks or changes a
// context's persistent state or an object's binding_head runs with it held
// for writing by the caller.  The staged state an execbuf builds while it
// verifies a command stream is private to that execbuf and links into no
// object list until vmw_binding_state_commit(), so vmw_binding_add(),
// vmw_binding_cb_offset_update() and vmw_binding_add_uav_index() on it need
// no lock.  The binding lock is never held across drm_gem_put() (which can
// re-enter object destruction and take it again); nothing here drops a GEM
// reference.
//
// Commands go through VMW_CMD_RESERVE / VMW_CMD_CTX_RESERVE and
// vmw_cmd_commit(): the legacy context commands carry their context id in
// the body, the DX ones are submitted on the context's own id.  DX
// commands that set a whole array of bindings at once (shader resources,
// render targets, vertex buffers, stream-output targets, UAVs) are not
// emitted per binding: a scrub or rebind only marks the slot dirty, and
// vmw_binding_emit_dirty() then emits one command per dirty array.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2015 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>

/* Scrubbing a shader-resource binding marks its shader stage dirty, and
 * the dirty stages are walked when the commands are emitted.  With 1 every
 * stage the device has is walked (vertex, pixel, geometry, and on SM5
 * contexts hull, domain and compute); with 0 only the first three, and a
 * view bound to a hull, domain or compute stage is then never unbound from
 * it when the view is destroyed -- the device would find the stale id on
 * its next validation of the context. */
#define VMW_BINDING_SCRUB_ALL_STAGES 1

#define VMW_BINDING_RT_BIT     0
#define VMW_BINDING_PS_BIT     1
#define VMW_BINDING_SO_T_BIT   2
#define VMW_BINDING_VB_BIT     3
#define VMW_BINDING_UAV_BIT    4
#define VMW_BINDING_CS_UAV_BIT 5
#define VMW_BINDING_NUM_BITS   6

#define VMW_BINDING_PS_SR_BIT  0

/**
 * struct vmw_ctx_binding_state - per context binding state
 *
 * @dev: The device.
 * @list: linked list of individual active bindings.
 * @render_targets: Render target bindings.
 * @texture_units: Texture units bindings.
 * @ds_view: Depth-stencil view binding.
 * @so_targets: StreamOutput target bindings.
 * @vertex_buffers: Vertex buffer bindings.
 * @index_buffer: Index buffer binding.
 * @per_shader: Per shader-type bindings.
 * @ua_views: UAV bindings.
 * @so_state: StreamOutput bindings.
 * @dirty: Bitmap tracking per binding-type changes that have not yet
 * been emitted to the device.
 * @dirty_vb: Bitmap tracking individual vertex buffer binding changes that
 * have not yet been emitted to the device.
 * @bind_cmd_buffer: Scratch space used to construct binding commands.
 * @bind_cmd_count: Number of binding command data entries in @bind_cmd_buffer
 * @bind_first_slot: Used together with @bind_cmd_buffer to indicate the
 * device binding slot of the first command data entry in @bind_cmd_buffer.
 * @uav_index_set: Bit n set: ua_views[n].index was set by
 * vmw_binding_add_uav_index() since the state was last reset or committed.
 * A staged state is reused across submissions and contexts, so only an
 * index the current submission actually set may be transferred.
 *
 * Note that this structure also provides storage space for the individual
 * struct vmw_ctx_binding objects, so that no dynamic allocation is needed
 * for individual bindings.  It is tens of kilobytes, so it always lives on
 * the heap, never on a kernel stack.
 */
struct vmw_ctx_binding_state {
	struct vmw_device *dev;
	struct list_head list;
	struct vmw_ctx_bindinfo_view render_targets[SVGA3D_RT_MAX];
	struct vmw_ctx_bindinfo_tex texture_units[SVGA3D_NUM_TEXTURE_UNITS];
	struct vmw_ctx_bindinfo_view ds_view;
	struct vmw_ctx_bindinfo_so_target so_targets[SVGA3D_DX_MAX_SOTARGETS];
	struct vmw_ctx_bindinfo_vb vertex_buffers[SVGA3D_DX_MAX_VERTEXBUFFERS];
	struct vmw_ctx_bindinfo_ib index_buffer;
	struct vmw_dx_shader_bindings per_shader[SVGA3D_NUM_SHADERTYPE];
	struct vmw_ctx_bindinfo_uav ua_views[VMW_MAX_UAV_BIND_TYPE];
	struct vmw_ctx_bindinfo_so so_state;

	unsigned long dirty;
	DECLARE_BITMAP(dirty_vb, SVGA3D_DX_MAX_VERTEXBUFFERS);

	u32 bind_cmd_buffer[VMW_MAX_VIEW_BINDINGS];
	u32 bind_cmd_count;
	u32 bind_first_slot;
	u32 uav_index_set;
};

static int vmw_binding_scrub_shader(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_render_target(struct vmw_ctx_bindinfo *bi,
					   bool rebind);
static int vmw_binding_scrub_texture(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_cb(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_dx_rt(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_sr(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_so_target(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_emit_dirty(struct vmw_ctx_binding_state *cbs);
static int vmw_binding_scrub_dx_shader(struct vmw_ctx_bindinfo *bi,
				       bool rebind);
static int vmw_binding_scrub_ib(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_vb(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_uav(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_cs_uav(struct vmw_ctx_bindinfo *bi, bool rebind);
static int vmw_binding_scrub_so(struct vmw_ctx_bindinfo *bi, bool rebind);

typedef int (*vmw_scrub_func)(struct vmw_ctx_bindinfo *, bool);

/**
 * struct vmw_binding_info - Per binding type information for the binding
 * manager
 *
 * @size: The size of the struct binding derived from a struct vmw_ctx_bindinfo.
 * @offsets: array[shader_slot] of offsets to the array[slot]
 * of struct bindings for the binding type.
 * @scrub_func: Pointer to the scrub function for this binding type.
 *
 * Holds static information to help optimize the binding manager and avoid
 * an excessive amount of switch statements.
 */
struct vmw_binding_info {
	size_t size;
	const size_t *offsets;
	vmw_scrub_func scrub_func;
};

/*
 * A number of static variables that help determine the scrub func and the
 * location of the struct vmw_ctx_bindinfo slots for each binding type.
 */
static const size_t vmw_binding_shader_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, per_shader[0].shader),
	offsetof(struct vmw_ctx_binding_state, per_shader[1].shader),
	offsetof(struct vmw_ctx_binding_state, per_shader[2].shader),
	offsetof(struct vmw_ctx_binding_state, per_shader[3].shader),
	offsetof(struct vmw_ctx_binding_state, per_shader[4].shader),
	offsetof(struct vmw_ctx_binding_state, per_shader[5].shader),
};
static const size_t vmw_binding_rt_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, render_targets),
};
static const size_t vmw_binding_tex_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, texture_units),
};
static const size_t vmw_binding_cb_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, per_shader[0].const_buffers),
	offsetof(struct vmw_ctx_binding_state, per_shader[1].const_buffers),
	offsetof(struct vmw_ctx_binding_state, per_shader[2].const_buffers),
	offsetof(struct vmw_ctx_binding_state, per_shader[3].const_buffers),
	offsetof(struct vmw_ctx_binding_state, per_shader[4].const_buffers),
	offsetof(struct vmw_ctx_binding_state, per_shader[5].const_buffers),
};
static const size_t vmw_binding_dx_ds_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, ds_view),
};
static const size_t vmw_binding_sr_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, per_shader[0].shader_res),
	offsetof(struct vmw_ctx_binding_state, per_shader[1].shader_res),
	offsetof(struct vmw_ctx_binding_state, per_shader[2].shader_res),
	offsetof(struct vmw_ctx_binding_state, per_shader[3].shader_res),
	offsetof(struct vmw_ctx_binding_state, per_shader[4].shader_res),
	offsetof(struct vmw_ctx_binding_state, per_shader[5].shader_res),
};
static const size_t vmw_binding_so_target_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, so_targets),
};
static const size_t vmw_binding_vb_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, vertex_buffers),
};
static const size_t vmw_binding_ib_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, index_buffer),
};
static const size_t vmw_binding_uav_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, ua_views[0].views),
};
static const size_t vmw_binding_cs_uav_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, ua_views[1].views),
};
static const size_t vmw_binding_so_offsets[] = {
	offsetof(struct vmw_ctx_binding_state, so_state),
};

static const struct vmw_binding_info vmw_binding_infos[] = {
	[vmw_ctx_binding_shader] = {
		.size = sizeof(struct vmw_ctx_bindinfo_shader),
		.offsets = vmw_binding_shader_offsets,
		.scrub_func = vmw_binding_scrub_shader},
	[vmw_ctx_binding_rt] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_rt_offsets,
		.scrub_func = vmw_binding_scrub_render_target},
	[vmw_ctx_binding_tex] = {
		.size = sizeof(struct vmw_ctx_bindinfo_tex),
		.offsets = vmw_binding_tex_offsets,
		.scrub_func = vmw_binding_scrub_texture},
	[vmw_ctx_binding_cb] = {
		.size = sizeof(struct vmw_ctx_bindinfo_cb),
		.offsets = vmw_binding_cb_offsets,
		.scrub_func = vmw_binding_scrub_cb},
	[vmw_ctx_binding_dx_shader] = {
		.size = sizeof(struct vmw_ctx_bindinfo_shader),
		.offsets = vmw_binding_shader_offsets,
		.scrub_func = vmw_binding_scrub_dx_shader},
	[vmw_ctx_binding_dx_rt] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_rt_offsets,
		.scrub_func = vmw_binding_scrub_dx_rt},
	[vmw_ctx_binding_sr] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_sr_offsets,
		.scrub_func = vmw_binding_scrub_sr},
	[vmw_ctx_binding_ds] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_dx_ds_offsets,
		.scrub_func = vmw_binding_scrub_dx_rt},
	[vmw_ctx_binding_so_target] = {
		.size = sizeof(struct vmw_ctx_bindinfo_so_target),
		.offsets = vmw_binding_so_target_offsets,
		.scrub_func = vmw_binding_scrub_so_target},
	[vmw_ctx_binding_vb] = {
		.size = sizeof(struct vmw_ctx_bindinfo_vb),
		.offsets = vmw_binding_vb_offsets,
		.scrub_func = vmw_binding_scrub_vb},
	[vmw_ctx_binding_ib] = {
		.size = sizeof(struct vmw_ctx_bindinfo_ib),
		.offsets = vmw_binding_ib_offsets,
		.scrub_func = vmw_binding_scrub_ib},
	[vmw_ctx_binding_uav] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_uav_offsets,
		.scrub_func = vmw_binding_scrub_uav},
	[vmw_ctx_binding_cs_uav] = {
		.size = sizeof(struct vmw_ctx_bindinfo_view),
		.offsets = vmw_binding_cs_uav_offsets,
		.scrub_func = vmw_binding_scrub_cs_uav},
	[vmw_ctx_binding_so] = {
		.size = sizeof(struct vmw_ctx_bindinfo_so),
		.offsets = vmw_binding_so_offsets,
		.scrub_func = vmw_binding_scrub_so},
};

/* Every binding type has an entry, and the per-shader offset tables cover
 * every shader stage the binding state has storage for. */
BUILD_BUG_ON(ARRAY_SIZE(vmw_binding_infos) != vmw_ctx_binding_max);
BUILD_BUG_ON(ARRAY_SIZE(vmw_binding_shader_offsets) != SVGA3D_NUM_SHADERTYPE);
BUILD_BUG_ON(ARRAY_SIZE(vmw_binding_cb_offsets) != SVGA3D_NUM_SHADERTYPE);
BUILD_BUG_ON(ARRAY_SIZE(vmw_binding_sr_offsets) != SVGA3D_NUM_SHADERTYPE);

/*
 * Assumptions the code below is written on.
 */
BUILD_BUG_ON(SVGA3D_NUM_SHADERTYPE_DX10 != 3);
BUILD_BUG_ON(SVGA3D_DX_MAX_RENDER_TARGETS > SVGA3D_RT_MAX);
BUILD_BUG_ON(sizeof(uint32) != sizeof(u32));
BUILD_BUG_ON(VMW_MAX_UAV_BIND_TYPE != 2);

/*
 * struct vmw_ctx_binding_state::bind_cmd_buffer is used for various
 * view id arrays.
 */
BUILD_BUG_ON(VMW_MAX_VIEW_BINDINGS < SVGA3D_RT_MAX);
BUILD_BUG_ON(VMW_MAX_VIEW_BINDINGS < SVGA3D_DX_MAX_SRVIEWS);
BUILD_BUG_ON(VMW_MAX_VIEW_BINDINGS < SVGA3D_DX_MAX_CONSTBUFFERS);
BUILD_BUG_ON(VMW_MAX_VIEW_BINDINGS < SVGA3D_DX11_1_MAX_UAVIEWS);

/*
 * struct vmw_ctx_binding_state::bind_cmd_buffer is used for
 * u32 view ids, SVGA3dSoTargets and SVGA3dVertexBuffers
 */
BUILD_BUG_ON(SVGA3D_DX_MAX_SOTARGETS * sizeof(SVGA3dSoTarget) >
	     VMW_MAX_VIEW_BINDINGS * sizeof(u32));
BUILD_BUG_ON(SVGA3D_DX_MAX_VERTEXBUFFERS * sizeof(SVGA3dVertexBuffer) >
	     VMW_MAX_VIEW_BINDINGS * sizeof(u32));

/* The device id of the object a binding points at.  Objects keep their id
 * as an int with -1 for "none"; the device spells that SVGA3D_INVALID_ID,
 * which is the same bit pattern. */
static inline uint32 vmw_bi_res_id(const struct vmw_ctx_bindinfo *bi)
{
	return (uint32)bi->res->id;
}

/* Clear `nbits' bits of `map' from `start' on. */
static void vmw_binding_bitmap_clear(unsigned long *map, unsigned long start,
				     unsigned long nbits)
{
	unsigned long i;

	for (i = start; i < start + nbits; ++i)
		__clear_bit(i, map);
}

/**
 * vmw_cbs_context - Return a pointer to the context resource of a
 * context binding state tracker.
 *
 * @cbs: The context binding state tracker.
 *
 * Provided there are any active bindings, this function will return an
 * unreferenced pointer to the context resource that owns the context
 * binding state tracker. If there are no active bindings, this function
 * will return NULL. Note that the caller must somehow ensure that a reference
 * is held on the context resource prior to calling this function.
 */
static const struct vmw_resource *
vmw_cbs_context(const struct vmw_ctx_binding_state *cbs)
{
	if (list_empty(&cbs->list))
		return NULL;

	return list_first_entry(&cbs->list, struct vmw_ctx_bindinfo,
				ctx_list)->ctx;
}

/**
 * vmw_binding_loc - determine the struct vmw_ctx_bindinfo slot location.
 *
 * @cbs: Pointer to a struct vmw_ctx_binding state which holds the slot.
 * @bt: The binding type.
 * @shader_slot: The shader slot of the binding. If none, then set to 0.
 * @slot: The slot of the binding.
 */
static struct vmw_ctx_bindinfo *
vmw_binding_loc(struct vmw_ctx_binding_state *cbs,
		enum vmw_ctx_binding_type bt, u32 shader_slot, u32 slot)
{
	const struct vmw_binding_info *b = &vmw_binding_infos[bt];
	size_t offset = b->offsets[shader_slot] + b->size*slot;

	return (struct vmw_ctx_bindinfo *)((u8 *) cbs + offset);
}

/**
 * vmw_binding_drop: Stop tracking a context binding
 *
 * @bi: Pointer to binding tracker storage.
 *
 * Stops tracking a context binding, and re-initializes its storage.
 * Typically used when the context binding is replaced with a binding to
 * another (or the same, for that matter) resource.
 */
static void vmw_binding_drop(struct vmw_ctx_bindinfo *bi)
{
	list_del(&bi->ctx_list);
	if (!list_empty(&bi->res_list))
		list_del(&bi->res_list);
	bi->ctx = NULL;
}

/**
 * vmw_binding_add: Start tracking a context binding
 *
 * @cbs: Pointer to the context binding state tracker.
 * @bi: Information about the binding to track.
 * @shader_slot: The shader slot of the binding.
 * @slot: The slot of the binding.
 *
 * Starts tracking the binding in the context binding
 * state structure @cbs.
 */
void vmw_binding_add(struct vmw_ctx_binding_state *cbs,
		     const struct vmw_ctx_bindinfo *bi,
		     u32 shader_slot, u32 slot)
{
	struct vmw_ctx_bindinfo *loc =
		vmw_binding_loc(cbs, bi->bt, shader_slot, slot);
	const struct vmw_binding_info *b = &vmw_binding_infos[bi->bt];

	if (loc->ctx != NULL)
		vmw_binding_drop(loc);

	mm_memcpy(loc, bi, b->size);
	loc->scrubbed = false;
	list_add(&loc->ctx_list, &cbs->list);
	INIT_LIST_HEAD(&loc->res_list);
}

/**
 * vmw_binding_cb_offset_update: Update the offset of a cb binding
 *
 * @cbs: Pointer to the context binding state tracker.
 * @shader_slot: The shader slot of the binding.
 * @slot: The slot of the binding.
 * @offsetInBytes: The new offset of the binding.
 *
 * Updates the offset of an existing cb binding in the context binding
 * state structure @cbs.
 */
void vmw_binding_cb_offset_update(struct vmw_ctx_binding_state *cbs,
				  u32 shader_slot, u32 slot, u32 offsetInBytes)
{
	struct vmw_ctx_bindinfo *loc =
		vmw_binding_loc(cbs, vmw_ctx_binding_cb, shader_slot, slot);
	struct vmw_ctx_bindinfo_cb *loc_cb =
		(struct vmw_ctx_bindinfo_cb *)((u8 *) loc);
	loc_cb->offset = offsetInBytes;
}

/**
 * vmw_binding_add_uav_index - Add UAV index for tracking.
 * @cbs: Pointer to the context binding state tracker.
 * @slot: UAV type to which bind this index.
 * @splice_index: The splice index to track.
 */
void vmw_binding_add_uav_index(struct vmw_ctx_binding_state *cbs, uint32 slot,
			       uint32 splice_index)
{
	cbs->ua_views[slot].index = splice_index;
	cbs->uav_index_set |= 1u << slot;
}

/**
 * vmw_binding_transfer: Transfer a context binding tracking entry.
 *
 * @cbs: Pointer to the persistent context binding state tracker.
 * @from: Staged binding info built during execbuf
 * @bi: Information about the binding to track.
 *
 */
static void vmw_binding_transfer(struct vmw_ctx_binding_state *cbs,
				 const struct vmw_ctx_binding_state *from,
				 const struct vmw_ctx_bindinfo *bi)
{
	size_t offset = (unsigned long)bi - (unsigned long)from;
	struct vmw_ctx_bindinfo *loc = (struct vmw_ctx_bindinfo *)
		((unsigned long) cbs + offset);

	if (loc->ctx != NULL) {
		WARN_ON(bi->scrubbed);

		vmw_binding_drop(loc);
	}

	if (bi->res != NULL) {
		mm_memcpy(loc, bi, vmw_binding_infos[bi->bt].size);
		list_add_tail(&loc->ctx_list, &cbs->list);
		list_add_tail(&loc->res_list, &loc->res->binding_head);
	}
}

/**
 * vmw_binding_state_kill - Kill all bindings associated with a
 * struct vmw_ctx_binding state structure, and re-initialize the structure.
 *
 * @cbs: Pointer to the context binding state tracker.
 *
 * Emits commands to scrub all bindings associated with the
 * context binding state tracker. Then re-initializes the whole structure.
 */
void vmw_binding_state_kill(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_ctx_bindinfo *entry, *next;

	vmw_binding_state_scrub(cbs);
	list_for_each_entry_safe(entry, next, &cbs->list, ctx_list)
		vmw_binding_drop(entry);
}

/**
 * vmw_binding_state_scrub - Scrub all bindings associated with a
 * struct vmw_ctx_binding state structure.
 *
 * @cbs: Pointer to the context binding state tracker.
 *
 * Emits commands to scrub all bindings associated with the
 * context binding state tracker.
 */
void vmw_binding_state_scrub(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_ctx_bindinfo *entry;

	list_for_each_entry(entry, &cbs->list, ctx_list) {
		if (!entry->scrubbed) {
			(void) vmw_binding_infos[entry->bt].scrub_func
				(entry, false);
			entry->scrubbed = true;
		}
	}

	(void) vmw_binding_emit_dirty(cbs);
}

/**
 * vmw_binding_res_list_kill - Kill all bindings on a
 * resource binding list
 *
 * @head: list head of resource binding list
 *
 * Kills all bindings associated with a specific resource. Typically
 * called before the resource is destroyed.
 */
void vmw_binding_res_list_kill(struct list_head *head)
{
	struct vmw_ctx_bindinfo *entry, *next;

	vmw_binding_res_list_scrub(head);
	list_for_each_entry_safe(entry, next, head, res_list)
		vmw_binding_drop(entry);
}

/**
 * vmw_binding_res_list_scrub - Scrub all bindings on a
 * resource binding list
 *
 * @head: list head of resource binding list
 *
 * Scrub all bindings associated with a specific resource.  The scrub
 * commands are emitted before the resource itself is destroyed, so the
 * device never sees a context binding name an id that no longer exists.
 */
void vmw_binding_res_list_scrub(struct list_head *head)
{
	struct vmw_ctx_bindinfo *entry;

	list_for_each_entry(entry, head, res_list) {
		if (!entry->scrubbed) {
			(void) vmw_binding_infos[entry->bt].scrub_func
				(entry, false);
			entry->scrubbed = true;
		}
	}

	list_for_each_entry(entry, head, res_list) {
		struct vmw_ctx_binding_state *cbs =
			vmw_context_binding_state(entry->ctx);

		(void) vmw_binding_emit_dirty(cbs);
	}
}


/**
 * vmw_binding_state_commit - Commit staged binding info
 *
 * @to:   Staged binding info area to copy into to.
 * @from: Staged binding info built during execbuf.
 *
 * Transfers binding info from a temporary structure
 * (typically used by execbuf) to the persistent
 * structure in the context. This can be done once commands have been
 * submitted to hardware
 */
void vmw_binding_state_commit(struct vmw_ctx_binding_state *to,
			      struct vmw_ctx_binding_state *from)
{
	struct vmw_ctx_bindinfo *entry, *next;
	u32 i;

	list_for_each_entry_safe(entry, next, &from->list, ctx_list) {
		vmw_binding_transfer(to, from, entry);
		vmw_binding_drop(entry);
	}

	/* Also transfer the uav splice indices this submission set; an index
	 * it did not set leaves the context's own value alone. */
	for (i = 0; i < VMW_MAX_UAV_BIND_TYPE; ++i) {
		if (from->uav_index_set & (1u << i))
			to->ua_views[i].index = from->ua_views[i].index;
	}
	from->uav_index_set = 0;
}

/**
 * vmw_binding_rebind_all - Rebind all scrubbed bindings of a context
 *
 * @cbs: Pointer to the context binding state tracker.
 *
 * Walks through the context binding list and rebinds all scrubbed
 * resources.
 */
int vmw_binding_rebind_all(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_ctx_bindinfo *entry;
	int ret;

	list_for_each_entry(entry, &cbs->list, ctx_list) {
		if (likely(!entry->scrubbed))
			continue;

		if ((entry->res == NULL || vmw_bi_res_id(entry) ==
			    SVGA3D_INVALID_ID))
			continue;

		ret = vmw_binding_infos[entry->bt].scrub_func(entry, true);
		if (unlikely(ret != 0))
			return ret;

		entry->scrubbed = false;
	}

	return vmw_binding_emit_dirty(cbs);
}

/**
 * vmw_binding_scrub_shader - scrub a shader binding from a context.
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_shader(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_shader *binding =
		container_of(bi, struct vmw_ctx_bindinfo_shader, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdSetShader body;
	} *cmd;

	cmd = VMW_CMD_RESERVE(v, sizeof(*cmd));
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_SET_SHADER;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.cid = (uint32)bi->ctx->id;
	cmd->body.type = binding->shader_slot + SVGA3D_SHADERTYPE_MIN;
	cmd->body.shid = ((rebind) ? vmw_bi_res_id(bi) : SVGA3D_INVALID_ID);
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_scrub_render_target - scrub a render target binding
 * from a context.
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_render_target(struct vmw_ctx_bindinfo *bi,
					   bool rebind)
{
	struct vmw_ctx_bindinfo_view *binding =
		container_of(bi, struct vmw_ctx_bindinfo_view, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdSetRenderTarget body;
	} *cmd;

	cmd = VMW_CMD_RESERVE(v, sizeof(*cmd));
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_SETRENDERTARGET;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.cid = (uint32)bi->ctx->id;
	cmd->body.type = binding->slot;
	cmd->body.target.sid = ((rebind) ? vmw_bi_res_id(bi) : SVGA3D_INVALID_ID);
	cmd->body.target.face = 0;
	cmd->body.target.mipmap = 0;
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_scrub_texture - scrub a texture binding from a context.
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 *
 * One command per texture stage; a context rarely has more than a few
 * textures bound to the object being destroyed.
 */
static int vmw_binding_scrub_texture(struct vmw_ctx_bindinfo *bi,
				     bool rebind)
{
	struct vmw_ctx_bindinfo_tex *binding =
		container_of(bi, struct vmw_ctx_bindinfo_tex, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		struct {
			SVGA3dCmdSetTextureState c;
			SVGA3dTextureState s1;
		} body;
	} *cmd;

	cmd = VMW_CMD_RESERVE(v, sizeof(*cmd));
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_SETTEXTURESTATE;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.c.cid = (uint32)bi->ctx->id;
	cmd->body.s1.stage = binding->texture_stage;
	cmd->body.s1.name = SVGA3D_TS_BIND_TEXTURE;
	cmd->body.s1.value = ((rebind) ? vmw_bi_res_id(bi) : SVGA3D_INVALID_ID);
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_scrub_dx_shader - scrub a dx shader binding from a context.
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_dx_shader(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_shader *binding =
		container_of(bi, struct vmw_ctx_bindinfo_shader, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetShader body;
	} *cmd;

	cmd = VMW_CMD_CTX_RESERVE(v, sizeof(*cmd), bi->ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_SHADER;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.type = binding->shader_slot + SVGA3D_SHADERTYPE_MIN;
	cmd->body.shaderId = ((rebind) ? vmw_bi_res_id(bi) : SVGA3D_INVALID_ID);
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_scrub_cb - scrub a constant buffer binding from a context.
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 *
 * The rebind carries the binding's current offset, which also tracks the
 * offset-only updates a command stream made after the bind
 * (vmw_binding_cb_offset_update()), so one SET_SINGLE_CONSTANT_BUFFER
 * restores the slot completely.
 */
static int vmw_binding_scrub_cb(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_cb *binding =
		container_of(bi, struct vmw_ctx_bindinfo_cb, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetSingleConstantBuffer body;
	} *cmd;

	cmd = VMW_CMD_CTX_RESERVE(v, sizeof(*cmd), bi->ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.slot = binding->slot;
	cmd->body.type = binding->shader_slot + SVGA3D_SHADERTYPE_MIN;
	if (rebind) {
		cmd->body.offsetInBytes = binding->offset;
		cmd->body.sizeInBytes = binding->size;
		cmd->body.sid = vmw_bi_res_id(bi);
	} else {
		cmd->body.offsetInBytes = 0;
		cmd->body.sizeInBytes = 0;
		cmd->body.sid = SVGA3D_INVALID_ID;
	}
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_collect_view_ids - Build view id data for a view binding command
 * without checking which bindings actually need to be emitted
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 * @biv: Pointer to where the binding info array is stored in @cbs
 * @max_num: Maximum number of entries in the @bi array.
 *
 * Scans the @bi array for bindings and builds a buffer of view id data.
 * Stops at the first non-existing binding in the @bi array.
 * On output, @cbs->bind_cmd_count contains the number of bindings to be
 * emitted, @cbs->bind_first_slot is set to zero, and @cbs->bind_cmd_buffer
 * contains the command data.
 */
static void vmw_collect_view_ids(struct vmw_ctx_binding_state *cbs,
				 const struct vmw_ctx_bindinfo_view *biv,
				 u32 max_num)
{
	unsigned long i;

	cbs->bind_cmd_count = 0;
	cbs->bind_first_slot = 0;

	for (i = 0; i < max_num; ++i, ++biv) {
		if (!biv->bi.ctx)
			break;

		cbs->bind_cmd_buffer[cbs->bind_cmd_count++] =
			((biv->bi.scrubbed) ?
			 SVGA3D_INVALID_ID : vmw_bi_res_id(&biv->bi));
	}
}

/**
 * vmw_collect_dirty_view_ids - Build view id data for a view binding command
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 * @bi: Pointer to where the binding info array is stored in @cbs
 * @dirty: Bitmap indicating which bindings need to be emitted.
 * @max_num: Maximum number of entries in the @bi array.
 *
 * Scans the @bi array for bindings that need to be emitted and
 * builds a buffer of view id data.
 * On output, @cbs->bind_cmd_count contains the number of bindings to be
 * emitted, @cbs->bind_first_slot indicates the index of the first emitted
 * binding, and @cbs->bind_cmd_buffer contains the command data.
 */
static void vmw_collect_dirty_view_ids(struct vmw_ctx_binding_state *cbs,
				       const struct vmw_ctx_bindinfo *bi,
				       unsigned long *dirty,
				       u32 max_num)
{
	const struct vmw_ctx_bindinfo_view *biv =
		container_of(bi, struct vmw_ctx_bindinfo_view, bi);
	unsigned long i, next_bit;

	cbs->bind_cmd_count = 0;
	i = find_first_bit(dirty, max_num);
	next_bit = i;
	cbs->bind_first_slot = i;

	biv += i;
	for (; i < max_num; ++i, ++biv) {
		cbs->bind_cmd_buffer[cbs->bind_cmd_count++] =
			((!biv->bi.ctx || biv->bi.scrubbed) ?
			 SVGA3D_INVALID_ID : vmw_bi_res_id(&biv->bi));

		if (next_bit == i) {
			next_bit = find_next_bit(dirty, max_num, i + 1);
			if (next_bit >= max_num)
				break;
		}
	}
}

/**
 * vmw_emit_set_sr - Issue delayed DX shader resource binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 * @shader_slot: The shader slot of the binding.
 */
static int vmw_emit_set_sr(struct vmw_ctx_binding_state *cbs,
			   int shader_slot)
{
	const struct vmw_ctx_bindinfo *loc =
		&cbs->per_shader[shader_slot].shader_res[0].bi;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetShaderResources body;
	} *cmd;
	size_t cmd_size, view_id_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_dirty_view_ids(cbs, loc,
				   cbs->per_shader[shader_slot].dirty_sr,
				   SVGA3D_DX_MAX_SRVIEWS);
	if (cbs->bind_cmd_count == 0)
		return 0;

	view_id_size = cbs->bind_cmd_count*sizeof(uint32);
	cmd_size = sizeof(*cmd) + view_id_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_SHADER_RESOURCES;
	cmd->header.size = sizeof(cmd->body) + view_id_size;
	cmd->body.type = shader_slot + SVGA3D_SHADERTYPE_MIN;
	cmd->body.startView = cbs->bind_first_slot;

	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, view_id_size);

	vmw_cmd_commit(ctx->dev, cmd_size);
	vmw_binding_bitmap_clear(cbs->per_shader[shader_slot].dirty_sr,
				 cbs->bind_first_slot, cbs->bind_cmd_count);

	return 0;
}

/**
 * vmw_emit_set_rt - Issue delayed DX rendertarget binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 */
static int vmw_emit_set_rt(struct vmw_ctx_binding_state *cbs)
{
	const struct vmw_ctx_bindinfo_view *loc = &cbs->render_targets[0];
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetRenderTargets body;
	} *cmd;
	size_t cmd_size, view_id_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_view_ids(cbs, loc, SVGA3D_DX_MAX_RENDER_TARGETS);
	view_id_size = cbs->bind_cmd_count*sizeof(uint32);
	cmd_size = sizeof(*cmd) + view_id_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_RENDERTARGETS;
	cmd->header.size = sizeof(cmd->body) + view_id_size;

	if (cbs->ds_view.bi.ctx && !cbs->ds_view.bi.scrubbed)
		cmd->body.depthStencilViewId = vmw_bi_res_id(&cbs->ds_view.bi);
	else
		cmd->body.depthStencilViewId = SVGA3D_INVALID_ID;

	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, view_id_size);

	vmw_cmd_commit(ctx->dev, cmd_size);

	return 0;

}

/**
 * vmw_collect_so_targets - Build SVGA3dSoTarget data for a binding command
 * without checking which bindings actually need to be emitted
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 * @biso: Pointer to where the binding info array is stored in @cbs
 * @max_num: Maximum number of entries in the @bi array.
 *
 * Scans the @bi array for bindings and builds a buffer of SVGA3dSoTarget data.
 * Stops at the first non-existing binding in the @bi array.
 * On output, @cbs->bind_cmd_count contains the number of bindings to be
 * emitted, @cbs->bind_first_slot is set to zero, and @cbs->bind_cmd_buffer
 * contains the command data.
 */
static void vmw_collect_so_targets(struct vmw_ctx_binding_state *cbs,
				   const struct vmw_ctx_bindinfo_so_target *biso,
				   u32 max_num)
{
	unsigned long i;
	SVGA3dSoTarget *so_buffer = (SVGA3dSoTarget *) cbs->bind_cmd_buffer;

	cbs->bind_cmd_count = 0;
	cbs->bind_first_slot = 0;

	for (i = 0; i < max_num; ++i, ++biso, ++so_buffer,
		    ++cbs->bind_cmd_count) {
		if (!biso->bi.ctx)
			break;

		if (!biso->bi.scrubbed) {
			so_buffer->sid = vmw_bi_res_id(&biso->bi);
			so_buffer->offset = biso->offset;
			so_buffer->sizeInBytes = biso->size;
		} else {
			so_buffer->sid = SVGA3D_INVALID_ID;
			so_buffer->offset = 0;
			so_buffer->sizeInBytes = 0;
		}
	}
}

/**
 * vmw_emit_set_so_target - Issue delayed streamout binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 */
static int vmw_emit_set_so_target(struct vmw_ctx_binding_state *cbs)
{
	const struct vmw_ctx_bindinfo_so_target *loc = &cbs->so_targets[0];
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetSOTargets body;
	} *cmd;
	size_t cmd_size, so_target_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_so_targets(cbs, loc, SVGA3D_DX_MAX_SOTARGETS);
	if (cbs->bind_cmd_count == 0)
		return 0;

	so_target_size = cbs->bind_cmd_count*sizeof(SVGA3dSoTarget);
	cmd_size = sizeof(*cmd) + so_target_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_SOTARGETS;
	cmd->header.size = sizeof(cmd->body) + so_target_size;
	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, so_target_size);

	vmw_cmd_commit(ctx->dev, cmd_size);

	return 0;

}

/**
 * vmw_binding_emit_dirty_ps - Issue delayed per shader binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 *
 */
static int vmw_binding_emit_dirty_ps(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_dx_shader_bindings *sb = &cbs->per_shader[0];
#if VMW_BINDING_SCRUB_ALL_STAGES
	const u32 num_stages = SVGA3D_NUM_SHADERTYPE;
#else
	const u32 num_stages = SVGA3D_NUM_SHADERTYPE_DX10;
#endif
	u32 i;
	int ret;

	for (i = 0; i < num_stages; ++i, ++sb) {
		if (!test_bit(VMW_BINDING_PS_SR_BIT, &sb->dirty))
			continue;

		ret = vmw_emit_set_sr(cbs, i);
		if (ret)
			break;

		__clear_bit(VMW_BINDING_PS_SR_BIT, &sb->dirty);
	}

	return 0;
}

/**
 * vmw_collect_dirty_vbs - Build SVGA3dVertexBuffer data for a
 * SVGA3dCmdDXSetVertexBuffers command
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 * @bi: Pointer to where the binding info array is stored in @cbs
 * @dirty: Bitmap indicating which bindings need to be emitted.
 * @max_num: Maximum number of entries in the @bi array.
 *
 * Scans the @bi array for bindings that need to be emitted and
 * builds a buffer of SVGA3dVertexBuffer data.
 * On output, @cbs->bind_cmd_count contains the number of bindings to be
 * emitted, @cbs->bind_first_slot indicates the index of the first emitted
 * binding, and @cbs->bind_cmd_buffer contains the command data.
 */
static void vmw_collect_dirty_vbs(struct vmw_ctx_binding_state *cbs,
				  const struct vmw_ctx_bindinfo *bi,
				  unsigned long *dirty,
				  u32 max_num)
{
	const struct vmw_ctx_bindinfo_vb *biv =
		container_of(bi, struct vmw_ctx_bindinfo_vb, bi);
	unsigned long i, next_bit;
	SVGA3dVertexBuffer *vbs = (SVGA3dVertexBuffer *) &cbs->bind_cmd_buffer;

	cbs->bind_cmd_count = 0;
	i = find_first_bit(dirty, max_num);
	next_bit = i;
	cbs->bind_first_slot = i;

	biv += i;
	for (; i < max_num; ++i, ++biv, ++vbs) {
		if (!biv->bi.ctx || biv->bi.scrubbed) {
			vbs->sid = SVGA3D_INVALID_ID;
			vbs->stride = 0;
			vbs->offset = 0;
		} else {
			vbs->sid = vmw_bi_res_id(&biv->bi);
			vbs->stride = biv->stride;
			vbs->offset = biv->offset;
		}
		cbs->bind_cmd_count++;
		if (next_bit == i) {
			next_bit = find_next_bit(dirty, max_num, i + 1);
			if (next_bit >= max_num)
				break;
		}
	}
}

/**
 * vmw_emit_set_vb - Issue delayed vertex buffer binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 *
 */
static int vmw_emit_set_vb(struct vmw_ctx_binding_state *cbs)
{
	const struct vmw_ctx_bindinfo *loc =
		&cbs->vertex_buffers[0].bi;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetVertexBuffers body;
	} *cmd;
	size_t cmd_size, set_vb_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_dirty_vbs(cbs, loc, cbs->dirty_vb,
			     SVGA3D_DX_MAX_VERTEXBUFFERS);
	if (cbs->bind_cmd_count == 0)
		return 0;

	set_vb_size = cbs->bind_cmd_count*sizeof(SVGA3dVertexBuffer);
	cmd_size = sizeof(*cmd) + set_vb_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS;
	cmd->header.size = sizeof(cmd->body) + set_vb_size;
	cmd->body.startBuffer = cbs->bind_first_slot;

	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, set_vb_size);

	vmw_cmd_commit(ctx->dev, cmd_size);
	vmw_binding_bitmap_clear(cbs->dirty_vb,
				 cbs->bind_first_slot, cbs->bind_cmd_count);

	return 0;
}

/**
 * vmw_emit_set_uav - Issue delayed UAV binding commands for the graphics
 * pipeline
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 */
static int vmw_emit_set_uav(struct vmw_ctx_binding_state *cbs)
{
	const struct vmw_ctx_bindinfo_view *loc = &cbs->ua_views[0].views[0];
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetUAViews body;
	} *cmd;
	size_t cmd_size, view_id_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_view_ids(cbs, loc, vmw_max_num_uavs(cbs->dev));
	view_id_size = cbs->bind_cmd_count*sizeof(uint32);
	cmd_size = sizeof(*cmd) + view_id_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (!cmd)
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_UA_VIEWS;
	cmd->header.size = sizeof(cmd->body) + view_id_size;

	/* Splice index is specified user-space   */
	cmd->body.uavSpliceIndex = cbs->ua_views[0].index;

	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, view_id_size);

	vmw_cmd_commit(ctx->dev, cmd_size);

	return 0;
}

/**
 * vmw_emit_set_cs_uav - Issue delayed UAV binding commands for the compute
 * pipeline
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 */
static int vmw_emit_set_cs_uav(struct vmw_ctx_binding_state *cbs)
{
	const struct vmw_ctx_bindinfo_view *loc = &cbs->ua_views[1].views[0];
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetCSUAViews body;
	} *cmd;
	size_t cmd_size, view_id_size;
	const struct vmw_resource *ctx = vmw_cbs_context(cbs);

	vmw_collect_view_ids(cbs, loc, vmw_max_num_uavs(cbs->dev));
	view_id_size = cbs->bind_cmd_count*sizeof(uint32);
	cmd_size = sizeof(*cmd) + view_id_size;
	cmd = VMW_CMD_CTX_RESERVE(ctx->dev, cmd_size, ctx->id);
	if (!cmd)
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_CS_UA_VIEWS;
	cmd->header.size = sizeof(cmd->body) + view_id_size;

	/* Start index is specified user-space */
	cmd->body.startIndex = cbs->ua_views[1].index;

	mm_memcpy(&cmd[1], cbs->bind_cmd_buffer, view_id_size);

	vmw_cmd_commit(ctx->dev, cmd_size);

	return 0;
}

/**
 * vmw_binding_emit_dirty - Issue delayed binding commands
 *
 * @cbs: Pointer to the context's struct vmw_ctx_binding_state
 *
 * This function issues the delayed binding commands that arise from
 * previous scrub / unscrub calls. These binding commands are typically
 * commands that batch a number of bindings and therefore it makes sense
 * to delay them.
 *
 * The commands are emitted on the context that owns the bindings, found
 * through the bindings themselves.  A tracker whose bindings were all
 * dropped after an emit failed (a failed reservation leaves its dirty bit
 * set) has no context left to emit on and nothing left bound to emit, so
 * its dirty state is simply forgotten.
 */
static int vmw_binding_emit_dirty(struct vmw_ctx_binding_state *cbs)
{
	int ret = 0;
	unsigned long hit = 0;

	if (vmw_cbs_context(cbs) == NULL) {
		u32 i;

		cbs->dirty = 0;
		bitmap_zero(cbs->dirty_vb, SVGA3D_DX_MAX_VERTEXBUFFERS);
		for (i = 0; i < SVGA3D_NUM_SHADERTYPE; ++i) {
			cbs->per_shader[i].dirty = 0;
			bitmap_zero(cbs->per_shader[i].dirty_sr,
				    SVGA3D_DX_MAX_SRVIEWS);
		}
		return 0;
	}

	while ((hit = find_next_bit(&cbs->dirty, VMW_BINDING_NUM_BITS, hit))
	      < VMW_BINDING_NUM_BITS) {

		switch (hit) {
		case VMW_BINDING_RT_BIT:
			ret = vmw_emit_set_rt(cbs);
			break;
		case VMW_BINDING_PS_BIT:
			ret = vmw_binding_emit_dirty_ps(cbs);
			break;
		case VMW_BINDING_SO_T_BIT:
			ret = vmw_emit_set_so_target(cbs);
			break;
		case VMW_BINDING_VB_BIT:
			ret = vmw_emit_set_vb(cbs);
			break;
		case VMW_BINDING_UAV_BIT:
			ret = vmw_emit_set_uav(cbs);
			break;
		case VMW_BINDING_CS_UAV_BIT:
			ret = vmw_emit_set_cs_uav(cbs);
			break;
		default:
			BUG();
		}
		if (ret)
			return ret;

		__clear_bit(hit, &cbs->dirty);
		hit++;
	}

	return 0;
}

/**
 * vmw_binding_scrub_sr - Schedule a dx shaderresource binding
 * scrub from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_sr(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_view *biv =
		container_of(bi, struct vmw_ctx_bindinfo_view, bi);
	struct vmw_ctx_binding_state *cbs =
		vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(biv->slot, cbs->per_shader[biv->shader_slot].dirty_sr);
	__set_bit(VMW_BINDING_PS_SR_BIT,
		  &cbs->per_shader[biv->shader_slot].dirty);
	__set_bit(VMW_BINDING_PS_BIT, &cbs->dirty);

	return 0;
}

/**
 * vmw_binding_scrub_dx_rt - Schedule a dx rendertarget binding
 * scrub from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_dx_rt(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_binding_state *cbs =
		vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(VMW_BINDING_RT_BIT, &cbs->dirty);

	return 0;
}

/**
 * vmw_binding_scrub_so_target - Schedule a dx streamoutput buffer binding
 * scrub from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_so_target(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_binding_state *cbs =
		vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(VMW_BINDING_SO_T_BIT, &cbs->dirty);

	return 0;
}

/**
 * vmw_binding_scrub_vb - Schedule a dx vertex buffer binding
 * scrub from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_vb(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_vb *bivb =
		container_of(bi, struct vmw_ctx_bindinfo_vb, bi);
	struct vmw_ctx_binding_state *cbs =
		vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(bivb->slot, cbs->dirty_vb);
	__set_bit(VMW_BINDING_VB_BIT, &cbs->dirty);

	return 0;
}

/**
 * vmw_binding_scrub_ib - scrub a dx index buffer binding from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_ib(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_bindinfo_ib *binding =
		container_of(bi, struct vmw_ctx_bindinfo_ib, bi);
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetIndexBuffer body;
	} *cmd;

	cmd = VMW_CMD_CTX_RESERVE(v, sizeof(*cmd), bi->ctx->id);
	if (unlikely(cmd == NULL))
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_INDEX_BUFFER;
	cmd->header.size = sizeof(cmd->body);
	if (rebind) {
		cmd->body.sid = vmw_bi_res_id(bi);
		cmd->body.format = binding->format;
		cmd->body.offset = binding->offset;
	} else {
		cmd->body.sid = SVGA3D_INVALID_ID;
		cmd->body.format = 0;
		cmd->body.offset = 0;
	}

	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_scrub_uav - Schedule a graphics-pipeline UAV binding scrub
 * from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_uav(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_binding_state *cbs = vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(VMW_BINDING_UAV_BIT, &cbs->dirty);
	return 0;
}

/**
 * vmw_binding_scrub_cs_uav - Schedule a compute-pipeline UAV binding scrub
 * from a context
 *
 * @bi: single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_cs_uav(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_ctx_binding_state *cbs = vmw_context_binding_state(bi->ctx);

	(void)rebind;
	__set_bit(VMW_BINDING_CS_UAV_BIT, &cbs->dirty);
	return 0;
}

/**
 * vmw_binding_scrub_so - Scrub a streamoutput binding from context.
 * @bi: Single binding information.
 * @rebind: Whether to issue a bind instead of scrub command.
 */
static int vmw_binding_scrub_so(struct vmw_ctx_bindinfo *bi, bool rebind)
{
	struct vmw_device *v = bi->ctx->dev;
	struct {
		SVGA3dCmdHeader header;
		SVGA3dCmdDXSetStreamOutput body;
	} *cmd;

	cmd = VMW_CMD_CTX_RESERVE(v, sizeof(*cmd), bi->ctx->id);
	if (!cmd)
		return -ENOMEM;

	cmd->header.id = SVGA_3D_CMD_DX_SET_STREAMOUTPUT;
	cmd->header.size = sizeof(cmd->body);
	cmd->body.soid = rebind ? vmw_bi_res_id(bi) : SVGA3D_INVALID_ID;
	vmw_cmd_commit(v, sizeof(*cmd));

	return 0;
}

/**
 * vmw_binding_state_alloc - Allocate a struct vmw_ctx_binding_state.
 *
 * @v: The device.
 *
 * Returns a pointer to a newly allocated struct or an error pointer on error.
 */
struct vmw_ctx_binding_state *vmw_binding_state_alloc(struct vmw_device *v)
{
	struct vmw_ctx_binding_state *cbs;

	cbs = kcalloc(1, sizeof(*cbs));
	if (!cbs)
		return ERR_PTR(-ENOMEM);

	cbs->dev = v;
	INIT_LIST_HEAD(&cbs->list);

	return cbs;
}

/**
 * vmw_binding_state_free - Free a struct vmw_ctx_binding_state.
 *
 * @cbs: Pointer to the struct vmw_ctx_binding_state to be freed.
 */
void vmw_binding_state_free(struct vmw_ctx_binding_state *cbs)
{
	if (IS_ERR_OR_NULL(cbs))
		return;
	kfree(cbs);
}

/**
 * vmw_binding_state_list - Get the binding list of a
 * struct vmw_ctx_binding_state
 *
 * @cbs: Pointer to the struct vmw_ctx_binding_state
 *
 * Returns the binding list which can be used to traverse through the bindings
 * and access the resource information of all bindings.
 */
struct list_head *vmw_binding_state_list(struct vmw_ctx_binding_state *cbs)
{
	return &cbs->list;
}

/**
 * vmw_binding_state_reset - clear a struct vmw_ctx_binding_state
 *
 * @cbs: Pointer to the struct vmw_ctx_binding_state to be cleared
 *
 * Drops all bindings registered in @cbs, and the uav splice indices with
 * them. No device binding actions are performed.
 */
void vmw_binding_state_reset(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_ctx_bindinfo *entry, *next;
	u32 i;

	list_for_each_entry_safe(entry, next, &cbs->list, ctx_list)
		vmw_binding_drop(entry);

	for (i = 0; i < VMW_MAX_UAV_BIND_TYPE; ++i)
		cbs->ua_views[i].index = 0;
	cbs->uav_index_set = 0;
}

/**
 * vmw_binding_dirtying - Return whether a binding type is dirtying its resource
 * @binding_type: The binding type
 *
 * Each time a resource is put on the validation list as the result of a
 * context binding referencing it, we need to determine whether that resource
 * will be dirtied (written to by the GPU) as a result of the corresponding
 * GPU operation. Currently rendertarget-, depth-stencil-, stream-output-target
 * and unordered access view bindings are capable of dirtying its resource.
 *
 * Return: Whether the binding type dirties the resource its binding points to.
 */
u32 vmw_binding_dirtying(enum vmw_ctx_binding_type binding_type)
{
	static const u32 is_binding_dirtying[vmw_ctx_binding_max] = {
		[vmw_ctx_binding_rt] = VMW_RES_DIRTY_SET,
		[vmw_ctx_binding_dx_rt] = VMW_RES_DIRTY_SET,
		[vmw_ctx_binding_ds] = VMW_RES_DIRTY_SET,
		[vmw_ctx_binding_so_target] = VMW_RES_DIRTY_SET,
		[vmw_ctx_binding_uav] = VMW_RES_DIRTY_SET,
		[vmw_ctx_binding_cs_uav] = VMW_RES_DIRTY_SET,
	};

	/* Review this function as new bindings are added. */
	BUILD_BUG_ON(vmw_ctx_binding_max != 14);
	if ((unsigned int)binding_type >= vmw_ctx_binding_max)
		return VMW_RES_DIRTY_NONE;
	return is_binding_dirtying[binding_type];
}
