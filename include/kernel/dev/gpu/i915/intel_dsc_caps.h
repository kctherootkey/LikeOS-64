// LikeOS -- what the display engine and a DP/eDP sink can do with Display
// Stream Compression, and whether a mode needs it.
//
// Compression is negotiated from three sides:
//  - the source: display version 11 (Ice Lake) and later have DSC engines
//    unless fused off; the version decides the input depths (8..10 on
//    version 11, 8..12 from 12 on), the compressed bpp range (8..23, or
//    8..27 from version 13 on, in any step from 13 on, else only the
//    VESA list), the small-joiner RAM, the DSC minor version (1.2 from
//    14 on) and where FEC can run (every port from 12 on; on 11 every
//    port but A, never MST);
//  - the sink: the DSC capability block at DPCD 0x60..0x6f (DP 1.4 and
//    eDP 1.4 sinks), FEC at 0x90 (external DP carries DSC only with
//    FEC), and a branch device's overall throughput and line width
//    limits at 0xa0..0xa2;
//  - the firmware: the VBT's compression parameters block (for DSI
//    panels, and a per-panel "no DSC" for eDP).
//
// intel_dsc_mode_verdict() answers "does this mode fit the link without
// compression, and if not, would it fit compressed" with the same
// arithmetic the modeset uses (link payload rate with 8b/10b or
// 128b/132b coding efficiency, stream overhead with FEC/SSC/slices, the
// compressed bpp limits of source and sink, the slice count the sink and
// the engine agree on).  intel_dsc_dp_compute_config() fills a
// drm_dsc_config for a chosen slice count and compressed bpp.  Nothing
// here programs hardware.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation
// Portions Copyright (C) 2018 Intel Corporation
// Portions Copyright (C) 2006 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_DSC_CAPS_H
#define KERNEL_DEV_GPU_I915_INTEL_DSC_CAPS_H

#include <kernel/dev/gpu/drm_dsc.h>
#include <kernel/dev/gpu/drm_dp.h>

struct drm_dp_aux;
struct drm_dp_desc;

/* "No limit" for the throughput and line width caps. */
#define INTEL_DSC_NO_LIMIT 0x7fffffff

/* The sink's line buffer depth is capped to what the encoder supports. */
#define INTEL_DP_DSC_MAX_LINE_BUF_DEPTH 13

enum intel_dsc_output_format {
	INTEL_DSC_FORMAT_RGB = 0,
	INTEL_DSC_FORMAT_YCBCR444,
	INTEL_DSC_FORMAT_YCBCR420,
};

/* ---- source ------------------------------------------------------------- */

struct intel_dsc_source_caps {
	int display_ver; /* major version */
	int display_verx100;
	bool has_dsc; /* engines present and not fused off */
	bool has_dsc_mst; /* DSC on MST streams (version 12 on) */
	bool has_dsc_3engines; /* three engines per pipe (Battlemage) */
	int min_input_bpc; /* 8 */
	int max_input_bpc; /* 10 on version 11, 12 later */
	int min_compressed_bpp; /* 8 */
	int max_compressed_bpp; /* 23 before version 13, 27 later */
	int dsc_version_minor; /* 2 from version 14 on, else 1 */
	int small_joiner_ram_bits;
};

/* The fuse and capability registers that can take the engines away. */
#define INTEL_DSC_DFSM_REG 0x51000 /* read on display versions 10..12 */
#define INTEL_DSC_DFSM_DISPLAY_DSC_DISABLE (1u << 7)
#define INTEL_DSC_DE_CAP_REG 0x41100 /* read on display version 20+ */
#define INTEL_DSC_DE_CAP_DSC_MASK GENMASK(29, 28)
#define INTEL_DSC_DE_CAP_DSC_REMOVED 1u

/*
 * Fill @src for a display version (x100, as intel_display_verx100())
 * and the fuse registers: @dfsm = INTEL_DSC_DFSM_REG on versions 10 to
 * 12, @de_cap = INTEL_DSC_DE_CAP_REG from version 20 on, 0 otherwise.
 */
