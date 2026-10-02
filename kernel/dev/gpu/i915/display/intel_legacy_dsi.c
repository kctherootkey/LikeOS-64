// LikeOS -- MIPI DSI panels of Valleyview and Cherryview.
//
// The Bay Trail and Cherry/Braswell tablets drive their built-in panel over
// MIPI DSI: a serial link of one to four data lanes plus a clock lane, fed
// by a dedicated DSI PLL in the clock unit (reached over the IOSF sideband)
// rather than by a pipe's DPLL.  DSI port A takes pipe A and port C pipe B;
// a large panel can use both ports as one dual link.  The controller of
// each port packetises the pipe's pixels in video mode and also carries
// the commands the panel needs (DCS and generic packets, sent in low power
// or high speed mode).  There is no detection on the link: the VBT says
// whether a panel is there, gives its link parameters (block 52) and the
// sequences that power it up and down (block 53) -- lists of packets to
// send, delays, and SoC GPIO pads to toggle in the GPIO communities.  This
// file parses those blocks, computes the DSI PLL and D-PHY timing, runs
// the sequences and walks the port through its enable and disable steps.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- MIPI registers (display-relative) -------------------------------------- */

#define DSI_PORT_A 0
#define DSI_PORT_C 2
#define DSI_PORT_SEL(port, a, c) ((port) == DSI_PORT_A ? (uint32_t)(a) : (uint32_t)(c))

/* The port control register: A in the 0x61xxx block, C apart from it. */
#define MIPI_PORT_CTRL(port) DSI_PORT_SEL(port, 0x61190u, 0x61700u)
#define MIPI_DPI_ENABLE (1u << 31)
#define MIPI_DUAL_LINK_MODE_SHIFT 26
#define MIPI_DUAL_LINK_MODE_MASK (1u << 26)
#define MIPI_DITHERING_ENABLE (1u << 25)
#define MIPI_AFE_LATCHOUT (1u << 17)
#define MIPI_LP_OUTPUT_HOLD (1u << 16)
#define MIPI_LANE_CONFIGURATION_MASK (3u << 0)
#define MIPI_LANE_CONFIGURATION_4LANE (0u << 0)
#define MIPI_LANE_CONFIGURATION_DUAL_LINK_A (1u << 0)
#define MIPI_LANE_CONFIGURATION_DUAL_LINK_B (2u << 0)

/* The DSI controller and its D-PHY, one block per port. */
#define MIPI_REG(port, a) ((uint32_t)(a) + ((port) == DSI_PORT_A ? 0u : 0x800u))
#define MIPI_DEVICE_READY(port) MIPI_REG(port, 0xb000u)
#define MIPI_BUS_POSSESSION (1u << 3)
#define MIPI_ULPS_STATE_MASK (3u << 1)
#define MIPI_ULPS_STATE_ENTER (2u << 1)
#define MIPI_ULPS_STATE_EXIT (1u << 1)
#define MIPI_ULPS_STATE_NORMAL_OPERATION (0u << 1)
#define MIPI_DEV_READY (1u << 0)
#define MIPI_INTR_STAT(port) MIPI_REG(port, 0xb004u)
#define MIPI_INTR_EN(port) MIPI_REG(port, 0xb008u)
#define MIPI_SPL_PKT_SENT_INTERRUPT (1u << 30)
#define MIPI_GEN_READ_DATA_AVAIL (1u << 29)
#define MIPI_DSI_FUNC_PRG(port) MIPI_REG(port, 0xb00cu)
#define MIPI_CMD_MODE_DATA_WIDTH_MASK (7u << 13)
#define MIPI_CMD_MODE_DATA_WIDTH_8_BIT (3u << 13)
#define MIPI_VID_MODE_FORMAT_MASK (0xfu << 7)
#define MIPI_VID_MODE_FORMAT_RGB565 (1u << 7)
#define MIPI_VID_MODE_FORMAT_RGB666_PACKED (2u << 7)
#define MIPI_VID_MODE_FORMAT_RGB666 (3u << 7)
#define MIPI_VID_MODE_FORMAT_RGB888 (4u << 7)
#define MIPI_CMD_MODE_CHANNEL_NUMBER_SHIFT 5
#define MIPI_VID_MODE_CHANNEL_NUMBER_SHIFT 3
#define MIPI_DATA_LANES_PRG_REG_SHIFT 0
#define MIPI_HS_TX_TIMEOUT(port) MIPI_REG(port, 0xb010u)
#define MIPI_LP_RX_TIMEOUT(port) MIPI_REG(port, 0xb014u)
#define MIPI_TURN_AROUND_TIMEOUT(port) MIPI_REG(port, 0xb018u)
#define MIPI_DEVICE_RESET_TIMER(port) MIPI_REG(port, 0xb01cu)
#define MIPI_DPI_RESOLUTION(port) MIPI_REG(port, 0xb020u)
#define MIPI_VERTICAL_ADDRESS_SHIFT 16
#define MIPI_HORIZONTAL_ADDRESS_SHIFT 0
#define MIPI_HSYNC_PADDING_COUNT(port) MIPI_REG(port, 0xb028u)
#define MIPI_HBP_COUNT(port) MIPI_REG(port, 0xb02cu)
#define MIPI_HFP_COUNT(port) MIPI_REG(port, 0xb030u)
#define MIPI_HACTIVE_AREA_COUNT(port) MIPI_REG(port, 0xb034u)
#define MIPI_VSYNC_PADDING_COUNT(port) MIPI_REG(port, 0xb038u)
#define MIPI_VBP_COUNT(port) MIPI_REG(port, 0xb03cu)
#define MIPI_VFP_COUNT(port) MIPI_REG(port, 0xb040u)
#define MIPI_HIGH_LOW_SWITCH_COUNT(port) MIPI_REG(port, 0xb044u)
#define MIPI_DPI_CONTROL(port) MIPI_REG(port, 0xb048u)
#define MIPI_DPI_LP_MODE (1u << 6)
#define MIPI_DPI_BACKLIGHT_OFF (1u << 5)
#define MIPI_DPI_BACKLIGHT_ON (1u << 4)
#define MIPI_DPI_COLOR_MODE_OFF (1u << 3)
#define MIPI_DPI_COLOR_MODE_ON (1u << 2)
#define MIPI_DPI_TURN_ON (1u << 1)
#define MIPI_DPI_SHUTDOWN (1u << 0)
#define MIPI_INIT_COUNT(port) MIPI_REG(port, 0xb050u)
#define MIPI_MAX_RETURN_PKT_SIZE(port) MIPI_REG(port, 0xb054u)
#define MIPI_VIDEO_MODE_FORMAT(port) MIPI_REG(port, 0xb058u)
#define MIPI_RANDOM_DPI_DISPLAY_RESOLUTION (1u << 4)
#define MIPI_DISABLE_VIDEO_BTA (1u << 3)
#define MIPI_IP_TG_CONFIG (1u << 2)
#define MIPI_VIDEO_MODE_NON_BURST_WITH_SYNC_PULSE (1u << 0)
#define MIPI_VIDEO_MODE_NON_BURST_WITH_SYNC_EVENTS (2u << 0)
#define MIPI_VIDEO_MODE_BURST (3u << 0)
#define MIPI_EOT_DISABLE_REG(port) MIPI_REG(port, 0xb05cu)
#define MIPI_CLOCKSTOP (1u << 1)
#define MIPI_EOT_DISABLE (1u << 0)
#define MIPI_LP_BYTECLK(port) MIPI_REG(port, 0xb060u)
#define MIPI_LP_GEN_DATA(port) MIPI_REG(port, 0xb064u)
#define MIPI_HS_GEN_DATA(port) MIPI_REG(port, 0xb068u)
#define MIPI_LP_GEN_CTRL(port) MIPI_REG(port, 0xb06cu)
#define MIPI_HS_GEN_CTRL(port) MIPI_REG(port, 0xb070u)
#define MIPI_GEN_FIFO_STAT(port) MIPI_REG(port, 0xb074u)
#define MIPI_DPI_FIFO_EMPTY (1u << 28)
#define MIPI_DBI_FIFO_EMPTY (1u << 27)
#define MIPI_LP_CTRL_FIFO_EMPTY (1u << 26)
#define MIPI_LP_CTRL_FIFO_FULL (1u << 24)
#define MIPI_HS_CTRL_FIFO_EMPTY (1u << 18)
#define MIPI_HS_CTRL_FIFO_FULL (1u << 16)
#define MIPI_LP_DATA_FIFO_EMPTY (1u << 10)
#define MIPI_LP_DATA_FIFO_FULL (1u << 8)
#define MIPI_HS_DATA_FIFO_EMPTY (1u << 2)
#define MIPI_HS_DATA_FIFO_FULL (1u << 0)
#define MIPI_DPHY_PARAM(port) MIPI_REG(port, 0xb080u)
#define MIPI_DBI_BW_CTRL(port) MIPI_REG(port, 0xb084u)
#define MIPI_CLK_LANE_SWITCH_TIME_CNT(port) MIPI_REG(port, 0xb088u)
#define MIPI_LP_HS_SSW_CNT_SHIFT 16
#define MIPI_HS_LP_PWR_SW_CNT_SHIFT 0
#define MIPI_CTRL(port) MIPI_REG(port, 0xb104u)
#define MIPI_ESCAPE_CLOCK_DIVIDER_SHIFT 5 /* port A only */
#define MIPI_ESCAPE_CLOCK_DIVIDER_MASK (3u << 5)
#define MIPI_ESCAPE_CLOCK_DIVIDER_1 (0u << 5)
#define MIPI_ESCAPE_CLOCK_DIVIDER_2 (1u << 5)
#define MIPI_ESCAPE_CLOCK_DIVIDER_4 (2u << 5)
#define MIPI_READ_REQUEST_PRIORITY_MASK (3u << 3)
#define MIPI_READ_REQUEST_PRIORITY_HIGH (3u << 3)

/* The pixel overlap of a front-back dual link (display-relative). */
#define VLV_DSI_CHICKEN_3 0x7040cu
#define VLV_DSI_PIXEL_OVERLAP_CNT_MASK (3u << 30)
#define VLV_DSI_PIXEL_OVERLAP_CNT_SHIFT 30

/* ---- the DSI PLL in the clock unit (CCK, over the sideband) ------------------ */

#define CCK_DSI_PLL_CONTROL 0x48u
#define CCK_DSI_PLL_VCO_EN (1u << 31)
#define CCK_DSI_PLL_LDO_GATE (1u << 30)
#define CCK_DSI_PLL_P1_POST_DIV_SHIFT 17
#define CCK_DSI_PLL_P1_POST_DIV_MASK (0x1ffu << 17)
#define CCK_DSI_PLL_CLK_GATE_DSI0_DSIPLL (1u << 8)
#define CCK_DSI_PLL_CLK_GATE_DSI1_DSIPLL (1u << 7)
#define CCK_DSI_PLL_LOCK (1u << 0)
#define CCK_DSI_PLL_DIVIDER 0x4cu
#define CCK_DSI_PLL_N1_DIV_SHIFT 16
#define CCK_DSI_PLL_N1_DIV_MASK (3u << 16)
#define CCK_DSI_PLL_M1_DIV_SHIFT 0
#define CCK_DSI_PLL_M1_DIV_MASK (0x1ffu << 0)

/* ---- the SoC GPIO communities (IOSF ports) ----------------------------------- */

#define DSI_IOSF_PORT_GPIO_NC 0x13 /* Valleyview: north core */
#define DSI_IOSF_PORT_CHV_GPIO_N 0x13
#define DSI_IOSF_PORT_CHV_GPIO_SE 0x48
#define DSI_IOSF_PORT_CHV_GPIO_E 0xa8
#define DSI_IOSF_PORT_CHV_GPIO_SW 0xb2

/* Valleyview: each pad has a PCONF0 register and, 8 bytes on, its value. */
#define VLV_DSI_GPIO_PCONF0(base) (base)
#define VLV_DSI_GPIO_PAD_VAL(base) ((base) + 8u)
#define VLV_DSI_GPIO_NC_PADS 28

/* Cherryview: the VBT numbers the pads across the four communities. */
#define CHV_DSI_GPIO_IDX_START_N 0
#define CHV_DSI_GPIO_IDX_START_E 73
#define CHV_DSI_GPIO_IDX_START_SW 100
#define CHV_DSI_GPIO_IDX_START_SE 198
#define CHV_DSI_VBT_MAX_PINS_PER_FMLY 15
#define CHV_DSI_GPIO_PAD_CFG0(f, i) (0x4400u + (uint32_t)(f) * 0x400u + (uint32_t)(i) * 8u)
#define CHV_DSI_GPIO_GPIOEN (1u << 15)
#define CHV_DSI_GPIO_GPIOCFG_GPO (1u << 8)
#define CHV_DSI_GPIO_GPIOTXSTATE(state) ((uint32_t)(!!(state)) << 1)
#define CHV_DSI_GPIO_PAD_CFG1(f, i) (0x4400u + (uint32_t)(f) * 0x400u + (uint32_t)(i) * 8u + 4u)

/* ---- MIPI DSI packets and DCS commands ---------------------------------------- */

#define MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM 0x03
#define MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM 0x13
#define MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM 0x23
#define MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM 0x04
#define MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM 0x14
#define MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM 0x24
#define MIPI_DSI_DCS_SHORT_WRITE 0x05
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM 0x15
#define MIPI_DSI_DCS_READ 0x06
#define MIPI_DSI_GENERIC_LONG_WRITE 0x29
#define MIPI_DSI_DCS_LONG_WRITE 0x39

#define MIPI_DCS_SET_DISPLAY_BRIGHTNESS 0x51
#define MIPI_DCS_WRITE_CONTROL_DISPLAY 0x53
#define MIPI_DCS_GET_CONTROL_DISPLAY 0x54
#define MIPI_DCS_WRITE_POWER_SAVE 0x55

/* WRITE_CONTROL_DISPLAY bits and the CABC power save levels */
#define DCS_CONTROL_DISPLAY_BCTRL (1u << 5)
#define DCS_CONTROL_DISPLAY_DD (1u << 3)
#define DCS_CONTROL_DISPLAY_BL (1u << 2)
#define DCS_POWER_SAVE_OFF 0
#define DCS_POWER_SAVE_MEDIUM 2
#define DCS_PANEL_PWM_MAX_VALUE 0xff

/* ---- the VBT's MIPI blocks ---------------------------------------------------- */

#define VBT_BLOCK_LFP_BACKLIGHT 43
#define VBT_BLOCK_MIPI_CONFIG 52
#define VBT_BLOCK_MIPI_SEQUENCE 53
#define VBT_MAX_MIPI_CONFIGURATIONS 6

/* The sequences of block 53.  The VBT spec swaps the names of the two
 * reset sequences; these are the usual names. */
enum dsi_seq {
	MIPI_SEQ_END = 0,
	MIPI_SEQ_DEASSERT_RESET,
	MIPI_SEQ_INIT_OTP,
	MIPI_SEQ_DISPLAY_ON,
	MIPI_SEQ_DISPLAY_OFF,
	MIPI_SEQ_ASSERT_RESET,
	MIPI_SEQ_BACKLIGHT_ON, /* sequence block v2+ */
	MIPI_SEQ_BACKLIGHT_OFF, /* sequence block v2+ */
	MIPI_SEQ_TEAR_ON, /* sequence block v2+ */
	MIPI_SEQ_TEAR_OFF, /* sequence block v3+ */
	MIPI_SEQ_POWER_ON, /* sequence block v3+ */
	MIPI_SEQ_POWER_OFF, /* sequence block v3+ */
	MIPI_SEQ_MAX
};

