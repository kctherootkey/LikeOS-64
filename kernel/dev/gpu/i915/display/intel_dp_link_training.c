// LikeOS -- DisplayPort link training on the DisplayPort helper library.
//
// A link is trained one PHY at a time from the port outwards: each
// link-training tunable PHY repeater (LTTPR) in non-transparent mode is a
// receiver of its own, trained like the sink, and the sink (DPRX) comes
// last.  Every PHY goes through clock recovery (training pattern 1: send
// it, wait, read what the receiver saw, raise the swing and pre-emphasis
// it asks for, until every lane reports CR done) and channel
// equalisation (pattern 2 or 3 until every lane has EQ done and symbol
// lock and the lanes are aligned).  Only the PHY the port drives itself
// is fed from the port's buffer levels; a repeater further along is told
// its levels over AUX.  The status decoders, the training waits and the
// repeater capability reads are the helper library's (drm_dp_helper.h).
//
// Repeaters are found at detect and again before each training
// (intel_dp_init_lttpr_and_dprx_caps()): I915_FEAT_DP_LTTPR 0 leaves them
// alone, 1 puts them into transparent mode, 2 (the default) into
// non-transparent mode where they take it.  Displays whose AUX timeout is
// shorter than the 3.2 ms the repeater detection needs (before display
// version 10, and Gemini Lake) never look for them.
//
// The loops keep the driver's earlier limits and waits: 10 clock recovery
// passes, 5 at one voltage, one more after the maximum swing, 6
// equalisation passes, the channel equalisation wait the sink asks for;
// the clock recovery wait is the library's (100 us, or the sink's
// interval before DPCD 1.4).  With I915_FEAT_DP_HELPERS 0 none of this is
// built and intel_dp.c trains the link itself.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2015 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dp_link_training.h>
#include <kernel/dev/gpu/i915/intel_dsc.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#if I915_FEAT_DP_HELPERS

/* ---- receiver capabilities ----------------------------------------------- */

int intel_dp_read_receiver_caps(struct intel_output *o, u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
#if I915_FEAT_DP_EXT_CAPS
	return drm_dp_read_dpcd_caps(&o->aux.dp, dpcd);
#else
	int ret = drm_dp_dpcd_read_data(&o->aux.dp, DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE);

	if (ret < 0)
		return ret;
	return dpcd[DP_DPCD_REV] ? 0 : -EIO;
#endif
}

/* ---- repeaters (LTTPRs) ---------------------------------------------------- */

#define LTTPR_CAP(o, reg) \
	((o)->lttpr_common_caps[(reg) - DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV])

/* Looking for repeaters needs an AUX timeout of at least 3.2 ms (DP 2.0
 * 2.11.2, 3.6.6.1), which displays before version 10 and Gemini Lake do
 * not have; a panel has none. */
static bool lttpr_detect_allowed(struct i915_device *i915, const struct intel_output *o)
{
	if (!I915_FEAT_DP_LTTPR || o->is_edp)
		return false;
	return i915->info->display_ver >= 10 &&
	       i915->info->platform != I915_PLATFORM_GEMINILAKE;
}

static void reset_lttpr_common_caps(struct intel_output *o)
{
	mm_memset(o->lttpr_common_caps, 0, sizeof(o->lttpr_common_caps));
}

static void reset_lttpr_count(struct intel_output *o)
{
	LTTPR_CAP(o, DP_PHY_REPEATER_CNT) = 0;
}

static u8 *lttpr_phy_caps(struct intel_output *o, enum drm_dp_phy dp_phy)
{
	return o->lttpr_phy_caps[dp_phy - DP_PHY_LTTPR1];
}

static void read_lttpr_phy_caps(struct intel_output *o, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
				enum drm_dp_phy dp_phy)
{
	u8 *phy_caps = lttpr_phy_caps(o, dp_phy);

