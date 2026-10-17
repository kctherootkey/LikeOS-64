// LikeOS HD Audio - Conexant / Synaptics codecs.
//
// Covers the CX2054x/5x (Venice, Waikiki, Hermosa), the CX2058x/9x
// "Pebble" family, the CX2063x/4x/5x/6x, CX2072x, CX2075x, CX8070/CX8200,
// CX11970 and the Synaptics-branded SN6140/SN6180 parts.  All of them are
// parsed by the generic code from the BIOS pin configuration; what this
// file adds is what the configuration does not say:
//
//  * board fixups keyed by PCI/codec subsystem id (ThinkPad docks, HP
//    EliteBook/ProBook docks, HP Spectre top speaker, Toshiba, Compaq,
//    Lemote, Acer digital microphones, ...),
//  * EAPD: chips with one or two EAPD-capable pins drive the amplifier of
//    every output from them, so those stay on permanently and the generic
//    per-output switching must not touch them; newer chips (three or more)
//    have one EAPD per pin and the generic code switches them with the pin,
//  * GPIO-driven mute/mic-mute LEDs on HP, the EAPD-as-LED trick of the
//    IdeaPad Z560 and boards whose speaker amplifier hangs off a GPIO,
//  * amplifier-range clamps on the analog loopback mixers of the old chips
//    (more than 0 dB there distorts on many laptops),
//  * the CX8070/SN6140 headset-type recognition (micbias comparator trim
//    and headset mic VREF after a plug).
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>
#include <kernel/hal/lapic.h>

/* ---- chip table ---------------------------------------------------------- */

enum cx_family {
	CX_FAM_5045 = 0, /* CX20549 Venice */
	CX_FAM_5047, /* CX20551 Waikiki */
	CX_FAM_5051, /* CX20561 Hermosa */
	CX_FAM_5066, /* everything newer: Pebble and successors */
};

#define CX_FAMBIT(f) (1u << (f))
#define CX_FAM_ALL 0u

/* chip flags */
#define CXC_HEADSET_RECOG (1u << 0) /* CX8070 / SN6140 headset detection */
#define CXC_SYNAPTICS (1u << 1) /* sold under the Synaptics name */
#define CXC_POWER_SAVE (1u << 2) /* widget power saving is safe */

struct cx_chip {
	uint32_t id;
	const char *name;
	uint8_t family;
	uint8_t flags;
};

static const struct cx_chip cx_chips[] = {
	{ 0x14f11f86, "CX8070", CX_FAM_5066, CXC_HEADSET_RECOG },
	{ 0x14f11f87, "SN6140", CX_FAM_5066,
	  CXC_HEADSET_RECOG | CXC_SYNAPTICS },
	{ 0x14f12008, "CX8200", CX_FAM_5066, 0 },
	{ 0x14f120d0, "CX11970", CX_FAM_5066, 0 },
	{ 0x14f120d1, "SN6180", CX_FAM_5066, CXC_SYNAPTICS },
	{ 0x14f15045, "CX20549 (Venice)", CX_FAM_5045, 0 },
	{ 0x14f15047, "CX20551 (Waikiki)", CX_FAM_5047, 0 },
	{ 0x14f15051, "CX20561 (Hermosa)", CX_FAM_5051, 0 },
	{ 0x14f15066, "CX20582 (Pebble)", CX_FAM_5066, 0 },
	{ 0x14f15067, "CX20583 (Pebble HSF)", CX_FAM_5066, 0 },
	{ 0x14f15068, "CX20584", CX_FAM_5066, 0 },
	{ 0x14f15069, "CX20585", CX_FAM_5066, 0 },
	{ 0x14f1506c, "CX20588", CX_FAM_5066, 0 },
	{ 0x14f1506e, "CX20590", CX_FAM_5066, 0 },
	{ 0x14f15097, "CX20631", CX_FAM_5066, 0 },
	{ 0x14f15098, "CX20632", CX_FAM_5066, 0 },
	{ 0x14f150a1, "CX20641", CX_FAM_5066, 0 },
	{ 0x14f150a2, "CX20642", CX_FAM_5066, 0 },
	{ 0x14f150ab, "CX20651", CX_FAM_5066, 0 },
	{ 0x14f150ac, "CX20652", CX_FAM_5066, 0 },
	{ 0x14f150b8, "CX20664", CX_FAM_5066, 0 },
	{ 0x14f150b9, "CX20665", CX_FAM_5066, 0 },
	{ 0x14f150f1, "CX21722", CX_FAM_5066, 0 },
	{ 0x14f150f2, "CX20722", CX_FAM_5066, CXC_POWER_SAVE },
	{ 0x14f150f3, "CX21724", CX_FAM_5066, 0 },
	{ 0x14f150f4, "CX20724", CX_FAM_5066, 0 },
	{ 0x14f1510f, "CX20751/2", CX_FAM_5066, 0 },
	{ 0x14f15110, "CX20751/2", CX_FAM_5066, 0 },
	{ 0x14f15111, "CX20753/4", CX_FAM_5066, 0 },
	{ 0x14f15113, "CX20755", CX_FAM_5066, 0 },
	{ 0x14f15114, "CX20756", CX_FAM_5066, 0 },
	{ 0x14f15115, "CX20757", CX_FAM_5066, 0 },
	{ 0x14f151d7, "CX20952", CX_FAM_5066, 0 },
	{ 0, 0, 0, 0 }
};

