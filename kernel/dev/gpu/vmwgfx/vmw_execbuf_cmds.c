// LikeOS -- vmwgfx: the per-command verifier.
//
// One table entry per SVGA3D command id says whether a client may send the
// command at all and which handler checks it.  A handler checks the body's
// size against the command's structure, rewrites every handle in it to the
// device id of the object the client owns (vmw_val_reloc_*: a handle that is
// not the client's fails the stream), makes sure the stream's DX context can
// hold the ids it defines, and keeps the books the command changes: the
// bindings it gives the context (staged, see vmw_execbuf.c) and the views it
// defines and destroys.
//
// Two kinds of failure.  What the verifier has always refused -- a short
// body, a handle that is not the client's, a DX command without a DX
// context, a command the kernel keeps for itself -- fails the stream with
// -EINVAL as it always did.  The rules added with the table -- the client
// flags per command, counts against the device's limits, ids of views the
// context never defined, guest pointers past the end of their buffer,
// commands of a shader model the device does not run -- go through
// vmw_cmd_rule(): named once on the console and, unless VMW_EXECBUF_STRICT,
// passed as before (with the bookkeeping that depends on the broken rule
// skipped for that command).
//
// The table accepts every command the driver has always accepted, with the
// same relocation, including the ones a strict reading of the device's
// rules would not list for clients but Mesa sends (DX_PRED_COPY, the
// partial image readback/invalidate, the _V2 vertex and index buffer
// commands and the other later DX additions).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/vmw_execbuf.h>
#include <kernel/ke/sched.h>
#include <kernel/io/console.h>

static const char *vmw_cmd_name(uint32_t id);

/* ---- the rules beyond the old verifier ----------------------------------- */

/* Name a broken rule, once per (command, rule): a client that breaks it on
 * every frame costs one line, and at most 64 lines are ever printed. */
static void vmw_cmd_note_rule(uint32_t id, const char *rule)
{
	static struct {
		uint32_t id;
		const char *rule;
	} seen[64];
	static unsigned nseen;

	for (unsigned i = 0; i < nseen; i++)
		if (seen[i].id == id && seen[i].rule == rule)
			return;
	if (nseen >= ARRAY_SIZE(seen))
		return;
	seen[nseen].id = id;
	seen[nseen].rule = rule;
	nseen++;
	kprintf("vmwgfx: execbuf: %s (%u): %s -- %s (pid %d)\n",
		vmw_cmd_name(id), (unsigned)id, rule,
		VMW_EXECBUF_STRICT ? "refused" : "passed",
		sched_current() ? (int)sched_current()->id : -1);
}

/* A rule is broken: 0 to carry on (the command goes as it always did), or
 * the error that fails the stream in strict mode. */
static int vmw_cmd_rule_err(struct vmw_val *val, SVGA3dCmdHeader *header,
			    const char *rule, int err)
{
	(void)val;
	vmw_cmd_note_rule(header->id, rule);
	return VMW_EXECBUF_STRICT ? err : 0;
}

static int vmw_cmd_rule(struct vmw_val *val, SVGA3dCmdHeader *header,
			const char *rule)
{
	return vmw_cmd_rule_err(val, header, rule, -EINVAL);
}

/* The body of `header' as a T, refusing a body shorter than T -- the size
 * check the verifier has always made. */
#define VMW_BODY(T, b)                                  \
	T *b = (T *)(header + 1);                       \
	if (header->size < sizeof(T))                   \
		return -EINVAL

static inline void *vmw_cmd_body(SVGA3dCmdHeader *header)
{
	return header + 1;
}

/* ---- bookkeeping helpers -------------------------------------------------- */

/* Stage a binding on `node' (the context's id is filled in here).  No-op
 * when the context's bindings are not tracked.  The caller has checked
 * `shader_slot' and `slot' against the tracker's arrays: it indexes
 * without looking. */
static void vmw_val_binding_add(struct vmw_val_ctx *node,
				struct vmw_ctx_bindinfo *bi, u32 shader_slot,
				u32 slot)
{
	if (!node || !node->staged)
		return;
	bi->ctx = &node->ctx->res;
	vmw_binding_add(node->staged, bi, shader_slot, slot);
}

static struct vmw_resource *vmw_srf_res(struct vmw_surface *srf)
{
	return srf ? &srf->res : NULL;
}

/* The node of the legacy context `cid' names, for the binding commands of
 * the pre-DX command set; NULL when it has no binding tracker. */
static struct vmw_val_ctx *vmw_val_legacy_node(struct vmw_val *val,
					       uint32_t cid)
{
	struct vmw_context *c;

	if (!val->v->has_gb)
		return NULL;
	c = vmw_file_context(val->fp, cid);
	if (!c || c->dx)
		return NULL;
	return vmw_val_ctx_node(val, c);
}

/* The view `id' of `type' in the stream's DX context.
 *
 * 1: known -- *res is the view, or NULL for SVGA3D_INVALID_ID (unbound);
 * 0: views are not tracked for this stream, there is nothing to check;
 * <0: the context has no such view. */
static int vmw_val_view(struct vmw_val *val, enum vmw_view_type type, u32 id,
			struct vmw_resource **res)
{
	struct vmw_resource *view;

	*res = NULL;
	if (!val->man)
		return 0;
	if (id == SVGA3D_INVALID_ID)
		return 1;
	view = vmw_view_lookup(val->man, type, id);
	if (IS_ERR(view))
		return (int)PTR_ERR(view);
	*res = view;
	return 1;
}

/* A command that names one view: the view must exist in the context. */
static int vmw_val_view_check(struct vmw_val *val, SVGA3dCmdHeader *header,
			      enum vmw_view_type type, u32 id)
{
	struct vmw_resource *view;

	if (vmw_val_view(val, type, id, &view) < 0)
		return vmw_cmd_rule(val, header,
				    "names a view the context does not have");
	return 0;
}

/* Bind `num' views of `view_type' to consecutive slots from `first_slot'.
 * An id the context does not know is a broken rule; when it is let through
 * the slot is recorded as unbound -- recording nothing would leave the
 * tracker believing the slot still holds what it held before, and a later
 * scrub of THAT object would unbind whatever the client put there. */
static int vmw_view_bindings_add(struct vmw_val *val, SVGA3dCmdHeader *header,
				 enum vmw_view_type view_type,
				 enum vmw_ctx_binding_type binding_type,
				 u32 shader_slot, const uint32 *view_ids,
				 u32 num_views, u32 first_slot)
{
	for (u32 i = 0; i < num_views; ++i) {
		struct vmw_ctx_bindinfo_view binding;
		struct vmw_resource *view;
		int r = vmw_val_view(val, view_type, view_ids[i], &view);

		if (r == 0)
			return 0;
		if (r < 0) {
			int rc = vmw_cmd_rule(val, header,
				"binds a view the context does not have");
			if (rc)
				return rc;
			view = NULL;
		}
		mm_memset(&binding, 0, sizeof(binding));
		binding.bi.res = view;
		binding.bi.bt = binding_type;
		binding.shader_slot = shader_slot;
		binding.slot = first_slot + i;
		vmw_val_binding_add(val->dx_node, &binding.bi, shader_slot,
				    binding.slot);
	}
	return 0;
}

/* A field naming a context by id (DX_BIND_SHADER, the *_ALL_* commands):
 * it must be one of the file's.  The old verifier passed these through. */
static int vmw_val_named_cid(struct vmw_val *val, SVGA3dCmdHeader *header,
			     uint32_t cid)
{
	if (cid == SVGA3D_INVALID_ID)
		return 0;
	if (vmw_file_context(val->fp, cid))
		return 0;
	return vmw_cmd_rule(val, header, "names a context that is not the client's");
}

static int vmw_val_need_sm5(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	if (has_sm5_context(val->v))
		return 0;
	return vmw_cmd_rule(val, header, "an SM5 command on a device without SM5");
}

/* ---- trivial handlers ----------------------------------------------------- */

