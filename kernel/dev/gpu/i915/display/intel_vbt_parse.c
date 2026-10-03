// LikeOS -- the Video BIOS Tables (pure: the host tests compile it).
//
// The firmware describes the board's display wiring in a table the
// driver cannot do without: which DDI ports exist and what they carry,
// which AUX channel and DDC pin each uses, the internal panel's timing,
// its power-sequencing delays and its backlight.  The VBT is a header,
// a BIOS data block header, and a chain of tagged blocks; the layout of
// several blocks changed with the BDB version, which the parser honours
// where the fields it needs moved.
//
// What a child device's port and AUX channel codes mean depends on the
// platform: Rocket Lake and DG1 name their PHYs (A, B, then the two
// "Type-C" combo ports as C and D), Alder Lake-S its ports A and TC1-4 as
// A-E, and from Alder Lake-P on the Type-C ports are F to I (and AUX F
// to I) while D and E are the extra combo ports.  The parser maps them
// onto the driver's port numbers (PORT_TC1.. for the Type-C ports from
// Tiger Lake on) and AUX channel indices (USBC1.. after A-C).  Display
// versions 14 to 35 (Meteor Lake to Nova Lake, Battlemage included) keep
// Alder Lake-P's numbering; BDB 264 adds the flag for a port whose PHY
// is a dedicated external one rather than a Type-C subsystem's.
//
// Besides the wiring, a child device can limit its port (the TMDS clock
// from BDB 204, the DisplayPort link rate from 216 and lanes from 244),
// mark it as a DP++ connector, and ask for stream compression with one
// of the sixteen entries of the compression parameters block (BDB 198+);
// the LFP power block says whether the panel may run variable refresh
// (BDB 233+).  These are parsed for the features that use them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/i915_device_info.h>
#include <kernel/dev/gpu/drm_edid.h>

#ifndef EINVAL
#define EINVAL 22
#endif

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Block ids */
#define BDB_GENERAL_FEATURES 1
#define BDB_GENERAL_DEFINITIONS 2
#define BDB_DRIVER_FEATURES 12
#define BDB_EDP 27
#define BDB_LVDS_OPTIONS 40
#define BDB_LVDS_LFP_DATA_PTRS 41
#define BDB_LVDS_LFP_DATA 42
#define BDB_LFP_BACKLIGHT 43
#define BDB_LFP_POWER 44
#define BDB_COMPRESSION_PARAMETERS 56 /* BDB 213+; the earlier id is not read */
#define BDB_GENERIC_DTD 58

/* Child device types (device_type field bits) */
#define DEVICE_TYPE_INTERNAL_CONNECTOR (1 << 12)
#define DEVICE_TYPE_NOT_HDMI_OUTPUT (1 << 11)
#define DEVICE_TYPE_MIPI_OUTPUT (1 << 10)
#define DEVICE_TYPE_COMPOSITE_OUTPUT (1 << 9)
#define DEVICE_TYPE_DUAL_CHANNEL (1 << 8)
#define DEVICE_TYPE_TMDS_DVI_SIGNALING (1 << 4)
#define DEVICE_TYPE_DISPLAYPORT_OUTPUT (1 << 2)
#define DEVICE_TYPE_DIGITAL_OUTPUT (1 << 1)
#define DEVICE_TYPE_ANALOG_OUTPUT (1 << 0)

/* DVO port codes in the child device */
#define DVO_PORT_HDMIA 0
#define DVO_PORT_HDMIB 1
#define DVO_PORT_HDMIC 2
#define DVO_PORT_HDMID 3
#define DVO_PORT_LVDS 4
#define DVO_PORT_TV 5
#define DVO_PORT_CRT 6
#define DVO_PORT_DPB 7
#define DVO_PORT_DPC 8
#define DVO_PORT_DPD 9
#define DVO_PORT_DPA 10
#define DVO_PORT_DPE 11
#define DVO_PORT_HDMIE 12
#define DVO_PORT_DPF 13
#define DVO_PORT_HDMIF 14
#define DVO_PORT_DPG 15
#define DVO_PORT_HDMIG 16
#define DVO_PORT_DPH 17
#define DVO_PORT_HDMIH 18
#define DVO_PORT_DPI 19
#define DVO_PORT_HDMII 20

/* The platform the table is read for. */
struct vbt_ctx {
	int display_ver;
	int platform;
};

