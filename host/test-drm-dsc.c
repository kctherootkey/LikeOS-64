/*
 * Tests for the Display Stream Compression library
 * (kernel/dev/gpu/drm/drm_dsc_helper.c), the display engine's QP tables
 * (kernel/dev/gpu/i915/display/intel_qp_tables.c) and the DSC capability,
 * verdict and configuration code (kernel/dev/gpu/i915/display/
 * intel_dsc_caps.c): PPS header and payload packing, RC buffer sizes, the
 * recommended RC parameter sets, the derived rate-control values for
 * standard configurations (8 bpc / 8 bpp, 10 bpc / 10 bpp, 12 bpc /
 * 12 bpp, the C-model formula path, native 4:2:0, 6 bpp), QP table
 * lookups, source capabilities per display version, sink capability
 * reads, the "needs DSC" verdict, and the VBT compression parameters.
 * See test-drm-dsc.sh.
 *
 * The expected configurations and PPS bytes were computed by an
 * independent model of the DSC 1.2 rate-control formulas and parameter
 * tables, and the 1920x1080 8 bpc / 8 bpp case and the verdict cases were
 * checked by hand.
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from Keith Packard's code: HPND-sell-variant
 * Portions Copyright (C) 2009 Keith Packard (the DSC capability decoders below)
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm_dsc_helper.h>
#include <kernel/dev/gpu/drm_dp_helper.h>
#include <kernel/dev/gpu/i915/intel_qp_tables.h>
#include <kernel/dev/gpu/i915/intel_dsc_caps.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_EQ(got, want, what) do { long __g = (long)(got), __w = (long)(want); CHECK(__g == __w, "%s: got %ld, want %ld", what, __g, __w); } while (0)

/* ---- what the files under test need from the kernel ---------------------- */

static int warnings;

int kprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (!strncmp(buf, "WARNING", 7))
		warnings++;
	return n;
}

void panic(const char *fmt, ...)
{
	(void)fmt;
	printf("panic\n");
	__builtin_trap();
}

/*
 * The helper library's DSC capability decoders and link bandwidth
 * arithmetic (kernel/dev/gpu/drm/drm_dp_helper.c), restated for the host:
 * that file also carries the AUX channel, which needs the kernel.
 */
ssize_t drm_dp_dpcd_read(struct drm_dp_aux *aux, unsigned int offset,
			 void *buffer, size_t size)
{
	(void)aux; (void)offset; (void)buffer; (void)size;
	return -EIO;
}

u8 drm_dp_dsc_sink_bpp_incr(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	u8 bpp_increment_dpcd = dsc_dpcd[DP_DSC_BITS_PER_PIXEL_INC - DP_DSC_SUPPORT];

	switch (bpp_increment_dpcd & DP_DSC_BITS_PER_PIXEL_MASK) {
	case DP_DSC_BITS_PER_PIXEL_1_16:
		return 16;
	case DP_DSC_BITS_PER_PIXEL_1_8:
		return 8;
	case DP_DSC_BITS_PER_PIXEL_1_4:
		return 4;
	case DP_DSC_BITS_PER_PIXEL_1_2:
		return 2;
	case DP_DSC_BITS_PER_PIXEL_1_1:
		return 1;
	}

	return 0;
}

u32 drm_dp_dsc_slice_count_to_mask(int slice_count)
{
	return BIT(slice_count - 1);
}

