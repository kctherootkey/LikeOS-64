// LikeOS-64 -- vmwgfx: guest-backed surfaces.
//
// A surface is the device's texture / render target / buffer object; its
// contents live in a MOB-backed buffer object the driver allocates
// ("backup") and binds to it.  Userspace names it by a handle, maps its
// backup, and shares it across processes as a dma-buf (the surface's
// creation parameters travel with it: GB_SURFACE_REF returns them).
#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- size of a surface, as the device serialises it ------------------ */

static uint32_t divup(uint32_t a, uint32_t b)
{
	return (a + b - 1) / b;
}

static uint32_t mip_dim(uint32_t base, uint32_t level)
{
	uint32_t d = base >> level;
	return d ? d : 1;
}

/* Bytes of one mip level: blocks across x rows x slices, rows padded to the
 * pitch block size. */
static uint32_t level_bytes(const SVGA3dSurfaceDesc *d, uint32_t w, uint32_t h,
			    uint32_t z)
{
	uint32_t bw = divup(w, d->blockSize.width);
	uint32_t bh = divup(h, d->blockSize.height);
	uint32_t bd = divup(z, d->blockSize.depth);

	/* A planar format stores its planes one after another and its block
	 * carries all of them, so its size comes from the block's STORAGE
	 * size and there is no row pitch to speak of.  Everything else is
	 * rows of pitch.  This is the split the reference's image-size helper
	 * makes, and it has to be made identically here and in the dirty
	 * tracker's surf_image_bytes(): this function decides how big the
	 * backing buffer is, that one decides where in it every row lives, and
	 * for NV12 and YV12 -- the only formats whose two per-block sizes
	 * differ, 6 against 2 -- they disagreed by a factor of three. */
	if (d->blockDesc & SVGA3DBLOCKDESC_PLANAR_YUV)
		return bw * bh * bd * d->bytesPerBlock;
	return (bw * d->pitchBytesPerBlock) * bh * bd;
}

uint32_t vmw_surface_size(uint32_t format, const SVGA3dSize *size,
			  uint32_t mip_levels, uint32_t array_size,
			  uint32_t samples, uint64_t flags)
{
	if (format >= SVGA3D_FORMAT_MAX)
		return 0;
	const SVGA3dSurfaceDesc *d = &g_SVGA3dSurfaceDescs[format];
	/* A format number inside the enum is not necessarily a format: the
	 * range has holes where formats were removed (SVGA3D_FORMAT_DEAD1 is
	 * one, and it sits at 23, right among the live ones).  Those entries
	 * carry a block size, so bytesPerBlock alone lets them through and
	 * the surface is then defined with a format the device refuses --
	 * which halts the command-buffer context, not just this call.  The
	 * table says what is real: a format with no block description is
	 * not.  */
	if (d->blockDesc == SVGA3DBLOCKDESC_NONE || d->bytesPerBlock == 0)
		return 0;
	uint64_t total = 0;
	/* A cubemap carries no array size and six faces.  Counting it as one
	 * layer sized the backing buffer at a sixth of what the surface needs,
	 * while the dirty tracker's layout -- which applies the same rule the
	 * reference does -- addressed all six.  The device, told the surface
	 * is a cubemap, reads the same six.  See vmw_surface_dirty_alloc(). */
	uint32_t layers = array_size ? array_size :
			  ((flags & SVGA3D_SURFACE_CUBEMAP) ?
				   SVGA3D_MAX_SURFACE_FACES : 1);
	if (samples < 1)
		samples = 1;
	for (uint32_t m = 0; m < mip_levels; m++) {
		uint32_t w = mip_dim(size->width, m);
		uint32_t h = mip_dim(size->height, m);
		uint32_t z = mip_dim(size->depth, m);
		total += (uint64_t)level_bytes(d, w, h, z) * samples;
	}
	total *= layers;
	if (total > 0xFFFFFFFFULL)
		return 0;
	return (uint32_t)total;
}

/* ---- ids ------------------------------------------------------------- */

int vmw_surface_alloc_id(struct vmw_device *v)
{
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	int id = vmw_id_alloc(v->surface_ids, VMW_NUM_SURFACES);
	spin_unlock_irqrestore(&v->id_lock, fl);
	return id;
}

void vmw_surface_free_id(struct vmw_device *v, uint32_t sid)
{
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	vmw_id_free(v->surface_ids, sid);
	spin_unlock_irqrestore(&v->id_lock, fl);
}

/* ---- device commands ----------------------------------------------------- */

