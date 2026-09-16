// LikeOS -- the display side of the Intel graphics driver.
//
// Outputs (DDI ports carrying eDP, DisplayPort or HDMI), the pipes and
// transcoders that feed them, the PLLs that clock them, the power wells
// they sit in, and the panel's power sequencing and backlight.  The
// state lives in struct intel_display inside the device; the mode-object
// side (connectors, CRTCs) is the DRM core's, mapped onto these by index.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_DISPLAY_H
#define KERNEL_DEV_GPU_I915_INTEL_DISPLAY_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/i2c.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h>

struct i915_device;

/* DDI A..E, plus the Type-C ports of Ice Lake (F) and of Tiger Lake and
 * later (TC1..TC6 at indices 3..8). */
#define INTEL_MAX_PORTS 9
/* A DDI wired for both DisplayPort and HDMI is two outputs (two
 * connectors) sharing one port, only one of them up at a time. */
#define INTEL_MAX_OUTPUTS DRM_MAX_CONNECTORS
#define INTEL_MAX_PIPES 4
#define INTEL_MAX_DPLLS 8 /* Skylake: DPLL0..3; Ice Lake: DPLL0..4 + TBT */

enum intel_output_type {
	INTEL_OUTPUT_NONE = 0,
	INTEL_OUTPUT_EDP,
	INTEL_OUTPUT_DP,
	INTEL_OUTPUT_HDMI,
	INTEL_OUTPUT_DVI,
};

/* ---- what the VBT says about a port (intel_vbt_parse.c, pure) ------------- */

struct intel_vbt_port {
	int present; /* a child device names this port */
	int supports_dp, supports_hdmi, supports_dvi, supports_edp;
	int is_internal; /* the panel */
	uint8_t aux_ch; /* AUX channel letter index (0 = A), 0xff none */
	uint8_t ddc_pin; /* GMBUS pin (VBT numbering), 0 none */
	uint8_t hdmi_level_shift; /* 0xff = default */
	uint8_t lane_reversal;
};

struct intel_vbt_pps {
	uint16_t t1_t3; /* panel power on to AUX/VDD ready, 100us units */
	uint16_t t8; /* power on to backlight on */
	uint16_t t9; /* backlight off to power off */
	uint16_t t10; /* power down */
	uint16_t t11_t12; /* power cycle delay */
	int valid;
};

struct intel_vbt {
	int valid;
	uint16_t version; /* BDB version */
	char signature[24];
	struct intel_vbt_port port[INTEL_MAX_PORTS];
	int nchildren;
	/* the internal panel */
	int panel_type; /* index into the LFP tables, -1 unknown */
	struct drm_mode_modeinfo panel_mode; /* from the LFP data block */
	int panel_mode_valid;
	struct intel_vbt_pps pps;
	uint8_t edp_rate; /* 0=1.62 1=2.7 2=5.4 GHz */
	uint8_t edp_lanes; /* 1, 2, 4 */
	uint8_t edp_vswing_preemph; /* VBT table index */
	uint8_t edp_low_vswing;
	/* backlight */
	int backlight_valid;
	uint16_t backlight_pwm_hz;
	uint8_t backlight_min;
	uint8_t backlight_active_low;
	uint8_t backlight_controller; /* 0/1 = PWM controller */
	int panel_bpc; /* from the LFP data block, 0 unknown */
	int int_crt_support, int_lvds_support;
	int lvds_dither;
};

/* Parse a VBT image.  `len' bytes starting at the "$VBT" signature.
 * Returns 0 or -EINVAL. */
int intel_vbt_parse(const uint8_t *vbt, unsigned len, struct intel_vbt *out);

/* ---- the OpRegion (intel_opregion.c) --------------------------------------- */

struct intel_opregion {
	uint64_t phys; /* from the ASLS config register, 0 = none */
	void *virt; /* 8 KB mapped */
	uint32_t size; /* 8 KB, plus the extended VBT when there is one */
	const uint8_t *vbt; /* inside the region (or the extended area) */
	uint32_t vbt_size;
	void *rvda_virt; /* the extended VBT mapping, if any */
	uint32_t rvda_pages;
	uint8_t asle_present;
};

