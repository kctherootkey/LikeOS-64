// LikeOS -- vmwgfx: rendering contexts and legacy shaders.
//
// A DX context is where all VGPU10 state lives; its object tables
// (COTables: views, states, shaders, queries...) are MOB-backed buffers
// the driver grows as userspace's ids climb.  Legacy GB contexts and
// shaders exist for hosts without DX.
//
// A guest-backed context also carries what its command streams created
// and bound: a binding tracker (vmw_binding.c) and a command-buffer
// resource manager holding its views (vmw_cmdbuf_res.c, vmw_so.c).  Both
// are torn down, and the device told, before the context itself is
// destroyed: a view or binding that outlived its context would be named
// later in a command for a context id the device no longer has -- or that
// by then belongs to another process's context.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright 2014-2023 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

static const uint32_t cotable_entry_size[SVGA_COTABLE_MAX] = {
	[SVGA_COTABLE_RTVIEW] = sizeof(SVGACOTableDXRTViewEntry),
	[SVGA_COTABLE_DSVIEW] = sizeof(SVGACOTableDXDSViewEntry),
	[SVGA_COTABLE_SRVIEW] = sizeof(SVGACOTableDXSRViewEntry),
	[SVGA_COTABLE_ELEMENTLAYOUT] = sizeof(SVGACOTableDXElementLayoutEntry),
	[SVGA_COTABLE_BLENDSTATE] = sizeof(SVGACOTableDXBlendStateEntry),
	[SVGA_COTABLE_DEPTHSTENCIL] = sizeof(SVGACOTableDXDepthStencilEntry),
	[SVGA_COTABLE_RASTERIZERSTATE] = sizeof(SVGACOTableDXRasterizerStateEntry),
	[SVGA_COTABLE_SAMPLER] = sizeof(SVGACOTableDXSamplerEntry),
	[SVGA_COTABLE_STREAMOUTPUT] = sizeof(SVGACOTableDXStreamOutputEntry),
	[SVGA_COTABLE_DXQUERY] = sizeof(SVGACOTableDXQueryEntry),
	[SVGA_COTABLE_DXSHADER] = sizeof(SVGACOTableDXShaderEntry),
	[SVGA_COTABLE_UAVIEW] = sizeof(SVGACOTableDXUAViewEntry),
};

/* ---- the device-object base -------------------------------------------- */

static const struct vmw_res_func vmw_legacy_context_func = {
	.res_type = vmw_res_context,
	.needs_guest_memory = false,
	.type_name = "legacy contexts",
};

static const struct vmw_res_func vmw_gb_context_func = {
	.res_type = vmw_res_context,
	.needs_guest_memory = true,
	.type_name = "guest backed contexts",
};

static const struct vmw_res_func vmw_dx_context_func = {
	.res_type = vmw_res_dx_context,
	.needs_guest_memory = true,
	.type_name = "dx contexts",
};

/* The last reference is gone: the device object went in
 * vmw_context_destroy(), so only the memory is left -- and the binding
 * tracker, which is freed here rather than at destroy because a binding
 * record reaches its context's tracker through the context
 * (vmw_context_binding_state()); the destroy killed every record, so
 * nothing links into it any more. */
static void vmw_context_res_free(struct vmw_resource *res)
{
	struct vmw_context *c = vmw_res_to_ctx(res);

	if (c->cbs)
		vmw_binding_state_free(c->cbs);
	c->cbs = NULL;
	(void)vmw_context_bind_dx_query(res, NULL);
	kfree(c);
}

struct list_head *vmw_context_binding_list(struct vmw_resource *ctx)
{
	struct vmw_context *c = vmw_res_to_ctx(ctx);

	return c->cbs ? vmw_binding_state_list(c->cbs) : NULL;
}

struct vmw_cmdbuf_res_manager *vmw_context_res_man(struct vmw_resource *ctx)
{
	return vmw_res_to_ctx(ctx)->man;
}

struct vmw_ctx_binding_state *vmw_context_binding_state(struct vmw_resource *ctx)
{
	return vmw_res_to_ctx(ctx)->cbs;
}