int vmw_surface_define(struct vmw_device *v, struct vmw_surface *s)
{
	int rc;

	if (s->defined)
		return 0;

	/* Only a surface meant for scan-out is defined synchronously: it is
	 * the one whose answer is acted on, because the screen-target flag
	 * this driver adds may be refused and the surface is then defined
	 * again without it.  There is one of those per framebuffer.  Every
	 * other surface -- a pixmap, of which there are a great many -- is
	 * queued like the rest of its life. */
	int sync = s->scanout;

	/* Which DEFINE the device takes is a property of the shader model it
	 * runs, not of DX on its own: V4 belongs to SM5, V3 to SM4.1, and the
	 * array form V2 to DX.  A surface with no array layer is defined with
	 * the original command, which every guest-backed device has.
	 *
	 * This used to send V4 to anything with DX, which a device that stops
	 * at SM4 or SM4.1 answers with a command error -- and a command error
	 * halts the command-buffer context, so getting this wrong costs the
	 * display and not just the surface. */
	if (v->has_sm5 && s->array_size > 0) {
		SVGA3dCmdDefineGBSurface_v4 c;
		mm_memset(&c, 0, sizeof(c));
		c.sid = s->sid;
		c.surfaceFlags = s->flags;
		c.format = s->format;
		c.numMipLevels = s->mip_levels;
		c.multisampleCount = s->multisample_count;
		c.multisamplePattern = s->multisample_pattern;
		c.qualityLevel = s->quality_level;
		c.autogenFilter = s->autogen_filter;
		c.size = s->base_size;
		c.arraySize = s->array_size;
		c.bufferByteStride = s->byte_stride;
		rc = sync ? vmw_cmd_one_sync(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V4, &c, sizeof(c)) :
			    vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V4, &c, sizeof(c));
	} else if (v->has_sm41 && s->array_size > 0) {
		SVGA3dCmdDefineGBSurface_v3 c;
		mm_memset(&c, 0, sizeof(c));
		c.sid = s->sid;
		c.surfaceFlags = s->flags;
		c.format = s->format;
		c.numMipLevels = s->mip_levels;
		c.multisampleCount = s->multisample_count;
		c.multisamplePattern = s->multisample_pattern;
		c.qualityLevel = s->quality_level;
		c.autogenFilter = s->autogen_filter;
		c.size = s->base_size;
		c.arraySize = s->array_size;
		rc = sync ? vmw_cmd_one_sync(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V3, &c, sizeof(c)) :
			    vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V3, &c, sizeof(c));
	} else if (s->array_size > 0) {
		SVGA3dCmdDefineGBSurface_v2 c;
		mm_memset(&c, 0, sizeof(c));
		c.sid = s->sid;
		c.surfaceFlags = (uint32_t)s->flags;
		c.format = s->format;
		c.numMipLevels = s->mip_levels;
		c.multisampleCount = s->multisample_count;
		c.autogenFilter = s->autogen_filter;
		c.size = s->base_size;
		c.arraySize = s->array_size;
		rc = sync ? vmw_cmd_one_sync(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V2, &c, sizeof(c)) :
			    vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SURFACE_V2, &c, sizeof(c));
	} else {
		SVGA3dCmdDefineGBSurface c;
		mm_memset(&c, 0, sizeof(c));
		c.sid = s->sid;
		c.surfaceFlags = (uint32_t)s->flags;
		c.format = s->format;
		c.numMipLevels = s->mip_levels;
		c.multisampleCount = s->multisample_count;
		c.autogenFilter = s->autogen_filter;
		c.size = s->base_size;
		rc = sync ? vmw_cmd_one_sync(v, SVGA_3D_CMD_DEFINE_GB_SURFACE, &c, sizeof(c)) :
			    vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SURFACE, &c, sizeof(c));
	}
	if (rc)
		return rc;
	s->defined = 1;
	return 0;
}

int vmw_surface_bind(struct vmw_device *v, struct vmw_surface *s)
{
	if (s->bound || !s->backup)
		return 0;
	struct vmw_bo *b = s->backup->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID)
		return -ENODEV;
	SVGA3dCmdBindGBSurface c;
	c.sid = s->sid;
	c.mobid = b->mob.id;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_SURFACE, &c, sizeof(c));
	if (rc)
		return rc;
	s->bound = 1;
	return 0;
}