static const struct cx_chip *cx_find_chip(uint32_t id)
{
	int i;

	for (i = 0; cx_chips[i].id; i++)
		if (cx_chips[i].id == id)
			return &cx_chips[i];
	return 0;
}

static int cx_family(const struct hda_codec *c)
{
	const struct cx_chip *chip = cx_find_chip(c->vendor_id);

	return chip ? chip->family : CX_FAM_5066;
}

static uint32_t cx_chip_flags(const struct hda_codec *c)
{
	const struct cx_chip *chip = cx_find_chip(c->vendor_id);

	return chip ? chip->flags : 0;
}

/* ---- board models ---------------------------------------------------------- */

/* model quirks */
#define CXQ_CAP_MIX_AMP (1u << 0) /* 5045: clamp mixer 0x17 to 0 dB */
#define CXQ_CAP_MIX_AMP_5047 (1u << 1) /* 5047: clamp mixer 0x10 to 0 dB */
#define CXQ_INC_MIC_BOOST (1u << 2) /* 5066: wider mic boost on 0x17 */
#define CXQ_STEREO_DMIC (1u << 3) /* digital mic is a stereo pair */
#define CXQ_MUTE_LED_EAPD (1u << 4) /* EAPD of 0x1b drives the mute LED */
#define CXQ_HP_GATE_MIC (1u << 5) /* mic jack 0x19 sensed with HP 0x16 */
#define CXQ_HEADPHONE_MIC (1u << 6) /* HP jack doubles as mic input */
#define CXQ_HEADSET_MIC (1u << 7) /* headset (combo) jack */
#define CXQ_THINKPAD (1u << 8) /* ThinkPad: EC mute LED, static EAPD */
#define CXQ_OLPC (1u << 9) /* OLPC XO-1.5 DC-coupled mic */
#define CXQ_STATIC_EAPD (1u << 10) /* never switch EAPD dynamically */

struct cx_model {
	uint32_t families; /* CX_FAMBIT() mask, 0 = every family */
	uint32_t subsys; /* (vendor << 16) | device */
	uint32_t mask; /* 0 = exact, 0xffff0000 = vendor only */
	const char *name;
	const struct hda_pincfg *pins;
	const struct hda_verb *verbs;
	uint32_t quirks; /* CXQ_* */
	uint8_t gpio_static; /* GPIO bits driven high permanently */
	uint8_t led_mute; /* GPIO bit lit while the master is muted */
	uint8_t led_micmute; /* GPIO bit of the mic-mute LED */
	uint8_t spk_amp_gpio; /* GPIO bit enabling the speaker amplifier */
};

/* ThinkPad X200 & co with CX20561 */
static const struct hda_pincfg cx_pins_lenovo_x200[] = {
	{ 0x16, 0x042140ff }, /* headphone, sequence moved to the end */
	{ 0x17, 0x21a11000 }, /* dock microphone */
	{ 0x19, 0x2121103f }, /* dock headphone */
	{ 0x1c, 0x21440100 }, /* dock S/PDIF out */
	{ 0, 0 }
};

/* ThinkPad T400/T410/T510/T420/T520/X201/X220 with CX20585/CX20590 */
static const struct hda_pincfg cx_pins_lenovo_tp410[] = {
	{ 0x19, 0x042110ff }, /* headphone, sequence moved to the end */
	{ 0x1a, 0x21a190f0 }, /* dock microphone */
	{ 0x1c, 0x212140ff }, /* dock headphone */
	{ 0, 0 }
};

