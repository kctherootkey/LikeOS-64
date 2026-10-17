// LikeOS Intel High Definition Audio - private state of the codec layer
// (hda_codec.c, hda_parse.c, hda_generic.c).  Nothing outside these three
// files includes this header; the rest of the driver sees only hda.h.
//
// A codec is allocated as a struct hda_codec_priv whose first member is the
// public struct hda_codec, so the codec layer can keep bookkeeping the
// shared header does not describe (the verb-sequence lock, jack tags, the
// software side of every mixer control, the converters bound to a stream,
// the fixup tables to replay after a reset) without other files knowing.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_HDA_CODEC_PRIV_H
#define KERNEL_DEV_SOUND_HDA_CODEC_PRIV_H

#include "hda.h"
#include "hda_hdmi.h"

#define HDA_LOG "hda"

/* Unsolicited-response tags 1..HDA_TAG_MAX-1 are handed out to jack pins
 * by the generic layer; tag 0 is never used (a zero tag cannot be told
 * apart from a stray response). */
#define HDA_TAG_MAX 64

/* Fixup tables remembered for replay after a controller reset/resume. */
#define HDA_REPLAY_MAX 8

/* Output/input ids on the card: the codec address in bits 7:4 keeps the
 * ids of several codecs on one card apart, inputs set bit 3.  All analog
 * ids stay below 0x100, the start of the HDMI/DP output ids (hda_hdmi.h). */
#define HDA_OUT_ID(addr, i) \
	((uint32_t)(((addr) & 0xf) << 4) | ((uint32_t)(i) & 7u))
#define HDA_IN_ID(addr, i) \
	((uint32_t)(((addr) & 0xf) << 4) | 8u | ((uint32_t)(i) & 7u))

/* Number of AUDIO_CTL_* controls kept in software. */
#define HDA_NCTL (AUDIO_CTL_MAX + 1)

struct hda_codec_priv {
	struct hda_codec c; /* must stay first */

	/* Verb-sequence lock.  `c.lock' (a spinlock) only guards these three
	 * fields for a moment; the sequence itself runs with the spinlock
	 * dropped, because hda_cmd() may sleep once the scheduler runs.  The
	 * lock is recursive per task so vendor callbacks invoked under it
	 * (automute, init) can use the locked helpers again. */
	volatile int seq_busy;
	void *volatile seq_owner;
	int seq_depth;

	/* PCI subsystem id of the controller (for fixup matching). */
	uint32_t pci_ssid;

	/* jack tags: tag -> pin nid, 0 = free */
	uint8_t tag_nid[HDA_TAG_MAX];
	uint8_t ntags;

	/* software state of every control (0..100), what get returns */
	uint32_t ctl_l[HDA_NCTL], ctl_r[HDA_NCTL];
	uint32_t ctl_mute[HDA_NCTL];
	int ctl_initialised;

	/* routing */
	uint8_t out_active[HDA_MAX_OUTPUTS];
	uint32_t active_mask; /* bit i = out[i] plays */
	uint32_t in_mode; /* AUDIO_OUTSEL_AUTO or _ID */
	uint32_t in_sel_id;
	int in_active; /* index into spec.in[], -1 = none */
	uint32_t fix_flags; /* HDA_FIX_* of applied fixups */

	/* converters currently bound to a stream */
	uint8_t bound_play[HDA_MAX_OUTPUTS + 2];
	int nbound_play;
	uint8_t bound_cap;
	int play_tag, cap_tag;
	uint16_t play_fmt, cap_fmt;
	int play_channels, cap_channels;

	/* fixup tables applied by the vendor driver, replayed on resume */
	const struct hda_pincfg *rp_pins[HDA_REPLAY_MAX];
	const struct hda_verb *rp_verbs[HDA_REPLAY_MAX];
	const struct hda_coef_fix *rp_coefs[HDA_REPLAY_MAX];
	int nrp_pins, nrp_verbs, nrp_coefs;
	const struct hda_fixup *fixup; /* the matched subsystem fixup */

	int generic_parsed; /* hda_generic_parse ran (analog function) */
	int probe_done;
};

static inline struct hda_codec_priv *hda_priv(struct hda_codec *c)
{
	return (struct hda_codec_priv *)c;
}

/* A display (HDMI/DP) codec, driven by hda_hdmi.c. */
static inline int hda_is_display(struct hda_codec *c)
{
	return c->is_hdmi || c->vendor == &hda_hdmi_driver;
}

