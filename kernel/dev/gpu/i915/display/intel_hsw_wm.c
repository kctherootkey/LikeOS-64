// LikeOS -- watermarks of Haswell and Broadwell.
//
// These parts have one display FIFO shared by the pipes, not a buffer
// handed out in shares.  Each pipe has a level-0 watermark (WM0) for
// its primary plane, sprite and cursor, in 64-byte cachelines; on top
// of that three low-power levels (LP1-LP3) are common to all pipes and
// are what lets the memory enter its self-refresh states.  They are
// worked out per pipe from the memory latencies the memory controller
// publishes (SSKPD), merged over the active pipes, checked against what
// the FIFO and the register fields hold, and written in the order the
// hardware needs: a low-power level that is about to change is switched
// off before anything else moves.  Values that would be too small for
// the frame still on screen are avoided by programming the larger of
// the old and new ones first and the new ones a frame later.  The line
// time (and, for pipe A, the IPS line time from CDCLK) goes into
// WM_LINETIME.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_wm_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define ILK_MAX_LEVELS 5
#define HSW_PIPES 3

enum ddb_partitioning {
	DDB_PART_1_2 = 0,
	DDB_PART_5_6, /* the sprite gets 5/6 (IVB+, one pipe) */
};

struct ilk_wm_level {
	uint8_t enable;
	uint32_t pri_val, spr_val, cur_val, fbc_val;
};

struct ilk_pipe_wm {
	struct ilk_wm_level wm[ILK_MAX_LEVELS];
	uint8_t pipe_enabled;
	uint8_t sprites_enabled;
	uint8_t sprites_scaled;
	uint8_t fbc_wm_enabled;
};

struct ilk_wm_values {
	uint32_t wm_pipe[HSW_PIPES];
	uint32_t wm_lp[3];
	uint32_t wm_lp_spr[3];
	uint8_t enable_fbc_wm;
	uint8_t partitioning;
};

struct ilk_wm_maximums {
	uint32_t pri, spr, cur, fbc;
};

struct wm_config {
	int num_pipes_active;
	int sprites_enabled;
	int sprites_scaled;
};

static struct {
	uint16_t pri_latency[ILK_MAX_LEVELS];
	uint16_t spr_latency[ILK_MAX_LEVELS];
	uint16_t cur_latency[ILK_MAX_LEVELS];
	int num_levels;
	int ver;
	int ready;
	struct ilk_pipe_wm active[HSW_PIPES]; /* what each pipe runs with */
	struct ilk_wm_values hw; /* as programmed */
} g_hsw;

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t div_round_up(uint64_t a, uint64_t b)
{
	return b ? (uint32_t)((a + b - 1) / b) : 0;
}

static int npipes(struct i915_device *i915)
{
	int n = i915->info->num_pipes;
	return n > HSW_PIPES ? HSW_PIPES : n;
}

/* ---- latencies ------------------------------------------------------------------------- */

/* SSKPD: level 0 in 0.1 us (the newer field, else the older one), the
 * others in 0.5 us. */
static void hsw_read_wm_latency(struct i915_device *i915, uint16_t *wm)
{
	uint32_t lo = i915_read32(i915, MCH_SSKPD);
	uint32_t hi = i915_read32(i915, MCH_SSKPD + 4);
	uint64_t sskpd = ((uint64_t)hi << 32) | lo;

	g_hsw.num_levels = 5;
	wm[0] = (uint16_t)((sskpd >> 56) & 0xff);
	if (wm[0] == 0)
		wm[0] = (uint16_t)(sskpd & 0xf);
	wm[1] = (uint16_t)((sskpd >> 4) & 0xff);
	wm[2] = (uint16_t)((sskpd >> 12) & 0xff);
	wm[3] = (uint16_t)((sskpd >> 20) & 0x1ff);
	wm[4] = (uint16_t)((sskpd >> 32) & 0x1ff);
}

/* ---- one level of one pipe ------------------------------------------------------------------- */

/* latency in 0.1 us */
static uint32_t ilk_wm_method1(uint32_t pixel_rate, uint32_t cpp, uint32_t latency)
{
	uint32_t ret = div_round_up((uint64_t)pixel_rate * cpp * latency, 10000);
	return div_round_up(ret, 64) + 2;
}

