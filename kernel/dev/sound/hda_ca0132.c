// LikeOS HD Audio - Creative codecs.
//
// Creative's Sound Core3D (CA0132) carries a DSP that normally runs a
// firmware image for effects, and on the desktop cards also routes the
// analog outputs.  This driver runs it without firmware: the converter
// feeds the output ports directly through the chip's internal crossbar
// ("DAC to port" control parameter) and the speaker/headphone selection
// is done with the vendor EAPD select verb and the pin controls.  That is
// enough for the laptop parts (Alienware, Dell, ASUS, MSI) and the
// Recon3Di found on Gigabyte/EVGA boards, whose output relay is an HDA
// GPIO.  The Sound Blaster Z/ZxR/Recon3D/AE-5/AE-7 cards switch their
// analog relays through a separate memory-mapped GPIO block the HDA link
// cannot reach; on those the crossbar and HDA GPIOs are set and the rear
// line output plays.
//
// Chip I/O goes through the vendor widget 0x15 ("chip control"):
// control flags (one bit each), control parameters (small values direct,
// larger ones through an extended id/value pair after a status check),
// and 32-bit chip memory writes.
//
// The CA0110 (X-Fi Xtreme Audio, SB0880) is a plain codec parsed
// generically; it only gets its name here.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>
#include <kernel/hal/lapic.h>

#define CA_CHIP_CTRL 0x15 /* vendor chip I/O widget */
#define CA_DSP_CTRL 0x16 /* vendor DSP I/O widget */

/* vendor verbs on CA_CHIP_CTRL */
#define CA_CHIPIO_ADDRESS_LOW 0x000
#define CA_CHIPIO_ADDRESS_HIGH 0x100
#define CA_CHIPIO_DATA_LOW 0x300
#define CA_CHIPIO_DATA_HIGH 0x400
#define CA_CHIPIO_STATUS 0xf01
#define CA_CHIPIO_8051_DATA_WRITE 0x707
#define CA_CHIPIO_CT_EXT_ENABLE 0x70a
#define CA_CHIPIO_8051_ADDR_LOW 0x70d
#define CA_CHIPIO_8051_ADDR_HIGH 0x70e
#define CA_CHIPIO_FLAG_SET 0x70f
#define CA_CHIPIO_PARAM_SET 0x710
#define CA_CHIPIO_PARAM_EX_ID_SET 0x717
#define CA_CHIPIO_PARAM_EX_VALUE_SET 0x718
#define CA_CHIPIO_EAPD_SEL_SET 0x78d

/* control flags */
#define CA_FLAG_IDLE_ENABLE 2
#define CA_FLAG_ADC_C_HIGH_PASS 17
#define CA_FLAG_PORT_A_COMMON_MODE 18
#define CA_FLAG_PORT_D_COMMON_MODE 19
#define CA_FLAG_PORT_A_10KOHM_LOAD 20
#define CA_FLAG_PORT_D_10KOHM_LOAD 21

/* control parameters */
#define CA_PARAM_PORTA_160OHM_GAIN 8
#define CA_PARAM_PORTD_160OHM_GAIN 10
#define CA_PARAM_DAC2PORT 0x0d

/* ---- boards ------------------------------------------------------------------------ */

enum ca_quirk {
	CA_Q_NONE = 0, /* laptop CA0132 */
	CA_Q_ALIENWARE,
	CA_Q_ALIENWARE_M17XR4,
	CA_Q_SBZ,
	CA_Q_ZXR,
	CA_Q_ZXR_DBPRO,
	CA_Q_R3D,
	CA_Q_R3DI,
	CA_Q_AE5,
	CA_Q_AE7,
};

struct ca_board {
	uint32_t ssid; /* PCI subsystem id */
	uint8_t quirk;
	const char *name;
};

