// LikeOS HD Audio - emulated codecs of virtual machines.
//
// Hypervisors put a small software codec behind their emulated HDA
// controller.  The ones handled here identify themselves with an ID no
// physical codec uses:
//
//   QEMU   hda-output / hda-duplex / hda-micro   vendor 0x1af4, device
//          0x0010-0x003f (low nibble 1 = no mixer, 2 = with amplifiers)
//   bhyve  hda codec                              vendor id 0x00008086
//   VirtualBox  STAC9221 A1 model                 0x83847680 with the same
//          value as subsystem id and no 48 kHz support (a real STAC9221
//          carries the board's subsystem id and does 48 kHz)
//   other hypervisor vendors (VMware 0x15ad, InnoTek 0x80ee, Parallels
//          0x1ab8, Microsoft 0x1414, Xen 0x5853, Bochs 0x1234) should one
//          of them ever expose a codec under its own vendor id
//
// VMware Workstation/ESXi expose a Realtek codec model under the VMware
// subsystem vendor: that is a Realtek vendor id and the Realtek driver owns
// it; this driver never claims a vendor id a physical chip maker uses.
//
// What an emulated codec needs is the opposite of what a laptop codec
// needs: no presence detect (the emulations either do not implement it or
// answer with made-up values), no unsolicited responses, no EAPD games and
// exactly one plain path from the converter to the pin the host listens
// to.  The controller is told to relax its command timeouts (HDA_F_VM).
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include <kernel/ke/syscall.h>

/* ---- identification ------------------------------------------------------ */

enum vm_kind {
	VM_NONE = 0,
	VM_QEMU_OUTPUT,
	VM_QEMU_DUPLEX,
	VM_QEMU_MICRO,
	VM_QEMU_OTHER,
	VM_BHYVE,
	VM_VBOX_STAC9221,
	VM_VMWARE,
	VM_VBOX_OWN,
	VM_PARALLELS,
	VM_HYPERV,
	VM_XEN,
	VM_BOCHS,
};

struct vm_vendor {
	uint16_t vendor;
	uint8_t kind;
	const char *vendor_name;
	const char *chip_name;
};

/* Vendor ids that belong to hypervisors, never to codec silicon. */
static const struct vm_vendor vm_vendors[] = {
	{ 0x1af4, VM_QEMU_OTHER, "QEMU", "HDA codec" },
	{ 0x15ad, VM_VMWARE, "VMware", "virtual HDA codec" },
	{ 0x80ee, VM_VBOX_OWN, "VirtualBox", "virtual HDA codec" },
	{ 0x1ab8, VM_PARALLELS, "Parallels", "virtual HDA codec" },
	{ 0x1414, VM_HYPERV, "Microsoft", "Hyper-V HDA codec" },
	{ 0x5853, VM_XEN, "Xen", "virtual HDA codec" },
	{ 0x1234, VM_BOCHS, "Bochs", "virtual HDA codec" },
	{ 0, 0, 0, 0 }
};

#define VBOX_STAC9221_ID 0x83847680u
#define BHYVE_CODEC_ID 0x00008086u
/* AC_PAR_PCM rate bit for 48 kHz */
#define VM_RATE_48K (1u << 6)

static int vm_is_vbox_stac(const struct hda_codec *c)
{
	uint32_t pcm;

	if (c->vendor_id != VBOX_STAC9221_ID)
		return 0;
	/* A real board puts its own subsystem id there; the emulation
	 * reports the codec id itself. */
	if (c->subsystem_id != VBOX_STAC9221_ID)
		return 0;
	/* The emulated converters do 16 kHz, 22.05 kHz and 44.1 kHz at 16
	 * bit only; every real STAC922x lists 48 kHz. */
	pcm = c->afg_pcm;
	if (pcm != 0 && pcm != 0xffffffffu)
		return (pcm & VM_RATE_48K) == 0;
	/* rates not read (yet): fall back to the emulated controller, an
	 * ICH6 (8086:2668) */
	if (c->ctrl && c->ctrl->pci && c->ctrl->pci->vendor_id == 0x8086 &&
	    c->ctrl->pci->device_id == 0x2668)
		return 1;
	return 0;
}

