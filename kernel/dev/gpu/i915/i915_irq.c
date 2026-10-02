// LikeOS -- interrupts of Intel graphics.
//
// One MSI vector, a hierarchy of cause registers beneath it.  Gen8-10:
// a master register whose bits name GT pairs, display pipes, ports and
// the PCH, each with its own ISR/IMR/IIR/IER quartet.  Gen11 onwards: a
// graphics master, GT cause dwords with an engine identity handshake,
// and a display master of the Gen8 shape one level down.  The handler
// disables the master, acknowledges every source it finds, hands each
// to its owner, and re-enables the master.
//
// At this stage nothing is enabled below the master except what the
// display and engine code turn on later; the handler acknowledges what
// arrives so a stray source can never storm.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/ke/irq.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

static void gen8_gt_irq_ack(struct i915_device *i915, uint32_t master)
{
	for (int n = 0; n < 4; n++) {
		uint32_t bit;
		switch (n) {
		case 0:
			bit = GEN8_GT_RCS_IRQ | GEN8_GT_BCS_IRQ;
			break;
		case 1:
			bit = GEN8_GT_VCS0_IRQ | GEN8_GT_VCS1_IRQ;
			break;
		case 2:
			bit = GEN8_GT_PM_IRQ | GEN8_GT_GUC_IRQ;
			break;
		default:
			bit = GEN8_GT_VECS_IRQ | GEN8_GT_GUC_IRQ;
			break;
		}
		if (!(master & bit))
			continue;
		uint32_t iir = i915_read32_fw(i915, GEN8_GT_IIR(n));
		if (!iir)
			continue;
		i915_write32_fw(i915, GEN8_GT_IIR(n), iir);
		if (n == 2 && (iir >> 16) & GUC_INTR_GUC2HOST)
			i915_guc_irq(i915);
		if (!i915->gt_ready)
			continue;
		/* each engine owns 16 bits of its pair's register */
		for (int e = 0; e < I915_NUM_ENGINES; e++) {
			struct i915_engine *eng = &i915->engines[e];
			if (!eng->present || eng->gt_iir != (uint32_t)n)
				continue;
			uint32_t bits = (iir >> eng->irq_shift) & 0xffff;
			if (bits)
				i915_engine_irq(eng, bits);
		}
	}
}

static void gen8_de_irq_ack(struct i915_device *i915, uint32_t master,
			    uint32_t de_hpd_iir)
{
	uint32_t port_iir = 0, pch_iir = 0;

	for (int pipe = 0; pipe < 4; pipe++) {
		if (!(master & GEN8_DE_PIPE_IRQ(pipe)))
			continue;
		uint32_t iir = i915_read32_fw(i915, GEN8_DE_PIPE_IIR(pipe));
		if (iir) {
			i915_write32_fw(i915, GEN8_DE_PIPE_IIR(pipe), iir);
			if (i915->display.ready)
				intel_display_irq(i915, pipe, iir);
		}
	}
	if (master & GEN8_DE_PORT_IRQ) {
		port_iir = i915_read32_fw(i915, GEN8_DE_PORT_IIR);
		if (port_iir)
			i915_write32_fw(i915, GEN8_DE_PORT_IIR, port_iir);
	}
	if (master & GEN8_DE_MISC_IRQ) {
		uint32_t iir = i915_read32_fw(i915, GEN8_DE_MISC_IIR);
		if (iir)
			i915_write32_fw(i915, GEN8_DE_MISC_IIR, iir);
	}
	if (master & GEN8_DE_PCH_IRQ) {
		/* Display version 14: the north hotplug bank (PICA) reports
		 * through the PCH bank's summary bit; its own handshake
		 * clears that bit */
		if (i915->info->display_ver >= 14)
			de_hpd_iir |= intel_hpd_de_hpd_ack(i915);
		pch_iir = i915_read32_fw(i915, SDEIIR);
		if (pch_iir)
			i915_write32_fw(i915, SDEIIR, pch_iir);
	}
	if (master & GEN8_PCU_IRQ) {
		uint32_t iir = i915_read32_fw(i915, GEN8_PCU_IIR);
		if (iir)
			i915_write32_fw(i915, GEN8_PCU_IIR, iir);
	}
	if ((port_iir || pch_iir || de_hpd_iir) && i915->display.ready)
		intel_display_hpd_irq(i915, port_iir, pch_iir, de_hpd_iir);
}

static int gen8_irq_handler(struct i915_device *i915)
{
	uint32_t master = i915_read32_fw(i915, GEN8_MASTER_IRQ);

	if (!(master & ~GEN8_MASTER_IRQ_CONTROL))
		return 0;
	i915_write32_fw(i915, GEN8_MASTER_IRQ, 0);
	i915->irq_last_master = master;
	gen8_gt_irq_ack(i915, master);
	gen8_de_irq_ack(i915, master, 0);
	i915_write32_fw(i915, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);
	(void)i915_read32_fw(i915, GEN8_MASTER_IRQ);
	return 1;
}

