// LikeOS -- vmwgfx: command stream submission.
//
// Userspace builds SVGA3D command streams naming its OWN handles: a surface
// handle where the device wants a surface id, a buffer handle where it
// wants a MOB or GMR id.  The device must never see a handle, and it must
// never see an object of another process, so every command is checked
// against a table that says where its ids sit and of what kind, the ids
// are rewritten to the device's, and each object touched is pinned until
// the fence the submission ends with has passed.  A command not in the
// table is refused: the stream is untrusted input.
//
// The table and the per-command checks live in vmw_execbuf_cmds.c.  This
// file walks the stream, owns the pinning and relocation helpers the checks
// use, and keeps the books a stream changes: the bindings it gives its
// contexts (staged while the stream is verified, committed to each
// context's tracker only once the stream has gone to the device) and the
// views it defines and destroys (staged in the context's manager the same
// way, reverted when the stream is refused).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/vmw_execbuf.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>
#include <kernel/hal/lapic.h>

/* Distinct objects one submission may touch.
 *
 * This was a fixed 256, and 256 is BELOW what the client is entitled to
 * send.  Mesa's winsys flushes its command buffer only when one of its
 * relocation arrays fills, and those are 1024 surface relocations, 1024
 * shader and 512 region (VMW_SURFACE_RELOCS and friends in
 * winsys/svga/drm/vmw_context.c) -- so a single batch can legitimately name
 * up to 1024 surfaces plus 512 buffers.  Past 256 distinct objects,
 * val_obj() returned NULL for want of a slot and every reloc_* turned that
 * into -EINVAL, failing the ioctl.
 *
 * That is a submission the client built correctly and the kernel refused
 * for its own reasons, and Mesa answers an execbuf error by abandoning the
 * batch and carrying on with state the device never received -- so the
 * damage surfaced later and elsewhere (a NULL surface handle, a freed
 * buffer still on a list) with nothing pointing back here.  Rare, because
 * it needs a frame complex enough to name 256 DISTINCT objects before the
 * winsys flushes on its own; ordinary pages never come close.
 *
 * The array is grown as it fills rather than reserved at full size: the
 * common submission touches a handful of objects, and a fixed full-size
 * array would be tens of kilobytes of kalloc on every single execbuf.
 *
 * The cap is the client's own worst case with headroom.  Mesa cannot carry
 * more relocations than its three arrays hold -- 1024 surface, 1024 shader,
 * 512 region -- so 2560 distinct objects is the most any conforming batch
 * can name, and 4096 puts the limit out of a correct client's reach
 * entirely.  Reaching it costs 32K, and only on a batch that would
 * previously have been refused outright. */
#define VMW_REFS_INITIAL 64
#define VMW_MAX_REFS 4096

/* struct vmw_val: vmw_execbuf.h */

/* Objects come from the slab allocator, so the low bits are alignment and
 * carry nothing; the rest goes through a multiply by the 64-bit golden
 * ratio, whose high word mixes every input bit into the index. */
static uint32_t val_hash(const struct drm_gem_object *o)
{
	uint64_t x = (uint64_t)(uintptr_t)o >> 4;

	x *= 0x9E3779B97F4A7C15ULL;
	return (uint32_t)(x >> 32);
}

/* Is this object already pinned by this submission? */
static int val_seen(struct vmw_val *val, struct drm_gem_object *o)
{
	if (!val->htab)
		return 0;
	for (uint32_t i = val_hash(o) & val->hmask; val->htab[i];
	     i = (i + 1) & val->hmask)
		if (val->htab[i] == o)
			return 1;
	return 0;
}

/* Record it.  Only ever called with room to spare -- see the load factor. */
static void val_seen_add(struct vmw_val *val, struct drm_gem_object *o)
{
	uint32_t i = val_hash(o) & val->hmask;

	while (val->htab[i])
		i = (i + 1) & val->hmask;
	val->htab[i] = o;
}

/* Room for one more reference, growing the array if that is what it takes.
 * Returns 0 when there is space. */
