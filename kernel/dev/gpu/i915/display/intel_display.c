// LikeOS -- pipes, transcoders and planes of Skylake, and the DRM
// backend built on them.
//
// A mode set for an output: its power wells up, the panel powered, a PLL
// found and the port clocked from it, the link trained, the transcoder
// programmed with the timings and attached to the port, the pipe turned
// on, the primary plane pointed at the framebuffer with watermarks that
// keep it fed, and finally the backlight.  Teardown is the reverse.  The
// DRM core calls in through the drm_driver entry points at the bottom.
//
// The features of the pipe have files of their own, called from here at
// fixed points: the planes (intel_plane.c), colour (intel_color.c),
// vblank (intel_vblank.c), variable refresh (intel_vrr.c), HDMI
// scrambling (intel_hdmi_feat.c), stream compression (intel_vdsc.c) and
// the connectors' probing (intel_connector.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2012 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/intel_snps_phy.h>
#include <kernel/dev/gpu/i915/intel_snps_phy_regs.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/intel_plane.h>
#include <kernel/dev/gpu/i915/intel_color.h>
#include <kernel/dev/gpu/i915/intel_vblank.h>
#include <kernel/dev/gpu/i915/intel_vrr.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/i915/intel_dsc.h>
#include <kernel/dev/gpu/i915/intel_connector.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>

/* From Alder Lake-P the pipe's vblank start is the transcoder's "set
 * context latency" past the active area (TRANS_VBLANK's start is
 * ignored); from display version 30 even a fixed refresh rate runs
 * through the variable-refresh timing generator, with its minimum,
 * maximum and flip line all at the vertical total (intel_vrr.c). */
#ifndef TRANS_SET_CONTEXT_LATENCY
#define TRANS_SET_CONTEXT_LATENCY(t) (TRANS_BASE(t) + 0x07c)
#endif

/* Lines between the end of the active area and the pipe's vblank. */
uint32_t intel_set_context_latency(struct i915_device *i915)
{
	/* at least one from display 30, or the safe window never opens */
	return i915->info->display_ver >= 30 ? 1 : 0;
}

struct intel_output *intel_output_for_crtc(struct i915_device *i915, int crtc)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < d->nout; i++)
		if (d->outputs[i].conn == crtc)
			return &d->outputs[i];
	return NULL;
}

struct intel_output *intel_output_for_conn(struct i915_device *i915,
					   struct drm_connector *c)
{
	int idx = (int)(c - i915->drm.conn);
	return intel_output_for_crtc(i915, idx);
}

/* Tiger Lake and later: the DisplayPort transport controls are the
 * transcoder's, the transcoder names its port in a four-bit field, and
 * there is no embedded-panel transcoder.  Meteor Lake keeps all of it. */
static int tgl_plus(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_TGL || i915->display.model == INTEL_DISPLAY_MTL ||
	       i915->display.model == INTEL_DISPLAY_DG2;
}

static enum intel_power_domain port_domain(int port)
{
	return (enum intel_power_domain)(INTEL_PW_DDI_A + port);
}

/* Skylake's DDI E shares AUX A; everywhere else a port has its own. */
static enum intel_power_domain aux_domain(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_SKL && port > 3)
		return INTEL_PW_AUX_A;
	return (enum intel_power_domain)(INTEL_PW_AUX_A + port);
}

const char *intel_port_name(struct i915_device *i915, int port)
{
	static const char *const ddi[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I" };
	static const char *const tc[] = { "TC1", "TC2", "TC3", "TC4", "TC5", "TC6" };
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		/* DG2's fourth and fifth ports sit at unusual indices */
		if (port == DG2_PORT_D_XELPD)
			return "D";
		if (port == DG2_PORT_TC1)
			return "TC1";
	}
	if (tgl_plus(i915) &&
	    ((i915->info->flags & I915_INFO_HAS_TC_PHY) ||
	     (i915->display.model == INTEL_DISPLAY_MTL && (i915->info->flags & I915_INFO_IS_DGFX))) &&
	    port >= PORT_TC1 && port - PORT_TC1 < 6)
		return tc[port - PORT_TC1];
	if (i915->display.model == INTEL_DISPLAY_ICL && port >= PORT_C && port - PORT_C < 4 &&
	    (i915->info->flags & I915_INFO_HAS_TC_PHY))
		return tc[port - PORT_C];
	return port >= 0 && port < 9 ? ddi[port] : "?";
}

/* Tiger Lake moved the DisplayPort transport control from the port to
 * the transcoder; the port must already have its pipe. */
uint32_t intel_dp_tp_ctl_reg(struct i915_device *i915, const struct intel_output *o)
{
	if (tgl_plus(i915)) {
		int t = o->pipe >= 0 ? i915->display.pipes[o->pipe].transcoder : 0;
		return TGL_DP_TP_CTL(t);
	}
	return DP_TP_CTL(o->port);
}

uint32_t intel_dp_tp_status_reg(struct i915_device *i915, const struct intel_output *o)
{
	if (tgl_plus(i915)) {
		int t = o->pipe >= 0 ? i915->display.pipes[o->pipe].transcoder : 0;
		return TGL_DP_TP_STATUS(t);
	}
	return DP_TP_STATUS(o->port);
}

/* The transcoder's port field and clock select, which Tiger Lake
 * re-encoded (four bits, port + 1). */
static uint32_t trans_ddi_select_port(struct i915_device *i915, int port)
{
	if (tgl_plus(i915))
		return TGL_TRANS_DDI_SELECT_PORT((uint32_t)port);
	return TRANS_DDI_SELECT_PORT((uint32_t)port);
}

static uint32_t trans_ddi_port_mask(struct i915_device *i915)
{
	return tgl_plus(i915) ? TGL_TRANS_DDI_PORT_MASK : TRANS_DDI_PORT_MASK;
}

static int trans_ddi_port_of(struct i915_device *i915, uint32_t func)
{
	if (tgl_plus(i915))
		return (int)((func & TGL_TRANS_DDI_PORT_MASK) >> TGL_TRANS_DDI_PORT_SHIFT) - 1;
	return (int)((func & TRANS_DDI_PORT_MASK) >> TRANS_DDI_PORT_SHIFT);
}

static uint32_t trans_clk_sel_port(struct i915_device *i915, int port)
{
	if (tgl_plus(i915))
		return TGL_TRANS_CLK_SEL_PORT((uint32_t)port);
	return TRANS_CLK_SEL_PORT((uint32_t)port);
}

/* Port A's transcoder: the embedded-panel one where there is one. */
static int transcoder_for(struct i915_device *i915, int port, int pipe)
{
	if (port == PORT_A && !tgl_plus(i915))
		return TRANSCODER_EDP;
	return pipe;
}

/* ---- the transcoder ----------------------------------------------------------- */

/* DisplayPort M/N: the ratio of pixel data to link symbols, reduced to
 * fit the 24-bit registers with the transfer unit fixed at 64. */
static void compute_m_n(uint32_t m_in, uint32_t n_in, uint32_t *m_out,
			uint32_t *n_out)
{
	uint64_t n = 0x800000; /* the largest N that keeps precision */
	uint64_t nn = n_in;
	/* round n_in up to a power of two, capped */
	uint64_t p = 1;
	while (p < nn)
		p <<= 1;
	if (p < n)
		n = p;
	uint64_t m = ((uint64_t)m_in * n + n_in - 1) / n_in;
	while (m > 0xffffff || n > 0xffffff) {
		m >>= 1;
		n >>= 1;
	}
	*m_out = (uint32_t)m;
	*n_out = (uint32_t)n;
}

/* The pipe's bits per pixel: 24 (8 bits a component) unless the stream
 * compression set-up chose another depth for the pipe (p->dsc.pipe_bpp),
 * in the fields of the transcoder's DDI function, the DisplayPort MSA and
 * PIPE_MISC. */
#ifndef TRANS_MSA_12_BPC
#define TRANS_MSA_12_BPC (3 << 5)
#endif

static uint32_t pipe_bpp(const struct intel_pipe *p)
{
	return p->dsc.pipe_bpp ? p->dsc.pipe_bpp : 24;
}

static uint32_t trans_ddi_bpc(uint32_t bpp)
{
	switch (bpp) {
	case 18: return TRANS_DDI_BPC_6;
	case 30: return TRANS_DDI_BPC_10;
	case 36: return TRANS_DDI_BPC_12;
	default: return TRANS_DDI_BPC_8;
	}
}

static uint32_t trans_msa_bpc(uint32_t bpp)
{
	switch (bpp) {
	case 18: return TRANS_MSA_6_BPC;
	case 30: return TRANS_MSA_10_BPC;
	case 36: return TRANS_MSA_12_BPC;
	default: return TRANS_MSA_8_BPC;
	}
}

static uint32_t pipe_misc_bpc(uint32_t bpp)
{
	switch (bpp) {
	case 18: return PIPE_MISC_BPC_6;
	case 30: return PIPE_MISC_BPC_10;
	case 36: return PIPE_MISC_BPC_12;
	default: return PIPE_MISC_BPC_8;
	}
}

/* An HDMI port's mode and width in the DDI function: DVI mode when the
 * sink's EDID is not an HDMI one -- such a sink knows nothing of the data
 * islands HDMI mode puts in the blanking (the infoframes are left out
 * for it too) -- and, from display version 14, the four TMDS lanes in
 * the width field, which those transcoders read for HDMI as well. */
static uint32_t trans_ddi_hdmi_mode(struct i915_device *i915, const struct intel_output *o)
{
	uint32_t v;

	if (!I915_FEAT_HDMI_DDI_FUNC)
		return TRANS_DDI_MODE_SELECT_HDMI;
	v = o->hdmi_sink ? TRANS_DDI_MODE_SELECT_HDMI : TRANS_DDI_MODE_SELECT_DVI;
	if (i915->info->display_ver >= 14)
		v |= TRANS_DDI_PORT_WIDTH(4u);
	return v;
}