void intel_dsc_source_caps_init(struct intel_dsc_source_caps *src,
				int display_verx100, u32 dfsm, u32 de_cap);

/* DSC on this transcoder (version 11 has no engine on transcoder A). */
bool intel_dsc_source_support(const struct intel_dsc_source_caps *src,
			      bool cpu_transcoder_is_a);
/* FEC on this port (port 0 = A). */
bool intel_dsc_source_supports_fec(const struct intel_dsc_source_caps *src,
				   int port, bool mst);

/* ---- sink ---------------------------------------------------------------- */

struct intel_dsc_sink_caps {
	u8 dsc_dpcd[DP_DSC_RECEIVER_CAP_SIZE]; /* DPCD 0x60..0x6f, 0 = none */
	u8 fec_capability; /* DPCD 0x90 (DP only) */
	struct {
		int rgb_yuv444; /* kPixels/s, INTEL_DSC_NO_LIMIT = none */
		int yuv422_420;
	} overall_throughput;
	int max_line_width; /* pixels, INTEL_DSC_NO_LIMIT = none */
	/* A branch device that cannot take more than 12 bpp above half its
	 * overall throughput. */
	bool throughput_quirk;
	bool is_edp;
	bool valid; /* the block was read */
};

/*
 * How the caps are read: @size bytes of DPCD at @offset into @buf.
 * Returns 0 or a negative errno.  intel_dsc_dpcd_read_drm_aux() adapts a
 * struct drm_dp_aux (ctx); any other AUX implementation passes its own.
 */
typedef int (*intel_dsc_dpcd_read_fn)(void *ctx, unsigned int offset,
				      void *buf, size_t size);
int intel_dsc_dpcd_read_drm_aux(void *ctx /* struct drm_dp_aux * */,
				unsigned int offset, void *buf, size_t size);

void intel_dsc_sink_caps_clear(struct intel_dsc_sink_caps *caps);
/*
 * External DP: @dpcd_rev is DPCD 0x000; the caps are read only for DP
 * 1.4 and later.  @desc (may be NULL) is the sink/branch identification
 * for the throughput quirk; @is_branch says whether a branch device is
 * in front (its throughput and line width limits are read then).
 * Returns 0 (caps read, or none to read) or a negative errno.
 */
int intel_dsc_read_dp_sink_caps(struct intel_dsc_sink_caps *caps, u8 dpcd_rev,
				const struct drm_dp_desc *desc, bool is_branch,
				intel_dsc_dpcd_read_fn read, void *ctx);
/* eDP: @edp_dpcd_rev is DPCD 0x700; the caps exist from eDP 1.4 on. */
int intel_dsc_read_edp_sink_caps(struct intel_dsc_sink_caps *caps,
				 u8 edp_dpcd_rev,
				 intel_dsc_dpcd_read_fn read, void *ctx);
/* One log line with the decoded caps, prefixed by @tag. */
void intel_dsc_log_sink_caps(const char *tag,
			     const struct intel_dsc_sink_caps *caps);

/* Source and sink both do DSC (eDP: unless the VBT disables it). */
bool intel_dsc_has_dsc(const struct intel_dsc_source_caps *src,
		       const struct intel_dsc_sink_caps *sink,
		       bool mst, bool vbt_edp_dsc_disable);
/* ... and FEC where the link needs it, and an engine on the transcoder. */
bool intel_dsc_supports_dsc(const struct intel_dsc_source_caps *src,
			    const struct intel_dsc_sink_caps *sink,
			    int port, bool mst, bool cpu_transcoder_is_a,
			    bool vbt_edp_dsc_disable);
bool intel_dsc_supports_fec(const struct intel_dsc_source_caps *src,
			    const struct intel_dsc_sink_caps *sink,
			    int port, bool mst);
bool intel_dsc_supports_format(const struct intel_dsc_source_caps *src,
			       const struct intel_dsc_sink_caps *sink,
			       enum intel_dsc_output_format format);

