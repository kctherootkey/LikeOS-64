// LikeOS -- vmwgfx: DX views and state objects.
//
// See vmw_so.h.  The only reason views are tracked at all is that the
// device refuses to destroy a surface while a view still points at it: so
// destroying a surface has to destroy every view of it first, in the view's
// own context, together with every context binding of those views.  For
// that each view records the define command that created it (to emit it
// again if the view ever has to be re-created), the surface it points at
// and the context and object table it lives in, and sits on two lists: the
// surface's view_list and its context's object-table list.
//
// Life cycle of a view:
//   - vmw_view_add() while a command stream is verified: the view exists,
//     is in the context's command-buffer resource manager (staged), but is
//     on neither list yet -- the define has not reached the device.
//   - commit (vmw_view_commit_notify(), state ADD): the stream went to the
//     device; the view joins the surface's and the object table's lists.
//   - destroyed on the device by the client's own destroy command (commit
//     with state DEL), by the destruction of its surface or of its context
//     (vmw_view_destroy()): it leaves both lists and its id becomes -1.
//   - released when the last reference goes (normally the manager's).
//
// Lists and `committed' are protected by vmw_device.binding_lock.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2014-2015 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/io/console.h>

/**
 * struct vmw_view - view metadata
 *
 * @res: The struct vmw_resource we derive from
 * @ctx: Refcounted pointer to the context this view belongs to.
 * @srf: Refcounted pointer to the surface pointed to by this view.
 * @cotable_list: The list, in @ctx, of the object table holding this view.
 * Valid as long as @ctx is, which the reference above guarantees.
 * @srf_head: List head for the surface-to-view list.
 * @cotable_head: List head for the cotable-to_view list.
 * @view_type: View type.
 * @view_id: User-space per context view id. Currently used also as per
 * context device view id.
 * @cmd_size: Size of the SVGA3D define view command that we've copied from the
 * command stream.
 * @committed: Whether the view is actually created or pending creation at the
 * device level.
 * @cmd: The SVGA3D define view command copied from the command stream.
 */
struct vmw_view {
	struct vmw_resource res;
	struct vmw_resource *ctx;	/* Immutable */
	struct vmw_resource *srf;	/* Immutable */
	struct list_head *cotable_list;	/* Immutable */
	struct list_head srf_head;	/* Protected by binding_lock */
	struct list_head cotable_head;	/* Protected by binding_lock */
	unsigned view_type;		/* Immutable */
	unsigned view_id;		/* Immutable */
	u32 cmd_size;			/* Immutable */
	bool committed;			/* Protected by binding_lock */
	u32 cmd[];			/* Immutable */
};

static int vmw_view_create(struct vmw_resource *res);
static void vmw_view_commit_notify(struct vmw_resource *res,
				   enum vmw_cmdbuf_res_state state);

static const struct vmw_res_func vmw_view_func = {
	.res_type = vmw_res_view,
	.needs_guest_memory = false,
	.type_name = "DX view",
	.create = vmw_view_create,
	.commit_notify = vmw_view_commit_notify,
};

/**
 * struct vmw_view_define - view define command body stub
 *
 * @view_id: The device id of the view being defined
 * @sid: The surface id of the view being defined
 *
 * This generic struct is used by the code to change @view_id and @sid of a
 * saved view define command.
 */
struct vmw_view_define {
	uint32 view_id;
	uint32 sid;
};

/**
 * vmw_view - Convert a struct vmw_resource to a struct vmw_view
 *
 * @res: Pointer to the resource to convert.
 *
 * Returns a pointer to a struct vmw_view.
 */
static struct vmw_view *vmw_view(struct vmw_resource *res)
{
	return container_of(res, struct vmw_view, res);
}

/**
 * vmw_view_commit_notify - Notify that a view operation has been committed to
 * hardware from a user-supplied command stream.
 *
 * @res: Pointer to the view resource.
 * @state: Indicating whether a creation or removal has been committed.
 *
 * Called with binding_lock held, in the same hold as the submission of the
 * stream (vmw_cmdbuf_res_commit_notify()).
 */
static void vmw_view_commit_notify(struct vmw_resource *res,
				   enum vmw_cmdbuf_res_state state)
{
	struct vmw_view *view = vmw_view(res);
	struct vmw_device *v = res->dev;

	mm_assert_write_locked(&v->binding_lock);
	if (state == VMW_CMDBUF_RES_ADD) {
		struct vmw_surface *srf = vmw_res_to_srf(view->srf);

		list_add_tail(&view->srf_head, &srf->view_list);
		list_add_tail(&view->cotable_head, view->cotable_list);
		view->committed = true;
		res->id = view->view_id;

	} else {
		list_del_init(&view->cotable_head);
		list_del_init(&view->srf_head);
		view->committed = false;
		res->id = -1;
	}
}

