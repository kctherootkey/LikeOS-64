// LikeOS HD Audio - Analog Devices SoundMAX codecs.
//
// AD1981HD/AD1983 (small two-channel parts of the ThinkPad T60/X60 and
// HP nx/tc era), AD1984/AD1984A/AD1984B/AD1884/AD1884A/AD1883 (ThinkPad
// T61/X61/R61, HP Pavilion/Compaq, HP TouchSmart), AD1986A (ASUS, Samsung,
// Lenovo 3000, Toshiba, also found in emulator setups), AD1882/AD1882A and
// AD1988/AD1988B/AD1989A/AD1989B (desktop boards).  All parsed from the
// BIOS pin configuration; this file adds what is specific to the parts:
//
//  * AD1986A implements EAPD inverted: the bit set means "amplifier off".
//    Its EAPD pins are taken away from the generic code and driven here
//    (amplifier on while the master is unmuted).  A few boards (Toshiba
//    Satellite L40, some ASUS) wired it the standard way.
//  * AD1981/AD1884: the analog loopback mixer amplifiers go beyond 0 dB
//    and distort the laptop speakers; their ranges are clamped at 0 dB.
//  * HP laptops switch the speaker amplifier (EAPD) with the master mute,
//    and the AD1884 ones carry the mute LED on GPIO1.
//  * ThinkPads with AD1984: EAPD of port D (0x12) follows the master, the
//    digital microphone needs a magic coefficient, firmware beeps go
//    through the analog mixer.
//  * AD1983: its capture selectors list loopback routes that confuse a
//    path search; only the direct ADC connections are kept.
//  * Board pin fixups (ASUS, Samsung, Lenovo, HP, PackardBell, FSC).
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>

enum ad_family {
	AD_1882 = 0,
	AD_1884, /* AD1884/1884A/1883/1984/1984A/1984B */
	AD_1981,
	AD_1983,
	AD_1986A,
	AD_1988, /* AD1988/1988B/1989A/1989B */
	AD_FAMILIES,
};

#define ADF(f) (1u << (f))

struct ad_chip {
	uint32_t id;
	const char *name;
	uint8_t family;
};

static const struct ad_chip ad_chips[] = {
	{ 0x11d41882, "AD1882", AD_1882 },
	{ 0x11d4882a, "AD1882A", AD_1882 },
	{ 0x11d41883, "AD1883", AD_1884 },
	{ 0x11d41884, "AD1884", AD_1884 },
	{ 0x11d4184a, "AD1884A", AD_1884 },
	{ 0x11d41984, "AD1984", AD_1884 },
	{ 0x11d4194a, "AD1984A", AD_1884 },
	{ 0x11d4194b, "AD1984B", AD_1884 },
	{ 0x11d41981, "AD1981", AD_1981 },
	{ 0x11d41983, "AD1983", AD_1983 },
	{ 0x11d41986, "AD1986A", AD_1986A },
	{ 0x11d41987, "AD1987", AD_1988 },
	{ 0x11d41988, "AD1988", AD_1988 },
	{ 0x11d4198b, "AD1988B", AD_1988 },
	{ 0x11d4989a, "AD1989A", AD_1988 },
	{ 0x11d4989b, "AD1989B", AD_1988 },
	{ 0, 0, 0 }
};

struct ad_family_info {
	const char *name;
	uint8_t mixer_nid; /* analog loopback mixer */
	uint8_t beep_nid; /* digital beep generator */
	uint8_t eapd_front, eapd_hp; /* pins whose EAPD powers the amps */
	uint8_t inv_eapd; /* EAPD bit set = amplifier off */
};

static const struct ad_family_info ad_families[AD_FAMILIES] = {
	[AD_1882] = { "AD1882", 0x20, 0x10, 0x12, 0x11, 0 },
	[AD_1884] = { "AD1884", 0x20, 0x10, 0x12, 0x11, 0 },
	[AD_1981] = { "AD1981", 0x0e, 0x10, 0x05, 0x06, 0 },
	[AD_1983] = { "AD1983", 0x0e, 0x10, 0x05, 0x06, 0 },
	[AD_1986A] = { "AD1986A", 0x07, 0x19, 0x1b, 0x1a, 1 },
	[AD_1988] = { "AD1988", 0x20, 0x10, 0x12, 0x11, 0 },
};

