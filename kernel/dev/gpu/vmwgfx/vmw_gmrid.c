// LikeOS -- vmwgfx: id and memory accounting for guest memory regions and
// MOBs.
//
// See vmw_gmrid.h for the contract.  One manager per id space: a page
// budget taken from the device's limits, and for MOBs the id table itself.
// A MOB id is the lowest free one, found from a hint that is never above
// it, so an allocation scans from where the free ids start instead of
// from 0 over every id in use.  Region ids are not allocated here: the
// hardware layer keeps that table, and this only puts the budget in front
// of it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2007-2010 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

struct vmw_gmrid_man {
	/* Covers everything below; never held across a call out of here. */
	spinlock_t lock;
	/* VMW_PL_MOB only: one bit per id, set = taken.  NULL for regions. */
	unsigned long *ids;
	uint32_t max_ids;
	/* No id below this is free.  Lowered by a free, raised past each
	 * allocation; the lowest free id is the first clear bit at or after
	 * it. */
	uint32_t id_hint;
	uint32_t used_ids;
	/* The page budget; 0 = the device states none, nothing is counted. */
	uint32_t max_pages;
	uint32_t used_pages;
	/* How far the budget may grow: half of RAM, in pages. */
	uint32_t max_graphics_pages;
	uint8_t type;
};

/* Console lines about the budget: a client that keeps allocating past it
 * keeps hitting the same failure, and each one must not print. */
#define VMW_GMRID_WARN_MAX 10
static uint32_t vmw_gmrid_warned;

static const char *vmw_gmrid_name(uint8_t type)
{
	return type == VMW_PL_MOB ? "MOB" : "GMR";
}

/* ---- ids ---------------------------------------------------------------- */

/* The lowest clear bit at or after `from', or `size' if none.  Whole words
 * at a time: a full word is skipped with one compare. */
static uint32_t vmw_gmrid_find_free(const unsigned long *bm, uint32_t size,
				    uint32_t from)
{
	while (from < size) {
		unsigned long word = ~bm[from / BITS_PER_LONG] >>
				     (from % BITS_PER_LONG);

		if (word) {
			from += (uint32_t)__builtin_ctzl(word);
			return from < size ? from : size;
		}
		from = (from / BITS_PER_LONG + 1) * BITS_PER_LONG;
	}
	return size;
}

/* 0 with *id_out, or -ENOSPC.  Called with the lock held. */
static int vmw_gmrid_id_alloc(struct vmw_gmrid_man *gman, uint32_t *id_out)
{
	uint32_t id = vmw_gmrid_find_free(gman->ids, gman->max_ids,
					  gman->id_hint);

	if (id >= gman->max_ids) {
		gman->id_hint = gman->max_ids;
		return -ENOSPC;
	}
	__set_bit(id, gman->ids);
	gman->id_hint = id + 1;
	gman->used_ids++;
	*id_out = id;
	return 0;
}

/* Called with the lock held. */
static void vmw_gmrid_id_free(struct vmw_gmrid_man *gman, uint32_t id)
{
	if (WARN_ON_ONCE(id >= gman->max_ids || !test_bit(id, gman->ids)))
		return;
	__clear_bit(id, gman->ids);
	if (id < gman->id_hint)
		gman->id_hint = id;
	gman->used_ids--;
}

/* ---- pages -------------------------------------------------------------- */

enum vmw_gmrid_budget_event {
	VMW_GMRID_FIT = 0,
	VMW_GMRID_GREW,
	VMW_GMRID_REFUSED,
};

/* Account `num_pages' against the budget, growing it when it overflows.
 * Called with the lock held; what happened is returned so the warnings can
 * be printed after the lock is dropped (the host log is port I/O and
 * slow).
 *
 * The device's figure is a soft limit: it accepts more than it states.
 * Failing the allocation the moment the figure is passed would make the
 * client stack fail where it need not, so the budget grows instead -- to
 * twice the figure, then to half of RAM -- and only an allocation that does
 * not fit even then is refused.  The warnings tell the user which knob to
 * turn (guest RAM, the VM's graphics memory). */
