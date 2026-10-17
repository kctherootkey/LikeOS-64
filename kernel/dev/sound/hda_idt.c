// LikeOS HD Audio - SigmaTel / IDT / Tempo codecs.
//
// SigmaTel (vendor 0x8384) built the STAC9200, STAC922x, STAC925x,
// STAC927x, STAC9205 families; after IDT bought them the parts became the
// 92HD7x/8x/9x line under vendor id 0x111d (later sold by Tempo).  They are
// all parsed generically from the BIOS pin configuration.  This file adds:
//
//  * per-family core verbs: volume knob in direct mode, analog PC beep,
//    DAC selector defaults, "power state controls the amplifiers",
//  * the GPIO that powers the speaker amplifier (EAPD on GPIO0 for most,
//    GPIO2 on the Dell 927x laptops, GPIO4 on some Toshiba) - switched off
//    while headphones play when the board allows it,
//  * the port power map of the 92HD7x/8x/9x (verb 0x7ec): unconnected
//    ports are powered down, every used one stays up,
//  * Dell and HP board fixups (pin tables for the Inspiron/Latitude/
//    Precision/Vostro/Studio/XPS, Alienware, HP Pavilion dv/HDX/Mini,
//    EliteBook/ProBook docks, Envy bass speakers), Intel desktop boards,
//    Gateway, Panasonic, Sony VAIO, ECS and the Intel Macs (codec
//    subsystem ids, GPIO 0/1 for the speaker and headphone amplifiers),
//  * HP mute and mic-mute LEDs on GPIOs, with either polarity,
//  * VirtualBox's emulated STAC9221 is detected and handed to the
//    emulated-codec driver (single line-out path on DAC1, which is where
//    the hypervisor takes its volume from).
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>
#include <kernel/hal/lapic.h>

/* vendor verbs */
#define IDT_VERB_SET_POWER_MAP 0x7ec
#define IDT_VERB_GET_POWER_MAP 0xfec
#define IDT_VERB_GPIO_CMOS 0x7e7

/* ---- families ----------------------------------------------------------------- */

enum idt_family {
	IDT_9200 = 0,
	IDT_922X,
	IDT_925X,
	IDT_927X,
	IDT_9205,
	IDT_9872,
	IDT_92HD73,
	IDT_92HD83,
	IDT_92HD71,
	IDT_92HD95,
	IDT_FAMILIES,
};

#define FB(f) (1u << (f))
#define FALL 0u

struct idt_chip {
	uint32_t id;
	const char *name;
	uint8_t family;
};

static const struct idt_chip idt_chips[] = {
	{ 0x83847690, "STAC9200", IDT_9200 },
	{ 0x83847882, "STAC9220 A1", IDT_922X },
	{ 0x83847680, "STAC9221 A1", IDT_922X },
	{ 0x83847880, "STAC9220 A2", IDT_922X },
	{ 0x83847681, "STAC9220D/9223D A2", IDT_922X },
	{ 0x83847682, "STAC9221 A2", IDT_922X },
	{ 0x83847683, "STAC9221D A2", IDT_922X },
	{ 0x83847618, "STAC9227", IDT_927X },
	{ 0x83847619, "STAC9227", IDT_927X },
	{ 0x83847638, "STAC92HD700", IDT_927X },
	{ 0x83847616, "STAC9228", IDT_927X },
	{ 0x83847617, "STAC9228", IDT_927X },
	{ 0x83847614, "STAC9229", IDT_927X },
	{ 0x83847615, "STAC9229", IDT_927X },
	{ 0x83847620, "STAC9274", IDT_927X },
	{ 0x83847621, "STAC9274D", IDT_927X },
	{ 0x83847622, "STAC9273X", IDT_927X },
	{ 0x83847623, "STAC9273D", IDT_927X },
	{ 0x83847624, "STAC9272X", IDT_927X },
	{ 0x83847625, "STAC9272D", IDT_927X },
	{ 0x83847626, "STAC9271X", IDT_927X },
	{ 0x83847627, "STAC9271D", IDT_927X },
	{ 0x83847628, "STAC9274X5NH", IDT_927X },
	{ 0x83847629, "STAC9274D5NH", IDT_927X },
	{ 0x83847632, "STAC9202", IDT_925X },
	{ 0x83847633, "STAC9202D", IDT_925X },
	{ 0x83847634, "STAC9250", IDT_925X },
	{ 0x83847635, "STAC9250D", IDT_925X },
	{ 0x83847636, "STAC9251", IDT_925X },
	{ 0x83847637, "STAC9250D", IDT_925X },
	{ 0x83847645, "92HD206X", IDT_927X },
	{ 0x83847646, "92HD206D", IDT_927X },
	{ 0x83847661, "CXD9872RD/K", IDT_9872 },
	{ 0x83847662, "STAC9872AK", IDT_9872 },
	{ 0x83847664, "CXD9872AKD", IDT_9872 },
	{ 0x83847698, "STAC9205", IDT_9205 },
	{ 0x838476a0, "STAC9205", IDT_9205 },
	{ 0x838476a1, "STAC9205D", IDT_9205 },
	{ 0x838476a2, "STAC9204", IDT_9205 },
	{ 0x838476a3, "STAC9204D", IDT_9205 },
	{ 0x838476a4, "STAC9255", IDT_9205 },
	{ 0x838476a5, "STAC9255D", IDT_9205 },
	{ 0x838476a6, "STAC9254", IDT_9205 },
	{ 0x838476a7, "STAC9254D", IDT_9205 },
	{ 0x111d7603, "92HD75B3X5", IDT_92HD71 },
	{ 0x111d7604, "92HD83C1X5", IDT_92HD83 },
	{ 0x111d76d4, "92HD83C1C5", IDT_92HD83 },
	{ 0x111d7605, "92HD81B1X5", IDT_92HD83 },
	{ 0x111d76d5, "92HD81B1C5", IDT_92HD83 },
	{ 0x111d76d1, "92HD87B1/3", IDT_92HD83 },
	{ 0x111d76d9, "92HD87B2/4", IDT_92HD83 },
	{ 0x111d7666, "92HD88B3", IDT_92HD83 },
	{ 0x111d7667, "92HD88B1", IDT_92HD83 },
	{ 0x111d7668, "92HD88B2", IDT_92HD83 },
	{ 0x111d7669, "92HD88B4", IDT_92HD83 },
	{ 0x111d7608, "92HD75B2X5", IDT_92HD71 },
	{ 0x111d7674, "92HD73D1X5", IDT_92HD73 },
	{ 0x111d7675, "92HD73C1X5", IDT_92HD73 },
	{ 0x111d7676, "92HD73E1X5", IDT_92HD73 },
	{ 0x111d7695, "92HD95", IDT_92HD95 },
	{ 0x111d76b0, "92HD71B8X", IDT_92HD71 },
	{ 0x111d76b1, "92HD71B8X", IDT_92HD71 },
	{ 0x111d76b2, "92HD71B7X", IDT_92HD71 },
	{ 0x111d76b3, "92HD71B7X", IDT_92HD71 },
	{ 0x111d76b4, "92HD71B6X", IDT_92HD71 },
	{ 0x111d76b5, "92HD71B6X", IDT_92HD71 },
	{ 0x111d76b6, "92HD71B5X", IDT_92HD71 },
	{ 0x111d76b7, "92HD71B5X", IDT_92HD71 },
	{ 0x111d76c0, "92HD89C3", IDT_92HD73 },
	{ 0x111d76c1, "92HD89C2", IDT_92HD73 },
	{ 0x111d76c2, "92HD89C1", IDT_92HD73 },
	{ 0x111d76c3, "92HD89B3", IDT_92HD73 },
	{ 0x111d76c4, "92HD89B2", IDT_92HD73 },
	{ 0x111d76c5, "92HD89B1", IDT_92HD73 },
	{ 0x111d76c6, "92HD89E3", IDT_92HD73 },
	{ 0x111d76c7, "92HD89E2", IDT_92HD73 },
	{ 0x111d76c8, "92HD89E1", IDT_92HD73 },
	{ 0x111d76c9, "92HD89D3", IDT_92HD73 },
	{ 0x111d76ca, "92HD89D2", IDT_92HD73 },
	{ 0x111d76cb, "92HD89D1", IDT_92HD73 },
	{ 0x111d76cc, "92HD89F3", IDT_92HD73 },
	{ 0x111d76cd, "92HD89F2", IDT_92HD73 },
	{ 0x111d76ce, "92HD89F1", IDT_92HD73 },
	{ 0x111d76df, "92HD93BXX", IDT_92HD83 },
	{ 0x111d76e0, "92HD91BXX", IDT_92HD83 },
	{ 0x111d76e3, "92HD98BXX", IDT_92HD83 },
	{ 0x111d76e5, "92HD99BXX", IDT_92HD83 },
	{ 0x111d76e7, "92HD90BXX", IDT_92HD83 },
	{ 0x111d76e8, "92HD66B1X5", IDT_92HD83 },
	{ 0x111d76e9, "92HD66B2X5", IDT_92HD83 },
	{ 0x111d76ea, "92HD66B3X5", IDT_92HD83 },
	{ 0x111d76eb, "92HD66C1X5", IDT_92HD83 },
	{ 0x111d76ec, "92HD66C2X5", IDT_92HD83 },
	{ 0x111d76ed, "92HD66C3X5", IDT_92HD83 },
	{ 0x111d76ee, "92HD66B1X3", IDT_92HD83 },
	{ 0x111d76ef, "92HD66B2X3", IDT_92HD83 },
	{ 0x111d76f0, "92HD66B3X3", IDT_92HD83 },
	{ 0x111d76f1, "92HD66C1X3", IDT_92HD83 },
	{ 0x111d76f2, "92HD66C2X3", IDT_92HD83 },
	{ 0x111d76f3, "92HD66C3/65", IDT_92HD83 },
	{ 0, 0, 0 }
};

static const struct idt_chip *idt_find_chip(uint32_t id)
{
	int i;

	for (i = 0; idt_chips[i].id; i++)
		if (idt_chips[i].id == id)
			return &idt_chips[i];
	return 0;
}

/* Unknown parts: guess the family from the id range. */
static int idt_family(const struct hda_codec *c)
{
	const struct idt_chip *chip = idt_find_chip(c->vendor_id);
	uint16_t dev = (uint16_t)(c->vendor_id & 0xffff);

	if (chip)
		return chip->family;
	if ((c->vendor_id >> 16) == 0x111d) {
		if (dev >= 0x76b0 && dev <= 0x76bf)
			return IDT_92HD71;
		if (dev >= 0x76c0 && dev <= 0x76cf)
			return IDT_92HD73;
		return IDT_92HD83;
	}
	if (dev >= 0x76a0 && dev <= 0x76af)
		return IDT_9205;
	if (dev >= 0x7610 && dev <= 0x762f)
		return IDT_927X;
	return IDT_922X;
}

