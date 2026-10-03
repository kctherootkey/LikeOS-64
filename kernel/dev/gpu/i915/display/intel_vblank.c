// LikeOS -- vblank of the Intel display pipes.
//
// Each pipe raises a vblank interrupt (and a FIFO underrun one) through
// its GEN8_DE_PIPE_* registers (Haswell: through the north display's
// DEIMR / DEIER bits).  The interrupt handler hands the bits to
// intel_display_irq(), which ticks the core's vblank counter of the crtc
// the pipe serves and keeps the underrun interrupt from flooding the
// machine.  The core's vblank watchdog asks intel_display_vblank_report()
// when a crtc goes quiet.
//
// With I915_FEAT_VBLANK_HW the core gets the vblank hardware itself: the
// pipe's frame counter (PIPE_FRMCOUNT) as the vblank count, the scanline
// counter (PIPEDSL) for timestamps that do not depend on interrupt
// latency, and the vblank interrupt switched on and off as the core's
// users need it.  A crtc's index is its connector's, not a pipe, and a
// crtc may be driven by another pipe after the next mode set: every hook
// maps crtc -> output -> pipe when it is called.  The mode set switches a
// crtc's vblank handling on once its pipe runs (intel_vblank_crtc_on())
// and off before the pipe stops (intel_vblank_crtc_off()).  Without the
// switch the vblank interrupt is unmasked for as long as the pipe runs
// and the core counts the interrupts, as it always did.
//
// Every read-modify-write of a pipe's interrupt mask and enable registers
// -- the vblank switch, the underrun masking in the interrupt handler and
// its re-arm, the watchdog's repair -- happens under one spinlock, taken
// with interrupts off, so that an update from one processor cannot undo
// one from another.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2023 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/intel_vblank.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/drm_vblank.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* The scanline counter, the frame time stamp of the last vblank start and
 * the free-running time stamp it is taken from (microseconds). */
#ifndef PIPEDSL_LINE_MASK
#define PIPEDSL_LINE_MASK 0xfffffu
#endif
#ifndef PIPE_FRMTMSTMP
#define PIPE_FRMTMSTMP(pipe) (0x70048 + (pipe) * 0x1000)
#endif
#ifndef IVB_TIMESTAMP_CTR
#define IVB_TIMESTAMP_CTR 0x44070
#endif

/* Haswell has no per-pipe interrupt registers: a pipe's vblank is a bit
 * of the north display's mask and enable. */
#define HSW_DEISR 0x44000
#define HSW_DEIMR 0x44004
#define HSW_DEIIR 0x44008
#define HSW_DEIER 0x4400c
#define HSW_DE_PIPE_VBLANK(pipe) (1u << ((pipe) * 5))

/* The lock of every read-modify-write of the display interrupt mask and
 * enable registers.  Taken from the vblank hooks (under the core's vblank
 * lock), from the interrupt handler and from process context, always
 * with interrupts off and never around a call into the vblank core. */
static spinlock_t g_de_irq_lock = SPINLOCK_INIT("i915_de_irq");

static int vbl_is_hsw(const struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_HASWELL;
}

/* ---- the interrupt registers (lock held) -------------------------------------- */

/* The pipe's IMR with the `mask' bits set to unmasked where `enabled' has
 * them.  The register is read rather than cached: a power well that went
 * down and came back has it at its reset value, and the watchdog's repair
 * goes by what the hardware holds. */
static void bdw_update_pipe_imr_locked(struct i915_device *i915, int pipe, uint32_t mask,
				       uint32_t enabled)
{
	uint32_t old = i915_read32_fw(i915, GEN8_DE_PIPE_IMR(pipe));
	uint32_t new_val = (old & ~mask) | (~enabled & mask);

	if (new_val != old) {
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(pipe), new_val);
		(void)i915_read32_fw(i915, GEN8_DE_PIPE_IMR(pipe));
	}
}

static void bdw_update_pipe_ier_locked(struct i915_device *i915, int pipe, uint32_t mask,
				       uint32_t enabled)
{
	uint32_t old = i915_read32_fw(i915, GEN8_DE_PIPE_IER(pipe));
	uint32_t new_val = (old & ~mask) | (enabled & mask);

	if (new_val != old) {
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(pipe), new_val);
		(void)i915_read32_fw(i915, GEN8_DE_PIPE_IER(pipe));
	}
}

