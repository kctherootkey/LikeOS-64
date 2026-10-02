// LikeOS -- the display of the Intel parts before DDI: pipes, mode sets,
// interrupts and the DRM backend.
//
// One pipe per active output.  A pipe is its timing generator (the
// transcoder registers), its source size and its configuration word
// (PIPECONF), fed by a primary plane and a cursor and clocked by a PLL --
// a DPLL of its own on the GMCH parts, a DPIO PLL on Valleyview and
// Cherryview, and on Ironlake to Ivy Bridge a PCH PLL for whichever PCH
// transcoder the pipe sends its pixels to over the FDI link.  A mode set
// brings these up in the order each family needs, calling the output's
// hooks at the points its port has to be ready; teardown is the reverse.
// Pipes are handed out to outputs as they are lit (the CRTCs of the DRM
// core are one per connector) within the outputs' own pipe limits.
//
// Interrupts arrive through the top level: the pipe status registers on
// the GMCH parts (vblank, underrun), the north display's IIR with the PCH
// behind it on Ironlake to Ivy Bridge, and the hotplug pins on both.  The
// hotplug interrupt only notes the pins; a worker probes them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2026 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/mm/memory.h>

/* Valleyview: the Gunit's clock gating (display-relative) */
#define LGD_VLV_GUNIT_CLOCK_GATE 0x2060u
#define LGD_GCFG_DIS (1u << 8)

static struct lg_display g_lg;
static int g_lg_active;
static int g_fw_released;
static int g_reset_pending; /* the outputs went down for a GPU reset */

struct lg_display *lg_get(void)
{
	return g_lg_active ? &g_lg : NULL;
}

int intel_legacy_display_active(struct i915_device *i915)
{
	return g_lg_active && g_lg.i915 == i915;
}

/* ---- small helpers ----------------------------------------------------------------- */

int lg_wait(struct lg_display *d, uint32_t reg, uint32_t mask, uint32_t value,
	    uint32_t timeout_us)
{
	for (uint32_t t = 0;; t += 10) {
		if ((lg_rd(d, reg) & mask) == value)
			return 0;
		if (t >= timeout_us)
			return -ETIMEDOUT;
		lapic_delay_us(10);
	}
}

void lg_timings_from_mode(struct lg_timings *t, const struct drm_mode_modeinfo *m)
{
	uint16_t vdisplay = m->vdisplay, vss = m->vsync_start, vse = m->vsync_end;
	uint16_t vtotal = m->vtotal;

	if (m->flags & DRM_MODE_FLAG_INTERLACE) {
		vdisplay /= 2;
		vss /= 2;
		vse /= 2;
		vtotal /= 2;
	}
	t->clock = m->clock;
	t->hdisplay = m->hdisplay;
	t->hsync_start = m->hsync_start;
	t->hsync_end = m->hsync_end;
	t->htotal = m->htotal;
	t->hblank_start = m->hsync_start < m->hdisplay ? m->hsync_start : m->hdisplay;
	t->hblank_end = m->hsync_end > m->htotal ? m->hsync_end : m->htotal;
	t->vdisplay = vdisplay;
	t->vsync_start = vss;
	t->vsync_end = vse;
	t->vtotal = vtotal;
	t->vblank_start = vss < vdisplay ? vss : vdisplay;
	t->vblank_end = vse > vtotal ? vse : vtotal;
	t->flags = m->flags;
}

/* M/N with a fixed N (what the firmware does too; some sinks are fussy
 * about large values), reduced into the 24-bit fields. */
static void compute_m_n(uint32_t *ret_m, uint32_t *ret_n, uint64_t m, uint64_t n,
			uint32_t constant_n)
{
	uint64_t rm, rn = constant_n;

	if (!n)
		n = 1;
	rm = m * rn / n;
	while (rm > DATA_LINK_M_N_MASK || rn > DATA_LINK_M_N_MASK) {
		rm >>= 1;
		rn >>= 1;
	}
	*ret_m = (uint32_t)rm;
	*ret_n = (uint32_t)rn;
}

void lg_link_compute_m_n(int bpp, int nlanes, uint32_t pixel_clock, uint32_t link_clock,
			 struct lg_link_m_n *m_n)
{
	m_n->tu = 64;
	compute_m_n(&m_n->data_m, &m_n->data_n, (uint64_t)pixel_clock * (uint32_t)bpp,
		    (uint64_t)link_clock * (uint32_t)nlanes * 8u, 0x8000000);
	compute_m_n(&m_n->link_m, &m_n->link_n, pixel_clock, link_clock, 0x80000);
}

static int has_frame_counter(struct lg_display *d)
{
	return d->ver >= 3;
}

static uint32_t frame_count(struct lg_display *d, int pipe)
{
	if (d->is_g4x || d->ver >= 5)
		return lg_rd(d, PIPE_FRMCOUNT_G4X(d, pipe));
	return lg_rd(d, PIPEFRAME(d, pipe)) & PIPE_FRAME_HIGH_MASK;
}

static uint32_t scanline(struct lg_display *d, int pipe)
{
	return lg_rd(d, PIPEDSL(d, pipe)) & (d->ver == 2 ? PIPEDSL_LINE_MASK_GEN2 :
						      PIPEDSL_LINE_MASK);
}

void lg_wait_for_vblank(struct lg_display *d, int pipe)
{
	if (!(lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE))
		return;
	if (has_frame_counter(d)) {
		uint32_t f0 = frame_count(d, pipe);
		for (int i = 0; i < 1000; i++) {
			if (frame_count(d, pipe) != f0)
				return;
			lapic_delay_us(100);
		}
		return;
	}
	/* gen2: no frame counter, the scan line wraps at the vblank */
	uint32_t last = scanline(d, pipe);
	for (int i = 0; i < 10000; i++) {
		uint32_t now = scanline(d, pipe);
		if (now < last)
			return;
		last = now;
		lapic_delay_us(10);
	}
}

int lg_pipe_scanline_moving(struct lg_display *d, int pipe)
{
	uint32_t l0 = scanline(d, pipe);
	for (int i = 0; i < 50; i++) {
		lapic_delay_us(100);
		if (scanline(d, pipe) != l0)
			return 1;
	}
	return 0;
}

static void wait_scanline(struct lg_display *d, int pipe, int moving)
{
	for (int i = 0; i < 20; i++) {
		if (lg_pipe_scanline_moving(d, pipe) == moving)
			return;
	}
}

static int has_hotplug(struct lg_display *d)
{
	if (!d->gmch)
		return 1;
	if (d->ver == 2 || d->is_i915g || d->is_i915gm)
		return 0;
	return 1;
}

int lg_hpd_live(struct lg_display *d, int pin)
{
	if (d->pch != LG_PCH_NONE) {
		uint32_t isr;
		if (pin == LG_HPD_PORT_A)
			return !!(lg_rd(d, DEISR) & (d->is_ivb ? DE_DP_A_HOTPLUG_IVB : DE_DP_A_HOTPLUG));
		isr = lg_rd(d, SDEISR);
		if (d->pch == LG_PCH_IBX) {
			switch (pin) {
			case LG_HPD_PORT_B: return !!(isr & SDE_PORTB_HOTPLUG);
			case LG_HPD_PORT_C: return !!(isr & SDE_PORTC_HOTPLUG);
			case LG_HPD_PORT_D: return !!(isr & SDE_PORTD_HOTPLUG);
			case LG_HPD_CRT: return !!(isr & SDE_CRT_HOTPLUG);
			case LG_HPD_SDVO_B: return !!(isr & SDE_SDVOB_HOTPLUG);
			default: return -1;
			}
		}
		switch (pin) {
		case LG_HPD_PORT_B: return !!(isr & SDE_PORTB_HOTPLUG_CPT);
		case LG_HPD_PORT_C: return !!(isr & SDE_PORTC_HOTPLUG_CPT);
		case LG_HPD_PORT_D: return !!(isr & SDE_PORTD_HOTPLUG_CPT);
		case LG_HPD_CRT: return !!(isr & SDE_CRT_HOTPLUG_CPT);
		case LG_HPD_SDVO_B: return !!(isr & SDE_SDVOB_HOTPLUG_CPT);
		default: return -1;
		}
	}
	if (d->is_g4x || d->is_vlv || d->is_chv) {
		uint32_t st = lg_rd(d, PORT_HOTPLUG_STAT);
		switch (pin) {
		case LG_HPD_PORT_B: return !!(st & PORTB_HOTPLUG_LIVE_STATUS_G4X);
		case LG_HPD_PORT_C: return !!(st & PORTC_HOTPLUG_LIVE_STATUS_G4X);
		case LG_HPD_PORT_D: return !!(st & PORTD_HOTPLUG_LIVE_STATUS_G4X);
		default: return -1;
		}
	}
	return -1;
}

int lg_read_edid(struct lg_display *d, struct lg_output *o)
{
	struct drm_edid_info *info;
	int blocks;

	(void)d;
	o->edid_len = 0;
	o->hdmi_sink = 0;
	o->has_audio = 0;
	if (!o->ddc)
		return -ENODEV;
	blocks = drm_edid_read(o->ddc, o->edid, (int)(sizeof(o->edid) / 128));
	if (blocks <= 0)
		return -ENODEV;
	o->edid_len = blocks * 128;
	info = kalloc(sizeof(*info));
	if (info) {
		if (drm_edid_parse(o->edid, (unsigned)o->edid_len, info) == 0) {
			o->hdmi_sink = info->is_hdmi;
			o->has_audio = info->has_audio;
			if (info->mm_width && info->mm_height) {
				o->mm_width = info->mm_width;
				o->mm_height = info->mm_height;
			}
		}
		kfree(info);
	}
	return 0;
}

static uint32_t max_mode_width(struct lg_display *d)
{
	return d->ver == 2 ? 2048 : 4096;
}

int lg_get_modes_edid(struct lg_display *d, struct lg_output *o, int conn,
		      uint32_t max_clock_khz)
{
	struct drm_device *dev = d->drm;
	int n = 0;

	if (o->edid_len <= 0)
		(void)lg_read_edid(d, o);
	if (o->edid_len > 0)
		n = drm_connector_set_edid(dev, conn, o->edid, (unsigned)o->edid_len);
	if (n <= 0) {
		drm_connector_set_edid(dev, conn, NULL, 0);
		drm_connector_clear_modes(dev, conn);
	}
	if (max_clock_khz > d->max_dotclk_khz && d->max_dotclk_khz)
		max_clock_khz = d->max_dotclk_khz;
	drm_connector_add_std_modes(dev, conn, max_clock_khz, max_mode_width(d),
				    max_mode_width(d) * 9 / 16 > 2304 ? 2304 :
									 max_mode_width(d));
	return (int)dev->conn[conn].nmodes;
}

struct lg_output *lg_output_new(struct lg_display *d)
{
	struct lg_output *o;

	if (d->nout >= LG_MAX_OUTPUTS)
		return NULL;
	o = &d->out[d->nout++];
	mm_memset(o, 0, sizeof(*o));
	o->conn = -1;
	o->crtc = -1;
	o->pipe = -1;
	o->sibling = -1;
	o->port = -1;
	o->pipe_mask = (1u << d->num_pipes) - 1;
	return o;
}

static int out_index(struct lg_display *d, struct lg_output *o)
{
	return (int)(o - d->out);
}

static struct lg_output *output_for_conn(struct lg_display *d, int conn)
{
	for (int i = 0; i < d->nout; i++)
		if (d->out[i].conn == conn)
			return &d->out[i];
	return NULL;
}

/* ---- the CPU side of a pipe ---------------------------------------------------------- */

static void set_m_n(struct lg_display *d, int pipe, const struct lg_link_m_n *mn, int second)
{
	if (d->ver >= 5 || d->is_vlv || d->is_chv) {
		if (second) {
			lg_wr(d, PIPE_DATA_M2(d, pipe), TU_SIZE(mn->tu) | mn->data_m);
			lg_wr(d, PIPE_DATA_N2(d, pipe), mn->data_n);
			lg_wr(d, PIPE_LINK_M2(d, pipe), mn->link_m);
			lg_wr(d, PIPE_LINK_N2(d, pipe), mn->link_n);
			return;
		}
		lg_wr(d, PIPE_DATA_M1(d, pipe), TU_SIZE(mn->tu) | mn->data_m);
		lg_wr(d, PIPE_DATA_N1(d, pipe), mn->data_n);
		lg_wr(d, PIPE_LINK_M1(d, pipe), mn->link_m);
		lg_wr(d, PIPE_LINK_N1(d, pipe), mn->link_n);
		return;
	}
	if (second)
		return;
	lg_wr(d, PIPE_DATA_M_G4X(pipe), TU_SIZE(mn->tu) | mn->data_m);
	lg_wr(d, PIPE_DATA_N_G4X(pipe), mn->data_n);
	lg_wr(d, PIPE_LINK_M_G4X(pipe), mn->link_m);
	lg_wr(d, PIPE_LINK_N_G4X(pipe), mn->link_n);
}