/* ---- per-family behaviour ---------------------------------------------------- */

static const struct hda_verb idt_9200_core[] = {
	{ 0x07, AC_VERB_SET_CONNECT_SEL, 0x00 }, /* DAC0 mux on the DAC */
	{ 0x08, AC_VERB_SET_EAPD_BTLENABLE, 0x02 },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_922x_core[] = {
	/* master volume knob: full scale, direct control */
	{ 0x16, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xff },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_925x_core[] = {
	{ 0x06, AC_VERB_SET_CONNECT_SEL, 0x00 }, /* DAC0 mux on the DAC */
	{ 0, 0, 0 }
};

static const struct hda_verb idt_927x_core[] = {
	{ 0x24, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xff },
	/* analog PC beep path on */
	{ 0x01, AC_VERB_SET_DIGI_CONVERT_2, 1 << 5 },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_927x_volknob_core[] = {
	/* knob without the delta bit */
	{ 0x24, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0x7f },
	{ 0x01, AC_VERB_SET_DIGI_CONVERT_2, 1 << 5 },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_9872_core[] = {
	{ 0x15, AC_VERB_SET_CONNECT_SEL, 0x1 }, /* mic select */
	{ 0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000 }, /* unmute it */
	{ 0, 0, 0 }
};

static const struct hda_verb idt_92hd73_core[] = {
	{ 0x1f, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xff },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_92hd73_dell_eq_core[] = {
	/* the knob at the highest setting that does not distort */
	{ 0x1f, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xec },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_92hd83_core[] = {
	/* let the power state control the amplifiers */
	{ 0x01, AC_VERB_SET_EAPD, 1 << 2 },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_92hd71_volknob[] = {
	{ 0x28, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0xff },
	{ 0, 0, 0 }
};

static const struct hda_verb idt_dell_3st_core[] = {
	{ 0x24, AC_VERB_SET_VOLUME_KNOB_CONTROL, 0x7f },
	{ 0x1b, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000 }, /* unmute 0x1b */
	{ 0x0b, AC_VERB_SET_CONNECT_SEL, 0x01 }, /* DAC 0x03 */
	{ 0, 0, 0 }
};

static const struct hda_verb idt_d965_core[] = {
	{ 0x1b, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000 },
	{ 0x0b, AC_VERB_SET_CONNECT_SEL, 0x01 },
	{ 0, 0, 0 }
};

static const uint8_t idt_92hd73_pwr[] = { 0x0a, 0x0b, 0x0c, 0x0d,
					  0x0e, 0x0f, 0x10, 0x11, 0 };
static const uint8_t idt_92hd83_pwr[] = { 0x0a, 0x0b, 0x0c, 0x0d,
					  0x0e, 0x0f, 0x10, 0 };
static const uint8_t idt_92hd71_pwr[] = { 0x0a, 0x0d, 0x0f, 0 };
static const uint8_t idt_92hd95_pwr[] = { 0x0a, 0x0b, 0x0c, 0x0d, 0 };

struct idt_family_info {
	const char *name;
	const struct hda_verb *core;
	const uint8_t *pwr_nids; /* 0-terminated, power map order */
	uint8_t eapd_gpio; /* GPIO bits powering the speaker amplifier */
	uint8_t eapd_switch; /* switch them with the speaker */
	uint8_t beep_nid; /* digital beep widget */
	uint8_t mixer_nid; /* analog loopback mixer */
};

static const struct idt_family_info idt_families[IDT_FAMILIES] = {
	[IDT_9200] = { "STAC9200", idt_9200_core, 0, 0, 0, 0, 0 },
	[IDT_922X] = { "STAC922x", idt_922x_core, 0, 0, 0, 0, 0 },
	[IDT_925X] = { "STAC925x", idt_925x_core, 0, 0, 0, 0, 0 },
	[IDT_927X] = { "STAC927x", idt_927x_core, 0, 0x01, 1, 0x23, 0 },
	[IDT_9205] = { "STAC9205", idt_927x_core, 0, 0x01, 1, 0x23, 0 },
	[IDT_9872] = { "STAC9872", idt_9872_core, 0, 0, 0, 0, 0 },
	[IDT_92HD73] = { "92HD73xx", idt_92hd73_core, idt_92hd73_pwr, 0x01, 1,
			 0x1c, 0x1d },
	[IDT_92HD83] = { "92HD83xx", idt_92hd83_core, idt_92hd83_pwr, 0, 0,
			 0x21, 0x1b },
	[IDT_92HD71] = { "92HD71xx", 0, idt_92hd71_pwr, 0x01, 0, 0x26, 0x17 },
	[IDT_92HD95] = { "92HD95", 0, idt_92hd95_pwr, 0, 0, 0x19, 0 },
};

/* ---- board models ---------------------------------------------------------- */

#define IDTQ_GPIO (1u << 0) /* model GPIO setup replaces the family's */
#define IDTQ_NO_EAPD_SWITCH (1u << 1) /* leave the EAPD GPIO on */
#define IDTQ_NO_JACK_DETECT (1u << 2) /* presence detect is broken */
#define IDTQ_NO_AUTOMUTE (1u << 3) /* hardware switches by itself */
#define IDTQ_DELL_EQ (1u << 4) /* knob at the no-distortion maximum */
#define IDTQ_LED_AUTO (1u << 5) /* mute LED on GPIO3, or GPIO0 */
#define IDTQ_INTEL_MAC (1u << 6) /* resolve by the codec subsystem id */
#define IDTQ_HP_DUAL_HP (1u << 7) /* dock + built-in headphones */
#define IDTQ_HP_BASS (1u << 8) /* bass speaker on GPIO5 */
#define IDTQ_HP_BNB_DOCK (1u << 9) /* HP business notebooks with dock */
#define IDTQ_HEADSET_JACK (1u << 10) /* combo headset jack */
#define IDTQ_HP_M4 (1u << 11) /* HP Mini 1000: GPIO1 vref detect */
#define IDTQ_VOLKNOB (1u << 12) /* knob without delta bit */
#define IDTQ_DELL_DMIC (1u << 13) /* GPIO2 EAPD + 3-stack core verbs */
#define IDTQ_D965 (1u << 14) /* Intel D965 core verbs */
#define IDTQ_REF (1u << 15) /* vendor evaluation board: no GPIO at all */
#define IDTQ_ZEPHYR (1u << 16) /* HP Zephyr analog tweaks */

struct idt_model {
	uint16_t families; /* FB() mask, 0 = all */
	uint8_t by_codec; /* match the codec subsystem id only */
	uint32_t subsys;
	uint32_t mask; /* 0 = exact */
	const char *name;
	const struct hda_pincfg *pins;
	const struct hda_verb *verbs;
	uint32_t quirks;
	uint8_t gpio_mask, gpio_dir, gpio_data, eapd_mask; /* IDTQ_GPIO */
	uint8_t led_gpio; /* mute LED */
	uint8_t led_polarity; /* 1: GPIO high while NOT muted */
	uint8_t micled_gpio; /* mic-mute LED */
};

#define SSID(v, d) (((uint32_t)(v) << 16) | (uint32_t)(d))
#define VONLY 0xffff0000u

/* -- STAC9200 -------------------------------------------------------------- */
static const struct hda_pincfg idt_ref9200_pins[] = {
	{ 0x08, 0x01c47010 }, { 0x09, 0x01447010 }, { 0x0d, 0x0221401f },
	{ 0x0e, 0x01114010 }, { 0x0f, 0x02a19020 }, { 0x10, 0x01a19021 },
	{ 0x11, 0x90100140 }, { 0x12, 0x01813122 }, { 0, 0 }
};
static const struct hda_pincfg idt_gateway9200_m4_pins[] = {
	{ 0x08, 0x400000fe }, { 0x09, 0x404500f4 }, { 0x0d, 0x400100f0 },
	{ 0x0e, 0x90110010 }, { 0x0f, 0x400100f1 }, { 0x10, 0x02a1902e },
	{ 0x11, 0x500000f2 }, { 0x12, 0x500000f3 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_d21_pins[] = {
	{ 0x08, 0x400001f0 }, { 0x09, 0x400001f1 }, { 0x0d, 0x02214030 },
	{ 0x0e, 0x01014010 }, { 0x0f, 0x02a19020 }, { 0x10, 0x01a19021 },
	{ 0x11, 0x90100140 }, { 0x12, 0x01813122 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_d22_pins[] = {
	{ 0x08, 0x400001f0 }, { 0x09, 0x400001f1 }, { 0x0d, 0x0221401f },
	{ 0x0e, 0x01014010 }, { 0x0f, 0x01813020 }, { 0x10, 0x02a19021 },
	{ 0x11, 0x90100140 }, { 0x12, 0x400001f2 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_d23_pins[] = {
	{ 0x08, 0x400001f0 }, { 0x09, 0x400001f1 }, { 0x0d, 0x0221401f },
	{ 0x0e, 0x01014010 }, { 0x0f, 0x01813020 }, { 0x10, 0x01a19021 },
	{ 0x11, 0x90100140 }, { 0x12, 0x400001f2 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m21_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x03441340 }, { 0x0d, 0x0321121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x408003fb }, { 0x10, 0x03a11020 },
	{ 0x11, 0x401003fc }, { 0x12, 0x403003fd }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m22_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x0144131f }, { 0x0d, 0x0321121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x90a70321 }, { 0x10, 0x03a11020 },
	{ 0x11, 0x401003fb }, { 0x12, 0x40f000fc }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m23_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x01441340 }, { 0x0d, 0x0421421f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x408003fb }, { 0x10, 0x04a1102e },
	{ 0x11, 0x90170311 }, { 0x12, 0x403003fc }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m24_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x404003fb }, { 0x0d, 0x0321121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x408003fc }, { 0x10, 0x03a11020 },
	{ 0x11, 0x401003fd }, { 0x12, 0x403003fe }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m25_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x01441340 }, { 0x0d, 0x0421121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x408003fb }, { 0x10, 0x04a11020 },
	{ 0x11, 0x401003fc }, { 0x12, 0x403003fd }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m26_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x404003fb }, { 0x0d, 0x0421121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x408003fc }, { 0x10, 0x04a11020 },
	{ 0x11, 0x401003fd }, { 0x12, 0x403003fe }, { 0, 0 }
};
static const struct hda_pincfg idt_dell9200_m27_pins[] = {
	{ 0x08, 0x40c003fa }, { 0x09, 0x01441340 }, { 0x0d, 0x0421121f },
	{ 0x0e, 0x90170310 }, { 0x0f, 0x90170310 }, { 0x10, 0x04a11020 },
	{ 0x11, 0x90170310 }, { 0x12, 0x40f003fc }, { 0, 0 }
};
static const struct hda_pincfg idt_oqo9200_pins[] = {
	{ 0x08, 0x40c000f0 }, { 0x09, 0x404000f1 }, { 0x0d, 0x0221121f },
	{ 0x0e, 0x02211210 }, { 0x0f, 0x90170111 }, { 0x10, 0x90a70120 },
	{ 0x11, 0x400000f2 }, { 0x12, 0x400000f3 }, { 0, 0 }
};
static const struct hda_verb idt_9200_eapd_verbs[] = {
	{ 0x08, AC_VERB_SET_EAPD_BTLENABLE, 0x02 },
	{ 0, 0, 0 }
};

/* -- STAC925x ------------------------------------------------------------------- */
static const struct hda_pincfg idt_ref925x_pins[] = {
	{ 0x07, 0x40c003f0 }, { 0x08, 0x424503f2 }, { 0x0a, 0x01813022 },
	{ 0x0b, 0x02a19021 }, { 0x0c, 0x90a70320 }, { 0x0d, 0x02214210 },
	{ 0x10, 0x01019020 }, { 0x11, 0x9033032e }, { 0, 0 }
};
/* the Gateway M1/M2/M5 layouts are identical */
static const struct hda_pincfg idt_925x_m1_pins[] = {
	{ 0x07, 0x40c003f4 }, { 0x08, 0x424503f2 }, { 0x0a, 0x400000f3 },
	{ 0x0b, 0x02a19020 }, { 0x0c, 0x40a000f0 }, { 0x0d, 0x90100210 },
	{ 0x10, 0x400003f1 }, { 0x11, 0x9033032e }, { 0, 0 }
};
static const struct hda_pincfg idt_925x_m3_pins[] = {
	{ 0x07, 0x40c003f4 }, { 0x08, 0x424503f2 }, { 0x0a, 0x400000f3 },
	{ 0x0b, 0x02a19020 }, { 0x0c, 0x40a000f0 }, { 0x0d, 0x90100210 },
	{ 0x10, 0x400003f1 }, { 0x11, 0x503303f3 }, { 0, 0 }
};
static const struct hda_pincfg idt_925x_m6_pins[] = {
	{ 0x07, 0x40c003f4 }, { 0x08, 0x424503f2 }, { 0x0a, 0x400000f3 },
	{ 0x0b, 0x02a19020 }, { 0x0c, 0x40a000f0 }, { 0x0d, 0x90100210 },
	{ 0x10, 0x400003f1 }, { 0x11, 0x90330320 }, { 0, 0 }
};

/* -- STAC922x ------------------------------------------------------------------- */
static const struct hda_pincfg idt_d945_ref_pins[] = {
	{ 0x0a, 0x01014010 }, { 0x0b, 0x01016011 }, { 0x0c, 0x01012012 },
	{ 0x0d, 0x0221401f }, { 0x0e, 0x01813122 }, { 0x0f, 0x01011014 },
	{ 0x10, 0x01441030 }, { 0x11, 0x01c41030 }, { 0x15, 0x40000100 },
	{ 0x1b, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_d945gtp3_pins[] = {
	{ 0x0a, 0x0221401f }, { 0x0b, 0x01a19022 }, { 0x0c, 0x01813021 },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x40000100 }, { 0x0f, 0x40000100 },
	{ 0x10, 0x40000100 }, { 0x11, 0x40000100 }, { 0x15, 0x02a19120 },
	{ 0x1b, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_d945gtp5_pins[] = {
	{ 0x0a, 0x0221401f }, { 0x0b, 0x01011012 }, { 0x0c, 0x01813024 },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x01a19021 }, { 0x0f, 0x01016011 },
	{ 0x10, 0x01452130 }, { 0x11, 0x40000100 }, { 0x15, 0x02a19320 },
	{ 0x1b, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell922x_d81_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x01a19021 }, { 0x0c, 0x01111012 },
	{ 0x0d, 0x01114010 }, { 0x0e, 0x02a19020 }, { 0x0f, 0x01117011 },
	{ 0x10, 0x400001f0 }, { 0x11, 0x400001f1 }, { 0x15, 0x01813122 },
	{ 0x1b, 0x400001f2 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell922x_d82_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x01a19021 }, { 0x0c, 0x01111012 },
	{ 0x0d, 0x01114010 }, { 0x0e, 0x02a19020 }, { 0x0f, 0x01117011 },
	{ 0x10, 0x01451140 }, { 0x11, 0x400001f0 }, { 0x15, 0x01813122 },
	{ 0x1b, 0x400001f1 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell922x_m81_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x01112024 }, { 0x0c, 0x01111222 },
	{ 0x0d, 0x91174220 }, { 0x0e, 0x03a11050 }, { 0x0f, 0x01116221 },
	{ 0x10, 0x90a70330 }, { 0x11, 0x01452340 }, { 0x15, 0x40c003f1 },
	{ 0x1b, 0x405003f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell922x_m82_pins[] = {
	{ 0x0a, 0x02211211 }, { 0x0b, 0x408103ff }, { 0x0c, 0x02a1123e },
	{ 0x0d, 0x90100310 }, { 0x0e, 0x408003f1 }, { 0x0f, 0x0221121f },
	{ 0x10, 0x03451340 }, { 0x11, 0x40c003f2 }, { 0x15, 0x508003f3 },
	{ 0x1b, 0x405003f4 }, { 0, 0 }
};
static const struct hda_pincfg idt_mac_v1_pins[] = {
	{ 0x0a, 0x0121e21f }, { 0x0b, 0x400000ff }, { 0x0c, 0x9017e110 },
	{ 0x0d, 0x400000fd }, { 0x0e, 0x400000fe }, { 0x0f, 0x0181e020 },
	{ 0x10, 0x1145e030 }, { 0x11, 0x11c5e240 }, { 0x15, 0x400000fc },
	{ 0x1b, 0x400000fb }, { 0, 0 }
};
static const struct hda_pincfg idt_mac_v2_pins[] = {
	{ 0x0a, 0x0121e21f }, { 0x0b, 0x90a7012e }, { 0x0c, 0x9017e110 },
	{ 0x0d, 0x400000fd }, { 0x0e, 0x400000fe }, { 0x0f, 0x0181e020 },
	{ 0x10, 0x1145e230 }, { 0x11, 0x500000fa }, { 0x15, 0x400000fc },
	{ 0x1b, 0x400000fb }, { 0, 0 }
};
static const struct hda_pincfg idt_mac_v3_pins[] = {
	{ 0x0a, 0x0121e21f }, { 0x0b, 0x90a7012e }, { 0x0c, 0x9017e110 },
	{ 0x0d, 0x400000fd }, { 0x0e, 0x400000fe }, { 0x0f, 0x0181e020 },
	{ 0x10, 0x1145e230 }, { 0x11, 0x11c5e240 }, { 0x15, 0x400000fc },
	{ 0x1b, 0x400000fb }, { 0, 0 }
};
/* v4 and v5 carry the same table */
static const struct hda_pincfg idt_mac_v4_pins[] = {
	{ 0x0a, 0x0321e21f }, { 0x0b, 0x03a1e02e }, { 0x0c, 0x9017e110 },
	{ 0x0d, 0x9017e11f }, { 0x0e, 0x400000fe }, { 0x0f, 0x0381e020 },
	{ 0x10, 0x1345e230 }, { 0x11, 0x13c5e240 }, { 0x15, 0x400000fc },
	{ 0x1b, 0x400000fb }, { 0, 0 }
};
static const struct hda_pincfg idt_ecs202_pins[] = {
	{ 0x0a, 0x0221401f }, { 0x0b, 0x02a19020 }, { 0x0c, 0x01a19020 },
	{ 0x0d, 0x01114010 }, { 0x0e, 0x408000f0 }, { 0x0f, 0x01813022 },
	{ 0x10, 0x074510a0 }, { 0x11, 0x40c400f1 }, { 0x15, 0x9037012e },
	{ 0x1b, 0x40e000f2 }, { 0, 0 }
};

/* -- STAC927x ------------------------------------------------------------------- */
static const struct hda_pincfg idt_ref927x_pins[] = {
	{ 0x0a, 0x02214020 }, { 0x0b, 0x02a19080 }, { 0x0c, 0x0181304e },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x01a19040 }, { 0x0f, 0x01011012 },
	{ 0x10, 0x01016011 }, { 0x11, 0x0101201f }, { 0x12, 0x183301f0 },
	{ 0x13, 0x18a001f0 }, { 0x14, 0x18a001f0 }, { 0x21, 0x01442070 },
	{ 0x22, 0x01c42190 }, { 0x23, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_d965_3st_pins[] = {
	{ 0x0a, 0x0221401f }, { 0x0b, 0x02a19120 }, { 0x0c, 0x40000100 },
	{ 0x0d, 0x01014011 }, { 0x0e, 0x01a19021 }, { 0x0f, 0x01813024 },
	{ 0x10, 0x40000100 }, { 0x11, 0x40000100 }, { 0x12, 0x40000100 },
	{ 0x13, 0x40000100 }, { 0x14, 0x40000100 }, { 0x21, 0x40000100 },
	{ 0x22, 0x40000100 }, { 0x23, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_d965_5st_pins[] = {
	{ 0x0a, 0x02214020 }, { 0x0b, 0x02a19080 }, { 0x0c, 0x0181304e },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x01a19040 }, { 0x0f, 0x01011012 },
	{ 0x10, 0x01016011 }, { 0x11, 0x40000100 }, { 0x12, 0x40000100 },
	{ 0x13, 0x40000100 }, { 0x14, 0x40000100 }, { 0x21, 0x01442070 },
	{ 0x22, 0x40000100 }, { 0x23, 0x40000100 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_3st_pins[] = {
	{ 0x0a, 0x02211230 }, { 0x0b, 0x02a11220 }, { 0x0c, 0x01a19040 },
	{ 0x0d, 0x01114210 }, { 0x0e, 0x01111212 }, { 0x0f, 0x01116211 },
	{ 0x10, 0x01813050 }, { 0x11, 0x01112214 }, { 0x12, 0x403003fa },
	{ 0x13, 0x90a60040 }, { 0x14, 0x90a60040 }, { 0x21, 0x404003fb },
	{ 0x22, 0x40c003fc }, { 0x23, 0x40000100 }, { 0, 0 }
};
/* Dell laptops with the verb table in the BIOS: fix the front jacks */
static const struct hda_pincfg idt_dell_bios_pins[] = {
	{ 0x0f, 0x0221101f }, /* front output jack: headphone */
	{ 0x0e, 0x02a79130 }, /* front input jack: microphone */
	{ 0, 0 }
};
static const struct hda_pincfg idt_dell_bios_spdif_pins[] = {
	{ 0x0f, 0x0221101f }, { 0x0e, 0x02a79130 },
	{ 0x21, 0x01442070 }, /* device field: S/PDIF out */
	{ 0, 0 }
};

/* -- STAC9205 ------------------------------------------------------------------- */
static const struct hda_pincfg idt_ref9205_pins[] = {
	{ 0x0a, 0x40000100 }, { 0x0b, 0x40000100 }, { 0x0c, 0x01016011 },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x01813122 }, { 0x0f, 0x01a19021 },
	{ 0x14, 0x01019020 }, { 0x16, 0x40000100 }, { 0x17, 0x90a000f0 },
	{ 0x18, 0x90a000f0 }, { 0x21, 0x01441030 }, { 0x22, 0x01c41030 },
	{ 0, 0 }
};
static const struct hda_pincfg idt_dell9205_m42_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x03a11020 }, { 0x0c, 0x400003fa },
	{ 0x0d, 0x90170310 }, { 0x0e, 0x400003fb }, { 0x0f, 0x400003fc },
	{ 0x14, 0x400003fd }, { 0x16, 0x40f000f9 }, { 0x17, 0x90a60330 },
	{ 0x18, 0x400003ff }, { 0x21, 0x0144131f }, { 0x22, 0x40c003fe },
	{ 0, 0 }
};
static const struct hda_pincfg idt_dell9205_m43_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x03a11020 }, { 0x0c, 0x90a70330 },
	{ 0x0d, 0x90170310 }, { 0x0e, 0x400000fe }, { 0x0f, 0x400000ff },
	{ 0x14, 0x400000fd }, { 0x16, 0x40f000f9 }, { 0x17, 0x400000fa },
	{ 0x18, 0x400000fc }, { 0x21, 0x0144131f }, { 0x22, 0x40c003f8 },
	{ 0x1f, 0x01441030 }, { 0x20, 0x1c410030 }, /* S/PDIF in and out */
	{ 0, 0 }
};
static const struct hda_pincfg idt_dell9205_m44_pins[] = {
	{ 0x0a, 0x0421101f }, { 0x0b, 0x04a11020 }, { 0x0c, 0x400003fa },
	{ 0x0d, 0x90170310 }, { 0x0e, 0x400003fb }, { 0x0f, 0x400003fc },
	{ 0x14, 0x400003fd }, { 0x16, 0x400003f9 }, { 0x17, 0x90a60330 },
	{ 0x18, 0x400003ff }, { 0x21, 0x01441340 }, { 0x22, 0x40c003fe },
	{ 0, 0 }
};

/* -- STAC9872 ------------------------------------------------------------------- */
static const struct hda_pincfg idt_vaio9872_pins[] = {
	{ 0x0a, 0x03211020 }, { 0x0b, 0x411111f0 }, { 0x0c, 0x411111f0 },
	{ 0x0d, 0x03a15030 }, { 0x0e, 0x411111f0 }, { 0x0f, 0x90170110 },
	{ 0x11, 0x411111f0 }, { 0x13, 0x411111f0 }, { 0x14, 0x90a7013e },
	{ 0, 0 }
};

/* -- 92HD73xx / 92HD89xx -------------------------------------------------------- */
static const struct hda_pincfg idt_ref92hd73_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x02a19040 }, { 0x0c, 0x01a19020 },
	{ 0x0d, 0x02214030 }, { 0x0e, 0x0181302e }, { 0x0f, 0x01014010 },
	{ 0x10, 0x01014020 }, { 0x11, 0x01014030 }, { 0x12, 0x02319040 },
	{ 0x13, 0x90a000f0 }, { 0x14, 0x90a000f0 }, { 0x22, 0x01452050 },
	{ 0x23, 0x01452050 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_m6_amic_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x90a70170 }, { 0x0c, 0x4f0000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x03a11020 }, { 0x0f, 0x0321101f },
	{ 0x10, 0x4f0000f0 }, { 0x11, 0x4f0000f0 }, { 0x12, 0x4f0000f0 },
	{ 0x13, 0x90a60160 }, { 0x14, 0x4f0000f0 }, { 0x22, 0x4f0000f0 },
	{ 0x23, 0x4f0000f0 }, { 0, 0 }
};
/* the digital-mic and both-mics variants share this table; only the
 * analog-mic one replaces port B */
static const struct hda_pincfg idt_dell_m6_dmic_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x4f00000f }, { 0x0c, 0x4f0000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x03a11020 }, { 0x0f, 0x0321101f },
	{ 0x10, 0x4f0000f0 }, { 0x11, 0x4f0000f0 }, { 0x12, 0x4f0000f0 },
	{ 0x13, 0x90a60160 }, { 0x14, 0x4f0000f0 }, { 0x22, 0x4f0000f0 },
	{ 0x23, 0x4f0000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_m6_both_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x90a70170 }, { 0x0c, 0x4f0000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x03a11020 }, { 0x0f, 0x0321101f },
	{ 0x10, 0x4f0000f0 }, { 0x11, 0x4f0000f0 }, { 0x12, 0x4f0000f0 },
	{ 0x13, 0x90a60160 }, { 0x14, 0x4f0000f0 }, { 0x22, 0x4f0000f0 },
	{ 0x23, 0x4f0000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_alienware_m17x_pins[] = {
	{ 0x0a, 0x0321101f }, { 0x0b, 0x0321101f }, { 0x0c, 0x03a11020 },
	{ 0x0d, 0x03014020 }, { 0x0e, 0x90170110 }, { 0x0f, 0x4f0000f0 },
	{ 0x10, 0x4f0000f0 }, { 0x11, 0x4f0000f0 }, { 0x12, 0x4f0000f0 },
	{ 0x13, 0x90a60160 }, { 0x14, 0x4f0000f0 }, { 0x22, 0x4f0000f0 },
	{ 0x23, 0x904601b0 }, { 0, 0 }
};
static const struct hda_pincfg idt_intel_dg45id_pins[] = {
	{ 0x0a, 0x02214230 }, { 0x0b, 0x02a19240 }, { 0x0c, 0x01013214 },
	{ 0x0d, 0x01014210 }, { 0x0e, 0x01a19250 }, { 0x0f, 0x01011212 },
	{ 0x10, 0x01016211 }, { 0x22, 0x01451380 }, { 0x23, 0x40f000f0 },
	{ 0, 0 }
};
static const struct hda_pincfg idt_hp_front_jack_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x02a19010 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_z1_g2_pins[] = {
	{ 0x0e, 0x400000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_asus_mobo_pins[] = {
	{ 0x0c, 0x01014411 }, { 0x0d, 0x01014410 }, { 0x0e, 0x01014412 },
	{ 0x22, 0x014b1180 }, { 0, 0 }
};

/* -- 92HD83xxx and relatives ---------------------------------------------------- */
static const struct hda_pincfg idt_ref92hd83_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x02211010 }, { 0x0c, 0x02a19020 },
	{ 0x0d, 0x02170130 }, { 0x0e, 0x01014050 }, { 0x0f, 0x01819040 },
	{ 0x10, 0x01014020 }, { 0x11, 0x90a3014e }, { 0x1f, 0x01451160 },
	{ 0x20, 0x98560170 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_s14_pins[] = {
	{ 0x0a, 0x0221403f }, { 0x0b, 0x0221101f }, { 0x0c, 0x02a19020 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x40f000f0 }, { 0x0f, 0x40f000f0 },
	{ 0x10, 0x40f000f0 }, { 0x11, 0x90a60160 }, { 0x1f, 0x40f000f0 },
	{ 0x20, 0x40f000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_vostro3500_pins[] = {
	{ 0x0a, 0x02a11020 }, { 0x0b, 0x0221101f }, { 0x0c, 0x400000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x400000f1 }, { 0x0f, 0x400000f2 },
	{ 0x10, 0x400000f3 }, { 0x11, 0x90a60160 }, { 0x1f, 0x400000f4 },
	{ 0x20, 0x400000f5 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_dv7_4000_pins[] = {
	{ 0x0a, 0x03a12050 }, { 0x0b, 0x0321201f }, { 0x0c, 0x40f000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x40f000f0 }, { 0x0f, 0x40f000f0 },
	{ 0x10, 0x90170110 }, { 0x11, 0xd5a30140 }, { 0x1f, 0x40f000f0 },
	{ 0x20, 0x40f000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_zephyr_pins[] = {
	{ 0x0a, 0x01813050 }, { 0x0b, 0x0421201f }, { 0x0c, 0x04a1205e },
	{ 0x0d, 0x96130310 }, { 0x0e, 0x96130310 }, { 0x0f, 0x0101401f },
	{ 0x10, 0x1111611f }, { 0x11, 0xd5a30130 }, { 0, 0 }
};
static const struct hda_verb idt_hp_zephyr_verbs[] = {
	{ 0x22, 0x785, 0x43 },
	{ 0x22, 0x782, 0xe0 },
	{ 0x22, 0x795, 0x00 },
	{ 0, 0, 0 }
};
static const struct hda_pincfg idt_hp_cnb11_pins[] = {
	{ 0x0a, 0x40f000f0 }, { 0x0b, 0x0221101f }, { 0x0c, 0x02a11020 },
	{ 0x0d, 0x92170110 }, { 0x0e, 0x40f000f0 }, { 0x0f, 0x92170110 },
	{ 0x10, 0x40f000f0 }, { 0x11, 0xd5a30130 }, { 0x1f, 0x40f000f0 },
	{ 0x20, 0x40f000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_envy_bass_pins[] = {
	{ 0x0f, 0x90170111 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_envy_ts_bass_pins[] = {
	{ 0x10, 0x92170111 }, { 0, 0 }
};

/* -- 92HD71bxx / 92HD75 ------------------------------------------------------- */
static const struct hda_pincfg idt_ref92hd71_pins[] = {
	{ 0x0a, 0x02214030 }, { 0x0b, 0x02a19040 }, { 0x0c, 0x01a19020 },
	{ 0x0d, 0x01014010 }, { 0x0e, 0x0181302e }, { 0x0f, 0x01014010 },
	{ 0x14, 0x01019020 }, { 0x18, 0x90a000f0 }, { 0x19, 0x90a000f0 },
	{ 0x1e, 0x01452050 }, { 0x1f, 0x01452050 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_m4_1_pins[] = {
	{ 0x0a, 0x0421101f }, { 0x0b, 0x04a11221 }, { 0x0c, 0x40f000f0 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x23a1902e }, { 0x0f, 0x23014250 },
	{ 0x14, 0x40f000f0 }, { 0x18, 0x90a000f0 }, { 0x19, 0x40f000f0 },
	{ 0x1e, 0x4f0000f0 }, { 0x1f, 0x4f0000f0 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_m4_2_pins[] = {
	{ 0x0a, 0x0421101f }, { 0x0b, 0x04a11221 }, { 0x0c, 0x90a70330 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x23a1902e }, { 0x0f, 0x23014250 },
	{ 0x14, 0x40f000f0 }, { 0x18, 0x40f000f0 }, { 0x19, 0x40f000f0 },
	{ 0x1e, 0x044413b0 }, { 0x1f, 0x044413b0 }, { 0, 0 }
};
static const struct hda_pincfg idt_dell_m4_3_pins[] = {
	{ 0x0a, 0x0421101f }, { 0x0b, 0x04a11221 }, { 0x0c, 0x90a70330 },
	{ 0x0d, 0x90170110 }, { 0x0e, 0x40f000f0 }, { 0x0f, 0x40f000f0 },
	{ 0x14, 0x40f000f0 }, { 0x18, 0x90a000f0 }, { 0x19, 0x40f000f0 },
	{ 0x1e, 0x044413b0 }, { 0x1f, 0x044413b0 }, { 0, 0 }
};
static const struct hda_pincfg idt_hp_dv5_pins[] = {
	{ 0x0d, 0x90170010 }, /* the speaker */
	{ 0, 0 }
};
static const struct hda_pincfg idt_hp_m4_pins[] = {
	{ 0x0e, 0x01813040 }, /* internal microphone */
	{ 0, 0 }
};

/* -- 92HD95 ------------------------------------------------------------------- */
static const struct hda_verb idt_hp_spectre13_verbs[] = {
	{ 0x1a, 0x795, 0x00 }, /* bass speaker high-pass to 100 Hz */
	{ 0, 0, 0 }
};

#define F9200 FB(IDT_9200)
#define F922X FB(IDT_922X)
#define F925X FB(IDT_925X)
#define F927X FB(IDT_927X)
#define F9205 FB(IDT_9205)
#define F9872 FB(IDT_9872)
#define F73 FB(IDT_92HD73)
#define F83 FB(IDT_92HD83)
#define F71 FB(IDT_92HD71)
#define F95 FB(IDT_92HD95)
/* evaluation boards: everything but the STAC922x, whose 8086:2668 subsystem
 * id is also what emulated ICH6 controllers report */
#define FREF (F9200 | F925X | F927X | F9205 | F73 | F83 | F71)

/* Model entry with no GPIO/LED overrides */
#define M(fam, ss, msk, nm, pins, verbs, q)                                    \
	{ fam, 0, ss, msk, nm, pins, verbs, q, 0, 0, 0, 0, 0, 0, 0 }
/* with a GPIO setup */
#define MG(fam, ss, msk, nm, pins, verbs, q, gm, gd, gv, em)                   \
	{ fam, 0, ss, msk, nm, pins, verbs, (q) | IDTQ_GPIO, gm, gd, gv, em, 0, \
	  0, 0 }
/* with a mute LED */
#define ML(fam, ss, msk, nm, pins, verbs, q, led, pol, mic)                    \
	{ fam, 0, ss, msk, nm, pins, verbs, q, 0, 0, 0, 0, led, pol, mic }

static const struct idt_model idt_models[] = {
	/* vendor evaluation boards */
	MG(FREF, SSID(0x8086, 0x2668), 0, "DFI LanParty", 0, 0, IDTQ_REF, 0, 0,
	   0, 0),
	MG(FREF, SSID(0x15bd, 0x3101), 0, "DFI LanParty", 0, 0, IDTQ_REF, 0, 0,
	   0, 0),
	MG(F922X, SSID(0x15bd, 0x3101), 0, "DFI LanParty", idt_d945_ref_pins, 0,
	   IDTQ_REF, 0, 0, 0, 0),

	/* -- STAC9200 -- */
	M(F9200, SSID(0x1028, 0x01a8), 0, "Dell", idt_dell9200_d21_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01b5), 0, "Dell Inspiron 630m",
	  idt_dell9200_m21_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01bd), 0, "Dell Inspiron E1505n",
	  idt_dell9200_m25_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c0), 0, "Dell", idt_dell9200_d22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c1), 0, "Dell", idt_dell9200_d22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c2), 0, "Dell Latitude D620",
	  idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c5), 0, "Dell", idt_dell9200_d23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c7), 0, "Dell", idt_dell9200_d23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c8), 0, "Dell", idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01c9), 0, "Dell", idt_dell9200_m24_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01ca), 0, "Dell", idt_dell9200_m24_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01cb), 0, "Dell Latitude 120L",
	  idt_dell9200_m24_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01cc), 0, "Dell Latitude D820",
	  idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01cd), 0, "Dell Inspiron E1705/9400",
	  idt_dell9200_m27_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01ce), 0, "Dell XPS M1710",
	  idt_dell9200_m23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01cf), 0, "Dell Precision M90",
	  idt_dell9200_m23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01d3), 0, "Dell", idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01d4), 0, "Dell", idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01d6), 0, "Dell", idt_dell9200_m22_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01d8), 0, "Dell Inspiron 640m",
	  idt_dell9200_m21_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01d9), 0, "Dell", idt_dell9200_d23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01da), 0, "Dell", idt_dell9200_d23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01de), 0, "Dell", idt_dell9200_d21_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01e3), 0, "Dell", idt_dell9200_d23_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01e8), 0, "Dell", idt_dell9200_d21_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01ee), 0, "Dell", idt_dell9200_m25_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01ef), 0, "Dell", idt_dell9200_m25_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01f5), 0, "Dell Inspiron 1501",
	  idt_dell9200_m26_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x01f6), 0, "Dell", idt_dell9200_m26_pins, 0, 0),
	M(F9200, SSID(0x1028, 0x0201), 0, "Dell Latitude D430",
	  idt_dell9200_m22_pins, 0, 0),
	/* Panasonic CF-74: the hardware switches HP/speaker itself */
	MG(F9200, SSID(0x10f7, 0x8338), 0, "Panasonic CF-74", 0, 0,
	   IDTQ_NO_AUTOMUTE | IDTQ_NO_JACK_DETECT, 0x09, 0x09, 0x00, 0),
	/* Gateway: EAPD must be re-set after every reset */
	M(F9200, SSID(0x107b, 0x0205), 0, "Gateway S-7110M",
	  idt_gateway9200_m4_pins, idt_9200_eapd_verbs, 0),
	M(F9200, SSID(0x107b, 0x0317), 0, "Gateway MT3423",
	  idt_gateway9200_m4_pins, idt_9200_eapd_verbs, 0),
	M(F9200, SSID(0x107b, 0x0318), 0, "Gateway ML3019",
	  idt_gateway9200_m4_pins, idt_9200_eapd_verbs, 0),
	M(F9200, SSID(0x1106, 0x3288), 0, "OQO Model 2", idt_oqo9200_pins,
	  idt_9200_eapd_verbs, 0),

	/* -- STAC925x -- */
	M(F925X, SSID(0x8384, 0x7632), 0, "STAC9202 evaluation board",
	  idt_ref925x_pins, 0, 0),
	M(F925X, SSID(0x1002, 0x437b), 0, "Gateway mobile", idt_925x_m1_pins,
	  0, 0),
	M(F925X, SSID(0x107b, 0x0316), 0, "Gateway M255", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0366), 0, "Gateway MP6954", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0461), 0, "Gateway NX560XL", idt_925x_m1_pins,
	  0, 0),
	M(F925X, SSID(0x107b, 0x0681), 0, "Gateway NX860", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0367), 0, "Gateway MX6453", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0281), 0, "Gateway mobile", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0507), 0, "Gateway mobile", idt_925x_m3_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0685), 0, "Gateway mobile", idt_925x_m1_pins, 0,
	  0),
	M(F925X, SSID(0x107b, 0x0000), VONLY, "Gateway", idt_925x_m6_pins, 0,
	  0),

	/* -- STAC922x -- */
	M(F922X, SSID(0x8086, 0x0101), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0202), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0606), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0601), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0111), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x1115), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x1116), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x1117), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x1118), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x1119), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x8826), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x5049), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x5055), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x5048), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0110), 0, "Intel D945G", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0404), 0, "Intel D945G 5-stack",
	  idt_d945gtp5_pins, 0, 0),
	M(F922X, SSID(0x8086, 0x0303), 0, "Intel D945G 5-stack",
	  idt_d945gtp5_pins, 0, 0),
	M(F922X, SSID(0x8086, 0x0013), 0, "Intel D945G 5-stack",
	  idt_d945gtp5_pins, 0, 0),
	M(F922X, SSID(0x8086, 0x0417), 0, "Intel D945G 5-stack",
	  idt_d945gtp5_pins, 0, 0),
	M(F922X, SSID(0x8086, 0x0b0b), 0, "Intel D945P", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0112), 0, "Intel D945P", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0d0d), 0, "Intel D945P", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0909), 0, "Intel D945P", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0505), 0, "Intel D945P", idt_d945gtp3_pins, 0,
	  0),
	M(F922X, SSID(0x8086, 0x0707), 0, "Intel D945P 5-stack",
	  idt_d945gtp5_pins, 0, 0),
	M(F922X, SSID(0x8086, 0x0204), 0, "Intel D945", idt_d945_ref_pins, 0,
	  0),
	/* Intel Macs share 8384:7680; the codec subsystem id tells them
	 * apart (VirtualBox is filtered out before this table) */
	M(F922X, SSID(0x8384, 0x7680), 0, "Intel Mac", 0, 0, IDTQ_INTEL_MAC),
	M(F922X, SSID(0x1028, 0x01a7), 0, "Dell", idt_dell922x_d81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01a9), 0, "Dell", idt_dell922x_d81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01ab), 0, "Dell", idt_dell922x_d81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01ac), 0, "Dell", idt_dell922x_d82_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01bf), 0, "Dell", idt_dell922x_m81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01d0), 0, "Dell", idt_dell922x_d82_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01d1), 0, "Dell", idt_dell922x_d81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01d2), 0, "Dell", idt_dell922x_d81_pins, 0, 0),
	M(F922X, SSID(0x1028, 0x01d7), 0, "Dell XPS M1210",
	  idt_dell922x_m82_pins, 0, 0),
	M(F922X, SSID(0x1019, 0x2000), 0xfffff000u, "ECS/PC Chips",
	  idt_ecs202_pins, 0, 0),

	/* -- STAC927x -- */
	M(F927X, SSID(0x8086, 0x3d01), 0, "Intel D946", idt_d965_3st_pins,
	  idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x8086, 0xa301), 0, "Intel D946", idt_d965_3st_pins,
	  idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x8086, 0x2100), 0xffffff00u, "Intel D965",
	  idt_d965_3st_pins, idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x8086, 0x2000), 0xffffff00u, "Intel D965",
	  idt_d965_3st_pins, idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x8086, 0x2300), 0xffffff00u, "Intel D965 5-stack",
	  idt_d965_5st_pins, idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x8086, 0x2500), 0xffffff00u, "Intel D965 5-stack",
	  idt_d965_5st_pins, idt_d965_core, IDTQ_D965),
	M(F927X, SSID(0x1028, 0x01dd), 0, "Dell Dimension E520",
	  idt_dell_3st_pins, 0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x01ed), 0, "Dell", idt_dell_3st_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x01f4), 0, "Dell", idt_dell_3st_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x01f3), 0, "Dell Inspiron 1420",
	  idt_dell_bios_pins, 0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x01f7), 0, "Dell XPS M1730", idt_dell_bios_pins,
	  0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x0227), 0, "Dell Vostro 1400",
	  idt_dell_bios_pins, 0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x022e), 0, "Dell", idt_dell_bios_spdif_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x022f), 0, "Dell Inspiron 1525",
	  idt_dell_bios_pins, 0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x0242), 0, "Dell", idt_dell_bios_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x0243), 0, "Dell", idt_dell_bios_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x02ff), 0, "Dell", idt_dell_bios_pins, 0,
	  IDTQ_DELL_DMIC),
	M(F927X, SSID(0x1028, 0x0209), 0, "Dell XPS 1330",
	  idt_dell_bios_spdif_pins, 0, IDTQ_DELL_DMIC),
	M(F927X, SSID(0x10cf, 0x0000), VONLY, "Fujitsu-Siemens", 0, 0,
	  IDTQ_VOLKNOB),

	/* -- STAC9205 -- */
	M(F9205, SSID(0x1028, 0x01f1), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x01f2), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x01fc), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x01fd), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x0204), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x021f), 0, "Dell", idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x0228), 0, "Dell Vostro 1500",
	  idt_dell9205_m42_pins, 0, 0),
	M(F9205, SSID(0x1028, 0x0229), 0, "Dell Vostro 1700",
	  idt_dell9205_m42_pins, 0, 0),
	/* M43: GPIO0 high = EAPD, GPIO1 low = headphone mute, GPIO3 low =
	 * DRM, GPIO4 = dock headphone sense */
	MG(F9205, SSID(0x1028, 0x01f9), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x01fa), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x01fe), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x01ff), 0, "Dell Precision M4300",
	   idt_dell9205_m43_pins, 0, 0, 0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x0206), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x0200), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	MG(F9205, SSID(0x1028, 0x0201), 0, "Dell", idt_dell9205_m43_pins, 0, 0,
	   0x1b, 0x0b, 0x01, 0x01),
	M(F9205, SSID(0x1028, 0x01f8), 0, "Dell", idt_dell9205_m44_pins, 0, 0),
	M(F9205, SSID(0x107b, 0x0560), 0, "Gateway T6834c", 0, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F9205, SSID(0x107b, 0x0565), 0, "Gateway T1616", 0, 0,
	  IDTQ_NO_EAPD_SWITCH),

	/* -- STAC9872 -- */
	M(F9872, SSID(0x104d, 0x81e0), 0xfffffff0u, "Sony VAIO F/S",
	  idt_vaio9872_pins, 0, 0),

	/* -- 92HD73xx / 92HD89xx -- */
	M(F73, SSID(0x8086, 0x5001), 0, "Intel DP45SG", idt_intel_dg45id_pins,
	  0, 0),
	M(F73, SSID(0x8086, 0x5002), 0, "Intel DG45ID", idt_intel_dg45id_pins,
	  0, 0),
	M(F73, SSID(0x8086, 0x5003), 0, "Intel DG45FC", idt_intel_dg45id_pins,
	  0, 0),
	M(F73, SSID(0x1028, 0x0254), 0, "Dell Studio 1535",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0255), 0, "Dell", idt_dell_m6_dmic_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0256), 0, "Dell", idt_dell_m6_both_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0257), 0, "Dell", idt_dell_m6_both_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x025e), 0, "Dell", idt_dell_m6_amic_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x025f), 0, "Dell", idt_dell_m6_amic_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0271), 0, "Dell", idt_dell_m6_dmic_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0272), 0, "Dell", idt_dell_m6_dmic_pins, 0,
	  IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x029f), 0, "Dell Studio 1537",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x02a0), 0, "Dell Studio 17",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x02be), 0, "Dell Studio 1555",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x02bd), 0, "Dell Studio 1557",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x02fe), 0, "Dell Studio XPS 1645",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0413), 0, "Dell Studio 1558",
	  idt_dell_m6_dmic_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x02a1), 0, "Alienware M17x",
	  idt_alienware_m17x_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x043a), 0, "Alienware M17x",
	  idt_alienware_m17x_pins, 0, IDTQ_NO_EAPD_SWITCH),
	M(F73, SSID(0x1028, 0x0490), 0, "Alienware M17x R3",
	  idt_dell_m6_dmic_pins, idt_92hd73_dell_eq_core,
	  IDTQ_NO_EAPD_SWITCH | IDTQ_DELL_EQ),
	M(F73, SSID(0x1059, 0x1011), 0, "ELO VuPoint 15MX", 0, 0,
	  IDTQ_NO_AUTOMUTE),
	M(F73, SSID(0x103c, 0x1927), 0, "HP Z1 G2", idt_hp_z1_g2_pins, 0, 0),
	M(F73, SSID(0x103c, 0x2b17), 0, "HP", idt_hp_front_jack_pins, 0, 0),
	M(F73, SSID(0x1043, 0x83f8), 0, "ASUS AT4NM10", idt_asus_mobo_pins, 0,
	  0),

	/* -- 92HD83xxx and relatives -- */
	M(F83, SSID(0x1028, 0x02ba), 0, "Dell", idt_dell_s14_pins, 0, 0),
	M(F83, SSID(0x1028, 0x0532), 0, "Dell Latitude E6230", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x0533), 0, "Dell Latitude E6330", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x0534), 0, "Dell Latitude E6430", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x0535), 0, "Dell Latitude E6530", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x053c), 0, "Dell Latitude E5430", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x053d), 0, "Dell Latitude E5530", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x0549), 0, "Dell Latitude E5430", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x057d), 0, "Dell Latitude E6430s", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x0584), 0, "Dell Latitude E6430U", 0, 0,
	  IDTQ_HEADSET_JACK),
	M(F83, SSID(0x1028, 0x1028), 0, "Dell Vostro 3500",
	  idt_dell_vostro3500_pins, 0, 0),
	M(F83, SSID(0x103c, 0x1656), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x1657), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x1658), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x1659), 0, "HP Pavilion dv7",
	  idt_hp_dv7_4000_pins, 0, IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x165a), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x165b), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x1888), 0, "HP Envy Spectre",
	  idt_hp_envy_bass_pins, 0, 0),
	ML(F83, SSID(0x103c, 0x1899), 0, "HP Folio 13", 0, 0, IDTQ_HP_BNB_DOCK,
	   0x10, 0, 0),
	M(F83, SSID(0x103c, 0x190e), 0, "HP ENVY TS", idt_hp_envy_ts_bass_pins,
	  0, 0),
	M(F83, SSID(0x103c, 0x1967), 0, "HP ENVY TS", idt_hp_envy_ts_bass_pins,
	  0, 0),
	M(F83, SSID(0x103c, 0x3388), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3389), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x355b), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x355c), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x355d), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x355e), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x355f), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3560), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x358b), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x358c), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x358d), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3591), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3592), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3593), 0, "HP", idt_hp_cnb11_pins, 0,
	  IDTQ_HP_BNB_DOCK),
	M(F83, SSID(0x103c, 0x3561), 0, "HP Zephyr", idt_hp_zephyr_pins,
	  idt_hp_zephyr_verbs, IDTQ_HP_BNB_DOCK | IDTQ_ZEPHYR),
	ML(F83, SSID(0x103c, 0x3660), 0, "HP Mini", 0, 0,
	   IDTQ_HP_BNB_DOCK | IDTQ_LED_AUTO, 0, 0, 0),
	ML(F83, SSID(0x103c, 0x144e), 0, "HP Pavilion dv5", 0, 0,
	   IDTQ_HP_BNB_DOCK | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F83, SSID(0x103c, 0x148a), 0, "HP Mini", 0, 0,
	   IDTQ_HP_BNB_DOCK | IDTQ_LED_AUTO, 0, 0, 0),
	/* HP business notebooks of 2012/13: mic-mute LED on GPIO3 */
	ML(F83, SSID(0x103c, 0x1900), 0xffffff00u, "HP", 0, 0,
	   IDTQ_HP_BNB_DOCK, 0, 0, 0x08),
	ML(F83, SSID(0x103c, 0x2000), 0xffffff00u, "HP", 0, 0,
	   IDTQ_HP_BNB_DOCK, 0, 0, 0x08),
	ML(F83, SSID(0x103c, 0x2100), 0xffffff00u, "HP", 0, 0,
	   IDTQ_HP_BNB_DOCK, 0, 0, 0x08),
	M(F83, SSID(0x103c, 0x0000), VONLY, "HP", 0, 0, IDTQ_HP_BNB_DOCK),
	/* Toshiba Satellite S50D: speaker amplifier on GPIO4 */
	MG(F83, SSID(0x1179, 0xfa91), 0xfffffffdu, "Toshiba Satellite S50D", 0,
	   0, IDTQ_NO_EAPD_SWITCH, 0x10, 0x10, 0x10, 0x10),

	/* -- 92HD71bxx -- */
	ML(F71, SSID(0x103c, 0x1720), 0xfffffff0u, "HP", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x3080), 0xfffffff0u, "HP", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x30f0), 0xfffffff0u, "HP dv4-7", idt_hp_dv5_pins,
	   0, IDTQ_HP_BASS | IDTQ_HP_DUAL_HP, 0x01, 1, 0),
	ML(F71, SSID(0x103c, 0x3600), 0xfffffff0u, "HP dv4-7", idt_hp_dv5_pins,
	   0, IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x3610), 0, "HP HDX18", 0, 0, IDTQ_HP_DUAL_HP,
	   0x08, 1, 0),
	ML(F71, SSID(0x103c, 0x361a), 0, "HP Mini 1000", idt_hp_m4_pins, 0,
	   IDTQ_HP_M4 | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x361b), 0, "HP HDX16", 0, 0, IDTQ_HP_DUAL_HP,
	   0x08, 1, 0),
	ML(F71, SSID(0x103c, 0x3620), 0xfffffff0u, "HP dv6", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x3061), 0, "HP dv6", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x363e), 0, "HP dv6", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x7010), 0xfffffff0u, "HP", idt_hp_dv5_pins, 0,
	   IDTQ_HP_BASS | IDTQ_HP_DUAL_HP | IDTQ_LED_AUTO, 0, 1, 0),
	ML(F71, SSID(0x103c, 0x0000), VONLY, "HP", 0, 0, IDTQ_HP_DUAL_HP, 0, 1,
	   0),
	M(F71, SSID(0x1028, 0x0233), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0234), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0250), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x024f), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x024d), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0251), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0277), 0, "Dell", idt_dell_m4_1_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0263), 0, "Dell", idt_dell_m4_2_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0265), 0, "Dell", idt_dell_m4_2_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0262), 0, "Dell", idt_dell_m4_2_pins, 0, 0),
	M(F71, SSID(0x1028, 0x0264), 0, "Dell", idt_dell_m4_2_pins, 0, 0),
	M(F71, SSID(0x1028, 0x02aa), 0, "Dell", idt_dell_m4_3_pins, 0, 0),

	/* -- 92HD95 -- */
	ML(F95, SSID(0x103c, 0x1911), 0, "HP Spectre 13", 0,
	   idt_hp_spectre13_verbs, IDTQ_LED_AUTO, 0, 0, 0),
	ML(F95, SSID(0x103c, 0x0000), VONLY, "HP", 0, 0, IDTQ_LED_AUTO, 0, 0,
	   0),

	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* Intel Macs, matched on the codec subsystem id (GPIO0/1 = amplifiers) */
