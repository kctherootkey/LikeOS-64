// LikeOS - NVMe over PCI Express: DMA pools, PRP and SGL mapping, metadata,
// bounce buffers, and the controller-side memories (CMB, PMR, HMB)
//
// Everything the controller reads or writes in system memory on behalf of
// a command goes through here.
//
//   * DESCRIPTOR POOLS: PRP lists and SGL segments come from two pools of
//     fixed-size blocks carved out of whole pages -- one page (512 PRP
//     entries / 256 SGL descriptors) for the large lists, 256 bytes for
//     the short ones (up to 32 PRP entries, 16 SGL descriptors, a
//     metadata SGL), the established split that keeps small I/O cheap.
//     They are allocated in any context: an empty pool grows by one page
//     from the page allocator, whose lock is interrupt safe.
//
//   * DATA MAPPING: a request's scatter/gather list holds kernel virtual
//     (or physical) addresses.  It is walked as physically contiguous
//     SEGMENTS -- direct-map ranges in one step, other mappings page by
//     page with neighbouring pages merged back when they are contiguous.
//     A single segment that fits in two pages goes into PRP1/PRP2 or one
//     SGL data block in the SQE; anything longer becomes a PRP list
//     (chained across list pages, the last entry of a full page pointing
//     to the next) or an SGL (data blocks in one last-segment list).  SGLs
//     are used when the controller has them and the average segment is at
//     least nvme.sgl_threshold bytes, and always for gaps PRPs cannot
//     describe, user commands (the controller then checks the length) and
//     metadata in several pieces.
//
//   * BOUNCE BUFFERS take what the controller cannot reach or describe: a
//     buffer above the DMA mask (32/48-bit controllers), a misaligned
//     PRP1, a gap on a controller without SGLs, more segments than one
//     SGL list holds.  The data is copied into one contiguous low area
//     before a write and out of it after a read.  The block layer's limits
//     (virt_boundary_mask, max_segments, max_hw_sectors from MDTS) make
//     this the rare path; the fallback makes every request mappable.
//
//   * METADATA: MPTR to one contiguous buffer, or -- data in SGLs, the
//     controller supporting metadata SGLs, and several segments or a user
//     command -- an SGL segment from the small pool (MPTR points at it).
//
//   * CMB: the Controller Memory Buffer is mapped write-combining and, when
//     it supports submission queues, carries the I/O SQs (a stack
//     allocator: queues are created bottom up and freed top down).
//   * PMR: the Persistent Memory Region is enabled (PMRCTL.EN, waiting for
//     PMRSTS.NRDY to clear within PMRTO) and mapped write-combining.
//   * HMB: the Host Memory Buffer is built from the largest chunks the page
//     allocator gives, halving the chunk size down to the controller's
//     minimum descriptor size, described by a descriptor list, handed over
//     with Set Features (Host Memory Buffer) and taken back (disabled)
//     before suspend; a reset disables it on the controller's side.
//
// DMA ownership: nothing a command's descriptors or bounce buffers point
// at is released before the completion was consumed (nvme_unmap_* from
// the completion path) or the controller was disabled (cancel after
// nvme_dev_disable).  Pools with blocks still out are never destroyed.
//
// The functions between the NVME_PCI_PURE markers depend on nothing but
// their arguments; the host tests compile them unchanged.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>

#define NVME_PCI_DMA_LOG NVME_LOG_PREFIX

/* Allocation statistics (diagnostics). */
static volatile uint64_t g_nvme_bounce_live;
static volatile uint64_t g_nvme_bounce_total;
static volatile uint64_t g_nvme_bounce_fail;

/* ==========================================================================
 * DMA pools
 * ========================================================================== */

/* One page of a pool (kept on pool->pages for the destruction). */
struct nvme_dma_pool_page {
	uint64_t phys;
	struct nvme_dma_pool_page *next;
};

static uint32_t nvme_pool_stride(const struct nvme_dma_pool *pool)
{
	uint32_t a = pool->align ? pool->align : 1;

	return (pool->size + a - 1) & ~(a - 1);
}

/* Is [phys, phys + len) reachable for the device and in the direct map? */
static bool nvme_dma_range_ok(uint64_t phys, uint64_t len, uint64_t mask)
{
	if (!phys || !len)
		return false;
	if (phys + len - 1 > mask)
		return false;
	return is_phys_in_direct_map(phys + len - 1);
}

/* Add one page of blocks.  Any context. */
static int nvme_dma_pool_grow(struct nvme_dma_pool *pool)
{
	struct nvme_dma_pool_page *pg;
	uint32_t stride = nvme_pool_stride(pool), n = 0;
	void *head = NULL, *tail = NULL;
	uint64_t phys, flags;
	uint8_t *base;

	pg = kalloc(sizeof(*pg));
	if (!pg)
		return -ENOMEM;
	phys = mm_allocate_contiguous_pages(1);
	if (!phys) {
		kfree(pg);
		return -ENOMEM;
	}
	if (!nvme_dma_range_ok(phys, PAGE_SIZE, pool->dma_mask)) {
		mm_free_contiguous_pages(phys, 1);
		kfree(pg);
		return -ENOMEM;
	}
	VM_BUG_ON(phys & (PAGE_SIZE - 1));
	base = phys_to_virt(phys);
	for (uint32_t off = 0; off + pool->size <= PAGE_SIZE; off += stride) {
		void *blk = base + off;

		*(void **)blk = NULL;
		if (tail)
			*(void **)tail = blk;
		else
			head = blk;
		tail = blk;
		n++;
	}
	if (WARN_ON(n == 0)) {
		mm_free_contiguous_pages(phys, 1);
		kfree(pg);
		return -EINVAL;
	}
	pg->phys = phys;
	spin_lock_irqsave(&pool->lock, &flags);
	*(void **)tail = pool->free_list;
	pool->free_list = head;
	pool->nr_free += n;
	pool->nr_total += n;
	pool->nr_pages++;
	pg->next = pool->pages;
	pool->pages = pg;
	spin_unlock_irqrestore(&pool->lock, flags);
	return 0;
}

int nvme_dma_pool_init(struct nvme_dma_pool *pool, const char *name,
		       uint32_t size, uint32_t align, uint64_t dma_mask,
		       uint32_t prealloc)
{
	if (!pool || !size || size > PAGE_SIZE || !align ||
	    (align & (align - 1)) || align > PAGE_SIZE)
		return -EINVAL;
	kmemset(pool, 0, sizeof(*pool));
	pool->name = name;
	pool->size = size < sizeof(void *) ? (uint32_t)sizeof(void *) : size;
	pool->align = align;
	pool->dma_mask = dma_mask ? dma_mask : DMA_BIT_MASK(32);
	spinlock_init(&pool->lock, name);
	pool->blocks_per_page = PAGE_SIZE / nvme_pool_stride(pool);
	while (pool->nr_free < prealloc) {
		int rc = nvme_dma_pool_grow(pool);

		if (rc) {
			nvme_dma_pool_destroy(pool);
			return rc;
		}
	}
	return 0;
}

void nvme_dma_pool_destroy(struct nvme_dma_pool *pool)
{
	struct nvme_dma_pool_page *pg, *next;
	uint64_t flags;

	if (!pool || !pool->name)
		return;
	spin_lock_irqsave(&pool->lock, &flags);
	if (pool->nr_free != pool->nr_total) {
		uint32_t out = pool->nr_total - pool->nr_free;

		spin_unlock_irqrestore(&pool->lock, flags);
		/* A block still out may be the target of a DMA the controller
		 * has not finished: keep every page (leak) rather than let
		 * the page allocator hand it to someone else. */
		WARN(1, NVME_PCI_DMA_LOG
		     "pool %s destroyed with %u block(s) in use, pages kept",
		     pool->name, out);
		return;
	}
	pg = pool->pages;
	pool->pages = NULL;
	pool->free_list = NULL;
	pool->nr_free = pool->nr_total = 0;
	pool->nr_pages = 0;
	spin_unlock_irqrestore(&pool->lock, flags);
	for (; pg; pg = next) {
		next = pg->next;
		mm_free_contiguous_pages(pg->phys, 1);
		kfree(pg);
	}
	pool->name = NULL;
}