static void set_timings(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	const struct lg_timings *t = &cfg->t;
	uint32_t vtotal = t->vtotal, vblank_end = t->vblank_end;
	int vsyncshift = 0;

	if (t->flags & DRM_MODE_FLAG_INTERLACE) {
		/* the hardware adds the two half lines itself */
		vtotal -= 1;
		vblank_end -= 1;
		if (lg_cfg_has(cfg, LG_OUTPUT_SDVO))
			vsyncshift = (t->htotal - 1) / 2;
		else
			vsyncshift = (int)t->hsync_start - t->htotal / 2;
		if (vsyncshift < 0)
			vsyncshift += t->htotal;
	}
	if (d->ver >= 4)
		lg_wr(d, TRANS_VSYNCSHIFT(d, pipe), (uint32_t)vsyncshift);
	lg_wr(d, TRANS_HTOTAL(d, pipe), ((uint32_t)(t->htotal - 1) << 16) | (t->hdisplay - 1u));
	lg_wr(d, TRANS_HBLANK(d, pipe),
	      ((uint32_t)(t->hblank_end - 1) << 16) | (t->hblank_start - 1u));
	lg_wr(d, TRANS_HSYNC(d, pipe),
	      ((uint32_t)(t->hsync_end - 1) << 16) | (t->hsync_start - 1u));
	lg_wr(d, TRANS_VTOTAL(d, pipe), ((vtotal - 1) << 16) | (t->vdisplay - 1u));
	lg_wr(d, TRANS_VBLANK(d, pipe), ((vblank_end - 1) << 16) | (t->vblank_start - 1u));
	lg_wr(d, TRANS_VSYNC(d, pipe),
	      ((uint32_t)(t->vsync_end - 1) << 16) | (t->vsync_start - 1u));
	lg_wr(d, PIPESRC(d, pipe), ((cfg->src_w - 1) << 16) | (cfg->src_h - 1));
}

static uint32_t bpc_bits(int pipe_bpp)
{
	switch (pipe_bpp) {
	case 18: return PIPECONF_BPC_6;
	case 30: return PIPECONF_BPC_10;
	case 36: return PIPECONF_BPC_12;
	default: return PIPECONF_BPC_8;
	}
}

static void i9xx_set_pipeconf(struct lg_display *d, const struct lg_config *cfg)
{
	uint32_t val = 0;

	/* both pipes stay on on the i830; otherwise the pipe is enabled later */
	if (d->is_i830)
		val |= PIPECONF_ENABLE;
	if (cfg->double_wide)
		val |= PIPECONF_DOUBLE_WIDE;
	if (d->is_g4x || d->is_vlv || d->is_chv) {
		if (cfg->dither && cfg->pipe_bpp != 30)
			val |= PIPECONF_DITHER_EN | PIPECONF_DITHER_TYPE_SP;
		val |= bpc_bits(cfg->pipe_bpp == 36 ? 24 : cfg->pipe_bpp);
	}
	if (cfg->t.flags & DRM_MODE_FLAG_INTERLACE) {
		if (d->ver < 4 || lg_cfg_has(cfg, LG_OUTPUT_SDVO))
			val |= PIPECONF_INTERLACE_W_FIELD_INDICATION;
		else
			val |= PIPECONF_INTERLACE_W_SYNC_SHIFT;
	} else {
		val |= PIPECONF_INTERLACE_PROGRESSIVE;
	}
	if ((d->is_vlv || d->is_chv) && cfg->limited_color_range)
		val |= PIPECONF_COLOR_RANGE_SELECT;
	val |= cfg->gamma_mode;
	lg_wr(d, PIPECONF(d, cfg->pipe), val);
	lg_posting_read(d, PIPECONF(d, cfg->pipe));
}

static void ilk_set_pipeconf(struct lg_display *d, const struct lg_config *cfg)
{
	uint32_t val = bpc_bits(cfg->pipe_bpp);

	if (cfg->dither)
		val |= PIPECONF_DITHER_EN | PIPECONF_DITHER_TYPE_SP;
	if (cfg->t.flags & DRM_MODE_FLAG_INTERLACE)
		val |= PIPECONF_INTERLACE_IF_ID_ILK;
	else
		val |= PIPECONF_INTERLACE_PF_PD_ILK;
	if (cfg->limited_color_range && !lg_cfg_has(cfg, LG_OUTPUT_SDVO))
		val |= PIPECONF_COLOR_RANGE_SELECT;
	val |= cfg->gamma_mode;
	val |= PIPECONF_MSA_TIMING_DELAY((uint32_t)cfg->msa_timing_delay & 3);
	lg_wr(d, PIPECONF(d, cfg->pipe), val);
	lg_posting_read(d, PIPECONF(d, cfg->pipe));
}

static void enable_pipe(struct lg_display *d, int pipe)
{
	uint32_t v = lg_rd(d, PIPECONF(d, pipe));

	if (!(v & PIPECONF_ENABLE)) {
		lg_wr(d, PIPECONF(d, pipe), v | PIPECONF_ENABLE);
		lg_posting_read(d, PIPECONF(d, pipe));
	}
	/* until the pipe runs, the scan line reads stale */
	wait_scanline(d, pipe, 1);
}

static void disable_pipe(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t v = lg_rd(d, PIPECONF(d, pipe));

	if (!(v & PIPECONF_ENABLE))
		return;
	/* double wide affects the planes: keep it off when unneeded */
	v &= ~PIPECONF_DOUBLE_WIDE;
	if (!d->is_i830)
		v &= ~PIPECONF_ENABLE;
	lg_wr(d, PIPECONF(d, pipe), v);
	if (v & PIPECONF_ENABLE)
		return;
	if (d->ver >= 4) {
		if (lg_wait(d, PIPECONF(d, pipe), PIPECONF_STATE_ENABLE, 0, 100000))
			kprintf("[drm] i915: pipe %c did not stop\n", 'A' + pipe);
	} else {
		wait_scanline(d, pipe, 0);
	}
}

/* ---- vblank interrupts ------------------------------------------------------------- */

static uint32_t pipestat_vblank_bit(struct lg_display *d)
{
	return d->ver >= 4 ? PIPE_START_VBLANK_INTERRUPT_STATUS : PIPE_VBLANK_INTERRUPT_STATUS;
}

static void pipestat_write(struct lg_display *d, int pipe)
{
	uint32_t status = d->pipes[pipe].pipestat_enable;
	uint32_t enable = (status << 16) & PIPESTAT_INT_ENABLE_MASK;

	enable &= ~PIPE_FIFO_UNDERRUN_STATUS;
	lg_wr(d, PIPESTAT(d, pipe), enable | status);
	lg_posting_read(d, PIPESTAT(d, pipe));
}

static void ilk_update_de_imr(struct lg_display *d, uint32_t mask, uint32_t enabled)
{
	uint64_t fl = local_irq_save();
	uint32_t imr = d->de_imr;

	imr &= ~mask;
	imr |= ~enabled & mask;
	d->de_imr = imr;
	if (d->irq_enabled) {
		lg_wr_fw(d, DEIMR, imr);
		(void)lg_rd_fw(d, DEIMR);
	}
	local_irq_restore(fl);
}

static uint32_t de_vblank_bit(struct lg_display *d, int pipe)
{
	return d->is_ivb ? DE_PIPE_VBLANK_IVB(pipe) : DE_PIPE_VBLANK(pipe);
}

static void vblank_enable(struct lg_display *d, int pipe, int on)
{
	struct lg_pipe *p = &d->pipes[pipe];

	p->vblank_enabled = on;
	if (d->gmch) {
		uint64_t fl = local_irq_save();
		if (on)
			p->pipestat_enable |= pipestat_vblank_bit(d);
		else
			p->pipestat_enable &= ~pipestat_vblank_bit(d);
		if (d->irq_enabled)
			pipestat_write(d, pipe);
		local_irq_restore(fl);
		return;
	}
	ilk_update_de_imr(d, de_vblank_bit(d, pipe), on ? de_vblank_bit(d, pipe) : 0);
}

/* ---- the enable and disable sequences --------------------------------------------- */

static void output_disable(struct lg_display *d, struct lg_output *o)
{
	const struct lg_output_funcs *f = o->funcs;
	struct lg_config *cfg = &o->cfg;
	int pipe = o->pipe;
	struct lg_pipe *p;

	if (!o->active || pipe < 0)
		return;
	p = &d->pipes[pipe];
	lg_wm_cxsr_disable(d);
	/* the planes first: they must not fetch for a pipe going away */
	lg_cursor_disable(d, pipe);
	lg_plane_disable(d, pipe);
	/* gen2 planes are double buffered and the pipe is not */
	if (d->ver == 2)
		lg_wait_for_vblank(d, pipe);
	if (f->disable)
		f->disable(d, o, cfg);
	vblank_enable(d, pipe, 0);
	disable_pipe(d, cfg);
	lg_pfit_disable(d, cfg);
	if (cfg->has_pch_encoder)
		lg_pch_disable(d, cfg);
	if (f->post_disable)
		f->post_disable(d, o, cfg);
	if (cfg->has_pch_encoder)
		lg_pch_post_disable(d, cfg);
	if (d->gmch && !lg_cfg_has(cfg, LG_OUTPUT_DSI)) {
		if (d->is_chv)
			lg_chv_disable_pll(d, pipe);
		else if (d->is_vlv)
			lg_vlv_disable_pll(d, pipe);
		else
			lg_i9xx_disable_pll(d, pipe);
	}
	if (f->post_pll_disable)
		f->post_pll_disable(d, o, cfg);
	o->active = 0;
	o->pipe = -1;
	p->active = 0;
	p->output = -1;
	p->crtc = -1;
	p->plane_enabled = 0;
	p->cursor_enabled = 0;
	p->flip_pending = 0;
	lg_wm_update(d);
	/* the i830 keeps the pipe running at 640x480 */
	if (d->is_i830)
		lg_i830_enable_pipe(d, pipe);
}

/* Light `pipe' for `o' with cfg; the plane and cursor are set afterwards. */
static int output_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *in)
{
	const struct lg_output_funcs *f = o->funcs;
	struct lg_config *cfg = &o->cfg;
	int pipe = in->pipe;
	struct lg_pipe *p = &d->pipes[pipe];

	*cfg = *in;
	o->pipe = pipe;
	p->cfg = *in;
	p->output = out_index(d, o);
	p->surf = 0;
	lg_wm_cxsr_disable(d);

	if (d->pch != LG_PCH_NONE) {
		/* Ironlake to Ivy Bridge */
		if (cfg->has_pch_encoder)
			set_m_n(d, pipe, &cfg->fdi_m_n, 0);
		else if (cfg->has_dp_encoder) {
			struct lg_link_m_n zero = { 1, 0, 0, 0, 0 };
			set_m_n(d, pipe, &cfg->dp_m_n, 0);
			set_m_n(d, pipe, &zero, 1);
		}
		set_timings(d, cfg);
		ilk_set_pipeconf(d, cfg);
		p->active = 1;
		if (f->pre_enable)
			f->pre_enable(d, o, cfg);
		if (cfg->has_pch_encoder)
			lg_fdi_pll_enable(d, cfg);
		lg_pfit_enable(d, cfg);
	} else {
		if (cfg->has_dp_encoder) {
			struct lg_link_m_n zero = { 1, 0, 0, 0, 0 };
			set_m_n(d, pipe, &cfg->dp_m_n, 0);
			if (d->is_vlv || d->is_chv)
				set_m_n(d, pipe, &zero, 1);
		}
		set_timings(d, cfg);
		i9xx_set_pipeconf(d, cfg);
		p->active = 1;
		if (d->is_vlv || d->is_chv) {
			lg_wr(d, VLV_PIPE_MSA_MISC(d, pipe), 0);
			if (d->is_chv && pipe == 1) {
				lg_wr(d, CHV_BLEND(d, pipe), CHV_BLEND_LEGACY);
				lg_wr(d, CHV_CANVAS(d, pipe), 0);
			}
			if (f->pre_pll_enable)
				f->pre_pll_enable(d, o, cfg);
			if (d->is_chv)
				lg_chv_enable_pll(d, cfg);
			else
				lg_vlv_enable_pll(d, cfg);
			if (f->pre_enable)
				f->pre_enable(d, o, cfg);
		} else {
			if (f->pre_enable)
				f->pre_enable(d, o, cfg);
			lg_i9xx_enable_pll(d, cfg);
		}
		lg_pfit_enable(d, cfg);
	}
	/* the watermarks for the planes about to come, before the pipe
	 * starts fetching */
	lg_wm_update(d);
	enable_pipe(d, pipe);
	if (cfg->has_pch_encoder)
		lg_pch_enable(d, cfg);
	vblank_enable(d, pipe, 1);
	o->active = 1;
	if (f->enable)
		f->enable(d, o, cfg);
	if (d->pch == LG_PCH_CPT)
		wait_scanline(d, pipe, 1);
	/* the PCH FIFO settles over two frames */
	if (cfg->has_pch_encoder) {
		lg_wait_for_vblank(d, pipe);
		lg_wait_for_vblank(d, pipe);
	}
	if (d->ver == 2)
		lg_wait_for_vblank(d, pipe);
	return 0;
}

