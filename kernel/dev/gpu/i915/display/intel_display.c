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
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>

static struct intel_output *output_for_crtc(struct i915_device *i915, int crtc)
{
	struct intel_display *d = &i915->display;
	for (int i = 0; i < d->nout; i++)
		if (d->outputs[i].conn == crtc)
			return &d->outputs[i];
	return NULL;
}

static struct intel_output *output_for_conn(struct i915_device *i915,
					    struct drm_connector *c)
{
	int idx = (int)(c - i915->drm.conn);
	return output_for_crtc(i915, idx);
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
	if (i915->display.model == INTEL_DISPLAY_TGL && port >= PORT_TC1 &&
	    port - PORT_TC1 < 6)
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
	if (i915->display.model == INTEL_DISPLAY_TGL) {
		int t = o->pipe >= 0 ? i915->display.pipes[o->pipe].transcoder : 0;
		return TGL_DP_TP_CTL(t);
	}
	return DP_TP_CTL(o->port);
}

uint32_t intel_dp_tp_status_reg(struct i915_device *i915, const struct intel_output *o)
{
	if (i915->display.model == INTEL_DISPLAY_TGL) {
		int t = o->pipe >= 0 ? i915->display.pipes[o->pipe].transcoder : 0;
		return TGL_DP_TP_STATUS(t);
	}
	return DP_TP_STATUS(o->port);
}

/* The transcoder's port field and clock select, which Tiger Lake
 * re-encoded (four bits, port + 1). */
static uint32_t trans_ddi_select_port(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_TGL)
		return TGL_TRANS_DDI_SELECT_PORT((uint32_t)port);
	return TRANS_DDI_SELECT_PORT((uint32_t)port);
}

static uint32_t trans_ddi_port_mask(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_TGL ? TGL_TRANS_DDI_PORT_MASK :
							   TRANS_DDI_PORT_MASK;
}

static int trans_ddi_port_of(struct i915_device *i915, uint32_t func)
{
	if (i915->display.model == INTEL_DISPLAY_TGL)
		return (int)((func & TGL_TRANS_DDI_PORT_MASK) >> TGL_TRANS_DDI_PORT_SHIFT) - 1;
	return (int)((func & TRANS_DDI_PORT_MASK) >> TRANS_DDI_PORT_SHIFT);
}

static uint32_t trans_clk_sel_port(struct i915_device *i915, int port)
{
	if (i915->display.model == INTEL_DISPLAY_TGL)
		return TGL_TRANS_CLK_SEL_PORT((uint32_t)port);
	return TRANS_CLK_SEL_PORT((uint32_t)port);
}

/* Port A's transcoder: the embedded-panel one where there is one. */
static int transcoder_for(struct i915_device *i915, int port, int pipe)
{
	if (port == PORT_A && i915->display.model != INTEL_DISPLAY_TGL)
		return TRANSCODER_EDP;
	return pipe;
}

/* ---- objects in the GGTT ------------------------------------------------- */

static int bo_bind(struct i915_device *i915, struct drm_gem_object *o)
{
	struct i915_bo *bo = o->priv;
	if (!bo)
		return -EINVAL;
	/* the display reads memory, not the processor's cache */
	i915_gem_object_set_display(i915, o);
	if (bo->bound)
		return 0;
	int rc = i915_ggtt_bind_scanout(i915, o, &bo->ggtt);
	if (rc)
		return rc;
	bo->bound = 1;
	return 0;
}

/* ---- watermarks and the data buffer ---------------------------------------- */

static uint32_t div_round_up(uint64_t a, uint64_t b)
{
	return (uint32_t)((a + b - 1) / b);
}

/* Said a few times, early: what a client puts on the screen and what the
 * plane was given for it.  A picture that is structured but wrong is
 * almost always one of these two disagreeing. */
static int g_fb_logged;
static int g_plane_logged;

/* The watermarks: how much of the display's data buffer a plane may
 * fill ahead of the beam, and how many scan lines that is worth.
 *
 * The arithmetic is defined in 16.16 fixed point and differs for a
 * TILED surface, which the engine fetches in whole tile rows: a tiled
 * plane needs several scan lines' worth of data buffered where a linear
 * one needs a fraction of a line.  Computing a tiled surface with the
 * linear formula asks the engine to start scanning with a few blocks
 * buffered instead of a hundred; it runs dry every frame, and what
 * reaches the panel is whatever the buffer held -- black, stripes, a
 * torn frame when the machine happens to be quiet.  The console's own
 * framebuffer is linear, which is why the console looked right and the
 * display server did not.
 */
#define WM_FP(x) ((uint64_t)(x) << 16)

static uint32_t wm_round_up(uint64_t fp)
{
	return (uint32_t)((fp + 0xffff) >> 16);
}

/* How many scan lines of a tiled surface the engine fetches at once. */
static uint32_t y_min_scanlines_for(int cpp)
{
	switch (cpp) {
	case 8: return 2;
	case 4: return 4;
	case 2: return 8;
	default: return 16;
	}
}

struct skl_wm {
	uint32_t blocks;
	uint32_t lines;
	int valid;
};

