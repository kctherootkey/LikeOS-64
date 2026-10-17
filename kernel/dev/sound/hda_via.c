// LikeOS HD Audio - VIA codecs.
//
// VT1708, VT1709 (6/10 channel), VT1708B (4/8 channel), VT1708BCE,
// VT1708S, VT1705, VT1702, VT1718S, VT2020, VT1828S, VT1716S, VT2002P,
// VT1812, VT1818S, VT1802, VT1705CF, VT1708SCE and VT1808 (the VT3476
// design).  These are parsed generically; the codec-specific parts are:
//
//  * vendor "back door" verbs on the function group: mic boost access,
//    "do not bypass the mixer", class-D speaker amplifier setup on the
//    VT2002P, the AOW0-to-MW9 path, digital microphone rates,
//  * the analog low-current mode, which must be left (1x bias current)
//    or the outputs are weak and distorted,
//  * a DAC-to-loopback-mixer connection the newer parts have but do not
//    list; without it the front DAC cannot reach the mixer-fed pins,
//  * microphone boost amplifiers the pins have without advertising them,
//  * VT1708: no unsolicited responses at all (the jack detect function is
//    polled), and 24-bit playback is noisy on the right channel,
//  * VT1802: two pins list a connection to a widget that does not exist,
//  * EAPD is kept on permanently (the amplifiers pop when switched),
//  * a few ASUS board fixups.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>
#include <kernel/hal/lapic.h>

enum via_type {
	VIA_UNKNOWN = -1,
	VT1708 = 0,
	VT1709_10CH,
	VT1709_6CH,
	VT1708B_8CH,
	VT1708B_4CH,
	VT1708S, /* also VT1708BCE, VT1705, VT1818S */
	VT1702,
	VT1718S, /* also VT2020, VT1828S */
	VT1716S,
	VT2002P,
	VT1812,
	VT1802,
	VT1705CF, /* VT3476 design: VT1705CF, VT1708SCE, VT1808 */
	VIA_TYPES,
};

struct via_chip {
	uint32_t id;
	const char *name;
};

static const struct via_chip via_chips[] = {
	{ 0x11061708, "VT1708" }, { 0x11061709, "VT1708" },
	{ 0x1106170a, "VT1708" }, { 0x1106170b, "VT1708" },
	{ 0x1106e710, "VT1709 10-Ch" }, { 0x1106e711, "VT1709 10-Ch" },
	{ 0x1106e712, "VT1709 10-Ch" }, { 0x1106e713, "VT1709 10-Ch" },
	{ 0x1106e714, "VT1709 6-Ch" }, { 0x1106e715, "VT1709 6-Ch" },
	{ 0x1106e716, "VT1709 6-Ch" }, { 0x1106e717, "VT1709 6-Ch" },
	{ 0x1106e720, "VT1708B 8-Ch" }, { 0x1106e721, "VT1708B 8-Ch" },
	{ 0x1106e722, "VT1708B 8-Ch" }, { 0x1106e723, "VT1708B 8-Ch" },
	{ 0x1106e724, "VT1708B 4-Ch" }, { 0x1106e725, "VT1708B 4-Ch" },
	{ 0x1106e726, "VT1708B 4-Ch" }, { 0x1106e727, "VT1708B 4-Ch" },
	{ 0x11060397, "VT1708S" }, { 0x11061397, "VT1708S" },
	{ 0x11062397, "VT1708S" }, { 0x11063397, "VT1708S" },
	{ 0x11064397, "VT1705" }, { 0x11065397, "VT1708S" },
	{ 0x11066397, "VT1708S" }, { 0x11067397, "VT1708S" },
	{ 0x11060398, "VT1702" }, { 0x11061398, "VT1702" },
	{ 0x11062398, "VT1702" }, { 0x11063398, "VT1702" },
	{ 0x11064398, "VT1702" }, { 0x11065398, "VT1702" },
	{ 0x11066398, "VT1702" }, { 0x11067398, "VT1702" },
	{ 0x11060428, "VT1718S" }, { 0x11064428, "VT1718S" },
	{ 0x11060441, "VT2020" }, { 0x11064441, "VT1828S" },
	{ 0x11060433, "VT1716S" }, { 0x1106a721, "VT1716S" },
	{ 0x11060438, "VT2002P" }, { 0x11064438, "VT2002P" },
	{ 0x11060448, "VT1812" }, { 0x11060440, "VT1818S" },
	{ 0x11060446, "VT1802" }, { 0x11068446, "VT1802" },
	{ 0x11064760, "VT1705CF" }, { 0x11064761, "VT1708SCE" },
	{ 0x11064762, "VT1808" },
	{ 0, 0 }
};