/* Lemote A1004/A1205 with CX20582 */
static const struct hda_pincfg cx_pins_lemote[] = {
	{ 0x1a, 0x90a10020 }, /* internal microphone */
	{ 0x1b, 0x03a11020 }, /* external microphone */
	{ 0x1d, 0x400101f0 }, /* not connected */
	{ 0x1e, 0x40a701f0 },
	{ 0x20, 0x404501f0 },
	{ 0x22, 0x404401f0 },
	{ 0x23, 0x40a701f0 },
	{ 0, 0 }
};

/* SuoWoSi / South-holding JS201D with SN6140 */
static const struct hda_pincfg cx_pins_sws_js201d[] = {
	{ 0x16, 0x03211040 }, /* headphone */
	{ 0x17, 0x91170110 }, /* class-D speaker */
	{ 0x18, 0x95a70130 }, /* internal microphone */
	{ 0x19, 0x03a11020 }, /* headset microphone */
	{ 0x1a, 0x40f001f0 },
	{ 0x21, 0x40f001f0 },
	{ 0, 0 }
};

/* Compaq CQ60: 0x17 claims to be a microphone, the real one is 0x1d */
static const struct hda_pincfg cx_pins_compaq_cq60[] = {
	{ 0x17, 0x400001f0 },
	{ 0x1d, 0x97a70120 },
	{ 0, 0 }
};

static const struct hda_pincfg cx_pins_toshiba_p105[] = {
	{ 0x10, 0x961701f0 }, /* speaker / headphone */
	{ 0x12, 0x02a1901e }, /* external microphone */
	{ 0x14, 0x95a70110 }, /* internal microphone */
	{ 0, 0 }
};

static const struct hda_pincfg cx_pins_hp_530[] = {
	{ 0x12, 0x90a60160 }, /* internal microphone */
	{ 0, 0 }
};

/* HP EliteBook/ProBook docking station: line out and line in */
static const struct hda_pincfg cx_pins_hp_dock[] = {
	{ 0x16, 0x21011020 }, /* dock line out */
	{ 0x18, 0x2181103f }, /* dock line in */
	{ 0, 0 }
};

/* HP Spectre x360: second speaker pair on top */
static const struct hda_pincfg cx_pins_hp_spectre[] = {
	{ 0x1d, 0x91170111 },
	{ 0, 0 }
};

/* HP desktops whose front mic jack has no working presence detect */
static const struct hda_pincfg cx_pins_hp_mic_no_presence[] = {
	{ 0x1a, 0x02a1113c },
	{ 0, 0 }
};

/* ASUS laptops using the headphone jack as headphone+mic */
static const struct hda_pincfg cx_pins_asus_hp_mic[] = {
	{ 0x18, 0x03a1913d }, /* headphone mic, no own presence detect */
	{ 0, 0 }
};

#define F5045 CX_FAMBIT(CX_FAM_5045)
#define F5047 CX_FAMBIT(CX_FAM_5047)
#define F5051 CX_FAMBIT(CX_FAM_5051)
#define F5066 CX_FAMBIT(CX_FAM_5066)
#define SSID(v, d) (((uint32_t)(v) << 16) | (uint32_t)(d))
#define VENDOR_ONLY 0xffff0000u

/*
 * First match wins, so exact entries precede vendor-wide ones of the same
 * family.
 */
