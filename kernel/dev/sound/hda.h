// LikeOS Intel High Definition Audio driver - internal interfaces.
//
// The driver is split along the hardware:
//
//   hda_controller.c, hda_stream.c, hda_pci_ids.c   the PCI controller:
//       reset, CORB/RIRB command transport, stream DMA engines, interrupts,
//       the sound card and its PCM devices (owner: controller side)
//   hda_codec.c, hda_parse.c, hda_generic.c         one codec on the link:
//       enumeration, widget graph, the generic parser that finds output and
//       input paths, volume/mute/jack handling (owner: codec side)
//   hda_realtek.c, hda_conexant.c, hda_idt.c, hda_analog.c, hda_cirrus.c,
//   hda_via.c, hda_ca0132.c, hda_vm.c               vendor fixups
//   hda_hdmi.c                                       HDMI/DP codecs
//
// Each side talks to the others only through what is declared here.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_HDA_H
#define KERNEL_DEV_SOUND_HDA_H

#include <kernel/dev/sound/snd.h>
#include "hda_verbs.h"

#define HDA_MAX_CODECS 15 /* SDIN lines/addresses */
#define HDA_MAX_STREAMS 30 /* ISS + OSS + BSS, spec maximum */
#define HDA_MAX_BDL 256 /* entries per stream */
#define HDA_MAX_NODES 256
#define HDA_MAX_CONNS 32 /* connection list entries kept per widget */
#define HDA_MAX_PATH 8 /* widgets on one path, pin to converter */
#define HDA_MAX_OUTPUTS 8 /* parsed outputs per codec */
#define HDA_MAX_INPUTS 8
#define HDA_MAX_HDMI_PINS 8

/* amp directions */
#define HDA_INPUT 0
#define HDA_OUTPUT 1

struct hda_controller;
struct hda_codec;
struct hda_vendor_driver;

/* ---- controller quirks and identification (hda_pci_ids.c) --------------- */

/* Controller behaviour flags (struct hda_pci_id.flags and ctrl->flags) */
#define HDA_F_INTEL_SNOOP (1u << 0) /* Intel PCH: set the snoop bits */
#define HDA_F_SCH_SNOOP (1u << 1) /* Intel SCH/Atom style snoop */
#define HDA_F_ATI_SNOOP (1u << 2)
#define HDA_F_NVIDIA_SNOOP (1u << 3)
#define HDA_F_NO_64BIT (1u << 4) /* DMA below 4 GiB regardless of GCAP */
#define HDA_F_NO_MSI (1u << 5) /* MSI is broken: use INTx */
#define HDA_F_POSFIX_LPIB (1u << 6) /* trust LPIB, not the position buffer */
#define HDA_F_POSFIX_POSBUF (1u << 7) /* trust the position buffer */
#define HDA_F_POSFIX_COMBO (1u << 8) /* LPIB for playback, posbuf capture */
#define HDA_F_SKL_PLUS (1u << 9) /* Skylake+: PPCAP/ML, link power */
#define HDA_F_HSW_DISPLAY (1u << 10) /* Haswell/Broadwell display HDA */
#define HDA_F_DSP_PRESENT (1u << 11) /* prog-if 0x80: has an audio DSP */
#define HDA_F_HDMI_ONLY (1u << 12) /* only display codecs on this link */
#define HDA_F_NO_STREAM_RESET (1u << 13)
#define HDA_F_ALIGN_BDLE_4K (1u << 14) /* BDL entries must not cross 4K */
#define HDA_F_PROBE_MASK_1 (1u << 15) /* only probe codec 0 */
#define HDA_F_SINGLE_CMD (1u << 16) /* use immediate command, no CORB */
#define HDA_F_NO_UNSOL (1u << 17) /* poll jacks; no unsolicited */
#define HDA_F_AMD_GCAP_FIX (1u << 18) /* GCAP lies about streams */
#define HDA_F_CTX_RESET_WAIT (1u << 19) /* extra wait after CRST */
#define HDA_F_PCH_PWR_WELL (1u << 20) /* needs the display power well */
#define HDA_F_VM (1u << 21) /* emulated: relax timeouts */
#define HDA_F_INTEL_PCH (1u << 22)
#define HDA_F_NVIDIA (1u << 23)
#define HDA_F_AMD (1u << 24)

