// LikeOS -- Display Stream Compression: source and sink capabilities, the
// "does this mode need DSC" verdict, and the DSC configuration of a DP
// stream.
//
// A DP link's payload rate is the per-lane rate times the lanes times
// the channel coding efficiency (80% for 8b/10b, 96.71% for 128b/132b).
// A mode fits uncompressed when its pixel clock times its lowest output
// bpp, plus the stream's allocation overhead, stays below that.  When it
// does not, DSC can still carry it if both ends do DSC (and, on external
// DP, FEC -- whose overhead counts on 8b/10b links only, 128b/132b has it
// built in), if a compressed bpp exists inside the source's,
// the sink's and the joiner RAM's limits, and if the sink and the
// engines agree on a slice count that divides the width.  The verdict
// runs exactly that chain for one pipe; until the compression engine is
// programmed it is used to report which modes would need it.
//
// The sink's capabilities come from DPCD 0x60..0x6f (the DSC block), 0x90
// (FEC) and, behind a branch device, 0xa0..0xa2.  They are read through a
// caller-supplied reader so both AUX implementations can use them.
//
// The VBT's compression parameters block (block 56) describes a DSI or
// eDP panel's fixed DSC setup; it is decoded byte by byte here.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation
// Portions Copyright (C) 2018 Intel Corporation
// Portions Copyright (C) 2006 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation

#include <kernel/dev/gpu/i915/intel_dsc_caps.h>
#include <kernel/dev/gpu/i915/intel_qp_tables.h>
#include <kernel/dev/gpu/drm_dsc_helper.h>
#include <kernel/dev/gpu/drm_dp_helper.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>

#ifndef EINVAL
#define EINVAL 22
#endif

/*
 * INTEL_DSC_DEBUG: 1 logs why a capability read or a VBT entry was not
 * usable (unreadable DPCD, unsupported depth or slice count).  0 (the
 * default) keeps these paths silent: they describe the sink or the
 * firmware, not a driver fault, and detect runs on every hotplug.
 */
#ifndef INTEL_DSC_DEBUG
#define INTEL_DSC_DEBUG 0
#endif

#define dsc_dbg(...) do { if (INTEL_DSC_DEBUG) kprintf(__VA_ARGS__); } while (0)


/* 1/0.972261 in ppm: 8b/10b FEC (2.4%) plus the DSC overhead (0.453%). */
#define DP_DSC_FEC_OVERHEAD_FACTOR 1028530

/* The compressed bpps every source and sink handle (DP 1.4 VESA list). */
static const u8 valid_dsc_bpp[] = {6, 8, 10, 12, 15};

static void dsc_memzero(void *p, size_t n)
{
	u8 *b = p;

	while (n--)
		*b++ = 0;
}

static int dsc_round_up(int x, int y)
{
	return ((x + y - 1) / y) * y;
}

static int dsc_round_down(int x, int y)
{
	return x - (x % y);
}

/* ---- source ------------------------------------------------------------- */

static int small_joiner_ram_size_bits(int display_ver)
{
	if (display_ver >= 13)
		return 17280 * 8;
	else if (display_ver >= 11)
		return 7680 * 8;
	else
		return 6144 * 8;
}

static int intel_dp_dsc_min_src_input_bpc(void)
{
	/* Min DSC Input BPC for ICL+ is 8 */
	return 8;
}

static int intel_dp_dsc_max_src_input_bpc(int display_ver)
{
	/* Max DSC Input BPC for ICL is 10 and for TGL+ is 12 */
	if (display_ver >= 12)
		return 12;
	if (display_ver == 11)
		return 10;

	return intel_dp_dsc_min_src_input_bpc();
}

void intel_dsc_source_caps_init(struct intel_dsc_source_caps *src,
				int display_verx100, u32 dfsm, u32 de_cap)
{
	int ver = display_verx100 / 100;

	dsc_memzero(src, sizeof(*src));
	src->display_ver = ver;
	src->display_verx100 = display_verx100;

	/* Every display from version 11 (Ice Lake) on has DSC engines. */
	src->has_dsc = ver >= 11;
	if (ver >= 10 && ver <= 12 && (dfsm & INTEL_DSC_DFSM_DISPLAY_DSC_DISABLE))
		src->has_dsc = false;
	if (ver >= 20 &&
	    FIELD_GET(INTEL_DSC_DE_CAP_DSC_MASK, de_cap) == INTEL_DSC_DE_CAP_DSC_REMOVED)
		src->has_dsc = false;

	src->has_dsc_mst = ver >= 12 && src->has_dsc;
	src->has_dsc_3engines = display_verx100 == 1401 && src->has_dsc;
	src->min_input_bpc = intel_dp_dsc_min_src_input_bpc();
	src->max_input_bpc = intel_dp_dsc_max_src_input_bpc(ver);
	/* Min Compressed bpp supported by source is 8 */
	src->min_compressed_bpp = 8;
	/* Max compressed bpp is 27 from version 13 on, 23 before. */
	src->max_compressed_bpp = ver < 13 ? 23 : 27;
	src->dsc_version_minor = ver >= 14 ? 2 : 1;
	src->small_joiner_ram_bits = small_joiner_ram_size_bits(ver);
}

bool intel_dsc_source_support(const struct intel_dsc_source_caps *src,
			      bool cpu_transcoder_is_a)
{
	if (!src->has_dsc)
		return false;

	if (src->display_ver == 11 && cpu_transcoder_is_a)
		return false;

	return true;
}

bool intel_dsc_source_supports_fec(const struct intel_dsc_source_caps *src,
				   int port, bool mst)
{
	if (src->display_ver >= 12)
		return true;

	if (src->display_ver == 11 && port != 0 /* PORT_A */ && !mst)
		return true;

	return false;
}

/* ---- sink ---------------------------------------------------------------- */

int intel_dsc_dpcd_read_drm_aux(void *ctx, unsigned int offset, void *buf,
				size_t size)
{
	return drm_dp_dpcd_read_data(ctx, offset, buf, size);
}

void intel_dsc_sink_caps_clear(struct intel_dsc_sink_caps *caps)
{
	dsc_memzero(caps, sizeof(*caps));
	caps->overall_throughput.rgb_yuv444 = INTEL_DSC_NO_LIMIT;
	caps->overall_throughput.yuv422_420 = INTEL_DSC_NO_LIMIT;
	caps->max_line_width = INTEL_DSC_NO_LIMIT;
}

