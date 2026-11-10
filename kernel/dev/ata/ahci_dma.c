// LikeOS AHCI driver - DMA memory, address translation, bounce buffers
//
// Everything the HBA reads or writes in system memory goes through here:
//
//   * the per-port STRUCTURES: the 1 KiB command list (32 command headers,
//     1 KiB aligned), the received-FIS area (256 bytes aligned to 256, or
//     4 KiB aligned to 4 KiB when FIS-based switching gives every device
//     behind a port multiplier its own area) and one 4 KiB command table
//     per slot (128-byte aligned; 248 PRD entries fill the page exactly).
//     They are allocated as physically contiguous runs of whole pages from
//     the page allocator, zeroed, and placed below 4 GiB unless the HBA can
//     address 64 bits (CAP.S64A and no 32-bit quirk).
//
//   * the ADDRESS TRANSLATION of data buffers: a block request's
//     scatter/gather list holds kernel virtual addresses, which are
//     translated page by page (a vmalloc-style buffer is contiguous only
//     virtually), merged back into the longest physically contiguous
//     runs, split at the 4 MiB a PRD entry can carry, and checked against
//     the rules of the PRDT (word-aligned base, even byte count).  The CPU
//     physical address goes through ahci_dma_translate() before the HBA
//     sees it -- the one place an IOMMU driver would hook in; the kernel
//     has none, so it is the identity today.
//
//   * BOUNCE BUFFERS: a 32-bit HBA cannot reach memory above 4 GiB, and no
//     HBA can take an odd address or length.  Such a command runs through
//     a contiguous low bounce area: written data is copied in before the
//     command is issued, read data is copied back out after it completed.
//
//   * CACHE COHERENCY: x86 DMA is cache-coherent, so the ordering rules are
//     what matter -- the command header, FIS and PRDT are ordinary stores
//     that must be globally visible before the CI write hands them to the
//     HBA (ahci_wmb), and device-written memory is read only after the
//     completion was observed (ahci_rmb).  For a platform or an emulation
//     whose DMA is not snooped, `ahci_dma_noncoherent' (debug knob, off by
//     default) adds explicit cache-line write-back/invalidate around every
//     transfer and command table.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define AHCI_PAGE_SIZE 4096ULL
#define AHCI_4G 0x100000000ULL
/* What a translation callback returns for an address it cannot map. */
#define AHCI_DMA_BAD_ADDR (~0ULL)

/* Debug knob: treat DMA as not cache-coherent (clflush around transfers).
 * Set through /sys/kernel/debug/ahci/dma_noncoherent. */
volatile int ahci_dma_noncoherent;

/* Allocation statistics (diagnostics) */
static volatile uint64_t g_ahci_dma_pages_live;
static volatile uint64_t g_ahci_bounce_live;
static volatile uint64_t g_ahci_bounce_total;
static volatile uint64_t g_ahci_bounce_fail;

uint64_t ahci_dma_stat_pages(void)
{
	return g_ahci_dma_pages_live;
}

uint64_t ahci_dma_stat_bounces(uint64_t *live, uint64_t *failed)
{
	if (live)
		*live = g_ahci_bounce_live;
	if (failed)
		*failed = g_ahci_bounce_fail;
	return g_ahci_bounce_total;
}

/* ==========================================================================
 * Translation
 * ========================================================================== */

uint64_t ahci_dma_translate(const struct ahci_hba *hba, uint64_t phys)
{
	/* The IOMMU hook.  Without a DMA remapping driver the bus address of
	 * a page is its CPU physical address.  When one exists, this is the
	 * single place that maps `phys' into the HBA's I/O address space
	 * (hba->pci identifies the requester). */
	(void)hba;
	return phys;
}

/* Kernel virtual -> bus address for one byte range that does not cross a
 * page boundary.  The direct map is a subtraction; anything else is a page
 * table walk (heap, vmalloc-style mappings). */
static uint64_t ahci_dma_virt_to_bus(const struct ahci_hba *hba,
				     const void *virt)
{
	uint64_t va = (uint64_t)virt;
	uint64_t phys;

	if (!virt)
		return AHCI_DMA_BAD_ADDR;
	phys = mm_get_physical_address(va);
	if (phys == 0)
		return AHCI_DMA_BAD_ADDR;
	return ahci_dma_translate(hba, phys);
}

/* ==========================================================================
 * Contiguous areas
 * ========================================================================== */

