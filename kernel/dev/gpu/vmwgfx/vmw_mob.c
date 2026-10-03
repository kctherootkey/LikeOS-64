// LikeOS -- vmwgfx: MOBs and object tables.
//
// A MOB is how the device sees guest memory: a size and a page table the
// device walks to find the pages.  The table is one, two or three levels
// of 64-bit page numbers -- depth 0 names the single data page directly,
// depth 1 is pages of data-page numbers (512 per page, 2 MB of data each),
// depth 2 is a page of numbers of depth-1 pages (1 GB).  The object tables
// (OTables) the device keeps its surfaces, contexts, shaders, screen
// targets and MOBs in are guest memory described the same way, but given to
// the device with SET_OTABLE_BASE64 instead of a MOB id.
//
// Every page-table page and every data page the device was told about is
// held until the device has worked through everything it was given
// (vmw_defer_free_page / vmw_defer_free_pages), never freed on the spot:
// see vmw_cmdbuf.c for what a page reused under the device turns into.
//
// MOB ids and the MOB page budget: vmw_gmrid.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2012-2023 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/waitq.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* Page-table entries are 64-bit page numbers. */
#define VMW_PPN_SIZE 8
#define VMW_PPN_PER_PAGE (PAGE_SIZE / VMW_PPN_SIZE) /* 512 */
#define VMW_MOBFMT_PTDEPTH_0 SVGA3D_MOBFMT_PT64_0
#define VMW_MOBFMT_PTDEPTH_1 SVGA3D_MOBFMT_PT64_1
#define VMW_MOBFMT_PTDEPTH_2 SVGA3D_MOBFMT_PT64_2
/* The most data pages a depth-2 table reaches. */
#define VMW_MOB_MAX_DATA_PAGES ((uint64_t)VMW_PPN_PER_PAGE * VMW_PPN_PER_PAGE)

/* ---- MOB ids ------------------------------------------------------------ */

/* An id alone, with no pages accounted against the MOB budget.  Kept for
 * callers that size nothing; a buffer object's MOB takes its id and its
 * pages together with vmw_gmrid_man_get_node(v, VMW_PL_MOB, npages, ...)
 * and returns both with vmw_gmrid_man_put_node(). */
int vmw_mob_alloc_id(struct vmw_device *v)
{
	uint32_t id;

	if (vmw_gmrid_man_get_node(v, VMW_PL_MOB, 0, &id) != 0)
		return -1;
	return (int)id;
}

void vmw_mob_free_id(struct vmw_device *v, uint32_t id)
{
	vmw_gmrid_man_put_node(v, VMW_PL_MOB, id, 0);
}

/* ---- page tables ---------------------------------------------------------
 *
 * The page-table pages of one MOB are one array, lowest level first: the
 * depth-1 pages that name the data pages, then the page that names those.
 * The root -- what the device is given -- is the first page of the last
 * level built. */

/* How many page-table pages `data_pages' of data need, over all levels. */
static unsigned long vmw_mob_calculate_pt_pages(unsigned long data_pages)
{
	unsigned long data_size = data_pages * PAGE_SIZE;
	unsigned long tot_size = 0;

	while (likely(data_size > PAGE_SIZE)) {
		data_size = DIV_ROUND_UP(data_size, PAGE_SIZE);
		data_size *= VMW_PPN_SIZE;
		tot_size += ALIGN(data_size, PAGE_SIZE);
	}

	return tot_size / PAGE_SIZE;
}

/* Hand the page-table pages back once the device is done with them.
 *
 * Held, not freed: the device may still be walking them -- a DESTROY or a
 * SET_OTABLE_BASE that stops it is queued like the rest -- so they go back
 * once the device has worked through everything it was given
 * (vmw_defer_free_page). */
static void vmw_mob_pt_release(struct vmw_device *v, struct vmw_mob *mob)
{
	if (mob->pt_pages) {
		for (uint32_t i = 0; i < mob->npt; i++)
			if (mob->pt_pages[i])
				vmw_defer_free_page(v, mob->pt_pages[i]);
		kfree(mob->pt_pages);
	}
	mob->pt_pages = NULL;
	mob->npt = 0;
}

/* Allocate the page-table pages for `num_data_pages' of data.  -ENOMEM
 * leaves nothing allocated. */
