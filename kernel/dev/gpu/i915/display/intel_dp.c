// LikeOS -- DisplayPort and eDP sinks: detection, EDID, link training.
//
// A DisplayPort link is negotiated: the sink says what rates and lane
// counts it takes (DPCD), the driver picks the least that carries the
// mode, and trains the link -- clock recovery, then channel
// equalisation, each a loop of "send this pattern, read what the sink
// saw, adjust the swing" until the sink reports every lane locked.
// eDP is the same protocol on the internal panel, with panel power
// sequencing around it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

uint32_t intel_dp_link_bw_khz(uint8_t bw)
{
	return (uint32_t)bw * 27000;
}

static uint8_t link_bw_code(uint32_t khz)
{
	return (uint8_t)(khz / 27000);
}

/* ---- detection --------------------------------------------------------- */

static int hpd_live(struct i915_device *i915, int port)
{
	return intel_hpd_live(i915, port);
}

static int read_dpcd(struct i915_device *i915, struct intel_output *o)
{
	int n = intel_dp_aux_native_read(&o->aux, DP_DPCD_REV, o->dpcd,
					 DP_RECEIVER_CAP_SIZE);
	(void)i915;
	if (n < 8 || o->dpcd[DP_DPCD_REV] == 0)
		return -EIO;
	o->nsink_rates = 0;
	if (o->is_edp) {
		intel_dp_aux_native_read(&o->aux, DP_EDP_DPCD_REV, o->edp_dpcd,
					 sizeof(o->edp_dpcd));
		/* eDP 1.4: a table of rates in 200 kHz units */
		if (o->dpcd[DP_DPCD_REV] >= 0x13 || o->edp_dpcd[0] >= 0x03) {
			uint8_t r[16];
			if (intel_dp_aux_native_read(&o->aux, DP_SUPPORTED_LINK_RATES,
						     r, 16) == 16) {
				for (int i = 0; i < 8; i++) {
					uint32_t v = (uint32_t)r[i * 2] |
						     ((uint32_t)r[i * 2 + 1] << 8);
					if (!v)
						break;
					o->sink_rates_khz[o->nsink_rates++] = v * 200;
				}
			}
		}
	}
	if (!o->nsink_rates) {
		uint8_t max = o->dpcd[DP_MAX_LINK_RATE];
		static const uint8_t std[] = { DP_LINK_BW_1_62, DP_LINK_BW_2_7,
					       DP_LINK_BW_5_4, DP_LINK_BW_8_1 };
		for (unsigned i = 0; i < sizeof(std); i++)
			if (std[i] <= max)
				o->sink_rates_khz[o->nsink_rates++] =
					intel_dp_link_bw_khz(std[i]);
	}
	return 0;
}

int intel_dp_detect(struct i915_device *i915, struct intel_output *o)
{
	if (o->is_edp) {
		/* The panel is wired; power its logic and ask. */
		intel_power_get(i915, INTEL_PW_AUX_A);
		intel_pps_vdd_on(i915, o);
		int rc = read_dpcd(i915, o);
		o->detected = (rc == 0);
		if (rc)
			kprintf("[drm] i915: eDP panel does not answer on AUX %c\n",
				'A' + o->aux.port);
		return o->detected;
	}
	if (!hpd_live(i915, o->port)) {
		o->detected = 0;
		return 0;
	}
	int rc = read_dpcd(i915, o);
	o->detected = (rc == 0);
	return o->detected;
}

int intel_dp_read_edid(struct i915_device *i915, struct intel_output *o)
{
	(void)i915;
	int blocks = drm_edid_read(&o->aux.i2c, o->edid, sizeof(o->edid) / DRM_EDID_BLOCK);
	if (blocks <= 0) {
		o->edid_len = 0;
		return blocks < 0 ? blocks : -ENOENT;
	}
	o->edid_len = blocks * DRM_EDID_BLOCK;
	return 0;
}

/* ---- the link ------------------------------------------------------------ */

/* Bandwidth: a lane at rate R carries R/10 bytes/s (8b/10b); 3 bytes per
 * pixel at 8 bpc.  The mode needs clock * 3 bytes/s, with some margin. */
static int link_carries(uint32_t rate_khz, int lanes, uint32_t pixel_khz, int bpp)
{
	uint64_t link = (uint64_t)rate_khz * lanes * 8 / 10; /* kbyte/s... */
	uint64_t need = (uint64_t)pixel_khz * bpp / 8;
	/* both in "kB/s" units; keep 1% headroom */
	return link * 99 >= need * 100;
}