/* The DVO port codes each driver port answers to (HDMI, DP), by
 * platform. */
static const int8_t port_map_direct[][2] = {
	{ DVO_PORT_HDMIA, DVO_PORT_DPA }, { DVO_PORT_HDMIB, DVO_PORT_DPB },
	{ DVO_PORT_HDMIC, DVO_PORT_DPC }, { DVO_PORT_HDMID, DVO_PORT_DPD },
	{ DVO_PORT_HDMIE, DVO_PORT_DPE }, { DVO_PORT_HDMIF, DVO_PORT_DPF },
	{ DVO_PORT_HDMIG, DVO_PORT_DPG }, { DVO_PORT_HDMIH, DVO_PORT_DPH },
	{ DVO_PORT_HDMII, DVO_PORT_DPI },
};
/* Rocket Lake, DG1: PHYs A-D are ports A, B, TC1, TC2. */
static const int8_t port_map_rkl[][2] = {
	{ DVO_PORT_HDMIA, DVO_PORT_DPA }, { DVO_PORT_HDMIB, DVO_PORT_DPB },
	{ -1, -1 }, { DVO_PORT_HDMIC, DVO_PORT_DPC }, { DVO_PORT_HDMID, DVO_PORT_DPD },
	{ -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 },
};
/* Alder Lake-S: ports A, TC1-TC4. */
static const int8_t port_map_adls[][2] = {
	{ DVO_PORT_HDMIA, DVO_PORT_DPA }, { -1, -1 }, { -1, -1 },
	{ DVO_PORT_HDMIB, DVO_PORT_DPB }, { DVO_PORT_HDMIC, DVO_PORT_DPC },
	{ DVO_PORT_HDMID, DVO_PORT_DPD }, { DVO_PORT_HDMIE, DVO_PORT_DPE },
	{ -1, -1 }, { -1, -1 },
};
/* Display version 13+: A-C, TC1-TC4 (F-I), and D/E after them. */
static const int8_t port_map_xelpd[][2] = {
	{ DVO_PORT_HDMIA, DVO_PORT_DPA }, { DVO_PORT_HDMIB, DVO_PORT_DPB },
	{ DVO_PORT_HDMIC, DVO_PORT_DPC }, { DVO_PORT_HDMIF, DVO_PORT_DPF },
	{ DVO_PORT_HDMIG, DVO_PORT_DPG }, { DVO_PORT_HDMIH, DVO_PORT_DPH },
	{ DVO_PORT_HDMII, DVO_PORT_DPI }, { DVO_PORT_HDMID, DVO_PORT_DPD },
	{ DVO_PORT_HDMIE, DVO_PORT_DPE },
};

static int dvo_port_to_ddi(const struct vbt_ctx *ctx, uint8_t dvo, int *is_dp)
{
	const int8_t (*map)[2] = port_map_direct;

	if (ctx->display_ver >= 13)
		map = port_map_xelpd;
	else if (ctx->platform == I915_PLATFORM_ALDERLAKE_S)
		map = port_map_adls;
	else if (ctx->platform == I915_PLATFORM_DG1 || ctx->platform == I915_PLATFORM_ROCKETLAKE)
		map = port_map_rkl;
	*is_dp = 0;
	for (int port = 0; port < 9; port++) {
		if (map[port][0] >= 0 && dvo == (uint8_t)map[port][0])
			return port;
		if (map[port][1] >= 0 && dvo == (uint8_t)map[port][1]) {
			*is_dp = 1;
			return port;
		}
	}
	return -1;
}

/* VBT AUX channel codes */
#define DP_AUX_A 0x40
#define DP_AUX_B 0x10
#define DP_AUX_C 0x20
#define DP_AUX_D 0x30
#define DP_AUX_E 0x50
#define DP_AUX_F 0x60
#define DP_AUX_G 0x70
#define DP_AUX_H 0x80
#define DP_AUX_I 0x90

/* The channel index (0 = A, 3.. = USBC1.. from Tiger Lake on) a code
 * names on this platform; 0xff for none. */