static int intel_dp_read_dsc_dpcd(intel_dsc_dpcd_read_fn read, void *ctx,
				  u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	int ret;

	ret = read(ctx, DP_DSC_SUPPORT, dsc_dpcd, DP_DSC_RECEIVER_CAP_SIZE);
	if (ret) {
		dsc_dbg("DSC: could not read DSC DPCD register 0x%x: %d\n",
			DP_DSC_SUPPORT, ret);
		return ret;
	}

	return 0;
}

static void init_dsc_overall_throughput_limits(struct intel_dsc_sink_caps *caps,
					       bool is_branch,
					       intel_dsc_dpcd_read_fn read,
					       void *ctx)
{
	u8 branch_caps[DP_DSC_BRANCH_CAP_SIZE];
	int line_width;
	int v;

	caps->overall_throughput.rgb_yuv444 = INTEL_DSC_NO_LIMIT;
	caps->overall_throughput.yuv422_420 = INTEL_DSC_NO_LIMIT;
	caps->max_line_width = INTEL_DSC_NO_LIMIT;

	if (!is_branch)
		return;

	if (read(ctx, DP_DSC_BRANCH_OVERALL_THROUGHPUT_0, branch_caps,
		 sizeof(branch_caps)) != 0)
		return;

	v = drm_dp_dsc_branch_max_overall_throughput(branch_caps, true);
	caps->overall_throughput.rgb_yuv444 = v ? v : INTEL_DSC_NO_LIMIT;

	v = drm_dp_dsc_branch_max_overall_throughput(branch_caps, false);
	caps->overall_throughput.yuv422_420 = v ? v : INTEL_DSC_NO_LIMIT;

	line_width = drm_dp_dsc_branch_max_line_width(branch_caps);
	caps->max_line_width = line_width > 0 ? line_width : INTEL_DSC_NO_LIMIT;
}

int intel_dsc_read_dp_sink_caps(struct intel_dsc_sink_caps *caps, u8 dpcd_rev,
				const struct drm_dp_desc *desc, bool is_branch,
				intel_dsc_dpcd_read_fn read, void *ctx)
{
	int ret;

	/*
	 * Clear the cached register set to avoid using stale values
	 * for the sinks that do not support DSC (and a stale FEC bit).
	 */
	intel_dsc_sink_caps_clear(caps);

	if (dpcd_rev < DP_DPCD_REV_14)
		return 0;

	ret = intel_dp_read_dsc_dpcd(read, ctx, caps->dsc_dpcd);
	if (ret < 0)
		return ret;

	ret = read(ctx, DP_FEC_CAPABILITY, &caps->fec_capability, 1);
	if (ret < 0) {
		dsc_dbg("DSC: could not read FEC DPCD register: %d\n", ret);
		return ret;
	}
	caps->valid = true;

	if (!(caps->dsc_dpcd[0] & DP_DSC_DECOMPRESSION_IS_SUPPORTED))
		return 0;

	init_dsc_overall_throughput_limits(caps, is_branch, read, ctx);

	if (desc && drm_dp_has_quirk(desc, DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT) &&
	    desc->ident.hw_rev == 0x10)
		caps->throughput_quirk = true;

	return 0;
}

int intel_dsc_read_edp_sink_caps(struct intel_dsc_sink_caps *caps,
				 u8 edp_dpcd_rev,
				 intel_dsc_dpcd_read_fn read, void *ctx)
{
	int ret;

	intel_dsc_sink_caps_clear(caps);
	caps->is_edp = true;

	if (edp_dpcd_rev < DP_EDP_14)
		return 0;

	ret = intel_dp_read_dsc_dpcd(read, ctx, caps->dsc_dpcd);
	if (ret < 0)
		return ret;
	caps->valid = true;

	if (caps->dsc_dpcd[0] & DP_DSC_DECOMPRESSION_IS_SUPPORTED)
		init_dsc_overall_throughput_limits(caps, false, read, ctx);

	return 0;
}

static int intel_dp_sink_dsc_version_minor(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	return (dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] & DP_DSC_MINOR_MASK) >>
		DP_DSC_MINOR_SHIFT;
}

static int intel_dp_sink_dsc_version_major(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	return (dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] & DP_DSC_MAJOR_MASK) >>
		DP_DSC_MAJOR_SHIFT;
}

