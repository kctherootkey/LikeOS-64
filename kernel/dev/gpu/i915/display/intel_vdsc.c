// LikeOS -- display stream compression (VDSC) on the Intel display
// pipes.
//
// A pipe whose DisplayPort or eDP link cannot carry its mode
// uncompressed may send it compressed (intel_dp_dsc.c chooses how): the
// pipe runs at the compressor's input depth, one engine per slice
// column (two when a line is split into two or four slices) compresses
// the picture before the transcoder, and the link carries the
// compressed rate.  The sink is told beforehand -- its decompression
// switched on, and on external DisplayPort FEC announced, before the
// link is trained -- and once the link runs it gets the picture
// parameter set as a secondary data packet, the same set the engines are
// programmed with.
//
// The engines of display version 12 and later belong to the pipe; on
// version 11 pipes B and C have them, and the embedded-panel transcoder
// uses the transcoder-level pair, which lives in power well 2 (as does
// pipe A's pair on version 12 parts other than Rocket Lake).
//
// With I915_FEAT_DSC 0 nothing here does anything: a mode the
// uncompressed link cannot carry is refused, and every pipe runs 24 bits
// per pixel on the link.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2018 Intel Corporation
// Portions Copyright (C) 2012 Intel Corporation
// Portions Copyright (C) 2008 Intel Corporation
// Portions Copyright (C) 2006-2009 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_device_info.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dsc.h>
#include <kernel/dev/gpu/i915/intel_vdsc_regs.h>
#include <kernel/dev/gpu/drm_dsc_helper.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* FEC's share of an 8b/10b link (1/0.972261: 2.4% parity and 0.453%
 * DSC overhead), in parts per million of the stream. */
#define INTEL_DSC_FEC_OVERHEAD_PPM 1028530

/* The power domain of the engines that live in power well 2. */
#define DSC_PW2_DOMAIN INTEL_PW_VDSC

void intel_dsc_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	(void)i915;
	(void)drv;
}

uint32_t intel_dsc_link_bpp_x16(const struct intel_pipe *p)
{
	if (p->dsc.enable)
		return p->dsc.compressed_bpp_x16;
	return (p->dsc.pipe_bpp ? p->dsc.pipe_bpp : 24) * 16u;
}

uint64_t intel_dsc_link_data_rate(const struct intel_pipe *p, uint32_t clock_khz)
{
	uint64_t bits_x16 = (uint64_t)clock_khz * intel_dsc_link_bpp_x16(p);

	if (p->dsc.enable && p->dsc.fec_enable)
		return DIV_ROUND_UP_ULL(bits_x16 * INTEL_DSC_FEC_OVERHEAD_PPM, 16ull * 1000000);
	return bits_x16 / 16;
}

uint32_t intel_dsc_min_cdclk_khz(const struct intel_dsc_config *cfg, uint32_t pixel_rate_khz,
				 uint32_t htotal)
{
	uint64_t num;
	uint32_t rate;
	uint32_t engines;

	if (!cfg->enable || !htotal)
		return 0;
	/* every slice costs 14 pixel clocks of bubbles a line */
	num = (uint64_t)pixel_rate_khz * (htotal + 14u * cfg->slice_count);
	rate = (uint32_t)DIV_ROUND_UP_ULL(num, htotal);
	/* an engine takes one pixel a clock */
	engines = cfg->num_vdsc_instances ? cfg->num_vdsc_instances : 1;
	return DIV_ROUND_UP(rate, engines);
}

/* ---- the register values of a parameter set ---------------------------------- */

void intel_vdsc_regs_pack(const struct drm_dsc_config *c, int num_vdsc_instances, int display_ver,
			  struct intel_vdsc_regs *r)
{
	int n = num_vdsc_instances > 0 ? num_vdsc_instances : 1;
	uint32_t v;
	int i;

	mm_memset(r, 0, sizeof(*r));