static const char *via_chip_name(uint32_t id)
{
	int i;

	for (i = 0; via_chips[i].id; i++)
		if (via_chips[i].id == id)
			return via_chips[i].name;
	return 0;
}

/* The VT1708BCE identifies as a VT1708B with a 7-entry list on 0x16. */
static int via_is_1708bce(struct hda_codec *c)
{
	struct hda_widget *w = hda_widget(c, 0x16);
	uint32_t len;

	if (w && w->nconn)
		return w->nconn == 7;
	len = hda_param(c, 0x16, AC_PAR_CONNLIST_LEN);
	return len != 0xffffffffu && (len & AC_CLIST_LENGTH) == 7;
}

static int via_type(struct hda_codec *c)
{
	uint16_t ven = (uint16_t)(c->vendor_id >> 16);
	uint16_t dev = (uint16_t)(c->vendor_id & 0xffff);

	if (ven != 0x1106)
		return VIA_UNKNOWN;
	if (dev >= 0x1708 && dev <= 0x170b)
		return VT1708;
	if (dev >= 0xe710 && dev <= 0xe713)
		return VT1709_10CH;
	if (dev >= 0xe714 && dev <= 0xe717)
		return VT1709_6CH;
	if (dev >= 0xe720 && dev <= 0xe723)
		return via_is_1708bce(c) ? VT1708S : VT1708B_8CH;
	if (dev >= 0xe724 && dev <= 0xe727)
		return VT1708B_4CH;
	if ((dev & 0xfff) == 0x397 && (dev >> 12) < 8)
		return VT1708S;
	if ((dev & 0xfff) == 0x398 && (dev >> 12) < 8)
		return VT1702;
	if ((dev & 0xfff) == 0x428 && (dev >> 12) < 8)
		return VT1718S;
	if (dev == 0x0433 || dev == 0xa721)
		return VT1716S;
	if (dev == 0x0441 || dev == 0x4441)
		return VT1718S;
	if (dev == 0x0438 || dev == 0x4438)
		return VT2002P;
	if (dev == 0x0448)
		return VT1812;
	if (dev == 0x0440)
		return VT1708S;
	if ((dev & 0xfff) == 0x446)
		return VT1802;
	if (dev == 0x4760 || dev == 0x4761 || dev == 0x4762)
		return VT1705CF;
	return VIA_UNKNOWN;
}

/* ---- per-type data --------------------------------------------------------------- */

