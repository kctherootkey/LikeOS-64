// LikeOS -- the display of the Intel parts before DDI: shared state.
//
// What the files of the legacy display backend (intel_legacy_*.c) share:
// the device's display model (which generation, which south display, how
// many pipes), the outputs with their per-type hooks, the pipe
// configuration a mode set computes and then applies, and the entry
// points each file offers the others.  The DRM-facing API is
// intel_display_legacy.h; nothing outside the backend includes this.
//
// A mode set runs in two halves, as everywhere in this driver: the check
// builds a struct lg_config per pipe (the output's compute_config hook
// fills in what the output needs, the PLL and FDI code what the clocks
// need), and the commit walks the families' enable sequences with it --
// GMCH (gen2-4), Valleyview/Cherryview, or Ironlake to Ivy Bridge with
// its PCH -- calling the output hooks at the points the hardware wants.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_LEGACY_H
#define KERNEL_DEV_GPU_I915_INTEL_LEGACY_H

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/intel_legacy_regs.h>
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/i2c.h>
#include <kernel/hal/lapic.h>

#define LG_MAX_PIPES 3
#define LG_MAX_OUTPUTS DRM_MAX_CONNECTORS
#define LG_VBT_MAX_CHILDREN 16

enum lg_pch {
	LG_PCH_NONE = 0, /* GMCH, Valleyview, Cherryview */
	LG_PCH_IBX, /* Ibex Peak (Ironlake) */
	LG_PCH_CPT, /* Cougar Point / Panther Point (Sandy and Ivy Bridge) */
};

/* Ports.  Integrated DP/HDMI/SDVO use B..D (A is the CPU eDP port of
 * Ironlake to Ivy Bridge); the gen2 DVO ports are A..C. */
enum lg_port {
	LG_PORT_A = 0,
	LG_PORT_B,
	LG_PORT_C,
	LG_PORT_D,
};

enum lg_output_type {
	LG_OUTPUT_UNUSED = 0,
	LG_OUTPUT_ANALOG, /* the VGA DAC */
	LG_OUTPUT_LVDS,
	LG_OUTPUT_SDVO, /* an SDVO encoder chip (TMDS, VGA, LVDS or TV) */
	LG_OUTPUT_DVO, /* a DVO encoder chip (gen2) */
	LG_OUTPUT_TVOUT, /* the integrated TV encoder */
	LG_OUTPUT_HDMI, /* integrated TMDS: HDMI, or DVI when the sink is */
	LG_OUTPUT_DP,
	LG_OUTPUT_EDP,
	LG_OUTPUT_DSI,
	LG_OUTPUT_TYPES
};

/* Hotplug detect pins. */
enum lg_hpd_pin {
	LG_HPD_NONE = 0,
	LG_HPD_TV,
	LG_HPD_CRT,
	LG_HPD_SDVO_B,
	LG_HPD_SDVO_C,
	LG_HPD_PORT_A,
	LG_HPD_PORT_B,
	LG_HPD_PORT_C,
	LG_HPD_PORT_D,
	LG_HPD_NUM
};

/* GMBUS pins (the same numbering on GMCH and PCH parts). */
#define LG_GMBUS_PIN_DISABLED 0
#define LG_GMBUS_PIN_SSC 1
#define LG_GMBUS_PIN_VGADDC 2
#define LG_GMBUS_PIN_PANEL 3
#define LG_GMBUS_PIN_DPD_CHV 3 /* Cherryview: HDMI D's DDC on the panel pin */
#define LG_GMBUS_PIN_DPC 4
#define LG_GMBUS_PIN_DPB 5
#define LG_GMBUS_PIN_DPD 6
#define LG_GMBUS_NUM_PINS 7

/* ---- clocks and links ------------------------------------------------------------- */

/* One set of DPLL dividers and what they make.  On Cherryview m2 is a
 * 22.22 fixed-point value. */
struct lg_dpll {
	int n, m1, m2, p1, p2;
	int dot, vco, m, p;
};

/* Data and link M/N values (DisplayPort, FDI). */
struct lg_link_m_n {
	uint32_t tu;
	uint32_t data_m, data_n;
	uint32_t link_m, link_n;
};

/* The timing the pipe's generator runs, as the registers take it. */
struct lg_timings {
	uint32_t clock; /* kHz */
	uint16_t hdisplay, hblank_start, hblank_end, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vblank_start, vblank_end, vsync_start, vsync_end, vtotal;
	uint32_t flags; /* DRM_MODE_FLAG_* */
};

/* ---- the configuration of one pipe --------------------------------------------------- */