static const struct cx_model cx_models[] = {
	/* -- CX20549 ---------------------------------------------------- */
	{ F5045, SSID(0x103c, 0x30d5), 0, "HP 530", cx_pins_hp_530, 0,
	  CXQ_CAP_MIX_AMP, 0, 0, 0, 0 },
	{ F5045, SSID(0x1179, 0xff31), 0, "Toshiba P105", cx_pins_toshiba_p105,
	  0, 0, 0, 0, 0, 0 },
	{ F5045, SSID(0x103c, 0), VENDOR_ONLY, "HP", 0, 0, CXQ_CAP_MIX_AMP, 0,
	  0, 0, 0 },
	{ F5045, SSID(0x1631, 0), VENDOR_ONLY, "Packard Bell", 0, 0,
	  CXQ_CAP_MIX_AMP, 0, 0, 0, 0 },
	{ F5045, SSID(0x1734, 0), VENDOR_ONLY, "Fujitsu", 0, 0,
	  CXQ_CAP_MIX_AMP, 0, 0, 0, 0 },
	{ F5045, SSID(0x17aa, 0), VENDOR_ONLY, "Lenovo", 0, 0, CXQ_CAP_MIX_AMP,
	  0, 0, 0, 0 },

	/* -- CX20551 ---------------------------------------------------- */
	{ F5047, SSID(0x103c, 0), VENDOR_ONLY, "HP", 0, 0, CXQ_CAP_MIX_AMP_5047,
	  0, 0, 0, 0 },

	/* -- CX20561 ---------------------------------------------------- */
	{ F5051, SSID(0x103c, 0x360b), 0, "Compaq CQ60", cx_pins_compaq_cq60,
	  0, 0, 0, 0, 0, 0 },
	{ F5051, SSID(0x17aa, 0x20f2), 0, "Lenovo X200", cx_pins_lenovo_x200,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },

	/* -- CX20582 and newer ------------------------------------------- */
	{ F5066, SSID(0x1025, 0x0543), 0, "Acer Aspire One 522", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	/* Aspire 3830TG/4830T: the speaker amplifier needs GPIO0 high */
	{ F5066, SSID(0x1025, 0x054c), 0, "Acer Aspire 3830TG", 0, 0,
	  CXQ_STEREO_DMIC, 0x01, 0, 0, 0 },
	{ F5066, SSID(0x1025, 0x054f), 0, "Acer Aspire 4830T", 0, 0,
	  CXQ_STEREO_DMIC, 0x01, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x8079), 0, "HP EliteBook 840 G3",
	  cx_pins_hp_dock, 0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x807c), 0, "HP EliteBook 820 G3",
	  cx_pins_hp_dock, 0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x80fd), 0, "HP ProBook 640 G2", cx_pins_hp_dock,
	  0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x8115), 0, "HP Z1 Gen3", 0, 0, CXQ_HP_GATE_MIC,
	  0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x814f), 0, "HP ZBook 15u G3", 0, 0, 0, 0, 0x01,
	  0x02, 0 },
	{ F5066, SSID(0x103c, 0x8174), 0, "HP Spectre x360",
	  cx_pins_hp_spectre, 0, 0, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x822e), 0, "HP ProBook 440 G4", 0, 0, 0, 0, 0x01,
	  0x02, 0 },
	{ F5066, SSID(0x103c, 0x828c), 0, "HP EliteBook 840 G4",
	  cx_pins_hp_dock, 0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x8299), 0, "HP 800 G3 SFF",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x829a), 0, "HP 800 G3 DM",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x82b4), 0, "HP ProDesk 600 G3",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x836e), 0, "HP ProBook 455 G5", 0, 0, 0, 0, 0x01,
	  0x02, 0 },
	{ F5066, SSID(0x103c, 0x837f), 0, "HP ProBook 470 G5", 0, 0, 0, 0, 0x01,
	  0x02, 0 },
	{ F5066, SSID(0x103c, 0x83b2), 0, "HP EliteBook 840 G5",
	  cx_pins_hp_dock, 0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x83b3), 0, "HP EliteBook 830 G5",
	  cx_pins_hp_dock, 0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x83d3), 0, "HP ProBook 640 G4", cx_pins_hp_dock,
	  0, 0, 0, 0x01, 0x02, 0 },
	{ F5066, SSID(0x103c, 0x8402), 0, "HP ProBook 645 G4", 0, 0, 0, 0, 0x01,
	  0x02, 0 },
	{ F5066, SSID(0x103c, 0x8427), 0, "HP ZBook Studio G5", 0, 0, 0, 0,
	  0x10, 0x20, 0 },
	{ F5066, SSID(0x103c, 0x844f), 0, "HP ZBook Studio G5", 0, 0, 0, 0,
	  0x10, 0x20, 0 },
	{ F5066, SSID(0x103c, 0x8455), 0, "HP Z2 G4",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x8456), 0, "HP Z2 G4 SFF",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x8457), 0, "HP Z2 G4 mini",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x103c, 0x8458), 0, "HP Z2 G4 mini premium",
	  cx_pins_hp_mic_no_presence, 0, CXQ_HEADSET_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x1043, 0x138d), 0, "ASUS", cx_pins_asus_hp_mic, 0,
	  CXQ_HEADPHONE_MIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x14f1, 0x0265), 0, "SWS JS201D", cx_pins_sws_js201d, 0,
	  0, 0, 0, 0, 0 },
	{ F5066, SSID(0x152d, 0x0833), 0, "OLPC XO-1.5", 0, 0, CXQ_OLPC, 0, 0,
	  0, 0 },
	{ F5066, SSID(0x17aa, 0x20f2), 0, "Lenovo T400", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x215e), 0, "Lenovo T410", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x215f), 0, "Lenovo T510", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x21ce), 0, "Lenovo T420", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x21cf), 0, "Lenovo T520", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x21d2), 0, "Lenovo T420s", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x21da), 0, "Lenovo X220", cx_pins_lenovo_tp410,
	  0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x21db), 0, "Lenovo X220 Tablet",
	  cx_pins_lenovo_tp410, 0, CXQ_THINKPAD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x38af), 0, "Lenovo IdeaPad Z560", 0, 0,
	  CXQ_MUTE_LED_EAPD, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x3905), 0, "Lenovo G50-30", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x390b), 0, "Lenovo G50-80", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x3975), 0, "Lenovo U300s", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x3977), 0, "Lenovo IdeaPad U310", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x3978), 0, "Lenovo G50-70", 0, 0,
	  CXQ_STEREO_DMIC, 0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0x397b), 0, "Lenovo S205", 0, 0, CXQ_STEREO_DMIC,
	  0, 0, 0, 0 },
	{ F5066, SSID(0x17aa, 0), VENDOR_ONLY, "ThinkPad", 0, 0, CXQ_THINKPAD,
	  0, 0, 0, 0 },
	{ F5066, SSID(0x1c06, 0x2011), 0, "Lemote A1004", cx_pins_lemote, 0,
	  CXQ_INC_MIC_BOOST, 0, 0, 0, 0 },
	{ F5066, SSID(0x1c06, 0x2012), 0, "Lemote A1205", cx_pins_lemote, 0, 0,
	  0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

static uint32_t cx_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	/* config space: vendor in the low half, device in the high half */
	return ((v & 0xffffu) << 16) | (v >> 16);
}