/* ---- what the firmware left running ------------------------------------------------ */

/* Every pipe the firmware lit comes down once, before this code lights
 * anything: its planes, its output (as far as the output hooks can be
 * told what runs), the pipe, the fitter, the PCH side and the PLL. */
static void firmware_release(struct lg_display *d)
{
	if (g_fw_released)
		return;
	g_fw_released = 1;
	for (int pipe = 0; pipe < d->num_pipes; pipe++) {
		struct lg_config cfg;
		struct lg_output *o = NULL;

		if (d->pipes[pipe].active)
			continue;
		if (!(lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE) && !d->is_i830)
			continue;
		mm_memset(&cfg, 0, sizeof(cfg));
		cfg.pipe = pipe;
		cfg.output = -1;
		cfg.pch_dpll = -1;
		cfg.pipe_bpp = 24;
		cfg.pixel_multiplier = 1;
		for (int i = 0; i < d->nout; i++) {
			struct lg_output *c = &d->out[i];
			int hw_pipe = -1;
			if (c->active || !c->funcs->get_hw_state)
				continue;
			if (c->funcs->get_hw_state(d, c, &hw_pipe) && hw_pipe == pipe) {
				o = c;
				break;
			}
		}
		if (o) {
			cfg.output = out_index(d, o);
			cfg.output_types = 1u << o->type;
			cfg.has_dp_encoder = o->type == LG_OUTPUT_DP || o->type == LG_OUTPUT_EDP;
			if (cfg.has_dp_encoder) {
				uint32_t port = lg_rd(d, o->reg);
				cfg.lane_count = (int)((port & DP_PORT_WIDTH_MASK) >> 19) + 1;
				cfg.port_clock = 162000;
			}
		}
		if (d->pch != LG_PCH_NONE && (lg_rd(d, FDI_TX_CTL(pipe)) & FDI_TX_ENABLE)) {
			cfg.has_pch_encoder = 1;
			if (d->pch == LG_PCH_IBX) {
				cfg.pch_dpll = pipe;
			} else {
				uint32_t sel = lg_rd(d, PCH_DPLL_SEL);
				if (sel & TRANS_DPLL_ENABLE(pipe))
					cfg.pch_dpll = (sel & TRANS_DPLLB_SEL(pipe)) ? 1 : 0;
			}
		}
		i915_dbg("[drm] i915: taking down the firmware's pipe %c%s%s\n", 'A' + pipe,
			 o ? " on " : "", o ? o->name : "");
		lg_wm_cxsr_disable(d);
		lg_cursor_disable(d, pipe);
		for (int pl = 0; pl < d->num_pipes; pl++) {
			int pp;
			if (lg_plane_enabled_hw(d, pl, &pp) && pp == pipe) {
				int save = d->pipes[pipe].plane;
				d->pipes[pipe].plane = pl;
				lg_plane_disable(d, pipe);
				d->pipes[pipe].plane = save;
			}
		}
		lg_wait_for_vblank(d, pipe);
		if (o && o->funcs->disable)
			o->funcs->disable(d, o, &cfg);
		if (d->is_i830) {
			lg_i830_disable_pipe(d, pipe);
		} else {
			disable_pipe(d, &cfg);
		}
		/* the firmware's fitter, whatever it scaled */
		if (d->gmch) {
			uint32_t pf = lg_rd(d, PFIT_CONTROL);
			if ((pf & PFIT_ENABLE) &&
			    (d->ver < 4 || (pf & PFIT_PIPE_MASK) == PFIT_PIPE(pipe)))
				lg_wr(d, PFIT_CONTROL, 0);
		} else if (lg_rd(d, PF_CTL(pipe)) & PF_ENABLE) {
			lg_wr(d, PF_CTL(pipe), 0);
			lg_wr(d, PF_WIN_POS(pipe), 0);
			lg_wr(d, PF_WIN_SZ(pipe), 0);
		}
		if (cfg.has_pch_encoder)
			lg_pch_disable(d, &cfg);
		if (o && o->funcs->post_disable)
			o->funcs->post_disable(d, o, &cfg);
		if (cfg.has_pch_encoder) {
			lg_pch_post_disable(d, &cfg);
			/* nobody refcounted the firmware's PCH PLL */
			if (cfg.pch_dpll >= 0 && !d->pch_dpll[cfg.pch_dpll].active_mask) {
				lg_wr(d, PCH_DPLL(cfg.pch_dpll), 0);
				lg_posting_read(d, PCH_DPLL(cfg.pch_dpll));
				lg_udelay(200);
			}
		}
		if (d->gmch && !(o && o->type == LG_OUTPUT_DSI)) {
			if (d->is_chv)
				lg_chv_disable_pll(d, pipe);
			else if (d->is_vlv)
				lg_vlv_disable_pll(d, pipe);
			else if (!d->is_i830)
				lg_i9xx_disable_pll(d, pipe);
		}
		if (o && o->funcs->post_pll_disable)
			o->funcs->post_pll_disable(d, o, &cfg);
		if (d->is_i830)
			lg_i830_enable_pipe(d, pipe);
	}
	/* nothing of the firmware's is left to fall back to */
	d->boot.pipe = -1;
	lg_wm_update(d);
	/* Valleyview/Cherryview: CDCLK can go to its maximum with every
	 * pipe off. */
	if (d->is_vlv || d->is_chv)
		lg_clocks_init(d);
}

/* ---- configuration (the check) ---------------------------------------------------- */

static int double_wide_ok(struct lg_display *d, int pipe)
{
	/* gen2/3: pipe A, or either pipe on the 915G */
	return d->ver < 4 && (pipe == 0 || d->is_i915g);
}

/* A configuration for `o' on `pipe' showing `mode'. */
static int compute_pipe(struct lg_display *d, struct lg_output *o, int pipe,
			const struct drm_mode_modeinfo *mode, struct lg_config *cfg)
{
	int rc;

	mm_memset(cfg, 0, sizeof(*cfg));
	cfg->pipe = pipe;
	cfg->output = out_index(d, o);
	cfg->output_types = 1u << o->type;
	cfg->mode = *mode;
	cfg->src_w = mode->hdisplay;
	cfg->src_h = mode->vdisplay;
	cfg->pipe_bpp = 24;
	cfg->pixel_multiplier = 1;
	cfg->pch_dpll = -1;
	cfg->gamma_mode = PIPECONF_GAMMA_MODE_8BIT;
	if (!mode->hdisplay || !mode->vdisplay || !mode->clock)
		return -EINVAL;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) && o->type != LG_OUTPUT_TVOUT)
		return -EINVAL;
	if (mode->hdisplay > max_mode_width(d) || mode->vdisplay > max_mode_width(d))
		return -EINVAL;
	if (!o->is_panel && o->funcs->mode_valid && o->funcs->mode_valid(d, o, mode))
		return -EINVAL;
	rc = o->funcs->compute_config ? o->funcs->compute_config(d, o, cfg) : 0;
	if (rc)
		return rc;
	if (!cfg->t_set)
		lg_timings_from_mode(&cfg->t, &cfg->mode);
	if (cfg->pixel_multiplier < 1)
		cfg->pixel_multiplier = 1;
	if (!cfg->port_clock)
		cfg->port_clock = cfg->t.clock * (uint32_t)cfg->pixel_multiplier;
	rc = lg_dpll_compute(d, cfg);
	if (rc)
		return rc;
	if (!cfg->pixel_rate)
		cfg->pixel_rate = cfg->t.clock;
	/* the pixel rate against CDCLK; gen2/3 run two pixels a clock in
	 * double wide mode above 90% of it */
	if (d->ver < 4 && d->max_cdclk_khz) {
		uint32_t limit = d->max_cdclk_khz * 9 / 10;
		if (cfg->t.clock > limit && double_wide_ok(d, pipe)) {
			cfg->double_wide = 1;
			limit = d->max_dotclk_khz;
		}
		if (cfg->t.clock > limit)
			return -EINVAL;
	} else if (d->max_dotclk_khz && cfg->t.clock > d->max_dotclk_khz) {
		return -EINVAL;
	}
	/* an odd source width does not work double wide or on dual LVDS */
	if (cfg->src_w & 1) {
		if (cfg->double_wide)
			return -EINVAL;
		if (lg_cfg_has(cfg, LG_OUTPUT_LVDS) && d->lvds_dual)
			return -EINVAL;
	}
	if (cfg->has_pch_encoder) {
		rc = lg_fdi_compute_config(d, cfg);
		if (rc)
			return rc;
	}
	return 0;
}

struct plan {
	int active;
	int modeset;
	int pipe;
	struct lg_config cfg;
};

static struct plan g_plan[DRM_MAX_CRTCS];

static int crtc_needs_modeset(struct lg_display *d, struct drm_atomic_state *st, int i,
			      struct lg_output *o)
{
	struct drm_crtc_state *cs = &st->crtcs[i];
	struct lg_pipe *p;

	if (!o->active || o->pipe < 0 || o->crtc != i)
		return 1;
	if (drm_crtc_state_needs_modeset(cs))
		return 1;
	p = &d->pipes[o->pipe];
	if (cs->mode.hdisplay != p->cfg.src_w || cs->mode.vdisplay != p->cfg.src_h)
		return 1;
	return 0;
}

/* Which pipe every active CRTC runs on, and with what. */
static int plan_state(struct lg_display *d, struct drm_atomic_state *st)
{
	struct drm_device *dev = d->drm;
	int used[LG_MAX_PIPES];
	struct lg_config *cfgs[LG_MAX_PIPES];
	int rc;

	for (int p = 0; p < LG_MAX_PIPES; p++) {
		used[p] = -1;
		cfgs[p] = NULL;
	}
	/* pipes lit for load detection stay taken */
	for (int i = 0; i < d->nout; i++)
		if (d->out[i].active && d->out[i].crtc < 0 && d->out[i].pipe >= 0)
			used[d->out[i].pipe] = 1000;
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct lg_output *o = output_for_conn(d, (int)i);
		struct plan *pl = &g_plan[i];

		mm_memset(pl, 0, sizeof(*pl));
		pl->pipe = -1;
		if (!st->crtcs[i].active || !o)
			continue;
		pl->active = 1;
		pl->modeset = crtc_needs_modeset(d, st, (int)i, o);
		if (!pl->modeset) {
			pl->pipe = o->pipe;
			pl->cfg = d->pipes[o->pipe].cfg;
			used[o->pipe] = (int)i;
			cfgs[o->pipe] = &pl->cfg;
		}
	}
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct lg_output *o = output_for_conn(d, (int)i);
		struct plan *pl = &g_plan[i];
		int cand[LG_MAX_PIPES + 2], nc = 0, pipe = -1;

		if (!pl->active || !pl->modeset)
			continue;
		if (o->active && o->pipe >= 0 && o->crtc >= 0)
			cand[nc++] = o->pipe;
		if ((int)i < d->num_pipes)
			cand[nc++] = (int)i;
		for (int p = 0; p < d->num_pipes; p++)
			cand[nc++] = p;
		for (int k = 0; k < nc; k++) {
			int p = cand[k];
			if (p < 0 || p >= d->num_pipes || used[p] >= 0)
				continue;
			if (!(o->pipe_mask & (1u << p)))
				continue;
			pipe = p;
			break;
		}
		if (pipe < 0)
			return -EBUSY;
		rc = compute_pipe(d, o, pipe, &st->crtcs[i].mode, &pl->cfg);
		if (rc)
			return rc;
		pl->pipe = pipe;
		used[pipe] = (int)i;
		cfgs[pipe] = &pl->cfg;
	}
	/* outputs sharing a port: one at a time */
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct lg_output *o = output_for_conn(d, (int)i);
		if (!g_plan[i].active || !o || o->sibling < 0)
			continue;
		int sc = d->out[o->sibling].conn;
		if (sc >= 0 && sc < DRM_MAX_CRTCS && g_plan[sc].active)
			return -EINVAL;
	}
	if (d->pch != LG_PCH_NONE) {
		rc = lg_fdi_check_lanes(d, cfgs);
		if (rc)
			return rc;
	}
	/* dithering only where the pipe sends six bits a colour */
	for (int p = 0; p < d->num_pipes; p++)
		if (cfgs[p])
			cfgs[p]->dither = cfgs[p]->pipe_bpp == 18;
	if (d->pch != LG_PCH_NONE) {
		for (int p = 0; p < d->num_pipes; p++) {
			int crtc = used[p];
			if (!cfgs[p] || crtc < 0 || crtc >= DRM_MAX_CRTCS || !g_plan[crtc].modeset)
				continue;
			cfgs[p]->pch_dpll = -1;
		}
		for (int p = 0; p < d->num_pipes; p++) {
			int crtc = used[p];
			if (!cfgs[p] || crtc < 0 || crtc >= DRM_MAX_CRTCS || !g_plan[crtc].modeset)
				continue;
			rc = lg_pch_dpll_reserve(d, cfgs[p], cfgs);
			if (rc)
				return rc;
		}
	}
	/* the GMCH fitter serves one pipe */
	if (d->gmch) {
		int users = 0;
		for (int p = 0; p < d->num_pipes; p++)
			if (cfgs[p] && (cfgs[p]->gmch_pfit.control & PFIT_ENABLE))
				users++;
		if (users > 1)
			return -EINVAL;
	}
	return 0;
}