struct hda_pci_id {
	uint16_t vendor, device;
	uint32_t flags; /* HDA_F_* */
	const char *name; /* "Sunrise Point-H PCH" */
};

/* The table entry for a PCI device, or a generic entry for an unknown
 * class-0x0403 controller (never NULL for a class match). */
const struct hda_pci_id *hda_pci_lookup(const pci_device_t *pci);

/* ---- streams (hda_stream.c) -------------------------------------------- */

struct hda_bdle {
	uint64_t addr;
	uint32_t len;
	uint32_t ioc; /* bit 0: interrupt on completion */
} __attribute__((packed));

struct hda_stream {
	struct hda_controller *ctrl;
	int index; /* engine index 0..n-1 in register order */
	int direction; /* AUDIO_DIR_* */
	int tag; /* stream tag 1..15 given to the codec */
	int in_use;
	uint32_t sd_off; /* register offset of this stream descriptor */
	struct hda_bdle *bdl; /* BDL (uncached) */
	uint64_t bdl_phys;
	uint32_t nbdl;
	uint64_t buf_phys; /* the ring the PCM core allocated */
	uint32_t buffer_bytes, period_bytes;
	uint16_t format; /* stream format word */
	int running;
	uint32_t last_pos;
	uint32_t irq_count;
	struct snd_pcm_substream *substream;
	struct hda_codec *codec; /* the codec playing this stream */
	int hdmi; /* the stream feeds an HDMI converter */
};

/* Find a free engine for the direction (output streams for playback). */
struct hda_stream *hda_stream_assign(struct hda_controller *ctrl,
				     int direction);
void hda_stream_release(struct hda_stream *st);
/* Program BDL (one entry per period, split at 4K boundaries if the
 * controller wants it), CBL, LVI, format and tag.  Stream stopped. */
int hda_stream_setup(struct hda_stream *st, uint64_t buf_phys,
		     uint32_t buffer_bytes, uint32_t period_bytes,
		     uint16_t format);
void hda_stream_reset(struct hda_stream *st);
void hda_stream_start(struct hda_stream *st);
void hda_stream_stop(struct hda_stream *st);
/* The DMA position in bytes, [0, buffer_bytes). */
uint32_t hda_stream_position(struct hda_stream *st);
/* The format word for rate/channels/bits; 0 if not representable. */
uint16_t hda_calc_format(uint32_t rate, uint32_t channels, uint32_t bits);
/* From the interrupt handler: SDnSTS for a stream. */
void hda_stream_irq(struct hda_stream *st);

/* ---- the controller (hda_controller.c) -------------------------------------- */

struct hda_controller {
	const pci_device_t *pci;
	const struct hda_pci_id *id;
	uint32_t flags; /* HDA_F_*, from the table plus runtime fixes */
	char name[64];
	char bus[32];

	volatile uint8_t *mmio;
	uint64_t mmio_phys;
	uint64_t mmio_size;

	/* capabilities */
	uint16_t gcap;
	int num_iss, num_oss, num_bss;
	int addr64; /* 64-bit DMA addresses allowed */
	uint8_t vmaj, vmin;

