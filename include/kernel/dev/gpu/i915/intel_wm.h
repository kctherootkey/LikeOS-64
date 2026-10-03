// LikeOS -- display watermarks, the data buffer, CDCLK and memory
// bandwidth.
//
// Three things decide whether a set of pipes can scan out without the
// display engine running dry: each plane's share of the display's data
// buffer (DBUF/DDB) and the watermark levels that say when it must be
// refilled (intel_skl_wm.c, intel_hsw_wm.c); the display core clock,
// which has to be fast enough for the pixel rate and the data the
// buffer moves (intel_cdclk_set.c); and the memory bandwidth, which the
// memory controller may lower by changing its frequency point (SAGV /
// QGV) unless the display holds it (intel_bw.c).  All three are computed
// from a description of every pipe -- struct intel_wm_pipe_cfg, filled
// in from display.pipes[] for what is running now, or by a caller for
// what is about to run -- and programmed in the order the hardware wants
// around a pipe going on or off.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_WM_H
#define KERNEL_DEV_GPU_I915_INTEL_WM_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/i915/intel_display.h>

struct i915_device;

/* ---- what a pipe runs ------------------------------------------------------------- */

/* One pipe as the watermark, CDCLK and bandwidth code sees it.  The
 * timing is the transcoder's (the adjusted mode); src_w x src_h is the
 * pipe's picture (PIPESRC), which the pipe scaler stretches onto the
 * timing when `scaled' is set.  The primary plane is plane_w x plane_h
 * pixels of `cpp' bytes in the layout `modifier' (DRM_FORMAT_MOD_LINEAR,
 * I915_FORMAT_MOD_X_TILED, _Y_TILED or _4_TILED); the cursor is a square
 * ARGB8888 of cursor_w pixels, 0 when it is off. */
struct intel_wm_pipe_cfg {
	uint8_t active;
	uint8_t scaled;
	uint8_t is_edp; /* the output is the panel (Skylake: DPLL0's VCO) */
	uint8_t is_dp; /* DisplayPort (eDP included) */
	uint8_t has_audio; /* never set by this driver yet */
	uint8_t lanes; /* DisplayPort lanes */
	uint16_t hdisplay, htotal, vdisplay, vtotal;
	uint16_t src_w, src_h;
	uint32_t clock_khz; /* the dot clock */
	uint32_t port_clock_khz; /* DP link rate, or the TMDS clock */
	uint8_t plane_on;
	uint8_t cpp;
	uint16_t plane_w, plane_h;
	uint16_t plane_src_x; /* where in the surface the plane starts (panning) */
	uint64_t modifier;
	uint16_t cursor_w;
	/* The CDCLK (kHz) the stream compression engines of the pipe need;
	 * 0 without compression. */
	uint32_t dsc_min_cdclk;
};

/* What is running now, from display.pipes[] and their outputs.  A pipe
 * counts as active once its p->active is set and it has a mode; the
 * primary plane is on when p->surf_ggtt is non-zero (p->width, height,
 * format, modifier describe it); the cursor is on when p->cursor_w is
 * non-zero. */
void intel_wm_cfg_from_hw(struct i915_device *i915,
			  struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES]);
/* And the compression engines' CDCLK of the running pipes into such a
 * configuration (intel_display.c). */
void intel_wm_cfg_dsc_from_hw(struct i915_device *i915,
			      struct intel_wm_pipe_cfg cfg[INTEL_MAX_PIPES]);
/* One pipe's timing and output, from the mode about to be set: `m' is
 * the transcoder's timing, src_w x src_h the client's picture,
 * `port_clock_khz' the link rate (DP) or TMDS clock (HDMI) as far as it
 * is known (an upper bound is fine: only the voltage level uses it). */
void intel_wm_cfg_set_mode(struct intel_wm_pipe_cfg *c, const struct drm_mode_modeinfo *m,
			   uint32_t src_w, uint32_t src_h, uint32_t port_clock_khz,
			   int is_dp, int is_edp, int lanes);
/* The primary plane and the cursor of a pipe about to be shown. */
void intel_wm_cfg_set_planes(struct intel_wm_pipe_cfg *c, int plane_on, uint32_t w,
			     uint32_t h, uint32_t drm_format, uint64_t modifier,
			     uint32_t cursor_w);

/* ---- the platform's display parameters (intel_skl_wm.c) --------------------------- */

#define INTEL_WM_HAS_IPC (1u << 0) /* isochronous priority control */
#define INTEL_WM_HAS_SAGV (1u << 1) /* the memory changes frequency on its own */
#define INTEL_WM_CDCLK_CRAWL (1u << 2) /* CDCLK PLL ratio changes without a stop */
#define INTEL_WM_CDCLK_SQUASH (1u << 3) /* CDCLK cycle squasher */
#define INTEL_WM_DGFX (1u << 4)
#define INTEL_WM_2PPC (1u << 5) /* two pixels per CDCLK cycle */

