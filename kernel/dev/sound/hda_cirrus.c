// LikeOS HD Audio - Cirrus Logic codecs.
//
//   CS4206/CS4207   MacBook/MacBook Pro/iMac 2009-2012 and some PCs;
//                   vendor processing widget 0x11
//   CS4208          MacBook Air 6,x / MacBook Pro 11,x / Mac mini 7,1;
//                   vendor widget 0x24
//   CS4210/CS4213   PC codecs (ChromeBox, Intel desktop boards); vendor widget
//                   0x0b / 0x09
//   CS8409          HDA bridge used by Dell Inspiron/Vostro/Latitude
//                   2020-2022 (Bullseye, Warlock, Cyborg, Odin, Dolphin).
//                   It has no analog section at all: its converters feed
//                   serial ports (ASP1/ASP2).  ASP2 drives the speaker
//                   amplifiers directly; ASP1 talks to one or two CS42L42
//                   headphone codecs that sit on an I2C bus mastered by the
//                   CS8409 and are programmed through its vendor widget
//                   (0x47).  Jack events of the CS42L42 arrive as a GPIO
//                   unsolicited response of the bridge.
//
// The Apple boards switch the speaker and headphone amplifiers with GPIOs
// (EAPD lines), which this driver flips with the headphone jack.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>
#include <kernel/hal/lapic.h>

/* ---- chips ------------------------------------------------------------------------ */

enum cs_kind {
	CS_420X = 0, /* CS4206/CS4207 */
	CS_4208,
	CS_4210,
	CS_4213,
	CS_8409,
	CS_UNKNOWN,
};

struct cs_chip {
	uint32_t id;
	const char *name;
	uint8_t kind;
	uint8_t vendor_nid;
};

static const struct cs_chip cs_chips[] = {
	{ 0x10134206, "CS4206", CS_420X, 0x11 },
	{ 0x10134207, "CS4207", CS_420X, 0x11 },
	{ 0x10134208, "CS4208", CS_4208, 0x24 },
	{ 0x10134210, "CS4210", CS_4210, 0x0b },
	{ 0x10134213, "CS4213", CS_4213, 0x09 },
	{ 0x10138409, "CS8409", CS_8409, 0x47 },
	{ 0, 0, 0, 0 }
};

static const struct cs_chip *cs_find_chip(uint32_t id)
{
	int i;

	for (i = 0; cs_chips[i].id; i++)
		if (cs_chips[i].id == id)
			return &cs_chips[i];
	return 0;
}

static int cs_kind(const struct hda_codec *c)
{
	const struct cs_chip *chip = cs_find_chip(c->vendor_id);

	return chip ? chip->kind : CS_UNKNOWN;
}

static uint8_t cs_vendor_nid(const struct hda_codec *c)
{
	const struct cs_chip *chip = cs_find_chip(c->vendor_id);

	return chip ? chip->vendor_nid : 0;
}

/* ---- common helpers ---------------------------------------------------------------- */

static void cs_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static uint32_t cs_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	return ((v & 0xffffu) << 16) | (v >> 16);
}

/* Coefficients of the vendor processing widget, raw verbs: the index
 * auto-increment is switched off on these parts. */
static uint32_t cs_coef_get(struct hda_codec *c, uint8_t nid, uint16_t idx)
{
	hda_codec_write(c, nid, AC_VERB_SET_COEF_INDEX, idx);
	return hda_codec_read(c, nid, AC_VERB_GET_PROC_COEF, 0);
}

static void cs_coef_set(struct hda_codec *c, uint8_t nid, uint16_t idx,
			uint16_t val)
{
	hda_codec_write(c, nid, AC_VERB_SET_COEF_INDEX, idx);
	hda_codec_write(c, nid, AC_VERB_SET_PROC_COEF, val);
}

static void cs_gpio_write(struct hda_codec *c, uint32_t mask, uint32_t dir,
			  uint32_t data)
{
	mask &= 0xff;
	dir &= 0xff;
	data &= 0xff;
	hda_gpio_set(c, mask, dir, data);
	c->gpio_mask = mask;
	c->gpio_dir = dir;
	c->gpio_data = data;
}

static int cs_pin_active(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w || w->type != AC_WID_PIN)
		return 0;
	return ((w->pin_cfg & AC_DEFCFG_PORT_CONN) >>
		AC_DEFCFG_PORT_CONN_SHIFT) != AC_JACK_PORT_NONE;
}

static void cs_override_amp_caps(struct hda_codec *c, uint8_t nid, int dir,
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

/* Cap a DAC's output amplifier at 0 dB (steps = offset). */
static void cs_dac_max_0db(struct hda_codec *c, uint8_t dac)
{
	uint32_t caps = hda_amp_caps(c, dac, HDA_OUTPUT);
	uint32_t ofs = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;

	if (!caps || caps == 0xffffffffu || !ofs)
		return;
	caps &= ~AC_AMPCAP_NUM_STEPS;
	caps |= ofs << AC_AMPCAP_NUM_STEPS_SHIFT;
	cs_override_amp_caps(c, dac, HDA_OUTPUT, caps);
}

static int cs_hp_present(struct hda_codec *c)
{
	int i;

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &c->spec.out[i];

		if ((o->type == AUDIO_OUT_HEADPHONE ||
		     o->type == AUDIO_OUT_HEADSET) && o->present &&
		    o->jack_detect)
			return 1;
	}
	return 0;
}

#define SSID(v, d) (((uint32_t)(v) << 16) | (uint32_t)(d))

/* ======================================================================
 * CS4206 / CS4207
 * ====================================================================== */

#define CS420X_VNID 0x11
#define CS_IDX_SPDIF_CTL 0x0001
#define CS_IDX_ADC_CFG 0x0002
#define CS_IDX_DAC_CFG 0x0003
#define CS_IDX_BEEP_CFG 0x0004
#define CS_DIG_OUT2_PIN 0x15
#define CS_DMIC1_PIN 0x0e
#define CS_DMIC2_PIN 0x12

/* Errata for C0/C1/C2 silicon: run the S/PDIF SRC blocks once so their
 * RAM does not leak current, then put everything back. */
static const struct hda_verb cs420x_errata_verbs[] = {
	{ 0x01, AC_VERB_SET_POWER_STATE, 0x00 },
	{ 0x11, AC_VERB_SET_PROC_STATE, 0x01 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0008 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x9999 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0017 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0xa412 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0001 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x0009 },
	{ 0x07, AC_VERB_SET_POWER_STATE, 0x00 }, /* S/PDIF receiver */
	{ 0x08, AC_VERB_SET_POWER_STATE, 0x00 }, /* S/PDIF transmitter */
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0017 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x2412 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0008 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x0000 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, 0x0001 },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x0008 },
	{ 0x11, AC_VERB_SET_PROC_STATE, 0x00 },
	{ 0, 0, 0 }
};

static const struct hda_verb cs420x_coef_verbs[] = {
	{ 0x11, AC_VERB_SET_PROC_STATE, 1 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, CS_IDX_DAC_CFG },
	/* DAC soft ramp, mute DACs on FIFO error, DAC high-pass filter,
	 * no coefficient index auto-increment */
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x002a | 0x0040 | 0x1000 | 0x0400 },
	{ 0x11, AC_VERB_SET_COEF_INDEX, CS_IDX_ADC_CFG },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x000a }, /* ADC soft ramp */
	{ 0x11, AC_VERB_SET_COEF_INDEX, CS_IDX_BEEP_CFG },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x0007 }, /* beep through the DACs */
	{ 0, 0, 0 }
};

static const struct hda_pincfg cs_mbp53_pins[] = {
	{ 0x09, 0x012b4050 }, { 0x0a, 0x90100141 }, { 0x0b, 0x90100140 },
	{ 0x0c, 0x018b3020 }, { 0x0d, 0x90a00110 }, { 0x0e, 0x400000f0 },
	{ 0x0f, 0x01cbe030 }, { 0x10, 0x014be060 }, { 0x12, 0x400000f0 },
	{ 0x15, 0x400000f0 }, { 0, 0 }
};
static const struct hda_pincfg cs_mbp55_pins[] = {
	{ 0x09, 0x012b4030 }, { 0x0a, 0x90100121 }, { 0x0b, 0x90100120 },
	{ 0x0c, 0x400000f0 }, { 0x0d, 0x90a00110 }, { 0x0e, 0x400000f0 },
	{ 0x0f, 0x400000f0 }, { 0x10, 0x014be040 }, { 0x12, 0x400000f0 },
	{ 0x15, 0x400000f0 }, { 0, 0 }
};
static const struct hda_pincfg cs_imac27_pins[] = {
	{ 0x09, 0x012b4050 }, { 0x0a, 0x90100140 }, { 0x0b, 0x90100142 },
	{ 0x0c, 0x018b3020 }, { 0x0d, 0x90a00110 }, { 0x0e, 0x400000f0 },
	{ 0x0f, 0x01cbe030 }, { 0x10, 0x014be060 }, { 0x12, 0x01ab9070 },
	{ 0x15, 0x400000f0 }, { 0, 0 }
};
static const struct hda_pincfg cs_mbp101_pins[] = {
	{ 0x0d, 0x40ab90f0 }, { 0x0e, 0x90a600f0 }, { 0x12, 0x50a600f0 },
	{ 0, 0 }
};
static const struct hda_pincfg cs_mba42_pins[] = {
	{ 0x09, 0x012b4030 }, /* headphone */
	{ 0x0a, 0x400000f0 },
	{ 0x0b, 0x90100120 }, /* speaker */
	{ 0x0c, 0x400000f0 },
	{ 0x0d, 0x90a00110 }, /* microphone */
	{ 0x0e, 0x400000f0 }, { 0x0f, 0x400000f0 }, { 0x10, 0x400000f0 },
	{ 0x12, 0x400000f0 }, { 0x15, 0x400000f0 }, { 0, 0 }
};
static const struct hda_verb cs_mbp81_verbs[] = {
	/* internal mic ADC2: right channel only, single ended */
	{ 0x11, AC_VERB_SET_COEF_INDEX, CS_IDX_ADC_CFG },
	{ 0x11, AC_VERB_SET_PROC_COEF, 0x102a },
	{ 0, 0, 0 }
};