void intel_dsc_log_sink_caps(const char *tag,
			     const struct intel_dsc_sink_caps *caps)
{
	const u8 *d = caps->dsc_dpcd;
	u8 bpcs[3] = { 0, 0, 0 };
	int nbpc;

	if (!caps->valid) {
		kprintf("%s: DSC: no capability block\n", tag);
		return;
	}
	if (!drm_dp_sink_supports_dsc(d)) {
		kprintf("%s: DSC: not supported (FEC %s)\n", tag,
			drm_dp_sink_supports_fec(caps->fec_capability) ? "yes" : "no");
		return;
	}

	nbpc = drm_dp_dsc_sink_supported_input_bpcs(d, bpcs);
	kprintf("%s: DSC %d.%d: slices max %d, slice width max %u, line buf %d, "
		"bpc %d/%d/%d (%d), formats 0x%x, bpp incr 1/%d, max bpp x16 %d, "
		"block pred %s, FEC %s, branch throughput %d/%d line width %d%s\n",
		tag, intel_dp_sink_dsc_version_major(d), intel_dp_sink_dsc_version_minor(d),
		drm_dp_dsc_sink_max_slice_count(d, caps->is_edp),
		(unsigned int)drm_dp_dsc_sink_max_slice_width(d),
		drm_dp_dsc_sink_line_buf_depth(d),
		bpcs[0], bpcs[1], bpcs[2], nbpc,
		d[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT],
		drm_dp_dsc_sink_bpp_incr(d), drm_edp_dsc_sink_output_bpp(d),
		(d[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		 DP_DSC_BLK_PREDICTION_IS_SUPPORTED) ? "yes" : "no",
		drm_dp_sink_supports_fec(caps->fec_capability) ? "yes" : "no",
		caps->overall_throughput.rgb_yuv444, caps->overall_throughput.yuv422_420,
		caps->max_line_width, caps->throughput_quirk ? ", throughput quirk" : "");
}

bool intel_dsc_has_dsc(const struct intel_dsc_source_caps *src,
		       const struct intel_dsc_sink_caps *sink,
		       bool mst, bool vbt_edp_dsc_disable)
{
	if (!src->has_dsc)
		return false;

	if (mst && !src->has_dsc_mst)
		return false;

	if (sink->is_edp && vbt_edp_dsc_disable)
		return false;

	if (!drm_dp_sink_supports_dsc(sink->dsc_dpcd))
		return false;

	return true;
}

bool intel_dsc_supports_fec(const struct intel_dsc_source_caps *src,
			    const struct intel_dsc_sink_caps *sink,
			    int port, bool mst)
{
	return intel_dsc_source_supports_fec(src, port, mst) &&
		drm_dp_sink_supports_fec(sink->fec_capability);
}

bool intel_dsc_supports_dsc(const struct intel_dsc_source_caps *src,
			    const struct intel_dsc_sink_caps *sink,
			    int port, bool mst, bool cpu_transcoder_is_a,
			    bool vbt_edp_dsc_disable)
{
	if (!intel_dsc_has_dsc(src, sink, mst, vbt_edp_dsc_disable))
		return false;

	/* External DP carries DSC only together with FEC. */
	if (!sink->is_edp && !mst &&
	    !intel_dsc_supports_fec(src, sink, port, mst))
		return false;

	return intel_dsc_source_support(src, cpu_transcoder_is_a);
}

bool intel_dsc_supports_format(const struct intel_dsc_source_caps *src,
			       const struct intel_dsc_sink_caps *sink,
			       enum intel_dsc_output_format format)
{
	u8 sink_dsc_format;

	switch (format) {
	case INTEL_DSC_FORMAT_RGB:
		sink_dsc_format = DP_DSC_RGB;
		break;
	case INTEL_DSC_FORMAT_YCBCR444:
		sink_dsc_format = DP_DSC_YCbCr444;
		break;
	case INTEL_DSC_FORMAT_YCBCR420:
		if (min(src->dsc_version_minor,
			intel_dp_sink_dsc_version_minor(sink->dsc_dpcd)) < 2)
			return false;
		sink_dsc_format = DP_DSC_YCbCr420_Native;
		break;
	default:
		return false;
	}

	return drm_dp_dsc_sink_supports_format(sink->dsc_dpcd, sink_dsc_format);
}

/* ---- limits ------------------------------------------------------------ */

static int output_format_link_bpp_x16(enum intel_dsc_output_format format,
				      int pipe_bpp)
{
	/*
	 * bpp value was assumed to RGB format. And YCbCr 4:2:0 output
	 * format of the number of bytes per pixel will be half the number
	 * of bytes of RGB pixel.
	 */
	if (format == INTEL_DSC_FORMAT_YCBCR420)
		pipe_bpp /= 2;

	return drm_dsc_q4_from_int(pipe_bpp);
}

/* The lowest pipe bpp a mode can run at uncompressed: 6 bpc for RGB. */
static int intel_dp_min_bpp(enum intel_dsc_output_format format)
{
	if (format == INTEL_DSC_FORMAT_RGB)
		return 6 * 3;
	else
		return 8 * 3;
}

static int align_max_sink_dsc_input_bpp(const struct intel_dsc_sink_caps *sink,
					int max_pipe_bpp)
{
	u8 dsc_bpc[3] = { 0, 0, 0 };
	int num_bpc;
	int i;

	num_bpc = drm_dp_dsc_sink_supported_input_bpcs(sink->dsc_dpcd, dsc_bpc);
	for (i = 0; i < num_bpc; i++) {
		if (dsc_bpc[i] * 3 <= max_pipe_bpp)
			return dsc_bpc[i] * 3;
	}

	return 0;
}

int intel_dsc_compute_max_pipe_bpp(const struct intel_dsc_source_caps *src,
				   const struct intel_dsc_sink_caps *sink,
				   int max_req_bpc)
{
	int dsc_max_bpc = src->max_input_bpc;

	if (!dsc_max_bpc)
		return dsc_max_bpc;

	dsc_max_bpc = min(dsc_max_bpc, max_req_bpc);

	return align_max_sink_dsc_input_bpp(sink, dsc_max_bpc * 3);
}

int intel_dsc_bpp_step_x16(const struct intel_dsc_source_caps *src,
			   const struct intel_dsc_sink_caps *sink)
{
	u8 incr = drm_dp_dsc_sink_bpp_incr(sink->dsc_dpcd);

	if (src->display_ver < 14 || !incr)
		return drm_dsc_q4_from_int(1);

	/* fxp q4 */
	return drm_dsc_q4_from_int(1) / incr;
}

static int align_min_vesa_compressed_bpp_x16(int min_link_bpp_x16)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(valid_dsc_bpp); i++) {
		int vesa_bpp_x16 = drm_dsc_q4_from_int(valid_dsc_bpp[i]);

		if (vesa_bpp_x16 >= min_link_bpp_x16)
			return vesa_bpp_x16;
	}

	return 0;
}

static int align_max_vesa_compressed_bpp_x16(int max_link_bpp_x16)
{
	int i;

	for (i = ARRAY_SIZE(valid_dsc_bpp) - 1; i >= 0; i--) {
		int vesa_bpp_x16 = drm_dsc_q4_from_int(valid_dsc_bpp[i]);

		if (vesa_bpp_x16 <= max_link_bpp_x16)
			return vesa_bpp_x16;
	}

	return 0;
}

static int align_min_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
					const struct intel_dsc_sink_caps *sink,
					int min_bpp_x16)
{
	if (src->display_ver >= 13) {
		int bpp_step_x16 = intel_dsc_bpp_step_x16(src, sink);

		WARN_ON_ONCE(!is_power_of_2(bpp_step_x16));

		return dsc_round_up(min_bpp_x16, bpp_step_x16);
	} else {
		return align_min_vesa_compressed_bpp_x16(min_bpp_x16);
	}
}

static int align_max_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
					const struct intel_dsc_sink_caps *sink,
					enum intel_dsc_output_format format,
					int pipe_bpp, int max_bpp_x16)
{
	int link_bpp_x16 = output_format_link_bpp_x16(format, pipe_bpp);
	int bpp_step_x16 = intel_dsc_bpp_step_x16(src, sink);

	max_bpp_x16 = min(max_bpp_x16, link_bpp_x16 - bpp_step_x16);

	if (src->display_ver >= 13) {
		WARN_ON_ONCE(!is_power_of_2(bpp_step_x16));

		return dsc_round_down(max_bpp_x16, bpp_step_x16);
	} else {
		return align_max_vesa_compressed_bpp_x16(max_bpp_x16);
	}
}