void *nvme_dma_pool_alloc(struct nvme_dma_pool *pool, uint64_t *phys)
{
	uint64_t flags;
	void *blk;

	for (int attempt = 0; attempt < 2; attempt++) {
		spin_lock_irqsave(&pool->lock, &flags);
		blk = pool->free_list;
		if (blk) {
			pool->free_list = *(void **)blk;
			BUG_ON(pool->nr_free == 0);
			pool->nr_free--;
			spin_unlock_irqrestore(&pool->lock, flags);
			*(void **)blk = NULL;
			*phys = virt_to_phys(blk);
			VM_BUG_ON((*phys & (pool->align - 1)) != 0);
			return blk;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
		if (nvme_dma_pool_grow(pool))
			break;
	}
	__atomic_add_fetch(&pool->nr_alloc_fail, 1, __ATOMIC_RELAXED);
	*phys = 0;
	return NULL;
}

void nvme_dma_pool_free(struct nvme_dma_pool *pool, void *vaddr,
			uint64_t phys)
{
	uint64_t flags;

	if (!vaddr)
		return;
	VM_BUG_ON(virt_to_phys(vaddr) != phys);
	(void)phys;
	spin_lock_irqsave(&pool->lock, &flags);
	BUG_ON(pool->nr_free >= pool->nr_total); /* double free */
	*(void **)vaddr = pool->free_list;
	pool->free_list = vaddr;
	pool->nr_free++;
	spin_unlock_irqrestore(&pool->lock, flags);
}

int nvme_dma_pool_refill(struct nvme_dma_pool *pool, uint32_t min_free)
{
	while (READ_ONCE(pool->nr_free) < min_free) {
		int rc = nvme_dma_pool_grow(pool);

		if (rc)
			return rc;
	}
	return 0;
}

/* ==========================================================================
 * Coherent memory
 * ========================================================================== */

void *nvme_dma_alloc_coherent(struct nvme_dev *dev, size_t size,
			      size_t align, uint64_t *phys)
{
	size_t pages, align_pages;
	uint64_t p, mask;

	might_sleep();
	*phys = 0;
	if (!size)
		return NULL;
	if (!align)
		align = PAGE_SIZE;
	if (align & (align - 1))
		return NULL;
	pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	align_pages = align > PAGE_SIZE ? align / PAGE_SIZE : 1;
	mask = dev && dev->dma_mask ? dev->dma_mask : DMA_BIT_MASK(32);

	if (align_pages > 1)
		p = mm_allocate_contiguous_pages_aligned(pages, align_pages);
	else
		p = mm_allocate_contiguous_pages(pages);
	if (!p)
		return NULL;
	if (!nvme_dma_range_ok(p, (uint64_t)pages * PAGE_SIZE, mask) ||
	    (p & (align - 1))) {
		mm_free_contiguous_pages(p, pages);
		return NULL;
	}
	kmemset(phys_to_virt(p), 0, pages * PAGE_SIZE);
	*phys = p;
	return phys_to_virt(p);
}

void nvme_dma_free_coherent(struct nvme_dev *dev, size_t size, void *vaddr,
			    uint64_t phys)
{
	size_t pages;

	(void)dev;
	if (!vaddr || !size)
		return;
	VM_BUG_ON(virt_to_phys(vaddr) != phys);
	pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	mm_free_contiguous_pages(phys, pages);
}

/* Physically contiguous pages for a bounce buffer: any context (the page
 * allocator's lock is interrupt safe), not zeroed. */
static uint64_t nvme_dma_alloc_bounce(uint64_t mask, uint32_t len,
				      uint32_t *pages_out)
{
	uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t p;

	*pages_out = 0;
	if (!pages)
		return 0;
	p = mm_allocate_contiguous_pages(pages);
	if (!p)
		return 0;
	if (!nvme_dma_range_ok(p, (uint64_t)pages * PAGE_SIZE, mask)) {
		mm_free_contiguous_pages(p, pages);
		return 0;
	}
	*pages_out = pages;
	return p;
}

/* NVME_PCI_PURE_BEGIN(dma) -------------------------------------------------
 * Segment iteration, PRP and SGL construction, HMB chunking.  Pure: no
 * global state, no locking, allocation through callbacks. */

/* Longest segment the iterator produces (an SGL data block's length is
 * 32 bits; page aligned so PRP arithmetic is unaffected). */
#define NVME_SEG_MAX_LEN 0xfffff000u

/* Bus address of byte `off' of `e' and in *contig how many bytes from
 * there are physically contiguous (> 0, <= e->len - off), or
 * NVME_DMA_BAD_ADDR. */
typedef uint64_t (*nvme_seg_xlate_fn)(void *ctx, const struct blk_sg_entry *e,
				      uint32_t off, uint32_t *contig);

/* Walks a scatter/gather list as maximal physically contiguous segments
 * (merging across elements), producing at most `limit' bytes. */
struct nvme_seg_iter {
	const struct blk_sg_entry *sg;
	uint32_t nsg;
	uint32_t idx; /* element of the next unconsumed byte */
	uint32_t off; /* its offset in that element */
	uint64_t limit; /* bytes still to produce */
	nvme_seg_xlate_fn xlate;
	void *ctx;
	uint64_t addr; /* the current segment (len 0: none) */
	uint32_t len;
	blk_status_t status;
};

static void nvme_seg_iter_init(struct nvme_seg_iter *it,
			       const struct blk_sg_entry *sg, uint32_t nsg,
			       uint64_t limit, nvme_seg_xlate_fn xlate,
			       void *ctx)
{
	it->sg = sg;
	it->nsg = sg ? nsg : 0;
	it->idx = 0;
	it->off = 0;
	it->limit = limit;
	it->xlate = xlate;
	it->ctx = ctx;
	it->addr = 0;
	it->len = 0;
	it->status = BLK_STS_OK;
}

/* Produce the next segment: true with it->addr/len set, false at the end
 * (status OK) or on a translation failure (status IOERR). */
static bool nvme_seg_iter_next(struct nvme_seg_iter *it)
{
	it->addr = 0;
	it->len = 0;
	if (it->status != BLK_STS_OK)
		return false;
	while (it->limit && it->idx < it->nsg) {
		const struct blk_sg_entry *e = &it->sg[it->idx];
		uint32_t contig = 0, piece;
		uint64_t bus;

		if (it->off >= e->len) {
			it->idx++;
			it->off = 0;
			continue;
		}
		bus = it->xlate(it->ctx, e, it->off, &contig);
		if (bus == NVME_DMA_BAD_ADDR || contig == 0 ||
		    contig > e->len - it->off) {
			it->status = BLK_STS_IOERR;
			it->len = 0;
			return false;
		}
		piece = contig;
		if (piece > it->limit)
			piece = (uint32_t)it->limit;
		if (it->len) {
			/* Merge only what continues the segment exactly. */
			if (bus != it->addr + it->len)
				break;
			if ((uint64_t)it->len + piece > NVME_SEG_MAX_LEN)
				piece = NVME_SEG_MAX_LEN - it->len;
			if (!piece)
				break;
		} else {
			it->addr = bus;
			if (piece > NVME_SEG_MAX_LEN)
				piece = NVME_SEG_MAX_LEN;
		}
		it->len += piece;
		it->off += piece;
		it->limit -= piece;
	}
	return it->len != 0;
}

/* The current segment if any bytes of it are left, else the next one. */
static bool nvme_seg_iter_advance(struct nvme_seg_iter *it)
{
	if (it->len)
		return true;
	return nvme_seg_iter_next(it);
}

/* What a first pass over the segments found. */
struct nvme_seg_scan {
	uint32_t nseg;
	uint64_t total;
	uint64_t first_addr;
	uint32_t first_len;
	bool prp_gap; /* an inner boundary not on a page boundary */
	bool addr_unaligned; /* a segment address not dword aligned */
	bool len_unaligned; /* a segment length not a dword multiple */
	bool beyond_mask; /* a segment the device cannot address */
	blk_status_t status;
};

static void nvme_seg_scan(struct nvme_seg_iter *it, uint64_t dma_mask,
			  struct nvme_seg_scan *s)
{
	uint64_t prev_end = 0;

	s->nseg = 0;
	s->total = 0;
	s->first_addr = 0;
	s->first_len = 0;
	s->prp_gap = false;
	s->addr_unaligned = false;
	s->len_unaligned = false;
	s->beyond_mask = false;
	while (nvme_seg_iter_next(it)) {
		if (s->nseg) {
			if ((it->addr & (NVME_CTRL_PAGE_SIZE - 1)) ||
			    (prev_end & (NVME_CTRL_PAGE_SIZE - 1)))
				s->prp_gap = true;
		} else {
			s->first_addr = it->addr;
			s->first_len = it->len;
		}
		if (it->addr & 3)
			s->addr_unaligned = true;
		if (it->len & 3)
			s->len_unaligned = true;
		if (it->addr > dma_mask || it->len - 1 > dma_mask - it->addr)
			s->beyond_mask = true;
		prev_end = it->addr + it->len;
		s->nseg++;
		s->total += it->len;
	}
	s->status = it->status;
}

/* Descriptor block allocation: a PRP list / SGL segment (small: from the
 * 256-byte pool), its bus address in *dma; NULL when out of memory. */
typedef void *(*nvme_desc_alloc_fn)(void *ctx, bool small, uint64_t *dma);

/* What the builders produce: the data pointer and the descriptor blocks
 * (desc[0] from the small pool when `small'). */
struct nvme_dptr_out {
	uint64_t prp1, prp2;
	uint32_t total_len;
	uint8_t nr_desc;
	bool small;
	void *desc[NVME_MAX_NR_DESCRIPTORS];
	uint64_t desc_dma[NVME_MAX_NR_DESCRIPTORS];
};

static void nvme_dptr_out_init(struct nvme_dptr_out *o)
{
	o->prp1 = 0;
	o->prp2 = 0;
	o->total_len = 0;
	o->nr_desc = 0;
	o->small = false;
	for (int i = 0; i < NVME_MAX_NR_DESCRIPTORS; i++) {
		o->desc[i] = NULL;
		o->desc_dma[i] = 0;
	}
}

/* PRPs for `length' bytes from a fresh iterator: PRP1 the first (possibly
 * unaligned) piece up to its page end, PRP2 the second page or a PRP
 * list; a full list page hands its last slot over to a pointer to the
 * next page, whose first entry takes the moved one.  On failure the
 * descriptors allocated so far are in `o' for the caller to free. */
static blk_status_t nvme_build_prps(struct nvme_seg_iter *it, uint32_t length,
				    nvme_desc_alloc_fn alloc, void *ctx,
				    struct nvme_dptr_out *o)
{
	uint32_t prp_len, i, cap;
	uint64_t prp_dma;
	__le64 *prp_list;

	nvme_dptr_out_init(o);
	if (!length)
		return BLK_STS_OK;
	if (!nvme_seg_iter_next(it))
		return it->status ? it->status : BLK_STS_IOERR;

	/* PRP1: the only entry that may start inside a page. */
	o->prp1 = it->addr;
	prp_len = NVME_CTRL_PAGE_SIZE -
		  (uint32_t)(it->addr & (NVME_CTRL_PAGE_SIZE - 1));
	if (prp_len > length)
		prp_len = length;
	if (WARN_ON_ONCE(it->len < prp_len))
		return BLK_STS_IOERR; /* a gap inside the first page */
	o->total_len += prp_len;
	it->addr += prp_len;
	it->len -= prp_len;
	length -= prp_len;
	if (!length)
		return BLK_STS_OK;

	if (!nvme_seg_iter_advance(it))
		return it->status ? it->status : BLK_STS_IOERR;
	if (WARN_ON_ONCE(it->addr & (NVME_CTRL_PAGE_SIZE - 1)))
		return BLK_STS_IOERR;

	/* PRP2 points at data when the rest fits in one page. */
	if (length <= NVME_CTRL_PAGE_SIZE) {
		if (WARN_ON_ONCE(it->len < length))
			return BLK_STS_IOERR;
		o->prp2 = it->addr;
		o->total_len += length;
		return BLK_STS_OK;
	}

	o->small = (length + NVME_CTRL_PAGE_SIZE - 1) / NVME_CTRL_PAGE_SIZE <=
		   NVME_SMALL_POOL_SIZE / sizeof(__le64);
	prp_list = alloc(ctx, o->small, &prp_dma);
	if (!prp_list)
		return BLK_STS_RESOURCE;
	o->desc[o->nr_desc] = prp_list;
	o->desc_dma[o->nr_desc++] = prp_dma;
	o->prp2 = prp_dma;
	cap = o->small ? NVME_SMALL_POOL_SIZE / sizeof(__le64) :
			 NVME_CTRL_PAGE_SIZE / sizeof(__le64);

	i = 0;
	for (;;) {
		prp_list[i++] = cpu_to_le64(it->addr);
		prp_len = length < NVME_CTRL_PAGE_SIZE ? length :
							 NVME_CTRL_PAGE_SIZE;
		if (WARN_ON_ONCE(it->len < prp_len))
			return BLK_STS_IOERR;
		o->total_len += prp_len;
		it->addr += prp_len;
		it->len -= prp_len;
		length -= prp_len;
		if (!length)
			break;
		if (!nvme_seg_iter_advance(it))
			return it->status ? it->status : BLK_STS_IOERR;
		if (WARN_ON_ONCE(it->addr & (NVME_CTRL_PAGE_SIZE - 1)))
			return BLK_STS_IOERR;
		if (i == cap) {
			__le64 *old = prp_list;
			uint64_t new_dma;

			/* A short list never fills (sized for the length). */
			if (WARN_ON_ONCE(o->small))
				return BLK_STS_IOERR;
			if (WARN_ON_ONCE(o->nr_desc == NVME_MAX_NR_DESCRIPTORS))
				return BLK_STS_IOERR;
			prp_list = alloc(ctx, false, &new_dma);
			if (!prp_list)
				return BLK_STS_RESOURCE;
			o->desc[o->nr_desc] = prp_list;
			o->desc_dma[o->nr_desc++] = new_dma;
			prp_list[0] = old[i - 1];
			old[i - 1] = cpu_to_le64(new_dma);
			i = 1;
		}
	}
	return BLK_STS_OK;
}

static void nvme_sgl_set_data(struct nvme_sgl_desc *sge, uint64_t addr,
			      uint32_t len)
{
	sge->addr = cpu_to_le64(addr);
	sge->length = cpu_to_le32(len);
	sge->rsvd[0] = sge->rsvd[1] = sge->rsvd[2] = 0;
	sge->type = NVME_SGL_FMT_DATA_DESC << 4;
}

static void nvme_sgl_set_seg(struct nvme_sgl_desc *sge, uint64_t dma_addr,
			     uint32_t entries)
{
	sge->addr = cpu_to_le64(dma_addr);
	sge->length = cpu_to_le32(entries * (uint32_t)sizeof(*sge));
	sge->rsvd[0] = sge->rsvd[1] = sge->rsvd[2] = 0;
	sge->type = NVME_SGL_FMT_LAST_SEG_DESC << 4;
}

/* SGL for the data of a fresh iterator with `nseg' segments: one data
 * block in the SQE, or a last-segment descriptor in the SQE pointing to a
 * list of data blocks. */
static blk_status_t nvme_build_sgl(struct nvme_seg_iter *it, uint32_t nseg,
				   nvme_desc_alloc_fn alloc, void *ctx,
				   struct nvme_sgl_desc *dptr,
				   struct nvme_dptr_out *o)
{
	struct nvme_sgl_desc *sg_list;
	uint32_t mapped = 0;
	uint64_t sgl_dma;

	nvme_dptr_out_init(o);
	if (!nvme_seg_iter_next(it))
		return it->status ? it->status : BLK_STS_IOERR;
	if (nseg == 1) {
		nvme_sgl_set_data(dptr, it->addr, it->len);
		o->total_len = it->len;
		return BLK_STS_OK;
	}
	if (WARN_ON_ONCE(nseg > NVME_MAX_SEGS || nseg == 0))
		return BLK_STS_IOERR;
	o->small = nseg <= NVME_SMALL_POOL_SIZE / sizeof(*sg_list);
	sg_list = alloc(ctx, o->small, &sgl_dma);
	if (!sg_list)
		return BLK_STS_RESOURCE;
	o->desc[o->nr_desc] = sg_list;
	o->desc_dma[o->nr_desc++] = sgl_dma;
	do {
		if (WARN_ON_ONCE(mapped == nseg))
			return BLK_STS_IOERR;
		nvme_sgl_set_data(&sg_list[mapped++], it->addr, it->len);
		o->total_len += it->len;
	} while (nvme_seg_iter_next(it));
	if (it->status)
		return it->status;
	nvme_sgl_set_seg(dptr, sgl_dma, mapped);
	return BLK_STS_OK;
}

/* Metadata SGL in one small block: entry 0 is the last-segment descriptor
 * MPTR points at when there are several segments (the data blocks follow
 * it), or the single data block itself. */
static blk_status_t nvme_build_meta_sgl(struct nvme_seg_iter *it,
					uint32_t nseg,
					nvme_desc_alloc_fn alloc, void *ctx,
					struct nvme_sgl_desc **list_out,
					uint64_t *dma_out, uint32_t *total)
{
	struct nvme_sgl_desc *sg_list;
	uint64_t sgl_dma;
	uint32_t i = 0;

	*list_out = NULL;
	*dma_out = 0;
	*total = 0;
	if (!nvme_seg_iter_next(it))
		return it->status ? it->status : BLK_STS_IOERR;
	sg_list = alloc(ctx, true, &sgl_dma);
	if (!sg_list)
		return BLK_STS_RESOURCE;
	*list_out = sg_list;
	*dma_out = sgl_dma;
	if (nseg == 1) {
		nvme_sgl_set_data(sg_list, it->addr, it->len);
		*total = it->len;
		return BLK_STS_OK;
	}
	do {
		if (WARN_ON_ONCE(i == NVME_MAX_META_SEGS))
			return BLK_STS_IOERR;
		nvme_sgl_set_data(&sg_list[++i], it->addr, it->len);
		*total += it->len;
	} while (nvme_seg_iter_next(it));
	if (it->status)
		return it->status;
	nvme_sgl_set_seg(sg_list, sgl_dma + sizeof(*sg_list), i);
	return BLK_STS_OK;
}

/* HMB construction.  The operations allocate one chunk (bus address in
 * *dma, NULL on failure), free one, and allocate / free the descriptor
 * list and the bookkeeping arrays. */
struct nvme_hmb_ops {
	void *(*alloc_chunk)(void *ctx, uint64_t len, uint64_t *dma);
	void (*free_chunk)(void *ctx, void *buf, uint64_t dma, uint64_t len);
	void *(*alloc_descs)(void *ctx, uint32_t size, uint64_t *dma);
	void (*free_descs)(void *ctx, void *descs, uint64_t dma,
			   uint32_t size);
	void *(*alloc_array)(void *ctx, size_t bytes);
	void (*free_array)(void *ctx, void *array);
};

struct nvme_hmb_result {
	struct nvme_host_mem_buf_desc *descs;
	uint64_t descs_dma;
	uint32_t descs_size;
	uint32_t nr_descs;
	uint64_t size;
	void **bufs;
	uint64_t *phys;
	uint64_t *lens;
	uint32_t chunk_size;
};

static void nvme_hmb_release(const struct nvme_hmb_ops *ops, void *ctx,
			     struct nvme_hmb_result *r)
{
	for (uint32_t i = 0; r->bufs && i < r->nr_descs; i++)
		ops->free_chunk(ctx, r->bufs[i], r->phys[i], r->lens[i]);
	if (r->bufs)
		ops->free_array(ctx, r->bufs);
	if (r->phys)
		ops->free_array(ctx, r->phys);
	if (r->lens)
		ops->free_array(ctx, r->lens);
	if (r->descs)
		ops->free_descs(ctx, r->descs, r->descs_dma, r->descs_size);
	r->descs = NULL;
	r->bufs = NULL;
	r->phys = NULL;
	r->lens = NULL;
	r->nr_descs = 0;
	r->size = 0;
	r->descs_size = 0;
}

/* Chunks of `chunk_size' (the last one shorter) up to `preferred' bytes,
 * at most hmmaxd descriptors (0: no limit).  0 when anything was
 * allocated, -ENOMEM when nothing. */
static int nvme_hmb_alloc_multi(const struct nvme_hmb_ops *ops, void *ctx,
				uint64_t preferred, uint32_t chunk_size,
				uint32_t hmmaxd, struct nvme_hmb_result *r)
{
	uint32_t max_entries, len, i = 0;
	uint64_t size, tmp;

	r->descs = NULL;
	r->bufs = NULL;
	r->phys = NULL;
	r->lens = NULL;
	r->nr_descs = 0;
	r->size = 0;
	r->descs_size = 0;
	if (!chunk_size || !preferred)
		return -ENOMEM;
	tmp = (preferred + chunk_size - 1) / chunk_size;
	max_entries = (uint32_t)tmp;
	if (hmmaxd && hmmaxd < max_entries)
		max_entries = hmmaxd;

	r->descs_size = max_entries * (uint32_t)sizeof(*r->descs);
	r->descs = ops->alloc_descs(ctx, r->descs_size, &r->descs_dma);
	if (!r->descs)
		goto fail;
	r->bufs = ops->alloc_array(ctx, max_entries * sizeof(void *));
	r->phys = ops->alloc_array(ctx, max_entries * sizeof(uint64_t));
	r->lens = ops->alloc_array(ctx, max_entries * sizeof(uint64_t));
	if (!r->bufs || !r->phys || !r->lens)
		goto fail;

	for (size = 0; size < preferred && i < max_entries; size += len) {
		uint64_t dma;

		len = (uint32_t)((preferred - size) < chunk_size ?
					 (preferred - size) :
					 chunk_size);
		r->bufs[i] = ops->alloc_chunk(ctx, len, &dma);
		if (!r->bufs[i])
			break;
		r->phys[i] = dma;
		r->lens[i] = len;
		r->descs[i].addr = cpu_to_le64(dma);
		r->descs[i].size = cpu_to_le32(len / NVME_CTRL_PAGE_SIZE);
		r->descs[i].rsvd = 0;
		i++;
		r->nr_descs = i;
	}
	if (!size)
		goto fail;
	r->size = size;
	r->chunk_size = chunk_size;
	return 0;
fail:
	nvme_hmb_release(ops, ctx, r);
	return -ENOMEM;
}

/* Start with the largest chunk and halve it down to the controller's
 * minimum descriptor size (at least two pages) until at least `min'
 * bytes (0: anything) were obtained. */
static int nvme_hmb_alloc(const struct nvme_hmb_ops *ops, void *ctx,
			  uint64_t min, uint64_t preferred, uint32_t hmminds,
			  uint32_t hmmaxd, struct nvme_hmb_result *r)
{
	uint64_t min_chunk = preferred < NVME_HMB_MAX_CHUNK ?
				     preferred :
				     NVME_HMB_MAX_CHUNK;
	uint64_t floor = (uint64_t)hmminds * 4096u;
	uint64_t chunk_size;

	if (floor < 2u * NVME_CTRL_PAGE_SIZE)
		floor = 2u * NVME_CTRL_PAGE_SIZE;
	for (chunk_size = min_chunk; chunk_size >= floor; chunk_size /= 2) {
		if (!nvme_hmb_alloc_multi(ops, ctx, preferred,
					  (uint32_t)chunk_size, hmmaxd, r)) {
			if (!min || r->size >= min)
				return 0;
			nvme_hmb_release(ops, ctx, r);
		}
	}
	return -ENOMEM;
}

/* NVME_PCI_PURE_END(dma) ------------------------------------------------- */

/* ==========================================================================
 * Translation and bounce buffers
 * ========================================================================== */

/* Kernel virtual / physical sg element -> bus address (no IOMMU: the
 * physical address).  Direct-map ranges are contiguous as a whole; any
 * other mapping is translated page by page. */
static uint64_t nvme_pci_xlate(void *ctx, const struct blk_sg_entry *e,
			       uint32_t off, uint32_t *contig)
{
	uint32_t left = e->len - off;
	uint64_t va, phys, page_left;

	(void)ctx;
	if (e->phys) {
		*contig = left;
		return e->phys + off;
	}
	if (!e->virt)
		return NVME_DMA_BAD_ADDR;
	va = (uint64_t)e->virt + off;
	if (is_direct_map_addr(va) && is_direct_map_addr(va + left - 1)) {
		*contig = left;
		return va - PHYS_MAP_BASE;
	}
	phys = mm_get_physical_address(va);
	if (!phys)
		return NVME_DMA_BAD_ADDR;
	page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
	*contig = left < page_left ? left : (uint32_t)page_left;
	return phys;
}

static uint8_t *nvme_sg_ptr(const struct blk_sg_entry *e)
{
	if (e->virt)
		return (uint8_t *)e->virt;
	if (e->phys)
		return (uint8_t *)phys_to_virt(e->phys);
	return NULL;
}

/* Copy between an sg list and a linear buffer (up to len bytes).
 * Returns the bytes copied. */
static uint32_t nvme_sg_copy(const struct blk_sg_entry *sg, uint32_t nsg,
			     uint8_t *buf, uint32_t len, bool to_buf)
{
	uint32_t done = 0;

	for (uint32_t i = 0; sg && i < nsg && done < len; i++) {
		uint8_t *p = nvme_sg_ptr(&sg[i]);
		uint32_t n = sg[i].len;

		if (!n)
			continue;
		if (!p)
			break;
		if (n > len - done)
			n = len - done;
		if (to_buf)
			kmemcpy(buf + done, p, n);
		else
			kmemcpy(p, buf + done, n);
		done += n;
	}
	return done;
}

struct nvme_pci_desc_ctx {
	struct nvme_descriptor_pools *pools;
};

static void *nvme_pci_desc_alloc(void *ctx, bool small, uint64_t *dma)
{
	struct nvme_pci_desc_ctx *c = ctx;

	return nvme_dma_pool_alloc(small ? &c->pools->small :
					   &c->pools->large,
				   dma);
}

static struct nvme_descriptor_pools *nvme_rq_pools(struct nvme_dev *dev,
						   struct blk_request *rq)
{
	struct nvme_queue *nvmeq = rq->mq_hctx ? rq->mq_hctx->driver_data :
						 NULL;

	if (nvmeq && nvmeq->descriptor_pools)
		return nvmeq->descriptor_pools;
	return &dev->descriptor_pools;
}

static uint16_t nvme_rq_queue_id(struct blk_request *rq)
{
	struct nvme_queue *nvmeq = rq->mq_hctx ? rq->mq_hctx->driver_data :
						 NULL;

	return nvmeq ? nvmeq->qid : 0;
}

/* Free the descriptor blocks recorded in the iod. */
static void nvme_free_descriptors(struct nvme_descriptor_pools *pools,
				  struct nvme_iod *iod)
{
	for (unsigned int i = 0; i < iod->nr_descriptors; i++) {
		struct nvme_dma_pool *pool =
			(i == 0 && (iod->flags & IOD_SMALL_DESCRIPTOR)) ?
				&pools->small :
				&pools->large;

		nvme_dma_pool_free(pool, iod->descriptors[i],
				   iod->descriptor_dma[i]);
		iod->descriptors[i] = NULL;
		iod->descriptor_dma[i] = 0;
	}
	iod->nr_descriptors = 0;
}

static void nvme_free_dptr_out(struct nvme_descriptor_pools *pools,
			       struct nvme_dptr_out *o)
{
	for (unsigned int i = 0; i < o->nr_desc; i++)
		nvme_dma_pool_free((i == 0 && o->small) ? &pools->small :
							  &pools->large,
				   o->desc[i], o->desc_dma[i]);
	o->nr_desc = 0;
}

static void nvme_iod_take_desc(struct nvme_iod *iod,
			       const struct nvme_dptr_out *o)
{
	BUG_ON(o->nr_desc > NVME_MAX_NR_DESCRIPTORS);
	for (unsigned int i = 0; i < o->nr_desc; i++) {
		iod->descriptors[i] = o->desc[i];
		iod->descriptor_dma[i] = o->desc_dma[i];
	}
	iod->nr_descriptors = o->nr_desc;
	if (o->small)
		iod->flags |= IOD_SMALL_DESCRIPTOR;
}

/* ==========================================================================
 * PRP or SGL: the decision
 * ========================================================================== */

enum nvme_use_sgl {
	SGL_UNSUPPORTED,
	SGL_SUPPORTED,
	SGL_FORCED,
};

static enum nvme_use_sgl nvme_pci_sgl_mode(struct nvme_dev *dev,
					   struct blk_request *rq,
					   const struct nvme_seg_scan *s)
{
	if (nvme_rq_queue_id(rq) && nvme_ctrl_sgl_supported(&dev->ctrl)) {
		/* A gap PRPs cannot describe, a user command (the
		 * controller checks the length of an SGL), metadata in
		 * several pieces: SGL is the only right form. */
		if (s->prp_gap || (nvme_req(rq)->flags & NVME_REQ_USERCMD) ||
		    rq->nr_integrity_segs > 1)
			return SGL_FORCED;
		return SGL_SUPPORTED;
	}
	return SGL_UNSUPPORTED;
}

static bool nvme_pci_sgl_capable(struct nvme_dev *dev,
				 enum nvme_use_sgl mode,
				 const struct nvme_seg_scan *s)
{
	if (mode == SGL_UNSUPPORTED || s->nseg > NVME_MAX_SEGS)
		return false;
	if (nvme_pci_ctrl_sgl_dword_only(&dev->ctrl) &&
	    (s->addr_unaligned || s->len_unaligned))
		return false;
	return true;
}

static bool nvme_pci_prp_capable(const struct nvme_seg_scan *s)
{
	return !s->prp_gap && !(s->first_addr & 3);
}

/* Does the average segment reach the threshold? */
static bool nvme_pci_sgl_by_threshold(const struct nvme_seg_scan *s)
{
	unsigned int thr = nvme_pci_param.sgl_threshold;
	uint64_t avg;

	if (!thr || !s->nseg)
		return false;
	avg = (s->total + s->nseg - 1) / s->nseg;
	return avg >= thr;
}

static void nvme_rq_iter_init(struct nvme_seg_iter *it, struct blk_request *rq,
			      uint32_t length)
{
	nvme_seg_iter_init(it, rq->sg, rq->nsg, length, nvme_pci_xlate, NULL);
}

bool nvme_pci_use_sgls(struct nvme_dev *dev, struct blk_request *rq,
		       int nseg)
{
	struct nvme_seg_iter it;
	struct nvme_seg_scan s;
	enum nvme_use_sgl mode;

	(void)nseg;
	nvme_rq_iter_init(&it, rq, (uint32_t)blk_rq_payload_bytes(rq));
	nvme_seg_scan(&it, dev->dma_mask, &s);
	if (s.status || !s.nseg)
		return false;
	mode = nvme_pci_sgl_mode(dev, rq, &s);
	if (!nvme_pci_sgl_capable(dev, mode, &s))
		return false;
	if (mode == SGL_FORCED)
		return true;
	if (nvme_pci_sgl_by_threshold(&s))
		return true;
	return !nvme_pci_prp_capable(&s);
}

/* ==========================================================================
 * Data mapping
 * ========================================================================== */

static void nvme_cmd_set_sgl(struct nvme_command *cmnd)
{
	cmnd->common.flags = (uint8_t)((cmnd->common.flags &
					~NVME_CMD_PSDT_MASK) |
				       NVME_CMD_SGL_METABUF);
}

/* Move the data through one contiguous buffer below the DMA mask; the sg
 * list is then described by a single entry for it. */
static blk_status_t nvme_pci_bounce_data(struct nvme_dev *dev,
					 struct blk_request *rq,
					 struct nvme_iod *iod,
					 const struct blk_sg_entry *sg,
					 uint32_t nsg, uint32_t length)
{
	uint64_t phys;
	uint32_t pages;

	VM_BUG_ON(iod->flags & IOD_BOUNCED);
	phys = nvme_dma_alloc_bounce(dev->dma_mask, length, &pages);
	if (!phys) {
		__atomic_add_fetch(&g_nvme_bounce_fail, 1, __ATOMIC_RELAXED);
		nvme_pci_warn_ratelimited(NVME_PCI_DMA_LOG
					  "%s: no bounce buffer for %u bytes\n",
					  dev->ctrl.name, length);
		return BLK_STS_RESOURCE;
	}
	iod->bounce = phys_to_virt(phys);
	iod->bounce_phys = phys;
	iod->bounce_len = length;
	iod->bounce_pages = pages;
	iod->flags |= IOD_BOUNCED;
	if (rq_data_dir(rq))
		nvme_sg_copy(sg, nsg, iod->bounce, length, true);
	__atomic_add_fetch(&g_nvme_bounce_total, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_nvme_bounce_live, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&dev->nr_bounces, 1, __ATOMIC_RELAXED);
	return BLK_STS_OK;
}

static void nvme_pci_unbounce_data(struct nvme_iod *iod,
				   struct blk_request *rq)
{
	if (!(iod->flags & IOD_BOUNCED))
		return;
	dma_rmb();
	/* A special payload (DSM range list) only goes to the device.  A
	 * read that failed, was cancelled or never reached the device left
	 * the bounce pages as the allocator handed them out: nothing of
	 * them goes back. */
	if (!rq_data_dir(rq) && iod->data_valid &&
	    !(nvme_req(rq)->flags & NVME_REQ_SPECIAL_PAYLOAD))
		nvme_sg_copy(rq->sg, rq->nsg, iod->bounce, iod->bounce_len,
			     false);
	mm_free_contiguous_pages(iod->bounce_phys, iod->bounce_pages);
	iod->bounce = NULL;
	iod->bounce_phys = 0;
	iod->bounce_len = 0;
	iod->bounce_pages = 0;
	iod->flags &= (uint8_t)~IOD_BOUNCED;
	__atomic_sub_fetch(&g_nvme_bounce_live, 1, __ATOMIC_RELAXED);
}

/* One segment: PRP1(/PRP2) or a single SGL data block in the SQE.
 * BLK_STS_AGAIN when neither fits (the list path takes it). */
static blk_status_t nvme_pci_setup_data_simple(struct nvme_dev *dev,
					       struct nvme_iod *iod,
					       struct nvme_command *cmnd,
					       enum nvme_use_sgl mode,
					       bool sgl_capable,
					       const struct nvme_seg_scan *s,
					       uint32_t length)
{
	uint32_t prp1_offset = (uint32_t)(s->first_addr &
					   (NVME_CTRL_PAGE_SIZE - 1));
	bool prp_possible = prp1_offset + length <= NVME_CTRL_PAGE_SIZE * 2 &&
			    !(s->first_addr & 3);

	(void)dev;
	if (!prp_possible && !sgl_capable)
		return BLK_STS_AGAIN;
	if (s->first_len < length)
		return BLK_STS_AGAIN;
	iod->total_len = length;
	iod->flags |= IOD_SINGLE_SEGMENT;
	iod->first_dma = s->first_addr;
	if (sgl_capable && (mode == SGL_FORCED || !prp_possible)) {
		nvme_cmd_set_sgl(cmnd);
		nvme_sgl_set_data(&cmnd->common.dptr.sgl, s->first_addr,
				  length);
	} else {
		uint32_t first_prp_len = NVME_CTRL_PAGE_SIZE - prp1_offset;

		cmnd->common.dptr.prp1 = cpu_to_le64(s->first_addr);
		cmnd->common.dptr.prp2 = 0;
		if (length > first_prp_len)
			cmnd->common.dptr.prp2 =
				cpu_to_le64(s->first_addr + first_prp_len);
	}
	return BLK_STS_OK;
}

static blk_status_t nvme_pci_map_segments(struct nvme_dev *dev,
					  struct blk_request *rq,
					  struct nvme_iod *iod,
					  struct nvme_command *cmnd,
					  const struct blk_sg_entry *sg,
					  uint32_t nsg, uint32_t length,
					  bool allow_bounce)
{
	struct nvme_pci_desc_ctx dctx = { .pools = nvme_rq_pools(dev, rq) };
	struct nvme_seg_iter it;
	struct nvme_seg_scan s;
	struct nvme_dptr_out o;
	enum nvme_use_sgl mode;
	bool sgl_capable, prp_capable, want_sgl;
	blk_status_t ret;

	nvme_seg_iter_init(&it, sg, nsg, length, nvme_pci_xlate, NULL);
	nvme_seg_scan(&it, dev->dma_mask, &s);
	if (s.status) {
		WARN_RATELIMIT(1,
			       NVME_PCI_DMA_LOG
			       "%s: unmapped data buffer (%u bytes)",
			       dev->ctrl.name, length);
		return BLK_STS_IOERR;
	}
	if (s.total < length) {
		WARN_RATELIMIT(1,
			       NVME_PCI_DMA_LOG
			       "%s: data buffer of %llu bytes for a %u byte transfer",
			       dev->ctrl.name, (unsigned long long)s.total,
			       length);
		return BLK_STS_IOERR;
	}
	if (s.beyond_mask)
		goto bounce;

	mode = nvme_pci_sgl_mode(dev, rq, &s);
	sgl_capable = nvme_pci_sgl_capable(dev, mode, &s);
	prp_capable = nvme_pci_prp_capable(&s);

	/* One segment: no iterator, no descriptors. */
	if (s.nseg == 1) {
		ret = nvme_pci_setup_data_simple(dev, iod, cmnd, mode,
						 sgl_capable, &s, length);
		if (ret != BLK_STS_AGAIN)
			return ret;
	}

	want_sgl = sgl_capable &&
		   (mode == SGL_FORCED || nvme_pci_sgl_by_threshold(&s) ||
		    !prp_capable);
	if (!want_sgl && !prp_capable)
		goto bounce;

	nvme_seg_iter_init(&it, sg, nsg, length, nvme_pci_xlate, NULL);
	if (want_sgl) {
		nvme_cmd_set_sgl(cmnd);
		ret = nvme_build_sgl(&it, s.nseg, nvme_pci_desc_alloc, &dctx,
				     &cmnd->common.dptr.sgl, &o);
	} else {
		ret = nvme_build_prps(&it, length, nvme_pci_desc_alloc, &dctx,
				      &o);
		cmnd->common.dptr.prp1 = cpu_to_le64(o.prp1);
		cmnd->common.dptr.prp2 = cpu_to_le64(o.prp2);
	}
	if (ret) {
		nvme_free_dptr_out(dctx.pools, &o);
		return ret;
	}
	nvme_iod_take_desc(iod, &o);
	iod->total_len = o.total_len;
	iod->first_dma = want_sgl ? le64_to_cpu(cmnd->common.dptr.sgl.addr) :
				    o.prp1;
	return BLK_STS_OK;

bounce:
	if (!allow_bounce) {
		WARN_ON_ONCE(1);
		kprintf(NVME_PCI_DMA_LOG "%s: bounce buffer not describable\n",
			dev->ctrl.name);
		return BLK_STS_IOERR;
	}
	ret = nvme_pci_bounce_data(dev, rq, iod, sg, nsg, length);
	if (ret)
		return ret;
	{
		struct blk_sg_entry one = {
			.virt = iod->bounce,
			.phys = iod->bounce_phys,
			.len = length,
			.flags = 0,
		};

		ret = nvme_pci_map_segments(dev, rq, iod, cmnd, &one, 1,
					    length, false);
	}
	if (ret)
		nvme_pci_unbounce_data(iod, rq);
	return ret;
}

blk_status_t nvme_map_data(struct nvme_dev *dev, struct blk_request *rq,
			   struct nvme_command *cmnd)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);
	uint64_t payload = blk_rq_payload_bytes(rq);

	BUG_ON(cmnd != &iod->cmd);
	/* A command whose data the core built (the DSM range list of a
	 * discard): that buffer is the one data segment, as the request's
	 * special payload. */
	if (nvme_req(rq)->flags & NVME_REQ_SPECIAL_PAYLOAD) {
		struct blk_sg_entry one = {
			.virt = nvme_req(rq)->special_vec,
			.phys = 0,
			.len = nvme_req(rq)->special_len,
			.flags = 0,
		};

		if (WARN_ON_ONCE(!one.virt || !one.len))
			return BLK_STS_IOERR;
		return nvme_pci_map_segments(dev, rq, iod, cmnd, &one, 1,
					     one.len, true);
	}
	if (!payload || !rq->nsg)
		return BLK_STS_OK;
	if (payload > 0xffffffffull) {
		WARN_RATELIMIT(1, NVME_PCI_DMA_LOG "%s: %llu byte request",
			       dev->ctrl.name, (unsigned long long)payload);
		return BLK_STS_IOERR;
	}
	return nvme_pci_map_segments(dev, rq, iod, cmnd, rq->sg, rq->nsg,
				     (uint32_t)payload, true);
}