	if (drm_dp_read_lttpr_phy_caps(&o->aux.dp, dpcd, dp_phy, phy_caps) < 0)
		i915_dbg("[drm] i915: DP port %c: %s: no PHY capabilities\n",
			 'A' + o->port, drm_dp_phy_name(dp_phy));
}

static bool read_lttpr_common_caps(struct intel_output *o, const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	if (drm_dp_read_lttpr_common_caps(&o->aux.dp, dpcd, o->lttpr_common_caps) < 0)
		goto reset_caps;
	/* The field structure is at least revision 1.4. */
	if (o->lttpr_common_caps[0] < 0x14)
		goto reset_caps;
	return true;

reset_caps:
	reset_lttpr_common_caps(o);
	return false;
}

/* Note the mode in the cached capabilities (what the repeaters were last
 * told), so an active link's mode can be kept. */
static void set_lttpr_transparent_mode(struct intel_output *o, bool enable)
{
	LTTPR_CAP(o, DP_PHY_REPEATER_MODE) = enable ? DP_PHY_REPEATER_MODE_TRANSPARENT :
						      DP_PHY_REPEATER_MODE_NON_TRANSPARENT;
}

bool intel_dp_lttpr_transparent_mode(const struct intel_output *o)
{
	return o->lttpr_count <= 0;
}

static bool lttpr_transparent_mode_enabled(struct intel_output *o)
{
	return LTTPR_CAP(o, DP_PHY_REPEATER_MODE) == DP_PHY_REPEATER_MODE_TRANSPARENT;
}

/*
 * Read the repeaters' common capabilities and switch them to the mode
 * the switch asks for.  The number of repeaters to train one by one, 0
 * when they stay transparent (or there are none, or the detection
 * failed).  An active link keeps the mode it trained in: changing it
 * resets the repeaters' state (DP 2.0 3.6.7) and loses the link.
 */
static int init_lttpr_phys(struct intel_output *o, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
			   bool link_active)
{
	int lttpr_count;

	if (!read_lttpr_common_caps(o, dpcd))
		return 0;

	lttpr_count = drm_dp_lttpr_count(o->lttpr_common_caps);
	/* Setting transparent mode explicitly with no repeater there breaks
	 * training on some docks (Dell WD19TB): only when there is one. */
	if (lttpr_count == 0)
		return 0;

	if (link_active) {
		if (lttpr_count < 0 || lttpr_transparent_mode_enabled(o))
			goto out_reset_lttpr_count;
		return lttpr_count;
	}

	if (I915_FEAT_DP_LTTPR == 1) {
		/* Transparent only: the repeaters pass the training through
		 * and the sink is trained as if they were not there. */
		if (drm_dp_lttpr_set_transparent_mode(&o->aux.dp, true))
			i915_dbg("[drm] i915: DP port %c: repeaters refused transparent mode\n",
				 'A' + o->port);
		set_lttpr_transparent_mode(o, true);
		goto out_reset_lttpr_count;
	}

	if (drm_dp_lttpr_init(&o->aux.dp, lttpr_count)) {
		i915_dbg("[drm] i915: DP port %c: repeaters refused non-transparent mode; transparent\n",
			 'A' + o->port);
		set_lttpr_transparent_mode(o, true);
		goto out_reset_lttpr_count;
	}
	set_lttpr_transparent_mode(o, false);
	return lttpr_count;

out_reset_lttpr_count:
	reset_lttpr_count(o);
	return 0;
}

static int init_lttpr(struct intel_output *o, const u8 dpcd[DP_RECEIVER_CAP_SIZE],
		      bool link_active)
{
	int lttpr_count = init_lttpr_phys(o, dpcd, link_active);

	for (int i = 0; i < lttpr_count; i++) {
		read_lttpr_phy_caps(o, dpcd, DP_PHY_LTTPR(i));
		if (I915_DEBUG)
			drm_dp_dump_lttpr_desc(&o->aux.dp, DP_PHY_LTTPR(i));
	}
	return lttpr_count;
}

/* The receiver caps, read first to wake the repeaters (an access to the
 * repeater field before them). */