struct lg_config {
	int pipe;
	int output; /* index into d->out[] */
	uint32_t output_types; /* 1 << enum lg_output_type */
	/* What the output is sent (the client's mode, or the panel's own
	 * timing with the fitter scaling onto it) and the pipe's source
	 * size (what the plane shows). */
	struct drm_mode_modeinfo mode;
	uint32_t src_w, src_h;
	/* The generator's timing.  Filled from `mode' by the core after
	 * compute_config unless the output set t_set (panel borders,
	 * TV-out). */
	struct lg_timings t;
	int t_set;
	uint32_t pixel_rate; /* kHz */
	int pipe_bpp; /* 18, 24, 30 */
	int dither;
	int limited_color_range;
	int double_wide; /* gen2/3: the pipe runs two pixels a clock */
	int pixel_multiplier; /* SDVO: 1..4 */
	/* The PLL: the clock it makes (TMDS/analog: the pixel clock times
	 * the multiplier; DP: the link rate) and its dividers. */
	uint32_t port_clock;
	int clock_set; /* the output chose the dividers (TV, fixed DP rates) */
	struct lg_dpll dpll;
	uint32_t dpll_hw, dpll_md, fp0, fp1; /* the registers' values */
	int sdvo_tv_clock; /* SDVO TV: the PLL takes the TV clock in */
	/* DisplayPort */
	int has_dp_encoder;
	int lane_count;
	struct lg_link_m_n dp_m_n;
	int enhanced_framing;
	/* Ironlake to Ivy Bridge: a PCH port behind FDI */
	int has_pch_encoder;
	int pch_dpll; /* the PCH PLL (0 = A, 1 = B), -1 none */
	int fdi_lanes;
	struct lg_link_m_n fdi_m_n;
	/* panel fitters */
	struct {
		uint32_t control, pgm_ratios, lvds_border_bits;
	} gmch_pfit;
	struct {
		int enabled;
		uint32_t pos, size; /* PF_WIN_POS / PF_WIN_SZ values */
		int force_thru;
	} pch_pfit;
	/* HDMI */
	int has_hdmi_sink;
	int has_infoframe;
	int has_audio;
	int msa_timing_delay; /* eDP, from the VBT */
	uint32_t gamma_mode; /* PIPECONF gamma field, 8 bit */
};

static inline int lg_cfg_has(const struct lg_config *cfg, enum lg_output_type t)
{
	return !!(cfg->output_types & (1u << t));
}

/* ---- outputs ----------------------------------------------------------------------- */

struct lg_display;
struct lg_output;

/* What every output type implements.  NULL hooks are skipped (detect:
 * always connected; get_modes: the EDID if one was read, else the
 * standard table).  The commit calls them in the order the families'
 * enable and disable sequences want:
 *
 *   GMCH:      timings, pre_enable, PLL on, fitter, pipe on, plane, enable
 *   VLV/CHV:   timings, pre_pll_enable, PLL on, pre_enable, fitter, pipe
 *              on, plane, enable
 *   ILK-IVB:   timings, pre_enable, FDI PLL, fitter, pipe on, PCH (FDI
 *              training, PCH PLL, transcoder), plane, enable
 *
 *   disable:   disable, plane off, pipe off, fitter off, [PCH: FDI off],
 *              post_disable, [PCH transcoder/PLL off], PLL off,
 *              post_pll_disable
 */
struct lg_output_funcs {
	/* 1 when a sink is there.  May read and keep the EDID. */
	int (*detect)(struct lg_display *d, struct lg_output *o);
	/* Fill DRM connector `conn' with the modes (drm_connector_set_edid,
	 * drm_connector_add_mode, drm_connector_add_std_modes); return
	 * how many there are. */
	int (*get_modes)(struct lg_display *d, struct lg_output *o, int conn);
	/* 0 when the output can show `m' as the client gave it. */
	int (*mode_valid)(struct lg_display *d, struct lg_output *o,
			  const struct drm_mode_modeinfo *m);
	/* Complete cfg for this output.  On entry: cfg->pipe, cfg->mode
	 * and src_w/src_h (the client's mode), output_types, pipe_bpp 24,
	 * pixel_multiplier 1.  Return 0 or -EINVAL. */
	int (*compute_config)(struct lg_display *d, struct lg_output *o,
			      struct lg_config *cfg);
	void (*pre_pll_enable)(struct lg_display *d, struct lg_output *o,
			       const struct lg_config *cfg);
	void (*pre_enable)(struct lg_display *d, struct lg_output *o,
			   const struct lg_config *cfg);
	void (*enable)(struct lg_display *d, struct lg_output *o,
		       const struct lg_config *cfg);
	void (*disable)(struct lg_display *d, struct lg_output *o,
			const struct lg_config *cfg);
	void (*post_disable)(struct lg_display *d, struct lg_output *o,
			     const struct lg_config *cfg);
	void (*post_pll_disable)(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg);
	/* Is the port enabled (by the firmware, say), and on which pipe? */
	int (*get_hw_state)(struct lg_display *d, struct lg_output *o, int *pipe);
	/* A hotplug pulse on the output's pin (from the hotplug worker,
	 * process context): a DP sink asking for a link check, say. */
	void (*hpd_event)(struct lg_display *d, struct lg_output *o, int long_pulse);
	/* System sleep: anything the output keeps that the hardware
	 * forgets (the outputs are already off). */
	void (*suspend)(struct lg_display *d, struct lg_output *o);
	void (*resume)(struct lg_display *d, struct lg_output *o);
};