int intel_opregion_init(struct i915_device *i915);
void intel_opregion_fini(struct i915_device *i915);
/* Tell the firmware the driver has the display; asks for the ACPI
 * backlight to be routed to the driver (ASLE). */
void intel_opregion_driver_ready(struct i915_device *i915);

/* ---- DisplayPort: AUX and DPCD (intel_dp_aux.c) --------------------------- */

#define DP_DPCD_REV 0x000
#define DP_MAX_LINK_RATE 0x001
#define DP_MAX_LANE_COUNT 0x002
#define DP_MAX_LANE_COUNT_MASK 0x1f
#define DP_TPS3_SUPPORTED (1 << 6)
#define DP_ENHANCED_FRAME_CAP (1 << 7)
#define DP_MAX_DOWNSPREAD 0x003
#define DP_TPS4_SUPPORTED (1 << 7)
#define DP_NORP 0x004
#define DP_DOWNSTREAMPORT_PRESENT 0x005
#define DP_MAIN_LINK_CHANNEL_CODING 0x006
#define DP_EDP_CONFIGURATION_CAP 0x00d
#define DP_TRAINING_AUX_RD_INTERVAL 0x00e
#define DP_SUPPORTED_LINK_RATES 0x010 /* eDP 1.4: 8 x 16-bit, 200 kHz units */
#define DP_LINK_BW_SET 0x100
#define DP_LINK_BW_1_62 0x06
#define DP_LINK_BW_2_7 0x0a
#define DP_LINK_BW_5_4 0x14
#define DP_LINK_BW_8_1 0x1e
#define DP_LANE_COUNT_SET 0x101
#define DP_LANE_COUNT_ENHANCED_FRAME_EN (1 << 7)
#define DP_TRAINING_PATTERN_SET 0x102
#define DP_TRAINING_PATTERN_DISABLE 0
#define DP_TRAINING_PATTERN_1 1
#define DP_TRAINING_PATTERN_2 2
#define DP_TRAINING_PATTERN_3 3
#define DP_TRAINING_PATTERN_4 7
#define DP_LINK_SCRAMBLING_DISABLE (1 << 5)
#define DP_TRAINING_LANE0_SET 0x103
#define DP_TRAIN_VOLTAGE_SWING_MASK 0x3
#define DP_TRAIN_VOLTAGE_SWING_SHIFT 0
#define DP_TRAIN_MAX_SWING_REACHED (1 << 2)
#define DP_TRAIN_PRE_EMPHASIS_MASK (3 << 3)
#define DP_TRAIN_PRE_EMPHASIS_SHIFT 3
#define DP_TRAIN_MAX_PRE_EMPHASIS_REACHED (1 << 5)
#define DP_DOWNSPREAD_CTRL 0x107
#define DP_SPREAD_AMP_0_5 (1 << 4)
#define DP_MAIN_LINK_CHANNEL_CODING_SET 0x108
#define DP_SET_ANSI_8B10B (1 << 0)
#define DP_LINK_RATE_SET 0x115 /* eDP 1.4: index into SUPPORTED_LINK_RATES */
#define DP_EDP_CONFIGURATION_SET 0x10a
#define DP_SET_POWER 0x600
#define DP_SET_POWER_D0 0x1
#define DP_SET_POWER_D3 0x2
#define DP_LANE0_1_STATUS 0x202
#define DP_LANE2_3_STATUS 0x203
#define DP_LANE_CR_DONE (1 << 0)
#define DP_LANE_CHANNEL_EQ_DONE (1 << 1)
#define DP_LANE_SYMBOL_LOCKED (1 << 2)
#define DP_LANE_ALIGN_STATUS_UPDATED 0x204
#define DP_INTERLANE_ALIGN_DONE (1 << 0)
#define DP_SINK_STATUS 0x205
#define DP_ADJUST_REQUEST_LANE0_1 0x206
#define DP_ADJUST_REQUEST_LANE2_3 0x207
#define DP_EDP_DPCD_REV 0x700
#define DP_EDP_GENERAL_CAP_1 0x701
#define DP_EDP_BACKLIGHT_MODE_SET_REGISTER 0x721
#define DP_EDP_DISPLAY_CONTROL_REGISTER 0x720
#define DP_EDP_BACKLIGHT_BRIGHTNESS_MSB 0x722
#define DP_EDP_BACKLIGHT_BRIGHTNESS_LSB 0x723
#define DP_SINK_COUNT 0x200
#define DP_RECEIVER_CAP_SIZE 15