static int read_dprx_caps(struct i915_device *i915, struct intel_output *o,
			  u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	if (o->is_edp)
		return 0;
	if (lttpr_detect_allowed(i915, o) &&
	    drm_dp_dpcd_probe(&o->aux.dp, DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV))
		return -EIO;
	return intel_dp_read_receiver_caps(o, dpcd);
}

int intel_dp_init_lttpr_and_dprx_caps(struct i915_device *i915, struct intel_output *o,
				      bool link_active)
{
	int lttpr_count = 0;
	int was = o->lttpr_count;

	if (lttpr_detect_allowed(i915, o)) {
		u8 dpcd[DP_RECEIVER_CAP_SIZE];
		int err = read_dprx_caps(i915, o, dpcd);

		if (err != 0)
			return err;
		lttpr_count = init_lttpr(o, dpcd, link_active);
	} else {
		reset_lttpr_common_caps(o);
	}
	o->lttpr_count = lttpr_count;
	if (lttpr_count != was && lttpr_count > 0)
		kprintf("[drm] i915: DP port %c: %d link-training repeater%s, trained one by one\n",
			'A' + o->port, lttpr_count, lttpr_count > 1 ? "s" : "");

	/* The receiver's capabilities are read after the repeaters'
	 * (the DPTX shall, after LTTPR detection); a failed read leaves
	 * what was read before in place. */
	u8 dprx[DP_RECEIVER_CAP_SIZE];
	if (intel_dp_read_receiver_caps(o, dprx)) {
		reset_lttpr_common_caps(o);
		o->lttpr_count = 0;
		return -EIO;
	}
	mm_memcpy(o->dpcd, dprx, sizeof(dprx));
	return lttpr_count;
}

int intel_dp_lttpr_max_link_rate(const struct intel_output *o)
{
	return drm_dp_lttpr_max_link_rate(o->lttpr_common_caps);
}

int intel_dp_lttpr_max_lane_count(const struct intel_output *o)
{
	return drm_dp_lttpr_max_lane_count(o->lttpr_common_caps);
}

/* ---- one PHY's training --------------------------------------------------- */

struct lt_state {
	struct i915_device *i915;
	struct intel_output *o;
	enum drm_dp_phy phy;
	int lanes;
	/* the PHY the port's own buffer drives */
	bool source_hop;
	u8 train_set[4];
};

/* The levels the port drives, from o->train_set: one level for every
 * lane (the buffer translation is per port). */
static void source_levels(struct i915_device *i915, struct intel_output *o)
{
	int swing = o->train_set[0] & 3;
	int pre = (o->train_set[0] >> DP_TRAIN_PRE_EMPHASIS_SHIFT) & 3;
	int level = intel_ddi_dp_level_for(swing, pre);

	if (intel_ddi_level_in_buf_ctl(i915)) {
		uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
		v &= ~DDI_BUF_EMP_MASK;
		v |= DDI_BUF_TRANS_SELECT((uint32_t)level);
		i915_write32(i915, DDI_BUF_CTL(o->port), v);
		(void)i915_read32(i915, DDI_BUF_CTL(o->port));
	} else {
		intel_ddi_set_buf_trans(i915, o, level);
	}
}

static unsigned pattern_set_reg(enum drm_dp_phy phy)
{
	return phy == DP_PHY_DPRX ? DP_TRAINING_PATTERN_SET :
				    DP_TRAINING_PATTERN_SET_PHY_REPEATER(phy);
}

/* The levels into the port (when it drives this PHY), then the pattern
 * and the lanes' levels into the receiver: DP_TRAINING_LANEx_SET follow
 * DP_TRAINING_PATTERN_SET, for the sink and for each repeater. */