struct lg_output {
	enum lg_output_type type;
	int port; /* enum lg_port, or the DVO/SDVO port */
	uint32_t reg; /* the port's control register (display-relative) */
	char name[16]; /* for the log: "VGA", "LVDS", "HDMI-B", "eDP", ... */
	const struct lg_output_funcs *funcs;
	uint32_t conn_type; /* DRM_MODE_CONNECTOR_* */
	uint32_t enc_type; /* DRM_MODE_ENCODER_* */
	uint32_t pipe_mask; /* the pipes it can drive */
	int hpd_pin; /* enum lg_hpd_pin */
	int polled; /* no hotplug interrupt: probed when asked */
	/* where it is in the DRM objects and the hardware */
	int conn; /* DRM connector index, -1 */
	int crtc; /* DRM crtc index while active, -1 */
	int pipe; /* while active, -1 */
	int active;
	struct lg_config cfg; /* what it runs with while active */
	/* the sink */
	int detected;
	uint8_t edid[512];
	int edid_len;
	int hdmi_sink; /* the EDID carries the HDMI vendor block */
	int has_audio;
	/* DDC (GMBUS pin and adapter), when the output has one */
	uint8_t ddc_pin;
	struct i2c_adapter *ddc;
	/* A panel shows one timing; other modes are scaled onto it. */
	int is_panel;
	struct drm_mode_modeinfo fixed_mode;
	int fixed_mode_valid;
	uint32_t mm_width, mm_height;
	/* The other output on the same port (DP and HDMI sharing port B,
	 * C or D): only one of the two can be up.  -1 none. */
	int sibling;
	/* the output file's own state */
	void *priv;
};

/* ---- pipes ---------------------------------------------------------------------- */

struct lg_pipe {
	int pipe;
	int active;
	int output; /* index into d->out[], -1 */
	int crtc; /* DRM crtc index, -1 */
	struct lg_config cfg; /* what runs */
	/* The primary plane feeding the pipe.  The mobile gen2/3 parts with
	 * a framebuffer compressor cross them (plane A on pipe B). */
	int plane;
	int plane_enabled;
	uint32_t surf; /* GGTT address of the first pixel's page */
	uint32_t stride, width, height, format, cpp;
	uint64_t modifier;
	uint32_t x, y; /* where in the framebuffer the picture starts */
	/* gen2/3: the X-tiled surface the plane reads through a fence
	 * register pinned for it (referenced while pinned), or NULL */
	struct drm_gem_object *fence_obj;
	/* cursor */
	int cursor_enabled;
	uint32_t cursor_w, cursor_h;
	uint32_t cursor_base; /* GGTT address, or physical where it must be */
	int cursor_x, cursor_y;
	/* vblank and flips */
	int vblank_enabled;
	volatile int flip_pending;
	uint64_t vblanks;
	uint64_t underruns;
	int underrun_reported;
	uint32_t pipestat_enable; /* GMCH/VLV: the PIPESTAT enables in use */
};

/* ---- what the VBT says (intel_legacy_vbt.c) ------------------------------------- */

struct lg_vbt_child {
	uint16_t handle;
	uint16_t device_type; /* DEVICE_TYPE_* bits */
	uint8_t dvo_port; /* DVO_PORT_* code */
	uint8_t i2c_pin; /* the encoder's control bus (SDVO, DVO) */
	uint8_t slave_addr; /* its 8-bit I2C address */
	uint8_t ddc_pin; /* the sink's DDC */
	uint8_t dvo_cfg;
	uint8_t dvo_wiring;
	uint8_t aux_ch; /* 0 = A .. 3 = D, 0xff none */
	uint8_t hdmi_level_shift; /* 0xff none */
	uint8_t lane_reversal;
	uint8_t supports_dp, supports_hdmi, supports_dvi, supports_edp;
	uint8_t is_internal;
	int port; /* enum lg_port for integrated ports, -1 */
};

struct lg_vbt {
	int valid;
	uint16_t version; /* BDB version */
	/* general features */
	int int_tv_support;
	int int_crt_support;
	int lvds_use_ssc;
	uint32_t lvds_ssc_freq_khz;
	int display_clock_mode;
	int fdi_rx_polarity_inverted;
	/* driver features */
	int int_lvds_support;
	/* general definitions */
	uint8_t crt_ddc_pin;
	int nchild;
	struct lg_vbt_child child[LG_VBT_MAX_CHILDREN];
	/* SDVO encoders: [0] port B, [1] port C */
	struct {
		int initialized;
		uint8_t dvo_port, slave_addr, dvo_wiring, i2c_pin, ddc_pin;
	} sdvo[2];
	/* the internal panel */
	int panel_type; /* -1 unknown */
	int lvds_dither;
	uint32_t bios_lvds_val; /* the LVDS register as the VBIOS set it, 0 */
	struct drm_mode_modeinfo lfp_mode;
	int lfp_mode_valid;
	uint32_t lfp_width_mm, lfp_height_mm;
	int lfp_bpc; /* 0 unknown */
	/* the SDVO-attached panel */
	struct drm_mode_modeinfo sdvo_lvds_mode;
	int sdvo_lvds_mode_valid;
	/* eDP */
	int edp_valid;
	uint16_t edp_t1_t3, edp_t8, edp_t9, edp_t10, edp_t11_t12; /* 100 us */
	uint32_t edp_rate_khz; /* 0 = the sink's best */
	int edp_lanes; /* 0 = the sink's */
	int edp_preemphasis, edp_vswing;
	int edp_bpp; /* 0 unknown */
	int edp_low_vswing;
	uint8_t edp_msa_timing_delay;
	/* backlight */
	int bl_present;
	uint16_t bl_pwm_freq_hz;
	uint8_t bl_min_brightness; /* of 255 */
	int bl_active_low;
	int bl_type;
	/* The whole table, copied, for the parts read on demand (the MIPI
	 * blocks of a DSI panel). */
	uint8_t *raw;
	uint32_t raw_len;
};

