// LikeOS -- the global GTT, stolen memory, and what the fuses say.
//
// The graphics function's config space says how much memory the firmware
// stole for the device (the framebuffer it set up lives there) and how
// large the global GTT is; BAR0's upper half is the GTT itself, one 8-byte
// entry per 4 KB page of graphics address space.  This file decodes those
// and reads the fuse registers that describe the render array.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* Stolen memory size from the GGC GMS field.  Gen8 counts in 32 MB
 * units; Gen9 and later use 32 MB units up to 0xef and 4 MB units from
 * 0xf0 (so that a firmware can leave small amounts). */
static uint64_t stolen_size_from_gms(const struct intel_device_info *info,
				     uint32_t gms)
{
	if (info->gen == 8)
		return (uint64_t)gms << 25;
	if (gms < 0xf0)
		return (uint64_t)gms << 25;
	return (uint64_t)(gms - 0xf0 + 1) << 22;
}

int i915_gtt_probe(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	uint16_t ggc = pci_cfg_read16(i915->pci, I915_PCI_GGC);
	uint32_t ggms = (ggc >> GGC_GGMS_SHIFT) & GGC_GGMS_MASK;
	uint32_t gms = (ggc >> GGC_GMS_SHIFT) & GGC_GMS_MASK;

	/* GTT size: 2 MB << (ggms-1) of entries on Gen8-12 (1=2MB, 2=4MB,
	 * 3=8MB); each entry maps a page, so the address space is 512x. */
	if (ggms == 0 || ggms > 3) {
		kprintf("[drm] i915: GGC %04x names no GTT (GGMS=%u)\n", ggc,
			ggms);
		return -ENODEV;
	}
	i915->gtt_size = (uint64_t)1 << (20 + ggms);
	i915->ggtt_bytes = i915->gtt_size / 8 * 4096;
	i915->stolen_size = stolen_size_from_gms(info, gms);
	i915->stolen_base = pci_cfg_read32_dev(i915->pci, I915_PCI_BSM) &
			    0xFFF00000u;
	if (info->gen <= 9)
		i915->gsm_base = pci_cfg_read32_dev(i915->pci, I915_PCI_BGSM) &
				 0xFFF00000u;

	/* The GTT is the upper half of the register window; on the parts
	 * driven here BAR0 is 16 MB and the GTT never exceeds 8 MB. */
	if (i915->bar_mmio.size < 2 * i915->gtt_size) {
		kprintf("[drm] i915: BAR0 (%llu MB) too small for a %llu MB GTT\n",
			(unsigned long long)(i915->bar_mmio.size >> 20),
			(unsigned long long)(i915->gtt_size >> 20));
		return -ENODEV;
	}
	uint64_t gtt_phys = i915->bar_mmio.base + i915->bar_mmio.size / 2;
	i915->gtt_virt = mm_map_mmio_flags(gtt_phys, i915->gtt_size / 4096,
					   MM_MMIO_UC);
	if (!i915->gtt_virt) {
		kprintf("[drm] i915: cannot map the GTT at %llx\n",
			(unsigned long long)gtt_phys);
		return -ENOMEM;
	}

	/* A page for the entries the driver unbinds to point at: the
	 * engine keeps the last context it ran loaded and writes its image
	 * once more when the next one is loaded, and a stray access of that
	 * kind lands here instead of faulting. */
	if (!i915->ggtt_scratch_phys) {
		i915->ggtt_scratch_phys = mm_allocate_physical_page();
		if (i915->ggtt_scratch_phys)
			mm_memset(phys_to_virt(i915->ggtt_scratch_phys), 0, 4096);
	}
	kprintf("[drm] i915: GTT %llu MB (%llu MB of graphics address space), stolen %llu MB at %llx%s\n",
		(unsigned long long)(i915->gtt_size >> 20),
		(unsigned long long)(i915->ggtt_bytes >> 20),
		(unsigned long long)(i915->stolen_size >> 20),
		(unsigned long long)i915->stolen_base,
		i915->stolen_base ? "" : " (none)");

	/* What the firmware left scanning out.  The console will replace
	 * it, but until then that plane's surface and the stolen range it
	 * points into are not ours to touch. */
	i915->boot_scanout.pipe = -1;
	for (int pipe = 0; pipe < info->num_pipes && pipe < 4; pipe++) {
		uint32_t conf = i915_read32(i915, PIPECONF(pipe));
		uint32_t src = i915_read32(i915, PIPESRC(pipe));
		if (info->display_ver < 9) {
			/* Broadwell: the older plane registers */
			uint32_t ctl = i915_read32(i915, DSPCNTR(pipe));
			if (!(conf & PIPECONF_ENABLE) || !(ctl & DISP_ENABLE))
				continue;
			i915->boot_scanout.pipe = pipe;
			i915->boot_scanout.plane_ctl = ctl;
			i915->boot_scanout.surf = i915_read32(i915, DSPSURF(pipe));
			i915->boot_scanout.stride = i915_read32(i915, DSPSTRIDE(pipe));
			i915->boot_scanout.size = src;
			i915_dbg("[drm] i915: firmware scanout on pipe %c: %ux%u, plane surface %08x, ctl %08x\n",
				'A' + pipe, (src >> 16 & 0xfff) + 1, (src & 0xfff) + 1,
				i915->boot_scanout.surf, ctl);
			break;
		}
		uint32_t ctl = i915_read32(i915, PLANE_CTL(pipe, 0));
		if (!(conf & PIPECONF_ENABLE) || !(ctl & PLANE_CTL_ENABLE))
			continue;
		i915->boot_scanout.pipe = pipe;
		i915->boot_scanout.plane_ctl = ctl;
		i915->boot_scanout.surf = i915_read32(i915, PLANE_SURF(pipe, 0));
		i915->boot_scanout.stride =
			i915_read32(i915, PLANE_STRIDE(pipe, 0));
		i915->boot_scanout.size = i915_read32(i915, PLANE_SIZE(pipe, 0));
		i915_dbg("[drm] i915: firmware scanout on pipe %c: %ux%u, plane surface %08x, ctl %08x\n",
			'A' + pipe, (src >> 16 & 0xfff) + 1, (src & 0xfff) + 1,
			i915->boot_scanout.surf, ctl);
		break;
	}
	return 0;
}

