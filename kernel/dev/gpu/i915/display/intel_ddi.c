// LikeOS -- the DDI ports of Skylake.
//
// A DDI is a digital output that carries DisplayPort or HDMI depending on
// what the board wires to it.  Bringing one up for DisplayPort: route
// its clock to a PLL, program the buffer translation (voltage swing and
// pre-emphasis for the link training level), enable the buffer, then run
// the link training through the DP_TP control register's pattern bits
// and the sink's DPCD (intel_dp.c).  The transcoder is attached to the
// port through TRANS_DDI_FUNC_CTL by the pipe code (intel_display.c).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* Buffer translations: each entry is the pair written to
 * DDI_BUF_TRANS_LO/HI for one voltage-swing/pre-emphasis level, plus the
 * current boost (the balance leg) that goes with it.  Skylake and its
 * successors up to Comet Lake each come in three tunings -- desktop and
 * H parts, U parts and Y parts -- and the eDP tables are for panels the
 * VBT marks as low voltage swing. */
struct buf_trans {
	uint32_t lo, hi;
	uint8_t iboost;
};

/* Skylake H and S */
static const struct buf_trans skl_dp[] = {
	{ 0x00002016, 0x000000A0, 0 }, { 0x00005012, 0x0000009B, 0 },
	{ 0x00007011, 0x00000088, 0 }, { 0x80009010, 0x000000C0, 1 },
	{ 0x00002016, 0x0000009B, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000C0, 1 }, { 0x00002016, 0x000000DF, 0 },
	{ 0x80005012, 0x000000C0, 1 },
};
/* Skylake U */
static const struct buf_trans skl_u_dp[] = {
	{ 0x0000201B, 0x000000A2, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000CD, 1 }, { 0x80009010, 0x000000C0, 1 },
	{ 0x0000201B, 0x0000009D, 0 }, { 0x80005012, 0x000000C0, 1 },
	{ 0x80007011, 0x000000C0, 1 }, { 0x00002016, 0x00000088, 0 },
	{ 0x80005012, 0x000000C0, 1 },
};
/* Skylake Y */
static const struct buf_trans skl_y_dp[] = {
	{ 0x00000018, 0x000000A2, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000CD, 3 }, { 0x80009010, 0x000000C0, 3 },
	{ 0x00000018, 0x0000009D, 0 }, { 0x80005012, 0x000000C0, 3 },
	{ 0x80007011, 0x000000C0, 3 }, { 0x00000018, 0x00000088, 0 },
	{ 0x80005012, 0x000000C0, 3 },
};
/* Kaby Lake (and Coffee/Comet Lake) H and S */
static const struct buf_trans kbl_dp[] = {
	{ 0x00002016, 0x000000A0, 0 }, { 0x00005012, 0x0000009B, 0 },
	{ 0x00007011, 0x00000088, 0 }, { 0x80009010, 0x000000C0, 1 },
	{ 0x00002016, 0x0000009B, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000C0, 1 }, { 0x00002016, 0x00000097, 0 },
	{ 0x80005012, 0x000000C0, 1 },
};
/* Kaby Lake U */
static const struct buf_trans kbl_u_dp[] = {
	{ 0x0000201B, 0x000000A1, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000CD, 3 }, { 0x80009010, 0x000000C0, 3 },
	{ 0x0000201B, 0x0000009D, 0 }, { 0x80005012, 0x000000C0, 3 },
	{ 0x80007011, 0x000000C0, 3 }, { 0x00002016, 0x0000004F, 0 },
	{ 0x80005012, 0x000000C0, 3 },
};
/* Kaby Lake Y */
static const struct buf_trans kbl_y_dp[] = {
	{ 0x00001017, 0x000000A1, 0 }, { 0x00005012, 0x00000088, 0 },
	{ 0x80007011, 0x000000CD, 3 }, { 0x8000800F, 0x000000C0, 3 },
	{ 0x00001017, 0x0000009D, 0 }, { 0x80005012, 0x000000C0, 3 },
	{ 0x80007011, 0x000000C0, 3 }, { 0x00001017, 0x0000004C, 0 },
	{ 0x80005012, 0x000000C0, 3 },
};
/* Low-voltage-swing eDP: H and S, U, Y */
static const struct buf_trans skl_edp[] = {
	{ 0x00000018, 0x000000A8, 0 }, { 0x00004013, 0x000000A9, 0 },
	{ 0x00007011, 0x000000A2, 0 }, { 0x00009010, 0x0000009C, 0 },
	{ 0x00000018, 0x000000A9, 0 }, { 0x00006013, 0x000000A2, 0 },
	{ 0x00007011, 0x000000A6, 0 }, { 0x00000018, 0x000000AB, 0 },
	{ 0x00007013, 0x0000009F, 0 }, { 0x00000018, 0x000000DF, 0 },
};
static const struct buf_trans skl_u_edp[] = {
	{ 0x00000018, 0x000000A8, 0 }, { 0x00004013, 0x000000A9, 0 },
	{ 0x00007011, 0x000000A2, 0 }, { 0x00009010, 0x0000009C, 0 },
	{ 0x00000018, 0x000000A9, 0 }, { 0x00006013, 0x000000A2, 0 },
	{ 0x00007011, 0x000000A6, 0 }, { 0x00002016, 0x000000AB, 0 },
	{ 0x00005013, 0x0000009F, 0 }, { 0x00000018, 0x000000DF, 0 },
};
static const struct buf_trans skl_y_edp[] = {
	{ 0x00000018, 0x000000A8, 0 }, { 0x00004013, 0x000000AB, 0 },
	{ 0x00007011, 0x000000A4, 0 }, { 0x00009010, 0x000000DF, 0 },
	{ 0x00000018, 0x000000AA, 0 }, { 0x00006013, 0x000000A4, 0 },
	{ 0x00007011, 0x0000009D, 0 }, { 0x00000018, 0x000000A0, 0 },
	{ 0x00006012, 0x000000DF, 0 }, { 0x00000018, 0x0000008A, 0 },
};
/* HDMI: U, H and S share one table; Y has its own.  Entry 8 is the
 * default level. */