static const struct ca_board ca_boards[] = {
	{ 0x1028057b, CA_Q_ALIENWARE_M17XR4, "Alienware M17x R4" },
	{ 0x10280685, CA_Q_ALIENWARE, "Alienware 15 2015" },
	{ 0x10280688, CA_Q_ALIENWARE, "Alienware 17 2015" },
	{ 0x10280708, CA_Q_ALIENWARE, "Alienware 15 R2 2016" },
	{ 0x11020010, CA_Q_SBZ, "Sound Blaster Z" },
	{ 0x11020023, CA_Q_SBZ, "Sound Blaster Z" },
	{ 0x11020027, CA_Q_SBZ, "Sound Blaster Z" },
	{ 0x11020033, CA_Q_SBZ, "Sound Blaster ZxR" },
	{ 0x1458a016, CA_Q_R3DI, "Recon3Di" },
	{ 0x1458a026, CA_Q_R3DI, "Gigabyte G1.Sniper Z97" },
	{ 0x1458a036, CA_Q_R3DI, "Gigabyte GA-Z170X-Gaming 7" },
	{ 0x38421038, CA_Q_R3DI, "EVGA X99 Classified" },
	{ 0x3842104b, CA_Q_R3DI, "EVGA X299 Dark" },
	{ 0x38421055, CA_Q_R3DI, "EVGA Z390 DARK" },
	{ 0x11020013, CA_Q_R3D, "Recon3D" },
	{ 0x11020018, CA_Q_R3D, "Recon3D" },
	{ 0x11020051, CA_Q_AE5, "Sound BlasterX AE-5" },
	{ 0x11020191, CA_Q_AE5, "Sound BlasterX AE-5 Plus" },
	{ 0x11020081, CA_Q_AE7, "Sound Blaster AE-7" },
	{ 0, 0, 0 }
};

/* Pin layouts from the boards' own drivers */
static const struct hda_pincfg ca_alienware_pins[] = {
	{ 0x0b, 0x90170110 }, /* built-in speaker */
	{ 0x0c, 0x411111f0 }, { 0x0d, 0x411111f0 }, { 0x0e, 0x411111f0 },
	{ 0x0f, 0x0321101f }, /* headphone */
	{ 0x10, 0x411111f0 }, /* headset: unused */
	{ 0x11, 0x03a11021 }, /* microphone */
	{ 0x12, 0xd5a30140 }, /* built-in microphone */
	{ 0x13, 0x411111f0 }, { 0x18, 0x411111f0 }, { 0, 0 }
};
static const struct hda_pincfg ca_sbz_pins[] = {
	{ 0x0b, 0x01017010 }, /* port G: line out front */
	{ 0x0c, 0x014510f0 }, /* S/PDIF out 1 */
	{ 0x0d, 0x014510f0 }, /* digital out */
	{ 0x0e, 0x01c510f0 }, /* S/PDIF in */
	{ 0x0f, 0x0221701f }, /* port A: back panel headphone */
	{ 0x10, 0x01017012 }, /* port D: center/LFE or front HP */
	{ 0x11, 0x01017014 }, /* port B: rear */
	{ 0x12, 0x01a170f0 }, /* port C: line in 1 */
	{ 0x13, 0x908700f0 }, /* what-you-hear in */
	{ 0x18, 0x50d000f0 }, { 0, 0 }
};
static const struct hda_pincfg ca_zxr_pins[] = {
	{ 0x0b, 0x01047110 }, { 0x0c, 0x414510f0 }, { 0x0d, 0x014510f0 },
	{ 0x0e, 0x41c520f0 }, { 0x0f, 0x0122711f }, { 0x10, 0x01017111 },
	{ 0x11, 0x01017114 }, { 0x12, 0x01a271f0 }, { 0x13, 0x908700f0 },
	{ 0x18, 0x50d000f0 }, { 0, 0 }
};
static const struct hda_pincfg ca_r3d_pins[] = {
	{ 0x0b, 0x01014110 }, { 0x0c, 0x014510f0 }, { 0x0d, 0x014510f0 },
	{ 0x0e, 0x01c520f0 }, { 0x0f, 0x0221401f }, { 0x10, 0x01016011 },
	{ 0x11, 0x01011014 }, { 0x12, 0x02a090f0 }, { 0x13, 0x908700f0 },
	{ 0x18, 0x50d000f0 }, { 0, 0 }
};
static const struct hda_pincfg ca_ae5_pins[] = {
	{ 0x0b, 0x01017010 }, { 0x0c, 0x014510f0 }, { 0x0d, 0x014510f0 },
	{ 0x0e, 0x01c510f0 }, { 0x0f, 0x01017114 }, { 0x10, 0x01017012 },
	{ 0x11, 0x012170ff }, /* port B: rear headphone */
	{ 0x12, 0x01a170f0 }, { 0x13, 0x908700f0 }, { 0x18, 0x50d000f0 },
	{ 0, 0 }
};
static const struct hda_pincfg ca_r3di_pins[] = {
	{ 0x0b, 0x01014110 }, { 0x0c, 0x014510f0 }, { 0x0d, 0x014510f0 },
	{ 0x0e, 0x41c520f0 }, { 0x0f, 0x0221401f }, { 0x10, 0x01016011 },
	{ 0x11, 0x01011014 }, { 0x12, 0x02a090f0 }, { 0x13, 0x908700f0 },
	{ 0x18, 0x500000f0 }, { 0, 0 }
};
static const struct hda_pincfg ca_ae7_pins[] = {
	{ 0x0b, 0x01017010 }, { 0x0c, 0x014510f0 }, { 0x0d, 0x414510f0 },
	{ 0x0e, 0x01c520f0 }, { 0x0f, 0x01017114 }, { 0x10, 0x01017011 },
	{ 0x11, 0x018170ff }, { 0x12, 0x01a170f0 }, { 0x13, 0x908700f0 },
	{ 0x18, 0x500000f0 }, { 0, 0 }
};

