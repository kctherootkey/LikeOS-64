// LikeOS -- watermarks and the display data buffer, Skylake and later.
//
// A plane reads its pixels from memory into the display data buffer
// (DBUF) ahead of the beam; the watermark levels tell the hardware how
// many blocks (512 bytes each) it must have buffered to survive a memory
// wake-up of each of up to eight latencies, from a plain DRAM access
// (level 0) to the deepest package power state.  Every plane gets a
// contiguous share of the buffer (its DDB entry): a pipe first gets a
// range of the DBUF slices assigned to it -- shared with other pipes in
// proportion to their widths -- the cursor a fixed piece at its end, and
// the primary plane what its highest achievable level needs plus a part
// of what is left over.  Levels that do not fit are switched off.
//
// From Ice Lake the buffer is split into slices that are powered and
// assigned per pipe from a table of allowed combinations; from Alder
// Lake-P a single pipe may join both halves of the memory bus (MBUS) to
// get all four.  Each pipe's path into MBUS has credits that depend on
// how many pipes share it.  Changes are made in an order that never
// lets two pipes' allocations overlap, one frame apart where needed.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_wm_regs.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define U16_MAX_ 0xffffu
#define U32_MAX_ 0xffffffffu

#define DBUF_S1 0
#define DBUF_S2 1
#define DBUF_S3 2
#define DBUF_S4 3
#define BIT8(n) ((uint8_t)(1u << (n)))

/* The planes this driver has per pipe: the first universal plane and
 * the cursor.  The other universal planes are kept off with nothing
 * allocated. */
#define WM_PRIMARY 0
#define WM_CURSOR 1
#define WM_NPLANES 2
#define WM_MAX_LEVELS 8

/* ---- the platforms ------------------------------------------------------------------ */

#define F_IPC INTEL_WM_HAS_IPC
#define F_SAGV INTEL_WM_HAS_SAGV
#define F_CRAWL INTEL_WM_CDCLK_CRAWL
#define F_SQUASH INTEL_WM_CDCLK_SQUASH
#define F_DGFX INTEL_WM_DGFX
#define F_2PPC INTEL_WM_2PPC

/* ip, DBUF blocks (Gen9/10 keep 4 for the bypass path), slices, ABOXes,
 * universal planes per pipe, flags. */
static const struct intel_wm_platform plat_hsw = { 700, 0, 0, 0, 2, 0 };
static const struct intel_wm_platform plat_bdw = { 800, 0, 0, 0, 2, 0 };
static const struct intel_wm_platform plat_skl = { 900, 896 - 4, 0x1, 0, 2, F_IPC | F_SAGV };
static const struct intel_wm_platform plat_bxt = { 900, 512 - 4, 0x1, 0, 3, F_IPC };
static const struct intel_wm_platform plat_glk = { 1000, 1024 - 4, 0x1, 0, 4, F_IPC | F_2PPC };
static const struct intel_wm_platform plat_cnl = { 1000, 1024 - 4, 0x1, 0, 4,
						   F_IPC | F_SAGV | F_2PPC };
static const struct intel_wm_platform plat_icl = { 1100, 2048, 0x3, 0x1, 7,
						   F_IPC | F_SAGV | F_2PPC };
static const struct intel_wm_platform plat_tgl = { 1200, 2048, 0x3, 0x6, 7,
						   F_IPC | F_SAGV | F_2PPC };
static const struct intel_wm_platform plat_dg1 = { 1200, 2048, 0x3, 0x6, 7,
						   F_IPC | F_SAGV | F_2PPC | F_DGFX };
static const struct intel_wm_platform plat_rkl = { 1200, 2048, 0x3, 0x1, 5,
						   F_IPC | F_SAGV | F_2PPC };
static const struct intel_wm_platform plat_adls = { 1200, 2048, 0x3, 0x6, 5,
						    F_IPC | F_SAGV | F_2PPC };
static const struct intel_wm_platform plat_adlp = { 1300, 4096, 0xf, 0x3, 5,
						    F_IPC | F_SAGV | F_2PPC | F_CRAWL };
static const struct intel_wm_platform plat_dg2 = { 1300, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_SQUASH | F_DGFX };
static const struct intel_wm_platform plat_mtl = { 1400, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH };
static const struct intel_wm_platform plat_bmg = {
	1401, 4096, 0xf, 0x3, 5, F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH | F_DGFX
};
static const struct intel_wm_platform plat_lnl = { 2000, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH };
static const struct intel_wm_platform plat_ptl = { 3000, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH };
/* Wildcat Lake: Panther Lake's buffer with three pipes; Nova Lake. */
static const struct intel_wm_platform plat_wcl = { 3002, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH };
static const struct intel_wm_platform plat_nvl = { 3500, 4096, 0xf, 0x3, 5,
						   F_IPC | F_SAGV | F_2PPC | F_CRAWL | F_SQUASH };

const struct intel_wm_platform *intel_wm_platform_get(struct i915_device *i915)
{
	switch (i915->info->platform) {
	case I915_PLATFORM_HASWELL: return &plat_hsw;
	case I915_PLATFORM_BROADWELL: return &plat_bdw;
	case I915_PLATFORM_SKYLAKE:
	case I915_PLATFORM_KABYLAKE:
	case I915_PLATFORM_COFFEELAKE:
	case I915_PLATFORM_COMETLAKE: return &plat_skl;
	case I915_PLATFORM_BROXTON: return &plat_bxt;
	case I915_PLATFORM_GEMINILAKE: return &plat_glk;
	case I915_PLATFORM_CANNONLAKE: return &plat_cnl;
	case I915_PLATFORM_ICELAKE:
	case I915_PLATFORM_ELKHARTLAKE:
	case I915_PLATFORM_JASPERLAKE: return &plat_icl;
	case I915_PLATFORM_TIGERLAKE: return &plat_tgl;
	case I915_PLATFORM_DG1: return &plat_dg1;
	case I915_PLATFORM_ROCKETLAKE: return &plat_rkl;
	case I915_PLATFORM_ALDERLAKE_S: return &plat_adls;
	case I915_PLATFORM_ALDERLAKE_P: return &plat_adlp;
	case I915_PLATFORM_DG2: return &plat_dg2;
	case I915_PLATFORM_METEORLAKE: return &plat_mtl;
	case I915_PLATFORM_LUNARLAKE: return &plat_lnl;
	case I915_PLATFORM_BATTLEMAGE: return &plat_bmg;
	case I915_PLATFORM_PANTHERLAKE: return &plat_ptl;
	case I915_PLATFORM_WILDCATLAKE: return &plat_wcl;
	case I915_PLATFORM_NOVALAKE_S:
	case I915_PLATFORM_NOVALAKE_P: return &plat_nvl;
	default: return NULL;
	}
}

int intel_wm_display_ver(struct i915_device *i915)
{
	const struct intel_wm_platform *p = intel_wm_platform_get(i915);
	return p ? p->ip / 100 : 0;
}

/* ---- small helpers -------------------------------------------------------------------- */

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t div_round_up(uint64_t a, uint64_t b)
{
	return b ? (uint32_t)((a + b - 1) / b) : 0;
}

static int hweight8(uint32_t v)
{
	int n = 0;
	for (v &= 0xff; v; v &= v - 1)
		n++;
	return n;
}

static int ffs8(uint32_t v)
{
	for (int i = 0; i < 8; i++)
		if (v & (1u << i))
			return i + 1;
	return 0;
}

static int fls8(uint32_t v)
{
	for (int i = 7; i >= 0; i--)
		if (v & (1u << i))
			return i + 1;
	return 0;
}

/* 16.16 fixed point, as the watermark arithmetic is specified. */
#define FP_MAX U32_MAX_

static uint32_t fp_clamp(uint64_t v)
{
	return v > U32_MAX_ ? U32_MAX_ : (uint32_t)v;
}

static uint32_t u32_to_fp(uint32_t v)
{
	return fp_clamp((uint64_t)v << 16);
}

static uint32_t fp_to_u32_round_up(uint32_t fp)
{
	return div_round_up(fp, 1u << 16);
}

static uint32_t div_fp(uint64_t val, uint64_t d)
{
	return fp_clamp(((val << 16) + d - 1) / d);
}

static uint32_t mul_u32_fp(uint32_t val, uint32_t fp)
{
	return fp_clamp((uint64_t)val * fp);
}

static uint32_t mul_round_up_u32_fp(uint32_t val, uint32_t fp)
{
	return fp_clamp(((uint64_t)val * fp + 0xffff) >> 16);
}

static uint32_t add_fp_u32(uint32_t fp, uint32_t v)
{
	return fp_clamp((uint64_t)fp + ((uint64_t)v << 16));
}

static uint32_t div_round_up_fp(uint32_t val, uint32_t d)
{
	return d ? div_round_up(val, d) : 0;
}

/* ---- state ------------------------------------------------------------------------------ */

struct skl_wm_level {
	uint16_t min_ddb_alloc;
	uint16_t blocks;
	uint16_t lines;
	uint8_t enable;
	uint8_t ignore_lines;
	uint8_t auto_min_alloc_wm_enable;
	uint8_t can_sagv;
};

struct skl_plane_wm {
	struct skl_wm_level wm[WM_MAX_LEVELS];
	struct skl_wm_level trans_wm;
	struct {
		struct skl_wm_level wm0;
		struct skl_wm_level trans_wm;
	} sagv;
};

struct skl_ddb_entry {
	uint16_t start, end; /* end exclusive */
};

struct skl_pipe_state {
	uint8_t active;
	uint8_t use_sagv_wm;
	uint8_t level; /* highest level the DDB holds */
	uint8_t can_sagv;
	struct skl_plane_wm planes[WM_NPLANES];
	struct skl_ddb_entry plane_ddb[WM_NPLANES];
	uint16_t min_ddb[WM_NPLANES], interim_ddb[WM_NPLANES];
	struct skl_ddb_entry ddb; /* absolute, for the overlap checks */
	uint16_t linetime, ips_linetime;
	uint32_t data_rate; /* kB/s of the primary plane */
	uint64_t rel_data_rate;
};

struct skl_dbuf_state {
	struct skl_ddb_entry ddb[INTEL_MAX_PIPES]; /* MBUS-relative */
	uint32_t weight[INTEL_MAX_PIPES];
	uint8_t slices[INTEL_MAX_PIPES];
	uint8_t enabled_slices;
	uint8_t active_pipes;
	uint8_t mdclk_cdclk_ratio;
	uint8_t joined_mbus;
};

struct skl_wm_state {
	struct skl_dbuf_state dbuf;
	struct skl_pipe_state pipe[INTEL_MAX_PIPES];
};

static struct {
	struct i915_device *i915;
	const struct intel_wm_platform *plat;
	int ver;
	int num_levels;
	uint16_t latency[WM_MAX_LEVELS];
	int ipc_enabled;
	int ready;
	struct skl_wm_state hw; /* as programmed */
	int warned;
} g_wm;

/* scratch for a computation (one at a time: the commit path); the
 * update keeps its own, as it calls into code that checks */
static struct skl_wm_state g_new;
static struct skl_wm_state g_check;
static struct intel_wm_pipe_cfg g_cfg[INTEL_MAX_PIPES];

static int npipes(struct i915_device *i915)
{
	int n = i915->info->num_pipes;
	return n > INTEL_MAX_PIPES ? INTEL_MAX_PIPES : n;
}

