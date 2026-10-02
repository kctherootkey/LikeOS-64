// LikeOS -- the global GTT, stolen memory and the PPGTT of the parts
// before Broadwell.
//
// Up to Ironlake the global GTT lives in the GMCH: gen2 keeps it inside
// the register window, gen3 behind a BAR of its own, gen4 and gen5 in
// the second half of the register BAR; its size is the aperture's on
// gen2 and most of gen3 and a field of the page table control register
// (set from the host bridge on G4x) everywhere else.  The firmware's
// stolen memory is described by the host bridge, chipset by chipset.
// From Sandy Bridge on the GTT is the second half of BAR0 and both sizes
// come from the graphics function's SNB_GMCH_CTRL.  Every entry is 32
// bits: the page's address with its top bits folded into the low byte,
// and, depending on the part, a snoop or an LLC/L3/eDRAM cache code.
//
// Clients of these parts get one address space between them.  Up to
// Ironlake that is the global GTT itself: a binding made for a context is
// a global binding.  Sandy Bridge to Haswell run batches through an
// aliasing PPGTT: a two-level table whose directory sits in the global
// GTT's own entry array, filled with the same addresses the global space
// hands out, so a client's binding and the driver's global bindings
// never overlap.  One bitmap (i915->ggtt_map) says which pages of the
// global space are taken, by either.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define KB(x) ((uint64_t)(x) << 10)
#define MB(x) ((uint64_t)(x) << 20)

/* ---- the host bridge's configuration space ----------------------------------- */

static uint32_t hb_read32(int func, unsigned off)
{
	return pci_cfg_read32(0, 0, (unsigned char)func, (unsigned char)(off & ~3u));
}

static uint8_t hb_read8(int func, unsigned off)
{
	return (uint8_t)(hb_read32(func, off) >> ((off & 3) * 8));
}

static uint16_t hb_read16(unsigned off)
{
	return (uint16_t)(hb_read32(0, off) >> ((off & 2) * 8));
}

static void hb_write16(unsigned off, uint16_t v)
{
	uint32_t d = hb_read32(0, off);
	unsigned sh = (off & 2) * 8;
	d = (d & ~(0xffffu << sh)) | ((uint32_t)v << sh);
	pci_cfg_write32(0, 0, 0, (unsigned char)(off & ~3u), d);
}

/* ---- stolen memory, per chipset ---------------------------------------------- */

static uint64_t i830_tseg_size(void)
{
	uint8_t esmramc = hb_read8(0, I830_ESMRAMC);

	if (!(esmramc & TSEG_ENABLE))
		return 0;
	return (esmramc & I830_TSEG_SIZE_1M) ? MB(1) : KB(512);
}

static uint64_t i845_tseg_size(void)
{
	uint8_t esmramc = hb_read8(0, I845_ESMRAMC);

	if (!(esmramc & TSEG_ENABLE))
		return 0;
	switch (esmramc & I845_TSEG_SIZE_MASK) {
	case I845_TSEG_SIZE_512K:
		return KB(512);
	case I845_TSEG_SIZE_1M:
		return MB(1);
	default:
		kprintf("[drm] i915: unknown ESMRAMC value %02x\n", esmramc);
		return 0;
	}
}

static uint64_t i85x_tseg_size(void)
{
	uint8_t esmramc = hb_read8(0, I85X_ESMRAMC);

	if (!(esmramc & TSEG_ENABLE))
		return 0;
	return MB(1);
}

static uint64_t i830_mem_size(void)
{
	return (uint64_t)hb_read8(0, I830_DRB3) * MB(32);
}

static uint64_t i85x_mem_size(void)
{
	return (uint64_t)hb_read8(1, I85X_DRB3) * MB(32);
}

static uint64_t i830_stolen_size(void)
{
	switch (hb_read16(I830_GMCH_CTRL) & I830_GMCH_GMS_MASK) {
	case I830_GMCH_GMS_STOLEN_512:
		return KB(512);
	case I830_GMCH_GMS_STOLEN_1024:
		return MB(1);
	case I830_GMCH_GMS_STOLEN_8192:
		return MB(8);
	/* local memory is not part of the normal address space */
	case I830_GMCH_GMS_LOCAL:
	default:
		return 0;
	}
}

static uint64_t gen3_stolen_size(void)
{
	switch (hb_read16(I830_GMCH_CTRL) & I855_GMCH_GMS_MASK) {
	case I855_GMCH_GMS_STOLEN_1M: return MB(1);
	case I855_GMCH_GMS_STOLEN_4M: return MB(4);
	case I855_GMCH_GMS_STOLEN_8M: return MB(8);
	case I855_GMCH_GMS_STOLEN_16M: return MB(16);
	case I855_GMCH_GMS_STOLEN_32M: return MB(32);
	case I915_GMCH_GMS_STOLEN_48M: return MB(48);
	case I915_GMCH_GMS_STOLEN_64M: return MB(64);
	case G33_GMCH_GMS_STOLEN_128M: return MB(128);
	case G33_GMCH_GMS_STOLEN_256M: return MB(256);
	case INTEL_GMCH_GMS_STOLEN_96M: return MB(96);
	case INTEL_GMCH_GMS_STOLEN_160M: return MB(160);
	case INTEL_GMCH_GMS_STOLEN_224M: return MB(224);
	case INTEL_GMCH_GMS_STOLEN_352M: return MB(352);
	default: return 0;
	}
}

static void leg_stolen(struct i915_device *i915)
{
	uint64_t size = 0, base = 0;

	switch (i915->info->platform) {
	case I915_PLATFORM_I830:
		size = i830_stolen_size();
		base = i830_mem_size() - i830_tseg_size() - size;
		break;
	case I915_PLATFORM_I845G:
		size = i830_stolen_size();
		base = i830_mem_size() - i845_tseg_size() - size;
		break;
	case I915_PLATFORM_I85X:
		size = gen3_stolen_size();
		base = i85x_mem_size() - i85x_tseg_size() - size;
		break;
	case I915_PLATFORM_I865G:
		size = gen3_stolen_size();
		base = (uint64_t)hb_read16(I865_TOUD) * KB(64) + i845_tseg_size();
		break;
	default:
		/* gen3 on: the graphics function's BSM has the base;
		 * gen3-5 the host bridge, Gen6/7 the function's own
		 * control word the size */
		base = pci_cfg_read32_dev(i915->pci, INTEL_BSM) & INTEL_BSM_MASK;
		if (leg_gen(i915) >= 6) {
			uint16_t ctl = pci_cfg_read16(i915->pci, SNB_GMCH_CTRL);
			size = (uint64_t)((ctl >> SNB_GMCH_GMS_SHIFT) & SNB_GMCH_GMS_MASK) * MB(32);
		} else {
			size = gen3_stolen_size();
		}
		break;
	}
	if (!size)
		base = 0;
	i915->stolen_base = base;
	i915->stolen_size = size;
}