static uint32_t ilk_wm_method2(uint32_t pixel_rate, uint32_t htotal, uint32_t width, uint32_t cpp,
			       uint32_t latency)
{
	uint32_t ret;

	if (htotal == 0)
		htotal = 1;
	ret = (uint32_t)(((uint64_t)latency * pixel_rate) / ((uint64_t)htotal * 10000));
	ret = (ret + 1) * width * cpp;
	return div_round_up(ret, 64) + 2;
}

static uint32_t ilk_wm_fbc(uint32_t pri_val, uint32_t horiz_pixels, uint32_t cpp)
{
	if (!cpp || !horiz_pixels)
		return 0;
	return div_round_up((uint64_t)pri_val * 64, (uint64_t)horiz_pixels * cpp) + 2;
}

static uint32_t pipe_pixel_rate(const struct intel_wm_pipe_cfg *c)
{
	uint32_t dst_w, dst_h;

	if (!c->scaled)
		return c->clock_khz;
	dst_w = umin(c->src_w, c->hdisplay);
	dst_h = umin(c->src_h, c->vdisplay);
	if (!dst_w || !dst_h)
		return c->clock_khz;
	return div_round_up((uint64_t)c->clock_khz * c->src_w * c->src_h, (uint64_t)dst_w * dst_h);
}

static uint32_t ilk_compute_pri_wm(const struct intel_wm_pipe_cfg *c, uint32_t mem_value, int is_lp)
{
	uint32_t m1, m2, rate = pipe_pixel_rate(c);

	if (mem_value == 0)
		return 0xffffffffu;
	if (!c->plane_on)
		return 0;
	m1 = ilk_wm_method1(rate, c->cpp, mem_value);
	if (!is_lp)
		return m1;
	m2 = ilk_wm_method2(rate, c->htotal, c->plane_w, c->cpp, mem_value);
	return umin(m1, m2);
}

static uint32_t ilk_compute_cur_wm(const struct intel_wm_pipe_cfg *c, uint32_t mem_value)
{
	if (mem_value == 0)
		return 0xffffffffu;
	if (!c->cursor_w)
		return 0;
	return ilk_wm_method2(pipe_pixel_rate(c), c->htotal, c->cursor_w, 4, mem_value);
}

static uint32_t ilk_compute_fbc_wm(const struct intel_wm_pipe_cfg *c, uint32_t pri_val)
{
	if (!c->plane_on)
		return 0;
	return ilk_wm_fbc(pri_val, c->plane_w, c->cpp);
}

static void ilk_compute_wm_level(const struct intel_wm_pipe_cfg *c, int level,
				 struct ilk_wm_level *result)
{
	uint32_t pri_latency = g_hsw.pri_latency[level];
	uint32_t cur_latency = g_hsw.cur_latency[level];

	/* LP1+ latencies are in 0.5 us */
	if (level > 0) {
		pri_latency *= 5;
		cur_latency *= 5;
	}
	result->pri_val = ilk_compute_pri_wm(c, pri_latency, level);
	result->fbc_val = ilk_compute_fbc_wm(c, result->pri_val);
	/* no sprite plane is used */
	result->spr_val = 0;
	result->cur_val = ilk_compute_cur_wm(c, cur_latency);
	result->enable = 1;
}

/* ---- the limits ---------------------------------------------------------------------------- */

static uint32_t ilk_display_fifo_size(void)
{
	return g_hsw.ver >= 8 ? 3072 : 768;
}

static uint32_t ilk_plane_wm_reg_max(int level)
{
	if (g_hsw.ver >= 8)
		return level == 0 ? 255 : 2047;
	return level == 0 ? 127 : 1023;
}

static uint32_t ilk_cursor_wm_reg_max(int level)
{
	return level == 0 ? 63 : 255;
}

static uint32_t ilk_fbc_wm_reg_max(void)
{
	return g_hsw.ver >= 8 ? 31 : 15;
}

static uint32_t ilk_plane_wm_max(int level, const struct wm_config *config,
				 enum ddb_partitioning part, int is_sprite)
{
	uint32_t fifo_size = ilk_display_fifo_size();

	if (is_sprite && !config->sprites_enabled)
		return 0;
	/* Haswell has LP1+ with several pipes */
	if (level == 0 || config->num_pipes_active > 1)
		fifo_size /= HSW_PIPES;
	if (config->sprites_enabled) {
		/* level 0 is always 1:1 */
		if (level > 0 && part == DDB_PART_5_6) {
			if (is_sprite)
				fifo_size *= 5;
			fifo_size /= 6;
		} else {
			fifo_size /= 2;
		}
	}
	return umin(fifo_size, ilk_plane_wm_reg_max(level));
}