static int vmw_mob_pt_populate(struct vmw_mob *mob, unsigned long num_data_pages)
{
	unsigned long n = vmw_mob_calculate_pt_pages(num_data_pages);

	if (WARN_ON_ONCE(mob->pt_pages != NULL))
		return -EINVAL;
	mob->pt_pages = kalloc(n * sizeof(uint64_t));
	if (!mob->pt_pages)
		return -ENOMEM;
	mm_memset(mob->pt_pages, 0, n * sizeof(uint64_t));
	for (unsigned long i = 0; i < n; i++) {
		mob->pt_pages[i] = mm_allocate_physical_page_reclaim();
		if (!mob->pt_pages[i]) {
			/* Never shown to the device: straight back. */
			for (unsigned long j = 0; j < i; j++)
				mm_free_physical_page(mob->pt_pages[j]);
			kfree(mob->pt_pages);
			mob->pt_pages = NULL;
			return -ENOMEM;
		}
		/* Zero first: a fresh physical page holds whatever its last
		 * owner left, and every entry past the end of this MOB would
		 * otherwise be a page number the device might follow. */
		mm_memset(phys_to_virt(mob->pt_pages[i]), 0, PAGE_SIZE);
	}
	mob->npt = (uint32_t)n;
	return 0;
}

/* One level: write the page numbers of `num_data_pages' pages (physical
 * addresses at `data') into consecutive page-table pages from `pt'.
 * Returns the page-table pages used. */
static unsigned long vmw_mob_build_pt(const uint64_t *data,
				      unsigned long num_data_pages,
				      const uint64_t *pt)
{
	unsigned long pt_size = num_data_pages * VMW_PPN_SIZE;
	unsigned long num_pt_pages = DIV_ROUND_UP(pt_size, PAGE_SIZE);
	unsigned long pt_page, i, d = 0;

	for (pt_page = 0; pt_page < num_pt_pages; ++pt_page) {
		uint64_t *addr = phys_to_virt(pt[pt_page]);

		for (i = 0; i < VMW_PPN_PER_PAGE && d < num_data_pages; ++i)
			addr[i] = data[d++] >> VMW_PAGE_SHIFT;
	}

	return num_pt_pages;
}

/* Build every level, bottom up, until one page names everything: the data
 * pages are described by the first level, that level's pages by the next.
 * Sets the depth and the root. */
static void vmw_mob_pt_setup(struct vmw_mob *mob, const uint64_t *data,
			     unsigned long num_data_pages)
{
	const uint64_t *data_iter = data;
	const uint64_t *pt_iter = mob->pt_pages;
	const uint64_t *save_pt_iter = NULL;
	unsigned pt_level = 0;

	BUG_ON(num_data_pages == 0);

	while (likely(num_data_pages > 1)) {
		unsigned long num_pt_pages;

		++pt_level;
		BUG_ON(pt_level > 2);
		save_pt_iter = pt_iter;
		num_pt_pages = vmw_mob_build_pt(data_iter, num_data_pages,
						pt_iter);
		pt_iter += num_pt_pages;
		data_iter = save_pt_iter;
		num_data_pages = num_pt_pages;
	}

	mob->fmt = (SVGAMobFormat)(pt_level + VMW_MOBFMT_PTDEPTH_1 -
				   SVGA3D_MOBFMT_PT_1);
	mob->base_ppn = save_pt_iter[0] >> VMW_PAGE_SHIFT;
}

/* Describe `npages' data pages in `mob': depth 0 for one page, page tables
 * otherwise.  0, -E2BIG past what depth 2 reaches, -ENOMEM. */
static int vmw_mob_pt_make(struct vmw_mob *mob, const uint64_t *pages,
			   uint32_t npages)
{
	int ret;

	mob->pt_pages = NULL;
	mob->npt = 0;
	if (likely(npages == 1)) {
		mob->fmt = VMW_MOBFMT_PTDEPTH_0;
		mob->base_ppn = pages[0] >> VMW_PAGE_SHIFT;
		return 0;
	}
	if (npages > VMW_MOB_MAX_DATA_PAGES)
		return -E2BIG;
	ret = vmw_mob_pt_populate(mob, npages);
	if (ret)
		return ret;
	vmw_mob_pt_setup(mob, pages, npages);
	return 0;
}

/* ---- MOBs ---------------------------------------------------------------- */

