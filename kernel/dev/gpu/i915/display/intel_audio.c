// LikeOS -- display audio: the graphics half of HDMI and DisplayPort sound.
//
// The HD Audio display codec (hda_hdmi.c on the sound side) sends its
// samples into the display engine, which packs them into the HDMI data
// islands or DisplayPort secondary data packets of a transcoder.  Per
// transcoder the display has an audio configuration register (the HDMI
// pixel clock class and N value, or the DisplayPort Maud/Naud mode), an
// 84-byte hardware ELD buffer the codec reads through its ELD verbs, and
// two bits in the shared pin register: "output enable" (the codec sees
// presence on the pin of the port the transcoder drives) and "ELD valid".
//
// The sequences, from the hardware documentation:
//
//   enable  (transcoder, port and link up):  early-hblank and DSC audio
//           settings (DisplayPort, display version 11+), the SDP split
//           for 128b/132b links, presence on, a frame, ELD invalid, the
//           ELD written, ELD valid, the clock settings;
//   disable (before the transcoder goes off): the clock settings back to
//           their defaults, ELD invalid, two frames, presence off.
//
// Around that: the audio registers and the codec sit in a power well
// (power well 2 on Skylake, the pipe wells after it on later parts), the
// display core clock must be at least twice the 96 MHz audio bit clock on
// display version 9 and later, the audio frequency and timestamp clock
// registers lose their settings with the power, and on Skylake and later
// the codec only answers the controller's reset when the display drives
// its wake signal.
//
// The sound core reaches this through snd_display_audio_register(): it
// asks for the ELD of a port (the ELD built here from the sink's EDID),
// for a sample rate (the HDMI N value depends on it for a few pixel
// clocks), and for the power; it is told about every change through
// snd_display_audio_notify().
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_audio.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_eld.h>
#include <kernel/dev/sound/snd.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The HD Audio controller driver (kernel/dev/sound/hda_controller.c):
 * the CDCLK its Haswell/Broadwell display controller's BCLK divider is
 * set from.  Safe at any time, also before the controller is probed. */
void hda_controller_set_cdclk(int cdclk_khz);

/* ---- registers ------------------------------------------------------------------- */

/* per transcoder (A..D), 0x100 apart */
#define IA_AUD_CFG(t) (0x65000u + (uint32_t)(t) * 0x100u)
#define IA_AUD_MISC_CTRL(t) (0x65010u + (uint32_t)(t) * 0x100u)
#define IA_AUD_M_CTS_ENABLE(t) (0x65028u + (uint32_t)(t) * 0x100u)
#define IA_AUD_EDID_DATA(t) (0x65050u + (uint32_t)(t) * 0x100u)
#define IA_AUD_DIP_ELD_CTRL(t) (0x650b4u + (uint32_t)(t) * 0x100u)
#define IA_AUD_DP_2DOT0_CTRL(t) (0x650bcu + (uint32_t)(t) * 0x100u)
/* shared */
#define IA_AUD_PIPE_CONV_CFG 0x6507cu
#define IA_AUD_PIN_ELD_CP_VLD 0x650c0u
#define IA_AUD_FREQ_CNTRL 0x65900u
#define IA_AUD_TS_CDCLK_M 0x65ea0u
#define IA_AUD_TS_CDCLK_N 0x65ea4u
#define IA_AUD_CONFIG_BE 0x65ef0u
#define IA_AUD_CHICKENBIT 0x65f10u
#define IA_AUD_CHICKENBIT_REG3 0x65f1cu
#define IA_AUD_PIN_BUF_CTL 0x48414u

/* AUD_CFG */
#define IA_CFG_N_VALUE_INDEX (1u << 29) /* DP: hardware Maud/Naud; HDMI: N from table */
#define IA_CFG_N_PROG_ENABLE (1u << 28)
#define IA_CFG_UPPER_N_MASK (0xffu << 20)
#define IA_CFG_PIXEL_CLOCK_HDMI_MASK (0xfu << 16)
#define IA_CFG_LOWER_N_MASK (0xfffu << 4)
#define IA_CFG_N_MASK (IA_CFG_UPPER_N_MASK | IA_CFG_LOWER_N_MASK)
#define IA_CFG_DISABLE_NCTS (1u << 3)
#define IA_CFG_N(n) ((((uint32_t)(n) >> 12) & 0xffu) << 20 | ((uint32_t)(n) & 0xfffu) << 4)
/* AUD_M_CTS_ENABLE */
#define IA_M_CTS_M_VALUE_INDEX (1u << 21)
#define IA_M_CTS_M_PROG_ENABLE (1u << 20)
#define IA_M_CTS_M_MASK 0xfffffu
/* AUD_DIP_ELD_CTRL */
#define IA_ELD_BUFFER_SIZE_MASK (0x1fu << 10) /* dwords */
#define IA_ELD_BUFFER_SIZE_SHIFT 10
#define IA_ELD_ADDRESS_MASK (0x1fu << 5)
#define IA_ELD_ACK (1u << 4)
/* AUD_PIN_ELD_CP_VLD, four bits per transcoder */
#define IA_AUDIO_INACTIVE(t) ((1u << 3) << ((t) * 4))
#define IA_AUDIO_OUTPUT_ENABLE(t) ((1u << 2) << ((t) * 4))
#define IA_AUDIO_CP_READY(t) ((1u << 1) << ((t) * 4))
#define IA_AUDIO_ELD_VALID(t) ((1u << 0) << ((t) * 4))
/* AUD_DP_2DOT0_CTRL */
#define IA_AUD_ENABLE_SDP_SPLIT (1u << 31)
/* AUD_CHICKENBIT */
#define IA_SKL_AUD_CODEC_WAKE_SIGNAL (1u << 15)
/* AUD_CHICKENBIT_REG3 */
#define IA_DACBE_DISABLE_MIN_HBLANK_FIX (1u << 18)
/* AUD_PIN_BUF_CTL */
#define IA_AUD_PIN_BUF_ENABLE (1u << 31)
/* AUD_TS_CDCLK_M */
#define IA_AUD_TS_CDCLK_M_EN (1u << 31)
/* AUD_FREQ_CNTRL: 8T mode, pull clocks, 96 MHz bit clock */
#define IA_FREQ_TMODE_SHIFT 14
#define IA_FREQ_8T (2u << IA_FREQ_TMODE_SHIFT)
#define IA_FREQ_PULLCLKS(x) (((uint32_t)(x) & 3u) << 11)
#define IA_FREQ_BCLK_96M (1u << 4)
#define IA_FREQ_GEN12 (IA_FREQ_8T | IA_FREQ_PULLCLKS(0) | IA_FREQ_BCLK_96M)
#define IA_FREQ_TGL_BROKEN (IA_FREQ_8T | IA_FREQ_PULLCLKS(2) | IA_FREQ_BCLK_96M)
/* AUD_CONFIG_BE: per transcoder hblank early enable, start count and
 * samples per line */
