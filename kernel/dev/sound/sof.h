// LikeOS Intel audio DSP (Sound Open Firmware, IPC4) and SoundWire -
// internal interfaces between the DSP side and the SoundWire side.
//
//   sof_pci.c, sof_dsp.c, sof_loader.c, sof_ipc4.c, sof_topology.c,
//   sof_pcm.c                      the DSP: PCI, power, firmware, IPC4,
//                                  topology pipelines, host DMA, PCM devices
//   sof_acpi.c, sdw_intel.c, sdw_bus.c, sdw_cs42l43.c, sdca.c
//                                  machine discovery (NHLT, ACPI _ADR),
//                                  SoundWire masters, bus, codecs
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SOF_H
#define KERNEL_DEV_SOUND_SOF_H

#include <kernel/dev/sound/snd.h>

/* Platform generations */
#define SOF_PLAT_UNKNOWN 0
#define SOF_PLAT_SKL 1 /* cAVS 1.5: Skylake, Kaby Lake */
#define SOF_PLAT_APL 2 /* cAVS 1.5+: Apollo/Gemini Lake */
#define SOF_PLAT_CNL 3 /* cAVS 1.8: Cannon/Whiskey/Comet Lake */
#define SOF_PLAT_ICL 4 /* cAVS 2.0: Ice Lake, Jasper/Elkhart */
#define SOF_PLAT_TGL 5 /* cAVS 2.5: Tiger/Alder/Raptor Lake */
#define SOF_PLAT_MTL 6 /* ACE 1.x: Meteor Lake, Arrow Lake */
#define SOF_PLAT_LNL 7 /* ACE 2.x: Lunar Lake */
#define SOF_PLAT_PTL 8 /* ACE 3.x: Panther Lake */

#define SOF_MAX_SDW_LINKS 4
#define SOF_MAX_SDW_PERIPHERALS 16
#define SOF_MAX_PCMS 8

/* Output ids the SoundWire codecs publish (audio_output_desc.id); the
 * PCM device dedicated to the jack carries the headphone one as its
 * snd_pcm.output_id. */
#define SOF_OUT_ID_SPEAKER 1
#define SOF_OUT_ID_HEADPHONE 2
#define SOF_OUT_ID_HDMI_BASE 0x100

/* Audio function classes of a SoundWire stream (sof_dev.sdw_prep_function) */
#define SOF_SDW_FN_JACK 1 /* headphone / headset jack */
#define SOF_SDW_FN_AMP 2 /* speaker amplifier */
#define SOF_SDW_FN_MIC 4 /* microphones */

/* A SoundWire peripheral the machine description names (ACPI _ADR). */
struct sof_sdw_peripheral {
	int link; /* SoundWire link (master) index */
	uint8_t unique_id;
	uint16_t mfg_id; /* MIPI manufacturer id, e.g. 0x01fa Cirrus */
	uint16_t part_id; /* e.g. 0x4243 for CS42L43 */
	uint8_t class_id;
	uint8_t version;
	uint32_t functions; /* SDCA function types present (bit mask) */
	void *codec_priv; /* the codec driver's state */
	int dev_num; /* assigned device number 1..11, 0 = unattached */
	int attached;
};

/* What sof_acpi.c learned about the machine. */
struct sof_machine {
	uint32_t sdw_link_mask; /* links with peripherals */
	struct sof_sdw_peripheral periph[SOF_MAX_SDW_PERIPHERALS];
	int nperiph;
	uint32_t dmic_count; /* from NHLT */
	uint32_t ssp_mask; /* BT/I2S links from NHLT */
	uint16_t hda_codec_mask; /* HDA (HDMI) codecs found on the link */
	char tplg_name[96]; /* topology file chosen for this machine */
	char fw_name[96]; /* firmware image path under /lib/firmware */
	char fw_lib_path[96];
};

