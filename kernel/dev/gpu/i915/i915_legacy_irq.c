// LikeOS -- interrupts of the Intel graphics parts before Broadwell (and
// Cherryview's).
//
// Gen2 to gen4 have one identity register, IIR, holding the render
// engine's user interrupt (and on G4x the video engine's), the error
// summary and one bit per display pipe whose details are in that pipe's
// PIPESTAT.  Ironlake moved the GT into GTIIR and put a master enable in
// the north display's DEIER, over the GT, the display and (from Sandy
// Bridge) the power management unit's PMIIR, which also carries
// Haswell's video-enhancement engine.  Valleyview has the Sandy Bridge
// GT under a display of the gen4 kind with a master of its own;
// Cherryview the same display over the Gen8 GT.
//
// The handler masks the master where there is one, reads and clears
// every source, and hands each to its owner: user interrupts to the
// engine's retirement, P-state events to the frequency code, the
// display's to intel_legacy_display_irq() (Haswell's DDI display goes to
// the common display code in the Gen8 form it understands).  Display
// status that lives in PIPESTAT must be cleared before IIR, so the
// display's turn comes first.
//
// MSI from Ironlake on; before, the chipsets lose or delay MSI and the
// shared legacy line is used.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2003-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/irq.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* The display side of the parts without DDI: PIPESTAT and the hotplug
 * status (gen2-4, Valleyview, Cherryview) or DEIIR and SDEIIR (Ironlake
 * to Ivy Bridge), read, cleared and acted on.  Without it the display
 * interrupt sources stay disabled. */
extern void intel_legacy_display_irq(struct i915_device *i915) __attribute__((weak));
/* What the display wants unmasked at the top level (the pipe event and
 * port bits of IIR on gen2-4, Valleyview and Cherryview), and its own
 * reset and enabling below that, once it is up. */
extern int intel_legacy_display_active(struct i915_device *i915) __attribute__((weak));
extern uint32_t intel_legacy_irq_enable_bits(struct i915_device *i915, uint32_t *ier_only)
	__attribute__((weak));
extern void intel_legacy_irq_postinstall(struct i915_device *i915) __attribute__((weak));
extern void intel_legacy_irq_reset(struct i915_device *i915) __attribute__((weak));

static inline int has_display_irq(void)
{
	return intel_legacy_display_irq != NULL;
}

static int display_active(struct i915_device *i915)
{
	return intel_legacy_display_active && intel_legacy_display_active(i915);
}

static void display_irq_reset(struct i915_device *i915)
{
	if (intel_legacy_irq_reset && display_active(i915))
		intel_legacy_irq_reset(i915);
}

static void display_irq_postinstall(struct i915_device *i915)
{
	if (intel_legacy_irq_postinstall && display_active(i915))
		intel_legacy_irq_postinstall(i915);
}

/* The top-level display bits of IIR (gen2-4, VLV, CHV): the display's
 * own answer once it is up, the pipe events and the port before. */
static uint32_t gmch_display_bits(struct i915_device *i915, uint32_t defaults)
{
	uint32_t ier_only = 0;

	if (!has_display_irq())
		return 0;
	if (intel_legacy_irq_enable_bits && display_active(i915))
		return intel_legacy_irq_enable_bits(i915, &ier_only) | ier_only;
	return defaults;
}

static inline void display_irq(struct i915_device *i915)
{
	if (intel_legacy_display_irq && i915->display.ready)
		intel_legacy_display_irq(i915);
}

#define ILK_DE_PCU_EVENT (1 << 25)

static inline struct i915_engine *engine_of(struct i915_device *i915, int id)
{
	struct i915_engine *e = &i915->engines[id];
	return (i915->gt_ready && e->present) ? e : NULL;
}

static void user_interrupt(struct i915_device *i915, int id)
{
	struct i915_engine *e = engine_of(i915, id);
	if (e)
		leg_engine_signal(e);
}

/* ---- the error summary of gen2-4 ----------------------------------------------- */

static int i9xx_has_fbc(struct i915_device *i915)
{
	int p = i915->info->platform;
	return p == I915_PLATFORM_I85X || p == I915_PLATFORM_I865G || p == I915_PLATFORM_I915GM ||
	       p == I915_PLATFORM_I945GM;
}

/* Gen2/3: frame buffer compression raises spurious page-table errors,
 * and the per-plane bits cannot be masked one by one. */