/* ---- the GTT's size and place ----------------------------------------------------- */

static int has_pgtbl_enable(struct i915_device *i915)
{
	int p = i915->info->platform;

	/* gen2, i915/i945 and the two non-G4x gen4 parts enable the page
	 * table themselves; G33, Pineview, G4x and Ironlake do not */
	return leg_gen(i915) == 2 || (leg_gen(i915) == 3 && !g_leg.is_g33) ||
	       p == I915_PLATFORM_I965G || p == I915_PLATFORM_I965GM;
}

/* G4x and Ironlake: the page table's size follows the host bridge's
 * GTT size field (PPGTT disabled on the way). */
static void i965_adjust_pgetbl_size(struct i915_device *i915, uint32_t size_flag)
{
	uint32_t ctl2 = i915_read32(i915, I965_PGETBL_CTL2);
	i915_write32(i915, I965_PGETBL_CTL2, ctl2 & ~I810_PGETBL_ENABLED);
	uint32_t ctl = i915_read32(i915, I810_PGETBL_CTL);
	ctl &= ~I965_PGETBL_SIZE_MASK;
	ctl |= size_flag;
	i915_write32(i915, I810_PGETBL_CTL, ctl);
}

static uint32_t i965_gtt_total_entries(struct i915_device *i915)
{
	uint32_t size;

	if (g_leg.is_g4x || leg_gen(i915) == 5) {
		switch (hb_read16(I830_GMCH_CTRL) & G4X_GMCH_SIZE_MASK) {
		case G4X_GMCH_SIZE_1M:
		case G4X_GMCH_SIZE_VT_1M:
			i965_adjust_pgetbl_size(i915, I965_PGETBL_SIZE_1MB);
			break;
		case G4X_GMCH_SIZE_VT_1_5M:
			i965_adjust_pgetbl_size(i915, I965_PGETBL_SIZE_1_5MB);
			break;
		case G4X_GMCH_SIZE_2M:
		case G4X_GMCH_SIZE_VT_2M:
			i965_adjust_pgetbl_size(i915, I965_PGETBL_SIZE_2MB);
			break;
		}
	}
	switch (i915_read32(i915, I810_PGETBL_CTL) & I965_PGETBL_SIZE_MASK) {
	case I965_PGETBL_SIZE_128KB:
		size = KB(128);
		break;
	case I965_PGETBL_SIZE_256KB:
		size = KB(256);
		break;
	case I965_PGETBL_SIZE_512KB:
		size = KB(512);
		break;
	/* sizes above 512 KB are not possible on G33 */
	case I965_PGETBL_SIZE_1MB:
		size = KB(1024);
		break;
	case I965_PGETBL_SIZE_2MB:
		size = KB(2048);
		break;
	case I965_PGETBL_SIZE_1_5MB:
		size = KB(1024 + 512);
		break;
	default:
		kprintf("[drm] i915: unknown page table size, assuming 512 KB\n");
		size = KB(512);
		break;
	}
	return size / 4;
}

/* The write buffers between the processor and memory, drained: the
 * chipset's flush page on gen3-5 when the firmware set one up, the host
 * interface's flush bit on gen2. */
static void leg_setup_flush_page(struct i915_device *i915)
{
	uint64_t addr;

	if (leg_gen(i915) < 3 || leg_gen(i915) > 5 || g_leg.ifp_page)
		return;
	if (leg_gen(i915) >= 4 || g_leg.is_g33) {
		uint32_t lo = hb_read32(0, I965_IFPADDR);
		uint32_t hi = hb_read32(0, I965_IFPADDR + 4);
		if (!(lo & 1))
			return;
		addr = ((uint64_t)hi << 32) | (lo & ~1u);
	} else {
		uint32_t v = hb_read32(0, I915_IFPADDR);
		if (!(v & 1))
			return;
		addr = v & ~1u;
	}
	uint64_t va = mm_map_mmio_flags(addr & ~0xfffULL, 1, MM_MMIO_UC);
	if (va)
		g_leg.ifp_page = (volatile uint32_t *)(va + (addr & 0xfff));
}

void i915_legacy_chipset_flush(struct i915_device *i915)
{
	int gen = leg_gen(i915);

	if (gen >= 6)
		return;
	__asm__ volatile("mfence" ::: "memory");
	if (gen == 2) {
		/* clflush is not enough here: the write buffers are
		 * emptied, then the host interface is told to drain */
		__asm__ volatile("wbinvd" ::: "memory");
		i915_write32_fw(i915, I830_HIC, i915_read32_fw(i915, I830_HIC) | (1u << 31));
		for (int t = 0; t < 20000; t++) {
			if (!(i915_read32_fw(i915, I830_HIC) & (1u << 31)))
				break;
			lapic_delay_us(50);
		}
		return;
	}
	/* without a flush page (the firmware set none up) there is no
	 * chipset flush; the processor's flushes are all there is */
	if (g_leg.ifp_page)
		*g_leg.ifp_page = 1;
}

/* ---- entries ------------------------------------------------------------------------ */