static struct skl_wm skl_wm_level(uint32_t latency_us, uint32_t pixel_rate_khz,
				  uint32_t htotal, uint32_t width, int cpp,
				  int y_tiled, int x_tiled)
{
	struct skl_wm wm = { 0, 0, 0 };
	uint32_t bytes_per_line = width * (uint32_t)cpp;
	uint64_t blocks_per_line;
	uint64_t y_tile_minimum = 0;
	uint32_t y_min = y_min_scanlines_for(cpp);

	if (!latency_us || !pixel_rate_khz || !htotal)
		return wm;
	if (y_tiled) {
		uint32_t interm = div_round_up((uint64_t)bytes_per_line * y_min, 512);
		blocks_per_line = WM_FP(interm) / y_min;
		y_tile_minimum = (uint64_t)y_min * blocks_per_line;
	} else {
		/* A surface tiled the other way is fetched by rows of tiles
		 * too, but its tiles are as wide as the buffer's own blocks,
		 * so a line of it is exactly its blocks -- with none of the
		 * rounding a linear surface needs. */
		uint32_t interm = div_round_up(bytes_per_line, 512);
		if (!x_tiled)
			interm++;
		blocks_per_line = WM_FP(interm);
	}
	/* the data fetched during the memory latency, two ways */
	uint64_t method1 = (WM_FP(1) * latency_us * pixel_rate_khz * (uint32_t)cpp) /
			   (1000ULL * 512ULL);
	uint64_t method2 = (uint64_t)div_round_up((uint64_t)latency_us * pixel_rate_khz,
						  (uint64_t)htotal * 1000) *
			   blocks_per_line;
	uint32_t linetime_us = (uint32_t)div_round_up((uint64_t)htotal * 1000, pixel_rate_khz);
	uint64_t selected;

	if (y_tiled) {
		selected = method2 > y_tile_minimum ? method2 : y_tile_minimum;
	} else if (latency_us >= linetime_us) {
		selected = method1 < method2 ? method1 : method2;
	} else {
		selected = method1;
	}
	wm.blocks = wm_round_up(selected) + 1;
	wm.lines = (uint32_t)((selected + blocks_per_line - 1) / blocks_per_line);
	if (y_tiled) {
		wm.blocks += wm_round_up(y_tile_minimum);
		wm.lines += y_min;
	}
	/* At least a whole line's worth, whatever the arithmetic says: the
	 * engine begins a line by fetching one, and starting it with less
	 * buffered is how a plane runs dry mid-line. */
	if (wm.blocks < wm_round_up(blocks_per_line))
		wm.blocks = wm_round_up(blocks_per_line);
	if (wm.lines < 1)
		wm.lines = 1;
	wm.valid = wm.lines <= 31;
	return wm;
}

/* Broadwell keeps the older plane registers (DSPCNTR and friends) and
 * the older watermarks: one level per pipe, a FIFO fill in 64-byte
 * units that the plane must have buffered to cover the memory latency. */
static int legacy_plane(struct i915_device *i915)
{
	return i915->info->display_ver < 9;
}

static void bdw_program_wm(struct i915_device *i915, struct intel_pipe *p,
			   uint32_t pixel_rate_khz, uint32_t htotal, int cpp)
{
	struct intel_display *d = &i915->display;
	uint32_t latency = d->mem_latency[0] ? d->mem_latency[0] : 2;
	uint32_t cursor_w = p->cursor_w ? p->cursor_w : 64;
	/* method 1: bytes fetched during the latency, in 64-byte units */
	uint32_t pri = (pixel_rate_khz * (uint32_t)cpp * latency + 64000 - 1) / 64000 + 2;
	uint32_t cur = (pixel_rate_khz * 4 * latency + 64000 - 1) / 64000 + 2;
	(void)cursor_w;
	if (pri > 255)
		pri = 255;
	if (cur > 63)
		cur = 63;
	/* only the first level: the low-power levels stay off */
	i915_write32(i915, WM1_LP_ILK, 0);
	i915_write32(i915, WM2_LP_ILK, 0);
	i915_write32(i915, WM3_LP_ILK, 0);
	i915_write32(i915, WM0_PIPE_ILK(p->pipe), (pri << 16) | cur);
	if (htotal && pixel_rate_khz) {
		uint32_t linetime = (htotal * 1000u * 8u + pixel_rate_khz / 2) / pixel_rate_khz;
		i915_write32(i915, PIPE_WM_LINETIME(p->pipe), linetime & 0x1ff);
	}
	if (g_plane_logged < 8)
		i915_dbg("[drm] i915: watermarks %c: plane %u, cursor %u (64-byte units), latency %u us\n",
			 'A' + p->pipe, pri, cur, latency);
}

static void bdw_plane_program(struct i915_device *i915, struct intel_pipe *p,
			      uint32_t ggtt, uint32_t pitch, uint32_t format,
			      uint64_t modifier)
{
	uint32_t ctl = DISP_ENABLE | DISP_PIPE_GAMMA_ENABLE;
	int cpp = (format == DRM_FORMAT_RGB565) ? 2 : 4;
	switch (format) {
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		ctl |= DISP_FORMAT_RGBX888;
		break;
	case DRM_FORMAT_XRGB2101010:
		ctl |= DISP_FORMAT_BGRX101010;
		break;
	case DRM_FORMAT_XBGR2101010:
		ctl |= DISP_FORMAT_RGBX101010;
		break;
	case DRM_FORMAT_RGB565:
		ctl |= DISP_FORMAT_BGRX565;
		break;
	default:
		ctl |= DISP_FORMAT_BGRX888;
		break;
	}
	if (modifier == I915_FORMAT_MOD_X_TILED)
		ctl |= DISP_TILED;
	i915_write32(i915, DSPSTRIDE(p->pipe), pitch);
	if (modifier == I915_FORMAT_MOD_X_TILED) {
		i915_write32(i915, DSPTILEOFF(p->pipe), (p->off_y << 16) | p->off_x);
		i915_write32(i915, DSPLINOFF(p->pipe), 0);
	} else {
		i915_write32(i915, DSPTILEOFF(p->pipe), 0);
		i915_write32(i915, DSPLINOFF(p->pipe), p->off_y * pitch + p->off_x * (uint32_t)cpp);
	}
	i915_write32(i915, DSPCNTR(p->pipe), ctl);
	/* the surface write latches the rest */
	i915_write32(i915, DSPSURF(p->pipe), ggtt);
	(void)i915_read32(i915, DSPSURF(p->pipe));
}