enum dsi_seq_element {
	MIPI_SEQ_ELEM_END = 0,
	MIPI_SEQ_ELEM_SEND_PKT,
	MIPI_SEQ_ELEM_DELAY,
	MIPI_SEQ_ELEM_GPIO,
	MIPI_SEQ_ELEM_I2C, /* sequence block v2+ */
	MIPI_SEQ_ELEM_SPI, /* sequence block v3+ */
	MIPI_SEQ_ELEM_PMIC, /* sequence block v3+ */
	MIPI_SEQ_ELEM_MAX
};

/* The send-packet element's flags byte */
#define MIPI_TRANSFER_MODE_SHIFT 0
#define MIPI_VIRTUAL_CHANNEL_SHIFT 1
#define MIPI_PORT_SHIFT 3

#define VBT_NON_BURST_SYNC_PULSE 0x1
#define VBT_NON_BURST_SYNC_EVENTS 0x2
#define VBT_BURST_MODE 0x3

#define VBT_PPS_BLC_PMIC 0
#define VBT_PPS_BLC_SOC 1

#define VBT_PIXEL_FORMAT_RGB565 0x1
#define VBT_PIXEL_FORMAT_RGB666 0x2
#define VBT_PIXEL_FORMAT_RGB666_LOOSELY_PACKED 0x3
#define VBT_PIXEL_FORMAT_RGB888 0x4

#define VBT_DL_DCS_PORT_A 0x00
#define VBT_DL_DCS_PORT_C 0x01
#define VBT_DL_DCS_PORT_A_AND_C 0x02

/* The backlight control method of block 43 (VBT 191+). */
#define VBT_BACKLIGHT_PMIC 0
#define VBT_BACKLIGHT_LPSS 1
#define VBT_BACKLIGHT_DISPLAY_DDI 2
#define VBT_BACKLIGHT_DSI_DCS 3
#define VBT_BACKLIGHT_TYPE_PWM 2 /* the entry's type */

/* One panel's configuration in block 52, as the VBT lays it out. */
struct vbt_mipi_config {
	uint16_t panel_id;

	/* general parameters */
	struct {
		uint32_t enable_dithering : 1;
		uint32_t rsvd1 : 1;
		uint32_t is_bridge : 1;
		uint32_t panel_arch_type : 2;
		uint32_t is_cmd_mode : 1;
		uint32_t video_transfer_mode : 2;
		uint32_t cabc_supported : 1;
		uint32_t pwm_blc : 1;
		uint32_t videomode_color_format : 4;
		uint32_t rotation : 2;
		uint32_t bta_disable : 1;
		uint32_t rsvd2 : 15;
	} __attribute__((packed));

	/* port description */
	struct {
		uint16_t dual_link : 2;
		uint16_t lane_cnt : 2;
		uint16_t pixel_overlap : 3;
		uint16_t rgb_flip : 1;
		uint16_t dl_dcs_cabc_ports : 2;
		uint16_t dl_dcs_backlight_ports : 2;
		uint16_t port_sync : 1;
		uint16_t rsvd3 : 3;
	} __attribute__((packed));

	/* DSI controller parameters */
	struct {
		uint16_t dsi_usage : 1;
		uint16_t rsvd4 : 15;
	} __attribute__((packed));

	uint8_t rsvd5;
	uint32_t target_burst_mode_freq;
	uint32_t dsi_ddr_clk;
	uint32_t bridge_ref_clk;

	/* LP byte clock */
	struct {
		uint8_t byte_clk_sel : 2;
		uint8_t rsvd6 : 6;
	} __attribute__((packed));

	/* D-PHY flags */
	struct {
		uint16_t dphy_param_valid : 1;
		uint16_t eot_pkt_disabled : 1;
		uint16_t enable_clk_stop : 1;
		uint16_t blanking_packets_during_bllp : 1;
		uint16_t lp_clock_during_lpm : 1;
		uint16_t rsvd7 : 11;
	} __attribute__((packed));

	uint32_t hs_tx_timeout;
	uint32_t lp_rx_timeout;
	uint32_t turn_around_timeout;
	uint32_t device_reset_timer;
	uint32_t master_init_timer;
	uint32_t dbi_bw_timer;
	uint32_t lp_byte_clk_val;

	/* D-PHY parameters */
	struct {
		uint32_t prepare_cnt : 6;
		uint32_t rsvd8 : 2;
		uint32_t clk_zero_cnt : 8;
		uint32_t trail_cnt : 5;
		uint32_t rsvd9 : 3;
		uint32_t exit_zero_cnt : 6;
		uint32_t rsvd10 : 2;
	} __attribute__((packed));

	uint32_t clk_lane_switch_cnt;
	uint32_t hl_switch_cnt;

	uint32_t rsvd11[6];

	/* timings from the D-PHY spec */
	uint8_t tclk_miss;
	uint8_t tclk_post;
	uint8_t rsvd12;
	uint8_t tclk_pre;
	uint8_t tclk_prepare;
	uint8_t tclk_settle;
	uint8_t tclk_term_enable;
	uint8_t tclk_trail;
	uint16_t tclk_prepare_clkzero;
	uint8_t rsvd13;
	uint8_t td_term_enable;
	uint8_t teot;
	uint8_t ths_exit;
	uint8_t ths_prepare;
	uint16_t ths_prepare_hszero;
	uint8_t rsvd14;
	uint8_t ths_settle;
	uint8_t ths_skip;
	uint8_t ths_trail;
	uint8_t tinit;
	uint8_t tlpx;
	uint8_t rsvd15[3];

	/* GPIOs */
	uint8_t panel_enable;
	uint8_t bl_enable;
	uint8_t pwm_enable;
	uint8_t reset_r_n;
	uint8_t pwr_down_r;
	uint8_t stdby_r_n;
} __attribute__((packed));

/* The panel's power sequencing delays, in units of 100 us. */
struct vbt_mipi_pps {
	uint16_t panel_on_delay;
	uint16_t bl_enable_delay;
	uint16_t bl_disable_delay;
	uint16_t panel_off_delay;
	uint16_t panel_power_cycle_delay;
} __attribute__((packed));

_Static_assert(sizeof(struct vbt_mipi_config) == 122, "MIPI config block layout");
_Static_assert(sizeof(struct vbt_mipi_pps) == 10, "MIPI PPS data layout");

/* ---- per-panel state ---------------------------------------------------------- */

enum dsi_pixel_format {
	DSI_FMT_RGB888,
	DSI_FMT_RGB666, /* loosely packed: 24 bits a pixel on the link */
	DSI_FMT_RGB666_PACKED,
	DSI_FMT_RGB565,
};

#define DSI_VIDEO_MODE 0
#define DSI_COMMAND_MODE 1

#define DSI_DUAL_LINK_NONE 0
#define DSI_DUAL_LINK_FRONT_BACK 1
#define DSI_DUAL_LINK_PIXEL_ALT 2

enum dsi_backlight {
	DSI_BL_NONE, /* the PMIC or LPSS PWM, out of our reach */
	DSI_BL_DISPLAY_PWM, /* the display controller's PWM */
	DSI_BL_DCS, /* DCS brightness commands over the link */
};

/* What a packet goes out with on one port's link. */
struct dsi_link_dev {
	int lpm; /* low power mode */
	int channel; /* virtual channel */
};

/* One message for the DSI host. */
struct dsi_msg {
	uint8_t channel;
	uint8_t type;
	int lpm;
	const uint8_t *tx_buf;
	uint32_t tx_len;
	uint8_t *rx_buf;
	uint32_t rx_len;
};

struct lg_dsi {
	int port; /* the port the VBT names */
	int pipe; /* A for port A, B for port C */
	uint16_t ports; /* the ports driven: bit 0 A, bit 2 C */
	struct dsi_link_dev dev[4];

	struct vbt_mipi_config mipi;
	struct vbt_mipi_pps pps;
	uint16_t bl_ports, cabc_ports;

	/* block 53, the panel's sequences */
	int seq_version;
	uint8_t *seq_data;
	uint32_t seq_size;
	uint8_t *deassert_seq;
	const uint8_t *sequence[MIPI_SEQ_MAX];

	/* the link */
	int channel;
	int operation_mode;
	unsigned int lane_count;
	int i2c_bus_num;
	enum dsi_pixel_format pixel_format;
	int video_mode;
	int lp_clock_during_lpm;
	int blanking_pkt;
	int eot_pkt;
	int clock_stop;
	uint8_t escape_clk_div;
	uint8_t dual_link;
	int bgr_enabled;
	uint8_t pixel_overlap;
	uint32_t bw_timer;
	uint32_t dphy_reg;
	uint32_t video_frmt_cfg_bits;
	uint16_t lp_byte_clk;
	uint16_t hs_tx_timeout;
	uint16_t lp_rx_timeout;
	uint16_t turn_arnd_val;
	uint16_t rst_timer_val;
	uint16_t hs_to_lp_count;
	uint16_t clk_lp_to_hs_count;
	uint16_t clk_hs_to_lp_count;
	uint16_t init_count;
	uint32_t pclk;
	uint16_t burst_mode_ratio;

	/* delays, ms */
	uint16_t backlight_off_delay;
	uint16_t backlight_on_delay;
	uint16_t panel_on_delay;
	uint16_t panel_off_delay;
	uint16_t panel_pwr_cycle_delay;

	/* the DSI PLL's CCK values */
	uint32_t pll_ctrl, pll_div;

	/* Panel and backlight enable lines the driver drives itself (some
	 * Valleyview boards whose sequences leave them out): a north core
	 * GPIO pad index, -1 none. */
	int gpio_panel, gpio_backlight;
	uint8_t vlv_gpio_init[VLV_DSI_GPIO_NC_PADS];

	/* backlight */
	enum dsi_backlight bl;
	int bl_precision_bits;
	uint32_t bl_max, bl_level;
};

/* ---- small helpers ------------------------------------------------------------ */

#define DSI_FOR_EACH_PORT(port, mask)                                           \
	for ((port) = 0; (port) < 4; (port)++)                                 \
		if ((mask) & (1u << (port)))

static inline uint32_t dsi_min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t dsi_max_u32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

static inline int dsi_abs(int v)
{
	return v < 0 ? -v : v;
}