	/* command transport */
	spinlock_t cmd_lock;
	uint32_t *corb; /* 256 entries max, uncached */
	uint64_t corb_phys;
	uint64_t *rirb; /* responses: low = response, high = ex */
	uint64_t rirb_phys;
	uint32_t corb_entries, rirb_entries;
	uint32_t corb_wp, rirb_rp;
	int single_cmd; /* immediate command interface instead */
	/* the response a waiter is waiting for, per codec */
	volatile uint32_t pending_resp[HDA_MAX_CODECS];
	volatile int resp_ready[HDA_MAX_CODECS];
	uint32_t cmd_timeouts;
	/* unsolicited responses queued by the IRQ for the work thread */
	uint32_t unsol_queue[64];
	uint32_t unsol_head, unsol_tail;

	/* codecs */
	uint16_t codec_mask;
	struct hda_codec *codec[HDA_MAX_CODECS];
	int ncodecs;

	/* streams */
	struct hda_stream streams[HDA_MAX_STREAMS];
	int nstreams;
	uint32_t *posbuf; /* DMA position buffer (8 bytes per stream) */
	uint64_t posbuf_phys;

	/* interrupts */
	int irq_vector;
	int irq_gsi;
	int msi;
	int polling; /* no usable interrupt: a timer drives periods */

	/* Skylake+ extended capabilities (offsets from mmio, 0 if absent) */
	uint32_t ppcap, spbcap, mlcap, gtscap, drsmcap;

	/* the card */
	struct snd_card *card;
	struct snd_pcm *pcm_analog;
	struct snd_pcm *pcm_hdmi[HDA_MAX_HDMI_PINS];
	int npcm_hdmi;
};

/* Send one verb and wait for its response.  Returns 0 and the response in
 * *res, or -ETIMEDOUT/-EIO.  Any context except interrupt; busy-waits
 * (bounded) before the scheduler runs. */
int hda_cmd(struct hda_controller *ctrl, uint32_t cmd, uint32_t *res);
/* Unsolicited responses are dispatched to hda_codec_unsol() from the work
 * thread (never from the interrupt handler). */

/* Register access helpers (hda_controller.c). */
uint8_t hda_read8(struct hda_controller *c, uint32_t reg);
uint16_t hda_read16(struct hda_controller *c, uint32_t reg);
uint32_t hda_read32(struct hda_controller *c, uint32_t reg);
void hda_write8(struct hda_controller *c, uint32_t reg, uint8_t v);
void hda_write16(struct hda_controller *c, uint32_t reg, uint16_t v);
void hda_write32(struct hda_controller *c, uint32_t reg, uint32_t v);

/* ---- codecs (hda_codec.c / hda_parse.c / hda_generic.c) ---------------------- */

struct hda_widget {
	uint8_t nid;
	uint8_t type; /* AC_WID_* */
	uint32_t caps; /* AC_PAR_AUDIO_WIDGET_CAP */
	uint32_t pincap; /* pins */
	uint32_t pin_cfg; /* default configuration (after fixups) */
	uint32_t pin_cfg_orig; /* as the BIOS left it */
	uint32_t amp_in_caps, amp_out_caps;
	uint32_t pcm; /* AC_PAR_PCM (converters), 0 = inherit AFG */
	uint32_t stream_fmts; /* AC_PAR_STREAM */
	uint32_t power_caps;
	uint8_t nconn;
	uint8_t conn[HDA_MAX_CONNS];
	uint8_t cur_conn; /* selected index for selectors/pins */
	uint8_t used; /* claimed by a parsed path */
	uint8_t eapd; /* has EAPD */
	uint8_t pinctl; /* current pin widget control value */
};

/* A route from a converter (DAC/ADC) to a pin, in signal order for
 * output (dac ... pin) and input (pin ... adc). */
struct hda_path {
	uint8_t nid[HDA_MAX_PATH];
	uint8_t idx[HDA_MAX_PATH]; /* connection index used at each hop */
	uint8_t depth;
	uint8_t active;
	/* where volume and mute are applied on this path */
	uint8_t vol_nid, vol_dir, vol_idx;
	uint8_t mute_nid, mute_dir, mute_idx;
};