static uint32_t i9xx_error_mask(struct i915_device *i915)
{
	if (i9xx_has_fbc(i915))
		return I915_ERROR_MEMORY_REFRESH;
	return I915_ERROR_PAGE_TABLE | I915_ERROR_MEMORY_REFRESH;
}

static uint32_t i965_error_mask(struct i915_device *i915)
{
	(void)i915;
	if (g_leg.is_g4x)
		return GM45_ERROR_PAGE_TABLE | GM45_ERROR_MEM_PRIV | GM45_ERROR_CP_PRIV |
		       I915_ERROR_MEMORY_REFRESH;
	return I915_ERROR_PAGE_TABLE | I915_ERROR_MEMORY_REFRESH;
}

static void i9xx_error_irq_ack(struct i915_device *i915, uint32_t *eir, uint32_t *eir_stuck)
{
	*eir = i915_read32(i915, EIR);
	i915_write32(i915, EIR, *eir);
	*eir_stuck = i915_read32(i915, EIR);
	if (!*eir_stuck)
		return;
	/* Some error bits only clear with the error itself: toggling every
	 * mask bit makes an edge for the master error bit, and what stays
	 * set is masked from now on. */
	uint32_t emr = i915_read32(i915, EMR);
	i915_write32(i915, EMR, 0xffffffff);
	i915_write32(i915, EMR, emr | *eir_stuck);
}

static void i9xx_error_irq_report(struct i915_device *i915, uint32_t eir, uint32_t eir_stuck)
{
	static int said;

	if (said >= 8)
		return;
	said++;
	kprintf("[drm] i915: master error, EIR %08x%s, PGTBL_ER %08x\n", eir,
		eir_stuck ? " (stuck bits masked)" : "", i915_read32(i915, PGTBL_ER));
}

static void gen2_irq_reset_regs(struct i915_device *i915, uint32_t imr, uint32_t ier, uint32_t iir)
{
	i915_write32(i915, imr, 0xffffffff);
	(void)i915_read32(i915, imr);
	i915_write32(i915, ier, 0);
	/* IIR can queue two events */
	i915_write32(i915, iir, 0xffffffff);
	(void)i915_read32(i915, iir);
	i915_write32(i915, iir, 0xffffffff);
	(void)i915_read32(i915, iir);
}

static void gen2_irq_init_regs(struct i915_device *i915, uint32_t imr, uint32_t ier, uint32_t iir,
			       uint32_t imr_val, uint32_t ier_val)
{
	if (i915_read32(i915, iir)) {
		i915_write32(i915, iir, 0xffffffff);
		(void)i915_read32(i915, iir);
		i915_write32(i915, iir, 0xffffffff);
		(void)i915_read32(i915, iir);
	}
	i915_write32(i915, ier, ier_val);
	i915_write32(i915, imr, imr_val);
	(void)i915_read32(i915, imr);
}

static void gen2_error_reset(struct i915_device *i915)
{
	i915_write32(i915, EMR, 0xffffffff);
	(void)i915_read32(i915, EMR);
	i915_write32(i915, EIR, 0xffffffff);
	(void)i915_read32(i915, EIR);
	i915_write32(i915, EIR, 0xffffffff);
	(void)i915_read32(i915, EIR);
}

static void gen2_error_init(struct i915_device *i915, uint32_t emr)
{
	i915_write32(i915, EIR, 0xffffffff);
	(void)i915_read32(i915, EIR);
	i915_write32(i915, EIR, 0xffffffff);
	(void)i915_read32(i915, EIR);
	i915_write32(i915, EMR, emr);
	(void)i915_read32(i915, EMR);
}

/* ---- gen2/3 and gen4 ---------------------------------------------------------------- */

static int i9xx_irq_handler(struct i915_device *i915)
{
	uint32_t iir = i915_read32(i915, GEN2_IIR);
	uint32_t eir = 0, eir_stuck = 0;

	if (!iir)
		return 0;
	/* PIPESTAT and the hotplug status first: IIR reflects them */
	display_irq(i915);
	if (iir & I915_MASTER_ERROR_INTERRUPT)
		i9xx_error_irq_ack(i915, &eir, &eir_stuck);
	i915_write32(i915, GEN2_IIR, iir);
	if (iir & I915_USER_INTERRUPT)
		user_interrupt(i915, I915_RCS0);
	if (leg_gen(i915) == 4 && (iir & I915_BSD_USER_INTERRUPT))
		user_interrupt(i915, I915_VCS0);
	if (iir & I915_MASTER_ERROR_INTERRUPT)
		i9xx_error_irq_report(i915, eir, eir_stuck);
	return 1;
}