	/* PPS 0 */
	v = DSC_PPS0_VER_MAJOR(1) | DSC_PPS0_VER_MINOR(c->dsc_version_minor) |
	    DSC_PPS0_BPC(c->bits_per_component) | DSC_PPS0_LINE_BUF_DEPTH(c->line_buf_depth);
	if (c->dsc_version_minor == 2) {
		v |= DSC_PPS0_ALT_ICH_SEL;
		if (c->native_420)
			v |= DSC_PPS0_NATIVE_420_ENABLE;
		if (c->native_422)
			v |= DSC_PPS0_NATIVE_422_ENABLE;
	}
	if (c->block_pred_enable)
		v |= DSC_PPS0_BLOCK_PREDICTION;
	if (c->convert_rgb)
		v |= DSC_PPS0_COLOR_SPACE_CONVERSION;
	if (c->simple_422)
		v |= DSC_PPS0_422_ENABLE;
	if (c->vbr_enable)
		v |= DSC_PPS0_VBR_ENABLE;
	r->pps[0] = v;

	/* PPS 1 */
	r->pps[1] = DSC_PPS1_BPP(c->bits_per_pixel);

	/* PPS 2: each engine's share of the line */
	r->pps[2] = DSC_PPS2_PIC_HEIGHT(c->pic_height) | DSC_PPS2_PIC_WIDTH(c->pic_width / n);

	/* PPS 3 */
	r->pps[3] = DSC_PPS3_SLICE_HEIGHT(c->slice_height) | DSC_PPS3_SLICE_WIDTH(c->slice_width);

	/* PPS 4 */
	r->pps[4] = DSC_PPS4_INITIAL_XMIT_DELAY(c->initial_xmit_delay) |
		    DSC_PPS4_INITIAL_DEC_DELAY(c->initial_dec_delay);

	/* PPS 5 */
	r->pps[5] = DSC_PPS5_SCALE_INC_INT(c->scale_increment_interval) |
		    DSC_PPS5_SCALE_DEC_INT(c->scale_decrement_interval);

	/* PPS 6 */
	r->pps[6] = DSC_PPS6_INITIAL_SCALE_VALUE(c->initial_scale_value) |
		    DSC_PPS6_FIRST_LINE_BPG_OFFSET(c->first_line_bpg_offset) |
		    DSC_PPS6_FLATNESS_MIN_QP(c->flatness_min_qp) |
		    DSC_PPS6_FLATNESS_MAX_QP(c->flatness_max_qp);

	/* PPS 7 */
	r->pps[7] = DSC_PPS7_SLICE_BPG_OFFSET(c->slice_bpg_offset) |
		    DSC_PPS7_NFL_BPG_OFFSET(c->nfl_bpg_offset);

	/* PPS 8 */
	r->pps[8] = DSC_PPS8_FINAL_OFFSET(c->final_offset) |
		    DSC_PPS8_INITIAL_OFFSET(c->initial_offset);

	/* PPS 9 */
	r->pps[9] = DSC_PPS9_RC_MODEL_SIZE(c->rc_model_size) |
		    DSC_PPS9_RC_EDGE_FACTOR(DSC_RC_EDGE_FACTOR_CONST);

	/* PPS 10 */
	r->pps[10] = DSC_PPS10_RC_QUANT_INC_LIMIT0(c->rc_quant_incr_limit0) |
		     DSC_PPS10_RC_QUANT_INC_LIMIT1(c->rc_quant_incr_limit1) |
		     DSC_PPS10_RC_TARGET_OFF_HIGH(DSC_RC_TGT_OFFSET_HI_CONST) |
		     DSC_PPS10_RC_TARGET_OFF_LOW(DSC_RC_TGT_OFFSET_LO_CONST);

	/* PPS 16 */
	r->pps[16] = DSC_PPS16_SLICE_CHUNK_SIZE(c->slice_chunk_size) |
		     DSC_PPS16_SLICE_PER_LINE(c->slice_width ?
					      (c->pic_width / n) / c->slice_width : 0) |
		     DSC_PPS16_SLICE_ROW_PER_FRAME(c->slice_height ?
						   c->pic_height / c->slice_height : 0);

	if (display_ver >= 14) {
		/* PPS 17 */
		r->pps[17] = DSC_PPS17_SL_BPG_OFFSET(c->second_line_bpg_offset);
		/* PPS 18 */
		r->pps[18] = DSC_PPS18_NSL_BPG_OFFSET(c->nsl_bpg_offset) |
			     DSC_PPS18_SL_OFFSET_ADJ(c->second_line_offset_adj);
	}