static uint8_t aux_ch_to_index(const struct vbt_ctx *ctx, uint8_t aux)
{
	static const uint8_t direct[] = { DP_AUX_A, DP_AUX_B, DP_AUX_C, DP_AUX_D, DP_AUX_E,
					  DP_AUX_F, DP_AUX_G, DP_AUX_H, DP_AUX_I };
	/* A, B, C, USBC1-4, then D and E (display 13+) */
	static const uint8_t adlp[] = { DP_AUX_A, DP_AUX_B, DP_AUX_C, DP_AUX_F, DP_AUX_G,
					DP_AUX_H, DP_AUX_I, DP_AUX_D, DP_AUX_E };
	/* A, then USBC1-4 */
	static const uint8_t adls[] = { DP_AUX_A, 0, 0, DP_AUX_B, DP_AUX_C, DP_AUX_D, DP_AUX_E };
	/* A, B, then USBC1-2 */
	static const uint8_t rkl[] = { DP_AUX_A, DP_AUX_B, 0, DP_AUX_C, DP_AUX_D };
	const uint8_t *map = direct;
	unsigned n = sizeof(direct);

	if (ctx->display_ver >= 13) {
		map = adlp;
		n = sizeof(adlp);
	} else if (ctx->platform == I915_PLATFORM_ALDERLAKE_S) {
		map = adls;
		n = sizeof(adls);
	} else if (ctx->platform == I915_PLATFORM_DG1 || ctx->platform == I915_PLATFORM_ROCKETLAKE) {
		map = rkl;
		n = sizeof(rkl);
	}
	for (unsigned i = 0; i < n; i++)
		if (map[i] && map[i] == aux)
			return (uint8_t)i;
	return 0xff;
}

/* The TMDS limit a child's HDMI max data rate code (BDB 204+) names, in
 * kHz; 0 for "the platform's" (and for a code this parser does not know). */
#define HDMI_MAX_DATA_RATE_PLATFORM 0
#define HDMI_MAX_DATA_RATE_297 1
#define HDMI_MAX_DATA_RATE_165 2
#define HDMI_MAX_DATA_RATE_594 3 /* 249+ */
#define HDMI_MAX_DATA_RATE_340 4 /* 249+ */
#define HDMI_MAX_DATA_RATE_300 5 /* 249+ */

static uint32_t hdmi_max_tmds_khz(uint8_t code)
{
	switch (code) {
	default:
	case HDMI_MAX_DATA_RATE_PLATFORM:
		return 0;
	case HDMI_MAX_DATA_RATE_594:
		return 594000;
	case HDMI_MAX_DATA_RATE_340:
		return 340000;
	case HDMI_MAX_DATA_RATE_300:
		return 300000;
	case HDMI_MAX_DATA_RATE_297:
		return 297000;
	case HDMI_MAX_DATA_RATE_165:
		return 165000;
	}
}

/* A child's DP max link rate code: from BDB 230 zero means no limit and
 * the codes count up from RBR; from 216 to 229 they count down from
 * HBR3, which zero names. */
#define BDB_216_VBT_DP_MAX_LINK_RATE_HBR3 0
#define BDB_216_VBT_DP_MAX_LINK_RATE_HBR2 1
#define BDB_216_VBT_DP_MAX_LINK_RATE_HBR 2
#define BDB_216_VBT_DP_MAX_LINK_RATE_LBR 3

#define BDB_230_VBT_DP_MAX_LINK_RATE_DEF 0
#define BDB_230_VBT_DP_MAX_LINK_RATE_LBR 1
#define BDB_230_VBT_DP_MAX_LINK_RATE_HBR 2
#define BDB_230_VBT_DP_MAX_LINK_RATE_HBR2 3
#define BDB_230_VBT_DP_MAX_LINK_RATE_HBR3 4
#define BDB_230_VBT_DP_MAX_LINK_RATE_UHBR10 5
#define BDB_230_VBT_DP_MAX_LINK_RATE_UHBR13P5 6
#define BDB_230_VBT_DP_MAX_LINK_RATE_UHBR20 7

static uint32_t dp_max_link_rate_230(uint8_t code)
{
	switch (code) {
	default:
	case BDB_230_VBT_DP_MAX_LINK_RATE_DEF:
		return 0;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR20:
		return 2000000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR13P5:
		return 1350000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR10:
		return 1000000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR3:
		return 810000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR2:
		return 540000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR:
		return 270000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_LBR:
		return 162000;
	}
}

static uint32_t dp_max_link_rate_216(uint8_t code)
{
	switch (code) {
	default:
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR3:
		return 810000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR2:
		return 540000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR:
		return 270000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_LBR:
		return 162000;
	}
}

