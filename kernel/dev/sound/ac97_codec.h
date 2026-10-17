// LikeOS AC'97 codec layer - register map and the interface between an
// AC'97 controller driver (ac97_ich.c) and the codec code (ac97_codec.c).
//
// An AC'97 link carries up to four codecs.  The controller moves PCM data
// over the link slots and gives the codec code a way to read and write the
// 64 16-bit codec registers; everything about the codec itself - reset and
// power-up, identification, sample-rate programming, the analog mixer
// (volumes, mutes, record source, gains) and per-vendor fixups - lives in
// ac97_codec.c and is reached through the functions declared here.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_AC97_CODEC_H
#define KERNEL_DEV_SOUND_AC97_CODEC_H

#include <kernel/dev/sound/snd.h>

/* ---- codec registers (AC'97 2.3) ------------------------------------------ */

#define AC97_RESET 0x00 /* write: register reset; read: capabilities */
#define AC97_MASTER 0x02 /* master (line out) volume */
#define AC97_HEADPHONE 0x04 /* headphone / aux out volume */
#define AC97_MASTER_MONO 0x06 /* mono out volume */
#define AC97_MASTER_TONE 0x08 /* bass and treble */
#define AC97_PC_BEEP 0x0a
#define AC97_PHONE 0x0c
#define AC97_MIC 0x0e
#define AC97_LINE 0x10
#define AC97_CD 0x12
#define AC97_VIDEO 0x14
#define AC97_AUX 0x16
#define AC97_PCM 0x18 /* PCM out (DAC) gain */
#define AC97_REC_SEL 0x1a /* record select */
#define AC97_REC_GAIN 0x1c /* record gain */
#define AC97_REC_GAIN_MIC 0x1e /* dedicated mic ADC gain */
#define AC97_GENERAL_PURPOSE 0x20
#define AC97_3D_CONTROL 0x22
#define AC97_INT_PAGING 0x24 /* 2.3: interrupt and page select */
#define AC97_POWERDOWN 0x26 /* powerdown control / status */
#define AC97_EXTENDED_ID 0x28
#define AC97_EXTENDED_STATUS 0x2a
#define AC97_PCM_FRONT_DAC_RATE 0x2c
#define AC97_PCM_SURR_DAC_RATE 0x2e
#define AC97_PCM_LFE_DAC_RATE 0x30
#define AC97_PCM_LR_ADC_RATE 0x32
#define AC97_PCM_MIC_ADC_RATE 0x34
#define AC97_CENTER_LFE_MASTER 0x36
#define AC97_SURROUND_MASTER 0x38
#define AC97_SPDIF 0x3a
#define AC97_EXTENDED_MID 0x3c /* modem codecs */
#define AC97_EXTENDED_MSTATUS 0x3e
#define AC97_VENDOR_ID1 0x7c
#define AC97_VENDOR_ID2 0x7e

#define AC97_NUM_REGS 64 /* 0x00..0x7e, word registers */

/* Volume register layout */
#define AC97_MUTE 0x8000 /* mute (left on split-mute codecs) */
#define AC97_MUTE_RIGHT 0x0080 /* right mute on split-mute codecs */
#define AC97_MIC_BOOST 0x0040 /* +20 dB microphone preamp */

/* Capabilities (read of AC97_RESET) */
#define AC97_BC_DEDICATED_MIC 0x0001
#define AC97_BC_BASS_TREBLE 0x0004
#define AC97_BC_SIM_STEREO 0x0008
#define AC97_BC_HEADPHONE 0x0010
#define AC97_BC_LOUDNESS 0x0020
#define AC97_BC_DAC_MASK 0x00c0 /* 0 = 16 bit, 0x40 = 18, 0x80 = 20 */
#define AC97_BC_ADC_MASK 0x0300
#define AC97_BC_3D_MASK 0x7c00

/* Powerdown register */
#define AC97_PD_ADC_READY 0x0001
#define AC97_PD_DAC_READY 0x0002
#define AC97_PD_ANL_READY 0x0004
#define AC97_PD_REF_READY 0x0008
#define AC97_PD_READY_MASK 0x000f
#define AC97_PD_PR0 0x0100 /* ADC and input mux off */
#define AC97_PD_PR1 0x0200 /* front DAC off */
#define AC97_PD_PR2 0x0400 /* analog mixer off (Vref on) */
#define AC97_PD_PR3 0x0800 /* analog mixer off (Vref off) */
#define AC97_PD_PR4 0x1000 /* AC-link off */
#define AC97_PD_PR5 0x2000 /* internal clock off */
#define AC97_PD_PR6 0x4000 /* headphone amplifier off */
#define AC97_PD_EAPD 0x8000 /* external amplifier power down */

/* Extended audio ID */
#define AC97_EI_VRA 0x0001
#define AC97_EI_DRA 0x0002
#define AC97_EI_SPDIF 0x0004
#define AC97_EI_VRM 0x0008
#define AC97_EI_CDAC 0x0040
#define AC97_EI_SDAC 0x0080
#define AC97_EI_LDAC 0x0100
#define AC97_EI_AMAP 0x0200
#define AC97_EI_REV_MASK 0x0c00
#define AC97_EI_REV_SHIFT 10
#define AC97_EI_ADDR_MASK 0xc000