static void i9xx_irq_reset(struct i915_device *i915)
{
	display_irq_reset(i915);
	gen2_error_reset(i915);
	gen2_irq_reset_regs(i915, GEN2_IMR, GEN2_IER, GEN2_IIR);
	g_leg.gen2_imr = ~0u;
}

static void i9xx_irq_postinstall(struct i915_device *i915)
{
	uint32_t enable;

	gen2_error_init(i915, ~(leg_gen(i915) == 4 ? i965_error_mask(i915) : i9xx_error_mask(i915)));
	enable = I915_MASTER_ERROR_INTERRUPT;
	enable |= gmch_display_bits(i915, I915_DISPLAY_PIPE_A_EVENT_INTERRUPT |
						  I915_DISPLAY_PIPE_B_EVENT_INTERRUPT |
						  (leg_gen(i915) >= 3 ? I915_DISPLAY_PORT_INTERRUPT : 0));
	/* the user interrupts stay unmasked: retirement is always wanted */
	enable |= I915_USER_INTERRUPT;
	if (g_leg.is_g4x)
		enable |= I915_BSD_USER_INTERRUPT;
	g_leg.gen2_imr = ~enable;
	gen2_irq_init_regs(i915, GEN2_IMR, GEN2_IER, GEN2_IIR, g_leg.gen2_imr, enable);
	display_irq_postinstall(i915);
}

/* ---- the GT and PM units (Ironlake on) --------------------------------------------- */

static void gen6_gt_irq_handler(struct i915_device *i915, uint32_t gt_iir)
{
	if (gt_iir & GT_RENDER_USER_INTERRUPT)
		user_interrupt(i915, I915_RCS0);
	if (gt_iir & GT_BSD_USER_INTERRUPT)
		user_interrupt(i915, I915_VCS0);
	if (gt_iir & GT_BLT_USER_INTERRUPT)
		user_interrupt(i915, I915_BCS0);
	if (gt_iir & (GT_BLT_CS_ERROR_INTERRUPT | GT_BSD_CS_ERROR_INTERRUPT |
		      GT_CS_MASTER_ERROR_INTERRUPT))
		i915_dbg("[drm] i915: command parser error, GT IIR %08x\n", gt_iir);
}

static void gen5_gt_irq_handler(struct i915_device *i915, uint32_t gt_iir)
{
	if (gt_iir & GT_RENDER_USER_INTERRUPT)
		user_interrupt(i915, I915_RCS0);
	if (gt_iir & ILK_BSD_USER_INTERRUPT)
		user_interrupt(i915, I915_VCS0);
}

static void pm_irq_handler(struct i915_device *i915, uint32_t pm_iir)
{
	leg_rps_irq(i915, pm_iir);
	if (pm_iir & PM_VEBOX_USER_INTERRUPT)
		user_interrupt(i915, I915_VECS0);
	if (pm_iir & PM_VEBOX_CS_ERROR_INTERRUPT)
		i915_dbg("[drm] i915: command parser error, PM IIR %08x\n", pm_iir);
}

static void gen5_gt_irq_reset(struct i915_device *i915)
{
	gen2_irq_reset_regs(i915, GTIMR, GTIER, GTIIR);
	if (leg_gen(i915) >= 6)
		gen2_irq_reset_regs(i915, GEN6_PMIMR, GEN6_PMIER, GEN6_PMIIR);
}

static void gen5_gt_irq_postinstall(struct i915_device *i915)
{
	uint32_t gt_irqs = GT_RENDER_USER_INTERRUPT;
	uint32_t pm_irqs = 0;

	if (leg_gen(i915) == 5)
		gt_irqs |= ILK_BSD_USER_INTERRUPT;
	else
		gt_irqs |= GT_BLT_USER_INTERRUPT | GT_BSD_USER_INTERRUPT;
	g_leg.gt_imr = ~gt_irqs;
	gen2_irq_init_regs(i915, GTIMR, GTIER, GTIIR, g_leg.gt_imr, gt_irqs);
	if (leg_gen(i915) >= 6) {
		/* the P-state events are switched on by the frequency code */
		if (leg_is(i915, I915_PLATFORM_HASWELL))
			pm_irqs |= PM_VEBOX_USER_INTERRUPT;
		g_leg.pm_imr = ~pm_irqs;
		gen2_irq_init_regs(i915, GEN6_PMIMR, GEN6_PMIER, GEN6_PMIIR, g_leg.pm_imr,
				   pm_irqs | g_leg.pm_events);
		if (g_leg.pm_events) {
			g_leg.pm_imr &= ~g_leg.pm_events;
			i915_write32(i915, GEN6_PMIMR, g_leg.pm_imr);
		}
	}
}

