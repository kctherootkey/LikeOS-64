// LikeOS VMware SVGA II display driver -- guest memory regions (GMR):
// DMA-visible guest pages the host can read and write directly.  Two
// mechanisms, both capability-gated: GMR2 (FIFO commands, page-list based)
// and legacy GMR (register-programmed descriptor chains).  All page addresses
// are page-aligned by construction (DMA alignment requirement of the device).
//
// GMR2 binding is a DEFINE_GMR2 (id, size in pages) followed by REMAP_GMR2
// commands that fill in the page numbers, a run of pages per command.  A
// REMAP carries at most SVGA_GMR2_REMAP_PPN_BYTES of page numbers, so a large
// region becomes several, and none of them -- nor the whole sequence, when it
// is sent as one reservation -- is ever larger than the FIFO accepts
// (vmsvga2_hw_fifo_max_bytes()).  Page numbers are 32-bit while every page
// lies below 16 TB (4 KB pages, 2^32 of them), 64-bit otherwise: half the
// command bytes in the common case.  Unbinding is a DEFINE_GMR2 of zero pages.
//
// The id and page limits are the device's, read once at probe.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's and Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2015 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include "vmsvga2_priv.h"
#include <kernel/mm/memory.h>

// 32-bit page numbers in REMAP_GMR2 whenever every page allows it.  Off
// sends 64-bit page numbers always, as before.
#ifndef VMSVGA2_GMR2_PPN32
#define VMSVGA2_GMR2_PPN32 1
#endif

// The device addresses guest memory in 4 KB pages.
#define SVGA_GMR_PAGE_SHIFT 12
// The first address whose page number no longer fits in 32 bits (16 TB).
#define SVGA_GMR_PPN32_LIMIT (1ULL << (32 + SVGA_GMR_PAGE_SHIFT))
// The most page-number bytes one REMAP_GMR2 carries.  Well below anything a
// host limits a single command to, and a remap of this size (plus its header)
// fits the smallest FIFO bounce buffer, so the split never depends on the
// host.
#define SVGA_GMR2_REMAP_PPN_BYTES (31U * 1024U)

#define SVGA_GMR2_DEFINE_BYTES                                               \
	((uint32_t)(sizeof(uint32_t) + sizeof(SVGAFifoCmdDefineGMR2)))
#define SVGA_GMR2_REMAP_HDR_BYTES                                            \
	((uint32_t)(sizeof(uint32_t) + sizeof(SVGAFifoCmdRemapGMR2)))

static spinlock_t svga_gmr_lock = SPINLOCK_INIT("svga_gmr");

// Id table states.  An id being freed is still taken: it is unbound before
// it can be handed out again, so a new owner's bind can never be undone by
// the old owner's unbind.
#define SVGA_GMR_FREE 0
#define SVGA_GMR_USED 1
#define SVGA_GMR_FREEING 2

typedef struct {
	int used; // SVGA_GMR_*
	uint32_t num_pages;
	uint64_t desc_phys; // legacy descriptor page (0 when GMR2)
	int bound; // the last bind reached the device (counted on resume)
} svga_gmr_t;

static svga_gmr_t g_gmrs[SVGA_MAX_GMRS];

static int svga_has_gmr2(void)
{
	return (g_svga.caps & SVGA_CAP_GMR2) != 0;
}

static int svga_has_gmr(void)
{
	return (g_svga.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) != 0;
}