static uint32_t ilk_cursor_wm_max(int level, const struct wm_config *config)
{
	if (level > 0 && config->num_pipes_active > 1)
		return 64;
	return ilk_cursor_wm_reg_max(level);
}

static void ilk_compute_wm_maximums(int level, const struct wm_config *config,
				    enum ddb_partitioning part, struct ilk_wm_maximums *max)
{
	max->pri = ilk_plane_wm_max(level, config, part, 0);
	max->spr = ilk_plane_wm_max(level, config, part, 1);
	max->cur = ilk_cursor_wm_max(level, config);
	max->fbc = ilk_fbc_wm_reg_max();
}

static void ilk_compute_wm_reg_maximums(int level, struct ilk_wm_maximums *max)
{
	max->pri = ilk_plane_wm_reg_max(level);
	max->spr = ilk_plane_wm_reg_max(level);
	max->cur = ilk_cursor_wm_reg_max(level);
	max->fbc = ilk_fbc_wm_reg_max();
}

static int ilk_validate_wm_level(int level, const struct ilk_wm_maximums *max,
				 struct ilk_wm_level *result)
{
	int ret;

	if (!result->enable)
		return 0;
	result->enable = result->pri_val <= max->pri && result->spr_val <= max->spr &&
			 result->cur_val <= max->cur;
	ret = result->enable;
	/* level 0 cannot fail gracefully here: clamp it */
	if (level == 0 && !result->enable) {
		result->pri_val = umin(result->pri_val, max->pri);
		result->spr_val = umin(result->spr_val, max->spr);
		result->cur_val = umin(result->cur_val, max->cur);
		result->enable = 1;
	}
	return ret;
}

/* The levels of one pipe; 0 or -EINVAL when level 0 does not fit. */
static int ilk_compute_pipe_wm(const struct intel_wm_pipe_cfg *c, struct ilk_pipe_wm *pipe_wm)
{
	struct ilk_wm_maximums max;
	struct wm_config config = { 1, 0, 0 };
	int usable_level = g_hsw.num_levels - 1, ret = 0;

	mm_memset(pipe_wm, 0, sizeof(*pipe_wm));
	pipe_wm->pipe_enabled = c->active;
	pipe_wm->sprites_enabled = 0;
	pipe_wm->sprites_scaled = 0;
	ilk_compute_wm_level(c, 0, &pipe_wm->wm[0]);
	/* level 0 maximums depend on this pipe alone, with a 1:1 split */
	ilk_compute_wm_maximums(0, &config, DDB_PART_1_2, &max);
	if (!ilk_validate_wm_level(0, &max, &pipe_wm->wm[0]))
		ret = -EINVAL;
	ilk_compute_wm_reg_maximums(1, &max);
	for (int level = 1; level <= usable_level; level++) {
		struct ilk_wm_level *wm = &pipe_wm->wm[level];
		ilk_compute_wm_level(c, level, wm);
		/* a level beyond the register fields is never valid */
		if (!ilk_validate_wm_level(level, &max, wm)) {
			mm_memset(wm, 0, sizeof(*wm));
			break;
		}
	}
	return ret;
}

/* ---- merging the pipes ------------------------------------------------------------------- */

static void ilk_merge_wm_level(int n, const struct ilk_pipe_wm *active, int level,
			       struct ilk_wm_level *ret_wm)
{
	ret_wm->enable = 1;
	for (int pipe = 0; pipe < n; pipe++) {
		const struct ilk_wm_level *wm = &active[pipe].wm[level];
		if (!active[pipe].pipe_enabled)
			continue;
		/* values in use may stay in the registers a while after their
		 * level was disabled: keep them */
		if (!wm->enable)
			ret_wm->enable = 0;
		ret_wm->pri_val = umax(ret_wm->pri_val, wm->pri_val);
		ret_wm->spr_val = umax(ret_wm->spr_val, wm->spr_val);
		ret_wm->cur_val = umax(ret_wm->cur_val, wm->cur_val);
		ret_wm->fbc_val = umax(ret_wm->fbc_val, wm->fbc_val);
	}
}

