// LikeOS -- integrated HDMI and DVI ports of G4X, Ironlake to Ivy Bridge, Valleyview and Cherryview.
//
// From G4X on, the SDVO port registers can drive a TMDS transmitter of
// their own: the port is switched to HDMI encoding and sends the pipe's
// pixels at the pixel clock, which the pipe's PLL (G4X, Valleyview,
// Cherryview) or a PCH PLL behind FDI (Ironlake to Ivy Bridge) makes.  G4X
// has ports B and C at the old SDVO offsets, the PCH B, C and D, Valleyview
// B and C and Cherryview B, C and D in the display block, the last two
// with a DPIO PHY that needs its lanes, clocks and swing set around the PLL
// and port enable.  An HDMI sink is told what it receives with data
// islands in the blanking -- the AVI, SPD and HDMI vendor infoframes and,
// past G4X, the general control packet -- written through the video DIP
// buffer: one buffer for the whole chip on G4X, one per pipe elsewhere.  A
// DVI sink gets the same stream with no packets.  Sinks are found by their
// EDID on the port's DDC pin.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2020 Intel Corporation
// Portions Copyright (C) 2006 Dave Airlie

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the video DIP (data island packet) buffer ------------------------------ */

/* G4X: one buffer, its port selected in the control register */
#define HDMI_DIP_CTL_G4X 0x61170u
#define HDMI_DIP_DATA_G4X 0x61178u
/* Ironlake to Ivy Bridge: one per PCH transcoder */
#define HDMI_DIP_CTL_PCH(p) (0xe0200u + (uint32_t)(p) * 0x1000u)
#define HDMI_DIP_DATA_PCH(p) (0xe0208u + (uint32_t)(p) * 0x1000u)
#define HDMI_DIP_GCP_PCH(p) (0xe0210u + (uint32_t)(p) * 0x1000u)
/* Valleyview/Cherryview: one per pipe (display-relative) */
#define HDMI_DIP_CTL_VLV_A 0x60200u
#define HDMI_DIP_CTL_VLV_B 0x61170u
#define HDMI_DIP_CTL_CHV_C 0x611f0u
#define HDMI_DIP_DATA_VLV_A 0x60208u
#define HDMI_DIP_DATA_VLV_B 0x61174u
#define HDMI_DIP_DATA_CHV_C 0x611f4u
#define HDMI_DIP_GCP_VLV_A 0x60210u
#define HDMI_DIP_GCP_VLV_B 0x61178u
#define HDMI_DIP_GCP_CHV_C 0x611f8u

#define HDMI_DIP_ENABLE (1u << 31)
#define HDMI_DIP_PORT(port) ((uint32_t)(port) << 29)
#define HDMI_DIP_PORT_MASK (3u << 29)
#define HDMI_DIP_ENABLE_GCP (1u << 25) /* not G4X */
#define HDMI_DIP_ENABLE_AVI (1u << 21)
#define HDMI_DIP_ENABLE_VENDOR (2u << 21)
#define HDMI_DIP_ENABLE_GAMUT (4u << 21) /* not G4X */
#define HDMI_DIP_ENABLE_SPD (8u << 21)
#define HDMI_DIP_SELECT_AVI (0u << 19)
#define HDMI_DIP_SELECT_VENDOR (1u << 19)
#define HDMI_DIP_SELECT_SPD (3u << 19)
#define HDMI_DIP_SELECT_MASK (3u << 19)
#define HDMI_DIP_FREQ_VSYNC (1u << 16)
#define HDMI_DIP_FREQ_MASK (3u << 16)
#define HDMI_DIP_DATA_SIZE 32

#define HDMI_GCP_COLOR_INDICATION (1u << 2)
#define HDMI_GCP_DEFAULT_PHASE_ENABLE (1u << 1)

/* Valleyview/Cherryview: port D's lanes report ready in the PHY status */
#define HDMI_DPIO_PHY_STATUS 0x6240u
#define HDMI_DPLL_PORTD_READY_MASK 0xfu

/* infoframe types */
#define HDMI_IF_VENDOR 0x81
#define HDMI_IF_AVI 0x82
#define HDMI_IF_SPD 0x83

/* TMDS limits */
#define HDMI_MIN_TMDS_KHZ 25000u
#define HDMI_DVI_MAX_TMDS_KHZ 165000u

struct lg_hdmi {
	uint32_t max_tmds_khz; /* what the source can send */
	int dvi_only; /* the VBT calls the port DVI */
};

static char port_name(int port)
{
	return (char)('A' + port);
}

static int hdmi_is_g4x(struct lg_display *d)
{
	return d->is_g4x || d->is_gm45;
}

static int hdmi_is_vlv(struct lg_display *d)
{
	return d->is_vlv || d->is_chv;
}

/* ---- TMDS clock limits ----------------------------------------------------------- */

static uint32_t hdmi_source_max_tmds(struct lg_display *d)
{
	if (d->ver >= 8)
		return 300000;
	if (d->ver >= 5)
		return 225000;
	return 165000;
}

/* A DVI sink without an EDID limit is single link: 165 MHz. */
static uint32_t hdmi_port_clock_limit(struct lg_display *d, struct lg_output *o,
				      int respect_downstream, int has_hdmi_sink)
{
	struct lg_hdmi *h = o->priv;
	uint32_t max = h ? h->max_tmds_khz : hdmi_source_max_tmds(d);

	if (respect_downstream && !has_hdmi_sink && max > HDMI_DVI_MAX_TMDS_KHZ)
		max = HDMI_DVI_MAX_TMDS_KHZ;
	return max;
}