void nvme_unmap_data(struct nvme_dev *dev, struct blk_request *rq)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);

	if (iod->nr_descriptors)
		nvme_free_descriptors(nvme_rq_pools(dev, rq), iod);
	nvme_pci_unbounce_data(iod, rq);
	iod->flags &= (uint8_t)~(IOD_SINGLE_SEGMENT | IOD_SMALL_DESCRIPTOR);
	iod->total_len = 0;
	iod->first_dma = 0;
}

/* ==========================================================================
 * Metadata
 * ========================================================================== */

static blk_status_t nvme_pci_bounce_meta(struct nvme_dev *dev,
					 struct blk_request *rq,
					 struct nvme_iod *iod,
					 const struct blk_sg_entry *e)
{
	uint32_t pages;
	uint64_t phys;

	phys = nvme_dma_alloc_bounce(dev->dma_mask, e->len, &pages);
	if (!phys) {
		__atomic_add_fetch(&g_nvme_bounce_fail, 1, __ATOMIC_RELAXED);
		return BLK_STS_RESOURCE;
	}
	iod->meta_bounce = phys_to_virt(phys);
	iod->meta_bounce_phys = phys;
	iod->meta_bounce_len = e->len;
	iod->meta_bounce_pages = pages;
	iod->meta_flags |= NVME_IOD_META_BOUNCED;
	if (rq_data_dir(rq))
		nvme_sg_copy(e, 1, iod->meta_bounce, e->len, true);
	__atomic_add_fetch(&g_nvme_bounce_total, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_nvme_bounce_live, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&dev->nr_bounces, 1, __ATOMIC_RELAXED);
	return BLK_STS_OK;
}