static const struct ad_chip *ad_find_chip(uint32_t id)
{
	int i;

	for (i = 0; ad_chips[i].id; i++)
		if (ad_chips[i].id == id)
			return &ad_chips[i];
	return 0;
}

static int ad_family(const struct hda_codec *c)
{
	const struct ad_chip *chip = ad_find_chip(c->vendor_id);
	uint16_t dev = (uint16_t)(c->vendor_id & 0xffff);

	if (chip)
		return chip->family;
	/* unknown parts: the closest relative by id */
	if ((dev & 0xff00) == 0x1900 || (dev & 0xff0f) == 0x180a)
		return AD_1884;
	if ((dev & 0xff00) == 0x9800 || dev == 0x198b)
		return AD_1988;
	return AD_1882;
}

/* ---- board models ---------------------------------------------------------- */

#define ADQ_AMP_1981 (1u << 0) /* clamp mixer 0x11 inputs to 0 dB */
#define ADQ_AMP_1884 (1u << 1) /* clamp mixer 0x20 inputs to 0 dB */
#define ADQ_EAPD_MASTER (1u << 2) /* EAPD of eapd_nid follows the master */
#define ADQ_HP_GPIO_LED (1u << 3) /* AD1884 HP: mute LED on GPIO1 */
#define ADQ_STD_EAPD (1u << 4) /* AD1986A wired with normal EAPD */
#define ADQ_INV_JACK (1u << 5) /* presence detect reads inverted */
#define ADQ_THINKPAD (1u << 6) /* firmware beeps via the analog mixer */
#define ADQ_DMIC_COEF (1u << 7) /* digital mic coefficient */
#define ADQ_EAPD_SPEAKER (1u << 8) /* eapd_nid = the parsed speaker pin */
#define ADQ_STEREO_MIX (1u << 9) /* keep the stereo-mix capture source */

struct ad_model {
	uint8_t families;
	uint32_t subsys;
	uint32_t mask;
	const char *name;
	const struct hda_pincfg *pins;
	const struct hda_verb *verbs;
	uint32_t quirks;
	uint8_t eapd_nid;
};

#define SSID(v, d) (((uint32_t)(v) << 16) | (uint32_t)(d))
#define VONLY 0xffff0000u

static const struct hda_pincfg ad1986a_ultra_pins[] = {
	{ 0x1b, 0x90170110 }, /* speaker */
	{ 0x1d, 0x90a7013e }, /* internal microphone */
	{ 0, 0 }
};

static const struct hda_pincfg ad1986a_samsung_pins[] = {
	{ 0x1b, 0x90170110 }, /* speaker */
	{ 0x1d, 0x90a7013e }, /* internal microphone */
	{ 0x20, 0x411111f0 },
	{ 0x24, 0x411111f0 },
	{ 0, 0 }
};

static const struct hda_pincfg ad1986a_3stack_pins[] = {
	{ 0x1a, 0x02214021 }, /* headphone */
	{ 0x1b, 0x01014011 }, /* front */
	{ 0x1c, 0x01813030 }, /* line in */
	{ 0x1d, 0x01a19020 }, /* rear microphone */
	{ 0x1e, 0x411111f0 },
	{ 0x1f, 0x02a190f0 }, /* microphone */
	{ 0x20, 0x411111f0 },
	{ 0, 0 }
};

static const struct hda_pincfg ad1986a_laptop_pins[] = {
	{ 0x1a, 0x02214021 }, /* headphone */
	{ 0x1b, 0x90170110 }, /* speaker */
	{ 0x1c, 0x411111f0 },
	{ 0x1d, 0x411111f0 },
	{ 0x1e, 0x411111f0 },
	{ 0x1f, 0x02a191f0 }, /* microphone */
	{ 0x20, 0x411111f0 },
	{ 0, 0 }
};

/* laptop layout with the internal microphone on port 0x1d */
static const struct hda_pincfg ad1986a_laptop_imic_pins[] = {
	{ 0x1a, 0x02214021 }, /* headphone */
	{ 0x1b, 0x90170110 }, /* speaker */
	{ 0x1c, 0x411111f0 },
	{ 0x1d, 0x90a7013e }, /* internal microphone */
	{ 0x1e, 0x411111f0 },
	{ 0x1f, 0x02a191f0 }, /* microphone */
	{ 0x20, 0x411111f0 },
	{ 0, 0 }
};