int intel_legacy_atomic_check(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct lg_display *d = lg_get();
	int rc;

	if (!d || !d->ready || d->drm != dev)
		return -ENODEV;
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		if (!cs->active)
			continue;
		if (!output_for_conn(d, (int)i))
			return -ENODEV;
		if (cs->connector_mask != (1u << i))
			return -EINVAL;
	}
	rc = plan_state(d, st);
	if (rc)
		return rc;
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct plan *pl = &g_plan[i];
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);

		if (!pl->active)
			continue;
		if (!prim || !cur)
			return -ENODEV;
		cs->adjusted_mode = pl->cfg.mode;
		cs->use_scaler = pl->cfg.src_w != pl->cfg.mode.hdisplay ||
				 pl->cfg.src_h != pl->cfg.mode.vdisplay;
		rc = lg_plane_check(d, drm_atomic_plane_state(st, prim), &pl->cfg);
		if (rc)
			return rc;
		rc = lg_cursor_check(d, drm_atomic_plane_state(st, cur));
		if (rc)
			return rc;
	}
	return 0;
}

/* ---- commit ----------------------------------------------------------------------- */

static int primary_update(struct lg_display *d, int pipe, const struct drm_plane_state *ps,
			  int full)
{
	if (!ps->fb) {
		lg_plane_disable(d, pipe);
		return 0;
	}
	if (full)
		return lg_plane_update(d, pipe, ps->fb, ps->src_x >> 16, ps->src_y >> 16);
	return lg_plane_flip(d, pipe, ps->fb, ps->src_x >> 16, ps->src_y >> 16);
}

/* What the planes of a pipe being lit will be, so that its first
 * watermarks already cover them. */
static void predict_planes(struct lg_display *d, const struct plan *pl,
			   const struct drm_plane_state *ps, const struct drm_plane_state *cps)
{
	struct lg_pipe *p = &d->pipes[pl->pipe];

	p->cfg = pl->cfg;
	p->plane_enabled = ps->fb != NULL;
	if (ps->fb) {
		p->width = pl->cfg.src_w;
		p->height = pl->cfg.src_h;
		p->format = ps->fb->format;
		p->cpp = ps->fb->bpp ? ps->fb->bpp / 8 : 4;
		p->modifier = ps->fb->modifier;
	}
	p->cursor_enabled = cps->fb != NULL;
	if (cps->fb) {
		p->cursor_w = cps->crtc_w;
		p->cursor_h = cps->crtc_h;
	}
}

int intel_legacy_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct lg_display *d = lg_get();
	int rc;

	if (!d || !d->ready || d->drm != dev)
		return -ENODEV;
	rc = plan_state(d, st);
	if (rc)
		return rc;
	/* first what goes off or is set again: it may free a pipe */
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct lg_output *o = output_for_conn(d, (int)i);
		if (!o)
			continue;
		if (!g_plan[i].active || g_plan[i].modeset) {
			if (o->active && o->crtc == (int)i)
				output_disable(d, o);
			if (!g_plan[i].active)
				o->crtc = -1;
		}
	}
	for (uint32_t i = 0; i < dev->ncrtc && i < DRM_MAX_CRTCS; i++) {
		struct plan *pl = &g_plan[i];
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct lg_output *o = output_for_conn(d, (int)i);
		struct drm_plane_state *ps, *cps;
		struct lg_pipe *p;

		if (!pl->active || !o)
			continue;
		ps = drm_atomic_plane_state(st, drm_crtc_primary(dev, (int)i));
		cps = drm_atomic_plane_state(st, drm_crtc_cursor(dev, (int)i));
		if (!pl->modeset) {
			p = &d->pipes[pl->pipe];
			if (!cs->changed)
				continue;
			if (cs->gamma_changed)
				lg_gamma_load(d, pl->pipe, cs->gamma);
			if (ps->changed) {
				int full = ps->geometry_changed || !p->plane_enabled;
				if (ps->fb && p->plane_enabled && !full &&
				    (ps->fb->pitch != p->stride || ps->fb->format != p->format ||
				     ps->fb->modifier != p->modifier))
					full = 1;
				if (full)
					lg_wm_cxsr_disable(d);
				rc = primary_update(d, pl->pipe, ps, full);
				if (rc)
					return rc;
				if (ps->fb_changed)
					p->flip_pending = 1;
				if (full)
					lg_wm_update(d);
			}
			if (cps->changed) {
				uint32_t w = p->cursor_w;
				rc = lg_cursor_update(d, pl->pipe, cps);
				if (rc)
					return rc;
				if (w != p->cursor_w)
					lg_wm_update(d);
			}
			continue;
		}
		firmware_release(d);
		if (o->active && o->crtc < 0) /* lit for load detection */
			output_disable(d, o);
		p = &d->pipes[pl->pipe];
		if (p->active && p->output >= 0 && p->output != out_index(d, o))
			output_disable(d, &d->out[p->output]);
		if (o->sibling >= 0 && d->out[o->sibling].active)
			output_disable(d, &d->out[o->sibling]);
		predict_planes(d, pl, ps, cps);
		rc = output_enable(d, o, &pl->cfg);
		if (rc)
			return rc;
		o->crtc = (int)i;
		p->crtc = (int)i;
		lg_gamma_load(d, pl->pipe, cs->gamma);
		rc = primary_update(d, pl->pipe, ps, 1);
		if (rc) {
			output_disable(d, o);
			o->crtc = -1;
			return rc;
		}
		rc = lg_cursor_update(d, pl->pipe, cps);
		if (rc)
			kprintf("[drm] i915: pipe %c: cursor not shown (%d)\n", 'A' + pl->pipe, rc);
		lg_wm_update(d);
		kprintf("[drm] i915: pipe %c on %s: %ux%u@%u (%u kHz)%s\n", 'A' + pl->pipe, o->name,
			pl->cfg.mode.hdisplay, pl->cfg.mode.vdisplay, pl->cfg.mode.vrefresh,
			pl->cfg.t.clock,
			(pl->cfg.src_w != pl->cfg.mode.hdisplay ||
			 pl->cfg.src_h != pl->cfg.mode.vdisplay) ? " (scaled)" : "");
	}
	return 0;
}

/* ---- load detection ---------------------------------------------------------------- */

int lg_load_detect_get(struct lg_display *d, struct lg_output *o,
		       const struct drm_mode_modeinfo *mode)
{
	struct lg_config cfg;
	int pipe = -1;

	if (o->active)
		return o->pipe;
	firmware_release(d);
	for (int p = d->num_pipes - 1; p >= 0; p--) {
		if (d->pipes[p].active || !(o->pipe_mask & (1u << p)))
			continue;
		pipe = p;
		break;
	}
	if (pipe < 0)
		return -EBUSY;
	if (compute_pipe(d, o, pipe, mode, &cfg))
		return -EINVAL;
	cfg.dither = cfg.pipe_bpp == 18;
	if (d->pch != LG_PCH_NONE && cfg.has_pch_encoder) {
		struct lg_config *cfgs[LG_MAX_PIPES] = { NULL, NULL, NULL };
		for (int p = 0; p < d->num_pipes; p++)
			if (d->pipes[p].active)
				cfgs[p] = &d->pipes[p].cfg;
		cfgs[pipe] = &cfg;
		if (lg_pch_dpll_reserve(d, &cfg, cfgs))
			return -EBUSY;
	}
	if (output_enable(d, o, &cfg))
		return -EIO;
	o->crtc = -1;
	d->pipes[pipe].crtc = -1;
	lg_wm_update(d);
	return pipe;
}

void lg_load_detect_release(struct lg_display *d, struct lg_output *o)
{
	if (!o->active || o->crtc >= 0)
		return;
	output_disable(d, o);
}

int lg_pipe_read_mode(struct lg_display *d, int pipe, struct drm_mode_modeinfo *m)
{
	uint32_t ht, hs, vt, vs, clock;

	if (pipe < 0 || pipe >= d->num_pipes)
		return -ENODEV;
	if (!(lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_ENABLE))
		return -ENODEV;
	mm_memset(m, 0, sizeof(*m));
	ht = lg_rd(d, TRANS_HTOTAL(d, pipe));
	hs = lg_rd(d, TRANS_HSYNC(d, pipe));
	vt = lg_rd(d, TRANS_VTOTAL(d, pipe));
	vs = lg_rd(d, TRANS_VSYNC(d, pipe));
	m->hdisplay = (uint16_t)((ht & 0xffff) + 1);
	m->htotal = (uint16_t)((ht >> 16) + 1);
	m->hsync_start = (uint16_t)((hs & 0xffff) + 1);
	m->hsync_end = (uint16_t)((hs >> 16) + 1);
	m->vdisplay = (uint16_t)((vt & 0xffff) + 1);
	m->vtotal = (uint16_t)((vt >> 16) + 1);
	m->vsync_start = (uint16_t)((vs & 0xffff) + 1);
	m->vsync_end = (uint16_t)((vs >> 16) + 1);
	clock = lg_dpll_get_port_clock(d, pipe);
	if (d->gmch && d->ver >= 4) {
		uint32_t md = lg_rd(d, DPLL_MD(d, pipe));
		uint32_t mult = ((md & DPLL_MD_UDI_MULTIPLIER_MASK) >> DPLL_MD_UDI_MULTIPLIER_SHIFT) + 1;
		if (!d->is_vlv && !d->is_chv && mult)
			clock /= mult;
	} else if (d->is_i945g || d->is_i945gm || d->is_g33 || d->is_pnv) {
		uint32_t dpll = lg_rd(d, DPLL(d, pipe));
		uint32_t mult = ((dpll & SDVO_MULTIPLIER_MASK) >> SDVO_MULTIPLIER_SHIFT_HIRES) + 1;
		if (mult && mult <= 5)
			clock /= mult;
	} else if (d->pch != LG_PCH_NONE) {
		/* a PCH port: the pixel clock from the FDI M/N */
		uint32_t lm = lg_rd(d, PIPE_LINK_M1(d, pipe));
		uint32_t ln = lg_rd(d, PIPE_LINK_N1(d, pipe));
		if (ln)
			clock = (uint32_t)((uint64_t)lg_fdi_link_freq(d) * lm / ln);
	}
	m->clock = clock;
	m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(m);
	return clock ? 0 : -ENODEV;
}

/* ---- connectors ------------------------------------------------------------------- */

int intel_legacy_detect(struct drm_device *dev, struct drm_connector *c)
{
	struct lg_display *d = lg_get();
	struct lg_output *o;

	if (!d)
		return DRM_MODE_DISCONNECTED;
	o = output_for_conn(d, (int)(c - dev->conn));
	if (!o)
		return DRM_MODE_DISCONNECTED;
	if (!o->funcs->detect) {
		o->detected = 1;
		return DRM_MODE_CONNECTED;
	}
	o->detected = o->funcs->detect(d, o) > 0;
	return o->detected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
}