static enum vmw_gmrid_budget_event
vmw_gmrid_pages_take(struct vmw_gmrid_man *gman, uint32_t num_pages)
{
	if (gman->max_pages == 0)
		return VMW_GMRID_FIT;
	/* A count that would wrap fits nowhere. */
	if (num_pages > U32_MAX - gman->used_pages)
		return VMW_GMRID_REFUSED;

	gman->used_pages += num_pages;
	if (likely(gman->used_pages <= gman->max_pages))
		return VMW_GMRID_FIT;

	const uint32_t max_graphics_pages = gman->max_graphics_pages;
	uint32_t new_max_pages;

	if (gman->max_pages > max_graphics_pages / 2)
		new_max_pages = max_graphics_pages;
	else
		new_max_pages = gman->max_pages * 2;
	if (new_max_pages > gman->max_pages &&
	    new_max_pages >= gman->used_pages) {
		gman->max_pages = new_max_pages;
		return VMW_GMRID_GREW;
	}
	gman->used_pages -= num_pages;
	return VMW_GMRID_REFUSED;
}

static void vmw_gmrid_budget_report(struct vmw_device *v,
				    enum vmw_gmrid_budget_event ev,
				    uint8_t type, uint32_t max_pages)
{
	char buf[160];

	if (ev == VMW_GMRID_FIT)
		return;
	if (__atomic_fetch_add(&vmw_gmrid_warned, 1, __ATOMIC_RELAXED) >=
	    VMW_GMRID_WARN_MAX)
		return;
	if (ev == VMW_GMRID_GREW) {
		kprintf("[drm] vmwgfx: %s memory overflow. Consider increasing guest RAM and graphicsMemory.\n",
			vmw_gmrid_name(type));
		kprintf("[drm] vmwgfx: increasing guest %s limits to %llu KiB.\n",
			vmw_gmrid_name(type),
			(unsigned long long)max_pages * (PAGE_SIZE / 1024));
		ksnprintf(buf, sizeof(buf),
			  "vmwgfx, warning: %s memory overflow. Consider increasing guest RAM and graphicsMemory.\n",
			  vmw_gmrid_name(type));
	} else {
		ksnprintf(buf, sizeof(buf),
			  "vmwgfx, error: guest graphics is out of memory (%s limit at: %llu KiB).\n",
			  vmw_gmrid_name(type),
			  (unsigned long long)max_pages * (PAGE_SIZE / 1024));
		kprintf("[drm] %s", buf);
	}
	if (v->has_msg)
		vmw_host_log(buf);
}

/* ---- the manager -------------------------------------------------------- */

int vmw_gmrid_man_init(struct vmw_device *v, int type)
{
	struct vmw_gmrid_man *gman;
	memory_stats_t st;
	uint32_t max_ids = 0, max_pages = 0;

	if (type != VMW_PL_GMR && type != VMW_PL_MOB)
		return -EINVAL;
	if (v->gmrid_man[type])
		return 0;

	switch (type) {
	case VMW_PL_GMR:
		vmsvga2_hw_gmr_limits(&max_ids, &max_pages);
		if (!max_ids)
			return -ENODEV;
		break;
	case VMW_PL_MOB: {
		uint64_t pages = v->max_mob_memory / PAGE_SIZE;

		if (!v->has_gb)
			return -ENODEV;
		max_ids = VMW_NUM_MOBS;
		max_pages = pages > U32_MAX ? U32_MAX : (uint32_t)pages;
		break;
	}
	}

	gman = kalloc(sizeof(*gman));
	if (!gman)
		return -ENOMEM;
	mm_memset(gman, 0, sizeof(*gman));
	spinlock_init(&gman->lock, "vmw_gmrid");
	gman->type = (uint8_t)type;
	gman->max_ids = max_ids;
	gman->max_pages = max_pages;

	/* RAM does not change under the driver: its half is taken once
	 * here rather than inside the lock on every overflow. */
	mm_memset(&st, 0, sizeof(st));
	mm_get_memory_stats(&st);
	gman->max_graphics_pages = st.total_pages / 2 > U32_MAX ?
					   U32_MAX :
					   (uint32_t)(st.total_pages / 2);

	if (type == VMW_PL_MOB) {
		size_t bytes = BITS_TO_LONGS(max_ids) * sizeof(unsigned long);

		gman->ids = kalloc(bytes);
		if (!gman->ids) {
			kfree(gman);
			return -ENOMEM;
		}
		mm_memset(gman->ids, 0, bytes);
		/* Id 0 is never handed out: userspace treats 0 as a valid id
		 * but a few consumers assume "0 = none". */
		__set_bit(0, gman->ids);
		gman->id_hint = 1;
	}

	v->gmrid_man[type] = gman;
	kprintf("[drm] vmwgfx: %s accounting: %u ids, %u pages (may grow to %u)\n",
		vmw_gmrid_name(gman->type), gman->max_ids, gman->max_pages,
		gman->max_pages ? gman->max_graphics_pages : 0);
	return 0;
}

