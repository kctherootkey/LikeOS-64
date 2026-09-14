// LikeOS-64 -- the DDI ports of Skylake.
//
// A DDI is a digital output that carries DisplayPort or HDMI depending on
// what the board wires to it.  Bringing one up for DisplayPort: route
// its clock to a PLL, program the buffer translation (voltage swing and
// pre-emphasis for the link training level), enable the buffer, then run
// the link training through the DP_TP control register's pattern bits
// and the sink's DPCD (intel_dp.c).  The transcoder is attached to the
// port through TRANS_DDI_FUNC_CTL by the pipe code (intel_display.c).
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* Buffer translations: each entry is the pair written to
 * DDI_BUF_TRANS_LO/HI for one voltage-swing/pre-emphasis level.  Skylake
 * DisplayPort (H/S parts, 1.35V), from the platform's tuning tables. */
struct buf_trans {
	uint32_t lo, hi;
};

static const struct buf_trans skl_dp[] = {
	{ 0x00002016, 0x000000a0 }, /* 400mV 0dB */
	{ 0x00005012, 0x0000009b }, /* 400mV 3.5dB */
	{ 0x00007011, 0x00000088 }, /* 400mV 6dB */
	{ 0x80009010, 0x000000c0 }, /* 400mV 9.5dB */
	{ 0x00002016, 0x0000009b }, /* 600mV 0dB */
	{ 0x00005012, 0x00000088 }, /* 600mV 3.5dB */
	{ 0x80007011, 0x000000c0 }, /* 600mV 6dB */
	{ 0x00002016, 0x000000df }, /* 800mV 0dB */
	{ 0x80005012, 0x000000c0 }, /* 800mV 3.5dB */
};

/* Low-voltage-swing eDP panels (VBT says so). */
static const struct buf_trans skl_edp[] = {
	{ 0x00000018, 0x000000a8 }, /* 200mV 0dB */
	{ 0x00004013, 0x000000a9 }, /* 200mV 1.5dB */
	{ 0x00007011, 0x000000a2 }, /* 200mV 4dB */
	{ 0x00009010, 0x0000009c }, /* 200mV 6dB */
	{ 0x00000018, 0x000000a9 }, /* 250mV 0dB */
	{ 0x00006013, 0x000000a2 }, /* 250mV 1.5dB */
	{ 0x00007011, 0x000000a6 }, /* 250mV 4dB */
	{ 0x00000018, 0x000000ab }, /* 300mV 0dB */
	{ 0x00007013, 0x0000009f }, /* 300mV 1.5dB */
	{ 0x00000018, 0x000000df }, /* 350mV 0dB */
};

static const struct buf_trans skl_hdmi[] = {
	{ 0x00000018, 0x000000ac },
	{ 0x00005012, 0x0000009d },
	{ 0x00007011, 0x00000088 },
	{ 0x00000018, 0x000000a1 },
	{ 0x00000018, 0x00000098 },
	{ 0x00004013, 0x00000088 },
	{ 0x80006012, 0x000000cd },
	{ 0x00000018, 0x000000df },
	{ 0x80003015, 0x000000cd },
	{ 0x80003015, 0x000000c0 },
	{ 0x80000018, 0x000000c0 },
};

/* The current boost that goes with each HDMI translation entry. */
static const uint8_t skl_hdmi_iboost[] = { 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1 };
#define SKL_HDMI_DEFAULT_LEVEL 8

/* The transmitter takes its HDMI swing from translation entry 9; the
 * boost of the chosen level goes to the balance-leg register, or the
 * leg is switched off when the level has none. */
void intel_ddi_prepare_hdmi(struct i915_device *i915, struct intel_output *o,
			    int level)
{
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW:
		bdw_ddi_set_buf_trans(i915, o, level);
		return;
	case INTEL_DISPLAY_BXT:
		bxt_phy_set_signal_level(i915, o, level);
		return;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
		if (o->is_tc) {
			intel_tc_set_signal_level(i915, o, level);
		} else {
			icl_combo_phy_init(i915, o->port);
			icl_combo_phy_power_lanes(i915, o->port, 4, o->lane_reversal);
			icl_combo_set_signal_level(i915, o, level, 4);
		}
		return;
	default:
		break;
	}
	int n = (int)(sizeof(skl_hdmi) / sizeof(skl_hdmi[0]));
	if (level < 0 || level >= n)
		level = SKL_HDMI_DEFAULT_LEVEL;
	uint32_t iboost = skl_hdmi_iboost[level];
	i915_write32(i915, DDI_BUF_TRANS_LO(o->port, 9), skl_hdmi[level].lo);
	i915_write32(i915, DDI_BUF_TRANS_HI(o->port, 9), skl_hdmi[level].hi);
	uint32_t v = i915_read32(i915, DISPIO_CR_TX_BMU_CR0);
	v &= ~(BALANCE_LEG_MASK(o->port) | BALANCE_LEG_DISABLE(o->port));
	if (iboost)
		v |= iboost << BALANCE_LEG_SHIFT(o->port);
	else
		v |= BALANCE_LEG_DISABLE(o->port);
	i915_write32(i915, DISPIO_CR_TX_BMU_CR0, v);
	(void)i915_read32(i915, DISPIO_CR_TX_BMU_CR0);
}