static const struct buf_trans skl_hdmi[] = {
	{ 0x00000018, 0x000000AC, 0 }, { 0x00005012, 0x0000009D, 0 },
	{ 0x00007011, 0x00000088, 0 }, { 0x00000018, 0x000000A1, 0 },
	{ 0x00000018, 0x00000098, 0 }, { 0x00004013, 0x00000088, 0 },
	{ 0x80006012, 0x000000CD, 1 }, { 0x00000018, 0x000000DF, 0 },
	{ 0x80003015, 0x000000CD, 1 }, { 0x80003015, 0x000000C0, 1 },
	{ 0x80000018, 0x000000C0, 1 },
};
static const struct buf_trans skl_y_hdmi[] = {
	{ 0x00000018, 0x000000A1, 0 }, { 0x00005012, 0x000000DF, 0 },
	{ 0x80007011, 0x000000CB, 3 }, { 0x00000018, 0x000000A4, 0 },
	{ 0x00000018, 0x0000009D, 0 }, { 0x00004013, 0x00000080, 0 },
	{ 0x80006013, 0x000000C0, 3 }, { 0x00000018, 0x0000008A, 0 },
	{ 0x80003015, 0x000000C0, 3 }, { 0x80003015, 0x000000C0, 3 },
	{ 0x80000018, 0x000000C0, 3 },
};
#define SKL_HDMI_DEFAULT_LEVEL 8
#define N_ENTRIES(t) ((int)(sizeof(t) / sizeof((t)[0])))

/* Which tuning a part gets: by its device id, as the parts are sold. */
static const uint16_t skl_ult_ids[] = { 0x1906, 0x1913, 0x1916, 0x1921, 0x1923, 0x1926, 0x1927 };
static const uint16_t skl_ulx_ids[] = { 0x190e, 0x1915, 0x191e };
static const uint16_t kbl_ult_ids[] = {
	0x5906, 0x5913, 0x5916, 0x5921, 0x5926,
	/* Coffee Lake U and Whiskey Lake, Comet Lake U */
	0x3ea9, 0x3ea5, 0x3ea6, 0x3ea7, 0x3ea8, 0x3ea1, 0x3ea4, 0x3ea0, 0x3ea3, 0x3ea2,
	0x9b21, 0x9baa, 0x9bac, 0x9b41, 0x9bca, 0x9bcc,
};
static const uint16_t kbl_ulx_ids[] = {
	0x590e, 0x5915, 0x591e, 0x591c, 0x87c0,
	0x87ca, /* Amber Lake on Coffee Lake */
};