	/* the 14 buffer thresholds, a byte each, four to a register */
	for (i = 0; i < DSC_NUM_BUF_RANGES - 1; i++)
		r->rc_buf_thresh[i / 4] |= (uint32_t)(c->rc_buf_thresh[i] & 0xff) << (8 * (i % 4));

	/* the 15 ranges, 16 bits each, two to a register */
	for (i = 0; i < DSC_NUM_BUF_RANGES; i++)
		r->rc_range[i / 2] |=
			(uint32_t)(((uint32_t)c->rc_range_params[i].range_bpg_offset << RC_BPG_OFFSET_SHIFT) |
				   ((uint32_t)c->rc_range_params[i].range_max_qp << RC_MAX_QP_SHIFT) |
				   ((uint32_t)c->rc_range_params[i].range_min_qp << RC_MIN_QP_SHIFT))
			<< (16 * (i % 2));
}

#if I915_FEAT_DSC

/* ---- where a pipe's engines are ------------------------------------------------- */

/* The pipe's own engines, or (version 11, embedded-panel transcoder) the
 * transcoder-level pair. */
static bool is_pipe_dsc(struct i915_device *i915, const struct intel_pipe *p)
{
	if (i915->info->display_ver >= 12)
		return true;
	if (p->transcoder == TRANSCODER_EDP)
		return false;
	return true;
}

/* Has the pipe an engine at all?  Version 11's pipe A has none. */
static bool pipe_has_dsc(struct i915_device *i915, const struct intel_pipe *p)
{
	return !(i915->info->display_ver == 11 && p->transcoder != TRANSCODER_EDP &&
		 p->pipe == PIPE_A);
}

static uint32_t dss_ctl1_reg(struct i915_device *i915, const struct intel_pipe *p)
{
	return is_pipe_dsc(i915, p) ? ICL_PIPE_DSS_CTL1(p->pipe) : DSS_CTL1;
}

static uint32_t dss_ctl2_reg(struct i915_device *i915, const struct intel_pipe *p)
{
	return is_pipe_dsc(i915, p) ? ICL_PIPE_DSS_CTL2(p->pipe) : DSS_CTL2;
}

/* The power domain the engines need on top of the pipe's own (which the
 * pipe holds while it runs), or -1 when the pipe's suffices. */
static int dsc_power_domain(struct i915_device *i915, const struct intel_pipe *p)
{
	if (i915->info->display_ver == 12 && i915->info->platform != I915_PLATFORM_ROCKETLAKE &&
	    p->pipe == PIPE_A)
		return DSC_PW2_DOMAIN;
	if (is_pipe_dsc(i915, p))
		return -1;
	return DSC_PW2_DOMAIN;
}

/* Can the engines' registers be read now?  Those in power well 2 only
 * while it is up (a read there otherwise is a fault the hardware logs). */
static bool dsc_regs_powered(struct i915_device *i915, const struct intel_pipe *p)
{
	if (dsc_power_domain(i915, p) < 0)
		return true;
	return (i915_read32(i915, HSW_PWR_WELL_CTL2) &
		HSW_PWR_WELL_CTL_STATE(ICL_PW_CTL_IDX_PW_2)) != 0;
}

/* ---- the sink -------------------------------------------------------------------- */

static int sink_set_flag(struct intel_output *o, uint32_t reg, uint8_t flag, bool set)
{
	uint8_t v;

	if (intel_dp_dpcd_read8(&o->aux, reg, &v))
		return -EIO;
	v = set ? (uint8_t)(v | flag) : (uint8_t)(v & ~flag);
	return intel_dp_dpcd_write8(&o->aux, reg, v) ? -EIO : 0;
}

void intel_dsc_pre_link_train(struct i915_device *i915, struct intel_output *o,
			      struct intel_pipe *p)
{
	struct intel_dsc_config *c = &p->dsc;