static int hdmi_port_clock_valid(struct lg_display *d, struct lg_output *o, uint32_t clock,
				 int respect_downstream, int has_hdmi_sink)
{
	if (clock < HDMI_MIN_TMDS_KHZ)
		return -ERANGE;
	if (clock > hdmi_port_clock_limit(d, o, respect_downstream, has_hdmi_sink))
		return -ERANGE;
	/* The Cherryview PLL cannot make 216 to 240 MHz. */
	if (d->is_chv && clock > 216000 && clock < 240000)
		return -ERANGE;
	return 0;
}

/* ---- CEA-861 video codes ---------------------------------------------------------- */

/* Picture aspect codes as the AVI infoframe carries them. */
#define HDMI_ASPECT_NONE 0
#define HDMI_ASPECT_4_3 1
#define HDMI_ASPECT_16_9 2

static int cea_vic_aspect(int vic)
{
	static const uint8_t four_three[] = { 1,  2,  6,  8,  10, 12, 14, 17, 21, 23, 25, 27,
					      29, 35, 37, 42, 44, 48, 50, 52, 54, 56, 58 };

	for (unsigned i = 0; i < sizeof(four_three); i++)
		if (four_three[i] == vic)
			return HDMI_ASPECT_4_3;
	if (vic >= 1 && vic <= 64)
		return HDMI_ASPECT_16_9;
	if (vic >= 93 && vic <= 97)
		return HDMI_ASPECT_16_9;
	/* 64:27 and 256:135 cannot be said in the infoframe; the VIC
	 * implies them. */
	return HDMI_ASPECT_NONE;
}

static int mode_aspect(const struct drm_mode_modeinfo *m)
{
	switch (m->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		return HDMI_ASPECT_4_3;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		return HDMI_ASPECT_16_9;
	default:
		return HDMI_ASPECT_NONE;
	}
}

static uint32_t khz_to_picos(uint32_t khz)
{
	return khz ? 1000000000u / khz : 0;
}

/* The CEA video code of a mode: the same timing, at the code's clock or
 * at its 1000/1001 variant (59.94 Hz and friends), the same sync and
 * interlace flags; when the mode names a picture aspect, that too.  0
 * when the mode is no CEA timing. */
static int hdmi_match_cea_mode(const struct drm_mode_modeinfo *m)
{
	const uint32_t fl = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC |
			    DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK;
	int aspect = mode_aspect(m);
	struct drm_mode_modeinfo c;

	if (!m->clock)
		return 0;
	for (int vic = 1; vic < 128; vic++) {
		if (drm_mode_cea_vic((uint8_t)vic, &c) != 0)
			continue;
		if (c.hdisplay != m->hdisplay || c.hsync_start != m->hsync_start ||
		    c.hsync_end != m->hsync_end || c.htotal != m->htotal ||
		    c.vdisplay != m->vdisplay || c.vsync_start != m->vsync_start ||
		    c.vsync_end != m->vsync_end || c.vtotal != m->vtotal ||
		    (c.flags & fl) != (m->flags & fl))
			continue;
		uint32_t c1 = c.clock;
		uint32_t c2 = (uint32_t)DIV_ROUND_CLOSEST((uint64_t)c1 * 1000u, 1001u);
		uint32_t pm = khz_to_picos(m->clock);

		if (pm != khz_to_picos(c1) && pm != khz_to_picos(c2))
			continue;
		if (aspect != HDMI_ASPECT_NONE && aspect != cea_vic_aspect(vic))
			continue;
		return vic;
	}
	return 0;
}

/* The four 4K timings HDMI 1.4b numbers itself: sent as an HDMI VIC in
 * the vendor infoframe instead of a CEA code in the AVI infoframe. */
static int hdmi_vic_for_cea(int vic)
{
	switch (vic) {
	case 95:
		return 1; /* 3840x2160 30 Hz */
	case 94:
		return 2; /* 25 Hz */
	case 93:
		return 3; /* 24 Hz */
	case 98:
		return 4; /* 4096x2160 24 Hz */
	default:
		return 0;
	}
}

/* CEA-861 default encoding: every CEA timing but 640x480 is limited range. */
static int hdmi_default_range_limited(const struct drm_mode_modeinfo *m)
{
	return hdmi_match_cea_mode(m) > 1;
}

/* ---- infoframes --------------------------------------------------------------- */

/* Pack a frame the way the DIP buffer wants it: the three header bytes,
 * a hole the hardware fills with the header's ECC, the checksum, the
 * payload.  The checksum makes header, checksum and payload sum to zero.
 * Returns the number of bytes used of the zeroed 32-byte buffer. */
static int hdmi_pack(uint8_t type, uint8_t version, const uint8_t *payload, int len,
		     uint8_t out[HDMI_DIP_DATA_SIZE])
{
	unsigned sum = type + version + (unsigned)len;

	mm_memset(out, 0, HDMI_DIP_DATA_SIZE);
	out[0] = type;
	out[1] = version;
	out[2] = (uint8_t)len;
	out[3] = 0;
	for (int i = 0; i < len; i++) {
		out[5 + i] = payload[i];
		sum += payload[i];
	}
	out[4] = (uint8_t)(0x100u - (sum & 0xffu));
	return 5 + len;
}