static void nvme_pci_unbounce_meta(struct nvme_iod *iod,
				   struct blk_request *rq,
				   const struct blk_integrity_payload *bip)
{
	if (!(iod->meta_flags & NVME_IOD_META_BOUNCED))
		return;
	dma_rmb();
	if (!rq_data_dir(rq) && bip && iod->data_valid) {
		struct blk_sg_entry e = {
			.virt = bip->buf,
			.phys = bip->phys,
			.len = bip->len,
			.flags = 0,
		};

		nvme_sg_copy(&e, 1, iod->meta_bounce, iod->meta_bounce_len,
			     false);
	}
	mm_free_contiguous_pages(iod->meta_bounce_phys,
				 iod->meta_bounce_pages);
	iod->meta_bounce = NULL;
	iod->meta_bounce_phys = 0;
	iod->meta_bounce_len = 0;
	iod->meta_bounce_pages = 0;
	iod->meta_flags &= (uint8_t)~NVME_IOD_META_BOUNCED;
	__atomic_sub_fetch(&g_nvme_bounce_live, 1, __ATOMIC_RELAXED);
}

blk_status_t nvme_map_metadata(struct nvme_dev *dev, struct blk_request *rq,
			       struct nvme_command *cmnd)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);
	struct blk_integrity_payload *bip = rq->integrity;
	struct nvme_pci_desc_ctx dctx = { .pools = nvme_rq_pools(dev, rq) };
	bool data_sgl = (cmnd->common.flags & NVME_CMD_SGL_METABUF) != 0;
	bool user = (nvme_req(rq)->flags & NVME_REQ_USERCMD) != 0;
	struct blk_sg_entry e;
	struct nvme_seg_iter it;
	struct nvme_seg_scan s;
	blk_status_t ret;
	bool use_sgl;

	iod->meta_flags = 0;
	iod->meta_descriptor = NULL;
	iod->meta_descriptor_dma = 0;
	iod->meta_dma = 0;
	iod->meta_total_len = 0;
	if (WARN_ON_ONCE(!bip || !bip->len))
		return BLK_STS_IOERR;
	e.virt = bip->buf;
	e.phys = bip->phys;
	e.len = bip->len;
	e.flags = 0;

	nvme_seg_iter_init(&it, &e, 1, e.len, nvme_pci_xlate, NULL);
	nvme_seg_scan(&it, dev->dma_mask, &s);
	if (s.status || s.total < e.len) {
		WARN_RATELIMIT(1, NVME_PCI_DMA_LOG "%s: unmapped metadata",
			       dev->ctrl.name);
		return BLK_STS_IOERR;
	}
	if (s.beyond_mask)
		goto bounce;

	/* Several pieces, or a user command whose length the controller
	 * should check: an SGL segment MPTR points to (data in SGLs and the
	 * controller supporting metadata SGLs). */
	use_sgl = data_sgl && nvme_ctrl_meta_sgl_supported(&dev->ctrl) &&
		  (s.nseg > 1 || user || rq->nr_integrity_segs > 1) &&
		  s.nseg <= NVME_MAX_META_SEGS &&
		  !(nvme_pci_ctrl_sgl_dword_only(&dev->ctrl) &&
		    (s.addr_unaligned || s.len_unaligned));
	if (use_sgl) {
		struct nvme_sgl_desc *list;
		uint64_t dma;
		uint32_t total;

		nvme_seg_iter_init(&it, &e, 1, e.len, nvme_pci_xlate, NULL);
		ret = nvme_build_meta_sgl(&it, s.nseg, nvme_pci_desc_alloc,
					  &dctx, &list, &dma, &total);
		if (ret) {
			if (list)
				nvme_dma_pool_free(&dctx.pools->small, list,
						   dma);
			return ret;
		}
		iod->meta_descriptor = list;
		iod->meta_descriptor_dma = dma;
		iod->meta_dma = dma;
		iod->meta_total_len = total;
		cmnd->common.flags = (uint8_t)((cmnd->common.flags &
						~NVME_CMD_PSDT_MASK) |
					       NVME_CMD_SGL_METASEG);
		cmnd->common.metadata = cpu_to_le64(dma);
		iod->meta_flags |= NVME_IOD_META_SGL | NVME_IOD_META_MAPPED;
		return BLK_STS_OK;
	}
	/* One contiguous buffer: MPTR (dword aligned unless the data is
	 * in SGLs, whose MPTR form is byte aligned). */
	if (s.nseg == 1 && (data_sgl || !(s.first_addr & 3))) {
		iod->meta_dma = s.first_addr;
		iod->meta_total_len = s.first_len;
		cmnd->common.metadata = cpu_to_le64(s.first_addr);
		iod->meta_flags |= NVME_IOD_META_SINGLE | NVME_IOD_META_MAPPED;
		return BLK_STS_OK;
	}