static void ilk_wm_merge(int n, const struct ilk_pipe_wm *active, const struct ilk_wm_maximums *max,
			 struct ilk_pipe_wm *merged)
{
	int last_enabled_level = g_hsw.num_levels - 1;

	mm_memset(merged, 0, sizeof(*merged));
	merged->fbc_wm_enabled = 1;
	for (int level = 1; level < g_hsw.num_levels; level++) {
		struct ilk_wm_level *wm = &merged->wm[level];
		ilk_merge_wm_level(n, active, level, wm);
		if (level > last_enabled_level)
			wm->enable = 0;
		else if (!ilk_validate_wm_level(level, max, wm))
			last_enabled_level = level - 1;
		/* rather the FBC watermark off than a level */
		if (wm->fbc_val > max->fbc) {
			if (wm->enable)
				merged->fbc_wm_enabled = 0;
			wm->fbc_val = 0;
		}
	}
}

/* LP1, LP2, LP3 are levels 1, 2, 3 or 1, 3, 4 */
static int ilk_wm_lp_to_level(int wm_lp, const struct ilk_pipe_wm *pipe_wm)
{
	return wm_lp + (wm_lp >= 2 && pipe_wm->wm[4].enable);
}

static void ilk_compute_wm_results(int n, const struct ilk_pipe_wm *active,
				   const struct ilk_pipe_wm *merged, enum ddb_partitioning part,
				   struct ilk_wm_values *results)
{
	mm_memset(results, 0, sizeof(*results));
	results->enable_fbc_wm = merged->fbc_wm_enabled;
	results->partitioning = (uint8_t)part;
	for (int wm_lp = 1; wm_lp <= 3; wm_lp++) {
		int level = ilk_wm_lp_to_level(wm_lp, merged);
		const struct ilk_wm_level *r = &merged->wm[level];
		/* the values stay even with the level off */
		results->wm_lp[wm_lp - 1] = WM_LP_LATENCY(2 * level) | WM_LP_PRIMARY(r->pri_val) |
					    WM_LP_CURSOR(r->cur_val);
		if (r->enable)
			results->wm_lp[wm_lp - 1] |= HSW_WM_LP_ENABLE;
		if (g_hsw.ver >= 8)
			results->wm_lp[wm_lp - 1] |= WM_LP_FBC_BDW(r->fbc_val);
		else
			results->wm_lp[wm_lp - 1] |= WM_LP_FBC_ILK(r->fbc_val);
		results->wm_lp_spr[wm_lp - 1] = WM_LP_SPRITE(r->spr_val);
	}
	for (int pipe = 0; pipe < n; pipe++) {
		const struct ilk_wm_level *r = &active[pipe].wm[0];
		if (!r->enable)
			continue;
		results->wm_pipe[pipe] = WM0_PIPE_PRIMARY(r->pri_val) | WM0_PIPE_SPRITE(r->spr_val) |
					 WM0_PIPE_CURSOR(r->cur_val);
	}
}

/* ---- writing ------------------------------------------------------------------------------- */

#define WM_DIRTY_PIPE(pipe) (1u << (pipe))
#define WM_DIRTY_LP(wm_lp) (1u << (15 + (wm_lp)))
#define WM_DIRTY_LP_ALL (WM_DIRTY_LP(1) | WM_DIRTY_LP(2) | WM_DIRTY_LP(3))
#define WM_DIRTY_FBC (1u << 24)
#define WM_DIRTY_DDB (1u << 25)

static uint32_t ilk_compute_wm_dirty(int n, const struct ilk_wm_values *old,
				     const struct ilk_wm_values *new)
{
	uint32_t dirty = 0;
	int wm_lp;

	for (int pipe = 0; pipe < n; pipe++)
		if (old->wm_pipe[pipe] != new->wm_pipe[pipe])
			dirty |= WM_DIRTY_PIPE(pipe) | WM_DIRTY_LP_ALL;
	if (old->enable_fbc_wm != new->enable_fbc_wm)
		dirty |= WM_DIRTY_FBC | WM_DIRTY_LP_ALL;
	if (old->partitioning != new->partitioning)
		dirty |= WM_DIRTY_DDB | WM_DIRTY_LP_ALL;
	if (dirty & WM_DIRTY_LP_ALL)
		return dirty;
	/* the lowest LP level that changes, and all above it */
	for (wm_lp = 1; wm_lp <= 3; wm_lp++)
		if (old->wm_lp[wm_lp - 1] != new->wm_lp[wm_lp - 1] ||
		    old->wm_lp_spr[wm_lp - 1] != new->wm_lp_spr[wm_lp - 1])
			break;
	for (; wm_lp <= 3; wm_lp++)
		dirty |= WM_DIRTY_LP(wm_lp);
	return dirty;
}