#define IA_HBLANK_EARLY_ENABLE_ICL(t) (1u << (20 - (t)))
#define IA_HBLANK_EARLY_ENABLE_TGL(t) (1u << (24 + (t)))
#define IA_HBLANK_START_COUNT_MASK(t) (7u << (3 + (t) * 6))
#define IA_HBLANK_START_COUNT(t, v) (((uint32_t)(v) & 7u) << (3 + (t) * 6))
#define IA_SAMPLES_PER_LINE_MASK(t) (3u << ((t) * 6))
#define IA_SAMPLES_PER_LINE(t, v) (((uint32_t)(v) & 3u) << ((t) * 6))
#define IA_HBLANK_START_COUNT_32 2
#define IA_HBLANK_START_COUNT_64 3
#define IA_HBLANK_START_COUNT_96 4
#define IA_HBLANK_START_COUNT_128 5

/* The transcoders that carry audio (TRANSCODER_EDP never does). */
#define IA_MAX_TRANS 4
/* The hardware ELD buffer, when the register does not say. */
#define IA_HW_ELD_DWORDS 21

/* ---- state ------------------------------------------------------------------------ */

struct ia_port {
	int active;
	int transcoder;
	int pipe;
	int is_dp;
	uint32_t clock_khz; /* pixel clock */
	uint32_t tmds_khz; /* HDMI: the TMDS clock (deep colour raises it) */
	uint32_t link_khz; /* DP: link rate */
	int lanes;
	int rate; /* sample rate the codec asked for, 0: automatic */
	int eld_len;
	uint8_t eld[DRM_ELD_BUF_SIZE];
	char monitor[20];
};

static struct {
	struct i915_device *i915;
	int ready;
	int registered;
	int dom; /* the power domain the audio well is reached through */
	int well_held; /* our own reference: the codec stays reachable */
	int power_refs; /* the sound side's get_power - put_power */
	int wake_forced; /* the codec wake signal is overridden on */
	uint32_t freq_cntrl;
	int freq_valid;
	int cdclk_warned;
	struct ia_port port[INTEL_MAX_PORTS];
} g_ia;

/* ---- helpers --------------------------------------------------------------------- */