static u16 intel_dp_dsc_max_delta_bppx16(const struct intel_dsc_sink_caps *sink,
					 enum intel_dsc_output_format format)
{
	const u8 *dsc_dpcd = sink->dsc_dpcd;
	u8 max_bpp_delta_v1 = dsc_dpcd[DP_DSC_MAX_BPP_DELTA_VERSION_1 - DP_DSC_SUPPORT];
	int max_bpp;

	if (!(dsc_dpcd[DP_DSC_MAX_BITS_PER_PIXEL_HI - DP_DSC_SUPPORT] &
	    DP_DSC_MAX_BPP_DELTA_AVAILABILITY))
		return 0;

	switch (format) {
	case INTEL_DSC_FORMAT_RGB:
	case INTEL_DSC_FORMAT_YCBCR444:
		max_bpp = max_bpp_delta_v1 & DP_DSC_RGB_YCbCr444_MAX_BPP_DELTA_MASK;
		if (max_bpp >= 1 && max_bpp <= 21)
			max_bpp = max_bpp + DP_DSC_BPP_DELTA_444 - 1;
		else
			max_bpp = 0;
		break;
	case INTEL_DSC_FORMAT_YCBCR420:
		max_bpp = (max_bpp_delta_v1 & DP_DSC_NATIVE_YCbCr420_MAX_BPP_DELTA_MASK) >>
			  DP_DSC_BPP_DELTA_SHIFT_420;
		if (max_bpp >= 1 && max_bpp <= 7)
			max_bpp = max_bpp + DP_DSC_BPP_DELTA_420 - 1;
		break;
	default:
		return 0;
	}

	return max_bpp << 4;
}

static u16 intel_dp_dsc_max_sink_compressed_bppx16(const struct intel_dsc_sink_caps *sink,
						   enum intel_dsc_output_format format,
						   int bpc)
{
	u16 max_bppx16 = intel_dp_dsc_max_delta_bppx16(sink, format);

	if (max_bppx16)
		return max_bppx16;

	max_bppx16 = drm_edp_dsc_sink_output_bpp(sink->dsc_dpcd);

	if (max_bppx16)
		return max_bppx16;
	/*
	 * If support not given in DPCD 67h, 68h, 6Eh, 6Fh use the Maximum Allowed bit rate
	 * values as given in spec Table 2-157 DP v2.0
	 */
	switch (format) {
	case INTEL_DSC_FORMAT_RGB:
	case INTEL_DSC_FORMAT_YCBCR444:
		return (3 * bpc) << 4;
	case INTEL_DSC_FORMAT_YCBCR420:
		return (3 * (bpc / 2)) << 4;
	default:
		break;
	}

	return 0;
}

static int intel_dp_dsc_sink_min_compressed_bpp(enum intel_dsc_output_format format)
{
	/* From Mandatory bit rate range Support Table 2-157 (DP v2.0) */
	switch (format) {
	case INTEL_DSC_FORMAT_RGB:
	case INTEL_DSC_FORMAT_YCBCR444:
		return 8;
	case INTEL_DSC_FORMAT_YCBCR420:
		return 6;
	default:
		break;
	}

	return 0;
}

int intel_dsc_min_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
				     const struct intel_dsc_sink_caps *sink,
				     enum intel_dsc_output_format format)
{
	int dsc_src_min_bpp, dsc_sink_min_bpp, dsc_min_bpp;
	int min_bpp_x16;

	dsc_src_min_bpp = src->min_compressed_bpp;
	dsc_sink_min_bpp = intel_dp_dsc_sink_min_compressed_bpp(format);
	dsc_min_bpp = max(dsc_src_min_bpp, dsc_sink_min_bpp);

	min_bpp_x16 = drm_dsc_q4_from_int(dsc_min_bpp);

	return align_min_compressed_bpp_x16(src, sink, min_bpp_x16);
}

static int dsc_throughput_quirk_max_bpp_x16(const struct intel_dsc_sink_caps *sink,
					    int mode_clock)
{
	if (!sink->throughput_quirk)
		return INTEL_DSC_NO_LIMIT;

	/*
	 * Some branch devices fail to decompress a stream with a compressed
	 * link-bpp higher than 12 if the pixel clock is higher than ~50 % of
	 * the maximum overall throughput the branch reports.  The smaller of
	 * the two throughput values is used, the RGB/YUV444 value being
	 * normally the smaller one.
	 */
	if (mode_clock <
	    min(sink->overall_throughput.rgb_yuv444,
		sink->overall_throughput.yuv422_420) / 2)
		return INTEL_DSC_NO_LIMIT;

	return drm_dsc_q4_from_int(12);
}

/* One pipe only: no big/ultra joiner here. */
static u32 small_joiner_ram_max_bpp(const struct intel_dsc_source_caps *src,
				    u32 mode_hdisplay)
{
	/* Small Joiner Check: output bpp <= joiner RAM (bits) / Horiz. width */
	return src->small_joiner_ram_bits / mode_hdisplay;
}

int intel_dsc_max_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
				     const struct intel_dsc_sink_caps *sink,
				     int mode_clock, int mode_hdisplay,
				     enum intel_dsc_output_format format,
				     int pipe_max_bpp, int max_link_bpp_x16)
{
	int dsc_src_max_bpp, dsc_sink_max_bpp, dsc_max_bpp;
	int throughput_max_bpp_x16;
	int joiner_max_bpp;

	if (mode_hdisplay <= 0)
		return 0;

	dsc_src_max_bpp = src->max_compressed_bpp;
	joiner_max_bpp = small_joiner_ram_max_bpp(src, mode_hdisplay);
	dsc_sink_max_bpp = intel_dp_dsc_max_sink_compressed_bppx16(sink, format,
								   pipe_max_bpp / 3) >> 4;
	dsc_max_bpp = min(dsc_sink_max_bpp, dsc_src_max_bpp);
	dsc_max_bpp = min(dsc_max_bpp, joiner_max_bpp);

	max_link_bpp_x16 = min(max_link_bpp_x16, drm_dsc_q4_from_int(dsc_max_bpp));

	throughput_max_bpp_x16 = dsc_throughput_quirk_max_bpp_x16(sink, mode_clock);
	if (throughput_max_bpp_x16 < max_link_bpp_x16)
		max_link_bpp_x16 = throughput_max_bpp_x16;

	return align_max_compressed_bpp_x16(src, sink, format, pipe_max_bpp,
					    max_link_bpp_x16);
}