static int hdmi_avi_frame(const struct lg_config *cfg, int has_hdmi_infoframe,
			  uint8_t out[HDMI_DIP_DATA_SIZE])
{
	const struct drm_mode_modeinfo *m = &cfg->mode;
	uint8_t pb[13];
	int cea = hdmi_match_cea_mode(m);
	int vic = cea;
	int aspect;

	/* A 4K HDMI timing goes in the vendor frame; codes past 64 are
	 * CEA-861-F and would confuse an HDMI 1.4 sink. */
	if (has_hdmi_infoframe && hdmi_vic_for_cea(cea))
		vic = 0;
	if (vic > 64)
		vic = 0;

	aspect = mode_aspect(m);
	if (aspect == HDMI_ASPECT_NONE && vic)
		aspect = cea_vic_aspect(vic);
	else if (aspect == HDMI_ASPECT_NONE && hdmi_vic_for_cea(cea) && has_hdmi_infoframe)
		aspect = hdmi_vic_for_cea(cea) == 4 ? HDMI_ASPECT_NONE : HDMI_ASPECT_16_9;

	mm_memset(pb, 0, sizeof(pb));
	/* RGB, active format present, no bars, no scan information */
	pb[0] = (0u << 5) | (1u << 4);
	/* no colorimetry, the picture aspect, active format = the picture */
	pb[1] = (uint8_t)((0u << 6) | ((unsigned)aspect << 4) | 8u);
	/* RGB quantization: say what is sent (limited 1, full 2); it is the
	 * mode's default range, which a sink accepts without the QS bit */
	pb[2] = (uint8_t)((cfg->limited_color_range ? 1u : 2u) << 2);
	pb[3] = (uint8_t)(vic & 0x7f);
	/* YCC quantization limited, no content type, the pixel repetition */
	pb[4] = (uint8_t)((cfg->pixel_multiplier > 1 ? cfg->pixel_multiplier - 1 : 0) & 0xf);
	return hdmi_pack(HDMI_IF_AVI, 2, pb, 13, out);
}

static int hdmi_spd_frame(uint8_t out[HDMI_DIP_DATA_SIZE])
{
	static const char vendor[] = "Intel";
	static const char product[] = "Integrated gfx";
	uint8_t pb[25];

	mm_memset(pb, 0, sizeof(pb));
	for (unsigned i = 0; i < sizeof(vendor) - 1 && i < 8; i++)
		pb[i] = (uint8_t)vendor[i];
	for (unsigned i = 0; i < sizeof(product) - 1 && i < 16; i++)
		pb[8 + i] = (uint8_t)product[i];
	pb[24] = 0x09; /* source device: PC general */
	return hdmi_pack(HDMI_IF_SPD, 1, pb, 25, out);
}

static int hdmi_vendor_frame(const struct lg_config *cfg, uint8_t out[HDMI_DIP_DATA_SIZE])
{
	uint8_t pb[5];
	int hvic = hdmi_vic_for_cea(hdmi_match_cea_mode(&cfg->mode));

	/* the HDMI licensing OUI, 00-0C-03, least significant byte first */
	pb[0] = 0x03;
	pb[1] = 0x0c;
	pb[2] = 0x00;
	if (hvic) {
		pb[3] = 1u << 5; /* extended resolution format */
		pb[4] = (uint8_t)hvic;
		return hdmi_pack(HDMI_IF_VENDOR, 1, pb, 5, out);
	}
	pb[3] = 0; /* no additional video format */
	return hdmi_pack(HDMI_IF_VENDOR, 1, pb, 4, out);
}

static uint32_t dip_ctl_reg(struct lg_display *d, int pipe)
{
	if (hdmi_is_vlv(d))
		return pipe == 0 ? HDMI_DIP_CTL_VLV_A :
		       pipe == 1 ? HDMI_DIP_CTL_VLV_B : HDMI_DIP_CTL_CHV_C;
	if (d->pch != LG_PCH_NONE)
		return HDMI_DIP_CTL_PCH(pipe);
	return HDMI_DIP_CTL_G4X;
}

static uint32_t dip_data_reg(struct lg_display *d, int pipe)
{
	if (hdmi_is_vlv(d))
		return pipe == 0 ? HDMI_DIP_DATA_VLV_A :
		       pipe == 1 ? HDMI_DIP_DATA_VLV_B : HDMI_DIP_DATA_CHV_C;
	if (d->pch != LG_PCH_NONE)
		return HDMI_DIP_DATA_PCH(pipe);
	return HDMI_DIP_DATA_G4X;
}

/* 0 on G4X, which has no general control packet. */
static uint32_t dip_gcp_reg(struct lg_display *d, int pipe)
{
	if (hdmi_is_vlv(d))
		return pipe == 0 ? HDMI_DIP_GCP_VLV_A :
		       pipe == 1 ? HDMI_DIP_GCP_VLV_B : HDMI_DIP_GCP_CHV_C;
	if (d->pch != LG_PCH_NONE)
		return HDMI_DIP_GCP_PCH(pipe);
	return 0;
}

static uint32_t dip_select(uint8_t type)
{
	switch (type) {
	case HDMI_IF_VENDOR:
		return HDMI_DIP_SELECT_VENDOR;
	case HDMI_IF_SPD:
		return HDMI_DIP_SELECT_SPD;
	default:
		return HDMI_DIP_SELECT_AVI;
	}
}

static uint32_t dip_enable(uint8_t type)
{
	switch (type) {
	case HDMI_IF_VENDOR:
		return HDMI_DIP_ENABLE_VENDOR;
	case HDMI_IF_SPD:
		return HDMI_DIP_ENABLE_SPD;
	default:
		return HDMI_DIP_ENABLE_AVI;
	}
}