static int val_refs_room(struct vmw_val *val)
{
	if (val->nrefs < val->refs_cap)
		return 0;
	if (val->refs_cap >= VMW_MAX_REFS)
		return -ENOSPC;

	uint32_t cap = val->refs_cap ? val->refs_cap * 2 : VMW_REFS_INITIAL;
	if (cap > VMW_MAX_REFS)
		cap = VMW_MAX_REFS;

	struct drm_gem_object **n =
		krealloc(val->refs, (size_t)cap * sizeof(*n));
	if (!n)
		return -ENOMEM;
	val->refs = n;

	/* The set is REBUILT, not grown: open addressing puts an entry where
	 * the mask says to, and a wider mask says somewhere else. */
	uint32_t hsize = cap * 2;
	struct drm_gem_object **h = kalloc((size_t)hsize * sizeof(*h));
	if (!h)
		return -ENOMEM; /* refs_cap not committed; the next call retries */
	mm_memset(h, 0, (size_t)hsize * sizeof(*h));
	kfree(val->htab);
	val->htab = h;
	val->hmask = hsize - 1;
	val->refs_cap = cap;
	for (uint32_t i = 0; i < val->nrefs; i++)
		val_seen_add(val, val->refs[i]);
	return 0;
}

/* Find (and pin for the submission) the object behind a handle. */
static struct drm_gem_object *val_obj(struct vmw_val *val, uint32_t handle,
				      enum drm_gem_kind kind)
{
	if (handle == SVGA3D_INVALID_ID)
		return NULL;
	struct drm_gem_object *o = drm_gem_lookup(val->fp, handle);
	if (!o)
		return NULL;
	if (o->kind != kind) {
		drm_gem_put(o);
		return NULL;
	}
	/* Already pinned? then drop the extra reference. */
	if (val_seen(val, o)) {
		drm_gem_put(o);
		return o;
	}
	if (val_refs_room(val) != 0) {
		drm_gem_put(o);
		return NULL;
	}
	val->refs[val->nrefs++] = o; /* keeps the reference */
	val_seen_add(val, o);
	return o;
}

/* Rewrite a surface handle field to the device sid; *srf_out (when asked
 * for) is the surface, or NULL for SVGA3D_INVALID_ID. */
static int reloc_sid(struct vmw_val *val, uint32_t *field,
		     struct vmw_surface **srf_out)
{
	if (srf_out)
		*srf_out = NULL;
	if (*field == SVGA3D_INVALID_ID)
		return 0;
	struct drm_gem_object *o = val_obj(val, *field, DRM_GEM_SURFACE);
	if (!o)
		return -EINVAL;
	struct vmw_surface *s = o->priv;
	if (!s->bound && s->backup)
		vmw_surface_bind(val->v, s);
	*field = s->sid;
	if (srf_out)
		*srf_out = s;
	return 0;
}

/* Rewrite a buffer handle field to the MOB id. */
static int reloc_mob(struct vmw_val *val, uint32_t *field,
		     struct drm_gem_object **obj_out)
{
	if (obj_out)
		*obj_out = NULL;
	if (*field == SVGA3D_INVALID_ID)
		return 0;
	struct drm_gem_object *o = val_obj(val, *field, DRM_GEM_BO);
	if (!o)
		return -EINVAL;
	struct vmw_bo *b = o->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID)
		return -EINVAL;
	*field = b->mob.id;
	if (obj_out)
		*obj_out = o;
	return 0;
}

/* Rewrite a buffer handle in a guest pointer to the GMR id. */
static int reloc_gmr(struct vmw_val *val, uint32_t *gmr_field,
		     struct drm_gem_object **obj_out)
{
	if (obj_out)
		*obj_out = NULL;
	if (*gmr_field == SVGA_GMR_NULL || *gmr_field == SVGA_GMR_FRAMEBUFFER)
		return 0;
	struct drm_gem_object *o = val_obj(val, *gmr_field, DRM_GEM_BO);
	if (!o)
		return -EINVAL;
	struct vmw_bo *b = o->priv;
	if (!b || b->gmr_id < 0)
		return -EINVAL;
	*gmr_field = (uint32_t)b->gmr_id;
	if (obj_out)
		*obj_out = o;
	return 0;
}

static int check_cid(struct vmw_val *val, uint32_t cid)
{
	/* Legacy commands carry the context id; it must be one of this
	 * file's.  DX streams carry it in the buffer header instead. */
	if (val->ctx)
		return 0;
	return vmw_file_context(val->fp, cid) ? 0 : -EINVAL;
}