void i915_gtt_fini(struct i915_device *i915)
{
	if (i915->gtt_virt) {
		mm_unmap_mmio(i915->gtt_virt, i915->gtt_size / 4096);
		i915->gtt_virt = 0;
	}
}

/* ---- topology --------------------------------------------------------------- */

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	while (v) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

void i915_read_topology(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	uint32_t eu_total = 0;

	i915->slice_mask = 0;
	for (int s = 0; s < 4; s++)
		i915->subslice_mask[s] = 0;

	if (info->gen == 8) {
		uint32_t fuse2 = i915_read32(i915, GEN8_FUSE2);
		uint32_t ss_dis = (fuse2 & GEN8_F2_SS_DIS_MASK) >> GEN8_F2_SS_DIS_SHIFT;
		i915->slice_mask = (fuse2 & GEN8_F2_S_ENA_MASK) >> GEN8_F2_S_ENA_SHIFT;
		uint32_t eu_dis[3] = { i915_read32(i915, GEN8_EU_DISABLE0),
				       i915_read32(i915, GEN8_EU_DISABLE1),
				       i915_read32(i915, GEN8_EU_DISABLE2) };
		for (int s = 0; s < 3; s++) {
			if (!(i915->slice_mask & (1 << s)))
				continue;
			i915->subslice_mask[s] = (~ss_dis) & 0x7;
			for (int ss = 0; ss < 3; ss++) {
				if (!(i915->subslice_mask[s] & (1 << ss)))
					continue;
				/* 8 EUs per subslice, disable bits packed
				 * 24 per slice across three registers */
				unsigned bit = s * 24 + ss * 8;
				uint32_t d = (eu_dis[bit / 32] >> (bit % 32)) & 0xff;
				eu_total += 8 - popcount32(d);
			}
		}
	} else if (info->gen <= 10) {
		uint32_t fuse2 = i915_read32(i915, GEN8_FUSE2);
		uint32_t ss_dis = (fuse2 & GEN9_F2_SS_DIS_MASK) >> GEN9_F2_SS_DIS_SHIFT;
		i915->slice_mask = (fuse2 & GEN8_F2_S_ENA_MASK) >> GEN8_F2_S_ENA_SHIFT;
		for (int s = 0; s < 3; s++) {
			if (!(i915->slice_mask & (1 << s)))
				continue;
			i915->subslice_mask[s] = (~ss_dis) & 0xf;
			uint32_t eu_dis = i915_read32(i915, GEN9_EU_DISABLE(s));
			for (int ss = 0; ss < 4; ss++) {
				if (!(i915->subslice_mask[s] & (1 << ss)))
					continue;
				uint32_t d = (eu_dis >> (ss * 8)) & 0xff;
				eu_total += 8 - popcount32(d);
			}
		}
	} else {
		/* Gen11/12: one slice-enable word, one subslice-disable
		 * word (8 per slice), EU disable per pair of subslices. */
		uint32_t s_en = i915_read32(i915, GEN11_GT_SLICE_ENABLE) &
				GEN11_GT_S_ENA_MASK;
		uint32_t ss_dis = i915_read32(i915, info->gen_x10 >= 120 ?
							     GEN12_GT_GEOMETRY_DSS_ENABLE :
							     GEN11_GT_SUBSLICE_DISABLE);
		uint32_t eu_dis = i915_read32(i915, GEN11_EU_DISABLE) &
				  GEN11_EU_DIS_MASK;
		unsigned eus_per_ss = 8;
		i915->slice_mask = s_en;
		for (int s = 0; s < 4; s++) {
			if (!(s_en & (1 << s)))
				continue;
			uint32_t ss = info->gen_x10 >= 120 ?
					      (ss_dis >> (s * 8)) & 0xff :
					      (~ss_dis >> (s * 8)) & 0xff;
			i915->subslice_mask[s] = ss;
			eu_total += popcount32(ss) *
				    (eus_per_ss - popcount32(eu_dis));
		}
		if (info->gen_x10 >= 120)
			eu_total = 0;
		for (int s = 0; s < 4 && info->gen_x10 >= 120; s++)
			eu_total += popcount32(i915->subslice_mask[s]) *
				    (16 - 2 * popcount32(eu_dis));
	}
	i915->eu_total = eu_total;
}

