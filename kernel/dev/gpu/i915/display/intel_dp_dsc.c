// LikeOS -- how a DisplayPort or eDP stream is compressed when its link
// cannot carry it uncompressed.
//
// The choice goes in this order:
//  - can it be done at all: engines in the source (not on transcoder A of
//    display version 11), a DSC 1.x decoder in the sink taking RGB, and on
//    external DisplayPort FEC on both ends (a compressed stream travels
//    there only with error correction; an embedded panel's link runs
//    without it);
//  - the pipe's depth into the compressor: the deepest input both ends
//    take, no deeper than the sink's EDID depth (8 bits a component when
//    it names none);
//  - the compressed bits per pixel and the link: an eDP panel gets the
//    fastest, widest link it has and the highest rate the source, the
//    sink and the small joiner RAM allow; external DisplayPort the
//    highest rate (in the steps the source takes: any 1/16 from display
//    version 13 on, the VESA list before) for which some link carries
//    the stream, FEC's share included, on the least of those links;
//  - the slices per line (the sink's throughput and widths, the engines,
//    a second slice near the CDCLK limit), and the complete parameter set
//    from them, which must come out valid.
//
// Nothing here touches hardware: the atomic check runs it for a mode the
// uncompressed link cannot carry, and the enable path for the output it
// lights (intel_vdsc.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dsc.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/drm_dsc_helper.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The compressed rates display versions before 13 take (whole bits). */
static const uint8_t vesa_dsc_bpp[] = { 6, 8, 10, 12, 15 };

static bool valid_compressed_bpp(const struct intel_dsc_source_caps *src, int bpp_x16)
{
	if (src->display_ver >= 13)
		return true;
	if (drm_dsc_q4_to_frac(bpp_x16))
		return false;
	for (unsigned i = 0; i < ARRAY_SIZE(vesa_dsc_bpp); i++)
		if (drm_dsc_q4_from_int(vesa_dsc_bpp[i]) == bpp_x16)
			return true;
	return false;
}

/* The sink's depth a component: what its EDID says, 8 when it says
 * nothing. */
static int sink_max_bpc(struct i915_device *i915, const struct intel_output *o)
{
	unsigned int bpc = 0;

	if (o->conn >= 0)
		bpc = i915->drm.conn[o->conn].display_info.bpc;
	return bpc ? (int)bpc : 8;
}

/* A link rate the port is trained at here: one its PLL can be clocked at,
 * within the board's limit from the VBT, on 8b/10b coding (128b/132b
 * links are not trained here).  The widths come from
 * intel_dp_max_lanes(), which has the same limits for lanes. */
static bool rate_usable(struct i915_device *i915, const struct intel_output *o, uint32_t rate)
{
	if (drm_dp_is_uhbr_rate((int)rate))
		return false;
	if (o->vbt_max_link_rate_khz && rate > o->vbt_max_link_rate_khz)
		return false;
	return intel_dpll_output_rate_supported(i915, o, rate) != 0;
}

/* Does `rate' x `lanes' carry the stream at `bpp_x16', the allocation
 * overhead (FEC's parity among it) included? */
static bool link_fits(uint32_t rate, int lanes, const struct drm_mode_modeinfo *m, int bpp_x16,
		      bool fec)
{
	int available = intel_dsc_max_link_data_rate((int)rate, lanes);
	int required = intel_dsc_link_required((int)rate, lanes, (int)m->clock, m->hdisplay, bpp_x16,
					       fec ? DRM_DP_BW_OVERHEAD_FEC : 0);

	return available >= required;
}

/* The fastest rate and every lane: an eDP panel's link under
 * compression.  false when the port drives none of the sink's rates. */
static bool max_link(struct i915_device *i915, const struct intel_output *o, uint32_t *rate,
		     int *lanes)
{
	uint32_t best = 0;

	for (int r = 0; r < o->nsink_rates; r++)
		if (o->sink_rates_khz[r] > best && rate_usable(i915, o, o->sink_rates_khz[r]))
			best = o->sink_rates_khz[r];
	if (!best)
		return false;
	*rate = best;
	*lanes = intel_dp_max_lanes(o);
	return true;
}

/* The link of least bandwidth (and of those the fewest lanes) that
 * carries the stream at `bpp_x16'. */
static bool least_link(struct i915_device *i915, const struct intel_output *o,
		       const struct drm_mode_modeinfo *m, int bpp_x16, bool fec, uint32_t *rate,
		       int *lanes)
{
	int max_lanes = intel_dp_max_lanes(o);
	uint64_t best_bw = 0;
	bool found = false;

	for (int l = 1; l <= max_lanes; l *= 2) {
		for (int r = 0; r < o->nsink_rates; r++) {
			uint32_t rt = o->sink_rates_khz[r];
			uint64_t bw = (uint64_t)rt * (uint32_t)l;

			if (!rate_usable(i915, o, rt))
				continue;
			if (found && bw >= best_bw)
				continue;
			if (!link_fits(rt, l, m, bpp_x16, fec))
				continue;
			found = true;
			best_bw = bw;
			*rate = rt;
			*lanes = l;
		}
	}
	return found;
}