#define MAC(ss, pins)                                                       \
	{ F922X, 1, ss, 0, "Intel Mac", pins, 0, IDTQ_GPIO, 0x03, 0x03, 0x03, \
	  0, 0, 0, 0 }
static const struct idt_model idt_mac_models[] = {
	MAC(SSID(0x0000, 0x0100), idt_mac_v3_pins), /* Mac mini */
	MAC(SSID(0x106b, 0x0800), idt_mac_v1_pins),
	MAC(SSID(0x106b, 0x0600), idt_mac_v2_pins),
	MAC(SSID(0x106b, 0x0700), idt_mac_v2_pins),
	MAC(SSID(0x106b, 0x0e00), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x0f00), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x1600), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x1700), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x0200), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x1e00), idt_mac_v3_pins),
	MAC(SSID(0x106b, 0x1a00), idt_mac_v4_pins),
	MAC(SSID(0x106b, 0x0a00), idt_mac_v4_pins),
	MAC(SSID(0x106b, 0x2200), idt_mac_v4_pins),
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* HP business notebooks of 2011 with a docking connector (codec ssid) */
static const uint16_t idt_hp_bnb_dock_ids[] = {
	0x1618, 0x1619, 0x161a, 0x161b, 0x161c, 0x161d, 0x161e, 0x161f,
	0x162a, 0x162b, 0x1630, 0x1631, 0x1633, 0x1634, 0x1635, 0x3587,
	0x3588, 0x3589, 0x358a, 0x3667, 0x3668, 0x3669, 0
};