static int cot(struct vmw_val *val, int type, uint32_t id)
{
	if (!val->ctx)
		return -EINVAL;
	if (id == SVGA3D_INVALID_ID)
		return 0;
	if (id > 65000)
		return -EINVAL;
	return vmw_context_cotable_reserve(val->v, val->ctx, type, id);
}

/* The helpers above, for the command table in vmw_execbuf_cmds.c. */
struct drm_gem_object *vmw_val_obj(struct vmw_val *val, uint32_t handle,
				   enum drm_gem_kind kind)
{
	return val_obj(val, handle, kind);
}

int vmw_val_reloc_sid(struct vmw_val *val, uint32_t *field)
{
	return reloc_sid(val, field, NULL);
}

int vmw_val_reloc_mob(struct vmw_val *val, uint32_t *field)
{
	return reloc_mob(val, field, NULL);
}

int vmw_val_reloc_gmr(struct vmw_val *val, uint32_t *gmr_field)
{
	return reloc_gmr(val, gmr_field, NULL);
}

int vmw_val_reloc_sid_srf(struct vmw_val *val, uint32_t *field,
			  struct vmw_surface **srf_out)
{
	return reloc_sid(val, field, srf_out);
}

int vmw_val_reloc_mob_obj(struct vmw_val *val, uint32_t *field,
			  struct drm_gem_object **obj_out)
{
	return reloc_mob(val, field, obj_out);
}

int vmw_val_reloc_gmr_obj(struct vmw_val *val, uint32_t *gmr_field,
			  struct drm_gem_object **obj_out)
{
	return reloc_gmr(val, gmr_field, obj_out);
}

int vmw_val_check_cid(struct vmw_val *val, uint32_t cid)
{
	return check_cid(val, cid);
}

int vmw_val_cotable(struct vmw_val *val, int type, uint32_t id)
{
	return cot(val, type, id);
}

/* ---- the books a stream keeps --------------------------------------------
 *
 * Bindings.  Every context the stream touches gets a node, and a node whose
 * context has a binding tracker gets a STAGED binding state: the commands
 * record what they bind there, and only a stream that actually went to the
 * device has its staged bindings moved into the context's tracker
 * (vmw_execbuf_commit_bindings()).  A refused stream resets the staged
 * state and leaves the tracker as it was.
 *
 * A staged state is tens of kilobytes, and nearly every submission stages
 * into exactly one context, so the device keeps ONE, allocated on first
 * use and lent to the first node of each submission under execbuf_lock;
 * only a stream that touches several tracked contexts (a legacy stream
 * naming more than one context id) allocates more, for the extra nodes.
 *
 * Views.  Defines and destroys are staged in the DX context's manager on
 * val->staged_cmd_res, committed after a successful submission, reverted
 * on every other path.
 *
 * Destroys of views that the device may have lost in the meantime: a view
 * is destroyed by the driver itself when its surface goes, and the client
 * does not know that.  Its own destroy of the same view is then a second
 * destroy the device refuses -- so such a command is noted here and turned
 * into a NOP at submission if, by then, the view's id has been given up
 * (vmw_execbuf_apply_nops()). */

/* The device's staged state for the first node, or a fresh one. */
static struct vmw_ctx_binding_state *val_staged_get(struct vmw_val *val,
						    bool *owned)
{
	struct vmw_device *v = val->v;
	struct vmw_ctx_binding_state *s;

	*owned = false;
	if (!val->staged_inuse) {
		if (!v->execbuf_staged) {
			s = vmw_binding_state_alloc(v);
			if (IS_ERR_OR_NULL(s))
				return NULL;
			v->execbuf_staged = s;
		}
		val->staged_inuse = true;
		return v->execbuf_staged;
	}
	s = vmw_binding_state_alloc(v);
	if (IS_ERR_OR_NULL(s))
		return NULL;
	*owned = true;
	return s;
}

struct vmw_val_ctx *vmw_val_ctx_node(struct vmw_val *val,
				     struct vmw_context *ctx)
{
	struct vmw_val_ctx *node;

	if (!ctx)
		return NULL;
	list_for_each_entry(node, &val->ctx_list, head)
		if (node->ctx == ctx)
			return node;

	/* The context is looked up without a lock; one that is already on
	 * its way out gets no node, and the stream goes as it always did. */
	if (!vmw_resource_reference_unless_doomed(&ctx->res))
		return NULL;