u32 drm_dp_dsc_sink_slice_count_mask(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				     bool is_edp)
{
	u8 slice_cap1 = dsc_dpcd[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT];
	u32 mask = 0;

	if (!is_edp) {
		u8 slice_cap2 = dsc_dpcd[DP_DSC_SLICE_CAP_2 - DP_DSC_SUPPORT];

		if (slice_cap2 & DP_DSC_24_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(24);
		if (slice_cap2 & DP_DSC_20_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(20);
		if (slice_cap2 & DP_DSC_16_PER_DP_DSC_SINK)
			mask |= drm_dp_dsc_slice_count_to_mask(16);
	}

	if (slice_cap1 & DP_DSC_12_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(12);
	if (slice_cap1 & DP_DSC_10_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(10);
	if (slice_cap1 & DP_DSC_8_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(8);
	if (slice_cap1 & DP_DSC_6_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(6);
	if (slice_cap1 & DP_DSC_4_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(4);
	if (slice_cap1 & DP_DSC_2_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(2);
	if (slice_cap1 & DP_DSC_1_PER_DP_DSC_SINK)
		mask |= drm_dp_dsc_slice_count_to_mask(1);

	return mask;
}

u8 drm_dp_dsc_sink_max_slice_count(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
				   bool is_edp)
{
	return gpu_fls(drm_dp_dsc_sink_slice_count_mask(dsc_dpcd, is_edp));
}

u8 drm_dp_dsc_sink_line_buf_depth(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE])
{
	u8 line_buf_depth = dsc_dpcd[DP_DSC_LINE_BUF_BIT_DEPTH - DP_DSC_SUPPORT];

	switch (line_buf_depth & DP_DSC_LINE_BUF_BIT_DEPTH_MASK) {
	case DP_DSC_LINE_BUF_BIT_DEPTH_9:
		return 9;
	case DP_DSC_LINE_BUF_BIT_DEPTH_10:
		return 10;
	case DP_DSC_LINE_BUF_BIT_DEPTH_11:
		return 11;
	case DP_DSC_LINE_BUF_BIT_DEPTH_12:
		return 12;
	case DP_DSC_LINE_BUF_BIT_DEPTH_13:
		return 13;
	case DP_DSC_LINE_BUF_BIT_DEPTH_14:
		return 14;
	case DP_DSC_LINE_BUF_BIT_DEPTH_15:
		return 15;
	case DP_DSC_LINE_BUF_BIT_DEPTH_16:
		return 16;
	case DP_DSC_LINE_BUF_BIT_DEPTH_8:
		return 8;
	}

	return 0;
}

int drm_dp_dsc_sink_supported_input_bpcs(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
					 u8 dsc_bpc[3])
{
	int num_bpc = 0;
	u8 color_depth = dsc_dpcd[DP_DSC_DEC_COLOR_DEPTH_CAP - DP_DSC_SUPPORT];

	if (!drm_dp_sink_supports_dsc(dsc_dpcd))
		return 0;

	if (color_depth & DP_DSC_12_BPC)
		dsc_bpc[num_bpc++] = 12;
	if (color_depth & DP_DSC_10_BPC)
		dsc_bpc[num_bpc++] = 10;

	/* A DP DSC Sink device shall support 8 bpc. */
	dsc_bpc[num_bpc++] = 8;

	return num_bpc;
}

static int dsc_sink_min_slice_throughput(int peak_pixel_rate)
{
	if (peak_pixel_rate >= 4800000)
		return 600000;
	else if (peak_pixel_rate >= 2700000)
		return 400000;
	else
		return 340000;
}

int drm_dp_dsc_sink_max_slice_throughput(const u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE],
					 int peak_pixel_rate, bool is_rgb_yuv444)
{
	int throughput;
	int delta = 0;
	int base = 0;

	throughput = dsc_dpcd[DP_DSC_PEAK_THROUGHPUT - DP_DSC_SUPPORT];

	if (is_rgb_yuv444) {
		throughput = (throughput & DP_DSC_THROUGHPUT_MODE_0_MASK) >>
			     DP_DSC_THROUGHPUT_MODE_0_SHIFT;

		delta = ((dsc_dpcd[DP_DSC_RC_BUF_BLK_SIZE - DP_DSC_SUPPORT]) &
			 DP_DSC_THROUGHPUT_MODE_0_DELTA_MASK) >>
			DP_DSC_THROUGHPUT_MODE_0_DELTA_SHIFT;
		delta *= 2000;
	} else {
		throughput = (throughput & DP_DSC_THROUGHPUT_MODE_1_MASK) >>
			     DP_DSC_THROUGHPUT_MODE_1_SHIFT;
	}

	if (throughput == 0)
		return dsc_sink_min_slice_throughput(peak_pixel_rate);
	else if (throughput == 1)
		base = 340000;
	else if (throughput <= 14)
		base = 400000 + 50000 * (throughput - 2);
	else
		base = 170000;

	return base + delta;
}

int drm_dp_dsc_branch_max_overall_throughput(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE],
					     bool is_rgb_yuv444)
{
	int throughput = dsc_branch_dpcd[is_rgb_yuv444 ? 0 : 1];

	switch (throughput) {
	case 0:
		return 0;
	case 1:
		return 680000;
	default:
		return 600000 + 50000 * throughput;
	}
}

int drm_dp_dsc_branch_max_line_width(const u8 dsc_branch_dpcd[DP_DSC_BRANCH_CAP_SIZE])
{
	int line_width = dsc_branch_dpcd[2];

	if (line_width == 0)
		return 0;
	if (line_width <= 15)
		return -EINVAL;
	return line_width * 320;
}

static int drm_dp_link_data_symbol_cycles(int lane_count, int pixels,
					  int bpp_x16, int symbol_size,
					  bool is_mst)
{
	int cycles = DIV_ROUND_UP(pixels * bpp_x16, 16 * symbol_size * lane_count);
	int align = is_mst ? 4 / lane_count : 1;

	return ALIGN(cycles, align);
}

int drm_dp_link_symbol_cycles(int lane_count, int pixels, int dsc_slice_count,
			      int bpp_x16, int symbol_size, bool is_mst)
{
	int slice_count = dsc_slice_count ? dsc_slice_count : 1;
	int slice_pixels = DIV_ROUND_UP(pixels, slice_count);
	int slice_data_cycles = drm_dp_link_data_symbol_cycles(lane_count,
							       slice_pixels,
							       bpp_x16,
							       symbol_size,
							       is_mst);
	int slice_eoc_cycles = 0;

	if (dsc_slice_count)
		slice_eoc_cycles = is_mst ? 4 / lane_count : 1;

	return slice_count * (slice_data_cycles + slice_eoc_cycles);
}

int drm_dp_bw_overhead(int lane_count, int hactive, int dsc_slice_count,
		       int bpp_x16, unsigned long flags)
{
	int symbol_size = flags & DRM_DP_BW_OVERHEAD_UHBR ? 32 : 8;
	bool is_mst = !!(flags & DRM_DP_BW_OVERHEAD_MST);
	u32 overhead = 1000000;
	int symbol_cycles;

	if (lane_count == 0 || hactive == 0 || bpp_x16 == 0)
		return 0;

	if (flags & DRM_DP_BW_OVERHEAD_SSC_REF_CLK)
		overhead += 6000;

	if (flags & DRM_DP_BW_OVERHEAD_FEC)
		overhead += 24016;

	symbol_cycles = drm_dp_link_symbol_cycles(lane_count, hactive,
						  dsc_slice_count,
						  bpp_x16, symbol_size,
						  is_mst);

	return DIV_ROUND_UP_ULL((u64)(u32)(symbol_cycles * symbol_size * lane_count) *
				(u32)(overhead * 16),
				(u64)(hactive * bpp_x16));
}

int drm_dp_bw_channel_coding_efficiency(bool is_uhbr)
{
	return is_uhbr ? 967100 : 800000;
}

int drm_dp_max_dprx_data_rate(int max_link_rate, int max_lanes)
{
	int ch_coding_efficiency =
		drm_dp_bw_channel_coding_efficiency(drm_dp_is_uhbr_rate(max_link_rate));

	return (int)(((u64)(u32)(max_link_rate * 10 * max_lanes) *
		      (u32)ch_coding_efficiency) / (1000000ULL * 8));
}

/* ---- vectors ------------------------------------------------------------- */

/* A DP 1.4 sink with DSC 1.2: 1, 2 and 4 slices, 8-bit line buffer, block
 * prediction, RGB/444/422/420 native, 8/10/12 bpc, slices up to 5120. */
static const u8 sink_dpcd[DP_DSC_RECEIVER_CAP_SIZE] = {
	0x01, 0x21, 0x03, 0x00, 0x0b, 0x08, 0x01, 0x00,
	0x00, 0x1b, 0x0e, 0x00, 0x10, 0x00, 0x00, 0x00
};

struct dsc_expect {
	int slice_width, slice_height, slice_chunk_size, bits_per_pixel;
	int bits_per_component, line_buf_depth, initial_xmit_delay;
	int initial_dec_delay, first_line_bpg_offset, initial_offset;
	int final_offset, nfl_bpg_offset, slice_bpg_offset;
	int scale_increment_interval, scale_decrement_interval;
	int initial_scale_value, rc_bits, mux_word_size;
	int flatness_min_qp, flatness_max_qp;
	int rc_quant_incr_limit0, rc_quant_incr_limit1;
	int second_line_bpg_offset, nsl_bpg_offset, second_line_offset_adj;
	int native_420, convert_rgb, dsc_version_major, dsc_version_minor;
	int block_pred_enable;
};

struct dsc_vector {
	const char *name;
	int verx100, h, v, slices, bpp_x16, pipe_bpp;
	enum intel_dsc_output_format format;
	struct dsc_expect e;
	u8 range[DSC_NUM_BUF_RANGES][3];
	u8 pps[128];
};

static const struct dsc_vector vectors[] = {
	{ "icl_1080p_8bpc_8bpp", 1100, 1920, 1080, 1, 128, 24, INTEL_DSC_FORMAT_RGB,
	  { 1920, 108, 1920, 128, 8, 8, 512, 1216, 12, 6144, 4336, 230, 68, 4257, 26, 32, 13824, 48, 3, 12, 11, 11, 0, 0, 0, 0, 1, 1, 1, 1 },
	  { { 0, 4, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 1, 6, 62 }, { 3, 7, 60 }, { 3, 7, 58 }, { 3, 7, 56 }, { 3, 8, 56 }, { 3, 9, 56 }, { 3, 10, 54 }, { 5, 11, 54 }, { 5, 12, 52 }, { 5, 13, 52 }, { 7, 13, 52 }, { 13, 15, 52 } },
	  {
	    0x11, 0x00, 0x00, 0x88, 0x30, 0x80, 0x04, 0x38, 0x07, 0x80, 0x00, 0x6c, 0x07, 0x80, 0x07, 0x80,
	    0x02, 0x00, 0x04, 0xc0, 0x00, 0x20, 0x10, 0xa1, 0x00, 0x1a, 0x00, 0x0c, 0x00, 0xe6, 0x00, 0x44,
	    0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	    0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19, 0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a, 0xf6,
	    0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x6b, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "tgl_4k_10bpc_10bpp", 1200, 3840, 2160, 2, 160, 30, INTEL_DSC_FORMAT_RGB,
	  { 1920, 108, 2400, 160, 10, 8, 410, 1216, 15, 5632, 4332, 288, 83, 3416, 37, 25, 16260, 48, 7, 16, 15, 15, 0, 0, 0, 0, 1, 1, 1, 1 },
	  { { 0, 7, 2 }, { 4, 8, 0 }, { 5, 9, 0 }, { 6, 10, 62 }, { 7, 11, 60 }, { 7, 11, 58 }, { 7, 11, 56 }, { 7, 12, 56 }, { 7, 13, 56 }, { 7, 13, 54 }, { 9, 14, 54 }, { 9, 14, 54 }, { 9, 15, 52 }, { 11, 15, 52 }, { 15, 16, 52 } },
	  {
	    0x11, 0x00, 0x00, 0xa8, 0x30, 0xa0, 0x08, 0x70, 0x0f, 0x00, 0x00, 0x6c, 0x07, 0x80, 0x09, 0x60,
	    0x01, 0x9a, 0x04, 0xc0, 0x00, 0x19, 0x0d, 0x58, 0x00, 0x25, 0x00, 0x0f, 0x01, 0x20, 0x00, 0x53,
	    0x16, 0x00, 0x10, 0xec, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0xc2, 0x22, 0x00, 0x2a, 0x40,
	    0x32, 0xbe, 0x3a, 0xfc, 0x3a, 0xfa, 0x3a, 0xf8, 0x3b, 0x38, 0x3b, 0x78, 0x3b, 0x76, 0x4b, 0xb6,
	    0x4b, 0xb6, 0x4b, 0xf4, 0x5b, 0xf4, 0x7c, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "tgl_1440p_12bpc_12bpp", 1200, 2560, 1440, 4, 192, 36, INTEL_DSC_FORMAT_RGB,
	  { 640, 120, 960, 192, 12, 8, 341, 780, 15, 2048, 4420, 259, 516, 1460, 107, 10, 13452, 64, 11, 20, 19, 19, 0, 0, 0, 0, 1, 1, 1, 1 },
	  { { 0, 6, 2 }, { 4, 9, 0 }, { 7, 11, 0 }, { 8, 12, 62 }, { 10, 13, 60 }, { 11, 14, 58 }, { 11, 15, 56 }, { 11, 16, 56 }, { 11, 17, 56 }, { 11, 18, 54 }, { 13, 19, 54 }, { 13, 20, 52 }, { 13, 21, 52 }, { 15, 21, 52 }, { 21, 23, 52 } },
	  {
	    0x11, 0x00, 0x00, 0xc8, 0x30, 0xc0, 0x05, 0xa0, 0x0a, 0x00, 0x00, 0x78, 0x02, 0x80, 0x03, 0xc0,
	    0x01, 0x55, 0x03, 0x0c, 0x00, 0x0a, 0x05, 0xb4, 0x00, 0x6b, 0x00, 0x0f, 0x01, 0x03, 0x02, 0x04,
	    0x08, 0x00, 0x11, 0x44, 0x0b, 0x14, 0x20, 0x00, 0x06, 0x13, 0x13, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x82, 0x22, 0x40, 0x3a, 0xc0,
	    0x43, 0x3e, 0x53, 0x7c, 0x5b, 0xba, 0x5b, 0xf8, 0x5c, 0x38, 0x5c, 0x78, 0x5c, 0xb6, 0x6c, 0xf6,
	    0x6d, 0x34, 0x6d, 0x74, 0x7d, 0x74, 0xad, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "adl_4k_8bpc_10bpp", 1300, 3840, 2160, 2, 160, 24, INTEL_DSC_FORMAT_RGB,
	  { 1920, 108, 2400, 160, 8, 8, 410, 1216, 15, 5632, 4332, 288, 83, 3416, 37, 25, 16260, 48, 3, 12, 11, 11, 0, 0, 0, 0, 1, 1, 1, 1 },
	  { { 0, 4, 3 }, { 0, 4, 1 }, { 1, 5, 1 }, { 1, 6, 63 }, { 3, 7, 61 }, { 3, 7, 59 }, { 3, 7, 57 }, { 3, 8, 57 }, { 3, 9, 57 }, { 4, 10, 55 }, { 5, 10, 55 }, { 5, 11, 55 }, { 5, 11, 53 }, { 8, 12, 53 }, { 12, 13, 53 } },
	  {
	    0x11, 0x00, 0x00, 0x88, 0x30, 0xa0, 0x08, 0x70, 0x0f, 0x00, 0x00, 0x6c, 0x07, 0x80, 0x09, 0x60,
	    0x01, 0x9a, 0x04, 0xc0, 0x00, 0x19, 0x0d, 0x58, 0x00, 0x25, 0x00, 0x0f, 0x01, 0x20, 0x00, 0x53,
	    0x16, 0x00, 0x10, 0xec, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x03, 0x01, 0x01, 0x09, 0x41,
	    0x09, 0xbf, 0x19, 0xfd, 0x19, 0xfb, 0x19, 0xf9, 0x1a, 0x39, 0x1a, 0x79, 0x22, 0xb7, 0x2a, 0xb7,
	    0x2a, 0xf7, 0x2a, 0xf5, 0x43, 0x35, 0x63, 0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "adl_4k_10bpc_15bpp", 1300, 3840, 2160, 4, 240, 30, INTEL_DSC_FORMAT_RGB,
	  { 960, 108, 1800, 240, 10, 8, 274, 730, 15, 2048, 4322, 288, 379, 1895, 160, 10, 15060, 48, 7, 16, 15, 15, 0, 0, 0, 0, 1, 1, 1, 1 },
	  { { 0, 6, 60 }, { 4, 8, 58 }, { 5, 9, 60 }, { 5, 10, 58 }, { 7, 11, 56 }, { 7, 11, 54 }, { 7, 11, 52 }, { 7, 12, 54 }, { 7, 13, 56 }, { 7, 13, 54 }, { 9, 14, 55 }, { 9, 14, 55 }, { 9, 14, 53 }, { 12, 15, 53 }, { 15, 16, 53 } },
	  {
	    0x11, 0x00, 0x00, 0xa8, 0x30, 0xf0, 0x08, 0x70, 0x0f, 0x00, 0x00, 0x6c, 0x03, 0xc0, 0x07, 0x08,
	    0x01, 0x12, 0x02, 0xda, 0x00, 0x0a, 0x07, 0x67, 0x00, 0xa0, 0x00, 0x0f, 0x01, 0x20, 0x01, 0x7b,
	    0x08, 0x00, 0x10, 0xe2, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0xbc, 0x22, 0x3a, 0x2a, 0x7c,
	    0x2a, 0xba, 0x3a, 0xf8, 0x3a, 0xf6, 0x3a, 0xf4, 0x3b, 0x36, 0x3b, 0x78, 0x3b, 0x76, 0x4b, 0xb7,
	    0x4b, 0xb7, 0x4b, 0xb5, 0x63, 0xf5, 0x7c, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "mtl_4k_420_8bpc_8bpp", 1400, 3840, 2160, 2, 128, 24, INTEL_DSC_FORMAT_YCBCR420,
	  { 1920, 108, 1920, 256, 8, 8, 256, 684, 15, 2048, 4336, 288, 379, 1901, 160, 10, 15040, 48, 3, 12, 11, 11, 12, 230, 512, 1, 0, 1, 2, 1 },
	  { { 0, 0, 60 }, { 0, 1, 58 }, { 0, 1, 60 }, { 0, 2, 58 }, { 1, 2, 56 }, { 1, 3, 54 }, { 1, 3, 52 }, { 2, 4, 54 }, { 2, 5, 56 }, { 3, 5, 54 }, { 4, 6, 55 }, { 4, 7, 55 }, { 5, 7, 53 }, { 7, 8, 53 }, { 8, 9, 53 } },
	  {
	    0x12, 0x00, 0x00, 0x88, 0x21, 0x00, 0x08, 0x70, 0x0f, 0x00, 0x00, 0x6c, 0x07, 0x80, 0x07, 0x80,
	    0x01, 0x00, 0x02, 0xac, 0x00, 0x0a, 0x07, 0x6d, 0x00, 0xa0, 0x00, 0x0f, 0x01, 0x20, 0x01, 0x7b,
	    0x08, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x00, 0x3c, 0x00, 0x7a, 0x00, 0x7c,
	    0x00, 0xba, 0x08, 0xb8, 0x08, 0xf6, 0x08, 0xf4, 0x11, 0x36, 0x11, 0x78, 0x19, 0x76, 0x21, 0xb7,
	    0x21, 0xf7, 0x29, 0xf5, 0x3a, 0x35, 0x42, 0x75, 0x02, 0x0c, 0x00, 0xe6, 0x02, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ "tgl_444_8bpc_6bpp", 1200, 1920, 1080, 2, 96, 24, INTEL_DSC_FORMAT_YCBCR444,
	  { 960, 108, 720, 96, 8, 8, 768, 1142, 15, 6144, 3824, 288, 136, 3078, 13, 32, 11460, 48, 3, 13, 11, 11, 0, 0, 0, 0, 0, 1, 1, 1 },
	  { { 0, 4, 0 }, { 1, 6, 62 }, { 3, 8, 62 }, { 4, 8, 60 }, { 5, 9, 58 }, { 5, 9, 58 }, { 6, 9, 58 }, { 6, 10, 56 }, { 7, 11, 56 }, { 8, 12, 54 }, { 9, 12, 54 }, { 10, 12, 52 }, { 10, 12, 52 }, { 11, 12, 52 }, { 13, 14, 52 } },
	  {
	    0x11, 0x00, 0x00, 0x88, 0x20, 0x60, 0x04, 0x38, 0x07, 0x80, 0x00, 0x6c, 0x03, 0xc0, 0x02, 0xd0,
	    0x03, 0x00, 0x04, 0x76, 0x00, 0x20, 0x0c, 0x06, 0x00, 0x0d, 0x00, 0x0f, 0x01, 0x20, 0x00, 0x88,
	    0x18, 0x00, 0x0e, 0xf0, 0x03, 0x0d, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	    0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7c, 0x7d, 0x01, 0x00, 0x09, 0xbe, 0x1a, 0x3e,
	    0x22, 0x3c, 0x2a, 0x7a, 0x2a, 0x7a, 0x32, 0x7a, 0x32, 0xb8, 0x3a, 0xf8, 0x43, 0x36, 0x4b, 0x36,
	    0x53, 0x34, 0x53, 0x34, 0x5b, 0x34, 0x6b, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

/* ---- checks -------------------------------------------------------------- */

static void check_config(const char *name, const struct drm_dsc_config *c,
			 const struct dsc_vector *v)
{
	const struct dsc_expect *e = &v->e;
	char what[128];
	int i;

#define F(field) do { snprintf(what, sizeof(what), "%s %s", name, #field); \
		CHECK_EQ(c->field, e->field, what); } while (0)
	F(slice_width); F(slice_height); F(slice_chunk_size); F(bits_per_pixel);
	F(bits_per_component); F(line_buf_depth); F(initial_xmit_delay);
	F(initial_dec_delay); F(first_line_bpg_offset); F(initial_offset);
	F(final_offset); F(nfl_bpg_offset); F(slice_bpg_offset);
	F(scale_increment_interval); F(scale_decrement_interval);
	F(initial_scale_value); F(rc_bits); F(mux_word_size);
	F(flatness_min_qp); F(flatness_max_qp);
	F(rc_quant_incr_limit0); F(rc_quant_incr_limit1);
	F(second_line_bpg_offset); F(nsl_bpg_offset); F(second_line_offset_adj);
	F(native_420); F(convert_rgb); F(dsc_version_major); F(dsc_version_minor);
	F(block_pred_enable);
#undef F
	for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
		snprintf(what, sizeof(what), "%s range %d", name, i);
		CHECK(c->rc_range_params[i].range_min_qp == v->range[i][0] &&
		      c->rc_range_params[i].range_max_qp == v->range[i][1] &&
		      c->rc_range_params[i].range_bpg_offset == v->range[i][2],
		      "%s: got %d/%d/%d, want %d/%d/%d", what,
		      c->rc_range_params[i].range_min_qp, c->rc_range_params[i].range_max_qp,
		      c->rc_range_params[i].range_bpg_offset,
		      v->range[i][0], v->range[i][1], v->range[i][2]);
	}
}

static void check_pps(const char *name, const struct drm_dsc_config *c,
		      const u8 want[128])
{
	struct drm_dsc_picture_parameter_set pps;
	const u8 *got = (const u8 *)&pps;
	int i, bad = -1;

	memset(&pps, 0xa5, sizeof(pps));
	drm_dsc_pps_payload_pack(&pps, c);
	for (i = 0; i < 128; i++)
		if (got[i] != want[i]) {
			bad = i;
			break;
		}
	CHECK(bad < 0, "%s PPS byte %d: got 0x%02x, want 0x%02x", name, bad,
	      bad < 0 ? 0 : got[bad], bad < 0 ? 0 : want[bad]);
}

static void sink_from(struct intel_dsc_sink_caps *s, const u8 *dpcd, u8 fec, bool edp)
{
	intel_dsc_sink_caps_clear(s);
	memcpy(s->dsc_dpcd, dpcd, DP_DSC_RECEIVER_CAP_SIZE);
	s->fec_capability = fec;
	s->is_edp = edp;
	s->valid = true;
}

static void test_basics(void)
{
	struct dp_sdp_header h;
	struct drm_dsc_config c;

	memset(&h, 0xff, sizeof(h));
	drm_dsc_dp_pps_header_init(&h);
	CHECK(h.HB0 == 0 && h.HB1 == 0x10 && h.HB2 == 0x7f && h.HB3 == 0,
	      "PPS SDP header %02x %02x %02x %02x", h.HB0, h.HB1, h.HB2, h.HB3);
	CHECK_EQ(sizeof(struct drm_dsc_picture_parameter_set), 128, "PPS size");
	CHECK_EQ(sizeof(struct drm_dsc_pps_infoframe), 132, "PPS infoframe size");

	CHECK_EQ(drm_dsc_dp_rc_buffer_size(DP_DSC_RC_BUF_BLK_SIZE_1, 0), 1024, "rc buf 1x1");
	CHECK_EQ(drm_dsc_dp_rc_buffer_size(DP_DSC_RC_BUF_BLK_SIZE_4, 3), 16384, "rc buf 4x4");
	CHECK_EQ(drm_dsc_dp_rc_buffer_size(DP_DSC_RC_BUF_BLK_SIZE_16, 0), 16384, "rc buf 16x1");
	CHECK_EQ(drm_dsc_dp_rc_buffer_size(DP_DSC_RC_BUF_BLK_SIZE_64, 1), 131072, "rc buf 64x2");
	CHECK_EQ(drm_dsc_dp_rc_buffer_size(4, 0), 0, "rc buf invalid code");

	memset(&c, 0, sizeof(c));
	c.rc_model_size = 8192;
	c.initial_offset = 6144;
	CHECK_EQ(drm_dsc_initial_scale_value(&c), 32, "initial scale 6144");
	c.initial_offset = 2048;
	CHECK_EQ(drm_dsc_initial_scale_value(&c), 10, "initial scale 2048");
	c.bits_per_component = 8;
	CHECK_EQ(drm_dsc_flatness_det_thresh(&c), 2, "flatness 8 bpc");
	c.bits_per_component = 12;
	CHECK_EQ(drm_dsc_flatness_det_thresh(&c), 32, "flatness 12 bpc");
	c.bits_per_pixel = 10 << 4;
	CHECK_EQ(drm_dsc_get_bpp_int(&c), 10, "bpp int");

	/* constant parameters */
	memset(&c, 0, sizeof(c));
	c.bits_per_component = 12;
	drm_dsc_set_const_params(&c);
	CHECK(c.rc_model_size == 8192 && c.rc_edge_factor == 6 &&
	      c.rc_tgt_offset_high == 3 && c.rc_tgt_offset_low == 3 &&
	      c.mux_word_size == 64, "const params 12 bpc");
	c.bits_per_component = 10;
	c.rc_model_size = 4096;
	drm_dsc_set_const_params(&c);
	CHECK(c.rc_model_size == 4096 && c.mux_word_size == 48, "const params keep model size");

	/* buffer thresholds, and the 6 bpp exception */
	memset(&c, 0, sizeof(c));
	c.bits_per_pixel = 8 << 4;
	drm_dsc_set_rc_buf_thresh(&c);
	CHECK(c.rc_buf_thresh[0] == 14 && c.rc_buf_thresh[7] == 105 &&
	      c.rc_buf_thresh[12] == 125 && c.rc_buf_thresh[13] == 126, "thresholds");
	c.bits_per_pixel = 6 << 4;
	drm_dsc_set_rc_buf_thresh(&c);
	CHECK(c.rc_buf_thresh[12] == 124 && c.rc_buf_thresh[13] == 125, "thresholds 6 bpp");

	/* table lookups that do not exist, and missing inputs */
	memset(&c, 0, sizeof(c));
	c.bits_per_pixel = 7 << 4;
	c.bits_per_component = 8;
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_1_PRE_SCR), -EINVAL, "no 7 bpp set");
	c.bits_per_pixel = 6 << 4;
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_1_PRE_SCR), 0, "6 bpp pre-SCR");
	CHECK(c.initial_xmit_delay == 683 && c.first_line_bpg_offset == 15 &&
	      c.rc_range_params[1].range_bpg_offset == (u8)(-2 & 0x3f), "6 bpp pre-SCR values");
	CHECK_EQ(drm_dsc_setup_rc_params(&c, (enum drm_dsc_params_type)9), -EINVAL, "bad type");
	c.bits_per_pixel = 0;
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_2_444), -EINVAL, "no bpp");
	c.bits_per_pixel = 8 << 4;
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_2_422), 0, "422 8 bpp 8 bpc");
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_2_420), 0, "420 8 bpp 8 bpc");

	/* a transmit delay that leaves no room for the final offset */
	memset(&c, 0, sizeof(c));
	c.slice_width = 1920;
	c.slice_height = 108;
	c.bits_per_pixel = 8 << 4;
	c.bits_per_component = 8;
	c.convert_rgb = true;
	c.mux_word_size = 48;
	c.rc_model_size = 8192;
	c.initial_xmit_delay = 1;
	c.initial_offset = 6144;
	c.initial_scale_value = 32;
	CHECK_EQ(drm_dsc_compute_rc_parameters(&c), -ERANGE, "final offset >= model size");
}

/*
 * The library on its own, the way an encoder without QP tables uses it:
 * 1920x1080 in one slice, 8 bpc, 8 bpp, the DSC 1.1 parameter set.
 */
static void test_library_1080p(void)
{
	const struct dsc_vector *v = &vectors[0];
	struct drm_dsc_config c;

	memset(&c, 0, sizeof(c));
	c.pic_width = 1920;
	c.pic_height = 1080;
	c.slice_count = 1;
	c.slice_width = 1920;
	c.slice_height = 108;
	c.bits_per_component = 8;
	c.bits_per_pixel = 8 << 4;
	c.convert_rgb = true;
	c.line_buf_depth = 8;
	c.block_pred_enable = true;
	c.dsc_version_major = 1;
	c.dsc_version_minor = 1;
	drm_dsc_set_const_params(&c);
	drm_dsc_set_rc_buf_thresh(&c);
	CHECK_EQ(drm_dsc_setup_rc_params(&c, DRM_DSC_1_1_PRE_SCR), 0, "1080p rc params");
	c.initial_scale_value = drm_dsc_initial_scale_value(&c);
	CHECK_EQ(drm_dsc_compute_rc_parameters(&c), 0, "1080p compute");
	check_config("library 1080p", &c, v);
	check_pps("library 1080p", &c, v->pps);
}

static void test_vectors(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(vectors); i++) {
		const struct dsc_vector *v = &vectors[i];
		struct intel_dsc_source_caps src;
		struct intel_dsc_sink_caps sink;
		struct drm_dsc_config c;
		int ret;

		intel_dsc_source_caps_init(&src, v->verx100, 0, 0);
		sink_from(&sink, sink_dpcd, DP_FEC_CAPABLE, false);
		ret = intel_dsc_dp_compute_config(&src, &sink, &c, v->h, v->v, v->slices,
						  v->bpp_x16, v->pipe_bpp, v->format);
		CHECK(ret == 0, "%s: compute %d", v->name, ret);
		if (ret)
			continue;
		check_config(v->name, &c, v);
		check_pps(v->name, &c, v->pps);
	}
}

static void test_qp_tables(void)
{
	static const int pts[][6] = {
		/* min?, bpc, range, bpp index, 420, value */
		{ 1, 8, 0, 0, 0, 0 },
		{ 0, 8, 0, 0, 0, 4 },
		{ 1, 8, 14, 36, 0, 3 },
		{ 0, 8, 14, 36, 0, 4 },
		{ 1, 10, 7, 20, 0, 6 },
		{ 0, 10, 7, 20, 0, 8 },
		{ 1, 12, 3, 60, 0, 0 },
		{ 0, 12, 3, 60, 0, 0 },
		{ 1, 8, 5, 16, 1, 0 },
		{ 0, 8, 5, 16, 1, 0 },
		{ 1, 10, 9, 22, 1, 1 },
		{ 0, 12, 14, 28, 1, 5 },
		{ 1, 12, 0, 0, 1, 0 },
	};
	size_t i;
	int w;

	for (i = 0; i < ARRAY_SIZE(pts); i++) {
		u8 got = pts[i][0] ?
			intel_lookup_range_min_qp(pts[i][1], pts[i][2], pts[i][3], pts[i][4]) :
			intel_lookup_range_max_qp(pts[i][1], pts[i][2], pts[i][3], pts[i][4]);

		CHECK(got == pts[i][5], "qp %s bpc %d range %d bpp %d %s: got %d want %d",
		      pts[i][0] ? "min" : "max", pts[i][1], pts[i][2], pts[i][3],
		      pts[i][4] ? "420" : "444", got, pts[i][5]);
	}

	/* outside the tables: 0 and a warning, never a read past them */
	w = warnings;
	CHECK_EQ(intel_lookup_range_min_qp(8, 0, 37, false), 0, "8 bpc column 37");
	CHECK_EQ(intel_lookup_range_max_qp(8, 15, 0, false), 0, "range 15");
	CHECK_EQ(intel_lookup_range_min_qp(8, 0, 17, true), 0, "420 8 bpc column 17");
	CHECK_EQ(intel_lookup_range_min_qp(14, 0, 0, false), 0, "14 bpc");
	CHECK(warnings == w + 4, "out-of-range lookups warn (%d)", warnings - w);
}

static void test_source_caps(void)
{
	struct intel_dsc_source_caps s;

	intel_dsc_source_caps_init(&s, 900, 0, 0);
	CHECK(!s.has_dsc, "Skylake has no DSC");
	intel_dsc_source_caps_init(&s, 1000, 0, 0);
	CHECK(!s.has_dsc, "Gemini Lake has no DSC");

	intel_dsc_source_caps_init(&s, 1100, 0, 0);
	CHECK(s.has_dsc && !s.has_dsc_mst && s.max_input_bpc == 10 &&
	      s.max_compressed_bpp == 23 && s.dsc_version_minor == 1 &&
	      s.small_joiner_ram_bits == 7680 * 8, "Ice Lake");
	CHECK(!intel_dsc_source_supports_fec(&s, 0, false), "Ice Lake: no FEC on port A");
	CHECK(intel_dsc_source_supports_fec(&s, 1, false), "Ice Lake: FEC on port B");
	CHECK(!intel_dsc_source_supports_fec(&s, 1, true), "Ice Lake: no FEC on MST");
	CHECK(!intel_dsc_source_support(&s, true), "Ice Lake: no engine on transcoder A");
	CHECK(intel_dsc_source_support(&s, false), "Ice Lake: engine elsewhere");

	intel_dsc_source_caps_init(&s, 1200, 0, 0);
	CHECK(s.has_dsc && s.has_dsc_mst && s.max_input_bpc == 12 &&
	      s.max_compressed_bpp == 23 && intel_dsc_source_supports_fec(&s, 0, true) &&
	      intel_dsc_source_support(&s, true), "Tiger Lake");
	intel_dsc_source_caps_init(&s, 1200, INTEL_DSC_DFSM_DISPLAY_DSC_DISABLE, 0);
	CHECK(!s.has_dsc && !s.has_dsc_mst, "Tiger Lake fused off");

	intel_dsc_source_caps_init(&s, 1300, INTEL_DSC_DFSM_DISPLAY_DSC_DISABLE, 0);
	CHECK(s.has_dsc && s.max_compressed_bpp == 27 && s.small_joiner_ram_bits == 17280 * 8 &&
	      s.dsc_version_minor == 1, "Alder Lake (fuse not consulted)");
	intel_dsc_source_caps_init(&s, 1400, 0, 0);
	CHECK(s.dsc_version_minor == 2 && !s.has_dsc_3engines, "Meteor Lake");
	intel_dsc_source_caps_init(&s, 1401, 0, 0);
	CHECK(s.has_dsc_3engines, "Battlemage three engines");
	intel_dsc_source_caps_init(&s, 2000, 0, 1u << 28);
	CHECK(!s.has_dsc, "Lunar Lake DSC removed");
	intel_dsc_source_caps_init(&s, 2000, 0, 2u << 28);
	CHECK(s.has_dsc, "Lunar Lake DSC present");
}

/* A fake DPCD for the cap reads. */
static u8 fake_dpcd[0x100];
static int fake_fail_at = -1;

static int fake_read(void *ctx, unsigned int offset, void *buf, size_t size)
{
	(void)ctx;
	if (fake_fail_at >= 0 && (unsigned int)fake_fail_at >= offset &&
	    (unsigned int)fake_fail_at < offset + size)
		return -EIO;
	if (offset + size > sizeof(fake_dpcd))
		return -EIO;
	memcpy(buf, fake_dpcd + offset, size);
	return 0;
}

static void test_sink_caps(void)
{
	struct intel_dsc_sink_caps s;
	struct drm_dp_desc desc;

	memset(fake_dpcd, 0, sizeof(fake_dpcd));
	memcpy(fake_dpcd + DP_DSC_SUPPORT, sink_dpcd, sizeof(sink_dpcd));
	fake_dpcd[DP_FEC_CAPABILITY] = DP_FEC_CAPABLE;
	fake_dpcd[DP_DSC_BRANCH_OVERALL_THROUGHPUT_0] = 1;	/* 680 MP/s */
	fake_dpcd[DP_DSC_BRANCH_OVERALL_THROUGHPUT_1] = 4;	/* 800 MP/s */
	fake_dpcd[DP_DSC_BRANCH_MAX_LINE_WIDTH] = 16;		/* 5120 */

	/* DP 1.2: nothing to read */
	CHECK_EQ(intel_dsc_read_dp_sink_caps(&s, 0x12, NULL, false, fake_read, NULL), 0, "dp 1.2");
	CHECK(!s.valid && !drm_dp_sink_supports_dsc(s.dsc_dpcd), "dp 1.2 no caps");

	/* DP 1.4 sink */
	CHECK_EQ(intel_dsc_read_dp_sink_caps(&s, 0x14, NULL, false, fake_read, NULL), 0, "dp 1.4");
	CHECK(s.valid && !memcmp(s.dsc_dpcd, sink_dpcd, sizeof(sink_dpcd)) &&
	      s.fec_capability == DP_FEC_CAPABLE && !s.is_edp, "dp 1.4 caps");
	CHECK(s.overall_throughput.rgb_yuv444 == INTEL_DSC_NO_LIMIT &&
	      s.max_line_width == INTEL_DSC_NO_LIMIT, "no branch limits on a sink");

	/* behind a branch device, with the throughput quirk */
	memset(&desc, 0, sizeof(desc));
	desc.quirks = BIT(DP_DPCD_QUIRK_DSC_THROUGHPUT_BPP_LIMIT);
	desc.ident.hw_rev = 0x10;
	CHECK_EQ(intel_dsc_read_dp_sink_caps(&s, 0x14, &desc, true, fake_read, NULL), 0, "branch");
	CHECK(s.overall_throughput.rgb_yuv444 == 680000 &&
	      s.overall_throughput.yuv422_420 == 800000 &&
	      s.max_line_width == 5120 && s.throughput_quirk, "branch limits %d %d %d %d",
	      s.overall_throughput.rgb_yuv444, s.overall_throughput.yuv422_420,
	      s.max_line_width, s.throughput_quirk);

	/* a failed FEC read, and a failed block read */
	fake_fail_at = DP_FEC_CAPABILITY;
	CHECK(intel_dsc_read_dp_sink_caps(&s, 0x14, NULL, false, fake_read, NULL) < 0, "fec read fails");
	CHECK(!s.valid, "fec read failure leaves no caps");
	fake_fail_at = DP_DSC_SUPPORT + 3;
	CHECK(intel_dsc_read_dp_sink_caps(&s, 0x14, NULL, false, fake_read, NULL) < 0, "dsc read fails");
	CHECK(!s.valid && s.fec_capability == 0, "dsc read failure leaves no caps");
	fake_fail_at = -1;

	/* eDP 1.4 panel: block read, no FEC */
	CHECK_EQ(intel_dsc_read_edp_sink_caps(&s, DP_EDP_13, fake_read, NULL), 0, "edp 1.3");
	CHECK(!s.valid && s.is_edp, "edp 1.3 no caps");
	CHECK_EQ(intel_dsc_read_edp_sink_caps(&s, DP_EDP_14, fake_read, NULL), 0, "edp 1.4");
	CHECK(s.valid && s.is_edp && s.fec_capability == 0 &&
	      drm_dp_sink_supports_dsc(s.dsc_dpcd), "edp 1.4 caps");

	intel_dsc_log_sink_caps("test", &s);
}

static void test_limits(void)
{
	struct intel_dsc_source_caps src;
	struct intel_dsc_sink_caps sink;
	u8 d[DP_DSC_RECEIVER_CAP_SIZE];

	sink_from(&sink, sink_dpcd, DP_FEC_CAPABLE, false);

	intel_dsc_source_caps_init(&src, 1100, 0, 0);
	CHECK_EQ(intel_dsc_compute_max_pipe_bpp(&src, &sink, 0xff), 30, "ICL max pipe bpp");
	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	CHECK_EQ(intel_dsc_compute_max_pipe_bpp(&src, &sink, 0xff), 36, "TGL max pipe bpp");
	CHECK_EQ(intel_dsc_compute_max_pipe_bpp(&src, &sink, 9), 24, "TGL max 9 bpc");

	/* VESA list before version 13: 8 .. 15 */
	CHECK_EQ(intel_dsc_min_compressed_bpp_x16(&src, &sink, INTEL_DSC_FORMAT_RGB), 128, "TGL min");
	CHECK_EQ(intel_dsc_max_compressed_bpp_x16(&src, &sink, 300000, 1920,
						  INTEL_DSC_FORMAT_RGB, 24, INTEL_DSC_NO_LIMIT),
		 15 << 4, "TGL max 1080p 8 bpc");
	/* joiner RAM: 61440 / 3840 = 16, then 15 from the list */
	CHECK_EQ(intel_dsc_max_compressed_bpp_x16(&src, &sink, 600000, 3840,
						  INTEL_DSC_FORMAT_RGB, 36, INTEL_DSC_NO_LIMIT),
		 15 << 4, "TGL max 4k");
	/* 4:2:0: 3 x (bpc / 2) - 1 step */
	CHECK_EQ(intel_dsc_max_compressed_bpp_x16(&src, &sink, 600000, 1920,
						  INTEL_DSC_FORMAT_YCBCR420, 24, INTEL_DSC_NO_LIMIT),
		 10 << 4, "TGL max 420 8 bpc");

	/* version 14 with 1/16 bpp steps */
	memcpy(d, sink_dpcd, sizeof(d));
	d[DP_DSC_BITS_PER_PIXEL_INC - DP_DSC_SUPPORT] = DP_DSC_BITS_PER_PIXEL_1_16;
	sink_from(&sink, d, DP_FEC_CAPABLE, false);
	intel_dsc_source_caps_init(&src, 1400, 0, 0);
	CHECK_EQ(intel_dsc_bpp_step_x16(&src, &sink), 1, "MTL 1/16 step");
	CHECK_EQ(intel_dsc_max_compressed_bpp_x16(&src, &sink, 300000, 1920,
						  INTEL_DSC_FORMAT_RGB, 24, INTEL_DSC_NO_LIMIT),
		 (24 << 4) - 1, "MTL max 1080p 8 bpc");
	intel_dsc_source_caps_init(&src, 1300, 0, 0);
	CHECK_EQ(intel_dsc_bpp_step_x16(&src, &sink), 16, "ADL whole steps");

	/* the sink's own maximum (eDP 1.4: DPCD 0x67/0x68) */
	d[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT] = 12 << 4;
	sink_from(&sink, d, 0, true);
	CHECK_EQ(intel_dsc_max_compressed_bpp_x16(&src, &sink, 300000, 1920,
						  INTEL_DSC_FORMAT_RGB, 30, INTEL_DSC_NO_LIMIT),
		 12 << 4, "eDP sink max 12");

	/* slice counts */
	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	sink_from(&sink, sink_dpcd, DP_FEC_CAPABLE, false);
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 148500, 1920, 0), 1, "1080p60 slices");
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 594000, 3840, 0), 2, "4k60 slices");
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 1075000, 3840, 0), 4, "4k120 slices");
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 148500, 1920, 170000), 2, "near max cdclk");
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 1500000, 3840, 0), 0, "too fast");
	sink_from(&sink, sink_dpcd, 0, true);
	CHECK_EQ(intel_dsc_get_slice_count(&src, &sink, 148500, 1920, 0), 4, "eDP max slices");

	CHECK_EQ(intel_dsc_dp_slice_height(2160), 108, "slice height 2160");
	CHECK_EQ(intel_dsc_dp_slice_height(1440), 120, "slice height 1440");
	CHECK_EQ(intel_dsc_dp_slice_height(1050), 150, "slice height 1050");
	CHECK_EQ(intel_dsc_dp_slice_height(100), 2, "slice height 100");
}