void vmw_surface_destroy(struct vmw_device *v, struct vmw_surface *s)
{
	if (s->defined) {
		/* Guest-backed and legacy surfaces are torn down by different
		 * commands; the bodies are the same single id. */
		SVGA3dCmdDestroyGBSurface c;
		c.sid = s->sid;
		/* Synchronous: the device has executed the destroy by the
		 * time this returns, so there is nothing left to wait for.
		 * Emitting a fence and waiting on it as well stalled every
		 * surface teardown for as long as that fence took to come
		 * back -- and a display server destroys surfaces constantly,
		 * so the pause landed in the middle of dragging a window. */
		vmw_cmd_one(v,
			    s->legacy ? SVGA_3D_CMD_SURFACE_DESTROY
				      : SVGA_3D_CMD_DESTROY_GB_SURFACE,
			    &c, sizeof(c));
		s->defined = 0;
	}
	if (s->backup) {
		/* The tracking taken at creation for a coherent surface goes
		 * with the surface, before the backup reference: the tracker
		 * lives on the buffer and this surface's share of it ends
		 * here. */
		if (s->coherent) {
			drm_gem_dirty_release(s->backup);
			vmw_surface_dirty_free(s);
			s->coherent = 0;
		}
		drm_gem_put(s->backup);
		s->backup = NULL;
	}
	if (s->sid && s->sid != SVGA3D_INVALID_ID) {
		/* Forget any cached screen-target binding on this id before
		 * handing it back.  Ids come from a bitmap that returns the
		 * LOWEST free one, so the very next surface created inherits
		 * it -- and a present of that surface then finds the cached
		 * id already matching, skips BIND_GB_SCREENTARGET, and leaves
		 * the screen target pointing at a surface that no longer
		 * exists.  Nothing fails and nothing is logged; the screen
		 * simply stops changing.  X and glamor recycle pixmap
		 * surfaces constantly, so this happens within moments of the
		 * first frame. */
		if (v->st_bound_sid == s->sid)
			v->st_bound_sid = SVGA3D_INVALID_ID;
		vmw_surface_free_id(v, s->sid);
	}
	kfree(s);
}

/* A surface's drm object: kind SURFACE, priv = vmw_surface.  Its pages
 * are the backup's, so mmap / dma-buf of a surface object maps the
 * backup. */
struct drm_gem_object *vmw_surface_object_create(struct vmw_device *v,
						 struct vmw_surface *s)
{
	struct drm_gem_object *o = drm_gem_alloc(&v->drm, DRM_GEM_SURFACE,
						 s->backup_size ? s->backup_size : PAGE_SIZE);
	if (!o)
		return NULL;
	o->priv = s;
	o->backend_id = s->sid;
	o->scanout = s->scanout;
	o->width = s->base_size.width;
	o->height = s->base_size.height;
	o->format = s->format;
	/* Share the backup's page array (owned by the backup). */
	if (s->backup) {
		o->pages = s->backup->pages;
		o->npages = s->backup->npages;
	}
	return o;
}

/* ---- the create / ref ioctls -------------------------------------------- */

/* What rules a surface out as a screen target.  Spelled out rather than
 * taken from SVGA3D_SURFACE_SCREENTARGET_DISALLOWED_MASK in the device
 * header: that mask names flags this header revision does not define. */
#define VMW_SCREENTARGET_DISALLOWED                                            \
	(SVGA3D_SURFACE_CUBEMAP | SVGA3D_SURFACE_AUTOGENMIPMAPS |              \
	 SVGA3D_SURFACE_VOLUME | SVGA3D_SURFACE_1D |                           \
	 SVGA3D_SURFACE_BIND_VERTEX_BUFFER |                                   \
	 SVGA3D_SURFACE_BIND_INDEX_BUFFER |                                    \
	 SVGA3D_SURFACE_BIND_CONSTANT_BUFFER |                                 \
	 SVGA3D_SURFACE_BIND_DEPTH_STENCIL |                                   \
	 SVGA3D_SURFACE_BIND_STREAM_OUTPUT | SVGA3D_SURFACE_INACTIVE |         \
	 SVGA3D_SURFACE_STAGING_UPLOAD | SVGA3D_SURFACE_STAGING_DOWNLOAD |     \
	 SVGA3D_SURFACE_HINT_INDIRECT_UPDATE |                                 \
	 SVGA3D_SURFACE_TRANSFER_FROM_BUFFER | SVGA3D_SURFACE_MULTISAMPLE |    \
	 SVGA3D_SURFACE_TRANSFER_TO_BUFFER | SVGA3D_SURFACE_BIND_RAW_VIEWS |   \
	 SVGA3D_SURFACE_BUFFER_STRUCTURED |                                    \
	 SVGA3D_SURFACE_DRAWINDIRECT_ARGS | SVGA3D_SURFACE_RESOURCE_CLAMP |    \
	 SVGA3D_SURFACE_STAGING_COPY)