uint32_t leg_pte_encode(struct i915_device *i915, uint64_t phys, enum leg_cache level, int readonly)
{
	uint32_t pte;

	(void)i915;
	switch (g_leg.pte_kind) {
	case LEG_PTE_I830:
		pte = (uint32_t)phys | I810_PTE_VALID;
		if (level != LEG_CACHE_NONE && g_leg.has_snoop)
			pte |= I830_PTE_SYSTEM_CACHED;
		return pte;
	case LEG_PTE_I965:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0xf0) | I810_PTE_VALID;
		if (level != LEG_CACHE_NONE && g_leg.has_snoop)
			pte |= I830_PTE_SYSTEM_CACHED;
		return pte;
	case LEG_PTE_SNB:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0xff0) | GEN6_PTE_VALID;
		pte |= level == LEG_CACHE_NONE ? GEN6_PTE_UNCACHED : GEN6_PTE_CACHE_LLC;
		return pte;
	case LEG_PTE_IVB:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0xff0) | GEN6_PTE_VALID;
		if (level == LEG_CACHE_L3_LLC)
			pte |= GEN7_PTE_CACHE_L3_LLC;
		else if (level == LEG_CACHE_NONE)
			pte |= GEN6_PTE_UNCACHED;
		else
			pte |= GEN6_PTE_CACHE_LLC;
		return pte;
	case LEG_PTE_BYT:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0xff0) | GEN6_PTE_VALID;
		if (!readonly)
			pte |= BYT_PTE_WRITEABLE;
		if (level != LEG_CACHE_NONE)
			pte |= BYT_PTE_SNOOPED_BY_CPU_CACHES;
		return pte;
	case LEG_PTE_HSW:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0x7f0) | GEN6_PTE_VALID;
		if (level != LEG_CACHE_NONE)
			pte |= HSW_WB_LLC_AGE3;
		return pte;
	case LEG_PTE_IRIS:
	default:
		pte = (uint32_t)phys | (uint32_t)((phys >> 28) & 0x7f0) | GEN6_PTE_VALID;
		if (level == LEG_CACHE_WT)
			pte |= HSW_WT_ELLC_LLC_AGE3;
		else if (level != LEG_CACHE_NONE)
			pte |= HSW_WB_ELLC_LLC_AGE3;
		return pte;
	}
}

/* The entry's page, or 0 when it is not valid. */
static uint64_t leg_pte_decode(uint32_t pte)
{
	if (!(pte & 1))
		return 0;
	switch (g_leg.pte_kind) {
	case LEG_PTE_I830:
		return pte & 0xfffff000u;
	case LEG_PTE_I965:
		return ((uint64_t)(pte & 0xf0) << 28) | (pte & 0xfffff000u);
	case LEG_PTE_HSW:
	case LEG_PTE_IRIS:
		return ((uint64_t)(pte & 0x7f0) << 28) | (pte & 0xfffff000u);
	default:
		return ((uint64_t)(pte & 0xff0) << 28) | (pte & 0xfffff000u);
	}
}

/* A page the device cannot address (above the part's DMA limit) is
 * mapped as the scratch page instead, and said once. */
static uint32_t leg_pte_for_page(struct i915_device *i915, uint64_t phys, enum leg_cache level)
{
	if (phys + 4096 > g_leg.dma_limit) {
		if (!g_leg.dma_warned) {
			g_leg.dma_warned = 1;
			kprintf("[drm] i915: page %llx is beyond what %s can address; scratch mapped instead\n",
				(unsigned long long)phys, i915->info->name);
		}
		return g_leg.scratch_pte;
	}
	return leg_pte_encode(i915, phys, level, 0);
}

void leg_ggtt_write(struct i915_device *i915, uint32_t index, uint32_t pte)
{
	if (index < g_leg.gtt_entries)
		((volatile uint32_t *)i915->gtt_virt)[index] = pte;
}