	if (!val->dx_node_storage.ctx) {
		node = &val->dx_node_storage;
	} else {
		node = kalloc(sizeof(*node));
		if (!node) {
			struct vmw_resource *res = &ctx->res;

			vmw_resource_unreference(&res);
			return NULL;
		}
	}
	mm_memset(node, 0, sizeof(*node));
	node->ctx = ctx;
#if VMW_TRACK_VIEWS
	/* Only a context with a tracker has bindings worth staging; without
	 * one the stream is verified and submitted exactly as before.  A
	 * staged state that cannot be had is the same as no tracker: the
	 * submission is not refused for the driver's own bookkeeping. */
	if (val->v->has_gb && !IS_ERR_OR_NULL(ctx->cbs))
		node->staged = val_staged_get(val, &node->staged_owned);
#endif
	list_add_tail(&node->head, &val->ctx_list);
	return node;
}

int vmw_val_cond_nop_add(struct vmw_val *val, uint32_t *id_loc,
			 struct vmw_resource *res)
{
	if (val->nnops == val->nops_cap) {
		uint32_t cap = val->nops_cap ? val->nops_cap * 2 : 8;
		struct vmw_val_nop *n =
			krealloc(val->nops, (size_t)cap * sizeof(*n));

		if (!n)
			return -ENOMEM;
		val->nops = n;
		val->nops_cap = cap;
	}
	val->nops[val->nnops].offset =
		(uint32_t)((uint8_t *)id_loc - val->buf_start);
	val->nops[val->nnops].res = res;
	val->nnops++;
	return 0;
}

/* Does this submission have any books to commit under the binding lock? */
static bool val_needs_binding_lock(const struct vmw_val *val)
{
	const struct vmw_val_ctx *node;

	if (val->nnops || !list_empty(&val->staged_cmd_res))
		return true;
	list_for_each_entry(node, &val->ctx_list, head)
		if (node->staged)
			return true;
	return false;
}

/* Turn the conditional destroys into NOPs where the object is gone.
 * `stream' is where the client's commands start in the buffer about to be
 * submitted.  Caller holds binding_lock, which is what destroying a view
 * takes to give up its id, so the answer holds until the stream is in. */
static void vmw_execbuf_apply_nops(struct vmw_val *val, uint8_t *stream)
{
	for (uint32_t i = 0; i < val->nnops; i++) {
		struct vmw_val_nop *n = &val->nops[i];

		if (n->res->id == -1)
			*(uint32_t *)(stream + n->offset) = SVGA_3D_CMD_NOP;
	}
}

/* The stream is in: move every node's staged bindings into its context's
 * tracker.  Caller holds binding_lock for writing.  The tracker is read
 * here, under the lock, because a context being destroyed gives it up
 * under the same lock. */
static void vmw_execbuf_commit_bindings(struct vmw_val *val)
{
	struct vmw_val_ctx *node;

	list_for_each_entry(node, &val->ctx_list, head) {
		struct vmw_ctx_binding_state *cbs;

		if (!node->staged)
			continue;
		cbs = node->ctx->cbs;
		if (!IS_ERR_OR_NULL(cbs))
			vmw_binding_state_commit(cbs, node->staged);
	}
}

/* End of the submission, committed or not: give back the staged states
 * (reset, so what a refused stream staged goes nowhere) and the context
 * references.  Not under binding_lock: dropping a context's last
 * reference releases it, and that takes the lock. */
static void vmw_execbuf_release_nodes(struct vmw_val *val)
{
	struct vmw_val_ctx *node, *next;

	list_for_each_entry_safe(node, next, &val->ctx_list, head) {
		struct vmw_resource *res = &node->ctx->res;

		list_del(&node->head);
		if (node->staged) {
			if (node->staged_owned)
				vmw_binding_state_free(node->staged);
			else
				vmw_binding_state_reset(node->staged);
		}
		vmw_resource_unreference(&res);
		if (node != &val->dx_node_storage)
			kfree(node);
	}
	val->dx_node = NULL;
	val->staged_inuse = false;
}