struct intel_wm_platform {
	uint16_t ip; /* display IP as I915_IP(ver, rel): 800, 900, 1000 .. 3000 */
	uint16_t dbuf_size; /* data buffer blocks the driver hands out */
	uint8_t dbuf_slice_mask;
	uint8_t abox_mask; /* the ABOXes pixel data is read through */
	uint8_t num_planes; /* universal planes per pipe (without the cursor) */
	uint8_t flags; /* INTEL_WM_* */
};

/* NULL for a part this code does not know. */
const struct intel_wm_platform *intel_wm_platform_get(struct i915_device *i915);
/* The display version (8, 9, 10, 11, 12, 13, 14, 20, 30), from the table. */
int intel_wm_display_ver(struct i915_device *i915);

/* ---- watermarks, DDB, DBUF slices and MBUS (intel_skl_wm.c, intel_hsw_wm.c) -------- */

/* What the other two parts need from a computed configuration. */
struct intel_wm_result {
	uint8_t active_pipes;
	uint8_t joined_mbus;
	uint8_t enabled_slices;
	uint8_t sagv_reject; /* pipes whose watermarks rule SAGV out */
	uint8_t num_active_planes[INTEL_MAX_PIPES]; /* the cursor not counted */
	uint32_t data_rate[INTEL_MAX_PIPES]; /* kB/s the planes read (cursor not counted) */
	uint32_t dbuf_bw_min_cdclk; /* kHz the DBUF bandwidth needs */
	uint8_t level[INTEL_MAX_PIPES]; /* the highest level each pipe reaches */
};

/* Once at display init, after intel_cdclk_init() and intel_power_init():
 * the memory latencies, the DRAM and its bandwidth, SAGV and IPC, and
 * the DBUF/MBUS state the firmware left. */
int intel_wm_init(struct i915_device *i915);
/* Display core bring-up (intel_power_init()): the DBUF slice every
 * configuration needs, the trackers' service setting and the ABOX
 * credits.  Replaces the bring-up's own DBUF and MBUS steps. */
void intel_dbuf_hw_init(struct i915_device *i915);
/* Can `cfg' (every pipe) be scanned out: the DDB holds level 0 of every
 * plane, a memory point carries the data, the CDCLK the pipes need is
 * one the platform has.  0 or -EINVAL.  `res' may be NULL. */
int intel_wm_check(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		   struct intel_wm_result *res);
/* The pure computation behind it (no register is touched). */
int intel_wm_compute(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		     struct intel_wm_result *res);
/* Make the hardware match display.pipes[]: DBUF slices, MBUS joining and
 * credits, every plane's DDB share and watermark levels, line times,
 * the memory frequency points the display allows.  Pipes that stay
 * active are reallocated in an order that never lets two pipes' shares
 * overlap (waiting a frame between where needed); the planes of a pipe
 * that has just been enabled get theirs last, so it must be called
 * before its primary plane is enabled.  Writes to an enabled plane are
 * armed by rewriting its surface address. */
void intel_wm_update(struct i915_device *i915);
/* A pipe is going off: its planes' watermarks and DDB shares to zero
 * (call after its planes and cursor were disabled, while its power well
 * is still up, before the transcoder stops).  The share is given to the
 * other pipes by the next intel_wm_update(). */
void intel_wm_pipe_disable(struct i915_device *i915, int pipe);
/* The ratio of the memory clock to CDCLK changed (intel_cdclk_set.c):
 * the DBUF minimum tracker setting and, from display version 20, the
 * MBUS translation throttle. */
void intel_dbuf_mdclk_ratio_update(struct i915_device *i915, int ratio, int joined_mbus);
int intel_wm_mbus_joined(struct i915_device *i915);
/* The linetime watermark value (1/8 us) of a pipe, for intel_hsw_wm.c. */
uint32_t intel_wm_linetime(struct i915_device *i915, const struct intel_wm_pipe_cfg *c);

/* Haswell and Broadwell (intel_hsw_wm.c), called by the above. */
int hsw_wm_init(struct i915_device *i915);
int hsw_wm_check(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg);
void hsw_wm_update(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg);
void hsw_wm_pipe_disable(struct i915_device *i915, int pipe);

/* ---- CDCLK (intel_cdclk_set.c) ------------------------------------------------------ */

/* After intel_cdclk_init(): the whole configuration as running (VCO,
 * reference, bypass, voltage), the platform's maximum, and a firmware
 * setting that is not one of the platform's corrected. */
int intel_cdclk_set_init(struct i915_device *i915);
/* The lowest CDCLK (kHz) `cfg' needs: the pixel rate per pipe (two
 * pixels a clock from display version 10), the planes' rates, the memory
 * read bandwidth of a pipe (display version 12+), the DBUF bandwidth,
 * the audio rules, the Gemini Lake audio workaround; and in
 * *min_voltage_level (may be NULL) the voltage level the ports need.
 * NULL `cfg': what is running. */
uint32_t intel_cdclk_required_khz(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
				  uint8_t *min_voltage_level);