static void test_verdict(void)
{
	struct intel_dsc_source_caps src;
	struct intel_dsc_sink_caps sink;
	struct intel_dsc_verdict_args a;
	struct intel_dsc_verdict_info info;
	u8 d[DP_DSC_RECEIVER_CAP_SIZE];
	enum intel_dsc_verdict v;

	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	sink_from(&sink, sink_dpcd, DP_FEC_CAPABLE, false);

	/* 4k60 on HBR2 x4: fits at 18 bpp */
	memset(&a, 0, sizeof(a));
	a.link_clock = 540000;
	a.lane_count = 4;
	a.mode_clock = 533250;
	a.mode_hdisplay = 3840;
	a.format = INTEL_DSC_FORMAT_RGB;
	a.port = 1;
	v = intel_dsc_mode_verdict(&src, &sink, &a, &info);
	CHECK(v == INTEL_DSC_NOT_NEEDED && info.max_rate == 2160000 &&
	      info.mode_rate == 1199813, "4k60 HBR2x4: %d %d %d", v, info.max_rate, info.mode_rate);

	/* 4k120 on HBR2 x4: needs DSC, fits with 4 slices at 8 bpp */
	a.mode_clock = 1075000;
	v = intel_dsc_mode_verdict(&src, &sink, &a, &info);
	CHECK(v == INTEL_DSC_NEEDED && info.mode_rate == 2418750 && info.slice_count == 4 &&
	      info.pipe_bpp == 36 && info.min_bpp_x16 == 128 && info.max_bpp_x16 == 240 &&
	      info.dsc_rate == 1105670,
	      "4k120 HBR2x4: %d rate %d slices %d pipe %d bpp %d..%d dsc %d", v,
	      info.mode_rate, info.slice_count, info.pipe_bpp, info.min_bpp_x16,
	      info.max_bpp_x16, info.dsc_rate);
	intel_dsc_log_verdict("test", 3840, 2160, a.mode_clock, v, &info);

	/* the same on UHBR10 x4 (128b/132b): fits uncompressed */
	a.link_clock = 1000000;
	v = intel_dsc_mode_verdict(&src, &sink, &a, &info);
	CHECK(v == INTEL_DSC_NOT_NEEDED && info.max_rate == 4835500, "4k120 UHBR10x4: %d %d",
	      v, info.max_rate);

	/* no FEC in the sink: external DP cannot compress */
	a.link_clock = 540000;
	sink_from(&sink, sink_dpcd, 0, false);
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_UNSUPPORTED,
		 "no sink FEC");

	/* Ice Lake port A: no FEC at the source */
	intel_dsc_source_caps_init(&src, 1100, 0, 0);
	sink_from(&sink, sink_dpcd, DP_FEC_CAPABLE, false);
	a.port = 0;
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_UNSUPPORTED,
		 "ICL port A");
	a.port = 1;
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED, "ICL port B");

	/* Skylake: no engines */
	intel_dsc_source_caps_init(&src, 900, 0, 0);
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_UNSUPPORTED,
		 "SKL");

	/* eDP panel on HBR2 x2 with a 8 bpp maximum: needs DSC, gets 4 slices */
	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	memcpy(d, sink_dpcd, sizeof(d));
	d[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT] = 8 << 4;
	sink_from(&sink, d, 0, true);
	a.lane_count = 2;
	a.mode_clock = 533250;
	a.port = 0;
	v = intel_dsc_mode_verdict(&src, &sink, &a, &info);
	CHECK(v == INTEL_DSC_NEEDED && info.slice_count == 4 && info.max_rate == 1080000,
	      "eDP 4k60 HBR2x2: %d slices %d rate %d", v, info.slice_count, info.max_rate);
	/* and the VBT may forbid it */
	a.vbt_edp_dsc_disable = true;
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_UNSUPPORTED,
		 "eDP DSC disabled by VBT");
	a.vbt_edp_dsc_disable = false;
	/* without a maximum the panel cannot be compressed for */
	d[DP_DSC_MAX_BITS_PER_PIXEL_LOW - DP_DSC_SUPPORT] = 0;
	sink_from(&sink, d, 0, true);
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_NO_FIT,
		 "eDP without max bpp");

	/* a sink that takes 1 slice only, 4k120: no slice count fits */
	memcpy(d, sink_dpcd, sizeof(d));
	d[DP_DSC_SLICE_CAP_1 - DP_DSC_SUPPORT] = DP_DSC_1_PER_DP_DSC_SINK;
	sink_from(&sink, d, DP_FEC_CAPABLE, false);
	a.lane_count = 4;
	a.mode_clock = 1075000;
	a.port = 1;
	CHECK_EQ(intel_dsc_mode_verdict(&src, &sink, &a, NULL), INTEL_DSC_NEEDED_NO_FIT,
		 "1-slice sink at 4k120");
}