/* Drop the modes nothing here can show. */
static void prune_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct drm_connector *c = &d->drm->conn[conn];
	uint32_t keep = 0;

	for (uint32_t i = 0; i < c->nmodes; i++) {
		const struct drm_mode_modeinfo *m = &c->modes[i];
		int ok = 1;
		if ((m->flags & DRM_MODE_FLAG_DBLSCAN) ||
		    ((m->flags & DRM_MODE_FLAG_INTERLACE) && o->type != LG_OUTPUT_TVOUT))
			ok = 0;
		if (m->hdisplay > max_mode_width(d) || m->vdisplay > max_mode_width(d))
			ok = 0;
		/* panels scale the rest onto their own timing */
		if (ok && !o->is_panel) {
			if (o->type != LG_OUTPUT_TVOUT && d->max_dotclk_khz &&
			    m->clock > d->max_dotclk_khz)
				ok = 0;
			if (ok && o->funcs->mode_valid && o->funcs->mode_valid(d, o, m))
				ok = 0;
		}
		if (ok) {
			if (keep != i)
				c->modes[keep] = c->modes[i];
			keep++;
		}
	}
	c->nmodes = keep;
}

int intel_legacy_get_modes(struct drm_device *dev, struct drm_connector *c)
{
	struct lg_display *d = lg_get();
	struct lg_output *o;
	int conn = (int)(c - dev->conn);

	if (!d)
		return (int)c->nmodes;
	o = output_for_conn(d, conn);
	if (!o)
		return (int)c->nmodes;
	if (o->funcs->get_modes)
		o->funcs->get_modes(d, o, conn);
	else
		lg_get_modes_edid(d, o, conn, d->max_dotclk_khz);
	prune_modes(d, o, conn);
	return (int)c->nmodes;
}

/* ---- verify and fallback ---------------------------------------------------------------- */

int intel_legacy_display_verify(struct drm_device *dev)
{
	struct lg_display *d = lg_get();

	if (!d || d->drm != dev)
		return -ENODEV;
	for (int i = 0; i < d->num_pipes; i++) {
		struct lg_pipe *p = &d->pipes[i];
		if (!p->active || p->crtc < 0)
			continue;
		uint32_t conf = lg_rd(d, PIPECONF(d, i));
		if (!(conf & PIPECONF_ENABLE) || (d->ver >= 4 && !(conf & PIPECONF_STATE_ENABLE))) {
			kprintf("[drm] i915: pipe %c not running (PIPECONF %08x)\n", 'A' + i, conf);
			return -EIO;
		}
		if (p->surf && !lg_plane_enabled_hw(d, p->plane, NULL)) {
			kprintf("[drm] i915: pipe %c: plane %c not enabled\n", 'A' + i, 'A' + p->plane);
			return -EIO;
		}
		if (!lg_pipe_scanline_moving(d, i)) {
			kprintf("[drm] i915: pipe %c: not scanning (line %u, PIPECONF %08x, DSPCNTR %08x, DPLL %08x)\n",
				'A' + i, scanline(d, i), conf, lg_rd(d, DSPCNTR(d, p->plane)),
				d->gmch ? lg_rd(d, DPLL(d, i)) : 0);
			return -EIO;
		}
		if (p->underruns)
			kprintf("[drm] i915: pipe %c: %llu FIFO underruns\n", 'A' + i,
				(unsigned long long)p->underruns);
	}
	return 0;
}

void intel_legacy_display_fallback(struct drm_device *dev)
{
	struct lg_display *d = lg_get();

	if (!d || d->drm != dev)
		return;
	if (d->boot.pipe >= 0) {
		int pl = d->boot.plane;
		lg_wr(d, DSPSTRIDE(d, pl), d->boot.dspstride);
		if (d->ver >= 4) {
			lg_wr(d, DSPLINOFF(d, pl), d->boot.dsplinoff);
			lg_wr(d, DSPTILEOFF(d, pl), d->boot.dsptileoff);
		}
		lg_wr(d, DSPCNTR(d, pl), d->boot.dspcntr);
		if (d->ver >= 4)
			lg_wr(d, DSPSURF(d, pl), d->boot.dspsurf);
		else
			lg_wr(d, DSPADDR(d, pl), d->boot.dspaddr);
		return;
	}
	for (int i = 0; i < d->num_pipes; i++)
		if (d->pipes[i].active)
			return;
	kprintf("[drm] i915: no pipe is running: the screen stays dark from here\n");
}

/* ---- interrupts ------------------------------------------------------------------- */

static uint32_t gmch_pipe_event_bit(int pipe)
{
	switch (pipe) {
	case 0: return I915_DISPLAY_PIPE_A_EVENT_INTERRUPT;
	case 1: return I915_DISPLAY_PIPE_B_EVENT_INTERRUPT;
	default: return I915_DISPLAY_PIPE_C_EVENT_INTERRUPT;
	}
}

uint32_t intel_legacy_irq_enable_bits(struct i915_device *i915, uint32_t *ier_only)
{
	struct lg_display *d = lg_get();
	uint32_t mask = 0;

	if (ier_only)
		*ier_only = 0;
	if (!d || d->i915 != i915)
		return 0;
	if (d->gmch) {
		for (int p = 0; p < d->num_pipes; p++)
			mask |= gmch_pipe_event_bit(p);
		if (has_hotplug(d))
			mask |= I915_DISPLAY_PORT_INTERRUPT;
		return mask;
	}
	if (d->is_ivb) {
		mask = DE_MASTER_IRQ_CONTROL | DE_PCH_EVENT_IVB;
		if (ier_only) {
			*ier_only = DE_ERR_INT_IVB | DE_DP_A_HOTPLUG_IVB;
			for (int p = 0; p < d->num_pipes; p++)
				*ier_only |= DE_PIPE_VBLANK_IVB(p);
		}
	} else {
		mask = DE_MASTER_IRQ_CONTROL | DE_PCH_EVENT | DE_POISON;
		if (ier_only)
			*ier_only = DE_PIPEA_VBLANK | DE_PIPEB_VBLANK | DE_DP_A_HOTPLUG |
				    DE_PIPEA_FIFO_UNDERRUN | DE_PIPEB_FIFO_UNDERRUN;
	}
	return mask;
}

static uint32_t hpd_en_bit(struct lg_display *d, int pin)
{
	(void)d;
	switch (pin) {
	case LG_HPD_CRT: return CRT_HOTPLUG_INT_EN;
	case LG_HPD_SDVO_B: return SDVOB_HOTPLUG_INT_EN;
	case LG_HPD_SDVO_C: return SDVOC_HOTPLUG_INT_EN;
	case LG_HPD_PORT_B: return PORTB_HOTPLUG_INT_EN;
	case LG_HPD_PORT_C: return PORTC_HOTPLUG_INT_EN;
	case LG_HPD_PORT_D: return PORTD_HOTPLUG_INT_EN;
	default: return 0;
	}
}

static uint32_t sde_hpd_bit(struct lg_display *d, int pin)
{
	if (d->pch == LG_PCH_IBX) {
		switch (pin) {
		case LG_HPD_CRT: return SDE_CRT_HOTPLUG;
		case LG_HPD_SDVO_B: return SDE_SDVOB_HOTPLUG;
		case LG_HPD_PORT_B: return SDE_PORTB_HOTPLUG;
		case LG_HPD_PORT_C: return SDE_PORTC_HOTPLUG;
		case LG_HPD_PORT_D: return SDE_PORTD_HOTPLUG;
		default: return 0;
		}
	}
	switch (pin) {
	case LG_HPD_CRT: return SDE_CRT_HOTPLUG_CPT;
	case LG_HPD_SDVO_B: return SDE_SDVOB_HOTPLUG_CPT;
	case LG_HPD_PORT_B: return SDE_PORTB_HOTPLUG_CPT;
	case LG_HPD_PORT_C: return SDE_PORTC_HOTPLUG_CPT;
	case LG_HPD_PORT_D: return SDE_PORTD_HOTPLUG_CPT;
	default: return 0;
	}
}

/* The hotplug pins of the outputs that have one: sense and interrupt. */
static void hpd_setup(struct lg_display *d)
{
	uint32_t pins = 0;

	for (int i = 0; i < d->nout; i++)
		if (d->out[i].hpd_pin != LG_HPD_NONE && !d->out[i].polled)
			pins |= 1u << d->out[i].hpd_pin;
	if (d->gmch) {
		uint32_t en = 0;
		if (!has_hotplug(d))
			return;
		for (int pin = 0; pin < LG_HPD_NUM; pin++)
			if (pins & (1u << pin))
				en |= hpd_en_bit(d, pin);
		/* the CRT sense parameters once: setting them provokes a
		 * spurious hotplug some seconds later */
		if (d->is_g4x)
			en |= CRT_HOTPLUG_ACTIVATION_PERIOD_64;
		en |= CRT_HOTPLUG_VOLTAGE_COMPARE_50;
		/* G45 desktop: the PEG band gap first, or the port fires
		 * without a cable */
		if (d->is_g4x && !d->is_gm45)
			lg_rmw(d, PEG_BAND_GAP_DATA, 0xf, 0xd);
		lg_rmw(d, PORT_HOTPLUG_EN,
		       HOTPLUG_INT_EN_MASK | CRT_HOTPLUG_VOLTAGE_COMPARE_MASK |
			       CRT_HOTPLUG_ACTIVATION_PERIOD_64,
		       en);
		d->hotplug_en = en;
		return;
	}
	/* the PCH pins, with the 2 ms DisplayPort short pulse */
	uint32_t sde = 0, hp_mask = 0, hp_en = 0;
	for (int pin = 0; pin < LG_HPD_NUM; pin++) {
		if (!(pins & (1u << pin)))
			continue;
		sde |= sde_hpd_bit(d, pin);
		switch (pin) {
		case LG_HPD_PORT_B:
			hp_mask |= PORTB_HOTPLUG_ENABLE | PORTB_PULSE_DURATION_MASK;
			hp_en |= PORTB_HOTPLUG_ENABLE | PORTB_PULSE_DURATION_2ms;
			break;
		case LG_HPD_PORT_C:
			hp_mask |= PORTC_HOTPLUG_ENABLE | PORTC_PULSE_DURATION_MASK;
			hp_en |= PORTC_HOTPLUG_ENABLE | PORTC_PULSE_DURATION_2ms;
			break;
		case LG_HPD_PORT_D:
			hp_mask |= PORTD_HOTPLUG_ENABLE | PORTD_PULSE_DURATION_MASK;
			hp_en |= PORTD_HOTPLUG_ENABLE | PORTD_PULSE_DURATION_2ms;
			break;
		default:
			break;
		}
	}
	d->sde_hpd_mask = sde;
	if (d->irq_enabled) {
		uint32_t all = d->pch == LG_PCH_IBX ? SDE_HOTPLUG_MASK : SDE_HOTPLUG_MASK_CPT;
		uint32_t imr = lg_rd(d, SDEIMR);
		imr = (imr & ~all) | (~sde & all);
		lg_wr(d, SDEIMR, imr);
		lg_posting_read(d, SDEIMR);
	}
	lg_rmw(d, PCH_PORT_HOTPLUG, hp_mask, hp_en);
	/* CPU port A */
	uint32_t a = DE_DP_A_HOTPLUG;
	if (d->is_ivb)
		a = DE_DP_A_HOTPLUG_IVB;
	if (pins & (1u << LG_HPD_PORT_A)) {
		lg_rmw(d, DIGITAL_PORT_HOTPLUG_CNTRL,
		       DIGITAL_PORTA_HOTPLUG_ENABLE | DIGITAL_PORTA_PULSE_DURATION_MASK,
		       DIGITAL_PORTA_HOTPLUG_ENABLE | DIGITAL_PORTA_PULSE_DURATION_2ms);
		d->de_hpd_mask = a;
	} else {
		d->de_hpd_mask = 0;
	}
	ilk_update_de_imr(d, a, d->de_hpd_mask);
}

void intel_legacy_irq_reset(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915)
		return;
	d->irq_enabled = 0;
	if (d->gmch) {
		if (has_hotplug(d)) {
			lg_rmw(d, PORT_HOTPLUG_EN, 0xffffffffu, 0);
			lg_wr(d, PORT_HOTPLUG_STAT, lg_rd(d, PORT_HOTPLUG_STAT));
		}
		for (int p = 0; p < d->num_pipes; p++)
			lg_wr(d, PIPESTAT(d, p), PIPESTAT_INT_STATUS_MASK | PIPE_FIFO_UNDERRUN_STATUS);
		return;
	}
	lg_wr(d, SDEIMR, 0xffffffffu);
	lg_wr(d, SDEIER, 0);
	lg_wr(d, SDEIIR, 0xffffffffu);
	lg_wr(d, SDEIIR, 0xffffffffu);
	if (d->is_ivb)
		lg_wr(d, GEN7_ERR_INT, 0xffffffffu);
	if (d->pch == LG_PCH_CPT)
		lg_wr(d, SERR_INT, 0xffffffffu);
}