/* Build the page tables for `pages' and send DEFINE_GB_MOB64 under the id
 * the caller put in mob->id (from vmw_gmrid_man_get_node()).  Every other
 * field of `mob' is set here.  On failure nothing is left behind: no page
 * tables, nothing defined, and the id is still the caller's to give back. */
int vmw_mob_bind(struct vmw_device *v, struct vmw_mob *mob, const uint64_t *pages,
		 uint32_t npages, uint32_t size_bytes)
{
	int ret;

	mob->size = size_bytes;
	mob->defined = 0;
	mob->pt_pages = NULL;
	mob->npt = 0;
	if (unlikely(!pages || npages == 0))
		return -EINVAL;
	if (unlikely(mob->id == SVGA3D_INVALID_ID))
		return -EINVAL;

	ret = vmw_mob_pt_make(mob, pages, npages);
	if (ret)
		return ret;

	SVGA3dCmdDefineGBMob64 c;
	c.mobid = mob->id;
	c.ptDepth = mob->fmt;
	c.base = mob->base_ppn;
	c.sizeInBytes = size_bytes;
	ret = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_MOB64, &c, sizeof(c));
	if (ret == 0) {
		mob->defined = 1;
		return 0;
	}
	/* Whether the command got anywhere is not known here, so the
	 * page tables are released the way a destroyed MOB's are. */
	vmw_mob_pt_release(v, mob);
	return ret;
}

void vmw_mob_unbind(struct vmw_device *v, struct vmw_mob *mob)
{
	if (mob->defined) {
		SVGA3dCmdDestroyGBMob c;
		c.mobid = mob->id;
		/* Queued like the rest.  What made this one wait was that
		 * its page tables are freed below and the device may still
		 * be walking them -- so the pages are held instead of freed,
		 * and handed back once the device has worked through
		 * everything it was given (vmw_defer_free_page). */
		vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_MOB, &c, sizeof(c));
		mob->defined = 0;
	}
	vmw_mob_pt_release(v, mob);
}

/* ---- a device that lost its MOBs ------------------------------------------
 *
 * A reset (a suspend that resets the device, or a device that comes back
 * from one without its state) empties the device's MOB table while every
 * buffer object here still holds its id, its pages and its page tables.
 * Nothing the guest kept has moved, so the same DEFINE_GB_MOB64 brings the
 * MOB back exactly as it was: same id, same page-table root, same size --
 * and every command naming the id means what it meant before. */

/* Tell the device about `mob' again from what was recorded when it was
 * bound.  Queued like any other command; it reaches the device after the
 * object tables the caller set up first.  0 (also for a MOB that was never
 * defined), or the submission's error -- the MOB is then marked undefined,
 * so that its destroy is not sent for an id the device does not have. */
int vmw_mob_redefine(struct vmw_device *v, struct vmw_mob *mob)
{
	SVGA3dCmdDefineGBMob64 c;
	int ret;

	if (mob->id == SVGA3D_INVALID_ID || !mob->defined)
		return 0;
	c.mobid = mob->id;
	c.ptDepth = mob->fmt;
	c.base = mob->base_ppn;
	c.sizeInBytes = mob->size;
	ret = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_MOB64, &c, sizeof(c));
	if (ret)
		mob->defined = 0;
	return ret;
}

/* The device no longer has `mob' and is not to be told about it again (its
 * object is on its way out): its destroy is skipped, and its page tables
 * and id are released as usual when the object goes.  Sends nothing. */
void vmw_mob_forget(struct vmw_mob *mob)
{
	mob->defined = 0;
}

/* ---- OTables ------------------------------------------------------------ */

/* One object table: its size (page-aligned once set up) and the page
 * tables that describe its part of the batch buffer to the device. */
struct vmw_otable {
	unsigned long size;
	struct vmw_mob *page_table;
	bool enabled;
};

/* All object tables live in one buffer, one after the other. */
struct vmw_otable_batch {
	unsigned num_otables;
	struct vmw_otable otables[SVGA_OTABLE_DX_MAX];
	struct drm_gem_object *otable_bo;
};

static const struct vmw_otable pre_dx_tables[] = {
	{VMW_NUM_MOBS * sizeof(SVGAOTableMobEntry), NULL, true},
	{VMW_NUM_SURFACES * sizeof(SVGAOTableSurfaceEntry), NULL, true},
	{VMW_NUM_CONTEXTS * sizeof(SVGAOTableContextEntry), NULL, true},
	{VMW_NUM_SHADERS * sizeof(SVGAOTableShaderEntry), NULL, true},
	{VMW_NUM_SCREENTARGETS * sizeof(SVGAOTableScreenTargetEntry),
	 NULL, true}
};