/* ---- the display ---------------------------------------------------------------- */

struct lg_display {
	struct i915_device *i915;
	struct drm_device *drm;
	/* what the hardware is */
	int platform; /* enum i915_platform */
	int ver; /* display version: 2..7 (Cherryview 8) */
	int mobile;
	int is_i830, is_i845, is_i85x, is_i865; /* i845 covers i845G only */
	int is_i915g, is_i915gm, is_i945g, is_i945gm, is_g33, is_pnv;
	int is_i965g, is_i965gm, is_g4x, is_gm45;
	int is_ilk, is_snb, is_ivb, is_vlv, is_chv;
	int gmch; /* no PCH split: gen2-4, Valleyview, Cherryview */
	int pch; /* enum lg_pch */
	int num_pipes;
	uint32_t base; /* the display block: 0x180000 on VLV/CHV, else 0 */
	uint32_t gmbus_base; /* 0xc0000 with a PCH, else 0 */
	int ready;
	/* clocks, kHz */
	uint32_t cdclk_khz, max_cdclk_khz, max_dotclk_khz;
	uint32_t rawclk_khz; /* the display's raw (hraw on GMCH) clock */
	uint32_t hpll_vco_khz; /* VLV/CHV */
	uint32_t czclk_khz; /* VLV/CHV */
	uint32_t fsb_khz, mem_khz; /* Pineview self refresh, from the MCHBAR */
	int is_ddr3;
	/* the VBT and the OpRegion (the OpRegion lives in i915->display) */
	struct lg_vbt vbt;
	/* LVDS */
	int lvds_dual; /* dual-channel panel */
	int lvds_use_ssc;
	uint32_t lvds_ssc_khz;
	/* outputs and pipes */
	int nout;
	struct lg_output out[LG_MAX_OUTPUTS];
	struct lg_pipe pipes[LG_MAX_PIPES];
	/* Ironlake to Ivy Bridge: the two PCH PLLs, refcounted by pipe */
	struct {
		uint32_t pipe_mask; /* pipes using it */
		uint32_t active_mask; /* pipes it is enabled for */
		uint32_t dpll, fp0, fp1;
	} pch_dpll[2];
	/* hotplug: the interrupt notes the pins, the worker probes */
	uint32_t hotplug_en; /* PORT_HOTPLUG_EN bits in use */
	uint32_t sde_hpd_mask; /* SDE bits in use */
	uint32_t de_hpd_mask; /* DE port A bit */
	volatile uint32_t hpd_pending; /* 1 << enum lg_hpd_pin */
	volatile uint32_t hpd_long; /* the long pulses among them */
	uint64_t hpd_events;
	struct wait_queue_head hpd_wq;
	task_t *hpd_worker;
	volatile int hpd_ready;
	/* interrupt state */
	uint32_t de_imr; /* DEIMR as programmed (ILK-IVB) */
	int irq_enabled; /* the top level unmasked our bits */
	/* GMBUS: one transfer at a time */
	volatile int gmbus_busy;
	/* the firmware's plane, for the fallback */
	struct {
		int pipe, plane; /* -1 none */
		uint32_t dspcntr, dspaddr, dspsurf, dspstride, dsptileoff, dsplinoff;
	} boot;
};

/* ---- register access ---------------------------------------------------------------- */

/* Display registers: the offsets of intel_legacy_regs.h, with the display
 * block base added (Valleyview/Cherryview). */
static inline uint32_t lg_rd(struct lg_display *d, uint32_t reg)
{
	return i915_read32(d->i915, d->base + reg);
}

static inline void lg_wr(struct lg_display *d, uint32_t reg, uint32_t val)
{
	i915_write32(d->i915, d->base + reg, val);
}

static inline void lg_rmw(struct lg_display *d, uint32_t reg, uint32_t clear,
			  uint32_t set)
{
	lg_wr(d, reg, (lg_rd(d, reg) & ~clear) | set);
}

static inline void lg_posting_read(struct lg_display *d, uint32_t reg)
{
	(void)lg_rd(d, reg);
}

/* Interrupt context: no forcewake bookkeeping. */
static inline uint32_t lg_rd_fw(struct lg_display *d, uint32_t reg)
{
	return i915_read32_fw(d->i915, d->base + reg);
}

static inline void lg_wr_fw(struct lg_display *d, uint32_t reg, uint32_t val)
{
	i915_write32_fw(d->i915, d->base + reg, val);
}