/**
 * vmw_view_create - Create a hardware view.
 *
 * @res: Pointer to the view resource.
 *
 * Create a hardware view from the saved define command.  Every object of
 * this driver stays resident, so nothing destroys a view only to bring it
 * back later and this has no caller today; it is the method that would
 * re-create a view destroyed behind the client's back.
 */
static int vmw_view_create(struct vmw_resource *res)
{
	struct vmw_view *view = vmw_view(res);
	struct vmw_surface *srf = vmw_res_to_srf(view->srf);
	struct vmw_device *v = res->dev;
	struct {
		SVGA3dCmdHeader header;
		struct vmw_view_define body;
	} *cmd;

	mm_write_lock(&v->binding_lock);
	if (!view->committed) {
		mm_write_unlock(&v->binding_lock);
		return 0;
	}

	cmd = VMW_CMD_CTX_RESERVE(v, view->cmd_size, view->ctx->id);
	if (!cmd) {
		mm_write_unlock(&v->binding_lock);
		return -ENOMEM;
	}

	mm_memcpy(cmd, &view->cmd, view->cmd_size);
	WARN_ON(cmd->body.view_id != view->view_id);
	/* Sid may have changed since the define was recorded. */
	WARN_ON(view->srf->id == (int)SVGA3D_INVALID_ID || view->srf->id == -1);
	cmd->body.sid = (uint32)view->srf->id;
	vmw_cmd_commit(v, view->cmd_size);
	res->id = view->view_id;
	list_add_tail(&view->srf_head, &srf->view_list);
	list_add_tail(&view->cotable_head, view->cotable_list);
	mm_write_unlock(&v->binding_lock);

	return 0;
}

/**
 * vmw_view_destroy - Destroy a hardware view.
 *
 * @res: Pointer to the view resource.
 *
 * Destroy a hardware view. Used on termination of the owning context or if
 * the surface the view is pointing to is destroyed.  The caller holds
 * binding_lock.
 *
 * Whatever happens on the device, the view leaves the surface's and the
 * object table's lists here: both lists belong to objects that are going
 * away, and a view left on them would be walked again after they are gone.
 *
 * With VMW_TRACK_VIEWS 0 no command is emitted at all -- neither the
 * scrub of the view's bindings nor the destroy -- which is how the driver
 * behaved before views were tracked.
 */
static int vmw_view_destroy(struct vmw_resource *res)
{
	struct vmw_device *v = res->dev;
	struct vmw_view *view = vmw_view(res);
	int ret = 0;
	struct {
		SVGA3dCmdHeader header;
		union vmw_view_destroy body;
	} *cmd;

	mm_assert_write_locked(&v->binding_lock);
#if VMW_TRACK_VIEWS
	vmw_binding_res_list_scrub(&res->binding_head);
#endif

	if (!view->committed || res->id == -1)
		goto out_unlink;

#if VMW_TRACK_VIEWS
	/* A context that is gone from the device took its views with it;
	 * naming it now would be an error, and its id may already belong to
	 * another context. */
	if (view->ctx->id != -1) {
		cmd = VMW_CMD_CTX_RESERVE(v, sizeof(*cmd), view->ctx->id);
		if (cmd) {
			cmd->header.id = vmw_view_destroy_cmds[view->view_type];
			cmd->header.size = sizeof(cmd->body);
			cmd->body.view_id = view->view_id;
			vmw_cmd_commit(v, sizeof(*cmd));
		} else {
			ret = -ENOMEM;
		}
	}
#else
	(void)cmd;
#endif
	res->id = -1;

out_unlink:
	list_del_init(&view->cotable_head);
	list_del_init(&view->srf_head);

	return ret;
}

#if VMW_TRACK_VIEWS
/**
 * vmw_hw_view_destroy - Destroy a hardware view as part of resource cleanup.
 *
 * @res: Pointer to the view resource.
 *
 * Destroy a hardware view if it's still present.
 */
static void vmw_hw_view_destroy(struct vmw_resource *res)
{
	struct vmw_device *v = res->dev;

	mm_write_lock(&v->binding_lock);
	(void)vmw_view_destroy(res);
	res->id = -1;
	mm_write_unlock(&v->binding_lock);
}
#endif

/* Forget every binding on `head' without telling the device.
 *
 * Marking a binding scrubbed is what keeps the tracker's kill from emitting
 * the command that unbinds it; the kill then unlinks it from its context.
 * For VMW_TRACK_VIEWS 0, where a binding record must still not outlive the
 * object it names but no new command may reach the device.  Caller holds
 * binding_lock. */