/* Haswell: the pipe's vblank bit of DEIMR. */
static void hsw_update_vblank_imr_locked(struct i915_device *i915, int pipe, int on)
{
	uint32_t bit = HSW_DE_PIPE_VBLANK(pipe);
	uint32_t old = i915_read32_fw(i915, HSW_DEIMR);
	uint32_t new_val = on ? old & ~bit : old | bit;

	if (new_val != old) {
		i915_write32_fw(i915, HSW_DEIMR, new_val);
		(void)i915_read32_fw(i915, HSW_DEIMR);
	}
}

/* ---- vblank interrupts as they always were --------------------------------------- */

/* Without I915_FEAT_VBLANK_HW: vblank and underrun unmasked for as long
 * as the pipe runs. */
static void pipe_vblank_enable(struct i915_device *i915, struct intel_pipe *p, int on)
{
	uint32_t bits = GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN;
	uint64_t fl;

	spin_lock_irqsave(&g_de_irq_lock, &fl);
	uint32_t imr = i915_read32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe));
	uint32_t ier = i915_read32_fw(i915, GEN8_DE_PIPE_IER(p->pipe));
	if (on) {
		i915_write32_fw(i915, GEN8_DE_PIPE_IIR(p->pipe), bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe), imr & ~bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(p->pipe), ier | bits);
	} else {
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe), imr | bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(p->pipe), ier & ~bits);
	}
	(void)i915_read32_fw(i915, GEN8_DE_PIPE_IER(p->pipe));
	p->vblank_enabled = on;
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
}

/* ---- the pipe of a crtc ------------------------------------------------------------ */

static uint32_t intel_vblank_get_counter(struct drm_device *dev, int crtc);
static bool intel_vblank_scanout_position(struct drm_device *dev, int crtc, bool in_vblank_irq,
					  int *vpos, int *hpos, uint64_t *stime_ns,
					  uint64_t *etime_ns, const struct drm_display_mode *mode);
static int intel_vblank_enable(struct drm_device *dev, int crtc);
static void intel_vblank_disable(struct drm_device *dev, int crtc);

/* Does the device's driver table hand the vblank hardware to the core
 * (intel_vblank_driver_setup() put the hooks in)? */
static int vbl_hw(struct i915_device *i915)
{
	return i915->drm.drv && i915->drm.drv->enable_vblank == intel_vblank_enable;
}

/* The pipe that drives crtc `crtc' now, with its vblank handling on:
 * crtc -> output (a crtc's index is its connector's) -> pipe.  NULL while
 * the crtc runs on no pipe.  Interrupts off or the display lock held;
 * the fields are read once each, since a mode set may be taking the
 * output down on another processor. */
static struct intel_pipe *vbl_pipe_for_crtc(struct i915_device *i915, int crtc)
{
	struct intel_output *o = intel_output_for_crtc(i915, crtc);
	struct intel_pipe *p;
	int pipe;

	if (!o || !*(volatile int *)&o->active)
		return NULL;
	pipe = *(volatile int *)&o->pipe;
	if (pipe < 0 || pipe >= INTEL_MAX_PIPES)
		return NULL;
	p = &i915->display.pipes[pipe];
	if (!*(volatile int *)&p->active || !*(volatile uint8_t *)&p->vbl.on ||
	    *(volatile int8_t *)&p->vbl.crtc != crtc)
		return NULL;
	return p;
}

/* ---- the core's vblank hooks (core vblank lock held, interrupts off) ------------- */

static uint32_t intel_vblank_get_counter(struct drm_device *dev, int crtc)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_pipe *p = vbl_pipe_for_crtc(i915, crtc);

	/* No pipe, or a pipe whose power well is down: its counter reads
	 * as nothing useful. */
	if (!p || i915->display.pw_count[INTEL_PW_PIPE_A + p->pipe] <= 0)
		return 0;
	return i915_read32_fw(i915, PIPE_FRMCOUNT(p->pipe));
}

/* The vertical timings as the scanline counter sees them, an interlaced
 * mode scanning one field. */
static int vbl_mode_vtotal(const struct drm_display_mode *mode)
{
	int vtotal = mode->crtc_vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		vtotal /= 2;
	return vtotal;
}

static int vbl_mode_vblank_start(const struct drm_display_mode *mode)
{
	int vblank_start = mode->crtc_vblank_start;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		vblank_start = DIV_ROUND_UP(vblank_start, 2);
	return vblank_start;
}

static int vbl_mode_vblank_end(const struct drm_display_mode *mode)
{
	int vblank_end = mode->crtc_vblank_end;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		vblank_end /= 2;
	return vblank_end;
}