/* The DDI function of the transcoder: its port, mode, width and sync
 * polarities -- everything but the enable bit. */
static uint32_t trans_ddi_func_value(struct i915_device *i915, struct intel_output *o,
				     struct intel_pipe *p, const struct drm_mode_modeinfo *m)
{
	int t = p->transcoder;
	uint32_t hsync_pol = (m->flags & DRM_MODE_FLAG_PHSYNC) ? TRANS_DDI_PHSYNC : 0;
	uint32_t vsync_pol = (m->flags & DRM_MODE_FLAG_PVSYNC) ? TRANS_DDI_PVSYNC : 0;
	uint32_t func = trans_ddi_select_port(i915, o->port) | trans_ddi_bpc(pipe_bpp(p)) |
			hsync_pol | vsync_pol;
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		func |= TRANS_DDI_MODE_SELECT_DP_SST |
			TRANS_DDI_PORT_WIDTH((uint32_t)o->lane_count);
	else if (o->type == INTEL_OUTPUT_HDMI)
		func |= trans_ddi_hdmi_mode(i915, o) | intel_hdmi_trans_ddi_bits(i915, o, p, m);
	else
		func |= TRANS_DDI_MODE_SELECT_DVI;
	if (t == TRANSCODER_EDP) {
		switch (p->pipe) {
		case PIPE_A:
			/* Pipe A feeds this transcoder and is switched on with
			 * it.  The other setting ("on/off") leaves the pipe
			 * under its own transcoder's control, which nothing
			 * here enables: the embedded transcoder then reports
			 * itself running while the pipe never scans a line
			 * and the panel stays dark. */
			func |= TRANS_DDI_EDP_INPUT_A_ON;
			break;
		case PIPE_B:
			func |= TRANS_DDI_EDP_INPUT_B_ONOFF;
			break;
		default:
			func |= TRANS_DDI_EDP_INPUT_C_ONOFF;
			break;
		}
	}
	return func;
}

/* Tiger Lake and later train the link through the transcoder's own
 * transport control, so the transcoder must already be clocked from the
 * port and name it -- with its function still disabled -- before the
 * first training pattern goes out.  Earlier parts do this after. */
static void transcoder_route_port(struct i915_device *i915, struct intel_output *o,
				  struct intel_pipe *p, const struct drm_mode_modeinfo *m)
{
	int t = p->transcoder;
	if (!tgl_plus(i915))
		return;
	i915_write32(i915, TRANS_CLK_SEL(t), trans_clk_sel_port(i915, o->port));
	i915_write32(i915, TRANS_DDI_FUNC_CTL(t), trans_ddi_func_value(i915, o, p, m));
	(void)i915_read32(i915, TRANS_DDI_FUNC_CTL(t));
}

static void transcoder_program(struct i915_device *i915, struct intel_output *o,
			       struct intel_pipe *p, const struct drm_mode_modeinfo *m,
			       uint32_t src_w, uint32_t src_h)
{
	int t = p->transcoder;

	i915_write32(i915, TRANS_HTOTAL(t), ((m->htotal - 1) << 16) | (m->hdisplay - 1));
	i915_write32(i915, TRANS_HBLANK(t), ((m->htotal - 1) << 16) | (m->hdisplay - 1));
	i915_write32(i915, TRANS_HSYNC(t), ((m->hsync_end - 1) << 16) | (m->hsync_start - 1));
	i915_write32(i915, TRANS_VTOTAL(t), ((m->vtotal - 1) << 16) | (m->vdisplay - 1));
	i915_write32(i915, TRANS_VBLANK(t), ((m->vtotal - 1) << 16) | (m->vdisplay - 1));
	i915_write32(i915, TRANS_VSYNC(t), ((m->vsync_end - 1) << 16) | (m->vsync_start - 1));
	i915_write32(i915, TRANS_VSYNCSHIFT(t), 0);
	i915_write32(i915, TRANS_MULT(t), 0);
	if (i915->info->display_ver >= 13)
		i915_write32(i915, TRANS_SET_CONTEXT_LATENCY(t), intel_set_context_latency(i915));
	/* The pipe's picture is the client's size; the scaler stretches it
	 * onto the timing when the two differ. */
	i915_write32(i915, PIPESRC(p->pipe), ((src_w - 1) << 16) | (src_h - 1));

	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		uint32_t dm, dn, lm, ln;
		/* data: the stream's bits on the link (pixel clock times 24,
		 * or the compressed rate with FEC's share) over link
		 * symbols * lanes */
		compute_m_n((uint32_t)intel_dsc_link_data_rate(p, m->clock),
			    o->link_rate_khz * 8 * (uint32_t)o->lane_count, &dm, &dn);
		compute_m_n(m->clock, o->link_rate_khz, &lm, &ln);
		i915_write32(i915, TRANS_DATA_M1(t), TU_SIZE(64) | dm);
		i915_write32(i915, TRANS_DATA_N1(t), dn);
		i915_write32(i915, TRANS_LINK_M1(t), lm);
		i915_write32(i915, TRANS_LINK_N1(t), ln);
		i915_write32(i915, TRANS_MSA_MISC(t), TRANS_MSA_SYNC_CLK | trans_msa_bpc(pipe_bpp(p)));
	}
	i915_write32(i915, PIPE_MISC(p->pipe), pipe_misc_bpc(pipe_bpp(p)));

	uint32_t func = TRANS_DDI_FUNC_ENABLE | trans_ddi_func_value(i915, o, p, m);
	if (t != TRANSCODER_EDP)
		i915_write32(i915, TRANS_CLK_SEL(t), trans_clk_sel_port(i915, o->port));
	i915_write32(i915, TRANS_DDI_FUNC_CTL(t), func);
}

static int transcoder_enable(struct i915_device *i915, struct intel_pipe *p)
{
	int t = p->transcoder;
	intel_vrr_transcoder_enable(i915, p);
	i915_write32(i915, TRANS_CONF(t), TRANS_CONF_ENABLE | TRANS_CONF_PROGRESSIVE);
	(void)i915_read32(i915, TRANS_CONF(t));
	for (int w = 0; w < 1000; w++) {
		if (i915_read32(i915, TRANS_CONF(t)) & TRANS_CONF_STATE_ENABLE)
			return 0;
		lapic_delay_us(100);
	}
	kprintf("[drm] i915: transcoder %d did not start\n", t);
	return -EIO;
}

static void transcoder_disable(struct i915_device *i915, struct intel_pipe *p)
{
	int t = p->transcoder;
	uint32_t v = i915_read32(i915, TRANS_CONF(t));
	i915_write32(i915, TRANS_CONF(t), v & ~TRANS_CONF_ENABLE);
	for (int w = 0; w < 1000; w++) {
		if (!(i915_read32(i915, TRANS_CONF(t)) & TRANS_CONF_STATE_ENABLE))
			break;
		lapic_delay_us(100);
	}
	intel_vrr_transcoder_disable(i915, p);
	uint32_t func = i915_read32(i915, TRANS_DDI_FUNC_CTL(t));
	func &= ~(TRANS_DDI_FUNC_ENABLE | trans_ddi_port_mask(i915) | TRANS_DDI_MODE_SELECT_MASK);
	i915_write32(i915, TRANS_DDI_FUNC_CTL(t), func);
	if (t != TRANSCODER_EDP)
		i915_write32(i915, TRANS_CLK_SEL(t), TRANS_CLK_SEL_DISABLED);
}

/* ---- framebuffers (the planes themselves: intel_plane.c) ----------------------- */

/* Said a few times, early: what a client puts on the screen and what the
 * plane was given for it.  A picture that is structured but wrong is
 * almost always one of these two disagreeing. */
static int g_fb_logged;

/* The scanout formats and layouts the universal planes take (Gen9+: the
 * Y layouts too; Gen8's planes were reprogrammed for Gen9, so its list
 * ends at X).  The core hands these out as IN_FORMATS and bounds ADDFB2
 * with them. */
const uint32_t intel_fb_formats[] = {
	DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565,   DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XBGR2101010,
};
const uint32_t intel_nfb_formats = sizeof(intel_fb_formats) / sizeof(intel_fb_formats[0]);
uint64_t intel_fb_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED, I915_FORMAT_MOD_Y_TILED,
};

/* DG2 and Meteor Lake scan out Tile4 (128-byte by 32-row tiles, like Y
 * but laid out differently inside) and have no Y layout at all. */
static int has_tile4(struct i915_device *i915)
{
	return !!(i915->info->flags & I915_INFO_HAS_4TILE);
}

const uint32_t intel_nfb_modifiers = sizeof(intel_fb_modifiers) / sizeof(intel_fb_modifiers[0]);

uint32_t intel_fb_tile_width(uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return 64;
	case I915_FORMAT_MOD_X_TILED:
		return 512;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_4_TILED:
		return 128;
	default:
		return 0;
	}
}

uint32_t intel_fb_tile_height(uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return 1;
	case I915_FORMAT_MOD_X_TILED:
		return 8;
	case I915_FORMAT_MOD_Y_TILED:
	case I915_FORMAT_MOD_4_TILED:
		return 32;
	default:
		return 0;
	}
}

int intel_fb_format_supported(uint32_t format)
{
	for (uint32_t i = 0; i < intel_nfb_formats; i++)
		if (intel_fb_formats[i] == format)
			return 1;
	return 0;
}

/* A framebuffer's layout is whatever its object was set to with
 * SET_TILING unless the caller named a modifier; both have to agree with
 * the object, and the pitch has to be whole tiles. */