/* The command streamer's timestamp frequency, which userspace uses to
 * turn GPU timestamps into time. */
uint32_t i915_timestamp_hz(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	if (info->gen <= 9)
		return (info->flags & I915_INFO_IS_LP) ? 19200000u : 12000000u;

	uint32_t ctc = i915_read32(i915, CTC_MODE);
	uint32_t base;
	if (!(ctc & CTC_SOURCE_DIVIDE_LOGIC)) {
		/* the crystal, per RPM_CONFIG0 */
		uint32_t rpm = i915_read32(i915, RPM_CONFIG0);
		uint32_t f, shift;
		if (info->gen == 10) {
			f = (rpm & GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK) >>
			    GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT;
			base = f ? 19200000u : 24000000u;
		} else {
			f = (rpm & GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK) >>
			    GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT;
			switch (f) {
			case 0:
				base = 24000000u;
				break;
			case 1:
				base = 19200000u;
				break;
			case 2:
				base = 38400000u;
				break;
			default:
				base = 25000000u;
				break;
			}
		}
		shift = (rpm & GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK) >>
			GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT;
		return base >> (3 - shift);
	}
	/* divide-logic: 12 MHz reference stepped down */
	uint32_t shift = (ctc & CTC_SHIFT_PARAMETER_MASK) >>
			 CTC_SHIFT_PARAMETER_SHIFT;
	return 12000000u >> (3 - shift);
}

/* ---- the global GTT's entries ------------------------------------------------ */

/* Gen8-11 page table entries: address, present, writable, and the
 * private PAT index in the PWT/PCD/PAT bit positions.  The PPAT table
 * the driver programs puts uncached at index 3 (PWT|PCD) and
 * write-back at 0; Gen12 selects a PAT table entry by index bits. */
#define GEN8_PTE_PRESENT (1ULL << 0)
#define GEN8_PTE_RW (1ULL << 1)
#define GEN8_PTE_PWT (1ULL << 3)
#define GEN8_PTE_PCD (1ULL << 4)
#define GEN8_PTE_PAT (1ULL << 7)
#define GEN12_GGTT_PTE_LM (1ULL << 1)

static uint64_t ggtt_pte_encode(struct i915_device *i915, uint64_t phys,
				int uncached)
{
	uint64_t pte = (phys & 0x0000FFFFFFFFF000ULL) | GEN8_PTE_PRESENT;
	if (i915->info->gen_x10 >= 120) {
		/* PAT index 3 = UC in the table programmed below; bits are
		 * PAT[0] at 7 (Gen12: bit 7 = index bit 0, bit 3 = index bit 1?)
		 * -- Gen12 GGTT uses bits 3:2 for the index on TGL/ADL. */
		if (uncached)
			pte |= (3ULL << 2);
		return pte;
	}
	pte |= GEN8_PTE_RW;
	if (uncached)
		pte |= GEN8_PTE_PWT | GEN8_PTE_PCD; /* PPAT index 3 */
	return pte;
}