/* Lines scanned since the start of the last vblank, from the frame time
 * stamp the pipe latched there and the time stamp counter now (both in
 * microseconds); read again when a vblank started between the two
 * reads. */
static uint32_t vbl_scanlines_since_frame_timestamp(struct i915_device *i915, int pipe,
						    const struct drm_display_mode *mode)
{
	uint32_t htotal = mode->crtc_htotal;
	uint32_t clock = (uint32_t)mode->crtc_clock;
	uint32_t scan_prev_time, scan_curr_time, scan_post_time;
	int tries = 0;

	do {
		scan_prev_time = i915_read32_fw(i915, PIPE_FRMTMSTMP(pipe));
		scan_curr_time = i915_read32_fw(i915, IVB_TIMESTAMP_CTR);
		scan_post_time = i915_read32_fw(i915, PIPE_FRMTMSTMP(pipe));
	} while (scan_post_time != scan_prev_time && ++tries < 100);

	if (!htotal)
		return 0;
	return (uint32_t)div_u64((uint64_t)(scan_curr_time - scan_prev_time) * clock,
				 1000u * htotal);
}

/* The line being scanned out (0 = the first active line), from PIPEDSL
 * corrected by the transcoder's scanline offset. */
static int vbl_get_scanline(struct i915_device *i915, const struct intel_pipe *p, int vtotal)
{
	int position = (int)(i915_read32_fw(i915, PIPE_DSL(p->pipe)) & PIPEDSL_LINE_MASK);

	/* The DSL register reads 0 when read just before the start of
	 * vblank; read it again so that a reading there does not seem to
	 * jump a whole frame back. */
	if (!position) {
		for (int i = 0; i < 100; i++) {
			lapic_delay_us(1);
			int temp = (int)(i915_read32_fw(i915, PIPE_DSL(p->pipe)) & PIPEDSL_LINE_MASK);
			if (temp != position) {
				position = temp;
				break;
			}
		}
	}
	return (position + vtotal + p->vbl.scanline_offset) % vtotal;
}

static bool intel_vblank_scanout_position(struct drm_device *dev, int crtc, bool in_vblank_irq,
					  int *vpos, int *hpos, uint64_t *stime_ns,
					  uint64_t *etime_ns, const struct drm_display_mode *mode)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_pipe *p = vbl_pipe_for_crtc(i915, crtc);
	int position, vbl_start, vbl_end, vtotal;
	int vrr = 0, vmax_vblank_start = 0;
	uint64_t fl;

	(void)in_vblank_irq;
	if (!p || !mode->crtc_clock)
		return false;

	vtotal = vbl_mode_vtotal(mode);
	vbl_start = vbl_mode_vblank_start(mode);
	vbl_end = vbl_mode_vblank_end(mode);
	/* Variable refresh: the frame lasts up to the range's maximum, and
	 * the vblank starts a guard band before the minimum. */
	if (p->vrr.enabled && p->vrr.vmax && p->vrr.vmin && p->vrr.guardband < p->vrr.vmin) {
		vrr = 1;
		vtotal = p->vrr.vmax;
		vbl_end = p->vrr.vmax;
		vbl_start = p->vrr.vmin - p->vrr.guardband;
		vmax_vblank_start = p->vrr.vmax - p->vrr.guardband;
	}
	if (vtotal <= 0)
		return false;

	fl = local_irq_save();
	if (stime_ns)
		*stime_ns = hrtimer_now_ns();
	if (vrr) {
		int scanlines = (int)vbl_scanlines_since_frame_timestamp(i915, p->pipe, mode);

		position = vbl_get_scanline(i915, p, vtotal);
		/* Already leaving the vblank: place the position as if the
		 * full vblank of the range's maximum were ending, which is
		 * when the active part starts. */
		if (position >= vbl_start && scanlines < position)
			position = min(vmax_vblank_start + scanlines, vtotal - 1);
	} else {
		position = vbl_get_scanline(i915, p, vtotal);
	}
	if (etime_ns)
		*etime_ns = hrtimer_now_ns();
	local_irq_restore(fl);

	/* Negative inside the vblank, counting up to 0 at its end; positive
	 * from there on. */
	if (position >= vbl_start)
		position -= vbl_end;
	else
		position += vtotal - vbl_end;
	*vpos = position;
	*hpos = 0;
	return true;
}