int intel_fb_check(struct drm_device *dev, struct drm_gem_object *o,
		   const struct drm_mode_fb_cmd2 *r, uint64_t *modifier)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_fb_check(dev, o, r, modifier);
	struct i915_device *i915 = to_i915(dev);
	struct i915_bo *bo = o->priv;
	uint64_t mod = *modifier;
	uint64_t obj_mod;

	if (!bo)
		return -EINVAL;
	if (!intel_fb_format_supported(r->pixel_format))
		return -EINVAL;
	obj_mod = bo->tiling == I915_TILING_X ? I915_FORMAT_MOD_X_TILED :
		  bo->tiling == I915_TILING_Y ? I915_FORMAT_MOD_Y_TILED :
					        DRM_FORMAT_MOD_LINEAR;
	/* an object tiled Y is fetched Tile4 on the parts that have no Y */
	if (obj_mod == I915_FORMAT_MOD_Y_TILED && has_tile4(i915))
		return -EINVAL;
	if (mod == DRM_FORMAT_MOD_INVALID)
		mod = obj_mod;
	else if (mod != obj_mod && bo->tiling != I915_TILING_NONE)
		return -EINVAL;
	if (mod == I915_FORMAT_MOD_Y_TILED && (i915->info->gen < 9 || has_tile4(i915)))
		return -EINVAL;
	if (mod == I915_FORMAT_MOD_4_TILED && !has_tile4(i915))
		return -EINVAL;
	uint32_t tw = intel_fb_tile_width(mod);
	uint32_t th = intel_fb_tile_height(mod);
	if (!tw)
		return -EINVAL;
	if (r->pitches[0] % tw)
		return -EINVAL;
	if (bo->tiling != I915_TILING_NONE && bo->stride != r->pitches[0])
		return -EINVAL;
	/* the plane base is a page, and a tiled surface is whole tile rows */
	if (r->offsets[0] & 4095)
		return -EINVAL;
	uint64_t rows = ((uint64_t)r->height + th - 1) / th * th;
	if ((uint64_t)r->offsets[0] + (uint64_t)r->pitches[0] * rows > o->size)
		return -EINVAL;
	/* PLANE_STRIDE is a 10-bit tile count */
	if (r->pitches[0] / tw > 1023)
		return -EINVAL;
	*modifier = mod;
	if (g_fb_logged < 4) {
		g_fb_logged++;
		i915_dbg("[drm] i915: framebuffer %u: %ux%u %c%c%c%c, %s, pitch %u, object %llu KB, tiling %u\n",
			g_fb_logged, r->width, r->height, (char)(r->pixel_format & 0xff),
			(char)((r->pixel_format >> 8) & 0xff),
			(char)((r->pixel_format >> 16) & 0xff),
			(char)((r->pixel_format >> 24) & 0xff),
			mod == DRM_FORMAT_MOD_LINEAR	  ? "linear" :
			mod == I915_FORMAT_MOD_X_TILED ? "X tiled" :
			mod == I915_FORMAT_MOD_Y_TILED ? "Y tiled" :
			mod == I915_FORMAT_MOD_4_TILED ? "Tile4" :
							       "an unknown layout",
			r->pitches[0], (unsigned long long)(o->size / 1024), bo->tiling);
	}
	return 0;
}

/* ---- the pipe scaler (the gamma table: intel_color.c) ---------------------------- */

/* Scaler 0 of the pipe stretches the pipe's picture (PIPESRC) onto the
 * transcoder's active area.  Programmed before the transcoder starts and
 * cleared after it stops, the way the hardware wants it. */
static void pipe_scaler_program(struct i915_device *i915, struct intel_pipe *p,
				uint32_t dst_w, uint32_t dst_h, int on)
{
	if (!on) {
		i915_write32(i915, SKL_PS_CTRL(p->pipe, 0), 0);
		i915_write32(i915, SKL_PS_WIN_POS(p->pipe, 0), 0);
		i915_write32(i915, SKL_PS_WIN_SZ(p->pipe, 0), 0);
		(void)i915_read32(i915, SKL_PS_WIN_SZ(p->pipe, 0));
		return;
	}
	i915_write32(i915, SKL_PS_CTRL(p->pipe, 0),
		     PS_SCALER_EN | PS_SCALER_MODE_HQ | PS_BINDING_PIPE | PS_FILTER_MEDIUM);
	i915_write32(i915, SKL_PS_WIN_POS(p->pipe, 0), 0);
	i915_write32(i915, SKL_PS_WIN_SZ(p->pipe, 0), (dst_w << 16) | dst_h);
	(void)i915_read32(i915, SKL_PS_WIN_SZ(p->pipe, 0));
}

/* ---- what the firmware left running ------------------------------------------ */

/* The firmware lit the panel through this port and left the pipe,
 * transcoder and link up.  Before the first mode set they come down in
 * order -- plane, transcoder, port, clock -- but the panel stays
 * powered: a power cycle here is a blank screen for half a second and
 * a T12 wait for nothing. */
static void firmware_state_release(struct i915_device *i915, struct intel_output *o)
{
	int t = -1;

	if (o->port == PORT_A && !tgl_plus(i915)) {
		t = TRANSCODER_EDP;
	} else {
		for (int i = 0; i < i915->info->num_pipes; i++) {
			uint32_t f = i915_read32(i915, TRANS_DDI_FUNC_CTL(i));
			if ((f & TRANS_DDI_FUNC_ENABLE) && trans_ddi_port_of(i915, f) == o->port) {
				t = i;
				break;
			}
		}
	}
	if (t < 0)
		return;
	uint32_t func = i915_read32(i915, TRANS_DDI_FUNC_CTL(t));
	if (!(func & TRANS_DDI_FUNC_ENABLE))
		return;
	if (trans_ddi_port_of(i915, func) != o->port)
		return;
	int pipe = t;
	if (t == TRANSCODER_EDP) {
		switch (func & TRANS_DDI_EDP_INPUT_MASK) {
		case TRANS_DDI_EDP_INPUT_B_ONOFF:
			pipe = PIPE_B;
			break;
		case TRANS_DDI_EDP_INPUT_C_ONOFF:
			pipe = PIPE_C;
			break;
		default:
			pipe = PIPE_A;
			break;
		}
	}
	struct intel_pipe tmp;
	mm_memset(&tmp, 0, sizeof(tmp));
	tmp.pipe = pipe;
	tmp.transcoder = t;
	i915_dbg("[drm] i915: releasing the firmware's pipe %c on port %c\n", 'A' + pipe,
		'A' + o->port);
	intel_plane_disable(i915, &tmp);
	i915_write32(i915, CUR_CTL(pipe), 0);
	transcoder_disable(i915, &tmp);
	/* an engine the firmware left compressing for that transcoder */
	intel_dsc_disable(i915, o, &tmp);
	intel_ddi_buf_disable(i915, o);
	intel_dpll_unroute_port(i915, o->port);
	i915->display.pipes[pipe].active = 0;
	/* nothing to restore from here on: the fallback keeps the console
	 * on whatever the driver manages to light */
	i915->boot_scanout.pipe = -1;
}

/* Does the link as trained still carry the mode?  The retry lowers the
 * rate, which can drop below what the picture needs. */
static int link_carries_mode(const struct intel_output *o, const struct intel_pipe *p,
			     const struct drm_mode_modeinfo *m)
{
	/* a symbol per byte: the symbol clock times the lanes, in kB/s,
	 * against the stream's rate (compressed, with FEC, where it is) */
	uint64_t link = (uint64_t)o->link_rate_khz * (uint32_t)o->lane_count;
	uint64_t need = intel_dsc_link_data_rate(p, m->clock) / 8;
	return link * 99 >= need * 100;
}

/* ---- output enable / disable ----------------------------------------------------- */

static void output_disable(struct i915_device *i915, struct intel_output *o)
{
	struct intel_pipe *p = o->pipe >= 0 ? &i915->display.pipes[o->pipe] : NULL;

	int tmds = (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI);

	if (!o->active)
		return;
	if (o->is_edp)
		intel_backlight_disable(i915, o);
	if (p) {
		intel_vrr_disable(i915, o, p, NULL);
		if (tmds)
			intel_hdmi_disable(i915, o, p->transcoder);
		intel_vblank_crtc_off(i915, p, o->crtc);
		intel_plane_disable(i915, p);
		i915_write32(i915, CUR_CTL(p->pipe), 0);
		i915_write32(i915, CUR_BASE(p->pipe), 0);
		intel_wm_pipe_disable(i915, p->pipe);
		p->cursor_w = 0;
		transcoder_disable(i915, p);
		intel_dsc_disable(i915, o, p);
		if (p->scaled)
			pipe_scaler_program(i915, p, 0, 0, 0);
		p->scaled = 0;
		p->active = 0;
		p->output = -1;
	}
	if (tmds) {
		intel_hdmi_post_disable(i915, o);
	} else {
		intel_dp_link_off(i915, o);
		if (o->is_edp)
			intel_pps_panel_off(i915, o);
		else if (o->type == INTEL_OUTPUT_DP)
			intel_dp_sink_power(i915, o, 0);
	}
	intel_ddi_post_disable(i915, o);
	intel_power_put(i915, port_domain(o->port));
	intel_power_put(i915, aux_domain(i915, o->port));
	if (p)
		intel_power_put(i915, (enum intel_power_domain)(INTEL_PW_PIPE_A + p->pipe));
	o->active = 0;
	o->pipe = -1;
	intel_pmdemand_update(i915);
	if (p)
		intel_wm_update(i915);
}