static const struct hda_pincfg ad1986a_easynote_pins[] = {
	{ 0x1a, 0x0421402f }, /* headphone */
	{ 0x1b, 0x90170110 }, /* speaker */
	{ 0x1c, 0x411111f0 },
	{ 0x1d, 0x90a70130 }, /* internal microphone */
	{ 0x1e, 0x411111f0 },
	{ 0x1f, 0x04a19040 }, /* microphone */
	{ 0x20, 0x411111f0 },
	{ 0x21, 0x411111f0 },
	{ 0x22, 0x411111f0 },
	{ 0x23, 0x411111f0 },
	{ 0x24, 0x411111f0 },
	{ 0x25, 0x411111f0 },
	{ 0, 0 }
};

/* ThinkPad / TouchSmart digital microphone coefficient */
static const struct hda_verb ad1884_dmic_verbs[] = {
	{ 0x01, AC_VERB_SET_COEF_INDEX, 0x13f7 },
	{ 0x01, AC_VERB_SET_PROC_COEF, 0x08 },
	{ 0, 0, 0 }
};

#define A1986 ADF(AD_1986A)
#define A1981 ADF(AD_1981)
#define A1884 ADF(AD_1884)

static const struct ad_model ad_models[] = {
	/* -- AD1986A -- */
	{ A1986, SSID(0x103c, 0x30af), 0, "HP B2800", ad1986a_laptop_imic_pins,
	  0, 0, 0 },
	{ A1986, SSID(0x1043, 0x1153), 0, "ASUS M9V", ad1986a_laptop_imic_pins,
	  0, 0, 0 },
	{ A1986, SSID(0x1043, 0x1443), 0, "ASUS Z99He", 0, 0,
	  ADQ_STD_EAPD | ADQ_EAPD_MASTER, 0x1b },
	{ A1986, SSID(0x1043, 0x1447), 0, "ASUS A8JN", 0, 0,
	  ADQ_STD_EAPD | ADQ_EAPD_MASTER, 0x1b },
	{ A1986, SSID(0x1043, 0x8100), 0xffffff00u, "ASUS P5",
	  ad1986a_3stack_pins, 0, 0, 0 },
	{ A1986, SSID(0x1043, 0x8200), 0xffffff00u, "ASUS M2",
	  ad1986a_3stack_pins, 0, 0, 0 },
	{ A1986, SSID(0x10de, 0xcb84), 0, "ASUS A8N-VM", ad1986a_3stack_pins, 0,
	  0, 0 },
	{ A1986, SSID(0x1179, 0xff40), 0, "Toshiba Satellite L40", 0, 0,
	  ADQ_STD_EAPD | ADQ_EAPD_MASTER, 0x1b },
	{ A1986, SSID(0x144d, 0xc01e), 0, "FSC V2060", ad1986a_laptop_pins, 0,
	  0, 0 },
	{ A1986, SSID(0x144d, 0xc027), 0, "Samsung Q1", ad1986a_ultra_pins, 0, 0,
	  0 },
	{ A1986, SSID(0x144d, 0xc000), 0xffffff00u, "Samsung",
	  ad1986a_samsung_pins, 0, 0, 0 },
	{ A1986, SSID(0x1631, 0xc022), 0, "PackardBell EasyNote MX65",
	  ad1986a_easynote_pins, 0,
	  ADQ_STD_EAPD | ADQ_EAPD_MASTER | ADQ_STEREO_MIX, 0x1b },
	{ A1986, SSID(0x17aa, 0x2066), 0, "Lenovo N100", 0, 0,
	  ADQ_INV_JACK | ADQ_EAPD_MASTER, 0x1b },
	{ A1986, SSID(0x17aa, 0x1011), 0, "Lenovo M55", ad1986a_3stack_pins, 0,
	  0, 0 },
	{ A1986, SSID(0x17aa, 0x1017), 0, "Lenovo A60", ad1986a_3stack_pins, 0,
	  0, 0 },

	/* -- AD1981HD -- */
	/* HP nx6320 reports its subsystem id byte-swapped */
	{ A1981, SSID(0x30b0, 0x103c), 0, "HP nx6320", 0, 0,
	  ADQ_EAPD_MASTER | ADQ_AMP_1981, 0x05 },
	{ A1981, SSID(0x1014, 0), VONLY, "Lenovo", 0, 0, ADQ_AMP_1981, 0 },
	{ A1981, SSID(0x103c, 0), VONLY, "HP", 0, 0,
	  ADQ_EAPD_MASTER | ADQ_AMP_1981, 0x05 },
	{ A1981, SSID(0x17aa, 0), VONLY, "Lenovo", 0, 0, ADQ_AMP_1981, 0 },

	/* -- AD1884 / AD1984 family -- */
	{ A1884, SSID(0x103c, 0x2a82), 0, "HP TouchSmart", 0,
	  ad1884_dmic_verbs,
	  ADQ_EAPD_MASTER | ADQ_EAPD_SPEAKER | ADQ_HP_GPIO_LED | ADQ_AMP_1884,
	  0 },
	{ A1884, SSID(0x103c, 0), VONLY, "HP", 0, 0,
	  ADQ_EAPD_MASTER | ADQ_EAPD_SPEAKER | ADQ_HP_GPIO_LED | ADQ_AMP_1884,
	  0 },
	{ A1884, SSID(0x17aa, 0), VONLY, "Lenovo ThinkPad", 0,
	  ad1884_dmic_verbs, ADQ_EAPD_MASTER | ADQ_THINKPAD, 0x12 },

	{ 0, 0, 0, 0, 0, 0, 0, 0 }
};