static int dsi_hweight16(uint16_t v)
{
	int n = 0;

	while (v) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

static int dsi_ffs(uint32_t v)
{
	int i;

	for (i = 0; i < 32; i++)
		if (v & (1u << i))
			return i + 1;
	return 0;
}

static int dsi_fls(uint32_t v)
{
	int i;

	for (i = 31; i >= 0; i--)
		if (v & (1u << i))
			return i + 1;
	return 0;
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static char dsi_port_name(int port)
{
	return (char)('A' + port);
}

static void dsi_sleep_ms(uint32_t ms)
{
	if (ms)
		lg_mdelay(ms);
}

static void dsi_sleep_us(uint32_t us)
{
	if (us >= 1000)
		lg_mdelay(us / 1000);
	if (us % 1000)
		lg_udelay(us % 1000);
}

static int dsi_wait_set(struct lg_display *d, uint32_t reg, uint32_t mask,
			uint32_t timeout_ms)
{
	return lg_wait(d, reg, mask, mask, timeout_ms * 1000u);
}

static int dsi_wait_clear(struct lg_display *d, uint32_t reg, uint32_t mask,
			  uint32_t timeout_ms)
{
	return lg_wait(d, reg, mask, 0, timeout_ms * 1000u);
}

static int dsi_is_vid_mode(const struct lg_dsi *dsi)
{
	return dsi->operation_mode == DSI_VIDEO_MODE;
}

static int dsi_is_cmd_mode(const struct lg_dsi *dsi)
{
	return dsi->operation_mode == DSI_COMMAND_MODE;
}

/* Bits a pixel takes on the link. */
static int dsi_pixel_format_to_bpp(enum dsi_pixel_format fmt)
{
	switch (fmt) {
	case DSI_FMT_RGB888:
	case DSI_FMT_RGB666:
		return 24;
	case DSI_FMT_RGB666_PACKED:
		return 18;
	case DSI_FMT_RGB565:
		return 16;
	}
	return 16;
}

static int dsi_bitrate(const struct lg_dsi *dsi)
{
	int bpp = dsi_pixel_format_to_bpp(dsi->pixel_format);

	return (int)(dsi->pclk * (uint32_t)bpp / dsi->lane_count);
}

static int dsi_tlpx_ns(const struct lg_dsi *dsi)
{
	switch (dsi->escape_clk_div) {
	default:
	case 0:
		return 50;
	case 1:
		return 100;
	case 2:
		return 200;
	}
}

/* Within 5% of each other. */
static int dsi_fuzzy_clock_check(int clock1, int clock2)
{
	int diff;

	if (clock1 == clock2)
		return 1;
	if (!clock1 || !clock2)
		return 0;
	diff = dsi_abs(clock1 - clock2);
	return ((diff + clock1 + clock2) * 100) / (clock1 + clock2) < 105;
}

/* ---- the DSI host: packets out through the generic FIFOs ---------------------- */

static void dsi_wait_for_fifo_empty(struct lg_display *d, int port)
{
	uint32_t mask = MIPI_LP_CTRL_FIFO_EMPTY | MIPI_HS_CTRL_FIFO_EMPTY |
			MIPI_LP_DATA_FIFO_EMPTY | MIPI_HS_DATA_FIFO_EMPTY;

	if (dsi_wait_set(d, MIPI_GEN_FIFO_STAT(port), mask, 100))
		kprintf("[drm] i915: DSI: DPI FIFOs are not empty\n");
}

static void dsi_write_data(struct lg_display *d, uint32_t reg, const uint8_t *data,
			   uint32_t len)
{
	uint32_t i, j;

	for (i = 0; i < len; i += 4) {
		uint32_t val = 0;

		for (j = 0; j < dsi_min_u32(len - i, 4); j++)
			val |= (uint32_t)*data++ << (8 * j);
		lg_wr(d, reg, val);
	}
}

static void dsi_read_data(struct lg_display *d, uint32_t reg, uint8_t *data, uint32_t len)
{
	uint32_t i, j;

	for (i = 0; i < len; i += 4) {
		uint32_t val = lg_rd(d, reg);

		for (j = 0; j < dsi_min_u32(len - i, 4); j++)
			*data++ = (uint8_t)(val >> (8 * j));
	}
}

static int dsi_packet_is_long(uint8_t type)
{
	switch (type) {
	case 0x09: /* null packet */
	case 0x19: /* blanking packet */
	case MIPI_DSI_GENERIC_LONG_WRITE:
	case MIPI_DSI_DCS_LONG_WRITE:
	case 0x0a: /* PPS long write */
	case 0x0c: case 0x1c: case 0x2c: case 0x0d: case 0x1d: case 0x3d:
	case 0x0e: case 0x1e: case 0x2e: case 0x3e: /* pixel streams */
		return 1;
	}
	return 0;
}

static int dsi_packet_is_short(uint8_t type)
{
	switch (type) {
	case 0x01: case 0x11: case 0x21: case 0x31: /* sync events */
	case 0x07: case 0x08: /* compression mode, end of transmission */
	case 0x02: case 0x12: case 0x22: case 0x32: /* color mode, peripheral */
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_DCS_READ:
	case 0x16: /* execute queue */
	case 0x37: /* set maximum return packet size */
		return 1;
	}
	return 0;
}

/*
 * Send one packet.  The header (data identifier with the virtual channel,
 * then two bytes: the word count of a long packet or the parameters of a
 * short one) goes into the control FIFO; a long packet's payload is pushed
 * into the data FIFO first, four bytes a write.  Low power packets use the
 * LP FIFOs, high speed ones the HS FIFOs.  A read waits for the panel's
 * answer and takes it from the same data register.
 */
static int dsi_host_transfer(struct lg_display *d, int port, const struct dsi_msg *msg)
{
	uint8_t header[3];
	const uint8_t *payload = NULL;
	uint32_t payload_length = 0;
	uint32_t data_reg, ctrl_reg, data_mask, ctrl_mask;

	if (!dsi_packet_is_short(msg->type) && !dsi_packet_is_long(msg->type))
		return -EINVAL;
	if (msg->channel > 3)
		return -EINVAL;

	header[0] = (uint8_t)(((msg->channel & 0x3) << 6) | (msg->type & 0x3f));
	if (dsi_packet_is_long(msg->type)) {
		header[1] = (uint8_t)(msg->tx_len & 0xff);
		header[2] = (uint8_t)((msg->tx_len >> 8) & 0xff);
		payload_length = msg->tx_len;
		payload = msg->tx_buf;
	} else {
		header[1] = msg->tx_len > 0 ? msg->tx_buf[0] : 0;
		header[2] = msg->tx_len > 1 ? msg->tx_buf[1] : 0;
	}

	if (msg->lpm) {
		data_reg = MIPI_LP_GEN_DATA(port);
		data_mask = MIPI_LP_DATA_FIFO_FULL;
		ctrl_reg = MIPI_LP_GEN_CTRL(port);
		ctrl_mask = MIPI_LP_CTRL_FIFO_FULL;
	} else {
		data_reg = MIPI_HS_GEN_DATA(port);
		data_mask = MIPI_HS_DATA_FIFO_FULL;
		ctrl_reg = MIPI_HS_GEN_CTRL(port);
		ctrl_mask = MIPI_HS_CTRL_FIFO_FULL;
	}

	/* never true for reads */
	if (payload_length) {
		if (dsi_wait_clear(d, MIPI_GEN_FIFO_STAT(port), data_mask, 50))
			kprintf("[drm] i915: DSI: timeout waiting for HS/LP DATA FIFO !full\n");
		dsi_write_data(d, data_reg, payload, payload_length);
	}

	if (msg->rx_len)
		lg_wr(d, MIPI_INTR_STAT(port), MIPI_GEN_READ_DATA_AVAIL);

	if (dsi_wait_clear(d, MIPI_GEN_FIFO_STAT(port), ctrl_mask, 50))
		kprintf("[drm] i915: DSI: timeout waiting for HS/LP CTRL FIFO !full\n");

	lg_wr(d, ctrl_reg, (uint32_t)header[2] << 16 | (uint32_t)header[1] << 8 | header[0]);

	if (msg->rx_len) {
		if (dsi_wait_set(d, MIPI_INTR_STAT(port), MIPI_GEN_READ_DATA_AVAIL, 50))
			kprintf("[drm] i915: DSI: timeout waiting for read data\n");
		dsi_read_data(d, data_reg, msg->rx_buf, msg->rx_len);
	}

	return (int)(4 + payload_length);
}

/* A generic write: the packet type follows from the payload size. */
static int dsi_generic_write(struct lg_display *d, struct lg_dsi *dsi, int port,
			     const uint8_t *payload, uint32_t size)
{
	struct dsi_msg msg;

	mm_memset(&msg, 0, sizeof(msg));
	msg.channel = (uint8_t)dsi->dev[port].channel;
	msg.lpm = dsi->dev[port].lpm;
	msg.tx_buf = payload;
	msg.tx_len = size;

	switch (size) {
	case 0:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM;
		break;
	case 1:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM;
		break;
	case 2:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM;
		break;
	default:
		msg.type = MIPI_DSI_GENERIC_LONG_WRITE;
		break;
	}

	return dsi_host_transfer(d, port, &msg);
}

/* A DCS write of a ready buffer (command byte first). */
static int dsi_dcs_write_buffer(struct lg_display *d, struct lg_dsi *dsi, int port,
				const uint8_t *data, uint32_t len)
{
	struct dsi_msg msg;

	if (len == 0)
		return -EINVAL;

	mm_memset(&msg, 0, sizeof(msg));
	msg.channel = (uint8_t)dsi->dev[port].channel;
	msg.lpm = dsi->dev[port].lpm;
	msg.tx_buf = data;
	msg.tx_len = len;

	switch (len) {
	case 1:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE;
		break;
	case 2:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;
	default:
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	return dsi_host_transfer(d, port, &msg);
}

static int dsi_dcs_write(struct lg_display *d, struct lg_dsi *dsi, int port, uint8_t cmd,
			 const uint8_t *data, uint32_t len)
{
	uint8_t buf[8];

	if (len + 1 > sizeof(buf))
		return -EINVAL;
	buf[0] = cmd;
	if (len)
		mm_memcpy(&buf[1], data, len);
	return dsi_dcs_write_buffer(d, dsi, port, buf, len + 1);
}

static int dsi_dcs_read(struct lg_display *d, struct lg_dsi *dsi, int port, uint8_t cmd,
			uint8_t *data, uint32_t len)
{
	struct dsi_msg msg;

	mm_memset(&msg, 0, sizeof(msg));
	msg.channel = (uint8_t)dsi->dev[port].channel;
	msg.lpm = dsi->dev[port].lpm;
	msg.type = MIPI_DSI_DCS_READ;
	msg.tx_buf = &cmd;
	msg.tx_len = 1;
	msg.rx_buf = data;
	msg.rx_len = len;

	return dsi_host_transfer(d, port, &msg);
}

/*
 * A video mode special packet (turn on, shut down, ...) through the DPI
 * control register; the controller sends it in the next blanking period
 * and flags SPL_PKT_SENT.
 */
static void dsi_dpi_send_cmd(struct lg_display *d, uint32_t cmd, int hs, int port)
{
	if (hs)
		cmd &= ~MIPI_DPI_LP_MODE;
	else
		cmd |= MIPI_DPI_LP_MODE;

	/* clear the sent bit */
	lg_wr(d, MIPI_INTR_STAT(port), MIPI_SPL_PKT_SENT_INTERRUPT);

	if (cmd == lg_rd(d, MIPI_DPI_CONTROL(port)))
		i915_dbg("[drm] i915: DSI: same special packet %02x twice in a row\n", cmd);

	lg_wr(d, MIPI_DPI_CONTROL(port), cmd);

	if (dsi_wait_set(d, MIPI_INTR_STAT(port), MIPI_SPL_PKT_SENT_INTERRUPT, 100))
		kprintf("[drm] i915: DSI: video mode command 0x%08x send failed\n", cmd);
}

/* ---- the VBT sequences --------------------------------------------------------- */

/*
 * With a single link the VBT's send-packet elements apparently always say
 * port 0; use the port the panel is on.  On a dual link a non-zero port
 * means port C.
 */
static int dsi_seq_port_to_port(const struct lg_dsi *dsi, uint8_t seq_port)
{
	if (dsi_hweight16(dsi->ports) == 1)
		return dsi_ffs(dsi->ports) - 1;

	if (seq_port && (dsi->ports & (1u << DSI_PORT_C)))
		return DSI_PORT_C;

	return DSI_PORT_A;
}

static const uint8_t *dsi_exec_send_packet(struct lg_display *d, struct lg_dsi *dsi,
					   const uint8_t *data)
{
	uint8_t type, flags, seq_port;
	uint16_t len;
	int port, hs_mode, ret;

	flags = *data++;
	type = *data++;
	len = get_le16(data);
	data += 2;

	seq_port = (flags >> MIPI_PORT_SHIFT) & 3;
	port = dsi_seq_port_to_port(dsi, seq_port);

	if (!(dsi->ports & (1u << port))) {
		kprintf("[drm] i915: DSI: no DSI host for port %c\n", dsi_port_name(port));
		goto out;
	}

	hs_mode = (flags >> MIPI_TRANSFER_MODE_SHIFT) & 1;
	dsi->dev[port].lpm = !hs_mode;
	dsi->dev[port].channel = (flags >> MIPI_VIRTUAL_CHANNEL_SHIFT) & 3;

	i915_dbg("[drm] i915: DSI packet: port %c (seq %u), flags 0x%02x, VC %d, %s, type 0x%02x, length %u\n",
		 dsi_port_name(port), seq_port, flags, dsi->dev[port].channel,
		 hs_mode ? "HS" : "LP", type, len);

	switch (type) {
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		ret = dsi_generic_write(d, dsi, port, NULL, 0);
		break;
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		ret = dsi_generic_write(d, dsi, port, data, 1);
		break;
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		ret = dsi_generic_write(d, dsi, port, data, 2);
		break;
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
		ret = -EOPNOTSUPP;
		break;
	case MIPI_DSI_GENERIC_LONG_WRITE:
		ret = dsi_generic_write(d, dsi, port, data, len);
		break;
	case MIPI_DSI_DCS_SHORT_WRITE:
		ret = dsi_dcs_write_buffer(d, dsi, port, data, 1);
		break;
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		ret = dsi_dcs_write_buffer(d, dsi, port, data, 2);
		break;
	case MIPI_DSI_DCS_READ:
		ret = -EOPNOTSUPP;
		break;
	case MIPI_DSI_DCS_LONG_WRITE:
		ret = dsi_dcs_write_buffer(d, dsi, port, data, len);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret < 0)
		kprintf("[drm] i915: DSI: send packet type 0x%02x failed (%d)\n", type, ret);

	dsi_wait_for_fifo_empty(d, port);

out:
	data += len;
	return data;
}

static const uint8_t *dsi_exec_delay(struct lg_display *d, struct lg_dsi *dsi,
				     const uint8_t *data)
{
	uint32_t delay = get_le32(data);

	(void)d;
	(void)dsi;
	i915_dbg("[drm] i915: DSI: delay %u us\n", delay);
	dsi_sleep_us(delay);
	return data + 4;
}

/*
 * The Valleyview north core community: the VBT numbers its pads 0..27 in
 * the order of the community's pin list; each sits at 0x4000 + 16 * pad
 * in the community's sideband space.
 */
static const uint16_t vlv_gpio_nc_table[VLV_DSI_GPIO_NC_PADS] = {
	0x4130, /* 0  HV_DDI0_HPD */
	0x4120, /* 1  HV_DDI0_DDC_SDA */
	0x4110, /* 2  HV_DDI0_DDC_SCL */
	0x4140, /* 3  PANEL0_VDDEN */
	0x4150, /* 4  PANEL0_BKLTEN */
	0x4160, /* 5  PANEL0_BKLTCTL */
	0x4180, /* 6  HV_DDI1_HPD */
	0x4190, /* 7  HV_DDI1_DDC_SDA */
	0x4170, /* 8  HV_DDI1_DDC_SCL */
	0x4100, /* 9  PANEL1_VDDEN */
	0x40e0, /* 10 PANEL1_BKLTEN */
	0x40f0, /* 11 PANEL1_BKLTCTL */
	0x40c0, /* 12 */
	0x41a0, /* 13 */
	0x41b0, /* 14 */
	0x4010, /* 15 */
	0x4040, /* 16 */
	0x4080, /* 17 */
	0x40b0, /* 18 */
	0x4000, /* 19 */
	0x4030, /* 20 */
	0x4060, /* 21 */
	0x40a0, /* 22 */
	0x40d0, /* 23 */
	0x4020, /* 24 */
	0x4050, /* 25 */
	0x4090, /* 26 */
	0x4070, /* 27 */
};

/* Drive a north core pad: the first time switch it to GPIO mode, then
 * set it as an output (input buffer off) at `value'. */
static void vlv_dsi_gpio_nc_set(struct lg_display *d, struct lg_dsi *dsi,
				unsigned int index, int value)
{
	uint16_t base;

	if (index >= VLV_DSI_GPIO_NC_PADS) {
		kprintf("[drm] i915: DSI: unknown GPIO NC index %u\n", index);
		return;
	}
	base = vlv_gpio_nc_table[index];

	if (!dsi->vlv_gpio_init[index]) {
		lg_vlv_iosf_write(d, DSI_IOSF_PORT_GPIO_NC, VLV_DSI_GPIO_PCONF0(base),
				  0x2000cc00u);
		dsi->vlv_gpio_init[index] = 1;
	}

	lg_vlv_iosf_write(d, DSI_IOSF_PORT_GPIO_NC, VLV_DSI_GPIO_PAD_VAL(base),
			  0x4u | (value ? 1u : 0u));
}

static void vlv_exec_gpio(struct lg_display *d, struct lg_dsi *dsi, uint8_t gpio_source,
			  uint8_t gpio_index, int value)
{
	if (dsi->seq_version < 3) {
		if (gpio_source == 1) {
			kprintf("[drm] i915: DSI: SC community GPIO %u not supported\n",
				gpio_index);
			return;
		}
		if (gpio_source > 1) {
			kprintf("[drm] i915: DSI: unknown GPIO source %u\n", gpio_source);
			return;
		}
	}

	/* the table holds only the north core pads */
	vlv_dsi_gpio_nc_set(d, dsi, gpio_index, value);
}

static void chv_exec_gpio(struct lg_display *d, struct lg_dsi *dsi, uint8_t gpio_source,
			  uint8_t gpio_index, int value)
{
	uint32_t cfg0, cfg1;
	uint16_t family_num;
	int port;

	if (dsi->seq_version >= 3) {
		if (gpio_index >= CHV_DSI_GPIO_IDX_START_SE) {
			/* it is unclear whether 255 -> 57 is part of SE */
			gpio_index -= CHV_DSI_GPIO_IDX_START_SE;
			port = DSI_IOSF_PORT_CHV_GPIO_SE;
		} else if (gpio_index >= CHV_DSI_GPIO_IDX_START_SW) {
			gpio_index -= CHV_DSI_GPIO_IDX_START_SW;
			port = DSI_IOSF_PORT_CHV_GPIO_SW;
		} else if (gpio_index >= CHV_DSI_GPIO_IDX_START_E) {
			gpio_index -= CHV_DSI_GPIO_IDX_START_E;
			port = DSI_IOSF_PORT_CHV_GPIO_E;
		} else {
			port = DSI_IOSF_PORT_CHV_GPIO_N;
		}
	} else {
		/* the spec is unclear about Cherryview GPIOs in v2 */
		if (gpio_source != 0) {
			kprintf("[drm] i915: DSI: unknown GPIO source %u\n", gpio_source);
			return;
		}
		if (gpio_index >= CHV_DSI_GPIO_IDX_START_E) {
			kprintf("[drm] i915: DSI: invalid GPIO index %u for GPIO N\n",
				gpio_index);
			return;
		}
		port = DSI_IOSF_PORT_CHV_GPIO_N;
	}

	family_num = gpio_index / CHV_DSI_VBT_MAX_PINS_PER_FMLY;
	gpio_index = gpio_index % CHV_DSI_VBT_MAX_PINS_PER_FMLY;

	cfg0 = CHV_DSI_GPIO_PAD_CFG0(family_num, gpio_index);
	cfg1 = CHV_DSI_GPIO_PAD_CFG1(family_num, gpio_index);

	lg_vlv_iosf_write(d, port, cfg1, 0);
	lg_vlv_iosf_write(d, port, cfg0,
			  CHV_DSI_GPIO_GPIOEN | CHV_DSI_GPIO_GPIOCFG_GPO |
				  CHV_DSI_GPIO_GPIOTXSTATE(value));
}

static const uint8_t *dsi_exec_gpio(struct lg_display *d, struct lg_dsi *dsi,
				    const uint8_t *data)
{
	uint8_t gpio_source = 0, gpio_index = 0, gpio_number;
	int value, size;

	if (dsi->seq_version >= 3) {
		size = 3;
		gpio_index = data[0];
		gpio_number = data[1];
		value = data[2] & 1;
	} else {
		size = 2;
		gpio_number = data[0];
		value = data[1] & 1;
		if (dsi->seq_version == 2)
			gpio_source = (data[1] >> 1) & 3;
	}

	i915_dbg("[drm] i915: DSI: GPIO index %u, number %u, source %u, set to %s\n",
		 gpio_index, gpio_number, gpio_source, value ? "on" : "off");

	if (d->is_vlv)
		vlv_exec_gpio(d, dsi, gpio_source, gpio_number, value);
	else if (d->is_chv)
		chv_exec_gpio(d, dsi, gpio_source, gpio_number, value);

	return data + size;
}

/*
 * I2C elements write to a device on one of the SoC's LPSS I2C controllers
 * (a bridge chip, a backlight driver); LikeOS has no driver for those
 * controllers, so the element is logged and skipped.
 */
static const uint8_t *dsi_exec_i2c(struct lg_display *d, struct lg_dsi *dsi,
				   const uint8_t *data)
{
	uint8_t vbt_i2c_bus_num = data[2];
	uint16_t target_addr = get_le16(data + 3);
	uint8_t reg_offset = data[5];
	uint8_t payload_size = data[6];

	(void)d;
	if (dsi->i2c_bus_num < 0)
		dsi->i2c_bus_num = vbt_i2c_bus_num;

	kprintf("[drm] i915: DSI: skipping I2C element (bus %u, addr 0x%02x, reg 0x%02x, %u bytes): no LPSS I2C support\n",
		vbt_i2c_bus_num, target_addr, reg_offset, payload_size);

	return data + payload_size + 7;
}

static const uint8_t *dsi_exec_spi(struct lg_display *d, struct lg_dsi *dsi,
				   const uint8_t *data)
{
	(void)d;
	(void)dsi;
	i915_dbg("[drm] i915: DSI: skipping SPI element\n");
	return data + data[5] + 6;
}

/*
 * PMIC elements set a register of the power management IC (the panel's
 * supply, its PWM) over the PMIC's I2C bus; LikeOS has no PMIC driver.
 */
static const uint8_t *dsi_exec_pmic(struct lg_display *d, struct lg_dsi *dsi,
				    const uint8_t *data)
{
	uint16_t i2c_address = get_le16(data + 1);
	uint32_t reg_address = get_le32(data + 3);
	uint32_t value = get_le32(data + 7);
	uint32_t mask = get_le32(data + 11);

	(void)d;
	(void)dsi;
	kprintf("[drm] i915: DSI: skipping PMIC element (i2c 0x%02x reg 0x%x value 0x%x mask 0x%x): no PMIC support\n",
		i2c_address, reg_address, value, mask);

	return data + 15;
}

typedef const uint8_t *(*dsi_elem_exec_fn)(struct lg_display *d, struct lg_dsi *dsi,
					    const uint8_t *data);

static const dsi_elem_exec_fn dsi_exec_elem[MIPI_SEQ_ELEM_MAX] = {
	[MIPI_SEQ_ELEM_SEND_PKT] = dsi_exec_send_packet,
	[MIPI_SEQ_ELEM_DELAY] = dsi_exec_delay,
	[MIPI_SEQ_ELEM_GPIO] = dsi_exec_gpio,
	[MIPI_SEQ_ELEM_I2C] = dsi_exec_i2c,
	[MIPI_SEQ_ELEM_SPI] = dsi_exec_spi,
	[MIPI_SEQ_ELEM_PMIC] = dsi_exec_pmic,
};

static const char *const dsi_seq_name[MIPI_SEQ_MAX] = {
	[MIPI_SEQ_END] = "MIPI_SEQ_END",
	[MIPI_SEQ_DEASSERT_RESET] = "MIPI_SEQ_DEASSERT_RESET",
	[MIPI_SEQ_INIT_OTP] = "MIPI_SEQ_INIT_OTP",
	[MIPI_SEQ_DISPLAY_ON] = "MIPI_SEQ_DISPLAY_ON",
	[MIPI_SEQ_DISPLAY_OFF] = "MIPI_SEQ_DISPLAY_OFF",
	[MIPI_SEQ_ASSERT_RESET] = "MIPI_SEQ_ASSERT_RESET",
	[MIPI_SEQ_BACKLIGHT_ON] = "MIPI_SEQ_BACKLIGHT_ON",
	[MIPI_SEQ_BACKLIGHT_OFF] = "MIPI_SEQ_BACKLIGHT_OFF",
	[MIPI_SEQ_TEAR_ON] = "MIPI_SEQ_TEAR_ON",
	[MIPI_SEQ_TEAR_OFF] = "MIPI_SEQ_TEAR_OFF",
	[MIPI_SEQ_POWER_ON] = "MIPI_SEQ_POWER_ON",
	[MIPI_SEQ_POWER_OFF] = "MIPI_SEQ_POWER_OFF",
};

/* Walk one sequence's elements. */
static void dsi_vbt_exec(struct lg_display *d, struct lg_dsi *dsi, enum dsi_seq seq_id)
{
	const uint8_t *data;
	dsi_elem_exec_fn exec;

	if ((unsigned int)seq_id >= MIPI_SEQ_MAX)
		return;

	data = dsi->sequence[seq_id];
	if (!data)
		return;

	if ((unsigned int)*data != (unsigned int)seq_id)
		kprintf("[drm] i915: DSI: sequence %d starts with id %u\n", seq_id, *data);

	i915_dbg("[drm] i915: DSI: starting MIPI sequence %d - %s\n", seq_id,
		 dsi_seq_name[seq_id]);

	/* the sequence byte */
	data++;
	/* the size of the sequence */
	if (dsi->seq_version >= 3)
		data += 4;

	while (*data != MIPI_SEQ_ELEM_END) {
		uint8_t operation_byte = *data++;
		uint8_t operation_size = 0;

		exec = operation_byte < MIPI_SEQ_ELEM_MAX ? dsi_exec_elem[operation_byte] : NULL;

		/* the size of the operation */
		if (dsi->seq_version >= 3)
			operation_size = *data++;

		if (exec) {
			const uint8_t *next = data + operation_size;

			data = exec(d, dsi, data);

			/* check against the size when there is one */
			if (operation_size && data != next) {
				kprintf("[drm] i915: DSI: inconsistent operation size\n");
				return;
			}
		} else if (operation_size) {
			i915_dbg("[drm] i915: DSI: unsupported MIPI operation byte %u\n",
				 operation_byte);
			data += operation_size;
		} else {
			/* no size: no way to skip it */
			kprintf("[drm] i915: DSI: unsupported MIPI operation byte %u\n",
				operation_byte);
			return;
		}
	}
}

/* A sequence, with the panel and backlight enable lines the driver
 * drives itself around it. */
static void dsi_vbt_exec_sequence(struct lg_display *d, struct lg_dsi *dsi, enum dsi_seq seq_id)
{
	if (seq_id == MIPI_SEQ_POWER_ON && dsi->gpio_panel >= 0)
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_panel, 1);
	if (seq_id == MIPI_SEQ_BACKLIGHT_ON && dsi->gpio_backlight >= 0)
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_backlight, 1);

	dsi_vbt_exec(d, dsi, seq_id);

	if (seq_id == MIPI_SEQ_POWER_OFF && dsi->gpio_panel >= 0)
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_panel, 0);
	if (seq_id == MIPI_SEQ_BACKLIGHT_OFF && dsi->gpio_backlight >= 0)
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_backlight, 0);
}