bounce:
	ret = nvme_pci_bounce_meta(dev, rq, iod, &e);
	if (ret)
		return ret;
	iod->meta_dma = iod->meta_bounce_phys;
	iod->meta_total_len = e.len;
	cmnd->common.metadata = cpu_to_le64(iod->meta_bounce_phys);
	iod->meta_flags |= NVME_IOD_META_SINGLE | NVME_IOD_META_MAPPED;
	return BLK_STS_OK;
}

void nvme_unmap_metadata(struct nvme_dev *dev, struct blk_request *rq)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);

	if (!(iod->meta_flags & NVME_IOD_META_MAPPED))
		return;
	if (iod->meta_descriptor)
		nvme_dma_pool_free(&nvme_rq_pools(dev, rq)->small,
				   iod->meta_descriptor,
				   iod->meta_descriptor_dma);
	iod->meta_descriptor = NULL;
	iod->meta_descriptor_dma = 0;
	nvme_pci_unbounce_meta(iod, rq, rq->integrity);
	iod->meta_dma = 0;
	iod->meta_total_len = 0;
	iod->meta_flags = 0;
}

/* ==========================================================================
 * Descriptor pools of the controller
 * ========================================================================== */

int nvme_setup_descriptor_pools(struct nvme_dev *dev)
{
	struct nvme_descriptor_pools *pools = &dev->descriptor_pools;
	uint32_t small_align = NVME_SMALL_POOL_SIZE;
	int rc;

	if (pools->initialized)
		return 0; /* already initialised */
	rc = nvme_dma_pool_init(&pools->large, "nvme descriptor page",
				NVME_CTRL_PAGE_SIZE, NVME_CTRL_PAGE_SIZE,
				dev->dma_mask, 8);
	if (rc)
		return rc;
	if (dev->ctrl.quirks & NVME_QUIRK_DMAPOOL_ALIGN_512)
		small_align = 512;
	rc = nvme_dma_pool_init(&pools->small, "nvme descriptor small",
				NVME_SMALL_POOL_SIZE, small_align,
				dev->dma_mask, 32);
	if (rc) {
		nvme_dma_pool_destroy(&pools->large);
		return rc;
	}
	pools->initialized = 1;
	return 0;
}