static int plat_is(struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

static int is_kbl_cfl(struct i915_device *i915)
{
	return plat_is(i915, I915_PLATFORM_KABYLAKE) || plat_is(i915, I915_PLATFORM_COFFEELAKE) ||
	       plat_is(i915, I915_PLATFORM_COMETLAKE);
}

static int is_dgfx(void)
{
	return !!(g_wm.plat->flags & F_DGFX);
}

/* SAGV watermarks the driver switches to (Tiger Lake on, not Rocket
 * Lake), and from display version 13 (integrated) the ones the hardware
 * switches to by itself. */
static int has_sagv_wm(struct i915_device *i915)
{
	return g_wm.ver >= 12 && !plat_is(i915, I915_PLATFORM_ROCKETLAKE);
}

static int has_hw_sagv_wm(void)
{
	return g_wm.ver >= 13 && !is_dgfx();
}

static int has_mbus_joining(struct i915_device *i915)
{
	return plat_is(i915, I915_PLATFORM_ALDERLAKE_P) || g_wm.ver >= 14;
}

static int num_planes_on_pipe(struct i915_device *i915, int pipe)
{
	/* Broxton's pipe C has one plane fewer. */
	if (plat_is(i915, I915_PLATFORM_BROXTON) && pipe == PIPE_C)
		return 2;
	return g_wm.plat->num_planes;
}

/* ---- describing a pipe ------------------------------------------------------------------- */

void intel_wm_cfg_set_mode(struct intel_wm_pipe_cfg *c, const struct drm_mode_modeinfo *m,
			   uint32_t src_w, uint32_t src_h, uint32_t port_clock_khz,
			   int is_dp, int is_edp, int lanes)
{
	c->active = 1;
	c->hdisplay = m->hdisplay;
	c->htotal = m->htotal;
	c->vdisplay = m->vdisplay;
	c->vtotal = m->vtotal;
	c->clock_khz = m->clock;
	c->src_w = (uint16_t)(src_w ? src_w : m->hdisplay);
	c->src_h = (uint16_t)(src_h ? src_h : m->vdisplay);
	c->scaled = c->src_w != m->hdisplay || c->src_h != m->vdisplay;
	c->port_clock_khz = port_clock_khz;
	c->is_dp = !!is_dp;
	c->is_edp = !!is_edp;
	c->lanes = (uint8_t)lanes;
}

void intel_wm_cfg_set_planes(struct intel_wm_pipe_cfg *c, int plane_on, uint32_t w,
			     uint32_t h, uint32_t drm_format, uint64_t modifier,
			     uint32_t cursor_w)
{
	c->plane_on = plane_on && w && h;
	c->plane_w = (uint16_t)w;
	c->plane_h = (uint16_t)h;
	c->cpp = drm_format == DRM_FORMAT_RGB565 ? 2 : 4;
	c->modifier = modifier;
	c->cursor_w = (uint16_t)cursor_w;
}

void intel_wm_cfg_from_hw(struct i915_device *i915, struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES])
{
	struct intel_display *d = &i915->display;

	mm_memset(cfg, 0, sizeof(struct intel_wm_pipe_cfg) * INTEL_MAX_PIPES);
	for (int pipe = 0; pipe < npipes(i915); pipe++) {
		struct intel_pipe *p = &d->pipes[pipe];
		struct intel_wm_pipe_cfg *c = &cfg[pipe];
		const struct intel_output *o = NULL;
		uint32_t port_clock;
		int dp;

		if (!p->active || !p->mode.clock || !p->mode.htotal || !p->mode.vtotal)
			continue;
		if (p->output >= 0 && p->output < d->nout)
			o = &d->outputs[p->output];
		dp = o && (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
		port_clock = dp && o->link_rate_khz ? o->link_rate_khz : p->mode.clock;
		intel_wm_cfg_set_mode(c, &p->mode, p->src_w, p->src_h, port_clock, dp,
				      o && o->is_edp, o ? o->lane_count : 0);
		intel_wm_cfg_set_planes(c, p->surf_ggtt != 0, p->width, p->height, p->format,
					p->modifier, p->cursor_w);
		c->plane_src_x = (uint16_t)p->off_x;
	}
}

/* The pipe's pixel rate: the dot clock, raised by the scaler's
 * downscaling when the picture is larger than the timing. */
static uint32_t pipe_pixel_rate(const struct intel_wm_pipe_cfg *c)
{
	uint32_t dst_w, dst_h;

	if (!c->scaled)
		return c->clock_khz;
	dst_w = umin(c->src_w, c->hdisplay);
	dst_h = umin(c->src_h, c->vdisplay);
	if (!dst_w || !dst_h)
		return c->clock_khz;
	return div_round_up((uint64_t)c->clock_khz * c->src_w * c->src_h,
			    (uint64_t)dst_w * dst_h);
}

/* ---- memory latencies, IPC ----------------------------------------------------------------- */

static uint32_t skl_wm_latency(int level, int x_tiled, int have_wp)
{
	uint32_t latency = g_wm.latency[level];
	struct i915_device *i915 = g_wm.i915;

	if (latency == 0)
		return 0;
	/* WaIncreaseLatencyIPCEnabled: Kaby/Coffee/Comet Lake (WA #1141) */
	if (is_kbl_cfl(i915) && g_wm.ipc_enabled)
		latency += 4;
	/* the memory bandwidth workaround of display version 9 */
	if (g_wm.ver == 9 && have_wp && x_tiled)
		latency += 15;
	return latency;
}

static int need_16gb_dimm_wa(struct i915_device *i915)
{
	const struct intel_dram_info *dram = intel_dram_get(i915);

	if (!(plat_is(i915, I915_PLATFORM_SKYLAKE) || is_kbl_cfl(i915) || g_wm.ver == 11))
		return 0;
	return !dram || dram->has_16gb_dimms;
}

/* The latencies as published (display version 14 in registers, the
 * rest through two pcode reads of four levels), then corrected: DG2's
 * are in units twice as long; a level after a zero one is off; they
 * must not decrease; a zero level 0 means the read latency itself was
 * left out (2 us, 3 from version 12, 6 from 14); and parts that may
 * carry 16 Gb DIMMs need one microsecond more. */
static void skl_setup_wm_latency(struct i915_device *i915)
{
	uint16_t *wm = g_wm.latency;
	int n, level;

	n = g_wm.num_levels = has_hw_sagv_wm() ? 6 : 8;
	for (level = 0; level < WM_MAX_LEVELS; level++)
		wm[level] = 0;
	if (g_wm.ver >= 14) {
		uint32_t v = i915_read32(i915, WM_LATENCY_LP0_LP1);
		wm[0] = (uint16_t)WM_LATENCY_EVEN(v);
		wm[1] = (uint16_t)WM_LATENCY_ODD(v);
		v = i915_read32(i915, WM_LATENCY_LP2_LP3);
		wm[2] = (uint16_t)WM_LATENCY_EVEN(v);
		wm[3] = (uint16_t)WM_LATENCY_ODD(v);
		v = i915_read32(i915, WM_LATENCY_LP4_LP5);
		wm[4] = (uint16_t)WM_LATENCY_EVEN(v);
		wm[5] = (uint16_t)WM_LATENCY_ODD(v);
	} else {
		uint32_t v = 0;
		if (intel_pcode_rw(i915, PCODE_GEN9_READ_MEM_LATENCY, &v, NULL, 500, 20, 1)) {
			kprintf("[drm] i915: the memory latencies could not be read\n");
		} else {
			wm[0] = v & 0xff;
			wm[1] = (v >> 8) & 0xff;
			wm[2] = (v >> 16) & 0xff;
			wm[3] = (v >> 24) & 0xff;
			v = 1;
			if (intel_pcode_rw(i915, PCODE_GEN9_READ_MEM_LATENCY, &v, NULL, 500, 20,
					   1) == 0) {
				wm[4] = v & 0xff;
				wm[5] = (v >> 8) & 0xff;
				wm[6] = (v >> 16) & 0xff;
				wm[7] = (v >> 24) & 0xff;
			}
		}
	}
	i915_dbg("[drm] i915: memory latency as published %u/%u/%u/%u/%u/%u/%u/%u us\n",
		 wm[0], wm[1], wm[2], wm[3], wm[4], wm[5], wm[6], wm[7]);

	if (plat_is(i915, I915_PLATFORM_DG2))
		for (level = 0; level < n; level++)
			wm[level] *= 2;
	if (g_wm.ver >= 35)
		wm[0] = 0;
	for (level = 1; level < n; level++)
		if (wm[level] == 0)
			break;
	for (level = level + 1; level < n; level++)
		wm[level] = 0;
	for (level = 1; level < n; level++) {
		if (wm[level] == 0)
			break;
		wm[level] = (uint16_t)umax(wm[level], wm[level - 1]);
	}
	if (wm[0] == 0) {
		uint16_t inc = g_wm.ver >= 14 ? 6 : g_wm.ver >= 12 ? 3 : 2;
		wm[0] += inc;
		for (level = 1; level < n && wm[level]; level++)
			wm[level] += inc;
	}
	if (need_16gb_dimm_wa(i915)) {
		wm[0] += 1;
		for (level = 1; level < n && wm[level]; level++)
			wm[level] += 1;
	}
	for (level = n; level < WM_MAX_LEVELS; level++)
		wm[level] = 0;

	/* the rest of the driver reads them here */
	for (level = 0; level < 8; level++)
		i915->display.mem_latency[level] = wm[level];
	i915->display.nlatency = n;
	kprintf("[drm] i915: watermark latencies %u/%u/%u/%u/%u/%u/%u/%u us (%d levels)\n",
		wm[0], wm[1], wm[2], wm[3], wm[4], wm[5], wm[6], wm[7], n);
}

/* Isochronous priority control: off on Skylake (WA #0477), and on Kaby,
 * Coffee and Comet Lake only with symmetric memory (WA #1141). */
static void skl_ipc_init(struct i915_device *i915)
{
	const struct intel_dram_info *dram = intel_dram_get(i915);
	int en = 1;

	if (!(g_wm.plat->flags & F_IPC))
		return;
	if (plat_is(i915, I915_PLATFORM_SKYLAKE))
		en = 0;
	else if (is_kbl_cfl(i915))
		en = dram && dram->detected && dram->symmetric_memory;
	g_wm.ipc_enabled = en;
	uint32_t v = i915_read32(i915, DISP_ARB_CTL2);
	v = en ? (v | DISP_IPC_ENABLE) : (v & ~DISP_IPC_ENABLE);
	i915_write32(i915, DISP_ARB_CTL2, v);
}

/* ---- the watermark of one plane at one level -------------------------------------------------- */

struct skl_wm_params {
	uint8_t x_tiled, y_tiled, rc_surface;
	uint8_t cpp;
	uint32_t width;
	uint32_t plane_pixel_rate;
	uint32_t y_min_scanlines;
	uint32_t plane_bytes_per_line;
	uint32_t plane_blocks_per_line; /* 16.16 */
	uint32_t y_tile_minimum; /* 16.16 */
	uint32_t linetime_us;
	uint32_t dbuf_block_size;
	uint32_t htotal;
};

static void skl_compute_wm_params(const struct intel_wm_pipe_cfg *c, uint32_t width, int cpp,
				  uint64_t modifier, uint32_t plane_pixel_rate,
				  struct skl_wm_params *wp, uint32_t pan_x)
{
	uint32_t interm_pbpl;

	mm_memset(wp, 0, sizeof(*wp));
	wp->x_tiled = modifier == I915_FORMAT_MOD_X_TILED;
	wp->y_tiled = !wp->x_tiled && modifier != DRM_FORMAT_MOD_LINEAR;
	wp->rc_surface = 0;
	wp->width = width;
	wp->cpp = (uint8_t)cpp;
	wp->plane_pixel_rate = plane_pixel_rate;
	wp->htotal = c->htotal;
	wp->dbuf_block_size = 512;
	wp->y_min_scanlines = 4;
	if (g_wm.ver == 9)
		wp->y_min_scanlines *= 2;
	wp->plane_bytes_per_line = wp->width * wp->cpp;
	if (wp->y_tiled) {
		interm_pbpl = div_round_up((uint64_t)wp->plane_bytes_per_line * wp->y_min_scanlines,
					   wp->dbuf_block_size);
		if (g_wm.ver >= 30)
			interm_pbpl += (pan_x != 0);
		else if (g_wm.ver >= 10)
			interm_pbpl++;
		wp->plane_blocks_per_line = div_fp(interm_pbpl, wp->y_min_scanlines);
	} else {
		interm_pbpl = div_round_up(wp->plane_bytes_per_line, wp->dbuf_block_size);
		if (!wp->x_tiled || g_wm.ver >= 10)
			interm_pbpl++;
		wp->plane_blocks_per_line = u32_to_fp(interm_pbpl);
	}
	wp->y_tile_minimum = mul_u32_fp(wp->y_min_scanlines, wp->plane_blocks_per_line);
	wp->linetime_us = plane_pixel_rate ? div_round_up((uint64_t)c->htotal * 1000,
							  plane_pixel_rate) : 0;
}

static uint32_t skl_wm_method1(uint32_t pixel_rate, uint32_t cpp, uint32_t latency,
			       uint32_t dbuf_block_size)
{
	uint32_t ret;

	if (latency == 0)
		return FP_MAX;
	ret = div_fp((uint64_t)latency * pixel_rate * cpp, 1000ull * dbuf_block_size);
	if (g_wm.ver >= 10)
		ret = add_fp_u32(ret, 1);
	return ret;
}

static uint32_t skl_wm_method2(uint32_t pixel_rate, uint32_t pipe_htotal, uint32_t latency,
			       uint32_t plane_blocks_per_line)
{
	uint32_t val;

	if (latency == 0)
		return FP_MAX;
	val = div_round_up((uint64_t)latency * pixel_rate, (uint64_t)pipe_htotal * 1000);
	return mul_u32_fp(val, plane_blocks_per_line);
}

static int skl_wm_has_lines(int level)
{
	if (g_wm.ver >= 10)
		return 1;
	/* the number of lines is ignored for level 0 */
	return level > 0;
}

static uint32_t skl_wm_max_lines(void)
{
	return g_wm.ver >= 13 ? 255 : 31;
}

static void skl_compute_plane_wm(int is_cursor, int level, uint32_t latency,
				 const struct skl_wm_params *wp,
				 const struct skl_wm_level *result_prev,
				 struct skl_wm_level *result)
{
	uint32_t method1, method2, selected;
	uint32_t blocks, lines, min_ddb_alloc = 0;

	if (latency == 0) {
		result->min_ddb_alloc = U16_MAX_; /* reject it */
		return;
	}
	method1 = skl_wm_method1(wp->plane_pixel_rate, wp->cpp, latency, wp->dbuf_block_size);
	method2 = skl_wm_method2(wp->plane_pixel_rate, wp->htotal, latency,
				 wp->plane_blocks_per_line);

	if (wp->y_tiled) {
		selected = umax(method2, wp->y_tile_minimum);
	} else if (g_wm.ver >= 35) {
		selected = method2;
	} else {
		if ((wp->cpp * wp->htotal / wp->dbuf_block_size < 1) &&
		    (wp->plane_bytes_per_line / wp->dbuf_block_size < 1))
			selected = method2;
		else if (latency >= wp->linetime_us)
			selected = g_wm.ver == 9 ? umin(method1, method2) : method2;
		else
			selected = method1;
	}

	blocks = fp_to_u32_round_up(selected);
	if (g_wm.ver < 30)
		blocks++;
	/* At least one line's worth: there is at least one line in the
	 * lines figure (and method 2 would have given that on parts with
	 * a buffer large enough for it). */
	if (skl_wm_has_lines(level))
		blocks = umax(blocks, fp_to_u32_round_up(wp->plane_blocks_per_line));
	lines = div_round_up_fp(selected, wp->plane_blocks_per_line);

	if (g_wm.ver == 9) {
		/* Display WA #1125 */
		if (level == 0 && wp->rc_surface)
			blocks += fp_to_u32_round_up(wp->y_tile_minimum);
		/* Display WA #1126 */
		if (level >= 1 && level <= 7) {
			if (wp->y_tiled) {
				blocks += fp_to_u32_round_up(wp->y_tile_minimum);
				lines += wp->y_min_scanlines;
			} else {
				blocks++;
			}
		}
	}

	/* no level may need less than the one below it */
	blocks = umax(blocks, result_prev->blocks);
	lines = umax(lines, result_prev->lines);

	if (g_wm.ver >= 11) {
		if (wp->y_tiled) {
			uint32_t extra_lines;
			if (lines % wp->y_min_scanlines == 0)
				extra_lines = wp->y_min_scanlines;
			else
				extra_lines = wp->y_min_scanlines * 2 - lines % wp->y_min_scanlines;
			min_ddb_alloc = mul_round_up_u32_fp(lines + extra_lines,
							    wp->plane_blocks_per_line);
		} else {
			min_ddb_alloc = blocks + div_round_up(blocks, 10);
		}
	}

	if (!skl_wm_has_lines(level))
		lines = 0;
	if (lines > skl_wm_max_lines()) {
		result->min_ddb_alloc = U16_MAX_; /* reject it */
		return;
	}

	/* Assume the level can be used; the DDB allocation turns it off
	 * again when there turn out to be too few blocks for it.  A value
	 * equal to the allocation is not valid, hence the +1. */
	result->blocks = (uint16_t)umin(blocks, 0xffff);
	result->lines = (uint16_t)lines;
	result->min_ddb_alloc = (uint16_t)umin(umax(min_ddb_alloc, blocks) + 1, U16_MAX_);
	result->enable = 1;
	result->auto_min_alloc_wm_enable = g_wm.ver >= 30 && level == 0 && !is_cursor;
	if (!has_sagv_wm(g_wm.i915) && intel_sagv_block_time_us(g_wm.i915))
		result->can_sagv = latency >= intel_sagv_block_time_us(g_wm.i915);
}

static void skl_compute_wm_levels(int is_cursor, const struct skl_wm_params *wp,
				  struct skl_wm_level *levels)
{
	const struct skl_wm_level *prev = &levels[0];

	for (int level = 0; level < g_wm.num_levels; level++) {
		struct skl_wm_level *result = &levels[level];
		uint32_t latency = skl_wm_latency(level, wp->x_tiled, 1);
		skl_compute_plane_wm(is_cursor, level, latency, wp, prev, result);
		prev = result;
	}
}

static void tgl_compute_sagv_wm(int is_cursor, const struct skl_wm_params *wp,
				struct skl_plane_wm *plane_wm)
{
	uint32_t latency = 0;
	uint32_t bt = intel_sagv_block_time_us(g_wm.i915);

	if (bt)
		latency = bt + skl_wm_latency(0, wp->x_tiled, 1);
	skl_compute_plane_wm(is_cursor, 0, latency, wp, &plane_wm->wm[0], &plane_wm->sagv.wm0);
}

static void skl_compute_transition_wm(struct skl_wm_level *trans_wm,
				      const struct skl_wm_level *wm0,
				      const struct skl_wm_params *wp)
{
	uint32_t trans_min, trans_amount, trans_offset, wm0_blocks, blocks;

	/* meaningless without IPC */
	if (!g_wm.ipc_enabled)
		return;
	/* WaDisableTWM: not recommended on display version 9 */
	if (g_wm.ver == 9)
		return;
	trans_min = g_wm.ver >= 11 ? 4 : 14;
	/* Display WA #1140: Gemini Lake, Cannon Lake */
	trans_amount = g_wm.ver == 10 ? 0 : 10;
	trans_offset = trans_min + trans_amount;
	/* the spec's "Selected Result Blocks" of level 0, which is the
	 * integer result less the one added to it */
	wm0_blocks = wm0->blocks ? wm0->blocks - 1u : 0;
	if (wp->y_tiled) {
		uint32_t trans_y_tile_min = mul_round_up_u32_fp(2, wp->y_tile_minimum);
		blocks = umax(wm0_blocks, trans_y_tile_min) + trans_offset;
	} else {
		blocks = wm0_blocks + trans_offset;
	}
	blocks++;
	trans_wm->blocks = (uint16_t)blocks;
	trans_wm->min_ddb_alloc = (uint16_t)umax(wm0->min_ddb_alloc, blocks + 1);
	trans_wm->enable = 1;
}

static void skl_build_plane_wm(const struct intel_wm_pipe_cfg *c, int is_cursor,
			       struct skl_plane_wm *wm)
{
	struct skl_wm_params wp;
	uint32_t rate = pipe_pixel_rate(c);

	mm_memset(wm, 0, sizeof(*wm));
	if (is_cursor) {
		if (!c->cursor_w)
			return;
		skl_compute_wm_params(c, c->cursor_w, 4, DRM_FORMAT_MOD_LINEAR, rate, &wp, 0);
	} else {
		if (!c->plane_on)
			return;
		skl_compute_wm_params(c, c->plane_w, c->cpp, c->modifier, rate, &wp,
				      c->plane_src_x);
	}
	skl_compute_wm_levels(is_cursor, &wp, wm->wm);
	skl_compute_transition_wm(&wm->trans_wm, &wm->wm[0], &wp);
	if (has_sagv_wm(g_wm.i915)) {
		tgl_compute_sagv_wm(is_cursor, &wp, wm);
		skl_compute_transition_wm(&wm->sagv.trans_wm, &wm->sagv.wm0, &wp);
	}
}

/* The cursor's fixed share: what its levels need at the largest size
 * the cursor can have, and at least 32 blocks with one pipe on (8 with
 * more). */
static uint32_t skl_cursor_allocation(const struct intel_wm_pipe_cfg *c, int num_active)
{
	struct skl_wm_level wm;
	struct skl_wm_params wp;
	uint32_t min_ddb_alloc = 0;

	mm_memset(&wm, 0, sizeof(wm));
	skl_compute_wm_params(c, 256, 4, DRM_FORMAT_MOD_LINEAR, pipe_pixel_rate(c), &wp, 0);
	for (int level = 0; level < g_wm.num_levels; level++) {
		uint32_t latency = skl_wm_latency(level, wp.x_tiled, 1);
		skl_compute_plane_wm(1, level, latency, &wp, &wm, &wm);
		if (wm.min_ddb_alloc == U16_MAX_)
			break;
		min_ddb_alloc = wm.min_ddb_alloc;
	}
	return umax(num_active == 1 ? 32 : 8, min_ddb_alloc);
}

/* ---- prefill: which levels the vertical blank leaves time for ---------------------------------- */

/* microseconds to lines of this timing, 16.16 */
static uint32_t prefill_usecs_to_lines(const struct intel_wm_pipe_cfg *c, uint32_t usecs)
{
	return div_round_up((uint64_t)c->clock_khz * ((uint64_t)usecs << 16),
			    (uint64_t)c->htotal * 1000);
}

static uint32_t prefill_adjust(uint32_t value, uint32_t factor)
{
	return div_round_up((uint64_t)value * factor, 0x10000);
}

struct skl_prefill {
	uint32_t full; /* lines before any latency, 16.16 */
};

static void skl_prefill_init(const struct intel_wm_pipe_cfg *c, const struct skl_pipe_state *ps,
			     struct skl_prefill *pf)
{
	uint32_t fixed, wm0 = 0, scaler_1st, scaler_2nd = 0, nocdclk, adj_cdclk;
	uint32_t ppc = (g_wm.plat->flags & F_2PPC) ? 2 : 1;
	uint32_t min_cdclk, cdclk;

	/* one line of frame start delay, and 20 us of address translation */
	fixed = (1u << 16) + prefill_usecs_to_lines(c, 20);
	for (int p = 0; p < WM_NPLANES; p++)
		wm0 = umax(wm0, ps->planes[p].wm[0].lines);
	wm0 <<= 16;
	scaler_1st = c->scaled ? (4u << 16) : 0;

	/* no compression; the scaler's own factors are taken as 1 */
	nocdclk = prefill_adjust(0, 0x10000);
	nocdclk += scaler_2nd;
	nocdclk = prefill_adjust(nocdclk, 0x10000);
	nocdclk += scaler_1st;
	nocdclk += wm0;

	/* the CDCLK this pipe alone would get, as a fraction of the rate */
	min_cdclk = div_round_up((uint64_t)c->clock_khz * 100, 100ull * ppc);
	cdclk = intel_cdclk_round_khz(g_wm.i915, min_cdclk);
	adj_cdclk = cdclk ? umin(0x10000, div_round_up((uint64_t)c->clock_khz << 16,
						      (uint64_t)ppc * cdclk)) : 0x10000;
	pf->full = fixed + prefill_adjust(nocdclk, adj_cdclk);
}

static int skl_prefill_vblank_too_short(const struct intel_wm_pipe_cfg *c,
					const struct skl_prefill *pf, uint32_t latency_us)
{
	uint32_t guardband = (uint32_t)(c->vtotal > c->vdisplay ? c->vtotal - c->vdisplay : 0)
			     << 16;
	return guardband < pf->full + prefill_usecs_to_lines(c, latency_us);
}

/* Levels whose latency the vertical blank cannot cover are switched off
 * (and the SAGV ones with them when the block time does not fit).
 * Returns -EINVAL when not even level 0 fits. */
static int skl_wm_check_vblank(const struct intel_wm_pipe_cfg *c, struct skl_pipe_state *ps)
{
	struct skl_prefill pf;
	int level;

	skl_prefill_init(c, ps, &pf);
	for (level = g_wm.num_levels - 1; level >= 0; level--) {
		uint32_t latency = skl_wm_latency(level, 0, 0);
		if (latency == 0)
			continue;
		if (level == 0)
			latency = 0;
		if (!skl_prefill_vblank_too_short(c, &pf, latency))
			break;
	}
	if (level < 0)
		return -EINVAL;
	for (level++; level < g_wm.num_levels; level++)
		for (int p = 0; p < WM_NPLANES; p++)
			ps->planes[p].wm[level].enable = 0;
	if (has_sagv_wm(g_wm.i915) && intel_sagv_block_time_us(g_wm.i915) &&
	    skl_prefill_vblank_too_short(c, &pf, intel_sagv_block_time_us(g_wm.i915))) {
		for (int p = 0; p < WM_NPLANES; p++) {
			ps->planes[p].sagv.wm0.enable = 0;
			ps->planes[p].sagv.trans_wm.enable = 0;
		}
	}
	return 0;
}

/* ---- DBUF slices per pipe ---------------------------------------------------------------- */

struct dbuf_slice_conf_entry {
	uint8_t active_pipes;
	uint8_t dbuf_mask[INTEL_MAX_PIPES];
	uint8_t join_mbus;
};

#define PA BIT8(PIPE_A)
#define PB BIT8(PIPE_B)
#define PC BIT8(PIPE_C)
#define PD BIT8(PIPE_D)
#define S1 BIT8(DBUF_S1)
#define S2 BIT8(DBUF_S2)
#define S3 BIT8(DBUF_S3)
#define S4 BIT8(DBUF_S4)

/* The allowed assignments, per combination of active pipes: pipes have
 * a preferred slice and fixed rules for sharing them. */
static const struct dbuf_slice_conf_entry icl_allowed_dbufs[] = {
	{ PA, { S1, 0, 0, 0 }, 0 },
	{ PB, { 0, S1, 0, 0 }, 0 },
	{ PA | PB, { S1, S2, 0, 0 }, 0 },
	{ PC, { 0, 0, S2, 0 }, 0 },
	{ PA | PC, { S1, 0, S2, 0 }, 0 },
	{ PB | PC, { 0, S1, S2, 0 }, 0 },
	{ PA | PB | PC, { S1, S1, S2, 0 }, 0 },
	{ 0, { 0, 0, 0, 0 }, 0 },
};

static const struct dbuf_slice_conf_entry tgl_allowed_dbufs[] = {
	{ PA, { S1 | S2, 0, 0, 0 }, 0 },
	{ PB, { 0, S1 | S2, 0, 0 }, 0 },
	{ PA | PB, { S2, S1, 0, 0 }, 0 },
	{ PC, { 0, 0, S2 | S1, 0 }, 0 },
	{ PA | PC, { S1, 0, S2, 0 }, 0 },
	{ PB | PC, { 0, S1, S2, 0 }, 0 },
	{ PA | PB | PC, { S1, S1, S2, 0 }, 0 },
	{ PD, { 0, 0, 0, S2 | S1 }, 0 },
	{ PA | PD, { S1, 0, 0, S2 }, 0 },
	{ PB | PD, { 0, S1, 0, S2 }, 0 },
	{ PA | PB | PD, { S1, S1, 0, S2 }, 0 },
	{ PC | PD, { 0, 0, S1, S2 }, 0 },
	{ PA | PC | PD, { S1, 0, S2, S2 }, 0 },
	{ PB | PC | PD, { 0, S1, S2, S2 }, 0 },
	{ PA | PB | PC | PD, { S1, S1, S2, S2 }, 0 },
	{ 0, { 0, 0, 0, 0 }, 0 },
};

static const struct dbuf_slice_conf_entry dg2_allowed_dbufs[] = {
	{ PA, { S1 | S2, 0, 0, 0 }, 0 },
	{ PB, { 0, S1 | S2, 0, 0 }, 0 },
	{ PA | PB, { S1, S2, 0, 0 }, 0 },
	{ PC, { 0, 0, S3 | S4, 0 }, 0 },
	{ PA | PC, { S1 | S2, 0, S3 | S4, 0 }, 0 },
	{ PB | PC, { 0, S1 | S2, S3 | S4, 0 }, 0 },
	{ PA | PB | PC, { S1, S2, S3 | S4, 0 }, 0 },
	{ PD, { 0, 0, 0, S3 | S4 }, 0 },
	{ PA | PD, { S1 | S2, 0, 0, S3 | S4 }, 0 },
	{ PB | PD, { 0, S1 | S2, 0, S3 | S4 }, 0 },
	{ PA | PB | PD, { S1, S2, 0, S3 | S4 }, 0 },
	{ PC | PD, { 0, 0, S3, S4 }, 0 },
	{ PA | PC | PD, { S1 | S2, 0, S3, S4 }, 0 },
	{ PB | PC | PD, { 0, S1 | S2, S3, S4 }, 0 },
	{ PA | PB | PC | PD, { S1, S2, S3, S4 }, 0 },
	{ 0, { 0, 0, 0, 0 }, 0 },
};

/* The joined-MBUS entries first, so that check_mbus_joined() prefers
 * them. */
static const struct dbuf_slice_conf_entry adlp_allowed_dbufs[] = {
	{ PA, { S1 | S2 | S3 | S4, 0, 0, 0 }, 1 },
	{ PB, { 0, S1 | S2 | S3 | S4, 0, 0 }, 1 },
	{ PA, { S1 | S2, 0, 0, 0 }, 0 },
	{ PB, { 0, S3 | S4, 0, 0 }, 0 },
	{ PA | PB, { S1 | S2, S3 | S4, 0, 0 }, 0 },
	{ PC, { 0, 0, S3 | S4, 0 }, 0 },
	{ PA | PC, { S1 | S2, 0, S3 | S4, 0 }, 0 },
	{ PB | PC, { 0, S3 | S4, S3 | S4, 0 }, 0 },
	{ PA | PB | PC, { S1 | S2, S3 | S4, S3 | S4, 0 }, 0 },
	{ PD, { 0, 0, 0, S1 | S2 }, 0 },
	{ PA | PD, { S1 | S2, 0, 0, S1 | S2 }, 0 },
	{ PB | PD, { 0, S3 | S4, 0, S1 | S2 }, 0 },
	{ PA | PB | PD, { S1 | S2, S3 | S4, 0, S1 | S2 }, 0 },
	{ PC | PD, { 0, 0, S3 | S4, S1 | S2 }, 0 },
	{ PA | PC | PD, { S1 | S2, 0, S3 | S4, S1 | S2 }, 0 },
	{ PB | PC | PD, { 0, S3 | S4, S3 | S4, S1 | S2 }, 0 },
	{ PA | PB | PC | PD, { S1 | S2, S3 | S4, S3 | S4, S1 | S2 }, 0 },
	{ 0, { 0, 0, 0, 0 }, 0 },
};

static int check_mbus_joined(uint8_t active_pipes, const struct dbuf_slice_conf_entry *t)
{
	for (int i = 0; t[i].active_pipes != 0; i++)
		if (t[i].active_pipes == active_pipes)
			return t[i].join_mbus;
	return 0;
}

static int adlp_check_mbus_joined(uint8_t active_pipes)
{
	return check_mbus_joined(active_pipes, adlp_allowed_dbufs);
}

static uint8_t compute_dbuf_slices(int pipe, uint8_t active_pipes, int join_mbus,
				   const struct dbuf_slice_conf_entry *t)
{
	for (int i = 0; t[i].active_pipes != 0; i++)
		if (t[i].active_pipes == active_pipes && t[i].join_mbus == join_mbus)
			return t[i].dbuf_mask[pipe];
	return 0;
}

static uint8_t skl_compute_dbuf_slices(struct i915_device *i915, int pipe, uint8_t active_pipes,
				       int join_mbus)
{
	if (plat_is(i915, I915_PLATFORM_DG2))
		return compute_dbuf_slices(pipe, active_pipes, join_mbus, dg2_allowed_dbufs);
	if (g_wm.ver >= 13)
		return compute_dbuf_slices(pipe, active_pipes, join_mbus, adlp_allowed_dbufs);
	if (g_wm.ver == 12)
		return compute_dbuf_slices(pipe, active_pipes, join_mbus, tgl_allowed_dbufs);
	if (g_wm.ver == 11)
		return compute_dbuf_slices(pipe, active_pipes, join_mbus, icl_allowed_dbufs);
	/* one slice before that */
	return (active_pipes & BIT8(pipe)) ? S1 : 0;
}

#undef PA
#undef PB
#undef PC
#undef PD
#undef S1
#undef S2
#undef S3
#undef S4

/* ---- the DDB -------------------------------------------------------------------------------- */

static uint32_t ddb_size(const struct skl_ddb_entry *e)
{
	return (uint32_t)(e->end - e->start);
}

static int ddb_equal(const struct skl_ddb_entry *a, const struct skl_ddb_entry *b)
{
	return a->start == b->start && a->end == b->end;
}

static uint16_t ddb_init(struct skl_ddb_entry *e, uint32_t start, uint32_t end)
{
	e->start = (uint16_t)start;
	e->end = (uint16_t)end;
	return (uint16_t)end;
}

static uint32_t dbuf_slice_size(void)
{
	int n = hweight8(g_wm.plat->dbuf_slice_mask);
	return n ? g_wm.plat->dbuf_size / (uint32_t)n : g_wm.plat->dbuf_size;
}

static void ddb_entry_for_slices(uint8_t slice_mask, struct skl_ddb_entry *ddb)
{
	uint32_t slice_size = dbuf_slice_size();

	if (!slice_mask) {
		ddb->start = ddb->end = 0;
		return;
	}
	ddb->start = (uint16_t)((ffs8(slice_mask) - 1) * slice_size);
	ddb->end = (uint16_t)(fls8(slice_mask) * slice_size);
}

/* Offsets in PLANE_BUF_CFG count from the start of the MBUS half the
 * pipe reads through (slices 1-2 or 3-4). */
static uint32_t mbus_ddb_offset(uint8_t slice_mask)
{
	struct skl_ddb_entry ddb;

	if (slice_mask & (BIT8(DBUF_S1) | BIT8(DBUF_S2)))
		slice_mask = BIT8(DBUF_S1);
	else if (slice_mask & (BIT8(DBUF_S3) | BIT8(DBUF_S4)))
		slice_mask = BIT8(DBUF_S3);
	ddb_entry_for_slices(slice_mask, &ddb);
	return ddb.start;
}

static uint8_t ddb_dbuf_slice_mask(const struct skl_ddb_entry *e)
{
	uint32_t slice_size = dbuf_slice_size();
	uint8_t mask = 0;

	if (!ddb_size(e) || !slice_size)
		return 0;
	for (uint32_t s = e->start / slice_size; s <= (uint32_t)(e->end - 1) / slice_size; s++)
		mask |= BIT8(s);
	return mask;
}

/* A pipe's range of its slices, in proportion to the widths of the pipes
 * on the same slices. */
static void skl_crtc_allocate_ddb(struct skl_wm_state *st, int pipe)
{
	struct skl_dbuf_state *db = &st->dbuf;
	struct skl_ddb_entry ddb_slices;
	uint32_t weight_start = 0, weight_end = 0, weight_total = 0;
	uint32_t mbus_offset = 0, range, start, end;
	uint8_t mask;

	if (db->weight[pipe] == 0) {
		ddb_init(&db->ddb[pipe], 0, 0);
		goto out;
	}
	mask = db->slices[pipe];
	ddb_entry_for_slices(mask, &ddb_slices);
	mbus_offset = mbus_ddb_offset(mask);
	range = ddb_size(&ddb_slices);
	for (int p = 0; p < INTEL_MAX_PIPES; p++) {
		uint32_t w = db->weight[p];
		/* slice sets never partially intersect: the same set or none */
		if (db->slices[p] != db->slices[pipe])
			continue;
		weight_total += w;
		if (p < pipe) {
			weight_start += w;
			weight_end += w;
		} else if (p == pipe) {
			weight_end += w;
		}
	}
	start = weight_total ? (uint32_t)((uint64_t)range * weight_start / weight_total) : 0;
	end = weight_total ? (uint32_t)((uint64_t)range * weight_end / weight_total) : 0;
	ddb_init(&db->ddb[pipe], ddb_slices.start - mbus_offset + start,
		 ddb_slices.start - mbus_offset + end);
out:
	/* absolute, for the overlap checks */
	st->pipe[pipe].ddb.start = (uint16_t)(mbus_offset + db->ddb[pipe].start);
	st->pipe[pipe].ddb.end = (uint16_t)(mbus_offset + db->ddb[pipe].end);
	if (!db->ddb[pipe].end)
		st->pipe[pipe].ddb.start = st->pipe[pipe].ddb.end = 0;
}

static void allocate_plane_ddb(uint32_t *iter_start, uint32_t *iter_size,
			       uint64_t *iter_data_rate, uint32_t min_ddb_alloc,
			       struct skl_ddb_entry *ddb, uint64_t data_rate)
{
	uint32_t size, extra = 0;

	if (data_rate && *iter_data_rate) {
		extra = umin(*iter_size, (uint32_t)((*iter_size * data_rate + *iter_data_rate - 1) /
						    *iter_data_rate));
		*iter_size -= extra;
		*iter_data_rate -= data_rate;
	}
	/* disabled planes keep an explicitly empty entry */
	size = min_ddb_alloc + extra;
	if (size)
		*iter_start = ddb_init(ddb, *iter_start, *iter_start + size);
}

static void skl_check_wm_level(struct skl_wm_level *wm, const struct skl_ddb_entry *ddb)
{
	if (wm->min_ddb_alloc > ddb_size(ddb))
		mm_memset(wm, 0, sizeof(*wm));
}

/* Wa_1408961008, Wa_14012656716, Wa_14017887344, Wa_14017868169: a
 * disabled level keeps sane values, as some power saving features read
 * them anyway. */
static int skl_need_wm_copy_wa(int level, const struct skl_plane_wm *wm)
{
	return level > 0 && !wm->wm[level].enable;
}

/* The planes' shares of the pipe's range: the cursor a fixed piece at
 * the end, then the highest level all planes can have, then the rest by
 * relative data rate. */
static int skl_crtc_allocate_plane_ddb(const struct intel_wm_pipe_cfg *c, struct skl_wm_state *st,
				       int pipe)
{
	struct skl_pipe_state *ps = &st->pipe[pipe];
	const struct skl_ddb_entry *alloc = &st->dbuf.ddb[pipe];
	int num_active = hweight8(st->dbuf.active_pipes);
	uint32_t iter_start, iter_size, cursor_size, blocks = 0;
	uint64_t iter_data_rate;
	int level;

	mm_memset(ps->plane_ddb, 0, sizeof(ps->plane_ddb));
	mm_memset(ps->min_ddb, 0, sizeof(ps->min_ddb));
	mm_memset(ps->interim_ddb, 0, sizeof(ps->interim_ddb));
	ps->level = 0;
	if (!c->active)
		return 0;
	iter_start = alloc->start;
	iter_size = ddb_size(alloc);
	if (iter_size == 0)
		return 0;

	cursor_size = skl_cursor_allocation(c, num_active);
	if (cursor_size > iter_size)
		cursor_size = iter_size;
	iter_size -= cursor_size;
	ddb_init(&ps->plane_ddb[WM_CURSOR], alloc->end - cursor_size, alloc->end);

	iter_data_rate = ps->rel_data_rate;

	for (level = g_wm.num_levels - 1; level >= 0; level--) {
		blocks = 0;
		if (ps->planes[WM_CURSOR].wm[level].min_ddb_alloc >
		    ddb_size(&ps->plane_ddb[WM_CURSOR])) {
			blocks = U32_MAX_;
		} else {
			blocks += ps->planes[WM_PRIMARY].wm[level].min_ddb_alloc;
		}
		if (blocks <= iter_size) {
			iter_size -= blocks;
			break;
		}
	}
	if (level < 0) {
		i915_dbg("[drm] i915: pipe %c: the configuration exceeds the data buffer (needs %u of %u blocks)\n",
			 'A' + pipe, blocks, iter_size);
		return -EINVAL;
	}
	ps->level = (uint8_t)level;
	if (iter_data_rate == 0)
		iter_size = 0;

	allocate_plane_ddb(&iter_start, &iter_size, &iter_data_rate,
			   ps->planes[WM_PRIMARY].wm[level].min_ddb_alloc,
			   &ps->plane_ddb[WM_PRIMARY], ps->rel_data_rate);
	if (g_wm.ver >= 30) {
		ps->min_ddb[WM_PRIMARY] = ps->planes[WM_PRIMARY].wm[0].min_ddb_alloc;
		ps->interim_ddb[WM_PRIMARY] = ps->planes[WM_PRIMARY].sagv.wm0.min_ddb_alloc;
	}

	/* every level was assumed possible; switch off the ones that are not */
	for (level++; level < g_wm.num_levels; level++) {
		for (int p = 0; p < WM_NPLANES; p++) {
			struct skl_plane_wm *wm = &ps->planes[p];
			skl_check_wm_level(&wm->wm[level], &ps->plane_ddb[p]);
			if (skl_need_wm_copy_wa(level, wm)) {
				wm->wm[level].blocks = wm->wm[level - 1].blocks;
				wm->wm[level].lines = wm->wm[level - 1].lines;
				wm->wm[level].ignore_lines = wm->wm[level - 1].ignore_lines;
			}
		}
	}
	/* and the transition and SAGV watermarks that do not fit */
	for (int p = 0; p < WM_NPLANES; p++) {
		struct skl_plane_wm *wm = &ps->planes[p];
		skl_check_wm_level(&wm->trans_wm, &ps->plane_ddb[p]);
		skl_check_wm_level(&wm->sagv.wm0, &ps->plane_ddb[p]);
		if (g_wm.ver >= 30 && p == WM_PRIMARY)
			ps->interim_ddb[p] = wm->sagv.wm0.min_ddb_alloc;
		skl_check_wm_level(&wm->sagv.trans_wm, &ps->plane_ddb[p]);
	}
	return 0;
}

/* When the DDB cannot hold even level 0: give the primary plane what
 * there is and keep level 0 at what fits, rather than leave the plane
 * with nothing (the configuration was not checked first). */
static void skl_crtc_allocate_fallback(struct skl_wm_state *st, int pipe)
{
	struct skl_pipe_state *ps = &st->pipe[pipe];
	const struct skl_ddb_entry *alloc = &st->dbuf.ddb[pipe];
	uint32_t size = ddb_size(alloc), cur = size >= 16 ? 8 : size / 2;

	mm_memset(ps->plane_ddb, 0, sizeof(ps->plane_ddb));
	ddb_init(&ps->plane_ddb[WM_CURSOR], alloc->end - cur, alloc->end);
	ddb_init(&ps->plane_ddb[WM_PRIMARY], alloc->start, alloc->end - cur);
	for (int p = 0; p < WM_NPLANES; p++) {
		struct skl_plane_wm *wm = &ps->planes[p];
		uint32_t s = ddb_size(&ps->plane_ddb[p]);
		for (int level = 1; level < WM_MAX_LEVELS; level++)
			mm_memset(&wm->wm[level], 0, sizeof(wm->wm[level]));
		mm_memset(&wm->trans_wm, 0, sizeof(wm->trans_wm));
		mm_memset(&wm->sagv, 0, sizeof(wm->sagv));
		if (wm->wm[0].enable && s && wm->wm[0].blocks >= s)
			wm->wm[0].blocks = (uint16_t)(s - 1);
	}
	ps->level = 0;
}

/* ---- SAGV per pipe ----------------------------------------------------------------------- */

static int skl_crtc_can_enable_sagv(const struct skl_pipe_state *ps)
{
	int max_level = 0x7fffffff;

	for (int p = 0; p < WM_NPLANES; p++) {
		const struct skl_plane_wm *wm = &ps->planes[p];
		int level;
		if (!wm->wm[0].enable)
			continue;
		for (level = g_wm.num_levels - 1; level > 0 && !wm->wm[level].enable; level--)
			;
		if (level < max_level)
			max_level = level;
	}
	if (max_level == 0x7fffffff)
		return 1;
	for (int p = 0; p < WM_NPLANES; p++) {
		const struct skl_plane_wm *wm = &ps->planes[p];
		if (wm->wm[0].enable && !wm->wm[max_level].can_sagv)
			return 0;
	}
	return 1;
}

static int tgl_crtc_can_enable_sagv(const struct skl_pipe_state *ps)
{
	for (int p = 0; p < WM_NPLANES; p++) {
		const struct skl_plane_wm *wm = &ps->planes[p];
		if (wm->wm[0].enable && !wm->sagv.wm0.enable)
			return 0;
	}
	return 1;
}

static int crtc_can_enable_sagv(struct i915_device *i915, const struct skl_pipe_state *ps)
{
	if (!intel_sagv_block_time_us(i915))
		return 0;
	if (!ps->active)
		return 1;
	if (has_sagv_wm(i915))
		return tgl_crtc_can_enable_sagv(ps);
	return skl_crtc_can_enable_sagv(ps);
}

/* ---- line time --------------------------------------------------------------------------- */

uint32_t intel_wm_linetime(struct i915_device *i915, const struct intel_wm_pipe_cfg *c)
{
	uint32_t lt, rate;

	if (!c->active || !c->clock_khz)
		return 0;
	if (intel_wm_display_ver(i915) < 9) {
		lt = (uint32_t)(((uint64_t)c->htotal * 1000 * 8 + c->clock_khz / 2) / c->clock_khz);
		return umin(lt, 0x1ff);
	}
	rate = pipe_pixel_rate(c);
	lt = div_round_up((uint64_t)c->htotal * 1000 * 8, rate);
	/* Display WA #1135: Broxton, Gemini Lake with IPC */
	if ((plat_is(i915, I915_PLATFORM_BROXTON) || plat_is(i915, I915_PLATFORM_GEMINILAKE)) &&
	    g_wm.ipc_enabled)
		lt /= 2;
	return umin(lt, 0x1ff);
}

/* ---- the computation ------------------------------------------------------------------------ */

static int skl_compute(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		       struct skl_wm_state *st, struct intel_wm_result *res, int strict)
{
	struct skl_dbuf_state *db = &st->dbuf;
	int ret = 0;
	int n = npipes(i915);

	mm_memset(st, 0, sizeof(*st));
	if (res)
		mm_memset(res, 0, sizeof(*res));

	/* the watermarks of every plane, and the levels the vblank allows */
	for (int pipe = 0; pipe < n; pipe++) {
		const struct intel_wm_pipe_cfg *c = &cfg[pipe];
		struct skl_pipe_state *ps = &st->pipe[pipe];

		if (!c->active || !c->clock_khz || !c->htotal)
			continue;
		ps->active = 1;
		db->active_pipes |= BIT8(pipe);
		skl_build_plane_wm(c, 0, &ps->planes[WM_PRIMARY]);
		skl_build_plane_wm(c, 1, &ps->planes[WM_CURSOR]);
		if (skl_wm_check_vblank(c, ps)) {
			i915_dbg("[drm] i915: pipe %c: the vertical blank is too short for the prefill\n",
				 'A' + pipe);
			if (strict)
				ret = -EINVAL;
		}
		if (c->plane_on) {
			ps->rel_data_rate = (uint64_t)c->plane_w * c->plane_h * c->cpp;
			ps->data_rate = pipe_pixel_rate(c) * c->cpp;
		}
		ps->linetime = (uint16_t)intel_wm_linetime(i915, c);
	}

	/* the slices of every pipe, MBUS joining */
	db->joined_mbus = has_mbus_joining(i915) ? (uint8_t)adlp_check_mbus_joined(db->active_pipes)
						 : 0;
	db->enabled_slices = BIT8(DBUF_S1);
	for (int pipe = 0; pipe < n; pipe++) {
		db->slices[pipe] = skl_compute_dbuf_slices(i915, pipe, db->active_pipes,
							   db->joined_mbus);
		db->enabled_slices |= db->slices[pipe];
	}
	db->mdclk_cdclk_ratio = (uint8_t)intel_cdclk_mdclk_ratio(i915);
	for (int pipe = 0; pipe < n; pipe++)
		db->weight[pipe] = st->pipe[pipe].active ? cfg[pipe].hdisplay : 0;
	for (int pipe = 0; pipe < n; pipe++)
		skl_crtc_allocate_ddb(st, pipe);
	for (int pipe = 0; pipe < n; pipe++) {
		if (!st->pipe[pipe].active)
			continue;
		if (skl_crtc_allocate_plane_ddb(&cfg[pipe], st, pipe)) {
			if (strict)
				ret = -EINVAL;
			skl_crtc_allocate_fallback(st, pipe);
		}
	}

	/* which watermarks (SAGV or not) each pipe runs with */
	for (int pipe = 0; pipe < n; pipe++) {
		struct skl_pipe_state *ps = &st->pipe[pipe];
		ps->can_sagv = (uint8_t)crtc_can_enable_sagv(i915, ps);
		ps->use_sagv_wm = has_sagv_wm(i915) && !has_hw_sagv_wm() && ps->can_sagv;
	}

	if (res) {
		uint32_t max_bw[4] = { 0, 0, 0, 0 };
		uint32_t planes[4] = { 0, 0, 0, 0 };
		uint32_t total = 0;

		res->active_pipes = db->active_pipes;
		res->joined_mbus = db->joined_mbus;
		res->enabled_slices = db->enabled_slices;
		for (int pipe = 0; pipe < n; pipe++) {
			struct skl_pipe_state *ps = &st->pipe[pipe];
			uint8_t mask;
			if (!ps->active)
				continue;
			if (!ps->can_sagv)
				res->sagv_reject |= BIT8(pipe);
			res->level[pipe] = ps->level;
			if (!cfg[pipe].plane_on)
				continue;
			res->num_active_planes[pipe] = 1;
			res->data_rate[pipe] = ps->data_rate;
			/* the arbiter shares a slice's bandwidth equally among
			 * its planes */
			mask = ddb_dbuf_slice_mask(&ps->plane_ddb[WM_PRIMARY]);
			for (int s = 0; s < 4; s++) {
				if (!(mask & BIT8(s)))
					continue;
				max_bw[s] = umax(max_bw[s], ps->data_rate);
				planes[s]++;
			}
		}
		for (int s = 0; s < 4; s++)
			total = umax(total, max_bw[s] * planes[s]);
		res->dbuf_bw_min_cdclk = div_round_up(total, 64);
	}
	return ret;
}

int intel_wm_compute(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		     struct intel_wm_result *res)
{
	if (!g_wm.ready || g_wm.ver < 9) {
		if (res) {
			mm_memset(res, 0, sizeof(*res));
			for (int pipe = 0; pipe < npipes(i915); pipe++) {
				if (!cfg[pipe].active)
					continue;
				res->active_pipes |= BIT8(pipe);
				if (cfg[pipe].plane_on) {
					res->num_active_planes[pipe] = 1;
					res->data_rate[pipe] = pipe_pixel_rate(&cfg[pipe]) *
							       cfg[pipe].cpp;
				}
			}
		}
		return 0;
	}
	return skl_compute(i915, cfg, &g_check, res, 1);
}

int intel_wm_check(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		   struct intel_wm_result *res)
{
	struct intel_wm_result local;
	uint16_t qgv;
	uint8_t vl;
	int ret;

	if (!res)
		res = &local;
	static unsigned said;
	if (g_wm.ready && g_wm.ver < 9) {
		ret = hsw_wm_check(i915, cfg);
		if (ret) {
			if (said++ < 4)
				kprintf("[drm] i915: refused: the watermarks cannot be met (%d)\n", ret);
			return ret;
		}
		intel_wm_compute(i915, cfg, res);
	} else {
		ret = intel_wm_compute(i915, cfg, res);
		if (ret) {
			if (said++ < 4)
				kprintf("[drm] i915: refused: the display buffer cannot feed the planes (%d)\n",
					ret);
			return ret;
		}
	}
	ret = intel_bw_check(i915, res, &qgv);
	if (ret) {
		if (said++ < 4)
			kprintf("[drm] i915: refused: memory bandwidth (%d)\n", ret);
		return ret;
	}
	uint32_t need = intel_cdclk_required_khz(i915, cfg, &vl);
	if (need > intel_cdclk_max_khz(i915)) {
		if (said++ < 4)
			kprintf("[drm] i915: refused: needs CDCLK %u kHz, at most %u\n", need,
				intel_cdclk_max_khz(i915));
		return -EINVAL;
	}
	return 0;
}

/* ---- programming ----------------------------------------------------------------------------- */

static uint32_t plane_wm_reg_val(const struct skl_wm_level *l)
{
	uint32_t v = 0;

	if (l->enable)
		v |= SKL_WM_EN;
	if (l->ignore_lines)
		v |= SKL_WM_IGNORE_LINES;
	if (l->auto_min_alloc_wm_enable)
		v |= SKL_WM_AUTO_MIN_ALLOC_EN;
	return v | SKL_PLANE_WM_BLOCKS(l->blocks) | SKL_WM_LINES(l->lines);
}

static uint32_t cursor_wm_reg_val(const struct skl_wm_level *l)
{
	uint32_t v = 0;

	if (l->enable)
		v |= SKL_WM_EN;
	if (l->ignore_lines)
		v |= SKL_WM_IGNORE_LINES;
	return v | SKL_CUR_WM_BLOCKS(l->blocks) | SKL_WM_LINES(l->lines);
}

static uint32_t plane_ddb_reg_val(const struct skl_ddb_entry *e)
{
	if (!e->end)
		return 0;
	return SKL_PLANE_BUF_END(e->end - 1u) | SKL_PLANE_BUF_START(e->start);
}

static uint32_t cursor_ddb_reg_val(const struct skl_ddb_entry *e)
{
	if (!e->end)
		return 0;
	return SKL_CUR_BUF_END(e->end - 1u) | SKL_CUR_BUF_START(e->start);
}

static uint32_t min_ddb_reg_val(uint16_t min_ddb, uint16_t interim)
{
	uint32_t v = 0;

	if (min_ddb)
		v |= PLANE_MIN_DBUF_BLOCKS(min_ddb);
	if (interim)
		v |= PLANE_INTERIM_DBUF_BLOCKS(interim);
	return v ? v | PLANE_AUTO_MIN_DBUF_EN : 0;
}

static const struct skl_wm_level *sel_wm_level(const struct skl_pipe_state *ps, int p, int level)
{
	if (level == 0 && ps->use_sagv_wm)
		return &ps->planes[p].sagv.wm0;
	return &ps->planes[p].wm[level];
}

static const struct skl_wm_level *sel_trans_wm(const struct skl_pipe_state *ps, int p)
{
	return ps->use_sagv_wm ? &ps->planes[p].sagv.trans_wm : &ps->planes[p].trans_wm;
}

/* All the watermark and DDB registers of a pipe from `ps' (NULL: all
 * zero), then -- `arm' -- the planes that are on rearmed with their own
 * surface address so that the new values latch at the next vblank.
 * The universal planes this driver does not use are switched off. */
static void skl_write_pipe(struct i915_device *i915, int pipe, const struct skl_pipe_state *ps,
			   int arm)
{
	static const struct skl_pipe_state zero;
	int nplanes = num_planes_on_pipe(i915, pipe);
	int hw_sagv = has_hw_sagv_wm();

	if (!ps)
		ps = &zero;
	for (int plane = 0; plane < nplanes; plane++) {
		const struct skl_pipe_state *src = plane == 0 ? ps : &zero;
		for (int level = 0; level < g_wm.num_levels; level++)
			i915_write32(i915, PLANE_WM(pipe, plane, level),
				     plane_wm_reg_val(sel_wm_level(src, WM_PRIMARY, level)));
		i915_write32(i915, PLANE_WM_TRANS(pipe, plane),
			     plane_wm_reg_val(sel_trans_wm(src, WM_PRIMARY)));
		if (hw_sagv) {
			i915_write32(i915, PLANE_WM_SAGV(pipe, plane),
				     plane_wm_reg_val(&src->planes[WM_PRIMARY].sagv.wm0));
			i915_write32(i915, PLANE_WM_SAGV_TRANS(pipe, plane),
				     plane_wm_reg_val(&src->planes[WM_PRIMARY].sagv.trans_wm));
		}
		i915_write32(i915, PLANE_BUF_CFG(pipe, plane),
			     plane_ddb_reg_val(&src->plane_ddb[WM_PRIMARY]));
		if (g_wm.ver < 11)
			i915_write32(i915, PLANE_NV12_BUF_CFG(pipe, plane), 0);
		if (g_wm.ver >= 30)
			i915_write32(i915, PLANE_MIN_BUF_CFG(pipe, plane),
				     min_ddb_reg_val(src->min_ddb[WM_PRIMARY],
						     src->interim_ddb[WM_PRIMARY]));
	}
	for (int level = 0; level < g_wm.num_levels; level++)
		i915_write32(i915, CUR_WM(pipe, level),
			     cursor_wm_reg_val(sel_wm_level(ps, WM_CURSOR, level)));
	i915_write32(i915, CUR_WM_TRANS(pipe), cursor_wm_reg_val(sel_trans_wm(ps, WM_CURSOR)));
	if (hw_sagv) {
		i915_write32(i915, CUR_WM_SAGV(pipe),
			     cursor_wm_reg_val(&ps->planes[WM_CURSOR].sagv.wm0));
		i915_write32(i915, CUR_WM_SAGV_TRANS(pipe),
			     cursor_wm_reg_val(&ps->planes[WM_CURSOR].sagv.trans_wm));
	}
	i915_write32(i915, CUR_BUF_CFG(pipe), cursor_ddb_reg_val(&ps->plane_ddb[WM_CURSOR]));

	for (int plane = 0; plane < nplanes; plane++) {
		uint32_t ctl = i915_read32(i915, PLANE_CTL(pipe, plane));
		if (plane > 0 && (ctl & SKL_PLANE_CTL_ENABLE)) {
			i915_write32(i915, PLANE_CTL(pipe, plane), 0);
			i915_write32(i915, PLANE_SURF(pipe, plane), 0);
		} else if (arm && (ctl & SKL_PLANE_CTL_ENABLE)) {
			i915_write32(i915, PLANE_SURF(pipe, plane),
				     i915_read32(i915, PLANE_SURF(pipe, plane)));
		}
	}
	if (arm && (i915_read32(i915, CUR_CTL(pipe)) & CUR_MODE_MASK))
		i915_write32(i915, CUR_BASE(pipe), i915_read32(i915, CUR_BASE(pipe)));
	(void)i915_read32(i915, CUR_BUF_CFG(pipe));
}

static void wait_for_vblank(struct i915_device *i915, int pipe)
{
	uint32_t f0 = i915_read32(i915, PIPE_FRMCOUNT(pipe));

	for (int t = 0; t < 1000; t++) {
		if (i915_read32(i915, PIPE_FRMCOUNT(pipe)) != f0)
			return;
		lapic_delay_us(100);
	}
}

static int pipe_state_equal(const struct skl_pipe_state *a, const struct skl_pipe_state *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;

	for (uint32_t i = 0; i < sizeof(*a); i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static int ddb_overlaps(const struct skl_ddb_entry *ddb, const struct skl_ddb_entry *entries,
			int ignore)
{
	for (int i = 0; i < INTEL_MAX_PIPES; i++) {
		if (i == ignore)
			continue;
		if (ddb->start < entries[i].end && entries[i].start < ddb->end)
			return 1;
	}
	return 0;
}

/* ---- DBUF slices, MBUS -------------------------------------------------------------------- */

static void dbuf_slice_set(struct i915_device *i915, int slice, int enable)
{
	uint32_t reg = DBUF_CTL_S(slice);
	uint32_t v = i915_read32(i915, reg);

	v = enable ? (v | SKL_DBUF_POWER_REQUEST) : (v & ~SKL_DBUF_POWER_REQUEST);
	i915_write32(i915, reg, v);
	(void)i915_read32(i915, reg);
	lapic_delay_us(10);
	if (!!(i915_read32(i915, reg) & SKL_DBUF_POWER_STATE) != !!enable)
		kprintf("[drm] i915: DBUF slice %d power %s timed out\n", slice + 1,
			enable ? "up" : "down");
}

static void dbuf_slices_update(struct i915_device *i915, uint8_t req)
{
	uint8_t mask = g_wm.plat->dbuf_slice_mask;

	if (req & ~mask)
		kprintf("[drm] i915: invalid DBUF slices 0x%x requested (have 0x%x)\n", req, mask);
	i915_dbg("[drm] i915: DBUF slices -> 0x%x\n", req);
	for (int s = 0; s < 4; s++)
		if (mask & BIT8(s))
			dbuf_slice_set(i915, s, !!(req & BIT8(s)));
	i915->display.dbuf_slices = req;
}

static uint8_t enabled_dbuf_slices_hw(struct i915_device *i915)
{
	uint8_t en = 0;

	for (int s = 0; s < 4; s++)
		if ((g_wm.plat->dbuf_slice_mask & BIT8(s)) &&
		    (i915_read32(i915, DBUF_CTL_S(s)) & SKL_DBUF_POWER_STATE))
			en |= BIT8(s);
	return en;
}

/* The slice set of display version 14 is part of the PM demand; it must
 * be raised before slices are powered and lowered after. */
static void dbuf_slices_change(struct i915_device *i915, uint8_t req, int raising)
{
	if (g_wm.ver >= 14 && raising) {
		i915->display.dbuf_slices = req;
		intel_pmdemand_update(i915);
	}
	dbuf_slices_update(i915, req);
	if (g_wm.ver >= 14 && !raising)
		intel_pmdemand_update(i915);
}

void intel_dbuf_hw_init(struct i915_device *i915)
{
	uint8_t slices;

	if (!g_wm.plat) {
		g_wm.plat = intel_wm_platform_get(i915);
		g_wm.ver = g_wm.plat ? g_wm.plat->ip / 100 : 0;
	}
	if (!g_wm.plat || g_wm.ver < 9)
		return;
	if (g_wm.ver == 12 || plat_is(i915, I915_PLATFORM_DG2))
		for (int s = 0; s < 4; s++) {
			uint32_t v;
			if (!(g_wm.plat->dbuf_slice_mask & BIT8(s)))
				continue;
			v = i915_read32(i915, DBUF_CTL_S(s));
			v &= ~SKL_DBUF_TRACKER_STATE_SERVICE_MASK;
			v |= SKL_DBUF_TRACKER_STATE_SERVICE(8);
			i915_write32(i915, DBUF_CTL_S(s), v);
		}
	/* at least slice 1; what else is needed comes with the pipes */
	slices = BIT8(DBUF_S1) | enabled_dbuf_slices_hw(i915);
	if (g_wm.ver >= 14)
		intel_pmdemand_program_dbuf(i915, slices);
	dbuf_slices_update(i915, slices);
	g_wm.hw.dbuf.enabled_slices = slices;

	/* the ABOX credits (Ice Lake through display version 13, not Alder
	 * Lake-P); version 12 reads through ABOX 1 and 2 but wants 0 set
	 * as well */
	if (g_wm.ver >= 11 && g_wm.ver < 14 && !plat_is(i915, I915_PLATFORM_ALDERLAKE_P)) {
		uint32_t aboxes = g_wm.plat->abox_mask | (g_wm.ver == 12 ? 1u : 0u);
		uint32_t mask = SKL_MBUS_ABOX_BT_CREDIT_POOL1_MASK |
				SKL_MBUS_ABOX_BT_CREDIT_POOL2_MASK | SKL_MBUS_ABOX_B_CREDIT_MASK |
				SKL_MBUS_ABOX_BW_CREDIT_MASK;
		uint32_t val = SKL_MBUS_ABOX_BT_CREDIT_POOL1(16) | SKL_MBUS_ABOX_BT_CREDIT_POOL2(16) |
			       SKL_MBUS_ABOX_B_CREDIT(1) | SKL_MBUS_ABOX_BW_CREDIT(1);
		for (int i = 0; i < 3; i++) {
			if (!(aboxes & (1u << i)))
				continue;
			uint32_t v = i915_read32(i915, SKL_MBUS_ABOX_CTL(i));
			i915_write32(i915, SKL_MBUS_ABOX_CTL(i), (v & ~mask) | val);
		}
	}
}

static int xelpdp_is_only_pipe_per_dbuf_bank(int pipe, uint8_t active_pipes)
{
	switch (pipe) {
	case PIPE_A:
	case PIPE_D:
		active_pipes &= BIT8(PIPE_A) | BIT8(PIPE_D);
		break;
	default:
		active_pipes &= BIT8(PIPE_B) | BIT8(PIPE_C);
		break;
	}
	return is_power_of_2(active_pipes);
}

/* The credits of a pipe's path into MBUS. */
static uint32_t pipe_mbus_dbox_ctl(struct i915_device *i915, int pipe,
				   const struct skl_dbuf_state *db)
{
	uint32_t v = 0;

	if (g_wm.ver >= 14)
		v |= SKL_MBUS_DBOX_I_CREDIT(2);
	if (g_wm.ver >= 12)
		v |= SKL_MBUS_DBOX_B2B_TRANSACTIONS_MAX(16) |
		     SKL_MBUS_DBOX_B2B_TRANSACTIONS_DELAY(1) |
		     SKL_MBUS_DBOX_REGULATE_B2B_TRANSACTIONS_EN;
	if (g_wm.ver >= 14)
		v |= db->joined_mbus ? SKL_MBUS_DBOX_A_CREDIT(12) : SKL_MBUS_DBOX_A_CREDIT(8);
	else if (plat_is(i915, I915_PLATFORM_ALDERLAKE_P)) /* Wa_22010947358 */
		v |= db->joined_mbus ? SKL_MBUS_DBOX_A_CREDIT(6) : SKL_MBUS_DBOX_A_CREDIT(4);
	else
		v |= SKL_MBUS_DBOX_A_CREDIT(2);
	if (g_wm.ver >= 14) {
		v |= SKL_MBUS_DBOX_B_CREDIT(0xA);
	} else if (plat_is(i915, I915_PLATFORM_ALDERLAKE_P)) {
		v |= SKL_MBUS_DBOX_BW_CREDIT(2) | SKL_MBUS_DBOX_B_CREDIT(8);
	} else if (g_wm.ver >= 12) {
		v |= SKL_MBUS_DBOX_BW_CREDIT(2) | SKL_MBUS_DBOX_B_CREDIT(12);
	} else {
		v |= SKL_MBUS_DBOX_BW_CREDIT(1) | SKL_MBUS_DBOX_B_CREDIT(8);
	}
	if (g_wm.plat->ip == 1400) {
		if (xelpdp_is_only_pipe_per_dbuf_bank(pipe, db->active_pipes))
			v |= SKL_MBUS_DBOX_BW_8CREDITS_MTL;
		else
			v |= SKL_MBUS_DBOX_BW_4CREDITS_MTL;
	}
	return v;
}

static void pipe_mbus_dbox_ctl_update(struct i915_device *i915, const struct skl_dbuf_state *db)
{
	for (int pipe = 0; pipe < npipes(i915); pipe++)
		if (db->active_pipes & BIT8(pipe))
			i915_write32(i915, SKL_PIPE_MBUS_DBOX_CTL(pipe),
				     pipe_mbus_dbox_ctl(i915, pipe, db));
}

static void mbus_dbox_update(struct i915_device *i915, const struct skl_dbuf_state *old,
			     const struct skl_dbuf_state *new)
{
	if (g_wm.ver < 11)
		return;
	if (new->joined_mbus == old->joined_mbus && new->active_pipes == old->active_pipes)
		return;
	pipe_mbus_dbox_ctl_update(i915, new);
}

void intel_dbuf_mdclk_ratio_update(struct i915_device *i915, int ratio, int joined_mbus)
{
	if (!g_wm.plat || !has_mbus_joining(i915))
		return;
	if (ratio < 1)
		ratio = 1;
	if (g_wm.ver >= 35) {
		uint32_t v = i915_read32(i915, SKL_MBUS_CTL);
		v &= ~XE3P_MBUS_TRANSLATION_THROTTLE_MIN_MASK;
		i915_write32(i915, SKL_MBUS_CTL, v | XE3P_MBUS_TRANSLATION_THROTTLE_MIN(ratio - 1));
	} else if (g_wm.ver >= 20) {
		uint32_t v = i915_read32(i915, SKL_MBUS_CTL);
		v &= ~MBUS_TRANSLATION_THROTTLE_MIN_MASK;
		i915_write32(i915, SKL_MBUS_CTL, v | MBUS_TRANSLATION_THROTTLE_MIN(ratio - 1));
	}
	if (joined_mbus)
		ratio *= 2;
	i915_dbg("[drm] i915: DBUF ratio %d (MBUS joined: %s)\n", ratio, joined_mbus ? "yes" : "no");
	for (int s = 0; s < 4; s++) {
		uint32_t v;
		if (!(g_wm.plat->dbuf_slice_mask & BIT8(s)))
			continue;
		v = i915_read32(i915, DBUF_CTL_S(s));
		if (g_wm.ver >= 35) {
			v &= ~XE3P_DBUF_MIN_TRACKER_STATE_SERVICE_MASK;
			v |= XE3P_DBUF_MIN_TRACKER_STATE_SERVICE(ratio - 1);
		} else {
			v &= ~SKL_DBUF_MIN_TRACKER_STATE_SERVICE_MASK;
			v |= SKL_DBUF_MIN_TRACKER_STATE_SERVICE(ratio - 1);
		}
		i915_write32(i915, DBUF_CTL_S(s), v);
	}
}

int intel_wm_mbus_joined(struct i915_device *i915)
{
	(void)i915;
	return g_wm.hw.dbuf.joined_mbus;
}

static void mbus_ctl_join_update(struct i915_device *i915, const struct skl_dbuf_state *db, int pipe)
{
	uint32_t v = i915_read32(i915, SKL_MBUS_CTL);

	v &= ~(SKL_MBUS_HASHING_MODE_MASK | SKL_MBUS_JOIN | SKL_MBUS_JOIN_PIPE_SELECT_MASK);
	if (db->joined_mbus)
		v |= SKL_MBUS_HASHING_MODE_1x4 | SKL_MBUS_JOIN;
	else
		v |= SKL_MBUS_HASHING_MODE_2x2;
	v |= pipe >= 0 ? SKL_MBUS_JOIN_PIPE_SELECT(pipe) : SKL_MBUS_JOIN_PIPE_SELECT_NONE;
	i915_write32(i915, SKL_MBUS_CTL, v);
	i915_dbg("[drm] i915: MBUS joined: %s (pipe %c)\n", db->joined_mbus ? "yes" : "no",
		 pipe >= 0 ? 'A' + pipe : '*');
}

/* The pipe MBUS is joined for: the only active one, when it stays up
 * through the change (it is not being enabled). */
static int mbus_joined_pipe(const struct skl_dbuf_state *db, uint8_t update_pipes)
{
	int pipe = ffs8(db->active_pipes) - 1;

	if (pipe >= 0 && (update_pipes & BIT8(pipe)))
		return pipe;
	return -1;
}

static void dbuf_min_tracker_update(struct i915_device *i915, const struct skl_dbuf_state *new)
{
	/* CDCLK (and with it the memory clock ratio) has already been
	 * raised, or will only be lowered after this: the ratio as it is
	 * now is the one in force */
	intel_dbuf_mdclk_ratio_update(i915, intel_cdclk_mdclk_ratio(i915), new->joined_mbus);
}

/* ---- Lunar Lake on: the package C-state latency --------------------------------------------- */

static void program_dpkgc_latency(struct i915_device *i915, const struct skl_wm_state *st)
{
	uint32_t latency = 0, max_linetime = 0, added_wake_time = 0;

	if (g_wm.ver < 20)
		return;
	for (int level = g_wm.num_levels - 1; level >= 1; level--) {
		latency = skl_wm_latency(level, 0, 0);
		if (latency)
			break;
	}
	for (int pipe = 0; pipe < npipes(i915); pipe++)
		max_linetime = umax(max_linetime, div_round_up(st->pipe[pipe].linetime, 8));
	if (max_linetime == 0 || latency == 0) {
		latency = LNL_PKG_C_LATENCY_MAX;
		added_wake_time = 0;
	} else {
		/* Wa_22020299601: a multiple of the line time */
		latency = div_round_up(latency, max_linetime) * max_linetime;
	}
	i915_write32(i915, LNL_PKG_C_LATENCY,
		     LNL_ADDED_WAKE_TIME(added_wake_time) | LNL_PKG_C_LATENCY_FIELD(latency));
}

/* ---- the update --------------------------------------------------------------------------- */

static void write_linetime(struct i915_device *i915, int pipe, const struct skl_pipe_state *ps)
{
	i915_write32(i915, WM_LINETIME(pipe), HSW_LINETIME(ps->linetime) |
					      HSW_IPS_LINETIME(ps->ips_linetime));
}

void intel_wm_update(struct i915_device *i915)
{
	struct intel_wm_result res;
	struct skl_wm_state *new = &g_new;
	struct skl_wm_state *old = &g_wm.hw;
	struct skl_ddb_entry entries[INTEL_MAX_PIPES];
	uint8_t update_pipes = 0, modeset_pipes = 0;
	int n = npipes(i915);

	if (!g_wm.ready)
		return;
	intel_wm_cfg_from_hw(i915, g_cfg);
	if (g_wm.ver < 9) {
		hsw_wm_update(i915, g_cfg);
		return;
	}
	/* nothing of ours running: what MBUS and the slices are is what the
	 * registers say (the firmware, or a bring-up sequence, may have
	 * changed them) */
	if (!old->dbuf.active_pipes) {
		if (has_mbus_joining(i915))
			old->dbuf.joined_mbus = !!(i915_read32(i915, SKL_MBUS_CTL) & SKL_MBUS_JOIN);
		old->dbuf.enabled_slices = enabled_dbuf_slices_hw(i915);
	}
	if (skl_compute(i915, g_cfg, new, &res, 0) && !g_wm.warned) {
		g_wm.warned = 1;
		kprintf("[drm] i915: the data buffer cannot hold this configuration; watermarks limited\n");
	}

	mm_memset(entries, 0, sizeof(entries));
	for (int pipe = 0; pipe < n; pipe++) {
		if (!new->pipe[pipe].active)
			continue;
		if (old->pipe[pipe].active) {
			entries[pipe] = old->pipe[pipe].ddb;
			update_pipes |= BIT8(pipe);
		} else {
			modeset_pipes |= BIT8(pipe);
		}
	}

	/* memory frequency points, before anything may need more */
	intel_bw_pre_update(i915, &res);

	/* the slices of the old and new configuration both powered */
	if ((old->dbuf.enabled_slices | new->dbuf.enabled_slices) != old->dbuf.enabled_slices)
		dbuf_slices_change(i915, old->dbuf.enabled_slices | new->dbuf.enabled_slices, 1);

	/* MBUS joined before the pipe that gets it grows into it (and, with
	 * none of our pipes running yet, joined afresh over whatever the
	 * firmware left selected) */
	if ((!old->dbuf.joined_mbus || !old->dbuf.active_pipes) && new->dbuf.joined_mbus) {
		mbus_ctl_join_update(i915, &new->dbuf, mbus_joined_pipe(&new->dbuf, update_pipes));
		mbus_dbox_update(i915, &old->dbuf, &new->dbuf);
		dbuf_min_tracker_update(i915, &new->dbuf);
	}

	/* The pipes that stay on, in an order that never overlaps one's new
	 * share with another's current one; a pipe whose share moved gets
	 * a frame for it to take effect before the next one moves. */
	while (update_pipes) {
		uint8_t progress = 0;
		for (int pipe = n - 1; pipe >= 0; pipe--) {
			if (!(update_pipes & BIT8(pipe)))
				continue;
			if (ddb_overlaps(&new->pipe[pipe].ddb, entries, pipe))
				continue;
			entries[pipe] = new->pipe[pipe].ddb;
			update_pipes &= (uint8_t)~BIT8(pipe);
			progress = 1;
			/* only the planes whose values change are touched */
			if (pipe_state_equal(&new->pipe[pipe], &old->pipe[pipe]))
				continue;
			skl_write_pipe(i915, pipe, &new->pipe[pipe], 1);
			write_linetime(i915, pipe, &new->pipe[pipe]);
			if (!ddb_equal(&new->pipe[pipe].ddb, &old->pipe[pipe].ddb) &&
			    (update_pipes | modeset_pipes))
				wait_for_vblank(i915, pipe);
		}
		if (!progress) {
			/* cannot happen with the tables above; do not spin */
			for (int pipe = 0; pipe < n; pipe++)
				if (update_pipes & BIT8(pipe)) {
					skl_write_pipe(i915, pipe, &new->pipe[pipe], 1);
					write_linetime(i915, pipe, &new->pipe[pipe]);
				}
			update_pipes = 0;
		}
	}

	/* MBUS split after the pipe that had it shrank out of it, or the
	 * credits and trackers for a different set of pipes */
	if (old->dbuf.joined_mbus && !new->dbuf.joined_mbus) {
		uint8_t stay = 0;
		for (int pipe = 0; pipe < n; pipe++)
			if (old->pipe[pipe].active && new->pipe[pipe].active)
				stay |= BIT8(pipe);
		int pipe = mbus_joined_pipe(&old->dbuf, stay);
		dbuf_min_tracker_update(i915, &new->dbuf);
		mbus_dbox_update(i915, &old->dbuf, &new->dbuf);
		mbus_ctl_join_update(i915, &new->dbuf, pipe);
		if (pipe >= 0)
			wait_for_vblank(i915, pipe);
	} else if (old->dbuf.joined_mbus == new->dbuf.joined_mbus &&
		   old->dbuf.active_pipes != new->dbuf.active_pipes) {
		dbuf_min_tracker_update(i915, &new->dbuf);
		mbus_dbox_update(i915, &old->dbuf, &new->dbuf);
	}

	/* the pipes just enabled: their planes come on after this */
	for (int pipe = 0; pipe < n; pipe++) {
		if (!(modeset_pipes & BIT8(pipe)))
			continue;
		skl_write_pipe(i915, pipe, &new->pipe[pipe], 1);
		write_linetime(i915, pipe, &new->pipe[pipe]);
	}
	/* pipes that went off without intel_wm_pipe_disable() (their power
	 * well may already be down: only the bookkeeping changes) */

	/* only the slices the new configuration uses */
	if (new->dbuf.enabled_slices != (old->dbuf.enabled_slices | new->dbuf.enabled_slices))
		dbuf_slices_change(i915, new->dbuf.enabled_slices, 0);
	else
		i915->display.dbuf_slices = new->dbuf.enabled_slices;

	intel_bw_post_update(i915, &res);
	program_dpkgc_latency(i915, new);

	*old = *new;
	for (int pipe = 0; pipe < n; pipe++)
		if (new->pipe[pipe].active)
			i915_dbg("[drm] i915: pipe %c: DDB %u-%u (slices 0x%x%s), plane %u-%u, cursor %u-%u, level %u, wm0 %u blocks/%u lines\n",
				 'A' + pipe, new->pipe[pipe].ddb.start, new->pipe[pipe].ddb.end,
				 new->dbuf.slices[pipe], new->dbuf.joined_mbus ? ", MBUS joined" : "",
				 new->pipe[pipe].plane_ddb[WM_PRIMARY].start,
				 new->pipe[pipe].plane_ddb[WM_PRIMARY].end,
				 new->pipe[pipe].plane_ddb[WM_CURSOR].start,
				 new->pipe[pipe].plane_ddb[WM_CURSOR].end, new->pipe[pipe].level,
				 new->pipe[pipe].planes[WM_PRIMARY].wm[0].blocks,
				 new->pipe[pipe].planes[WM_PRIMARY].wm[0].lines);
}

void intel_wm_pipe_disable(struct i915_device *i915, int pipe)
{
	if (!g_wm.ready || pipe < 0 || pipe >= npipes(i915))
		return;
	if (g_wm.ver < 9) {
		hsw_wm_pipe_disable(i915, pipe);
		return;
	}
	/* the planes are off: their watermarks and shares go to zero with
	 * them (the disable writes arm the values) */
	skl_write_pipe(i915, pipe, NULL, 0);
	for (int plane = 0; plane < num_planes_on_pipe(i915, pipe); plane++) {
		i915_write32(i915, PLANE_CTL(pipe, plane), 0);
		i915_write32(i915, PLANE_SURF(pipe, plane), 0);
	}
	i915_write32(i915, CUR_CTL(pipe), 0);
	i915_write32(i915, CUR_BASE(pipe), 0);
	(void)i915_read32(i915, CUR_BASE(pipe));
	mm_memset(&g_wm.hw.pipe[pipe], 0, sizeof(g_wm.hw.pipe[pipe]));
	mm_memset(&g_wm.hw.dbuf.ddb[pipe], 0, sizeof(g_wm.hw.dbuf.ddb[pipe]));
	g_wm.hw.dbuf.weight[pipe] = 0;
	g_wm.hw.dbuf.active_pipes &= (uint8_t)~BIT8(pipe);
}

/* ---- initialisation ------------------------------------------------------------------------- */

int intel_wm_init(struct i915_device *i915)
{
	g_wm.i915 = i915;
	g_wm.plat = intel_wm_platform_get(i915);
	if (!g_wm.plat) {
		kprintf("[drm] i915: no watermark parameters for %s\n", i915->info->name);
		return -ENODEV;
	}
	g_wm.ver = g_wm.plat->ip / 100;
	g_wm.ipc_enabled = 0;
	g_wm.warned = 0;
	mm_memset(&g_wm.hw, 0, sizeof(g_wm.hw));

	intel_dram_detect(i915);
	intel_bw_init_hw(i915);
	intel_bw_init(i915);
	if (g_wm.ver < 9) {
		int rc = hsw_wm_init(i915);
		g_wm.ready = rc == 0;
		return rc;
	}
	skl_setup_wm_latency(i915);
	skl_ipc_init(i915);

	/* what the firmware left: powered slices, MBUS joining */
	g_wm.hw.dbuf.enabled_slices = enabled_dbuf_slices_hw(i915);
	if (has_mbus_joining(i915))
		g_wm.hw.dbuf.joined_mbus = !!(i915_read32(i915, SKL_MBUS_CTL) & SKL_MBUS_JOIN);
	g_wm.hw.dbuf.mdclk_cdclk_ratio = (uint8_t)intel_cdclk_mdclk_ratio(i915);
	i915->display.dbuf_slices = g_wm.hw.dbuf.enabled_slices;
	g_wm.ready = 1;
	i915_dbg("[drm] i915: watermarks: display %u.%02u, DBUF %u blocks in slices 0x%x, IPC %s, SAGV %s (block time %u us)\n",
		 g_wm.plat->ip / 100, g_wm.plat->ip % 100, g_wm.plat->dbuf_size,
		 g_wm.plat->dbuf_slice_mask, g_wm.ipc_enabled ? "on" : "off",
		 intel_has_sagv(i915) ? "controlled" : "not controlled",
		 intel_sagv_block_time_us(i915));
	return 0;
}
