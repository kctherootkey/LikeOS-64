// LikeOS -- compile-time switches of the Intel display driver's features.
//
// Every display feature that changes what the driver does on a machine
// it already drives has a switch here.  1 turns the feature on (the full
// behaviour of the feature); 0 keeps the driver as it was before the
// feature existed, which is the way back when a feature misbehaves on a
// particular machine.  Each switch is a block of its own so a build can
// override one with -D or by editing its value; every value must build
// without warnings.
//
// A switch whose code has not landed yet does nothing either way: the
// stubs behind it keep the old behaviour until the feature's code
// replaces them, and only then does the value start to matter.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_FEATURES_H
#define KERNEL_DEV_GPU_I915_INTEL_FEATURES_H

/* ---- planes (intel_plane.c, intel_plane_props.c) ----------------------------- */

/* The plane properties of the universal planes: rotation and reflection
 * per platform, the immutable zpos, colour encoding and range, with the
 * matching PLANE_CTL / PLANE_COLOR_CTL programming and the 180-degree
 * surface offset arithmetic.  Also: planes clipped to the mode instead
 * of refused when partly outside it (the primary of Skylake and later
 * may sit anywhere, Broadwell's must cover the pipe; the cursor goes off
 * when wholly outside), with the watermarks and the display buffer sized
 * from the clipped plane; Gemini Lake's plane colour/alpha bits in
 * PLANE_COLOR_CTL; and Haswell/Broadwell linear offsets through
 * DSPOFFSET.  0: the planes have none of these properties and scan out
 * unrotated, as before. */
#ifndef I915_FEAT_PLANE_PROPS
#define I915_FEAT_PLANE_PROPS 1
#endif

/* Plane alpha and pixel blend mode ("alpha", "pixel blend mode") on the
 * universal planes, programmed through the plane's alpha mode bits.
 * 0: alpha is ignored (opaque planes), as before. */
#ifndef I915_FEAT_PLANE_ALPHA
#define I915_FEAT_PLANE_ALPHA 1
#endif

/* FB_DAMAGE_CLIPS on the planes of display version 12 and later: the
 * property is offered and accepted.  0: no damage clips property. */
#ifndef I915_FEAT_DAMAGE_CLIPS
#define I915_FEAT_DAMAGE_CLIPS 1
#endif

/* A discrete card's scanout shadow in local memory refreshed only over
 * the damaged rectangles instead of the whole surface.  0: the whole
 * copy is refreshed on every commit, as before. */
#ifndef I915_FEAT_LMEM_DAMAGE
#define I915_FEAT_LMEM_DAMAGE 1
#endif

/* ---- colour (intel_color.c) ----------------------------------------------------- */

/* Colour management: DEGAMMA_LUT, CTM and GAMMA_LUT at the platform's
 * sizes and precisions, the pipe CSC, GAMMA_MODE / CSC_MODE and the plane
 * gamma and CSC enable bits.  0: the 8-bit legacy palette only, as
 * before. */
#ifndef I915_FEAT_COLOR_MGMT
#define I915_FEAT_COLOR_MGMT 1
#endif

/* Ice Lake and later: the multi-segmented gamma table (the 262145-entry
 * GAMMA_LUT).  0: those parts offer the 10-bit 1024-entry table instead.
 * Only meaningful with I915_FEAT_COLOR_MGMT. */
#ifndef I915_FEAT_COLOR_ICL_MULTISEG
#define I915_FEAT_COLOR_ICL_MULTISEG 1
#endif

/* ---- vblank, probing and fences (intel_vblank.c, intel_connector.c) ------- */

/* The hardware frame counter and scanout position as the vblank count
 * and timestamp source, the vblank interrupt switched on and off as
 * clients need it (under an interrupt lock), and the core's
 * vblank on/off/reset bookkeeping per crtc.  0: the vblank interrupt is
 * unmasked for as long as the pipe runs and the core counts the
 * interrupts, as before. */
#ifndef I915_FEAT_VBLANK_HW
#define I915_FEAT_VBLANK_HW 1
#endif

/* The core's probe helper for the connectors: detect, get_modes, then
 * every mode put through intel_mode_valid() (link bandwidth, CDCLK,
 * downstream port and HDMI limits, panel scaler bounds), deduplicated
 * and sorted.  0: the driver fills the mode list itself, as before. */
#ifndef I915_FEAT_PROBE_HELPER
#define I915_FEAT_PROBE_HELPER 1
#endif

/* The embedded panel's mode list carries the standard modes up to the
 * panel's size, each scaled onto the panel's own timing by the pipe.
 * This is what the driver has always done and stays the default; 0
 * offers only the panel's own modes. */
#ifndef I915_FEAT_EDP_STD_MODES
#define I915_FEAT_EDP_STD_MODES 1
#endif