void nvme_release_descriptor_pools(struct nvme_dev *dev)
{
	struct nvme_descriptor_pools *pools = &dev->descriptor_pools;

	if (!pools->initialized)
		return;
	nvme_dma_pool_destroy(&pools->large);
	nvme_dma_pool_destroy(&pools->small);
	pools->initialized = 0;
}

/* ==========================================================================
 * Controller Memory Buffer
 * ========================================================================== */

static uint64_t nvme_cmb_size_unit(struct nvme_dev *dev)
{
	uint8_t szu = (dev->cmbsz >> NVME_CMBSZ_SZU_SHIFT) & NVME_CMBSZ_SZU_MASK;

	if (szu > NVME_CMBSZ_SZU_64G)
		return 0; /* reserved encoding */
	return 1ULL << (12 + 4 * szu);
}

static uint32_t nvme_cmb_size(struct nvme_dev *dev)
{
	return (dev->cmbsz >> NVME_CMBSZ_SZ_SHIFT) & NVME_CMBSZ_SZ_MASK;
}

void nvme_map_cmb(struct nvme_dev *dev)
{
	pci_device_t *pdev = dev->pdev;
	uint64_t size, offset, bar_size, bar_start, unit, map;
	int bar;

	if (dev->cmb_size)
		return;
	/* NVMe 1.4: the capability registers read zero until enabled. */
	if (NVME_CAP_CMBS(dev->ctrl.cap))
		writel(NVME_CMBMSC_CRE,
		       (volatile uint8_t *)dev->bar + NVME_REG_CMBMSC);

	dev->cmbsz = readl((volatile uint8_t *)dev->bar + NVME_REG_CMBSZ);
	if (!dev->cmbsz || dev->cmbsz == 0xffffffffu) {
		dev->cmbsz = 0;
		return;
	}
	dev->cmbloc = readl((volatile uint8_t *)dev->bar + NVME_REG_CMBLOC);

	unit = nvme_cmb_size_unit(dev);
	if (!unit)
		return;
	size = unit * nvme_cmb_size(dev);
	offset = unit * NVME_CMB_OFST(dev->cmbloc);
	bar = NVME_CMB_BIR(dev->cmbloc);
	if (bar > 5)
		return;
	bar_size = pci_resource_len(pdev, bar);
	bar_start = pci_resource_start(pdev, bar);
	if (!bar_size || !bar_start || offset > bar_size)
		return;

	/* A CMB larger than its BAR (behind a bridge): use what the BAR
	 * shows. */
	if (size > bar_size - offset)
		size = bar_size - offset;
	if (!size || (size & (PAGE_SIZE - 1)) ||
	    ((bar_start + offset) & (PAGE_SIZE - 1)))
		return;

	/* Tell the controller where the host sees the CMB and turn the
	 * memory space on (NVMe 1.4 scheme). */
	if (NVME_CAP_CMBS(dev->ctrl.cap))
		hi_lo_writeq(NVME_CMBMSC_CRE | NVME_CMBMSC_CMSE |
				     (bar_start + offset),
			     (volatile uint8_t *)dev->bar + NVME_REG_CMBMSC);

	/* Only the CMB's own pages, write-combining (when BIR is 0 the
	 * registers share the BAR and keep their uncached mapping). */
	map = mm_map_mmio_flags(bar_start + offset, size / PAGE_SIZE,
				MM_MMIO_WC);
	if (!map) {
		kprintf(NVME_PCI_DMA_LOG "%s: failed to map the CMB\n",
			dev->ctrl.name);
		if (NVME_CAP_CMBS(dev->ctrl.cap))
			hi_lo_writeq(0, (volatile uint8_t *)dev->bar +
						NVME_REG_CMBMSC);
		return;
	}
	dev->cmb = (volatile void *)map;
	dev->cmb_map_len = size;
	dev->cmb_bar = bar;
	dev->cmb_bus_addr = bar_start + offset;
	dev->cmb_alloc_offset = 0;
	dev->cmb_size = size;
	dev->cmb_use_sqes = nvme_pci_param.use_cmb_sqes &&
			    (dev->cmbsz & NVME_CMBSZ_SQS);
	kprintf(NVME_PCI_DMA_LOG
		"%s: CMB %llu KiB in BAR%d at +0x%llx%s\n",
		dev->ctrl.name, (unsigned long long)(size >> 10), bar,
		(unsigned long long)offset,
		dev->cmb_use_sqes ? ", I/O SQs in the CMB" : "");
}

