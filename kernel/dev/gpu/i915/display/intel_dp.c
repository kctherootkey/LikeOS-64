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
// With I915_FEAT_DP_HELPERS the sink is read through the DisplayPort
// helper library: the receiver capabilities (and their extended copy),
// the sink or branch identification and its quirks, link-training
// repeaters, a branch device's sink count and downstream ports (whose
// limits intel_dp_mode_valid_downstream() applies), the DSC and FEC
// capabilities, and the link status the hotplug check reads.  The
// training itself is intel_dp_link_training.c's; the driver's own
// detection and training stay here for I915_FEAT_DP_HELPERS 0.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dp_link_training.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

uint32_t intel_dp_link_bw_khz(uint8_t bw)
{
	return (uint32_t)bw * 27000;
}

#if !I915_FEAT_DP_HELPERS
static uint8_t link_bw_code(uint32_t khz)
{
	return (uint8_t)(khz / 27000);
}
#endif

/* ---- detection --------------------------------------------------------- */

static int hpd_live(struct i915_device *i915, int port)
{
	return intel_hpd_live(i915, port);
}

#if !I915_FEAT_DP_HELPERS

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
		/* eDP 1.4: a table of rates in 200 kHz units of the bit rate;
		 * the driver counts the link in symbol clock (a tenth of it),
		 * so 13500 (2.7 Gbit/s) is 270000 */
		if (o->dpcd[DP_DPCD_REV] >= 0x13 || o->edp_dpcd[0] >= 0x03) {
			uint8_t r[16];
			if (intel_dp_aux_native_read(&o->aux, DP_SUPPORTED_LINK_RATES,
						     r, 16) == 16) {
				for (int i = 0; i < 8; i++) {
					uint32_t v = (uint32_t)r[i * 2] |
						     ((uint32_t)r[i * 2 + 1] << 8);
					if (!v)
						break;
					o->sink_rates_khz[o->nsink_rates++] = v * 20;
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
		/* The panel is wired; power its logic and its AUX channel's
		 * well, and ask. */
		int ch = o->aux.port >= 0 && o->aux.port < 9 ? o->aux.port : 0;
		enum intel_power_domain aux = (enum intel_power_domain)(INTEL_PW_AUX_A + ch);
		intel_power_get(i915, aux);
		intel_pps_vdd_on(i915, o);
		int rc = read_dpcd(i915, o);
		intel_power_put(i915, aux);
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

#else /* I915_FEAT_DP_HELPERS */

/* ---- detection on the helper library -------------------------------------- */

/* The rates of the DP_MAX_LINK_RATE method, or the table of a sink that
 * is known to do 3.24 Gbit/s although it says less; capped at what the
 * repeaters in front of it pass on. */
static void set_dpcd_sink_rates(struct intel_output *o)
{
	static const uint32_t dp_rates[] = { 162000, 270000, 540000, 810000 };
	int max_rate, max_lttpr_rate;
	unsigned i;

	if (drm_dp_has_quirk(&o->dp_desc, DP_DPCD_QUIRK_CAN_DO_MAX_LINK_RATE_3_24_GBPS)) {
		/* e.g. the 15-inch eDP Retina panel of the 2017 MacBook Pro */
		static const uint32_t quirk_rates[] = { 162000, 270000, 324000 };

		for (i = 0; i < ARRAY_SIZE(quirk_rates); i++)
			o->sink_rates_khz[i] = quirk_rates[i];
		o->nsink_rates = (int)ARRAY_SIZE(quirk_rates);
		return;
	}

	max_rate = drm_dp_max_link_rate(o->dpcd);
	max_lttpr_rate = intel_dp_lttpr_max_link_rate(o);
	if (max_lttpr_rate)
		max_rate = min(max_rate, max_lttpr_rate);

	for (i = 0; i < ARRAY_SIZE(dp_rates); i++) {
		if ((int)dp_rates[i] > max_rate)
			break;
		o->sink_rates_khz[i] = dp_rates[i];
	}
	o->nsink_rates = (int)i;
}

static void set_sink_rates(struct intel_output *o)
{
	set_dpcd_sink_rates(o);
	if (o->nsink_rates)
		return;
	kprintf("[drm] i915: DP port %c: the sink names no link rate; using 1.62 Gbit/s\n",
		'A' + o->port);
	o->sink_rates_khz[0] = 162000;
	o->nsink_rates = 1;
}

/* eDP 1.4: the panel's own table of rates (DP_SUPPORTED_LINK_RATES, 200
 * kHz units of the bit rate; the driver counts the link in symbol clock,
 * a tenth of it, so 13500 (2.7 Gbit/s) is 270000), selected by index.
 * Otherwise, or with an empty table, the DP_MAX_LINK_RATE method. */
static void edp_set_sink_rates(struct intel_output *o)
{
	o->nsink_rates = 0;
	o->use_rate_select = 0;
	if (o->edp_dpcd[0] >= DP_EDP_14) {
		uint8_t r[DP_MAX_SUPPORTED_RATES * 2];
		int i;

		if (drm_dp_dpcd_read_data(&o->aux.dp, DP_SUPPORTED_LINK_RATES, r, sizeof(r)) < 0)
			mm_memset(r, 0, sizeof(r));
		for (i = 0; i < DP_MAX_SUPPORTED_RATES; i++) {
			uint32_t v = (uint32_t)r[i * 2] | ((uint32_t)r[i * 2 + 1] << 8);
			if (!v)
				break;
			o->sink_rates_khz[i] = v * 200 / 10;
		}
		o->nsink_rates = i;
	}
	if (o->nsink_rates)
		o->use_rate_select = 1;
	else
		set_sink_rates(o);
}

/* The sink's DSC and FEC capabilities, where the source has engines;
 * logged when they differ from what was logged last for this output. */
static void detect_dsc_caps(struct i915_device *i915, struct intel_output *o)
{
	struct intel_dsc_sink_caps *caps = &o->dsc_caps;
	uint32_t digest = 2166136261u;
	char tag[24] = "[drm] i915: DP port ?";

	intel_dp_dsc_source_init(i915);
	if (!i915->display.dsc_src.has_dsc) {
		intel_dsc_sink_caps_clear(caps);
		return;
	}
	if (o->is_edp)
		intel_dsc_read_edp_sink_caps(caps, o->edp_dpcd[0], intel_dsc_dpcd_read_drm_aux,
					     &o->aux.dp);
	else
		intel_dsc_read_dp_sink_caps(caps, o->dpcd[DP_DPCD_REV], &o->dp_desc,
					    drm_dp_is_branch(o->dpcd), intel_dsc_dpcd_read_drm_aux,
					    &o->aux.dp);
	for (unsigned i = 0; i < sizeof(caps->dsc_dpcd); i++)
		digest = (digest ^ caps->dsc_dpcd[i]) * 16777619u;
	digest = (digest ^ caps->fec_capability) * 16777619u;
	digest = (digest ^ (uint32_t)(caps->valid ? 1 : 2)) * 16777619u;
	if (digest == o->dsc_caps_logged)
		return;
	o->dsc_caps_logged = digest;
	tag[20] = (char)('A' + o->port);
	intel_dsc_log_sink_caps(tag, caps);
}

/* The throw-away read before every DPCD read is for the sinks whose EDID
 * says they need it (they corrupt the first access after power save); a
 * panel never does. */
static void dp_set_probe(struct i915_device *i915, struct intel_output *o)
{
	bool need = false;

	if (!o->is_edp && o->conn >= 0)
		need = drm_edid_has_quirk(&i915->drm.conn[o->conn].display_info,
					  DRM_EDID_QUIRK_DP_DPCD_PROBE);
	drm_dp_dpcd_set_probe(&o->aux.dp, need);
}

static int edp_init_dpcd(struct i915_device *i915, struct intel_output *o)
{
	u8 dpcd[DP_RECEIVER_CAP_SIZE];

	if (intel_dp_read_receiver_caps(o, dpcd))
		return -EIO;
	mm_memcpy(o->dpcd, dpcd, sizeof(dpcd));
	drm_dp_read_desc(&o->aux.dp, &o->dp_desc, drm_dp_is_branch(o->dpcd));
	/* The display control registers, whatever DP_EDP_CONFIGURATION_CAP
	 * says: some panels do not set DPCD_DISPLAY_CONTROL_CAPABLE but need
	 * the eDP 1.4 rate table, and the registers read zero where absent. */
	mm_memset(o->edp_dpcd, 0, sizeof(o->edp_dpcd));
	if (drm_dp_dpcd_read(&o->aux.dp, DP_EDP_DPCD_REV, o->edp_dpcd, sizeof(o->edp_dpcd)) !=
	    (ssize_t)sizeof(o->edp_dpcd))
		mm_memset(o->edp_dpcd, 0, sizeof(o->edp_dpcd));
	edp_set_sink_rates(o);
	detect_dsc_caps(i915, o);
	return 0;
}

static bool dp_has_sink_count(struct i915_device *i915, struct intel_output *o)
{
	if (o->conn < 0)
		return false;
	return drm_dp_read_sink_count_cap(&i915->drm.conn[o->conn], o->dpcd, &o->dp_desc);
}

/* The receiver (after the repeaters), the identification, the rates, a
 * branch device's sink count and downstream ports.  false: nothing to
 * drive (no answer, or a branch device with no sink behind it). */
static bool dp_get_dpcd(struct i915_device *i915, struct intel_output *o)
{
	if (intel_dp_init_lttpr_and_dprx_caps(i915, o, o->active) < 0)
		return false;
	drm_dp_read_desc(&o->aux.dp, &o->dp_desc, drm_dp_is_branch(o->dpcd));
	set_sink_rates(o);
	o->use_rate_select = 0;

	o->sink_count = -1;
	if (dp_has_sink_count(i915, o)) {
		int ret = drm_dp_read_sink_count(&o->aux.dp);
		if (ret < 0)
			return false;
		o->sink_count = ret;
		/* SINK_COUNT 0 behind a downstream port: an adaptor or dock
		 * with no display on it. */
		if (!o->sink_count)
			return false;
	}
	return drm_dp_read_downstream_info(&o->aux.dp, o->dpcd, o->downstream_ports) == 0;
}

/* A bare one-byte EDID read: is anything on the DDC behind the adaptor? */
static bool dp_probe_ddc(struct intel_output *o)
{
	uint8_t off = 0, b = 0;
	struct i2c_msg m[2] = {
		{ .addr = 0x50, .flags = 0, .len = 1, .buf = &off },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 1, .buf = &b },
	};

	return i2c_transfer(intel_dp_aux_ddc(&o->aux), m, 2) == 2;
}

/* What is on an external port: DRM_MODE_CONNECTED, _DISCONNECTED, or
 * _UNKNOWNCONNECTION for an adaptor whose display cannot be seen (VGA,
 * no EDID). */
static int dp_detect_dpcd(struct i915_device *i915, struct intel_output *o)
{
	uint8_t type;

	if (!dp_get_dpcd(i915, o))
		return DRM_MODE_DISCONNECTED;
	/* no downstream port: the sink itself */
	if (!drm_dp_is_branch(o->dpcd))
		return DRM_MODE_CONNECTED;
	/* a branch device that reports its sinks' hotplug */
	if (dp_has_sink_count(i915, o) && (o->downstream_ports[0] & DP_DS_PORT_HPD))
		return o->sink_count ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	/* no HPD: ask the DDC gently */
	if (dp_probe_ddc(o))
		return DRM_MODE_CONNECTED;
	/* the port types that may well have a display without an EDID */
	if (o->dpcd[DP_DPCD_REV] >= 0x11) {
		type = o->downstream_ports[0] & DP_DS_PORT_TYPE_MASK;
		if (type == DP_DS_PORT_TYPE_VGA || type == DP_DS_PORT_TYPE_NON_EDID)
			return DRM_MODE_UNKNOWNCONNECTION;
	} else {
		type = o->dpcd[DP_DOWNSTREAMPORT_PRESENT] & DP_DWN_STRM_PORT_TYPE_MASK;
		if (type == DP_DWN_STRM_PORT_TYPE_ANALOG || type == DP_DWN_STRM_PORT_TYPE_OTHER)
			return DRM_MODE_UNKNOWNCONNECTION;
	}
	/* anything else is out of spec */
	i915_dbg("[drm] i915: DP port %c: broken branch device, ignored\n", 'A' + o->port);
	return DRM_MODE_DISCONNECTED;
}

static void dp_clear_sink(struct intel_output *o)
{
	mm_memset(&o->dfp, 0, sizeof(o->dfp));
	mm_memset(o->downstream_ports, 0, sizeof(o->downstream_ports));
	intel_dsc_sink_caps_clear(&o->dsc_caps);
}

int intel_dp_detect(struct i915_device *i915, struct intel_output *o)
{
	if (o->is_edp) {
		/* The panel is wired; power its logic and its AUX channel's
		 * well, and ask. */
		int ch = o->aux.port >= 0 && o->aux.port < 9 ? o->aux.port : 0;
		enum intel_power_domain aux = (enum intel_power_domain)(INTEL_PW_AUX_A + ch);
		intel_power_get(i915, aux);
		intel_pps_vdd_on(i915, o);
		dp_set_probe(i915, o);
		int rc = edp_init_dpcd(i915, o);
		intel_power_put(i915, aux);
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
	int status = dp_detect_dpcd(i915, o);
	if (status == DRM_MODE_DISCONNECTED)
		dp_clear_sink(o);
	else
		detect_dsc_caps(i915, o);
	dp_set_probe(i915, o);
	/* an adaptor whose display cannot be seen counts as connected */
	o->detected = (status != DRM_MODE_DISCONNECTED);
	return o->detected;
}

/* A branch device's limits, from its downstream port caps and the EDID
 * behind it. */
static void dp_update_dfp(struct intel_output *o)
{
	const struct drm_edid *e = o->edid_len > 0 ?
					   drm_edid_alloc(o->edid, (size_t)o->edid_len) : NULL;

	o->dfp.max_bpc = drm_dp_downstream_max_bpc(o->dpcd, o->downstream_ports, e);
	o->dfp.max_dotclock = drm_dp_downstream_max_dotclock(o->dpcd, o->downstream_ports);
	o->dfp.min_tmds_clock = drm_dp_downstream_min_tmds_clock(o->dpcd, o->downstream_ports, e);
	o->dfp.max_tmds_clock = drm_dp_downstream_max_tmds_clock(o->dpcd, o->downstream_ports, e);
	o->dfp.pcon_max_frl_bw = drm_dp_get_pcon_max_frl_bw(o->dpcd, o->downstream_ports);
	if (e)
		drm_edid_free(e);
	if (drm_dp_is_branch(o->dpcd))
		i915_dbg("[drm] i915: DP port %c: downstream max bpc %d, max dot clock %d, TMDS %d-%d kHz, FRL %d Gbit/s\n",
			 'A' + o->port, o->dfp.max_bpc, o->dfp.max_dotclock,
			 o->dfp.min_tmds_clock, o->dfp.max_tmds_clock, o->dfp.pcon_max_frl_bw);
}

int intel_dp_read_edid(struct i915_device *i915, struct intel_output *o)
{
	(void)i915;
	/* the counts DP compliance asks about are the EDID read's own */
	o->aux.dp.i2c_nack_count = 0;
	o->aux.dp.i2c_defer_count = 0;
	int blocks = drm_edid_read(intel_dp_aux_ddc(&o->aux), o->edid,
				   sizeof(o->edid) / DRM_EDID_BLOCK);
	if (blocks <= 0) {
		o->edid_len = 0;
		if (!o->is_edp)
			dp_update_dfp(o);
		return blocks < 0 ? blocks : -ENOENT;
	}
	o->edid_len = blocks * DRM_EDID_BLOCK;
	if (!o->is_edp)
		dp_update_dfp(o);
	return 0;
}

#endif /* I915_FEAT_DP_HELPERS */

/* ---- the link ------------------------------------------------------------ */

/* Bandwidth: a lane at rate R carries R/10 bytes/s (8b/10b); 3 bytes per
 * pixel at 8 bpc.  The mode needs clock * 3 bytes/s, with some margin. */
static int link_carries(uint32_t rate_khz, int lanes, uint32_t pixel_khz, int bpp)
{
	/* The rate is the symbol clock (a tenth of the bit rate), and with
	 * 8b/10b each symbol carries one byte: a lane moves rate_khz kB/s. */
	uint64_t link = (uint64_t)rate_khz * lanes;
	uint64_t need = (uint64_t)pixel_khz * bpp / 8;
	/* both in kB/s; keep 1% headroom */
	return link * 99 >= need * 100;
}

/* The board's highest link rate for the port (from the VBT; 0: none). */
static int vbt_rate_ok(const struct intel_output *o, uint32_t rate_khz)
{
	return !o->vbt_max_link_rate_khz || rate_khz <= o->vbt_max_link_rate_khz;
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
	/* (The VBT's eDP lane count is the firmware's fast-training hint,
	 * not a limit: the panel's DPCD says what it takes.  The child
	 * device's maximum lane count is the board's wiring, and is.) */
	if (o->vbt_max_lanes && o->vbt_max_lanes < max_lanes)
		max_lanes = o->vbt_max_lanes;
	if (o->is_tc && o->tc_owned && intel_tc_max_lanes(i915, o) < max_lanes)
		max_lanes = intel_tc_max_lanes(i915, o); /* what the FIA granted */
#if I915_FEAT_DP_HELPERS
	/* and what the repeaters in front of the sink pass on */
	if (intel_dp_lttpr_max_lane_count(o) && intel_dp_lttpr_max_lane_count(o) < max_lanes)
		max_lanes = intel_dp_lttpr_max_lane_count(o);
#endif
	/* Spread spectrum where the sink takes it: an embedded panel's link
	 * normally runs with it, and the source's PLL has to agree with what
	 * the sink is told. */
	/* (only the PHY PLLs of Meteor Lake and DG2 can spread: the
	 * Gen9-13 port PLLs never do) */
	o->ssc = o->is_edp && (o->dpcd[DP_MAX_DOWNSPREAD] & 1) &&
		 (i915->display.model == INTEL_DISPLAY_MTL || i915->display.model == INTEL_DISPLAY_DG2);
#if I915_FEAT_DSC
	/* A compressed stream was sized for one link (intel_dsc_compute_config()
	 * just ran, within the same rate and lane limits as above). */
	if (o->pipe >= 0 && o->pipe < INTEL_MAX_PIPES && i915->display.pipes[o->pipe].dsc.enable &&
	    i915->display.pipes[o->pipe].dsc.lane_count <= max_lanes &&
	    vbt_rate_ok(o, i915->display.pipes[o->pipe].dsc.link_rate_khz)) {
		o->link_rate_khz = i915->display.pipes[o->pipe].dsc.link_rate_khz;
		o->lane_count = i915->display.pipes[o->pipe].dsc.lane_count;
		return 0;
	}
#endif

	/* The configuration the firmware had this port running at, if it
	 * carries the mode: the panel demonstrably trains at it, and
	 * nothing about the board is being guessed. */
	if (o->fw_link_rate_khz && o->fw_lanes >= 1 && o->fw_lanes <= max_lanes &&
	    vbt_rate_ok(o, o->fw_link_rate_khz) &&
	    intel_dpll_output_rate_supported(i915, o, o->fw_link_rate_khz) &&
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
			if (!vbt_rate_ok(o, rate) || !intel_dpll_output_rate_supported(i915, o, rate))
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
		if (vbt_rate_ok(o, rate) && intel_dpll_output_rate_supported(i915, o, rate)) {
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
	    intel_dpll_output_rate_supported(i915, o, o->fw_link_rate_khz) &&
	    link_carries(o->fw_link_rate_khz, o->fw_lanes, mode->clock, bpp)) {
		kprintf("[drm] i915: port %c: no link fits the limits read here; using the one the firmware ran (%u kHz x%d)\n",
			'A' + o->port, o->fw_link_rate_khz, o->fw_lanes);
		o->link_rate_khz = o->fw_link_rate_khz;
		o->lane_count = o->fw_lanes;
		return 0;
	}
	return -ENOSPC;
}

/* The widest link the sink and the port allow. */
int intel_dp_max_lanes(const struct intel_output *o)
{
	int lanes = o->dpcd[DP_MAX_LANE_COUNT] & 0x1f;
	if (o->port == PORT_A && !o->four_lane_strap && lanes > 2)
		lanes = 2;
	if (o->is_tc && o->tc_lanes && (int)o->tc_lanes < lanes)
		lanes = (int)o->tc_lanes;
#if I915_FEAT_DP_HELPERS
	if (intel_dp_lttpr_max_lane_count(o) && intel_dp_lttpr_max_lane_count(o) < lanes)
		lanes = intel_dp_lttpr_max_lane_count(o);
#endif
	if (o->vbt_max_lanes && o->vbt_max_lanes < lanes)
		lanes = o->vbt_max_lanes;
	if (lanes < 1)
		lanes = 1;
	if (lanes > 4)
		lanes = 4;
	return lanes;
}

/* Can the sink's link carry the mode at all?  The rate and width chosen
 * at enable time are the best that train; this is the ceiling. */
int intel_dp_link_carries(struct i915_device *i915, const struct intel_output *o,
			  const struct drm_mode_modeinfo *m)
{
	uint32_t rate = 0;
	for (int r = 0; r < o->nsink_rates; r++)
		if (o->sink_rates_khz[r] > rate && vbt_rate_ok(o, o->sink_rates_khz[r]) &&
		    intel_dpll_output_rate_supported(i915, o, o->sink_rates_khz[r]))
			rate = o->sink_rates_khz[r];
	if (!rate)
		return 1; /* unknown yet: the enable path decides */
	int lanes = intel_dp_max_lanes(o);
	uint64_t link = (uint64_t)rate * (uint32_t)lanes; /* kB/s, as above */
	uint64_t need = (uint64_t)m->clock * 24 / 8;
	return link * 99 >= need * 100;
}

#if !I915_FEAT_DP_HELPERS
/* After the hotplug worker's re-probe: nothing to repair yet (a link that
 * was lost is set up again by the next mode set). */
int intel_dp_hpd_check(struct i915_device *i915, struct intel_output *o)
{
	(void)i915;
	(void)o;
	return 0;
}
#else
/*
 * After the hotplug worker's re-probe of an external port: a sink that
 * dropped its link (it went to sleep, was replugged, reset itself) while
 * the pipe kept running shows it in its link status.  The link is then
 * trained again where that can be done under a running pipe; where it
 * cannot, or the training fails, the connector's link-status goes BAD and
 * clients are told to set the mode again.  A sink that does not answer
 * yet is looked at again after another debounce, a few times.
 */
#define DP_HPD_CHECK_RETRIES 3

int intel_dp_hpd_check(struct i915_device *i915, struct intel_output *o)
{
	u8 ls[DP_LINK_STATUS_SIZE];
	int rc;

	/* only a lit link to an external sink still reported there */
	if (!o->active || o->is_edp || o->type != INTEL_OUTPUT_DP || !o->detected ||
	    !hpd_live(i915, o->port)) {
		o->hpd_check_retries = 0;
		return 0;
	}
	if (drm_dp_dpcd_read_link_status(&o->aux.dp, ls) < 0) {
		if (++o->hpd_check_retries <= DP_HPD_CHECK_RETRIES)
			return 1;
		o->hpd_check_retries = 0;
		return 0;
	}
	o->hpd_check_retries = 0;
	if (drm_dp_channel_eq_ok(ls, o->lane_count))
		return 0;
	kprintf("[drm] i915: DP port %c: the sink lost the link (status %02x %02x %02x); training it again\n",
		'A' + o->port, ls[0], ls[1], ls[2]);
	rc = intel_dp_retrain_link(i915, o);
	if (rc == 0)
		return 0;
	if (o->conn >= 0) {
		drm_connector_set_link_status_property(&i915->drm.conn[o->conn],
						       DRM_MODE_LINK_STATUS_BAD);
		drm_connector_hotplug(&i915->drm, o->conn);
	}
	kprintf("[drm] i915: DP port %c: link %s; the mode must be set again\n", 'A' + o->port,
		rc == -EOPNOTSUPP ? "not retrainable under a running pipe here" : "training failed");
	return 0;
}
#endif

/* A branch device's limits for `mode' (RGB, 8 bpc): a protocol
 * converter's FRL bandwidth, the dot clock, the TMDS clock range of the
 * HDMI/DVI port (and of the sink behind it, when its EDID says). */
enum drm_mode_status intel_dp_mode_valid_downstream(struct i915_device *i915,
						    const struct intel_output *o,
						    const struct drm_display_mode *mode)
{
	int target_clock = mode->clock;
	int tmds_clock, min_tmds_clock, max_tmds_clock;

	if (!o || o->is_edp)
		return MODE_OK;
	/* a protocol converter running FRL: its bandwidth (8 bpc) */
	if (o->dfp.pcon_max_frl_bw) {
		int64_t target_bw = (int64_t)target_clock * 8 * 3;
		int64_t max_frl_bw = (int64_t)o->dfp.pcon_max_frl_bw * 1000000;

		return target_bw > max_frl_bw ? MODE_CLOCK_HIGH : MODE_OK;
	}
	if (o->dfp.max_dotclock && target_clock > o->dfp.max_dotclock)
		return MODE_CLOCK_HIGH;

	/* at 8 bpc the TMDS clock is the pixel clock */
	tmds_clock = target_clock;
	min_tmds_clock = o->dfp.min_tmds_clock;
	max_tmds_clock = o->dfp.max_tmds_clock;
	/* the sink's own TMDS limit only behind an HDMI adaptor */
	if (max_tmds_clock && o->conn >= 0) {
		int sink_max = i915->drm.conn[o->conn].display_info.max_tmds_clock;
		if (sink_max)
			max_tmds_clock = min(max_tmds_clock, sink_max);
	}
	if (min_tmds_clock && tmds_clock < min_tmds_clock)
		return MODE_CLOCK_LOW;
	if (max_tmds_clock && tmds_clock > max_tmds_clock)
		return MODE_CLOCK_HIGH;
	return MODE_OK;
}

void intel_dp_dsc_source_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	int ver = i915->info->display_ver;
	uint32_t dfsm = 0, de_cap = 0;

	if (d->dsc_src.display_ver)
		return;
	if (ver >= 10 && ver <= 12)
		dfsm = i915_read32(i915, INTEL_DSC_DFSM_REG);
	if (ver >= 20)
		de_cap = i915_read32(i915, INTEL_DSC_DE_CAP_REG);
	intel_dsc_source_caps_init(&d->dsc_src, intel_display_verx100(i915), dfsm, de_cap);
}

int intel_dp_set_msa_timing_par_ignore(struct intel_output *o, bool enable)
{
	o->msa_timing_par_ignore = enable ? 1 : 0;
	/* the next training writes it with the rest of the link setup */
	if (!o->active)
		return 0;
	uint8_t v = (uint8_t)((o->ssc ? DP_SPREAD_AMP_0_5 : 0) |
			      (enable ? DP_MSA_TIMING_PAR_IGNORE_EN : 0));
	return intel_dp_dpcd_write8(&o->aux, DP_DOWNSPREAD_CTRL, v) ? -EIO : 0;
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
#if I915_FEAT_DP_HELPERS
		/* Display version 14 on: grant the longer wake-up time a sink
		 * (or, in non-transparent mode, a repeater) asks for. */
		if (I915_FEAT_DP_LTTPR && i915->info->display_ver >= 14)
			drm_dp_lttpr_wake_timeout_setup(&o->aux.dp, intel_dp_lttpr_transparent_mode(o));
#endif
	} else {
		intel_dp_dpcd_write8(&o->aux, DP_SET_POWER, DP_SET_POWER_D3);
	}
}

/* With the helpers the link is trained in intel_dp_link_training.c. */
#if !I915_FEAT_DP_HELPERS

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
	intel_dp_dpcd_write8(&o->aux, DP_DOWNSPREAD_CTRL,
			     (uint8_t)((o->ssc ? DP_SPREAD_AMP_0_5 : 0) |
				       (o->msa_timing_par_ignore ? DP_MSA_TIMING_PAR_IGNORE_EN : 0)));
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
		if (i915->display.model == INTEL_DISPLAY_SKL)
			kprintf("[drm] i915:   DPLL%d, DPLL_CTRL1 %08x, DPLL_CTRL2 %08x, status %08x, AUX errors %u\n",
				o->pll, i915_read32(i915, DPLL_CTRL1), i915_read32(i915, DPLL_CTRL2),
				i915_read32(i915, DPLL_STATUS), o->aux.errors);
		else
			kprintf("[drm] i915:   PLL %d, AUX errors %u\n", o->pll, o->aux.errors);
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

#endif /* !I915_FEAT_DP_HELPERS */

void intel_dp_link_off(struct i915_device *i915, struct intel_output *o)
{
	intel_ddi_buf_disable(i915, o);
}