/* CS4208 */
static const struct hda_verb cs4208_coef_verbs[] = {
	{ 0x01, AC_VERB_SET_POWER_STATE, 0x00 },
	{ 0x24, AC_VERB_SET_PROC_STATE, 0x01 },
	{ 0x24, AC_VERB_SET_COEF_INDEX, 0x0033 },
	{ 0x24, AC_VERB_SET_PROC_COEF, 0x0001 }, /* A1 ICS */
	{ 0x24, AC_VERB_SET_COEF_INDEX, 0x0034 },
	{ 0x24, AC_VERB_SET_PROC_COEF, 0x1c01 }, /* A1 on, threshold 300 mV */
	{ 0, 0, 0 }
};
static const struct hda_pincfg cs4208_mba6_pins[] = {
	{ 0x10, 0x032120f0 }, /* headphone */
	{ 0x11, 0x500000f0 },
	{ 0x12, 0x90100010 }, /* speaker */
	{ 0x13, 0x500000f0 }, { 0x14, 0x500000f0 }, { 0x15, 0x770000f0 },
	{ 0x16, 0x770000f0 }, { 0x17, 0x430000f0 },
	{ 0x18, 0x43ab9030 }, /* microphone */
	{ 0x19, 0x770000f0 }, { 0x1a, 0x770000f0 }, { 0x1b, 0x770000f0 },
	{ 0x1c, 0x90a00090 }, { 0x1d, 0x500000f0 }, { 0x1e, 0x500000f0 },
	{ 0x1f, 0x500000f0 }, { 0x20, 0x500000f0 }, { 0x21, 0x430000f0 },
	{ 0x22, 0x430000f0 }, { 0, 0 }
};
static const struct hda_pincfg cs4208_macmini_pins[] = {
	{ 0x18, 0x00ab9150 }, /* audio-in jack: no presence detect */
	{ 0x21, 0x004be140 }, /* S/PDIF: no presence detect */
	{ 0, 0 }
};

/* CS4210 */
static const struct hda_verb cs421x_coef_verbs[] = {
	{ 0x0b, AC_VERB_SET_PROC_STATE, 1 },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x01 },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x0001 }, /* no auto-increment */
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x02 },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x0002 }, /* ADC soft ramp */
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x03 },
	/* DAC soft ramp, mute on FIFO error, high-pass filter */
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x0002 | 0x0004 | 0x0008 },
	{ 0, 0, 0 }
};
/* rev A1 silicon: ADC performance, speaker mute on HP, clipping noise */
static const struct hda_verb cs421x_a1_verbs[] = {
	{ 0x0b, AC_VERB_SET_PROC_STATE, 0x01 },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x0006 },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x9999 },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x000a },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x14cb },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x0011 },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0xa2d0 },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x001a },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x02a9 },
	{ 0x0b, AC_VERB_SET_COEF_INDEX, 0x001b },
	{ 0x0b, AC_VERB_SET_PROC_COEF, 0x1006 },
	{ 0, 0, 0 }
};
static const struct hda_pincfg cs_cdb4210_pins[] = {
	{ 0x05, 0x0321401f }, { 0x06, 0x90170010 }, { 0x07, 0x03813031 },
	{ 0x08, 0xb7a70037 }, { 0x09, 0xb7a6003e }, { 0x0a, 0x034510f0 },
	{ 0, 0 }
};

/* ---- CS42xx board models -------------------------------------------------------- */

#define CSQ_GPIO13 (1u << 0) /* HP amp GPIO1, speaker amp GPIO3 */
#define CSQ_GPIO23 (1u << 1) /* HP amp GPIO2, speaker amp GPIO3 */
#define CSQ_GPIO0_SPK (1u << 2) /* CS4208 Macs: speaker amp on GPIO0 */
#define CSQ_INV_HP_JACK (1u << 3) /* HP jack sense reads backwards */
#define CSQ_SENSE_B (1u << 4) /* CS4210: SENSE_B mutes HP on S/PDIF */
#define CSQ_CODEC_SSID (1u << 5) /* match the codec subsystem id */

struct cs_model {
	uint8_t kind;
	uint32_t subsys;
	uint32_t mask;
	const char *name;
	const struct hda_pincfg *pins;
	const struct hda_verb *verbs;
	uint32_t quirks;
};

static const struct cs_model cs_models[] = {
	{ CS_420X, SSID(0x10de, 0x0ac0), 0, "MacBook Pro 5,3", cs_mbp53_pins, 0,
	  CSQ_GPIO13 },
	{ CS_420X, SSID(0x10de, 0x0d94), 0, "MacBook Air 3,1",
	  cs_mbp55_pins, 0, CSQ_GPIO13 },
	{ CS_420X, SSID(0x10de, 0xcb79), 0, "MacBook Pro 5,5", cs_mbp55_pins, 0,
	  CSQ_GPIO13 },
	{ CS_420X, SSID(0x10de, 0xcb89), 0, "MacBook Pro 7,1", cs_mbp55_pins, 0,
	  CSQ_GPIO13 },
	{ CS_420X, SSID(0x106b, 0x0600), 0, "iMac 14,1", 0, 0,
	  CSQ_GPIO23 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x0900), 0, "iMac 12,1", 0, 0,
	  CSQ_GPIO23 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x1c00), 0, "MacBook Pro 8,1", 0, cs_mbp81_verbs,
	  CSQ_GPIO13 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x2000), 0, "iMac 12,2", 0, 0,
	  CSQ_GPIO23 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x2800), 0, "MacBook Pro 10,1", cs_mbp101_pins,
	  0, CSQ_GPIO13 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x5600), 0, "MacBook Air 5,2", 0, cs_mbp81_verbs,
	  CSQ_GPIO13 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0x5b00), 0, "MacBook Air 4,2", cs_mba42_pins, 0,
	  CSQ_GPIO13 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0), 0xffff0000u, "Apple", 0, 0,
	  CSQ_GPIO13 | CSQ_CODEC_SSID },
	{ CS_420X, SSID(0x106b, 0), 0xffff0000u, "Apple", 0, 0, CSQ_GPIO13 },
	/* an iMac 27" without its own codec id */
	{ CS_420X, SSID(0x8086, 0x7270), 0, "iMac 27", cs_imac27_pins, 0, 0 },

	{ CS_4208, SSID(0x106b, 0x5e00), 0, "MacBook Pro 11,2", 0, 0,
	  CSQ_GPIO0_SPK | CSQ_CODEC_SSID },
	{ CS_4208, SSID(0x106b, 0x6c00), 0, "Mac mini 7,1", cs4208_macmini_pins,
	  0, CSQ_GPIO0_SPK | CSQ_INV_HP_JACK | CSQ_CODEC_SSID },
	{ CS_4208, SSID(0x106b, 0x7100), 0, "MacBook Air 6,1", cs4208_mba6_pins,
	  0, CSQ_GPIO0_SPK | CSQ_CODEC_SSID },
	{ CS_4208, SSID(0x106b, 0x7200), 0, "MacBook Air 6,2", cs4208_mba6_pins,
	  0, CSQ_GPIO0_SPK | CSQ_CODEC_SSID },
	{ CS_4208, SSID(0x106b, 0x7b00), 0, "MacBook Pro 12,1", 0, 0,
	  CSQ_GPIO0_SPK | CSQ_CODEC_SSID },
	{ CS_4208, SSID(0x106b, 0), 0xffff0000u, "Apple", 0, 0, CSQ_GPIO0_SPK },

	{ CS_4210, SSID(0x8086, 0x5001), 0, "DP45SG/CDB4210", cs_cdb4210_pins,
	  0, CSQ_SENSE_B },

	{ 0, 0, 0, 0, 0, 0, 0 }
};

static const struct cs_model *cs_lookup(const struct hda_codec *c)
{
	int kind = cs_kind(c);
	uint32_t pci = cs_pci_ssid(c);
	int pass, i;

	/* the codec subsystem id is the more precise one on the Macs */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; cs_models[i].name; i++) {
			const struct cs_model *m = &cs_models[i];
			uint32_t mask = m->mask ? m->mask : 0xffffffffu;
			uint32_t ssid;

			if (m->kind != kind)
				continue;
			if (pass == 0) {
				if (!(m->quirks & CSQ_CODEC_SSID))
					continue;
				ssid = c->subsystem_id;
			} else {
				if (m->quirks & CSQ_CODEC_SSID)
					continue;
				ssid = pci;
			}
			if (ssid && (ssid & mask) == (m->subsys & mask))
				return m;
		}
	}
	return 0;
}

/* GPIO plan of the Apple amplifiers */
static void cs_amp_gpios(const struct cs_model *m, uint32_t *hp, uint32_t *spk)
{
	*hp = *spk = 0;
	if (!m)
		return;
	if (m->quirks & CSQ_GPIO13) {
		*hp = 0x02;
		*spk = 0x08;
	} else if (m->quirks & CSQ_GPIO23) {
		*hp = 0x04;
		*spk = 0x08;
	} else if (m->quirks & CSQ_GPIO0_SPK) {
		*hp = 0;
		*spk = 0x01;
	}
}

static void cs_update_amps(struct hda_codec *c, const struct cs_model *m,
			   int hp_present)
{
	uint32_t hp, spk, data;

	cs_amp_gpios(m, &hp, &spk);
	if (!(hp | spk))
		return;
	if (c->spec.auto_mute)
		data = hp_present ? hp : spk;
	else
		data = hp | spk;
	cs_gpio_write(c, hp | spk, hp | spk, data);
}

