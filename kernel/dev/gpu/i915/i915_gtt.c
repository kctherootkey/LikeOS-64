// LikeOS -- the global GTT, stolen memory, and what the fuses say.
//
// The graphics function's config space says how much memory the firmware
// stole for the device (the framebuffer it set up lives there) and how
// large the global GTT is; BAR0's upper half is the GTT itself, one 8-byte
// entry per 4 KB page of graphics address space.  This file decodes those
// and reads the fuse registers that describe the render array.
//
// Meteor Lake changed the picture: there is no aperture, BAR2 being a
// window onto the GTT and the stolen memory instead, the stolen size is
// a register of the GT, and the page attribute table grew coherency
// modes that every page table entry -- the global ones included -- names
// by index.  Xe2 and Xe3 grew the table to 32 entries with L3 and L4
// policies, compression and classes of service; their global GTT is
// always 8 MB of entries (4 GB of address space).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
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

/* Cherryview keeps Valleyview's graphics control word: the GTT size two
 * bits up from Broadwell's (8), the stolen size a five-bit field at 3 --
 * 32 MB units to 0x10, then 4 MB units from 8 MB (0x11-0x16) and from
 * 36 MB (0x17-0x1d). */
#define CHV_GGC_GGMS_SHIFT 8
#define CHV_GGC_GMS_SHIFT 3
#define CHV_GGC_GMS_MASK 0x1f

static uint64_t chv_stolen_size(uint32_t gms)
{
	if (gms < 0x11)
		return (uint64_t)gms << 25;
	if (gms < 0x17)
		return (uint64_t)(gms - 0x11 + 2) << 22;
	return (uint64_t)(gms - 0x17 + 9) << 22;
}

/* Meteor Lake: the stolen memory's size is in the GT's own copy of the
 * graphics control word, in 32 MB units up to 128 MB and 4 MB units
 * from 0xf0; the GTT is fixed at 8 MB.  The memory itself sits behind
 * BAR2, the GTT's 8 MB first and the data stolen memory right after:
 * that, and not a processor address, is how the processor reaches it.
 * BAR2 is no graphics aperture on this part. */
static void mtl_stolen_probe(struct i915_device *i915)
{
	uint16_t ggc = (uint16_t)i915_read32_fw(i915, MTL_GGC);
	uint32_t gms = MTL_GGC_GMS(ggc);

	i915->bar_lmem = i915->bar_aperture;
	i915->bar_aperture.size = 0;
	i915->stolen_size = 0;
	i915->stolen_io = 0;
	if (MTL_GGC_GGMS(ggc) != 0x3) {
		kprintf("[drm] i915: graphics control %04x names no 8 MB GTT; no stolen memory\n", ggc);
		return;
	}
	if (gms <= 0x04)
		i915->stolen_size = (uint64_t)gms * 32 << 20;
	else if (gms >= 0xf0 && gms <= 0xfe)
		i915->stolen_size = (uint64_t)(gms - 0xf0 + 1) * 4 << 20;
	else
		kprintf("[drm] i915: graphics control %04x: unknown stolen size %02x\n", ggc, gms);
	i915->stolen_base = (((uint64_t)i915_read32_fw(i915, GEN6_DSMBASE + 4) << 32) |
			     i915_read32_fw(i915, GEN6_DSMBASE)) & GEN11_BDSM_MASK;
	if (i915->bar_lmem.size >= (8u << 20) + i915->stolen_size)
		i915->stolen_io = i915->bar_lmem.base + (8u << 20);
	i915_dbg("[drm] i915: stolen memory reached through BAR2 at %llx\n",
		 (unsigned long long)i915->stolen_io);
}