static int vmw_cmd_invalid(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	(void)val;
	(void)header;
	return -EINVAL;
}

static int vmw_cmd_ok(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	(void)val;
	(void)header;
	return 0;
}

/* Legacy command whose first word is the context id. */
static int vmw_cmd_cid_check(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	if (header->size < 4)
		return -EINVAL;
	return vmw_val_check_cid(val, *(uint32_t *)vmw_cmd_body(header));
}

/* DX command that only needs the stream's DX context. */
static int vmw_cmd_dx_cid_check(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	(void)header;
	return val->ctx ? 0 : -EINVAL;
}

/* Command whose first word is a surface id. */
static int vmw_cmd_sid_first(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	if (header->size < 4)
		return -EINVAL;
	return vmw_val_reloc_sid(val, (uint32_t *)vmw_cmd_body(header));
}

/* Command that starts with an SVGA3dSurfaceImageId. */
static int vmw_cmd_image_check(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	if (header->size < sizeof(SVGA3dSurfaceImageId))
		return -EINVAL;
	return vmw_val_reloc_sid(val,
		&((SVGA3dSurfaceImageId *)vmw_cmd_body(header))->sid);
}

/* ---- legacy (pre-DX) commands -------------------------------------------- */

static int vmw_cmd_surface_copy_check(struct vmw_val *val,
				      SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSurfaceCopy, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->src.sid);

	return rc ? rc : vmw_val_reloc_sid(val, &cmd->dest.sid);
}

static int vmw_cmd_stretch_blt_check(struct vmw_val *val,
				     SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSurfaceStretchBlt, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->src.sid);

	return rc ? rc : vmw_val_reloc_sid(val, &cmd->dest.sid);
}

/* SURFACE_DMA: the guest pointer and the surface, then the rules the DMA
 * itself must keep -- the suffix is the one the verifier expects, and the
 * transfer stays inside the buffer the pointer names (the device is told
 * the most it may touch, clamped to the buffer's end, so a client cannot
 * use a DMA to reach past its own memory). */
static int vmw_cmd_dma(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSurfaceDMA, cmd);
	SVGA3dCmdSurfaceDMASuffix *suffix;
	struct drm_gem_object *bo;
	struct vmw_surface *srf;
	int rc;

	rc = vmw_val_reloc_gmr_obj(val, &cmd->guest.ptr.gmrId, &bo);
	if (rc)
		return rc;
	rc = vmw_val_reloc_sid_srf(val, &cmd->host.sid, &srf);
	if (rc)
		return rc;

	if (header->size < sizeof(*cmd) + sizeof(*suffix))
		return vmw_cmd_rule(val, header, "has no DMA suffix");
	suffix = (SVGA3dCmdSurfaceDMASuffix *)((uint8_t *)cmd + header->size -
					       sizeof(*suffix));
	/* Make sure device and verifier stay in sync. */
	if (suffix->suffixSize != sizeof(*suffix))
		return vmw_cmd_rule(val, header, "has an unknown DMA suffix size");

	if (bo) {
		uint64_t room;

		/* Make sure the DMA doesn't cross the buffer's end. */
		if (cmd->guest.ptr.offset > bo->size)
			return vmw_cmd_rule(val, header,
				"guest pointer past the end of its buffer");
		room = bo->size - cmd->guest.ptr.offset;
		if ((uint64_t)suffix->maximumOffset > room)
			suffix->maximumOffset = (uint32_t)room;
	}

	if (srf)
		vmw_cursor_cmd_dma_snoop(header, srf, bo);
	return 0;
}

static int vmw_cmd_set_render_target_check(struct vmw_val *val,
					   SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSetRenderTarget, cmd);
	struct vmw_surface *srf;
	bool track = true;
	int rc;

	if (cmd->type >= SVGA3D_RT_MAX) {
		rc = vmw_cmd_rule(val, header, "render target type out of range");
		if (rc)
			return rc;
		track = false;
	}
	rc = vmw_val_check_cid(val, cmd->cid);
	if (rc)
		return rc;
	rc = vmw_val_reloc_sid_srf(val, &cmd->target.sid, &srf);
	if (rc)
		return rc;

	if (track) {
		struct vmw_ctx_bindinfo_view binding;

		mm_memset(&binding, 0, sizeof(binding));
		binding.bi.res = vmw_srf_res(srf);
		binding.bi.bt = vmw_ctx_binding_rt;
		binding.slot = cmd->type;
		vmw_val_binding_add(vmw_val_legacy_node(val, cmd->cid),
				    &binding.bi, 0, binding.slot);
	}
	return 0;
}

static int vmw_cmd_tex_state(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSetTextureState, cmd);
	struct vmw_val_ctx *node;
	int rc;

	rc = vmw_val_check_cid(val, cmd->cid);
	if (rc)
		return rc;
	node = vmw_val_legacy_node(val, cmd->cid);

	/* Followed by SVGA3dTextureState[]: {stage, name, value}; a
	 * value for SVGA3D_TS_BIND_TEXTURE is a surface handle. */
	uint32_t n = (header->size - sizeof(*cmd)) / sizeof(SVGA3dTextureState);
	SVGA3dTextureState *ts = (SVGA3dTextureState *)(cmd + 1);
	for (uint32_t i = 0; i < n; i++) {
		struct vmw_surface *srf;
		bool track = true;

		if (ts[i].name != SVGA3D_TS_BIND_TEXTURE)
			continue;
		if (ts[i].stage >= SVGA3D_NUM_TEXTURE_UNITS) {
			rc = vmw_cmd_rule(val, header,
					  "texture unit out of range");
			if (rc)
				return rc;
			track = false;
		}
		rc = vmw_val_reloc_sid_srf(val, &ts[i].value, &srf);
		if (rc)
			return rc;
		if (track) {
			struct vmw_ctx_bindinfo_tex binding;

			mm_memset(&binding, 0, sizeof(binding));
			binding.bi.res = vmw_srf_res(srf);
			binding.bi.bt = vmw_ctx_binding_tex;
			binding.texture_stage = ts[i].stage;
			vmw_val_binding_add(node, &binding.bi, 0,
					    binding.texture_stage);
		}
	}
	return 0;
}

static int vmw_cmd_present_check(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdPresent, cmd);
	return vmw_val_reloc_sid(val, &cmd->sid);
}

static int vmw_cmd_generate_mipmaps(struct vmw_val *val,
				    SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdGenerateMipmaps, cmd);
	return vmw_val_reloc_sid(val, &cmd->sid);
}

static int vmw_cmd_blt_surf_screen_check(struct vmw_val *val,
					 SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdBlitSurfaceToScreen, cmd);
	return vmw_val_reloc_sid(val, &cmd->srcImage.sid);
}

static int vmw_cmd_set_shader(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	int rc = vmw_cmd_cid_check(val, header);

	if (rc)
		return rc;
	if (header->size < sizeof(SVGA3dCmdSetShader))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	SVGA3dCmdSetShader *cmd = vmw_cmd_body(header);
	if (!vmw_shadertype_is_valid(VMW_SM_LEGACY, cmd->type))
		return vmw_cmd_rule(val, header, "illegal shader type");
	return 0;
}

static int vmw_cmd_draw(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDrawPrimitives, cmd);
	uint32_t size = header->size;
	int rc = vmw_val_check_cid(val, cmd->cid);

	if (rc)
		return rc;
	uint32_t nd = cmd->numVertexDecls, nr = cmd->numRanges;
	if (nd > 32 || nr > 32)
		return -EINVAL;
	if (size < sizeof(*cmd) + nd * sizeof(SVGA3dVertexDecl) + nr * sizeof(SVGA3dPrimitiveRange))
		return -EINVAL;
	SVGA3dVertexDecl *d = (SVGA3dVertexDecl *)(cmd + 1);
	for (uint32_t i = 0; i < nd; i++) {
		rc = vmw_val_reloc_sid(val, &d[i].array.surfaceId);
		if (rc)
			return rc;
	}
	SVGA3dPrimitiveRange *r = (SVGA3dPrimitiveRange *)(d + nd);
	for (uint32_t i = 0; i < nr; i++) {
		rc = vmw_val_reloc_sid(val, &r[i].indexArray.surfaceId);
		if (rc)
			return rc;
	}
	return 0;
}

