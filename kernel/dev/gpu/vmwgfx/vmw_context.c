// LikeOS-64 -- vmwgfx: rendering contexts and legacy shaders.
//
// A DX context is where all VGPU10 state lives; its object tables
// (COTables: views, states, shaders, queries...) are MOB-backed buffers
// the driver grows as userspace's ids climb.  Legacy GB contexts and
// shaders exist for hosts without DX.
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
 *   buffer, and validSizeInBytes on the switch is the ENTIRE old size --
 *   exactly what the reference implementation does (its readback records
 *   the full table size and its resize copies every page of the old
 *   buffer).  Copying any tracked prefix instead throws away the entries
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
		 * objects defined in it, and then every draw that uses them.
		 * The reference implementation waits for the same thing at
		 * this point, on the buffer itself. */
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
	int rc;
	if (v->has_gb) {
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
			 * (validated in vmw_execbuf.c), exactly as the
			 * reference driver expects -- there, the kernel only
			 * remembers which MOB that was so it can re-bind it
			 * with DX_BIND_ALL_QUERY when a context it had
			 * evicted comes back.
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

void vmw_context_destroy(struct vmw_device *v, struct vmw_context *c)
{
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
	}
	for (int t = 0; t < SVGA_COTABLE_MAX; t++)
		if (c->cot[t].bo)
			drm_gem_put(c->cot[t].bo);
	if (c->shader_bo)
		drm_gem_put(c->shader_bo);
	if (c->state_bo)
		drm_gem_put(c->state_bo);
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	vmw_id_free(v->context_ids, c->cid);
	spin_unlock_irqrestore(&v->id_lock, fl);
	kfree(c);
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
	for (int i = 0; i < 64; i++)
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
	for (int i = 0; i < 64; i++)
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
	for (int i = 0; i < 64; i++) {
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
	for (int i = 0; i < 64; i++)
		if (f->contexts[i]) {
			struct vmw_context *c = f->contexts[i];
			f->contexts[i] = NULL;
			vmw_context_destroy(v, c);
		}
	for (int i = 0; i < 256; i++)
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
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	int shid = vmw_id_alloc(v->shader_ids, VMW_NUM_SHADERS);
	spin_unlock_irqrestore(&v->id_lock, fl);
	if (shid < 0)
		return -ENOSPC;
	SVGA3dCmdDefineGBShader d;
	d.shid = (uint32_t)shid;
	d.type = a->shader_type == drm_vmw_shader_type_vs ? SVGA3D_SHADERTYPE_VS :
							   SVGA3D_SHADERTYPE_PS;
	d.sizeInBytes = a->size;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SHADER, &d, sizeof(d));
	if (rc) {
		vmw_id_free(v->shader_ids, (uint32_t)shid);
		return rc;
	}
	if (a->buffer_handle != SVGA3D_INVALID_ID) {
		struct drm_gem_object *bo = drm_gem_lookup(fp, a->buffer_handle);
		if (bo) {
			SVGA3dCmdBindGBShader b;
			b.shid = (uint32_t)shid;
			b.mobid = bo_mobid(bo);
			b.offsetInBytes = (uint32_t)a->offset;
			vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_SHADER, &b, sizeof(b));
			drm_gem_put(bo);
		}
	}
	for (int i = 0; i < 256; i++)
		if (!f->shaders[i]) {
			f->shaders[i] = (uint32_t)shid;
			break;
		}
	a->shader_handle = (uint32_t)shid;
	return 0;
}

long vmw_ioctl_unref_shader(struct vmw_device *v, struct drm_file *fp, uint32_t shid)
{
	struct vmw_file *f = fp->priv;
	if (!f)
		return -EINVAL;
	for (int i = 0; i < 256; i++) {
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
