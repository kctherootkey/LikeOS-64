/*
 * EDID test vectors for the EDID parser host test (test-edid-corpus.c).
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from Maxime Ripard's EDID test vectors: GPL-2.0
 * Portions by Maxime Ripard <mripard@kernel.org>
 */

#ifndef HOST_TEST_EDID_CORPUS_H
#define HOST_TEST_EDID_CORPUS_H

extern const unsigned char test_edid_dvi_1080p[128];
extern const unsigned char test_edid_hdmi_1080p_rgb_max_100mhz[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_max_200mhz[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_max_200mhz_hdr[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_max_340mhz[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz[256];
extern const unsigned char test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz[256];
extern const unsigned char test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz[256];

#endif