static int vmw_cmd_set_vertex_streams(struct vmw_val *val,
				      SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdSetVertexStreams, cmd);
	int rc = vmw_val_check_cid(val, cmd->cid);

	if (rc)
		return rc;
	uint32_t n = cmd->numStreams;
	if (header->size < sizeof(*cmd) + n * sizeof(SVGA3dVertexStream))
		return -EINVAL;
	SVGA3dVertexStream *s = (SVGA3dVertexStream *)(cmd + 1);
	for (uint32_t i = 0; i < n; i++) {
		rc = vmw_val_reloc_sid(val, &s[i].sid);
		if (rc)
			return rc;
	}
	return 0;
}

static int vmw_cmd_draw_indexed(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDrawIndexed, cmd);
	int rc = vmw_val_check_cid(val, cmd->cid);

	return rc ? rc : vmw_val_reloc_sid(val, &cmd->indexBufferSid);
}

/* The query result the device writes must fit in the buffer named for it. */
static int vmw_query_result_check(struct vmw_val *val, SVGA3dCmdHeader *header,
				  struct drm_gem_object *bo, uint32_t offset,
				  bool end)
{
	int rc;

	if (!bo)
		return 0;
	if ((uint64_t)offset + sizeof(SVGA3dQueryResult) > bo->size) {
		rc = vmw_cmd_rule(val, header,
				  "query result past the end of its buffer");
		if (rc)
			return rc;
	}
	/* The buffer a query ends into is kept small: it is the one the
	 * device writes the result to, never a general data buffer. */
	if (end && PFN_UP(bo->size) > 4)
		return vmw_cmd_rule(val, header, "query buffer too large");
	return 0;
}

/* END_QUERY / WAIT_FOR_QUERY: {cid, type, SVGAGuestPtr guestResult} */
static int vmw_cmd_end_query(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	struct drm_gem_object *bo;
	uint8_t *body = vmw_cmd_body(header);
	int rc;

	if (header->size < 16)
		return -EINVAL;
	rc = vmw_val_check_cid(val, *(uint32_t *)body);
	if (rc)
		return rc;
	rc = vmw_val_reloc_gmr_obj(val, (uint32_t *)(body + 8), &bo);
	if (rc)
		return rc;
	return vmw_query_result_check(val, header, bo,
				      *(uint32_t *)(body + 12),
				      header->id == SVGA_3D_CMD_END_QUERY);
}

/* END_GB_QUERY / WAIT_FOR_GB_QUERY */
static int vmw_cmd_end_gb_query(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdEndGBQuery, cmd);
	struct drm_gem_object *bo;
	int rc = vmw_val_check_cid(val, cmd->cid);

	if (rc)
		return rc;
	rc = vmw_val_reloc_mob_obj(val, &cmd->mobid, &bo);
	if (rc)
		return rc;
	return vmw_query_result_check(val, header, bo, cmd->offset,
				      header->id == SVGA_3D_CMD_END_GB_QUERY);
}

/* ---- guest-backed object commands ----------------------------------------- */

static int vmw_cmd_bind_gb_surface(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdBindGBSurface, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->sid);

	return rc ? rc : vmw_val_reloc_mob(val, &cmd->mobid);
}

static int vmw_cmd_bind_gb_surface_pitch(struct vmw_val *val,
					 SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdBindGBSurfaceWithPitch, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->sid);

	return rc ? rc : vmw_val_reloc_mob(val, &cmd->mobid);
}

static int vmw_cmd_bind_gb_shader(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdBindGBShader, cmd);
	return vmw_val_reloc_mob(val, &cmd->mobid);
}

static int vmw_cmd_gb_mob_fence(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdGBMobFence, cmd);
	return vmw_val_reloc_mob(val, &cmd->mobId);
}

/* ---- DX: constant, vertex, index and stream-output buffers ---------------- */

static int vmw_cmd_dx_set_single_constant_buffer(struct vmw_val *val,
						 SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXSetSingleConstantBuffer, cmd);
	struct vmw_surface *srf;
	bool track = true;
	int rc;

	rc = vmw_val_reloc_sid_srf(val, &cmd->sid, &srf);
	if (rc)
		return rc;
	if (!val->ctx) {
		rc = vmw_cmd_rule(val, header, "a DX command without a DX context");
		if (rc)
			return rc;
		track = false;
	}
	if (!vmw_shadertype_is_valid(vmw_sm_type(val->v), cmd->type) ||
	    cmd->slot >= SVGA3D_DX_MAX_CONSTBUFFERS) {
		rc = vmw_cmd_rule(val, header,
				  "illegal constant buffer shader or slot");
		if (rc)
			return rc;
		track = false;
	}
	if (track) {
		struct vmw_ctx_bindinfo_cb binding;

		mm_memset(&binding, 0, sizeof(binding));
		binding.bi.res = vmw_srf_res(srf);
		binding.bi.bt = vmw_ctx_binding_cb;
		binding.shader_slot = cmd->type - SVGA3D_SHADERTYPE_MIN;
		binding.offset = cmd->offsetInBytes;
		binding.size = cmd->sizeInBytes;
		binding.slot = cmd->slot;
		vmw_val_binding_add(val->dx_node, &binding.bi,
				    binding.shader_slot, binding.slot);
	}
	return 0;
}

/* DX_SET_{VS,PS,GS,HS,DS,CS}_CONSTANT_BUFFER_OFFSET */
static int vmw_cmd_dx_set_constant_buffer_offset(struct vmw_val *val,
						 SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetConstantBufferOffset *cmd = vmw_cmd_body(header);
	u32 shader_slot;
	int rc;

	if (!val->ctx)
		return -EINVAL;
	rc = vmw_val_need_sm5(val, header);
	if (rc || !has_sm5_context(val->v))
		return rc;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	if (cmd->slot >= SVGA3D_DX_MAX_CONSTBUFFERS)
		return vmw_cmd_rule(val, header, "constant buffer slot out of range");

	shader_slot = header->id - SVGA_3D_CMD_DX_SET_VS_CONSTANT_BUFFER_OFFSET;
	if (val->dx_node && val->dx_node->staged)
		vmw_binding_cb_offset_update(val->dx_node->staged, shader_slot,
					     cmd->slot, cmd->offsetInBytes);
	return 0;
}

/* DX_SET_VERTEX_BUFFERS and its _V2: same slots, wider elements. */
static int vmw_cmd_dx_vb_common(struct vmw_val *val, SVGA3dCmdHeader *header,
				uint32_t start, uint8_t *elems, uint32_t n,
				uint32_t elem_size)
{
	bool track = true;
	int rc;

	if ((u64)n + (u64)start > (u64)SVGA3D_DX_MAX_VERTEXBUFFERS) {
		rc = vmw_cmd_rule(val, header, "too many vertex buffers");
		if (rc)
			return rc;
		track = false;
	}
	for (uint32_t i = 0; i < n; i++) {
		/* Both layouts start with {sid, stride, offset}. */
		SVGA3dVertexBuffer *vb = (SVGA3dVertexBuffer *)(elems +
								 i * elem_size);
		struct vmw_surface *srf;

		rc = vmw_val_reloc_sid_srf(val, &vb->sid, &srf);
		if (rc)
			return rc;
		if (track) {
			struct vmw_ctx_bindinfo_vb binding;

			mm_memset(&binding, 0, sizeof(binding));
			binding.bi.res = vmw_srf_res(srf);
			binding.bi.bt = vmw_ctx_binding_vb;
			binding.offset = vb->offset;
			binding.stride = vb->stride;
			binding.slot = i + start;
			vmw_val_binding_add(val->dx_node, &binding.bi, 0,
					    binding.slot);
		}
	}
	return val->ctx ? 0 : -EINVAL;
}