static int output_enable(struct i915_device *i915, struct intel_output *o,
			 struct intel_pipe *p, const struct drm_mode_modeinfo *mode,
			 uint32_t src_w, uint32_t src_h)
{
	int rc;

	o->pipe = p->pipe;
	intel_power_get(i915, (enum intel_power_domain)(INTEL_PW_PIPE_A + p->pipe));
	intel_power_get(i915, port_domain(o->port));
	/* A Type-C port's mode decides which AUX well it needs (its own or
	 * the Thunderbolt one): connect first. */
	if (o->is_tc && intel_tc_connect(i915, o)) {
		kprintf("[drm] i915: port %s: nothing on the Type-C connector\n",
			intel_port_name(i915, o->port));
		intel_power_get(i915, aux_domain(i915, o->port));
		goto fail;
	}
	intel_power_get(i915, aux_domain(i915, o->port));
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		if (o->is_edp) {
			intel_pps_vdd_on(i915, o);
		}
		if (!o->detected && !intel_dp_detect(i915, o))
			goto fail;
		(void)intel_dsc_compute_config(i915, o, mode, &p->dsc);
		rc = intel_dp_choose_link(i915, o, mode);
		if (rc) {
			kprintf("[drm] i915: port %c: no link carries %ux%u\n",
				'A' + o->port, mode->hdisplay, mode->vdisplay);
			goto fail;
		}
		intel_pmdemand_pre_enable(i915, o, p->pipe, o->link_rate_khz);
		o->pll = intel_dpll_get_dp(i915, o->port, o->link_rate_khz, o->ssc);
		if (o->pll < 0) {
			kprintf("[drm] i915: port %c: no PLL for %u kHz\n", 'A' + o->port,
				o->link_rate_khz);
			goto fail;
		}
		intel_ddi_pre_enable(i915, o, mode);
		transcoder_route_port(i915, o, p, mode);
		if (o->is_edp)
			intel_pps_panel_on(i915, o);
		intel_dp_sink_power(i915, o, 1);
		intel_dsc_pre_link_train(i915, o, p);
		rc = intel_dp_link_train(i915, o);
		if (rc) {
			/* One retry at the slowest link the sink offers that
			 * still carries the mode, with every lane it has. */
			intel_dp_link_off(i915, o);
			intel_dpll_unroute_port(i915, o->port);
			intel_dpll_put(i915, o->pll);
			uint32_t slowest = 0;
			for (int r = 0; r < o->nsink_rates; r++) {
				uint32_t rate = o->sink_rates_khz[r];
				if (!intel_dpll_output_rate_supported(i915, o, rate) ||
				    (o->vbt_max_link_rate_khz && rate > o->vbt_max_link_rate_khz))
					continue;
				if (!slowest || rate < slowest)
					slowest = rate;
			}
			/* A panel driven from the small swing table that will
			 * not train gets the standard one on the retry. */
			if (o->is_edp && i915->display.vbt.edp_low_vswing &&
			    !o->swing_table_std) {
				o->swing_table_std = 1;
				kprintf("[drm] i915: port %c: retrying with the standard swing table\n",
					'A' + o->port);
			} else if (!slowest || slowest == o->link_rate_khz) {
				goto fail;
			}
			if (slowest)
				o->link_rate_khz = slowest;
			o->pll = intel_dpll_get_dp(i915, o->port, o->link_rate_khz, o->ssc);
			if (o->pll < 0)
				goto fail;
			intel_ddi_pre_enable(i915, o, mode);
			transcoder_route_port(i915, o, p, mode);
			intel_dsc_pre_link_train(i915, o, p);
			rc = intel_dp_link_train(i915, o);
			if (rc)
				goto fail;
			if (!link_carries_mode(o, p, mode)) {
				kprintf("[drm] i915: port %c: the link that trained does not carry %ux%u\n",
					'A' + o->port, mode->hdisplay, mode->vdisplay);
				goto fail;
			}
		}
		intel_dsc_enable(i915, o, p, mode);
	} else {
		if (!o->detected && !intel_hdmi_detect(i915, o))
			goto fail;
		if (intel_hdmi_mode_valid(i915, o, mode)) {
			kprintf("[drm] i915: port %c: %ux%u (%u kHz) is not a TMDS mode here\n",
				'A' + o->port, mode->hdisplay, mode->vdisplay, mode->clock);
			goto fail;
		}
		intel_pmdemand_pre_enable(i915, o, p->pipe, mode->clock);
		rc = intel_hdmi_pre_enable(i915, o, mode, p->transcoder);
		if (rc)
			goto fail;
	}
	o->pipe = p->pipe;
	p->output = (int)(o - i915->display.outputs);
	p->mode = *mode;
	p->pixel_rate_khz = mode->clock;
	p->src_w = src_w;
	p->src_h = src_h;
	p->scaled = (src_w != mode->hdisplay || src_h != mode->vdisplay);
	transcoder_program(i915, o, p, mode, src_w, src_h);
	if (p->scaled)
		pipe_scaler_program(i915, p, mode->hdisplay, mode->vdisplay, 1);
	o->active = 1;
	return 0;
fail:
	if (o->pll >= 0) {
		intel_dpll_unroute_port(i915, o->port);
		intel_dpll_put(i915, o->pll);
		o->pll = -1;
	}
	intel_power_put(i915, aux_domain(i915, o->port));
	intel_power_put(i915, port_domain(o->port));
	intel_power_put(i915, (enum intel_power_domain)(INTEL_PW_PIPE_A + p->pipe));
	mm_memset(&p->dsc, 0, sizeof(p->dsc));
	o->pipe = -1;
	return -EIO;
}

/* The pipe an output goes on: its CRTC's index when that is a pipe and
 * is free (or already this output's), else any idle pipe, else -- with
 * every pipe taken -- the CRTC's, whose current output gets displaced.
 * A CRTC is a DRM object per connector; there are more of them than
 * pipes on a board with many ports. */
static struct intel_pipe *pipe_for_output(struct i915_device *i915,
					  struct intel_output *o, int crtc_index)
{
	struct intel_display *d = &i915->display;
	int me = (int)(o - d->outputs);
	int npipes = i915->info->num_pipes;
	if (npipes > INTEL_MAX_PIPES)
		npipes = INTEL_MAX_PIPES;
	if (crtc_index < npipes) {
		struct intel_pipe *p = &d->pipes[crtc_index];
		if (!p->active || p->output == me)
			return p;
	}
	for (int i = 0; i < npipes; i++)
		if (!d->pipes[i].active)
			return &d->pipes[i];
	if (npipes > 0)
		return &d->pipes[crtc_index < npipes ? crtc_index : 0];
	return NULL;
}

/* The pixel rate the pipe of `c' runs at: the dot clock, raised by the
 * scaler's downscaling when the picture is larger than the timing. */
static uint32_t wm_cfg_pixel_rate(const struct intel_wm_pipe_cfg *c)
{
	uint32_t dst_w = c->src_w < c->hdisplay ? c->src_w : c->hdisplay;
	uint32_t dst_h = c->src_h < c->vdisplay ? c->src_h : c->vdisplay;

	if (!c->scaled || !dst_w || !dst_h)
		return c->clock_khz;
	return (uint32_t)(((uint64_t)c->clock_khz * c->src_w * c->src_h + (uint64_t)dst_w * dst_h - 1) /
			  ((uint64_t)dst_w * dst_h));
}

void intel_wm_cfg_dsc_from_hw(struct i915_device *i915,
			      struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES])
{
	for (int i = 0; i < i915->info->num_pipes && i < INTEL_MAX_PIPES; i++) {
		const struct intel_pipe *p = &i915->display.pipes[i];
		if (cfg[i].active && p->active)
			cfg[i].dsc_min_cdclk = intel_dsc_min_cdclk_khz(&p->dsc, wm_cfg_pixel_rate(&cfg[i]),
								       cfg[i].htotal);
	}
}

/* What a crtc's state asks of the display buffer, the memory and CDCLK. */
static void crtc_wm_cfg(struct i915_device *i915, struct intel_output *o,
			const struct drm_crtc_state *cs, const struct drm_plane_state *ps,
			const struct drm_plane_state *cps, struct intel_wm_pipe_cfg *c)
{
	const struct drm_mode_modeinfo *hw = &cs->adjusted_mode;
	int dp = o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP;
	uint32_t port_clock = hw->clock;
	if (dp) { /* the link is chosen later: the fastest one stands in */
		port_clock = 0;
		for (int r = 0; r < o->nsink_rates; r++)
			if (o->sink_rates_khz[r] > port_clock &&
			    (!o->vbt_max_link_rate_khz ||
			     o->sink_rates_khz[r] <= o->vbt_max_link_rate_khz) &&
			    intel_dpll_output_rate_supported(i915, o, o->sink_rates_khz[r]))
				port_clock = o->sink_rates_khz[r];
		if (!port_clock)
			port_clock = 270000;
	}
	mm_memset(c, 0, sizeof(*c));
	intel_wm_cfg_set_mode(c, hw, cs->mode.hdisplay, cs->mode.vdisplay, port_clock, dp,
			      o->is_edp, 4);
	/* a mode the uncompressed link cannot carry runs compressed: the
	 * engines' clock, from the configuration the check and the enable
	 * work out the same way */
	if (dp && o->detected && !intel_dp_link_carries(i915, o, hw)) {
		struct intel_dsc_config dsc;
		if (intel_dsc_compute_config(i915, o, hw, &dsc) == 0)
			c->dsc_min_cdclk = intel_dsc_min_cdclk_khz(&dsc, wm_cfg_pixel_rate(c),
								   hw->htotal);
	}
	/* the primary as the plane check left it (clipped to the mode where
	 * the planes are clipped) */
	{
		int on;
		uint32_t pw, ph, psx;
		intel_plane_wm_geometry(ps, &on, &pw, &ph, &psx);
		intel_wm_cfg_set_planes(c, on, pw, ph, ps->fb ? ps->fb->format : 0,
					ps->fb ? ps->fb->modifier : 0, cps->fb ? cps->crtc_w : 0);
		c->plane_src_x = (uint16_t)psx;
	}
}

/* The pipes as they will be after a commit: what runs now, with the
 * crtcs the commit changes (or all of them) replaced by their new state. */