static int devid_in(struct i915_device *i915, const uint16_t *ids, int n)
{
	for (int i = 0; i < n; i++)
		if (i915->devid == ids[i])
			return 1;
	return 0;
}

struct skl_tables {
	const struct buf_trans *dp, *edp, *hdmi;
	int ndp, nedp, nhdmi;
};

#define SKL_TABLES(d, e, h) { d, e, h, N_ENTRIES(d), N_ENTRIES(e), N_ENTRIES(h) }

static struct skl_tables skl_tables_for(struct i915_device *i915)
{
	static const struct skl_tables skl = SKL_TABLES(skl_dp, skl_edp, skl_hdmi);
	static const struct skl_tables skl_u = SKL_TABLES(skl_u_dp, skl_u_edp, skl_hdmi);
	static const struct skl_tables skl_y = SKL_TABLES(skl_y_dp, skl_y_edp, skl_y_hdmi);
	static const struct skl_tables kbl = SKL_TABLES(kbl_dp, skl_edp, skl_hdmi);
	static const struct skl_tables kbl_u = SKL_TABLES(kbl_u_dp, skl_u_edp, skl_hdmi);
	static const struct skl_tables kbl_y = SKL_TABLES(kbl_y_dp, skl_y_edp, skl_y_hdmi);

	if (i915->info->platform == I915_PLATFORM_SKYLAKE) {
		if (devid_in(i915, skl_ulx_ids, N_ENTRIES(skl_ulx_ids)))
			return skl_y;
		if (devid_in(i915, skl_ult_ids, N_ENTRIES(skl_ult_ids)))
			return skl_u;
		return skl;
	}
	if (devid_in(i915, kbl_ulx_ids, N_ENTRIES(kbl_ulx_ids)))
		return kbl_y;
	if (devid_in(i915, kbl_ult_ids, N_ENTRIES(kbl_ult_ids)))
		return kbl_u;
	return kbl;
}

/* The balance leg: the boost of the chosen entry, or off. */
static void skl_set_iboost(struct i915_device *i915, int port, uint8_t iboost)
{
	uint32_t v = i915_read32(i915, DISPIO_CR_TX_BMU_CR0);
	v &= ~(BALANCE_LEG_MASK(port) | BALANCE_LEG_DISABLE(port));
	if (iboost)
		v |= (uint32_t)iboost << BALANCE_LEG_SHIFT(port);
	else
		v |= BALANCE_LEG_DISABLE(port);
	i915_write32(i915, DISPIO_CR_TX_BMU_CR0, v);
	(void)i915_read32(i915, DISPIO_CR_TX_BMU_CR0);
}

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
			icl_combo_set_signal_level(i915, o, level, 4);
			icl_combo_phy_power_lanes(i915, o->port, 4, o->lane_reversal);
		}
		return;
	case INTEL_DISPLAY_MTL:
		mtl_phy_set_signal_level(i915, o, level);
		return;
	case INTEL_DISPLAY_DG2:
		dg2_phy_set_signal_level(i915, o, level);
		return;
	default:
		break;
	}
	struct skl_tables tt = skl_tables_for(i915);
	if (level < 0 || level >= tt.nhdmi)
		level = SKL_HDMI_DEFAULT_LEVEL;
	i915_write32(i915, DDI_BUF_TRANS_LO(o->port, 9), tt.hdmi[level].lo);
	i915_write32(i915, DDI_BUF_TRANS_HI(o->port, 9), tt.hdmi[level].hi);
	skl_set_iboost(i915, o->port, tt.hdmi[level].iboost);
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
	return lvl;
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
	case INTEL_DISPLAY_MTL:
		mtl_phy_set_signal_level(i915, o, level);
		return;
	case INTEL_DISPLAY_DG2:
		dg2_phy_set_signal_level(i915, o, level);
		return;
	default:
		break;
	}
	struct skl_tables tt = skl_tables_for(i915);
	int hdmi = (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI);
	if (hdmi) {
		t = tt.hdmi;
		n = (unsigned)tt.nhdmi;
		if (level < 0 || (unsigned)level >= n)
			level = SKL_HDMI_DEFAULT_LEVEL;
	} else if (o->is_edp && i915->display.vbt.edp_low_vswing && !o->swing_table_std) {
		t = tt.edp;
		n = (unsigned)tt.nedp;
	} else {
		t = tt.dp;
		n = (unsigned)tt.ndp;
	}
	/* Only DDI A and E can select a tenth entry for DisplayPort. */
	if (!hdmi && o->port != PORT_A && o->port != 4 && n > 9)
		n = 9;
	if (level < 0)
		level = 0;
	if ((unsigned)level >= n)
		level = (int)n - 1;
	/* The whole table goes in; the buffer control register then picks
	 * the entry by level. */
	for (unsigned i = 0; i < n && i < 10; i++) {
		i915_write32(i915, DDI_BUF_TRANS_LO(o->port, (int)i), t[i].lo);
		i915_write32(i915, DDI_BUF_TRANS_HI(o->port, (int)i), t[i].hi);
	}
	skl_set_iboost(i915, o->port, t[level].iboost);
	/* entry 9 doubles as the HDMI default on ports that carry both */
	if (!hdmi) {
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
	int gen11 = i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL;
	(void)mode;
	/* combo PHY up, port clocked, a Type-C PHY told the link's width,
	 * the levels in, and only then the unused combo lanes powered down */
	if (gen11 && !o->is_tc)
		icl_combo_phy_init(i915, o->port);
	intel_dpll_route_port(i915, o->port, o->pll);
	if (gen11 && o->is_tc)
		intel_tc_program_dp_mode(i915, o, o->lane_count);
	intel_ddi_set_buf_trans(i915, o, 0);
	if (gen11 && !o->is_tc)
		icl_combo_phy_power_lanes(i915, o->port, o->lane_count, o->lane_reversal);
	if (i915->display.model != INTEL_DISPLAY_MTL && i915->display.model != INTEL_DISPLAY_DG2)
		intel_tc_clock_gating(i915, o, 0);
}