static int intel_vblank_enable(struct drm_device *dev, int crtc)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_pipe *p = vbl_pipe_for_crtc(i915, crtc);
	uint64_t fl;

	if (!p)
		return -EINVAL;
	spin_lock_irqsave(&g_de_irq_lock, &fl);
	if (vbl_is_hsw(i915)) {
		hsw_update_vblank_imr_locked(i915, p->pipe, 1);
	} else {
		/* The enable too: it sits in the pipe's power well with the
		 * mask, and is set at crtc_on only. */
		bdw_update_pipe_ier_locked(i915, p->pipe, GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK);
		bdw_update_pipe_imr_locked(i915, p->pipe, GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK);
	}
	p->vblank_enabled = 1;
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
	return 0;
}

static void intel_vblank_disable(struct drm_device *dev, int crtc)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_pipe *p = vbl_pipe_for_crtc(i915, crtc);
	uint64_t fl;

	if (!p)
		return;
	spin_lock_irqsave(&g_de_irq_lock, &fl);
	if (vbl_is_hsw(i915))
		hsw_update_vblank_imr_locked(i915, p->pipe, 0);
	else
		bdw_update_pipe_imr_locked(i915, p->pipe, GEN8_PIPE_VBLANK, 0);
	p->vblank_enabled = 0;
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
}

/* ---- the interrupt and the watchdog --------------------------------------------- */

void intel_display_irq(struct i915_device *i915, int pipe, uint32_t iir)
{
	struct intel_pipe *p = &i915->display.pipes[pipe];
	if (iir & GEN8_PIPE_VBLANK) {
		p->vblank_irqs++;
		/* read once: a mode set on another processor may be taking
		 * the pipe down under this interrupt */
		int out = *(volatile int *)&p->output;
		if (p->active && out >= 0 && out < i915->display.nout) {
			struct intel_output *o = &i915->display.outputs[out];
			/* the crtc whose vblank handling the pipe was switched
			 * on for, else the one its output serves */
			int crtc = *(volatile uint8_t *)&p->vbl.on ?
					   *(volatile int8_t *)&p->vbl.crtc :
					   *(volatile int *)&o->crtc;
			if (crtc >= 0)
				drm_vblank_tick(&i915->drm, crtc);
		}
		p->flip_pending = 0;
	}
	if (iir & GEN8_PIPE_FIFO_UNDERRUN) {
		p->underruns++;
		/* A plane that cannot keep up underruns on EVERY line, which
		 * is half a million interrupts a second -- enough to starve
		 * the machine that is trying to fix it.  The first one says
		 * everything the rest would; mask it and keep counting the
		 * ones the status register still records. */
		if (!p->underrun_reported) {
			uint64_t fl;

			spin_lock_irqsave(&g_de_irq_lock, &fl);
			bdw_update_pipe_imr_locked(i915, pipe, GEN8_PIPE_FIFO_UNDERRUN, 0);
			bdw_update_pipe_ier_locked(i915, pipe, GEN8_PIPE_FIFO_UNDERRUN, 0);
			p->underrun_reported = 1;
			spin_unlock_irqrestore(&g_de_irq_lock, fl);
		}
	}
}