static void skl_program_wm(struct i915_device *i915, struct intel_pipe *p,
			   uint32_t pixel_rate_khz, uint32_t width,
			   uint32_t htotal, int cpp, uint64_t modifier)
{
	struct intel_display *d = &i915->display;
	if (legacy_plane(i915)) {
		(void)width;
		(void)modifier;
		bdw_program_wm(i915, p, pixel_rate_khz, htotal, cpp);
		return;
	}
	uint32_t latency = d->mem_latency[0] ? d->mem_latency[0] : 2;
	int y_tiled = (modifier == I915_FORMAT_MOD_Y_TILED);
	int x_tiled = (modifier == I915_FORMAT_MOD_X_TILED);
	/* The data buffer of one slice, less the blocks the display keeps
	 * for its own bypass path on this generation.  The cursor takes a
	 * slice at the end, the primary plane the rest. */
	uint32_t ddb_size = d->ddb_blocks;
	uint32_t cursor_w = p->cursor_w ? p->cursor_w : 64;
	struct skl_wm cursor = skl_wm_level(latency, pixel_rate_khz, htotal, cursor_w, 4,
					    0, 0);
	uint32_t ddb_cursor = cursor.blocks + 2;
	uint32_t ddb_plane;
	struct skl_wm plane = skl_wm_level(latency, pixel_rate_khz, htotal, width, cpp,
					   y_tiled, x_tiled);

	if (ddb_cursor < 8)
		ddb_cursor = 8;
	ddb_plane = ddb_size - ddb_cursor;

	if (!plane.valid || plane.blocks >= ddb_plane) {
		/* Nothing the buffer can satisfy: take what there is rather
		 * than program a level the engine will refuse. */
		plane.blocks = ddb_plane - 1;
		plane.lines = 31;
	}
	if (!cursor.valid || cursor.blocks >= ddb_cursor) {
		cursor.blocks = ddb_cursor - 1;
		cursor.lines = cursor.lines > 31 ? 31 : cursor.lines;
	}
	i915_write32(i915, PLANE_BUF_CFG(p->pipe, 0), (ddb_plane - 1) << 16);
	i915_write32(i915, CUR_BUF_CFG(p->pipe), ((ddb_size - 1) << 16) | ddb_plane);
	i915_write32(i915, PLANE_WM(p->pipe, 0, 0),
		     PLANE_WM_EN | (plane.lines << PLANE_WM_LINES_SHIFT) | plane.blocks);
	for (int l = 1; l < 8; l++)
		i915_write32(i915, PLANE_WM(p->pipe, 0, l), 0);
	i915_write32(i915, PLANE_WM_TRANS(p->pipe, 0), 0);
	i915_write32(i915, CUR_WM(p->pipe, 0),
		     PLANE_WM_EN | (cursor.lines << PLANE_WM_LINES_SHIFT) | cursor.blocks);
	for (int l = 1; l < 8; l++)
		i915_write32(i915, CUR_WM(p->pipe, l), 0);
	i915_write32(i915, CUR_WM_TRANS(p->pipe), 0);
	if (g_plane_logged < 8)
		i915_dbg("[drm] i915: watermarks %c: plane %u blocks / %u lines (%s), cursor %u / %u in %u blocks, latency %u us\n",
			'A' + p->pipe, plane.blocks, plane.lines,
			y_tiled ? "Y tiled" : x_tiled ? "X tiled" : "linear", cursor.blocks,
			cursor.lines, ddb_cursor, latency);
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

static void transcoder_program(struct i915_device *i915, struct intel_output *o,
			       struct intel_pipe *p, const struct drm_mode_modeinfo *m,
			       uint32_t src_w, uint32_t src_h)
{
	int t = p->transcoder;
	uint32_t hsync_pol = (m->flags & DRM_MODE_FLAG_PHSYNC) ? TRANS_DDI_PHSYNC : 0;
	uint32_t vsync_pol = (m->flags & DRM_MODE_FLAG_PVSYNC) ? TRANS_DDI_PVSYNC : 0;

	i915_write32(i915, TRANS_HTOTAL(t), ((m->htotal - 1) << 16) | (m->hdisplay - 1));
	i915_write32(i915, TRANS_HBLANK(t), ((m->htotal - 1) << 16) | (m->hdisplay - 1));
	i915_write32(i915, TRANS_HSYNC(t), ((m->hsync_end - 1) << 16) | (m->hsync_start - 1));
	i915_write32(i915, TRANS_VTOTAL(t), ((m->vtotal - 1) << 16) | (m->vdisplay - 1));
	i915_write32(i915, TRANS_VBLANK(t), ((m->vtotal - 1) << 16) | (m->vdisplay - 1));
	i915_write32(i915, TRANS_VSYNC(t), ((m->vsync_end - 1) << 16) | (m->vsync_start - 1));
	i915_write32(i915, TRANS_VSYNCSHIFT(t), 0);
	i915_write32(i915, TRANS_MULT(t), 0);
	/* The pipe's picture is the client's size; the scaler stretches it
	 * onto the timing when the two differ. */
	i915_write32(i915, PIPESRC(p->pipe), ((src_w - 1) << 16) | (src_h - 1));

	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		uint32_t dm, dn, lm, ln;
		/* data: bytes per pixel * pixel clock over link symbols * lanes */
		compute_m_n(m->clock * 24, o->link_rate_khz * 8 * (uint32_t)o->lane_count, &dm, &dn);
		compute_m_n(m->clock, o->link_rate_khz, &lm, &ln);
		i915_write32(i915, TRANS_DATA_M1(t), TU_SIZE(64) | dm);
		i915_write32(i915, TRANS_DATA_N1(t), dn);
		i915_write32(i915, TRANS_LINK_M1(t), lm);
		i915_write32(i915, TRANS_LINK_N1(t), ln);
		i915_write32(i915, TRANS_MSA_MISC(t), TRANS_MSA_SYNC_CLK | TRANS_MSA_8_BPC);
	}
	i915_write32(i915, PIPE_MISC(p->pipe), PIPE_MISC_BPC_8);

	uint32_t func = TRANS_DDI_FUNC_ENABLE | trans_ddi_select_port(i915, o->port) |
			TRANS_DDI_BPC_8 | hsync_pol | vsync_pol;
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		func |= TRANS_DDI_MODE_SELECT_DP_SST |
			TRANS_DDI_PORT_WIDTH((uint32_t)o->lane_count);
	else if (o->type == INTEL_OUTPUT_HDMI)
		func |= TRANS_DDI_MODE_SELECT_HDMI;
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
	} else {
		i915_write32(i915, TRANS_CLK_SEL(t), trans_clk_sel_port(i915, o->port));
	}
	i915_write32(i915, TRANS_DDI_FUNC_CTL(t), func);
}

static int transcoder_enable(struct i915_device *i915, struct intel_pipe *p)
{
	int t = p->transcoder;
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
	uint32_t func = i915_read32(i915, TRANS_DDI_FUNC_CTL(t));
	func &= ~(TRANS_DDI_FUNC_ENABLE | trans_ddi_port_mask(i915) | TRANS_DDI_MODE_SELECT_MASK);
	i915_write32(i915, TRANS_DDI_FUNC_CTL(t), func);
	if (t != TRANSCODER_EDP)
		i915_write32(i915, TRANS_CLK_SEL(t), TRANS_CLK_SEL_DISABLED);
}