static int vm_classify(const struct hda_codec *c)
{
	uint16_t ven = (uint16_t)(c->vendor_id >> 16);
	uint16_t dev = (uint16_t)(c->vendor_id & 0xffff);
	int i;

	if (c->vendor_id == 0 || c->vendor_id == 0xffffffffu)
		return VM_NONE;
	if (c->vendor_id == BHYVE_CODEC_ID)
		return VM_BHYVE;
	if (vm_is_vbox_stac(c))
		return VM_VBOX_STAC9221;
	if (ven == 0x1af4) {
		switch (dev & 0xfff0) {
		case 0x0010:
			return VM_QEMU_OUTPUT;
		case 0x0020:
			return VM_QEMU_DUPLEX;
		case 0x0030:
			return VM_QEMU_MICRO;
		default:
			return VM_QEMU_OTHER;
		}
	}
	for (i = 0; vm_vendors[i].vendor; i++)
		if (vm_vendors[i].vendor == ven)
			return vm_vendors[i].kind;
	return VM_NONE;
}

static void vm_copy_name(char *dst, size_t size, const char *src)
{
	size_t i;

	if (!dst || size == 0)
		return;
	for (i = 0; src && src[i] && i + 1 < size; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static void vm_set_names(struct hda_codec *c, int kind)
{
	uint16_t ven = (uint16_t)(c->vendor_id >> 16);
	uint16_t dev = (uint16_t)(c->vendor_id & 0xffff);
	const char *vendor = "Virtual";
	const char *chip = "HDA codec";
	int i;

	switch (kind) {
	case VM_QEMU_OUTPUT:
		vendor = "QEMU";
		chip = (dev & 0xf) >= 2 ? "hda-output (mixer)" : "hda-output";
		break;
	case VM_QEMU_DUPLEX:
		vendor = "QEMU";
		chip = (dev & 0xf) >= 2 ? "hda-duplex (mixer)" : "hda-duplex";
		break;
	case VM_QEMU_MICRO:
		vendor = "QEMU";
		chip = (dev & 0xf) >= 2 ? "hda-micro (mixer)" : "hda-micro";
		break;
	case VM_BHYVE:
		vendor = "bhyve";
		chip = "HDA codec";
		break;
	case VM_VBOX_STAC9221:
		vendor = "VirtualBox";
		chip = "STAC9221 (emulated)";
		break;
	default:
		for (i = 0; vm_vendors[i].vendor; i++) {
			if (vm_vendors[i].vendor == ven) {
				vendor = vm_vendors[i].vendor_name;
				chip = vm_vendors[i].chip_name;
				break;
			}
		}
		break;
	}
	vm_copy_name(c->vendor_name, sizeof(c->vendor_name), vendor);
	vm_copy_name(c->chip_name, sizeof(c->chip_name), chip);
}

/* ---- pin helpers ------------------------------------------------------------ */

static inline uint32_t vm_cfg_port(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT;
}

static inline uint32_t vm_cfg_device(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_DEVICE) >> AC_DEFCFG_DEVICE_SHIFT;
}

static int vm_cfg_is_output(uint32_t cfg)
{
	switch (vm_cfg_device(cfg)) {
	case AC_JACK_LINE_OUT:
	case AC_JACK_SPEAKER:
	case AC_JACK_HP_OUT:
		return 1;
	default:
		return 0;
	}
}

static int vm_cfg_unusable(uint32_t cfg)
{
	return cfg == 0 || cfg == 0xffffffffu ||
	       vm_cfg_port(cfg) == AC_JACK_PORT_NONE;
}

/* Line out at the back, green 1/8" jack, association 1, no presence. */
#define VM_CFG_LINE_OUT 0x01014110u
/* Line in at the back, blue, association 3, no presence. */
#define VM_CFG_LINE_IN 0x01813130u
/* Microphone at the front, pink, association 2, no presence. */
#define VM_CFG_MIC_IN 0x02a19120u
/* Not connected. */
#define VM_CFG_NC 0x40f000f0u

static void vm_set_pin_cfg(struct hda_codec *c, uint8_t nid, uint32_t cfg)
{
	struct hda_widget *w = hda_widget(c, nid);

	if (!w || w->type != AC_WID_PIN)
		return;
	hda_pin_override_cfg(c, nid, cfg);
	w->pin_cfg = cfg;
}

/*
 * Strip jack sensing from every pin: the generic code must treat each
 * connected pin as permanently present and never wait for (or poll) a
 * plug event the emulation cannot deliver.  The default configurations
 * also get the "no presence detect" misc bit, so whatever reads them
 * reaches the same conclusion.
 */
static void vm_strip_jack_sensing(struct hda_codec *c)
{
	unsigned int i;

	if (!c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		w->caps &= ~AC_WCAP_UNSOL_CAP;
		if (w->type != AC_WID_PIN)
			continue;
		w->pincap &= ~(AC_PINCAP_PRES_DETECT | AC_PINCAP_TRIG_REQ |
			       AC_PINCAP_IMP_SENSE);
		/* nothing to drive: the emulations have no amplifier enable */
		w->pincap &= ~AC_PINCAP_EAPD;
		w->eapd = 0;
		if (!vm_cfg_unusable(w->pin_cfg) &&
		    !(w->pin_cfg & (AC_DEFCFG_MISC_NO_PRESENCE
				    << AC_DEFCFG_MISC_SHIFT))) {
			uint32_t cfg = w->pin_cfg |
				       (AC_DEFCFG_MISC_NO_PRESENCE
					<< AC_DEFCFG_MISC_SHIFT);
			vm_set_pin_cfg(c, w->nid, cfg);
		}
	}
	c->unsol_capable = 0;
}

/*
 * Emulations that leave the default configuration empty (or mark every pin
 * unconnected) would end with no parsed output at all.  Give the first
 * output-capable pin a line-out configuration and the first input-capable
 * pin a line-in one, but only when nothing usable is configured already.
 */
static void vm_fix_unconfigured_pins(struct hda_codec *c)
{
	unsigned int i;
	int have_out = 0, have_in = 0;
	struct hda_widget *first_out = 0, *first_in = 0;

	if (!c->w)
		return;
	for (i = 0; i < c->num_nodes; i++) {
		struct hda_widget *w = &c->w[i];

		if (w->type != AC_WID_PIN)
			continue;
		if (!vm_cfg_unusable(w->pin_cfg)) {
			if (vm_cfg_is_output(w->pin_cfg))
				have_out = 1;
			else
				have_in = 1;
			continue;
		}
		if ((w->pincap & AC_PINCAP_OUT) && !first_out)
			first_out = w;
		else if ((w->pincap & AC_PINCAP_IN) && !first_in)
			first_in = w;
	}
	if (!have_out && first_out) {
		kprintf("[snd] hda: codec %d: pin 0x%02x unconfigured, using it "
			"as line out\n",
			c->addr, first_out->nid);
		vm_set_pin_cfg(c, first_out->nid, VM_CFG_LINE_OUT);
	}
	if (!have_in && first_in)
		vm_set_pin_cfg(c, first_in->nid, VM_CFG_LINE_IN);
}

/* ---- VirtualBox ---------------------------------------------------------- */

/*
 * The emulated STAC9221 has four DACs (0x02-0x05) and six analog ports,
 * but the host side only ever plays the stream bound to the most recently
 * programmed DAC, and it takes the playback volume and mute exclusively
 * from the output amplifier of DAC1 (0x03), which feeds port C (0x0c).
 * That amplifier comes out of reset muted.  Any other route either plays
 * at the wrong volume or rebinds the host stream when a second converter
 * gets a stream id.  So: port C is the one line out, every other output
 * pin and the S/PDIF pins are disconnected, and capture uses port E (line
 * in) and port D (mic).
 */
#define VBOX_NID_DAC1 0x03
#define VBOX_NID_PORT_A 0x0a
#define VBOX_NID_PORT_B 0x0b
#define VBOX_NID_PORT_C 0x0c
#define VBOX_NID_PORT_D 0x0d
#define VBOX_NID_PORT_E 0x0e
#define VBOX_NID_PORT_F 0x0f
#define VBOX_NID_SPDIF_OUT_PIN 0x10
#define VBOX_NID_SPDIF_IN_PIN 0x11
#define VBOX_NID_CD_PIN 0x15

static const struct hda_pincfg vbox_pins[] = {
	{ VBOX_NID_PORT_A, VM_CFG_NC },
	{ VBOX_NID_PORT_B, VM_CFG_NC },
	{ VBOX_NID_PORT_C, VM_CFG_LINE_OUT },
	{ VBOX_NID_PORT_D, VM_CFG_MIC_IN },
	{ VBOX_NID_PORT_E, VM_CFG_LINE_IN },
	{ VBOX_NID_PORT_F, VM_CFG_NC },
	{ VBOX_NID_SPDIF_OUT_PIN, VM_CFG_NC },
	{ VBOX_NID_SPDIF_IN_PIN, VM_CFG_NC },
	{ VBOX_NID_CD_PIN, VM_CFG_NC },
	{ 0, 0 }
};

static void vbox_pre_parse(struct hda_codec *c)
{
	const struct hda_pincfg *p;

	for (p = vbox_pins; p->nid; p++)
		vm_set_pin_cfg(c, p->nid, p->cfg);
}

/* The emulated amplifiers count 0..0x7f, 0x7f being 0 dB. */
static void vbox_unmute_line_out(struct hda_codec *c)
{
	uint32_t caps = hda_amp_caps(c, VBOX_NID_DAC1, HDA_OUTPUT);
	uint32_t gain = (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;

	if (gain == 0)
		gain = (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
	if (gain == 0)
		gain = 0x7f;
	hda_amp_set(c, VBOX_NID_DAC1, HDA_OUTPUT, 0, gain, gain, 0);
}

/* ---- path helpers ------------------------------------------------------------ */

/*
 * When the parser found no amplifier to put the volume on (the QEMU
 * no-mixer models have none at all), make sure whatever amplifier sits on
 * the path is open at 0 dB so the stream is audible.
 */
static void vm_open_path_amps(struct hda_codec *c, const struct hda_path *p)
{
	unsigned int i;

	for (i = 0; i < p->depth && i < HDA_MAX_PATH; i++) {
		uint8_t nid = p->nid[i];
		struct hda_widget *w = hda_widget(c, nid);
		uint32_t caps, gain;

		if (!w)
			continue;
		if (w->caps & AC_WCAP_OUT_AMP) {
			caps = hda_amp_caps(c, nid, HDA_OUTPUT);
			gain = (caps & AC_AMPCAP_OFFSET) >>
			       AC_AMPCAP_OFFSET_SHIFT;
			hda_amp_set(c, nid, HDA_OUTPUT, 0, gain, gain, 0);
		}
		if ((w->caps & AC_WCAP_IN_AMP) && i > 0) {
			caps = hda_amp_caps(c, nid, HDA_INPUT);
			gain = (caps & AC_AMPCAP_OFFSET) >>
			       AC_AMPCAP_OFFSET_SHIFT;
			hda_amp_set(c, nid, HDA_INPUT, p->idx[i], gain, gain,
				    0);
		}
	}
}

/* ---- driver callbacks ---------------------------------------------------- */

static int vm_match(struct hda_codec *c)
{
	return vm_classify(c) != VM_NONE;
}

static int vm_pre_parse(struct hda_codec *c)
{
	int kind = vm_classify(c);

	if (kind == VM_NONE)
		return -ENODEV;
	vm_set_names(c, kind);

	/* the controller in front of an emulated codec is emulated too */
	if (c->ctrl)
		c->ctrl->flags |= HDA_F_VM;

	if (kind == VM_VBOX_STAC9221)
		vbox_pre_parse(c);

	vm_fix_unconfigured_pins(c);
	vm_strip_jack_sensing(c);

	/* no GPIOs worth touching on any of these */
	c->gpio_mask = c->gpio_dir = c->gpio_data = 0;

	kprintf("[snd] hda: codec %d: %s %s (0x%08x), emulated: plain "
		"path, no jack sensing\n",
		c->addr, c->vendor_name, c->chip_name, c->vendor_id);
	return 0;
}

static int vm_post_parse(struct hda_codec *c)
{
	struct hda_spec *s = &c->spec;
	int kind = vm_classify(c);
	int i;

	if (kind == VM_NONE)
		return 0;

	s->auto_mute = 0;
	s->jack_poll = 0;
	if (s->nout > HDA_MAX_OUTPUTS)
		s->nout = HDA_MAX_OUTPUTS;
	if (s->nin > HDA_MAX_INPUTS)
		s->nin = HDA_MAX_INPUTS;
	for (i = 0; i < s->nout; i++) {
		s->out[i].jack_detect = 0;
		s->out[i].present = 1;
		s->out[i].eapd = 0;
		if (!s->out[i].path.vol_nid)
			vm_open_path_amps(c, &s->out[i].path);
	}
	for (i = 0; i < s->nin; i++) {
		s->in[i].jack_detect = 0;
		s->in[i].present = 1;
	}

	if (kind == VM_VBOX_STAC9221) {
		if (s->primary_dac && s->primary_dac != VBOX_NID_DAC1)
			kprintf("[snd] hda: codec %d: VirtualBox: playing "
				"through DAC 0x%02x, host volume follows DAC "
				"0x%02x\n",
				c->addr, s->primary_dac, VBOX_NID_DAC1);
		/* host volume and mute follow this amplifier only */
		vbox_unmute_line_out(c);
		/* a stray S/PDIF converter binding would steal the stream */
		s->dig_out_nid = 0;
	}
	if (s->nout == 0)
		kprintf("[snd] hda: codec %d: emulated codec without a usable "
			"output pin\n",
			c->addr);
	return 0;
}

static int vm_init(struct hda_codec *c)
{
	int kind = vm_classify(c);
	int i;

	if (kind == VM_NONE)
		return 0;
	/* VirtualBox resets its function group to D2 */
	if (c->afg)
		hda_set_power(c, c->afg, AC_PWRST_D0);

	for (i = 0; i < c->spec.nout && i < HDA_MAX_OUTPUTS; i++) {
		struct hda_out *o = &c->spec.out[i];

		if (o->dac)
			hda_set_power(c, o->dac, AC_PWRST_D0);
		if (o->pin)
			hda_set_power(c, o->pin, AC_PWRST_D0);
	}
	if (kind == VM_VBOX_STAC9221) {
		hda_set_power(c, VBOX_NID_DAC1, AC_PWRST_D0);
		hda_pin_set_ctl(c, VBOX_NID_PORT_C, AC_PINCTL_OUT_EN);
		/* a reset re-mutes the line-out amplifier; restore it only
		 * when the parser has no volume control of its own on it */
		if (!c->spec.nout || !c->spec.out[0].path.vol_nid)
			vbox_unmute_line_out(c);
	}
	return 0;
}

static void vm_automute(struct hda_codec *c, int hp_present)
{
	(void)c;
	(void)hp_present;
	/* every pin is permanently present: nothing switches */
}

static void vm_unsol(struct hda_codec *c, uint32_t res)
{
	/* the emulations never send any; ignore whatever shows up */
	(void)c;
	(void)res;
}

static void vm_playback_hook(struct hda_codec *c, int start)
{
	if (!start)
		return;
	/* VirtualBox drops the function group to a low power state on its
	 * own after a reset; make sure it runs when a stream starts */
	if (vm_classify(c) == VM_VBOX_STAC9221 && c->afg)
		hda_set_power(c, c->afg, AC_PWRST_D0);
}

const struct hda_vendor_driver hda_vm_driver = {
	.name = "vm",
	.match = vm_match,
	.pre_parse = vm_pre_parse,
	.post_parse = vm_post_parse,
	.init = vm_init,
	.automute = vm_automute,
	.unsol = vm_unsol,
	.playback_hook = vm_playback_hook,
	.probe = 0,
	.remove = 0,
};
