// LikeOS-64 -- vmwgfx: dirty regions of a coherent surface.
//
// The page tracker (drm_dirty.c) answers in pages of the backing buffer;
// the device wants boxes of texels in a subresource.  This file is the
// translation, ported from the reference driver: a byte range of the
// backing store is located within the surface's layout -- sheet, layer,
// mip level, block coordinates -- and folded into one box per
// subresource, the union of everything that touched it.  At submission
// the boxes become update commands, one per dirtied subresource, sent
// ahead of the client's batch in the same channel so the device reads
// the pages before any command that consumes them.
//
// The location arithmetic assumes what the layout guarantees: a byte
// range of the backing store is a sequential walk of blocks, rows,
// slices, mip levels and layers, in that nesting order.  A range that
// spans whole rows therefore dirties the rows in full whatever its x
// extents, one that spans slices dirties them in full, and one that
// spans subresources dirties the interior ones in full -- exactly the
// reference driver's reading, kept bit for bit because a box drawn too
// small is the corruption this machinery once caused: texels the client
// wrote and the device never re-read.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_surfacedefs.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga3d_limits.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- surface layout ----------------------------------------------------- */

struct vmw_surf_mip {
	size_t bytes; /* backing bytes of one image at this level */
	size_t img_stride; /* bytes per slice */
	size_t row_stride; /* bytes per block row */
	SVGA3dSize size; /* texel dimensions at this level */
};

struct vmw_surf_layout {
	const SVGA3dSurfaceDesc *desc;
	struct vmw_surf_mip mip[DRM_VMW_MAX_MIP_LEVELS];
	size_t mip_chain_bytes; /* one layer, all levels */
	size_t sheet_bytes; /* one sample: all layers */
	uint32_t num_mip_levels;
	uint32_t num_layers;
};

/* A position within the surface: the multisample sheet, the subresource
 * (layer * levels + level), and block-aligned texel coordinates. */
struct vmw_surf_loc {
	uint32_t sheet;
	uint32_t sub_resource;
	uint32_t x, y, z;
};

static const SVGA3dSurfaceDesc *surf_desc(uint32_t format)
{
	if (format < SVGA3D_FORMAT_MAX)
		return &g_SVGA3dSurfaceDescs[format];
	return &g_SVGA3dSurfaceDescs[SVGA3D_FORMAT_INVALID];
}

static SVGA3dSize surf_mip_size(SVGA3dSize base, uint32_t level)
{
	SVGA3dSize s;

	s.width = base.width >> level ? base.width >> level : 1;
	s.height = base.height >> level ? base.height >> level : 1;
	s.depth = base.depth >> level ? base.depth >> level : 1;
	return s;
}

static void surf_size_in_blocks(const SVGA3dSurfaceDesc *desc,
				const SVGA3dSize *px, SVGA3dSize *bl)
{
	bl->width = (px->width + desc->blockSize.width - 1) /
		    desc->blockSize.width;
	bl->height = (px->height + desc->blockSize.height - 1) /
		     desc->blockSize.height;
	bl->depth = (px->depth + desc->blockSize.depth - 1) /
		    desc->blockSize.depth;
}

static int surf_is_planar(const SVGA3dSurfaceDesc *desc)
{
	return (desc->blockDesc & SVGA3DBLOCKDESC_PLANAR_YUV) != 0;
}

/* Backing bytes of one image, rows tightly packed. */
static size_t surf_image_bytes(const SVGA3dSurfaceDesc *desc,
			       const SVGA3dSize *size)
{
	SVGA3dSize bl;

	surf_size_in_blocks(desc, size, &bl);
	if (surf_is_planar(desc))
		return (size_t)bl.width * bl.height * bl.depth *
		       desc->bytesPerBlock;
	return (size_t)bl.height * bl.depth *
	       ((size_t)bl.width * desc->bytesPerBlock);
}