static uint32_t ad_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	return ((v & 0xffffu) << 16) | (v >> 16);
}

static const struct ad_model *ad_lookup_ssid(int fam, uint32_t ssid)
{
	int i;

	if (!ssid)
		return 0;
	for (i = 0; ad_models[i].name; i++) {
		const struct ad_model *m = &ad_models[i];
		uint32_t mask = m->mask ? m->mask : 0xffffffffu;

		if (m->families && !(m->families & ADF(fam)))
			continue;
		if ((ssid & mask) == (m->subsys & mask))
			return m;
	}
	return 0;
}

static const struct ad_model *ad_lookup(const struct hda_codec *c)
{
	int fam = ad_family(c);
	uint32_t pci = ad_pci_ssid(c);
	const struct ad_model *m = ad_lookup_ssid(fam, pci);

	if (!m && c->subsystem_id != pci)
		m = ad_lookup_ssid(fam, c->subsystem_id);
	return m;
}

/* ---- helpers --------------------------------------------------------------------- */

static void ad_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static void ad_override_amp_caps(struct hda_codec *c, uint8_t nid, int dir,
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

#define AD_AMP_0DB_CAPS                                                  \
	((0x17u << AC_AMPCAP_OFFSET_SHIFT) |                             \
	 (0x17u << AC_AMPCAP_NUM_STEPS_SHIFT) |                          \
	 (0x05u << AC_AMPCAP_STEP_SIZE_SHIFT) | AC_AMPCAP_MUTE)

static int ad_pin_has_eapd(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);

	return w && w->type == AC_WID_PIN && (w->pincap & AC_PINCAP_EAPD);
}

/* Make the generic code leave the EAPD of a pin alone. */
static void ad_take_eapd(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	int i;

	if (!w)
		return;
	w->eapd = 0;
	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].pin == nid)
			c->spec.out[i].eapd = 0;
}

/* Amplifier on/off on a pin, honouring the inverted AD1986A wiring. */
static void ad_amp_power(struct hda_codec *c, uint8_t nid, int on,
			 int inverted)
{
	if (!nid || !ad_pin_has_eapd(c, nid))
		return;
	if (inverted)
		on = !on;
	hda_codec_write(c, nid, AC_VERB_SET_EAPD_BTLENABLE,
			on ? AC_EAPDBTL_EAPD : 0);
}

static int ad_inverted(const struct hda_codec *c, const struct ad_model *m)
{
	if (!ad_families[ad_family(c)].inv_eapd)
		return 0;
	return !(m && (m->quirks & ADQ_STD_EAPD));
}

