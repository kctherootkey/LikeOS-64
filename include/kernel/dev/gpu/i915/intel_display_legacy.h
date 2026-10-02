// LikeOS -- the display backend of the Intel parts before DDI.
//
// Generation 2 (i830, i845G, i85x, i865G), 3 (i915G/GM, i945G/GM, G33,
// Pineview), 4 (i965G/GM, G45, GM45), Ironlake, Sandy Bridge, Ivy Bridge,
// Valleyview and Cherryview: the parts whose outputs hang off fixed
// function ports rather than DDIs.  The same DRM entry points as the DDI
// display (intel_display.h), backed by the per-pipe DPLLs, the
// PIPECONF-era pipes and planes, FDI and the PCH transcoders, and the
// analog, LVDS, SDVO, DVO, TV, HDMI, DisplayPort and DSI outputs.
//
// intel_display.c and i915_drv.c dispatch here when
// intel_legacy_display_init() accepted the device.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_DISPLAY_LEGACY_H
#define KERNEL_DEV_GPU_I915_INTEL_DISPLAY_LEGACY_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;

/* Bring the display up: chooses its model from the device info
 * (info->platform), reads the VBT, probes every output and adds a DRM
 * connector (and with it a CRTC) per output, panels first.  0, or
 * -ENODEV for a part this backend does not drive. */
int intel_legacy_display_init(struct i915_device *i915);
void intel_legacy_display_fini(struct i915_device *i915);
/* 1 once intel_legacy_display_init() succeeded on this device. */
int intel_legacy_display_active(struct i915_device *i915);
/* System sleep: every output off; and back to where a mode set can be
 * made (the core replays the committed state afterwards). */
void intel_legacy_display_suspend(struct i915_device *i915);
int intel_legacy_display_resume(struct i915_device *i915);
/* A GPU reset on gen2-4 (G4x excepted) resets the display too: every
 * output off before it, the display set up again and the committed state
 * shown again after it.  Called by the GT around such a reset. */
void intel_legacy_display_reset_prepare(struct i915_device *i915);
void intel_legacy_display_reset_finish(struct i915_device *i915);

/* The drm_driver entry points. */
int intel_legacy_atomic_check(struct drm_device *dev, struct drm_atomic_state *st);
int intel_legacy_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st);
int intel_legacy_detect(struct drm_device *dev, struct drm_connector *c);
int intel_legacy_get_modes(struct drm_device *dev, struct drm_connector *c);
int intel_legacy_display_verify(struct drm_device *dev);
void intel_legacy_display_fallback(struct drm_device *dev);
/* Gen2-7 primary plane constraints: linear everywhere, X tiled from
 * gen4 (the plane detiles itself); the stride limits of the generation. */
int intel_legacy_fb_check(struct drm_device *dev, struct drm_gem_object *o,
			  const struct drm_mode_fb_cmd2 *r, uint64_t *modifier);
/* What the primary planes scan out (for the drm_driver's lists). */
extern const uint32_t intel_legacy_fb_formats[];
extern const uint32_t intel_legacy_nfb_formats;
extern const uint64_t intel_legacy_fb_modifiers[];
extern const uint32_t intel_legacy_nfb_modifiers;
/* The largest cursor (square, ARGB) the part takes: 64 on gen2 and the
 * gen3 parts with the old cursor, else 256 (Ironlake on) or 64. */
uint32_t intel_legacy_cursor_max(struct i915_device *i915);

/* ---- interrupts --------------------------------------------------------------- */

/* The display's share of the top-level interrupt registers, for the
 * interrupt code to unmask:
 *   GMCH (gen2-4) and VLV/CHV: bits of GEN2_IER/IMR (VLV_IER/IMR): the
 *     pipe event bits and the display port (hotplug) bit; *ier_only 0.
 *   Ironlake to Ivy Bridge: DEIMR bits unmasked (and set in DEIER), with
 *     *ier_only the DEIER bits enabled but kept masked in DEIMR until the
 *     display unmasks them (vblanks, port A hotplug).  DE_MASTER_IRQ_CONTROL
 *     is included; the display never changes the master bit itself.
 * Call after intel_legacy_display_init(). */
uint32_t intel_legacy_irq_enable_bits(struct i915_device *i915, uint32_t *ier_only);
/* The display's own enables below the top-level registers: PIPESTAT,
 * PORT_HOTPLUG_EN; on Ironlake to Ivy Bridge DEIMR and the display bits of
 * DEIER (the master bit kept as found) and the south display's SDEIMR /
 * SDEIER.  intel_legacy_display_init() and _resume() call it; the
 * interrupt code may call it again after a reset of those registers. */
void intel_legacy_irq_postinstall(struct i915_device *i915);
/* Mask and clear the display's sources (PIPESTAT, PORT_HOTPLUG_STAT,
 * SDE, GEN7_ERR_INT) at interrupt reset / uninstall. */
void intel_legacy_irq_reset(struct i915_device *i915);
/* The display part of the interrupt handler (process every display
 * source, acknowledging it).
 *   GMCH and VLV/CHV: call it before writing back GEN2_IIR (VLV_IIR):
 *     the pipe status and hotplug status registers are cleared here, and
 *     the IIR bits would re-assert otherwise.
 *   Ironlake to Ivy Bridge: call it with the DE master disabled and
 *     SDEIER zeroed; DEIIR (except DE_PCU_EVENT, left to the caller),
 *     SDEIIR and GEN7_ERR_INT are read and cleared here. */
void intel_legacy_display_irq(struct i915_device *i915);
/* Start the hotplug worker (once the scheduler runs: from late init). */
int intel_legacy_hpd_start(struct i915_device *i915);

#endif