/* Build the layout, refusing shapes whose strides come out empty; the
 * caller then refuses coherence for the surface rather than tracking it
 * with arithmetic that divides by zero. */
static int surf_layout_setup(const struct vmw_surface *s, uint32_t num_layers,
			     struct vmw_surf_layout *l)
{
	uint32_t samples = s->multisample_count ? s->multisample_count : 1;

	mm_memset(l, 0, sizeof(*l));
	l->desc = surf_desc(s->format);
	l->num_mip_levels = s->mip_levels ? s->mip_levels : 1;
	l->num_layers = num_layers;
	if (l->num_mip_levels > DRM_VMW_MAX_MIP_LEVELS)
		return -EINVAL;
	for (uint32_t i = 0; i < l->num_mip_levels; i++) {
		struct vmw_surf_mip *mip = &l->mip[i];

		mip->size = surf_mip_size(s->base_size, i);
		mip->bytes = surf_image_bytes(l->desc, &mip->size);
		mip->row_stride = (size_t)((mip->size.width +
					    l->desc->blockSize.width - 1) /
					   l->desc->blockSize.width) *
				  l->desc->bytesPerBlock * samples;
		if (!mip->row_stride)
			return -EINVAL;
		mip->img_stride = (size_t)((mip->size.height +
					    l->desc->blockSize.height - 1) /
					   l->desc->blockSize.height) *
				  mip->row_stride;
		if (!mip->img_stride)
			return -EINVAL;
		l->mip_chain_bytes += mip->bytes;
	}
	l->sheet_bytes = l->mip_chain_bytes * num_layers;
	if (!l->sheet_bytes)
		return -EINVAL;
	return 0;
}

static uint32_t surf_subres(const struct vmw_surf_layout *l, uint32_t level,
			    uint32_t layer)
{
	return l->num_mip_levels * layer + level;
}

/* Locate a backing-store offset: which sheet, which subresource, and the
 * block-aligned texel coordinates within it. */
static void surf_get_loc(const struct vmw_surf_layout *l,
			 struct vmw_surf_loc *loc, size_t offset)
{
	const struct vmw_surf_mip *mip = &l->mip[0];
	const SVGA3dSurfaceDesc *desc = l->desc;
	uint32_t layer;
	uint32_t i;

	loc->sheet = offset / l->sheet_bytes;
	offset -= (size_t)loc->sheet * l->sheet_bytes;

	layer = offset / l->mip_chain_bytes;
	offset -= (size_t)layer * l->mip_chain_bytes;
	for (i = 0; i < l->num_mip_levels - 1; ++i, ++mip) {
		if (mip->bytes > offset)
			break;
		offset -= mip->bytes;
	}

	loc->sub_resource = surf_subres(l, i, layer);
	loc->z = offset / mip->img_stride;
	offset -= (size_t)loc->z * mip->img_stride;
	loc->z *= desc->blockSize.depth;
	loc->y = offset / mip->row_stride;
	offset -= (size_t)loc->y * mip->row_stride;
	loc->y *= desc->blockSize.height;
	loc->x = offset / desc->bytesPerBlock;
	loc->x *= desc->blockSize.width;
}

/* Turn the location of a range's LAST byte into the exclusive end a box
 * wants: one block further in each dimension, clamped to the level. */
static void surf_inc_loc(const struct vmw_surf_layout *l,
			 struct vmw_surf_loc *loc)
{
	const SVGA3dSurfaceDesc *desc = l->desc;
	uint32_t level = loc->sub_resource % l->num_mip_levels;
	const SVGA3dSize *size = &l->mip[level].size;

	loc->sub_resource++;
	loc->x += desc->blockSize.width;
	if (loc->x > size->width)
		loc->x = size->width;
	loc->y += desc->blockSize.height;
	if (loc->y > size->height)
		loc->y = size->height;
	loc->z += desc->blockSize.depth;
	if (loc->z > size->depth)
		loc->z = size->depth;
}