/* ---- helpers ------------------------------------------------------------------- */

static uint32_t idt_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	return ((v & 0xffffu) << 16) | (v >> 16);
}

static int idt_ssid_match(uint32_t ssid, uint32_t want, uint32_t mask)
{
	if (ssid == 0 && want != 0)
		return 0;
	if (mask == 0)
		mask = 0xffffffffu;
	return (ssid & mask) == (want & mask);
}

static const struct idt_model *idt_lookup_in(const struct idt_model *t,
					     int fam, uint32_t ssid,
					     int codec_table)
{
	int i;

	for (i = 0; t[i].subsys || t[i].name; i++) {
		const struct idt_model *m = &t[i];

		if (m->families && !(m->families & FB(fam)))
			continue;
		if (!codec_table && m->by_codec)
			continue;
		if (idt_ssid_match(ssid, m->subsys, m->mask))
			return m;
	}
	return 0;
}

static const struct idt_model *idt_lookup(const struct hda_codec *c)
{
	int fam = idt_family(c);
	uint32_t pci = idt_pci_ssid(c);
	const struct idt_model *m = idt_lookup_in(idt_models, fam, pci, 0);

	if (!m && c->subsystem_id && c->subsystem_id != pci)
		m = idt_lookup_in(idt_models, fam, c->subsystem_id, 0);
	if (m && (m->quirks & IDTQ_INTEL_MAC)) {
		/* the Macs are told apart by the codec's own id; no match
		 * means a board we know nothing about: plain BIOS config */
		m = idt_lookup_in(idt_mac_models, fam, c->subsystem_id, 1);
	}
	return m;
}