	if (o->type != INTEL_OUTPUT_DP && o->type != INTEL_OUTPUT_EDP)
		return;
	if (!c->enable) {
		/* A sink the firmware left decompressing would take the
		 * uncompressed stream for a compressed one. */
		uint8_t v;

		if (!o->dsc_caps.valid || !drm_dp_sink_supports_dsc(o->dsc_caps.dsc_dpcd))
			return;
		if (intel_dp_dpcd_read8(&o->aux, DP_DSC_ENABLE, &v) == 0 &&
		    (v & DP_DECOMPRESSION_EN)) {
			i915_dbg("[drm] i915: DP port %c: the sink was left decompressing; switching that off\n",
				 'A' + o->port);
			(void)intel_dp_dpcd_write8(&o->aux, DP_DSC_ENABLE,
						   (uint8_t)(v & ~DP_DECOMPRESSION_EN));
		}
		return;
	}
	(void)i915;
	if (!c->sink_decompression) {
		if (sink_set_flag(o, DP_DSC_ENABLE, DP_DECOMPRESSION_EN, true) == 0)
			c->sink_decompression = 1;
		else
			kprintf("[drm] i915: DP port %c: the sink did not take the decompression switch\n",
				'A' + o->port);
	}
	/* "Anticipates enabling FEC encoding": FEC_READY before the link
	 * is trained, and the detected flags cleared for the check after. */
	if (c->fec_enable) {
		if (intel_dp_dpcd_write8(&o->aux, DP_FEC_CONFIGURATION, DP_FEC_READY))
			kprintf("[drm] i915: DP port %c: the sink did not take FEC_READY\n",
				'A' + o->port);
		(void)intel_dp_dpcd_write8(&o->aux, DP_FEC_STATUS,
					   DP_FEC_DECODE_EN_DETECTED | DP_FEC_DECODE_DIS_DETECTED);
	}
}

/* ---- FEC on the link ----------------------------------------------------------------- */

/* The port reports FEC running (or stopped) within 1 ms; on the way up
 * the sink also says it saw the decode-enable sequence, within 200 ms. */
static int fec_wait_status(struct i915_device *i915, struct intel_output *o, bool enabled)
{
	uint32_t status = intel_dp_tp_status_reg(i915, o);
	int t;

	for (t = 0; t < 100; t++) {
		bool live = (i915_read32(i915, status) & DP_TP_STATUS_FEC_ENABLE_LIVE) != 0;

		if (live == enabled)
			break;
		lapic_delay_us(10);
	}
	if (t == 100)
		return -ETIMEDOUT;
	/* The detected flag for the disable is not set by every sink. */
	if (!enabled)
		return 0;
	for (t = 0; t < 20; t++) {
		uint8_t st;

		if (intel_dp_dpcd_read8(&o->aux, DP_FEC_STATUS, &st))
			return -EIO;
		if (st & DP_FEC_DECODE_EN_DETECTED)
			return 0;
		lapic_delay_ms(10);
	}
	return -ETIMEDOUT;
}

static void fec_set(struct i915_device *i915, struct intel_output *o, bool on)
{
	uint32_t reg = intel_dp_tp_ctl_reg(i915, o);
	uint32_t v = i915_read32(i915, reg);

	v = on ? (v | DP_TP_CTL_FEC_ENABLE) : (v & ~DP_TP_CTL_FEC_ENABLE);
	i915_write32(i915, reg, v);
	(void)i915_read32(i915, reg);
}

void intel_dsc_fec_enable(struct i915_device *i915, struct intel_output *o,
			  struct intel_pipe *p)
{
	if (!p->dsc.enable || !p->dsc.fec_enable)
		return;
	fec_set(i915, o, true);
	/* Display version 30 and later are checked, and retried. */
	if (i915->info->display_ver < 30)
		return;
	if (fec_wait_status(i915, o, true) == 0)
		return;
	for (int i = 0; i < 3; i++) {
		fec_set(i915, o, false);
		if (fec_wait_status(i915, o, false))
			continue;
		fec_set(i915, o, true);
		if (fec_wait_status(i915, o, true) == 0)
			return;
	}
	kprintf("[drm] i915: DP port %c: FEC did not come up on the link\n", 'A' + o->port);
}

/* ---- the engines --------------------------------------------------------------------- */