/* Load one frame into its slot of the buffer.  The slot's enable goes off
 * while it is rewritten -- except the AVI slot on Cougar Point, which the
 * hardware wants updated with its enable left on.  Every byte of the slot
 * is written so the hardware's ECC covers the right data. */
static void dip_write(struct lg_display *d, int pipe, uint8_t type, const uint8_t *buf, int len)
{
	uint32_t reg = dip_ctl_reg(d, pipe);
	uint32_t data = dip_data_reg(d, pipe);
	uint32_t val = lg_rd(d, reg);
	int i;

	if (!(val & HDMI_DIP_ENABLE))
		i915_dbg("[drm] i915: HDMI: writing a DIP with the buffer disabled\n");

	val &= ~(HDMI_DIP_SELECT_MASK | 0xfu); /* and the data offset */
	val |= dip_select(type);
	if (!(d->pch == LG_PCH_CPT && type == HDMI_IF_AVI))
		val &= ~dip_enable(type);
	lg_wr(d, reg, val);

	for (i = 0; i < len; i += 4) {
		uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
			     ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
		lg_wr(d, data, w);
	}
	for (; i < HDMI_DIP_DATA_SIZE; i += 4)
		lg_wr(d, data, 0);

	val |= dip_enable(type);
	val &= ~HDMI_DIP_FREQ_MASK;
	val |= HDMI_DIP_FREQ_VSYNC;
	lg_wr(d, reg, val);
	lg_posting_read(d, reg);
}

/* Can the deep-colour packing phase be announced as the default?  Only
 * when every horizontal timing value falls on a whole pixel group. */
static int gcp_default_phase_possible(int pipe_bpp, const struct lg_timings *t)
{
	unsigned ppg;

	switch (pipe_bpp) {
	case 30:
		ppg = 4; /* 4 pixels in 5 clocks */
		break;
	case 36:
		ppg = 2; /* 2 pixels in 3 clocks */
		break;
	case 48:
		ppg = 1; /* 1 pixel in 2 clocks */
		break;
	default:
		return 0; /* no phase at 8 bpc */
	}
	return t->hdisplay % ppg == 0 && t->htotal % ppg == 0 && t->hblank_start % ppg == 0 &&
	       t->hblank_end % ppg == 0 && t->hsync_start % ppg == 0 &&
	       t->hsync_end % ppg == 0 &&
	       (!(t->flags & DRM_MODE_FLAG_INTERLACE) || (t->htotal / 2) % ppg == 0);
}

/* The general control packet (not on G4X): deep-colour indication. */
static int hdmi_set_gcp(struct lg_display *d, const struct lg_config *cfg)
{
	uint32_t reg = dip_gcp_reg(d, cfg->pipe);
	uint32_t gcp = 0;

	if (hdmi_is_g4x(d) || !cfg->has_infoframe || !reg)
		return 0;
	if (cfg->pipe_bpp > 24)
		gcp |= HDMI_GCP_COLOR_INDICATION;
	if (gcp_default_phase_possible(cfg->pipe_bpp, &cfg->t))
		gcp |= HDMI_GCP_DEFAULT_PHASE_ENABLE;
	lg_wr(d, reg, gcp);
	return 1;
}

static void hdmi_write_frames(struct lg_display *d, const struct lg_config *cfg)
{
	uint8_t buf[HDMI_DIP_DATA_SIZE];
	int n;

	/* has_infoframe implies an HDMI sink, which takes the vendor frame */
	n = hdmi_avi_frame(cfg, 1, buf);
	dip_write(d, cfg->pipe, HDMI_IF_AVI, buf, n);
	n = hdmi_spd_frame(buf);
	dip_write(d, cfg->pipe, HDMI_IF_SPD, buf, n);
	n = hdmi_vendor_frame(cfg, buf);
	dip_write(d, cfg->pipe, HDMI_IF_VENDOR, buf, n);
}

/* Turn the packets on (with the port still off) or off.
 *
 * Zeroed control registers would select the AVI slot at "send once",
 * which confuses the hardware (the AVI frame must go every frame); so
 * the register is not written more than needed, and the AVI slot and the
 * every-vsync frequency are always selected explicitly. */