/* The formats a screen target can scan out, as upstream lists them: the two
 * legacy 32-bit ones and the DX B/R 8888 pair. */
int vmw_format_is_screen_target(uint32_t f)
{
	return f == SVGA3D_X8R8G8B8 || f == SVGA3D_A8R8G8B8 ||
	       f == SVGA3D_R8G8B8A8_UNORM || f == SVGA3D_B8G8R8A8_UNORM ||
	       f == SVGA3D_B8G8R8X8_UNORM;
}

static int surface_create_common(struct vmw_device *v, struct drm_file *fp,
				 const struct drm_vmw_gb_surface_create_ext_req *req,
				 int ext, struct drm_vmw_gb_surface_create_rep *rep)
{
	const struct drm_vmw_gb_surface_create_req *b = &req->base;

	if (b->format >= SVGA3D_FORMAT_MAX || b->mip_levels == 0 ||
	    b->mip_levels > DRM_VMW_MAX_MIP_LEVELS)
		return -EINVAL;
	if (b->base_size.width == 0 || b->base_size.height == 0 ||
	    b->base_size.depth == 0)
		return -EINVAL;
	/* The upper half of the 64-bit surface flags only reaches the device
	 * through the V3/V4 DEFINE, which is SM4.1 and up.  Below that they
	 * would be silently dropped and the caller would get a surface it did
	 * not ask for. */
	if (ext && req->svga3d_flags_upper_32_bits && !v->has_sm41)
		return -EINVAL;
	uint32_t samples = b->multisample_count ? b->multisample_count : 1;
	uint64_t sflags = b->svga3d_flags;
	if (ext)
		sflags |= (uint64_t)req->svga3d_flags_upper_32_bits << 32;
	uint32_t size = vmw_surface_size(b->format, (const SVGA3dSize *)&b->base_size,
					 b->mip_levels, b->array_size, samples,
					 sflags);
	if (size == 0)
		return -EINVAL;
	if (v->max_mob_size && size > v->max_mob_size)
		return -ENOMEM;

	struct vmw_surface *s = kalloc(sizeof(*s));
	if (!s)
		return -ENOMEM;
	mm_memset(s, 0, sizeof(*s));
	int sid = vmw_surface_alloc_id(v);
	if (sid < 0) {
		kfree(s);
		return -ENOSPC;
	}
	s->sid = (uint32_t)sid;
	s->flags = b->svga3d_flags;
	if (ext)
		s->flags |= (uint64_t)req->svga3d_flags_upper_32_bits << 32;
	s->format = b->format;
	s->mip_levels = b->mip_levels;
	s->multisample_count = b->multisample_count;
	s->multisample_pattern = ext ? req->multisample_pattern : 0;
	s->quality_level = ext ? req->quality_level : 0;
	s->autogen_filter = b->autogen_filter;
	s->base_size.width = b->base_size.width;
	s->base_size.height = b->base_size.height;
	s->base_size.depth = b->base_size.depth;
	s->array_size = b->array_size;
	s->byte_stride = ext ? req->buffer_byte_stride : 0;
	s->backup_size = size;
	s->scanout = (b->drm_surface_flags & drm_vmw_surface_flag_scanout) != 0;
	s->shareable = (b->drm_surface_flags & drm_vmw_surface_flag_shareable) != 0;
	/* The coherence contract: the client maps this surface's backing for
	 * good and STOPS sending update commands for it -- its GL stack skips
	 * every explicit upload for a persistently-mapped buffer and leaves
	 * making the CPU's writes visible to the kernel.  The reference
	 * driver honours that by write-protecting the mapping, collecting
	 * dirtied ranges, and emitting the update commands itself at every
	 * submission that references the surface.  Ignoring the flag is not
	 * an option with teeth so much as a jaw: the compositor's entire
	 * vertex stream lives in such buffers, so every one of its draws
	 * reads data the device never received and renders nothing --
	 * silently.  vmw_execbuf() carries the sync (see there). */
	s->coherent = (b->drm_surface_flags & drm_vmw_surface_flag_coherent) != 0;

	/* Backup buffer: the caller's, or a new one. */
	if (b->buffer_handle != SVGA3D_INVALID_ID) {
		struct drm_gem_object *bo = drm_gem_lookup(fp, b->buffer_handle);
		if (!bo || bo->kind != DRM_GEM_BO || bo->size < size) {
			if (bo)
				drm_gem_put(bo);
			vmw_surface_free_id(v, s->sid);
			kfree(s);
			return -EINVAL;
		}
		s->backup = bo; /* keeps the lookup reference */
	} else if (b->drm_surface_flags & drm_vmw_surface_flag_create_buffer) {
		struct drm_gem_object *bo = drm_gem_alloc(&v->drm, DRM_GEM_BO, size);
		if (bo)
			bo->scanout = s->scanout;
		if (!bo || drm_gem_alloc_pages(bo) || v->drm.drv->gem_init(bo)) {
			if (bo)
				drm_gem_put(bo);
			vmw_surface_free_id(v, s->sid);
			kfree(s);
			return -ENOMEM;
		}
		s->backup = bo;
	}

	/* Scan-out is asked for with the DRM flag; SVGA3D_SURFACE_SCREENTARGET
	 * is the device flag that makes the surface bindable to a screen
	 * target, and translating one into the other is the kernel's job.
	 * Mesa never sets it -- it passes drm_vmw_surface_flag_scanout and
	 * expects the display side to cope -- so without this the screen
	 * target could not scan a client surface out directly, and glamor's
	 * front buffer went down the fallback instead: a legacy SURFACE_COPY
	 * from its B8G8R8A8 surface into this driver's X8R8G8B8 display
	 * surface.  That is a cross-format copy of a DX surface through a
	 * pre-DX command, and the screen stayed black with everything else
	 * -- glamor, DRI2, DRI3, input -- working perfectly.
	 *
	 * Only where the device would accept it: a format a screen target can
	 * scan out, and no binding that rules screen targets out. */
	if (s->scanout && vmw_format_is_screen_target(s->format) &&
	    !(s->flags & VMW_SCREENTARGET_DISALLOWED))
		s->flags |= SVGA3D_SURFACE_SCREENTARGET |
			    SVGA3D_SURFACE_HINT_RENDERTARGET;

	/* A coherent surface is one whose backing the client writes through
	 * a persistent mapping and never announces, so the writes have to be
	 * watched from here on: the page tracker on the backup buffer, and
	 * the per-subresource boxes the pages are translated into at each
	 * submission (vmw_dirty.c, drm_dirty.c).  Taken up at creation and
	 * put down at destruction -- and if either part cannot be set up,
	 * the surface is refused rather than created quietly incoherent:
	 * the client would draw with data the device never received. */
	if (s->coherent) {
		int trc = s->backup ? vmw_surface_dirty_alloc(s) : -EINVAL;

		if (trc == 0) {
			trc = drm_gem_dirty_add(s->backup);
			if (trc)
				vmw_surface_dirty_free(s);
		}
		if (trc) {
			drm_gem_put(s->backup);
			s->backup = NULL;
			vmw_surface_free_id(v, s->sid);
			kfree(s);
			return trc;
		}
	}

	int rc = vmw_surface_define(v, s);
	if (rc && (s->flags & SVGA3D_SURFACE_SCREENTARGET)) {
		/* A host that will not take the screen-target flag beside
		 * these bindings must still get its surface: the client asked
		 * for a surface, scan-out was the kernel's embellishment. */
		s->flags &= ~(uint64_t)SVGA3D_SURFACE_SCREENTARGET;
		rc = vmw_surface_define(v, s);
	}
	if (rc == 0 && s->backup)
		rc = vmw_surface_bind(v, s);
	if (rc) {
		vmw_surface_destroy(v, s);
		return rc;
	}
	struct drm_gem_object *so = vmw_surface_object_create(v, s);
	if (!so) {
		vmw_surface_destroy(v, s);
		return -ENOMEM;
	}
	uint32_t handle = 0, bhandle = 0;
	rc = drm_gem_handle_create(fp, so, &handle);
	if (rc == 0 && s->backup && b->buffer_handle == SVGA3D_INVALID_ID)
		rc = drm_gem_handle_create(fp, s->backup, &bhandle);
	else if (s->backup)
		bhandle = b->buffer_handle;
	drm_gem_put(so); /* the handle holds it */
	if (rc)
		return rc;
	mm_memset(rep, 0, sizeof(*rep));
	rep->handle = handle;
	rep->backup_size = size;
	rep->buffer_handle = s->backup ? bhandle : SVGA3D_INVALID_ID;
	rep->buffer_size = s->backup ? (uint32_t)s->backup->size : 0;
	rep->buffer_map_handle = s->backup ? drm_gem_mmap_offset(s->backup) : 0;
	return 0;
}