static void idt_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static int idt_is_vbox(struct hda_codec *c)
{
	return c->vendor_id == 0x83847680u && hda_vm_driver.match &&
	       hda_vm_driver.match(c);
}

static uint32_t idt_gpio_count(struct hda_codec *c)
{
	uint32_t caps = c->gpio_caps;

	if (caps == 0xffffffffu)
		return 0;
	return caps & AC_GPIO_IO_COUNT;
}

/* Merge bits into the codec GPIO state and push it out.  The pins are
 * switched to CMOS drive first, as these parts want. */
static void idt_gpio_update(struct hda_codec *c, uint32_t bits, uint32_t dir,
			    uint32_t data)
{
	uint32_t nmask = (c->gpio_mask | bits) & 0xff;
	uint32_t ndir = ((c->gpio_dir & ~bits) | (dir & bits)) & 0xff;
	uint32_t ndata = ((c->gpio_data & ~bits) | (data & bits)) & 0xff;

	if (!bits)
		return;
	if (c->afg)
		hda_codec_write(c, c->afg, IDT_VERB_GPIO_CMOS, 0);
	hda_gpio_set(c, nmask, ndir, ndata);
	c->gpio_mask = nmask;
	c->gpio_dir = ndir;
	c->gpio_data = ndata;
}