static int intel_dp_dsc_min_slice_count(const struct intel_dsc_sink_caps *sink,
					int mode_clock, int mode_hdisplay,
					int max_cdclk_khz)
{
	int min_slice_count;
	int max_slice_width;
	int tp_rgb_yuv444;
	int tp_yuv422_420;

	/* An eDP panel gets the most slices it takes. */
	if (sink->is_edp)
		return drm_dp_dsc_sink_max_slice_count(sink->dsc_dpcd, true);

	if (mode_clock > max(sink->overall_throughput.rgb_yuv444,
			     sink->overall_throughput.yuv422_420))
		return 0;

	if (mode_hdisplay > sink->max_line_width)
		return 0;

	tp_rgb_yuv444 = drm_dp_dsc_sink_max_slice_throughput(sink->dsc_dpcd,
							     mode_clock, true);
	tp_yuv422_420 = drm_dp_dsc_sink_max_slice_throughput(sink->dsc_dpcd,
							     mode_clock, false);

	/*
	 * Use the smaller of the two per-slice throughputs, which may ask
	 * for more slices than the actual format needs, but never fewer.
	 */
	min_slice_count = DIV_ROUND_UP(mode_clock, min(tp_rgb_yuv444, tp_yuv422_420));

	/*
	 * Due to some DSC engine BW limitations, we need to enable second
	 * slice and VDSC engine, whenever we approach close enough to max CDCLK
	 */
	if (max_cdclk_khz && mode_clock >= ((max_cdclk_khz * 85) / 100))
		min_slice_count = max(min_slice_count, 2);

	max_slice_width = drm_dp_dsc_sink_max_slice_width(sink->dsc_dpcd);
	if (max_slice_width < DP_DSC_MIN_SLICE_WIDTH_VALUE)
		return 0; /* unsupported slice width */

	/* Also take into account max slice width */
	min_slice_count = max(min_slice_count,
			      DIV_ROUND_UP(mode_hdisplay, max_slice_width));

	return min_slice_count;
}

/*
 * How @slices_per_pipe slices are spread over the engines of
 * @pipes_per_line joined pipes; the slices per line, or 0 when the
 * engines cannot do it.  3 slices per pipe need three engines per pipe,
 * which only ultrajoined (4) pipes on parts with them have; 2 and 4
 * slices use two engine streams; 1 slice one engine, and joined pipes
 * need at least 2 slices each.
 */
static int intel_dsc_get_slice_config(const struct intel_dsc_source_caps *src,
				      int pipes_per_line, int slices_per_pipe)
{
	int streams_per_pipe;
	int slices_per_stream;

	switch (slices_per_pipe) {
	case 3:
		if (!src->has_dsc_3engines || pipes_per_line != 4)
			return 0;

		streams_per_pipe = 3;
		break;
	case 4:
	case 2:
		streams_per_pipe = 2;
		break;
	case 1:
		if (pipes_per_line > 1)
			return 0;

		streams_per_pipe = 1;
		break;
	default:
		return 0;
	}

	slices_per_stream = slices_per_pipe / streams_per_pipe;

	return pipes_per_line * streams_per_pipe * slices_per_stream;
}

int intel_dsc_get_slice_count(const struct intel_dsc_source_caps *src,
			      const struct intel_dsc_sink_caps *sink,
			      int mode_clock, int mode_hdisplay,
			      int max_cdclk_khz)
{
	int min_slice_count =
		intel_dp_dsc_min_slice_count(sink, mode_clock, mode_hdisplay,
					     max_cdclk_khz);
	u32 sink_slice_count_mask =
		drm_dp_dsc_sink_slice_count_mask(sink->dsc_dpcd, sink->is_edp);
	int slices_per_pipe;

	if (mode_hdisplay <= 0)
		return 0;

	/* Find the closest match to the valid slice count values */
	for (slices_per_pipe = 1; slices_per_pipe <= 4; slices_per_pipe++) {
		int slices_per_line =
			intel_dsc_get_slice_config(src, 1, slices_per_pipe);

		if (!slices_per_line)
			continue;

		if (!(drm_dp_dsc_slice_count_to_mask(slices_per_line) &
		      sink_slice_count_mask))
			continue;

		if (mode_hdisplay % slices_per_line)
			continue;

		if (min_slice_count <= slices_per_line)
			return slices_per_line;
	}

	return 0;
}

/* ---- link bandwidth ------------------------------------------------------ */

static int intel_dp_bw_fec_overhead(bool fec_enabled)
{
	/*
	 * The hard-coded 1/0.972261=2.853% overhead factor corresponds
	 * (for instance) to the 8b/10b DP FEC 2.4% + 0.453% DSC overhead.
	 * This is enough for a 3840 width mode, which has a DSC overhead of
	 * up to ~0.2%, but may not be enough for a 1024 width mode where
	 * this is ~0.8% (on a 4 lane DP link, with 2 DSC slices and 8 bpp
	 * color depth).
	 */
	return fec_enabled ? DP_DSC_FEC_OVERHEAD_FACTOR : 1000000;
}

int intel_dsc_link_bw_overhead(int link_clock, int lane_count, int hdisplay,
			       int dsc_slice_count, int bpp_x16,
			       unsigned long flags)
{
	int overhead;

	WARN_ON(flags & ~(DRM_DP_BW_OVERHEAD_MST | DRM_DP_BW_OVERHEAD_SSC_REF_CLK |
			  DRM_DP_BW_OVERHEAD_FEC));

	if (drm_dp_is_uhbr_rate(link_clock))
		flags |= DRM_DP_BW_OVERHEAD_UHBR;

	if (dsc_slice_count)
		flags |= DRM_DP_BW_OVERHEAD_DSC;

	overhead = drm_dp_bw_overhead(lane_count, hdisplay,
				      dsc_slice_count,
				      bpp_x16,
				      flags);

	return max(overhead, intel_dp_bw_fec_overhead(flags & DRM_DP_BW_OVERHEAD_FEC));
}

/*
 * The pixel data rate in kB/s, the allocation overhead (ppm) included:
 * pixel clock (kHz) x bpp x16 x overhead / (10^6 x 16 x 8).
 */
static int intel_dp_effective_data_rate(int pixel_clock, int bpp_x16,
					int bw_overhead)
{
	return (int)DIV_ROUND_UP_ULL((u64)(u32)(pixel_clock * bpp_x16) * (u32)bw_overhead,
				     1000000ULL * 16 * 8);
}

int intel_dsc_link_required(int link_clock, int lane_count,
			    int mode_clock, int mode_hdisplay,
			    int link_bpp_x16, unsigned long bw_overhead_flags)
{
	int bw_overhead = intel_dsc_link_bw_overhead(link_clock, lane_count, mode_hdisplay,
						     0, link_bpp_x16, bw_overhead_flags);

	return intel_dp_effective_data_rate(mode_clock, link_bpp_x16, bw_overhead);
}