static void ggtt_write_pte(struct i915_device *i915, uint32_t index,
			   uint64_t pte)
{
	volatile uint64_t *e = (volatile uint64_t *)(i915->gtt_virt +
						     (uint64_t)index * 8);
	*e = pte;
}

static void ggtt_flush(struct i915_device *i915)
{
	/* A posting read of the last entry, then the flush control. */
	(void)*(volatile uint64_t *)i915->gtt_virt;
	i915_write32_fw(i915, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
	(void)i915_read32_fw(i915, GFX_FLSH_CNTL_GEN6);
}

/* The private PAT: index 0 write-back (LLC), 1 write-combining, 2
 * write-through, 3 uncached, 4-7 write-back with age hints. */
#define GEN8_PPAT_UC 0
#define GEN8_PPAT_WC 1
#define GEN8_PPAT_WT 2
#define GEN8_PPAT_WB 3
#define GEN8_PPAT_LLC (1 << 2)
#define GEN8_PPAT_LLCELLC (2 << 2)
#define GEN8_PPAT_LLCELLC_ALT (3 << 2)
#define GEN8_PPAT_AGE(x) ((x) << 4)
#define GEN8_PPAT(i, v) ((uint64_t)(v) << ((i) * 8))

void i915_ggtt_program_pat(struct i915_device *i915)
{
	if (i915->info->gen_x10 >= 120) {
		/* Gen12: a PAT register per index */
		static const uint32_t pat[8] = { 0 /* WB */, 1 /* WC */,
						 2 /* WT */, 3 /* UC */,
						 0, 0, 0, 0 };
		for (int i = 0; i < 8; i++)
			i915_write32(i915, GEN12_PAT_INDEX(i), pat[i]);
		return;
	}
	uint64_t pat = GEN8_PPAT(0, GEN8_PPAT_WB | GEN8_PPAT_LLC) |
		       GEN8_PPAT(1, GEN8_PPAT_WC | GEN8_PPAT_LLCELLC) |
		       GEN8_PPAT(2, GEN8_PPAT_WT | GEN8_PPAT_LLCELLC) |
		       GEN8_PPAT(3, GEN8_PPAT_UC) |
		       GEN8_PPAT(4, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(0)) |
		       GEN8_PPAT(5, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(1)) |
		       GEN8_PPAT(6, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(2)) |
		       GEN8_PPAT(7, GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(3));
	i915_write32(i915, GEN8_PRIVATE_PAT_LO, (uint32_t)pat);
	i915_write32(i915, GEN8_PRIVATE_PAT_HI, (uint32_t)(pat >> 32));
	/* Every translation names an entry of this table; if the write did
	 * not take, the engines cache nothing the CPU wrote and read stale
	 * memory for every small upload. */
	uint32_t lo = i915_read32(i915, GEN8_PRIVATE_PAT_LO);
	uint32_t hi = i915_read32(i915, GEN8_PRIVATE_PAT_HI);
	if (lo != (uint32_t)pat || hi != (uint32_t)(pat >> 32))
		kprintf("[drm] i915: page attribute table did not take: wrote %08x %08x, read %08x %08x\n",
			(uint32_t)(pat >> 32), (uint32_t)pat, hi, lo);
	else
		i915_dbg("[drm] i915: page attribute table %08x %08x\n", hi, lo);
}

/* Global address space, handed out from the top of the part below 4 GB
 * downwards (the display engine's surface registers are 32-bit) and
 * above the firmware's stolen range, which its framebuffer lives in.
 * One bit per page; a range is given back when its object is unbound,
 * so a session that creates and destroys contexts and framebuffers all
 * day never runs the space dry.  Scanout surfaces are 256 KB aligned,
 * as the display engine wants; everything else is page aligned. */
#define SCANOUT_ALIGN_PAGES 64u

static int ggtt_map_init(struct i915_device *i915)
{
	uint64_t top = i915->ggtt_bytes;

	if (i915->ggtt_map)
		return 0;
	if (top > 0x100000000ULL)
		top = 0x100000000ULL;
	i915->ggtt_map_pages = (uint32_t)(top / 4096);
	i915->ggtt_map_first = (uint32_t)((i915->stolen_size + (64u << 20)) / 4096);
	i915->ggtt_map = kalloc(i915->ggtt_map_pages / 8 + 1);
	if (!i915->ggtt_map)
		return -ENOMEM;
	mm_memset(i915->ggtt_map, 0, i915->ggtt_map_pages / 8 + 1);
	return 0;
}

static inline int ggtt_map_used(const struct i915_device *i915, uint32_t page)
{
	return (i915->ggtt_map[page / 8] >> (page % 8)) & 1;
}

static inline void ggtt_map_set(struct i915_device *i915, uint32_t page, int used)
{
	if (used)
		i915->ggtt_map[page / 8] |= (uint8_t)(1u << (page % 8));
	else
		i915->ggtt_map[page / 8] &= (uint8_t)~(1u << (page % 8));
}

/* The highest free run of `npages' pages aligned to `align' pages, as a
 * page number, or 0 when there is none. */
static uint32_t ggtt_map_find(struct i915_device *i915, uint32_t npages, uint32_t align)
{
	if (!npages || npages > i915->ggtt_map_pages)
		return 0;
	uint32_t start = (i915->ggtt_map_pages - npages) & ~(align - 1);
	for (;; start -= align) {
		if (start < i915->ggtt_map_first)
			return 0;
		uint32_t i = 0;
		for (; i < npages; i++)
			if (ggtt_map_used(i915, start + i))
				break;
		if (i == npages)
			return start;
		if (start < align)
			return 0;
	}
}

int i915_ggtt_bind_obj(struct i915_device *i915, struct drm_gem_object *o,
		       int uncached, uint32_t *ggtt_offset);

int i915_ggtt_bind_scanout(struct i915_device *i915, struct drm_gem_object *o,
			   uint32_t *ggtt_offset)
{
	return i915_ggtt_bind_obj(i915, o, 1, ggtt_offset);
}

int i915_ggtt_bind_obj(struct i915_device *i915, struct drm_gem_object *o,
		       int uncached, uint32_t *ggtt_offset)
{
	if (!i915->gtt_virt || !o->pages || !o->npages)
		return -EINVAL;
	if (ggtt_map_init(i915) != 0)
		return -ENOMEM;
	uint32_t start = ggtt_map_find(i915, o->npages,
				       uncached ? SCANOUT_ALIGN_PAGES : 1u);
	if (!start) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: no room in the global address space for %u pages\n",
				o->npages);
		}
		return -ENOSPC;
	}
	for (uint32_t i = 0; i < o->npages; i++) {
		ggtt_map_set(i915, start + i, 1);
		ggtt_write_pte(i915, start + i,
			       ggtt_pte_encode(i915, o->pages[i], uncached));
	}
	ggtt_flush(i915);
	*ggtt_offset = start * 4096;
	if (o->priv)
		((struct i915_bo *)o->priv)->ggtt_uncached = uncached;
	return 0;
}