struct list_head *vmw_context_cotable_list(struct vmw_resource *ctx,
					   SVGACOTableType cotable_type)
{
	struct vmw_context *c = vmw_res_to_ctx(ctx);

	if ((unsigned)cotable_type >= SVGA_COTABLE_MAX)
		return NULL;
	return &c->cot[cotable_type].resource_list;
}

/**
 * vmw_context_bind_dx_query -
 * Sets query MOB for the context.  If @mob is NULL, then this function will
 * remove the association between the MOB and the context.  Called under
 * execbuf_lock (or with the context unreachable by any submitter); never
 * under binding_lock, since forgetting the MOB drops a buffer reference.
 *
 * @ctx_res: The context resource
 * @mob: the query MOB; the context takes its own reference.
 *
 * Returns -EINVAL if a MOB has already been set and does not match the one
 * specified in the parameter.  0 otherwise.
 *
 * Only the association is kept: every buffer of this driver stays resident
 * with its MOB, so the context's queries never have to be read back and
 * re-bound to a moved buffer.  See also "No query MOB here" in
 * vmw_context_create().
 */
int vmw_context_bind_dx_query(struct vmw_resource *ctx_res,
			      struct drm_gem_object *mob)
{
	struct vmw_context *c = vmw_res_to_ctx(ctx_res);

	if (mob == NULL) {
		if (c->dx_query_mob) {
			struct drm_gem_object *old = c->dx_query_mob;

			c->dx_query_mob = NULL;
			drm_gem_put(old);
		}

		return 0;
	}

	/* Can only have one MOB per context for queries */
	if (c->dx_query_mob && c->dx_query_mob != mob)
		return -EINVAL;

	if (!c->dx_query_mob) {
		drm_gem_get(mob);
		c->dx_query_mob = mob;
	}

	return 0;
}

struct drm_gem_object *vmw_context_get_dx_query_mob(struct vmw_resource *ctx_res)
{
	return vmw_res_to_ctx(ctx_res)->dx_query_mob;
}

/* ---- device objects ---------------------------------------------------- */

static struct drm_gem_object *mob_bo_alloc(struct vmw_device *v, uint32_t size)
{
	struct drm_gem_object *bo = drm_gem_alloc(&v->drm, DRM_GEM_BO, size);

	if (!bo)
		return NULL;
	if (drm_gem_alloc_pages(bo) || v->drm.drv->gem_init(bo)) {
		drm_gem_put(bo);
		return NULL;
	}
	struct vmw_bo *b = bo->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID) {
		drm_gem_put(bo);
		return NULL;
	}
	/* drm_gem_alloc_pages hands these out zeroed, which the device
	 * relies on: anything past a COTable's declared valid size must
	 * read as empty. */
	return bo;
}

static uint32_t bo_mobid(struct drm_gem_object *bo)
{
	struct vmw_bo *b = bo ? bo->priv : NULL;
	return b ? b->mob.id : SVGA3D_INVALID_ID;
}

/* (Re)size COTable `type' so entry `id' fits: read the device's table back,
 * allocate the bigger one, copy EVERYTHING across, and switch the device to
 * it.
 *
 * Two rules here carry the whole DX state of the context and were both
 * violated once, silently killing every view, shader and state object past
 * the first table's worth:
 *
 * - The device writes an entry for every DEFINE that fits the current
 *   table, without this function hearing about it.  So no kernel-side
 *   watermark can say how much of the table is live: after READBACK the
 *   ENTIRE buffer is the device's state, the copy must span the ENTIRE old
 *   buffer, and validSizeInBytes on the switch is the ENTIRE old size:
 *   the readback records the full table size and the resize copies
 *   every page of the old buffer.  Copying any tracked prefix instead
 *   throws away the entries
 *   defined since the last grow; the ids stay valid-looking, draws that
 *   use them are accepted, and the rendering lands nowhere.
 *
 * - The new pages must be zeroed (mob_bo_alloc guarantees it): the region
 *   past validSizeInBytes becomes device state the moment the next entry
 *   is defined, and recycled kernel pages there are neither zero nor
 *   harmless. */