static int vmw_cmd_dx_set_vertex_buffers(struct vmw_val *val,
					 SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXSetVertexBuffers, cmd);
	uint32_t n = (header->size - sizeof(*cmd)) / sizeof(SVGA3dVertexBuffer);

	return vmw_cmd_dx_vb_common(val, header, cmd->startBuffer,
				    (uint8_t *)(cmd + 1), n,
				    sizeof(SVGA3dVertexBuffer));
}

static int vmw_cmd_dx_set_vertex_buffers_v2(struct vmw_val *val,
					    SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXSetVertexBuffers_v2, cmd);
	uint32_t n = (header->size - sizeof(*cmd)) / sizeof(SVGA3dVertexBuffer_v2);

	BUILD_BUG_ON(offsetof(SVGA3dVertexBuffer_v2, sid) !=
		     offsetof(SVGA3dVertexBuffer, sid));
	BUILD_BUG_ON(offsetof(SVGA3dVertexBuffer_v2, stride) !=
		     offsetof(SVGA3dVertexBuffer, stride));
	BUILD_BUG_ON(offsetof(SVGA3dVertexBuffer_v2, offset) !=
		     offsetof(SVGA3dVertexBuffer, offset));
	return vmw_cmd_dx_vb_common(val, header, cmd->startBuffer,
				    (uint8_t *)(cmd + 1), n,
				    sizeof(SVGA3dVertexBuffer_v2));
}

/* DX_SET_INDEX_BUFFER and its _V2 ({sid, format, offset} first in both). */
static int vmw_cmd_dx_set_index_buffer(struct vmw_val *val,
				       SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetIndexBuffer *cmd = vmw_cmd_body(header);
	struct vmw_surface *srf;
	int rc;

	BUILD_BUG_ON(offsetof(SVGA3dCmdDXSetIndexBuffer_v2, sid) !=
		     offsetof(SVGA3dCmdDXSetIndexBuffer, sid));
	BUILD_BUG_ON(offsetof(SVGA3dCmdDXSetIndexBuffer_v2, format) !=
		     offsetof(SVGA3dCmdDXSetIndexBuffer, format));
	BUILD_BUG_ON(offsetof(SVGA3dCmdDXSetIndexBuffer_v2, offset) !=
		     offsetof(SVGA3dCmdDXSetIndexBuffer, offset));
	if (header->size < (header->id == SVGA_3D_CMD_DX_SET_INDEX_BUFFER_V2 ?
			    sizeof(SVGA3dCmdDXSetIndexBuffer_v2) :
			    sizeof(SVGA3dCmdDXSetIndexBuffer)))
		return -EINVAL;
	rc = vmw_val_reloc_sid_srf(val, &cmd->sid, &srf);
	if (rc)
		return rc;
	if (!val->ctx)
		return vmw_cmd_rule(val, header, "a DX command without a DX context");

	struct vmw_ctx_bindinfo_ib binding;

	mm_memset(&binding, 0, sizeof(binding));
	binding.bi.res = vmw_srf_res(srf);
	binding.bi.bt = vmw_ctx_binding_ib;
	binding.offset = cmd->offset;
	binding.format = cmd->format;
	vmw_val_binding_add(val->dx_node, &binding.bi, 0, 0);
	return 0;
}

static int vmw_cmd_dx_set_so_targets(struct vmw_val *val,
				     SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXSetSOTargets, cmd);
	uint32_t n = (header->size - sizeof(*cmd)) / sizeof(SVGA3dSoTarget);
	SVGA3dSoTarget *t = (SVGA3dSoTarget *)(cmd + 1);
	bool track = true;
	int rc;

	if (n > SVGA3D_DX_MAX_SOTARGETS) {
		rc = vmw_cmd_rule(val, header, "too many stream-output targets");
		if (rc)
			return rc;
		track = false;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct vmw_surface *srf;

		rc = vmw_val_reloc_sid_srf(val, &t[i].sid, &srf);
		if (rc)
			return rc;
		if (track) {
			struct vmw_ctx_bindinfo_so_target binding;

			mm_memset(&binding, 0, sizeof(binding));
			binding.bi.res = vmw_srf_res(srf);
			binding.bi.bt = vmw_ctx_binding_so_target;
			binding.offset = t[i].offset;
			binding.size = t[i].sizeInBytes;
			binding.slot = i;
			vmw_val_binding_add(val->dx_node, &binding.bi, 0,
					    binding.slot);
		}
	}
	return val->ctx ? 0 : -EINVAL;
}

/* ---- DX: views ------------------------------------------------------------ */

static int vmw_cmd_dx_set_shader_res(struct vmw_val *val,
				     SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetShaderResources *cmd = vmw_cmd_body(header);
	u32 num_sr_view;

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	num_sr_view = (header->size - sizeof(*cmd)) /
		      sizeof(SVGA3dShaderResourceViewId);
	if ((u64)cmd->startView + (u64)num_sr_view >
		    (u64)SVGA3D_DX_MAX_SRVIEWS ||
	    !vmw_shadertype_is_valid(vmw_sm_type(val->v), cmd->type))
		return vmw_cmd_rule(val, header, "invalid shader binding");

	return vmw_view_bindings_add(val, header, vmw_view_sr,
				     vmw_ctx_binding_sr,
				     cmd->type - SVGA3D_SHADERTYPE_MIN,
				     (const uint32 *)(cmd + 1), num_sr_view,
				     cmd->startView);
}

static int vmw_cmd_dx_set_rendertargets(struct vmw_val *val,
					SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetRenderTargets *cmd = vmw_cmd_body(header);
	u32 num_rt_view;
	int rc;

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	num_rt_view = (header->size - sizeof(*cmd)) /
		      sizeof(SVGA3dRenderTargetViewId);
	if (num_rt_view > SVGA3D_DX_MAX_RENDER_TARGETS)
		return vmw_cmd_rule(val, header, "too many render targets");

	rc = vmw_view_bindings_add(val, header, vmw_view_ds, vmw_ctx_binding_ds,
				   0, &cmd->depthStencilViewId, 1, 0);
	if (rc)
		return rc;
	return vmw_view_bindings_add(val, header, vmw_view_rt,
				     vmw_ctx_binding_dx_rt, 0,
				     (const uint32 *)(cmd + 1), num_rt_view, 0);
}

static int vmw_cmd_dx_clear_rendertarget_view(struct vmw_val *val,
					      SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXClearRenderTargetView *cmd = vmw_cmd_body(header);

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	return vmw_val_view_check(val, header, vmw_view_rt,
				  cmd->renderTargetViewId);
}

static int vmw_cmd_dx_clear_depthstencil_view(struct vmw_val *val,
					      SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXClearDepthStencilView *cmd = vmw_cmd_body(header);

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	return vmw_val_view_check(val, header, vmw_view_ds,
				  cmd->depthStencilViewId);
}

static int vmw_cmd_dx_genmips(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXGenMips *cmd = vmw_cmd_body(header);

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	return vmw_val_view_check(val, header, vmw_view_sr,
				  cmd->shaderResourceViewId);
}

/* DX_DEFINE_{SHADERRESOURCE,RENDERTARGET,DEPTHSTENCIL,UA}_VIEW(_V2).
 *
 * Every one of them starts {view id, sid}: the context's object table must
 * hold the id and the surface must be the client's -- what the verifier
 * always checked -- and then the view is staged in the context's manager,
 * keeping the whole command so that it can be destroyed with its surface. */