static void commit_wm_cfg(struct i915_device *i915, struct drm_atomic_state *st, int all,
			  struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES])
{
	struct drm_device *dev = &i915->drm;
	intel_wm_cfg_from_hw(i915, cfg);
	intel_wm_cfg_dsc_from_hw(i915, cfg);
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = intel_output_for_crtc(i915, (int)i);
		struct intel_pipe *p;
		if ((!cs->changed && !all) || !o)
			continue;
		if (o->active && o->pipe >= 0)
			mm_memset(&cfg[o->pipe], 0, sizeof(cfg[0]));
		if (!cs->active)
			continue;
		p = (o->active && o->pipe >= 0) ? &i915->display.pipes[o->pipe] :
						  pipe_for_output(i915, o, (int)i);
		if (p)
			crtc_wm_cfg(i915, o, cs,
				    drm_atomic_plane_state(st, drm_crtc_primary(dev, (int)i)),
				    drm_atomic_plane_state(st, drm_crtc_cursor(dev, (int)i)),
				    &cfg[p - i915->display.pipes]);
	}
}

/* ---- one thread at a time ------------------------------------------------------- */

/* The mode set, the probes a client asks for and the hotplug worker all
 * drive the same AUX channels, panel power sequencer, Type-C PHYs and
 * power-well counts; two of them inside at once train a link while the
 * other re-reads the sink, or turn VDD off under a transfer.  Process
 * context only (a waiter yields).  The holder taking it again nests.  A
 * holder that never lets go is not waited for forever: after 20 s the
 * waiter says so and goes ahead, which is no worse than having no lock. */
#define DISPLAY_LOCK_GIVE_UP_NS 20000000000ULL

static volatile int g_disp_busy;
static task_t *volatile g_disp_owner;
static int g_disp_depth;

void intel_display_lock(struct i915_device *i915)
{
	task_t *me = sched_current();
	uint64_t start = 0;

	(void)i915;
	if (g_disp_depth > 0 && g_disp_owner == me) {
		g_disp_depth++;
		return;
	}
	while (__sync_lock_test_and_set(&g_disp_busy, 1)) {
		uint64_t now = hrtimer_now_ns();
		if (!start)
			start = now;
		if (now - start > DISPLAY_LOCK_GIVE_UP_NS) {
			static unsigned said;
			if (said < 4) {
				said++;
				kprintf("[drm] i915: display lock held for 20 s by another thread; going ahead without it\n");
			}
			break;
		}
		sched_yield_in_kernel();
	}
	g_disp_owner = me;
	g_disp_depth = 1;
}

void intel_display_unlock(struct i915_device *i915)
{
	(void)i915;
	/* a lock taken over from a holder that took too long is no longer
	 * that holder's to release */
	if (g_disp_depth <= 0 || g_disp_owner != sched_current())
		return;
	if (--g_disp_depth > 0)
		return;
	g_disp_owner = NULL;
	__sync_lock_release(&g_disp_busy);
}

/* ---- the DRM backend ------------------------------------------------------------- */

static const char *layout_name(uint64_t modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR	 ? "linear" :
	       modifier == I915_FORMAT_MOD_X_TILED ? "X tiled" :
	       modifier == I915_FORMAT_MOD_Y_TILED ? "Y tiled" :
	       modifier == I915_FORMAT_MOD_4_TILED ? "Tile4" :
						     "an unknown layout";
}

/* The CPU wrote into the framebuffer through write-back memory; the
 * display reads DRAM.  Flush the lines of the rectangles. */
int intel_fb_dirty(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_framebuffer *fb, const struct drm_mode_rect_k *rects,
		   uint32_t n)
{
	(void)dev;
	(void)crtc;
	if (!fb || !fb->obj || !fb->obj->pages)
		return -EINVAL;
	/* a discrete part: the copy in local memory is refreshed instead,
	 * and a surface already there needs no flush */
	if (i915_lmem_scanout_dirty(to_i915(dev), fb, rects, n) || i915_lmem_object(fb->obj))
		return 0;
	uint32_t cpp = fb->bpp / 8;
	if (!cpp)
		cpp = 4;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		/* Rectangles are in pixel space; a tiled surface interleaves
		 * them across the whole allocation, so flush all of it. */
		uint64_t off = fb->offset & ~63ULL;
		uint64_t end = (uint64_t)fb->offset + (uint64_t)fb->pitch * fb->height;
		while (off < end) {
			uint32_t page = (uint32_t)(off / 4096);
			if (page >= fb->obj->npages)
				break;
			uint8_t *va = (uint8_t *)phys_to_virt(fb->obj->pages[page]) + (off & 4095);
			__asm__ volatile("clflush (%0)" ::"r"(va) : "memory");
			off += 64;
		}
		__asm__ volatile("mfence" ::: "memory");
		return 0;
	}
	for (uint32_t r = 0; r < n; r++) {
		int32_t x1 = rects[r].x1 < 0 ? 0 : rects[r].x1;
		int32_t y1 = rects[r].y1 < 0 ? 0 : rects[r].y1;
		int32_t x2 = rects[r].x2 > (int32_t)fb->width ? (int32_t)fb->width : rects[r].x2;
		int32_t y2 = rects[r].y2 > (int32_t)fb->height ? (int32_t)fb->height : rects[r].y2;
		for (int32_t y = y1; y < y2; y++) {
			uint64_t off = (uint64_t)y * fb->pitch + (uint64_t)x1 * cpp + fb->offset;
			uint64_t end = (uint64_t)y * fb->pitch + (uint64_t)x2 * cpp + fb->offset;
			off &= ~63ULL;
			while (off < end) {
				uint32_t page = (uint32_t)(off / 4096);
				if (page >= fb->obj->npages)
					break;
				uint8_t *va = (uint8_t *)phys_to_virt(fb->obj->pages[page]) + (off & 4095);
				__asm__ volatile("clflush (%0)" ::"r"(va) : "memory");
				off += 64;
			}
		}
	}
	__asm__ volatile("mfence" ::: "memory");
	return 0;
}

/* A configuration refused, said in the log (the first few times): a
 * client only sees EINVAL. */
int intel_display_check_reject(struct i915_device *i915, const char *why)
{
	static unsigned said;
	(void)i915;
	if (said < 8) {
		said++;
		kprintf("[drm] i915: configuration refused: %s\n", why);
	}
	return -EINVAL;
}

static int atomic_check_locked(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct i915_device *i915 = to_i915(dev);

	if (!i915->display.ready)
		return -ENODEV;
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = intel_output_for_crtc(i915, (int)i);
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);

		if (!cs->active)
			continue;
		if (!o || !prim || !cur)
			return -ENODEV;
		/* the crtc's own connector, and no other, drives its output */
		if (cs->connector_mask != (1u << i))
			return intel_display_check_reject(i915, "the crtc is not driven by its own connector");
		const struct drm_mode_modeinfo *m = &cs->mode;
		cs->adjusted_mode = *m;
		cs->use_scaler = 0;
		if (o->is_edp && o->fixed_mode_valid &&
		    (m->hdisplay != o->fixed_mode.hdisplay ||
		     m->vdisplay != o->fixed_mode.vdisplay)) {
			/* the panel runs its own timing; the pipe scales */
			const struct drm_mode_modeinfo *f = &o->fixed_mode;
			if (m->hdisplay < 8 || m->vdisplay < 8 ||
			    m->hdisplay > f->hdisplay * 3 || m->vdisplay > f->vdisplay * 3 ||
			    m->hdisplay > 4096 || m->vdisplay > 4096)
				return intel_display_check_reject(i915, "the mode cannot be scaled onto the panel");
			cs->adjusted_mode = *f;
			cs->use_scaler = 1;
		}
		const struct drm_mode_modeinfo *hw = &cs->adjusted_mode;
		if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
			if (o->detected && !intel_dp_link_carries(i915, o, hw)) {
				/* compressed, perhaps */
				struct intel_dsc_config dsc;
				if (intel_dsc_compute_config(i915, o, hw, &dsc))
					return intel_display_check_reject(i915, "the DisplayPort link cannot carry the mode");
			}
		} else if (o->detected && intel_hdmi_mode_valid(i915, o, hw)) {
			return intel_display_check_reject(i915, "the mode is not a TMDS mode for this port");
		}
		int rc = intel_vrr_check(i915, o, cs);
		if (rc)
			return rc;
		rc = intel_color_check(i915, o, cs);
		if (rc)
			return rc;
		rc = intel_plane_atomic_check(i915, st, (int)i, cs);
		if (rc)
			return rc;
	}
	{
		struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES];
		commit_wm_cfg(i915, st, 0, cfg);
		if (intel_wm_check(i915, cfg, NULL))
			return intel_display_check_reject(i915, "the display buffer, memory bandwidth or CDCLK cannot carry the configuration"); /* display buffer, memory bandwidth or CDCLK */
	}
	return 0;
}

int intel_atomic_check(struct drm_device *dev, struct drm_atomic_state *st)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_atomic_check(dev, st);
	struct i915_device *i915 = to_i915(dev);
	int rc;

	intel_display_lock(i915);
	rc = atomic_check_locked(dev, st);
	intel_display_unlock(i915);
	return rc;
}

/* A full mode set of one crtc: its output comes down (or the firmware's
 * state is released) and goes up with the new mode. */