/* ---- the primary plane -------------------------------------------------------- */

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
const uint64_t intel_fb_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED, I915_FORMAT_MOD_Y_TILED,
};
const uint32_t intel_nfb_modifiers = sizeof(intel_fb_modifiers) / sizeof(intel_fb_modifiers[0]);

uint32_t intel_fb_tile_width(uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
		return 64;
	case I915_FORMAT_MOD_X_TILED:
		return 512;
	case I915_FORMAT_MOD_Y_TILED:
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
		return 32;
	default:
		return 0;
	}
}

static int format_supported(uint32_t format)
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
	struct i915_device *i915 = to_i915(dev);
	struct i915_bo *bo = o->priv;
	uint64_t mod = *modifier;
	uint64_t obj_mod;

	if (!bo)
		return -EINVAL;
	if (!format_supported(r->pixel_format))
		return -EINVAL;
	obj_mod = bo->tiling == I915_TILING_X ? I915_FORMAT_MOD_X_TILED :
		  bo->tiling == I915_TILING_Y ? I915_FORMAT_MOD_Y_TILED :
					        DRM_FORMAT_MOD_LINEAR;
	if (mod == DRM_FORMAT_MOD_INVALID)
		mod = obj_mod;
	else if (mod != obj_mod && bo->tiling != I915_TILING_NONE)
		return -EINVAL;
	if (mod == I915_FORMAT_MOD_Y_TILED && i915->info->gen < 9)
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
							       "an unknown layout",
			r->pitches[0], (unsigned long long)(o->size / 1024), bo->tiling);
	}
	return 0;
}

/* Said once, for the first surface a client puts on the screen: what it
 * asked for and what the plane was given.  A picture that is structured
 * but wrong is almost always one of these disagreeing. */

static void plane_program(struct i915_device *i915, struct intel_pipe *p,
			  uint32_t ggtt, uint32_t pitch, uint32_t w, uint32_t h,
			  uint32_t format, uint64_t modifier)
{
	/* The plane's own gamma stays off and the PIPE's table applies: that
	 * table is the identity until a client sets one.  Gen11 moved those
	 * bits (and alpha) to a register of their own. */
	int color_ctl = i915->info->display_ver >= 11;
	uint32_t ctl = PLANE_CTL_ENABLE;
	if (!color_ctl)
		ctl |= PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_GAMMA_ENABLE |
		       PLANE_CTL_ALPHA_DISABLE;
	uint32_t tw = intel_fb_tile_width(modifier);
	if (!tw) {
		tw = 64;
		modifier = DRM_FORMAT_MOD_LINEAR;
	}
	switch (modifier) {
	case I915_FORMAT_MOD_X_TILED:
		ctl |= PLANE_CTL_TILED_X;
		break;
	case I915_FORMAT_MOD_Y_TILED:
		ctl |= PLANE_CTL_TILED_Y;
		break;
	default:
		ctl |= PLANE_CTL_TILED_LINEAR;
		break;
	}
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888;
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888 | PLANE_CTL_ORDER_RGBX;
		break;
	case DRM_FORMAT_XRGB2101010:
		ctl |= PLANE_CTL_FORMAT_XRGB_2101010;
		break;
	case DRM_FORMAT_XBGR2101010:
		ctl |= PLANE_CTL_FORMAT_XRGB_2101010 | PLANE_CTL_ORDER_RGBX;
		break;
	case DRM_FORMAT_RGB565:
		ctl |= PLANE_CTL_FORMAT_RGB_565;
		break;
	default:
		ctl |= PLANE_CTL_FORMAT_XRGB_8888;
		break;
	}
	/* the watermarks belong to the surface actually being scanned out */
	int cpp = (format == DRM_FORMAT_RGB565) ? 2 : 4;
	if (p->mode.clock && p->mode.htotal)
		skl_program_wm(i915, p, p->mode.clock, w, p->mode.htotal, cpp, modifier);
	if (legacy_plane(i915)) {
		bdw_plane_program(i915, p, ggtt, pitch, format, modifier);
		goto done;
	}
	i915_write32(i915, PLANE_STRIDE(p->pipe, 0), pitch / tw);
	i915_write32(i915, PLANE_POS(p->pipe, 0), (p->pos_y << 16) | p->pos_x);
	i915_write32(i915, PLANE_OFFSET(p->pipe, 0), (p->off_y << 16) | p->off_x);
	i915_write32(i915, PLANE_SIZE(p->pipe, 0), ((h - 1) << 16) | (w - 1));
	if (color_ctl)
		i915_write32(i915, PLANE_COLOR_CTL(p->pipe, 0),
			     PLANE_COLOR_PIPE_GAMMA_ENABLE | PLANE_COLOR_PLANE_GAMMA_DISABLE |
				     PLANE_COLOR_ALPHA_DISABLE);
	i915_write32(i915, PLANE_CTL(p->pipe, 0), ctl);
	i915_write32(i915, PLANE_SURF(p->pipe, 0), ggtt);
	(void)i915_read32(i915, PLANE_SURF(p->pipe, 0));