/* Run CDCLK at the platform's lowest frequency that is at least `khz'
 * (and at the voltage level that needs, or `min_voltage_level' if that
 * is higher), with the platform's full sequence.  Updates
 * display.cdclk_khz, cdclk_vco_khz, cdclk_ref_khz, cdclk_voltage_level.
 * Returns -EBUSY, changing nothing, when this platform can only make
 * the change with every pipe off and some pipe is on (see
 * intel_cdclk_change_needs_pipes_off()); -EINVAL above the maximum. */
int intel_cdclk_set(struct i915_device *i915, uint32_t khz);
int intel_cdclk_set_level(struct i915_device *i915, uint32_t khz, uint8_t min_voltage_level);
/* Raise (before a pipe goes on) or lower (after one went off; only when
 * that does not need the running pipes stopped) CDCLK to what `cfg'
 * (NULL: what is running) needs.  On display version 14 the PM demand
 * request is raised before and lowered after, as the hardware wants. */
int intel_cdclk_update(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		       int allow_decrease);
/* Would going to `khz' stop the running pipes: 1 when this platform has
 * to turn every pipe off for it (Broadwell, Skylake..Comet Lake always;
 * Broxton..Tiger Lake/Alder Lake-S/Rocket Lake unless one pipe is on
 * and only the CD2X divider changes) and a pipe is on. */
int intel_cdclk_change_needs_pipes_off(struct i915_device *i915, uint32_t khz);
uint32_t intel_cdclk_max_khz(struct i915_device *i915);
/* The platform's frequency for a minimum: what intel_cdclk_set() would
 * run (without the hardware). */
uint32_t intel_cdclk_round_khz(struct i915_device *i915, uint32_t min_khz);
/* The ratio of the memory clock to CDCLK (2: the memory clock is CD2X). */
int intel_cdclk_mdclk_ratio(struct i915_device *i915);

/* The pcode mailbox with the errors decoded (-ENXIO, -ETIMEDOUT, -EINVAL,
 * -EBUSY, -EACCES ...), and the request that is repeated until the pcode
 * answers `reply' under `reply_mask' or `timeout_base_ms' + 50 ms pass. */
int intel_pcode_rw(struct i915_device *i915, uint32_t mbox, uint32_t *val, uint32_t *val1,
		   uint32_t fast_timeout_us, uint32_t slow_timeout_ms, int is_read);
int intel_pcode_request(struct i915_device *i915, uint32_t mbox, uint32_t request,
			uint32_t reply_mask, uint32_t reply, uint32_t timeout_base_ms);

/* ---- DRAM and memory bandwidth (intel_bw.c) ------------------------------------------ */

enum intel_dram_type {
	INTEL_DRAM_UNKNOWN = 0,
	INTEL_DRAM_DDR2,
	INTEL_DRAM_DDR3,
	INTEL_DRAM_DDR4,
	INTEL_DRAM_LPDDR3,
	INTEL_DRAM_LPDDR4,
	INTEL_DRAM_DDR5,
	INTEL_DRAM_LPDDR5,
	INTEL_DRAM_GDDR,
	INTEL_DRAM_GDDR_ECC,
};

struct intel_dram_info {
	uint8_t detected; /* the tables could be read */
	uint8_t type; /* enum intel_dram_type */
	uint8_t num_channels;
	uint8_t num_qgv_points;
	uint8_t num_psf_gv_points;
	uint8_t symmetric_memory;
	uint8_t has_16gb_dimms; /* assumed when unknown */
	uint8_t ecc_impacting_de_bw;
};

/* What the memory controller reports (or the pcode, display version 11
 * to 13; registers from 14). */
int intel_dram_detect(struct i915_device *i915);
const struct intel_dram_info *intel_dram_get(struct i915_device *i915);
/* The memory frequency points and what each carries for 1..N planes. */
void intel_bw_init_hw(struct i915_device *i915);
/* Display version 11-13: SAGV forced off until the first configuration
 * is known to allow it (the firmware's state cannot be read back). */
void intel_bw_init(struct i915_device *i915);
/* Do the memory points carry the planes of `res'; the mask of points
 * the pcode is to be told to avoid (display version 11-13), or the peak
 * bandwidth of the point to hold (14+, for PM demand), in *qgv. */
int intel_bw_check(struct i915_device *i915, const struct intel_wm_result *res,
		   uint16_t *qgv);
/* Around a configuration change: before it, restrict the memory points
 * (or turn SAGV off) to what both the old and the new configuration can
 * live with; after it, relax them to what the new one needs. */
void intel_bw_pre_update(struct i915_device *i915, const struct intel_wm_result *res);
void intel_bw_post_update(struct i915_device *i915, const struct intel_wm_result *res);
/* SAGV as the watermark code sees it. */
int intel_has_sagv(struct i915_device *i915);
uint32_t intel_sagv_block_time_us(struct i915_device *i915);
/* Display version 14: the peak bandwidth (100 MB/s units) of the memory
 * point PM demand should hold, 0xffff for "do not change frequency". */
uint16_t intel_bw_qgv_peakbw(struct i915_device *i915);

#endif