/* The board's GPIO plan: which bits, which outputs, which values. */
struct idt_gpio_plan {
	uint8_t mask, dir, data;
	uint8_t eapd; /* bits powering the speaker amplifier */
	uint8_t eapd_switch; /* switch them with the speaker */
	uint8_t led, led_pol, micled;
};

static void idt_plan(struct hda_codec *c, const struct idt_model *m,
		     struct idt_gpio_plan *p)
{
	int fam = idt_family(c);
	const struct idt_family_info *fi = &idt_families[fam];

	kmemset(p, 0, sizeof(*p));
	/* family default: the EAPD GPIO */
	p->mask = p->dir = p->data = p->eapd = fi->eapd_gpio;
	p->eapd_switch = fi->eapd_switch;
	if (fam == IDT_92HD71) {
		/* GPIO0 = EAPD, always on */
		p->mask = p->dir = p->data = 0x01;
		p->eapd = 0;
	}
	if (!m)
		return;
	if (m->quirks & IDTQ_REF) {
		kmemset(p, 0, sizeof(*p));
		return;
	}
	if (m->quirks & IDTQ_GPIO) {
		p->mask = m->gpio_mask;
		p->dir = m->gpio_dir;
		p->data = m->gpio_data;
		p->eapd = m->eapd_mask;
		p->mask |= p->eapd;
		p->dir |= p->eapd;
	}
	if ((m->quirks & IDTQ_DELL_DMIC) && c->subsystem_id != 0x1028022fu) {
		/* GPIO2 high = speaker amplifier on */
		p->mask = p->dir = p->data = p->eapd = 0x04;
	}
	if (m->quirks & IDTQ_NO_EAPD_SWITCH)
		p->eapd_switch = 0;
	if (m->quirks & IDTQ_HP_M4)
		p->mask |= 0x02; /* GPIO1: input, vref detect */
	if ((m->quirks & IDTQ_HP_BASS) && idt_gpio_count(c) >= 6) {
		/* bass speaker on GPIO5, on by default */
		p->mask |= 0x20;
		p->dir |= 0x20;
		p->data |= 0x20;
	}
	p->led = m->led_gpio;
	p->led_pol = m->led_polarity;
	if (!p->led && (m->quirks & IDTQ_LED_AUTO))
		p->led = idt_gpio_count(c) > 3 ? 0x08 : 0x01;
	if (p->led) {
		p->mask |= p->led;
		p->dir |= p->led;
		/* LED state for "not muted" */
		if (p->led_pol)
			p->data |= p->led;
		else
			p->data &= ~p->led;
	}
	p->micled = m->micled_gpio;
	if (p->micled) {
		p->mask |= p->micled;
		p->dir |= p->micled;
		p->data &= ~p->micled;
	}
}