static int crtc_modeset(struct i915_device *i915, struct drm_atomic_state *st,
			uint32_t idx)
{
	struct drm_device *dev = &i915->drm;
	struct drm_crtc_state *cs = &st->crtcs[idx];
	struct intel_output *o = intel_output_for_crtc(i915, (int)idx);
	struct drm_plane_state *ps = drm_atomic_plane_state(st, drm_crtc_primary(dev, (int)idx));
	struct drm_plane_state *cps = drm_atomic_plane_state(st, drm_crtc_cursor(dev, (int)idx));
	const struct drm_mode_modeinfo *hw = &cs->adjusted_mode;
	uint32_t src_w = cs->mode.hdisplay, src_h = cs->mode.vdisplay;
	int rc;

	if (!o)
		return -ENODEV;
	if (o->active)
		output_disable(i915, o);
	else
		firmware_state_release(i915, o);
	/* The other output of a dual-mode DDI cannot be up at the same time. */
	if (o->sibling >= 0 && i915->display.outputs[o->sibling].active)
		output_disable(i915, &i915->display.outputs[o->sibling]);
	struct intel_pipe *p = pipe_for_output(i915, o, (int)idx);
	if (!p)
		return -EBUSY;
	if (p->active && p->output >= 0 && p->output != (int)(o - i915->display.outputs))
		output_disable(i915, &i915->display.outputs[p->output]);
	/* The pipe the firmware still scans out of (through another port
	 * than this output's) is not free either: its transcoder, port and
	 * clock come down before the pipe is programmed for this one. */
	if (i915->boot_scanout.pipe == (int)(p - i915->display.pipes))
		for (int k = 0; k < i915->display.nout; k++)
			if (!i915->display.outputs[k].active)
				firmware_state_release(i915, &i915->display.outputs[k]);
	static unsigned mode_sets;
	if (mode_sets < 4)
		mode_sets++;
	if (mode_sets < 4)
		i915_dbg("[drm] i915: mode set %u: %ux%u%s on %ux%u, fb %u %s pitch %u\n",
			 mode_sets, src_w, src_h, cs->use_scaler ? " scaled" : "",
			 hw->hdisplay, hw->vdisplay, ps->fb ? ps->fb->id : 0,
			 ps->fb ? layout_name(ps->fb->modifier) : "-",
			 ps->fb ? ps->fb->pitch : 0);
	p->pipe = (int)(p - i915->display.pipes);
	p->transcoder = transcoder_for(i915, o->port, p->pipe);
	/* what the sink is told about the MSA timing goes out with the link
	 * training below */
	intel_vrr_link_prepare(i915, o, cs);
	rc = output_enable(i915, o, p, hw, src_w, src_h);
	if (rc)
		return rc;
	/* the table before the pipe shows anything through it */
	intel_color_commit(i915, p, cs, 1);
	rc = transcoder_enable(i915, p);
	if (rc) {
		output_disable(i915, o);
		return rc;
	}
	p->active = 1;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) {
		intel_hdmi_pre_port_enable(i915, o, p, hw);
		intel_hdmi_enable(i915, o);
	}
	intel_pmdemand_update(i915);
	p->surf_ggtt = 0;
	p->cursor_w = 0;
	intel_wm_update(i915);
	rc = intel_plane_primary_program(i915, p, ps, 1);
	if (rc) {
		output_disable(i915, o);
		return rc;
	}
	intel_plane_cursor_program(i915, p, cps);
	intel_vblank_crtc_on(i915, p, (int)idx);
	if (o->is_edp)
		intel_backlight_enable(i915, o);
	o->crtc = (int)idx;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		intel_hdmi_post_enable(i915, o, p);
	intel_vrr_enable(i915, o, p, cs);
	kprintf("[drm] i915: pipe %c on port %c: %ux%u@%u (%u kHz)%s, plane at %08x\n",
		'A' + p->pipe, 'A' + o->port, hw->hdisplay, hw->vdisplay, hw->vrefresh,
		hw->clock, cs->use_scaler ? " (scaled)" : "", p->surf_ggtt);
	return 0;
}

/* Everything the commit is going to scan out, bound in the global address
 * space before any of the hardware is touched.  A buffer that cannot be
 * bound refuses the commit while the screen still shows what it showed --
 * not after the old configuration (the firmware's pipe, say) has already
 * been taken down for it, which leaves a dark panel and nothing else. */
static int commit_prepare(struct i915_device *i915, struct drm_atomic_state *st)
{
	struct drm_device *dev = &i915->drm;

	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);

		if (!cs->changed || !cs->active || !intel_output_for_crtc(i915, (int)i))
			continue;
		if (prim) {
			struct drm_plane_state *ps = drm_atomic_plane_state(st, prim);
			if (ps->fb && ps->fb->obj) {
				/* bound (on a discrete card: its local copy brought up
				 * to date over the damage), and a rotated view made */
				int rc = intel_plane_fb_prepare(i915, ps);
				if (rc) {
					static unsigned said;
					if (said < 4) {
						said++;
						kprintf("[drm] i915: commit refused before any change: framebuffer %u of crtc %u cannot be bound (%d)\n",
							ps->fb->id, i, rc);
					}
					return rc;
				}
			}
		}
		/* a cursor that cannot be bound is left off (intel_plane_cursor_program) */
		if (cur) {
			struct drm_plane_state *cps = drm_atomic_plane_state(st, cur);
			if (cps->fb && cps->fb->obj)
				(void)intel_plane_bo_bind(i915, cps->fb->obj);
		}
	}
	return 0;
}

/* A mode set that failed part-way, said in the log (the first few): the
 * old configuration is already gone by then. */
static void commit_failed(struct i915_device *i915, uint32_t crtc, int rc)
{
	static unsigned said;
	int lit = i915->boot_scanout.pipe >= 0;

	if (said >= 4)
		return;
	said++;
	for (int i = 0; i < i915->info->num_pipes && i < INTEL_MAX_PIPES; i++)
		if (i915->display.pipes[i].active)
			lit = 1;
	kprintf("[drm] i915: mode set of crtc %u failed (%d)%s\n", crtc, rc,
		lit ? "" : "; no pipe is running, the panel stays dark until the next mode set");
}

static int atomic_commit_locked(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct i915_device *i915 = to_i915(dev);
	int rc = 0;
	int all = 0;
	struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES];

	if (!i915->display.ready)
		return -ENODEV;
	rc = commit_prepare(i915, st);
	if (rc)
		return rc;
	/* crtcs going off first: they may free the pipe another one needs */
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = intel_output_for_crtc(i915, (int)i);
		if (!cs->changed || cs->active || !o)
			continue;
		if (o->active)
			output_disable(i915, o);
		o->crtc = -1;
	}
	/* CDCLK up to what the commit leads to; where it can only change with
	 * every pipe off, they all come down here and are set again below */
	commit_wm_cfg(i915, st, 0, cfg);
	if (intel_cdclk_update(i915, cfg, 0) == -EBUSY) {
		for (int k = 0; k < i915->display.nout; k++)
			if (i915->display.outputs[k].active)
				output_disable(i915, &i915->display.outputs[k]);
		/* ...and the firmware's pipe, which runs from the same clock:
		 * the PLL does not change under it */
		for (int k = 0; k < i915->display.nout; k++)
			if (!i915->display.outputs[k].active)
				firmware_state_release(i915, &i915->display.outputs[k]);
		all = 1;
		commit_wm_cfg(i915, st, 1, cfg);
		intel_cdclk_update(i915, cfg, 1);
	}
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = intel_output_for_crtc(i915, (int)i);
		if ((!cs->changed && !all) || !cs->active || !o)
			continue;
		int modeset = drm_crtc_state_needs_modeset(cs) || !o->active || o->pipe < 0 ||
			      o->crtc != (int)i;
		if (!modeset) {
			/* the pipe stays up: a different picture, a different
			 * table, a moved cursor */
			struct intel_pipe *p = &i915->display.pipes[o->pipe];
			if (cs->use_scaler != p->scaled ||
			    cs->mode.hdisplay != p->src_w || cs->mode.vdisplay != p->src_h)
				modeset = 1;
			if (!modeset) {
				struct drm_plane_state *ps = drm_atomic_plane_state(st, drm_crtc_primary(dev, (int)i));
				struct drm_plane_state *cps = drm_atomic_plane_state(st, drm_crtc_cursor(dev, (int)i));
				intel_vrr_disable(i915, o, p, cs);
				intel_color_commit(i915, p, cs, 0);
				if (ps->changed) {
					rc = intel_plane_primary_program(i915, p, ps, 0);
					if (rc)
						return rc;
					if (ps->fb_changed) {
						p->flip_pending = 1;
						static unsigned flips;
						if (flips < 4)
							flips++;
						if (flips < 4 && ps->fb)
							i915_dbg("[drm] i915: flip %u: fb %u, %s, pitch %u, at %08x\n",
								 flips, ps->fb->id,
								 layout_name(ps->fb->modifier),
								 ps->fb->pitch, p->surf_ggtt);
					}
				}
				if (cps->changed)
					intel_plane_cursor_program(i915, p, cps);
				/* variable refresh on (where it is coming on) before
				 * the push that sends the new frame out */
				intel_vrr_enable(i915, o, p, cs);
				if (ps->changed || cps->changed)
					intel_vrr_send_push(i915, p);
				continue;
			}
		}
		rc = crtc_modeset(i915, st, i);
		if (rc) {
			commit_failed(i915, i, rc);
			return rc;
		}
	}
	/* and down again where that needs nothing stopped */
	intel_cdclk_update(i915, NULL, 1);
	return 0;
}

int intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_atomic_commit(dev, st);
	struct i915_device *i915 = to_i915(dev);
	int rc;

	intel_display_lock(i915);
	rc = atomic_commit_locked(dev, st);
	intel_display_unlock(i915);
	return rc;
}