static inline uint32_t svga_gmr_min(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

// The device's limits as read once at probe (svga_device_gmr_limits()): the
// usable ids, at most SVGA_MAX_GMRS (the size of the id table here), and for
// GMR2 the pages over all regions (0: no limit given).
static void svga_gmr_limits_get(uint32_t *max_ids, uint32_t *max_pages)
{
	uint32_t ids, pages;

	svga_device_gmr_limits(&ids, &pages);
	if (!svga_has_gmr())
		ids = 0;
	if (ids > SVGA_MAX_GMRS)
		ids = SVGA_MAX_GMRS;
	if (!svga_has_gmr2())
		pages = 0;
	if (max_ids)
		*max_ids = ids;
	if (max_pages)
		*max_pages = pages;
}

void svga_gmr_init(void)
{
	uint32_t ids, pages;

	svga_gmr_limits_get(&ids, &pages);
	if (svga_has_gmr2())
		kprintf("svga2: GMR2: %u region ids, %u pages\n", ids, pages);
	else if (svga_has_gmr())
		kprintf("svga2: GMR (descriptor): %u region ids\n", ids);
}

void vmsvga2_hw_gmr_limits(uint32_t *max_ids, uint32_t *max_pages)
{
	if (!g_svga.present || !svga_has_gmr()) {
		if (max_ids)
			*max_ids = 0;
		if (max_pages)
			*max_pages = 0;
		return;
	}
	svga_gmr_limits_get(max_ids, max_pages);
}

int vmsvga2_gmr_alloc(uint32_t num_pages)
{
	uint64_t f;
	uint32_t max_ids, max_pages;
	int id;

	if (!g_svga.present || !svga_has_gmr() || num_pages == 0)
		return -1;
	svga_gmr_limits_get(&max_ids, &max_pages);
	if (svga_has_gmr2() && max_pages && num_pages > max_pages)
		return -1;

	spin_lock_irqsave(&svga_gmr_lock, &f);
	for (id = 0; id < (int)max_ids; id++) {
		if (g_gmrs[id].used == SVGA_GMR_FREE) {
			g_gmrs[id].used = SVGA_GMR_USED;
			g_gmrs[id].num_pages = num_pages;
			g_gmrs[id].desc_phys = 0;
			g_gmrs[id].bound = 0;
			spin_unlock_irqrestore(&svga_gmr_lock, f);
			return id;
		}
	}
	spin_unlock_irqrestore(&svga_gmr_lock, f);
	return -1;
}

// ===========================================================================
// GMR2
// ===========================================================================

// DEFINE_GMR2 into `cmd'; returns the dwords written.
static uint32_t svga_gmr2_emit_define(uint32_t *cmd, int gmr_id,
				      uint32_t num_pages)
{
	SVGAFifoCmdDefineGMR2 define_cmd;

	define_cmd.gmrId = (uint32_t)gmr_id;
	define_cmd.numPages = num_pages;
	cmd[0] = SVGA_CMD_DEFINE_GMR2;
	kmemcpy(&cmd[1], &define_cmd, sizeof(define_cmd));
	return SVGA_GMR2_DEFINE_BYTES / sizeof(uint32_t);
}

// REMAP_GMR2 of pages [off, off + n) into `cmd'; returns the dwords written.
static uint32_t svga_gmr2_emit_remap(uint32_t *cmd, int gmr_id,
				     const uint64_t *page_phys, uint32_t off,
				     uint32_t n, int ppn64)
{
	SVGAFifoCmdRemapGMR2 remap_cmd;
	uint32_t *p = cmd;
	uint32_t i;

	remap_cmd.gmrId = (uint32_t)gmr_id;
	remap_cmd.flags = ppn64 ? SVGA_REMAP_GMR2_PPN64 : SVGA_REMAP_GMR2_PPN32;
	remap_cmd.offsetPages = off;
	remap_cmd.numPages = n;

	*p++ = SVGA_CMD_REMAP_GMR2;
	kmemcpy(p, &remap_cmd, sizeof(remap_cmd));
	p += sizeof(remap_cmd) / sizeof(*p);

	for (i = 0; i < n; i++) {
		uint64_t ppn = page_phys[off + i] >> SVGA_GMR_PAGE_SHIFT;

		if (ppn64) {
			// Little-endian, dword-aligned: low half first.
			*p++ = (uint32_t)ppn;
			*p++ = (uint32_t)(ppn >> 32);
		} else {
			*p++ = (uint32_t)ppn;
		}
	}
	return (uint32_t)(p - cmd);
}

// Down the owner's command channel, so the region is defined before anything
// on that channel names it.  Each command is handed over on its own (the
// channel gathers them); the last one announces the run.
static int svga_gmr2_bind_channel(int gmr_id, const uint64_t *page_phys,
				  uint32_t num_pages, int ppn64,
				  uint32_t per_remap)
{
	uint32_t ppn_size = ppn64 ? 8 : 4;
	uint32_t def[SVGA_GMR2_DEFINE_BYTES / sizeof(uint32_t)];
	uint32_t *remap;
	uint32_t off;

	remap = kalloc(SVGA_GMR2_REMAP_HDR_BYTES + per_remap * ppn_size);
	if (!remap)
		return -1;
	svga_gmr2_emit_define(def, gmr_id, num_pages);
	if (g_cmd_channel(def, sizeof(def), 0) != 0) {
		kfree(remap);
		return -1;
	}
	for (off = 0; off < num_pages; off += per_remap) {
		uint32_t n = svga_gmr_min(num_pages - off, per_remap);
		uint32_t words =
			svga_gmr2_emit_remap(remap, gmr_id, page_phys, off, n,
					     ppn64);
		int last = off + n >= num_pages;

		if (g_cmd_channel(remap, words * sizeof(uint32_t), last) != 0) {
			kfree(remap);
			return -1;
		}
	}
	kfree(remap);
	return 0;
}

// Into the FIFO.  The whole sequence in one reservation when it fits, so it
// is one atomic run; otherwise the DEFINE and each REMAP in reservations of
// their own -- nothing names the id until the bind has returned, so commands
// of other users landing between them change nothing.
static int svga_gmr2_bind_fifo(int gmr_id, const uint64_t *page_phys,
			       uint32_t num_pages, int ppn64,
			       uint32_t per_remap)
{
	uint32_t ppn_size = ppn64 ? 8 : 4;
	uint32_t max_bytes = vmsvga2_hw_fifo_max_bytes();
	uint32_t remap_num = num_pages / per_remap + (num_pages % per_remap > 0);
	uint64_t total = (uint64_t)SVGA_GMR2_DEFINE_BYTES +
			 (uint64_t)remap_num * SVGA_GMR2_REMAP_HDR_BYTES +
			 (uint64_t)num_pages * ppn_size;
	uint32_t *cmd;
	uint32_t off, words;

	if (max_bytes < SVGA_GMR2_REMAP_HDR_BYTES + ppn_size)
		return -1;

	if (total <= max_bytes) {
		cmd = vmsvga2_hw_fifo_reserve((uint32_t)total);
		if (!cmd)
			return -1;
		words = svga_gmr2_emit_define(cmd, gmr_id, num_pages);
		for (off = 0; off < num_pages; off += per_remap) {
			uint32_t n = svga_gmr_min(num_pages - off, per_remap);

			words += svga_gmr2_emit_remap(cmd + words, gmr_id,
						      page_phys, off, n,
						      ppn64);
		}
		vmsvga2_hw_fifo_commit((uint32_t)total);
		// Checked once the FIFO lock is gone: printing under it
		// deadlocks against the console's flush.
		WARN_ON_ONCE(words * sizeof(uint32_t) != total);
		return 0;
	}

	// Split.  A remap no larger than a reservation may be.
	if (SVGA_GMR2_REMAP_HDR_BYTES + per_remap * ppn_size > max_bytes)
		per_remap = (max_bytes - SVGA_GMR2_REMAP_HDR_BYTES) / ppn_size;

	cmd = vmsvga2_hw_fifo_reserve(SVGA_GMR2_DEFINE_BYTES);
	if (!cmd)
		return -1;
	svga_gmr2_emit_define(cmd, gmr_id, num_pages);
	vmsvga2_hw_fifo_commit(SVGA_GMR2_DEFINE_BYTES);

	for (off = 0; off < num_pages; off += per_remap) {
		uint32_t n = svga_gmr_min(num_pages - off, per_remap);
		uint32_t bytes = SVGA_GMR2_REMAP_HDR_BYTES + n * ppn_size;

		cmd = vmsvga2_hw_fifo_reserve(bytes);
		if (!cmd)
			return -1;
		words = svga_gmr2_emit_remap(cmd, gmr_id, page_phys, off, n,
					     ppn64);
		vmsvga2_hw_fifo_commit(bytes);
		WARN_ON_ONCE(words * sizeof(uint32_t) != bytes);
	}
	return 0;
}

static int svga_gmr2_bind(int gmr_id, const uint64_t *page_phys,
			  uint32_t num_pages)
{
	int ppn64 = !VMSVGA2_GMR2_PPN32;
	uint32_t ppn_size, per_remap, i;

	BUILD_BUG_ON(sizeof(SVGAFifoCmdDefineGMR2) != 8);
	BUILD_BUG_ON(sizeof(SVGAFifoCmdRemapGMR2) != 16);

	for (i = 0; i < num_pages && !ppn64; i++)
		if (page_phys[i] >= SVGA_GMR_PPN32_LIMIT)
			ppn64 = 1;
	ppn_size = ppn64 ? 8 : 4;
	per_remap = SVGA_GMR2_REMAP_PPN_BYTES / ppn_size;

	if (g_cmd_channel)
		return svga_gmr2_bind_channel(gmr_id, page_phys, num_pages,
					      ppn64, per_remap);
	return svga_gmr2_bind_fifo(gmr_id, page_phys, num_pages, ppn64,
				   per_remap);
}

static void svga_gmr2_unbind(int gmr_id)
{
	uint32_t cmd[SVGA_GMR2_DEFINE_BYTES / sizeof(uint32_t)];

	svga_gmr2_emit_define(cmd, gmr_id, 0);
	if (g_cmd_channel)
		(void)g_cmd_channel(cmd, sizeof(cmd), 1);
	else
		(void)svga_fifo_write_cmd(cmd, sizeof(cmd));
}

// ===========================================================================
// Legacy descriptor GMR
// ===========================================================================

// Build a descriptor page (ppn/num_pages run list, {0,0}-terminated) and
// hand its PPN to the device.  One 4K page holds 511 descriptors +
// terminator; runs of contiguous pages are coalesced, so this covers any
// realistic buffer object.
static int svga_gmr_legacy_bind(int gmr_id, const uint64_t *page_phys,
				uint32_t num_pages)
{
	uint64_t desc_phys = mm_allocate_physical_page();
	uint64_t old_phys;
	SVGAGuestMemDescriptor *desc;
	uint32_t d = 0;
	uint32_t i;

	if (!desc_phys)
		return -1;
	desc = (SVGAGuestMemDescriptor *)phys_to_virt(desc_phys);
	for (i = 0; i < num_pages && d < 510; i++) {
		uint32_t ppn = (uint32_t)(page_phys[i] >> SVGA_GMR_PAGE_SHIFT);

		// Coalesce runs of physically contiguous pages.
		if (d > 0 && ppn == desc[d - 1].ppn + desc[d - 1].numPages) {
			desc[d - 1].numPages++;
			continue;
		}
		desc[d].ppn = ppn;
		desc[d].numPages = 1;
		d++;
	}
	if (i < num_pages) {
		// Page list too fragmented for a single descriptor page;
		// chaining is not implemented (buffer objects use contiguous
		// memory, which always coalesces).
		mm_free_physical_page(desc_phys);
		WARN_ON_ONCE(1);
		return -1;
	}
	desc[d].ppn = 0;
	desc[d].numPages = 0; // terminator

	svga_write_reg(SVGA_REG_GMR_ID, (uint32_t)gmr_id);
	svga_write_reg(SVGA_REG_GMR_DESCRIPTOR,
		       (uint32_t)(desc_phys >> SVGA_GMR_PAGE_SHIFT));
	// A region bound again (its owner rebinding it after a resume) gets
	// the new page; the one the device was pointed at before is no longer
	// referenced once the register names the new one.
	old_phys = g_gmrs[gmr_id].desc_phys;
	g_gmrs[gmr_id].desc_phys = desc_phys;
	if (old_phys)
		mm_free_physical_page(old_phys);
	return 0;
}

// Program the region's descriptor page into the device again.
static void svga_gmr_legacy_reprogram(int gmr_id, uint64_t desc_phys)
{
	svga_write_reg(SVGA_REG_GMR_ID, (uint32_t)gmr_id);
	svga_write_reg(SVGA_REG_GMR_DESCRIPTOR,
		       (uint32_t)(desc_phys >> SVGA_GMR_PAGE_SHIFT));
}

// ===========================================================================
// Bind / free
// ===========================================================================

// Bind guest pages to a GMR id.  page_phys[] entries must be page-aligned.
int vmsvga2_gmr_bind(int gmr_id, const uint64_t *page_phys,
		     uint32_t num_pages)
{
	uint32_t i;
	int rc;

	if (!g_svga.present || gmr_id < 0 || gmr_id >= SVGA_MAX_GMRS)
		return -1;
	if (g_gmrs[gmr_id].used != SVGA_GMR_USED || !page_phys ||
	    num_pages == 0)
		return -1;
	if (WARN_ON_ONCE(num_pages != g_gmrs[gmr_id].num_pages))
		return -1;
	for (i = 0; i < num_pages; i++)
		if (WARN_ON_ONCE(page_phys[i] & (PAGE_SIZE - 1)))
			return -1; // DMA alignment violation

	if (svga_has_gmr2())
		rc = svga_gmr2_bind(gmr_id, page_phys, num_pages);
	else
		rc = svga_gmr_legacy_bind(gmr_id, page_phys, num_pages);
	g_gmrs[gmr_id].bound = rc == 0;
	return rc;
}

int vmsvga2_gmr_free(int gmr_id)
{
	uint64_t f;

	if (!g_svga.present || gmr_id < 0 || gmr_id >= SVGA_MAX_GMRS)
		return -1;
	spin_lock_irqsave(&svga_gmr_lock, &f);
	if (g_gmrs[gmr_id].used != SVGA_GMR_USED) {
		spin_unlock_irqrestore(&svga_gmr_lock, f);
		return -1;
	}
	g_gmrs[gmr_id].used = SVGA_GMR_FREEING;
	spin_unlock_irqrestore(&svga_gmr_lock, f);

	// Ensure the host is done with the region before unbinding.  On the
	// owner's channel the unbind simply follows whatever named the region,
	// in order; the FIFO drain is for the legacy path.
	vmsvga2_fifo_flush();
	if (svga_has_gmr2()) {
		svga_gmr2_unbind(gmr_id);
	} else {
		svga_write_reg(SVGA_REG_GMR_ID, (uint32_t)gmr_id);
		svga_write_reg(SVGA_REG_GMR_DESCRIPTOR, 0);
	}
	if (g_gmrs[gmr_id].desc_phys) {
		mm_free_physical_page(g_gmrs[gmr_id].desc_phys);
		g_gmrs[gmr_id].desc_phys = 0;
	}
	g_gmrs[gmr_id].bound = 0;

	// Only now may the id be handed out again.
	spin_lock_irqsave(&svga_gmr_lock, &f);
	g_gmrs[gmr_id].used = SVGA_GMR_FREE;
	spin_unlock_irqrestore(&svga_gmr_lock, f);
	return 0;
}

// ===========================================================================
// Resume
// ===========================================================================

/* After the device was re-initialized every region it had is gone.  Who
 * brings each one back is decided by who holds what it takes:
 *
 *   legacy (descriptor) regions: this file -- the descriptor page that
 *       describes the region is this file's, so programming it again is two
 *       register writes and needs nothing from the owner;
 *   GMR2 regions: their owner -- the page list went into DEFINE/REMAP
 *       commands and is not kept here; the owner still has it and binds the
 *       same id again (vmsvga2_gmr_bind()), which is why ids stay allocated
 *       across a suspend.
 *
 * A legacy region is programmed with svga_gmr_lock held (svga_reg_lock
 * nests inside it, nothing nests inside that): a free that marks the region
 * SVGA_GMR_FREEING does so either before -- the region is skipped -- or
 * after, and then its own descriptor-0 write and the release of the page
 * come after this write, never under it. */
uint32_t svga_gmr_replay(uint32_t *gmr2_bound)
{
	uint32_t replayed = 0, gmr2 = 0;
	int id;

	for (id = 0; id < SVGA_MAX_GMRS; id++) {
		uint64_t f;

		spin_lock_irqsave(&svga_gmr_lock, &f);
		if (g_gmrs[id].used == SVGA_GMR_USED && g_gmrs[id].bound) {
			if (g_gmrs[id].desc_phys) {
				svga_gmr_legacy_reprogram(id,
							  g_gmrs[id].desc_phys);
				replayed++;
			} else if (svga_has_gmr2()) {
				gmr2++;
			}
		}
		spin_unlock_irqrestore(&svga_gmr_lock, f);
	}
	if (gmr2_bound)
		*gmr2_bound = gmr2;
	return replayed;
}