done:
	p->surf_ggtt = ggtt;
	p->stride = pitch;
	p->width = w;
	p->height = h;
	p->format = format;
	p->modifier = modifier;
	/* new watermarks: let the engine tell us again whether they hold */
	if (p->underrun_reported) {
		uint32_t imr = i915_read32(i915, GEN8_DE_PIPE_IMR(p->pipe));
		i915_write32(i915, GEN8_DE_PIPE_IMR(p->pipe),
			     imr & ~GEN8_PIPE_FIFO_UNDERRUN);
		uint32_t ier = i915_read32(i915, GEN8_DE_PIPE_IER(p->pipe));
		i915_write32(i915, GEN8_DE_PIPE_IER(p->pipe),
			     ier | GEN8_PIPE_FIFO_UNDERRUN);
		p->underrun_reported = 0;
	}
	if (g_plane_logged < 8) {
		g_plane_logged++;
		i915_dbg("[drm] i915: plane %c: %ux%u %c%c%c%c, %s, pitch %u (%u units), at %08x\n",
			'A' + p->pipe, w, h, (char)(format & 0xff),
			(char)((format >> 8) & 0xff), (char)((format >> 16) & 0xff),
			(char)((format >> 24) & 0xff),
			modifier == DRM_FORMAT_MOD_LINEAR	 ? "linear" :
			modifier == I915_FORMAT_MOD_X_TILED ? "X tiled" :
			modifier == I915_FORMAT_MOD_Y_TILED ? "Y tiled" :
								    "an unknown layout",
			pitch, pitch / tw, ggtt);
		if (legacy_plane(i915))
			i915_dbg("[drm] i915:   DSPCNTR %08x, DSPSTRIDE %08x, DSPSURF %08x\n",
				 i915_read32(i915, DSPCNTR(p->pipe)),
				 i915_read32(i915, DSPSTRIDE(p->pipe)),
				 i915_read32(i915, DSPSURF(p->pipe)));
		else
			i915_dbg("[drm] i915:   PLANE_CTL %08x, STRIDE %08x, SIZE %08x, SURF %08x\n",
				 i915_read32(i915, PLANE_CTL(p->pipe, 0)),
				 i915_read32(i915, PLANE_STRIDE(p->pipe, 0)),
				 i915_read32(i915, PLANE_SIZE(p->pipe, 0)),
				 i915_read32(i915, PLANE_SURF(p->pipe, 0)));
	}
}

static void plane_disable(struct i915_device *i915, struct intel_pipe *p)
{
	if (legacy_plane(i915)) {
		i915_write32(i915, DSPCNTR(p->pipe), 0);
		i915_write32(i915, DSPSURF(p->pipe), 0);
		(void)i915_read32(i915, DSPSURF(p->pipe));
		return;
	}
	i915_write32(i915, PLANE_CTL(p->pipe, 0), 0);
	i915_write32(i915, PLANE_SURF(p->pipe, 0), 0);
	(void)i915_read32(i915, PLANE_SURF(p->pipe, 0));
}

/* The surface register of the primary plane, for a flip. */
static void plane_flip(struct i915_device *i915, struct intel_pipe *p, uint32_t surf)
{
	uint32_t reg = legacy_plane(i915) ? DSPSURF(p->pipe) : PLANE_SURF(p->pipe, 0);
	i915_write32(i915, reg, surf);
	(void)i915_read32(i915, reg);
}

static int plane_enabled(struct i915_device *i915, int pipe)
{
	if (legacy_plane(i915))
		return !!(i915_read32(i915, DSPCNTR(pipe)) & DISP_ENABLE);
	return !!(i915_read32(i915, PLANE_CTL(pipe, 0)) & PLANE_CTL_ENABLE);
}

/* ---- the pipe scaler and the gamma table ----------------------------------------- */

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

/* The pipe's 8-bit palette: 256 entries of 8:8:8, the high byte of each
 * 16-bit value the client handed over. */
static void pipe_gamma_program(struct i915_device *i915, struct intel_pipe *p,
			       const uint16_t gamma[3][256])
{
	i915_write32(i915, GAMMA_MODE(p->pipe), GAMMA_MODE_MODE_8BIT);
	for (int i = 0; i < 256; i++)
		i915_write32(i915, LGC_PALETTE(p->pipe, i),
			     ((uint32_t)(gamma[0][i] >> 8) << 16) |
				     ((uint32_t)(gamma[1][i] >> 8) << 8) |
				     (uint32_t)(gamma[2][i] >> 8));
}

/* ---- vblank interrupts --------------------------------------------------------- */

static void pipe_vblank_enable(struct i915_device *i915, struct intel_pipe *p, int on)
{
	uint32_t bits = GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN;
	uint32_t imr = i915_read32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe));
	uint32_t ier = i915_read32_fw(i915, GEN8_DE_PIPE_IER(p->pipe));
	if (on) {
		i915_write32_fw(i915, GEN8_DE_PIPE_IIR(p->pipe), bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe), imr & ~bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(p->pipe), ier | bits);
	} else {
		i915_write32_fw(i915, GEN8_DE_PIPE_IMR(p->pipe), imr | bits);
		i915_write32_fw(i915, GEN8_DE_PIPE_IER(p->pipe), ier & ~bits);
	}
	(void)i915_read32_fw(i915, GEN8_DE_PIPE_IER(p->pipe));
	p->vblank_enabled = on;
}