/* The pin whose EAPD follows the master: fixed per model, or the parsed
 * speaker. */
static uint8_t ad_master_eapd_pin(struct hda_codec *c, const struct ad_model *m)
{
	int i;

	if (!m || !(m->quirks & ADQ_EAPD_MASTER))
		return 0;
	if (!(m->quirks & ADQ_EAPD_SPEAKER))
		return m->eapd_nid;
	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].type == AUDIO_OUT_SPEAKER)
			return c->spec.out[i].pin;
	/* no speaker: the first line out */
	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++)
		if (c->spec.out[i].type == AUDIO_OUT_LINE)
			return c->spec.out[i].pin;
	return 0;
}

static void ad_update_master(struct hda_codec *c, const struct ad_model *m)
{
	int unmuted = c->spec.master_mute == 0;
	uint8_t pin = ad_master_eapd_pin(c, m);

	if (pin)
		ad_amp_power(c, pin, unmuted, ad_inverted(c, m));
	if (m && (m->quirks & ADQ_HP_GPIO_LED)) {
		/* GPIO1 high lights the mute LED */
		uint32_t mask = (c->gpio_mask | 0x02) & 0xff;
		uint32_t dir = (c->gpio_dir | 0x02) & 0xff;
		uint32_t data = (c->gpio_data & ~0x02u) | (unmuted ? 0 : 0x02);

		hda_gpio_set(c, mask, dir, data & 0xff);
		c->gpio_mask = mask;
		c->gpio_dir = dir;
		c->gpio_data = data & 0xff;
	}
}

/* AD1986A: both amplifier pins on (in their own polarity), except the
 * one that follows the master. */
static void ad1986a_amps_on(struct hda_codec *c, const struct ad_model *m)
{
	const struct ad_family_info *fi = &ad_families[AD_1986A];
	uint8_t master = ad_master_eapd_pin(c, m);
	int inv = ad_inverted(c, m);

	if (fi->eapd_front != master)
		ad_amp_power(c, fi->eapd_front, 1, inv);
	if (fi->eapd_hp != master)
		ad_amp_power(c, fi->eapd_hp, 1, inv);
}

/* AD1983: keep only the direct ADC sources on the two capture selectors. */
static void ad1983_trim_connections(struct hda_codec *c)
{
	static const uint8_t sel[2] = { 0x0c, 0x0d };
	static const uint8_t src[2] = { 0x08, 0x09 };
	int i, j;

	for (i = 0; i < 2; i++) {
		struct hda_widget *w = hda_widget(c, sel[i]);

		if (!w || w->nconn <= 1)
			continue;
		for (j = 0; j < w->nconn && j < HDA_MAX_CONNS; j++) {
			if (w->conn[j] == src[i]) {
				/* the selector index stays the hardware
				 * one, so only shrink when it is the first */
				if (j == 0)
					w->nconn = 1;
				break;
			}
		}
	}
}

static void ad_disable_jack_detect(struct hda_codec *c)
{
	unsigned int i;

	if (!c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		w->pincap &= ~(AC_PINCAP_PRES_DETECT | AC_PINCAP_TRIG_REQ);
		w->caps &= ~AC_WCAP_UNSOL_CAP;
	}
}

/* ---- driver callbacks ---------------------------------------------------------- */

static int ad_match(struct hda_codec *c)
{
	return (c->vendor_id >> 16) == 0x11d4 && c->afg != 0;
}