static void hdmi_set_infoframes(struct lg_display *d, struct lg_output *o,
				const struct lg_config *cfg, int enable)
{
	uint32_t reg = dip_ctl_reg(d, cfg->pipe);
	uint32_t val = lg_rd(d, reg);
	uint32_t port = HDMI_DIP_PORT(o->port);
	uint32_t all = HDMI_DIP_ENABLE_AVI | HDMI_DIP_ENABLE_VENDOR | HDMI_DIP_ENABLE_SPD;

	if (!hdmi_is_g4x(d))
		all |= HDMI_DIP_ENABLE_GAMUT | HDMI_DIP_ENABLE_GCP;

	val |= HDMI_DIP_SELECT_AVI | HDMI_DIP_FREQ_VSYNC;

	if (!enable) {
		if (!(val & HDMI_DIP_ENABLE))
			return;
		if (hdmi_is_g4x(d) && port != (val & HDMI_DIP_PORT_MASK)) {
			i915_dbg("[drm] i915: HDMI-%c: video DIP still enabled on port %c\n",
				 port_name(o->port), port_name((int)((val & HDMI_DIP_PORT_MASK) >> 29)));
			return;
		}
		val &= ~(HDMI_DIP_ENABLE | all);
		lg_wr(d, reg, val);
		lg_posting_read(d, reg);
		return;
	}

	if (hdmi_is_g4x(d)) {
		/* one buffer for the chip: the first HDMI port keeps it */
		if (port != (val & HDMI_DIP_PORT_MASK)) {
			if (val & HDMI_DIP_ENABLE) {
				i915_dbg("[drm] i915: HDMI-%c: video DIP already enabled on port %c\n",
					 port_name(o->port),
					 port_name((int)((val & HDMI_DIP_PORT_MASK) >> 29)));
				return;
			}
			val &= ~HDMI_DIP_PORT_MASK;
			val |= port;
		}
		val |= HDMI_DIP_ENABLE;
		val &= ~all;
	} else if (d->pch == LG_PCH_CPT) {
		/* the buffer and the AVI slot go on and off together */
		val |= HDMI_DIP_ENABLE | HDMI_DIP_ENABLE_AVI;
		val &= ~(HDMI_DIP_ENABLE_VENDOR | HDMI_DIP_ENABLE_GAMUT | HDMI_DIP_ENABLE_SPD |
			 HDMI_DIP_ENABLE_GCP);
	} else {
		if (port != (val & HDMI_DIP_PORT_MASK)) {
			if (val & HDMI_DIP_ENABLE)
				i915_dbg("[drm] i915: HDMI-%c: DIP already enabled on port %c\n",
					 port_name(o->port),
					 port_name((int)((val & HDMI_DIP_PORT_MASK) >> 29)));
			val &= ~HDMI_DIP_PORT_MASK;
			val |= port;
		}
		val |= HDMI_DIP_ENABLE;
		val &= ~all;
	}

	if (hdmi_set_gcp(d, cfg))
		val |= HDMI_DIP_ENABLE_GCP;

	lg_wr(d, reg, val);
	lg_posting_read(d, reg);

	hdmi_write_frames(d, cfg);
}

/* ---- the port ---------------------------------------------------------------- */

/* The port register for the mode, still disabled. */
static void hdmi_prepare(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	uint32_t val = SDVO_ENCODING_HDMI;

	if (d->pch == LG_PCH_NONE && cfg->limited_color_range)
		val |= HDMI_COLOR_RANGE_16_235;
	if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC)
		val |= SDVO_VSYNC_ACTIVE_HIGH;
	if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC)
		val |= SDVO_HSYNC_ACTIVE_HIGH;
	if (cfg->pipe_bpp > 24)
		val |= HDMI_COLOR_FORMAT_12bpc;
	else
		val |= SDVO_COLOR_FORMAT_8bpc;
	if (cfg->has_hdmi_sink)
		val |= HDMI_MODE_SELECT_HDMI;

	if (d->pch == LG_PCH_CPT)
		val |= SDVO_PIPE_SEL_CPT(cfg->pipe);
	else if (d->is_chv)
		val |= SDVO_PIPE_SEL_CHV(cfg->pipe);
	else
		val |= SDVO_PIPE_SEL(cfg->pipe);

	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);
}

static void hdmi_enable_port(struct lg_display *d, struct lg_output *o)
{
	uint32_t val = lg_rd(d, o->reg);

	val |= SDVO_ENABLE;
	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);
}

/* Valleyview/Cherryview: wait for the PHY lanes of the port to report
 * ready (B and C in DPLL A's register, D in the PHY status). */
static void vlv_wait_port_ready(struct lg_display *d, struct lg_output *o, uint32_t expected)
{
	uint32_t reg, mask;

	switch (o->port) {
	case LG_PORT_C:
		reg = DPLL(d, 0);
		mask = DPLL_PORTC_READY_MASK;
		expected <<= 4;
		break;
	case LG_PORT_D:
		reg = HDMI_DPIO_PHY_STATUS;
		mask = HDMI_DPLL_PORTD_READY_MASK;
		break;
	default:
		reg = DPLL(d, 0);
		mask = DPLL_PORTB_READY_MASK;
		break;
	}
	if (lg_wait(d, reg, mask, expected, 1000000))
		kprintf("[drm] i915: HDMI-%c: timed out waiting for the port to be ready "
			"(0x%x, expected 0x%x)\n",
			port_name(o->port), lg_rd(d, reg) & mask, expected);
}

static int hdmi_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	uint32_t val = lg_rd(d, o->reg);

	if (d->pch == LG_PCH_CPT)
		*pipe = (int)((val & SDVO_PIPE_SEL_MASK_CPT) >> SDVO_PIPE_SEL_SHIFT_CPT);
	else if (d->is_chv)
		*pipe = (int)((val & SDVO_PIPE_SEL_MASK_CHV) >> SDVO_PIPE_SEL_SHIFT_CHV);
	else
		*pipe = (int)((val & SDVO_PIPE_SEL_MASK) >> SDVO_PIPE_SEL_SHIFT);
	return !!(val & SDVO_ENABLE);
}

/* GMCH and PCH: the port register and the packets before the pipe runs. */
static void hdmi_pre_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	hdmi_prepare(d, o, cfg);
	hdmi_set_infoframes(d, o, cfg, cfg->has_infoframe);
}

static void g4x_hdmi_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	(void)cfg;
	hdmi_enable_port(d, o);
}