static int intel_dsc_link_required_dsc(int link_clock, int lane_count,
				       int mode_clock, int mode_hdisplay,
				       int dsc_slice_count, int link_bpp_x16,
				       unsigned long bw_overhead_flags)
{
	int bw_overhead = intel_dsc_link_bw_overhead(link_clock, lane_count, mode_hdisplay,
						     dsc_slice_count, link_bpp_x16,
						     bw_overhead_flags);

	return intel_dp_effective_data_rate(mode_clock, link_bpp_x16, bw_overhead);
}

int intel_dsc_max_link_data_rate(int link_clock, int lane_count)
{
	return drm_dp_max_dprx_data_rate(link_clock, lane_count);
}

/* ---- verdict --------------------------------------------------------------- */

static bool intel_dsc_mode_valid_with_dsc(const struct intel_dsc_source_caps *src,
					  const struct intel_dsc_sink_caps *sink,
					  const struct intel_dsc_verdict_args *a,
					  int pipe_bpp, unsigned long bw_overhead_flags,
					  struct intel_dsc_verdict_info *info)
{
	int min_bpp_x16 = intel_dsc_min_compressed_bpp_x16(src, sink, a->format);
	int max_bpp_x16 = intel_dsc_max_compressed_bpp_x16(src, sink,
							   a->mode_clock, a->mode_hdisplay,
							   a->format, pipe_bpp,
							   INTEL_DSC_NO_LIMIT);

	info->min_bpp_x16 = min_bpp_x16;
	info->max_bpp_x16 = max_bpp_x16;

	if (min_bpp_x16 <= 0 || min_bpp_x16 > max_bpp_x16)
		return false;

	if (info->slice_count == 0)
		return false;

	info->dsc_rate = intel_dsc_link_required_dsc(a->link_clock, a->lane_count,
						     a->mode_clock, a->mode_hdisplay,
						     info->slice_count, min_bpp_x16,
						     bw_overhead_flags);

	return info->max_rate >= info->dsc_rate;
}

enum intel_dsc_verdict
intel_dsc_mode_verdict(const struct intel_dsc_source_caps *src,
		       const struct intel_dsc_sink_caps *sink,
		       const struct intel_dsc_verdict_args *a,
		       struct intel_dsc_verdict_info *info)
{
	struct intel_dsc_verdict_info local;
	int link_bpp_x16;
	bool dsc = false;

	if (!info)
		info = &local;
	dsc_memzero(info, sizeof(*info));

	if (a->link_clock <= 0 || a->lane_count <= 0 || a->mode_clock <= 0 ||
	    a->mode_hdisplay <= 0)
		return INTEL_DSC_NEEDED_UNSUPPORTED;

	info->max_rate = intel_dsc_max_link_data_rate(a->link_clock, a->lane_count);

	link_bpp_x16 = output_format_link_bpp_x16(a->format, intel_dp_min_bpp(a->format));
	info->mode_rate = intel_dsc_link_required(a->link_clock, a->lane_count,
						  a->mode_clock, a->mode_hdisplay,
						  link_bpp_x16, 0);

	if (info->mode_rate <= info->max_rate)
		return INTEL_DSC_NOT_NEEDED;

	if (!intel_dsc_supports_dsc(src, sink, a->port, false, a->cpu_transcoder_is_a,
				    a->vbt_edp_dsc_disable) ||
	    !intel_dsc_supports_format(src, sink, a->format))
		return INTEL_DSC_NEEDED_UNSUPPORTED;

	info->slice_count = intel_dsc_get_slice_count(src, sink, a->mode_clock,
						      a->mode_hdisplay, a->max_cdclk_khz);
	/* The highest input depth both ends take as DSC input. */
	info->pipe_bpp = intel_dsc_compute_max_pipe_bpp(src, sink, 0xff);

	if (sink->is_edp) {
		int dsc_max_compressed_bpp = drm_edp_dsc_sink_output_bpp(sink->dsc_dpcd) >> 4;

		dsc = dsc_max_compressed_bpp && info->slice_count;
	} else if (drm_dp_sink_supports_fec(sink->fec_capability)) {
		unsigned long bw_overhead_flags = 0;

		if (!drm_dp_is_uhbr_rate(a->link_clock))
			bw_overhead_flags |= DRM_DP_BW_OVERHEAD_FEC;

		dsc = intel_dsc_mode_valid_with_dsc(src, sink, a, info->pipe_bpp,
						    bw_overhead_flags, info);
	}

	return dsc ? INTEL_DSC_NEEDED : INTEL_DSC_NEEDED_NO_FIT;
}

const char *intel_dsc_verdict_name(enum intel_dsc_verdict v)
{
	switch (v) {
	case INTEL_DSC_NOT_NEEDED:
		return "fits uncompressed";
	case INTEL_DSC_NEEDED:
		return "needs DSC";
	case INTEL_DSC_NEEDED_UNSUPPORTED:
		return "needs DSC, not supported on this path";
	case INTEL_DSC_NEEDED_NO_FIT:
		return "needs DSC, does not fit even compressed";
	}

	return "?";
}

void intel_dsc_log_verdict(const char *tag, int hdisplay, int vdisplay,
			   int mode_clock, enum intel_dsc_verdict v,
			   const struct intel_dsc_verdict_info *info)
{
	if (!info) {
		kprintf("%s: %dx%d@%dkHz %s\n", tag, hdisplay, vdisplay, mode_clock,
			intel_dsc_verdict_name(v));
		return;
	}

	kprintf("%s: %dx%d@%dkHz %s (link %d kB/s, mode %d kB/s, dsc %d kB/s, "
		"pipe bpp %d, compressed bpp x16 %d..%d, slices %d)\n",
		tag, hdisplay, vdisplay, mode_clock, intel_dsc_verdict_name(v),
		info->max_rate, info->mode_rate, info->dsc_rate, info->pipe_bpp,
		info->min_bpp_x16, info->max_bpp_x16, info->slice_count);
}

/* ---- configuration ---------------------------------------------------------- */