/* When the vblank interrupt of a running crtc goes off.
 *
 * 1: at the vblank after its last user dropped its reference (the counter
 * and the vblank timestamps come from the hardware frame counter and the
 * scanline, so they stay exact across the gap).  The core takes the
 * reference of a flip event and of an atomic out-fence before it reads
 * the count they wait for, so neither completes early after a gap
 * (DRM_FLIP_EVENT_VBLANK_REF and DRM_OUT_FENCE_VBLANK_REF, drm_internal.h:
 * with either of those at 0 this switch should be 0 too).
 * 0: the interrupt stays on for as long as the crtc runs, as before; the
 * frame counter, scanout position and locking are the same.
 * Only meaningful with I915_FEAT_VBLANK_HW.  The displays before the DDI
 * kind always switch theirs this way (I915_FEAT_LEGACY_VBLANK_HW). */
#ifndef I915_FEAT_VBLANK_IRQ_OFF
#define I915_FEAT_VBLANK_IRQ_OFF 1
#endif

/* IN_FENCE_FD / OUT_FENCE_PTR on the atomic interface, on every part
 * (the older displays commit through the atomic interface too).  0: the
 * properties are not offered. */
#ifndef I915_FEAT_ATOMIC_FENCES
#define I915_FEAT_ATOMIC_FENCES 1
#endif

/* ---- the displays before the DDI kind (intel_legacy_*.c) --------------------- */

/* Their hardware frame counters (32-bit from G4X and Ironlake, 24-bit on
 * gen3/4, none on gen2) and scanlines as the vblank source, and the
 * vblank interrupt switched under a real lock.  0: as before. */
#ifndef I915_FEAT_LEGACY_VBLANK_HW
#define I915_FEAT_LEGACY_VBLANK_HW 1
#endif

/* Rotation by 180 degrees, reflection, blending and zpos on their
 * planes.  0: as before (unrotated, opaque). */
#ifndef I915_FEAT_LEGACY_PLANE_PROPS
#define I915_FEAT_LEGACY_PLANE_PROPS 1
#endif

/* Their colour management (i9xx/i965/Ironlake/Ivy Bridge/Valleyview/
 * Cherryview gamma, degamma and CSC).  0: the 8-bit palette, as
 * before. */
#ifndef I915_FEAT_LEGACY_COLOR_MGMT
#define I915_FEAT_LEGACY_COLOR_MGMT 1
#endif

/* The DisplayPort helper library under their DP/eDP ports: the AUX
 * channel as a struct drm_dp_aux (the library's retries, probe read and
 * I2C over AUX for the EDID), receiver caps, sink identification and
 * quirks, sink count and downstream ports, link status and training
 * decoders, and short pulses that read and acknowledge the sink's event
 * vectors and reprobe on sink count or capability changes.  0: the
 * driver's own AUX, I2C and training code, as before. */
#ifndef I915_FEAT_LEGACY_DP_HELPERS
#define I915_FEAT_LEGACY_DP_HELPERS 1
#endif

/* After a passed training, grant a DPCD 1.3+ sink that asks for it its
 * post-training swing/pre-emphasis adjustments.  0: no grant, as before.
 * Only meaningful with I915_FEAT_LEGACY_DP_HELPERS. */
#ifndef I915_FEAT_LEGACY_DP_POST_LT_ADJ_REQ
#define I915_FEAT_LEGACY_DP_POST_LT_ADJ_REQ 1
#endif

/* DP++ adaptors on their HDMI ports: type read over DDC, TMDS limit
 * honoured, a type 2 adaptor's TMDS output switched with the port.
 * 0: an adaptor is treated as a plain HDMI sink, as before. */
#ifndef I915_FEAT_LEGACY_HDMI_DUAL_MODE
#define I915_FEAT_LEGACY_HDMI_DUAL_MODE 1
#endif

/* AVI/SPD/vendor infoframes built by the HDMI infoframe library.
 * 0: the driver's own packing, as before. */
#ifndef I915_FEAT_LEGACY_HDMI_INFOFRAME_LIB
#define I915_FEAT_LEGACY_HDMI_INFOFRAME_LIB 1
#endif

/* ---- DisplayPort (intel_dp.c, intel_dp_aux.c, intel_dp_link_training.c) -- */

/* The DisplayPort helper library underneath the driver's DP code: DPCD
 * access with retries, I2C over AUX, link status and training helpers,
 * downstream port limits, the hotplug link check.  0: the driver's own
 * AUX and training code, as before. */
#ifndef I915_FEAT_DP_HELPERS
#define I915_FEAT_DP_HELPERS 1
#endif

/* Read the extended receiver capabilities (DPCD 0x2200) where the sink
 * says it has them.  0: the base capabilities only, as before.  Only
 * meaningful with I915_FEAT_DP_HELPERS. */