struct intel_dp_aux {
	struct i915_device *i915;
	int port; /* DDI port, which names the AUX channel on Skylake */
	uint32_t ctl_reg, data_reg;
	struct i2c_adapter i2c; /* I2C over AUX, for the EDID */
	uint32_t errors;
};

void intel_dp_aux_init(struct i915_device *i915, struct intel_dp_aux *aux,
		       int port);
/* Native AUX: read/write `len' (1..16) bytes at DPCD `addr'.  Returns
 * the byte count or a negative errno. */
int intel_dp_aux_native_read(struct intel_dp_aux *aux, uint32_t addr,
			     uint8_t *buf, unsigned len);
int intel_dp_aux_native_write(struct intel_dp_aux *aux, uint32_t addr,
			      const uint8_t *buf, unsigned len);
static inline int intel_dp_dpcd_read8(struct intel_dp_aux *aux, uint32_t addr,
				      uint8_t *v)
{
	return intel_dp_aux_native_read(aux, addr, v, 1) == 1 ? 0 : -1;
}
static inline int intel_dp_dpcd_write8(struct intel_dp_aux *aux, uint32_t addr,
				       uint8_t v)
{
	return intel_dp_aux_native_write(aux, addr, &v, 1) == 1 ? 0 : -1;
}

/* ---- power wells (intel_power.c) ----------------------------------------- */

enum intel_power_domain {
	INTEL_PW_PIPE_A = 0,
	INTEL_PW_PIPE_B,
	INTEL_PW_PIPE_C,
	INTEL_PW_PIPE_D,
	INTEL_PW_TRANSCODER_EDP,
	INTEL_PW_DDI_A, /* DDI_A + port */
	INTEL_PW_DDI_B,
	INTEL_PW_DDI_C,
	INTEL_PW_DDI_D,
	INTEL_PW_DDI_E,
	INTEL_PW_DDI_F,
	INTEL_PW_DDI_G,
	INTEL_PW_DDI_H,
	INTEL_PW_DDI_I,
	INTEL_PW_AUX_A, /* AUX_A + port */
	INTEL_PW_AUX_B,
	INTEL_PW_AUX_C,
	INTEL_PW_AUX_D,
	INTEL_PW_AUX_E,
	INTEL_PW_AUX_F,
	INTEL_PW_AUX_G,
	INTEL_PW_AUX_H,
	INTEL_PW_AUX_I,
	INTEL_PW_GMBUS,
	INTEL_PW_DISPLAY_CORE,
	INTEL_PW_COUNT
};

/* How the display of this part is put together: which registers its
 * power wells, clocks, PLLs, hotplug pins, panel sequencer and backlight
 * live in.  Chosen once from the device info and the PCH. */
enum intel_display_model {
	INTEL_DISPLAY_SKL = 0, /* Skylake, Kaby/Coffee/Comet Lake */
	INTEL_DISPLAY_BDW, /* Broadwell */
	INTEL_DISPLAY_BXT, /* Broxton, Gemini Lake */
	INTEL_DISPLAY_ICL, /* Ice Lake, Elkhart/Jasper Lake */
	INTEL_DISPLAY_TGL, /* Tiger/Rocket/Alder/Raptor Lake */
};

int intel_power_init(struct i915_device *i915);
void intel_power_fini(struct i915_device *i915);
void intel_power_get(struct i915_device *i915, enum intel_power_domain d);
void intel_power_put(struct i915_device *i915, enum intel_power_domain d);