int i915_gtt_probe(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	if (i915_is_legacy(i915))
		return i915_legacy_gtt_probe(i915);
	uint16_t ggc = pci_cfg_read16(i915->pci, I915_PCI_GGC);
	int chv = info->platform == I915_PLATFORM_CHERRYVIEW;
	uint32_t ggms = (ggc >> (chv ? CHV_GGC_GGMS_SHIFT : GGC_GGMS_SHIFT)) & GGC_GGMS_MASK;
	uint32_t gms = chv ? (ggc >> CHV_GGC_GMS_SHIFT) & CHV_GGC_GMS_MASK :
			     (ggc >> GGC_GGMS_SHIFT) & GGC_GMS_MASK;

	/* Xe2 on: 8 MB of entries whatever the configuration space says;
	 * the stolen memory is found in the GT's registers (integrated) or
	 * in local memory (discrete, i915_lmem.c) */
	if (i915->gt_ip >= I915_IP(20, 0)) {
		ggms = 3;
		gms = 0;
	}

	/* GTT size: 2 MB << (ggms-1) of entries on Gen8-12 (1=2MB, 2=4MB,
	 * 3=8MB; Cherryview's field elsewhere, the same sizes); each entry
	 * maps a page, so the address space is 512x. */
	if (ggms == 0 || ggms > 3) {
		kprintf("[drm] i915: GGC %04x names no GTT (GGMS=%u)\n", ggc,
			ggms);
		return -ENODEV;
	}
	i915->gtt_size = (uint64_t)1 << (20 + ggms);
	i915->ggtt_bytes = i915->gtt_size / 8 * 4096;
	i915->stolen_size = chv ? chv_stolen_size(gms) : stolen_size_from_gms(info, gms);
	i915->stolen_base = pci_cfg_read32_dev(i915->pci, I915_PCI_BSM) &
			    0xFFF00000u;
	if (info->gen <= 9)
		i915->gsm_base = pci_cfg_read32_dev(i915->pci, I915_PCI_BGSM) &
				 0xFFF00000u;
	if (info->gen_x10 >= 127 && !(info->flags & I915_INFO_IS_DGFX))
		mtl_stolen_probe(i915);

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
	/* Cherryview's display registers are Valleyview's, elsewhere: its
	 * display backend records the firmware's plane itself */
	if (chv)
		return 0;
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
	if (i915_is_legacy(i915)) {
		i915_legacy_gtt_fini(i915);
		return;
	}
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

/* Gen12 on: a fuse bit stands for a pair of execution units. */
static uint16_t pairs_to_units(uint32_t pairs)
{
	uint16_t units = 0;

	for (unsigned i = 0; i < 8; i++)
		if (pairs & (1u << i))
			units |= (uint16_t)(3u << (2 * i));
	return units;
}

/* The L3 banks of Xe2 and Xe3, as one mask: a pattern of banks per node
 * (four bits a node on Xe2, a register's 32 on Xe3) repeated for every
 * node the fuses enable, and the steering targets of the bank and node
 * ranges taken from the first bank (a node has four banks as the
 * steering counts them). */
static void xe2_l3_banks(struct i915_device *i915)
{
	uint32_t fuse3 = i915_read32(i915, GEN10_MIRROR_FUSE3);
	uint32_t nodes = XE2_NODE_ENABLE(fuse3);
	uint64_t mask = 0;
	unsigned first = 0;

	if (i915->gt_ip >= I915_IP(35, 0)) {
		mask = i915_read32(i915, XE2_MIRROR_L3BANK_ENABLE);
	} else if (i915->gt_ip >= I915_IP(30, 0)) {
		uint32_t per_node = i915_read32(i915, XE2_MIRROR_L3BANK_ENABLE);
		for (unsigned n = 0; n < 2; n++)
			if (nodes & (1u << n))
				mask |= (uint64_t)per_node << (32 * n);
	} else {
		uint32_t per_node = XE2_GT_L3_MODE(fuse3);
		for (unsigned n = 0; n < 16; n++)
			if (nodes & (1u << n))
				mask |= (uint64_t)per_node << (4 * n);
	}
	i915->l3bank_mask = (uint32_t)mask;
	for (first = 0; first < 64 && !(mask & (1ULL << first)); first++)
		;
	if (first == 64)
		first = 0;
	i915->l3bank_group = (uint8_t)(first / 4);
	i915->l3bank_instance = (uint8_t)(first % 4);
	i915->node_group = (uint8_t)((first / 4) >> 1);
	i915->node_instance = (uint8_t)((first / 4) & 1);
	/* the SQIDI and PSMI units: the first node enabled */
	unsigned sel = 0;
	while (sel < 16 && !(nodes & (1u << sel)))
		sel++;
	if (sel == 16)
		sel = 0;
	i915->sqidi_group = (uint8_t)(sel >> 1);
	i915->sqidi_instance = (uint8_t)(sel & 1);
}

/* Xe2 and Xe3: the dual subslices ("Xe cores") of the geometry and the
 * compute pipelines in up to three registers each, the execution units a
 * bit each (SIMD16), and the L3 banks of the nodes. */
static void xe2_read_topology(struct i915_device *i915)
{
	uint32_t g = i915_read32(i915, GEN12_GT_GEOMETRY_DSS_ENABLE);
	uint32_t c = i915_read32(i915, GEN12_GT_COMPUTE_DSS_ENABLE);
	uint32_t more = i915_read32(i915, XE2_GT_GEOMETRY_DSS_1) |
			i915_read32(i915, XE2_GT_GEOMETRY_DSS_2) |
			i915_read32(i915, XE2_GT_COMPUTE_DSS_EXT) |
			i915_read32(i915, XE2_GT_COMPUTE_DSS_2);
	uint32_t eu = i915_read32(i915, XEHP_EU_ENABLE) & XEHP_EU_ENA_MASK;
	unsigned eus = popcount32(eu);

	if (more)
		kprintf("[drm] i915: more than 32 Xe cores fused in; the first 32 are used\n");
	i915->dss_geometry = g;
	i915->dss_compute = c;
	i915->slice_mask = 1;
	i915->subslice_mask[0] = g | c;
	i915->eus_per_subslice = eus;
	i915->eu_mask = (uint16_t)eu;
	i915->eu_total = popcount32(g | c) * eus;
	xe2_l3_banks(i915);
}

void i915_read_topology(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;
	uint32_t eu_total = 0;

	if (i915_is_legacy(i915)) {
		i915_legacy_read_topology(i915);
		return;
	}
	i915->slice_mask = 0;
	for (int s = 0; s < 4; s++)
		i915->subslice_mask[s] = 0;
	mm_memset(i915->gen8_eu_mask, 0, sizeof(i915->gen8_eu_mask));

	if (info->platform == I915_PLATFORM_CHERRYVIEW) {
		/* one slice, two subslices of eight units */
		uint32_t fuse = i915_read32(i915, CHV_FUSE_GT);
		i915->slice_mask = 1;
		for (int ss = 0; ss < 2; ss++) {
			if (fuse & (ss ? CHV_FGT_DISABLE_SS1 : CHV_FGT_DISABLE_SS0))
				continue;
			uint32_t d = (fuse >> CHV_FGT_EU_DIS_SS_SHIFT(ss)) & 0xff;
			i915->subslice_mask[0] |= 1u << ss;
			i915->gen8_eu_mask[0][ss] = (uint8_t)~d;
			eu_total += 8 - popcount32(d);
		}
	} else if (info->gen == 8) {
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
				i915->gen8_eu_mask[s][ss] = (uint8_t)~d;
				eu_total += 8 - popcount32(d);
			}
		}
	} else if (info->gen <= 10) {
		uint32_t fuse2 = i915_read32(i915, GEN8_FUSE2);
		uint32_t ss_dis = (fuse2 & GEN9_F2_SS_DIS_MASK) >> GEN9_F2_SS_DIS_SHIFT;
		/* the low-power parts: one slice of at most three subslices */
		int lp = info->platform == I915_PLATFORM_BROXTON ||
			 info->platform == I915_PLATFORM_GEMINILAKE;
		i915->slice_mask = (fuse2 & GEN8_F2_S_ENA_MASK) >> GEN8_F2_S_ENA_SHIFT;
		if (lp)
			i915->slice_mask &= 1;
		for (int s = 0; s < 3; s++) {
			if (!(i915->slice_mask & (1 << s)))
				continue;
			i915->subslice_mask[s] = (~ss_dis) & (lp ? 0x7 : 0xf);
			uint32_t eu_dis = i915_read32(i915, GEN9_EU_DISABLE(s));
			for (int ss = 0; ss < 4; ss++) {
				if (!(i915->subslice_mask[s] & (1 << ss)))
					continue;
				uint32_t d = (eu_dis >> (ss * 8)) & 0xff;
				i915->gen8_eu_mask[s][ss] = (uint8_t)~d;
				eu_total += 8 - popcount32(d);
			}
		}
	} else if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_read_topology(i915);
		eu_total = i915->eu_total;
	} else if (info->gen_x10 >= 125) {
		/* Xe_HP: no slices any more.  One software slice, the dual
		 * subslices of the geometry and the compute pipelines, the
		 * same set of units enabled in each, a bit a pair. */
		uint32_t eu_fuse = i915_read32(i915, XEHP_EU_ENABLE) & XEHP_EU_ENA_MASK;
		unsigned eus = 2 * popcount32(eu_fuse);
		i915->dss_geometry = i915_read32(i915, GEN12_GT_GEOMETRY_DSS_ENABLE);
		i915->dss_compute = i915_read32(i915, GEN12_GT_COMPUTE_DSS_ENABLE);
		i915->slice_mask = 1;
		i915->subslice_mask[0] = i915->dss_geometry | i915->dss_compute;
		i915->eus_per_subslice = eus;
		i915->eu_mask = pairs_to_units(eu_fuse);
		eu_total = popcount32(i915->subslice_mask[0]) * eus;
	} else if (info->gen_x10 >= 120) {
		/* Gen12: dual subslices, which behave like two Gen11
		 * subslices each and are counted as one; one slice on every
		 * part (Tiger Lake, Rocket Lake, DG1, Alder Lake); the EU
		 * fuse a bit a pair of units, the same for every subslice. */
		uint32_t s_en = i915_read32(i915, GEN11_GT_SLICE_ENABLE) & GEN11_GT_S_ENA_MASK;
		uint32_t dss = i915_read32(i915, GEN12_GT_GEOMETRY_DSS_ENABLE) & 0x3f;
		uint32_t eu_en = ~i915_read32(i915, GEN11_EU_DISABLE) & GEN11_EU_DIS_MASK;
		unsigned eus = 2 * popcount32(eu_en);
		i915->slice_mask = s_en ? s_en : 1;
		i915->subslice_mask[0] = dss;
		i915->dss_geometry = dss;
		i915->eus_per_subslice = eus;
		i915->eu_mask = pairs_to_units(eu_en);
		eu_total = popcount32(dss) * eus;
	} else {
		/* Gen11: one slice-enable word, one subslice-disable
		 * word (8 per slice), EU disable per pair of subslices. */
		uint32_t s_en = i915_read32(i915, GEN11_GT_SLICE_ENABLE) &
				GEN11_GT_S_ENA_MASK;
		uint32_t ss_dis = i915_read32(i915, GEN11_GT_SUBSLICE_DISABLE);
		uint32_t eu_dis = i915_read32(i915, GEN11_EU_DISABLE) &
				  GEN11_EU_DIS_MASK;
		unsigned eus_per_ss = 8;
		i915->slice_mask = s_en;
		for (int s = 0; s < 4; s++) {
			if (!(s_en & (1 << s)))
				continue;
			uint32_t ss = (~ss_dis >> (s * 8)) & 0xff;
			i915->subslice_mask[s] = ss;
			eu_total += popcount32(ss) *
				    (eus_per_ss - popcount32(eu_dis));
		}
		i915->eus_per_subslice = eus_per_ss - popcount32(eu_dis);
		i915->eu_mask = (uint16_t)(~eu_dis & 0xff);
	}
	i915->eu_total = eu_total;
}