/* Extended audio status / control */
#define AC97_EA_VRA 0x0001
#define AC97_EA_DRA 0x0002
#define AC97_EA_SPDIF 0x0004
#define AC97_EA_VRM 0x0008
#define AC97_EA_PRI 0x0800 /* center DAC off */
#define AC97_EA_PRJ 0x1000 /* surround DACs off */
#define AC97_EA_PRK 0x2000 /* LFE DAC off */
#define AC97_EA_PRL 0x4000 /* mic ADC off */

/* Record select sources (same value for both channels) */
#define AC97_REC_MIC 0
#define AC97_REC_CD 1
#define AC97_REC_VIDEO 2
#define AC97_REC_AUX 3
#define AC97_REC_LINE 4
#define AC97_REC_STEREO_MIX 5
#define AC97_REC_MONO_MIX 6
#define AC97_REC_PHONE 7
#define AC97_REC_NSOURCES 8

/* ---- per-machine and per-codec quirks ------------------------------------ */

#define AC97_Q_HP_ONLY (1u << 0) /* the headphone amp drives the main jack */
#define AC97_Q_SWAP_HP (1u << 1) /* master and headphone are swapped */
#define AC97_Q_INV_EAPD (1u << 2) /* EAPD bit set = amplifier ON */
#define AC97_Q_AD_SHARING (1u << 3) /* AD1985: route mic to the back panel */
#define AC97_Q_NO_VRA (1u << 4) /* do not trust variable rate audio */
#define AC97_Q_NO_EAPD (1u << 5) /* leave the EAPD bit alone */
#define AC97_Q_STEREO_MUTES (1u << 6) /* split left/right mute bits */
#define AC97_Q_NO_POWER_WAIT (1u << 7) /* powerdown status never settles */

/* ---- the codec ------------------------------------------------------------ */

#define AC97_MAX_CODECS 4

/* How the controller reaches the codec registers.  `num' is the codec
 * number on the link (0 = primary).  Both may busy-wait briefly and must
 * work with interrupts disabled; they return 0 or a negative errno
 * (-EIO on a link timeout).  A failed read stores 0xffff. */
struct ac97_bus_ops {
	int (*read)(void *bus, unsigned int num, unsigned int reg,
		    uint16_t *val);
	int (*write)(void *bus, unsigned int num, unsigned int reg,
		     uint16_t val);
};

/* One analog gain/attenuation register as the codec implements it. */
struct ac97_vol {
	uint8_t reg; /* AC97_* register, 0 = absent */
	uint8_t present;
	uint8_t stereo; /* has a left field in bits 13:8 */
	uint8_t max; /* largest field value (15, 31 or 63) */
	uint8_t zero; /* field value meaning 0 dB (gain registers) */
	uint8_t gain; /* 1 = field counts up in gain, 0 = attenuation */
	uint8_t use_steps; /* steps 1..100 % are spread over */
};

/* A control as the user sees it: 0..100 per channel and a mute. */
struct ac97_level {
	uint32_t left, right; /* 0..100 */
	uint32_t mute;
};

/* Physical outputs */
#define AC97_OUT_MAIN 0 /* "Speaker/Line Out": master (or hp on HP_ONLY) */
#define AC97_OUT_HP 1 /* "Headphone" */
#define AC97_NUM_OUTS 2

/* Output ids reported through audio_outputs */
#define AC97_OUT_ID_MAIN 1
#define AC97_OUT_ID_HP 2

struct ac97_codec {
	spinlock_t lock; /* serialises register access and the cache */
	const struct ac97_bus_ops *ops;
	void *bus;
	unsigned int num; /* codec number on the link */

	/* identity */
	uint32_t id; /* VENDOR_ID1 << 16 | VENDOR_ID2 */
	char vendor[28];
	char chip[36];
	char name[64]; /* "<vendor> <chip>" */
	uint16_t caps; /* capabilities from AC97_RESET */
	uint16_t ext_id; /* AC97_EXTENDED_ID */
	uint16_t subsys_vendor, subsys_device; /* the controller's */
	uint32_t quirks; /* AC97_Q_* */
	uint32_t ctrl_flags; /* AC97_CTRL_* from the controller */
	int revision; /* 0 = 2.1, 1 = 2.2, 2 = 2.3 */

	/* sample rates */
	uint32_t clock; /* link reference clock, nominally 48000 */
	int vra; /* variable rate audio enabled */
	uint32_t rates[2]; /* AUDIO_RATE_* per AUDIO_DIR_* */
	uint32_t cur_rate[2];

	/* register shadow */
	uint16_t regs[AC97_NUM_REGS];
	uint64_t reg_valid; /* bit (reg >> 1) set once regs[] holds it */