/* Per-board output plumbing */
struct ca_layout {
	const struct hda_pincfg *pins;
	uint8_t spk_pin, hp_pin; /* main output, headphone */
	uint8_t dac2port_spk, dac2port_hp; /* crossbar values, 0 = none */
	uint8_t desktop; /* the effects/relay design of the cards */
	uint8_t hda_gpio_relay; /* GPIO bit set for speakers (R3Di) */
};

static void ca_layout(int quirk, struct ca_layout *l)
{
	kmemset(l, 0, sizeof(*l));
	l->spk_pin = 0x0b;
	l->hp_pin = 0x10;
	l->dac2port_spk = l->dac2port_hp = 0x20;
	switch (quirk) {
	case CA_Q_ALIENWARE:
	case CA_Q_ALIENWARE_M17XR4:
		l->pins = ca_alienware_pins;
		l->hp_pin = 0x0f;
		break;
	case CA_Q_SBZ:
		l->pins = ca_sbz_pins;
		l->hp_pin = 0x0f;
		l->dac2port_spk = 0x18;
		l->dac2port_hp = 0x12;
		l->desktop = 1;
		break;
	case CA_Q_ZXR:
	case CA_Q_ZXR_DBPRO:
		l->pins = ca_zxr_pins;
		l->hp_pin = 0x0f;
		l->dac2port_spk = 0x24;
		l->dac2port_hp = 0x21;
		l->desktop = 1;
		break;
	case CA_Q_R3D:
		l->pins = ca_r3d_pins;
		l->hp_pin = 0x0f;
		l->dac2port_spk = 0x24;
		l->dac2port_hp = 0x21;
		l->desktop = 1;
		break;
	case CA_Q_R3DI:
		l->pins = ca_r3di_pins;
		l->hp_pin = 0x0f;
		l->dac2port_spk = 0x24;
		l->dac2port_hp = 0x21;
		l->desktop = 1;
		l->hda_gpio_relay = 1u << 2;
		break;
	case CA_Q_AE5:
		l->pins = ca_ae5_pins;
		l->hp_pin = 0x11;
		l->dac2port_spk = 0xa4;
		l->dac2port_hp = 0xa1;
		l->desktop = 1;
		break;
	case CA_Q_AE7:
		l->pins = ca_ae7_pins;
		l->hp_pin = 0x11;
		l->dac2port_spk = 0x58;
		l->dac2port_hp = 0x58;
		l->desktop = 1;
		break;
	default:
		break;
	}
}