static int display_verify_locked(struct i915_device *i915)
{
	for (int i = 0; i < i915->info->num_pipes && i < INTEL_MAX_PIPES; i++) {
		struct intel_pipe *p = &i915->display.pipes[i];
		if (!p->active)
			continue;
		if (!(i915_read32(i915, TRANS_CONF(p->transcoder)) & TRANS_CONF_STATE_ENABLE)) {
			kprintf("[drm] i915: pipe %c: transcoder not running\n", 'A' + i);
			return -EIO;
		}
		if (!intel_plane_enabled(i915, p->pipe)) {
			kprintf("[drm] i915: pipe %c: plane not enabled\n", 'A' + i);
			return -EIO;
		}
		uint32_t f0 = i915_read32(i915, PIPE_FRMCOUNT(p->pipe));
		uint32_t l0 = i915_read32(i915, PIPE_DSL(p->pipe));
		lapic_delay_ms(40);
		uint32_t f1 = i915_read32(i915, PIPE_FRMCOUNT(p->pipe));
		uint32_t l1 = i915_read32(i915, PIPE_DSL(p->pipe));
		if (f0 == f1 && l0 == l1) {
			kprintf("[drm] i915: pipe %c: no scanning (frame counter %u, scan line %u)\n",
				'A' + i, f0, l0 & 0x1fff);
			kprintf("[drm] i915:   TRANS_CONF %08x, TRANS_DDI_FUNC_CTL %08x, PIPESRC %08x, PIPE_MISC %08x\n",
				i915_read32(i915, TRANS_CONF(p->transcoder)),
				i915_read32(i915, TRANS_DDI_FUNC_CTL(p->transcoder)),
				i915_read32(i915, PIPESRC(p->pipe)),
				i915_read32(i915, PIPE_MISC(p->pipe)));
			if (intel_plane_dspcntr(i915))
				kprintf("[drm] i915:   DSPCNTR %08x, DSPSURF %08x, DSPSTRIDE %08x, WM0 %08x\n",
					i915_read32(i915, DSPCNTR(p->pipe)),
					i915_read32(i915, DSPSURF(p->pipe)),
					i915_read32(i915, DSPSTRIDE(p->pipe)),
					i915_read32(i915, WM0_PIPE_ILK(p->pipe)));
			else
				kprintf("[drm] i915:   PLANE_CTL %08x, SURF %08x, STRIDE %08x, SIZE %08x, WM0 %08x, BUF_CFG %08x\n",
					i915_read32(i915, PLANE_CTL(p->pipe, 0)),
					i915_read32(i915, PLANE_SURF(p->pipe, 0)),
					i915_read32(i915, PLANE_STRIDE(p->pipe, 0)),
					i915_read32(i915, PLANE_SIZE(p->pipe, 0)),
					i915_read32(i915, PLANE_WM(p->pipe, 0, 0)),
					i915_read32(i915, PLANE_BUF_CFG(p->pipe, 0)));
			kprintf("[drm] i915:   HTOTAL %08x, VTOTAL %08x, DATA_M1 %08x, DATA_N1 %08x, DDI_BUF_CTL %08x, DP_TP_CTL %08x\n",
				i915_read32(i915, TRANS_HTOTAL(p->transcoder)),
				i915_read32(i915, TRANS_VTOTAL(p->transcoder)),
				i915_read32(i915, TRANS_DATA_M1(p->transcoder)),
				i915_read32(i915, TRANS_DATA_N1(p->transcoder)),
				i915_read32(i915, DDI_BUF_CTL(PORT_A)),
				i915_read32(i915, DP_TP_CTL(PORT_A)));
			return -EIO;
		}
		if (p->underruns) {
			kprintf("[drm] i915: pipe %c: %llu underruns\n", 'A' + i,
				(unsigned long long)p->underruns);
			/* underruns are reported, not fatal */
		}
	}
	return 0;
}

int intel_display_verify(struct drm_device *dev)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_display_verify(dev);
	struct i915_device *i915 = to_i915(dev);
	int rc;

	intel_display_lock(i915);
	rc = display_verify_locked(i915);
	intel_display_unlock(i915);
	return rc;
}

static void display_fallback_locked(struct i915_device *i915)
{
	/* Back to what the firmware left: its plane on its pipe, if the
	 * pipe and transcoder are still running the mode it set. */
	if (i915->boot_scanout.pipe >= 0) {
		int pipe = i915->boot_scanout.pipe;
		if (intel_plane_dspcntr(i915)) {
			i915_write32(i915, DSPSTRIDE(pipe), i915->boot_scanout.stride);
			i915_write32(i915, DSPCNTR(pipe), i915->boot_scanout.plane_ctl);
			i915_write32(i915, DSPSURF(pipe), i915->boot_scanout.surf);
			return;
		}
		i915_write32(i915, PLANE_STRIDE(pipe, 0), i915->boot_scanout.stride);
		i915_write32(i915, PLANE_SIZE(pipe, 0), i915->boot_scanout.size);
		i915_write32(i915, PLANE_CTL(pipe, 0), i915->boot_scanout.plane_ctl);
		i915_write32(i915, PLANE_SURF(pipe, 0), i915->boot_scanout.surf);
		return;
	}
	/* The firmware's pipe was already taken apart, so there is nothing
	 * to fall back to: the framebuffer the console returns to is not
	 * being scanned out by anything and the panel stays dark.  Say so --
	 * from the machine's point of view the boot simply goes silent. */
	for (int i = 0; i < i915->info->num_pipes && i < INTEL_MAX_PIPES; i++)
		if (i915->display.pipes[i].active)
			return;
	kprintf("[drm] i915: no pipe is running: the panel stays dark from here (the console is writing to memory nothing displays)\n");
}

void intel_display_fallback(struct drm_device *dev)
{
	if (intel_legacy_display_active(to_i915(dev))) {
		intel_legacy_display_fallback(dev);
		return;
	}
	struct i915_device *i915 = to_i915(dev);

	kprintf("[drm] i915: display fallback: %s\n",
		i915->boot_scanout.pipe >= 0 ? "the firmware's plane back on its pipe" :
					       "no firmware pipe left to go back to");
	intel_display_lock(i915);
	display_fallback_locked(i915);
	intel_display_unlock(i915);
}

/* ---- initialisation ----------------------------------------------------------- */

static uint32_t conn_type_for(enum intel_output_type t)
{
	switch (t) {
	case INTEL_OUTPUT_EDP: return DRM_MODE_CONNECTOR_eDP;
	case INTEL_OUTPUT_DP: return DRM_MODE_CONNECTOR_DisplayPort;
	case INTEL_OUTPUT_HDMI: return DRM_MODE_CONNECTOR_HDMIA;
	case INTEL_OUTPUT_DVI: return DRM_MODE_CONNECTOR_DVID;
	default: return DRM_MODE_CONNECTOR_Unknown;
	}
}

static void outputs_from_vbt(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	struct intel_vbt *vbt = &d->vbt;

	d->nout = 0;
	for (int port = 0; port < INTEL_MAX_PORTS; port++) {
		struct intel_vbt_port *vp = &vbt->port[port];
		enum intel_output_type types[2] = { INTEL_OUTPUT_NONE, INTEL_OUTPUT_NONE };
		int ntypes = 0;
		if (!vbt->valid) {
			/* No VBT: assume the usual laptop wiring, eDP on A. */
			if (port == PORT_A)
				types[ntypes++] = INTEL_OUTPUT_EDP;
			else
				continue;
		} else if (!vp->present) {
			continue;
		} else if (vp->supports_edp) {
			types[ntypes++] = INTEL_OUTPUT_EDP;
		} else {
			/* A port wired for both is two outputs: which one
			 * carries a picture depends on what gets plugged in. */
			if (vp->supports_dp)
				types[ntypes++] = INTEL_OUTPUT_DP;
			if (vp->supports_hdmi)
				types[ntypes++] = INTEL_OUTPUT_HDMI;
			else if (vp->supports_dvi)
				types[ntypes++] = INTEL_OUTPUT_DVI;
		}
		int first = d->nout;
		for (int k = 0; k < ntypes && d->nout < INTEL_MAX_OUTPUTS; k++) {
			enum intel_output_type type = types[k];
			struct intel_output *o = &d->outputs[d->nout];
			mm_memset(o, 0, sizeof(*o));
			o->present = 1;
			o->port = port;
			o->type = type;
			o->is_edp = (type == INTEL_OUTPUT_EDP);
			o->conn = -1;
			o->crtc = -1;
			o->pipe = -1;
			o->pll = -1;
			o->sibling = -1;
			o->ddc_pin = vp->ddc_pin;
			o->lane_reversal = vbt->valid ? vp->lane_reversal : 0;
			/* what the board's DisplayPort wiring allows (0: no limit) */
			if (I915_FEAT_DP_VBT_LIMITS && vbt->valid &&
			    (type == INTEL_OUTPUT_DP || type == INTEL_OUTPUT_EDP)) {
				o->vbt_max_link_rate_khz = vp->dp_max_link_rate_khz;
				o->vbt_max_lanes = vp->dp_max_lanes;
			}
			/* A discrete card's "Type-C" ports are fixed connectors, and
			 * a VBT can say a port's PHY is dedicated to an external one. */
			if ((i915->info->flags & I915_INFO_HAS_TC_PHY) &&
			    !(i915->info->flags & I915_INFO_IS_DGFX) &&
			    !(vbt->valid && vp->dedicated_external)) {
				int first_tc = tgl_plus(i915) ? PORT_TC1 : PORT_C;
				if (port >= first_tc) {
					o->is_tc = 1;
					o->tc_index = port - first_tc;
				}
			}
			int aux_port = (vp->aux_ch != 0xff && vbt->valid) ? vp->aux_ch : port;
			if (aux_port >= INTEL_MAX_PORTS)
				aux_port = port;
			intel_dp_aux_init(i915, &o->aux, aux_port);
			o->gmbus_pin = intel_gmbus_pin_for_port(i915, port, o->ddc_pin);
			char name[24] = "i915 gmbus DDI ?";
			name[15] = (char)('A' + port);
			intel_gmbus_adapter_init(i915, &o->ddc, o->gmbus_pin, name);
			d->nout++;
		}
		if (d->nout - first == 2) {
			d->outputs[first].sibling = first + 1;
			d->outputs[first + 1].sibling = first;
		}
	}
}