static const struct vmw_otable dx_tables[] = {
	{VMW_NUM_MOBS * sizeof(SVGAOTableMobEntry), NULL, true},
	{VMW_NUM_SURFACES * sizeof(SVGAOTableSurfaceEntry), NULL, true},
	{VMW_NUM_CONTEXTS * sizeof(SVGAOTableContextEntry), NULL, true},
	{VMW_NUM_SHADERS * sizeof(SVGAOTableShaderEntry), NULL, true},
	{VMW_NUM_SCREENTARGETS * sizeof(SVGAOTableScreenTargetEntry),
	 NULL, true},
	{VMW_NUM_DXCONTEXTS * sizeof(SVGAOTableDXContextEntry), NULL, true},
};

/* The device takes object tables at depth 0 or 1 only; the sizes are fixed
 * here, so this is settled at build time: no table may need a second
 * level. */
#define VMW_OTABLE_MAX_BYTES ((unsigned long)VMW_PPN_PER_PAGE * PAGE_SIZE)
_Static_assert(VMW_NUM_MOBS * sizeof(SVGAOTableMobEntry) <= VMW_OTABLE_MAX_BYTES,
	       "MOB OTable needs a depth-2 page table");
_Static_assert(VMW_NUM_SURFACES * sizeof(SVGAOTableSurfaceEntry) <=
		       VMW_OTABLE_MAX_BYTES,
	       "surface OTable needs a depth-2 page table");
_Static_assert(VMW_NUM_CONTEXTS * sizeof(SVGAOTableContextEntry) <=
		       VMW_OTABLE_MAX_BYTES,
	       "context OTable needs a depth-2 page table");
_Static_assert(VMW_NUM_SHADERS * sizeof(SVGAOTableShaderEntry) <=
		       VMW_OTABLE_MAX_BYTES,
	       "shader OTable needs a depth-2 page table");
_Static_assert(VMW_NUM_SCREENTARGETS * sizeof(SVGAOTableScreenTargetEntry) <=
		       VMW_OTABLE_MAX_BYTES,
	       "screen-target OTable needs a depth-2 page table");
_Static_assert(VMW_NUM_DXCONTEXTS * sizeof(SVGAOTableDXContextEntry) <=
		       VMW_OTABLE_MAX_BYTES,
	       "DX context OTable needs a depth-2 page table");
_Static_assert(sizeof(dx_tables) / sizeof(dx_tables[0]) == SVGA_OTABLE_DX_MAX,
	       "one entry per OTable type");
_Static_assert(sizeof(pre_dx_tables) / sizeof(pre_dx_tables[0]) ==
		       SVGA_OTABLE_DX9_MAX,
	       "one entry per pre-DX OTable type");

static void vmw_mob_destroy(struct vmw_device *v, struct vmw_mob *mob)
{
	vmw_mob_pt_release(v, mob);
	kfree(mob);
}

/* Point the device at one table: its pages start `offset' bytes into the
 * batch buffer.
 *
 * Waited for: a table the device refused leaves every object of that type
 * undefinable, and finding that out at setup is what lets the driver turn
 * 3D off instead of failing later, one command at a time. */
static int vmw_setup_otable_base(struct vmw_device *v, SVGAOTableType type,
				 struct drm_gem_object *otable_bo,
				 unsigned long offset,
				 struct vmw_otable *otable)
{
	const uint64_t *pages = otable_bo->pages + offset / PAGE_SIZE;
	uint32_t npages = (uint32_t)(otable->size / PAGE_SIZE);
	struct vmw_mob *mob;
	int ret;

	BUG_ON(otable->page_table != NULL);
	if (WARN_ON(offset / PAGE_SIZE + npages > otable_bo->npages))
		return -EINVAL;

	mob = kalloc(sizeof(*mob));
	if (unlikely(mob == NULL)) {
		kprintf("[drm] vmwgfx: failed creating OTable page table\n");
		return -ENOMEM;
	}
	mm_memset(mob, 0, sizeof(*mob));
	mob->id = SVGA3D_INVALID_ID; /* not a MOB: no id, no DEFINE */