/* ---- Ironlake to Haswell ------------------------------------------------------------- */

#define HSW_DE_ROUTED                                                                  \
	(DE_PCH_EVENT_IVB | DE_DP_A_HOTPLUG_IVB | DE_AUX_CHANNEL_A_IVB |               \
	 DE_PIPE_VBLANK_IVB(0) | DE_PIPE_VBLANK_IVB(1) | DE_PIPE_VBLANK_IVB(2) |       \
	 DE_PLANE_FLIP_DONE_IVB(0) | DE_PLANE_FLIP_DONE_IVB(1) | DE_PLANE_FLIP_DONE_IVB(2))

/* Haswell's DDI display is driven by the common display code, which
 * speaks the Gen8 interrupt layout: DEIIR translated into it. */
static int hsw_de_irq(struct i915_device *i915)
{
	uint32_t de_iir = i915_read32_fw(i915, DEIIR);
	uint32_t port_iir = 0, pch_iir = 0;

	if (!de_iir)
		return 0;
	i915_write32_fw(i915, DEIIR, de_iir);
	for (int pipe = 0; pipe < 3; pipe++) {
		uint32_t bits = 0;
		if (de_iir & DE_PIPE_VBLANK_IVB(pipe))
			bits |= GEN8_PIPE_VBLANK;
		if (de_iir & DE_PLANE_FLIP_DONE_IVB(pipe))
			bits |= GEN8_PIPE_PRIMARY_FLIP_DONE;
		if (bits && i915->display.ready)
			intel_display_irq(i915, pipe, bits);
	}
	if (de_iir & DE_ERR_INT_IVB) {
		uint32_t err = i915_read32_fw(i915, GEN7_ERR_INT);
		i915_write32_fw(i915, GEN7_ERR_INT, err);
	}
	if (de_iir & DE_DP_A_HOTPLUG_IVB)
		port_iir |= GEN8_PORT_DP_A_HOTPLUG;
	if (de_iir & DE_AUX_CHANNEL_A_IVB)
		port_iir |= GEN8_AUX_CHANNEL_A;
	if (de_iir & DE_PCH_EVENT_IVB) {
		pch_iir = i915_read32_fw(i915, SDEIIR);
		if (pch_iir)
			i915_write32_fw(i915, SDEIIR, pch_iir);
	}
	if ((port_iir || pch_iir) && i915->display.ready)
		intel_display_hpd_irq(i915, port_iir, pch_iir, 0);
	return 1;
}

static int ilk_irq_handler(struct i915_device *i915)
{
	uint32_t de_ier, sde_ier = 0, gt_iir, pm_iir = 0;
	int handled = 0;
	int pch = !!(i915->info->flags & I915_INFO_HAS_PCH);

	/* master and south off: what arrives meanwhile queues */
	de_ier = i915_read32_fw(i915, DEIER);
	i915_write32_fw(i915, DEIER, de_ier & ~DE_MASTER_IRQ_CONTROL);
	if (pch) {
		sde_ier = i915_read32_fw(i915, SDEIER);
		i915_write32_fw(i915, SDEIER, 0);
	}

	gt_iir = i915_read32_fw(i915, GTIIR);
	if (gt_iir) {
		i915_write32_fw(i915, GTIIR, gt_iir);
		if (leg_gen(i915) >= 6)
			gen6_gt_irq_handler(i915, gt_iir);
		else
			gen5_gt_irq_handler(i915, gt_iir);
		handled = 1;
	}
	if (leg_is(i915, I915_PLATFORM_HASWELL)) {
		handled |= hsw_de_irq(i915);
	} else if (has_display_irq()) {
		if (i915_read32_fw(i915, DEIIR) || (pch && i915_read32_fw(i915, SDEIIR))) {
			display_irq(i915);
			handled = 1;
		}
		/* Ironlake's PCU event is the frequency code's, which this
		 * driver leaves to the firmware: acknowledged */
		if (leg_gen(i915) == 5) {
			uint32_t pcu = i915_read32_fw(i915, DEIIR) & ILK_DE_PCU_EVENT;
			if (pcu) {
				i915_write32_fw(i915, DEIIR, pcu);
				handled = 1;
			}
		}
	} else {
		/* nobody to hand them to: acknowledged, so they cannot storm */
		uint32_t de_iir = i915_read32_fw(i915, DEIIR);
		if (de_iir) {
			i915_write32_fw(i915, DEIIR, de_iir);
			handled = 1;
		}
		if (pch) {
			uint32_t sde_iir = i915_read32_fw(i915, SDEIIR);
			if (sde_iir) {
				i915_write32_fw(i915, SDEIIR, sde_iir);
				handled = 1;
			}
		}
	}
	if (leg_gen(i915) >= 6) {
		pm_iir = i915_read32_fw(i915, GEN6_PMIIR);
		if (pm_iir) {
			i915_write32_fw(i915, GEN6_PMIIR, pm_iir);
			pm_irq_handler(i915, pm_iir);
			handled = 1;
		}
	}

	i915_write32_fw(i915, DEIER, de_ier);
	if (sde_ier)
		i915_write32_fw(i915, SDEIER, sde_ier);
	return handled;
}