/* CS420x: digital microphones replace GPIO0/1 when their pins are used */
static void cs420x_input_coef(struct hda_codec *c)
{
	uint32_t coef = cs_coef_get(c, CS420X_VNID, CS_IDX_BEEP_CFG);

	if (coef == 0xffffffffu)
		return;
	if (cs_pin_active(c, CS_DMIC2_PIN))
		coef |= 1u << 4; /* DMIC2: two channels, GPIO1 off */
	if (cs_pin_active(c, CS_DMIC1_PIN))
		coef |= 1u << 3; /* DMIC1: two channels, GPIO0 off */
	cs_coef_set(c, CS420X_VNID, CS_IDX_BEEP_CFG, (uint16_t)coef);
}

static void cs420x_digital_coef(struct hda_codec *c)
{
	uint16_t coef = 0x0002 | 0x0008; /* soft mute without lock, mute on
					  * error */

	if (cs_pin_active(c, CS_DIG_OUT2_PIN))
		coef |= 0x4000; /* second S/PDIF output */
	cs_coef_set(c, CS420X_VNID, CS_IDX_SPDIF_CTL, coef);
}

/* CS4210: GPIO, digital mic and SENSE_B share pins; set the mux */
static void cs4210_pinmux(struct hda_codec *c, const struct cs_model *m)
{
	uint32_t coef = cs_coef_get(c, 0x0b, 0x01);
	int sense_b = m && (m->quirks & CSQ_SENSE_B);
	int gpio = c->gpio_mask != 0;

	if (coef == 0xffffffffu)
		return;
	if (gpio)
		coef |= 0x0008;
	else
		coef &= ~0x0008u;
	if (sense_b)
		coef |= 0x0010;
	else
		coef &= ~0x0010u;
	cs_coef_set(c, 0x0b, 0x01, (uint16_t)coef);
	if ((gpio || sense_b) && cs_pin_active(c, 0x09)) {
		/* the digital microphone lost its pins */
		struct hda_widget *w = hda_widget(c, 0x09);
		uint32_t cfg = w->pin_cfg;

		cfg &= ~AC_DEFCFG_PORT_CONN;
		cfg |= (uint32_t)AC_JACK_PORT_NONE << AC_DEFCFG_PORT_CONN_SHIFT;
		hda_pin_override_cfg(c, 0x09, cfg);
		w->pin_cfg = cfg;
	}
}

static void cs42xx_hw_init(struct hda_codec *c, const struct cs_model *m)
{
	uint8_t vnid = cs_vendor_nid(c);
	/* a part without its processing widget (odd revision or a broken
	 * enumeration) gets no coefficient writes at all */
	int has_vendor = vnid && hda_widget(c, vnid) != 0;

	switch (has_vendor ? cs_kind(c) : CS_UNKNOWN) {
	case CS_420X:
		hda_apply_verbs(c, cs420x_errata_verbs);
		hda_apply_verbs(c, cs420x_coef_verbs);
		break;
	case CS_4208:
		hda_apply_verbs(c, cs4208_coef_verbs);
		break;
	case CS_4210:
		hda_apply_verbs(c, cs421x_coef_verbs);
		hda_apply_verbs(c, cs421x_a1_verbs);
		cs4210_pinmux(c, m);
		break;
	case CS_4213:
		/* the CS4213 runs with its power-on coefficients */
		break;
	default:
		break;
	}
	if (m && m->verbs && has_vendor)
		hda_apply_verbs(c, m->verbs);
	cs_update_amps(c, m, cs_hp_present(c));
	if (cs_kind(c) == CS_420X && has_vendor) {
		cs420x_input_coef(c);
		cs420x_digital_coef(c);
	}
}

/* ======================================================================
 * CS8409 + CS42L42
 * ====================================================================== */

#define CS8409_VNID 0x47
#define CS8409_AFG 0x01

/* pins */
#define CS8409_ASP1_TX_A 0x24 /* headphone (CS42L42 #0) */
#define CS8409_ASP1_TX_B 0x25 /* line out (CS42L42 #1, Dolphin) */
#define CS8409_ASP2_TX_A 0x2c /* speaker amplifiers */
#define CS8409_ASP1_RX_A 0x34 /* headset microphone */
#define CS8409_DMIC1_IN 0x44
#define CS8409_DMIC1 0x22 /* the digital mic converter */

/* vendor coefficients */
#define CS8409_DEV_CFG1 0x00
#define CS8409_DEV_CFG2 0x01
#define CS8409_DEV_CFG3 0x02
#define CS8409_ASP1_CLK_CTRL1 0x03
#define CS8409_ASP1_CLK_CTRL2 0x04
#define CS8409_ASP1_CLK_CTRL3 0x05
#define CS8409_ASP2_CLK_CTRL1 0x06
#define CS8409_ASP2_CLK_CTRL2 0x07
#define CS8409_ASP2_CLK_CTRL3 0x08
#define CS8409_DMIC_CFG 0x09
#define CS8409_BEEP_CFG 0x0a
#define CS8409_ASP1_A_TX_CTRL1 0x19
#define CS8409_ASP1_A_TX_CTRL2 0x1a
#define CS8409_ASP1_B_TX_CTRL1 0x1b
#define CS8409_ASP1_B_TX_CTRL2 0x1c
#define CS8409_ASP2_A_TX_CTRL1 0x29
#define CS8409_ASP2_A_TX_CTRL2 0x2a
#define CS8409_ASP1_A_RX_CTRL1 0x39
#define CS8409_ASP1_A_RX_CTRL2 0x3a
#define CS8409_I2C_ADDR 0x59
#define CS8409_I2C_CTRL 0x5b
#define CS8409_I2C_STS 0x5c
#define CS8409_I2C_QWRITE 0x5d
#define CS8409_I2C_QREAD 0x5e
#define CS8409_PFE_COEF_W1 0x63
#define CS8409_PFE_COEF_W2 0x64
#define CS8409_PFE_CTRL1 0x65
#define CS8409_PRE_SCALE_ATTN2 0x68
#define CS8409_PAD_CFG_SLW_RATE_CTRL 0x82

/* CS42L42 registers: page in the high byte */
#define L42_I2C_TIMEOUT 0x1010
#define L42_SRC_CTL 0x1007
#define L42_MCLK_CTL 0x1009
#define L42_PWR_CTL1 0x1101
#define L42_PWR_CTL2 0x1102
#define L42_PWR_CTL3 0x1103
#define L42_RSENSE_CTL2 0x1105
#define L42_OSC_SWITCH 0x1107
#define L42_RSENSE_CTL3 0x1112
#define L42_TSENSE_CTL 0x1113
#define L42_HSDET_CTL1 0x111f
#define L42_HSDET_CTL2 0x1120
#define L42_HS_SWITCH_CTL 0x1121
#define L42_HS_DET_STATUS 0x1124
#define L42_HS_CLAMP_DISABLE 0x1129
#define L42_MCLK_SRC_SEL 0x1201
#define L42_SPDIF_CLK_CFG 0x1202
#define L42_FSYNC_P_LOWER 0x1205
#define L42_FSYNC_P_UPPER 0x1206
#define L42_ASP_CLK_CFG 0x1207
#define L42_ASP_FRM_CFG 0x1208
#define L42_CODEC_STATUS 0x1308
#define L42_DET_INT_STATUS1 0x1309
#define L42_DET_INT_STATUS2 0x130a
#define L42_TSRS_PLUG_STATUS 0x130f
#define L42_ADC_OVFL_INT_MASK 0x1316
#define L42_MIXER_INT_MASK 0x1317
#define L42_SRC_INT_MASK 0x1318
#define L42_ASP_RX_INT_MASK 0x1319
#define L42_ASP_TX_INT_MASK 0x131a
#define L42_CODEC_INT_MASK 0x131b
#define L42_SRCPL_INT_MASK 0x131c
#define L42_VPMON_INT_MASK 0x131e
#define L42_PLL_LOCK_INT_MASK 0x131f
#define L42_TSRS_PLUG_INT_MASK 0x1320
#define L42_HSBIAS_SC_AUTOCTL 0x1b70
#define L42_WAKE_CTL 0x1b71
#define L42_TIPSENSE_CTL 0x1b73
#define L42_MISC_DET_CTL 0x1b74
#define L42_MIC_DET_CTL1 0x1b75
#define L42_DET_STATUS1 0x1b77
#define L42_DET_INT1_MASK 0x1b79
#define L42_DET_INT2_MASK 0x1b7a
#define L42_HS_BIAS_CTL 0x1c03
#define L42_ADC_CTL 0x1d01
#define L42_ADC_VOLUME 0x1d03
#define L42_DAC_CTL2 0x1f06
#define L42_HP_CTL 0x2001
#define L42_MIXER_CHA_VOL 0x2301
#define L42_MIXER_ADC_VOL 0x2302
#define L42_MIXER_CHB_VOL 0x2303
#define L42_ASP_TX_SZ_EN 0x2901
#define L42_ASP_TX_CH_EN 0x2902
#define L42_ASP_TX_CH_AP_RES 0x2903
#define L42_ASP_TX_CH1_BIT_MSB 0x2904
#define L42_ASP_TX_CH1_BIT_LSB 0x2905
#define L42_ASP_RX_DAI0_EN 0x2a01
#define L42_DAI0_CH1_AP_RES 0x2a02
#define L42_DAI0_CH1_BIT_MSB 0x2a03
#define L42_DAI0_CH1_BIT_LSB 0x2a04
#define L42_DAI0_CH2_AP_RES 0x2a05
#define L42_DAI0_CH2_BIT_MSB 0x2a06
#define L42_DAI0_CH2_BIT_LSB 0x2a07
#define L42_DAI0_CH3_AP_RES 0x2a08
#define L42_DAI0_CH3_BIT_MSB 0x2a09
#define L42_DAI0_CH3_BIT_LSB 0x2a0a
#define L42_DAI0_CH4_AP_RES 0x2a0b
#define L42_DAI0_CH4_BIT_MSB 0x2a0c
#define L42_DAI0_CH4_BIT_LSB 0x2a0d

