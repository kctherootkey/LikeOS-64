// LikeOS sound core - the interface between sound card drivers and the
// device layer.
//
// A driver (HDA, AC97, ES1371, SOF) finds its hardware, creates a card with
// snd_card_new(), adds one or more PCM devices with snd_pcm_new() and a
// control interface (volume, mute, outputs), and publishes the card with
// snd_card_register().  The core then owns everything user-facing: the
// /dev/audioN, /dev/dspN and /dev/mixerN nodes, the DMA ring buffer, the
// read/write/poll/mmap paths, the OSS and native ioctls, state tracking,
// underrun handling and the event queue.  A driver only programs hardware.
//
// Locking: every substream has a spinlock (`lock') taken with interrupts
// off.  ops->trigger and ops->pointer are called with it held and must not
// sleep.  ops->open/close/hw_params/hw_free/prepare are called without it,
// from process context, and may sleep after the scheduler runs.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SND_H
#define KERNEL_DEV_SOUND_SND_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/sound.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h>
#include <kernel/fs/device.h>
#include <kernel/io/console.h>

/* Group owning the sound device nodes (must match res/etc/group). */
#ifndef DEVFS_GID_AUDIO
#define DEVFS_GID_AUDIO 29
#endif

#define SND_MAX_CARDS AUDIO_MAX_CARDS
#define SND_MAX_PCMS 8 /* PCM devices per card (analog + HDMI/DP ports) */
#define SND_EVENT_QUEUE 32 /* queued audio_event per card */

/* Trigger commands */
#define SND_TRIGGER_START 1
#define SND_TRIGGER_STOP 2
#define SND_TRIGGER_PAUSE 3
#define SND_TRIGGER_RESUME 4

struct snd_card;
struct snd_pcm;
struct snd_pcm_substream;

/* What a direction of a PCM device can do.  Filled by the driver. */
struct snd_pcm_hw {
	uint32_t formats; /* AUDIO_FMT_BIT() mask; 0 = direction absent */
	uint32_t rates; /* AUDIO_RATE_* mask */
	uint32_t channels_min, channels_max;
	uint32_t period_bytes_min, period_bytes_max;
	uint32_t periods_min, periods_max;
	uint32_t buffer_bytes_max;
	uint32_t buffer_align; /* bytes; the ring start and size multiple */
	uint32_t period_align; /* bytes; HDA BDL entries want 128 */
	int dma32; /* the ring must lie below 4 GiB */
	int dma_coherent; /* device snoops: map the ring write-back */
};

struct snd_pcm_ops {
	/* A process opened the substream.  May refuse with -EBUSY. */
	int (*open)(struct snd_pcm_substream *ss);
	void (*close)(struct snd_pcm_substream *ss);
	/* ss->params, ->buffer_bytes, ->period_bytes, ->periods and the ring
	 * (ss->dma_phys / ss->dma_area) are set.  Program the stream format
	 * and the buffer descriptors here. */
	int (*hw_params)(struct snd_pcm_substream *ss);
	/* The ring is about to be freed. */
	int (*hw_free)(struct snd_pcm_substream *ss);
	/* Reset the stream to the start of the ring, ready to trigger. */
	int (*prepare)(struct snd_pcm_substream *ss);
	/* SND_TRIGGER_*.  Lock held, interrupts off, no sleeping. */
	int (*trigger)(struct snd_pcm_substream *ss, int cmd);
	/* The hardware's position in the ring, in bytes [0, buffer_bytes).
	 * Lock held, interrupts off. */
	uint32_t (*pointer)(struct snd_pcm_substream *ss);
	/* Optional: the extra delay (frames) the hardware adds after the
	 * DMA position (FIFOs, DSP pipeline). */
	uint32_t (*delay)(struct snd_pcm_substream *ss);
};

/* Core-owned runtime of one direction of one PCM device. */
struct snd_pcm_substream {
	struct snd_pcm *pcm;
	int direction; /* AUDIO_DIR_* */
	void *private_data; /* the driver's stream */

	/* negotiated parameters */
	struct audio_params params;
	uint32_t frame_bytes;
	uint32_t buffer_bytes;
	uint32_t period_bytes;
	uint32_t periods;

	/* the ring (core-allocated, zeroed before first use) */
	uint64_t dma_phys;
	uint8_t *dma_area; /* kernel mapping of the ring */
	uint32_t dma_pages;