static int vmw_cmd_dx_view_define(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	struct {
		uint32 defined_id;
		uint32 sid;
	} *cmd = vmw_cmd_body(header);
	enum vmw_view_type view_type = vmw_view_cmd_to_type(header->id);
	struct vmw_surface *srf;
	uint32_t min_size;
	int rc;

	switch (header->id) {
	case SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW:
		min_size = sizeof(SVGA3dCmdDXDefineShaderResourceView);
		break;
	case SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW:
		min_size = sizeof(SVGA3dCmdDXDefineRenderTargetView);
		break;
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW:
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2:
		min_size = sizeof(SVGA3dCmdDXDefineDepthStencilView);
		break;
	case SVGA_3D_CMD_DX_DEFINE_UA_VIEW:
		min_size = sizeof(SVGA3dCmdDXDefineUAView);
		break;
	default:
		return -EINVAL;
	}
	if (view_type == vmw_view_max || header->size < min_size)
		return -EINVAL;

	rc = vmw_val_cotable(val, vmw_view_cotables[view_type],
			     cmd->defined_id);
	if (rc)
		return rc;
	rc = vmw_val_reloc_sid_srf(val, &cmd->sid, &srf);
	if (rc)
		return rc;

	if (header->id == SVGA_3D_CMD_DX_DEFINE_UA_VIEW ||
	    header->id == SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2) {
		rc = vmw_val_need_sm5(val, header);
		if (rc || !has_sm5_context(val->v))
			return rc;
	}
	if (!srf)
		return vmw_cmd_rule(val, header, "a view of no surface");
	if (!val->man)
		return 0;

	rc = vmw_view_add(val->man, &val->ctx->res, &srf->res, view_type,
			  cmd->defined_id, header,
			  header->size + sizeof(*header), &val->staged_cmd_res);
	if (rc)
		return vmw_cmd_rule(val, header,
			"view could not be recorded (id in use?)");
	return 0;
}

/* DX_DESTROY_{SHADERRESOURCE,RENDERTARGET,DEPTHSTENCIL,UA}_VIEW.
 *
 * The view is staged for removal.  If it was created by an earlier stream,
 * the driver may have destroyed it on the device already -- with its
 * surface, or its context -- and then the client's destroy must not reach
 * the device a second time: the command becomes a NOP at submission if
 * the view's id has been given up by then. */
static int vmw_cmd_dx_view_remove(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	union vmw_view_destroy *cmd = vmw_cmd_body(header);
	enum vmw_view_type view_type = vmw_view_cmd_to_type(header->id);
	struct vmw_resource *view;
	int rc;

	if (!val->ctx)
		return -EINVAL;
	if (view_type == vmw_view_ua) {
		rc = vmw_val_need_sm5(val, header);
		if (rc || !has_sm5_context(val->v))
			return rc;
	}
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	if (!val->man || view_type == vmw_view_max)
		return 0;

	rc = vmw_view_remove(val->man, cmd->view_id, view_type,
			     &val->staged_cmd_res, &view);
	if (rc)
		return vmw_cmd_rule(val, header,
			"destroys a view the context does not have");
	if (!view)
		return 0;
	return vmw_val_cond_nop_add(val, &header->id, view);
}

static int vmw_cmd_clear_uav(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXClearUAViewUint *cmd = vmw_cmd_body(header);
	int rc;

	BUILD_BUG_ON(offsetof(SVGA3dCmdDXClearUAViewFloat, uaViewId) !=
		     offsetof(SVGA3dCmdDXClearUAViewUint, uaViewId));
	if (!val->ctx)
		return -EINVAL;
	rc = vmw_val_need_sm5(val, header);
	if (rc || !has_sm5_context(val->v))
		return rc;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	return vmw_val_view_check(val, header, vmw_view_ua, cmd->uaViewId);
}

/* DX_SET_UA_VIEWS and DX_SET_CS_UA_VIEWS: one word (splice or start index)
 * then the view ids. */
static int vmw_cmd_set_uav(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	uint32_t *body = vmw_cmd_body(header);
	bool cs = header->id == SVGA_3D_CMD_DX_SET_CS_UA_VIEWS;
	u32 num_uav;
	int rc;

	BUILD_BUG_ON(sizeof(SVGA3dCmdDXSetUAViews) != sizeof(uint32_t));
	BUILD_BUG_ON(sizeof(SVGA3dCmdDXSetCSUAViews) != sizeof(uint32_t));
	if (!val->ctx)
		return -EINVAL;
	rc = vmw_val_need_sm5(val, header);
	if (rc || !has_sm5_context(val->v))
		return rc;
	if (header->size < sizeof(uint32_t))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	num_uav = (header->size - sizeof(uint32_t)) / sizeof(SVGA3dUAViewId);
	if (num_uav > vmw_max_num_uavs(val->v))
		return vmw_cmd_rule(val, header, "too many UA views");

	rc = vmw_view_bindings_add(val, header, vmw_view_ua,
				   cs ? vmw_ctx_binding_cs_uav :
					vmw_ctx_binding_uav,
				   0, body + 1, num_uav, 0);
	if (rc)
		return rc;
	if (val->man && val->dx_node && val->dx_node->staged)
		vmw_binding_add_uav_index(val->dx_node->staged, cs ? 1 : 0,
					  body[0]);
	return 0;
}

/* ---- DX: shaders, state objects, queries, stream output ------------------- */

static int vmw_cmd_dx_set_shader(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetShader *cmd = vmw_cmd_body(header);

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	if (!vmw_shadertype_is_valid(vmw_sm_type(val->v), cmd->type))
		return vmw_cmd_rule(val, header, "illegal shader type");
	return 0;
}

static int vmw_cmd_dx_define_shader(struct vmw_val *val,
				    SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXDefineShader, cmd);
	int rc = vmw_val_cotable(val, SVGA_COTABLE_DXSHADER, cmd->shaderId);

	if (rc)
		return rc;
	if (cmd->shaderId > ((1u << 20) - 1) || (unsigned)cmd->type >= 16)
		return vmw_cmd_rule(val, header, "shader id or type out of range");
	return 0;
}

static int vmw_cmd_dx_bind_shader(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindShader, cmd);
	int rc = vmw_val_cotable(val, SVGA_COTABLE_DXSHADER, cmd->shid);

	if (rc)
		return rc;
	rc = vmw_val_reloc_mob(val, &cmd->mobid);
	if (rc)
		return rc;
	return vmw_val_named_cid(val, header, cmd->cid);
}

static int vmw_cmd_dx_bind_all_shader(struct vmw_val *val,
				      SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindAllShader, cmd);
	int rc = vmw_val_reloc_mob(val, &cmd->mobid);

	return rc ? rc : vmw_val_named_cid(val, header, cmd->cid);
}

/* DX_DEFINE_{ELEMENTLAYOUT,BLEND_STATE,DEPTHSTENCIL_STATE,RASTERIZER_STATE
 * (_V2),SAMPLER_STATE,STREAMOUTPUT}: the first word is the id the context's
 * object table must hold. */
static int vmw_cmd_dx_so_define(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	enum vmw_so_type so_type = vmw_so_cmd_to_type(header->id);

	if (header->size < 4)
		return -EINVAL;
	if (so_type == vmw_so_max)
		return -EINVAL;
	return vmw_val_cotable(val, vmw_so_cotables[so_type],
			       *(uint32_t *)vmw_cmd_body(header));
}

static int vmw_cmd_dx_define_streamoutput(struct vmw_val *val,
					  SVGA3dCmdHeader *header)
{
	int rc = vmw_cmd_dx_so_define(val, header);

	return rc ? rc : vmw_val_need_sm5(val, header);
}

static int vmw_cmd_dx_bind_streamoutput(struct vmw_val *val,
					SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindStreamOutput, cmd);
	int rc = vmw_val_cotable(val, SVGA_COTABLE_STREAMOUTPUT, cmd->soid);

	if (rc)
		return rc;
	rc = vmw_val_reloc_mob(val, &cmd->mobid);
	return rc ? rc : vmw_val_need_sm5(val, header);
}