/* Name a rejected command, once per (command, error).
 *
 * The refusal itself stays where client errors belong -- in the ioctl's
 * return value -- and the walk below still does not log per call.  But a
 * bare EINVAL out of a stream of a hundred commands says only that ONE of
 * some thirty checks in the verifier fired, and Mesa answers it by
 * abandoning the submission ("vmw_ioctl_command error Invalid argument")
 * and carrying on with state the device never received, so the crash that
 * follows is somewhere else entirely and names nothing.
 *
 * The command id narrows thirty checks to the one or two that command has.
 * Deduplicated on (id, rc) so a client looping on a command it cannot use
 * costs one line, not a console: the same bargain drm_drv.c strikes for
 * the ioctl number itself. */
static void vmw_execbuf_note_reject(uint32_t id, uint32_t bsize, int rc)
{
	static uint64_t seen[64];
	static unsigned nseen;
	uint64_t key = ((uint64_t)id << 32) | (uint32_t)(-rc);

	for (unsigned i = 0; i < nseen && i < 64; i++)
		if (seen[i] == key)
			return;
	if (nseen >= 64)
		return;
	seen[nseen++] = key;
	kprintf("vmwgfx: execbuf refused cmd id=%u (0x%x) body=%u bytes: %d for pid %d\n",
		(unsigned)id, (unsigned)id, (unsigned)bsize, rc,
		sched_current() ? (int)sched_current()->id : -1);
}

/* Put back the dirt of every coherent surface this submission emitted for.
 *
 * vmw_surface_dirty_emit() clears each box as it writes the command that
 * carries it, on the understanding that the command is about to run.  When
 * the batch does not run after all, that understanding is wrong and the
 * writes those boxes described are lost: the device keeps the bytes it had,
 * and nothing marks them again until the content is redrawn from scratch. */
static void execbuf_restore_dirt(struct vmw_val *val)
{
	for (uint32_t i = 0; i < val->nrefs; i++) {
		struct drm_gem_object *o = val->refs[i];
		struct vmw_surface *cs =
			o->kind == DRM_GEM_SURFACE ? o->priv : NULL;

		if (cs && cs->coherent && cs->defined && cs->backup)
			vmw_surface_dirty_mark_all(cs);
	}
}

static int vmw_execbuf_do(struct vmw_device *v, struct drm_file *fp,
			  struct drm_vmw_execbuf_arg *a)
{
	struct drm_vmw_fence_rep rep;
	int rc = 0;
	/* Whether the stream's staged books were made permanent; every path
	 * that leaves without doing so reverts them at `out'. */
	bool committed = false;

	if (a->command_size > 1024 * 1024)
		return -EINVAL;
	if (a->command_size & 3)
		return -EINVAL;
	if (!v->has_3d)
		return -ENODEV;


	struct vmw_val *val = kalloc(sizeof(*val));
	if (!val)
		return -ENOMEM;
	/* Everything from here to `out' rewrites shared state -- the
	 * context's object tables above all.  See execbuf_lock. */
	/* Timed on its own.  This is a WRITE lock across the whole ioctl, so
	 * every client submitting to this device -- the display server's
	 * acceleration and the browser's web process at once -- takes turns
	 * through it, and a queue here is invisible in every other stage. */
	{

		mm_write_lock(&v->execbuf_lock);
	}
	mm_memset(val, 0, sizeof(*val));
	val->v = v;
	val->fp = fp;
	INIT_LIST_HEAD(&val->ctx_list);
	INIT_LIST_HEAD(&val->staged_cmd_res);
	uint32_t dx_cid = SVGA3D_INVALID_ID;
	if (a->version >= 2 && a->context_handle != SVGA3D_INVALID_ID) {
		val->ctx = vmw_file_context(fp, a->context_handle);
		if (!val->ctx || !val->ctx->dx) {
			mm_write_unlock(&v->execbuf_lock);
			kfree(val);
			return -EINVAL;
		}
		dx_cid = val->ctx->cid;
		/* The stream's DX context: its bindings are staged on this
		 * node, its views in its manager. */
		val->dx_node = vmw_val_ctx_node(val, val->ctx);
#if VMW_TRACK_VIEWS
		if (val->dx_node && !IS_ERR_OR_NULL(val->ctx->man))
			val->man = val->ctx->man;
#endif
	}