void intel_legacy_irq_postinstall(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915)
		return;
	if (d->gmch) {
		d->irq_enabled = 1;
		for (int p = 0; p < d->num_pipes; p++)
			pipestat_write(d, p);
		hpd_setup(d);
		return;
	}
	/* the south display: everything enabled, the hotplug pins unmasked
	 * by hpd_setup (GMBUS and AUX are polled here) */
	lg_wr(d, SDEIIR, 0xffffffffu);
	lg_wr(d, SDEIMR, 0xffffffffu);
	lg_wr(d, SDEIER, 0xffffffffu);
	lg_posting_read(d, SDEIER);
	/* The north display: the enables that are always unmasked, the
	 * ones unmasked on demand (vblank, port A) enabled but masked.  The
	 * master bit in DEIER is the interrupt code's: kept, and set
	 * whenever the interrupt is installed (read inside the handler,
	 * which turns it off meanwhile, it would be lost otherwise). */
	{
		uint32_t ier_only;
		uint32_t on = intel_legacy_irq_enable_bits(i915, &ier_only);
		uint32_t master = lg_rd(d, DEIER) & DE_MASTER_IRQ_CONTROL;
		if (i915->irq_vector >= 0)
			master = DE_MASTER_IRQ_CONTROL;
		d->de_imr = ~on;
		for (int p = 0; p < d->num_pipes; p++)
			if (d->pipes[p].vblank_enabled)
				d->de_imr &= ~de_vblank_bit(d, p);
		lg_wr(d, DEIMR, d->de_imr);
		lg_wr(d, DEIIR, (on | ier_only) & ~DE_MASTER_IRQ_CONTROL);
		lg_wr(d, DEIER, ((on | ier_only) & ~DE_MASTER_IRQ_CONTROL) | master);
		lg_posting_read(d, DEIER);
	}
	d->irq_enabled = 1;
	hpd_setup(d);
}

static void hpd_note(struct lg_display *d, uint32_t pins, uint32_t long_pins)
{
	if (!pins)
		return;
	__sync_fetch_and_or(&d->hpd_pending, pins);
	__sync_fetch_and_or(&d->hpd_long, long_pins);
	d->hpd_events++;
	if (d->hpd_ready)
		poll_notify_wq(&d->hpd_wq);
}

static void vblank_event(struct lg_display *d, int pipe)
{
	struct lg_pipe *p = &d->pipes[pipe];

	p->vblanks++;
	p->flip_pending = 0;
	if (p->active && p->crtc >= 0)
		drm_vblank_tick(d->drm, p->crtc);
}

static void gmch_irq(struct lg_display *d, uint32_t iir)
{
	uint32_t stats[LG_MAX_PIPES] = { 0, 0, 0 };
	uint32_t hotplug_status = 0;

	if ((iir & I915_DISPLAY_PORT_INTERRUPT) && has_hotplug(d)) {
		uint32_t mask = (d->is_g4x || d->is_vlv || d->is_chv) ?
					HOTPLUG_INT_STATUS_G4X | DP_AUX_CHANNEL_MASK_INT_STATUS_G4X :
					HOTPLUG_INT_STATUS_I915;
		/* every pending bit has to go, or the port bit never sees
		 * another edge */
		for (int i = 0; i < 10; i++) {
			uint32_t tmp = lg_rd_fw(d, PORT_HOTPLUG_STAT) & mask;
			if (!tmp)
				break;
			hotplug_status |= tmp;
			lg_wr_fw(d, PORT_HOTPLUG_STAT, hotplug_status);
		}
	}
	/* the pipe status before the IIR: clearing them makes the edge */
	for (int p = 0; p < d->num_pipes; p++) {
		struct lg_pipe *pp = &d->pipes[p];
		uint32_t status = PIPE_FIFO_UNDERRUN_STATUS;
		if (iir & gmch_pipe_event_bit(p))
			status |= pp->pipestat_enable;
		stats[p] = lg_rd_fw(d, PIPESTAT(d, p)) & status;
		if (stats[p]) {
			uint32_t enable = (pp->pipestat_enable << 16) & PIPESTAT_INT_ENABLE_MASK &
					  ~PIPE_FIFO_UNDERRUN_STATUS;
			lg_wr_fw(d, PIPESTAT(d, p), stats[p]);
			lg_wr_fw(d, PIPESTAT(d, p), enable);
		}
	}
	for (int p = 0; p < d->num_pipes; p++) {
		if (stats[p] & pipestat_vblank_bit(d))
			vblank_event(d, p);
		if (stats[p] & PIPE_FIFO_UNDERRUN_STATUS)
			d->pipes[p].underruns++;
	}
	if (hotplug_status) {
		uint32_t pins = 0, long_pins = 0;
		int g4x = d->is_g4x || d->is_vlv || d->is_chv;
		if (hotplug_status & CRT_HOTPLUG_INT_STATUS)
			pins |= 1u << LG_HPD_CRT;
		if (hotplug_status & (g4x ? SDVOB_HOTPLUG_INT_STATUS_G4X : SDVOB_HOTPLUG_INT_STATUS_I915))
			pins |= 1u << LG_HPD_SDVO_B;
		if (hotplug_status & (g4x ? SDVOC_HOTPLUG_INT_STATUS_G4X : SDVOC_HOTPLUG_INT_STATUS_I915))
			pins |= 1u << LG_HPD_SDVO_C;
		long_pins = pins;
		if (hotplug_status & PORTB_HOTPLUG_INT_STATUS) {
			pins |= 1u << LG_HPD_PORT_B;
			if (hotplug_status & PORTB_HOTPLUG_INT_LONG_PULSE)
				long_pins |= 1u << LG_HPD_PORT_B;
		}
		if (hotplug_status & PORTC_HOTPLUG_INT_STATUS) {
			pins |= 1u << LG_HPD_PORT_C;
			if (hotplug_status & PORTC_HOTPLUG_INT_LONG_PULSE)
				long_pins |= 1u << LG_HPD_PORT_C;
		}
		if (hotplug_status & PORTD_HOTPLUG_INT_STATUS) {
			pins |= 1u << LG_HPD_PORT_D;
			if (hotplug_status & PORTD_HOTPLUG_INT_LONG_PULSE)
				long_pins |= 1u << LG_HPD_PORT_D;
		}
		hpd_note(d, pins, long_pins);
	}
}

static void pch_irq(struct lg_display *d, uint32_t pch_iir)
{
	uint32_t trigger = pch_iir & (d->pch == LG_PCH_IBX ? SDE_HOTPLUG_MASK : SDE_HOTPLUG_MASK_CPT);
	uint32_t dig = lg_rd_fw(d, PCH_PORT_HOTPLUG);
	uint32_t pins = 0, long_pins = 0;

	/* The PCH only takes the interrupt as handled once the hotplug
	 * register was written, trigger or not. */
	if (!trigger)
		dig &= ~(PORTD_HOTPLUG_STATUS_MASK | PORTC_HOTPLUG_STATUS_MASK |
			 PORTB_HOTPLUG_STATUS_MASK | (3u << 24));
	lg_wr_fw(d, PCH_PORT_HOTPLUG, dig);
	if (trigger) {
		for (int pin = 0; pin < LG_HPD_NUM; pin++) {
			uint32_t bit = sde_hpd_bit(d, pin);
			if (!bit || !(trigger & bit))
				continue;
			pins |= 1u << pin;
			switch (pin) {
			case LG_HPD_PORT_B:
				if ((dig & PORTB_HOTPLUG_STATUS_MASK) == (2u << 0))
					long_pins |= 1u << pin;
				break;
			case LG_HPD_PORT_C:
				if ((dig & PORTC_HOTPLUG_STATUS_MASK) == (2u << 8))
					long_pins |= 1u << pin;
				break;
			case LG_HPD_PORT_D:
				if ((dig & PORTD_HOTPLUG_STATUS_MASK) == (2u << 16))
					long_pins |= 1u << pin;
				break;
			default:
				long_pins |= 1u << pin;
				break;
			}
		}
		hpd_note(d, pins, long_pins);
	}
	if (d->pch == LG_PCH_CPT && (pch_iir & SDE_ERROR_CPT)) {
		uint32_t serr = lg_rd_fw(d, SERR_INT);
		for (int p = 0; p < d->num_pipes; p++)
			if (serr & SERR_INT_TRANS_FIFO_UNDERRUN(p))
				d->pipes[p].underruns++;
		lg_wr_fw(d, SERR_INT, serr);
	}
	if (d->pch == LG_PCH_IBX) {
		if (pch_iir & SDE_TRANSA_FIFO_UNDER)
			d->pipes[0].underruns++;
		if (pch_iir & SDE_TRANSB_FIFO_UNDER)
			d->pipes[1].underruns++;
	}
}

static void ilk_irq(struct lg_display *d, uint32_t de_iir)
{
	uint32_t a = d->is_ivb ? DE_DP_A_HOTPLUG_IVB : DE_DP_A_HOTPLUG;

	if (de_iir & a) {
		uint32_t dig = lg_rd_fw(d, DIGITAL_PORT_HOTPLUG_CNTRL);
		lg_wr_fw(d, DIGITAL_PORT_HOTPLUG_CNTRL, dig);
		hpd_note(d, 1u << LG_HPD_PORT_A,
			 (dig & DIGITAL_PORTA_HOTPLUG_STATUS_MASK) == DIGITAL_PORTA_HOTPLUG_LONG_DETECT ?
				 1u << LG_HPD_PORT_A : 0);
	}
	if (d->is_ivb && (de_iir & DE_ERR_INT_IVB)) {
		uint32_t err = lg_rd_fw(d, GEN7_ERR_INT);
		for (int p = 0; p < d->num_pipes; p++)
			if (err & ERR_INT_FIFO_UNDERRUN(p))
				d->pipes[p].underruns++;
		lg_wr_fw(d, GEN7_ERR_INT, err);
	}
	for (int p = 0; p < d->num_pipes; p++) {
		if (de_iir & de_vblank_bit(d, p))
			vblank_event(d, p);
		if (!d->is_ivb && (de_iir & DE_PIPE_FIFO_UNDERRUN(p)))
			d->pipes[p].underruns++;
	}
	if (de_iir & (d->is_ivb ? DE_PCH_EVENT_IVB : DE_PCH_EVENT)) {
		uint32_t pch_iir = lg_rd_fw(d, SDEIIR);
		pch_irq(d, pch_iir);
		/* the PCH event before the CPU's */
		lg_wr_fw(d, SDEIIR, pch_iir);
	}
}

void intel_legacy_display_irq(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915 || !d->irq_enabled)
		return;
	if (d->gmch) {
		/* every source is looked at: the pipe status registers and the
		 * hotplug status are cleared here, before the caller clears
		 * the IIR they feed */
		gmch_irq(d, 0xffffffffu);
		return;
	}
	uint32_t de_iir = lg_rd_fw(d, DEIIR) & ~DE_PCU_EVENT;
	if (de_iir) {
		lg_wr_fw(d, DEIIR, de_iir);
		ilk_irq(d, de_iir);
	} else if (lg_rd_fw(d, SDEIIR)) {
		/* a south event the north has not summarised yet */
		ilk_irq(d, d->is_ivb ? DE_PCH_EVENT_IVB : DE_PCH_EVENT);
	}
}

/* ---- the hotplug worker ------------------------------------------------------------ */

#define LG_HPD_DEBOUNCE_NS 150000000ULL

static uint8_t g_hpd_stack[16384] __attribute__((aligned(16)));

static void hpd_worker(void *arg)
{
	struct lg_display *d = arg;
	task_t *cur = sched_current();

	d->hpd_ready = 1;
	for (;;) {
		uint32_t pending = __sync_lock_test_and_set(&d->hpd_pending, 0);
		if (pending) {
			uint64_t rem;
			uint32_t long_pins;
			hrtimer_sleep_until(hrtimer_now_ns() + LG_HPD_DEBOUNCE_NS, &rem);
			pending |= __sync_lock_test_and_set(&d->hpd_pending, 0);
			long_pins = __sync_lock_test_and_set(&d->hpd_long, 0);
			for (int i = 0; i < d->nout; i++) {
				struct lg_output *o = &d->out[i];
				int pin = o->hpd_pin;
				if (pin == LG_HPD_NONE || !(pending & (1u << pin)))
					continue;
				int is_long = !!(long_pins & (1u << pin));
				if (o->funcs->hpd_event)
					o->funcs->hpd_event(d, o, is_long);
				if (!is_long)
					continue;
				o->detected = 0;
				o->edid_len = 0;
				if (o->conn >= 0)
					drm_connector_hotplug(d->drm, o->conn);
			}
			continue;
		}
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&d->hpd_wq, &we);
		if (d->hpd_pending) {
			local_irq_restore(fl);
			wq_remove(&d->hpd_wq, &we);
			continue;
		}
		cur->wait_channel = &d->hpd_wq;
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		cur->wait_channel = NULL;
		wq_remove(&d->hpd_wq, &we);
	}
}