/* ---- parsing blocks 52 and 53 ------------------------------------------------- */

static void dsi_parse_backlight_ports(struct lg_display *d, struct lg_dsi *dsi)
{
	uint16_t port_c = 1u << DSI_PORT_C;

	if (!dsi->mipi.dual_link || d->vbt.version < 197) {
		dsi->bl_ports = (uint16_t)(1u << dsi->port);
		if (dsi->mipi.cabc_supported)
			dsi->cabc_ports = (uint16_t)(1u << dsi->port);
		return;
	}

	switch (dsi->mipi.dl_dcs_backlight_ports) {
	case VBT_DL_DCS_PORT_A:
		dsi->bl_ports = 1u << DSI_PORT_A;
		break;
	case VBT_DL_DCS_PORT_C:
		dsi->bl_ports = port_c;
		break;
	default:
	case VBT_DL_DCS_PORT_A_AND_C:
		dsi->bl_ports = (1u << DSI_PORT_A) | port_c;
		break;
	}

	if (!dsi->mipi.cabc_supported)
		return;

	switch (dsi->mipi.dl_dcs_cabc_ports) {
	case VBT_DL_DCS_PORT_A:
		dsi->cabc_ports = 1u << DSI_PORT_A;
		break;
	case VBT_DL_DCS_PORT_C:
		dsi->cabc_ports = port_c;
		break;
	default:
	case VBT_DL_DCS_PORT_A_AND_C:
		dsi->cabc_ports = (1u << DSI_PORT_A) | port_c;
		break;
	}
}

static int dsi_panel_type(struct lg_display *d)
{
	return d->vbt.panel_type >= 0 ? d->vbt.panel_type : 0;
}

/* Block 52: the panel's link configuration and its power delays. */
static int dsi_parse_mipi_config(struct lg_display *d, struct lg_dsi *dsi)
{
	const uint8_t *blk;
	uint32_t len, cfg_off, pps_off;
	int panel_type = dsi_panel_type(d);

	blk = lg_vbt_block(d, VBT_BLOCK_MIPI_CONFIG, &len);
	if (!blk) {
		kprintf("[drm] i915: DSI: no MIPI config block in the VBT\n");
		return -ENODEV;
	}
	if (panel_type >= VBT_MAX_MIPI_CONFIGURATIONS) {
		kprintf("[drm] i915: DSI: panel type %d has no MIPI config\n", panel_type);
		return -ENODEV;
	}

	i915_dbg("[drm] i915: DSI: found MIPI config block, panel index %d\n", panel_type);

	cfg_off = (uint32_t)panel_type * sizeof(struct vbt_mipi_config);
	pps_off = VBT_MAX_MIPI_CONFIGURATIONS * sizeof(struct vbt_mipi_config) +
		  (uint32_t)panel_type * sizeof(struct vbt_mipi_pps);
	if (cfg_off + sizeof(struct vbt_mipi_config) > len ||
	    pps_off + sizeof(struct vbt_mipi_pps) > len) {
		kprintf("[drm] i915: DSI: MIPI config block too short (%u bytes)\n", len);
		return -ENODEV;
	}

	mm_memcpy(&dsi->mipi, blk + cfg_off, sizeof(dsi->mipi));
	mm_memcpy(&dsi->pps, blk + pps_off, sizeof(dsi->pps));

	dsi_parse_backlight_ports(d, dsi);

	if (dsi->mipi.rotation)
		i915_dbg("[drm] i915: DSI: VBT panel rotation %u (not applied)\n",
			 dsi->mipi.rotation);
	return 0;
}

/* The sequences of one panel within block 53, and their size. */
static const uint8_t *dsi_find_panel_sequence_block(int version, const uint8_t *body,
						    uint32_t total, int panel_id,
						    uint32_t *seq_size)
{
	const uint8_t *data = body;
	uint32_t index = 0, current_size;
	int header_size = version >= 3 ? 5 : 3;
	uint8_t current_id;
	int i;

	/* skip the v3 block size */
	if (version >= 3)
		data += 4;

	for (i = 0; i < VBT_MAX_MIPI_CONFIGURATIONS && index < total; i++) {
		if (index + (uint32_t)header_size > total) {
			kprintf("[drm] i915: DSI: invalid sequence block (header)\n");
			return NULL;
		}

		current_id = data[index];
		if (version >= 3)
			current_size = get_le32(data + index + 1);
		else
			current_size = get_le16(data + index + 1);

		index += (uint32_t)header_size;

		if (index + current_size > total) {
			kprintf("[drm] i915: DSI: invalid sequence block\n");
			return NULL;
		}

		if (current_id == panel_id) {
			*seq_size = current_size;
			return data + index;
		}

		index += current_size;
	}

	kprintf("[drm] i915: DSI: sequence block detected but no valid configuration\n");
	return NULL;
}

static uint32_t dsi_goto_next_sequence(const uint8_t *data, uint32_t index, uint32_t total)
{
	uint32_t len = 0;

	/* skip the sequence byte */
	for (index = index + 1; index < total; index += len) {
		uint8_t operation_byte = data[index];

		index++;

		switch (operation_byte) {
		case MIPI_SEQ_ELEM_END:
			return index;
		case MIPI_SEQ_ELEM_SEND_PKT:
			if (index + 4 > total)
				return 0;
			len = (uint32_t)get_le16(data + index + 2) + 4;
			break;
		case MIPI_SEQ_ELEM_DELAY:
			len = 4;
			break;
		case MIPI_SEQ_ELEM_GPIO:
			len = 2;
			break;
		case MIPI_SEQ_ELEM_I2C:
			if (index + 7 > total)
				return 0;
			len = (uint32_t)data[index + 6] + 7;
			break;
		default:
			kprintf("[drm] i915: DSI: unknown operation byte\n");
			return 0;
		}
	}

	return 0;
}

static uint32_t dsi_goto_next_sequence_v3(const uint8_t *data, uint32_t index, uint32_t total)
{
	uint32_t seq_end, size_of_sequence, len;

	if (total < 5) {
		kprintf("[drm] i915: DSI: too small sequence size\n");
		return 0;
	}

	/* skip the sequence byte */
	index++;

	/* The size excludes the sequence byte and itself, includes the
	 * element end byte, excludes the final sequence end byte. */
	if (index + 4 > total)
		return 0;
	size_of_sequence = get_le32(data + index);
	index += 4;

	seq_end = index + size_of_sequence;
	if (seq_end > total || seq_end < index) {
		kprintf("[drm] i915: DSI: invalid sequence size\n");
		return 0;
	}

	for (; index < total; index += len) {
		uint8_t operation_byte = data[index];

		index++;

		if (operation_byte == MIPI_SEQ_ELEM_END) {
			if (index != seq_end) {
				kprintf("[drm] i915: DSI: invalid element structure\n");
				return 0;
			}
			return index;
		}

		if (index >= total)
			return 0;
		len = data[index];
		index++;

		switch (operation_byte) {
		case MIPI_SEQ_ELEM_SEND_PKT:
		case MIPI_SEQ_ELEM_DELAY:
		case MIPI_SEQ_ELEM_GPIO:
		case MIPI_SEQ_ELEM_I2C:
		case MIPI_SEQ_ELEM_SPI:
		case MIPI_SEQ_ELEM_PMIC:
			break;
		default:
			kprintf("[drm] i915: DSI: unknown operation byte %u\n", operation_byte);
			break;
		}
	}

	return 0;
}

/* The length of a v1/v2 init OTP sequence's leading delay and GPIO
 * elements, up to its first packet. */
static int dsi_init_otp_deassert_fragment_len(const struct lg_dsi *dsi)
{
	const uint8_t *data = dsi->sequence[MIPI_SEQ_INIT_OTP];
	int index, len;

	if (!data || dsi->seq_version >= 3)
		return 0;

	/* index 1 skips the sequence byte */
	for (index = 1; data[index] != MIPI_SEQ_ELEM_END; index += len) {
		switch (data[index]) {
		case MIPI_SEQ_ELEM_SEND_PKT:
			return index == 1 ? 0 : index;
		case MIPI_SEQ_ELEM_DELAY:
			len = 5; /* operand byte + 32-bit delay */
			break;
		case MIPI_SEQ_ELEM_GPIO:
			len = 3; /* operand byte, GPIO number, value */
			break;
		default:
			return 0;
		}
	}

	return 0;
}

/*
 * Some v1/v2 video mode VBTs release the panel's reset at the start of the
 * init OTP sequence instead of in a deassert sequence of their own.  The
 * reset must be released before the link is made ready, so split that
 * fragment off into a deassert sequence.
 */