void intel_display_irq(struct i915_device *i915, int pipe, uint32_t iir)
{
	struct intel_pipe *p = &i915->display.pipes[pipe];
	if (iir & GEN8_PIPE_VBLANK) {
		if (p->active && p->output >= 0) {
			struct intel_output *o = &i915->display.outputs[p->output];
			if (o->crtc >= 0)
				drm_vblank_tick(&i915->drm, o->crtc);
		}
		p->flip_pending = 0;
	}
	if (iir & GEN8_PIPE_FIFO_UNDERRUN) {
		p->underruns++;
		/* A plane that cannot keep up underruns on EVERY line, which
		 * is half a million interrupts a second -- enough to starve
		 * the machine that is trying to fix it.  The first one says
		 * everything the rest would; mask it and keep counting the
		 * ones the status register still records. */
		if (!p->underrun_reported) {
			uint32_t imr = i915_read32_fw(i915, GEN8_DE_PIPE_IMR(pipe));
			i915_write32_fw(i915, GEN8_DE_PIPE_IMR(pipe),
					imr | GEN8_PIPE_FIFO_UNDERRUN);
			uint32_t ier = i915_read32_fw(i915, GEN8_DE_PIPE_IER(pipe));
			i915_write32_fw(i915, GEN8_DE_PIPE_IER(pipe),
					ier & ~GEN8_PIPE_FIFO_UNDERRUN);
			p->underrun_reported = 1;
		}
	}
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

	if (o->port == PORT_A && i915->display.model != INTEL_DISPLAY_TGL) {
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
	plane_disable(i915, &tmp);
	i915_write32(i915, CUR_CTL(pipe), 0);
	transcoder_disable(i915, &tmp);
	intel_ddi_buf_disable(i915, o);
	intel_dpll_unroute_port(i915, o->port);
	i915->display.pipes[pipe].active = 0;
	/* nothing to restore from here on: the fallback keeps the console
	 * on whatever the driver manages to light */
	i915->boot_scanout.pipe = -1;
}

/* Does the link as trained still carry the mode?  The retry lowers the
 * rate, which can drop below what the picture needs. */
static int link_carries_mode(const struct intel_output *o,
			     const struct drm_mode_modeinfo *m)
{
	uint64_t link = (uint64_t)o->link_rate_khz * (uint32_t)o->lane_count * 8 / 10;
	uint64_t need = (uint64_t)m->clock * 24 / 8;
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
		if (tmds)
			intel_hdmi_disable(i915, o, p->transcoder);
		pipe_vblank_enable(i915, p, 0);
		plane_disable(i915, p);
		i915_write32(i915, CUR_CTL(p->pipe), 0);
		i915_write32(i915, CUR_BASE(p->pipe), 0);
		transcoder_disable(i915, p);
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
}

static int output_enable(struct i915_device *i915, struct intel_output *o,
			 struct intel_pipe *p, const struct drm_mode_modeinfo *mode,
			 uint32_t src_w, uint32_t src_h)
{
	int rc;

	o->pipe = p->pipe;
	intel_power_get(i915, (enum intel_power_domain)(INTEL_PW_PIPE_A + p->pipe));
	intel_power_get(i915, port_domain(o->port));
	intel_power_get(i915, aux_domain(i915, o->port));

	if (o->is_tc && intel_tc_connect(i915, o)) {
		kprintf("[drm] i915: port %s: nothing on the Type-C connector\n",
			intel_port_name(i915, o->port));
		goto fail;
	}
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		if (o->is_edp) {
			intel_pps_vdd_on(i915, o);
		}
		if (!o->detected && !intel_dp_detect(i915, o))
			goto fail;
		rc = intel_dp_choose_link(i915, o, mode);
		if (rc) {
			kprintf("[drm] i915: port %c: no link carries %ux%u\n",
				'A' + o->port, mode->hdisplay, mode->vdisplay);
			goto fail;
		}
		o->pll = intel_dpll_get_dp(i915, o->port, o->link_rate_khz, o->ssc);
		if (o->pll < 0) {
			kprintf("[drm] i915: port %c: no PLL for %u kHz\n", 'A' + o->port,
				o->link_rate_khz);
			goto fail;
		}
		intel_ddi_pre_enable(i915, o, mode);
		if (o->is_edp)
			intel_pps_panel_on(i915, o);
		intel_dp_sink_power(i915, o, 1);
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
				if (!intel_dpll_rate_supported(i915, rate))
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
			rc = intel_dp_link_train(i915, o);
			if (rc)
				goto fail;
			if (!link_carries_mode(o, mode)) {
				kprintf("[drm] i915: port %c: the link that trained does not carry %ux%u\n",
					'A' + o->port, mode->hdisplay, mode->vdisplay);
				goto fail;
			}
		}
	} else {
		if (!o->detected && !intel_hdmi_detect(i915, o))
			goto fail;
		if (intel_hdmi_mode_valid(i915, o, mode)) {
			kprintf("[drm] i915: port %c: %ux%u (%u kHz) is not a TMDS mode here\n",
				'A' + o->port, mode->hdisplay, mode->vdisplay, mode->clock);
			goto fail;
		}
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

/* ---- the DRM backend ------------------------------------------------------------- */

static const char *layout_name(uint64_t modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR	 ? "linear" :
	       modifier == I915_FORMAT_MOD_X_TILED ? "X tiled" :
	       modifier == I915_FORMAT_MOD_Y_TILED ? "Y tiled" :
						     "an unknown layout";
}

/* Can the sink's link carry the mode at all?  The rate and width chosen
 * at enable time are the best that train; this is the ceiling. */
static int dp_link_carries(struct i915_device *i915, const struct intel_output *o,
			   const struct drm_mode_modeinfo *m)
{
	uint32_t rate = 0;
	for (int r = 0; r < o->nsink_rates; r++)
		if (o->sink_rates_khz[r] > rate && intel_dpll_rate_supported(i915, o->sink_rates_khz[r]))
			rate = o->sink_rates_khz[r];
	if (!rate)
		return 1; /* unknown yet: the enable path decides */
	int lanes = o->dpcd[DP_MAX_LANE_COUNT] & 0x1f;
	if (o->port == PORT_A && !o->four_lane_strap && lanes > 2)
		lanes = 2;
	if (lanes < 1)
		lanes = 1;
	uint64_t link = (uint64_t)rate * (uint32_t)lanes * 8 / 10;
	uint64_t need = (uint64_t)m->clock * 24 / 8;
	return link * 99 >= need * 100;
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

int intel_atomic_check(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct i915_device *i915 = to_i915(dev);

	if (!i915->display.ready)
		return -ENODEV;
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = output_for_crtc(i915, (int)i);
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);

		if (!cs->active)
			continue;
		if (!o || !prim || !cur)
			return -ENODEV;
		/* the crtc's own connector, and no other, drives its output */
		if (cs->connector_mask != (1u << i))
			return -EINVAL;
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
				return -EINVAL;
			cs->adjusted_mode = *f;
			cs->use_scaler = 1;
		}
		const struct drm_mode_modeinfo *hw = &cs->adjusted_mode;
		if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
			if (o->detected && !dp_link_carries(i915, o, hw))
				return -EINVAL;
		} else if (o->detected && intel_hdmi_mode_valid(i915, o, hw)) {
			return -EINVAL;
		}
		/* the planes of this crtc */
		struct drm_plane_state *ps = drm_atomic_plane_state(st, prim);
		if (ps->fb) {
			if (!format_supported(ps->fb->format))
				return -EINVAL;
			uint32_t tw = intel_fb_tile_width(ps->fb->modifier);
			if (!tw || ps->fb->pitch % tw)
				return -EINVAL;
			if (ps->crtc_x < 0 || ps->crtc_y < 0 ||
			    (uint32_t)ps->crtc_x + ps->crtc_w > m->hdisplay ||
			    (uint32_t)ps->crtc_y + ps->crtc_h > m->vdisplay)
				return -EINVAL;
			/* Broadwell's primary plane has no position or size of
			 * its own: it is the pipe, X tiled or linear. */
			if (legacy_plane(i915) &&
			    (ps->crtc_x || ps->crtc_y || ps->crtc_w != m->hdisplay ||
			     ps->crtc_h != m->vdisplay ||
			     ps->fb->modifier == I915_FORMAT_MOD_Y_TILED))
				return -EINVAL;
		}
		struct drm_plane_state *cps = drm_atomic_plane_state(st, cur);
		if (cps->fb) {
			if (cps->crtc_w != cps->crtc_h ||
			    (cps->crtc_w != 64 && cps->crtc_w != 128 && cps->crtc_w != 256))
				return -EINVAL;
			if (cps->fb->format != DRM_FORMAT_ARGB8888 ||
			    cps->fb->modifier != DRM_FORMAT_MOD_LINEAR ||
			    cps->fb->pitch != cps->crtc_w * 4 || cps->src_x || cps->src_y)
				return -EINVAL;
		}
	}
	return 0;
}