int intel_legacy_hpd_start(struct i915_device *i915)
{
	struct lg_display *d = lg_get();
	int have = 0;

	if (!d || d->i915 != i915 || d->hpd_worker)
		return 0;
	for (int i = 0; i < d->nout; i++)
		if (d->out[i].hpd_pin != LG_HPD_NONE)
			have = 1;
	if (!have)
		return 0;
	d->hpd_worker = sched_add_task(hpd_worker, d, g_hpd_stack, sizeof(g_hpd_stack));
	return d->hpd_worker ? 0 : -ENOMEM;
}

/* ---- the platform ---------------------------------------------------------------- */

static int platform_init(struct lg_display *d, struct i915_device *i915)
{
	uint16_t id = i915->devid;

	d->platform = i915->info->platform;
	switch (d->platform) {
	case I915_PLATFORM_I830: d->is_i830 = 1; d->ver = 2; d->mobile = 1; d->num_pipes = 2; break;
	case I915_PLATFORM_I845G: d->is_i845 = 1; d->ver = 2; d->num_pipes = 1; break;
	case I915_PLATFORM_I85X: d->is_i85x = 1; d->ver = 2; d->mobile = 1; d->num_pipes = 2; break;
	case I915_PLATFORM_I865G: d->is_i865 = 1; d->ver = 2; d->num_pipes = 1; break;
	case I915_PLATFORM_I915G: d->is_i915g = 1; d->ver = 3; d->num_pipes = 2; break;
	case I915_PLATFORM_I915GM: d->is_i915gm = 1; d->ver = 3; d->mobile = 1; d->num_pipes = 2; break;
	case I915_PLATFORM_I945G: d->is_i945g = 1; d->ver = 3; d->num_pipes = 2; break;
	case I915_PLATFORM_I945GM: d->is_i945gm = 1; d->ver = 3; d->mobile = 1; d->num_pipes = 2; break;
	case I915_PLATFORM_G33: d->is_g33 = 1; d->ver = 3; d->num_pipes = 2; break;
	case I915_PLATFORM_PINEVIEW:
		d->is_pnv = 1;
		d->ver = 3;
		d->num_pipes = 2;
		d->mobile = id == 0xa011;
		break;
	case I915_PLATFORM_I965G: d->is_i965g = 1; d->ver = 4; d->num_pipes = 2; break;
	case I915_PLATFORM_I965GM: d->is_i965gm = 1; d->ver = 4; d->mobile = 1; d->num_pipes = 2; break;
	case I915_PLATFORM_G45: d->is_g4x = 1; d->ver = 4; d->num_pipes = 2; break;
	case I915_PLATFORM_GM45:
		d->is_g4x = 1;
		d->is_gm45 = 1;
		d->ver = 4;
		d->mobile = 1;
		d->num_pipes = 2;
		break;
	case I915_PLATFORM_IRONLAKE:
		d->is_ilk = 1;
		d->ver = 5;
		d->num_pipes = 2;
		d->pch = LG_PCH_IBX;
		d->mobile = id == 0x0046;
		break;
	case I915_PLATFORM_SANDYBRIDGE:
		d->is_snb = 1;
		d->ver = 6;
		d->num_pipes = 2;
		d->pch = LG_PCH_CPT;
		d->mobile = id == 0x0106 || id == 0x0116 || id == 0x0126;
		break;
	case I915_PLATFORM_IVYBRIDGE:
		d->is_ivb = 1;
		d->ver = 7;
		d->num_pipes = 3;
		d->pch = LG_PCH_CPT;
		d->mobile = id == 0x0156 || id == 0x0166;
		break;
	case I915_PLATFORM_VALLEYVIEW:
		d->is_vlv = 1;
		d->ver = 7;
		d->num_pipes = 2;
		d->mobile = 1;
		break;
	case I915_PLATFORM_CHERRYVIEW:
		d->is_chv = 1;
		d->ver = 8;
		d->num_pipes = 3;
		d->mobile = 1;
		break;
	default:
		return -ENODEV;
	}
	d->gmch = d->pch == LG_PCH_NONE;
	d->base = (d->is_vlv || d->is_chv) ? VLV_DISPLAY_BASE : 0;
	d->gmbus_base = d->pch != LG_PCH_NONE ? 0xc0000u : 0;
	if (d->pch != LG_PCH_NONE) {
		uint32_t fuse = lg_rd(d, FUSE_STRAP);
		if (fuse & ILK_INTERNAL_DISPLAY_DISABLE) {
			kprintf("[drm] i915: the display is fused off\n");
			return -ENODEV;
		}
		if (d->is_ivb && (fuse & IVB_PIPE_C_DISABLE))
			d->num_pipes = 2;
		if (lg_rd(d, SFUSE_STRAP) & SFUSE_STRAP_DISPLAY_DISABLED) {
			kprintf("[drm] i915: the PCH display is fused off\n");
			return -ENODEV;
		}
	}
	return 0;
}