static void surf_min_loc(const struct vmw_surf_layout *l, uint32_t sub_res,
			 struct vmw_surf_loc *loc)
{
	(void)l; /* symmetry with surf_max_loc */
	loc->sheet = 0;
	loc->sub_resource = sub_res;
	loc->x = loc->y = loc->z = 0;
}

static void surf_max_loc(const struct vmw_surf_layout *l, uint32_t sub_res,
			 struct vmw_surf_loc *loc)
{
	const SVGA3dSize *size;
	uint32_t level;

	loc->sheet = 0;
	loc->sub_resource = sub_res + 1;
	level = sub_res % l->num_mip_levels;
	size = &l->mip[level].size;
	loc->x = size->width;
	loc->y = size->height;
	loc->z = size->depth;
}

/* ---- the per-surface tracker ------------------------------------------- */

struct vmw_surface_dirty {
	struct vmw_surf_layout layout;
	/* Two submitting threads can reference one surface: the boxes are
	 * written by one submission's pull while another's emit reads and
	 * clears them, so every access below is under this.  Held only for
	 * box arithmetic -- never across an allocation or a sweep. */
	spinlock_t lock;
	uint32_t num_subres;
	SVGA3dBox boxes[]; /* box.d == 0 means the subresource is clean */
};

/* Fold [loc_start, loc_end) into the subresource's box.  A range crossing
 * rows dirties the rows in full, one crossing slices dirties them in
 * full: the range is sequential backing bytes, so whatever the x of its
 * endpoints, everything between the crossed boundaries was inside it. */
static void surf_subres_dirty_add(struct vmw_surface_dirty *dirty,
				  const struct vmw_surf_loc *loc_start,
				  const struct vmw_surf_loc *loc_end)
{
	const struct vmw_surf_layout *l = &dirty->layout;
	SVGA3dBox *box;
	uint32_t level = loc_start->sub_resource % l->num_mip_levels;
	const SVGA3dSize *size = &l->mip[level].size;
	uint32_t box_c2;

	if (loc_start->sub_resource >= dirty->num_subres)
		return;
	box = &dirty->boxes[loc_start->sub_resource];
	box_c2 = box->z + box->d;
	if (box->d == 0 || box->z > loc_start->z)
		box->z = loc_start->z;
	if (box_c2 < loc_end->z)
		box->d = loc_end->z - box->z;

	if (loc_start->z + 1 == loc_end->z) {
		box_c2 = box->y + box->h;
		if (box->h == 0 || box->y > loc_start->y)
			box->y = loc_start->y;
		if (box_c2 < loc_end->y)
			box->h = loc_end->y - box->y;

		if (loc_start->y + 1 == loc_end->y) {
			box_c2 = box->x + box->w;
			if (box->w == 0 || box->x > loc_start->x)
				box->x = loc_start->x;
			if (box_c2 < loc_end->x)
				box->w = loc_end->x - box->x;
		} else {
			box->x = 0;
			box->w = size->width;
		}
	} else {
		box->y = 0;
		box->h = size->height;
		box->x = 0;
		box->w = size->width;
	}
}

static void surf_subres_dirty_full(struct vmw_surface_dirty *dirty,
				   uint32_t sub_res)
{
	const struct vmw_surf_layout *l = &dirty->layout;

	if (sub_res >= dirty->num_subres)
		return; /* backing tail past the layout; nothing to mark */
	uint32_t level = sub_res % l->num_mip_levels;
	const SVGA3dSize *size = &l->mip[level].size;
	SVGA3dBox *box = &dirty->boxes[sub_res];

	box->x = 0;
	box->y = 0;
	box->z = 0;
	box->w = size->width;
	box->h = size->height;
	box->d = size->depth;
}