static int idt_has_speaker(struct hda_codec *c)
{
	int i;

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].type == AUDIO_OUT_SPEAKER)
			return 1;
	return 0;
}

static int idt_hp_present(struct hda_codec *c)
{
	int i;

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &c->spec.out[i];

		if ((o->type == AUDIO_OUT_HEADPHONE ||
		     o->type == AUDIO_OUT_HEADSET) &&
		    o->jack_detect && o->present)
			return 1;
	}
	return 0;
}

/* Bring the GPIO lines to the plan, EAPD and LEDs per the current state. */
static void idt_apply_gpio(struct hda_codec *c, const struct idt_model *m,
			   int hp_present)
{
	struct idt_gpio_plan p;
	uint8_t data;

	idt_plan(c, m, &p);
	if (!p.mask)
		return;
	data = p.data;
	if (p.eapd) {
		int spk_off = p.eapd_switch && c->spec.auto_mute &&
			      idt_has_speaker(c) && hp_present;

		if (spk_off)
			data &= ~p.eapd;
		else
			data |= p.eapd;
	}
	if (p.led) {
		int lit = c->spec.master_mute != 0;

		if (p.led_pol)
			lit = !lit;
		if (lit)
			data |= p.led;
		else
			data &= ~p.led;
	}
	idt_gpio_update(c, p.mask, p.dir, data);
}

/* Port power map: unconnected ports off, every other one on. */
static void idt_power_map(struct hda_codec *c)
{
	const struct idt_family_info *fi = &idt_families[idt_family(c)];
	uint32_t bits = 0;
	int i;

	if (!fi->pwr_nids || !c->afg)
		return;
	for (i = 0; fi->pwr_nids[i]; i++) {
		struct hda_widget *w = hda_widget(c, fi->pwr_nids[i]);

		if (!w || w->type != AC_WID_PIN)
			continue;
		if (((w->pin_cfg & AC_DEFCFG_PORT_CONN) >>
		     AC_DEFCFG_PORT_CONN_SHIFT) == AC_JACK_PORT_NONE)
			bits |= 1u << i;
	}
	hda_codec_write(c, c->afg, IDT_VERB_SET_POWER_MAP, bits);
}

