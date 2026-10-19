// LikeOS -- display-manager core: the fixed tables EDIDs refer to.
//
// The modes and sink facts only the EDID code looks things up in: the
// list of sinks whose EDID needs a correction (quirks), the established
// timings I/II and III, and the geometries tried against a range
// descriptor's formula.  The VESA DMT, CEA-861 and HDMI tables every
// mode user shares are drm_modes.c's.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's and Red Hat's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.

#include "drm_edid_internal.h"

#define EDID_QUIRK(vend_chr_0, vend_chr_1, vend_chr_2, product_id, _quirks) \
	{ \
		.ident = { \
			.panel_id = drm_edid_encode_panel_id(vend_chr_0, vend_chr_1, \
							     vend_chr_2, product_id), \
		}, \
		.quirks = _quirks \
	}

/*
 * EDID blocks out in the wild have a variety of bugs; the sinks known for
 * one are collected here with what the EDID code does about it.  The first
 * match wins.
 */
const struct edid_quirk edid_tbl_quirk_list[] = {
	/* Acer AL1706 */
	EDID_QUIRK('A', 'C', 'R', 44358, BIT(EDID_QUIRK_PREFER_LARGE_60)),
	/* Acer F51 */
	EDID_QUIRK('A', 'P', 'I', 0x7602, BIT(EDID_QUIRK_PREFER_LARGE_60)),

	/* AEO model 0 reports 8 bpc, but is a 6 bpc panel */
	EDID_QUIRK('A', 'E', 'O', 0, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* BenQ GW2765 */
	EDID_QUIRK('B', 'N', 'Q', 0x78d6, BIT(EDID_QUIRK_FORCE_8BPC)),

	/* BOE model on HP Pavilion 15-n233sl reports 8 bpc, but is a 6 bpc panel */
	EDID_QUIRK('B', 'O', 'E', 0x78b, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* CPT panel of Asus UX303LA reports 8 bpc, but is a 6 bpc panel */
	EDID_QUIRK('C', 'P', 'T', 0x17df, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* SDC panel of Lenovo B50-80 reports 8 bpc, but is a 6 bpc panel */
	EDID_QUIRK('S', 'D', 'C', 0x3652, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* BOE model 0x0771 reports 8 bpc, but is a 6 bpc panel */
	EDID_QUIRK('B', 'O', 'E', 0x0771, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* Belinea 10 15 55 */
	EDID_QUIRK('M', 'A', 'X', 1516, BIT(EDID_QUIRK_PREFER_LARGE_60)),
	EDID_QUIRK('M', 'A', 'X', 0x77e, BIT(EDID_QUIRK_PREFER_LARGE_60)),

	/* Envision Peripherals, Inc. EN-7100e */
	EDID_QUIRK('E', 'P', 'I', 59264, BIT(EDID_QUIRK_135_CLOCK_TOO_HIGH)),
	/* Envision EN2028 */
	EDID_QUIRK('E', 'P', 'I', 8232, BIT(EDID_QUIRK_PREFER_LARGE_60)),

	/* Funai Electronics PM36B */
	EDID_QUIRK('F', 'C', 'M', 13600, BIT(EDID_QUIRK_PREFER_LARGE_75) |
					 BIT(EDID_QUIRK_DETAILED_IN_CM)),

	/* LG 27GP950 */
	EDID_QUIRK('G', 'S', 'M', 0x5bbf, BIT(EDID_QUIRK_CAP_DSC_15BPP)),

	/* LG 27GN950 */
	EDID_QUIRK('G', 'S', 'M', 0x5b9a, BIT(EDID_QUIRK_CAP_DSC_15BPP)),

	/* LGD panel of HP zBook 17 G2, eDP 10 bpc, but reports unknown bpc */
	EDID_QUIRK('L', 'G', 'D', 764, BIT(EDID_QUIRK_FORCE_10BPC)),

	/* LG Philips LCD LP154W01-A5 */
	EDID_QUIRK('L', 'P', 'L', 0, BIT(EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE)),
	EDID_QUIRK('L', 'P', 'L', 0x2a00, BIT(EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE)),

	/* Samsung SyncMaster 205BW.  Note: irony */
	EDID_QUIRK('S', 'A', 'M', 541, BIT(EDID_QUIRK_DETAILED_SYNC_PP)),
	/* Samsung SyncMaster 22[5-6]BW */
	EDID_QUIRK('S', 'A', 'M', 596, BIT(EDID_QUIRK_PREFER_LARGE_60)),
	EDID_QUIRK('S', 'A', 'M', 638, BIT(EDID_QUIRK_PREFER_LARGE_60)),

	/* Sony PVM-2541A does up to 12 bpc, but only reports max 8 bpc */
	EDID_QUIRK('S', 'N', 'Y', 0x2541, BIT(EDID_QUIRK_FORCE_12BPC)),

	/* ViewSonic VA2026w */
	EDID_QUIRK('V', 'S', 'C', 5020, BIT(EDID_QUIRK_FORCE_REDUCED_BLANKING)),

	/* Medion MD 30217 PG */
	EDID_QUIRK('M', 'E', 'D', 0x7b8, BIT(EDID_QUIRK_PREFER_LARGE_75)),

	/* Lenovo G50 */
	EDID_QUIRK('S', 'D', 'C', 18514, BIT(EDID_QUIRK_FORCE_6BPC)),

	/* Panel in Samsung NP700G7A-S01PL notebook reports 6bpc */
	EDID_QUIRK('S', 'E', 'C', 0xd033, BIT(EDID_QUIRK_FORCE_8BPC)),

	/* Rotel RSX-1058 forwards sink's EDID but only does HDMI 1.1*/
	EDID_QUIRK('E', 'T', 'R', 13896, BIT(EDID_QUIRK_FORCE_8BPC)),

	/* Valve Index Headset */
	EDID_QUIRK('V', 'L', 'V', 0x91a8, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b0, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b1, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b2, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b3, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b4, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b5, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b6, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b7, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b8, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91b9, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91ba, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91bb, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91bc, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91bd, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91be, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('V', 'L', 'V', 0x91bf, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* HTC Vive and Vive Pro VR Headsets */
	EDID_QUIRK('H', 'V', 'R', 0xaa01, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('H', 'V', 'R', 0xaa02, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* Oculus Rift DK1, DK2, CV1 and Rift S VR Headsets */
	EDID_QUIRK('O', 'V', 'R', 0x0001, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('O', 'V', 'R', 0x0003, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('O', 'V', 'R', 0x0004, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('O', 'V', 'R', 0x0012, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* Windows Mixed Reality Headsets */
	EDID_QUIRK('A', 'C', 'R', 0x7fce, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('L', 'E', 'N', 0x0408, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('F', 'U', 'J', 0x1970, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('D', 'E', 'L', 0x7fce, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('S', 'E', 'C', 0x144a, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('A', 'U', 'S', 0xc102, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* Sony PlayStation VR Headset */
	EDID_QUIRK('S', 'N', 'Y', 0x0704, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* Sensics VR Headsets */
	EDID_QUIRK('S', 'E', 'N', 0x1019, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* OSVR HDK and HDK2 VR Headsets */
	EDID_QUIRK('S', 'V', 'R', 0x1019, BIT(EDID_QUIRK_NON_DESKTOP)),
	EDID_QUIRK('A', 'U', 'O', 0x1111, BIT(EDID_QUIRK_NON_DESKTOP)),

	/* LQ116M1JW10 displays noise when 8 bpc, but display fine as 6 bpc */
	EDID_QUIRK('S', 'H', 'P', 0x154c, BIT(EDID_QUIRK_FORCE_6BPC)),

	/*
	 * @drm_edid_internal_quirk entries end here, following with the
	 * @drm_edid_quirk entries.
	 */

	/* HP ZR24w DP AUX DPCD access requires probing to prevent corruption. */
	EDID_QUIRK('H', 'W', 'P', 0x2869, BIT(DRM_EDID_QUIRK_DP_DPCD_PROBE)),
};
const int edid_tbl_quirk_count = ARRAY_SIZE(edid_tbl_quirk_list);

/*
 * These more or less come from the DMT spec.  The 720x400 modes are
 * inferred from historical 80x25 practice.  The 640x480@67 and 832x624@75
 * modes are old-school Mac modes.  The EDID spec says the 1152x864@75 mode
 * should be 1152x870, again for the Mac, but instead we use the x864 DMT
 * mode.
 *
 * The DMT modes have been fact-checked; the rest are mild guesses.
 */
const struct drm_display_mode edid_tbl_est_modes[] = {
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840,
		   968, 1056, 0, 600, 601, 605, 628, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 800x600@60Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 36000, 800, 824,
		   896, 1024, 0, 600, 601, 603,  625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 800x600@56Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 656,
		   720, 840, 0, 480, 481, 484, 500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 640x480@75Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 664,
		   704,  832, 0, 480, 489, 492, 520, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 640x480@72Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 30240, 640, 704,
		   768,  864, 0, 480, 483, 486, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 640x480@67Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 490, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 640x480@60Hz */
	{ DRM_MODE("720x400", DRM_MODE_TYPE_DRIVER, 35500, 720, 738,
		   846, 900, 0, 400, 421, 423,  449, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 720x400@88Hz */
	{ DRM_MODE("720x400", DRM_MODE_TYPE_DRIVER, 28320, 720, 738,
		   846,  900, 0, 400, 412, 414, 449, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 720x400@70Hz */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 135000, 1280, 1296,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 1280x1024@75Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 78750, 1024, 1040,
		   1136, 1312, 0,  768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 1024x768@75Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 75000, 1024, 1048,
		   1184, 1328, 0,  768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 1024x768@70Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0,  768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 1024x768@60Hz */
	{ DRM_MODE("1024x768i", DRM_MODE_TYPE_DRIVER,44900, 1024, 1032,
		   1208, 1264, 0, 768, 768, 776, 817, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_INTERLACE) }, /* 1024x768@43Hz */
	{ DRM_MODE("832x624", DRM_MODE_TYPE_DRIVER, 57284, 832, 864,
		   928, 1152, 0, 624, 625, 628, 667, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }, /* 832x624@75Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 49500, 800, 816,
		   896, 1056, 0, 600, 601, 604,  625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 800x600@75Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 50000, 800, 856,
		   976, 1040, 0, 600, 637, 643, 666, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 800x600@72Hz */
	{ DRM_MODE("1152x864", DRM_MODE_TYPE_DRIVER, 108000, 1152, 1216,
		   1344, 1600, 0,  864, 865, 868, 900, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) }, /* 1152x864@75Hz */
};
const int edid_tbl_est_count = ARRAY_SIZE(edid_tbl_est_modes);

const struct minimode edid_tbl_est3_modes[] = {
	/* byte 6 */
	{ 640, 350, 85, 0 },
	{ 640, 400, 85, 0 },
	{ 720, 400, 85, 0 },
	{ 640, 480, 85, 0 },
	{ 848, 480, 60, 0 },
	{ 800, 600, 85, 0 },
	{ 1024, 768, 85, 0 },
	{ 1152, 864, 75, 0 },
	/* byte 7 */
	{ 1280, 768, 60, 1 },
	{ 1280, 768, 60, 0 },
	{ 1280, 768, 75, 0 },
	{ 1280, 768, 85, 0 },
	{ 1280, 960, 60, 0 },
	{ 1280, 960, 85, 0 },
	{ 1280, 1024, 60, 0 },
	{ 1280, 1024, 85, 0 },
	/* byte 8 */
	{ 1360, 768, 60, 0 },
	{ 1440, 900, 60, 1 },
	{ 1440, 900, 60, 0 },
	{ 1440, 900, 75, 0 },
	{ 1440, 900, 85, 0 },
	{ 1400, 1050, 60, 1 },
	{ 1400, 1050, 60, 0 },
	{ 1400, 1050, 75, 0 },
	/* byte 9 */
	{ 1400, 1050, 85, 0 },
	{ 1680, 1050, 60, 1 },
	{ 1680, 1050, 60, 0 },
	{ 1680, 1050, 75, 0 },
	{ 1680, 1050, 85, 0 },
	{ 1600, 1200, 60, 0 },
	{ 1600, 1200, 65, 0 },
	{ 1600, 1200, 70, 0 },
	/* byte 10 */
	{ 1600, 1200, 75, 0 },
	{ 1600, 1200, 85, 0 },
	{ 1792, 1344, 60, 0 },
	{ 1792, 1344, 75, 0 },
	{ 1856, 1392, 60, 0 },
	{ 1856, 1392, 75, 0 },
	{ 1920, 1200, 60, 1 },
	{ 1920, 1200, 60, 0 },
	/* byte 11 */
	{ 1920, 1200, 75, 0 },
	{ 1920, 1200, 85, 0 },
	{ 1920, 1440, 60, 0 },
	{ 1920, 1440, 75, 0 },
};
const int edid_tbl_est3_count = ARRAY_SIZE(edid_tbl_est3_modes);

const struct minimode edid_tbl_extra_modes[] = {
	{ 1024, 576,  60, 0 },
	{ 1366, 768,  60, 0 },
	{ 1600, 900,  60, 0 },
	{ 1680, 945,  60, 0 },
	{ 1920, 1080, 60, 0 },
	{ 2048, 1152, 60, 0 },
	{ 2048, 1536, 60, 0 },
};
const int edid_tbl_extra_count = ARRAY_SIZE(edid_tbl_extra_modes);