/* The display parts of each platform's clock gating set-up. */
static void clock_gating_init(struct lg_display *d)
{
	if (d->is_g4x) {
		uint32_t gate = VRHUNIT_CLOCK_GATE_DISABLE | OVRUNIT_CLOCK_GATE_DISABLE |
				OVCUNIT_CLOCK_GATE_DISABLE;
		if (d->is_gm45)
			gate |= DSSUNIT_CLOCK_GATE_DISABLE;
		lg_wr(d, DSPCLK_GATE_D, gate);
	} else if (d->is_i965gm) {
		lg_wr(d, DSPCLK_GATE_D, 0);
		lg_wr(d, MI_ARB_STATE, LG_MASKED_ENABLE(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (d->is_i965g) {
		lg_wr(d, MI_ARB_STATE, LG_MASKED_ENABLE(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (d->ver == 3) {
		lg_wr(d, MI_ARB_STATE, LG_MASKED_ENABLE(MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (d->is_i85x) {
		lg_wr(d, MEM_MODE, LG_MASKED_ENABLE(MEM_DISPLAY_TRICKLE_FEED_DISABLE));
	} else if (d->is_i830) {
		lg_wr(d, MEM_MODE, LG_MASKED_ENABLE(MEM_DISPLAY_A_TRICKLE_FEED_DISABLE) |
					   LG_MASKED_ENABLE(MEM_DISPLAY_B_TRICKLE_FEED_DISABLE));
	} else if (d->is_ilk) {
		uint32_t gate = ILK_VRHUNIT_CLOCK_GATE_DISABLE | ILK_DPFCRUNIT_CLOCK_GATE_DISABLE |
				ILK_DPFCUNIT_CLOCK_GATE_DISABLE | ILK_DPFDUNIT_CLOCK_GATE_ENABLE |
				ILK_DPARBUNIT_CLOCK_GATE_ENABLE;
		lg_rmw(d, ILK_DISPLAY_CHICKEN2, 0, ILK_DPARB_GATE | ILK_VSDPFD_FULL);
		lg_rmw(d, DISP_ARB_CTL, 0, DISP_FBC_WM_DIS);
		if (d->mobile) {
			lg_rmw(d, ILK_DISPLAY_CHICKEN1, 0, ILK_FBCQ_DIS);
			lg_rmw(d, ILK_DISPLAY_CHICKEN2, 0, ILK_DPARB_GATE);
		}
		lg_wr(d, ILK_DSPCLK_GATE_D, gate);
		lg_rmw(d, ILK_DISPLAY_CHICKEN2, 0, ILK_ELPIN_409_SELECT);
	} else if (d->is_snb) {
		lg_wr(d, ILK_DSPCLK_GATE_D, ILK_VRHUNIT_CLOCK_GATE_DISABLE);
		lg_rmw(d, ILK_DISPLAY_CHICKEN2, 0, ILK_ELPIN_409_SELECT);
		lg_rmw(d, ILK_DISPLAY_CHICKEN1, 0, ILK_FBCQ_DIS | ILK_PABSTRETCH_DIS);
		lg_rmw(d, ILK_DISPLAY_CHICKEN2, 0, ILK_DPARB_GATE | ILK_VSDPFD_FULL);
		lg_rmw(d, ILK_DSPCLK_GATE_D, 0,
		       ILK_DPARBUNIT_CLOCK_GATE_ENABLE | ILK_DPFDUNIT_CLOCK_GATE_ENABLE);
	} else if (d->is_ivb) {
		lg_wr(d, ILK_DSPCLK_GATE_D, ILK_VRHUNIT_CLOCK_GATE_DISABLE);
		lg_rmw(d, ILK_DISPLAY_CHICKEN1, 0, ILK_FBCQ_DIS);
	} else if (d->is_vlv) {
		/* GCFG clock gating delays the vblank reports */
		lg_wr(d, LGD_VLV_GUNIT_CLOCK_GATE, LGD_GCFG_DIS);
	}
	/* trickle feed off on every plane (and the surface latched again) */
	if (d->is_g4x || d->is_ilk || d->is_snb || d->is_ivb) {
		for (int p = 0; p < d->num_pipes; p++) {
			lg_rmw(d, DSPCNTR(d, p), 0, DISP_TRICKLE_FEED_DISABLE);
			lg_wr(d, DSPSURF(d, p), lg_rd(d, DSPSURF(d, p)));
			lg_posting_read(d, DSPSURF(d, p));
		}
	}
	/* the PCH: the panel sequencer's clock must run with no port up */
	if (d->pch == LG_PCH_IBX) {
		lg_wr(d, SOUTH_DSPCLK_GATE_D, PCH_DPLSUNIT_CLOCK_GATE_DISABLE);
	} else if (d->pch == LG_PCH_CPT) {
		lg_wr(d, SOUTH_DSPCLK_GATE_D, PCH_DPLSUNIT_CLOCK_GATE_DISABLE |
						      PCH_DPLUNIT_CLOCK_GATE_DISABLE |
						      PCH_CPUNIT_CLOCK_GATE_DISABLE);
		lg_rmw(d, SOUTH_CHICKEN2, 0, DPLS_EDP_PPS_FIX_DIS);
		/* a few pixels shifted down on some LVDS panels otherwise */
		for (int p = 0; p < d->num_pipes; p++) {
			uint32_t v = lg_rd(d, TRANS_CHICKEN2(p));
			v |= TRANS_CHICKEN2_TIMING_OVERRIDE;
			v &= ~TRANS_CHICKEN2_FDI_POLARITY_REVERSED;
			if (d->vbt.fdi_rx_polarity_inverted)
				v |= TRANS_CHICKEN2_FDI_POLARITY_REVERSED;
			v &= ~(TRANS_CHICKEN2_DISABLE_DEEP_COLOR_COUNTER |
			       TRANS_CHICKEN2_DISABLE_DEEP_COLOR_MODESWITCH);
			lg_wr(d, TRANS_CHICKEN2(p), v);
		}
		for (int p = 0; p < d->num_pipes; p++)
			lg_wr(d, TRANS_CHICKEN1(p), TRANS_CHICKEN1_DP0UNIT_GC_DISABLE);
	}
}

/* What the firmware shows, for the fallback. */
static void boot_plane_record(struct lg_display *d)
{
	d->boot.pipe = -1;
	d->boot.plane = -1;
	for (int pl = 0; pl < d->num_pipes; pl++) {
		int pipe;
		if (!lg_plane_enabled_hw(d, pl, &pipe))
			continue;
		d->boot.pipe = pipe;
		d->boot.plane = pl;
		d->boot.dspcntr = lg_rd(d, DSPCNTR(d, pl));
		d->boot.dspstride = lg_rd(d, DSPSTRIDE(d, pl));
		if (d->ver >= 4) {
			d->boot.dspsurf = lg_rd(d, DSPSURF(d, pl));
			d->boot.dsplinoff = lg_rd(d, DSPLINOFF(d, pl));
			d->boot.dsptileoff = lg_rd(d, DSPTILEOFF(d, pl));
		} else {
			d->boot.dspaddr = lg_rd(d, DSPADDR(d, pl));
		}
		break;
	}
}

/* Which outputs exist: the ports' strap bits, the VBT and probes. */
static void setup_outputs(struct lg_display *d)
{
	if (d->pch != LG_PCH_NONE) {
		int found;
		int dpd_is_edp = lg_vbt_port_is_edp(d, LG_PORT_D);

		/* the LVDS first: eDP and LVDS share the one sequencer */
		lg_lvds_init(d);
		lg_crt_init(d);
		if (!(lg_rd(d, FUSE_STRAP) & ILK_eDP_A_DISABLE) && (lg_rd(d, DP_A) & DP_DETECTED))
			lg_dp_init(d, DP_A, LG_PORT_A);
		if (lg_rd(d, PCH_HDMIB) & SDVO_DETECTED) {
			/* PCH SDVO B shares its port with HDMI B */
			found = lg_sdvo_init(d, PCH_SDVOB, LG_PORT_B);
			if (!found)
				lg_hdmi_init(d, PCH_HDMIB, LG_PORT_B);
			if (!found && (lg_rd(d, PCH_DP_B) & DP_DETECTED))
				lg_dp_init(d, PCH_DP_B, LG_PORT_B);
		}
		if (lg_rd(d, PCH_HDMIC) & SDVO_DETECTED)
			lg_hdmi_init(d, PCH_HDMIC, LG_PORT_C);
		if (!dpd_is_edp && (lg_rd(d, PCH_HDMID) & SDVO_DETECTED))
			lg_hdmi_init(d, PCH_HDMID, LG_PORT_D);
		if (lg_rd(d, PCH_DP_C) & DP_DETECTED)
			lg_dp_init(d, PCH_DP_C, LG_PORT_C);
		if (lg_rd(d, PCH_DP_D) & DP_DETECTED)
			lg_dp_init(d, PCH_DP_D, LG_PORT_D);
	} else if (d->is_vlv || d->is_chv) {
		int has_edp, has_port;

		if (d->is_vlv && d->vbt.int_crt_support)
			lg_crt_init(d);
		/* The DP_DETECTED strap may be missing for eDP (its DDC pins
		 * do something else) and even for HDMI: ask the VBT too. */
		has_edp = lg_vbt_port_is_edp(d, LG_PORT_B);
		has_port = lg_vbt_port_present(d, LG_PORT_B);
		if ((lg_rd(d, DP_B) & DP_DETECTED) || has_port)
			has_edp &= lg_dp_init(d, DP_B, LG_PORT_B);
		if (((lg_rd(d, GEN3_SDVOB) & SDVO_DETECTED) || has_port) && !has_edp)
			lg_hdmi_init(d, GEN3_SDVOB, LG_PORT_B);
		has_edp = lg_vbt_port_is_edp(d, LG_PORT_C);
		has_port = lg_vbt_port_present(d, LG_PORT_C);
		if ((lg_rd(d, DP_C) & DP_DETECTED) || has_port)
			has_edp &= lg_dp_init(d, DP_C, LG_PORT_C);
		if (((lg_rd(d, GEN3_SDVOC) & SDVO_DETECTED) || has_port) && !has_edp)
			lg_hdmi_init(d, GEN3_SDVOC, LG_PORT_C);
		if (d->is_chv) {
			has_port = lg_vbt_port_present(d, LG_PORT_D);
			if ((lg_rd(d, DP_D) & DP_DETECTED) || has_port)
				lg_dp_init(d, DP_D, LG_PORT_D);
			if ((lg_rd(d, CHV_HDMID) & SDVO_DETECTED) || has_port)
				lg_hdmi_init(d, CHV_HDMID, LG_PORT_D);
		}
		lg_dsi_init(d);
	} else if (d->is_pnv) {
		lg_lvds_init(d);
		lg_crt_init(d);
	} else if (d->ver == 3 || d->ver == 4) {
		int found = 0;

		if (d->mobile)
			lg_lvds_init(d);
		lg_crt_init(d);
		if (lg_rd(d, GEN3_SDVOB) & SDVO_DETECTED) {
			found = lg_sdvo_init(d, GEN3_SDVOB, LG_PORT_B);
			if (!found && d->is_g4x)
				lg_hdmi_init(d, GEN4_HDMIB, LG_PORT_B);
			if (!found && d->is_g4x)
				lg_dp_init(d, DP_B, LG_PORT_B);
		}
		/* before G4X SDVO C has no detect bit of its own */
		if (lg_rd(d, GEN3_SDVOB) & SDVO_DETECTED)
			found = lg_sdvo_init(d, GEN3_SDVOC, LG_PORT_C);
		if (!found && (lg_rd(d, GEN3_SDVOC) & SDVO_DETECTED)) {
			if (d->is_g4x) {
				lg_hdmi_init(d, GEN4_HDMIC, LG_PORT_C);
				lg_dp_init(d, DP_C, LG_PORT_C);
			}
		}
		if (d->is_g4x && (lg_rd(d, DP_D) & DP_DETECTED))
			lg_dp_init(d, DP_D, LG_PORT_D);
		if (d->is_i915gm || d->is_i945gm || d->is_i965gm || d->is_gm45)
			lg_tv_init(d);
	} else {
		if (d->is_i85x)
			lg_lvds_init(d);
		lg_crt_init(d);
		lg_dvo_init(d);
	}
	/* DP and HDMI on one port: one connector's picture at a time */
	for (int i = 0; i < d->nout; i++) {
		struct lg_output *a = &d->out[i];
		if (a->type != LG_OUTPUT_DP && a->type != LG_OUTPUT_HDMI)
			continue;
		for (int j = i + 1; j < d->nout; j++) {
			struct lg_output *b = &d->out[j];
			if ((b->type == LG_OUTPUT_DP || b->type == LG_OUTPUT_HDMI) &&
			    b->type != a->type && b->port == a->port) {
				a->sibling = j;
				b->sibling = i;
			}
		}
	}
}

static int output_rank(struct lg_output *o)
{
	if (o->is_panel)
		return 0;
	if (o->detected)
		return 1;
	return 2;
}

static void add_connectors(struct lg_display *d)
{
	struct drm_device *dev = d->drm;

	for (int i = 0; i < d->nout; i++) {
		struct lg_output *o = &d->out[i];
		o->detected = o->funcs->detect ? o->funcs->detect(d, o) > 0 : 1;
	}
	/* panels first, then what is plugged in: the console takes the
	 * first connector's mode */
	for (int rank = 0; rank <= 2; rank++) {
		for (int i = 0; i < d->nout; i++) {
			struct lg_output *o = &d->out[i];
			if (o->conn >= 0 || output_rank(o) != rank)
				continue;
			int conn = drm_connector_add(dev, o->conn_type, o->mm_width, o->mm_height);
			if (conn < 0)
				return;
			o->conn = conn;
			dev->enc[conn].type = o->enc_type;
			dev->conn[conn].priv = o;
			dev->conn[conn].connected = o->detected;
			if (o->detected)
				intel_legacy_get_modes(dev, &dev->conn[conn]);
			kprintf("[drm] i915: connector %d: %s, %s, %u modes\n", conn, o->name,
				o->detected ? "connected" : "disconnected", dev->conn[conn].nmodes);
		}
	}
}

/* ---- init, sleep, teardown ---------------------------------------------------------- */

static void hw_init(struct lg_display *d)
{
	if (d->is_vlv || d->is_chv)
		lg_vlv_power_init(d);
	lg_clocks_init(d);
	clock_gating_init(d);
	lg_vga_disable(d);
	lg_pps_init(d);
	lg_gmbus_init(d);
	if (d->pch != LG_PCH_NONE)
		lg_fdi_init(d);
	lg_wm_init(d);
}

int intel_legacy_display_init(struct i915_device *i915)
{
	struct lg_display *d = &g_lg;
	struct drm_device *dev = &i915->drm;

	mm_memset(d, 0, sizeof(*d));
	d->i915 = i915;
	d->drm = dev;
	d->boot.pipe = -1;
	if (platform_init(d, i915))
		return -ENODEV;
	for (int p = 0; p < LG_MAX_PIPES; p++) {
		d->pipes[p].pipe = p;
		d->pipes[p].output = -1;
		d->pipes[p].crtc = -1;
	}
	wq_head_init(&d->hpd_wq, "i915_lg_hpd");
	g_lg_active = 1;
	g_fw_released = 0;

	intel_opregion_init(i915);
	lg_vbt_init(d);
	/* the panel's spread spectrum as the VBT has it, until the LVDS
	 * code decides otherwise */
	d->lvds_use_ssc = d->vbt.lvds_use_ssc;
	d->lvds_ssc_khz = d->vbt.lvds_ssc_freq_khz;
	boot_plane_record(d);
	hw_init(d);
	lg_plane_init(d);
	setup_outputs(d);
	if (d->pch != LG_PCH_NONE)
		lg_pch_refclk_init(d);
	if (!d->nout) {
		kprintf("[drm] i915: %s: no display outputs found\n", i915->info->name);
		g_lg_active = 0;
		return -ENODEV;
	}
	/* the display's interrupt sources and the hotplug sense (the live
	 * status of the digital ports depends on it); the top level owns
	 * the IIR/IMR/IER (and the masters) above them */
	intel_legacy_irq_postinstall(i915);
	dev->refresh_hz = 60;
	dev->min_width = 320;
	dev->min_height = 200;
	dev->max_width = max_mode_width(d);
	dev->max_height = max_mode_width(d);
	d->ready = 1;
	/* the common interrupt code hands display interrupts on while the
	 * display says it is up */
	i915->display.ready = 1;
	add_connectors(d);
	intel_opregion_driver_ready(i915);
	kprintf("[drm] i915: %s display: %d pipes, %d outputs, CDCLK %u MHz\n", i915->info->name,
		d->num_pipes, d->nout, d->cdclk_khz / 1000);
	return 0;
}

void intel_legacy_display_suspend(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915 || !d->ready)
		return;
	for (int i = 0; i < d->nout; i++) {
		struct lg_output *o = &d->out[i];
		if (o->active)
			output_disable(d, o);
		o->crtc = -1;
	}
	for (int i = 0; i < d->nout; i++)
		if (d->out[i].funcs->suspend)
			d->out[i].funcs->suspend(d, &d->out[i]);
}

int intel_legacy_display_resume(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915 || !d->ready)
		return 0;
	/* nothing of the firmware's runs any more */
	g_fw_released = 1;
	d->boot.pipe = -1;
	hw_init(d);
	if (d->pch != LG_PCH_NONE)
		lg_pch_refclk_init(d);
	for (int p = 0; p < LG_MAX_PIPES; p++) {
		d->pipes[p].active = 0;
		d->pipes[p].output = -1;
		d->pipes[p].crtc = -1;
		d->pipes[p].plane_enabled = 0;
		d->pipes[p].cursor_enabled = 0;
	}
	for (int i = 0; i < 2; i++) {
		d->pch_dpll[i].pipe_mask = 0;
		d->pch_dpll[i].active_mask = 0;
	}
	for (int i = 0; i < d->nout; i++) {
		struct lg_output *o = &d->out[i];
		o->active = 0;
		o->pipe = -1;
		o->detected = 0;
		o->edid_len = 0;
		if (o->funcs->resume)
			o->funcs->resume(d, o);
	}
	intel_legacy_irq_postinstall(i915);
	return 0;
}

/* Gen2-4 (G4x excepted): a reset of the GT resets the display with it.
 * The outputs go off first, the way a suspend takes them down; after the
 * reset the display is set up again the way a resume does it, and the
 * committed state is shown again. */
void intel_legacy_display_reset_prepare(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915 || !d->ready || i915->drm.suspended)
		return;
	intel_legacy_display_suspend(i915);
	g_reset_pending = 1;
}

void intel_legacy_display_reset_finish(struct i915_device *i915)
{
	struct lg_display *d = lg_get();
	int rc;

	if (!d || d->i915 != i915 || !g_reset_pending)
		return;
	g_reset_pending = 0;
	rc = intel_legacy_display_resume(i915);
	if (rc == 0)
		rc = drm_atomic_replay(&i915->drm);
	if (rc)
		kprintf("[drm] i915: the display did not come back after the GPU reset (%d)\n", rc);
}

void intel_legacy_display_fini(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915)
		return;
	d->hpd_ready = 0;
	for (int i = 0; i < d->nout; i++)
		output_disable(d, &d->out[i]);
	if (d->is_vlv || d->is_chv)
		lg_vlv_power_fini(d);
	lg_vbt_fini(d);
	intel_opregion_fini(i915);
	d->ready = 0;
	i915->display.ready = 0;
	g_lg_active = 0;
}

uint32_t intel_legacy_cursor_max(struct i915_device *i915)
{
	struct lg_display *d = lg_get();

	if (!d || d->i915 != i915)
		return 64;
	return lg_cursor_max_size(d);
}