/* The identity names the engine by its HARDWARE class -- render 0,
 * video decode 1, video enhancement 2, copy 3, other 4, compute 5 --
 * which is not the numbering of the interface the engines are kept in
 * (render 0, copy 1, video 2, video enhancement 3, compute 4).  Taken
 * as it stands, a copy engine's completion went to the video
 * enhancement engine, a video engine's to the copy engine, and the
 * compute engine's nowhere: their fences were found by polling only. */
static int hw_class_to_uapi(uint32_t hw_class)
{
	switch (hw_class) {
	case 0: return I915_ENGINE_CLASS_RENDER;
	case 1: return I915_ENGINE_CLASS_VIDEO;
	case 2: return I915_ENGINE_CLASS_VIDEO_ENHANCE;
	case 3: return I915_ENGINE_CLASS_COPY;
	case 5: return I915_ENGINE_CLASS_COMPUTE;
	default: return -1;
	}
}

static void gen11_gt_irq_ack(struct i915_device *i915, uint32_t bank)
{
	uint32_t dw = i915_read32_fw(i915, GEN11_GT_INTR_DW(bank));

	for (int bit = 0; bit < 32; bit++) {
		if (!(dw & (1u << bit)))
			continue;
		/* Ask which engine and which interrupt, then clear. */
		i915_write32_fw(i915, GEN11_IIR_REG_SELECTOR(bank), 1u << bit);
		uint32_t ident = 0;
		for (int t = 0; t < 100; t++) {
			ident = i915_read32_fw(i915, GEN11_INTR_IDENTITY_REG(bank));
			if (ident & GEN11_INTR_DATA_VALID)
				break;
		}
		/* the identity is released by its valid bit, whether or not
		 * it ever showed: a value written back without that bit
		 * releases nothing, and the source would stay asserted */
		i915_write32_fw(i915, GEN11_INTR_IDENTITY_REG(bank), GEN11_INTR_DATA_VALID);
		if (!(ident & GEN11_INTR_DATA_VALID)) {
			if (i915->irq_ident_lost++ < 4)
				kprintf("[drm] i915: interrupt bank %u bit %d: no identity\n", bank, bit);
			continue;
		}
		/* class 4 is "other": instance 0 the GuC, 16 the media
		 * GT's GuC (Meteor Lake) */
		if (GEN11_INTR_ENGINE_CLASS(ident) == 4) {
			uint32_t inst = GEN11_INTR_ENGINE_INSTANCE(ident);
			if ((inst == 0 || inst == 16) &&
			    (GEN11_INTR_ENGINE_INTR(ident) & GUC_INTR_GUC2HOST))
				i915_guc_irq(i915);
			continue;
		}
		if (!i915->gt_ready)
			continue;
		int uclass = hw_class_to_uapi(GEN11_INTR_ENGINE_CLASS(ident));
		struct i915_engine *eng = uclass < 0 ? NULL :
			i915_engine_by_class(i915, uclass, (int)GEN11_INTR_ENGINE_INSTANCE(ident));
		if (eng)
			i915_engine_irq(eng, GEN11_INTR_ENGINE_INTR(ident));
	}
	i915_write32_fw(i915, GEN11_GT_INTR_DW(bank), dw);
}

/* Graphics 12.10 on -- the discrete cards, and Meteor Lake and every
 * integrated part after it as well -- put a tile master above the
 * graphics master: the interrupt reaches the processor only while the
 * tile master's enable is set.  Left alone (it is clear out of reset),
 * nothing below it ever interrupts: completions are found by polling
 * alone and the display's vblanks are never seen at all. */
static int has_tile_master(const struct i915_device *i915)
{
	return i915->info->gen >= 12 &&
	       ((i915->info->flags & I915_INFO_IS_DGFX) || i915->gt_ip >= I915_IP(12, 10));
}