	/* runtime state, protected by `lock' */
	spinlock_t lock;
	uint32_t state; /* AUDIO_STATE_* */
	int opened;
	uint32_t owner_uid; /* who has it open */
	uint64_t hw_frames; /* consumed (playback) / produced (capture) */
	uint64_t appl_frames; /* written (playback) / read (capture) */
	uint32_t last_hw_offset; /* bytes, for wrap detection */
	uint64_t start_ns;
	uint64_t hw_timestamp_ns;
	uint32_t xruns;
	uint32_t flags; /* AUDIO_PARAM_* */
	uint32_t avail_min_frames; /* wake writers when this much is free */
	uint32_t oss_frag_shift, oss_frag_max; /* SNDCTL_DSP_SETFRAGMENT */
	uint32_t oss_trigger; /* PCM_ENABLE_* */
	uint32_t oss_blocks; /* fragments since the last GETOPTR */
	struct wait_queue_head wq; /* readers/writers and poll */
	int mmap_count;
};

struct snd_pcm {
	struct snd_card *card;
	int device; /* index within the card */
	char name[32];
	const struct snd_pcm_ops *ops;
	struct snd_pcm_hw hw[2]; /* [AUDIO_DIR_*] */
	struct snd_pcm_substream sub[2];
	void *private_data;
	/* What this PCM device plays to, for AUDIO_GETPCMINFO: the output id
	 * (audio_output_desc.id) and type (AUDIO_OUT_*) of a dedicated output
	 * such as one HDMI/DP port, or 0/0 for the card's general analog
	 * device.  Set by the driver after snd_pcm_new(). */
	uint32_t output_id;
	uint32_t output_type;
};

/* Mixer/outputs, implemented by the driver.  Called from process context
 * without any core lock held; the driver serialises against itself. */
struct snd_ctl_ops {
	/* `ctl' is AUDIO_CTL_*; -ENOENT if the card has no such control.
	 * Values are 0..100. */
	int (*get_volume)(struct snd_card *card, uint32_t ctl,
			  struct audio_volume *v);
	int (*set_volume)(struct snd_card *card, uint32_t ctl, uint32_t left,
			  uint32_t right);
	int (*get_mute)(struct snd_card *card, uint32_t ctl, uint32_t *mute);
	int (*set_mute)(struct snd_card *card, uint32_t ctl, uint32_t mute);
	int (*get_outputs)(struct snd_card *card, struct audio_outputs *o);
	int (*set_output)(struct snd_card *card,
			  const struct audio_setoutput *s);
	int (*get_inputs)(struct snd_card *card, struct audio_outputs *o);
	int (*set_input)(struct snd_card *card,
			 const struct audio_setoutput *s);
	/* Write a text dump into `buf' (kernel memory), return its length. */
	long (*diag)(struct snd_card *card, uint32_t what, char *buf,
		     size_t len);
};

struct snd_card {
	int index; /* card number, set by snd_card_register */
	char name[64]; /* audio_info.card_name */
	char driver[32];
	char codec[64];
	char bus[32];
	const pci_device_t *pci;
	struct snd_pcm *pcm[SND_MAX_PCMS];
	int npcm;
	const struct snd_ctl_ops *ctl;
	void *private_data; /* the driver's card */
	uint32_t info_flags; /* AUDIO_INFO_* the driver adds */
	uint32_t num_outputs, num_inputs;

	/* core-owned */
	int registered;
	int disconnected;
	spinlock_t ev_lock;
	struct audio_event ev[SND_EVENT_QUEUE];
	uint32_t ev_head, ev_tail;
	struct wait_queue_head ev_wq;
	uint32_t modify_counter; /* OSS mixer_info */
	struct devfs_node node_audio, node_dsp, node_mixer;
	char path_audio[24], path_dsp[24], path_mixer[24];
};

/* ---- driver API ----------------------------------------------------------- */

/* Allocate a card.  `pci' may be NULL for non-PCI hardware. */
int snd_card_new(const char *driver, const pci_device_t *pci,
		 struct snd_card **out);
void snd_card_free(struct snd_card *card);

/* Add a PCM device.  `play'/`capture' describe each direction (NULL or a
 * zero `formats' for "not present"). */
int snd_pcm_new(struct snd_card *card, const char *name,
		const struct snd_pcm_ops *ops, const struct snd_pcm_hw *play,
		const struct snd_pcm_hw *capture, void *private_data,
		struct snd_pcm **out);