int vmw_context_cotable_reserve(struct vmw_device *v, struct vmw_context *c,
				int type, uint32_t id)
{
	if (type < 0 || type >= SVGA_COTABLE_MAX)
		return -EINVAL;
	struct vmw_cotable *ct = &c->cot[type];
	uint32_t need = (id + 1) * cotable_entry_size[type];
	if (ct->bo && need <= ct->size)
		return 0;
	uint32_t nsize = ct->size ? ct->size : 4096;
	while (nsize < need)
		nsize *= 2;
	nsize = (nsize + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
	struct drm_gem_object *nbo = mob_bo_alloc(v, nsize);
	if (!nbo)
		return -ENOMEM;
	SVGA3dCmdDXSetCOTable cmd;
	cmd.cid = c->cid;
	cmd.mobid = bo_mobid(nbo);
	cmd.type = (SVGACOTableType)type;
	/* What the device may read from the new buffer: everything the
	 * readback below put in the old one -- its full size.  A fresh
	 * table has nothing to carry over. */
	cmd.validSizeInBytes = ct->bo ? ct->size : 0;
	if (ct->bo) {
		/* Ask the device to write the current contents back first,
		 * then switch. */
		SVGA3dCmdDXReadbackCOTable rb;
		rb.cid = c->cid;
		rb.type = (SVGACOTableType)type;
		/* Everything already submitted has to finish first.
		 *
		 * Commands are queued, so batches built against THIS table
		 * may still be executing -- and what follows takes the table
		 * away from the device: it reads the old buffer with the
		 * processor and then points the context at a different one.
		 * Doing that while the device is still working through the
		 * old table is how a context comes to reject the state
		 * objects defined in it, and then every draw that uses them. */
		vmw_cmd_drain(v);
		/* Synchronous: the table has been written back by the time
		 * this returns, so the copy below sees it.  And CHECKED:
		 * without a readback the old buffer holds whatever the
		 * processor last saw, while validSizeInBytes below tells the
		 * device to treat that much of the new buffer as live state.
		 * Carrying stale entries over is worse than not growing. */
		int rbrc = vmw_cmd_one_sync(v, SVGA_3D_CMD_DX_READBACK_COTABLE,
					    &rb, sizeof(rb));
		if (rbrc) {
			kprintf("[drm] vmwgfx: COTable %d readback failed (%d); not growing\n",
				type, rbrc);
			drm_gem_put(nbo);
			return rbrc;
		}
		/* The whole old buffer, page by page. */
		for (uint32_t off = 0; off < ct->size; off += PAGE_SIZE) {
			void *src = drm_gem_page_virt(ct->bo, off / PAGE_SIZE);
			void *dst = drm_gem_page_virt(nbo, off / PAGE_SIZE);
			if (src && dst)
				mm_memcpy(dst, src, PAGE_SIZE);
		}
	}
	/* SYNCHRONOUS, and the answer decides everything below it.
	 *
	 * Queued submission returns 0 as soon as the command has been copied
	 * into the gather buffer; it cannot report a rejection, and this is
	 * the one caller that cannot do without one.  Committing ct->size on
	 * that answer is what turned a single refused resize into a
	 * permanent one: the table was recorded as grown, so no later id in
	 * the new range asked to grow it again, and every DEFINE past the
	 * device's real table was handed over to be rejected.  A command
	 * error halts the context, so the batch after it fails too, and the
	 * one after that -- the endless stream of command errors that
	 * followed a single refused DX_SET_COTABLE were all consequences of
	 * believing this one had worked.
	 *
	 * It also orders the release below.  drm_gem_put() hands the old
	 * buffer's pages straight back to the allocator; with the switch
	 * merely queued, they were recycled while the device had yet to
	 * execute either the command that stops reading them or the
	 * DESTROY_GB_MOB that follows it in the same buffer.
	 *
	 * The cost is one device round trip per growth of one COTable of one
	 * context -- a handful over a session, on a path that already drains
	 * the queue and waits for a readback two statements above. */
	int rc = vmw_cmd_one_sync(v, SVGA_3D_CMD_DX_SET_COTABLE, &cmd,
				  sizeof(cmd));
	if (rc) {
		/* The context still has the table it had.  Say so: the
		 * caller then refuses the batch instead of submitting
		 * defines the device is certain to reject, and the next id
		 * in this range tries the resize again rather than the
		 * driver spending the rest of the session sure it succeeded. */
		kprintf("[drm] vmwgfx: COTable %d resize %u -> %u refused (%d)\n",
			type, ct->size, nsize, rc);
		drm_gem_put(nbo);
		return rc;
	}
	if (ct->bo)
		drm_gem_put(ct->bo);
	ct->bo = nbo;
	ct->size = nsize;
	return 0;
}

int vmw_context_create(struct vmw_device *v, struct drm_file *fp, int dx,
		       struct vmw_context **out)
{
	uint64_t fl;

	if (dx && !v->has_dx)
		return -ENODEV;
	spin_lock_irqsave(&v->id_lock, &fl);
	int cid = vmw_id_alloc(v->context_ids, VMW_NUM_CONTEXTS);
	spin_unlock_irqrestore(&v->id_lock, fl);
	if (cid < 0)
		return -ENOSPC;
	struct vmw_context *c = kalloc(sizeof(*c));
	if (!c) {
		vmw_id_free(v->context_ids, (uint32_t)cid);
		return -ENOMEM;
	}
	mm_memset(c, 0, sizeof(*c));
	c->cid = (uint32_t)cid;
	c->dx = dx;
	c->owner = fp;
	vmw_resource_init(v, &c->res, cid, vmw_context_res_free,
			  dx ? &vmw_dx_context_func :
			  v->has_gb ? &vmw_gb_context_func :
				      &vmw_legacy_context_func);
	for (int t = 0; t < SVGA_COTABLE_MAX; t++)
		INIT_LIST_HEAD(&c->cot[t].resource_list);
	int rc;
	if (v->has_gb) {
		/* What its command streams create and bind.  Only a
		 * guest-backed context has either: a legacy context's state
		 * is the device's business alone.  The tracker is large
		 * (tens of kilobytes) and there can be hundreds of contexts,
		 * which is the other reason not to give it to contexts that
		 * cannot use it. */
		c->man = vmw_cmdbuf_res_man_create(v);
		if (IS_ERR(c->man)) {
			rc = (int)PTR_ERR(c->man);
			c->man = NULL;
			goto fail;
		}
		c->cbs = vmw_binding_state_alloc(v);
		if (IS_ERR(c->cbs)) {
			rc = (int)PTR_ERR(c->cbs);
			c->cbs = NULL;
			goto fail;
		}
		/* Context state storage: 16 KB is what the device uses for
		 * a bound context on this device family. */
		c->state_bo = mob_bo_alloc(v, 16384);
		if (!c->state_bo) {
			rc = -ENOMEM;
			goto fail;
		}
		if (dx) {
			SVGA3dCmdDXDefineContext d = { .cid = c->cid };
			rc = vmw_cmd_one(v, SVGA_3D_CMD_DX_DEFINE_CONTEXT, &d, sizeof(d));
			if (rc)
				goto fail;
			c->defined = 1;
			SVGA3dCmdDXBindContext b = { .cid = c->cid,
						     .mobid = bo_mobid(c->state_bo),
						     .validContents = 0 };
			rc = vmw_cmd_one(v, SVGA_3D_CMD_DX_BIND_CONTEXT, &b, sizeof(b));
			if (rc)
				goto fail;
			/* Every COTable starts with room for a few entries so
			 * the first define never trips a grow mid-stream. */
			for (int t = 0; t < SVGA_COTABLE_MAX; t++) {
				if (t == SVGA_COTABLE_UAVIEW && !v->has_sm5)
					continue;
				rc = vmw_context_cotable_reserve(v, c, t, 15);
				if (rc)
					goto fail;
			}
			/* No query MOB here.
			 *
			 * A DX context's query results live in a MOB the
			 * CLIENT owns and names: Mesa allocates it and binds
			 * it with DX_BIND_QUERY in its own command stream
			 * (validated in vmw_execbuf.c), and the kernel's only
			 * part in it is to remember which MOB that was, so it
			 * can be re-bound with DX_BIND_ALL_QUERY should the
			 * context ever be evicted and brought back.
			 *
			 * Binding a kernel MOB to every query of a context
			 * that has not defined a single query is not that,
			 * and the device says so: it answers DX_BIND_ALL_QUERY
			 * with a command error, which STOPS command-buffer
			 * context 0.  Every buffer already queued behind it is
			 * then dropped -- the surface defines, the MOB
			 * destroys, and the fence that ends each batch -- so
			 * the ids they would have created are rejected in
			 * turn and every fence wait ends in ETIMEDOUT.  One
			 * command the device would not take cost the whole
			 * 3D path. */
		} else {
			SVGA3dCmdDefineGBContext d = { .cid = c->cid };
			rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_CONTEXT, &d, sizeof(d));
			if (rc)
				goto fail;
			c->defined = 1;
			SVGA3dCmdBindGBContext b = { .cid = c->cid,
						     .mobid = bo_mobid(c->state_bo),
						     .validContents = 0 };
			rc = vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_CONTEXT, &b, sizeof(b));
			if (rc)
				goto fail;
		}
	} else {
		SVGA3dCmdDefineContext d = { .cid = c->cid };
		rc = vmw_cmd_one(v, SVGA_3D_CMD_CONTEXT_DEFINE, &d, sizeof(d));
		if (rc)
			goto fail;
		c->defined = 1;
	}
	*out = c;
	return 0;