void intel_display_vblank_report(struct drm_device *dev, int crtc)
{
	struct i915_device *i915 = to_i915(dev);
	static uint32_t said;

	if (intel_legacy_display_active(i915) || !i915->display.ready)
		return;
	for (int k = 0; k < i915->display.nout; k++) {
		struct intel_output *o = &i915->display.outputs[k];
		if (o->crtc != crtc || o->pipe < 0 || o->pipe >= INTEL_MAX_PIPES)
			continue;
		struct intel_pipe *p = &i915->display.pipes[o->pipe];
		int hsw = vbl_is_hsw(i915) && vbl_hw(i915);
		uint32_t imr, ier, isr, iir, vbit;
		uint64_t fl;

		if (hsw) {
			vbit = HSW_DE_PIPE_VBLANK(p->pipe);
			imr = i915_read32_fw(i915, HSW_DEIMR);
			ier = i915_read32_fw(i915, HSW_DEIER);
			isr = i915_read32_fw(i915, HSW_DEISR);
			iir = i915_read32_fw(i915, HSW_DEIIR);
		} else {
			vbit = GEN8_PIPE_VBLANK;
			imr = i915_read32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe));
			ier = i915_read32_fw(i915, GEN8_DE_PIPE_IER(p->pipe));
			isr = i915_read32_fw(i915, GEN8_DE_PIPE_ISR(p->pipe));
			iir = i915_read32_fw(i915, GEN8_DE_PIPE_IIR(p->pipe));
		}
		/* Lost only where the interrupt is meant to be on: with the
		 * vblank hooks it is switched off between users on purpose. */
		int lost = p->active && p->vblank_enabled && ((imr & vbit) || !(ier & vbit));
		if (said < 4) {
			said++;
			kprintf("[drm] i915: pipe %c (crtc %d): %u vblank interrupts so far, %u interrupts in all; %s IMR %08x IER %08x ISR %08x IIR %08x, display control %08x, graphics master %08x%s\n",
				'A' + p->pipe, crtc, p->vblank_irqs, (unsigned)i915->irq_count,
				hsw ? "display" : "pipe", imr, ier, isr, iir,
				i915->info->gen >= 11 ? i915_read32_fw(i915, GEN11_DISPLAY_INT_CTL) : 0,
				i915->info->gen >= 11 ? i915_read32_fw(i915, GEN11_GFX_MSTR_IRQ) :
							i915_read32_fw(i915, GEN8_MASTER_IRQ),
				lost ? "; the vblank enable was lost and is set again" : "");
		}
		/* A pipe's interrupt registers live in its power well: a
		 * well that went down and came back has them at their reset
		 * values, everything masked.  Looked at again under the lock:
		 * the interrupt may have been switched off meanwhile. */
		if (lost) {
			spin_lock_irqsave(&g_de_irq_lock, &fl);
			if (p->active && p->vblank_enabled) {
				if (hsw) {
					hsw_update_vblank_imr_locked(i915, p->pipe, 1);
				} else {
					i915_write32_fw(i915, GEN8_DE_PIPE_IIR(p->pipe), GEN8_PIPE_VBLANK);
					bdw_update_pipe_imr_locked(i915, p->pipe, GEN8_PIPE_VBLANK,
								   GEN8_PIPE_VBLANK);
					bdw_update_pipe_ier_locked(i915, p->pipe, GEN8_PIPE_VBLANK,
								   GEN8_PIPE_VBLANK);
				}
			}
			spin_unlock_irqrestore(&g_de_irq_lock, fl);
		}
		return;
	}
}

/* ---- the entry points intel_display.c calls --------------------------------- */

void intel_vblank_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	/* The base table's hw_vblank and vblank_report stay.  The displays
	 * before the DDI kind set their own hooks. */
	if (!I915_FEAT_VBLANK_HW || intel_display_legacy_part(i915))
		return;
	drv->get_vblank_counter = intel_vblank_get_counter;
	/* the counter width is set per crtc at crtc_on */
	drv->max_vblank_count = 0;
	drv->get_scanout_position = intel_vblank_scanout_position;
	drv->enable_vblank = intel_vblank_enable;
	drv->disable_vblank = intel_vblank_disable;
}

void intel_vblank_crtc_reset(struct i915_device *i915, int crtc)
{
	if (!vbl_hw(i915))
		return;
	drm_crtc_vblank_reset(&i915->drm, crtc);
}

/* The scanline counter runs one line behind; on an HDMI transcoder of
 * Haswell up to display version 20 two. */
static uint8_t vbl_scanline_offset(struct i915_device *i915, const struct intel_pipe *p)
{
	const struct intel_output *o = NULL;

	if (i915->info->display_ver >= 20 || i915->info->platform == I915_PLATFORM_BATTLEMAGE)
		return 1;
	if (p->output >= 0 && p->output < i915->display.nout)
		o = &i915->display.outputs[p->output];
	return o && (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) ? 2 : 1;
}

/* The pipe's interrupts as a running pipe has them before anybody asks
 * for vblanks: underrun reported, vblank masked (the core unmasks it
 * through intel_vblank_enable()), nothing stale pending. */
static void vbl_pipe_irq_arm(struct i915_device *i915, struct intel_pipe *p)
{
	uint64_t fl;

	spin_lock_irqsave(&g_de_irq_lock, &fl);
	if (vbl_is_hsw(i915)) {
		hsw_update_vblank_imr_locked(i915, p->pipe, 0);
	} else {
		uint32_t bits = GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN;

		i915_write32_fw(i915, GEN8_DE_PIPE_IIR(p->pipe), bits);
		/* an underrun already reported stays masked until the next
		 * watermarks re-arm it (intel_vblank_underrun_rearm()) */
		bdw_update_pipe_imr_locked(i915, p->pipe, bits,
					   p->underrun_reported ? 0 : GEN8_PIPE_FIFO_UNDERRUN);
		bdw_update_pipe_ier_locked(i915, p->pipe, bits, bits);
	}
	p->vblank_enabled = 0;
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
}