static void idt_core_verbs(struct hda_codec *c, const struct idt_model *m)
{
	int fam = idt_family(c);
	const struct idt_family_info *fi = &idt_families[fam];

	if (m && (m->quirks & IDTQ_DELL_DMIC)) {
		hda_apply_verbs(c, idt_dell_3st_core);
		return;
	}
	if (m && (m->quirks & IDTQ_VOLKNOB)) {
		hda_apply_verbs(c, idt_927x_volknob_core);
		return;
	}
	if (m && (m->quirks & IDTQ_DELL_EQ))
		return; /* the model verbs carry the knob setting */
	if (fam == IDT_92HD71) {
		struct hda_widget *w = hda_widget(c, 0x28);

		if (w && w->type == AC_WID_VOL_KNB)
			hda_apply_verbs(c, idt_92hd71_volknob);
		return;
	}
	if (fi->core)
		hda_apply_verbs(c, fi->core);
}

/* 92HD71: the analog input amplifiers of these ports reset muted. */
static void idt_92hd71_unmute(struct hda_codec *c)
{
	static const uint8_t nids[] = { 0x0f, 0x0a, 0x0d };
	struct hda_widget *w = hda_widget(c, 0x0a);
	unsigned int i = 0;

	if (!w || !(w->caps & AC_WCAP_IN_AMP))
		return;
	switch (c->vendor_id) {
	case 0x111d76b6:
	case 0x111d76b7:
	case 0x111d7608:
		i = 1; /* no 0x0f on the 4/5-port parts */
		break;
	default:
		break;
	}
	for (; i < sizeof(nids); i++)
		hda_amp_set(c, nids[i], HDA_INPUT, 0, 0, 0, 0);
}

static uint32_t idt_cfg_device(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_DEVICE) >> AC_DEFCFG_DEVICE_SHIFT;
}

static int idt_is_hp_output(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t dev;

	if (!w || w->type != AC_WID_PIN)
		return 0;
	if (((w->pin_cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT) ==
	    AC_JACK_PORT_NONE)
		return 0;
	dev = idt_cfg_device(w->pin_cfg);
	/* the BIOS often calls them line outs */
	return dev == AC_JACK_LINE_OUT || dev == AC_JACK_HP_OUT;
}

/* Turn a pin into a headphone at the end of the association list. */
static void idt_make_headphone(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t cfg;

	if (!w)
		return;
	cfg = w->pin_cfg;
	cfg = (cfg & ~AC_DEFCFG_DEVICE) |
	      ((uint32_t)AC_JACK_HP_OUT << AC_DEFCFG_DEVICE_SHIFT);
	cfg = (cfg & ~(AC_DEFCFG_DEF_ASSOC | AC_DEFCFG_SEQUENCE)) | 0x1f;
	hda_pin_override_cfg(c, nid, cfg);
	w->pin_cfg = cfg;
}

static int idt_hp_bnb_with_dock(struct hda_codec *c)
{
	int i;

	if (c->vendor_id != 0x111d7605 && c->vendor_id != 0x111d76d1)
		return 0;
	if ((c->subsystem_id >> 16) != 0x103c)
		return 0;
	for (i = 0; idt_hp_bnb_dock_ids[i]; i++)
		if ((c->subsystem_id & 0xffff) == idt_hp_bnb_dock_ids[i])
			return 1;
	return 0;
}

/* ---- driver callbacks ------------------------------------------------------- */

static int idt_match(struct hda_codec *c)
{
	uint16_t ven = (uint16_t)(c->vendor_id >> 16);

	return (ven == 0x8384 || ven == 0x111d) && c->afg != 0;
}

static int idt_pre_parse(struct hda_codec *c)
{
	const struct idt_chip *chip;
	const struct idt_model *m;
	int fam;

	if (idt_is_vbox(c))
		return hda_vm_driver.pre_parse(c);

	chip = idt_find_chip(c->vendor_id);
	fam = idt_family(c);
	m = idt_lookup(c);

	idt_copy_name(c->vendor_name, sizeof(c->vendor_name),
		      (c->vendor_id >> 16) == 0x111d ? "IDT" : "SigmaTel");
	if (chip)
		idt_copy_name(c->chip_name, sizeof(c->chip_name), chip->name);
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "%s (%04x)",
			  idt_families[fam].name,
			  (unsigned int)(c->vendor_id & 0xffff));

	/* chip-wide pin fixes */
	if (c->vendor_id == 0x111d7608) {
		/* 92HD75B2: disable the VSW pins */
		hda_pin_override_cfg(c, 0x0f, 0x40f000f0);
		hda_pin_override_cfg(c, 0x19, 0x40f000f3);
	}
	if (fam == IDT_922X) {
		/* capture mux 0x12: two steps of boost at most */
		struct hda_widget *w = hda_widget(c, 0x12);

		if (w) {
			w->amp_out_caps =
				(0u << AC_AMPCAP_OFFSET_SHIFT) |
				(2u << AC_AMPCAP_NUM_STEPS_SHIFT) |
				(0x27u << AC_AMPCAP_STEP_SIZE_SHIFT);
			w->caps |= AC_WCAP_AMP_OVRD;
		}
	}

	if (m) {
		if (m->quirks & IDTQ_REF) {
			if (fam == IDT_9200)
				hda_apply_pincfgs(c, idt_ref9200_pins);
			else if (fam == IDT_925X)
				hda_apply_pincfgs(c, idt_ref925x_pins);
			else if (fam == IDT_927X)
				hda_apply_pincfgs(c, idt_ref927x_pins);
			else if (fam == IDT_9205)
				hda_apply_pincfgs(c, idt_ref9205_pins);
			else if (fam == IDT_92HD73)
				hda_apply_pincfgs(c, idt_ref92hd73_pins);
			else if (fam == IDT_92HD83)
				hda_apply_pincfgs(c, idt_ref92hd83_pins);
			else if (fam == IDT_92HD71)
				hda_apply_pincfgs(c, idt_ref92hd71_pins);
		}
		if (m->pins)
			hda_apply_pincfgs(c, m->pins);
		if ((m->quirks & IDTQ_HP_BNB_DOCK) && idt_hp_bnb_with_dock(c)) {
			hda_pin_override_cfg(c, 0x0a, 0x2101201f);
			hda_pin_override_cfg(c, 0x0f, 0x2181205e);
		}
		if ((m->quirks & IDTQ_HP_DUAL_HP) &&
		    idt_is_hp_output(c, 0x0a) && idt_is_hp_output(c, 0x0f)) {
			/* both A and F assigned: dock and built-in
			 * headphones, not line outs */
			idt_make_headphone(c, 0x0a);
			idt_make_headphone(c, 0x0f);
		}
		if ((m->quirks & IDTQ_HP_M4) && c->afg)
			/* GPIO1 reports the vref state */
			hda_codec_write(c, c->afg,
					AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK,
					0x02);
		kprintf("[snd] hda: codec %d: %s %s, board fixup \"%s\"\n",
			c->addr, c->vendor_name, c->chip_name, m->name);
	} else {
		kprintf("[snd] hda: codec %d: %s %s, BIOS pin configuration\n",
			c->addr, c->vendor_name, c->chip_name);
	}
	/* Dell M43: GPIO4 reports the dock headphone */
	if (m && fam == IDT_9205 && (m->quirks & IDTQ_GPIO) &&
	    (m->gpio_mask & 0x10) && c->afg)
		hda_codec_write(c, c->afg,
				AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK, 0x10);

	idt_core_verbs(c, m);
	if (m && m->verbs)
		hda_apply_verbs(c, m->verbs);
	if (fam == IDT_92HD71)
		idt_92hd71_unmute(c);
	idt_apply_gpio(c, m, 0);

	/* the digital beep widget, when it is one */
	if (!c->spec.beep_nid && idt_families[fam].beep_nid) {
		struct hda_widget *w = hda_widget(c, idt_families[fam].beep_nid);

		if (w && w->type == AC_WID_BEEP)
			c->spec.beep_nid = w->nid;
	}
	return 0;
}

static int idt_post_parse(struct hda_codec *c)
{
	const struct idt_model *m;
	int i;

	if (idt_is_vbox(c))
		return hda_vm_driver.post_parse(c);

	m = idt_lookup(c);
	if (m && (m->quirks & IDTQ_NO_AUTOMUTE))
		c->spec.auto_mute = 0;
	if (m && (m->quirks & IDTQ_NO_JACK_DETECT)) {
		c->spec.jack_poll = 0;
		for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++) {
			c->spec.out[i].jack_detect = 0;
			c->spec.out[i].present = 1;
		}
	}
	/* The STAC9200 powers its outputs from the EAPD of 0x08 only; the
	 * generic per-pin switching would silence the headphone too. */
	if (idt_family(c) == IDT_9200) {
		struct hda_widget *w = hda_widget(c, 0x08);

		if (w)
			w->eapd = 0;
		for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
			if (c->spec.out[i].pin == 0x08)
				c->spec.out[i].eapd = 0;
		hda_set_eapd(c, 0x08, 1);
	}
	idt_power_map(c);
	idt_apply_gpio(c, m, idt_hp_present(c));
	return 0;
}

static int idt_init(struct hda_codec *c)
{
	const struct idt_model *m;
	int fam;

	if (idt_is_vbox(c))
		return hda_vm_driver.init(c);

	m = idt_lookup(c);
	fam = idt_family(c);
	/* everything below is lost on a codec or controller reset */
	if (m && m->pins)
		hda_apply_pincfgs(c, m->pins);
	idt_core_verbs(c, m);
	if (m && m->verbs)
		hda_apply_verbs(c, m->verbs);
	if (fam == IDT_92HD71)
		idt_92hd71_unmute(c);
	if (fam == IDT_9200)
		hda_set_eapd(c, 0x08, 1);
	idt_power_map(c);
	idt_apply_gpio(c, m, idt_hp_present(c));
	return 0;
}

static void idt_automute(struct hda_codec *c, int hp_present)
{
	if (idt_is_vbox(c)) {
		hda_vm_driver.automute(c, hp_present);
		return;
	}
	/* speaker amplifier GPIO follows the speaker, LEDs the master */
	idt_apply_gpio(c, idt_lookup(c), hp_present);
}

static void idt_unsol(struct hda_codec *c, uint32_t res)
{
	if (idt_is_vbox(c)) {
		hda_vm_driver.unsol(c, res);
		return;
	}
	/* GPIO events (HP Mini vref, Dell dock headphone): re-evaluate
	 * which outputs play */
	hda_codec_update_routing(c);
}

static void idt_playback_hook(struct hda_codec *c, int start)
{
	if (idt_is_vbox(c)) {
		if (hda_vm_driver.playback_hook)
			hda_vm_driver.playback_hook(c, start);
		return;
	}
	if (start)
		idt_apply_gpio(c, idt_lookup(c), idt_hp_present(c));
}

const struct hda_vendor_driver hda_idt_driver = {
	.name = "idt",
	.match = idt_match,
	.pre_parse = idt_pre_parse,
	.post_parse = idt_post_parse,
	.init = idt_init,
	.automute = idt_automute,
	.unsol = idt_unsol,
	.playback_hook = idt_playback_hook,
	.probe = 0,
	.remove = 0,
};