void i915_ggtt_rewrite_all(struct i915_device *i915)
{
	struct drm_device *dev = &i915->drm;
	unsigned n = 0;

	if (!i915->gtt_virt)
		return;
	for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
		struct i915_bo *bo = o->priv;
		if (!bo || !bo->bound || !o->pages)
			continue;
		uint32_t first = bo->ggtt / 4096;
		for (uint32_t i = 0; i < o->npages; i++)
			ggtt_write_pte(i915, first + i,
				       ggtt_pte_encode(i915, o->pages[i], bo->ggtt_uncached));
		n++;
	}
	ggtt_flush(i915);
	i915_dbg("[drm] i915: %u global bindings written again\n", n);
}

void i915_ggtt_unbind(struct i915_device *i915, uint32_t ggtt_offset,
		      uint32_t size)
{
	if (!i915->gtt_virt)
		return;
	/* Point at the scratch page; without one a cleared entry is
	 * unmapped and any late access of the engine's faults. */
	uint32_t first = ggtt_offset / 4096;
	uint32_t n = (size + 4095) / 4096;
	uint64_t pte = i915->ggtt_scratch_phys ?
			       ggtt_pte_encode(i915, i915->ggtt_scratch_phys, 1) : 0;
	for (uint32_t i = 0; i < n; i++) {
		ggtt_write_pte(i915, first + i, pte);
		if (i915->ggtt_map && first + i < i915->ggtt_map_pages)
			ggtt_map_set(i915, first + i, 0);
	}
	ggtt_flush(i915);
}