/* ---- clocks (intel_cdclk.c, intel_dpll.c) ------------------------------- */

int intel_pcode_read(struct i915_device *i915, uint32_t mbox, uint32_t *val,
		     uint32_t *val1);
int intel_pcode_write(struct i915_device *i915, uint32_t mbox, uint32_t val);
int intel_cdclk_init(struct i915_device *i915);
uint32_t intel_cdclk_khz(struct i915_device *i915);

struct intel_dpll_state {
	int in_use; /* refcount */
	int is_hdmi; /* WRPLL divider mode vs DP link rate */
	int ssc; /* spread spectrum on the link */
	uint32_t link_rate_khz; /* for DP: 162000, 270000, 540000 */
	uint32_t cfgcr1, cfgcr2; /* HDMI dividers */
	uint32_t cfgcr0; /* Gen11+: the combo PLL's first configuration word */
};

/* Find or program a PLL for the given output; returns the DPLL id
 * (0..3) or a negative errno.  `ssc' asks for spread spectrum, which an
 * embedded panel's link usually runs with. */
int intel_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz,
		      int ssc);
/* What the port is clocked at right now, from the PLL it is routed to:
 * the link rate in kHz, or 0 when the port has no clock.  Used to read
 * out the link the firmware left running. */
uint32_t intel_dpll_port_link_rate(struct i915_device *i915, int port);
/* The link rates a port's PLL can be given on this generation. */
int intel_dpll_rate_supported(struct i915_device *i915, uint32_t link_rate_khz);
int intel_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz);
void intel_dpll_put(struct i915_device *i915, int pll);
/* Route the port's clock to the PLL (DPLL_CTRL2) or cut it. */
void intel_dpll_route_port(struct i915_device *i915, int port, int pll);
void intel_dpll_unroute_port(struct i915_device *i915, int port);
int intel_dpll_init(struct i915_device *i915);

/* The per-model PLL code behind the intel_dpll_* entry points above. */
int icl_dpll_init(struct i915_device *i915);
int icl_dpll_rate_supported(uint32_t link_rate_khz);
int icl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc);
int icl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz);
void icl_dpll_put(struct i915_device *i915, int pll);
void icl_dpll_route_port(struct i915_device *i915, int port, int pll);
void icl_dpll_unroute_port(struct i915_device *i915, int port);
uint32_t icl_dpll_port_link_rate(struct i915_device *i915, int port);
int icl_combo_phy_init(struct i915_device *i915, int port);
void icl_combo_phy_power_lanes(struct i915_device *i915, int port, int lanes, int reversed);
struct intel_output;
void icl_combo_set_signal_level(struct i915_device *i915, struct intel_output *o,
				int level, int lanes);
int skl_dpll_init(struct i915_device *i915);
int skl_dpll_rate_supported(uint32_t link_rate_khz);
int skl_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc);
int skl_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz);
void skl_dpll_put(struct i915_device *i915, int pll);
void skl_dpll_route_port(struct i915_device *i915, int port, int pll);
void skl_dpll_unroute_port(struct i915_device *i915, int port);
uint32_t skl_dpll_port_link_rate(struct i915_device *i915, int port);
int bxt_dpll_init(struct i915_device *i915);
int bxt_dpll_rate_supported(uint32_t link_rate_khz);
int bxt_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc);
int bxt_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz);
void bxt_dpll_put(struct i915_device *i915, int port);
void bxt_dpll_route_port(struct i915_device *i915, int port, int pll);
void bxt_dpll_unroute_port(struct i915_device *i915, int port);
uint32_t bxt_dpll_port_link_rate(struct i915_device *i915, int port);
int bxt_phy_init(struct i915_device *i915, int port);
void bxt_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level);
int hsw_dpll_init(struct i915_device *i915);
int hsw_dpll_rate_supported(uint32_t link_rate_khz);
int hsw_dpll_get_dp(struct i915_device *i915, int port, uint32_t link_rate_khz, int ssc);
int hsw_dpll_get_hdmi(struct i915_device *i915, int port, uint32_t clock_khz);
void hsw_dpll_put(struct i915_device *i915, int pll);
void hsw_dpll_route_port(struct i915_device *i915, int port, int pll);
void hsw_dpll_unroute_port(struct i915_device *i915, int port);
uint32_t hsw_dpll_port_link_rate(struct i915_device *i915, int port);
void bdw_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o, int level);
/* Type-C (intel_tc.c): the FIA's mode and lanes, the MG/DKL PLLs and PHYs. */
#define INTEL_TC_PLL_BASE 2 /* the dpll[] slots after the two combo PLLs */
#define INTEL_TBT_PLL_ID 99 /* the shared Thunderbolt PLL, refcounted apart */
int intel_tc_connect(struct i915_device *i915, struct intel_output *o);
void intel_tc_disconnect(struct i915_device *i915, struct intel_output *o);
int intel_tc_max_lanes(struct i915_device *i915, struct intel_output *o);
int intel_tc_dpll_get_dp(struct i915_device *i915, struct intel_output *o,
			 uint32_t link_rate_khz, int ssc);