void leg_ggtt_invalidate(struct i915_device *i915)
{
	if (!i915->gtt_virt)
		return;
	/* the entries have landed before the TLBs are told */
	(void)((volatile uint32_t *)i915->gtt_virt)[0];
	if (leg_gen(i915) >= 6) {
		i915_write32_fw(i915, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
		(void)i915_read32_fw(i915, GFX_FLSH_CNTL_GEN6);
	} else {
		i915_legacy_chipset_flush(i915);
	}
}

/* The cache attribute of a binding.  With an LLC everything not asked to
 * be uncached goes through it.  Without one, a part that can snoop does
 * so only for an object its client asked to be cached (a binding made
 * for a client's address space carries that request in `uncached' and
 * comes without an object); the driver's own objects stay uncached and
 * are flushed by the processor instead. */
enum leg_cache leg_cache_of(struct i915_device *i915, const struct drm_gem_object *o, int uncached)
{
	if (uncached)
		return LEG_CACHE_NONE;
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return LEG_CACHE_LLC;
	if (!g_leg.has_snoop)
		return LEG_CACHE_NONE;
	if (!o)
		return LEG_CACHE_LLC;
	if (o->priv && ((const struct i915_bo *)o->priv)->caching == I915_CACHING_CACHED)
		return LEG_CACHE_LLC;
	return LEG_CACHE_NONE;
}

/* ---- the allocator over the global space ------------------------------------------- */

static inline int map_used(const struct i915_device *i915, uint32_t page)
{
	return (i915->ggtt_map[page / 8] >> (page % 8)) & 1;
}

static inline void map_set(uint8_t *map, uint32_t page, int on)
{
	if (on)
		map[page / 8] |= (uint8_t)(1u << (page % 8));
	else
		map[page / 8] &= (uint8_t)~(1u << (page % 8));
}

static int range_free(const struct i915_device *i915, uint32_t first, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (map_used(i915, first + i))
			return 0;
	return 1;
}

/* A free run of `npages' pages aligned to `align_pages' (a power of two)
 * inside [lo, hi), the lowest or the highest one; marked taken.  Caller
 * holds gtt_lock. */
static int ggtt_alloc_locked(struct i915_device *i915, uint32_t npages, uint32_t align_pages,
			     uint32_t lo, uint32_t hi, int top_down, uint32_t *first)
{
	if (!align_pages)
		align_pages = 1;
	if (lo < i915->ggtt_map_first)
		lo = i915->ggtt_map_first;
	if (hi > i915->ggtt_map_pages)
		hi = i915->ggtt_map_pages;
	if (!npages || hi <= lo || hi - lo < npages)
		return -ENOSPC;
	if (top_down) {
		uint32_t start = (hi - npages) & ~(align_pages - 1);
		for (;;) {
			if (start < lo)
				return -ENOSPC;
			if (range_free(i915, start, npages))
				break;
			if (start < align_pages)
				return -ENOSPC;
			start -= align_pages;
		}
		*first = start;
	} else {
		uint32_t start = (lo + align_pages - 1) & ~(align_pages - 1);
		for (;;) {
			if (start + npages > hi || start + npages < start)
				return -ENOSPC;
			uint32_t i = 0;
			for (; i < npages; i++)
				if (map_used(i915, start + i))
					break;
			if (i == npages)
				break;
			start = (start + i + 1 + align_pages - 1) & ~(align_pages - 1);
		}
		*first = start;
	}
	for (uint32_t i = 0; i < npages; i++)
		map_set(i915->ggtt_map, *first + i, 1);
	return 0;
}

int leg_ggtt_alloc(struct i915_device *i915, uint32_t npages, uint32_t align_pages,
		   uint32_t lo, uint32_t hi, int top_down, uint32_t *first)
{
	uint64_t fl;
	int rc;

	if (!i915->ggtt_map)
		return -ENODEV;
	spin_lock_irqsave(&g_leg.gtt_lock, &fl);
	rc = ggtt_alloc_locked(i915, npages, align_pages, lo, hi, top_down, first);
	spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
	return rc;
}

void leg_ggtt_free(struct i915_device *i915, uint32_t first, uint32_t npages)
{
	uint64_t fl;

	if (!i915->ggtt_map)
		return;
	spin_lock_irqsave(&g_leg.gtt_lock, &fl);
	for (uint32_t i = 0; i < npages && first + i < i915->ggtt_map_pages; i++) {
		map_set(i915->ggtt_map, first + i, 0);
		if (g_leg.vm_owned)
			map_set(g_leg.vm_owned, first + i, 0);
	}
	spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
}

static uint32_t mappable_pages(struct i915_device *i915)
{
	uint64_t m = g_leg.mappable_end / 4096;
	return m < i915->ggtt_map_pages ? (uint32_t)m : i915->ggtt_map_pages;
}

/* ---- the driver's global bindings --------------------------------------------------- */

static void bind_pages(struct i915_device *i915, uint32_t first, const uint64_t *pages,
		       uint32_t npages, enum leg_cache level)
{
	for (uint32_t i = 0; i < npages; i++)
		leg_ggtt_write(i915, first + i, leg_pte_for_page(i915, pages[i], level));
	leg_ggtt_invalidate(i915);
}

int leg_ggtt_bind_level(struct i915_device *i915, struct drm_gem_object *o, enum leg_cache level,
			int mappable, uint32_t *ggtt)
{
	uint32_t first;
	int rc;

	if (!i915->gtt_virt || !o->pages || !o->npages)
		return -EINVAL;
	if (mappable)
		rc = leg_ggtt_alloc(i915, o->npages, 1, 0, mappable_pages(i915), 0, &first);
	else
		rc = leg_ggtt_alloc(i915, o->npages, 1, 0, i915->ggtt_map_pages, 1, &first);
	if (rc)
		return rc;
	bind_pages(i915, first, o->pages, o->npages, level);
	*ggtt = first * 4096;
	return 0;
}

#define SCANOUT_ALIGN_PAGES 64u

int i915_legacy_ggtt_bind_obj(struct i915_device *i915, struct drm_gem_object *o,
			      int uncached, uint32_t *ggtt_offset)
{
	uint32_t first;
	int rc;

	if (!i915->gtt_virt || !o->pages || !o->npages)
		return -EINVAL;
	/* Scanout surfaces (the uncached kind) at 256 KB, as the display
	 * engine wants them; everything else from the top down, out of
	 * the aperture's way. */
	rc = leg_ggtt_alloc(i915, o->npages, uncached ? SCANOUT_ALIGN_PAGES : 1u, 0,
			    i915->ggtt_map_pages, 1, &first);
	if (rc) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: no room in the global address space for %u pages\n",
				o->npages);
		}
		return rc;
	}
	bind_pages(i915, first, o->pages, o->npages, leg_cache_of(i915, o, uncached));
	*ggtt_offset = first * 4096;
	if (o->priv)
		((struct i915_bo *)o->priv)->ggtt_uncached = uncached;
	return 0;
}

int i915_legacy_ggtt_bind_obj_mappable(struct i915_device *i915, struct drm_gem_object *o,
				       uint32_t *ggtt_offset)
{
	uint32_t first;
	int rc;

	if (!i915->gtt_virt || !o->pages || !o->npages || !g_leg.mappable_end)
		return -EINVAL;
	rc = leg_ggtt_alloc(i915, o->npages, 1, 0, mappable_pages(i915), 0, &first);
	if (rc) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: no room behind the aperture for %u pages\n", o->npages);
		}
		return rc;
	}
	bind_pages(i915, first, o->pages, o->npages, LEG_CACHE_NONE);
	*ggtt_offset = first * 4096;
	return 0;
}

void i915_legacy_ggtt_unbind(struct i915_device *i915, uint32_t ggtt_offset, uint32_t size)
{
	uint32_t first = ggtt_offset / 4096;
	uint32_t n = (size + 4095) / 4096;

	if (!i915->gtt_virt)
		return;
	for (uint32_t i = 0; i < n; i++)
		leg_ggtt_write(i915, first + i, g_leg.scratch_pte);
	leg_ggtt_invalidate(i915);
	leg_ggtt_free(i915, first, n);
}

/* The global space not taken by the driver's own bindings: what clients
 * can still have (theirs count as available, as they would be moved to
 * make room). */
uint64_t i915_legacy_aperture_available(struct i915_device *i915)
{
	uint64_t used = 0;
	uint64_t fl;
	uint32_t m = i915->ggtt_map_pages;

	if (!i915->ggtt_map)
		return 0;
	spin_lock_irqsave(&g_leg.gtt_lock, &fl);
	for (uint32_t p = 0; p < m; p++)
		used += p < i915->ggtt_map_first ||
			(map_used(i915, p) && !((g_leg.vm_owned[p / 8] >> (p % 8)) & 1));
	spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
	return ((uint64_t)m - used) * 4096;
}

/* ---- the Gen6/7 PPGTT --------------------------------------------------------------- */

static inline uint32_t gen6_pde_encode(uint64_t pt_phys)
{
	return (uint32_t)pt_phys | (uint32_t)((pt_phys >> 28) & 0xff0) | GEN6_PDE_VALID;
}

static void pt_flush(struct i915_device *i915, void *va, uint32_t bytes)
{
	if (!(i915->info->flags & I915_INFO_HAS_LLC))
		leg_clflush_range(va, bytes);
}