/* One child device, laid out per BDB version. */
static void parse_child(const struct vbt_ctx *ctx, const uint8_t *c, unsigned size,
			uint16_t version, struct intel_vbt *out)
{
	uint16_t handle = rd16(c + 0);
	uint16_t device_type = rd16(c + 2);
	if (!handle || !device_type)
		return;
	/* The fields, by byte: 4..13 hold either an identifying string or,
	 * from version 158, the re-driver and level-shifter settings --
	 * byte 7 carries the HDMI level shifter in its low five bits.
	 * 14 addin_offset; 16 dvo_port; 17 i2c_pin; 18 slave_addr;
	 * 19 ddc_pin; 20 edid_ptr; 22 dvo_cfg.  Bytes 23..26 are a second
	 * port's wiring on old tables and, from version 158, flags (23:
	 * bit 1 lane reversal, from 184), the supported signalling (24:
	 * bits 0/1/2 HDMI, DisplayPort, TMDS), the AUX channel (25) and
	 * dongle detection (26).  Byte 7's top three bits are the HDMI max
	 * data rate (204+); byte 10 carries the compression flags (198+:
	 * bit 1 enable, bit 2 the CPS method) and byte 11 the compression
	 * parameters entry in its low nibble; byte 23's top two bits the DP
	 * lane limit (244+) and byte 38's low three the DP link rate limit
	 * (216+). */
	uint8_t dvo_port = c[16];
	uint8_t ddc_pin = size > 19 ? c[19] : 0;
	uint8_t aux_ch = (version >= 158 && size > 25) ? c[25] : 0;
	uint8_t hdmi_level = (version >= 158 && size > 7) ? (uint8_t)(c[7] & 0x1f) : 0xff;
	uint8_t flags1 = (version >= 158 && size > 23) ? c[23] : 0;
	uint8_t signalling = (version >= 158 && size > 24) ? c[24] : 0;
	int is_dp;
	int ddi = dvo_port_to_ddi(ctx, dvo_port, &is_dp);
	if (ddi < 0 || ddi >= INTEL_MAX_PORTS)
		return;
	struct intel_vbt_port *p = &out->port[ddi];
	p->present = 1;
	int internal = !!(device_type & DEVICE_TYPE_INTERNAL_CONNECTOR);
	int dp = !!(device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT);
	int tmds = !!(device_type & DEVICE_TYPE_TMDS_DVI_SIGNALING);
	int not_hdmi = !!(device_type & DEVICE_TYPE_NOT_HDMI_OUTPUT);
	if (dp || is_dp) {
		if (internal) {
			p->supports_edp = 1;
			p->is_internal = 1;
		} else {
			p->supports_dp = 1;
		}
	}
	if (tmds && !internal) {
		if (not_hdmi)
			p->supports_dvi = 1;
		else
			p->supports_hdmi = 1;
	}
	/* From version 158 the child also states its signalling directly. */
	if (signalling) {
		if ((signalling & 0x02) && !internal)
			p->supports_dp = 1;
		if (signalling & 0x01)
			p->supports_hdmi = 1;
		else if (signalling & 0x04)
			p->supports_dvi = 1;
	}
	/* DP++: DisplayPort and HDMI both, named as a DP port, or as an
	 * HDMI port that has an AUX channel. */
	if (dp && tmds && !not_hdmi && (is_dp || aux_ch))
		p->dp_dual_mode = 1;
	if (version >= 204 && size > 7) {
		uint32_t khz = hdmi_max_tmds_khz((uint8_t)(c[7] >> 5));
		if (khz)
			p->hdmi_max_tmds_khz = khz;
	}
	if (version >= 216 && size > 38)
		p->dp_max_link_rate_khz = version >= 230 ? dp_max_link_rate_230(c[38] & 7) :
							     dp_max_link_rate_216(c[38] & 7);
	if (version >= 244)
		p->dp_max_lanes = (uint8_t)(((flags1 >> 6) & 3) + 1);
	if (version >= 198 && size > 11 && (c[10] & 0x02)) {
		p->compression_enable = 1;
		p->dsc_cps = (c[10] >> 2) & 1;
		p->dsc_index = c[11] & 0x0f;
	}
	if (version >= 184)
		p->lane_reversal = (flags1 >> 1) & 1;
	if (version >= 196)
		p->hpd_invert = (flags1 >> 4) & 1;
	/* byte 33: bit 0 the port is USB-C (195+), bit 1 Thunderbolt (209+),
	 * bit 2 the PHY is a dedicated external one, outside the Type-C
	 * subsystem (264+), for which the other two mean nothing */
	if (version >= 195 && size > 33) {
		p->dp_usb_type_c = c[33] & 1;
		p->tbt = version >= 209 ? (c[33] >> 1) & 1 : 0;
		p->typec_valid = 1;
		p->dedicated_external = version >= 264 ? (c[33] >> 2) & 1 : 0;
		if (p->dedicated_external) {
			p->dp_usb_type_c = 0;
			p->tbt = 0;
		}
	}
	if (ddc_pin)
		p->ddc_pin = ddc_pin;
	if (aux_ch)
		p->aux_ch = aux_ch_to_index(ctx, aux_ch);
	else if (!p->aux_ch && (p->supports_dp || p->supports_edp))
		p->aux_ch = (uint8_t)ddi; /* the port's own channel */
	p->hdmi_level_shift = hdmi_level;
	out->nchildren++;
}