static void ilk_disable_lp_wm(struct i915_device *i915, uint32_t dirty)
{
	struct ilk_wm_values *prev = &g_hsw.hw;
	static const uint32_t regs[3] = { HSW_WM1_LP, HSW_WM2_LP, HSW_WM3_LP };

	/* (the sprite enable bits stay: clearing them causes underruns) */
	for (int wm_lp = 3; wm_lp >= 1; wm_lp--) {
		if ((dirty & WM_DIRTY_LP(wm_lp)) && (prev->wm_lp[wm_lp - 1] & HSW_WM_LP_ENABLE)) {
			prev->wm_lp[wm_lp - 1] &= ~HSW_WM_LP_ENABLE;
			i915_write32(i915, regs[wm_lp - 1], prev->wm_lp[wm_lp - 1]);
		}
	}
}

/* Only what changed: every write makes the hardware re-evaluate. */
static void ilk_write_wm_values(struct i915_device *i915, int n, const struct ilk_wm_values *results)
{
	struct ilk_wm_values *prev = &g_hsw.hw;
	uint32_t dirty = ilk_compute_wm_dirty(n, prev, results);

	if (!dirty)
		return;
	ilk_disable_lp_wm(i915, dirty);
	for (int pipe = 0; pipe < n; pipe++)
		if (dirty & WM_DIRTY_PIPE(pipe))
			i915_write32(i915, HSW_WM0_PIPE(pipe), results->wm_pipe[pipe]);
	if (dirty & WM_DIRTY_DDB) {
		uint32_t v = i915_read32(i915, WM_MISC) & ~WM_MISC_DATA_PARTITION_5_6;
		if (results->partitioning != DDB_PART_1_2)
			v |= WM_MISC_DATA_PARTITION_5_6;
		i915_write32(i915, WM_MISC, v);
	}
	if (dirty & WM_DIRTY_FBC) {
		uint32_t v = i915_read32(i915, DISP_ARB_CTL) & ~DISP_FBC_WM_DIS;
		if (!results->enable_fbc_wm)
			v |= DISP_FBC_WM_DIS;
		i915_write32(i915, DISP_ARB_CTL, v);
	}
	if ((dirty & WM_DIRTY_LP(1)) && prev->wm_lp_spr[0] != results->wm_lp_spr[0])
		i915_write32(i915, WM1S_LP_ILK, results->wm_lp_spr[0]);
	if ((dirty & WM_DIRTY_LP(2)) && prev->wm_lp_spr[1] != results->wm_lp_spr[1])
		i915_write32(i915, WM2S_LP_IVB, results->wm_lp_spr[1]);
	if ((dirty & WM_DIRTY_LP(3)) && prev->wm_lp_spr[2] != results->wm_lp_spr[2])
		i915_write32(i915, WM3S_LP_IVB, results->wm_lp_spr[2]);
	if ((dirty & WM_DIRTY_LP(1)) && prev->wm_lp[0] != results->wm_lp[0])
		i915_write32(i915, HSW_WM1_LP, results->wm_lp[0]);
	if ((dirty & WM_DIRTY_LP(2)) && prev->wm_lp[1] != results->wm_lp[1])
		i915_write32(i915, HSW_WM2_LP, results->wm_lp[1]);
	if ((dirty & WM_DIRTY_LP(3)) && prev->wm_lp[2] != results->wm_lp[2])
		i915_write32(i915, HSW_WM3_LP, results->wm_lp[2]);
	*prev = *results;
}

/* Merge the pipes' levels and write them: the 1/2 split only, as the
 * 5/6 one is for a single pipe with its sprite on. */