/* The command streamer's timestamp frequency, which userspace uses to
 * turn GPU timestamps into time. */
uint32_t i915_timestamp_hz(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	if (i915_is_legacy(i915))
		return i915_legacy_timestamp_hz(i915);
	/* Broadwell and Cherryview: the PCU's 10 ns counter, bits 38:3 */
	if (info->gen == 8)
		return 12500000u;
	if (info->gen <= 9)
		return (info->flags & I915_INFO_IS_LP) ? 19200000u : 12000000u;

	/* Xe2 on: the crystal, divided as RPM_CONFIG0 says; and the period
	 * the engines' idle timers count in */
	if (i915->gt_ip >= I915_IP(20, 0)) {
		uint32_t rpm = i915_read32(i915, RPM_CONFIG0);
		uint32_t f = (rpm & GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK) >>
			     GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT;
		uint32_t base;
		switch (f) {
		case 0:
			base = 24000000u;
			i915->ts_base_ps = 83333;
			break;
		case 1:
			base = 19200000u;
			i915->ts_base_ps = 52083;
			break;
		case 2:
			base = 38400000u;
			i915->ts_base_ps = 52083;
			break;
		case 3:
			base = 25000000u;
			i915->ts_base_ps = 80000;
			break;
		default:
			kprintf("[drm] i915: unknown crystal clock (%u)\n", f);
			i915->ts_base_ps = 0;
			return 0;
		}
		return base >> (3 - ((rpm & GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK) >>
				     GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT));
	}

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

/* The page attribute table entry each kind of binding uses: Gen12's
 * table has write-back at 0 and uncached at 3 (as the older parts'
 * index bits did); Meteor Lake's has uncached at 2 and write-back,
 * coherent with the processor's caches one way, at 3. */
uint32_t i915_pat_index(struct i915_device *i915, int uncached)
{
	/* Xe2 on: uncached in L3 and L4 at 3; write-back in the L3,
	 * coherent with the processor both ways, at 2 */
	if (i915->gt_ip >= I915_IP(20, 0))
		return uncached ? 3 : 2;
	if (i915->gt_ip >= I915_IP(12, 70))
		return uncached ? 2 : 3;
	return uncached ? 3 : 0;
}

int i915_pat_is_uncached(struct i915_device *i915, uint32_t pat_index)
{
	return pat_index == i915_pat_index(i915, 1);
}

/* Global entries: before Gen12 the PAT index in the PWT/PCD bits (uncached
 * at index 3); Gen12 global entries carry no index at all (entry 0);
 * Meteor Lake's carry two index bits. */
static uint64_t ggtt_pte_encode(struct i915_device *i915, uint64_t phys,
				int uncached)
{
	uint64_t pte = (phys & 0x0000FFFFFFFFF000ULL) | GEN8_PTE_PRESENT |
		       i915_lmem_ggtt_pte_bits(i915, phys);
	if (i915->gt_ip >= I915_IP(12, 70)) {
		uint32_t pat = i915_pat_index(i915, uncached);
		if (pat & 1)
			pte |= MTL_GGTT_PTE_PAT0;
		if (pat & 2)
			pte |= MTL_GGTT_PTE_PAT1;
		return pte;
	}
	if (i915->info->gen_x10 >= 120)
		return pte;
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
	/* Wa_22019338487: the GMD_ID register is read-only; a write of it
	 * makes the graphics unit finish the entries written before it */
	if (i915->ggtt_wa_every && ++i915->ggtt_wa_count % i915->ggtt_wa_every == 0) {
		i915_write32_fw(i915, i915->ggtt_wa_reg, 0);
		i915->ggtt_wa_count = 0;
	}
}

static void ggtt_flush(struct i915_device *i915)
{
	/* A posting read of the last entry, then the flush control. */
	(void)*(volatile uint64_t *)i915->gtt_virt;
	if (i915->gt_ip >= I915_IP(20, 0)) {
		/* Xe2 on: a read the GuC's view of the table waits for
		 * (without it the GuC was seen reading the scratch page
		 * instead of the page just bound) */
		(void)i915_read32_fw(i915, XE2_VF_CAP_REG);
		if (i915->guc.loaded)
			i915_guc_ggtt_invalidate(i915);
		return;
	}
	i915_write32_fw(i915, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
	(void)i915_read32_fw(i915, GFX_FLSH_CNTL_GEN6);
	/* the GuC keeps translations of its own, which matter once it
	 * submits */
	if (i915->guc.submission)
		i915_guc_ggtt_invalidate(i915);
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

#define GEN8_PPAT_ELLC_OVERRIDE (0 << 2)
#define CHV_PPAT_SNOOP (1 << 6)

/* ---- Xe2, Xe3 and Xe3P: the page attribute tables ------------------------------
 *
 * An entry: no promotion, compression, the L3 class of service, the L3
 * policy (0 write-back, 1 write-back transient for the display, 2
 * write-back transient for the application on Xe3P, 3 uncached), the L4
 * policy (0 write-back, 1 write-through, 3 uncached) and the coherency (0
 * none, 2 one-way, 3 two-way), as the tables of the hardware's
 * documentation list them.  Reserved entries stay zero: the most
 * caching, the least coherency. */
#define XE2_PAT(no_promote, comp_en, l3clos, l3, l4, coh)                         \
	((no_promote ? XE2_NO_PROMOTE : 0) | (comp_en ? XE2_COMP_EN : 0) |           \
	 XE2_L3_CLOS(l3clos) | XE2_L3_POLICY(l3) | XE2_L4_POLICY(l4) | XE2_COH_MODE(coh))

static const uint32_t xe2_pat_table[XE2_PAT_ENTRIES] = {
	[0] = XE2_PAT(0, 0, 0, 0, 3, 0),
	[1] = XE2_PAT(0, 0, 0, 0, 3, 2),
	[2] = XE2_PAT(0, 0, 0, 0, 3, 3),
	[3] = XE2_PAT(0, 0, 0, 3, 3, 0),
	[4] = XE2_PAT(0, 0, 0, 3, 0, 2),
	[5] = XE2_PAT(0, 0, 0, 3, 3, 2),
	[6] = XE2_PAT(1, 0, 0, 1, 3, 0),
	[7] = XE2_PAT(0, 0, 0, 3, 0, 3),
	[8] = XE2_PAT(0, 0, 0, 3, 0, 0),
	[9] = XE2_PAT(0, 1, 0, 0, 3, 0),
	[10] = XE2_PAT(0, 1, 0, 3, 0, 0),
	[11] = XE2_PAT(1, 1, 0, 1, 3, 0),
	[12] = XE2_PAT(0, 1, 0, 3, 3, 0),
	[13] = XE2_PAT(0, 0, 0, 0, 0, 0),
	[14] = XE2_PAT(0, 1, 0, 0, 0, 0),
	[15] = XE2_PAT(1, 1, 0, 1, 1, 0),
	/* 16..19 reserved */
	[20] = XE2_PAT(0, 0, 1, 0, 3, 0),
	[21] = XE2_PAT(0, 1, 1, 0, 3, 0),
	[22] = XE2_PAT(0, 0, 1, 0, 3, 2),
	[23] = XE2_PAT(0, 0, 1, 0, 3, 3),
	[24] = XE2_PAT(0, 0, 2, 0, 3, 0),
	[25] = XE2_PAT(0, 1, 2, 0, 3, 0),
	[26] = XE2_PAT(0, 0, 2, 0, 3, 2),
	[27] = XE2_PAT(0, 0, 2, 0, 3, 3),
	[28] = XE2_PAT(0, 0, 3, 0, 3, 0),
	[29] = XE2_PAT(0, 1, 3, 0, 3, 0),
	[30] = XE2_PAT(0, 0, 3, 0, 3, 2),
	[31] = XE2_PAT(0, 0, 3, 0, 3, 3),
};

/* Xe3: Xe2's, with entry 16 compressed write-back coherent one way */
static const uint32_t xe3_lpg_pat_table[XE2_PAT_ENTRIES] = {
	[0] = XE2_PAT(0, 0, 0, 0, 3, 0),
	[1] = XE2_PAT(0, 0, 0, 0, 3, 2),
	[2] = XE2_PAT(0, 0, 0, 0, 3, 3),
	[3] = XE2_PAT(0, 0, 0, 3, 3, 0),
	[4] = XE2_PAT(0, 0, 0, 3, 0, 2),
	[5] = XE2_PAT(0, 0, 0, 3, 3, 2),
	[6] = XE2_PAT(1, 0, 0, 1, 3, 0),
	[7] = XE2_PAT(0, 0, 0, 3, 0, 3),
	[8] = XE2_PAT(0, 0, 0, 3, 0, 0),
	[9] = XE2_PAT(0, 1, 0, 0, 3, 0),
	[10] = XE2_PAT(0, 1, 0, 3, 0, 0),
	[11] = XE2_PAT(1, 1, 0, 1, 3, 0),
	[12] = XE2_PAT(0, 1, 0, 3, 3, 0),
	[13] = XE2_PAT(0, 0, 0, 0, 0, 0),
	[14] = XE2_PAT(0, 1, 0, 0, 0, 0),
	[15] = XE2_PAT(1, 1, 0, 1, 1, 0),
	[16] = XE2_PAT(0, 1, 0, 0, 3, 2),
	/* 17..19 reserved */
	[20] = XE2_PAT(0, 0, 1, 0, 3, 0),
	[21] = XE2_PAT(0, 1, 1, 0, 3, 0),
	[22] = XE2_PAT(0, 0, 1, 0, 3, 2),
	[23] = XE2_PAT(0, 0, 1, 0, 3, 3),
	[24] = XE2_PAT(0, 0, 2, 0, 3, 0),
	[25] = XE2_PAT(0, 1, 2, 0, 3, 0),
	[26] = XE2_PAT(0, 0, 2, 0, 3, 2),
	[27] = XE2_PAT(0, 0, 2, 0, 3, 3),
	[28] = XE2_PAT(0, 0, 3, 0, 3, 0),
	[29] = XE2_PAT(0, 1, 3, 0, 3, 0),
	[30] = XE2_PAT(0, 0, 3, 0, 3, 2),
	[31] = XE2_PAT(0, 0, 3, 0, 3, 3),
};

/* Xe3P: Xe3's, with the application-transient L3 entries 18 and 19 */
static const uint32_t xe3p_lpg_pat_table[XE2_PAT_ENTRIES] = {
	[0] = XE2_PAT(0, 0, 0, 0, 3, 0),
	[1] = XE2_PAT(0, 0, 0, 0, 3, 2),
	[2] = XE2_PAT(0, 0, 0, 0, 3, 3),
	[3] = XE2_PAT(0, 0, 0, 3, 3, 0),
	[4] = XE2_PAT(0, 0, 0, 3, 0, 2),
	[5] = XE2_PAT(0, 0, 0, 3, 3, 2),
	[6] = XE2_PAT(1, 0, 0, 1, 3, 0),
	[7] = XE2_PAT(0, 0, 0, 3, 0, 3),
	[8] = XE2_PAT(0, 0, 0, 3, 0, 0),
	[9] = XE2_PAT(0, 1, 0, 0, 3, 0),
	[10] = XE2_PAT(0, 1, 0, 3, 0, 0),
	[11] = XE2_PAT(1, 1, 0, 1, 3, 0),
	[12] = XE2_PAT(0, 1, 0, 3, 3, 0),
	[13] = XE2_PAT(0, 0, 0, 0, 0, 0),
	[14] = XE2_PAT(0, 1, 0, 0, 0, 0),
	[15] = XE2_PAT(1, 1, 0, 1, 1, 0),
	[16] = XE2_PAT(0, 1, 0, 0, 3, 2),
	/* 17 reserved */
	[18] = XE2_PAT(1, 0, 0, 2, 3, 0),
	[19] = XE2_PAT(1, 0, 0, 2, 3, 2),
	[20] = XE2_PAT(0, 0, 1, 0, 3, 0),
	[21] = XE2_PAT(0, 1, 1, 0, 3, 0),
	[22] = XE2_PAT(0, 0, 1, 0, 3, 2),
	[23] = XE2_PAT(0, 0, 1, 0, 3, 3),
	[24] = XE2_PAT(0, 0, 2, 0, 3, 0),
	[25] = XE2_PAT(0, 1, 2, 0, 3, 0),
	[26] = XE2_PAT(0, 0, 2, 0, 3, 2),
	[27] = XE2_PAT(0, 0, 2, 0, 3, 3),
	[28] = XE2_PAT(0, 0, 3, 0, 3, 0),
	[29] = XE2_PAT(0, 1, 3, 0, 3, 0),
	[30] = XE2_PAT(0, 0, 3, 0, 3, 2),
	[31] = XE2_PAT(0, 0, 3, 0, 3, 3),
};

const uint32_t *i915_xe2_pat_table(const struct i915_device *i915, unsigned *n)
{
	if (i915->gt_ip >= I915_IP(35, 0)) {
		*n = XE2_PAT_ENTRIES;
		return xe3p_lpg_pat_table;
	}
	if (i915->gt_ip >= I915_IP(30, 0)) {
		*n = XE2_PAT_ENTRIES;
		return xe3_lpg_pat_table;
	}
	/* Wa_16023588340: Battlemage's class of service 3 is not used */
	*n = i915->gt_ip == I915_IP(20, 1) ? 28 : XE2_PAT_ENTRIES;
	return xe2_pat_table;
}

/* Whether a client may name `idx' (GEM_CREATE_EXT_SET_PAT): Meteor
 * Lake's five entries; from Xe2 on the table's, less the ones it leaves
 * reserved (16-19 on Xe2, 17-19 on Xe3, 17 on Xe3P) and Battlemage's
 * class of service 3. */
int i915_pat_index_valid(const struct i915_device *i915, uint32_t idx)
{
	unsigned n;

	if (i915->gt_ip < I915_IP(20, 0))
		return idx <= 4;
	i915_xe2_pat_table(i915, &n);
	if (idx >= n)
		return 0;
	if (i915->gt_ip >= I915_IP(35, 0))
		return idx != 17;
	if (i915->gt_ip >= I915_IP(30, 0))
		return idx < 17 || idx > 19;
	return idx < 16 || idx > 19;
}

/* Whether entry `idx' compresses: Xe2 on (Meteor Lake's do not). */
int i915_pat_compressed(const struct i915_device *i915, uint32_t idx)
{
	unsigned n;
	const uint32_t *t;

	if (i915->gt_ip < I915_IP(20, 0))
		return 0;
	t = i915_xe2_pat_table(i915, &n);
	return idx < n && (t[idx] & XE2_COMP_EN);
}

/* One GT's table: the primary GT's registers have a copy per unit (MCR),
 * the media GT's do not.  Besides the entries, the attributes of the
 * address translation services and -- on the discrete Xe2/Xe3 parts and
 * the integrated Xe3P ones -- those of the hardware's own page table
 * reads. */
static void xe2_program_pat_gt(struct i915_device *i915, int media)
{
	uint32_t gsi = media ? I915_MEDIA_GT_BASE : 0;
	int dgfx = !!(i915->info->flags & I915_INFO_IS_DGFX);
	unsigned n;
	const uint32_t *t = i915_xe2_pat_table(i915, &n);
	uint32_t ats = XE2_PAT(0, 0, 0, 0, 3, 3);
	uint32_t pta = 0, tr_pta = 0;
	int has_pta = 0, has_tr_pta = 0;

	if (i915->gt_ip >= I915_IP(35, 0)) {
		if (!dgfx) {
			pta = media ? XE2_PAT(0, 0, 0, 0, 0, 2) : XE2_PAT(0, 0, 0, 0, 0, 3);
			tr_pta = XE2_PAT(0, 0, 0, 0, 0, 0);
			has_pta = has_tr_pta = 1;
		}
	} else if (dgfx) {
		pta = XE2_PAT(0, 0, 0, 0, 3, 0);
		has_pta = 1;
	}
	for (unsigned i = 0; i < n; i++) {
		if (media)
			i915_write32(i915, gsi + XEHP_PAT_INDEX(i), t[i]);
		else
			i915_mcr_write(i915, XEHP_PAT_INDEX(i), t[i]);
	}
	if (media) {
		i915_write32(i915, gsi + XE2_PAT_ATS, ats);
		if (has_pta)
			i915_write32(i915, gsi + XE2_PAT_PTA, pta);
		if (has_tr_pta)
			i915_write32(i915, gsi + XE3P_PAT_TR_PTA, tr_pta);
	} else {
		i915_mcr_write(i915, XE2_PAT_ATS, ats);
		if (has_pta)
			i915_mcr_write(i915, XE2_PAT_PTA, pta);
		if (has_tr_pta)
			i915_mcr_write(i915, XE3P_PAT_TR_PTA, tr_pta);
	}
}

void i915_ggtt_program_pat(struct i915_device *i915)
{
	if (i915_is_legacy(i915))
		return; /* no private PAT before Broadwell */
	if (i915->gt_ip >= I915_IP(20, 0)) {
		xe2_program_pat_gt(i915, 0);
		if (i915->has_media_gt)
			xe2_program_pat_gt(i915, 1);
		return;
	}
	if (i915->gt_ip >= I915_IP(12, 70)) {
		/* Meteor Lake: write-back, write-through and uncached in the
		 * L4 cache, then write-back coherent one way and both ways;
		 * the rest stay at the hardware's fully cached default.  The
		 * media GT has a table of its own. */
		static const uint32_t pat[5] = {
			MTL_PPAT_L4_0_WB, MTL_PPAT_L4_1_WT, MTL_PPAT_L4_3_UC,
			MTL_PPAT_L4_0_WB | MTL_2_COH_1W, MTL_PPAT_L4_0_WB | MTL_3_COH_2W,
		};
		for (unsigned i = 0; i < 5; i++) {
			i915_mcr_write(i915, XEHP_PAT_INDEX(i), pat[i]);
			if (i915->has_media_gt)
				i915_write32(i915, I915_MEDIA_GT_BASE + XEHP_PAT_INDEX(i), pat[i]);
		}
		return;
	}
	if (i915->info->gen_x10 >= 120) {
		/* Gen12: a register per index, no LLC or age settings;
		 * Xe_HP's has a copy per unit, written to all */
		static const uint32_t pat[8] = { GEN8_PPAT_WB, GEN8_PPAT_WC, GEN8_PPAT_WT,
						 GEN8_PPAT_UC, GEN8_PPAT_WB, GEN8_PPAT_WB,
						 GEN8_PPAT_WB, GEN8_PPAT_WB };
		for (unsigned i = 0; i < 8; i++) {
			if (i915->gt_ip >= I915_IP(12, 55))
				i915_mcr_write(i915, XEHP_PAT_INDEX(i), pat[i]);
			else
				i915_write32(i915, GEN12_PAT_INDEX(i), pat[i]);
		}
		return;
	}
	if (i915->info->gen == 11) {
		/* Gen11: a register per index, at the old table's place */
		static const uint32_t pat[8] = {
			GEN8_PPAT_WB | GEN8_PPAT_LLC,
			GEN8_PPAT_WC | GEN8_PPAT_LLCELLC,
			GEN8_PPAT_WB | GEN8_PPAT_ELLC_OVERRIDE,
			GEN8_PPAT_UC,
			GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(0),
			GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(1),
			GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(2),
			GEN8_PPAT_WB | GEN8_PPAT_LLCELLC | GEN8_PPAT_AGE(3),
		};
		for (unsigned i = 0; i < 8; i++)
			i915_write32(i915, GEN10_PAT_INDEX(i), pat[i]);
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
	/* Cherryview and the low-power Gen9 parts (Broxton, Gemini Lake)
	 * have no shared cache: only the snoop bit means anything.  The
	 * cached entries snoop, the uncached one (3) does not; a global
	 * entry always uses index 0, which keeps the status pages coherent. */
	if (i915->info->platform == I915_PLATFORM_CHERRYVIEW ||
	    i915->info->platform == I915_PLATFORM_BROXTON ||
	    i915->info->platform == I915_PLATFORM_GEMINILAKE)
		pat = GEN8_PPAT(0, CHV_PPAT_SNOOP) | GEN8_PPAT(1, 0) | GEN8_PPAT(2, 0) |
		      GEN8_PPAT(3, 0) | GEN8_PPAT(4, CHV_PPAT_SNOOP) | GEN8_PPAT(5, CHV_PPAT_SNOOP) |
		      GEN8_PPAT(6, CHV_PPAT_SNOOP) | GEN8_PPAT(7, CHV_PPAT_SNOOP);
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
	/* The GuC cannot reach the top of the global space: where the
	 * firmware may run, nothing is bound there. */
	if ((i915->info->flags & I915_INFO_HAS_GUC) && top > GUC_GGTT_TOP)
		top = GUC_GGTT_TOP;
	uint32_t pages = (uint32_t)(top / 4096);
	uint8_t *map = kalloc(pages / 8 + 1);
	if (!map)
		return -ENOMEM;
	mm_memset(map, 0, pages / 8 + 1);
	/* two first bindings at once: one map wins, the other is dropped */
	uint64_t fl;
	spin_lock_irqsave(&i915->ggtt_lock, &fl);
	if (!i915->ggtt_map) {
		i915->ggtt_map_pages = pages;
		i915->ggtt_map_first = (uint32_t)((i915->stolen_size + (64u << 20)) / 4096);
		i915->ggtt_map = map;
		map = NULL;
	}
	spin_unlock_irqrestore(&i915->ggtt_lock, fl);
	if (map)
		kfree(map);
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
	if (i915_is_legacy(i915))
		return i915_legacy_ggtt_bind_obj(i915, o, uncached, ggtt_offset);
	if (!i915->gtt_virt || !o->pages || !o->npages)
		return -EINVAL;
	if (ggtt_map_init(i915) != 0)
		return -ENOMEM;
	/* DG2's local memory is mapped 64 KB at a time */
	uint32_t align = uncached ? SCANOUT_ALIGN_PAGES : 1u;
	if (align < i915_lmem_gtt_align_pages(i915, o))
		align = i915_lmem_gtt_align_pages(i915, o);
	/* The range is found and taken under the map's lock: bindings are
	 * made from every thread (a client's context image, a scanout
	 * buffer) and dropped from every thread, and two that took the
	 * same range would have the engines write one object's data into
	 * the other's pages. */
	uint64_t fl;
	spin_lock_irqsave(&i915->ggtt_lock, &fl);
	uint32_t start = ggtt_map_find(i915, o->npages, align);
	if (start)
		for (uint32_t i = 0; i < o->npages; i++)
			ggtt_map_set(i915, start + i, 1);
	spin_unlock_irqrestore(&i915->ggtt_lock, fl);
	if (!start) {
		static int said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: no room in the global address space for %u pages\n",
				o->npages);
		}
		return -ENOSPC;
	}
	for (uint32_t i = 0; i < o->npages; i++)
		ggtt_write_pte(i915, start + i,
			       ggtt_pte_encode(i915, o->pages[i], uncached));
	ggtt_flush(i915);
	*ggtt_offset = start * 4096;
	if (o->priv)
		((struct i915_bo *)o->priv)->ggtt_uncached = uncached;
	return 0;
}

/* A binding the aperture can reach: bottom-up from the first page the map
 * hands out, below the aperture's size, uncached (the processor reaches
 * these pages through the aperture, the engines by their own means). */
int i915_ggtt_bind_obj_mappable(struct i915_device *i915, struct drm_gem_object *o,
				uint32_t *ggtt_offset)
{
	if (i915_is_legacy(i915))
		return i915_legacy_ggtt_bind_obj_mappable(i915, o, ggtt_offset);
	if (!i915->gtt_virt || !o->pages || !o->npages || !i915->bar_aperture.size)
		return -EINVAL;
	if (ggtt_map_init(i915) != 0)
		return -ENOMEM;
	uint32_t limit = (uint32_t)(i915->bar_aperture.size / 4096);
	if (limit > i915->ggtt_map_pages)
		limit = i915->ggtt_map_pages;
	if (o->npages > limit)
		return -ENOSPC;
	uint32_t start = i915->ggtt_map_first;
	uint64_t fl;
	spin_lock_irqsave(&i915->ggtt_lock, &fl);
	while (start + o->npages <= limit) {
		uint32_t i = 0;
		for (; i < o->npages; i++)
			if (ggtt_map_used(i915, start + i))
				break;
		if (i == o->npages) {
			for (uint32_t k = 0; k < o->npages; k++)
				ggtt_map_set(i915, start + k, 1);
			spin_unlock_irqrestore(&i915->ggtt_lock, fl);
			for (uint32_t k = 0; k < o->npages; k++)
				ggtt_write_pte(i915, start + k,
					       ggtt_pte_encode(i915, o->pages[k], 1));
			ggtt_flush(i915);
			*ggtt_offset = start * 4096;
			return 0;
		}
		start += i + 1;
	}
	spin_unlock_irqrestore(&i915->ggtt_lock, fl);
	static int said;
	if (said < 4) {
		said++;
		kprintf("[drm] i915: no room behind the aperture for %u pages\n", o->npages);
	}
	return -ENOSPC;
}

void i915_ggtt_rewrite_all(struct i915_device *i915)
{
	struct drm_device *dev = &i915->drm;
	unsigned n = 0;

	if (i915_is_legacy(i915)) {
		i915_legacy_ggtt_rewrite_all(i915);
		return;
	}
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
	if (i915_is_legacy(i915)) {
		i915_legacy_ggtt_unbind(i915, ggtt_offset, size);
		return;
	}
	if (!i915->gtt_virt)
		return;
	/* Point at the scratch page; without one a cleared entry is
	 * unmapped and any late access of the engine's faults. */
	uint32_t first = ggtt_offset / 4096;
	uint32_t n = (size + 4095) / 4096;
	uint64_t pte = i915->ggtt_scratch_phys ?
			       ggtt_pte_encode(i915, i915->ggtt_scratch_phys, 1) : 0;
	for (uint32_t i = 0; i < n; i++)
		ggtt_write_pte(i915, first + i, pte);
	ggtt_flush(i915);
	/* the range is free only once nothing of this binding is left in
	 * it: a binding made meanwhile keeps its own entries */
	if (i915->ggtt_map) {
		uint64_t fl;
		spin_lock_irqsave(&i915->ggtt_lock, &fl);
		for (uint32_t i = 0; i < n; i++)
			if (first + i < i915->ggtt_map_pages)
				ggtt_map_set(i915, first + i, 0);
		spin_unlock_irqrestore(&i915->ggtt_lock, fl);
	}
}