static void dsi_vlv_fixup_sequences(struct lg_dsi *dsi)
{
	uint8_t *init_otp;
	int len;

	if (dsi->mipi.is_cmd_mode || dsi->seq_version >= 3)
		return;

	/* only with init OTP and assert sequences but no deassert one */
	if (!dsi->sequence[MIPI_SEQ_INIT_OTP] || !dsi->sequence[MIPI_SEQ_ASSERT_RESET] ||
	    dsi->sequence[MIPI_SEQ_DEASSERT_RESET])
		return;

	len = dsi_init_otp_deassert_fragment_len(dsi);
	if (!len)
		return;

	i915_dbg("[drm] i915: DSI: using the init OTP fragment to deassert reset\n");

	init_otp = (uint8_t *)dsi->sequence[MIPI_SEQ_INIT_OTP];
	dsi->deassert_seq = kalloc((size_t)len + 1);
	if (!dsi->deassert_seq)
		return;
	mm_memcpy(dsi->deassert_seq, init_otp, (size_t)len + 1);
	dsi->deassert_seq[0] = MIPI_SEQ_DEASSERT_RESET;
	dsi->deassert_seq[len] = MIPI_SEQ_ELEM_END;
	dsi->sequence[MIPI_SEQ_DEASSERT_RESET] = dsi->deassert_seq;
	/* the fragment's last byte becomes the init OTP sequence byte */
	init_otp[len - 1] = MIPI_SEQ_INIT_OTP;
	dsi->sequence[MIPI_SEQ_INIT_OTP] = init_otp + len - 1;
}

/* Block 53: copy the panel's sequences and note where each starts. */
static void dsi_parse_mipi_sequence(struct lg_display *d, struct lg_dsi *dsi)
{
	const uint8_t *blk, *seq_data;
	uint32_t len, total, seq_size = 0, index = 0;
	int version;
	uint8_t *data;

	if (dsi->mipi.panel_id != 1)
		i915_dbg("[drm] i915: DSI: VBT panel id %u\n", dsi->mipi.panel_id);

	blk = lg_vbt_block(d, VBT_BLOCK_MIPI_SEQUENCE, &len);
	if (!blk || len < 1) {
		kprintf("[drm] i915: DSI: no MIPI sequence block in the VBT\n");
		return;
	}

	version = blk[0];
	if (version >= 4) {
		kprintf("[drm] i915: DSI: unable to parse MIPI sequence block v%d\n", version);
		return;
	}
	i915_dbg("[drm] i915: DSI: found MIPI sequence block v%d\n", version);

	/* v3 blocks outgrew the 16-bit block size and carry a 32-bit one
	 * right after the version byte. */
	total = len;
	if (version >= 3) {
		if (len < 5)
			return;
		total = get_le32(blk + 1);
		if (d->vbt.raw && blk > d->vbt.raw && blk < d->vbt.raw + d->vbt.raw_len) {
			uint32_t room = (uint32_t)(d->vbt.raw + d->vbt.raw_len - blk);

			if (total > room)
				total = room;
		}
	}

	seq_data = dsi_find_panel_sequence_block(version, blk + 1, total,
						 dsi_panel_type(d), &seq_size);
	if (!seq_data || !seq_size)
		return;

	data = kalloc(seq_size);
	if (!data)
		return;
	mm_memcpy(data, seq_data, seq_size);

	dsi->seq_version = version;
	for (;;) {
		uint8_t seq_id;

		if (index >= seq_size) {
			kprintf("[drm] i915: DSI: MIPI sequences run past the block\n");
			goto err;
		}
		seq_id = data[index];
		if (seq_id == MIPI_SEQ_END)
			break;

		if (seq_id >= MIPI_SEQ_MAX) {
			kprintf("[drm] i915: DSI: unknown sequence %u\n", seq_id);
			goto err;
		}

		if (seq_id == MIPI_SEQ_TEAR_ON || seq_id == MIPI_SEQ_TEAR_OFF)
			i915_dbg("[drm] i915: DSI: unsupported sequence %u\n", seq_id);

		dsi->sequence[seq_id] = data + index;

		if (version >= 3)
			index = dsi_goto_next_sequence_v3(data, index, seq_size);
		else
			index = dsi_goto_next_sequence(data, index, seq_size);
		if (!index) {
			kprintf("[drm] i915: DSI: invalid sequence %u\n", seq_id);
			goto err;
		}
	}

	dsi->seq_data = data;
	dsi->seq_size = seq_size;

	if (d->is_vlv)
		dsi_vlv_fixup_sequences(dsi);

	i915_dbg("[drm] i915: DSI: MIPI VBT parsing complete\n");
	return;

err:
	kfree(data);
	mm_memset(dsi->sequence, 0, sizeof(dsi->sequence));
}

/* Block 43: how the backlight is controlled (VBT 191+) and the brightness
 * precision (236+). */
static void dsi_parse_backlight(struct lg_display *d, struct lg_dsi *dsi, int *ctl_type)
{
	const uint8_t *blk;
	uint32_t len;
	int pt = dsi_panel_type(d);

	*ctl_type = -1;
	dsi->bl_precision_bits = 0;

	if (pt >= 16)
		return;
	blk = lg_vbt_block(d, VBT_BLOCK_LFP_BACKLIGHT, &len);
	if (!blk || len < 1 + 16 * 6 || blk[0] != 6)
		return;
	/* data[pt].type: 2 = PWM */
	if ((blk[1 + pt * 6] & 3) != VBT_BACKLIGHT_TYPE_PWM)
		return;

	*ctl_type = VBT_BACKLIGHT_DISPLAY_DDI;
	if (d->vbt.version >= 191 && len >= 113 + 16)
		*ctl_type = blk[113 + pt] & 0xf;
	if (d->vbt.version >= 236 && len >= 257 + 16)
		dsi->bl_precision_bits = blk[257 + pt];
}

/* ---- link parameters ------------------------------------------------------------ */

static enum dsi_pixel_format dsi_vbt_to_pixel_format(unsigned int format)
{
	switch (format) {
	case VBT_PIXEL_FORMAT_RGB888:
		return DSI_FMT_RGB888;
	case VBT_PIXEL_FORMAT_RGB666_LOOSELY_PACKED:
		return DSI_FMT_RGB666;
	case VBT_PIXEL_FORMAT_RGB666:
		return DSI_FMT_RGB666_PACKED;
	case VBT_PIXEL_FORMAT_RGB565:
		return DSI_FMT_RGB565;
	default:
		kprintf("[drm] i915: DSI: unknown VBT pixel format %u\n", format);
		return DSI_FMT_RGB666;
	}
}

static uint32_t dsi_pixel_format_to_reg(enum dsi_pixel_format fmt)
{
	switch (fmt) {
	case DSI_FMT_RGB888:
		return MIPI_VID_MODE_FORMAT_RGB888;
	case DSI_FMT_RGB666:
		return MIPI_VID_MODE_FORMAT_RGB666;
	case DSI_FMT_RGB666_PACKED:
		return MIPI_VID_MODE_FORMAT_RGB666_PACKED;
	case DSI_FMT_RGB565:
		return MIPI_VID_MODE_FORMAT_RGB565;
	}
	return MIPI_VID_MODE_FORMAT_RGB666;
}

static void dsi_log_params(const struct lg_dsi *dsi)
{
	i915_dbg("[drm] i915: DSI parameters: pclk %u, pixel overlap %u, lanes %u, DPHY param 0x%x\n",
		 dsi->pclk, dsi->pixel_overlap, dsi->lane_count, dsi->dphy_reg);
	i915_dbg("[drm] i915: DSI parameters: video mode format %s, burst ratio %u, reset timer %u\n",
		 dsi->video_mode == VBT_NON_BURST_SYNC_PULSE ? "non-burst with sync pulse" :
		 dsi->video_mode == VBT_NON_BURST_SYNC_EVENTS ? "non-burst with sync events" :
		 dsi->video_mode == VBT_BURST_MODE ? "burst" : "<unknown>",
		 dsi->burst_mode_ratio, dsi->rst_timer_val);
	i915_dbg("[drm] i915: DSI parameters: LP clock in LPM %d, blanking packets %d, EoT %d, clock stop %d, %s mode\n",
		 dsi->lp_clock_during_lpm, dsi->blanking_pkt, dsi->eot_pkt, dsi->clock_stop,
		 dsi->operation_mode ? "command" : "video");
	i915_dbg("[drm] i915: DSI parameters: dual link %u, pixel format %d, TLPX %u\n",
		 dsi->dual_link, dsi->pixel_format, dsi->escape_clk_div);
	i915_dbg("[drm] i915: DSI parameters: LP RX timeout 0x%x, turnaround 0x%x, init count 0x%x, HS to LP 0x%x\n",
		 dsi->lp_rx_timeout, dsi->turn_arnd_val, dsi->init_count, dsi->hs_to_lp_count);
	i915_dbg("[drm] i915: DSI parameters: LP byte clock %u, DBI BW timer 0x%x, clock LP to HS 0x%x, HS to LP 0x%x, BTA %s\n",
		 dsi->lp_byte_clk, dsi->bw_timer, dsi->clk_lp_to_hs_count, dsi->clk_hs_to_lp_count,
		 (dsi->video_frmt_cfg_bits & MIPI_DISABLE_VIDEO_BTA) ? "disabled" : "enabled");
}