	/* mixer layout, detected at init */
	struct ac97_vol hw_master; /* AC97_MASTER */
	struct ac97_vol hw_hp; /* AC97_HEADPHONE */
	struct ac97_vol hw_mono; /* AC97_MASTER_MONO */
	struct ac97_vol hw_pcm; /* AC97_PCM */
	struct ac97_vol hw_recgain; /* AC97_REC_GAIN */
	struct ac97_vol hw_mic; /* AC97_MIC (analog loopback, boost bit) */
	uint8_t out_reg[AC97_NUM_OUTS]; /* volume register for each output */
	uint8_t out_reg2[AC97_NUM_OUTS]; /* a second register kept in step */
	int num_outputs;
	uint32_t rec_sources; /* bitmask of AC97_REC_* offered */

	/* user state */
	struct ac97_level master; /* AUDIO_CTL_MASTER: scales all outputs */
	struct ac97_level out[AC97_NUM_OUTS]; /* SPEAKER / HEADPHONE */
	struct ac97_level pcm; /* AUDIO_CTL_PCM */
	struct ac97_level capture; /* AUDIO_CTL_CAPTURE */
	uint32_t mic_boost; /* 0 or 100 */
	uint32_t out_mode; /* AUDIO_OUTSEL_* */
	uint32_t out_selected; /* id for AUDIO_OUTSEL_ID */
	uint32_t rec_source; /* AC97_REC_* */
	int eapd_on;

	int ready; /* init completed */
};

/* Controller hints for ac97_codec_init() */
#define AC97_CTRL_NO_VRA (1u << 0) /* controller cannot do variable rates */
#define AC97_CTRL_IN_VM (1u << 1) /* emulated hardware: fewer retries */

/* ---- functions for the controller ----------------------------------------- */

/* Probe codec `num' on a link that the controller has already brought out
 * of reset: wait until it answers, identify it and tell audio codecs from
 * modem-only ones.  Fills `c'.  Returns 0 for a usable audio codec,
 * -ENODEV for nothing (or a modem), -EIO for a link that does not answer.
 * Busy-waits; call before the device is published. */
int ac97_codec_probe(struct ac97_codec *c, const struct ac97_bus_ops *ops,
		     void *bus, unsigned int num, uint16_t subsys_vendor,
		     uint16_t subsys_device, uint32_t ctrl_flags);

/* Bring a probed codec up: register reset, power-up handshake, vendor
 * fixups, rate detection, mixer layout detection and sane defaults
 * (outputs audible, analog loopbacks muted, record from the microphone). */
int ac97_codec_init(struct ac97_codec *c);

/* Re-apply every setting after the controller reset the link (resume, or
 * recovery from a wedged link). */
int ac97_codec_restore(struct ac97_codec *c);

/* Power everything down and mute (driver detach). */
void ac97_codec_shutdown(struct ac97_codec *c);

/* Override the link clock (measured by the controller); 0 = keep. */
void ac97_codec_set_clock(struct ac97_codec *c, uint32_t clock);

/* Program the DAC (AUDIO_DIR_PLAYBACK) or ADC (AUDIO_DIR_CAPTURE) rate.
 * Returns 0 or -EINVAL for a rate the codec cannot do.  Process context. */
int ac97_codec_set_rate(struct ac97_codec *c, int dir, uint32_t rate);
uint32_t ac97_codec_rates(const struct ac97_codec *c, int dir);

/* Power the ADC path up or down as capture starts and stops. */
void ac97_codec_capture_power(struct ac97_codec *c, int on);

/* snd_ctl_ops back ends (process context). */
int ac97_codec_get_volume(struct ac97_codec *c, uint32_t ctl,
			  struct audio_volume *v);
int ac97_codec_set_volume(struct ac97_codec *c, uint32_t ctl, uint32_t left,
			  uint32_t right);
int ac97_codec_get_mute(struct ac97_codec *c, uint32_t ctl, uint32_t *mute);
int ac97_codec_set_mute(struct ac97_codec *c, uint32_t ctl, uint32_t mute);
int ac97_codec_get_outputs(struct ac97_codec *c, struct audio_outputs *o);
int ac97_codec_set_output(struct ac97_codec *c,
			  const struct audio_setoutput *s);
int ac97_codec_get_inputs(struct ac97_codec *c, struct audio_outputs *o);
int ac97_codec_set_input(struct ac97_codec *c,
			 const struct audio_setoutput *s);

/* Text dump of the codec (identity, rates, mixer state, every register)
 * into `buf'; returns the length written (NUL not counted). */
long ac97_codec_diag(struct ac97_codec *c, int full, char *buf, size_t len);

/* Raw access (diagnostics, controller quirks).  Locked. */
int ac97_codec_read(struct ac97_codec *c, unsigned int reg, uint16_t *val);
int ac97_codec_write(struct ac97_codec *c, unsigned int reg, uint16_t val);

/* The vendor and chip names for an id ("SigmaTel", "STAC9700,83,84");
 * never NULL. */
const char *ac97_vendor_name(uint32_t id);
const char *ac97_chip_name(uint32_t id);

#endif /* KERNEL_DEV_SOUND_AC97_CODEC_H */