/* A byte range of a texture's backing store. */
static void surf_tex_dirty_range_add(struct vmw_surface_dirty *dirty,
				     size_t start, size_t end)
{
	const struct vmw_surf_layout *l = &dirty->layout;
	struct vmw_surf_loc loc1, loc2;

	surf_get_loc(l, &loc1, start);
	surf_get_loc(l, &loc2, end - 1);
	surf_inc_loc(l, &loc2);

	if (loc1.sheet != loc2.sheet) {
		/* Several multisample sheets.  Working out the union per
		 * sheet is possible and the case is not worth it: dirty
		 * everything. */
		for (uint32_t i = 0; i < dirty->num_subres; ++i)
			surf_subres_dirty_full(dirty, i);
		return;
	}
	if (loc1.sub_resource + 1 == loc2.sub_resource) {
		/* One subresource. */
		surf_subres_dirty_add(dirty, &loc1, &loc2);
	} else {
		/* Partial first and last, everything between in full. */
		struct vmw_surf_loc loc_min, loc_max;

		surf_max_loc(l, loc1.sub_resource, &loc_max);
		surf_subres_dirty_add(dirty, &loc1, &loc_max);
		surf_min_loc(l, loc2.sub_resource - 1, &loc_min);
		surf_subres_dirty_add(dirty, &loc_min, &loc2);
		for (uint32_t i = loc1.sub_resource + 1;
		     i + 1 < loc2.sub_resource; ++i)
			surf_subres_dirty_full(dirty, i);
	}
}

/* A byte range of a buffer surface: bytes are texels on one line. */
static void surf_buf_dirty_range_add(struct vmw_surface_dirty *dirty,
				     size_t start, size_t end)
{
	const struct vmw_surf_layout *l = &dirty->layout;
	size_t backup_end = l->mip_chain_bytes;
	SVGA3dBox *box = &dirty->boxes[0];
	uint32_t box_c2;

	if (start >= backup_end)
		return;
	if (end > backup_end)
		end = backup_end;
	box->h = box->d = 1;
	box_c2 = box->x + box->w;
	if (box->w == 0 || box->x > start)
		box->x = start;
	if (box_c2 < end)
		box->w = end - box->x;
}

/* ---- driver interface --------------------------------------------------- */

void vmw_surface_dirty_range_add(struct vmw_surface *s, size_t start,
				 size_t end)
{
	struct vmw_surface_dirty *dirty = s->dirty;
	uint64_t fl;

	if (!dirty || end <= start)
		return;
	if (start >= s->backup_size)
		return;
	if (end > s->backup_size)
		end = s->backup_size;
	spin_lock_irqsave(&dirty->lock, &fl);
	if (s->format == SVGA3D_BUFFER)
		surf_buf_dirty_range_add(dirty, start, end);
	else
		surf_tex_dirty_range_add(dirty, start, end);
	spin_unlock_irqrestore(&dirty->lock, fl);
}

int vmw_surface_dirty_alloc(struct vmw_surface *s)
{
	struct vmw_surface_dirty *dirty;
	uint32_t num_layers = 1;
	uint32_t num_subres;
	size_t size;
	int rc;

	if (s->array_size)
		num_layers = s->array_size;
	else if (s->flags & SVGA3D_SURFACE_CUBEMAP)
		num_layers = SVGA3D_MAX_SURFACE_FACES;

	uint32_t num_mip = s->mip_levels ? s->mip_levels : 1;

	num_subres = num_layers * num_mip;
	size = sizeof(*dirty) + (size_t)num_subres * sizeof(SVGA3dBox);
	dirty = kalloc(size);
	if (!dirty)
		return -ENOMEM;
	mm_memset(dirty, 0, size);
	rc = surf_layout_setup(s, num_layers, &dirty->layout);
	if (rc) {
		kfree(dirty);
		return rc;
	}
	dirty->num_subres = num_subres;
	spinlock_init(&dirty->lock, "vmw_surf_dirty");
	s->dirty = dirty;
	return 0;
}