int intel_tc_dpll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz);
void intel_tc_dpll_put(struct i915_device *i915, int pll);
void intel_tc_route_port(struct i915_device *i915, struct intel_output *o, int pll);
void intel_tc_unroute_port(struct i915_device *i915, struct intel_output *o);
uint32_t intel_tc_port_link_rate(struct i915_device *i915, struct intel_output *o);
void intel_tc_program_dp_mode(struct i915_device *i915, struct intel_output *o, int lanes);
void intel_tc_set_signal_level(struct i915_device *i915, struct intel_output *o, int level);
void intel_tc_clock_gating(struct i915_device *i915, struct intel_output *o, int enable);

/* ---- outputs (intel_ddi.c, intel_dp.c, intel_hdmi.c) ---------------------- */

struct intel_output {
	int present;
	int port; /* DDI A..E */
	enum intel_output_type type; /* what is wired now */
	int conn; /* DRM connector index, -1 none */
	int crtc; /* DRM crtc index when active, -1 */
	int pipe; /* pipe when active */
	int transcoder;
	int pll; /* DPLL id when active */
	int active;
	/* DP */
	struct intel_dp_aux aux;
	uint8_t dpcd[DP_RECEIVER_CAP_SIZE];
	uint8_t edp_dpcd[4];
	uint32_t sink_rates_khz[8];
	int nsink_rates;
	uint32_t link_rate_khz; /* chosen */
	int lane_count; /* chosen */
	int ssc; /* the link runs with spread spectrum */
	/* A panel whose table says "low voltage swing" is driven from the
	 * smaller table; set after a failed attempt to fall back to the
	 * standard one rather than leave the screen dark. */
	int swing_table_std;
	/* What the firmware had this port running at, read out before its
	 * pipe was taken apart: the configuration most likely to train. */
	uint32_t fw_link_rate_khz;
	int fw_lanes;
	/* DDI A carries four lanes only where the board wired them that way
	 * (else two, with the other two going to DDI E).  The strap says so,
	 * and is read once: enabling the buffer for a two-lane link clears
	 * the bit, so later reads would understate the port. */
	int four_lane_strap;
	uint8_t train_set[4];
	int is_edp;
	int panel_powered;
	int vdd_forced;
	/* HDMI/DVI */
	uint8_t ddc_pin; /* the VBT's DDC pin */
	uint8_t gmbus_pin; /* the GMBUS pin it maps to */
	struct i2c_adapter ddc; /* DDC over GMBUS */
	int hdmi_sink; /* the EDID carries the HDMI vendor block */
	uint8_t lane_reversal;
	int sibling; /* the other output on this DDI (dual-mode port), -1 */
	/* the sink */
	uint8_t edid[512];
	int edid_len;
	int detected;
	/* A panel has one timing it takes (the EDID's preferred): any
	 * other mode a client sets is run through the pipe scaler onto
	 * it. */
	struct drm_mode_modeinfo fixed_mode;
	int fixed_mode_valid;
	/* Type-C ports (Ice Lake and later): which PHY the port is on and
	 * the mode the connector was found in (intel_tc.c). */
	int is_tc;
	int tc_index; /* 0..5 */
	int tc_mode; /* enum intel_tc_mode */
	int tc_fia; /* the flexible I/O adapter carrying the port, and */
	int tc_fia_idx; /* the port's index within it */
	int tc_owned;
	uint32_t tc_lanes; /* lanes the FIA grants */
};