static void dsc_write_pps(struct i915_device *i915, const struct intel_pipe *p, bool pipe_dsc,
			  int engines, int pps, uint32_t v)
{
	if (pipe_dsc) {
		i915_write32(i915, ICL_DSC0_PPS(p->pipe, pps), v);
		if (engines > 1)
			i915_write32(i915, ICL_DSC1_PPS(p->pipe, pps), v);
	} else {
		i915_write32(i915, DSCA_PPS(pps), v);
		if (engines > 1)
			i915_write32(i915, DSCC_PPS(pps), v);
	}
}

static void dsc_engines_program(struct i915_device *i915, const struct intel_pipe *p,
				const struct intel_vdsc_regs *r)
{
	bool pipe_dsc = is_pipe_dsc(i915, p);
	int engines = p->dsc.num_vdsc_instances > 1 ? 2 : 1;
	int last = i915->info->display_ver >= 14 ? 18 : 16;
	uint32_t thresh0, thresh1, range0;
	int i;

	for (i = 0; i <= last; i++) {
		if (i > 10 && i < 16)
			continue;
		dsc_write_pps(i915, p, pipe_dsc, engines, i, r->pps[i]);
	}

	for (int e = 0; e < engines; e++) {
		if (pipe_dsc) {
			thresh0 = e ? ICL_DSC1_RC_BUF_THRESH_0(p->pipe) : ICL_DSC0_RC_BUF_THRESH_0(p->pipe);
			thresh1 = e ? ICL_DSC1_RC_BUF_THRESH_1(p->pipe) : ICL_DSC0_RC_BUF_THRESH_1(p->pipe);
			range0 = e ? ICL_DSC1_RC_RANGE_PARAMETERS_0(p->pipe) :
				     ICL_DSC0_RC_RANGE_PARAMETERS_0(p->pipe);
		} else {
			thresh0 = e ? DSCC_RC_BUF_THRESH_0 : DSCA_RC_BUF_THRESH_0;
			thresh1 = e ? DSCC_RC_BUF_THRESH_1 : DSCA_RC_BUF_THRESH_1;
			range0 = e ? DSCC_RC_RANGE_PARAMETERS_0 : DSCA_RC_RANGE_PARAMETERS_0;
		}
		i915_write32(i915, thresh0, r->rc_buf_thresh[0]);
		i915_write32(i915, thresh0 + 4, r->rc_buf_thresh[1]);
		i915_write32(i915, thresh1, r->rc_buf_thresh[2]);
		i915_write32(i915, thresh1 + 4, r->rc_buf_thresh[3]);
		for (i = 0; i < 8; i++)
			i915_write32(i915, DSC_RC_RANGE_PARAMETERS_N(range0, i), r->rc_range[i]);
	}
}

/* The parameter set to the sink: a secondary data packet the
 * transcoder's video DIP sends every frame. */
static void dsc_pps_sdp_write(struct i915_device *i915, const struct intel_pipe *p,
			      const struct drm_dsc_config *c, struct drm_dsc_pps_infoframe *sdp)
{
	int t = p->transcoder;
	uint32_t ctl = i915_read32(i915, HSW_TVIDEO_DIP_CTL(t));
	const uint8_t *b = (const uint8_t *)sdp;
	unsigned i;

	drm_dsc_dp_pps_header_init(&sdp->pps_header);
	drm_dsc_pps_payload_pack(&sdp->pps_payload, c);

	ctl &= ~VDIP_ENABLE_PPS;
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(t), ctl);
	for (i = 0; i + 4 <= sizeof(*sdp); i += 4)
		i915_write32(i915, ICL_VIDEO_DIP_PPS_DATA(t, i / 4),
			     (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) |
				     ((uint32_t)b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24));
	/* every data byte written, for the packet's ECC */
	for (; i < VIDEO_DIP_PPS_DATA_SIZE; i += 4)
		i915_write32(i915, ICL_VIDEO_DIP_PPS_DATA(t, i / 4), 0);
	ctl |= VDIP_ENABLE_PPS;
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(t), ctl);
	(void)i915_read32(i915, HSW_TVIDEO_DIP_CTL(t));
}