static void lt_apply(struct lt_state *st, u8 pattern)
{
	struct intel_output *o = st->o;
	u8 buf[5];
	unsigned len;

	if (st->source_hop) {
		mm_memcpy(o->train_set, st->train_set, sizeof(o->train_set));
		source_levels(st->i915, o);
	}
	buf[0] = pattern;
	for (int l = 0; l < 4; l++)
		buf[1 + l] = st->train_set[l];
	len = pattern == DP_TRAINING_PATTERN_DISABLE ? 1 : (unsigned)(1 + st->lanes);
	intel_dp_aux_native_write(&o->aux, pattern_set_reg(st->phy), buf, len);
}

static u8 dp_voltage_max(u8 preemph)
{
	switch (preemph & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case DP_TRAIN_PRE_EMPH_LEVEL_0:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
	case DP_TRAIN_PRE_EMPH_LEVEL_1:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
	case DP_TRAIN_PRE_EMPH_LEVEL_2:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_1;
	case DP_TRAIN_PRE_EMPH_LEVEL_3:
	default:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_0;
	}
}

/*
 * What the receiver asks for, into the training set.  From the port: the
 * highest swing and pre-emphasis any lane asked for, on every lane (one
 * buffer level per port), the maximum flags at level 3.  From a repeater:
 * each lane its own request, capped at what that repeater's transmitter
 * can do (level 3 where its capabilities say so, else 2), and the swing
 * capped so swing + pre-emphasis stays within the allowed total.
 */
static void lt_adjust(struct lt_state *st, const u8 ls[DP_LINK_STATUS_SIZE])
{
	if (st->source_hop) {
		int swing = 0, pre = 0;

		for (int l = 0; l < st->lanes; l++) {
			int s = drm_dp_get_adjust_request_voltage(ls, l) >> DP_TRAIN_VOLTAGE_SWING_SHIFT;
			int p = drm_dp_get_adjust_request_pre_emphasis(ls, l) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
			if (s > swing)
				swing = s;
			if (p > pre)
				pre = p;
		}
		for (int l = 0; l < 4; l++) {
			u8 v = (u8)(swing | (pre << DP_TRAIN_PRE_EMPHASIS_SHIFT));
			if (swing == 3)
				v |= DP_TRAIN_MAX_SWING_REACHED;
			if (pre == 3)
				v |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
			st->train_set[l] = v;
		}
		return;
	}

	/* the transmitter feeding this repeater's receiver: the next one
	 * towards the port */
	const u8 *tx_caps = lttpr_phy_caps(st->o, (enum drm_dp_phy)(st->phy + 1));
	u8 vmax = drm_dp_lttpr_voltage_swing_level_3_supported(tx_caps) ?
			  DP_TRAIN_VOLTAGE_SWING_LEVEL_3 : DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
	u8 pmax = drm_dp_lttpr_pre_emphasis_level_3_supported(tx_caps) ?
			  DP_TRAIN_PRE_EMPH_LEVEL_3 : DP_TRAIN_PRE_EMPH_LEVEL_2;

	for (int lane = 0; lane < 4; lane++) {
		int l = lane < st->lanes ? lane : st->lanes - 1;
		u8 v = drm_dp_get_adjust_request_voltage(ls, l);
		u8 p = drm_dp_get_adjust_request_pre_emphasis(ls, l);

		if (p >= pmax)
			p = pmax | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
		if (v > dp_voltage_max(p))
			v = dp_voltage_max(p);
		if (v >= vmax)
			v = vmax | DP_TRAIN_MAX_SWING_REACHED;
		st->train_set[lane] = v | p;
	}
}

static void lt_cr_delay(struct lt_state *st)
{
	if (st->phy == DP_PHY_DPRX)
		drm_dp_link_train_clock_recovery_delay(&st->o->aux.dp, st->o->dpcd);
	else
		drm_dp_lttpr_link_train_clock_recovery_delay();
}

static void lt_eq_delay(struct lt_state *st)
{
	if (st->phy == DP_PHY_DPRX)
		drm_dp_link_train_channel_eq_delay(&st->o->aux.dp, st->o->dpcd);
	else
		drm_dp_lttpr_link_train_channel_eq_delay(&st->o->aux.dp,
							 lttpr_phy_caps(st->o, st->phy));
}