static const struct hda_verb via_1708_verbs[] = {
	/* jack detect function on (it is polled; there are no events) */
	{ 0x01, 0xf81, 0x00 },
	{ 0, 0, 0 }
};
static const struct hda_verb via_1708s_verbs[] = {
	{ 0x01, 0xf98, 0x01 }, /* mic boost back door */
	{ 0x01, 0xf88, 0xc0 }, /* do not bypass the mixer */
	{ 0, 0, 0 }
};
static const struct hda_verb via_1702_verbs[] = {
	{ 0x01, 0xf88, 0x03 }, /* mixer enable */
	{ 0x01, 0xf82, 0x3f }, /* GPIO 0-2 */
	{ 0, 0, 0 }
};
static const struct hda_verb via_1718s_verbs[] = {
	{ 0x01, 0xfb2, 0x10 }, /* MW0 gain adjust 5 */
	{ 0x01, 0xf88, 0x08 }, /* boost back door */
	{ 0, 0, 0 }
};
static const struct hda_verb via_1716s_verbs[] = {
	{ 0x01, 0xf8a, 0x80 }, /* boost back door */
	{ 0x01, 0xf88, 0xc0 }, /* do not bypass the mixer */
	{ 0x01, 0xf90, 0x08 }, /* mono output on */
	{ 0, 0, 0 }
};
static const struct hda_verb via_2002p_verbs[] = {
	/* class-D speaker amplifier */
	{ 0x01, 0xfe0, 0x04 },
	{ 0x01, 0xfe9, 0x80 },
	{ 0x01, 0xfe2, 0x22 },
	{ 0x01, 0xfb9, 0x24 }, /* boost back door */
	{ 0x01, 0xfb8, 0x88 }, /* AOW0 to MW9 */
	{ 0, 0, 0 }
};
static const struct hda_verb via_1802_verbs[] = {
	{ 0x01, 0xfb9, 0x24 },
	{ 0x01, 0xfb8, 0x88 },
	{ 0, 0, 0 }
};
static const struct hda_verb via_1812_verbs[] = {
	{ 0x01, 0xfb9, 0x24 },
	{ 0x01, 0xfb8, 0xa8 },
	{ 0, 0, 0 }
};
static const struct hda_verb via_3476_verbs[] = {
	{ 0x01, 0xf7b, 0x30 }, /* digital mic 8/16/32 kHz */
	{ 0x01, 0xfb9, 0x20 }, /* boost back door */
	{ 0x01, 0xfb8, 0x10 }, /* AOW to MW9 */
	{ 0, 0, 0 }
};

struct via_type_info {
	const char *family;
	uint8_t mixer_nid; /* analog loopback mixer */
	const struct hda_verb *verbs;
	uint16_t lc_verb; /* analog low-current mode verb, 0 = none */
	uint8_t lc_normal; /* its "1x current" (normal) setting */
	uint8_t boost[2]; /* pins with hidden mic boost */
	uint8_t secret_dac; /* the mixer hides its DAC input */
};

static const struct via_type_info via_types[VIA_TYPES] = {
	[VT1708] = { "VT1708", 0x17, via_1708_verbs, 0, 0, { 0, 0 }, 0 },
	[VT1709_10CH] = { "VT1709", 0x18, 0, 0, 0, { 0, 0 }, 0 },
	[VT1709_6CH] = { "VT1709", 0x18, 0, 0, 0, { 0, 0 }, 0 },
	[VT1708B_8CH] = { "VT1708B", 0x16, 0, 0xf70, 0x00, { 0, 0 }, 0 },
	[VT1708B_4CH] = { "VT1708B", 0x16, 0, 0xf70, 0x00, { 0, 0 }, 0 },
	[VT1708S] = { "VT1708S", 0x16, via_1708s_verbs, 0xf73, 0xe1,
		      { 0x1a, 0x1e }, 0 },
	[VT1702] = { "VT1702", 0x1a, via_1702_verbs, 0xf73, 0x1d, { 0, 0 }, 0 },
	[VT1718S] = { "VT1718S", 0x21, via_1718s_verbs, 0xf73, 0xe1,
		      { 0x2b, 0x29 }, 1 },
	[VT1716S] = { "VT1716S", 0x16, via_1716s_verbs, 0xf73, 0xe1,
		      { 0x1a, 0x1e }, 0 },
	[VT2002P] = { "VT2002P", 0x21, via_2002p_verbs, 0xf93, 0xe0,
		      { 0x2b, 0x29 }, 1 },
	[VT1812] = { "VT1812", 0x21, via_1812_verbs, 0xf93, 0xe0,
		     { 0x2b, 0x29 }, 1 },
	[VT1802] = { "VT1802", 0x21, via_1802_verbs, 0xf93, 0xe0,
		     { 0x2b, 0x29 }, 1 },
	[VT1705CF] = { "VT3476", 0x3f, via_3476_verbs, 0xf82, 0xe0, { 0, 0 },
		       1 },
};

/* ---- board fixups ------------------------------------------------------------------ */