/* status bits */
#define L42_HSDET_AUTO_DONE (1u << 1) /* CODEC_STATUS */
#define L42_TS_PLUG_SHIFT 2 /* TSRS_PLUG_STATUS: bits 3:2 */
#define L42_TS_PLUGGED 3
#define L42_TS_UNPLUGGED 0
#define L42_HSDET_TYPE_MASK 3 /* HS_DET_STATUS */
#define L42_PLUG_CTIA 0
#define L42_PLUG_OMTP 1
#define L42_PLUG_HEADPHONE 2
#define L42_PLUG_INVALID 3
#define L42_FULL_SCALE_VOL (1u << 1) /* HP_CTL */

struct l42_reg {
	uint16_t reg;
	uint8_t val;
};

/* Companion codec setup: ASP slave at 48 kHz/24 bit, clocks from the
 * bridge, headset detection armed, every interrupt masked until the
 * jack logic enables its own.  Channel slots 1-4 for the Dell layouts. */
static const struct l42_reg l42_init_dell[] = {
	{ L42_I2C_TIMEOUT, 0xb0 }, { L42_ADC_CTL, 0x00 }, { 0x1d02, 0x06 },
	{ L42_ADC_VOLUME, 0x9f }, { L42_OSC_SWITCH, 0x01 },
	{ L42_MCLK_CTL, 0x02 }, { L42_SRC_CTL, 0x03 },
	{ L42_MCLK_SRC_SEL, 0x00 }, { L42_ASP_FRM_CFG, 0x13 },
	{ L42_FSYNC_P_LOWER, 0xff }, { L42_FSYNC_P_UPPER, 0x00 },
	{ L42_ASP_CLK_CFG, 0x20 }, { L42_SPDIF_CLK_CFG, 0x0d },
	{ L42_DAI0_CH1_AP_RES, 0x02 }, { L42_DAI0_CH1_BIT_MSB, 0x00 },
	{ L42_DAI0_CH1_BIT_LSB, 0x00 }, { L42_DAI0_CH2_AP_RES, 0x02 },
	{ L42_DAI0_CH2_BIT_MSB, 0x00 }, { L42_DAI0_CH2_BIT_LSB, 0x20 },
	{ L42_DAI0_CH3_AP_RES, 0x02 }, { L42_DAI0_CH3_BIT_MSB, 0x00 },
	{ L42_DAI0_CH3_BIT_LSB, 0x80 }, { L42_DAI0_CH4_AP_RES, 0x02 },
	{ L42_DAI0_CH4_BIT_MSB, 0x00 }, { L42_DAI0_CH4_BIT_LSB, 0xa0 },
	{ L42_ASP_RX_DAI0_EN, 0x0c }, { L42_ASP_TX_CH_EN, 0x01 },
	{ L42_ASP_TX_CH_AP_RES, 0x02 }, { L42_ASP_TX_CH1_BIT_MSB, 0x00 },
	{ L42_ASP_TX_CH1_BIT_LSB, 0x00 }, { L42_ASP_TX_SZ_EN, 0x01 },
	{ L42_PWR_CTL1, 0x0a }, { L42_PWR_CTL2, 0x84 },
	{ L42_MIXER_CHA_VOL, 0x3f }, { L42_MIXER_CHB_VOL, 0x3f },
	{ L42_MIXER_ADC_VOL, 0x3f }, { L42_HP_CTL, 0x03 },
	{ L42_MIC_DET_CTL1, 0xb6 }, { L42_TIPSENSE_CTL, 0xc2 },
	{ L42_HS_CLAMP_DISABLE, 0x01 }, { L42_HS_SWITCH_CTL, 0xf3 },
	{ L42_PWR_CTL3, 0x20 }, { L42_RSENSE_CTL2, 0x00 },
	{ L42_RSENSE_CTL3, 0x00 }, { L42_TSENSE_CTL, 0x80 },
	{ L42_HS_BIAS_CTL, 0xc0 }, { L42_PWR_CTL1, 0x02 },
	{ L42_ADC_OVFL_INT_MASK, 0xff }, { L42_MIXER_INT_MASK, 0xff },
	{ L42_SRC_INT_MASK, 0xff }, { L42_ASP_RX_INT_MASK, 0xff },
	{ L42_ASP_TX_INT_MASK, 0xff }, { L42_CODEC_INT_MASK, 0xff },
	{ L42_SRCPL_INT_MASK, 0xff }, { L42_VPMON_INT_MASK, 0xff },
	{ L42_PLL_LOCK_INT_MASK, 0xff }, { L42_TSRS_PLUG_INT_MASK, 0xff },
	{ L42_DET_INT1_MASK, 0xff }, { L42_DET_INT2_MASK, 0xff },
	{ 0, 0 }
};

/* Dolphin, first CS42L42 (headphone + headset mic), two RX slots */
static const struct l42_reg l42_init_dolphin0[] = {
	{ L42_I2C_TIMEOUT, 0xb0 }, { L42_ADC_CTL, 0x00 }, { 0x1d02, 0x06 },
	{ L42_ADC_VOLUME, 0x9f }, { L42_OSC_SWITCH, 0x01 },
	{ L42_MCLK_CTL, 0x02 }, { L42_SRC_CTL, 0x03 },
	{ L42_MCLK_SRC_SEL, 0x00 }, { L42_ASP_FRM_CFG, 0x13 },
	{ L42_FSYNC_P_LOWER, 0xff }, { L42_FSYNC_P_UPPER, 0x00 },
	{ L42_ASP_CLK_CFG, 0x20 }, { L42_SPDIF_CLK_CFG, 0x0d },
	{ L42_DAI0_CH1_AP_RES, 0x02 }, { L42_DAI0_CH1_BIT_MSB, 0x00 },
	{ L42_DAI0_CH1_BIT_LSB, 0x00 }, { L42_DAI0_CH2_AP_RES, 0x02 },
	{ L42_DAI0_CH2_BIT_MSB, 0x00 }, { L42_DAI0_CH2_BIT_LSB, 0x20 },
	{ L42_ASP_RX_DAI0_EN, 0x0c }, { L42_ASP_TX_CH_EN, 0x01 },
	{ L42_ASP_TX_CH_AP_RES, 0x02 }, { L42_ASP_TX_CH1_BIT_MSB, 0x00 },
	{ L42_ASP_TX_CH1_BIT_LSB, 0x00 }, { L42_ASP_TX_SZ_EN, 0x01 },
	{ L42_PWR_CTL1, 0x0a }, { L42_PWR_CTL2, 0x84 }, { L42_HP_CTL, 0x03 },
	{ L42_MIXER_CHA_VOL, 0x3f }, { L42_MIXER_CHB_VOL, 0x3f },
	{ L42_MIXER_ADC_VOL, 0x3f }, { L42_MIC_DET_CTL1, 0xb6 },
	{ L42_TIPSENSE_CTL, 0xc2 }, { L42_HS_CLAMP_DISABLE, 0x01 },
	{ L42_HS_SWITCH_CTL, 0xf3 }, { L42_PWR_CTL3, 0x20 },
	{ L42_RSENSE_CTL2, 0x00 }, { L42_RSENSE_CTL3, 0x00 },
	{ L42_TSENSE_CTL, 0x80 }, { L42_HS_BIAS_CTL, 0xc0 },
	{ L42_PWR_CTL1, 0x02 }, { L42_ADC_OVFL_INT_MASK, 0xff },
	{ L42_MIXER_INT_MASK, 0xff }, { L42_SRC_INT_MASK, 0xff },
	{ L42_ASP_RX_INT_MASK, 0xff }, { L42_ASP_TX_INT_MASK, 0xff },
	{ L42_CODEC_INT_MASK, 0xff }, { L42_SRCPL_INT_MASK, 0xff },
	{ L42_VPMON_INT_MASK, 0xff }, { L42_PLL_LOCK_INT_MASK, 0xff },
	{ L42_TSRS_PLUG_INT_MASK, 0xff }, { L42_DET_INT1_MASK, 0xff },
	{ L42_DET_INT2_MASK, 0xff }, { 0, 0 }
};

/* Dolphin, second CS42L42 (line out): slots 3/4, no capture */
static const struct l42_reg l42_init_dolphin1[] = {
	{ L42_I2C_TIMEOUT, 0xb0 }, { L42_ADC_CTL, 0x00 }, { 0x1d02, 0x06 },
	{ L42_ADC_VOLUME, 0x9f }, { L42_OSC_SWITCH, 0x01 },
	{ L42_MCLK_CTL, 0x02 }, { L42_SRC_CTL, 0x03 },
	{ L42_MCLK_SRC_SEL, 0x00 }, { L42_ASP_FRM_CFG, 0x13 },
	{ L42_FSYNC_P_LOWER, 0xff }, { L42_FSYNC_P_UPPER, 0x00 },
	{ L42_ASP_CLK_CFG, 0x20 }, { L42_SPDIF_CLK_CFG, 0x0d },
	{ L42_DAI0_CH1_AP_RES, 0x02 }, { L42_DAI0_CH1_BIT_MSB, 0x00 },
	{ L42_DAI0_CH1_BIT_LSB, 0x80 }, { L42_DAI0_CH2_AP_RES, 0x02 },
	{ L42_DAI0_CH2_BIT_MSB, 0x00 }, { L42_DAI0_CH2_BIT_LSB, 0xa0 },
	{ L42_ASP_RX_DAI0_EN, 0x0c }, { L42_ASP_TX_CH_EN, 0x00 },
	{ L42_ASP_TX_CH_AP_RES, 0x02 }, { L42_ASP_TX_CH1_BIT_MSB, 0x00 },
	{ L42_ASP_TX_CH1_BIT_LSB, 0x00 }, { L42_ASP_TX_SZ_EN, 0x00 },
	{ L42_PWR_CTL1, 0x0e }, { L42_PWR_CTL2, 0x84 }, { L42_HP_CTL, 0x01 },
	{ L42_MIXER_CHA_VOL, 0x3f }, { L42_MIXER_CHB_VOL, 0x3f },
	{ L42_MIXER_ADC_VOL, 0x3f }, { L42_MIC_DET_CTL1, 0xb6 },
	{ L42_TIPSENSE_CTL, 0xc2 }, { L42_HS_CLAMP_DISABLE, 0x01 },
	{ L42_HS_SWITCH_CTL, 0xf3 }, { L42_PWR_CTL3, 0x20 },
	{ L42_RSENSE_CTL2, 0x00 }, { L42_RSENSE_CTL3, 0x00 },
	{ L42_TSENSE_CTL, 0x80 }, { L42_HS_BIAS_CTL, 0xc0 },
	{ L42_PWR_CTL1, 0x06 }, { L42_ADC_OVFL_INT_MASK, 0xff },
	{ L42_MIXER_INT_MASK, 0xff }, { L42_SRC_INT_MASK, 0xff },
	{ L42_ASP_RX_INT_MASK, 0xff }, { L42_ASP_TX_INT_MASK, 0xff },
	{ L42_CODEC_INT_MASK, 0xff }, { L42_SRCPL_INT_MASK, 0xff },
	{ L42_VPMON_INT_MASK, 0xff }, { L42_PLL_LOCK_INT_MASK, 0xff },
	{ L42_TSRS_PLUG_INT_MASK, 0xff }, { L42_DET_INT1_MASK, 0xff },
	{ L42_DET_INT2_MASK, 0xff }, { 0, 0 }
};