/* ---- limits ------------------------------------------------------------ */

/* Highest pipe bpp (3 x bpc) both ends take as DSC input, <= max_req_bpc. */
int intel_dsc_compute_max_pipe_bpp(const struct intel_dsc_source_caps *src,
				   const struct intel_dsc_sink_caps *sink,
				   int max_req_bpc);
/* Compressed bpp step in 1/16 bpp (16 = whole bits). */
int intel_dsc_bpp_step_x16(const struct intel_dsc_source_caps *src,
			   const struct intel_dsc_sink_caps *sink);
int intel_dsc_min_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
				     const struct intel_dsc_sink_caps *sink,
				     enum intel_dsc_output_format format);
int intel_dsc_max_compressed_bpp_x16(const struct intel_dsc_source_caps *src,
				     const struct intel_dsc_sink_caps *sink,
				     int mode_clock, int mode_hdisplay,
				     enum intel_dsc_output_format format,
				     int pipe_max_bpp, int max_link_bpp_x16);
/*
 * Slices per line for a mode on one pipe: the smallest count the sink,
 * the engines and the mode's width agree on, or 0.  @max_cdclk_khz (0 =
 * unknown) asks for a second slice near the CDCLK limit.
 */
int intel_dsc_get_slice_count(const struct intel_dsc_source_caps *src,
			      const struct intel_dsc_sink_caps *sink,
			      int mode_clock, int mode_hdisplay,
			      int max_cdclk_khz);

/* ---- link bandwidth ------------------------------------------------------ */

/* Stream allocation overhead (100% + overhead, ppm); flags DRM_DP_BW_OVERHEAD_*. */
int intel_dsc_link_bw_overhead(int link_clock, int lane_count, int hdisplay,
			       int dsc_slice_count, int bpp_x16,
			       unsigned long flags);
/* kB/s a mode needs on the link at @link_bpp_x16. */
int intel_dsc_link_required(int link_clock, int lane_count,
			    int mode_clock, int mode_hdisplay,
			    int link_bpp_x16, unsigned long bw_overhead_flags);
/* kB/s the link carries (8b/10b below UHBR, 128b/132b from UHBR10). */
int intel_dsc_max_link_data_rate(int link_clock, int lane_count);

/* ---- verdict --------------------------------------------------------------- */

enum intel_dsc_verdict {
	INTEL_DSC_NOT_NEEDED = 0, /* fits uncompressed at the lowest bpp */
	INTEL_DSC_NEEDED, /* too big uncompressed, fits compressed */
	INTEL_DSC_NEEDED_UNSUPPORTED, /* too big, and no DSC on this path */
	INTEL_DSC_NEEDED_NO_FIT, /* too big even compressed */
};

struct intel_dsc_verdict_args {
	int link_clock; /* per lane, 10 kbit/s units (162000 .. 2000000) */
	int lane_count;
	int mode_clock; /* kHz */
	int mode_hdisplay;
	enum intel_dsc_output_format format;
	int port; /* 0 = A */
	bool is_edp;
	bool cpu_transcoder_is_a;
	bool vbt_edp_dsc_disable;
	int max_cdclk_khz; /* 0 = unknown */
};

struct intel_dsc_verdict_info {
	int max_rate; /* kB/s the link carries */
	int mode_rate; /* kB/s uncompressed at the lowest bpp */
	int pipe_bpp; /* DSC input */
	int min_bpp_x16, max_bpp_x16; /* compressed range */
	int slice_count;
	int dsc_rate; /* kB/s at min_bpp_x16 with DSC, 0 not computed */
};

enum intel_dsc_verdict
intel_dsc_mode_verdict(const struct intel_dsc_source_caps *src,
		       const struct intel_dsc_sink_caps *sink,
		       const struct intel_dsc_verdict_args *args,
		       struct intel_dsc_verdict_info *info);