static void surface_fill_req(const struct vmw_surface *s,
			     struct drm_vmw_gb_surface_create_ext_req *req)
{
	mm_memset(req, 0, sizeof(*req));
	req->base.svga3d_flags = (uint32_t)s->flags;
	req->svga3d_flags_upper_32_bits = (uint32_t)(s->flags >> 32);
	req->base.format = s->format;
	req->base.mip_levels = s->mip_levels;
	req->base.drm_surface_flags = (s->shareable ? drm_vmw_surface_flag_shareable : 0) |
				      (s->scanout ? drm_vmw_surface_flag_scanout : 0);
	req->base.multisample_count = s->multisample_count;
	req->base.autogen_filter = s->autogen_filter;
	req->base.array_size = s->array_size;
	req->base.base_size.width = s->base_size.width;
	req->base.base_size.height = s->base_size.height;
	req->base.base_size.depth = s->base_size.depth;
	req->multisample_pattern = s->multisample_pattern;
	req->quality_level = s->quality_level;
	req->buffer_byte_stride = s->byte_stride;
}

/* Reference by handle (LEGACY) or by dma-buf descriptor (PRIME): a handle
 * in this file for the surface and for its backup, plus the parameters. */
static int surface_ref_common(struct vmw_device *v, struct drm_file *fp,
			      const struct drm_vmw_surface_arg *arg,
			      struct drm_vmw_gb_surface_create_ext_req *creq,
			      struct drm_vmw_gb_surface_create_rep *crep)
{
	struct drm_gem_object *so;