static void ilk_irq_reset(struct i915_device *i915)
{
	/* the master enable lives in DEIER: off first */
	i915_write32(i915, DEIER, i915_read32(i915, DEIER) & ~DE_MASTER_IRQ_CONTROL);
	if (!leg_is(i915, I915_PLATFORM_HASWELL))
		display_irq_reset(i915);
	if (leg_is(i915, I915_PLATFORM_HASWELL) || !has_display_irq()) {
		gen2_irq_reset_regs(i915, DEIMR, DEIER, DEIIR);
		if (leg_gen(i915) >= 7)
			i915_write32(i915, GEN7_ERR_INT, 0xffffffff);
	}
	gen5_gt_irq_reset(i915);
}

static void ilk_irq_postinstall(struct i915_device *i915)
{
	gen5_gt_irq_postinstall(i915);
	if (leg_is(i915, I915_PLATFORM_HASWELL)) {
		gen2_irq_init_regs(i915, DEIMR, DEIER, DEIIR, ~(uint32_t)HSW_DE_ROUTED,
				   HSW_DE_ROUTED | DE_MASTER_IRQ_CONTROL);
		return;
	}
	/* the rest of DEIER is the display's */
	display_irq_postinstall(i915);
	i915_write32(i915, DEIER, i915_read32(i915, DEIER) | DE_MASTER_IRQ_CONTROL);
	(void)i915_read32(i915, DEIER);
}

/* ---- Valleyview and Cherryview --------------------------------------------------------- */

static uint32_t vlv_display_mask(struct i915_device *i915)
{
	uint32_t m = I915_DISPLAY_PORT_INTERRUPT | I915_DISPLAY_PIPE_A_EVENT_INTERRUPT |
		     I915_DISPLAY_PIPE_B_EVENT_INTERRUPT;

	if (leg_is(i915, I915_PLATFORM_CHERRYVIEW))
		m |= (1 << 9); /* pipe C */
	return gmch_display_bits(i915, m);
}

static int vlv_irq_handler(struct i915_device *i915)
{
	uint32_t gt_iir = i915_read32_fw(i915, GTIIR);
	uint32_t pm_iir = i915_read32_fw(i915, GEN6_PMIIR);
	uint32_t iir = i915_read32_fw(i915, VLV_IIR);
	uint32_t ier;

	if (!gt_iir && !pm_iir && !iir)
		return 0;
	/* An interrupt is an edge of (display && IER) || ((GT || PM) &&
	 * master): both off so that whatever is left uncleared makes a
	 * new edge when they come back. */
	i915_write32_fw(i915, VLV_MASTER_IER, 0);
	ier = i915_read32_fw(i915, VLV_IER);
	i915_write32_fw(i915, VLV_IER, 0);
	if (gt_iir)
		i915_write32_fw(i915, GTIIR, gt_iir);
	if (pm_iir)
		i915_write32_fw(i915, GEN6_PMIIR, pm_iir);
	/* VLV_IIR reflects PIPESTAT: cleared after it */
	if (iir)
		display_irq(i915);
	if (iir)
		i915_write32_fw(i915, VLV_IIR, iir);
	i915_write32_fw(i915, VLV_IER, ier);
	i915_write32_fw(i915, VLV_MASTER_IER, MASTER_INTERRUPT_ENABLE);
	if (gt_iir)
		gen6_gt_irq_handler(i915, gt_iir);
	if (pm_iir)
		pm_irq_handler(i915, pm_iir);
	return 1;
}