static void ilk_program_watermarks(struct i915_device *i915, const struct ilk_pipe_wm *active)
{
	struct ilk_pipe_wm merged;
	struct ilk_wm_maximums max;
	struct ilk_wm_values results;
	struct wm_config config = { 0, 0, 0 };
	int n = npipes(i915);

	for (int pipe = 0; pipe < n; pipe++) {
		if (!active[pipe].pipe_enabled)
			continue;
		config.sprites_enabled |= active[pipe].sprites_enabled;
		config.sprites_scaled |= active[pipe].sprites_scaled;
		config.num_pipes_active++;
	}
	ilk_compute_wm_maximums(1, &config, DDB_PART_1_2, &max);
	ilk_wm_merge(n, active, &max, &merged);
	ilk_compute_wm_results(n, active, &merged, DDB_PART_1_2, &results);
	ilk_write_wm_values(i915, n, &results);
}

/* The larger of two pipe watermark sets: safe for both the frame on
 * screen and the next. */
static void ilk_intermediate(const struct ilk_pipe_wm *a, const struct ilk_pipe_wm *b,
			     struct ilk_pipe_wm *out)
{
	*out = *b;
	if (!a->pipe_enabled)
		return;
	out->pipe_enabled = a->pipe_enabled || b->pipe_enabled;
	for (int level = 0; level < ILK_MAX_LEVELS; level++) {
		struct ilk_wm_level *o = &out->wm[level];
		const struct ilk_wm_level *x = &a->wm[level];
		o->enable = o->enable && x->enable;
		o->pri_val = umax(o->pri_val, x->pri_val);
		o->spr_val = umax(o->spr_val, x->spr_val);
		o->cur_val = umax(o->cur_val, x->cur_val);
		o->fbc_val = umax(o->fbc_val, x->fbc_val);
	}
	if (!out->wm[0].enable)
		out->wm[0].enable = 1;
}

static int pipe_wm_equal(const struct ilk_pipe_wm *a, const struct ilk_pipe_wm *b)
{
	if (a->pipe_enabled != b->pipe_enabled)
		return 0;
	for (int level = 0; level < ILK_MAX_LEVELS; level++) {
		const struct ilk_wm_level *x = &a->wm[level], *y = &b->wm[level];
		if (x->enable != y->enable || x->pri_val != y->pri_val || x->spr_val != y->spr_val ||
		    x->cur_val != y->cur_val || x->fbc_val != y->fbc_val)
			return 0;
	}
	return 1;
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

/* ---- the entry points ---------------------------------------------------------------------------- */

static int has_ips(struct i915_device *i915)
{
	if (i915->info->platform == I915_PLATFORM_BROADWELL)
		return 1;
	return i915->info->platform == I915_PLATFORM_HASWELL && (i915->devid & 0xff00) == 0x0a00;
}

int hsw_wm_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	int n = npipes(i915);

	g_hsw.ver = i915->info->platform == I915_PLATFORM_BROADWELL ? 8 : 7;
	hsw_read_wm_latency(i915, g_hsw.pri_latency);
	for (int i = 0; i < ILK_MAX_LEVELS; i++) {
		g_hsw.spr_latency[i] = g_hsw.pri_latency[i];
		g_hsw.cur_latency[i] = g_hsw.pri_latency[i];
	}
	/* what is programmed now, then the low-power levels off until the
	 * first configuration is known */
	mm_memset(&g_hsw.hw, 0, sizeof(g_hsw.hw));
	for (int pipe = 0; pipe < n; pipe++)
		g_hsw.hw.wm_pipe[pipe] = i915_read32(i915, HSW_WM0_PIPE(pipe));
	g_hsw.hw.wm_lp[0] = i915_read32(i915, HSW_WM1_LP);
	g_hsw.hw.wm_lp[1] = i915_read32(i915, HSW_WM2_LP);
	g_hsw.hw.wm_lp[2] = i915_read32(i915, HSW_WM3_LP);
	g_hsw.hw.wm_lp_spr[0] = i915_read32(i915, WM1S_LP_ILK);
	g_hsw.hw.wm_lp_spr[1] = i915_read32(i915, WM2S_LP_IVB);
	g_hsw.hw.wm_lp_spr[2] = i915_read32(i915, WM3S_LP_IVB);
	g_hsw.hw.partitioning = (i915_read32(i915, WM_MISC) & WM_MISC_DATA_PARTITION_5_6) ?
					DDB_PART_5_6 : DDB_PART_1_2;
	g_hsw.hw.enable_fbc_wm = !(i915_read32(i915, DISP_ARB_CTL) & DISP_FBC_WM_DIS);
	ilk_disable_lp_wm(i915, WM_DIRTY_LP_ALL);
	mm_memset(g_hsw.active, 0, sizeof(g_hsw.active));

	for (int i = 0; i < 8; i++)
		d->mem_latency[i] = i < ILK_MAX_LEVELS ? g_hsw.pri_latency[i] : 0;
	d->nlatency = ILK_MAX_LEVELS;
	kprintf("[drm] i915: watermark latencies %u (0.1 us), %u/%u/%u/%u (0.5 us)\n",
		g_hsw.pri_latency[0], g_hsw.pri_latency[1], g_hsw.pri_latency[2],
		g_hsw.pri_latency[3], g_hsw.pri_latency[4]);
	g_hsw.ready = 1;
	return 0;
}