/* 8051 patch words the chip wants at start-up (non-desktop parts) */
static const struct hda_verb ca_chip_init_verbs[] = {
	{ 0x15, 0x70d, 0xf0 }, { 0x15, 0x70e, 0xfe }, { 0x15, 0x707, 0x75 },
	{ 0x15, 0x707, 0xd3 }, { 0x15, 0x707, 0x09 }, { 0x15, 0x707, 0x53 },
	{ 0x15, 0x707, 0xd4 }, { 0x15, 0x707, 0xef }, { 0x15, 0x707, 0x75 },
	{ 0x15, 0x707, 0xd3 }, { 0x15, 0x707, 0x09 }, { 0x15, 0x707, 0x02 },
	{ 0x15, 0x707, 0x37 }, { 0x15, 0x707, 0x78 }, { 0x15, 0x53c, 0xce },
	{ 0x15, 0x575, 0xc9 }, { 0x15, 0x53d, 0xce }, { 0x15, 0x5b7, 0xc9 },
	{ 0x15, 0x70d, 0xe8 }, { 0x15, 0x70e, 0xfe }, { 0x15, 0x707, 0x02 },
	{ 0x15, 0x707, 0x68 }, { 0x15, 0x707, 0x62 }, { 0x15, 0x53a, 0xce },
	{ 0x15, 0x546, 0xc9 }, { 0x15, 0x53b, 0xce }, { 0x15, 0x5e8, 0xc9 },
	{ 0, 0, 0 }
};

/* ---- identification ------------------------------------------------------------- */

static int ca_is_ca0132(const struct hda_codec *c)
{
	return c->vendor_id == 0x11020011;
}

static uint32_t ca_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	return ((v & 0xffffu) << 16) | (v >> 16);
}

static const struct ca_board *ca_board(const struct hda_codec *c)
{
	uint32_t ssid = ca_pci_ssid(c);
	int i;

	if (!ssid)
		return 0;
	for (i = 0; ca_boards[i].ssid; i++)
		if (ca_boards[i].ssid == ssid)
			return &ca_boards[i];
	return 0;
}

static int ca_quirk(const struct hda_codec *c)
{
	const struct ca_board *b = ca_board(c);
	int q;

	if (!ca_is_ca0132(c) || !b)
		return CA_Q_NONE;
	q = b->quirk;
	if (q == CA_Q_SBZ) {
		/* the ZxR shares PCI ids with the Z; its codec id differs */
		if (c->subsystem_id == 0x11020033)
			q = CA_Q_ZXR;
		else if (c->subsystem_id == 0x1102003f)
			q = CA_Q_ZXR_DBPRO;
	}
	return q;
}

static const char *ca_chip_name(uint32_t id)
{
	switch (id) {
	case 0x11020011:
		return "CA0132";
	case 0x1102000a:
	case 0x1102000b:
		return "CA0110-IBG";
	case 0x1102000d:
		return "SB0880 X-Fi";
	default:
		return 0;
	}
}

/* ---- chip I/O ------------------------------------------------------------------ */

/* Send a status-checked chip I/O command: the chip answers 0 when it took
 * it, 1 while busy.  Bounded at ~1 s. */
static int ca_chipio_send(struct hda_codec *c, uint32_t verb, uint32_t data)
{
	int i;

	for (i = 0; i < 50; i++) {
		uint32_t res = hda_codec_read(c, CA_CHIP_CTRL, verb, data);

		if (res == 0)
			return 0;
		if (res == 0xffffffffu)
			return -EIO;
		lapic_delay_ms(20);
	}
	return -ETIMEDOUT;
}