static int intel_dsc_slice_dimensions_valid(enum intel_dsc_output_format format,
					    const struct drm_dsc_config *vdsc_cfg)
{
	if (format == INTEL_DSC_FORMAT_RGB || format == INTEL_DSC_FORMAT_YCBCR444) {
		if (vdsc_cfg->slice_height > 4095)
			return -EINVAL;
		if (vdsc_cfg->slice_height * vdsc_cfg->slice_width < 15000)
			return -EINVAL;
	} else if (format == INTEL_DSC_FORMAT_YCBCR420) {
		if (vdsc_cfg->slice_width % 2)
			return -EINVAL;
		if (vdsc_cfg->slice_height % 2)
			return -EINVAL;
		if (vdsc_cfg->slice_height > 4094)
			return -EINVAL;
		if (vdsc_cfg->slice_height * vdsc_cfg->slice_width < 30000)
			return -EINVAL;
	}

	return 0;
}

int intel_dsc_compute_params(const struct intel_dsc_source_caps *src,
			     struct drm_dsc_config *vdsc_cfg, int slice_count,
			     int compressed_bpp_x16, int pipe_bpp,
			     enum intel_dsc_output_format format,
			     bool dsi_dsc_1_1)
{
	u16 compressed_bpp = drm_dsc_q4_to_int(compressed_bpp_x16);
	int err;
	int ret;

	if (slice_count <= 0)
		return -EINVAL;

	vdsc_cfg->slice_count = slice_count;
	vdsc_cfg->slice_width = DIV_ROUND_UP(vdsc_cfg->pic_width, slice_count);

	err = intel_dsc_slice_dimensions_valid(format, vdsc_cfg);
	if (err)
		return err; /* slice dimension requirements not met */

	/*
	 * According to DSC 1.2 specs if colorspace is YCbCr then convert_rgb is 0
	 * else 1
	 */
	vdsc_cfg->convert_rgb = format != INTEL_DSC_FORMAT_YCBCR420 &&
				format != INTEL_DSC_FORMAT_YCBCR444;

	if (src->display_ver >= 14 && format == INTEL_DSC_FORMAT_YCBCR420)
		vdsc_cfg->native_420 = true;
	/* YCbCr 4:2:2 is not supported */
	vdsc_cfg->native_422 = false;
	vdsc_cfg->simple_422 = false;
	/* Gen 11 does not support VBR */
	vdsc_cfg->vbr_enable = false;

	vdsc_cfg->bits_per_pixel = compressed_bpp_x16;

	/*
	 * According to DSC 1.2 specs in Section 4.1 if native_420 is set
	 * we need to double the current bpp.
	 */
	if (vdsc_cfg->native_420)
		vdsc_cfg->bits_per_pixel <<= 1;

	vdsc_cfg->bits_per_component = pipe_bpp / 3;

	if (vdsc_cfg->bits_per_component < 8)
		return -EINVAL; /* DSC bpc requirements not met */

	drm_dsc_set_rc_buf_thresh(vdsc_cfg);

	/*
	 * From display version 13 on, compressed bpps go in steps of 1 up
	 * to the uncompressed bpp - 1, so all RC parameters are computed.
	 *
	 * Not for a DSI panel using DSC 1.1: some panels have hardcoded PPS
	 * parameters in the VBT, and parameters derived through
	 * interpolation would differ from what the panel expects (noise on
	 * the screen).  Such a panel's compressed bpp comes from the VBT
	 * too, so the fixed tables have its RC parameters.
	 */
	if (src->display_ver >= 13 && !dsi_dsc_1_1) {
		intel_qp_calculate_rc_params(vdsc_cfg);
	} else {
		if ((compressed_bpp == 8 ||
		     compressed_bpp == 12) &&
		    (vdsc_cfg->bits_per_component == 8 ||
		     vdsc_cfg->bits_per_component == 10 ||
		     vdsc_cfg->bits_per_component == 12))
			ret = drm_dsc_setup_rc_params(vdsc_cfg, DRM_DSC_1_1_PRE_SCR);
		else
			ret = drm_dsc_setup_rc_params(vdsc_cfg, DRM_DSC_1_2_444);

		if (ret)
			return ret;
	}

	/*
	 * BitsPerComponent value determines mux_word_size:
	 * When BitsPerComponent is less than or 10bpc, muxWordSize will be equal to
	 * 48 bits otherwise 64
	 */
	if (vdsc_cfg->bits_per_component <= 10)
		vdsc_cfg->mux_word_size = DSC_MUX_WORD_SIZE_8_10_BPC;
	else
		vdsc_cfg->mux_word_size = DSC_MUX_WORD_SIZE_12_BPC;

	/* InitialScaleValue is a 6 bit value with 3 fractional bits (U3.3) */
	vdsc_cfg->initial_scale_value = (vdsc_cfg->rc_model_size << 3) /
		(vdsc_cfg->rc_model_size - vdsc_cfg->initial_offset);

	return 0;
}

int intel_dsc_dp_slice_height(int vactive)
{
	int slice_height;

	/*
	 * VDSC 1.2a spec in Section 3.8 Options for Slices implies that 108
	 * lines is an optimal slice height, but any size can be used as long as
	 * vertical active integer multiple and maximum vertical slice count
	 * requirements are met.
	 */
	for (slice_height = 108; slice_height <= vactive; slice_height += 2)
		if (vactive % slice_height == 0)
			return slice_height;

	/*
	 * Highly unlikely we reach here as most of the resolutions will end up
	 * finding appropriate slice_height in above loop but returning
	 * slice_height as 2 here as it should work with all resolutions.
	 */
	return 2;
}

int intel_dsc_dp_compute_config(const struct intel_dsc_source_caps *src,
				const struct intel_dsc_sink_caps *sink,
				struct drm_dsc_config *vdsc_cfg,
				int hdisplay, int vdisplay, int slice_count,
				int compressed_bpp_x16, int pipe_bpp,
				enum intel_dsc_output_format format)
{
	int ret;

	dsc_memzero(vdsc_cfg, sizeof(*vdsc_cfg));

	/*
	 * RC_MODEL_SIZE is a constant across all configurations (the sink's
	 * DP_DSC_RC_BUF_BLK_SIZE / DP_DSC_RC_BUF_SIZE are not used for it).
	 */
	vdsc_cfg->rc_model_size = DSC_RC_MODEL_SIZE_CONST;
	vdsc_cfg->pic_width = hdisplay;
	vdsc_cfg->pic_height = vdisplay;

	vdsc_cfg->slice_height = intel_dsc_dp_slice_height(vdsc_cfg->pic_height);

	ret = intel_dsc_compute_params(src, vdsc_cfg, slice_count, compressed_bpp_x16,
				       pipe_bpp, format, false);
	if (ret)
		return ret;