void vmw_binding_res_list_forget(struct list_head *head)
{
	struct vmw_ctx_bindinfo *entry;

	list_for_each_entry(entry, head, res_list)
		entry->scrubbed = true;
	vmw_binding_res_list_kill(head);
}

/**
 * vmw_view_key - Compute a view key suitable for the cmdbuf resource manager
 *
 * @user_key: The user-space id used for the view.
 * @view_type: The view type.
 *
 * Returns the key the view is filed under in its context's manager.
 */
static u32 vmw_view_key(u32 user_key, enum vmw_view_type view_type)
{
	return user_key | ((u32)view_type << 20);
}

/**
 * vmw_view_id_ok - Basic view id and type range checks.
 *
 * @user_key: The user-space id used for the view.
 * @view_type: The view type.
 *
 * Checks that the view id and type (typically provided by user-space) is
 * valid.
 */
static bool vmw_view_id_ok(u32 user_key, enum vmw_view_type view_type)
{
	return (user_key < SVGA_COTABLE_MAX_IDS &&
		(unsigned)view_type < (unsigned)vmw_view_max);
}

/**
 * vmw_view_res_free - resource res_free callback for view resources
 *
 * @res: Pointer to a struct vmw_resource
 *
 * Frees memory held by the struct vmw_view.
 */
static void vmw_view_res_free(struct vmw_resource *res)
{
	struct vmw_view *view = vmw_view(res);

#if !VMW_TRACK_VIEWS
	/* No hw_destroy in this mode, so the release did not drop the
	 * bindings or take the view off its lists: do both here, quietly. */
	struct vmw_device *v = res->dev;

	mm_write_lock(&v->binding_lock);
	vmw_binding_res_list_forget(&res->binding_head);
	(void)vmw_view_destroy(res);
	mm_write_unlock(&v->binding_lock);
#endif
	vmw_resource_unreference(&view->ctx);
	vmw_resource_unreference(&view->srf);
	kfree(view);
}

/* The define command body size for a view type; the depth-stencil view has
 * a second, longer form for SM5. */
static size_t vmw_view_define_size(enum vmw_view_type view_type, u32 cmd_id)
{
	static const size_t vmw_view_define_sizes[] = {
		[vmw_view_sr] = sizeof(SVGA3dCmdDXDefineShaderResourceView),
		[vmw_view_rt] = sizeof(SVGA3dCmdDXDefineRenderTargetView),
		[vmw_view_ds] = sizeof(SVGA3dCmdDXDefineDepthStencilView),
		[vmw_view_ua] = sizeof(SVGA3dCmdDXDefineUAView)
	};

	if (view_type == vmw_view_ds &&
	    cmd_id == SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2)
		return sizeof(SVGA3dCmdDXDefineDepthStencilView_v2);
	return vmw_view_define_sizes[view_type];
}

/**
 * vmw_view_add - Create a view resource and stage it for addition
 * as a command buffer managed resource.
 *
 * @man: Pointer to the compat shader manager identifying the shader namespace.
 * @ctx: Pointer to a struct vmw_resource identifying the active context.
 * @srf: Pointer to a struct vmw_resource identifying the surface the view
 * points to.
 * @view_type: The view type deduced from the view create command.
 * @user_key: The key that is used to identify the shader. The key is
 * unique to the view type and to the context.
 * @cmd: Pointer to the view create command in the command stream.
 * @cmd_size: Size of the view create command in the command stream.
 * @list: Caller's list of staged command buffer resource actions.
 */
int vmw_view_add(struct vmw_cmdbuf_res_manager *man, struct vmw_resource *ctx,
		 struct vmw_resource *srf, enum vmw_view_type view_type,
		 u32 user_key, const void *cmd, size_t cmd_size,
		 struct list_head *list)
{
	struct vmw_device *v = ctx->dev;
	const SVGA3dCmdHeader *header = cmd;
	struct list_head *cotable_list;
	struct vmw_resource *res;
	struct vmw_view *view;
	size_t size;
	int ret;

	if (!man || !srf || cmd_size < sizeof(SVGA3dCmdHeader))
		return -EINVAL;

	if (!vmw_view_id_ok(user_key, view_type)) {
		VMW_DEBUG_USER("Illegal view add view id.\n");
		return -EINVAL;
	}

	if (cmd_size != vmw_view_define_size(view_type, header->id) +
	    sizeof(SVGA3dCmdHeader)) {
		VMW_DEBUG_USER("Illegal view create command size.\n");
		return -EINVAL;
	}