/* Registers outside the display block (MCHBAR mirror, pcode, ...). */
static inline uint32_t lg_rd_abs(struct lg_display *d, uint32_t reg)
{
	return i915_read32(d->i915, reg);
}

static inline void lg_wr_abs(struct lg_display *d, uint32_t reg, uint32_t val)
{
	i915_write32(d->i915, reg, val);
}

/* Poll until (reg & mask) == value; 0, or -ETIMEDOUT after `timeout_us'. */
int lg_wait(struct lg_display *d, uint32_t reg, uint32_t mask, uint32_t value,
	    uint32_t timeout_us);

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#ifndef DIV_ROUND_CLOSEST
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#endif

/* ---- the core (intel_legacy_display.c) --------------------------------------------- */

/* The one display (file-static in the core). */
struct lg_display *lg_get(void);
/* A free output slot, zeroed, with conn/crtc/pipe -1 and sibling -1;
 * NULL when full.  The caller fills it in; the core adds the DRM
 * connector after every output was probed. */
struct lg_output *lg_output_new(struct lg_display *d);
/* The pipe's timing from a mode (blanking = active..total, interlaced
 * modes' vertical values halved). */
void lg_timings_from_mode(struct lg_timings *t, const struct drm_mode_modeinfo *m);
/* M/N for `bpp' bits per pixel at `pixel_clock' over `nlanes' lanes of
 * `link_clock' (kHz; DP: the link rate, FDI: its clock). */
void lg_link_compute_m_n(int bpp, int nlanes, uint32_t pixel_clock,
			 uint32_t link_clock, struct lg_link_m_n *m_n);
/* Wait for the pipe's next vertical blank (polled, no interrupt needed). */
void lg_wait_for_vblank(struct lg_display *d, int pipe);
/* Is the pipe's scan line moving? */
int lg_pipe_scanline_moving(struct lg_display *d, int pipe);
/* Live state of a hotplug pin (DP/HDMI ports on G4X/VLV/CHV and the PCH,
 * port A on Ironlake to Ivy Bridge).  -1 when the pin has none. */
int lg_hpd_live(struct lg_display *d, int pin);
/* Read the sink's EDID over o->ddc into o->edid; 0, or -ENODEV when none
 * answered.  Sets o->hdmi_sink and o->has_audio from it. */
int lg_read_edid(struct lg_display *d, struct lg_output *o);
/* The common get_modes: the kept EDID's modes (reading it if there is
 * none yet), else the standard table up to `max_clock_khz'. */
int lg_get_modes_edid(struct lg_display *d, struct lg_output *o, int conn,
		      uint32_t max_clock_khz);
/* Load detection: light a pipe for `o' with `mode' and no plane, so the
 * output can sense a load; returns the pipe or a negative errno.  An
 * output that is already active returns its pipe untouched. */
int lg_load_detect_get(struct lg_display *d, struct lg_output *o,
		       const struct drm_mode_modeinfo *mode);
void lg_load_detect_release(struct lg_display *d, struct lg_output *o);
/* The mode a running pipe shows, read back from its timing registers and
 * its PLL (the firmware's panel mode, say): 0, or -ENODEV when the pipe
 * is off. */
int lg_pipe_read_mode(struct lg_display *d, int pipe, struct drm_mode_modeinfo *m);
/* Short busy waits for the sequences that need them. */
static inline void lg_udelay(uint32_t us)
{
	lapic_delay_us(us);
}
static inline void lg_mdelay(uint32_t ms)
{
	lapic_delay_ms(ms);
}

/* ---- clocks (intel_legacy_clock.c) ------------------------------------------------- */

/* CDCLK, its maximum, the dot clock limit, the raw clock and (Pineview)
 * the FSB and memory clocks: into d. */
void lg_clocks_init(struct lg_display *d);

/* ---- DPLLs (intel_legacy_dpll.c) ------------------------------------------------- */

/* Dividers and register values for cfg->port_clock (unless the output
 * set clock_set), for the platform's PLL and the output types in cfg.
 * Sets cfg->port_clock to what the PLL makes.  0 or -EINVAL. */
int lg_dpll_compute(struct lg_display *d, struct lg_config *cfg);
/* The PLL's dot clock from dividers (for outputs that pick dividers). */
int lg_i9xx_calc_dpll_params(int refclk, struct lg_dpll *clock);
int lg_pnv_calc_dpll_params(int refclk, struct lg_dpll *clock);
int lg_vlv_calc_dpll_params(int refclk, struct lg_dpll *clock);
int lg_chv_calc_dpll_params(int refclk, struct lg_dpll *clock);
/* The register values for dividers already in cfg->dpll (clock_set). */
void lg_dpll_compute_regs(struct lg_display *d, struct lg_config *cfg);
/* What the PLL of a running pipe makes now (kHz; including the pixel
 * multiplier), read back from its registers; 0 when it is off. */
uint32_t lg_dpll_get_port_clock(struct lg_display *d, int pipe);
/* GMCH (gen2-4): FP0/FP1/DPLL/DPLL_MD of the pipe, and off. */
void lg_i9xx_enable_pll(struct lg_display *d, const struct lg_config *cfg);
void lg_i9xx_disable_pll(struct lg_display *d, int pipe);
/* i830: both pipes must run all the time; a pipe nobody uses runs
 * 640x480 from its own PLL. */