static void ibx_hdmi_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	uint32_t val = lg_rd(d, o->reg);

	val |= SDVO_ENABLE;
	/* The first write may be lost: write it twice. */
	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);
	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);

	/* 12 bpc with pixel repetition needs the enable toggled off and on
	 * again. */
	if (cfg->pipe_bpp > 24 && cfg->pixel_multiplier > 1) {
		lg_wr(d, o->reg, val & ~SDVO_ENABLE);
		lg_posting_read(d, o->reg);
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);
	}
}

/* Cougar Point: 12 bpc must be reached through 8 bpc with the HDMI unit's
 * clock gating off (disable gating, enable at 8 bpc, switch to 12 bpc,
 * gating back on). */
static void cpt_hdmi_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	uint32_t val = lg_rd(d, o->reg);

	val |= SDVO_ENABLE;
	if (cfg->pipe_bpp > 24) {
		lg_rmw(d, TRANS_CHICKEN1(cfg->pipe), 0, TRANS_CHICKEN1_HDMIUNIT_GC_DISABLE);
		val &= ~SDVO_COLOR_FORMAT_MASK;
		val |= SDVO_COLOR_FORMAT_8bpc;
	}
	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);

	if (cfg->pipe_bpp > 24) {
		val &= ~SDVO_COLOR_FORMAT_MASK;
		val |= HDMI_COLOR_FORMAT_12bpc;
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);
		lg_rmw(d, TRANS_CHICKEN1(cfg->pipe), TRANS_CHICKEN1_HDMIUNIT_GC_DISABLE, 0);
	}
}

static void hdmi_disable_port(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	uint32_t val = lg_rd(d, o->reg);

	val &= ~SDVO_ENABLE;
	lg_wr(d, o->reg, val);
	lg_posting_read(d, o->reg);

	/* Ibex Peak: a port left selecting transcoder B keeps the DP port
	 * sharing it from being enabled on transcoder A, so it is moved to A
	 * (briefly enabled there, written twice as the first write may be
	 * lost) before it is left off. */
	if (d->pch == LG_PCH_IBX && cfg->pipe == 1) {
		val &= ~SDVO_PIPE_SEL_MASK;
		val |= SDVO_ENABLE | SDVO_PIPE_SEL(0);
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);

		val &= ~SDVO_ENABLE;
		lg_wr(d, o->reg, val);
		lg_posting_read(d, o->reg);

		if (d->pipes[0].active)
			lg_wait_for_vblank(d, 0);
	}

	hdmi_set_infoframes(d, o, cfg, 0);
}

/* GMCH (and Valleyview/Cherryview): off right after the plane. */
static void g4x_hdmi_disable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	hdmi_disable_port(d, o, cfg);
}

/* PCH: the port goes off after the pipe and FDI. */
static void pch_hdmi_post_disable(struct lg_display *d, struct lg_output *o,
				  const struct lg_config *cfg)
{
	hdmi_disable_port(d, o, cfg);
}

/* Valleyview */
static void vlv_hdmi_pre_pll_enable(struct lg_display *d, struct lg_output *o,
				    const struct lg_config *cfg)
{
	hdmi_prepare(d, o, cfg);
	lg_vlv_phy_pre_pll_enable(d, o, cfg);
}

static void vlv_hdmi_pre_enable(struct lg_display *d, struct lg_output *o,
				const struct lg_config *cfg)
{
	lg_vlv_phy_pre_encoder_enable(d, o, cfg);
	/* HDMI 1.0 V, -2 dB */
	lg_vlv_set_phy_signal_level(d, o, cfg, 0x2b245f5f, 0x00002000, 0x5578b83a, 0x2b247878);
	hdmi_set_infoframes(d, o, cfg, cfg->has_infoframe);
	hdmi_enable_port(d, o);
	vlv_wait_port_ready(d, o, 0x0);
}

static void vlv_hdmi_post_disable(struct lg_display *d, struct lg_output *o,
				  const struct lg_config *cfg)
{
	/* reset the lanes, or the next enable flickers */
	lg_vlv_phy_reset_lanes(d, o, cfg);
}

/* Cherryview */
static void chv_hdmi_pre_pll_enable(struct lg_display *d, struct lg_output *o,
				    const struct lg_config *cfg)
{
	hdmi_prepare(d, o, cfg);
	lg_chv_phy_pre_pll_enable(d, o, cfg);
}

static void chv_hdmi_pre_enable(struct lg_display *d, struct lg_output *o,
				const struct lg_config *cfg)
{
	lg_chv_phy_pre_encoder_enable(d, o, cfg);
	/* 800 mV, 0 dB */
	lg_chv_set_phy_signal_level(d, o, cfg, 128, 102, 0);
	hdmi_set_infoframes(d, o, cfg, cfg->has_infoframe);
	hdmi_enable_port(d, o);
	vlv_wait_port_ready(d, o, 0x0);
	/* the second common lane stays up on its own from here */
	lg_chv_phy_release_cl2_override(d, o);
}

static void chv_hdmi_post_disable(struct lg_display *d, struct lg_output *o,
				  const struct lg_config *cfg)
{
	/* hold the data lanes in reset */
	lg_chv_data_lane_soft_reset(d, o, cfg, 1);
}

static void chv_hdmi_post_pll_disable(struct lg_display *d, struct lg_output *o,
				      const struct lg_config *cfg)
{
	lg_chv_phy_post_pll_disable(d, o, cfg);
}

/* ---- detection and modes ---------------------------------------------------------- */

/* The EDID over the port's DDC; when the GMBUS controller fails, once
 * more bit-banging the pins. */