int intel_dp_dsc_compute(struct i915_device *i915, struct intel_output *o,
			 const struct drm_mode_modeinfo *m, struct intel_dsc_config *cfg,
			 bool cpu_transcoder_is_a)
{
	const struct intel_dsc_source_caps *src;
	const struct intel_dsc_sink_caps *sink = &o->dsc_caps;
	struct drm_dsc_config dsc;
	int pipe_bpp, min_bpp_x16, max_bpp_x16, step_x16, bpp_x16, slices, rc;
	uint32_t rate = 0;
	int lanes = 0;
	bool fec;

	mm_memset(cfg, 0, sizeof(*cfg));
	intel_dp_dsc_source_init(i915);
	src = &i915->display.dsc_src;
	if (!sink->valid || m->hdisplay == 0 || m->vdisplay == 0 || m->clock == 0)
		return -EINVAL;
	/* external DisplayPort: only with FEC; no MST here */
	if (!intel_dsc_supports_dsc(src, sink, o->port, false, cpu_transcoder_is_a,
				    o->is_edp && i915->display.vbt.valid &&
					    i915->display.vbt.edp_dsc_disable))
		return -EOPNOTSUPP;
	if (!intel_dsc_supports_format(src, sink, INTEL_DSC_FORMAT_RGB))
		return -EOPNOTSUPP;
	/* FEC is optional for eDP and costs bandwidth: off there, as on
	 * every 8b/10b link that carries no compressed stream. */
	fec = !o->is_edp;

	/* The input depth: the deepest both take, within the sink's own. */
	pipe_bpp = intel_dsc_compute_max_pipe_bpp(src, sink, sink_max_bpc(i915, o));
	if (pipe_bpp < 24)
		return -EINVAL;

	min_bpp_x16 = intel_dsc_min_compressed_bpp_x16(src, sink, INTEL_DSC_FORMAT_RGB);
	max_bpp_x16 = intel_dsc_max_compressed_bpp_x16(src, sink, (int)m->clock, m->hdisplay,
						       INTEL_DSC_FORMAT_RGB, pipe_bpp,
						       drm_dsc_q4_from_int(pipe_bpp));
	if (min_bpp_x16 <= 0 || min_bpp_x16 > max_bpp_x16)
		return -EINVAL;
	step_x16 = intel_dsc_bpp_step_x16(src, sink);
	if (step_x16 <= 0)
		return -EINVAL;

	if (o->is_edp) {
		/* The panel's one link, at the most it takes: a panel is
		 * built for its fastest rate and width, and its maximum
		 * compressed rate is what it was qualified at. */
		if (!max_link(i915, o, &rate, &lanes))
			return -EINVAL;
		bpp_x16 = max_bpp_x16;
	} else {
		bool found = false;

		for (bpp_x16 = max_bpp_x16; bpp_x16 >= min_bpp_x16; bpp_x16 -= step_x16) {
			if (!valid_compressed_bpp(src, bpp_x16))
				continue;
			if (least_link(i915, o, m, bpp_x16, fec, &rate, &lanes)) {
				found = true;
				break;
			}
		}
		if (!found)
			return -ENOSPC;
	}

	slices = intel_dsc_get_slice_count(src, sink, (int)m->clock, m->hdisplay,
					   (int)intel_cdclk_max_khz(i915));
	if (slices <= 0)
		return -EINVAL;

	/* the parameter set must come out valid for these choices */
	rc = intel_dsc_dp_compute_config(src, sink, &dsc, m->hdisplay, m->vdisplay, slices, bpp_x16,
					 pipe_bpp, INTEL_DSC_FORMAT_RGB);
	if (rc)
		return rc;

	cfg->enable = 1;
	cfg->pipe_bpp = (uint8_t)pipe_bpp;
	cfg->compressed_bpp_x16 = (uint16_t)bpp_x16;
	cfg->slice_count = (uint8_t)slices;
	/* one engine for one slice; two (each taking half the line) for
	 * two or four */
	cfg->num_vdsc_instances = slices >= 2 ? 2 : 1;
	cfg->slice_height = dsc.slice_height;
	cfg->fec_enable = fec;
	cfg->link_rate_khz = rate;
	cfg->lane_count = (uint8_t)lanes;
	return 0;
}

int intel_dp_dsc_params(struct i915_device *i915, struct intel_output *o,
			const struct intel_dsc_config *cfg, int hdisplay, int vdisplay,
			struct drm_dsc_config *out)
{
	intel_dp_dsc_source_init(i915);
	return intel_dsc_dp_compute_config(&i915->display.dsc_src, &o->dsc_caps, out, hdisplay,
					   vdisplay, cfg->slice_count, cfg->compressed_bpp_x16,
					   cfg->pipe_bpp, INTEL_DSC_FORMAT_RGB);
}