int intel_ddi_init(struct i915_device *i915);
void intel_ddi_pre_enable(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *mode);
void intel_ddi_enable(struct i915_device *i915, struct intel_output *o);
void intel_ddi_disable(struct i915_device *i915, struct intel_output *o);
void intel_ddi_post_disable(struct i915_device *i915, struct intel_output *o);
void intel_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o,
			     int level);
int intel_ddi_dp_level_for(int swing, int preemph);
int intel_ddi_level_in_buf_ctl(struct i915_device *i915);
void intel_ddi_set_train_pattern(struct i915_device *i915, struct intel_output *o,
				 uint32_t pattern);
void intel_ddi_dp_tp_enable(struct i915_device *i915, struct intel_output *o,
			    int enhanced_framing);
void intel_ddi_buf_enable(struct i915_device *i915, struct intel_output *o,
			  int lanes, int level);
void intel_ddi_buf_disable(struct i915_device *i915, struct intel_output *o);
int intel_ddi_wait_idle_done(struct i915_device *i915, struct intel_output *o);

/* DP */
int intel_dp_detect(struct i915_device *i915, struct intel_output *o);
int intel_dp_read_edid(struct i915_device *i915, struct intel_output *o);
int intel_dp_choose_link(struct i915_device *i915, struct intel_output *o,
			 const struct drm_mode_modeinfo *mode);
int intel_dp_link_train(struct i915_device *i915, struct intel_output *o);
void intel_dp_link_off(struct i915_device *i915, struct intel_output *o);
void intel_dp_sink_power(struct i915_device *i915, struct intel_output *o, int on);
uint32_t intel_dp_link_bw_khz(uint8_t bw);

/* eDP panel power and backlight (intel_pps.c, intel_backlight.c) */
int intel_pps_init(struct i915_device *i915);
void intel_pps_vdd_on(struct i915_device *i915, struct intel_output *o);
void intel_pps_vdd_off(struct i915_device *i915, struct intel_output *o);
void intel_pps_panel_on(struct i915_device *i915, struct intel_output *o);
void intel_pps_panel_off(struct i915_device *i915, struct intel_output *o);
void intel_pps_backlight(struct i915_device *i915, int on);
int intel_backlight_init(struct i915_device *i915);
void intel_backlight_enable(struct i915_device *i915, struct intel_output *o);
void intel_backlight_disable(struct i915_device *i915, struct intel_output *o);
int intel_backlight_set(struct i915_device *i915, uint32_t level);
uint32_t intel_backlight_get(struct i915_device *i915);
uint32_t intel_backlight_max(struct i915_device *i915);
/* /sys/class/backlight/intel_backlight (when there is a panel) */
int intel_backlight_sysfs_init(struct i915_device *i915);

/* ---- pipes (intel_display.c) ----------------------------------------------- */

struct intel_pipe {
	int pipe;
	int active;
	int transcoder;
	int output; /* index into display.outputs, -1 */
	struct drm_mode_modeinfo mode;
	uint32_t pixel_rate_khz;
	/* What the transcoder runs (mode above) versus what the pipe is
	 * fed: the client's picture size, scaled onto the timing when they
	 * differ. */
	uint32_t src_w, src_h;
	int scaled;
	uint32_t surf_ggtt; /* current scanout */
	uint32_t stride;
	uint32_t width, height;
	uint32_t pos_x, pos_y; /* PLANE_POS: where the plane sits in the pipe */
	uint32_t off_x, off_y; /* PLANE_OFFSET: where the surface starts */
	uint32_t format;   /* DRM_FORMAT_* of the plane as programmed */
	uint64_t modifier; /* its layout (DRM_FORMAT_MOD_LINEAR, X or Y tiled) */
	int vblank_enabled;
	/* a pending flip: the plane was reprogrammed, the event goes out
	 * at the next vblank */
	int flip_pending;
	uint64_t underruns;
	int underrun_reported; /* the interrupt is masked after the first */
	uint32_t cursor_w; /* what the client set, for the watermark */
};