struct hda_out {
	uint32_t id; /* audio_output_desc.id */
	uint32_t type; /* AUDIO_OUT_* */
	uint8_t pin;
	uint8_t dac;
	uint8_t jack_detect; /* presence detect usable */
	uint8_t present;
	uint8_t hp_capable;
	uint8_t eapd;
	uint8_t dock;
	uint8_t seq;
	struct hda_path path;
	char name[32];
};

struct hda_in {
	uint32_t id;
	uint32_t type; /* AC_JACK_MIC_IN, AC_JACK_LINE_IN, ... */
	uint8_t pin;
	uint8_t adc;
	uint8_t jack_detect, present;
	uint8_t internal;
	struct hda_path path;
	char name[32];
};

/* What the generic parser decided (hda_generic.c). */
struct hda_spec {
	struct hda_out out[HDA_MAX_OUTPUTS];
	int nout;
	struct hda_in in[HDA_MAX_INPUTS];
	int nin;
	uint8_t primary_dac; /* the DAC the analog PCM plays through */
	uint8_t hp_dac; /* separate headphone DAC, 0 = shared */
	uint8_t adc;
	uint32_t outsel_mode; /* AUDIO_OUTSEL_* */
	uint32_t outsel_id;
	uint32_t master_left, master_right; /* 0..100 */
	uint32_t master_mute;
	uint32_t pcm_left, pcm_right;
	uint32_t capture_left, capture_right;
	int auto_mute; /* headphone in mutes speakers */
	int jack_poll; /* no unsolicited responses: poll */
	uint8_t beep_nid;
	uint8_t dig_out_nid; /* S/PDIF */
};

struct hda_codec {
	struct hda_controller *ctrl;
	int addr; /* 0..14 */
	uint32_t vendor_id; /* AC_PAR_VENDOR_ID of the root */
	uint32_t subsystem_id; /* AC_VERB_GET_SUBSYSTEM_ID of the AFG */
	uint32_t revision_id;
	uint8_t afg; /* audio function group node, 0 if none */
	uint8_t mfg; /* modem function group node */
	uint8_t start_nid, num_nodes;
	uint32_t afg_caps;
	uint32_t afg_pcm, afg_stream_fmts; /* defaults for converters */
	uint32_t afg_amp_in_caps, afg_amp_out_caps;
	uint32_t gpio_caps;
	uint32_t gpio_data, gpio_mask, gpio_dir;
	struct hda_widget *w; /* num_nodes entries, w[nid - start_nid] */
	char vendor_name[24]; /* "Realtek" */
	char chip_name[32]; /* "ALC298" */
	int is_hdmi; /* a display codec (hda_hdmi.c owns it) */
	int unsol_capable;
	const struct hda_vendor_driver *vendor;
	void *vendor_data;
	struct hda_spec spec;
	spinlock_t lock; /* serialises verbs sequences of this codec */
	uint32_t coef_cache[64];
};

/* Verb helpers (hda_codec.c).  Return 0xffffffff for "no response". */
uint32_t hda_codec_read(struct hda_codec *c, uint8_t nid, uint32_t verb,
			uint32_t payload);
int hda_codec_write(struct hda_codec *c, uint8_t nid, uint32_t verb,
		    uint32_t payload);
uint32_t hda_param(struct hda_codec *c, uint8_t nid, uint32_t param);
struct hda_widget *hda_widget(struct hda_codec *c, uint8_t nid);
uint32_t hda_widget_type(struct hda_codec *c, uint8_t nid);
/* Amp caps of a widget for a direction (inheriting the AFG's when the
 * widget has no override). */
uint32_t hda_amp_caps(struct hda_codec *c, uint8_t nid, int dir);
/* Set/get an amp: `gain' in steps, `mute' 0/1, both channels. */
void hda_amp_set(struct hda_codec *c, uint8_t nid, int dir, int idx,
		 uint32_t gain_l, uint32_t gain_r, int mute);