static int cx_ssid_match(uint32_t ssid, uint32_t want, uint32_t mask)
{
	if (ssid == 0)
		return 0;
	if (mask == 0)
		mask = 0xffffffffu;
	return (ssid & mask) == (want & mask);
}

static const struct cx_model *cx_lookup_ssid(int family, uint32_t ssid)
{
	int i;

	for (i = 0; cx_models[i].subsys; i++) {
		const struct cx_model *m = &cx_models[i];

		if (m->families && !(m->families & CX_FAMBIT(family)))
			continue;
		if (cx_ssid_match(ssid, m->subsys, m->mask))
			return m;
	}
	return 0;
}

/* The PCI subsystem decides; the codec's own id is the fallback. */
static const struct cx_model *cx_lookup(const struct hda_codec *c)
{
	int fam = cx_family(c);
	const struct cx_model *m = cx_lookup_ssid(fam, cx_pci_ssid(c));

	if (!m && c->subsystem_id != cx_pci_ssid(c))
		m = cx_lookup_ssid(fam, c->subsystem_id);
	return m;
}

/* ---- helpers ------------------------------------------------------------------ */

static void cx_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static void cx_gpio_update(struct hda_codec *c, uint32_t bits, uint32_t dir,
			   uint32_t data)
{
	uint32_t nmask = (c->gpio_mask | bits) & 0xff;
	uint32_t ndir = ((c->gpio_dir & ~bits) | (dir & bits)) & 0xff;
	uint32_t ndata = ((c->gpio_data & ~bits) | (data & bits)) & 0xff;

	hda_gpio_set(c, nmask, ndir, ndata);
	c->gpio_mask = nmask;
	c->gpio_dir = ndir;
	c->gpio_data = ndata;
}

static void cx_override_amp_caps(struct hda_codec *c, uint8_t nid, int dir,
				 uint32_t caps)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w)
		return;
	if (dir == HDA_OUTPUT)
		w->amp_out_caps = caps;
	else
		w->amp_in_caps = caps;
	w->caps |= AC_WCAP_AMP_OVRD;
}

#define CX_AMPCAPS(ofs, steps, size, mute)                       \
	(((uint32_t)(ofs) << AC_AMPCAP_OFFSET_SHIFT) |           \
	 ((uint32_t)(steps) << AC_AMPCAP_NUM_STEPS_SHIFT) |      \
	 ((uint32_t)(size) << AC_AMPCAP_STEP_SIZE_SHIFT) |       \
	 ((mute) ? AC_AMPCAP_MUTE : 0))

static int cx_is_pin(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	return w && w->type == AC_WID_PIN;
}

/* EAPD-capable pins in node order; returns how many (at most `max'). */
static int cx_eapd_pins(struct hda_codec *c, uint8_t *out, int max)
{
	unsigned int i;
	int n = 0;

	if (!c->w)
		return 0;
	for (i = 0; i < c->num_nodes && n < max; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		if (!(w->pincap & AC_PINCAP_EAPD))
			continue;
		out[n++] = w->nid;
	}
	return n;
}