static void ca_set_flag(struct hda_codec *c, unsigned int flag, int on)
{
	hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_FLAG_SET,
			((on ? 1u : 0u) << 7) | (flag & 0x7f));
}

static void ca_set_param(struct hda_codec *c, unsigned int id,
			 unsigned int val)
{
	if (id < 32 && val < 8) {
		hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_PARAM_SET,
				(val << 5) | id);
		return;
	}
	if (ca_chipio_send(c, CA_CHIPIO_STATUS, 0) == 0) {
		hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_PARAM_EX_ID_SET,
				id & 0xff);
		hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_PARAM_EX_VALUE_SET,
				val & 0xff);
	}
}

/* 32-bit write into chip memory: address halves, then data halves. */
static int ca_chipio_write(struct hda_codec *c, uint32_t addr, uint32_t data)
{
	int err;

	err = ca_chipio_send(c, CA_CHIPIO_ADDRESS_LOW, addr & 0xffff);
	if (!err)
		err = ca_chipio_send(c, CA_CHIPIO_ADDRESS_HIGH, addr >> 16);
	if (!err)
		err = ca_chipio_send(c, CA_CHIPIO_DATA_LOW, data & 0xffff);
	if (!err)
		err = ca_chipio_send(c, CA_CHIPIO_DATA_HIGH, data >> 16);
	return err;
}

/* ---- outputs ----------------------------------------------------------------------- */

static void ca_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static int ca_is_pin(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	return w && w->type == AC_WID_PIN;
}

/* Pin on, its output amplifier and the DAC's open at 0 dB. */
static void ca_init_output(struct hda_codec *c, uint8_t pin, uint8_t dac)
{
	struct hda_widget *w = hda_widget(c, pin);
	uint32_t caps, g;

	if (!w || w->type != AC_WID_PIN)
		return;
	if (w->caps & AC_WCAP_OUT_AMP) {
		caps = hda_amp_caps(c, pin, HDA_OUTPUT);
		g = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;
		hda_amp_set(c, pin, HDA_OUTPUT, 0, g, g, 0);
	}
	w = hda_widget(c, dac);
	if (w && (w->caps & AC_WCAP_OUT_AMP)) {
		caps = hda_amp_caps(c, dac, HDA_OUTPUT);
		g = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;
		hda_amp_set(c, dac, HDA_OUTPUT, 0, g, g, 0);
	}
}

/*
 * Speaker or headphone: the EAPD select routes the chip's amplifier
 * enable to one port, the pin controls switch the drivers, and the
 * crossbar sends the DAC to the right analog port.
 */