#define VIAQ_INTMIC_BOOST (1u << 0) /* internal mic 0x30 has a boost */
#define VIAQ_NO_POWER_SAVE (1u << 1) /* never power widgets down */

struct via_model {
	uint32_t subsys;
	uint32_t mask;
	const char *name;
	const struct hda_pincfg *pins;
	uint32_t quirks;
};

static const struct hda_pincfg via_asus_g75_pins[] = {
	{ 0x24, 0x991301f0 }, /* speaker */
	{ 0x33, 0x991301f1 }, /* subwoofer */
	{ 0, 0 }
};

static const struct via_model via_models[] = {
	{ 0x104313f7, 0, "ASUS B23E", 0, VIAQ_NO_POWER_SAVE },
	{ 0x10431487, 0, "ASUS G75", via_asus_g75_pins, 0 },
	{ 0x10438532, 0, "ASUS X202E", 0, VIAQ_INTMIC_BOOST },
	{ 0x15580000, 0xffff0000u, "Clevo", 0, VIAQ_NO_POWER_SAVE },
	{ 0, 0, 0, 0, 0 }
};

static uint32_t via_pci_ssid(const struct hda_codec *c)
{
	uint32_t v;

	if (!c->ctrl || !c->ctrl->pci)
		return 0;
	v = pci_cfg_read32_dev(c->ctrl->pci, 0x2c);
	if (v == 0 || v == 0xffffffffu)
		return 0;
	return ((v & 0xffffu) << 16) | (v >> 16);
}

static const struct via_model *via_lookup(struct hda_codec *c)
{
	uint32_t ids[2];
	int t, i;

	/* the fixups exist for the VT2002P/VT1802 family only */
	t = via_type(c);
	if (t != VT2002P && t != VT1802)
		return 0;
	ids[0] = via_pci_ssid(c);
	ids[1] = c->subsystem_id;
	for (t = 0; t < 2; t++) {
		if (!ids[t])
			continue;
		for (i = 0; via_models[i].name; i++) {
			uint32_t mask = via_models[i].mask ? via_models[i].mask :
							     0xffffffffu;

			if ((ids[t] & mask) == (via_models[i].subsys & mask))
				return &via_models[i];
		}
	}
	return 0;
}

/* ---- helpers --------------------------------------------------------------------- */

static void via_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* A pin with a mic boost its caps do not show: add an input amplifier
 * with `steps' steps of `size' (0.25 dB units) starting at 0 dB. */
static void via_override_mic_boost(struct hda_codec *c, uint8_t nid,
				   unsigned int steps, unsigned int size)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w || w->type != AC_WID_PIN)
		return;
	w->caps |= AC_WCAP_IN_AMP | AC_WCAP_AMP_OVRD;
	w->amp_in_caps = (0u << AC_AMPCAP_OFFSET_SHIFT) |
			 (steps << AC_AMPCAP_NUM_STEPS_SHIFT) |
			 (size << AC_AMPCAP_STEP_SIZE_SHIFT);
}

/* The loopback mixer of the newer parts takes the first analog DAC
 * although its connection list does not say so; the hidden input sits
 * right after the listed ones. */
static void via_add_secret_dac(struct hda_codec *c, uint8_t mixer)
{
	struct hda_widget *mw = hda_widget(c, mixer);
	unsigned int i;
	int j;

	if (!mw || !c->w)
		return;
	for (j = 0; j < mw->nconn && j < HDA_MAX_CONNS; j++) {
		struct hda_widget *src = hda_widget(c, mw->conn[j]);

		if (src && src->type == AC_WID_AUD_OUT)
			return; /* already listed */
	}
	if (mw->nconn >= HDA_MAX_CONNS)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type == AC_WID_AUD_OUT &&
		    !(w->caps & AC_WCAP_DIGITAL)) {
			mw->conn[mw->nconn++] = w->nid;
			mw->caps |= AC_WCAP_CONN_LIST;
			return;
		}
	}
}

/* VT1802: 0x24 and 0x33 list a widget 0x3e that does not exist; the
 * mixer 0x1c is what is actually wired. */
