// LikeOS -- HDMI helpers: what the connector state says, put into the
// infoframes; the TMDS character rate of a mode; audio clock regeneration.
//
// A client sets "Colorspace", "content type" and HDR_OUTPUT_METADATA on an
// HDMI connector; these turn the committed values into the AVI infoframe's
// colorimetry and content type fields and into the Dynamic Range and
// Mastering infoframe.  drm_hdmi_compute_mode_clock() answers what TMDS
// character rate a mode needs at a colour depth and pixel encoding, the
// number a driver compares against the sink's and the port's TMDS limits.
// drm_hdmi_acr_get_n_cts() gives the N and CTS values the audio clock
// regeneration packets carry.
//
// Pure computation: no locks, no sleeping.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the HDMI helper code: MIT

#include <kernel/dev/gpu/drm_hdmi_helper.h>
#include <kernel/dev/gpu/hdmi.h>
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* An HDR metadata EOTF the sink did not list is still sent (the client
 * asked for it); a note in the log only with this set. */
#ifndef DRM_HDMI_HELPER_DEBUG
#define DRM_HDMI_HELPER_DEBUG 0
#endif

static inline bool is_eotf_supported(u8 output_eotf, u8 sink_eotf)
{
	return (sink_eotf & BIT(output_eotf)) != 0;
}

int drm_hdmi_infoframe_set_hdr_metadata(struct hdmi_drm_infoframe *frame,
					const struct drm_connector_state *conn_state)
{
	struct drm_connector *connector;
	const struct hdr_output_metadata *hdr_metadata;
	int err;

	if (!frame || !conn_state)
		return -EINVAL;

	connector = conn_state->conn;

	if (!conn_state->hdr_output_metadata)
		return -EINVAL;

	hdr_metadata = conn_state->hdr_output_metadata->data;

	if (!hdr_metadata || !connector)
		return -EINVAL;
	/* the property code checks the blob's size; a short one is not
	 * read past */
	if (conn_state->hdr_output_metadata->length < sizeof(*hdr_metadata))
		return -EINVAL;

	/* The sink's EOTF field is a bitmap, the infoframe's a value. */
	if (!is_eotf_supported(hdr_metadata->hdmi_metadata_type1.eotf,
			       connector->display_info.hdr_sink_metadata.hdmi_type1.eotf) &&
	    DRM_HDMI_HELPER_DEBUG)
		kprintf("drm: hdmi: unknown EOTF %d\n",
			hdr_metadata->hdmi_metadata_type1.eotf);

	err = hdmi_drm_infoframe_init(frame);
	if (err < 0)
		return err;

	frame->eotf = hdr_metadata->hdmi_metadata_type1.eotf;
	frame->metadata_type = hdr_metadata->hdmi_metadata_type1.metadata_type;

	_Static_assert(sizeof(frame->display_primaries) ==
		       sizeof(hdr_metadata->hdmi_metadata_type1.display_primaries),
		       "display primaries layout");
	_Static_assert(sizeof(frame->white_point) ==
		       sizeof(hdr_metadata->hdmi_metadata_type1.white_point),
		       "white point layout");

	for (int i = 0; i < 3; i++) {
		frame->display_primaries[i].x =
			hdr_metadata->hdmi_metadata_type1.display_primaries[i].x;
		frame->display_primaries[i].y =
			hdr_metadata->hdmi_metadata_type1.display_primaries[i].y;
	}
	frame->white_point.x = hdr_metadata->hdmi_metadata_type1.white_point.x;
	frame->white_point.y = hdr_metadata->hdmi_metadata_type1.white_point.y;

	frame->max_display_mastering_luminance =
		hdr_metadata->hdmi_metadata_type1.max_display_mastering_luminance;
	frame->min_display_mastering_luminance =
		hdr_metadata->hdmi_metadata_type1.min_display_mastering_luminance;
	frame->max_fall = hdr_metadata->hdmi_metadata_type1.max_fall;
	frame->max_cll = hdr_metadata->hdmi_metadata_type1.max_cll;

	return 0;
}

/* HDMI colorimetry: the AVI C field, the EC field and the ACE field (the
 * additional colorimetry extension of CTA-861-G) packed side by side. */
#define FULL_COLORIMETRY_MASK 0x1FF
#define NORMAL_COLORIMETRY_MASK 0x3
#define EXTENDED_COLORIMETRY_MASK 0x7
#define EXTENDED_ACE_COLORIMETRY_MASK 0xF

#define C(x) ((x) << 0)
#define EC(x) ((x) << 2)
#define ACE(x) ((x) << 5)