uint32_t hda_amp_get(struct hda_codec *c, uint8_t nid, int dir, int idx,
		     int left);
/* Processing coefficients (vendor widgets, Realtek COEF). */
uint32_t hda_coef_read(struct hda_codec *c, uint8_t nid, uint16_t idx);
void hda_coef_write(struct hda_codec *c, uint8_t nid, uint16_t idx,
		    uint16_t val);
void hda_coef_update(struct hda_codec *c, uint8_t nid, uint16_t idx,
		     uint16_t mask, uint16_t val);
/* Pins */
void hda_pin_set_ctl(struct hda_codec *c, uint8_t nid, uint8_t ctl);
int hda_pin_sense(struct hda_codec *c, uint8_t nid); /* 1 present */
void hda_pin_override_cfg(struct hda_codec *c, uint8_t nid, uint32_t cfg);
void hda_set_eapd(struct hda_codec *c, uint8_t nid, int on);
void hda_set_power(struct hda_codec *c, uint8_t nid, int state);
void hda_gpio_set(struct hda_codec *c, uint32_t mask, uint32_t dir,
		  uint32_t data);

/* Codec lifecycle (hda_codec.c).  Called by the controller for each bit in
 * STATESTS.  Creates, enumerates, applies the vendor driver, parses. */
int hda_codec_probe(struct hda_controller *ctrl, int addr,
		    struct hda_codec **out);
void hda_codec_free(struct hda_codec *c);
/* An unsolicited response arrived for this codec (work thread). */
void hda_codec_unsol(struct hda_codec *c, uint32_t res);
/* Jack polling for codecs without unsolicited responses (work thread). */
void hda_codec_poll_jacks(struct hda_codec *c);
/* Power the codec back up after a controller reset / resume. */
int hda_codec_resume(struct hda_codec *c);

/* Playback/capture setup on the analog function (hda_generic.c):
 * bind the stream tag and format to the path's converter(s). */
int hda_codec_setup_stream(struct hda_codec *c, int direction, int tag,
			   int channels, uint16_t format);
void hda_codec_cleanup_stream(struct hda_codec *c, int direction);
/* Capabilities of the analog function for the PCM core. */
void hda_codec_pcm_hw(struct hda_codec *c, int direction,
		      struct snd_pcm_hw *hw);

/* Mixer (hda_generic.c), backing struct snd_ctl_ops. */
int hda_codec_get_volume(struct hda_codec *c, uint32_t ctl,
			 struct audio_volume *v);
int hda_codec_set_volume(struct hda_codec *c, uint32_t ctl, uint32_t l,
			 uint32_t r);
int hda_codec_get_mute(struct hda_codec *c, uint32_t ctl, uint32_t *mute);
int hda_codec_set_mute(struct hda_codec *c, uint32_t ctl, uint32_t mute);
int hda_codec_get_outputs(struct hda_codec *c, struct audio_outputs *o);
int hda_codec_set_output(struct hda_codec *c,
			 const struct audio_setoutput *s);
int hda_codec_get_inputs(struct hda_codec *c, struct audio_outputs *o);
int hda_codec_set_input(struct hda_codec *c, const struct audio_setoutput *s);
/* Re-evaluate which outputs play (jack change, mode change). */
void hda_codec_update_routing(struct hda_codec *c);
/* Text dump of the codec for AUDIO_DIAG (hda_parse.c). */
long hda_codec_dump(struct hda_codec *c, char *buf, size_t len);

/* The generic parser (hda_generic.c): fills c->spec from the pin configs,
 * then programs the paths.  Vendor drivers may call it from their probe
 * after fixing pin configs. */
int hda_generic_parse(struct hda_codec *c);
int hda_generic_init(struct hda_codec *c);

/* ---- vendor drivers (hda_realtek.c ... hda_vm.c, hda_hdmi.c) --------------- */