static void parse_general_definitions(const struct vbt_ctx *ctx, const uint8_t *b,
				      unsigned len, uint16_t version, struct intel_vbt *out)
{
	/* byte 0: crt_ddc_gmbus_pin; 1: dpms bits; 2: boot display[2];
	 * 4: child_dev_size; 5..: children */
	if (len < 5)
		return;
	unsigned csize = b[4];
	if (csize < 22)
		return;
	for (unsigned off = 5; off + csize <= len; off += csize)
		parse_child(ctx, b + off, csize, version, out);
	out->int_crt_support = 0;
}

static void parse_edp(const uint8_t *b, unsigned len, struct intel_vbt *out)
{
	int pt = out->panel_type;
	if (pt < 0 || pt > 15)
		return;
	/* 16 x power_seq (5 x u16 = 10 bytes) at 0; then color_depth u32;
	 * 16 x fast_link_params (u8 rate, u8 lanes, u8 preemph, u8 vswing);
	 * sdrrs_msa_timing_delay u32; ... low_vswing u16 bitfield after
	 * more (version dependent). */
	if (len >= 160) {
		const uint8_t *seq = b + pt * 10;
		out->pps.t1_t3 = rd16(seq + 0);
		out->pps.t8 = rd16(seq + 2);
		out->pps.t9 = rd16(seq + 4);
		out->pps.t10 = rd16(seq + 6);
		out->pps.t11_t12 = rd16(seq + 8);
		out->pps.valid = 1;
	}
	/* Then the colour depth (two bits per panel), and the link each
	 * panel trains at: ONE BYTE holding the rate in its low nibble and
	 * the lane count in its high one, then one byte of pre-emphasis
	 * and voltage swing.  Two bytes per panel, not four -- reading
	 * them four apart lands on another panel's entry, and a lane
	 * count of one there leaves no link that carries the picture. */
	if (len >= 164 + 32) {
		uint32_t depth = rd32(b + 160);
		int bpc_code = (depth >> (pt * 2)) & 3;
		static const int bpcs[4] = { 6, 8, 10, 12 };
		if (!out->panel_bpc)
			out->panel_bpc = bpcs[bpc_code];
		const uint8_t *fl = b + 164 + pt * 2;
		out->edp_rate = (uint8_t)(fl[0] & 0x0f);
		uint8_t lanes = (uint8_t)((fl[0] >> 4) & 0x0f);
		out->edp_lanes = lanes == 0 ? 1 : (lanes == 1 ? 2 : 4);
		out->edp_vswing_preemph = (uint8_t)((fl[1] & 0x0f) | ((fl[1] & 0xf0) >> 4) << 4);
	}
	/* After the link parameters: a delay word at 196, a stereo-3d word
	 * at 200, a timing-optimisation word at 202, then four bits of
	 * voltage swing and pre-emphasis per panel from 204.  A swing
	 * setting of zero is what "low voltage swing" means. */
	if (out->version >= 173 && len >= 212) {
		uint32_t lo = rd32(b + 204);
		uint32_t hi = rd32(b + 208);
		uint32_t nib = pt < 8 ? (lo >> (pt * 4)) : (hi >> ((pt - 8) * 4));
		out->edp_low_vswing = (nib & 0xf) == 0;
	}
	/* Further on, past the PWM delays (16 x 4 bytes from 216), the full
	 * link parameters (a word at 280, 16 bytes), the Apical block (a
	 * word at 298, 16 x 28 bytes), the fast training rates (16 words
	 * from 748) and the maximum port link rates (16 words from 780): a
	 * word at 812 with a bit per panel that forbids stream compression
	 * on it. */
	if (out->version >= 251 && len >= 814)
		out->edp_dsc_disable = (uint8_t)((rd16(b + 812) >> pt) & 1);
}