	if (arg->handle_type == DRM_VMW_HANDLE_PRIME) {
		so = drm_prime_import(arg->sid);
	} else {
		so = drm_gem_lookup(fp, (uint32_t)arg->sid);
		/* A shared surface is named by the id its CREATOR holds, and
		 * the process referencing it has none of its own yet -- that
		 * is what it is asking for.  Handles carry the file they
		 * belong to, so the object can be found whoever created it.
		 *
		 * Not from a render node: a render client has no display
		 * server to have been handed an id by, and letting it name
		 * any id would let it reach another process's surfaces.  The
		 * reference implementation draws the line in the same place
		 * (a render client must already hold the object). */
		if (!so && !fp->is_render)
			so = drm_gem_lookup_foreign(&v->drm, (uint32_t)arg->sid);
	}
	if (!so)
		return -ENOENT;
	if (so->kind != DRM_GEM_SURFACE) {
		drm_gem_put(so);
		return -EINVAL;
	}
	struct vmw_surface *s = so->priv;
	uint32_t handle = 0, bhandle = 0;
	int rc = 0;
	/* ALWAYS a new handle, for the by-handle case too.  A reference must
	 * hand the caller something of its OWN, because the caller will
	 * UNREF what it was handed: handing back the caller's original
	 * handle -- as this did -- made that UNREF delete the original, and
	 * with it the last kernel reference to a surface its owner was still
	 * using.  The surface was destroyed, its sid went back to the bitmap
	 * (lowest-free), the NEXT surface created inherited both the sid and
	 * the freed handle slot -- and the owner's rendering now named a
	 * different object without a single call failing.  Found as: glamor's
	 * scan-out surface turning into Mesa's vertex buffer after a
	 * REF+UNREF pair, and the first DX_DRAW rejected by the device
	 * because its render target had silently become a buffer. */
	rc = drm_gem_handle_create(fp, so, &handle);
	if (rc == 0 && s->backup)
		rc = drm_gem_handle_create(fp, s->backup, &bhandle);
	drm_gem_put(so);
	if (rc)
		return rc;
	surface_fill_req(s, creq);
	creq->base.buffer_handle = s->backup ? bhandle : SVGA3D_INVALID_ID;
	mm_memset(crep, 0, sizeof(*crep));
	crep->handle = handle;
	crep->backup_size = s->backup_size;
	crep->buffer_handle = s->backup ? bhandle : SVGA3D_INVALID_ID;
	crep->buffer_size = s->backup ? (uint32_t)s->backup->size : 0;
	crep->buffer_map_handle = s->backup ? drm_gem_mmap_offset(s->backup) : 0;
	return 0;
}