static int vmw_cmd_dx_define_query(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXDefineQuery, cmd);
	int rc = vmw_val_cotable(val, SVGA_COTABLE_DXQUERY, cmd->queryId);

	if (rc)
		return rc;
	if ((int)cmd->type < SVGA3D_QUERYTYPE_MIN ||
	    cmd->type >= SVGA3D_QUERYTYPE_MAX)
		return vmw_cmd_rule(val, header, "query type out of range");
	return 0;
}

/* DX_BIND_QUERY: the MOB the context's query results go to.  Remembered
 * on the context once the stream is submitted (vmw_execbuf.c); a context
 * keeps one query MOB, so a stream naming a second one breaks a rule. */
static int vmw_cmd_dx_bind_query(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindQuery, cmd);
	struct drm_gem_object *bo, *cur;
	int rc = vmw_val_cotable(val, SVGA_COTABLE_DXQUERY, cmd->queryId);

	if (rc)
		return rc;
	rc = vmw_val_reloc_mob_obj(val, &cmd->mobid, &bo);
	if (rc || !bo)
		return rc;

	cur = vmw_context_get_dx_query_mob(&val->ctx->res);
	if ((cur && cur != bo) ||
	    (val->dx_query_mob && val->dx_query_mob != bo))
		return vmw_cmd_rule(val, header,
				    "a second query MOB for the context");
	val->dx_query_mob = bo;
	val->dx_query_ctx = val->ctx;
	return 0;
}

static int vmw_cmd_dx_set_query_offset(struct vmw_val *val,
				       SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXSetQueryOffset *cmd = vmw_cmd_body(header);
	struct drm_gem_object *mob;

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return vmw_cmd_rule(val, header, "body shorter than the command");
	mob = val->dx_query_mob ? val->dx_query_mob :
		vmw_context_get_dx_query_mob(&val->ctx->res);
	if (mob && cmd->mobOffset >= mob->size)
		return vmw_cmd_rule(val, header,
				    "query offset past the end of the query MOB");
	return 0;
}

/* DX_MOVE_QUERY: move a query's result slot to `mobOffset' of `mobid'.
 * The MOB is the client's buffer handle, relocated like every other MOB
 * reference -- passed through as written it would let a client point the
 * device at any MOB at all, another process's included -- and the result
 * must start inside it.  A body too short to hold the MOB field cannot be
 * relocated, so it is refused rather than passed through. */
static int vmw_cmd_dx_move_query(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	SVGA3dCmdDXMoveQuery *cmd = vmw_cmd_body(header);
	struct drm_gem_object *bo;
	int rc;

	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(*cmd))
		return -EINVAL;
	rc = vmw_val_reloc_mob_obj(val, &cmd->mobid, &bo);
	if (rc)
		return rc;
	if (bo && cmd->mobOffset >= bo->size)
		return vmw_cmd_rule(val, header,
				    "query offset past the end of the query MOB");
	return 0;
}

static int vmw_cmd_dx_bind_all_query(struct vmw_val *val,
				     SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindAllQuery, cmd);
	int rc = vmw_val_reloc_mob(val, &cmd->mobid);

	return rc ? rc : vmw_val_named_cid(val, header, cmd->cid);
}

static int vmw_cmd_dx_readback_all_query(struct vmw_val *val,
					 SVGA3dCmdHeader *header)
{
	if (!val->ctx)
		return -EINVAL;
	if (header->size < sizeof(SVGA3dCmdDXReadbackAllQuery))
		return 0;
	return vmw_val_named_cid(val, header,
				 *(uint32_t *)vmw_cmd_body(header));
}

static int vmw_cmd_sm5(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	if (!val->ctx)
		return -EINVAL;
	return vmw_val_need_sm5(val, header);
}

/* DX_DRAW_INDEXED_INSTANCED_INDIRECT, DX_DRAW_INSTANCED_INDIRECT,
 * DX_DISPATCH_INDIRECT: the arguments buffer. */
static int vmw_cmd_indirect(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXDrawIndexedInstancedIndirect, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->argsBufferSid);

	return rc ? rc : vmw_val_need_sm5(val, header);
}

/* ---- DX: copies and transfers ----------------------------------------------
 *
 * Each names two surfaces; both are relocated, in the order the verifier
 * has always used. */

#define VMW_CMD_TWO_SIDS(name, T, first, second)                          \
	static int name(struct vmw_val *val, SVGA3dCmdHeader *header)     \
	{                                                                 \
		VMW_BODY(T, cmd);                                         \
		int rc = vmw_val_reloc_sid(val, &cmd->first);             \
		return rc ? rc : vmw_val_reloc_sid(val, &cmd->second);    \
	}

VMW_CMD_TWO_SIDS(vmw_cmd_whole_surface_copy, SVGA3dCmdWholeSurfaceCopy,
		 srcSid, destSid)
VMW_CMD_TWO_SIDS(vmw_cmd_pred_copy_region, SVGA3dCmdDXPredCopyRegion,
		 dstSid, srcSid)
VMW_CMD_TWO_SIDS(vmw_cmd_pred_copy, SVGA3dCmdDXPredCopy, dstSid, srcSid)
VMW_CMD_TWO_SIDS(vmw_cmd_pred_convert_region, SVGA3dCmdDXPredConvertRegion,
		 dstSid, srcSid)
VMW_CMD_TWO_SIDS(vmw_cmd_presentblt, SVGA3dCmdDXPresentBlt, srcSid, dstSid)
VMW_CMD_TWO_SIDS(vmw_cmd_transfer_from_buffer, SVGA3dCmdDXTransferFromBuffer,
		 srcSid, destSid)
VMW_CMD_TWO_SIDS(vmw_cmd_transfer_to_buffer, SVGA3dCmdDXTransferToBuffer,
		 srcSid, destSid)
VMW_CMD_TWO_SIDS(vmw_cmd_buffer_copy_check, SVGA3dCmdDXBufferCopy, dest, src)
VMW_CMD_TWO_SIDS(vmw_cmd_copy_and_readback, SVGA3dCmdDXSurfaceCopyAndReadback,
		 srcSid, destSid)
VMW_CMD_TWO_SIDS(vmw_cmd_resolve_copy, SVGA3dCmdDXResolveCopy, dstSid, srcSid)
VMW_CMD_TWO_SIDS(vmw_cmd_staging_copy, SVGA3dCmdDXStagingCopy, dstSid, srcSid)
VMW_CMD_TWO_SIDS(vmw_cmd_pred_staging_copy_region,
		 SVGA3dCmdDXPredStagingCopyRegion, dstSid, srcSid)

static int vmw_cmd_intra_surface_copy(struct vmw_val *val,
				      SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdIntraSurfaceCopy, cmd);
	int rc = vmw_val_reloc_sid(val, &cmd->surface.sid);

	if (rc)
		return rc;
	if (!(val->v->cap2 & SVGA_CAP2_INTRA_SURFACE_COPY))
		return vmw_cmd_rule(val, header,
				    "intra-surface copy on a device without it");
	return 0;
}

static int vmw_cmd_copy_structure_count(struct vmw_val *val,
					SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXCopyStructureCount, cmd);
	return vmw_val_reloc_sid(val, &cmd->destSid);
}

static int vmw_cmd_dx_mob_fence_64(struct vmw_val *val, SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXMobFence64, cmd);
	return vmw_val_reloc_mob(val, &cmd->mobId);
}

static int vmw_cmd_dx_bind_shader_iface(struct vmw_val *val,
					SVGA3dCmdHeader *header)
{
	VMW_BODY(SVGA3dCmdDXBindShaderIface, cmd);
	return vmw_val_reloc_mob(val, &cmd->mobid);
}

/* ---- non-3D commands ------------------------------------------------------ */

/* The two-dimensional SVGA_CMD_* set has no size field; the verifier knows
 * the size of the few it recognises.  None of them is a client's to send:
 * they program the scan-out, which the kernel owns. */