int hsw_wm_check(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg)
{
	struct ilk_pipe_wm wm;

	if (!g_hsw.ready)
		return 0;
	for (int pipe = 0; pipe < npipes(i915); pipe++)
		if (cfg[pipe].active && ilk_compute_pipe_wm(&cfg[pipe], &wm))
			return -EINVAL;
	return 0;
}

void hsw_wm_update(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg)
{
	struct ilk_pipe_wm optimal[HSW_PIPES], inter[HSW_PIPES];
	uint8_t changed = 0;
	int n = npipes(i915);

	if (!g_hsw.ready)
		return;
	mm_memset(optimal, 0, sizeof(optimal));
	for (int pipe = 0; pipe < n; pipe++) {
		if (cfg[pipe].active) {
			if (ilk_compute_pipe_wm(&cfg[pipe], &optimal[pipe]))
				kprintf("[drm] i915: pipe %c: level 0 watermark clamped to the FIFO\n",
					'A' + pipe);
		} else {
			/* an inactive pipe: level 0 of nothing */
			optimal[pipe].wm[0].enable = 1;
		}
		ilk_intermediate(&g_hsw.active[pipe], &optimal[pipe], &inter[pipe]);
		if (!pipe_wm_equal(&inter[pipe], &optimal[pipe]))
			changed |= (uint8_t)(1u << pipe);
	}
	/* the larger values first, the final ones a frame later */
	ilk_program_watermarks(i915, inter);
	if (changed) {
		for (int pipe = 0; pipe < n; pipe++)
			if ((changed & (1u << pipe)) && cfg[pipe].active)
				wait_for_vblank(i915, pipe);
		ilk_program_watermarks(i915, optimal);
	}
	for (int pipe = 0; pipe < n; pipe++)
		g_hsw.active[pipe] = optimal[pipe];

	/* line time, and on pipe A the IPS line time from CDCLK */
	for (int pipe = 0; pipe < n; pipe++) {
		uint32_t lt, ips = 0;
		if (!cfg[pipe].active)
			continue;
		lt = intel_wm_linetime(i915, &cfg[pipe]);
		if (pipe == PIPE_A && has_ips(i915) && i915->display.cdclk_khz)
			ips = umin((uint32_t)(((uint64_t)cfg[pipe].htotal * 1000 * 8 +
					       i915->display.cdclk_khz / 2) /
					      i915->display.cdclk_khz),
				   0x1ff);
		i915_write32(i915, WM_LINETIME(pipe), HSW_LINETIME(lt) | HSW_IPS_LINETIME(ips));
	}
	i915_dbg("[drm] i915: watermarks: WM0 %08x/%08x/%08x, LP %08x/%08x/%08x\n",
		 g_hsw.hw.wm_pipe[0], g_hsw.hw.wm_pipe[1], g_hsw.hw.wm_pipe[2], g_hsw.hw.wm_lp[0],
		 g_hsw.hw.wm_lp[1], g_hsw.hw.wm_lp[2]);
}

void hsw_wm_pipe_disable(struct i915_device *i915, int pipe)
{
	struct ilk_pipe_wm wms[HSW_PIPES];
	int n = npipes(i915);

	if (!g_hsw.ready || pipe < 0 || pipe >= n)
		return;
	/* the pipe's level 0 to nothing, the others as they are */
	for (int p = 0; p < n; p++)
		wms[p] = g_hsw.active[p];
	mm_memset(&wms[pipe], 0, sizeof(wms[pipe]));
	wms[pipe].wm[0].enable = 1;
	ilk_program_watermarks(i915, wms);
	g_hsw.active[pipe] = wms[pipe];
}