/*
 * With at most two EAPD pins the EAPD bit is global on these chips: it
 * powers the amplifier of every output, so it has to stay on no matter
 * which output plays.  From three on, every pin has its own.
 */
static int cx_static_eapd(struct hda_codec *c, const struct cx_model *m)
{
	uint8_t pins[8];
	int n = cx_eapd_pins(c, pins, 8);

	if (m && (m->quirks & CXQ_STATIC_EAPD))
		return 1;
	if (m && (m->quirks & CXQ_MUTE_LED_EAPD))
		return 0;
	return n <= 2;
}

/* ---- mute LEDs ---------------------------------------------------------------- */

static void cx_update_mute_led(struct hda_codec *c, const struct cx_model *m)
{
	int muted = c->spec.master_mute != 0;

	if (!m)
		return;
	if (m->led_mute)
		cx_gpio_update(c, m->led_mute, m->led_mute,
			       muted ? m->led_mute : 0);
	if (m->quirks & CXQ_MUTE_LED_EAPD) {
		/* the IdeaPad Z560 wired its mute LED to the EAPD line of
		 * port 0x1b: lit while muted */
		if (cx_is_pin(c, 0x1b))
			hda_codec_write(c, 0x1b, AC_VERB_SET_EAPD_BTLENABLE,
					muted ? AC_EAPDBTL_EAPD : 0);
	}
}

static void cx_init_leds(struct hda_codec *c, const struct cx_model *m)
{
	uint32_t leds;

	if (!m)
		return;
	leds = (uint32_t)m->led_mute | m->led_micmute;
	if (leds)
		/* both LEDs off: nothing muted at start */
		cx_gpio_update(c, leds, leds, 0);
	cx_update_mute_led(c, m);
}

/* ---- CX8070 / SN6140 headset recognition --------------------------------- */

#define CX_HS_NID_VENDOR 0x1c
#define CX_HS_NID_HP 0x16
#define CX_HS_NID_MIC 0x19