int ahci_dma_alloc(struct ahci_hba *hba, size_t size, size_t align,
		   struct ahci_dma_area *out)
{
	uint64_t phys, end;
	size_t pages;
	int need_low;

	if (!out)
		return -EINVAL;
	kmemset(out, 0, sizeof(*out));
	if (size == 0 || align == 0 || (align & (align - 1)) != 0)
		return -EINVAL;

	pages = (size + AHCI_PAGE_SIZE - 1) / AHCI_PAGE_SIZE;
	need_low = !hba || !hba->dma64;

	if (align > AHCI_PAGE_SIZE)
		phys = mm_allocate_contiguous_pages_aligned(
			pages, align / AHCI_PAGE_SIZE);
	else
		phys = mm_allocate_contiguous_pages(pages);
	if (!phys)
		return -ENOMEM;

	end = phys + pages * AHCI_PAGE_SIZE;
	if (need_low && end > AHCI_4G) {
		/* The allocator serves contiguous runs from low memory while
		 * it has any; this happens only when that is exhausted. */
		kprintf("[ahci] %s: no DMA memory below 4 GiB for %lu bytes (got 0x%llx)\n",
			hba ? hba->name : "ahci", (unsigned long)size,
			(unsigned long long)phys);
		mm_free_contiguous_pages(phys, pages);
		return -ENOMEM;
	}
	if (WARN_ON((phys & (align - 1)) != 0)) {
		mm_free_contiguous_pages(phys, pages);
		return -ENOMEM;
	}

	out->phys = phys;
	out->virt = phys_to_virt(phys);
	out->bus = ahci_dma_translate(hba, phys);
	out->size = size;
	out->pages = (uint32_t)pages;
	out->flags = (end <= AHCI_4G) ? AHCI_DMA_F_BELOW_4G : 0;
	kmemset(out->virt, 0, pages * AHCI_PAGE_SIZE);
	__atomic_add_fetch(&g_ahci_dma_pages_live, pages, __ATOMIC_RELAXED);
	return 0;
}

void ahci_dma_free(struct ahci_dma_area *a)
{
	if (!a || !a->phys || !a->pages) {
		if (a)
			kmemset(a, 0, sizeof(*a));
		return;
	}
	mm_free_contiguous_pages(a->phys, a->pages);
	__atomic_sub_fetch(&g_ahci_dma_pages_live, (uint64_t)a->pages,
			   __ATOMIC_RELAXED);
	kmemset(a, 0, sizeof(*a));
}

/* ==========================================================================
 * Per-port structures
 * ========================================================================== */

/* Does this port get the 4 KiB FIS-based-switching receive area?  The
 * area is sized once, at allocation, for what the port could ever use. */
static int ahci_port_wants_fbs_area(const struct ahci_port *port)
{
	const struct ahci_hba *hba = port->hba;
	uint32_t cmd;

	if (port->fbs_capable)
		return 1;
	if (hba->quirks & AHCI_Q_NO_FBS)
		return 0;
	if (!hba->caps.spm)
		return 0;
	if (hba->quirks & AHCI_Q_YES_FBS)
		return 1;
	if (!hba->caps.fbss)
		return 0;
	if (port->mmio) {
		cmd = ahci_port_readl(port, AHCI_PxCMD);
	} else if (hba->mmio && port->index >= 0 &&
		   port->index < AHCI_MAX_PORTS) {
		/* allocated before its register window was recorded */
		cmd = ahci_readl(hba->mmio + AHCI_PORT_BASE +
					 (uint32_t)port->index * AHCI_PORT_SIZE,
				 AHCI_PxCMD);
	} else {
		return 0;
	}
	return cmd != 0xFFFFFFFFu && (cmd & AHCI_PxCMD_FBSCP) != 0;
}