/* How a Type-C connector was found (intel_tc.c). */
enum intel_tc_mode {
	INTEL_TC_DISCONNECTED = 0,
	INTEL_TC_DP_ALT, /* a DisplayPort alternate-mode sink */
	INTEL_TC_TBT_ALT, /* through the Thunderbolt controller */
	INTEL_TC_LEGACY, /* a fixed DP/HDMI connector behind the Type-C PHY */
};

struct intel_display {
	enum intel_display_model model;
	int tbt_pll_users; /* Type-C outputs on the shared Thunderbolt PLL */
	uint32_t pps_base; /* the panel power sequencer's registers */
	int bl_bxt; /* the backlight PWM is the Broxton/Cannon Point kind */
	uint32_t hpd_irq_mask_de; /* Gen11+: the north display's Type-C bits */
	uint32_t hpd_irq_mask_port; /* Broxton: the DE port register's bits */
	uint32_t ddb_blocks; /* the data buffer this driver hands out */
	struct intel_opregion opregion;
	struct intel_vbt vbt;
	int nout;
	struct intel_output outputs[INTEL_MAX_OUTPUTS];
	struct intel_pipe pipes[INTEL_MAX_PIPES];
	struct intel_dpll_state dpll[INTEL_MAX_DPLLS];
	uint32_t dpll0_link_rate_khz; /* set by the firmware, drives CDCLK */
	uint32_t cdclk_khz;
	uint32_t rawclk_khz;
	int pw_count[INTEL_PW_COUNT];
	uint32_t mem_latency[8]; /* watermark latency levels, us */
	int nlatency;
	/* GGTT range for scanout buffers (i915_gtt.c hands them out) */
	/* backlight */
	uint32_t bl_max, bl_level, bl_min;
	int bl_enabled;
	/* The panel has been brought up once, so bl_level is a level somebody
	 * chose rather than an empty field.  A resume re-runs the init and
	 * must keep it; only the first bring-up picks the default. */
	int bl_ready;
	/* GMBUS: one transfer at a time */
	volatile int gmbus_busy;
	/* hotplug: the interrupt notes the ports, the worker re-probes them */
	volatile uint32_t hpd_pending; /* 1 << port */
	uint32_t hpd_irq_mask; /* the south display engine's bits */
	uint64_t hpd_events;
	struct wait_queue_head hpd_wq;
	task_t *hpd_worker;
	volatile int hpd_ready;
	/* the display microcontroller */
	int dmc_loaded;
	uint32_t dmc_version;
	int ready; /* display code initialised */
};

/* ---- the display microcontroller (intel_dmc.c) ----------------------------- */
int intel_dmc_load(struct i915_device *i915);

/* ---- GMBUS, the display engine's I2C controller (intel_gmbus.c) ------------ */
int intel_gmbus_init(struct i915_device *i915);
/* The GMBUS pin a port's DDC is on: the VBT's when that names a pin the
 * platform has, else the port's usual one. */
uint8_t intel_gmbus_pin_for_port(struct i915_device *i915, int port,
				 uint8_t vbt_pin);
void intel_gmbus_adapter_init(struct i915_device *i915, struct i2c_adapter *a,
			      uint8_t pin, const char *name);

/* ---- HDMI and DVI (intel_hdmi.c) ------------------------------------------- */
int intel_hdmi_detect(struct i915_device *i915, struct intel_output *o);
int intel_hdmi_read_edid(struct i915_device *i915, struct intel_output *o);
int intel_hdmi_mode_valid(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m);
/* Before the transcoder: PLL, port clock, buffer translations,
 * infoframes.  Then the transcoder goes on, then intel_hdmi_enable()
 * turns the port's buffer on. */