static void test_vbt(void)
{
	u8 block[2 + INTEL_VBT_DSC_ENTRY_SIZE * INTEL_VBT_DSC_NUM_ENTRIES];
	struct intel_vbt_dsc_params p[INTEL_VBT_DSC_NUM_ENTRIES];
	static const u8 entry[INTEL_VBT_DSC_ENTRY_SIZE] = {
		0x11,			/* DSC 1.1 */
		0x00, 0x07,		/* 1 KB blocks, 8 of them */
		0x06, 0x00, 0x00, 0x00,	/* 2 or 4 slices */
		0x05,			/* line buffer 13 bits */
		0x01,			/* block prediction */
		0x01,			/* max 8 bpp */
		0x0e,			/* 8/10/12 bpc */
		0x6c, 0x00,		/* slice height 108 */
	};
	struct intel_dsc_source_caps src;
	struct drm_dsc_config c;
	int pipe_bpp = 0, bpp_x16 = 0, slices = 0;

	memset(block, 0, sizeof(block));
	block[0] = INTEL_VBT_DSC_ENTRY_SIZE;
	memcpy(block + 2 + 3 * INTEL_VBT_DSC_ENTRY_SIZE, entry, sizeof(entry));

	CHECK_EQ(intel_vbt_dsc_parse_block(block, sizeof(block), 197, p), -EINVAL, "BDB 197");
	CHECK_EQ(intel_vbt_dsc_parse_block(block, sizeof(block) - 1, 198, p), -EINVAL, "short");
	block[0] = 12;
	CHECK_EQ(intel_vbt_dsc_parse_block(block, sizeof(block), 198, p), -EINVAL, "entry size");
	block[0] = INTEL_VBT_DSC_ENTRY_SIZE;
	CHECK_EQ(intel_vbt_dsc_parse_block(block, sizeof(block), 198, p), 16, "parse");
	CHECK(p[3].version_major == 1 && p[3].version_minor == 1 &&
	      p[3].rc_buffer_block_size == 0 && p[3].rc_buffer_size == 7 &&
	      p[3].slices_per_line == 6 && p[3].line_buffer_depth == 5 &&
	      p[3].block_prediction_enable && p[3].max_bpp == 1 &&
	      p[3].support_8bpc && p[3].support_10bpc && p[3].support_12bpc &&
	      p[3].slice_height == 108, "entry 3 decoded");
	CHECK(p[0].slices_per_line == 0 && p[0].slice_height == 0, "entry 0 empty");

	intel_dsc_source_caps_init(&src, 1200, 0, 0);
	memset(&c, 0, sizeof(c));
	CHECK_EQ(intel_vbt_dsc_fill_config(&src, &p[3], 10, 1920, &c, &pipe_bpp, &bpp_x16,
					   &slices), 0, "fill");
	CHECK(pipe_bpp == 30 && bpp_x16 == 128 && slices == 4 && c.rc_model_size == 8192 &&
	      c.line_buf_depth == 13 && c.block_pred_enable && c.slice_height == 108 &&
	      c.dsc_version_major == 1 && c.dsc_version_minor == 1,
	      "fill values: bpp %d/%d slices %d model %d lbd %d", pipe_bpp, bpp_x16, slices,
	      c.rc_model_size, c.line_buf_depth);

	/* no usable depth: 8 bpc; a width the slices do not divide still fills */
	p[3].support_8bpc = p[3].support_10bpc = p[3].support_12bpc = false;
	p[3].slices_per_line = 0;
	CHECK_EQ(intel_vbt_dsc_fill_config(&src, &p[3], 10, 1918, &c, &pipe_bpp, &bpp_x16,
					   &slices), 0, "fill unsupported bpc");
	CHECK(pipe_bpp == 24 && bpp_x16 == 128 && slices == 1, "fallback 8 bpc, 1 slice (%d %d %d)",
	      pipe_bpp, bpp_x16, slices);
}

int main(void)
{
	test_basics();
	test_library_1080p();
	test_vectors();
	test_qp_tables();
	test_source_caps();
	test_sink_caps();
	test_limits();
	test_verdict();
	test_vbt();

	printf("test-drm-dsc: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