int ahci_port_dma_alloc(struct ahci_port *port)
{
	struct ahci_hba *hba;
	struct ahci_cmd_table *tables;
	uint32_t nr_slots;
	size_t rx_size, rx_align;
	int rc;

	BUG_ON(port == NULL);
	hba = port->hba;
	BUG_ON(hba == NULL);
	if (WARN_ON(port->cmd_list_area.virt || port->cmd_table_area.virt))
		return -EBUSY; /* already allocated */

	nr_slots = port->nr_slots ? port->nr_slots : hba->caps.n_slots;
	if (nr_slots == 0 || nr_slots > AHCI_MAX_SLOTS) {
		WARN(1, "[ahci] %s: %u command slots, using %u", port->name,
		     nr_slots, nr_slots ? AHCI_MAX_SLOTS : 1);
		nr_slots = nr_slots ? AHCI_MAX_SLOTS : 1;
	}

	if (ahci_port_wants_fbs_area(port)) {
		rx_size = AHCI_RX_FIS_FBS_SIZE;
		rx_align = AHCI_RX_FIS_FBS_ALIGN;
	} else {
		rx_size = AHCI_RX_FIS_SIZE;
		rx_align = AHCI_RX_FIS_ALIGN;
	}

	rc = ahci_dma_alloc(hba, AHCI_CMD_LIST_SIZE, AHCI_CMD_LIST_ALIGN,
			    &port->cmd_list_area);
	if (rc)
		goto fail;
	rc = ahci_dma_alloc(hba, rx_size, rx_align, &port->rx_fis_area);
	if (rc)
		goto fail;
	rc = ahci_dma_alloc(hba, (size_t)nr_slots * AHCI_CMD_TABLE_SIZE,
			    AHCI_CMD_TABLE_ALIGN, &port->cmd_table_area);
	if (rc)
		goto fail;

	/* The alignment rules of the specification, checked on the bus
	 * addresses the HBA will actually be given. */
	BUG_ON(port->cmd_list_area.bus & (AHCI_CMD_LIST_ALIGN - 1));
	BUG_ON(port->rx_fis_area.bus & (rx_align - 1));
	BUG_ON(port->cmd_table_area.bus & (AHCI_CMD_TABLE_ALIGN - 1));

	port->cmd_list = (struct ahci_cmd_header *)port->cmd_list_area.virt;
	port->rx_fis = port->rx_fis_area.virt;
	tables = (struct ahci_cmd_table *)port->cmd_table_area.virt;

	for (uint32_t i = 0; i < AHCI_MAX_SLOTS; i++) {
		struct ahci_cmd *cmd = &port->cmds[i];

		cmd->slot = i;
		cmd->qc = NULL;
		cmd->state = AHCI_CMD_FREE;
		cmd->is_ncq = 0;
		cmd->internal = 0;
		cmd->n_prd = 0;
		cmd->nbytes = 0;
		kmemset(&cmd->bounce, 0, sizeof(cmd->bounce));
		if (i >= nr_slots) {
			cmd->hdr = NULL;
			cmd->table = NULL;
			cmd->table_bus = 0;
			continue;
		}
		cmd->hdr = &port->cmd_list[i];
		cmd->table = &tables[i];
		cmd->table_bus = port->cmd_table_area.bus +
				 (uint64_t)i * AHCI_CMD_TABLE_SIZE;
		cmd->hdr->opts = 0;
		cmd->hdr->prdbc = 0;
		cmd->hdr->ctba = (uint32_t)cmd->table_bus;
		cmd->hdr->ctbau = (uint32_t)(cmd->table_bus >> 32);
		for (int r = 0; r < 4; r++)
			cmd->hdr->rsvd[r] = 0;
	}
	if (!port->nr_slots)
		port->nr_slots = nr_slots;

	/* The headers are in memory before CLB is programmed. */
	if (ahci_dma_noncoherent) {
		ahci_clflush_range(port->cmd_list_area.virt, AHCI_CMD_LIST_SIZE);
		ahci_clflush_range(port->rx_fis_area.virt, rx_size);
	}
	ahci_wmb();
	return 0;

fail:
	kprintf("[ahci] %s: DMA structures: allocation failed (%d)\n",
		port->name[0] ? port->name : "port", rc);
	ahci_port_dma_free(port);
	return rc ? rc : -ENOMEM;
}

void ahci_port_dma_free(struct ahci_port *port)
{
	BUG_ON(port == NULL);
	for (uint32_t i = 0; i < AHCI_MAX_SLOTS; i++) {
		struct ahci_cmd *cmd = &port->cmds[i];

		/* Freeing the tables under a live command would let the HBA
		 * fetch from memory that is being reused. */
		BUG_ON(cmd->state != AHCI_CMD_FREE && cmd->qc != NULL);
		if (cmd->bounce.area.size)
			ahci_dma_free(&cmd->bounce.area);
		cmd->hdr = NULL;
		cmd->table = NULL;
		cmd->table_bus = 0;
	}
	ahci_dma_free(&port->cmd_table_area);
	ahci_dma_free(&port->rx_fis_area);
	ahci_dma_free(&port->cmd_list_area);
	port->cmd_list = NULL;
	port->rx_fis = NULL;
}