void vmw_surface_dirty_free(struct vmw_surface *s)
{
	if (s->dirty) {
		kfree(s->dirty);
		s->dirty = NULL;
	}
}

/* How many update commands the dirty boxes will become. */
uint32_t vmw_surface_dirty_count(const struct vmw_surface *s)
{
	struct vmw_surface_dirty *dirty = s->dirty;
	uint32_t n = 0;
	uint64_t fl;

	if (!dirty)
		return 0;
	spin_lock_irqsave(&dirty->lock, &fl);
	for (uint32_t i = 0; i < dirty->num_subres; i++)
		if (dirty->boxes[i].d)
			n++;
	spin_unlock_irqrestore(&dirty->lock, fl);
	return n;
}

/* Write the update commands into `out' -- at most `cap' bytes -- and
 * clear each box AS IT IS WRITTEN; returns the bytes written.  A box that
 * appears after the caller sized the buffer (another thread's submission
 * pulling into the same surface) simply stays, and the next submission
 * says it.  The caller places the commands AHEAD of the batch that reads
 * the surface, in the same channel, which is all the ordering the device
 * needs.
 *
 * DX_UPDATE_SUBRESOURCE addresses array surfaces; UPDATE_GB_IMAGE only
 * knows face and level.  The device that offers DX contexts takes the
 * former, exactly as the reference driver chooses. */
uint32_t vmw_surface_dirty_emit(struct vmw_device *v, struct vmw_surface *s,
				void *out, uint32_t cap)
{
	struct vmw_surface_dirty *dirty = s->dirty;
	uint8_t *p = out;
	uint64_t fl;

	if (!dirty)
		return 0;
	spin_lock_irqsave(&dirty->lock, &fl);
	for (uint32_t i = 0; i < dirty->num_subres; i++) {
		SVGA3dBox *box = &dirty->boxes[i];

		if (!box->d)
			continue;
		if ((uint32_t)(p - (uint8_t *)out) + VMW_SURF_DIRTY_CMD_MAX >
		    cap)
			break; /* the rest keeps its dirt for next time */
		SVGA3dCmdHeader *h = (SVGA3dCmdHeader *)p;

		if (v->has_dx) {
			SVGA3dCmdDXUpdateSubResource *b =
				(SVGA3dCmdDXUpdateSubResource *)(h + 1);

			h->id = SVGA_3D_CMD_DX_UPDATE_SUBRESOURCE;
			h->size = sizeof(*b);
			b->sid = s->sid;
			b->subResource = i;
			b->box = *box;
			p += sizeof(*h) + sizeof(*b);
		} else {
			SVGA3dCmdUpdateGBImage *b =
				(SVGA3dCmdUpdateGBImage *)(h + 1);

			h->id = SVGA_3D_CMD_UPDATE_GB_IMAGE;
			h->size = sizeof(*b);
			b->image.sid = s->sid;
			b->image.face = i / dirty->layout.num_mip_levels;
			b->image.mipmap =
				i - (dirty->layout.num_mip_levels *
				     b->image.face);
			b->box = *box;
			p += sizeof(*h) + sizeof(*b);
		}
		mm_memset(box, 0, sizeof(*box));
	}
	spin_unlock_irqrestore(&dirty->lock, fl);
	return (uint32_t)(p - (uint8_t *)out);
}

/* The page tracker hands over page runs; bytes are what the boxes want. */
static void surf_pull_cb(void *arg, uint64_t first, uint64_t last)
{
	struct vmw_surface *s = arg;

	vmw_surface_dirty_range_add(s, (size_t)first * PAGE_SIZE,
				    (size_t)last * PAGE_SIZE);
}

void vmw_surface_dirty_pull(struct vmw_surface *s)
{
	if (s->dirty && s->backup)
		drm_gem_dirty_transfer(s->backup, surf_pull_cb, s);
}