long vmw_ioctl_gb_surface_create(struct vmw_device *v, struct drm_file *fp,
				 void *kb, int ext)
{
	struct drm_vmw_gb_surface_create_ext_req req;
	struct drm_vmw_gb_surface_create_rep rep;

	if (ext) {
		union drm_vmw_gb_surface_create_ext_arg *a = kb;
		req = a->req;
		if (req.version != drm_vmw_gb_surface_v1 || req.must_be_zero)
			return -EINVAL;
	} else {
		union drm_vmw_gb_surface_create_arg *a = kb;
		mm_memset(&req, 0, sizeof(req));
		req.base = a->req;
	}
	int rc = surface_create_common(v, fp, &req, ext, &rep);
	if (rc)
		return rc;
	if (ext)
		((union drm_vmw_gb_surface_create_ext_arg *)kb)->rep = rep;
	else
		((union drm_vmw_gb_surface_create_arg *)kb)->rep = rep;
	return 0;
}

long vmw_ioctl_gb_surface_ref(struct vmw_device *v, struct drm_file *fp,
			      void *kb, int ext)
{
	struct drm_vmw_surface_arg arg;
	struct drm_vmw_gb_surface_create_ext_req creq;
	struct drm_vmw_gb_surface_create_rep crep;

	if (ext)
		arg = ((union drm_vmw_gb_surface_reference_ext_arg *)kb)->req;
	else
		arg = ((union drm_vmw_gb_surface_reference_arg *)kb)->req;
	int rc = surface_ref_common(v, fp, &arg, &creq, &crep);
	if (rc)
		return rc;
	if (ext) {
		union drm_vmw_gb_surface_reference_ext_arg *a = kb;
		a->rep.creq = creq;
		a->rep.crep = crep;
	} else {
		union drm_vmw_gb_surface_reference_arg *a = kb;
		a->rep.creq = creq.base;
		a->rep.crep = crep;
	}
	return 0;
}

long vmw_ioctl_unref_surface(struct vmw_device *v, struct drm_file *fp, void *kb)
{
	struct drm_vmw_surface_arg *a = kb;
	(void)v;
	return drm_gem_handle_delete(fp, (uint32_t)a->sid);
}

/* Called from the driver's gem_free for surface objects. */
void vmw_surface_gem_free(struct vmw_device *v, struct drm_gem_object *o)
{
	struct vmw_surface *s = o->priv;

	o->pages = NULL; /* the backup owns them */
	o->npages = 0;
	if (s)
		vmw_surface_destroy(v, s);
	o->priv = NULL;
}

/* ---- legacy (non-guest-backed) surfaces --------------------------------- */
//
// On a host without guest-backed objects there are no MOBs and no GB
// surfaces: a surface is defined by a command that carries its own geometry,
// it lives entirely in host memory, and the guest moves pixels in and out of
// it with SURFACE_DMA out of a GMR.  That is the whole 3D interface of the
// older devices, and it is what Mesa's svga driver falls back to when
// DRM_VMW_PARAM_HW_CAPS reports no SVGA_CAP_GBOBJECTS.
//
// Nothing here is guest-backed, so there is no backup buffer and no bind;
// what the handle names is a host object with an id.  The rest of the driver
// is unchanged: the object is a DRM_GEM_SURFACE like any other, and the
// execbuf validator relocates a handle to its sid the same way.

/* The mip-level sizes the caller passed, flattened per face. */
static int legacy_surface_sizes(const struct drm_vmw_surface_create_req *req,
				struct drm_vmw_size *out, uint32_t cap,
				uint32_t *total)
{
	uint32_t n = 0;

	for (int f = 0; f < DRM_VMW_MAX_SURFACE_FACES; f++) {
		if (req->mip_levels[f] > DRM_VMW_MAX_MIP_LEVELS)
			return -EINVAL;
		n += req->mip_levels[f];
	}
	if (!n || n > cap)
		return -EINVAL;
	if (drm_copy_from_user(out, (const void *)(uintptr_t)req->size_addr,
			       n * sizeof(*out)) != 0)
		return -EFAULT;
	*total = n;
	return 0;
}