/* ==========================================================================
 * PRDT construction (pure apart from the translation callback)
 * ========================================================================== */

/* Append one physically contiguous piece [bus, bus+len) to the table,
 * merging with the previous entry when it continues it.  Returns the new
 * entry count or -E2BIG. */
static int ahci_prdt_append(struct ahci_prdt_entry *prdt, uint32_t max_prd,
			    int n, uint64_t *last_bus_end, uint32_t *last_len,
			    uint64_t bus, uint32_t len)
{
	while (len) {
		uint32_t room, piece;

		/* Continue the previous entry when contiguous and not full */
		if (n > 0 && *last_bus_end == bus &&
		    *last_len < AHCI_PRD_MAX_BYTES) {
			room = AHCI_PRD_MAX_BYTES - *last_len;
			piece = len < room ? len : room;
			*last_len += piece;
			prdt[n - 1].dbc = (prdt[n - 1].dbc & AHCI_PRD_I) |
					  ((*last_len - 1) & AHCI_PRD_DBC_MASK);
			*last_bus_end += piece;
			bus += piece;
			len -= piece;
			continue;
		}
		if ((uint32_t)n >= max_prd)
			return -E2BIG;
		piece = len < AHCI_PRD_MAX_BYTES ? len : AHCI_PRD_MAX_BYTES;
		prdt[n].dba = (uint32_t)bus;
		prdt[n].dbau = (uint32_t)(bus >> 32);
		prdt[n].rsvd = 0;
		prdt[n].dbc = (piece - 1) & AHCI_PRD_DBC_MASK;
		*last_len = piece;
		*last_bus_end = bus + piece;
		n++;
		bus += piece;
		len -= piece;
	}
	return n;
}

int ahci_prdt_build(struct ahci_prdt_entry *prdt, uint32_t max_prd,
		    const struct blk_sg_entry *sg, uint32_t nsg,
		    uint64_t (*xlate)(void *ctx, const void *virt), void *ctx,
		    uint32_t *nbytes)
{
	uint64_t last_bus_end = 0;
	uint32_t last_len = 0, total = 0;
	int n = 0;

	if (nbytes)
		*nbytes = 0;
	if (nsg == 0)
		return 0;
	if (!prdt || !sg || max_prd == 0)
		return -EINVAL;

	for (uint32_t i = 0; i < nsg; i++) {
		const struct blk_sg_entry *e = &sg[i];
		uint32_t left = e->len;
		uint64_t off = 0;

		if (left == 0)
			continue;
		/* Each piece the HBA transfers must start on a word and be
		 * a whole number of words. */
		if (left & (AHCI_PRD_ALIGN - 1))
			return -EINVAL;
		if (e->phys) {
			if (e->phys & (AHCI_PRD_ALIGN - 1))
				return -EINVAL;
		} else if ((uint64_t)e->virt & (AHCI_PRD_ALIGN - 1)) {
			return -EINVAL;
		}
		if (!e->phys && !e->virt)
			return -EFAULT;
		if ((uint64_t)total + left > 0xFFFFFFFFULL)
			return -E2BIG;

		while (left) {
			uint64_t bus, page_off;
			uint32_t chunk;

			if (e->phys && !xlate) {
				/* Physically contiguous by promise: no page
				 * walk, only the 4 MiB entry limit. */
				bus = ahci_dma_translate(NULL, e->phys + off);
				chunk = left;
			} else {
				const uint8_t *va = e->virt ?
							    (const uint8_t *)e->virt + off :
							    (const uint8_t *)phys_to_virt(e->phys + off);

				page_off = (uint64_t)va & (AHCI_PAGE_SIZE - 1);
				chunk = (uint32_t)(AHCI_PAGE_SIZE - page_off);
				if (chunk > left)
					chunk = left;
				bus = xlate ? xlate(ctx, va) :
					      ahci_dma_virt_to_bus(NULL, va);
			}
			if (bus == AHCI_DMA_BAD_ADDR)
				return -EFAULT;
			if (bus & (AHCI_PRD_ALIGN - 1))
				return -EINVAL;
			n = ahci_prdt_append(prdt, max_prd, n, &last_bus_end,
					     &last_len, bus, chunk);
			if (n < 0)
				return n;
			off += chunk;
			left -= chunk;
			total += chunk;
		}
	}
	if (nbytes)
		*nbytes = total;
	return n;
}

/* ==========================================================================
 * Mapping a command's data
 * ========================================================================== */