	uint8_t *cmds = NULL;  /* the client's stream, rewritten in place */
	uint8_t *batch = NULL; /* stream with coherent updates ahead of it */
	if (a->command_size) {
		cmds = kalloc(a->command_size);
		if (!cmds) {
			rc = -ENOMEM;
			goto out;
		}
		if (!validate_user_ptr(a->commands, a->command_size) ||
		    copy_from_user(cmds, (void *)(uintptr_t)a->commands,
				   a->command_size) != 0) {
			rc = -EFAULT;
			goto out;
		}
		/* Split out, because "validate" covering both cannot say
		 * whether the cost is the copy of the stream or the walk of
		 * it, and those have completely different answers. */
		/* Walk and rewrite. */
		val->buf_start = cmds;
		uint32_t off = 0;
		while (off < a->command_size) {
			uint32_t csize = 0;

			rc = vmw_execbuf_cmd_check(val, cmds + off,
						   a->command_size - off,
						   &csize);
			if (rc) {
				/* The refusal is a CLIENT error and is
				 * reported where those belong, in the ioctl's
				 * return value.  What goes to the console is
				 * only WHICH command was refused, once per
				 * (command, error) -- see the note above the
				 * reporter.  Logging every rejected handle
				 * filled the console the moment anything
				 * probed the interface; this cannot. */
				if (a->command_size - off >=
				    sizeof(SVGA3dCmdHeader)) {
					SVGA3dCmdHeader *h =
						(SVGA3dCmdHeader *)(cmds + off);

					vmw_execbuf_note_reject(h->id, h->size,
								rc);
				}
				goto out;
			}
			off += csize;
		}
	}

	/* Wait for an imported fence first. */
	if ((a->flags & DRM_VMW_EXECBUF_FLAG_IMPORT_FENCE_FD) && a->imported_fence_fd >= 0) {
		struct drm_fence *f = drm_fence_from_fd(a->imported_fence_fd);
		if (f) {
			/* Uninterruptible: the batch below is submitted either
			 * way, and it may only run once this fence has passed.
			 * Nothing here can report a refusal to the caller. */
			drm_fence_wait_flags(f, 2000000000ULL, 0);
			drm_fence_put(f);
		}
	}

	/* Coherent surfaces referenced by this submission: their backing is
	 * persistently mapped by the client, which by contract sends NO
	 * update commands for it -- the kernel makes the CPU's writes
	 * visible.  Write-protecting the mapping, collecting the dirtied
	 * boxes and emitting updates for exactly those at every submission
	 * that references the surface is the precise way; where that
	 * machinery is not available the whole surface is treated as dirty
	 * instead, which transfers more bytes to the same effect.
	 * The update must reach the device BEFORE the commands that read
	 * the data; both go through the one submission channel, which
	 * orders them.  The objects are pinned in refs[] until after
	 * submission, so the ids cannot be recycled underneath this.
	 *
	 * The other direction -- device writes a coherent surface, CPU
	 * reads the mapping -- would need a readback here after the fence;
	 * nothing exercises it yet (stream-output readers would). */
	/* Coherent surfaces this submission references: their backing is
	 * persistently mapped by the client, which by contract sends NO
	 * update commands for it -- making the processor's writes visible to
	 * the device is the kernel's job, and this is where it happens.
	 *
	 * Three steps per surface, in an order that is load-bearing:
	 *
	 *   scan  -- harvest the processor's record of writes into the page
	 *            tracker.  For a fault-tracked buffer this also takes
	 *            the write bit back from every page handed out since
	 *            the last submission, so the record consumed below is
	 *            complete: any later write faults and lands in the NEXT
	 *            submission's record.
	 *   pull  -- translate the harvested pages into per-subresource
	 *            boxes on the surface (vmw_dirty.c).
	 *   emit  -- turn the boxes into update commands, placed AHEAD of
	 *            the client's stream in one buffer: one submission is
	 *            two register writes that leave the virtual machine,
	 *            and the same channel orders the update before every
	 *            command that reads the data.
	 *
	 * If the buffer for the combined stream cannot be had, the boxes
	 * keep their dirt -- emit is what clears them -- and the next
	 * submission says everything this one could not. */
	uint32_t pre = 0;
	uint8_t *start = cmds;
	uint32_t nboxes = 0;