/* The link's parameters from block 52 and the panel's timing. */
static int dsi_vbt_init(struct lg_display *d, struct lg_dsi *dsi,
			const struct drm_mode_modeinfo *mode)
{
	struct vbt_mipi_config *mipi = &dsi->mipi;
	const struct vbt_mipi_pps *pps = &dsi->pps;
	uint16_t burst_mode_ratio;

	(void)d;
	dsi->lp_clock_during_lpm = mipi->lp_clock_during_lpm;
	dsi->blanking_pkt = mipi->blanking_packets_during_bllp;
	dsi->eot_pkt = !mipi->eot_pkt_disabled;
	dsi->clock_stop = mipi->enable_clk_stop;
	dsi->lane_count = mipi->lane_cnt + 1u;
	dsi->pixel_format = dsi_vbt_to_pixel_format(mipi->videomode_color_format);

	dsi->dual_link = mipi->dual_link;
	dsi->pixel_overlap = mipi->pixel_overlap;
	dsi->operation_mode = mipi->is_cmd_mode;
	dsi->video_mode = mipi->video_transfer_mode;
	dsi->escape_clk_div = mipi->byte_clk_sel;
	dsi->lp_rx_timeout = (uint16_t)mipi->lp_rx_timeout;
	dsi->hs_tx_timeout = (uint16_t)mipi->hs_tx_timeout;
	dsi->turn_arnd_val = (uint16_t)mipi->turn_around_timeout;
	dsi->rst_timer_val = (uint16_t)mipi->device_reset_timer;
	dsi->init_count = (uint16_t)mipi->master_init_timer;
	dsi->bw_timer = mipi->dbi_bw_timer;
	dsi->video_frmt_cfg_bits = mipi->bta_disable ? MIPI_DISABLE_VIDEO_BTA : 0;
	dsi->bgr_enabled = mipi->rgb_flip;

	/* the starting point, adjusted for dual link and burst mode */
	dsi->pclk = mode->clock;

	/* each port of a dual link carries half the pixels */
	if (dsi->dual_link) {
		dsi->pclk /= 2;

		/* the overlapping pixels of a front-back link need extra clock */
		if (dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
			dsi->pclk += DIV_ROUND_UP((uint32_t)mode->vtotal * dsi->pixel_overlap * 60,
						  1000u);
	}

	/* Burst mode ratio: the VBT's target DDR frequency over the
	 * non-burst one, times 100 to keep the remainder. */
	if (dsi->video_mode == VBT_BURST_MODE) {
		uint32_t bitrate;

		if (mipi->target_burst_mode_freq == 0) {
			kprintf("[drm] i915: DSI: burst mode target is not set\n");
			return -EINVAL;
		}

		bitrate = (uint32_t)dsi_bitrate(dsi);

		/* A VBT target slightly below the computed bitrate is
		 * rounding: use the computed one. */
		if (mipi->target_burst_mode_freq < bitrate &&
		    dsi_fuzzy_clock_check((int)mipi->target_burst_mode_freq, (int)bitrate))
			mipi->target_burst_mode_freq = bitrate;

		if (mipi->target_burst_mode_freq < bitrate) {
			kprintf("[drm] i915: DSI: burst mode freq is less than computed\n");
			return -EINVAL;
		}

		burst_mode_ratio = (uint16_t)DIV_ROUND_UP(mipi->target_burst_mode_freq * 100u,
							  bitrate);
		dsi->pclk = DIV_ROUND_UP(dsi->pclk * burst_mode_ratio, 100u);
	} else {
		burst_mode_ratio = 100;
	}
	dsi->burst_mode_ratio = burst_mode_ratio;

	/* the VBT's delays are in 100 us units */
	dsi->backlight_off_delay = pps->bl_disable_delay / 10;
	dsi->backlight_on_delay = pps->bl_enable_delay / 10;
	dsi->panel_on_delay = pps->panel_on_delay / 10;
	dsi->panel_off_delay = pps->panel_off_delay / 10;
	dsi->panel_pwr_cycle_delay = pps->panel_power_cycle_delay / 10;

	dsi->i2c_bus_num = -1;
	return 0;
}

#define DSI_NS_KHZ_RATIO 1000000u
#define DSI_PREPARE_CNT_MAX 0x3fu
#define DSI_EXIT_ZERO_CNT_MAX 0x3fu
#define DSI_CLK_ZERO_CNT_MAX 0xffu
#define DSI_TRAIL_CNT_MAX 0x1fu

/* The D-PHY timing counts from the VBT's nanosecond values at the link's
 * bit rate (prepare, zero, trail, the LP <-> HS switch times). */
static void dsi_dphy_param_init(struct lg_dsi *dsi)
{
	const struct vbt_mipi_config *mipi = &dsi->mipi;
	uint32_t tlpx_ns, extra_byte_count, tlpx_ui;
	uint32_t ui_num, ui_den;
	uint32_t prepare_cnt, exit_zero_cnt, clk_zero_cnt, trail_cnt;
	uint32_t ths_prepare_ns, tclk_trail_ns;
	uint32_t tclk_prepare_clkzero, ths_prepare_hszero;
	uint32_t lp_to_hs_switch, hs_to_lp_switch;
	uint32_t mul = 2; /* counts are in HS DDR clocks */

	tlpx_ns = (uint32_t)dsi_tlpx_ns(dsi);

	switch (dsi->lane_count) {
	case 1:
	case 2:
		extra_byte_count = 2;
		break;
	case 3:
		extra_byte_count = 4;
		break;
	case 4:
	default:
		extra_byte_count = 3;
		break;
	}

	/* the UI from the bit rate in kbps */
	ui_num = DSI_NS_KHZ_RATIO;
	ui_den = (uint32_t)dsi_bitrate(dsi);

	tclk_prepare_clkzero = mipi->tclk_prepare_clkzero;
	ths_prepare_hszero = mipi->ths_prepare_hszero;

	/* B060: LP byte clock = TLPX / (8 UI) */
	dsi->lp_byte_clk = (uint16_t)DIV_ROUND_UP((uint64_t)tlpx_ns * ui_den, 8ull * ui_num);

	/* DDR clock period = 2 UI; the counts are ns / DDR clock period. */
	ths_prepare_ns = dsi_max_u32(mipi->ths_prepare, mipi->tclk_prepare);

	/* prepare count */
	prepare_cnt = (uint32_t)DIV_ROUND_UP((uint64_t)ths_prepare_ns * ui_den,
					     (uint64_t)ui_num * mul);
	if (prepare_cnt > DSI_PREPARE_CNT_MAX) {
		i915_dbg("[drm] i915: DSI: prepare count too high %u\n", prepare_cnt);
		prepare_cnt = DSI_PREPARE_CNT_MAX;
	}

	/* exit zero count */
	exit_zero_cnt = (uint32_t)DIV_ROUND_UP(
		(uint64_t)(uint32_t)(ths_prepare_hszero - ths_prepare_ns) * ui_den,
		(uint64_t)ui_num * mul);

	/* Exit zero unifies ths_zero and ths_exit; ths_exit is at least
	 * 110 ns, so exit_zero_cnt is at least 55 / UI. */
	if (exit_zero_cnt < (55 * ui_den / ui_num) && (55 * ui_den) % ui_num)
		exit_zero_cnt += 1;

	if (exit_zero_cnt > DSI_EXIT_ZERO_CNT_MAX) {
		i915_dbg("[drm] i915: DSI: exit zero count too high %u\n", exit_zero_cnt);
		exit_zero_cnt = DSI_EXIT_ZERO_CNT_MAX;
	}

	/* clock zero count */
	clk_zero_cnt = (uint32_t)DIV_ROUND_UP(
		(uint64_t)(uint32_t)(tclk_prepare_clkzero - ths_prepare_ns) * ui_den,
		(uint64_t)ui_num * mul);
	if (clk_zero_cnt > DSI_CLK_ZERO_CNT_MAX) {
		i915_dbg("[drm] i915: DSI: clock zero count too high %u\n", clk_zero_cnt);
		clk_zero_cnt = DSI_CLK_ZERO_CNT_MAX;
	}

	/* trail count */
	tclk_trail_ns = dsi_max_u32(mipi->tclk_trail, mipi->ths_trail);
	trail_cnt = (uint32_t)DIV_ROUND_UP((uint64_t)tclk_trail_ns * ui_den,
					   (uint64_t)ui_num * mul);
	if (trail_cnt > DSI_TRAIL_CNT_MAX) {
		i915_dbg("[drm] i915: DSI: trail count too high %u\n", trail_cnt);
		trail_cnt = DSI_TRAIL_CNT_MAX;
	}

	/* B080 */
	dsi->dphy_reg = exit_zero_cnt << 24 | trail_cnt << 16 | clk_zero_cnt << 8 | prepare_cnt;

	/*
	 * B044: the high/low switch count is the larger of
	 *   LP to HS = 4 TLPX + prepare * mul + exit zero * mul + 10 UI
	 *   HS to LP = THS-TRAIL + 2 TLPX
	 * in byte clocks, plus the extra byte count of the lane count.
	 */
	tlpx_ui = (uint32_t)DIV_ROUND_UP((uint64_t)tlpx_ns * ui_den, ui_num);

	lp_to_hs_switch = DIV_ROUND_UP(4 * tlpx_ui + prepare_cnt * mul + exit_zero_cnt * mul + 10,
				       8u);
	hs_to_lp_switch = DIV_ROUND_UP(mipi->ths_trail + 2 * tlpx_ui, 8u);

	dsi->hs_to_lp_count = (uint16_t)dsi_max_u32(lp_to_hs_switch, hs_to_lp_switch);
	dsi->hs_to_lp_count += (uint16_t)extra_byte_count;

	/* B088: LP -> HS for the clock lane, 4 TLPX + prepare and zero in
	 * DDR clocks, in byte clocks, plus the extra byte count */
	dsi->clk_lp_to_hs_count =
		(uint16_t)DIV_ROUND_UP(4 * tlpx_ui + prepare_cnt * 2 + clk_zero_cnt * 2, 8u);
	dsi->clk_lp_to_hs_count += (uint16_t)extra_byte_count;

	/* HS -> LP for the clock lane: 2 TLPX + 8 UI + the trail, in byte
	 * clocks, plus the extra byte count */
	dsi->clk_hs_to_lp_count = (uint16_t)DIV_ROUND_UP(2 * tlpx_ui + trail_cnt * 2 + 8, 8u);
	dsi->clk_hs_to_lp_count += (uint16_t)extra_byte_count;

	dsi_log_params(dsi);
}

/* ---- the DSI PLL --------------------------------------------------------------- */

/* The PLL's M divider goes in as an LFSR seed: index m - 62. */
static const uint16_t dsi_lfsr_converts[] = {
	426, 469, 234, 373, 442, 221, 110, 311, 411, /* 62 - 70 */
	461, 486, 243, 377, 188, 350, 175, 343, 427, 213, /* 71 - 80 */
	106, 53, 282, 397, 454, 227, 113, 56, 284, 142, /* 81 - 90 */
	71, 35, 273, 136, 324, 418, 465, 488, 500, 506 /* 91 - 100 */
};

#define DSI_LFSR_COUNT (sizeof(dsi_lfsr_converts) / sizeof(dsi_lfsr_converts[0]))

/* The link's bit clock per lane from the pixel clock. */
static uint32_t dsi_clk_from_pclk(uint32_t pclk, enum dsi_pixel_format fmt, unsigned int lanes)
{
	uint32_t bpp = (uint32_t)dsi_pixel_format_to_bpp(fmt);

	return DIV_ROUND_CLOSEST(pclk * bpp, lanes);
}

/* M and P for the target DSI clock: Valleyview runs a 25 MHz reference
 * with N 1, Cherryview 100 MHz with N 4. */
static int dsi_calc_mnp(struct lg_display *d, struct lg_dsi *dsi, int target_dsi_clk)
{
	unsigned int m_min, m_max, p_min = 2, p_max = 6;
	unsigned int m, n, p;
	unsigned int calc_m, calc_p;
	int delta, ref_clk;

	if (target_dsi_clk < 300000 || target_dsi_clk > 1150000) {
		kprintf("[drm] i915: DSI: DSI clock %d kHz out of range\n", target_dsi_clk);
		return -EINVAL;
	}

	if (d->is_chv) {
		ref_clk = 100000;
		n = 4;
		m_min = 70;
		m_max = 96;
	} else {
		ref_clk = 25000;
		n = 1;
		m_min = 62;
		m_max = 92;
	}

	calc_p = p_min;
	calc_m = m_min;
	delta = dsi_abs(target_dsi_clk - (int)((m_min * (unsigned int)ref_clk) / (p_min * n)));

	for (m = m_min; m <= m_max && delta; m++) {
		for (p = p_min; p <= p_max && delta; p++) {
			int calc_dsi_clk = (int)((m * (unsigned int)ref_clk) / (p * n));
			int dd = dsi_abs(target_dsi_clk - calc_dsi_clk);

			if (dd < delta) {
				delta = dd;
				calc_m = m;
				calc_p = p;
			}
		}
	}

	/* the register takes log2(N1), fine for the powers of two used */
	dsi->pll_ctrl = 1u << (CCK_DSI_PLL_P1_POST_DIV_SHIFT + calc_p - 2);
	dsi->pll_div = (uint32_t)(dsi_ffs(n) - 1) << CCK_DSI_PLL_N1_DIV_SHIFT |
		       (uint32_t)dsi_lfsr_converts[calc_m - 62] << CCK_DSI_PLL_M1_DIV_SHIFT;
	return 0;
}

/* The pixel clock the PLL values make. */
static uint32_t dsi_pclk_from_pll(struct lg_display *d, const struct lg_dsi *dsi,
				  uint32_t pll_ctl, uint32_t pll_div)
{
	int bpp = dsi_pixel_format_to_bpp(dsi->pixel_format);
	uint32_t dsi_clock, m, p, n;
	uint32_t refclk = d->is_chv ? 100000u : 25000u;
	unsigned int i;

	/* the P1 divisor */
	pll_ctl &= CCK_DSI_PLL_P1_POST_DIV_MASK;
	pll_ctl = pll_ctl >> (CCK_DSI_PLL_P1_POST_DIV_SHIFT - 2);

	/* N1, as log2 */
	n = (pll_div & CCK_DSI_PLL_N1_DIV_MASK) >> CCK_DSI_PLL_N1_DIV_SHIFT;
	n = 1u << n;

	/* the M1 seed */
	pll_div &= CCK_DSI_PLL_M1_DIV_MASK;
	pll_div = pll_div >> CCK_DSI_PLL_M1_DIV_SHIFT;

	p = (uint32_t)dsi_fls(pll_ctl);
	if (p)
		p--;
	if (!p) {
		kprintf("[drm] i915: DSI: wrong P1 divisor\n");
		return 0;
	}

	for (i = 0; i < DSI_LFSR_COUNT; i++)
		if (dsi_lfsr_converts[i] == pll_div)
			break;
	if (i == DSI_LFSR_COUNT) {
		kprintf("[drm] i915: DSI: wrong M seed programmed\n");
		return 0;
	}
	m = i + 62;

	dsi_clock = (m * refclk) / (p * n);

	return DIV_ROUND_CLOSEST(dsi_clock * dsi->lane_count, (uint32_t)bpp);
}

/* The PLL values for the panel and the pixel clock they make.  The clock
 * gates of the ports driven are opened; the muxes stay on the DSI PLL. */
static int dsi_pll_compute(struct lg_display *d, struct lg_dsi *dsi, uint32_t *pclk)
{
	uint32_t dsi_clk;
	int ret;

	dsi_clk = dsi_clk_from_pclk(dsi->pclk, dsi->pixel_format, dsi->lane_count);

	ret = dsi_calc_mnp(d, dsi, (int)dsi_clk);
	if (ret) {
		i915_dbg("[drm] i915: DSI: dsi_calc_mnp failed\n");
		return ret;
	}

	if (dsi->ports & (1u << DSI_PORT_A))
		dsi->pll_ctrl |= CCK_DSI_PLL_CLK_GATE_DSI0_DSIPLL;
	if (dsi->ports & (1u << DSI_PORT_C))
		dsi->pll_ctrl |= CCK_DSI_PLL_CLK_GATE_DSI1_DSIPLL;

	dsi->pll_ctrl |= CCK_DSI_PLL_VCO_EN;

	i915_dbg("[drm] i915: DSI: PLL div %08x, ctrl %08x\n", dsi->pll_div, dsi->pll_ctrl);

	*pclk = dsi_pclk_from_pll(d, dsi, dsi->pll_ctrl, dsi->pll_div);
	return *pclk ? 0 : -EINVAL;
}

static void dsi_pll_enable(struct lg_display *d, struct lg_dsi *dsi)
{
	uint32_t val = 0;
	int i;

	i915_dbg("[drm] i915: DSI: PLL enable\n");

	lg_vlv_cck_write(d, CCK_DSI_PLL_CONTROL, 0);
	lg_vlv_cck_write(d, CCK_DSI_PLL_DIVIDER, dsi->pll_div);
	lg_vlv_cck_write(d, CCK_DSI_PLL_CONTROL, dsi->pll_ctrl & ~CCK_DSI_PLL_VCO_EN);

	/* at least 0.5 us after ungating before the VCO goes on */
	lg_udelay(10);

	lg_vlv_cck_write(d, CCK_DSI_PLL_CONTROL, dsi->pll_ctrl);

	/* lock within 20 ms, polled every 500 us */
	for (i = 0; i <= 40; i++) {
		val = lg_vlv_cck_read(d, CCK_DSI_PLL_CONTROL);
		if (val & CCK_DSI_PLL_LOCK)
			break;
		lg_udelay(500);
	}
	if (!(val & CCK_DSI_PLL_LOCK)) {
		kprintf("[drm] i915: DSI: PLL lock failed\n");
		return;
	}

	i915_dbg("[drm] i915: DSI: PLL locked\n");
}

static void dsi_pll_disable(struct lg_display *d)
{
	uint32_t tmp;

	i915_dbg("[drm] i915: DSI: PLL disable\n");

	tmp = lg_vlv_cck_read(d, CCK_DSI_PLL_CONTROL);
	tmp &= ~CCK_DSI_PLL_VCO_EN;
	tmp |= CCK_DSI_PLL_LDO_GATE;
	lg_vlv_cck_write(d, CCK_DSI_PLL_CONTROL, tmp);
}

static void dsi_reset_clocks(struct lg_display *d, struct lg_dsi *dsi, int port)
{
	uint32_t temp;

	temp = lg_rd(d, MIPI_CTRL(port));
	temp &= ~MIPI_ESCAPE_CLOCK_DIVIDER_MASK;
	lg_wr(d, MIPI_CTRL(port),
	      temp | (uint32_t)dsi->escape_clk_div << MIPI_ESCAPE_CLOCK_DIVIDER_SHIFT);
}

/* ---- the port: timing, ready states, enable --------------------------------------- */

/* pixels in high speed byte clocks */
static uint16_t dsi_txbyteclkhs(uint16_t pixels, int bpp, unsigned int lane_count,
				uint16_t burst_mode_ratio)
{
	return (uint16_t)DIV_ROUND_UP(DIV_ROUND_UP((uint32_t)pixels * (uint32_t)bpp *
						   burst_mode_ratio, 8u * 100u),
				      lane_count);
}

/* escape clock cycles for a duration in us at a divider */
static uint16_t dsi_txclkesc(uint32_t divider, unsigned int us)
{
	switch (divider) {
	case MIPI_ESCAPE_CLOCK_DIVIDER_1:
	default:
		return (uint16_t)(20 * us);
	case MIPI_ESCAPE_CLOCK_DIVIDER_2:
		return (uint16_t)(10 * us);
	case MIPI_ESCAPE_CLOCK_DIVIDER_4:
		return (uint16_t)(5 * us);
	}
}

/* Horizontal values go in as high speed byte clocks (per port on a dual
 * link), vertical ones as lines. */
static void dsi_set_timings(struct lg_display *d, struct lg_dsi *dsi, const struct lg_timings *t)
{
	int bpp = dsi_pixel_format_to_bpp(dsi->pixel_format);
	unsigned int lane_count = dsi->lane_count;
	uint16_t hactive, hfp, hsync, hbp, vfp, vsync, vbp;
	int port;

	hactive = t->hdisplay;
	hfp = (uint16_t)(t->hsync_start - t->hdisplay);
	hsync = (uint16_t)(t->hsync_end - t->hsync_start);
	hbp = (uint16_t)(t->htotal - t->hsync_end);

	if (dsi->dual_link) {
		hactive /= 2;
		if (dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
			hactive += dsi->pixel_overlap;
		hfp /= 2;
		hsync /= 2;
		hbp /= 2;
	}

	vfp = (uint16_t)(t->vsync_start - t->vdisplay);
	vsync = (uint16_t)(t->vsync_end - t->vsync_start);
	vbp = (uint16_t)(t->vtotal - t->vsync_end);

	hactive = dsi_txbyteclkhs(hactive, bpp, lane_count, dsi->burst_mode_ratio);
	hfp = dsi_txbyteclkhs(hfp, bpp, lane_count, dsi->burst_mode_ratio);
	hsync = dsi_txbyteclkhs(hsync, bpp, lane_count, dsi->burst_mode_ratio);
	hbp = dsi_txbyteclkhs(hbp, bpp, lane_count, dsi->burst_mode_ratio);

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		lg_wr(d, MIPI_HACTIVE_AREA_COUNT(port), hactive);
		lg_wr(d, MIPI_HFP_COUNT(port), hfp);
		/* only meaningful in non-burst sync pulse mode */
		lg_wr(d, MIPI_HSYNC_PADDING_COUNT(port), hsync);
		lg_wr(d, MIPI_HBP_COUNT(port), hbp);
		lg_wr(d, MIPI_VFP_COUNT(port), vfp);
		lg_wr(d, MIPI_VSYNC_PADDING_COUNT(port), vsync);
		lg_wr(d, MIPI_VBP_COUNT(port), vbp);
	}
}

/* Program the controller of each port for the mode: escape clock, D-PHY
 * timing, resolution, the pixel format, timeouts and switch counts, the
 * video mode format.  The device must not be ready while this runs. */
static void dsi_prepare(struct lg_display *d, struct lg_dsi *dsi, const struct lg_config *cfg)
{
	const struct lg_timings *t = &cfg->t;
	int bpp = dsi_pixel_format_to_bpp(dsi->pixel_format);
	uint32_t val, tmp;
	uint16_t mode_hdisplay;
	int port;

	i915_dbg("[drm] i915: DSI: prepare, pipe %c\n", (char)('A' + cfg->pipe));

	mode_hdisplay = t->hdisplay;
	if (dsi->dual_link) {
		mode_hdisplay /= 2;
		if (dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
			mode_hdisplay += dsi->pixel_overlap;
	}

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		/* escape clock divider, 20 MHz, shared by A and C */
		tmp = lg_rd(d, MIPI_CTRL(DSI_PORT_A));
		tmp &= ~MIPI_ESCAPE_CLOCK_DIVIDER_MASK;
		lg_wr(d, MIPI_CTRL(DSI_PORT_A), tmp | MIPI_ESCAPE_CLOCK_DIVIDER_1);

		/* the read request priority is per pipe */
		tmp = lg_rd(d, MIPI_CTRL(port));
		tmp &= ~MIPI_READ_REQUEST_PRIORITY_MASK;
		lg_wr(d, MIPI_CTRL(port), tmp | MIPI_READ_REQUEST_PRIORITY_HIGH);

		lg_wr(d, MIPI_INTR_STAT(port), 0xffffffffu);
		lg_wr(d, MIPI_INTR_EN(port), 0xffffffffu);

		lg_wr(d, MIPI_DPHY_PARAM(port), dsi->dphy_reg);

		lg_wr(d, MIPI_DPI_RESOLUTION(port),
		      (uint32_t)t->vdisplay << MIPI_VERTICAL_ADDRESS_SHIFT |
			      (uint32_t)mode_hdisplay << MIPI_HORIZONTAL_ADDRESS_SHIFT);
	}

	dsi_set_timings(d, dsi, t);

	val = dsi->lane_count << MIPI_DATA_LANES_PRG_REG_SHIFT;
	if (dsi_is_cmd_mode(dsi)) {
		val |= (uint32_t)dsi->channel << MIPI_CMD_MODE_CHANNEL_NUMBER_SHIFT;
		val |= MIPI_CMD_MODE_DATA_WIDTH_8_BIT;
	} else {
		val |= (uint32_t)dsi->channel << MIPI_VID_MODE_CHANNEL_NUMBER_SHIFT;
		val |= dsi_pixel_format_to_reg(dsi->pixel_format);
	}

	tmp = 0;
	if (!dsi->eot_pkt)
		tmp |= MIPI_EOT_DISABLE;
	if (dsi->clock_stop)
		tmp |= MIPI_CLOCKSTOP;

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		lg_wr(d, MIPI_DSI_FUNC_PRG(port), val);

		/*
		 * The HS TX recovery timeout, in byte clocks: a little more
		 * than one line in burst mode, than one frame otherwise.
		 * The count is kept to 16 bits, as the hardware has always
		 * been programmed.
		 */
		if (dsi_is_vid_mode(dsi) && dsi->video_mode == VBT_BURST_MODE)
			lg_wr(d, MIPI_HS_TX_TIMEOUT(port),
			      (uint32_t)dsi_txbyteclkhs(t->htotal, bpp, dsi->lane_count,
							dsi->burst_mode_ratio) + 1);
		else
			lg_wr(d, MIPI_HS_TX_TIMEOUT(port),
			      (uint32_t)dsi_txbyteclkhs((uint16_t)((uint32_t)t->vtotal * t->htotal),
							bpp, dsi->lane_count,
							dsi->burst_mode_ratio) + 1);

		lg_wr(d, MIPI_LP_RX_TIMEOUT(port), dsi->lp_rx_timeout);
		lg_wr(d, MIPI_TURN_AROUND_TIMEOUT(port), dsi->turn_arnd_val);
		lg_wr(d, MIPI_DEVICE_RESET_TIMER(port), dsi->rst_timer_val);

		/* in low power clocks */
		lg_wr(d, MIPI_INIT_COUNT(port), dsi_txclkesc(dsi->escape_clk_div, 100));

		/* recovery disables */
		lg_wr(d, MIPI_EOT_DISABLE_REG(port), tmp);

		/* in low power clocks */
		lg_wr(d, MIPI_INIT_COUNT(port), dsi->init_count);

		/* in byte clocks: the high to low switch plus the stop state
		 * stall times the LP byte clock */
		lg_wr(d, MIPI_HIGH_LOW_SWITCH_COUNT(port), dsi->hs_to_lp_count);

		/* how many byte clocks one low power clock takes */
		lg_wr(d, MIPI_LP_BYTECLK(port), dsi->lp_byte_clk);

		/* the bandwidth for 16 long packets of 252 bytes (DCS memory
		 * writes), in byte clocks */
		lg_wr(d, MIPI_DBI_BW_CTRL(port), dsi->bw_timer);

		lg_wr(d, MIPI_CLK_LANE_SWITCH_TIME_CNT(port),
		      (uint32_t)dsi->clk_lp_to_hs_count << MIPI_LP_HS_SSW_CNT_SHIFT |
			      (uint32_t)dsi->clk_hs_to_lp_count << MIPI_HS_LP_PWR_SW_CNT_SHIFT);

		if (dsi_is_vid_mode(dsi)) {
			uint32_t fmt = dsi->video_frmt_cfg_bits | MIPI_IP_TG_CONFIG;

			/* panels whose width is not a multiple of 64
			 * (1366x768) need random resolution support */
			fmt |= MIPI_RANDOM_DPI_DISPLAY_RESOLUTION;

			switch (dsi->video_mode) {
			default:
				kprintf("[drm] i915: DSI: unknown video mode %d\n", dsi->video_mode);
				/* fall through */
			case VBT_NON_BURST_SYNC_EVENTS:
				fmt |= MIPI_VIDEO_MODE_NON_BURST_WITH_SYNC_EVENTS;
				break;
			case VBT_NON_BURST_SYNC_PULSE:
				fmt |= MIPI_VIDEO_MODE_NON_BURST_WITH_SYNC_PULSE;
				break;
			case VBT_BURST_MODE:
				fmt |= MIPI_VIDEO_MODE_BURST;
				break;
			}

			lg_wr(d, MIPI_VIDEO_MODE_FORMAT(port), fmt);
		}
	}
}

