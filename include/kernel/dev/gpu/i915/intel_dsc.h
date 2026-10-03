// LikeOS -- display stream compression (VDSC) on the Intel display
// pipes: where the mode-set path asks whether a mode needs compressing,
// switches the compression engine on and off, and learns how many bits a
// pixel takes on the link.
//
// A DisplayPort link that cannot carry a mode uncompressed may carry it
// compressed when the source and the sink both can.  The atomic check
// asks intel_dsc_compute_config() for such a mode; the enable path asks
// again for the output being lit and keeps the answer in the pipe
// (p->dsc), which the link choice (p->dsc.link_rate_khz/lane_count), the
// transcoder's M/N values (intel_dsc_link_data_rate()) and the engine's
// enable and disable go by.  Everything is off while p->dsc.enable is 0.
//
// The order on the way up: intel_dsc_compute_config() before the link is
// chosen, intel_dsc_pre_link_train() before every link training (the
// sink's decompression and FEC readiness), intel_dsc_enable() once the
// link trained (FEC on the link, the parameter set to the sink, the
// engine), then the transcoder.  On the way down intel_dsc_disable()
// right after the transcoder stopped.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_DSC_H
#define KERNEL_DEV_GPU_I915_INTEL_DSC_H

#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct intel_output;
struct intel_pipe;

/* The compression a pipe runs (embedded in struct intel_pipe as `dsc';
 * also filled on the stack by the atomic check, so it stays small).  All
 * zero: uncompressed at the pipe's 24 bits per pixel. */
struct intel_dsc_config {
	uint8_t enable;
	/* Bits per pixel of the pipe (into the compressor when enabled),
	 * 0 = 24.  The transcoder's BPC fields, the DisplayPort MSA and
	 * PIPE_MISC are programmed from it (18, 24, 30 or 36). */
	uint8_t pipe_bpp;
	uint16_t compressed_bpp_x16; /* bits per pixel on the link, 1/16 units */
	uint8_t slice_count; /* slices per line */
	uint8_t num_vdsc_instances; /* engines the line is split over (1, 2) */
	uint16_t slice_height;
	/* Forward error correction on the link (external DisplayPort
	 * carries a compressed stream only with it; eDP runs without). */
	uint8_t fec_enable;
	/* What intel_dsc_enable() did, for intel_dsc_disable(): the power
	 * domain it took + 1 (0 none), the sink's decompression switched
	 * on. */
	uint8_t power_ref;
	uint8_t sink_decompression;
	uint8_t lane_count; /* the link the stream was sized for */
	uint32_t link_rate_khz;
};

/* Compression settings in the device's driver table (from
 * intel_display_driver_setup(), before registration). */
void intel_dsc_driver_setup(struct i915_device *i915, struct drm_driver *drv);

/* The compression output `o' needs for mode `m'.  Fills `cfg' (zeroed
 * first; enable set only when compression is to be used) and returns 0
 * when the output can carry the mode that way, a negative errno when it
 * cannot.  Called from the atomic check (display lock held, cfg on the
 * stack) only for a mode the uncompressed DisplayPort link cannot carry
 * -- a failure refuses the mode -- and from the enable path for every
 * DisplayPort/eDP output being lit, before its link is chosen (cfg =
 * &p->dsc; enable stays 0 when the uncompressed link carries the mode,
 * and a failure just leaves the pipe uncompressed).  With compression
 * the link to train is cfg->link_rate_khz x cfg->lane_count. */
int intel_dsc_compute_config(struct i915_device *i915, struct intel_output *o,
			     const struct drm_mode_modeinfo *m, struct intel_dsc_config *cfg);

/* Bits per pixel on the link in 1/16 units of pipe `p': the compressed
 * rate when p->dsc is enabled, else the pipe's bits per pixel
 * (p->dsc.pipe_bpp, 0 meaning 24) * 16. */
uint32_t intel_dsc_link_bpp_x16(const struct intel_pipe *p);

/* The stream's data rate on the link in kbit/s for pixel clock
 * `clock_khz': the pixel clock times the link bits per pixel, and with
 * FEC the share of the link its parity takes on top.  The numerator of
 * the transcoder's data M/N and what a trained link must carry; without
 * compression simply clock * pipe bits per pixel. */
uint64_t intel_dsc_link_data_rate(const struct intel_pipe *p, uint32_t clock_khz);

/* Before each training of the link of output `o' on pipe `p' (the first,
 * a retry, a retrain): with compression the sink's decompression on
 * (DPCD DSC_ENABLE) and, with FEC, the sink told to expect it
 * (FEC_READY); without it a decompression switch the firmware left on
 * in the sink goes off. */
void intel_dsc_pre_link_train(struct i915_device *i915, struct intel_output *o,
			      struct intel_pipe *p);

/* FEC on the trained link (DP_TP_CTL), where p->dsc wants it: done by
 * intel_dsc_enable(); after a retrain of a running link call it again. */
void intel_dsc_fec_enable(struct i915_device *i915, struct intel_output *o,
			  struct intel_pipe *p);

/* Compression on for output `o' on pipe `p' running mode `m': FEC on the
 * link, the picture parameter set sent to the sink, the engines
 * programmed and switched on.  After the link trained, before the
 * transcoder is programmed.  Nothing when p->dsc.enable is 0. */
void intel_dsc_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		      const struct drm_mode_modeinfo *m);

/* And off, after the transcoder has stopped; p->dsc cleared.  An engine
 * found running without p->dsc saying so (the firmware's) is switched off
 * too, so `p' may be a pipe description made up for the firmware's
 * pipe and transcoder. */
void intel_dsc_disable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p);

/* The lowest CDCLK (kHz) the engines of `cfg' need for a pipe running
 * `pixel_rate_khz' with `htotal' (each engine takes one pixel a clock,
 * plus the slices' bubbles); 0 when compression is off. */
uint32_t intel_dsc_min_cdclk_khz(const struct intel_dsc_config *cfg, uint32_t pixel_rate_khz,
				 uint32_t htotal);

/* ---- between the DSC files (intel_dp_dsc.c for intel_vdsc.c) ---------------- */

struct drm_dsc_config;

/* The compression of DisplayPort/eDP output `o' for mode `m' (whose
 * uncompressed link does not suffice), into `cfg' (zeroed first): 0 with
 * cfg->enable set, or a negative errno when it cannot be compressed onto
 * any link.  `cpu_transcoder_is_a': the stream would leave through
 * transcoder A (no engine there on display version 11). */
int intel_dp_dsc_compute(struct i915_device *i915, struct intel_output *o,
			 const struct drm_mode_modeinfo *m, struct intel_dsc_config *cfg,
			 bool cpu_transcoder_is_a);

/* The complete parameter set of `cfg' for a `hdisplay' x `vdisplay'
 * picture on output `o', into `out'; 0 or a negative errno. */
int intel_dp_dsc_params(struct i915_device *i915, struct intel_output *o,
			const struct intel_dsc_config *cfg, int hdisplay, int vdisplay,
			struct drm_dsc_config *out);

#endif