struct sof_dev {
	const pci_device_t *pci;
	int platform; /* SOF_PLAT_* */
	volatile uint8_t *hda; /* BAR0: HDA link + host DMA registers */
	volatile uint8_t *dsp; /* BAR4: DSP registers (cAVS/ACE) */
	uint64_t hda_phys, dsp_phys;
	uint64_t hda_size, dsp_size;
	struct sof_machine machine;
	struct snd_card *card;
	int irq_vector;
	int fw_booted;
	uint32_t fw_version[4];
	/* IPC4 */
	spinlock_t ipc_lock;
	volatile int ipc_reply_ready;
	uint32_t ipc_reply_primary, ipc_reply_ext;
	uint8_t *ipc_mailbox; /* inbox/outbox windows inside BAR4 */
	/* The audio function of the stream sdw_stream_prepare() is being
	 * called for (SOF_SDW_FN_*, 0 = not known), so a codec that carries
	 * several functions on one link and direction (the CS42L43 jack and
	 * speaker) picks the right data port.  Set by the caller around the
	 * call. */
	int sdw_prep_function;
	void *tplg; /* sof_topology.c state */
	void *sdw; /* sdw_intel.c state */
	void *pcm_priv; /* sof_pcm.c state */
};

/* ---- DSP side (sof_*.c) ------------------------------------------------------ */

/* IPC4: send a message and wait for the reply.  `data' goes to the outbox
 * mailbox, the reply payload (if any) is copied into `reply'.  Returns 0 or
 * a negative errno (the firmware status mapped to an errno). */
int sof_ipc4_send(struct sof_dev *sdev, uint32_t primary, uint32_t extension,
		  const void *data, size_t len, void *reply, size_t reply_max);

/* SoundWire DAI configuration the DSP needs for an ALH/SoundWire copier:
 * the side that owns SoundWire tells the DSP side through these. */
int sof_dai_sdw_config(struct sof_dev *sdev, int link, int alh_stream,
		       uint32_t rate, uint32_t channels, uint32_t bits);

/* ---- SoundWire side (sof_acpi.c, sdw_*.c, sdca.c) ------------------------------ */

/* Discover the machine: HDA codec mask, NHLT, ACPI SoundWire peripherals.
 * Fills sdev->machine (incl. topology/firmware names).  0 or -errno. */
int sof_acpi_scan(struct sof_dev *sdev);

/* Bring up the SoundWire masters named in sdev->machine.sdw_link_mask,
 * enumerate and initialise the codecs.  Called after the DSP firmware
 * booted (ACE: the links are powered through the DSP's SHIM). */
int sdw_intel_init(struct sof_dev *sdev);
void sdw_intel_exit(struct sof_dev *sdev);
/* From the DSP interrupt handler when a SoundWire link raised one. */
void sdw_intel_irq(struct sof_dev *sdev);

/* Stream control for a playback/capture DAI on a link: program the
 * frame shape and peripheral data ports, prepare and enable/disable
 * (bank switch).  `peripheral_mask' selects the codecs taking part. */
int sdw_stream_prepare(struct sof_dev *sdev, int link, int direction,
		       uint32_t rate, uint32_t channels, uint32_t bits,
		       uint32_t peripheral_mask);
int sdw_stream_enable(struct sof_dev *sdev, int link, int direction, int on);
int sdw_stream_release(struct sof_dev *sdev, int link, int direction);
/* The PCM data interface (PDI) behind an audio-link-hub stream id, or
 * -EINVAL for an id that is not one of the link's PCM streams. */
int sdw_intel_alh_to_pdi(int link, int alh_id);

/* Codec controls (volume/mute/jack/outputs) of the SoundWire codecs,
 * used by the card's snd_ctl_ops. */
int sdw_codec_get_volume(struct sof_dev *sdev, uint32_t ctl,
			 struct audio_volume *v);
int sdw_codec_set_volume(struct sof_dev *sdev, uint32_t ctl, uint32_t l,
			 uint32_t r);
int sdw_codec_get_mute(struct sof_dev *sdev, uint32_t ctl, uint32_t *mute);
int sdw_codec_set_mute(struct sof_dev *sdev, uint32_t ctl, uint32_t mute);
int sdw_codec_get_outputs(struct sof_dev *sdev, struct audio_outputs *o);
int sdw_codec_set_output(struct sof_dev *sdev,
			 const struct audio_setoutput *s);
long sdw_dump(struct sof_dev *sdev, char *buf, size_t len);

#endif /* KERNEL_DEV_SOUND_SOF_H */