static void parse_lvds_options(const uint8_t *b, unsigned len,
			       struct intel_vbt *out)
{
	if (len < 2)
		return;
	/* byte 0: panel_type; byte 1: bits: pfit, dither...  A type of 0xff
	 * asks for the panel to be matched by its EDID's PnP id, which
	 * cannot be done here: the first entry is the fallback then, as
	 * it is for an out-of-range type. */
	out->panel_type = b[0] <= 0x0f ? b[0] : 0;
	out->lvds_dither = !!(b[1] & 0x01);
}

/* The LFP data pointers block gives, per panel type, where the DTD of
 * that panel sits in the LFP data block. */
static void parse_lfp_data(const uint8_t *bdb, const uint8_t *ptrs, unsigned ptrs_len,
			   const uint8_t *data, unsigned data_len,
			   struct intel_vbt *out)
{
	int pt = out->panel_type;
	const uint8_t *d = 0;
	if (pt < 0 || pt > 15 || !data)
		return;
	if (ptrs) {
		/* ptrs: byte 0 = lvds_entries; then 16 x (3 x (u16 offset,
		 * u8 size)) for fp_timing, dvo_timing, panel_pnp_id.  The
		 * offsets count from the start of the BIOS data block; some
		 * tables (and this driver's tests) count them from the LFP
		 * data block's header instead, which is tried when the first
		 * reading falls outside the block. */
		if (ptrs_len < 1 + 16 * 9)
			return;
		const uint8_t *e = ptrs + 1 + pt * 9;
		uint32_t dvo_off = rd16(e + 3);
		uint8_t dvo_size = e[5];
		uint32_t data_off = (uint32_t)(data - bdb);
		if (dvo_size < 18)
			return;
		if (dvo_off >= data_off && dvo_off - data_off + 18 <= data_len)
			d = data + (dvo_off - data_off);
		else if (dvo_off >= 3 && dvo_off - 3 + 18 <= data_len)
			d = data + dvo_off - 3;
		else
			return;
	} else {
		/* Modern tables have no pointer block: the entries are 38
		 * bytes of panel timing, the 18-byte DTD and the 10-byte PnP
		 * id, sixteen of them in a row. */
		if (out->version < 155 || 16 * (38 + 18 + 10) > data_len)
			return;
		d = data + (unsigned)pt * (38 + 18 + 10) + 38;
	}
	/* the same 18-byte detailed timing layout as an EDID descriptor */
	uint32_t clock = (uint32_t)rd16(d) * 10;
	uint32_t hactive = d[2] | ((d[4] & 0xf0) << 4);
	uint32_t hblank = d[3] | ((d[4] & 0x0f) << 8);
	uint32_t vactive = d[5] | ((d[7] & 0xf0) << 4);
	uint32_t vblank = d[6] | ((d[7] & 0x0f) << 8);
	uint32_t hsync_off = d[8] | ((d[11] & 0xc0) << 2);
	uint32_t hsync_w = d[9] | ((d[11] & 0x30) << 4);
	uint32_t vsync_off = (d[10] >> 4) | ((d[11] & 0x0c) << 2);
	uint32_t vsync_w = (d[10] & 0x0f) | ((d[11] & 0x03) << 4);
	if (!clock || !hactive || !vactive)
		return;
	struct drm_mode_modeinfo *m = &out->panel_mode;
	for (unsigned i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	m->clock = clock;
	m->hdisplay = (uint16_t)hactive;
	m->hsync_start = (uint16_t)(hactive + hsync_off);
	m->hsync_end = (uint16_t)(hactive + hsync_off + hsync_w);
	m->htotal = (uint16_t)(hactive + hblank);
	m->vdisplay = (uint16_t)vactive;
	m->vsync_start = (uint16_t)(vactive + vsync_off);
	m->vsync_end = (uint16_t)(vactive + vsync_off + vsync_w);
	m->vtotal = (uint16_t)(vactive + vblank);
	m->flags = (d[17] & 0x02) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	m->flags |= (d[17] & 0x04) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_finish(m);
	out->panel_mode_valid = 1;
}

static void parse_backlight(const uint8_t *b, unsigned len,
			    struct intel_vbt *out)
{
	int pt = out->panel_type;
	if (pt < 0 || pt > 15)
		return;
	/* byte 0: entry_size; then 16 entries: u8 type/active_low bits,
	 * u16 pwm_freq_hz, u8 min_brightness, u8 obsolete, u8 obsolete2 */
	unsigned esz = b[0];
	if (esz < 6 || len < 1 + 16 * esz)
		return;
	const uint8_t *e = b + 1 + pt * esz;
	out->backlight_valid = 1;
	/* The first byte packs two fields: the controller type in bits 0-1
	 * and the "PWM is active low" flag in BIT 2.
	 *
	 * Bit 1 is not the flag, and reading it there is how this driver
	 * inverted every panel it ever drove: the type of a panel with a
	 * backlight is 2 (PWM), whose bit pattern is 0b10 -- so bit 1 is set
	 * on every such panel, the polarity bit went into BLC_PWM_PCH_CTL1,
	 * the transmitter inverted the duty cycle, and the brightness
	 * control ran backwards.  A duty of zero was the brightest the panel
	 * would go and the maximum was dark. */
	out->backlight_active_low = (e[0] >> 2) & 1;
	out->backlight_pwm_hz = rd16(e + 1);
	out->backlight_min = e[3];
	/* controller: after the 16 entries, 16 x u8 level (v191), then
	 * 16 x u8 backlight_control (u8 type:4, controller:4) (v191+) */
	unsigned ctl_off = 1 + 16 * esz + 16;
	if (len >= ctl_off + 16)
		out->backlight_controller = (b[ctl_off + pt] >> 4) & 0xf;
}

/* The LFP power block: from BDB 233 a bit per panel type saying whether
 * the panel may run variable refresh, a u16 at byte 56 (after the
 * feature byte, five ambient light entries, a profile byte, eight u16
 * feature masks and sixteen aggressiveness bytes).  A block too short to
 * hold it says no. */
static void parse_lfp_power(const uint8_t *b, unsigned len, struct intel_vbt *out)
{
	int pt = out->panel_type;
	if (out->version < 233 || pt < 0 || pt > 15)
		return;
	uint16_t vrr = len >= 58 ? rd16(b + 56) : 0;
	out->lfp_vrr = (vrr >> pt) & 1;
}

/* The compression parameters block: a u16 entry size, then sixteen
 * entries of 13 bytes -- version (major in the low nibble), the rate
 * buffer's block size (two bits) and size, a u32 of allowed slice counts,
 * the line buffer depth (four bits), the block prediction flag (bit 0),
 * the maximum bits per pixel code, the supported component depths (bits
 * 1, 2, 3: 8, 10, 12 bpc) and a u16 slice height.  Each child that asks
 * for compression (and not by the CPS method) gets a copy of its entry. */
#define VBT_DSC_ENTRY_SIZE 13

static void parse_compression_parameters(const uint8_t *b, unsigned len,
					 struct intel_vbt *out)
{
	if (out->version < 198 || !b)
		return;
	if (len < 2 || rd16(b) != VBT_DSC_ENTRY_SIZE)
		return;
	if (len < 2 + 16 * VBT_DSC_ENTRY_SIZE)
		return;
	for (int i = 0; i < INTEL_MAX_PORTS; i++) {
		struct intel_vbt_port *p = &out->port[i];
		if (!p->present || !p->compression_enable || p->dsc_cps)
			continue;
		const uint8_t *e = b + 2 + (unsigned)p->dsc_index * VBT_DSC_ENTRY_SIZE;
		struct intel_vbt_dsc *d = &p->dsc;
		d->version_major = e[0] & 0x0f;
		d->version_minor = (uint8_t)(e[0] >> 4);
		d->rc_buffer_block_size = e[1] & 0x03;
		d->rc_buffer_size = e[2];
		d->slices_per_line = rd32(e + 3);
		d->line_buffer_depth = e[7] & 0x0f;
		d->block_prediction_enable = e[8] & 0x01;
		d->max_bpp = e[9];
		d->support_8bpc = (e[10] >> 1) & 1;
		d->support_10bpc = (e[10] >> 2) & 1;
		d->support_12bpc = (e[10] >> 3) & 1;
		d->slice_height = rd16(e + 11);
		p->dsc_valid = 1;
	}
}

int intel_vbt_parse(const uint8_t *vbt, unsigned len, struct intel_vbt *out)
{
	return intel_vbt_parse_platform(vbt, len, 0, 0, out);
}

int intel_vbt_parse_platform(const uint8_t *vbt, unsigned len, int display_ver,
			     int platform, struct intel_vbt *out)
{
	struct vbt_ctx ctx = { display_ver, platform };
	for (unsigned i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	out->panel_type = -1;
	out->lfp_vrr = 1;
	for (int i = 0; i < INTEL_MAX_PORTS; i++) {
		out->port[i].aux_ch = 0xff;
		out->port[i].hdmi_level_shift = 0xff;
	}
	if (!vbt || len < 48)
		return -EINVAL;
	if (vbt[0] != '$' || vbt[1] != 'V' || vbt[2] != 'B' || vbt[3] != 'T')
		return -EINVAL;
	for (int i = 0; i < 20; i++)
		out->signature[i] = (char)vbt[i];
	out->signature[20] = 0;
	/* header: 20 bytes of signature, u16 version, u16 header_size,
	 * u16 vbt_size, u8 checksum, u8 reserved, then at byte 28 the u32
	 * offset of the data blocks.  Reading it four bytes early picks up
	 * the table's own size instead, which sends the whole table away
	 * as malformed and leaves the board's wiring unknown. */
	uint32_t bdb_off = rd32(vbt + 28);
	uint16_t header_size = rd16(vbt + 22);
	if (bdb_off < header_size || bdb_off < 32)
		return -EINVAL;
	if (bdb_off + 24 > len)
		return -EINVAL;
	const uint8_t *bdb = vbt + bdb_off;
	/* BDB header: 16 sig "BIOS_DATA_BLOCK ", u16 version, u16 header
	 * size, u16 bdb_size */
	out->version = rd16(bdb + 16);
	uint16_t hsize = rd16(bdb + 18);
	uint16_t bsize = rd16(bdb + 20);
	if (hsize < 22 || bdb_off + bsize > len)
		bsize = (uint16_t)(len - bdb_off);
	const uint8_t *ptrs_blk = 0;
	unsigned ptrs_len = 0;
	const uint8_t *data_blk = 0;
	unsigned data_len = 0;
	const uint8_t *edp_blk = 0;
	unsigned edp_len = 0;
	const uint8_t *bl_blk = 0;
	unsigned bl_len = 0;
	const uint8_t *power_blk = 0;
	unsigned power_len = 0;
	const uint8_t *dsc_blk = 0;
	unsigned dsc_len = 0;
	/* first pass: everything that does not depend on the panel type */
	for (unsigned off = hsize; off + 3 <= bsize;) {
		uint8_t id = bdb[off];
		uint16_t blen = rd16(bdb + off + 1);
		const uint8_t *body = bdb + off + 3;
		if (off + 3 + blen > bsize)
			break;
		switch (id) {
		case BDB_GENERAL_DEFINITIONS:
			parse_general_definitions(&ctx, body, blen, out->version, out);
			break;
		case BDB_LVDS_OPTIONS:
			parse_lvds_options(body, blen, out);
			break;
		case BDB_LVDS_LFP_DATA_PTRS:
			ptrs_blk = body;
			ptrs_len = blen;
			break;
		case BDB_LVDS_LFP_DATA:
			data_blk = body;
			data_len = blen;
			break;
		case BDB_EDP:
			edp_blk = body;
			edp_len = blen;
			break;
		case BDB_LFP_BACKLIGHT:
			bl_blk = body;
			bl_len = blen;
			break;
		case BDB_LFP_POWER:
			power_blk = body;
			power_len = blen;
			break;
		case BDB_COMPRESSION_PARAMETERS:
			dsc_blk = body;
			dsc_len = blen;
			break;
		default:
			break;
		}
		off += 3 + blen;
	}
	if (out->panel_type < 0)
		out->panel_type = 0;
	if (data_blk)
		parse_lfp_data(bdb, ptrs_blk, ptrs_len, data_blk, data_len, out);
	if (edp_blk)
		parse_edp(edp_blk, edp_len, out);
	if (bl_blk)
		parse_backlight(bl_blk, bl_len, out);
	if (power_blk)
		parse_lfp_power(power_blk, power_len, out);
	parse_compression_parameters(dsc_blk, dsc_len, out);
	out->valid = 1;
	return 0;
}