struct hda_vendor_driver {
	const char *name; /* "realtek" */
	/* 1 if this driver handles the codec (vendor_id, subsystem). */
	int (*match)(struct hda_codec *c);
	/* Before parsing: fix pin configs, GPIOs, COEF init, chip name.
	 * May set c->is_hdmi and take the codec over entirely. */
	int (*pre_parse)(struct hda_codec *c);
	/* After the generic parser chose paths: extra amps, EAPD,
	 * speaker amps, model-specific tweaks. */
	int (*post_parse)(struct hda_codec *c);
	/* After every (re)initialisation, e.g. COEF writes lost on reset. */
	int (*init)(struct hda_codec *c);
	/* Called when the output set changes (auto-mute, headphone amp). */
	void (*automute)(struct hda_codec *c, int hp_present);
	/* Unsolicited response the generic code does not understand. */
	void (*unsol)(struct hda_codec *c, uint32_t res);
	/* Stream about to start/stop (amp power, DSP). */
	void (*playback_hook)(struct hda_codec *c, int start);
	/* For codecs a driver handles entirely itself (HDMI): */
	int (*probe)(struct hda_codec *c); /* replaces generic parsing */
	void (*remove)(struct hda_codec *c);
};

extern const struct hda_vendor_driver hda_realtek_driver;
extern const struct hda_vendor_driver hda_conexant_driver;
extern const struct hda_vendor_driver hda_idt_driver;
extern const struct hda_vendor_driver hda_analog_driver;
extern const struct hda_vendor_driver hda_cirrus_driver;
extern const struct hda_vendor_driver hda_via_driver;
extern const struct hda_vendor_driver hda_ca0132_driver;
extern const struct hda_vendor_driver hda_vm_driver;
extern const struct hda_vendor_driver hda_hdmi_driver;

/* Fixup table helpers for vendor drivers. */
struct hda_pincfg {
	uint8_t nid;
	uint32_t cfg;
};
struct hda_verb {
	uint8_t nid;
	uint16_t verb;
	uint16_t payload;
};
struct hda_coef_fix {
	uint8_t nid;
	uint16_t idx;
	uint16_t mask;
	uint16_t val;
};
/* A subsystem-matched fixup: apply pins, verbs and coefs in that order. */
struct hda_fixup {
	uint32_t codec_id; /* 0 = any codec of the vendor */
	uint32_t subsys; /* PCI/codec subsystem id, 0 = any */
	uint32_t subsys_mask; /* 0xffff0000 = vendor only */
	const char *name; /* "Lenovo ThinkPad P50" */
	const struct hda_pincfg *pins;
	const struct hda_verb *verbs;
	const struct hda_coef_fix *coefs;
	uint32_t flags; /* HDA_FIX_* */
};
#define HDA_FIX_NO_AUTOMUTE (1u << 0)
#define HDA_FIX_HP_GPIO (1u << 1)
#define HDA_FIX_SPK_AMP_GPIO (1u << 2)
#define HDA_FIX_MUTE_LED_GPIO (1u << 3)
#define HDA_FIX_JACK_POLL (1u << 4)

void hda_apply_pincfgs(struct hda_codec *c, const struct hda_pincfg *p);
void hda_apply_verbs(struct hda_codec *c, const struct hda_verb *v);
void hda_apply_coefs(struct hda_codec *c, const struct hda_coef_fix *f);
/* Find and apply the first matching fixup in a 0-terminated table.
 * Returns it or NULL. */
const struct hda_fixup *hda_apply_fixups(struct hda_codec *c,
					 const struct hda_fixup *table);

/* ---- HDMI/DP (hda_hdmi.c) ---------------------------------------------------- */

/* Card-level glue the controller calls: create the HDMI PCM devices of a
 * display codec on the controller's card. */
int hda_hdmi_add_pcms(struct hda_codec *c);
/* From the display driver's notification. */
void hda_hdmi_notify(int port, int present);

#endif /* KERNEL_DEV_SOUND_HDA_H */