#define HDMI_COLORIMETRY_NO_DATA 0x0
#define HDMI_COLORIMETRY_SMPTE_170M_YCC (C(1) | EC(0) | ACE(0))
#define HDMI_COLORIMETRY_BT709_YCC (C(2) | EC(0) | ACE(0))
#define HDMI_COLORIMETRY_XVYCC_601 (C(3) | EC(0) | ACE(0))
#define HDMI_COLORIMETRY_XVYCC_709 (C(3) | EC(1) | ACE(0))
#define HDMI_COLORIMETRY_SYCC_601 (C(3) | EC(2) | ACE(0))
#define HDMI_COLORIMETRY_OPYCC_601 (C(3) | EC(3) | ACE(0))
#define HDMI_COLORIMETRY_OPRGB (C(3) | EC(4) | ACE(0))
#define HDMI_COLORIMETRY_BT2020_CYCC (C(3) | EC(5) | ACE(0))
#define HDMI_COLORIMETRY_BT2020_RGB (C(3) | EC(6) | ACE(0))
#define HDMI_COLORIMETRY_BT2020_YCC (C(3) | EC(6) | ACE(0))
#define HDMI_COLORIMETRY_DCI_P3_RGB_D65 (C(3) | EC(7) | ACE(0))
#define HDMI_COLORIMETRY_DCI_P3_RGB_THEATER (C(3) | EC(7) | ACE(1))

/* By "Colorspace" value; the ones past BT2020_YCC (the DCI-P3 and wide
 * gamut RGB values a DisplayPort sink takes) have no AVI encoding and
 * read as no data. */
static const u32 hdmi_colorimetry_val[] = {
	[DRM_MODE_COLORIMETRY_NO_DATA] = HDMI_COLORIMETRY_NO_DATA,
	[DRM_MODE_COLORIMETRY_SMPTE_170M_YCC] = HDMI_COLORIMETRY_SMPTE_170M_YCC,
	[DRM_MODE_COLORIMETRY_BT709_YCC] = HDMI_COLORIMETRY_BT709_YCC,
	[DRM_MODE_COLORIMETRY_XVYCC_601] = HDMI_COLORIMETRY_XVYCC_601,
	[DRM_MODE_COLORIMETRY_XVYCC_709] = HDMI_COLORIMETRY_XVYCC_709,
	[DRM_MODE_COLORIMETRY_SYCC_601] = HDMI_COLORIMETRY_SYCC_601,
	[DRM_MODE_COLORIMETRY_OPYCC_601] = HDMI_COLORIMETRY_OPYCC_601,
	[DRM_MODE_COLORIMETRY_OPRGB] = HDMI_COLORIMETRY_OPRGB,
	[DRM_MODE_COLORIMETRY_BT2020_CYCC] = HDMI_COLORIMETRY_BT2020_CYCC,
	[DRM_MODE_COLORIMETRY_BT2020_RGB] = HDMI_COLORIMETRY_BT2020_RGB,
	[DRM_MODE_COLORIMETRY_BT2020_YCC] = HDMI_COLORIMETRY_BT2020_YCC,
};

#undef C
#undef EC
#undef ACE

void drm_hdmi_avi_infoframe_colorimetry(struct hdmi_avi_infoframe *frame,
					const struct drm_connector_state *conn_state)
{
	u32 colorimetry_val;
	u32 colorimetry_index = conn_state->colorspace & FULL_COLORIMETRY_MASK;

	if (colorimetry_index >= ARRAY_SIZE(hdmi_colorimetry_val))
		colorimetry_val = HDMI_COLORIMETRY_NO_DATA;
	else
		colorimetry_val = hdmi_colorimetry_val[colorimetry_index];

	frame->colorimetry = colorimetry_val & NORMAL_COLORIMETRY_MASK;
	/* The ACE values (DCI-P3 theater) would need fields the AVI frame
	 * structure does not have yet; only C and EC are carried. */
	frame->extended_colorimetry = (colorimetry_val >> 2) &
				      EXTENDED_COLORIMETRY_MASK;
}

void drm_hdmi_avi_infoframe_bars(struct hdmi_avi_infoframe *frame,
				 const struct drm_connector_state *conn_state)
{
	/* The bars are the TV margins of the state; LikeOS connector
	 * states have no margin properties, so there are none. */
	(void)conn_state;
	frame->right_bar = 0;
	frame->left_bar = 0;
	frame->top_bar = 0;
	frame->bottom_bar = 0;
}