fail:
	vmw_context_destroy(v, c);
	return rc;
}

#if !VMW_TRACK_VIEWS
/* VMW_TRACK_VIEWS 0: drop every binding record of the context without the
 * commands that would unbind them on the device (see vmw_so.h). */
static void vmw_context_bindings_forget(struct vmw_ctx_binding_state *cbs)
{
	struct vmw_ctx_bindinfo *entry;

	list_for_each_entry(entry, vmw_binding_state_list(cbs), ctx_list)
		entry->scrubbed = true;
	vmw_binding_state_kill(cbs);
}
#endif

/* Tear down what the context's command streams left behind, before the
 * context itself goes.
 *
 * First the command-buffer resources: dropping the manager drops the last
 * reference to each view, and a view's release unbinds it from every
 * context and destroys it on the device -- in THIS context, which still
 * exists.  A view somebody else still holds a reference to is destroyed
 * through the object-table lists instead, so that no view is left on its
 * surface's view_list naming a context that is about to disappear: the
 * surface's destroy would otherwise emit a view destroy for a dead (or
 * reused) context id.  Then everything the context has bound is unbound
 * and the records dropped, so that no object's binding list still leads
 * here.
 *
 * Called under execbuf_lock, so no submission is staging against the
 * manager or recording bindings on the tracker meanwhile.  The manager is
 * destroyed outside binding_lock, because a view's release takes it. */