static int hdmi_read_edid(struct lg_display *d, struct lg_output *o)
{
	int rc = lg_read_edid(d, o);

	if (rc) {
		i915_dbg("[drm] i915: HDMI-%c: GMBUS EDID read failed, retrying with bit-banging\n",
			 port_name(o->port));
		lg_gmbus_force_bit(d, o->ddc_pin, 1);
		rc = lg_read_edid(d, o);
		lg_gmbus_force_bit(d, o->ddc_pin, 0);
	}
	return rc;
}

/* The live hotplug state of these ports is known to be unreliable, so the
 * sink counts as there when a digital EDID can be read. */
static int hdmi_detect(struct lg_display *d, struct lg_output *o)
{
	int live = lg_hpd_live(d, o->hpd_pin);
	int connected = 0;

	o->edid_len = 0;
	o->hdmi_sink = 0;
	o->has_audio = 0;
	if (o->ddc && hdmi_read_edid(d, o) == 0 && o->edid_len >= 128 &&
	    (o->edid[20] & 0x80))
		connected = 1;
	if (!connected) {
		o->hdmi_sink = 0;
		o->has_audio = 0;
	}
	i915_dbg("[drm] i915: HDMI-%c: live %d, %s\n", port_name(o->port), live,
		 connected ? (o->hdmi_sink ? "HDMI sink" : "DVI sink") : "nothing");
	o->detected = connected;
	return connected;
}

static int hdmi_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	return lg_get_modes_edid(d, o, conn, hdmi_port_clock_limit(d, o, 1, o->hdmi_sink));
}

static int hdmi_mode_valid(struct lg_display *d, struct lg_output *o,
			   const struct drm_mode_modeinfo *m)
{
	uint32_t clock = m->clock;

	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	if (d->max_dotclk_khz && clock > d->max_dotclk_khz)
		return -EINVAL;
	if (m->flags & DRM_MODE_FLAG_DBLCLK) {
		/* pixel repetition needs an HDMI sink */
		if (!o->hdmi_sink)
			return -EINVAL;
		clock *= 2;
	}
	if (clock > 600000)
		return -EINVAL;
	/* only 8 bpc on these parts' modes list: no sink deep colour data */
	return hdmi_port_clock_valid(d, o, clock, 1, o->hdmi_sink) ? -EINVAL : 0;
}

/* G4X can send packets from one HDMI port at a time: another port that
 * already carries them keeps them. */
static int g4x_infoframe_port_taken(struct lg_display *d, struct lg_output *o)
{
	for (int i = 0; i < d->nout; i++) {
		struct lg_output *x = &d->out[i];

		if (x == o || x->type != LG_OUTPUT_HDMI || !x->active)
			continue;
		if (x->cfg.has_hdmi_sink)
			return 1;
	}
	return 0;
}

static int hdmi_compute_config(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	const struct drm_mode_modeinfo *m = &cfg->mode;
	uint32_t clock = m->clock;

	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	if (d->pch != LG_PCH_NONE)
		cfg->has_pch_encoder = 1;

	cfg->has_hdmi_sink = o->hdmi_sink;
	if (hdmi_is_g4x(d) && cfg->has_hdmi_sink && g4x_infoframe_port_taken(d, o))
		cfg->has_hdmi_sink = 0;
	cfg->has_infoframe = cfg->has_hdmi_sink;

	if (m->flags & DRM_MODE_FLAG_DBLCLK) {
		cfg->pixel_multiplier = 2;
		clock *= 2;
	}

	/* 8 bpc; the sink's TMDS limit first, and if that fails, what the
	 * user asked for as long as the source can send it */
	if (hdmi_port_clock_valid(d, o, clock, 1, cfg->has_hdmi_sink) &&
	    hdmi_port_clock_valid(d, o, clock, 0, cfg->has_hdmi_sink)) {
		i915_dbg("[drm] i915: HDMI-%c: unsupported clock %u kHz, rejecting mode\n",
			 port_name(o->port), m->clock);
		return -EINVAL;
	}
	cfg->port_clock = clock;
	if (cfg->pipe_bpp > 24)
		cfg->pipe_bpp = 24;

	/* CEA-861 default encoding: limited range for CEA timings on an HDMI
	 * sink, full range otherwise */
	cfg->limited_color_range = cfg->has_hdmi_sink && hdmi_default_range_limited(m);
	cfg->has_audio = 0;
	return 0;
}

/* ---- init ---------------------------------------------------------------- */

static const struct lg_output_funcs g4x_hdmi_funcs = {
	.detect = hdmi_detect,
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
	.compute_config = hdmi_compute_config,
	.pre_enable = hdmi_pre_enable,
	.enable = g4x_hdmi_enable,
	.disable = g4x_hdmi_disable,
	.get_hw_state = hdmi_get_hw_state,
};

static const struct lg_output_funcs ibx_hdmi_funcs = {
	.detect = hdmi_detect,
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
	.compute_config = hdmi_compute_config,
	.pre_enable = hdmi_pre_enable,
	.enable = ibx_hdmi_enable,
	.post_disable = pch_hdmi_post_disable,
	.get_hw_state = hdmi_get_hw_state,
};

static const struct lg_output_funcs cpt_hdmi_funcs = {
	.detect = hdmi_detect,
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
	.compute_config = hdmi_compute_config,
	.pre_enable = hdmi_pre_enable,
	.enable = cpt_hdmi_enable,
	.post_disable = pch_hdmi_post_disable,
	.get_hw_state = hdmi_get_hw_state,
};