int intel_hdmi_pre_enable(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m, int transcoder);
void intel_hdmi_enable(struct i915_device *i915, struct intel_output *o);
/* Before the transcoder goes off: infoframes off.  After: buffer off. */
void intel_hdmi_disable(struct i915_device *i915, struct intel_output *o,
			int transcoder);
void intel_hdmi_post_disable(struct i915_device *i915, struct intel_output *o);
/* The HDMI entry of the DDI buffer translations plus the current boost
 * (intel_ddi.c). */
void intel_ddi_prepare_hdmi(struct i915_device *i915, struct intel_output *o,
			    int level);

/* ---- hotplug (intel_hotplug.c) --------------------------------------------- */
/* Is a sink present on the port right now (the hotplug pin's live state)? */
int intel_hpd_live(struct i915_device *i915, int port);
int intel_hpd_init(struct i915_device *i915);
int intel_hpd_start(struct i915_device *i915);

int intel_display_init(struct i915_device *i915);
void intel_display_fini(struct i915_device *i915);
/* Every output off and the wells released (suspend); the display core,
 * clocks, PLL bookkeeping, panel and backlight registers set up again
 * (resume) -- the mode sets come afterwards from the core's replay. */
void intel_display_suspend(struct i915_device *i915);
int intel_display_resume(struct i915_device *i915);
/* The DRM backend entry points intel_display.c implements: the atomic
 * pair (every mode set, flip, cursor and DPMS change comes through them
 * as a checked state) and the rest. */
int intel_atomic_check(struct drm_device *dev, struct drm_atomic_state *st);
int intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st);
int intel_fb_dirty(struct drm_device *dev, struct drm_crtc *crtc,
		   struct drm_framebuffer *fb, const struct drm_mode_rect_k *rects,
		   uint32_t n);
int intel_fb_check(struct drm_device *dev, struct drm_gem_object *o,
		   const struct drm_mode_fb_cmd2 *r, uint64_t *modifier);
/* bytes per tile row for a layout: 64 linear, 512 X, 128 Y; 0 = unknown */
uint32_t intel_fb_tile_width(uint64_t modifier);
uint32_t intel_fb_tile_height(uint64_t modifier);
extern const uint32_t intel_fb_formats[];
extern const uint32_t intel_nfb_formats;
extern const uint64_t intel_fb_modifiers[];
extern const uint32_t intel_nfb_modifiers;
int intel_detect(struct drm_device *dev, struct drm_connector *c);
int intel_get_modes(struct drm_device *dev, struct drm_connector *c);
int intel_display_verify(struct drm_device *dev);
void intel_display_fallback(struct drm_device *dev);
/* Called from the interrupt handler with the DE pipe IIR bits. */
void intel_display_irq(struct i915_device *i915, int pipe, uint32_t iir);
void intel_display_hpd_irq(struct i915_device *i915, uint32_t de_port_iir,
			   uint32_t pch_iir, uint32_t de_hpd_iir);
/* Which of the three the port's DisplayPort transport control lives in. */
uint32_t intel_dp_tp_ctl_reg(struct i915_device *i915, const struct intel_output *o);
uint32_t intel_dp_tp_status_reg(struct i915_device *i915, const struct intel_output *o);
/* A port's name for the log: "A".."E", or "TC1".."TC6". */
const char *intel_port_name(struct i915_device *i915, int port);

/* GGTT scanout mapping (i915_gtt.c): bind an object's pages at a fresh
 * GGTT offset (256 KB aligned, uncached) and return it; unbind. */
int i915_ggtt_bind_scanout(struct i915_device *i915, struct drm_gem_object *o,
			   uint32_t *ggtt_offset);
/* The same with the cache attribute chosen (0 = through the LLC, for
 * rings and context images the command streamer reads). */
int i915_ggtt_bind_obj(struct i915_device *i915, struct drm_gem_object *o,
		       int uncached, uint32_t *ggtt_offset);
void i915_ggtt_unbind(struct i915_device *i915, uint32_t ggtt_offset,
		      uint32_t size);

#endif