/* ---- hda_codec.c ---------------------------------------------------------- */

/* Serialise a multi-verb sequence of one codec (recursive per task). */
void hda_codec_lock(struct hda_codec *c);
void hda_codec_unlock(struct hda_codec *c);
/* Number of channels a widget handles (1..16). */
uint32_t hda_widget_channels(const struct hda_widget *w);
/* The PCM / stream caps of a converter, resolving "inherit the AFG". */
uint32_t hda_conv_pcm(struct hda_codec *c, uint8_t nid);
uint32_t hda_conv_stream_fmts(struct hda_codec *c, uint8_t nid);
/* Re-read the connection select of a widget (cache in w->cur_conn). */
int hda_conn_index(struct hda_codec *c, uint8_t from, uint8_t to);
/* Give a jack pin an unsolicited tag (0 if the table is full). */
uint8_t hda_jack_tag_for(struct hda_codec *c, uint8_t nid);
/* Presence detect is usable on this pin (caps and config agree). */
int hda_pin_has_presence(struct hda_codec *c, uint8_t nid);

/* ---- hda_parse.c ---------------------------------------------------------- */

/* Fill c->vendor_name / c->chip_name from the vendor and device id. */
void hda_codec_set_names(struct hda_codec *c);
/* Short descriptive strings for pin default-config fields. */
const char *hda_pincfg_port_name(uint32_t cfg);
const char *hda_pincfg_location_name(uint32_t cfg);
const char *hda_pincfg_device_name(uint32_t cfg);
const char *hda_pincfg_conn_name(uint32_t cfg);
const char *hda_pincfg_color_name(uint32_t cfg);
const char *hda_widget_type_name(uint32_t type);

/* ---- hda_generic.c -------------------------------------------------------- */

/* Apply the volume/mute plan to every output and input amp. */
void hda_generic_apply_amps(struct hda_codec *c);
/* Re-read presence of every jack pin; returns 1 if anything changed and
 * reports the changes to the card. */
int hda_generic_sense_jacks(struct hda_codec *c, int report);
/* dB (1/100) of a given step on an amp, and the step for a percentage. */
int32_t hda_amp_step_db(uint32_t caps, uint32_t step);
uint32_t hda_amp_pct_to_step(uint32_t caps, uint32_t pct);

/* ---- pin default configuration fields ---------------------------------------- */

static inline uint32_t hda_cfg_port(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT;
}
static inline uint32_t hda_cfg_location(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_LOCATION) >> AC_DEFCFG_LOCATION_SHIFT;
}
static inline uint32_t hda_cfg_device(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_DEVICE) >> AC_DEFCFG_DEVICE_SHIFT;
}
static inline uint32_t hda_cfg_conn_type(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_CONN_TYPE) >> AC_DEFCFG_CONN_TYPE_SHIFT;
}
static inline uint32_t hda_cfg_color(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_COLOR) >> AC_DEFCFG_COLOR_SHIFT;
}
static inline uint32_t hda_cfg_misc(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_MISC) >> AC_DEFCFG_MISC_SHIFT;
}
static inline uint32_t hda_cfg_assoc(uint32_t cfg)
{
	return (cfg & AC_DEFCFG_DEF_ASSOC) >> AC_DEFCFG_ASSOC_SHIFT;
}
static inline uint32_t hda_cfg_seq(uint32_t cfg)
{
	return cfg & AC_DEFCFG_SEQUENCE;
}

/* amp caps fields */
static inline uint32_t hda_amp_nsteps(uint32_t caps)
{
	return (caps & AC_AMPCAP_NUM_STEPS) >> AC_AMPCAP_NUM_STEPS_SHIFT;
}
static inline uint32_t hda_amp_offset(uint32_t caps)
{
	return (caps & AC_AMPCAP_OFFSET) >> AC_AMPCAP_OFFSET_SHIFT;
}
static inline uint32_t hda_amp_stepsize(uint32_t caps)
{
	return (caps & AC_AMPCAP_STEP_SIZE) >> AC_AMPCAP_STEP_SIZE_SHIFT;
}

/* Bounded string helpers (the kernel has no strlcpy). */
static inline void hda_strlcpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	if (!n)
		return;
	if (src) {
		for (; i + 1 < n && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = '\0';
}

#endif /* KERNEL_DEV_SOUND_HDA_CODEC_PRIV_H */