/* Whether the level goes through the DDI's translation select (the
 * Skylake and Broadwell way) or straight into a PHY. */
int intel_ddi_level_in_buf_ctl(struct i915_device *i915)
{
	/* Skylake's levels also carry a current boost, which only the full
	 * translation write sets; Haswell/Broadwell select by index alone. */
	return i915->display.model == INTEL_DISPLAY_BDW;
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
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		/* the level is the PHY's (set already); the buffer is the
		 * port's and the PHY's together */
		(void)level;
		mtl_ddi_buf_enable(i915, o, lanes);
		return;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		(void)level;
		dg2_ddi_buf_enable(i915, o, lanes);
		return;
	}
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	v &= ~(DDI_PORT_WIDTH_MASK | DDI_BUF_EMP_MASK | DDI_BUF_PORT_REVERSAL);
	v |= DDI_BUF_CTL_ENABLE | ddi_port_width(lanes) |
	     DDI_BUF_TRANS_SELECT((uint32_t)level);
	/* Alder Lake-P's Type-C link rate and ownership, the lane stagger of
	 * Gen11-13 Type-C ports */
	if (i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL) {
		uint32_t mask, bits = intel_dpll_ddi_buf_ctl_bits(i915, o, &mask);
		v = (v & ~mask) | bits;
	}
	if (o->lane_reversal)
		v |= DDI_BUF_PORT_REVERSAL;
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
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		mtl_ddi_buf_disable(i915, o);
		goto tp_off;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		dg2_ddi_buf_disable(i915, o);
		goto tp_off;
	}
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
tp_off:;
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
	/* Before Tiger Lake port A is the embedded transcoder's alone and
	 * has no idle-done status; from Tiger Lake the status is the
	 * transcoder's, like every other port's. */
	if (o->port == PORT_A && i915->info->display_ver < 12) {
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
	if (i915->display.model != INTEL_DISPLAY_MTL && i915->display.model != INTEL_DISPLAY_DG2)
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
	/* Gen11 on: which ports exist is the VBT's word alone. */
	if (i915->info->display_ver >= 11)
		return 0;
	uint32_t strap = i915_read32(i915, SFUSE_STRAP);
	i915_dbg("[drm] i915: DDI straps: B%s C%s D%s\n",
		(strap & SFUSE_STRAP_DDIB_DETECTED) ? "+" : "-",
		(strap & SFUSE_STRAP_DDIC_DETECTED) ? "+" : "-",
		(strap & SFUSE_STRAP_DDID_DETECTED) ? "+" : "-");
	return 0;
}