static void ca_select_out(struct hda_codec *c, int hp)
{
	struct ca_layout l;
	int q = ca_quirk(c);

	ca_layout(q, &l);
	if (!ca_is_pin(c, l.spk_pin) || !ca_is_pin(c, l.hp_pin))
		return;

	if (!hp) {
		hda_codec_write(c, l.hp_pin, CA_CHIPIO_EAPD_SEL_SET, 0x02);
		hda_codec_write(c, l.spk_pin, AC_VERB_SET_EAPD_BTLENABLE, 0x00);
		hda_codec_write(c, l.spk_pin, CA_CHIPIO_EAPD_SEL_SET, 0x00);
		hda_codec_write(c, l.spk_pin, AC_VERB_SET_EAPD_BTLENABLE,
				AC_EAPDBTL_EAPD);
		hda_pin_set_ctl(c, l.hp_pin, 0);
		hda_pin_set_ctl(c, l.spk_pin, AC_PINCTL_OUT_EN);
	} else {
		hda_codec_write(c, l.spk_pin, CA_CHIPIO_EAPD_SEL_SET, 0x00);
		hda_codec_write(c, l.spk_pin, AC_VERB_SET_EAPD_BTLENABLE, 0x00);
		hda_codec_write(c, l.hp_pin, CA_CHIPIO_EAPD_SEL_SET, 0x02);
		hda_codec_write(c, l.spk_pin, AC_VERB_SET_EAPD_BTLENABLE,
				AC_EAPDBTL_EAPD);
		hda_pin_set_ctl(c, l.spk_pin, 0);
		hda_pin_set_ctl(c, l.hp_pin,
				AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN);
	}

	if (l.desktop) {
		uint8_t port = hp ? l.dac2port_hp : l.dac2port_spk;

		if (l.hda_gpio_relay && c->afg) {
			/* Recon3Di: the output relay is GPIO2 */
			uint32_t data = c->gpio_data;

			if (hp)
				data &= ~(uint32_t)l.hda_gpio_relay;
			else
				data |= l.hda_gpio_relay;
			hda_gpio_set(c, c->gpio_mask, c->gpio_dir, data & 0xff);
			c->gpio_data = data & 0xff;
		}
		if (port)
			ca_set_param(c, CA_PARAM_DAC2PORT, port);
		if (q == CA_Q_AE5 && !hp)
			ca_chipio_write(c, 0x0018b03c, 0x00000012);
	}
}

static int ca_hp_present(struct hda_codec *c)
{
	struct ca_layout l;
	int i;

	ca_layout(ca_quirk(c), &l);
	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++) {
		const struct hda_out *o = &c->spec.out[i];

		if (o->pin == l.hp_pin)
			return o->jack_detect ? o->present : 0;
	}
	/* not parsed: ask the pin */
	if (ca_is_pin(c, l.hp_pin)) {
		struct hda_widget *w = hda_widget(c, l.hp_pin);

		if (w->pincap & AC_PINCAP_PRES_DETECT)
			return hda_pin_sense(c, l.hp_pin) == 1;
	}
	return 0;
}

/* Desktop cards: GPIO setup through the function group. */
static void ca_desktop_gpio(struct hda_codec *c, int q)
{
	if (!c->afg)
		return;
	switch (q) {
	case CA_Q_SBZ:
	case CA_Q_AE5:
	case CA_Q_AE7:
		hda_codec_write(c, c->afg, 0x793, 0x00);
		hda_codec_write(c, c->afg, 0x794, 0x53);
		hda_codec_write(c, c->afg, 0x790, 0x23);
		break;
	case CA_Q_R3DI:
		hda_codec_write(c, c->afg, 0x793, 0x00);
		hda_codec_write(c, c->afg, 0x794, 0x5b);
		break;
	default:
		break;
	}
	switch (q) {
	case CA_Q_SBZ:
		hda_gpio_set(c, 0x07, 0x07, 0x04);
		hda_gpio_set(c, 0x07, 0x07, 0x06);
		c->gpio_mask = c->gpio_dir = 0x07;
		c->gpio_data = 0x06;
		break;
	case CA_Q_R3DI:
		hda_gpio_set(c, 0x1f, 0x1e, 0x0c);
		c->gpio_mask = 0x1f;
		c->gpio_dir = 0x1e;
		c->gpio_data = 0x0c;
		break;
	default:
		break;
	}
}