static void vmw_context_scrub_objects(struct vmw_device *v,
				      struct vmw_context *c)
{
	if (c->man) {
		vmw_cmdbuf_res_man_destroy(c->man);
		c->man = NULL;
	}

	mm_write_lock(&v->binding_lock);
	for (int t = 0; t < (int)vmw_view_max; t++)
		vmw_view_cotable_list_destroy(v,
			&c->cot[vmw_view_cotables[t]].resource_list, false);
	/* Only views are put on an object table's list; anything else
	 * there would be left pointing into freed memory. */
	for (int t = 0; t < SVGA_COTABLE_MAX; t++)
		WARN_ON_ONCE(!list_empty(&c->cot[t].resource_list));
	if (c->cbs) {
#if VMW_TRACK_VIEWS
		vmw_binding_state_kill(c->cbs);
#else
		vmw_context_bindings_forget(c->cbs);
#endif
	}
	mm_write_unlock(&v->binding_lock);
}

void vmw_context_destroy(struct vmw_device *v, struct vmw_context *c)
{
	/* No submission may be using the context while it is torn down:
	 * a stream in flight holds a pointer to it, stages views on its
	 * manager and records bindings on its tracker.  Submissions look the
	 * context up under this lock, so once it is held nobody else can
	 * reach the context. */
	mm_write_lock(&v->execbuf_lock);
	vmw_context_scrub_objects(v, c);
	if (c->defined) {
		if (c->dx) {
			SVGA3dCmdDXDestroyContext d = { .cid = c->cid };
			vmw_cmd_one(v, SVGA_3D_CMD_DX_DESTROY_CONTEXT, &d, sizeof(d));
		} else if (v->has_gb) {
			SVGA3dCmdDestroyGBContext d = { .cid = c->cid };
			vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_CONTEXT, &d, sizeof(d));
		} else {
			SVGA3dCmdDestroyContext d = { .cid = c->cid };
			vmw_cmd_one(v, SVGA_3D_CMD_CONTEXT_DESTROY, &d, sizeof(d));
		}
		c->defined = 0;
	}
	/* From here the device has no such context.  A view that is still
	 * referenced somewhere sees this and does not name it again. */
	c->res.id = -1;
	(void)vmw_context_bind_dx_query(&c->res, NULL);
	mm_write_unlock(&v->execbuf_lock);
	for (int t = 0; t < SVGA_COTABLE_MAX; t++)
		if (c->cot[t].bo) {
			drm_gem_put(c->cot[t].bo);
			c->cot[t].bo = NULL;
		}
	if (c->shader_bo) {
		drm_gem_put(c->shader_bo);
		c->shader_bo = NULL;
	}
	if (c->state_bo) {
		drm_gem_put(c->state_bo);
		c->state_bo = NULL;
	}
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	vmw_id_free(v->context_ids, c->cid);
	spin_unlock_irqrestore(&v->id_lock, fl);
	/* The file's reference.  Normally the last one, and the memory goes
	 * with it (vmw_context_res_free()). */
	struct vmw_resource *res = &c->res;

	vmw_resource_unreference(&res);
}