static void dsi_unprepare(struct lg_display *d, struct lg_dsi *dsi)
{
	int port;

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		/* panel commands can be sent while the clock is in LP-11 */
		lg_wr(d, MIPI_DEVICE_READY(port), 0x0);

		dsi_reset_clocks(d, dsi, port);
		lg_wr(d, MIPI_EOT_DISABLE_REG(port), MIPI_CLOCKSTOP);

		lg_rmw(d, MIPI_DSI_FUNC_PRG(port), MIPI_VID_MODE_FORMAT_MASK, 0);

		lg_wr(d, MIPI_DEVICE_READY(port), 0x1);
	}
}

/* The D-PHY's bandgap needs a reset after every power gating. */
static void dsi_band_gap_reset(struct lg_display *d)
{
	lg_vlv_flisdsi_write(d, 0x08, 0x0001);
	lg_vlv_flisdsi_write(d, 0x0f, 0x0005);
	lg_vlv_flisdsi_write(d, 0x0f, 0x0025);
	lg_udelay(150);
	lg_vlv_flisdsi_write(d, 0x0f, 0x0000);
	lg_vlv_flisdsi_write(d, 0x08, 0x0000);
}

/* Bring the link to LP-11: through ULPS into the ready state. */
static void dsi_device_ready(struct lg_display *d, struct lg_dsi *dsi)
{
	int port;

	i915_dbg("[drm] i915: DSI: device ready\n");

	/* rcomp for compliance: 50 down to 45 ohms, after every power gate */
	lg_vlv_flisdsi_write(d, 0x04, 0x0004);

	dsi_band_gap_reset(d);

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_ULPS_STATE_ENTER);
		dsi_sleep_us(2500);

		/* the PHY's transparent latch: one bit for both ports, in
		 * port A's control register */
		lg_rmw(d, MIPI_PORT_CTRL(DSI_PORT_A), 0, MIPI_LP_OUTPUT_HOLD);
		dsi_sleep_us(1000);

		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_ULPS_STATE_EXIT);
		dsi_sleep_us(2500);

		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_DEV_READY);
		dsi_sleep_us(2500);
	}
}

/* Back to LP-00: through ULPS, latch off, not ready. */
static void dsi_clear_device_ready(struct lg_display *d, struct lg_dsi *dsi)
{
	uint32_t port_ctrl = MIPI_PORT_CTRL(DSI_PORT_A); /* shared by A and C */
	int port;

	i915_dbg("[drm] i915: DSI: clear device ready\n");

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_DEV_READY | MIPI_ULPS_STATE_ENTER);
		dsi_sleep_us(2000);

		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_DEV_READY | MIPI_ULPS_STATE_EXIT);
		dsi_sleep_us(2000);

		lg_wr(d, MIPI_DEVICE_READY(port), MIPI_DEV_READY | MIPI_ULPS_STATE_ENTER);
		dsi_sleep_us(2000);

		/* The clock lane reaches LP-00 when the AFE latch drops;
		 * only port A has the bit. */
		if (port == DSI_PORT_A && dsi_wait_clear(d, port_ctrl, MIPI_AFE_LATCHOUT, 30))
			kprintf("[drm] i915: DSI: LP not going low\n");

		/* the PHY's transparent latch off */
		lg_rmw(d, port_ctrl, MIPI_LP_OUTPUT_HOLD, 0);
		dsi_sleep_us(1000);

		lg_wr(d, MIPI_DEVICE_READY(port), 0x00);
		dsi_sleep_us(2000);
	}
}

static void dsi_port_enable(struct lg_display *d, struct lg_dsi *dsi, const struct lg_config *cfg)
{
	int port;

	if (dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
		lg_rmw(d, VLV_DSI_CHICKEN_3, VLV_DSI_PIXEL_OVERLAP_CNT_MASK,
		       (uint32_t)dsi->pixel_overlap << VLV_DSI_PIXEL_OVERLAP_CNT_SHIFT);

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		uint32_t port_ctrl = MIPI_PORT_CTRL(port);
		uint32_t temp = lg_rd(d, port_ctrl);

		temp &= ~MIPI_LANE_CONFIGURATION_MASK;
		temp &= ~MIPI_DUAL_LINK_MODE_MASK;

		if (dsi->ports == ((1u << DSI_PORT_A) | (1u << DSI_PORT_C))) {
			temp |= (uint32_t)(dsi->dual_link - 1) << MIPI_DUAL_LINK_MODE_SHIFT;
			temp |= cfg->pipe ? MIPI_LANE_CONFIGURATION_DUAL_LINK_B :
					    MIPI_LANE_CONFIGURATION_DUAL_LINK_A;
		}

		if (dsi->pixel_format != DSI_FMT_RGB888)
			temp |= MIPI_DITHERING_ENABLE;

		/* assert ip_tg_enable */
		lg_wr(d, port_ctrl, temp | MIPI_DPI_ENABLE);
		lg_posting_read(d, port_ctrl);
	}
}

static void dsi_port_disable(struct lg_display *d, struct lg_dsi *dsi)
{
	int port;

	DSI_FOR_EACH_PORT(port, dsi->ports) {
		uint32_t port_ctrl = MIPI_PORT_CTRL(port);

		/* deassert ip_tg_enable */
		lg_rmw(d, port_ctrl, MIPI_DPI_ENABLE, 0);
		lg_posting_read(d, port_ctrl);
	}
}

/* ---- backlight ------------------------------------------------------------------- */

static void dsi_dcs_set_backlight(struct lg_display *d, struct lg_dsi *dsi, uint32_t level)
{
	uint8_t data[2] = { 0, 0 };
	uint32_t len = dsi->bl_max > 0xff ? 2 : 1;
	int port;

	if (len == 1) {
		data[0] = (uint8_t)level;
	} else {
		data[0] = (uint8_t)(level >> 8);
		data[1] = (uint8_t)level;
	}

	DSI_FOR_EACH_PORT(port, dsi->bl_ports) {
		int lpm = dsi->dev[port].lpm;

		/* brightness goes out in high speed mode */
		dsi->dev[port].lpm = 0;
		dsi_dcs_write(d, dsi, port, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, data, len);
		dsi->dev[port].lpm = lpm;
	}
}

static void dsi_dcs_enable_backlight(struct lg_display *d, struct lg_dsi *dsi)
{
	int port;

	DSI_FOR_EACH_PORT(port, dsi->bl_ports) {
		uint8_t ctrl = 0;

		dsi_dcs_read(d, dsi, port, MIPI_DCS_GET_CONTROL_DISPLAY, &ctrl, 1);
		ctrl |= DCS_CONTROL_DISPLAY_BL | DCS_CONTROL_DISPLAY_DD |
			DCS_CONTROL_DISPLAY_BCTRL;
		dsi_dcs_write(d, dsi, port, MIPI_DCS_WRITE_CONTROL_DISPLAY, &ctrl, 1);
	}

	DSI_FOR_EACH_PORT(port, dsi->cabc_ports) {
		uint8_t cabc = DCS_POWER_SAVE_MEDIUM;

		dsi_dcs_write(d, dsi, port, MIPI_DCS_WRITE_POWER_SAVE, &cabc, 1);
	}

	dsi_dcs_set_backlight(d, dsi, dsi->bl_level);
}

static void dsi_dcs_disable_backlight(struct lg_display *d, struct lg_dsi *dsi)
{
	int port;

	dsi_dcs_set_backlight(d, dsi, 0);

	DSI_FOR_EACH_PORT(port, dsi->cabc_ports) {
		uint8_t cabc = DCS_POWER_SAVE_OFF;

		dsi_dcs_write(d, dsi, port, MIPI_DCS_WRITE_POWER_SAVE, &cabc, 1);
	}

	DSI_FOR_EACH_PORT(port, dsi->bl_ports) {
		uint8_t ctrl = 0;

		dsi_dcs_read(d, dsi, port, MIPI_DCS_GET_CONTROL_DISPLAY, &ctrl, 1);
		ctrl &= (uint8_t)~(DCS_CONTROL_DISPLAY_BL | DCS_CONTROL_DISPLAY_DD |
				   DCS_CONTROL_DISPLAY_BCTRL);
		dsi_dcs_write(d, dsi, port, MIPI_DCS_WRITE_CONTROL_DISPLAY, &ctrl, 1);
	}
}

/*
 * Which backlight the panel has.  A VBT asking for DCS gets brightness
 * commands over the link; one naming the display controller's PWM gets
 * that.  Otherwise the PWM is the PMIC's (on the PMIC's I2C bus) or the
 * SoC's LPSS PWM, neither of which LikeOS can drive: the VBT's backlight
 * sequences and the enable lines still run, the brightness stays where
 * the firmware left it.
 */