static void via_fix_1802_connections(struct hda_codec *c)
{
	struct hda_widget *w24 = hda_widget(c, 0x24);
	struct hda_widget *w33 = hda_widget(c, 0x33);

	if (w24 && w24->type == AC_WID_PIN) {
		w24->conn[0] = 0x14;
		w24->conn[1] = 0x1c;
		w24->nconn = 2;
		if (w24->cur_conn >= w24->nconn)
			w24->cur_conn = 0;
	}
	if (w33 && w33->type == AC_WID_PIN) {
		w33->conn[0] = 0x1c;
		w33->nconn = 1;
		w33->cur_conn = 0;
	}
}

/* VT1708: HP and CD pins that the BIOS left "not connected" with an
 * all-ones sequence are really jacks. */
static void via_1708_pin_connect(struct hda_codec *c, uint8_t nid)
{
	struct hda_widget *w = hda_widget(c, nid);
	uint32_t cfg, seqassoc;

	if (!w || w->type != AC_WID_PIN)
		return;
	cfg = w->pin_cfg;
	seqassoc = cfg & (AC_DEFCFG_DEF_ASSOC | AC_DEFCFG_SEQUENCE);
	if (((cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT) ==
		    AC_JACK_PORT_NONE &&
	    (seqassoc == 0xf0 || seqassoc == 0xff)) {
		cfg &= ~AC_DEFCFG_PORT_CONN;
		hda_pin_override_cfg(c, nid, cfg);
		w->pin_cfg = cfg;
	}
}

/* VT1708: 24-bit playback is noisy on the right channel; 16 bit only. */
static void via_1708_16bit_only(struct hda_codec *c)
{
	unsigned int i;

	if (c->vendor_id != 0x11061708 || !c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];
		uint32_t pcm;

		if (w->type != AC_WID_AUD_OUT || (w->caps & AC_WCAP_DIGITAL))
			continue;
		pcm = w->pcm ? w->pcm : c->afg_pcm;
		if (!pcm || pcm == 0xffffffffu)
			continue;
		pcm &= ~(AC_SUPPCM_BITS_8 | AC_SUPPCM_BITS_20 |
			 AC_SUPPCM_BITS_24 | AC_SUPPCM_BITS_32);
		pcm |= AC_SUPPCM_BITS_16;
		w->pcm = pcm;
		w->caps |= AC_WCAP_FORMAT_OVRD;
	}
}

/* EAPD stays on: the amplifiers pop when switched with the outputs. */
static void via_eapd_on(struct hda_codec *c)
{
	unsigned int i;
	int j;

	if (!c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN || !(w->pincap & AC_PINCAP_EAPD))
			continue;
		w->eapd = 0;
		for (j = 0; j < c->spec.nout && j < HDA_MAX_OUTPUTS; j++)
			if (c->spec.out[j].pin == w->nid)
				c->spec.out[j].eapd = 0;
		hda_set_eapd(c, w->nid, 1);
	}
}

/* Normal analog bias: low-current mode makes the outputs weak. */
static void via_normal_current(struct hda_codec *c)
{
	int t = via_type(c);

	if (t < 0 || t >= VIA_TYPES || !via_types[t].lc_verb || !c->afg)
		return;
	hda_codec_write(c, c->afg, via_types[t].lc_verb,
			via_types[t].lc_normal);
}

static void via_apply_verbs(struct hda_codec *c)
{
	int t = via_type(c);

	if (t < 0 || t >= VIA_TYPES)
		return;
	if (via_types[t].verbs)
		hda_apply_verbs(c, via_types[t].verbs);
}

/* ---- driver callbacks ----------------------------------------------------------- */

static int via_match(struct hda_codec *c)
{
	return (c->vendor_id >> 16) == 0x1106 && c->afg != 0;
}