static void cursor_program(struct i915_device *i915, struct intel_pipe *cp,
			   const struct drm_plane_state *ps)
{
	int pipe = cp->pipe;
	if (!ps->fb) {
		i915_write32(i915, CUR_CTL(pipe), 0);
		i915_write32(i915, CUR_BASE(pipe), 0);
		(void)i915_read32(i915, CUR_BASE(pipe));
		return;
	}
	if (bo_bind(i915, ps->fb->obj))
		return;
	struct i915_bo *bo = ps->fb->obj->priv;
	uint32_t w = ps->crtc_w;
	uint32_t mode = w == 64 ? CUR_MODE_64_ARGB_AX :
			w == 128 ? CUR_MODE_128_ARGB_AX : CUR_MODE_256_ARGB_AX;
	if (cp->cursor_w != w) {
		cp->cursor_w = w;
		/* the cursor's share of the buffer depends on its size */
		if (cp->active && cp->mode.clock && cp->mode.htotal)
			skl_program_wm(i915, cp, cp->pixel_rate_khz, cp->width,
				       cp->mode.htotal,
				       cp->format == DRM_FORMAT_RGB565 ? 2 : 4,
				       cp->modifier);
	}
	uint32_t pos = 0;
	int x = ps->crtc_x, y = ps->crtc_y;
	if (x < 0) {
		pos |= CUR_POS_SIGN;
		x = -x;
	}
	if (y < 0) {
		pos |= CUR_POS_SIGN << 16;
		y = -y;
	}
	pos |= (uint32_t)x | ((uint32_t)y << 16);
	i915_write32(i915, CUR_CTL(pipe),
		     mode | CUR_PIPE_SELECT((uint32_t)pipe) | CUR_GAMMA_ENABLE);
	i915_write32(i915, CUR_POS(pipe), pos);
	/* the position and the mode take effect on the base write */
	i915_write32(i915, CUR_BASE(pipe), bo->ggtt + ps->fb->offset);
	(void)i915_read32(i915, CUR_BASE(pipe));
}

/* The primary plane from its state: the whole thing when the layout
 * changed, just the surface address for a flip. */
static int primary_program(struct i915_device *i915, struct intel_pipe *p,
			   const struct drm_plane_state *ps, int force)
{
	if (!ps->fb) {
		plane_disable(i915, p);
		p->surf_ggtt = 0;
		return 0;
	}
	int rc = bo_bind(i915, ps->fb->obj);
	if (rc)
		return rc;
	struct i915_bo *bo = ps->fb->obj->priv;
	uint32_t w = ps->crtc_w, h = ps->crtc_h;
	uint32_t surf = bo->ggtt + ps->fb->offset;
	int same = !force && p->surf_ggtt && ps->fb->pitch == p->stride &&
		   ps->fb->format == p->format && ps->fb->modifier == p->modifier &&
		   w == p->width && h == p->height &&
		   (uint32_t)ps->crtc_x == p->pos_x && (uint32_t)ps->crtc_y == p->pos_y &&
		   (ps->src_x >> 16) == p->off_x && (ps->src_y >> 16) == p->off_y;
	if (same) {
		plane_flip(i915, p, surf);
		p->surf_ggtt = surf;
		return 0;
	}
	p->pos_x = (uint32_t)ps->crtc_x;
	p->pos_y = (uint32_t)ps->crtc_y;
	p->off_x = ps->src_x >> 16;
	p->off_y = ps->src_y >> 16;
	plane_program(i915, p, surf, ps->fb->pitch, w, h, ps->fb->format,
		      ps->fb->modifier);
	return 0;
}

/* A full mode set of one crtc: its output comes down (or the firmware's
 * state is released) and goes up with the new mode. */
static int crtc_modeset(struct i915_device *i915, struct drm_atomic_state *st,
			uint32_t idx)
{
	struct drm_device *dev = &i915->drm;
	struct drm_crtc_state *cs = &st->crtcs[idx];
	struct intel_output *o = output_for_crtc(i915, (int)idx);
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
	rc = output_enable(i915, o, p, hw, src_w, src_h);
	if (rc)
		return rc;
	/* the table before the pipe shows anything through it */
	pipe_gamma_program(i915, p, cs->gamma);
	rc = transcoder_enable(i915, p);
	if (rc) {
		output_disable(i915, o);
		return rc;
	}
	p->active = 1;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		intel_hdmi_enable(i915, o);
	p->surf_ggtt = 0;
	rc = primary_program(i915, p, ps, 1);
	if (rc) {
		output_disable(i915, o);
		return rc;
	}
	cursor_program(i915, p, cps);
	pipe_vblank_enable(i915, p, 1);
	if (o->is_edp)
		intel_backlight_enable(i915, o);
	o->crtc = (int)idx;
	kprintf("[drm] i915: pipe %c on port %c: %ux%u@%u (%u kHz)%s, plane at %08x\n",
		'A' + p->pipe, 'A' + o->port, hw->hdisplay, hw->vdisplay, hw->vrefresh,
		hw->clock, cs->use_scaler ? " (scaled)" : "", p->surf_ggtt);
	return 0;
}

int intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st)
{
	struct i915_device *i915 = to_i915(dev);
	int rc = 0;

	if (!i915->display.ready)
		return -ENODEV;
	/* crtcs going off first: they may free the pipe another one needs */
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = output_for_crtc(i915, (int)i);
		if (!cs->changed || cs->active || !o)
			continue;
		if (o->active)
			output_disable(i915, o);
		o->crtc = -1;
	}
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct intel_output *o = output_for_crtc(i915, (int)i);
		if (!cs->changed || !cs->active || !o)
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
				if (cs->gamma_changed)
					pipe_gamma_program(i915, p, cs->gamma);
				if (ps->changed) {
					rc = primary_program(i915, p, ps, 0);
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
					cursor_program(i915, p, cps);
				continue;
			}
		}
		rc = crtc_modeset(i915, st, i);
		if (rc)
			return rc;
	}
	return 0;
}