static int vmw_cmd_check_not_3d(struct vmw_val *val, void *buf, uint32_t avail,
				uint32_t *size)
{
	uint32_t cmd_id = ((uint32_t *)buf)[0];
	uint32_t need;

	(void)val;
	switch (cmd_id) {
	case SVGA_CMD_UPDATE:
		need = sizeof(uint32_t) + sizeof(SVGAFifoCmdUpdate);
		break;
	case SVGA_CMD_DEFINE_GMRFB:
		need = sizeof(uint32_t) + sizeof(SVGAFifoCmdDefineGMRFB);
		break;
	case SVGA_CMD_BLIT_GMRFB_TO_SCREEN:
		need = sizeof(uint32_t) + sizeof(SVGAFifoCmdBlitGMRFBToScreen);
		break;
	case SVGA_CMD_BLIT_SCREEN_TO_GMRFB:
		need = sizeof(uint32_t) + sizeof(SVGAFifoCmdBlitGMRFBToScreen);
		break;
	default:
		return -EINVAL;
	}
	if (need > avail)
		return -EINVAL;
	*size = need;
	/* Kernel-only. */
	return -EPERM;
}

/* ---- the table ------------------------------------------------------------ */

#define VMW_CMD_DEF(_cmd, _func, _user_allow, _gb_disable, _gb_enable)       \
	[(_cmd) - SVGA_3D_CMD_BASE] = { (_func), (_user_allow), (_gb_disable), \
					(_gb_enable), #_cmd }

#define VMW_CMD_TABLE_SIZE (SVGA_3D_CMD_MAX - SVGA_3D_CMD_BASE)

/* Columns: handler, a client may send it, refused with guest-backed
 * objects, needs guest-backed objects.
 *
 * Every command the driver accepted before the table existed is here with
 * a handler doing what was done for it then.  A few of them are commands a
 * client is not expected to send (the presents to a screen, the legacy
 * mipmap generation, the conditional and pitched surface binds, the MOB
 * fence, the query readback, the structure-count copy): they keep
 * their handler and say user_allow = false, which the strict verifier
 * refuses and the default one only names.  The commands that create and
 * destroy device objects the kernel accounts for (surfaces, contexts,
 * MOBs, object tables, screen targets, shaders) are refused, as they
 * always were. */
static const struct vmw_cmd_entry vmw_cmd_entries[VMW_CMD_TABLE_SIZE] = {
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_DEFINE, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_DESTROY, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_COPY, &vmw_cmd_surface_copy_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_STRETCHBLT, &vmw_cmd_stretch_blt_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_DMA, &vmw_cmd_dma,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_CONTEXT_DEFINE, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_CONTEXT_DESTROY, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETTRANSFORM, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETZRANGE, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETRENDERSTATE, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETRENDERTARGET,
		    &vmw_cmd_set_render_target_check, true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETTEXTURESTATE, &vmw_cmd_tex_state,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETMATERIAL, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETLIGHTDATA, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETLIGHTENABLED, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETVIEWPORT, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETCLIPPLANE, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_CLEAR, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_PRESENT, &vmw_cmd_present_check,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SHADER_DEFINE, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SHADER_DESTROY, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_SHADER, &vmw_cmd_set_shader,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_SHADER_CONST, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DRAW_PRIMITIVES, &vmw_cmd_draw,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SETSCISSORRECT, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_BEGIN_QUERY, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_END_QUERY, &vmw_cmd_end_query,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_WAIT_FOR_QUERY, &vmw_cmd_end_query,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_PRESENT_READBACK, &vmw_cmd_ok,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN,
		    &vmw_cmd_blt_surf_screen_check, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SURFACE_DEFINE_V2, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_GENERATE_MIPMAPS, &vmw_cmd_generate_mipmaps,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_ACTIVATE_SURFACE, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEACTIVATE_SURFACE, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SCREEN_DMA, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD1, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD2, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD12, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD13, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD14, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD15, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD16, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DEAD17, &vmw_cmd_invalid, false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_OTABLE_BASE, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_READBACK_OTABLE, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_MOB, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DESTROY_GB_MOB, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_REDEFINE_GB_MOB64, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_UPDATE_GB_MOB_MAPPING, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_SURFACE, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DESTROY_GB_SURFACE, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_BIND_GB_SURFACE, &vmw_cmd_bind_gb_surface,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_COND_BIND_GB_SURFACE, &vmw_cmd_bind_gb_surface,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_UPDATE_GB_IMAGE, &vmw_cmd_image_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_UPDATE_GB_SURFACE, &vmw_cmd_sid_first,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_READBACK_GB_IMAGE, &vmw_cmd_image_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_READBACK_GB_SURFACE, &vmw_cmd_sid_first,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_INVALIDATE_GB_IMAGE, &vmw_cmd_image_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_INVALIDATE_GB_SURFACE, &vmw_cmd_sid_first,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DESTROY_GB_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_BIND_GB_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_READBACK_GB_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_INVALIDATE_GB_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_SHADER, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_BIND_GB_SHADER, &vmw_cmd_bind_gb_shader,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DESTROY_GB_SHADER, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_OTABLE_BASE64, &vmw_cmd_invalid,
		    false, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_BEGIN_GB_QUERY, &vmw_cmd_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_END_GB_QUERY, &vmw_cmd_end_gb_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_WAIT_FOR_GB_QUERY, &vmw_cmd_end_gb_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_NOP, &vmw_cmd_ok,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_NOP_ERROR, &vmw_cmd_ok,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_ENABLE_GART, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DISABLE_GART, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_MAP_MOB_INTO_GART, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_UNMAP_GART_RANGE, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_SCREENTARGET, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DESTROY_GB_SCREENTARGET, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_BIND_GB_SCREENTARGET, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_UPDATE_GB_SCREENTARGET, &vmw_cmd_invalid,
		    false, false, true),
	/* Mesa reads back and invalidates partial images. */
	VMW_CMD_DEF(SVGA_3D_CMD_READBACK_GB_IMAGE_PARTIAL, &vmw_cmd_image_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_INVALIDATE_GB_IMAGE_PARTIAL,
		    &vmw_cmd_image_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE, &vmw_cmd_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_GB_SCREEN_DMA, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_BIND_GB_SURFACE_WITH_PITCH,
		    &vmw_cmd_bind_gb_surface_pitch, false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_GB_MOB_FENCE, &vmw_cmd_gb_mob_fence,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_SURFACE_V2, &vmw_cmd_invalid,
		    false, false, true),

	/* The vertex-array commands of the legacy set. */
	VMW_CMD_DEF(SVGA_3D_CMD_SET_VERTEX_STREAMS, &vmw_cmd_set_vertex_streams,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_VERTEX_DECLS, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_SET_VERTEX_DIVISORS, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DRAW, &vmw_cmd_cid_check,
		    true, false, false),
	VMW_CMD_DEF(SVGA_3D_CMD_DRAW_INDEXED, &vmw_cmd_draw_indexed,
		    true, false, false),

	/* SM commands */
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_READBACK_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_INVALIDATE_CONTEXT, &vmw_cmd_invalid,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER,
		    &vmw_cmd_dx_set_single_constant_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SHADER_RESOURCES,
		    &vmw_cmd_dx_set_shader_res, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SHADER, &vmw_cmd_dx_set_shader,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SAMPLERS, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_INDEXED, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_INSTANCED, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_AUTO, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_INPUT_LAYOUT,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS,
		    &vmw_cmd_dx_set_vertex_buffers, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_INDEX_BUFFER,
		    &vmw_cmd_dx_set_index_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_TOPOLOGY,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_RENDERTARGETS,
		    &vmw_cmd_dx_set_rendertargets, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_BLEND_STATE, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_DEPTHSTENCIL_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_RASTERIZER_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_QUERY, &vmw_cmd_dx_define_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_QUERY, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_QUERY, &vmw_cmd_dx_bind_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_QUERY_OFFSET,
		    &vmw_cmd_dx_set_query_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BEGIN_QUERY, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_END_QUERY, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_READBACK_QUERY, &vmw_cmd_dx_cid_check,
		    false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_PREDICATION, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SOTARGETS,
		    &vmw_cmd_dx_set_so_targets, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_VIEWPORTS, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SCISSORRECTS, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_CLEAR_RENDERTARGET_VIEW,
		    &vmw_cmd_dx_clear_rendertarget_view, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_CLEAR_DEPTHSTENCIL_VIEW,
		    &vmw_cmd_dx_clear_depthstencil_view, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_COPY_REGION,
		    &vmw_cmd_pred_copy_region, true, false, true),
	/* Mesa's whole-surface copy. */
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_COPY, &vmw_cmd_pred_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRESENTBLT, &vmw_cmd_presentblt,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_GENMIPS, &vmw_cmd_dx_genmips,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_UPDATE_SUBRESOURCE,
		    &vmw_cmd_sid_first, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_READBACK_SUBRESOURCE,
		    &vmw_cmd_sid_first, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_INVALIDATE_SUBRESOURCE,
		    &vmw_cmd_sid_first, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW,
		    &vmw_cmd_dx_view_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW,
		    &vmw_cmd_dx_view_remove, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
		    &vmw_cmd_dx_view_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
		    &vmw_cmd_dx_view_remove, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW,
		    &vmw_cmd_dx_view_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW,
		    &vmw_cmd_dx_view_remove, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_ELEMENTLAYOUT,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_ELEMENTLAYOUT,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_BLEND_STATE,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_BLEND_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_STATE,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_RASTERIZER_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_SAMPLER_STATE,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_SAMPLER_STATE,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_SHADER,
		    &vmw_cmd_dx_define_shader, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_SHADER,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_SHADER,
		    &vmw_cmd_dx_bind_shader, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_STREAMOUTPUT,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_STREAMOUTPUT,
		    &vmw_cmd_dx_cid_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BUFFER_COPY,
		    &vmw_cmd_buffer_copy_check, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_TRANSFER_FROM_BUFFER,
		    &vmw_cmd_transfer_from_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SURFACE_COPY_AND_READBACK,
		    &vmw_cmd_copy_and_readback, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_MOVE_QUERY, &vmw_cmd_dx_move_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_ALL_QUERY, &vmw_cmd_dx_bind_all_query,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_READBACK_ALL_QUERY,
		    &vmw_cmd_dx_readback_all_query, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_TRANSFER_FROM_BUFFER,
		    &vmw_cmd_transfer_from_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_MOB_FENCE_64, &vmw_cmd_dx_mob_fence_64,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_ALL_SHADER, &vmw_cmd_dx_bind_all_shader,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_HINT, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BUFFER_UPDATE, &vmw_cmd_sid_first,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_VS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_PS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_GS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_HS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_DS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_CS_CONSTANT_BUFFER_OFFSET,
		    &vmw_cmd_dx_set_constant_buffer_offset, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_INTRA_SURFACE_COPY, &vmw_cmd_intra_surface_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_RESOLVE_COPY, &vmw_cmd_resolve_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_RESOLVE_COPY, &vmw_cmd_resolve_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_CONVERT_REGION,
		    &vmw_cmd_pred_convert_region, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_CONVERT, &vmw_cmd_pred_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_WHOLE_SURFACE_COPY, &vmw_cmd_whole_surface_copy,
		    true, false, false),

	/*
	 * SM5 commands
	 */
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_UA_VIEW, &vmw_cmd_dx_view_define,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DESTROY_UA_VIEW, &vmw_cmd_dx_view_remove,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_CLEAR_UA_VIEW_UINT, &vmw_cmd_clear_uav,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_CLEAR_UA_VIEW_FLOAT, &vmw_cmd_clear_uav,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_COPY_STRUCTURE_COUNT,
		    &vmw_cmd_copy_structure_count, false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_UA_VIEWS, &vmw_cmd_set_uav,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED_INDIRECT,
		    &vmw_cmd_indirect, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DRAW_INSTANCED_INDIRECT,
		    &vmw_cmd_indirect, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DISPATCH, &vmw_cmd_sm5, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DISPATCH_INDIRECT,
		    &vmw_cmd_indirect, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_TRANSFER_TO_BUFFER,
		    &vmw_cmd_transfer_to_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DEFINE_GB_SURFACE_V4,
		    &vmw_cmd_invalid, false, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_CS_UA_VIEWS, &vmw_cmd_set_uav,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_MIN_LOD, &vmw_cmd_sid_first,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2,
		    &vmw_cmd_dx_view_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT_WITH_MOB,
		    &vmw_cmd_dx_define_streamoutput, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_SHADER_IFACE, &vmw_cmd_dx_cid_check,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_STREAMOUTPUT,
		    &vmw_cmd_dx_bind_streamoutput, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_BIND_SHADER_IFACE,
		    &vmw_cmd_dx_bind_shader_iface, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_STAGING_COPY, &vmw_cmd_staging_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_STAGING_COPY, &vmw_cmd_staging_copy,
		    true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_PRED_STAGING_COPY_REGION,
		    &vmw_cmd_pred_staging_copy_region, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS_V2,
		    &vmw_cmd_dx_set_vertex_buffers_v2, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_SET_INDEX_BUFFER_V2,
		    &vmw_cmd_dx_set_index_buffer, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE_V2,
		    &vmw_cmd_dx_so_define, true, false, true),
	VMW_CMD_DEF(SVGA_3D_CMD_DX_STAGING_BUFFER_COPY,
		    &vmw_cmd_buffer_copy_check, true, false, true),
};