struct cs8409_coef {
	uint8_t idx;
	uint16_t val;
};

/* Bridge setup for one CS42L42 on ASP1 and the amplifiers on ASP2 */
static const struct cs8409_coef cs8409_cfg_dell[] = {
	{ CS8409_DEV_CFG1, 0xb008 }, /* PLL1/2 on, I2C on */
	{ CS8409_DEV_CFG2, 0x0002 }, /* ASPs off, ASP1 stop */
	{ CS8409_DEV_CFG3, 0x0a80 }, /* bus idle, GPIO I2C */
	{ CS8409_ASP1_A_TX_CTRL1, 0x0800 }, /* 24-bit slots */
	{ CS8409_ASP1_A_TX_CTRL2, 0x0820 },
	{ CS8409_ASP2_A_TX_CTRL1, 0x0800 },
	{ CS8409_ASP2_A_TX_CTRL2, 0x2800 },
	{ CS8409_ASP1_A_RX_CTRL1, 0x0800 },
	{ CS8409_ASP1_A_RX_CTRL2, 0x0800 },
	{ CS8409_ASP1_CLK_CTRL1, 0x8000 },
	{ CS8409_ASP1_CLK_CTRL2, 0x28ff }, /* clocks from PLL1 */
	{ CS8409_ASP1_CLK_CTRL3, 0x0062 },
	{ CS8409_ASP2_CLK_CTRL1, 0x801f },
	{ CS8409_ASP2_CLK_CTRL2, 0x283f },
	{ CS8409_ASP2_CLK_CTRL3, 0x805c },
	{ CS8409_DMIC_CFG, 0x0023 },
	{ CS8409_BEEP_CFG, 0x0000 },
	{ CS8409_DEV_CFG2, 0x0062 }, /* ASP1/2 on */
	{ CS8409_DEV_CFG1, 0x9008 }, /* PLL2 off */
	{ CS8409_PRE_SCALE_ATTN2, 0x0000 }, /* speaker path 0 dB */
	{ CS8409_PAD_CFG_SLW_RATE_CTRL, 0xfc03 },
	{ 0xc0, 0x9999 }, /* test mode on */
	{ 0xc5, 0x0000 }, /* GPIO hysteresis 30 us */
	{ 0xc0, 0x0000 }, /* test mode off */
	{ 0xff, 0 }
};

/* Bridge setup for the Dolphin: two CS42L42 on ASP1, no ASP2 */
static const struct cs8409_coef cs8409_cfg_dolphin[] = {
	{ CS8409_DEV_CFG1, 0xb008 }, { CS8409_DEV_CFG2, 0x0002 },
	{ CS8409_DEV_CFG3, 0x0a80 }, { CS8409_ASP1_A_TX_CTRL1, 0x0800 },
	{ CS8409_ASP1_A_TX_CTRL2, 0x0820 }, { CS8409_ASP1_B_TX_CTRL1, 0x0880 },
	{ CS8409_ASP1_B_TX_CTRL2, 0x08a0 }, { CS8409_ASP1_A_RX_CTRL1, 0x0800 },
	{ CS8409_ASP1_A_RX_CTRL2, 0x0800 }, { CS8409_ASP1_CLK_CTRL1, 0x8000 },
	{ CS8409_ASP1_CLK_CTRL2, 0x28ff }, { CS8409_ASP1_CLK_CTRL3, 0x0062 },
	{ CS8409_BEEP_CFG, 0x0000 }, { CS8409_DEV_CFG2, 0x0022 },
	{ CS8409_DEV_CFG1, 0x9008 }, { CS8409_PAD_CFG_SLW_RATE_CTRL, 0x5400 },
	{ 0xc0, 0x9999 }, { 0xc5, 0x0000 }, { 0xc0, 0x0000 }, { 0xff, 0 }
};

/* Bullseye speaker equaliser: an attenuating biquad on the ASP2 path */
static const struct cs8409_coef cs8409_bullseye_eq[] = {
	{ CS8409_PFE_CTRL1, 0x4000 }, { CS8409_PFE_COEF_W2, 0x4000 },
	{ CS8409_PFE_CTRL1, 0x4010 }, { CS8409_PFE_COEF_W1, 0x0647 },
	{ CS8409_PFE_COEF_W2, 0xc0c7 }, { CS8409_PFE_COEF_W1, 0x0647 },
	{ CS8409_PFE_COEF_W2, 0xc1c7 }, { CS8409_PFE_COEF_W1, 0xf370 },
	{ CS8409_PFE_COEF_W2, 0xc271 }, { CS8409_PFE_COEF_W1, 0x1ef8 },
	{ CS8409_PFE_COEF_W2, 0xc348 }, { CS8409_PFE_COEF_W1, 0xc110 },
	{ CS8409_PFE_COEF_W2, 0xc45a }, { CS8409_PFE_COEF_W1, 0x1f29 },
	{ CS8409_PFE_COEF_W2, 0xc574 }, { CS8409_PFE_COEF_W1, 0x1d7a },
	{ CS8409_PFE_COEF_W2, 0xc653 }, { CS8409_PFE_COEF_W1, 0xc38c },
	{ CS8409_PFE_COEF_W2, 0xc714 }, { CS8409_PFE_COEF_W1, 0x1ca3 },
	{ CS8409_PFE_COEF_W2, 0xc8c7 }, { CS8409_PFE_COEF_W1, 0xc38c },
	{ CS8409_PFE_COEF_W2, 0xc914 }, { CS8409_PFE_COEF_W2, 0x0000 },
	{ 0xff, 0 }
};

static const struct hda_verb cs8409_init_verbs[] = {
	{ CS8409_AFG, AC_VERB_SET_GPIO_WAKE_MASK, 0x0018 }, /* GPIO 3, 4 */
	{ CS8409_VNID, AC_VERB_SET_PROC_STATE, 0x0001 },
	{ CS8409_VNID, AC_VERB_SET_COEF_INDEX, 0x0002 },
	{ CS8409_VNID, AC_VERB_SET_PROC_COEF, 0x0080 }, /* GPIO 6/7: I2C */
	{ CS8409_VNID, AC_VERB_SET_COEF_INDEX, 0x005b },
	{ CS8409_VNID, AC_VERB_SET_PROC_COEF, 0x0200 }, /* 100 kHz */
	{ 0, 0, 0 }
};

static const struct hda_verb cs8409_dolphin_init_verbs[] = {
	{ CS8409_AFG, AC_VERB_SET_GPIO_WAKE_MASK, 0x0011 }, /* GPIO 0, 4 */
	{ CS8409_VNID, AC_VERB_SET_PROC_STATE, 0x0001 },
	{ CS8409_VNID, AC_VERB_SET_COEF_INDEX, 0x0002 },
	{ CS8409_VNID, AC_VERB_SET_PROC_COEF, 0x0080 },
	{ CS8409_VNID, AC_VERB_SET_COEF_INDEX, 0x005b },
	{ CS8409_VNID, AC_VERB_SET_PROC_COEF, 0x0200 },
	{ 0, 0, 0 }
};

static const struct hda_pincfg cs8409_dell_pins[] = {
	{ CS8409_ASP1_TX_A, 0x042120f0 }, /* headphone jack */
	{ CS8409_ASP1_RX_A, 0x04a12050 }, /* headset microphone */
	{ CS8409_ASP2_TX_A, 0x901000f0 }, /* speakers */
	{ CS8409_DMIC1_IN, 0x90a00090 }, /* digital microphone */
	{ 0, 0 }
};

static const struct hda_pincfg cs8409_dell_nodmic_pins[] = {
	{ CS8409_ASP1_TX_A, 0x042120f0 },
	{ CS8409_ASP1_RX_A, 0x04a12050 },
	{ CS8409_ASP2_TX_A, 0x901000f0 },
	{ 0, 0 }
};

static const struct hda_pincfg cs8409_dolphin_pins[] = {
	{ CS8409_ASP1_TX_A, 0x022210f0 }, /* headphone */
	{ CS8409_ASP1_TX_B, 0x010240f0 }, /* line out */
	{ CS8409_ASP1_RX_A, 0x02a21050 }, /* microphone */
	{ 0, 0 }
};

enum cs8409_board {
	CS8409_NONE = 0,
	CS8409_BULLSEYE,
	CS8409_WARLOCK,
	CS8409_WARLOCK_MLK,
	CS8409_WARLOCK_MLK_DUAL_MIC,
	CS8409_CYBORG,
	CS8409_ODIN,
	CS8409_DOLPHIN,
};

struct cs8409_quirk {
	uint16_t device; /* Dell subsystem device */
	uint8_t board;
};