void nvme_release_cmb(struct nvme_dev *dev)
{
	if (!dev->cmb_size)
		return;
	WARN_ON(dev->cmb_alloc_offset != 0);
	if (dev->bar && NVME_CAP_CMBS(dev->ctrl.cap) &&
	    !pci_dev_is_disconnected(dev->pdev))
		hi_lo_writeq(0, (volatile uint8_t *)dev->bar + NVME_REG_CMBMSC);
	if (dev->cmb)
		mm_unmap_mmio((uint64_t)dev->cmb, dev->cmb_map_len / PAGE_SIZE);
	dev->cmb = NULL;
	dev->cmb_map_len = 0;
	dev->cmb_size = 0;
	dev->cmb_use_sqes = false;
	dev->cmb_bar = -1;
	dev->cmb_alloc_offset = 0;
}

void *nvme_cmb_alloc_sq(struct nvme_dev *dev, uint64_t size, uint64_t *bus,
			uint64_t *offset)
{
	uint64_t len = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	if (!dev->cmb || !len || dev->cmb_alloc_offset + len > dev->cmb_size)
		return NULL;
	*offset = dev->cmb_alloc_offset;
	*bus = dev->cmb_bus_addr + *offset;
	dev->cmb_alloc_offset += len;
	return (void *)((volatile uint8_t *)dev->cmb + *offset);
}

void nvme_cmb_free_sq(struct nvme_dev *dev, uint64_t offset, uint64_t size)
{
	uint64_t len = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	/* Queues are freed top down: anything else leaves a hole that the
	 * stack allocator cannot reuse until everything above it goes. */
	if (WARN_ON_ONCE(offset + len != dev->cmb_alloc_offset))
		return;
	dev->cmb_alloc_offset = offset;
}

/* ==========================================================================
 * Persistent Memory Region
 * ========================================================================== */

static void nvme_pci_msleep(uint32_t ms)
{
	if (blk_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ull;

		while (hrtimer_sleep_until(until, NULL) != 0)
			;
	} else {
		while (ms) {
			uint32_t chunk = ms > 100 ? 100 : ms;

			lapic_delay_ms(chunk);
			ms -= chunk;
		}
	}
}

int nvme_pmr_enable(struct nvme_dev *dev)
{
	volatile uint8_t *bar = dev->bar;
	uint64_t start, len, map, waited = 0, timeout_ms;
	uint32_t sts, ctl;
	int bir;

	if (!nvme_pci_param.use_pmr || !NVME_CAP_PMRS(dev->ctrl.cap) || !bar)
		return 0;
	if (dev->bar_mapped_size < NVME_REG_PMRMSCU + 4)
		return -ENOMEM;

	if (!dev->pmr_enabled) {
		dev->pmrcap = readl(bar + NVME_REG_PMRCAP);
		if (dev->pmrcap == 0xffffffffu)
			return -ENODEV;
		bir = NVME_PMRCAP_BIR(dev->pmrcap);
		if (bir < 2 || bir > 5) {
			kprintf(NVME_PCI_DMA_LOG
				"%s: PMR in invalid BAR %d, not used\n",
				dev->ctrl.name, bir);
			return -EINVAL;
		}
		start = pci_resource_start(dev->pdev, bir);
		len = pci_resource_len(dev->pdev, bir);
		if (!start || !len || (len & (PAGE_SIZE - 1)))
			return -ENODEV;
		dev->pmr_bar = bir;
		dev->pmr_bus_addr = start;
		dev->pmr_size = len;
	}

	/* The controller must know the host address when it decodes the
	 * region through the memory space control registers. */
	if (dev->pmrcap & NVME_PMRCAP_CMSS) {
		writel((uint32_t)(dev->pmr_bus_addr >> 32),
		       bar + NVME_REG_PMRMSCU);
		writel(((uint32_t)dev->pmr_bus_addr & NVME_PMRMSCL_CBA_MASK) |
			       NVME_PMRMSCL_CMSE,
		       bar + NVME_REG_PMRMSCL);
	}
	ctl = readl(bar + NVME_REG_PMRCTL);
	if (!(ctl & NVME_PMRCTL_EN))
		writel(ctl | NVME_PMRCTL_EN, bar + NVME_REG_PMRCTL);

	timeout_ms = (uint64_t)NVME_PMRCAP_PMRTO(dev->pmrcap) *
		     (NVME_PMRCAP_PMRTU(dev->pmrcap) == NVME_PMRCAP_PMRTU_MIN ?
			      60000u :
			      500u);
	if (!timeout_ms)
		timeout_ms = 500;
	for (;;) {
		sts = readl(bar + NVME_REG_PMRSTS);
		if (sts == 0xffffffffu)
			return -ENODEV;
		if (!(sts & NVME_PMRSTS_NRDY))
			break;
		if (waited >= timeout_ms) {
			kprintf(NVME_PCI_DMA_LOG
				"%s: PMR not ready after %llu ms\n",
				dev->ctrl.name, (unsigned long long)waited);
			writel(ctl & ~NVME_PMRCTL_EN, bar + NVME_REG_PMRCTL);
			return -ETIMEDOUT;
		}
		nvme_pci_msleep(10);
		waited += 10;
	}
	if (NVME_PMRSTS_HSTS(sts) != NVME_PMRSTS_HSTS_NORMAL)
		kprintf(NVME_PCI_DMA_LOG "%s: PMR health status %u\n",
			dev->ctrl.name, (unsigned)NVME_PMRSTS_HSTS(sts));
	if (sts & NVME_PMRSTS_CBAI)
		kprintf(NVME_PCI_DMA_LOG
			"%s: PMR controller base address invalid\n",
			dev->ctrl.name);

	if (!dev->pmr_enabled) {
		map = mm_map_mmio_flags(dev->pmr_bus_addr,
					dev->pmr_size / PAGE_SIZE, MM_MMIO_WC);
		if (!map) {
			writel(ctl & ~NVME_PMRCTL_EN, bar + NVME_REG_PMRCTL);
			return -ENOMEM;
		}
		dev->pmr = (volatile void *)map;
		dev->pmr_map_len = dev->pmr_size;
		dev->pmr_enabled = true;
		kprintf(NVME_PCI_DMA_LOG "%s: PMR %llu KiB in BAR%d enabled\n",
			dev->ctrl.name,
			(unsigned long long)(dev->pmr_size >> 10),
			dev->pmr_bar);
	}
	return 0;
}