/* Write every directory entry into its slot of the global GTT's array. */
void leg_ppgtt_rewrite_pd(struct i915_device *i915)
{
	struct leg_ppgtt *pp = &g_leg.ppgtt;
	uint32_t first = pp->pd_ggtt / 4096;

	if (!pp->present)
		return;
	for (uint32_t pde = 0; pde < GEN6_PDES; pde++)
		leg_ggtt_write(i915, first + pde,
			       gen6_pde_encode(pp->pt_phys[pde] ? pp->pt_phys[pde] : pp->scratch_pt));
	leg_ggtt_invalidate(i915);
}

static int ppgtt_init(struct i915_device *i915)
{
	struct leg_ppgtt *pp = &g_leg.ppgtt;
	uint32_t first;

	mm_memset(pp, 0, sizeof(*pp));
	/* the scratch page read-only where the entry can say so */
	pp->scratch_pte = leg_pte_encode(i915, g_leg.scratch_phys, LEG_CACHE_NONE, 1);
	pp->scratch_pt = mm_allocate_physical_page_reclaim();
	if (!pp->scratch_pt)
		return -ENOMEM;
	uint32_t *t = phys_to_virt(pp->scratch_pt);
	for (int i = 0; i < GEN6_PTES; i++)
		t[i] = pp->scratch_pte;
	pt_flush(i915, t, 4096);
	/* The directory's 512 entries are the global GTT entries of a
	 * 2 MB range given up for them, at the top of the space. */
	if (leg_ggtt_alloc(i915, GEN6_PD_SIZE / 4096, GEN6_PD_ALIGN / 4096, 0, i915->ggtt_map_pages,
			   1, &first) != 0) {
		mm_free_physical_page(pp->scratch_pt);
		pp->scratch_pt = 0;
		return -ENOSPC;
	}
	pp->pd_ggtt = first * 4096;
	pp->pp_dir = (first * 4) << 10;
	pp->present = 1;
	leg_ppgtt_rewrite_pd(i915);
	return 0;
}

static void ppgtt_fini(struct i915_device *i915)
{
	struct leg_ppgtt *pp = &g_leg.ppgtt;

	if (!pp->present)
		return;
	for (uint32_t pde = 0; pde < GEN6_PDES; pde++)
		if (pp->pt_phys[pde])
			mm_free_physical_page(pp->pt_phys[pde]);
	if (pp->scratch_pt)
		mm_free_physical_page(pp->scratch_pt);
	leg_ggtt_free(i915, pp->pd_ggtt / 4096, GEN6_PD_SIZE / 4096);
	mm_memset(pp, 0, sizeof(*pp));
}

/* Page tables for every directory entry the range touches; new ones are
 * allocated outside the lock (where memory can be reclaimed) and the
 * directory written into the global GTT. */
static int ppgtt_ensure_tables(struct i915_device *i915, uint32_t first, uint32_t npages)
{
	struct leg_ppgtt *pp = &g_leg.ppgtt;
	uint32_t pde_first = first / GEN6_PTES;
	uint32_t pde_last = (first + npages - 1) / GEN6_PTES;
	int changed = 0;

	if (pde_last >= GEN6_PDES)
		return -EINVAL;
	for (uint32_t pde = pde_first; pde <= pde_last; pde++) {
		uint64_t fl;

		if (pp->pt_phys[pde])
			continue;
		uint64_t pt = mm_allocate_physical_page_reclaim();
		if (!pt)
			return -ENOMEM;
		uint32_t *v = phys_to_virt(pt);
		for (int i = 0; i < GEN6_PTES; i++)
			v[i] = pp->scratch_pte;
		pt_flush(i915, v, 4096);
		spin_lock_irqsave(&g_leg.gtt_lock, &fl);
		if (!pp->pt_phys[pde]) {
			pp->pt_phys[pde] = pt;
			leg_ggtt_write(i915, pp->pd_ggtt / 4096 + pde, gen6_pde_encode(pt));
			pt = 0;
			changed = 1;
		}
		spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
		if (pt)
			mm_free_physical_page(pt);
	}
	if (changed)
		leg_ggtt_invalidate(i915);
	return 0;
}

static void ppgtt_write(struct i915_device *i915, uint32_t first, const uint64_t *pages,
			uint32_t npages, enum leg_cache level)
{
	struct leg_ppgtt *pp = &g_leg.ppgtt;
	uint32_t i = 0;

	while (i < npages) {
		uint32_t pde = (first + i) / GEN6_PTES;
		uint32_t pte = (first + i) % GEN6_PTES;
		uint32_t *t = phys_to_virt(pp->pt_phys[pde]);
		uint32_t from = pte;

		for (; i < npages && pte < GEN6_PTES; i++, pte++)
			t[pte] = pages ? leg_pte_for_page(i915, pages[i], level) : pp->scratch_pte;
		pt_flush(i915, &t[from], (pte - from) * 4);
	}
}

/* ---- the shared address space ----------------------------------------------------------- */

struct i915_vm *i915_legacy_vm_create(struct i915_device *i915)
{
	(void)i915;
	if (!g_leg.vm)
		return NULL;
	i915_vm_get(g_leg.vm);
	return g_leg.vm;
}

/* `low' is 1 for an object that must stay below 4 GB (all of these
 * spaces are) and 2 for one that must sit behind the aperture (a fenced
 * object on gen2/3). */
int i915_legacy_vm_vma_alloc(struct i915_vm *vm, struct i915_vma *v, uint32_t npages,
			     uint64_t align, int low)
{
	struct i915_device *i915 = g_leg.i915;
	uint32_t align_pages = align > 4096 ? (uint32_t)(align / 4096) : 1u;
	uint32_t first = 0;
	uint64_t fl;
	int rc;

	if (align_pages & (align_pages - 1))
		return -EINVAL;
	spin_lock_irqsave(&vm->lock, &fl);
	if (v->attached)
		i915_vma_list_detach(&vm->vmas, v);
	spin_lock(&g_leg.gtt_lock);
	if (low == 2) {
		rc = ggtt_alloc_locked(i915, npages, align_pages, 0, mappable_pages(i915), 1, &first);
	} else {
		/* clients' objects above the aperture first: the
		 * aperture is for what the processor maps */
		rc = ggtt_alloc_locked(i915, npages, align_pages, mappable_pages(i915),
				       i915->ggtt_map_pages, 0, &first);
		if (rc)
			rc = ggtt_alloc_locked(i915, npages, align_pages, 0, mappable_pages(i915), 1,
					       &first);
	}
	if (rc == 0)
		for (uint32_t i = 0; i < npages; i++)
			map_set(g_leg.vm_owned, first + i, 1);
	spin_unlock(&g_leg.gtt_lock);
	if (rc == 0) {
		v->addr = (uint64_t)first * 4096;
		v->npages = npages;
		v->allocated = 1;
		i915_vma_list_attach(&vm->vmas, v);
	}
	spin_unlock_irqrestore(&vm->lock, fl);
	return rc;
}