struct ahci_xlate_ctx {
	struct ahci_port *port;
};

static uint64_t ahci_dma_xlate_cb(void *ctx, const void *virt)
{
	struct ahci_xlate_ctx *c = ctx;

	return ahci_dma_virt_to_bus(c->port->hba, virt);
}

/* The kernel address of an sg element's byte `off' (for the bounce copy). */
static uint8_t *ahci_sg_ptr(const struct blk_sg_entry *e, uint32_t off)
{
	if (e->virt)
		return (uint8_t *)e->virt + off;
	if (e->phys)
		return (uint8_t *)phys_to_virt(e->phys + off);
	return NULL;
}

/* Write back (and invalidate) the data of an sg list -- the non-coherent
 * path only.  Virtual ranges are flushed through their own mapping. */
static void ahci_sg_flush(const struct blk_sg_entry *sg, uint32_t nsg)
{
	for (uint32_t i = 0; i < nsg; i++) {
		uint8_t *p = ahci_sg_ptr(&sg[i], 0);

		if (p && sg[i].len)
			ahci_clflush_range(p, sg[i].len);
	}
}

/* Do all entries of a built PRDT lie below 4 GiB? */
static int ahci_prdt_below_4g(const struct ahci_prdt_entry *prdt, int n)
{
	for (int i = 0; i < n; i++) {
		uint64_t a = ((uint64_t)prdt[i].dbau << 32) | prdt[i].dba;
		uint64_t len = (uint64_t)(prdt[i].dbc & AHCI_PRD_DBC_MASK) + 1;

		if (prdt[i].dbau != 0 || a + len > AHCI_4G)
			return 0;
	}
	return 1;
}

/* Run a command through a bounce area: allocate it (below 4 GiB unless the
 * HBA is 64-bit capable), copy the data in for a write, build a PRDT over
 * the area. */
static int ahci_dma_bounce(struct ahci_port *port, struct ahci_cmd *cmd,
			   const struct blk_sg_entry *sg, uint32_t nsg,
			   int write, uint32_t total)
{
	struct ahci_hba *hba = port->hba;
	struct blk_sg_entry one;
	uint32_t nb = 0, copied = 0;
	int rc, n;

	BUG_ON(cmd->bounce.area.size != 0); /* bounce of a live command */
	rc = ahci_dma_alloc(hba, total, AHCI_PRD_ALIGN, &cmd->bounce.area);
	if (rc) {
		__atomic_add_fetch(&g_ahci_bounce_fail, 1, __ATOMIC_RELAXED);
		WARN_RATELIMIT(1, "[ahci] %s: bounce buffer of %u bytes: %d",
			       port->name, total, rc);
		return rc;
	}
	if (write) {
		for (uint32_t i = 0; i < nsg; i++) {
			uint8_t *src = ahci_sg_ptr(&sg[i], 0);

			if (!sg[i].len)
				continue;
			if (!src) {
				ahci_dma_free(&cmd->bounce.area);
				return -EFAULT;
			}
			kmemcpy((uint8_t *)cmd->bounce.area.virt + copied, src,
				sg[i].len);
			copied += sg[i].len;
		}
	}
	one.virt = cmd->bounce.area.virt;
	one.phys = cmd->bounce.area.phys;
	one.len = total;
	one.flags = 0;
	n = ahci_prdt_build(cmd->table->prdt, AHCI_MAX_PRD, &one, 1, NULL,
			    NULL, &nb);
	if (n < 0) {
		ahci_dma_free(&cmd->bounce.area);
		return n;
	}
	if (!hba->dma64 && !ahci_prdt_below_4g(cmd->table->prdt, n)) {
		/* ahci_dma_alloc() promised low memory */
		WARN(1, "[ahci] %s: bounce area above 4 GiB", port->name);
		ahci_dma_free(&cmd->bounce.area);
		return -ENOMEM;
	}
	cmd->bounce.sg = sg;
	cmd->bounce.nsg = nsg;
	cmd->bounce.nbytes = total;
	cmd->n_prd = (uint32_t)n;
	cmd->nbytes = nb;
	port->stats.bounces++;
	__atomic_add_fetch(&g_ahci_bounce_total, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_ahci_bounce_live, 1, __ATOMIC_RELAXED);
	if (ahci_dma_noncoherent)
		ahci_clflush_range(cmd->bounce.area.virt, total);
	return 0;
}