static const struct cs8409_quirk cs8409_dell[] = {
	{ 0x0a11, CS8409_BULLSEYE }, { 0x0a12, CS8409_BULLSEYE },
	{ 0x0a23, CS8409_BULLSEYE }, { 0x0a24, CS8409_BULLSEYE },
	{ 0x0a25, CS8409_BULLSEYE }, { 0x0a29, CS8409_BULLSEYE },
	{ 0x0a2a, CS8409_BULLSEYE }, { 0x0a2b, CS8409_BULLSEYE },
	{ 0x0a77, CS8409_CYBORG }, { 0x0a78, CS8409_CYBORG },
	{ 0x0a79, CS8409_CYBORG }, { 0x0a7a, CS8409_CYBORG },
	{ 0x0a7d, CS8409_CYBORG }, { 0x0a7e, CS8409_CYBORG },
	{ 0x0a7f, CS8409_CYBORG }, { 0x0a80, CS8409_CYBORG },
	{ 0x0ab0, CS8409_WARLOCK }, { 0x0ab1, CS8409_WARLOCK },
	{ 0x0ab2, CS8409_WARLOCK }, { 0x0ab3, CS8409_WARLOCK },
	{ 0x0ab4, CS8409_WARLOCK }, { 0x0ab5, CS8409_WARLOCK },
	{ 0x0acf, CS8409_DOLPHIN }, { 0x0ad0, CS8409_DOLPHIN },
	{ 0x0ad1, CS8409_DOLPHIN }, { 0x0ad2, CS8409_DOLPHIN },
	{ 0x0ad3, CS8409_DOLPHIN }, { 0x0ad9, CS8409_WARLOCK },
	{ 0x0ada, CS8409_WARLOCK }, { 0x0adb, CS8409_WARLOCK },
	{ 0x0adc, CS8409_WARLOCK }, { 0x0adf, CS8409_CYBORG },
	{ 0x0ae0, CS8409_CYBORG }, { 0x0ae1, CS8409_CYBORG },
	{ 0x0ae2, CS8409_CYBORG }, { 0x0ae9, CS8409_CYBORG },
	{ 0x0aea, CS8409_CYBORG }, { 0x0aeb, CS8409_CYBORG },
	{ 0x0aec, CS8409_CYBORG }, { 0x0aed, CS8409_CYBORG },
	{ 0x0aee, CS8409_CYBORG }, { 0x0aef, CS8409_CYBORG },
	{ 0x0af0, CS8409_CYBORG }, { 0x0af4, CS8409_WARLOCK },
	{ 0x0af5, CS8409_WARLOCK }, { 0x0b92, CS8409_WARLOCK_MLK },
	{ 0x0b93, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0b94, CS8409_WARLOCK_MLK },
	{ 0x0b95, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0b96, CS8409_WARLOCK_MLK },
	{ 0x0b97, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0ba5, CS8409_ODIN },
	{ 0x0ba6, CS8409_ODIN }, { 0x0ba8, CS8409_ODIN },
	{ 0x0baa, CS8409_ODIN }, { 0x0bae, CS8409_ODIN },
	{ 0x0bb2, CS8409_WARLOCK_MLK }, { 0x0bb3, CS8409_WARLOCK_MLK },
	{ 0x0bb4, CS8409_WARLOCK_MLK }, { 0x0bb5, CS8409_WARLOCK },
	{ 0x0bb6, CS8409_WARLOCK }, { 0x0bb8, CS8409_WARLOCK_MLK },
	{ 0x0bb9, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0bba, CS8409_WARLOCK_MLK },
	{ 0x0bbb, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0bbc, CS8409_WARLOCK_MLK },
	{ 0x0bbd, CS8409_WARLOCK_MLK_DUAL_MIC }, { 0x0bd4, CS8409_DOLPHIN },
	{ 0x0bd5, CS8409_DOLPHIN }, { 0x0bd6, CS8409_DOLPHIN },
	{ 0x0bd7, CS8409_DOLPHIN }, { 0x0bd8, CS8409_DOLPHIN },
	{ 0x0c43, CS8409_DOLPHIN }, { 0x0c50, CS8409_DOLPHIN },
	{ 0x0c51, CS8409_DOLPHIN }, { 0x0c52, CS8409_DOLPHIN },
	{ 0x0c73, CS8409_DOLPHIN }, { 0x0c75, CS8409_DOLPHIN },
	{ 0x0c7d, CS8409_DOLPHIN }, { 0x0c7f, CS8409_DOLPHIN },
	{ 0, 0 }
};

static const char *const cs8409_board_names[] = {
	[CS8409_NONE] = "unknown board",
	[CS8409_BULLSEYE] = "Dell Bullseye",
	[CS8409_WARLOCK] = "Dell Warlock",
	[CS8409_WARLOCK_MLK] = "Dell Warlock MLK",
	[CS8409_WARLOCK_MLK_DUAL_MIC] = "Dell Warlock MLK dual mic",
	[CS8409_CYBORG] = "Dell Cyborg",
	[CS8409_ODIN] = "Dell Odin",
	[CS8409_DOLPHIN] = "Dell Dolphin",
};

/* One CS42L42 behind the bridge */
struct l42_desc {
	uint8_t i2c_addr; /* 8-bit form (7-bit address << 1) */
	uint8_t reset_gpio;
	uint8_t irq_gpio;
	uint8_t pin; /* the bridge pin that carries its output */
	uint8_t full_scale_0db; /* HP full scale 0 dB instead of -6 dB */
	uint8_t no_type_detect; /* line out: no headset typing */
	const struct l42_reg *init;
};

struct cs8409_layout {
	uint8_t nl42;
	struct l42_desc l42[2];
	uint8_t spk_pdn_gpio; /* speaker amplifier power-down line */
	uint8_t spk_pin;
	const struct cs8409_coef *cfg;
	const struct hda_verb *init_verbs;
	const struct hda_pincfg *pins;
};

static int cs8409_board(const struct hda_codec *c)
{
	uint32_t ssid = cs_pci_ssid(c);
	int pass, i;

	for (pass = 0; pass < 2; pass++) {
		uint32_t s = pass ? c->subsystem_id : ssid;

		if ((s >> 16) != 0x1028)
			continue;
		for (i = 0; cs8409_dell[i].device; i++)
			if (cs8409_dell[i].device == (s & 0xffff))
				return cs8409_dell[i].board;
	}
	return CS8409_NONE;
}

static void cs8409_layout(int board, struct cs8409_layout *l)
{
	kmemset(l, 0, sizeof(*l));
	if (board == CS8409_NONE)
		return;
	if (board == CS8409_DOLPHIN) {
		l->nl42 = 2;
		l->l42[0].i2c_addr = 0x48 << 1;
		l->l42[0].reset_gpio = 1u << 5;
		l->l42[0].irq_gpio = 1u << 4;
		l->l42[0].pin = CS8409_ASP1_TX_A;
		l->l42[0].init = l42_init_dolphin0;
		l->l42[1].i2c_addr = 0x49 << 1;
		l->l42[1].reset_gpio = 1u << 1;
		l->l42[1].irq_gpio = 1u << 0;
		l->l42[1].pin = CS8409_ASP1_TX_B;
		l->l42[1].no_type_detect = 1;
		l->l42[1].init = l42_init_dolphin1;
		l->cfg = cs8409_cfg_dolphin;
		l->init_verbs = cs8409_dolphin_init_verbs;
		l->pins = cs8409_dolphin_pins;
		return;
	}
	l->nl42 = 1;
	l->l42[0].i2c_addr = 0x48 << 1;
	l->l42[0].reset_gpio = 1u << 5;
	l->l42[0].irq_gpio = 1u << 4;
	l->l42[0].pin = CS8409_ASP1_TX_A;
	l->l42[0].init = l42_init_dell;
	l->cfg = cs8409_cfg_dell;
	l->init_verbs = cs8409_init_verbs;
	l->pins = board == CS8409_ODIN ? cs8409_dell_nodmic_pins :
					 cs8409_dell_pins;
	l->spk_pin = CS8409_ASP2_TX_A;
	switch (board) {
	case CS8409_CYBORG:
		l->spk_pdn_gpio = 1u << 2;
		break;
	case CS8409_ODIN:
		l->spk_pdn_gpio = 1u << 2;
		l->l42[0].full_scale_0db = 1;
		break;
	case CS8409_WARLOCK_MLK:
	case CS8409_WARLOCK_MLK_DUAL_MIC:
		l->spk_pdn_gpio = 1u << 1;
		l->l42[0].full_scale_0db = 1;
		break;
	default: /* Bullseye, Warlock */
		l->spk_pdn_gpio = 1u << 1;
		break;
	}
}

/* ---- the I2C master in the bridge --------------------------------------------- */

/* Serialises bridge I2C transactions (jack work vs. resume). */
static volatile int cs8409_i2c_busy;

static void cs8409_i2c_lock(void)
{
	int spins = 0;

	while (__atomic_exchange_n(&cs8409_i2c_busy, 1, __ATOMIC_ACQUIRE)) {
		if (++spins > 50000) /* a stuck holder: ~0.5 s, take over */
			break;
		lapic_delay_us(10);
	}
}

static void cs8409_i2c_unlock(void)
{
	__atomic_store_n(&cs8409_i2c_busy, 0, __ATOMIC_RELEASE);
}

static void cs8409_coef_seq(struct hda_codec *c, const struct cs8409_coef *s)
{
	for (; s->idx != 0xff; s++)
		cs_coef_set(c, CS8409_VNID, s->idx, s->val);
}

/* Wait for the transaction-done/error bits; up to 20 ms. */
static int cs8409_i2c_wait(struct hda_codec *c)
{
	int i;

	for (i = 0; i < 100; i++) {
		uint32_t sts = cs_coef_get(c, CS8409_VNID, CS8409_I2C_STS);

		if (sts == 0xffffffffu)
			return -EIO;
		if (sts & 0x18)
			return 0;
		lapic_delay_us(200);
	}
	return -ETIMEDOUT;
}