void intel_dsc_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		      const struct drm_mode_modeinfo *m)
{
	struct intel_dsc_config *c = &p->dsc;
	struct dsc_scratch {
		struct drm_dsc_config cfg;
		struct drm_dsc_pps_infoframe sdp;
		struct intel_vdsc_regs regs;
	} *s;
	uint32_t ctl1 = 0, ctl2 = 0;
	int d, rc;

	if (!c->enable)
		return;
	d = dsc_power_domain(i915, p);
	if (d >= 0 && !c->power_ref) {
		intel_power_get(i915, (enum intel_power_domain)d);
		c->power_ref = (uint8_t)(d + 1);
	}

	intel_dsc_fec_enable(i915, o, p);

	s = kalloc(sizeof(*s));
	if (!s) {
		kprintf("[drm] i915: pipe %c: no memory for the compression set-up\n", 'A' + p->pipe);
		return;
	}
	rc = intel_dp_dsc_params(i915, o, c, m->hdisplay, m->vdisplay, &s->cfg);
	if (rc) {
		/* The same inputs gave a valid set at the mode check. */
		kprintf("[drm] i915: pipe %c: the compression parameters no longer compute (%d)\n",
			'A' + p->pipe, rc);
		kfree(s);
		return;
	}

	dsc_pps_sdp_write(i915, p, &s->cfg, &s->sdp);

	intel_vdsc_regs_pack(&s->cfg, c->num_vdsc_instances, i915->info->display_ver, &s->regs);
	dsc_engines_program(i915, p, &s->regs);

	ctl2 |= VDSC0_ENABLE;
	if (c->num_vdsc_instances > 1) {
		/* the line's two halves through the small joiner */
		ctl2 |= VDSC1_ENABLE;
		ctl1 |= JOINER_ENABLE;
	}
	i915_write32(i915, dss_ctl1_reg(i915, p), ctl1);
	i915_write32(i915, dss_ctl2_reg(i915, p), ctl2);
	(void)i915_read32(i915, dss_ctl2_reg(i915, p));
	kfree(s);
}

void intel_dsc_disable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p)
{
	struct intel_dsc_config *c = &p->dsc;
	bool engine_on = c->enable;

	/* An engine running that this driver did not start: the firmware
	 * compressed the stream it left behind. */
	if (!engine_on) {
		intel_dp_dsc_source_init(i915);
		if (i915->display.dsc_src.has_dsc && pipe_has_dsc(i915, p) &&
		    dsc_regs_powered(i915, p) &&
		    (i915_read32(i915, dss_ctl2_reg(i915, p)) & VDSC0_ENABLE)) {
			i915_dbg("[drm] i915: pipe %c: switching off the firmware's stream compression\n",
				 'A' + p->pipe);
			engine_on = true;
		}
	}
	if (engine_on) {
		i915_write32(i915, dss_ctl1_reg(i915, p), 0);
		i915_write32(i915, dss_ctl2_reg(i915, p), 0);
		(void)i915_read32(i915, dss_ctl2_reg(i915, p));
		/* and the parameter set no longer sent */
		uint32_t ctl = i915_read32(i915, HSW_TVIDEO_DIP_CTL(p->transcoder));
		if (ctl & VDIP_ENABLE_PPS)
			i915_write32(i915, HSW_TVIDEO_DIP_CTL(p->transcoder), ctl & ~VDIP_ENABLE_PPS);
	}
	if (c->enable && c->fec_enable) {
		/* FEC off on the (idle) link before the port goes down, then
		 * the sink no longer expects it. */
		fec_set(i915, o, false);
		if (fec_wait_status(i915, o, false))
			i915_dbg("[drm] i915: DP port %c: FEC still reported running\n", 'A' + o->port);
		(void)intel_dp_dpcd_write8(&o->aux, DP_FEC_CONFIGURATION, 0);
	}
	if (c->sink_decompression)
		(void)sink_set_flag(o, DP_DSC_ENABLE, DP_DECOMPRESSION_EN, false);
	if (c->power_ref)
		intel_power_put(i915, (enum intel_power_domain)(c->power_ref - 1));
	mm_memset(c, 0, sizeof(*c));
}

/* ---- the choice ---------------------------------------------------------------------- */

/* Would the stream leave through transcoder A (version 11 has no engine
 * there)?  At enable time the pipe is known; at the check the pipe the
 * output gets is predicted as the enable path picks it: its own (the
 * crtc's index is the connector's) unless it runs on another already. */