/* The translation index for a swing/pre-emphasis pair, DP: the table is
 * ordered 400mV x 4, 600mV x 3, 800mV x 2, 1200mV x 1. */
static int dp_level(int swing, int preemph)
{
	static const int base[4] = { 0, 4, 7, 9 };
	int lvl;
	if (swing > 3)
		swing = 3;
	if (preemph > 3)
		preemph = 3;
	/* no combination beyond 9.5 dB in total */
	if (swing + preemph > 3)
		preemph = 3 - swing;
	lvl = base[swing] + preemph;
	return lvl > 8 ? 8 : lvl;
}

void intel_ddi_set_buf_trans(struct i915_device *i915, struct intel_output *o,
			     int level)
{
	const struct buf_trans *t;
	unsigned n;

	/* The other display models drive their PHYs directly: the level
	 * is written into the PHY, not picked from a DDI table. */
	switch (i915->display.model) {
	case INTEL_DISPLAY_BDW:
		bdw_ddi_set_buf_trans(i915, o, level);
		return;
	case INTEL_DISPLAY_BXT:
		bxt_phy_set_signal_level(i915, o, level);
		return;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
		if (o->is_tc)
			intel_tc_set_signal_level(i915, o, level);
		else
			icl_combo_set_signal_level(i915, o, level, o->lane_count ? o->lane_count : 4);
		return;
	default:
		break;
	}
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI) {
		t = skl_hdmi;
		n = sizeof(skl_hdmi) / sizeof(skl_hdmi[0]);
		if (level < 0 || (unsigned)level >= n)
			level = 8; /* the platform default */
	} else if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std) {
		t = skl_edp;
		n = sizeof(skl_edp) / sizeof(skl_edp[0]);
	} else {
		t = skl_dp;
		n = sizeof(skl_dp) / sizeof(skl_dp[0]);
	}
	/* The whole table goes in; the buffer control register then picks
	 * the entry by level. */
	for (unsigned i = 0; i < n && i < 10; i++) {
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, (int)i), t[i].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, (int)i), t[i].hi);
	}
	/* entry 9 doubles as the HDMI default on ports that carry both */
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
		v &= ~DDI_BUF_EMP_MASK;
		v |= DDI_BUF_TRANS_SELECT((uint32_t)level);
		i915_write32(i915, DDI_BUF_CTL(o->port), v);
	}
}

/* Translate the sink's request (swing/pre-emphasis per lane, from the
 * ADJUST_REQUEST registers) into the level written. */
int intel_ddi_dp_level_for(int swing, int preemph)
{
	return dp_level(swing, preemph);
}

static uint32_t ddi_port_width(int lanes)
{
	return DDI_PORT_WIDTH((uint32_t)lanes);
}

/* Before the transcoder: clock, translations, buffer on, training
 * pattern 1 sent while the sink is told to expect it (intel_dp.c
 * handles the DPCD side and calls back here through the DP_TP helpers). */
void intel_ddi_pre_enable(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *mode)
{
	(void)mode;
	if (i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL) {
		if (o->is_tc) {
			intel_tc_program_dp_mode(i915, o, o->lane_count);
		} else {
			icl_combo_phy_init(i915, o->port);
			icl_combo_phy_power_lanes(i915, o->port, o->lane_count, o->lane_reversal);
		}
	}
	intel_dpll_route_port(i915, o->port, o->pll);
	intel_ddi_set_buf_trans(i915, o, 0);
	intel_tc_clock_gating(i915, o, 0);
}

/* Whether the level goes through the DDI's translation select (the
 * Skylake and Broadwell way) or straight into a PHY. */
int intel_ddi_level_in_buf_ctl(struct i915_device *i915)
{
	return i915->display.model == INTEL_DISPLAY_SKL || i915->display.model == INTEL_DISPLAY_BDW;
}