static void cs8409_i2c_begin(struct hda_codec *c, uint8_t addr)
{
	uint32_t cfg = cs_coef_get(c, CS8409_VNID, CS8409_DEV_CFG1);

	/* I2C clock on */
	if (cfg != 0xffffffffu && !(cfg & 0x8))
		cs_coef_set(c, CS8409_VNID, CS8409_DEV_CFG1,
			    (uint16_t)(cfg | 0x8));
	cs_coef_set(c, CS8409_VNID, CS8409_I2C_ADDR, addr);
}

/* The CS42L42 register space is paged: register 0 selects the page. */
static int cs8409_i2c_page(struct hda_codec *c, uint16_t reg)
{
	cs_coef_set(c, CS8409_VNID, CS8409_I2C_QWRITE, reg >> 8);
	return cs8409_i2c_wait(c);
}

static int l42_write(struct hda_codec *c, const struct l42_desc *d,
		     uint16_t reg, uint8_t val)
{
	int err;

	cs8409_i2c_lock();
	cs8409_i2c_begin(c, d->i2c_addr);
	err = cs8409_i2c_page(c, reg);
	if (!err) {
		cs_coef_set(c, CS8409_VNID, CS8409_I2C_QWRITE,
			    (uint16_t)(((reg & 0xff) << 8) | val));
		err = cs8409_i2c_wait(c);
	}
	cs8409_i2c_unlock();
	return err;
}

static int l42_read(struct hda_codec *c, const struct l42_desc *d,
		    uint16_t reg)
{
	int err;
	uint32_t v = 0;

	cs8409_i2c_lock();
	cs8409_i2c_begin(c, d->i2c_addr);
	err = cs8409_i2c_page(c, reg);
	if (!err) {
		cs_coef_set(c, CS8409_VNID, CS8409_I2C_QREAD,
			    (uint16_t)((reg & 0xff) << 8));
		err = cs8409_i2c_wait(c);
		if (!err)
			v = cs_coef_get(c, CS8409_VNID, CS8409_I2C_QREAD);
	}
	cs8409_i2c_unlock();
	if (err)
		return err;
	if (v == 0xffffffffu)
		return -EIO;
	return (int)(v & 0xff);
}

static int l42_write_seq(struct hda_codec *c, const struct l42_desc *d,
			 const struct l42_reg *seq)
{
	int err = 0;

	for (; seq->reg; seq++) {
		err = l42_write(c, d, seq->reg, seq->val);
		if (err)
			break;
	}
	return err;
}

/* ---- CS42L42 bring-up and jack handling ---------------------------------- */

static void l42_arm_jack_detect(struct hda_codec *c, const struct l42_desc *d)
{
	l42_write(c, d, L42_HSBIAS_SC_AUTOCTL, 0x20);
	/* clear WAKE#, then let it follow the detection logic */
	l42_write(c, d, L42_WAKE_CTL, 0xc1);
	lapic_delay_us(2500);
	l42_write(c, d, L42_WAKE_CTL, 0xc0);
	l42_read(c, d, L42_TSRS_PLUG_STATUS); /* clear */
	l42_write(c, d, L42_TSRS_PLUG_INT_MASK, 0xf3); /* tip sense only */
}

/* Start the automatic headset-type detection (result via interrupt). */
static void l42_run_type_detect(struct hda_codec *c, const struct l42_desc *d)
{
	l42_read(c, d, L42_CODEC_STATUS);
	l42_read(c, d, L42_DET_STATUS1);
	l42_write(c, d, L42_TSRS_PLUG_INT_MASK, 0xff);
	l42_read(c, d, L42_TSRS_PLUG_STATUS);
	l42_write(c, d, L42_PWR_CTL2, 0x87);
	l42_write(c, d, L42_DAC_CTL2, 0x86);
	l42_write(c, d, L42_MISC_DET_CTL, 0x07);
	l42_write(c, d, L42_CODEC_INT_MASK, 0xfd);
	l42_write(c, d, L42_HSDET_CTL2, 0x80);
	lapic_delay_ms(20);
	l42_write(c, d, L42_HSDET_CTL1, 0x77);
	l42_write(c, d, L42_HSDET_CTL2, 0xc0);
}

static int l42_tip_plugged(struct hda_codec *c, const struct l42_desc *d)
{
	int ts = l42_read(c, d, L42_TSRS_PLUG_STATUS);

	if (ts < 0)
		return -1;
	return ((ts >> L42_TS_PLUG_SHIFT) & 3) == L42_TS_PLUGGED;
}

static int l42_resume(struct hda_codec *c, const struct l42_desc *d)
{
	static const uint16_t irq_regs[] = { L42_CODEC_STATUS,
					     L42_DET_INT_STATUS1,
					     L42_DET_INT_STATUS2,
					     L42_TSRS_PLUG_STATUS };
	int fsv, err;
	unsigned int i;

	/* out of reset */
	cs_gpio_write(c, c->gpio_mask, c->gpio_dir,
		      c->gpio_data | d->reset_gpio);
	lapic_delay_ms(10);

	err = l42_write_seq(c, d, d->init);
	if (err) {
		kprintf("[snd] hda: codec %d: CS42L42 at 0x%02x does not answer "
			"(%d)\n",
			c->addr, d->i2c_addr >> 1, err);
		return err;
	}
	lapic_delay_ms(45);
	for (i = 0; i < sizeof(irq_regs) / sizeof(irq_regs[0]); i++)
		l42_read(c, d, irq_regs[i]);

	/* headphone full scale: 0 dB or -6 dB depending on the board */
	fsv = l42_read(c, d, L42_HP_CTL);
	if (fsv >= 0) {
		int nfsv = d->full_scale_0db ? (fsv & ~(int)L42_FULL_SCALE_VOL) :
					       (fsv & (int)L42_FULL_SCALE_VOL);

		if (nfsv != fsv)
			l42_write(c, d, L42_HP_CTL, (uint8_t)nfsv);
	}
	/* DAC at 0 dB; the mixer does the volume */
	l42_write(c, d, L42_MIXER_CHA_VOL, 0x00);
	l42_write(c, d, L42_MIXER_CHB_VOL, 0x00);
	/* capture gain at 0 dB */
	l42_write(c, d, L42_ADC_VOLUME, 0x00);
	l42_arm_jack_detect(c, d);
	return 0;
}

/* The tag our GPIO unsolicited responses carry. */
#define CS8409_UNSOL_TAG 0x37

static void cs8409_enable_unsol(struct hda_codec *c,
				const struct cs8409_layout *l, int on)
{
	uint32_t irqs = 0;
	int i;

	for (i = 0; i < l->nl42; i++)
		irqs |= l->l42[i].irq_gpio;
	hda_codec_write(c, CS8409_AFG, AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK,
			on ? irqs : 0);
	hda_codec_write(c, CS8409_AFG, AC_VERB_SET_UNSOLICITED_ENABLE,
			on ? (AC_USRSP_EN | CS8409_UNSOL_TAG) : 0);
}

static struct hda_out *cs8409_out_for_pin(struct hda_codec *c, uint8_t pin)
{
	int i;

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].pin == pin)
			return &c->spec.out[i];
	return 0;
}

static struct hda_in *cs8409_in_for_pin(struct hda_codec *c, uint8_t pin)
{
	int i;

	for (i = 0; i < c->spec.nin && i < HDA_MAX_INPUTS; i++)
		if (c->spec.in[i].pin == pin)
			return &c->spec.in[i];
	return 0;
}

/* Apply the jack state: the speaker port goes quiet while the headphone
 * of codec #0 is in (the CS8409 pins have no sense of their own). */
static void cs8409_route(struct hda_codec *c, const struct cs8409_layout *l)
{
	struct hda_out *hp = cs8409_out_for_pin(c, l->l42[0].pin);
	int hp_in = hp && hp->present;
	int i;

	for (i = 0; i < l->nl42; i++)
		hda_pin_set_ctl(c, l->l42[i].pin,
				AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN);
	if (l->spk_pin)
		hda_pin_set_ctl(c, l->spk_pin, hp_in ? 0 : AC_PINCTL_OUT_EN);
}

static void cs8409_set_jack(struct hda_codec *c, const struct l42_desc *d,
			    int hp_in, int mic_in)
{
	struct hda_out *o = cs8409_out_for_pin(c, d->pin);
	struct hda_in *in = cs8409_in_for_pin(c, CS8409_ASP1_RX_A);

	if (o)
		o->present = hp_in ? 1 : 0;
	if (in && d->pin == CS8409_ASP1_TX_A)
		in->present = mic_in ? 1 : 0;
}

/* Interrupt from one CS42L42: read and acknowledge its status, return 1
 * when the jack state changed. */
static int cs8409_l42_event(struct hda_codec *c, const struct l42_desc *d)
{
	int cdc = l42_read(c, d, L42_CODEC_STATUS);
	int hs = l42_read(c, d, L42_HS_DET_STATUS);
	int ts = l42_read(c, d, L42_TSRS_PLUG_STATUS);
	int plug;

	if (cdc < 0 || hs < 0 || ts < 0)
		return 0;
	plug = (ts >> L42_TS_PLUG_SHIFT) & 3;

	if (cdc & L42_HSDET_AUTO_DONE) {
		int type = hs & L42_HSDET_TYPE_MASK;

		l42_write(c, d, L42_CODEC_INT_MASK, 0xff);
		l42_write(c, d, L42_HSDET_CTL2, 0x80);
		if (d->no_type_detect) {
			cs8409_set_jack(c, d, plug == L42_TS_PLUGGED, 0);
		} else {
			switch (type) {
			case L42_PLUG_CTIA:
			case L42_PLUG_OMTP:
				cs8409_set_jack(c, d, 1, 1);
				break;
			case L42_PLUG_HEADPHONE:
				cs8409_set_jack(c, d, 1, 0);
				break;
			default:
				/* undecided: trust the tip sense */
				cs8409_set_jack(c, d, plug == L42_TS_PLUGGED,
						0);
				break;
			}
		}
		/* HP output ground clamp and pull-down back on, tip sense
		 * interrupt back on */
		l42_write(c, d, L42_DAC_CTL2, 0x02);
		l42_write(c, d, L42_TSRS_PLUG_INT_MASK, 0xf3);
		return 1;
	}
	if (plug == L42_TS_PLUGGED) {
		if (d->no_type_detect) {
			cs8409_set_jack(c, d, 1, 0);
			return 1;
		}
		/* tell headset from headphone; the result interrupts */
		l42_run_type_detect(c, d);
		return 0;
	}
	if (plug == L42_TS_UNPLUGGED) {
		cs8409_set_jack(c, d, 0, 0);
		return 1;
	}
	return 0; /* in transition */
}