void lg_i830_enable_pipe(struct lg_display *d, int pipe);
void lg_i830_disable_pipe(struct lg_display *d, int pipe);
/* Ironlake to Ivy Bridge: pick the PCH PLL for a pipe (fixed on Ibex
 * Peak; shared or free on Cougar Point) in the light of `cfgs' (the new
 * configuration of every pipe, NULL when off); sets cfg->pch_dpll. */
int lg_pch_dpll_reserve(struct lg_display *d, struct lg_config *cfg,
			struct lg_config *const cfgs[LG_MAX_PIPES]);
/* PCH_DPLL_SEL, then the PLL on for this pipe (refcounted) -- called
 * from the PCH enable sequence -- and off again. */
void lg_pch_dpll_enable(struct lg_display *d, const struct lg_config *cfg);
void lg_pch_dpll_disable(struct lg_display *d, const struct lg_config *cfg);
/* The PCH reference clocks (PCH_DREF_CONTROL), once the outputs are
 * known: SSC for a panel that wants it, the CPU source for CPU eDP. */
void lg_pch_refclk_init(struct lg_display *d);

/* ---- planes, cursor, palette (intel_legacy_plane.c) ---------------------------------- */

void lg_plane_init(struct lg_display *d);
/* Program the primary plane of `pipe' with the framebuffer `fb' at
 * source offset (x, y), or turn it off when fb is NULL. */
int lg_plane_update(struct lg_display *d, int pipe, struct drm_framebuffer *fb,
		    uint32_t x, uint32_t y);
/* Just a new surface (a flip with an otherwise unchanged plane). */
int lg_plane_flip(struct lg_display *d, int pipe, struct drm_framebuffer *fb,
		  uint32_t x, uint32_t y);
void lg_plane_disable(struct lg_display *d, int pipe);
int lg_plane_enabled_hw(struct lg_display *d, int plane, int *pipe);
/* The cursor plane: the state as the core checked it. */
int lg_cursor_update(struct lg_display *d, int pipe, const struct drm_plane_state *ps);
void lg_cursor_disable(struct lg_display *d, int pipe);
int lg_cursor_check(struct lg_display *d, const struct drm_plane_state *ps);
int lg_plane_check(struct lg_display *d, const struct drm_plane_state *ps,
		   const struct lg_config *cfg);
uint32_t lg_cursor_max_size(struct lg_display *d);
/* The 8-bit palette of the pipe from 16-bit tables. */
void lg_gamma_load(struct lg_display *d, int pipe, const uint16_t gamma[3][256]);
/* VGA plane off (the VGA control register). */
void lg_vga_disable(struct lg_display *d);

/* ---- watermarks (intel_legacy_wm.c) ------------------------------------------------ */

/* Latencies, FIFO sizes; self refresh off until lg_wm_update decides. */
void lg_wm_init(struct lg_display *d);
/* Program every pipe's watermarks (and the FIFO split, self refresh)
 * from d->pipes[]: active, cfg.t / cfg.pixel_rate, the plane's width,
 * cpp and layout, the cursor's size.  Call after a pipe or plane change. */
void lg_wm_update(struct lg_display *d);
/* Self refresh off before planes or pipes are turned off or changed. */
void lg_wm_cxsr_disable(struct lg_display *d);

/* ---- FDI and the PCH transcoders (intel_legacy_fdi.c) -------------------------------- */

void lg_fdi_init(struct lg_display *d);
/* FDI lanes and M/N for a PCH output on cfg->pipe (cfg->t, pipe_bpp). */
int lg_fdi_compute_config(struct lg_display *d, struct lg_config *cfg);
/* The lanes of all pipes together (Ivy Bridge's B and C share).  cfgs[p]
 * is the new configuration of pipe p, NULL when off. */
int lg_fdi_check_lanes(struct lg_display *d, struct lg_config *const cfgs[LG_MAX_PIPES]);
/* Before the CPU pipe is enabled. */
void lg_fdi_pll_enable(struct lg_display *d, const struct lg_config *cfg);
/* After the CPU pipe: FDI training, the PCH PLL, the PCH transcoder's
 * timing and DP control, the transcoder on. */
void lg_pch_enable(struct lg_display *d, const struct lg_config *cfg);
/* After the CPU pipe went off: FDI off; then (after the port's
 * post_disable) the transcoder and the FDI and PCH PLLs off. */
void lg_pch_disable(struct lg_display *d, const struct lg_config *cfg);
void lg_pch_post_disable(struct lg_display *d, const struct lg_config *cfg);
/* The FDI link clock (kHz). */
uint32_t lg_fdi_link_freq(struct lg_display *d);

/* ---- Valleyview / Cherryview (intel_legacy_vlv.c) ----------------------------------- */