int intel_display_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	struct drm_device *dev = &i915->drm;
	int rc;

	mm_memset(d, 0, sizeof(*d));
	for (int i = 0; i < INTEL_MAX_PIPES; i++) {
		d->pipes[i].pipe = i;
		d->pipes[i].output = -1;
	}
	intel_fb_modifiers[2] = has_tile4(i915) ? I915_FORMAT_MOD_4_TILED : I915_FORMAT_MOD_Y_TILED;
	switch (i915->info->dpll_model) {
	case I915_DPLL_SKL:
		d->model = INTEL_DISPLAY_SKL;
		break;
	case I915_DPLL_BXT:
		d->model = INTEL_DISPLAY_BXT;
		break;
	case I915_DPLL_ICL:
		d->model = i915->info->display_ver >= 12 ? INTEL_DISPLAY_TGL : INTEL_DISPLAY_ICL;
		break;
	case I915_DPLL_MTL:
		d->model = INTEL_DISPLAY_MTL;
		break;
	case I915_DPLL_DG2:
		d->model = INTEL_DISPLAY_DG2;
		break;
	case I915_DPLL_LEGACY:
		/* Broadwell's display is Haswell's DDI one; Cherryview, though
		 * the same generation, has Valleyview's instead. */
		if ((i915->info->gen == 8 && i915->info->platform != I915_PLATFORM_CHERRYVIEW) ||
		    i915->info->platform == I915_PLATFORM_HASWELL) {
			d->model = INTEL_DISPLAY_BDW;
			break;
		}
		/* fall through */
	default:
		kprintf("[drm] i915: display code for %s is not implemented (%s)\n",
			i915->info->name,
			(i915->info->flags & I915_INFO_IS_DGFX) ?
				"a discrete part's local memory and PHYs are not driven" :
				"its PLL model is not driven");
		return -ENODEV;
	}
	d->pps_base = d->model == INTEL_DISPLAY_BXT ? BXT_PP_BASE : PCH_PP_BASE;
	d->bl_bxt = d->model == INTEL_DISPLAY_BXT || i915->pch >= I915_PCH_CNP;
	switch (d->model) {
	case INTEL_DISPLAY_BXT:
		/* Gemini Lake doubled Broxton's buffer */
		d->ddb_blocks = (i915->info->platform == I915_PLATFORM_GEMINILAKE ? 1024 : 512) - 4;
		break;
	case INTEL_DISPLAY_ICL: d->ddb_blocks = ICL_DDB_SIZE - 4; break;
	case INTEL_DISPLAY_TGL:
	case INTEL_DISPLAY_MTL:
	case INTEL_DISPLAY_DG2: d->ddb_blocks = TGL_DDB_SLICE_SIZE - 4; break;
	default: d->ddb_blocks = SKL_DDB_SIZE - 4; break;
	}

	intel_opregion_init(i915);
	if (d->opregion.vbt) {
		rc = intel_vbt_parse_platform(d->opregion.vbt, d->opregion.vbt_size,
					      i915->info->display_ver, i915->info->platform,
					      &d->vbt);
		if (rc == 0) {
			i915_dbg("[drm] i915: VBT %.20s version %u, %d child devices, panel type %d%s\n",
				d->vbt.signature, d->vbt.version, d->vbt.nchildren,
				d->vbt.panel_type,
				d->vbt.panel_mode_valid ? ", panel timing" : "");
			for (int p = 0; p < INTEL_MAX_PORTS; p++) {
				struct intel_vbt_port *vp = &d->vbt.port[p];
				if (!vp->present)
					continue;
				i915_dbg("[drm] i915:   DDI %c:%s%s%s%s aux %c ddc %u\n", 'A' + p,
					vp->supports_edp ? " eDP" : "",
					vp->supports_dp ? " DP" : "",
					vp->supports_hdmi ? " HDMI" : "",
					vp->supports_dvi ? " DVI" : "",
					vp->aux_ch == 0xff ? '-' : 'A' + vp->aux_ch, vp->ddc_pin);
			}
		}
	}

	rc = intel_power_init(i915);
	if (rc)
		return rc;
	/* the stream compression engines the display has (its fuse
	 * registers are readable once the display's power is up) */
	intel_dp_dsc_source_init(i915);
	rc = intel_cdclk_init(i915);
	if (rc)
		return rc;
	intel_wm_init(i915);
	intel_dpll_init(i915);
	intel_ddi_init(i915);
	intel_pps_init(i915);
	intel_backlight_init(i915);
	i915_ggtt_program_pat(i915);

	intel_gmbus_init(i915);
	outputs_from_vbt(i915);
	if (d->model == INTEL_DISPLAY_MTL)
		mtl_phy_init(i915);
	else if (d->model == INTEL_DISPLAY_DG2)
		dg2_phy_init(i915);
	/* What the firmware left the ports running at, while it is still
	 * there to be read: the rate of the PLL each port is routed to and
	 * the width its buffer was enabled with. */
	for (int i = 0; i < d->nout; i++) {
		struct intel_output *o = &d->outputs[i];
		uint32_t buf = i915_read32(i915, DDI_BUF_CTL(o->port));
		/* Port A shares its lanes with E only up to Gen10, where the
		 * strap says how they were split (Broxton and Gemini Lake
		 * only ever run it four wide); from Gen11 it has all four. */
		if (o->port == PORT_A)
			o->four_lane_strap = i915->info->display_ver >= 11 ||
					     d->model == INTEL_DISPLAY_BXT ||
					     !!(buf & DDI_A_4_LANES);
		if (!(buf & DDI_BUF_CTL_ENABLE))
			continue;
		o->fw_link_rate_khz = intel_dpll_port_link_rate(i915, o->port);
		o->fw_lanes = (int)(((buf & DDI_PORT_WIDTH_MASK) >> 1) + 1);
		/* From Tiger Lake the width that counts is the transcoder's
		 * (Meteor Lake no longer keeps one in the port's buffer
		 * control at all). */
		if (tgl_plus(i915)) {
			for (int t = 0; t < i915->info->num_pipes && t < INTEL_MAX_PIPES; t++) {
				uint32_t f = i915_read32(i915, TRANS_DDI_FUNC_CTL(t));
				if ((f & TRANS_DDI_FUNC_ENABLE) && trans_ddi_port_of(i915, f) == o->port) {
					o->fw_lanes = (int)(((f & TRANS_DDI_PORT_WIDTH_MASK) >> 1) + 1);
					break;
				}
			}
		}
		if (o->fw_link_rate_khz)
			i915_dbg("[drm] i915: port %c: the firmware runs it at %u kHz x%d\n",
				'A' + o->port, o->fw_link_rate_khz, o->fw_lanes);
	}
	dev->refresh_hz = 60;
	dev->min_width = 320;
	dev->min_height = 200;
	dev->max_width = 4096;
	dev->max_height = 4096;
	for (int i = 0; i < d->nout; i++) {
		struct intel_output *o = &d->outputs[i];
		int conn = drm_connector_add(dev, conn_type_for(o->type), 0, 0);
		if (conn < 0)
			break;
		o->conn = conn;
		dev->enc[conn].type = DRM_MODE_ENCODER_TMDS;
		dev->conn[conn].priv = o;
		/* the crtc and its planes exist now (a crtc's index is its
		 * connector's): their vblank state, plane and colour
		 * properties */
		intel_vblank_crtc_reset(i915, conn);
		int frc = intel_plane_props_init(i915, conn);
		if (frc)
			kprintf("[drm] i915: crtc %d: plane properties not attached (%d)\n", conn, frc);
		frc = intel_color_crtc_init(i915, conn);
		if (frc)
			kprintf("[drm] i915: crtc %d: colour properties not attached (%d)\n", conn, frc);
		/* the connector's vrr_capable, false until a probe finds a
		 * refresh range */
		intel_vrr_connector_init(i915, o, conn);
		/* Probe now: the console needs modes before anyone asks. */
		intel_connector_probe(i915, conn);
		kprintf("[drm] i915: connector %d: DDI %s %s, %s, %u modes%s%s\n", conn,
			intel_port_name(i915, o->port),
			o->type == INTEL_OUTPUT_EDP ? "eDP" : o->type == INTEL_OUTPUT_DP ? "DP" :
			o->type == INTEL_OUTPUT_HDMI ? "HDMI" : "DVI",
			dev->conn[conn].connected ? "connected" : "disconnected",
			dev->conn[conn].nmodes,
			dev->conn[conn].sink_name[0] ? " " : "", dev->conn[conn].sink_name);
	}
	intel_hpd_init(i915);
	intel_backlight_sysfs_init(i915);
	d->ready = 1;
	intel_opregion_driver_ready(i915);
	return 0;
}

void intel_display_suspend(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	if (!d->ready)
		return;
	intel_display_lock(i915);
	for (int i = 0; i < d->nout; i++) {
		struct intel_output *o = &d->outputs[i];
		if (o->active)
			output_disable(i915, o);
		o->crtc = -1;
	}
	for (int i = 0; i < INTEL_MAX_PIPES; i++) {
		d->pipes[i].active = 0;
		d->pipes[i].output = -1;
		d->pipes[i].surf_ggtt = 0;
	}
	intel_display_unlock(i915);
}

int intel_display_resume(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	int rc;

	if (!d->ready)
		return 0;
	/* nothing of the firmware's is running any more */
	i915->boot_scanout.pipe = -1;
	rc = intel_power_init(i915);
	if (rc)
		return rc;
	rc = intel_cdclk_init(i915);
	if (rc)
		return rc;
	intel_wm_init(i915);
	intel_dpll_init(i915);
	if (d->model == INTEL_DISPLAY_DG2)
		dg2_phy_init(i915);
	intel_ddi_init(i915);
	intel_pps_init(i915);
	intel_backlight_init(i915);
	i915_ggtt_program_pat(i915);
	intel_gmbus_init(i915);
	for (int i = 0; i < d->nout; i++) {
		struct intel_output *o = &d->outputs[i];
		o->pll = -1;
		o->pipe = -1;
		o->active = 0;
		o->panel_powered = 0;
		o->vdd_forced = 0;
	}
	intel_hpd_init(i915);
	/* the microcontroller lost its program with the power */
	if (d->dmc_loaded)
		(void)intel_dmc_load(i915);
	return 0;
}

void intel_display_fini(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	d->hpd_ready = 0;
	for (int i = 0; i < d->nout; i++)
		output_disable(i915, &d->outputs[i]);
	intel_opregion_fini(i915);
	d->ready = 0;
}