void drm_hdmi_avi_infoframe_content_type(struct hdmi_avi_infoframe *frame,
					 const struct drm_connector_state *conn_state)
{
	switch (conn_state->content_type) {
	case DRM_MODE_CONTENT_TYPE_GRAPHICS:
		frame->content_type = HDMI_CONTENT_TYPE_GRAPHICS;
		break;
	case DRM_MODE_CONTENT_TYPE_CINEMA:
		frame->content_type = HDMI_CONTENT_TYPE_CINEMA;
		break;
	case DRM_MODE_CONTENT_TYPE_GAME:
		frame->content_type = HDMI_CONTENT_TYPE_GAME;
		break;
	case DRM_MODE_CONTENT_TYPE_PHOTO:
		frame->content_type = HDMI_CONTENT_TYPE_PHOTO;
		break;
	default:
		/* Graphics is the default(0) */
		frame->content_type = HDMI_CONTENT_TYPE_GRAPHICS;
	}

	frame->itc = conn_state->content_type != DRM_MODE_CONTENT_TYPE_NO_DATA;
}

unsigned long long
drm_hdmi_compute_mode_clock(const struct drm_display_mode *mode,
			    unsigned int bpc,
			    enum drm_output_color_format fmt)
{
	unsigned long long clock = (unsigned long long)mode->clock * 1000ULL;
	unsigned int vic = drm_match_cea_mode(mode);

	/* CTA-861-G section 5.4 (Color Coding and Quantization): VIC 1 is
	 * always sent at 8 bpc. */
	if (vic == 1 && bpc != 8)
		return 0;

	if (fmt == DRM_OUTPUT_COLOR_FORMAT_YCBCR422) {
		/* HDMI 1.0 section 6.5 (Pixel Encoding): 4:2:2 sends 24 bits
		 * over three channels, Cb and Cr on odd and even pixels;
		 * fewer than 12 bpc are left justified. */
		if (bpc > 12)
			return 0;

		/* Two 12-bit components over three TMDS channels per pixel
		 * clock: the same character rate as three 8-bit RGB
		 * components. */
		bpc = 8;
	}

	/* HDMI 2.0 section 7.1 (YCbCr 4:2:0 Pixel Encoding): 4:2:0 runs at a
	 * TMDS character rate of half the pixel clock. */
	if (fmt == DRM_OUTPUT_COLOR_FORMAT_YCBCR420)
		clock = clock / 2;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		clock = clock * 2;

	return DIV_ROUND_CLOSEST_ULL(clock * bpc, 8);
}

struct drm_hdmi_acr_n_cts_entry {
	unsigned int n;
	unsigned int cts;
};

struct drm_hdmi_acr_data {
	unsigned long tmds_clock_khz;
	struct drm_hdmi_acr_n_cts_entry n_cts_32k,
					n_cts_44k1,
					n_cts_48k;
};