int intel_detect(struct drm_device *dev, struct drm_connector *c)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_output *o = output_for_conn(i915, c);
	if (!o)
		return DRM_MODE_DISCONNECTED;
	/* a Type-C port answers only once the display holds its PHY */
	if (o->is_tc && !o->active && intel_tc_connect(i915, o))
		return DRM_MODE_DISCONNECTED;
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		return intel_dp_detect(i915, o) ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		return intel_hdmi_detect(i915, o) ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	return DRM_MODE_DISCONNECTED;
}

int intel_get_modes(struct drm_device *dev, struct drm_connector *c)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_output *o = output_for_conn(i915, c);
	int conn = (int)(c - dev->conn);
	int n = 0;

	if (!o || !o->detected)
		return (int)c->nmodes;
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		if (intel_dp_read_edid(i915, o) == 0)
			n = drm_connector_set_edid(dev, conn, o->edid, (unsigned)o->edid_len);
	} else if (o->edid_len > 0 || intel_hdmi_read_edid(i915, o) == 0) {
		n = drm_connector_set_edid(dev, conn, o->edid, (unsigned)o->edid_len);
	}
	if (n <= 0) {
		drm_connector_set_edid(dev, conn, NULL, 0);
		drm_connector_clear_modes(dev, conn);
		if (o->is_edp && i915->display.vbt.panel_mode_valid) {
			drm_connector_add_mode(dev, conn, &i915->display.vbt.panel_mode);
			n = 1;
		}
	}
	if (o->is_edp) {
		/* The panel's timing is the first mode; anything else on the
		 * list is scaled onto it by the pipe, so the standard modes
		 * up to its size are offered too. */
		o->fixed_mode_valid = 0;
		if (c->nmodes) {
			o->fixed_mode = c->modes[0];
			o->fixed_mode_valid = 1;
			drm_connector_add_std_modes(dev, conn, o->fixed_mode.clock,
						    o->fixed_mode.hdisplay,
						    o->fixed_mode.vdisplay);
		}
	} else {
		/* an external sink: the standard modes under its limits */
		uint32_t max_clock;
		if (o->type == INTEL_OUTPUT_DP)
			max_clock = o->nsink_rates ?
					    o->sink_rates_khz[o->nsink_rates - 1] * 4 * 8 / 10 / 3 :
					    165000;
		else
			max_clock = (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink) ? 300000 : 165000;
		drm_connector_add_std_modes(dev, conn, max_clock, 4096, 2304);
	}
	return (int)c->nmodes;
}

int intel_display_verify(struct drm_device *dev)
{
	struct i915_device *i915 = to_i915(dev);
	for (int i = 0; i < i915->info->num_pipes; i++) {
		struct intel_pipe *p = &i915->display.pipes[i];
		if (!p->active)
			continue;
		if (!(i915_read32(i915, TRANS_CONF(p->transcoder)) & TRANS_CONF_STATE_ENABLE)) {
			kprintf("[drm] i915: pipe %c: transcoder not running\n", 'A' + i);
			return -EIO;
		}
		if (!plane_enabled(i915, p->pipe)) {
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
			if (legacy_plane(i915))
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

void intel_display_fallback(struct drm_device *dev)
{
	struct i915_device *i915 = to_i915(dev);
	/* Back to what the firmware left: its plane on its pipe, if the
	 * pipe and transcoder are still running the mode it set. */
	if (i915->boot_scanout.pipe >= 0) {
		int pipe = i915->boot_scanout.pipe;
		if (legacy_plane(i915)) {
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
			if (i915->info->flags & I915_INFO_HAS_TC_PHY) {
				int first_tc = d->model == INTEL_DISPLAY_TGL ? PORT_TC1 : PORT_C;
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
	case I915_DPLL_LEGACY:
		if (i915->info->gen == 8) {
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
	case INTEL_DISPLAY_BXT: d->ddb_blocks = 512 - 4; break;
	case INTEL_DISPLAY_ICL: d->ddb_blocks = ICL_DDB_SIZE - 4; break;
	case INTEL_DISPLAY_TGL: d->ddb_blocks = TGL_DDB_SLICE_SIZE - 4; break;
	default: d->ddb_blocks = SKL_DDB_SIZE - 4; break;
	}

	intel_opregion_init(i915);
	if (d->opregion.vbt) {
		rc = intel_vbt_parse(d->opregion.vbt, d->opregion.vbt_size, &d->vbt);
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
	rc = intel_cdclk_init(i915);
	if (rc)
		return rc;
	intel_dpll_init(i915);
	intel_ddi_init(i915);
	intel_pps_init(i915);
	intel_backlight_init(i915);
	i915_ggtt_program_pat(i915);

	intel_gmbus_init(i915);
	outputs_from_vbt(i915);
	/* What the firmware left the ports running at, while it is still
	 * there to be read: the rate of the PLL each port is routed to and
	 * the width its buffer was enabled with. */
	for (int i = 0; i < d->nout; i++) {
		struct intel_output *o = &d->outputs[i];
		uint32_t buf = i915_read32(i915, DDI_BUF_CTL(o->port));
		if (o->port == PORT_A)
			o->four_lane_strap = !!(buf & DDI_A_4_LANES);
		if (!(buf & DDI_BUF_CTL_ENABLE))
			continue;
		o->fw_link_rate_khz = intel_dpll_port_link_rate(i915, o->port);
		o->fw_lanes = (int)(((buf & DDI_PORT_WIDTH_MASK) >> 1) + 1);
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
		/* Probe now: the console needs modes before anyone asks. */
		int st = intel_detect(dev, &dev->conn[conn]);
		dev->conn[conn].connected = (st == DRM_MODE_CONNECTED);
		if (dev->conn[conn].connected)
			intel_get_modes(dev, &dev->conn[conn]);
		kprintf("[drm] i915: connector %d: DDI %c %s, %s, %u modes%s%s\n", conn,
			'A' + o->port,
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
	intel_dpll_init(i915);
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