	cotable_list = vmw_context_cotable_list(ctx, vmw_view_cotables[view_type]);
	if (!cotable_list)
		return -EINVAL;

	size = offsetof(struct vmw_view, cmd) + cmd_size;

	view = kalloc(size);
	if (!view)
		return -ENOMEM;

	res = &view->res;
	view->ctx = vmw_resource_reference(ctx);
	view->srf = vmw_resource_reference(srf);
	view->cotable_list = cotable_list;
	view->view_type = view_type;
	view->view_id = user_key;
	view->cmd_size = (u32)cmd_size;
	view->committed = false;
	INIT_LIST_HEAD(&view->srf_head);
	INIT_LIST_HEAD(&view->cotable_head);
	mm_memcpy(&view->cmd, cmd, cmd_size);
	ret = vmw_resource_init(v, res, -1, vmw_view_res_free, &vmw_view_func);
	if (ret)
		goto out_resource_init;

	ret = vmw_cmdbuf_res_add(man, vmw_cmdbuf_res_view,
				 vmw_view_key(user_key, view_type),
				 res, list);
	if (ret)
		goto out_resource_init;

	res->id = view->view_id;
#if VMW_TRACK_VIEWS
	res->hw_destroy = vmw_hw_view_destroy;
#endif

out_resource_init:
	/* The creator's reference: on success the manager's entry holds the
	 * view, on failure this frees it. */
	vmw_resource_unreference(&res);

	return ret;
}

/**
 * vmw_view_remove - Stage a view for removal.
 *
 * @man: Pointer to the view manager identifying the shader namespace.
 * @user_key: The key that is used to identify the view. The key is
 * unique to the view type.
 * @view_type: View type
 * @list: Caller's list of staged command buffer resource actions.
 * @res_p: If the resource is in an already committed state, points to the
 * struct vmw_resource on successful return. The pointer will be
 * non ref-counted.
 */
int vmw_view_remove(struct vmw_cmdbuf_res_manager *man, u32 user_key,
		    enum vmw_view_type view_type, struct list_head *list,
		    struct vmw_resource **res_p)
{
	*res_p = NULL;
	if (!vmw_view_id_ok(user_key, view_type)) {
		VMW_DEBUG_USER("Illegal view remove view id.\n");
		return -EINVAL;
	}

	return vmw_cmdbuf_res_remove(man, vmw_cmdbuf_res_view,
				     vmw_view_key(user_key, view_type),
				     list, res_p);
}

/**
 * vmw_view_cotable_list_destroy - Destroy all views belonging to a cotable.
 *
 * @v: The device.
 * @list: List of views belonging to a cotable.
 * @readback: Unused. Needed for function interface only.
 *
 * This function destroys all views belonging to a cotable.
 * It must be called with the binding_lock held. This is called before the
 * cotable goes away with its context.
 */
void vmw_view_cotable_list_destroy(struct vmw_device *v, struct list_head *list,
				   bool readback)
{
	struct vmw_view *entry, *next;

	(void)readback;
	mm_assert_write_locked(&v->binding_lock);

	list_for_each_entry_safe(entry, next, list, cotable_head)
		(void)vmw_view_destroy(&entry->res);
}

/**
 * vmw_view_surface_list_destroy - Destroy all views pointing to a surface
 *
 * @v: The device.
 * @view_list: List of views pointing to a surface.
 *
 * This function destroys all views pointing to a surface. It is called
 * before the surface is destroyed, with the binding_lock held.
 */
void vmw_view_surface_list_destroy(struct vmw_device *v,
				   struct list_head *view_list)
{
	struct vmw_view *entry, *next;

	mm_assert_write_locked(&v->binding_lock);

	list_for_each_entry_safe(entry, next, view_list, srf_head)
		(void)vmw_view_destroy(&entry->res);
}

/**
 * vmw_view_srf - Return a non-refcounted pointer to the surface a view is
 * pointing to.
 *
 * @res: pointer to a view resource.
 *
 * Note that the view itself is holding a reference, so as long
 * the view resource is alive, the surface resource will be.
 */
struct vmw_resource *vmw_view_srf(struct vmw_resource *res)
{
	return vmw_view(res)->srf;
}

/**
 * vmw_view_lookup - Look up a view.
 *
 * @man: The context's cmdbuf ref manager.
 * @view_type: The view type.
 * @user_key: The view user id.
 *
 * returns a non-refcounted pointer to a view or an error pointer if not
 * found.
 */
struct vmw_resource *vmw_view_lookup(struct vmw_cmdbuf_res_manager *man,
				     enum vmw_view_type view_type, u32 user_key)
{
	if (!vmw_view_id_ok(user_key, view_type))
		return ERR_PTR(-EINVAL);
	return vmw_cmdbuf_res_lookup(man, vmw_cmdbuf_res_view,
				     vmw_view_key(user_key, view_type));
}