long vmw_ioctl_create_surface(struct vmw_device *v, struct drm_file *fp, void *kb)
{
	union drm_vmw_surface_create_arg *a = kb;
	struct drm_vmw_surface_create_req *req = &a->req;
	struct drm_vmw_size sizes[DRM_VMW_MAX_SURFACE_FACES * DRM_VMW_MAX_MIP_LEVELS];
	uint32_t nsizes = 0;
	int rc;

	if (!v->has_3d)
		return -ENODEV;
	rc = legacy_surface_sizes(req, sizes, (uint32_t)(sizeof(sizes) / sizeof(sizes[0])),
				  &nsizes);
	if (rc)
		return rc;

	struct vmw_surface *s = kalloc(sizeof(*s));
	if (!s)
		return -ENOMEM;
	mm_memset(s, 0, sizeof(*s));
	int sid = vmw_surface_alloc_id(v);
	if (sid < 0) {
		kfree(s);
		return -ENOSPC;
	}
	s->sid = (uint32_t)sid;
	s->flags = req->flags;
	s->format = req->format;
	s->mip_levels = req->mip_levels[0] ? req->mip_levels[0] : 1;
	s->array_size = 1;
	s->base_size.width = sizes[0].width;
	s->base_size.height = sizes[0].height;
	s->base_size.depth = sizes[0].depth;
	s->scanout = req->scanout;
	s->shareable = req->shareable;
	/* No backup: the storage is the host's. */
	s->backup = NULL;
	s->backup_size = 0;
	s->legacy = 1;

	/* DEFINE_SURFACE carries the face table and then one SVGA3dSize per
	 * mip level, in the same order the caller flattened them. */
	uint32_t body = sizeof(SVGA3dCmdDefineSurface) + nsizes * sizeof(SVGA3dSize);
	uint8_t *cmd = kalloc(body);
	if (!cmd) {
		vmw_surface_free_id(v, s->sid);
		kfree(s);
		return -ENOMEM;
	}
	mm_memset(cmd, 0, body);
	SVGA3dCmdDefineSurface *d = (SVGA3dCmdDefineSurface *)cmd;
	d->sid = s->sid;
	d->surfaceFlags = (SVGA3dSurface1Flags)req->flags;
	d->format = req->format;
	for (int f = 0; f < DRM_VMW_MAX_SURFACE_FACES && f < SVGA3D_MAX_SURFACE_FACES; f++)
		d->face[f].numMipLevels = req->mip_levels[f];
	SVGA3dSize *ms = (SVGA3dSize *)(cmd + sizeof(SVGA3dCmdDefineSurface));
	for (uint32_t i = 0; i < nsizes; i++) {
		ms[i].width = sizes[i].width;
		ms[i].height = sizes[i].height;
		ms[i].depth = sizes[i].depth;
	}
	rc = vmw_cmd_one(v, SVGA_3D_CMD_SURFACE_DEFINE, cmd, body);
	kfree(cmd);
	if (rc) {
		vmw_surface_free_id(v, s->sid);
		kfree(s);
		return rc;
	}
	s->defined = 1;

	struct drm_gem_object *so = vmw_surface_object_create(v, s);
	if (!so) {
		vmw_surface_destroy(v, s); /* frees `s' as well */
		return -ENOMEM;
	}
	uint32_t handle = 0;
	rc = drm_gem_handle_create(fp, so, &handle);
	drm_gem_put(so); /* the handle holds it */
	if (rc)
		return rc;
	a->rep.sid = (int32_t)handle;
	a->rep.handle_type = DRM_VMW_HANDLE_LEGACY;
	return 0;
}

long vmw_ioctl_ref_surface(struct vmw_device *v, struct drm_file *fp, void *kb)
{
	union drm_vmw_surface_reference_arg *a = kb;
	struct drm_vmw_surface_arg arg = a->req;
	struct drm_gem_object *o;
	(void)v;

	o = drm_gem_lookup(fp, (uint32_t)arg.sid);
	if (!o)
		return -EINVAL;
	if (o->kind != DRM_GEM_SURFACE || !o->priv) {
		drm_gem_put(o);
		return -EINVAL;
	}
	struct vmw_surface *s = o->priv;
	if (!s->shareable) {
		drm_gem_put(o);
		return -EPERM;
	}

	/* Referencing hands back the parameters the surface was created with,
	 * which is how a second process learns its geometry without having
	 * been told.  size_addr stays zero: the mip-level sizes were the
	 * caller's array at create time and there is nowhere here to put
	 * them, which is what the reference driver reports as well when the
	 * caller passed no buffer for them. */
	mm_memset(&a->rep, 0, sizeof(a->rep));
	a->rep.flags = (uint32_t)s->flags;
	a->rep.format = s->format;
	a->rep.mip_levels[0] = s->mip_levels;
	a->rep.shareable = s->shareable;
	a->rep.scanout = s->scanout;
	a->rep.size_addr = 0;
	/* The lookup's reference is not kept: this ioctl reports parameters,
	 * it does not hand out a new handle.  Keeping it leaked the surface
	 * for good. */
	drm_gem_put(o);
	return 0;
}