int ahci_dma_map(struct ahci_port *port, struct ahci_cmd *cmd,
		 const struct blk_sg_entry *sg, uint32_t nsg, int write)
{
	struct ahci_xlate_ctx ctx;
	uint32_t nbytes = 0, total = 0;
	int n;

	BUG_ON(port == NULL || cmd == NULL);
	VM_BUG_ON(cmd->table == NULL);
	VM_BUG_ON(cmd->bounce.area.size != 0);

	cmd->n_prd = 0;
	cmd->nbytes = 0;
	if (nsg == 0 || sg == NULL)
		return 0;

	for (uint32_t i = 0; i < nsg; i++)
		total += sg[i].len;
	if (total == 0)
		return 0;
	/* An odd total can never be described by a PRDT, bounced or not. */
	if (total & (AHCI_PRD_ALIGN - 1)) {
		WARN_RATELIMIT(1, "[ahci] %s: odd transfer length %u",
			       port->name, total);
		return -EINVAL;
	}

	ctx.port = port;
	n = ahci_prdt_build(cmd->table->prdt, AHCI_MAX_PRD, sg, nsg,
			    ahci_dma_xlate_cb, &ctx, &nbytes);
	if (n >= 0 && (port->hba->dma64 ||
		       ahci_prdt_below_4g(cmd->table->prdt, n))) {
		cmd->n_prd = (uint32_t)n;
		cmd->nbytes = nbytes;
		if (ahci_dma_noncoherent)
			ahci_sg_flush(sg, nsg);
		return 0;
	}
	if (n == -EFAULT) {
		WARN_RATELIMIT(1, "[ahci] %s: unmapped data buffer", port->name);
		return -EFAULT;
	}
	/* -EINVAL (misaligned piece), -E2BIG (too fragmented), or memory a
	 * 32-bit HBA cannot reach: one contiguous low copy fixes all three. */
	return ahci_dma_bounce(port, cmd, sg, nsg, write, total);
}

void ahci_dma_unmap(struct ahci_port *port, struct ahci_cmd *cmd, int write)
{
	BUG_ON(port == NULL || cmd == NULL);

	if (cmd->bounce.area.size) {
		const struct blk_sg_entry *sg = cmd->bounce.sg;
		uint32_t off = 0;

		/* The device wrote the area; make sure no stale line of it
		 * is read on a non-snooping platform. */
		if (ahci_dma_noncoherent)
			ahci_clflush_range(cmd->bounce.area.virt,
					   cmd->bounce.nbytes);
		ahci_rmb();
		if (!write && sg) {
			for (uint32_t i = 0; i < cmd->bounce.nsg; i++) {
				uint8_t *dst = ahci_sg_ptr(&sg[i], 0);
				uint32_t len = sg[i].len;

				if (!len)
					continue;
				if (off + len > cmd->bounce.nbytes)
					len = cmd->bounce.nbytes - off;
				if (dst && len)
					kmemcpy(dst,
						(uint8_t *)cmd->bounce.area.virt + off,
						len);
				off += len;
				if (off >= cmd->bounce.nbytes)
					break;
			}
		}
		ahci_dma_free(&cmd->bounce.area);
		cmd->bounce.sg = NULL;
		cmd->bounce.nsg = 0;
		cmd->bounce.nbytes = 0;
		__atomic_sub_fetch(&g_ahci_bounce_live, 1, __ATOMIC_RELAXED);
		return;
	}
	if (!write && ahci_dma_noncoherent && cmd->qc && cmd->qc->sg)
		ahci_sg_flush(cmd->qc->sg, cmd->qc->n_elem);
}

/* ==========================================================================
 * Coherency helpers for the command engine
 * ========================================================================== */

void ahci_dma_sync_cmd_for_device(struct ahci_port *port, struct ahci_cmd *cmd)
{
	(void)port;
	if (unlikely(ahci_dma_noncoherent) && cmd && cmd->table && cmd->hdr) {
		size_t tlen = AHCI_CT_PRDT_OFF +
			      (size_t)cmd->n_prd * sizeof(struct ahci_prdt_entry);

		ahci_clflush_range(cmd->table, tlen);
		ahci_clflush_range(cmd->hdr, sizeof(*cmd->hdr));
	}
	ahci_wmb();
}

void ahci_dma_sync_rx_for_cpu(struct ahci_port *port)
{
	ahci_rmb();
	if (unlikely(ahci_dma_noncoherent) && port && port->rx_fis_area.virt)
		ahci_clflush_range(port->rx_fis_area.virt,
				   port->rx_fis_area.size);
}
