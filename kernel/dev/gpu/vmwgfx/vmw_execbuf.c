// LikeOS-64 -- vmwgfx: command stream submission.
//
// Userspace builds SVGA3D command streams naming its OWN handles: a surface
// handle where the device wants a surface id, a buffer handle where it
// wants a MOB or GMR id.  The device must never see a handle, and it must
// never see an object of another process, so every command is checked
// against a table that says where its ids sit and of what kind, the ids
// are rewritten to the device's, and each object touched is pinned until
// the fence the submission ends with has passed.  A command not in the
// table is refused: the stream is untrusted input.
#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
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

struct vmw_val {
	struct vmw_device *v;
	struct drm_file *fp;
	struct vmw_context *ctx; /* DX context of the stream, or NULL */
	struct drm_gem_object **refs;
	uint32_t nrefs;
	uint32_t refs_cap;
	/* Set of what refs[] already holds, so the duplicate check below is
	 * not a walk of it.  Open addressing, power-of-two, always twice
	 * refs_cap -- a load factor of one half, which both keeps the probe
	 * short and is what guarantees the loops terminate: the table can
	 * never fill, so an empty slot is always found. */
	struct drm_gem_object **htab;
	uint32_t hmask; /* size - 1 */
	uint32_t cid_seen; /* for legacy per-command cid */
};

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

/* Rewrite a surface handle field to the device sid. */
static int reloc_sid(struct vmw_val *val, uint32_t *field)
{
	if (*field == SVGA3D_INVALID_ID)
		return 0;
	struct drm_gem_object *o = val_obj(val, *field, DRM_GEM_SURFACE);
	if (!o)
		return -EINVAL;
	struct vmw_surface *s = o->priv;
	if (!s->bound && s->backup)
		vmw_surface_bind(val->v, s);
	*field = s->sid;
	return 0;
}

/* Rewrite a buffer handle field to the MOB id. */
static int reloc_mob(struct vmw_val *val, uint32_t *field)
{
	if (*field == SVGA3D_INVALID_ID)
		return 0;
	struct drm_gem_object *o = val_obj(val, *field, DRM_GEM_BO);
	if (!o)
		return -EINVAL;
	struct vmw_bo *b = o->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID)
		return -EINVAL;
	*field = b->mob.id;
	return 0;
}

/* Rewrite a buffer handle in a guest pointer to the GMR id. */
static int reloc_gmr(struct vmw_val *val, uint32_t *gmr_field)
{
	if (*gmr_field == SVGA_GMR_NULL || *gmr_field == SVGA_GMR_FRAMEBUFFER)
		return 0;
	struct drm_gem_object *o = val_obj(val, *gmr_field, DRM_GEM_BO);
	if (!o)
		return -EINVAL;
	struct vmw_bo *b = o->priv;
	if (!b || b->gmr_id < 0)
		return -EINVAL;
	*gmr_field = (uint32_t)b->gmr_id;
	return 0;
}