#ifndef I915_FEAT_DP_EXT_CAPS
#define I915_FEAT_DP_EXT_CAPS 1
#endif

/* Link-training tunable PHY repeaters: 0 leaves them alone (as before),
 * 1 trains through them in transparent mode, 2 trains each repeater in
 * non-transparent mode where there are any.  Only meaningful with
 * I915_FEAT_DP_HELPERS. */
#ifndef I915_FEAT_DP_LTTPR
#define I915_FEAT_DP_LTTPR 2
#endif

/* The board's DisplayPort limits from the VBT's child device of the port:
 * the highest link rate it is wired for (BDB 216+) and the most lanes
 * (BDB 244+) bound the links the driver picks, checks a mode against and
 * offers modes for.  0: only the sink's, the PHY's and the strap's limits
 * apply, as before. */
#ifndef I915_FEAT_DP_VBT_LIMITS
#define I915_FEAT_DP_VBT_LIMITS 1
#endif

/* ---- HDMI (intel_hdmi.c, intel_infoframe.c) --------------------------------- */

/* HDMI 2.0: TMDS clocks above 340 MHz with scrambling and the 1/40 bit
 * clock ratio set over SCDC, up to the platform's limit (600 MHz at most);
 * the sink's EDID maximum TMDS clock and the VBT's per-port HDMI limit are
 * honoured too.  0: only the old limits apply -- TMDS clamped at 340 MHz
 * (165 MHz for a DVI sink) -- and the sink's SCDC is not touched, as
 * before.  The DP++ adaptor limits (I915_FEAT_DP_DUAL_MODE) are a switch
 * of their own and apply either way. */
#ifndef I915_FEAT_HDMI_SCRAMBLING
#define I915_FEAT_HDMI_SCRAMBLING 1
#endif

/* Scrambling below 340 MHz too, for a sink that says it can do it.  0:
 * scrambling only where the clock requires it.  Only meaningful with
 * I915_FEAT_HDMI_SCRAMBLING. */
#ifndef I915_FEAT_HDMI_LOW_RATE_SCRAMBLING
#define I915_FEAT_HDMI_LOW_RATE_SCRAMBLING 1
#endif

/* DP++ (dual-mode) adaptors: their type read over DDC, their TMDS
 * limit honoured and their level shifter switched on.  0: an adaptor is
 * treated as a plain HDMI sink, as before. */
#ifndef I915_FEAT_DP_DUAL_MODE
#define I915_FEAT_DP_DUAL_MODE 1
#endif

/* AVI / vendor / SPD infoframes built by the HDMI infoframe library.
 * 0: the driver's own infoframe packing, as before. */
#ifndef I915_FEAT_HDMI_INFOFRAME_LIB
#define I915_FEAT_HDMI_INFOFRAME_LIB 1
#endif

/* The transcoder's DDI function for an HDMI port as the hardware wants
 * it: DVI mode for a sink whose EDID is not an HDMI one (no data
 * islands for a sink that does not know them), and from display version
 * 14 the port width of the four TMDS lanes.  0: HDMI mode and a width of
 * one whatever the sink, as before. */
#ifndef I915_FEAT_HDMI_DDI_FUNC
#define I915_FEAT_HDMI_DDI_FUNC 1
#endif

/* ---- variable refresh (intel_vrr.c) --------------------------------------- */

/* Variable refresh: VRR_ENABLED on the crtcs and vrr_capable on the
 * connectors whose sinks have a refresh range, the timing generator run
 * between the range's limits, and a push after every flip.  0: the
 * timing generator runs only at the fixed refresh rate on the display
 * versions that need it for that, as before. */
#ifndef I915_FEAT_VRR
#define I915_FEAT_VRR 1
#endif

/* ---- display stream compression (intel_vdsc.c) ----------------------------- */

/* Compress the stream (VDSC) when a DisplayPort link cannot carry the
 * mode uncompressed and both ends can: external DisplayPort with FEC,
 * eDP without (and not on a panel the VBT forbids it for); the pipe then
 * runs at the compressor's input depth (the EDID's bpc, up to 10/12).
 * Also clears a decompression switch the firmware left on in the sink,
 * and an engine it left running.  0: such a mode is refused, as
 * before. */
#ifndef I915_FEAT_DSC
#define I915_FEAT_DSC 1
#endif

/* ---- rotated scanout views (i915_vma.c, i915_gtt.c, intel_plane.c) ----------- */

/* 90- and 270-degree rotation of Y / Yf tiled surfaces through a rotated
 * GGTT view.  0: those rotations are refused, as before. */
#ifndef I915_FEAT_ROTATED_VIEWS
#define I915_FEAT_ROTATED_VIEWS 1
#endif

#endif