	vdsc_cfg->dsc_version_major = intel_dp_sink_dsc_version_major(sink->dsc_dpcd);
	vdsc_cfg->dsc_version_minor =
		min(src->dsc_version_minor,
		    intel_dp_sink_dsc_version_minor(sink->dsc_dpcd));
	if (vdsc_cfg->convert_rgb)
		vdsc_cfg->convert_rgb =
			!!(sink->dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] &
			   DP_DSC_RGB);

	vdsc_cfg->line_buf_depth = min_t(int, INTEL_DP_DSC_MAX_LINE_BUF_DEPTH,
					 drm_dp_dsc_sink_line_buf_depth(sink->dsc_dpcd));
	if (!vdsc_cfg->line_buf_depth)
		return -EINVAL; /* sink line buffer depth invalid */

	vdsc_cfg->block_pred_enable =
		!!(sink->dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		   DP_DSC_BLK_PREDICTION_IS_SUPPORTED);

	return drm_dsc_compute_rc_parameters(vdsc_cfg);
}

/* ---- VBT compression parameters ------------------------------------------- */

#define VBT_DSC_LINE_BUFFER_DEPTH(vbt_value)	((vbt_value) + 8) /* bits */
#define VBT_DSC_MAX_BPP(vbt_value)		(6 + (vbt_value) * 2)

void intel_vbt_dsc_decode_entry(const u8 *raw, struct intel_vbt_dsc_params *out)
{
	/*
	 * Byte 0: version major [3:0], minor [7:4]; 1: RC buffer block size
	 * [1:0]; 2: RC buffer size; 3..6: slices per line (LE32); 7: line
	 * buffer depth [3:0]; 8: block prediction [0]; 9: max bpp; 10:
	 * 8/10/12 bpc support [1]/[2]/[3]; 11..12: slice height (LE16).
	 */
	out->version_major = raw[0] & 0xf;
	out->version_minor = raw[0] >> 4;
	out->rc_buffer_block_size = raw[1] & 0x3;
	out->rc_buffer_size = raw[2];
	out->slices_per_line = (u32)raw[3] | ((u32)raw[4] << 8) |
			       ((u32)raw[5] << 16) | ((u32)raw[6] << 24);
	out->line_buffer_depth = raw[7] & 0xf;
	out->block_prediction_enable = raw[8] & 0x1;
	out->max_bpp = raw[9];
	out->support_8bpc = !!(raw[10] & (1 << 1));
	out->support_10bpc = !!(raw[10] & (1 << 2));
	out->support_12bpc = !!(raw[10] & (1 << 3));
	out->slice_height = (u16)(raw[11] | (raw[12] << 8));
}

int intel_vbt_dsc_parse_block(const u8 *block, unsigned int len, u16 bdb_version,
			      struct intel_vbt_dsc_params out[INTEL_VBT_DSC_NUM_ENTRIES])
{
	unsigned int entry_size;
	int i;

	if (bdb_version < 198)
		return -EINVAL;

	if (!block || len < 2)
		return -EINVAL;

	/* Sanity checks */
	entry_size = block[0] | (block[1] << 8);
	if (entry_size != INTEL_VBT_DSC_ENTRY_SIZE)
		return -EINVAL; /* unsupported compression param entry size */

	if (len < 2 + INTEL_VBT_DSC_ENTRY_SIZE * INTEL_VBT_DSC_NUM_ENTRIES)
		return -EINVAL; /* expected 16 compression param entries */

	for (i = 0; i < INTEL_VBT_DSC_NUM_ENTRIES; i++)
		intel_vbt_dsc_decode_entry(block + 2 + i * INTEL_VBT_DSC_ENTRY_SIZE,
					   &out[i]);

	return INTEL_VBT_DSC_NUM_ENTRIES;
}

int intel_vbt_dsc_fill_config(const struct intel_dsc_source_caps *src,
			      const struct intel_vbt_dsc_params *dsc,
			      int dsc_max_bpc, int hdisplay,
			      struct drm_dsc_config *vdsc_cfg, int *pipe_bpp,
			      int *compressed_bpp_x16, int *slices_per_line_ret)
{
	int slices_per_line;
	int bpc = 8;

	vdsc_cfg->dsc_version_major = dsc->version_major;
	vdsc_cfg->dsc_version_minor = dsc->version_minor;

	if (dsc->support_12bpc && dsc_max_bpc >= 12)
		bpc = 12;
	else if (dsc->support_10bpc && dsc_max_bpc >= 10)
		bpc = 10;
	else if (dsc->support_8bpc && dsc_max_bpc >= 8)
		bpc = 8;
	else
		dsc_dbg("VBT: unsupported BPC %d for DSC\n", dsc_max_bpc);

	*pipe_bpp = bpc * 3;

	*compressed_bpp_x16 = drm_dsc_q4_from_int(min(*pipe_bpp,
						      VBT_DSC_MAX_BPP(dsc->max_bpp)));

	/*
	 * The slice count is the largest the entry allows, without regard
	 * to the engine throughput; DSI also allows 3 slices, never chosen
	 * here.
	 */
	if (dsc->slices_per_line & BIT(2)) {
		slices_per_line = 4;
	} else if (dsc->slices_per_line & BIT(1)) {
		slices_per_line = 2;
	} else {
		if (!(dsc->slices_per_line & BIT(0)))
			dsc_dbg("VBT: unsupported DSC slice count for DSI\n");

		slices_per_line = 1;
	}

	if (WARN_ON(!intel_dsc_get_slice_config(src, 1, slices_per_line)))
		return -EINVAL;

	if (hdisplay % slices_per_line != 0)
		dsc_dbg("VBT: DSC hdisplay %d not divisible by slice count %d\n",
			hdisplay, slices_per_line);

	*slices_per_line_ret = slices_per_line;

	/*
	 * The VBT rc_buffer_block_size and rc_buffer_size definitions
	 * correspond to DP 1.4 DPCD offsets 0x62 and 0x63.
	 */
	vdsc_cfg->rc_model_size = drm_dsc_dp_rc_buffer_size(dsc->rc_buffer_block_size,
							    dsc->rc_buffer_size);

	/* The DSI specification says bpc + 1 for this one. */
	vdsc_cfg->line_buf_depth = VBT_DSC_LINE_BUFFER_DEPTH(dsc->line_buffer_depth);

	vdsc_cfg->block_pred_enable = dsc->block_prediction_enable;

	vdsc_cfg->slice_height = dsc->slice_height;

	return 0;
}