static int check_cid(struct vmw_val *val, uint32_t cid)
{
	/* Legacy commands carry the context id; it must be one of this
	 * file's.  DX streams carry it in the buffer header instead. */
	if (val->ctx)
		return 0;
	extern struct vmw_context *vmw_file_context(struct drm_file *fp, uint32_t cid);
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

#define BODY(T) T *b = (T *)body; if (size < sizeof(T)) return -EINVAL

static int validate_one(struct vmw_val *val, uint32_t id, uint8_t *body,
			uint32_t size)
{
	int rc = 0;

	switch (id) {
	/* ---- legacy (non-GB) ---- */
	case SVGA_3D_CMD_SURFACE_DMA: {
		BODY(SVGA3dCmdSurfaceDMA);
		rc = reloc_gmr(val, &b->guest.ptr.gmrId);
		if (!rc)
			rc = reloc_sid(val, &b->host.sid);
		return rc;
	}
	case SVGA_3D_CMD_SURFACE_COPY: {
		BODY(SVGA3dCmdSurfaceCopy);
		rc = reloc_sid(val, &b->src.sid);
		return rc ? rc : reloc_sid(val, &b->dest.sid);
	}
	case SVGA_3D_CMD_SURFACE_STRETCHBLT: {
		BODY(SVGA3dCmdSurfaceStretchBlt);
		rc = reloc_sid(val, &b->src.sid);
		return rc ? rc : reloc_sid(val, &b->dest.sid);
	}
	case SVGA_3D_CMD_INTRA_SURFACE_COPY: {
		BODY(SVGA3dCmdIntraSurfaceCopy);
		return reloc_sid(val, &b->surface.sid);
	}
	case SVGA_3D_CMD_WHOLE_SURFACE_COPY: {
		BODY(SVGA3dCmdWholeSurfaceCopy);
		rc = reloc_sid(val, &b->srcSid);
		return rc ? rc : reloc_sid(val, &b->destSid);
	}
	case SVGA_3D_CMD_PRESENT: {
		BODY(SVGA3dCmdPresent);
		return reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_GENERATE_MIPMAPS: {
		BODY(SVGA3dCmdGenerateMipmaps);
		return reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN: {
		BODY(SVGA3dCmdBlitSurfaceToScreen);
		return reloc_sid(val, &b->srcImage.sid);
	}
	case SVGA_3D_CMD_SETRENDERTARGET: {
		BODY(SVGA3dCmdSetRenderTarget);
		rc = check_cid(val, b->cid);
		return rc ? rc : reloc_sid(val, &b->target.sid);
	}
	case SVGA_3D_CMD_SETTEXTURESTATE: {
		BODY(SVGA3dCmdSetTextureState);
		rc = check_cid(val, b->cid);
		if (rc)
			return rc;
		/* Followed by SVGA3dTextureState[]: {stage, name, value}; a
		 * value for SVGA3D_TS_BIND_TEXTURE is a surface handle. */
		uint32_t n = (size - sizeof(*b)) / sizeof(SVGA3dTextureState);
		SVGA3dTextureState *ts = (SVGA3dTextureState *)(body + sizeof(*b));
		for (uint32_t i = 0; i < n; i++)
			if (ts[i].name == SVGA3D_TS_BIND_TEXTURE) {
				rc = reloc_sid(val, &ts[i].value);
				if (rc)
					return rc;
			}
		return 0;
	}
	case SVGA_3D_CMD_DRAW_PRIMITIVES: {
		BODY(SVGA3dCmdDrawPrimitives);
		rc = check_cid(val, b->cid);
		if (rc)
			return rc;
		uint32_t nd = b->numVertexDecls, nr = b->numRanges;
		if (nd > 32 || nr > 32)
			return -EINVAL;
		if (size < sizeof(*b) + nd * sizeof(SVGA3dVertexDecl) + nr * sizeof(SVGA3dPrimitiveRange))
			return -EINVAL;
		SVGA3dVertexDecl *d = (SVGA3dVertexDecl *)(body + sizeof(*b));
		for (uint32_t i = 0; i < nd; i++) {
			rc = reloc_sid(val, &d[i].array.surfaceId);
			if (rc)
				return rc;
		}
		SVGA3dPrimitiveRange *r = (SVGA3dPrimitiveRange *)(d + nd);
		for (uint32_t i = 0; i < nr; i++) {
			rc = reloc_sid(val, &r[i].indexArray.surfaceId);
			if (rc)
				return rc;
		}
		return 0;
	}
	case SVGA_3D_CMD_SET_VERTEX_STREAMS: {
		BODY(SVGA3dCmdSetVertexStreams);
		rc = check_cid(val, b->cid);
		if (rc)
			return rc;
		uint32_t n = b->numStreams;
		if (size < sizeof(*b) + n * sizeof(SVGA3dVertexStream))
			return -EINVAL;
		SVGA3dVertexStream *s = (SVGA3dVertexStream *)(body + sizeof(*b));
		for (uint32_t i = 0; i < n; i++) {
			rc = reloc_sid(val, &s[i].sid);
			if (rc)
				return rc;
		}
		return 0;
	}
	case SVGA_3D_CMD_DRAW_INDEXED: {
		BODY(SVGA3dCmdDrawIndexed);
		rc = check_cid(val, b->cid);
		return rc ? rc : reloc_sid(val, &b->indexBufferSid);
	}
	/* Context-id-only legacy state commands: check the cid, pass. */
	case SVGA_3D_CMD_SETTRANSFORM:
	case SVGA_3D_CMD_SETZRANGE:
	case SVGA_3D_CMD_SETRENDERSTATE:
	case SVGA_3D_CMD_SETMATERIAL:
	case SVGA_3D_CMD_SETLIGHTDATA:
	case SVGA_3D_CMD_SETLIGHTENABLED:
	case SVGA_3D_CMD_SETVIEWPORT:
	case SVGA_3D_CMD_SETCLIPPLANE:
	case SVGA_3D_CMD_CLEAR:
	case SVGA_3D_CMD_SET_SHADER:
	case SVGA_3D_CMD_SET_SHADER_CONST:
	case SVGA_3D_CMD_SETSCISSORRECT:
	case SVGA_3D_CMD_BEGIN_QUERY:
	case SVGA_3D_CMD_SET_VERTEX_DECLS:
	case SVGA_3D_CMD_SET_VERTEX_DIVISORS:
	case SVGA_3D_CMD_DRAW:
	case SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE:
	case SVGA_3D_CMD_BEGIN_GB_QUERY: {
		if (size < 4)
			return -EINVAL;
		return check_cid(val, *(uint32_t *)body);
	}
	case SVGA_3D_CMD_END_QUERY:
	case SVGA_3D_CMD_WAIT_FOR_QUERY: {
		/* {cid, type, SVGAGuestPtr guestResult} */
		if (size < 16)
			return -EINVAL;
		rc = check_cid(val, *(uint32_t *)body);
		return rc ? rc : reloc_gmr(val, (uint32_t *)(body + 8));
	}
	case SVGA_3D_CMD_END_GB_QUERY:
	case SVGA_3D_CMD_WAIT_FOR_GB_QUERY: {
		BODY(SVGA3dCmdEndGBQuery);
		rc = check_cid(val, b->cid);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_SHADER_DEFINE:
	case SVGA_3D_CMD_SHADER_DESTROY: {
		if (size < 4)
			return -EINVAL;
		return check_cid(val, *(uint32_t *)body);
	}

	/* ---- guest-backed ---- */
	case SVGA_3D_CMD_BIND_GB_SURFACE:
	case SVGA_3D_CMD_COND_BIND_GB_SURFACE: {
		BODY(SVGA3dCmdBindGBSurface);
		rc = reloc_sid(val, &b->sid);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_BIND_GB_SURFACE_WITH_PITCH: {
		BODY(SVGA3dCmdBindGBSurfaceWithPitch);
		rc = reloc_sid(val, &b->sid);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_UPDATE_GB_IMAGE:
	case SVGA_3D_CMD_READBACK_GB_IMAGE:
	case SVGA_3D_CMD_INVALIDATE_GB_IMAGE:
	case SVGA_3D_CMD_READBACK_GB_IMAGE_PARTIAL:
	case SVGA_3D_CMD_INVALIDATE_GB_IMAGE_PARTIAL: {
		if (size < sizeof(SVGA3dSurfaceImageId))
			return -EINVAL;
		return reloc_sid(val, &((SVGA3dSurfaceImageId *)body)->sid);
	}
	case SVGA_3D_CMD_UPDATE_GB_SURFACE:
	case SVGA_3D_CMD_READBACK_GB_SURFACE:
	case SVGA_3D_CMD_INVALIDATE_GB_SURFACE: {
		if (size < 4)
			return -EINVAL;
		return reloc_sid(val, (uint32_t *)body);
	}
	case SVGA_3D_CMD_BIND_GB_SHADER: {
		BODY(SVGA3dCmdBindGBShader);
		return reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_GB_MOB_FENCE: {
		BODY(SVGA3dCmdGBMobFence);
		return reloc_mob(val, &b->mobId);
	}
	case SVGA_3D_CMD_NOP:
	case SVGA_3D_CMD_NOP_ERROR:
		return 0;

	/* ---- DX ---- */
	case SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER: {
		BODY(SVGA3dCmdDXSetSingleConstantBuffer);
		return reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_SET_SHADER_RESOURCES:
	case SVGA_3D_CMD_DX_SET_SAMPLERS:
	case SVGA_3D_CMD_DX_SET_RENDERTARGETS:
	case SVGA_3D_CMD_DX_SET_SHADER:
	case SVGA_3D_CMD_DX_DRAW:
	case SVGA_3D_CMD_DX_DRAW_INDEXED:
	case SVGA_3D_CMD_DX_DRAW_INSTANCED:
	case SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED:
	case SVGA_3D_CMD_DX_DRAW_AUTO:
	case SVGA_3D_CMD_DX_SET_INPUT_LAYOUT:
	case SVGA_3D_CMD_DX_SET_TOPOLOGY:
	case SVGA_3D_CMD_DX_SET_BLEND_STATE:
	case SVGA_3D_CMD_DX_SET_DEPTHSTENCIL_STATE:
	case SVGA_3D_CMD_DX_SET_RASTERIZER_STATE:
	case SVGA_3D_CMD_DX_SET_PREDICATION:
	case SVGA_3D_CMD_DX_SET_VIEWPORTS:
	case SVGA_3D_CMD_DX_SET_SCISSORRECTS:
	case SVGA_3D_CMD_DX_CLEAR_RENDERTARGET_VIEW:
	case SVGA_3D_CMD_DX_CLEAR_DEPTHSTENCIL_VIEW:
	case SVGA_3D_CMD_DX_GENMIPS:
	case SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW:
	case SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW:
	case SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW:
	case SVGA_3D_CMD_DX_DESTROY_ELEMENTLAYOUT:
	case SVGA_3D_CMD_DX_DESTROY_BLEND_STATE:
	case SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_STATE:
	case SVGA_3D_CMD_DX_DESTROY_RASTERIZER_STATE:
	case SVGA_3D_CMD_DX_DESTROY_SAMPLER_STATE:
	case SVGA_3D_CMD_DX_DESTROY_SHADER:
	case SVGA_3D_CMD_DX_DESTROY_STREAMOUTPUT:
	case SVGA_3D_CMD_DX_SET_STREAMOUTPUT:
	case SVGA_3D_CMD_DX_DESTROY_QUERY:
	case SVGA_3D_CMD_DX_BEGIN_QUERY:
	case SVGA_3D_CMD_DX_END_QUERY:
	case SVGA_3D_CMD_DX_READBACK_QUERY:
	case SVGA_3D_CMD_DX_SET_QUERY_OFFSET:
	case SVGA_3D_CMD_DX_READBACK_ALL_QUERY:
	case SVGA_3D_CMD_DX_HINT:
	case SVGA_3D_CMD_DX_SET_VS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_SET_PS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_SET_GS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_SET_HS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_SET_DS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_SET_CS_CONSTANT_BUFFER_OFFSET:
	case SVGA_3D_CMD_DX_DESTROY_UA_VIEW:
	case SVGA_3D_CMD_DX_CLEAR_UA_VIEW_UINT:
	case SVGA_3D_CMD_DX_CLEAR_UA_VIEW_FLOAT:
	case SVGA_3D_CMD_DX_SET_UA_VIEWS:
	case SVGA_3D_CMD_DX_SET_CS_UA_VIEWS:
	case SVGA_3D_CMD_DX_DISPATCH:
	case SVGA_3D_CMD_DX_SET_SHADER_IFACE:
		return val->ctx ? 0 : -EINVAL;
	case SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS: {
		BODY(SVGA3dCmdDXSetVertexBuffers);
		uint32_t n = (size - sizeof(*b)) / sizeof(SVGA3dVertexBuffer);
		SVGA3dVertexBuffer *vb = (SVGA3dVertexBuffer *)(body + sizeof(*b));
		for (uint32_t i = 0; i < n; i++) {
			rc = reloc_sid(val, &vb[i].sid);
			if (rc)
				return rc;
		}
		return val->ctx ? 0 : -EINVAL;
	}
	case SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS_V2: {
		BODY(SVGA3dCmdDXSetVertexBuffers_v2);
		uint32_t n = (size - sizeof(*b)) / sizeof(SVGA3dVertexBuffer_v2);
		SVGA3dVertexBuffer_v2 *vb = (SVGA3dVertexBuffer_v2 *)(body + sizeof(*b));
		for (uint32_t i = 0; i < n; i++) {
			rc = reloc_sid(val, &vb[i].sid);
			if (rc)
				return rc;
		}
		return val->ctx ? 0 : -EINVAL;
	}
	case SVGA_3D_CMD_DX_SET_INDEX_BUFFER: {
		BODY(SVGA3dCmdDXSetIndexBuffer);
		return reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_SET_INDEX_BUFFER_V2: {
		BODY(SVGA3dCmdDXSetIndexBuffer_v2);
		return reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_SET_SOTARGETS: {
		BODY(SVGA3dCmdDXSetSOTargets);
		uint32_t n = (size - sizeof(*b)) / sizeof(SVGA3dSoTarget);
		SVGA3dSoTarget *t = (SVGA3dSoTarget *)(body + sizeof(*b));
		for (uint32_t i = 0; i < n; i++) {
			rc = reloc_sid(val, &t[i].sid);
			if (rc)
				return rc;
		}
		return val->ctx ? 0 : -EINVAL;
	}
	case SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW: {
		BODY(SVGA3dCmdDXDefineShaderResourceView);
		rc = cot(val, SVGA_COTABLE_SRVIEW, b->shaderResourceViewId);
		return rc ? rc : reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW: {
		BODY(SVGA3dCmdDXDefineRenderTargetView);
		rc = cot(val, SVGA_COTABLE_RTVIEW, b->renderTargetViewId);
		return rc ? rc : reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW:
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2: {
		BODY(SVGA3dCmdDXDefineDepthStencilView);
		rc = cot(val, SVGA_COTABLE_DSVIEW, b->depthStencilViewId);
		return rc ? rc : reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_DEFINE_UA_VIEW: {
		BODY(SVGA3dCmdDXDefineUAView);
		rc = cot(val, SVGA_COTABLE_UAVIEW, b->uaViewId);
		return rc ? rc : reloc_sid(val, &b->sid);
	}
	case SVGA_3D_CMD_DX_DEFINE_ELEMENTLAYOUT:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_ELEMENTLAYOUT, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_DEFINE_BLEND_STATE:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_BLENDSTATE, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_STATE:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_DEPTHSTENCIL, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE:
	case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE_V2:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_RASTERIZERSTATE, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_DEFINE_SAMPLER_STATE:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_SAMPLER, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_DEFINE_SHADER: {
		BODY(SVGA3dCmdDXDefineShader);
		return cot(val, SVGA_COTABLE_DXSHADER, b->shaderId);
	}
	case SVGA_3D_CMD_DX_BIND_SHADER: {
		BODY(SVGA3dCmdDXBindShader);
		rc = cot(val, SVGA_COTABLE_DXSHADER, b->shid);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_DX_BIND_ALL_SHADER: {
		BODY(SVGA3dCmdDXBindAllShader);
		return reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT:
	case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT_WITH_MOB:
		if (size < 4)
			return -EINVAL;
		return cot(val, SVGA_COTABLE_STREAMOUTPUT, *(uint32_t *)body);
	case SVGA_3D_CMD_DX_BIND_STREAMOUTPUT: {
		BODY(SVGA3dCmdDXBindStreamOutput);
		rc = cot(val, SVGA_COTABLE_STREAMOUTPUT, b->soid);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_DX_DEFINE_QUERY: {
		BODY(SVGA3dCmdDXDefineQuery);
		return cot(val, SVGA_COTABLE_DXQUERY, b->queryId);
	}
	case SVGA_3D_CMD_DX_BIND_QUERY: {
		BODY(SVGA3dCmdDXBindQuery);
		rc = cot(val, SVGA_COTABLE_DXQUERY, b->queryId);
		return rc ? rc : reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_DX_BIND_ALL_QUERY: {
		BODY(SVGA3dCmdDXBindAllQuery);
		return reloc_mob(val, &b->mobid);
	}
	case SVGA_3D_CMD_DX_MOVE_QUERY:
		return val->ctx ? 0 : -EINVAL;
	case SVGA_3D_CMD_DX_PRED_COPY_REGION: {
		BODY(SVGA3dCmdDXPredCopyRegion);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_PRED_COPY:
	case SVGA_3D_CMD_DX_PRED_CONVERT: {
		BODY(SVGA3dCmdDXPredCopy);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_PRED_CONVERT_REGION: {
		BODY(SVGA3dCmdDXPredConvertRegion);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_PRESENTBLT: {
		BODY(SVGA3dCmdDXPresentBlt);
		rc = reloc_sid(val, &b->srcSid);
		return rc ? rc : reloc_sid(val, &b->dstSid);
	}
	case SVGA_3D_CMD_DX_UPDATE_SUBRESOURCE:
	case SVGA_3D_CMD_DX_READBACK_SUBRESOURCE:
	case SVGA_3D_CMD_DX_INVALIDATE_SUBRESOURCE:
	case SVGA_3D_CMD_DX_BUFFER_UPDATE:
	case SVGA_3D_CMD_DX_SET_MIN_LOD:
		if (size < 4)
			return -EINVAL;
		return reloc_sid(val, (uint32_t *)body);
	case SVGA_3D_CMD_DX_TRANSFER_FROM_BUFFER:
	case SVGA_3D_CMD_DX_PRED_TRANSFER_FROM_BUFFER: {
		BODY(SVGA3dCmdDXTransferFromBuffer);
		rc = reloc_sid(val, &b->srcSid);
		return rc ? rc : reloc_sid(val, &b->destSid);
	}
	case SVGA_3D_CMD_DX_TRANSFER_TO_BUFFER: {
		BODY(SVGA3dCmdDXTransferToBuffer);
		rc = reloc_sid(val, &b->srcSid);
		return rc ? rc : reloc_sid(val, &b->destSid);
	}
	case SVGA_3D_CMD_DX_BUFFER_COPY:
	case SVGA_3D_CMD_DX_STAGING_BUFFER_COPY: {
		BODY(SVGA3dCmdDXBufferCopy);
		rc = reloc_sid(val, &b->dest);
		return rc ? rc : reloc_sid(val, &b->src);
	}
	case SVGA_3D_CMD_DX_SURFACE_COPY_AND_READBACK: {
		BODY(SVGA3dCmdDXSurfaceCopyAndReadback);
		rc = reloc_sid(val, &b->srcSid);
		return rc ? rc : reloc_sid(val, &b->destSid);
	}
	case SVGA_3D_CMD_DX_RESOLVE_COPY:
	case SVGA_3D_CMD_DX_PRED_RESOLVE_COPY: {
		BODY(SVGA3dCmdDXResolveCopy);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_STAGING_COPY:
	case SVGA_3D_CMD_DX_PRED_STAGING_COPY: {
		BODY(SVGA3dCmdDXStagingCopy);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_PRED_STAGING_COPY_REGION: {
		BODY(SVGA3dCmdDXPredStagingCopyRegion);
		rc = reloc_sid(val, &b->dstSid);
		return rc ? rc : reloc_sid(val, &b->srcSid);
	}
	case SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED_INDIRECT:
	case SVGA_3D_CMD_DX_DRAW_INSTANCED_INDIRECT:
	case SVGA_3D_CMD_DX_DISPATCH_INDIRECT: {
		BODY(SVGA3dCmdDXDrawIndexedInstancedIndirect);
		return reloc_sid(val, &b->argsBufferSid);
	}
	case SVGA_3D_CMD_DX_COPY_STRUCTURE_COUNT: {
		BODY(SVGA3dCmdDXCopyStructureCount);
		return reloc_sid(val, &b->destSid);
	}
	case SVGA_3D_CMD_DX_MOB_FENCE_64: {
		BODY(SVGA3dCmdDXMobFence64);
		return reloc_mob(val, &b->mobId);
	}
	case SVGA_3D_CMD_DX_BIND_SHADER_IFACE: {
		BODY(SVGA3dCmdDXBindShaderIface);
		return reloc_mob(val, &b->mobid);
	}
	default:
		/* Object-table commands (define/destroy/bind of surfaces,
		 * contexts, MOBs, OTables, screen targets) belong to the
		 * kernel; everything else is unknown. */
		return -EINVAL;
	}
}

/* Name a rejected command, once per (command, error).
 *
 * The refusal itself stays where client errors belong -- in the ioctl's
 * return value -- and the walk below still does not log per call.  But a
 * bare EINVAL out of a stream of a hundred commands says only that ONE of
 * some thirty checks in validate_one() fired, and Mesa answers it by
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
	uint32_t dx_cid = SVGA3D_INVALID_ID;
	if (a->version >= 2 && a->context_handle != SVGA3D_INVALID_ID) {
		extern struct vmw_context *vmw_file_context(struct drm_file *fp, uint32_t cid);
		val->ctx = vmw_file_context(fp, a->context_handle);
		if (!val->ctx || !val->ctx->dx) {
			mm_write_unlock(&v->execbuf_lock);
			kfree(val);
			return -EINVAL;
		}
		dx_cid = val->ctx->cid;
	}

	uint8_t *cmds = NULL;  /* the client's stream, rewritten in place */
	uint8_t *batch = NULL; /* stream with coherent updates ahead of it */
	if (a->command_size) {
		cmds = kalloc(a->command_size);
		if (!cmds) {
			mm_write_unlock(&v->execbuf_lock);
			kfree(val);
			return -ENOMEM;
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
		uint32_t off = 0;
		while (off < a->command_size) {
			if (a->command_size - off < sizeof(SVGA3dCmdHeader)) {
				rc = -EINVAL;
				goto out;
			}
			SVGA3dCmdHeader *h = (SVGA3dCmdHeader *)(cmds + off);
			uint32_t bsize = h->size;
			if (bsize & 3 || bsize > a->command_size - off - sizeof(*h)) {
				rc = -EINVAL;
				goto out;
			}
			rc = validate_one(val, h->id, cmds + off + sizeof(*h),
					  bsize);
			if (rc) {
				/* The refusal is a CLIENT error and is
				 * reported where those belong, in the ioctl's
				 * return value.  What goes to the console is
				 * only WHICH command was refused, once per
				 * (command, error) -- see the note above the
				 * reporter.  Logging every rejected handle
				 * filled the console the moment anything
				 * probed the interface; this cannot. */
				vmw_execbuf_note_reject(h->id, bsize, rc);
				goto out;
			}
			off += sizeof(*h) + bsize;
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
	 * visible.  The reference driver write-protects the mapping,
	 * collects the dirtied boxes and emits updates for exactly those at
	 * every submission that references the surface; with no
	 * write-protection machinery here the whole surface is treated as
	 * dirty instead, which transfers more bytes to the same effect.
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

	/* Asynchronous: the caller's fence is what tells it when the work is
	 * done, and it is submitted through the same channel just below, so
	 * it cannot pass before this batch.  Waiting here instead put every
	 * client's rendering thread to sleep for the host's execution time
	 * of every batch it submitted. */
	if (rc == 0 && (a->command_size || pre)) {
		rc = vmw_cmd_submit_async(v, start, a->command_size + pre, dx_cid);
	}
	/* A DEVICE rejection is not the client's errno.  The reference driver
	 * never reports execution errors through this ioctl -- the commands
	 * were validated and queued, and what the device later makes of them
	 * goes to the log, not to the caller: Mesa's winsys treats any error
	 * here as unrecoverable and calls abort(), so returning the device's
	 * verdict turns one dropped draw into a dead client (and under X,
	 * into the session-wide wreckage of crashing clients).  The channel
	 * has already been restarted by cb_submit_raw(); the batch is lost,
	 * which the log records, and the client draws on.  The kernel's OWN
	 * validation failures above still fail the ioctl -- those mean the
	 * stream never reached the device at all. */
	if (rc == -EINVAL || rc == -EIO) {
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
		rep.mask = fence->flags;
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

	for (uint32_t i = 0; i < val->nrefs; i++)
		drm_gem_put(val->refs[i]);
	if (cmds)
		kfree(cmds);
	if (batch)
		kfree(batch);
	kfree(val->refs);
	kfree(val->htab);
	kfree(val);
	mm_write_unlock(&v->execbuf_lock);
	return rc;
}

int vmw_execbuf(struct vmw_device *v, struct drm_file *fp,
		struct drm_vmw_execbuf_arg *a)
{
	return vmw_execbuf_do(v, fp, a);
}