	ret = vmw_mob_pt_make(mob, pages, npages);
	if (unlikely(ret != 0))
		goto out_no_populate;
	/* Settled at build time (above); a depth-2 table would be refused. */
	if (WARN_ON(mob->fmt == VMW_MOBFMT_PTDEPTH_2)) {
		ret = -EINVAL;
		goto out_no_fifo;
	}

	SVGA3dCmdSetOTableBase64 c;
	mm_memset(&c, 0, sizeof(c));
	c.type = type;
	c.baseAddress = mob->base_ppn;
	c.sizeInBytes = (uint32_t)otable->size;
	c.validSizeInBytes = 0;
	c.ptDepth = mob->fmt;
	ret = vmw_cmd_one_sync(v, SVGA_3D_CMD_SET_OTABLE_BASE64, &c, sizeof(c));
	if (unlikely(ret != 0))
		goto out_no_fifo;

	otable->page_table = mob;
	return 0;

out_no_fifo:
out_no_populate:
	vmw_mob_destroy(v, mob);
	return ret;
}

/* Tell the device to stop using one table, then release its page tables
 * (held until the device is past the command). */
static void vmw_takedown_otable_base(struct vmw_device *v, SVGAOTableType type,
				     struct vmw_otable *otable)
{
	if (otable->page_table == NULL)
		return;

	SVGA3dCmdSetOTableBase c;
	mm_memset(&c, 0, sizeof(c));
	c.type = type;
	c.baseAddress = 0;
	c.sizeInBytes = 0;
	c.validSizeInBytes = 0;
	c.ptDepth = SVGA3D_MOBFMT_INVALID;
	vmw_cmd_one(v, SVGA_3D_CMD_SET_OTABLE_BASE, &c, sizeof(c));

	vmw_mob_destroy(v, otable->page_table);
	otable->page_table = NULL;
}

static int vmw_otable_batch_setup(struct vmw_device *v,
				  struct vmw_otable_batch *batch)
{
	unsigned long offset;
	unsigned long bo_size;
	struct vmw_otable *otables = batch->otables;
	unsigned i;
	int ret;

	bo_size = 0;
	for (i = 0; i < batch->num_otables; ++i) {
		if (!otables[i].enabled)
			continue;

		otables[i].size = ALIGN(otables[i].size, PAGE_SIZE);
		bo_size += otables[i].size;
	}

	/* Zeroed pages (drm_gem_alloc_pages): the device reads the tables
	 * as they are given.  No gem_init: the buffer is not a MOB itself,
	 * and releasing it holds its pages until the device is done (the
	 * backend's gem_release_pages). */
	batch->otable_bo = drm_gem_alloc(&v->drm, DRM_GEM_BO, bo_size);
	if (!batch->otable_bo)
		return -ENOMEM;
	if (drm_gem_alloc_pages(batch->otable_bo)) {
		drm_gem_put(batch->otable_bo);
		batch->otable_bo = NULL;
		return -ENOMEM;
	}

	offset = 0;
	for (i = 0; i < batch->num_otables; ++i) {
		if (!batch->otables[i].enabled)
			continue;

		ret = vmw_setup_otable_base(v, (SVGAOTableType)i,
					    batch->otable_bo, offset,
					    &otables[i]);
		if (unlikely(ret != 0)) {
			kprintf("[drm] vmwgfx: OTable %u setup failed (%d)\n",
				i, ret);
			goto out_no_setup;
		}
		v->otable[i].size = (uint32_t)otables[i].size;
		offset += otables[i].size;
	}

	return 0;

out_no_setup:
	for (i = 0; i < batch->num_otables; ++i) {
		if (batch->otables[i].enabled)
			vmw_takedown_otable_base(v, (SVGAOTableType)i,
						 &batch->otables[i]);
		v->otable[i].size = 0;
	}

	drm_gem_put(batch->otable_bo);
	batch->otable_bo = NULL;
	return ret;
}

/* Hand the device its object tables: every table in one buffer, each
 * described by its own page tables.  0, or -errno with nothing left set up;
 * on success vmw_otables_takedown() undoes it. */