static const char *vmw_cmd_name(uint32_t id)
{
	if (id >= SVGA_3D_CMD_BASE && id < SVGA_3D_CMD_MAX &&
	    vmw_cmd_entries[id - SVGA_3D_CMD_BASE].cmd_name)
		return vmw_cmd_entries[id - SVGA_3D_CMD_BASE].cmd_name;
	return "SVGA3D command";
}

int vmw_execbuf_cmd_check(struct vmw_val *val, void *buf, uint32_t avail,
			  uint32_t *size)
{
	SVGA3dCmdHeader *header = buf;
	const struct vmw_cmd_entry *entry;
	bool gb = val->v->has_gb;
	uint32_t cmd_id;
	int rc;

	*size = 0;
	if (avail < sizeof(uint32_t))
		return -EINVAL;
	cmd_id = ((uint32_t *)buf)[0];
	/* Handle any non-3D commands. */
	if (cmd_id < SVGA_CMD_MAX)
		return vmw_cmd_check_not_3d(val, buf, avail, size);

	if (avail < sizeof(*header))
		return -EINVAL;
	if (header->size & 3 || header->size > avail - sizeof(*header))
		return -EINVAL;
	*size = header->size + sizeof(*header);

	if (header->size > SVGA_CMD_MAX_DATASIZE) {
		rc = vmw_cmd_rule_err(val, header,
				      "larger than any command the device takes",
				      -E2BIG);
		if (rc)
			return rc;
	}
	if (cmd_id < SVGA_3D_CMD_BASE || cmd_id >= SVGA_3D_CMD_MAX)
		return -EINVAL;

	entry = &vmw_cmd_entries[cmd_id - SVGA_3D_CMD_BASE];
	/* Object-table commands (define/destroy/bind of surfaces, contexts,
	 * MOBs, OTables, screen targets) belong to the kernel; everything
	 * else without a handler is unknown. */
	if (!entry->func || entry->func == vmw_cmd_invalid)
		return -EINVAL;

	if (!entry->user_allow) {
		rc = vmw_cmd_rule_err(val, header, "not a command for clients",
				      -EPERM);
		if (rc)
			return rc;
	}
	if (entry->gb_disable && gb) {
		rc = vmw_cmd_rule(val, header,
				  "deprecated on a guest-backed device");
		if (rc)
			return rc;
	}
	if (entry->gb_enable && !gb) {
		rc = vmw_cmd_rule(val, header,
				  "needs a device with guest-backed objects");
		if (rc)
			return rc;
	}

	return entry->func(val, header);
}