int intel_dp_choose_link(struct i915_device *i915, struct intel_output *o,
			 const struct drm_mode_modeinfo *mode)
{
	int max_lanes = o->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
	int bpp = 24;
	(void)i915;

	if (max_lanes < 1)
		max_lanes = 1;
	if (max_lanes > 4)
		max_lanes = 4;
	if (o->port == PORT_A && !o->four_lane_strap && max_lanes > 2)
		max_lanes = 2; /* DDI A strapped to two lanes (E takes the rest) */
	if (i915->display.vbt.edp_lanes && o->is_edp &&
	    i915->display.vbt.edp_lanes < max_lanes)
		max_lanes = i915->display.vbt.edp_lanes;
	if (o->is_tc && o->tc_owned && intel_tc_max_lanes(i915, o) < max_lanes)
		max_lanes = intel_tc_max_lanes(i915, o); /* what the FIA granted */
	/* Spread spectrum where the sink takes it: an embedded panel's link
	 * normally runs with it, and the source's PLL has to agree with what
	 * the sink is told. */
	o->ssc = o->is_edp && (o->dpcd[DP_MAX_DOWNSPREAD] & 1);

	/* The configuration the firmware had this port running at, if it
	 * carries the mode: the panel demonstrably trains at it, and
	 * nothing about the board is being guessed. */
	if (o->fw_link_rate_khz && o->fw_lanes >= 1 && o->fw_lanes <= max_lanes &&
	    intel_dpll_rate_supported(i915, o->fw_link_rate_khz) &&
	    link_carries(o->fw_link_rate_khz, o->fw_lanes, mode->clock, bpp)) {
		for (int r = 0; r < o->nsink_rates; r++) {
			if (o->sink_rates_khz[r] != o->fw_link_rate_khz)
				continue;
			o->link_rate_khz = o->fw_link_rate_khz;
			o->lane_count = o->fw_lanes;
			return 0;
		}
	}
	/* Otherwise the slowest link that carries the mode, fewest lanes
	 * first, among the rates the port's PLL can be clocked at. */
	for (int lanes = 1; lanes <= max_lanes; lanes *= 2) {
		for (int r = 0; r < o->nsink_rates; r++) {
			uint32_t rate = o->sink_rates_khz[r];
			if (!intel_dpll_rate_supported(i915, rate))
				continue;
			if (link_carries(rate, lanes, mode->clock, bpp)) {
				o->link_rate_khz = rate;
				o->lane_count = lanes;
				return 0;
			}
		}
	}
	/* Nothing fits: the fastest rate the port can drive, all lanes. */
	for (int r = o->nsink_rates - 1; r >= 0; r--) {
		uint32_t rate = o->sink_rates_khz[r];
		if (intel_dpll_rate_supported(i915, rate)) {
			o->link_rate_khz = rate;
			o->lane_count = max_lanes;
			if (link_carries(rate, max_lanes, mode->clock, bpp))
				return 0;
			break;
		}
	}
	/* Still nothing.  The firmware had a link running on this port that
	 * carried this very picture, so take it: what it used is a fact
	 * about the board, while the limits above are read from tables. */
	if (o->fw_link_rate_khz && o->fw_lanes >= 1 && o->fw_lanes <= 4 &&
	    intel_dpll_rate_supported(i915, o->fw_link_rate_khz) &&
	    link_carries(o->fw_link_rate_khz, o->fw_lanes, mode->clock, bpp)) {
		kprintf("[drm] i915: port %c: no link fits the limits read here; using the one the firmware ran (%u kHz x%d)\n",
			'A' + o->port, o->fw_link_rate_khz, o->fw_lanes);
		o->link_rate_khz = o->fw_link_rate_khz;
		o->lane_count = o->fw_lanes;
		return 0;
	}
	return -ENOSPC;
}

void intel_dp_sink_power(struct i915_device *i915, struct intel_output *o, int on)
{
	(void)i915;
	if (o->dpcd[DP_DPCD_REV] < 0x11)
		return;
	if (on) {
		for (int i = 0; i < 3; i++) {
			if (intel_dp_dpcd_write8(&o->aux, DP_SET_POWER, DP_SET_POWER_D0) == 0)
				break;
			lapic_delay_ms(1);
		}
		lapic_delay_ms(1);
	} else {
		intel_dp_dpcd_write8(&o->aux, DP_SET_POWER, DP_SET_POWER_D3);
	}
}

static int lane_status(const uint8_t *st, int lane)
{
	return (st[lane / 2] >> (4 * (lane & 1))) & 0xf;
}

static void adjust_from_request(struct intel_output *o, const uint8_t *adj)
{
	/* the highest swing / pre-emphasis any lane asked for, applied to
	 * all (the buffer translation is per port) */
	int swing = 0, pre = 0;
	for (int l = 0; l < o->lane_count; l++) {
		int v = (adj[l / 2] >> (4 * (l & 1))) & 0xf;
		int s = v & 3, p = (v >> 2) & 3;
		if (s > swing)
			swing = s;
		if (p > pre)
			pre = p;
	}
	for (int l = 0; l < 4; l++) {
		uint8_t v = (uint8_t)(swing | (pre << DP_TRAIN_PRE_EMPHASIS_SHIFT));
		if (swing == 3)
			v |= DP_TRAIN_MAX_SWING_REACHED;
		if (pre == 3)
			v |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
		o->train_set[l] = v;
	}
}