int i915_legacy_vm_bind(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
			uint32_t npages, int uncached)
{
	struct i915_device *i915 = g_leg.i915;
	uint32_t first = (uint32_t)(addr / 4096);
	enum leg_cache level = leg_cache_of(i915, NULL, uncached);
	uint64_t fl;
	int rc = 0;

	if ((addr & 0xfff) || !npages || addr + (uint64_t)npages * 4096 > i915->ggtt_bytes ||
	    first + npages > i915->ggtt_map_pages)
		return -EINVAL;
	/* the range is the shared space's, or free: a client may place an
	 * object itself, but not over the driver's own bindings */
	spin_lock_irqsave(&g_leg.gtt_lock, &fl);
	for (uint32_t i = 0; i < npages; i++) {
		uint32_t p = first + i;
		if (p < i915->ggtt_map_first ||
		    (map_used(i915, p) && !((g_leg.vm_owned[p / 8] >> (p % 8)) & 1))) {
			rc = -ENOSPC;
			break;
		}
	}
	if (rc == 0)
		for (uint32_t i = 0; i < npages; i++) {
			map_set(i915->ggtt_map, first + i, 1);
			map_set(g_leg.vm_owned, first + i, 1);
		}
	spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
	if (rc)
		return rc;
	if (g_leg.ppgtt.present) {
		rc = ppgtt_ensure_tables(i915, first, npages);
		if (rc)
			return rc;
		ppgtt_write(i915, first, pages, npages, level);
	} else {
		bind_pages(i915, first, pages, npages, level);
	}
	vm->tlb_dirty = 1;
	return 0;
}

void i915_legacy_vm_unbind(struct i915_vm *vm, uint64_t addr, uint32_t npages)
{
	struct i915_device *i915 = g_leg.i915;
	uint32_t first = (uint32_t)(addr / 4096);
	uint64_t fl;

	if (first >= i915->ggtt_map_pages)
		return;
	if (first + npages > i915->ggtt_map_pages)
		npages = i915->ggtt_map_pages - first;
	if (g_leg.ppgtt.present) {
		/* only the tables that exist: an absent one leads to
		 * scratch already */
		for (uint32_t i = 0; i < npages; i++) {
			uint32_t pde = (first + i) / GEN6_PTES;
			if (g_leg.ppgtt.pt_phys[pde])
				ppgtt_write(i915, first + i, NULL, 1, LEG_CACHE_NONE);
		}
	} else {
		for (uint32_t i = 0; i < npages; i++)
			leg_ggtt_write(i915, first + i, g_leg.scratch_pte);
		leg_ggtt_invalidate(i915);
	}
	spin_lock_irqsave(&g_leg.gtt_lock, &fl);
	for (uint32_t i = 0; i < npages; i++) {
		uint32_t p = first + i;
		if ((g_leg.vm_owned[p / 8] >> (p % 8)) & 1) {
			map_set(g_leg.vm_owned, p, 0);
			map_set(i915->ggtt_map, p, 0);
		}
	}
	spin_unlock_irqrestore(&g_leg.gtt_lock, fl);
	vm->tlb_dirty = 1;
}

uint64_t i915_legacy_vm_lookup(struct i915_vm *vm, uint64_t addr)
{
	struct i915_device *i915 = g_leg.i915;
	uint32_t page = (uint32_t)(addr / 4096);
	uint64_t phys;

	(void)vm;
	if (page >= g_leg.gtt_entries)
		return 0;
	if (g_leg.ppgtt.present) {
		uint32_t pde = page / GEN6_PTES;
		if (pde >= GEN6_PDES || !g_leg.ppgtt.pt_phys[pde])
			return 0;
		phys = leg_pte_decode(((uint32_t *)phys_to_virt(g_leg.ppgtt.pt_phys[pde]))
					      [page % GEN6_PTES]);
	} else {
		phys = leg_pte_decode(((volatile uint32_t *)i915->gtt_virt)[page]);
	}
	if (!phys || phys == g_leg.scratch_phys)
		return 0;
	return phys | (addr & 0xfff);
}

/* ---- what the firmware left scanning out ------------------------------------------------ */

static uint64_t boot_scanout_end(struct i915_device *i915)
{
	int npipes = i915->info->num_pipes ? i915->info->num_pipes : 2;
	uint32_t disp = leg_is(i915, I915_PLATFORM_VALLEYVIEW) ? VLV_DISPLAY_BASE : 0;

	i915->boot_scanout.pipe = -1;
	for (int pipe = 0; pipe < npipes && pipe < 3; pipe++) {
		uint32_t conf = i915_read32(i915, disp + LEG_PIPE_REG(_PIPEACONF, pipe));
		uint32_t ctl = i915_read32(i915, disp + LEG_PIPE_REG(_DSPACNTR, pipe));
		uint32_t src = i915_read32(i915, disp + LEG_PIPE_REG(_PIPEASRC, pipe));
		if (!(conf & PIPECONF_ENABLE) || !(ctl & DISPLAY_PLANE_ENABLE))
			continue;
		uint32_t surf = i915_read32(i915, disp + LEG_PIPE_REG(leg_gen(i915) >= 4 ? _DSPASURF :
										 _DSPAADDR, pipe));
		uint32_t stride = i915_read32(i915, disp + LEG_PIPE_REG(_DSPASTRIDE, pipe));
		i915->boot_scanout.pipe = pipe;
		i915->boot_scanout.plane_ctl = ctl;
		i915->boot_scanout.surf = surf;
		i915->boot_scanout.stride = stride;
		i915->boot_scanout.size = src;
		i915_dbg("[drm] i915: firmware scanout on pipe %c: %ux%u at %08x, stride %u\n",
			 'A' + pipe, (src >> 16 & 0xfff) + 1, (src & 0xfff) + 1, surf, stride);
		return (uint64_t)(surf & ~0xfffu) + (uint64_t)(stride & 0x1ffc0) * ((src & 0xfff) + 1);
	}
	return 0;
}

