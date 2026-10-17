// LikeOS Intel High Definition Audio - Realtek codec support.
//
// Covers the Realtek HD-audio codec families (vendor 0x10ec, plus the
// Huawei HW8326 clone of the ALC256):
//
//   ALC2xx "269 family"  ALC215 ALC221 ALC222 ALC225 ALC230 ALC231 ALC233
//                        ALC234 ALC235 ALC236 ALC245 ALC255 ALC256 ALC257
//                        ALC269(VA/VB/VC/VD) ALC270 ALC274 ALC275 ALC276
//                        ALC280 ALC282 ALC283 ALC284 ALC285 ALC286 ALC287
//                        ALC288 ALC289 ALC290 ALC292 ALC293 ALC294 ALC295
//                        ALC298 ALC299 ALC300 ALC623 ALC700 ALC701 ALC703
//                        ALC711 and the Dell/HP/Lenovo/ASUS rebrands
//                        (ALC3220, ALC3234, ALC3266, ...)
//   ALC66x "662 family"  ALC272 ALC662 ALC663 ALC665 ALC667 ALC668 ALC670
//                        ALC671 ALC672 ALC676 ALC891 ALC892 ALC897
//   ALC88x "882 family"  ALC880 ALC882 ALC883 ALC885 ALC887 ALC888 ALC889
//                        ALC889A ALC898 ALC1150 ALC1200 ALCS1200A ALC1220
//   older parts          ALC260 ALC262 ALC267 ALC268 ALC660 ALC660-VD
//                        ALC861 ALC861-VD ALC680
//
// The generic parser (hda_generic.c) does the heavy lifting: it finds the
// output and input paths from the pin default configurations.  This file
// adds what the generic code cannot know:
//
//   * chip identification and naming (COEF 0 revision bits, subsystem
//     vendor rebrands),
//   * the per-chip "processing coefficient" programming Realtek parts need
//     after every reset: EAPD under verb control, class-D speaker amp and
//     capless headphone amp defaults, the depop sequences that power the
//     headphone driver up without a click, PLL workarounds,
//   * headset jack handling (combo jacks that carry a microphone: the codec
//     must be told whether a CTIA or an OMTP plug is inserted),
//   * GPIO driven speaker/headphone amplifiers and mute/mic-mute LEDs,
//   * a large table of board fixups, selected by the PCI subsystem id (or the
//     codec's own subsystem id, or the shape of the BIOS pin configuration),
//     that override wrong pin configurations, pin DACs to outputs, enable
//     docking-station jacks and program board-specific coefficients.
//
// Board fixups are "recipes": a set of pin overrides, init verbs, DAC
// preferences, behaviour flags (ALC_OP_*) and LED descriptors.  The quirk
// tables (struct hda_fixup) carry the recipe number in their flags.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/sound/snd.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include "hda.h"

/* ==========================================================================
 * Constants
 * ========================================================================== */

/* Pin widget control values */
#define PINCTL_OFF 0x00
#define PINCTL_IN AC_PINCTL_IN_EN
#define PINCTL_VREFHIZ (AC_PINCTL_IN_EN | AC_PINCTL_VREF_HIZ)
#define PINCTL_VREF50 (AC_PINCTL_IN_EN | AC_PINCTL_VREF_50)
#define PINCTL_VREFGRD (AC_PINCTL_IN_EN | AC_PINCTL_VREF_GRD)
#define PINCTL_VREF80 (AC_PINCTL_IN_EN | AC_PINCTL_VREF_80)
#define PINCTL_VREF100 (AC_PINCTL_IN_EN | AC_PINCTL_VREF_100)
#define PINCTL_OUT AC_PINCTL_OUT_EN
#define PINCTL_HP (AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN)

/* Raw SET_AMP_GAIN_MUTE payloads for both channels */
#define AMP_OUT_MUTE_ALL                                                \
	(AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT | AC_AMP_MUTE)
#define AMP_OUT_UNMUTE_ALL                                              \
	(AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT)
#define AMP_IN_MUTE_IDX(i)                                              \
	(AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT | AC_AMP_MUTE | \
	 (((uint32_t)(i) & 0xf) << AC_AMP_SET_INDEX_SHIFT))
#define AMP_IN_UNMUTE_IDX(i, gain)                                      \
	(AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT |         \
	 (((uint32_t)(i) & 0xf) << AC_AMP_SET_INDEX_SHIFT) |             \
	 ((uint32_t)(gain) & AC_AMP_GAIN))

/* The vendor processing widget that holds the COEF registers */
#define ALC_COEF_NID 0x20
/* Further vendor widgets addressed with the same index/value pair */
#define ALC_COEFEX_HP_AMP 0x57 /* direct-drive headphone amp, charge pump */
#define ALC_COEFEX_DEPOP 0x58 /* headphone depop state machine */
#define ALC_COEFEX_AGC 0x53 /* AGC / beep / misc */
#define ALC_COEFEX_ALC260 0x1a /* ALC260 keeps its COEFs here */
#define ALC_DSP5505_NID 0x51 /* ALC5505 companion DSP, if fitted */

/* "no response" from a verb */
#define ALC_NORESP 0xffffffffu

/* Codec families */
enum {
	ALC_FAM_269 = 1, /* ALC2xx, ALC3xxx rebrands, ALC7xx */
	ALC_FAM_662, /* ALC66x, ALC891/892/897 */
	ALC_FAM_882, /* ALC88x, ALC1150, ALC1220 */
	ALC_FAM_260,
	ALC_FAM_262,
	ALC_FAM_268,
	ALC_FAM_861,
	ALC_FAM_861VD,
	ALC_FAM_680,
};

/* Initialisation classes inside the 269 family.  They decide which reset
 * coefficients, depop sequence and headset COEF layout a chip uses. */
enum {
	ALC_T_GENERIC = 0,
	ALC_T_269VA,
	ALC_T_269VB,
	ALC_T_269VC,
	ALC_T_269VD,
	ALC_T_280, /* ALC280, ALC290 */
	ALC_T_282,
	ALC_T_283, /* ALC233, ALC283 */
	ALC_T_284, /* ALC284, ALC292 */
	ALC_T_293,
	ALC_T_286, /* ALC286, ALC288 */
	ALC_T_298,
	ALC_T_255, /* ALC235, ALC255 */
	ALC_T_256, /* ALC230, ALC236, ALC256, HW8326 */
	ALC_T_257,
	ALC_T_215, /* ALC215, ALC285, ALC289 (and ALC245 by COEF) */
	ALC_T_225, /* ALC225, ALC295, ALC299 */
	ALC_T_245,
	ALC_T_287,
	ALC_T_294, /* ALC234, ALC274, ALC294 */
	ALC_T_300,
	ALC_T_623, /* ALC222, ALC623 */
	ALC_T_700, /* ALC700, ALC701, ALC703, ALC711 */
	ALC_T_221,
	ALC_T_275,
	/* 662 family */
	ALC_T_662,
	ALC_T_668,
	/* 882 family */
	ALC_T_882, /* ALC882, ALC885, ALC1150, ALCS1200A, ALC1220 */
	ALC_T_883, /* ALC883, ALC887, ALC888, ALC889, ALC898 ... */
};

/* How amplifiers are brought up at init */
enum {
	ALC_INIT_UNDEFINED = 0,
	ALC_INIT_NONE,
	ALC_INIT_DEFAULT,
};

/* Headset jack state machine */
enum {
	ALC_HS_MODE_UNKNOWN = 0,
	ALC_HS_MODE_UNPLUGGED,
	ALC_HS_MODE_HEADSET,
	ALC_HS_MODE_HEADPHONE,
};
enum {
	ALC_HS_TYPE_UNKNOWN = 0,
	ALC_HS_TYPE_CTIA, /* tip L, ring1 R, ring2 GND, sleeve MIC */
	ALC_HS_TYPE_OMTP, /* tip L, ring1 R, ring2 MIC, sleeve GND */
};

/* ==========================================================================
 * Chip table
 * ========================================================================== */

struct alc_chip {
	uint32_t vendor_id;
	uint32_t rev; /* AC_PAR_REV_ID to match exactly, 0 = any */
	const char *name;
	uint8_t family;
	uint8_t type;
};

/* Exact revision entries come before the "any revision" entry of the same
 * vendor id: the first match wins. */
static const struct alc_chip alc_chips[] = {
	/* ---- 269 family ---- */
	{ 0x10ec0215, 0, "ALC215", ALC_FAM_269, ALC_T_215 },
	{ 0x10ec0221, 0, "ALC221", ALC_FAM_269, ALC_T_221 },
	{ 0x10ec0222, 0, "ALC222", ALC_FAM_269, ALC_T_623 },
	{ 0x10ec0225, 0, "ALC225", ALC_FAM_269, ALC_T_225 },
	{ 0x10ec0230, 0, "ALC236", ALC_FAM_269, ALC_T_256 },
	{ 0x10ec0231, 0, "ALC231", ALC_FAM_269, ALC_T_GENERIC },
	{ 0x10ec0233, 0, "ALC233", ALC_FAM_269, ALC_T_283 },
	{ 0x10ec0234, 0, "ALC234", ALC_FAM_269, ALC_T_294 },
	{ 0x10ec0235, 0, "ALC233", ALC_FAM_269, ALC_T_255 },
	{ 0x10ec0236, 0, "ALC236", ALC_FAM_269, ALC_T_256 },
	{ 0x10ec0245, 0, "ALC245", ALC_FAM_269, ALC_T_215 },
	{ 0x10ec0255, 0, "ALC255", ALC_FAM_269, ALC_T_255 },
	{ 0x10ec0256, 0, "ALC256", ALC_FAM_269, ALC_T_256 },
	{ 0x10ec0257, 0, "ALC257", ALC_FAM_269, ALC_T_257 },
	{ 0x10ec0269, 0, "ALC269", ALC_FAM_269, ALC_T_269VA },
	{ 0x10ec0270, 0, "ALC270", ALC_FAM_269, ALC_T_GENERIC },
	{ 0x10ec0274, 0, "ALC274", ALC_FAM_269, ALC_T_294 },
	{ 0x10ec0275, 0, "ALC275", ALC_FAM_269, ALC_T_275 },
	{ 0x10ec0276, 0, "ALC276", ALC_FAM_269, ALC_T_GENERIC },
	{ 0x10ec0280, 0, "ALC280", ALC_FAM_269, ALC_T_280 },
	{ 0x10ec0282, 0, "ALC282", ALC_FAM_269, ALC_T_282 },
	{ 0x10ec0283, 0, "ALC283", ALC_FAM_269, ALC_T_283 },
	{ 0x10ec0284, 0, "ALC284", ALC_FAM_269, ALC_T_284 },
	{ 0x10ec0285, 0, "ALC285", ALC_FAM_269, ALC_T_215 },
	{ 0x10ec0286, 0, "ALC286", ALC_FAM_269, ALC_T_286 },
	{ 0x10ec0287, 0, "ALC287", ALC_FAM_269, ALC_T_287 },
	{ 0x10ec0288, 0, "ALC288", ALC_FAM_269, ALC_T_286 },
	{ 0x10ec0289, 0, "ALC289", ALC_FAM_269, ALC_T_215 },
	{ 0x10ec0290, 0, "ALC290", ALC_FAM_269, ALC_T_280 },
	{ 0x10ec0292, 0, "ALC292", ALC_FAM_269, ALC_T_284 },
	{ 0x10ec0293, 0, "ALC293", ALC_FAM_269, ALC_T_293 },
	{ 0x10ec0294, 0, "ALC294", ALC_FAM_269, ALC_T_294 },
	{ 0x10ec0295, 0, "ALC295", ALC_FAM_269, ALC_T_225 },
	{ 0x10ec0298, 0, "ALC298", ALC_FAM_269, ALC_T_298 },
	{ 0x10ec0299, 0, "ALC299", ALC_FAM_269, ALC_T_225 },
	{ 0x10ec0300, 0, "ALC300", ALC_FAM_269, ALC_T_300 },
	{ 0x10ec0623, 0, "ALC623", ALC_FAM_269, ALC_T_623 },
	{ 0x10ec0700, 0, "ALC700", ALC_FAM_269, ALC_T_700 },
	{ 0x10ec0701, 0, "ALC701", ALC_FAM_269, ALC_T_700 },
	{ 0x10ec0703, 0, "ALC703", ALC_FAM_269, ALC_T_700 },
	{ 0x10ec0711, 0, "ALC711", ALC_FAM_269, ALC_T_700 },
	{ 0x19e58326, 0, "HW8326", ALC_FAM_269, ALC_T_256 },
	/* ---- 662 family ---- */
	{ 0x10ec0272, 0, "ALC272", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0662, 0x100101, "ALC662 rev1", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0662, 0x100300, "ALC662 rev3", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0662, 0x100002, "ALC662 rev2", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0662, 0, "ALC662", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0663, 0, "ALC663", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0665, 0, "ALC665", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0667, 0, "ALC667", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0668, 0, "ALC668", ALC_FAM_662, ALC_T_668 },
	{ 0x10ec0670, 0, "ALC670", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0671, 0, "ALC671", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0672, 0, "ALC672", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0676, 0, "ALC676", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0867, 0, "ALC891", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0892, 0, "ALC892", ALC_FAM_662, ALC_T_662 },
	{ 0x10ec0897, 0, "ALC897", ALC_FAM_662, ALC_T_662 },
	/* ---- 882 family ---- */
	{ 0x10ec0880, 0, "ALC880", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0882, 0, "ALC882", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0883, 0, "ALC883", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0885, 0x100101, "ALC889A", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0885, 0x100103, "ALC889A", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0885, 0, "ALC885", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0887, 0, "ALC887", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0888, 0x100101, "ALC1200", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0888, 0, "ALC888", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0889, 0, "ALC889", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0899, 0, "ALC898", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec0900, 0, "ALC1150", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec0b00, 0, "ALCS1200A", ALC_FAM_882, ALC_T_882 },
	{ 0x10ec1168, 0, "ALC1220", ALC_FAM_882, ALC_T_883 },
	{ 0x10ec1220, 0, "ALC1220", ALC_FAM_882, ALC_T_882 },
	/* ---- older parts ---- */
	{ 0x10ec0260, 0, "ALC260", ALC_FAM_260, ALC_T_GENERIC },
	{ 0x10ec0262, 0, "ALC262", ALC_FAM_262, ALC_T_GENERIC },
	{ 0x10ec0267, 0, "ALC267", ALC_FAM_268, ALC_T_GENERIC },
	{ 0x10ec0268, 0, "ALC268", ALC_FAM_268, ALC_T_GENERIC },
	{ 0x10ec0861, 0x100340, "ALC660", ALC_FAM_861, ALC_T_GENERIC },
	{ 0x10ec0861, 0, "ALC861", ALC_FAM_861, ALC_T_GENERIC },
	{ 0x10ec0660, 0, "ALC660-VD", ALC_FAM_861VD, ALC_T_GENERIC },
	{ 0x10ec0862, 0, "ALC861-VD", ALC_FAM_861VD, ALC_T_GENERIC },
	{ 0x10ec0680, 0, "ALC680", ALC_FAM_680, ALC_T_GENERIC },
	{ 0, 0, NULL, 0, 0 },
};

/* Renames by the chip's COEF 0 revision register: the same vendor id is
 * used for several silicon variants. */
struct alc_coef_rename {
	uint32_t vendor_id;
	uint16_t coef_mask, coef_bits;
	const char *name;
};

static const struct alc_coef_rename alc_coef_renames[] = {
	{ 0x10ec0221, 0xf00f, 0x1003, "ALC231" },
	{ 0x10ec0269, 0xfff0, 0x3010, "ALC277" },
	{ 0x10ec0269, 0xf0f0, 0x2010, "ALC259" },
	{ 0x10ec0269, 0xf0f0, 0x3010, "ALC258" },
	{ 0x10ec0269, 0x00f0, 0x0010, "ALC269VB" },
	{ 0x10ec0269, 0xffff, 0xa023, "ALC259" },
	{ 0x10ec0269, 0xffff, 0x6023, "ALC281X" },
	{ 0x10ec0269, 0x00f0, 0x0020, "ALC269VC" },
	{ 0x10ec0269, 0x00f0, 0x0030, "ALC269VD" },
	{ 0x10ec0662, 0xffff, 0x4020, "ALC656" },
	{ 0x10ec0887, 0x00f0, 0x0030, "ALC887-VD" },
	{ 0x10ec0888, 0x00f0, 0x0030, "ALC888-VD" },
	{ 0x10ec0888, 0xf0f0, 0x3020, "ALC886" },
	{ 0x10ec0899, 0x2000, 0x2000, "ALC899" },
	{ 0x10ec0892, 0xffff, 0x8020, "ALC661" },
	{ 0x10ec0892, 0xffff, 0x8011, "ALC661" },
	{ 0x10ec0892, 0xffff, 0x4011, "ALC656" },
	{ 0, 0, 0, NULL },
};

/* OEM part numbers: Dell, HP, Lenovo, ASUS and Acer order the same silicon
 * under their own names.  Matched on the PCI subsystem vendor (and device
 * when non-zero). */
struct alc_oem_rename {
	uint32_t vendor_id;
	uint16_t ssvid, ssdid;
	const char *name;
};

static const struct alc_oem_rename alc_oem_renames[] = {
	{ 0x10ec0280, 0x1028, 0, "ALC3220" },
	{ 0x10ec0282, 0x1028, 0, "ALC3221" },
	{ 0x10ec0283, 0x1028, 0, "ALC3223" },
	{ 0x10ec0288, 0x1028, 0, "ALC3263" },
	{ 0x10ec0292, 0x1028, 0, "ALC3226" },
	{ 0x10ec0293, 0x1028, 0, "ALC3235" },
	{ 0x10ec0255, 0x1028, 0, "ALC3234" },
	{ 0x10ec0668, 0x1028, 0, "ALC3661" },
	{ 0x10ec0275, 0x1028, 0, "ALC3260" },
	{ 0x10ec0899, 0x1028, 0, "ALC3861" },
	{ 0x10ec0298, 0x1028, 0, "ALC3266" },
	{ 0x10ec0236, 0x1028, 0, "ALC3204" },
	{ 0x10ec0256, 0x1028, 0, "ALC3246" },
	{ 0x10ec0225, 0x1028, 0, "ALC3253" },
	{ 0x10ec0295, 0x1028, 0, "ALC3254" },
	{ 0x10ec0299, 0x1028, 0, "ALC3271" },
	{ 0x10ec0670, 0x1025, 0, "ALC669X" },
	{ 0x10ec0676, 0x1025, 0, "ALC679X" },
	{ 0x10ec0282, 0x1043, 0, "ALC3229" },
	{ 0x10ec0233, 0x1043, 0, "ALC3236" },
	{ 0x10ec0280, 0x103c, 0, "ALC3228" },
	{ 0x10ec0282, 0x103c, 0, "ALC3227" },
	{ 0x10ec0286, 0x103c, 0, "ALC3242" },
	{ 0x10ec0290, 0x103c, 0, "ALC3241" },
	{ 0x10ec0668, 0x103c, 0, "ALC3662" },
	{ 0x10ec0283, 0x17aa, 0, "ALC3239" },
	{ 0x10ec0292, 0x17aa, 0, "ALC3232" },
	{ 0x10ec0257, 0x12f0, 0, "ALC3328" },
	{ 0, 0, 0, NULL },
};

/* ==========================================================================
 * Board recipes: types used by the generated tables below
 * ========================================================================== */

/* Behaviour flags of a recipe */
#define ALC_OP_HS_MODE (1ull << 0) /* combo jack: CTIA/OMTP mode switching */
#define ALC_OP_HS_NO_HPMIC (1ull << 1) /* ... without a headphone-mic pin */
#define ALC_OP_HS_MIC (1ull << 2) /* headset mic pin, no mode COEFs */
#define ALC_OP_HS_KEYS (1ull << 3) /* headset button detection COEFs */
#define ALC_OP_HS_REDETECT (1ull << 4) /* re-sense the plug after cold boot */
#define ALC_OP_JACK_CTIA_DEFAULT (1ull << 5) /* ALC255/256: CTIA at reset */
#define ALC_OP_ALC668_HS (1ull << 6)
#define ALC_OP_ALC662_HS (1ull << 7)
#define ALC_OP_SKU_IGNORE (1ull << 8) /* do not trust the BIOS SKU word */
#define ALC_OP_NO_SHUTUP (1ull << 9) /* leave pins alone at shutdown */
#define ALC_OP_NO_AAMIX (1ull << 10) /* analog loopback causes noise */
#define ALC_OP_INT_MIC_BOOST1 (1ull << 11) /* internal mic boost 0/1 only */
#define ALC_OP_NO_HP_FIXUP (1ull << 12) /* keep several HPs as given */
#define ALC_OP_NO_AUTOMUTE (1ull << 13) /* speakers stay on with HP */
#define ALC_OP_DOCK_ENABLE (1ull << 14) /* ThinkPad: unlock dock jacks */
#define ALC_OP_XPS13 (1ull << 15) /* Dell XPS13: mic VREF, HP depop */
#define ALC_OP_MIC_VREF_HIZ (1ull << 16) /* 0x19 starts with VREF Hi-Z */
#define ALC_OP_DELL4_QUIET (1ull << 17) /* 0x19/0x1b Hi-Z against pops */
#define ALC_OP_AMP_COEF65 (1ull << 18) /* HP: speaker amp enable COEF 0x65 */
#define ALC_OP_GPIO0_PULSE (1ull << 19) /* pulse GPIO0 to wake the amp */
#define ALC_OP_GPIO03_PULSE (1ull << 20) /* pulse GPIO0+1 to wake the amp */
#define ALC_OP_SAMSUNG_AMP (1ull << 21) /* Samsung Galaxy Book amps (v1) */
#define ALC_OP_SAMSUNG_AMP2 (1ull << 22) /* ... v2, two amplifiers */
#define ALC_OP_SAMSUNG_AMP4 (1ull << 23) /* ... v2, four amplifiers */
#define ALC_OP_STEREO_DMIC (1ull << 24) /* ALC269 PDM mic as mono */
#define ALC_OP_JD_RESTART (1ull << 25) /* restart combo-jack detection */
#define ALC_OP_NO_INT_MIC (1ull << 26) /* all-in-one: no internal mic */
#define ALC_OP_COMBO_SWAP (1ull << 27) /* mic on ring/sleeve swapped */
#define ALC_OP_ULTRA_LOW_POWER (1ull << 28) /* Chromebooks */
#define ALC_OP_QUANTA_MUTE (1ull << 29) /* toggle COEF 0x0c on automute */
#define ALC_OP_HP_AMP_GPIO4 (1ull << 30) /* GPIO4 powers the HP amp */
#define ALC_OP_CLEVO_P950 (1ull << 31) /* ALC1220 Clevo COEF 0x07 */
#define ALC_OP_GB_X570 (1ull << 32) /* Gigabyte X570 COEF set */
#define ALC_OP_MBP_VREF (1ull << 33) /* Apple: VREF80 on the HP pin */
#define ALC_OP_ALC889_COEF (1ull << 34)
#define ALC_OP_ASUS887_JACK (1ull << 35)
#define ALC_OP_CLEVO_P775 (1ull << 36)
#define ALC_OP_ALC671_HSMIC (1ull << 37)
#define ALC_OP_ALC897_HSMIC (1ull << 38)
#define ALC_OP_ALC897_HSMODE (1ull << 39)
#define ALC_OP_USI_HSMIC (1ull << 40)
#define ALC_OP_X101_HSMIC (1ull << 41)
#define ALC_OP_ALC283_SENSE_COMBO (1ull << 42)
#define ALC_OP_ALC283_CHROMEBOOK (1ull << 43)
#define ALC_OP_DELL_HP_RESUME (1ull << 44)
#define ALC_OP_HWEQ (1ull << 45)
#define ALC_OP_ASPIRE_E1_COEF (1ull << 46)
#define ALC_OP_HADES_CANYON (1ull << 47)
#define ALC_OP_COEF10_BIT9 (1ull << 48) /* keep COEF 0x10 bit 9 clear */
#define ALC_OP_MINISFORUM_JD (1ull << 49)
#define ALC_OP_EXT_AMP (1ull << 50) /* speakers behind an I2C/SPI amp */
#define ALC_OP_INV_DMIC (1ull << 51) /* inverted stereo digital mic */
#define ALC_OP_DELL_ALC288 (1ull << 52) /* GPIO6 enables the headset mic */
#define ALC_OP_NJ51CU (1ull << 53)
#define ALC_OP_ALC233_JACK_COEFS (1ull << 54)
#define ALC_OP_ALC256_COEF_DEFAULTS (1ull << 55)
#define ALC_OP_AMP_GPIO2_PLAYBACK (1ull << 56) /* GPIO2 high while playing */
#define ALC_OP_ENVY_X360 (1ull << 57)
#define ALC_OP_GX502 (1ull << 58)
#define ALC_OP_GU502 (1ull << 59)
#define ALC_OP_HP_AMP_LIMIT (1ull << 60) /* cap the headphone DAC gain */
#define ALC_OP_ALC271_DMIC (1ull << 61)
#define ALC_OP_NO_JACK_DETECT (1ull << 62)

/* A DAC preference: route `pin' from `dac' (pin 0 = every pin).  With
 * ALC_AVOID() the DAC is never used for that pin. */
#define ALC_DAC_AVOID_FLAG 0x80
#define ALC_AVOID(nid) ((nid) | ALC_DAC_AVOID_FLAG)
struct alc_dacpref {
	uint8_t pin;
	uint8_t dac;
};

/* Mute and mic-mute indicator LEDs */
enum {
	ALC_LED_NONE = 0,
	ALC_LED_GPIO_01,
	ALC_LED_GPIO_01_INV,
	ALC_LED_GPIO_02,
	ALC_LED_GPIO_04,
	ALC_LED_GPIO_04_INV,
	ALC_LED_GPIO_08,
	ALC_LED_GPIO_10,
	ALC_LED_GPIO_20,
	ALC_LED_VREF_18,
	ALC_LED_VREF_19,
	ALC_LED_VREF_1A,
	ALC_LED_VREF_1B,
	ALC_LED_VREF100_1A,
	ALC_LED_COEF_285,
	ALC_LED_COEF_236,
	ALC_LED_COEF_236B,
	ALC_LED_COEF_245,
	ALC_LED_COEF_245V1,
	ALC_LED_COEF_245V2,
	ALC_LED_COEF_295,
	ALC_LED_COEF_MIC285,
	ALC_LED_COEF_MIC236,
	ALC_LED_COEF_MIC233,
	ALC_LED_COUNT
};

enum { ALC_LEDK_NONE = 0, ALC_LEDK_GPIO, ALC_LEDK_VREF, ALC_LEDK_VREF100, ALC_LEDK_COEF };

struct alc_led {
	uint8_t kind;
	uint8_t inverted; /* active low */
	uint8_t gpio; /* GPIO mask */
	uint8_t nid; /* VREF pin */
	uint16_t idx, mask, on, off; /* COEF bits */
};

static const struct alc_led alc_leds[ALC_LED_COUNT] = {
	[ALC_LED_NONE] = { ALC_LEDK_NONE, 0, 0, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_01] = { ALC_LEDK_GPIO, 0, 0x01, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_01_INV] = { ALC_LEDK_GPIO, 1, 0x01, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_02] = { ALC_LEDK_GPIO, 0, 0x02, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_04] = { ALC_LEDK_GPIO, 0, 0x04, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_04_INV] = { ALC_LEDK_GPIO, 1, 0x04, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_08] = { ALC_LEDK_GPIO, 0, 0x08, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_10] = { ALC_LEDK_GPIO, 0, 0x10, 0, 0, 0, 0, 0 },
	[ALC_LED_GPIO_20] = { ALC_LEDK_GPIO, 0, 0x20, 0, 0, 0, 0, 0 },
	[ALC_LED_VREF_18] = { ALC_LEDK_VREF, 0, 0, 0x18, 0, 0, 0, 0 },
	[ALC_LED_VREF_19] = { ALC_LEDK_VREF, 0, 0, 0x19, 0, 0, 0, 0 },
	[ALC_LED_VREF_1A] = { ALC_LEDK_VREF, 0, 0, 0x1a, 0, 0, 0, 0 },
	[ALC_LED_VREF_1B] = { ALC_LEDK_VREF, 0, 0, 0x1b, 0, 0, 0, 0 },
	[ALC_LED_VREF100_1A] = { ALC_LEDK_VREF100, 0, 0, 0x1a, 0, 0, 0, 0 },
	[ALC_LED_COEF_285] = { ALC_LEDK_COEF, 0, 0, 0, 0x0b, 1 << 3, 1 << 3, 0 },
	[ALC_LED_COEF_236] = { ALC_LEDK_COEF, 0, 0, 0, 0x34, 1 << 5, 0, 1 << 5 },
	[ALC_LED_COEF_236B] = { ALC_LEDK_COEF, 0, 0, 0, 0x07, 1, 1, 0 },
	[ALC_LED_COEF_245] = { ALC_LEDK_COEF, 0, 0, 0, 0x0b, 3 << 2, 2 << 2,
			       1 << 2 },
	[ALC_LED_COEF_245V1] = { ALC_LEDK_COEF, 0, 0, 0, 0x0b, 3 << 2, 1 << 3,
				 0 },
	[ALC_LED_COEF_245V2] = { ALC_LEDK_COEF, 0, 0, 0, 0x0b, 1 << 3, 1 << 3,
				 0 },
	[ALC_LED_COEF_295] = { ALC_LEDK_COEF, 0, 0, 0, 0x0b, 3 << 3, 1 << 3,
			       1 << 4 },
	[ALC_LED_COEF_MIC285] = { ALC_LEDK_COEF, 0, 0, 0, 0x19, 1 << 13,
				  1 << 13, 0 },
	[ALC_LED_COEF_MIC236] = { ALC_LEDK_COEF, 0, 0, 0, 0x35, 3 << 2, 2 << 2,
				  1 << 2 },
	[ALC_LED_COEF_MIC233] = { ALC_LEDK_COEF, 0, 0, 0, 0x10, 1 << 13, 0,
				  1 << 13 },
};

struct alc_recipe {
	const char *name;
	const struct hda_pincfg *pins; /* before parsing */
	const struct hda_verb *verbs; /* at every init */
	const struct alc_dacpref *dacs; /* after parsing */
	uint64_t ops; /* ALC_OP_* */
	uint8_t mute_led, micmute_led; /* ALC_LED_* */
	uint8_t gpio_mask, gpio_data; /* extra GPIO outputs */
	uint8_t vref50[2]; /* input pins kept at VREF 50% */
};

/* Board matching from the BIOS pin configuration (when the subsystem id is
 * not in the table): codec, subsystem vendor and a set of pin configs. */
struct alc_pin_quirk {
	uint32_t codec_id;
	uint16_t subvendor;
	uint8_t strict; /* every configured pin must be listed */
	uint16_t recipe;
	const char *name;
	const struct hda_pincfg *pins;
};

/* Quirk table entries are struct hda_fixup; the recipe number travels in the
 * upper half of the flags. */
#define ALC_FIX_RECIPE_SHIFT 16
#define ALC_FIX_CODEC_SSID (1u << 15) /* match the codec SSID only */
#define ALC_Q(v, d, n, r)                                                  \
	{ 0, ((uint32_t)(v) << 16) | (d), 0xffffffffu, n, NULL, NULL, NULL,   \
	  (uint32_t)(r) << ALC_FIX_RECIPE_SHIFT }
#define ALC_MQ(v, m, d, n, r)                                              \
	{ 0, ((uint32_t)(v) << 16) | (d), 0xffff0000u | (m), n, NULL, NULL,  \
	  NULL, (uint32_t)(r) << ALC_FIX_RECIPE_SHIFT }
#define ALC_VQ(v, n, r)                                                    \
	{ 0, (uint32_t)(v) << 16, 0xffff0000u, n, NULL, NULL, NULL,         \
	  (uint32_t)(r) << ALC_FIX_RECIPE_SHIFT }
#define ALC_CQ(v, d, n, r)                                                 \
	{ 0, ((uint32_t)(v) << 16) | (d), 0xffffffffu, n, NULL, NULL, NULL,   \
	  ((uint32_t)(r) << ALC_FIX_RECIPE_SHIFT) | ALC_FIX_CODEC_SSID }

/* ==========================================================================
 * Board tables
 *
 * The recipes and quirk lists below were assembled from published codec
 * dumps and board configuration data.  Pin config words follow the HDA
 * default configuration layout (port/location/device/connection/colour/
 * misc/association/sequence).
 * ========================================================================== */

/* Recipe numbers (index into alc_recipes[]). */
enum alc_recipe_id {
	R_NONE = 0,
	R_ALC269_INV_DMIC,
	R_ALC269_ACER_AC700,
	R_ALC269_ASPIRE_HEADSET_MIC,
	R_ALC271_HP_GATE_MIC_JACK,
	R_ALC271_HP_GATE_MIC_JACK_E1_572,
	R_ALC282_ASPIRE_V5_PINS,
	R_ALC269VB_ASPIRE_E1_COEF,
	R_ALC255_ACER_LIMIT_INT_MIC_BOOST,
	R_ALC269_LIFEBOOK,
	R_ALC286_ACER_AIO_MIC_NO_PRESENCE,
	R_ALC269VC_ACER_HEADSET_MIC,
	R_ALC283_CHROME_BOOK,
	R_ALC255_ACER_MIC_NO_PRESENCE,
	R_ALC255_PREDATOR_SUBWOOFER,
	R_ALC299_PREDATOR_SPK,
	R_ALC269VC_ACER_VCOPPERBOX_PINS,
	R_ALC269VC_ACER_MIC_NO_PRESENCE,
	R_ALC256_ACER_HEADSET_MIC,
	R_ALC256_ACER_MIC_NO_PRESENCE,
	R_ALC286_ACER_AIO_HEADSET_MIC,
	R_ALC233_ACER_HEADSET_MIC,
	R_ALC255_ACER_HEADSET_MIC,
	R_ALC255_ACER_HEADPHONE_AND_MIC,
	R_ALC2XX_HEADSET_MIC,
	R_ALC256_ACER_COLDBOOT,
	R_ALC256_ACER_SFG16_MICMUTE_LED,
	R_ALC245_ACER_MICMUTE_LED,
	R_ALC287_PREDATOR_SPK_CS35L41_I2C_2,
	R_ALC269_DELL_M101Z,
	R_ALC292_DELL_E7X,
	R_ALC275_DELL_XPS,
	R_ALC290_SUBWOOFER,
	R_ALC269_DELL1_MIC_NO_PRESENCE,
	R_ALC283_DELL_HP_RESUME,
	R_ALC290_SUBWOOFER_HSJACK,
	R_ALC290_MONO_SPEAKERS_HSJACK,
	R_ALC293_DELL1_MIC_NO_PRESENCE,
	R_ALC288_DELL_XPS_13,
	R_ALC255_DELL1_MIC_NO_PRESENCE,
	R_ALC293_DISABLE_AAMIX_MULTIJACK,
	R_ALC256_DELL_INSPIRON_7559_SUBWOOFER,
	R_ALC255_DELL_SPK_NOISE,
	R_ALC269_NO_SHUTUP,
	R_ALC298_SPK_VOLUME,
	R_ALC295_DISABLE_DAC3,
	R_ALC225_DELL_WYSE_MIC_NO_PRESENCE,
	R_ALC274_DELL_AIO_LINEOUT_VERB,
	R_ALC255_DELL_HEADSET_MIC,
	R_ALC255_DUMMY_LINEOUT_VERB,
	R_ALC269_DELL4_MIC_NO_PRESENCE,
	R_ALC225_DELL_WYSE_AIO_MIC_NO_PRESENCE,
	R_ALC225_DELL1_MIC_NO_PRESENCE,
	R_ALC289_DUAL_SPK,
	R_ALC236_DELL_AIO_HEADSET_MIC,
	R_ALC269_DELL4_MIC_NO_PRESENCE_QUIET,
	R_ALC245_CS35L41_SPI_2,
	R_ALC295_DELL_INSPIRON_TOP_SPEAKERS,
	R_ALC289_DELL_CS35L41_SPI_2,
	R_ALC289_RTK_AMP_DUAL_SPK,
	R_ALC236_DELL_HP_POP_NOISE,
	R_ALC269_HP_MUTE_LED_MIC2,
	R_ALC269_HP_GPIO_LED,
	R_ALC269_LIMIT_INT_MIC_BOOST_MUTE_LED,
	R_ALC269_HP_MUTE_LED_MIC1,
	R_ALC269_HP_GPIO_MIC1_LED,
	R_ALC280_HP_HEADSET_MIC,
	R_ALC269_HP_LINE1_MIC1_LED,
	R_ALC269_HP_DOCK_GPIO_MIC1_LED,
	R_ALC280_HP_GPIO2_MIC_HOTKEY,
	R_ALC286_HP_GPIO_LED,
	R_ALC280_HP_DOCK_PINS,
	R_ALC280_HP_9480M,
	R_ALC221_HP_288PRO_MIC_NO_PRESENCE,
	R_ALC221_HP_MIC_NO_PRESENCE,
	R_ALC256_HP_HEADSET_MIC,
	R_ALC295_HP_X360,
	R_ALC221_HP_FRONT_MIC,
	R_ALC295_HP_PAVILION_X360,
	R_ALC236_HP_MUTE_LED_COEFBIT2,
	R_ALC295_HP_OMEN,
	R_ALC285_HP_SPECTRE_X360,
	R_ALC236_HP_MUTE_LED_MICMUTE_VREF,
	R_ALC285_HP_GPIO_LED,
	R_ALC225_HP_AIO_SPK,
	R_ALC295_HP_MUTE_LED_COEFBIT11,
	R_ALC285_HP_ENVY_X360,
	R_ALC236_HP_MUTE_LED_MICMUTE_GPIO,
	R_ALC285_HP_MUTE_LED,
	R_ALC285_HP_GPIO_AMP_INIT,
	R_ALC236_HP_MUTE_LED,
	R_ALC285_HP_ENVY_13AQ,
	R_ALC274_HP_ENVY_GPIO,
	R_ALC285_HP_SPECTRE_X360_EB1,
	R_ALC285_HP_SPECTRE_X360_DF1,
	R_ALC285_HP_SPECTRE_X360_MUTE_LED,
	R_ALC245_HP_X360_MUTE_LEDS,
	R_ALC287_HP_GPIO_LED,
	R_ALC236_HP_GPIO_LED,
	R_ALC245_HP_X360_AMP,
	R_ALC236_HP_LIMIT_INT_MIC_BOOST,
	R_ALC285_HP_SPEAKERS_MICMUTE_LED,
	R_ALC285_HP_LIMIT_INT_MIC_BOOST,
	R_ALC245_HP_ENVY_X360_MUTE_LED,
	R_ALC245_HP_MUTE_LED_V1_COEFBIT,
	R_ALC245_HP_MUTE_LED_V2_COEFBIT,
	R_ALC245_HP_MUTE_LED_COEFBIT,
	R_ALC245_CS35L41_SPI_2_HP_GPIO_LED,
	R_ALC287_CS35L41_I2C_2_HP_GPIO_LED,
	R_ALC245_HP_SPECTRE_X360_EU0XXX,
	R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS,
	R_ALC274_HP_VERBS,
	R_ALC236_HP_15_FD0XXX,
	R_ALC274_HP_AIO_BIND_DACS,
	R_ALC236_HP_DMIC,
	R_ALC245_HP_ZBOOK_FIREFLY_G12A,
	R_ALC245_CS35L41_I2C_2_MUTE_LED,
	R_ALC245_HP_TAS2781_I2C_MUTE_LED,
	R_ALC245_HP_TAS2781_I2C_MUTE_LED_INVERTED,
	R_ALC285_ASUS_GA403U_HEADSET_MIC,
	R_ALC256_ASUS_MIC_NO_PRESENCE,
	R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1,
	R_ALC256_ASUS_MIC,
	R_ALC282_ASUS_TX300,
	R_ALC269_LIMIT_INT_MIC_BOOST,
	R_ALC294_ASUS_SPK,
	R_ALC287_TAS2781_I2C,
	R_ALC287_TXNW2781_I2C_ASUS,
	R_ALC233_EAPD_COEF_AND_MIC_NO_PRESENCE,
	R_ALC269_ASUS_N7601ZM,
	R_ALC294_ASUS_CS35L41_SPI_2,
	R_ALC269VB_ASUS_MIC_NO_PRESENCE,
	R_ALC269VB_ASUS_ZENBOOK,
	R_ALC285_ASUS_I2C_HEADSET_MIC,
	R_ALC269VB_ASUS_ZENBOOK_UX31A,
	R_ALC285_ASUS_G533Z_PINS,
	R_ALC289_ASUS_ZEPHYRUS_DUAL_SPK,
	R_ALC294_ASUS_DUAL_SPK,
	R_ALC256_ASUS_PM3406CHA,
	R_ALC289_ASUS_GA401,
	R_ALC269_STEREO_DMIC,
	R_ALC295_ASUS_DACS,
	R_ALC294_ASUS_ALLY,
	R_ALC294_ASUS_GX502_PINS,
	R_ALC256_ASUS_HEADSET_MIC,
	R_ALC294_ASUS_HPE,
	R_ALC256_ASUS_HPE,
	R_ALC294_ASUS_I2C_HEADSET_MIC,
	R_ALC269_ASUS_G73JW,
	R_ALC294_ASUS_SPI_HEADSET_MIC,
	R_ALC294_LENOVO_MIC_LOCATION,
	R_ALC294_ASUS_COEF_1B,
	R_ALC285_ASUS_SPI_REAR_SPEAKERS,
	R_ALC294_BASS_SPEAKER_15,
	R_ALC289_ASUS_GA502,
	R_ALC294_ASUS_GU502_PINS,
	R_ALC294_ASUS_G513_PINS,
	R_ALC287_ASUS_ALLY_X,
	R_ALC285_ASUS_I2C_SPEAKER2_TO_DAC1,
	R_ALC256_ASUS_AIO_GPIO2,
	R_ALC274_ASUS_ZEN_AIO_27,
	R_ALC269_ASUS_X101,
	R_ALC275_SONY_VAIO_GPIO2,
	R_ALC275_SONY_HWEQ,
	R_ALC275_SONY_DISABLE_AAMIX,
	R_ALC269_LIFEBOOK_NO_HP_TO_LINEOUT,
	R_ALC269_LIFEBOOK_HP_PIN,
	R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC,
	R_ALC269_LIFEBOOK_EXTMIC,
	R_ALC700_INTEL_REFERENCE,
	R_ALC290_MONO_SPEAKERS,
	R_ALC256_MEDION_HEADSET_NO_PRESENCE,
	R_ALC295_CHROME_BOOK,
	R_ALC288_SURFACE_SWAP_DACS,
	R_ALC298_SAMSUNG_AMP,
	R_ALC298_SAMSUNG_AMP_V2_2_AMPS,
	R_ALC298_SAMSUNG_HEADPHONE_VERY_QUIET,
	R_ALC256_SAMSUNG_HEADPHONE_VERY_QUIET,
	R_ALC298_SAMSUNG_AMP_V2_4_AMPS,
	R_ALC283_HEADSET_MIC,
	R_ALC256_SYSTEM76_MIC_NO_PRESENCE,
	R_ALC293_SYSTEM76_MIC_NO_PRESENCE,
	R_ALC245_CLEVO_NOISY_MIC,
	R_ALC269_HEADSET_MIC,
	R_ALC269_DMIC,
	R_ALC256_MIC_NO_PRESENCE_AND_RESUME,
	R_ALC269_VAIO_VJFH52_MIC_NO_PRESENCE,
	R_ALC256_VAIO_RPL_MIC_NO_PRESENCE,
	R_ALC233_LENOVO_MULTI_CODECS,
	R_ALC623_LENOVO_THINKSTATION_P340,
	R_ALC269_SKU_IGNORE,
	R_ALC269_LENOVO_DOCK,
	R_ALC269_LENOVO_DOCK_LIMIT_BOOST,
	R_ALC292_TPT440,
	R_ALC292_TPT440_DOCK,
	R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST,
	R_ALC298_TPT470_DOCK,
	R_ALC292_TPT460,
	R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK,
	R_ALC285_THINKPAD_HEADSET_JACK,
	R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD,
	R_ALC287_LENOVO_THKPAD_WH_ALC1318,
	R_ALC233_LENOVO_GPIO2_MIC_HOTKEY,
	R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN,
	R_ALC287_YOGA7_14ITL_SPEAKERS,
	R_ALC287_LEGION_15IMHG05_SPEAKERS,
	R_ALC298_LENOVO_C940_DUET7,
	R_ALC287_13S_GEN2_SPEAKERS,
	R_ALC285_LEGION_Y9000X_SPEAKERS,
	R_ALC287_YOGA9_14IMH9_BASS_SPK_PIN,
	R_ALC285_YOGA_SPEAKER2_TO_DAC1,
	R_ALC287_LENOVO_YOGA_PRO7,
	R_ALC269_DMIC_THINKPAD_ACPI,
	R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN_HEADSET,
	R_ALC236_LENOVO_INV_DMIC,
	R_ALC287_YOGA9_SPEAKER2_TO_DAC1,
	R_ALC287_LENOVO_XPAD_HEADSET_JACK,
	R_ALC283_INT_MIC,
	R_ALC269_PCM_44K,
	R_ALC293_LENOVO_SPK_NOISE,
	R_ALC269_LENOVO_EAPD,
	R_ALC233_NO_AUDIO_JACK,
	R_ALC256_HEADPHONE_AMP_VOL,
	R_ALC256_HUAWEI_MACH_WX9_PINS,
	R_ALC298_RAZER_BLADE16_2025_PINS,
	R_ALC269_CZC_B20,
	R_ALC269_CZC_TMI,
	R_ALC269_CZC_L101,
	R_ALC269VB_ORDISSIMO_EVE2,
	R_ALC269_LEMOTE_A1802,
	R_ALC269_LEMOTE_A190X,
	R_ALC288_DELL1_MIC_NO_PRESENCE,
	R_ALC256_SET_COEF_DEFAULTS,
	R_ALC255_XIAOMI_HEADSET_MIC,
	R_ALC256_XIAOMI_PRO15_RESUME,
	R_ALC233_MEDION_MTL_SPK,
	R_ALC233_WUJIE_SPEAKERS,
	R_ALC256_HONOR_MRB_XXX_M1020_AUDIO,
	R_ALC245_MINISFORUM_JACK_DETECT,
	R_ALC245_BASS_HP_DAC,
	R_ALC233_STARLABS_STARFIGHTER,
	R_ALC269VB_INFINIX_ZERO_BOOK_13,
	R_ALC269VB_CHUWI_COREBOOK_XPRO,
	R_ALC269VB_INFINIX_INBOOK_X3_SLIM,
	R_ALC269_POSITIVO_P15X_HEADSET_MIC,
	R_ALC269VC_INFINIX_Y4_MAX,
	R_ALC700_INTEL_HADES_CANYON,
	R_ALC233_INTEL_NUC8_DMIC,
	R_ALC256_INTEL_NUC8_RUGGED,
	R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE,
	R_ALC295_FRAMEWORK_LAPTOP_LIMIT_INT_MIC_BOOST,
	R_ALC269_AMIC,
	R_ALC271_DMIC,
	R_ALC269_SONY_VAIO,
	R_ALC221_HP_HEADSET_MIC,
	R_ALC255_DELL2_MIC_NO_PRESENCE,
	R_ALC274_HP_HEADSET_MIC,
	R_ALC280_HP_GPIO4,
	R_ALC282_ACER_DISABLE_LINEOUT,
	R_ALC285_LENOVO_PC_BEEP_IN_NOISE,
	R_ALC287_THINKPAD_I2S_SPK,
	R_ALC269_DELL2_MIC_NO_PRESENCE,
	R_ALC294_ASUS_MIC,
	R_ALC298_ALIENWARE_MIC_NO_PRESENCE,
	R_ALC255_DELL1_LIMIT_INT_MIC_BOOST,
	R_ALC269_DELL1_LIMIT_INT_MIC_BOOST,
	R_ALC662_ASUS_MODE2,
	R_ALC897_HEADSET_MIC_PIN3,
	R_ALC662_CSL_GPIO,
	R_ALC662_ASPIRE,
	R_ALC669_ACER_ASPIRE_ETHOS,
	R_ALC662_ACER_NITRO_HEADSET_MODE,
	R_ALC662_ACER_X2660G_HEADSET_MODE,
	R_ALC668_DELL_MIC_NO_PRESENCE,
	R_ALC668_DELL_XPS13,
	R_ALC662_HP_RP5800,
	R_ALC897_HP_HSMIC_VERB,
	R_ALC671_HP_HEADSET_MIC2,
	R_ALC668_HEADSET_MODE,
	R_ALC662_ASUS_Nx50,
	R_ALC668_ASUS_G751,
	R_ALC662_BASS_MODE4_CHMAP,
	R_ALC662_BASS_16,
	R_ALC668_ASUS_Nx51,
	R_ALC668_ASUS_NO_HEADSET_MIC,
	R_ALC662_ASUS_MODE8,
	R_ALC662_NO_JACK_DETECT,
	R_ALC662_IDEAPAD,
	R_ALC662_USI_HEADSET_MODE,
	R_ALC897_HEADSET_MIC_PIN,
	R_ALC897_HEADSET_MIC_PIN2,
	R_ALC892_ASROCK_MOBO,
	R_ALC662_ZOTAC_Z68,
	R_ALC662_ACER_VERITON,
	R_ALC662_CZC_ET26,
	R_ALC662_ASUS_MODE1,
	R_ALC662_ASUS_MODE3,
	R_ALC662_ASUS_MODE7,
	R_ALC662_ASUS_MODE6,
	R_ALC662_ASUS_MODE5,
	R_ALC891_DELL_MIC_NO_PRESENCE,
	R_ALC662_DELL_MIC_NO_PRESENCE,
	R_ALC883_ACER_EAPD,
	R_ALC882_ACER_ASPIRE_4930G,
	R_ALC882_ACER_ASPIRE_8930G,
	R_ALC882_PB_M5210,
	R_ALC882_EAPD,
	R_ALC882_ASUS_W90V,
	R_ALC882_ASUS_W2JC,
	R_ALC887_ASUS_HMIC,
	R_ALC888_EEE1601,
	R_ALC887_ASUS_BASS,
	R_ALC882_GPIO3,
	R_ALCS1200A_MIC_VREF,
	R_ALC889_VAIO_TT,
	R_ALC889_MBP_VREF,
	R_ALC889_MP11_VREF,
	R_ALC889_MBA11_VREF,
	R_ALC889_MBA21_VREF,
	R_ALC889_IMAC91_VREF,
	R_ALC889_MP41_VREF,
	R_ALC888VD_MIC_100VREF,
	R_ALC886_EAPD,
	R_ALC889_FRONT_HP_NO_PRESENCE,
	R_ALC1220_GB_X570,
	R_ALC1220_CLEVO_P950,
	R_ALC889_CD,
	R_ALC882_ABIT_AW9D_MAX,
	R_ALC1220_CLEVO_PB51ED_PINS,
	R_ALC898_CLEVO_P775TM1,
	R_ALC1220_CLEVO_PB51ED,
	R_ALC883_EAPD,
	R_ALC882_LENOVO_Y530,
	R_ALC889_COEF,
	R_COUNT
};

static const struct hda_pincfg alc_pins_0[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x18, 0x03a11c20 }, { 0x1e, 0x0346101e }, { 0x21, 0x0321101f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_1[] = {
	{ 0x19, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_2[] = {
	{ 0x14, 0x99130110 }, { 0x19, 0x01a19c20 }, { 0x1b, 0x99a7012f }, { 0x21, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_3[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x17, 0x40000008 }, { 0x18, 0x411111f0 }, { 0x19, 0x01a1913c }, { 0x1a, 0x411111f0 }, { 0x1b, 0x411111f0 }, { 0x1d, 0x40f89b2d }, { 0x1e, 0x411111f0 }, { 0x21, 0x0321101f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_4[] = {
	{ 0x1a, 0x2101103f }, { 0x1b, 0x23a11040 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_5[] = {
	{ 0x18, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_6[] = {
	{ 0x18, 0x02a11030 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_0[] = {
	{ 0x00, ALC_AVOID(0x03) },
	{ 0 }
};

static const struct hda_pincfg alc_pins_7[] = {
	{ 0x17, 0x90170151 }, { 0x1b, 0x90170152 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_8[] = {
	{ 0x21, 0x90170150 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_9[] = {
	{ 0x14, 0x90100120 }, { 0x18, 0x02a111f0 }, { 0x1a, 0x01011020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_10[] = {
	{ 0x18, 0x01a11130 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_11[] = {
	{ 0x19, 0x02a1113c }, { 0x1a, 0x90a1092f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_12[] = {
	{ 0x19, 0x02a11120 },
	{ 0 }
};

static const struct hda_verb alc_verbs_0[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x004f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5029 },
	{ 0 }
};

static const struct hda_verb alc_verbs_1[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0045 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5089 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_13[] = {
	{ 0x19, 0x03a11130 }, { 0x1a, 0x90a60140 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_14[] = {
	{ 0x21, 0x03211030 }, { 0x19, 0x03a11020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_15[] = {
	{ 0x19, 0x03a1103c },
	{ 0 }
};

static const struct hda_verb alc_verbs_2[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000d },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x4040 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_16[] = {
	{ 0x16, 0x21014020 }, { 0x19, 0x21a19030 }, { 0x1a, 0x01a1913c },
	{ 0 }
};

static const struct hda_verb alc_verbs_3[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x001f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x00c0 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0030 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x00b1 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_17[] = {
	{ 0x17, 0x90170112 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_1[] = {
	{ 0x14, 0x02 }, { 0x15, 0x02 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_18[] = {
	{ 0x19, 0x01a1913c }, { 0x1a, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_19[] = {
	{ 0x17, 0x90170112 }, { 0x1a, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_20[] = {
	{ 0x1a, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_21[] = {
	{ 0x18, 0x01a1913d }, { 0x1a, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_22[] = {
	{ 0x18, 0x01a1913c }, { 0x1a, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_23[] = {
	{ 0x1b, 0x90170151 }, { 0x19, 0x01a1913c }, { 0x1a, 0x01a1913d },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_2[] = {
	{ 0x17, 0x02 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_3[] = {
	{ 0x17, ALC_AVOID(0x06) },
	{ 0 }
};

static const struct hda_pincfg alc_pins_24[] = {
	{ 0x16, 0x0101102f }, { 0x19, 0x02a1913c }, { 0x1a, 0x01a19030 }, { 0x1b, 0x02011020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_25[] = {
	{ 0x1b, 0x0401102f }, { 0x19, 0x01a1913c }, { 0x1a, 0x01a1913d },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_4[] = {
	{ 0x21, 0x03 }, { 0x1b, 0x03 }, { 0x16, 0x02 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_26[] = {
	{ 0x14, 0x0201101f }, { 0x19, 0x01a1913c }, { 0x1a, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_27[] = {
	{ 0x19, 0x01a1913c }, { 0x1b, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_28[] = {
	{ 0x16, 0x01011020 }, { 0x19, 0x01a1913c },
	{ 0 }
};

static const struct hda_verb alc_verbs_4[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0036 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x57d7 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_29[] = {
	{ 0x17, 0x90170130 }, { 0x19, 0x01a1913c }, { 0x1b, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_30[] = {
	{ 0x14, 0x90170151 }, { 0x17, 0x90170150 }, { 0x19, 0x01a1913c }, { 0x1b, 0x01a1913d },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_5[] = {
	{ 0x14, 0x02 }, { 0x17, 0x03 }, { 0x21, 0x02 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_31[] = {
	{ 0x14, 0x90170140 }, { 0x19, 0x01a1913c }, { 0x1b, 0x01a1913d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_32[] = {
	{ 0x1b, 0x21011020 }, { 0x18, 0x2181103f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_33[] = {
	{ 0x1b, 0x21011020 }, { 0x1a, 0x01a1903c }, { 0x18, 0x2181103f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_34[] = {
	{ 0x19, 0x01a1913c }, { 0x1a, 0x01813030 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_35[] = {
	{ 0x19, 0x02a19020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_5[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7770 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000d },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x3000 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_36[] = {
	{ 0x12, 0xb7a60130 }, { 0x13, 0x40000000 }, { 0x14, 0x411111f0 }, { 0x16, 0x411111f0 }, { 0x17, 0x90170110 }, { 0x18, 0x411111f0 }, { 0x19, 0x02a11030 }, { 0x1a, 0x411111f0 }, { 0x1b, 0x04a19030 }, { 0x1d, 0x40600001 }, { 0x1e, 0x411111f0 }, { 0x21, 0x03211020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_37[] = {
	{ 0x14, 0x90170110 },
	{ 0 }
};

static const struct hda_verb alc_verbs_6[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0220 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_38[] = {
	{ 0x12, 0xb7a60130 }, { 0x14, 0x90170150 }, { 0x17, 0x90170153 }, { 0x19, 0x03a11040 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_39[] = {
	{ 0x14, 0x90170150 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_40[] = {
	{ 0x14, 0x90170110 }, { 0x17, 0x90170130 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_6[] = {
	{ 0x14, 0x02 }, { 0x17, 0x02 },
	{ 0 }
};

static const struct hda_verb alc_verbs_7[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0019 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x8e11 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_41[] = {
	{ 0x14, 0x90170110 }, { 0x17, 0x90170111 },
	{ 0 }
};

static const struct hda_verb alc_verbs_8[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7778 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xc580 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5757 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0062 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xa007 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x006b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0060 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_7[] = {
	{ 0x16, 0x02 }, { 0x17, 0x02 }, { 0x21, 0x03 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_42[] = {
	{ 0x12, 0x90a60160 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_43[] = {
	{ 0x19, 0x03a11050 }, { 0x1b, 0x03a11c30 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_44[] = {
	{ 0x19, 0x04a11120 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_45[] = {
	{ 0x13, 0x90a60160 }, { 0x19, 0x04a11120 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_46[] = {
	{ 0x1b, 0x21114000 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_47[] = {
	{ 0x19, 0x01a1103c },
	{ 0 }
};

static const struct hda_verb alc_verbs_9[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0040 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x8800 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7774 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_8[] = {
	{ 0x14, 0x02 }, { 0x17, 0x03 }, { 0x21, 0x03 },
	{ 0 }
};

static const struct hda_verb alc_verbs_10[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0040 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x8800 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_48[] = {
	{ 0x19, 0x03a11050 }, { 0x1a, 0x03a11c30 }, { 0x21, 0x03211420 },
	{ 0 }
};

static const struct hda_verb alc_verbs_11[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0062 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xa007 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x8420 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7774 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_49[] = {
	{ 0x18, 0x01a110f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_50[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x18, 0x01a19c20 }, { 0x21, 0x0121401f },
	{ 0 }
};

static const struct hda_verb alc_verbs_12[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0012 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x2800 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_51[] = {
	{ 0x14, 0x90170152 }, { 0x19, 0x03a19020 }, { 0x1a, 0x03a11c30 }, { 0x1e, 0x90170151 }, { 0x21, 0x03211420 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_52[] = {
	{ 0x17, 0x90170151 }, { 0x1e, 0x90170150 },
	{ 0 }
};

static const struct hda_verb alc_verbs_13[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7f20 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_53[] = {
	{ 0x19, 0x03a11020 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_9[] = {
	{ 0x14, 0x02 }, { 0x17, 0x02 }, { 0x21, 0x03 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_10[] = {
	{ 0x17, 0x02 }, { 0x21, 0x03 },
	{ 0 }
};

static const struct hda_verb alc_verbs_14[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0045 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5089 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0046 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0004 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0047 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xa47a },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0049 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0049 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x004a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x201b },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x006b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x4278 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_54[] = {
	{ 0x19, 0x03a11050 }, { 0x1a, 0x01a11830 }, { 0x21, 0x03211020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_15[] = {
	{ 0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00c0 },
	{ 0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000 },
	{ 0 }
};

static const struct hda_verb alc_verbs_16[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7774 },
	{ 0 }
};

static const struct hda_verb alc_verbs_17[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x7778 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_55[] = {
	{ 0x19, 0x03a19020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_56[] = {
	{ 0x17, 0x99130111 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_57[] = {
	{ 0x19, 0x04a11020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_58[] = {
	{ 0x1a, 0x04a19040 },
	{ 0 }
};

static const struct hda_verb alc_verbs_18[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x001b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x4e4b },
	{ 0 }
};

static const struct hda_pincfg alc_pins_59[] = {
	{ 0x14, 0x90170120 }, { 0x19, 0x03a11050 }, { 0x1b, 0x03a11c30 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_11[] = {
	{ 0x15, ALC_AVOID(0x06) },
	{ 0 }
};

static const struct hda_pincfg alc_pins_60[] = {
	{ 0x19, 0x01a11050 }, { 0x1a, 0x01a11830 }, { 0x21, 0x012110f0 },
	{ 0 }
};

static const struct hda_verb alc_verbs_19[] = {
	{ 0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00c0 },
	{ 0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000 },
	{ 0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0024 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_61[] = {
	{ 0x19, 0x03a11050 },
	{ 0 }
};

static const struct hda_verb alc_verbs_20[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xc420 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0040 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x8800 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0049 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0249 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x004a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x202b },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0062 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xa007 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x006b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5060 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_62[] = {
	{ 0x18, 0x04a1182c },
	{ 0 }
};

static const struct hda_verb alc_verbs_21[] = {
	{ 0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0000 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0008 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0310 },
	{ 0 }
};

static const struct hda_verb alc_verbs_22[] = {
	{ 0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0022 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_63[] = {
	{ 0x21, 0x0221102f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_64[] = {
	{ 0x19, 0x01a1903c },
	{ 0 }
};

static const struct hda_verb alc_verbs_23[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0045 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x5289 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x004a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x001b },
	{ 0x58, AC_VERB_SET_COEF_INDEX, 0x0000 },
	{ 0x58, AC_VERB_SET_PROC_COEF, 0x3888 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x006f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x2c0b },
	{ 0 }
};

static const struct hda_pincfg alc_pins_65[] = {
	{ 0x19, 0x04a11040 }, { 0x21, 0x04211020 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_12[] = {
	{ 0x21, 0x03 }, { 0x14, 0x02 },
	{ 0 }
};

static const struct hda_verb alc_verbs_24[] = {
	{ 0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00c5 },
	{ 0 }
};

static const struct hda_verb alc_verbs_25[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0008 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x2fcf },
	{ 0 }
};

static const struct hda_pincfg alc_pins_66[] = {
	{ 0x19, 0x04a110f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_67[] = {
	{ 0x19, 0x03a11120 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_68[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x18, 0x01a19c20 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_69[] = {
	{ 0x19, 0x03a1113c }, { 0x1b, 0x20a11040 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_70[] = {
	{ 0x19, 0x03a1113c }, { 0x1a, 0x22a190a0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_71[] = {
	{ 0x19, 0x23a11040 }, { 0x1b, 0x2121103f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_72[] = {
	{ 0x16, 0x21211010 }, { 0x19, 0x21a11010 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_73[] = {
	{ 0x17, 0x21211010 }, { 0x19, 0x21a11010 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_13[] = {
	{ 0x14, 0x03 }, { 0x17, 0x02 }, { 0x21, 0x02 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_14[] = {
	{ 0x17, 0x02 }, { 0x21, 0x03 }, { 0x17, ALC_AVOID(0x06) },
	{ 0 }
};

static const struct hda_pincfg alc_pins_74[] = {
	{ 0x17, 0x90170121 },
	{ 0 }
};

static const struct hda_verb alc_verbs_26[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0041 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x001a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0042 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0040 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0046 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x002a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000f },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0046 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0010 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0044 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_27[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0041 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x001a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0046 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x002a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_28[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0041 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x001a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0042 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x000c },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x002a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_29[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0041 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0024 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0042 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0026 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0002 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0xb020 },
	{ 0 }
};

static const struct hda_verb alc_verbs_30[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x001a },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0011 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_15[] = {
	{ 0x17, 0x02 }, { 0x14, 0x02 }, { 0x17, 0x03 }, { 0x21, 0x03 },
	{ 0 }
};

static const struct hda_verb alc_verbs_31[] = {
	{ 0x14, AC_VERB_SET_EAPD_BTLENABLE, 0x0000 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_75[] = {
	{ 0x12, 0x90a60130 }, { 0x13, 0x40000000 }, { 0x14, 0x90170110 }, { 0x18, 0x411111f0 }, { 0x19, 0x04a11040 }, { 0x1a, 0x411111f0 }, { 0x1b, 0x90170112 }, { 0x1d, 0x40759a05 }, { 0x1e, 0x411111f0 }, { 0x21, 0x04211020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_76[] = {
	{ 0x14, 0x90170121 }, { 0x17, 0x90170120 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_77[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x17, 0x411111f0 }, { 0x18, 0x01a19c20 }, { 0x19, 0xb7a7013f }, { 0x1a, 0x0181305f }, { 0x1b, 0x411111f0 }, { 0x1d, 0x411111f0 }, { 0x1e, 0x411111f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_78[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x17, 0x411111f0 }, { 0x18, 0x01a19c20 }, { 0x19, 0x411111f0 }, { 0x1a, 0x411111f0 }, { 0x1b, 0x411111f0 }, { 0x1d, 0x40448505 }, { 0x1e, 0x411111f0 }, { 0x20, 0x8000ffff },
	{ 0 }
};

static const struct hda_pincfg alc_pins_79[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x16, 0x411111f0 }, { 0x18, 0x01a19c20 }, { 0x19, 0x02a19021 }, { 0x1a, 0x0181302f }, { 0x1b, 0x0221401f }, { 0x1c, 0x411111f0 }, { 0x1d, 0x4044c601 }, { 0x1e, 0x411111f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_80[] = {
	{ 0x12, 0x99a3092f }, { 0x18, 0x03a11d20 }, { 0x19, 0x411111f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_81[] = {
	{ 0x12, 0x99a3092f }, { 0x14, 0x99130110 }, { 0x17, 0x411111f0 }, { 0x18, 0x01a19c20 }, { 0x19, 0x90a70130 }, { 0x1a, 0x411111f0 }, { 0x1b, 0x411111f0 }, { 0x1d, 0x40489d2d }, { 0x1e, 0x411111f0 }, { 0x20, 0x0003ffff }, { 0x21, 0x03214020 }, { 0x15, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_82[] = {
	{ 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x18, 0x01a19c20 }, { 0x19, 0x99a3092f }, { 0x1b, 0x0201401f }, { 0x12, 0x99a3092f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_83[] = {
	{ 0x1b, 0x90170110 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_84[] = {
	{ 0x1b, 0x90170150 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_85[] = {
	{ 0x14, 0x90170111 }, { 0x19, 0x03a1113c }, { 0x1a, 0x22a190a0 }, { 0x1b, 0x90170110 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_86[] = {
	{ 0x14, 0x90170151 }, { 0x1b, 0x90170152 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_87[] = {
	{ 0x18, 0x03a19020 }, { 0x1b, 0x90170150 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_88[] = {
	{ 0x14, 0x411111f0 }, { 0x1b, 0x90170110 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_89[] = {
	{ 0x1b, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_90[] = {
	{ 0x19, 0x02a1112c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_91[] = {
	{ 0x14, 0x99130110 }, { 0x15, 0x0121401f }, { 0x18, 0x01a19c20 }, { 0x19, 0x99a3092f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_92[] = {
	{ 0x19, 0x0181313f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_93[] = {
	{ 0x1b, 0x411111f0 }, { 0x18, 0x01a1913c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_94[] = {
	{ 0x14, 0x99130110 }, { 0x18, 0x01a19820 }, { 0x19, 0x99a3092f }, { 0x1b, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_95[] = {
	{ 0x15, 0x99130112 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_96[] = {
	{ 0x15, 0x92130110 }, { 0x18, 0x99130111 }, { 0x1b, 0x11130012 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_97[] = {
	{ 0x1a, 0x01a11140 }, { 0x1b, 0x0221144f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_98[] = {
	{ 0x1a, 0x02a1113c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_99[] = {
	{ 0x19, 0x03a1913d }, { 0x1b, 0x03a1113c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_100[] = {
	{ 0x14, 0x0221201f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_101[] = {
	{ 0x19, 0x02a11040 }, { 0x1b, 0x0181304f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_102[] = {
	{ 0x1a, 0x80106111 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_103[] = {
	{ 0x16, 0x0421101f },
	{ 0 }
};

static const struct hda_verb alc_verbs_32[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x00c3 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x4000 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_104[] = {
	{ 0x14, 0x99130110 }, { 0x16, 0x99130111 }, { 0x18, 0x01a19840 }, { 0x19, 0x99a3094f }, { 0x21, 0x0121441f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_105[] = {
	{ 0x16, 0x80106111 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_106[] = {
	{ 0x19, 0x03a1913d }, { 0x1a, 0x90170151 }, { 0x1b, 0x03a1113c },
	{ 0 }
};

static const struct hda_pincfg alc_pins_107[] = {
	{ 0x1b, 0x04a1112c },
	{ 0 }
};

static const struct hda_verb alc_verbs_33[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0015 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0d60 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_108[] = {
	{ 0x14, 0x99130110 }, { 0x12, 0x99a30970 }, { 0x15, 0x01214020 }, { 0x17, 0x99130111 }, { 0x18, 0x01a19840 }, { 0x21, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_109[] = {
	{ 0x17, 0x99130112 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_110[] = {
	{ 0x19, 0x02a1913c }, { 0x18, 0x01a1903d },
	{ 0 }
};

static const struct hda_pincfg alc_pins_111[] = {
	{ 0x1a, 0x03a11050 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_112[] = {
	{ 0x1a, 0x01a11140 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_113[] = {
	{ 0x15, 0x40f000f0 }, { 0x16, 0x40f000f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_114[] = {
	{ 0x1b, 0x02214020 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_115[] = {
	{ 0x15, 0x50170120 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_116[] = {
	{ 0x12, 0x403cc000 }, { 0x14, 0x90170110 }, { 0x15, 0x411111f0 }, { 0x16, 0x411111f0 }, { 0x18, 0x01a19030 }, { 0x19, 0x90a7013f }, { 0x1a, 0x01014020 }, { 0x1b, 0x0121401f }, { 0x1c, 0x411111f0 }, { 0x1d, 0x411111f0 }, { 0x1e, 0x40478e35 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_117[] = {
	{ 0x14, 0x99130110 }, { 0x18, 0x01a19c20 }, { 0x19, 0x99a3092f }, { 0x21, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_118[] = {
	{ 0x14, 0x99130110 }, { 0x15, 0x0121441f }, { 0x18, 0x01a19840 }, { 0x19, 0x99a3094f }, { 0x21, 0x01211420 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_119[] = {
	{ 0x14, 0x99130110 }, { 0x17, 0x99130111 }, { 0x18, 0x01a19840 }, { 0x19, 0x99a3094f }, { 0x1b, 0x01214020 }, { 0x21, 0x0121401f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_120[] = {
	{ 0x14, 0x99130110 }, { 0x15, 0x01211420 }, { 0x18, 0x01a19840 }, { 0x19, 0x99a3094f }, { 0x1b, 0x0121441f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_121[] = {
	{ 0x14, 0x99130110 }, { 0x15, 0x0121441f }, { 0x16, 0x99130111 }, { 0x18, 0x01a19840 }, { 0x19, 0x99a3094f },
	{ 0 }
};

static const struct hda_pincfg alc_pins_122[] = {
	{ 0x19, 0x03a1113c },
	{ 0 }
};

static const struct hda_verb alc_verbs_34[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x3050 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_123[] = {
	{ 0x16, 0x99130111 }, { 0x17, 0x99130112 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_124[] = {
	{ 0x16, 0x99130111 }, { 0x1b, 0x99130112 },
	{ 0 }
};

static const struct hda_verb alc_verbs_35[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0003 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0008 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0003 },
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x3050 },
	{ 0 }
};

static const struct hda_verb alc_verbs_36[] = {
	{ 0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0021 },
	{ 0 }
};

static const struct hda_verb alc_verbs_37[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x3060 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_125[] = {
	{ 0x16, 0x99130110 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_126[] = {
	{ 0x15, 0x02a14150 }, { 0x19, 0x22219420 },
	{ 0 }
};

static const struct hda_verb alc_verbs_38[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x000b },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0838 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_127[] = {
	{ 0x16, 0x99130130 },
	{ 0 }
};

static const struct hda_verb alc_verbs_39[] = {
	{ 0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0021 },
	{ 0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0021 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_128[] = {
	{ 0x17, 0x90170111 },
	{ 0 }
};

static const struct hda_verb alc_verbs_40[] = {
	{ 0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x0025 },
	{ 0 }
};

static const struct hda_verb alc_verbs_41[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x0068 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_129[] = {
	{ 0x1b, 0x02214120 }, { 0x1c, 0x993301f0 },
	{ 0 }
};

static const struct alc_dacpref alc_dacs_16[] = {
	{ 0x14, 0x02 }, { 0x1b, 0x02 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_130[] = {
	{ 0x1c, 0x993301f0 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_131[] = {
	{ 0x15, 0x01080104 }, { 0x16, 0x01011012 }, { 0x17, 0x01016011 },
	{ 0 }
};

static const struct hda_verb alc_verbs_42[] = {
	{ 0x20, AC_VERB_SET_COEF_INDEX, 0x0007 },
	{ 0x20, AC_VERB_SET_PROC_COEF, 0x3070 },
	{ 0 }
};

static const struct hda_pincfg alc_pins_132[] = {
	{ 0x15, 0x99130112 }, { 0x16, 0x99130111 },
	{ 0 }
};

/* The recipes: pins, init verbs, DAC preferences, flags, LEDs, GPIOs. */
static const struct alc_recipe alc_recipes[R_COUNT] = {
	[R_NONE] = { "none", NULL, NULL, NULL, 0, ALC_LED_NONE, ALC_LED_NONE, 0, 0, { 0 } },
	[R_ALC269_INV_DMIC] = { "alc269_inv_dmic", NULL, NULL, NULL,
		ALC_OP_INV_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_ACER_AC700] = { "alc269_acer_ac700", alc_pins_0, NULL, NULL,
		ALC_OP_ALC271_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_ASPIRE_HEADSET_MIC] = { "alc269_aspire_headset_mic", alc_pins_1, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC271_HP_GATE_MIC_JACK] = { "alc271_hp_gate_mic_jack", alc_pins_2, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC271_HP_GATE_MIC_JACK_E1_572] = { "alc271_hp_gate_mic_jack_e1_572", alc_pins_2, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC282_ASPIRE_V5_PINS] = { "alc282_aspire_v5_pins", alc_pins_3, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_ASPIRE_E1_COEF] = { "alc269vb_aspire_e1_coef", NULL, NULL, NULL,
		ALC_OP_ASPIRE_E1_COEF,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_ACER_LIMIT_INT_MIC_BOOST] = { "alc255_acer_limit_int_mic_boost", alc_pins_1, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_INT_MIC_BOOST1 | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LIFEBOOK] = { "alc269_lifebook", alc_pins_4, NULL, NULL,
		ALC_OP_QUANTA_MUTE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC286_ACER_AIO_MIC_NO_PRESENCE] = { "alc286_acer_aio_mic_no_presence", alc_pins_5, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VC_ACER_HEADSET_MIC] = { "alc269vc_acer_headset_mic", alc_pins_6, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC283_CHROME_BOOK] = { "alc283_chrome_book", NULL, NULL, alc_dacs_0,
		ALC_OP_ALC283_CHROMEBOOK | ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_ACER_MIC_NO_PRESENCE] = { "alc255_acer_mic_no_presence", alc_pins_1, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_PREDATOR_SUBWOOFER] = { "alc255_predator_subwoofer", alc_pins_7, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC299_PREDATOR_SPK] = { "alc299_predator_spk", alc_pins_8, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VC_ACER_VCOPPERBOX_PINS] = { "alc269vc_acer_vcopperbox_pins", alc_pins_9, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VC_ACER_MIC_NO_PRESENCE] = { "alc269vc_acer_mic_no_presence", alc_pins_10, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ACER_HEADSET_MIC] = { "alc256_acer_headset_mic", alc_pins_11, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ACER_MIC_NO_PRESENCE] = { "alc256_acer_mic_no_presence", alc_pins_12, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC286_ACER_AIO_HEADSET_MIC] = { "alc286_acer_aio_headset_mic", alc_pins_5, alc_verbs_0, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_ACER_HEADSET_MIC] = { "alc233_acer_headset_mic", alc_pins_1, alc_verbs_1, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_ACER_HEADSET_MIC] = { "alc255_acer_headset_mic", alc_pins_13, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_ACER_HEADPHONE_AND_MIC] = { "alc255_acer_headphone_and_mic", alc_pins_14, alc_verbs_1, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC2XX_HEADSET_MIC] = { "alc2xx_headset_mic", alc_pins_15, NULL, NULL,
		ALC_OP_COMBO_SWAP | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ACER_COLDBOOT] = { "alc256_acer_coldboot", NULL, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_REDETECT,
		ALC_LED_NONE, ALC_LED_GPIO_04, 0x00, 0x00, { 0 } },
	[R_ALC256_ACER_SFG16_MICMUTE_LED] = { "alc256_acer_sfg16_micmute_led", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_GPIO_04, 0x00, 0x00, { 0 } },
	[R_ALC245_ACER_MICMUTE_LED] = { "alc245_acer_micmute_led", alc_pins_15, NULL, NULL,
		ALC_OP_COMBO_SWAP | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC287_PREDATOR_SPK_CS35L41_I2C_2] = { "alc287_predator_spk_cs35l41_i2c_2", alc_pins_7, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL_M101Z] = { "alc269_dell_m101z", NULL, alc_verbs_2, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC292_DELL_E7X] = { "alc292_dell_e7x", alc_pins_16, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_NO_AAMIX | ALC_OP_XPS13,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC275_DELL_XPS] = { "alc275_dell_xps", NULL, alc_verbs_3, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC290_SUBWOOFER] = { "alc290_subwoofer", alc_pins_17, NULL, alc_dacs_1,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL1_MIC_NO_PRESENCE] = { "alc269_dell1_mic_no_presence", alc_pins_18, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC283_DELL_HP_RESUME] = { "alc283_dell_hp_resume", NULL, NULL, NULL,
		ALC_OP_DELL_HP_RESUME,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC290_SUBWOOFER_HSJACK] = { "alc290_subwoofer_hsjack", alc_pins_19, NULL, alc_dacs_1,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC290_MONO_SPEAKERS_HSJACK] = { "alc290_mono_speakers_hsjack", alc_pins_20, NULL, alc_dacs_1,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC293_DELL1_MIC_NO_PRESENCE] = { "alc293_dell1_mic_no_presence", alc_pins_21, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC288_DELL_XPS_13] = { "alc288_dell_xps_13", alc_pins_22, NULL, NULL,
		ALC_OP_DELL_ALC288 | ALC_OP_HS_MODE | ALC_OP_NO_AAMIX | ALC_OP_XPS13,
		ALC_LED_NONE, ALC_LED_NONE, 0x40, 0x00, { 0 } },
	[R_ALC255_DELL1_MIC_NO_PRESENCE] = { "alc255_dell1_mic_no_presence", alc_pins_18, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC293_DISABLE_AAMIX_MULTIJACK] = { "alc293_disable_aamix_multijack", alc_pins_21, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_DELL_INSPIRON_7559_SUBWOOFER] = { "alc256_dell_inspiron_7559_subwoofer", alc_pins_23, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_DELL_SPK_NOISE] = { "alc255_dell_spk_noise", alc_pins_18, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT | ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_NO_SHUTUP] = { "alc269_no_shutup", NULL, NULL, NULL,
		ALC_OP_NO_SHUTUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_SPK_VOLUME] = { "alc298_spk_volume", alc_pins_5, NULL, alc_dacs_2,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_DISABLE_DAC3] = { "alc295_disable_dac3", NULL, NULL, alc_dacs_3,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC225_DELL_WYSE_MIC_NO_PRESENCE] = { "alc225_dell_wyse_mic_no_presence", alc_pins_24, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC274_DELL_AIO_LINEOUT_VERB] = { "alc274_dell_aio_lineout_verb", alc_pins_25, NULL, alc_dacs_4,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_DELL_HEADSET_MIC] = { "alc255_dell_headset_mic", alc_pins_1, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_DUMMY_LINEOUT_VERB] = { "alc255_dummy_lineout_verb", alc_pins_26, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL4_MIC_NO_PRESENCE] = { "alc269_dell4_mic_no_presence", alc_pins_27, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC225_DELL_WYSE_AIO_MIC_NO_PRESENCE] = { "alc225_dell_wyse_aio_mic_no_presence", alc_pins_28, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_MIC_VREF_HIZ,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC225_DELL1_MIC_NO_PRESENCE] = { "alc225_dell1_mic_no_presence", alc_pins_18, alc_verbs_4, NULL,
		ALC_OP_HS_MODE | ALC_OP_MIC_VREF_HIZ,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_DUAL_SPK] = { "alc289_dual_spk", alc_pins_29, NULL, alc_dacs_2,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_DELL_AIO_HEADSET_MIC] = { "alc236_dell_aio_headset_mic", alc_pins_18, NULL, NULL,
		ALC_OP_COMBO_SWAP | ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT | ALC_OP_JD_RESTART | ALC_OP_NO_INT_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL4_MIC_NO_PRESENCE_QUIET] = { "alc269_dell4_mic_no_presence_quiet", alc_pins_27, NULL, NULL,
		ALC_OP_DELL4_QUIET | ALC_OP_HS_MODE | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_CS35L41_SPI_2] = { "alc245_cs35l41_spi_2", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_DELL_INSPIRON_TOP_SPEAKERS] = { "alc295_dell_inspiron_top_speakers", alc_pins_30, NULL, alc_dacs_5,
		ALC_OP_HS_MODE | ALC_OP_NO_SHUTUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_DELL_CS35L41_SPI_2] = { "alc289_dell_cs35l41_spi_2", alc_pins_29, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP | ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_RTK_AMP_DUAL_SPK] = { "alc289_rtk_amp_dual_spk", alc_pins_31, NULL, alc_dacs_2,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_DELL_HP_POP_NOISE] = { "alc236_dell_hp_pop_noise", alc_pins_18, NULL, alc_dacs_0,
		ALC_OP_HS_MODE | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_MUTE_LED_MIC2] = { "alc269_hp_mute_led_mic2", NULL, NULL, NULL,
		0,
		ALC_LED_VREF_19, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_GPIO_LED] = { "alc269_hp_gpio_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_GPIO_10, 0x00, 0x00, { 0 } },
	[R_ALC269_LIMIT_INT_MIC_BOOST_MUTE_LED] = { "alc269_limit_int_mic_boost_mute_led", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_VREF_18, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_MUTE_LED_MIC1] = { "alc269_hp_mute_led_mic1", NULL, NULL, NULL,
		0,
		ALC_LED_VREF_18, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_GPIO_MIC1_LED] = { "alc269_hp_gpio_mic1_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_VREF_18, 0x10, 0x00, { 0 } },
	[R_ALC280_HP_HEADSET_MIC] = { "alc280_hp_headset_mic", NULL, NULL, NULL,
		ALC_OP_HS_MIC | ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_LINE1_MIC1_LED] = { "alc269_hp_line1_mic1_led", NULL, NULL, NULL,
		0,
		ALC_LED_VREF_1A, ALC_LED_VREF_18, 0x00, 0x00, { 0 } },
	[R_ALC269_HP_DOCK_GPIO_MIC1_LED] = { "alc269_hp_dock_gpio_mic1_led", alc_pins_32, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_VREF_18, 0x10, 0x00, { 0 } },
	[R_ALC280_HP_GPIO2_MIC_HOTKEY] = { "alc280_hp_gpio2_mic_hotkey", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC286_HP_GPIO_LED] = { "alc286_hp_gpio_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_02, ALC_LED_GPIO_20, 0x00, 0x00, { 0 } },
	[R_ALC280_HP_DOCK_PINS] = { "alc280_hp_dock_pins", alc_pins_33, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_VREF_18, 0x00, 0x00, { 0 } },
	[R_ALC280_HP_9480M] = { "alc280_hp_9480m", NULL, NULL, NULL,
		ALC_OP_HP_AMP_GPIO4,
		ALC_LED_GPIO_08, ALC_LED_NONE, 0x10, 0x00, { 0 } },
	[R_ALC221_HP_288PRO_MIC_NO_PRESENCE] = { "alc221_hp_288pro_mic_no_presence", alc_pins_34, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC221_HP_MIC_NO_PRESENCE] = { "alc221_hp_mic_no_presence", alc_pins_22, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_HP_HEADSET_MIC] = { "alc256_hp_headset_mic", NULL, NULL, NULL,
		ALC_OP_JD_RESTART,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_HP_X360] = { "alc295_hp_x360", NULL, NULL, NULL,
		0,
		ALC_LED_VREF_1B, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC221_HP_FRONT_MIC] = { "alc221_hp_front_mic", alc_pins_35, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_HP_PAVILION_X360] = { "alc295_hp_pavilion_x360", NULL, alc_verbs_5, NULL,
		0,
		ALC_LED_VREF_1B, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_MUTE_LED_COEFBIT2] = { "alc236_hp_mute_led_coefbit2", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_236B, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_HP_OMEN] = { "alc295_hp_omen", alc_pins_36, NULL, NULL,
		0,
		ALC_LED_VREF_1A, ALC_LED_VREF_18, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_SPECTRE_X360] = { "alc285_hp_spectre_x360", alc_pins_37, NULL, alc_dacs_2,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_MUTE_LED_MICMUTE_VREF] = { "alc236_hp_mute_led_micmute_vref", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_236, ALC_LED_VREF_1A, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_GPIO_LED] = { "alc285_hp_gpio_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_04, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC225_HP_AIO_SPK] = { "alc225_hp_aio_spk", NULL, alc_verbs_6, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_HP_MUTE_LED_COEFBIT11] = { "alc295_hp_mute_led_coefbit11", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_295, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_ENVY_X360] = { "alc285_hp_envy_x360", alc_pins_38, NULL, alc_dacs_3,
		ALC_OP_AMP_COEF65 | ALC_OP_ENVY_X360,
		ALC_LED_GPIO_04, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_MUTE_LED_MICMUTE_GPIO] = { "alc236_hp_mute_led_micmute_gpio", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_236B, ALC_LED_GPIO_01_INV, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_MUTE_LED] = { "alc285_hp_mute_led", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_285, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_GPIO_AMP_INIT] = { "alc285_hp_gpio_amp_init", NULL, NULL, NULL,
		ALC_OP_AMP_COEF65,
		ALC_LED_GPIO_04, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_MUTE_LED] = { "alc236_hp_mute_led", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_236, ALC_LED_COEF_MIC236, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_ENVY_13AQ] = { "alc285_hp_envy_13aq", alc_pins_39, NULL, NULL,
		ALC_OP_AMP_COEF65,
		ALC_LED_GPIO_04, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC274_HP_ENVY_GPIO] = { "alc274_hp_envy_gpio", NULL, NULL, NULL,
		ALC_OP_AMP_GPIO2_PLAYBACK,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x00, { 0 } },
	[R_ALC285_HP_SPECTRE_X360_EB1] = { "alc285_hp_spectre_x360_eb1", alc_pins_40, NULL, alc_dacs_6,
		ALC_OP_GPIO0_PULSE,
		ALC_LED_NONE, ALC_LED_GPIO_04_INV, 0x01, 0x00, { 0 } },
	[R_ALC285_HP_SPECTRE_X360_DF1] = { "alc285_hp_spectre_x360_df1", alc_pins_40, NULL, alc_dacs_6,
		ALC_OP_GPIO0_PULSE,
		ALC_LED_COEF_285, ALC_LED_NONE, 0x01, 0x00, { 0 } },
	[R_ALC285_HP_SPECTRE_X360_MUTE_LED] = { "alc285_hp_spectre_x360_mute_led", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_285, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_X360_MUTE_LEDS] = { "alc245_hp_x360_mute_leds", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_245, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC287_HP_GPIO_LED] = { "alc287_hp_gpio_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_10, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_GPIO_LED] = { "alc236_hp_gpio_led", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_02, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_X360_AMP] = { "alc245_hp_x360_amp", NULL, NULL, NULL,
		ALC_OP_GPIO0_PULSE,
		ALC_LED_NONE, ALC_LED_GPIO_04_INV, 0x01, 0x00, { 0 } },
	[R_ALC236_HP_LIMIT_INT_MIC_BOOST] = { "alc236_hp_limit_int_mic_boost", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_COEF_236, ALC_LED_VREF_1A, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_SPEAKERS_MICMUTE_LED] = { "alc285_hp_speakers_micmute_led", NULL, alc_verbs_7, NULL,
		0,
		ALC_LED_COEF_285, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC285_HP_LIMIT_INT_MIC_BOOST] = { "alc285_hp_limit_int_mic_boost", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_COEF_285, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_ENVY_X360_MUTE_LED] = { "alc245_hp_envy_x360_mute_led", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_245V1, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_MUTE_LED_V1_COEFBIT] = { "alc245_hp_mute_led_v1_coefbit", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_245V1, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_MUTE_LED_V2_COEFBIT] = { "alc245_hp_mute_led_v2_coefbit", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_245V2, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_MUTE_LED_COEFBIT] = { "alc245_hp_mute_led_coefbit", NULL, NULL, NULL,
		0,
		ALC_LED_COEF_245, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_CS35L41_SPI_2_HP_GPIO_LED] = { "alc245_cs35l41_spi_2_hp_gpio_led", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_GPIO_04, ALC_LED_GPIO_01, 0x00, 0x00, { 0 } },
	[R_ALC287_CS35L41_I2C_2_HP_GPIO_LED] = { "alc287_cs35l41_i2c_2_hp_gpio_led", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_285, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_SPECTRE_X360_EU0XXX] = { "alc245_hp_spectre_x360_eu0xxx", alc_pins_41, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS] = { "alc287_cs35l41_i2c_2_hp_mute_leds", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC274_HP_VERBS] = { "alc274_hp_verbs", NULL, alc_verbs_8, NULL,
		ALC_OP_GPIO03_PULSE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_15_FD0XXX] = { "alc236_hp_15_fd0xxx", NULL, NULL, NULL,
		ALC_OP_NO_SHUTUP,
		ALC_LED_VREF100_1A, ALC_LED_GPIO_01_INV, 0x00, 0x00, { 0 } },
	[R_ALC274_HP_AIO_BIND_DACS] = { "alc274_hp_aio_bind_dacs", NULL, NULL, alc_dacs_7,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_HP_DMIC] = { "alc236_hp_dmic", alc_pins_42, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_ZBOOK_FIREFLY_G12A] = { "alc245_hp_zbook_firefly_g12a", NULL, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245, ALC_LED_COEF_MIC285, 0x00, 0x00, { 0 } },
	[R_ALC245_CS35L41_I2C_2_MUTE_LED] = { "alc245_cs35l41_i2c_2_mute_led", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_TAS2781_I2C_MUTE_LED] = { "alc245_hp_tas2781_i2c_mute_led", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245V1, ALC_LED_GPIO_04_INV, 0x00, 0x00, { 0 } },
	[R_ALC245_HP_TAS2781_I2C_MUTE_LED_INVERTED] = { "alc245_hp_tas2781_i2c_mute_led_inverted", NULL, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_COEF_245V1, ALC_LED_GPIO_04, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_GA403U_HEADSET_MIC] = { "alc285_asus_ga403u_headset_mic", alc_pins_43, NULL, alc_dacs_2,
		ALC_OP_INV_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ASUS_MIC_NO_PRESENCE] = { "alc256_asus_mic_no_presence", alc_pins_44, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1] = { "alc285_asus_gu605_spi_speaker2_to_dac1", alc_pins_43, NULL, alc_dacs_2,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ASUS_MIC] = { "alc256_asus_mic", alc_pins_45, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC282_ASUS_TX300] = { "alc282_asus_tx300", alc_pins_46, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x04, { 0 } },
	[R_ALC269_LIMIT_INT_MIC_BOOST] = { "alc269_limit_int_mic_boost", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_SPK] = { "alc294_asus_spk", alc_pins_47, alc_verbs_9, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_TAS2781_I2C] = { "alc287_tas2781_i2c", NULL, NULL, alc_dacs_8,
		ALC_OP_EXT_AMP | ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_TXNW2781_I2C_ASUS] = { "alc287_txnw2781_i2c_asus", alc_pins_47, alc_verbs_9, NULL,
		ALC_OP_EXT_AMP | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_EAPD_COEF_AND_MIC_NO_PRESENCE] = { "alc233_eapd_coef_and_mic_no_presence", alc_pins_1, alc_verbs_10, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_ASUS_N7601ZM] = { "alc269_asus_n7601zm", alc_pins_48, alc_verbs_11, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_CS35L41_SPI_2] = { "alc294_asus_cs35l41_spi_2", alc_pins_47, NULL, NULL,
		ALC_OP_EXT_AMP | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_ASUS_MIC_NO_PRESENCE] = { "alc269vb_asus_mic_no_presence", alc_pins_49, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_ASUS_ZENBOOK] = { "alc269vb_asus_zenbook", alc_pins_50, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_I2C_HEADSET_MIC] = { "alc285_asus_i2c_headset_mic", alc_pins_43, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_ASUS_ZENBOOK_UX31A] = { "alc269vb_asus_zenbook_ux31a", alc_pins_50, alc_verbs_12, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_G533Z_PINS] = { "alc285_asus_g533z_pins", alc_pins_51, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_ASUS_ZEPHYRUS_DUAL_SPK] = { "alc289_asus_zephyrus_dual_spk", alc_pins_52, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_DUAL_SPK] = { "alc294_asus_dual_spk", alc_pins_47, NULL, alc_dacs_2,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x04, { 0 } },
	[R_ALC256_ASUS_PM3406CHA] = { "alc256_asus_pm3406cha", NULL, alc_verbs_13, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_ASUS_GA401] = { "alc289_asus_ga401", alc_pins_53, NULL, alc_dacs_9,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_STEREO_DMIC] = { "alc269_stereo_dmic", NULL, NULL, NULL,
		ALC_OP_STEREO_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_ASUS_DACS] = { "alc295_asus_dacs", NULL, NULL, alc_dacs_10,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_ALLY] = { "alc294_asus_ally", alc_pins_48, alc_verbs_14, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_GX502_PINS] = { "alc294_asus_gx502_pins", alc_pins_54, alc_verbs_15, NULL,
		ALC_OP_GX502,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ASUS_HEADSET_MIC] = { "alc256_asus_headset_mic", alc_pins_53, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_HPE] = { "alc294_asus_hpe", alc_pins_47, alc_verbs_16, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ASUS_HPE] = { "alc256_asus_hpe", alc_pins_47, alc_verbs_17, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_I2C_HEADSET_MIC] = { "alc294_asus_i2c_headset_mic", alc_pins_55, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_ASUS_G73JW] = { "alc269_asus_g73jw", alc_pins_56, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_SPI_HEADSET_MIC] = { "alc294_asus_spi_headset_mic", alc_pins_57, NULL, NULL,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_LENOVO_MIC_LOCATION] = { "alc294_lenovo_mic_location", alc_pins_58, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_COEF_1B] = { "alc294_asus_coef_1b", alc_pins_53, alc_verbs_18, alc_dacs_9,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_SPI_REAR_SPEAKERS] = { "alc285_asus_spi_rear_speakers", alc_pins_59, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_BASS_SPEAKER_15] = { "alc294_bass_speaker_15", NULL, NULL, alc_dacs_11,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC289_ASUS_GA502] = { "alc289_asus_ga502", alc_pins_53, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_GU502_PINS] = { "alc294_asus_gu502_pins", alc_pins_60, alc_verbs_19, NULL,
		ALC_OP_GU502,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_G513_PINS] = { "alc294_asus_g513_pins", alc_pins_48, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_ASUS_ALLY_X] = { "alc287_asus_ally_x", alc_pins_61, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP | ALC_OP_HS_KEYS,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_ASUS_I2C_SPEAKER2_TO_DAC1] = { "alc285_asus_i2c_speaker2_to_dac1", NULL, NULL, alc_dacs_2,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_ASUS_AIO_GPIO2] = { "alc256_asus_aio_gpio2", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x04, { 0 } },
	[R_ALC274_ASUS_ZEN_AIO_27] = { "alc274_asus_zen_aio_27", alc_pins_15, alc_verbs_20, NULL,
		ALC_OP_COMBO_SWAP | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_ASUS_X101] = { "alc269_asus_x101", alc_pins_62, alc_verbs_21, NULL,
		ALC_OP_HS_MIC | ALC_OP_X101_HSMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC275_SONY_VAIO_GPIO2] = { "alc275_sony_vaio_gpio2", NULL, alc_verbs_22, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x00, { 0 } },
	[R_ALC275_SONY_HWEQ] = { "alc275_sony_hweq", NULL, alc_verbs_22, NULL,
		ALC_OP_HWEQ,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x00, { 0 } },
	[R_ALC275_SONY_DISABLE_AAMIX] = { "alc275_sony_disable_aamix", NULL, alc_verbs_22, NULL,
		ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LIFEBOOK_NO_HP_TO_LINEOUT] = { "alc269_lifebook_no_hp_to_lineout", NULL, NULL, NULL,
		ALC_OP_NO_HP_FIXUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LIFEBOOK_HP_PIN] = { "alc269_lifebook_hp_pin", alc_pins_63, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC] = { "alc255_lifebook_u7x7_headset_mic", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LIFEBOOK_EXTMIC] = { "alc269_lifebook_extmic", alc_pins_64, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC700_INTEL_REFERENCE] = { "alc700_intel_reference", NULL, alc_verbs_23, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC290_MONO_SPEAKERS] = { "alc290_mono_speakers", NULL, NULL, alc_dacs_1,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_MEDION_HEADSET_NO_PRESENCE] = { "alc256_medion_headset_no_presence", alc_pins_65, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_CHROME_BOOK] = { "alc295_chrome_book", NULL, NULL, NULL,
		ALC_OP_HS_KEYS | ALC_OP_JD_RESTART | ALC_OP_ULTRA_LOW_POWER,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC288_SURFACE_SWAP_DACS] = { "alc288_surface_swap_dacs", NULL, NULL, alc_dacs_12,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_SAMSUNG_AMP] = { "alc298_samsung_amp", NULL, alc_verbs_24, NULL,
		ALC_OP_SAMSUNG_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_SAMSUNG_AMP_V2_2_AMPS] = { "alc298_samsung_amp_v2_2_amps", NULL, NULL, NULL,
		ALC_OP_SAMSUNG_AMP2,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_SAMSUNG_HEADPHONE_VERY_QUIET] = { "alc298_samsung_headphone_very_quiet", NULL, alc_verbs_24, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_SAMSUNG_HEADPHONE_VERY_QUIET] = { "alc256_samsung_headphone_very_quiet", NULL, alc_verbs_25, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_SAMSUNG_AMP_V2_4_AMPS] = { "alc298_samsung_amp_v2_4_amps", NULL, NULL, NULL,
		ALC_OP_SAMSUNG_AMP4,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC283_HEADSET_MIC] = { "alc283_headset_mic", alc_pins_66, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_SYSTEM76_MIC_NO_PRESENCE] = { "alc256_system76_mic_no_presence", alc_pins_67, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC293_SYSTEM76_MIC_NO_PRESENCE] = { "alc293_system76_mic_no_presence", alc_pins_20, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_CLEVO_NOISY_MIC] = { "alc245_clevo_noisy_mic", alc_pins_67, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_HEADSET_MIC] = { "alc269_headset_mic", NULL, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DMIC] = { "alc269_dmic", alc_pins_68, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_MIC_NO_PRESENCE_AND_RESUME] = { "alc256_mic_no_presence_and_resume", NULL, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_NJ51CU,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_VAIO_VJFH52_MIC_NO_PRESENCE] = { "alc269_vaio_vjfh52_mic_no_presence", alc_pins_69, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_VAIO_RPL_MIC_NO_PRESENCE] = { "alc256_vaio_rpl_mic_no_presence", alc_pins_70, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_LENOVO_MULTI_CODECS] = { "alc233_lenovo_multi_codecs", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x02, 0x02, { 0 } },
	[R_ALC623_LENOVO_THINKSTATION_P340] = { "alc623_lenovo_thinkstation_p340", alc_pins_66, NULL, NULL,
		ALC_OP_NO_SHUTUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_SKU_IGNORE] = { "alc269_sku_ignore", NULL, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LENOVO_DOCK] = { "alc269_lenovo_dock", alc_pins_71, NULL, NULL,
		ALC_OP_NO_HP_FIXUP | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LENOVO_DOCK_LIMIT_BOOST] = { "alc269_lenovo_dock_limit_boost", alc_pins_71, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_NO_HP_FIXUP | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC292_TPT440] = { "alc292_tpt440", alc_pins_72, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_NO_AAMIX | ALC_OP_NO_HP_FIXUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC292_TPT440_DOCK] = { "alc292_tpt440_dock", alc_pins_72, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_NO_HP_FIXUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST] = { "alc269_thinkpad_limit_int_mic_boost", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_TPT470_DOCK] = { "alc298_tpt470_dock", alc_pins_73, NULL, alc_dacs_13,
		ALC_OP_DOCK_ENABLE | ALC_OP_NO_AAMIX | ALC_OP_NO_HP_FIXUP | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC292_TPT460] = { "alc292_tpt460", alc_pins_72, NULL, NULL,
		ALC_OP_NO_AAMIX | ALC_OP_NO_HP_FIXUP | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK] = { "alc285_thinkpad_no_bass_spk_headset_jack", NULL, NULL, NULL,
		ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_THINKPAD_HEADSET_JACK] = { "alc285_thinkpad_headset_jack", NULL, NULL, alc_dacs_8,
		ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD] = { "alc287_mg_rtkc_csamp_cs35l41_i2c_thinkpad", NULL, NULL, alc_dacs_14,
		ALC_OP_EXT_AMP | ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_LENOVO_THKPAD_WH_ALC1318] = { "alc287_lenovo_thkpad_wh_alc1318", NULL, NULL, NULL,
		ALC_OP_EXT_AMP | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_LENOVO_GPIO2_MIC_HOTKEY] = { "alc233_lenovo_gpio2_mic_hotkey", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_COEF_MIC233, 0x00, 0x00, { 0 } },
	[R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN] = { "alc287_yoga9_14iap7_bass_spk_pin", alc_pins_74, alc_verbs_26, alc_dacs_9,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_YOGA7_14ITL_SPEAKERS] = { "alc287_yoga7_14itl_speakers", NULL, alc_verbs_27, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_LEGION_15IMHG05_SPEAKERS] = { "alc287_legion_15imhg05_speakers", NULL, alc_verbs_28, NULL,
		ALC_OP_HS_MODE | ALC_OP_NO_AUTOMUTE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_LENOVO_C940_DUET7] = { "alc298_lenovo_c940_duet7", NULL, NULL, alc_dacs_2,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_13S_GEN2_SPEAKERS] = { "alc287_13s_gen2_speakers", NULL, alc_verbs_29, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_LEGION_Y9000X_SPEAKERS] = { "alc285_legion_y9000x_speakers", NULL, NULL, NULL,
		ALC_OP_NO_AUTOMUTE | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_YOGA9_14IMH9_BASS_SPK_PIN] = { "alc287_yoga9_14imh9_bass_spk_pin", alc_pins_74, NULL, alc_dacs_9,
		ALC_OP_EXT_AMP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_YOGA_SPEAKER2_TO_DAC1] = { "alc285_yoga_speaker2_to_dac1", NULL, NULL, alc_dacs_2,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_LENOVO_YOGA_PRO7] = { "alc287_lenovo_yoga_pro7", NULL, NULL, alc_dacs_8,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DMIC_THINKPAD_ACPI] = { "alc269_dmic_thinkpad_acpi", NULL, NULL, NULL,
		ALC_OP_INV_DMIC | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN_HEADSET] = { "alc287_yoga9_14iap7_bass_spk_pin_headset", alc_pins_74, alc_verbs_26, alc_dacs_9,
		ALC_OP_HS_KEYS,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC236_LENOVO_INV_DMIC] = { "alc236_lenovo_inv_dmic", NULL, alc_verbs_30, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_INV_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_YOGA9_SPEAKER2_TO_DAC1] = { "alc287_yoga9_speaker2_to_dac1", NULL, NULL, alc_dacs_15,
		ALC_OP_EXT_AMP | ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_LENOVO_XPAD_HEADSET_JACK] = { "alc287_lenovo_xpad_headset_jack", NULL, NULL, alc_dacs_8,
		ALC_OP_HS_KEYS | ALC_OP_INT_MIC_BOOST1 | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC283_INT_MIC] = { "alc283_int_mic", NULL, alc_verbs_30, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_PCM_44K] = { "alc269_pcm_44k", NULL, NULL, NULL,
		ALC_OP_QUANTA_MUTE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC293_LENOVO_SPK_NOISE] = { "alc293_lenovo_spk_noise", NULL, NULL, NULL,
		ALC_OP_NO_AAMIX | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LENOVO_EAPD] = { "alc269_lenovo_eapd", NULL, alc_verbs_31, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_NO_AUDIO_JACK] = { "alc233_no_audio_jack", NULL, NULL, NULL,
		ALC_OP_ALC233_JACK_COEFS,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_HEADPHONE_AMP_VOL] = { "alc256_headphone_amp_vol", NULL, NULL, NULL,
		ALC_OP_HP_AMP_LIMIT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_HUAWEI_MACH_WX9_PINS] = { "alc256_huawei_mach_wx9_pins", alc_pins_75, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_RAZER_BLADE16_2025_PINS] = { "alc298_razer_blade16_2025_pins", alc_pins_76, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_CZC_B20] = { "alc269_czc_b20", alc_pins_77, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_CZC_TMI] = { "alc269_czc_tmi", alc_pins_78, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_CZC_L101] = { "alc269_czc_l101", alc_pins_79, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_ORDISSIMO_EVE2] = { "alc269vb_ordissimo_eve2", alc_pins_80, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LEMOTE_A1802] = { "alc269_lemote_a1802", alc_pins_81, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_LEMOTE_A190X] = { "alc269_lemote_a190x", alc_pins_82, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC288_DELL1_MIC_NO_PRESENCE] = { "alc288_dell1_mic_no_presence", alc_pins_22, NULL, NULL,
		ALC_OP_DELL_ALC288 | ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x40, 0x00, { 0 } },
	[R_ALC256_SET_COEF_DEFAULTS] = { "alc256_set_coef_defaults", NULL, NULL, NULL,
		ALC_OP_ALC256_COEF_DEFAULTS,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_XIAOMI_HEADSET_MIC] = { "alc255_xiaomi_headset_mic", alc_pins_53, alc_verbs_1, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_XIAOMI_PRO15_RESUME] = { "alc256_xiaomi_pro15_resume", NULL, NULL, NULL,
		ALC_OP_COEF10_BIT9,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_MEDION_MTL_SPK] = { "alc233_medion_mtl_spk", alc_pins_83, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_WUJIE_SPEAKERS] = { "alc233_wujie_speakers", alc_pins_84, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_HONOR_MRB_XXX_M1020_AUDIO] = { "alc256_honor_mrb_xxx_m1020_audio", alc_pins_85, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_MINISFORUM_JACK_DETECT] = { "alc245_minisforum_jack_detect", NULL, NULL, NULL,
		ALC_OP_MINISFORUM_JD,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC245_BASS_HP_DAC] = { "alc245_bass_hp_dac", NULL, NULL, alc_dacs_8,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_STARLABS_STARFIGHTER] = { "alc233_starlabs_starfighter", NULL, NULL, NULL,
		ALC_OP_AMP_GPIO2_PLAYBACK | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x04, 0x00, { 0 } },
	[R_ALC269VB_INFINIX_ZERO_BOOK_13] = { "alc269vb_infinix_zero_book_13", alc_pins_86, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_CHUWI_COREBOOK_XPRO] = { "alc269vb_chuwi_corebook_xpro", alc_pins_87, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VB_INFINIX_INBOOK_X3_SLIM] = { "alc269vb_infinix_inbook_x3_slim", alc_pins_88, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_POSITIVO_P15X_HEADSET_MIC] = { "alc269_positivo_p15x_headset_mic", alc_pins_10, NULL, NULL,
		ALC_OP_HS_MIC | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269VC_INFINIX_Y4_MAX] = { "alc269vc_infinix_y4_max", alc_pins_84, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC700_INTEL_HADES_CANYON] = { "alc700_intel_hades_canyon", NULL, NULL, NULL,
		ALC_OP_HADES_CANYON,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC233_INTEL_NUC8_DMIC] = { "alc233_intel_nuc8_dmic", NULL, NULL, NULL,
		ALC_OP_INT_MIC_BOOST1 | ALC_OP_INV_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC256_INTEL_NUC8_RUGGED] = { "alc256_intel_nuc8_rugged", alc_pins_89, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE] = { "alc295_framework_laptop_mic_no_presence", alc_pins_90, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC295_FRAMEWORK_LAPTOP_LIMIT_INT_MIC_BOOST] = { "alc295_framework_laptop_limit_int_mic_boost", alc_pins_90, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_AMIC] = { "alc269_amic", alc_pins_91, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC271_DMIC] = { "alc271_dmic", NULL, NULL, NULL,
		ALC_OP_ALC271_DMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_SONY_VAIO] = { "alc269_sony_vaio", NULL, alc_verbs_22, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC221_HP_HEADSET_MIC] = { "alc221_hp_headset_mic", alc_pins_92, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_DELL2_MIC_NO_PRESENCE] = { "alc255_dell2_mic_no_presence", alc_pins_1, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC274_HP_HEADSET_MIC] = { "alc274_hp_headset_mic", NULL, alc_verbs_1, NULL,
		ALC_OP_JD_RESTART,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC280_HP_GPIO4] = { "alc280_hp_gpio4", NULL, NULL, NULL,
		0,
		ALC_LED_GPIO_08, ALC_LED_VREF_18, 0x00, 0x00, { 0 } },
	[R_ALC282_ACER_DISABLE_LINEOUT] = { "alc282_acer_disable_lineout", alc_pins_93, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC285_LENOVO_PC_BEEP_IN_NOISE] = { "alc285_lenovo_pc_beep_in_noise", NULL, alc_verbs_4, alc_dacs_0,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC287_THINKPAD_I2S_SPK] = { "alc287_thinkpad_i2s_spk", NULL, NULL, alc_dacs_14,
		ALC_OP_HS_KEYS | ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL2_MIC_NO_PRESENCE] = { "alc269_dell2_mic_no_presence", alc_pins_16, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC294_ASUS_MIC] = { "alc294_asus_mic", alc_pins_45, NULL, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC298_ALIENWARE_MIC_NO_PRESENCE] = { "alc298_alienware_mic_no_presence", alc_pins_5, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC255_DELL1_LIMIT_INT_MIC_BOOST] = { "alc255_dell1_limit_int_mic_boost", alc_pins_18, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_INT_MIC_BOOST1 | ALC_OP_JACK_CTIA_DEFAULT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC269_DELL1_LIMIT_INT_MIC_BOOST] = { "alc269_dell1_limit_int_mic_boost", alc_pins_18, NULL, NULL,
		ALC_OP_HS_MODE | ALC_OP_INT_MIC_BOOST1,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE2] = { "alc662_asus_mode2", alc_pins_94, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC897_HEADSET_MIC_PIN3] = { "alc897_headset_mic_pin3", alc_pins_61, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_CSL_GPIO] = { "alc662_csl_gpio", NULL, NULL, NULL,
		ALC_OP_GPIO03_PULSE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASPIRE] = { "alc662_aspire", alc_pins_95, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC669_ACER_ASPIRE_ETHOS] = { "alc669_acer_aspire_ethos", alc_pins_96, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ACER_NITRO_HEADSET_MODE] = { "alc662_acer_nitro_headset_mode", alc_pins_97, NULL, NULL,
		ALC_OP_HS_MIC | ALC_OP_USI_HSMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ACER_X2660G_HEADSET_MODE] = { "alc662_acer_x2660g_headset_mode", alc_pins_98, NULL, NULL,
		ALC_OP_HS_MIC | ALC_OP_USI_HSMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_DELL_MIC_NO_PRESENCE] = { "alc668_dell_mic_no_presence", alc_pins_99, NULL, NULL,
		ALC_OP_ALC668_HS | ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_DELL_XPS13] = { "alc668_dell_xps13", alc_pins_99, NULL, NULL,
		ALC_OP_ALC668_HS | ALC_OP_HS_MODE | ALC_OP_NO_AAMIX | ALC_OP_XPS13,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_HP_RP5800] = { "alc662_hp_rp5800", alc_pins_100, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC897_HP_HSMIC_VERB] = { "alc897_hp_hsmic_verb", alc_pins_1, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC671_HP_HEADSET_MIC2] = { "alc671_hp_headset_mic2", alc_pins_101, NULL, NULL,
		ALC_OP_ALC671_HSMIC | ALC_OP_HS_MIC | ALC_OP_NO_AAMIX,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_HEADSET_MODE] = { "alc668_headset_mode", NULL, NULL, NULL,
		ALC_OP_ALC668_HS | ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_Nx50] = { "alc662_asus_nx50", alc_pins_102, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_ASUS_G751] = { "alc668_asus_g751", alc_pins_103, alc_verbs_32, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_BASS_MODE4_CHMAP] = { "alc662_bass_mode4_chmap", alc_pins_104, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_BASS_16] = { "alc662_bass_16", alc_pins_105, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_ASUS_Nx51] = { "alc668_asus_nx51", alc_pins_106, NULL, NULL,
		ALC_OP_ALC668_HS | ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC668_ASUS_NO_HEADSET_MIC] = { "alc668_asus_no_headset_mic", alc_pins_107, alc_verbs_33, NULL,
		ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE8] = { "alc662_asus_mode8", alc_pins_108, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_NO_JACK_DETECT] = { "alc662_no_jack_detect", NULL, NULL, NULL,
		ALC_OP_NO_JACK_DETECT,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_IDEAPAD] = { "alc662_ideapad", alc_pins_109, NULL, NULL,
		0,
		ALC_LED_GPIO_01_INV, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_USI_HEADSET_MODE] = { "alc662_usi_headset_mode", alc_pins_110, NULL, NULL,
		ALC_OP_HS_MIC | ALC_OP_USI_HSMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC897_HEADSET_MIC_PIN] = { "alc897_headset_mic_pin", alc_pins_111, NULL, NULL,
		ALC_OP_ALC897_HSMIC | ALC_OP_NO_SHUTUP,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC897_HEADSET_MIC_PIN2] = { "alc897_headset_mic_pin2", alc_pins_112, NULL, NULL,
		ALC_OP_ALC897_HSMODE | ALC_OP_HS_MIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC892_ASROCK_MOBO] = { "alc892_asrock_mobo", alc_pins_113, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ZOTAC_Z68] = { "alc662_zotac_z68", alc_pins_114, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ACER_VERITON] = { "alc662_acer_veriton", alc_pins_115, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_CZC_ET26] = { "alc662_czc_et26", alc_pins_116, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE1] = { "alc662_asus_mode1", alc_pins_117, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE3] = { "alc662_asus_mode3", alc_pins_118, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE7] = { "alc662_asus_mode7", alc_pins_119, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE6] = { "alc662_asus_mode6", alc_pins_120, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_ASUS_MODE5] = { "alc662_asus_mode5", alc_pins_121, NULL, NULL,
		ALC_OP_SKU_IGNORE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC891_DELL_MIC_NO_PRESENCE] = { "alc891_dell_mic_no_presence", alc_pins_99, NULL, NULL,
		ALC_OP_HS_MODE,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC662_DELL_MIC_NO_PRESENCE] = { "alc662_dell_mic_no_presence", alc_pins_122, NULL, NULL,
		ALC_OP_ALC662_HS | ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC883_ACER_EAPD] = { "alc883_acer_eapd", NULL, alc_verbs_34, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_ACER_ASPIRE_4930G] = { "alc882_acer_aspire_4930g", alc_pins_123, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0 } },
	[R_ALC882_ACER_ASPIRE_8930G] = { "alc882_acer_aspire_8930g", alc_pins_124, alc_verbs_35, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0 } },
	[R_ALC882_PB_M5210] = { "alc882_pb_m5210", NULL, alc_verbs_36, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_EAPD] = { "alc882_eapd", NULL, alc_verbs_37, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_ASUS_W90V] = { "alc882_asus_w90v", alc_pins_125, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_ASUS_W2JC] = { "alc882_asus_w2jc", NULL, alc_verbs_37, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0 } },
	[R_ALC887_ASUS_HMIC] = { "alc887_asus_hmic", alc_pins_126, NULL, NULL,
		ALC_OP_ASUS887_JACK,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC888_EEE1601] = { "alc888_eee1601", NULL, alc_verbs_38, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC887_ASUS_BASS] = { "alc887_asus_bass", alc_pins_127, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_GPIO3] = { "alc882_gpio3", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x03, 0x03, { 0 } },
	[R_ALCS1200A_MIC_VREF] = { "alcs1200a_mic_vref", NULL, alc_verbs_39, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC889_VAIO_TT] = { "alc889_vaio_tt", alc_pins_128, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC889_MBP_VREF] = { "alc889_mbp_vref", NULL, NULL, NULL,
		ALC_OP_MBP_VREF,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0 } },
	[R_ALC889_MP11_VREF] = { "alc889_mp11_vref", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x03, 0x03, { 0x18 } },
	[R_ALC889_MBA11_VREF] = { "alc889_mba11_vref", NULL, NULL, NULL,
		ALC_OP_MBP_VREF,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0x18 } },
	[R_ALC889_MBA21_VREF] = { "alc889_mba21_vref", NULL, NULL, NULL,
		ALC_OP_MBP_VREF,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0x18, 0x19 } },
	[R_ALC889_IMAC91_VREF] = { "alc889_imac91_vref", NULL, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x01, 0x01, { 0x18, 0x1a } },
	[R_ALC889_MP41_VREF] = { "alc889_mp41_vref", NULL, NULL, NULL,
		ALC_OP_MBP_VREF,
		ALC_LED_NONE, ALC_LED_NONE, 0x03, 0x03, { 0 } },
	[R_ALC888VD_MIC_100VREF] = { "alc888vd_mic_100vref", NULL, alc_verbs_40, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC886_EAPD] = { "alc886_eapd", NULL, alc_verbs_41, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC889_FRONT_HP_NO_PRESENCE] = { "alc889_front_hp_no_presence", alc_pins_129, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC1220_GB_X570] = { "alc1220_gb_x570", NULL, NULL, alc_dacs_16,
		ALC_OP_GB_X570,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC1220_CLEVO_P950] = { "alc1220_clevo_p950", NULL, NULL, alc_dacs_16,
		ALC_OP_CLEVO_P950,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC889_CD] = { "alc889_cd", alc_pins_130, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_ABIT_AW9D_MAX] = { "alc882_abit_aw9d_max", alc_pins_131, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC1220_CLEVO_PB51ED_PINS] = { "alc1220_clevo_pb51ed_pins", alc_pins_1, NULL, alc_dacs_16,
		ALC_OP_CLEVO_P950 | ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC898_CLEVO_P775TM1] = { "alc898_clevo_p775tm1", NULL, alc_verbs_37, NULL,
		ALC_OP_CLEVO_P775,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC1220_CLEVO_PB51ED] = { "alc1220_clevo_pb51ed", NULL, NULL, alc_dacs_16,
		ALC_OP_CLEVO_P950 | ALC_OP_HS_MODE | ALC_OP_HS_NO_HPMIC,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC883_EAPD] = { "alc883_eapd", NULL, alc_verbs_42, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC882_LENOVO_Y530] = { "alc882_lenovo_y530", alc_pins_132, NULL, NULL,
		0,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
	[R_ALC889_COEF] = { "alc889_coef", NULL, NULL, NULL,
		ALC_OP_ALC889_COEF,
		ALC_LED_NONE, ALC_LED_NONE, 0x00, 0x00, { 0 } },
};
/* ALC2xx boards by PCI (or codec) subsystem id.  First match wins, so
 * masked entries come after the exact ones they would shadow. */
static const struct hda_fixup alc269_quirks[] = {
	ALC_Q(0x1025, 0x0283, "Acer TravelMate 8371", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x029b, "Acer 1810TZ", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x0349, "Acer AOD260", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x047c, "Acer AC700", R_ALC269_ACER_AC700),
	ALC_Q(0x1025, 0x072d, "Acer Aspire V5-571G", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x1025, 0x0740, "Acer AO725", R_ALC271_HP_GATE_MIC_JACK),
	ALC_Q(0x1025, 0x0742, "Acer AO756", R_ALC271_HP_GATE_MIC_JACK),
	ALC_Q(0x1025, 0x0762, "Acer Aspire E1-472", R_ALC271_HP_GATE_MIC_JACK_E1_572),
	ALC_Q(0x1025, 0x0775, "Acer Aspire E1-572", R_ALC271_HP_GATE_MIC_JACK_E1_572),
	ALC_Q(0x1025, 0x079b, "Acer Aspire V5-573G", R_ALC282_ASPIRE_V5_PINS),
	ALC_Q(0x1025, 0x080d, "Acer Aspire V5-122P", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x1025, 0x0840, "Acer Aspire E1", R_ALC269VB_ASPIRE_E1_COEF),
	ALC_Q(0x1025, 0x0943, "Acer Aspire V3-572G", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x1025, 0x100c, "Acer Aspire E5-574G", R_ALC255_ACER_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1025, 0x101c, "Acer Veriton N2510G", R_ALC269_LIFEBOOK),
	ALC_Q(0x1025, 0x102b, "Acer Aspire C24-860", R_ALC286_ACER_AIO_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1065, "Acer Aspire C20-820", R_ALC269VC_ACER_HEADSET_MIC),
	ALC_Q(0x1025, 0x106d, "Acer Cloudbook 14", R_ALC283_CHROME_BOOK),
	ALC_Q(0x1025, 0x1094, "Acer Aspire E5-575T", R_ALC255_ACER_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1025, 0x1099, "Acer Aspire E5-523G", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x110e, "Acer Aspire ES1-432", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1166, "Acer Veriton N4640G", R_ALC269_LIFEBOOK),
	ALC_Q(0x1025, 0x1167, "Acer Veriton N6640G", R_ALC269_LIFEBOOK),
	ALC_Q(0x1025, 0x1177, "Acer Predator G9-593", R_ALC255_PREDATOR_SUBWOOFER),
	ALC_Q(0x1025, 0x1178, "Acer Predator G9-593", R_ALC255_PREDATOR_SUBWOOFER),
	ALC_Q(0x1025, 0x1246, "Acer Predator Helios 500", R_ALC299_PREDATOR_SPK),
	ALC_Q(0x1025, 0x1247, "Acer vCopperbox", R_ALC269VC_ACER_VCOPPERBOX_PINS),
	ALC_Q(0x1025, 0x1248, "Acer Veriton N4660G", R_ALC269VC_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1269, "Acer SWIFT SF314-54", R_ALC256_ACER_HEADSET_MIC),
	ALC_Q(0x1025, 0x126a, "Acer Swift SF114-32", R_ALC256_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x128f, "Acer Veriton Z6860G", R_ALC286_ACER_AIO_HEADSET_MIC),
	ALC_Q(0x1025, 0x1290, "Acer Veriton Z4860G", R_ALC286_ACER_AIO_HEADSET_MIC),
	ALC_Q(0x1025, 0x1291, "Acer Veriton Z4660G", R_ALC286_ACER_AIO_HEADSET_MIC),
	ALC_Q(0x1025, 0x129c, "Acer SWIFT SF314-55", R_ALC256_ACER_HEADSET_MIC),
	ALC_Q(0x1025, 0x129d, "Acer SWIFT SF313-51", R_ALC256_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1300, "Acer SWIFT SF314-56", R_ALC256_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1308, "Acer Aspire Z24-890", R_ALC286_ACER_AIO_HEADSET_MIC),
	ALC_Q(0x1025, 0x132a, "Acer TravelMate B114-21", R_ALC233_ACER_HEADSET_MIC),
	ALC_Q(0x1025, 0x1330, "Acer TravelMate X514-51T", R_ALC255_ACER_HEADSET_MIC),
	ALC_Q(0x1025, 0x1360, "Acer Aspire A115", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x141f, "Acer Spin SP513-54N", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x142b, "Acer Swift SF314-42", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1430, "Acer TravelMate B311R-31", R_ALC256_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1466, "Acer Aspire A515-56", R_ALC255_ACER_HEADPHONE_AND_MIC),
	ALC_Q(0x1025, 0x151e, "Acer Nitro AN515-45", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x1534, "Acer Predator PH315-54", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1539, "Acer Nitro 5 AN515-57", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x159c, "Acer Nitro 5 AN515-58", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x1597, "Acer Nitro 5 AN517-55", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x159e, "Acer Nitro 5 AN515-46", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x160e, "Acer PT316-51S", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x1616, "Acer Aspire A515-57", R_ALC256_ACER_COLDBOOT),
	ALC_Q(0x1025, 0x161f, "Acer S40-54", R_ALC256_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1025, 0x1640, "Acer Aspire A315-44P", R_ALC256_ACER_SFG16_MICMUTE_LED),
	ALC_Q(0x1025, 0x166c, "Acer Predator PH16-71", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x1679, "Acer Nitro 16 AN16-41", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x169a, "Acer Swift SFG16", R_ALC256_ACER_SFG16_MICMUTE_LED),
	ALC_Q(0x1025, 0x171e, "Acer Nitro ANV15-51", R_ALC245_ACER_MICMUTE_LED),
	ALC_Q(0x1025, 0x172c, "Acer Nitro ANV16-41", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x1731, "Acer Predator PHN16-72", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1025, 0x173a, "Acer Swift SFG14-73", R_ALC245_ACER_MICMUTE_LED),
	ALC_Q(0x1025, 0x173b, "Acer Swift SFG16-72", R_ALC245_ACER_MICMUTE_LED),
	ALC_Q(0x1025, 0x1758, "Acer Nitro ANV15-41", R_ALC245_ACER_MICMUTE_LED),
	ALC_Q(0x1025, 0x1826, "Acer Helios ZPC", R_ALC287_PREDATOR_SPK_CS35L41_I2C_2),
	ALC_Q(0x1025, 0x182c, "Acer Helios ZPD", R_ALC287_PREDATOR_SPK_CS35L41_I2C_2),
	ALC_Q(0x1025, 0x1844, "Acer Helios ZPS", R_ALC287_PREDATOR_SPK_CS35L41_I2C_2),
	ALC_Q(0x1025, 0x1909, "Acer Nitro ANV16-42", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1028, 0x0470, "Dell M101z", R_ALC269_DELL_M101Z),
	ALC_Q(0x1028, 0x053c, "Dell Latitude E5430", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x054b, "Dell XPS one 2710", R_ALC275_DELL_XPS),
	ALC_Q(0x1028, 0x05bd, "Dell Latitude E6440", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x05be, "Dell Latitude E6540", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x05ca, "Dell Latitude E7240", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x05cb, "Dell Latitude E7440", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x05da, "Dell Vostro 5460", R_ALC290_SUBWOOFER),
	ALC_Q(0x1028, 0x05f4, "Dell", R_ALC269_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x05f5, "Dell", R_ALC269_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x05f6, "Dell", R_ALC269_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0604, "Dell Venue 11 Pro 7130", R_ALC283_DELL_HP_RESUME),
	ALC_Q(0x1028, 0x0615, "Dell Vostro 5470", R_ALC290_SUBWOOFER_HSJACK),
	ALC_Q(0x1028, 0x0616, "Dell Vostro 5470", R_ALC290_SUBWOOFER_HSJACK),
	ALC_Q(0x1028, 0x062c, "Dell Latitude E5550", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x062e, "Dell Latitude E7450", R_ALC292_DELL_E7X),
	ALC_Q(0x1028, 0x0638, "Dell Inspiron 5439", R_ALC290_MONO_SPEAKERS_HSJACK),
	ALC_Q(0x1028, 0x064a, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x064b, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0665, "Dell XPS 13", R_ALC288_DELL_XPS_13),
	ALC_Q(0x1028, 0x0669, "Dell Optiplex 9020m", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x069a, "Dell Vostro 5480", R_ALC290_SUBWOOFER_HSJACK),
	ALC_Q(0x1028, 0x06c7, "Dell", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x06d9, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x06da, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x06db, "Dell", R_ALC293_DISABLE_AAMIX_MULTIJACK),
	ALC_Q(0x1028, 0x06dd, "Dell", R_ALC293_DISABLE_AAMIX_MULTIJACK),
	ALC_Q(0x1028, 0x06de, "Dell", R_ALC293_DISABLE_AAMIX_MULTIJACK),
	ALC_Q(0x1028, 0x06df, "Dell", R_ALC293_DISABLE_AAMIX_MULTIJACK),
	ALC_Q(0x1028, 0x06e0, "Dell", R_ALC293_DISABLE_AAMIX_MULTIJACK),
	ALC_Q(0x1028, 0x0706, "Dell Inspiron 7559", R_ALC256_DELL_INSPIRON_7559_SUBWOOFER),
	ALC_Q(0x1028, 0x0725, "Dell Inspiron 3162", R_ALC255_DELL_SPK_NOISE),
	ALC_Q(0x1028, 0x0738, "Dell Precision 5820", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1028, 0x075c, "Dell XPS 27 7760", R_ALC298_SPK_VOLUME),
	ALC_Q(0x1028, 0x075d, "Dell AIO", R_ALC298_SPK_VOLUME),
	ALC_Q(0x1028, 0x0798, "Dell Inspiron 17 7000 Gaming", R_ALC256_DELL_INSPIRON_7559_SUBWOOFER),
	ALC_Q(0x1028, 0x07b0, "Dell Precision 7520", R_ALC295_DISABLE_DAC3),
	ALC_Q(0x1028, 0x080c, "Dell WYSE", R_ALC225_DELL_WYSE_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x084b, "Dell", R_ALC274_DELL_AIO_LINEOUT_VERB),
	ALC_Q(0x1028, 0x084e, "Dell", R_ALC274_DELL_AIO_LINEOUT_VERB),
	ALC_Q(0x1028, 0x0871, "Dell Precision 3630", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1028, 0x0872, "Dell Precision 3630", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1028, 0x0873, "Dell Precision 3930", R_ALC255_DUMMY_LINEOUT_VERB),
	ALC_Q(0x1028, 0x0879, "Dell Latitude 5420 Rugged", R_ALC269_DELL4_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x08ad, "Dell WYSE AIO", R_ALC225_DELL_WYSE_AIO_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x08ae, "Dell WYSE NB", R_ALC225_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0935, "Dell", R_ALC274_DELL_AIO_LINEOUT_VERB),
	ALC_Q(0x1028, 0x097d, "Dell Precision", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x097e, "Dell Precision", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x098d, "Dell Precision", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1028, 0x09bf, "Dell Precision", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1028, 0x0a2e, "Dell", R_ALC236_DELL_AIO_HEADSET_MIC),
	ALC_Q(0x1028, 0x0a30, "Dell", R_ALC236_DELL_AIO_HEADSET_MIC),
	ALC_Q(0x1028, 0x0a38, "Dell Latitude 7520", R_ALC269_DELL4_MIC_NO_PRESENCE_QUIET),
	ALC_Q(0x1028, 0x0a58, "Dell", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1028, 0x0a61, "Dell XPS 15 9510", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x0a62, "Dell Precision 5560", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x0a9d, "Dell Latitude 5430", R_ALC269_DELL4_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0a9e, "Dell Latitude 5430", R_ALC269_DELL4_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0b19, "Dell XPS 15 9520", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x0b1a, "Dell Precision 5570", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x0b27, "Dell", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0b28, "Dell", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0b37, "Dell Inspiron 16 Plus 7620 2-in-1", R_ALC295_DELL_INSPIRON_TOP_SPEAKERS),
	ALC_Q(0x1028, 0x0b71, "Dell Inspiron 16 Plus 7620", R_ALC295_DELL_INSPIRON_TOP_SPEAKERS),
	ALC_Q(0x1028, 0x0beb, "Dell XPS 15 9530 (2023)", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0c03, "Dell Precision 5340", R_ALC269_DELL4_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c0b, "Dell Oasis 14 RPL-P", R_ALC289_RTK_AMP_DUAL_SPK),
	ALC_Q(0x1028, 0x0c0d, "Dell Oasis", R_ALC289_RTK_AMP_DUAL_SPK),
	ALC_Q(0x1028, 0x0c0e, "Dell Oasis 16", R_ALC289_RTK_AMP_DUAL_SPK),
	ALC_Q(0x1028, 0x0c19, "Dell Precision 3340", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c1a, "Dell Precision 3340", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c1b, "Dell Precision 3440", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c1c, "Dell Precision 3540", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c1d, "Dell Precision 3440", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c1e, "Dell Precision 3540", R_ALC255_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0c28, "Dell Inspiron 16 Plus 7630", R_ALC295_DELL_INSPIRON_TOP_SPEAKERS),
	ALC_Q(0x1028, 0x0c4d, "Dell", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0c90, "Alienware x16 R2", R_ALC289_DUAL_SPK),
	ALC_Q(0x1028, 0x0c94, "Dell Polaris 3 metal", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0c96, "Dell Polaris 2in1", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cbd, "Dell Oasis 13 CS MTL-U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cbe, "Dell Oasis 13 2-IN-1 MTL-U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cbf, "Dell Oasis 13 Low Weight MTU-L", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cc0, "Dell Oasis 13", R_ALC289_RTK_AMP_DUAL_SPK),
	ALC_Q(0x1028, 0x0cc1, "Dell Oasis 14 MTL-H/U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cc2, "Dell Oasis 14 2-in-1 MTL-H/U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cc3, "Dell Oasis 14 Low Weight MTL-U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cc4, "Dell Oasis 16 MTL-H/U", R_ALC289_DELL_CS35L41_SPI_2),
	ALC_Q(0x1028, 0x0cc5, "Dell Oasis 14", R_ALC289_RTK_AMP_DUAL_SPK),
	ALC_Q(0x1028, 0x0d56, "Dell Pro Tower QCT1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x0d58, "Dell Pro Micro QCM1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x0d5a, "Dell Pro Slim QCS1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x0e6b, "Dell Pro Tower QCT1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x0e6c, "Dell Pro Slim QCS1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x0e6d, "Dell Pro Micro QCM1255", R_ALC236_DELL_HP_POP_NOISE),
	ALC_Q(0x1028, 0x164a, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x164b, "Dell", R_ALC293_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x1586, "HP", R_ALC269_HP_MUTE_LED_MIC2),
	ALC_Q(0x103c, 0x18e6, "HP", R_ALC269_HP_GPIO_LED),
	ALC_Q(0x103c, 0x2164, "HP Pavilion 15 Notebook PC", R_ALC269_LIMIT_INT_MIC_BOOST_MUTE_LED),
	ALC_Q(0x103c, 0x218b, "HP", R_ALC269_LIMIT_INT_MIC_BOOST_MUTE_LED),
	ALC_Q(0x103c, 0x21f9, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2210, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2214, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x221b, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x221c, "HP EliteBook 755 G2", R_ALC280_HP_HEADSET_MIC),
	ALC_Q(0x103c, 0x2221, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2225, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2236, "HP", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x2237, "HP", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x2238, "HP", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x2239, "HP", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x224b, "HP", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x2253, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2254, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2255, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2256, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2257, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2259, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x225a, "HP", R_ALC269_HP_DOCK_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x225f, "HP", R_ALC280_HP_GPIO2_MIC_HOTKEY),
	ALC_Q(0x103c, 0x2260, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2263, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2264, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2265, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2268, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x226a, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x226b, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x226e, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2271, "HP", R_ALC286_HP_GPIO_LED),
	ALC_Q(0x103c, 0x2272, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2272, "HP", R_ALC280_HP_DOCK_PINS),
	ALC_Q(0x103c, 0x2273, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2273, "HP", R_ALC280_HP_DOCK_PINS),
	ALC_Q(0x103c, 0x2278, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x227f, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2282, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x228b, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x228e, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x229e, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22b2, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22b7, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22bf, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22c4, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22c5, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22c7, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22c8, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22cf, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x22db, "HP", R_ALC280_HP_9480M),
	ALC_Q(0x103c, 0x22dc, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x22fb, "HP", R_ALC269_HP_GPIO_MIC1_LED),
	ALC_Q(0x103c, 0x2334, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2335, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2336, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2337, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_Q(0x103c, 0x2b5e, "HP 288 Pro G2 MT", R_ALC221_HP_288PRO_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x802e, "HP Z240 SFF", R_ALC221_HP_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x802f, "HP Z240", R_ALC221_HP_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x8077, "HP", R_ALC256_HP_HEADSET_MIC),
	ALC_Q(0x103c, 0x8158, "HP", R_ALC256_HP_HEADSET_MIC),
	ALC_Q(0x103c, 0x820d, "HP Pavilion 15", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x8256, "HP", R_ALC221_HP_FRONT_MIC),
	ALC_Q(0x103c, 0x8259, "HP OMEN 15-ax202nf", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x827e, "HP x360", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x827f, "HP x360", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x82bf, "HP G3 mini", R_ALC221_HP_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x82c0, "HP G3 mini premium", R_ALC221_HP_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x83b9, "HP Spectre x360", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x841c, "HP Pavilion 15-CK0xx", R_ALC295_HP_X360),
	ALC_CQ(0x103c, 0x8486, "HP Pavilion x360", R_ALC295_HP_PAVILION_X360),
	ALC_Q(0x103c, 0x8497, "HP Envy x360", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x84a6, "HP 250 G7 Notebook PC", R_ALC269_HP_LINE1_MIC1_LED),
	ALC_Q(0x103c, 0x84ae, "HP 15-db0403ng", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x84da, "HP OMEN dc0019-ur", R_ALC295_HP_OMEN),
	ALC_Q(0x103c, 0x84e7, "HP Pavilion 15", R_ALC295_HP_X360),
	ALC_Q(0x103c, 0x8519, "HP Spectre x360 15-df0xxx", R_ALC285_HP_SPECTRE_X360),
	ALC_Q(0x103c, 0x8536, "HP ProBook 430 G6", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8537, "HP ProBook 440 G6", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8548, "HP EliteBook x360 830 G6", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x854a, "HP EliteBook 830 G6", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x854d, "HP EliteBook 840 G6", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x856a, "HP Pavilion 15-cs1xxx", R_ALC295_HP_X360),
	ALC_CQ(0x103c, 0x85ba, "HP Pavilion AiO 24", R_ALC225_HP_AIO_SPK),
	ALC_Q(0x103c, 0x85c6, "HP Pavilion x360 Convertible 14-dy1xxx", R_ALC295_HP_MUTE_LED_COEFBIT11),
	ALC_Q(0x103c, 0x85de, "HP Envy x360 13-ar0xxx", R_ALC285_HP_ENVY_X360),
	ALC_Q(0x103c, 0x85f0, "HP Laptop 15-dw0xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x85f1, "HP Laptop 15-dw1xxx", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x85f3, "HP 250 G8 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8603, "HP Omen 17-cb0xxx", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x860c, "HP ZBook 17 G6", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x860f, "HP ZBook 15 G6", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x861f, "HP Elite Dragonfly G1", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x864f, "HP Laptop 15-dy0xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x869d, "HP", R_ALC236_HP_MUTE_LED),
	ALC_Q(0x103c, 0x86ad, "HP ENVY 13-aq1xxx", R_ALC285_HP_ENVY_13AQ),
	ALC_Q(0x103c, 0x86c1, "HP Laptop 15-da3001TU", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x86c7, "HP Envy AiO 32", R_ALC274_HP_ENVY_GPIO),
	ALC_Q(0x103c, 0x86c8, "HP Laptop 14s-dr1xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x86e7, "HP Spectre x360 15-eb0xxx", R_ALC285_HP_SPECTRE_X360_EB1),
	ALC_Q(0x103c, 0x863e, "HP Spectre x360 15-df1xxx", R_ALC285_HP_SPECTRE_X360_DF1),
	ALC_Q(0x103c, 0x86e8, "HP Spectre x360 15-eb0xxx", R_ALC285_HP_SPECTRE_X360_EB1),
	ALC_Q(0x103c, 0x86f9, "HP Spectre x360 13-aw0xxx", R_ALC285_HP_SPECTRE_X360_MUTE_LED),
	ALC_Q(0x103c, 0x8706, "HP Laptop 15s-eq1xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8716, "HP Elite Dragonfly G2 Notebook PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8720, "HP EliteBook x360 1040 G8 Notebook PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8724, "HP EliteBook 850 G7", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8728, "HP EliteBook 840 G7", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8729, "HP", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8730, "HP ProBook 445 G7", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8735, "HP ProBook 435 G7", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8736, "HP", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8756, "HP ENVY Laptop 13-ba0xxx", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x8760, "HP EliteBook 8{4,5}5 G7", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x876e, "HP ENVY x360 Convertible 13-ay0xxx", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x877a, "HP", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x877d, "HP", R_ALC236_HP_MUTE_LED),
	ALC_Q(0x103c, 0x8780, "HP ZBook Fury 17 G7 Mobile Workstation", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8783, "HP ZBook Fury 15 G7 Mobile Workstation", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8786, "HP OMEN 15", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x8787, "HP OMEN 15", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x8788, "HP OMEN 15", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x87b2, "HP Pavilion Gaming Laptop 15-ec1xxx", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x87b7, "HP Laptop 14-fq0xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x87c8, "HP", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87cb, "HP Pavilion 15-eg0xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87cc, "HP Pavilion 15-eg0xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87d3, "HP Laptop 15-gw0xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x87df, "HP ProBook 430 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87e5, "HP ProBook 440 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87e7, "HP ProBook 450 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87f1, "HP ProBook 630 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87f2, "HP ProBook 640 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87f4, "HP", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87f5, "HP", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x87f6, "HP Spectre x360 14", R_ALC245_HP_X360_AMP),
	ALC_Q(0x103c, 0x87f7, "HP Spectre x360 14", R_ALC245_HP_X360_AMP),
	ALC_Q(0x103c, 0x87fd, "HP Laptop 14-dq2xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x87fe, "HP Laptop 15s-fq2xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8805, "HP ProBook 650 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x880d, "HP EliteBook 830 G8 Notebook PC", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8811, "HP Spectre x360 15-eb1xxx", R_ALC285_HP_SPECTRE_X360_EB1),
	ALC_Q(0x103c, 0x8812, "HP Spectre x360 15-eb1xxx", R_ALC285_HP_SPECTRE_X360_EB1),
	ALC_Q(0x103c, 0x881d, "HP 250 G8 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x881e, "HP Laptop 15s-du3xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8846, "HP EliteBook 850 G8 Notebook PC", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8847, "HP EliteBook x360 830 G8 Notebook PC", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x884b, "HP EliteBook 840 Aero G8 Notebook PC", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x884c, "HP EliteBook 840 G8 Notebook PC", R_ALC285_HP_GPIO_LED),
	ALC_CQ(0x103c, 0x885b, "HP Spectre x360 14-ea", R_ALC245_HP_X360_AMP),
	ALC_Q(0x103c, 0x8862, "HP ProBook 445 G8 Notebook PC", R_ALC236_HP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x103c, 0x8863, "HP ProBook 445 G8 Notebook PC", R_ALC236_HP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x103c, 0x886d, "HP ZBook Fury 17.3 Inch G8 Mobile Workstation PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8870, "HP ZBook Fury 15.6 Inch G8 Mobile Workstation PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8873, "HP ZBook Studio 15.6 Inch G8 Mobile Workstation PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x887a, "HP Laptop 15s-eq2xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x887c, "HP Laptop 14s-fq1xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x888a, "HP ENVY x360 Convertible 15-eu0xxx", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x888d, "HP ZBook Power 15.6 inch G8 Mobile Workstation PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8890, "HP Elite Dragonfly Max G2 Notebook PC", R_ALC285_HP_GPIO_AMP_INIT),
	ALC_Q(0x103c, 0x8895, "HP EliteBook 855 G8 Notebook PC", R_ALC285_HP_SPEAKERS_MICMUTE_LED),
	ALC_Q(0x103c, 0x8896, "HP EliteBook 855 G8 Notebook PC", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x8898, "HP EliteBook 845 G8 Notebook PC", R_ALC285_HP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x103c, 0x88b3, "HP ENVY x360 Convertible 15-es0xxx", R_ALC245_HP_ENVY_X360_MUTE_LED),
	ALC_Q(0x103c, 0x88d0, "HP Pavilion 15-eh1xxx (mainboard 88D0)", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x88d1, "HP Pavilion 15-eh1xxx (mainboard 88D1)", R_ALC245_HP_MUTE_LED_V1_COEFBIT),
	ALC_Q(0x103c, 0x88dd, "HP Pavilion 15z-ec200", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x88eb, "HP Victus 16-e0xxx", R_ALC245_HP_MUTE_LED_V2_COEFBIT),
	ALC_Q(0x103c, 0x88ed, "HP Victus 16-e0xxx (MB 88ED)", R_ALC245_HP_MUTE_LED_V2_COEFBIT),
	ALC_Q(0x103c, 0x88ee, "HP Victus 16-e0xxx (MB 88EE)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8902, "HP OMEN 16", R_ALC285_HP_MUTE_LED),
	ALC_Q(0x103c, 0x890e, "HP 255 G8 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8919, "HP Pavilion Aero Laptop 13-be0xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x896d, "HP ZBook Firefly 16 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x896e, "HP EliteBook x360 830 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8971, "HP EliteBook 830 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8972, "HP EliteBook 840 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8973, "HP EliteBook 860 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8974, "HP EliteBook 840 Aero G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8975, "HP EliteBook x360 840 Aero G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x897d, "HP mt440 Mobile Thin Client U74", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8981, "HP Elite Dragonfly G3", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8987, "HP Pavilion 14-dv1001TU", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x898a, "HP Pavilion 15-eg100", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x898e, "HP EliteBook 835 G9", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x898f, "HP EliteBook 835 G9", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8991, "HP EliteBook 845 G9", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8992, "HP EliteBook 845 G9", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8994, "HP EliteBook 855 G9", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8995, "HP EliteBook 855 G9", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x89a0, "HP Laptop 15-dw4xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x89a4, "HP ProBook 440 G9", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89a6, "HP ProBook 450 G9", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89aa, "HP EliteBook 630 G9", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89ac, "HP EliteBook 640 G9", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89ae, "HP EliteBook 650 G9", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89c0, "HP ZBook Power 15.6 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89c3, "Zbook Studio G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89c6, "Zbook Fury 17 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x89ca, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x89d3, "HP EliteBook 645 G9 (MB 89D2)", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x89da, "HP Spectre x360 14t-ea100", R_ALC245_HP_SPECTRE_X360_EU0XXX),
	ALC_Q(0x103c, 0x89e7, "HP Elite x2 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a05, "HP Dragonfly Folio G3 2-in-1", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a06, "HP Dragonfly Folio G3 2-in-1", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a0e, "HP Pavilion Laptop 15-eh2xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a0f, "HP Pavilion 14-ec1xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a1b, "HP 255 15.6 inch G9 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8a1f, "HP Laptop 14s-dr5xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8a20, "HP Laptop 15s-fq5xxx", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8a25, "HP Victus 16-d1xxx (MB 8A25)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a26, "HP Victus 16-d1xxx (MB 8A26)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a28, "HP Envy 13", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a29, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a2a, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a2b, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a2c, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a2d, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a2e, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a30, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a31, "HP ENVY x360 2-in-1 Laptop 15-ey0xxx", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8a34, "HP Pavilion x360 2-in-1 Laptop 14-ek0xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a36, "HP Pavilion Plus 14-eh0xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a3d, "HP Victus 15-fb0xxx (MB 8A3D)", R_ALC245_HP_MUTE_LED_V2_COEFBIT),
	ALC_Q(0x103c, 0x8a4f, "HP Victus 15-fa0xxx (MB 8A4F)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a50, "HP Victus 15-fa0xxx (MB 8A50)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8a6b, "HP Pavilion All-in-One Desktop 27-ca1xxx", R_ALC274_HP_VERBS),
	ALC_Q(0x103c, 0x8a6e, "HP EDNA 360", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8a74, "HP ProBook 440 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a75, "HP ProBook 450 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a76, "HP ProBook 440 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a77, "HP ProBook 450 G8 Notebook PC", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8a78, "HP Dev One", R_ALC285_HP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x103c, 0x8aa0, "HP ProBook 440 G9 (MB 8A9E)", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8aa3, "HP ProBook 450 G9 (MB 8AA1)", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8aa8, "HP EliteBook 640 G9 (MB 8AA6)", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8aab, "HP EliteBook 650 G9 (MB 8AA9)", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ab8, "HP EliteBook 830 G8 Notebook PC (MB 8AB8)", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ab9, "HP EliteBook 840 G8 (MB 8AB8)", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8abb, "HP ZBook Firefly 14 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ad1, "HP EliteBook 840 14 inch G9 Notebook PC", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ad2, "HP EliteBook 860 16 inch G9 Notebook PC", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ad8, "HP 800 G9", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b0f, "HP Elite mt645 G7 Mobile Thin Client U81", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b2f, "HP 255 15.6 inch G10 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8b34, "HP 250 15.6 inch G10 Notebook PC", R_ALC236_HP_MUTE_LED_COEFBIT2),
	ALC_Q(0x103c, 0x8b3a, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8b3f, "HP mt440 Mobile Thin Client U91", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b42, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b43, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b44, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b45, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b46, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b47, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b59, "HP Elite mt645 G7 Mobile Thin Client U89", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b5d, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b5e, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b5f, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b63, "HP Elite Dragonfly 13.5 inch G4", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b65, "HP ProBook 455 15.6 inch G10 Notebook PC", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b66, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b70, "HP EliteBook 835 G10", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b72, "HP EliteBook 845 G10", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b74, "HP EliteBook 845W G10", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b77, "HP ElieBook 865 G10", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8b7a, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b7d, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b87, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b8a, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b8b, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b8d, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b8f, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b92, "HP", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8b96, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8b97, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8ba9, "HP Omen 16-wd0xxx", R_ALC245_HP_MUTE_LED_V1_COEFBIT),
	ALC_Q(0x103c, 0x8bb1, "HP Victus 15-fa1xxx (MB 8BB1)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8bb3, "HP Slim OMEN", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8bb4, "HP Slim OMEN", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8bb6, "HP Laptop 15-fd0039nt", R_ALC236_HP_15_FD0XXX),
	ALC_Q(0x103c, 0x8bbe, "HP Victus 16-r0xxx (MB 8BBE)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8bc7, "HP Pavilion 15-eh3174nw", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8bc8, "HP Victus 15-fa1xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8bcd, "HP Omen 16-xd0xxx", R_ALC245_HP_MUTE_LED_V1_COEFBIT),
	ALC_Q(0x103c, 0x8bd4, "HP Victus 16-s0xxx (MB 8BD4)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8bd6, "HP Pavilion Aero Laptop 13z-be200", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8bdd, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8bde, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8bdf, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be0, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be1, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be2, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be3, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be5, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be6, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be7, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be8, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8be9, "HP Envy x360 2-in-1 Laptop 15-fh0xxx", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8bf0, "HP", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8bfd, "HP Pavilion 15-eg3xxx", R_ALC287_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c15, "HP Spectre x360 2-in-1 Laptop 14-eu0xxx", R_ALC245_HP_SPECTRE_X360_EU0XXX),
	ALC_Q(0x103c, 0x8c16, "HP Spectre x360 2-in-1 Laptop 16-aa0xxx", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8c17, "HP Spectre x360 2-in-1 Laptop 16-aa0xxx", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8c21, "HP Pavilion Plus Laptop 14-ey0XXX", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x8c2d, "HP Victus 15-fa1xxx (MB 8C2D)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8c2f, "HP Victus 15-fb2xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8c30, "HP Victus 15-fb1xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8c3f, "HP Victus 15-fa1xxx (MB 8C3F)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8c46, "HP EliteBook 830 G11", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c47, "HP EliteBook 840 G11", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c48, "HP EliteBook 860 G11", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c49, "HP Elite x360 830 2-in-1 G11", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c4d, "HP Omen", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c4e, "HP Omen", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c4f, "HP Envy 15", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c50, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c51, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c52, "HP EliteBook 1040 G11", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c53, "HP Elite x360 1040 2-in-1 G11", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c66, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c67, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c68, "HP Envy 17", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c6a, "HP Envy 16", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8c70, "HP EliteBook 835 G11", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c71, "HP EliteBook 845 G11", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c72, "HP EliteBook 865 G11", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c7b, "HP ProBook 445 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c7c, "HP ProBook 445 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c7d, "HP ProBook 465 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c7e, "HP ProBook 465 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c7f, "HP EliteBook 645 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c80, "HP EliteBook 645 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c81, "HP EliteBook 665 G11", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c89, "HP ProBook 460 G11", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c8a, "HP EliteBook 630", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c8c, "HP EliteBook 660", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c8d, "HP ProBook 440 G11", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c8e, "HP ProBook 460 G11", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c8f, "HP EliteBook 630 G11", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c90, "HP EliteBook 640", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c91, "HP EliteBook 660", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8c96, "HP", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c97, "HP ZBook", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8c99, "HP Victus 16-r1xxx (MB 8C99)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8c9c, "HP Victus 16-s1xxx (MB 8C9C)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8ca1, "HP ZBook Power", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ca2, "HP ZBook Power", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ca4, "HP ZBook Fury", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ca7, "HP ZBook Fury", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8caf, "HP Elite mt645 G8 Mobile Thin Client", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8cbc, "HP Pavilion Laptop 16-ag0xxx", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x8cbd, "HP Pavilion Aero Laptop 13-bg0xxx", R_ALC245_HP_X360_MUTE_LEDS),
	ALC_Q(0x103c, 0x8cdd, "HP Spectre", R_ALC245_HP_SPECTRE_X360_EU0XXX),
	ALC_Q(0x103c, 0x8cde, "HP OmniBook Ultra Flip Laptop 14t", R_ALC245_HP_SPECTRE_X360_EU0XXX),
	ALC_Q(0x103c, 0x8cdf, "HP SnowWhite", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ce0, "HP SnowWhite", R_ALC287_CS35L41_I2C_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8cf5, "HP ZBook Studio 16", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d01, "HP ZBook Power 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d07, "HP Victus 15-fb2xxx (MB 8D07)", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8d18, "HP EliteStudio 8 AIO", R_ALC274_HP_AIO_BIND_DACS),
	ALC_Q(0x103c, 0x8d84, "HP EliteBook X G1i", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d85, "HP EliteBook 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d86, "HP Elite X360 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d8c, "HP EliteBook 13 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d8d, "HP Elite X360 13 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d8e, "HP EliteBook 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d8f, "HP EliteBook 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d90, "HP EliteBook 16 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d91, "HP ZBook Firefly 14 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8d92, "HP ZBook Firefly 16 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8dcd, "HP Victus 15-fa2xxx", R_ALC245_HP_MUTE_LED_COEFBIT),
	ALC_Q(0x103c, 0x8d9b, "HP 17 Turbine OmniBook 7 UMA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8d9c, "HP 17 Turbine OmniBook 7 DIS", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8d9d, "HP 17 Turbine OmniBook X UMA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8d9e, "HP 17 Turbine OmniBook X DIS", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8d9f, "HP 14 Cadet (x360)", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8da0, "HP 16 Clipper OmniBook 7(X360)", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8da1, "HP 16 Clipper OmniBook X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8da7, "HP 14 Enstrom OmniBook X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8da8, "HP 16 Piston OmniBook X", R_ALC287_CS35L41_I2C_2_HP_MUTE_LEDS),
	ALC_Q(0x103c, 0x8dc9, "HP Laptop 15-fc0xxx", R_ALC236_HP_DMIC),
	ALC_Q(0x103c, 0x8dd4, "HP EliteStudio 8 AIO", R_ALC274_HP_AIO_BIND_DACS),
	ALC_Q(0x103c, 0x8dd7, "HP Laptop 15-fd0xxx", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8de8, "HP Gemtree", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8de9, "HP Gemtree", R_ALC245_CS35L41_SPI_2_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8dec, "HP EliteBook 640 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8ded, "HP EliteBook 640 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8dee, "HP EliteBook 660 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8def, "HP EliteBook 660 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8df0, "HP EliteBook 630 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8df1, "HP EliteBook 630 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8df7, "HP Z66 G6", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8dfb, "HP EliteBook 6 G1a 14", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8dfc, "HP EliteBook 645 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8dfd, "HP EliteBook 6 G1a 16", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8dfe, "HP EliteBook 665 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8e0d, "HP EliteBook 6 G1a 14 (AD3Q9ET#UUG)", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8e11, "HP Trekker", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e12, "HP Trekker", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e13, "HP Trekker", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e14, "HP ZBook Firefly 14 G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e15, "HP ZBook Firefly 14 G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e16, "HP ZBook Firefly 14 G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e17, "HP ZBook Firefly 14 G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e18, "HP ZBook Firefly 14 G12A", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e19, "HP ZBook Firefly 14 G12A", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e1a, "HP ZBook Firefly 14 G12A", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e1b, "HP EliteBook G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e1c, "HP EliteBook G12", R_ALC245_HP_ZBOOK_FIREFLY_G12A),
	ALC_Q(0x103c, 0x8e1d, "HP ZBook X Gli 16 G12", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8e2c, "HP EliteBook 16 G12", R_ALC285_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8e36, "HP 14 Enstrom OmniBook X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e37, "HP 16 Piston OmniBook X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e3a, "HP Agusta", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e3b, "HP Agusta", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e43, "HP Laptop 15-fd2xxx", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8e60, "HP OmniBook 7 Laptop 16-bh0xxx", R_ALC245_CS35L41_I2C_2_MUTE_LED),
	ALC_Q(0x103c, 0x8e61, "HP Trekker ", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e62, "HP Trekker ", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e75, "HP Trekker G7JC", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e8a, "HP NexusX", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8e9c, "HP 16 Clipper OmniBook X X360", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e9d, "HP 17 Turbine OmniBook X UMA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8e9e, "HP 17 Turbine OmniBook X UMA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8eb6, "HP Abe A6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8eb8, "HP Abe A6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ec1, "HP 200 G2i", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ec4, "HP Bantie I6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ec5, "HP Bantie I6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ece, "HP Abe I6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ecf, "HP Abe I6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ed2, "HP Abe I6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ed5, "HP EliteBook 8 Flip G2i 13", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8ed6, "HP EliteBook 8 G2i 13", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8ed7, "HP EliteBook 8 G2i 14", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8ed8, "HP EliteBook 8 G2i 16", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8ed9, "HP ZBook Firefly 14W", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8eda, "HP ZBook Firefly 16W", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8ee4, "HP Bantie A6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ee5, "HP Bantie A6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8ee7, "HP Abe A6U", R_ALC236_HP_MUTE_LED_MICMUTE_GPIO),
	ALC_Q(0x103c, 0x8f07, "HP Agusta G7KX", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8f0c, "HP ZBook X G2i 16W", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f0e, "HP ZBook X G2i 16W", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f2d, "HP Auster 14", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8f2e, "HP Auster 14", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8f37, "HP EliteBook 6 G2i", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f38, "HP EliteBook 6 G2i", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f39, "HP EliteBook 6 G2i", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f3a, "HP EliteBook 6 G2i", R_ALC236_HP_GPIO_LED),
	ALC_Q(0x103c, 0x8f3c, "HP EliteBook 6 G2a", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8f3d, "HP EliteBook 6 G2a", R_ALC236_HP_MUTE_LED_MICMUTE_VREF),
	ALC_Q(0x103c, 0x8f40, "HP ZBook 8 G2a 14", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8f41, "HP ZBook 8 G2a 16", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8f42, "HP ZBook 8 G2a 14W", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8f57, "HP Trekker G7JC", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x103c, 0x8f62, "HP ZBook 8 G2a 16W", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8f7e, "HP Messi", R_ALC245_HP_TAS2781_I2C_MUTE_LED),
	ALC_Q(0x103c, 0x8f94, "HP ZBook 8 G2a 14", R_ALC245_HP_TAS2781_I2C_MUTE_LED_INVERTED),
	ALC_Q(0x103c, 0x8f95, "HP ZBook 8 G2a 16", R_ALC245_HP_TAS2781_I2C_MUTE_LED_INVERTED),
	ALC_Q(0x1043, 0x1024, "ASUS Zephyrus G14 2025", R_ALC285_ASUS_GA403U_HEADSET_MIC),
	ALC_Q(0x1043, 0x1032, "ASUS VivoBook X513EA", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1034, "ASUS GU605C", R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x103e, "ASUS X540SA", R_ALC256_ASUS_MIC),
	ALC_Q(0x1043, 0x103f, "ASUS TX300", R_ALC282_ASUS_TX300),
	ALC_Q(0x1043, 0x1044, "ASUS GA403UM", R_ALC285_ASUS_GA403U_HEADSET_MIC),
	ALC_Q(0x1043, 0x1054, "ASUS G614FH/FM/FP", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x106d, "Asus K53BE", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1043, 0x106f, "ASUS VivoBook X515UA", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1074, "ASUS G614PH/PM/PP", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x10a1, "ASUS UX391UA", R_ALC294_ASUS_SPK),
	ALC_Q(0x1043, 0x10a4, "ASUS TP3407SA", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x10c0, "ASUS X540SA", R_ALC256_ASUS_MIC),
	ALC_Q(0x1043, 0x10c4, "ASUS Vivobook S14 S5406SA", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1043, 0x10d0, "ASUS X540LA/X540LJ", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x10d3, "ASUS K6500ZC", R_ALC294_ASUS_SPK),
	ALC_Q(0x1043, 0x1154, "ASUS TP3607SH", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x115d, "Asus 1015E", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1043, 0x1194, "ASUS UM3406KA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x11c0, "ASUS X556UR", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1204, "ASUS Strix G615JHR_JMR_JPR", R_ALC287_TXNW2781_I2C_ASUS),
	ALC_Q(0x1043, 0x1214, "ASUS Strix G615LH_LM_LP", R_ALC287_TXNW2781_I2C_ASUS),
	ALC_Q(0x1043, 0x125e, "ASUS Q524UQK", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1271, "ASUS X430UN", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1290, "ASUS X441SA", R_ALC233_EAPD_COEF_AND_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1294, "ASUS B3405CVA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x12a0, "ASUS X441UV", R_ALC233_EAPD_COEF_AND_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x12a3, "Asus N7691ZM", R_ALC269_ASUS_N7601ZM),
	ALC_Q(0x1043, 0x12af, "ASUS UX582ZS", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x12b4, "ASUS B3405CCA / P3405CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x12e0, "ASUS X541SA", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x12f0, "ASUS X541UV", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1313, "Asus K42JZ", R_ALC269VB_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1314, "ASUS GA605K", R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x1384, "ASUS RC73XA", R_ALC287_TXNW2781_I2C_ASUS),
	ALC_Q(0x1043, 0x1394, "ASUS RC73YA", R_ALC287_TXNW2781_I2C_ASUS),
	ALC_Q(0x1043, 0x13b0, "ASUS Z550SA", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1427, "Asus Zenbook UX31E", R_ALC269VB_ASUS_ZENBOOK),
	ALC_Q(0x1043, 0x1433, "ASUS GX650PY/PZ/PV/PU/PYV/PZV/PIV/PVV", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1454, "ASUS PM3406CKA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1460, "Asus VivoBook 15", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1463, "Asus GA402X/GA402N", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1473, "ASUS GU604VI/VC/VE/VG/VJ/VQ/VU/VV/VY/VZ", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1483, "ASUS GU603VQ/VU/VV/VJ/VI", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1493, "ASUS GV601VV/VU/VJ/VQ/VI", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x14d3, "ASUS G614JY/JZ/JG", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x14e3, "ASUS G513PI/PU/PV", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x14f2, "ASUS VivoBook X515JA", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1503, "ASUS G733PY/PZ/PZV/PYV", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1514, "ASUS ROG Flow Z13 GZ302EAC", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1517, "Asus Zenbook UX31A", R_ALC269VB_ASUS_ZENBOOK_UX31A),
	ALC_Q(0x1043, 0x1533, "ASUS GV302XA/XJ/XQ/XU/XV/XI", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1573, "ASUS GZ301VV/VQ/VU/VJ/VA/VC/VE/VVC/VQC/VUC/VJC/VEC/VCC", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1584, "ASUS UM3406GA ", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1602, "ASUS ROG Strix SCAR 15", R_ALC285_ASUS_G533Z_PINS),
	ALC_Q(0x1043, 0x1652, "ASUS ROG Zephyrus Do 15 SE", R_ALC289_ASUS_ZEPHYRUS_DUAL_SPK),
	ALC_Q(0x1043, 0x1662, "ASUS GV301QH", R_ALC294_ASUS_DUAL_SPK),
	ALC_Q(0x1043, 0x1663, "ASUS GU603ZI/ZJ/ZQ/ZU/ZV", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1664, "ASUS ExpertBook PM3406CHA", R_ALC256_ASUS_PM3406CHA),
	ALC_Q(0x1043, 0x1683, "ASUS UM3402YAR", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x16a3, "ASUS UX3402VA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x16b2, "ASUS GU603", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x16d3, "ASUS UX5304VA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x16e3, "ASUS UX50", R_ALC269_STEREO_DMIC),
	ALC_Q(0x1043, 0x16f3, "ASUS UX7602VI/BZ", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1740, "ASUS UX430UA", R_ALC295_ASUS_DACS),
	ALC_Q(0x1043, 0x17d1, "ASUS UX431FL", R_ALC294_ASUS_DUAL_SPK),
	ALC_Q(0x1043, 0x17f3, "ROG Ally NR2301L/X", R_ALC294_ASUS_ALLY),
	ALC_Q(0x1043, 0x1863, "ASUS UX6404VI/VV", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1881, "ASUS Zephyrus S/M", R_ALC294_ASUS_GX502_PINS),
	ALC_Q(0x1043, 0x18b1, "Asus MJ401TA", R_ALC256_ASUS_HEADSET_MIC),
	ALC_Q(0x1043, 0x18d3, "ASUS UM3504DA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x18f1, "Asus FX505DT", R_ALC256_ASUS_HEADSET_MIC),
	ALC_Q(0x1043, 0x194e, "ASUS UX563FD", R_ALC294_ASUS_HPE),
	ALC_Q(0x1043, 0x1970, "ASUS UX550VE", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x197e, "ASUS VivoBook X509DAP", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1982, "ASUS B1400CEPE", R_ALC256_ASUS_HPE),
	ALC_Q(0x1043, 0x19ce, "ASUS B9450FA", R_ALC294_ASUS_HPE),
	ALC_Q(0x1043, 0x19e1, "ASUS UX581LV", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x1043, 0x19f4, "ASUS UM3405GA", R_ALC294_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1a13, "Asus G73Jw", R_ALC269_ASUS_G73JW),
	ALC_Q(0x1043, 0x1a63, "ASUS UX3405MA", R_ALC294_ASUS_SPI_HEADSET_MIC),
	ALC_Q(0x1043, 0x1a83, "ASUS UM5302LA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1a8e, "ASUS G712LWS", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x1043, 0x1a8f, "ASUS UX582ZS", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1b11, "ASUS UX431DA", R_ALC294_ASUS_COEF_1B),
	ALC_Q(0x1043, 0x1b13, "ASUS U41SV/GA403U", R_ALC285_ASUS_GA403U_HEADSET_MIC),
	ALC_Q(0x1043, 0x1b93, "ASUS G614JVR/JIR", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1bbd, "ASUS Z550MA", R_ALC255_ACER_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1c03, "ASUS UM3406HA", R_ALC294_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1c23, "Asus X55U", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1043, 0x1c33, "ASUS UX5304MA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1c43, "ASUS UX8406MA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1c62, "ASUS GU603", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1c63, "ASUS GU605M", R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x1c80, "ASUS VivoBook TP401", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1c92, "ASUS ROG Strix G15", R_ALC285_ASUS_G533Z_PINS),
	ALC_Q(0x1043, 0x1c9f, "ASUS G614JU/JV/JI", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1caf, "ASUS G634JY/JZ/JI/JG", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x1ccd, "ASUS X555UB", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1ccf, "ASUS G814JU/JV/JI", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1cdf, "ASUS G814JY/JZ/JG", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1cef, "ASUS G834JY/JZ/JI/JG", R_ALC285_ASUS_I2C_HEADSET_MIC),
	ALC_Q(0x1043, 0x1d1f, "ASUS G713PI/PU/PV/PVN", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1d42, "ASUS Zephyrus G14 2022", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1d4e, "ASUS TM420", R_ALC256_ASUS_HPE),
	ALC_Q(0x1043, 0x1da2, "ASUS UP6502ZA/ZD", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1df3, "ASUS UM5606WA", R_ALC294_BASS_SPEAKER_15),
	ALC_Q(0x1043, 0x1264, "ASUS UM5606KA", R_ALC294_BASS_SPEAKER_15),
	ALC_Q(0x1043, 0x1e02, "ASUS UX3402ZA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1e10, "ASUS VivoBook X507UAR", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1e3e, "ASUS VivoBook M515DA/X515DAP", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x1043, 0x1e11, "ASUS Zephyrus G15", R_ALC289_ASUS_GA502),
	ALC_Q(0x1043, 0x1e12, "ASUS UM3402", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1e1f, "ASUS Vivobook 15 X1504VAP", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1043, 0x1e51, "ASUS Zephyrus M15", R_ALC294_ASUS_GU502_PINS),
	ALC_Q(0x1043, 0x1e5e, "ASUS ROG Strix G513", R_ALC294_ASUS_G513_PINS),
	ALC_Q(0x1043, 0x1e63, "ASUS H7606W", R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x1e83, "ASUS GA605W", R_ALC285_ASUS_GU605_SPI_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x1e8e, "ASUS Zephyrus G15", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1e93, "ASUS ExpertBook B9403CVAR", R_ALC294_ASUS_HPE),
	ALC_Q(0x1043, 0x1eb3, "ASUS Ally RC72LA", R_ALC287_ASUS_ALLY_X),
	ALC_Q(0x1043, 0x1ed3, "ASUS HN7306W", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1ee2, "ASUS UM6702RA/RC", R_ALC285_ASUS_I2C_SPEAKER2_TO_DAC1),
	ALC_Q(0x1043, 0x1c52, "ASUS Zephyrus G15 2022", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1f11, "ASUS Zephyrus G14", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1f12, "ASUS UM5302", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1f1f, "ASUS H7604JI/JV/J3D", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1f62, "ASUS UX7602ZM", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1f63, "ASUS P5405CSA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x1f92, "ASUS ROG Flow X16", R_ALC289_ASUS_GA401),
	ALC_Q(0x1043, 0x1fb3, "ASUS ROG Flow Z13 GZ302EA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3011, "ASUS B5605CVA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3030, "ASUS ZN270IE", R_ALC256_ASUS_AIO_GPIO2),
	ALC_Q(0x1043, 0x3061, "ASUS B3405CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3071, "ASUS B5405CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x30c1, "ASUS B3605CCA / P3605CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x30d1, "ASUS B5405CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x30e1, "ASUS B5605CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x31d0, "ASUS Zen AIO 27 Z272SD_A272SD", R_ALC274_ASUS_ZEN_AIO_27),
	ALC_Q(0x1043, 0x31e1, "ASUS B5605CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x31f1, "ASUS B3605CCA", R_ALC294_ASUS_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3391, "ASUS PM3606CKA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3601, "ASUS PM5406CGA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3611, "ASUS PM5606CGA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3701, "ASUS P5406CCA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3711, "ASUS P5606CCA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3a20, "ASUS G614JZR", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x3a30, "ASUS G814JVR/JIR", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x3a40, "ASUS G814JZR", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x3a50, "ASUS G834JYR/JZR", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x3a60, "ASUS G634JYR/JZR", R_ALC285_ASUS_SPI_REAR_SPEAKERS),
	ALC_Q(0x1043, 0x3d78, "ASUS GA603KH", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3d88, "ASUS GA603KM", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3e00, "ASUS G814FH/FM/FP", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3e20, "ASUS G814PH/PM/PP", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3e30, "ASUS TP3607SA", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3ee0, "ASUS Strix G815_JHR_JMR_JPR", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3ef0, "ASUS Strix G635LR_LW_LX", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3f00, "ASUS Strix G815LH_LM_LP", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3f10, "ASUS Strix G835LR_LW_LX", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3f20, "ASUS Strix G615LR_LW", R_ALC287_TXNW2781_I2C_ASUS),
	ALC_Q(0x1043, 0x3f30, "ASUS Strix G815LR_LW", R_ALC287_TAS2781_I2C),
	ALC_Q(0x1043, 0x3fd0, "ASUS B3605CVA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x3ff0, "ASUS B5405CVA", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1043, 0x831a, "ASUS P901", R_ALC269_STEREO_DMIC),
	ALC_Q(0x1043, 0x834a, "ASUS S101", R_ALC269_STEREO_DMIC),
	ALC_Q(0x1043, 0x8398, "ASUS P1005", R_ALC269_STEREO_DMIC),
	ALC_Q(0x1043, 0x83ce, "ASUS P1005", R_ALC269_STEREO_DMIC),
	ALC_Q(0x1043, 0x8516, "ASUS X101CH", R_ALC269_ASUS_X101),
	ALC_Q(0x1043, 0x88f4, "ASUS NUC14LNS", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x104d, 0x9073, "Sony VAIO", R_ALC275_SONY_VAIO_GPIO2),
	ALC_Q(0x104d, 0x907b, "Sony VAIO", R_ALC275_SONY_HWEQ),
	ALC_Q(0x104d, 0x9084, "Sony VAIO", R_ALC275_SONY_HWEQ),
	ALC_Q(0x104d, 0x9099, "Sony VAIO S13", R_ALC275_SONY_DISABLE_AAMIX),
	ALC_Q(0x104d, 0x90b5, "Sony VAIO Pro 11", R_ALC286_ACER_AIO_MIC_NO_PRESENCE),
	ALC_Q(0x104d, 0x90b6, "Sony VAIO Pro 13", R_ALC286_ACER_AIO_MIC_NO_PRESENCE),
	ALC_Q(0x10cf, 0x1475, "Lifebook", R_ALC269_LIFEBOOK),
	ALC_Q(0x10cf, 0x159f, "Lifebook E780", R_ALC269_LIFEBOOK_NO_HP_TO_LINEOUT),
	ALC_Q(0x10cf, 0x15dc, "Lifebook T731", R_ALC269_LIFEBOOK_HP_PIN),
	ALC_Q(0x10cf, 0x1629, "Lifebook U7x7", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x10cf, 0x1757, "Lifebook E752", R_ALC269_LIFEBOOK_HP_PIN),
	ALC_Q(0x10cf, 0x1845, "Lifebook U904", R_ALC269_LIFEBOOK_EXTMIC),
	ALC_Q(0x10ec, 0x10f2, "Intel Reference board", R_ALC700_INTEL_REFERENCE),
	ALC_Q(0x10ec, 0x111e, "Higole F9B", R_ALC290_MONO_SPEAKERS),
	ALC_Q(0x10ec, 0x118c, "Medion EE4254 MD62100", R_ALC256_MEDION_HEADSET_NO_PRESENCE),
	ALC_Q(0x10ec, 0x119e, "Positivo SU C1400", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x10ec, 0x11bc, "VAIO VJFE-IL", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x10ec, 0x1230, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10ec, 0x124c, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10ec, 0x1252, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10ec, 0x1254, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10ec, 0x12cc, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10ec, 0x12f6, "Intel Reference board", R_ALC295_CHROME_BOOK),
	ALC_Q(0x10f7, 0x8338, "Panasonic CF-SZ6", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x1414, 0x9c20, "Microsoft Surface Pro 2/3", R_ALC288_SURFACE_SWAP_DACS),
	ALC_Q(0x144d, 0xc109, "Samsung Ativ book 9 (NP900X3G)", R_ALC269_INV_DMIC),
	ALC_Q(0x144d, 0xc169, "Samsung Notebook 9 Pen (NP930SBE-K01US)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc176, "Samsung Notebook 9 Pro (NP930MBE-K04US)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc188, "Samsung Galaxy Book Flex (NT950QCT-A38A)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc189, "Samsung Galaxy Book Flex (NT950QCG-X716)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc18a, "Samsung Galaxy Book Ion (NP930XCJ-K01US)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc1ac, "Samsung Galaxy Book2 Pro 360 (NP950QED)", R_ALC298_SAMSUNG_AMP_V2_2_AMPS),
	ALC_Q(0x144d, 0xc1a3, "Samsung Galaxy Book Pro (NP935XDB-KC1SE)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc1a4, "Samsung Galaxy Book Pro 360 (NT935QBD)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc1a6, "Samsung Galaxy Book Pro 360 (NP930QBD)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc740, "Samsung Ativ book 8 (NP870Z5G)", R_ALC269_NO_SHUTUP),
	ALC_Q(0x144d, 0xc812, "Samsung Notebook Pen S (NT950SBE-X58)", R_ALC298_SAMSUNG_AMP),
	ALC_CQ(0x144d, 0xc824, "Samsung ELECTRONICS (750XBE/730XBE)", R_ALC298_SAMSUNG_HEADPHONE_VERY_QUIET),
	ALC_Q(0x144d, 0xc830, "Samsung Galaxy Book Ion (NT950XCJ-X716A)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc832, "Samsung Galaxy Book Flex Alpha (NP730QCJ)", R_ALC256_SAMSUNG_HEADPHONE_VERY_QUIET),
	ALC_Q(0x144d, 0xc876, "Samsung 730QED (NP730QED-KA2US)", R_ALC256_SAMSUNG_HEADPHONE_VERY_QUIET),
	ALC_Q(0x144d, 0xca03, "Samsung Galaxy Book2 Pro 360 (NP930QED)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xca06, "Samsung Galaxy Book3 360 (NP730QFG)", R_ALC298_SAMSUNG_HEADPHONE_VERY_QUIET),
	ALC_Q(0x144d, 0xc868, "Samsung Galaxy Book2 Pro (NP930XED)", R_ALC298_SAMSUNG_AMP),
	ALC_Q(0x144d, 0xc870, "Samsung Galaxy Book2 Pro (NP950XED)", R_ALC298_SAMSUNG_AMP_V2_2_AMPS),
	ALC_Q(0x144d, 0xc872, "Samsung Galaxy Book2 Pro (NP950XEE)", R_ALC298_SAMSUNG_AMP_V2_2_AMPS),
	ALC_Q(0x144d, 0xc886, "Samsung Galaxy Book3 Pro (NP964XFG)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x144d, 0xc902, "Samsung Galaxy Book5 360 (NP750QHA)", R_ALC256_SAMSUNG_HEADPHONE_VERY_QUIET),
	ALC_Q(0x144d, 0xc1ca, "Samsung Galaxy Book3 Pro 360 (NP960QFG)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x144d, 0xc1cb, "Samsung Galaxy Book3 Pro 360 (NP965QFG)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x144d, 0xc1cc, "Samsung Galaxy Book3 Ultra (NT960XFH)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x1458, 0x900e, "Gigabyte G5 KF5 (2023)", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1458, 0xfa53, "Gigabyte BXBT-2807", R_ALC283_HEADSET_MIC),
	ALC_Q(0x1462, 0xb120, "MSI Cubi MS-B120", R_ALC283_HEADSET_MIC),
	ALC_Q(0x1462, 0xb171, "Cubi N 8GL (MS-B171)", R_ALC283_HEADSET_MIC),
	ALC_Q(0x152d, 0x1082, "Quanta NL3", R_ALC269_LIFEBOOK),
	ALC_Q(0x152d, 0x1262, "Huawei NBLB-WAX9N", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1558, 0x0353, "Clevo V35[05]SN[CDE]Q", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x1323, "Clevo N130ZU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x1325, "Clevo N15[01][CW]U", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x1401, "Clevo L140[CZ]U", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x1403, "Clevo N140CU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x1404, "Clevo N150CU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x14a1, "Clevo L141MU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x2624, "Clevo L240TU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x28c1, "Clevo V370VND", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1558, 0x35a1, "Clevo V3[56]0EN[CDE]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x35b1, "Clevo V3[57]0WN[MNP]Q", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x4018, "Clevo NV40M[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x4019, "Clevo NV40MZ", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x4020, "Clevo NV40MB", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x4041, "Clevo NV4[15]PZ", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x40a1, "Clevo NL40GU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x40c1, "Clevo NL40[CZ]U", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x40d1, "Clevo NL41DU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5015, "Clevo NH5[58]H[HJK]Q", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5017, "Clevo NH7[79]H[HJK]Q", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50a3, "Clevo NJ51GU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50b3, "Clevo NK50S[BEZ]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50b6, "Clevo NK50S5", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50b8, "Clevo NK50SZ", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50d5, "Clevo NP50D5", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50e1, "Clevo NH5[58]HPQ", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50e2, "Clevo NH7[79]HPQ", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50f0, "Clevo NH50A[CDF]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50f2, "Clevo NH50E[PR]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50f3, "Clevo NH58DPQ", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50f5, "Clevo NH55EPY", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x50f6, "Clevo NH55DPQ", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5101, "Clevo S510WU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5157, "Clevo W517GU1", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x51a1, "Clevo NS50MU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x51b1, "Clevo NS50AU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x51b3, "Clevo NS70AU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5630, "Clevo NP50RNJS", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x5700, "Clevo X560WN[RST]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x6480, "Clevo V6xxAW", R_ALC245_CLEVO_NOISY_MIC),
	ALC_Q(0x1558, 0x70a1, "Clevo NB70T[HJK]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x70b3, "Clevo NK70SB", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x70f2, "Clevo NH79EPY", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x70f3, "Clevo NH77DPQ", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x70f4, "Clevo NH77EPY", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x70f6, "Clevo NH77DPQ-Y", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x7716, "Clevo NS50PU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x7717, "Clevo NS70PU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x7718, "Clevo L140PU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x7724, "Clevo L140AU", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8228, "Clevo NR40BU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8520, "Clevo NH50D[CD]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8521, "Clevo NH77D[CD]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8535, "Clevo NH50D[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8536, "Clevo NH79D[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8550, "Clevo NH[57][0-9][ER][ACDH]Q", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8551, "Clevo NH[57][0-9][ER][ACDH]Q", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8560, "Clevo NH[57][0-9][ER][ACDH]Q", R_ALC269_HEADSET_MIC),
	ALC_Q(0x1558, 0x8561, "Clevo NH[57][0-9][ER][ACDH]Q", R_ALC269_HEADSET_MIC),
	ALC_Q(0x1558, 0x8562, "Clevo NH[57][0-9]RZ[Q]", R_ALC269_DMIC),
	ALC_Q(0x1558, 0x8668, "Clevo NP50B[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x866d, "Clevo NP5[05]PN[HJK]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x867c, "Clevo NP7[01]PNP", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x867d, "Clevo NP7[01]PN[HJK]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8680, "Clevo NJ50LU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8686, "Clevo NH50[CZ]U", R_ALC256_MIC_NO_PRESENCE_AND_RESUME),
	ALC_Q(0x1558, 0x8a20, "Clevo NH55DCQ-Y", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8a51, "Clevo NH70RCQ-Y", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x8d50, "Clevo NH55RCQ-M", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x951d, "Clevo N950T[CDF]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x9600, "Clevo N960K[PR]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x961d, "Clevo N960S[CDF]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0x971d, "Clevo N970T[CDF]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa500, "Clevo NL5[03]RU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa554, "VAIO VJFH52", R_ALC269_VAIO_VJFH52_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa559, "VAIO RPL", R_ALC256_VAIO_RPL_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa600, "Clevo NL50NU", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa650, "Clevo NP[567]0SN[CD]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa671, "Clevo NP70SN[CDE]", R_ALC256_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xa741, "Clevo V54x_6x_TNE", R_ALC245_CLEVO_NOISY_MIC),
	ALC_Q(0x1558, 0xa743, "Clevo V54x_6x_TU", R_ALC245_CLEVO_NOISY_MIC),
	ALC_Q(0x1558, 0xa763, "Clevo V54x_6x_TU", R_ALC245_CLEVO_NOISY_MIC),
	ALC_Q(0x1558, 0xb018, "Clevo NP50D[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xb019, "Clevo NH77D[BE]Q", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xb022, "Clevo NH77D[DC][QW]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xc018, "Clevo NP50D[BE]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xc019, "Clevo NH77D[BE]Q", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x1558, 0xc022, "Clevo NH77[DC][QW]", R_ALC293_SYSTEM76_MIC_NO_PRESENCE),
	ALC_Q(0x17aa, 0x1036, "Lenovo P520", R_ALC233_LENOVO_MULTI_CODECS),
	ALC_Q(0x17aa, 0x1048, "ThinkCentre Station", R_ALC623_LENOVO_THINKSTATION_P340),
	ALC_Q(0x17aa, 0x20f2, "Thinkpad SL410/510", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x215e, "Thinkpad L512", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x21b8, "Thinkpad Edge 14", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x21ca, "Thinkpad L412", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x21e9, "Thinkpad Edge 15", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x21f3, "Thinkpad T430", R_ALC269_LENOVO_DOCK),
	ALC_Q(0x17aa, 0x21f6, "Thinkpad T530", R_ALC269_LENOVO_DOCK_LIMIT_BOOST),
	ALC_Q(0x17aa, 0x21fa, "Thinkpad X230", R_ALC269_LENOVO_DOCK),
	ALC_Q(0x17aa, 0x21fb, "Thinkpad T430s", R_ALC269_LENOVO_DOCK),
	ALC_Q(0x17aa, 0x2203, "Thinkpad X230 Tablet", R_ALC269_LENOVO_DOCK),
	ALC_Q(0x17aa, 0x2208, "Thinkpad T431s", R_ALC269_LENOVO_DOCK),
	ALC_Q(0x17aa, 0x220c, "Thinkpad T440s", R_ALC292_TPT440),
	ALC_Q(0x17aa, 0x220e, "Thinkpad T440p", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2210, "Thinkpad T540p", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2211, "Thinkpad W541", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2212, "Thinkpad T440", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2214, "Thinkpad X240", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2215, "Thinkpad", R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x2218, "Thinkpad X1 Carbon 2nd", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2223, "ThinkPad T550", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x2226, "ThinkPad X250", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x222d, "ThinkPad P50/P70 class (ALC298)", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x222e, "ThinkPad P50", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x2231, "Thinkpad T560", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x2233, "Thinkpad", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x2234, "Thinkpad ICE-1", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x2245, "Thinkpad T470", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x2246, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x2247, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x2249, "Thinkpad", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x224b, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x224c, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x224d, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x225c, "Lenovo ThinkPad X1 Carbon 6th Gen", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x225d, "Thinkpad T480", R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x2288, "Thinkpad X390", R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK),
	ALC_Q(0x17aa, 0x2292, "Thinkpad X1 Carbon 7th", R_ALC285_THINKPAD_HEADSET_JACK),
	ALC_Q(0x17aa, 0x22be, "Thinkpad X1 Carbon 8th", R_ALC285_THINKPAD_HEADSET_JACK),
	ALC_Q(0x17aa, 0x22c1, "Thinkpad P1 Gen 3", R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK),
	ALC_Q(0x17aa, 0x22c2, "Thinkpad X1 Extreme Gen 3", R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK),
	ALC_Q(0x17aa, 0x22f1, "Thinkpad", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x22f2, "Thinkpad", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x22f3, "Thinkpad", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x2316, "Thinkpad P1 Gen 6", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x2317, "Thinkpad P1 Gen 6", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x2318, "Thinkpad Z13 Gen2", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x2319, "Thinkpad Z16 Gen2", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x231a, "Thinkpad Z16 Gen2", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x231e, "Thinkpad", R_ALC287_LENOVO_THKPAD_WH_ALC1318),
	ALC_Q(0x17aa, 0x231f, "Thinkpad", R_ALC287_LENOVO_THKPAD_WH_ALC1318),
	ALC_Q(0x17aa, 0x2326, "Hera2", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x30bb, "ThinkCentre AIO", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x30e2, "ThinkCentre AIO", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x310c, "ThinkCentre Station", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x17aa, 0x3111, "ThinkCentre Station", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x17aa, 0x312a, "ThinkCentre Station", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x17aa, 0x312f, "ThinkCentre Station", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x17aa, 0x313c, "ThinkCentre Station", R_ALC294_LENOVO_MIC_LOCATION),
	ALC_Q(0x17aa, 0x3151, "ThinkCentre Station", R_ALC283_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3176, "ThinkCentre Station", R_ALC283_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3178, "ThinkCentre Station", R_ALC283_HEADSET_MIC),
	ALC_Q(0x17aa, 0x31af, "ThinkCentre Station", R_ALC623_LENOVO_THINKSTATION_P340),
	ALC_Q(0x17aa, 0x3341, "Lenovo ThinkCentre M90 Gen4", R_ALC233_LENOVO_GPIO2_MIC_HOTKEY),
	ALC_Q(0x17aa, 0x3342, "Lenovo ThinkCentre M90 Gen4", R_ALC233_LENOVO_GPIO2_MIC_HOTKEY),
	ALC_Q(0x17aa, 0x3343, "Lenovo ThinkCentre M70 Gen4", R_ALC233_LENOVO_GPIO2_MIC_HOTKEY),
	ALC_Q(0x17aa, 0x3344, "Lenovo ThinkCentre M70 Gen4", R_ALC233_LENOVO_GPIO2_MIC_HOTKEY),
	ALC_Q(0x17aa, 0x334b, "Lenovo ThinkCentre M70 Gen5", R_ALC283_HEADSET_MIC),
	ALC_Q(0x17aa, 0x334f, "Lenovo ThinkCentre M90a Gen5", R_ALC233_LENOVO_GPIO2_MIC_HOTKEY),
	ALC_Q(0x17aa, 0x3384, "ThinkCentre M90a PRO", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3386, "ThinkCentre M90a Gen6", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3387, "ThinkCentre M70a Gen6", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3801, "Lenovo Yoga9 14IAP7", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x3802, "DuetITL 2021", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_Q(0x17aa, 0x3802, "Lenovo Yoga Pro 9 14IRP8", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x380b, "Lenovo Yoga Slim 9 14ILL10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x38d5, "Lenovo Yoga Pro 9 16IMH9", R_ALC287_TAS2781_I2C),
	ALC_CQ(0x17aa, 0x38d6, "Lenovo Yoga Pro 9 16IMH9", R_ALC287_TAS2781_I2C),
	ALC_CQ(0x17aa, 0x3855, "Legion 7 16ITHG6", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3811, "Legion S7 15IMH05", R_ALC287_LEGION_15IMHG05_SPEAKERS),
	ALC_Q(0x17aa, 0x3813, "Legion 7i 15IMHG05", R_ALC287_LEGION_15IMHG05_SPEAKERS),
	ALC_Q(0x17aa, 0x3818, "Lenovo C940 / Yoga Duet 7", R_ALC298_LENOVO_C940_DUET7),
	ALC_Q(0x17aa, 0x3819, "Lenovo 13s Gen2 ITL", R_ALC287_13S_GEN2_SPEAKERS),
	ALC_CQ(0x17aa, 0x3820, "IdeaPad 330-17IKB 81DM", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x17aa, 0x3820, "Yoga Duet 7 13ITL6", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_Q(0x17aa, 0x3824, "Legion Y9000X 2020", R_ALC285_LEGION_Y9000X_SPEAKERS),
	ALC_Q(0x17aa, 0x3827, "Ideapad S740", R_ALC269_SKU_IGNORE),
	ALC_Q(0x17aa, 0x3834, "Lenovo IdeaPad Slim 9i 14ITL5", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_Q(0x17aa, 0x383d, "Legion Y9000X 2019", R_ALC285_LEGION_Y9000X_SPEAKERS),
	ALC_Q(0x17aa, 0x3843, "Lenovo Yoga 9i / Yoga Book 9i", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x38cf, "Lenovo Yoga Pro 7 14IMH9", R_ALC287_YOGA9_14IMH9_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3847, "Legion 7 16ACHG6", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x384a, "Lenovo Yoga 7 15ITL5", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_CQ(0x17aa, 0x38b1, "Lenovo Yoga Pro 7 14IRH8", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3852, "Lenovo Yoga 7 14ITL5", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_Q(0x17aa, 0x3853, "Lenovo Yoga 7 15ITL5", R_ALC287_YOGA7_14ITL_SPEAKERS),
	ALC_Q(0x17aa, 0x3855, "Legion 7 16ITHG6", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3862, "Lenovo IdeaPad Slim 3 15ABR8", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x3865, "Lenovo 13X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3866, "Lenovo 13X", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3869, "Lenovo Yoga7 14IAL7", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x386a, "Lenovo Yoga 7 16IAP7", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x386e, "Legion Y9000X 2022 IAH7", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x386e, "Yoga Pro 7 14ARP8", R_ALC285_YOGA_SPEAKER2_TO_DAC1),
	ALC_CQ(0x17aa, 0x38a7, "Legion Pro 7 16ARX8H", R_ALC287_TAS2781_I2C),
	ALC_CQ(0x17aa, 0x38a8, "Legion Pro 7 16ARX8H", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x386f, "Legion Pro 7i 16IAX7", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3870, "Lenovo Yoga 7 14ARB7", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3871, "Lenovo ThinkBook 16p Gen 3 ARH", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3874, "Legion 7i 16IAX7", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3877, "Lenovo Legion 7 Slim 16ARHA7", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x3878, "Lenovo Legion 7 Slim 16ARHA7", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x387d, "Yoga S780-16 pro Quad AAC", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x387e, "Yoga S780-16 pro Quad YC", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x387f, "Yoga S780-16 pro dual LX", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3880, "Yoga S780-16 pro dual YC", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3881, "YB9 dual power mode2 YC", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3882, "Lenovo Yoga Pro 7 14APH8", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3884, "Y780 YG DUAL", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3886, "Y780 VECO DUAL", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3891, "Lenovo Yoga Pro 7 14AHP9", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x38a5, "Y580P AMD dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38a7, "Y780P AMD YG dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38a8, "Y780P AMD VECO dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38a9, "Thinkbook 16P", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x38ab, "Thinkbook 16P", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_CQ(0x17aa, 0x391a, "Lenovo Yoga Slim 7 14AKP10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x38b4, "Legion Slim 7 16IRH8", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x391c, "Lenovo Yoga 7 2-in-1 14AKP10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x391d, "Lenovo Yoga 7 2-in-1 16AKP10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x38b5, "Legion Slim 7 16IRH8", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x38b6, "Legion Slim 7 16APH8", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x38b7, "Legion Slim 7 16APH8", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x38b8, "Yoga S780-14.5 proX AMD YC Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38b9, "Yoga S780-14.5 proX AMD LX Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38ba, "Yoga S780-14.5 Air AMD quad YC", R_ALC287_TAS2781_I2C),
	ALC_CQ(0x17aa, 0x3926, "Legion Pro 5 16ADR10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_CQ(0x17aa, 0x3927, "Legion R9000P ADR10", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x3928, "Legion R9000P ADR10", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x38bb, "Yoga S780-14.5 Air AMD quad AAC", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38be, "Yoga S980-14.5 proX YC Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38bf, "Yoga S980-14.5 proX LX Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38c3, "Y980 DUAL", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38c7, "Thinkbook 13x Gen 4", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x38c8, "Thinkbook 13x Gen 4", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x38cb, "Y790 YG DUAL", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38cd, "Y790 VECO DUAL", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38d2, "Lenovo Yoga 9 14IMH9", R_ALC287_YOGA9_14IMH9_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x38d3, "Yoga S990-16 Pro IMH YC Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38d4, "Yoga S990-16 Pro IMH VECO Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38d5, "Yoga S990-16 Pro IMH YC Quad", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38d6, "Yoga S990-16 Pro IMH VECO Quad", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38d7, "Lenovo Yoga 9 14IMH9", R_ALC287_YOGA9_14IMH9_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x38dc, "Lenovo ThinkBook G8", R_ALC298_LENOVO_C940_DUET7),
	ALC_Q(0x17aa, 0x38df, "Yoga Y990 Intel YC Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38e0, "Yoga Y990 Intel VECO Dual", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x38f8, "Yoga Book 9i", R_ALC287_TAS2781_I2C),
	ALC_CQ(0x17aa, 0x395b, "Lenovo Legion 7 15ASH11", R_ALC287_LENOVO_YOGA_PRO7),
	ALC_Q(0x17aa, 0x38f9, "Thinkbook 16P Gen5", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x38fa, "Thinkbook 16P Gen5", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x38fc, "Lenovo Yoga Pro 7 15ASH11", R_ALC287_LENOVO_YOGA_PRO7),
	ALC_Q(0x17aa, 0x38fd, "ThinkBook plus Gen5 Hybrid", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3902, "Lenovo E50-80", R_ALC269_DMIC_THINKPAD_ACPI),
	ALC_CQ(0x17aa, 0x3906, "Legion Pro 7i 16IAX10H / Y9000P IAX10", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x3907, "Legion Pro 7i 16IAX10H / Y9000P IAX10", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x390d, "Lenovo Yoga Pro 7 14ASP10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN_HEADSET),
	ALC_Q(0x17aa, 0x3911, "Lenovo Yoga Pro 7 14IAH10", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3912, "Lenovo Xiaoxin 14 GT", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3913, "Lenovo 145", R_ALC236_LENOVO_INV_DMIC),
	ALC_Q(0x17aa, 0x391f, "Yoga S990-16 pro Quad YC Quad", R_ALC287_TAS2781_I2C),
	ALC_Q(0x17aa, 0x3920, "Yoga S990-16 pro Quad VECO Quad", R_ALC287_YOGA9_SPEAKER2_TO_DAC1),
	ALC_Q(0x17aa, 0x3929, "Thinkbook 13x Gen 5", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_Q(0x17aa, 0x392b, "Thinkbook 13x Gen 5", R_ALC287_MG_RTKC_CSAMP_CS35L41_I2C_THINKPAD),
	ALC_CQ(0x17aa, 0x3936, "Legion R9000P ADR10H", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x3937, "Legion R9000P ADR10H", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x3938, "Legion Pro 7 16AFR10H", R_ALC245_CS35L41_SPI_2),
	ALC_CQ(0x17aa, 0x3939, "Legion Pro 7 16AFR10H", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x17aa, 0x393e, "Lenovo ThinkBook 14 G8+ IPH", R_ALC287_LENOVO_XPAD_HEADSET_JACK),
	ALC_CQ(0x17aa, 0x394c, "Lenovo Yoga Slim 7 14AGP11", R_ALC287_YOGA9_14IAP7_BASS_SPK_PIN),
	ALC_Q(0x17aa, 0x3977, "IdeaPad S210", R_ALC283_INT_MIC),
	ALC_Q(0x17aa, 0x3978, "Lenovo B50-70", R_ALC269_DMIC_THINKPAD_ACPI),
	ALC_Q(0x17aa, 0x3bf8, "Quanta FL1", R_ALC269_PCM_44K),
	ALC_Q(0x17aa, 0x5013, "Thinkpad", R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x501a, "Thinkpad", R_ALC283_INT_MIC),
	ALC_Q(0x17aa, 0x501e, "Thinkpad L440", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x5026, "Thinkpad", R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x5034, "Thinkpad T450", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x5036, "Thinkpad T450s", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x503c, "Thinkpad L450", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x504a, "ThinkPad X260", R_ALC292_TPT440_DOCK),
	ALC_Q(0x17aa, 0x504b, "Thinkpad", R_ALC293_LENOVO_SPK_NOISE),
	ALC_Q(0x17aa, 0x5050, "Thinkpad T560p", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x5051, "Thinkpad L460", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x5053, "Thinkpad T460", R_ALC292_TPT460),
	ALC_Q(0x17aa, 0x505d, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x505f, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x5062, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x508b, "Thinkpad X12 Gen 1", R_ALC287_LEGION_15IMHG05_SPEAKERS),
	ALC_Q(0x17aa, 0x5109, "Thinkpad", R_ALC269_THINKPAD_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x17aa, 0x511e, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x511f, "Thinkpad", R_ALC298_TPT470_DOCK),
	ALC_Q(0x17aa, 0x9e54, "LENOVO NB", R_ALC269_LENOVO_EAPD),
	ALC_Q(0x17aa, 0x9e56, "Lenovo ZhaoYang CF4620Z", R_ALC286_ACER_AIO_MIC_NO_PRESENCE),
	ALC_Q(0x1849, 0x0269, "Positivo Master C6400", R_ALC269VB_ASUS_ZENBOOK),
	ALC_Q(0x1849, 0x1233, "ASRock NUC Box 1100", R_ALC233_NO_AUDIO_JACK),
	ALC_Q(0x1849, 0x6256, "ASRock NUC Box 358H", R_ALC233_NO_AUDIO_JACK),
	ALC_Q(0x1849, 0xa233, "Positivo Master C6300", R_ALC269_HEADSET_MIC),
	ALC_Q(0x1854, 0x0440, "LG CQ6", R_ALC256_HEADPHONE_AMP_VOL),
	ALC_Q(0x1854, 0x0441, "LG CQ6 AIO", R_ALC256_HEADPHONE_AMP_VOL),
	ALC_Q(0x1854, 0x0488, "LG gram 16 (16Z90R)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x1854, 0x0489, "LG gram 16 (16Z90R-A)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x1854, 0x048a, "LG gram 17 (17ZD90R)", R_ALC298_SAMSUNG_AMP_V2_4_AMPS),
	ALC_Q(0x1854, 0x0490, "LG Gram Style 14 (14Z90RS)", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x1854, 0x0554, "LG gram 16 (16Z90TR)", R_ALC298_SAMSUNG_AMP_V2_2_AMPS),
	ALC_Q(0x19e5, 0x3204, "Huawei MACH-WX9", R_ALC256_HUAWEI_MACH_WX9_PINS),
	ALC_Q(0x19e5, 0x320f, "Huawei WRT-WX9 ", R_ALC256_ASUS_MIC_NO_PRESENCE),
	ALC_Q(0x19e5, 0x3212, "Huawei KLV-WX9 ", R_ALC256_ACER_HEADSET_MIC),
	ALC_Q(0x1a58, 0x2023, "Razer Blade 16 (2025)", R_ALC298_RAZER_BLADE16_2025_PINS),
	ALC_Q(0x1a58, 0x300e, "Razer Blade 16 (2025)", R_ALC298_RAZER_BLADE16_2025_PINS),
	ALC_Q(0x1b35, 0x1235, "CZC B20", R_ALC269_CZC_B20),
	ALC_Q(0x1b35, 0x1236, "CZC TMI", R_ALC269_CZC_TMI),
	ALC_Q(0x1b35, 0x1237, "CZC L101", R_ALC269_CZC_L101),
	ALC_Q(0x1b7d, 0xa831, "Ordissimo EVE2 ", R_ALC269VB_ORDISSIMO_EVE2),
	ALC_Q(0x1c06, 0x2013, "Lemote A1802", R_ALC269_LEMOTE_A1802),
	ALC_Q(0x1c06, 0x2015, "Lemote A190X", R_ALC269_LEMOTE_A190X),
	ALC_Q(0x1c6c, 0x122a, "Positivo N14AP7", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1c6c, 0x1251, "Positivo N14KP6-TG", R_ALC288_DELL1_MIC_NO_PRESENCE),
	ALC_Q(0x1d05, 0x1132, "TongFang PHxTxX1", R_ALC256_SET_COEF_DEFAULTS),
	ALC_Q(0x1d05, 0x1096, "TongFang GMxMRxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1100, "TongFang GKxNRxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1111, "TongFang GMxZGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1119, "TongFang GMxZGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1129, "TongFang GMxZGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1147, "TongFang GMxTGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x115c, "TongFang GMxTGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x121b, "TongFang GMxAGxx", R_ALC269_NO_SHUTUP),
	ALC_Q(0x1d05, 0x1387, "TongFang GMxIXxx", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x1409, "TongFang GMxIXxx", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x300f, "TongFang X6AR5xxY", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x3019, "TongFang X6FR5xxY", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x3031, "TongFang X6AR55xU", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x3033, "TongFang X6SP45xU", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x3034, "TongFang X6KK45xU", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d05, 0x30ba, "TongFang XxAF5xxx", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1d17, 0x3288, "Haier Boyue G42", R_ALC269VC_ACER_VCOPPERBOX_PINS),
	ALC_Q(0x1d19, 0x0006, "VAIO VJS131", R_ALC255_DELL_HEADSET_MIC),
	ALC_Q(0x1d72, 0x1602, "RedmiBook", R_ALC255_XIAOMI_HEADSET_MIC),
	ALC_Q(0x1d72, 0x1701, "XiaomiNotebook Pro", R_ALC221_HP_MIC_NO_PRESENCE),
	ALC_Q(0x1d72, 0x1901, "RedmiBook 14", R_ALC256_ASUS_HEADSET_MIC),
	ALC_Q(0x1d72, 0x1905, "Xiaomi Mi Laptop Pro 15", R_ALC256_XIAOMI_PRO15_RESUME),
	ALC_Q(0x1d72, 0x1945, "Redmi G", R_ALC256_ASUS_HEADSET_MIC),
	ALC_Q(0x1d72, 0x1947, "RedmiBook Air", R_ALC255_XIAOMI_HEADSET_MIC),
	ALC_Q(0x1e39, 0xca14, "MEDION NM14LNL", R_ALC233_MEDION_MTL_SPK),
	ALC_Q(0x1e50, 0x7007, "Positivo DN50E", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1e50, 0x7036, "Acer Gadget E10 ETBook", R_ALC233_WUJIE_SPEAKERS),
	ALC_Q(0x1e50, 0x7038, "Positivo DN140", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1ee7, 0x2078, "HONOR BRB-X M1010", R_ALC2XX_HEADSET_MIC),
	ALC_Q(0x1ee7, 0x2081, "HONOR MRB-XXX M1020", R_ALC256_HONOR_MRB_XXX_M1020_AUDIO),
	ALC_Q(0x1f4c, 0xb020, "Minisforum AI X1 Pro", R_ALC245_MINISFORUM_JACK_DETECT),
	ALC_Q(0x1f4c, 0xe001, "Minisforum V3 (SE)", R_ALC245_BASS_HP_DAC),
	ALC_Q(0x1f66, 0x0105, "Ayaneo Portable Game Player", R_ALC245_CS35L41_SPI_2),
	ALC_Q(0x2014, 0x800a, "Positivo ARN50", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x2039, 0x0001, "Inspur S14-G1", R_ALC295_CHROME_BOOK),
	ALC_Q(0x2145, 0x0001, "Star Labs StarFighter", R_ALC233_STARLABS_STARFIGHTER),
	ALC_Q(0x2782, 0x0206, "IPASON SmartBook S1", R_ALC233_MEDION_MTL_SPK),
	ALC_Q(0x2782, 0x0214, "VAIO VJFE-CL", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x2782, 0x0228, "Infinix ZERO BOOK 13", R_ALC269VB_INFINIX_ZERO_BOOK_13),
	ALC_Q(0x2782, 0x0232, "CHUWI CoreBook XPro", R_ALC269VB_CHUWI_COREBOOK_XPRO),
	ALC_Q(0x2782, 0x0250, "Infinix INBOOK X3 Slim", R_ALC269VB_INFINIX_INBOOK_X3_SLIM),
	ALC_Q(0x2782, 0x1407, "Positivo P15X", R_ALC269_POSITIVO_P15X_HEADSET_MIC),
	ALC_Q(0x2782, 0x1409, "Positivo K116J", R_ALC269_POSITIVO_P15X_HEADSET_MIC),
	ALC_Q(0x2782, 0x1701, "Infinix Y4 Max", R_ALC269VC_INFINIX_Y4_MAX),
	ALC_Q(0x2782, 0x1705, "MEDION E15433", R_ALC269VC_INFINIX_Y4_MAX),
	ALC_Q(0x2782, 0x1707, "Vaio VJFE-ADL", R_ALC298_SPK_VOLUME),
	ALC_Q(0x2782, 0x4900, "MEDION E15443", R_ALC233_MEDION_MTL_SPK),
	ALC_Q(0x2782, 0xa128, "Positivo N15RPE-S", R_ALC269_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x2782, 0xa212, "Lunnen Ground 14", R_ALC233_WUJIE_SPEAKERS),
	ALC_Q(0x7017, 0x2014, "Star Labs StarFighter", R_ALC233_STARLABS_STARFIGHTER),
	ALC_Q(0x8086, 0x2073, "Intel NUC 8 Hades Canyon", R_ALC700_INTEL_HADES_CANYON),
	ALC_Q(0x8086, 0x2074, "Intel NUC 8", R_ALC233_INTEL_NUC8_DMIC),
	ALC_Q(0x8086, 0x2080, "Intel NUC 8 Rugged", R_ALC256_INTEL_NUC8_RUGGED),
	ALC_Q(0x8086, 0x2081, "Intel NUC 10", R_ALC269_ASPIRE_HEADSET_MIC),
	ALC_Q(0x8086, 0x3038, "Intel NUC 13", R_ALC295_CHROME_BOOK),
	ALC_Q(0xc011, 0x1d05, "MECHREVO WUJIE Series", R_ALC233_WUJIE_SPEAKERS),
	ALC_Q(0xf111, 0x0001, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE),
	ALC_Q(0xf111, 0x0006, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE),
	ALC_Q(0xf111, 0x0009, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE),
	ALC_Q(0xf111, 0x000b, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE),
	ALC_Q(0xf111, 0x000c, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_MIC_NO_PRESENCE),
	ALC_Q(0xf111, 0x000f, "Framework Laptop 13 Pro PTL", R_ALC295_FRAMEWORK_LAPTOP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0xf111, 0x010f, "Framework Laptop 13 PTL", R_ALC295_FRAMEWORK_LAPTOP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0xf111, 0x0010, "Framework Laptop", R_ALC295_FRAMEWORK_LAPTOP_LIMIT_INT_MIC_BOOST),
	ALC_Q(0x1043, 0x8330, "ASUS Eeepc P703 P900A", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1013, "ASUS N61Da", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1143, "ASUS B53f", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1133, "ASUS UJ20ft", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1183, "ASUS K72DR", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x11b3, "ASUS K52DR", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x11e3, "ASUS U33Jc", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1273, "ASUS UL80Jt", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1283, "ASUS U53Jc", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x12b3, "ASUS N82JV", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x12d3, "ASUS N61Jv", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x13a3, "ASUS UL30Vt", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1373, "ASUS G73JX", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1383, "ASUS UJ30Jc", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x13d3, "ASUS N61JA", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1413, "ASUS UL50", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1443, "ASUS UL30", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1453, "ASUS M60Jv", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1483, "ASUS UL80", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x14f3, "ASUS F83Vf", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x14e3, "ASUS UL20", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1513, "ASUS UX30", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1593, "ASUS N51Vn", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x15a3, "ASUS N60Jv", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x15b3, "ASUS N60Dp", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x15c3, "ASUS N70De", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x15e3, "ASUS F83T", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1643, "ASUS M60J", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1653, "ASUS U50", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1693, "ASUS F50N", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x16a3, "ASUS F5Q", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1723, "ASUS P80", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1743, "ASUS U80", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1773, "ASUS U20A", R_ALC269_AMIC),
	ALC_Q(0x1043, 0x1883, "ASUS F81Se", R_ALC269_AMIC),
	ALC_Q(0x152d, 0x1778, "Quanta ON1", R_ALC269_DMIC),
	ALC_Q(0x17aa, 0x3be9, "Quanta Wistron", R_ALC269_AMIC),
	ALC_Q(0x17aa, 0x3bf8, "Quanta FL1", R_ALC269_AMIC),
	ALC_Q(0x17ff, 0x059a, "Quanta EL3", R_ALC269_DMIC),
	ALC_Q(0x17ff, 0x059b, "Quanta JR1", R_ALC269_DMIC),
	{ 0 }
};

/* Last resort for the 269 family: one recipe for a whole vendor. */
static const struct hda_fixup alc269_vendor_quirks[] = {
	ALC_VQ(0x1025, "Acer Aspire", R_ALC271_DMIC),
	ALC_VQ(0x103c, "HP", R_ALC269_HP_MUTE_LED_MIC1),
	ALC_VQ(0x104d, "Sony VAIO", R_ALC269_SONY_VAIO),
	ALC_VQ(0x17aa, "Lenovo XPAD", R_ALC269_SKU_IGNORE),
	ALC_VQ(0x19e5, "Huawei Matebook", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_VQ(0x2145, "Star Labs", R_ALC269_LIMIT_INT_MIC_BOOST),
	{ 0 }
};

/* ALC66x / ALC89x boards. */
static const struct hda_fixup alc662_quirks[] = {
	ALC_Q(0x1019, 0x9087, "ECS", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1019, 0x9859, "JP-IK LEAP W502", R_ALC897_HEADSET_MIC_PIN3),
	ALC_Q(0x1022, 0xc950, "CSL Unity BF24B", R_ALC662_CSL_GPIO),
	ALC_Q(0x1025, 0x022f, "Acer Aspire One", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x0241, "Packard Bell DOTS", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x0308, "Acer Aspire 8942G", R_ALC662_ASPIRE),
	ALC_Q(0x1025, 0x031c, "Gateway NV79", R_ALC269_SKU_IGNORE),
	ALC_Q(0x1025, 0x0349, "eMachines eM250", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x034a, "Gateway LT27", R_ALC269_INV_DMIC),
	ALC_Q(0x1025, 0x038b, "Acer Aspire 8943G", R_ALC662_ASPIRE),
	ALC_Q(0x1025, 0x0566, "Acer Aspire Ethos 8951G", R_ALC669_ACER_ASPIRE_ETHOS),
	ALC_Q(0x1025, 0x123c, "Acer Nitro N50-600", R_ALC662_ACER_NITRO_HEADSET_MODE),
	ALC_Q(0x1025, 0x124e, "Acer 2660G", R_ALC662_ACER_X2660G_HEADSET_MODE),
	ALC_Q(0x1028, 0x05d8, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x05db, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x05fe, "Dell XPS 15", R_ALC668_DELL_XPS13),
	ALC_Q(0x1028, 0x060a, "Dell XPS 13", R_ALC668_DELL_XPS13),
	ALC_Q(0x1028, 0x060d, "Dell M3800", R_ALC668_DELL_XPS13),
	ALC_Q(0x1028, 0x0625, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0626, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0696, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x0698, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x1028, 0x069f, "Dell", R_ALC668_DELL_MIC_NO_PRESENCE),
	ALC_Q(0x103c, 0x1632, "HP RP5800", R_ALC662_HP_RP5800),
	ALC_Q(0x103c, 0x870c, "HP", R_ALC897_HP_HSMIC_VERB),
	ALC_Q(0x103c, 0x8719, "HP", R_ALC897_HP_HSMIC_VERB),
	ALC_Q(0x103c, 0x872b, "HP", R_ALC897_HP_HSMIC_VERB),
	ALC_Q(0x103c, 0x873e, "HP", R_ALC671_HP_HEADSET_MIC2),
	ALC_Q(0x103c, 0x8768, "HP Slim Desktop S01", R_ALC671_HP_HEADSET_MIC2),
	ALC_Q(0x103c, 0x877e, "HP 288 Pro G6", R_ALC671_HP_HEADSET_MIC2),
	ALC_Q(0x103c, 0x885f, "HP 288 Pro G8", R_ALC671_HP_HEADSET_MIC2),
	ALC_Q(0x1043, 0x1080, "Asus UX501VW", R_ALC668_HEADSET_MODE),
	ALC_Q(0x1043, 0x11cd, "Asus N550", R_ALC662_ASUS_Nx50),
	ALC_Q(0x1043, 0x129d, "Asus N750", R_ALC662_ASUS_Nx50),
	ALC_Q(0x1043, 0x12ff, "ASUS G751", R_ALC668_ASUS_G751),
	ALC_Q(0x1043, 0x13df, "Asus N550JX", R_ALC662_ASUS_Nx50),
	ALC_Q(0x1043, 0x1477, "ASUS N56VZ", R_ALC662_BASS_MODE4_CHMAP),
	ALC_Q(0x1043, 0x15a7, "ASUS UX51VZH", R_ALC662_BASS_16),
	ALC_Q(0x1043, 0x177d, "ASUS N551", R_ALC668_ASUS_Nx51),
	ALC_Q(0x1043, 0x17bd, "ASUS N751", R_ALC668_ASUS_Nx51),
	ALC_Q(0x1043, 0x185d, "ASUS G551JW", R_ALC668_ASUS_NO_HEADSET_MIC),
	ALC_Q(0x1043, 0x1963, "ASUS X71SL", R_ALC662_ASUS_MODE8),
	ALC_Q(0x1043, 0x1b73, "ASUS N55SF", R_ALC662_BASS_16),
	ALC_Q(0x1043, 0x1bf3, "ASUS N76VZ", R_ALC662_BASS_MODE4_CHMAP),
	ALC_Q(0x1043, 0x8469, "ASUS mobo", R_ALC662_NO_JACK_DETECT),
	ALC_Q(0x105b, 0x0cd6, "Foxconn", R_ALC662_ASUS_MODE2),
	ALC_Q(0x144d, 0xc051, "Samsung R720", R_ALC662_IDEAPAD),
	ALC_Q(0x14cd, 0x5003, "USI", R_ALC662_USI_HEADSET_MODE),
	ALC_Q(0x17aa, 0x1036, "Lenovo P520", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x17aa, 0x1057, "Lenovo P360", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x1064, "Lenovo P3 Tower", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x32ca, "Lenovo ThinkCentre M80", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x32cb, "Lenovo ThinkCentre M70", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x32cf, "Lenovo ThinkCentre M950", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x32f7, "Lenovo ThinkCentre M90", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x3321, "Lenovo ThinkCentre M70 Gen4", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x331b, "Lenovo ThinkCentre M90 Gen4", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x3364, "Lenovo ThinkCentre M90 Gen5", R_ALC897_HEADSET_MIC_PIN),
	ALC_Q(0x17aa, 0x3742, "Lenovo TianYi510Pro-14IOB", R_ALC897_HEADSET_MIC_PIN2),
	ALC_Q(0x17aa, 0x38af, "Lenovo Ideapad Y550P", R_ALC662_IDEAPAD),
	ALC_Q(0x17aa, 0x3a0d, "Lenovo Ideapad Y550", R_ALC662_IDEAPAD),
	ALC_Q(0x1849, 0x5892, "ASRock B150M", R_ALC892_ASROCK_MOBO),
	ALC_Q(0x19da, 0xa130, "Zotac Z68", R_ALC662_ZOTAC_Z68),
	ALC_Q(0x1b0a, 0x01b8, "ACER Veriton", R_ALC662_ACER_VERITON),
	ALC_Q(0x1b35, 0x1234, "CZC ET26", R_ALC662_CZC_ET26),
	ALC_Q(0x1b35, 0x2206, "CZC P10T", R_ALC269_LENOVO_EAPD),
	ALC_Q(0x1c6c, 0x1239, "Compaq N14JP6-V2", R_ALC897_HP_HSMIC_VERB),
	ALC_Q(0x1e63, 0x6d9a, "F+ FLAPTOP r", R_ALC897_HP_HSMIC_VERB),
	ALC_Q(0x1f66, 0x0202, "Beelink SER6 Max 6900", R_ALC256_ASUS_AIO_GPIO2),
	ALC_Q(0x1043, 0x1000, "ASUS N50Vm", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1092, "ASUS NB", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x1173, "ASUS K73Jn", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x11c3, "ASUS M70V", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x11d3, "ASUS NB", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x11f3, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1203, "ASUS NB", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1303, "ASUS G60J", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1333, "ASUS G60Jx", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1339, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x13e3, "ASUS N71JA", R_ALC662_ASUS_MODE7),
	ALC_Q(0x1043, 0x1463, "ASUS N71", R_ALC662_ASUS_MODE7),
	ALC_Q(0x1043, 0x14d3, "ASUS G72", R_ALC662_ASUS_MODE8),
	ALC_Q(0x1043, 0x1563, "ASUS N90", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x15d3, "ASUS N50SF F50SF", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x16c3, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x16f3, "ASUS K40C K50C", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1733, "ASUS N81De", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1753, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1763, "ASUS NB", R_ALC662_ASUS_MODE6),
	ALC_Q(0x1043, 0x1765, "ASUS NB", R_ALC662_ASUS_MODE6),
	ALC_Q(0x1043, 0x1783, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1793, "ASUS F50GX", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x17b3, "ASUS F70SL", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x17f3, "ASUS X58LE", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1813, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1823, "ASUS NB", R_ALC662_ASUS_MODE5),
	ALC_Q(0x1043, 0x1833, "ASUS NB", R_ALC662_ASUS_MODE6),
	ALC_Q(0x1043, 0x1843, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1853, "ASUS F50Z", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1864, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1876, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1893, "ASUS M50Vm", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x1894, "ASUS X55", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x18b3, "ASUS N80Vc", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x18c3, "ASUS VX5", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x18d3, "ASUS N81Te", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x18f3, "ASUS N505Tp", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1903, "ASUS F5GL", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1913, "ASUS NB", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1933, "ASUS F80Q", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x1943, "ASUS Vx3V", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1953, "ASUS NB", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1963, "ASUS X71C", R_ALC662_ASUS_MODE3),
	ALC_Q(0x1043, 0x1983, "ASUS N5051A", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x1993, "ASUS N20", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x19b3, "ASUS F7Z", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x19c3, "ASUS F5Z/F6x", R_ALC662_ASUS_MODE2),
	ALC_Q(0x1043, 0x19e3, "ASUS NB", R_ALC662_ASUS_MODE1),
	ALC_Q(0x1043, 0x19f3, "ASUS NB", R_ALC662_BASS_MODE4_CHMAP),
	{ 0 }
};

/* ALC88x / ALC1150 / ALC1220 boards. */
static const struct hda_fixup alc882_quirks[] = {
	ALC_Q(0x1025, 0x006c, "Acer Aspire 9810", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x0090, "Acer Aspire", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x0107, "Acer Aspire", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x010a, "Acer Ferrari 5000", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x0110, "Acer Aspire", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x0112, "Acer Aspire 9303", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x0121, "Acer Aspire 5920G", R_ALC883_ACER_EAPD),
	ALC_Q(0x1025, 0x013e, "Acer Aspire 4930G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x013f, "Acer Aspire 5930G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x0145, "Acer Aspire 8930G", R_ALC882_ACER_ASPIRE_8930G),
	ALC_Q(0x1025, 0x0146, "Acer Aspire 6935G", R_ALC882_ACER_ASPIRE_8930G),
	ALC_Q(0x1025, 0x0142, "Acer Aspire 7730G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x0155, "Packard-Bell M5120", R_ALC882_PB_M5210),
	ALC_Q(0x1025, 0x015e, "Acer Aspire 6930G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x0166, "Acer Aspire 6530G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x021e, "Acer Aspire 5739G", R_ALC882_ACER_ASPIRE_4930G),
	ALC_Q(0x1025, 0x0259, "Acer Aspire 5935", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x1025, 0x026b, "Acer Aspire 8940G", R_ALC882_ACER_ASPIRE_8930G),
	ALC_Q(0x1025, 0x0296, "Acer Aspire 7736z", R_ALC269_SKU_IGNORE),
	ALC_Q(0x1043, 0x13c2, "Asus A7M", R_ALC882_EAPD),
	ALC_Q(0x1043, 0x1873, "ASUS W90V", R_ALC882_ASUS_W90V),
	ALC_Q(0x1043, 0x1971, "Asus W2JC", R_ALC882_ASUS_W2JC),
	ALC_Q(0x1043, 0x2390, "Asus D700SA", R_ALC887_ASUS_HMIC),
	ALC_Q(0x1043, 0x835f, "Asus Eee 1601", R_ALC888_EEE1601),
	ALC_Q(0x1043, 0x84bc, "ASUS ET2700", R_ALC887_ASUS_BASS),
	ALC_Q(0x1043, 0x8691, "ASUS ROG Ranger VIII", R_ALC882_GPIO3),
	ALC_Q(0x1043, 0x8797, "ASUS TUF B550M-PLUS", R_ALCS1200A_MIC_VREF),
	ALC_Q(0x104d, 0x9043, "Sony Vaio VGC-LN51JGB", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x104d, 0x9044, "Sony VAIO AiO", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x104d, 0x9047, "Sony Vaio TT", R_ALC889_VAIO_TT),
	ALC_Q(0x104d, 0x905a, "Sony Vaio Z", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x104d, 0x9060, "Sony Vaio VPCL14M1R", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x106b, 0x00a0, "MacBookPro 3,1", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x00a1, "Macbook", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x00a4, "MacbookPro 4,1", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x0c00, "Mac Pro", R_ALC889_MP11_VREF),
	ALC_Q(0x106b, 0x1000, "iMac 24", R_ALC882_GPIO3),
	ALC_Q(0x106b, 0x2800, "AppleTV", R_ALC882_GPIO3),
	ALC_Q(0x106b, 0x2c00, "MacbookPro rev3", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x3000, "iMac", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x3200, "iMac 7,1 Aluminum", R_ALC882_EAPD),
	ALC_Q(0x106b, 0x3400, "MacBookAir 1,1", R_ALC889_MBA11_VREF),
	ALC_Q(0x106b, 0x3500, "MacBookAir 2,1", R_ALC889_MBA21_VREF),
	ALC_Q(0x106b, 0x3600, "Macbook 3,1", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x3800, "MacbookPro 4,1", R_ALC889_MBP_VREF),
	ALC_Q(0x106b, 0x3e00, "iMac 24 Aluminum", R_ALC882_GPIO3),
	ALC_Q(0x106b, 0x3f00, "Macbook 5,1", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4000, "MacbookPro 5,1", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4100, "Macmini 3,1", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4200, "Mac Pro 4,1/5,1", R_ALC889_MP41_VREF),
	ALC_Q(0x106b, 0x4300, "iMac 9,1", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4600, "MacbookPro 5,2", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4900, "iMac 9,1 Aluminum", R_ALC889_IMAC91_VREF),
	ALC_Q(0x106b, 0x4a00, "Macbook 5,2", R_ALC889_MBA11_VREF),
	ALC_Q(0x1071, 0x8258, "Evesham Voyaeger", R_ALC882_EAPD),
	ALC_Q(0x10ec, 0x12d8, "iBase Elo Touch", R_ALC888VD_MIC_100VREF),
	ALC_Q(0x13fe, 0x1009, "Advantech MIT-W101", R_ALC886_EAPD),
	ALC_Q(0x1458, 0xa002, "Gigabyte EP45-DS3/Z87X-UD3H", R_ALC889_FRONT_HP_NO_PRESENCE),
	ALC_Q(0x1458, 0xa0b8, "Gigabyte AZ370-Gaming", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x1458, 0xa0cd, "Gigabyte X570 Aorus Master", R_ALC1220_GB_X570),
	ALC_Q(0x1458, 0xa0ce, "Gigabyte X570 Aorus Xtreme", R_ALC1220_GB_X570),
	ALC_Q(0x1458, 0xa0d5, "Gigabyte X570S Aorus Master", R_ALC1220_GB_X570),
	ALC_Q(0x1462, 0x11f7, "MSI-GE63", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x1228, "MSI-GP63", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x1229, "MSI-GP73", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x1275, "MSI-GL63", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x1276, "MSI-GL73", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x1293, "MSI-GP65", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1462, 0x7350, "MSI-7350", R_ALC889_CD),
	ALC_Q(0x1462, 0xcc34, "MSI Godlike X570", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_Q(0x1462, 0xda57, "MSI Z270-Gaming", R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC),
	ALC_VQ(0x1462, "MSI", R_ALC882_GPIO3),
	ALC_Q(0x147b, 0x107a, "Abit AW9D-MAX", R_ALC882_ABIT_AW9D_MAX),
	ALC_Q(0x1558, 0x3702, "Clevo X370SN[VW]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x50d3, "Clevo PC50[ER][CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x5802, "Clevo X58[05]WN[RST]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65d1, "Clevo PB51[ER][CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65d2, "Clevo PB51R[CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65e1, "Clevo PB51[ED][DF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65e5, "Clevo PC50D[PRS](?:-D|-G)?", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65f1, "Clevo PC50HS", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x65f5, "Clevo PD50PN[NRT]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x66a2, "Clevo PE60RNE", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x66a6, "Clevo PE60SN[CDE]-[GS]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x67d1, "Clevo PB71[ER][CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x67e1, "Clevo PB71[DE][CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x67e5, "Clevo PC70D[PRS](?:-D|-G)?", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x67f1, "Clevo PC70H[PRS]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x67f5, "Clevo PD70PN[NRT]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x70d1, "Clevo PC70[ER][CDF]", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x7709, "Clevo P775TM1", R_ALC898_CLEVO_P775TM1),
	ALC_Q(0x1558, 0x7714, "Clevo X170SM", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_Q(0x1558, 0x7715, "Clevo X170KM-G", R_ALC1220_CLEVO_PB51ED),
	ALC_Q(0x1558, 0x9501, "Clevo P950HR", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x9506, "Clevo P955HQ", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x950a, "Clevo P955H[PR]", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e1, "Clevo P95xER", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e2, "Clevo P950ER", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e3, "Clevo P955[ER]T", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e4, "Clevo P955ER", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e5, "Clevo P955EE6", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x95e6, "Clevo P950R[CDF]", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x96e1, "Clevo P960[ER][CDFN]-K", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x97e1, "Clevo P970[ER][CDFN]", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0x97e2, "Clevo P970RC-M", R_ALC1220_CLEVO_P950),
	ALC_Q(0x1558, 0xd502, "Clevo PD50SNE", R_ALC1220_CLEVO_PB51ED_PINS),
	ALC_VQ(0x1558, "Clevo laptop", R_ALC882_EAPD),
	ALC_Q(0x161f, 0x2054, "Medion laptop", R_ALC883_EAPD),
	ALC_Q(0x17aa, 0x3a0d, "Lenovo Y530", R_ALC882_LENOVO_Y530),
	ALC_Q(0x8086, 0x0022, "DX58SO", R_ALC889_COEF),
	{ 0 }
};

static const struct hda_pincfg alc269_pin_fixup_tbl_p0[] = {
	{ 0x14, 0x01014020 }, { 0x17, 0x90170110 }, { 0x18, 0x02a11030 }, { 0x21, 0x0221102f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p1[] = {
	{ 0x12, 0x90a601c0 }, { 0x14, 0x90171120 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p2[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x90a70130 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p3[] = {
	{ 0x1a, 0x90a70130 }, { 0x1b, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p4[] = {
	{ 0x12, 0xb7a60130 }, { 0x14, 0x901701a0 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p5[] = {
	{ 0x12, 0xb7a60130 }, { 0x14, 0x901701b0 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p6[] = {
	{ 0x12, 0xb7a60150 }, { 0x14, 0x901701a0 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p7[] = {
	{ 0x12, 0xb7a60150 }, { 0x14, 0x901701b0 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p8[] = {
	{ 0x12, 0xb7a60130 }, { 0x1b, 0x90170110 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p9[] = {
	{ 0x1b, 0x01111010 }, { 0x1e, 0x01451130 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p10[] = {
	{ 0x12, 0x90a60140 }, { 0x14, 0x90170110 }, { 0x19, 0x02a11030 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p11[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x02a11030 }, { 0x1a, 0x02a11040 }, { 0x1b, 0x01014020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p12[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x02a11030 }, { 0x1a, 0x02a11040 }, { 0x1b, 0x01011020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p13[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x02a11020 }, { 0x1a, 0x02a11030 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p14[] = {
	{ 0x21, 0x02211010 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p15[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x02a11020 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p16[] = {
	{ 0x14, 0x90170110 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p17[] = {
	{ 0x14, 0x90170130 }, { 0x21, 0x02211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p18[] = {
	{ 0x12, 0x90a60140 }, { 0x14, 0x90170110 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p19[] = {
	{ 0x12, 0x90a60160 }, { 0x14, 0x90170120 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p20[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x02011020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p21[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x01011020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p22[] = {
	{ 0x14, 0x90170130 }, { 0x1b, 0x01014020 }, { 0x21, 0x0221103f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p23[] = {
	{ 0x14, 0x90170130 }, { 0x1b, 0x01011020 }, { 0x21, 0x0221103f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p24[] = {
	{ 0x14, 0x90170130 }, { 0x1b, 0x02011020 }, { 0x21, 0x0221103f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p25[] = {
	{ 0x14, 0x90170150 }, { 0x1b, 0x02011020 }, { 0x21, 0x0221105f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p26[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x01014020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p27[] = {
	{ 0x12, 0x90a60160 }, { 0x14, 0x90170120 }, { 0x17, 0x90170140 }, { 0x21, 0x0321102f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p28[] = {
	{ 0x12, 0x90a60160 }, { 0x14, 0x90170130 }, { 0x21, 0x02211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p29[] = {
	{ 0x12, 0x90a60160 }, { 0x14, 0x90170140 }, { 0x21, 0x02211050 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p30[] = {
	{ 0x12, 0x90a60170 }, { 0x14, 0x90170120 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p31[] = {
	{ 0x12, 0x90a60170 }, { 0x14, 0x90170130 }, { 0x21, 0x02211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p32[] = {
	{ 0x12, 0x90a60170 }, { 0x14, 0x90171130 }, { 0x21, 0x02211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p33[] = {
	{ 0x12, 0x90a60170 }, { 0x14, 0x90170140 }, { 0x21, 0x02211050 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p34[] = {
	{ 0x12, 0x90a60180 }, { 0x14, 0x90170130 }, { 0x21, 0x02211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p35[] = {
	{ 0x12, 0x90a60180 }, { 0x14, 0x90170120 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p36[] = {
	{ 0x1b, 0x01011020 }, { 0x21, 0x02211010 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p37[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x90a70130 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p38[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x90a70130 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p39[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p40[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p41[] = {
	{ 0x1a, 0x90a70130 }, { 0x1b, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p42[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x02a11020 }, { 0x21, 0x0221101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p43[] = {
	{ 0x17, 0x90170110 }, { 0x19, 0x03a11030 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p44[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x15, 0x0421101f }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p45[] = {
	{ 0x12, 0x90a60140 }, { 0x14, 0x90170110 }, { 0x15, 0x0421101f }, { 0x18, 0x02811030 }, { 0x1a, 0x04a1103f }, { 0x1b, 0x02011020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p46[] = {
	{ 0x12, 0x99a30130 }, { 0x19, 0x03a11020 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p47[] = {
	{ 0x12, 0x99a30130 }, { 0x19, 0x03a11020 }, { 0x21, 0x03211040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p48[] = {
	{ 0x12, 0x99a30130 }, { 0x19, 0x03a11030 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p49[] = {
	{ 0x12, 0x99a30130 }, { 0x19, 0x04a11020 }, { 0x21, 0x0421101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p50[] = {
	{ 0x12, 0x90a60140 }, { 0x19, 0x04a11030 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p51[] = {
	{ 0x12, 0x90a609c0 }, { 0x18, 0x03a11830 }, { 0x19, 0x04a19831 }, { 0x1a, 0x0481303f }, { 0x1b, 0x04211020 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p52[] = {
	{ 0x12, 0x90a60940 }, { 0x18, 0x03a11830 }, { 0x19, 0x04a19831 }, { 0x1a, 0x0481303f }, { 0x1b, 0x04211020 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p53[] = {
	{ 0x12, 0x90a60130 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p54[] = {
	{ 0x12, 0x90a60160 }, { 0x14, 0x90170120 }, { 0x21, 0x02211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p55[] = {
	{ 0x12, 0x90a60130 }, { 0x19, 0x03a11020 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p56[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x19, 0x04a11040 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p57[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x04a11040 }, { 0x1d, 0x40600001 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p58[] = {
	{ 0x14, 0x90170110 }, { 0x19, 0x04a11040 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p59[] = {
	{ 0x14, 0x90170110 }, { 0x17, 0x90170111 }, { 0x19, 0x03a11030 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p60[] = {
	{ 0x17, 0x90170110 }, { 0x19, 0x03a11030 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p61[] = {
	{ 0x17, 0x90170110 }, { 0x19, 0x04a11040 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p62[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p63[] = {
	{ 0x12, 0x90a60120 }, { 0x14, 0x90170110 }, { 0x21, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p64[] = {
	{ 0x15, 0x04211040 }, { 0x18, 0x90170112 }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p65[] = {
	{ 0x15, 0x04211040 }, { 0x18, 0x90170110 }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p66[] = {
	{ 0x15, 0x0421101f }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p67[] = {
	{ 0x15, 0x04211020 }, { 0x1a, 0x04a11040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p68[] = {
	{ 0x14, 0x90170110 }, { 0x15, 0x04211020 }, { 0x1a, 0x04a11040 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p69[] = {
	{ 0x14, 0x90170110 }, { 0x15, 0x04211020 }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p70[] = {
	{ 0x14, 0x90170110 }, { 0x15, 0x0421101f }, { 0x1a, 0x04a11020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p71[] = {
	{ 0x12, 0x90a60140 }, { 0x16, 0x01014020 }, { 0x19, 0x01a19030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p72[] = {
	{ 0x12, 0x90a60140 }, { 0x16, 0x01014020 }, { 0x18, 0x02a19031 }, { 0x19, 0x01a1903e },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p73[] = {
	{ 0x12, 0x90a60140 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p74[] = {
	{ 0x13, 0x90a60140 }, { 0x16, 0x21014020 }, { 0x19, 0x21a19030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p75[] = {
	{ 0x13, 0x90a60140 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p76[] = {
	{ 0x17, 0x90170110 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p77[] = {
	{ 0x14, 0x90170110 }, { 0x1b, 0x90a70130 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p78[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p79[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x04211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p80[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p81[] = {
	{ 0x12, 0x90a60120 }, { 0x17, 0x90170110 }, { 0x21, 0x04211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p82[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p83[] = {
	{ 0x12, 0x90a60130 }, { 0x17, 0x90170110 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p84[] = {
	{ 0x17, 0x90170110 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p85[] = {
	{ 0x17, 0x90170140 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p86[] = {
	{ 0x17, 0x90170150 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p87[] = {
	{ 0x12, 0xb7a60140 }, { 0x13, 0xb7a60150 }, { 0x17, 0x90170110 }, { 0x1a, 0x03011020 }, { 0x21, 0x03211030 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p88[] = {
	{ 0x12, 0xb7a60140 }, { 0x17, 0x90170110 }, { 0x1a, 0x03a11030 }, { 0x21, 0x03211020 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p89[] = {
	{ 0x12, 0xb7a60130 }, { 0x17, 0x90170110 },
	{ 0 }
};
static const struct hda_pincfg alc269_pin_fixup_tbl_p90[] = {
	{ 0x14, 0x01014010 }, { 0x17, 0x90170120 }, { 0x18, 0x02a11030 }, { 0x19, 0x02a1103f }, { 0x21, 0x0221101f },
	{ 0 }
};
/* ALC2xx boards recognised by their BIOS pin layout (strict match). */
static const struct alc_pin_quirk alc269_pin_quirks[] = {
	{ 0x10ec0221, 0x103c, 1, R_ALC221_HP_HEADSET_MIC, "HP Workstation", alc269_pin_fixup_tbl_p0 },
	{ 0x10ec0255, 0x1025, 1, R_ALC255_ACER_MIC_NO_PRESENCE, "Acer", alc269_pin_fixup_tbl_p1 },
	{ 0x10ec0255, 0x1043, 1, R_ALC255_ACER_MIC_NO_PRESENCE, "ASUS", alc269_pin_fixup_tbl_p2 },
	{ 0x10ec0255, 0x1043, 1, R_ALC255_ACER_MIC_NO_PRESENCE, "ASUS", alc269_pin_fixup_tbl_p3 },
	{ 0x10ec0225, 0x1028, 1, R_ALC225_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p4 },
	{ 0x10ec0225, 0x1028, 1, R_ALC225_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p5 },
	{ 0x10ec0225, 0x1028, 1, R_ALC225_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p6 },
	{ 0x10ec0225, 0x1028, 1, R_ALC225_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p7 },
	{ 0x10ec0225, 0x1028, 1, R_ALC225_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p8 },
	{ 0x10ec0233, 0x8086, 1, R_ALC269_DELL1_MIC_NO_PRESENCE, "Intel NUC Skull Canyon", alc269_pin_fixup_tbl_p9 },
	{ 0x10ec0235, 0x17aa, 1, R_ALC255_LIFEBOOK_U7x7_HEADSET_MIC, "Lenovo", alc269_pin_fixup_tbl_p10 },
	{ 0x10ec0235, 0x17aa, 1, R_ALC294_LENOVO_MIC_LOCATION, "Lenovo", alc269_pin_fixup_tbl_p11 },
	{ 0x10ec0235, 0x17aa, 1, R_ALC294_LENOVO_MIC_LOCATION, "Lenovo", alc269_pin_fixup_tbl_p12 },
	{ 0x10ec0235, 0x17aa, 1, R_ALC294_LENOVO_MIC_LOCATION, "Lenovo", alc269_pin_fixup_tbl_p13 },
	{ 0x10ec0236, 0x1028, 1, R_ALC236_DELL_AIO_HEADSET_MIC, "Dell", alc269_pin_fixup_tbl_p14 },
	{ 0x10ec0236, 0x103c, 1, R_ALC256_HP_HEADSET_MIC, "HP", alc269_pin_fixup_tbl_p15 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL2_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p16 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p17 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p18 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p19 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p20 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p21 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p22 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p23 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p24 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p25 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p26 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p27 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p28 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p29 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p30 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p31 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p32 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p33 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell Inspiron 5548", alc269_pin_fixup_tbl_p34 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell Inspiron 5565", alc269_pin_fixup_tbl_p35 },
	{ 0x10ec0255, 0x1028, 1, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p36 },
	{ 0x10ec0256, 0x1043, 1, R_ALC256_ASUS_MIC, "ASUS", alc269_pin_fixup_tbl_p37 },
	{ 0x10ec0256, 0x1043, 1, R_ALC256_ASUS_MIC, "ASUS", alc269_pin_fixup_tbl_p38 },
	{ 0x10ec0256, 0x1043, 1, R_ALC256_ASUS_MIC_NO_PRESENCE, "ASUS", alc269_pin_fixup_tbl_p39 },
	{ 0x10ec0256, 0x1043, 1, R_ALC256_ASUS_MIC_NO_PRESENCE, "ASUS", alc269_pin_fixup_tbl_p40 },
	{ 0x10ec0256, 0x1043, 1, R_ALC256_ASUS_MIC_NO_PRESENCE, "ASUS", alc269_pin_fixup_tbl_p41 },
	{ 0x10ec0256, 0x103c, 1, R_ALC256_HP_HEADSET_MIC, "HP", alc269_pin_fixup_tbl_p42 },
	{ 0x10ec0274, 0x103c, 1, R_ALC274_HP_HEADSET_MIC, "HP", alc269_pin_fixup_tbl_p43 },
	{ 0x10ec0280, 0x103c, 1, R_ALC280_HP_GPIO4, "HP", alc269_pin_fixup_tbl_p44 },
	{ 0x10ec0280, 0x103c, 1, R_ALC269_HP_GPIO_MIC1_LED, "HP", alc269_pin_fixup_tbl_p45 },
	{ 0x10ec0282, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP 15 Touchsmart", alc269_pin_fixup_tbl_p46 },
	{ 0x10ec0282, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p47 },
	{ 0x10ec0282, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p48 },
	{ 0x10ec0282, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p49 },
	{ 0x10ec0282, 0x103c, 1, R_ALC269_HP_LINE1_MIC1_LED, "HP", alc269_pin_fixup_tbl_p50 },
	{ 0x10ec0282, 0x1025, 1, R_ALC282_ACER_DISABLE_LINEOUT, "Acer", alc269_pin_fixup_tbl_p51 },
	{ 0x10ec0282, 0x1025, 1, R_ALC282_ACER_DISABLE_LINEOUT, "Acer", alc269_pin_fixup_tbl_p52 },
	{ 0x10ec0283, 0x1028, 1, R_ALC269_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p53 },
	{ 0x10ec0283, 0x1028, 1, R_ALC269_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p54 },
	{ 0x10ec0283, 0x1028, 1, R_ALC269_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p55 },
	{ 0x10ec0285, 0x17aa, 1, R_ALC285_LENOVO_PC_BEEP_IN_NOISE, "Lenovo", alc269_pin_fixup_tbl_p56 },
	{ 0x10ec0285, 0x17aa, 1, R_ALC285_LENOVO_PC_BEEP_IN_NOISE, "Lenovo", alc269_pin_fixup_tbl_p57 },
	{ 0x10ec0285, 0x17aa, 1, R_ALC285_THINKPAD_NO_BASS_SPK_HEADSET_JACK, "Lenovo", alc269_pin_fixup_tbl_p58 },
	{ 0x10ec0287, 0x17aa, 1, R_ALC285_THINKPAD_HEADSET_JACK, "Lenovo", alc269_pin_fixup_tbl_p59 },
	{ 0x10ec0287, 0x17aa, 1, R_ALC287_THINKPAD_I2S_SPK, "Lenovo", alc269_pin_fixup_tbl_p60 },
	{ 0x10ec0287, 0x17aa, 1, R_ALC287_THINKPAD_I2S_SPK, "Lenovo", alc269_pin_fixup_tbl_p61 },
	{ 0x10ec0286, 0x1025, 1, R_ALC286_ACER_AIO_MIC_NO_PRESENCE, "Acer", alc269_pin_fixup_tbl_p62 },
	{ 0x10ec0288, 0x1028, 1, R_ALC288_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p63 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p64 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p65 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p66 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p67 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p68 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p69 },
	{ 0x10ec0290, 0x103c, 1, R_ALC269_HP_MUTE_LED_MIC1, "HP", alc269_pin_fixup_tbl_p70 },
	{ 0x10ec0292, 0x1028, 1, R_ALC269_DELL2_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p71 },
	{ 0x10ec0292, 0x1028, 1, R_ALC269_DELL2_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p72 },
	{ 0x10ec0292, 0x1028, 1, R_ALC293_SYSTEM76_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p73 },
	{ 0x10ec0293, 0x1028, 1, R_ALC293_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p74 },
	{ 0x10ec0293, 0x1028, 1, R_ALC293_DELL1_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p75 },
	{ 0x10ec0294, 0x1043, 1, R_ALC294_ASUS_HPE, "ASUS", alc269_pin_fixup_tbl_p76 },
	{ 0x10ec0294, 0x1043, 1, R_ALC294_ASUS_MIC, "ASUS", alc269_pin_fixup_tbl_p77 },
	{ 0x10ec0294, 0x1043, 1, R_ALC294_ASUS_SPK, "ASUS", alc269_pin_fixup_tbl_p78 },
	{ 0x10ec0294, 0x1043, 1, R_ALC294_ASUS_SPK, "ASUS", alc269_pin_fixup_tbl_p79 },
	{ 0x10ec0295, 0x1043, 1, R_ALC294_ASUS_SPK, "ASUS", alc269_pin_fixup_tbl_p80 },
	{ 0x10ec0295, 0x1043, 1, R_ALC269_ASPIRE_HEADSET_MIC, "ASUS", alc269_pin_fixup_tbl_p81 },
	{ 0x10ec0295, 0x1043, 1, R_ALC269_ASPIRE_HEADSET_MIC, "ASUS", alc269_pin_fixup_tbl_p82 },
	{ 0x10ec0295, 0x1043, 1, R_ALC269_ASPIRE_HEADSET_MIC, "ASUS", alc269_pin_fixup_tbl_p83 },
	{ 0x10ec0298, 0x1028, 1, R_ALC221_HP_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p84 },
	{ 0x10ec0298, 0x1028, 1, R_ALC221_HP_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p85 },
	{ 0x10ec0298, 0x1028, 1, R_ALC221_HP_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p86 },
	{ 0x10ec0298, 0x1028, 1, R_ALC298_SPK_VOLUME, "Dell", alc269_pin_fixup_tbl_p87 },
	{ 0x10ec0298, 0x1028, 1, R_ALC298_ALIENWARE_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p88 },
	{ 0x10ec0299, 0x1028, 1, R_ALC269_DELL4_MIC_NO_PRESENCE, "Dell", alc269_pin_fixup_tbl_p89 },
	{ 0x10ec0623, 0x17aa, 1, R_ALC283_HEADSET_MIC, "Lenovo", alc269_pin_fixup_tbl_p90 },
	{ 0 }
};

static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p0[] = {
	{ 0x19, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p1[] = {
	{ 0x19, 0x40000000 }, { 0x1b, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p2[] = {
	{ 0x19, 0x40000000 }, { 0x1b, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p3[] = {
	{ 0x19, 0x40000000 }, { 0x1a, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p4[] = {
	{ 0x19, 0x40000000 }, { 0x1a, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p5[] = {
	{ 0x19, 0x40000000 }, { 0x1a, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p6[] = {
	{ 0x19, 0x40000000 },
	{ 0 }
};
static const struct hda_pincfg alc269_fallback_pin_fixup_tbl_p7[] = {
	{ 0x19, 0x40000000 },
	{ 0 }
};
/* Partial pin layouts: headset mic pins left unconfigured by the BIOS. */
static const struct alc_pin_quirk alc269_fallback_pin_quirks[] = {
	{ 0x10ec0256, 0x1025, 0, R_ALC2XX_HEADSET_MIC, "Acer", alc269_fallback_pin_fixup_tbl_p0 },
	{ 0x10ec0289, 0x1028, 0, R_ALC269_DELL4_MIC_NO_PRESENCE, "Dell", alc269_fallback_pin_fixup_tbl_p1 },
	{ 0x10ec0295, 0x1028, 0, R_ALC269_DELL4_MIC_NO_PRESENCE_QUIET, "Dell", alc269_fallback_pin_fixup_tbl_p2 },
	{ 0x10ec0256, 0x1028, 0, R_ALC255_DELL1_MIC_NO_PRESENCE, "Dell", alc269_fallback_pin_fixup_tbl_p3 },
	{ 0x10ec0236, 0x1028, 0, R_ALC255_DELL1_LIMIT_INT_MIC_BOOST, "Dell", alc269_fallback_pin_fixup_tbl_p4 },
	{ 0x10ec0274, 0x1028, 0, R_ALC269_DELL1_LIMIT_INT_MIC_BOOST, "Dell", alc269_fallback_pin_fixup_tbl_p5 },
	{ 0x10ec0256, 0x1043, 0, R_ALC2XX_HEADSET_MIC, "ASUS", alc269_fallback_pin_fixup_tbl_p6 },
	{ 0x10ec0255, 0x1558, 0, R_ALC2XX_HEADSET_MIC, "Clevo", alc269_fallback_pin_fixup_tbl_p7 },
	{ 0 }
};

static const struct hda_pincfg alc662_pin_fixup_tbl_p0[] = {
	{ 0x17, 0x02211010 }, { 0x18, 0x01a19030 }, { 0x1a, 0x01813040 }, { 0x21, 0x01014020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p1[] = {
	{ 0x16, 0x01813030 }, { 0x17, 0x02211010 }, { 0x18, 0x01a19040 }, { 0x21, 0x01014020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p2[] = {
	{ 0x14, 0x01014010 }, { 0x18, 0x01a19020 }, { 0x1a, 0x0181302f }, { 0x1b, 0x0221401f },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p3[] = {
	{ 0x12, 0x99a30130 }, { 0x14, 0x90170110 }, { 0x15, 0x0321101f }, { 0x16, 0x03011020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p4[] = {
	{ 0x12, 0x99a30140 }, { 0x14, 0x90170110 }, { 0x15, 0x0321101f }, { 0x16, 0x03011020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p5[] = {
	{ 0x12, 0x99a30150 }, { 0x14, 0x90170110 }, { 0x15, 0x0321101f }, { 0x16, 0x03011020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p6[] = {
	{ 0x14, 0x90170110 }, { 0x15, 0x0321101f }, { 0x16, 0x03011020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p7[] = {
	{ 0x12, 0x90a60130 }, { 0x14, 0x90170110 }, { 0x15, 0x0321101f },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p8[] = {
	{ 0x14, 0x01014010 }, { 0x17, 0x90170150 }, { 0x19, 0x02a11060 }, { 0x1b, 0x01813030 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p9[] = {
	{ 0x14, 0x01014010 }, { 0x18, 0x01a19040 }, { 0x1b, 0x01813030 }, { 0x21, 0x02211020 },
	{ 0 }
};
static const struct hda_pincfg alc662_pin_fixup_tbl_p10[] = {
	{ 0x14, 0x01014020 }, { 0x17, 0x90170110 }, { 0x18, 0x01a19050 }, { 0x1b, 0x01813040 }, { 0x21, 0x02211030 },
	{ 0 }
};
/* ALC66x boards recognised by their BIOS pin layout. */
static const struct alc_pin_quirk alc662_pin_quirks[] = {
	{ 0x10ec0867, 0x1028, 1, R_ALC891_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p0 },
	{ 0x10ec0867, 0x1028, 1, R_ALC891_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p1 },
	{ 0x10ec0662, 0x1028, 1, R_ALC662_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p2 },
	{ 0x10ec0668, 0x1028, 1, R_ALC668_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p3 },
	{ 0x10ec0668, 0x1028, 1, R_ALC668_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p4 },
	{ 0x10ec0668, 0x1028, 1, R_ALC668_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p5 },
	{ 0x10ec0668, 0x1028, 1, R_ALC668_DELL_MIC_NO_PRESENCE, "Dell", alc662_pin_fixup_tbl_p6 },
	{ 0x10ec0668, 0x1028, 1, R_ALC668_DELL_MIC_NO_PRESENCE, "Dell XPS 15", alc662_pin_fixup_tbl_p7 },
	{ 0x10ec0671, 0x103c, 1, R_ALC671_HP_HEADSET_MIC2, "HP cPC", alc662_pin_fixup_tbl_p8 },
	{ 0x10ec0671, 0x103c, 1, R_ALC671_HP_HEADSET_MIC2, "HP cPC", alc662_pin_fixup_tbl_p9 },
	{ 0x10ec0671, 0x103c, 1, R_ALC671_HP_HEADSET_MIC2, "HP cPC", alc662_pin_fixup_tbl_p10 },
	{ 0 }
};

static const struct hda_pincfg alc882_pin_fixup_tbl_p0[] = {
	{ 0x14, 0x01014010 }, { 0x15, 0x01011012 }, { 0x16, 0x01016011 }, { 0x18, 0x01a19040 }, { 0x19, 0x02a19050 }, { 0x1a, 0x0181304f }, { 0x1b, 0x0221401f }, { 0x1e, 0x01456130 },
	{ 0 }
};
static const struct hda_pincfg alc882_pin_fixup_tbl_p1[] = {
	{ 0x14, 0x01015010 }, { 0x15, 0x01011012 }, { 0x16, 0x01011011 }, { 0x18, 0x01a11040 }, { 0x19, 0x02a19050 }, { 0x1a, 0x0181104f }, { 0x1b, 0x0221401f }, { 0x1e, 0x01451130 },
	{ 0 }
};
/* ALC88x boards recognised by their BIOS pin layout. */
static const struct alc_pin_quirk alc882_pin_quirks[] = {
	{ 0x10ec1220, 0x1043, 1, R_ALC1220_CLEVO_P950, "ASUS", alc882_pin_fixup_tbl_p0 },
	{ 0x10ec1220, 0x1462, 1, R_ALC1220_CLEVO_P950, "MS-7C35", alc882_pin_fixup_tbl_p1 },
	{ 0 }
};


/* ==========================================================================
 * Per-codec state
 * ========================================================================== */

struct alc_state {
	const struct alc_chip *chip;
	uint32_t vendor_id;
	uint8_t family, type;
	uint16_t coef0;
	int coef0_valid;

	/* identification */
	uint32_t pci_ssid; /* PCI subsystem (vendor << 16 | device), 0 = none */
	uint16_t pci_ssvid, pci_ssdid;
	uint16_t ctrl_vendor; /* PCI vendor of the HDA controller */
	uint32_t codec_ssid;

	/* the board fixup in effect */
	const struct alc_recipe *rcp;
	const char *board; /* name from the quirk table */
	const char *match_kind; /* "PCI SSID", "codec SSID", "pin layout", ... */
	uint64_t ops;

	/* BIOS SKU / customisation word */
	uint32_t sku;
	int sku_valid;
	int sku_platform_laptop;
	int sku_hp_port; /* -1 none, else 0..3 */

	/* amplifier bring-up */
	int init_amp; /* ALC_INIT_* */
	uint32_t gpio_mask, gpio_dir, gpio_data;
	int gpio_write_delay;
	uint8_t pll_nid, pll_coef_idx, pll_coef_bit;

	/* chip behaviour */
	uint8_t aamix_nid; /* analog loopback mixer, 0 if none */
	int en_3kpull_low;
	int ultra_low_power;
	int no_shutup_pins;
	int no_internal_mic_pin;
	int has_dsp5505;
	int done_hp_init;

	/* pins found after parsing */
	uint8_t hp_pin; /* primary headphone / combo jack */
	uint8_t hp2_pin; /* second headphone (dock, front) */
	uint8_t hs_mic_pin; /* microphone ring of the combo jack */
	uint8_t hp_mic_pin; /* headphone jack used as mic input */
	uint8_t int_mic_pin;
	uint8_t spk_pin[4];
	int nspk;

	/* headset state machine */
	int hs_mode, hs_type;
	int hs_capable; /* chip has headset COEFs and the board uses them */

	/* LEDs */
	const struct alc_led *mute_led, *micmute_led;
	int mute_led_state; /* -1 = unknown */

	/* Samsung speaker amplifiers behind the COEF bridge */
	int samsung_amps; /* v2 amplifier count, 0 = none */
	int samsung_amps_on;

	/* misc */
	int hp_present; /* last reported */
	int playing;
	uint32_t init_count;
	int ext_amp_warned;
};

static inline struct alc_state *alc_st(struct hda_codec *c)
{
	return (struct alc_state *)c->vendor_data;
}

static void alc_strlcpy(char *dst, const char *src, size_t size)
{
	size_t i = 0;

	if (!dst || !size)
		return;
	if (src) {
		while (i + 1 < size && src[i]) {
			dst[i] = src[i];
			i++;
		}
	}
	dst[i] = 0;
}

static int alc_streq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* ==========================================================================
 * Delays
 *
 * Depop sequences and headset detection wait for analog circuits to settle
 * (tens to hundreds of milliseconds).  Sleep when we are an ordinary task
 * with interrupts on; busy-wait during early boot or when a caller holds a
 * spinlock.
 * ========================================================================== */

static void alc_msleep(uint32_t ms)
{
	task_t *cur;

	if (!ms)
		return;
	if (ms > 2000)
		ms = 2000; /* never stall a jack event for longer */
	cur = sched_current();
	if (irqs_enabled() && cur && !sched_task_hidden(cur)) {
		uint64_t deadline = hrtimer_now_ns() + (uint64_t)ms * 1000000ull;
		uint64_t now;

		if (hrtimer_sleep_until(deadline, NULL) == 0)
			return;
		/* a signal cut the sleep short: the hardware still needs the
		 * full time */
		now = hrtimer_now_ns();
		if (now < deadline)
			lapic_delay_us((uint32_t)((deadline - now + 999) / 1000));
		return;
	}
	lapic_delay_ms(ms);
}

/* ==========================================================================
 * COEF access
 *
 * Realtek codecs hide most of their analog configuration behind an index /
 * value register pair on vendor widget 0x20 (and a few further widgets for
 * the headphone amp, depop engine and AGC).  All values are 16 bit.
 * ========================================================================== */

static uint32_t alc_coefex_read(struct hda_codec *c, uint8_t nid,
				uint16_t idx)
{
	uint32_t v = hda_coef_read(c, nid, idx);

	if (v == ALC_NORESP)
		return ALC_NORESP;
	return v & 0xffff;
}

static void alc_coefex_write(struct hda_codec *c, uint8_t nid, uint16_t idx,
			     uint16_t val)
{
	hda_coef_write(c, nid, idx, val);
}

/* Read-modify-write; skipped when the codec does not answer, so a dead
 * codec never gets garbage written into it. */
static void alc_coefex_update(struct hda_codec *c, uint8_t nid, uint16_t idx,
			      uint16_t mask, uint16_t set)
{
	uint32_t v = alc_coefex_read(c, nid, idx);

	if (v == ALC_NORESP)
		return;
	alc_coefex_write(c, nid, idx, (uint16_t)((v & ~(uint32_t)mask) | set));
}

static inline uint32_t alc_coef_read(struct hda_codec *c, uint16_t idx)
{
	return alc_coefex_read(c, ALC_COEF_NID, idx);
}

static inline void alc_coef_write(struct hda_codec *c, uint16_t idx,
				  uint16_t val)
{
	alc_coefex_write(c, ALC_COEF_NID, idx, val);
}

static inline void alc_coef_update(struct hda_codec *c, uint16_t idx,
				   uint16_t mask, uint16_t set)
{
	alc_coefex_update(c, ALC_COEF_NID, idx, mask, set);
}

/* COEF programs: a mask of 0xffff writes the value outright, anything else
 * is a read-modify-write of the masked bits. */
#define CW(i, v) { ALC_COEF_NID, (i), 0xffff, (v) }
#define CU(i, m, v) { ALC_COEF_NID, (i), (m), (v) }
#define CXW(n, i, v) { (n), (i), 0xffff, (v) }
#define CXU(n, i, m, v) { (n), (i), (m), (v) }
#define CEND { 0, 0, 0, 0 }

static void alc_coef_seq(struct hda_codec *c, const struct hda_coef_fix *f)
{
	for (; f && f->nid; f++) {
		if (f->mask == 0xffff)
			alc_coefex_write(c, f->nid, f->idx, f->val);
		else
			alc_coefex_update(c, f->nid, f->idx, f->mask, f->val);
	}
}

/* COEF 0 holds the silicon revision; it never changes, read it once. */
static uint16_t alc_coef0(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint32_t v;

	if (st && st->coef0_valid)
		return st->coef0;
	v = alc_coef_read(c, 0);
	if (v == ALC_NORESP)
		v = 0;
	if (st) {
		st->coef0 = (uint16_t)v;
		st->coef0_valid = 1;
	}
	return (uint16_t)v;
}

/* ==========================================================================
 * Widgets, pins, GPIO, EAPD
 * ========================================================================== */

static struct hda_widget *alc_pin(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w;

	if (!nid)
		return NULL;
	w = hda_widget(c, nid);
	if (!w || w->type != AC_WID_PIN)
		return NULL;
	return w;
}

static uint32_t alc_pin_cfg(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = alc_pin(c, nid);

	return w ? w->pin_cfg : 0;
}

static inline uint32_t cfg_device(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_DEVICE) >> AC_DEFCFG_DEVICE_SHIFT;
}

static inline uint32_t cfg_port(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT;
}

static inline uint32_t cfg_location(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_LOCATION) >> AC_DEFCFG_LOCATION_SHIFT;
}

static inline int cfg_unconnected(uint32_t cfg)
{
	return cfg_port(cfg) == AC_JACK_PORT_NONE;
}

/* A built-in device (speaker, internal mic) rather than a jack. */
static inline int cfg_internal(uint32_t cfg)
{
	return cfg_port(cfg) == AC_JACK_PORT_FIXED ||
	       (cfg_location(cfg) & 0x30) == AC_JACK_LOC_INTERNAL;
}

static void alc_set_pinctl(struct hda_codec *c, uint8_t nid, uint8_t ctl)
{
	if (alc_pin(c, nid))
		hda_pin_set_ctl(c, nid, ctl);
}

static void alc_raw_pinctl(struct hda_codec *c, uint8_t nid, uint8_t ctl)
{
	if (alc_pin(c, nid))
		hda_codec_write(c, nid, AC_VERB_SET_PIN_WIDGET_CONTROL, ctl);
}

static void alc_raw_amp(struct hda_codec *c, uint8_t nid, uint16_t payload)
{
	if (hda_widget(c, nid))
		hda_codec_write(c, nid, AC_VERB_SET_AMP_GAIN_MUTE, payload);
}

/* The control a headphone pin should end with after a depop sequence.  The
 * vendor init runs after the generic init, so the cached value is the
 * generic code's decision (enabled with HP drive, or off when the output is
 * not selected); the sequence ends there and hardware and cache agree. */
static uint8_t alc_hp_target(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = alc_pin(c, nid);

	if (!w)
		return PINCTL_HP;
	return w->pinctl;
}

static int alc_jack_present(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = alc_pin(c, nid);

	if (!w || !(w->pincap & AC_PINCAP_PRES_DETECT))
		return 0;
	if (w->pin_cfg & (AC_DEFCFG_MISC_NO_PRESENCE << AC_DEFCFG_MISC_SHIFT))
		return 0;
	return hda_pin_sense(c, nid) > 0;
}

/* Sense regardless of the misc "no presence detect" bit: the depop
 * sequences only need to know whether something is plugged in. */
static int alc_jack_present_raw(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = alc_pin(c, nid);

	if (!w || !(w->pincap & AC_PINCAP_PRES_DETECT))
		return 0;
	return hda_pin_sense(c, nid) > 0;
}

static void alc_setup_gpio(struct alc_state *st, uint32_t mask)
{
	st->gpio_mask |= mask;
	st->gpio_dir |= mask;
	st->gpio_data |= mask;
}

static void alc_write_gpio(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (!st || !st->gpio_mask || !c->afg)
		return;
	if (st->gpio_write_delay) {
		/* some Mac Pro boards need the direction to settle before
		 * data is driven */
		hda_gpio_set(c, st->gpio_mask, st->gpio_dir,
			     c->gpio_data & st->gpio_mask);
		alc_msleep(1);
	}
	hda_gpio_set(c, st->gpio_mask, st->gpio_dir, st->gpio_data);
}

static void alc_gpio_update(struct hda_codec *c, uint32_t mask, int on)
{
	struct alc_state *st = alc_st(c);
	uint32_t old;

	if (!st || !mask)
		return;
	old = st->gpio_data;
	st->gpio_mask |= mask;
	st->gpio_dir |= mask;
	if (on)
		st->gpio_data |= mask;
	else
		st->gpio_data &= ~mask;
	if (old != st->gpio_data || (c->gpio_mask & mask) != mask)
		hda_gpio_set(c, st->gpio_mask, st->gpio_dir, st->gpio_data);
}

/* EAPD (external amplifier power down) lines live on the output pins. */
static const uint8_t alc_eapd_pins[] = { 0x0f, 0x10, 0x14, 0x15, 0x17, 0 };

static void alc_setup_eapd(struct hda_codec *c, int on)
{
	const uint8_t *p;

	for (p = alc_eapd_pins; *p; p++) {
		struct hda_widget *w = alc_pin(c, *p);

		if (w && (w->pincap & AC_PINCAP_EAPD))
			hda_set_eapd(c, *p, on);
	}
}

/* PLL workaround: some revisions need one COEF bit cleared at each init. */
static void alc_fix_pll(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (st && st->pll_nid)
		alc_coefex_update(c, st->pll_nid, st->pll_coef_idx,
				  (uint16_t)(1u << st->pll_coef_bit), 0);
}

static void alc_fix_pll_init(struct hda_codec *c, uint8_t nid, uint8_t idx,
			     uint8_t bit)
{
	struct alc_state *st = alc_st(c);

	st->pll_nid = nid;
	st->pll_coef_idx = idx;
	st->pll_coef_bit = bit;
	alc_fix_pll(c);
}

/* ==========================================================================
 * EAPD under verb control
 *
 * Out of reset many Realtek parts drive EAPD from an internal state machine.
 * Hand it to the EAPD/BTL verb so the driver decides when the speaker amp
 * is powered.  Also clears the "0x10 bit 9" power-down latch on the newer
 * parts, without which speakers stay silent on some boards.
 * ========================================================================== */

static void alc_fill_eapd_coef(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint16_t coef = alc_coef0(c);

	switch (st->vendor_id) {
	case 0x10ec0262:
		alc_coef_update(c, 0x07, 0, 1 << 5);
		break;
	case 0x10ec0267:
	case 0x10ec0268:
		alc_coef_update(c, 0x07, 0, 1 << 13);
		break;
	case 0x10ec0269:
		switch (coef & 0x00f0) {
		case 0x0010:
			alc_coef_update(c, 0x0d, 0, 1 << 14);
			break;
		case 0x0020:
			alc_coef_update(c, 0x04, 1 << 15, 0);
			break;
		case 0x0030:
			alc_coef_update(c, 0x10, 1 << 9, 0);
			break;
		}
		break;
	case 0x10ec0280:
	case 0x10ec0284:
	case 0x10ec0290:
	case 0x10ec0292:
		alc_coef_update(c, 0x04, 1 << 15, 0);
		break;
	case 0x10ec0225:
	case 0x10ec0295:
	case 0x10ec0299:
		alc_coef_update(c, 0x67, 0xf000, 0x3000);
		alc_coef_update(c, 0x36, 1 << 13, 0);
		alc_coef_update(c, 0x10, 1 << 9, 0);
		break;
	case 0x10ec0215:
	case 0x10ec0236:
	case 0x10ec0245:
	case 0x10ec0256:
	case 0x10ec0257:
	case 0x10ec0285:
	case 0x10ec0289:
		alc_coef_update(c, 0x36, 1 << 13, 0);
		alc_coef_update(c, 0x10, 1 << 9, 0);
		break;
	case 0x10ec0230:
	case 0x10ec0233:
	case 0x10ec0235:
	case 0x10ec0255:
	case 0x19e58326:
	case 0x10ec0282:
	case 0x10ec0283:
	case 0x10ec0286:
	case 0x10ec0288:
	case 0x10ec0298:
	case 0x10ec0300:
		alc_coef_update(c, 0x10, 1 << 9, 0);
		break;
	case 0x10ec0275:
		alc_coef_update(c, 0x0e, 0, 1 << 0);
		break;
	case 0x10ec0287:
		alc_coef_update(c, 0x10, 1 << 9, 0);
		alc_coef_write(c, 0x08, 0x4ab7);
		break;
	case 0x10ec0293:
		alc_coef_update(c, 0x0a, 1 << 13, 0);
		break;
	case 0x10ec0234:
	case 0x10ec0274:
		alc_coef_write(c, 0x6e, 0x0c25);
		alc_coef_update(c, 0x10, 1 << 15, 0);
		break;
	case 0x10ec0294:
	case 0x10ec0700:
	case 0x10ec0701:
	case 0x10ec0703:
	case 0x10ec0711:
		alc_coef_update(c, 0x10, 1 << 15, 0);
		break;
	case 0x10ec0662:
		if ((coef & 0x00f0) == 0x0030)
			alc_coef_update(c, 0x04, 1 << 10, 0);
		break;
	case 0x10ec0272:
	case 0x10ec0273:
	case 0x10ec0663:
	case 0x10ec0665:
	case 0x10ec0670:
	case 0x10ec0671:
	case 0x10ec0672:
		alc_coef_update(c, 0x0d, 0, 1 << 14);
		break;
	case 0x10ec0222:
	case 0x10ec0623:
		alc_coef_update(c, 0x19, 1 << 13, 0);
		break;
	case 0x10ec0668:
		alc_coef_update(c, 0x07, 3 << 13, 0);
		break;
	case 0x10ec0867:
		alc_coef_update(c, 0x04, 1 << 10, 0);
		break;
	case 0x10ec0888:
		if ((coef & 0x00f0) == 0x0020 || (coef & 0x00f0) == 0x0030)
			alc_coef_update(c, 0x07, 1 << 5, 0);
		break;
	case 0x10ec0892:
	case 0x10ec0897:
		alc_coef_update(c, 0x07, 1 << 5, 0);
		break;
	case 0x10ec0899:
	case 0x10ec0900:
	case 0x10ec0b00:
	case 0x10ec1168:
	case 0x10ec1220:
		alc_coef_update(c, 0x07, 1 << 1, 0);
		break;
	}
}

/* Default amplifier COEFs of the desktop families, applied with EAPD. */
static void alc_auto_init_amp(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	alc_setup_eapd(c, 1);
	alc_write_gpio(c);
	if (st->init_amp != ALC_INIT_DEFAULT)
		return;
	switch (st->vendor_id) {
	case 0x10ec0260:
		alc_coefex_update(c, ALC_COEFEX_ALC260, 7, 0, 0x2010);
		break;
	case 0x10ec0880:
	case 0x10ec0882:
	case 0x10ec0883:
	case 0x10ec0885:
		alc_coef_update(c, 7, 0, 0x2030);
		break;
	case 0x10ec0888:
		/* ALC888-VA and -VB: EAPD output high */
		switch (alc_coef0(c) & 0x00f0) {
		case 0x0000:
		case 0x0010:
			alc_coef_update(c, 7, 0, 0x2030);
			break;
		}
		break;
	}
}

/* ==========================================================================
 * Family reset values and depop sequences
 * ========================================================================== */

/* ALC282: power-down control, clocks, jack detect thresholds, capless HP
 * amp and class-D speaker amp defaults. */
static const struct hda_coef_fix alc282_reset_coefs[] = {
	CW(0x03, 0x0002), /* power down control */
	CU(0x05, 0xff3f, 0x0700), /* FIFO and filter clock */
	CW(0x07, 0x0200), /* DMIC control */
	CU(0x06, 0x00f0, 0), /* analog clock */
	CU(0x08, 0xfffc, 0x0c2c), /* jack detect */
	CW(0x0a, 0xcccc), /* JD offset 1 */
	CW(0x0b, 0xcccc), /* JD offset 2 */
	CW(0x0e, 0x6e00), /* LDO1/2/3, DAC/ADC */
	CU(0x0f, 0xf800, 0x1000), /* JD */
	CU(0x10, 0xfc00, 0x0c00), /* capless */
	CW(0x6f, 0x0), /* class-D test 4 */
	CU(0x0c, 0xfe00, 0), /* IO power down directly */
	CW(0x34, 0xa0c0), /* ANC */
	CU(0x16, 0x0008, 0), /* AGC mux */
	CU(0x1d, 0x00e0, 0), /* DAC simple content protection */
	CU(0x1f, 0x00e0, 0), /* ADC simple content protection */
	CW(0x21, 0x8804), /* DAC/ADC zero detection */
	CW(0x63, 0x2902), /* PLL */
	CW(0x68, 0xa080), /* capless control 2 */
	CW(0x69, 0x3400), /* capless control 3 */
	CW(0x6a, 0x2f3e), /* capless control 4 */
	CW(0x6b, 0x0), /* capless control 5 */
	CU(0x6d, 0x0fff, 0x0900), /* class-D test 2 */
	CW(0x6e, 0x110a), /* class-D test 3 */
	CU(0x70, 0x00f8, 0x00d8), /* class-D test 5 */
	CW(0x71, 0x0014), /* class-D test 6 */
	CW(0x72, 0xc2ba), /* class-D over-current protection */
	CU(0x77, 0x0f80, 0), /* class-D pure DC test */
	CW(0x6c, 0xfc06), /* class-D amp control */
	CEND
};

/* ALC283 / ALC233: same blocks, different register map. */
static const struct hda_coef_fix alc283_reset_coefs[] = {
	CW(0x03, 0x0002), /* power down control */
	CU(0x05, 0xff3f, 0x0700), /* FIFO and filter clock */
	CW(0x07, 0x0200), /* DMIC control */
	CU(0x06, 0x00f0, 0), /* analog clock */
	CU(0x08, 0xfffc, 0x0c2c), /* jack detect */
	CW(0x0a, 0xcccc), /* JD offset 1 */
	CW(0x0b, 0xcccc), /* JD offset 2 */
	CW(0x0e, 0x6fc0), /* LDO1/2/3, DAC/ADC */
	CU(0x0f, 0xf800, 0x1000), /* JD */
	CU(0x10, 0xfc00, 0x0c00), /* capless */
	CW(0x3a, 0x0), /* class-D test 4 */
	CU(0x0c, 0xfe00, 0x0), /* IO power down directly */
	CW(0x22, 0xa0c0), /* ANC */
	CXU(ALC_COEFEX_AGC, 0x01, 0x000f, 0x0008), /* AGC mux */
	CU(0x1d, 0x00e0, 0), /* DAC simple content protection */
	CU(0x1f, 0x00e0, 0), /* ADC simple content protection */
	CW(0x21, 0x8804), /* DAC/ADC zero detection */
	CW(0x2e, 0x2902), /* PLL */
	CW(0x33, 0xa080), /* capless control 2 */
	CW(0x34, 0x3400), /* capless control 3 */
	CW(0x35, 0x2f3e), /* capless control 4 */
	CW(0x36, 0x0), /* capless control 5 */
	CU(0x38, 0x0fff, 0x0900), /* class-D test 2 */
	CW(0x39, 0x110a), /* class-D test 3 */
	CU(0x3b, 0x00f8, 0x00d8), /* class-D test 5 */
	CW(0x3c, 0x0014), /* class-D test 6 */
	CW(0x3d, 0xc2ba), /* class-D over-current protection */
	CU(0x42, 0x0f80, 0x0), /* class-D pure DC test */
	CW(0x49, 0x0), /* test mode */
	CU(0x40, 0xf800, 0x9800), /* class-D DC enable */
	CU(0x42, 0xf000, 0x2000), /* DC offset */
	CW(0x37, 0xfc06), /* class-D amp control */
	CU(0x1b, 0x8000, 0), /* HP jack detect control */
	CEND
};

/* ALC668: full analog default set (the BIOS of some Dell desktops leaves
 * the part in a test configuration). */
static const struct hda_coef_fix alc668_reset_coefs[] = {
	CW(0x01, 0xbebe), CW(0x02, 0xaaaa), CW(0x03, 0x0),
	CW(0x04, 0x0180), CW(0x06, 0x0), CW(0x07, 0x0f80),
	CW(0x08, 0x0031), CW(0x0a, 0x0060), CW(0x0b, 0x0),
	CW(0x0c, 0x7cf7), CW(0x0d, 0x1080), CW(0x0e, 0x7f7f),
	CW(0x0f, 0xcccc), CW(0x10, 0xddcc), CW(0x11, 0x0001),
	CW(0x13, 0x0), CW(0x14, 0x2aa0), CW(0x17, 0xa940),
	CW(0x19, 0x0), CW(0x1a, 0x0), CW(0x1b, 0x0),
	CW(0x1c, 0x0), CW(0x1d, 0x0), CW(0x1e, 0x7418),
	CW(0x1f, 0x0804), CW(0x20, 0x4200), CW(0x21, 0x0468),
	CW(0x22, 0x8ccc), CW(0x23, 0x0250), CW(0x24, 0x7418),
	CW(0x27, 0x0), CW(0x28, 0x8ccc), CW(0x2a, 0xff00),
	CW(0x2b, 0x8000), CW(0xa7, 0xff00), CW(0xa8, 0x8000),
	CW(0xaa, 0x2e17), CW(0xab, 0xa0c0), CW(0xac, 0x0),
	CW(0xad, 0x0), CW(0xae, 0x2ac6), CW(0xaf, 0xa480),
	CW(0xb0, 0x0), CW(0xb1, 0x0), CW(0xb2, 0x0),
	CW(0xb3, 0x0), CW(0xb4, 0x0), CW(0xb5, 0x1040),
	CW(0xb6, 0xd697), CW(0xb7, 0x902b), CW(0xb8, 0xd697),
	CW(0xb9, 0x902b), CW(0xba, 0xb8ba), CW(0xbb, 0xaaab),
	CW(0xbc, 0xaaaf), CW(0xbd, 0x6aaa), CW(0xbe, 0x1c02),
	CW(0xc0, 0x00ff), CW(0xc1, 0x0fa6),
	CEND
};

static uint8_t alc_hp_pin_or(struct hda_codec *c, uint8_t fallback)
{
	struct alc_state *st = alc_st(c);

	if (st->hp_pin)
		return st->hp_pin;
	return alc_pin(c, fallback) ? fallback : 0;
}

/* ALC269VB: the early silicon revisions need their capless and class-D
 * blocks trimmed. */
static void alc269_fill_coef(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint16_t rev = alc_coef0(c) & 0x00ff;
	uint32_t v;

	if (st->type != ALC_T_269VB)
		return;
	if (rev < 0x015) {
		alc_coef_write(c, 0x0f, 0x960b);
		alc_coef_write(c, 0x0e, 0x8817);
	}
	if (rev == 0x016) {
		alc_coef_write(c, 0x0f, 0x960b);
		alc_coef_write(c, 0x0e, 0x8814);
	}
	if (rev == 0x017)
		alc_coef_update(c, 0x04, 0, 1 << 11); /* output pin power */
	if (rev == 0x018) {
		v = alc_coef_read(c, 0x0d);
		if (v != ALC_NORESP && ((v & 0x0c00) >> 10) != 0x1)
			alc_coef_write(c, 0x0d, (uint16_t)(v | (1 << 10)));
		v = alc_coef_read(c, 0x17);
		if (v != ALC_NORESP && ((v & 0x01c0) >> 6) != 0x4)
			alc_coef_write(c, 0x17, (uint16_t)(v | (1 << 7)));
	}
	/* headphone output power */
	alc_coef_update(c, 0x04, 0, 1 << 11);
}

/* ALC282: reset values, then bring the capless headphone amp up in high
 * power mode around the pin enable so plugging in does not pop. */
static void alc282_init(struct hda_codec *c)
{
	uint8_t hp = alc_hp_pin_or(c, 0);
	uint32_t coef78;
	int sense;

	alc_coef_seq(c, alc282_reset_coefs);
	if (!hp)
		return;
	sense = alc_jack_present_raw(c, hp);
	coef78 = alc_coef_read(c, 0x78);
	alc_coef_write(c, 0x78, 0x9004); /* capless: high power */
	if (sense)
		alc_msleep(2);
	alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
	if (sense)
		alc_msleep(85);
	alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
	if (sense)
		alc_msleep(100);
	if (coef78 != ALC_NORESP)
		alc_coef_write(c, 0x78, (uint16_t)coef78);
}

static void alc283_init(struct hda_codec *c)
{
	uint8_t hp = alc_hp_pin_or(c, 0);
	int sense;

	alc_coef_seq(c, alc283_reset_coefs);
	if (!hp)
		return;
	alc_msleep(30);
	sense = alc_jack_present_raw(c, hp);
	alc_coef_write(c, 0x43, 0x9004); /* capless: high power */
	alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
	if (sense)
		alc_msleep(85);
	alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
	if (sense)
		alc_msleep(85);
	/* combo jack: release the 3k pull-down */
	alc_coef_update(c, 0x46, 3 << 12, 0);
	alc_coef_write(c, 0x43, 0x9614); /* capless: normal */
}

/* ALC255/256/257: optional ultra-low-power exit, then the direct-drive
 * headphone amp is ramped from low to high power around unmute. */
static void alc256_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint8_t hp = alc_hp_pin_or(c, 0x21);
	int sense;

	if (st->ultra_low_power) {
		alc_coef_update(c, 0x03, 1 << 1, 1 << 1);
		alc_coef_update(c, 0x08, 3 << 2, 3 << 2);
		alc_coef_update(c, 0x08, 7 << 4, 0);
		alc_coef_update(c, 0x3b, 1 << 15, 0);
		alc_coef_update(c, 0x0e, 7 << 6, 7 << 6);
		alc_msleep(30);
	}
	alc_msleep(30);
	sense = hp ? alc_jack_present_raw(c, hp) : 0;
	if (sense) {
		alc_msleep(2);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x04, 0x0007, 0x1);
		alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
		alc_msleep(75);
		alc_raw_amp(c, hp, AMP_OUT_UNMUTE_ALL);
		alc_msleep(75);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x04, 0x0007, 0x4);
	}
	alc_coef_update(c, 0x46, 3 << 12, 0);
	/* pulse the AGC clear bit */
	alc_coefex_update(c, ALC_COEFEX_AGC, 0x02, 0x8000, 1 << 15);
	alc_coefex_update(c, ALC_COEFEX_AGC, 0x02, 0x8000, 0);
	/* pin 0x1a is the headphone-mic input, not PC beep, and it is
	 * removed from the analog loopback of every output */
	alc_coef_write(c, 0x36, 0x5757);
}

/* ALC285 class: the headphone depop engine runs once after power-up.  It
 * calibrates the charge pump; the result is written back. */
static void alc285_hp_init(struct hda_codec *c)
{
	uint8_t hp = alc_hp_pin_or(c, 0);
	uint32_t coef38, coef0d, coef36, v;
	int i;

	alc_coefex_write(c, ALC_COEFEX_DEPOP, 0x00, 0x1888);
	alc_coef_update(c, 0x4a, 1 << 15, 1 << 15); /* reset HP JD */
	coef38 = alc_coef_read(c, 0x38); /* amp control */
	coef0d = alc_coef_read(c, 0x0d); /* digital misc */
	coef36 = alc_coef_read(c, 0x36); /* passthrough */
	alc_coef_update(c, 0x38, 1 << 4, 0);
	alc_coef_update(c, 0x0d, 0x110, 0);
	alc_coef_update(c, 0x67, 0xf000, 0x3000);
	if (hp)
		alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
	alc_msleep(130);
	alc_coef_update(c, 0x36, 1 << 14, 1 << 14);
	alc_coef_update(c, 0x36, 1 << 13, 0);
	if (hp)
		alc_raw_pinctl(c, hp, PINCTL_OFF);
	alc_msleep(10);
	alc_coef_write(c, 0x67, 0x0); /* depop: manual mode */
	alc_coefex_write(c, ALC_COEFEX_DEPOP, 0x00, 0x7880);
	alc_coefex_write(c, ALC_COEFEX_DEPOP, 0x0f, 0xf049);
	alc_coefex_update(c, ALC_COEFEX_DEPOP, 0x03, 0x00f0, 0x00c0);
	alc_coefex_write(c, ALC_COEFEX_DEPOP, 0x00, 0xf888); /* start */
	v = alc_coefex_read(c, ALC_COEFEX_DEPOP, 0x00);
	for (i = 0; i < 20 && v != ALC_NORESP && (v & 0x8000); i++) {
		alc_msleep(50);
		v = alc_coefex_read(c, ALC_COEFEX_DEPOP, 0x00);
	}
	if (v != ALC_NORESP)
		alc_coefex_write(c, ALC_COEFEX_DEPOP, 0x00, (uint16_t)v);
	if (coef38 != ALC_NORESP)
		alc_coef_update(c, 0x38, 1 << 4, (uint16_t)coef38 & (1 << 4));
	if (coef0d != ALC_NORESP)
		alc_coef_update(c, 0x0d, 0x110, (uint16_t)coef0d & 0x110);
	if (coef36 != ALC_NORESP)
		alc_coef_update(c, 0x36, 3 << 13, (uint16_t)coef36 & (3 << 13));
	alc_msleep(50);
	alc_coef_update(c, 0x4a, 1 << 15, 0);
}

/* ALC215/225/245/285/287/289/295/299: two possible headphone pins (0x21
 * and the second jack on 0x16). */
static void alc225_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint8_t hp = alc_hp_pin_or(c, 0x21);
	int s1, s2;

	if (st->ultra_low_power) {
		alc_coef_update(c, 0x08, 0x0f << 2, 3 << 2);
		alc_coef_update(c, 0x0e, 7 << 6, 7 << 6);
		alc_coef_update(c, 0x33, 1 << 11, 0);
		alc_msleep(30);
	}
	if (st->type != ALC_T_287 && st->type != ALC_T_245 &&
	    !st->done_hp_init) {
		alc285_hp_init(c);
		st->done_hp_init = 1;
	}
	alc_msleep(30);
	s1 = hp ? alc_jack_present_raw(c, hp) : 0;
	s2 = alc_jack_present_raw(c, 0x16);
	if (s1 || s2) {
		alc_msleep(2);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x04, 0x0007, 0x1);
		if (s1)
			alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
		if (s2)
			alc_raw_pinctl(c, 0x16, alc_hp_target(c, 0x16));
		alc_msleep(75);
		if (s1)
			alc_raw_amp(c, hp, AMP_OUT_UNMUTE_ALL);
		if (s2)
			alc_raw_amp(c, 0x16, AMP_OUT_UNMUTE_ALL);
		alc_msleep(75);
		alc_coef_update(c, 0x4a, 3 << 10, 0);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x04, 0x0007, 0x4);
	}
}

/* ALC222/623: headphone on the HP pin or on 0x14. */
static void alc222_init(struct hda_codec *c)
{
	uint8_t hp = alc_hp_pin_or(c, 0);
	int s1, s2;

	if (!hp)
		return;
	alc_msleep(30);
	s1 = alc_jack_present_raw(c, hp);
	s2 = alc_jack_present_raw(c, 0x14);
	if (!s1 && !s2)
		return;
	alc_msleep(2);
	if (s1)
		alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
	if (s2)
		alc_raw_pinctl(c, 0x14, alc_hp_target(c, 0x14));
	alc_msleep(75);
	if (s1)
		alc_raw_amp(c, hp, AMP_OUT_UNMUTE_ALL);
	if (s2)
		alc_raw_amp(c, 0x14, AMP_OUT_UNMUTE_ALL);
	alc_msleep(75);
}

static void alc_default_init(struct hda_codec *c)
{
	uint8_t hp = alc_hp_pin_or(c, 0);

	if (!hp)
		return;
	alc_msleep(30);
	if (!alc_jack_present_raw(c, hp))
		return;
	alc_msleep(2);
	alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
	alc_msleep(75);
	alc_raw_amp(c, hp, AMP_OUT_UNMUTE_ALL);
	alc_msleep(75);
}

/* ALC234/274/294 and ALC700 class: run the depop engine once, in manual
 * mode, then hand it back to automatic mode. */
static void alc294_hp_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint8_t hp = alc_hp_pin_or(c, 0);
	uint32_t v;
	int i;

	if (!hp)
		return;
	alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
	alc_msleep(100);
	if (!st->no_shutup_pins)
		alc_raw_pinctl(c, hp, PINCTL_OFF);
	alc_coef_update(c, 0x6f, 0x000f, 0); /* depop: manual */
	alc_coefex_update(c, ALC_COEFEX_DEPOP, 0x00, 0x8000, 0x8000); /* go */
	v = alc_coefex_read(c, ALC_COEFEX_DEPOP, 0x01);
	for (i = 0; i < 20 && v != ALC_NORESP && (v & 0x0080); i++) {
		alc_msleep(50);
		v = alc_coefex_read(c, ALC_COEFEX_DEPOP, 0x01);
	}
	alc_coef_update(c, 0x6f, 0x000f, 0x000b); /* depop: automatic */
	alc_msleep(50);
	/* the pin is enabled again by the default sequence / generic init */
	alc_raw_pinctl(c, hp, alc_hp_target(c, hp));
}

static void alc294_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (!st->done_hp_init) {
		alc294_hp_init(c);
		st->done_hp_init = 1;
	}
	alc_default_init(c);
}

/* ---- ALC5505 companion DSP --------------------------------------------- */

/* The DSP registers are 32 bit, reached through widget 0x51 with a 16-bit
 * index (register address / 2) and two 16-bit data halves. */
static void alc5505_set(struct hda_codec *c, uint32_t reg, uint32_t val)
{
	hda_codec_write(c, ALC_DSP5505_NID, AC_VERB_SET_COEF_INDEX,
			(reg >> 1) & 0xffff);
	hda_codec_write(c, ALC_DSP5505_NID, AC_VERB_SET_PROC_COEF,
			val & 0xffff);
	hda_codec_write(c, ALC_DSP5505_NID, AC_VERB_SET_PROC_COEF,
			(val >> 16) & 0xffff);
}

static uint32_t alc5505_get(struct hda_codec *c, uint32_t reg)
{
	uint32_t lo, hi;

	hda_codec_write(c, ALC_DSP5505_NID, AC_VERB_SET_COEF_INDEX,
			(reg >> 1) & 0xffff);
	lo = hda_codec_read(c, ALC_DSP5505_NID, AC_VERB_GET_PROC_COEF, 0);
	hi = hda_codec_read(c, ALC_DSP5505_NID, AC_VERB_GET_PROC_COEF, 0);
	if (lo == ALC_NORESP || hi == ALC_NORESP)
		return ALC_NORESP;
	return (lo & 0xffff) | ((hi & 0xffff) << 16);
}

/* Park the DSP: CPU stopped, DRAM in self refresh, PLLs off.  The codec
 * audio path does not depend on it. */
static void alc5505_halt(struct hda_codec *c)
{
	uint32_t v;

	alc5505_set(c, 0x3000, 0x000c); /* CPU stop */
	alc5505_set(c, 0x880c, 0x0008); /* DDR self refresh */
	alc5505_set(c, 0x61c0, 0x11110080); /* PLL and CPU clock */
	alc5505_set(c, 0x6230, 0xfc0d4011); /* input OP off */
	alc5505_set(c, 0x61b4, 0x040a2b03); /* PLL2 stop */
	alc5505_set(c, 0x61b0, 0x00005b17); /* PLL1 stop */
	alc5505_set(c, 0x61b8, 0x04133303); /* PLL3 stop */
	v = alc5505_get(c, 0x6220);
	if (v != ALC_NORESP)
		alc5505_set(c, 0x6220, v | 0x3000); /* ring buffer on DBUS */
}

static void alc5505_unhalt(struct hda_codec *c)
{
	alc5505_set(c, 0x61b8, 0x04133302);
	alc5505_set(c, 0x61b0, 0x00005b16);
	alc5505_set(c, 0x61b4, 0x040a2b02);
	alc5505_set(c, 0x6230, 0xf80d4011);
	alc5505_set(c, 0x6220, 0x2002010f);
	alc5505_set(c, 0x880c, 0x00000004);
}

/* Full DSP bring-up (clocks, DRAM, pads), then halt it again: we have no
 * firmware for it, but it must be in a defined low-power state. */
static void alc5505_init(struct hda_codec *c)
{
	uint32_t rev;

	alc5505_halt(c);
	alc5505_unhalt(c);
	alc5505_set(c, 0x61b0, 0x5b14); /* PLL1 */
	alc5505_set(c, 0x61b0, 0x5b16);
	alc5505_set(c, 0x61b4, 0x04132b00); /* PLL2 */
	alc5505_set(c, 0x61b4, 0x04132b02);
	alc5505_set(c, 0x61b8, 0x041f3300); /* PLL3 */
	alc5505_set(c, 0x61b8, 0x041f3302);
	hda_codec_write(c, ALC_DSP5505_NID, AC_VERB_SET_CODEC_RESET, 0);
	alc5505_set(c, 0x61b8, 0x041b3302);
	alc5505_set(c, 0x61b8, 0x04173302);
	alc5505_set(c, 0x61b8, 0x04163302);
	alc5505_set(c, 0x8800, 0x348b328b); /* DRAM */
	alc5505_set(c, 0x8808, 0x00020022);
	alc5505_set(c, 0x8818, 0x00000400);
	rev = alc5505_get(c, 0x6200);
	if (rev != ALC_NORESP && (rev >> 16) <= 3)
		alc5505_set(c, 0x6220, 0x2002010f); /* IO pads */
	else
		alc5505_set(c, 0x6220, 0x6002018f);
	alc5505_set(c, 0x61ac, 0x055525f0);
	alc5505_set(c, 0x61c0, 0x12230080); /* clocks */
	alc5505_set(c, 0x61b4, 0x040e2b02); /* PLL2 */
	alc5505_set(c, 0x61bc, 0x010234f8); /* oscillator */
	alc5505_set(c, 0x880c, 0x00000004); /* DRAM function */
	alc5505_set(c, 0x880c, 0x00000003);
	alc5505_set(c, 0x880c, 0x00000010);
	alc5505_halt(c);
}

/* The chip's own reset-time initialisation (before fixups and EAPD). */
static void alc_family_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (st->has_dsp5505) {
		alc5505_init(c);
		return;
	}
	switch (st->type) {
	case ALC_T_269VA:
	case ALC_T_269VB:
	case ALC_T_269VC:
	case ALC_T_269VD:
		alc269_fill_coef(c);
		break;
	case ALC_T_282:
		alc282_init(c);
		break;
	case ALC_T_283:
		alc283_init(c);
		break;
	case ALC_T_255:
	case ALC_T_256:
	case ALC_T_257:
		alc256_init(c);
		break;
	case ALC_T_215:
	case ALC_T_225:
	case ALC_T_245:
	case ALC_T_287:
		alc225_init(c);
		break;
	case ALC_T_294:
		alc_coef_update(c, 0x6b, 0x0018, (1 << 4) | (1 << 3));
		alc294_init(c);
		break;
	case ALC_T_700:
		alc_coef_update(c, 0x4a, 1 << 15, 0);
		alc294_init(c);
		break;
	case ALC_T_623:
		alc222_init(c);
		break;
	case ALC_T_668:
		alc_coef_seq(c, alc668_reset_coefs);
		break;
	case ALC_T_662:
	case ALC_T_882:
	case ALC_T_883:
		break;
	default:
		if (st->family == ALC_FAM_269)
			alc_default_init(c);
		break;
	}
}

/* ---- shutdown --------------------------------------------------------------- */

/* Disable every pin except the external mic (its VREF keeps the headset
 * detection alive on the combo-jack parts). */
static void alc_shutup_pins(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	int keep_mic = 0;
	int i;

	if (st->no_shutup_pins)
		return;
	switch (st->vendor_id) {
	case 0x10ec0236:
	case 0x10ec0256:
	case 0x10ec0257:
	case 0x19e58326:
	case 0x10ec0283:
	case 0x10ec0285:
	case 0x10ec0286:
	case 0x10ec0287:
	case 0x10ec0288:
	case 0x10ec0295:
	case 0x10ec0298:
		keep_mic = 1;
		break;
	}
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		if (keep_mic && w->nid == st->hs_mic_pin)
			continue;
		if (cfg_unconnected(w->pin_cfg))
			continue;
		hda_codec_write(c, w->nid, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	}
}

static void alc_hp_shutdown(struct hda_codec *c, uint8_t hp, int mute_first)
{
	struct alc_state *st = alc_st(c);
	int sense;

	if (!hp)
		return;
	sense = alc_jack_present_raw(c, hp);
	if (!sense)
		return;
	if (mute_first)
		alc_msleep(2);
	alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
	alc_msleep(75);
	if (!st->no_shutup_pins)
		alc_raw_pinctl(c, hp, PINCTL_OFF);
	alc_msleep(75);
}

static void alc_family_shutup(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint8_t hp;
	uint32_t v;

	if (st->ops & ALC_OP_XPS13) {
		hp = alc_hp_pin_or(c, 0);
		if (hp) {
			alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
			alc_msleep(20);
		}
	}
	switch (st->type) {
	case ALC_T_269VB:
		alc_coef_update(c, 0x04, 1 << 11, 0); /* output power off */
		if ((alc_coef0(c) & 0x00ff) == 0x018)
			alc_msleep(150);
		break;
	case ALC_T_282:
		hp = alc_hp_pin_or(c, 0);
		if (!hp)
			break;
		v = alc_coef_read(c, 0x78);
		alc_coef_write(c, 0x78, 0x9004);
		alc_hp_shutdown(c, hp, 1);
		alc_setup_eapd(c, 0);
		alc_shutup_pins(c);
		if (v != ALC_NORESP)
			alc_coef_write(c, 0x78, (uint16_t)v);
		return;
	case ALC_T_283:
		hp = alc_hp_pin_or(c, 0);
		if (!hp)
			break;
		alc_coef_write(c, 0x43, 0x9004);
		alc_coef_write(c, 0x06, 0x2100); /* depop during power down */
		alc_hp_shutdown(c, hp, 0);
		alc_coef_update(c, 0x46, 0, 3 << 12);
		alc_setup_eapd(c, 0);
		alc_shutup_pins(c);
		alc_coef_write(c, 0x43, 0x9614);
		return;
	case ALC_T_255:
	case ALC_T_256:
	case ALC_T_257:
		hp = alc_hp_pin_or(c, 0x21);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x04, 0x0007, 0x1);
		/* the 3k pull-down must be set before the pin is cleared or
		 * the codec stalls */
		if (st->en_3kpull_low)
			alc_coef_update(c, 0x46, 0, 3 << 12);
		alc_hp_shutdown(c, hp, 1);
		alc_setup_eapd(c, 0);
		alc_shutup_pins(c);
		if (st->ultra_low_power) {
			alc_msleep(50);
			alc_coef_update(c, 0x03, 1 << 1, 0);
			alc_coef_update(c, 0x08, 7 << 4, 7 << 4);
			alc_coef_update(c, 0x08, 3 << 2, 0);
			alc_coef_update(c, 0x3b, 1 << 15, 1 << 15);
			alc_coef_update(c, 0x0e, 7 << 6, 0);
			alc_msleep(30);
		}
		return;
	case ALC_T_215:
	case ALC_T_225:
	case ALC_T_245:
	case ALC_T_287: {
		int s1, s2;

		hp = alc_hp_pin_or(c, 0x21);
		s1 = hp ? alc_jack_present_raw(c, hp) : 0;
		s2 = alc_jack_present_raw(c, 0x16);
		if (s1 || s2) {
			alc_coef_update(c, 0x4a, 0, 3 << 10);
			alc_msleep(2);
			if (s1)
				alc_raw_amp(c, hp, AMP_OUT_MUTE_ALL);
			if (s2)
				alc_raw_amp(c, 0x16, AMP_OUT_MUTE_ALL);
			alc_msleep(75);
			if (s1 && !st->no_shutup_pins)
				alc_raw_pinctl(c, hp, PINCTL_OFF);
			if (s2 && !st->no_shutup_pins)
				alc_raw_pinctl(c, 0x16, PINCTL_OFF);
			alc_msleep(75);
			alc_coef_update(c, 0x4a, 3 << 10, 0);
		}
		alc_setup_eapd(c, 0);
		alc_shutup_pins(c);
		if (st->ultra_low_power) {
			alc_msleep(50);
			alc_coef_update(c, 0x08, 0x0f << 2, 0x0c << 2);
			alc_coef_update(c, 0x0e, 7 << 6, 0);
			alc_coef_update(c, 0x33, 1 << 11, 1 << 11);
			alc_coef_update(c, 0x4a, 3 << 4, 2 << 4);
			alc_msleep(30);
		}
		return;
	}
	case ALC_T_623:
		hp = alc_hp_pin_or(c, 0x21);
		alc_hp_shutdown(c, hp, 1);
		if (alc_jack_present_raw(c, 0x14))
			alc_hp_shutdown(c, 0x14, 1);
		alc_setup_eapd(c, 0);
		alc_shutup_pins(c);
		return;
	default:
		break;
	}
	if (st->family == ALC_FAM_269) {
		hp = alc_hp_pin_or(c, 0);
		alc_hp_shutdown(c, hp, 1);
	}
	/* generic: EAPD off and a short pause against the pop */
	alc_setup_eapd(c, 0);
	alc_msleep(200);
	alc_shutup_pins(c);
}


/* ==========================================================================
 * Combo (headset) jack
 *
 * Laptop audio jacks carry headphones and a microphone on one 3.5 mm plug.
 * Two incompatible wirings exist: CTIA (mic on the sleeve) and OMTP (mic on
 * the second ring).  The codec has to be told which one is inserted, and
 * the headphone amp, the mic bias and the "universal audio jack" switch
 * are programmed through COEFs whose layout differs between chip classes.
 *
 * States: unplugged -> (plug) -> determine the type -> CTIA or OMTP mode;
 * "headphone" mode when no headset mic exists on the board.
 * ========================================================================== */

enum {
	ALC_HSC_NONE = 0,
	ALC_HSC_255, /* ALC235, ALC255 */
	ALC_HSC_256, /* ALC230, ALC236, ALC256, HW8326 */
	ALC_HSC_274, /* ALC234, ALC274, ALC294 */
	ALC_HSC_233, /* ALC233, ALC283 */
	ALC_HSC_288, /* ALC286, ALC288 */
	ALC_HSC_298,
	ALC_HSC_292,
	ALC_HSC_293,
	ALC_HSC_668,
	ALC_HSC_225, /* ALC215, ALC225, ALC285, ALC289, ALC295, ALC299 */
	ALC_HSC_867, /* ALC891 */
	ALC_HSC_221, /* ALC221, ALC662: pin control only */
};

static int alc_hs_class(uint32_t vid)
{
	switch (vid) {
	case 0x10ec0235:
	case 0x10ec0255:
		return ALC_HSC_255;
	case 0x10ec0230:
	case 0x10ec0236:
	case 0x10ec0256:
	case 0x19e58326:
		return ALC_HSC_256;
	case 0x10ec0234:
	case 0x10ec0274:
	case 0x10ec0294:
		return ALC_HSC_274;
	case 0x10ec0233:
	case 0x10ec0283:
		return ALC_HSC_233;
	case 0x10ec0286:
	case 0x10ec0288:
		return ALC_HSC_288;
	case 0x10ec0298:
		return ALC_HSC_298;
	case 0x10ec0292:
		return ALC_HSC_292;
	case 0x10ec0293:
		return ALC_HSC_293;
	case 0x10ec0668:
		return ALC_HSC_668;
	case 0x10ec0215:
	case 0x10ec0225:
	case 0x10ec0285:
	case 0x10ec0289:
	case 0x10ec0295:
	case 0x10ec0299:
		return ALC_HSC_225;
	case 0x10ec0867:
		return ALC_HSC_867;
	case 0x10ec0221:
	case 0x10ec0662:
		return ALC_HSC_221;
	}
	return ALC_HSC_NONE;
}

/* the ALC225 class needs these before every mode change */
static const struct hda_coef_fix hs225_pre[] = {
	CU(0x4a, 1 << 8, 0),
	CXU(ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0),
	CU(0x63, 3 << 14, 3 << 14),
	CU(0x4a, 3 << 4, 2 << 4),
	CU(0x4a, 3 << 10, 3 << 10),
	CU(0x45, 0x3f << 10, 0x34 << 10),
	CU(0x4a, 3 << 10, 0),
	CEND
};

/* ---- unplugged ---- */
static const struct hda_coef_fix hs_unplug_255[] = {
	CW(0x1b, 0x0c0b), /* LDO and misc */
	CW(0x45, 0xd089), /* UAJ: manual mode */
	CXU(ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0), /* HP amp by verb */
	CW(0x06, 0x6104), /* MIC2 VREF gated with HP */
	CXW(ALC_COEFEX_HP_AMP, 0x03, 0x8aa6), /* direct drive HP amp */
	CEND
};
static const struct hda_coef_fix hs_unplug_256[] = {
	CW(0x1b, 0x0c4b),
	CW(0x45, 0xd089),
	CW(0x06, 0x6104),
	CXW(ALC_COEFEX_HP_AMP, 0x03, 0x09a3),
	CXU(ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0),
	CEND
};
static const struct hda_coef_fix hs_unplug_233[] = {
	CW(0x1b, 0x0c0b), CW(0x45, 0xc429), CU(0x35, 0x4000, 0),
	CW(0x06, 0x2104), CW(0x1a, 0x0001), CW(0x26, 0x0004),
	CW(0x32, 0x42a3), CEND
};
static const struct hda_coef_fix hs_unplug_288[] = {
	CU(0x4f, 0xfcc0, 0xc400), CU(0x50, 0x2000, 0x2000),
	CU(0x56, 0x0006, 0x0006), CU(0x66, 0x0008, 0),
	CU(0x67, 0x2000, 0), CEND
};
static const struct hda_coef_fix hs_unplug_298[] = {
	CU(0x19, 0x1300, 0x0300), CEND
};
static const struct hda_coef_fix hs_unplug_292[] = {
	CW(0x76, 0x000e), CW(0x6c, 0x2400), CW(0x18, 0x7308),
	CW(0x6b, 0xc429), CEND
};
static const struct hda_coef_fix hs_unplug_293[] = {
	CU(0x10, 7 << 8, 6 << 8), /* line1 JD to 0 */
	CXU(ALC_COEFEX_HP_AMP, 0x05, (1 << 15) | (1 << 13), 0), /* charge pump by verb */
	CXU(ALC_COEFEX_HP_AMP, 0x03, 1 << 10, 1 << 10), /* EN_OSW */
	CU(0x1a, 1 << 3, 1 << 3), /* combo JD gated by LINE1-VREFO */
	CW(0x45, 0xc429), /* TRS */
	CU(0x4a, 0x000f, 0x000e), /* combo jack auto detect */
	CEND
};
static const struct hda_coef_fix hs_unplug_668[] = {
	CW(0x15, 0x0d40), CW(0xb7, 0x802b), CEND
};
static const struct hda_coef_fix hs_unplug_225[] = {
	CU(0x63, 3 << 14, 0), CEND
};
static const struct hda_coef_fix hs_unplug_274[] = {
	CU(0x4a, 0x0100, 0), CXU(ALC_COEFEX_HP_AMP, 0x05, 0x4000, 0),
	CU(0x6b, 0xf000, 0x5000), CU(0x4a, 0x0010, 0),
	CU(0x4a, 0x0c00, 0x0c00), CW(0x45, 0x5289),
	CU(0x4a, 0x0c00, 0), CEND
};

/* ---- headphone only (TRS, no mic) ---- */
static const struct hda_coef_fix hs_hp_225[] = {
	CU(0x45, 0x3f << 10, 0x30 << 10), CU(0x45, 0x3f << 10, 0x31 << 10),
	CU(0x49, 3 << 8, 0 << 8), CU(0x4a, 3 << 4, 3 << 4),
	CU(0x63, 3 << 14, 0), CU(0x67, 0xf000, 0x3000), CEND
};
static const struct hda_coef_fix hs_hp_255[] = {
	CW(0x45, 0xc089), CW(0x45, 0xc489),
	CXW(ALC_COEFEX_HP_AMP, 0x03, 0x8ea6), CW(0x49, 0x0049), CEND
};
static const struct hda_coef_fix hs_hp_256[] = {
	CW(0x45, 0xc489), CXW(ALC_COEFEX_HP_AMP, 0x03, 0x0da3),
	CW(0x49, 0x0049), CXU(ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0),
	CW(0x06, 0x6100), CEND
};
static const struct hda_coef_fix hs_hp_233[] = {
	CW(0x06, 0x2100), CW(0x32, 0x4ea3), CEND
};
static const struct hda_coef_fix hs_hp_288[] = {
	CU(0x4f, 0xfcc0, 0xc400), CU(0x50, 0x2000, 0x2000),
	CU(0x56, 0x0006, 0x0006), CU(0x66, 0x0008, 0),
	CU(0x67, 0x2000, 0), CEND
};
static const struct hda_coef_fix hs_hp_292[] = {
	CW(0x76, 0x000e), CW(0x6c, 0x2400), CW(0x6b, 0xc429),
	CW(0x18, 0x7308), CEND
};
static const struct hda_coef_fix hs_hp_293[] = {
	CU(0x4a, 0x000f, 0x000e), CW(0x45, 0xc429), CU(0x1a, 1 << 3, 0),
	CEND
};
static const struct hda_coef_fix hs_hp_668[] = {
	CW(0x11, 0x0041), CW(0x15, 0x0d40), CW(0xb7, 0x802b), CEND
};
static const struct hda_coef_fix hs_hp_274[] = {
	CW(0x45, 0x4289), CU(0x4a, 0x0010, 0x0010), CU(0x6b, 0x0f00, 0),
	CU(0x49, 0x0300, 0x0300), CEND
};

/* ---- CTIA ---- */
static const struct hda_coef_fix hs_ctia_255[] = {
	CW(0x45, 0xd489), CW(0x1b, 0x0c2b),
	CXW(ALC_COEFEX_HP_AMP, 0x03, 0x8ea6), CEND
};
static const struct hda_coef_fix hs_ctia_256[] = {
	CW(0x45, 0xd489), CW(0x1b, 0x0e6b), CEND
};
static const struct hda_coef_fix hs_ctia_233[] = {
	CW(0x45, 0xd429), CW(0x1b, 0x0c2b), CW(0x32, 0x4ea3), CEND
};
static const struct hda_coef_fix hs_mode_288[] = {
	CU(0x50, 0x2000, 0x2000), CU(0x56, 0x0006, 0x0006),
	CU(0x66, 0x0008, 0), CU(0x67, 0x2000, 0), CEND
};
static const struct hda_coef_fix hs_ctia_292[] = {
	CW(0x6b, 0xd429), CW(0x76, 0x0008), CW(0x18, 0x7388), CEND
};
static const struct hda_coef_fix hs_ctia_293[] = {
	CW(0x45, 0xd429), CU(0x10, 7 << 8, 7 << 8), CEND
};
static const struct hda_coef_fix hs_ctia_668[] = {
	CW(0x11, 0x0001), CW(0x15, 0x0d60), CW(0xc3, 0x0000), CEND
};
static const struct hda_coef_fix hs_ctia_225_a[] = {
	CU(0x45, 0x3f << 10, 0x35 << 10), CU(0x63, 3 << 14, 2 << 14), CEND
};
static const struct hda_coef_fix hs_ctia_225_b[] = {
	CU(0x45, 0x3f << 10, 0x35 << 10), CU(0x63, 3 << 14, 1 << 14), CEND
};

/* ---- OMTP ---- */
static const struct hda_coef_fix hs_omtp_255[] = {
	CW(0x45, 0xe489), CW(0x1b, 0x0c2b),
	CXW(ALC_COEFEX_HP_AMP, 0x03, 0x8ea6), CEND
};
static const struct hda_coef_fix hs_omtp_256[] = {
	CW(0x45, 0xe489), CW(0x1b, 0x0e6b), CEND
};
static const struct hda_coef_fix hs_omtp_233[] = {
	CW(0x45, 0xe429), CW(0x1b, 0x0c2b), CW(0x32, 0x4ea3), CEND
};
static const struct hda_coef_fix hs_omtp_292[] = {
	CW(0x6b, 0xe429), CW(0x76, 0x0008), CW(0x18, 0x7388), CEND
};
static const struct hda_coef_fix hs_omtp_293[] = {
	CW(0x45, 0xe429), CU(0x10, 7 << 8, 7 << 8), CEND
};
static const struct hda_coef_fix hs_omtp_668[] = {
	CW(0x11, 0x0001), CW(0x15, 0x0d50), CW(0xc3, 0x0000), CEND
};
static const struct hda_coef_fix hs_omtp_225[] = {
	CU(0x45, 0x3f << 10, 0x39 << 10), CU(0x63, 3 << 14, 2 << 14), CEND
};

/* ---- type detection ---- */
static const struct hda_coef_fix hs_det_255[] = {
	CW(0x45, 0xd089), /* combo jack auto switch: check type */
	CW(0x49, 0x0149), /* combo jack auto switch: VREF */
	CEND
};
static const struct hda_coef_fix hs_det_288[] = {
	CU(0x4f, 0xfcc0, 0xd400), CEND
};
static const struct hda_coef_fix hs_det_298_post[] = {
	CU(0x50, 0x2000, 0x2000), CU(0x56, 0x0006, 0x0006),
	CU(0x66, 0x0008, 0), CU(0x67, 0x2000, 0),
	CU(0x19, 0x1300, 0x1300), CEND
};
static const struct hda_coef_fix hs_det_293[] = {
	CU(0x4a, 0x000f, 0x0008), CW(0x45, 0xd429), CEND
};
static const struct hda_coef_fix hs_det_668[] = {
	CW(0x11, 0x0001), CW(0xb7, 0x802b), CW(0x15, 0x0d60),
	CW(0xc3, 0x0c00), CEND
};
static const struct hda_coef_fix hs_det_274[] = {
	CU(0x4a, 0x0010, 0), CU(0x4a, 0x8000, 0), CW(0x45, 0xd289),
	CU(0x49, 0x0300, 0x0300), CEND
};

/* The headphone pin 0x21 switched off around a mode change, then back on;
 * the direct-drive amp clicks otherwise. */
static void alc_hs_hp_off(struct hda_codec *c, uint32_t delay)
{
	if (!alc_pin(c, 0x21))
		return;
	alc_raw_amp(c, 0x21, AMP_OUT_MUTE_ALL);
	alc_msleep(delay);
	alc_raw_pinctl(c, 0x21, PINCTL_OFF);
	alc_msleep(delay);
}

static void alc_hs_hp_on(struct hda_codec *c, uint32_t delay)
{
	if (!alc_pin(c, 0x21))
		return;
	alc_raw_pinctl(c, 0x21, PINCTL_OUT);
	alc_msleep(delay);
	alc_raw_amp(c, 0x21, AMP_OUT_UNMUTE_ALL);
	alc_msleep(delay);
}

static void alc_hs_unplugged(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (st->no_internal_mic_pin) {
		alc_coef_update(c, 0x45, (0xf << 12) | (1 << 10), 5 << 12);
		return;
	}
	switch (alc_hs_class(st->vendor_id)) {
	case ALC_HSC_255:
		alc_coef_seq(c, hs_unplug_255);
		break;
	case ALC_HSC_256:
		alc_hs_hp_off(c, 75);
		alc_coef_seq(c, hs_unplug_256);
		break;
	case ALC_HSC_274:
		alc_coef_seq(c, hs_unplug_274);
		break;
	case ALC_HSC_233:
		alc_coef_seq(c, hs_unplug_233);
		break;
	case ALC_HSC_288:
		alc_coef_seq(c, hs_unplug_288);
		break;
	case ALC_HSC_298:
		alc_coef_seq(c, hs_unplug_298);
		alc_coef_seq(c, hs_unplug_288);
		break;
	case ALC_HSC_292:
		alc_coef_seq(c, hs_unplug_292);
		break;
	case ALC_HSC_293:
		alc_coef_seq(c, hs_unplug_293);
		break;
	case ALC_HSC_668:
		alc_coef_seq(c, hs_unplug_668);
		break;
	case ALC_HSC_225:
		alc_hs_hp_off(c, 75);
		alc_coef_seq(c, hs225_pre);
		alc_coef_seq(c, hs_unplug_225);
		break;
	case ALC_HSC_867:
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0);
		break;
	}
}

static void alc_hs_headphone(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	switch (alc_hs_class(st->vendor_id)) {
	case ALC_HSC_225:
		alc_coef_seq(c, hs225_pre);
		alc_coef_seq(c, hs_hp_225);
		alc_hs_hp_on(c, 75);
		break;
	case ALC_HSC_255:
		alc_coef_seq(c, hs_hp_255);
		break;
	case ALC_HSC_256:
		alc_coef_write(c, 0x1b, 0x0e4b);
		alc_coef_write(c, 0x45, 0xc089);
		alc_msleep(50);
		alc_coef_seq(c, hs_hp_256);
		alc_hs_hp_on(c, 75);
		break;
	case ALC_HSC_274:
		alc_coef_seq(c, hs_hp_274);
		break;
	case ALC_HSC_233:
		alc_coef_seq(c, hs_hp_233);
		break;
	case ALC_HSC_288:
	case ALC_HSC_298:
		alc_coef_seq(c, hs_hp_288);
		break;
	case ALC_HSC_292:
		alc_coef_seq(c, hs_hp_292);
		break;
	case ALC_HSC_293:
		alc_coef_seq(c, hs_hp_293);
		break;
	case ALC_HSC_668:
		alc_coef_seq(c, hs_hp_668);
		break;
	case ALC_HSC_867:
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0);
		break;
	}
}

static void alc_hs_ctia(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint32_t v;

	switch (alc_hs_class(st->vendor_id)) {
	case ALC_HSC_255:
		alc_coef_seq(c, hs_ctia_255);
		break;
	case ALC_HSC_256:
		alc_coef_seq(c, hs_ctia_256);
		alc_hs_hp_on(c, 75);
		break;
	case ALC_HSC_274:
		alc_coef_write(c, 0x45, 0xd689);
		break;
	case ALC_HSC_233:
		alc_coef_seq(c, hs_ctia_233);
		break;
	case ALC_HSC_298:
		/* bit 12 of COEF 0x50 tells which half of the headset
		 * output stage is wired on this board */
		v = alc_coef_read(c, 0x50);
		if (v != ALC_NORESP && (v & (1 << 12)))
			alc_coef_update(c, 0x8e, 0x0070, 0x0020);
		else
			alc_coef_update(c, 0x8e, 0x0070, 0x0010);
		alc_coef_update(c, 0x4f, 0xfcc0, 0xd400);
		alc_msleep(300);
		break;
	case ALC_HSC_288:
		alc_coef_update(c, 0x4f, 0xfcc0, 0xd400);
		alc_msleep(300);
		alc_coef_seq(c, hs_mode_288);
		break;
	case ALC_HSC_292:
		alc_coef_seq(c, hs_ctia_292);
		break;
	case ALC_HSC_293:
		alc_coef_seq(c, hs_ctia_293);
		break;
	case ALC_HSC_668:
		alc_coef_seq(c, hs_ctia_668);
		break;
	case ALC_HSC_225:
		v = alc_coef_read(c, 0x45);
		if (v != ALC_NORESP && (v & (1 << 9)))
			alc_coef_seq(c, hs_ctia_225_b);
		else
			alc_coef_seq(c, hs_ctia_225_a);
		alc_hs_hp_on(c, 75);
		break;
	case ALC_HSC_867:
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0);
		break;
	}
}

static void alc_hs_omtp(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	switch (alc_hs_class(st->vendor_id)) {
	case ALC_HSC_255:
		alc_coef_seq(c, hs_omtp_255);
		break;
	case ALC_HSC_256:
		alc_coef_seq(c, hs_omtp_256);
		alc_hs_hp_on(c, 75);
		break;
	case ALC_HSC_274:
		alc_coef_write(c, 0x45, 0xe689);
		break;
	case ALC_HSC_233:
		alc_coef_seq(c, hs_omtp_233);
		break;
	case ALC_HSC_298:
		alc_coef_update(c, 0x8e, 0x0070, 0x0010); /* headset output on */
		alc_coef_update(c, 0x4f, 0xfcc0, 0xe400);
		alc_msleep(300);
		break;
	case ALC_HSC_288:
		alc_coef_update(c, 0x4f, 0xfcc0, 0xe400);
		alc_msleep(300);
		alc_coef_seq(c, hs_mode_288);
		break;
	case ALC_HSC_292:
		alc_coef_seq(c, hs_omtp_292);
		break;
	case ALC_HSC_293:
		alc_coef_seq(c, hs_omtp_293);
		break;
	case ALC_HSC_668:
		alc_coef_seq(c, hs_omtp_668);
		break;
	case ALC_HSC_225:
		alc_coef_seq(c, hs_omtp_225);
		alc_hs_hp_on(c, 75);
		break;
	}
}

/* Let the codec measure the plug: the impedance between sleeve and ring 2
 * shows where the microphone is. */
static void alc_hs_detect_type(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	uint32_t v;
	int ctia = 0;

	if (st->no_internal_mic_pin) {
		alc_coef_update(c, 0x45, (0xf << 12) | (1 << 10), 5 << 12);
		return;
	}
	switch (alc_hs_class(st->vendor_id)) {
	case ALC_HSC_255:
		alc_coef_seq(c, hs_det_255);
		alc_msleep(300);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		break;
	case ALC_HSC_256:
		alc_coef_write(c, 0x1b, 0x0e4b);
		alc_coef_write(c, 0x06, 0x6104);
		alc_coefex_write(c, ALC_COEFEX_HP_AMP, 0x03, 0x09a3);
		alc_coef_seq(c, hs_det_255);
		alc_msleep(300);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		if (!ctia) {
			/* second opinion with the OMTP switch closed */
			alc_coef_write(c, 0x45, 0xe089);
			alc_msleep(100);
			v = alc_coef_read(c, 0x46);
			ctia = !(v != ALC_NORESP && (v & 0x0070) == 0x0070);
		}
		alc_coefex_write(c, ALC_COEFEX_HP_AMP, 0x03, 0x0da3);
		alc_coefex_update(c, ALC_COEFEX_HP_AMP, 0x05, 1 << 14, 0);
		break;
	case ALC_HSC_274:
		alc_coef_seq(c, hs_det_274);
		alc_msleep(850);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x00f0) == 0x00f0;
		break;
	case ALC_HSC_233:
		alc_coef_write(c, 0x45, 0xd029);
		alc_msleep(300);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		break;
	case ALC_HSC_298:
		if (alc_pin(c, 0x21)) {
			alc_raw_amp(c, 0x21, AMP_OUT_MUTE_ALL);
			alc_msleep(100);
			alc_raw_pinctl(c, 0x21, PINCTL_OFF);
			alc_msleep(200);
		}
		v = alc_coef_read(c, 0x50);
		if (v != ALC_NORESP && (v & (1 << 12)))
			alc_coef_update(c, 0x8e, 0x0070, 0x0020);
		else
			alc_coef_update(c, 0x8e, 0x0070, 0x0010);
		alc_coef_seq(c, hs_det_288);
		alc_msleep(350);
		v = alc_coef_read(c, 0x50);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		alc_coef_seq(c, hs_det_298_post);
		if (alc_pin(c, 0x21)) {
			alc_raw_pinctl(c, 0x21, PINCTL_HP);
			alc_msleep(75);
			alc_raw_amp(c, 0x21, AMP_OUT_UNMUTE_ALL);
		}
		break;
	case ALC_HSC_288:
		alc_coef_seq(c, hs_det_288);
		alc_msleep(350);
		v = alc_coef_read(c, 0x50);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		break;
	case ALC_HSC_292:
		alc_coef_write(c, 0x6b, 0xd429);
		alc_msleep(300);
		v = alc_coef_read(c, 0x6c);
		ctia = v != ALC_NORESP && (v & 0x001c) == 0x001c;
		break;
	case ALC_HSC_293:
		alc_coef_seq(c, hs_det_293);
		alc_msleep(300);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x0070) == 0x0070;
		break;
	case ALC_HSC_668:
		alc_coef_seq(c, hs_det_668);
		alc_msleep(300);
		v = alc_coef_read(c, 0xbe);
		ctia = v != ALC_NORESP && (v & 0x1c02) == 0x1c02;
		break;
	case ALC_HSC_225:
		alc_coef_seq(c, hs225_pre);
		alc_coef_update(c, 0x67, 0xf000, 0x1000);
		v = alc_coef_read(c, 0x45);
		alc_coef_update(c, 0x45, 0x3f << 10, 0x34 << 10);
		if (v != ALC_NORESP && (v & (1 << 9)))
			alc_coef_update(c, 0x49, 3 << 8, 2 << 8);
		else
			alc_coef_update(c, 0x49, 3 << 8, 1 << 8);
		alc_msleep(800);
		v = alc_coef_read(c, 0x46);
		ctia = v != ALC_NORESP && (v & 0x00f0) == 0x00f0;
		if (!ctia) {
			alc_coef_update(c, 0x45, 0x3f << 10, 0x38 << 10);
			alc_coef_update(c, 0x49, 3 << 8, 1 << 8);
			alc_msleep(100);
			v = alc_coef_read(c, 0x46);
			ctia = !(v != ALC_NORESP && (v & 0x00f0) == 0x00f0);
		}
		alc_coef_update(c, 0x4a, 7 << 6, 7 << 6);
		alc_coef_update(c, 0x4a, 3 << 4, 3 << 4);
		alc_coef_update(c, 0x67, 0xf000, 0x3000);
		break;
	case ALC_HSC_867:
		ctia = 1;
		break;
	}
	st->hs_type = ctia ? ALC_HS_TYPE_CTIA : ALC_HS_TYPE_OMTP;
	snd_info("hda", "%s: headset jack: %s plug\n", c->chip_name,
		ctia ? "CTIA" : "OMTP");
}

/* ALC255/256 boards that expect a CTIA plug at reset */
static void alc255_default_jack_ctia(struct hda_codec *c)
{
	static const struct hda_coef_fix seq255[] = {
		CW(0x1b, 0x880b), CW(0x45, 0xd089), CW(0x1b, 0x080b),
		CW(0x46, 0x0004), CW(0x1b, 0x0c0b), CEND
	};
	static const struct hda_coef_fix seq256[] = {
		CW(0x1b, 0x884b), CW(0x45, 0xd089), CW(0x1b, 0x084b),
		CW(0x46, 0x0004), CW(0x1b, 0x0c4b), CEND
	};
	struct alc_state *st = alc_st(c);

	switch (st->vendor_id) {
	case 0x10ec0255:
		alc_coef_seq(c, seq255);
		break;
	case 0x10ec0230:
	case 0x10ec0236:
	case 0x10ec0256:
	case 0x19e58326:
		alc_coef_seq(c, seq256);
		break;
	default:
		return;
	}
	alc_msleep(30);
}

/* Restart the combo-jack detection state machine (after it latched a
 * wrong state during boot). */
static void alc_combo_jd_restart(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	switch (st->vendor_id) {
	case 0x10ec0274:
	case 0x10ec0294:
	case 0x10ec0225:
	case 0x10ec0295:
	case 0x10ec0299:
		alc_coef_update(c, 0x4a, 0x8000, 1 << 15);
		alc_coef_update(c, 0x4a, 0x8000, 0);
		break;
	case 0x10ec0230:
	case 0x10ec0235:
	case 0x10ec0236:
	case 0x10ec0255:
	case 0x10ec0256:
	case 0x10ec0257:
	case 0x19e58326:
		alc_coef_update(c, 0x1b, 0x8000, 1 << 15);
		alc_coef_update(c, 0x1b, 0x8000, 0);
		break;
	}
}

/* The headset mode for the current jack state.  Called at init and from
 * every headphone jack change. */
static void alc_hs_update(struct hda_codec *c, int hp_present)
{
	struct alc_state *st = alc_st(c);
	struct hda_widget *w;
	uint8_t saved, saved21;
	int mode;

	if (!st->hs_capable || !st->hp_pin)
		return;
	/* With a headset microphone on the board the plug type is measured
	 * and the matching mode set; a 3-pole headphone plug works in either
	 * mode (ring 2 and sleeve are both ground on it).  Boards without a
	 * headset mic only get the headphone (TRS) setup. */
	if (!hp_present)
		mode = ALC_HS_MODE_UNPLUGGED;
	else if (st->hs_mic_pin)
		mode = ALC_HS_MODE_HEADSET;
	else
		mode = ALC_HS_MODE_HEADPHONE;

	if (mode == st->hs_mode)
		return;

	/* the mode sequences switch the headphone pin off and on by hand;
	 * whether it ends up enabled is the router's decision */
	w = alc_pin(c, st->hp_pin);
	saved = w ? w->pinctl : PINCTL_HP;
	w = alc_pin(c, 0x21);
	saved21 = w ? w->pinctl : 0;

	switch (mode) {
	case ALC_HS_MODE_UNPLUGGED:
		alc_hs_unplugged(c);
		st->hs_type = ALC_HS_TYPE_UNKNOWN;
		break;
	case ALC_HS_MODE_HEADSET:
		if (st->hs_type == ALC_HS_TYPE_UNKNOWN)
			alc_hs_detect_type(c);
		if (st->hs_type == ALC_HS_TYPE_CTIA)
			alc_hs_ctia(c);
		else if (st->hs_type == ALC_HS_TYPE_OMTP)
			alc_hs_omtp(c);
		break;
	case ALC_HS_MODE_HEADPHONE:
		alc_hs_headphone(c);
		break;
	}
	alc_set_pinctl(c, st->hp_pin, saved);
	if (st->hp_pin != 0x21 && alc_pin(c, 0x21))
		alc_set_pinctl(c, 0x21, saved21);
	if (st->hp_mic_pin && st->hp_mic_pin != st->hp_pin)
		alc_set_pinctl(c, st->hp_mic_pin, PINCTL_VREFHIZ);
	/* "unplugged" is re-entered on the next plug event from scratch */
	st->hs_mode = (mode == ALC_HS_MODE_UNPLUGGED) ? ALC_HS_MODE_UNKNOWN :
							 mode;
}

/* ==========================================================================
 * Indicator LEDs
 * ========================================================================== */

static void alc_led_set(struct hda_codec *c, const struct alc_led *l, int on)
{
	struct hda_widget *w;
	uint8_t ctl;

	if (!l || l->kind == ALC_LEDK_NONE)
		return;
	if (l->inverted)
		on = !on;
	switch (l->kind) {
	case ALC_LEDK_GPIO:
		alc_gpio_update(c, l->gpio, on);
		break;
	case ALC_LEDK_VREF:
		/* the LED hangs on an unused mic pin's bias output */
		w = alc_pin(c, l->nid);
		if (!w)
			break;
		ctl = (uint8_t)(w->pinctl & ~AC_PINCTL_VREFEN);
		ctl |= on ? AC_PINCTL_VREF_80 : AC_PINCTL_VREF_HIZ;
		hda_pin_set_ctl(c, l->nid, ctl);
		break;
	case ALC_LEDK_VREF100:
		if (alc_pin(c, l->nid))
			hda_pin_set_ctl(c, l->nid,
					on ? PINCTL_VREF100 : PINCTL_IN);
		break;
	case ALC_LEDK_COEF:
		alc_coef_update(c, l->idx, l->mask, on ? l->on : l->off);
		break;
	}
}

/* The mute LED shows the master mute; force = write even if unchanged
 * (after a reset). */
static void alc_sync_leds(struct hda_codec *c, int force)
{
	struct alc_state *st = alc_st(c);
	int muted;

	if (!st)
		return;
	muted = c->spec.master_mute ? 1 : 0;
	if (st->mute_led && (force || muted != st->mute_led_state)) {
		alc_led_set(c, st->mute_led, muted);
		st->mute_led_state = muted;
	}
	if (force && st->micmute_led)
		alc_led_set(c, st->micmute_led, 0);
}

/* ==========================================================================
 * Speaker amplifiers reached through the COEF bridge (Samsung Galaxy Book)
 * ========================================================================== */

static void alc298_amp_reg(struct hda_codec *c, uint16_t reg, uint16_t val)
{
	alc_coef_write(c, 0x23, reg);
	alc_coef_write(c, 0x25, val);
	alc_coef_write(c, 0x26, 0xb011); /* commit */
}

struct alc298_amp_seq {
	uint16_t reg, val;
};

static void alc298_samsung_amp_v1(struct hda_codec *c)
{
	static const struct alc298_amp_seq common[] = {
		{ 0x19, 0x00 }, { 0x20, 0xc0 }, { 0x22, 0x44 }, { 0x23, 0x08 },
		{ 0x24, 0x85 }, { 0x25, 0x41 }, { 0x35, 0x40 }, { 0x36, 0x01 },
		{ 0x38, 0x81 }, { 0x3a, 0x03 }, { 0x3b, 0x81 }, { 0x40, 0x3e },
		{ 0x41, 0x07 }, { 0x400, 0x1 },
	};
	static const struct {
		uint8_t addr;
		struct alc298_amp_seq seq[2];
	} amps[] = {
		{ 0x3a, { { 0x18, 0x1 }, { 0x26, 0x0 } } },
		{ 0x39, { { 0x18, 0x2 }, { 0x26, 0x1 } } },
	};
	unsigned int i, j;

	for (i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		alc_coef_write(c, 0x22, amps[i].addr);
		for (j = 0; j < 2; j++)
			alc298_amp_reg(c, amps[i].seq[j].reg, amps[i].seq[j].val);
		for (j = 0; j < sizeof(common) / sizeof(common[0]); j++)
			alc298_amp_reg(c, common[j].reg, common[j].val);
	}
}

struct alc298_amp_v2 {
	uint8_t addr;
	uint8_t n;
	struct alc298_amp_seq seq[18];
};

static const struct alc298_amp_v2 alc298_amps_v2[4] = {
	{ 0x38, 18, {
		{ 0x23e1, 0x0000 }, { 0x2012, 0x006f }, { 0x2014, 0x0000 },
		{ 0x201b, 0x0001 }, { 0x201d, 0x0001 }, { 0x201f, 0x00fe },
		{ 0x2021, 0x0000 }, { 0x2022, 0x0010 }, { 0x203d, 0x0005 },
		{ 0x203f, 0x0003 }, { 0x2050, 0x002c }, { 0x2076, 0x000e },
		{ 0x207c, 0x004a }, { 0x2081, 0x0003 }, { 0x2399, 0x0003 },
		{ 0x23a4, 0x00b5 }, { 0x23a5, 0x0001 }, { 0x23ba, 0x0094 } } },
	{ 0x39, 18, {
		{ 0x23e1, 0x0000 }, { 0x2012, 0x006f }, { 0x2014, 0x0000 },
		{ 0x201b, 0x0002 }, { 0x201d, 0x0002 }, { 0x201f, 0x00fd },
		{ 0x2021, 0x0001 }, { 0x2022, 0x0010 }, { 0x203d, 0x0005 },
		{ 0x203f, 0x0003 }, { 0x2050, 0x002c }, { 0x2076, 0x000e },
		{ 0x207c, 0x004a }, { 0x2081, 0x0003 }, { 0x2399, 0x0003 },
		{ 0x23a4, 0x00b5 }, { 0x23a5, 0x0001 }, { 0x23ba, 0x0094 } } },
	{ 0x3c, 15, {
		{ 0x23e1, 0x0000 }, { 0x2012, 0x006f }, { 0x2014, 0x0000 },
		{ 0x201b, 0x0001 }, { 0x201d, 0x0001 }, { 0x201f, 0x00fe },
		{ 0x2021, 0x0000 }, { 0x2022, 0x0010 }, { 0x203d, 0x0005 },
		{ 0x203f, 0x0003 }, { 0x2050, 0x002c }, { 0x2076, 0x000e },
		{ 0x207c, 0x004a }, { 0x2081, 0x0003 }, { 0x23ba, 0x008d } } },
	{ 0x3d, 15, {
		{ 0x23e1, 0x0000 }, { 0x2012, 0x006f }, { 0x2014, 0x0000 },
		{ 0x201b, 0x0002 }, { 0x201d, 0x0002 }, { 0x201f, 0x00fd },
		{ 0x2021, 0x0001 }, { 0x2022, 0x0010 }, { 0x203d, 0x0005 },
		{ 0x203f, 0x0003 }, { 0x2050, 0x002c }, { 0x2076, 0x000e },
		{ 0x207c, 0x004a }, { 0x2081, 0x0003 }, { 0x23ba, 0x008d } } },
};

static void alc298_samsung_v2_power(struct hda_codec *c, int on)
{
	struct alc_state *st = alc_st(c);
	int i;

	for (i = 0; i < st->samsung_amps && i < 4; i++) {
		alc_coef_write(c, 0x22, alc298_amps_v2[i].addr);
		if (on) {
			alc298_amp_reg(c, 0x203a, 0x0081);
			alc298_amp_reg(c, 0x23ff, 0x0001);
		} else {
			alc298_amp_reg(c, 0x23ff, 0x0000);
			alc298_amp_reg(c, 0x203a, 0x0080);
		}
	}
	st->samsung_amps_on = on;
}

static void alc298_samsung_v2_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	int i, j;

	/* amplifiers off before reprogramming them */
	alc298_samsung_v2_power(c, 0);
	for (i = 0; i < st->samsung_amps && i < 4; i++) {
		const struct alc298_amp_v2 *a = &alc298_amps_v2[i];

		alc_coef_write(c, 0x22, a->addr);
		for (j = 0; j < a->n && j < 18; j++)
			alc298_amp_reg(c, a->seq[j].reg, a->seq[j].val);
		alc_coef_write(c, 0x89, 0x0);
	}
	if (st->playing)
		alc298_samsung_v2_power(c, 1);
}

/* ==========================================================================
 * Board COEF sets referenced by recipe flags
 * ========================================================================== */

static const struct hda_coef_fix alc_gb_x570_coefs[] = {
	CW(0x07, 0x03c0), CW(0x1a, 0x01c1), CW(0x1b, 0x0202),
	CW(0x43, 0x3005), CEND
};

/* HP ENVY x360 (ALC285): headphone amp, jack sense and speaker trims */
static const struct hda_coef_fix alc_envy_x360_coefs[] = {
	CW(0x08, 0x6a0c), CW(0x0d, 0xa023), CW(0x10, 0x0320),
	CW(0x1a, 0x8c03), CW(0x25, 0x1800), CW(0x26, 0x003a),
	CW(0x28, 0x1dfe), CW(0x29, 0xb014), CW(0x2b, 0x1dfe),
	CW(0x37, 0xfe15), CW(0x38, 0x7909), CW(0x45, 0xd489),
	CW(0x46, 0x00f4), CW(0x4a, 0x21e0), CW(0x66, 0x03f0),
	CW(0x67, 0x1000), CW(0x6e, 0x1005), CEND
};

/* Barebones whose previous OS left the jack detection reversed: restore
 * the reset defaults. */
static const struct hda_coef_fix alc256_coef_defaults[] = {
	CW(0x10, 0x0020), CW(0x24, 0x0000), CW(0x26, 0x0000),
	CW(0x29, 0x3000), CW(0x37, 0xfe05), CW(0x45, 0x5089), CEND
};

/* ASRock NUC Box: jack detection only works with these values */
static const struct hda_coef_fix alc233_jack_coefs[] = {
	CW(0x1a, 0x9003), CW(0x1b, 0x0e2b), CW(0x37, 0xfe06),
	CW(0x38, 0x4981), CW(0x45, 0xd489), CW(0x46, 0x0074),
	CW(0x49, 0x0149), CEND
};


/* ==========================================================================
 * Board fixup selection
 * ========================================================================== */

static const struct hda_fixup *alc_family_quirks(int family)
{
	switch (family) {
	case ALC_FAM_269:
		return alc269_quirks;
	case ALC_FAM_662:
		return alc662_quirks;
	case ALC_FAM_882:
		return alc882_quirks;
	}
	return NULL;
}

static const struct alc_pin_quirk *alc_family_pin_quirks(int family,
							 int fallback)
{
	switch (family) {
	case ALC_FAM_269:
		return fallback ? alc269_fallback_pin_quirks :
				  alc269_pin_quirks;
	case ALC_FAM_662:
		return fallback ? NULL : alc662_pin_quirks;
	case ALC_FAM_882:
		return fallback ? NULL : alc882_pin_quirks;
	}
	return NULL;
}

static int alc_ssid_usable(uint32_t ssid)
{
	return (ssid >> 16) != 0 && (ssid & 0xffff) != 0 &&
	       ssid != 0xffffffffu;
}

static inline int alc_quirk_hit(const struct hda_fixup *q, uint32_t ssid)
{
	return ((ssid ^ q->subsys) & q->subsys_mask) == 0;
}

/* The subsystem id the board vendor put on the HDA controller identifies
 * the machine; the codec carries its own copy (usually the same), which is
 * used when an entry asks for it or the controller has none. */
static const struct hda_fixup *alc_lookup_quirk(struct alc_state *st,
						const struct hda_fixup *tbl,
						const char **kind)
{
	const struct hda_fixup *q;
	int pci_ok = alc_ssid_usable(st->pci_ssid);
	int codec_ok = alc_ssid_usable(st->codec_ssid);

	if (!tbl)
		return NULL;
	for (q = tbl; q->subsys; q++) {
		if (q->codec_id && q->codec_id != st->vendor_id)
			continue;
		if (!pci_ok || (q->flags & ALC_FIX_CODEC_SSID)) {
			if (codec_ok && alc_quirk_hit(q, st->codec_ssid)) {
				*kind = "codec SSID";
				return q;
			}
		} else if (alc_quirk_hit(q, st->pci_ssid)) {
			*kind = "PCI SSID";
			return q;
		}
	}
	if (!codec_ok)
		return NULL;
	for (q = tbl; q->subsys; q++) {
		if (q->codec_id && q->codec_id != st->vendor_id)
			continue;
		if (alc_quirk_hit(q, st->codec_ssid)) {
			*kind = "codec SSID";
			return q;
		}
	}
	return NULL;
}

/* Matching on the shape of the BIOS pin configuration.  Association and
 * sequence are ignored; unconnected pins (port connectivity "none") match
 * each other whatever their remaining bits.  In strict mode every
 * connected pin of the codec must be listed. */
static int alc_pin_quirk_hit(struct hda_codec *c, struct alc_state *st,
			     const struct alc_pin_quirk *pq)
{
	int i;

	if (pq->codec_id != st->vendor_id || pq->subvendor != st->pci_ssvid)
		return 0;
	for (i = 0; i < c->num_nodes; i++) {
		const struct hda_widget *w = &c->w[i];
		const struct hda_pincfg *t;
		uint32_t cfg;
		int found = 0;

		if (w->type != AC_WID_PIN)
			continue;
		cfg = w->pin_cfg_orig;
		for (t = pq->pins; t && t->nid; t++) {
			if (t->nid != w->nid)
				continue;
			found = 1;
			if ((t->cfg & ~0xffu) == (cfg & ~0xffu))
				break;
			if ((cfg & 0xf0000000u) == 0x40000000u &&
			    (t->cfg & 0xf0000000u) == 0x40000000u)
				break;
			return 0;
		}
		if (pq->strict && !found && (cfg & 0xf0000000u) != 0x40000000u)
			return 0;
	}
	return 1;
}

static void alc_pick_recipe(struct hda_codec *c, struct alc_state *st)
{
	const struct hda_fixup *q;
	const struct alc_pin_quirk *pq;
	const char *kind = NULL;
	uint32_t r;
	int pass;

	st->rcp = NULL;
	q = alc_lookup_quirk(st, alc_family_quirks(st->family), &kind);
	if (q) {
		r = q->flags >> ALC_FIX_RECIPE_SHIFT;
		if (r > 0 && r < R_COUNT) {
			st->rcp = &alc_recipes[r];
			st->board = q->name;
			st->match_kind = kind;
			return;
		}
	}
	for (pass = 0; pass < 2; pass++) {
		pq = alc_family_pin_quirks(st->family, pass);
		for (; pq && pq->codec_id; pq++) {
			if (!alc_pin_quirk_hit(c, st, pq))
				continue;
			if (pq->recipe > 0 && pq->recipe < R_COUNT) {
				st->rcp = &alc_recipes[pq->recipe];
				st->board = pq->name;
				st->match_kind = pass ? "pin layout (fallback)" :
							"pin layout";
				return;
			}
		}
	}
	if (st->family == ALC_FAM_269 && alc_ssid_usable(st->pci_ssid)) {
		for (q = alc269_vendor_quirks; q->subsys; q++) {
			if (!alc_quirk_hit(q, st->pci_ssid))
				continue;
			r = q->flags >> ALC_FIX_RECIPE_SHIFT;
			if (r > 0 && r < R_COUNT) {
				st->rcp = &alc_recipes[r];
				st->board = q->name;
				st->match_kind = "vendor";
			}
			return;
		}
	}
}

/* ==========================================================================
 * BIOS SKU word
 *
 * The low 16 bits of the codec subsystem id (when they differ from the PCI
 * one and bit 0 is set), or the default config of the SKU pin (0x1d, 0x17
 * on ALC260) with a valid checksum, describe the board:
 *   bit 0        override valid
 *   bit 2        0 desktop, 1 laptop
 *   bits 3-5     external amplifier control (1: GPIO0, 3: GPIO1, 7: GPIO2)
 *   bits 11-12   which port is the headphone jack
 *   bit 15       mute the speaker when headphones are plugged
 * ========================================================================== */

static void alc_parse_sku(struct hda_codec *c, struct alc_state *st)
{
	uint32_t ass, sum = 0;
	struct hda_widget *w;
	uint8_t nid;
	int i;

	st->sku_valid = 0;
	st->sku_hp_port = -1;
	ass = c->subsystem_id & 0xffff;
	if ((ass & 1) && ass != st->pci_ssdid)
		goto valid;
	nid = (st->vendor_id == 0x10ec0260) ? 0x17 : 0x1d;
	w = alc_pin(c, nid);
	if (!w)
		return;
	ass = w->pin_cfg_orig;
	if (!(ass & 1) || (ass >> 30) != 1)
		return;
	for (i = 1; i < 16; i++)
		if ((ass >> i) & 1)
			sum++;
	if (((ass >> 16) & 0xf) != sum)
		return;
valid:
	st->sku = ass;
	st->sku_valid = 1;
	st->sku_platform_laptop = (ass >> 2) & 1;
	if (ass & 0x8000)
		st->sku_hp_port = (int)((ass >> 11) & 3);
}

/* What the SKU says about amplifiers, unless a recipe already decided. */
static void alc_apply_sku(struct hda_codec *c, struct alc_state *st)
{
	static const uint8_t ports_269[4] = { 0, 0x1b, 0x14, 0x21 };
	static const uint8_t ports_va[4] = { 0x15, 0x1b, 0x14, 0 };
	static const uint8_t ports_663[4] = { 0x15, 0x1b, 0x14, 0x21 };
	static const uint8_t ports_260[4] = { 0x10, 0x15, 0x0f, 0 };
	static const uint8_t ports_861[4] = { 0x0e, 0x0f, 0x0b, 0 };
	const uint8_t *ports;
	struct hda_widget *w;
	uint8_t hp;
	int i, have_hp = 0;

	if ((st->ops & ALC_OP_SKU_IGNORE) || !st->sku_valid) {
		if (st->init_amp == ALC_INIT_UNDEFINED)
			st->init_amp = ALC_INIT_DEFAULT;
		return;
	}
	if (st->init_amp == ALC_INIT_UNDEFINED) {
		switch ((st->sku & 0x38) >> 3) {
		case 1:
			alc_setup_gpio(st, 0x01);
			break;
		case 3:
			alc_setup_gpio(st, 0x02);
			break;
		case 7:
			alc_setup_gpio(st, 0x04);
			break;
		default:
			st->init_amp = ALC_INIT_DEFAULT;
			break;
		}
	}
	if (st->sku_hp_port < 0 || (st->ops & ALC_OP_NO_HP_FIXUP))
		return;

	/* Board says "mute speakers on headphone": make sure the parser sees
	 * a headphone on the indicated port if the BIOS forgot to mark one. */
	for (i = 0; i < c->num_nodes; i++) {
		w = &c->w[i];
		if (w->type == AC_WID_PIN && !cfg_unconnected(w->pin_cfg) &&
		    cfg_device(w->pin_cfg) == AC_JACK_HP_OUT)
			have_hp = 1;
	}
	if (have_hp)
		return;
	switch (st->family) {
	case ALC_FAM_269:
		ports = (st->type == ALC_T_269VA || st->type == ALC_T_269VC ||
			 st->type == ALC_T_280 || st->type == ALC_T_284 ||
			 st->type == ALC_T_293) ? ports_va : ports_269;
		break;
	case ALC_FAM_662:
		ports = alc_pin(c, 0x21) ? ports_663 : ports_va;
		break;
	case ALC_FAM_260:
		ports = ports_260;
		break;
	case ALC_FAM_861:
		ports = ports_861;
		break;
	default:
		ports = ports_va;
		break;
	}
	hp = ports[st->sku_hp_port & 3];
	w = alc_pin(c, hp);
	if (!w || cfg_unconnected(w->pin_cfg) || cfg_internal(w->pin_cfg))
		return;
	if (cfg_device(w->pin_cfg) != AC_JACK_LINE_OUT)
		return;
	hda_pin_override_cfg(c, hp,
			     (w->pin_cfg & ~AC_DEFCFG_DEVICE) |
				     ((uint32_t)AC_JACK_HP_OUT
				      << AC_DEFCFG_DEVICE_SHIFT));
}

/* ==========================================================================
 * Pin configuration sanity
 * ========================================================================== */

static int cfg_is_output(uint32_t cfg)
{
	switch (cfg_device(cfg)) {
	case AC_JACK_LINE_OUT:
	case AC_JACK_SPEAKER:
	case AC_JACK_HP_OUT:
		return 1;
	}
	return 0;
}

static void alc_set_default(struct hda_codec *c, uint8_t nid, uint32_t cfg,
			    int need_out)
{
	struct hda_widget *w = alc_pin(c, nid);

	if (!w)
		return;
	if (need_out && !(w->pincap & AC_PINCAP_OUT))
		return;
	if (!need_out && !(w->pincap & AC_PINCAP_IN))
		return;
	hda_pin_override_cfg(c, nid, cfg);
}

/* Firmware without a Realtek verb table (some coreboot builds, bare
 * evaluation boards) leaves every pin unconfigured, and the parser would
 * find nothing to play to.  Invent the usual layout for the chip family. */
static void alc_fill_missing_pins(struct hda_codec *c, struct alc_state *st)
{
	int i, outs = 0, zeros = 0, pins = 0, laptop;
	uint8_t ignore = 0;

	switch (st->family) {
	case ALC_FAM_269:
	case ALC_FAM_662:
	case ALC_FAM_882:
	case ALC_FAM_262:
	case ALC_FAM_861:
	case ALC_FAM_861VD:
		ignore = 0x1d; /* the SKU / PC-beep pin */
		break;
	case ALC_FAM_260:
		ignore = 0x17;
		break;
	}

	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		pins++;
		if (w->nid == ignore) {
			if (!cfg_unconnected(w->pin_cfg))
				hda_pin_override_cfg(c, w->nid, 0x411111f0);
			continue;
		}
		if (w->pin_cfg == 0) {
			zeros++;
			continue;
		}
		if (!cfg_unconnected(w->pin_cfg) && cfg_is_output(w->pin_cfg))
			outs++;
	}
	if (outs) {
		/* a configured board: an all-zero word is "not programmed",
		 * not "line out jack, association 0" */
		if (zeros) {
			for (i = 0; i < c->num_nodes; i++) {
				struct hda_widget *w = &c->w[i];

				if (w->type == AC_WID_PIN && w->pin_cfg == 0)
					hda_pin_override_cfg(c, w->nid,
							     0x411111f0);
			}
		}
		return;
	}
	if (!pins)
		return;

	laptop = st->sku_valid ? st->sku_platform_laptop :
				 st->family == ALC_FAM_269;
	snd_info("hda", "%s: BIOS left no output pin configured; "
		"using a default %s layout\n",
		c->chip_name, laptop ? "laptop" : "desktop");

	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type == AC_WID_PIN && w->nid != ignore)
			hda_pin_override_cfg(c, w->nid, 0x411111f0);
	}
	if (st->family == ALC_FAM_269 && laptop) {
		alc_set_default(c, 0x14, 0x90170110, 1); /* internal speaker */
		if (alc_pin(c, 0x21))
			alc_set_default(c, 0x21, 0x02211020, 1); /* HP jack */
		else
			alc_set_default(c, 0x15, 0x02211020, 1);
		alc_set_default(c, 0x12, 0x90a60130, 0); /* digital mic */
		if (alc_pin(c, 0x19))
			alc_set_default(c, 0x19, 0x02a11030, 0); /* mic jack */
		else
			alc_set_default(c, 0x18, 0x02a11030, 0);
	} else if (st->family == ALC_FAM_269) {
		alc_set_default(c, 0x14, 0x01014010, 1); /* rear line out */
		if (alc_pin(c, 0x21))
			alc_set_default(c, 0x21, 0x02211020, 1); /* front HP */
		else
			alc_set_default(c, 0x15, 0x02211020, 1);
		alc_set_default(c, 0x18, 0x01a19040, 0); /* rear mic */
		alc_set_default(c, 0x19, 0x02a19050, 0); /* front mic */
		alc_set_default(c, 0x1a, 0x0181304f, 0); /* rear line in */
	} else if (st->family == ALC_FAM_260) {
		alc_set_default(c, 0x0f, 0x01014010, 1); /* line out */
		alc_set_default(c, 0x10, 0x02214020, 1); /* HP */
		alc_set_default(c, 0x12, 0x01a19030, 0); /* mic */
		alc_set_default(c, 0x14, 0x0181303f, 0); /* line in */
	} else if (st->family == ALC_FAM_861) {
		alc_set_default(c, 0x0e, 0x01014010, 1);
		alc_set_default(c, 0x0f, 0x02214020, 1);
		alc_set_default(c, 0x10, 0x01a19030, 0);
		alc_set_default(c, 0x0c, 0x0181303f, 0);
	} else {
		/* 662/882/262/268/861-VD/680 desktop layout */
		alc_set_default(c, 0x14, 0x01014010, 1); /* rear line out */
		if (laptop && alc_pin(c, 0x21)) {
			alc_set_default(c, 0x21, 0x02211020, 1);
		} else {
			alc_set_default(c, 0x1b, 0x02214020, 1); /* front HP */
			alc_set_default(c, 0x15, 0x02214020, 1);
		}
		alc_set_default(c, 0x18, 0x01a19030, 0); /* rear mic */
		alc_set_default(c, 0x19, 0x02a19040, 0); /* front mic */
		alc_set_default(c, 0x1a, 0x0181303f, 0); /* line in */
	}
}

/* Keep the internal-mic boost to 0/+10 dB; the higher steps only amplify
 * noise from the laptop's own microphone. */
static void alc_limit_int_mic_boost(struct hda_codec *c)
{
	int i;

	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN || !(w->caps & AC_WCAP_IN_AMP))
			continue;
		if (cfg_device(w->pin_cfg) != AC_JACK_MIC_IN ||
		    !cfg_internal(w->pin_cfg))
			continue;
		w->amp_in_caps = (0x00u << AC_AMPCAP_OFFSET_SHIFT) |
				 (0x01u << AC_AMPCAP_NUM_STEPS_SHIFT) |
				 (0x2fu << AC_AMPCAP_STEP_SIZE_SHIFT);
		w->caps |= AC_WCAP_AMP_OVRD;
	}
}

/* Headphone DAC 0x03 with ten fewer (louder) steps: some ALC256 boards
 * drive their headphone amp into clipping at the top. */
static void alc_limit_hp_dac(struct hda_codec *c)
{
	struct hda_widget *w = hda_widget(c, 0x03);
	uint32_t caps, steps, offs;

	if (!w || w->type != AC_WID_AUD_OUT || !(w->caps & AC_WCAP_OUT_AMP))
		return;
	caps = hda_amp_caps(c, 0x03, HDA_OUTPUT);
	steps = (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
	offs = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;
	if (steps <= 10 || offs < 10)
		return;
	steps -= 10;
	offs -= 10;
	caps &= ~(AC_AMPCAP_NUM_STEPS | AC_AMPCAP_OFFSET);
	caps |= (steps << AC_AMPCAP_NUM_STEPS_SHIFT) |
		(offs << AC_AMPCAP_OFFSET_SHIFT);
	w->amp_out_caps = caps;
	w->caps |= AC_WCAP_AMP_OVRD;
}

/* ==========================================================================
 * Output routing: DAC preferences
 *
 * Some boards only sound right when particular outputs use particular DACs
 * (the speaker DAC carries a hidden EQ, a DAC has no volume amplifier, a
 * DAC introduces pops).  After the generic parser chose its paths, outputs
 * are rerouted where a recipe asks for it, and outputs that ended up on a
 * DAC without any volume control are moved to share a DAC that has one.
 * ========================================================================== */

struct alc_route {
	uint8_t nid[HDA_MAX_PATH]; /* pin first */
	uint8_t idx[HDA_MAX_PATH]; /* idx[i]: connection of nid[i] to nid[i+1] */
	int depth;
};

static int alc_is_analog_dac(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	return w && w->type == AC_WID_AUD_OUT && !(w->caps & AC_WCAP_DIGITAL);
}

static uint32_t amp_steps(uint32_t caps)
{
	return (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
}

static int alc_dac_has_volume(struct hda_codec *c, uint8_t dac)
{
	struct hda_widget *w = hda_widget(c, dac);

	if (!w || !(w->caps & AC_WCAP_OUT_AMP))
		return 0;
	return amp_steps(hda_amp_caps(c, dac, HDA_OUTPUT)) > 0;
}

static int alc_route_dfs(struct hda_codec *c, struct alc_route *r, int depth,
			 int maxd, uint8_t dac, uint8_t avoid,
			 uint8_t *visited)
{
	struct hda_widget *w = hda_widget(c, r->nid[depth]);
	int i;

	if (!w)
		return 0;
	for (i = 0; i < w->nconn && i < HDA_MAX_CONNS; i++) {
		uint8_t src = w->conn[i];
		struct hda_widget *sw;

		if (src == dac) {
			r->idx[depth] = (uint8_t)i;
			r->nid[depth + 1] = dac;
			r->depth = depth + 2;
			return 1;
		}
		if (depth + 2 >= maxd || src == avoid)
			continue;
		sw = hda_widget(c, src);
		if (!sw || (sw->type != AC_WID_AUD_MIX &&
			    sw->type != AC_WID_AUD_SEL))
			continue;
		if (visited[src >> 3] & (1u << (src & 7)))
			continue;
		visited[src >> 3] |= (uint8_t)(1u << (src & 7));
		r->idx[depth] = (uint8_t)i;
		r->nid[depth + 1] = src;
		if (alc_route_dfs(c, r, depth + 1, maxd, dac, avoid, visited))
			return 1;
	}
	return 0;
}

/* Shortest route pin -> ... -> dac that does not pass the loopback mixer. */
static int alc_find_route(struct hda_codec *c, uint8_t pin, uint8_t dac,
			  struct hda_path *p)
{
	struct alc_state *st = alc_st(c);
	struct alc_route r;
	uint8_t visited[32];
	int maxd, i;

	if (!alc_pin(c, pin) || !alc_is_analog_dac(c, dac))
		return 0;
	for (maxd = 2; maxd <= HDA_MAX_PATH; maxd++) {
		mm_memset(&r, 0, sizeof(r));
		mm_memset(visited, 0, sizeof(visited));
		r.nid[0] = pin;
		if (!alc_route_dfs(c, &r, 0, maxd, dac, st->aamix_nid, visited))
			continue;
		/* into signal order: dac first */
		mm_memset(p, 0, sizeof(*p));
		p->depth = (uint8_t)r.depth;
		for (i = 0; i < r.depth; i++) {
			p->nid[i] = r.nid[r.depth - 1 - i];
			/* idx[i] belongs to nid[i]: its connection to nid[i-1] */
			p->idx[i] = i ? r.idx[r.depth - 1 - i] : 0;
		}
		return 1;
	}
	return 0;
}

/* Where volume and mute live on a path: volume as close to the DAC as
 * possible, mute as close to the pin as possible. */
static void alc_path_amps(struct hda_codec *c, struct hda_path *p)
{
	int i;

	p->vol_nid = p->vol_dir = p->vol_idx = 0;
	p->mute_nid = p->mute_dir = p->mute_idx = 0;
	for (i = 0; i < p->depth; i++) {
		struct hda_widget *w = hda_widget(c, p->nid[i]);

		if (!w)
			continue;
		if ((w->caps & AC_WCAP_OUT_AMP) &&
		    amp_steps(hda_amp_caps(c, w->nid, HDA_OUTPUT))) {
			p->vol_nid = w->nid;
			p->vol_dir = HDA_OUTPUT;
			p->vol_idx = 0;
			break;
		}
		if (i && w->type == AC_WID_AUD_MIX &&
		    (w->caps & AC_WCAP_IN_AMP) &&
		    amp_steps(hda_amp_caps(c, w->nid, HDA_INPUT))) {
			p->vol_nid = w->nid;
			p->vol_dir = HDA_INPUT;
			p->vol_idx = p->idx[i];
			break;
		}
	}
	for (i = p->depth - 1; i >= 0; i--) {
		struct hda_widget *w = hda_widget(c, p->nid[i]);

		if (!w)
			continue;
		if ((w->caps & AC_WCAP_OUT_AMP) &&
		    (hda_amp_caps(c, w->nid, HDA_OUTPUT) & AC_AMPCAP_MUTE)) {
			p->mute_nid = w->nid;
			p->mute_dir = HDA_OUTPUT;
			p->mute_idx = 0;
			break;
		}
		if (i && w->type == AC_WID_AUD_MIX &&
		    (w->caps & AC_WCAP_IN_AMP) &&
		    (hda_amp_caps(c, w->nid, HDA_INPUT) & AC_AMPCAP_MUTE)) {
			p->mute_nid = w->nid;
			p->mute_dir = HDA_INPUT;
			p->mute_idx = p->idx[i];
			break;
		}
	}
}

static void alc_program_route(struct hda_codec *c, const struct hda_path *p)
{
	int i;

	for (i = 0; i < p->depth; i++) {
		struct hda_widget *w = hda_widget(c, p->nid[i]);

		if (!w)
			continue;
		w->used = 1;
		if (!i)
			continue;
		if (w->nconn > 1 &&
		    (w->type == AC_WID_AUD_SEL || w->type == AC_WID_PIN)) {
			hda_codec_write(c, w->nid, AC_VERB_SET_CONNECT_SEL,
					p->idx[i]);
			w->cur_conn = p->idx[i];
		}
		if (w->type == AC_WID_AUD_MIX && (w->caps & AC_WCAP_IN_AMP)) {
			uint32_t caps = hda_amp_caps(c, w->nid, HDA_INPUT);
			uint32_t zero = (caps & AC_AMPCAP_OFFSET) >>
					AC_AMPCAP_OFFSET_SHIFT;

			hda_amp_set(c, w->nid, HDA_INPUT, p->idx[i], zero,
				    zero, 0);
		}
	}
}

static int alc_reroute_out(struct hda_codec *c, struct hda_out *o,
			   uint8_t dac)
{
	struct hda_path p;

	if (!alc_find_route(c, o->pin, dac, &p))
		return -1;
	p.active = o->path.active;
	alc_path_amps(c, &p);
	alc_program_route(c, &p);
	o->path = p;
	o->dac = dac;
	return 0;
}

static int alc_dac_avoided(const struct alc_dacpref *d, uint8_t pin,
			   uint8_t dac)
{
	for (; d && (d->pin || d->dac); d++) {
		if (!(d->dac & ALC_DAC_AVOID_FLAG))
			continue;
		if ((d->dac & 0x7f) == dac && (!d->pin || d->pin == pin))
			return 1;
	}
	return 0;
}

static uint8_t alc_dac_wanted(const struct alc_dacpref *d, uint8_t pin)
{
	for (; d && (d->pin || d->dac); d++)
		if (d->pin == pin && !(d->dac & ALC_DAC_AVOID_FLAG))
			return d->dac;
	return 0;
}

static int alc_out_is_analog(const struct hda_out *o)
{
	return o->type == AUDIO_OUT_SPEAKER || o->type == AUDIO_OUT_HEADPHONE ||
	       o->type == AUDIO_OUT_LINE || o->type == AUDIO_OUT_HEADSET;
}

static int alc_out_is_hp(const struct hda_out *o)
{
	return o->type == AUDIO_OUT_HEADPHONE || o->type == AUDIO_OUT_HEADSET;
}

/* Move an output to another DAC: first one already used by an output of
 * the same kind, then any DAC already in use, then any DAC at all. */
static int alc_move_out(struct hda_codec *c, struct hda_out *o,
			const struct alc_dacpref *d)
{
	struct hda_spec *s = &c->spec;
	int pass, i;

	for (pass = 0; pass < 3; pass++) {
		if (pass < 2) {
			for (i = 0; i < s->nout; i++) {
				struct hda_out *x = &s->out[i];

				if (x == o || !x->dac || x->dac == o->dac ||
				    !alc_out_is_analog(x))
					continue;
				if (pass == 0 && alc_out_is_hp(x) != alc_out_is_hp(o))
					continue;
				if (alc_dac_avoided(d, o->pin, x->dac) ||
				    !alc_dac_has_volume(c, x->dac))
					continue;
				if (alc_reroute_out(c, o, x->dac) == 0)
					return 0;
			}
		} else {
			for (i = 0; i < c->num_nodes; i++) {
				struct hda_widget *w = &c->w[i];

				if (w->nid == o->dac ||
				    !alc_is_analog_dac(c, w->nid) ||
				    alc_dac_avoided(d, o->pin, w->nid) ||
				    !alc_dac_has_volume(c, w->nid))
					continue;
				if (alc_reroute_out(c, o, w->nid) == 0)
					return 0;
			}
		}
	}
	return -1;
}

static void alc_update_spec_dacs(struct hda_codec *c)
{
	struct hda_spec *s = &c->spec;
	uint8_t primary = 0, hp = 0;
	int i;

	for (i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];

		if (alc_out_is_analog(o) && !alc_out_is_hp(o) && o->dac &&
		    !primary)
			primary = o->dac;
	}
	for (i = 0; i < s->nout; i++) {
		struct hda_out *o = &s->out[i];

		if (!alc_out_is_hp(o) || !o->dac)
			continue;
		if (!primary)
			primary = o->dac;
		else if (o->dac != primary && !hp)
			hp = o->dac;
	}
	if (primary) {
		s->primary_dac = primary;
		s->hp_dac = hp;
	}
}

static void alc_route_outputs(struct hda_codec *c, struct alc_state *st)
{
	struct hda_spec *s = &c->spec;
	const struct alc_dacpref *d = st->rcp ? st->rcp->dacs : NULL;
	int i, changed = 0;

	for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
		struct hda_out *o = &s->out[i];
		uint8_t want;

		if (!alc_out_is_analog(o) || !o->pin)
			continue;
		want = alc_dac_wanted(d, o->pin);
		if (want && want != o->dac && alc_is_analog_dac(c, want)) {
			if (alc_reroute_out(c, o, want) == 0) {
				changed = 1;
				continue;
			}
			snd_info("hda", "%s: no route from pin 0x%02x to "
				"DAC 0x%02x\n",
				c->chip_name, o->pin, want);
		}
		if (o->dac && alc_dac_avoided(d, o->pin, o->dac)) {
			if (alc_move_out(c, o, d) == 0)
				changed = 1;
		}
	}

	/* outputs left without any volume control */
	for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
		struct hda_out *o = &s->out[i];
		struct hda_path tmp;

		if (!alc_out_is_analog(o) || !o->pin || !o->dac)
			continue;
		if (alc_dac_wanted(d, o->pin) == o->dac)
			continue; /* the board wants exactly this */
		tmp = o->path;
		alc_path_amps(c, &tmp);
		if (tmp.vol_nid)
			continue;
		if (alc_move_out(c, o, d) == 0)
			changed = 1;
	}

	if (changed) {
		alc_update_spec_dacs(c);
		for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
			const struct hda_out *o = &s->out[i];

			if (alc_out_is_analog(o) && o->pin)
				snd_info("hda", "%s: pin 0x%02x <- DAC "
					"0x%02x\n",
					c->chip_name, o->pin, o->dac);
		}
	}
}

/* The analog loopback mixer feeds the inputs straight to the outputs.  We
 * never use it; on several boards it injects noise, so silence it (unless
 * the parser routed an output through it). */
static void alc_silence_aamix(struct hda_codec *c, struct alc_state *st)
{
	struct hda_spec *s = &c->spec;
	struct hda_widget *mix;
	int i, j;

	if (!st->aamix_nid)
		return;
	mix = hda_widget(c, st->aamix_nid);
	if (!mix || mix->type != AC_WID_AUD_MIX)
		return;
	for (i = 0; i < s->nout; i++)
		for (j = 0; j < s->out[i].path.depth && j < HDA_MAX_PATH; j++)
			if (s->out[i].path.nid[j] == st->aamix_nid)
				return;
	if (mix->caps & AC_WCAP_IN_AMP)
		for (i = 0; i < mix->nconn && i < 16; i++)
			alc_raw_amp(c, mix->nid, AMP_IN_MUTE_IDX(i));
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_AUD_MIX || w->nid == st->aamix_nid ||
		    !(w->caps & AC_WCAP_IN_AMP))
			continue;
		for (j = 0; j < w->nconn && j < 16; j++)
			if (w->conn[j] == st->aamix_nid)
				alc_raw_amp(c, w->nid, AMP_IN_MUTE_IDX(j));
	}
}

/* ==========================================================================
 * Pins of interest after parsing
 * ========================================================================== */

static void alc_find_pins(struct hda_codec *c, struct alc_state *st)
{
	struct hda_spec *s = &c->spec;
	int i;

	st->hp_pin = st->hp2_pin = 0;
	st->hs_mic_pin = st->hp_mic_pin = st->int_mic_pin = 0;
	st->nspk = 0;

	/* headphones: prefer the front jack 0x21 (the combo jack on the
	 * 269 family), then any jack-detectable one */
	for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &s->out[i];

		if (alc_out_is_hp(o) && o->pin == 0x21)
			st->hp_pin = 0x21;
	}
	for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &s->out[i];

		if (!alc_out_is_hp(o) || !o->pin || o->pin == st->hp_pin)
			continue;
		if (!st->hp_pin && !o->dock)
			st->hp_pin = o->pin;
		else if (!st->hp2_pin)
			st->hp2_pin = o->pin;
	}
	if (!st->hp_pin && st->hp2_pin) {
		st->hp_pin = st->hp2_pin;
		st->hp2_pin = 0;
	}
	for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &s->out[i];

		if (o->type == AUDIO_OUT_SPEAKER && st->nspk < 4)
			st->spk_pin[st->nspk++] = o->pin;
	}

	/* microphones from the pin configuration: association-sequence 0xc
	 * marks the headset mic, 0xd the headphone mic */
	for (i = 0; i < c->num_nodes; i++) {
		const struct hda_widget *w = &c->w[i];
		uint32_t cfg = w->pin_cfg;

		if (w->type != AC_WID_PIN || cfg_unconnected(cfg) ||
		    cfg_device(cfg) != AC_JACK_MIC_IN)
			continue;
		if (cfg_internal(cfg)) {
			if (!st->int_mic_pin)
				st->int_mic_pin = w->nid;
			continue;
		}
		if ((cfg & AC_DEFCFG_SEQUENCE) == 0xc && !st->hs_mic_pin)
			st->hs_mic_pin = w->nid;
		else if ((cfg & AC_DEFCFG_SEQUENCE) == 0xd && !st->hp_mic_pin)
			st->hp_mic_pin = w->nid;
	}
	if (!st->hs_mic_pin && (st->ops & (ALC_OP_HS_MODE | ALC_OP_HS_MIC))) {
		/* first external mic that is not on a docking station */
		for (i = 0; i < c->num_nodes; i++) {
			const struct hda_widget *w = &c->w[i];
			uint32_t cfg = w->pin_cfg;

			if (w->type != AC_WID_PIN || cfg_unconnected(cfg) ||
			    cfg_device(cfg) != AC_JACK_MIC_IN ||
			    cfg_internal(cfg) ||
			    (cfg_location(cfg) & 0x30) == AC_JACK_LOC_SEPARATE)
				continue;
			if (w->nid == st->hp_mic_pin)
				continue;
			st->hs_mic_pin = w->nid;
			break;
		}
	}
	if (st->ops & ALC_OP_HS_NO_HPMIC)
		st->hp_mic_pin = 0;
}


/* ==========================================================================
 * Chip identification
 * ========================================================================== */

static const struct alc_chip *alc_find_chip(uint32_t vid, uint32_t rev)
{
	const struct alc_chip *ch;

	for (ch = alc_chips; ch->vendor_id; ch++) {
		if (ch->vendor_id != vid)
			continue;
		if (ch->rev && ch->rev != rev)
			continue;
		return ch;
	}
	return NULL;
}

/* An id we do not list: guess the family from the part number. */
static void alc_guess_family(uint32_t vid, uint8_t *family, uint8_t *type)
{
	uint32_t dev = vid & 0xffff;

	*type = ALC_T_GENERIC;
	if ((dev >= 0x0200 && dev < 0x0300) || (dev >= 0x0700 && dev < 0x0800)) {
		*family = ALC_FAM_269;
	} else if ((dev >= 0x0660 && dev < 0x0700) || dev == 0x0272 ||
		   dev == 0x0867 || dev == 0x0892 || dev == 0x0897) {
		*family = ALC_FAM_662;
		*type = ALC_T_662;
	} else {
		*family = ALC_FAM_882;
		*type = ALC_T_882;
	}
}

static void alc_name_chip(struct hda_codec *c, struct alc_state *st)
{
	const struct alc_coef_rename *cr;
	const struct alc_oem_rename *orn;
	const char *name = st->chip ? st->chip->name : NULL;
	uint16_t coef0 = alc_coef0(c);

	alc_strlcpy(c->vendor_name, "Realtek", sizeof(c->vendor_name));
	if (st->vendor_id == 0x19e58326)
		alc_strlcpy(c->vendor_name, "Huawei", sizeof(c->vendor_name));

	for (cr = alc_coef_renames; cr->vendor_id; cr++) {
		if (cr->vendor_id == st->vendor_id &&
		    (coef0 & cr->coef_mask) == cr->coef_bits) {
			name = cr->name;
			break;
		}
	}
	/* OEM part numbers (the PCI subsystem vendor ordered them) */
	if (st->pci_ssvid) {
		for (orn = alc_oem_renames; orn->vendor_id; orn++) {
			if (orn->vendor_id != st->vendor_id ||
			    orn->ssvid != st->pci_ssvid)
				continue;
			if (orn->ssdid && orn->ssdid != st->pci_ssdid)
				continue;
			name = orn->name;
			break;
		}
	}
	/* variants told apart by board type */
	if (st->vendor_id == 0x10ec0269) {
		if ((coef0 & 0x00f0) == 0x0010 && st->pci_ssvid == 0x1025 &&
		    st->sku_valid && st->sku_platform_laptop)
			name = "ALC271X";
		if ((coef0 & 0x00f0) == 0x0020 && st->pci_ssid == 0x17aa21f3)
			name = "ALC3202";
	}
	if (st->family == ALC_FAM_662 && (coef0 & (1 << 14)) &&
	    st->pci_ssvid == 0x1025 && st->sku_valid &&
	    st->sku_platform_laptop)
		name = "ALC272X";

	if (name)
		alc_strlcpy(c->chip_name, name, sizeof(c->chip_name));
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "ALC%x",
			  st->vendor_id & 0xffff);
}

/* Chip class details that depend on the silicon revision or the platform. */
static void alc_chip_setup(struct hda_codec *c, struct alc_state *st)
{
	uint16_t coef0 = alc_coef0(c);

	st->en_3kpull_low = 1;
	st->init_amp = ALC_INIT_UNDEFINED;

	switch (st->family) {
	case ALC_FAM_269:
		st->aamix_nid = 0x0b;
		break;
	case ALC_FAM_662:
	case ALC_FAM_882:
	case ALC_FAM_262:
	case ALC_FAM_861VD:
		st->aamix_nid = 0x0b;
		break;
	case ALC_FAM_260:
		st->aamix_nid = 0x07;
		break;
	case ALC_FAM_861:
		st->aamix_nid = 0x15;
		break;
	default:
		st->aamix_nid = 0; /* ALC268, ALC680: no loopback */
		break;
	}

	switch (st->type) {
	case ALC_T_269VA:
		switch (coef0 & 0x00f0) {
		case 0x0010:
			st->type = ALC_T_269VB;
			break;
		case 0x0020:
			st->type = ALC_T_269VC;
			break;
		case 0x0030:
			st->type = ALC_T_269VD;
			break;
		default:
			alc_fix_pll_init(c, ALC_COEF_NID, 0x04, 15);
			break;
		}
		break;
	case ALC_T_215:
		if (coef0 & 0x0010)
			st->type = ALC_T_245;
		st->aamix_nid = 0;
		break;
	case ALC_T_256:
		st->aamix_nid = 0; /* no loopback path */
		/* the 3k pull-down on shutdown confuses mic detection on
		 * Intel platforms with ALC236 */
		if (st->vendor_id == 0x10ec0236 && st->ctrl_vendor != 0x1022)
			st->en_3kpull_low = 0;
		break;
	case ALC_T_257:
		st->aamix_nid = 0;
		st->en_3kpull_low = 0;
		break;
	case ALC_T_225:
	case ALC_T_287:
	case ALC_T_294:
	case ALC_T_300:
	case ALC_T_700:
		st->aamix_nid = 0;
		break;
	case ALC_T_662:
	case ALC_T_668:
		alc_fix_pll_init(c, ALC_COEF_NID, 0x04, 15);
		break;
	case ALC_T_883:
		alc_fix_pll_init(c, ALC_COEF_NID, 0x0a, 10);
		break;
	default:
		break;
	}
	if (st->family == ALC_FAM_262)
		alc_fix_pll_init(c, ALC_COEF_NID, 0x0a, 10);

	/* probe-time COEFs of particular classes */
	if (st->type == ALC_T_294) /* UAJ mic VREF by verb */
		alc_coef_update(c, 0x6b, 0x0018, (1 << 4) | (1 << 3));
	if (st->type == ALC_T_700) /* combo jack auto trigger off */
		alc_coef_update(c, 0x4a, 1 << 15, 0);
	if (st->type == ALC_T_269VB)
		alc269_fill_coef(c);
}

/* ALC5505 DSP companion: widget 0x51 answers with its own vendor id. */
static int alc_detect_dsp5505(struct hda_codec *c, struct alc_state *st)
{
	uint32_t v;

	if (st->family != ALC_FAM_269)
		return 0;
	if (c->num_nodes && c->start_nid + c->num_nodes <= ALC_DSP5505_NID)
		v = hda_codec_read(c, ALC_DSP5505_NID, AC_VERB_PARAMETERS,
				   AC_PAR_VENDOR_ID);
	else
		return 0;
	return v == 0x10ec5505;
}

/* ==========================================================================
 * Recipe actions
 * ========================================================================== */

static void alc_recipe_pre(struct hda_codec *c, struct alc_state *st)
{
	const struct alc_recipe *r = st->rcp;
	uint64_t ops;
	struct hda_widget *w;

	if (!r)
		return;
	ops = st->ops = r->ops;

	if (r->pins)
		hda_apply_pincfgs(c, r->pins);

	if (r->gpio_mask) {
		st->gpio_mask |= r->gpio_mask;
		st->gpio_dir |= r->gpio_mask;
		st->gpio_data = (st->gpio_data & ~(uint32_t)r->gpio_mask) |
				(r->gpio_data & r->gpio_mask);
	}
	if (r->mute_led && r->mute_led < ALC_LED_COUNT) {
		st->mute_led = &alc_leds[r->mute_led];
		if (st->mute_led->kind == ALC_LEDK_GPIO) {
			st->gpio_mask |= st->mute_led->gpio;
			st->gpio_dir |= st->mute_led->gpio;
		}
	}
	if (r->micmute_led && r->micmute_led < ALC_LED_COUNT) {
		st->micmute_led = &alc_leds[r->micmute_led];
		if (st->micmute_led->kind == ALC_LEDK_GPIO) {
			st->gpio_mask |= st->micmute_led->gpio;
			st->gpio_dir |= st->micmute_led->gpio;
		}
	}

	if (ops & ALC_OP_NO_SHUTUP)
		st->no_shutup_pins = 1;
	if (ops & ALC_OP_ULTRA_LOW_POWER)
		st->ultra_low_power = 1;
	if (ops & ALC_OP_NO_INT_MIC)
		st->no_internal_mic_pin = 1;
	if (ops & (ALC_OP_COMBO_SWAP | ALC_OP_NO_INT_MIC))
		alc_coef_update(c, 0x45, (0xf << 12) | (1 << 10), 5 << 12);
	if (ops & ALC_OP_JACK_CTIA_DEFAULT)
		alc255_default_jack_ctia(c);
	if (ops & ALC_OP_ALC668_HS) {
		alc_coef_write(c, 0xc4, 0x8000);
		alc_coef_update(c, 0xc2, 0xff01, 0);
		alc_set_pinctl(c, 0x18, 0);
	}
	if (ops & ALC_OP_ALC662_HS) {
		/* the headphone pin doubles as mic input: no boost there */
		alc_raw_amp(c, 0x1b, AC_AMP_SET_INPUT | AC_AMP_SET_LEFT |
					     AC_AMP_SET_RIGHT);
		w = alc_pin(c, 0x1b);
		if (w)
			w->caps &= ~AC_WCAP_IN_AMP;
	}
	if (ops & ALC_OP_CLEVO_P950)
		alc_coef_update(c, 0x07, 0, 0x3c3);
	if (ops & ALC_OP_NJ51CU) {
		if (st->vendor_id == 0x10ec0256) {
			alc_coef_update(c, 0x10, 1 << 9, 0);
			hda_pin_override_cfg(c, 0x19, 0x04a11120);
		} else {
			hda_pin_override_cfg(c, 0x1a, 0x04a1113c);
		}
	}
	if (ops & ALC_OP_SAMSUNG_AMP2)
		st->samsung_amps = 2;
	if (ops & ALC_OP_SAMSUNG_AMP4)
		st->samsung_amps = 4;
	if (ops & ALC_OP_ENVY_X360)
		alc_raw_amp(c, 0x21, AMP_OUT_UNMUTE_ALL);
	if (ops & ALC_OP_ALC256_COEF_DEFAULTS)
		alc_coef_seq(c, alc256_coef_defaults);
	if (ops & ALC_OP_ALC233_JACK_COEFS)
		alc_coef_seq(c, alc233_jack_coefs);
	if (ops & ALC_OP_INT_MIC_BOOST1)
		alc_limit_int_mic_boost(c);
	if (ops & ALC_OP_HP_AMP_LIMIT)
		alc_limit_hp_dac(c);
	if (ops & ALC_OP_GPIO03_PULSE) {
		st->gpio_mask |= 0x03;
		st->gpio_dir |= 0x03;
	}
	if (ops & ALC_OP_DELL_ALC288) {
		st->gpio_mask |= 0x40;
		st->gpio_dir |= 0x40;
	}
	if (ops & ALC_OP_EXT_AMP)
		snd_info("hda", "%s: speakers sit behind a separate smart "
			"amplifier (I2C/SPI); the internal speakers need that "
			"amplifier's driver, headphones work directly\n",
			c->chip_name);
	if (ops & ALC_OP_INV_DMIC)
		snd_info("hda", "%s: inverted stereo digital mic; record "
			"it as mono\n",
			c->chip_name);
}

/* Jack behaviour the board needs, decided before the generic init. */
static void alc_recipe_post(struct hda_codec *c, struct alc_state *st)
{
	struct hda_spec *s = &c->spec;
	uint64_t ops = st->ops;
	int i;

	if (ops & ALC_OP_NO_AUTOMUTE)
		s->auto_mute = 0;
	if (ops & ALC_OP_NO_JACK_DETECT) {
		s->auto_mute = 0;
		for (i = 0; i < s->nout && i < HDA_MAX_OUTPUTS; i++)
			s->out[i].jack_detect = 0;
		for (i = 0; i < s->nin && i < HDA_MAX_INPUTS; i++)
			s->in[i].jack_detect = 0;
	}
}

static void alc_vref50_pins(struct hda_codec *c, const uint8_t *nids, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		struct hda_widget *w = alc_pin(c, nids[i]);
		uint8_t ctl;

		if (!w)
			continue;
		ctl = (uint8_t)((w->pinctl & ~AC_PINCTL_VREFEN) |
				AC_PINCTL_VREF_50);
		hda_pin_set_ctl(c, nids[i], ctl);
	}
}

/* Everything a recipe needs after each reset. */
static void alc_recipe_init(struct hda_codec *c, struct alc_state *st)
{
	const struct alc_recipe *r = st->rcp;
	uint64_t ops = st->ops;
	struct hda_widget *w;
	int i;

	if (!r)
		return;
	if (r->verbs)
		hda_apply_verbs(c, r->verbs);

	/* pin controls on top of what the generic init programmed */
	if (ops & (ALC_OP_XPS13 | ALC_OP_MIC_VREF_HIZ | ALC_OP_DELL4_QUIET))
		alc_set_pinctl(c, 0x19, PINCTL_VREFHIZ);
	if (ops & ALC_OP_DELL4_QUIET)
		alc_set_pinctl(c, 0x1b, PINCTL_VREFHIZ);
	if (ops & ALC_OP_ASUS887_JACK)
		alc_set_pinctl(c, 0x1b, PINCTL_HP);
	if (ops & ALC_OP_CLEVO_P775)
		alc_set_pinctl(c, 0x1b, PINCTL_VREF80);
	if (ops & ALC_OP_ALC897_HSMIC)
		alc_set_pinctl(c, 0x1a, PINCTL_VREF100);
	if (ops & ALC_OP_ENVY_X360)
		alc_set_pinctl(c, 0x19, PINCTL_VREF50);

	if (ops & ALC_OP_DOCK_ENABLE) {
		/* ThinkPad docks: writing the top byte of the default config
		 * register is what switches the dock jacks on */
		if (alc_pin(c, 0x17))
			hda_codec_write(c, 0x17,
					AC_VERB_SET_CONFIG_DEFAULT_BYTES_3, 0);
		if (alc_pin(c, 0x19))
			hda_codec_write(c, 0x19,
					AC_VERB_SET_CONFIG_DEFAULT_BYTES_3, 0);
	}
	if (ops & ALC_OP_STEREO_DMIC)
		alc_coef_update(c, 0x07, 0, 0x80);
	if (ops & ALC_OP_HWEQ)
		alc_coef_update(c, 0x1e, 0, 0x80);
	if (ops & ALC_OP_ASPIRE_E1_COEF)
		alc_coef_update(c, 0x0d, 0x6000, 0x6000);
	if (ops & ALC_OP_HADES_CANYON)
		alc_coef_update(c, 0x1b, 0x0400, 0x0400);
	if (ops & ALC_OP_COEF10_BIT9)
		alc_coef_update(c, 0x10, 1 << 9, 0);
	if (ops & ALC_OP_MINISFORUM_JD)
		alc_coef_update(c, 0x4a, 1 << 15, 0);
	if (ops & ALC_OP_DELL_HP_RESUME)
		alc_coef_write(c, 0x0d, 0x2800);
	if (ops & ALC_OP_ALC889_COEF)
		alc_coef_update(c, 7, 0, 0x2030);
	if (ops & ALC_OP_CLEVO_P950)
		alc_coef_update(c, 0x07, 0, 0x3c3);
	if (ops & ALC_OP_GB_X570)
		alc_coef_seq(c, alc_gb_x570_coefs);
	if (ops & ALC_OP_ENVY_X360)
		alc_coef_seq(c, alc_envy_x360_coefs);
	if (ops & ALC_OP_ALC256_COEF_DEFAULTS)
		alc_coef_seq(c, alc256_coef_defaults);
	if (ops & ALC_OP_ALC233_JACK_COEFS)
		alc_coef_seq(c, alc233_jack_coefs);
	if (ops & ALC_OP_ALC671_HSMIC)
		alc_coef_write(c, 0x19, 0xa054);
	if (ops & (ALC_OP_ALC283_SENSE_COMBO | ALC_OP_ALC283_CHROMEBOOK))
		alc_coef_update(c, 0x06, 0x000c, 0); /* MIC2 VREF manual */
	if (ops & ALC_OP_ALC283_CHROMEBOOK)
		alc_coef_update(c, 0x1a, 0, 1 << 4); /* line1 by verb */
	if (ops & ALC_OP_ALC271_DMIC) {
		if (alc_streq(c->chip_name, "ALC271X") ||
		    alc_streq(c->chip_name, "ALC269VB")) {
			if (cfg_port(alc_pin_cfg(c, 0x12)) == AC_JACK_PORT_FIXED)
				alc_coef_write(c, 0x0d, 0x4000);
		}
	}
	if (ops & ALC_OP_JD_RESTART)
		alc_combo_jd_restart(c);
	if (ops & ALC_OP_MBP_VREF) {
		static const uint8_t hp_pins[] = { 0x14, 0x15, 0x19 };

		for (i = 0; i < 3; i++) {
			w = alc_pin(c, hp_pins[i]);
			if (!w || cfg_device(w->pin_cfg) != AC_JACK_HP_OUT)
				continue;
			hda_pin_set_ctl(c, hp_pins[i],
					(uint8_t)((w->pinctl & ~AC_PINCTL_VREFEN) |
						  AC_PINCTL_VREF_80));
			break;
		}
	}
	if (r->vref50[0])
		alc_vref50_pins(c, r->vref50, r->vref50[1] ? 2 : 1);
	if (ops & ALC_OP_AMP_COEF65) {
		alc_msleep(100);
		alc_coef_write(c, 0x65, 0x0);
	}
	if (ops & ALC_OP_GPIO0_PULSE) {
		/* the amplifier latches on a GPIO0 pulse */
		alc_gpio_update(c, 0x01, 1);
		alc_msleep(100);
		alc_gpio_update(c, 0x01, 0);
	}
	if (ops & ALC_OP_GPIO03_PULSE) {
		alc_gpio_update(c, 0x03, 1);
		alc_msleep(100);
		alc_gpio_update(c, 0x03, 0);
	}
	if (ops & ALC_OP_SAMSUNG_AMP)
		alc298_samsung_amp_v1(c);
	if (st->samsung_amps)
		alc298_samsung_v2_init(c);
	if (ops & ALC_OP_AMP_GPIO2_PLAYBACK)
		alc_gpio_update(c, 0x04, st->playing);
}

/* Jack-change actions of particular boards. */
static void alc_recipe_automute(struct hda_codec *c, struct alc_state *st,
				int hp)
{
	uint64_t ops = st->ops;

	if (ops & ALC_OP_QUANTA_MUTE) {
		alc_coef_write(c, 0x0c, 0x680);
		alc_coef_write(c, 0x0c, 0x480);
	}
	if (ops & ALC_OP_HP_AMP_GPIO4)
		alc_gpio_update(c, 0x10, hp);
	if (ops & ALC_OP_DELL_ALC288)
		alc_gpio_update(c, 0x40, hp); /* headset mic power */
	if (ops & ALC_OP_ASUS887_JACK)
		alc_set_pinctl(c, 0x19,
			       PINCTL_HP | (hp ? AC_PINCTL_VREF_80 :
						 AC_PINCTL_VREF_HIZ));
	if (ops & (ALC_OP_ALC897_HSMIC | ALC_OP_ALC897_HSMODE))
		alc_set_pinctl(c, 0x1b, PINCTL_HP |
					    (hp ? AC_PINCTL_VREF_100 : 0));
	if (ops & ALC_OP_USI_HSMIC) {
		alc_msleep(100);
		alc_raw_pinctl(c, 0x19, hp ? PINCTL_VREF80 : 0);
	}
	if (ops & ALC_OP_X101_HSMIC) {
		alc_msleep(100);
		alc_raw_pinctl(c, 0x18, hp ? PINCTL_VREF80 : 0);
		alc_msleep(500);
		alc_raw_pinctl(c, 0x18, hp ? PINCTL_VREF80 : 0);
	}
	if (ops & ALC_OP_ALC283_SENSE_COMBO) {
		alc_msleep(600);
		alc_raw_pinctl(c, 0x19, hp ? PINCTL_VREF80 : 0);
	}
	if (ops & ALC_OP_GX502)
		alc_coef_write(c, 0x10, hp ? 0x8a20 : 0x0a20);
	if (ops & ALC_OP_GU502)
		alc_coef_write(c, 0x10, hp ? 0x8420 : 0x0a20);
}

/* ==========================================================================
 * Vendor driver hooks
 * ========================================================================== */

static int alc_match(struct hda_codec *c)
{
	uint32_t v = c->vendor_id;

	if (!c->afg)
		return 0; /* modem-only function: not ours */
	if ((v >> 16) == 0x10ec)
		return 1;
	return v == 0x19e58326; /* Huawei HW8326 (ALC256 class) */
}

static int alc_pre_parse(struct hda_codec *c)
{
	struct alc_state *st;
	const pci_device_t *pci = c->ctrl ? c->ctrl->pci : NULL;
	uint32_t ss;

	st = kalloc(sizeof(*st));
	if (!st)
		return -ENOMEM;
	mm_memset(st, 0, sizeof(*st));
	c->vendor_data = st;

	st->vendor_id = c->vendor_id;
	st->codec_ssid = c->subsystem_id;
	st->sku_hp_port = -1;
	st->mute_led_state = -1;
	st->hs_mode = ALC_HS_MODE_UNKNOWN;
	st->hs_type = ALC_HS_TYPE_UNKNOWN;
	if (pci) {
		ss = pci_cfg_read32_dev(pci, 0x2c); /* subsystem ids */
		if (ss != 0xffffffffu && ss != 0) {
			st->pci_ssvid = (uint16_t)(ss & 0xffff);
			st->pci_ssdid = (uint16_t)(ss >> 16);
			st->pci_ssid = ((uint32_t)st->pci_ssvid << 16) |
				       st->pci_ssdid;
		}
		st->ctrl_vendor = pci->vendor_id;
	}

	st->chip = alc_find_chip(st->vendor_id, c->revision_id);
	if (st->chip) {
		st->family = st->chip->family;
		st->type = st->chip->type;
	} else {
		alc_guess_family(st->vendor_id, &st->family, &st->type);
	}

	alc_coef0(c);
	alc_parse_sku(c, st);
	alc_name_chip(c, st);
	alc_chip_setup(c, st);
	alc_fill_eapd_coef(c);
	st->has_dsp5505 = alc_detect_dsp5505(c, st);

	alc_pick_recipe(c, st);
	alc_recipe_pre(c, st);
	alc_apply_sku(c, st);
	alc_fill_missing_pins(c, st);

	snd_info("hda", "codec %s (%s %08x rev %06x coef0 %04x) "
		"ssid %08x pci-ssid %08x%s%s%s%s%s\n",
		c->chip_name, c->vendor_name, st->vendor_id,
		c->revision_id & 0xffffff, st->coef0, st->codec_ssid,
		st->pci_ssid, st->rcp ? ", fixup " : "",
		st->rcp ? st->rcp->name : "", st->rcp ? " for " : "",
		st->board ? st->board : "",
		st->has_dsp5505 ? ", ALC5505 DSP" : "");
	if (st->rcp && st->match_kind)
		snd_info("hda", "%s: board matched by %s\n", c->chip_name,
			st->match_kind);
	return 0;
}

static int alc_post_parse(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (!st)
		return 0;
	alc_find_pins(c, st);
	st->hs_capable = (st->ops & ALC_OP_HS_MODE) &&
			 alc_hs_class(st->vendor_id) != ALC_HSC_NONE &&
			 st->hp_pin;
	alc_route_outputs(c, st);
	alc_silence_aamix(c, st);
	alc_recipe_post(c, st);
	alc_setup_eapd(c, 1);
	alc_write_gpio(c);

	snd_info("hda", "%s: hp 0x%02x hp2 0x%02x speakers %d (0x%02x) "
		"headset-mic 0x%02x int-mic 0x%02x%s\n",
		c->chip_name, st->hp_pin, st->hp2_pin, st->nspk,
		st->nspk ? st->spk_pin[0] : 0, st->hs_mic_pin, st->int_mic_pin,
		st->hs_capable ? " (headset mode)" : "");
	return 0;
}

static int alc_init(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);
	int hp;

	if (!st)
		return 0;
	st->init_count++;
	/* COEFs and the headset state are gone after a reset */
	alc_fill_eapd_coef(c);
	alc_family_init(c);
	alc_fix_pll(c);
	alc_auto_init_amp(c);
	alc_recipe_init(c, st);

	hp = st->hp_pin ? alc_jack_present(c, st->hp_pin) : 0;
	st->hp_present = hp;
	st->hs_mode = ALC_HS_MODE_UNKNOWN;
	st->hs_type = ALC_HS_TYPE_UNKNOWN;
	alc_hs_update(c, hp);
	if (hp && (st->ops & ALC_OP_HS_REDETECT) && st->hs_capable &&
	    st->init_count == 1) {
		/* a plug present at cold boot can be sensed wrongly while
		 * the analog side is still settling: measure again */
		alc_msleep(500);
		st->hs_mode = ALC_HS_MODE_UNKNOWN;
		st->hs_type = ALC_HS_TYPE_UNKNOWN;
		alc_hs_update(c, hp);
	}
	alc_recipe_automute(c, st, hp);
	/* the reset and depop sequences above wrote pins and amps by hand:
	 * let the router put back the selected outputs, levels and mutes */
	hda_codec_update_routing(c);
	alc_sync_leds(c, 1);
	return 0;
}

static void alc_automute(struct hda_codec *c, int hp_present)
{
	struct alc_state *st = alc_st(c);

	if (!st)
		return;
	hp_present = hp_present ? 1 : 0;
	if (!st->init_count) {
		/* the generic init routes outputs before our first init:
		 * the headset COEFs are set up there, after the chip's own
		 * reset sequence */
		st->hp_present = hp_present;
		alc_sync_leds(c, 0);
		return;
	}
	alc_hs_update(c, hp_present);
	if (hp_present != st->hp_present)
		alc_recipe_automute(c, st, hp_present);
	st->hp_present = hp_present;
	alc_sync_leds(c, 0);
}

static void alc_unsol(struct hda_codec *c, uint32_t res)
{
	struct alc_state *st = alc_st(c);
	int hp;

	(void)res;
	if (!st)
		return;
	/* events the generic code does not route (GPIO, headset buttons,
	 * the combo-jack state machine): recheck the jack ourselves */
	if (st->hp_pin) {
		hp = alc_jack_present(c, st->hp_pin);
		if (hp != st->hp_present)
			alc_automute(c, hp);
	}
	alc_sync_leds(c, 0);
}

static void alc_playback_hook(struct hda_codec *c, int start)
{
	struct alc_state *st = alc_st(c);

	if (!st)
		return;
	st->playing = start ? 1 : 0;
	if (st->samsung_amps && st->samsung_amps_on != st->playing)
		alc298_samsung_v2_power(c, st->playing);
	if (st->ops & ALC_OP_AMP_GPIO2_PLAYBACK)
		alc_gpio_update(c, 0x04, st->playing);
	if (start) {
		/* a codec power cycle may have dropped the amplifier enable */
		alc_setup_eapd(c, 1);
		if (st->type == ALC_T_269VB)
			alc_coef_update(c, 0x04, 0, 1 << 11);
	}
	alc_sync_leds(c, 0);
}

static void alc_remove(struct hda_codec *c)
{
	struct alc_state *st = alc_st(c);

	if (!st)
		return;
	if (st->samsung_amps)
		alc298_samsung_v2_power(c, 0);
	alc_family_shutup(c);
	c->vendor_data = NULL;
	kfree(st);
}

const struct hda_vendor_driver hda_realtek_driver = {
	.name = "realtek",
	.match = alc_match,
	.pre_parse = alc_pre_parse,
	.post_parse = alc_post_parse,
	.init = alc_init,
	.automute = alc_automute,
	.unsol = alc_unsol,
	.playback_hook = alc_playback_hook,
	.probe = NULL,
	.remove = alc_remove,
};