static int train_set_level(const struct intel_output *o)
{
	int swing = o->train_set[0] & 3;
	int pre = (o->train_set[0] >> DP_TRAIN_PRE_EMPHASIS_SHIFT) & 3;
	return intel_ddi_dp_level_for(swing, pre);
}

static void apply_train_set(struct i915_device *i915, struct intel_output *o,
			    uint8_t pattern)
{
	uint8_t buf[5];
	int level = train_set_level(o);
	if (intel_ddi_level_in_buf_ctl(i915)) {
		uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
		v &= ~DDI_BUF_EMP_MASK;
		v |= DDI_BUF_TRANS_SELECT((uint32_t)level);
		i915_write32(i915, DDI_BUF_CTL(o->port), v);
		(void)i915_read32(i915, DDI_BUF_CTL(o->port));
	} else {
		intel_ddi_set_buf_trans(i915, o, level);
	}
	buf[0] = pattern;
	for (int l = 0; l < 4; l++)
		buf[1 + l] = o->train_set[l];
	if (pattern == DP_TRAINING_PATTERN_DISABLE)
		intel_dp_aux_native_write(&o->aux, DP_TRAINING_PATTERN_SET, buf, 1);
	else
		intel_dp_aux_native_write(&o->aux, DP_TRAINING_PATTERN_SET, buf,
					  (unsigned)(1 + o->lane_count));
}

static uint32_t rd_interval_us(const struct intel_output *o, int eq)
{
	uint32_t v = o->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & 0x7f;
	if (!eq)
		return 100;
	return v ? v * 4000 : 400;
}