static const struct lg_output_funcs vlv_hdmi_funcs = {
	.detect = hdmi_detect,
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
	.compute_config = hdmi_compute_config,
	.pre_pll_enable = vlv_hdmi_pre_pll_enable,
	.pre_enable = vlv_hdmi_pre_enable,
	.disable = g4x_hdmi_disable,
	.post_disable = vlv_hdmi_post_disable,
	.get_hw_state = hdmi_get_hw_state,
};

static const struct lg_output_funcs chv_hdmi_funcs = {
	.detect = hdmi_detect,
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
	.compute_config = hdmi_compute_config,
	.pre_pll_enable = chv_hdmi_pre_pll_enable,
	.pre_enable = chv_hdmi_pre_enable,
	.disable = g4x_hdmi_disable,
	.post_disable = chv_hdmi_post_disable,
	.post_pll_disable = chv_hdmi_post_pll_disable,
	.get_hw_state = hdmi_get_hw_state,
};

static uint8_t hdmi_default_ddc_pin(struct lg_display *d, int port)
{
	switch (port) {
	case LG_PORT_C:
		return LG_GMBUS_PIN_DPC;
	case LG_PORT_D:
		return d->is_chv ? LG_GMBUS_PIN_DPD_CHV : LG_GMBUS_PIN_DPD;
	default:
		return LG_GMBUS_PIN_DPB;
	}
}

static uint8_t hdmi_ddc_pin(struct lg_display *d, int port, const struct lg_vbt_child *child)
{
	uint8_t pin = 0;
	const char *source = "VBT";

	if (child && child->ddc_pin) {
		pin = child->ddc_pin;
		if (!lg_gmbus_pin_valid(d, pin)) {
			i915_dbg("[drm] i915: HDMI-%c: VBT DDC pin %u invalid\n", port_name(port),
				 pin);
			pin = 0;
		}
	}
	if (!pin) {
		pin = hdmi_default_ddc_pin(d, port);
		source = "platform default";
	}
	if (!lg_gmbus_pin_valid(d, pin)) {
		i915_dbg("[drm] i915: HDMI-%c: invalid DDC pin %u\n", port_name(port), pin);
		return 0;
	}
	/* two HDMI ports cannot share one DDC bus */
	for (int i = 0; i < d->nout; i++) {
		if (d->out[i].type == LG_OUTPUT_HDMI && d->out[i].ddc_pin == pin) {
			i915_dbg("[drm] i915: HDMI-%c: DDC pin %u already claimed by %s\n",
				 port_name(port), pin, d->out[i].name);
			return 0;
		}
	}
	i915_dbg("[drm] i915: HDMI-%c: using DDC pin %u (%s)\n", port_name(port), pin, source);
	return pin;
}

void lg_hdmi_init(struct lg_display *d, uint32_t reg, int port)
{
	const struct lg_vbt_child *child;
	struct lg_output *o;
	struct lg_hdmi *h;
	uint8_t pin;

	if (hdmi_is_g4x(d) || d->is_vlv) {
		if (port != LG_PORT_B && port != LG_PORT_C) {
			kprintf("[drm] i915: no HDMI on port %c on this platform\n", port_name(port));
			return;
		}
	} else if (port < LG_PORT_B || port > LG_PORT_D) {
		kprintf("[drm] i915: no HDMI on port %c on this platform\n", port_name(port));
		return;
	}

	child = lg_vbt_child_for_port(d, port);
	if (!child)
		i915_dbg("[drm] i915: no VBT child device for HDMI-%c\n", port_name(port));

	pin = hdmi_ddc_pin(d, port, child);
	if (!pin)
		return;

	h = kalloc(sizeof(*h));
	if (!h)
		return;
	mm_memset(h, 0, sizeof(*h));
	h->max_tmds_khz = hdmi_source_max_tmds(d);
	h->dvi_only = child && child->supports_dvi && !child->supports_hdmi;

	o = lg_output_new(d);
	if (!o) {
		kfree(h);
		return;
	}

	o->type = LG_OUTPUT_HDMI;
	o->port = port;
	o->reg = reg;
	ksnprintf(o->name, sizeof(o->name), "HDMI-%c", port_name(port));
	if (d->is_chv)
		o->funcs = &chv_hdmi_funcs;
	else if (d->is_vlv)
		o->funcs = &vlv_hdmi_funcs;
	else if (d->pch == LG_PCH_CPT)
		o->funcs = &cpt_hdmi_funcs;
	else if (d->pch == LG_PCH_IBX)
		o->funcs = &ibx_hdmi_funcs;
	else
		o->funcs = &g4x_hdmi_funcs;
	o->conn_type = h->dvi_only ? DRM_MODE_CONNECTOR_DVID : DRM_MODE_CONNECTOR_HDMIA;
	o->enc_type = DRM_MODE_ENCODER_TMDS;
	if (d->is_chv)
		o->pipe_mask = port == LG_PORT_D ? (1u << 2) : ((1u << 0) | (1u << 1));
	else
		o->pipe_mask = (1u << d->num_pipes) - 1;
	o->hpd_pin = port == LG_PORT_B ? LG_HPD_PORT_B :
		     port == LG_PORT_C ? LG_HPD_PORT_C : LG_HPD_PORT_D;
	o->polled = 0;
	o->ddc_pin = pin;
	o->ddc = lg_gmbus_adapter(d, pin);
	o->priv = h;

	kprintf("[drm] i915: %s at 0x%x (DDC pin %u, TMDS up to %u MHz)\n", o->name, reg, pin,
		h->max_tmds_khz / 1000);
}