/* ---- per-file context / shader tables ----------------------------------- */

static struct vmw_file *vmw_file_of(struct drm_file *fp)
{
	if (!fp->priv) {
		struct vmw_file *f = kalloc(sizeof(*f));
		if (!f)
			return NULL;
		mm_memset(f, 0, sizeof(*f));
		fp->priv = f;
	}
	return fp->priv;
}

struct vmw_context *vmw_file_context(struct drm_file *fp, uint32_t cid)
{
	struct vmw_file *f = fp->priv;
	if (!f)
		return NULL;
	for (uint32_t i = 0; i < ARRAY_SIZE(f->contexts); i++)
		if (f->contexts[i] && f->contexts[i]->cid == cid)
			return f->contexts[i];
	return NULL;
}

long vmw_ioctl_create_context(struct vmw_device *v, struct drm_file *fp,
			      int dx, int32_t *cid_out)
{
	struct vmw_file *f = vmw_file_of(fp);
	if (!f)
		return -ENOMEM;
	int slot = -1;
	for (int i = 0; i < (int)ARRAY_SIZE(f->contexts); i++)
		if (!f->contexts[i]) {
			slot = i;
			break;
		}
	if (slot < 0)
		return -ENOSPC;
	struct vmw_context *c;
	int rc = vmw_context_create(v, fp, dx, &c);
	if (rc)
		return rc;
	f->contexts[slot] = c;
	*cid_out = (int32_t)c->cid;
	return 0;
}

long vmw_ioctl_unref_context(struct vmw_device *v, struct drm_file *fp, uint32_t cid)
{
	struct vmw_file *f = fp->priv;
	if (!f)
		return -EINVAL;
	for (uint32_t i = 0; i < ARRAY_SIZE(f->contexts); i++) {
		if (f->contexts[i] && f->contexts[i]->cid == cid) {
			struct vmw_context *c = f->contexts[i];
			f->contexts[i] = NULL;
			vmw_context_destroy(v, c);
			return 0;
		}
	}
	return -EINVAL;
}

void vmw_file_release(struct vmw_device *v, struct drm_file *fp)
{
	struct vmw_file *f = fp->priv;
	if (!f)
		return;
	for (uint32_t i = 0; i < ARRAY_SIZE(f->contexts); i++)
		if (f->contexts[i]) {
			struct vmw_context *c = f->contexts[i];
			f->contexts[i] = NULL;
			vmw_context_destroy(v, c);
		}
	for (int i = 0; i < (int)ARRAY_SIZE(f->shaders); i++)
		if (f->shaders[i]) {
			SVGA3dCmdDestroyGBShader d = { .shid = f->shaders[i] };
			vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_SHADER, &d, sizeof(d));
			uint64_t fl;
			spin_lock_irqsave(&v->id_lock, &fl);
			vmw_id_free(v->shader_ids, f->shaders[i]);
			spin_unlock_irqrestore(&v->id_lock, fl);
			f->shaders[i] = 0;
		}
	kfree(f);
	fp->priv = NULL;
}