	for (uint32_t i = 0; i < val->nrefs; i++) {
		struct drm_gem_object *o = val->refs[i];
		struct vmw_surface *cs =
			o->kind == DRM_GEM_SURFACE ? o->priv : NULL;

		if (!cs || !cs->coherent || !cs->defined || !cs->backup)
			continue;
		drm_gem_dirty_scan(cs->backup);
		vmw_surface_dirty_pull(cs);
		nboxes += vmw_surface_dirty_count(cs);
	}
	if (nboxes) {
		batch = kalloc((size_t)nboxes * VMW_SURF_DIRTY_CMD_MAX +
			       a->command_size);
		if (!batch) {
			rc = -ENOMEM;
			goto out;
		}
		for (uint32_t i = 0; i < val->nrefs; i++) {
			struct drm_gem_object *o = val->refs[i];
			struct vmw_surface *cs =
				o->kind == DRM_GEM_SURFACE ? o->priv : NULL;

			if (!cs || !cs->coherent || !cs->defined ||
			    !cs->backup)
				continue;
			pre += vmw_surface_dirty_emit(
				v, cs, batch + pre,
				(uint32_t)(nboxes * VMW_SURF_DIRTY_CMD_MAX -
					   pre));
		}
		if (a->command_size)
			mm_memcpy(batch + pre, cmds, a->command_size);
		start = batch;
	}

	/* The books this stream keeps are committed under binding_lock,
	 * taken around the hand-over itself: what the conditional NOPs
	 * decide about a view (gone or not) must still be true when the
	 * stream reaches the device, and a surface destroyed meanwhile gives
	 * up its views under this lock.  Only a stream that has such books
	 * takes it at all. */
	bool locked_bindings = val_needs_binding_lock(val);

	if (locked_bindings) {
		mm_write_lock(&v->binding_lock);
		if (a->command_size)
			vmw_execbuf_apply_nops(val, start + pre);
	}

	/* Asynchronous: the caller's fence is what tells it when the work is
	 * done, and it is submitted through the same channel just below, so
	 * it cannot pass before this batch.  Waiting here instead put every
	 * client's rendering thread to sleep for the host's execution time
	 * of every batch it submitted. */
	if (rc == 0 && (a->command_size || pre)) {
		rc = vmw_cmd_submit_async(v, start, a->command_size + pre, dx_cid);
	}
	/* A DEVICE rejection is not the client's errno.  Execution errors
	 * are never reported through this ioctl -- the commands
	 * were validated and queued, and what the device later makes of them
	 * goes to the log, not to the caller: Mesa's winsys treats any error
	 * here as unrecoverable and calls abort(), so returning the device's
	 * verdict turns one dropped draw into a dead client (and under X,
	 * into the session-wide wreckage of crashing clients).  The channel
	 * has already been restarted by cb_submit_raw(); the batch is lost,
	 * which the log records, and the client draws on.  The kernel's OWN
	 * validation failures above still fail the ioctl -- those mean the
	 * stream never reached the device at all. */
	bool device_rejected = (rc == -EINVAL || rc == -EIO);

	/* The device's verdict on a verified stream is asynchronous in
	 * principle -- whatever of it ran, ran with the bindings and views it
	 * declared -- so the books follow the stream whenever it was handed
	 * over, rejected or not; only a stream that never left (a kernel
	 * error below) is reverted. */
	if (locked_bindings) {
		if (rc == 0 || device_rejected) {
			vmw_execbuf_commit_bindings(val);
			/* In the same hold as the hand-over.  A view this
			 * stream destroyed must be dead (id -1, off its
			 * surface) before binding_lock is let go: a surface
			 * freed right after -- by another thread, or by the
			 * display server dropping a shared buffer -- destroys
			 * the views still on it, and found this one alive,
			 * queueing a second destroy of the same id behind the
			 * client's.  The device rejected that whole command
			 * buffer. */
			vmw_cmdbuf_res_commit_notify(&val->staged_cmd_res);
		}
		mm_write_unlock(&v->binding_lock);
	}
	if (rc == 0 || device_rejected) {
		/* Not under binding_lock: a removed view may be released
		 * here, and its release takes that lock. */
		vmw_cmdbuf_res_commit(&val->staged_cmd_res);
		committed = true;
		if (val->dx_query_mob && val->dx_query_ctx)
			(void)vmw_context_bind_dx_query(&val->dx_query_ctx->res,
							val->dx_query_mob);
	}