static int gen11_irq_handler(struct i915_device *i915)
{
	int tile = has_tile_master(i915);
	uint32_t master;

	if (tile) {
		/* the tile master off, its levels read and acknowledged;
		 * tile 0's graphics master acknowledged by writing back */
		i915_write32_fw(i915, DG1_MSTR_TILE_INTR, 0);
		uint32_t levels = i915_read32_fw(i915, DG1_MSTR_TILE_INTR);
		if (!levels || !(levels & DG1_MSTR_TILE(0))) {
			i915_write32_fw(i915, DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
			return 0;
		}
		i915_write32_fw(i915, DG1_MSTR_TILE_INTR, levels);
		master = i915_read32_fw(i915, GEN11_GFX_MSTR_IRQ);
		i915_write32_fw(i915, GEN11_GFX_MSTR_IRQ, master);
	} else {
		master = i915_read32_fw(i915, GEN11_GFX_MSTR_IRQ);
		if (!(master & ~GEN11_MASTER_IRQ))
			return 0;
		i915_write32_fw(i915, GEN11_GFX_MSTR_IRQ, 0);
	}
	i915->irq_last_master = master;
	for (int bank = 0; bank < 2; bank++)
		if (master & GEN11_GT_DW_IRQ(bank))
			gen11_gt_irq_ack(i915, bank);
	if (master & GEN11_DISPLAY_IRQ) {
		uint32_t disp = i915_read32_fw(i915, GEN11_DISPLAY_INT_CTL);
		uint32_t hpd = 0;
		i915->irq_last_disp = disp;
		i915_write32_fw(i915, GEN11_DISPLAY_INT_CTL, 0);
		if ((disp & GEN11_DE_HPD_IRQ) && i915->info->display_ver < 14) {
			hpd = intel_hpd_de_hpd_ack(i915);
		} else if (disp & GEN11_DE_HPD_IRQ) {
			/* Display version 14 reports hotplug through the PCH
			 * bank; this bank is not used, but whatever it latched
			 * (an enable the firmware left) is cleared, or it
			 * would assert again the moment the master is back */
			uint32_t iir = i915_read32_fw(i915, GEN11_DE_HPD_IIR);
			if (iir) {
				i915_write32_fw(i915, GEN11_DE_HPD_IER, 0);
				i915_write32_fw(i915, GEN11_DE_HPD_IIR, iir);
			}
		}
		gen8_de_irq_ack(i915, disp, hpd);
		i915_write32_fw(i915, GEN11_DISPLAY_INT_CTL,
				GEN11_DISPLAY_IRQ_ENABLE);
	}
	if (master & GEN11_GU_MISC_IRQ) {
		uint32_t iir = i915_read32_fw(i915, GEN11_GU_MISC_IIR);
		if (iir)
			i915_write32_fw(i915, GEN11_GU_MISC_IIR, iir);
	}
	if (tile) {
		i915_write32_fw(i915, DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
		(void)i915_read32_fw(i915, DG1_MSTR_TILE_INTR);
		return 1;
	}
	i915_write32_fw(i915, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	(void)i915_read32_fw(i915, GEN11_GFX_MSTR_IRQ);
	return 1;
}

/* The masters on: the graphics master, and from graphics 12.10 on (the
 * discrete parts, Meteor Lake and later) the tile master above it. */
static void gen11_master_enable(struct i915_device *i915)
{
	i915_write32_fw(i915, GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE);
	i915_write32_fw(i915, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	if (has_tile_master(i915)) {
		i915_write32_fw(i915, DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
		(void)i915_read32_fw(i915, DG1_MSTR_TILE_INTR);
	}
}

static int i915_irq_handler(void *arg)
{
	struct i915_device *i915 = arg;
	int mine;

	if (i915->info->gen >= 11)
		mine = gen11_irq_handler(i915);
	else
		mine = gen8_irq_handler(i915);
	if (mine) {
		/* A source that is never acknowledged asserts again the
		 * moment the master is back on, and the processor taking
		 * the vector does nothing else: said, with what the summary
		 * registers held, when 65536 interrupts take under a second */
		if (!(++i915->irq_count & 0xffff)) {
			uint64_t now = hrtimer_now_ns();
			if (i915->irq_storm_mark && now - i915->irq_storm_mark < 1000000000ULL &&
			    i915->irq_storm_said < 4) {
				i915->irq_storm_said++;
				kprintf("[drm] i915: interrupt storm: 65536 interrupts in %u ms (master %08x, display %08x)\n",
					(unsigned)((now - i915->irq_storm_mark) / 1000000),
					i915->irq_last_master, i915->irq_last_disp);
			}
			i915->irq_storm_mark = now;
		}
	} else {
		i915->irq_unclaimed++;
	}
	return mine;
}

/* Everything masked and cleared, then only the master enabled: sources
 * are switched on by the code that consumes them. */
static void i915_irq_reset(struct i915_device *i915)
{
	if (i915->info->gen >= 11) {
		if (has_tile_master(i915))
			i915_write32_fw(i915, DG1_MSTR_TILE_INTR, 0);
		i915_write32_fw(i915, GEN11_GFX_MSTR_IRQ, 0);
		i915_write32_fw(i915, GEN11_DISPLAY_INT_CTL, 0);
		for (int bank = 0; bank < 2; bank++)
			i915_write32_fw(i915, GEN11_GT_INTR_DW(bank), ~0u);
		i915_write32(i915, GEN11_RENDER_COPY_INTR_ENABLE, 0);
		i915_write32(i915, GEN11_VCS_VECS_INTR_ENABLE, 0);
		i915_write32(i915, GEN11_GUC_SG_INTR_ENABLE, 0);
		i915_write32(i915, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		i915_write32(i915, GEN11_CRYPTO_RSVD_INTR_ENABLE, 0);
		i915_write32(i915, GEN11_GUNIT_CSME_INTR_ENABLE, 0);
		if (i915->info->engine_mask & 0xf00)
			i915_write32(i915, GEN12_CCS_RSVD_INTR_ENABLE, 0);
		intel_hpd_de_hpd_reset(i915);
		i915_write32_fw(i915, GEN11_GU_MISC_IMR, ~0u);
		i915_write32_fw(i915, GEN11_GU_MISC_IER, 0);
		i915_write32_fw(i915, GEN11_GU_MISC_IIR, ~0u);
	} else {
		i915_write32_fw(i915, GEN8_MASTER_IRQ, 0);
		for (int n = 0; n < 4; n++) {
			i915_write32_fw(i915, GEN8_GT_IMR(n), ~0u);
			i915_write32_fw(i915, GEN8_GT_IER(n), 0);
			i915_write32_fw(i915, GEN8_GT_IIR(n), ~0u);
		}
	}
	for (int pipe = 0; pipe < i915->info->num_pipes; pipe++) {
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(pipe), ~0u);
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(pipe), 0);
		i915_write32_fw(i915, GEN8_DE_PIPE_IIR(pipe), ~0u);
	}
	i915_write32_fw(i915, GEN8_DE_PORT_IMR, ~0u);
	i915_write32_fw(i915, GEN8_DE_PORT_IER, 0);
	i915_write32_fw(i915, GEN8_DE_PORT_IIR, ~0u);
	i915_write32_fw(i915, GEN8_DE_MISC_IMR, ~0u);
	i915_write32_fw(i915, GEN8_DE_MISC_IER, 0);
	i915_write32_fw(i915, GEN8_DE_MISC_IIR, ~0u);
	i915_write32_fw(i915, GEN8_PCU_IMR, ~0u);
	i915_write32_fw(i915, GEN8_PCU_IER, 0);
	i915_write32_fw(i915, GEN8_PCU_IIR, ~0u);
	/* the south display bank: a PCH's, or the card's own on DG1/DG2 */
	if (i915->pch != I915_PCH_NONE) {
		i915_write32_fw(i915, SDEIMR, ~0u);
		i915_write32_fw(i915, SDEIER, 0);
		i915_write32_fw(i915, SDEIIR, ~0u);
	}
}

int i915_irq_init(struct i915_device *i915)
{
	int vec = -1;
	int rc;

	/* before Broadwell, and Cherryview: another hierarchy */
	if (i915_legacy_irqs(i915))
		return i915_legacy_irq_init(i915);
	i915_irq_reset(i915);
	i915->irq_vector = -1;
	rc = irq_request_msi(i915->pci, 0, i915_irq_handler, i915, "i915",
			     &vec);
	if (rc) {
		kprintf("[drm] i915: no MSI vector (%d); running without interrupts\n",
			rc);
		return rc;
	}
	i915->irq_vector = vec;
	/* Master on; every source still masked. */
	if (i915->info->gen >= 11) {
		gen11_master_enable(i915);
	} else {
		i915_write32_fw(i915, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);
	}
	(void)i915_read32_fw(i915, GEN8_MASTER_IRQ);
	i915_dbg("[drm] i915: MSI on vector %d\n", vec);
	return 0;
}

void i915_irq_suspend(struct i915_device *i915)
{
	if (i915_legacy_irqs(i915)) {
		i915_legacy_irq_suspend(i915);
		return;
	}
	i915_irq_reset(i915);
}

void i915_irq_resume(struct i915_device *i915)
{
	if (i915_legacy_irqs(i915)) {
		i915_legacy_irq_resume(i915);
		return;
	}
	i915_irq_reset(i915);
	if (i915->irq_vector < 0)
		return;
	if (i915->info->gen >= 11) {
		gen11_master_enable(i915);
	} else {
		i915_write32_fw(i915, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);
	}
	(void)i915_read32_fw(i915, GEN8_MASTER_IRQ);
}

void i915_irq_fini(struct i915_device *i915)
{
	if (i915_legacy_irqs(i915)) {
		i915_legacy_irq_fini(i915);
		return;
	}
	i915_irq_reset(i915);
	if (i915->irq_vector >= 0) {
		irq_free(i915->irq_vector, i915_irq_handler, i915);
		i915->irq_vector = -1;
	}
}