static void vlv_irq_reset(struct i915_device *i915)
{
	i915_write32(i915, VLV_MASTER_IER, 0);
	(void)i915_read32(i915, VLV_MASTER_IER);
	gen5_gt_irq_reset(i915);
	display_irq_reset(i915);
	gen2_irq_reset_regs(i915, VLV_IMR, VLV_IER, VLV_IIR);
}

static void vlv_irq_postinstall(struct i915_device *i915)
{
	uint32_t m = vlv_display_mask(i915);

	gen5_gt_irq_postinstall(i915);
	gen2_irq_init_regs(i915, VLV_IMR, VLV_IER, VLV_IIR, ~m, m);
	display_irq_postinstall(i915);
	i915_write32(i915, VLV_MASTER_IER, MASTER_INTERRUPT_ENABLE);
	(void)i915_read32(i915, VLV_MASTER_IER);
}

/* Cherryview's GT: the Gen8 banks, each engine owning sixteen bits of
 * one of them (the shifts the engine code set up). */
static void chv_gt_irq_handler(struct i915_device *i915, uint32_t master)
{
	static const uint32_t bank_bits[4] = {
		GEN8_GT_RCS_IRQ | GEN8_GT_BCS_IRQ,
		GEN8_GT_VCS0_IRQ | GEN8_GT_VCS1_IRQ,
		GEN8_GT_PM_IRQ | GEN8_GT_GUC_IRQ,
		GEN8_GT_VECS_IRQ,
	};

	for (int n = 0; n < 4; n++) {
		if (!(master & bank_bits[n]))
			continue;
		uint32_t iir = i915_read32_fw(i915, GEN8_GT_IIR(n));
		if (!iir)
			continue;
		i915_write32_fw(i915, GEN8_GT_IIR(n), iir);
		if (!i915->gt_ready)
			continue;
		for (int i = 0; i < I915_NUM_ENGINES; i++) {
			struct i915_engine *e = &i915->engines[i];
			if (!e->present || e->gt_iir != (uint32_t)n)
				continue;
			uint32_t bits = (iir >> e->irq_shift) & 0xffff;
			if (bits)
				i915_engine_irq(e, bits);
		}
	}
}

static int chv_irq_handler(struct i915_device *i915)
{
	uint32_t master = i915_read32_fw(i915, GEN8_MASTER_IRQ) & ~GEN8_MASTER_IRQ_CONTROL;
	uint32_t iir = i915_read32_fw(i915, VLV_IIR);
	uint32_t ier;

	if (!master && !iir)
		return 0;
	i915_write32_fw(i915, GEN8_MASTER_IRQ, 0);
	ier = i915_read32_fw(i915, VLV_IER);
	i915_write32_fw(i915, VLV_IER, 0);
	chv_gt_irq_handler(i915, master);
	if (iir) {
		display_irq(i915);
		i915_write32_fw(i915, VLV_IIR, iir);
	}
	i915_write32_fw(i915, VLV_IER, ier);
	i915_write32_fw(i915, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);
	return 1;
}

static void chv_irq_reset(struct i915_device *i915)
{
	i915_write32(i915, GEN8_MASTER_IRQ, 0);
	(void)i915_read32(i915, GEN8_MASTER_IRQ);
	for (int n = 0; n < 4; n++)
		gen2_irq_reset_regs(i915, GEN8_GT_IMR(n), GEN8_GT_IER(n), GEN8_GT_IIR(n));
	gen2_irq_reset_regs(i915, GEN8_PCU_IMR, GEN8_PCU_IER, GEN8_PCU_IIR);
	display_irq_reset(i915);
	gen2_irq_reset_regs(i915, VLV_IMR, VLV_IER, VLV_IIR);
}

static void chv_irq_postinstall(struct i915_device *i915)
{
	uint32_t m = vlv_display_mask(i915);

	/* the GT banks are the engine code's to enable */
	gen2_irq_init_regs(i915, VLV_IMR, VLV_IER, VLV_IIR, ~m, m);
	display_irq_postinstall(i915);
	i915_write32(i915, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);
	(void)i915_read32(i915, GEN8_MASTER_IRQ);
}