/* Publish the card: creates the device nodes and sysfs entries. */
int snd_card_register(struct snd_card *card);
/* The hardware is gone (or the driver is unloading): wake everyone with
 * AUDIO_STATE_DISCONNECTED and remove the nodes. */
void snd_card_disconnect(struct snd_card *card);

/* From the driver's interrupt handler (or a timer): a period completed.
 * Updates the position, detects underruns, wakes readers/writers. */
void snd_pcm_period_elapsed(struct snd_pcm_substream *ss);

/* A driver whose hardware raises no per-period interrupt asks the core to
 * drive the stream from a high-resolution timer instead: while the
 * substream runs, the core reads ops->pointer once per period and does
 * everything snd_pcm_period_elapsed() would.  May be called at any time,
 * from any context; takes effect at the next start.  The core never
 * decides this by itself: late interrupts are not missing ones. */
void snd_pcm_set_timer_driven(struct snd_pcm_substream *ss, int on);

/* Queue an event for /dev/mixer readers (AUDIO_EV_*).  Any context. */
void snd_card_event(struct snd_card *card, uint32_t type, uint32_t id,
		    uint32_t value);
/* Shorthand: a jack changed. */
void snd_jack_report(struct snd_card *card, uint32_t output_id, int present);

/* Deferred work in a sleepable kernel thread (ksnd), for the things an
 * interrupt handler must not do (codec verbs after an unsolicited
 * response, firmware loading, jack polling).  Safe from any context after
 * snd_init(); runs in submission order.  `delay_ms' 0 = as soon as
 * possible. */
typedef void (*snd_work_fn)(void *arg);
int snd_schedule_work(snd_work_fn fn, void *arg, uint32_t delay_ms);

/* Busy-wait helper usable before the scheduler runs. */
void snd_udelay(uint32_t us);
void snd_mdelay(uint32_t ms);

/* Physically contiguous, zeroed, uncached (unless `coherent') DMA memory
 * for driver-private structures (CORB/RIRB, BDL, position buffers, DSP
 * firmware images).  `dma32' keeps it below 4 GiB. */
void *snd_dma_alloc(size_t bytes, int dma32, int coherent, uint64_t *phys);
void snd_dma_free(void *virt, size_t bytes);

/* Logging: "[snd] <driver>: ..." */
#define snd_info(drv, fmt, ...) kprintf("[snd] %s: " fmt, drv, ##__VA_ARGS__)

/* ---- entry points ---------------------------------------------------------- */

/* Called once from kernel init after PCI and the display drivers: probes
 * every sound driver.  Busy-waits only. */
void snd_init(void);
/* Called once the root filesystem is mounted and the scheduler runs:
 * starts ksnd, loads firmware (SOF), enables jack polling. */
void snd_late_init(void);

/* Driver probe functions, called by snd_init in this order.  Each returns
 * the number of cards it registered. */
int hda_probe_all(void);
int ac97_probe_all(void);
int es1371_probe_all(void);
int sof_probe_all(void);
/* Late parts of drivers (after the root FS is mounted). */
void hda_late_init(void);
void sof_late_init(void);

/* Iterate registered cards (for /proc-style dumps and HDMI hookups). */
struct snd_card *snd_card_get(int index);
int snd_card_count(void);

/* ---- display audio (i915 <-> HDA HDMI codec) ------------------------------- */

/* The display driver tells the sound side about monitors with audio.
 * `port' is the display port index, `eld'/`eld_len' the monitor's ELD (0
 * length = unplugged). */
struct snd_display_audio_ops {
	/* the HDA side asks the display side to keep its power well up */
	void (*get_power)(void *display);
	void (*put_power)(void *display);
	/* minimum CDCLK etc. for a given audio sample rate */
	int (*sync_audio_rate)(void *display, int port, int rate);
	/* fetch the ELD of a port; returns its length or 0 */
	int (*get_eld)(void *display, int port, uint8_t *buf, int max);
};

void snd_display_audio_register(const struct snd_display_audio_ops *ops,
				void *display);
/* From the display driver: the audio-capable monitor on `port' changed. */
void snd_display_audio_notify(int port, int present);
/* For the HDA HDMI code: the registered display hooks (NULL if none). */
const struct snd_display_audio_ops *snd_display_audio_ops(void **display);

#endif /* KERNEL_DEV_SOUND_SND_H */