static int via_pre_parse(struct hda_codec *c)
{
	const char *name = via_chip_name(c->vendor_id);
	const struct via_model *m = via_lookup(c);
	int t = via_type(c);

	via_copy_name(c->vendor_name, sizeof(c->vendor_name), "VIA");
	if (t == VT1708S && (c->vendor_id & 0xfff0) == 0xe720)
		name = "VT1708BCE";
	if (name)
		via_copy_name(c->chip_name, sizeof(c->chip_name), name);
	else
		ksnprintf(c->chip_name, sizeof(c->chip_name), "VT%04x",
			  (unsigned int)(c->vendor_id & 0xffff));

	if (t >= 0 && t < VIA_TYPES) {
		const struct via_type_info *ti = &via_types[t];

		if (ti->boost[0])
			via_override_mic_boost(c, ti->boost[0], 3, 40);
		if (ti->boost[1])
			via_override_mic_boost(c, ti->boost[1], 3, 40);
		if (t == VT1802)
			via_fix_1802_connections(c);
		if (ti->secret_dac)
			via_add_secret_dac(c, ti->mixer_nid);
		if (t == VT1702) {
			/* loopback mixer inputs at most 0 dB */
			struct hda_widget *w = hda_widget(c, 0x1a);

			if (w) {
				w->amp_in_caps =
					(0x17u << AC_AMPCAP_OFFSET_SHIFT) |
					(0x17u << AC_AMPCAP_NUM_STEPS_SHIFT) |
					(0x05u << AC_AMPCAP_STEP_SIZE_SHIFT) |
					AC_AMPCAP_MUTE;
				w->caps |= AC_WCAP_AMP_OVRD;
			}
		}
		if (t == VT1708) {
			via_1708_pin_connect(c, 0x20); /* headphone */
			via_1708_pin_connect(c, 0x24); /* CD */
			via_1708_16bit_only(c);
		}
	}
	if (m) {
		if (m->pins)
			hda_apply_pincfgs(c, m->pins);
		if (m->quirks & VIAQ_INTMIC_BOOST)
			via_override_mic_boost(c, 0x30, 2, 40);
		kprintf("[snd] hda: codec %d: VIA %s, board fixup \"%s\"\n",
			c->addr, c->chip_name, m->name);
	} else {
		kprintf("[snd] hda: codec %d: VIA %s, BIOS pin configuration\n",
			c->addr, c->chip_name);
	}

	via_apply_verbs(c);
	via_normal_current(c);

	/* the digital beep generator */
	if (!c->spec.beep_nid && c->w) {
		unsigned int i;

		for (i = 0; i < c->num_nodes; i++) {
			if (c->w[i].type == AC_WID_BEEP) {
				c->spec.beep_nid = c->w[i].nid;
				break;
			}
		}
	}
	return 0;
}

static int via_post_parse(struct hda_codec *c)
{
	if (via_type(c) == VT1708) {
		/* the VT1708 never sends unsolicited responses */
		c->spec.jack_poll = 1;
		c->unsol_capable = 0;
	}
	via_eapd_on(c);
	return 0;
}

static int via_init(struct hda_codec *c)
{
	const struct via_model *m = via_lookup(c);

	/* jack detection needs a moment after power-up */
	lapic_delay_ms(10);
	if (m && m->pins)
		hda_apply_pincfgs(c, m->pins);
	via_apply_verbs(c);
	via_normal_current(c);
	via_eapd_on(c);
	return 0;
}

static void via_automute(struct hda_codec *c, int hp_present)
{
	(void)hp_present;
	/* keep the amplifiers powered whatever the routing */
	via_eapd_on(c);
}

static void via_unsol(struct hda_codec *c, uint32_t res)
{
	(void)res;
	hda_codec_update_routing(c);
}

static void via_playback_hook(struct hda_codec *c, int start)
{
	/* leave the low-current mode before audio flows */
	if (start)
		via_normal_current(c);
}

const struct hda_vendor_driver hda_via_driver = {
	.name = "via",
	.match = via_match,
	.pre_parse = via_pre_parse,
	.post_parse = via_post_parse,
	.init = via_init,
	.automute = via_automute,
	.unsol = via_unsol,
	.playback_hook = via_playback_hook,
	.probe = 0,
	.remove = 0,
};