/* ---- dispatch ------------------------------------------------------------------------- */

int i915_legacy_irq_handler(struct i915_device *i915)
{
	if (leg_is(i915, I915_PLATFORM_CHERRYVIEW))
		return chv_irq_handler(i915);
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		return vlv_irq_handler(i915);
	if (leg_gen(i915) >= 5)
		return ilk_irq_handler(i915);
	return i9xx_irq_handler(i915);
}

static int legacy_irq(void *arg)
{
	struct i915_device *i915 = arg;
	int mine = i915_legacy_irq_handler(i915);

	if (mine)
		i915->irq_count++;
	else
		i915->irq_unclaimed++;
	return mine;
}

static void legacy_irq_reset(struct i915_device *i915)
{
	if (leg_is(i915, I915_PLATFORM_CHERRYVIEW))
		chv_irq_reset(i915);
	else if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		vlv_irq_reset(i915);
	else if (leg_gen(i915) >= 5)
		ilk_irq_reset(i915);
	else
		i9xx_irq_reset(i915);
}

static void legacy_irq_postinstall(struct i915_device *i915)
{
	if (leg_is(i915, I915_PLATFORM_CHERRYVIEW))
		chv_irq_postinstall(i915);
	else if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		vlv_irq_postinstall(i915);
	else if (leg_gen(i915) >= 5)
		ilk_irq_postinstall(i915);
	else
		i9xx_irq_postinstall(i915);
}

/* The legacy line: the ACPI routing table first, the firmware's line
 * register after. */
static int legacy_gsi(struct i915_device *i915)
{
	const pci_device_t *pci = i915->pci;
	uint32_t gsi = 0;
	uint8_t pin = pci->interrupt_pin;

	if (pin >= 1 && pin <= 4 && pci->bus == 0 &&
	    acpi_pci_lookup_irq("\\\\_SB_.PCI0", pci->device, (uint8_t)(pin - 1), &gsi) == 0 &&
	    gsi <= 23)
		return (int)gsi;
	if (pci->interrupt_line != 0xff && pci->interrupt_line <= 23)
		return pci->interrupt_line;
	return -1;
}

int i915_legacy_irq_init(struct i915_device *i915)
{
	int rc;

	legacy_irq_reset(i915);
	i915->irq_vector = -1;
	g_leg.irq_gsi = -1;
	if (leg_gen(i915) >= 5) {
		int vec = -1;
		rc = irq_request_msi(i915->pci, 0, legacy_irq, i915, "i915", &vec);
		if (rc == 0)
			i915->irq_vector = vec;
	} else {
		/* MSI is lost or delayed on the 965GM and dead on G4x: every
		 * part before Ironlake stays on its shared, level-triggered
		 * line */
		int gsi = legacy_gsi(i915);
		uint32_t cmd = pci_cfg_read32_dev(i915->pci, 0x04);
		if (cmd & PCI_CMD_INTX_DISABLE)
			pci_cfg_write32_dev(i915->pci, 0x04, cmd & ~PCI_CMD_INTX_DISABLE);
		rc = gsi < 0 ? -ENODEV : irq_request_gsi(gsi, legacy_irq, i915, "i915", 1, 1);
		if (rc == 0) {
			g_leg.irq_gsi = gsi;
			i915->irq_vector = 32 + gsi;
		}
	}
	if (rc) {
		kprintf("[drm] i915: no interrupt (%d); completions are polled\n", rc);
		return rc;
	}
	legacy_irq_postinstall(i915);
	i915_dbg("[drm] i915: interrupts on vector %d\n", i915->irq_vector);
	return 0;
}

void i915_legacy_irq_suspend(struct i915_device *i915)
{
	legacy_irq_reset(i915);
}

void i915_legacy_irq_resume(struct i915_device *i915)
{
	legacy_irq_reset(i915);
	if (i915->irq_vector < 0)
		return;
	legacy_irq_postinstall(i915);
}

void i915_legacy_irq_fini(struct i915_device *i915)
{
	legacy_irq_reset(i915);
	if (i915->irq_vector >= 0) {
		irq_free(i915->irq_vector, legacy_irq, i915);
		if (g_leg.irq_gsi < 0)
			irq_free_vector(i915->irq_vector);
		i915->irq_vector = -1;
	}
}