static const char *lt_where(const struct lt_state *st)
{
	return st->phy == DP_PHY_DPRX ? "" : drm_dp_phy_name(st->phy);
}

static int lt_clock_recovery(struct lt_state *st)
{
	struct i915_device *i915 = st->i915;
	struct intel_output *o = st->o;
	u8 ls[DP_LINK_STATUS_SIZE];
	int voltage_tries = 0, max_swing_tries = 0;
	u8 last_set = 0xff;

	mm_memset(ls, 0, sizeof(ls));
	mm_memset(st->train_set, 0, sizeof(st->train_set));
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_PAT1);
	lt_apply(st, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	for (int tries = 0; tries < 10; tries++) {
		lt_cr_delay(st);
		if (drm_dp_dpcd_read_phy_link_status(&o->aux.dp, st->phy, ls) < 0)
			break;
		if (drm_dp_clock_recovery_ok(ls, st->lanes))
			return 0;
		if ((st->train_set[0] & DP_TRAIN_MAX_SWING_REACHED) && ++max_swing_tries > 1)
			break;
		lt_adjust(st, ls);
		if (st->train_set[0] == last_set) {
			if (++voltage_tries >= 5)
				break;
		} else {
			voltage_tries = 0;
			last_set = st->train_set[0];
		}
		lt_apply(st, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	}
	kprintf("[drm] i915: DP port %c: clock recovery failed%s%s (rate %u, %u lanes, status %02x %02x)\n",
		'A' + o->port, st->phy == DP_PHY_DPRX ? "" : " at ", lt_where(st),
		o->link_rate_khz, st->lanes, ls[0], ls[1]);
	kprintf("[drm] i915:   DDI_BUF_CTL %08x, DP_TP_CTL %08x, train set %02x, adjust %02x %02x\n",
		i915_read32(i915, DDI_BUF_CTL(o->port)),
		i915_read32(i915, intel_dp_tp_ctl_reg(i915, o)), st->train_set[0],
		ls[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS],
		ls[DP_ADJUST_REQUEST_LANE2_3 - DP_LANE0_1_STATUS]);
	if (i915->display.model == INTEL_DISPLAY_SKL)
		kprintf("[drm] i915:   DPLL%d, DPLL_CTRL1 %08x, DPLL_CTRL2 %08x, status %08x, AUX errors %u\n",
			o->pll, i915_read32(i915, DPLL_CTRL1), i915_read32(i915, DPLL_CTRL2),
			i915_read32(i915, DPLL_STATUS), o->aux.errors);
	else
		kprintf("[drm] i915:   PLL %d, AUX errors %u\n", o->pll, o->aux.errors);
	return -EIO;
}

static int lt_channel_eq(struct lt_state *st)
{
	struct i915_device *i915 = st->i915;
	struct intel_output *o = st->o;
	u8 ls[DP_LINK_STATUS_SIZE];
	/* A repeater takes pattern 3; the sink when it says so. */
	int tps3 = st->phy != DP_PHY_DPRX ||
		   !!(o->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED);
	u8 pat = tps3 ? DP_TRAINING_PATTERN_3 : DP_TRAINING_PATTERN_2;

	mm_memset(ls, 0, sizeof(ls));
	intel_ddi_set_train_pattern(i915, o, tps3 ? DP_TP_CTL_LINK_TRAIN_PAT3 :
						   DP_TP_CTL_LINK_TRAIN_PAT2);
	lt_apply(st, pat | DP_LINK_SCRAMBLING_DISABLE);
	for (int tries = 0; tries < 6; tries++) {
		lt_eq_delay(st);
		if (drm_dp_dpcd_read_phy_link_status(&o->aux.dp, st->phy, ls) < 0)
			break;
		/* clock recovery lost */
		if (!drm_dp_clock_recovery_ok(ls, st->lanes))
			break;
		if (drm_dp_channel_eq_ok(ls, st->lanes))
			return 0;
		lt_adjust(st, ls);
		lt_apply(st, pat | DP_LINK_SCRAMBLING_DISABLE);
	}
	kprintf("[drm] i915: DP port %c: channel equalisation failed%s%s (status %02x %02x %02x)\n",
		'A' + o->port, st->phy == DP_PHY_DPRX ? "" : " at ", lt_where(st),
		ls[0], ls[1], ls[2]);
	return -EIO;
}

static int lt_train_phy(struct lt_state *st)
{
	int rc = lt_clock_recovery(st);

	if (rc == 0)
		rc = lt_channel_eq(st);
	return rc;
}

static void lt_init(struct lt_state *st, struct i915_device *i915, struct intel_output *o,
		    enum drm_dp_phy phy)
{
	mm_memset(st, 0, sizeof(*st));
	st->i915 = i915;
	st->o = o;
	st->phy = phy;
	st->lanes = o->lane_count;
	/* the port drives the repeater nearest to it, or the sink */
	st->source_hop = o->lttpr_count <= 0 || (int)phy == DP_PHY_LTTPR(o->lttpr_count - 1);
}

/* ---- the link ---------------------------------------------------------------- */

/* Tell the sink the link: the rate (eDP 1.4 by index into its table),
 * the width and framing, spread spectrum and the MSA-ignore bit, 8b/10b
 * coding. */
static void lt_prepare(struct i915_device *i915, struct intel_output *o, int enhanced)
{
	uint8_t lanes = (uint8_t)o->lane_count;
	(void)i915;

	if (o->use_rate_select) {
		/* eDP 1.4: select by index into the rate table.  Some muxes in
		 * front of the panel (Parade PS8461E, on many laptops) snoop
		 * the table to run in jitter-cleaning mode and forget it when
		 * powered down: let them read it again first. */
		uint8_t rates[DP_MAX_SUPPORTED_RATES * 2];
		uint8_t idx = 0;

		intel_dp_aux_native_read(&o->aux, DP_SUPPORTED_LINK_RATES, rates, sizeof(rates));
		for (int i = 0; i < o->nsink_rates; i++)
			if (o->sink_rates_khz[i] == o->link_rate_khz)
				idx = (uint8_t)i;
		intel_dp_dpcd_write8(&o->aux, DP_LINK_BW_SET, 0);
		intel_dp_dpcd_write8(&o->aux, DP_LINK_RATE_SET, idx);
	} else {
		intel_dp_dpcd_write8(&o->aux, DP_LINK_BW_SET,
				     drm_dp_link_rate_to_bw_code((int)o->link_rate_khz));
	}
	intel_dp_dpcd_write8(&o->aux, DP_LANE_COUNT_SET,
			     (uint8_t)(lanes | (enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0)));
	/* Only claim spread spectrum when the source's PLL actually spreads:
	 * a sink told to expect it while the link is steady tracks a
	 * frequency that never moves, and one not told while the link does
	 * spread loses lock. */
	intel_dp_dpcd_write8(&o->aux, DP_DOWNSPREAD_CTRL,
			     (uint8_t)((o->ssc ? DP_SPREAD_AMP_0_5 : 0) |
				       (o->msa_timing_par_ignore ? DP_MSA_TIMING_PAR_IGNORE_EN : 0)));
	intel_dp_dpcd_write8(&o->aux, DP_MAIN_LINK_CHANNEL_CODING_SET, DP_SET_ANSI_8B10B);
}

int intel_dp_link_train(struct i915_device *i915, struct intel_output *o)
{
	uint8_t lanes = (uint8_t)o->lane_count;
	int enhanced = !!(o->dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
	int tps3 = !!(o->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED);
	struct lt_state st;
	int rc = 0;

	/* The repeaters again, link down: a detect made while the link was
	 * up could not switch them to non-transparent mode.  Training goes
	 * on without them if they do not answer. */
	if (lttpr_detect_allowed(i915, o) &&
	    intel_dp_init_lttpr_and_dprx_caps(i915, o, false) < 0)
		o->lttpr_count = 0;

	lt_prepare(i915, o, enhanced);
	for (int l = 0; l < 4; l++)
		o->train_set[l] = 0;
	i915_dbg("[drm] i915: DP port %c: training at %u kHz x%u, %s swing table%s (DPCD %x.%x, TPS3 %d, %d repeaters)\n",
		'A' + o->port, o->link_rate_khz, lanes,
		(o->is_edp && i915->display.vbt.edp_low_vswing) ? "low-voltage" : "standard",
		o->ssc ? ", spread spectrum" : "", o->dpcd[DP_DPCD_REV] >> 4,
		o->dpcd[DP_DPCD_REV] & 0xf, tps3, o->lttpr_count);
	intel_ddi_dp_tp_enable(i915, o, enhanced);
	intel_ddi_buf_enable(i915, o, lanes, 0);

	/* The repeaters from the port outwards, each left without a
	 * pattern once trained, then the sink. */
	for (int i = o->lttpr_count - 1; i >= 0; i--) {
		lt_init(&st, i915, o, DP_PHY_LTTPR(i));
		rc = lt_train_phy(&st);
		lt_apply(&st, DP_TRAINING_PATTERN_DISABLE);
		if (rc)
			break;
	}
	lt_init(&st, i915, o, DP_PHY_DPRX);
	if (rc == 0)
		rc = lt_train_phy(&st);
	if (rc) {
		lt_apply(&st, DP_TRAINING_PATTERN_DISABLE);
		return -EIO;
	}
	/* idle pattern, then normal operation; the sink stops training */
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_IDLE);
	intel_ddi_wait_idle_done(i915, o);
	lt_apply(&st, DP_TRAINING_PATTERN_DISABLE);
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_NORMAL);
	i915_dbg("[drm] i915: DP port %c: link trained at %u kHz x%u (swing %u, pre-emphasis %u)\n",
		'A' + o->port, o->link_rate_khz, lanes, o->train_set[0] & 3,
		(o->train_set[0] >> DP_TRAIN_PRE_EMPHASIS_SHIFT) & 3);
	/* A link reported failed works again. */
	if (o->conn >= 0 && i915->drm.conn[o->conn].link_status != DRM_MODE_LINK_STATUS_GOOD)
		drm_connector_set_link_status_property(&i915->drm.conn[o->conn],
						       DRM_MODE_LINK_STATUS_GOOD);
	return 0;
}