static void cx_headset_trim(struct hda_codec *c)
{
	uint32_t sense;

	/* micbias output current comparator threshold 66% -> 55%: some
	 * headsets are otherwise not recognised */
	hda_codec_write(c, CX_HS_NID_VENDOR, 0x320, 0x010);
	/* DFET off voltage -1.2 V -> -0.8 V, headset micbias trim 2.2k ->
	 * 2.0k */
	hda_codec_write(c, CX_HS_NID_VENDOR, 0x3b0, 0xe10);
	/* a headset already plugged at boot keeps its mic bias */
	sense = hda_codec_read(c, CX_HS_NID_MIC, AC_VERB_GET_PIN_SENSE, 0);
	if (sense != 0xffffffffu && (sense & AC_PINSENSE_PRESENCE))
		hda_pin_set_ctl(c, CX_HS_NID_MIC,
				AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	else
		hda_pin_set_ctl(c, CX_HS_NID_MIC, AC_PINCTL_IN_EN);
}

/* After a plug: wait for the headset-type detection, then bias the mic
 * for a CTIA or OMTP headset; plain headphones leave the mic unbiased. */
static void cx_headset_plugged(struct hda_codec *c)
{
	uint32_t val = 0;
	int tries;

	for (tries = 0; tries < 3; tries++) {
		val = hda_codec_read(c, CX_HS_NID_VENDOR, 0xca0, 0);
		if (val != 0xffffffffu && (val & 0x080))
			break;
		lapic_delay_ms(20);
	}
	val = hda_codec_read(c, CX_HS_NID_VENDOR, 0xcb0, 0);
	if (val == 0xffffffffu)
		return;
	if (val & 0x800) /* CTIA */
		hda_pin_set_ctl(c, CX_HS_NID_MIC,
				AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	else if (val & 0x400) /* OMTP */
		hda_pin_set_ctl(c, CX_HS_NID_MIC,
				AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
}

static void cx_headset_update(struct hda_codec *c, int hp_present)
{
	uint32_t mic;

	if (!hp_present) {
		/* unplugged: drop the bias */
		hda_pin_set_ctl(c, CX_HS_NID_MIC, AC_PINCTL_IN_EN);
		return;
	}
	mic = hda_codec_read(c, CX_HS_NID_MIC, AC_VERB_GET_PIN_SENSE, 0);
	if (mic != 0xffffffffu && (mic & AC_PINSENSE_PRESENCE))
		cx_headset_plugged(c);
}

/* ---- EAPD ---------------------------------------------------------------------- */

static void cx_eapd_all(struct hda_codec *c, const struct cx_model *m, int on)
{
	uint8_t pins[8];
	int n = cx_eapd_pins(c, pins, 8);
	int i;

	for (i = 0; i < n; i++) {
		if (m && (m->quirks & CXQ_MUTE_LED_EAPD) && pins[i] == 0x1b)
			continue; /* that one is the LED */
		hda_set_eapd(c, pins[i], on);
	}
}

/* Keep the generic code's hands off the shared EAPD lines. */
static void cx_take_eapd(struct hda_codec *c)
{
	uint8_t pins[8];
	int n = cx_eapd_pins(c, pins, 8);
	int i, j;

	for (i = 0; i < n; i++) {
		struct hda_widget *w = hda_widget(c, pins[i]);

		if (w)
			w->eapd = 0;
		for (j = 0; j < c->spec.nout && j < HDA_MAX_OUTPUTS; j++)
			if (c->spec.out[j].pin == pins[i])
				c->spec.out[j].eapd = 0;
	}
}

/* ---- speaker amplifier on a GPIO -------------------------------------------- */

static int cx_any_hp_present(struct hda_codec *c)
{
	int i;

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if ((c->spec.out[i].type == AUDIO_OUT_HEADPHONE ||
		     c->spec.out[i].type == AUDIO_OUT_HEADSET) &&
		    c->spec.out[i].jack_detect && c->spec.out[i].present)
			return 1;
	return 0;
}

static void cx_update_spk_amp(struct hda_codec *c, const struct cx_model *m,
			      int hp_present)
{
	int on;

	if (!m || !m->spk_amp_gpio)
		return;
	on = !hp_present || !c->spec.auto_mute;
	cx_gpio_update(c, m->spk_amp_gpio, m->spk_amp_gpio,
		       on ? m->spk_amp_gpio : 0);
}

/* ---- driver callbacks ----------------------------------------------------------- */

static int cx_match(struct hda_codec *c)
{
	return (c->vendor_id >> 16) == 0x14f1 && c->afg != 0;
}

static int cx_pre_parse(struct hda_codec *c)
{
	const struct cx_chip *chip = cx_find_chip(c->vendor_id);
	const struct cx_model *m = cx_lookup(c);
	int fam = cx_family(c);
	unsigned int i;

	if (chip) {
		cx_copy_name(c->vendor_name, sizeof(c->vendor_name),
			     (chip->flags & CXC_SYNAPTICS) ? "Synaptics" :
							     "Conexant");
		cx_copy_name(c->chip_name, sizeof(c->chip_name), chip->name);
	} else {
		cx_copy_name(c->vendor_name, sizeof(c->vendor_name),
			     "Conexant");
		ksnprintf(c->chip_name, sizeof(c->chip_name), "CX%04x",
			  (unsigned int)(c->vendor_id & 0xffff));
	}

	/* chip-wide amplifier fixes */
	switch (fam) {
	case CX_FAM_5051: {
		/* the DAC output amplifiers can mute although their caps
		 * do not say so; the mute is needed for the master switch */
		static const uint8_t dacs[] = { 0x10, 0x11 };

		for (i = 0; i < sizeof(dacs); i++) {
			struct hda_widget *w = hda_widget(c, dacs[i]);
			uint32_t caps;

			if (!w || w->type != AC_WID_AUD_OUT)
				continue;
			caps = hda_amp_caps(c, dacs[i], HDA_OUTPUT);
			cx_override_amp_caps(c, dacs[i], HDA_OUTPUT,
					     caps | AC_AMPCAP_MIN_MUTE);
		}
		break;
	}
	default:
		break;
	}

	if (m) {
		if (m->pins)
			hda_apply_pincfgs(c, m->pins);
		if (m->verbs)
			hda_apply_verbs(c, m->verbs);
		/* loopback mixer gain beyond 0 dB distorts on these */
		if (m->quirks & CXQ_CAP_MIX_AMP)
			cx_override_amp_caps(c, 0x17, HDA_INPUT,
					     CX_AMPCAPS(0x14, 0x14, 0x05, 1));
		if (m->quirks & CXQ_CAP_MIX_AMP_5047)
			cx_override_amp_caps(c, 0x10, HDA_INPUT,
					     CX_AMPCAPS(0x17, 0x17, 0x05, 1));
		if (m->quirks & CXQ_INC_MIC_BOOST)
			cx_override_amp_caps(c, 0x17, HDA_OUTPUT,
					     CX_AMPCAPS(0x03, 0x04, 0x27, 0));
		if (m->gpio_static)
			cx_gpio_update(c, m->gpio_static, m->gpio_static,
				       m->gpio_static);
		if (m->spk_amp_gpio)
			cx_gpio_update(c, m->spk_amp_gpio, m->spk_amp_gpio,
				       m->spk_amp_gpio);
		cx_init_leds(c, m);
		kprintf("[snd] hda: codec %d: %s %s, board fixup \"%s\"\n",
			c->addr, c->vendor_name, c->chip_name, m->name);
	} else {
		kprintf("[snd] hda: codec %d: %s %s, BIOS pin configuration\n",
			c->addr, c->vendor_name, c->chip_name);
	}

	/* the beeper, if any, for the generic code */
	if (!c->spec.beep_nid && c->w) {
		for (i = 0; i < c->num_nodes; i++) {
			if (c->w[i].type == AC_WID_BEEP) {
				c->spec.beep_nid = c->w[i].nid;
				break;
			}
		}
	}
	return 0;
}

static int cx_post_parse(struct hda_codec *c)
{
	const struct cx_model *m = cx_lookup(c);

	if (cx_static_eapd(c, m)) {
		cx_take_eapd(c);
		cx_eapd_all(c, m, 1);
	}
	if (m && (m->quirks & CXQ_MUTE_LED_EAPD)) {
		/* port 0x1b is the LED, not an amplifier */
		struct hda_widget *w = hda_widget(c, 0x1b);
		int i;

		if (w)
			w->eapd = 0;
		for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
			if (c->spec.out[i].pin == 0x1b)
				c->spec.out[i].eapd = 0;
	}
	if (cx_chip_flags(c) & CXC_HEADSET_RECOG) {
		/* the mic pin gives no event of its own; the HP event
		 * re-evaluates it, so jack sensing must stay on */
		if (!c->unsol_capable)
			c->spec.jack_poll = 1;
	}
	cx_update_mute_led(c, m);
	return 0;
}

static int cx_init(struct hda_codec *c)
{
	const struct cx_model *m = cx_lookup(c);

	/* a controller reset clears the GPIO lines: re-drive them */
	if (m) {
		if (m->pins)
			hda_apply_pincfgs(c, m->pins);
		if (m->verbs)
			hda_apply_verbs(c, m->verbs);
		if (m->gpio_static)
			cx_gpio_update(c, m->gpio_static, m->gpio_static,
				       m->gpio_static);
		cx_init_leds(c, m);
		cx_update_spk_amp(c, m, cx_any_hp_present(c));
	}
	if (cx_static_eapd(c, m))
		cx_eapd_all(c, m, 1);
	if (cx_chip_flags(c) & CXC_HEADSET_RECOG)
		cx_headset_trim(c);
	return 0;
}

static void cx_automute(struct hda_codec *c, int hp_present)
{
	const struct cx_model *m = cx_lookup(c);

	/* static EAPD lines stay on even when the speaker path is muted:
	 * the headphone amplifier hangs off the same line */
	if (cx_static_eapd(c, m))
		cx_eapd_all(c, m, 1);
	cx_update_spk_amp(c, m, hp_present);
	cx_update_mute_led(c, m);
	if (cx_chip_flags(c) & CXC_HEADSET_RECOG)
		cx_headset_update(c, hp_present);
}

static void cx_unsol(struct hda_codec *c, uint32_t res)
{
	/* an event the generic code did not claim: on the headset chips
	 * it is the mic jack; re-check the headset state */
	if (cx_chip_flags(c) & CXC_HEADSET_RECOG) {
		uint32_t hp = hda_codec_read(c, CX_HS_NID_HP,
					     AC_VERB_GET_PIN_SENSE, 0);

		cx_headset_update(c, hp != 0xffffffffu &&
					     (hp & AC_PINSENSE_PRESENCE));
	}
	(void)res;
}

static void cx_playback_hook(struct hda_codec *c, int start)
{
	const struct cx_model *m;

	if (!start)
		return;
	m = cx_lookup(c);
	/* the amplifiers may have been dropped by a power transition */
	if (cx_static_eapd(c, m))
		cx_eapd_all(c, m, 1);
	cx_update_mute_led(c, m);
}

const struct hda_vendor_driver hda_conexant_driver = {
	.name = "conexant",
	.match = cx_match,
	.pre_parse = cx_pre_parse,
	.post_parse = cx_post_parse,
	.init = cx_init,
	.automute = cx_automute,
	.unsol = cx_unsol,
	.playback_hook = cx_playback_hook,
	.probe = 0,
	.remove = 0,
};