void vmw_gmrid_man_fini(struct vmw_device *v, int type)
{
	struct vmw_gmrid_man *gman;

	if (type != VMW_PL_GMR && type != VMW_PL_MOB)
		return;
	gman = v->gmrid_man[type];
	if (!gman)
		return;
	v->gmrid_man[type] = NULL;
	/* Nothing can be evicted here: an object still accounted keeps its
	 * id and pages on the device, and its put_node will find no manager
	 * and only do the hardware half. */
	if (gman->used_ids || gman->used_pages)
		kprintf("[drm] vmwgfx: %s accounting torn down with %u ids, %u pages still in use\n",
			vmw_gmrid_name(gman->type), gman->used_ids,
			gman->used_pages);
	kfree(gman->ids);
	kfree(gman);
}

int vmw_gmrid_man_get_node(struct vmw_device *v, int type, uint32_t num_pages,
			   uint32_t *id_out)
{
	struct vmw_gmrid_man *gman;
	enum vmw_gmrid_budget_event ev;
	uint32_t max_pages;
	uint64_t fl;
	int rc = 0;

	*id_out = SVGA3D_INVALID_ID;
	if (type != VMW_PL_GMR && type != VMW_PL_MOB)
		return -EINVAL;
	gman = v->gmrid_man[type];
	if (!gman) {
		int id;

		if (type == VMW_PL_MOB)
			return -ENODEV;
		id = vmsvga2_gmr_alloc(num_pages);
		if (id < 0)
			return -ENOSPC;
		*id_out = (uint32_t)id;
		return 0;
	}

	spin_lock_irqsave(&gman->lock, &fl);
	if (gman->ids) {
		rc = vmw_gmrid_id_alloc(gman, id_out);
		if (rc) {
			spin_unlock_irqrestore(&gman->lock, fl);
			return rc;
		}
	}
	ev = vmw_gmrid_pages_take(gman, num_pages);
	max_pages = gman->max_pages;
	if (ev == VMW_GMRID_REFUSED) {
		if (gman->ids)
			vmw_gmrid_id_free(gman, *id_out);
		*id_out = SVGA3D_INVALID_ID;
		rc = -ENOSPC;
	}
	spin_unlock_irqrestore(&gman->lock, fl);
	vmw_gmrid_budget_report(v, ev, gman->type, max_pages);
	if (rc || gman->ids)
		return rc;

	/* A region: the budget is taken, now the id, from the one table
	 * there is.  It refuses for its own reasons too (all ids taken, a
	 * region over the device's per-region limit) -- the budget goes back
	 * then. */
	int id = vmsvga2_gmr_alloc(num_pages);

	if (id < 0) {
		spin_lock_irqsave(&gman->lock, &fl);
		if (gman->max_pages)
			gman->used_pages -= num_pages;
		spin_unlock_irqrestore(&gman->lock, fl);
		return -ENOSPC;
	}
	spin_lock_irqsave(&gman->lock, &fl);
	gman->used_ids++;
	spin_unlock_irqrestore(&gman->lock, fl);
	*id_out = (uint32_t)id;
	return 0;
}

void vmw_gmrid_man_put_node(struct vmw_device *v, int type, uint32_t id,
			    uint32_t num_pages)
{
	struct vmw_gmrid_man *gman;
	uint64_t fl;

	if (type != VMW_PL_GMR && type != VMW_PL_MOB)
		return;
	if (id == SVGA3D_INVALID_ID)
		return;
	/* The region is unbound and its id freed first: the budget it held is
	 * only free once the device no longer has the region. */
	if (type == VMW_PL_GMR)
		vmsvga2_gmr_free((int)id);
	gman = v->gmrid_man[type];
	if (!gman)
		return;
	spin_lock_irqsave(&gman->lock, &fl);
	if (gman->ids)
		vmw_gmrid_id_free(gman, id);
	else if (!WARN_ON_ONCE(gman->used_ids == 0))
		gman->used_ids--;
	if (gman->max_pages) {
		if (WARN_ON_ONCE(num_pages > gman->used_pages))
			gman->used_pages = 0;
		else
			gman->used_pages -= num_pages;
	}
	spin_unlock_irqrestore(&gman->lock, fl);
}