int intel_dp_link_train(struct i915_device *i915, struct intel_output *o)
{
	uint8_t lanes = (uint8_t)o->lane_count;
	uint8_t st[3] = { 0, 0, 0 }, adj[2] = { 0, 0 };
	int enhanced = !!(o->dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP);
	int tps3 = !!(o->dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED);

	/* Tell the sink the link, then start pattern 1 at the port and at
	 * the sink together. */
	if (o->nsink_rates && o->edp_dpcd[0] >= 0x03 && o->is_edp) {
		/* eDP 1.4: select by index into the rate table */
		uint8_t idx = 0;
		for (int i = 0; i < o->nsink_rates; i++)
			if (o->sink_rates_khz[i] == o->link_rate_khz)
				idx = (uint8_t)i;
		intel_dp_dpcd_write8(&o->aux, DP_LINK_BW_SET, 0);
		intel_dp_dpcd_write8(&o->aux, DP_LINK_RATE_SET, idx);
	} else {
		intel_dp_dpcd_write8(&o->aux, DP_LINK_BW_SET, link_bw_code(o->link_rate_khz));
	}
	intel_dp_dpcd_write8(&o->aux, DP_LANE_COUNT_SET,
			     (uint8_t)(lanes | (enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0)));
	/* Only claim spread spectrum when the source's PLL actually spreads:
	 * a sink told to expect it while the link is steady tracks a
	 * frequency that never moves, and one not told while the link does
	 * spread loses lock. */
	intel_dp_dpcd_write8(&o->aux, DP_DOWNSPREAD_CTRL, o->ssc ? DP_SPREAD_AMP_0_5 : 0);
	intel_dp_dpcd_write8(&o->aux, DP_MAIN_LINK_CHANNEL_CODING_SET, DP_SET_ANSI_8B10B);

	for (int l = 0; l < 4; l++)
		o->train_set[l] = 0;
	i915_dbg("[drm] i915: DP port %c: training at %u kHz x%u, %s swing table%s (DPCD %x.%x, TPS3 %d)\n",
		'A' + o->port, o->link_rate_khz, lanes,
		(o->is_edp && i915->display.vbt.edp_low_vswing) ? "low-voltage" : "standard",
		o->ssc ? ", spread spectrum" : "", o->dpcd[DP_DPCD_REV] >> 4,
		o->dpcd[DP_DPCD_REV] & 0xf, tps3);
	intel_ddi_dp_tp_enable(i915, o, enhanced);
	intel_ddi_buf_enable(i915, o, lanes, 0);

	/* --- clock recovery --- */
	int cr_ok = 0;
	int voltage_tries = 0, max_swing_tries = 0;
	uint8_t last_set = 0xff;
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_PAT1);
	apply_train_set(i915, o, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	for (int tries = 0; tries < 10; tries++) {
		lapic_delay_us(rd_interval_us(o, 0));
		if (intel_dp_aux_native_read(&o->aux, DP_LANE0_1_STATUS, st, 2) != 2)
			break;
		int all = 1;
		for (int l = 0; l < lanes; l++)
			if (!(lane_status(st, l) & DP_LANE_CR_DONE))
				all = 0;
		if (all) {
			cr_ok = 1;
			break;
		}
		if (intel_dp_aux_native_read(&o->aux, DP_ADJUST_REQUEST_LANE0_1, adj, 2) != 2)
			break;
		if ((o->train_set[0] & DP_TRAIN_MAX_SWING_REACHED) && ++max_swing_tries > 1)
			break;
		adjust_from_request(o, adj);
		if (o->train_set[0] == last_set) {
			if (++voltage_tries >= 5)
				break;
		} else {
			voltage_tries = 0;
			last_set = o->train_set[0];
		}
		apply_train_set(i915, o, DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	}
	if (!cr_ok) {
		kprintf("[drm] i915: DP port %c: clock recovery failed (rate %u, %u lanes, status %02x %02x)\n",
			'A' + o->port, o->link_rate_khz, lanes, st[0], st[1]);
		kprintf("[drm] i915:   DDI_BUF_CTL %08x, DP_TP_CTL %08x, train set %02x, adjust %02x %02x\n",
			i915_read32(i915, DDI_BUF_CTL(o->port)),
			i915_read32(i915, intel_dp_tp_ctl_reg(i915, o)), o->train_set[0], adj[0], adj[1]);
		kprintf("[drm] i915:   DPLL%d, DPLL_CTRL1 %08x, DPLL_CTRL2 %08x, status %08x, AUX errors %u\n",
			o->pll, i915_read32(i915, DPLL_CTRL1), i915_read32(i915, DPLL_CTRL2),
			i915_read32(i915, DPLL_STATUS), o->aux.errors);
		apply_train_set(i915, o, DP_TRAINING_PATTERN_DISABLE);
		return -EIO;
	}

	/* --- channel equalisation --- */
	uint8_t pat = tps3 ? DP_TRAINING_PATTERN_3 : DP_TRAINING_PATTERN_2;
	intel_ddi_set_train_pattern(i915, o, tps3 ? DP_TP_CTL_LINK_TRAIN_PAT3 :
						   DP_TP_CTL_LINK_TRAIN_PAT2);
	apply_train_set(i915, o, pat | DP_LINK_SCRAMBLING_DISABLE);
	int eq_ok = 0;
	for (int tries = 0; tries < 6; tries++) {
		lapic_delay_us(rd_interval_us(o, 1));
		if (intel_dp_aux_native_read(&o->aux, DP_LANE0_1_STATUS, st, 3) != 3)
			break;
		int all = 1;
		for (int l = 0; l < lanes; l++) {
			int s = lane_status(st, l);
			if (!(s & DP_LANE_CR_DONE))
				all = -1;
			if (!(s & DP_LANE_CHANNEL_EQ_DONE) || !(s & DP_LANE_SYMBOL_LOCKED))
				all = all > 0 ? 0 : all;
		}
		if (all < 0)
			break; /* clock recovery lost */
		if (all && (st[2] & DP_INTERLANE_ALIGN_DONE)) {
			eq_ok = 1;
			break;
		}
		if (intel_dp_aux_native_read(&o->aux, DP_ADJUST_REQUEST_LANE0_1, adj, 2) != 2)
			break;
		adjust_from_request(o, adj);
		apply_train_set(i915, o, pat | DP_LINK_SCRAMBLING_DISABLE);
	}
	if (!eq_ok) {
		kprintf("[drm] i915: DP port %c: channel equalisation failed (status %02x %02x %02x)\n",
			'A' + o->port, st[0], st[1], st[2]);
		apply_train_set(i915, o, DP_TRAINING_PATTERN_DISABLE);
		return -EIO;
	}
	/* idle pattern, then normal operation; the sink stops training */
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_IDLE);
	intel_ddi_wait_idle_done(i915, o);
	apply_train_set(i915, o, DP_TRAINING_PATTERN_DISABLE);
	intel_ddi_set_train_pattern(i915, o, DP_TP_CTL_LINK_TRAIN_NORMAL);
	i915_dbg("[drm] i915: DP port %c: link trained at %u kHz x%u (swing %u, pre-emphasis %u)\n",
		'A' + o->port, o->link_rate_khz, lanes, o->train_set[0] & 3,
		(o->train_set[0] >> DP_TRAIN_PRE_EMPHASIS_SHIFT) & 3);
	return 0;
}

void intel_dp_link_off(struct i915_device *i915, struct intel_output *o)
{
	intel_ddi_buf_disable(i915, o);
}