/* ---- probe -------------------------------------------------------------------------------- */

/* gen2-5: the GTT switched on (and its control word kept for resume). */
static int leg_enable_gtt(struct i915_device *i915)
{
	int gen = leg_gen(i915);

	if (gen >= 6)
		return 0;
	if (gen == 2) {
		uint16_t ctl = hb_read16(I830_GMCH_CTRL);
		hb_write16(I830_GMCH_CTRL, ctl | I830_GMCH_ENABLED);
		if (!(hb_read16(I830_GMCH_CTRL) & I830_GMCH_ENABLED)) {
			kprintf("[drm] i915: cannot enable the GTT (GMCH_CTRL %04x)\n",
				hb_read16(I830_GMCH_CTRL));
			return -EIO;
		}
	}
	/* the chipset's write buffers flushed around the control word */
	if (gen >= 3)
		i915_write32(i915, GFX_FLSH_CNTL, 0);
	i915_write32(i915, I810_PGETBL_CTL, g_leg.pgetbl_save);
	if (has_pgtbl_enable(i915) && !(i915_read32(i915, I810_PGETBL_CTL) & I810_PGETBL_ENABLED)) {
		kprintf("[drm] i915: cannot enable the GTT (PGETBL %08x, wanted %08x)\n",
			i915_read32(i915, I810_PGETBL_CTL), g_leg.pgetbl_save);
		return -EIO;
	}
	if (gen >= 3)
		i915_write32(i915, GFX_FLSH_CNTL, 0);
	return 0;
}

static void clear_unreserved(struct i915_device *i915)
{
	for (uint32_t p = i915->ggtt_map_first; p < g_leg.gtt_entries; p++)
		leg_ggtt_write(i915, p, g_leg.scratch_pte);
	leg_ggtt_invalidate(i915);
}

static uint64_t scratch_page_alloc(void)
{
	/* below every part's limit, even gen2's 4 GB */
	for (int tries = 0; tries < 64; tries++) {
		uint64_t p = mm_allocate_physical_page();
		if (!p)
			return 0;
		if (p + 4096 <= (1ULL << 32))
			return p;
		mm_free_physical_page(p);
	}
	return 0;
}

int i915_legacy_gtt_probe(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	int gen = leg_gen(i915);
	uint64_t aperture = i915->bar_aperture.size;
	uint32_t entries;
	int own_map = 1;

	spinlock_init(&g_leg.gtt_lock, "i915_ggtt");
	leg_stolen(i915);

	switch (gen) {
	case 2:
		/* the aperture's size is the host bridge's word, the GTT
		 * covers exactly it and lives in the register window */
		aperture = (hb_read16(I830_GMCH_CTRL) & I830_GMCH_MEM_MASK) == I830_GMCH_MEM_64M ?
				   MB(64) :
				   MB(128);
		entries = (uint32_t)(aperture / 4096);
		g_leg.gtt_phys = i915->bar_mmio.base + I810_PTE_BASE;
		i915->gtt_virt = i915->mmio_virt + I810_PTE_BASE;
		own_map = 0;
		g_leg.pte_kind = LEG_PTE_I830;
		break;
	case 3: {
		struct pci_bar gtt;
		if (pci_bar_decode(i915->pci, GEN3_GTTADR_BAR, &gtt) != 0) {
			kprintf("[drm] i915: no GTT BAR\n");
			return -ENODEV;
		}
		g_leg.gtt_phys = gtt.base;
		entries = g_leg.is_g33 ? i965_gtt_total_entries(i915) : (uint32_t)(aperture / 4096);
		g_leg.pte_kind = g_leg.is_g33 ? LEG_PTE_I965 : LEG_PTE_I830;
		break;
	}
	case 4:
	case 5:
		g_leg.gtt_phys = i915->bar_mmio.base + ((g_leg.is_g4x || gen == 5) ? MB(2) : KB(512));
		entries = i965_gtt_total_entries(i915);
		g_leg.pte_kind = LEG_PTE_I965;
		break;
	default: {
		uint16_t ctl = pci_cfg_read16(i915->pci, SNB_GMCH_CTRL);
		uint32_t size = (uint32_t)((ctl >> SNB_GMCH_GGMS_SHIFT) & SNB_GMCH_GGMS_MASK) << 20;
		if (aperture < MB(64) || aperture > MB(512)) {
			kprintf("[drm] i915: unknown aperture size (%llu MB)\n",
				(unsigned long long)(aperture >> 20));
			return -ENXIO;
		}
		if (!size) {
			kprintf("[drm] i915: SNB_GMCH_CTRL %04x names no GTT\n", ctl);
			return -ENODEV;
		}
		entries = size / 4;
		g_leg.gtt_phys = i915->bar_mmio.base + MB(2);
		if (info->platform == I915_PLATFORM_HASWELL) {
			g_leg.has_edram = i915_read32(i915, HSW_EDRAM_CAP) & EDRAM_ENABLED;
			g_leg.pte_kind = g_leg.has_edram ? LEG_PTE_IRIS : LEG_PTE_HSW;
		} else if (info->platform == I915_PLATFORM_VALLEYVIEW) {
			g_leg.pte_kind = LEG_PTE_BYT;
		} else if (gen == 7) {
			g_leg.pte_kind = LEG_PTE_IVB;
		} else {
			g_leg.pte_kind = LEG_PTE_SNB;
		}
		break;
	}
	}
	/* more than 32 bits of global space was never built */
	if ((uint64_t)entries * 4096 > (1ULL << 32))
		entries = (uint32_t)((1ULL << 32) / 4096);
	g_leg.gtt_entries = entries;
	i915->gtt_size = (uint64_t)entries * 4;
	i915->ggtt_bytes = (uint64_t)entries * 4096;
	g_leg.mappable_end = aperture < i915->ggtt_bytes ? aperture : i915->ggtt_bytes;
	if (own_map) {
		i915->gtt_virt = mm_map_mmio_flags(g_leg.gtt_phys, (i915->gtt_size + 4095) / 4096,
						   MM_MMIO_UC);
		if (!i915->gtt_virt) {
			kprintf("[drm] i915: cannot map the GTT at %llx\n",
				(unsigned long long)g_leg.gtt_phys);
			return -ENOMEM;
		}
	}

	/* the page unbound entries point at */
	if (!g_leg.scratch_phys) {
		g_leg.scratch_phys = scratch_page_alloc();
		if (!g_leg.scratch_phys) {
			i915_legacy_gtt_fini(i915);
			return -ENOMEM;
		}
		mm_memset(phys_to_virt(g_leg.scratch_phys), 0, 4096);
	}
	i915->ggtt_scratch_phys = g_leg.scratch_phys;
	g_leg.scratch_pte = leg_pte_encode(i915, g_leg.scratch_phys, LEG_CACHE_NONE, 1);

	if (gen <= 5) {
		g_leg.pgetbl_save = i915_read32(i915, I810_PGETBL_CTL) & ~I810_PGETBL_ENABLED;
		if (has_pgtbl_enable(i915))
			g_leg.pgetbl_save |= I810_PGETBL_ENABLED;
		if (leg_enable_gtt(i915) != 0) {
			i915_legacy_gtt_fini(i915);
			return -EIO;
		}
		leg_setup_flush_page(i915);
	}

	/* What stays as the firmware left it: its framebuffer, or, when
	 * nothing is scanning out, a slice of the stolen range it may still
	 * be using. */
	uint64_t reserved = boot_scanout_end(i915);
	if (!reserved) {
		reserved = i915->stolen_size;
		if (reserved > g_leg.mappable_end / 4)
			reserved = g_leg.mappable_end / 4;
	}
	if (reserved < KB(64))
		reserved = KB(64);
	reserved = (reserved + KB(256) - 1) & ~(KB(256) - 1);
	if (reserved > i915->ggtt_bytes / 2)
		reserved = i915->ggtt_bytes / 2;

	if (!i915->ggtt_map) {
		i915->ggtt_map_pages = entries;
		i915->ggtt_map = kalloc(entries / 8 + 1);
		g_leg.vm_owned = kalloc(entries / 8 + 1);
		if (!i915->ggtt_map || !g_leg.vm_owned) {
			i915_legacy_gtt_fini(i915);
			return -ENOMEM;
		}
		mm_memset(i915->ggtt_map, 0, entries / 8 + 1);
		mm_memset(g_leg.vm_owned, 0, entries / 8 + 1);
	}
	i915->ggtt_map_first = (uint32_t)(reserved / 4096);
	clear_unreserved(i915);

	/* the space every context runs in */
	if (!g_leg.vm) {
		struct i915_vm *vm = kalloc(sizeof(*vm));
		if (!vm) {
			i915_legacy_gtt_fini(i915);
			return -ENOMEM;
		}
		mm_memset(vm, 0, sizeof(*vm));
		vm->refs = 1; /* the driver's own, until fini */
		spinlock_init(&vm->lock, "i915_vm");
		g_leg.vm = vm;
	}
	if (gen >= 6 && !g_leg.ppgtt.present && ppgtt_init(i915) != 0) {
		kprintf("[drm] i915: no room for the PPGTT directory\n");
		i915_legacy_gtt_fini(i915);
		return -ENOMEM;
	}

	kprintf("[drm] i915: GTT %llu KB (%llu MB of graphics address space, %llu MB behind the aperture), stolen %llu MB at %llx%s\n",
		(unsigned long long)(i915->gtt_size >> 10),
		(unsigned long long)(i915->ggtt_bytes >> 20),
		(unsigned long long)(g_leg.mappable_end >> 20),
		(unsigned long long)(i915->stolen_size >> 20),
		(unsigned long long)i915->stolen_base, i915->stolen_size ? "" : " (none)");
	return 0;
}