static const struct drm_hdmi_acr_data hdmi_acr_n_cts[] = {
	{
		/* "Other" entry */
		.n_cts_32k =  { .n = 4096, },
		.n_cts_44k1 = { .n = 6272, },
		.n_cts_48k =  { .n = 6144, },
	}, {
		.tmds_clock_khz = 25175,
		.n_cts_32k =  { .n = 4576,  .cts = 28125, },
		.n_cts_44k1 = { .n = 7007,  .cts = 31250, },
		.n_cts_48k =  { .n = 6864,  .cts = 28125, },
	}, {
		.tmds_clock_khz = 25200,
		.n_cts_32k =  { .n = 4096,  .cts = 25200, },
		.n_cts_44k1 = { .n = 6272,  .cts = 28000, },
		.n_cts_48k =  { .n = 6144,  .cts = 25200, },
	}, {
		.tmds_clock_khz = 27000,
		.n_cts_32k =  { .n = 4096,  .cts = 27000, },
		.n_cts_44k1 = { .n = 6272,  .cts = 30000, },
		.n_cts_48k =  { .n = 6144,  .cts = 27000, },
	}, {
		.tmds_clock_khz = 27027,
		.n_cts_32k =  { .n = 4096,  .cts = 27027, },
		.n_cts_44k1 = { .n = 6272,  .cts = 30030, },
		.n_cts_48k =  { .n = 6144,  .cts = 27027, },
	}, {
		.tmds_clock_khz = 54000,
		.n_cts_32k =  { .n = 4096,  .cts = 54000, },
		.n_cts_44k1 = { .n = 6272,  .cts = 60000, },
		.n_cts_48k =  { .n = 6144,  .cts = 54000, },
	}, {
		.tmds_clock_khz = 54054,
		.n_cts_32k =  { .n = 4096,  .cts = 54054, },
		.n_cts_44k1 = { .n = 6272,  .cts = 60060, },
		.n_cts_48k =  { .n = 6144,  .cts = 54054, },
	}, {
		.tmds_clock_khz = 74176,
		.n_cts_32k =  { .n = 11648, .cts = 210937, }, /* and 210938 */
		.n_cts_44k1 = { .n = 17836, .cts = 234375, },
		.n_cts_48k =  { .n = 11648, .cts = 140625, },
	}, {
		.tmds_clock_khz = 74250,
		.n_cts_32k =  { .n = 4096,  .cts = 74250, },
		.n_cts_44k1 = { .n = 6272,  .cts = 82500, },
		.n_cts_48k =  { .n = 6144,  .cts = 74250, },
	}, {
		.tmds_clock_khz = 148352,
		.n_cts_32k =  { .n = 11648, .cts = 421875, },
		.n_cts_44k1 = { .n = 8918,  .cts = 234375, },
		.n_cts_48k =  { .n = 5824,  .cts = 140625, },
	}, {
		.tmds_clock_khz = 148500,
		.n_cts_32k =  { .n = 4096,  .cts = 148500, },
		.n_cts_44k1 = { .n = 6272,  .cts = 165000, },
		.n_cts_48k =  { .n = 6144,  .cts = 148500, },
	}, {
		.tmds_clock_khz = 296703,
		.n_cts_32k =  { .n = 5824,  .cts = 421875, },
		.n_cts_44k1 = { .n = 4459,  .cts = 234375, },
		.n_cts_48k =  { .n = 5824,  .cts = 281250, },
	}, {
		.tmds_clock_khz = 297000,
		.n_cts_32k =  { .n = 3072,  .cts = 222750, },
		.n_cts_44k1 = { .n = 4704,  .cts = 247500, },
		.n_cts_48k =  { .n = 5120,  .cts = 247500, },
	}, {
		.tmds_clock_khz = 593407,
		.n_cts_32k =  { .n = 5824,  .cts = 843750, },
		.n_cts_44k1 = { .n = 8918,  .cts = 937500, },
		.n_cts_48k =  { .n = 5824,  .cts = 562500, },
	}, {
		.tmds_clock_khz = 594000,
		.n_cts_32k =  { .n = 3072,  .cts = 445500, },
		.n_cts_44k1 = { .n = 9408,  .cts = 990000, },
		.n_cts_48k =  { .n = 6144,  .cts = 594000, },
	},
};

static int drm_hdmi_acr_find_tmds_entry(unsigned long tmds_clock_khz)
{
	int i;

	/* skip the "other" entry */
	for (i = 1; i < (int)ARRAY_SIZE(hdmi_acr_n_cts); i++) {
		if (hdmi_acr_n_cts[i].tmds_clock_khz == tmds_clock_khz)
			return i;
	}

	return 0;
}

void
drm_hdmi_acr_get_n_cts(unsigned long long tmds_char_rate,
		       unsigned int sample_rate,
		       unsigned int *out_n,
		       unsigned int *out_cts)
{
	/* rounded to kHz: tolerant of the 1/1.001 rates being a little off */
	unsigned long tmds_clock_khz = (unsigned long)DIV_ROUND_CLOSEST_ULL(tmds_char_rate, 1000);
	const struct drm_hdmi_acr_n_cts_entry *entry;
	unsigned int n, cts, mult = 1;
	int tmds_idx;

	tmds_idx = drm_hdmi_acr_find_tmds_entry(tmds_clock_khz);

	/* The order matters: 192 kHz divides by 48k and by 32k, and takes
	 * the 48k entry. */
	if (sample_rate % 48000 == 0) {
		entry = &hdmi_acr_n_cts[tmds_idx].n_cts_48k;
		mult = sample_rate / 48000;
	} else if (sample_rate % 44100 == 0) {
		entry = &hdmi_acr_n_cts[tmds_idx].n_cts_44k1;
		mult = sample_rate / 44100;
	} else if (sample_rate % 32000 == 0) {
		entry = &hdmi_acr_n_cts[tmds_idx].n_cts_32k;
		mult = sample_rate / 32000;
	} else {
		entry = NULL;
	}

	if (entry) {
		n = entry->n * mult;
		cts = entry->cts;
	} else {
		/* Recommended optimal value, HDMI 1.4b, Section 7.2.1 */
		n = 128 * sample_rate / 1000;
		cts = 0;
	}

	/* A sample rate of 0 would divide by zero below: no clock to
	 * regenerate. */
	if (!cts && sample_rate)
		cts = (unsigned int)DIV_ROUND_CLOSEST_ULL(tmds_char_rate * n,
							  128ULL * sample_rate);

	*out_n = n;
	*out_cts = cts;
}