void nvme_pmr_disable(struct nvme_dev *dev)
{
	volatile uint8_t *bar = dev->bar;

	if (!dev->pmr_enabled)
		return;
	if (bar && !pci_dev_is_disconnected(dev->pdev)) {
		writel(0, bar + NVME_REG_PMRCTL);
		if (dev->pmrcap & NVME_PMRCAP_CMSS)
			writel(0, bar + NVME_REG_PMRMSCL);
	}
	if (dev->pmr)
		mm_unmap_mmio((uint64_t)dev->pmr, dev->pmr_map_len / PAGE_SIZE);
	dev->pmr = NULL;
	dev->pmr_map_len = 0;
	dev->pmr_enabled = false;
}

/* ==========================================================================
 * Host Memory Buffer
 * ========================================================================== */

int nvme_set_host_mem(struct nvme_dev *dev, uint32_t bits)
{
	uint32_t host_mem_size =
		(uint32_t)(dev->host_mem_size >> NVME_CTRL_PAGE_SHIFT);
	uint64_t dma_addr = dev->host_mem_descs_dma;
	struct nvme_command c;
	uint32_t seq;
	uint64_t fl;
	int ret;

	might_sleep();
	lockdep_assert_held(&dev->hmb_lock);
	spin_lock_irqsave(&dev->ctrl.lock, &fl);
	seq = dev->hmb_disable_seq;
	spin_unlock_irqrestore(&dev->ctrl.lock, fl);
	kmemset(&c, 0, sizeof(c));
	c.features.opcode = nvme_admin_set_features;
	c.features.fid = cpu_to_le32(NVME_FEAT_HOST_MEM_BUF);
	c.features.dword11 = cpu_to_le32(bits);
	c.features.dword12 = cpu_to_le32(host_mem_size);
	c.features.dword13 = cpu_to_le32((uint32_t)dma_addr);
	c.features.dword14 = cpu_to_le32((uint32_t)(dma_addr >> 32));
	c.features.dword15 = cpu_to_le32(dev->nr_host_mem_descs);

	ret = nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
	if (ret) {
		kprintf(NVME_PCI_DMA_LOG
			"%s: failed to set host mem (err %d, flags %#x)\n",
			dev->ctrl.name, ret, bits);
		return ret;
	}
	/* Answered, but the controller may have been disabled since the
	 * command went out (a reset raced a sysfs write): then it holds no
	 * buffer, and the state says so. */
	spin_lock_irqsave(&dev->ctrl.lock, &fl);
	if (dev->hmb_disable_seq == seq)
		dev->hmb = (bits & NVME_HOST_MEM_ENABLE) != 0;
	else
		dev->hmb = false;
	spin_unlock_irqrestore(&dev->ctrl.lock, fl);
	return ret;
}

static void *nvme_hmb_k_alloc_chunk(void *ctx, uint64_t len, uint64_t *dma)
{
	struct nvme_dev *dev = ctx;
	uint32_t pages;
	uint64_t p = nvme_dma_alloc_bounce(dev->dma_mask, (uint32_t)len, &pages);

	if (!p) {
		dev->hmb_alloc_fail++;
		return NULL;
	}
	*dma = p;
	return phys_to_virt(p);
}

static void nvme_hmb_k_free_chunk(void *ctx, void *buf, uint64_t dma,
				  uint64_t len)
{
	(void)ctx;
	(void)buf;
	mm_free_contiguous_pages(dma, (len + PAGE_SIZE - 1) / PAGE_SIZE);
}

static void *nvme_hmb_k_alloc_descs(void *ctx, uint32_t size, uint64_t *dma)
{
	return nvme_dma_alloc_coherent(ctx, size, PAGE_SIZE, dma);
}

static void nvme_hmb_k_free_descs(void *ctx, void *descs, uint64_t dma,
				  uint32_t size)
{
	nvme_dma_free_coherent(ctx, size, descs, dma);
}

static void *nvme_hmb_k_alloc_array(void *ctx, size_t bytes)
{
	(void)ctx;
	return kcalloc(1, bytes);
}

static void nvme_hmb_k_free_array(void *ctx, void *array)
{
	(void)ctx;
	kfree(array);
}

static const struct nvme_hmb_ops nvme_hmb_kernel_ops = {
	.alloc_chunk = nvme_hmb_k_alloc_chunk,
	.free_chunk = nvme_hmb_k_free_chunk,
	.alloc_descs = nvme_hmb_k_alloc_descs,
	.free_descs = nvme_hmb_k_free_descs,
	.alloc_array = nvme_hmb_k_alloc_array,
	.free_array = nvme_hmb_k_free_array,
};

/* The chunk lengths are kept next to the addresses (host_mem_desc_phys
 * holds 2 * nr entries: addresses, then lengths). */
void nvme_free_host_mem(struct nvme_dev *dev)
{
	struct nvme_hmb_result r;

	might_sleep();
	lockdep_assert_held(&dev->hmb_lock);
	if (!dev->host_mem_descs)
		return;
	/* An enabled HMB is the controller's memory: it may be written at
	 * any time until Set Features disables it or the controller is
	 * disabled (which clears dev->hmb). */
	if (WARN(dev->hmb, NVME_PCI_DMA_LOG
		 "%s: host memory buffer still enabled, kept",
		 dev->ctrl.name))
		return;
	r.descs = dev->host_mem_descs;
	r.descs_dma = dev->host_mem_descs_dma;
	r.descs_size = dev->host_mem_descs_size;
	r.nr_descs = dev->nr_host_mem_descs;
	r.size = dev->host_mem_size;
	r.bufs = dev->host_mem_desc_bufs;
	r.phys = dev->host_mem_desc_phys;
	r.lens = dev->host_mem_desc_phys ?
			 dev->host_mem_desc_phys + dev->nr_host_mem_descs :
			 NULL;
	/* phys and lens live in one array: free it once. */
	{
		uint64_t *both = r.phys;

		r.phys = NULL;
		r.lens = NULL;
		for (uint32_t i = 0; r.bufs && i < r.nr_descs; i++)
			nvme_hmb_k_free_chunk(dev, r.bufs[i], both[i],
					      both[r.nr_descs + i]);
		r.nr_descs = 0;
		if (both)
			kfree(both);
	}
	nvme_hmb_release(&nvme_hmb_kernel_ops, dev, &r);
	dev->host_mem_descs = NULL;
	dev->host_mem_desc_bufs = NULL;
	dev->host_mem_desc_phys = NULL;
	dev->host_mem_descs_size = 0;
	dev->nr_host_mem_descs = 0;
	dev->host_mem_size = 0;
	dev->host_mem_descs_dma = 0;
}

static int nvme_alloc_host_mem(struct nvme_dev *dev, uint64_t min,
			       uint64_t preferred)
{
	struct nvme_hmb_result r;
	uint64_t *both;
	int rc;

	rc = nvme_hmb_alloc(&nvme_hmb_kernel_ops, dev, min, preferred,
			    dev->ctrl.hmminds, dev->ctrl.hmmaxd, &r);
	if (rc)
		return rc;
	both = kcalloc(2u * r.nr_descs, sizeof(uint64_t));
	if (!both) {
		nvme_hmb_release(&nvme_hmb_kernel_ops, dev, &r);
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < r.nr_descs; i++) {
		both[i] = r.phys[i];
		both[r.nr_descs + i] = r.lens[i];
	}
	kfree(r.phys);
	kfree(r.lens);
	dev->host_mem_descs = r.descs;
	dev->host_mem_descs_dma = r.descs_dma;
	dev->host_mem_descs_size = r.descs_size;
	dev->nr_host_mem_descs = r.nr_descs;
	dev->host_mem_size = r.size;
	dev->host_mem_desc_bufs = r.bufs;
	dev->host_mem_desc_phys = both;
	dev->hmb_chunk_size = r.chunk_size;
	return 0;
}

int nvme_setup_host_mem(struct nvme_dev *dev)
{
	uint64_t max = (uint64_t)nvme_pci_param.max_host_mem_size_mb << 20;
	uint64_t preferred = (uint64_t)dev->ctrl.hmpre * 4096u;
	uint64_t min = (uint64_t)dev->ctrl.hmmin * 4096u;
	uint32_t enable_bits = NVME_HOST_MEM_ENABLE;
	int ret;

	might_sleep();
	lockdep_assert_held(&dev->hmb_lock);
	if (!dev->ctrl.hmpre)
		return 0;

	if (preferred > max)
		preferred = max;
	if (min > max) {
		kprintf(NVME_PCI_DMA_LOG
			"%s: min host memory (%llu MiB) above limit (%u MiB)\n",
			dev->ctrl.name, (unsigned long long)(min >> 20),
			nvme_pci_param.max_host_mem_size_mb);
		nvme_free_host_mem(dev);
		return 0;
	}

	/* A buffer from before the reset can be handed back as it is (the
	 * controller is told its content is unchanged). */
	if (dev->host_mem_descs) {
		if (dev->host_mem_size >= min)
			enable_bits |= NVME_HOST_MEM_RETURN;
		else
			nvme_free_host_mem(dev);
	}

	if (!dev->host_mem_descs) {
		if (nvme_alloc_host_mem(dev, min, preferred)) {
			kprintf(NVME_PCI_DMA_LOG
				"%s: failed to allocate host memory buffer\n",
				dev->ctrl.name);
			return 0; /* the controller must work without */
		}
		kprintf(NVME_PCI_DMA_LOG
			"%s: allocated %llu MiB host memory buffer (%u segment%s)\n",
			dev->ctrl.name,
			(unsigned long long)(dev->host_mem_size >> 20),
			dev->nr_host_mem_descs,
			dev->nr_host_mem_descs == 1 ? "" : "s");
	}

	ret = nvme_set_host_mem(dev, enable_bits);
	if (ret)
		nvme_free_host_mem(dev);
	return ret;
}