static void ca0132_chip_init(struct hda_codec *c)
{
	int q = ca_quirk(c);
	struct ca_layout l;

	ca_layout(q, &l);
	/* the creative extensions of the chip I/O widget */
	hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_CT_EXT_ENABLE, 0x1);

	/* analog output stage: no idle mode, no common mode, low-impedance
	 * ramp generators, ADC high pass; headphone stage gain */
	ca_set_param(c, CA_PARAM_PORTA_160OHM_GAIN, 6);
	ca_set_param(c, CA_PARAM_PORTD_160OHM_GAIN, 6);
	ca_set_flag(c, CA_FLAG_IDLE_ENABLE, 0);
	ca_set_flag(c, CA_FLAG_PORT_A_COMMON_MODE, 0);
	ca_set_flag(c, CA_FLAG_PORT_D_COMMON_MODE, 0);
	ca_set_flag(c, CA_FLAG_PORT_A_10KOHM_LOAD, l.desktop ? 1 : 0);
	ca_set_flag(c, CA_FLAG_PORT_D_10KOHM_LOAD, 0);
	ca_set_flag(c, CA_FLAG_ADC_C_HIGH_PASS, 1);

	if (!l.desktop) {
		hda_apply_verbs(c, ca_chip_init_verbs);
		/* DAC straight to the analog ports */
		hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_PARAM_EX_ID_SET,
				CA_PARAM_DAC2PORT);
		hda_codec_write(c, CA_CHIP_CTRL, CA_CHIPIO_PARAM_EX_VALUE_SET,
				0x20);
	} else {
		ca_desktop_gpio(c, q);
	}

	/* EAPD select starts on the speaker port */
	if (ca_is_pin(c, 0x0b))
		hda_codec_write(c, 0x0b, CA_CHIPIO_EAPD_SEL_SET, 0x00);

	ca_init_output(c, l.spk_pin, 0x02);
	ca_init_output(c, l.hp_pin, 0x02);
	ca_select_out(c, ca_hp_present(c));
}

/* No parsed output at all (the graph runs through the DSP on some
 * revisions): fall back to the fixed DAC 0x02 -> port layout. */
static void ca_force_outputs(struct hda_codec *c)
{
	struct hda_spec *s = &c->spec;
	struct ca_layout l;
	uint8_t pins[2];
	int i, n = 0;

	if (s->nout > 0)
		return;
	ca_layout(ca_quirk(c), &l);
	pins[0] = l.spk_pin;
	pins[1] = l.hp_pin;
	for (i = 0; i < 2; i++) {
		struct hda_widget *pw = hda_widget(c, pins[i]);
		struct hda_widget *dw = hda_widget(c, 0x02);
		struct hda_out *o;
		int j;

		if (!pw || pw->type != AC_WID_PIN || !dw ||
		    dw->type != AC_WID_AUD_OUT || n >= HDA_MAX_OUTPUTS)
			continue;
		o = &s->out[n];
		kmemset(o, 0, sizeof(*o));
		o->id = (uint32_t)n;
		o->type = i ? AUDIO_OUT_HEADPHONE : AUDIO_OUT_SPEAKER;
		o->pin = pins[i];
		o->dac = 0x02;
		o->present = 1;
		o->hp_capable = (pw->pincap & AC_PINCAP_HP_DRV) ? 1 : 0;
		o->jack_detect = i && (pw->pincap & AC_PINCAP_PRES_DETECT);
		o->path.nid[0] = 0x02;
		o->path.nid[1] = pins[i];
		o->path.idx[0] = 0;
		o->path.idx[1] = 0;
		for (j = 0; j < pw->nconn && j < HDA_MAX_CONNS; j++)
			if (pw->conn[j] == 0x02)
				o->path.idx[1] = (uint8_t)j;
		o->path.depth = 2;
		o->path.active = 1;
		if (dw->caps & AC_WCAP_OUT_AMP) {
			o->path.vol_nid = o->path.mute_nid = 0x02;
			o->path.vol_dir = o->path.mute_dir = HDA_OUTPUT;
		}
		ksnprintf(o->name, sizeof(o->name), "%s",
			  i ? "Headphone" : "Speaker");
		n++;
	}
	s->nout = n;
	if (n && !s->primary_dac)
		s->primary_dac = 0x02;
	if (n)
		kprintf("[snd] hda: codec %d: CA0132 graph without a parsed "
			"path, using DAC 0x02 to ports 0x%02x/0x%02x\n",
			c->addr, l.spk_pin, l.hp_pin);
}

/* ---- driver callbacks --------------------------------------------------------- */