	if (device_rejected) {
		static int reported;
		if (reported < 8) {
			reported++;
			kprintf("[drm] vmwgfx: execbuf: device rejected a %u-byte batch (%d); dropped\n",
				a->command_size, rc);
		}
		/* The batch carried this submission's coherent-surface updates
		 * in front of the client's commands, and emit() cleared each
		 * box as it wrote it.  Dropping the batch therefore drops
		 * those updates while the record of them is already gone: the
		 * device keeps the old bytes and nothing asks again, so the
		 * stale region stays until the content itself is redrawn.
		 * Put the dirt back so the next submission re-sends it. */
		if (pre) {
			execbuf_restore_dirt(val);
		}
		rc = 0;
	}
	if (rc) {
		/* Never submitted, so the updates emitted in front of the
		 * client's commands never ran either -- and their boxes are
		 * already cleared. */
		if (pre)
			execbuf_restore_dirt(val);
		goto out;
	}

	struct drm_fence *fence = vmw_fence_emit(v, DRM_VMW_FENCE_FLAG_EXEC | DRM_VMW_FENCE_FLAG_QUERY);
	if (!fence) {
		rc = -ENOMEM;
		goto out;
	}
	/* Every object referenced is busy until this fence. */
	for (uint32_t i = 0; i < val->nrefs; i++) {
		struct drm_gem_object *o = val->refs[i];
		if (o->fence)
			drm_fence_put(o->fence);
		drm_fence_get(fence);
		o->fence = fence;
		if (o->kind == DRM_GEM_SURFACE) {
			struct vmw_surface *s = o->priv;
			if (s && s->backup) {
				if (s->backup->fence)
					drm_fence_put(s->backup->fence);
				drm_fence_get(fence);
				s->backup->fence = fence;
			}
		}
	}
	if (a->fence_rep) {
		mm_memset(&rep, 0, sizeof(rep));
		uint32_t h = 0;
		if (drm_fence_handle_create(fp, fence, &h) == 0)
			rep.handle = h;
		/* ZERO -- not the fence's
		 * flags.  Mesa's winsys reads `mask' the other way round: on
		 * every wait and every "has it passed?" it computes
		 * EXEC & ~mask and treats a result of zero as "nothing left to
		 * wait for" (vmw_fence.c, vmw_fence_finish/vmw_fence_signalled).
		 * Reporting EXEC|QUERY here therefore told userspace that every
		 * fence had passed the moment it was created: no map ever
		 * waited for its readback (the CPU read the previous frame),
		 * and every vertex, index, upload and texture buffer was reused
		 * while the device was still reading it -- the skewed geometry
		 * and misplaced image strips in the browser, and every wrong
		 * paint gltile reports. */
		rep.mask = 0;
		rep.seqno = fence->seqno;
		rep.passed_seqno = v->drm.fence_passed;
		rep.fd = -1;
		if (a->flags & DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD)
			rep.fd = drm_fence_export_fd(fence, 1);
		rep.error = 0;
		if (validate_user_ptr(a->fence_rep, sizeof(rep)))
			copy_to_user((void *)(uintptr_t)a->fence_rep, &rep, sizeof(rep));
	}
	drm_fence_put(fence);
out:
	;

	/* A stream that never reached the device takes back what it staged:
	 * the views it defined or removed go back to how they were (this can
	 * release a view, so it is done without binding_lock), and the
	 * staged bindings are dropped with the nodes just below. */
	if (!committed)
		vmw_cmdbuf_res_revert(&val->staged_cmd_res);
	vmw_execbuf_release_nodes(val);
	/* binding_lock is NOT held here, and must not be: the last reference
	 * to a surface can go with the puts below, and destroying it scrubs
	 * its bindings under that lock. */
	for (uint32_t i = 0; i < val->nrefs; i++)
		drm_gem_put(val->refs[i]);
	if (cmds)
		kfree(cmds);
	if (batch)
		kfree(batch);
	kfree(val->nops);
	kfree(val->refs);
	kfree(val->htab);
	kfree(val);
	mm_write_unlock(&v->execbuf_lock);
	/* A legacy cursor surface the stream wrote into (its SURFACE_DMA was
	 * snooped by the verifier) gets its new image defined now that the
	 * stream is in.  Outside execbuf_lock: it needs only the cursor lock. */
	if (committed && rc == 0)
		vmw_kms_cursor_post_execbuf(v);
	return rc;
}

int vmw_execbuf(struct vmw_device *v, struct drm_file *fp,
		struct drm_vmw_execbuf_arg *a)
{
	return vmw_execbuf_do(v, fp, a);
}