/**
 * vmw_view_dirtying - Return whether a view type is dirtying its resource
 * @res: Pointer to the view
 *
 * Each time a resource is put on the validation list as the result of a
 * view pointing to it, we need to determine whether that resource will
 * be dirtied (written to by the GPU) as a result of the corresponding
 * GPU operation. Currently only rendertarget-, depth-stencil and unordered
 * access views are capable of dirtying its resource.
 *
 * Return: Whether the view type of @res dirties the resource it points to.
 */
u32 vmw_view_dirtying(struct vmw_resource *res)
{
	static const u32 view_is_dirtying[vmw_view_max] = {
		[vmw_view_rt] = VMW_RES_DIRTY_SET,
		[vmw_view_ds] = VMW_RES_DIRTY_SET,
		[vmw_view_ua] = VMW_RES_DIRTY_SET,
	};

	/* Update this function as we add more view types */
	BUILD_BUG_ON(vmw_view_max != 4);
	return view_is_dirtying[vmw_view(res)->view_type];
}

const u32 vmw_view_destroy_cmds[] = {
	[vmw_view_sr] = SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW,
	[vmw_view_rt] = SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
	[vmw_view_ds] = SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW,
	[vmw_view_ua] = SVGA_3D_CMD_DX_DESTROY_UA_VIEW,
};

const SVGACOTableType vmw_view_cotables[] = {
	[vmw_view_sr] = SVGA_COTABLE_SRVIEW,
	[vmw_view_rt] = SVGA_COTABLE_RTVIEW,
	[vmw_view_ds] = SVGA_COTABLE_DSVIEW,
	[vmw_view_ua] = SVGA_COTABLE_UAVIEW,
};

const SVGACOTableType vmw_so_cotables[] = {
	[vmw_so_el] = SVGA_COTABLE_ELEMENTLAYOUT,
	[vmw_so_bs] = SVGA_COTABLE_BLENDSTATE,
	[vmw_so_ds] = SVGA_COTABLE_DEPTHSTENCIL,
	[vmw_so_rs] = SVGA_COTABLE_RASTERIZERSTATE,
	[vmw_so_ss] = SVGA_COTABLE_SAMPLER,
	[vmw_so_so] = SVGA_COTABLE_STREAMOUTPUT,
	[vmw_so_max]= SVGA_COTABLE_MAX
};

/* The layout assumptions the view code is built on, checked by the
 * compiler; never called. */
static void vmw_so_build_asserts(void) __attribute__((used));

static void vmw_so_build_asserts(void)
{
	/* Assert that our vmw_view_cmd_to_type() function is correct. */
	BUILD_BUG_ON(SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW !=
		     SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW + 1);
	BUILD_BUG_ON(SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW !=
		     SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW + 2);
	BUILD_BUG_ON(SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW !=
		     SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW + 3);
	BUILD_BUG_ON(SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW !=
		     SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW + 4);
	BUILD_BUG_ON(SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW !=
		     SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW + 5);

	/* Assert that our "one body fits all" assumption is valid */
	BUILD_BUG_ON(sizeof(union vmw_view_destroy) != sizeof(u32));

	/* Assert that the view key space can hold all view ids. */
	BUILD_BUG_ON(SVGA_COTABLE_MAX_IDS >= ((1 << 20) - 1));

	/* And that the view types fit below the command-buffer resource
	 * type, which takes the top byte of the manager's key. */
	BUILD_BUG_ON(((u32)vmw_view_max << 20) >= (1u << 24));

	/*
	 * Assert that the offset of sid in all view define commands
	 * is what we assume it to be.
	 */
	BUILD_BUG_ON(offsetof(struct vmw_view_define, sid) !=
		     offsetof(SVGA3dCmdDXDefineShaderResourceView, sid));
	BUILD_BUG_ON(offsetof(struct vmw_view_define, sid) !=
		     offsetof(SVGA3dCmdDXDefineRenderTargetView, sid));
	BUILD_BUG_ON(offsetof(struct vmw_view_define, sid) !=
		     offsetof(SVGA3dCmdDXDefineDepthStencilView, sid));
	BUILD_BUG_ON(offsetof(struct vmw_view_define, sid) !=
		     offsetof(SVGA3dCmdDXDefineUAView, sid));
	BUILD_BUG_ON(offsetof(struct vmw_view_define, sid) !=
		     offsetof(SVGA3dCmdDXDefineDepthStencilView_v2, sid));
}