static int ia_plat(struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

static int ia_ver(struct i915_device *i915)
{
	return i915->info->display_ver;
}

static uint32_t ia_rd(struct i915_device *i915, uint32_t reg)
{
	return i915_read32(i915, reg);
}

static void ia_wr(struct i915_device *i915, uint32_t reg, uint32_t v)
{
	i915_write32(i915, reg, v);
}

static void ia_rmw(struct i915_device *i915, uint32_t reg, uint32_t clear, uint32_t set)
{
	uint32_t v = ia_rd(i915, reg);
	uint32_t n = (v & ~clear) | set;
	if (n != v)
		ia_wr(i915, reg, n);
}

/* The power domain whose wells hold the audio registers and the codec:
 * the second pipe's on every model (Broadwell's one well; Skylake and
 * Broxton power well 2; Ice Lake to Alder Lake the chain up to power
 * well 3 or the pipe-B well behind power well 2; Meteor Lake PW2 and
 * PW_B), except display version 30 and later, whose pipe-B well hangs
 * off power well 1 -- the third pipe's domain reaches power well 2. */
static enum intel_power_domain ia_domain(struct i915_device *i915)
{
	if (i915->display.model == INTEL_DISPLAY_MTL && ia_ver(i915) >= 30)
		return INTEL_PW_PIPE_C;
	return INTEL_PW_PIPE_B;
}

/* A frame of pipe `p' (at least 5 ms, at most 50 ms): what the hardware
 * wants between the presence and ELD steps; there is no vblank
 * interrupt to wait for on a pipe being set up. */
static void ia_wait_frames(const struct intel_pipe *p, int frames)
{
	uint64_t us = 20000;

	if (p && p->mode.clock && p->mode.htotal && p->mode.vtotal)
		us = (uint64_t)p->mode.htotal * p->mode.vtotal * 1000ull / p->mode.clock;
	if (us < 5000)
		us = 5000;
	if (us > 50000)
		us = 50000;
	for (int f = 0; f < frames; f++)
		lapic_delay_us((uint32_t)us);
}

static struct drm_connector *ia_connector(struct i915_device *i915, const struct intel_output *o)
{
	if (!o || o->conn < 0 || (uint32_t)o->conn >= i915->drm.nconn)
		return NULL;
	return &i915->drm.conn[o->conn];
}

/* ---- the ELD --------------------------------------------------------------------- */

/* HDMI vendor block latency fields: 0 unknown, 1..251 = (x - 1) * 2 ms,
 * 255 = no audio/video at all. */
static int ia_latency_ms(int raw)
{
	if (raw <= 0 || raw > 251)
		return -1;
	return (raw - 1) * 2;
}

/* How much later than the picture the sound must be (2 ms units, the
 * ELD's AUD_SYNCH_DELAY): the video latency minus the audio latency. */
static uint8_t ia_sync_delay(int vraw, int araw)
{
	int v = ia_latency_ms(vraw), a = ia_latency_ms(araw);
	int d;

	if (v < 0 || a < 0)
		return 0;
	d = v - a;
	if (d <= 0)
		return 0;
	if (d > 500)
		d = 500;
	return (uint8_t)(d / 2);
}

/* An ELD straight from the sink's EDID, for an output whose connector the
 * core has not parsed (or parsed without one).  Fills `eld' (128 bytes)
 * and returns 1 when the sink takes audio at all. */
static int ia_eld_from_edid(const uint8_t *edid, int len, int is_dp, uint8_t *eld,
			    int *vlat, int *alat)
{
	static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	int mnl = 0, nsad = 0, basic = 0, cea_rev = 0, next;

	mm_memset(eld, 0, DRM_ELD_BUF_SIZE);
	*vlat = 0;
	*alat = 0;
	if (!edid || len < 128 || kmemcmp(edid, hdr, sizeof(hdr)) != 0)
		return 0;
	eld[DRM_ELD_MANUFACTURER_NAME0] = edid[8];
	eld[DRM_ELD_MANUFACTURER_NAME1] = edid[9];
	eld[DRM_ELD_PRODUCT_CODE0] = edid[10];
	eld[DRM_ELD_PRODUCT_CODE1] = edid[11];

	/* the monitor name descriptor (tag 0xfc) */
	for (int d = 0; d < 4; d++) {
		const uint8_t *x = edid + 54 + 18 * d;
		if (x[0] || x[1] || x[2] || x[3] != 0xfc)
			continue;
		for (int i = 0; i < 13; i++) {
			uint8_t ch = x[5 + i];
			if (ch == 0x0a || ch == 0)
				break;
			eld[DRM_ELD_MONITOR_NAME_STRING + mnl++] = (ch < 0x20 || ch > 0x7e) ? '?' : ch;
		}
		while (mnl > 0 && eld[DRM_ELD_MONITOR_NAME_STRING + mnl - 1] == ' ')
			eld[DRM_ELD_MONITOR_NAME_STRING + --mnl] = 0;
		break;
	}

	/* the CTA extensions: audio and speaker blocks, the HDMI block */
	next = DRM_ELD_MONITOR_NAME_STRING + mnl;
	for (int b = 1; b <= edid[126] && (b + 1) * 128 <= len; b++) {
		const uint8_t *x = edid + 128 * b;
		int dtd;
		if (x[0] != 0x02)
			continue;
		if (x[1] > cea_rev)
			cea_rev = x[1];
		if (x[1] >= 2 && (x[3] & 0x40))
			basic = 1;
		dtd = x[2];
		if (dtd < 4 || dtd > 127)
			dtd = dtd == 0 ? 4 : 127;
		for (int i = 4; i < dtd;) {
			int tag = x[i] >> 5, l = x[i] & 0x1f;
			if (i + 1 + l > dtd)
				break;
			const uint8_t *pl = x + i + 1;
			if (tag == 1) {
				for (int s = 0; s + 3 <= l && nsad < 15; s += 3) {
					if (next + 3 > DRM_ELD_BUF_SIZE)
						break;
					eld[next++] = pl[s];
					eld[next++] = pl[s + 1];
					eld[next++] = pl[s + 2];
					nsad++;
				}
			} else if (tag == 4 && l >= 1) {
				eld[DRM_ELD_SPEAKER] = pl[0] & DRM_ELD_SPEAKER_MASK;
			} else if (tag == 3 && l >= 5 && pl[0] == 0x03 && pl[1] == 0x0c &&
				   pl[2] == 0x00) {
				/* HDMI 1.4 vendor block: "supports AI" and the
				 * progressive latencies */
				if (l >= 6 && (pl[5] & 0x80))
					eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= DRM_ELD_SUPPORTS_AI;
				if (l >= 10 && (pl[7] & 0x80)) {
					*vlat = pl[8];
					*alat = pl[9];
				}
			}
			i += 1 + l;
		}
	}
	/* "basic audio" without a descriptor: 2-channel LPCM at 32, 44.1
	 * and 48 kHz with 16-bit samples is what that promises */
	if (!nsad && basic && next + 3 <= DRM_ELD_BUF_SIZE) {
		eld[next++] = (1 << 3) | 1;
		eld[next++] = 0x07;
		eld[next++] = 0x01;
		nsad = 1;
	}
	if (!nsad)
		return 0;
	eld[DRM_ELD_VER] = DRM_ELD_VER_CEA861D;
	eld[DRM_ELD_CEA_EDID_VER_MNL] =
		(uint8_t)(((cea_rev >= 3 ? 3 : cea_rev) << DRM_ELD_CEA_EDID_VER_SHIFT) | mnl);
	eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= (uint8_t)(nsad << DRM_ELD_SAD_COUNT_SHIFT);
	eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= is_dp ? DRM_ELD_CONN_TYPE_DP : DRM_ELD_CONN_TYPE_HDMI;
	eld[DRM_ELD_BASELINE_ELD_LEN] =
		(uint8_t)((drm_eld_calc_baseline_block_size(eld) + 3) / 4);
	return 1;
}

/* Is the ELD in `eld' one we can hand out (version, lengths)? */
static int ia_eld_sane(const uint8_t *eld)
{
	int ver = (eld[DRM_ELD_VER] & DRM_ELD_VER_MASK) >> DRM_ELD_VER_SHIFT;
	int mnl = drm_eld_mnl(eld);
	int nsad = drm_eld_sad_count(eld);

	if (ver != 2 && ver != 31)
		return 0;
	if (mnl > 16 || !eld[DRM_ELD_BASELINE_ELD_LEN])
		return 0;
	if (DRM_ELD_CEA_SAD(mnl, nsad) > DRM_ELD_BUF_SIZE)
		return 0;
	return drm_eld_size(eld) <= DRM_ELD_BUF_SIZE;
}

/* The ELD of output `o' as it runs on pipe `p': the connector's (from the
 * EDID the core parsed), else built here; with the connection type and
 * the sync delay set for this output.  Returns its size, 0 for none. */
static int ia_build_eld(struct i915_device *i915, const struct intel_output *o, uint8_t *eld)
{
	struct drm_connector *c = ia_connector(i915, o);
	int is_dp = o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP;
	int vlat = 0, alat = 0, have = 0;

	mm_memset(eld, 0, DRM_ELD_BUF_SIZE);
	if (c && c->eld[DRM_ELD_VER] && ia_eld_sane(c->eld) && drm_eld_sad_count(c->eld) > 0) {
		mm_memcpy(eld, c->eld, DRM_ELD_BUF_SIZE);
		if (c->latency_present[0]) {
			vlat = c->video_latency[0];
			alat = c->audio_latency[0];
		}
		have = 1;
	}
	if (!have) {
		if (!ia_eld_from_edid(o->edid, o->edid_len, is_dp, eld, &vlat, &alat))
			return 0;
		if (!ia_eld_sane(eld))
			return 0;
	}
	/* what this output is, whatever the EDID parse assumed */
	eld[DRM_ELD_SAD_COUNT_CONN_TYPE] &= (uint8_t)~DRM_ELD_CONN_TYPE_MASK;
	eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= is_dp ? DRM_ELD_CONN_TYPE_DP : DRM_ELD_CONN_TYPE_HDMI;
	/* the picture's latency is HDMI's to tell */
	eld[DRM_ELD_AUD_SYNCH_DELAY] = is_dp ? 0 : ia_sync_delay(vlat, alat);
	return drm_eld_size(eld);
}

/* ---- audio wanted? ----------------------------------------------------------------- */

int intel_audio_output_wants_audio(struct i915_device *i915, const struct intel_output *o)
{
	struct drm_connector *c;
	uint8_t eld[DRM_ELD_BUF_SIZE];
	int vlat, alat;

	if (!i915 || !o || !g_ia.ready)
		return 0;
	if (o->is_edp || o->type == INTEL_OUTPUT_EDP)
		return 0;
	if (o->type == INTEL_OUTPUT_HDMI) {
		if (!o->hdmi_sink)
			return 0; /* DVI mode carries no audio */
	} else if (o->type != INTEL_OUTPUT_DP) {
		return 0;
	}
	c = ia_connector(i915, o);
	if (c && (c->display_info.has_audio || (c->eld[DRM_ELD_VER] && drm_eld_sad_count(c->eld))))
		return 1;
	return ia_eld_from_edid(o->edid, o->edid_len, o->type == INTEL_OUTPUT_DP, eld, &vlat,
				&alat);
}

int intel_audio_output_active(struct i915_device *i915, const struct intel_output *o)
{
	(void)i915;
	if (!o || o->port < 0 || o->port >= INTEL_MAX_PORTS || !g_ia.ready)
		return 0;
	return g_ia.port[o->port].active && g_ia.port[o->port].pipe == o->pipe;
}

/* ---- the clocks ------------------------------------------------------------------- */

/* The pixel clock class of AUD_CFG for HDMI.  Below version 12 the
 * classes above 148.5 MHz are not to be used: the default (25.2 MHz)
 * goes in then, and for clocks without a class. */
static uint32_t ia_hdmi_clock_class(struct i915_device *i915, uint32_t khz)
{
	static const uint32_t clocks[14] = {
		25175, 25200, 27000, 27027, 54000, 54054, 74176,
		74250, 148352, 148500, 296703, 297000, 593407, 594000,
	};
	int idx = 1;

	for (int i = 0; i < 14; i++)
		if (clocks[i] == khz)
			idx = i;
	if (ia_ver(i915) < 12 && khz > 148500)
		idx = 1;
	return (uint32_t)idx << 16;
}

/* The HDMI N for the clocks where the hardware's own choice is off (the
 * 1.001 clocks of 4K, and the deep colour TMDS clocks): from the HDMI
 * specification's recommended N tables.  0 = let the hardware choose. */
static uint32_t ia_hdmi_n(uint32_t tmds_khz, int rate)
{
	static const uint32_t tmds[8] = {
		296703, 297000, 593407, 594000, /* 8 bit per component */
		370878, 371250, /* 10 bit */
		445054, 445500, /* 12 bit */
	};
	/* rows: 32k 44.1k 48k 88.2k 96k 176.4k 192k */
	static const uint32_t n[7][8] = {
		{ 5824, 3072, 5824, 3072, 5824, 6144, 5824, 4096 },
		{ 4459, 4704, 8918, 9408, 8918, 4704, 8918, 4704 },
		{ 5824, 5120, 5824, 6144, 11648, 5120, 5824, 5120 },
		{ 8918, 9408, 17836, 18816, 17836, 9408, 17836, 9408 },
		{ 11648, 10240, 11648, 12288, 23296, 10240, 11648, 10240 },
		{ 17836, 18816, 35672, 37632, 35672, 18816, 35672, 18816 },
		{ 23296, 20480, 23296, 24576, 46592, 20480, 23296, 20480 },
	};
	int row, col = -1;

	switch (rate) {
	case 32000: row = 0; break;
	case 44100: row = 1; break;
	case 48000: row = 2; break;
	case 88200: row = 3; break;
	case 96000: row = 4; break;
	case 176400: row = 5; break;
	case 192000: row = 6; break;
	default: return 0;
	}
	for (int i = 0; i < 8; i++)
		if (tmds[i] == tmds_khz)
			col = i;
	return col < 0 ? 0 : n[row][col];
}

/* AUD_CFG / AUD_M_CTS for what transcoder `st' runs. */
static void ia_config_update(struct i915_device *i915, const struct ia_port *st)
{
	int t = st->transcoder;
	uint32_t cfg = ia_rd(i915, IA_AUD_CFG(t));

	if (st->is_dp) {
		/* the hardware derives Maud/Naud from the link */
		cfg &= ~(IA_CFG_N_VALUE_INDEX | IA_CFG_PIXEL_CLOCK_HDMI_MASK | IA_CFG_N_MASK |
			 IA_CFG_N_PROG_ENABLE);
		cfg |= IA_CFG_N_VALUE_INDEX;
		ia_wr(i915, IA_AUD_CFG(t), cfg);
		return;
	}
	cfg &= ~(IA_CFG_N_VALUE_INDEX | IA_CFG_PIXEL_CLOCK_HDMI_MASK | IA_CFG_N_PROG_ENABLE);
	cfg |= ia_hdmi_clock_class(i915, st->clock_khz);
	{
		uint32_t nv = ia_hdmi_n(st->tmds_khz, st->rate);
		if (nv) {
			cfg &= ~IA_CFG_N_MASK;
			cfg |= IA_CFG_N(nv) | IA_CFG_N_PROG_ENABLE;
		}
	}
	ia_wr(i915, IA_AUD_CFG(t), cfg);
	/* CTS from the hardware */
	ia_rmw(i915, IA_AUD_M_CTS_ENABLE(t), IA_M_CTS_M_PROG_ENABLE | IA_M_CTS_M_VALUE_INDEX, 0);
}

/* The audio clocks that follow CDCLK: the Haswell/Broadwell display HDA
 * controller derives its link bit clock from it (told to the controller
 * driver), and display version 13+ has the audio timestamp clock as a
 * fraction of it (24 MHz = CDCLK * M / N with M = 60). */
static void ia_ts_cdclk(struct i915_device *i915)
{
	uint32_t cd = intel_cdclk_khz(i915);

	if (!cd)
		return;
	hda_controller_set_cdclk((int)cd);
	if (ia_ver(i915) < 13)
		return;
	ia_rmw(i915, IA_AUD_TS_CDCLK_M, IA_AUD_TS_CDCLK_M_EN, 0);
	ia_wr(i915, IA_AUD_TS_CDCLK_N, cd * 60u / 24000u);
	ia_wr(i915, IA_AUD_TS_CDCLK_M, 60u | IA_AUD_TS_CDCLK_M_EN);
}

/* CDCLK at least twice the 96 MHz audio bit clock (display version 9+),
 * as far as that can change without taking the pipes down. */
static void ia_cdclk_for_audio(struct i915_device *i915)
{
	uint32_t want;
	int rc;

	if (ia_ver(i915) < 9)
		return;
	if (intel_cdclk_khz(i915) >= 2 * 96000)
		return;
	want = intel_cdclk_round_khz(i915, 2 * 96000);
	if (!want || intel_cdclk_change_needs_pipes_off(i915, want)) {
		if (!g_ia.cdclk_warned++)
			kprintf("[drm] i915: audio wants CDCLK %u kHz (now %u); it changes with the next mode set\n",
				2 * 96000u, intel_cdclk_khz(i915));
		return;
	}
	rc = intel_cdclk_update(i915, NULL, 0);
	if (rc == 0 && intel_cdclk_khz(i915) < 2 * 96000)
		rc = intel_cdclk_set(i915, want);
	if (rc && !g_ia.cdclk_warned++)
		kprintf("[drm] i915: CDCLK not raised for audio (%d)\n", rc);
	ia_ts_cdclk(i915);
}

/* ---- the DisplayPort audio workarounds ------------------------------------------- */

static uint64_t ia_div_up(uint64_t a, uint64_t b)
{
	return b ? (a + b - 1) / b : 0;
}

/* Where in the horizontal blank the audio of a compressed 4K DisplayPort
 * stream has to start (link symbols), from the stream's timing, link and
 * compressed rate. */
static uint32_t ia_hblank_early_prog(struct i915_device *i915, const struct intel_output *o,
				     const struct intel_pipe *p)
{
	const uint64_t fec = 972261; /* FEC efficiency * 1e6 */
	uint64_t h_active = p->mode.hdisplay, h_total = p->mode.htotal;
	uint64_t pclk = p->mode.clock, link = o->link_rate_khz, lanes = (uint64_t)o->lane_count;
	uint64_t bpp16 = p->dsc.compressed_bpp_x16, cd = intel_cdclk_khz(i915);
	uint64_t avail, need, delta, tu_data, tu_line, active, rise;

	if (!pclk || !link || !lanes || !bpp16 || !cd || h_total <= h_active)
		return 0;
	avail = (h_total - h_active) * link / pclk;
	avail = avail > 28 ? avail - 28 : 0;
	need = ia_div_up(192000ull * h_total, 1000ull * pclk) * (48 / lanes + 2);
	if (avail > need)
		delta = 32;
	else
		delta = ia_div_up(5ull * (link + cd) * pclk, link * cd);
	tu_data = (pclk * bpp16 * 8ull * 1000000ull) / (link * lanes * 16ull * fec);
	tu_line = (h_active * link * fec) / (64ull * pclk * 1000000ull);
	active = (tu_line ? tu_line - 1 : 0) * 64 + tu_data;
	rise = (active + 6 * ia_div_up(active, 250) + 4) * pclk / link;
	if (rise > h_active + delta)
		return 0;
	return (uint32_t)(h_active - rise + delta);
}

static uint32_t ia_samples_room(const struct intel_output *o, const struct intel_pipe *p)
{
	uint64_t h_active = p->mode.hdisplay, h_total = p->mode.htotal;
	uint64_t pclk = p->mode.clock, link = o->link_rate_khz, lanes = (uint64_t)o->lane_count;
	uint64_t room;

	if (!pclk || !lanes || h_total <= h_active)
		return 0;
	room = (h_total - h_active) * link;
	if (room < 12 * pclk)
		return 0;
	return (uint32_t)((room - 12 * pclk) / (pclk * (48 / lanes + 2)));
}

static void ia_dp_audio_be(struct i915_device *i915, const struct intel_output *o,
			   const struct intel_pipe *p, int t)
{
	uint32_t v;

	if (ia_ver(i915) < 11)
		return;
	v = ia_rd(i915, IA_AUD_CONFIG_BE);
	if (ia_ver(i915) == 11)
		v |= IA_HBLANK_EARLY_ENABLE_ICL(t);
	else
		v |= IA_HBLANK_EARLY_ENABLE_TGL(t);
	if (p->dsc.enable && p->mode.hdisplay >= 3840 && p->mode.vdisplay >= 2160) {
		uint32_t prog = ia_hblank_early_prog(i915, o, p);
		uint32_t room = ia_samples_room(o, p);
		uint32_t cnt = prog < 32 ? IA_HBLANK_START_COUNT_32 :
			       prog < 64 ? IA_HBLANK_START_COUNT_64 :
			       prog < 96 ? IA_HBLANK_START_COUNT_96 :
					   IA_HBLANK_START_COUNT_128;
		v &= ~IA_HBLANK_START_COUNT_MASK(t);
		v |= IA_HBLANK_START_COUNT(t, cnt);
		v &= ~IA_SAMPLES_PER_LINE_MASK(t);
		/* 0: every sample is in the buffer */
		v |= IA_SAMPLES_PER_LINE(t, room < 3 ? room : 0);
	}
	ia_wr(i915, IA_AUD_CONFIG_BE, v);
}

static int ia_has_dp20(struct i915_device *i915)
{
	return ia_ver(i915) >= 14 || ia_plat(i915, I915_PLATFORM_DG2);
}

/* Audio hangs with the minimum-hblank fix on these (Battlemage, Lunar
 * Lake, Panther Lake): the fix is turned off while audio runs. */
static int ia_needs_min_hblank_wa(struct i915_device *i915)
{
	int v = intel_display_verx100(i915);
	return v == 1401 || v == 2000 || v == 3000;
}

/* ---- enable / disable -------------------------------------------------------------- */

static void ia_well_get(struct i915_device *i915)
{
	intel_power_get(i915, (enum intel_power_domain)g_ia.dom);
}

static void ia_well_put(struct i915_device *i915)
{
	intel_power_put(i915, (enum intel_power_domain)g_ia.dom);
}

/* Write the ELD into the transcoder's hardware buffer (the rest of the
 * buffer cleared, so nothing of an earlier sink stays readable). */
static void ia_write_eld(struct i915_device *i915, int t, const uint8_t *eld, int len)
{
	uint32_t ctl = ia_rd(i915, IA_AUD_DIP_ELD_CTRL(t));
	uint32_t dw = (ctl & IA_ELD_BUFFER_SIZE_MASK) >> IA_ELD_BUFFER_SIZE_SHIFT;

	if (!dw)
		dw = IA_HW_ELD_DWORDS;
	if (len > (int)(dw * 4))
		len = (int)(dw * 4);
	ia_wr(i915, IA_AUD_DIP_ELD_CTRL(t), ctl & ~IA_ELD_ADDRESS_MASK);
	for (uint32_t i = 0; i < dw; i++) {
		uint32_t v = 0;
		for (int b = 0; b < 4; b++) {
			int at = (int)i * 4 + b;
			if (at < len)
				v |= (uint32_t)eld[at] << (8 * b);
		}
		ia_wr(i915, IA_AUD_EDID_DATA(t), v);
	}
}

void intel_audio_codec_enable(struct i915_device *i915, struct intel_output *o,
			      struct intel_pipe *p)
{
	struct ia_port *st;
	uint8_t eld[DRM_ELD_BUF_SIZE];
	int t, len, port;
	uint32_t v;

	if (!g_ia.ready || !o || !p || !intel_audio_output_wants_audio(i915, o))
		return;
	t = p->transcoder;
	port = o->port;
	if (t < 0 || t >= IA_MAX_TRANS || t == TRANSCODER_EDP || port < 0 || port >= INTEL_MAX_PORTS)
		return;
	len = ia_build_eld(i915, o, eld);
	if (len <= 0)
		return;

	st = &g_ia.port[port];
	mm_memset(st, 0, sizeof(*st));
	st->transcoder = t;
	st->pipe = p->pipe;
	st->is_dp = o->type == INTEL_OUTPUT_DP;
	st->clock_khz = p->mode.clock;
	st->tmds_khz = p->mode.clock;
	if (!st->is_dp && p->dsc.pipe_bpp > 24)
		st->tmds_khz = (uint32_t)((uint64_t)p->mode.clock * p->dsc.pipe_bpp / 24);
	st->link_khz = o->link_rate_khz;
	st->lanes = o->lane_count;
	st->eld_len = len;
	mm_memcpy(st->eld, eld, (size_t)len);
	{
		int mnl = drm_eld_mnl(eld);
		if (mnl > (int)sizeof(st->monitor) - 1)
			mnl = (int)sizeof(st->monitor) - 1;
		mm_memcpy(st->monitor, eld + DRM_ELD_MONITOR_NAME_STRING, (size_t)mnl);
	}

	ia_well_get(i915);
	if (st->is_dp)
		ia_dp_audio_be(i915, o, p, t);
	if (ia_has_dp20(i915))
		ia_rmw(i915, IA_AUD_DP_2DOT0_CTRL(t), IA_AUD_ENABLE_SDP_SPLIT,
		       (st->is_dp && st->link_khz >= 1000000) ? IA_AUD_ENABLE_SDP_SPLIT : 0);
	if (ia_needs_min_hblank_wa(i915))
		ia_rmw(i915, IA_AUD_CHICKENBIT_REG3, 0, IA_DACBE_DISABLE_MIN_HBLANK_FIX);

	/* presence: the codec's pin of this port sees a sink */
	ia_rmw(i915, IA_AUD_PIN_ELD_CP_VLD, 0, IA_AUDIO_OUTPUT_ENABLE(t));
	ia_wait_frames(p, 1);
	/* the ELD: invalid while it is written, valid after */
	ia_rmw(i915, IA_AUD_PIN_ELD_CP_VLD, IA_AUDIO_ELD_VALID(t), 0);
	ia_write_eld(i915, t, eld, len);
	ia_rmw(i915, IA_AUD_PIN_ELD_CP_VLD, 0, IA_AUDIO_ELD_VALID(t));
	ia_config_update(i915, st);
	ia_ts_cdclk(i915);
	v = ia_rd(i915, IA_AUD_PIN_ELD_CP_VLD);
	ia_well_put(i915);

	st->active = 1;
	kprintf("[drm] i915: audio on port %s (transcoder %c): %s \"%s\", %d descriptor(s)%s\n",
		intel_port_name(i915, port), 'A' + t, st->is_dp ? "DisplayPort" : "HDMI",
		st->monitor[0] ? st->monitor : "?", drm_eld_sad_count(eld),
		(v & IA_AUDIO_ELD_VALID(t)) ? "" : " (ELD not latched)");
	snd_display_audio_notify(port, 1);
}

static void ia_disable_transcoder(struct i915_device *i915, int t, int is_dp,
				  const struct intel_pipe *p)
{
	/* the clock settings back to their defaults */
	ia_rmw(i915, IA_AUD_CFG(t), IA_CFG_N_VALUE_INDEX | IA_CFG_N_MASK,
	       IA_CFG_N_PROG_ENABLE | (is_dp ? IA_CFG_N_VALUE_INDEX : 0));
	/* ELD invalid, then (two frames later) presence off */
	ia_rmw(i915, IA_AUD_PIN_ELD_CP_VLD, IA_AUDIO_ELD_VALID(t), 0);
	ia_wait_frames(p, 2);
	ia_rmw(i915, IA_AUD_PIN_ELD_CP_VLD, IA_AUDIO_OUTPUT_ENABLE(t), 0);
	if (ia_has_dp20(i915))
		ia_rmw(i915, IA_AUD_DP_2DOT0_CTRL(t), IA_AUD_ENABLE_SDP_SPLIT, 0);
	if (ia_ver(i915) >= 11) {
		uint32_t bit = ia_ver(i915) == 11 ? IA_HBLANK_EARLY_ENABLE_ICL(t) :
						    IA_HBLANK_EARLY_ENABLE_TGL(t);
		ia_rmw(i915, IA_AUD_CONFIG_BE, bit | IA_HBLANK_START_COUNT_MASK(t) |
						       IA_SAMPLES_PER_LINE_MASK(t), 0);
	}
}

static int ia_any_active_except(int port)
{
	for (int i = 0; i < INTEL_MAX_PORTS; i++)
		if (i != port && g_ia.port[i].active)
			return 1;
	return 0;
}

void intel_audio_codec_disable(struct i915_device *i915, struct intel_output *o,
			       struct intel_pipe *p)
{
	struct ia_port *st;
	int port;

	if (!g_ia.ready || !o || !p)
		return;
	port = o->port;
	if (port < 0 || port >= INTEL_MAX_PORTS)
		return;
	st = &g_ia.port[port];
	if (!st->active || st->transcoder != p->transcoder)
		return;
	ia_well_get(i915);
	ia_disable_transcoder(i915, st->transcoder, st->is_dp, p);
	if (ia_needs_min_hblank_wa(i915) && !ia_any_active_except(port))
		ia_rmw(i915, IA_AUD_CHICKENBIT_REG3, IA_DACBE_DISABLE_MIN_HBLANK_FIX, 0);
	ia_well_put(i915);
	i915_dbg("[drm] i915: audio off on port %s\n", intel_port_name(i915, port));
	mm_memset(st, 0, sizeof(*st));
	snd_display_audio_notify(port, 0);
}

void intel_audio_release_transcoder(struct i915_device *i915, int transcoder)
{
	uint32_t v;

	if (!g_ia.ready || transcoder < 0 || transcoder >= IA_MAX_TRANS)
		return;
	ia_well_get(i915);
	v = ia_rd(i915, IA_AUD_PIN_ELD_CP_VLD);
	if (v & (IA_AUDIO_OUTPUT_ENABLE(transcoder) | IA_AUDIO_ELD_VALID(transcoder))) {
		uint32_t cfg = ia_rd(i915, IA_AUD_CFG(transcoder));
		ia_disable_transcoder(i915, transcoder, (cfg & IA_CFG_N_VALUE_INDEX) != 0, NULL);
	}
	ia_well_put(i915);
	/* a port of ours on it is off too */
	for (int i = 0; i < INTEL_MAX_PORTS; i++) {
		if (g_ia.port[i].active && g_ia.port[i].transcoder == transcoder) {
			mm_memset(&g_ia.port[i], 0, sizeof(g_ia.port[i]));
			snd_display_audio_notify(i, 0);
		}
	}
}

/* ---- power and the codec's wake ----------------------------------------------------- */

/* Skylake and later: the codec signals wake (and so shows up in the
 * controller's codec mask after a reset) only while the display drives
 * this override.  On from init until the sound side has seen the codec. */
static void ia_codec_wake(struct i915_device *i915, int on)
{
	if (ia_ver(i915) < 9)
		return;
	ia_rmw(i915, IA_AUD_CHICKENBIT, IA_SKL_AUD_CODEC_WAKE_SIGNAL, 0);
	lapic_delay_us(1000);
	if (on) {
		ia_rmw(i915, IA_AUD_CHICKENBIT, 0, IA_SKL_AUD_CODEC_WAKE_SIGNAL);
		lapic_delay_us(1000);
	}
	g_ia.wake_forced = on;
}

/* The registers that lose their settings with the well. */
static void ia_restore_regs(struct i915_device *i915)
{
	if (ia_ver(i915) >= 9 && g_ia.freq_valid)
		ia_wr(i915, IA_AUD_FREQ_CNTRL, g_ia.freq_cntrl);
	if (ia_ver(i915) >= 10)
		ia_rmw(i915, IA_AUD_PIN_BUF_CTL, 0, IA_AUD_PIN_BUF_ENABLE);
	ia_ts_cdclk(i915);
}

static void ia_get_power(void *display)
{
	struct i915_device *i915 = display;

	if (!i915 || i915 != g_ia.i915 || !g_ia.ready)
		return;
	intel_display_lock(i915);
	if (g_ia.power_refs++ == 0) {
		ia_well_get(i915);
		ia_restore_regs(i915);
		/* Gemini Lake: CDCLK at twice the bit clock while the audio
		 * is powered, not only while a pipe plays it */
		if (ia_plat(i915, I915_PLATFORM_GEMINILAKE))
			ia_cdclk_for_audio(i915);
	}
	intel_display_unlock(i915);
}

static void ia_put_power(void *display)
{
	struct i915_device *i915 = display;

	if (!i915 || i915 != g_ia.i915 || !g_ia.ready)
		return;
	intel_display_lock(i915);
	if (g_ia.power_refs > 0 && --g_ia.power_refs == 0)
		ia_well_put(i915);
	intel_display_unlock(i915);
}

static int ia_rate_ok(int rate)
{
	switch (rate) {
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
	case 176400:
	case 192000:
		return 1;
	default:
		return 0;
	}
}

static int ia_sync_audio_rate(void *display, int port, int rate)
{
	struct i915_device *i915 = display;
	struct ia_port *st;
	int rc = 0;

	if (!i915 || i915 != g_ia.i915 || !g_ia.ready)
		return -ENODEV;
	if (port < 0 || port >= INTEL_MAX_PORTS || !ia_rate_ok(rate))
		return -EINVAL;
	intel_display_lock(i915);
	st = &g_ia.port[port];
	if (!st->active) {
		rc = -ENODEV;
	} else {
		st->rate = rate;
		ia_well_get(i915);
		ia_config_update(i915, st);
		ia_cdclk_for_audio(i915);
		ia_well_put(i915);
	}
	intel_display_unlock(i915);
	return rc;
}

static int ia_get_eld(void *display, int port, uint8_t *buf, int max)
{
	struct i915_device *i915 = display;
	struct ia_port *st;
	int n = 0;

	if (!i915 || i915 != g_ia.i915 || !g_ia.ready || !buf || max <= 0)
		return 0;
	if (port < 0 || port >= INTEL_MAX_PORTS)
		return 0;
	intel_display_lock(i915);
	/* the sound side has the codec: the wake override can go */
	if (g_ia.wake_forced) {
		ia_well_get(i915);
		ia_codec_wake(i915, 0);
		ia_well_put(i915);
	}
	st = &g_ia.port[port];
	if (st->active && st->eld_len > 0) {
		n = st->eld_len < max ? st->eld_len : max;
		mm_memcpy(buf, st->eld, (size_t)n);
	}
	intel_display_unlock(i915);
	return n;
}

static const struct snd_display_audio_ops ia_ops = {
	.get_power = ia_get_power,
	.put_power = ia_put_power,
	.sync_audio_rate = ia_sync_audio_rate,
	.get_eld = ia_get_eld,
};

/* ---- init / fini / suspend / resume -------------------------------------------------- */

/* AUD_FREQ_CNTRL: version 12 and later get the 8T/96 MHz setting, except
 * Tiger and Rocket Lake, whose firmware value is kept unless it is the one
 * known to be wrong; older versions keep the firmware's. */
static void ia_freq_policy(struct i915_device *i915)
{
	uint32_t init, use;

	if (ia_ver(i915) < 9)
		return;
	init = ia_rd(i915, IA_AUD_FREQ_CNTRL);
	use = ia_ver(i915) >= 12 ? IA_FREQ_GEN12 : init;
	if ((ia_plat(i915, I915_PLATFORM_TIGERLAKE) || ia_plat(i915, I915_PLATFORM_ROCKETLAKE)) &&
	    init != IA_FREQ_TGL_BROKEN)
		use = init;
	g_ia.freq_cntrl = use;
	g_ia.freq_valid = 1;
}

/* Audio the firmware left on a transcoder that is not running is turned
 * off, so the codec does not report a sink that is not there. */
static void ia_clean_firmware_state(struct i915_device *i915)
{
	uint32_t v = ia_rd(i915, IA_AUD_PIN_ELD_CP_VLD);

	for (int t = 0; t < IA_MAX_TRANS && t < i915->info->num_pipes; t++) {
		if (!(v & (IA_AUDIO_OUTPUT_ENABLE(t) | IA_AUDIO_ELD_VALID(t))))
			continue;
		if (ia_rd(i915, TRANS_CONF(t)) & TRANS_CONF_ENABLE)
			continue; /* the firmware's running pipe keeps it until released */
		v &= ~(IA_AUDIO_OUTPUT_ENABLE(t) | IA_AUDIO_ELD_VALID(t));
	}
	ia_wr(i915, IA_AUD_PIN_ELD_CP_VLD, v);
}

void intel_audio_init(struct i915_device *i915)
{
	if (!i915 || !i915->display.ready || intel_legacy_display_active(i915))
		return;
	mm_memset(&g_ia, 0, sizeof(g_ia));
	g_ia.i915 = i915;
	g_ia.dom = ia_domain(i915);

	/* The well stays up while the driver runs: the codec in it must
	 * answer the controller's probe, its presence and ELD verbs and the
	 * jack checks whatever pipes happen to be lit. */
	ia_well_get(i915);
	g_ia.well_held = 1;
	ia_freq_policy(i915);
	ia_restore_regs(i915);
	ia_clean_firmware_state(i915);
	ia_codec_wake(i915, 1);
	g_ia.ready = 1;

	snd_display_audio_register(&ia_ops, i915);
	g_ia.registered = 1;
	kprintf("[drm] i915: display audio ready (AUD_FREQ_CNTRL %08x, CDCLK %u kHz)\n",
		ia_ver(i915) >= 9 ? ia_rd(i915, IA_AUD_FREQ_CNTRL) : 0, intel_cdclk_khz(i915));
}

void intel_audio_fini(struct i915_device *i915)
{
	if (!g_ia.ready || i915 != g_ia.i915)
		return;
	for (int port = 0; port < INTEL_MAX_PORTS; port++)
		if (g_ia.port[port].active)
			snd_display_audio_notify(port, 0);
	if (g_ia.registered)
		snd_display_audio_register(NULL, NULL);
	g_ia.registered = 0;
	g_ia.ready = 0;
	if (g_ia.wake_forced)
		ia_codec_wake(i915, 0);
	while (g_ia.power_refs > 0) {
		g_ia.power_refs--;
		ia_well_put(i915);
	}
	if (g_ia.well_held) {
		ia_well_put(i915);
		g_ia.well_held = 0;
	}
}

void intel_audio_suspend(struct i915_device *i915)
{
	if (!g_ia.ready || i915 != g_ia.i915)
		return;
	/* the outputs went off through intel_audio_codec_disable(); what is
	 * left is a port the firmware had */
	for (int port = 0; port < INTEL_MAX_PORTS; port++) {
		if (g_ia.port[port].active) {
			mm_memset(&g_ia.port[port], 0, sizeof(g_ia.port[port]));
			snd_display_audio_notify(port, 0);
		}
	}
}

void intel_audio_resume(struct i915_device *i915)
{
	if (!g_ia.ready || i915 != g_ia.i915)
		return;
	/* intel_power_init() started the counts afresh: our references (and
	 * the sound side's) are taken again */
	intel_display_lock(i915);
	if (g_ia.well_held)
		ia_well_get(i915);
	if (g_ia.power_refs > 0)
		ia_well_get(i915);
	ia_restore_regs(i915);
	ia_clean_firmware_state(i915);
	ia_codec_wake(i915, 1);
	intel_display_unlock(i915);
}