int vmw_otables_setup(struct vmw_device *v)
{
	struct vmw_otable_batch *batch;
	int ret;

	if (v->otable_batch)
		return 0;
	batch = kalloc(sizeof(*batch));
	if (!batch)
		return -ENOMEM;
	mm_memset(batch, 0, sizeof(*batch));

	if (has_sm4_context(v)) {
		mm_memcpy(batch->otables, dx_tables, sizeof(dx_tables));
		batch->num_otables = sizeof(dx_tables) / sizeof(dx_tables[0]);
	} else {
		mm_memcpy(batch->otables, pre_dx_tables, sizeof(pre_dx_tables));
		batch->num_otables =
			sizeof(pre_dx_tables) / sizeof(pre_dx_tables[0]);
	}

	ret = vmw_otable_batch_setup(v, batch);
	if (unlikely(ret != 0)) {
		kfree(batch);
		return ret;
	}

	v->otable_batch = batch;
	v->otables_ready = 1;
	return 0;
}

static void vmw_otable_batch_takedown(struct vmw_device *v,
				      struct vmw_otable_batch *batch)
{
	unsigned i;

	for (i = 0; i < batch->num_otables; ++i) {
		if (batch->otables[i].enabled)
			vmw_takedown_otable_base(v, (SVGAOTableType)i,
						 &batch->otables[i]);
		v->otable[i].size = 0;
	}

	/* The table storage itself: its pages are held by the backend's
	 * gem_release_pages until the device is past the commands above. */
	if (batch->otable_bo) {
		drm_gem_put(batch->otable_bo);
		batch->otable_bo = NULL;
	}
}

/* Take the object tables away from the device and release them: the page
 * tables and the table storage both.  The count of tables is the one they
 * were set up with, whatever has_dx says by now. */
void vmw_otables_takedown(struct vmw_device *v)
{
	struct vmw_otable_batch *batch = v->otable_batch;

	v->otables_ready = 0;
	if (!batch)
		return;
	v->otable_batch = NULL;
	vmw_otable_batch_takedown(v, batch);
	kfree(batch);
}

/* ---- bring-up ------------------------------------------------------------- */

int vmw_gb_init(struct vmw_device *v)
{
	spinlock_init(&v->id_lock, "vmw_ids");
	spinlock_init(&v->defer_lock, "vmw_defer");
	/* MOB ids are the MOB manager's (vmw_gmrid.c); v->mob_ids stays
	 * unallocated. */
	v->surface_ids = kalloc(VMW_NUM_SURFACES / 8);
	v->context_ids = kalloc(VMW_NUM_CONTEXTS / 8);
	v->shader_ids = kalloc(VMW_NUM_SHADERS / 8);
	if (!v->surface_ids || !v->context_ids || !v->shader_ids)
		return -ENOMEM;
	mm_memset(v->surface_ids, 0, VMW_NUM_SURFACES / 8);
	mm_memset(v->context_ids, 0, VMW_NUM_CONTEXTS / 8);
	mm_memset(v->shader_ids, 0, VMW_NUM_SHADERS / 8);
	/* id 0 of each is left unused: userspace treats 0 as a valid id but
	 * a few consumers assume "0 = none". */
	v->surface_ids[0] |= 1;
	v->context_ids[0] |= 1;
	v->shader_ids[0] |= 1;
	/* Region accounting wherever the device has regions; without it the
	 * regions still work, unaccounted, so a failure is only reported. */
	if (v->has_gmr) {
		int r = vmw_gmrid_man_init(v, VMW_PL_GMR);

		if (r && r != -ENODEV)
			kprintf("[drm] vmwgfx: no GMR accounting (%d)\n", r);
	}
	int rc = vmw_cmdbuf_bringup(v);
	if (rc)
		return rc;
	if (!v->has_gb)
		return 0;
	/* No MOB accounting means no MOBs: every buffer object of a
	 * guest-backed device is one, so 3D goes with it. */
	rc = vmw_gmrid_man_init(v, VMW_PL_MOB);
	if (rc) {
		kprintf("[drm] vmwgfx: no MOB memory available (%d)\n", rc);
		return rc;
	}
	rc = vmw_otables_setup(v);
	if (rc)
		vmw_gmrid_man_fini(v, VMW_PL_MOB);
	return rc;
}

/* Undo vmw_gb_init()'s guest-backed half: the object tables, then the MOB
 * accounting.  Every buffer object's MOB must be gone first. */
void vmw_gb_takedown(struct vmw_device *v)
{
	vmw_otables_takedown(v);
	vmw_gmrid_man_fini(v, VMW_PL_MOB);
}