static void cs8409_hw_init(struct hda_codec *c, int board)
{
	struct cs8409_layout l;
	uint32_t mask, dir, data;
	int i;

	cs8409_layout(board, &l);
	if (!l.nl42) {
		hda_apply_verbs(c, cs8409_init_verbs);
		return;
	}
	hda_apply_verbs(c, l.init_verbs);

	/* GPIO 0-5: codec resets and speaker power-down out, codec
	 * interrupts in; codecs held in reset, speaker amp powered */
	mask = 0x3f;
	dir = l.spk_pdn_gpio;
	for (i = 0; i < l.nl42; i++)
		dir |= l.l42[i].reset_gpio;
	data = l.spk_pdn_gpio;
	cs_gpio_write(c, mask, dir, data);

	cs8409_coef_seq(c, l.cfg);
	if (board == CS8409_BULLSEYE)
		cs8409_coef_seq(c, cs8409_bullseye_eq);
	switch (board) {
	case CS8409_CYBORG:
	case CS8409_WARLOCK_MLK_DUAL_MIC:
		cs_coef_set(c, CS8409_VNID, CS8409_DMIC_CFG, 0x0003);
		break;
	case CS8409_ODIN:
		cs_coef_set(c, CS8409_VNID, CS8409_PAD_CFG_SLW_RATE_CTRL,
			    0xfc00);
		break;
	default:
		break;
	}

	for (i = 0; i < l.nl42; i++) {
		const struct l42_desc *d = &l.l42[i];
		int plugged;

		if (l42_resume(c, d))
			continue;
		/* initial state from the tip sense; a headset gets typed
		 * by the detection the interrupt reports later */
		plugged = l42_tip_plugged(c, d);
		if (plugged > 0) {
			cs8409_set_jack(c, d, 1, 0);
			if (!d->no_type_detect)
				l42_run_type_detect(c, d);
		} else if (plugged == 0) {
			cs8409_set_jack(c, d, 0, 0);
		}
	}
	cs8409_enable_unsol(c, &l, 1);
	cs8409_route(c, &l);
}

/* The serial ports are clocked for 48 kHz only. */
static void cs8409_fix_rates(struct hda_codec *c)
{
	unsigned int i;

	if (!c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];
		uint32_t pcm;

		if (w->type != AC_WID_AUD_OUT && w->type != AC_WID_AUD_IN)
			continue;
		pcm = w->pcm ? w->pcm : c->afg_pcm;
		if (!pcm || pcm == 0xffffffffu)
			pcm = AC_SUPPCM_BITS_16 | AC_SUPPCM_BITS_24;
		pcm &= ~AC_SUPPCM_RATES;
		pcm |= 1u << 6; /* 48 kHz */
		w->pcm = pcm;
		w->caps |= AC_WCAP_FORMAT_OVRD;
	}
}

/* ---- driver callbacks ------------------------------------------------------------ */

static int cs_match(struct hda_codec *c)
{
	return (c->vendor_id >> 16) == 0x1013 && c->afg != 0;
}

static int cs_pre_parse(struct hda_codec *c)
{
	const struct cs_chip *chip = cs_find_chip(c->vendor_id);
	int kind = cs_kind(c);

	cs_copy_name(c->vendor_name, sizeof(c->vendor_name), "Cirrus Logic");
	if (chip)
		cs_copy_name(c->chip_name, sizeof(c->chip_name), chip->name);
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "CS%04x",
			  (unsigned int)(c->vendor_id & 0xffff));

	if (kind == CS_8409) {
		int board = cs8409_board(c);
		struct cs8409_layout l;

		cs8409_layout(board, &l);
		if (l.pins)
			hda_apply_pincfgs(c, l.pins);
		cs8409_fix_rates(c);
		if (board != CS8409_NONE)
			cs_copy_name(c->chip_name, sizeof(c->chip_name),
				     "CS8409/CS42L42");
		kprintf("[snd] hda: codec %d: Cirrus Logic CS8409 bridge, %s\n",
			c->addr, cs8409_board_names[board]);
		return 0;
	} else {
		const struct cs_model *m = cs_lookup(c);
		uint32_t hp, spk;

		switch (kind) {
		case CS_4208: {
			static const uint8_t adcs[] = { 0x18, 0x1b, 0x1c };
			struct hda_widget *w = hda_widget(c, 0x18);
			unsigned int i;

			/* port 0x18 is stereo although it says otherwise,
			 * and the input amplifiers' 0 dB point is step 2 */
			if (w)
				w->caps |= AC_WCAP_STEREO;
			for (i = 0; i < sizeof(adcs); i++) {
				uint32_t caps =
					hda_amp_caps(c, adcs[i], HDA_INPUT);

				if (!caps || caps == 0xffffffffu)
					continue;
				caps &= ~AC_AMPCAP_OFFSET;
				caps |= 0x02;
				cs_override_amp_caps(c, adcs[i], HDA_INPUT,
						     caps);
			}
			break;
		}
		case CS_4210:
		case CS_4213:
			cs_dac_max_0db(c, 0x02);
			break;
		default:
			break;
		}
		if (m) {
			if (m->pins)
				hda_apply_pincfgs(c, m->pins);
			if (m->quirks & CSQ_INV_HP_JACK) {
				/* the headphone sense of 0x10 reads
				 * backwards: do without it */
				struct hda_widget *w = hda_widget(c, 0x10);

				if (w) {
					w->pincap &= ~(AC_PINCAP_PRES_DETECT |
						       AC_PINCAP_TRIG_REQ);
					w->caps &= ~AC_WCAP_UNSOL_CAP;
				}
			}
			cs_amp_gpios(m, &hp, &spk);
			if (hp | spk)
				cs_gpio_write(c, hp | spk, hp | spk, spk);
			kprintf("[snd] hda: codec %d: %s %s, board fixup "
				"\"%s\"\n",
				c->addr, c->vendor_name, c->chip_name, m->name);
		} else {
			kprintf("[snd] hda: codec %d: %s %s, BIOS pin "
				"configuration\n",
				c->addr, c->vendor_name, c->chip_name);
		}
		if (kind == CS_4210)
			cs4210_pinmux(c, m);
	}
	return 0;
}

static int cs_post_parse(struct hda_codec *c)
{
	int kind = cs_kind(c);

	if (kind == CS_8409) {
		int board = cs8409_board(c);
		int i;

		if (board != CS8409_NONE) {
			/* the bridge pins cannot sense: the companion codec
			 * reports the jacks and this driver switches */
			c->spec.auto_mute = 0;
			c->spec.jack_poll = 0;
			for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS;
			     i++) {
				c->spec.out[i].jack_detect = 0;
				c->spec.out[i].eapd = 0;
			}
		}
		/* the bridge, the companion codecs and the jack state are
		 * brought up by init, which runs after the generic setup */
		return 0;
	}
	/* amplifier GPIOs for the parsed outputs; the coefficient setup
	 * follows in init */
	cs_update_amps(c, cs_lookup(c), cs_hp_present(c));
	return 0;
}

static int cs_init(struct hda_codec *c)
{
	if (cs_kind(c) == CS_8409) {
		int board = cs8409_board(c);
		struct cs8409_layout l;

		cs8409_layout(board, &l);
		if (l.pins)
			hda_apply_pincfgs(c, l.pins);
		cs8409_hw_init(c, board);
		return 0;
	}
	cs42xx_hw_init(c, cs_lookup(c));
	return 0;
}

static void cs_automute(struct hda_codec *c, int hp_present)
{
	if (cs_kind(c) == CS_8409) {
		struct cs8409_layout l;

		cs8409_layout(cs8409_board(c), &l);
		if (l.nl42)
			cs8409_route(c, &l);
		return;
	}
	cs_update_amps(c, cs_lookup(c), hp_present);
}

static void cs_unsol(struct hda_codec *c, uint32_t res)
{
	struct cs8409_layout l;
	uint32_t tag = (res & AC_UNSOL_RES_TAG) >> AC_UNSOL_RES_TAG_SHIFT;
	int changed = 0;
	int i;

	if (cs_kind(c) != CS_8409) {
		hda_codec_update_routing(c);
		return;
	}
	if (tag != CS8409_UNSOL_TAG)
		return;
	cs8409_layout(cs8409_board(c), &l);
	for (i = 0; i < l.nl42; i++) {
		/* the interrupt line is active low; a high line means this
		 * codec has nothing pending (it rose because the status
		 * read of the previous event cleared it) */
		if (res & l.l42[i].irq_gpio)
			continue;
		changed |= cs8409_l42_event(c, &l.l42[i]);
	}
	if (changed) {
		cs8409_route(c, &l);
		hda_codec_update_routing(c);
	}
}

static void cs_playback_hook(struct hda_codec *c, int start)
{
	if (!start)
		return;
	if (cs_kind(c) == CS_8409) {
		struct cs8409_layout l;

		cs8409_layout(cs8409_board(c), &l);
		if (l.nl42)
			cs8409_route(c, &l);
		return;
	}
	cs_update_amps(c, cs_lookup(c), cs_hp_present(c));
}

const struct hda_vendor_driver hda_cirrus_driver = {
	.name = "cirrus",
	.match = cs_match,
	.pre_parse = cs_pre_parse,
	.post_parse = cs_post_parse,
	.init = cs_init,
	.automute = cs_automute,
	.unsol = cs_unsol,
	.playback_hook = cs_playback_hook,
	.probe = 0,
	.remove = 0,
};