static void dsi_backlight_init(struct lg_display *d, struct lg_output *o, struct lg_dsi *dsi)
{
	int ctl_type;

	dsi_parse_backlight(d, dsi, &ctl_type);

	if (ctl_type == VBT_BACKLIGHT_DSI_DCS) {
		dsi->bl = DSI_BL_DCS;
		if (dsi->bl_precision_bits > 8 && dsi->bl_precision_bits <= 16)
			dsi->bl_max = (1u << dsi->bl_precision_bits) - 1;
		else
			dsi->bl_max = DCS_PANEL_PWM_MAX_VALUE;
		dsi->bl_level = dsi->bl_max;
		kprintf("[drm] i915: DSI: using DCS for backlight control (max %u)\n",
			dsi->bl_max);
		return;
	}

	if (ctl_type == VBT_BACKLIGHT_DISPLAY_DDI && d->vbt.version >= 191) {
		if (lg_backlight_setup(d, o, dsi->pipe) == 0) {
			dsi->bl = DSI_BL_DISPLAY_PWM;
			kprintf("[drm] i915: DSI: using the display PWM of pipe %c for backlight\n",
				(char)('A' + dsi->pipe));
			return;
		}
	}

	dsi->bl = DSI_BL_NONE;
	kprintf("[drm] i915: DSI: backlight PWM is the %s, which LikeOS cannot drive; brightness stays as the firmware set it\n",
		dsi->mipi.pwm_blc == VBT_PPS_BLC_PMIC ? "PMIC's (I2C)" : "SoC LPSS PWM");
}

/*
 * The panel and backlight enable lines some boards need driven by hand.
 * With a PMIC backlight the panel enable is a PMIC GPIO (out of reach);
 * Valleyview boards with the SoC PWM use the north core pads 11 (panel)
 * and 10 (backlight).  They start in the panel's current state.
 */
static void dsi_vbt_gpio_init(struct lg_display *d, struct lg_dsi *dsi, int panel_is_on)
{
	dsi->gpio_panel = -1;
	dsi->gpio_backlight = -1;

	if (dsi->mipi.pwm_blc == VBT_PPS_BLC_PMIC) {
		kprintf("[drm] i915: DSI: the panel enable is a PMIC GPIO; not driven (no PMIC support)\n");
		return;
	}

	if (d->is_vlv && dsi->mipi.pwm_blc == VBT_PPS_BLC_SOC) {
		dsi->gpio_panel = 11;
		dsi->gpio_backlight = 10;
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_panel, panel_is_on);
		vlv_dsi_gpio_nc_set(d, dsi, (unsigned int)dsi->gpio_backlight, panel_is_on);
		kprintf("[drm] i915: DSI: the SoC PWM0 pin mux and the LPSS PWM are not programmed (no LPSS support)\n");
	}
}

/* ---- the output's hooks ----------------------------------------------------------- */

static struct lg_dsi *to_dsi(struct lg_output *o)
{
	return (struct lg_dsi *)o->priv;
}

static int dsi_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct drm_mode_modeinfo m;

	if (!o->fixed_mode_valid)
		return 0;
	m = o->fixed_mode;
	m.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	return drm_connector_add_mode(d->drm, conn, &m) < 0 ? 0 : 1;
}

/* The panel shows its own timing; smaller modes are scaled up onto it. */
static int dsi_mode_valid(struct lg_display *d, struct lg_output *o,
			  const struct drm_mode_modeinfo *m)
{
	const struct drm_mode_modeinfo *fixed = &o->fixed_mode;

	if (m->flags & (DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_INTERLACE))
		return -EINVAL;
	if (m->hdisplay > fixed->hdisplay || m->vdisplay > fixed->vdisplay)
		return -EINVAL;
	if (d->max_dotclk_khz && fixed->clock > d->max_dotclk_khz)
		return -EINVAL;
	return 0;
}

static int dsi_compute_config(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	struct lg_dsi *dsi = to_dsi(o);
	uint32_t pclk;
	int ret;

	if (cfg->mode.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	cfg->has_pch_encoder = 0;
	cfg->pipe_bpp = dsi->pixel_format == DSI_FMT_RGB888 ? 24 : 18;

	/* the panel's own timing, the fitter scaling the picture onto it */
	cfg->mode = o->fixed_mode;
	lg_timings_from_mode(&cfg->t, &cfg->mode);
	cfg->t_set = 1;
	if (cfg->src_w != o->fixed_mode.hdisplay || cfg->src_h != o->fixed_mode.vdisplay) {
		ret = lg_pfit_compute(d, o, cfg);
		if (ret)
			return ret;
	}

	/* DSI sends sync events as short packets: no polarities */
	cfg->t.flags = 0;
	cfg->mode.flags = 0;

	ret = dsi_pll_compute(d, dsi, &pclk);
	if (ret)
		return -EINVAL;

	cfg->port_clock = pclk;
	/* not exact for burst or command mode or pixel overlap */
	cfg->t.clock = dsi->dual_link ? pclk * 2 : pclk;
	/* the pipe's DPLL only feeds its reference input (VCO off) */
	cfg->clock_set = 1;
	mm_memset(&cfg->dpll, 0, sizeof(cfg->dpll));
	return 0;
}

/*
 * Panel enable, from the VBT spec (v2 VBTs leave some steps to the
 * driver; v3 has sequences for them):
 *
 *   power on (MIPIPanelPowerOn), wait t1+t2, deassert reset, lines to
 *   LP-11, MIPISendInitialDcsCmds, [command mode: TearOn, DisplayOn],
 *   turn on DPI, MIPIDisplayOn, wait t5, backlight on
 *
 * and in reverse for disable.  The port is enabled here, before the pipe
 * and plane, as the hardware wants.
 */
static void dsi_pre_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct lg_dsi *dsi = to_dsi(o);
	int port;

	i915_dbg("[drm] i915: DSI: pre-enable, pipe %c\n", (char)('A' + cfg->pipe));

	/* The panel must stay off for its power cycle delay.  LikeOS has
	 * no clock to measure how long it was off, so the full delay. */
	dsi_sleep_ms(dsi->panel_pwr_cycle_delay);

	/* the firmware may leave the PLL unable to lock: power it down first */
	dsi_pll_disable(d);
	dsi_pll_enable(d, dsi);

	/* DPO unit clock gating can stall the pipe */
	lg_rmw(d, DSPCLK_GATE_D, 0, DPOUNIT_CLOCK_GATE_DISABLE);

	dsi_prepare(d, dsi, cfg);

	/* power the panel on, give it time, release its reset */
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_POWER_ON);
	dsi_sleep_ms(dsi->panel_on_delay);
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_DEASSERT_RESET);

	/* the link to LP-11 */
	dsi_device_ready(d, dsi);

	/* initialisation commands, in LP mode */
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_INIT_OTP);

	if (dsi_is_cmd_mode(dsi)) {
		DSI_FOR_EACH_PORT(port, dsi->ports)
			lg_wr(d, MIPI_MAX_RETURN_PKT_SIZE(port), 8 * 4);
		dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_TEAR_ON);
		dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_DISPLAY_ON);
	} else {
		dsi_sleep_ms(20);
		DSI_FOR_EACH_PORT(port, dsi->ports)
			dsi_dpi_send_cmd(d, MIPI_DPI_TURN_ON, 0, port);
		dsi_sleep_ms(100);

		dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_DISPLAY_ON);

		dsi_port_enable(d, dsi, cfg);
	}
}

static void dsi_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct lg_dsi *dsi = to_dsi(o);

	if (dsi->bl == DSI_BL_DISPLAY_PWM)
		lg_backlight_enable(d, o, cfg);
	else if (dsi->bl == DSI_BL_DCS)
		dsi_dcs_enable_backlight(d, dsi);

	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_BACKLIGHT_ON);
}

static void dsi_disable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct lg_dsi *dsi = to_dsi(o);
	int port;

	(void)cfg;
	i915_dbg("[drm] i915: DSI: disable\n");

	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_BACKLIGHT_OFF);
	if (dsi->bl == DSI_BL_DISPLAY_PWM)
		lg_backlight_disable(d, o);
	else if (dsi->bl == DSI_BL_DCS)
		dsi_dcs_disable_backlight(d, dsi);

	/* The spec sends SHUTDOWN before DisplayOff only for v3 VBTs, but
	 * that order works for v2 ones too. */
	if (dsi_is_vid_mode(dsi)) {
		DSI_FOR_EACH_PORT(port, dsi->ports)
			dsi_dpi_send_cmd(d, MIPI_DPI_SHUTDOWN, 0, port);
		dsi_sleep_ms(10);
	}
}

static void dsi_post_disable(struct lg_display *d, struct lg_output *o,
			     const struct lg_config *cfg)
{
	struct lg_dsi *dsi = to_dsi(o);
	int port;

	(void)cfg;
	i915_dbg("[drm] i915: DSI: post-disable\n");

	if (dsi_is_vid_mode(dsi)) {
		DSI_FOR_EACH_PORT(port, dsi->ports)
			dsi_wait_for_fifo_empty(d, port);

		dsi_port_disable(d, dsi);
		dsi_sleep_us(2000);
	}

	dsi_unprepare(d, dsi);

	/* disable packets before the shutdown packet make the next turn
	 * on packet fail */
	if (dsi_is_cmd_mode(dsi))
		dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_TEAR_OFF);
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_DISPLAY_OFF);

	/* the link to LP-00 */
	dsi_clear_device_ready(d, dsi);

	dsi_pll_disable(d);
	lg_rmw(d, DSPCLK_GATE_D, DPOUNIT_CLOCK_GATE_DISABLE, 0);

	/* reset the panel and power it off */
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_ASSERT_RESET);
	dsi_sleep_ms(dsi->panel_off_delay);
	dsi_vbt_exec_sequence(d, dsi, MIPI_SEQ_POWER_OFF);
}

static int dsi_hw_state(struct lg_display *d, struct lg_dsi *dsi, int *pipe)
{
	int port;

	/* only one DSI output */
	DSI_FOR_EACH_PORT(port, dsi->ports) {
		int enabled = !!(lg_rd(d, MIPI_PORT_CTRL(port)) & MIPI_DPI_ENABLE);

		/* port C's DPI enable bit does not read back set; pipe B's
		 * state stands in for it */
		if (port == DSI_PORT_C)
			enabled = !!(lg_rd(d, PIPECONF(d, 1)) & PIPECONF_ENABLE);

		/* command mode, if video mode is not on */
		if (!enabled)
			enabled = !!(lg_rd(d, MIPI_DSI_FUNC_PRG(port)) &
				     MIPI_CMD_MODE_DATA_WIDTH_MASK);
		if (!enabled)
			continue;

		if (!(lg_rd(d, MIPI_DEVICE_READY(port)) & MIPI_DEV_READY))
			continue;

		if (pipe)
			*pipe = port == DSI_PORT_A ? 0 : 1;
		return 1;
	}
	return 0;
}

static int dsi_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	return dsi_hw_state(d, to_dsi(o), pipe);
}

static const struct lg_output_funcs dsi_funcs = {
	.detect = NULL, /* a panel: always there */
	.get_modes = dsi_get_modes,
	.mode_valid = dsi_mode_valid,
	.compute_config = dsi_compute_config,
	.pre_enable = dsi_pre_enable,
	.enable = dsi_enable,
	.disable = dsi_disable,
	.post_disable = dsi_post_disable,
	.get_hw_state = dsi_get_hw_state,
};

/* ---- init ------------------------------------------------------------------------- */

static void dsi_free(struct lg_dsi *dsi)
{
	if (!dsi)
		return;
	if (dsi->seq_data)
		kfree(dsi->seq_data);
	if (dsi->deassert_seq)
		kfree(dsi->deassert_seq);
	kfree(dsi);
}

void lg_dsi_init(struct lg_display *d)
{
	struct lg_dsi *dsi;
	struct lg_output *o;
	int port = -1, pipe, hw_pipe = -1, panel_on;

	if (!d->is_vlv && !d->is_chv)
		return;

	/* nothing on the link says a panel is there: the VBT does */
	if (!lg_vbt_dsi_present(d, &port))
		return;

	if (port != DSI_PORT_A && port != DSI_PORT_C) {
		kprintf("[drm] i915: DSI: VBT names unsupported DSI port %d\n", port);
		return;
	}

	if (!d->vbt.lfp_mode_valid) {
		kprintf("[drm] i915: DSI: no panel mode in the VBT\n");
		return;
	}

	dsi = kalloc(sizeof(*dsi));
	if (!dsi)
		return;
	mm_memset(dsi, 0, sizeof(*dsi));

	/* pipe A drives port A, pipe B port C */
	pipe = port == DSI_PORT_A ? 0 : 1;
	dsi->port = port;
	dsi->pipe = pipe;
	dsi->gpio_panel = -1;
	dsi->gpio_backlight = -1;

	if (dsi_parse_mipi_config(d, dsi))
		goto err;

	if (dsi->mipi.dual_link)
		dsi->ports = (1u << DSI_PORT_A) | (1u << DSI_PORT_C);
	else
		dsi->ports = (uint16_t)(1u << port);

	dsi->bl_ports &= dsi->ports;
	dsi->cabc_ports &= dsi->ports;

	dsi_parse_mipi_sequence(d, dsi);

	if (dsi_vbt_init(d, dsi, &d->vbt.lfp_mode)) {
		kprintf("[drm] i915: DSI: no usable panel configuration\n");
		goto err;
	}

	/* A panel the firmware lit: keep its pixel clock when it is close
	 * to ours (fast boot). */
	panel_on = dsi_hw_state(d, dsi, &hw_pipe);
	if (panel_on) {
		uint32_t pll_ctl = lg_vlv_cck_read(d, CCK_DSI_PLL_CONTROL);
		uint32_t pll_div = lg_vlv_cck_read(d, CCK_DSI_PLL_DIVIDER);
		uint32_t gop = dsi_pclk_from_pll(d, dsi, pll_ctl & ~CCK_DSI_PLL_LOCK, pll_div);

		if (dsi->dual_link)
			gop *= 2;
		i915_dbg("[drm] i915: DSI: calculated pclk %u, GOP %u\n", dsi->pclk, gop);
		if (dsi_fuzzy_clock_check((int)dsi->pclk, (int)gop)) {
			i915_dbg("[drm] i915: DSI: using the GOP pclk\n");
			dsi->pclk = gop;
		}
	}

	dsi_dphy_param_init(dsi);

	o = lg_output_new(d);
	if (!o)
		goto err;

	o->type = LG_OUTPUT_DSI;
	o->port = port;
	o->reg = MIPI_PORT_CTRL(port);
	ksnprintf(o->name, sizeof(o->name), "DSI");
	o->funcs = &dsi_funcs;
	o->conn_type = DRM_MODE_CONNECTOR_DSI;
	o->enc_type = DRM_MODE_ENCODER_DSI;
	o->pipe_mask = 1u << pipe;
	o->hpd_pin = LG_HPD_NONE;
	o->polled = 0;
	o->is_panel = 1;
	o->fixed_mode = d->vbt.lfp_mode;
	o->fixed_mode.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	o->fixed_mode_valid = 1;
	o->mm_width = d->vbt.lfp_width_mm;
	o->mm_height = d->vbt.lfp_height_mm;
	o->priv = dsi;

	dsi_vbt_gpio_init(d, dsi, panel_on);
	dsi_backlight_init(d, o, dsi);

	kprintf("[drm] i915: found DSI panel on port %c%s (pipe %c): %ux%u, %u lane%s, %s mode, pclk %u kHz\n",
		dsi_port_name(port), dsi->dual_link ? " (dual link)" : "",
		(char)('A' + pipe), o->fixed_mode.hdisplay, o->fixed_mode.vdisplay,
		dsi->lane_count, dsi->lane_count == 1 ? "" : "s",
		dsi_is_cmd_mode(dsi) ? "command" : "video", dsi->pclk);
	if (!dsi->seq_data)
		kprintf("[drm] i915: DSI: no MIPI sequences: the panel may not power up\n");
	return;

err:
	dsi_free(dsi);
}