/* IOSF sideband units */
uint32_t lg_vlv_punit_read(struct lg_display *d, uint32_t addr);
int lg_vlv_punit_write(struct lg_display *d, uint32_t addr, uint32_t val);
uint32_t lg_vlv_cck_read(struct lg_display *d, uint32_t reg);
void lg_vlv_cck_write(struct lg_display *d, uint32_t reg, uint32_t val);
uint32_t lg_vlv_ccu_read(struct lg_display *d, uint32_t reg);
void lg_vlv_ccu_write(struct lg_display *d, uint32_t reg, uint32_t val);
uint32_t lg_vlv_nc_read(struct lg_display *d, uint32_t addr);
uint32_t lg_vlv_dpio_read(struct lg_display *d, int phy, uint32_t reg);
void lg_vlv_dpio_write(struct lg_display *d, int phy, uint32_t reg, uint32_t val);
uint32_t lg_vlv_flisdsi_read(struct lg_display *d, uint32_t reg);
void lg_vlv_flisdsi_write(struct lg_display *d, uint32_t reg, uint32_t val);
/* any unit by IOSF port number (the GPIO communities for DSI panels) */
uint32_t lg_vlv_iosf_read(struct lg_display *d, int port, uint32_t reg);
void lg_vlv_iosf_write(struct lg_display *d, int port, uint32_t reg, uint32_t val);
/* clocks: HPLL, CZCLK, CDCLK (raised to the most the part allows) */
void lg_vlv_clocks_init(struct lg_display *d);
/* the display power wells and the DPIO PHYs: on, and kept on */
int lg_vlv_power_init(struct lg_display *d);
void lg_vlv_power_fini(struct lg_display *d);
/* which PHY / channel serves a port or pipe */
int lg_vlv_port_to_phy(struct lg_display *d, int port);
int lg_vlv_port_to_channel(int port);
int lg_vlv_pipe_to_phy(struct lg_display *d, int pipe);
int lg_vlv_pipe_to_channel(int pipe);
/* the pipe PLLs (through the PHY) */
void lg_vlv_enable_pll(struct lg_display *d, const struct lg_config *cfg);
void lg_chv_enable_pll(struct lg_display *d, const struct lg_config *cfg);
void lg_vlv_disable_pll(struct lg_display *d, int pipe);
void lg_chv_disable_pll(struct lg_display *d, int pipe);
/* the port's PHY lanes around the PLL and port enable */
void lg_vlv_phy_pre_pll_enable(struct lg_display *d, struct lg_output *o,
			       const struct lg_config *cfg);
void lg_vlv_phy_pre_encoder_enable(struct lg_display *d, struct lg_output *o,
				   const struct lg_config *cfg);
void lg_vlv_phy_reset_lanes(struct lg_display *d, struct lg_output *o,
			    const struct lg_config *cfg);
void lg_vlv_set_phy_signal_level(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, uint32_t demph_reg_value,
				 uint32_t preemph_reg_value, uint32_t uniqtranscale_reg_value,
				 uint32_t tx3_demph);
void lg_chv_phy_pre_pll_enable(struct lg_display *d, struct lg_output *o,
			       const struct lg_config *cfg);
void lg_chv_phy_pre_encoder_enable(struct lg_display *d, struct lg_output *o,
				   const struct lg_config *cfg);
void lg_chv_phy_release_cl2_override(struct lg_display *d, struct lg_output *o);
void lg_chv_phy_post_pll_disable(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg);
void lg_chv_set_phy_signal_level(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, uint32_t deemph_reg_value,
				 uint32_t margin_reg_value, int uniq_trans_scale);
void lg_chv_data_lane_soft_reset(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg, int reset);
/* Cherryview: the power-down override of a port's PHY lanes: `mask' is
 * the lanes powered DOWN (0xc for a two-lane link); override 0 releases
 * it.  lg_chv_phy_pre_pll_enable / post_pll_disable already do this. */
void lg_chv_phy_powergate_lanes(struct lg_display *d, struct lg_output *o,
				int override, unsigned int mask);
/* Run a pipe's PLL without the pipe (the eDP power sequencer of VLV/CHV
 * needs a clock to switch ports), and stop it again. */
int lg_vlv_force_pll_on(struct lg_display *d, int pipe, const struct lg_dpll *dpll);
void lg_vlv_force_pll_off(struct lg_display *d, int pipe);
/* Wait for the port's lanes to report ready (DPLL / DPIO_PHY_STATUS
 * ready bits) after the port was enabled; `expected_mask' as the
 * hardware's lane bits (0xf for four lanes). */
void lg_vlv_wait_port_ready(struct lg_display *d, struct lg_output *o,
			    unsigned int expected_mask);

/* ---- GMBUS and DDC (intel_legacy_gmbus.c) ------------------------------------------ */

int lg_gmbus_init(struct lg_display *d);
/* After a resume: the controller back to idle. */
void lg_gmbus_reset(struct lg_display *d);
int lg_gmbus_pin_valid(struct lg_display *d, unsigned pin);
/* The I2C adapter of a pin (GMBUS, falling back to bit-banging the
 * pin's GPIO pair when the controller fails), NULL for an invalid pin. */
struct i2c_adapter *lg_gmbus_adapter(struct lg_display *d, unsigned pin);
/* Bit-bang this pin instead of using the controller (SDVO control bus,
 * i830). */