static int ca_match(struct hda_codec *c)
{
	return (c->vendor_id >> 16) == 0x1102 && c->afg != 0;
}

static int ca_pre_parse(struct hda_codec *c)
{
	const char *name = ca_chip_name(c->vendor_id);
	const struct ca_board *b;
	struct ca_layout l;
	int q;

	ca_copy_name(c->vendor_name, sizeof(c->vendor_name), "Creative");
	if (name)
		ca_copy_name(c->chip_name, sizeof(c->chip_name), name);
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "CA%04x",
			  (unsigned int)(c->vendor_id & 0xffff));

	if (!ca_is_ca0132(c)) {
		kprintf("[snd] hda: codec %d: Creative %s\n", c->addr,
			c->chip_name);
		return 0;
	}

	b = ca_board(c);
	q = ca_quirk(c);
	ca_layout(q, &l);
	if (l.pins)
		hda_apply_pincfgs(c, l.pins);
	kprintf("[snd] hda: codec %d: Creative CA0132 (%s), running without "
		"DSP firmware%s\n",
		c->addr,
		q == CA_Q_ZXR ? "Sound Blaster ZxR" :
		q == CA_Q_ZXR_DBPRO ? "ZxR DBPro" :
		b ? b->name : "laptop",
		l.desktop && q != CA_Q_R3DI ?
			": analog relays need the card's MMIO block, rear "
			"line out only" :
			"");

	/* the generic code must not toggle the EAPD of the two output
	 * ports: their amplifier enable is routed by the EAPD select */
	if (ca_is_pin(c, l.spk_pin))
		hda_widget(c, l.spk_pin)->eapd = 0;
	if (ca_is_pin(c, l.hp_pin))
		hda_widget(c, l.hp_pin)->eapd = 0;
	/* the chip I/O setup runs in init, after the generic setup */
	return 0;
}

static int ca_post_parse(struct hda_codec *c)
{
	struct ca_layout l;
	int i;

	if (!ca_is_ca0132(c))
		return 0;
	ca_force_outputs(c);
	ca_layout(ca_quirk(c), &l);
	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].pin == l.spk_pin ||
		    c->spec.out[i].pin == l.hp_pin)
			c->spec.out[i].eapd = 0;
	/* the analog stream only exists two-channel without the DSP */
	ca_init_output(c, l.spk_pin, c->spec.primary_dac ?
					      c->spec.primary_dac :
					      0x02);
	ca_init_output(c, l.hp_pin, c->spec.primary_dac ?
					    c->spec.primary_dac :
					    0x02);
	ca_select_out(c, ca_hp_present(c));
	return 0;
}

static int ca_init(struct hda_codec *c)
{
	struct ca_layout l;

	if (!ca_is_ca0132(c))
		return 0;
	ca_layout(ca_quirk(c), &l);
	if (l.pins)
		hda_apply_pincfgs(c, l.pins);
	ca0132_chip_init(c);
	return 0;
}

static void ca_automute(struct hda_codec *c, int hp_present)
{
	if (!ca_is_ca0132(c))
		return;
	ca_select_out(c, hp_present);
}

static void ca_unsol(struct hda_codec *c, uint32_t res)
{
	(void)res;
	if (!ca_is_ca0132(c))
		return;
	hda_codec_update_routing(c);
	ca_select_out(c, ca_hp_present(c));
}

static void ca_playback_hook(struct hda_codec *c, int start)
{
	(void)c;
	(void)start;
	/* nothing to stage without the DSP */
}

const struct hda_vendor_driver hda_ca0132_driver = {
	.name = "ca0132",
	.match = ca_match,
	.pre_parse = ca_pre_parse,
	.post_parse = ca_post_parse,
	.init = ca_init,
	.automute = ca_automute,
	.unsol = ca_unsol,
	.playback_hook = ca_playback_hook,
	.probe = 0,
	.remove = 0,
};