void i915_legacy_gtt_fini(struct i915_device *i915)
{
	ppgtt_fini(i915);
	if (g_leg.vm && g_leg.vm->refs <= 1) {
		kfree(g_leg.vm);
		g_leg.vm = NULL;
	}
	if (i915->gtt_virt) {
		if (leg_gen(i915) != 2)
			mm_unmap_mmio(i915->gtt_virt, (i915->gtt_size + 4095) / 4096);
		i915->gtt_virt = 0;
	}
}

/* After a resume: the GTT switched on again and every entry the driver
 * owns written back -- its own bindings, the aperture mappings, the
 * shared space (gen2-5) or the PPGTT directory (Gen6/7). */
void i915_legacy_ggtt_rewrite_all(struct i915_device *i915)
{
	struct drm_device *dev = &i915->drm;
	unsigned n = 0;

	if (!i915->gtt_virt)
		return;
	if (leg_gen(i915) <= 5)
		(void)leg_enable_gtt(i915);
	clear_unreserved(i915);
	for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
		struct i915_bo *bo = o->priv;
		if (!bo || !o->pages)
			continue;
		if (bo->bound) {
			uint32_t first = bo->ggtt / 4096;
			enum leg_cache level = leg_cache_of(i915, o, bo->ggtt_uncached);
			for (uint32_t i = 0; i < o->npages; i++)
				leg_ggtt_write(i915, first + i, leg_pte_for_page(i915, o->pages[i], level));
			n++;
		}
		if (bo->gtt_map_bound && bo->gtt_map_own) {
			uint32_t first = bo->gtt_map_ggtt / 4096;
			for (uint32_t i = 0; i < o->npages; i++)
				leg_ggtt_write(i915, first + i,
					       leg_pte_for_page(i915, o->pages[i], LEG_CACHE_NONE));
			n++;
		}
		if (g_leg.ppgtt.present)
			continue;
		for (struct i915_vma *v = bo->vmas; v; v = v->next) {
			if (v->vm != g_leg.vm || !v->attached)
				continue;
			enum leg_cache level =
				leg_cache_of(i915, o, bo->caching == I915_CACHING_NONE);
			uint32_t first = (uint32_t)(v->addr / 4096);
			for (uint32_t i = 0; i < o->npages && i < v->npages; i++)
				leg_ggtt_write(i915, first + i, leg_pte_for_page(i915, o->pages[i], level));
			n++;
		}
	}
	leg_ggtt_invalidate(i915);
	leg_ppgtt_rewrite_pd(i915);
	i915_dbg("[drm] i915: %u global bindings written again\n", n);
}