/* And everything off again before the pipe stops. */
static void vbl_pipe_irq_disarm(struct i915_device *i915, struct intel_pipe *p)
{
	uint64_t fl;

	spin_lock_irqsave(&g_de_irq_lock, &fl);
	if (vbl_is_hsw(i915)) {
		hsw_update_vblank_imr_locked(i915, p->pipe, 0);
	} else {
		uint32_t bits = GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN;

		bdw_update_pipe_imr_locked(i915, p->pipe, bits, 0);
		bdw_update_pipe_ier_locked(i915, p->pipe, bits, 0);
	}
	p->vblank_enabled = 0;
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
}

/* The core's timing constants from the mode the transcoder runs: the
 * frame and line duration, and the timings the scanout position is
 * measured against.  From Alder Lake-P the vblank starts the set context
 * latency after the active area. */
static void vbl_timing_constants(struct i915_device *i915, struct intel_pipe *p, int crtc)
{
	struct drm_display_mode *m = kalloc(sizeof(*m));

	if (!m)
		return;
	if (drm_mode_from_umode(m, &p->mode) == 0) {
		drm_mode_set_crtcinfo(m, 0);
		if (i915->info->display_ver >= 13) {
			uint32_t scl = intel_set_context_latency(i915);
			if (m->crtc_vblank_start + scl < m->crtc_vblank_end)
				m->crtc_vblank_start = (uint16_t)(m->crtc_vblank_start + scl);
		}
		drm_calc_timestamping_constants(&i915->drm, crtc, m);
	}
	kfree(m);
}

void intel_vblank_crtc_on(struct i915_device *i915, struct intel_pipe *p, int crtc)
{
	struct drm_device *dev = &i915->drm;
	const struct drm_vblank_crtc_config config = {
#if I915_FEAT_VBLANK_IRQ_OFF
		.offdelay_ms = DRM_VBLANK_OFFDELAY_MS,
		.disable_immediate = true,
#else
		.offdelay_ms = 0,
		.disable_immediate = false,
#endif
	};

	if (!vbl_hw(i915)) {
		pipe_vblank_enable(i915, p, 1);
		return;
	}
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc) {
		pipe_vblank_enable(i915, p, 1);
		return;
	}
	p->vbl.crtc = (int8_t)crtc;
	p->vbl.scanline_offset = vbl_scanline_offset(i915, p);
	vbl_pipe_irq_arm(i915, p);
	vbl_timing_constants(i915, p, crtc);
	p->vbl.on = 1;
	/* the full 32-bit frame counter; set while the crtc is still off */
	drm_crtc_set_max_vblank_count(dev, crtc, 0xffffffffu);
	drm_crtc_vblank_on_config(dev, crtc, &config);
}

void intel_vblank_crtc_off(struct i915_device *i915, struct intel_pipe *p, int crtc)
{
	if (!vbl_hw(i915)) {
		pipe_vblank_enable(i915, p, 0);
		return;
	}
	/* The crtc the pipe was switched on for: the output's own record
	 * may already have been cleared by the caller. */
	if (p->vbl.on) {
		int on_crtc = p->vbl.crtc;

		if (crtc >= 0 && crtc != on_crtc)
			kprintf("[drm] i915: pipe %c: vblank off for crtc %d, but it serves crtc %d\n",
				'A' + p->pipe, crtc, on_crtc);
		/* pending events answered, the interrupt off through the
		 * disable hook while the pipe can still be found */
		drm_crtc_vblank_off(&i915->drm, on_crtc);
		p->vbl.on = 0;
	}
	vbl_pipe_irq_disarm(i915, p);
}

void intel_vblank_underrun_rearm(struct i915_device *i915, struct intel_pipe *p)
{
	uint64_t fl;

	if (!p->underrun_reported)
		return;
	spin_lock_irqsave(&g_de_irq_lock, &fl);
	if (p->underrun_reported) {
		bdw_update_pipe_imr_locked(i915, p->pipe, GEN8_PIPE_FIFO_UNDERRUN,
					   GEN8_PIPE_FIFO_UNDERRUN);
		bdw_update_pipe_ier_locked(i915, p->pipe, GEN8_PIPE_FIFO_UNDERRUN,
					   GEN8_PIPE_FIFO_UNDERRUN);
		p->underrun_reported = 0;
	}
	spin_unlock_irqrestore(&g_de_irq_lock, fl);
}