/* DP_TP: the training pattern the port transmits. */
void intel_ddi_set_train_pattern(struct i915_device *i915, struct intel_output *o,
				 uint32_t pattern)
{
	uint32_t v = i915_read32(i915, intel_dp_tp_ctl_reg(i915, o));
	v &= ~DP_TP_CTL_LINK_TRAIN_MASK;
	v |= pattern;
	i915_write32(i915, intel_dp_tp_ctl_reg(i915, o), v);
	(void)i915_read32(i915, intel_dp_tp_ctl_reg(i915, o));
}

void intel_ddi_dp_tp_enable(struct i915_device *i915, struct intel_output *o,
			    int enhanced_framing)
{
	uint32_t v = DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST |
		     DP_TP_CTL_LINK_TRAIN_PAT1;
	if (enhanced_framing)
		v |= DP_TP_CTL_ENHANCED_FRAME_ENABLE;
	i915_write32(i915, intel_dp_tp_ctl_reg(i915, o), v);
	(void)i915_read32(i915, intel_dp_tp_ctl_reg(i915, o));
}

void intel_ddi_buf_enable(struct i915_device *i915, struct intel_output *o,
			  int lanes, int level)
{
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	v &= ~(DDI_PORT_WIDTH_MASK | DDI_BUF_EMP_MASK | DDI_BUF_PORT_REVERSAL);
	v |= DDI_BUF_CTL_ENABLE | ddi_port_width(lanes) |
	     DDI_BUF_TRANS_SELECT((uint32_t)level);
	if (o->port == PORT_A && lanes == 4)
		v |= DDI_A_4_LANES;
	i915_write32(i915, DDI_BUF_CTL(o->port), v);
	(void)i915_read32(i915, DDI_BUF_CTL(o->port));
	/* the buffer takes a moment to leave idle */
	for (int t = 0; t < 100; t++) {
		if (!(i915_read32(i915, DDI_BUF_CTL(o->port)) & DDI_BUF_IS_IDLE))
			break;
		lapic_delay_us(10);
	}
	lapic_delay_us(600);
}

void intel_ddi_buf_disable(struct i915_device *i915, struct intel_output *o)
{
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	if (v & DDI_BUF_CTL_ENABLE) {
		i915_write32(i915, DDI_BUF_CTL(o->port), v & ~DDI_BUF_CTL_ENABLE);
		(void)i915_read32(i915, DDI_BUF_CTL(o->port));
		for (int t = 0; t < 100; t++) {
			if (i915_read32(i915, DDI_BUF_CTL(o->port)) & DDI_BUF_IS_IDLE)
				break;
			lapic_delay_us(10);
		}
	}
	uint32_t tp = i915_read32(i915, intel_dp_tp_ctl_reg(i915, o));
	if (tp & DP_TP_CTL_ENABLE) {
		tp &= ~(DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_MASK);
		tp |= DP_TP_CTL_LINK_TRAIN_PAT1;
		i915_write32(i915, intel_dp_tp_ctl_reg(i915, o), tp);
		(void)i915_read32(i915, intel_dp_tp_ctl_reg(i915, o));
	}
}

/* Wait for the port to report it is sending idle patterns after
 * training, before the transcoder goes on. */
int intel_ddi_wait_idle_done(struct i915_device *i915, struct intel_output *o)
{
	/* The eDP port A has no idle-done status. */
	if (o->port == PORT_A) {
		lapic_delay_us(500);
		return 0;
	}
	for (int t = 0; t < 1000; t++) {
		if (i915_read32(i915, intel_dp_tp_status_reg(i915, o)) & DP_TP_STATUS_IDLE_DONE)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

void intel_ddi_enable(struct i915_device *i915, struct intel_output *o)
{
	/* For DP the link is already trained and sending the normal
	 * pattern; nothing more to do at the port. */
	(void)i915;
	(void)o;
}

void intel_ddi_disable(struct i915_device *i915, struct intel_output *o)
{
	intel_ddi_buf_disable(i915, o);
}

void intel_ddi_post_disable(struct i915_device *i915, struct intel_output *o)
{
	intel_tc_clock_gating(i915, o, 1);
	intel_dpll_unroute_port(i915, o->port);
	if (o->pll >= 0) {
		intel_dpll_put(i915, o->pll);
		o->pll = -1;
	}
}

/* Which DDIs the board strapped (B-D; A and E are always there where the
 * VBT says so). */
int intel_ddi_init(struct i915_device *i915)
{
	uint32_t strap = i915_read32(i915, SFUSE_STRAP);
	i915_dbg("[drm] i915: DDI straps: B%s C%s D%s\n",
		(strap & SFUSE_STRAP_DDIB_DETECTED) ? "+" : "-",
		(strap & SFUSE_STRAP_DDIC_DETECTED) ? "+" : "-",
		(strap & SFUSE_STRAP_DDID_DETECTED) ? "+" : "-");
	return 0;
}