/*
 * The sink lost the link while the pipe runs: take the port's buffer
 * down and train again at the same rate and width, the transcoder
 * running on (the port is fed again once the link is back).  Only on the
 * displays whose port buffer and training pattern can be cycled under a
 * running transcoder (DDI_BUF_CTL / DP_TP_CTL, display versions before
 * 14); the PHYs of Meteor Lake and DG2 need the whole mode set.
 */
int intel_dp_retrain_link(struct i915_device *i915, struct intel_output *o)
{
	if (!o->active || o->type != INTEL_OUTPUT_DP)
		return -EINVAL;
	if (i915->display.model == INTEL_DISPLAY_MTL || i915->display.model == INTEL_DISPLAY_DG2)
		return -EOPNOTSUPP;
	struct intel_pipe *p = o->pipe >= 0 && o->pipe < INTEL_MAX_PIPES ?
				       &i915->display.pipes[o->pipe] : NULL;
	int rc;

	intel_dp_link_off(i915, o);
	/* a compressed stream keeps the sink decompressing and FEC on the
	 * new link, or the running picture is lost */
	if (p)
		intel_dsc_pre_link_train(i915, o, p);
	rc = intel_dp_link_train(i915, o);
	if (rc == 0 && p)
		intel_dsc_fec_enable(i915, o, p);
	return rc;
}

#endif /* I915_FEAT_DP_HELPERS */