const char *intel_dsc_verdict_name(enum intel_dsc_verdict v);
/* One log line for a verdict, prefixed by @tag; @info may be NULL. */
void intel_dsc_log_verdict(const char *tag, int hdisplay, int vdisplay,
			   int mode_clock, enum intel_dsc_verdict v,
			   const struct intel_dsc_verdict_info *info);

/* ---- configuration ---------------------------------------------------------- */

/*
 * The DSC parameters for one pipe of a DP/eDP stream: picture
 * @hdisplay x @vdisplay, @slice_count slices per line, compressed
 * @compressed_bpp_x16, @pipe_bpp input, @format.  Fills @cfg completely
 * (rate control included); 0 or a negative errno.
 */
int intel_dsc_dp_compute_config(const struct intel_dsc_source_caps *src,
				const struct intel_dsc_sink_caps *sink,
				struct drm_dsc_config *cfg,
				int hdisplay, int vdisplay, int slice_count,
				int compressed_bpp_x16, int pipe_bpp,
				enum intel_dsc_output_format format);
/*
 * The encoder-side part shared with DSI: slice width and dimension
 * checks, colour flags, bpp/bpc, buffer thresholds, the RC parameter
 * sets (the C-model formula from version 13 on unless @dsi_dsc_1_1, the
 * fixed tables before), mux word size and initial scale.  @cfg's
 * pic_width/pic_height, slice_height and rc_model_size must be set.
 */
int intel_dsc_compute_params(const struct intel_dsc_source_caps *src,
			     struct drm_dsc_config *cfg, int slice_count,
			     int compressed_bpp_x16, int pipe_bpp,
			     enum intel_dsc_output_format format,
			     bool dsi_dsc_1_1);
/* A slice height that divides @vactive (108 lines preferred). */
int intel_dsc_dp_slice_height(int vactive);

/* ---- VBT compression parameters (block 56, BDB 198+) ---------------------- */

#define INTEL_VBT_BDB_COMPRESSION_PARAMETERS 56
#define INTEL_VBT_DSC_ENTRY_SIZE 13
#define INTEL_VBT_DSC_NUM_ENTRIES 16

struct intel_vbt_dsc_params {
	u8 version_major;
	u8 version_minor;
	u8 rc_buffer_block_size; /* DPCD 0x62 code */
	u8 rc_buffer_size; /* DPCD 0x63: blocks - 1 */
	u32 slices_per_line; /* bit 0 = 1, bit 1 = 2, bit 2 = 4 slices */
	u8 line_buffer_depth; /* bits - 8 */
	bool block_prediction_enable;
	u8 max_bpp; /* 6 + 2 * value */
	bool support_8bpc, support_10bpc, support_12bpc;
	u16 slice_height;
};

/* Decode one 13-byte entry. */
void intel_vbt_dsc_decode_entry(const u8 *raw, struct intel_vbt_dsc_params *out);
/*
 * Decode the block's payload (@block, @len bytes after the 3-byte block
 * header) into @out[16].  Returns the entry count, or -EINVAL when the
 * BDB version (< 198), the entry size or the length is wrong.
 */
int intel_vbt_dsc_parse_block(const u8 *block, unsigned int len, u16 bdb_version,
			      struct intel_vbt_dsc_params out[INTEL_VBT_DSC_NUM_ENTRIES]);
/*
 * Turn an entry into the panel's DSC setup for a link whose input depth
 * may go up to @dsc_max_bpc: version, depth (@pipe_bpp), compressed bpp
 * (@compressed_bpp_x16), slices per line, RC buffer size, line buffer
 * depth, block prediction, slice height.  @hdisplay only for the
 * divisibility warning.  Returns 0 or -EINVAL.
 */
int intel_vbt_dsc_fill_config(const struct intel_dsc_source_caps *src,
			      const struct intel_vbt_dsc_params *dsc,
			      int dsc_max_bpc, int hdisplay,
			      struct drm_dsc_config *cfg, int *pipe_bpp,
			      int *compressed_bpp_x16, int *slices_per_line);

#endif /* KERNEL_DEV_GPU_I915_INTEL_DSC_CAPS_H */