static bool dsc_transcoder_is_a(struct i915_device *i915, const struct intel_output *o)
{
	int pipe;

	if (i915->info->display_ver != 11)
		return false;
	/* port A goes out through the embedded-panel transcoder there */
	if (o->port == PORT_A)
		return false;
	pipe = o->pipe >= 0 ? o->pipe : o->conn;
	return pipe == PIPE_A;
}

/* What the enable path chose, said once per output and choice. */
static uint32_t g_dsc_logged[INTEL_MAX_OUTPUTS];

static void dsc_log_choice(struct i915_device *i915, const struct intel_output *o,
			   const struct drm_mode_modeinfo *m, const struct intel_dsc_config *c,
			   int rc)
{
	int idx = (int)(o - i915->display.outputs);
	uint32_t digest = 2166136261u;
	const uint8_t *b = (const uint8_t *)c;

	for (unsigned i = 0; i < sizeof(*c); i++)
		digest = (digest ^ b[i]) * 16777619u;
	digest = (digest ^ ((uint32_t)m->hdisplay << 16 | m->vdisplay)) * 16777619u;
	digest = (digest ^ (uint32_t)rc) * 16777619u;
	if (idx < 0 || idx >= INTEL_MAX_OUTPUTS || g_dsc_logged[idx] == digest)
		return;
	g_dsc_logged[idx] = digest;
	if (rc) {
		kprintf("[drm] i915: DP port %c: %ux%u (%u kHz) cannot be compressed onto the link (%d)\n",
			'A' + o->port, m->hdisplay, m->vdisplay, m->clock, rc);
		return;
	}
	kprintf("[drm] i915: DP port %c: %ux%u compressed: %u bpp in, %u.%04u bpp on the link, %u slice%s, %u kHz x%u%s\n",
		'A' + o->port, m->hdisplay, m->vdisplay, c->pipe_bpp, c->compressed_bpp_x16 >> 4,
		(c->compressed_bpp_x16 & 15) * 625u, c->slice_count, c->slice_count == 1 ? "" : "s",
		c->link_rate_khz, c->lane_count, c->fec_enable ? ", FEC" : "");
}

int intel_dsc_compute_config(struct i915_device *i915, struct intel_output *o,
			     const struct drm_mode_modeinfo *m, struct intel_dsc_config *cfg)
{
	bool enable_ctx;
	int rc;

	mm_memset(cfg, 0, sizeof(*cfg));
	if (o->type != INTEL_OUTPUT_DP && o->type != INTEL_OUTPUT_EDP)
		return -EINVAL;
	/* the uncompressed link carries it: nothing to do */
	if (intel_dp_link_carries(i915, o, m))
		return 0;
	enable_ctx = o->pipe >= 0 && o->pipe < INTEL_MAX_PIPES &&
		     cfg == &i915->display.pipes[o->pipe].dsc;
	rc = intel_dp_dsc_compute(i915, o, m, cfg, dsc_transcoder_is_a(i915, o));
	if (rc)
		mm_memset(cfg, 0, sizeof(*cfg));
	if (enable_ctx)
		dsc_log_choice(i915, o, m, cfg, rc);
	return rc;
}

#else /* !I915_FEAT_DSC */

int intel_dsc_compute_config(struct i915_device *i915, struct intel_output *o,
			     const struct drm_mode_modeinfo *m, struct intel_dsc_config *cfg)
{
	(void)i915;
	(void)o;
	(void)m;
	mm_memset(cfg, 0, sizeof(*cfg));
	return -EINVAL;
}

void intel_dsc_pre_link_train(struct i915_device *i915, struct intel_output *o,
			      struct intel_pipe *p)
{
	(void)i915;
	(void)o;
	(void)p;
}

void intel_dsc_fec_enable(struct i915_device *i915, struct intel_output *o,
			  struct intel_pipe *p)
{
	(void)i915;
	(void)o;
	(void)p;
}

void intel_dsc_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		      const struct drm_mode_modeinfo *m)
{
	(void)i915;
	(void)o;
	(void)p;
	(void)m;
}

void intel_dsc_disable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p)
{
	(void)i915;
	(void)o;
	mm_memset(&p->dsc, 0, sizeof(p->dsc));
}

#endif /* I915_FEAT_DSC */