/* Legacy GB shaders (non-DX hosts): DEFINE_GB_SHADER + BIND to a MOB. */
long vmw_ioctl_create_shader(struct vmw_device *v, struct drm_file *fp,
			     struct drm_vmw_shader_create_arg *a)
{
	struct vmw_file *f = vmw_file_of(fp);
	if (!f)
		return -ENOMEM;
	if (!v->has_gb)
		return -ENODEV;

	SVGA3dShaderType shader_type;

	switch (a->shader_type) {
	case drm_vmw_shader_type_vs:
		shader_type = SVGA3D_SHADERTYPE_VS;
		break;
	case drm_vmw_shader_type_ps:
		shader_type = SVGA3D_SHADERTYPE_PS;
		break;
	default:
		VMW_DEBUG_USER("Illegal shader type.\n");
		return -EINVAL;
	}

	/* The code has to lie inside the buffer it is said to be in: the
	 * device reads `size' bytes from `offset' of that MOB. */
	struct drm_gem_object *bo = NULL;

	if (a->buffer_handle != SVGA3D_INVALID_ID) {
		uint64_t end = (uint64_t)a->size + a->offset;

		bo = drm_gem_lookup(fp, a->buffer_handle);
		if (!bo || bo->kind != DRM_GEM_BO) {
			VMW_DEBUG_USER("Couldn't find buffer for shader creation.\n");
			if (bo)
				drm_gem_put(bo);
			return -EINVAL;
		}
		if (end < a->offset || end > bo->size) {
			VMW_DEBUG_USER("Illegal buffer- or shader size.\n");
			drm_gem_put(bo);
			return -EINVAL;
		}
	}

	/* A slot in the file's table first: a shader the file cannot record
	 * is one nothing would ever destroy, and its id would be lost to the
	 * device for good. */
	int slot = -1;

	for (int i = 0; i < (int)ARRAY_SIZE(f->shaders); i++)
		if (!f->shaders[i]) {
			slot = i;
			break;
		}
	if (slot < 0) {
		if (bo)
			drm_gem_put(bo);
		return -ENOSPC;
	}

	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	int shid = vmw_id_alloc(v->shader_ids, VMW_NUM_SHADERS);
	spin_unlock_irqrestore(&v->id_lock, fl);
	if (shid < 0) {
		if (bo)
			drm_gem_put(bo);
		return -ENOSPC;
	}
	SVGA3dCmdDefineGBShader d;
	d.shid = (uint32_t)shid;
	d.type = shader_type;
	d.sizeInBytes = a->size;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SHADER, &d, sizeof(d));
	if (rc) {
		spin_lock_irqsave(&v->id_lock, &fl);
		vmw_id_free(v->shader_ids, (uint32_t)shid);
		spin_unlock_irqrestore(&v->id_lock, fl);
		if (bo)
			drm_gem_put(bo);
		return rc;
	}
	if (bo) {
		SVGA3dCmdBindGBShader b;
		b.shid = (uint32_t)shid;
		b.mobid = bo_mobid(bo);
		b.offsetInBytes = (uint32_t)a->offset;
		vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_SHADER, &b, sizeof(b));
		drm_gem_put(bo);
	}
	/* Shader id 0 is reserved at bring-up (vmw_mob.c), so 0 can mark a
	 * free slot. */
	f->shaders[slot] = (uint32_t)shid;
	a->shader_handle = (uint32_t)shid;
	return 0;
}

long vmw_ioctl_unref_shader(struct vmw_device *v, struct drm_file *fp, uint32_t shid)
{
	struct vmw_file *f = fp->priv;
	if (!f)
		return -EINVAL;
	if (shid == 0)
		return -EINVAL;
	for (int i = 0; i < (int)ARRAY_SIZE(f->shaders); i++) {
		if (f->shaders[i] == shid) {
			SVGA3dCmdDestroyGBShader d = { .shid = shid };
			vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_SHADER, &d, sizeof(d));
			uint64_t fl;
			spin_lock_irqsave(&v->id_lock, &fl);
			vmw_id_free(v->shader_ids, shid);
			spin_unlock_irqrestore(&v->id_lock, fl);
			f->shaders[i] = 0;
			return 0;
		}
	}
	return -EINVAL;
}