void lg_gmbus_force_bit(struct lg_display *d, unsigned pin, int force);
/* GMBUS_RATE_* for the pin's transfers. */
void lg_gmbus_set_speed(struct lg_display *d, unsigned pin, uint32_t rate);

/* ---- the VBT (intel_legacy_vbt.c) ------------------------------------------------- */

/* Find the VBT (OpRegion, else the video BIOS image) and parse it into
 * d->vbt; without one d->vbt holds the defaults. */
int lg_vbt_init(struct lg_display *d);
void lg_vbt_fini(struct lg_display *d);
/* A BDB block's body by id, NULL when absent; *len its size. */
const uint8_t *lg_vbt_block(struct lg_display *d, int id, uint32_t *len);
/* The child device describing integrated port `port' (B, C, D; A for
 * eDP), NULL when the VBT names none. */
const struct lg_vbt_child *lg_vbt_child_for_port(struct lg_display *d, int port);
int lg_vbt_port_present(struct lg_display *d, int port);
int lg_vbt_port_is_edp(struct lg_display *d, int port);
/* Is there an LVDS panel (and the GMBUS pin of its DDC)? */
int lg_vbt_lvds_present(struct lg_display *d, uint8_t *i2c_pin);
int lg_vbt_tv_present(struct lg_display *d);
/* Is there a DSI panel, and on which DSI port (0 = A, 2 = C)? */
int lg_vbt_dsi_present(struct lg_display *d, int *port);

/* ---- panel: power sequencer, backlight, fitters (intel_legacy_panel.c) ----------------- */

/* Display init: unlock the sequencer registers (the PLLs behind a locked
 * sequencer refuse writes), and on VLV/CHV detach unused sequencers. */
void lg_pps_init(struct lg_display *d);
/* An eDP panel found on `o': its sequencer's delays from the VBT and the
 * hardware, programmed. */
void lg_pps_edp_init(struct lg_display *d, struct lg_output *o);
/* eDP: force panel VDD for the AUX channel, and drop it again. */
void lg_pps_vdd_on(struct lg_display *d, struct lg_output *o);
void lg_pps_vdd_off(struct lg_display *d, struct lg_output *o);
/* Panel power on / off through the sequencer, waiting it out. */
void lg_pps_on(struct lg_display *d, struct lg_output *o);
void lg_pps_off(struct lg_display *d, struct lg_output *o);
/* eDP: the sequencer's backlight enable (EDP_BLC_ENABLE) with T8/T9. */
void lg_pps_backlight_on(struct lg_display *d, struct lg_output *o);
void lg_pps_backlight_off(struct lg_display *d, struct lg_output *o);
/* VLV/CHV: the sequencer of `pipe' takes the eDP port (before the port
 * is enabled), and lets it go. */
void lg_pps_vlv_port_enable(struct lg_display *d, struct lg_output *o, int pipe);
void lg_pps_vlv_port_disable(struct lg_display *d, struct lg_output *o);
/* Backlight of the panel on `o' (LVDS or eDP) */
int lg_backlight_setup(struct lg_display *d, struct lg_output *o, int pipe);
void lg_backlight_enable(struct lg_display *d, struct lg_output *o,
			 const struct lg_config *cfg);
void lg_backlight_disable(struct lg_display *d, struct lg_output *o);
int lg_backlight_set(struct lg_display *d, uint32_t level);
uint32_t lg_backlight_get(struct lg_display *d);
uint32_t lg_backlight_max(struct lg_display *d);
/* Panel fitting: cfg->mode is the panel's timing (cfg->t filled from it),
 * cfg->src_w/src_h the client's picture; aspect-preserving scaling into
 * cfg->gmch_pfit or cfg->pch_pfit (and cfg->t's borders on GMCH). */
int lg_pfit_compute(struct lg_display *d, struct lg_output *o, struct lg_config *cfg);
void lg_pfit_enable(struct lg_display *d, const struct lg_config *cfg);
void lg_pfit_disable(struct lg_display *d, const struct lg_config *cfg);

/* ---- outputs: each file creates its outputs with lg_output_new() ------------------- */

void lg_crt_init(struct lg_display *d); /* intel_legacy_crt.c */
void lg_lvds_init(struct lg_display *d); /* intel_legacy_lvds.c */
/* An integrated HDMI/DVI port (G4X, PCH, VLV/CHV). */
void lg_hdmi_init(struct lg_display *d, uint32_t reg, int port); /* intel_legacy_hdmi.c */
/* An integrated DisplayPort (or eDP) port; 1 when an output was made. */
int lg_dp_init(struct lg_display *d, uint32_t reg, int port); /* intel_legacy_dp.c */
/* An SDVO encoder on SDVO B/C; 1 when one answered. */
int lg_sdvo_init(struct lg_display *d, uint32_t reg, int port); /* intel_legacy_sdvo.c */
void lg_dvo_init(struct lg_display *d); /* intel_legacy_dvo.c */
void lg_tv_init(struct lg_display *d); /* intel_legacy_tv.c */
void lg_dsi_init(struct lg_display *d); /* intel_legacy_dsi.c */

#endif
