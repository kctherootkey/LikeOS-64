// LikeOS -- HDMI helpers between the connector state and the infoframes:
// colorimetry, bars, content type and HDR metadata in the infoframes, the
// TMDS character rate of a mode at a colour depth and pixel encoding, and
// the N / CTS values of audio clock regeneration.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the HDMI helper code: MIT

#ifndef KERNEL_DEV_GPU_DRM_HDMI_HELPER_H
#define KERNEL_DEV_GPU_DRM_HDMI_HELPER_H

#include <kernel/dev/gpu/hdmi.h>

struct drm_connector;
struct drm_connector_state;
struct drm_display_mode;

/* The pixel encoding sent on the link: what a TMDS character rate is
 * computed for.  (Not the DRM_COLOR_FORMAT_* bits of a sink's
 * capabilities, which are a mask.) */
#ifndef DRM_OUTPUT_COLOR_FORMAT_DEFINED
#define DRM_OUTPUT_COLOR_FORMAT_DEFINED
enum drm_output_color_format {
	DRM_OUTPUT_COLOR_FORMAT_RGB444 = 0,
	DRM_OUTPUT_COLOR_FORMAT_YCBCR444,
	DRM_OUTPUT_COLOR_FORMAT_YCBCR422,
	DRM_OUTPUT_COLOR_FORMAT_YCBCR420,
	DRM_OUTPUT_COLOR_FORMAT_COUNT,
};
#endif

/* The AVI colorimetry and extended colorimetry for the state's
 * "Colorspace" (DRM_MODE_COLORIMETRY_*). */
void drm_hdmi_avi_infoframe_colorimetry(struct hdmi_avi_infoframe *frame,
					const struct drm_connector_state *conn_state);
/* The AVI bar fields: the connector state carries no TV margins here, so
 * they are cleared. */
void drm_hdmi_avi_infoframe_bars(struct hdmi_avi_infoframe *frame,
				 const struct drm_connector_state *conn_state);
/* The DRM (HDR) infoframe from the state's HDR_OUTPUT_METADATA blob: 0,
 * or -EINVAL when the state has none (send no frame then). */
int drm_hdmi_infoframe_set_hdr_metadata(struct hdmi_drm_infoframe *frame,
					const struct drm_connector_state *conn_state);
/* The AVI content type and IT content flag from the state's
 * "content type". */
void drm_hdmi_avi_infoframe_content_type(struct hdmi_avi_infoframe *frame,
					 const struct drm_connector_state *conn_state);

/* The TMDS character rate in Hz of a mode at `bpc' bits per component in
 * the pixel encoding `fmt'; 0 for a combination HDMI does not allow (VIC
 * 1 at more than 8 bpc, 4:2:2 at more than 12). */
unsigned long long drm_hdmi_compute_mode_clock(const struct drm_display_mode *mode,
					       unsigned int bpc,
					       enum drm_output_color_format fmt);

/* The audio clock regeneration N and CTS values for a TMDS character rate
 * (Hz) and an audio sample rate (Hz): the table values of HDMI 1.4b
 * section 7.2 where it has the rate, computed ones otherwise. */
void drm_hdmi_acr_get_n_cts(unsigned long long tmds_char_rate,
			    unsigned int sample_rate,
			    unsigned int *out_n,
			    unsigned int *out_cts);

#endif