static int ad_pre_parse(struct hda_codec *c)
{
	const struct ad_chip *chip = ad_find_chip(c->vendor_id);
	const struct ad_model *m = ad_lookup(c);
	int fam = ad_family(c);
	const struct ad_family_info *fi = &ad_families[fam];

	ad_copy_name(c->vendor_name, sizeof(c->vendor_name), "Analog Devices");
	if (chip)
		ad_copy_name(c->chip_name, sizeof(c->chip_name), chip->name);
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "AD%04x",
			  (unsigned int)(c->vendor_id & 0xffff));

	if (fam == AD_1983)
		ad1983_trim_connections(c);

	if (m) {
		if (m->pins)
			hda_apply_pincfgs(c, m->pins);
		if (m->verbs)
			hda_apply_verbs(c, m->verbs);
		if (m->quirks & ADQ_AMP_1981)
			ad_override_amp_caps(c, 0x11, HDA_INPUT,
					     AD_AMP_0DB_CAPS);
		if (m->quirks & ADQ_AMP_1884)
			ad_override_amp_caps(c, 0x20, HDA_INPUT,
					     AD_AMP_0DB_CAPS);
		if (m->quirks & ADQ_INV_JACK)
			/* the jack sense of this board reads backwards;
			 * without it both outputs simply play */
			ad_disable_jack_detect(c);
		if (m->quirks & ADQ_HP_GPIO_LED) {
			uint32_t mask = (c->gpio_mask | 0x02) & 0xff;
			uint32_t dir = (c->gpio_dir | 0x02) & 0xff;
			uint32_t data = (c->gpio_data | 0x02) & 0xff;

			hda_gpio_set(c, mask, dir, data);
			c->gpio_mask = mask;
			c->gpio_dir = dir;
			c->gpio_data = data;
		}
		kprintf("[snd] hda: codec %d: %s %s, board fixup \"%s\"\n",
			c->addr, c->vendor_name, c->chip_name, m->name);
	} else {
		kprintf("[snd] hda: codec %d: %s %s, BIOS pin configuration\n",
			c->addr, c->vendor_name, c->chip_name);
	}

	if (!c->spec.beep_nid && fi->beep_nid && !(m && (m->quirks &
							  ADQ_THINKPAD))) {
		struct hda_widget *w = hda_widget(c, fi->beep_nid);

		if (w && w->type == AC_WID_BEEP)
			c->spec.beep_nid = w->nid;
	}
	return 0;
}

static int ad_post_parse(struct hda_codec *c)
{
	const struct ad_model *m = ad_lookup(c);
	int fam = ad_family(c);
	uint8_t master = ad_master_eapd_pin(c, m);

	if (fam == AD_1986A) {
		/* inverted EAPD: the generic code would switch the
		 * amplifiers off exactly when it means on */
		ad_take_eapd(c, ad_families[fam].eapd_front);
		ad_take_eapd(c, ad_families[fam].eapd_hp);
		ad1986a_amps_on(c, m);
	}
	if (master)
		ad_take_eapd(c, master);
	if (m && (m->quirks & ADQ_THINKPAD)) {
		/* firmware beeps (battery, lid) reach the speaker through the
		 * beep input of the analog mixer 0x20 (index 3), not through
		 * a digital beep widget; that input is left as the firmware
		 * set it, so the machine beeps exactly as loud as it did
		 * before the OS took over */
		c->spec.beep_nid = 0;
	}
	ad_update_master(c, m);
	return 0;
}

static int ad_init(struct hda_codec *c)
{
	const struct ad_model *m = ad_lookup(c);

	if (m && m->pins)
		hda_apply_pincfgs(c, m->pins);
	if (m && m->verbs)
		hda_apply_verbs(c, m->verbs);
	if (ad_family(c) == AD_1986A)
		ad1986a_amps_on(c, m);
	ad_update_master(c, m);
	return 0;
}

static void ad_automute(struct hda_codec *c, int hp_present)
{
	const struct ad_model *m = ad_lookup(c);

	(void)hp_present;
	/* the AD1986A cannot switch its pins smoothly: its amplifiers stay
	 * on and the generic code mutes through the amplifier path; the
	 * master-following EAPD and the LED track the master mute */
	if (ad_family(c) == AD_1986A)
		ad1986a_amps_on(c, m);
	ad_update_master(c, m);
}

static void ad_unsol(struct hda_codec *c, uint32_t res)
{
	(void)res;
	hda_codec_update_routing(c);
}

static void ad_playback_hook(struct hda_codec *c, int start)
{
	if (start)
		ad_update_master(c, ad_lookup(c));
}

const struct hda_vendor_driver hda_analog_driver = {
	.name = "analog",
	.match = ad_match,
	.pre_parse = ad_pre_parse,
	.post_parse = ad_post_parse,
	.init = ad_init,
	.automute = ad_automute,
	.unsol = ad_unsol,
	.playback_hook = ad_playback_hook,
	.probe = 0,
	.remove = 0,
};
